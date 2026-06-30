#include "gossip.h"

#include "chain_log.h"
#include "../shared/sha256.h"
#include "../shared/holographic_nn.h"
#include "../shared/station_util.h"
#include "../shared/manifest.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool gossip_hnn_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *value = getenv("SIGNAL_HNN_DEBUG");
        cached = (value && value[0] != '\0' &&
                  strcmp(value, "0") != 0) ? 1 : 0;
    }
    return cached != 0;
}

#define GOSSIP_HNN_DEBUG_LOG(...) \
    do { \
        if (gossip_hnn_debug_enabled()) fprintf(stderr, __VA_ARGS__); \
    } while (0)

contract_summary_t contract_summary_make(const contract_t *ct) {
    contract_summary_t s = {0};
    if (!ct) return s;
    s.active = ct->active;
    s.action = (uint8_t)ct->action;
    s.station_index = ct->station_index;
    s.commodity = (uint8_t)ct->commodity;
    s.required_grade = ct->required_grade;
    s.proof_flags = ct->proof_flags;
    s.required_prefix_class = ct->required_prefix_class;
    s.required_recipe_id = ct->required_recipe_id;
    memcpy(s.required_parent, ct->required_parent, sizeof(s.required_parent));
    memcpy(s.target_pub, ct->target_pub, sizeof(s.target_pub));
    s.forbidden_origin_mask = ct->forbidden_origin_mask;
    s.quantity_needed = ct->quantity_needed;
    s.base_price = ct->base_price;
    s.age_at_copy = ct->age;
    return s;
}

static bool contract_summary_matches(const contract_summary_t *a,
                                     const contract_summary_t *b) {
    return a->action == b->action &&
           a->station_index == b->station_index &&
           a->commodity == b->commodity &&
           a->required_grade == b->required_grade &&
           a->proof_flags == b->proof_flags &&
           a->required_prefix_class == b->required_prefix_class &&
           a->required_recipe_id == b->required_recipe_id &&
           memcmp(a->required_parent, b->required_parent,
                  sizeof(a->required_parent)) == 0 &&
           memcmp(a->target_pub, b->target_pub,
                  sizeof(a->target_pub)) == 0 &&
           a->forbidden_origin_mask == b->forbidden_origin_mask;
}

static uint16_t gossip_u16_hint_from_float(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 65535.0f) return 65535;
    return (uint16_t)(value + 0.5f);
}

enum {
    GOSSIP_STALE_CONTRACT_AGE_SECONDS = 300,
    GOSSIP_HNN_MARKET_MAX_EXPERIENCE = 16,
    GOSSIP_HNN_MARKET_RETAIN_NUM = 3,
    GOSSIP_HNN_MARKET_RETAIN_DEN = 4,
    GOSSIP_HNN_MARKET_DECAY_TICKS = 120 * 60,
    GOSSIP_SHIP_CONTACT_RANGE = 420,
    GOSSIP_ROUTE_HISTORY_PROMOTE_EVIDENCE = 4,
};

static const float GOSSIP_HNN_TRACE_CARGO_MIN_FIDELITY = 0.20f;
static const float GOSSIP_HNN_TRACE_CARGO_MAX_LOAD = 1.0f;

