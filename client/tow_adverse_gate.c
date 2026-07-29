#include "tow_adverse_gate.h"

#include "client.h"
#include "net_sync.h"
#include "tow_presentation_diagnostics.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

enum {
    TOW_GATE_TARGET_INDEX = MAX_CARGO_PODS - 1,
    TOW_GATE_PAYLOAD_INTERVAL_MS = 25,
    TOW_GATE_PAYLOAD_COUNT = 81,
    TOW_GATE_PACKET_COUNT = TOW_GATE_PAYLOAD_COUNT * 2,
    TOW_GATE_REPORT_SIZE = 4096,

    TOW_GATE_ATTACH = 1u << 0,
    TOW_GATE_RELEASE = 1u << 1,
    TOW_GATE_REATTACH = 1u << 2,
    TOW_GATE_RETIRE = 1u << 3,
    TOW_GATE_REPLACEMENT_CLEAR = 1u << 4,
    TOW_GATE_REPLACEMENT_ATTACH = 1u << 5,
    TOW_GATE_RELEVANCE_EXIT = 1u << 6,
    TOW_GATE_RELEVANCE_REENTRY = 1u << 7,
    TOW_GATE_FINAL_RELEASE = 1u << 8,
};

#define TOW_GATE_REQUIRED_LIFECYCLE \
    (TOW_GATE_ATTACH | TOW_GATE_RELEASE | TOW_GATE_REATTACH | \
     TOW_GATE_RETIRE | TOW_GATE_REPLACEMENT_CLEAR | \
     TOW_GATE_REPLACEMENT_ATTACH | TOW_GATE_RELEVANCE_EXIT | \
     TOW_GATE_RELEVANCE_REENTRY | TOW_GATE_FINAL_RELEASE)

typedef struct {
    uint32_t send_ms;
    uint32_t relation_revision;
    uint16_t target_generation;
    bool target_active;
    bool relation_active;
    NetCargoPodState target;
} tow_gate_payload_t;

typedef struct {
    uint8_t towed_count;
    int16_t towed_fragments[10];
    uint8_t towed_pod_count;
    int16_t towed_pods[10];
    int16_t towed_scaffold;
} tow_gate_ship_projection_t;

typedef struct {
    uint8_t prev_count;
    uint8_t curr_count;
    uint16_t prev_fragments[10];
    uint16_t curr_fragments[10];
} tow_gate_player_projection_t;

typedef struct {
    int16_t prev_fragment;
    int16_t curr_fragment;
    int16_t prev_scaffold;
    int16_t curr_scaffold;
} tow_gate_npc_projection_t;

typedef struct {
    tow_link_t tow_links[MAX_TOW_LINKS];
    uint32_t tow_revision;
    uint32_t tow_revision_tick;
    bool tow_snapshot_received;
    uint32_t tow_snapshot_revision;
    uint32_t tow_snapshot_server_tick;

    cargo_pod_t target_prev;
    cargo_pod_t target_curr;
    bool cargo_prev_active[MAX_CARGO_PODS];
    float cargo_elapsed[MAX_CARGO_PODS];

    tractor_binding_t asteroid_world[MAX_ASTEROIDS];
    tractor_binding_t asteroid_prev[MAX_ASTEROIDS];
    tractor_binding_t asteroid_curr[MAX_ASTEROIDS];
    tractor_binding_t cargo_world[MAX_CARGO_PODS];
    tractor_binding_t cargo_prev[MAX_CARGO_PODS];
    tractor_binding_t cargo_curr[MAX_CARGO_PODS];
    tractor_binding_t scaffold_world[MAX_SCAFFOLDS];
    tractor_binding_t scaffold_prev[MAX_SCAFFOLDS];
    tractor_binding_t scaffold_curr[MAX_SCAFFOLDS];
    tow_gate_ship_projection_t ships[WORLD_SHIP_CAP];
    tow_gate_player_projection_t players[NET_MAX_PLAYERS];
    tow_gate_npc_projection_t npcs[MAX_NPC_SHIPS];
} tow_gate_restore_t;

