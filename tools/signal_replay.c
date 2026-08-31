/*
 * signal_replay.c -- deterministic counterfactual replay harness.
 *
 * Rebuilds a seeded Signal world, replays a low-level input prefix, branches a
 * bounded candidate action set, and emits JSONL rows with replay hashes and
 * safety/economy counters. This is intentionally narrower than full chain-log
 * world reconstruction: it is the reusable seed+prefix harness that research
 * tools can call before the ledger-to-world replay CLI exists.
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define SR_GETPID() ((unsigned long)_getpid())
#define SR_MKDIR(path) _mkdir(path)
#define SR_RMDIR(path) _rmdir(path)
#else
#include <unistd.h>
#define SR_GETPID() ((unsigned long)getpid())
#define SR_MKDIR(path) mkdir((path), 0700)
#define SR_RMDIR(path) rmdir(path)
#endif

#include "chain_log.h"
#include "cargo_receipt.h"
#include "cargo_receipt_issue.h"
#include "fixpoint.h"
#include "game_sim.h"
#include "gossip.h"
#include "holographic_nn.h"
#include "holographic_nn_backend.h"
#include "holographic_nn_confidence.h"
#include "manifest.h"
#include "protocol.h"
#include "sha256.h"
#include "sim_ai.h"
#include "sim_asteroid.h"
#include "sim_construction.h"
#include "signal_intelligence.h"
#include "sim_nav.h"
#include "sim_physics.h"
#include "station_authority.h"
#include "station_util.h"
#include "state_digest.h"

#define SR_SCHEMA "signal.replay_counterfactual.v1"
#define SR_AI_EVAL_SCHEMA "signal.ai_eval_world.v1"
#define SR_OUTCOME_FACTS_SCHEMA "signal.ai_outcome_facts.v1"
#define SR_AI_EVAL_CORPUS_VERSION 1u
#define SR_AI_EVAL_GENERATOR_VERSION 1u
#define SR_PUBLIC_STATE_HASH_SCHEMA "signal.replay.public_state_hash"
#define SR_PUBLIC_STATE_HASH_VERSION 7u
#define SR_PUBLIC_STATE_HASH_DOMAIN "signal-replay-state-v7-public"
#define SR_PUBLIC_EVENT_HASH_SCHEMA "signal.replay.public_event_hash"
#define SR_PUBLIC_EVENT_HASH_VERSION 3u
#define SR_PUBLIC_EVENT_HASH_DOMAIN \
    "signal-replay-events-v3-public-actor"
#define SR_OUTCOME_REPORT_VERSION 1u
#define SR_ACTION_COUNT 9
#define SR_MAX_PREFIX 4096
#define SR_MAX_HORIZON_TICKS 120000
#define SR_EVAL_MAX_OUTPOSTS 8
#define SR_ROUTE_CELL_CAP 256
#define SR_ROUTE_CELL_SIZE 256.0f
#define SR_STUCK_TICK_THRESHOLD 120
#define SR_GOAL_COMPLETION_RADIUS 250.0f

typedef enum {
    SR_PROVENANCE_SCRIPT_NONE = 0,
    SR_PROVENANCE_SCRIPT_BUY_SELL,
    SR_PROVENANCE_SCRIPT_POD_TOW_SELL,
    SR_PROVENANCE_SCRIPT_MINE_FRACTURE,
    SR_PROVENANCE_SCRIPT_ASTEROID_DEATH,
    SR_PROVENANCE_SCRIPT_PLANNED_OUTPOST,
    SR_PROVENANCE_SCRIPT_STATION_JOSTLE,
    SR_PROVENANCE_SCRIPT_PLAYER_RAM,
    SR_PROVENANCE_SCRIPT_NPC_RAM,
    SR_PROVENANCE_SCRIPT_THROWN_ROCK_HIT,
    SR_PROVENANCE_SCRIPT_FRACTURE_CLAIM,
    SR_PROVENANCE_SCRIPT_WORKER_TOW_HNN,
    SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN,
    SR_PROVENANCE_SCRIPT_WORKER_DELIVERY_PROOF_HNN,
    SR_PROVENANCE_SCRIPT_WORKER_GOSSIP_COURIER,
    SR_PROVENANCE_SCRIPT_DENSE_ASTEROIDS,
} sr_provenance_script_t;

typedef enum {
    SR_EVAL_WORLD_NONE = 0,
    SR_EVAL_WORLD_SEEDED_ONLY,
    SR_EVAL_WORLD_SEEDED_SPARSE,
    SR_EVAL_WORLD_OUTPOST_LOW,
    SR_EVAL_WORLD_OUTPOST_MID,
    SR_EVAL_WORLD_OUTPOST_HIGH,
    SR_EVAL_WORLD_SCARCITY,
    SR_EVAL_WORLD_WEAK_SIGNAL,
    SR_EVAL_WORLD_ROUTE_DISRUPTED,
    SR_EVAL_WORLD_PERMUTATION_LOW,
    SR_EVAL_WORLD_PERMUTATION_HIGH,
} sr_eval_world_t;

typedef struct {
    int turn;
    int thrust;
    const char *name;
} sr_action_def_t;

typedef struct {
    int x;
    int y;
} sr_route_cell_t;

typedef struct {
    int npc_slot;
    sr_route_cell_t cell;
} sr_worker_route_visit_t;

typedef struct {
    uint32_t seed;
    int station;
    bool spawn_set;
    bool goal_set;
    bool velocity_set;
    bool angle_set;
    vec2 spawn;
    vec2 goal;
    vec2 hnn_query_goal;
    vec2 velocity;
    float angle;
    int horizon_ticks;
    int prefix[SR_MAX_PREFIX];
    int prefix_count;
    bool candidate_enabled[SR_ACTION_COUNT];
    bool hnn_trace;
    bool hnn_query_goal_set;
    int hnn_label_shift;
    hnn_confidence_mode_t hnn_confidence_mode;
    bool active_workers;
    int hnn_cleanup_steps;
    sr_provenance_script_t provenance_script;
    sr_eval_world_t eval_world;
    const char *out_path;
} sr_config_t;

typedef struct {
    int damage_events;
    int death_events;
    int dock_events;
    int launch_events;
    int pickup_events;
    int buy_events;
    int sell_events;
    int repair_events;
    int mining_tick_events;
    int fracture_events;
    int outpost_placed_events;
    int scaffold_ready_events;
    int contract_complete_events;
    int order_rejected_events;
    uint32_t first_contract_complete_tick;
    bool first_contract_complete_tick_set;
    uint32_t first_repair_tick;
    bool first_repair_tick_set;
    int pickup_fragments;
    float pickup_ore;
    float damage_amount;
    int buy_cost;
    int buy_quantity;
    int sell_base;
    int sell_bonus;
} sr_event_counts_t;

typedef struct {
    int active_ticks;
    int worker_selected_rows_peak;
    int worker_hologram_rows_peak;
    int worker_assignment_ticks;
    int worker_hologram_assignment_ticks;
    int worker_mine_assignment_ticks;
    int worker_haul_assignment_ticks;
    int worker_tow_assignment_ticks;
    int worker_delivery_assignment_ticks;
    int worker_scout_assignment_ticks;
    int worker_repair_assignment_ticks;
    int worker_motion_ticks;
    int worker_route_support_ticks;
    int worker_cargo_ticks;
    int worker_scaffold_motion_ticks;
    int worker_delivery_shipment_ticks;
    int worker_useful_outcome_ticks;
    int first_worker_completion_tick;
    int worker_stuck_ticks;
    int worker_recovery_events;
    int worker_loop_revisits;
    bool worker_stuck_latched;
} sr_ai_branch_summary_t;

typedef struct {
    bool enabled;
    int active_npcs;
    int worker_diag_rows;
    int worker_selected_rows;
    int worker_hologram_rows;
    int worker_mine_assignments;
    int worker_hologram_mine_assignments;
    int worker_haul_assignments;
    int worker_hologram_haul_assignments;
    int worker_tow_assignments;
    int worker_hologram_tow_assignments;
    int worker_delivery_assignments;
    int worker_hologram_delivery_assignments;
    int worker_scout_assignments;
    int worker_hologram_scout_assignments;
    int worker_repair_assignments;
    int worker_hologram_repair_assignments;
    int workers_travel_to_pickup;
    int workers_travel_to_dest;
    int workers_unloading;
    int workers_returning;
    int workers_towing_scaffold;
    int workers_with_finished_cargo;
    int scaffolds_loose;
    int scaffolds_towing;
    int scaffolds_towed_by_worker;
    int scaffolds_snapping;
    int scaffolds_placed;
    int npc_delivery_shipments_active;
    int npc_delivery_shipments_picked_up;
    int npc_delivery_shipments_delivered;
    int npc_delivery_shipments_cleared;
    int npc_delivery_shipments_defaulted;
    int npc_delivery_shipments_black_market_sold;
    int npc_known_contracts;
    int npc_knowledge_items;
    int station_known_contracts;
    int station_knowledge_items;
    int station_remote_known_contracts;
    int station_remote_market_memory_items;
    int npc_hnn_market_stored;
    int station_hnn_market_stored;
    int npc_hnn_flight_stored;
    int station_hnn_experience_stored;
    int station_hnn_market_versions;
    int station_hnn_experience_versions;
    int signal_field_occupied_slots;
    int signal_field_capacity_slots;
    int signal_field_noisy_station_cells;
    float worker_finished_cargo_units;
    float max_npc_market_load;
    float max_station_market_load;
    float max_npc_flight_load;
    float max_station_experience_load;
    float signal_field_load;
    float signal_field_max_strength;
    float signal_field_min_margin;
    float signal_field_min_snr;
    sr_ai_branch_summary_t branch;
} sr_ai_summary_t;

typedef struct {
    bool enabled;
    int top_action;
    int top_allowed_action;
    int candidate_rank;
    int candidate_allowed_rank;
    bool candidate_allowed;
    uint16_t allowed_mask;
    float candidate_score;
    float top_score;
    float top_allowed_score;
    float margin;
    float allowed_margin;
    float trace_fidelity;
    hnn_confidence_decision_t confidence;
    hnn_confidence_mode_t confidence_mode;
    int teacher_action;
    int selected_action;
    hnn_memory_contract_t contract;
    bool holonet_enabled;
    int holonet_active_count;
    int holonet_last_route;
    int holonet_scored_count;
    float holonet_route_similarity;
    hnn_memory_contract_t holonet_contract;
    float scores[HNN_ACTION_COUNT];
} sr_hnn_eval_t;

typedef struct {
    int existing_stations;
    int active_stations;
    int generated_outposts;
    int weak_signal_stations;
    int disconnected_stations;
    int scarce_finished_goods;
    int production_edges;
    int consumption_edges;
    int outpost_slots[SR_EVAL_MAX_OUTPOSTS];
    int outpost_slot_count;
    uint8_t topology_hash[32];
    uint8_t semantic_topology_hash[32];
} sr_eval_summary_t;

typedef struct {
    cargo_receipt_trust_status_t trusted_smelt;
    cargo_receipt_trust_status_t trusted_craft;
    cargo_receipt_trust_status_t trusted_rotated;
    cargo_receipt_trust_status_t bad_arguments;
    cargo_receipt_trust_status_t broken_chain;
    cargo_receipt_trust_status_t missing_origin;
    cargo_receipt_trust_status_t wrong_event_type;
    cargo_receipt_trust_status_t wrong_cargo;
    cargo_receipt_trust_status_t origin_metadata;
    cargo_receipt_trust_status_t wrong_origin_pin;
    cargo_receipt_trust_status_t wrong_origin_authority;
    cargo_receipt_trust_status_t wrong_origin_authority_lifecycle;
    cargo_receipt_trust_status_t unknown_authority;
    cargo_receipt_trust_status_t untrusted_authority;
    cargo_receipt_trust_status_t revoked_authority;
} sr_receipt_trust_eval_t;

typedef struct {
    int manifest_count;
    int receipt_count;
    int missing_receipt_chains;
    int invalid_receipt_chains;
    bool receipt_manifest_parity;
    uint8_t identity_hash[32];
} sr_lineage_summary_t;

typedef struct {
    bool ok;
    int candidate;
    int prefix_ticks;
    int horizon_ticks;
    int start_station;
    float start_dist;
    float end_dist;
    float progress;
    float start_hull;
    float end_hull;
    float hull_loss;
    float start_cargo;
    float end_cargo;
    float start_balance;
    float end_balance;
    double utility;
    vec2 start_pos;
    vec2 end_pos;
    vec2 end_vel;
    float end_speed;
    float end_angle;
    bool end_docked;
    int end_current_station;
    int ticks_executed;
    int goal_completion_tick;
    int contract_completion_tick;
    int repair_completion_tick;
    float route_start_dist;
    float travel_distance;
    float route_efficiency;
    int stuck_ticks;
    int recovery_events;
    int loop_revisits;
    int start_active_contracts;
    int end_active_contracts;
    uint64_t contract_decisions;
    uint64_t contract_teacher_decisions;
    uint64_t worker_decisions;
    uint64_t worker_teacher_decisions;
    sr_lineage_summary_t start_lineage;
    sr_lineage_summary_t end_lineage;
    uint16_t end_manifest_count;
    sr_event_counts_t events;
    sr_ai_summary_t ai;
    sr_hnn_eval_t hnn;
    sr_eval_summary_t evaluation;
    sr_receipt_trust_eval_t receipt_trust;
    uint8_t prefix_state_hash[32];
    uint8_t state_hash[32];
    uint8_t prefix_state_root[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t state_root[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t event_hash[32];
} sr_result_t;

_Static_assert((int)HNN_ACTION_COUNT == (int)SR_ACTION_COUNT,
               "signal_replay HNN actions must match replay actions");

static const sr_action_def_t SR_ACTIONS[SR_ACTION_COUNT] = {
    { 0,  0, "NONE"},
    { 0,  1, "W"},
    {-1,  0, "A"},
    { 1,  0, "D"},
    { 0, -1, "S"},
    {-1,  1, "WA"},
    { 1,  1, "WD"},
    {-1, -1, "SA"},
    { 1, -1, "SD"},
};

static bool sr_receipt_trust_known_vector(
    const world_t *w,
    int station_index,
    sr_receipt_trust_eval_t *out) {
    if (!w || !out || station_index < 0 || station_index >= MAX_STATIONS)
        return false;

    uint8_t cargo_pub[32];
    uint8_t recipient_pub[32];
    uint8_t origin_hash[32];
    for (int i = 0; i < 32; i++) {
        cargo_pub[i] = (uint8_t)(0x31 + i);
        recipient_pub[i] = (uint8_t)(0x61 + i);
        origin_hash[i] = (uint8_t)(0x91 + i);
    }

    const station_t *station = &w->stations[station_index];
    cargo_receipt_t receipt;
    if (!cargo_receipt_issue(station, 7u, 11u, cargo_pub, recipient_pub,
                             origin_hash, &receipt)) {
        return false;
    }

    cargo_receipt_origin_proof_t valid = {
        .event_type = CARGO_RECEIPT_ORIGIN_EVENT_SMELT,
        .authority_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        .event_id = 11u,
        .epoch = 7u,
        .output_semantics_version =
            CARGO_RECEIPT_ORIGIN_SEMANTICS_V1,
    };
    memcpy(valid.event_hash, origin_hash, sizeof(valid.event_hash));
    memcpy(valid.output_cargo_pub, cargo_pub, sizeof(valid.output_cargo_pub));
    memcpy(valid.output_cargo.pub, cargo_pub,
           sizeof(valid.output_cargo.pub));
    memcpy(valid.authority, station->station_pubkey, sizeof(valid.authority));

    memset(out, 0, sizeof(*out));
    out->trusted_smelt = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &valid,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;

    cargo_receipt_origin_proof_t variant = valid;
    variant.event_type = CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
    out->trusted_craft = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    variant = valid;
    variant.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED;
    out->trusted_rotated = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED).status;
    out->bad_arguments = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &valid,
        (cargo_receipt_authority_trust_t)99).status;

    cargo_receipt_t tampered = receipt;
    tampered.signature[0] ^= 0x01u;
    out->broken_chain = cargo_receipt_trust_verify(
        &tampered, 1, cargo_pub, &valid,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    out->missing_origin = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, NULL,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;

    variant = valid;
    variant.event_type = CARGO_RECEIPT_ORIGIN_EVENT_NONE;
    out->wrong_event_type = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    variant = valid;
    variant.output_cargo_pub[0] ^= 0x80u;
    out->wrong_cargo = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    variant = valid;
    variant.output_semantics_version =
        CARGO_RECEIPT_ORIGIN_SEMANTICS_UNBOUND;
    out->origin_metadata = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    variant = valid;
    variant.event_hash[1] ^= 0x40u;
    out->wrong_origin_pin = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    variant = valid;
    variant.authority[2] ^= 0x20u;
    out->wrong_origin_authority = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    variant = valid;
    variant.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED;
    out->wrong_origin_authority_lifecycle = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &variant,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT).status;
    out->unknown_authority = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &valid,
        CARGO_RECEIPT_AUTHORITY_UNKNOWN).status;
    out->untrusted_authority = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &valid,
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED).status;
    out->revoked_authority = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pub, &valid,
        CARGO_RECEIPT_AUTHORITY_REVOKED).status;

    return out->trusted_smelt == CARGO_RECEIPT_TRUST_VALID_TRUSTED &&
           out->trusted_craft == CARGO_RECEIPT_TRUST_VALID_TRUSTED &&
           out->trusted_rotated ==
               CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED &&
           out->bad_arguments ==
               CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS &&
           out->broken_chain == CARGO_RECEIPT_TRUST_REJECT_CHAIN &&
           out->missing_origin ==
               CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN &&
           out->wrong_event_type ==
               CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE &&
           out->wrong_cargo == CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO &&
           out->origin_metadata ==
               CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA &&
           out->wrong_origin_pin == CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN &&
           out->wrong_origin_authority ==
               CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY &&
           out->wrong_origin_authority_lifecycle ==
               CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY_LIFECYCLE &&
           out->unknown_authority ==
               CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY &&
           out->untrusted_authority ==
               CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY &&
           out->revoked_authority ==
               CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY;
}

static void sr_usage(FILE *fp)
{
    fprintf(fp,
            "usage: signal_replay [options]\n"
            "\n"
            "Options:\n"
            "  --seed N             deterministic world seed (default 2037)\n"
            "  --station N          station index for default spawn/goal (default 0)\n"
            "  --spawn X,Y          branch start position; default near station\n"
            "  --velocity X,Y       branch start velocity; default 0,0\n"
            "  --angle R            branch start angle in radians; default points at goal\n"
            "  --goal X,Y           replay utility target; default beyond station\n"
            "  --history LIST       comma-separated prefix actions, e.g. W,W,WA,D\n"
            "  --horizon-ticks N    branch horizon per candidate (default 36; max 120000)\n"
            "  --candidates LIST    comma-separated candidate actions; default all 9\n"
            "  --hnn-trace          train an HNN trace from the prefix and score each branch candidate\n"
            "  --hnn-query-goal X,Y query the trace with an unrelated goal (offline null control)\n"
            "  --hnn-label-shift N  rotate stored action labels by N (offline null control; 0..8)\n"
            "  --hnn-confidence-mode MODE  shadow (default) or mixed\n"
            "  --active-workers     keep seeded NPC workers active and include AI/gossip/HNN metrics\n"
            "  --hnn-cleanup-steps N cleanup steps for HNN retrieval (default 3; 0..8)\n"
            "  --provenance-script NAME  run a deterministic setup/action script\n"
            "                       before each branch; names: none,buy-sell,pod-tow-sell,mine-fracture,asteroid-death,planned-outpost,station-jostle,player-ram,npc-ram,thrown-rock-hit,fracture-claim,worker-tow-hnn,worker-repair-hnn,worker-delivery-proof-hnn,worker-gossip-courier,dense-asteroids\n"
            "  --evaluation-world NAME  deterministic AI topology fixture; names:\n"
            "                       none,seeded-only,seeded-sparse,outpost-low,outpost-mid,\n"
            "                       outpost-high,scarcity,weak-signal,route-disrupted,\n"
            "                       permutation-low,permutation-high\n"
            "  --out PATH           write JSONL to PATH instead of stdout\n"
            "  --self-test-public-hash  verify public hashes exclude bearer-only state\n"
            "  --help               show this help\n"
            "\n"
            "Actions: NONE,W,A,D,S,WA,WD,SA,SD or numeric ids 0..8.\n");
}

static bool sr_parse_i32(const char *text, int min_value, int max_value, int *out)
{
    char *end = NULL;
    long value;
    if (!text || !out || text[0] == '\0') return false;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < min_value || value > max_value) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool sr_parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;
    if (!text || !out || text[0] == '\0') return false;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool sr_parse_float(const char *text, float *out)
{
    char *end = NULL;
    float value;
    if (!text || !out || text[0] == '\0') return false;
    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

static bool sr_parse_vec2(const char *text, vec2 *out)
{
    char left[64];
    char right[64];
    const char *comma;
    size_t n;
    if (!text || !out) return false;
    comma = strchr(text, ',');
    if (!comma) return false;
    n = (size_t)(comma - text);
    if (n == 0 || n >= sizeof(left) || strlen(comma + 1) >= sizeof(right)) {
        return false;
    }
    memcpy(left, text, n);
    left[n] = '\0';
    snprintf(right, sizeof(right), "%s", comma + 1);
    return sr_parse_float(left, &out->x) && sr_parse_float(right, &out->y);
}

static bool sr_parse_action(const char *text, int *out)
{
    int id = -1;
    if (!text || !out || text[0] == '\0') return false;
    if (sr_parse_i32(text, 0, SR_ACTION_COUNT - 1, &id)) {
        *out = id;
        return true;
    }
    for (int i = 0; i < SR_ACTION_COUNT; i++) {
        if (strcmp(text, SR_ACTIONS[i].name) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

static bool sr_parse_action_list(const char *text,
                                 int *actions,
                                 int *count,
                                 int max_count)
{
    char buf[8192];
    char *cursor;
    if (!text || !actions || !count || max_count <= 0) return false;
    if (strlen(text) >= sizeof(buf)) return false;
    snprintf(buf, sizeof(buf), "%s", text);
    *count = 0;
    cursor = buf;
    while (cursor && *cursor) {
        char *comma = strchr(cursor, ',');
        int action = -1;
        if (comma) *comma = '\0';
        if (!sr_parse_action(cursor, &action)) return false;
        if (*count >= max_count) return false;
        actions[(*count)++] = action;
        cursor = comma ? comma + 1 : NULL;
    }
    return true;
}

static bool sr_parse_candidate_list(const char *text, bool enabled[SR_ACTION_COUNT])
{
    int actions[SR_ACTION_COUNT];
    int count = 0;
    for (int i = 0; i < SR_ACTION_COUNT; i++) enabled[i] = false;
    if (!sr_parse_action_list(text, actions, &count, SR_ACTION_COUNT)) return false;
    if (count <= 0) return false;
    for (int i = 0; i < count; i++) enabled[actions[i]] = true;
    return true;
}

static bool sr_parse_provenance_script(const char *text,
                                       sr_provenance_script_t *out)
{
    if (!text || !out) return false;
    if (strcmp(text, "none") == 0) {
        *out = SR_PROVENANCE_SCRIPT_NONE;
        return true;
    }
    if (strcmp(text, "buy-sell") == 0) {
        *out = SR_PROVENANCE_SCRIPT_BUY_SELL;
        return true;
    }
    if (strcmp(text, "pod-tow-sell") == 0) {
        *out = SR_PROVENANCE_SCRIPT_POD_TOW_SELL;
        return true;
    }
    if (strcmp(text, "mine-fracture") == 0) {
        *out = SR_PROVENANCE_SCRIPT_MINE_FRACTURE;
        return true;
    }
    if (strcmp(text, "asteroid-death") == 0) {
        *out = SR_PROVENANCE_SCRIPT_ASTEROID_DEATH;
        return true;
    }
    if (strcmp(text, "planned-outpost") == 0) {
        *out = SR_PROVENANCE_SCRIPT_PLANNED_OUTPOST;
        return true;
    }
    if (strcmp(text, "station-jostle") == 0) {
        *out = SR_PROVENANCE_SCRIPT_STATION_JOSTLE;
        return true;
    }
    if (strcmp(text, "player-ram") == 0) {
        *out = SR_PROVENANCE_SCRIPT_PLAYER_RAM;
        return true;
    }
    if (strcmp(text, "npc-ram") == 0) {
        *out = SR_PROVENANCE_SCRIPT_NPC_RAM;
        return true;
    }
    if (strcmp(text, "thrown-rock-hit") == 0) {
        *out = SR_PROVENANCE_SCRIPT_THROWN_ROCK_HIT;
        return true;
    }
    if (strcmp(text, "fracture-claim") == 0) {
        *out = SR_PROVENANCE_SCRIPT_FRACTURE_CLAIM;
        return true;
    }
    if (strcmp(text, "worker-tow-hnn") == 0) {
        *out = SR_PROVENANCE_SCRIPT_WORKER_TOW_HNN;
        return true;
    }
    if (strcmp(text, "worker-repair-hnn") == 0) {
        *out = SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN;
        return true;
    }
    if (strcmp(text, "worker-delivery-proof-hnn") == 0) {
        *out = SR_PROVENANCE_SCRIPT_WORKER_DELIVERY_PROOF_HNN;
        return true;
    }
    if (strcmp(text, "worker-gossip-courier") == 0) {
        *out = SR_PROVENANCE_SCRIPT_WORKER_GOSSIP_COURIER;
        return true;
    }
    if (strcmp(text, "dense-asteroids") == 0) {
        *out = SR_PROVENANCE_SCRIPT_DENSE_ASTEROIDS;
        return true;
    }
    return false;
}

static const char *sr_provenance_script_name(sr_provenance_script_t script)
{
    switch (script) {
    case SR_PROVENANCE_SCRIPT_DENSE_ASTEROIDS:
        return "dense-asteroids";
    case SR_PROVENANCE_SCRIPT_WORKER_GOSSIP_COURIER:
        return "worker-gossip-courier";
    case SR_PROVENANCE_SCRIPT_WORKER_DELIVERY_PROOF_HNN:
        return "worker-delivery-proof-hnn";
    case SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN:
        return "worker-repair-hnn";
    case SR_PROVENANCE_SCRIPT_WORKER_TOW_HNN:
        return "worker-tow-hnn";
    case SR_PROVENANCE_SCRIPT_FRACTURE_CLAIM:
        return "fracture-claim";
    case SR_PROVENANCE_SCRIPT_THROWN_ROCK_HIT:
        return "thrown-rock-hit";
    case SR_PROVENANCE_SCRIPT_NPC_RAM:
        return "npc-ram";
    case SR_PROVENANCE_SCRIPT_PLAYER_RAM:
        return "player-ram";
    case SR_PROVENANCE_SCRIPT_STATION_JOSTLE:
        return "station-jostle";
    case SR_PROVENANCE_SCRIPT_PLANNED_OUTPOST:
        return "planned-outpost";
    case SR_PROVENANCE_SCRIPT_ASTEROID_DEATH:
        return "asteroid-death";
    case SR_PROVENANCE_SCRIPT_MINE_FRACTURE:
        return "mine-fracture";
    case SR_PROVENANCE_SCRIPT_POD_TOW_SELL:
        return "pod-tow-sell";
    case SR_PROVENANCE_SCRIPT_BUY_SELL:
        return "buy-sell";
    case SR_PROVENANCE_SCRIPT_NONE:
    default:
        return "none";
    }
}

static bool sr_parse_eval_world(const char *text, sr_eval_world_t *out)
{
    static const struct {
        const char *name;
        sr_eval_world_t world;
    } definitions[] = {
        {"none", SR_EVAL_WORLD_NONE},
        {"seeded-only", SR_EVAL_WORLD_SEEDED_ONLY},
        {"seeded-sparse", SR_EVAL_WORLD_SEEDED_SPARSE},
        {"outpost-low", SR_EVAL_WORLD_OUTPOST_LOW},
        {"outpost-mid", SR_EVAL_WORLD_OUTPOST_MID},
        {"outpost-high", SR_EVAL_WORLD_OUTPOST_HIGH},
        {"scarcity", SR_EVAL_WORLD_SCARCITY},
        {"weak-signal", SR_EVAL_WORLD_WEAK_SIGNAL},
        {"route-disrupted", SR_EVAL_WORLD_ROUTE_DISRUPTED},
        {"permutation-low", SR_EVAL_WORLD_PERMUTATION_LOW},
        {"permutation-high", SR_EVAL_WORLD_PERMUTATION_HIGH},
    };
    if (!text || !out) return false;
    for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); i++) {
        if (strcmp(text, definitions[i].name) == 0) {
            *out = definitions[i].world;
            return true;
        }
    }
    return false;
}

static const char *sr_eval_world_name(sr_eval_world_t world)
{
    switch (world) {
    case SR_EVAL_WORLD_SEEDED_ONLY: return "seeded-only";
    case SR_EVAL_WORLD_SEEDED_SPARSE: return "seeded-sparse";
    case SR_EVAL_WORLD_OUTPOST_LOW: return "outpost-low";
    case SR_EVAL_WORLD_OUTPOST_MID: return "outpost-mid";
    case SR_EVAL_WORLD_OUTPOST_HIGH: return "outpost-high";
    case SR_EVAL_WORLD_SCARCITY: return "scarcity";
    case SR_EVAL_WORLD_WEAK_SIGNAL: return "weak-signal";
    case SR_EVAL_WORLD_ROUTE_DISRUPTED: return "route-disrupted";
    case SR_EVAL_WORLD_PERMUTATION_LOW: return "permutation-low";
    case SR_EVAL_WORLD_PERMUTATION_HIGH: return "permutation-high";
    case SR_EVAL_WORLD_NONE:
    default:
        return "none";
    }
}

static bool sr_parse_args(int argc, char **argv, sr_config_t *config)
{
    if (!config) return false;
    memset(config, 0, sizeof(*config));
    config->seed = 2037u;
    config->station = 0;
    config->horizon_ticks = 36;
    config->hnn_cleanup_steps = 3;
    for (int i = 0; i < SR_ACTION_COUNT; i++) config->candidate_enabled[i] = true;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            sr_usage(stdout);
            exit(0);
        } else if (strcmp(arg, "--seed") == 0 && value) {
            if (!sr_parse_u32(value, &config->seed)) return false;
            i++;
        } else if (strcmp(arg, "--station") == 0 && value) {
            if (!sr_parse_i32(value, 0, MAX_STATIONS - 1, &config->station)) return false;
            i++;
        } else if (strcmp(arg, "--spawn") == 0 && value) {
            if (!sr_parse_vec2(value, &config->spawn)) return false;
            config->spawn_set = true;
            i++;
        } else if (strcmp(arg, "--velocity") == 0 && value) {
            if (!sr_parse_vec2(value, &config->velocity)) return false;
            config->velocity_set = true;
            i++;
        } else if (strcmp(arg, "--angle") == 0 && value) {
            if (!sr_parse_float(value, &config->angle)) return false;
            config->angle_set = true;
            i++;
        } else if (strcmp(arg, "--goal") == 0 && value) {
            if (!sr_parse_vec2(value, &config->goal)) return false;
            config->goal_set = true;
            i++;
        } else if (strcmp(arg, "--history") == 0 && value) {
            if (!sr_parse_action_list(value, config->prefix,
                                      &config->prefix_count,
                                      SR_MAX_PREFIX)) {
                return false;
            }
            i++;
        } else if (strcmp(arg, "--horizon-ticks") == 0 && value) {
            if (!sr_parse_i32(value, 1, SR_MAX_HORIZON_TICKS,
                              &config->horizon_ticks)) return false;
            i++;
        } else if (strcmp(arg, "--candidates") == 0 && value) {
            if (!sr_parse_candidate_list(value, config->candidate_enabled)) return false;
            i++;
        } else if (strcmp(arg, "--hnn-trace") == 0) {
            config->hnn_trace = true;
        } else if (strcmp(arg, "--hnn-query-goal") == 0 && value) {
            if (!sr_parse_vec2(value, &config->hnn_query_goal)) return false;
            config->hnn_query_goal_set = true;
            i++;
        } else if (strcmp(arg, "--hnn-label-shift") == 0 && value) {
            if (!sr_parse_i32(value, 0, HNN_ACTION_COUNT - 1,
                              &config->hnn_label_shift)) return false;
            i++;
        } else if (strcmp(arg, "--hnn-confidence-mode") == 0 && value) {
            if (strcmp(value, "shadow") != 0 && strcmp(value, "mixed") != 0)
                return false;
            config->hnn_confidence_mode =
                hnn_confidence_mode_from_string(value);
            i++;
        } else if (strcmp(arg, "--active-workers") == 0) {
            config->active_workers = true;
        } else if (strcmp(arg, "--hnn-cleanup-steps") == 0 && value) {
            if (!sr_parse_i32(value, 0, 8, &config->hnn_cleanup_steps)) return false;
            i++;
        } else if (strcmp(arg, "--provenance-script") == 0 && value) {
            if (!sr_parse_provenance_script(value, &config->provenance_script)) {
                return false;
            }
            i++;
        } else if (strcmp(arg, "--evaluation-world") == 0 && value) {
            if (!sr_parse_eval_world(value, &config->eval_world)) return false;
            i++;
        } else if (strcmp(arg, "--out") == 0 && value) {
            config->out_path = value;
            i++;
        } else {
            fprintf(stderr, "signal_replay: unknown or incomplete option '%s'\n", arg);
            return false;
        }
    }
    return true;
}

static void sr_hex(const uint8_t bytes[32], char out[65])
{
    static const char hexdigits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = hexdigits[bytes[i] >> 4];
        out[i * 2 + 1] = hexdigits[bytes[i] & 15u];
    }
    out[64] = '\0';
}

static void sr_hash_u8(sha256_ctx_t *ctx, uint8_t v)
{
    sha256_update(ctx, &v, sizeof(v));
}

static void sr_hash_u16(sha256_ctx_t *ctx, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    sha256_update(ctx, b, sizeof(b));
}

static void sr_hash_u32(sha256_ctx_t *ctx, uint32_t v)
{
    uint8_t b[4] = {
        (uint8_t)v, (uint8_t)(v >> 8),
        (uint8_t)(v >> 16), (uint8_t)(v >> 24)
    };
    sha256_update(ctx, b, sizeof(b));
}

static void sr_hash_i32(sha256_ctx_t *ctx, int32_t v)
{
    sr_hash_u32(ctx, (uint32_t)v);
}

static void sr_hash_u64(sha256_ctx_t *ctx, uint64_t v)
{
    uint8_t b[8] = {
        (uint8_t)v, (uint8_t)(v >> 8),
        (uint8_t)(v >> 16), (uint8_t)(v >> 24),
        (uint8_t)(v >> 32), (uint8_t)(v >> 40),
        (uint8_t)(v >> 48), (uint8_t)(v >> 56)
    };
    sha256_update(ctx, b, sizeof(b));
}

static void sr_hash_float_bits(sha256_ctx_t *ctx, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    sr_hash_u32(ctx, bits);
}

static int sr_eval_outpost_slot(sr_eval_world_t world)
{
    switch (world) {
    case SR_EVAL_WORLD_OUTPOST_LOW:
    case SR_EVAL_WORLD_SCARCITY:
    case SR_EVAL_WORLD_WEAK_SIGNAL:
    case SR_EVAL_WORLD_ROUTE_DISRUPTED:
    case SR_EVAL_WORLD_PERMUTATION_LOW:
        return SIGNAL_FIRST_OUTPOST_INDEX;
    case SR_EVAL_WORLD_OUTPOST_MID:
        return MAX_STATIONS / 2;
    case SR_EVAL_WORLD_OUTPOST_HIGH:
    case SR_EVAL_WORLD_PERMUTATION_HIGH:
        return MAX_STATIONS - 1;
    case SR_EVAL_WORLD_NONE:
    case SR_EVAL_WORLD_SEEDED_ONLY:
    case SR_EVAL_WORLD_SEEDED_SPARSE:
    default:
        return -1;
    }
}

static bool sr_build_eval_outpost(world_t *w,
                                  int station_idx,
                                  vec2 pos,
                                  float signal_range)
{
    static const uint8_t founder[32] = {
        0x65, 0x76, 0x61, 0x6c, 0x2d, 0x6f, 0x75, 0x74,
        0x70, 0x6f, 0x73, 0x74, 0x2d, 0x76, 0x31, 0x00,
    };
    static const uint8_t stock_origin[8] = {
        'A', 'I', 'E', 'V', 'A', 'L', '0', '1',
    };
    if (!w || station_idx < SIGNAL_FIRST_OUTPOST_INDEX ||
        station_idx >= MAX_STATIONS) {
        return false;
    }

    station_t *st = &w->stations[station_idx];
    station_cleanup(st);
    memset(st, 0, sizeof(*st));
    if (!station_manifest_bootstrap(st)) return false;

    st->id = w->next_station_id++;
    snprintf(st->name, sizeof(st->name), "%s", "Eval Relay");
    snprintf(st->station_slug, sizeof(st->station_slug), "%s", "eval-relay");
    snprintf(st->currency_name, sizeof(st->currency_name), "%s", "eval marks");
    st->pos = pos;
    st->radius = 34.0f;
    st->dock_radius = 220.0f;
    st->signal_range = signal_range;
    st->base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    st->base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    st->base_price[COMMODITY_FRAME] = 2.0f;
    st->base_price[COMMODITY_LASER_MODULE] = 16.0f;
    st->base_price[COMMODITY_TRACTOR_MODULE] = 18.0f;
    st->base_price[COMMODITY_REPAIR_KIT] = 1.0f;
    st->_inventory_cache[COMMODITY_FERRITE_ORE] = 4.0f;

    add_module_at(st, MODULE_DOCK, 1, 0);
    add_module_at(st, MODULE_SIGNAL_RELAY, 1, 1);
    add_furnace_for(st, 1, 2, COMMODITY_FERRITE_INGOT);
    add_module_at(st, MODULE_FRAME_PRESS, 2, 0);
    add_hopper_for(st, 2, 4, COMMODITY_FERRITE_ORE);
    add_hopper_for(st, 3, 0, COMMODITY_FERRITE_INGOT);
    st->arm_count = 3;
    st->arm_speed[1] = STATION_RING_SPEED;
    st->arm_omega[1] = STATION_RING_SPEED;
    rebuild_station_services(st);

    station_authority_init_outpost(st, founder, UINT64_C(649001));
    chain_log_health_set(st, CHAIN_HEALTH_FRESH, false, 0, NULL,
                         "AI evaluation outpost; no chain events");
    if (station_finished_mint(
            st, COMMODITY_FERRITE_INGOT, 4, stock_origin) != 4 ||
        station_finished_mint(st, COMMODITY_FRAME, 2, stock_origin) != 2 ||
        station_finished_mint(
            st, COMMODITY_REPAIR_KIT, 1, stock_origin) != 1) {
        return false;
    }

    if (station_idx >= w->station_count) w->station_count = station_idx + 1;
    return true;
}

static void sr_make_eval_world_scarce(world_t *w)
{
    if (!w) return;
    for (int station_idx = 0; station_idx < MAX_STATIONS; station_idx++) {
        station_t *st = &w->stations[station_idx];
        if (!station_exists(st)) continue;
        for (int commodity = 0; commodity < COMMODITY_RAW_ORE_COUNT;
             commodity++) {
            st->_inventory_cache[commodity] = 0.0f;
        }
        for (int commodity = COMMODITY_RAW_ORE_COUNT;
             commodity < COMMODITY_COUNT; commodity++) {
            int count =
                station_finished_count(st, (commodity_t)commodity);
            if (count > 0) {
                (void)station_finished_drain(
                    st, (commodity_t)commodity, count);
            }
        }
    }
}

static bool sr_apply_eval_world(const sr_config_t *config, world_t *w)
{
    if (!config || !w) return false;
    if (config->eval_world == SR_EVAL_WORLD_NONE ||
        config->eval_world == SR_EVAL_WORLD_SEEDED_ONLY) {
        return true;
    }

    if (config->eval_world == SR_EVAL_WORLD_SEEDED_SPARSE) {
        w->stations[2].signal_range = 0.0f;
        w->stations[2].signal_connected = false;
        rebuild_signal_chain(w);
        station_rebuild_all_nav(w);
        gossip_bootstrap_world_stations(w);
        return true;
    }

    int station_idx = sr_eval_outpost_slot(config->eval_world);
    vec2 pos = v2_add(w->stations[0].pos, v2(6000.0f, 0.0f));
    float signal_range = 2600.0f;
    if (config->eval_world == SR_EVAL_WORLD_WEAK_SIGNAL) {
        pos = v2_add(w->stations[0].pos, v2(8700.0f, 0.0f));
        signal_range = 600.0f;
    } else if (config->eval_world == SR_EVAL_WORLD_ROUTE_DISRUPTED) {
        pos = v2_add(w->stations[0].pos, v2(18000.0f, 0.0f));
        signal_range = 700.0f;
    }
    if (!sr_build_eval_outpost(w, station_idx, pos, signal_range)) {
        return false;
    }
    if (config->eval_world == SR_EVAL_WORLD_SCARCITY) {
        sr_make_eval_world_scarce(w);
    }
    rebuild_signal_chain(w);
    station_rebuild_all_nav(w);
    gossip_bootstrap_world_stations(w);
    return true;
}

static void sr_hash_eval_station(const station_t *st, uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, "signal-ai-eval-station-v1",
                  sizeof("signal-ai-eval-station-v1") - 1u);
    sr_hash_u32(&ctx, st->id);
    sha256_update(&ctx, st->station_pubkey, sizeof(st->station_pubkey));
    sr_hash_float_bits(&ctx, st->pos.x);
    sr_hash_float_bits(&ctx, st->pos.y);
    sr_hash_float_bits(&ctx, st->radius);
    sr_hash_float_bits(&ctx, st->dock_radius);
    sr_hash_float_bits(&ctx, st->signal_range);
    sr_hash_u8(&ctx, st->signal_connected ? 1u : 0u);
    sr_hash_u8(&ctx, st->scaffold ? 1u : 0u);
    sr_hash_u8(&ctx, st->planned ? 1u : 0u);
    sr_hash_i32(&ctx, st->module_count);
    for (int module_idx = 0;
         module_idx < st->module_count &&
         module_idx < MAX_MODULES_PER_STATION;
         module_idx++) {
        const station_module_t *module = &st->modules[module_idx];
        sr_hash_i32(&ctx, (int32_t)module->type);
        sr_hash_u8(&ctx, module->ring);
        sr_hash_u8(&ctx, module->slot);
        sr_hash_u8(&ctx, module->commodity);
        sr_hash_u8(&ctx, module->scaffold ? 1u : 0u);
    }
    for (int commodity = 0; commodity < COMMODITY_COUNT; commodity++) {
        sr_hash_float_bits(
            &ctx,
            station_inventory_amount(st, (commodity_t)commodity));
    }
    sha256_final(&ctx, out);
}

static int sr_compare_hashes(const void *left, const void *right)
{
    return memcmp(left, right, 32);
}

static void sr_collect_eval_summary(const world_t *w,
                                    sr_eval_summary_t *summary)
{
    static const commodity_t scarcity_goods[] = {
        COMMODITY_FRAME,
        COMMODITY_LASER_MODULE,
        COMMODITY_TRACTOR_MODULE,
        COMMODITY_REPAIR_KIT,
    };
    uint8_t station_hashes[MAX_STATIONS][32];
    int station_hash_count = 0;
    sha256_ctx_t topology;
    sha256_ctx_t semantic;

    memset(summary, 0, sizeof(*summary));
    sha256_init(&topology);
    sha256_update(&topology, "signal-ai-eval-topology-v1",
                  sizeof("signal-ai-eval-topology-v1") - 1u);
    for (int station_idx = 0; station_idx < MAX_STATIONS; station_idx++) {
        const station_t *st = &w->stations[station_idx];
        if (!station_exists(st)) continue;

        uint8_t station_hash[32];
        sr_hash_eval_station(st, station_hash);
        sr_hash_i32(&topology, station_idx);
        sha256_update(&topology, station_hash, sizeof(station_hash));
        memcpy(station_hashes[station_hash_count++],
               station_hash, sizeof(station_hash));

        summary->existing_stations++;
        if (station_is_active(st)) {
            summary->active_stations++;
            if (st->signal_range < 1000.0f)
                summary->weak_signal_stations++;
            if (!st->signal_connected)
                summary->disconnected_stations++;
        }
        if (station_idx >= SIGNAL_FIRST_OUTPOST_INDEX &&
            station_is_active(st)) {
            summary->generated_outposts++;
            if (summary->outpost_slot_count < SR_EVAL_MAX_OUTPOSTS) {
                summary->outpost_slots[summary->outpost_slot_count++] =
                    station_idx;
            }
        }
        for (int commodity = 0; commodity < COMMODITY_COUNT; commodity++) {
            if (station_produces(st, (commodity_t)commodity))
                summary->production_edges++;
            if (station_consumes(st, (commodity_t)commodity))
                summary->consumption_edges++;
        }
    }
    sha256_final(&topology, summary->topology_hash);

    qsort(station_hashes, (size_t)station_hash_count,
          sizeof(station_hashes[0]), sr_compare_hashes);
    sha256_init(&semantic);
    sha256_update(&semantic, "signal-ai-eval-semantic-v1",
                  sizeof("signal-ai-eval-semantic-v1") - 1u);
    sr_hash_i32(&semantic, station_hash_count);
    for (int i = 0; i < station_hash_count; i++)
        sha256_update(&semantic, station_hashes[i], sizeof(station_hashes[i]));
    sha256_final(&semantic, summary->semantic_topology_hash);

    for (size_t i = 0;
         i < sizeof(scarcity_goods) / sizeof(scarcity_goods[0]); i++) {
        int total = 0;
        for (int station_idx = 0; station_idx < MAX_STATIONS; station_idx++) {
            const station_t *st = &w->stations[station_idx];
            if (station_exists(st))
                total += station_finished_count(st, scarcity_goods[i]);
        }
        if (total <= 1) summary->scarce_finished_goods++;
    }
}

static bool sr_reverse_allowed(const server_player_t *sp)
{
    vec2 forward = ship_forward(sp->ship->angle);
    return v2_dot(sp->ship->vel, forward) <= 2.0f;
}

static void sr_apply_action(server_player_t *sp, int action)
{
    const sr_action_def_t *def = &SR_ACTIONS[action];
    memset(&sp->input, 0, sizeof(sp->input));
    sp->input.turn = (float)def->turn;
    sp->input.thrust = (float)def->thrust;
    sp->input.reverse_thrust = def->thrust < 0 && sr_reverse_allowed(sp);
}

static void sr_reset_player(world_t *w, server_player_t *sp)
{
    world_tow_links_clear_source(w, sp->ship_ref);
    ship_cleanup(sp->ship);
    memset(sp, 0, sizeof(*sp));
    player_init_ship(sp, w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->docked = false;
    sp->in_dock_range = false;
    sp->nearby_station = -1;
    sp->current_station = 0;
    sp->autopilot_mode = 0;
    sp->was_in_signal = true;
    sp->boost_hold_timer = 0.0f;
    memset(sp->session_token, 0x51, sizeof(sp->session_token));
    memset(sp->pubkey, 0xA7, sizeof(sp->pubkey));
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    memset(&sp->input, 0, sizeof(sp->input));
}

static int sr_first_station_module(const station_t *st, module_type_t type)
{
    if (!st) return -1;
    for (int i = 0; i < st->module_count && i < MAX_MODULES_PER_STATION; i++) {
        const station_module_t *module = &st->modules[i];
        if (!module->scaffold && module->type == type)
            return i;
    }
    return -1;
}

static int sr_spawn_station_market_pod(world_t *w,
                                       int station_idx,
                                       commodity_t commodity,
                                       uint16_t count,
                                       const uint8_t origin[8])
{
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        commodity >= COMMODITY_COUNT || count == 0 ||
        count > CARGO_POD_MANIFEST_CAP) {
        return -1;
    }

    station_t *st = &w->stations[station_idx];
    int dock_idx = sr_first_station_module(st, MODULE_DOCK);
    if (dock_idx < 0) return -1;

    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    for (uint16_t i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, commodity, i, &units[i]))
            return -1;
    }

    vec2 pos = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    int pod_idx = spawn_cargo_pod_with_manifest_deterministic(
        w, pos, v2(0.0f, 0.0f), commodity, units, count,
        CARGO_POD_CARGO, 0.0f, 0.0f);
    if (pod_idx < 0) return -1;
    if (!world_cargo_pod_set_module_tractor(
            w, pod_idx, station_idx, dock_idx)) {
        memset(&w->cargo_pods[pod_idx], 0, sizeof(w->cargo_pods[pod_idx]));
        return -1;
    }
    return pod_idx;
}

static void sr_move_pod_past_station_charge_boundary(world_t *w,
                                                     int station_idx,
                                                     int pod_idx)
{
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return;
    }
    station_t *st = &w->stations[station_idx];
    vec2 base = st->pos;
    int dock_idx = sr_first_station_module(st, MODULE_DOCK);
    if (dock_idx >= 0) {
        base = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    }
    w->cargo_pods[pod_idx].pos =
        v2_add(base, v2(CARGO_POD_DOCK_TRACTOR_RANGE +
                        HOPPER_INTAKE_STAGING_RANGE + 80.0f, 0.0f));
    w->cargo_pods[pod_idx].vel = v2(0.0f, 0.0f);
}

static bool sr_setup_world(const sr_config_t *config,
                           world_t *w,
                           server_player_t **out_sp,
                           vec2 *out_spawn,
                           vec2 *out_goal)
{
    server_player_t *sp;
    int station_index;
    station_t *station;

    if (!config || !w) return false;
    memset(w, 0, sizeof(*w));
    w->rng = config->seed;
    world_reset(w);
    if (!sr_apply_eval_world(config, w)) return false;
    if (!config->active_workers) {
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            w->npc_ships[i].active = false;
        }
    }

    station_index = config->station;
    if (station_index < 0 || station_index >= w->station_count) {
        station_index = 0;
    }
    station = &w->stations[station_index];

    sp = &w->players[0];
    sr_reset_player(w, sp);
    sp->current_station = station_index;
    sp->ship->pos = config->spawn_set
                 ? config->spawn
                 : v2_add(station->pos, v2(1600.0f, 200.0f));
    sp->ship->vel = config->velocity_set ? config->velocity : v2(0.0f, 0.0f);
    if (config->goal_set) {
        *out_goal = config->goal;
    } else {
        *out_goal = v2_add(station->pos, v2(2600.0f, -100.0f));
    }
    sp->ship->angle = config->angle_set
                   ? config->angle
                   : fixp_atan2f(out_goal->y - sp->ship->pos.y,
                                  out_goal->x - sp->ship->pos.x);
    sp->ship->hull = ship_max_hull(sp->ship);

    if (out_spawn) *out_spawn = sp->ship->pos;
    if (out_sp) *out_sp = sp;
    return true;
}

static void sr_reset_worker_fixture_state(world_t *w)
{
    if (!w) return;
    memset(w->contracts, 0, sizeof(w->contracts));
    signal_field_init(&w->signal_field);
    w->signal_field_decay_tick = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(&w->stations[s].knowledge, 0,
               sizeof(w->stations[s].knowledge));
        hnn_memory_init(&w->stations[s].hnn_market_memory);
        w->stations[s].hnn_market_version = 0;
        w->stations[s].hnn_market_decay_tick = 0;
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        world_npc_ship_slot_release(w, i);
        memset(&w->npc_ships[i], 0, sizeof(w->npc_ships[i]));
    }
    for (int i = 0; i < MAX_PLAYERS + MAX_NPC_SHIPS; i++)
        memset(&w->characters[i], 0, sizeof(w->characters[i]));
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_cleanup(&w->ship_assets[i].stored_ship);
        memset(&w->ship_assets[i], 0, sizeof(w->ship_assets[i]));
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        w->scaffolds[i].active = false;
    memset(w->delivery_shipments, 0, sizeof(w->delivery_shipments));
    w->next_delivery_shipment_id = 1;
    w->npc_respawn_timer = 3600.0f;
    w->frontier_virtual_pilots = 0;
}

typedef struct {
    bool active;
    char dir[256];
} sr_chain_fixture_t;

static uint32_t sr_chain_fixture_nonce;

/*
 * Exact cargo-origin verification intentionally reads the signed durable
 * station history. Most replay scenarios keep chain I/O disabled, but the
 * delivery-proof scenario specifically exercises that trust boundary. Give
 * each branch an isolated temporary chain root so its fixture uses the same
 * resolver as production without sharing history across candidates or runs.
 */
