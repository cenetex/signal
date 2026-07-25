/*
 * local_server.c -- In-process authoritative simulation for singleplayer.
 *
 * Singleplayer uses the normal networking stack against this in-process
 * authority. Client-to-server packets enter through net.c's loopback
 * transport, and server-to-client state is serialized with the same protocol
 * packets that the dedicated server sends. Keep new behavior on that packet
 * path; direct copies into g.world are legacy architecture.
 */
#include "local_server.h"
#include "client.h"
#include "manifest.h"
#include "mining_client.h"
#include "net.h"
#include "net_protocol.h"
#include "cargo_receipt_issue.h"
#include "sim_ai.h"
#include "sim_asteroid.h"
#include "station_util.h"

#include <string.h>

static void local_server_emit_frame(local_server_t *ls, int player_slot);

#define LOCAL_SERVER_PLAYER_TICKS 6u       /* 20 Hz at 120 Hz sim */
#define LOCAL_SERVER_WORLD_TICKS 12u       /* 10 Hz at 120 Hz sim */
#define LOCAL_SERVER_PRIVATE_TICKS 30u     /* 4 Hz at 120 Hz sim */
#define LOCAL_SERVER_STATION_DIAG_TICKS 36u
#define LOCAL_SERVER_STATION_ECON_TICKS 120u
#define LOCAL_SERVER_STATION_IDENTITY_TICKS 1200u
#define LOCAL_SERVER_GLOBAL_TICKS 30u

void local_server_init(local_server_t *ls, uint32_t seed) {
    memset(ls, 0, sizeof(*ls));
    ls->world.rng = seed ? seed : 2037u;
    world_reset(&ls->world);
    /* Mirror the dedicated-server load path: turn the seeded float
     * inventory into manifest units so the manifest-only TRADE picker
     * has rows to surface. Without this, a fresh singleplayer start
     * shows empty markets at every station. */
    world_seed_station_manifests(&ls->world);
    /* Singleplayer is always a fresh world at this layer (no save
     * load); seed the chain log genesis events so MOTDs are part of
     * the chain history just like on the dedicated server. */
    world_seed_station_chain_genesis(&ls->world);
    ls->world.players[0].connected = true;
    ls->world.players[0].id = 0;
    ls->world.players[0].session_ready = false;
    ls->world.players[0].grace_timer = 5.0f;
    player_init_ship(&ls->world.players[0], &ls->world);
    ls->active = true;
    ls->station_snapshot_dirty = true;
    ls->private_snapshot_dirty = true;
    ls->global_snapshot_dirty = true;
}

static uint8_t local_server_msg_buf[
    ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE
];
static server_world_snapshot_scratch_t local_server_world_snapshot_scratch;
static server_private_snapshot_scratch_t local_server_private_snapshot_scratch;
static server_station_snapshot_scratch_t local_server_station_snapshot_scratch;

static void local_server_send_to_client(const uint8_t *data, int len) {
    if (data && len > 0) net_loopback_receive(data, len);
}

static void local_server_send_packet(void *user, const uint8_t *data, int len) {
    (void)user;
    local_server_send_to_client(data, len);
}

static void local_server_send_action_result(uint16_t action_id,
                                            uint16_t input_seq,
                                            uint8_t status,
                                            uint8_t action,
                                            uint32_t server_tick) {
    uint8_t buf[NET_ACTION_RESULT_SIZE];
    int len = serialize_action_result(buf, action_id, input_seq, status,
                                      action, server_tick);
    local_server_send_to_client(buf, len);
}

static void local_server_receipt_chain_sink(
    void *user,
    const cargo_receipt_chain_t *chain) {
    (void)user;
    uint8_t buf[3 + CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE];
    int len = serialize_cargo_receipt_bundle(buf, chain);
    if (len > 0) local_server_send_to_client(buf, len);
}