typedef struct {
    uint16_t latency_ms;
    bool passed;
    const char *failure;
    uint32_t lifecycle_mask;
    uint32_t stale_projection_failures;
    uint32_t post_reentry_relation_packets;
    tow_adverse_schedule_stats_t schedule;
    tow_presentation_diagnostics_t presentation;
} tow_gate_profile_result_t;

static tow_gate_profile_result_t tow_gate_results[3];
static char tow_gate_report[TOW_GATE_REPORT_SIZE];

static void tow_gate_save(tow_gate_restore_t *saved)
{
    memcpy(saved->tow_links, g.world.tow_links,
           sizeof(saved->tow_links));
    saved->tow_revision = g.world.tow_revision;
    saved->tow_revision_tick = g.world.tow_revision_tick;
    saved->tow_snapshot_received = g.tow_snapshot_received;
    saved->tow_snapshot_revision = g.tow_snapshot_revision;
    saved->tow_snapshot_server_tick = g.tow_snapshot_server_tick;
    saved->target_prev =
        g.cargo_pod_interp.prev[TOW_GATE_TARGET_INDEX];
    saved->target_curr =
        g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX];

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        saved->cargo_prev_active[i] =
            g.cargo_pod_interp.prev[i].active;
        saved->cargo_elapsed[i] = g.cargo_pod_interp.elapsed[i];
        saved->cargo_world[i] = g.world.cargo_pods[i].tractor;
        saved->cargo_prev[i] = g.cargo_pod_interp.prev[i].tractor;
        saved->cargo_curr[i] = g.cargo_pod_interp.curr[i].tractor;
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        saved->asteroid_world[i] = g.world.asteroids[i].tractor;
        saved->asteroid_prev[i] = g.asteroid_interp.prev[i].tractor;
        saved->asteroid_curr[i] = g.asteroid_interp.curr[i].tractor;
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        saved->scaffold_world[i] = g.world.scaffolds[i].tractor;
        saved->scaffold_prev[i] = g.scaffold_interp.prev[i].tractor;
        saved->scaffold_curr[i] = g.scaffold_interp.curr[i].tractor;
    }
    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        const ship_t *ship = &g.world.ships[i].component;
        tow_gate_ship_projection_t *projection = &saved->ships[i];
        projection->towed_count = ship->towed_count;
        memcpy(projection->towed_fragments, ship->towed_fragments,
               sizeof(projection->towed_fragments));
        projection->towed_pod_count = ship->towed_pod_count;
        memcpy(projection->towed_pods, ship->towed_pods,
               sizeof(projection->towed_pods));
        projection->towed_scaffold = ship->towed_scaffold;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        tow_gate_player_projection_t *projection = &saved->players[i];
        projection->prev_count = g.player_interp.prev[i].towed_count;
        projection->curr_count = g.player_interp.curr[i].towed_count;
        memcpy(projection->prev_fragments,
               g.player_interp.prev[i].towed_fragments,
               sizeof(projection->prev_fragments));
        memcpy(projection->curr_fragments,
               g.player_interp.curr[i].towed_fragments,
               sizeof(projection->curr_fragments));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        saved->npcs[i] = (tow_gate_npc_projection_t){
            .prev_fragment = g.npc_interp.prev[i].towed_fragment,
            .curr_fragment = g.npc_interp.curr[i].towed_fragment,
            .prev_scaffold = g.npc_interp.prev[i].towed_scaffold,
            .curr_scaffold = g.npc_interp.curr[i].towed_scaffold,
        };
    }
}

