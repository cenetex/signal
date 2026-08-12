#include "zero_latency_gate.h"

#include "client.h"
#include "input.h"
#include "net_protocol.h"
#include "net_sync.h"
#include "state_digest.h"
#include "station_util.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

enum {
    ZERO_LATENCY_MINING_ASTEROID = MAX_ASTEROIDS - 2,
    ZERO_LATENCY_TOW_ASTEROID = MAX_ASTEROIDS - 1,
    ZERO_LATENCY_NPC = MAX_NPC_SHIPS - 1,
    ZERO_LATENCY_MAX_NORMAL_TICKS = 64,
    ZERO_LATENCY_REPORT_SIZE = NET_RECONCILE_JSON_SIZE * 2,
};

typedef struct {
    int status;
    const char *failure_stage;
    uint32_t scenario_mask;
    uint32_t normal_numeric_drift;
    uint32_t normal_samples;
    uint32_t normal_exact;
    uint32_t normal_input_frontier;
    uint32_t normal_semantic;
    uint32_t normal_asteroid_motion;
    uint32_t normal_npc_motion;
    uint32_t normal_death_respawn;
    uint32_t ticks;
    uint32_t first_tick;
    uint32_t last_tick;
    float mining_hp_before;
    float mining_hp_after;
    float mining_signal;
    int mining_hover_asteroid;
    bool mining_beam_active;
    bool mining_input_active;
    char report[ZERO_LATENCY_REPORT_SIZE];
} zero_latency_gate_result_t;

static zero_latency_gate_result_t zero_latency_result;

static bool zero_latency_ready(world_t **authority_out,
                               server_player_t **server_player_out,
                               server_player_t **client_player_out)
{
    if (!g.local_server.active || !net_is_loopback() ||
        !net_is_gameplay_ready() ||
        g.local_player_slot < 0 ||
        g.local_player_slot >= MAX_PLAYERS) {
        return false;
    }
    world_t *authority = local_server_world(&g.local_server);
    if (!authority) return false;
    server_player_t *server_player =
        &authority->players[g.local_player_slot];
    server_player_t *client_player =
        &g.world.players[g.local_player_slot];
    if (!server_player->connected || !server_player->ship ||
        !server_player->replication ||
        !client_player->connected || !client_player->ship) {
        return false;
    }
    if (authority_out) *authority_out = authority;
    if (server_player_out) *server_player_out = server_player;
    if (client_player_out) *client_player_out = client_player;
    return true;
}

static void zero_latency_clear_nearby_asteroids(world_t *world,
                                                vec2 center,
                                                float radius)
{
    if (!world) return;
    float radius_sq = radius * radius;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!world->asteroids[i].active ||
            v2_dist_sq(world->asteroids[i].pos, center) >= radius_sq) {
            continue;
        }
        world_asteroid_clear_tractor(world, i);
        memset(&world->asteroids[i], 0, sizeof(world->asteroids[i]));
        world->asteroid_generation_live[i] = false;
    }
}

static void zero_latency_clear_client_interpolation(void)
{
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));
}

static void zero_latency_set_player_pose(server_player_t *player,
                                         vec2 pos)
{
    player->docked = false;
    player->docking_approach = false;
    player->in_dock_range = false;
    player->nearby_station = -1;
    player->dock_berth = 0;
    player->ship->pos = pos;
    player->ship->vel = v2(0.0f, 0.0f);
    player->ship->angle = 0.0f;
    player->ship->hull = ship_max_hull(player->ship);
    player->ship->tractor_active = false;
    player->boost_hold_timer = 0.0f;
    memset(&player->input, 0, sizeof(player->input));
    player->input.mining_target_hint = -1;
}