static float gossip_clampf(float value, float lo, float hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static bool gossip_signal_field_kind_for_market(
    const market_memory_t *memory,
    signal_field_kind_t *out) {
    if (!memory || !memory->active || !out) return false;
    switch ((market_memory_kind_t)memory->memory_kind) {
    case MARKET_MEMORY_DEMAND:
    case MARKET_MEMORY_ORE_PRESSURE:
    case MARKET_MEMORY_SCAFFOLD_PRESSURE:
        *out = SIGNAL_FIELD_KIND_DEMAND;
        return true;
    case MARKET_MEMORY_SUPPLY:
        *out = SIGNAL_FIELD_KIND_SUPPLY;
        return true;
    case MARKET_MEMORY_ROUTE_SUCCESS:
    case MARKET_MEMORY_ROUTE_REPUTATION:
        *out = SIGNAL_FIELD_KIND_ROUTE;
        return true;
    case MARKET_MEMORY_DELIVERY_RECEIPT:
    case MARKET_MEMORY_STATION_TRUST:
        *out = SIGNAL_FIELD_KIND_PROOF;
        return true;
    case MARKET_MEMORY_ROUTE_DANGER:
    case MARKET_MEMORY_ROUTE_RISK:
    case MARKET_MEMORY_STATION_RISK:
        *out = SIGNAL_FIELD_KIND_RISK;
        return true;
    case MARKET_MEMORY_NONE:
    default:
        return false;
    }
}

static float gossip_signal_field_strength(const market_memory_t *memory) {
    if (!memory || !memory->active) return 0.0f;
    float confidence = (float)memory->confidence / 255.0f;
    float salience = (float)memory->salience / 255.0f;
    float strength = confidence * 0.35f + salience * 0.65f;
    return gossip_clampf(strength, 0.02f, 1.0f);
}

static void gossip_signal_field_observe_market(world_t *w,
                                               vec2 pos,
                                               const market_memory_t *memory) {
    if (!w || !memory || !memory->active) return;
    signal_field_kind_t kind;
    if (!gossip_signal_field_kind_for_market(memory, &kind)) return;
    (void)signal_field_observe(&w->signal_field, pos, kind,
                               gossip_signal_field_strength(memory), w->tick);
}

static int gossip_signal_field_observe_view(world_t *w,
                                            const knowledge_view_t *view,
                                            vec2 pos) {
    if (!w || !view) return 0;
    uint8_t count = view->count;
    if (count > KNOWLEDGE_VIEW_MAX_CAP) count = KNOWLEDGE_VIEW_MAX_CAP;
    int observed = 0;
    for (int i = 0; i < count; i++) {
        market_memory_t memory;
        if (!market_memory_from_knowledge_item(&view->items[i], &memory))
            continue;
        gossip_signal_field_observe_market(w, pos, &memory);
        observed++;
    }
    return observed;
}

bool market_memory_from_contract_summary(const contract_summary_t *s,
                                         market_memory_t *out) {
    if (!s || !out || !s->active) return false;
    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_DEMAND;
    out->station_a = s->station_index;
    out->station_b = 0xff;
    out->commodity = s->commodity;
    out->action = s->action;
    out->confidence = 235;
    out->salience = 180;
    if (s->age_at_copy >= (float)GOSSIP_STALE_CONTRACT_AGE_SECONDS) {
        float stale = (s->age_at_copy -
                       (float)GOSSIP_STALE_CONTRACT_AGE_SECONDS) /
                      (float)GOSSIP_STALE_CONTRACT_AGE_SECONDS;
        if (stale > 1.0f) stale = 1.0f;
        out->confidence = (uint8_t)(205.0f - stale * 55.0f);
        out->salience = (uint8_t)(120.0f - stale * 45.0f);
    }
    out->quantity_hint = gossip_u16_hint_from_float(s->quantity_needed);
    out->value_hint = gossip_u16_hint_from_float(s->base_price);
    out->observed_tick = gossip_u16_hint_from_float(s->age_at_copy);
    return true;
}

bool market_memory_from_station_supply(const station_t *st,
                                       int station_index,
                                       commodity_t commodity,
                                       uint32_t observed_tick,
                                       market_memory_t *out) {
    if (!st || !out) return false;
    if (station_index < 0 || station_index >= MAX_STATIONS) return false;
    if (commodity < COMMODITY_RAW_ORE_COUNT || commodity >= COMMODITY_COUNT)
        return false;
    int stock = station_finished_count(st, commodity);
    if (stock <= 0) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_SUPPLY;
    out->station_a = (uint8_t)station_index;
    out->station_b = 0xffu;
    out->commodity = (uint8_t)commodity;
    out->action = (uint8_t)CONTRACT_TRACTOR;
    out->confidence = 215;
    uint16_t stock_hint = gossip_u16_hint_from_float((float)stock);
    out->quantity_hint = stock_hint;
    out->value_hint = gossip_u16_hint_from_float(station_sell_price(st, commodity));
    out->salience = stock >= 8 ? 190 : (uint8_t)(110 + stock * 10);
    out->observed_tick = observed_tick;
    out->subject_nonce = (uint64_t)(uint8_t)station_index
                       | ((uint64_t)(uint8_t)commodity << 8);
    return true;
}

bool market_memory_from_ore_pressure(const station_t *st,
                                     int station_index,
                                     commodity_t ore,
                                     uint32_t observed_tick,
                                     market_memory_t *out) {
    if (!st || !out) return false;
    if (station_index < 0 || station_index >= MAX_STATIONS) return false;
    if (ore >= COMMODITY_RAW_ORE_COUNT) return false;
    float need = station_raw_ore_need_score(st, ore);
    if (need <= 0.01f) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_ORE_PRESSURE;
    out->station_a = (uint8_t)station_index;
    out->station_b = 0xffu;
    out->commodity = (uint8_t)ore;
    out->action = 0xffu;
    out->confidence = 210;
    out->salience = (uint8_t)gossip_clampf(120.0f + need * 110.0f,
                                           120.0f, 230.0f);
    out->quantity_hint = gossip_u16_hint_from_float(need * 100.0f);
    out->value_hint = gossip_u16_hint_from_float(need * 255.0f);
    out->observed_tick = observed_tick;
    out->subject_nonce = (uint64_t)MARKET_MEMORY_ORE_PRESSURE
                       | ((uint64_t)(uint8_t)station_index << 8)
                       | ((uint64_t)(uint8_t)ore << 16);
    return true;
}

bool market_memory_from_scaffold_pressure(int destination_station,
                                          int source_station,
                                          module_type_t module_type,
                                          uint32_t observed_tick,
                                          uint64_t scaffold_nonce,
                                          market_memory_t *out) {
    if (!out) return false;
    if (destination_station < 0 || destination_station >= MAX_STATIONS)
        return false;
    if (source_station < -1 || source_station >= MAX_STATIONS) return false;
    if (module_type < 0 || module_type >= MODULE_COUNT) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_SCAFFOLD_PRESSURE;
    out->station_a = (uint8_t)destination_station;
    out->station_b = source_station >= 0 ? (uint8_t)source_station : 0xffu;
    out->commodity = (uint8_t)COMMODITY_COUNT;
    out->action = 0xffu;
    out->confidence = 215;
    out->salience = 205;
    out->quantity_hint = (uint16_t)module_type;
    out->value_hint = 1;
    out->observed_tick = observed_tick;
    out->subject_nonce = scaffold_nonce
                       ? scaffold_nonce
                       : ((uint64_t)MARKET_MEMORY_SCAFFOLD_PRESSURE
                          | ((uint64_t)(uint8_t)destination_station << 8)
                          | ((uint64_t)(uint8_t)(source_station < 0 ? 0xff : source_station) << 16)
                          | ((uint64_t)(uint8_t)module_type << 24));
    return true;
}

bool market_memory_from_delivery_receipt(int origin_station,
                                         int destination_station,
                                         commodity_t commodity,
                                         uint16_t units,
                                         float value_hint,
                                         uint32_t observed_tick,
                                         uint64_t receipt_nonce,
                                         market_memory_t *out) {
    if (!out) return false;
    if (origin_station < 0 || origin_station >= MAX_STATIONS) return false;
    if (destination_station < 0 || destination_station >= MAX_STATIONS)
        return false;
    if (commodity >= COMMODITY_COUNT) return false;
    if (units == 0) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT;
    out->station_a = (uint8_t)destination_station;
    out->station_b = (uint8_t)origin_station;
    out->commodity = (uint8_t)commodity;
    out->action = (uint8_t)CONTRACT_DELIVERY;
    out->confidence = 250;
    out->salience = units >= 8 ? 235 : (uint8_t)(170 + units * 8);
    out->quantity_hint = units;
    out->value_hint = gossip_u16_hint_from_float(value_hint);
    out->observed_tick = observed_tick;
    out->subject_nonce = receipt_nonce;
    return true;
}

bool market_memory_from_route_reputation(int origin_station,
                                         int destination_station,
                                         commodity_t commodity,
                                         uint16_t evidence_count,
                                         float value_hint,
                                         uint32_t observed_tick,
                                         bool risk,
                                         market_memory_t *out) {
    if (!out) return false;
    if (origin_station < 0 || origin_station >= MAX_STATIONS) return false;
    if (destination_station < 0 || destination_station >= MAX_STATIONS)
        return false;
    if (commodity >= COMMODITY_COUNT) return false;
    if (evidence_count == 0) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = risk
        ? (uint8_t)MARKET_MEMORY_ROUTE_RISK
        : (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION;
    out->station_a = (uint8_t)destination_station;
    out->station_b = (uint8_t)origin_station;
    out->commodity = (uint8_t)commodity;
    out->action = (uint8_t)CONTRACT_TRACTOR;
    out->confidence = risk ? 205 : 220;
    out->salience = evidence_count >= 8
        ? (risk ? 235 : 225)
        : (uint8_t)((risk ? 135 : 130) + evidence_count * 11);
    out->quantity_hint = evidence_count;
    out->value_hint = gossip_u16_hint_from_float(value_hint);
    out->observed_tick = observed_tick;
    out->subject_nonce = (uint64_t)(risk ? MARKET_MEMORY_ROUTE_RISK
                                         : MARKET_MEMORY_ROUTE_REPUTATION)
                       | ((uint64_t)(uint8_t)origin_station << 8)
                       | ((uint64_t)(uint8_t)destination_station << 16)
                       | ((uint64_t)(uint8_t)commodity << 24);
    return true;
}

bool market_memory_from_station_trust(int station_index,
                                      uint8_t action,
                                      commodity_t commodity,
                                      uint16_t evidence_count,
                                      float value_hint,
                                      uint32_t observed_tick,
                                      market_memory_t *out) {
    if (!out) return false;
    if (station_index < 0 || station_index >= MAX_STATIONS) return false;
    if (commodity >= COMMODITY_COUNT) return false;
    if (evidence_count == 0) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_STATION_TRUST;
    out->station_a = (uint8_t)station_index;
    out->station_b = 0xffu;
    out->commodity = (uint8_t)commodity;
    out->action = action;
    out->confidence = 215;
    out->salience = evidence_count >= 8
        ? 225
        : (uint8_t)(130 + evidence_count * 10);
    out->quantity_hint = evidence_count;
    out->value_hint = gossip_u16_hint_from_float(value_hint);
    out->observed_tick = observed_tick;
    out->subject_nonce = (uint64_t)MARKET_MEMORY_STATION_TRUST
                       | ((uint64_t)(uint8_t)station_index << 8)
                       | ((uint64_t)action << 16)
                       | ((uint64_t)(uint8_t)commodity << 24);
    return true;
}

bool market_memory_from_station_risk(int station_index,
                                     uint8_t action,
                                     commodity_t commodity,
                                     uint16_t evidence_count,
                                     float value_hint,
                                     uint32_t observed_tick,
                                     market_memory_t *out) {
    if (!out) return false;
    if (station_index < 0 || station_index >= MAX_STATIONS) return false;
    if (commodity >= COMMODITY_COUNT) return false;
    if (evidence_count == 0) return false;

    memset(out, 0, sizeof(*out));
    out->active = true;
    out->memory_kind = (uint8_t)MARKET_MEMORY_STATION_RISK;
    out->station_a = (uint8_t)station_index;
    out->station_b = 0xffu;
    out->commodity = (uint8_t)commodity;
    out->action = action;
    out->confidence = 205;
    out->salience = evidence_count >= 8
        ? 235
        : (uint8_t)(135 + evidence_count * 11);
    out->quantity_hint = evidence_count;
    out->value_hint = gossip_u16_hint_from_float(value_hint);
    out->observed_tick = observed_tick;
    out->subject_nonce = (uint64_t)MARKET_MEMORY_STATION_RISK
                       | ((uint64_t)(uint8_t)station_index << 8)
                       | ((uint64_t)action << 16)
                       | ((uint64_t)(uint8_t)commodity << 24);
    return true;
}

static bool market_memory_from_stale_contract_summary(
    const contract_summary_t *s,
    market_memory_t *out) {
    if (!s || !out || !s->active) return false;
    if (s->station_index >= MAX_STATIONS) return false;
    if (s->commodity >= (uint8_t)COMMODITY_COUNT) return false;
    if (s->age_at_copy < (float)GOSSIP_STALE_CONTRACT_AGE_SECONDS)
        return false;

    float over_age = s->age_at_copy - (float)GOSSIP_STALE_CONTRACT_AGE_SECONDS;
    uint16_t evidence = 1u + gossip_u16_hint_from_float(
        over_age / (float)GOSSIP_STALE_CONTRACT_AGE_SECONDS);
    if (evidence > 8u) evidence = 8u;
    return market_memory_from_station_risk((int)s->station_index,
                                           s->action,
                                           (commodity_t)s->commodity,
                                           evidence,
                                           s->base_price,
                                           gossip_u16_hint_from_float(s->age_at_copy),
                                           out);
}

static uint8_t gossip_market_bucket(uint8_t value) {
    return (uint8_t)(value / 32u);
}

static uint64_t gossip_hnn_field_seed(uint32_t field, uint32_t value) {
    return 0x6d61726b65740000ull |
           ((uint64_t)(field & 0xffu) << 32) |
           (uint64_t)(value & 0xffffffffu);
}

static void gossip_hnn_bundle_field(float key[HNN_DIM],
                                    uint32_t field,
                                    uint32_t value) {
    float field_key[HNN_DIM];
    float value_key[HNN_DIM];
    float pair[HNN_DIM];
    hnn_key_vector(gossip_hnn_field_seed(field, 0x9e3779b9u), field_key);
    hnn_key_vector(gossip_hnn_field_seed(field, value), value_key);
    hnn_bind(field_key, value_key, pair);
    hnn_bundle(key, pair);
}

static void gossip_hnn_encode_market_key(const market_memory_t *memory,
                                         float out[HNN_DIM]) {
    memset(out, 0, HNN_DIM * sizeof(float));
    if (!memory || !memory->active) return;
    gossip_hnn_bundle_field(out, 1u, memory->memory_kind);
    gossip_hnn_bundle_field(out, 2u, memory->station_a);
    gossip_hnn_bundle_field(out, 3u, memory->station_b);
    gossip_hnn_bundle_field(out, 4u, memory->commodity);
    gossip_hnn_bundle_field(out, 5u, memory->action);
    gossip_hnn_bundle_field(out, 6u, gossip_market_bucket(memory->confidence));
    gossip_hnn_bundle_field(out, 7u, gossip_market_bucket(memory->salience));
}

static bool gossip_hnn_job_value(gossip_hnn_job_t job, float out[HNN_DIM]) {
    if (!out) return false;
    if (job < GOSSIP_HNN_JOB_HAUL || job > GOSSIP_HNN_JOB_REPAIR)
        return false;
    hnn_key_vector(0x6a6f620000000000ull | (uint64_t)job, out);
    return true;
}

static bool gossip_hnn_job_for_market_memory(const market_memory_t *memory,
                                             gossip_hnn_job_t *out) {
    if (!memory || !memory->active || !out) return false;
    uint8_t action = memory->action;
    switch ((market_memory_kind_t)memory->memory_kind) {
    case MARKET_MEMORY_DEMAND:
        if (action == (uint8_t)CONTRACT_DELIVERY) {
            *out = GOSSIP_HNN_JOB_DELIVER_PROOF;
            return true;
        }
        if (action == (uint8_t)CONTRACT_FRACTURE) {
            *out = GOSSIP_HNN_JOB_SCOUT;
            return true;
        }
        *out = GOSSIP_HNN_JOB_HAUL;
        return true;
    case MARKET_MEMORY_SUPPLY:
        if (memory->commodity == (uint8_t)COMMODITY_REPAIR_KIT) {
            *out = GOSSIP_HNN_JOB_REPAIR;
            return true;
        }
        *out = GOSSIP_HNN_JOB_HAUL;
        return true;
    case MARKET_MEMORY_ROUTE_DANGER:
    case MARKET_MEMORY_ROUTE_SUCCESS:
    case MARKET_MEMORY_ROUTE_REPUTATION:
    case MARKET_MEMORY_ROUTE_RISK:
    case MARKET_MEMORY_STATION_TRUST:
    case MARKET_MEMORY_STATION_RISK:
        if (action == (uint8_t)CONTRACT_DELIVERY) {
            *out = GOSSIP_HNN_JOB_DELIVER_PROOF;
            return true;
        }
        *out = GOSSIP_HNN_JOB_HAUL;
        return true;
    case MARKET_MEMORY_DELIVERY_RECEIPT:
        *out = GOSSIP_HNN_JOB_DELIVER_PROOF;
        return true;
    case MARKET_MEMORY_ORE_PRESSURE:
        *out = GOSSIP_HNN_JOB_MINE;
        return true;
    case MARKET_MEMORY_SCAFFOLD_PRESSURE:
        *out = GOSSIP_HNN_JOB_TOW;
        return true;
    case MARKET_MEMORY_NONE:
    default:
        return false;
    }
}

static bool gossip_hnn_secondary_job_for_market_memory(const market_memory_t *memory,
                                                       gossip_hnn_job_t *out) {
    if (!memory || !memory->active || !out) return false;
    switch ((market_memory_kind_t)memory->memory_kind) {
    case MARKET_MEMORY_DELIVERY_RECEIPT:
        /* A delivery receipt proves a delivery route, but it is still useful
         * weak evidence for ordinary haul routing across the same endpoints. */
        *out = GOSSIP_HNN_JOB_HAUL;
        return true;
    case MARKET_MEMORY_NONE:
    default:
        return false;
    }
}

bool gossip_hnn_store_market_memory(hnn_memory_t *mem,
                                    const market_memory_t *memory) {
    if (!mem || !memory || !memory->active) return false;
    gossip_hnn_job_t job;
    if (!gossip_hnn_job_for_market_memory(memory, &job)) return false;
    float key[HNN_DIM];
    float value[HNN_DIM];
    gossip_hnn_encode_market_key(memory, key);
    if (!gossip_hnn_job_value(job, value)) return false;
    hnn_memory_store(mem, key, value);
    if (gossip_hnn_secondary_job_for_market_memory(memory, &job) &&
        gossip_hnn_job_value(job, value)) {
        hnn_memory_store(mem, key, value);
    }
    return true;
}

float gossip_hnn_market_resonance(const hnn_memory_t *mem,
                                  const market_memory_t *memory,
                                  gossip_hnn_job_t job) {
    if (!mem || !memory || !memory->active) return 0.0f;
    if (mem->experience_count <= 0) return 0.0f;
    float key[HNN_DIM];
    float expected[HNN_DIM];
    float retrieved[HNN_DIM];
    gossip_hnn_encode_market_key(memory, key);
    if (!gossip_hnn_job_value(job, expected)) return 0.0f;
    hnn_memory_cleanup(mem, key, retrieved, 2);
    float sim = hnn_similarity(retrieved, expected);
    return gossip_clampf(sim, 0.0f, 1.0f);
}

static void knowledge_contract_subject_hash(const contract_summary_t *s,
                                            uint8_t out[32]) {
    if (!s || !out) return;
    uint8_t key[81] = {
        (uint8_t)KNOW_CONTRACT,
        s->action,
        s->station_index,
        s->commodity,
        s->required_grade,
        s->proof_flags,
        s->required_prefix_class,
    };
    key[7] = (uint8_t)(s->required_recipe_id & 0xffu);
    key[8] = (uint8_t)((s->required_recipe_id >> 8) & 0xffu);
    memcpy(&key[9], s->required_parent, 32);
    for (int i = 0; i < 8; i++)
        key[41 + i] = (uint8_t)((s->forbidden_origin_mask >> (8 * i)) & 0xffu);
    memcpy(&key[49], s->target_pub, 32);
    sha256_bytes(key, sizeof(key), out);
}

static void knowledge_market_subject_hash(const market_memory_t *memory,
                                          uint8_t out[32]) {
    if (!memory || !out) return;
    uint8_t key[16] = {
        (uint8_t)KNOW_MARKET,
        memory->memory_kind,
        memory->station_a,
        memory->station_b,
        memory->commodity,
        memory->action,
    };
    for (int i = 0; i < 8; i++)
        key[6 + i] = (uint8_t)((memory->subject_nonce >> (8 * i)) & 0xffu);
    sha256_bytes(key, sizeof(key), out);
}

bool knowledge_item_from_contract_summary(const contract_summary_t *s,
                                          knowledge_item_t *out) {
    if (!s || !out || !s->active) return false;
    memset(out, 0, sizeof(*out));
    out->kind = (uint8_t)KNOW_CONTRACT;
    out->confidence = 255;
    out->salience = 160;
    out->payload_kind = (uint8_t)KNOW_PAYLOAD_CONTRACT_SUMMARY;
    knowledge_contract_subject_hash(s, out->subject_hash);
    memcpy(out->payload, s, sizeof(*s));
    return true;
}

bool knowledge_item_from_market_memory(const market_memory_t *memory,
                                       knowledge_item_t *out) {
    if (!memory || !out || !memory->active) return false;
    if (memory->memory_kind == (uint8_t)MARKET_MEMORY_NONE) return false;
    memset(out, 0, sizeof(*out));
    out->kind = (uint8_t)KNOW_MARKET;
    out->confidence = memory->confidence;
    out->salience = memory->salience;
    out->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    out->observed_tick = memory->observed_tick;
    out->learned_tick = memory->observed_tick;
    knowledge_market_subject_hash(memory, out->subject_hash);
    memcpy(out->payload, memory, sizeof(*memory));
    return true;
}

bool contract_summary_from_knowledge_item(const knowledge_item_t *item,
                                          contract_summary_t *out) {
    if (!item || !out) return false;
    if (item->kind != (uint8_t)KNOW_CONTRACT) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_CONTRACT_SUMMARY)
        return false;
    memcpy(out, item->payload, sizeof(*out));
    return out->active;
}

bool market_memory_from_knowledge_item(const knowledge_item_t *item,
                                       market_memory_t *out) {
    if (!item || !out) return false;
    if (item->kind != (uint8_t)KNOW_MARKET) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY)
        return false;
    memcpy(out, item->payload, sizeof(*out));
    return out->active;
}