static void local_server_handoff_ticket_sink(
    void *user,
    uint8_t status,
    uint8_t source_station,
    uint8_t dest_station,
    const handoff_ticket_t *ticket) {
    (void)user;
    uint8_t buf[4 + HANDOFF_TICKET_SIZE];
    int len = serialize_handoff_ticket(buf, status, source_station,
                                       dest_station, ticket);
    local_server_send_to_client(buf, len);
}

static void local_server_handoff_result_sink(
    void *user,
    uint8_t status,
    uint8_t reason,
    uint8_t dest_station,
    const uint8_t ticket_hash[32]) {
    (void)user;
    uint8_t buf[NET_HANDOFF_RESULT_SIZE];
    int len = serialize_handoff_result(buf, status, reason, dest_station,
                                       ticket_hash);
    local_server_send_to_client(buf, len);
}

static void local_server_handle_signed_action(local_server_t *ls, int pid,
                                              const uint8_t *data, int len) {
    uint8_t action_type = 0;
    uint64_t nonce = 0;
    const uint8_t *payload = NULL;
    uint16_t payload_len = 0;
    signed_action_result_t res = signed_action_verify(
        &ls->world, pid, data, len, &action_type, &nonce, &payload, &payload_len);
    if (res != SIGNED_ACTION_OK) return;
    ls->world.players[pid].last_signed_nonce = nonce;

    server_signed_action_dispatch_result_t dispatch_result;
    (void)server_dispatch_signed_action_payload(
        &ls->world, pid, action_type, payload, payload_len,
        local_server_receipt_chain_sink, NULL, &dispatch_result);
}

static void local_server_handle_input(local_server_t *ls, int pid,
                                      const uint8_t *data, int len) {
    server_input_dispatch_result_t result;
    if (!ls) return;
    uint32_t now_ms = (uint32_t)(ls->world.time * 1000.0f);
    if (!server_dispatch_input_message(&ls->world, pid, data, len,
                                       now_ms, &result)) {
        return;
    }
    server_player_t *sp = &ls->world.players[pid];
    if (result.ack_status == NET_ACTION_ACK_RECEIVED &&
        result.action_id != 0) {
        server_begin_pending_action_result(&ls->world, sp, result.action_id,
                                           result.input_seq, result.action);
    }
    server_merge_one_shot_input(&sp->input, &result.intent);
    if (result.ack_status != 0) {
        uint8_t ack[NET_ACTION_ACK_SIZE];
        int alen = serialize_action_ack(ack, result.action_id,
                                        result.input_seq,
                                        result.ack_status,
                                        result.action);
        local_server_send_to_client(ack, alen);
        if (result.force_authoritative_resync) {
            sp->replication->force_authoritative_resync = true;
            local_server_send_action_result(result.action_id,
                                            result.input_seq,
                                            NET_ACTION_RESULT_REJECTED,
                                            result.action,
                                            ls->world.tick);
        }
    }
}