static bool sr_chain_fixture_begin(const sr_config_t *config,
                                   sr_chain_fixture_t *fixture)
{
    if (!config || !fixture) return false;
    memset(fixture, 0, sizeof(*fixture));
    if (config->provenance_script !=
        SR_PROVENANCE_SCRIPT_WORKER_DELIVERY_PROOF_HNN) {
        return true;
    }

#if defined(_WIN32)
    const char *tmp_root = getenv("TEMP");
    if (!tmp_root || tmp_root[0] == '\0') tmp_root = getenv("TMP");
    if (!tmp_root || tmp_root[0] == '\0') tmp_root = ".";
#else
    const char *tmp_root = getenv("TMPDIR");
    if (!tmp_root || tmp_root[0] == '\0') tmp_root = "/tmp";
#endif

    for (int attempt = 0; attempt < 1024; attempt++) {
        uint32_t nonce = ++sr_chain_fixture_nonce;
        if (nonce == 0) nonce = ++sr_chain_fixture_nonce;
        int written = snprintf(
            fixture->dir, sizeof(fixture->dir),
            "%s/signal-replay-chain-%lu-%" PRIu32,
            tmp_root, SR_GETPID(), nonce);
        /* Leave room for "/<base58 station pubkey>.log". */
        if (written <= 0 || written > 190) return false;

        if (SR_MKDIR(fixture->dir) != 0) {
            if (errno == EEXIST) continue;
            return false;
        }

        cargo_receipt_origin_cache_reset();
        chain_log_set_dir(fixture->dir);
        chain_log_set_disk_enabled(true);
        fixture->active = true;
        return true;
    }
    return false;
}

