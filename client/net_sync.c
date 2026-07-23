/*
 * net_sync.c -- Multiplayer network state synchronization for the
 * Signal Space Miner client.
 */
#include "net_sync.h"
#include "input.h"   /* set_notice() */
#include "manifest.h"
#include "onboarding.h"
#include "episode.h"
#include "contract_objective.h"
#include "net_input_lead.h"
#include "net_clock.h"

#define STATION_RING_CORRECTION_SEC 0.35f
#define NET_MOTION_TELEMETRY_WINDOW_SEC 5.0f
#define LOCAL_PLAYER_RENDER_OFFSET_MAX 140.0f
#define LOCAL_PLAYER_RENDER_OFFSET_LATENCY_MAX 260.0f
#define LOCAL_PLAYER_RENDER_SNAP_DIST 200.0f
#define LOCAL_PLAYER_RENDER_SNAP_LATENCY_DIST 360.0f
#define LOCAL_PLAYER_STALE_ACK_DEFER_DIST 200.0f
#define NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC 0.075f
#define NET_REPLAY_LATENCY_BLEND_MAX_SEC 0.45f
#define ASTEROID_RENDER_CORRECTION_SEC 0.18f
/* Dedicated-server asteroid heartbeats range up to 40 seconds for quiet,
 * distant rocks. Prediction must span that interval instead of freezing
 * after a fraction of a second. */
#define ASTEROID_RENDER_PREDICT_MAX_SEC 60.0f
#define ASTEROID_AMBIENT_DRAG 0.42f
#define ASTEROID_RENDER_CORRECTION_CUTOFF_SEC \
    (ASTEROID_RENDER_CORRECTION_SEC * 4.0f)
#define NPC_RENDER_CORRECTION_SEC 0.18f
#define NPC_RENDER_EXTRAPOLATE_MAX_SEC 2.20f
#define REMOTE_PLAYER_RENDER_CORRECTION_SEC 0.18f
#define REMOTE_PLAYER_RENDER_EXTRAPOLATE_MAX_SEC 2.25f
#define SCAFFOLD_RENDER_CORRECTION_SEC 0.18f
#define SCAFFOLD_RENDER_EXTRAPOLATE_MAX_SEC 0.60f
#define CARGO_POD_RENDER_CORRECTION_SEC 0.18f
#define CARGO_POD_RENDER_EXTRAPOLATE_MAX_SEC 2.20f
#define CARGO_POD_RENDER_CORRECTION_CUTOFF_SEC \
    (CARGO_POD_RENDER_CORRECTION_SEC * 4.0f)
#define REMOTE_PENDING_RECEIPT_CAP 64
#define NET_INPUT_JITTER_BUFFER_TICKS 1u
/* Replay is keyed to server-anchored sim ticks. Movement packets carry the
 * client-predicted target tick, and the server only applies them during the
 * matching world_sim_step(), so snapshots and prediction frames share one
 * integer clock instead of racing packet-arrival time. */
#define NET_REPLAY_ENABLED 1

static float station_ring_correction[MAX_STATIONS][MAX_ARMS];
static bool station_ring_have_snapshot[MAX_STATIONS];
static cargo_receipt_chain_t remote_pending_receipts[REMOTE_PENDING_RECEIPT_CAP];
static uint8_t remote_pending_receipt_pub[REMOTE_PENDING_RECEIPT_CAP][32];
static uint8_t remote_pending_receipt_count;

static void net_replay_clear_frames(void);

bool net_local_prediction_enabled(void) {
    if (!g.net_authority_enabled) return true;
    return g.net_input_tick_protocol;
}

static bool net_replay_enabled(void) {
    return NET_REPLAY_ENABLED && g.net_input_tick_protocol;
}

static float nearest_angle_delta(float from, float to) {
    float delta = to - from;
    while (delta >  PI_F) delta -= TWO_PI_F;
    while (delta < -PI_F) delta += TWO_PI_F;
    return delta;
}

void reset_station_ring_smoothing(void) {
    memset(station_ring_correction, 0, sizeof(station_ring_correction));
    memset(station_ring_have_snapshot, 0, sizeof(station_ring_have_snapshot));
}

void step_remote_station_rings(float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        for (int a = 0; a < MAX_ARMS; a++) {
            float correction = station_ring_correction[s][a];
            float correction_step = 0.0f;
            if (fabsf(correction) > 0.00001f) {
                float k = dt / STATION_RING_CORRECTION_SEC;
                if (k > 1.0f) k = 1.0f;
                correction_step = correction * k;
                station_ring_correction[s][a] -= correction_step;
            } else {
                station_ring_correction[s][a] = 0.0f;
            }
            st->arm_rotation[a] += st->arm_omega[a] * dt + correction_step;
        }
    }
}

static bool replay_tick_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static bool net_input_seq_after(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

float net_prediction_control_rtt_sec(void) {
    return net_latency_control_rtt_sec(&g.net_ping_latency,
                                       &g.net_ack_latency,
                                       g.net_time,
                                       NET_LATENCY_STALE_SEC,
                                       g.net_last_ping_rtt,
                                       g.net_last_ack_rtt);
}

static uint32_t net_one_way_latency_ticks(void) {
    float rtt = net_prediction_control_rtt_sec();
    if (rtt <= 0.0f) return 0;
    uint32_t ticks = (uint32_t)lroundf((rtt * 0.5f) / SIM_DT);
    if (ticks > NET_INPUT_APPLY_FUTURE_MAX_TICKS)
        ticks = NET_INPUT_APPLY_FUTURE_MAX_TICKS;
    return ticks;
}

void net_observe_server_tick(uint32_t server_tick) {
    if (server_tick == 0) return;
    g.net_last_server_tick = server_tick;
    g.net_last_server_tick_time = g.net_time;
}

uint32_t net_estimated_server_tick_now(uint32_t server_tick) {
    if (server_tick == 0) return 0;

    uint32_t tick = server_tick + net_one_way_latency_ticks();
    if (g.net_last_ping_rtt > 0.0f || g.net_last_ack_rtt > 0.0f)
        tick += NET_INPUT_JITTER_BUFFER_TICKS;

    if (g.net_last_server_tick == server_tick &&
        g.net_last_server_tick_time > 0.0f &&
        g.net_time >= g.net_last_server_tick_time) {
        uint32_t elapsed_ticks =
            (uint32_t)floorf((g.net_time - g.net_last_server_tick_time) /
                             SIM_DT);
        tick += elapsed_ticks;
    }
    return tick;
}

static bool net_prediction_tick_skew_for_sample(uint32_t server_tick,
                                                int32_t *skew_out) {
    if (server_tick == 0 || !g.net_prediction_tick_valid) return false;
    uint32_t estimated_tick = net_estimated_server_tick_now(server_tick);
    if (estimated_tick == 0) estimated_tick = server_tick;
    int32_t skew = (int32_t)(g.net_prediction_tick - estimated_tick);
    if (skew_out) *skew_out = skew;
    return true;
}

static void net_record_prediction_tick_skew(uint32_t server_tick) {
    int32_t skew = 0;
    if (!net_prediction_tick_skew_for_sample(server_tick, &skew)) return;
    int32_t abs_skew = skew < 0 ? -skew : skew;
    g.net_motion.tick_skew = skew;
    if (abs_skew > g.net_motion.max_tick_skew_abs)
        g.net_motion.max_tick_skew_abs = abs_skew;
}

void net_anchor_prediction_tick(uint32_t server_tick, bool clear_replay) {
    if (server_tick == 0) return;
    net_observe_server_tick(server_tick);
    /* Replay frames must be contiguous from the authoritative snapshot tick.
     * Input scheduling uses net_estimated_server_tick_now() separately. */
    g.net_prediction_tick = server_tick;
    g.net_prediction_tick_valid = true;
    if (clear_replay) net_replay_clear_frames();
}

static bool net_latest_input_unacked(const NetPlayerState *state) {
    if (!state || g.net_input_seq == 0) return false;
    uint16_t ack = state->input_seq_ack;
    if (ack == g.net_input_seq) return false;
    if (ack == 0) return true;
    return net_input_seq_after(g.net_input_seq, ack);
}

static input_intent_t replay_movement_intent(const input_intent_t *intent) {
    input_intent_t out = {0};
    if (!intent) return out;
    out.turn = intent->turn;
    out.thrust = intent->thrust;
    out.mine = intent->mine;
    out.mining_target_hint = intent->mining_target_hint;
    out.tractor_hold = intent->tractor_hold;
    out.boost = intent->boost;
    out.reverse_thrust = intent->reverse_thrust;
    return out;
}

static input_replay_frame_t *net_replay_frame_at(int offset) {
    int index = ((int)g.net_replay_start + offset) % NET_REPLAY_FRAME_CAP;
    return &g.net_replay[index];
}

static void net_replay_clear_frames(void) {
    g.net_replay_start = 0;
    g.net_replay_count = 0;
}

void net_replay_reset(void) {
    g.net_prediction_tick = 0;
    g.net_prediction_tick_valid = false;
    net_replay_clear_frames();
}

static void net_replay_append(const input_replay_frame_t *frame) {
    if (g.net_replay_count >= NET_REPLAY_FRAME_CAP) {
        g.net_replay_start =
            (uint16_t)((g.net_replay_start + 1u) % NET_REPLAY_FRAME_CAP);
        g.net_replay_count--;
    }
    int index = ((int)g.net_replay_start + (int)g.net_replay_count) %
                NET_REPLAY_FRAME_CAP;
    g.net_replay[index] = *frame;
    g.net_replay_count++;
}

void net_replay_record_prediction(const input_intent_t *intent, float dt) {
    if (!intent || dt <= 0.0f) return;
    if (!net_replay_enabled()) return;
    if (!g.net_authority_enabled || !net_is_connected())
        return;
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    if (!g.net_prediction_tick_valid) return;

    g.net_prediction_tick++;
    input_replay_frame_t frame = {
        .tick = g.net_prediction_tick,
        .input_seq = g.net_input_seq,
        .dt = dt,
        .intent = replay_movement_intent(intent),
    };
    net_replay_append(&frame);
}

static void net_replay_prune_through(uint32_t server_tick) {
    while (g.net_replay_count > 0) {
        input_replay_frame_t *frame = net_replay_frame_at(0);
        if (replay_tick_after(frame->tick, server_tick)) break;
        g.net_replay_start =
            (uint16_t)((g.net_replay_start + 1u) % NET_REPLAY_FRAME_CAP);
        g.net_replay_count--;
    }
    if (g.net_replay_count == 0) g.net_replay_start = 0;
}

static int net_replay_first_after(uint32_t server_tick) {
    for (int i = 0; i < (int)g.net_replay_count; i++) {
        if (replay_tick_after(net_replay_frame_at(i)->tick, server_tick))
            return i;
    }
    return -1;
}

static bool net_replay_missing_prefix(uint32_t server_tick, int first_after) {
    if (first_after < 0) return false;
    return net_replay_frame_at(first_after)->tick != server_tick + 1u;
}

float net_prediction_latency_blend(void) {
    if (g.net_last_ack_rtt <= NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC)
        return 0.0f;
    return clampf((g.net_last_ack_rtt - NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC) /
                  (NET_REPLAY_LATENCY_BLEND_MAX_SEC -
                   NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC),
                  0.0f, 1.0f);
}

static bool net_replay_has_frames_after(uint32_t server_tick) {
    if (!g.net_prediction_tick_valid) return false;
    if (!replay_tick_after(g.net_prediction_tick, server_tick)) return false;
    return net_replay_first_after(server_tick) >= 0;
}

static bool net_replay_has_turn_after(uint32_t server_tick) {
    int first_after = net_replay_first_after(server_tick);
    if (first_after < 0) return false;
    for (int i = first_after; i < (int)g.net_replay_count; i++) {
        const input_replay_frame_t *frame = net_replay_frame_at(i);
        if (fabsf(frame->intent.turn) > 0.01f) return true;
    }
    return false;
}

static bool replay_frame_has_motion(const input_replay_frame_t *frame) {
    if (!frame) return false;
    const input_intent_t *intent = &frame->intent;
    return fabsf(intent->turn) > 0.01f ||
           fabsf(intent->thrust) > 0.01f ||
           intent->boost ||
           intent->reverse_thrust;
}

static bool net_replay_has_motion_after(uint32_t server_tick) {
    int first_after = net_replay_first_after(server_tick);
    if (first_after < 0) return false;
    for (int i = first_after; i < (int)g.net_replay_count; i++) {
        if (replay_frame_has_motion(net_replay_frame_at(i))) return true;
    }
    return false;
}

static bool net_local_turn_prediction_active(uint32_t server_tick) {
    return fabsf(LOCAL_PLAYER.input.turn) > 0.01f ||
           net_replay_has_turn_after(server_tick);
}

static bool net_local_motion_prediction_active(uint32_t server_tick) {
    return fabsf(LOCAL_PLAYER.input.turn) > 0.01f ||
           fabsf(LOCAL_PLAYER.input.thrust) > 0.01f ||
           LOCAL_PLAYER.input.boost ||
           LOCAL_PLAYER.input.reverse_thrust ||
           net_replay_has_motion_after(server_tick);
}

static bool should_defer_stale_unacked_motion(const NetPlayerState *state,
                                              bool has_unacked_input,
                                              float dist_sq) {
    if (!state || !net_local_prediction_enabled() || !has_unacked_input)
        return false;
    float defer_dist = LOCAL_PLAYER_STALE_ACK_DEFER_DIST;
    if (dist_sq > defer_dist * defer_dist) return false;
    if ((state->flags & 4) != 0) return true;
    if (!net_replay_enabled()) return true;
    return replay_tick_after(g.net_prediction_tick, state->server_tick) &&
           !net_replay_has_frames_after(state->server_tick);
}

static bool should_defer_active_prediction_motion(const NetPlayerState *state,
                                                  float dist_sq) {
    if (!state || !net_local_prediction_enabled() || !net_replay_enabled())
        return false;
    if ((state->flags & 4) != 0) return false;
    float defer_dist = LOCAL_PLAYER_STALE_ACK_DEFER_DIST;
    if (dist_sq > defer_dist * defer_dist) return false;
    if (!net_replay_has_frames_after(state->server_tick)) return false;
    return net_local_motion_prediction_active(state->server_tick);
}

static void apply_authoritative_local_motion(const NetPlayerState *state,
                                             server_player_t *sp) {
    sp->ship->pos.x = state->x;
    sp->ship->pos.y = state->y;
    sp->ship->vel.x = state->vx;
    sp->ship->vel.y = state->vy;
    sp->ship->angle = state->angle;
    if ((state->flags & 4) == 0)
        sp->docked = false;
}

static void sync_local_tow_state_from_authority(const NetPlayerState *state,
                                                server_player_t *sp) {
    if (!state || !sp) return;
    sp->ship->tractor_level = (int)state->tractor_level;

    int tow_cap = (int)(sizeof(sp->ship->towed_fragments) /
                        sizeof(sp->ship->towed_fragments[0]));
    int tow_count = state->towed_count;
    if (tow_count > tow_cap) tow_count = tow_cap;
    sp->ship->towed_count = (uint8_t)tow_count;
    for (int t = 0; t < tow_cap; t++) {
        uint16_t wire = (t < tow_count) ? state->towed_fragments[t] : 0xFFFFu;
        sp->ship->towed_fragments[t] =
            (wire != 0xFFFFu && wire < MAX_ASTEROIDS) ? (int16_t)wire : -1;
    }
}

static void apply_local_player_remote_flags(const NetPlayerState *state,
                                            server_player_t *sp) {
    sp->beam_active      = (state->flags & 2) != 0;
    sp->beam_ineffective = (state->flags & 32) != 0;
    sp->beam_hit         = (state->flags & 64) != 0;
    sp->scan_active      = (state->flags & 8) != 0;
    sp->beam_start = v2(state->beam_start_x, state->beam_start_y);
    sp->beam_end   = v2(state->beam_end_x,   state->beam_end_y);
    sp->ship->tractor_active = (state->flags & 16) != 0;
    g.server_thrusting = (state->flags & 1) != 0;
}

/* Dock proximity is not carried by NetPlayerState. Re-derive it from the
 * authoritative position instead of clearing it whenever an undocked state
 * arrives. Clearing the flag left clients unable to emit an E-key dock action
 * until local movement prediction happened to run another sim step (and on
 * newly launched/legacy connections that step may not run at all). */
static void sync_local_undocked_dock_proximity(server_player_t *sp) {
    if (!sp) return;
    sp->in_dock_range = false;
    sp->nearby_station = -1;
    if (sp->docked) return;

    float approach_sq = DOCK_APPROACH_RANGE * DOCK_APPROACH_RANGE;
    float best_d = INFINITY;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &g.world.stations[i];
        if (!station_exists(st) || !station_has_module(st, MODULE_DOCK))
            continue;
        float d_sq = v2_dist_sq(sp->ship->pos, st->pos);
        if (d_sq <= approach_sq && d_sq < best_d) {
            best_d = d_sq;
            sp->nearby_station = i;
        }
    }
    sp->in_dock_range = sp->nearby_station >= 0;
}

