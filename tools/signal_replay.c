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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_log.h"
#include "fixpoint.h"
#include "game_sim.h"
#include "gossip.h"
#include "holographic_nn.h"
#include "manifest.h"
#include "protocol.h"
#include "sha256.h"
#include "sim_ai.h"
#include "sim_asteroid.h"
#include "sim_nav.h"
#include "sim_physics.h"
#include "station_util.h"

#define SR_SCHEMA "signal.replay_counterfactual.v1"
#define SR_ACTION_COUNT 9
#define SR_MAX_PREFIX 4096
#define SR_MAX_HORIZON_TICKS 120000

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
    bool hnn_trace;
    bool active_workers;
    int hnn_cleanup_steps;
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
    int outpost_placed_events;
    int scaffold_ready_events;
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
    sr_ai_summary_t ai;
    sr_hnn_eval_t hnn;
    uint8_t prefix_state_hash[32];
    uint8_t state_hash[32];
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
            "  --active-workers     keep seeded NPC workers active and include AI/gossip/HNN metrics\n"
            "  --hnn-cleanup-steps N cleanup steps for HNN retrieval (default 3; 0..8)\n"
            "  --provenance-script NAME  run a deterministic setup/action script\n"
            "                       before each branch; names: none,buy-sell,pod-tow-sell,mine-fracture,asteroid-death,planned-outpost,station-jostle,player-ram,npc-ram,thrown-rock-hit,fracture-claim,worker-tow-hnn,worker-repair-hnn,worker-delivery-proof-hnn,worker-gossip-courier\n"
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
    return false;
}

static const char *sr_provenance_script_name(sr_provenance_script_t script)
{
    switch (script) {
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
    w->cargo_pods[pod_idx].towed_by = -1;
    cargo_pod_set_module_tractor(&w->cargo_pods[pod_idx],
                                 station_idx, dock_idx);
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

static void sr_reset_worker_fixture_state(world_t *w)
{
    if (!w) return;
    memset(w->contracts, 0, sizeof(w->contracts));
    signal_field_init(&w->signal_field);
    w->signal_field_decay_tick = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w->stations[s].known_contracts, 0,
               sizeof(w->stations[s].known_contracts));
        w->stations[s].known_contract_count = 0;
        memset(&w->stations[s].knowledge, 0,
               sizeof(w->stations[s].knowledge));
        hnn_memory_init(&w->stations[s].hnn_market_memory);
        w->stations[s].hnn_market_version = 0;
        w->stations[s].hnn_market_decay_tick = 0;
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w->npc_ships[i].active) {
            ship_cleanup(&w->npc_ships[i].ship);
        }
        memset(&w->npc_ships[i], 0, sizeof(w->npc_ships[i]));
    }
    for (int i = 0; i < MAX_SHIPS; i++) {
        ship_cleanup(&w->ships[i]);
        memset(&w->ships[i], 0, sizeof(w->ships[i]));
    }
    for (int i = 0; i < MAX_PLAYERS + MAX_NPC_SHIPS; i++)
        memset(&w->characters[i], 0, sizeof(w->characters[i]));
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_cleanup(&w->ship_assets[i].ship);
        memset(&w->ship_assets[i], 0, sizeof(w->ship_assets[i]));
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        w->scaffolds[i].active = false;
    memset(w->delivery_shipments, 0, sizeof(w->delivery_shipments));
    w->next_delivery_shipment_id = 1;
    w->npc_respawn_timer = 3600.0f;
    w->frontier_virtual_pilots = 0;
}

static int sr_station_remote_market_memory_items(
    const knowledge_view_t *view, int local_station);