static void sr_chain_fixture_end(sr_chain_fixture_t *fixture,
                                 const world_t *w)
{
    if (!fixture || !fixture->active) return;

    cargo_receipt_origin_cache_reset();
    if (w) {
        for (int s = 0; s < MAX_STATIONS; s++) {
            const station_t *station = &w->stations[s];
            if (!station_exists(station)) continue;
            for (uint8_t a = 0;
                 a < station->authority_registry_count &&
                 a < STATION_AUTHORITY_REGISTRY_CAP; a++) {
                char path[256];
                if (chain_log_path_for(
                        station->authority_registry[a].pubkey,
                        path, sizeof(path))) {
                    (void)remove(path);
                }
            }
        }
    }
    (void)SR_RMDIR(fixture->dir);
    chain_log_set_disk_enabled(false);
    chain_log_set_dir(NULL);
    memset(fixture, 0, sizeof(*fixture));
}

static int sr_station_remote_market_memory_items(
    const knowledge_view_t *view, int local_station);
static int sr_station_remote_known_contracts(
    const knowledge_view_t *view, int local_station);

static bool sr_setup_provenance_script(const sr_config_t *config,
                                       world_t *w,
                                       server_player_t *sp)
{
    if (!config || !w || !sp) return false;
    if (config->provenance_script == SR_PROVENANCE_SCRIPT_NONE) return true;

    switch (config->provenance_script) {
    case SR_PROVENANCE_SCRIPT_DENSE_ASTEROIDS: {
        enum { DENSE_ASTEROID_COUNT = 32 };
        int cell_x = 0;
        int cell_y = 0;
        const station_t *station = &w->stations[sp->current_station];
        vec2 desired = v2_add(station->pos, v2(3200.0f, 1600.0f));
        spatial_grid_cell(
            &w->asteroid_grid, desired, &cell_x, &cell_y);
        vec2 cell_center = v2(
            ((float)cell_x + 0.5f) * SPATIAL_CELL_SIZE,
            ((float)cell_y + 0.5f) * SPATIAL_CELL_SIZE);

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(w->fracture_claims, 0, sizeof(w->fracture_claims));
        memset(w->asteroid_origin, 0, sizeof(w->asteroid_origin));
        memset(w->asteroid_generation, 0,
               sizeof(w->asteroid_generation));
        memset(w->asteroid_generation_live, 0,
               sizeof(w->asteroid_generation_live));
        for (int i = 0; i < DENSE_ASTEROID_COUNT; i++) {
            asteroid_t *asteroid = &w->asteroids[i];
            asteroid->active = true;
            asteroid->tier =
                (i % 2) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
            asteroid->pos = v2_add(
                cell_center,
                v2(
                    (float)(i % 8) * 12.0f - 42.0f,
                    (float)(i / 8) * 12.0f - 18.0f));
            asteroid->vel = v2(
                (float)((i % 5) - 2) * 0.35f,
                (float)((i % 7) - 3) * 0.25f);
            asteroid->radius = (i % 2) ? 36.0f : 30.0f;
            asteroid->hp = 40.0f;
            asteroid->max_hp = 40.0f;
            asteroid->ore = 20.0f;
            asteroid->max_ore = 20.0f;
            asteroid->commodity = COMMODITY_FERRITE_ORE;
            asteroid->seed = (float)i + 0.25f;
            asteroid->spin =
                (float)((i % 3) - 1) * 0.02f;
            asteroid->net_dirty = true;
            asteroid->rock_pub[0] = 0xd5;
            asteroid->rock_pub[30] = (uint8_t)((i + 1) >> 8);
            asteroid->rock_pub[31] = (uint8_t)(i + 1);
        }
        /* Keep the replay focused on the fixed dense fixture instead of
         * materializing viewport chunks during its short horizon. */
        w->field_spawn_timer = -3600.0f;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_BUY_SELL: {
        const int station_index = 1; /* Kepler: seeded frame producer. */
        station_t *st;
        const uint8_t origin[8] = { 'R','E','P','L','A','Y','0','1' };
        if (station_index >= w->station_count) return false;
        st = &w->stations[station_index];
        if (!station_manifest_bootstrap(st)) return false;
        if (station_finished_mint(st, COMMODITY_FRAME, 4, NULL) < 4) {
            return false;
        }
        if (sr_spawn_station_market_pod(
                w, station_index, COMMODITY_FRAME, 1, origin) < 0) {
            return false;
        }
        ledger_earn_by_pubkey(st, sp->pubkey, 10000.0f);

        memset(w->contracts, 0, sizeof(w->contracts));
        w->contracts[0] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = (uint8_t)station_index,
            .commodity = COMMODITY_FRAME,
            .quantity_needed = 1.0f,
            .base_price = station_buy_price(st, COMMODITY_FRAME),
            .target_index = -1,
            .claimed_by = -1,
        };

        sp->docked = true;
        sp->current_station = station_index;
        sp->nearby_station = station_index;
        sp->in_dock_range = true;
        sp->dock_berth = 0;
        anchor_ship_in_station(sp, w);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_POD_TOW_SELL: {
        const int station_index = 1; /* Kepler consumes ferrite ingots. */
        const commodity_t pod_commodity = COMMODITY_FERRITE_INGOT;
        station_t *st;
        int pod_idx;
        cargo_unit_t units[7];
        const uint8_t origin[8] = { 'R','E','P','L','A','Y','0','2' };
        if (station_index >= w->station_count) return false;
        st = &w->stations[station_index];
        memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
        if (st->base_price[pod_commodity] <= FLOAT_EPSILON) {
            st->base_price[pod_commodity] = 10.0f;
        }

        sp->docked = false;
        sp->current_station = station_index;
        sp->nearby_station = -1;
        sp->in_dock_range = false;
        sp->docking_approach = false;
        sp->dock_berth = 0;
        /* Keep the fixture pod outside every station-module acquisition
         * envelope so this scenario starts with a genuinely loose target.
         * With target-side tractor authority, a module-owned pod may no
         * longer also be claimed by a player through a parallel flag. */
        sp->ship->pos = v2_add(st->pos, v2(1200.0f, 0.0f));
        sp->ship->vel = v2(0.0f, 0.0f);
        sp->ship->angle = PI_F;

        memset(units, 0, sizeof(units));
        for (uint16_t i = 0; i < 7; i++) {
            if (!hash_legacy_migrate_unit(origin, pod_commodity,
                                          i, &units[i])) {
                return false;
            }
        }
        pod_idx = spawn_cargo_pod_with_manifest_deterministic(
            w, v2_add(sp->ship->pos, v2(28.0f, 0.0f)),
            v2(0.0f, 0.0f), pod_commodity,
            units, 7, CARGO_POD_CARGO, 0.0f, 0.0f);
        return pod_idx >= 0;
    }
    case SR_PROVENANCE_SCRIPT_MINE_FRACTURE: {
        const int asteroid_idx = 0;
        asteroid_t *a = &w->asteroids[asteroid_idx];
        vec2 forward = ship_forward(sp->ship->angle);
        vec2 muzzle = ship_muzzle(sp->ship->pos, sp->ship->angle, sp->ship);

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(w->fracture_claims, 0, sizeof(w->fracture_claims));
        memset(a, 0, sizeof(*a));
        a->active = true;
        a->fracture_child = false;
        a->tier = ASTEROID_TIER_M;
        a->commodity = COMMODITY_FERRITE_ORE;
        a->pos = v2_add(muzzle, v2_scale(forward, 90.0f));
        a->vel = v2(0.0f, 0.0f);
        a->radius = 34.0f;
        a->hp = 0.32f;
        a->max_hp = 0.32f;
        a->ore = 6.0f;
        a->max_ore = 6.0f;
        a->rotation = 0.25f;
        a->spin = 0.0f;
        a->seed = 588.0f;
        a->last_towed_by = -1;
        a->last_fractured_by = -1;
        a->crystal_stage_station = 0xFF;
        a->crystal_stage_module = 0xFF;
        a->phase = ASTEROID_PHASE_SOLID;
        a->net_dirty = true;

        sp->ship->vel = v2(0.0f, 0.0f);
        sp->ship->mining_level = 0;
        sp->input.mining_target_hint = asteroid_idx;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_ASTEROID_DEATH: {
        const int asteroid_idx = 0;
        asteroid_t *a = &w->asteroids[asteroid_idx];
        const float asteroid_radius = 36.0f;
        float ship_radius = ship_hull_def(sp->ship)->ship_radius;

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(a, 0, sizeof(*a));
        a->active = true;
        a->fracture_child = false;
        a->tier = ASTEROID_TIER_M;
        a->commodity = COMMODITY_FERRITE_ORE;
        a->pos = v2(sp->ship->pos.x - (asteroid_radius + ship_radius - 3.0f),
                    sp->ship->pos.y);
        a->vel = v2(1800.0f, 0.0f);
        a->radius = asteroid_radius;
        a->hp = 25.0f;
        a->max_hp = 25.0f;
        a->ore = 5.0f;
        a->max_ore = 5.0f;
        a->rotation = 0.0f;
        a->spin = 0.0f;
        a->seed = 589.0f;
        a->last_towed_by = -1;
        a->last_fractured_by = -1;
        a->crystal_stage_station = 0xFF;
        a->crystal_stage_module = 0xFF;
        a->phase = ASTEROID_PHASE_SOLID;
        a->net_dirty = true;

        sp->ship->vel = v2(0.0f, 0.0f);
        sp->ship->hull = 20.0f;
        sp->docked = false;
        sp->docking_approach = false;
        sp->in_dock_range = false;
        sp->nearby_station = -1;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_PLANNED_OUTPOST: {
        const int station_index = SIGNAL_FIRST_OUTPOST_INDEX;
        station_t *st;
        int sc_idx;
        vec2 plan_pos = v2_add(w->stations[0].pos, v2(6200.0f, 0.0f));
        if (station_index >= MAX_STATIONS) return false;
        st = &w->stations[station_index];
        station_cleanup(st);
        memset(st, 0, sizeof(*st));
        (void)station_manifest_bootstrap(st);
        st->id = (uint32_t)station_index;
        snprintf(st->name, sizeof(st->name), "Replay Outpost");
        st->pos = plan_pos;
        st->planned = true;
        st->planned_owner = (int8_t)sp->id;
        st->placement_plan_count = 1;
        st->placement_plans[0].type = MODULE_SIGNAL_RELAY;
        st->placement_plans[0].ring = 1;
        st->placement_plans[0].slot = 0;
        st->placement_plans[0].owner = (int8_t)sp->id;
        if (w->station_count <= station_index) {
            w->station_count = station_index + 1;
        }

        sc_idx = spawn_scaffold(w, MODULE_SIGNAL_RELAY,
                                v2_add(plan_pos, v2(24.0f, 0.0f)),
                                sp->id);
        if (sc_idx < 0) return false;
        w->scaffolds[sc_idx].vel = v2(0.0f, 0.0f);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_STATION_JOSTLE: {
        const int station_index = 1;
        station_t *root;
        station_t *crowded;
        if (w->station_count <= station_index) return false;
        root = &w->stations[0];
        crowded = &w->stations[station_index];
        if (!station_is_active(root) || !station_is_active(crowded)) {
            return false;
        }
        crowded->pos = v2_add(root->pos, v2(160.0f, 0.0f));
        root->jostle_vel = v2(0.0f, 0.0f);
        crowded->jostle_vel = v2(0.0f, 0.0f);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_PLAYER_RAM: {
        server_player_t *other = &w->players[1];
        vec2 center = v2_add(w->stations[0].pos, v2(1700.0f, 240.0f));
        sr_reset_player(w, other);
        other->id = 1;
        memset(other->session_token, 0x52, sizeof(other->session_token));
        memset(other->pubkey, 0xB8, sizeof(other->pubkey));
        sp->ship->pos = center;
        other->ship->pos = v2_add(center, v2(30.0f, 0.0f));
        sp->ship->vel = v2(250.0f, 0.0f);
        other->ship->vel = v2(-250.0f, 0.0f);
        sp->ship->angle = 0.0f;
        other->ship->angle = PI_F;
        sp->ship->hull = ship_max_hull(sp->ship);
        other->ship->hull = ship_max_hull(other->ship);
        sp->docked = false;
        other->docked = false;
        sp->in_dock_range = false;
        other->in_dock_range = false;
        sp->nearby_station = -1;
        other->nearby_station = -1;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_NPC_RAM: {
        int left = spawn_npc(w, 0, NPC_ROLE_MINER);
        int right = spawn_npc(w, 0, NPC_ROLE_MINER);
        if (left < 0 || right < 0) return false;
        npc_ship_t *a = &w->npc_ships[left];
        npc_ship_t *b = &w->npc_ships[right];
        ship_t *a_ship = world_npc_ship_for(w, left);
        ship_t *b_ship = world_npc_ship_for(w, right);
        asteroid_t *target = &w->asteroids[0];
        vec2 center = v2_add(w->stations[0].pos, v2(1800.0f, 320.0f));
        if (!a_ship || !b_ship) return false;

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(target, 0, sizeof(*target));
        target->active = true;
        target->tier = ASTEROID_TIER_M;
        target->commodity = COMMODITY_FERRITE_ORE;
        target->pos = v2_add(center, v2(420.0f, 0.0f));
        target->vel = v2(0.0f, 0.0f);
        target->radius = 30.0f;
        target->hp = 20.0f;
        target->max_hp = 20.0f;
        target->ore = 5.0f;
        target->max_ore = 5.0f;
        target->phase = ASTEROID_PHASE_SOLID;
        target->net_dirty = true;

        a->state = NPC_STATE_TRAVEL_TO_ASTEROID;
        b->state = NPC_STATE_TRAVEL_TO_ASTEROID;
        a->target_asteroid = 0;
        b->target_asteroid = 0;
        a->brain_mode = SERVER_BRAIN_MODE_NONE;
        b->brain_mode = SERVER_BRAIN_MODE_NONE;
        a->ship->pos = center;
        b->ship->pos = v2_add(center, v2(30.0f, 0.0f));
        a->ship->vel = v2(250.0f, 0.0f);
        b->ship->vel = v2(-250.0f, 0.0f);
        a->ship->angle = 0.0f;
        b->ship->angle = PI_F;
        a->ship->hull = npc_max_hull(a);
        b->ship->hull = npc_max_hull(b);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_THROWN_ROCK_HIT: {
        server_player_t *target = &w->players[1];
        asteroid_t *a = &w->asteroids[0];
        vec2 center = v2_add(w->stations[0].pos, v2(1900.0f, 260.0f));

        sr_reset_player(w, target);
        target->id = 1;
        memset(target->session_token, 0x62, sizeof(target->session_token));
        memset(target->pubkey, 0xC2, sizeof(target->pubkey));

        sp->ship->pos = v2_add(center, v2(-360.0f, 0.0f));
        sp->ship->vel = v2(0.0f, 0.0f);
        sp->ship->angle = 0.0f;
        sp->docked = false;
        sp->in_dock_range = false;
        sp->nearby_station = -1;

        target->ship->pos = center;
        target->ship->vel = v2(0.0f, 0.0f);
        target->ship->angle = PI_F;
        target->ship->hull = ship_max_hull(target->ship);
        target->docked = false;
        target->in_dock_range = false;
        target->nearby_station = -1;

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(w->fracture_claims, 0, sizeof(w->fracture_claims));
        memset(a, 0, sizeof(*a));
        a->active = true;
        a->fracture_child = true;
        a->tier = ASTEROID_TIER_L;
        a->commodity = COMMODITY_FERRITE_ORE;
        a->pos = v2(target->ship->pos.x - 80.0f, target->ship->pos.y);
        a->vel = v2(500.0f, 0.0f);
        a->radius = 50.0f;
        a->hp = 1.0f;
        a->max_hp = 1.0f;
        a->ore = 1.0f;
        a->max_ore = 1.0f;
        a->rotation = 0.0f;
        a->spin = 0.0f;
        a->seed = 594.0f;
        a->last_towed_by = (int8_t)sp->id;
        memcpy(a->last_towed_token, sp->session_token,
               sizeof(a->last_towed_token));
        a->crystal_stage_station = 0xFF;
        a->crystal_stage_module = 0xFF;
        a->phase = ASTEROID_PHASE_SOLID;
        asteroid_mark_thrown(a, sp->session_token,
                             ROCK_THROW_BALLISTIC_SECONDS);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_FRACTURE_CLAIM: {
        const int asteroid_idx = 0;
        asteroid_t *a = &w->asteroids[asteroid_idx];
        fracture_claim_state_t *state = &w->fracture_claims[asteroid_idx];

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(w->fracture_claims, 0, sizeof(w->fracture_claims));
        memset(a, 0, sizeof(*a));
        memset(state, 0, sizeof(*state));

        sp->ship->pos = w->stations[0].pos;
        sp->ship->vel = v2(0.0f, 0.0f);
        sp->docked = false;
        sp->in_dock_range = false;
        sp->nearby_station = -1;

        a->active = true;
        a->fracture_child = true;
        a->tier = ASTEROID_TIER_S;
        a->commodity = COMMODITY_FERRITE_ORE;
        a->pos = v2_add(w->stations[0].pos, v2(64.0f, 0.0f));
        a->vel = v2(0.0f, 0.0f);
        a->radius = 8.0f;
        a->hp = 1.0f;
        a->max_hp = 1.0f;
        a->ore = 1.0f;
        a->max_ore = 1.0f;
        a->grade = MINING_GRADE_COMMON;
        a->phase = ASTEROID_PHASE_SOLID;
        a->crystal_stage_station = 0xFF;
        a->crystal_stage_module = 0xFF;
        for (int i = 0; i < MINING_FRACTURE_SEED_BYTES; i++) {
            a->fracture_seed[i] = (uint8_t)(0x41 + i);
        }

        state->active = true;
        state->resolved = false;
        state->challenge_dirty = true;
        state->fracture_id = 5941;
        state->deadline_ms = 500;
        state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;
        state->challenge_last_ms = 0;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_TOW_HNN: {
        const int home_station = 1;
        const int plan_slot = SIGNAL_FIRST_OUTPOST_INDEX;
        station_t *planned;
        int sc_idx;
        int npc_slot;
        npc_ship_t *npc;

        if (home_station >= w->station_count || plan_slot >= MAX_STATIONS)
            return false;

        sr_reset_worker_fixture_state(w);

        planned = &w->stations[plan_slot];
        station_cleanup(planned);
        memset(planned, 0, sizeof(*planned));
        (void)station_manifest_bootstrap(planned);
        planned->id = (uint32_t)plan_slot;
        snprintf(planned->name, sizeof(planned->name), "Replay Relay Plan");
        planned->pos = v2_add(w->stations[home_station].pos, v2(4200.0f, 0.0f));
        planned->planned = true;
        planned->planned_owner = -1;
        if (w->station_count <= plan_slot) w->station_count = plan_slot + 1;

        sc_idx = spawn_scaffold(w, MODULE_SIGNAL_RELAY,
                                v2_add(w->stations[home_station].pos,
                                       v2(220.0f, 0.0f)),
                                sp->id);
        if (sc_idx < 0) return false;
        w->scaffolds[sc_idx].state = SCAFFOLD_LOOSE;
        world_scaffold_clear_tractor(w, sc_idx);
        w->scaffolds[sc_idx].built_at_station = home_station;
        w->scaffolds[sc_idx].vel = v2(0.0f, 0.0f);

        npc_slot = spawn_npc(w, home_station, NPC_ROLE_HAULER);
        if (npc_slot < 0) return false;
        npc = &w->npc_ships[npc_slot];
        npc->role = NPC_ROLE_HAULER;
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        memset(&npc->ship->knowledge, 0, sizeof(npc->ship->knowledge));
        knowledge_view_configure(&npc->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;
        npc->ship->pos = w->stations[home_station].pos;
        npc->ship->vel = v2(0.0f, 0.0f);
        npc->ship->hull_class = HULL_CLASS_HAULER;
        npc->ship->hull = hull_max_for_class(HULL_CLASS_HAULER);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN: {
        const int home_station = 0;
        station_t *home;
        int existing_kits;
        int npc_slot;
        npc_ship_t *npc;
        market_memory_t supply = {0};
        knowledge_item_t item;

        if (home_station >= w->station_count) return false;
        home = &w->stations[home_station];
        if (!station_manifest_bootstrap(home)) return false;
        if (!station_has_module(home, MODULE_DOCK)) return false;

        sr_reset_worker_fixture_state(w);
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            w->asteroids[i].active = false;

        existing_kits = station_finished_count(home, COMMODITY_REPAIR_KIT);
        if (existing_kits > 0)
            (void)station_finished_drain(home, COMMODITY_REPAIR_KIT,
                                         existing_kits);
        if (station_finished_mint(home, COMMODITY_REPAIR_KIT, 20, NULL) < 20)
            return false;

        npc_slot = spawn_npc(w, home_station, NPC_ROLE_MINER);
        if (npc_slot < 0) return false;
        npc = &w->npc_ships[npc_slot];
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        memset(&npc->ship->knowledge, 0, sizeof(npc->ship->knowledge));
        knowledge_view_configure(&npc->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;

        npc->ship->pos = home->pos;
        npc->ship->vel = v2(0.0f, 0.0f);
        npc->ship->hull = npc_max_hull(npc) - 12.0f;

        if (!market_memory_from_station_supply(home, home_station,
                                               COMMODITY_REPAIR_KIT,
                                               w->tick, &supply)) {
            return false;
        }
        if (!knowledge_item_from_market_memory(&supply, &item))
            return false;
        knowledge_view_insert(&npc->ship->knowledge, &item);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_DELIVERY_PROOF_HNN: {
        const int origin_station = 0;
        const int dest_station = 2;
        station_t *origin;
        station_t *dest;
        int existing_origin;
        int existing_dest;
        int npc_slot;
        npc_ship_t *npc;
        contract_summary_t summary;
        market_memory_t demand = {0};
        knowledge_item_t item;

        if (origin_station >= w->station_count ||
            dest_station >= w->station_count) {
            return false;
        }
        origin = &w->stations[origin_station];
        dest = &w->stations[dest_station];
        if (!station_manifest_bootstrap(origin) ||
            !station_manifest_bootstrap(dest)) {
            return false;
        }
        if (!station_has_module(origin, MODULE_DOCK)) return false;

        sr_reset_worker_fixture_state(w);
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            w->asteroids[i].active = false;

        existing_origin = station_finished_count(origin,
                                                COMMODITY_FERRITE_INGOT);
        if (existing_origin > 0)
            (void)station_finished_drain(origin, COMMODITY_FERRITE_INGOT,
                                         existing_origin);
        existing_dest = station_finished_count(dest, COMMODITY_FERRITE_INGOT);
        if (existing_dest > 0)
            (void)station_finished_drain(dest, COMMODITY_FERRITE_INGOT,
                                         existing_dest);
        if (station_finished_mint(origin, COMMODITY_FERRITE_INGOT,
                                  2, NULL) < 2) {
            return false;
        }
        cargo_unit_t *anchored_units[2] = {
            &origin->manifest.units[origin->manifest.count - 2u],
            &origin->manifest.units[origin->manifest.count - 1u],
        };
        if (!world_anchor_legacy_cargo_origins(
                w, origin_station, anchored_units, 2)) {
            return false;
        }

        w->contracts[0] = (contract_t){
            .active = true,
            .action = CONTRACT_DELIVERY,
            .station_index = (uint8_t)dest_station,
            .target_index = origin_station,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity_needed = 1.0f,
            .base_price = 500.0f,
            .claimed_by = -1,
        };
        w->contracts[0].proof_flags = CONTRACT_PROOF_REQUIRE_PROOF;

        npc_slot = spawn_npc(w, origin_station, NPC_ROLE_HAULER);
        if (npc_slot < 0) return false;
        npc = &w->npc_ships[npc_slot];
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        memset(&npc->ship->knowledge, 0, sizeof(npc->ship->knowledge));
        knowledge_view_configure(&npc->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;

        npc->ship->pos = origin->pos;
        npc->ship->vel = v2(0.0f, 0.0f);

        summary = contract_summary_make(&w->contracts[0]);
        if (!market_memory_from_contract_summary(&summary, &demand))
            return false;
        if (!knowledge_item_from_market_memory(&demand, &item))
            return false;
        knowledge_view_insert(&npc->ship->knowledge, &item);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_GOSSIP_COURIER: {
        const int source_station = 2;
        const int receiving_station = 0;
        int npc_slot;
        npc_ship_t *npc;

        if (source_station >= w->station_count ||
            receiving_station >= w->station_count) {
            return false;
        }

        sr_reset_worker_fixture_state(w);
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            w->asteroids[i].active = false;

        w->contracts[0] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = (uint8_t)source_station,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity_needed = 3.0f,
            .base_price = 120.0f,
            .target_index = -1,
            .claimed_by = -1,
        };

        gossip_bootstrap_world_stations(w);
        if (knowledge_view_contract_count(
                &w->stations[source_station].knowledge) <= 0 ||
            knowledge_view_contract_count(
                &w->stations[receiving_station].knowledge) != 0 ||
            sr_station_remote_market_memory_items(
                &w->stations[receiving_station].knowledge,
                receiving_station) != 0) {
            return false;
        }

        npc_slot = spawn_npc(w, source_station, NPC_ROLE_HAULER);
        if (npc_slot < 0) return false;
        npc = &w->npc_ships[npc_slot];

        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        memset(&npc->ship->knowledge, 0, sizeof(npc->ship->knowledge));
        knowledge_view_configure(&npc->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;

        gossip_dock_handshake(w, source_station, &npc->ship->knowledge);
        if (knowledge_view_contract_count(&npc->ship->knowledge) <= 0 ||
            sr_station_remote_market_memory_items(&npc->ship->knowledge,
                                                  receiving_station) <= 0) {
            return false;
        }

        npc->dest_station = receiving_station;
        npc->pickup_station = -1;
        npc->pickup_commodity = COMMODITY_COUNT;
        npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
        npc->state = NPC_STATE_UNLOADING;
        npc->state_timer = 0.0f;
        npc->ship->pos = station_approach_target(
            &w->stations[receiving_station], npc->ship->pos);
        npc->ship->vel = v2(0.0f, 0.0f);
        npc->ship->hull = npc_max_hull(npc);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_NONE:
    default:
        return true;
    }
}

static void sr_hash_manifest(sha256_ctx_t *ctx, const manifest_t *manifest)
{
    uint16_t count = manifest ? manifest->count : 0;
    sr_hash_u16(ctx, count);
    if (!manifest || !manifest->units) return;
    for (uint16_t i = 0; i < count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        sr_hash_u8(ctx, u->kind);
        sr_hash_u8(ctx, u->commodity);
        sr_hash_u8(ctx, u->grade);
        sr_hash_u8(ctx, u->prefix_class);
        sr_hash_u16(ctx, u->recipe_id);
        sr_hash_u8(ctx, u->origin_station);
        sr_hash_u8(ctx, u->quantity);
        sr_hash_u64(ctx, u->mined_block);
        sha256_update(ctx, u->pub, sizeof(u->pub));
        sha256_update(ctx, u->parent_merkle, sizeof(u->parent_merkle));
    }
}

static void sr_hash_receipts(sha256_ctx_t *ctx,
                             const manifest_t *manifest,
                             const ship_receipts_t *receipts)
{
    uint16_t manifest_count = manifest ? manifest->count : 0;
    uint16_t receipt_count = receipts ? receipts->count : 0;
    sr_hash_u16(ctx, receipt_count);
    for (uint16_t i = 0; i < manifest_count; i++) {
        uint8_t len = 0;
        if (receipts && receipts->chains && i < receipts->count) {
            len = receipts->chains[i].len;
            if (len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
                len = CARGO_RECEIPT_CHAIN_MAX_LEN;
            }
        }
        sr_hash_u8(ctx, len);
        for (uint8_t j = 0; j < len; j++) {
            uint8_t packed[CARGO_RECEIPT_SIZE];
            cargo_receipt_pack(&receipts->chains[i].links[j], packed);
            sha256_update(ctx, packed, sizeof(packed));
        }
    }
}

static void sr_hash_ship_cargo_identity(sha256_ctx_t *ctx, const ship_t *ship)
{
    sr_hash_manifest(ctx, &ship->manifest);
    sr_hash_receipts(ctx, &ship->manifest, ship_get_receipts_const(ship));
}

static void sr_collect_lineage_summary(const ship_t *ship,
                                       sr_lineage_summary_t *summary)
{
    sha256_ctx_t ctx;
    const ship_receipts_t *receipts;
    if (!summary) return;
    memset(summary, 0, sizeof(*summary));
    sha256_init(&ctx);
    sha256_update(&ctx, "signal-ai-outcome-lineage-v1",
                  sizeof("signal-ai-outcome-lineage-v1") - 1u);
    if (!ship) {
        sha256_final(&ctx, summary->identity_hash);
        return;
    }

    sr_hash_ship_cargo_identity(&ctx, ship);
    sha256_final(&ctx, summary->identity_hash);

    summary->manifest_count = ship->manifest.count;
    receipts = ship_get_receipts_const(ship);
    summary->receipt_count = receipts ? receipts->count : 0;
    summary->receipt_manifest_parity =
        summary->manifest_count == summary->receipt_count;
    for (int i = 0; i < summary->manifest_count; i++) {
        if (!receipts || !receipts->chains || i >= receipts->count ||
            receipts->chains[i].len == 0) {
            summary->missing_receipt_chains++;
            continue;
        }
        const cargo_receipt_chain_t *chain = &receipts->chains[i];
        if (cargo_receipt_chain_verify(
                chain->links, chain->len, ship->manifest.units[i].pub) !=
            CARGO_RECEIPT_OK) {
            summary->invalid_receipt_chains++;
        }
    }
}

static int sr_active_contract_count(const world_t *w)
{
    int count = 0;
    if (!w) return 0;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (w->contracts[i].active) count++;
    }
    return count;
}

static sr_route_cell_t sr_route_cell_for(vec2 pos)
{
    return (sr_route_cell_t){
        .x = (int)floorf(pos.x / SR_ROUTE_CELL_SIZE),
        .y = (int)floorf(pos.y / SR_ROUTE_CELL_SIZE),
    };
}

static bool sr_route_cell_equal(sr_route_cell_t left, sr_route_cell_t right)
{
    return left.x == right.x && left.y == right.y;
}

static bool sr_route_observe_cell(sr_route_cell_t cells[SR_ROUTE_CELL_CAP],
                                  int *cell_count,
                                  sr_route_cell_t cell)
{
    if (!cells || !cell_count) return false;
    for (int i = 0; i < *cell_count; i++) {
        if (sr_route_cell_equal(cells[i], cell)) return true;
    }
    if (*cell_count < SR_ROUTE_CELL_CAP) {
        cells[*cell_count] = cell;
        (*cell_count)++;
    }
    return false;
}

static bool sr_npc_has_selected_assignment(const npc_ship_t *npc)
{
    if (!npc || !npc->active) return false;
    int diag_count = npc->job_diag_count;
    if (diag_count > 4) diag_count = 4;
    for (int i = 0; i < diag_count; i++) {
        if (npc->job_diag_selected[i] >= 200) return true;
    }
    return false;
}

static void sr_observe_worker_routes(
    const world_t *w,
    sr_worker_route_visit_t visits[SR_ROUTE_CELL_CAP],
    int *visit_count,
    sr_route_cell_t previous_cells[MAX_NPC_SHIPS],
    bool previous_cell_set[MAX_NPC_SHIPS],
    sr_ai_branch_summary_t *summary)
{
    if (!w || !visits || !visit_count || !previous_cells ||
        !previous_cell_set || !summary) {
        return;
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &w->npc_ships[i];
        if (!sr_npc_has_selected_assignment(npc) || !npc->ship) continue;
        sr_route_cell_t cell = sr_route_cell_for(npc->ship->pos);
        if (previous_cell_set[i] &&
            sr_route_cell_equal(previous_cells[i], cell)) {
            continue;
        }

        bool revisited = false;
        for (int j = 0; j < *visit_count; j++) {
            if (visits[j].npc_slot == i &&
                sr_route_cell_equal(visits[j].cell, cell)) {
                revisited = true;
                break;
            }
        }
        if (revisited && summary->worker_loop_revisits < 65535) {
            summary->worker_loop_revisits++;
        } else if (!revisited && *visit_count < SR_ROUTE_CELL_CAP) {
            visits[*visit_count] = (sr_worker_route_visit_t){
                .npc_slot = i,
                .cell = cell,
            };
            (*visit_count)++;
        }
        previous_cells[i] = cell;
        previous_cell_set[i] = true;
    }
}

static void sr_hash_station_ledger(sha256_ctx_t *ctx, const station_t *st)
{
    int count = st->ledger_count;
    if (count < 0) count = 0;
    if (count > STATION_LEDGER_MAX) count = STATION_LEDGER_MAX;
    sr_hash_i32(ctx, count);
    for (int i = 0; i < count; i++) {
        /*
         * Token-era rows are token[8] || zero[24]. Publishing their digest
         * would retain a cheap bearer oracle, so collapse every such key to
         * one explicit legacy marker. Full pubkeys remain public identity.
         */
        uint8_t legacy_suffix = 0;
        for (size_t b = 8;
             b < sizeof(st->ledger[i].player_pubkey); b++) {
            legacy_suffix |= st->ledger[i].player_pubkey[b];
        }
        sr_hash_u8(ctx, legacy_suffix == 0 ? 1u : 0u);
        if (legacy_suffix != 0) {
            sha256_update(ctx, st->ledger[i].player_pubkey,
                          sizeof(st->ledger[i].player_pubkey));
        }
        sr_hash_float_bits(ctx, st->ledger[i].balance);
        sr_hash_float_bits(ctx, st->ledger[i].lifetime_supply);
        sr_hash_u64(ctx, st->ledger[i].first_dock_tick);
        sr_hash_u64(ctx, st->ledger[i].last_dock_tick);
        sr_hash_u32(ctx, st->ledger[i].total_docks);
        sr_hash_u32(ctx, st->ledger[i].lifetime_ore_units);
        sr_hash_u32(ctx, st->ledger[i].lifetime_credits_in);
        sr_hash_u32(ctx, st->ledger[i].lifetime_credits_out);
        sr_hash_u8(ctx, st->ledger[i].top_commodity);
        sha256_update(ctx, st->ledger[i]._pad, sizeof(st->ledger[i]._pad));
    }
}

static void sr_hash_station_construction(sha256_ctx_t *ctx, const station_t *st)
{
    int module_count = st->module_count;
    int arm_count = st->arm_count;
    int pending_count = st->pending_scaffold_count;
    int plan_count = st->placement_plan_count;

    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    if (arm_count < 0) arm_count = 0;
    if (arm_count > MAX_ARMS) arm_count = MAX_ARMS;
    if (pending_count < 0) pending_count = 0;
    if (pending_count > 4) pending_count = 4;
    if (plan_count < 0) plan_count = 0;
    if (plan_count > 8) plan_count = 8;

    sr_hash_float_bits(ctx, st->radius);
    sr_hash_float_bits(ctx, st->dock_radius);
    sr_hash_float_bits(ctx, st->signal_range);
    sr_hash_float_bits(ctx, st->jostle_vel.x);
    sr_hash_float_bits(ctx, st->jostle_vel.y);
    sr_hash_u8(ctx, st->signal_connected ? 1u : 0u);
    sr_hash_u8(ctx, st->scaffold ? 1u : 0u);
    sr_hash_u8(ctx, st->planned ? 1u : 0u);
    sr_hash_i32(ctx, st->planned_owner);
    sr_hash_float_bits(ctx, st->scaffold_progress);

    sr_hash_i32(ctx, module_count);
    for (int m = 0; m < module_count; m++) {
        const station_module_t *mod = &st->modules[m];
        sr_hash_u8(ctx, (uint8_t)mod->type);
        sr_hash_u8(ctx, mod->ring);
        sr_hash_u8(ctx, mod->slot);
        sr_hash_u8(ctx, mod->scaffold ? 1u : 0u);
        sr_hash_u8(ctx, mod->last_smelt_commodity);
        sr_hash_u8(ctx, mod->commodity);
        sr_hash_float_bits(ctx, mod->build_progress);
        sr_hash_float_bits(ctx, mod->input_buffer);
        sr_hash_float_bits(ctx, mod->output_buffer);
        sr_hash_float_bits(ctx, mod->active_pulse);
        sr_hash_float_bits(ctx, mod->craft_progress);
        sr_hash_u8(ctx, mod->flow_diag);
    }

    sr_hash_i32(ctx, arm_count);
    for (int a = 0; a < MAX_ARMS; a++) {
        sr_hash_float_bits(ctx, st->arm_rotation[a]);
        sr_hash_float_bits(ctx, st->arm_speed[a]);
        sr_hash_float_bits(ctx, st->arm_omega[a]);
        sr_hash_float_bits(ctx, st->ring_offset[a]);
    }

    sr_hash_i32(ctx, pending_count);
    for (int p = 0; p < pending_count; p++) {
        sr_hash_u8(ctx, (uint8_t)st->pending_scaffolds[p].type);
        sr_hash_i32(ctx, st->pending_scaffolds[p].owner);
    }

    sr_hash_i32(ctx, plan_count);
    for (int p = 0; p < plan_count; p++) {
        sr_hash_u8(ctx, (uint8_t)st->placement_plans[p].type);
        sr_hash_u8(ctx, st->placement_plans[p].ring);
        sr_hash_u8(ctx, st->placement_plans[p].slot);
        sr_hash_i32(ctx, st->placement_plans[p].owner);
    }
}

static void sr_hash_contracts(sha256_ctx_t *ctx, const world_t *w)
{
    int active_count = 0;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (w->contracts[i].active) active_count++;
    }
    sr_hash_i32(ctx, active_count);
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        const contract_t *ct = &w->contracts[i];
        if (!ct->active) continue;
        sr_hash_i32(ctx, i);
        sr_hash_u8(ctx, (uint8_t)ct->action);
        sr_hash_u8(ctx, ct->station_index);
        sr_hash_u8(ctx, (uint8_t)ct->commodity);
        sr_hash_u8(ctx, ct->required_grade);
        sr_hash_u8(ctx, ct->proof_flags);
        sr_hash_u8(ctx, ct->required_prefix_class);
        sr_hash_u16(ctx, ct->required_recipe_id);
        sha256_update(ctx, ct->required_parent, sizeof(ct->required_parent));
        sha256_update(ctx, ct->target_pub, sizeof(ct->target_pub));
        sr_hash_u64(ctx, ct->forbidden_origin_mask);
        sr_hash_float_bits(ctx, ct->quantity_needed);
        sr_hash_float_bits(ctx, ct->base_price);
        sr_hash_float_bits(ctx, ct->age);
        sr_hash_float_bits(ctx, ct->target_pos.x);
        sr_hash_float_bits(ctx, ct->target_pos.y);
        sr_hash_i32(ctx, ct->target_index);
        sr_hash_i32(ctx, ct->claimed_by);
    }
}

static void sr_find_best_fracture_claim(const uint8_t seed[32],
                                        const uint8_t player_pub[32],
                                        uint16_t cap,
                                        uint32_t *out_nonce,
                                        mining_grade_t *out_grade)
{
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;
    for (uint32_t n = 0; n < (uint32_t)cap; n++) {
        mining_keypair_t kp;
        char callsign[8];
        mining_grade_t grade;
        mining_keypair_derive(seed, player_pub, n, &kp);
        mining_callsign_from_pubkey(kp.pub, callsign);
        grade = mining_classify_base58(callsign);
        if (grade > best_grade) {
            best_grade = grade;
            best_nonce = n;
        }
    }
    if (out_nonce) *out_nonce = best_nonce;
    if (out_grade) *out_grade = best_grade;
}

static float sr_player_station_balance(const world_t *w, const server_player_t *sp)
{
    if (!w || !sp ||
        sp->current_station < 0 || sp->current_station >= MAX_STATIONS) {
        return 0.0f;
    }
    const station_t *st = &w->stations[sp->current_station];
    return server_player_can_use_pubkey_persistence(sp)
         ? ledger_balance_by_pubkey(st, sp->pubkey)
         : ledger_balance(st, sp->session_token);
}

static void sr_hash_ship_body(sha256_ctx_t *ctx, const ship_t *ship)
{
    sr_hash_float_bits(ctx, ship->pos.x);
    sr_hash_float_bits(ctx, ship->pos.y);
    sr_hash_float_bits(ctx, ship->vel.x);
    sr_hash_float_bits(ctx, ship->vel.y);
    sr_hash_float_bits(ctx, ship->angle);
    sr_hash_float_bits(ctx, ship->hull);
    sr_hash_u8(ctx, (uint8_t)ship->hull_class);
    sr_hash_u8(ctx, ship->towed_count);
    sr_hash_u8(ctx, ship->towed_pod_count);
    sr_hash_i32(ctx, ship->towed_scaffold);
}

static void sr_hash_tractor_binding(sha256_ctx_t *ctx,
                                    const tractor_binding_t *binding)
{
    sr_hash_u8(ctx, binding ? (uint8_t)binding->kind
                            : (uint8_t)TRACTOR_SOURCE_NONE);
    sr_hash_i32(ctx, binding ? binding->source_index : -1);
    sr_hash_i32(ctx, binding ? binding->source_part : -1);
    sr_hash_u16(ctx, binding ? binding->source_generation : 0);
}

static void sr_hash_player_state(sha256_ctx_t *ctx, const server_player_t *player)
{
    sr_hash_i32(ctx, player->id);
    sha256_update(ctx, player->pubkey, sizeof(player->pubkey));
    sr_hash_u8(ctx, player->session_ready ? 1u : 0u);
    sr_hash_u8(ctx, player->pubkey_set ? 1u : 0u);
    sr_hash_u8(ctx, player->pubkey_proof_ok ? 1u : 0u);
    sr_hash_u8(ctx, player->docked ? 1u : 0u);
    sr_hash_i32(ctx, player->current_station);
    sr_hash_i32(ctx, player->nearby_station);
    sr_hash_u8(ctx, player->in_dock_range ? 1u : 0u);
    sr_hash_i32(ctx, player->dock_berth);
    sr_hash_i32(ctx, player->autopilot_mode);
    sr_hash_i32(ctx, player->autopilot_state);
    sr_hash_i32(ctx, player->autopilot_target);
    sr_hash_i32(ctx, player->autopilot_station_target);
    sr_hash_u8(ctx, (uint8_t)player->autopilot_cargo);
    sr_hash_float_bits(ctx, player->autopilot_timer);
    sr_hash_u8(ctx, player->was_in_signal ? 1u : 0u);
    sr_hash_float_bits(ctx, player->boost_hold_timer);
    sr_hash_ship_body(ctx, player->ship);
    for (int i = 0; i < (int)(sizeof(player->ship->towed_fragments) /
                              sizeof(player->ship->towed_fragments[0])); i++) {
        sr_hash_i32(ctx, player->ship->towed_fragments[i]);
    }
    for (int i = 0; i < (int)(sizeof(player->ship->towed_pods) /
                              sizeof(player->ship->towed_pods[0])); i++) {
        sr_hash_i32(ctx, player->ship->towed_pods[i]);
    }
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        sr_hash_float_bits(ctx, player->ship->cargo[c]);
    }
    sr_hash_ship_cargo_identity(ctx, player->ship);
}

static void sr_hash_contract_summary(sha256_ctx_t *ctx,
                                     const contract_summary_t *summary)
{
    sr_hash_u8(ctx, summary && summary->active ? 1u : 0u);
    if (!summary || !summary->active) return;
    sr_hash_u8(ctx, summary->action);
    sr_hash_u8(ctx, summary->station_index);
    sr_hash_u8(ctx, summary->commodity);
    sr_hash_u8(ctx, summary->required_grade);
    sr_hash_u8(ctx, summary->proof_flags);
    sr_hash_u8(ctx, summary->required_prefix_class);
    sr_hash_u16(ctx, summary->required_recipe_id);
    sha256_update(ctx, summary->required_parent,
                  sizeof(summary->required_parent));
    sha256_update(ctx, summary->target_pub, sizeof(summary->target_pub));
    sr_hash_float_bits(ctx, summary->quantity_needed);
    sr_hash_float_bits(ctx, summary->base_price);
    sr_hash_float_bits(ctx, summary->age_at_copy);
    sr_hash_u64(ctx, summary->forbidden_origin_mask);
}

static void sr_hash_known_contracts(sha256_ctx_t *ctx,
                                    const contract_summary_t *contracts,
                                    int count,
                                    int cap)
{
    if (count < 0) count = 0;
    if (count > cap) count = cap;
    sr_hash_i32(ctx, count);
    for (int i = 0; i < count; i++) {
        sr_hash_contract_summary(ctx, &contracts[i]);
    }
}

static void sr_hash_known_contract_view(sha256_ctx_t *ctx,
                                        const knowledge_view_t *view,
                                        uint8_t cap)
{
    contract_summary_t contracts[KNOWLEDGE_VIEW_MAX_CAP];
    if (cap > KNOWLEDGE_VIEW_MAX_CAP) cap = KNOWLEDGE_VIEW_MAX_CAP;
    uint8_t count = knowledge_view_collect_contracts(view, contracts, cap);
    sr_hash_known_contracts(ctx, contracts, count, cap);
}

static void sr_hash_knowledge_view(sha256_ctx_t *ctx,
                                   const knowledge_view_t *view)
{
    int count;
    int cap;
    if (!view) {
        sr_hash_i32(ctx, 0);
        sr_hash_i32(ctx, 0);
        return;
    }
    count = view->count;
    cap = view->capacity;
    if (count < 0) count = 0;
    if (cap < 0) cap = 0;
    if (count > KNOWLEDGE_VIEW_MAX_CAP) count = KNOWLEDGE_VIEW_MAX_CAP;
    if (cap > KNOWLEDGE_VIEW_MAX_CAP) cap = KNOWLEDGE_VIEW_MAX_CAP;
    sr_hash_i32(ctx, count);
    sr_hash_i32(ctx, cap);
    for (int i = 0; i < count; i++) {
        const knowledge_item_t *item = &view->items[i];
        sr_hash_u8(ctx, item->kind);
        sr_hash_u8(ctx, item->hops);
        sr_hash_u8(ctx, item->confidence);
        sr_hash_u8(ctx, item->salience);
        sr_hash_u8(ctx, item->payload_kind);
        sha256_update(ctx, item->subject_hash, sizeof(item->subject_hash));
        sha256_update(ctx, item->chain_anchor, sizeof(item->chain_anchor));
        sha256_update(ctx, item->source_hash, sizeof(item->source_hash));
        sha256_update(ctx, item->witness_hash, sizeof(item->witness_hash));
        sr_hash_u64(ctx, item->observed_tick);
        sr_hash_u64(ctx, item->learned_tick);
        sha256_update(ctx, item->payload, sizeof(item->payload));
    }
}

static void sr_hash_hnn_memory(sha256_ctx_t *ctx, const hnn_memory_t *mem)
{
    if (!mem) {
        sr_hash_i32(ctx, 0);
        return;
    }
    sr_hash_i32(ctx, mem->experience_count);
    sr_hash_float_bits(ctx, mem->last_retrieval_similarity);
    sr_hash_float_bits(ctx, mem->last_margin);
    for (int i = 0; i < HNN_DIM; i++) {
        sr_hash_float_bits(ctx, mem->store[i]);
    }
}

static void sr_hash_signal_field(sha256_ctx_t *ctx,
                                 const signal_field_t *field)
{
    if (!field) {
        sr_hash_i32(ctx, 0);
        return;
    }
    sr_hash_i32(ctx, SIGNAL_FIELD_CELL_COUNT);
    sr_hash_i32(ctx, SIGNAL_FIELD_KIND_COUNT);
    for (int i = 0; i < SIGNAL_FIELD_CELL_COUNT; i++) {
        const signal_field_cell_t *cell = &field->cells[i];
        for (int kind = 0; kind < SIGNAL_FIELD_KIND_COUNT; kind++) {
            sr_hash_float_bits(ctx, cell->strength[kind]);
            sr_hash_u32(ctx, cell->last_tick[kind]);
            sr_hash_u16(ctx, cell->observations[kind]);
        }
    }
}

static void sr_hash_fracture_claims(sha256_ctx_t *ctx, const world_t *w)
{
    int active_claims = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w->fracture_claims[i].active ||
            w->fracture_claims[i].resolved ||
            w->fracture_claims[i].challenge_dirty ||
            w->fracture_claims[i].resolved_dirty) {
            active_claims++;
        }
    }
    sr_hash_i32(ctx, active_claims);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const fracture_claim_state_t *state = &w->fracture_claims[i];
        if (!state->active &&
            !state->resolved &&
            !state->challenge_dirty &&
            !state->resolved_dirty) {
            continue;
        }
        sr_hash_i32(ctx, i);
        sr_hash_u8(ctx, state->active ? 1u : 0u);
        sr_hash_u8(ctx, state->resolved ? 1u : 0u);
        sr_hash_u8(ctx, state->challenge_dirty ? 1u : 0u);
        sr_hash_u8(ctx, state->resolved_dirty ? 1u : 0u);
        sr_hash_u32(ctx, state->fracture_id);
        sr_hash_u32(ctx, state->deadline_ms);
        sr_hash_u16(ctx, state->burst_cap);
        sr_hash_u32(ctx, state->best_nonce);
        sr_hash_u8(ctx, state->best_grade);
        sha256_update(ctx, state->best_player_pub,
                      sizeof(state->best_player_pub));
        /* Claimant bearer bytes are private; only public aggregate state is
         * part of the exported replay verifier. */
        sr_hash_u8(ctx, state->seen_claimant_count);
        sr_hash_u32(ctx, state->challenge_last_ms);
    }
}

static void sr_state_hash(const world_t *w,
                          const server_player_t *sp,
                          uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, SR_PUBLIC_STATE_HASH_DOMAIN,
                  sizeof(SR_PUBLIC_STATE_HASH_DOMAIN) - 1u);
    sr_hash_u64(&ctx, w->tick);
    sr_hash_float_bits(&ctx, w->time);
    sr_hash_u32(&ctx, w->belt_seed);
    int connected_players = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (w->players[p].connected) connected_players++;
    }
    sr_hash_i32(&ctx, connected_players);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].connected) continue;
        sr_hash_i32(&ctx, p);
        sr_hash_player_state(&ctx, &w->players[p]);
    }

    int station_count = w->station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    sr_hash_i32(&ctx, station_count);
    for (int s = 0; s < station_count; s++) {
        const station_t *st = &w->stations[s];
        sr_hash_i32(&ctx, st->id);
        sr_hash_float_bits(&ctx, st->pos.x);
        sr_hash_float_bits(&ctx, st->pos.y);
        sr_hash_station_construction(&ctx, st);
        sha256_update(&ctx, st->station_pubkey, sizeof(st->station_pubkey));
        sha256_update(&ctx, st->outpost_founder_pubkey,
                      sizeof(st->outpost_founder_pubkey));
        sr_hash_u64(&ctx, st->outpost_planted_tick);
        sr_hash_u8(&ctx, st->authority_registry_version);
        sr_hash_u8(&ctx, st->authority_registry_count);
        sha256_update(&ctx, st->authority_registry_pad,
                      sizeof(st->authority_registry_pad));
        for (uint8_t a = 0; a < STATION_AUTHORITY_REGISTRY_CAP; a++) {
            const station_authority_record_t *record =
                &st->authority_registry[a];
            sha256_update(&ctx, record->pubkey, sizeof(record->pubkey));
            sr_hash_u8(&ctx, record->lifecycle);
            sr_hash_u8(&ctx, record->trust);
            sha256_update(&ctx, record->_pad, sizeof(record->_pad));
        }
        sr_hash_manifest(&ctx, &st->manifest);
        sr_hash_receipts(&ctx, &st->manifest, station_get_receipts_const(st));
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            sr_hash_float_bits(
                &ctx, station_inventory_amount(st, (commodity_t)c));
        }
        sr_hash_station_ledger(&ctx, st);
        sr_hash_float_bits(&ctx, ledger_balance_by_pubkey(st, sp->pubkey));
        sr_hash_u64(&ctx, st->chain_event_count);
        sha256_update(&ctx, st->chain_last_hash, sizeof(st->chain_last_hash));
        sr_hash_known_contract_view(&ctx, &st->knowledge,
                                    STATION_KNOWN_CONTRACT_CAP);
        sr_hash_knowledge_view(&ctx, &st->knowledge);
        sr_hash_u32(&ctx, st->hnn_market_version);
        sr_hash_u32(&ctx, st->hnn_market_decay_tick);
        sr_hash_hnn_memory(&ctx, &st->hnn_market_memory);
        sr_hash_u32(&ctx, st->hnn_experience_version);
        sr_hash_u32(&ctx, st->hnn_experience_upload_count);
        sr_hash_u32(&ctx, st->hnn_experience_download_count);
        sr_hash_u8(&ctx, st->hnn_experience_last_source_station);
        sr_hash_hnn_memory(&ctx, &st->hnn_experience);
    }
    sr_hash_contracts(&ctx, w);
    sr_hash_fracture_claims(&ctx, w);
    sr_hash_u32(&ctx, w->signal_field_decay_tick);
    sr_hash_signal_field(&ctx, &w->signal_field);

    int active_asteroids = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (w->asteroids[i].active) active_asteroids++;
    sr_hash_i32(&ctx, active_asteroids);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        sr_hash_i32(&ctx, i);
        sr_hash_u8(&ctx, a->fracture_child ? 1u : 0u);
        sr_hash_u8(&ctx, (uint8_t)a->tier);
        sr_hash_u8(&ctx, (uint8_t)a->commodity);
        sr_hash_u8(&ctx, a->crystal_stage);
        sr_hash_u8(&ctx, a->phase);
        sr_hash_float_bits(&ctx, a->pos.x);
        sr_hash_float_bits(&ctx, a->pos.y);
        sr_hash_float_bits(&ctx, a->vel.x);
        sr_hash_float_bits(&ctx, a->vel.y);
        sr_hash_float_bits(&ctx, a->radius);
        sr_hash_float_bits(&ctx, a->hp);
        sr_hash_float_bits(&ctx, a->ore);
        sr_hash_float_bits(&ctx, a->rotation);
        sr_hash_float_bits(&ctx, a->spin);
        sr_hash_float_bits(&ctx, a->smelt_progress);
        sr_hash_tractor_binding(&ctx, &a->tractor);
        sr_hash_i32(&ctx, a->last_towed_by);
        sr_hash_i32(&ctx, a->last_fractured_by);
        sr_hash_u8(&ctx, a->thrown_timer_q);
        sr_hash_u8(&ctx, a->grade);
        sha256_update(&ctx, a->fracture_seed, sizeof(a->fracture_seed));
        sha256_update(&ctx, a->fragment_pub, sizeof(a->fragment_pub));
        sha256_update(&ctx, a->rock_pub, sizeof(a->rock_pub));
    }

    int active_npcs = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        if (w->npc_ships[i].active) active_npcs++;
    sr_hash_i32(&ctx, active_npcs);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &w->npc_ships[i];
        if (!npc->active) continue;
        sr_hash_i32(&ctx, i);
        sr_hash_u8(&ctx, (uint8_t)npc->role);
        sr_hash_u8(&ctx, (uint8_t)npc->state);
        sr_hash_ship_body(&ctx, npc->ship);
        sr_hash_ship_cargo_identity(&ctx, npc->ship);
        for (int c = 0; c < COMMODITY_COUNT; c++)
            sr_hash_float_bits(&ctx, npc->ship->cargo[c]);
        sr_hash_i32(&ctx, npc->target_asteroid);
        sr_hash_i32(&ctx, npc->home_station);
        sr_hash_i32(&ctx, npc->dest_station);
        sr_hash_float_bits(&ctx, npc->state_timer);
        sr_hash_u8(&ctx, npc->thrusting ? 1u : 0u);
        sr_hash_i32(&ctx, npc_towed_fragment_index(npc));
        sr_hash_known_contract_view(&ctx, &npc->ship->knowledge,
                                    SHIP_KNOWN_CONTRACT_CAP);
        sr_hash_knowledge_view(&ctx, &npc->ship->knowledge);
        sr_hash_u8(&ctx, npc->job_diag_count);
        for (int j = 0; j < 4; j++) {
            sr_hash_u8(&ctx, npc->job_diag_kind[j]);
            sr_hash_u8(&ctx, npc->job_diag_score[j]);
            sr_hash_u8(&ctx, npc->job_diag_selected[j]);
            sr_hash_u8(&ctx, npc->job_diag_source[j]);
            sr_hash_u8(&ctx, npc->job_diag_dest[j]);
            sr_hash_u8(&ctx, npc->job_diag_commodity[j]);
            sr_hash_u16(&ctx, npc->job_diag_hint[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_value[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_demand[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_supply[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_route[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_freshness[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_capability[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_proof[j]);
            sr_hash_u8(&ctx, npc->job_diag_factor_hologram[j]);
            sr_hash_u8(&ctx, npc->job_diag_reason[j]);
            sr_hash_u8(&ctx, npc->job_diag_memory_kind[j]);
            sr_hash_u8(&ctx, npc->job_diag_memory_hops[j]);
            sr_hash_u8(&ctx, npc->job_diag_memory_age[j]);
            sr_hash_u8(&ctx, npc->job_diag_memory_station[j]);
            sr_hash_u8(&ctx, npc->job_diag_proof_kind[j]);
            sha256_update(&ctx, npc->job_diag_proof_prefix[j],
                          sizeof(npc->job_diag_proof_prefix[j]));
            sha256_update(&ctx, npc->job_diag_proof_hash[j],
                          sizeof(npc->job_diag_proof_hash[j]));
        }
        sr_hash_u8(&ctx, npc->brain_mode);
        sr_hash_u32(&ctx, npc->hnn_market_version);
        sr_hash_u8(&ctx, npc->hnn_market_station);
        sr_hash_u32(&ctx, npc->hnn_market_decay_tick);
        sr_hash_hnn_memory(&ctx, &npc->hnn_market_mem);
        sr_hash_u32(&ctx, npc->hnn_experience_version);
        sr_hash_u32(&ctx, npc->hnn_experience_local_version);
        sr_hash_u32(&ctx, npc->hnn_experience_uploaded_local_version);
        sr_hash_u32(&ctx, npc->hnn_experience_uploaded_source_version);
        sr_hash_u8(&ctx, npc->hnn_experience_station);
        sr_hash_u8(&ctx, npc->hnn_experience_uploaded_station);
        sr_hash_u8(&ctx, npc->hnn_experience_uploaded_source_station);
        sr_hash_hnn_memory(&ctx, &npc->hnn_mem);
    }

    int active_scaffolds = 0;
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        if (w->scaffolds[i].active) active_scaffolds++;
    sr_hash_i32(&ctx, active_scaffolds);
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        sr_hash_i32(&ctx, i);
        sr_hash_u8(&ctx, (uint8_t)sc->module_type);
        sr_hash_u8(&ctx, (uint8_t)sc->state);
        sr_hash_i32(&ctx, sc->owner);
        sr_hash_float_bits(&ctx, sc->pos.x);
        sr_hash_float_bits(&ctx, sc->pos.y);
        sr_hash_float_bits(&ctx, sc->vel.x);
        sr_hash_float_bits(&ctx, sc->vel.y);
        sr_hash_float_bits(&ctx, sc->rotation);
        sr_hash_float_bits(&ctx, sc->spin);
        sr_hash_i32(&ctx, sc->placed_station);
        sr_hash_i32(&ctx, sc->placed_ring);
        sr_hash_i32(&ctx, sc->placed_slot);
        sr_hash_tractor_binding(&ctx, &sc->tractor);
        sr_hash_i32(&ctx, sc->built_at_station);
        sr_hash_float_bits(&ctx, sc->build_amount);
    }

    int active_pods = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++)
        if (w->cargo_pods[i].active) active_pods++;
    sr_hash_i32(&ctx, active_pods);
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active) continue;
        sr_hash_i32(&ctx, i);
        sr_hash_u8(&ctx, (uint8_t)pod->kind);
        sr_hash_u8(&ctx, (uint8_t)pod->commodity);
        sr_hash_u16(&ctx, pod->quantity);
        sr_hash_float_bits(&ctx, pod->pos.x);
        sr_hash_float_bits(&ctx, pod->pos.y);
        sr_hash_float_bits(&ctx, pod->vel.x);
        sr_hash_float_bits(&ctx, pod->vel.y);
        sr_hash_float_bits(&ctx, pod->rotation);
        sr_hash_float_bits(&ctx, pod->spin);
        sr_hash_float_bits(&ctx, pod->age);
        sr_hash_tractor_binding(&ctx, &pod->tractor);
    }
    sha256_final(&ctx, out);
}

static void sr_hash_public_actor(sha256_ctx_t *ctx,
                                 const public_actor_id_t *actor)
{
    public_actor_id_t value = public_actor_id_unattributed();
    if (public_actor_id_is_canonical(actor) &&
        actor->kind != (uint8_t)PUBLIC_ACTOR_ID_NONE) {
        value = *actor;
    }
    sr_hash_u8(ctx, value.kind);
    sha256_update(ctx, value.id, sizeof(value.id));
}

static void sr_hash_event(sha256_ctx_t *ctx, const sim_event_t *ev)
{
    sr_hash_u8(ctx, (uint8_t)ev->type);
    sr_hash_i32(ctx, ev->player_id);
    /*
     * Public replay output follows the v6 event trust boundary. Never hash a
     * reconnect bearer into an exported verifier; typed public IDs retain
     * stable attribution where proof exists and explicit unknown otherwise.
     */
    sr_hash_public_actor(ctx, &ev->subject_actor);
    sr_hash_public_actor(ctx, &ev->source_actor);
    switch (ev->type) {
    case SIM_EVENT_DAMAGE:
        sr_hash_float_bits(ctx, ev->damage.amount);
        sr_hash_float_bits(ctx, ev->damage.source_x);
        sr_hash_float_bits(ctx, ev->damage.source_y);
        break;
    case SIM_EVENT_DEATH:
        sr_hash_u8(ctx, ev->death.cause);
        sr_hash_i32(ctx, ev->death.respawn_station);
        sr_hash_i32(ctx, (int32_t)ev->death.respawn_fee);
        break;
    case SIM_EVENT_BUY:
        sr_hash_i32(ctx, ev->buy.station);
        sr_hash_u8(ctx, ev->buy.commodity);
        sr_hash_u8(ctx, ev->buy.grade);
        sr_hash_i32(ctx, ev->buy.cost);
        sr_hash_u16(ctx, ev->buy.quantity);
        break;
    case SIM_EVENT_SELL:
        sr_hash_i32(ctx, ev->sell.station);
        sr_hash_u8(ctx, ev->sell.grade);
        sr_hash_i32(ctx, ev->sell.base_cr);
        sr_hash_i32(ctx, ev->sell.bonus_cr);
        sr_hash_u8(ctx, ev->sell.by_contract);
        break;
    case SIM_EVENT_PICKUP:
        sr_hash_float_bits(ctx, ev->pickup.ore);
        sr_hash_i32(ctx, ev->pickup.fragments);
        break;
    case SIM_EVENT_FRACTURE:
        sr_hash_i32(ctx, ev->fracture.tier);
        sr_hash_i32(ctx, ev->fracture.asteroid_id);
        break;
    case SIM_EVENT_OUTPOST_PLACED:
        sr_hash_i32(ctx, ev->outpost_placed.slot);
        break;
    case SIM_EVENT_OUTPOST_ACTIVATED:
        sr_hash_i32(ctx, ev->outpost_activated.slot);
        break;
    case SIM_EVENT_SCAFFOLD_READY:
        sr_hash_i32(ctx, ev->scaffold_ready.station);
        sr_hash_i32(ctx, ev->scaffold_ready.module_type);
        break;
    case SIM_EVENT_CONTRACT_COMPLETE:
        sr_hash_u8(ctx, (uint8_t)ev->contract_complete.action);
        break;
    case SIM_EVENT_ORDER_REJECTED:
        sr_hash_u8(ctx, ev->order_rejected.reason);
        break;
    case SIM_EVENT_MINING_TICK:
        break;
    case SIM_EVENT_REPAIR:
    case SIM_EVENT_DOCK:
    case SIM_EVENT_LAUNCH:
    default:
        break;
    }
}

static void sr_hash_event_sequence(const sim_event_t *events,
                                   size_t count,
                                   uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, SR_PUBLIC_EVENT_HASH_DOMAIN,
                  sizeof(SR_PUBLIC_EVENT_HASH_DOMAIN) - 1u);
    for (size_t i = 0; i < count; i++) {
        sr_hash_event(&ctx, &events[i]);
    }
    sha256_final(&ctx, out);
}

/*
 * Executable regression for the exported public hash contracts. Keep the
 * fixture deliberately small but representative: every token-bearing surface
 * intentionally excluded by v7/v3 is populated, mutated, and hashed again.
 * A public state field and a public event field are then changed to prove the
 * test is not merely comparing two constant digests.
 */
static bool sr_self_test_public_hash(void)
{
    world_t *w = calloc(1, sizeof(*w));
    if (!w) {
        fprintf(stderr, "signal_replay: public hash self-test allocation failed\n");
        return false;
    }

    ship_t player_ship = {0};
    ship_t npc_ship = {0};
    server_player_t *player = &w->players[0];
    npc_ship_t *npc = &w->npc_ships[0];
    station_t *station = &w->stations[0];
    asteroid_t *asteroid = &w->asteroids[0];
    fracture_claim_state_t *claim = &w->fracture_claims[0];

    w->tick = 77u;
    w->time = 1.25f;
    w->belt_seed = 0x12345678u;
    w->station_count = 1;

    player->connected = true;
    player->id = 0;
    player->ship = &player_ship;
    player->session_ready = true;
    player->pubkey_set = true;
    player->pubkey_proof_ok = true;
    player_ship.hull = 100.0f;
    for (size_t i = 0; i < sizeof(player->pubkey); i++) {
        player->pubkey[i] = (uint8_t)(0x40u + i);
    }
    memset(player->session_token, 0x11, sizeof(player->session_token));

    station->id = 9u;
    station->ledger_count = 1;
    memset(station->ledger[0].player_pubkey, 0,
           sizeof(station->ledger[0].player_pubkey));
    memset(station->ledger[0].player_pubkey, 0x22, 8);
    station->ledger[0].balance = 42.0f;

    asteroid->active = true;
    asteroid->tier = ASTEROID_TIER_M;
    asteroid->commodity = COMMODITY_FERRITE_ORE;
    asteroid->pos = v2(5.0f, 6.0f);
    asteroid->hp = 12.0f;
    asteroid->ore = 3.0f;
    memset(asteroid->last_towed_token, 0x33,
           sizeof(asteroid->last_towed_token));
    memset(asteroid->thrown_by_token, 0x44,
           sizeof(asteroid->thrown_by_token));
    memset(asteroid->last_fractured_token, 0x55,
           sizeof(asteroid->last_fractured_token));

    claim->active = true;
    claim->fracture_id = 123u;
    claim->seen_claimant_count = 2;
    memset(claim->seen_claimant_tokens[0], 0x66,
           sizeof(claim->seen_claimant_tokens[0]));
    memset(claim->seen_claimant_tokens[1], 0x77,
           sizeof(claim->seen_claimant_tokens[1]));

    npc->active = true;
    npc->role = NPC_ROLE_HAULER;
    npc->state = NPC_STATE_DOCKED;
    npc->ship = &npc_ship;
    npc_ship.hull = 80.0f;
    memset(npc->session_token, 0x88, sizeof(npc->session_token));

    public_actor_id_t player_actor = {
        .kind = PUBLIC_ACTOR_ID_DERIVED,
        .id = {0x91},
    };
    sim_event_t events[2] = {0};
    events[0].type = SIM_EVENT_DEATH;
    events[0].player_id = 0;
    events[0].subject_actor = player_actor;
    events[0].source_actor = public_actor_id_unattributed();
    events[0].death.cause = DEATH_CAUSE_ASTEROID;
    events[0].death.respawn_station = 0;
    memset(events[0].death.killer_token, 0x99,
           sizeof(events[0].death.killer_token));
    events[1].type = SIM_EVENT_NPC_KILL;
    events[1].player_id = 0;
    events[1].subject_actor = public_actor_id_unattributed();
    events[1].source_actor = player_actor;
    events[1].npc_kill.cause = DEATH_CAUSE_RAM;
    events[1].npc_kill.npc_role = NPC_ROLE_HAULER;
    memset(events[1].npc_kill.killer_token, 0xaa,
           sizeof(events[1].npc_kill.killer_token));

    uint8_t state_before[32];
    uint8_t state_after_bearers[32];
    uint8_t state_after_public[32];
    uint8_t events_before[32];
    uint8_t events_after_bearers[32];
    uint8_t events_after_public[32];
    sr_state_hash(w, player, state_before);
    sr_hash_event_sequence(events, 2, events_before);

    memset(player->session_token, 0xb1, sizeof(player->session_token));
    memset(npc->session_token, 0xb2, sizeof(npc->session_token));
    memset(asteroid->last_towed_token, 0xb3,
           sizeof(asteroid->last_towed_token));
    memset(asteroid->thrown_by_token, 0xb4,
           sizeof(asteroid->thrown_by_token));
    memset(asteroid->last_fractured_token, 0xb5,
           sizeof(asteroid->last_fractured_token));
    memset(claim->seen_claimant_tokens[0], 0xb6,
           sizeof(claim->seen_claimant_tokens[0]));
    memset(claim->seen_claimant_tokens[1], 0xb7,
           sizeof(claim->seen_claimant_tokens[1]));
    memset(station->ledger[0].player_pubkey, 0xb8, 8);
    memset(events[0].death.killer_token, 0xb9,
           sizeof(events[0].death.killer_token));
    memset(events[1].npc_kill.killer_token, 0xba,
           sizeof(events[1].npc_kill.killer_token));

    sr_state_hash(w, player, state_after_bearers);
    sr_hash_event_sequence(events, 2, events_after_bearers);
    if (memcmp(state_before, state_after_bearers,
               sizeof(state_before)) != 0 ||
        memcmp(events_before, events_after_bearers,
               sizeof(events_before)) != 0) {
        fprintf(stderr,
                "signal_replay: bearer-only state changed a public hash\n");
        free(w);
        return false;
    }

    asteroid->pos.x += 1.0f;
    sr_state_hash(w, player, state_after_public);
    events[0].death.cause = DEATH_CAUSE_STATION;
    sr_hash_event_sequence(events, 2, events_after_public);
    if (memcmp(state_before, state_after_public,
               sizeof(state_before)) == 0 ||
        memcmp(events_before, events_after_public,
               sizeof(events_before)) == 0) {
        fprintf(stderr,
                "signal_replay: public state failed to change a public hash\n");
        free(w);
        return false;
    }

    free(w);
    printf("signal_replay public hash invariance: ok\n");
    return true;
}

static void sr_accumulate_events(const world_t *w,
                                 sr_event_counts_t *counts,
                                 sha256_ctx_t *event_hash)
{
    if (!w || !counts || !event_hash) return;
    for (int i = 0; i < w->events.count; i++) {
        const sim_event_t *ev = &w->events.events[i];
        sr_hash_event(event_hash, ev);
        switch (ev->type) {
        case SIM_EVENT_DAMAGE:
            counts->damage_events++;
            counts->damage_amount += ev->damage.amount;
            break;
        case SIM_EVENT_DEATH:
            counts->death_events++;
            break;
        case SIM_EVENT_DOCK:
            counts->dock_events++;
            break;
        case SIM_EVENT_LAUNCH:
            counts->launch_events++;
            break;
        case SIM_EVENT_PICKUP:
            counts->pickup_events++;
            counts->pickup_ore += ev->pickup.ore;
            counts->pickup_fragments += ev->pickup.fragments;
            break;
        case SIM_EVENT_BUY:
            counts->buy_events++;
            counts->buy_cost += ev->buy.cost;
            counts->buy_quantity += ev->buy.quantity;
            break;
        case SIM_EVENT_SELL:
            counts->sell_events++;
            counts->sell_base += ev->sell.base_cr;
            counts->sell_bonus += ev->sell.bonus_cr;
            break;
        case SIM_EVENT_REPAIR:
            if (!counts->first_repair_tick_set) {
                counts->first_repair_tick = w->tick;
                counts->first_repair_tick_set = true;
            }
            counts->repair_events++;
            break;
        case SIM_EVENT_MINING_TICK:
            counts->mining_tick_events++;
            break;
        case SIM_EVENT_FRACTURE:
            counts->fracture_events++;
            break;
        case SIM_EVENT_OUTPOST_PLACED:
            counts->outpost_placed_events++;
            break;
        case SIM_EVENT_SCAFFOLD_READY:
            counts->scaffold_ready_events++;
            break;
        case SIM_EVENT_CONTRACT_COMPLETE:
            if (!counts->first_contract_complete_tick_set) {
                counts->first_contract_complete_tick = w->tick;
                counts->first_contract_complete_tick_set = true;
            }
            counts->contract_complete_events++;
            break;
        case SIM_EVENT_ORDER_REJECTED:
            counts->order_rejected_events++;
            break;
        default:
            break;
        }
    }
}

static bool sr_run_provenance_script(const sr_config_t *config,
                                     world_t *w,
                                     server_player_t *sp,
                                     sr_event_counts_t *counts,
                                     sha256_ctx_t *event_hash)
{
    if (!config || !w || !sp || !counts || !event_hash) return false;
    if (config->provenance_script == SR_PROVENANCE_SCRIPT_NONE) return true;

    switch (config->provenance_script) {
    case SR_PROVENANCE_SCRIPT_DENSE_ASTEROIDS: {
        enum { DENSE_ASTEROID_COUNT = 32 };
        spatial_grid_build(w);
        asteroid_pair_plan_t plan;
        return asteroid_pair_plan_build(w, &plan) &&
               plan.active_count == DENSE_ASTEROID_COUNT &&
               plan.max_cell_count == DENSE_ASTEROID_COUNT &&
               plan.candidate_pair_count == 128u;
    }
    case SR_PROVENANCE_SCRIPT_BUY_SELL: {
        int buy_before = counts->buy_events;
        int sell_before = counts->sell_events;
        int start_towed_pods = sp->ship->towed_pod_count;
        int pod_idx = -1;

        sp->input.buy_product = true;
        sp->input.buy_commodity = COMMODITY_FRAME;
        sp->input.buy_grade = MINING_GRADE_COMMON;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        sp->input.buy_product = false;
        if (sp->ship->towed_pod_count <= start_towed_pods) {
            return false;
        }
        pod_idx = sp->ship->towed_pods[start_towed_pods];
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS ||
            !w->cargo_pods[pod_idx].active ||
            cargo_pod_player_tractor(&w->cargo_pods[pod_idx]) != sp->id ||
            w->cargo_pods[pod_idx].commodity != COMMODITY_FRAME ||
            w->cargo_pods[pod_idx].manifest_count == 0) {
            return false;
        }

        sr_move_pod_past_station_charge_boundary(
            w, sp->current_station, pod_idx);
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->buy_events <= buy_before ||
            cargo_pod_custody_station(&w->cargo_pods[pod_idx]) >= 0) {
            return false;
        }

        int hopper_idx = station_find_hopper_for(
            &w->stations[sp->current_station], COMMODITY_FRAME);
        if (hopper_idx < 0) return false;
        w->cargo_pods[pod_idx].pos = module_world_pos_ring(
            &w->stations[sp->current_station],
            w->stations[sp->current_station].modules[hopper_idx].ring,
            w->stations[sp->current_station].modules[hopper_idx].slot);
        w->cargo_pods[pod_idx].vel = v2(0.0f, 0.0f);
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->sell_events <= sell_before ||
            sp->ship->towed_pod_count != start_towed_pods ||
            !w->cargo_pods[pod_idx].active ||
            !cargo_pod_has_module_tractor(&w->cargo_pods[pod_idx])) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_POD_TOW_SELL: {
        int pickup_before = counts->pickup_events;
        int sell_before = counts->sell_events;
        int station_index = sp->current_station;
        const commodity_t pod_commodity = COMMODITY_FERRITE_INGOT;
        int pod_idx = -1;

        sp->input.tractor_hold = true;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->pickup_events <= pickup_before ||
            sp->ship->towed_pod_count <= 0) {
            return false;
        }

        pod_idx = sp->ship->towed_pods[0];
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS ||
            !w->cargo_pods[pod_idx].active ||
            cargo_pod_player_tractor(&w->cargo_pods[pod_idx]) != sp->id) {
            return false;
        }
        int hopper_idx = station_find_hopper_for(
            &w->stations[station_index], pod_commodity);
        if (hopper_idx < 0) return false;
        w->cargo_pods[pod_idx].pos = module_world_pos_ring(
            &w->stations[station_index],
            w->stations[station_index].modules[hopper_idx].ring,
            w->stations[station_index].modules[hopper_idx].slot);
        w->cargo_pods[pod_idx].vel = v2(0.0f, 0.0f);
        w->events.count = 0;
        step_station_cargo_pod_tractors(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->sell_events <= sell_before ||
            sp->ship->towed_pod_count != 0 ||
            !w->cargo_pods[pod_idx].active ||
            !cargo_pod_has_module_tractor(&w->cargo_pods[pod_idx])) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_MINE_FRACTURE: {
        int mining_before = counts->mining_tick_events;
        int fracture_before = counts->fracture_events;

        for (int i = 0; i < 30 && counts->fracture_events <= fracture_before; i++) {
            sp->input.mine = true;
            sp->input.mining_target_hint = 0;
            world_sim_step(w, SIM_DT);
            sr_accumulate_events(w, counts, event_hash);
        }
        sp->input.mine = false;

        if (counts->mining_tick_events <= mining_before ||
            counts->fracture_events <= fracture_before ||
            !w->asteroids[0].active ||
            !w->asteroids[0].fracture_child ||
            sp->ship->stat_asteroids_fractured <= 0) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_ASTEROID_DEATH: {
        int damage_before = counts->damage_events;
        int death_before = counts->death_events;

        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        if (counts->damage_events <= damage_before ||
            counts->death_events <= death_before ||
            !sp->docked ||
            sp->ship->hull <= 0.0f) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_PLANNED_OUTPOST: {
        const int station_index = SIGNAL_FIRST_OUTPOST_INDEX;
        int outpost_before = counts->outpost_placed_events;
        const station_t *st;

        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        if (counts->outpost_placed_events <= outpost_before ||
            station_index >= w->station_count) {
            return false;
        }
        st = &w->stations[station_index];
        if (st->planned ||
            !st->scaffold ||
            st->radius <= 0.0f ||
            st->dock_radius <= 0.0f ||
            st->signal_range <= 0.0f ||
            st->module_count < 2 ||
            st->placement_plan_count != 0) {
            return false;
        }
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            if (w->scaffolds[i].active) return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_STATION_JOSTLE: {
        float before = v2_dist_sq(w->stations[0].pos, w->stations[1].pos);
        for (int i = 0; i < 12; i++) {
            world_sim_step(w, SIM_DT);
            sr_accumulate_events(w, counts, event_hash);
        }
        float after = v2_dist_sq(w->stations[0].pos, w->stations[1].pos);
        if (after <= before ||
            v2_len_sq(w->stations[0].jostle_vel) <= 0.0001f ||
            v2_len_sq(w->stations[1].jostle_vel) <= 0.0001f) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_PLAYER_RAM: {
        server_player_t *other = &w->players[1];
        int damage_before = counts->damage_events;
        float primary_hull = sp->ship->hull;
        float other_hull = other->ship->hull;

        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        if (counts->damage_events <= damage_before ||
            sp->ship->hull >= primary_hull ||
            other->ship->hull >= other_hull ||
            v2_dist_sq(sp->ship->pos, other->ship->pos) <= 0.0f) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_NPC_RAM: {
        ship_t *left = world_npc_ship_for(w, 0);
        ship_t *right = world_npc_ship_for(w, 1);
        float left_hull;
        float right_hull;
        if (!left || !right) return false;
        left_hull = left->hull;
        right_hull = right->hull;

        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        if (!w->npc_ships[0].active ||
            !w->npc_ships[1].active ||
            left->hull >= left_hull ||
            right->hull >= right_hull ||
            v2_dist_sq(w->npc_ships[0].ship->pos,
                       w->npc_ships[1].ship->pos) <= 0.0f) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_THROWN_ROCK_HIT: {
        server_player_t *target = &w->players[1];
        asteroid_t *a = &w->asteroids[0];
        int damage_before = counts->damage_events;
        float target_hull = target->ship->hull;
        bool hit = false;

        for (int i = 0; i < 60 && !hit; i++) {
            world_sim_step(w, SIM_DT);
            sr_accumulate_events(w, counts, event_hash);
            hit = counts->damage_events > damage_before ||
                  target->ship->hull < target_hull ||
                  !asteroid_is_ballistic(a);
        }

        if (counts->damage_events <= damage_before ||
            target->ship->hull >= target_hull ||
            asteroid_is_ballistic(a)) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_FRACTURE_CLAIM: {
        asteroid_t *a = &w->asteroids[0];
        fracture_claim_state_t *state = &w->fracture_claims[0];
        uint8_t player_pub[32];
        uint8_t expected_pub[32];
        uint32_t best_nonce = 0;
        mining_grade_t best_grade = MINING_GRADE_COMMON;

        if (!a->active || !state->active || state->resolved) return false;
        sha256_bytes(sp->session_token, sizeof(sp->session_token), player_pub);
        sr_find_best_fracture_claim(a->fracture_seed, player_pub,
                                    state->burst_cap,
                                    &best_nonce, &best_grade);
        if (!submit_fracture_claim(w, sp->id, state->fracture_id,
                                   best_nonce, (uint8_t)best_grade)) {
            return false;
        }
        if (state->best_nonce != best_nonce ||
            state->best_grade != (uint8_t)best_grade ||
            state->seen_claimant_count != 1 ||
            memcmp(state->seen_claimant_tokens[0], sp->session_token,
                   sizeof(sp->session_token)) != 0) {
            return false;
        }

        w->time = 1.0f;
        step_fracture_claims(w);
        mining_fragment_pub_compute(a->fracture_seed, player_pub,
                                    best_nonce, expected_pub);
        if (state->active ||
            !state->resolved ||
            a->grade != (uint8_t)best_grade ||
            memcmp(a->fragment_pub, expected_pub, sizeof(expected_pub)) != 0) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_TOW_HNN: {
        bool selected_worker = false;
        bool hologram_worker = false;
        bool tow_worker = false;
        bool worker_pickup = false;
        int tow_npc_slot = -1;
        int tow_scaffold_slot = -1;

        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            const npc_ship_t *npc = &w->npc_ships[n];
            if (!npc->active) continue;
            for (int j = 0; j < npc->job_diag_count && j < 4; j++) {
                if (npc->job_diag_selected[j] >= 200) {
                    selected_worker = true;
                    if (npc->job_diag_kind[j] ==
                        (uint8_t)INSPECT_DIAG_JOB_TOW) {
                        tow_worker = true;
                        tow_npc_slot = n;
                        tow_scaffold_slot = npc->target_asteroid;
                    }
                    if (npc->job_diag_factor_hologram[j] > 0) {
                        hologram_worker = true;
                    }
                }
            }
        }
        if (!selected_worker || !tow_worker || !hologram_worker ||
            tow_npc_slot < 0 ||
            tow_scaffold_slot < 0 ||
            tow_scaffold_slot >= MAX_SCAFFOLDS) {
            return false;
        }

        npc_ship_t *npc = &w->npc_ships[tow_npc_slot];
        scaffold_t *sc = &w->scaffolds[tow_scaffold_slot];
        ship_t *ship = world_npc_ship_for(w, tow_npc_slot);
        if (!npc->active || !sc->active || sc->state != SCAFFOLD_LOOSE)
            return false;
        npc->ship->pos = sc->pos;
        npc->ship->vel = v2(0.0f, 0.0f);
        if (ship) {
            ship->pos = npc->ship->pos;
            ship->vel = npc->ship->vel;
        }

        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        worker_pickup =
            npc->ship->towed_scaffold == tow_scaffold_slot &&
            sc->state == SCAFFOLD_TOWING &&
            scaffold_tractor_npc(sc) == tow_npc_slot;
        return worker_pickup;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN: {
        bool selected_repair = false;
        bool hologram_repair = false;
        int repair_npc_slot = -1;
        int kits_before = -1;
        float hull_before = 0.0f;

        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            const npc_ship_t *npc = &w->npc_ships[n];
            const ship_t *ship = world_npc_ship_for(w, n);
            if (!npc->active || !ship) continue;
            if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS)
                continue;
            if (ship->hull < npc_max_hull(npc) - 0.5f) {
                repair_npc_slot = n;
                hull_before = ship->hull;
                kits_before = station_finished_count(
                    &w->stations[npc->home_station], COMMODITY_REPAIR_KIT);
                break;
            }
        }
        if (repair_npc_slot < 0 || kits_before <= 0)
            return false;

        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        npc_ship_t *npc = &w->npc_ships[repair_npc_slot];
        ship_t *ship = world_npc_ship_for(w, repair_npc_slot);
        if (!npc->active || !ship) return false;

        for (int j = 0; j < npc->job_diag_count && j < 4; j++) {
            if (npc->job_diag_kind[j] == (uint8_t)INSPECT_DIAG_JOB_REPAIR &&
                npc->job_diag_selected[j] >= 200) {
                selected_repair = true;
                if (npc->job_diag_factor_hologram[j] > 0)
                    hologram_repair = true;
            }
        }

        int kits_after = station_finished_count(
            &w->stations[npc->home_station], COMMODITY_REPAIR_KIT);
        return selected_repair &&
               hologram_repair &&
               ship->hull > hull_before &&
               kits_after < kits_before;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_DELIVERY_PROOF_HNN: {
        bool selected_delivery = false;
        bool hologram_delivery = false;
        int delivery_npc_slot = -1;
        delivery_shipment_t *shipment = NULL;

        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            const npc_ship_t *npc = &w->npc_ships[n];
            if (!npc->active) continue;
            for (int j = 0; j < npc->job_diag_count && j < 4; j++) {
                if (npc->job_diag_kind[j] ==
                        (uint8_t)INSPECT_DIAG_JOB_DELIVER_PROOF &&
                    npc->job_diag_selected[j] >= 200) {
                    selected_delivery = true;
                    delivery_npc_slot = n;
                    if (npc->job_diag_factor_hologram[j] > 0)
                        hologram_delivery = true;
                }
            }
        }
        if (!selected_delivery || !hologram_delivery ||
            delivery_npc_slot < 0) {
            return false;
        }

        ship_t *ship = world_npc_ship_for(w, delivery_npc_slot);
        npc_ship_t *npc = &w->npc_ships[delivery_npc_slot];
        if (!ship || !npc->active) return false;
        for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
            delivery_shipment_t *candidate = &w->delivery_shipments[i];
            if (!candidate->active) continue;
            if (candidate->contract_index != 0) continue;
            if (candidate->debtor_player !=
                (uint8_t)(MAX_PLAYERS + delivery_npc_slot)) {
                continue;
            }
            shipment = candidate;
            break;
        }
        if (!shipment ||
            shipment->status != DELIVERY_SHIPMENT_PICKED_UP ||
            shipment->origin_station != 0 ||
            shipment->destination_station != 2 ||
            shipment->quantity_bound <= 0 ||
            ship_finished_count(ship, COMMODITY_FERRITE_INGOT) <= 0) {
            return false;
        }

        npc->dest_station = 2;
        npc->pickup_station = -1;
        npc->pickup_commodity = COMMODITY_COUNT;
        npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
        npc->state = NPC_STATE_UNLOADING;
        npc->state_timer = 0.0f;
        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (shipment->status != DELIVERY_SHIPMENT_DELIVERED ||
            shipment->quantity_delivered != shipment->quantity_total ||
            w->contracts[0].quantity_needed > 0.01f ||
            station_finished_count(&w->stations[2],
                                   COMMODITY_FERRITE_INGOT) <= 0) {
            return false;
        }

        npc->dest_station = 0;
        npc->state = NPC_STATE_UNLOADING;
        npc->state_timer = 0.0f;
        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        return shipment->status == DELIVERY_SHIPMENT_CLEARED &&
               !w->contracts[0].active;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_GOSSIP_COURIER: {
        const int receiving_station = 0;
        npc_ship_t *npc = NULL;
        int courier_slot = -1;

        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            if (!w->npc_ships[n].active) continue;
            if (w->npc_ships[n].dest_station != receiving_station) continue;
            courier_slot = n;
            npc = &w->npc_ships[n];
            break;
        }
        if (courier_slot < 0 || !npc) return false;

        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        return sr_station_remote_known_contracts(
                   &w->stations[receiving_station].knowledge,
                   receiving_station) > 0 &&
               sr_station_remote_market_memory_items(
                   &w->stations[receiving_station].knowledge,
                   receiving_station) > 0 &&
               signal_field_query(&w->signal_field,
                                  w->stations[receiving_station].pos,
                                  SIGNAL_FIELD_KIND_DEMAND, 0) > 0.0f;
    }
    case SR_PROVENANCE_SCRIPT_NONE:
    default:
        return true;
    }
}

static int sr_clamped_u8_count(uint8_t count, int cap)
{
    int value = (int)count;
    if (value < 0) value = 0;
    if (value > cap) value = cap;
    return value;
}

static void sr_track_hnn_load(float *max_load, const hnn_memory_t *mem)
{
    float load;
    if (!max_load || !mem || mem->experience_count <= 0) return;
    load = hnn_memory_capacity_load(mem);
    if (isfinite(load) && load > *max_load) *max_load = load;
}

static float sr_npc_finished_cargo_total(const npc_ship_t *npc)
{
    float total = 0.0f;
    if (!npc) return 0.0f;
    const ship_t *ship = npc->ship;
    if (ship->manifest.count > 0) {
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
            total += (float)ship_finished_count(ship, (commodity_t)c);
        return total;
    }
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
        total += ship->cargo[c];
    return total;
}

static bool sr_delivery_debtor_is_npc(uint8_t debtor)
{
    int value = (int)debtor;
    return value >= MAX_PLAYERS && value < MAX_PLAYERS + MAX_NPC_SHIPS;
}

static void sr_count_selected_job(sr_ai_summary_t *out,
                                  uint8_t job_kind,
                                  bool hologram)
{
    if (!out) return;
    switch ((inspect_diag_kind_t)job_kind) {
    case INSPECT_DIAG_JOB_MINE:
        out->worker_mine_assignments++;
        if (hologram) out->worker_hologram_mine_assignments++;
        break;
    case INSPECT_DIAG_JOB_HAUL:
        out->worker_haul_assignments++;
        if (hologram) out->worker_hologram_haul_assignments++;
        break;
    case INSPECT_DIAG_JOB_TOW:
        out->worker_tow_assignments++;
        if (hologram) out->worker_hologram_tow_assignments++;
        break;
    case INSPECT_DIAG_JOB_DELIVER_PROOF:
        out->worker_delivery_assignments++;
        if (hologram) out->worker_hologram_delivery_assignments++;
        break;
    case INSPECT_DIAG_JOB_SCOUT:
        out->worker_scout_assignments++;
        if (hologram) out->worker_hologram_scout_assignments++;
        break;
    case INSPECT_DIAG_JOB_REPAIR:
        out->worker_repair_assignments++;
        if (hologram) out->worker_hologram_repair_assignments++;
        break;
    default:
        break;
    }
}

static void sr_ai_branch_observe(sr_ai_summary_t *out,
                                 const sr_ai_summary_t *sample,
                                 int relative_tick)
{
    if (!out || !sample || !sample->enabled) return;
    sr_ai_branch_summary_t *b = &out->branch;
    int assignments =
        sample->worker_mine_assignments +
        sample->worker_haul_assignments +
        sample->worker_tow_assignments +
        sample->worker_delivery_assignments +
        sample->worker_scout_assignments +
        sample->worker_repair_assignments;
    int hologram_assignments =
        sample->worker_hologram_mine_assignments +
        sample->worker_hologram_haul_assignments +
        sample->worker_hologram_tow_assignments +
        sample->worker_hologram_delivery_assignments +
        sample->worker_hologram_scout_assignments +
        sample->worker_hologram_repair_assignments;
    int motion =
        sample->workers_travel_to_pickup +
        sample->workers_travel_to_dest +
        sample->workers_unloading +
        sample->workers_returning +
        sample->workers_towing_scaffold;
    int route_support =
        sample->workers_travel_to_dest +
        sample->workers_returning;
    int scaffold_motion =
        sample->workers_towing_scaffold +
        sample->scaffolds_towing +
        sample->scaffolds_towed_by_worker +
        sample->scaffolds_snapping +
        sample->scaffolds_placed;
    int delivery_work =
        sample->npc_delivery_shipments_active +
        sample->npc_delivery_shipments_picked_up +
        sample->npc_delivery_shipments_delivered +
        sample->npc_delivery_shipments_cleared +
        sample->npc_delivery_shipments_defaulted +
        sample->npc_delivery_shipments_black_market_sold;
    int useful =
        sample->workers_unloading +
        sample->workers_with_finished_cargo +
        scaffold_motion +
        delivery_work +
        sample->worker_repair_assignments +
        sample->worker_delivery_assignments;

    out->enabled = true;
    if (sample->active_npcs > 0) b->active_ticks++;
    if (sample->worker_selected_rows > b->worker_selected_rows_peak)
        b->worker_selected_rows_peak = sample->worker_selected_rows;
    if (sample->worker_hologram_rows > b->worker_hologram_rows_peak)
        b->worker_hologram_rows_peak = sample->worker_hologram_rows;
    if (assignments > 0) b->worker_assignment_ticks++;
    if (hologram_assignments > 0) b->worker_hologram_assignment_ticks++;
    if (sample->worker_mine_assignments > 0) b->worker_mine_assignment_ticks++;
    if (sample->worker_haul_assignments > 0) b->worker_haul_assignment_ticks++;
    if (sample->worker_tow_assignments > 0) b->worker_tow_assignment_ticks++;
    if (sample->worker_delivery_assignments > 0)
        b->worker_delivery_assignment_ticks++;
    if (sample->worker_scout_assignments > 0)
        b->worker_scout_assignment_ticks++;
    if (sample->worker_repair_assignments > 0)
        b->worker_repair_assignment_ticks++;
    if (motion > 0) b->worker_motion_ticks++;
    if (route_support > 0) b->worker_route_support_ticks++;
    if (sample->workers_with_finished_cargo > 0 ||
        sample->worker_finished_cargo_units > 0.01f) {
        b->worker_cargo_ticks++;
    }
    if (scaffold_motion > 0) b->worker_scaffold_motion_ticks++;
    if (delivery_work > 0) b->worker_delivery_shipment_ticks++;
    if (useful > 0) b->worker_useful_outcome_ticks++;
    if (assignments > 0 && motion == 0 && useful == 0) {
        b->worker_stuck_ticks++;
        b->worker_stuck_latched = true;
    } else if (b->worker_stuck_latched && (motion > 0 || useful > 0)) {
        b->worker_recovery_events++;
        b->worker_stuck_latched = false;
    }
    if (b->first_worker_completion_tick < 0 &&
        (sample->npc_delivery_shipments_cleared > 0 ||
         sample->scaffolds_placed > 0)) {
        b->first_worker_completion_tick = relative_tick;
    }
}

static int sr_station_remote_known_contracts(
    const knowledge_view_t *view, int local_station)
{
    contract_summary_t contracts[KNOWLEDGE_VIEW_MAX_CAP];
    int total = 0;
    if (!view || local_station < 0 || local_station >= MAX_STATIONS)
        return 0;
    int count = knowledge_view_collect_contracts(
        view, contracts, KNOWLEDGE_VIEW_MAX_CAP);
    for (int i = 0; i < count; i++) {
        const contract_summary_t *summary = &contracts[i];
        if (!summary->active) continue;
        if (summary->station_index >= MAX_STATIONS) continue;
        if ((int)summary->station_index != local_station)
            total++;
    }
    return total;
}

static bool sr_station_ref_is_remote(uint8_t station, int local_station)
{
    return station < MAX_STATIONS && (int)station != local_station;
}

static int sr_station_remote_market_memory_items(
    const knowledge_view_t *view, int local_station)
{
    int total = 0;
    int count;
    if (!view || local_station < 0 || local_station >= MAX_STATIONS)
        return 0;
    count = sr_clamped_u8_count(view->count, KNOWLEDGE_VIEW_MAX_CAP);
    for (int i = 0; i < count; i++) {
        market_memory_t memory;
        if (!market_memory_from_knowledge_item(&view->items[i], &memory))
            continue;
        if (!memory.active) continue;
        if (sr_station_ref_is_remote(memory.station_a, local_station) ||
            sr_station_ref_is_remote(memory.station_b, local_station)) {
            total++;
        }
    }
    return total;
}

static void sr_collect_ai_summary(const world_t *w, sr_ai_summary_t *out)
{
    int station_count;
    if (!w || !out) return;
    memset(out, 0, sizeof(*out));
    out->enabled = true;

    station_count = w->station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    {
        signal_field_diagnostics_t field =
            signal_field_diagnostics(&w->signal_field, v2(0.0f, 0.0f), 1);
        out->signal_field_occupied_slots = field.occupied_slots;
        out->signal_field_capacity_slots = field.capacity_slots;
        out->signal_field_load = field.load;
    }
    for (int s = 0; s < station_count; s++) {
        const station_t *st = &w->stations[s];
        signal_field_diagnostics_t field =
            signal_field_diagnostics(&w->signal_field, st->pos, 1);
        if (field.noisy)
            out->signal_field_noisy_station_cells++;
        if (field.top_strength > out->signal_field_max_strength)
            out->signal_field_max_strength = field.top_strength;
        if (field.top_strength > 0.0001f) {
            if (out->signal_field_min_margin <= 0.0f ||
                field.top_margin < out->signal_field_min_margin) {
                out->signal_field_min_margin = field.top_margin;
            }
            if (out->signal_field_min_snr <= 0.0f ||
                field.recall_snr_estimate < out->signal_field_min_snr) {
                out->signal_field_min_snr = field.recall_snr_estimate;
            }
        }
        out->station_known_contracts +=
            knowledge_view_contract_count(&st->knowledge);
        out->station_knowledge_items += sr_clamped_u8_count(
            st->knowledge.count, KNOWLEDGE_VIEW_MAX_CAP);
        out->station_remote_known_contracts +=
            sr_station_remote_known_contracts(&st->knowledge, s);
        out->station_remote_market_memory_items +=
            sr_station_remote_market_memory_items(&st->knowledge, s);
        if (st->hnn_market_memory.experience_count > 0) {
            out->station_hnn_market_stored +=
                st->hnn_market_memory.experience_count;
        }
        if (st->hnn_experience.experience_count > 0) {
            out->station_hnn_experience_stored +=
                st->hnn_experience.experience_count;
        }
        out->station_hnn_market_versions += (int)st->hnn_market_version;
        out->station_hnn_experience_versions +=
            (int)st->hnn_experience_version;
        sr_track_hnn_load(&out->max_station_market_load,
                          &st->hnn_market_memory);
        sr_track_hnn_load(&out->max_station_experience_load,
                          &st->hnn_experience);
    }

    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        switch (sc->state) {
        case SCAFFOLD_LOOSE:
            out->scaffolds_loose++;
            break;
        case SCAFFOLD_TOWING:
            out->scaffolds_towing++;
            if (scaffold_tractor_npc(sc) >= 0)
                out->scaffolds_towed_by_worker++;
            break;
        case SCAFFOLD_SNAPPING:
            out->scaffolds_snapping++;
            break;
        case SCAFFOLD_PLACED:
            out->scaffolds_placed++;
            break;
        case SCAFFOLD_NASCENT:
        default:
            break;
        }
    }

    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        const delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (!shipment->active ||
            !sr_delivery_debtor_is_npc(shipment->debtor_player)) {
            continue;
        }
        switch ((delivery_shipment_status_t)shipment->status) {
        case DELIVERY_SHIPMENT_OFFERED:
            out->npc_delivery_shipments_active++;
            break;
        case DELIVERY_SHIPMENT_PICKED_UP:
            out->npc_delivery_shipments_active++;
            out->npc_delivery_shipments_picked_up++;
            break;
        case DELIVERY_SHIPMENT_DELIVERED:
            out->npc_delivery_shipments_active++;
            out->npc_delivery_shipments_delivered++;
            break;
        case DELIVERY_SHIPMENT_CLEARED:
            out->npc_delivery_shipments_cleared++;
            break;
        case DELIVERY_SHIPMENT_DEFAULTED:
            out->npc_delivery_shipments_defaulted++;
            break;
        case DELIVERY_SHIPMENT_BLACK_MARKET_SOLD:
            out->npc_delivery_shipments_black_market_sold++;
            break;
        default:
            break;
        }
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &w->npc_ships[i];
        int diag_count;
        float finished_cargo;
        if (!npc->active) continue;
        out->active_npcs++;
        switch (npc->state) {
        case NPC_STATE_TRAVEL_TO_ASTEROID:
            out->workers_travel_to_pickup++;
            break;
        case NPC_STATE_TRAVEL_TO_DEST:
            out->workers_travel_to_dest++;
            break;
        case NPC_STATE_UNLOADING:
            out->workers_unloading++;
            break;
        case NPC_STATE_RETURN_TO_STATION:
            out->workers_returning++;
            break;
        case NPC_STATE_IDLE:
        case NPC_STATE_MINING:
        case NPC_STATE_DOCKED:
        default:
            break;
        }
        if (npc->ship->towed_scaffold >= 0)
            out->workers_towing_scaffold++;
        finished_cargo = sr_npc_finished_cargo_total(npc);
        if (finished_cargo > 0.01f) {
            out->workers_with_finished_cargo++;
            out->worker_finished_cargo_units += finished_cargo;
        }
        out->npc_known_contracts +=
            knowledge_view_contract_count(&npc->ship->knowledge);
        out->npc_knowledge_items += sr_clamped_u8_count(
            npc->ship->knowledge.count, KNOWLEDGE_VIEW_MAX_CAP);
        if (npc->hnn_market_mem.experience_count > 0) {
            out->npc_hnn_market_stored += npc->hnn_market_mem.experience_count;
        }
        if (npc->hnn_mem.experience_count > 0) {
            out->npc_hnn_flight_stored += npc->hnn_mem.experience_count;
        }
        sr_track_hnn_load(&out->max_npc_market_load, &npc->hnn_market_mem);
        sr_track_hnn_load(&out->max_npc_flight_load, &npc->hnn_mem);

        diag_count = sr_clamped_u8_count(npc->job_diag_count, 4);
        out->worker_diag_rows += diag_count;
        for (int j = 0; j < diag_count; j++) {
            bool selected = npc->job_diag_selected[j] >= 200;
            bool hologram = npc->job_diag_factor_hologram[j] > 0;
            if (selected)
                out->worker_selected_rows++;
            if (hologram)
                out->worker_hologram_rows++;
            if (selected)
                sr_count_selected_job(out, npc->job_diag_kind[j], hologram);
        }
    }
}

static float sr_feature_clamp(float value, float lo, float hi)
{
    if (!isfinite(value)) return 0.0f;
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void sr_hnn_fill_features(const world_t *w,
                                 const server_player_t *sp,
                                 vec2 goal,
                                 int action,
                                 hnn_pilot_features_t *out)
{
    const ship_t *ship;
    const sr_action_def_t *def;
    vec2 to_goal;
    vec2 forward;
    vec2 right;
    nav_path_t *path;
    const hull_def_t *hull;
    float dist;
    float desired;
    float heading_error;
    float speed;
    float fwd_speed;
    float lat_speed;
    float brake_dist;
    float ship_radius;
    float max_hull;
    float fwd_clear;
    float left_clear;
    float right_clear;
    float target_dist;

    if (!w || !sp || !out) return;
    ship = sp->ship;
    def = (action >= 0 && action < SR_ACTION_COUNT)
        ? &SR_ACTIONS[action]
        : &SR_ACTIONS[0];
    memset(out, 0, sizeof(*out));

    to_goal = v2_sub(goal, ship->pos);
    dist = v2_len(to_goal);
    desired = dist > 0.001f
        ? fixp_atan2f(to_goal.y, to_goal.x)
        : ship->angle;
    heading_error = wrap_angle(desired - ship->angle);
    forward = v2_from_angle(ship->angle);
    right = v2(-forward.y, forward.x);
    speed = v2_len(ship->vel);
    fwd_speed = v2_dot(ship->vel, forward);
    lat_speed = v2_dot(ship->vel, right);
    brake_dist = (speed * speed) / (2.0f * SHIP_BRAKE);
    hull = ship_hull_def(ship);
    ship_radius = hull ? hull->ship_radius : 16.0f;
    max_hull = ship_max_hull(ship);

    fwd_clear = nav_forward_clearance(w, ship->pos, ship->vel,
                                      ship_radius, ship->angle);
    left_clear = nav_forward_clearance(w, ship->pos, ship->vel,
                                       ship_radius, ship->angle + 0.7f);
    right_clear = nav_forward_clearance(w, ship->pos, ship->vel,
                                        ship_radius, ship->angle - 0.7f);
    target_dist = sr_feature_clamp(dist / 6000.0f, 0.0f, 1.0f);
    path = nav_player_path(sp->id);

    out->target_dist = target_dist;
    out->heading_error = sr_feature_clamp(heading_error / PI_F, -1.0f, 1.0f);
    out->heading_cos = fixp_cosf(heading_error);
    out->heading_sin = fixp_sinf(heading_error);
    out->speed = sr_feature_clamp(speed / 350.0f, 0.0f, 1.0f);
    out->forward_speed = sr_feature_clamp(fwd_speed / 350.0f, -1.0f, 1.0f);
    out->lateral_speed = sr_feature_clamp(lat_speed / 350.0f, -1.0f, 1.0f);
    out->brake_distance = sr_feature_clamp(brake_dist / 700.0f, 0.0f, 1.0f);
    out->fwd_clear = sr_feature_clamp(fwd_clear, 0.0f, 1.0f);
    out->left_clear = sr_feature_clamp(left_clear, 0.0f, 1.0f);
    out->right_clear = sr_feature_clamp(right_clear, 0.0f, 1.0f);
    out->signal_quality = sr_feature_clamp(signal_strength_at(w, ship->pos),
                                           0.0f, 1.0f);
    out->hull_ratio = max_hull > 0.0f
        ? sr_feature_clamp(ship->hull / max_hull, 0.0f, 1.0f)
        : 1.0f;
    out->path_count = path ? sr_feature_clamp((float)path->count / 16.0f,
                                              0.0f, 1.0f) : 0.0f;
    out->path_current = path ? sr_feature_clamp((float)path->current / 16.0f,
                                                0.0f, 1.0f) : 0.0f;
    out->fwd_blocked = out->fwd_clear < 0.15f ? 1.0f : 0.0f;
    out->left_blocked = out->left_clear < 0.15f ? 1.0f : 0.0f;
    out->right_blocked = out->right_clear < 0.15f ? 1.0f : 0.0f;
    out->goal_close = 1.0f - target_dist;
    out->action_delta_turn = (float)def->turn;
    out->action_delta_thrust = (float)def->thrust;
    out->action_is_none = action == 0 ? 1.0f : 0.0f;
    out->action_is_reverse = def->thrust < 0 ? 1.0f : 0.0f;
    out->composite_dot = (float)def->turn * out->heading_sin;
}

static void sr_hnn_store_observation(const world_t *w,
                                     const server_player_t *sp,
                                     vec2 goal,
                                     int action,
                                     int label_shift,
                                     hnn_memory_t *mem,
                                     hnn_holonet_t *net,
                                     const hnn_action_table_t *actions)
{
    hnn_pilot_features_t route_features;
    hnn_pilot_features_t features;
    float route_vec[HNN_DIM];
    float state_vec[HNN_DIM];
    if (!mem || !actions || action < 0 || action >= HNN_ACTION_COUNT) return;
    int stored_action = (action + label_shift) % HNN_ACTION_COUNT;
    sr_hnn_fill_features(w, sp, goal, 0, &route_features);
    sr_hnn_fill_features(w, sp, goal, action, &features);
    hnn_encode_state(&route_features, route_vec);
    hnn_encode_state(&features, state_vec);
    hnn_memory_store(mem, state_vec, actions->vecs[stored_action]);
    if (net)
        hnn_holonet_store(net, route_vec, state_vec,
                          actions->vecs[stored_action]);
}

static bool sr_hnn_action_allowed(const hnn_pilot_features_t *state,
                                  const sr_action_def_t *action,
                                  int action_index)
{
    if (!state || !action || action_index < 0 ||
        action_index >= HNN_ACTION_COUNT) {
        return false;
    }

    const float slow_speed = 20.0f / 350.0f;
    bool forward_blocked =
        state->fwd_blocked > 0.5f || state->fwd_clear < 0.15f;
    bool positive_turn_blocked =
        state->left_blocked > 0.5f || state->left_clear < 0.12f;
    bool negative_turn_blocked =
        state->right_blocked > 0.5f || state->right_clear < 0.12f;

    if (forward_blocked) {
        if (action->thrust > 0) return false;
        if (state->speed > slow_speed && action->thrust >= 0) return false;
    }

    if (action->thrust >= 0) {
        if (action->turn > 0 && positive_turn_blocked) return false;
        if (action->turn < 0 && negative_turn_blocked) return false;
    }

    if (state->target_dist > (450.0f / 6000.0f) &&
        state->speed < slow_speed) {
        if (action_index == 0 || action->thrust < 0) return false;
        if (fabsf(state->heading_error) > 0.35f &&
            action->thrust > 0 &&
            action->turn == 0) {
            return false;
        }
    }

    return true;
}

static uint16_t sr_hnn_allowed_mask(const hnn_pilot_features_t *state,
                                    uint8_t allowed[HNN_ACTION_COUNT])
{
    uint16_t mask = 0;
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        bool ok = sr_hnn_action_allowed(state, &SR_ACTIONS[i], i);
        if (allowed) allowed[i] = ok ? 1u : 0u;
        if (ok) mask |= (uint16_t)(1u << i);
    }
    return mask;
}

static int sr_hnn_best_allowed_action(const float scores[HNN_ACTION_COUNT],
                                      const uint8_t allowed[HNN_ACTION_COUNT],
                                      float *out_score,
                                      float *out_margin)
{
    int best = -1;
    float best_score = -INFINITY;
    float second_score = -INFINITY;
    if (!scores || !allowed) {
        if (out_score) *out_score = 0.0f;
        if (out_margin) *out_margin = 0.0f;
        return -1;
    }
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        if (!allowed[i] || !isfinite(scores[i])) continue;
        if (scores[i] > best_score) {
            second_score = best_score;
            best_score = scores[i];
            best = i;
        } else if (scores[i] > second_score) {
            second_score = scores[i];
        }
    }
    if (out_score)
        *out_score = isfinite(best_score) ? best_score : 0.0f;
    if (out_margin) {
        *out_margin = (isfinite(best_score) && isfinite(second_score))
            ? best_score - second_score
            : 0.0f;
    }
    return best;
}

static int sr_hnn_teacher_action(
    const hnn_pilot_features_t *state,
    const uint8_t allowed[HNN_ACTION_COUNT],
    uint16_t allowed_mask)
{
    if (!state || !allowed || allowed_mask == 0) return 0;
    float heading_error = state->heading_error * PI_F;
    int preferred = 1;
    if (fabsf(heading_error) > 0.3f)
        preferred = heading_error > 0.0f ? 6 : 5;
    if (allowed[preferred]) return preferred;
    int turn_only = heading_error >= 0.0f ? 3 : 2;
    if (allowed[turn_only]) return turn_only;
    if (allowed[4]) return 4;
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        if (allowed[i]) return i;
    }
    return 0;
}

static void sr_hnn_evaluate_branch(const world_t *w,
                                   const server_player_t *sp,
                                   vec2 goal,
                                   int candidate,
                                   hnn_memory_t *mem,
                                   hnn_holonet_t *net,
                                   const hnn_action_table_t *actions,
                                   int cleanup_steps,
                                   hnn_confidence_mode_t confidence_mode,
                                   sr_hnn_eval_t *out)
{
    hnn_pilot_features_t state_only;
    float margin = 0.0f;
    float fidelity = 0.0f;
    float allowed_margin = 0.0f;
    float top_allowed_score = 0.0f;
    uint8_t allowed[HNN_ACTION_COUNT];
    int top;
    int top_allowed;
    int rank = 1;
    int allowed_rank = 1;

    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!mem || !actions || candidate < 0 || candidate >= HNN_ACTION_COUNT) {
        return;
    }

    sr_hnn_fill_features(w, sp, goal, 0, &state_only);
    if (net) {
        out->holonet_active_count = hnn_holonet_active_count(net);
        out->holonet_last_route = net->last_route;
        out->holonet_scored_count = net->last_scored_count;
        out->holonet_route_similarity = net->last_route_similarity;
        out->holonet_contract = hnn_holonet_contract(net);
    }
    if (net && hnn_holonet_active_count(net) > 0) {
        top = hnn_holonet_score_actions(net, actions, &state_only, out->scores,
                                        &margin, &fidelity, cleanup_steps);
        out->holonet_enabled = top >= 0;
        out->holonet_active_count = hnn_holonet_active_count(net);
        out->holonet_last_route = net->last_route;
        out->holonet_scored_count = net->last_scored_count;
        out->holonet_route_similarity = net->last_route_similarity;
        out->holonet_contract = hnn_holonet_contract(net);
    } else {
        top = hnn_score_actions(mem, actions, &state_only, out->scores,
                                &margin, &fidelity, cleanup_steps);
    }
    if (top < 0) {
        top = hnn_score_actions(mem, actions, &state_only, out->scores,
                                &margin, &fidelity, cleanup_steps);
    }
    if (top < 0) top = 0;
    out->allowed_mask = sr_hnn_allowed_mask(&state_only, allowed);
    top_allowed = sr_hnn_best_allowed_action(
        out->scores, allowed, &top_allowed_score, &allowed_margin);
    if (top_allowed < 0) top_allowed = top;
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        if (out->scores[i] > out->scores[candidate]) rank++;
        if (allowed[candidate] && allowed[i] &&
            out->scores[i] > out->scores[candidate]) {
            allowed_rank++;
        }
    }

    mem->last_retrieval_similarity = out->scores[top_allowed];
    mem->last_margin = allowed_margin;

    out->enabled = true;
    out->top_action = top;
    out->top_allowed_action = top_allowed;
    out->candidate_rank = rank;
    out->candidate_allowed = allowed[candidate] != 0;
    out->candidate_allowed_rank = out->candidate_allowed ? allowed_rank : -1;
    out->candidate_score = out->scores[candidate];
    out->top_score = out->scores[top];
    out->top_allowed_score = top_allowed_score;
    out->margin = margin;
    out->allowed_margin = allowed_margin;
    out->trace_fidelity = fidelity;
    out->contract = hnn_memory_contract(mem);
    out->confidence_mode = confidence_mode;
    out->teacher_action = sr_hnn_teacher_action(
        &state_only, allowed, out->allowed_mask);
    out->confidence = hnn_confidence_evaluate(
        hnn_backend_active_kind(),
        &out->contract,
        top_allowed_score,
        allowed_margin,
        top_allowed >= 0 && top_allowed < HNN_ACTION_COUNT,
        top_allowed >= 0 && top_allowed < HNN_ACTION_COUNT &&
            allowed[top_allowed] != 0);
    out->selected_action = hnn_confidence_select_action(
        confidence_mode, &out->confidence, top_allowed, out->teacher_action);
}