static bool zero_latency_seed_mining_asteroid(world_t *authority,
                                               vec2 player_pos)
{
    asteroid_t seeded = {
        .active = true,
        .fracture_child = false,
        .tier = ASTEROID_TIER_M,
        .commodity = COMMODITY_FERRITE_ORE,
        .pos = {player_pos.x + 120.0f, player_pos.y},
        .vel = {3.0f, 0.5f},
        .hp = 80.0f,
        .max_hp = 80.0f,
        .ore = 30.0f,
        .max_ore = 30.0f,
        .radius = 22.0f,
        .grade = MINING_GRADE_COMMON,
        .net_dirty = true,
    };
    int idx = ZERO_LATENCY_MINING_ASTEROID;
    world_asteroid_clear_tractor(authority, idx);
    memset(&authority->asteroids[idx], 0,
           sizeof(authority->asteroids[idx]));
    authority->asteroid_generation_live[idx] = false;
    authority->asteroids[idx] = seeded;
    (void)world_entity_ref_for_slot(
        authority, ENTITY_KIND_ASTEROID, idx, -1);

    world_asteroid_clear_tractor(&g.world, idx);
    memset(&g.world.asteroids[idx], 0,
           sizeof(g.world.asteroids[idx]));
    g.world.asteroid_generation_live[idx] = false;
    g.world.asteroids[idx] = seeded;
    (void)world_entity_ref_for_slot(
        &g.world, ENTITY_KIND_ASTEROID, idx, -1);
    g.asteroid_interp.curr[idx] = seeded;
    g.asteroid_interp.prev[idx] = seeded;
    g.asteroid_interp.elapsed[idx] = 0.0f;
    return true;
}

static bool zero_latency_seed_npc(world_t *authority, vec2 player_pos)
{
    int idx = ZERO_LATENCY_NPC;
    if (!world_npc_ship_slot_activate(authority, idx))
        return false;
    npc_ship_t *npc = &authority->npc_ships[idx];
    npc->active = true;
    ship_t *ship = world_npc_ship_for(authority, idx);
    if (!ship) return false;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_IDLE;
    npc->state_timer = 1000.0f;
    npc->home_station = 0;
    npc->dest_station = -1;
    npc->pickup_station = -1;
    npc->target_asteroid = -1;
    npc_clear_towed_fragment(npc);
    ship->hull_class = HULL_CLASS_NPC_MINER;
    ship->hull = hull_max_for_class(ship->hull_class);
    ship->towed_scaffold = -1;
    ship->pos = v2(player_pos.x + 1600.0f, player_pos.y + 800.0f);
    ship->vel = v2(7.0f, -2.0f);
    ship->angle = 0.25f;
    return true;
}

static bool zero_latency_prepare_flight(bool reset_diagnostics)
{
    world_t *authority = NULL;
    server_player_t *server_player = NULL;
    server_player_t *client_player = NULL;
    if (!zero_latency_ready(
            &authority, &server_player, &client_player)) {
        return false;
    }

    const vec2 start = {2400.0f, -2400.0f};
    zero_latency_clear_nearby_asteroids(authority, start, 1800.0f);
    zero_latency_clear_nearby_asteroids(&g.world, start, 1800.0f);
    zero_latency_clear_client_interpolation();

    world_tow_links_clear_source(authority, server_player->ship_ref);
    world_tow_links_clear_source(&g.world, client_player->ship_ref);
    zero_latency_set_player_pose(server_player, start);
    zero_latency_set_player_pose(client_player, start);
    server_player->movement_queue_count = 0;
    server_player->session_ready = true;

    uint16_t base_seq = server_player->last_input_seq;
    if (base_seq == 0) base_seq = 100u;
    server_player->last_input_seq = base_seq;
    server_player->last_input_tick = authority->tick;
    g.net_input_seq = base_seq;
    g.net_last_server_ack = base_seq;
    g.net_last_server_tick = authority->tick;
    g.net_last_server_tick_time = g.net_time;
    g.net_input_have_last = false;
    g.net_input_tick_protocol = true;
    g.net_local_state_ready = true;
    g.action_predict_timer = 0.0f;
    g.death_cinematic.active = false;
    g.death_screen_timer = 0.0f;
    g.local_player_render_offset = v2(0.0f, 0.0f);
    net_replay_reset();
    net_anchor_prediction_tick(authority->tick, true);
    server_player_reset_authoritative_ack_state(server_player);
    server_player->replication->force_authoritative_resync = true;

    if (!zero_latency_seed_mining_asteroid(authority, start) ||
        !zero_latency_seed_npc(authority, start)) {
        return false;
    }
    if (reset_diagnostics)
        net_reconcile_diagnostics_reset(&g.net_reconcile);
    return true;
}

static uint16_t zero_latency_next_input_seq(void)
{
    g.net_input_seq++;
    if (g.net_input_seq == 0) g.net_input_seq++;
    return g.net_input_seq;
}