static bool local_server_loopback_send(const uint8_t *data, int len, void *user) {
    local_server_t *ls = (local_server_t *)user;
    if (!ls || !ls->active || !data || len < 1) return false;
    int pid = 0;
    server_player_t *sp = &ls->world.players[pid];

    switch (data[0]) {
    case NET_MSG_LATENCY_PING:
        if (len >= NET_LATENCY_PING_SIZE) {
            uint8_t pong[NET_LATENCY_PONG_SIZE];
            uint32_t seq = read_u32_le(&data[1]);
            uint32_t client_sent = read_u32_le(&data[5]);
            uint32_t now_ms = (uint32_t)(ls->world.time * 1000.0f);
            int plen = serialize_latency_pong(pong, seq, client_sent, now_ms,
                                              now_ms, ls->world.tick);
            local_server_send_to_client(pong, plen);
        }
        break;
    case NET_MSG_CLIENT_METRICS:
        break;
    case NET_MSG_REGISTER_PUBKEY:
    {
        server_pubkey_register_result_t result;
        if (server_dispatch_register_pubkey_message(&ls->world, pid, data,
                                                    len, &result) &&
            result.same_pubkey &&
            server_finalize_pubkey_identity(&ls->world, pid)) {
            player_seed_credits(sp, &ls->world);
        }
        break;
    }
    case NET_MSG_PROVE_PUBKEY:
    {
        server_pubkey_proof_result_t result;
        if (server_dispatch_pubkey_proof_message(&ls->world, pid, data,
                                                 len, &result) &&
            result.verified) {
            (void)server_finalize_pubkey_identity(&ls->world, pid);
            player_seed_credits(sp, &ls->world);
        }
        break;
    }
    case NET_MSG_CLAIM_LEGACY_SAVE:
        break;
    case NET_MSG_SESSION:
    {
        server_session_message_t session;
        if (server_parse_session_message(data, len, &session) &&
            server_apply_session_message(&ls->world, pid, &session)) {
            player_seed_credits(sp, &ls->world);
        }
        break;
    }
    case NET_MSG_INPUT:
        local_server_handle_input(ls, pid, data, len);
        break;
    case NET_MSG_PLAN:
        (void)server_dispatch_legacy_plan_message(&ls->world, pid, data,
                                                  len, NULL);
        break;
    case NET_MSG_STATE:
    case NET_MSG_MINING_ACTION:
        break;
    case NET_MSG_BUY_INGOT:
        (void)server_dispatch_legacy_buy_ingot_message(
            &ls->world, pid, data, len, local_server_receipt_chain_sink,
            NULL, NULL);
        break;
    case NET_MSG_DELIVER_INGOT:
        (void)server_dispatch_legacy_deliver_ingot_message(
            &ls->world, pid, data, len, local_server_receipt_chain_sink,
            NULL, NULL);
        break;
    case NET_MSG_PRESENT_RECEIPT_CHAIN:
        (void)server_dispatch_receipt_presentation_message(
            &ls->world, pid, data, len, NULL);
        break;
    case NET_MSG_HANDOFF_REQUEST:
        (void)server_dispatch_handoff_request(&ls->world, pid, data, len,
                                              local_server_handoff_ticket_sink,
                                              NULL);
        break;
    case NET_MSG_HANDOFF_PRESENT:
        (void)server_dispatch_handoff_present(&ls->world, pid, data, len,
                                              local_server_handoff_result_sink,
                                              NULL);
        break;
    case NET_MSG_FRACTURE_CLAIM:
        (void)server_dispatch_fracture_claim_message(&ls->world, pid,
                                                     data, len, NULL);
        break;
    case NET_MSG_SIGNED_ACTION:
        local_server_handle_signed_action(ls, pid, data, len);
        break;
    default:
        break;
    }
    return true;
}

void local_server_attach_loopback(local_server_t *ls) {
    net_set_loopback_send(local_server_loopback_send, ls);
}

static void local_server_emit_hail_response(local_server_t *ls,
                                            const sim_event_t *ev) {
    if (!ls || !ev) return;
    uint8_t msg[NET_HAIL_RESPONSE_REASON_SIZE];
    int len = serialize_hail_response_for_world(msg, &ls->world, ev);
    if (len > 0) local_server_send_to_client(msg, len);
}

static void local_server_player_packet_sink(void *user, int player_slot,
                                            const uint8_t *data, int len) {
    (void)user;
    (void)player_slot;
    local_server_send_to_client(data, len);
}

