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

#include "chain_log.h"
#include "fixpoint.h"
#include "game_sim.h"
#include "manifest.h"
#include "sha256.h"

#define SR_SCHEMA "signal.replay_counterfactual.v1"
#define SR_ACTION_COUNT 9
#define SR_MAX_PREFIX 4096

typedef enum {
    SR_PROVENANCE_SCRIPT_NONE = 0,
    SR_PROVENANCE_SCRIPT_BUY_SELL,
    SR_PROVENANCE_SCRIPT_POD_TOW_SELL,
    SR_PROVENANCE_SCRIPT_MINE_FRACTURE,
    SR_PROVENANCE_SCRIPT_ASTEROID_DEATH,
} sr_provenance_script_t;

typedef struct {
    int turn;
    int thrust;
    const char *name;
} sr_action_def_t;

typedef struct {
    uint32_t seed;
    int station;
    bool spawn_set;
    bool goal_set;
    bool velocity_set;
    bool angle_set;
    vec2 spawn;
    vec2 goal;
    vec2 velocity;
    float angle;
    int horizon_ticks;
    int prefix[SR_MAX_PREFIX];
    int prefix_count;
    bool candidate_enabled[SR_ACTION_COUNT];
    sr_provenance_script_t provenance_script;
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
    int pickup_fragments;
    float pickup_ore;
    float damage_amount;
    int buy_cost;
    int buy_quantity;
    int sell_base;
    int sell_bonus;
} sr_event_counts_t;

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
    uint16_t end_manifest_count;
    sr_event_counts_t events;
    uint8_t prefix_state_hash[32];
    uint8_t state_hash[32];
    uint8_t event_hash[32];
} sr_result_t;

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
            "  --horizon-ticks N    branch horizon per candidate (default 36)\n"
            "  --candidates LIST    comma-separated candidate actions; default all 9\n"
            "  --provenance-script NAME  run a deterministic setup/action script\n"
            "                       before each branch; names: none,buy-sell,pod-tow-sell,mine-fracture,asteroid-death\n"
            "  --out PATH           write JSONL to PATH instead of stdout\n"
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
    return false;
}