static bool zero_latency_send_action(uint8_t action, uint16_t action_id)
{
    if (action == NET_ACTION_NONE) return true;
    if (!net_has_identity_pubkey()) return false;
    if (!net_has_identity_secret()) return false;

    uint8_t payload[7] = {
        action,
        MINING_GRADE_COUNT,
        (uint8_t)-1,
        (uint8_t)-1,
        (uint8_t)-1,
        (uint8_t)(action_id & 0xffu),
        (uint8_t)(action_id >> 8),
    };
    return net_send_signed_action(
        SIGNED_ACTION_INPUT_ACTION, payload, sizeof(payload));
}

static bool zero_latency_step(const input_intent_t *intent, uint8_t action)
{
    world_t *authority = NULL;
    server_player_t *server_player = NULL;
    if (!intent || !zero_latency_ready(
            &authority, &server_player, NULL)) {
        return false;
    }
    if (zero_latency_result.ticks >= ZERO_LATENCY_MAX_NORMAL_TICKS)
        return false;

    uint32_t target_tick = authority->tick + 1u;
    if (target_tick == 0) return false;
    uint16_t seq = zero_latency_next_input_seq();
    uint8_t flags = input_intent_net_flags(intent);
    uint16_t mining_target = intent->mine
        ? (uint16_t)intent->mining_target_hint : 0xffffu;
    uint16_t action_id =
        action == NET_ACTION_NONE ? 0u : seq;

    if (!zero_latency_send_action(action, action_id))
        return false;
    (void)net_send_input(
        flags, NET_ACTION_NONE, seq, mining_target,
        MINING_GRADE_COUNT, -1, -1, -1,
        0, target_tick);

    /*
     * Canonical zero-delay order: schedule the target tick, predict that
     * exact tick, then let the local authority consume the queued command
     * and synchronously encode/decode its snapshot. Every iteration is one
     * fixed SIM_DT; browser frame time is intentionally absent.
     */
    submit_input(intent, SIM_DT);
    server_player->replication->force_authoritative_resync = true;
    g.net_time += SIM_DT;
    local_server_step_loopback(
        &g.local_server, g.local_player_slot, SIM_DT);

    zero_latency_result.ticks++;
    zero_latency_result.last_tick = authority->tick;
    return authority->tick == target_tick &&
        server_player->last_input_seq == seq &&
        server_player->last_input_tick == target_tick &&
        g.net_last_server_ack == seq;
}

static bool zero_latency_steps(input_intent_t intent,
                               uint8_t action,
                               int count)
{
    for (int i = 0; i < count; i++) {
        if (!zero_latency_step(&intent, i == 0 ? action : NET_ACTION_NONE))
            return false;
    }
    return true;
}

static bool zero_latency_seed_tow(world_t *authority,
                                  server_player_t *server_player,
                                  server_player_t *client_player)
{
    int idx = ZERO_LATENCY_TOW_ASTEROID;
    asteroid_t seeded = {
        .active = true,
        .fracture_child = true,
        .tier = ASTEROID_TIER_S,
        .commodity = COMMODITY_CUPRITE_ORE,
        .pos = {
            server_player->ship->pos.x - 70.0f,
            server_player->ship->pos.y + 20.0f,
        },
        .vel = {0.0f, 0.0f},
        .hp = 8.0f,
        .max_hp = 8.0f,
        .ore = 4.0f,
        .max_ore = 4.0f,
        .radius = 14.0f,
        .grade = MINING_GRADE_COMMON,
        .net_dirty = true,
    };

    world_asteroid_clear_tractor(authority, idx);
    memset(&authority->asteroids[idx], 0,
           sizeof(authority->asteroids[idx]));
    authority->asteroid_generation_live[idx] = false;
    authority->asteroids[idx] = seeded;
    if (!world_asteroid_set_player_tractor(
            authority, idx, g.local_player_slot)) {
        return false;
    }

    world_asteroid_clear_tractor(&g.world, idx);
    memset(&g.world.asteroids[idx], 0,
           sizeof(g.world.asteroids[idx]));
    g.world.asteroid_generation_live[idx] = false;
    g.world.asteroids[idx] = seeded;
    if (!world_asteroid_set_player_tractor(
            &g.world, idx, g.local_player_slot)) {
        return false;
    }
    g.asteroid_interp.curr[idx] = seeded;
    g.asteroid_interp.prev[idx] = seeded;
    g.asteroid_interp.elapsed[idx] = 0.0f;
    return server_player->ship->towed_count == 1 &&
        client_player->ship->towed_count == 1;
}

