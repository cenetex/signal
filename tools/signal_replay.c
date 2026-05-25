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
#include "game_sim.h"
#include "manifest.h"
#include "sha256.h"

#define SR_SCHEMA "signal.replay_counterfactual.v1"
#define SR_ACTION_COUNT 9
#define SR_MAX_PREFIX 4096

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
    const char *out_path;
} sr_config_t;

typedef struct {
    int damage_events;
    int death_events;
    int dock_events;
    int launch_events;
    int buy_events;
    int sell_events;
    int repair_events;
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
                   : atan2f(out_goal->y - sp->ship.pos.y,
                            out_goal->x - sp->ship.pos.x);
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.towed_count = 0;
    sp->ship.towed_scaffold = -1;

    if (out_spawn) *out_spawn = sp->ship.pos;
    if (out_sp) *out_sp = sp;
    return true;
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

static void sr_state_hash(const world_t *w,
                          const server_player_t *sp,
                          uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, "signal-replay-state-v1", 22);
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
    sr_hash_i32(&ctx, sp->ship.towed_scaffold);
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        sr_hash_float_milli(&ctx, sp->ship.cargo[c]);
    }
    sr_hash_manifest(&ctx, &sp->ship.manifest);

    int station_count = w->station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    sr_hash_i32(&ctx, station_count);
    for (int s = 0; s < station_count; s++) {
        const station_t *st = &w->stations[s];
        sr_hash_i32(&ctx, st->id);
        sr_hash_float_milli(&ctx, st->pos.x);
        sr_hash_float_milli(&ctx, st->pos.y);
        sr_hash_u16(&ctx, st->manifest.count);
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            sr_hash_float_milli(&ctx, st->_inventory_cache[c]);
        }
        sr_hash_float_milli(&ctx, ledger_balance(st, sp->session_token));
        sr_hash_float_milli(&ctx, ledger_balance_by_pubkey(st, sp->pubkey));
        sr_hash_u64(&ctx, st->chain_event_count);
        sha256_update(&ctx, st->chain_last_hash, sizeof(st->chain_last_hash));
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
        default:
            break;
        }
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
    world_t w = {0};
    server_player_t *sp = NULL;
    vec2 spawn = v2(0.0f, 0.0f);
    vec2 goal = v2(0.0f, 0.0f);
    sha256_ctx_t event_hash;
    bool ok = false;

    memset(out, 0, sizeof(*out));
    out->candidate = candidate;
    out->prefix_ticks = config->prefix_count;
    out->horizon_ticks = config->horizon_ticks;

    if (!sr_setup_world(config, &w, &sp, &spawn, &goal)) {
        return false;
    }
    (void)spawn;

    if (!sr_replay_prefix(config, &w, sp)) {
        world_cleanup(&w);
        return false;
    }

    out->start_station = sp->current_station;
    out->start_pos = sp->ship.pos;
    out->start_dist = v2_len(v2_sub(goal, sp->ship.pos));
    out->start_hull = sp->ship.hull;
    out->start_cargo = ship_total_cargo(&sp->ship);
    if (sp->current_station >= 0 && sp->current_station < MAX_STATIONS) {
        out->start_balance = ledger_balance(&w.stations[sp->current_station],
                                            sp->session_token);
    }
    sr_state_hash(&w, sp, out->prefix_state_hash);

    sha256_init(&event_hash);
    sha256_update(&event_hash, "signal-replay-events-v1", 23);
    for (int i = 0; i < config->horizon_ticks; i++) {
        sr_apply_action(sp, candidate);
        world_sim_step(&w, SIM_DT);
        sr_accumulate_events(&w, &out->events, &event_hash);
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
    if (sp->current_station >= 0 && sp->current_station < MAX_STATIONS) {
        out->end_balance = ledger_balance(&w.stations[sp->current_station],
                                          sp->session_token);
    }
    out->end_docked = sp->docked;
    out->end_current_station = sp->current_station;
    out->end_manifest_count = sp->ship.manifest.count;
    out->utility = ((double)out->progress / 1000.0) -
                   ((double)out->hull_loss * 0.45) -
                   ((double)out->events.damage_amount * 0.25) -
                   ((double)out->events.damage_events * 2.0) -
                   ((double)out->events.death_events * 80.0);
    sr_state_hash(&w, sp, out->state_hash);
    out->ok = true;
    ok = true;

    world_cleanup(&w);
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
            "\"prefix_ticks\":%d,"
            "\"horizon_ticks\":%d,"
            "\"candidate\":%d,"
            "\"candidate_name\":\"%s\",",
            SR_SCHEMA,
            config->seed,
            config->station,
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
            ",\"buy_events\":%d"
            ",\"buy_cost\":%d"
            ",\"buy_quantity\":%d"
            ",\"sell_events\":%d"
            ",\"sell_base\":%d"
            ",\"sell_bonus\":%d"
            ",\"repair_events\":%d"
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
            r->events.buy_events,
            r->events.buy_cost,
            r->events.buy_quantity,
            r->events.sell_events,
            r->events.sell_base,
            r->events.sell_bonus,
            r->events.repair_events,
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