static void local_server_emit_fracture_updates(local_server_t *ls,
                                               int player_slot) {
    if (!ls || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_emit_fracture_updates(
        &ls->world, player_slot, local_server_player_packet_sink, NULL);
}

typedef struct {
    local_server_t *ls;
    int player_slot;
} local_server_event_context_t;

static void local_server_event_hail_response(void *user,
                                             const sim_event_t *ev) {
    local_server_event_context_t *ctx =
        (local_server_event_context_t *)user;
    if (!ctx || !ctx->ls) return;
    local_server_emit_hail_response(ctx->ls, ev);
}

static void local_server_event_death(void *user, const sim_event_t *ev) {
    local_server_event_context_t *ctx =
        (local_server_event_context_t *)user;
    if (!ctx || !ctx->ls || !ev) return;
    if (ev->player_id != ctx->player_slot) return;
    uint8_t msg[NET_DEATH_MSG_SIZE];
    int len = serialize_death(msg, (uint8_t)ctx->player_slot, ev);
    local_server_send_to_client(msg, len);
}

static void local_server_event_player_state_change(void *user,
                                                   const sim_event_t *ev) {
    (void)ev;
    local_server_event_context_t *ctx =
        (local_server_event_context_t *)user;
    if (!ctx || !ctx->ls) return;
    ctx->ls->private_snapshot_dirty = true;
    ctx->ls->global_snapshot_dirty = true;
}

static void local_server_event_structure_dirty(void *user,
                                               const sim_event_t *ev) {
    (void)ev;
    local_server_event_context_t *ctx =
        (local_server_event_context_t *)user;
    if (!ctx || !ctx->ls) return;
    ctx->ls->station_snapshot_dirty = true;
    ctx->ls->global_snapshot_dirty = true;
}

static const server_sim_event_hooks_t local_server_event_hooks = {
    .player_state_change = local_server_event_player_state_change,
    .hail_response = local_server_event_hail_response,
    .death = local_server_event_death,
    .structure_dirty = local_server_event_structure_dirty,
};

static void local_server_emit_events(local_server_t *ls, int player_slot) {
    if (!ls || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    local_server_event_context_t ctx = {
        .ls = ls,
        .player_slot = player_slot,
    };
    for (int i = 0; i < ls->world.events.count; i++) {
        const sim_event_t *ev = &ls->world.events.events[i];
        server_process_sim_event_transport(
            ev, &local_server_event_hooks, &ctx);
    }
    uint8_t ebuf[2 + SIM_MAX_EVENTS * NET_EVENT_RECORD_SIZE];
    int elen = serialize_events(ebuf, &ls->world.events);
    if (elen > 2) local_server_send_to_client(ebuf, elen);
}

static void local_server_emit_pending_action_results(local_server_t *ls,
                                                     int player_slot,
                                                     const sim_events_t *events) {
    if (!ls) return;
    uint32_t server_tick = ls->world.tick;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!server_player_slot_in_emit_range(i, player_slot)) continue;
        server_player_t *sp = &ls->world.players[i];
        if (!sp->pending_action_result_valid) continue;
        uint8_t status = server_pending_action_result_status(&ls->world,
                                                             sp, events);
        sp->replication->force_authoritative_resync = true;
        if (sp->connected) {
            local_server_send_action_result(sp->pending_action_result_id,
                                            sp->pending_action_result_input_seq,
                                            status,
                                            sp->pending_action_result_action,
                                            server_tick);
        }
        sp->pending_action_result_valid = false;
    }
}

static void local_server_emit_station_snapshots(local_server_t *ls) {
    if (!ls) return;
    server_emit_station_snapshot(
        &ls->world, true, local_server_send_packet, NULL,
        &local_server_station_snapshot_scratch);
    for (int s = 0; s < MAX_STATIONS; s++)
        ls->world.stations[s].manifest_dirty = false;
    ls->station_snapshot_dirty = false;
}

static void local_server_emit_station_identity_snapshots(local_server_t *ls) {
    if (!ls) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &ls->world.stations[s];
        if (!station_exists(st)) continue;
        int id_len = serialize_station_identity(
            local_server_station_snapshot_scratch.station_identity, s, st);
        local_server_send_to_client(
            local_server_station_snapshot_scratch.station_identity, id_len);
    }
    int station_len = serialize_stations(
        local_server_station_snapshot_scratch.world_stations,
        ls->world.stations);
    local_server_send_to_client(
        local_server_station_snapshot_scratch.world_stations, station_len);
    ls->station_snapshot_dirty = false;
}