void knowledge_view_configure(knowledge_view_t *view, uint8_t capacity) {
    if (!view) return;
    if (capacity > KNOWLEDGE_VIEW_MAX_CAP)
        capacity = KNOWLEDGE_VIEW_MAX_CAP;
    view->capacity = capacity;
    uint8_t old_count = view->count <= KNOWLEDGE_VIEW_MAX_CAP
        ? view->count
        : KNOWLEDGE_VIEW_MAX_CAP;
    if (old_count > view->capacity) {
        int drop = (int)old_count - (int)view->capacity;
        for (int i = 0; i < view->capacity; i++)
            view->items[i] = view->items[i + drop];
        for (int i = view->capacity; i < KNOWLEDGE_VIEW_MAX_CAP; i++)
            memset(&view->items[i], 0, sizeof(view->items[i]));
        view->count = view->capacity;
    } else if (view->count > old_count) {
        view->count = old_count;
    }
}

static uint8_t knowledge_view_capacity(const knowledge_view_t *view) {
    if (!view) return 0;
    return view->capacity <= KNOWLEDGE_VIEW_MAX_CAP
        ? view->capacity
        : KNOWLEDGE_VIEW_MAX_CAP;
}

static bool knowledge_items_match(const knowledge_item_t *a,
                                  const knowledge_item_t *b) {
    return a->kind == b->kind &&
           a->payload_kind == b->payload_kind &&
           memcmp(a->subject_hash, b->subject_hash, 32) == 0;
}