static bool sr_replay_prefix(const sr_config_t *config,
                             world_t *w,
                             server_player_t *sp,
                             vec2 goal,
                             hnn_memory_t *hnn_mem,
                             hnn_holonet_t *hnn_net,
                             const hnn_action_table_t *hnn_actions)
{
    for (int i = 0; i < config->prefix_count; i++) {
        if (hnn_mem && hnn_actions) {
            sr_hnn_store_observation(w, sp, goal, config->prefix[i],
                                     config->hnn_label_shift,
                                     hnn_mem, hnn_net, hnn_actions);
        }
        sr_apply_action(sp, config->prefix[i]);
        world_sim_step(w, SIM_DT);
        for (int e = 0; e < w->events.count; e++) {
            if (w->events.events[e].player_id == sp->id &&
                w->events.events[e].type == SIM_EVENT_DEATH) {
                return false;
            }
        }
        if (sp->ship->hull <= 0.0f) return false;
    }
    return true;
}

static bool sr_run_branch(const sr_config_t *config, int candidate, sr_result_t *out)
{
    world_t *w = NULL;
    server_player_t *sp = NULL;
    vec2 spawn = v2(0.0f, 0.0f);
    vec2 goal = v2(0.0f, 0.0f);
    hnn_memory_t hnn_mem;
    hnn_holonet_t hnn_net;
    hnn_action_table_t hnn_actions;
    hnn_memory_t *hnn_mem_ptr = NULL;
    hnn_holonet_t *hnn_net_ptr = NULL;
    hnn_action_table_t *hnn_actions_ptr = NULL;
    sha256_ctx_t event_hash;
    sr_chain_fixture_t chain_fixture = {0};
    bool world_ready = false;
    sr_route_cell_t route_cells[SR_ROUTE_CELL_CAP];
    sr_route_cell_t previous_cell = {0};
    sr_worker_route_visit_t worker_route_visits[SR_ROUTE_CELL_CAP];
    sr_route_cell_t worker_previous_cells[MAX_NPC_SHIPS];
    bool worker_previous_cell_set[MAX_NPC_SHIPS] = {false};
    vec2 previous_pos = v2(0.0f, 0.0f);
    float previous_dist = 0.0f;
    int route_cell_count = 0;
    int worker_route_visit_count = 0;
    int consecutive_stuck_ticks = 0;
    bool stuck_latched = false;
    uint32_t episode_start_tick = 0;
    uint64_t contract_decisions_start = 0;
    uint64_t contract_teacher_start = 0;
    uint64_t worker_decisions_start = 0;
    uint64_t worker_teacher_start = 0;
    bool ok = false;

    memset(out, 0, sizeof(*out));
    out->candidate = candidate;
    out->prefix_ticks = config->prefix_count;
    out->horizon_ticks = config->horizon_ticks;
    out->goal_completion_tick = -1;
    out->contract_completion_tick = -1;
    out->repair_completion_tick = -1;
    out->ai.branch.first_worker_completion_tick = -1;

    w = (world_t *)calloc(1, sizeof(*w));
    if (!w) return false;

    if (!sr_setup_world(config, w, &sp, &spawn, &goal)) {
        goto cleanup;
    }
    world_ready = true;
    if (!sr_receipt_trust_known_vector(
            w, config->station, &out->receipt_trust)) {
        goto cleanup;
    }
    if (!sr_chain_fixture_begin(config, &chain_fixture)) {
        goto cleanup;
    }
    if (!sr_setup_provenance_script(config, w, sp)) {
        goto cleanup;
    }
    sr_collect_eval_summary(w, &out->evaluation);
    (void)spawn;

    if (config->hnn_trace) {
        hnn_memory_init(&hnn_mem);
        hnn_holonet_init(&hnn_net);
        hnn_action_table_init(&hnn_actions);
        hnn_mem_ptr = &hnn_mem;
        hnn_net_ptr = &hnn_net;
        hnn_actions_ptr = &hnn_actions;
    }

    if (!sr_replay_prefix(config, w, sp, goal, hnn_mem_ptr, hnn_net_ptr,
                          hnn_actions_ptr)) {
        goto cleanup;
    }

    out->start_station = sp->current_station;
    out->start_pos = sp->ship->pos;
    out->start_dist = v2_len(v2_sub(goal, sp->ship->pos));
    out->start_hull = sp->ship->hull;
    out->start_cargo = ship_total_cargo(sp->ship);
    out->start_balance = sr_player_station_balance(w, sp);
    out->start_active_contracts = sr_active_contract_count(w);
    sr_collect_lineage_summary(sp->ship, &out->start_lineage);
    episode_start_tick = w->tick;
    contract_decisions_start =
        signal_intelligence_contract_decision_count();
    contract_teacher_start =
        signal_intelligence_contract_teacher_decision_count();
    worker_decisions_start =
        signal_intelligence_npc_worker_decision_count();
    worker_teacher_start =
        signal_intelligence_npc_worker_teacher_decision_count();
    sr_state_hash(w, sp, out->prefix_state_hash);
    signal_authoritative_state_digest(w, out->prefix_state_root);

    sha256_init(&event_hash);
    sha256_update(&event_hash, SR_PUBLIC_EVENT_HASH_DOMAIN,
                  sizeof(SR_PUBLIC_EVENT_HASH_DOMAIN) - 1u);
    if (!sr_run_provenance_script(config, w, sp, &out->events, &event_hash)) {
        goto cleanup;
    }
    if (config->provenance_script == SR_PROVENANCE_SCRIPT_WORKER_TOW_HNN ||
        config->provenance_script == SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN) {
        out->ai.branch.first_worker_completion_tick =
            (int)(w->tick - episode_start_tick);
    }
    if (config->hnn_trace) {
        vec2 hnn_query_goal = config->hnn_query_goal_set
            ? config->hnn_query_goal : goal;
        sr_hnn_evaluate_branch(w, sp, hnn_query_goal, candidate,
                               hnn_mem_ptr, hnn_net_ptr, hnn_actions_ptr,
                               config->hnn_cleanup_steps,
                               config->hnn_confidence_mode,
                               &out->hnn);
    }
    previous_pos = sp->ship->pos;
    previous_dist = v2_len(v2_sub(goal, previous_pos));
    out->route_start_dist = previous_dist;
    previous_cell = sr_route_cell_for(previous_pos);
    route_cells[route_cell_count++] = previous_cell;
    if (previous_dist <= SR_GOAL_COMPLETION_RADIUS)
        out->goal_completion_tick = 0;
    if (config->active_workers) {
        sr_ai_summary_t sample;
        sr_collect_ai_summary(w, &sample);
        sr_ai_branch_observe(
            &out->ai, &sample, (int)(w->tick - episode_start_tick));
        sr_observe_worker_routes(
            w, worker_route_visits, &worker_route_visit_count,
            worker_previous_cells, worker_previous_cell_set,
            &out->ai.branch);
    }
    for (int i = 0; i < config->horizon_ticks; i++) {
        sr_apply_action(sp, candidate);
        world_sim_step(w, SIM_DT);
        out->ticks_executed = i + 1;
        sr_accumulate_events(w, &out->events, &event_hash);
        vec2 current_pos = sp->ship->pos;
        float step_distance = v2_len(v2_sub(current_pos, previous_pos));
        float current_dist = v2_len(v2_sub(goal, current_pos));
        out->travel_distance += step_distance;
        if (step_distance <= 0.05f &&
            fabsf(current_dist - previous_dist) <= 0.05f) {
            consecutive_stuck_ticks++;
            if (consecutive_stuck_ticks >= SR_STUCK_TICK_THRESHOLD) {
                out->stuck_ticks++;
                stuck_latched = true;
            }
        } else {
            if (stuck_latched && step_distance > 0.5f)
                out->recovery_events++;
            consecutive_stuck_ticks = 0;
            stuck_latched = false;
        }
        sr_route_cell_t current_cell = sr_route_cell_for(current_pos);
        if (!sr_route_cell_equal(current_cell, previous_cell)) {
            if (sr_route_observe_cell(
                    route_cells, &route_cell_count, current_cell) &&
                out->loop_revisits < 65535) {
                out->loop_revisits++;
            }
            previous_cell = current_cell;
        }
        if (out->goal_completion_tick < 0 &&
            current_dist <= SR_GOAL_COMPLETION_RADIUS) {
            out->goal_completion_tick = i + 1;
        }
        previous_pos = current_pos;
        previous_dist = current_dist;
        if (config->active_workers) {
            sr_ai_summary_t sample;
            sr_collect_ai_summary(w, &sample);
            sr_ai_branch_observe(
                &out->ai, &sample, (int)(w->tick - episode_start_tick));
            sr_observe_worker_routes(
                w, worker_route_visits, &worker_route_visit_count,
                worker_previous_cells, worker_previous_cell_set,
                &out->ai.branch);
        }
        if (out->events.death_events > 0 || sp->ship->hull <= 0.0f) {
            break;
        }
    }
    sha256_final(&event_hash, out->event_hash);

    out->end_pos = sp->ship->pos;
    out->end_vel = sp->ship->vel;
    out->end_angle = sp->ship->angle;
    out->end_speed = v2_len(sp->ship->vel);
    out->end_hull = sp->ship->hull;
    out->end_dist = v2_len(v2_sub(goal, sp->ship->pos));
    out->progress = out->start_dist - out->end_dist;
    out->hull_loss = out->start_hull - out->end_hull;
    if (out->hull_loss < 0.0f) out->hull_loss = 0.0f;
    out->end_cargo = ship_total_cargo(sp->ship);
    out->end_balance = sr_player_station_balance(w, sp);
    out->end_docked = sp->docked;
    out->end_current_station = sp->current_station;
    out->end_manifest_count = sp->ship->manifest.count;
    out->end_active_contracts = sr_active_contract_count(w);
    sr_collect_lineage_summary(sp->ship, &out->end_lineage);
    out->contract_decisions =
        signal_intelligence_contract_decision_count() -
        contract_decisions_start;
    out->contract_teacher_decisions =
        signal_intelligence_contract_teacher_decision_count() -
        contract_teacher_start;
    out->worker_decisions =
        signal_intelligence_npc_worker_decision_count() -
        worker_decisions_start;
    out->worker_teacher_decisions =
        signal_intelligence_npc_worker_teacher_decision_count() -
        worker_teacher_start;
    if (out->events.first_contract_complete_tick_set) {
        out->contract_completion_tick =
            (int)(out->events.first_contract_complete_tick -
                  episode_start_tick);
    }
    if (out->events.first_repair_tick_set) {
        out->repair_completion_tick =
            (int)(out->events.first_repair_tick - episode_start_tick);
    }
    {
        float route_progress = out->route_start_dist - out->end_dist;
        if (route_progress > 0.0f && out->travel_distance > 0.0001f) {
            out->route_efficiency =
                sr_feature_clamp(
                    route_progress / out->travel_distance, 0.0f, 1.0f);
        }
    }
    out->utility = ((double)out->progress / 1000.0) -
                   ((double)out->hull_loss * 0.45) -
                   ((double)out->events.damage_amount * 0.25) -
                   ((double)out->events.damage_events * 2.0) -
                   ((double)out->events.death_events * 80.0);
    if (config->active_workers) {
        sr_ai_branch_summary_t branch = out->ai.branch;
        sr_collect_ai_summary(w, &out->ai);
        out->ai.branch = branch;
    }
    sr_state_hash(w, sp, out->state_hash);
    signal_authoritative_state_digest(w, out->state_root);
    out->ok = true;
    ok = true;

cleanup:
    sr_chain_fixture_end(&chain_fixture, world_ready ? w : NULL);
    if (world_ready) world_cleanup(w);
    free(w);
    return ok;
}