static void tow_gate_restore(const tow_gate_restore_t *saved)
{
    memcpy(g.world.tow_links, saved->tow_links,
           sizeof(saved->tow_links));
    g.world.tow_revision = saved->tow_revision;
    g.world.tow_revision_tick = saved->tow_revision_tick;
    g.tow_snapshot_received = saved->tow_snapshot_received;
    g.tow_snapshot_revision = saved->tow_snapshot_revision;
    g.tow_snapshot_server_tick = saved->tow_snapshot_server_tick;
    g.cargo_pod_interp.prev[TOW_GATE_TARGET_INDEX] =
        saved->target_prev;
    g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX] =
        saved->target_curr;

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        g.cargo_pod_interp.prev[i].active =
            saved->cargo_prev_active[i];
        g.cargo_pod_interp.elapsed[i] = saved->cargo_elapsed[i];
        g.world.cargo_pods[i].tractor = saved->cargo_world[i];
        g.cargo_pod_interp.prev[i].tractor = saved->cargo_prev[i];
        g.cargo_pod_interp.curr[i].tractor = saved->cargo_curr[i];
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        g.world.asteroids[i].tractor = saved->asteroid_world[i];
        g.asteroid_interp.prev[i].tractor = saved->asteroid_prev[i];
        g.asteroid_interp.curr[i].tractor = saved->asteroid_curr[i];
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        g.world.scaffolds[i].tractor = saved->scaffold_world[i];
        g.scaffold_interp.prev[i].tractor = saved->scaffold_prev[i];
        g.scaffold_interp.curr[i].tractor = saved->scaffold_curr[i];
    }
    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        ship_t *ship = &g.world.ships[i].component;
        const tow_gate_ship_projection_t *projection =
            &saved->ships[i];
        ship->towed_count = projection->towed_count;
        memcpy(ship->towed_fragments, projection->towed_fragments,
               sizeof(projection->towed_fragments));
        ship->towed_pod_count = projection->towed_pod_count;
        memcpy(ship->towed_pods, projection->towed_pods,
               sizeof(projection->towed_pods));
        ship->towed_scaffold = projection->towed_scaffold;
    }
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        const tow_gate_player_projection_t *projection =
            &saved->players[i];
        g.player_interp.prev[i].towed_count =
            projection->prev_count;
        g.player_interp.curr[i].towed_count =
            projection->curr_count;
        memcpy(g.player_interp.prev[i].towed_fragments,
               projection->prev_fragments,
               sizeof(projection->prev_fragments));
        memcpy(g.player_interp.curr[i].towed_fragments,
               projection->curr_fragments,
               sizeof(projection->curr_fragments));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        g.npc_interp.prev[i].towed_fragment =
            saved->npcs[i].prev_fragment;
        g.npc_interp.curr[i].towed_fragment =
            saved->npcs[i].curr_fragment;
        g.npc_interp.prev[i].towed_scaffold =
            saved->npcs[i].prev_scaffold;
        g.npc_interp.curr[i].towed_scaffold =
            saved->npcs[i].curr_scaffold;
    }
}

static uint32_t tow_gate_revision_at(uint32_t send_ms)
{
    if (send_ms < 100u) return 1u;
    if (send_ms < 350u) return 2u;
    if (send_ms < 400u) return 3u;
    if (send_ms < 650u) return 4u;
    if (send_ms < 1050u) return 5u;
    if (send_ms < 1750u) return 6u;
    return 7u;
}

static bool tow_gate_relation_active_at(uint32_t send_ms)
{
    return (send_ms >= 100u && send_ms < 350u) ||
           (send_ms >= 400u && send_ms < 650u) ||
           (send_ms >= 1050u && send_ms < 1750u);
}

static bool tow_gate_target_active_at(uint32_t send_ms)
{
    return !((send_ms >= 650u && send_ms < 900u) ||
             (send_ms >= 1300u && send_ms < 1500u));
}

static void tow_gate_target_motion(uint32_t send_ms,
                                   vec2 *pos,
                                   vec2 *vel)
{
    const float t = (float)send_ms * 0.001f;
    const float x_phase = TWO_PI_F * 0.40f * t;
    const float y_phase = TWO_PI_F * 0.35f * t;
    *pos = v2(
        40.0f + 24.0f * t + 1.5f * sinf(x_phase),
        -20.0f + 8.0f * t + cosf(y_phase));
    *vel = v2(
        24.0f + 1.5f * TWO_PI_F * 0.40f * cosf(x_phase),
        8.0f - TWO_PI_F * 0.35f * sinf(y_phase));
}