static bool knowledge_view_contains_item(const knowledge_view_t *view,
                                         const knowledge_item_t *item) {
    if (!view || !item || item->kind == (uint8_t)KNOW_NONE) return false;
    uint8_t cap = knowledge_view_capacity(view);
    for (int i = 0; i < view->count && i < cap; i++) {
        if (knowledge_items_match(&view->items[i], item))
            return true;
    }
    return false;
}

static bool knowledge_item_is_newer_or_equal(const knowledge_item_t *incoming,
                                             const knowledge_item_t *stored) {
    contract_summary_t in_contract;
    contract_summary_t stored_contract;
    if (contract_summary_from_knowledge_item(incoming, &in_contract) &&
        contract_summary_from_knowledge_item(stored, &stored_contract)) {
        return in_contract.age_at_copy >= stored_contract.age_at_copy;
    }
    market_memory_t in_market;
    market_memory_t stored_market;
    if (market_memory_from_knowledge_item(incoming, &in_market) &&
        market_memory_from_knowledge_item(stored, &stored_market)) {
        if (in_market.observed_tick != stored_market.observed_tick)
            return in_market.observed_tick >= stored_market.observed_tick;
        return incoming->learned_tick >= stored->learned_tick;
    }
    return incoming->learned_tick >= stored->learned_tick;
}

void knowledge_view_insert(knowledge_view_t *view, const knowledge_item_t *item) {
    if (!view || !item || item->kind == (uint8_t)KNOW_NONE) return;
    uint8_t cap = knowledge_view_capacity(view);
    if (cap == 0) return;

    if (view->count > cap) view->count = cap;
    for (int i = 0; i < view->count; i++) {
        if (knowledge_items_match(&view->items[i], item)) {
            if (knowledge_item_is_newer_or_equal(item, &view->items[i]))
                view->items[i] = *item;
            return;
        }
    }

    if (view->count < cap) {
        view->items[view->count++] = *item;
    } else {
        for (int i = 1; i < cap; i++)
            view->items[i - 1] = view->items[i];
        view->items[cap - 1] = *item;
    }
}