static void sr_json_hash(FILE *out, const char *key, const uint8_t hash[32])
{
    char hex[65];
    sr_hex(hash, hex);
    fprintf(out, "\"%s\":\"%s\"", key, hex);
}

static void sr_json_float(FILE *out, float value)
{
    if (isfinite(value)) fprintf(out, "%.9f", value);
    else fprintf(out, "null");
}

static void sr_write_hnn_contract(FILE *out,
                                  const hnn_memory_contract_t *contract)
{
    fprintf(out,
            "{\"dim\":%d,"
            "\"seed\":\"%016" PRIx64 "\","
            "\"keygen_version\":%u,"
            "\"encoder_version\":%u,"
            "\"action_vocabulary_hash\":\"%016" PRIx64 "\","
            "\"trace_format_version\":%u,"
            "\"stored_count\":%d,"
            "\"capacity_load\":",
            contract->dim,
            contract->seed,
            contract->keygen_version,
            contract->encoder_version,
            contract->action_vocabulary_hash,
            contract->trace_format_version,
            contract->stored_count);
    sr_json_float(out, contract->capacity_load);
    fprintf(out, ",\"fidelity_estimate\":");
    sr_json_float(out, contract->fidelity_estimate);
    fprintf(out, ",\"last_margin\":");
    sr_json_float(out, contract->last_margin);
    fprintf(out, "}");
}