static bool zero_latency_move_to_dock(world_t *authority,
                                      server_player_t *server_player,
                                      server_player_t *client_player)
{
    int station_idx = 0;
    station_t *station = &authority->stations[station_idx];
    int dock_module = -1;
    for (int i = 0; i < station->module_count; i++) {
        if (!station->modules[i].scaffold &&
            station->modules[i].type == MODULE_DOCK) {
            dock_module = i;
            break;
        }
    }
    if (dock_module < 0) return false;
    const station_module_t *dock = &station->modules[dock_module];
    vec2 module_pos = module_world_pos_ring(
        station, dock->ring, dock->slot);
    float angle = module_angle_ring(station, dock->ring, dock->slot);
    vec2 berth = v2_add(module_pos, v2_scale(v2_from_angle(angle), 55.0f));

    server_player->ship->pos = berth;
    server_player->ship->vel = v2(0.0f, 0.0f);
    server_player->nearby_station = station_idx;
    server_player->in_dock_range = true;
    server_player->docking_approach = false;
    server_player->dock_berth = 0;

    client_player->ship->pos = berth;
    client_player->ship->vel = v2(0.0f, 0.0f);
    client_player->nearby_station = station_idx;
    client_player->in_dock_range = true;
    client_player->docking_approach = false;
    client_player->dock_berth = 0;
    g.local_player_render_offset = v2(0.0f, 0.0f);
    return true;
}

static void zero_latency_capture_normal_result(void)
{
    zero_latency_result.normal_numeric_drift =
        g.net_reconcile.class_count[NET_RECONCILE_NUMERIC_DRIFT];
    zero_latency_result.normal_samples = g.net_reconcile.total_samples;
    zero_latency_result.normal_exact =
        g.net_reconcile.class_count[NET_RECONCILE_EXACT];
    zero_latency_result.normal_input_frontier =
        g.net_reconcile.class_count[NET_RECONCILE_INPUT_FRONTIER];
    zero_latency_result.normal_semantic =
        g.net_reconcile.class_count[
            NET_RECONCILE_SEMANTIC_DISCONTINUITY];
    zero_latency_result.normal_asteroid_motion =
        g.net_reconcile.asteroid_motion_samples;
    zero_latency_result.normal_npc_motion =
        g.net_reconcile.npc_motion_samples;
    zero_latency_result.normal_death_respawn =
        g.net_reconcile.death_respawn_events;
}

static void zero_latency_format_report(void)
{
    char drift[NET_RECONCILE_JSON_SIZE];
    if (net_reconcile_first_drift_json(
            &g.net_reconcile,
            signal_authoritative_state_digest_schema(),
            drift, sizeof(drift)) < 0) {
        snprintf(drift, sizeof(drift), "{\"error\":\"artifact-overflow\"}");
    }
    snprintf(
        zero_latency_result.report,
        sizeof(zero_latency_result.report),
        "{\"status\":%d,\"failure_stage\":\"%s\","
        "\"scenario_mask\":%" PRIu32
        ",\"required_mask\":%u,\"ticks\":%" PRIu32
        ",\"first_tick\":%" PRIu32 ",\"last_tick\":%" PRIu32
        ",\"mining\":{\"hp_before\":%.9g,\"hp_after\":%.9g,"
        "\"signal\":%.9g,\"hover\":%d,\"beam\":%u,\"input\":%u}"
        ",\"normal\":{\"samples\":%" PRIu32 ",\"exact\":%" PRIu32
        ",\"input_frontier\":%" PRIu32 ",\"semantic\":%" PRIu32
        ",\"numeric_drift\":%" PRIu32 ",\"asteroid_motion\":%" PRIu32
        ",\"npc_motion\":%" PRIu32 ",\"death_respawn\":%" PRIu32
        "},\"current_numeric_drift\":%u,\"first_drift\":%s}",
        zero_latency_result.status,
        zero_latency_result.failure_stage
            ? zero_latency_result.failure_stage : "",
        zero_latency_result.scenario_mask,
        (unsigned)SIGNAL_ZERO_LATENCY_REQUIRED_MASK,
        zero_latency_result.ticks,
        zero_latency_result.first_tick,
        zero_latency_result.last_tick,
        (double)zero_latency_result.mining_hp_before,
        (double)zero_latency_result.mining_hp_after,
        (double)zero_latency_result.mining_signal,
        zero_latency_result.mining_hover_asteroid,
        zero_latency_result.mining_beam_active ? 1u : 0u,
        zero_latency_result.mining_input_active ? 1u : 0u,
        zero_latency_result.normal_samples,
        zero_latency_result.normal_exact,
        zero_latency_result.normal_input_frontier,
        zero_latency_result.normal_semantic,
        zero_latency_result.normal_numeric_drift,
        zero_latency_result.normal_asteroid_motion,
        zero_latency_result.normal_npc_motion,
        zero_latency_result.normal_death_respawn,
        (unsigned)g.net_reconcile.class_count[
            NET_RECONCILE_NUMERIC_DRIFT],
        drift);
}