static int sr_station_remote_known_contracts(
    const contract_summary_t *contracts, int count, int cap, int local_station);

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
        sp->ship.pos = v2_add(st->pos, v2(800.0f, 0.0f));
        sp->ship.vel = v2(0.0f, 0.0f);
        sp->ship.angle = PI_F;

        memset(units, 0, sizeof(units));
        for (uint16_t i = 0; i < 7; i++) {
            if (!hash_legacy_migrate_unit(origin, pod_commodity,
                                          i, &units[i])) {
                return false;
            }
        }
        pod_idx = spawn_cargo_pod_with_manifest_deterministic(
            w, v2_add(sp->ship.pos, v2(28.0f, 0.0f)),
            v2(0.0f, 0.0f), pod_commodity,
            units, 7, CARGO_POD_CARGO, 0.0f, 0.0f);
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
        sp->ship.pos = center;
        other->ship.pos = v2_add(center, v2(30.0f, 0.0f));
        sp->ship.vel = v2(250.0f, 0.0f);
        other->ship.vel = v2(-250.0f, 0.0f);
        sp->ship.angle = 0.0f;
        other->ship.angle = PI_F;
        sp->ship.hull = ship_max_hull(&sp->ship);
        other->ship.hull = ship_max_hull(&other->ship);
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
        a->ship.pos = center;
        b->ship.pos = v2_add(center, v2(30.0f, 0.0f));
        a->ship.vel = v2(250.0f, 0.0f);
        b->ship.vel = v2(-250.0f, 0.0f);
        a->ship.angle = 0.0f;
        b->ship.angle = PI_F;
        a->ship.hull = npc_max_hull(a);
        b->ship.hull = npc_max_hull(b);
        a->hull = a->ship.hull;
        b->hull = b->ship.hull;
        *a_ship = a->ship;
        *b_ship = b->ship;
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

        sp->ship.pos = v2_add(center, v2(-360.0f, 0.0f));
        sp->ship.vel = v2(0.0f, 0.0f);
        sp->ship.angle = 0.0f;
        sp->docked = false;
        sp->in_dock_range = false;
        sp->nearby_station = -1;

        target->ship.pos = center;
        target->ship.vel = v2(0.0f, 0.0f);
        target->ship.angle = PI_F;
        target->ship.hull = ship_max_hull(&target->ship);
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
        a->pos = v2(target->ship.pos.x - 80.0f, target->ship.pos.y);
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

        sp->ship.pos = w->stations[0].pos;
        sp->ship.vel = v2(0.0f, 0.0f);
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
        ship_t *ship;

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
        w->scaffolds[sc_idx].towed_by = -1;
        w->scaffolds[sc_idx].built_at_station = home_station;
        w->scaffolds[sc_idx].vel = v2(0.0f, 0.0f);

        npc_slot = spawn_npc(w, home_station, NPC_ROLE_HAULER);
        if (npc_slot < 0) return false;
        npc = &w->npc_ships[npc_slot];
        npc->role = NPC_ROLE_HAULER;
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        npc->known_contract_count = 0;
        memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
        memset(&npc->knowledge, 0, sizeof(npc->knowledge));
        knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;
        npc->ship.pos = w->stations[home_station].pos;
        npc->ship.vel = v2(0.0f, 0.0f);
        npc->ship.hull_class = HULL_CLASS_HAULER;
        npc->ship.hull = hull_max_for_class(HULL_CLASS_HAULER);
        npc->hull = npc->ship.hull;
        ship = world_npc_ship_for(w, npc_slot);
        if (!ship) return false;
        ship->hull_class = HULL_CLASS_HAULER;
        ship->hull = npc->ship.hull;
        ship->pos = npc->ship.pos;
        ship->vel = npc->ship.vel;
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_REPAIR_HNN: {
        const int home_station = 0;
        station_t *home;
        int existing_kits;
        int npc_slot;
        npc_ship_t *npc;
        ship_t *ship;
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
        npc->known_contract_count = 0;
        memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
        memset(&npc->knowledge, 0, sizeof(npc->knowledge));
        knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;

        ship = world_npc_ship_for(w, npc_slot);
        if (!ship) return false;
        npc->ship.pos = home->pos;
        npc->ship.vel = v2(0.0f, 0.0f);
        ship->pos = npc->ship.pos;
        ship->vel = npc->ship.vel;
        ship->hull = npc_max_hull(npc) - 12.0f;
        npc->hull = ship->hull;

        if (!market_memory_from_station_supply(home, home_station,
                                               COMMODITY_REPAIR_KIT,
                                               w->tick, &supply)) {
            return false;
        }
        if (!knowledge_item_from_market_memory(&supply, &item))
            return false;
        knowledge_view_insert(&npc->knowledge, &item);
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
        ship_t *ship;
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
        npc->known_contract_count = 0;
        memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
        memset(&npc->knowledge, 0, sizeof(npc->knowledge));
        knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;

        ship = world_npc_ship_for(w, npc_slot);
        if (!ship) return false;
        npc->ship.pos = origin->pos;
        npc->ship.vel = v2(0.0f, 0.0f);
        ship->pos = npc->ship.pos;
        ship->vel = npc->ship.vel;

        summary = contract_summary_make(&w->contracts[0]);
        if (!market_memory_from_contract_summary(&summary, &demand))
            return false;
        if (!knowledge_item_from_market_memory(&demand, &item))
            return false;
        knowledge_view_insert(&npc->knowledge, &item);
        return true;
    }
    case SR_PROVENANCE_SCRIPT_WORKER_GOSSIP_COURIER: {
        const int source_station = 2;
        const int receiving_station = 0;
        int npc_slot;
        npc_ship_t *npc;
        ship_t *ship;

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
        if (w->stations[source_station].known_contract_count <= 0 ||
            w->stations[receiving_station].known_contract_count != 0 ||
            sr_station_remote_market_memory_items(
                &w->stations[receiving_station].knowledge,
                receiving_station) != 0) {
            return false;
        }

        npc_slot = spawn_npc(w, source_station, NPC_ROLE_HAULER);
        if (npc_slot < 0) return false;
        npc = &w->npc_ships[npc_slot];
        ship = world_npc_ship_for(w, npc_slot);
        if (!ship) return false;

        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = 0.0f;
        npc->known_contract_count = 0;
        memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
        memset(&npc->knowledge, 0, sizeof(npc->knowledge));
        knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&npc->hnn_market_mem);
        npc->hnn_market_station = 0xffu;
        npc->hnn_market_version = 0;
        npc->hnn_market_decay_tick = 0;

        gossip_dock_handshake(w, source_station,
                              npc->known_contracts,
                              &npc->known_contract_count,
                              SHIP_KNOWN_CONTRACT_CAP,
                              &npc->knowledge);
        if (npc->known_contract_count <= 0 ||
            sr_station_remote_market_memory_items(&npc->knowledge,
                                                  receiving_station) <= 0) {
            return false;
        }

        npc->dest_station = receiving_station;
        npc->pickup_station = -1;
        npc->pickup_commodity = COMMODITY_COUNT;
        npc->pickup_action = (uint8_t)CONTRACT_TRACTOR;
        npc->state = NPC_STATE_UNLOADING;
        npc->state_timer = 0.0f;
        npc->ship.pos = station_approach_target(
            &w->stations[receiving_station], npc->ship.pos);
        npc->ship.vel = v2(0.0f, 0.0f);
        npc->ship.hull = npc_max_hull(npc);
        npc->hull = npc->ship.hull;
        ship->pos = npc->ship.pos;
        ship->vel = npc->ship.vel;
        ship->hull = npc->ship.hull;
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
        sr_hash_float_bits(ctx, st->module_input[m]);
        sr_hash_float_bits(ctx, st->module_output[m]);
        sr_hash_float_bits(ctx, st->module_craft_progress[m]);
        sr_hash_u8(ctx, st->module_diag[m]);
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