static void sync_local_dock_state_from_authority(const NetPlayerState *state,
                                                 server_player_t *sp) {
    bool state_docked = (state->flags & 4) != 0;
    sp->docked = state_docked;
    sp->docking_approach = false;
    if (state_docked) {
        int station = nearest_station_index(sp->ship->pos);
        if (station >= 0 && station < MAX_STATIONS &&
            station_exists(&g.world.stations[station])) {
            sp->current_station = station;
            sp->nearby_station = station;
        }
        sp->in_dock_range = true;
    } else {
        sync_local_undocked_dock_proximity(sp);
    }
}

static void frame_camera_on_authoritative_baseline(const server_player_t *sp) {
    if (!sp) return;
    g.camera_pos = sp->ship->pos;
    g.camera_initialized = true;
    g.camera_station_index = -1;
    g.camera_station_side = 0;
    g.camera_station_v_side = 0;
    g.camera_drift_timer = 0.0f;
    g.local_player_render_offset = v2(0.0f, 0.0f);
}

static void accept_initial_local_player_state(const NetPlayerState *state,
                                              server_player_t *sp) {
    bool state_docked = (state->flags & 4) != 0;
    apply_authoritative_local_motion(state, sp);
    sync_local_dock_state_from_authority(state, sp);
    apply_local_player_remote_flags(state, sp);

    net_replay_reset();
    net_anchor_prediction_tick(state->server_tick, false);
    g.net_local_state_ready = true;
    g.was_docked = state_docked;
    g.action_predict_timer = 0.0f;
    frame_camera_on_authoritative_baseline(sp);
}

static void accept_docked_local_player_state(const NetPlayerState *state,
                                             server_player_t *sp) {
    vec2 before_pos = sp->ship->pos;
    apply_authoritative_local_motion(state, sp);
    sync_local_dock_state_from_authority(state, sp);
    apply_local_player_remote_flags(state, sp);
    net_replay_reset();
    net_anchor_prediction_tick(state->server_tick, false);
    g.was_docked = true;
    if (!g.camera_initialized ||
        v2_dist_sq(before_pos, sp->ship->pos) > 20.0f * 20.0f) {
        frame_camera_on_authoritative_baseline(sp);
    } else {
        g.local_player_render_offset = v2(0.0f, 0.0f);
    }
}

static void accept_authoritative_local_launch_state(const NetPlayerState *state,
                                                    server_player_t *sp) {
    apply_authoritative_local_motion(state, sp);
    sync_local_dock_state_from_authority(state, sp);
    apply_local_player_remote_flags(state, sp);
    net_replay_reset();
    net_anchor_prediction_tick(state->server_tick, false);
    g.action_predict_timer = 0.0f;
    frame_camera_on_authoritative_baseline(sp);
}

static bool net_replay_reconcile_local_player(const NetPlayerState *state,
                                              server_player_t *sp,
                                              int *out_replayed) {
    *out_replayed = 0;
    if (!net_replay_enabled()) return false;
    uint32_t server_tick = state->server_tick;
    if (server_tick == 0 && !g.net_prediction_tick_valid) return false;

    if (!g.net_prediction_tick_valid) {
        net_anchor_prediction_tick(server_tick, true);
    }

    if ((state->flags & 4) != 0) {
        apply_authoritative_local_motion(state, sp);
        net_anchor_prediction_tick(server_tick, true);
        return true;
    }

    int first_after = net_replay_first_after(server_tick);
    if (first_after < 0 && replay_tick_after(g.net_prediction_tick, server_tick))
        return false;
    if (net_replay_missing_prefix(server_tick, first_after)) return false;

    sim_events_t saved_events = g.world.events;
    apply_authoritative_local_motion(state, sp);
    uint32_t last_tick = server_tick;
    int replay_count = (int)g.net_replay_count;
    for (int i = first_after; i >= 0 && i < replay_count; i++) {
        input_replay_frame_t *frame = net_replay_frame_at(i);
        sp->input = frame->intent;
        world_sim_step_player_only(&g.world, g.local_player_slot, frame->dt);
        last_tick = frame->tick;
        (*out_replayed)++;
    }
    net_adopt_local_tow_prediction(0.0f);
    g.world.events = saved_events;

    g.net_prediction_tick = last_tick;
    g.net_prediction_tick_valid = true;
    net_replay_prune_through(server_tick);
    return true;
}

static void client_reset_player_slot(int player_slot) {
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    world_player_ship_slot_release(&g.world, player_slot);
    world_player_runtime_slot_reset(&g.world, player_slot);
    (void)world_player_ship_slot_activate(&g.world, player_slot);
}

static bool client_move_player_slot(int dst_slot, int src_slot) {
    if (dst_slot < 0 || dst_slot >= MAX_PLAYERS ||
        src_slot < 0 || src_slot >= MAX_PLAYERS || dst_slot == src_slot) {
        return false;
    }
    server_player_t controller = g.world.players[src_slot];
    if (!world_player_transfer_ship_state(&g.world, dst_slot, src_slot))
        return false;

    server_player_t *dst = &g.world.players[dst_slot];
    server_connection_t *connection = dst->connection;
    server_replication_t *replication = dst->replication;
    entity_ref_t ship_ref = dst->ship_ref;
    uint32_t ship_asset_id = dst->ship_asset_id;
    *dst = controller;
    dst->connection = connection;
    dst->replication = replication;
    dst->ship_ref = ship_ref;
    dst->ship_asset_id = ship_asset_id;
    world_rebind_ship_controllers(&g.world);

    world_player_runtime_slot_reset(&g.world, src_slot);
    (void)world_player_ship_slot_activate(&g.world, src_slot);
    return true;
}

void on_player_join(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    g.world.players[player_id].connected = true;
    g.world.players[player_id].id = player_id;
    /* Don't show join notice here — callsign hasn't arrived yet.
     * We detect new players in apply_remote_player_state instead. */
    (void)0;
}

void on_player_leave(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    const NetPlayerState *ps = &net_get_players()[player_id];
    if ((int)player_id != g.local_player_slot) {
        if (ps->callsign[0])
            set_notice("%s left.", ps->callsign);
        else
            set_notice("Pilot left.");
        client_reset_player_slot(player_id);
    } else {
        g.world.players[player_id].connected = false;
    }
    memset(&g.player_interp.prev[player_id], 0,
           sizeof(g.player_interp.prev[player_id]));
    memset(&g.player_interp.curr[player_id], 0,
           sizeof(g.player_interp.curr[player_id]));
    g.scanned_players[player_id] = false;
}

static void net_collect_towed_asteroids(bool towed[MAX_ASTEROIDS]) {
    memset(towed, 0, sizeof(bool) * MAX_ASTEROIDS);

    for (int p = 0; p < MAX_PLAYERS; p++) {
        const server_player_t *sp = &g.world.players[p];
        if (!sp->connected) continue;
        int tow_count = sp->ship->towed_count;
        int tow_cap = (int)(sizeof(sp->ship->towed_fragments) /
                            sizeof(sp->ship->towed_fragments[0]));
        if (tow_count > tow_cap) tow_count = tow_cap;
        for (int t = 0; t < tow_count; t++) {
            int idx = sp->ship->towed_fragments[t];
            if (idx >= 0 && idx < MAX_ASTEROIDS) towed[idx] = true;
        }
    }

    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        const npc_ship_t *npc = &g.world.npc_ships[n];
        if (!npc->active) continue;
        int idx = npc_towed_fragment_index(npc);
        if (idx >= 0 && idx < MAX_ASTEROIDS) towed[idx] = true;
    }

    /* Furnace/hopper tractors are published as interactions instead of a
     * single asteroid ownership binding because two station modules pull the
     * same fragment. Treat those targets as tow-driven for render prediction:
     * constant-velocity extrapolation over the 10 Hz motion stream is much
     * closer than applying ambient drag and snapping to every acceleration
     * update. */
    for (int i = 0; i < g.world.interactions.count; i++) {
        const sim_interaction_t *interaction =
            &g.world.interactions.items[i];
        if (interaction->type != SIM_INTERACTION_TRACTOR_BEAM ||
            interaction->visual !=
                SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR ||
            interaction->target.type != SIM_INTERACTION_ENTITY_ASTEROID) {
            continue;
        }
        int idx = interaction->target.index;
        if (idx >= 0 && idx < MAX_ASTEROIDS) towed[idx] = true;
    }
}

static void asteroid_predict_motion(const asteroid_t *base, float elapsed,
                                    bool towed, vec2 *out_pos, vec2 *out_vel) {
    vec2 pos = base->pos;
    vec2 vel = base->vel;

    if (towed) {
        pos = v2_add(pos, v2_scale(vel, elapsed));
    } else if (elapsed > 0.0f) {
        /* Match sim_step_asteroid_dynamics(): integrate position, then apply
         * the rational drag factor once per fixed simulation tick. Cache the
         * derived constants and use one exponential for the smooth
         * fractional-tick continuation; fixed-tick positions remain exact. */
        static float drag_decay_rate;
        static float drag_displacement_limit;
        if (drag_decay_rate <= 0.0f) {
            float drag_step =
                1.0f / (1.0f + ASTEROID_AMBIENT_DRAG * SIM_DT);
            drag_decay_rate = -logf(drag_step) / SIM_DT;
            drag_displacement_limit = SIM_DT / (1.0f - drag_step);
        }
        float retained = expf(-drag_decay_rate * elapsed);
        float displacement_scale =
            drag_displacement_limit * (1.0f - retained);
        pos = v2_add(pos, v2_scale(vel, displacement_scale));
        vel = v2_scale(vel, retained);
    }

    *out_pos = pos;
    *out_vel = vel;
}

static asteroid_t asteroid_render_state_at(int slot, float elapsed,
                                           bool towed) {
    const asteroid_t *prev = &g.asteroid_interp.prev[slot];
    const asteroid_t *curr = &g.asteroid_interp.curr[slot];
    asteroid_t out = *curr;
    if (!curr->active) return out;

    elapsed = clampf(elapsed, 0.0f, ASTEROID_RENDER_PREDICT_MAX_SEC);
    asteroid_predict_motion(curr, elapsed, towed, &out.pos, &out.vel);
    out.age += elapsed;
    out.rotation = wrap_angle(curr->rotation + curr->spin * elapsed);

    if (prev->active) {
        /* Critically damp the visual-to-authoritative offset. Unlike a
         * position-only lerp, this is continuous in both position and
         * velocity when a correction arrives. */
        float omega = 4.0f / ASTEROID_RENDER_CORRECTION_SEC;
        float decay = expf(-omega * elapsed);
        vec2 pos_error = v2_sub(prev->pos, curr->pos);
        vec2 vel_error = v2_sub(prev->vel, curr->vel);
        vec2 c = v2_add(vel_error, v2_scale(pos_error, omega));
        vec2 offset = v2_scale(v2_add(pos_error, v2_scale(c, elapsed)), decay);
        vec2 offset_vel = v2_scale(
            v2_sub(vel_error, v2_scale(c, omega * elapsed)), decay);
        out.pos = v2_add(out.pos, offset);
        out.vel = v2_add(out.vel, offset_vel);

        float rotation_error = nearest_angle_delta(curr->rotation,
                                                   prev->rotation);
        float spin_error = prev->spin - curr->spin;
        float rotation_c = spin_error + omega * rotation_error;
        float rotation_offset =
            (rotation_error + rotation_c * elapsed) * decay;
        out.rotation = wrap_angle(out.rotation + rotation_offset);
        out.spin += (spin_error - omega * rotation_c * elapsed) * decay;
    }
    return out;
}

static client_npc_render_state_t npc_render_state_at(int slot, float elapsed) {
    const client_npc_render_state_t *prev = &g.npc_interp.prev[slot];
    const client_npc_render_state_t *curr = &g.npc_interp.curr[slot];
    client_npc_render_state_t out = *curr;
    if (!curr->active) return out;

    out.pos.x += curr->vel.x * elapsed;
    out.pos.y += curr->vel.y * elapsed;

    if (prev->active) {
        float blend = clampf(elapsed / NPC_RENDER_CORRECTION_SEC, 0.0f, 1.0f);
        out.pos.x = lerpf(prev->pos.x, out.pos.x, blend);
        out.pos.y = lerpf(prev->pos.y, out.pos.y, blend);
        out.angle = lerp_angle(prev->angle, out.angle, blend);
    }
    return out;
}

static scaffold_t scaffold_render_state_at(int slot, float elapsed) {
    const scaffold_t *prev = &g.scaffold_interp.prev[slot];
    const scaffold_t *curr = &g.scaffold_interp.curr[slot];
    scaffold_t out = *curr;
    if (!curr->active) return out;

    out.pos.x += curr->vel.x * elapsed;
    out.pos.y += curr->vel.y * elapsed;

    if (prev->active &&
        prev->state == curr->state &&
        prev->module_type == curr->module_type) {
        float blend = clampf(elapsed / SCAFFOLD_RENDER_CORRECTION_SEC, 0.0f, 1.0f);
        out.pos.x = lerpf(prev->pos.x, out.pos.x, blend);
        out.pos.y = lerpf(prev->pos.y, out.pos.y, blend);
    }
    return out;
}

static cargo_pod_t cargo_pod_render_state_at(int slot, float elapsed) {
    const cargo_pod_t *prev = &g.cargo_pod_interp.prev[slot];
    const cargo_pod_t *curr = &g.cargo_pod_interp.curr[slot];
    cargo_pod_t out = *curr;
    if (!curr->active) return out;

    out.pos.x += curr->vel.x * elapsed;
    out.pos.y += curr->vel.y * elapsed;

    if (prev->active &&
        prev->kind == curr->kind &&
        prev->commodity == curr->commodity) {
        float blend = clampf(elapsed / CARGO_POD_RENDER_CORRECTION_SEC, 0.0f, 1.0f);
        out.pos.x = lerpf(prev->pos.x, out.pos.x, blend);
        out.pos.y = lerpf(prev->pos.y, out.pos.y, blend);
        out.rotation = lerp_angle(prev->rotation, curr->rotation, blend);
    }
    return out;
}