void knowledge_view_reinforce_route_reputation(knowledge_view_t *view,
                                               const market_memory_t *memory) {
    if (!view || !memory || !memory->active) return;
    if (memory->memory_kind != (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION &&
        memory->memory_kind != (uint8_t)MARKET_MEMORY_ROUTE_RISK) return;
    knowledge_view_configure(view, view->capacity ? view->capacity
                                                  : KNOWLEDGE_VIEW_MAX_CAP);
    market_memory_t merged = *memory;
    uint8_t cap = knowledge_view_capacity(view);
    for (int i = 0; i < view->count && i < cap; i++) {
        market_memory_t existing;
        if (!market_memory_from_knowledge_item(&view->items[i], &existing))
            continue;
        if (existing.memory_kind != memory->memory_kind) continue;
        if (existing.station_a != memory->station_a) continue;
        if (existing.station_b != memory->station_b) continue;
        if (existing.commodity != memory->commodity) continue;
        if (existing.action != memory->action) continue;
        uint32_t evidence = (uint32_t)existing.quantity_hint +
                            (uint32_t)memory->quantity_hint;
        if (evidence > 65535u) evidence = 65535u;
        merged.quantity_hint = (uint16_t)evidence;
        uint32_t value = (uint32_t)existing.value_hint +
                         (uint32_t)memory->value_hint;
        if (value > 65535u) value = 65535u;
        merged.value_hint = (uint16_t)value;
        int conf = (int)existing.confidence + 18;
        if (memory->confidence > conf) conf = memory->confidence;
        merged.confidence = (uint8_t)(conf > 255 ? 255 : conf);
        int sal = (int)existing.salience + (int)(memory->salience / 3);
        if (memory->salience > sal) sal = memory->salience;
        merged.salience = (uint8_t)(sal > 255 ? 255 : sal);
        if (existing.observed_tick > merged.observed_tick)
            merged.observed_tick = existing.observed_tick;
        break;
    }

    knowledge_item_t item;
    if (knowledge_item_from_market_memory(&merged, &item))
        knowledge_view_insert(view, &item);
}

void knowledge_view_reinforce_station_trust(knowledge_view_t *view,
                                            const market_memory_t *memory) {
    if (!view || !memory || !memory->active) return;
    if (memory->memory_kind != (uint8_t)MARKET_MEMORY_STATION_TRUST &&
        memory->memory_kind != (uint8_t)MARKET_MEMORY_STATION_RISK) return;
    knowledge_view_configure(view, view->capacity ? view->capacity
                                                  : KNOWLEDGE_VIEW_MAX_CAP);
    market_memory_t merged = *memory;
    uint8_t cap = knowledge_view_capacity(view);
    for (int i = 0; i < view->count && i < cap; i++) {
        market_memory_t existing;
        if (!market_memory_from_knowledge_item(&view->items[i], &existing))
            continue;
        if (existing.memory_kind != memory->memory_kind) continue;
        if (existing.station_a != memory->station_a) continue;
        if (existing.commodity != memory->commodity) continue;
        if (existing.action != memory->action) continue;
        uint32_t evidence = (uint32_t)existing.quantity_hint +
                            (uint32_t)memory->quantity_hint;
        if (evidence > 65535u) evidence = 65535u;
        merged.quantity_hint = (uint16_t)evidence;
        uint32_t value = (uint32_t)existing.value_hint +
                         (uint32_t)memory->value_hint;
        if (value > 65535u) value = 65535u;
        merged.value_hint = (uint16_t)value;
        int conf = (int)existing.confidence + 14;
        if (memory->confidence > conf) conf = memory->confidence;
        merged.confidence = (uint8_t)(conf > 255 ? 255 : conf);
        int sal = (int)existing.salience + (int)(memory->salience / 4);
        if (memory->salience > sal) sal = memory->salience;
        merged.salience = (uint8_t)(sal > 255 ? 255 : sal);
        if (existing.observed_tick > merged.observed_tick)
            merged.observed_tick = existing.observed_tick;
        break;
    }

    knowledge_item_t item;
    if (knowledge_item_from_market_memory(&merged, &item))
        knowledge_view_insert(view, &item);
}

static void knowledge_seed_stale_contract_risk(knowledge_view_t *view,
                                               const contract_summary_t *s) {
    if (!view || !s) return;
    market_memory_t risk;
    if (market_memory_from_stale_contract_summary(s, &risk))
        knowledge_view_reinforce_station_trust(view, &risk);
}

static bool knowledge_view_find_route_memory(const knowledge_view_t *view,
                                             const market_memory_t *needle,
                                             market_memory_t *out) {
    if (!view || !needle) return false;
    uint8_t cap = knowledge_view_capacity(view);
    for (int i = 0; i < view->count && i < cap; i++) {
        market_memory_t existing;
        if (!market_memory_from_knowledge_item(&view->items[i], &existing))
            continue;
        if (existing.memory_kind != needle->memory_kind) continue;
        if (existing.station_a != needle->station_a) continue;
        if (existing.station_b != needle->station_b) continue;
        if (existing.commodity != needle->commodity) continue;
        if (existing.action != needle->action) continue;
        if (out) *out = existing;
        return true;
    }
    return false;
}

static void gossip_emit_route_history_if_promoted(world_t *w,
                                                  station_t *authority,
                                                  const market_memory_t *before,
                                                  const market_memory_t *after) {
    if (!w || !authority || !after || !after->active) return;
    if (after->memory_kind != (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION &&
        after->memory_kind != (uint8_t)MARKET_MEMORY_ROUTE_RISK) {
        return;
    }
    if (after->station_a >= MAX_STATIONS || after->station_b >= MAX_STATIONS)
        return;
    uint16_t old_evidence = before && before->active
        ? before->quantity_hint
        : 0;
    if (old_evidence >= GOSSIP_ROUTE_HISTORY_PROMOTE_EVIDENCE)
        return;
    if (after->quantity_hint < GOSSIP_ROUTE_HISTORY_PROMOTE_EVIDENCE)
        return;

    chain_payload_route_history_t payload = {0};
    payload.memory_kind = after->memory_kind;
    payload.origin_station = after->station_b;
    payload.destination_station = after->station_a;
    payload.commodity = after->commodity;
    payload.action = after->action;
    payload.confidence = after->confidence;
    payload.salience = after->salience;
    payload.evidence_count = after->quantity_hint;
    payload.value_hint = after->value_hint;
    payload.observed_tick = after->observed_tick;
    payload.subject_nonce = after->subject_nonce;
    (void)chain_log_emit(w, authority, CHAIN_EVT_ROUTE_HISTORY,
                         &payload, (uint16_t)sizeof(payload));
}

static void knowledge_promote_delivery_receipt_history(
    knowledge_view_t *view,
    const knowledge_item_t *item,
    world_t *w,
    station_t *authority) {
    if (!view || !item) return;
    market_memory_t receipt;
    if (!market_memory_from_knowledge_item(item, &receipt)) return;
    if (receipt.memory_kind != (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT)
        return;
    if (receipt.station_a >= MAX_STATIONS || receipt.station_b >= MAX_STATIONS)
        return;
    if (receipt.commodity >= (uint8_t)COMMODITY_COUNT) return;
    if (receipt.quantity_hint == 0) return;

    market_memory_t reputation;
    if (market_memory_from_route_reputation((int)receipt.station_b,
                                            (int)receipt.station_a,
                                            (commodity_t)receipt.commodity,
                                            receipt.quantity_hint,
                                            (float)receipt.value_hint,
                                            receipt.observed_tick,
                                            false,
                                            &reputation)) {
        market_memory_t before = {0};
        bool had_before = knowledge_view_find_route_memory(view, &reputation,
                                                           &before);
        knowledge_view_reinforce_route_reputation(view, &reputation);
        market_memory_t after = {0};
        if (knowledge_view_find_route_memory(view, &reputation, &after)) {
            gossip_emit_route_history_if_promoted(w, authority,
                                                  had_before ? &before : NULL,
                                                  &after);
        }
    }

    market_memory_t trust;
    if (market_memory_from_station_trust((int)receipt.station_a,
                                         (uint8_t)CONTRACT_DELIVERY,
                                         (commodity_t)receipt.commodity,
                                         receipt.quantity_hint,
                                         (float)receipt.value_hint,
                                         receipt.observed_tick,
                                         &trust)) {
        knowledge_view_reinforce_station_trust(view, &trust);
    }
}

static void knowledge_view_exchange_internal(knowledge_view_t *a,
                                             knowledge_view_t *b,
                                             world_t *w,
                                             station_t *a_authority,
                                             station_t *b_authority) {
    if (!a || !b) return;
    uint8_t a_count = a->count;
    uint8_t b_count = b->count;
    if (a_count > knowledge_view_capacity(a)) a_count = knowledge_view_capacity(a);
    if (b_count > knowledge_view_capacity(b)) b_count = knowledge_view_capacity(b);

    knowledge_item_t a_pre[KNOWLEDGE_VIEW_MAX_CAP];
    knowledge_item_t b_pre[KNOWLEDGE_VIEW_MAX_CAP];
    for (int i = 0; i < a_count; i++) a_pre[i] = a->items[i];
    for (int i = 0; i < b_count; i++) b_pre[i] = b->items[i];

    for (int i = 0; i < a_count; i++) {
        if (a_pre[i].kind == (uint8_t)KNOW_NONE) continue;
        if (a_pre[i].hops < 255) a_pre[i].hops++;
        bool already_known = knowledge_view_contains_item(b, &a_pre[i]);
        knowledge_view_insert(b, &a_pre[i]);
        if (!already_known)
            knowledge_promote_delivery_receipt_history(b, &a_pre[i],
                                                       w, b_authority);
    }
    for (int i = 0; i < b_count; i++) {
        if (b_pre[i].kind == (uint8_t)KNOW_NONE) continue;
        if (b_pre[i].hops < 255) b_pre[i].hops++;
        bool already_known = knowledge_view_contains_item(a, &b_pre[i]);
        knowledge_view_insert(a, &b_pre[i]);
        if (!already_known)
            knowledge_promote_delivery_receipt_history(a, &b_pre[i],
                                                       w, a_authority);
    }
}

void knowledge_view_exchange(knowledge_view_t *a, knowledge_view_t *b) {
    knowledge_view_exchange_internal(a, b, NULL, NULL, NULL);
}

void knowledge_view_decay(knowledge_view_t *view,
                          uint8_t confidence_decay,
                          uint8_t salience_decay) {
    if (!view) return;
    uint8_t cap = knowledge_view_capacity(view);
    int out = 0;
    for (int i = 0; i < view->count && i < cap; i++) {
        knowledge_item_t item = view->items[i];
        if (item.kind == (uint8_t)KNOW_NONE) continue;
        item.confidence = item.confidence > confidence_decay
            ? (uint8_t)(item.confidence - confidence_decay)
            : 0;
        item.salience = item.salience > salience_decay
            ? (uint8_t)(item.salience - salience_decay)
            : 0;
        if (item.confidence == 0 || item.salience == 0)
            continue;
        if (item.kind == (uint8_t)KNOW_MARKET &&
            item.payload_kind == (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY) {
            market_memory_t memory;
            if (market_memory_from_knowledge_item(&item, &memory)) {
                memory.confidence = item.confidence;
                memory.salience = item.salience;
                memcpy(item.payload, &memory, sizeof(memory));
            }
        }
        view->items[out] = item;
        out++;
    }
    for (int i = out; i < view->count && i < cap; i++)
        memset(&view->items[i], 0, sizeof(view->items[i]));
    view->count = (uint8_t)out;
}

static void knowledge_seed_contract_pool(knowledge_view_t *view,
                                         const contract_summary_t *list,
                                         uint8_t count, int cap) {
    if (!view || !list || cap <= 0) return;
    int n = count < cap ? count : cap;
    for (int i = 0; i < n; i++) {
        if (!list[i].active) continue;
        knowledge_item_t item;
        if (knowledge_item_from_contract_summary(&list[i], &item))
            knowledge_view_insert(view, &item);
        market_memory_t memory;
        if (market_memory_from_contract_summary(&list[i], &memory) &&
            knowledge_item_from_market_memory(&memory, &item)) {
            knowledge_view_insert(view, &item);
        }
        knowledge_seed_stale_contract_risk(view, &list[i]);
    }
}

static void knowledge_seed_station_supply(knowledge_view_t *view,
                                          const station_t *st,
                                          int station_index,
                                          uint32_t observed_tick) {
    if (!view || !st) return;
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
        market_memory_t memory;
        knowledge_item_t item;
        if (market_memory_from_station_supply(st, station_index,
                                              (commodity_t)c,
                                              observed_tick,
                                              &memory) &&
            knowledge_item_from_market_memory(&memory, &item)) {
            knowledge_view_insert(view, &item);
        }
    }
}

static void knowledge_seed_station_ore_pressure(knowledge_view_t *view,
                                                const station_t *st,
                                                int station_index,
                                                uint32_t observed_tick) {
    if (!view || !st) return;
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
        market_memory_t memory;
        knowledge_item_t item;
        if (market_memory_from_ore_pressure(st, station_index,
                                            (commodity_t)c,
                                            observed_tick,
                                            &memory) &&
            knowledge_item_from_market_memory(&memory, &item)) {
            knowledge_view_insert(view, &item);
        }
    }
}

static bool gossip_station_accepts_scaffold_type(const station_t *st,
                                                 module_type_t type) {
    if (!st) return false;
    if (type == MODULE_SIGNAL_RELAY && st->planned) return true;
    if (!station_is_active(st)) return false;
    for (int p = 0; p < st->placement_plan_count; p++) {
        if (st->placement_plans[p].type == type) return true;
    }
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].scaffold && st->modules[m].type == type)
            return true;
    }
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        if (ring > 1 && !ring_has_dock(st, ring - 1)) continue;
        if (station_ring_free_slot(st, ring, STATION_RING_SLOTS[ring]) >= 0)
            return true;
    }
    return false;
}