static void sr_hash_player_state(sha256_ctx_t *ctx, const server_player_t *player)
{
    sr_hash_i32(ctx, player->id);
    sha256_update(ctx, player->session_token, sizeof(player->session_token));
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
    sr_hash_ship_body(ctx, &player->ship);
    for (int i = 0; i < (int)(sizeof(player->ship.towed_fragments) /
                              sizeof(player->ship.towed_fragments[0])); i++) {
        sr_hash_i32(ctx, player->ship.towed_fragments[i]);
    }
    for (int i = 0; i < (int)(sizeof(player->ship.towed_pods) /
                              sizeof(player->ship.towed_pods[0])); i++) {
        sr_hash_i32(ctx, player->ship.towed_pods[i]);
    }
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        sr_hash_float_bits(ctx, player->ship.cargo[c]);
    }
    sr_hash_ship_cargo_identity(ctx, &player->ship);
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

static const ship_t *sr_npc_paired_ship_const(const world_t *w, int npc_slot)
{
    if (!w || npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return NULL;
    if (!w->npc_ships[npc_slot].active) return NULL;
    for (int c = 0; c < MAX_PLAYERS + MAX_NPC_SHIPS; c++) {
        const character_t *ch = &w->characters[c];
        if (!ch->active || ch->npc_slot != npc_slot) continue;
        if (ch->ship_idx < 0 || ch->ship_idx >= MAX_SHIPS) return NULL;
        return &w->ships[ch->ship_idx];
    }
    return NULL;
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
        sr_hash_u8(ctx, state->seen_claimant_count);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            sha256_update(ctx, state->seen_claimant_tokens[p],
                          sizeof(state->seen_claimant_tokens[p]));
        }
        sr_hash_u32(ctx, state->challenge_last_ms);
    }
}