static void cargo_pod_interp_begin_update(int idx) {
    if (idx < 0 || idx >= MAX_CARGO_PODS) return;
    float elapsed = clampf(g.cargo_pod_interp.elapsed[idx], 0.0f,
                           CARGO_POD_RENDER_EXTRAPOLATE_MAX_SEC);
    g.cargo_pod_interp.prev[idx] =
        cargo_pod_render_state_at(idx, elapsed);
    g.cargo_pod_interp.elapsed[idx] = 0.0f;
}

/* Cargo identity deltas already carry the authoritative tractor player.
 * Project those bindings into the local ship's compatibility list so tow
 * prediction, render offsets, capacity/UI, and release affordances all read
 * the same relationship. A held pod is necessarily within the player's
 * relevance window, so the sparse identity cache is sufficient authority. */
static void sync_local_towed_pods_from_cargo_authority(void) {
    int player_idx = g.local_player_slot;
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return;
    server_player_t *sp = &g.world.players[player_idx];
    if (!sp->connected || !sp->ship) return;

    int cap = (int)(sizeof(sp->ship->towed_pods) /
                    sizeof(sp->ship->towed_pods[0]));
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS && count < cap; i++) {
        const cargo_pod_t *pod = &g.cargo_pod_interp.curr[i];
        if (!pod->active || cargo_pod_player_tractor(pod) != player_idx)
            continue;
        sp->ship->towed_pods[count++] = (int16_t)i;
    }
    for (int i = count; i < cap; i++) sp->ship->towed_pods[i] = -1;
    sp->ship->towed_pod_count = (uint8_t)count;
}

static bool net_local_player_towing_asteroid(int idx) {
    if (!g.net_authority_enabled || !net_local_prediction_enabled())
        return false;
    if (idx < 0 || idx >= MAX_ASTEROIDS) return false;
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS)
        return false;

    const server_player_t *sp = &g.world.players[g.local_player_slot];
    if (!sp->connected || sp->docked) return false;

    int tow_count = sp->ship->towed_count;
    int tow_cap = (int)(sizeof(sp->ship->towed_fragments) /
                        sizeof(sp->ship->towed_fragments[0]));
    if (tow_count > tow_cap) tow_count = tow_cap;
    for (int t = 0; t < tow_count; t++) {
        if (sp->ship->towed_fragments[t] == idx) return true;
    }
    return false;
}

static float net_prediction_future_elapsed(float t, float interval,
                                           float dt, float max_elapsed) {
    if (!isfinite(interval) || interval <= 0.0f) interval = 0.01f;
    float elapsed = (isfinite(t) && t > 0.0f) ? t * interval : 0.0f;
    if (isfinite(dt) && dt > 0.0f) elapsed += dt;
    return clampf(elapsed, 0.0f, max_elapsed);
}

static void net_adopt_local_asteroid_prediction_base(int idx, float elapsed,
                                                     const asteroid_t *base) {
    if (idx < 0 || idx >= MAX_ASTEROIDS) return;
    const asteroid_t *predicted = &g.world.asteroids[idx];
    if (!predicted->active) return;
    if (base && !base->active) return;

    asteroid_t visual = base ? *base : *predicted;
    visual.active = predicted->active;
    visual.pos = predicted->pos;
    visual.vel = predicted->vel;
    visual.rotation = predicted->rotation;
    visual.spin = predicted->spin;
    visual.age = predicted->age;

    asteroid_t backdated = visual;
    backdated.pos = v2_sub(visual.pos, v2_scale(visual.vel, elapsed));
    backdated.rotation =
        wrap_angle(visual.rotation - visual.spin * elapsed);
    backdated.age = fmaxf(0.0f, visual.age - elapsed);

    /* The backdated baseline reconstructs the already-predicted visual pose
     * at this slot's clock. It is not a server correction, so give it a zero
     * reconciliation offset. */
    g.asteroid_interp.prev[idx] = backdated;
    g.asteroid_interp.prev[idx].active = false;
    g.asteroid_interp.curr[idx] = backdated;
}

static void net_adopt_local_asteroid_prediction(int idx, float elapsed) {
    net_adopt_local_asteroid_prediction_base(idx, elapsed, NULL);
}

static void net_preserve_local_towed_asteroid_prediction(int idx) {
    if (!net_local_player_towing_asteroid(idx)) return;
    net_adopt_local_asteroid_prediction_base(
        idx, 0.0f, &g.asteroid_interp.curr[idx]);
}

static void net_adopt_local_cargo_pod_prediction(int idx, float elapsed) {
    if (idx < 0 || idx >= MAX_CARGO_PODS) return;
    const cargo_pod_t *predicted = &g.world.cargo_pods[idx];
    if (!predicted->active) return;

    /* The interpolation record owns authoritative identity and tow metadata.
     * Prediction owns only pose. Copying the whole rendered pod here allowed
     * a stale client projection to erase a tractor binding that had just
     * arrived in an identity delta. */
    cargo_pod_t backdated = g.cargo_pod_interp.curr[idx];
    backdated.active = predicted->active;
    backdated.pos = predicted->pos;
    backdated.vel = predicted->vel;
    backdated.rotation = predicted->rotation;
    backdated.pos = v2_sub(predicted->pos,
                           v2_scale(predicted->vel, elapsed));

    g.cargo_pod_interp.prev[idx] = backdated;
    g.cargo_pod_interp.prev[idx].active = false;
    g.cargo_pod_interp.curr[idx] = backdated;
    /* Leave the slot clock at its pre-step value. sim_step advances every
     * interpolation clock once after prediction; storing the future elapsed
     * value here made held pods advance by dt twice and snap back on each
     * authoritative correction. This mirrors the asteroid adoption path. */
}

static void net_adopt_local_scaffold_prediction(int idx, float elapsed) {
    if (idx < 0 || idx >= MAX_SCAFFOLDS) return;
    const scaffold_t *predicted = &g.world.scaffolds[idx];
    if (!predicted->active) return;

    scaffold_t backdated = *predicted;
    backdated.pos = v2_sub(predicted->pos,
                           v2_scale(predicted->vel, elapsed));

    g.scaffold_interp.prev[idx] = *predicted;
    g.scaffold_interp.curr[idx] = backdated;
}

void net_adopt_local_tow_prediction(float dt) {
    if (!g.net_authority_enabled || !net_local_prediction_enabled()) return;
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;

    const server_player_t *sp = &g.world.players[g.local_player_slot];
    if (!sp->connected || sp->docked) return;

    int tow_count = sp->ship->towed_count;
    int tow_cap = (int)(sizeof(sp->ship->towed_fragments) /
                        sizeof(sp->ship->towed_fragments[0]));
    if (tow_count > tow_cap) tow_count = tow_cap;
    for (int t = 0; t < tow_count; t++) {
        int idx = sp->ship->towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        float elapsed = g.asteroid_interp.elapsed[idx];
        if (isfinite(dt) && dt > 0.0f) elapsed += dt;
        elapsed = clampf(elapsed, 0.0f, ASTEROID_RENDER_PREDICT_MAX_SEC);
        net_adopt_local_asteroid_prediction(idx, elapsed);
    }

    int pod_count = sp->ship->towed_pod_count;
    int pod_cap = (int)(sizeof(sp->ship->towed_pods) /
                        sizeof(sp->ship->towed_pods[0]));
    if (pod_count > pod_cap) pod_count = pod_cap;
    for (int t = 0; t < pod_count; t++) {
        int idx = sp->ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        float elapsed = g.cargo_pod_interp.elapsed[idx];
        if (isfinite(dt) && dt > 0.0f) elapsed += dt;
        elapsed = clampf(elapsed, 0.0f,
                         CARGO_POD_RENDER_EXTRAPOLATE_MAX_SEC);
        net_adopt_local_cargo_pod_prediction(idx, elapsed);
    }

    float scaffold_elapsed = net_prediction_future_elapsed(
        g.scaffold_interp.t, g.scaffold_interp.interval, dt,
        SCAFFOLD_RENDER_EXTRAPOLATE_MAX_SEC);
    net_adopt_local_scaffold_prediction(sp->ship->towed_scaffold,
                                        scaffold_elapsed);
}

void net_advance_asteroid_interpolation(float dt) {
    if (!isfinite(dt) || dt <= 0.0f) return;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *curr = &g.asteroid_interp.curr[i];
        asteroid_t *prev = &g.asteroid_interp.prev[i];
        if (!curr->active) {
            if (prev->active) prev->active = false;
            if (g.asteroid_interp.elapsed[i] != 0.0f)
                g.asteroid_interp.elapsed[i] = 0.0f;
            continue;
        }
        float elapsed = g.asteroid_interp.elapsed[i];
        if (elapsed < ASTEROID_RENDER_PREDICT_MAX_SEC) {
            elapsed = fminf(elapsed + dt,
                            ASTEROID_RENDER_PREDICT_MAX_SEC);
            g.asteroid_interp.elapsed[i] = elapsed;
        }
        /* At four correction windows the critically damped residual is below
         * two parts per million. Retire it so settled rocks skip expf(). */
        if (prev->active &&
            elapsed >= ASTEROID_RENDER_CORRECTION_CUTOFF_SEC)
            prev->active = false;
    }
}

void net_advance_cargo_pod_interpolation(float dt) {
    if (!isfinite(dt) || dt <= 0.0f) return;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *curr = &g.cargo_pod_interp.curr[i];
        cargo_pod_t *prev = &g.cargo_pod_interp.prev[i];
        if (!curr->active) {
            g.cargo_pod_interp.elapsed[i] = 0.0f;
            prev->active = false;
            continue;
        }
        float elapsed = g.cargo_pod_interp.elapsed[i] + dt;
        g.cargo_pod_interp.elapsed[i] = clampf(
            elapsed, 0.0f, CARGO_POD_RENDER_EXTRAPOLATE_MAX_SEC);
        if (prev->active &&
            g.cargo_pod_interp.elapsed[i] >=
                CARGO_POD_RENDER_CORRECTION_CUTOFF_SEC) {
            prev->active = false;
        }
    }
}

void net_reset_local_input_stream(void) {
    g.pending_net_action = NET_ACTION_NONE;
    g.pending_net_buy_grade = MINING_GRADE_COUNT;
    g.pending_net_place_station = -1;
    g.pending_net_place_ring = -1;
    g.pending_net_place_slot = -1;
    g.action_predict_timer = 0.0f;

    g.net_input_timer = 0.0f;
    g.net_input_ack_timer = 0.0f;
    g.net_input_have_last = false;
    g.net_last_sent_flags = 0;
    g.net_last_sent_mining_target = 0xFFFFu;
    g.net_input_seq = 0;
    g.net_last_server_ack = 0;
    g.net_last_server_tick = 0;
    g.net_last_server_tick_time = 0.0f;
    g.net_input_tick_protocol = false;
    g.net_local_state_ready = false;
    g.net_last_ack_rtt = 0.0f;
    net_latency_stats_reset(&g.net_ack_latency);
    net_latency_gap_stats_reset(&g.net_ack_gap);
    g.net_missed_input_acks = 0;
    g.net_ack_recovery_packets = 0;
    g.net_ack_miss_windows_reported = 0;
    g.net_ack_recovery_tier = NET_LATENCY_ACK_RECOVERY_STEADY;
    g.net_max_ack_rtt_5s = 0.0f;
    g.net_ack_window_elapsed = 0.0f;
    g.net_input_packets_sent = 0;
    g.net_action_packets_sent = 0;
    g.net_action_resend_packets = 0;
    g.net_action_dropped = 0;
    g.net_next_action_id = 1;
    g.net_action_queue_start = 0;
    g.net_action_queue_count = 0;
    memset(g.net_action_queue, 0, sizeof(g.net_action_queue));
    memset(g.net_input_timing, 0, sizeof(g.net_input_timing));
    memset(&g.net_motion, 0, sizeof(g.net_motion));
    g.net_motion.input_lead_margin_ticks =
        NET_INPUT_LEAD_DEFAULT_MARGIN_TICKS;
    g.local_player_render_offset = v2(0.0f, 0.0f);
    net_replay_reset();
}

void reset_remote_dynamic_sync(void) {
    net_reset_local_input_stream();
    g.net_last_ping_raw_rtt = 0.0f;
    g.net_last_ping_rtt = 0.0f;
    g.net_last_ping_server_turnaround_ms = 0.0f;
    g.net_last_dedicated_ping_sample_time = 0.0f;
    g.net_last_ack_transport_sample_time = 0.0f;
    g.net_max_ping_rtt_5s = 0.0f;
    g.net_ping_samples = 0;
    net_latency_stats_reset(&g.net_ping_latency);
    g.net_missed_pongs = 0;
    g.net_ping_miss_windows_reported = 0;
    memset(g.scanned_players, 0, sizeof(g.scanned_players));

    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        world_npc_ship_slot_release(&g.world, i);
        memset(&g.world.npc_ships[i], 0, sizeof(g.world.npc_ships[i]));
    }
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    g.npc_interp.interval = 0.1f;

    memset(g.world.scaffolds, 0, sizeof(g.world.scaffolds));
    memset(&g.scaffold_interp, 0, sizeof(g.scaffold_interp));
    g.scaffold_interp.interval = 0.1f;

    memset(g.world.cargo_pods, 0, sizeof(g.world.cargo_pods));
    memset(&g.cargo_pod_interp, 0, sizeof(g.cargo_pod_interp));

    memset(&g.world.interactions, 0, sizeof(g.world.interactions));

    LOCAL_PLAYER.hover_asteroid = -1;
}

bool net_remote_player_scanned(int player_id) {
    if (player_id < 0 || player_id >= NET_MAX_PLAYERS) return false;
    return g.scanned_players[player_id];
}

void net_update_remote_player_scans(const NetPlayerState *players) {
    if (!g.net_authority_enabled || !players) return;
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;

    const server_player_t *local = &LOCAL_PLAYER;
    int local_id = (int)net_local_id();
    float tractor_range = ship_tractor_range(local->ship);
    float tractor_range_sq = tractor_range * tractor_range;

    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (i == local_id || i == g.local_player_slot) continue;
        if (!players[i].active) {
            g.scanned_players[i] = false;
            continue;
        }

        bool active_scan =
            local->scan_active &&
            local->scan_target_type == INSPECT_TARGET_PLAYER &&
            local->scan_target_index == i;
        if (active_scan) {
            g.scanned_players[i] = true;
            continue;
        }

        vec2 remote_pos = v2(players[i].x, players[i].y);
        if (v2_dist_sq(local->ship->pos, remote_pos) <= tractor_range_sq)
            g.scanned_players[i] = true;
    }
}