EMSCRIPTEN_KEEPALIVE
int signal_zero_latency_gate_run(void)
{
    memset(&zero_latency_result, 0, sizeof(zero_latency_result));
    zero_latency_result.failure_stage = "prepare";
    world_t *authority = NULL;
    server_player_t *server_player = NULL;
    server_player_t *client_player = NULL;
    if (!zero_latency_prepare_flight(true) ||
        !zero_latency_ready(
            &authority, &server_player, &client_player)) {
        zero_latency_result.status = -1;
        zero_latency_format_report();
        return zero_latency_result.status;
    }

    bool saved_throttled = g.local_server.throttled_snapshots;
    g.local_server.throttled_snapshots = false;
    zero_latency_result.first_tick = authority->tick;
    float mining_hp_before =
        authority->asteroids[ZERO_LATENCY_MINING_ASTEROID].hp;
    vec2 motion_asteroid_before =
        authority->asteroids[ZERO_LATENCY_MINING_ASTEROID].pos;
    ship_t *motion_npc =
        world_npc_ship_for(authority, ZERO_LATENCY_NPC);
    if (!motion_npc) goto failed;
    vec2 motion_npc_before = motion_npc->pos;

    input_intent_t intent = {0};
    intent.mining_target_hint = -1;
    intent.thrust = 1.0f;
    zero_latency_result.failure_stage = "flight";
    if (!zero_latency_steps(intent, NET_ACTION_NONE, 4)) goto failed;
    intent.turn = 1.0f;
    if (!zero_latency_steps(intent, NET_ACTION_NONE, 3)) goto failed;
    intent.turn = -1.0f;
    if (!zero_latency_steps(intent, NET_ACTION_NONE, 3)) goto failed;
    zero_latency_result.scenario_mask |= SIGNAL_ZERO_LATENCY_FLIGHT;

    intent.turn = 0.0f;
    intent.boost = true;
    zero_latency_result.failure_stage = "boost";
    if (!zero_latency_steps(intent, NET_ACTION_NONE, 4)) goto failed;
    zero_latency_result.scenario_mask |= SIGNAL_ZERO_LATENCY_BOOST;

    intent.thrust = 0.0f;
    intent.boost = false;
    intent.mine = true;
    intent.mining_target_hint = ZERO_LATENCY_MINING_ASTEROID;
    zero_latency_result.failure_stage = "mining";
    if (!zero_latency_steps(intent, NET_ACTION_NONE, 3)) goto failed;
    zero_latency_result.mining_hp_before = mining_hp_before;
    zero_latency_result.mining_hp_after =
        authority->asteroids[ZERO_LATENCY_MINING_ASTEROID].hp;
    zero_latency_result.mining_signal =
        signal_strength_at(authority, server_player->ship->pos);
    zero_latency_result.mining_hover_asteroid =
        server_player->hover_asteroid;
    zero_latency_result.mining_beam_active = server_player->beam_active;
    zero_latency_result.mining_input_active = server_player->input.mine;
    if (!server_player->beam_active ||
        authority->asteroids[ZERO_LATENCY_MINING_ASTEROID].hp >=
            mining_hp_before) {
        goto failed;
    }
    zero_latency_result.scenario_mask |=
        SIGNAL_ZERO_LATENCY_MINING;

    intent.mine = false;
    intent.mining_target_hint = -1;
    zero_latency_result.failure_stage = "tow";
    if (!zero_latency_seed_tow(
            authority, server_player, client_player)) {
        goto failed;
    }
    intent.tractor_hold = true;
    vec2 tow_pos_before =
        authority->asteroids[ZERO_LATENCY_TOW_ASTEROID].pos;
    if (!zero_latency_steps(intent, NET_ACTION_NONE, 3)) goto failed;
    entity_ref_t tow_ref = world_entity_ref_for_slot(
        authority, ENTITY_KIND_ASTEROID,
        ZERO_LATENCY_TOW_ASTEROID, -1);
    const tow_link_t *tow_link =
        world_tow_link_for_target_const(authority, tow_ref);
    if (server_player->ship->towed_count == 0 ||
        client_player->ship->towed_count == 0 ||
        !tow_link ||
        tow_link->state != TOW_LINK_HELD ||
        v2_dist_sq(
            authority->asteroids[ZERO_LATENCY_TOW_ASTEROID].pos,
            tow_pos_before) == 0.0f) {
        goto failed;
    }
    zero_latency_result.scenario_mask |= SIGNAL_ZERO_LATENCY_TOWING;

    intent.tractor_hold = false;
    zero_latency_result.failure_stage = "release";
    if (!zero_latency_steps(intent, NET_ACTION_RELEASE_TOW, 1))
        goto failed;
    if (server_player->ship->towed_count != 0 ||
        client_player->ship->towed_count != 0 ||
        world_tow_link_for_target_const(authority, tow_ref) != NULL) {
        goto failed;
    }
    zero_latency_result.scenario_mask |= SIGNAL_ZERO_LATENCY_RELEASE;

    if (!zero_latency_move_to_dock(
            authority, server_player, client_player)) {
        goto failed;
    }
    zero_latency_result.failure_stage = "dock";
    if (!zero_latency_steps(intent, NET_ACTION_DOCK, 1))
        goto failed;
    if (!server_player->docked || !client_player->docked)
        goto failed;
    zero_latency_result.scenario_mask |= SIGNAL_ZERO_LATENCY_DOCK;

    if (!zero_latency_steps(intent, NET_ACTION_NONE, 1))
        goto failed;
    zero_latency_result.failure_stage = "launch";
    if (!zero_latency_steps(intent, NET_ACTION_LAUNCH, 1))
        goto failed;
    if (server_player->docked || client_player->docked)
        goto failed;
    zero_latency_result.scenario_mask |= SIGNAL_ZERO_LATENCY_LAUNCH;

    zero_latency_result.failure_stage = "death-respawn";
    if (!zero_latency_steps(intent, NET_ACTION_RESET, 1))
        goto failed;
    if (!server_player->docked || !client_player->docked ||
        g.net_reconcile.death_respawn_events == 0) {
        goto failed;
    }
    zero_latency_result.scenario_mask |=
        SIGNAL_ZERO_LATENCY_DEATH_RESPAWN;
    if (g.net_reconcile.asteroid_motion_samples > 0 &&
        v2_dist_sq(
            authority->asteroids[ZERO_LATENCY_MINING_ASTEROID].pos,
            motion_asteroid_before) > 0.0f) {
        zero_latency_result.scenario_mask |=
            SIGNAL_ZERO_LATENCY_ASTEROID_MOTION;
    }
    if (g.net_reconcile.npc_motion_samples > 0 &&
        v2_dist_sq(motion_npc->pos, motion_npc_before) > 0.0f) {
        zero_latency_result.scenario_mask |=
            SIGNAL_ZERO_LATENCY_NPC_MOTION;
    }

    zero_latency_capture_normal_result();
    zero_latency_result.failure_stage = "";
    zero_latency_result.status =
        zero_latency_result.scenario_mask ==
            SIGNAL_ZERO_LATENCY_REQUIRED_MASK &&
        zero_latency_result.normal_samples > 0 &&
        zero_latency_result.normal_exact > 0 &&
        zero_latency_result.normal_numeric_drift == 0
            ? 1 : -2;
    g.local_server.throttled_snapshots = saved_throttled;
    zero_latency_format_report();
    return zero_latency_result.status;

failed:
    zero_latency_capture_normal_result();
    zero_latency_result.status = -3;
    g.local_server.throttled_snapshots = saved_throttled;
    zero_latency_format_report();
    return zero_latency_result.status;
}