static void local_server_emit_station_diag_snapshots(local_server_t *ls) {
    if (!ls) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &ls->world.stations[s];
        if (!station_exists(st)) continue;
        int diag_len = serialize_station_diag(
            local_server_station_snapshot_scratch.station_diag, s, st);
        local_server_send_to_client(
            local_server_station_snapshot_scratch.station_diag, diag_len);
    }
}

static void local_server_emit_station_econ_snapshots(local_server_t *ls) {
    if (!ls) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &ls->world.stations[s];
        if (!station_exists(st) || !st->manifest_dirty) continue;
        int manifest_len = serialize_station_manifest(
            local_server_station_snapshot_scratch.station_manifest, s, st);
        local_server_send_to_client(
            local_server_station_snapshot_scratch.station_manifest,
            manifest_len);
        st->manifest_dirty = false;
    }
    int station_len = serialize_stations(
        local_server_station_snapshot_scratch.world_stations,
        ls->world.stations);
    local_server_send_to_client(
        local_server_station_snapshot_scratch.world_stations,
        station_len);
}

static void local_server_emit_world_snapshots(local_server_t *ls,
                                              int player_slot,
                                              bool include_player_states) {
    if (!ls || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_emit_world_snapshot_for_player(
        &ls->world, player_slot, include_player_states,
        local_server_send_packet, NULL,
        &local_server_world_snapshot_scratch);
    server_clear_asteroid_net_dirty(&ls->world);
}

static void local_server_emit_player_snapshots(local_server_t *ls,
                                               int player_slot) {
    if (!ls || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    int len = serialize_player_states_except_recipient(
        local_server_msg_buf, ls->world.players, player_slot,
        ls->world.tick);
    if (len > 2) local_server_send_to_client(local_server_msg_buf, len);
}

static void local_server_emit_private_snapshots(local_server_t *ls,
                                                int player_slot) {
    if (!ls || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &ls->world.players[player_slot];
    if (sp->replication->force_authoritative_resync)
        server_player_reset_authoritative_ack_state(sp);
    server_emit_private_snapshot_for_player(
        &ls->world, player_slot, ls->throttled_snapshots,
        local_server_send_packet, NULL,
        &local_server_private_snapshot_scratch);
    sp->replication->force_authoritative_resync = false;
    ls->private_snapshot_dirty = false;
}

static void local_server_emit_global_snapshots(local_server_t *ls) {
    if (!ls) return;
    uint8_t contract_buf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int contract_len = serialize_contracts(contract_buf, ls->world.contracts);
    local_server_send_to_client(contract_buf, contract_len);

    if (ls->world.signal_channel.count > 0) {
        size_t cap = (size_t)(3 + ls->world.signal_channel.count *
                              SIGNAL_CHANNEL_RECORD_SIZE);
        if (cap <= sizeof(local_server_msg_buf)) {
            int len = serialize_signal_channel(local_server_msg_buf,
                                               &ls->world.signal_channel);
            local_server_send_to_client(local_server_msg_buf, len);
        }
    }
    ls->global_snapshot_dirty = false;
}

static void local_server_emit_frame(local_server_t *ls, int player_slot) {
    if (!ls || !ls->active) return;
    local_server_emit_events(ls, player_slot);
    local_server_emit_pending_action_results(ls, player_slot,
                                             &ls->world.events);
    local_server_emit_fracture_updates(ls, player_slot);

    uint32_t tick = ls->world.tick;

    if (!ls->throttled_snapshots) {
        /* Diagnostic loopback mode: full-fidelity pose data every tick
         * (see the throttled_snapshots comment in local_server.h). Station
         * identity must be checked BEFORE emit_station_snapshots, which
         * clears station_snapshot_dirty. Diag/econ telemetry keeps its
         * cadence — it carries no motion. */
        if (ls->station_snapshot_dirty ||
            (tick % LOCAL_SERVER_STATION_IDENTITY_TICKS) == 0u) {
            local_server_emit_station_identity_snapshots(ls);
        }
        local_server_emit_player_snapshots(ls, player_slot);
        local_server_emit_world_snapshots(ls, player_slot, false);
        local_server_emit_private_snapshots(ls, player_slot);
        local_server_emit_station_snapshots(ls);
        local_server_emit_global_snapshots(ls);
        if ((tick % LOCAL_SERVER_STATION_DIAG_TICKS) == 0u)
            local_server_emit_station_diag_snapshots(ls);
        if ((tick % LOCAL_SERVER_STATION_ECON_TICKS) == 0u)
            local_server_emit_station_econ_snapshots(ls);
        return;
    }

    /* Default loopback: mirror the dedicated server's broadcast cadences
     * so packet work is bounded and singleplayer exercises the same
     * prediction/dead-reckoning path as multiplayer. */
    if ((tick % LOCAL_SERVER_PLAYER_TICKS) == 0u)
        local_server_emit_player_snapshots(ls, player_slot);
    if ((tick % LOCAL_SERVER_WORLD_TICKS) == 0u)
        local_server_emit_world_snapshots(ls, player_slot, false);
    if (ls->private_snapshot_dirty ||
        (tick % LOCAL_SERVER_PRIVATE_TICKS) == 0u ||
        ls->world.players[player_slot].replication->force_authoritative_resync) {
        local_server_emit_private_snapshots(ls, player_slot);
    }
    if (ls->station_snapshot_dirty ||
        (tick % LOCAL_SERVER_STATION_IDENTITY_TICKS) == 0u) {
        local_server_emit_station_identity_snapshots(ls);
    }
    if ((tick % LOCAL_SERVER_STATION_DIAG_TICKS) == 0u)
        local_server_emit_station_diag_snapshots(ls);
    if ((tick % LOCAL_SERVER_STATION_ECON_TICKS) == 0u)
        local_server_emit_station_econ_snapshots(ls);
    if (ls->global_snapshot_dirty ||
        (tick % LOCAL_SERVER_GLOBAL_TICKS) == 0u) {
        local_server_emit_global_snapshots(ls);
    }
}

void local_server_send_initial_snapshot(local_server_t *ls, int player_slot) {
    if (!ls || !ls->active) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    /* Advertise the cadence this loopback will actually run at — client
     * extrapolation/smoothing windows are derived from these values, so
     * claiming 50/100 ms streams while emitting per-tick (or vice versa)
     * mis-tunes the receiver. */
    int proto_len = ls->throttled_snapshots
        ? serialize_protocol_info(
              local_server_msg_buf, 8u, 50u, 100u, 250u, 300u, 10000u)
        : serialize_protocol_info(
              local_server_msg_buf, 8u, 8u, 8u, 8u, 300u, 10000u);
    if (proto_len > 0)
        local_server_send_to_client(local_server_msg_buf, proto_len);
    local_server_emit_station_snapshots(ls);
    local_server_emit_world_snapshots(ls, player_slot, true);
    local_server_emit_private_snapshots(ls, player_slot);
    local_server_emit_global_snapshots(ls);
}

void local_server_step_loopback(local_server_t *ls, int player_slot, float dt) {
    if (!ls || !ls->active) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    uint16_t input_ack_before =
        ls->world.players[player_slot].last_input_seq;
    world_sim_step(&ls->world, dt);
    server_pending_input_ack_t pending;
    server_pending_input_ack_reset(&pending);
    if (server_pending_input_ack_note(
            &pending, &ls->world.players[player_slot], input_ack_before,
            ls->world.tick)) {
        server_player_t *sp = &ls->world.players[player_slot];
        (void)server_emit_pending_input_ack_adaptive(
            &pending, sp, (uint8_t)player_slot,
            sp->replication->force_authoritative_resync,
            local_server_send_packet, NULL);
    }
    local_server_emit_frame(ls, player_slot);
}