void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {
    bool asteroid_towed[MAX_ASTEROIDS];
    net_collect_towed_asteroids(asteroid_towed);

    bool received[MAX_ASTEROIDS];
    memset(received, 0, sizeof(received));
    const NetProtocolInfo *info = net_protocol_info();
    bool replacement_snapshot = !info ||
        (info->capabilities & SIGNAL_PROTOCOL_CAP_ASTEROID_REMOVE) == 0;

    for (int i = 0; i < count; i++) {
        uint16_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;
        received[idx] = true;

        float elapsed = g.asteroid_interp.elapsed[idx];
        asteroid_t visual = asteroid_render_state_at(
            (int)idx, elapsed, asteroid_towed[idx]);
        g.asteroid_interp.prev[idx] = visual;
        g.asteroid_interp.elapsed[idx] = 0.0f;

        asteroid_t* a = &g.asteroid_interp.curr[idx];
        bool was_active = a->active;
        bool was_child = a->fracture_child;
        asteroid_tier_t was_tier = a->tier;
        commodity_t was_commodity = a->commodity;
        float carried_age = visual.age;
        a->active = (asteroids[i].flags & 1) != 0;
        a->fracture_child = (asteroids[i].flags & (1 << 1)) != 0;
        a->tier = (asteroid_tier_t)((asteroids[i].flags >> 2) & 0x7);
        a->commodity = (commodity_t)((asteroids[i].flags >> 5) & 0x7);
        a->pos.x = asteroids[i].x;
        a->pos.y = asteroids[i].y;
        a->vel.x = asteroids[i].vx;
        a->vel.y = asteroids[i].vy;
        a->hp    = asteroids[i].hp;
        a->ore   = asteroids[i].ore;
        a->radius = asteroids[i].radius;
        a->smelt_progress = asteroids[i].smelt_progress;
        a->grade = asteroids[i].grade;
        a->crystal_stage = asteroids[i].crystal_stage;
        a->phase = asteroids[i].phase;
        bool same_identity = was_active && a->active &&
            was_child == a->fracture_child &&
            was_tier == a->tier &&
            was_commodity == a->commodity;
        if (!a->active) {
            a->age = 0.0f;
            a->max_hp = 0.0f;
            a->max_ore = 0.0f;
            g.asteroid_interp.prev[idx].active = false;
        } else if (same_identity) {
            a->age = carried_age;
        } else {
            a->age = 0.0f;
            a->max_hp = 0.0f;
            a->max_ore = 0.0f;
            g.asteroid_interp.prev[idx].active = false;
        }
        if (a->max_hp < a->hp) a->max_hp = a->hp;
        if (a->max_ore < a->ore) a->max_ore = a->ore;
        net_preserve_local_towed_asteroid_prediction((int)idx);
    }

    if (replacement_snapshot) {
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (!received[i] && g.asteroid_interp.curr[i].active) {
                /* Legacy snapshots are replacement lists, so a missing
                 * asteroid means inactive. Modern streams use explicit
                 * WORLD_ASTEROID_REMOVE records; sparse identity upserts must
                 * not collapse motion targets for unrelated rocks. */
                asteroid_t visual = asteroid_render_state_at(
                    i, g.asteroid_interp.elapsed[i], asteroid_towed[i]);
                g.asteroid_interp.prev[i] = visual;
                g.asteroid_interp.curr[i] = visual;
                g.asteroid_interp.curr[i].active = false;
                g.asteroid_interp.prev[i].active = false;
                g.asteroid_interp.elapsed[i] = 0.0f;
            }
        }
    }

    /* World asteroids are updated by interpolate_world_for_render() at
     * render time, ensuring game logic and rendering see the same positions. */
}

void apply_remote_asteroid_motion(const NetAsteroidMotionState* asteroids,
                                  int count) {
    if (!asteroids || count <= 0) return;
    bool asteroid_towed[MAX_ASTEROIDS];
    net_collect_towed_asteroids(asteroid_towed);

    for (int i = 0; i < count; i++) {
        uint16_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;

        asteroid_t* a = &g.asteroid_interp.curr[idx];
        if (!a->active) continue;
        asteroid_t visual = asteroid_render_state_at(
            (int)idx, g.asteroid_interp.elapsed[idx], asteroid_towed[idx]);
        g.asteroid_interp.prev[idx] = visual;
        g.asteroid_interp.elapsed[idx] = 0.0f;

        float carried_age = visual.age;
        vec2 carried_vel = visual.vel;
        bool keep_velocity =
            !isfinite(asteroids[i].vx) || !isfinite(asteroids[i].vy);
        a->pos.x = asteroids[i].x;
        a->pos.y = asteroids[i].y;
        a->vel.x = keep_velocity ? carried_vel.x : asteroids[i].vx;
        a->vel.y = keep_velocity ? carried_vel.y : asteroids[i].vy;
        a->age = carried_age;
        net_preserve_local_towed_asteroid_prediction((int)idx);
    }
}

void apply_remote_asteroid_state_q(const NetAsteroidStateQ* asteroids,
                                   int count) {
    if (!asteroids || count <= 0) return;
    for (int i = 0; i < count; i++) {
        uint16_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;
        asteroid_t *a = &g.asteroid_interp.curr[idx];
        if (!a->active) continue;
        a->hp = asteroids[i].hp;
        a->ore = asteroids[i].ore;
        a->radius = asteroids[i].radius;
        a->smelt_progress = asteroids[i].smelt_progress;
        a->grade = asteroids[i].grade;
        a->crystal_stage = asteroids[i].crystal_stage;
        a->phase = asteroids[i].phase;
        if (a->max_hp < a->hp) a->max_hp = a->hp;
        if (a->max_ore < a->ore) a->max_ore = a->ore;

        asteroid_t *prev = &g.asteroid_interp.prev[idx];
        if (prev->active) {
            prev->hp = a->hp;
            prev->ore = a->ore;
            prev->radius = a->radius;
            prev->smelt_progress = a->smelt_progress;
            prev->grade = a->grade;
            prev->crystal_stage = a->crystal_stage;
            prev->phase = a->phase;
            if (prev->max_hp < prev->hp) prev->max_hp = prev->hp;
            if (prev->max_ore < prev->ore) prev->max_ore = prev->ore;
        }
    }
}

static int16_t remote_npc_asteroid_index(int value) {
    return (value >= 0 && value < MAX_ASTEROIDS) ? (int16_t)value : -1;
}

void apply_remote_npcs(const NetNpcState* npcs, int count) {
    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        g.npc_interp.prev[i] = npc_render_state_at(i, npc_elapsed);

    float packet_interval = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, packet_interval, 0.3f);
    g.npc_interp.t = 0.0f;

    bool received[MAX_NPC_SHIPS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;
        received[idx] = true;

        client_npc_render_state_t* n = &g.npc_interp.curr[idx];
        n->active = (npcs[i].flags & 1) != 0;
        n->role = (npc_role_t)((npcs[i].flags >> 1) & 0x3);
        n->state = (npc_state_t)((npcs[i].flags >> 3) & 0x7);
        n->thrusting = (npcs[i].flags & (1 << 6)) != 0;
        n->hull_class = npc_default_hull_class_for_role(n->role);
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->vel.x = npcs[i].vx;
        n->vel.y = npcs[i].vy;
        n->angle = npcs[i].angle;
        n->target_asteroid =
            remote_npc_asteroid_index(npcs[i].target_asteroid);
        n->towed_fragment =
            remote_npc_asteroid_index(npcs[i].towed_fragment);
        n->towed_scaffold = -1;
        n->tint_r = (float)npcs[i].tint_r / 255.0f;
        n->tint_g = (float)npcs[i].tint_g / 255.0f;
        n->tint_b = (float)npcs[i].tint_b / 255.0f;
        memcpy(n->session_token, npcs[i].session_token, sizeof(n->session_token));
        n->home_station = (npcs[i].home_station == 0xFFu)
            ? -1 : (int)npcs[i].home_station;
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!received[i]) {
            g.npc_interp.curr[i].active = false;
        }
    }

    /* World NPCs updated by interpolate_world_for_render(). */
}

void apply_remote_npc_motion(const NetNpcMotionState* npcs, int count) {
    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        g.npc_interp.prev[i] = npc_render_state_at(i, npc_elapsed);

    float packet_interval = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, packet_interval, 0.3f);
    g.npc_interp.t = 0.0f;

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;

        client_npc_render_state_t* n = &g.npc_interp.curr[idx];
        if (!n->active) continue;
        n->thrusting = (npcs[i].flags & (1 << 6)) != 0;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->vel.x = npcs[i].vx;
        n->vel.y = npcs[i].vy;
        n->angle = npcs[i].angle;
    }

    /* Visibility and identity stay owned by full WORLD_NPCS records. */
}

void apply_remote_npc_pos(const NetNpcPosState* npcs, int count) {
    if (!npcs || count <= 0) return;

    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        g.npc_interp.prev[i] = npc_render_state_at(i, npc_elapsed);

    float packet_interval = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, packet_interval, 0.3f);
    g.npc_interp.t = 0.0f;

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;

        client_npc_render_state_t* n = &g.npc_interp.curr[idx];
        if (!n->active) continue;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
    }
}

void apply_remote_npc_pose(const NetNpcPoseState* npcs, int count) {
    if (!npcs || count <= 0) return;

    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        g.npc_interp.prev[i] = npc_render_state_at(i, npc_elapsed);

    float packet_interval = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, packet_interval, 0.3f);
    g.npc_interp.t = 0.0f;

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;

        client_npc_render_state_t* n = &g.npc_interp.curr[idx];
        if (!n->active) continue;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->angle = npcs[i].angle;
    }
}

void apply_remote_npc_linear(const NetNpcLinearState* npcs, int count) {
    if (!npcs || count <= 0) return;

    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        g.npc_interp.prev[i] = npc_render_state_at(i, npc_elapsed);

    float packet_interval = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, packet_interval, 0.3f);
    g.npc_interp.t = 0.0f;

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;

        client_npc_render_state_t* n = &g.npc_interp.curr[idx];
        if (!n->active) continue;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->vel.x = npcs[i].vx;
        n->vel.y = npcs[i].vy;
    }
}

void apply_remote_npc_status(const NetNpcStatusState* npcs, int count) {
    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;

        client_npc_render_state_t* n = &g.npc_interp.curr[idx];
        if (!n->active) continue;
        n->role = (npc_role_t)((npcs[i].flags >> 1) & 0x3);
        n->state = (npc_state_t)((npcs[i].flags >> 3) & 0x7);
        n->hull_class = npc_default_hull_class_for_role(n->role);
        n->target_asteroid =
            remote_npc_asteroid_index(npcs[i].target_asteroid);
        n->towed_fragment =
            remote_npc_asteroid_index(npcs[i].towed_fragment);
        n->towed_scaffold = -1;
    }
}

void apply_remote_stations(uint8_t index, const float* inventory, float credit_pool) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    /* Diff against last seen inventory + credit_pool to fire a chain-
     * event heartbeat pulse on the world. Inventory tracks production
     * and consumption; credit_pool tracks commerce (ledger movement
     * from sales, supplier credits, contract payouts). Together they
     * cover the chain events visible to a player at-a-glance.
     * Thresholds are loose so float drift in the smelter (~0.016/tick)
     * doesn't fire every frame: 0.5 units of any commodity, or 5
     * credits of pool delta. Mirror the offline existence gate
     * so an uninhabited slot with stale prev_seen=true doesn't fire. */
    if (g.station_prev_seen[index] && station_exists(st)) {
        bool fired = false;
        for (int i = 0; i < COMMODITY_COUNT; i++) {
            if (fabsf(inventory[i] -
                      g.station_stock_summary[index][i]) >= 0.5f) {
                fired = true;
                break;
            }
        }
        if (!fired
            && fabsf(credit_pool - g.station_prev_credit_pool[index]) >= 5.0f) {
            fired = true;
        }
        if (fired) g.station_heartbeat[index] = 1.0f;
    }
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        g.station_stock_summary[index][i] = inventory[i];
    }
    /* Raw ore is genuinely stored in station_t on both sides. Finished
     * stock remains exclusively in the client read model above. */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        st->_inventory_cache[i] = inventory[i];
    for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++)
        st->_inventory_cache[i] = 0.0f;
    g.station_stock_summary_valid[index] = true;
    g.station_prev_credit_pool[index] = credit_pool;
    g.station_prev_seen[index] = station_exists(st);
}

/* Phase 2 wire: server → client station manifest summary. Fully
 * replaces the (commodity, grade) count matrix for this station so a
 * missing entry reads as zero. */
void apply_remote_station_manifest(uint8_t station_id,
                                   const NetStationManifestEntry *entries,
                                   int count) {
    if (station_id >= MAX_STATIONS) return;
    if (count < 0) count = 0;
    memset(&g.station_manifest_summary[station_id][0][0], 0,
           sizeof(g.station_manifest_summary[station_id]));
    for (int i = 0; i < count; i++) {
        uint8_t c = entries[i].commodity;
        uint8_t gr = entries[i].grade;
        if (c >= COMMODITY_COUNT) continue;
        if (gr >= MINING_GRADE_COUNT) continue;
        g.station_manifest_summary[station_id][c][gr] = entries[i].count;
    }
}

static bool cargo_unit_from_named_ingot_entry(const NetNamedIngotEntry *entry,
                                             cargo_unit_t *out) {
    if (!entry || !out) return false;
    if (entry->commodity >= COMMODITY_COUNT) return false;
    if (entry->grade >= MINING_GRADE_COUNT) return false;
    memset(out, 0, sizeof(*out));
    out->kind = (uint8_t)CARGO_KIND_INGOT;
    out->commodity = entry->commodity;
    out->grade = entry->grade;
    out->prefix_class = entry->prefix_class;
    if (out->prefix_class >= INGOT_PREFIX_COUNT)
        out->prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    out->recipe_id = (uint16_t)RECIPE_SMELT;
    out->origin_station = entry->origin_station;
    out->quantity = 1;
    out->mined_block = entry->mined_block;
    memcpy(out->pub, entry->pub, sizeof(out->pub));
    return true;
}

/* Detailed station named-ingot snapshot. The station manifest remains a
 * partial provenance mirror under network authority: counts come from
 * g.station_manifest_summary, while this manifest holds only the named
 * ingot units needed for representative lineage strings. */
void apply_remote_station_ingots(uint8_t station_id,
                                 const NetNamedIngotEntry *entries,
                                 int count) {
    if (station_id >= MAX_STATIONS) return;
    if (count < 0) count = 0;
    if (count > NET_NAMED_INGOT_MAX) count = NET_NAMED_INGOT_MAX;
    station_t *st = &g.world.stations[station_id];
    if (!st->manifest.units && !station_manifest_bootstrap(st)) return;
    manifest_clear(&st->manifest);
    ship_receipts_t *station_receipts = station_get_receipts(st);
    if (station_receipts) ship_receipts_clear(station_receipts);
    for (int i = 0; i < count; i++) {
        cargo_unit_t unit = {0};
        if (!cargo_unit_from_named_ingot_entry(&entries[i], &unit)) continue;
        if (!station_manifest_push_with_chain(st, &unit, NULL)) break;
    }
}

void apply_remote_hold_ingots(const NetNamedIngotEntry *entries, int count) {
    if (count < 0) count = 0;
    if (count > NET_NAMED_INGOT_MAX) count = NET_NAMED_INGOT_MAX;
    g.remote_hold_named_ingot_count = 0;
    if (!entries || count == 0) return;
    for (int i = 0; i < count; i++)
        g.remote_hold_named_ingots[g.remote_hold_named_ingot_count++] = entries[i];
}

static int remote_pending_receipt_find(const uint8_t cargo_pub[32]) {
    if (!cargo_pub) return -1;
    for (int i = 0; i < remote_pending_receipt_count; i++) {
        if (memcmp(remote_pending_receipt_pub[i], cargo_pub, 32) == 0)
            return i;
    }
    return -1;
}