EMSCRIPTEN_KEEPALIVE
int signal_zero_latency_gate_perturb(void)
{
    if (zero_latency_result.status != 1 ||
        !zero_latency_prepare_flight(true)) {
        zero_latency_result.status = -4;
        zero_latency_format_report();
        return zero_latency_result.status;
    }

    world_t *authority = NULL;
    server_player_t *server_player = NULL;
    server_player_t *client_player = NULL;
    if (!zero_latency_ready(
            &authority, &server_player, &client_player)) {
        zero_latency_result.status = -5;
        zero_latency_format_report();
        return zero_latency_result.status;
    }
    bool saved_throttled = g.local_server.throttled_snapshots;
    g.local_server.throttled_snapshots = false;

    input_intent_t intent = {0};
    intent.mining_target_hint = -1;
    uint32_t target_tick = authority->tick + 1u;
    uint16_t seq = zero_latency_next_input_seq();
    (void)net_send_input(
        0, NET_ACTION_NONE, seq, 0xffffu,
        MINING_GRADE_COUNT, -1, -1, -1, 0, target_tick);
    submit_input(&intent, SIM_DT);

    uint32_t perturbed = 0;
    memcpy(&perturbed, &client_player->ship->pos.x, sizeof(perturbed));
    perturbed ^= 1u;
    memcpy(&client_player->ship->pos.x, &perturbed, sizeof(perturbed));

    server_player->replication->force_authoritative_resync = true;
    g.net_time += SIM_DT;
    local_server_step_loopback(
        &g.local_server, g.local_player_slot, SIM_DT);
    g.local_server.throttled_snapshots = saved_throttled;
    zero_latency_result.ticks++;
    zero_latency_result.last_tick = authority->tick;

    bool passed =
        authority->tick == target_tick &&
        g.net_reconcile.class_count[
            NET_RECONCILE_NUMERIC_DRIFT] == 1u &&
        g.net_reconcile.first_numeric_drift_valid &&
        g.net_reconcile.first_domain ==
            NET_RECONCILE_DOMAIN_PLAYER_POS_X &&
        g.net_reconcile.first_server_tick ==
            g.net_reconcile.first_prediction_tick &&
        g.net_reconcile.first_predicted_input_seq ==
            g.net_reconcile.first_authoritative_input_seq &&
        g.net_reconcile.first_authoritative_root_valid;
    zero_latency_result.status = passed ? 2 : -6;
    zero_latency_format_report();
    return passed ? 1 : zero_latency_result.status;
}