static void sr_write_hnn_backend(FILE *out)
{
    hnn_backend_metadata_t metadata = hnn_backend_metadata();
    fprintf(out,
            ",\"hnn_backend\":{"
            "\"schema\":\"signal.hnn_backend.v1\","
            "\"active_library\":\"%s\","
            "\"active_library_version\":\"%s\","
            "\"active_abi_version\":%u,"
            "\"active_backend\":\"%s\","
            "\"dimension\":%d,"
            "\"active_source_revision\":\"%s\","
            "\"scratch_bytes\":%zu,"
            "\"liblecore_pin\":{"
            "\"compiled\":%s,"
            "\"version\":\"%s\","
            "\"abi_version\":%u,"
            "\"source_revision\":\"%s\","
            "\"source_checksum\":\"%s\"}}",
            metadata.active_library,
            metadata.active_library_version,
            metadata.active_abi_version,
            metadata.active_backend,
            metadata.dimension,
            metadata.active_source_revision,
            metadata.scratch_bytes,
            metadata.liblecore_compiled ? "true" : "false",
            metadata.liblecore_version,
            metadata.liblecore_abi_version,
            metadata.liblecore_source_revision,
            metadata.liblecore_source_checksum);
}

static void sr_write_ai_summary(FILE *out, const sr_ai_summary_t *ai)
{
    fprintf(out,
            ",\"ai\":{\"schema\":\"signal.replay_ai_memory.v4\","
            "\"active_npcs\":%d,"
            "\"worker_diag_rows\":%d,"
            "\"worker_selected_rows\":%d,"
            "\"worker_hologram_rows\":%d,"
            "\"worker_mine_assignments\":%d,"
            "\"worker_hologram_mine_assignments\":%d,"
            "\"worker_haul_assignments\":%d,"
            "\"worker_hologram_haul_assignments\":%d,"
            "\"worker_tow_assignments\":%d,"
            "\"worker_hologram_tow_assignments\":%d,"
            "\"worker_delivery_assignments\":%d,"
            "\"worker_hologram_delivery_assignments\":%d,"
            "\"worker_scout_assignments\":%d,"
            "\"worker_hologram_scout_assignments\":%d,"
            "\"worker_repair_assignments\":%d,"
            "\"worker_hologram_repair_assignments\":%d,"
            "\"workers_travel_to_pickup\":%d,"
            "\"workers_travel_to_dest\":%d,"
            "\"workers_unloading\":%d,"
            "\"workers_returning\":%d,"
            "\"workers_towing_scaffold\":%d,"
            "\"workers_with_finished_cargo\":%d,"
            "\"scaffolds_loose\":%d,"
            "\"scaffolds_towing\":%d,"
            "\"scaffolds_towed_by_worker\":%d,"
            "\"scaffolds_snapping\":%d,"
            "\"scaffolds_placed\":%d,"
            "\"npc_delivery_shipments_active\":%d,"
            "\"npc_delivery_shipments_picked_up\":%d,"
            "\"npc_delivery_shipments_delivered\":%d,"
            "\"npc_delivery_shipments_cleared\":%d,"
            "\"npc_delivery_shipments_defaulted\":%d,"
            "\"npc_delivery_shipments_black_market_sold\":%d,"
            "\"npc_known_contracts\":%d,"
            "\"npc_knowledge_items\":%d,"
            "\"station_known_contracts\":%d,"
            "\"station_knowledge_items\":%d,"
            "\"station_remote_known_contracts\":%d,"
            "\"station_remote_market_memory_items\":%d,"
            "\"npc_hnn_market_stored\":%d,"
            "\"station_hnn_market_stored\":%d,"
            "\"npc_hnn_flight_stored\":%d,"
            "\"station_hnn_experience_stored\":%d,"
            "\"station_hnn_market_versions\":%d,"
            "\"station_hnn_experience_versions\":%d,"
            "\"signal_field_occupied_slots\":%d,"
            "\"signal_field_capacity_slots\":%d,"
            "\"signal_field_noisy_station_cells\":%d,"
            "\"worker_finished_cargo_units\":",
            ai->active_npcs,
            ai->worker_diag_rows,
            ai->worker_selected_rows,
            ai->worker_hologram_rows,
            ai->worker_mine_assignments,
            ai->worker_hologram_mine_assignments,
            ai->worker_haul_assignments,
            ai->worker_hologram_haul_assignments,
            ai->worker_tow_assignments,
            ai->worker_hologram_tow_assignments,
            ai->worker_delivery_assignments,
            ai->worker_hologram_delivery_assignments,
            ai->worker_scout_assignments,
            ai->worker_hologram_scout_assignments,
            ai->worker_repair_assignments,
            ai->worker_hologram_repair_assignments,
            ai->workers_travel_to_pickup,
            ai->workers_travel_to_dest,
            ai->workers_unloading,
            ai->workers_returning,
            ai->workers_towing_scaffold,
            ai->workers_with_finished_cargo,
            ai->scaffolds_loose,
            ai->scaffolds_towing,
            ai->scaffolds_towed_by_worker,
            ai->scaffolds_snapping,
            ai->scaffolds_placed,
            ai->npc_delivery_shipments_active,
            ai->npc_delivery_shipments_picked_up,
            ai->npc_delivery_shipments_delivered,
            ai->npc_delivery_shipments_cleared,
            ai->npc_delivery_shipments_defaulted,
            ai->npc_delivery_shipments_black_market_sold,
            ai->npc_known_contracts,
            ai->npc_knowledge_items,
            ai->station_known_contracts,
            ai->station_knowledge_items,
            ai->station_remote_known_contracts,
            ai->station_remote_market_memory_items,
            ai->npc_hnn_market_stored,
            ai->station_hnn_market_stored,
            ai->npc_hnn_flight_stored,
            ai->station_hnn_experience_stored,
            ai->station_hnn_market_versions,
            ai->station_hnn_experience_versions,
            ai->signal_field_occupied_slots,
            ai->signal_field_capacity_slots,
            ai->signal_field_noisy_station_cells);
    sr_json_float(out, ai->worker_finished_cargo_units);
    fprintf(out, ",\"signal_field_load\":");
    sr_json_float(out, ai->signal_field_load);
    fprintf(out, ",\"signal_field_max_strength\":");
    sr_json_float(out, ai->signal_field_max_strength);
    fprintf(out, ",\"signal_field_min_margin\":");
    sr_json_float(out, ai->signal_field_min_margin);
    fprintf(out, ",\"signal_field_min_snr\":");
    sr_json_float(out, ai->signal_field_min_snr);
    fprintf(out, ",\"max_npc_market_load\":");
    sr_json_float(out, ai->max_npc_market_load);
    fprintf(out, ",\"max_station_market_load\":");
    sr_json_float(out, ai->max_station_market_load);
    fprintf(out, ",\"max_npc_flight_load\":");
    sr_json_float(out, ai->max_npc_flight_load);
    fprintf(out, ",\"max_station_experience_load\":");
    sr_json_float(out, ai->max_station_experience_load);
    fprintf(out,
            ",\"branch_active_ticks\":%d,"
            "\"worker_selected_rows_peak\":%d,"
            "\"worker_hologram_rows_peak\":%d,"
            "\"worker_assignment_ticks\":%d,"
            "\"worker_hologram_assignment_ticks\":%d,"
            "\"worker_mine_assignment_ticks\":%d,"
            "\"worker_haul_assignment_ticks\":%d,"
            "\"worker_tow_assignment_ticks\":%d,"
            "\"worker_delivery_assignment_ticks\":%d,"
            "\"worker_scout_assignment_ticks\":%d,"
            "\"worker_repair_assignment_ticks\":%d,"
            "\"worker_motion_ticks\":%d,"
            "\"worker_route_support_ticks\":%d,"
            "\"worker_cargo_ticks\":%d,"
            "\"worker_scaffold_motion_ticks\":%d,"
            "\"worker_delivery_shipment_ticks\":%d,"
            "\"worker_useful_outcome_ticks\":%d",
            ai->branch.active_ticks,
            ai->branch.worker_selected_rows_peak,
            ai->branch.worker_hologram_rows_peak,
            ai->branch.worker_assignment_ticks,
            ai->branch.worker_hologram_assignment_ticks,
            ai->branch.worker_mine_assignment_ticks,
            ai->branch.worker_haul_assignment_ticks,
            ai->branch.worker_tow_assignment_ticks,
            ai->branch.worker_delivery_assignment_ticks,
            ai->branch.worker_scout_assignment_ticks,
            ai->branch.worker_repair_assignment_ticks,
            ai->branch.worker_motion_ticks,
            ai->branch.worker_route_support_ticks,
            ai->branch.worker_cargo_ticks,
            ai->branch.worker_scaffold_motion_ticks,
            ai->branch.worker_delivery_shipment_ticks,
            ai->branch.worker_useful_outcome_ticks);
    fprintf(out, "}");
}