static void knowledge_seed_station_scaffold_pressure(knowledge_view_t *view,
                                                    const world_t *w,
                                                    int station_index,
                                                    uint32_t observed_tick) {
    if (!view || !w || station_index < 0 || station_index >= MAX_STATIONS)
        return;
    const station_t *dest = &w->stations[station_index];
    if (!station_is_active(dest) && !dest->planned) return;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active || sc->state != SCAFFOLD_LOOSE) continue;
        if (sc->towed_by >= 0) continue;
        if (!gossip_station_accepts_scaffold_type(dest, sc->module_type))
            continue;
        market_memory_t memory;
        knowledge_item_t item;
        int source = sc->built_at_station;
        uint64_t nonce = (uint64_t)MARKET_MEMORY_SCAFFOLD_PRESSURE
                       | ((uint64_t)(uint16_t)i << 8)
                       | ((uint64_t)(uint8_t)station_index << 24)
                       | ((uint64_t)(uint8_t)sc->module_type << 32);
        if (market_memory_from_scaffold_pressure(station_index, source,
                                                 sc->module_type,
                                                 observed_tick, nonce,
                                                 &memory) &&
            knowledge_item_from_market_memory(&memory, &item)) {
            knowledge_view_insert(view, &item);
        }
    }
}

void knowledge_view_forget_contract(knowledge_view_t *view, uint8_t action,
                                    int station_idx, commodity_t commodity) {
    if (!view || station_idx < 0 || station_idx > 255) return;
    int out = 0;
    uint8_t cap = knowledge_view_capacity(view);
    for (int i = 0; i < view->count && i < cap; i++) {
        const knowledge_item_t *item = &view->items[i];
        bool drop = false;
        contract_summary_t s;
        if (contract_summary_from_knowledge_item(item, &s)) {
            drop = s.action == action &&
                   s.station_index == (uint8_t)station_idx &&
                   s.commodity == (uint8_t)commodity;
        } else {
            market_memory_t memory;
            if (market_memory_from_knowledge_item(item, &memory)) {
                drop = memory.memory_kind == (uint8_t)MARKET_MEMORY_DEMAND &&
                       memory.action == action &&
                       memory.station_a == (uint8_t)station_idx &&
                       memory.commodity == (uint8_t)commodity;
            }
        }
        if (!drop) {
            if (out != i) view->items[out] = view->items[i];
            out++;
        }
    }
    for (int i = out; i < view->count && i < cap; i++)
        memset(&view->items[i], 0, sizeof(view->items[i]));
    view->count = (uint8_t)out;
}

void contract_pool_insert(contract_summary_t *list, uint8_t *count, int cap,
                          const contract_summary_t *s) {
    if (!list || !count || !s || cap <= 0) return;
    /* Dedup-on-match: keep whichever snapshot is "newer" (larger
     * age_at_copy = snapshotted later in the contract's life). This
     * stops an older snapshot from clobbering a fresher one when ships
     * arrive out of order. */
    for (int i = 0; i < *count; i++) {
        if (contract_summary_matches(&list[i], s)) {
            if (s->age_at_copy >= list[i].age_at_copy) {
                list[i] = *s;
            }
            return;
        }
    }
    if (*count < cap) {
        list[*count] = *s;
        (*count)++;
    } else {
        /* FIFO eviction: oldest entry (slot 0) drops, others shift left. */
        for (int i = 1; i < cap; i++)
            list[i-1] = list[i];
        list[cap - 1] = *s;
    }
}

static void gossip_seed_station_local_pressure(world_t *w, int station_index,
                                               bool decay) {
    if (!w || station_index < 0 || station_index >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_index];
    knowledge_view_configure(&st->knowledge, STATION_KNOWN_ITEM_CAP);
    if (decay)
        knowledge_view_decay(&st->knowledge, 1, 1);

    knowledge_seed_contract_pool(&st->knowledge, st->known_contracts,
                                 st->known_contract_count,
                                 STATION_KNOWN_CONTRACT_CAP);
    knowledge_seed_station_supply(&st->knowledge, st, station_index, w->tick);
    knowledge_seed_station_ore_pressure(&st->knowledge, st, station_index,
                                        w->tick);
    knowledge_seed_station_scaffold_pressure(&st->knowledge, w, station_index,
                                             w->tick);

    /* Merge this station's locally-issued contracts into its own
     * known pool. Tractor/fracture contracts are local by station_index.
     * Delivery contracts are visible at both ends: station_index is the
     * destination, target_index is the recourse origin. */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &w->contracts[k];
        if (!ct->active) continue;
        if (ct->station_index != station_index &&
            !(ct->action == CONTRACT_DELIVERY &&
              ct->target_index == station_index)) {
            continue;
        }
        contract_summary_t s = contract_summary_make(ct);
        contract_pool_insert(st->known_contracts, &st->known_contract_count,
                             STATION_KNOWN_CONTRACT_CAP, &s);
        knowledge_item_t item;
        if (knowledge_item_from_contract_summary(&s, &item))
            knowledge_view_insert(&st->knowledge, &item);
        market_memory_t memory;
        if (market_memory_from_contract_summary(&s, &memory) &&
            knowledge_item_from_market_memory(&memory, &item)) {
            knowledge_view_insert(&st->knowledge, &item);
        }
        knowledge_seed_stale_contract_risk(&st->knowledge, &s);
    }

    (void)gossip_signal_field_observe_view(w, &st->knowledge, st->pos);
}

void gossip_dock_handshake(world_t *w, int station_index,
                           contract_summary_t *ship_pool,
                           uint8_t *ship_count, int ship_cap,
                           knowledge_view_t *ship_knowledge) {
    if (!w || !ship_pool || !ship_count) return;
    if (station_index < 0 || station_index >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_index];
    if (ship_knowledge)
        knowledge_view_configure(ship_knowledge, SHIP_KNOWN_ITEM_CAP);

    /* Dock contact is the natural cadence for v0 situated-memory decay:
     * memories fade as they are carried between stations, while the
     * local exact contract pass below immediately refreshes active
     * station-authored pressure. */
    gossip_seed_station_local_pressure(w, station_index, true);
    if (ship_knowledge)
        knowledge_view_decay(ship_knowledge, 1, 2);

    if (ship_knowledge) {
        knowledge_seed_contract_pool(ship_knowledge, ship_pool, *ship_count,
                                     ship_cap);
    }

    /* Bidirectional copy. Snapshot ship's pre-handshake set so we
     *    don't double-pump after station-side merges back into ship. */
    contract_summary_t ship_pre[SHIP_KNOWN_CONTRACT_CAP];
    int max_ship_pre = ship_cap < SHIP_KNOWN_CONTRACT_CAP ? ship_cap : SHIP_KNOWN_CONTRACT_CAP;
    uint8_t ship_pre_count = *ship_count < max_ship_pre ? *ship_count : (uint8_t)max_ship_pre;
    for (int i = 0; i < ship_pre_count; i++)
        ship_pre[i] = ship_pool[i];

    /* Station → ship */
    for (int i = 0; i < st->known_contract_count; i++) {
        if (!st->known_contracts[i].active) continue;
        contract_pool_insert(ship_pool, ship_count, ship_cap,
                             &st->known_contracts[i]);
    }
    /* Ship → station */
    for (int i = 0; i < ship_pre_count; i++) {
        if (!ship_pre[i].active) continue;
        contract_pool_insert(st->known_contracts, &st->known_contract_count,
                             STATION_KNOWN_CONTRACT_CAP, &ship_pre[i]);
    }

    if (ship_knowledge)
        knowledge_view_exchange_internal(&st->knowledge, ship_knowledge,
                                         w, st, NULL);
    (void)gossip_signal_field_observe_view(w, &st->knowledge, st->pos);
    if (ship_knowledge)
        (void)gossip_signal_field_observe_view(w, ship_knowledge, st->pos);
}

void gossip_bootstrap_world_stations(world_t *w) {
    if (!w) return;
    signal_field_init(&w->signal_field);
    w->signal_field_decay_tick = w->tick;
    for (int s_idx = 0; s_idx < MAX_STATIONS; s_idx++) {
        if (!station_is_active(&w->stations[s_idx])) continue;
        gossip_seed_station_local_pressure(w, s_idx, false);
    }
}

/* ---- Holographic experience exchange ---- */