static int tow_gate_build_payloads(tow_gate_payload_t *payloads,
                                   tow_adverse_packet_t *packets)
{
    uint32_t sequence = 1u;
    int packet_count = 0;
    for (int i = 0; i < TOW_GATE_PAYLOAD_COUNT; i++) {
        uint32_t send_ms =
            (uint32_t)i * TOW_GATE_PAYLOAD_INTERVAL_MS;
        vec2 pos;
        vec2 vel;
        tow_gate_target_motion(send_ms, &pos, &vel);
        payloads[i] = (tow_gate_payload_t){
            .send_ms = send_ms,
            .relation_revision = tow_gate_revision_at(send_ms),
            .target_generation = send_ms < 900u ? 1u : 2u,
            .target_active = tow_gate_target_active_at(send_ms),
            .relation_active = tow_gate_relation_active_at(send_ms),
            .target = {
                .index = TOW_GATE_TARGET_INDEX,
                .kind = CARGO_POD_CARGO,
                .commodity = COMMODITY_FERRITE_ORE,
                .tractor_player = -1,
                .pos_x = pos.x,
                .pos_y = pos.y,
                .vel_x = vel.x,
                .vel_y = vel.y,
                .radius = 12.0f,
                .quantity = 1,
            },
        };
        packets[packet_count++] = (tow_adverse_packet_t){
            .sequence = sequence++,
            .send_ms = send_ms,
            .payload_index = (uint32_t)i,
            .channel = TOW_ADVERSE_CHANNEL_TARGET,
        };
        /*
         * From relevance exit at 1300 ms through re-entry at 1500 ms,
         * deliberately withhold every relation snapshot until the final
         * release at 1750 ms. The blackout begins far enough before re-entry
         * that even this scheduler's maximum jitter/reordering cannot leave
         * an older relation packet in flight. The accepted revision-six
         * relation must be projected from the roster transition itself.
         */
        if (send_ms < 1300u || send_ms >= 1750u) {
            packets[packet_count++] = (tow_adverse_packet_t){
                .sequence = sequence++,
                .send_ms = send_ms,
                .payload_index = (uint32_t)i,
                .channel = TOW_ADVERSE_CHANNEL_RELATION,
            };
        }
    }
    return packet_count;
}

static const tow_link_t *tow_gate_current_link(void)
{
    for (int i = 0; i < MAX_TOW_LINKS; i++) {
        const tow_link_t *link = &g.world.tow_links[i];
        if (link->active &&
            link->target.kind == ENTITY_KIND_CARGO_POD &&
            link->target.index == TOW_GATE_TARGET_INDEX) {
            return link;
        }
    }
    return NULL;
}

static bool tow_gate_binding_matches(const tractor_binding_t *binding,
                                     int player_slot,
                                     uint16_t source_generation)
{
    return binding &&
        binding->kind == TRACTOR_SOURCE_PLAYER &&
        binding->source_index == player_slot &&
        binding->source_part == -1 &&
        binding->source_generation == source_generation;
}

static bool tow_gate_source_projects_target(const ship_t *ship)
{
    if (!ship) return false;
    int cap = (int)(sizeof(ship->towed_pods) /
                    sizeof(ship->towed_pods[0]));
    int count = ship->towed_pod_count;
    if (count > cap) count = cap;
    for (int i = 0; i < count; i++) {
        if (ship->towed_pods[i] == TOW_GATE_TARGET_INDEX)
            return true;
    }
    return false;
}

static bool tow_gate_projection_consistent(
    entity_ref_t source,
    uint16_t current_generation)
{
    const tow_link_t *link = tow_gate_current_link();
    bool current_relation =
        link &&
        entity_ref_equal(link->source, source) &&
        link->target.generation == current_generation;
    bool should_project =
        current_relation &&
        g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX].active;
    int player_slot = source.index - WORLD_PLAYER_SHIP_BASE;
    const tractor_binding_t *world_binding =
        &g.world.cargo_pods[TOW_GATE_TARGET_INDEX].tractor;
    const tractor_binding_t *prev_binding =
        &g.cargo_pod_interp.prev[TOW_GATE_TARGET_INDEX].tractor;
    const tractor_binding_t *curr_binding =
        &g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX].tractor;
    const ship_t *ship =
        world_ship_resolve_const(&g.world, source);
    bool source_projection = tow_gate_source_projects_target(ship);
    bool target_projection =
        world_binding->kind != TRACTOR_SOURCE_NONE ||
        prev_binding->kind != TRACTOR_SOURCE_NONE ||
        curr_binding->kind != TRACTOR_SOURCE_NONE;
    if (!target_projection && !source_projection)
        return true;
    return should_project &&
           tow_gate_binding_matches(
               world_binding, player_slot, source.generation) &&
           tow_gate_binding_matches(
               prev_binding, player_slot, source.generation) &&
           tow_gate_binding_matches(
               curr_binding, player_slot, source.generation) &&
           source_projection;
}