static const char *sr_provenance_script_name(sr_provenance_script_t script)
{
    switch (script) {
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

static bool sr_parse_args(int argc, char **argv, sr_config_t *config)
{
    if (!config) return false;
    memset(config, 0, sizeof(*config));
    config->seed = 2037u;
    config->station = 0;
    config->horizon_ticks = 36;
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
            if (!sr_parse_i32(value, 1, 12000, &config->horizon_ticks)) return false;
            i++;
        } else if (strcmp(arg, "--candidates") == 0 && value) {
            if (!sr_parse_candidate_list(value, config->candidate_enabled)) return false;
            i++;
        } else if (strcmp(arg, "--provenance-script") == 0 && value) {
            if (!sr_parse_provenance_script(value, &config->provenance_script)) {
                return false;
            }
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

static void sr_hash_float_milli(sha256_ctx_t *ctx, float v)
{
    if (!isfinite(v)) v = 0.0f;
    sr_hash_i32(ctx, (int32_t)lroundf(v * 1000.0f));
}

static bool sr_reverse_allowed(const server_player_t *sp)
{
    vec2 forward = ship_forward(sp->ship.angle);
    return v2_dot(sp->ship.vel, forward) <= 2.0f;
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
    ship_cleanup(&sp->ship);
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
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        w->npc_ships[i].active = false;
    }

    station_index = config->station;
    if (station_index < 0 || station_index >= w->station_count) {
        station_index = 0;
    }
    station = &w->stations[station_index];

    sp = &w->players[0];
    sr_reset_player(w, sp);
    sp->current_station = station_index;
    sp->ship.pos = config->spawn_set
                 ? config->spawn
                 : v2_add(station->pos, v2(1600.0f, 200.0f));
    sp->ship.vel = config->velocity_set ? config->velocity : v2(0.0f, 0.0f);
    if (config->goal_set) {
        *out_goal = config->goal;
    } else {
        *out_goal = v2_add(station->pos, v2(2600.0f, -100.0f));
    }
    sp->ship.angle = config->angle_set
                   ? config->angle
                   : fixp_atan2f(out_goal->y - sp->ship.pos.y,
                                  out_goal->x - sp->ship.pos.x);
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.towed_count = 0;
    sp->ship.towed_scaffold = -1;

    if (out_spawn) *out_spawn = sp->ship.pos;
    if (out_sp) *out_sp = sp;
    return true;
}

static bool sr_setup_provenance_script(const sr_config_t *config,
                                       world_t *w,
                                       server_player_t *sp)
{
    if (!config || !w || !sp) return false;
    if (config->provenance_script == SR_PROVENANCE_SCRIPT_NONE) return true;

    switch (config->provenance_script) {
    case SR_PROVENANCE_SCRIPT_BUY_SELL: {
        const int station_index = 1; /* Kepler: seeded frame producer. */
        station_t *st;
        if (station_index >= w->station_count) return false;
        st = &w->stations[station_index];
        if (!station_manifest_bootstrap(st)) return false;
        if (station_finished_mint(st, COMMODITY_FRAME, 4, NULL) < 4) {
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
        const int station_index = 0;
        station_t *st;
        int pod_idx;
        if (station_index >= w->station_count) return false;
        st = &w->stations[station_index];
        memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
        if (st->base_price[COMMODITY_FERRITE_ORE] <= FLOAT_EPSILON) {
            st->base_price[COMMODITY_FERRITE_ORE] = 10.0f;
        }

        sp->docked = false;
        sp->current_station = station_index;
        sp->nearby_station = -1;
        sp->in_dock_range = false;
        sp->docking_approach = false;
        sp->dock_berth = 0;
        sp->ship.pos = v2_add(st->pos, v2(220.0f, 0.0f));
        sp->ship.vel = v2(0.0f, 0.0f);
        sp->ship.angle = PI_F;

        pod_idx = spawn_cargo_pod(w,
                                  v2_add(sp->ship.pos, v2(28.0f, 0.0f)),
                                  v2(0.0f, 0.0f),
                                  COMMODITY_FERRITE_ORE,
                                  7,
                                  CARGO_POD_CARGO);
        return pod_idx >= 0;
    }
    case SR_PROVENANCE_SCRIPT_MINE_FRACTURE: {
        const int asteroid_idx = 0;
        asteroid_t *a = &w->asteroids[asteroid_idx];
        vec2 forward = ship_forward(sp->ship.angle);
        vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);

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

        sp->ship.vel = v2(0.0f, 0.0f);
        sp->ship.mining_level = 0;
        sp->input.mining_target_hint = asteroid_idx;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_ASTEROID_DEATH: {
        const int asteroid_idx = 0;
        asteroid_t *a = &w->asteroids[asteroid_idx];
        const float asteroid_radius = 36.0f;
        float ship_radius = ship_hull_def(&sp->ship)->ship_radius;

        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(a, 0, sizeof(*a));
        a->active = true;
        a->fracture_child = false;
        a->tier = ASTEROID_TIER_M;
        a->commodity = COMMODITY_FERRITE_ORE;
        a->pos = v2(sp->ship.pos.x - (asteroid_radius + ship_radius - 3.0f),
                    sp->ship.pos.y);
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

        sp->ship.vel = v2(0.0f, 0.0f);
        sp->ship.hull = 20.0f;
        sp->docked = false;
        sp->docking_approach = false;
        sp->in_dock_range = false;
        sp->nearby_station = -1;
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

static void sr_hash_station_ledger(sha256_ctx_t *ctx, const station_t *st)
{
    int count = st->ledger_count;
    if (count < 0) count = 0;
    if (count > STATION_LEDGER_MAX) count = STATION_LEDGER_MAX;
    sr_hash_i32(ctx, count);
    for (int i = 0; i < count; i++) {
        sha256_update(ctx, st->ledger[i].player_pubkey,
                      sizeof(st->ledger[i].player_pubkey));
        sr_hash_float_milli(ctx, st->ledger[i].balance);
        sr_hash_float_milli(ctx, st->ledger[i].lifetime_supply);
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
    sr_hash_float_milli(ctx, ship->pos.x);
    sr_hash_float_milli(ctx, ship->pos.y);
    sr_hash_float_milli(ctx, ship->vel.x);
    sr_hash_float_milli(ctx, ship->vel.y);
    sr_hash_float_milli(ctx, ship->angle);
    sr_hash_float_milli(ctx, ship->hull);
    sr_hash_u8(ctx, (uint8_t)ship->hull_class);
    sr_hash_u8(ctx, ship->towed_count);
    sr_hash_u8(ctx, ship->towed_pod_count);
    sr_hash_i32(ctx, ship->towed_scaffold);
}

static void sr_state_hash(const world_t *w,
                          const server_player_t *sp,
                          uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, "signal-replay-state-v2", 22);
    sr_hash_u64(&ctx, w->tick);
    sr_hash_float_milli(&ctx, w->time);
    sr_hash_u32(&ctx, w->belt_seed);
    sr_hash_float_milli(&ctx, sp->ship.pos.x);
    sr_hash_float_milli(&ctx, sp->ship.pos.y);
    sr_hash_float_milli(&ctx, sp->ship.vel.x);
    sr_hash_float_milli(&ctx, sp->ship.vel.y);
    sr_hash_float_milli(&ctx, sp->ship.angle);
    sr_hash_float_milli(&ctx, sp->ship.hull);
    sr_hash_u8(&ctx, sp->docked ? 1u : 0u);
    sr_hash_i32(&ctx, sp->current_station);
    sr_hash_i32(&ctx, sp->nearby_station);
    sr_hash_u8(&ctx, sp->in_dock_range ? 1u : 0u);
    sr_hash_u8(&ctx, sp->ship.towed_count);
    for (int i = 0; i < (int)(sizeof(sp->ship.towed_fragments) /
                              sizeof(sp->ship.towed_fragments[0])); i++) {
        sr_hash_i32(&ctx, sp->ship.towed_fragments[i]);
    }
    sr_hash_u8(&ctx, sp->ship.towed_pod_count);
    for (int i = 0; i < (int)(sizeof(sp->ship.towed_pods) /
                              sizeof(sp->ship.towed_pods[0])); i++) {
        sr_hash_i32(&ctx, sp->ship.towed_pods[i]);
    }
    sr_hash_i32(&ctx, sp->ship.towed_scaffold);
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        sr_hash_float_milli(&ctx, sp->ship.cargo[c]);
    }
    sr_hash_ship_cargo_identity(&ctx, &sp->ship);

    int station_count = w->station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    sr_hash_i32(&ctx, station_count);
    for (int s = 0; s < station_count; s++) {
        const station_t *st = &w->stations[s];
        sr_hash_i32(&ctx, st->id);
        sr_hash_float_milli(&ctx, st->pos.x);
        sr_hash_float_milli(&ctx, st->pos.y);
        sha256_update(&ctx, st->station_pubkey, sizeof(st->station_pubkey));
        sha256_update(&ctx, st->outpost_founder_pubkey,
                      sizeof(st->outpost_founder_pubkey));
        sr_hash_u64(&ctx, st->outpost_planted_tick);
        sr_hash_manifest(&ctx, &st->manifest);
        sr_hash_receipts(&ctx, &st->manifest, station_get_receipts_const(st));
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            sr_hash_float_milli(&ctx, st->_inventory_cache[c]);
        }
        sr_hash_station_ledger(&ctx, st);
        sr_hash_float_milli(&ctx, ledger_balance(st, sp->session_token));
        sr_hash_float_milli(&ctx, ledger_balance_by_pubkey(st, sp->pubkey));
        sr_hash_u64(&ctx, st->chain_event_count);
        sha256_update(&ctx, st->chain_last_hash, sizeof(st->chain_last_hash));
    }

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
        sr_hash_float_milli(&ctx, a->pos.x);
        sr_hash_float_milli(&ctx, a->pos.y);
        sr_hash_float_milli(&ctx, a->vel.x);
        sr_hash_float_milli(&ctx, a->vel.y);
        sr_hash_float_milli(&ctx, a->radius);
        sr_hash_float_milli(&ctx, a->hp);
        sr_hash_float_milli(&ctx, a->ore);
        sr_hash_float_milli(&ctx, a->rotation);
        sr_hash_float_milli(&ctx, a->spin);
        sr_hash_float_milli(&ctx, a->smelt_progress);
        sr_hash_i32(&ctx, a->last_towed_by);
        sr_hash_i32(&ctx, a->last_fractured_by);
        sr_hash_u8(&ctx, a->thrown_timer_q);
        sr_hash_u8(&ctx, a->grade);
        sha256_update(&ctx, a->last_towed_token, sizeof(a->last_towed_token));
        sha256_update(&ctx, a->thrown_by_token, sizeof(a->thrown_by_token));
        sha256_update(&ctx, a->last_fractured_token,
                      sizeof(a->last_fractured_token));
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
        sr_hash_ship_body(&ctx, &npc->ship);
        for (int c = 0; c < COMMODITY_COUNT; c++)
            sr_hash_float_milli(&ctx, npc->cargo[c]);
        sr_hash_i32(&ctx, npc->target_asteroid);
        sr_hash_i32(&ctx, npc->home_station);
        sr_hash_i32(&ctx, npc->dest_station);
        sr_hash_float_milli(&ctx, npc->state_timer);
        sr_hash_u8(&ctx, npc->thrusting ? 1u : 0u);
        sr_hash_i32(&ctx, npc->towed_fragment);
        sr_hash_i32(&ctx, npc->towed_scaffold);
        sr_hash_float_milli(&ctx, npc->hull);
        sha256_update(&ctx, npc->session_token, sizeof(npc->session_token));
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
        sr_hash_float_milli(&ctx, sc->pos.x);
        sr_hash_float_milli(&ctx, sc->pos.y);
        sr_hash_float_milli(&ctx, sc->vel.x);
        sr_hash_float_milli(&ctx, sc->vel.y);
        sr_hash_float_milli(&ctx, sc->rotation);
        sr_hash_float_milli(&ctx, sc->spin);
        sr_hash_i32(&ctx, sc->placed_station);
        sr_hash_i32(&ctx, sc->placed_ring);
        sr_hash_i32(&ctx, sc->placed_slot);
        sr_hash_i32(&ctx, sc->towed_by);
        sr_hash_i32(&ctx, sc->built_at_station);
        sr_hash_float_milli(&ctx, sc->build_amount);
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
        sr_hash_float_milli(&ctx, pod->pos.x);
        sr_hash_float_milli(&ctx, pod->pos.y);
        sr_hash_float_milli(&ctx, pod->vel.x);
        sr_hash_float_milli(&ctx, pod->vel.y);
        sr_hash_float_milli(&ctx, pod->rotation);
        sr_hash_float_milli(&ctx, pod->spin);
        sr_hash_float_milli(&ctx, pod->age);
        sr_hash_i32(&ctx, pod->towed_by);
    }
    sha256_final(&ctx, out);
}

static void sr_hash_event(sha256_ctx_t *ctx, const sim_event_t *ev)
{
    sr_hash_u8(ctx, (uint8_t)ev->type);
    sr_hash_i32(ctx, ev->player_id);
    switch (ev->type) {
    case SIM_EVENT_DAMAGE:
        sr_hash_float_milli(ctx, ev->damage.amount);
        sr_hash_float_milli(ctx, ev->damage.source_x);
        sr_hash_float_milli(ctx, ev->damage.source_y);
        break;
    case SIM_EVENT_DEATH:
        sr_hash_u8(ctx, ev->death.cause);
        sha256_update(ctx, ev->death.killer_token, sizeof(ev->death.killer_token));
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
        sr_hash_float_milli(ctx, ev->pickup.ore);
        sr_hash_i32(ctx, ev->pickup.fragments);
        break;
    case SIM_EVENT_FRACTURE:
        sr_hash_i32(ctx, ev->fracture.tier);
        sr_hash_i32(ctx, ev->fracture.asteroid_id);
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
            counts->repair_events++;
            break;
        case SIM_EVENT_MINING_TICK:
            counts->mining_tick_events++;
            break;
        case SIM_EVENT_FRACTURE:
            counts->fracture_events++;
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
    case SR_PROVENANCE_SCRIPT_BUY_SELL: {
        uint16_t start_station_manifest = w->stations[sp->current_station].manifest.count;
        uint16_t start_ship_manifest = sp->ship.manifest.count;

        sp->input.buy_product = true;
        sp->input.buy_commodity = COMMODITY_FRAME;
        sp->input.buy_grade = MINING_GRADE_COMMON;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->buy_events <= 0 ||
            sp->ship.manifest.count <= start_ship_manifest ||
            w->stations[sp->current_station].manifest.count >= start_station_manifest) {
            return false;
        }

        sp->input.service_sell = true;
        sp->input.service_sell_only = COMMODITY_FRAME;
        sp->input.service_sell_grade = MINING_GRADE_COUNT;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->sell_events <= 0 ||
            sp->ship.manifest.count != start_ship_manifest) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_POD_TOW_SELL: {
        int pickup_before = counts->pickup_events;
        int dock_before = counts->dock_events;
        int sell_before = counts->sell_events;
        int station_index = sp->current_station;

        sp->input.tractor_hold = true;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->pickup_events <= pickup_before ||
            sp->ship.towed_pod_count <= 0) {
            return false;
        }

        sp->input.tractor_hold = true;
        sp->input.dock = true;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        for (int i = 0; i < 240 && !sp->docked; i++) {
            sp->input.tractor_hold = true;
            world_sim_step(w, SIM_DT);
            sr_accumulate_events(w, counts, event_hash);
        }
        if (!sp->docked || sp->current_station != station_index ||
            counts->dock_events <= dock_before ||
            sp->ship.towed_pod_count <= 0) {
            return false;
        }

        sp->input.service_sell = true;
        sp->input.service_sell_only = COMMODITY_COUNT;
        sp->input.service_sell_grade = MINING_GRADE_COUNT;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        if (counts->sell_events <= sell_before ||
            sp->ship.towed_pod_count != 0) {
            return false;
        }
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            if (w->cargo_pods[i].active) return false;
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
            sp->ship.stat_asteroids_fractured <= 0) {
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
            sp->ship.hull <= 0.0f) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_NONE:
    default:
        return true;
    }
}

static bool sr_replay_prefix(const sr_config_t *config,
                             world_t *w,
                             server_player_t *sp)
{
    for (int i = 0; i < config->prefix_count; i++) {
        sr_apply_action(sp, config->prefix[i]);
        world_sim_step(w, SIM_DT);
        for (int e = 0; e < w->events.count; e++) {
            if (w->events.events[e].player_id == sp->id &&
                w->events.events[e].type == SIM_EVENT_DEATH) {
                return false;
            }
        }
        if (sp->ship.hull <= 0.0f) return false;
    }
    return true;
}

static bool sr_run_branch(const sr_config_t *config, int candidate, sr_result_t *out)
{
    world_t *w = NULL;
    server_player_t *sp = NULL;
    vec2 spawn = v2(0.0f, 0.0f);
    vec2 goal = v2(0.0f, 0.0f);
    sha256_ctx_t event_hash;
    bool ok = false;

    memset(out, 0, sizeof(*out));
    out->candidate = candidate;
    out->prefix_ticks = config->prefix_count;
    out->horizon_ticks = config->horizon_ticks;

    w = (world_t *)calloc(1, sizeof(*w));
    if (!w) {
        return false;
    }

    if (!sr_setup_world(config, w, &sp, &spawn, &goal)) {
        free(w);
        return false;
    }
    if (!sr_setup_provenance_script(config, w, sp)) {
        world_cleanup(w);
        free(w);
        return false;
    }
    (void)spawn;

    if (!sr_replay_prefix(config, w, sp)) {
        world_cleanup(w);
        free(w);
        return false;
    }

    out->start_station = sp->current_station;
    out->start_pos = sp->ship.pos;
    out->start_dist = v2_len(v2_sub(goal, sp->ship.pos));
    out->start_hull = sp->ship.hull;
    out->start_cargo = ship_total_cargo(&sp->ship);
    out->start_balance = sr_player_station_balance(w, sp);
    sr_state_hash(w, sp, out->prefix_state_hash);

    sha256_init(&event_hash);
    sha256_update(&event_hash, "signal-replay-events-v1", 23);
    if (!sr_run_provenance_script(config, w, sp, &out->events, &event_hash)) {
        world_cleanup(w);
        free(w);
        return false;
    }
    for (int i = 0; i < config->horizon_ticks; i++) {
        sr_apply_action(sp, candidate);
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, &out->events, &event_hash);
        if (out->events.death_events > 0 || sp->ship.hull <= 0.0f) {
            break;
        }
    }
    sha256_final(&event_hash, out->event_hash);

    out->end_pos = sp->ship.pos;
    out->end_vel = sp->ship.vel;
    out->end_angle = sp->ship.angle;
    out->end_speed = v2_len(sp->ship.vel);
    out->end_hull = sp->ship.hull;
    out->end_dist = v2_len(v2_sub(goal, sp->ship.pos));
    out->progress = out->start_dist - out->end_dist;
    out->hull_loss = out->start_hull - out->end_hull;
    if (out->hull_loss < 0.0f) out->hull_loss = 0.0f;
    out->end_cargo = ship_total_cargo(&sp->ship);
    out->end_balance = sr_player_station_balance(w, sp);
    out->end_docked = sp->docked;
    out->end_current_station = sp->current_station;
    out->end_manifest_count = sp->ship.manifest.count;
    out->utility = ((double)out->progress / 1000.0) -
                   ((double)out->hull_loss * 0.45) -
                   ((double)out->events.damage_amount * 0.25) -
                   ((double)out->events.damage_events * 2.0) -
                   ((double)out->events.death_events * 80.0);
    sr_state_hash(w, sp, out->state_hash);
    out->ok = true;
    ok = true;

    world_cleanup(w);
    free(w);
    return ok;
}

static void sr_json_hash(FILE *out, const char *key, const uint8_t hash[32])
{
    char hex[65];
    sr_hex(hash, hex);
    fprintf(out, "\"%s\":\"%s\"", key, hex);
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
            "\"candidate_name\":\"%s\",",
            SR_SCHEMA,
            config->seed,
            config->station,
            sr_provenance_script_name(config->provenance_script),
            r->prefix_ticks,
            r->horizon_ticks,
            r->candidate,
            SR_ACTIONS[r->candidate].name);
    sr_json_hash(out, "prefix_state_hash", r->prefix_state_hash);
    fprintf(out, ",");
    sr_json_hash(out, "state_hash", r->state_hash);
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
            ",\"damage_amount\":%.3f"
            ",\"authority\":\"deterministic_seed_prefix_replay\"}\n",
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
            r->events.damage_amount);
}

int main(int argc, char **argv)
{
    sr_config_t config;
    FILE *out = stdout;
    int emitted = 0;

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