static int gossip_hnn_store_market_view(hnn_memory_t *mem,
                                        const knowledge_view_t *view) {
    if (!mem || !view) return 0;
    if (mem->experience_count > 0) return 0;
    int count = view->count;
    if (count > KNOWLEDGE_VIEW_MAX_CAP) count = KNOWLEDGE_VIEW_MAX_CAP;
    int stored = 0;
    for (int i = 0; i < count; i++) {
        market_memory_t memory;
        if (market_memory_from_knowledge_item(&view->items[i], &memory) &&
            gossip_hnn_store_market_memory(mem, &memory)) {
            stored++;
            if (stored >= 4) break;
        }
    }
    return stored;
}

static void gossip_hnn_bundle_memory(hnn_memory_t *dst,
                                     const hnn_memory_t *src) {
    if (!dst || !src || src->experience_count <= 0) return;
    for (int i = 0; i < HNN_DIM; i++)
        dst->store[i] += src->store[i];
    dst->experience_count += src->experience_count;
    if (dst->experience_count > (int)HNN_TRACE_CAPACITY)
        dst->experience_count = (int)HNN_TRACE_CAPACITY;
    hnn_normalize(dst->store);
}

static void gossip_hnn_clamp_market_memory(hnn_memory_t *mem) {
    if (!mem) return;
    if (mem->experience_count <= GOSSIP_HNN_MARKET_MAX_EXPERIENCE) return;
    mem->experience_count = GOSSIP_HNN_MARKET_MAX_EXPERIENCE;
    hnn_normalize(mem->store);
}

void gossip_hnn_decay_market_memory(hnn_memory_t *mem,
                                    uint32_t *last_decay_tick,
                                    uint32_t now_tick) {
    if (!mem || !last_decay_tick) return;

    if (*last_decay_tick == 0) {
        *last_decay_tick = now_tick + 1u;
        return;
    }

    uint32_t last_tick = *last_decay_tick - 1u;
    if (now_tick <= last_tick) return;

    uint32_t elapsed = now_tick - last_tick;
    uint32_t epochs = elapsed / (uint32_t)GOSSIP_HNN_MARKET_DECAY_TICKS;
    if (epochs == 0) return;

    last_tick += epochs * (uint32_t)GOSSIP_HNN_MARKET_DECAY_TICKS;
    *last_decay_tick = last_tick + 1u;

    if (mem->experience_count <= 0) return;
    if (epochs >= (uint32_t)mem->experience_count) {
        hnn_memory_init(mem);
        return;
    }

    mem->experience_count -= (int)epochs;
    hnn_normalize(mem->store);
}

static void gossip_hnn_bundle_market_memory(hnn_memory_t *dst,
                                            const hnn_memory_t *src) {
    if (!dst || !src || src->experience_count <= 0) return;
    if (dst->experience_count <= 0) {
        *dst = *src;
        gossip_hnn_clamp_market_memory(dst);
        return;
    }

    const float retain =
        (float)GOSSIP_HNN_MARKET_RETAIN_NUM /
        (float)GOSSIP_HNN_MARKET_RETAIN_DEN;
    for (int i = 0; i < HNN_DIM; i++)
        dst->store[i] = dst->store[i] * retain + src->store[i];

    int retained_count =
        (dst->experience_count * GOSSIP_HNN_MARKET_RETAIN_NUM +
         GOSSIP_HNN_MARKET_RETAIN_DEN - 1) /
        GOSSIP_HNN_MARKET_RETAIN_DEN;
    dst->experience_count = retained_count + src->experience_count;
    gossip_hnn_clamp_market_memory(dst);
}

static bool gossip_npc_contact_in_range(const npc_ship_t *a,
                                        const npc_ship_t *b) {
    if (!a || !b || !a->active || !b->active) return false;
    if (a->state == NPC_STATE_DOCKED || b->state == NPC_STATE_DOCKED)
        return false;
    float dx = a->ship.pos.x - b->ship.pos.x;
    float dy = a->ship.pos.y - b->ship.pos.y;
    float range = (float)GOSSIP_SHIP_CONTACT_RANGE;
    return dx * dx + dy * dy <= range * range;
}

static void gossip_contract_pool_exchange(contract_summary_t *a_pool,
                                          uint8_t *a_count,
                                          contract_summary_t *b_pool,
                                          uint8_t *b_count) {
    if (!a_pool || !a_count || !b_pool || !b_count) return;
    uint8_t a_pre_count = *a_count;
    uint8_t b_pre_count = *b_count;
    if (a_pre_count > SHIP_KNOWN_CONTRACT_CAP)
        a_pre_count = SHIP_KNOWN_CONTRACT_CAP;
    if (b_pre_count > SHIP_KNOWN_CONTRACT_CAP)
        b_pre_count = SHIP_KNOWN_CONTRACT_CAP;

    contract_summary_t a_pre[SHIP_KNOWN_CONTRACT_CAP];
    contract_summary_t b_pre[SHIP_KNOWN_CONTRACT_CAP];
    for (int i = 0; i < a_pre_count; i++) a_pre[i] = a_pool[i];
    for (int i = 0; i < b_pre_count; i++) b_pre[i] = b_pool[i];

    for (int i = 0; i < a_pre_count; i++)
        contract_pool_insert(b_pool, b_count, SHIP_KNOWN_CONTRACT_CAP,
                             &a_pre[i]);
    for (int i = 0; i < b_pre_count; i++)
        contract_pool_insert(a_pool, a_count, SHIP_KNOWN_CONTRACT_CAP,
                             &b_pre[i]);
}

static void gossip_hnn_ship_contact_exchange(npc_ship_t *a, npc_ship_t *b) {
    if (!a || !b) return;
    if (a->brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT ||
        b->brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT)
        return;
    hnn_memory_t a_pre = a->hnn_market_mem;
    hnn_memory_t b_pre = b->hnn_market_mem;
    bool a_changed = false;
    bool b_changed = false;
    if (b_pre.experience_count > 0) {
        gossip_hnn_bundle_market_memory(&a->hnn_market_mem, &b_pre);
        a_changed = true;
    }
    if (a_pre.experience_count > 0) {
        gossip_hnn_bundle_market_memory(&b->hnn_market_mem, &a_pre);
        b_changed = true;
    }
    if (a_changed) {
        a->hnn_market_station = 0xffu;
        a->hnn_market_version = 0;
    }
    if (b_changed) {
        b->hnn_market_station = 0xffu;
        b->hnn_market_version = 0;
    }
}

int gossip_ship_contact_exchange(world_t *w) {
    if (!w) return 0;
    int contacts = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *a = &w->npc_ships[i];
        if (!a->active) continue;
        knowledge_view_configure(&a->knowledge, SHIP_KNOWN_ITEM_CAP);
        for (int j = i + 1; j < MAX_NPC_SHIPS; j++) {
            npc_ship_t *b = &w->npc_ships[j];
            if (!gossip_npc_contact_in_range(a, b)) continue;
            knowledge_view_configure(&b->knowledge, SHIP_KNOWN_ITEM_CAP);
            gossip_contract_pool_exchange(a->known_contracts,
                                          &a->known_contract_count,
                                          b->known_contracts,
                                          &b->known_contract_count);
            knowledge_view_exchange(&a->knowledge, &b->knowledge);
            gossip_hnn_ship_contact_exchange(a, b);
            vec2 contact_pos = v2_scale(v2_add(a->ship.pos, b->ship.pos),
                                        0.5f);
            (void)gossip_signal_field_observe_view(w, &a->knowledge,
                                                   contact_pos);
            (void)gossip_signal_field_observe_view(w, &b->knowledge,
                                                   contact_pos);
            if (a->hnn_market_mem.experience_count > 0 ||
                b->hnn_market_mem.experience_count > 0) {
                (void)signal_field_observe(&w->signal_field, contact_pos,
                                           SIGNAL_FIELD_KIND_HOLOGRAM,
                                           0.35f, w->tick);
            }
            contacts++;
        }
    }
    return contacts;
}

static bool gossip_hnn_pilot_carries_other_station_cell(
    const npc_ship_t *npc,
    int station_idx) {
    if (!npc || npc->hnn_experience_version == 0) return false;
    if (npc->hnn_experience_station == 0xffu) return false;
    return npc->hnn_experience_station != (uint8_t)station_idx;
}