static void tow_gate_observe_lifecycle(
    tow_gate_profile_result_t *result,
    entity_ref_t source,
    uint16_t current_generation)
{
    const tow_link_t *link = tow_gate_current_link();
    bool relation =
        link &&
        entity_ref_equal(link->source, source) &&
        link->target.generation == current_generation;
    bool active =
        g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX].active;
    bool projection =
        tow_gate_source_projects_target(
            world_ship_resolve_const(&g.world, source));
    uint32_t revision = g.tow_snapshot_revision;

    if (revision == 2u && active && current_generation == 1u &&
        relation && projection) {
        result->lifecycle_mask |= TOW_GATE_ATTACH;
    }
    if (revision == 3u && active && current_generation == 1u &&
        !relation && !projection) {
        result->lifecycle_mask |= TOW_GATE_RELEASE;
    }
    if (revision == 4u && active && current_generation == 1u &&
        relation && projection) {
        result->lifecycle_mask |= TOW_GATE_REATTACH;
    }
    if (revision == 5u && !active && current_generation == 1u &&
        !relation && !projection) {
        result->lifecycle_mask |= TOW_GATE_RETIRE;
    }
    if (revision == 5u && active && current_generation == 2u &&
        !relation && !projection) {
        result->lifecycle_mask |= TOW_GATE_REPLACEMENT_CLEAR;
    }
    if (revision == 6u && active && current_generation == 2u &&
        relation && projection) {
        result->lifecycle_mask |= TOW_GATE_REPLACEMENT_ATTACH;
    }
    if (revision == 6u && !active && current_generation == 2u &&
        relation && !projection) {
        result->lifecycle_mask |= TOW_GATE_RELEVANCE_EXIT;
    }
    if ((result->lifecycle_mask & TOW_GATE_RELEVANCE_EXIT) != 0u &&
        revision == 6u && active && current_generation == 2u &&
        relation && projection) {
        result->lifecycle_mask |= TOW_GATE_RELEVANCE_REENTRY;
    }
    if (revision == 7u && active && current_generation == 2u &&
        !relation && !projection) {
        result->lifecycle_mask |= TOW_GATE_FINAL_RELEASE;
    }
}

static void tow_gate_apply_target(
    const tow_gate_payload_t *payload,
    tow_presentation_diagnostics_t *presentation,
    uint16_t previous_generation)
{
    if (!payload->target_active) {
        uint8_t index = TOW_GATE_TARGET_INDEX;
        apply_remote_cargo_pod_remove(&index, 1);
        return;
    }

    vec2 authoritative_pos = v2(
        payload->target.pos_x, payload->target.pos_y);
    vec2 authoritative_vel = v2(
        payload->target.vel_x, payload->target.vel_y);
    vec2 presented_pos = authoritative_pos;
    vec2 presented_vel = authoritative_vel;
    if (previous_generation == payload->target_generation) {
        (void)net_remote_cargo_pod_presentation(
            TOW_GATE_TARGET_INDEX,
            &presented_pos, &presented_vel);
    }
    tow_presentation_diagnostics_snapshot(
        presentation,
        presented_pos, presented_vel,
        authoritative_pos, authoritative_vel);
    apply_remote_cargo_pods(&payload->target, 1);
}