EMSCRIPTEN_KEEPALIVE
int signal_zero_latency_gate_ready(void)
{
    return zero_latency_ready(NULL, NULL, NULL) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int signal_zero_latency_gate_status(void)
{
    return zero_latency_result.status;
}

EMSCRIPTEN_KEEPALIVE
int signal_zero_latency_gate_scenario_mask(void)
{
    return (int)zero_latency_result.scenario_mask;
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_exact_samples(void)
{
    return (int)g.net_reconcile.class_count[NET_RECONCILE_EXACT];
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_bootstrap_samples(void)
{
    return (int)g.net_reconcile.class_count[NET_RECONCILE_BOOTSTRAP];
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_input_frontier_samples(void)
{
    return (int)g.net_reconcile.class_count[NET_RECONCILE_INPUT_FRONTIER];
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_semantic_samples(void)
{
    return (int)g.net_reconcile.class_count[
        NET_RECONCILE_SEMANTIC_DISCONTINUITY];
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_transport_recovery_samples(void)
{
    return (int)g.net_reconcile.class_count[
        NET_RECONCILE_TRANSPORT_RECOVERY];
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_numeric_drift_samples(void)
{
    return (int)g.net_reconcile.class_count[
        NET_RECONCILE_NUMERIC_DRIFT];
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_asteroid_motion_samples(void)
{
    return (int)g.net_reconcile.asteroid_motion_samples;
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_npc_motion_samples(void)
{
    return (int)g.net_reconcile.npc_motion_samples;
}

EMSCRIPTEN_KEEPALIVE
int get_net_reconcile_death_respawn_events(void)
{
    return (int)g.net_reconcile.death_respawn_events;
}

EMSCRIPTEN_KEEPALIVE
const char *get_net_reconcile_first_drift_json(void)
{
    static char json[NET_RECONCILE_JSON_SIZE];
    if (net_reconcile_first_drift_json(
            &g.net_reconcile,
            signal_authoritative_state_digest_schema(),
            json, sizeof(json)) < 0) {
        snprintf(json, sizeof(json), "{\"error\":\"artifact-overflow\"}");
    }
    return json;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_zero_latency_gate_report_json(void)
{
    zero_latency_format_report();
    return zero_latency_result.report;
}