static void sr_write_hnn_eval(FILE *out,
                              const sr_config_t *config,
                              const sr_hnn_eval_t *hnn)
{
    fprintf(out,
            ",\"hnn\":{\"schema\":\"signal.replay_hnn_eval.v1\","
            "\"null_control\":{\"query_goal_unrelated\":%s,"
            "\"label_shift\":%d,\"overloaded\":%s},"
            "\"top_action\":%d,"
            "\"top_action_name\":\"%s\","
            "\"top_allowed_action\":%d,"
            "\"top_allowed_action_name\":\"%s\","
            "\"allowed_mask\":\"0x%03x\","
            "\"candidate_rank\":%d,"
            "\"candidate_allowed\":%s,"
            "\"candidate_allowed_rank\":%d,"
            "\"candidate_score\":",
            config->hnn_query_goal_set ? "true" : "false",
            config->hnn_label_shift,
            hnn->contract.capacity_load > 1.0f ? "true" : "false",
            hnn->top_action,
            SR_ACTIONS[hnn->top_action].name,
            hnn->top_allowed_action,
            SR_ACTIONS[hnn->top_allowed_action].name,
            (unsigned)hnn->allowed_mask,
            hnn->candidate_rank,
            hnn->candidate_allowed ? "true" : "false",
            hnn->candidate_allowed_rank);
    sr_json_float(out, hnn->candidate_score);
    fprintf(out, ",\"top_score\":");
    sr_json_float(out, hnn->top_score);
    fprintf(out, ",\"top_allowed_score\":");
    sr_json_float(out, hnn->top_allowed_score);
    fprintf(out, ",\"margin\":");
    sr_json_float(out, hnn->margin);
    fprintf(out, ",\"allowed_margin\":");
    sr_json_float(out, hnn->allowed_margin);
    fprintf(out, ",\"trace_fidelity\":");
    sr_json_float(out, hnn->trace_fidelity);
    fprintf(out,
            ",\"confidence\":{\"mode\":\"%s\","
            "\"accepted\":%s,\"reason\":\"%s\","
            "\"min_score\":",
            hnn_confidence_mode_name(hnn->confidence_mode),
            hnn->confidence.accepted ? "true" : "false",
            hnn_confidence_reason_name(hnn->confidence.reason));
    sr_json_float(out, hnn->confidence.thresholds.min_score);
    fprintf(out, ",\"min_margin\":");
    sr_json_float(out, hnn->confidence.thresholds.min_margin);
    fprintf(out, ",\"max_capacity_load\":");
    sr_json_float(out, hnn->confidence.thresholds.max_capacity_load);
    fprintf(out,
            ",\"teacher_action\":%d,\"teacher_action_name\":\"%s\","
            "\"selected_action\":%d,\"selected_action_name\":\"%s\"}",
            hnn->teacher_action,
            SR_ACTIONS[hnn->teacher_action].name,
            hnn->selected_action,
            SR_ACTIONS[hnn->selected_action].name);
    fprintf(out, ",\"contract\":");
    sr_write_hnn_contract(out, &hnn->contract);
    fprintf(out,
            ",\"holonet\":{\"enabled\":%s,"
            "\"active_count\":%d,"
            "\"last_route\":%d,"
            "\"scored_count\":%d,"
            "\"route_similarity\":",
            hnn->holonet_enabled ? "true" : "false",
            hnn->holonet_active_count,
            hnn->holonet_last_route,
            hnn->holonet_scored_count);
    sr_json_float(out, hnn->holonet_route_similarity);
    fprintf(out, ",\"contract\":");
    sr_write_hnn_contract(out, &hnn->holonet_contract);
    fprintf(out, "}");
    fprintf(out, ",\"scores\":[");
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        if (i > 0) fprintf(out, ",");
        fprintf(out, "{\"index\":%d,\"name\":\"%s\",\"allowed\":%s,\"score\":",
                i, SR_ACTIONS[i].name,
                (hnn->allowed_mask & (uint16_t)(1u << i)) ? "true" : "false");
        sr_json_float(out, hnn->scores[i]);
        fprintf(out, "}");
    }
    fprintf(out, "]}");
}