static bool receipt_chain_cargo_pub(const cargo_receipt_chain_t *chain,
                                    uint8_t out[32]) {
    static const uint8_t zero32[32] = {0};
    if (!chain || chain->len == 0 || chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return false;
    if (memcmp(chain->links[0].cargo_pub, zero32, 32) == 0) return false;
    memcpy(out, chain->links[0].cargo_pub, 32);
    return true;
}

static bool remote_attach_receipt_chain(ship_t *ship,
                                        const cargo_receipt_chain_t *chain) {
    uint8_t cargo_pub[32];
    if (!ship || !chain || !receipt_chain_cargo_pub(chain, cargo_pub))
        return false;
    if (!ship->manifest.units || !ship->receipts_opaque) return false;
    int idx = manifest_find(&ship->manifest, cargo_pub);
    if (idx < 0) return false;
    ship_receipts_t *receipts = ship_get_receipts(ship);
    if (!receipts || idx >= (int)receipts->count) return false;
    receipts->chains[idx] = *chain;
    return true;
}

static void remote_store_receipt_chain(const cargo_receipt_chain_t *chain) {
    uint8_t cargo_pub[32];
    if (!receipt_chain_cargo_pub(chain, cargo_pub)) return;
    int idx = remote_pending_receipt_find(cargo_pub);
    if (idx < 0) {
        if (remote_pending_receipt_count >= REMOTE_PENDING_RECEIPT_CAP) {
            memmove(remote_pending_receipt_pub,
                    &remote_pending_receipt_pub[1],
                    (REMOTE_PENDING_RECEIPT_CAP - 1) * sizeof(remote_pending_receipt_pub[0]));
            memmove(remote_pending_receipts,
                    &remote_pending_receipts[1],
                    (REMOTE_PENDING_RECEIPT_CAP - 1) * sizeof(remote_pending_receipts[0]));
            idx = REMOTE_PENDING_RECEIPT_CAP - 1;
        } else {
            idx = remote_pending_receipt_count++;
        }
    }
    memcpy(remote_pending_receipt_pub[idx], cargo_pub, 32);
    remote_pending_receipts[idx] = *chain;

    if (g.local_player_slot >= 0 && g.local_player_slot < MAX_PLAYERS)
        (void)remote_attach_receipt_chain(g.world.players[g.local_player_slot].ship,
                                          chain);
}

static bool receipt_bundle_is_one_chain(const cargo_receipt_t *receipts,
                                        int count) {
    if (!receipts || count <= 0 || count > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return false;
    const uint8_t *cargo_pub = receipts[0].cargo_pub;
    for (int i = 1; i < count; i++) {
        if (memcmp(receipts[i].cargo_pub, cargo_pub, 32) != 0)
            return false;
    }
    return cargo_receipt_chain_verify(receipts, (size_t)count, cargo_pub)
        == CARGO_RECEIPT_OK;
}

void apply_remote_cargo_receipt_bundle(const cargo_receipt_t *receipts,
                                       int count) {
    if (!receipts || count <= 0) return;
    if (count > CARGO_RECEIPT_CHAIN_MAX_LEN)
        count = CARGO_RECEIPT_CHAIN_MAX_LEN;

    if (receipt_bundle_is_one_chain(receipts, count)) {
        cargo_receipt_chain_t chain = {0};
        memcpy(chain.links, receipts, (size_t)count * sizeof(receipts[0]));
        chain.len = (uint8_t)count;
        remote_store_receipt_chain(&chain);
        return;
    }

    for (int i = 0; i < count; i++) {
        if (cargo_receipt_chain_verify(&receipts[i], 1, receipts[i].cargo_pub)
            != CARGO_RECEIPT_OK) {
            continue;
        }
        cargo_receipt_chain_t chain = {0};
        chain.links[0] = receipts[i];
        chain.len = 1;
        remote_store_receipt_chain(&chain);
    }
}

void apply_remote_inspect_snapshot(const NetInspectSnapshot *snapshot) {
    if (!snapshot) return;

    /* Linger: keep the snapshot on screen for ~3.5s after release.
     * The was_active flag marks the active→idle edge. Once we've
     * bumped the timer into the linger window, was_active is false
     * and this branch is a no-op until the next active scan — the
     * timer counts down naturally. Earlier `timer ≤ 0.60` trick
     * silently re-fired ~2.9s into the linger when the timer
     * crossed back below 0.60 on its way to zero, restarting the
     * countdown indefinitely. */
    if (snapshot->target_type == INSPECT_TARGET_NONE) {
        if (g.inspect_was_active) {
            g.inspect_snapshot_timer = 3.5f;
            g.inspect_was_active = false;
        }
    } else {
        if (g.inspect_snapshot.target_type != snapshot->target_type ||
            g.inspect_snapshot.target_index != snapshot->target_index ||
            g.inspect_snapshot.module_index != snapshot->module_index) {
            g.inspect_receipt_page = 0;
            g.inspect_receipt_browser = false;
        }
        g.inspect_snapshot = *snapshot;
        g.inspect_snapshot_timer = 0.60f;
        g.inspect_was_active = true;
    }

    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &g.world.players[g.local_player_slot];
    if (snapshot->target_type == INSPECT_TARGET_NONE) {
        sp->scan_active = false;
        sp->scan_target_type = 0;
        sp->scan_target_index = -1;
        sp->scan_module_index = -1;
        return;
    }

    sp->scan_active = true;
    sp->scan_target_type = (int)snapshot->target_type;
    sp->scan_target_index = (snapshot->target_index == 0xFFu)
        ? -1 : (int)snapshot->target_index;
    sp->scan_module_index = (snapshot->module_index == 0xFFu)
        ? -1 : (int)snapshot->module_index;
}

void apply_remote_highscores(const NetHighscoreEntry *entries, int count) {
    if (count < 0) count = 0;
    int cap = (int)(sizeof(g.highscores) / sizeof(g.highscores[0]));
    if (count > cap) count = cap;
    memset(g.highscores, 0, sizeof(g.highscores));
    for (int i = 0; i < count; i++) {
        memcpy(g.highscores[i].callsign, entries[i].callsign, 8);
        g.highscores[i].credits_earned = entries[i].credits_earned;
        g.highscores[i].world_id   = entries[i].world_id;
        g.highscores[i].world_seq  = entries[i].world_seq;
        g.highscores[i].build_id   = entries[i].build_id;
        g.highscores[i].epoch_tick = entries[i].epoch_tick;
        memcpy(g.highscores[i].killed_by, entries[i].killed_by, 8);
    }
    g.highscore_count = count;
}

/* Replace the local player's ship.manifest with units that match the
 * server-authoritative count summary. HOLD_INGOTS supplies detailed
 * named-ingot provenance for units the protocol can describe; the rest
 * are synthesized legacy-migrate units so counts remain complete. */
void apply_remote_player_manifest(const NetStationManifestEntry *entries,
                                  int count) {
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    ship_t *ship = g.world.players[g.local_player_slot].ship;
    /* Always apply -- WORLD_STATE overwrites cargo[] every tick, so
     * gating manifest on action_predict_timer leaves cargo and
     * manifest in inconsistent states (cargo refreshed, manifest
     * frozen at pre-action). The trade UI then shows phantom rows
     * (manifest > cargo). The brief predict/snapshot flicker is the
     * lesser evil compared to ghost SELL rows the player can't act on. */
    if (!ship->manifest.units && !ship_manifest_bootstrap(ship)) return;
    manifest_clear(&ship->manifest);
    ship_receipts_t *receipts = ship_get_receipts(ship);
    if (receipts) ship_receipts_clear(receipts);
    if (count <= 0) return;
    uint8_t origin[8] = { 'S','R','V','M','I','R','R','0' };
    uint16_t out_idx = 0;
    bool named_used[NET_NAMED_INGOT_MAX] = { false };
    for (int i = 0; i < count; i++) {
        uint8_t c = entries[i].commodity;
        uint8_t gr = entries[i].grade;
        uint16_t n = entries[i].count;
        if (c >= COMMODITY_COUNT) continue;
        if (gr >= MINING_GRADE_COUNT) continue;
        cargo_kind_t kind;
        if (!cargo_kind_for_commodity((commodity_t)c, &kind)) continue;
        uint16_t remaining = n;
        for (int j = 0; j < g.remote_hold_named_ingot_count && remaining > 0; j++) {
            if (named_used[j]) continue;
            const NetNamedIngotEntry *entry = &g.remote_hold_named_ingots[j];
            if (entry->commodity != c || entry->grade != gr) continue;
            if (ship->manifest.count >= ship->manifest.cap) return;
            cargo_unit_t unit = {0};
            if (!cargo_unit_from_named_ingot_entry(entry, &unit)) continue;
            if (!ship_manifest_push_with_chain(ship, &unit, NULL)) return;
            int pending_idx = remote_pending_receipt_find(unit.pub);
            if (pending_idx >= 0)
                (void)remote_attach_receipt_chain(ship,
                                                  &remote_pending_receipts[pending_idx]);
            named_used[j] = true;
            remaining--;
        }
        for (uint16_t k = 0; k < remaining; k++) {
            if (ship->manifest.count >= ship->manifest.cap) return;
            cargo_unit_t unit = {0};
            if (!hash_legacy_migrate_unit(origin, (commodity_t)c, out_idx++, &unit))
                continue;
            unit.grade = gr;
            if (!ship_manifest_push_with_chain(ship, &unit, NULL)) return;
        }
    }
}

void apply_remote_contracts(const contract_t* contracts, int count) {
    /* Full replacement: clear all, then copy received */
    for (int i = 0; i < MAX_CONTRACTS; i++)
        g.world.contracts[i].active = false;
    for (int i = 0; i < count && i < MAX_CONTRACTS; i++)
        g.world.contracts[i] = contracts[i];
}

void apply_remote_player_known_contracts(uint32_t mask) {
    g.player_known_contract_mask = mask;
}

void apply_remote_player_market_memories(
    const NetMarketMemoryEntry *entries, int count) {
    /* Loopback already shares the authoritative ship component; rebuilding it
     * from a presentation packet would discard carried contract summaries. */
    if (net_is_loopback()) return;
    if (count < 0) count = 0;
    if (count > PLAYER_MARKET_MEMORY_MAX_RECORDS)
        count = PLAYER_MARKET_MEMORY_MAX_RECORDS;
    knowledge_view_t *view = &LOCAL_PLAYER.ship->knowledge;
    memset(view, 0, sizeof(*view));
    view->capacity = SHIP_KNOWN_ITEM_CAP;
    for (int i = 0; i < count; i++) {
        const market_memory_t *memory = &entries[i].memory;
        if (!memory->active ||
            memory->memory_kind == (uint8_t)MARKET_MEMORY_NONE) {
            continue;
        }
        knowledge_item_t *item = &view->items[view->count++];
        item->kind = (uint8_t)KNOW_MARKET;
        item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
        item->confidence = memory->confidence;
        item->salience = memory->salience;
        item->hops = entries[i].hops;
        item->observed_tick = memory->observed_tick;
        item->learned_tick = memory->observed_tick;
        memcpy(item->payload, memory, sizeof(*memory));
    }
}

void apply_remote_player_known_ledger(const NetKnownLedgerEntry *entries,
                                      int count) {
    if (count < 0) count = 0;
    if (count > PLAYER_KNOWN_LEDGER_MAX_RECORDS)
        count = PLAYER_KNOWN_LEDGER_MAX_RECORDS;
    g.known_station_ledger_count = count;
    for (int i = 0; i < count; i++)
        g.known_station_ledger[i] = entries[i];
}

void apply_remote_delivery_ledger(const NetDeliveryLedgerEntry *entries,
                                  int count) {
    if (count < 0) count = 0;
    if (count > DELIVERY_LEDGER_MAX_RECORDS)
        count = DELIVERY_LEDGER_MAX_RECORDS;
    g.delivery_ledger_count = count;
    for (int i = 0; i < count; i++)
        g.delivery_ledger[i] = entries[i];
}

void apply_remote_station_identity(const NetStationIdentity* si) {
    if (si->index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[si->index];
    float local_rotation[MAX_ARMS];
    for (int a = 0; a < MAX_ARMS; a++)
        local_rotation[a] = st->arm_rotation[a];
    bool smooth_rotation = station_ring_have_snapshot[si->index];

    st->scaffold = (si->flags & 1) != 0;
    st->planned  = (si->flags & 2) != 0;
    st->scaffold_progress = si->scaffold_progress;
    st->services = si->services;
    st->pos = v2(si->pos_x, si->pos_y);
    st->radius = si->radius;
    st->dock_radius = si->dock_radius;
    st->signal_range = si->signal_range;
    snprintf(st->name, sizeof(st->name), "%s", si->name);
    for (int c = 0; c < COMMODITY_COUNT; c++)
        st->base_price[c] = si->base_price[c];
    station_reconcile_module_diag_for_identity(st, si->modules, si->module_count);
    st->module_count = si->module_count;
    for (int m = 0; m < si->module_count && m < MAX_MODULES_PER_STATION; m++)
        station_module_copy_identity(&st->modules[m], &si->modules[m]);
    for (int m = si->module_count; m < MAX_MODULES_PER_STATION; m++)
        memset(&st->modules[m], 0, sizeof(st->modules[m]));
    st->arm_count = si->arm_count;
    for (int a = 0; a < MAX_ARMS; a++) {
        st->arm_speed[a] = si->arm_speed[a];
        st->ring_offset[a] = si->ring_offset[a];
        if (smooth_rotation) {
            station_ring_correction[si->index][a] =
                nearest_angle_delta(local_rotation[a], si->arm_rotation[a]);
            st->arm_rotation[a] = local_rotation[a];
        } else {
            st->arm_rotation[a] = si->arm_rotation[a];
            station_ring_correction[si->index][a] = 0.0f;
        }
        st->arm_omega[a] = si->arm_omega[a];
    }
    station_ring_have_snapshot[si->index] = true;
    /* Placement plans (faction-shared blueprint slots) */
    st->placement_plan_count = si->plan_count;
    for (int p = 0; p < si->plan_count && p < 8; p++) {
        st->placement_plans[p].type  = si->plans[p].type;
        st->placement_plans[p].ring  = si->plans[p].ring;
        st->placement_plans[p].slot  = si->plans[p].slot;
        st->placement_plans[p].owner = si->plans[p].owner;
    }
    /* Pending shipyard orders — head-of-queue first */
    st->pending_scaffold_count = si->pending_scaffold_count;
    if (st->pending_scaffold_count > 4) st->pending_scaffold_count = 4;
    for (int p = 0; p < st->pending_scaffold_count; p++) {
        st->pending_scaffolds[p].type  = si->pending_scaffolds[p].type;
        st->pending_scaffolds[p].owner = si->pending_scaffolds[p].owner;
    }
    st->pending_ship_build_count = si->pending_ship_build_count;
    if (st->pending_ship_build_count > 4) st->pending_ship_build_count = 4;
    for (int p = 0; p < st->pending_ship_build_count; p++) {
        st->pending_ship_builds[p].hull_class =
            si->pending_ship_builds[p].hull_class;
        st->pending_ship_builds[p].owner =
            si->pending_ship_builds[p].owner;
        st->pending_ship_builds[p].build_progress =
            si->pending_ship_builds[p].build_progress;
    }
    memcpy(st->stored_hull_count, si->stored_hull_count,
           sizeof(st->stored_hull_count));
    snprintf(st->hail_message, sizeof(st->hail_message), "%s", si->hail_message);
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        snprintf(st->miner_chatter[i], sizeof(st->miner_chatter[i]), "%s",
                 si->miner_chatter[i]);
        snprintf(st->hauler_chatter[i], sizeof(st->hauler_chatter[i]), "%s",
                 si->hauler_chatter[i]);
    }
    snprintf(st->rati_hail_message, sizeof(st->rati_hail_message), "%s",
             si->rati_hail_message);
    snprintf(st->currency_name, sizeof(st->currency_name), "%s", si->currency_name);
    /* Mirror the station's Ed25519 pubkey for client-side verification of
     * future signed events (#479 B). The secret stays server-side. */
    memcpy(st->station_pubkey, si->station_pubkey, sizeof(st->station_pubkey));
    st->faction_id = si->faction_id;
    st->faction_allegiance = si->faction_allegiance;
    st->faction_ideology = si->faction_ideology;
    memcpy(st->faction_relations, si->faction_relations,
           sizeof(st->faction_relations));
    st->policy_card_count = si->policy_card_count;
    if (st->policy_card_count > STATION_IDENTITY_POLICY_CARD_COUNT)
        st->policy_card_count = STATION_IDENTITY_POLICY_CARD_COUNT;
    for (int i = 0; i < STATION_IDENTITY_POLICY_CARD_COUNT; i++) {
        st->policy_card_ids[i] =
            (i < st->policy_card_count) ? si->policy_card_ids[i] : 0;
        st->policy_card_domains[i] = 0;
        st->policy_card_costs[i] = 0;
        st->policy_card_scores[i] = 0.0f;
    }
}

void apply_remote_station_diag(uint8_t station_id, const uint8_t *diag,
                               int module_count) {
    if (station_id >= MAX_STATIONS || !diag) return;
    station_t *st = &g.world.stations[station_id];
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        st->modules[m].flow_diag = (m < module_count) ? diag[m] : STATION_FLOW_DIAG_NONE;
}

void apply_remote_scaffolds(const NetScaffoldState* received, int count) {
    float elapsed = g.scaffold_interp.t * g.scaffold_interp.interval;
    elapsed = clampf(elapsed, 0.0f, SCAFFOLD_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        g.scaffold_interp.prev[i] = scaffold_render_state_at(i, elapsed);

    float packet_interval = clampf(elapsed, 0.05f, 0.2f);
    g.scaffold_interp.interval = lerpf(g.scaffold_interp.interval, packet_interval, 0.3f);
    g.scaffold_interp.t = 0.0f;

    bool seen[MAX_SCAFFOLDS] = { false };
    const NetProtocolInfo *info = net_protocol_info();
    bool replacement_snapshot = !info ||
        (info->capabilities & SIGNAL_PROTOCOL_CAP_SCAFFOLD_REMOVE) == 0;
    for (int i = 0; i < count; i++) {
        uint8_t idx = received[i].index;
        if (idx >= MAX_SCAFFOLDS) continue;
        scaffold_t *sc = &g.scaffold_interp.curr[idx];
        sc->active = true;
        sc->state = (scaffold_state_t)received[i].state;
        sc->module_type = (module_type_t)received[i].module_type;
        sc->owner = received[i].owner;
        sc->pos = v2(received[i].pos_x, received[i].pos_y);
        sc->vel = v2(received[i].vel_x, received[i].vel_y);
        sc->radius = received[i].radius;
        sc->build_amount = received[i].build_amount;
        if (sc->state == SCAFFOLD_NASCENT) {
            /* Nascent scaffolds need built_at_station so the SHIPYARD UI
             * can match them. We don't network it explicitly; instead,
             * derive from nearest station while NASCENT. */
            float best_d = 1e18f;
            int best_s = -1;
            for (int s = 0; s < MAX_STATIONS; s++) {
                const station_t *st = &g.world.stations[s];
                if (!station_exists(st)) continue;
                float d = v2_dist_sq(sc->pos, st->pos);
                if (d < best_d) { best_d = d; best_s = s; }
            }
            sc->built_at_station = best_s;
        } else {
            sc->built_at_station = -1;
        }
        seen[idx] = true;
    }
    if (replacement_snapshot) {
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            if (!seen[i]) g.scaffold_interp.curr[i].active = false;
        }
    }
}

void apply_remote_scaffold_remove(const uint8_t* indices, int count) {
    if (!indices || count <= 0) return;

    float elapsed = g.scaffold_interp.t * g.scaffold_interp.interval;
    elapsed = clampf(elapsed, 0.0f, SCAFFOLD_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        g.scaffold_interp.prev[i] = scaffold_render_state_at(i, elapsed);

    float packet_interval = clampf(elapsed, 0.05f, 0.2f);
    g.scaffold_interp.interval = lerpf(g.scaffold_interp.interval,
                                       packet_interval, 0.3f);
    g.scaffold_interp.t = 0.0f;

    for (int i = 0; i < count; i++) {
        uint8_t idx = indices[i];
        if (idx >= MAX_SCAFFOLDS) continue;
        g.scaffold_interp.curr[idx].active = false;
    }
}

void apply_remote_scaffold_motion(const NetScaffoldMotionState* received,
                                  int count) {
    if (!received || count <= 0) return;

    float elapsed = g.scaffold_interp.t * g.scaffold_interp.interval;
    elapsed = clampf(elapsed, 0.0f, SCAFFOLD_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_SCAFFOLDS; i++)
        g.scaffold_interp.prev[i] = scaffold_render_state_at(i, elapsed);

    float packet_interval = clampf(elapsed, 0.05f, 0.3f);
    g.scaffold_interp.interval = lerpf(g.scaffold_interp.interval,
                                       packet_interval, 0.3f);
    g.scaffold_interp.t = 0.0f;

    for (int i = 0; i < count; i++) {
        uint8_t idx = received[i].index;
        if (idx >= MAX_SCAFFOLDS) continue;
        scaffold_t *sc = &g.scaffold_interp.curr[idx];
        if (!sc->active) continue;
        sc->pos = v2(received[i].pos_x, received[i].pos_y);
        sc->vel = v2(received[i].vel_x, received[i].vel_y);
    }
}

void apply_remote_cargo_pods(const NetCargoPodState* received, int count) {
    bool seen[MAX_CARGO_PODS] = { false };
    const NetProtocolInfo *info = net_protocol_info();
    bool replacement_snapshot = !info ||
        (info->capabilities & SIGNAL_PROTOCOL_CAP_CARGO_POD_REMOVE) == 0;
    for (int i = 0; i < count; i++) {
        uint8_t idx = received[i].index;
        if (idx >= MAX_CARGO_PODS) continue;
        cargo_pod_interp_begin_update(idx);
        cargo_pod_t *pod = &g.cargo_pod_interp.curr[idx];
        pod->active = true;
        pod->kind = (cargo_pod_kind_t)received[i].kind;
        pod->commodity = (commodity_t)received[i].commodity;
        cargo_pod_clear_tractor(pod);
        pod->pos = v2(received[i].pos_x, received[i].pos_y);
        pod->vel = v2(received[i].vel_x, received[i].vel_y);
        pod->radius = received[i].radius;
        pod->rotation = received[i].rotation;
        pod->quantity = received[i].quantity;
        pod->manifest_count = received[i].manifest_count;
        memset(pod->manifest_units, 0, sizeof(pod->manifest_units));
        pod->shipment_id = received[i].shipment_id;
        pod->summary_flags = received[i].summary_flags;
        pod->summary_grade = received[i].summary_grade;
        if (received[i].tractor_player >= 0) {
            cargo_pod_set_player_tractor(pod, received[i].tractor_player);
        } else if (received[i].tractor_station > 0 &&
                   received[i].tractor_module > 0) {
            cargo_pod_set_module_tractor(
                pod, (int)received[i].tractor_station - 1,
                (int)received[i].tractor_module - 1);
        }
        pod->tow_hardpoint_tag = received[i].tow_hardpoint_tag <=
                CARGO_POD_HARDPOINT_COUNT
            ? received[i].tow_hardpoint_tag : 0;
        seen[idx] = true;
    }
    if (replacement_snapshot) {
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            if (seen[i] || !g.cargo_pod_interp.curr[i].active) continue;
            cargo_pod_interp_begin_update(i);
            g.cargo_pod_interp.curr[i].active = false;
        }
    }
    sync_local_towed_pods_from_cargo_authority();
}

void apply_remote_cargo_pod_remove(const uint8_t* indices, int count) {
    if (!indices || count <= 0) return;

    for (int i = 0; i < count; i++) {
        uint8_t idx = indices[i];
        if (idx >= MAX_CARGO_PODS) continue;
        cargo_pod_interp_begin_update(idx);
        g.cargo_pod_interp.curr[idx].active = false;
    }
    sync_local_towed_pods_from_cargo_authority();
}

void apply_remote_cargo_pod_motion(const NetCargoPodMotionState* received,
                                   int count) {
    if (!received || count <= 0) return;

    for (int i = 0; i < count; i++) {
        uint8_t idx = received[i].index;
        if (idx >= MAX_CARGO_PODS) continue;

        cargo_pod_t *pod = &g.cargo_pod_interp.curr[idx];
        if (!pod->active) continue;
        cargo_pod_interp_begin_update(idx);
        pod->pos = v2(received[i].pos_x, received[i].pos_y);
        pod->vel = v2(received[i].vel_x, received[i].vel_y);
        pod->rotation = received[i].rotation;
    }
}

void apply_remote_cargo_pod_linear(const NetCargoPodLinearState* received,
                                   int count) {
    if (!received || count <= 0) return;

    for (int i = 0; i < count; i++) {
        uint8_t idx = received[i].index;
        if (idx >= MAX_CARGO_PODS) continue;

        cargo_pod_t *pod = &g.cargo_pod_interp.curr[idx];
        if (!pod->active) continue;
        cargo_pod_interp_begin_update(idx);
        pod->pos = v2(received[i].pos_x, received[i].pos_y);
        pod->vel = v2(received[i].vel_x, received[i].vel_y);
    }
}

void apply_remote_interactions(const sim_interaction_t *items, int count) {
    memset(&g.world.interactions, 0, sizeof(g.world.interactions));
    if (!items || count <= 0) return;
    if (count > SIM_MAX_INTERACTIONS) count = SIM_MAX_INTERACTIONS;
    for (int i = 0; i < count; i++) {
        if (items[i].type == SIM_INTERACTION_NONE) continue;
        if (g.world.interactions.count >= SIM_MAX_INTERACTIONS) break;
        g.world.interactions.items[g.world.interactions.count++] = items[i];
    }
}

void apply_remote_interaction_drift(const NetInteractionDriftState *items,
                                    int count) {
    if (!items || count <= 0) return;
    if (count > SIM_MAX_INTERACTIONS) count = SIM_MAX_INTERACTIONS;
    for (int i = 0; i < count; i++) {
        uint8_t idx = items[i].index;
        if (idx >= g.world.interactions.count) continue;
        sim_interaction_t *it = &g.world.interactions.items[idx];
        if (it->type == SIM_INTERACTION_NONE) continue;
        it->source_pos = v2(items[i].source_x, items[i].source_y);
        it->target_pos = v2(items[i].target_x, items[i].target_y);
        it->range = items[i].range;
        it->intensity = items[i].intensity;
    }
}

/* Defined in main.c — process events for audio + UI */
extern void process_sim_events(const sim_events_t *events);

void apply_remote_events(const sim_event_t *events, int count) {
    /* Process immediately — Emscripten WebSocket callbacks fire async,
     * so we can't rely on g.world.events surviving until sim_step. */
    if (count > SIM_MAX_EVENTS) count = SIM_MAX_EVENTS;
    sim_events_t temp;
    memcpy(temp.events, events, (size_t)count * sizeof(sim_event_t));
    temp.count = count;
    process_sim_events(&temp);
}

void apply_remote_signal_channel(const NetSignalChannelMsg *msgs, int count) {
    /* Rebuild the client-side ring buffer from the snapshot. Server is
     * authoritative; on every post we get the current tail. */
    signal_channel_t *ch = &g.world.signal_channel;
    memset(ch, 0, sizeof(*ch));
    int n = count;
    if (n > SIGNAL_CHANNEL_CAPACITY) n = SIGNAL_CHANNEL_CAPACITY;
    for (int i = 0; i < n; i++) {
        signal_channel_msg_t *dst = &ch->msgs[i];
        memset(dst, 0, sizeof(*dst));
        dst->id = msgs[i].id;
        dst->timestamp_ms = msgs[i].timestamp_ms;
        dst->sender_station = msgs[i].sender_station;
        size_t tn = strlen(msgs[i].text);
        if (tn > SIGNAL_CHANNEL_TEXT_MAX - 1) tn = SIGNAL_CHANNEL_TEXT_MAX - 1;
        memcpy(dst->text, msgs[i].text, tn);
        dst->text_len = (uint8_t)tn;
        memcpy(dst->entry_hash, msgs[i].entry_hash, sizeof(dst->entry_hash));
        if (msgs[i].id > ch->next_id) ch->next_id = msgs[i].id;
        memcpy(ch->last_hash, dst->entry_hash, sizeof(ch->last_hash));
    }
    ch->count = n;
    ch->head = n % SIGNAL_CHANNEL_CAPACITY;
}

static bool net_hash32_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static void net_station_hail_label(uint8_t station, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (station >= MAX_STATIONS) {
        snprintf(out, cap, "Unknown");
        return;
    }
    const station_t *st = &g.world.stations[station];
    if (net_hash32_is_zero(st->station_pubkey)) {
        snprintf(out, cap, "%s", st->name);
        return;
    }
    char id[8];
    mining_callsign_from_pubkey(st->station_pubkey, id);
    snprintf(out, cap, "%s [%s]", st->name, id);
}

static bool net_hail_reason_text(const NetHailReason *reason,
                                 char *out,
                                 size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!reason || reason->flags == 0u) return false;

    switch ((hail_decision_mode_t)reason->mode) {
    case HAIL_DECISION_MODE_DOCKED:
        snprintf(out, cap, "station knows you are docked");
        return true;
    case HAIL_DECISION_MODE_DOCK_RANGE:
        snprintf(out, cap, "dock signal has priority");
        return true;
    case HAIL_DECISION_MODE_SIGNAL_RANGE:
        if (reason->candidate_count > 1u) {
            snprintf(out, cap, "closest of %u station signals",
                     (unsigned)reason->candidate_count);
        } else {
            snprintf(out, cap, "only station signal in range");
        }
        return true;
    case HAIL_DECISION_MODE_NONE:
    default:
        if (reason->candidate_count == 0u) {
            snprintf(out, cap, "no station signal answered");
            return true;
        }
        break;
    }
    return false;
}

void apply_remote_hail_response(uint8_t station,
                                float credits,
                                int contract_index,
                                const NetHailReason *reason) {
    char why[96];
    bool has_reason = net_hail_reason_text(reason, why, sizeof(why));
    if (station >= MAX_STATIONS) {
        if (has_reason)
            set_notice("Local scan sweep. %s.", why);
        else
            set_notice("Local scan sweep.");
        return;
    }
    /* Use the same hail overlay as local play — station name + the
     * operator-authored station hail + credits. Tutorial/system guidance
     * is intentionally kept out of station hails. */
    net_station_hail_label(station, g.hail_station, sizeof(g.hail_station));
    const char *msg = g.world.stations[station].hail_message;
    snprintf(g.hail_message, sizeof(g.hail_message), "%s",
             msg[0] ? msg : "Signal acknowledged.");
    float shown_credits = credits >= 0.0f ? credits : 0.0f;
    g.hail_credits = shown_credits;
    g.hail_station_index = station;
    g.hail_timer = 6.0f;
    /* Route the hail through the bottom-right hint bar. Includes the
     * station balance so all the info the old center-screen overlay
     * carried lands there. `credits` is authoritative from the server. */
    {
        const char *unit = g.world.stations[station].currency_name;
        if (!unit[0]) unit = "credits";
        if (credits >= 0.0f) {
            if (has_reason) {
                set_notice("%s: %s  (%s; balance %d %s)",
                    g.hail_station, g.hail_message, why,
                    (int)lroundf(shown_credits), unit);
            } else {
                set_notice("%s: %s  (balance %d %s)",
                    g.hail_station, g.hail_message,
                    (int)lroundf(shown_credits), unit);
            }
        } else {
            if (has_reason)
                set_notice("%s: %s  (%s)", g.hail_station,
                           g.hail_message, why);
            else
                set_notice("%s: %s", g.hail_station, g.hail_message);
        }
    }
    /* Track station work when the hail response names a real board contract.
     * Hail itself is scan/contact; the server no longer mints ad hoc
     * nearest-rock fracture jobs just to have something to point at. */
    char step[192];
    if (contract_objective_track_contract(contract_index, step, sizeof(step)))
        set_notice("Tracking: %s", step);
    onboarding_mark_hailed();
}

static NetPlayerState remote_player_render_state_at(int slot, float elapsed) {
    const NetPlayerState *prev = &g.player_interp.prev[slot];
    const NetPlayerState *curr = &g.player_interp.curr[slot];
    NetPlayerState out = *curr;
    if (!curr->active) return out;

    if ((curr->flags & 4u) == 0u) {
        out.x += curr->vx * elapsed;
        out.y += curr->vy * elapsed;
    }
    if (prev->active) {
        float blend =
            clampf(elapsed / REMOTE_PLAYER_RENDER_CORRECTION_SEC, 0.0f, 1.0f);
        out.x = lerpf(prev->x, out.x, blend);
        out.y = lerpf(prev->y, out.y, blend);
        out.angle = lerp_angle(prev->angle, out.angle, blend);
    }
    return out;
}

void begin_player_state_batch(void) {
    float prev_interval = g.net_motion.packet_interval;
    float prev_raw_interval = g.net_motion.raw_packet_interval;
    float raw_elapsed = g.player_interp.t * g.player_interp.interval;
    float render_elapsed =
        clampf(raw_elapsed, 0.0f, REMOTE_PLAYER_RENDER_EXTRAPOLATE_MAX_SEC);
    NetPlayerState carried[NET_MAX_PLAYERS];
    for (int i = 0; i < NET_MAX_PLAYERS; i++)
        carried[i] = remote_player_render_state_at(i, render_elapsed);
    memcpy(g.player_interp.prev, carried, sizeof(g.player_interp.prev));
    memcpy(g.player_interp.curr, carried, sizeof(g.player_interp.curr));

    g.net_motion.raw_packet_interval = raw_elapsed;
    if (raw_elapsed > g.net_motion.max_raw_packet_interval_run)
        g.net_motion.max_raw_packet_interval_run = raw_elapsed;
    if (prev_raw_interval > 0.0f) {
        float raw_jitter = fabsf(raw_elapsed - prev_raw_interval);
        if (raw_jitter > g.net_motion.max_raw_packet_jitter_run)
            g.net_motion.max_raw_packet_jitter_run = raw_jitter;
    }
    float elapsed = clampf(raw_elapsed, 0.03f, 0.15f);
    g.net_motion.packet_interval = elapsed;
    if (elapsed > g.net_motion.max_packet_interval_run)
        g.net_motion.max_packet_interval_run = elapsed;
    if (prev_interval > 0.0f) {
        float jitter = fabsf(elapsed - prev_interval);
        if (jitter > g.net_motion.max_packet_jitter_run)
            g.net_motion.max_packet_jitter_run = jitter;
    }
    g.net_motion.total_player_batches++;
    g.player_interp.interval = lerpf(g.player_interp.interval, elapsed, 0.3f);
    g.player_interp.t = 0.0f;
}

void net_record_input_ack(uint16_t input_seq_ack,
                          uint32_t server_tick,
                          uint32_t input_tick_ack) {
    if (input_seq_ack == 0) return;
    if (input_tick_ack != 0) g.net_input_tick_protocol = true;
    net_record_prediction_tick_skew(server_tick);
    net_observe_server_tick(server_tick);
    if (server_tick != 0 && !g.net_prediction_tick_valid) {
        net_anchor_prediction_tick(server_tick, true);
    }
    if (g.net_last_server_ack == 0 ||
        input_seq_ack == g.net_last_server_ack ||
        net_input_seq_after(input_seq_ack, g.net_last_server_ack)) {
        g.net_last_server_ack = input_seq_ack;
    }

    int index = (int)(input_seq_ack % NET_INPUT_TIMING_CAP);
    net_input_timing_t *timing = &g.net_input_timing[index];
    if (timing->seq != input_seq_ack || timing->sent_at <= 0.0f) return;

    float rtt = 0.0f;
    if (timing->sent_ms != 0) {
        uint32_t elapsed_ms = net_now_ms32() - timing->sent_ms;
        rtt = (float)elapsed_ms / 1000.0f;
    } else {
        rtt = g.net_time - timing->sent_at;
    }
    if (rtt < 0.0f || rtt > 30.0f) return;
    if (timing->target_tick != 0 && input_tick_ack != 0) {
        int32_t error = (int32_t)(input_tick_ack - timing->target_tick);
        int32_t abs_error = error < 0 ? -error : error;
        g.net_motion.input_apply_error_ticks = error;
        if (abs_error > g.net_motion.max_input_apply_error_abs)
            g.net_motion.max_input_apply_error_abs = abs_error;
        g.net_motion.input_lead_margin_ticks =
            net_input_lead_margin_after_ack(
                g.net_motion.input_lead_margin_ticks,
                &g.net_motion.input_lead_exact_acks,
                error);
    }
    g.net_last_ack_rtt = rtt;
    net_latency_stats_observe(&g.net_ack_latency, rtt, g.net_time);
    net_latency_gap_stats_observe(&g.net_ack_gap, rtt,
                                  timing->ping_rtt_at_send, g.net_time);
    g.net_ack_miss_windows_reported = 0;
    if (rtt > g.net_max_ack_rtt_5s) g.net_max_ack_rtt_5s = rtt;
    if (rtt > g.net_motion.max_ack_rtt_run)
        g.net_motion.max_ack_rtt_run = rtt;
    g.net_motion.total_input_acks++;
    timing->seq = 0;
    timing->sent_at = 0.0f;
    timing->sent_ms = 0;
    timing->target_tick = 0;
    timing->ping_rtt_at_send = 0.0f;
}

static void record_local_player_motion_telemetry(float correction_dist,
                                                 float velocity_error,
                                                 float applied_correction_dist,
                                                 bool deferred,
                                                 int replayed_frames) {
    g.net_motion.correction_dist = correction_dist;
    g.net_motion.applied_correction_dist = applied_correction_dist;
    g.net_motion.velocity_error = velocity_error;
    if (correction_dist > g.net_motion.max_correction_5s)
        g.net_motion.max_correction_5s = correction_dist;
    if (applied_correction_dist > g.net_motion.max_applied_correction_5s)
        g.net_motion.max_applied_correction_5s = applied_correction_dist;
    if (correction_dist > g.net_motion.max_correction_run)
        g.net_motion.max_correction_run = correction_dist;
    if (applied_correction_dist > g.net_motion.max_applied_correction_run)
        g.net_motion.max_applied_correction_run = applied_correction_dist;
    if (velocity_error > g.net_motion.max_velocity_error_run)
        g.net_motion.max_velocity_error_run = velocity_error;
    g.net_motion.window_elapsed += g.net_motion.packet_interval;
    g.net_motion.samples++;
    g.net_motion.total_samples++;
    if (deferred) g.net_motion.deferred_samples++;
    if (deferred) g.net_motion.total_deferred_samples++;
    if (replayed_frames > 0) {
        g.net_motion.replayed_samples++;
        g.net_motion.replayed_frames += (uint32_t)replayed_frames;
        g.net_motion.total_replayed_samples++;
        g.net_motion.total_replayed_frames += (uint32_t)replayed_frames;
    }
    if (g.net_motion.window_elapsed < NET_MOTION_TELEMETRY_WINDOW_SEC) return;

    printf("[net-motion] pkt=%.3fs raw=%.3fs corr=%.1f max5=%.1f applied=%.1f maxapp5=%.1f velerr=%.1f input_tick_err=%d lead_margin=%d deferred=%u/%u replayed=%u/%u frames=%u\n",
           g.net_motion.packet_interval,
           g.net_motion.raw_packet_interval,
           g.net_motion.correction_dist,
           g.net_motion.max_correction_5s,
           g.net_motion.applied_correction_dist,
           g.net_motion.max_applied_correction_5s,
           g.net_motion.velocity_error,
           (int)g.net_motion.input_apply_error_ticks,
           (int)g.net_motion.input_lead_margin_ticks,
           (unsigned)g.net_motion.deferred_samples,
           (unsigned)g.net_motion.samples,
           (unsigned)g.net_motion.replayed_samples,
           (unsigned)g.net_motion.samples,
           (unsigned)g.net_motion.replayed_frames);
    g.net_motion.max_correction_5s = 0.0f;
    g.net_motion.max_applied_correction_5s = 0.0f;
    g.net_motion.window_elapsed = 0.0f;
    g.net_motion.samples = 0;
    g.net_motion.deferred_samples = 0;
    g.net_motion.replayed_samples = 0;
    g.net_motion.replayed_frames = 0;
}

static void add_local_player_render_correction(vec2 applied_delta,
                                               float correction_dist,
                                               bool docked) {
    float latency_blend = net_prediction_latency_blend();
    float snap_dist = lerpf(LOCAL_PLAYER_RENDER_SNAP_DIST,
                            LOCAL_PLAYER_RENDER_SNAP_LATENCY_DIST,
                            latency_blend);
    if (docked || correction_dist > snap_dist) {
        g.local_player_render_offset = v2(0.0f, 0.0f);
        return;
    }

    g.local_player_render_offset =
        v2_add(g.local_player_render_offset, applied_delta);
    float len = v2_len(g.local_player_render_offset);
    float max_offset = lerpf(LOCAL_PLAYER_RENDER_OFFSET_MAX,
                             LOCAL_PLAYER_RENDER_OFFSET_LATENCY_MAX,
                             latency_blend);
    if (len > max_offset) {
        g.local_player_render_offset =
            v2_scale(g.local_player_render_offset,
                     max_offset / len);
        len = max_offset;
    }
    if (len > g.net_motion.max_render_offset_run)
        g.net_motion.max_render_offset_run = len;
}

static void frame_camera_on_authoritative_undock(const server_player_t *sp,
                                                 bool state_docked) {
    if (!sp || state_docked) return;
    if (!(g.was_docked || g.action_predict_timer > 0.0f)) return;

    vec2 offset = v2_sub(sp->ship->pos, g.camera_pos);
    if (v2_len_sq(offset) < 140.0f * 140.0f) return;

    g.camera_pos = sp->ship->pos;
    g.camera_initialized = true;
    g.camera_station_index = -1;
    g.camera_station_side = 0;
    g.camera_station_v_side = 0;
    g.camera_drift_timer = 0.0f;
    g.local_player_render_offset = v2(0.0f, 0.0f);
}

void apply_remote_player_state(const NetPlayerState* state) {
    if (state->player_id >= NET_MAX_PLAYERS) return;

    if (state->player_id == net_local_id()) {
        /* Reconcile local prediction with server-authoritative position. */
        server_player_t* sp = &g.world.players[state->player_id];
        vec2 before_pos = sp->ship->pos;
        if (state->has_input_tick_ack) g.net_input_tick_protocol = true;
        bool force_rebase = false;
        int32_t skew = 0;
        if (net_prediction_tick_skew_for_sample(state->server_tick, &skew)) {
            net_record_prediction_tick_skew(state->server_tick);
            force_rebase = skew > (int32_t)NET_REPLAY_REBASE_SKEW_TICKS;
        }
        bool has_input_ack = state->input_seq_ack != 0;
        bool has_unacked_input = net_latest_input_unacked(state);
        if (has_input_ack) {
            /* A restored/reconnected ship can briefly carry an ack from a
             * previous browser input stream. Fast-forward so fresh held
             * controls advance past it instead of being treated as stale. */
            if (g.net_input_seq == 0 ||
                net_input_seq_after(state->input_seq_ack, g.net_input_seq)) {
                g.net_input_seq = state->input_seq_ack;
                g.net_input_have_last = false;
            }
            if (state->ack_client_sent_ms != 0 &&
                state->ack_server_recv_ms != 0 &&
                state->ack_server_send_ms != 0) {
                uint32_t now_ms = net_now_ms32();
                float rtt_ms =
                    (float)(uint32_t)(now_ms - state->ack_client_sent_ms);
                float server_turnaround_ms =
                    (float)(uint32_t)(state->ack_server_send_ms -
                                      state->ack_server_recv_ms);
                net_observe_transport_latency_sample(rtt_ms,
                                                     server_turnaround_ms,
                                                     state->server_tick,
                                                     true);
            }
            net_record_input_ack(state->input_seq_ack,
                                 state->server_tick,
                                 state->input_tick_ack);
        }
        net_observe_server_tick(state->server_tick);
        sync_local_tow_state_from_authority(state, sp);

        if (!g.net_local_state_ready) {
            accept_initial_local_player_state(state, sp);
            return;
        }

        bool state_docked = (state->flags & 4) != 0;
        if (state_docked && sp->docked) {
            accept_docked_local_player_state(state, sp);
            return;
        }
        if (!state_docked && sp->docked) {
            accept_authoritative_local_launch_state(state, sp);
            return;
        }
        if (g.local_server.active && net_is_loopback() &&
            !net_replay_enabled()) {
            apply_authoritative_local_motion(state, sp);
            sync_local_dock_state_from_authority(state, sp);
            apply_local_player_remote_flags(state, sp);
            net_replay_reset();
            net_anchor_prediction_tick(state->server_tick, false);
            g.local_player_render_offset = v2(0.0f, 0.0f);
            g.action_predict_timer = 0.0f;
            if (!state_docked) {
                sp->docked = false;
                sp->docking_approach = false;
                sync_local_undocked_dock_proximity(sp);
            }
            frame_camera_on_authoritative_undock(sp, state_docked);
            return;
        }

        float target_x = state->x;
        float target_y = state->y;

        float dx = target_x - sp->ship->pos.x;
        float dy = target_y - sp->ship->pos.y;
        float dist_sq = dx * dx + dy * dy;
        float correction_dist = sqrtf(dist_sq);
        float dvx = state->vx - sp->ship->vel.x;
        float dvy = state->vy - sp->ship->vel.y;
        float velocity_error = sqrtf(dvx * dvx + dvy * dvy);

        int replayed_frames = 0;
        bool used_replay = false;
        bool used_snap = false;
        bool used_lerp = false;
        bool has_local_turn_prediction =
            net_local_turn_prediction_active(state->server_tick);
        bool defer_motion_correction = false;
        bool defer_predicted_undock =
            state_docked && !sp->docked && g.action_predict_timer > 0.0f;
        if (force_rebase) {
            apply_authoritative_local_motion(state, sp);
            net_replay_clear_frames();
            net_anchor_prediction_tick(state->server_tick, false);
            used_snap = true;
        } else if (defer_predicted_undock) {
            defer_motion_correction = true;
        } else {
            defer_motion_correction =
                should_defer_stale_unacked_motion(state, has_unacked_input, dist_sq);
        }
        if (!force_rebase && !defer_motion_correction)
            used_replay =
                net_replay_reconcile_local_player(state, sp, &replayed_frames);
        if (!force_rebase && !defer_motion_correction && !used_replay) {
            defer_motion_correction =
                should_defer_stale_unacked_motion(state, has_unacked_input, dist_sq) ||
                should_defer_active_prediction_motion(state, dist_sq);
        }
        if (!force_rebase && !used_replay && !defer_motion_correction) {
            if (dist_sq > 200.0f * 200.0f) {
                used_snap = true;
                sp->ship->pos.x = target_x;
                sp->ship->pos.y = target_y;
                sp->ship->vel.x = state->vx;
                sp->ship->vel.y = state->vy;
            } else if (dist_sq > 20.0f * 20.0f) {
                used_lerp = true;
                sp->ship->pos.x = lerpf(sp->ship->pos.x, target_x, 0.5f);
                sp->ship->pos.y = lerpf(sp->ship->pos.y, target_y, 0.5f);
                sp->ship->vel.x = lerpf(sp->ship->vel.x, state->vx, 0.5f);
                sp->ship->vel.y = lerpf(sp->ship->vel.y, state->vy, 0.5f);
            } else {
                used_lerp = dist_sq > 0.01f;
                sp->ship->pos.x = lerpf(sp->ship->pos.x, target_x, 0.2f);
                sp->ship->pos.y = lerpf(sp->ship->pos.y, target_y, 0.2f);
                sp->ship->vel.x = lerpf(sp->ship->vel.x, state->vx, 0.2f);
                sp->ship->vel.y = lerpf(sp->ship->vel.y, state->vy, 0.2f);
            }
            net_replay_reset();
            net_anchor_prediction_tick(state->server_tick, false);
        }
        if (used_snap) g.net_motion.total_snap_samples++;
        if (used_lerp) g.net_motion.total_lerp_samples++;
        vec2 applied_delta = v2_sub(before_pos, sp->ship->pos);
        add_local_player_render_correction(
            applied_delta, v2_len(applied_delta), state_docked);
        record_local_player_motion_telemetry(
            correction_dist, velocity_error, v2_len(applied_delta),
            defer_motion_correction, replayed_frames);
        if (!state_docked) {
            sp->docked = false;
            sp->docking_approach = false;
            sync_local_undocked_dock_proximity(sp);
        }
        frame_camera_on_authoritative_undock(sp, state_docked);
        /* A/D changes often have tiny position error, so stale snapshots can
         * look "safe" to accept while still carrying an older angle. Ack
         * timing can also outrun replay pruning, so key this to active local
         * steering rather than only "latest input unacked"; otherwise the
         * ship visibly turns, then gets blended back by authority. */
        if (!used_replay && !defer_motion_correction &&
            !has_local_turn_prediction)
            sp->ship->angle = lerp_angle(sp->ship->angle, state->angle, 0.3f);
        /* Beam/tractor/thrust visual flags are server-authoritative for the
         * local player too; autopilot and mining effects run server-side. */
        apply_local_player_remote_flags(state, sp);
    } else {
        /* Remote player: update curr for interpolation.
         * begin_player_state_batch() already shifted prev←curr. */
        bool was_active = g.player_interp.prev[state->player_id].active ||
            g.player_interp.curr[state->player_id].active;
        NetPlayerState next = *state;
        if ((next.flags & NET_PLAYER_STATE_STATUS_ONLY) != 0u) {
            next.flags &=
                (uint8_t)(UINT8_MAX ^ NET_PLAYER_STATE_STATUS_ONLY);
            if (g.player_interp.curr[state->player_id].active) {
                uint8_t status_flags =
                    next.flags & PLAYER_DOCK_STATUS_FLAGS_MASK;
                next = g.player_interp.curr[state->player_id];
                next.flags = (uint8_t)(
                    (next.flags &
                     (uint8_t)(UINT8_MAX ^
                               PLAYER_DOCK_STATUS_FLAGS_MASK)) |
                    status_flags);
                if ((status_flags & 0x04u) != 0u) {
                    next.vx = 0.0f;
                    next.vy = 0.0f;
                }
            }
        }
        g.player_interp.curr[state->player_id] = next;
        /* First time we see this player with a callsign — show join notice */
        if (!was_active && next.active && next.callsign[0])
            set_notice("%s joined.", next.callsign);
    }
}

void apply_remote_player_ship(const NetPlayerShipState* state) {
    /* Apply server-authoritative ship state for the local player. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    g.station_balance = state->station_balance;
    /* While the action predict timer is active, the client has made an
     * optimistic change (buy/sell/upgrade/launch) that the server hasn't
     * confirmed yet.  Skip overwriting mutable ship state to prevent
     * flicker from stale PLAYER_SHIP messages. Station balance is not
     * locally predicted under network authority, so keep it authoritative even
     * during the predict window; otherwise ws_send_if_changed can deliver
     * the only changed balance packet while the client is ignoring it. */
    if (g.action_predict_timer <= 0.0f) {
        /* Death detection moved to on_remote_death (NET_MSG_DEATH).
         * The packet now carries position + stats so the cinematic can
         * anchor at the wreckage. */
        sp->ship->hull = state->hull;
        sp->ship->mining_level = (int)state->mining_level;
        sp->ship->hold_level = (int)state->hold_level;
        sp->ship->tractor_level = (int)state->tractor_level;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            sp->ship->cargo[c] = state->cargo[c];
        sp->nearby_fragments = (int)state->nearby_fragments;
        sp->tractor_fragments = (int)state->tractor_fragments;
        sp->ship->towed_count = state->towed_count;
        for (int t = 0; t < 10; t++)
            sp->ship->towed_fragments[t] = (state->towed_fragments[t] == 0xFFFFu)
                ? -1 : (int16_t)state->towed_fragments[t];
        /* Autopilot is also predict-protected: the [O] press triggers an
         * optimistic local toggle, and stale PLAYER_SHIP messages can
         * arrive carrying the pre-toggle value before the server has
         * processed the action. Without this guard the HUD label flickered
         * on/off during the round-trip window. */
        sp->autopilot_mode = state->autopilot_mode;
        sp->autopilot_target = (state->autopilot_target == 0xFF) ? -1 : (int)state->autopilot_target;
        /* Apply server's actual A* path for preview rendering. */
        g.autopilot_path_count = (int)state->path_count;
        g.autopilot_path_current = (int)state->path_current;
        for (int i = 0; i < state->path_count && i < 12; i++)
            g.autopilot_path[i] = v2(state->path_x[i], state->path_y[i]);
    }
    /* Dock-state reconciliation:
     * - Server says undocked -> always accept.
     * - Server says docked  -> only accept if we locally agree
     *   or the predict window has expired. */
    if (!state->docked) {
        sp->docked = false;
        sp->docking_approach = false;
        sync_local_undocked_dock_proximity(sp);
    } else if (sp->docked || g.action_predict_timer <= 0.0f) {
        sp->docked = true;
        sp->current_station = (int)state->current_station;
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
    }
}

void sync_local_player_slot_from_network(void) {
    uint8_t net_id = net_local_id();
    if (net_id == 0xFF || net_id >= MAX_PLAYERS) return;
    if (g.local_player_slot == (int)net_id) {
        LOCAL_PLAYER.connected = true;
        return;
    }

    net_reset_local_input_stream();
    int previous_slot = g.local_player_slot;
    server_player_t* assigned = &g.world.players[net_id];
    bool should_move = !assigned->connected && assigned->ship &&
                       assigned->ship->hull <= 0.0f;
    bool moved = should_move && client_move_player_slot(net_id, previous_slot);
    if (!moved) client_reset_player_slot(previous_slot);
    g.local_player_slot = (int)net_id;
    LOCAL_PLAYER.id = net_id;
    LOCAL_PLAYER.connected = true;
    if (LOCAL_PLAYER.connection) LOCAL_PLAYER.connection->conn = NULL;
}

void interpolate_world_for_render(void) {
    bool asteroid_towed[MAX_ASTEROIDS];
    net_collect_towed_asteroids(asteroid_towed);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *curr = &g.asteroid_interp.curr[i];
        const asteroid_t *prev = &g.asteroid_interp.prev[i];
        /* Skip inactive slots — avoid ~100-byte struct copy for empty entries. */
        if (!curr->active && !prev->active) {
            g.world.asteroids[i].active = false;
            continue;
        }
        asteroid_t *dst = &g.world.asteroids[i];
        *dst = asteroid_render_state_at(
            i, g.asteroid_interp.elapsed[i], asteroid_towed[i]);
    }

    float npc_elapsed = clampf(g.npc_interp.t * g.npc_interp.interval,
                               0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const client_npc_render_state_t *curr = &g.npc_interp.curr[i];
        const client_npc_render_state_t *prev = &g.npc_interp.prev[i];
        if (!curr->active && !prev->active) {
            if (g.world.npc_ships[i].active) {
                world_npc_ship_slot_release(&g.world, i);
                memset(&g.world.npc_ships[i], 0,
                       sizeof(g.world.npc_ships[i]));
            }
            continue;
        }
        client_npc_render_state_t render = npc_render_state_at(i, npc_elapsed);
        if (!g.world.npc_ships[i].ship &&
            !world_npc_ship_slot_activate(&g.world, i)) {
            continue;
        }
        npc_ship_t *dst = &g.world.npc_ships[i];
        dst->active = render.active;
        dst->role = render.role;
        dst->state = render.state;
        dst->thrusting = render.thrusting;
        dst->target_asteroid = render.target_asteroid;
        dst->tint_r = render.tint_r;
        dst->tint_g = render.tint_g;
        dst->tint_b = render.tint_b;
        memcpy(dst->session_token, render.session_token,
               sizeof(dst->session_token));
        dst->home_station = render.home_station;
        dst->ship->hull_class = render.hull_class;
        dst->ship->pos = render.pos;
        dst->ship->vel = render.vel;
        dst->ship->angle = render.angle;
        npc_set_towed_fragment_index(dst, render.towed_fragment);
        dst->ship->towed_scaffold = render.towed_scaffold;
    }

    float scaffold_elapsed = clampf(g.scaffold_interp.t * g.scaffold_interp.interval,
                                    0.0f, SCAFFOLD_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *curr = &g.scaffold_interp.curr[i];
        const scaffold_t *prev = &g.scaffold_interp.prev[i];
        if (!curr->active && !prev->active) {
            g.world.scaffolds[i].active = false;
            continue;
        }
        scaffold_t *dst = &g.world.scaffolds[i];
        *dst = scaffold_render_state_at(i, scaffold_elapsed);
    }

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *curr = &g.cargo_pod_interp.curr[i];
        const cargo_pod_t *prev = &g.cargo_pod_interp.prev[i];
        if (!curr->active && !prev->active) {
            g.world.cargo_pods[i].active = false;
            continue;
        }
        cargo_pod_t *dst = &g.world.cargo_pods[i];
        *dst = cargo_pod_render_state_at(
            i, clampf(g.cargo_pod_interp.elapsed[i], 0.0f,
                      CARGO_POD_RENDER_EXTRAPOLATE_MAX_SEC));
    }
    sync_local_towed_pods_from_cargo_authority();
}