static void tow_gate_apply_relation(
    const tow_gate_payload_t *payload,
    entity_ref_t source)
{
    if (!payload->relation_active) {
        apply_remote_tow_links(
            NULL, 0, payload->relation_revision,
            payload->send_ms / TOW_GATE_PAYLOAD_INTERVAL_MS + 1u);
        return;
    }
    tow_link_t link = {
        .active = true,
        .source = source,
        .target = {
            .kind = ENTITY_KIND_CARGO_POD,
            .index = TOW_GATE_TARGET_INDEX,
            .part = -1,
            .generation = payload->target_generation,
        },
        .profile = TOW_PROFILE_SHIP_POD,
        .slot = 0,
        .state = TOW_LINK_HELD,
        .attached_tick =
            payload->send_ms / TOW_GATE_PAYLOAD_INTERVAL_MS + 1u,
        .revision = payload->relation_revision,
    };
    apply_remote_tow_links(
        &link, 1, payload->relation_revision, link.attached_tick);
}

static bool tow_gate_run_profile(
    tow_gate_profile_result_t *result,
    uint16_t latency_ms,
    const tow_gate_payload_t *payloads,
    const tow_adverse_packet_t *packets,
    int packet_count,
    entity_ref_t source)
{
    memset(result, 0, sizeof(*result));
    result->latency_ms = latency_ms;
    result->failure = "none";
    tow_presentation_diagnostics_reset(&result->presentation);

    tow_adverse_profile_t profile =
        tow_adverse_profile(latency_ms);
    tow_adverse_delivery_t deliveries[TOW_ADVERSE_MAX_DELIVERIES];
    int delivery_count = tow_adverse_schedule(
        &profile, packets, packet_count,
        deliveries, TOW_ADVERSE_MAX_DELIVERIES,
        &result->schedule);
    if (delivery_count <= 0) {
        result->failure = "schedule";
        return false;
    }

    memset(&g.cargo_pod_interp.prev[TOW_GATE_TARGET_INDEX], 0,
           sizeof(g.cargo_pod_interp.prev[TOW_GATE_TARGET_INDEX]));
    memset(&g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX], 0,
           sizeof(g.cargo_pod_interp.curr[TOW_GATE_TARGET_INDEX]));
    g.cargo_pod_interp.elapsed[TOW_GATE_TARGET_INDEX] = 0.0f;
    memset(g.world.tow_links, 0, sizeof(g.world.tow_links));
    g.tow_snapshot_received = false;
    g.tow_snapshot_revision = 0;
    g.tow_snapshot_server_tick = 0;
    apply_remote_tow_links(NULL, 0, 1u, 1u);

    int next_delivery = 0;
    uint16_t current_generation = 0;
    uint32_t last_delivery_ms =
        deliveries[delivery_count - 1].delivery_ms;
    uint32_t frame_count =
        (last_delivery_ms * 60u + 999u) / 1000u + 1u;
    const float frame_dt = 1.0f / 60.0f;

    for (uint32_t frame = 0; frame < frame_count; frame++) {
        uint32_t frame_ms = frame * 1000u / 60u;
        while (next_delivery < delivery_count &&
               deliveries[next_delivery].delivery_ms <= frame_ms) {
            const tow_adverse_delivery_t *delivery =
                &deliveries[next_delivery++];
            const tow_gate_payload_t *payload =
                &payloads[delivery->packet.payload_index];
            if (delivery->packet.channel ==
                TOW_ADVERSE_CHANNEL_TARGET) {
                tow_gate_apply_target(
                    payload, &result->presentation,
                    current_generation);
                current_generation = payload->target_generation;
            } else {
                if ((result->lifecycle_mask &
                     TOW_GATE_RELEVANCE_REENTRY) != 0u &&
                    payload->relation_revision == 6u) {
                    result->post_reentry_relation_packets++;
                    result->failure = "reentry_relation_blackout";
                    return false;
                }
                tow_gate_apply_relation(payload, source);
            }
            if (!tow_gate_projection_consistent(
                    source, current_generation)) {
                result->stale_projection_failures++;
                result->failure = "stale_projection";
                return false;
            }
            tow_gate_observe_lifecycle(
                result, source, current_generation);
        }

        net_advance_cargo_pod_interpolation(frame_dt);
        vec2 presented_pos = {0};
        vec2 presented_vel = {0};
        bool visible = net_remote_cargo_pod_presentation(
            TOW_GATE_TARGET_INDEX,
            &presented_pos, &presented_vel);
        tow_presentation_diagnostics_frame(
            &result->presentation,
            visible, presented_pos, frame_dt,
            TOW_PRESENTATION_TEST_PIXELS_PER_WORLD);
        if (!tow_gate_projection_consistent(
                source, current_generation)) {
            result->stale_projection_failures++;
            result->failure = "frame_projection";
            return false;
        }
        tow_gate_observe_lifecycle(
            result, source, current_generation);
    }

    if (next_delivery != delivery_count) {
        result->failure = "delivery_flush";
        return false;
    }
    if (result->schedule.dropped_packets == 0 ||
        result->schedule.duplicated_packets == 0 ||
        result->schedule.reordered_packets == 0) {
        result->failure = "adverse_coverage";
        return false;
    }
    if (result->lifecycle_mask != TOW_GATE_REQUIRED_LIFECYCLE) {
        result->failure = "lifecycle";
        return false;
    }
    if (!tow_presentation_diagnostics_within_thresholds(
            &result->presentation)) {
        result->failure = "presentation_threshold";
        return false;
    }
    result->passed = true;
    return true;
}