static void sr_write_eval_summary(FILE *out,
                                  const sr_config_t *config,
                                  const sr_eval_summary_t *evaluation)
{
    const char *permutation_group =
        config->eval_world == SR_EVAL_WORLD_PERMUTATION_LOW ||
        config->eval_world == SR_EVAL_WORLD_PERMUTATION_HIGH
            ? "relay-slot-permutation-v1"
            : "";
    fprintf(out,
            ",\"evaluation\":{\"schema\":\"%s\","
            "\"corpus_version\":%u,"
            "\"generator_version\":%u,"
            "\"scenario\":\"%s\","
            "\"permutation_group\":\"%s\","
            "\"existing_stations\":%d,"
            "\"active_stations\":%d,"
            "\"generated_outposts\":%d,"
            "\"weak_signal_stations\":%d,"
            "\"disconnected_stations\":%d,"
            "\"scarce_finished_goods\":%d,"
            "\"production_edges\":%d,"
            "\"consumption_edges\":%d,"
            "\"outpost_slots\":[",
            SR_AI_EVAL_SCHEMA,
            SR_AI_EVAL_CORPUS_VERSION,
            SR_AI_EVAL_GENERATOR_VERSION,
            sr_eval_world_name(config->eval_world),
            permutation_group,
            evaluation->existing_stations,
            evaluation->active_stations,
            evaluation->generated_outposts,
            evaluation->weak_signal_stations,
            evaluation->disconnected_stations,
            evaluation->scarce_finished_goods,
            evaluation->production_edges,
            evaluation->consumption_edges);
    for (int i = 0; i < evaluation->outpost_slot_count; i++) {
        fprintf(out, "%s%d", i > 0 ? "," : "",
                evaluation->outpost_slots[i]);
    }
    fprintf(out, "],");
    sr_json_hash(out, "topology_hash", evaluation->topology_hash);
    fprintf(out, ",");
    sr_json_hash(out, "semantic_topology_hash",
                 evaluation->semantic_topology_hash);
    fprintf(out, "}");
}

static void sr_write_optional_tick(FILE *out, int tick)
{
    if (tick >= 0) fprintf(out, "%d", tick);
    else fprintf(out, "null");
}

static void sr_write_lineage_summary(FILE *out,
                                     const sr_lineage_summary_t *lineage)
{
    fprintf(out,
            "{\"manifest_count\":%d,"
            "\"receipt_count\":%d,"
            "\"missing_receipt_chains\":%d,"
            "\"invalid_receipt_chains\":%d,"
            "\"receipt_manifest_parity\":%s,",
            lineage->manifest_count,
            lineage->receipt_count,
            lineage->missing_receipt_chains,
            lineage->invalid_receipt_chains,
            lineage->receipt_manifest_parity ? "true" : "false");
    sr_json_hash(out, "identity_hash", lineage->identity_hash);
    fprintf(out, "}");
}

static void sr_write_outcome_facts(FILE *out, const sr_result_t *r)
{
    bool lineage_integrity_preserved =
        r->start_lineage.receipt_manifest_parity &&
        r->end_lineage.receipt_manifest_parity &&
        r->start_lineage.invalid_receipt_chains == 0 &&
        r->end_lineage.invalid_receipt_chains == 0 &&
        r->start_lineage.missing_receipt_chains == 0 &&
        r->end_lineage.missing_receipt_chains == 0;
    bool identity_unchanged =
        memcmp(r->start_lineage.identity_hash,
               r->end_lineage.identity_hash,
               sizeof(r->start_lineage.identity_hash)) == 0;
    int safety_overrides = r->events.order_rejected_events;
    int worker_completion_tick =
        r->ai.branch.first_worker_completion_tick;
    float worker_route_efficiency = 0.0f;
    if (r->repair_completion_tick >= 0 &&
        (worker_completion_tick < 0 ||
         r->repair_completion_tick < worker_completion_tick)) {
        worker_completion_tick = r->repair_completion_tick;
    }
    if (r->hnn.enabled && !r->hnn.candidate_allowed)
        safety_overrides++;
    if (r->ai.branch.worker_assignment_ticks > 0) {
        worker_route_efficiency = sr_feature_clamp(
            (float)r->ai.branch.worker_useful_outcome_ticks /
                (float)r->ai.branch.worker_assignment_ticks,
            0.0f, 1.0f);
    }

    fprintf(out,
            ",\"outcome_facts\":{\"schema\":\"%s\","
            "\"report_version\":%u,"
            "\"feature_contracts\":{"
            "\"flight\":{\"feature_set\":\"%s\","
            "\"encoder_version\":%u,\"model_loaded\":%s},"
            "\"contract\":{\"feature_set\":\"%s\","
            "\"encoder_version\":%u,\"model_loaded\":%s},"
            "\"worker\":{\"feature_set\":\"%s\","
            "\"encoder_version\":%u,\"model_loaded\":%s}},"
            "\"ticks_executed\":%d,"
            "\"goal_completion_tick\":",
            SR_OUTCOME_FACTS_SCHEMA,
            SR_OUTCOME_REPORT_VERSION,
            signal_intelligence_flight_feature_set(),
            signal_intelligence_flight_feature_encoder_version(),
            signal_intelligence_flight_loaded() ? "true" : "false",
            SIGNAL_CONTRACT_FEATURE_SET,
            (unsigned)SIGNAL_CONTRACT_FEATURE_ENCODER_VERSION,
            signal_intelligence_contract_loaded() ? "true" : "false",
            SIGNAL_NPC_WORKER_FEATURE_SET,
            (unsigned)SIGNAL_NPC_WORKER_FEATURE_ENCODER_VERSION,
            signal_intelligence_npc_worker_loaded() ? "true" : "false",
            r->ticks_executed);
    sr_write_optional_tick(out, r->goal_completion_tick);
    fprintf(out, ",\"contract_completion_tick\":");
    sr_write_optional_tick(out, r->contract_completion_tick);
    fprintf(out, ",\"worker_completion_tick\":");
    sr_write_optional_tick(out, worker_completion_tick);
    fprintf(out,
            ",\"route\":{\"start_distance\":%.3f,"
            "\"end_distance\":%.3f,"
            "\"distance_traveled\":%.3f,"
            "\"progress\":%.3f,"
            "\"efficiency\":%.9f,"
            "\"stuck_ticks\":%d,"
            "\"recovery_events\":%d,"
            "\"loop_revisits\":%d},"
            "\"worker_route\":{\"assignment_ticks\":%d,"
            "\"motion_ticks\":%d,"
            "\"route_support_ticks\":%d,"
            "\"useful_outcome_ticks\":%d,"
            "\"efficiency\":%.9f,"
            "\"stuck_ticks\":%d,"
            "\"recovery_events\":%d,"
            "\"loop_revisits\":%d},"
            "\"safety\":{\"collision_events\":%d,"
            "\"damage_amount\":%.3f,"
            "\"death_events\":%d,"
            "\"safety_overrides\":%d,"
            "\"order_rejected_events\":%d},"
            "\"decisions\":{\"flight\":1,"
            "\"contract\":%" PRIu64 ","
            "\"contract_teacher_fallbacks\":%" PRIu64 ","
            "\"worker\":%" PRIu64 ","
            "\"worker_teacher_fallbacks\":%" PRIu64 "},"
            "\"contracts\":{\"start_active\":%d,"
            "\"end_active\":%d,"
            "\"completed\":%d},"
            "\"station_need\":{\"contract_completions\":%d,"
            "\"sell_events\":%d,"
            "\"sell_value\":%d,"
            "\"delivery_cleared\":%d,"
            "\"repair_events\":%d,"
            "\"scaffolds_placed\":%d},"
            "\"cargo\":{\"start\":",
            r->route_start_dist,
            r->end_dist,
            r->travel_distance,
            r->route_start_dist - r->end_dist,
            r->route_efficiency,
            r->stuck_ticks,
            r->recovery_events,
            r->loop_revisits,
            r->ai.branch.worker_assignment_ticks,
            r->ai.branch.worker_motion_ticks,
            r->ai.branch.worker_route_support_ticks,
            r->ai.branch.worker_useful_outcome_ticks,
            worker_route_efficiency,
            r->ai.branch.worker_stuck_ticks,
            r->ai.branch.worker_recovery_events,
            r->ai.branch.worker_loop_revisits,
            r->events.damage_events,
            r->events.damage_amount,
            r->events.death_events,
            safety_overrides,
            r->events.order_rejected_events,
            r->contract_decisions,
            r->contract_teacher_decisions,
            r->worker_decisions,
            r->worker_teacher_decisions,
            r->start_active_contracts,
            r->end_active_contracts,
            r->events.contract_complete_events,
            r->events.contract_complete_events,
            r->events.sell_events,
            r->events.sell_base,
            r->ai.npc_delivery_shipments_cleared,
            r->events.repair_events,
            r->ai.scaffolds_placed);
    sr_write_lineage_summary(out, &r->start_lineage);
    fprintf(out, ",\"end\":");
    sr_write_lineage_summary(out, &r->end_lineage);
    fprintf(out,
            ",\"identity_unchanged\":%s,"
            "\"lineage_integrity_preserved\":%s}}",
            identity_unchanged ? "true" : "false",
            lineage_integrity_preserved ? "true" : "false");
}

static void sr_write_row(FILE *out, const sr_config_t *config, const sr_result_t *r)
{
    fprintf(out,
            "{\"schema\":\"%s\","
            "\"seed\":%" PRIu32 ","
            "\"station\":%d,"
            "\"provenance_script\":\"%s\","
            "\"prefix_ticks\":%d,"
            "\"horizon_ticks\":%d,"
            "\"candidate\":%d,"
            "\"candidate_name\":\"%s\","
            "\"state_digest_schema\":\"%s\","
            "\"state_digest_version\":%" PRIu32 ","
            "\"public_state_hash_schema\":\"%s\","
            "\"public_state_hash_version\":%" PRIu32 ","
            "\"public_event_hash_schema\":\"%s\","
            "\"public_event_hash_version\":%" PRIu32 ",",
            SR_SCHEMA,
            config->seed,
            config->station,
            sr_provenance_script_name(config->provenance_script),
            r->prefix_ticks,
            r->horizon_ticks,
            r->candidate,
            SR_ACTIONS[r->candidate].name,
            signal_authoritative_state_digest_schema(),
            signal_authoritative_state_digest_version(),
            SR_PUBLIC_STATE_HASH_SCHEMA,
            (uint32_t)SR_PUBLIC_STATE_HASH_VERSION,
            SR_PUBLIC_EVENT_HASH_SCHEMA,
            (uint32_t)SR_PUBLIC_EVENT_HASH_VERSION);
    sr_json_hash(out, "prefix_state_hash", r->prefix_state_hash);
    fprintf(out, ",");
    sr_json_hash(out, "state_hash", r->state_hash);
    fprintf(out, ",");
    sr_json_hash(out, "prefix_state_root", r->prefix_state_root);
    fprintf(out, ",");
    sr_json_hash(out, "state_root", r->state_root);
    fprintf(out, ",");
    sr_json_hash(out, "event_hash", r->event_hash);
    fprintf(out,
            ",\"start_dist\":%.3f"
            ",\"end_dist\":%.3f"
            ",\"progress\":%.3f"
            ",\"start_hull\":%.3f"
            ",\"end_hull\":%.3f"
            ",\"hull_loss\":%.3f"
            ",\"start_cargo\":%.3f"
            ",\"end_cargo\":%.3f"
            ",\"start_balance\":%.3f"
            ",\"end_balance\":%.3f"
            ",\"utility\":%.9f"
            ",\"end_x\":%.3f"
            ",\"end_y\":%.3f"
            ",\"end_vx\":%.3f"
            ",\"end_vy\":%.3f"
            ",\"end_speed\":%.3f"
            ",\"end_angle\":%.6f"
            ",\"end_docked\":%s"
            ",\"end_current_station\":%d"
            ",\"end_manifest_count\":%u"
            ",\"damage_events\":%d"
            ",\"death_events\":%d"
            ",\"dock_events\":%d"
            ",\"launch_events\":%d"
            ",\"pickup_events\":%d"
            ",\"pickup_fragments\":%d"
            ",\"pickup_ore\":%.3f"
            ",\"buy_events\":%d"
            ",\"buy_cost\":%d"
            ",\"buy_quantity\":%d"
            ",\"sell_events\":%d"
            ",\"sell_base\":%d"
            ",\"sell_bonus\":%d"
            ",\"repair_events\":%d"
            ",\"mining_tick_events\":%d"
            ",\"fracture_events\":%d"
            ",\"outpost_placed_events\":%d"
            ",\"scaffold_ready_events\":%d"
            ",\"damage_amount\":%.3f",
            r->start_dist,
            r->end_dist,
            r->progress,
            r->start_hull,
            r->end_hull,
            r->hull_loss,
            r->start_cargo,
            r->end_cargo,
            r->start_balance,
            r->end_balance,
            r->utility,
            r->end_pos.x,
            r->end_pos.y,
            r->end_vel.x,
            r->end_vel.y,
            r->end_speed,
            r->end_angle,
            r->end_docked ? "true" : "false",
            r->end_current_station,
            (unsigned)r->end_manifest_count,
            r->events.damage_events,
            r->events.death_events,
            r->events.dock_events,
            r->events.launch_events,
            r->events.pickup_events,
            r->events.pickup_fragments,
            r->events.pickup_ore,
            r->events.buy_events,
            r->events.buy_cost,
            r->events.buy_quantity,
            r->events.sell_events,
            r->events.sell_base,
            r->events.sell_bonus,
            r->events.repair_events,
            r->events.mining_tick_events,
            r->events.fracture_events,
            r->events.outpost_placed_events,
            r->events.scaffold_ready_events,
            r->events.damage_amount);
    if (config->active_workers && r->ai.enabled) {
        sr_write_ai_summary(out, &r->ai);
    }
    if (config->hnn_trace && r->hnn.enabled) {
        sr_write_hnn_eval(out, config, &r->hnn);
    }
    sr_write_hnn_backend(out);
    sr_write_eval_summary(out, config, &r->evaluation);
    fprintf(out,
            ",\"receipt_trust\":{\"schema\":\"signal.receipt_trust.v1\","
            "\"trusted_smelt\":%d,"
            "\"trusted_craft\":%d,"
            "\"trusted_rotated\":%d,"
            "\"bad_arguments\":%d,"
            "\"broken_chain\":%d,"
            "\"missing_origin\":%d,"
            "\"wrong_event_type\":%d,"
            "\"wrong_cargo\":%d,"
            "\"origin_metadata\":%d,"
            "\"wrong_origin_pin\":%d,"
            "\"wrong_origin_authority\":%d,"
            "\"wrong_origin_authority_lifecycle\":%d,"
            "\"unknown_authority\":%d,"
            "\"untrusted_authority\":%d,"
            "\"revoked_authority\":%d}",
            (int)r->receipt_trust.trusted_smelt,
            (int)r->receipt_trust.trusted_craft,
            (int)r->receipt_trust.trusted_rotated,
            (int)r->receipt_trust.bad_arguments,
            (int)r->receipt_trust.broken_chain,
            (int)r->receipt_trust.missing_origin,
            (int)r->receipt_trust.wrong_event_type,
            (int)r->receipt_trust.wrong_cargo,
            (int)r->receipt_trust.origin_metadata,
            (int)r->receipt_trust.wrong_origin_pin,
            (int)r->receipt_trust.wrong_origin_authority,
            (int)r->receipt_trust.wrong_origin_authority_lifecycle,
            (int)r->receipt_trust.unknown_authority,
            (int)r->receipt_trust.untrusted_authority,
            (int)r->receipt_trust.revoked_authority);
    sr_write_outcome_facts(out, r);
    fprintf(out, ",\"authority\":\"deterministic_seed_prefix_replay\"}\n");
}

int main(int argc, char **argv)
{
    sr_config_t config;
    FILE *out = stdout;
    int emitted = 0;

    if (argc == 2 && strcmp(argv[1], "--self-test-public-hash") == 0) {
        return sr_self_test_public_hash() ? 0 : 1;
    }

    if (!sr_parse_args(argc, argv, &config)) {
        sr_usage(stderr);
        return 2;
    }

    if (config.out_path) {
        out = fopen(config.out_path, "w");
        if (!out) {
            perror("signal_replay: open output");
            return 1;
        }
    }

    chain_log_set_disk_enabled(false);

    for (int candidate = 0; candidate < SR_ACTION_COUNT; candidate++) {
        sr_result_t result;
        if (!config.candidate_enabled[candidate]) continue;
        if (!sr_run_branch(&config, candidate, &result)) {
            fprintf(stderr,
                    "signal_replay: prefix replay failed before candidate %s\n",
                    SR_ACTIONS[candidate].name);
            if (out != stdout) fclose(out);
            return 1;
        }
        sr_write_row(out, &config, &result);
        emitted++;
    }

    if (out != stdout && fclose(out) != 0) {
        perror("signal_replay: close output");
        return 1;
    }
    if (emitted == 0) {
        fprintf(stderr, "signal_replay: no candidates emitted\n");
        return 1;
    }
    return 0;
}