static void sr_state_hash(const world_t *w,
                          const server_player_t *sp,
                          uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, "signal-replay-state-v5-ai-memory", 32);
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
        sr_hash_manifest(&ctx, &st->manifest);
        sr_hash_receipts(&ctx, &st->manifest, station_get_receipts_const(st));
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            sr_hash_float_bits(&ctx, st->_inventory_cache[c]);
        }
        sr_hash_station_ledger(&ctx, st);
        sr_hash_float_bits(&ctx, ledger_balance(st, sp->session_token));
        sr_hash_float_bits(&ctx, ledger_balance_by_pubkey(st, sp->pubkey));
        sr_hash_u64(&ctx, st->chain_event_count);
        sha256_update(&ctx, st->chain_last_hash, sizeof(st->chain_last_hash));
        sr_hash_known_contracts(&ctx, st->known_contracts,
                                st->known_contract_count,
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
        const ship_t *paired_ship;
        if (!npc->active) continue;
        sr_hash_i32(&ctx, i);
        sr_hash_u8(&ctx, (uint8_t)npc->role);
        sr_hash_u8(&ctx, (uint8_t)npc->state);
        sr_hash_ship_body(&ctx, &npc->ship);
        paired_ship = sr_npc_paired_ship_const(w, i);
        sr_hash_u8(&ctx, paired_ship ? 1u : 0u);
        if (paired_ship) {
            sr_hash_ship_body(&ctx, paired_ship);
            sr_hash_ship_cargo_identity(&ctx, paired_ship);
        }
        for (int c = 0; c < COMMODITY_COUNT; c++)
            sr_hash_float_bits(&ctx, npc->cargo[c]);
        sr_hash_i32(&ctx, npc->target_asteroid);
        sr_hash_i32(&ctx, npc->home_station);
        sr_hash_i32(&ctx, npc->dest_station);
        sr_hash_float_bits(&ctx, npc->state_timer);
        sr_hash_u8(&ctx, npc->thrusting ? 1u : 0u);
        sr_hash_i32(&ctx, npc->towed_fragment);
        sr_hash_i32(&ctx, npc->towed_scaffold);
        sr_hash_float_bits(&ctx, npc->hull);
        sha256_update(&ctx, npc->session_token, sizeof(npc->session_token));
        sr_hash_known_contracts(&ctx, npc->known_contracts,
                                npc->known_contract_count,
                                SHIP_KNOWN_CONTRACT_CAP);
        sr_hash_knowledge_view(&ctx, &npc->knowledge);
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
        sr_hash_i32(&ctx, sc->towed_by);
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
        sr_hash_float_bits(ctx, ev->damage.amount);
        sr_hash_float_bits(ctx, ev->damage.source_x);
        sr_hash_float_bits(ctx, ev->damage.source_y);
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
        case SIM_EVENT_OUTPOST_PLACED:
            counts->outpost_placed_events++;
            break;
        case SIM_EVENT_SCAFFOLD_READY:
            counts->scaffold_ready_events++;
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
        int buy_before = counts->buy_events;
        int sell_before = counts->sell_events;
        int start_towed_pods = sp->ship.towed_pod_count;
        int pod_idx = -1;

        sp->input.buy_product = true;
        sp->input.buy_commodity = COMMODITY_FRAME;
        sp->input.buy_grade = MINING_GRADE_COMMON;
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);
        sp->input.buy_product = false;
        if (sp->ship.towed_pod_count <= start_towed_pods) {
            return false;
        }
        pod_idx = sp->ship.towed_pods[start_towed_pods];
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS ||
            !w->cargo_pods[pod_idx].active ||
            w->cargo_pods[pod_idx].towed_by != (int8_t)sp->id ||
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
            sp->ship.towed_pod_count != start_towed_pods ||
            !w->cargo_pods[pod_idx].active ||
            w->cargo_pods[pod_idx].towed_by != -1) {
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
            sp->ship.towed_pod_count <= 0) {
            return false;
        }

        pod_idx = sp->ship.towed_pods[0];
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS ||
            !w->cargo_pods[pod_idx].active ||
            w->cargo_pods[pod_idx].towed_by != (int8_t)sp->id) {
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
            sp->ship.towed_pod_count != 0 ||
            !w->cargo_pods[pod_idx].active ||
            w->cargo_pods[pod_idx].towed_by != -1 ||
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
        float primary_hull = sp->ship.hull;
        float other_hull = other->ship.hull;

        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        if (counts->damage_events <= damage_before ||
            sp->ship.hull >= primary_hull ||
            other->ship.hull >= other_hull ||
            v2_dist_sq(sp->ship.pos, other->ship.pos) <= 0.0f) {
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
            v2_dist_sq(w->npc_ships[0].ship.pos,
                       w->npc_ships[1].ship.pos) <= 0.0f) {
            return false;
        }
        return true;
    }
    case SR_PROVENANCE_SCRIPT_THROWN_ROCK_HIT: {
        server_player_t *target = &w->players[1];
        asteroid_t *a = &w->asteroids[0];
        int damage_before = counts->damage_events;
        float target_hull = target->ship.hull;
        bool hit = false;

        for (int i = 0; i < 60 && !hit; i++) {
            world_sim_step(w, SIM_DT);
            sr_accumulate_events(w, counts, event_hash);
            hit = counts->damage_events > damage_before ||
                  target->ship.hull < target_hull ||
                  !asteroid_is_ballistic(a);
        }

        if (counts->damage_events <= damage_before ||
            target->ship.hull >= target_hull ||
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
        npc->ship.pos = sc->pos;
        npc->ship.vel = v2(0.0f, 0.0f);
        if (ship) {
            ship->pos = npc->ship.pos;
            ship->vel = npc->ship.vel;
        }

        w->events.count = 0;
        step_npc_ships(w, SIM_DT);
        sr_accumulate_events(w, counts, event_hash);

        worker_pickup =
            npc->towed_scaffold == tow_scaffold_slot &&
            sc->state == SCAFFOLD_TOWING &&
            sc->towed_by == -2 - tow_npc_slot;
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
                   w->stations[receiving_station].known_contracts,
                   w->stations[receiving_station].known_contract_count,
                   STATION_KNOWN_CONTRACT_CAP,
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

static float sr_npc_finished_cargo_total(const npc_ship_t *npc,
                                         const ship_t *ship)
{
    float total = 0.0f;
    if (ship && ship->manifest.count > 0) {
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
            total += (float)ship_finished_count(ship, (commodity_t)c);
        return total;
    }
    if (!npc) return 0.0f;
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
        total += npc->cargo[c];
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
                                 const sr_ai_summary_t *sample)
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
}

static int sr_station_remote_known_contracts(
    const contract_summary_t *contracts, int count, int cap, int local_station)
{
    int total = 0;
    count = sr_clamped_u8_count(count, cap);
    if (!contracts || local_station < 0 || local_station >= MAX_STATIONS)
        return 0;
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
        out->station_known_contracts += sr_clamped_u8_count(
            st->known_contract_count, STATION_KNOWN_CONTRACT_CAP);
        out->station_knowledge_items += sr_clamped_u8_count(
            st->knowledge.count, KNOWLEDGE_VIEW_MAX_CAP);
        out->station_remote_known_contracts +=
            sr_station_remote_known_contracts(st->known_contracts,
                                              st->known_contract_count,
                                              STATION_KNOWN_CONTRACT_CAP,
                                              s);
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
            if (sc->towed_by <= -2)
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
        if (npc->towed_scaffold >= 0)
            out->workers_towing_scaffold++;
        finished_cargo = sr_npc_finished_cargo_total(
            npc, sr_npc_paired_ship_const(w, i));
        if (finished_cargo > 0.01f) {
            out->workers_with_finished_cargo++;
            out->worker_finished_cargo_units += finished_cargo;
        }
        out->npc_known_contracts += sr_clamped_u8_count(
            npc->known_contract_count, SHIP_KNOWN_CONTRACT_CAP);
        out->npc_knowledge_items += sr_clamped_u8_count(
            npc->knowledge.count, KNOWLEDGE_VIEW_MAX_CAP);
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
    ship = &sp->ship;
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
                                     hnn_memory_t *mem,
                                     hnn_holonet_t *net,
                                     const hnn_action_table_t *actions)
{
    hnn_pilot_features_t route_features;
    hnn_pilot_features_t features;
    float route_vec[HNN_DIM];
    float state_vec[HNN_DIM];
    if (!mem || !actions || action < 0 || action >= HNN_ACTION_COUNT) return;
    sr_hnn_fill_features(w, sp, goal, 0, &route_features);
    sr_hnn_fill_features(w, sp, goal, action, &features);
    hnn_encode_state(&route_features, route_vec);
    hnn_encode_state(&features, state_vec);
    hnn_memory_store(mem, state_vec, actions->vecs[action]);
    if (net)
        hnn_holonet_store(net, route_vec, state_vec, actions->vecs[action]);
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

static void sr_hnn_evaluate_branch(const world_t *w,
                                   const server_player_t *sp,
                                   vec2 goal,
                                   int candidate,
                                   hnn_memory_t *mem,
                                   hnn_holonet_t *net,
                                   const hnn_action_table_t *actions,
                                   int cleanup_steps,
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
    hnn_memory_t hnn_mem;
    hnn_holonet_t hnn_net;
    hnn_action_table_t hnn_actions;
    hnn_memory_t *hnn_mem_ptr = NULL;
    hnn_holonet_t *hnn_net_ptr = NULL;
    hnn_action_table_t *hnn_actions_ptr = NULL;
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
    sha256_update(&event_hash, "signal-replay-events-v2-float-bits", 34);
    if (!sr_run_provenance_script(config, w, sp, &out->events, &event_hash)) {
        world_cleanup(w);
        free(w);
        return false;
    }
    if (config->hnn_trace) {
        sr_hnn_evaluate_branch(w, sp, goal, candidate,
                               hnn_mem_ptr, hnn_net_ptr, hnn_actions_ptr,
                               config->hnn_cleanup_steps,
                               &out->hnn);
    }
    if (config->active_workers) {
        sr_ai_summary_t sample;
        sr_collect_ai_summary(w, &sample);
        sr_ai_branch_observe(&out->ai, &sample);
    }
    for (int i = 0; i < config->horizon_ticks; i++) {
        sr_apply_action(sp, candidate);
        world_sim_step(w, SIM_DT);
        sr_accumulate_events(w, &out->events, &event_hash);
        if (config->active_workers) {
            sr_ai_summary_t sample;
            sr_collect_ai_summary(w, &sample);
            sr_ai_branch_observe(&out->ai, &sample);
        }
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
    if (config->active_workers) {
        sr_ai_branch_summary_t branch = out->ai.branch;
        sr_collect_ai_summary(w, &out->ai);
        out->ai.branch = branch;
    }
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

static void sr_write_hnn_eval(FILE *out, const sr_hnn_eval_t *hnn)
{
    fprintf(out,
            ",\"hnn\":{\"schema\":\"signal.replay_hnn_eval.v1\","
            "\"top_action\":%d,"
            "\"top_action_name\":\"%s\","
            "\"top_allowed_action\":%d,"
            "\"top_allowed_action_name\":\"%s\","
            "\"allowed_mask\":\"0x%03x\","
            "\"candidate_rank\":%d,"
            "\"candidate_allowed\":%s,"
            "\"candidate_allowed_rank\":%d,"
            "\"candidate_score\":",
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
        sr_write_hnn_eval(out, &r->hnn);
    }
    fprintf(out, ",\"authority\":\"deterministic_seed_prefix_replay\"}\n");
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