static void tow_gate_format_report(bool passed, const char *failure)
{
    const tow_gate_profile_result_t *a = &tow_gate_results[0];
    const tow_gate_profile_result_t *b = &tow_gate_results[1];
    const tow_gate_profile_result_t *c = &tow_gate_results[2];
    (void)snprintf(
        tow_gate_report, sizeof(tow_gate_report),
        "{\"status\":%d,\"scope\":\"player_ship_to_cargo_pod\","
        "\"failure\":\"%s\","
        "\"thresholds\":{\"correction_world\":%.3f,"
        "\"velocity_discontinuity\":%.3f,"
        "\"snapshot_gap_sec\":%.3f,\"world_jerk\":%.1f,"
        "\"screen_jerk\":%.1f,\"pixels_per_world\":%.1f},"
        "\"profiles\":["
        "{\"latency_ms\":%u,\"pass\":%d,\"failure\":\"%s\","
        "\"delivered\":%u,\"dropped\":%u,\"duplicated\":%u,"
        "\"reordered\":%u,\"lifecycle\":%u,\"stale\":%u,"
        "\"post_reentry_relation\":%u,"
        "\"correction\":%.6f,\"velocity\":%.6f,\"gap\":%.6f,"
        "\"starvation\":%u,\"world_jerk\":%.3f,"
        "\"screen_jerk\":%.3f},"
        "{\"latency_ms\":%u,\"pass\":%d,\"failure\":\"%s\","
        "\"delivered\":%u,\"dropped\":%u,\"duplicated\":%u,"
        "\"reordered\":%u,\"lifecycle\":%u,\"stale\":%u,"
        "\"post_reentry_relation\":%u,"
        "\"correction\":%.6f,\"velocity\":%.6f,\"gap\":%.6f,"
        "\"starvation\":%u,\"world_jerk\":%.3f,"
        "\"screen_jerk\":%.3f},"
        "{\"latency_ms\":%u,\"pass\":%d,\"failure\":\"%s\","
        "\"delivered\":%u,\"dropped\":%u,\"duplicated\":%u,"
        "\"reordered\":%u,\"lifecycle\":%u,\"stale\":%u,"
        "\"post_reentry_relation\":%u,"
        "\"correction\":%.6f,\"velocity\":%.6f,\"gap\":%.6f,"
        "\"starvation\":%u,\"world_jerk\":%.3f,"
        "\"screen_jerk\":%.3f}]}",
        passed ? 1 : 0, failure,
        TOW_PRESENTATION_MAX_CORRECTION_WORLD,
        TOW_PRESENTATION_MAX_VELOCITY_DISCONTINUITY,
        TOW_PRESENTATION_MAX_SNAPSHOT_GAP_SEC,
        TOW_PRESENTATION_MAX_WORLD_JERK,
        TOW_PRESENTATION_MAX_SCREEN_JERK,
        TOW_PRESENTATION_TEST_PIXELS_PER_WORLD,
        a->latency_ms, a->passed ? 1 : 0, a->failure,
        a->schedule.delivered_packets, a->schedule.dropped_packets,
        a->schedule.duplicated_packets, a->schedule.reordered_packets,
        a->lifecycle_mask, a->stale_projection_failures,
        a->post_reentry_relation_packets,
        a->presentation.max_correction_world,
        a->presentation.max_velocity_discontinuity,
        a->presentation.max_snapshot_gap_sec,
        a->presentation.starvation_events,
        a->presentation.max_world_jerk,
        a->presentation.max_screen_jerk,
        b->latency_ms, b->passed ? 1 : 0, b->failure,
        b->schedule.delivered_packets, b->schedule.dropped_packets,
        b->schedule.duplicated_packets, b->schedule.reordered_packets,
        b->lifecycle_mask, b->stale_projection_failures,
        b->post_reentry_relation_packets,
        b->presentation.max_correction_world,
        b->presentation.max_velocity_discontinuity,
        b->presentation.max_snapshot_gap_sec,
        b->presentation.starvation_events,
        b->presentation.max_world_jerk,
        b->presentation.max_screen_jerk,
        c->latency_ms, c->passed ? 1 : 0, c->failure,
        c->schedule.delivered_packets, c->schedule.dropped_packets,
        c->schedule.duplicated_packets, c->schedule.reordered_packets,
        c->lifecycle_mask, c->stale_projection_failures,
        c->post_reentry_relation_packets,
        c->presentation.max_correction_world,
        c->presentation.max_velocity_discontinuity,
        c->presentation.max_snapshot_gap_sec,
        c->presentation.starvation_events,
        c->presentation.max_world_jerk,
        c->presentation.max_screen_jerk);
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_adverse_towable_gate(void)
{
    static const uint16_t latencies[3] = {50, 125, 250};
    memset(tow_gate_results, 0, sizeof(tow_gate_results));
    tow_gate_report[0] = '\0';
    for (int i = 0; i < 3; i++) {
        tow_gate_results[i].latency_ms = latencies[i];
        tow_gate_results[i].failure = "not_run";
    }

    if (g.local_player_slot < 0 ||
        g.local_player_slot >= MAX_PLAYERS ||
        !g.world.players[g.local_player_slot].connected) {
        tow_gate_format_report(false, "local_player");
        return 0;
    }
    const NetProtocolInfo *protocol = net_protocol_info();
    if (!protocol ||
        (protocol->capabilities &
         SIGNAL_PROTOCOL_CAP_CARGO_POD_REMOVE) == 0u) {
        tow_gate_format_report(false, "cargo_remove_capability");
        return 0;
    }
    entity_ref_t source =
        g.world.players[g.local_player_slot].ship_ref;
    if (!world_ship_resolve_const(&g.world, source)) {
        tow_gate_format_report(false, "source_ship");
        return 0;
    }

    tow_gate_restore_t *saved =
        (tow_gate_restore_t *)malloc(sizeof(*saved));
    if (!saved) {
        tow_gate_format_report(false, "allocation");
        return 0;
    }
    tow_gate_save(saved);

    tow_gate_payload_t payloads[TOW_GATE_PAYLOAD_COUNT];
    tow_adverse_packet_t packets[TOW_GATE_PACKET_COUNT];
    int packet_count = tow_gate_build_payloads(payloads, packets);

    bool passed = true;
    const char *failure = "none";
    for (int i = 0; i < 3; i++) {
        if (!tow_gate_run_profile(
                &tow_gate_results[i], latencies[i],
                payloads, packets, packet_count, source)) {
            passed = false;
            failure = tow_gate_results[i].failure;
            break;
        }
    }

    tow_gate_restore(saved);
    free(saved);
    tow_gate_format_report(passed, failure);
    return passed ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
const char *signal_smoke_adverse_towable_report(void)
{
    return tow_gate_report;
}