static bool gossip_hnn_trace_contract_matches_runtime(
    const hnn_memory_contract_t *contract) {
    if (!contract) return false;
    if (contract->dim != HNN_DIM) return false;
    if (contract->seed != HNN_CONTRACT_SEED) return false;
    if (contract->keygen_version != HNN_KEYGEN_VERSION) return false;
    if (contract->encoder_version != HNN_PILOT_ENCODER_VERSION) return false;
    if (contract->trace_format_version != HNN_TRACE_FORMAT_VERSION)
        return false;
    if (contract->action_vocabulary_hash != hnn_action_vocabulary_hash())
        return false;
    if (contract->stored_count <= 0) return false;
    if (!isfinite(contract->capacity_load) ||
        !isfinite(contract->fidelity_estimate) ||
        !isfinite(contract->last_margin)) {
        return false;
    }
    if (contract->capacity_load < 0.0f) return false;
    return true;
}

static bool gossip_hnn_trace_contract_is_mergeable_cargo(
    const hnn_memory_contract_t *contract) {
    if (!gossip_hnn_trace_contract_matches_runtime(contract)) return false;
    if (contract->stored_count > (int)HNN_TRACE_CAPACITY) return false;
    if (contract->capacity_load > GOSSIP_HNN_TRACE_CARGO_MAX_LOAD)
        return false;
    if (contract->fidelity_estimate < GOSSIP_HNN_TRACE_CARGO_MIN_FIDELITY)
        return false;
    return true;
}

static bool gossip_hnn_carried_station_cell_has_provenance(
    const world_t *w,
    int station_idx,
    const npc_ship_t *npc) {
    if (!w || !npc) return false;
    if (!gossip_hnn_pilot_carries_other_station_cell(npc, station_idx))
        return false;

    uint8_t source_idx = npc->hnn_experience_station;
    if (source_idx >= MAX_STATIONS) return false;
    const station_t *source = &w->stations[source_idx];
    if (!station_exists(source)) return false;
    if (source->hnn_experience.experience_count <= 0) return false;
    if (source->hnn_experience_version == 0) return false;
    if (npc->hnn_experience_version > source->hnn_experience_version)
        return false;
    return true;
}

static bool gossip_hnn_pilot_should_upload_experience(
    const world_t *w,
    const station_t *st,
    int station_idx,
    const npc_ship_t *npc) {
    if (!st || !npc || npc->hnn_mem.experience_count <= 0) return false;

    hnn_memory_contract_t contract = hnn_memory_contract(&npc->hnn_mem);
    if (!gossip_hnn_trace_contract_matches_runtime(&contract)) return false;

    bool carries_other_station_cell =
        gossip_hnn_pilot_carries_other_station_cell(npc, station_idx);
    if (carries_other_station_cell) {
        if (!gossip_hnn_trace_contract_is_mergeable_cargo(&contract))
            return false;
        if (!gossip_hnn_carried_station_cell_has_provenance(w,
                                                            station_idx,
                                                            npc)) {
            return false;
        }
        return npc->hnn_experience_uploaded_station !=
                   (uint8_t)station_idx ||
               npc->hnn_experience_uploaded_source_station !=
                   npc->hnn_experience_station ||
               npc->hnn_experience_uploaded_source_version <
                   npc->hnn_experience_version;
    }

    if (st->hnn_experience.experience_count <= 0) return true;

    if (npc->hnn_experience_local_version > 0 &&
        (npc->hnn_experience_uploaded_station != (uint8_t)station_idx ||
         npc->hnn_experience_uploaded_local_version <
            npc->hnn_experience_local_version)) {
        return true;
    }

    return false;
}

static void gossip_hnn_mark_pilot_uploaded(int station_idx,
                                           npc_ship_t *npc) {
    if (!npc || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    npc->hnn_experience_uploaded_station = (uint8_t)station_idx;
    npc->hnn_experience_uploaded_local_version =
        npc->hnn_experience_local_version;
    npc->hnn_experience_uploaded_source_station =
        npc->hnn_experience_station;
    npc->hnn_experience_uploaded_source_version =
        npc->hnn_experience_version;
}

void gossip_hnn_exchange(world_t *w, int station_idx, npc_ship_t *npc) {
    if (!w || !npc || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_idx];
    if (!station_exists(st)) return;

    gossip_hnn_decay_market_memory(&st->hnn_market_memory,
                                   &st->hnn_market_decay_tick, w->tick);

    int station_market_stored =
        gossip_hnn_store_market_view(&st->hnn_market_memory, &st->knowledge);
    if (station_market_stored > 0) {
        st->hnn_market_version++;
        (void)signal_field_observe(&w->signal_field, st->pos,
                                   SIGNAL_FIELD_KIND_HOLOGRAM,
                                   gossip_clampf((float)station_market_stored *
                                                 0.18f,
                                                 0.18f, 0.72f),
                                   w->tick);
    }

    if (npc->brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT) {
        gossip_hnn_decay_market_memory(&npc->hnn_market_mem,
                                       &npc->hnn_market_decay_tick, w->tick);
        (void)gossip_hnn_store_market_view(&npc->hnn_market_mem,
                                           &npc->knowledge);
        if (npc->hnn_market_mem.experience_count > 0 &&
            (st->hnn_market_memory.experience_count <= 0 ||
             npc->hnn_market_station != (uint8_t)station_idx ||
             npc->hnn_market_version != st->hnn_market_version)) {
            gossip_hnn_bundle_market_memory(&st->hnn_market_memory,
                                            &npc->hnn_market_mem);
            st->hnn_market_version++;
            (void)signal_field_observe(&w->signal_field, st->pos,
                                       SIGNAL_FIELD_KIND_HOLOGRAM,
                                       0.45f, w->tick);
        }
        if (st->hnn_market_memory.experience_count > 0 &&
            (npc->hnn_market_station != (uint8_t)station_idx ||
             npc->hnn_market_version < st->hnn_market_version)) {
            gossip_hnn_bundle_market_memory(&npc->hnn_market_mem,
                                            &st->hnn_market_memory);
            npc->hnn_market_station = (uint8_t)station_idx;
            npc->hnn_market_version = st->hnn_market_version;
        }
    }

    if (npc->brain_mode != SERVER_BRAIN_MODE_HOLOGRAPHIC) return;

    GOSSIP_HNN_DEBUG_LOG("[hnn] exchange called: slot=%ld mode=%d st=%d exp=%d\n",
                         (long)(npc - w->npc_ships),
                         npc->brain_mode,
                         station_idx,
                         npc->hnn_mem.experience_count);

    bool station_cell_was_empty = st->hnn_experience.experience_count <= 0;
    bool uploaded = false;

    /*
     * Upload: bundle the pilot's experience into this station's local cell
     * only when the worker has new local experience or is carrying a memory
     * cell learned somewhere else. Re-docking at the same station without new
     * stores must not amplify the same hologram.
     */
    if (gossip_hnn_pilot_should_upload_experience(w, st, station_idx, npc)) {
        gossip_hnn_bundle_memory(&st->hnn_experience, &npc->hnn_mem);
        st->hnn_experience_version++;
        st->hnn_experience_upload_count++;
        st->hnn_experience_last_source_station =
            (npc->hnn_experience_version > 0 &&
             npc->hnn_experience_station != 0xffu)
                ? npc->hnn_experience_station
                : (uint8_t)station_idx;
        gossip_hnn_mark_pilot_uploaded(station_idx, npc);
        uploaded = true;

        GOSSIP_HNN_DEBUG_LOG("[hnn] pilot %d uploaded experience to station %d "
                             "(pilot_count=%d, station_total=%d, version=%u)\n",
                             (int)(npc - w->npc_ships),
                             station_idx,
                             npc->hnn_mem.experience_count,
                             st->hnn_experience.experience_count,
                             st->hnn_experience_version);
    }

    /*
     * Download: if the station has experience the pilot hasn't seen yet
     * (version mismatch), bundle it into the pilot's memory. When the station
     * cell was empty and this pilot just seeded it, only stamp the provenance:
     * bundling the same vector back into the pilot would just self-amplify it.
     */
    if (st->hnn_experience.experience_count > 0 &&
        (npc->hnn_experience_station != (uint8_t)station_idx ||
         npc->hnn_experience_version < st->hnn_experience_version)) {

        if (!(uploaded && station_cell_was_empty))
            gossip_hnn_bundle_memory(&npc->hnn_mem, &st->hnn_experience);
        npc->hnn_experience_station = (uint8_t)station_idx;
        npc->hnn_experience_version = st->hnn_experience_version;
        st->hnn_experience_download_count++;
        if (npc->hnn_experience_uploaded_station == (uint8_t)station_idx) {
            npc->hnn_experience_uploaded_source_station = (uint8_t)station_idx;
            npc->hnn_experience_uploaded_source_version =
                st->hnn_experience_version;
        }

        GOSSIP_HNN_DEBUG_LOG("[hnn] pilot %d downloaded experience from station %d "
                             "(station_version=%u, pilot_total=%d)\n",
                             (int)(npc - w->npc_ships),
                             station_idx,
                             st->hnn_experience_version,
                             npc->hnn_mem.experience_count);
    }
}