const NetPlayerState* net_get_interpolated_players(void) {
    static NetPlayerState result[NET_MAX_PLAYERS];

    float elapsed = clampf(g.player_interp.t * g.player_interp.interval,
                           0.0f, REMOTE_PLAYER_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < NET_MAX_PLAYERS; i++)
        result[i] = remote_player_render_state_at(i, elapsed);
    return result;
}

void on_remote_death(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured,
                     uint8_t respawn_station, float respawn_fee) {
    if ((int)player_id != g.local_player_slot) return;
    net_replay_reset();
    float impact_speed = sqrtf(vel_x * vel_x + vel_y * vel_y);
    float severity = clampf(impact_speed / 260.0f, 0.8f, 2.4f);
    uint32_t spin_seed = ((uint32_t)player_id << 24) ^
                         ((uint32_t)respawn_station << 16) ^
                         ((uint32_t)asteroids_fractured << 1) ^
                         client_death_spin_float_bits(impact_speed);
    float spin_dir = client_death_spin_dir(spin_seed);
    g.death_ore_mined = ore_mined;
    g.death_credits_earned = credits_earned;
    g.death_credits_spent = credits_spent;
    g.death_asteroids_fractured = asteroids_fractured;
    g.death_respawn_station = respawn_station;
    g.death_respawn_fee = respawn_fee;
    /* Fire the cinematic at the death position. */
    g.death_cinematic.active = true;
    g.death_cinematic.phase = 0;
    g.death_cinematic.pos = v2(pos_x, pos_y);
    g.death_cinematic.vel = v2(vel_x, vel_y);
    g.death_cinematic.angle = angle;
    g.death_cinematic.spin = spin_dir * clampf(3.0f + impact_speed / 45.0f, 5.0f, 16.0f);
    g.death_cinematic.age = 0.0f;
    g.death_cinematic.menu_alpha = 0.0f;
    g.thrusting = false;
    LOCAL_PLAYER.beam_active = false;
    LOCAL_PLAYER.beam_hit = false;
    LOCAL_PLAYER.ship->tractor_active = false;
    g.screen_shake = fmaxf(g.screen_shake, clampf(26.0f + impact_speed * 0.12f, 38.0f, 82.0f));
    for (int i = 0; i < 8; i++) {
        float ang = ((float)i / 8.0f) * 2.0f * PI_F + (float)(i * 13 % 7) * 0.15f;
        float speed = 82.0f + severity * 34.0f + (float)((i * 7 + 3) % 5) * 22.0f;
        g.death_cinematic.fragments[i][0] = 0.0f;
        g.death_cinematic.fragments[i][1] = 0.0f;
        g.death_cinematic.fragments[i][2] = cosf(ang) * speed + vel_x * 0.45f;
        g.death_cinematic.fragments[i][3] = sinf(ang) * speed + vel_y * 0.45f;
        g.death_cinematic.fragments[i][4] = ang;
        g.death_cinematic.fragments[i][5] = ((float)((i * 19 + 7) % 11) - 5.0f) *
                                            (1.0f + severity * 0.45f);
    }
    /* Suppress the legacy detector path */
    g.death_screen_timer = 0.0f;
    g.death_screen_max = 0.0f;
    memset(g.episode.watched, 0, sizeof(g.episode.watched));
    g.episode.stations_visited = 0;
    episode_trigger(&g.episode, 9);
    episode_save(&g.episode);
    music_enter_death(&g.music);
}

void on_remote_world_time(float server_time) {
    float delta = server_time - g.world.time;
    if (fabsf(delta) > 2.0f) {
        g.world.time = server_time;
    } else {
        g.world.time += delta * 0.10f;
    }
}
