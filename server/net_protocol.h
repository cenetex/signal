/*
 * net_protocol.h -- Server-side serialization helpers for the Signal
 * Space Miner authoritative server.
 *
 * Protocol enums, message types, and record sizes live in
 * shared/net_protocol.h (single source of truth).
 */
#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include <math.h>
#include <string.h>

#include "game_sim.h"
#include "cargo_receipt.h"
#include "handoff_ticket.h"
#include "manifest.h"
#include "sim_ai.h"
#include "sim_nav.h"
#include "protocol.h"   /* shared/protocol.h — protocol enums & constants */

/* Forward declaration — defined below. */
static inline int serialize_signal_channel(uint8_t *buf, const signal_channel_t *ch);

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline void write_f32_le(uint8_t *buf, float v) {
    union { float f; uint32_t u; } conv;
    conv.f = v;
    buf[0] = (uint8_t)(conv.u);
    buf[1] = (uint8_t)(conv.u >> 8);
    buf[2] = (uint8_t)(conv.u >> 16);
    buf[3] = (uint8_t)(conv.u >> 24);
}

static inline void write_u16_le(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
}

static inline uint16_t read_u16_le(const uint8_t *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline void write_u32_le(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

static inline uint32_t read_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static inline void write_u64_le(uint8_t *buf, uint64_t v) {
    for (int i = 0; i < 8; i++)
        buf[i] = (uint8_t)(v >> (8 * i));
}

static inline uint64_t read_u64_le(const uint8_t *buf) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)buf[i]) << (8 * i);
    return v;
}

static inline float read_f32_le(const uint8_t *buf) {
    union { float f; uint32_t u; } conv;
    conv.u = (uint32_t)buf[0]
           | ((uint32_t)buf[1] << 8)
           | ((uint32_t)buf[2] << 16)
           | ((uint32_t)buf[3] << 24);
    return conv.f;
}

/* ------------------------------------------------------------------ */
/* Serialisation (server -> client)                                   */
/* ------------------------------------------------------------------ */

static inline int serialize_action_ack(uint8_t *buf, uint16_t action_id,
                                       uint16_t input_seq, uint8_t status,
                                       uint8_t action) {
    buf[0] = NET_MSG_ACTION_ACK;
    write_u16_le(&buf[1], action_id);
    write_u16_le(&buf[3], input_seq);
    buf[5] = status;
    buf[6] = action;
    return NET_ACTION_ACK_SIZE;
}

static inline int serialize_action_result(uint8_t *buf, uint16_t action_id,
                                          uint16_t input_seq, uint8_t status,
                                          uint8_t action,
                                          uint32_t server_tick) {
    buf[0] = NET_MSG_ACTION_RESULT;
    write_u16_le(&buf[1], action_id);
    write_u16_le(&buf[3], input_seq);
    buf[5] = status;
    buf[6] = action;
    write_u32_le(&buf[7], server_tick);
    return NET_ACTION_RESULT_SIZE;
}

static inline int serialize_input_applied(uint8_t *buf, uint16_t input_seq,
                                          uint32_t server_tick,
                                          uint32_t input_tick_ack) {
    buf[0] = NET_MSG_INPUT_APPLIED;
    write_u16_le(&buf[1], input_seq);
    write_u32_le(&buf[3], server_tick);
    write_u32_le(&buf[7], input_tick_ack);
    return NET_INPUT_APPLIED_SIZE;
}

static inline int serialize_cargo_receipt_bundle(
    uint8_t *buf,
    const cargo_receipt_chain_t *chain) {
    if (!buf || !chain || chain->len == 0 ||
        chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
        return 0;
    }
    buf[0] = NET_MSG_CARGO_RECEIPT_BUNDLE;
    write_u16_le(&buf[1], chain->len);
    for (uint8_t i = 0; i < chain->len; i++) {
        cargo_receipt_pack(&chain->links[i],
                           &buf[3 + i * CARGO_RECEIPT_SIZE]);
    }
    return 3 + chain->len * CARGO_RECEIPT_SIZE;
}

static inline int serialize_latency_pong(uint8_t *buf, uint32_t seq,
                                         uint32_t client_sent_ms,
                                         uint32_t server_recv_ms,
                                         uint32_t server_send_ms) {
    buf[0] = NET_MSG_LATENCY_PONG;
    write_u32_le(&buf[1], seq);
    write_u32_le(&buf[5], client_sent_ms);
    write_u32_le(&buf[9], server_recv_ms);
    write_u32_le(&buf[13], server_send_ms);
    return NET_LATENCY_PONG_SIZE;
}

static inline int serialize_death(uint8_t *buf, uint8_t player_id,
                                  const sim_event_t *ev) {
    if (!buf || !ev) return 0;
    buf[0] = NET_MSG_DEATH;
    buf[1] = player_id;
    write_f32_le(&buf[2],  ev->death.pos_x);
    write_f32_le(&buf[6],  ev->death.pos_y);
    write_f32_le(&buf[10], ev->death.vel_x);
    write_f32_le(&buf[14], ev->death.vel_y);
    write_f32_le(&buf[18], ev->death.angle);
    write_f32_le(&buf[22], ev->death.ore_mined);
    write_f32_le(&buf[26], ev->death.credits_earned);
    write_f32_le(&buf[30], ev->death.credits_spent);
    write_f32_le(&buf[34], (float)ev->death.asteroids_fractured);
    buf[38] = ev->death.respawn_station;
    write_f32_le(&buf[39], ev->death.respawn_fee);
    return NET_DEATH_MSG_SIZE;
}

static inline float server_fracture_signal_radius_for_world(
    const world_t *w,
    vec2 pos) {
    if (!w) return 0.0f;
    float radius = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_provides_signal(st)) continue;
        if (v2_dist_sq(pos, st->pos) <= st->signal_range * st->signal_range &&
            st->signal_range > radius)
            radius = st->signal_range;
    }
    return radius;
}

static inline bool server_fracture_player_in_range_for_world(
    const world_t *w,
    int player_id,
    int asteroid_idx) {
    if (!w || player_id < 0 || player_id >= MAX_PLAYERS ||
        asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return false;
    const server_player_t *sp = &w->players[player_id];
    if (!sp->connected || !sp->session_ready ||
        !w->asteroids[asteroid_idx].active) {
        return false;
    }
    float radius =
        server_fracture_signal_radius_for_world(w, w->asteroids[asteroid_idx].pos);
    if (radius <= 0.0f) return false;
    return v2_dist_sq(sp->ship.pos, w->asteroids[asteroid_idx].pos) <=
        radius * radius;
}

static inline int serialize_fracture_challenge_for_world(
    uint8_t *buf,
    const world_t *w,
    int asteroid_idx) {
    if (!buf || !w || asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return 0;
    const fracture_claim_state_t *state = &w->fracture_claims[asteroid_idx];
    buf[0] = NET_MSG_FRACTURE_CHALLENGE;
    write_u32_le(&buf[1], state->fracture_id);
    memcpy(&buf[5], w->asteroids[asteroid_idx].fracture_seed, 32);
    write_u32_le(&buf[37], state->deadline_ms);
    write_u16_le(&buf[41], state->burst_cap);
    return FRACTURE_CHALLENGE_SIZE;
}

static inline int serialize_fracture_resolved_for_world(
    uint8_t *buf,
    const world_t *w,
    int asteroid_idx) {
    if (!buf || !w || asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return 0;
    const fracture_claim_state_t *state = &w->fracture_claims[asteroid_idx];
    buf[0] = NET_MSG_FRACTURE_RESOLVED;
    write_u32_le(&buf[1], state->fracture_id);
    memcpy(&buf[5], w->asteroids[asteroid_idx].fragment_pub, 32);
    memcpy(&buf[37], state->best_player_pub, 32);
    buf[69] = state->best_grade;
    return FRACTURE_RESOLVED_SIZE;
}

static inline int serialize_pending_fracture_resolved(
    uint8_t *buf,
    const pending_resolve_t *pr) {
    if (!buf || !pr) return 0;
    buf[0] = NET_MSG_FRACTURE_RESOLVED;
    write_u32_le(&buf[1], pr->fracture_id);
    memcpy(&buf[5], pr->fragment_pub, 32);
    memcpy(&buf[37], pr->winner_pub, 32);
    buf[69] = pr->grade;
    return FRACTURE_RESOLVED_SIZE;
}

static inline int serialize_handoff_ticket(uint8_t *buf, uint8_t status,
                                           uint8_t source_station,
                                           uint8_t dest_station,
                                           const handoff_ticket_t *ticket) {
    buf[0] = NET_MSG_HANDOFF_TICKET;
    buf[1] = status;
    buf[2] = source_station;
    buf[3] = dest_station;
    handoff_ticket_pack(ticket, &buf[4]);
    return 4 + HANDOFF_TICKET_SIZE;
}

static inline int serialize_handoff_result(uint8_t *buf, uint8_t status,
                                           uint8_t reason,
                                           uint8_t dest_station,
                                           const uint8_t ticket_hash[32]) {
    buf[0] = NET_MSG_HANDOFF_RESULT;
    buf[1] = status;
    buf[2] = reason;
    buf[3] = dest_station;
    if (ticket_hash) memcpy(&buf[4], ticket_hash, 32);
    else memset(&buf[4], 0, 32);
    return NET_HANDOFF_RESULT_SIZE;
}

static inline void protocol_info_write_stream(uint8_t *p, uint8_t msg,
                                              uint8_t stream_class,
                                              uint16_t flags,
                                              uint16_t header_size,
                                              uint16_t record_size,
                                              uint16_t max_records,
                                              uint16_t cadence_ms) {
    p[0] = msg;
    p[1] = stream_class;
    write_u16_le(&p[2], flags);
    write_u16_le(&p[4], header_size);
    write_u16_le(&p[6], record_size);
    write_u16_le(&p[8], max_records);
    write_u16_le(&p[10], cadence_ms);
}

static inline int serialize_protocol_info(uint8_t *buf,
                                          uint16_t sim_tick_ms,
                                          uint16_t state_tick_ms,
                                          uint16_t world_tick_ms,
                                          uint16_t ship_tick_ms,
                                          uint16_t station_diag_min_ms,
                                          uint16_t station_identity_fallback_ms) {
    buf[0] = NET_MSG_PROTOCOL_INFO;
    write_u16_le(&buf[1], SIGNAL_PROTOCOL_VERSION);
    write_u32_le(&buf[3], SIGNAL_PROTOCOL_CAPABILITIES);
    buf[7] = 0;

    int count = 0;
#define ADD_PROTOCOL_STREAM(msg, klass, flags, header, record, max, cadence) do { \
        if (count >= PROTOCOL_INFO_STREAM_CAPACITY) return 0; \
        protocol_info_write_stream(&buf[PROTOCOL_INFO_HEADER_SIZE + \
                                   count * PROTOCOL_INFO_STREAM_RECORD_SIZE], \
                                   (uint8_t)(msg), (uint8_t)(klass), \
                                   (uint16_t)(flags), (uint16_t)(header), \
                                   (uint16_t)(record), (uint16_t)(max), \
                                   (uint16_t)(cadence)); \
        count++; \
    } while (0)

    ADD_PROTOCOL_STREAM(NET_MSG_SERVER_INFO, PROTOCOL_STREAM_CLASS_STATIC,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT,
                        1, 1, 11, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_LATENCY_PING, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_LATENCY_PING_SIZE, 0, 1, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_LATENCY_PONG, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_LATENCY_PONG_SIZE, 0, 1, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_CLIENT_METRICS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_CLIENT_METRICS_SIZE, 0, 1, 1000);
    ADD_PROTOCOL_STREAM(NET_MSG_INPUT, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_INPUT_MSG_SIZE, 0, 1,
                        NET_INPUT_ACTIVE_HEARTBEAT_MS);
    ADD_PROTOCOL_STREAM(NET_MSG_INPUT_APPLIED, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_INPUT_APPLIED_SIZE, 0, 1, sim_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_STATION_IDENTITY, PROTOCOL_STREAM_CLASS_STATIC,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        STATION_IDENTITY_SIZE, 0, 1,
                        station_identity_fallback_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_STATION_DIAG, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        3, 1, MAX_MODULES_PER_STATION, station_diag_min_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYERS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT,
                        2, PLAYER_RECORD_SIZE, MAX_PLAYERS, state_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPCS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, NPC_RECORD_SIZE, MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_PODS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, CARGO_POD_RECORD_SIZE, MAX_CARGO_PODS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_PLAYER_SHIP, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        PLAYER_SHIP_SIZE, 0, 1, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_STATIONS, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        2, STATION_RECORD_SIZE, MAX_STATIONS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_STATION_MANIFEST, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        STATION_MANIFEST_HEADER, STATION_MANIFEST_ENTRY,
                        COMMODITY_COUNT * MINING_GRADE_COUNT, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_PLAYER_MANIFEST, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        PLAYER_MANIFEST_HEADER, PLAYER_MANIFEST_ENTRY,
                        COMMODITY_COUNT * MINING_GRADE_COUNT, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_STATION_INGOTS, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        STATION_INGOTS_HEADER, NAMED_INGOT_RECORD_SIZE,
                        255, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_HOLD_INGOTS, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        HOLD_INGOTS_HEADER, NAMED_INGOT_RECORD_SIZE,
                        255, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_CONTRACTS, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        2, CONTRACT_RECORD_SIZE, MAX_CONTRACTS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_DELIVERY_LEDGER, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        DELIVERY_LEDGER_HEADER, DELIVERY_LEDGER_RECORD_SIZE,
                        DELIVERY_LEDGER_MAX_RECORDS, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_INSPECT_SNAPSHOT, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        INSPECT_SNAPSHOT_HEADER, INSPECT_SNAPSHOT_ROW,
                        INSPECT_SNAPSHOT_MAX_ROWS, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_CARGO_RECEIPT_BUNDLE, PROTOCOL_STREAM_CLASS_AUTH,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT,
                        3, CARGO_RECEIPT_SIZE, CARGO_RECEIPT_CHAIN_MAX_LEN, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_PRESENT_RECEIPT_CHAIN, PROTOCOL_STREAM_CLASS_AUTH,
                        PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER,
                        35, CARGO_RECEIPT_SIZE, CARGO_RECEIPT_CHAIN_MAX_LEN, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_HANDOFF_REQUEST, PROTOCOL_STREAM_CLASS_AUTH,
                        PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_HANDOFF_REQUEST_SIZE, 0, 1, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_HANDOFF_TICKET, PROTOCOL_STREAM_CLASS_AUTH,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        4 + HANDOFF_TICKET_SIZE, 0, 1, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_HANDOFF_PRESENT, PROTOCOL_STREAM_CLASS_AUTH,
                        PROTOCOL_STREAM_FLAG_CLIENT_TO_SERVER,
                        1 + HANDOFF_TICKET_SIZE + 4 +
                        HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE,
                        HANDOFF_CARGO_UNIT_WIRE_SIZE + 1 +
                        CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE,
                        HANDOFF_SHIP_SNAPSHOT_MAX_CARGO, 0);
    ADD_PROTOCOL_STREAM(NET_MSG_HANDOFF_RESULT, PROTOCOL_STREAM_CLASS_AUTH,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        NET_HANDOFF_RESULT_SIZE, 0, 1, 0);

#undef ADD_PROTOCOL_STREAM
    buf[7] = (uint8_t)count;
    return PROTOCOL_INFO_HEADER_SIZE + count * PROTOCOL_INFO_STREAM_RECORD_SIZE;
}

/*
 * STATE message (45 bytes):
 * [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:20]
 * towed_frags = 10 × uint16_t (little-endian), 0xFFFF = unused. Widened
 * from uint8_t so slots 255-2047 aren't silently dropped (#285 Phase 3).
 */
static inline int serialize_player_state(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    buf[0] = NET_MSG_STATE;
    buf[1] = id;
    write_f32_le(&buf[2],  sp->ship.pos.x);
    write_f32_le(&buf[6],  sp->ship.pos.y);
    write_f32_le(&buf[10], sp->ship.vel.x);
    write_f32_le(&buf[14], sp->ship.vel.y);
    write_f32_le(&buf[18], sp->ship.angle);
    uint8_t flags = 0;
    if (sp->actual_thrusting) flags |= 1;
    if (sp->beam_active) flags |= 2;
    if (sp->docked) flags |= 4;
    if (sp->scan_active) flags |= 8;
    if (sp->ship.tractor_active) flags |= 16;
    if (sp->beam_ineffective) flags |= 32;
    if (sp->beam_hit) flags |= 64;
    buf[22] = flags;
    buf[23] = (uint8_t)sp->ship.tractor_level;
    buf[24] = sp->ship.towed_count;
    for (int t = 0; t < 10; t++) {
        int16_t fi = (t < sp->ship.towed_count) ? sp->ship.towed_fragments[t] : -1;
        uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        buf[25 + t * 2]     = (uint8_t)(wire & 0xFFu);
        buf[25 + t * 2 + 1] = (uint8_t)(wire >> 8);
    }
    return 45;
}

/*
 * WORLD_PLAYERS message (batched):
 * [type:1][count:1] + count * PLAYER_RECORD_SIZE records
 * Each record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1]
 *              [tractor_lvl:1][towed_count:1][towed_frags:20][callsign:7]
 *              [beam_start_x:f32][beam_start_y:f32][beam_end_x:f32][beam_end_y:f32]
 *              [last_input_seq:u16][server_tick:u32][last_input_tick:u32]
 */
/* PLAYER_RECORD_SIZE defined in shared/net_protocol.h */
static inline int serialize_all_player_states(uint8_t *buf, const server_player_t *players,
                                              uint32_t server_tick) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!server_player_is_gameplay_ready(&players[i])) continue;
        uint8_t *p = &buf[2 + count * PLAYER_RECORD_SIZE];
        p[0] = (uint8_t)i;
        write_f32_le(&p[1],  players[i].ship.pos.x);
        write_f32_le(&p[5],  players[i].ship.pos.y);
        write_f32_le(&p[9],  players[i].ship.vel.x);
        write_f32_le(&p[13], players[i].ship.vel.y);
        write_f32_le(&p[17], players[i].ship.angle);
        uint8_t flags = 0;
        if (players[i].actual_thrusting) flags |= 1;
        /* bit 1 was "beam_active && beam_hit" — now just "beam_active" so
         * the client can render the beam even when it's firing into empty
         * space (no rock target). beam_hit is implied by the beam_end
         * coords matching a target. */
        if (players[i].beam_active) flags |= 2;
        if (players[i].docked) flags |= 4;
        if (players[i].scan_active) flags |= 8;
        if (players[i].ship.tractor_active) flags |= 16;
        if (players[i].beam_ineffective) flags |= 32;
        if (players[i].beam_hit) flags |= 64;
        p[21] = flags;
        p[22] = (uint8_t)players[i].ship.tractor_level;
        p[23] = players[i].ship.towed_count;
        for (int t = 0; t < 10; t++) {
            int16_t fi = (t < players[i].ship.towed_count) ? players[i].ship.towed_fragments[t] : -1;
            uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
            p[24 + t * 2]     = (uint8_t)(wire & 0xFFu);
            p[24 + t * 2 + 1] = (uint8_t)(wire >> 8);
        }
        /* Callsign: 7 bytes (e.g. "KRX-472") */
        memcpy(&p[44], players[i].callsign, 7);
        /* Beam coordinates — server-authoritative. The client mirrors
         * these into LOCAL_PLAYER.beam_start/end so the autopilot's
         * laser visual works (and so combat hits are deterministic
         * once we have weapons). */
        write_f32_le(&p[51], players[i].beam_start.x);
        write_f32_le(&p[55], players[i].beam_start.y);
        write_f32_le(&p[59], players[i].beam_end.x);
        write_f32_le(&p[63], players[i].beam_end.y);
        write_u16_le(&p[67], players[i].last_input_seq);
        write_u32_le(&p[69], server_tick);
        write_u32_le(&p[73], players[i].last_input_tick);
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYERS;
    buf[1] = (uint8_t)count;
    return 2 + count * PLAYER_RECORD_SIZE;
}

/*
 * WORLD_ASTEROIDS message (v2 — uint16 indices):
 * [type:1][count:2] + count * ASTEROID_RECORD_SIZE-byte records
 * Record: [index:2][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32]
 * [radius:f32][smelt:u8][grade:u8][crystal_stage:u8][phase:u8]
 */
#define ASTEROID_MSG_HEADER 3  /* type + uint16 count */

/* Per-player asteroid serialization with relevance filtering. */
#define ASTEROID_VIEW_RADIUS_SQ (3000.0f * 3000.0f)
static inline int serialize_asteroids_for_player(
    uint8_t *buf, const asteroid_t *asteroids, vec2 player_pos, bool *sent) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &asteroids[i];
        bool in_view = a->active &&
            v2_dist_sq(a->pos, player_pos) < ASTEROID_VIEW_RADIUS_SQ;

        if (in_view) {
            uint8_t *p = &buf[ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
            write_u16_le(&p[0], (uint16_t)i);
            p[2] = 1;
            if (a->fracture_child) p[2] |= (1 << 1);
            p[2] |= (((uint8_t)a->tier & 0x7) << 2);
            p[2] |= (((uint8_t)a->commodity & 0x7) << 5);
            write_f32_le(&p[3],  a->pos.x);
            write_f32_le(&p[7],  a->pos.y);
            write_f32_le(&p[11], a->vel.x);
            write_f32_le(&p[15], a->vel.y);
            write_f32_le(&p[19], a->hp);
            write_f32_le(&p[23], a->ore);
            write_f32_le(&p[27], a->radius);
            /* smelt_progress: 0.0-1.0 quantized to uint8 so the client
             * furnace-glow + laser-beam visuals fire in multiplayer. */
            {
                float sp_f = a->smelt_progress;
                if (sp_f < 0.0f) sp_f = 0.0f;
                if (sp_f > 1.0f) sp_f = 1.0f;
                p[31] = (uint8_t)(sp_f * 255.0f);
            }
            p[32] = a->grade;
            p[33] = a->crystal_stage;
            p[34] = a->phase;
            sent[i] = true;
            count++;
        } else if (sent[i] && !in_view) {
            uint8_t *p = &buf[ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
            memset(p, 0, ASTEROID_RECORD_SIZE);
            write_u16_le(&p[0], (uint16_t)i);
            p[2] = 0; /* active = false */
            sent[i] = false;
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    write_u16_le(&buf[1], (uint16_t)count);
    return ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE;
}

/* Serialize active asteroid slots for a new-player snapshot. Inactive
 * slots are omitted because MAX_ASTEROIDS is large; clients entering
 * remote-authoritative mode must clear any locally seeded asteroid state
 * before applying this active-only snapshot. Does not clear dirty flags. */
static inline int serialize_asteroids_full(uint8_t *buf, const asteroid_t *asteroids) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &asteroids[i];
        if (!a->active) continue; /* skip inactive for full snapshot — too many slots at 2048 */
        uint8_t *p = &buf[ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
        memset(p, 0, ASTEROID_RECORD_SIZE);
        write_u16_le(&p[0], (uint16_t)i);
        p[2] = 1;
        if (a->fracture_child) p[2] |= (1 << 1);
        p[2] |= (((uint8_t)a->tier & 0x7) << 2);
        p[2] |= (((uint8_t)a->commodity & 0x7) << 5);
        write_f32_le(&p[3],  a->pos.x);
        write_f32_le(&p[7],  a->pos.y);
        write_f32_le(&p[11], a->vel.x);
        write_f32_le(&p[15], a->vel.y);
        write_f32_le(&p[19], a->hp);
        write_f32_le(&p[23], a->ore);
        write_f32_le(&p[27], a->radius);
        {
            float sp_f = a->smelt_progress;
            if (sp_f < 0.0f) sp_f = 0.0f;
            if (sp_f > 1.0f) sp_f = 1.0f;
            p[31] = (uint8_t)(sp_f * 255.0f);
        }
        p[32] = a->grade;
        p[33] = a->crystal_stage;
        p[34] = a->phase;
        count++;
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    write_u16_le(&buf[1], (uint16_t)count);
    return ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE;
}

/* RATi v2 — write a single named-ingot wire record from a cargo_unit_t.
 * Layout matches the on-wire NAMED_INGOT_RECORD_SIZE definition. The
 * record size is unchanged after the named_ingot_t -> cargo_unit_t
 * unification; p[34] was formerly padding and now carries grade so
 * clients can attach provenance to the correct market row. */
static inline void write_named_ingot_unit(uint8_t *p, const cargo_unit_t *u) {
    memset(p, 0, NAMED_INGOT_RECORD_SIZE);
    memcpy(&p[0], u->pub, 32);
    p[32] = u->prefix_class;
    p[33] = u->commodity;
    p[34] = u->grade;
    /* p[35] pad */
    for (int k = 0; k < 8; k++) p[36 + k] = (uint8_t)(u->mined_block >> (8 * k));
    p[44] = u->origin_station;
    /* p[45..51] pad */
}

/* Per-station named-ingot snapshot. Walks the station manifest and
 * surfaces every CARGO_KIND_INGOT unit whose prefix is non-anonymous
 * (the rest are bulk fungibles already covered by the manifest summary).
 * Cap at 255 (wire count is u8). */
static inline int serialize_station_ingots(uint8_t *buf, int station_idx,
                                           const station_t *st) {
    int n = 0;
    buf[0] = NET_MSG_STATION_INGOTS;
    buf[1] = (uint8_t)station_idx;
    if (st->manifest.units) {
        for (uint16_t i = 0; i < st->manifest.count && n < 255; i++) {
            const cargo_unit_t *u = &st->manifest.units[i];
            if ((cargo_kind_t)u->kind != CARGO_KIND_INGOT) continue;
            if ((ingot_prefix_t)u->prefix_class == INGOT_PREFIX_ANONYMOUS) continue;
            uint8_t *p = &buf[STATION_INGOTS_HEADER + n * NAMED_INGOT_RECORD_SIZE];
            write_named_ingot_unit(p, u);
            n++;
        }
    }
    buf[2] = (uint8_t)n;
    return STATION_INGOTS_HEADER + n * NAMED_INGOT_RECORD_SIZE;
}

/* Per-station manifest summary (Phase 2). Runs through every unit in
 * station.manifest, builds a per-(commodity, grade) count table, and
 * serializes only the non-zero slots.
 * Wire layout is defined in shared/protocol.h (STATION_MANIFEST_HEADER /
 * STATION_MANIFEST_ENTRY) so the client can decode. Sent per-station on
 * manifest change (cheapest) or on dock. */
static inline int serialize_station_manifest(uint8_t *buf, int station_idx,
                                             const station_t *st) {
    /* Build the (commodity × grade) count table from the manifest. */
    uint16_t table[COMMODITY_COUNT][MINING_GRADE_COUNT];
    memset(table, 0, sizeof(table));
    if (st->manifest.units) {
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            const cargo_unit_t *u = &st->manifest.units[i];
            if (u->commodity >= COMMODITY_COUNT) continue;
            if (u->grade >= MINING_GRADE_COUNT) continue;
            if (table[u->commodity][u->grade] < 0xFFFF)
                table[u->commodity][u->grade]++;
        }
    }
    buf[0] = NET_MSG_STATION_MANIFEST;
    buf[1] = (uint8_t)station_idx;
    int n = 0;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        for (int g = 0; g < MINING_GRADE_COUNT; g++) {
            uint16_t count = table[c][g];
            if (count == 0) continue;
            uint8_t *p = &buf[STATION_MANIFEST_HEADER + n * STATION_MANIFEST_ENTRY];
            p[0] = (uint8_t)c;
            p[1] = (uint8_t)g;
            write_u16_le(&p[2], count);
            n++;
        }
    }
    write_u16_le(&buf[2], (uint16_t)n);
    return STATION_MANIFEST_HEADER + n * STATION_MANIFEST_ENTRY;
}

/* Local player's manifest summary. Same (commodity × grade) count
 * shape as the station summary, with no station_idx (the recipient is
 * the implicit local player). Sent each tick alongside PLAYER_SHIP so
 * the trade UI's SELL rows reflect server-authoritative manifest state
 * (buy/sell/transfer mutations on the server side reach the client
 * without being lost). */
static inline int serialize_player_manifest(uint8_t *buf, const ship_t *ship) {
    uint16_t table[COMMODITY_COUNT][MINING_GRADE_COUNT];
    memset(table, 0, sizeof(table));
    if (ship && ship->manifest.units) {
        for (uint16_t i = 0; i < ship->manifest.count; i++) {
            const cargo_unit_t *u = &ship->manifest.units[i];
            if (u->commodity >= COMMODITY_COUNT) continue;
            if (u->grade >= MINING_GRADE_COUNT) continue;
            if (table[u->commodity][u->grade] < 0xFFFF)
                table[u->commodity][u->grade]++;
        }
    }
    buf[0] = NET_MSG_PLAYER_MANIFEST;
    int n = 0;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        for (int g = 0; g < MINING_GRADE_COUNT; g++) {
            uint16_t count = table[c][g];
            if (count == 0) continue;
            uint8_t *p = &buf[PLAYER_MANIFEST_HEADER + n * PLAYER_MANIFEST_ENTRY];
            p[0] = (uint8_t)c;
            p[1] = (uint8_t)g;
            write_u16_le(&p[2], count);
            n++;
        }
    }
    write_u16_le(&buf[1], (uint16_t)n);
    return PLAYER_MANIFEST_HEADER + n * PLAYER_MANIFEST_ENTRY;
}

/* Local player's hold-ingot snapshot, derived from the ship manifest.
 * Walks ship.manifest for non-anonymous CARGO_KIND_INGOT units. Cap at
 * 255 (wire count is u8). */
static inline int serialize_hold_ingots(uint8_t *buf, const ship_t *ship) {
    int n = 0;
    buf[0] = NET_MSG_HOLD_INGOTS;
    if (ship && ship->manifest.units) {
        for (uint16_t i = 0; i < ship->manifest.count && n < 255; i++) {
            const cargo_unit_t *u = &ship->manifest.units[i];
            if ((cargo_kind_t)u->kind != CARGO_KIND_INGOT) continue;
            if ((ingot_prefix_t)u->prefix_class == INGOT_PREFIX_ANONYMOUS) continue;
            uint8_t *p = &buf[HOLD_INGOTS_HEADER + n * NAMED_INGOT_RECORD_SIZE];
            write_named_ingot_unit(p, u);
            n++;
        }
    }
    buf[1] = (uint8_t)n;
    return HOLD_INGOTS_HEADER + n * NAMED_INGOT_RECORD_SIZE;
}

/* Laser/scan inspection snapshot. The target-only helper is used for
 * station/player scans so clients can still mirror authoritative scan
 * metadata even when there is no manifest to project. */
static inline int serialize_inspect_snapshot_target(uint8_t *buf,
                                                    int target_type,
                                                    int target_index,
                                                    int module_index) {
    memset(buf, 0, INSPECT_SNAPSHOT_HEADER);
    buf[0] = NET_MSG_INSPECT_SNAPSHOT;
    buf[1] = (target_type > 0) ? (uint8_t)target_type : (uint8_t)INSPECT_TARGET_NONE;
    buf[2] = (target_index >= 0) ? (uint8_t)target_index : 0xFFu;
    buf[3] = (module_index >= 0) ? (uint8_t)module_index : 0xFFu;
    buf[6] = 0xFFu; /* home_station */
    buf[7] = 0xFFu; /* dest_station */
    write_u16_le(&buf[9], 0);
    return INSPECT_SNAPSHOT_HEADER;
}

static inline void write_inspect_snapshot_row(uint8_t *p,
                                              const cargo_unit_t *u,
                                              uint8_t commodity,
                                              uint8_t grade,
                                              uint16_t quantity,
                                              const cargo_receipt_chain_t *chain,
                                              bool grouped,
                                              uint8_t group_prefix_class) {
    memset(p, 0, INSPECT_SNAPSHOT_ROW);
    p[0] = commodity;
    p[1] = grade;
    if (quantity == 0) quantity = 1;
    write_u16_le(&p[12], quantity);

    if (grouped) {
        p[2] = group_prefix_class;  /* repurposed: prefix_class on grouped rows (0 = ANONYMOUS bulk) */
        p[3] |= INSPECT_ROW_GROUPED;
        return;
    }

    if (u) memcpy(&p[14], u->pub, 32);

    if (chain && chain->len > 0) {
        const cargo_receipt_t *origin = &chain->links[0];
        const cargo_receipt_t *latest = &chain->links[chain->len - 1];
        p[2] = chain->len;
        p[3] |= INSPECT_ROW_HAS_RECEIPT;
        write_u64_le(&p[4], latest->event_id);
        cargo_receipt_hash(latest, &p[46]);
        memcpy(&p[78], origin->authoring_station, 32);
        memcpy(&p[110], latest->authoring_station, 32);
    }
}

static inline uint64_t inspect_snapshot_market_meta(const market_memory_t *memory) {
    if (!memory) return 0;
    return (uint64_t)memory->station_a
         | ((uint64_t)memory->station_b << 8)
         | ((uint64_t)memory->action << 16)
         | ((uint64_t)memory->commodity << 24);
}

static inline bool inspect_snapshot_market_memory_from_item(const knowledge_item_t *item,
                                                            market_memory_t *out) {
    if (!item || !out) return false;
    if (item->kind != (uint8_t)KNOW_MARKET) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY) return false;
    market_memory_t memory;
    memset(&memory, 0, sizeof(memory));
    memcpy(&memory, item->payload, sizeof(memory));
    if (!memory.active) return false;
    if (memory.memory_kind == (uint8_t)MARKET_MEMORY_NONE) return false;
    memory.confidence = item->confidence;
    memory.salience = item->salience;
    *out = memory;
    return true;
}

static inline uint8_t inspect_snapshot_diag_kind_from_market(uint8_t memory_kind) {
    switch ((market_memory_kind_t)memory_kind) {
    case MARKET_MEMORY_DEMAND:           return (uint8_t)INSPECT_DIAG_MARKET_DEMAND;
    case MARKET_MEMORY_SUPPLY:           return (uint8_t)INSPECT_DIAG_MARKET_SUPPLY;
    case MARKET_MEMORY_ROUTE_DANGER:     return (uint8_t)INSPECT_DIAG_ROUTE_DANGER;
    case MARKET_MEMORY_ROUTE_SUCCESS:    return (uint8_t)INSPECT_DIAG_ROUTE_SUCCESS;
    case MARKET_MEMORY_DELIVERY_RECEIPT: return (uint8_t)INSPECT_DIAG_DELIVERY_RECEIPT;
    case MARKET_MEMORY_ROUTE_REPUTATION: return (uint8_t)INSPECT_DIAG_ROUTE_REPUTATION;
    case MARKET_MEMORY_ROUTE_RISK:       return (uint8_t)INSPECT_DIAG_ROUTE_RISK;
    case MARKET_MEMORY_STATION_TRUST:    return (uint8_t)INSPECT_DIAG_STATION_TRUST;
    case MARKET_MEMORY_STATION_RISK:     return (uint8_t)INSPECT_DIAG_STATION_RISK;
    case MARKET_MEMORY_NONE:
    default:                             return (uint8_t)INSPECT_DIAG_NONE;
    }
}

static inline void write_inspect_snapshot_market_diag_row(uint8_t *p,
                                                          const market_memory_t *memory,
                                                          const knowledge_item_t *item) {
    memset(p, 0, INSPECT_SNAPSHOT_ROW);
    if (!memory) return;
    p[0] = inspect_snapshot_diag_kind_from_market(memory->memory_kind);
    p[1] = memory->confidence;
    p[2] = memory->salience;
    p[3] = INSPECT_ROW_DIAGNOSTIC;
    write_u64_le(&p[4], inspect_snapshot_market_meta(memory));
    uint16_t hint = memory->value_hint ? memory->value_hint : memory->quantity_hint;
    write_u16_le(&p[12], hint);
    if (item) {
        memcpy(&p[14], item->subject_hash, 32);
        memcpy(&p[46], item->chain_anchor, 32);
        memcpy(&p[78], item->source_hash, 32);
        memcpy(&p[110], item->witness_hash, 32);
    } else {
        write_u64_le(&p[14], memory->subject_nonce);
        write_u32_le(&p[22], memory->observed_tick);
    }
}

static inline bool inspect_hash32_nonzero(const uint8_t hash[32]) {
    if (!hash) return false;
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return true;
    }
    return false;
}

static inline bool inspect_hash32_equal(const uint8_t a[32],
                                        const uint8_t b[32]) {
    if (!inspect_hash32_nonzero(a) || !inspect_hash32_nonzero(b))
        return false;
    return memcmp(a, b, 32) == 0;
}

static inline bool inspect_snapshot_item_matches_job_proof(
    const knowledge_item_t *item,
    const npc_ship_t *npc,
    int job_idx) {
    if (!item || !npc || job_idx < 0 || job_idx >= npc->job_diag_count)
        return false;
    const uint8_t *proof = npc->job_diag_proof_hash[job_idx];
    if (!inspect_hash32_nonzero(proof)) return false;
    return inspect_hash32_equal(proof, item->chain_anchor) ||
           inspect_hash32_equal(proof, item->witness_hash) ||
           inspect_hash32_equal(proof, item->subject_hash);
}

static inline int write_inspect_snapshot_job_source_rows(
    uint8_t *buf,
    int row_count,
    int max_rows,
    const knowledge_view_t *knowledge,
    const npc_ship_t *npc,
    uint8_t emitted[KNOWLEDGE_VIEW_MAX_CAP]) {
    if (!buf || !knowledge || !npc || !emitted || row_count >= max_rows)
        return row_count;
    int item_cap = knowledge->count;
    if (item_cap > KNOWLEDGE_VIEW_MAX_CAP) item_cap = KNOWLEDGE_VIEW_MAX_CAP;
    int job_count = npc->job_diag_count;
    int job_cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
    if (job_count > job_cap) job_count = job_cap;
    for (int j = 0; j < job_count && row_count < max_rows; j++) {
        if (npc->job_diag_kind[j] == (uint8_t)INSPECT_DIAG_NONE) continue;
        for (int i = 0; i < item_cap && row_count < max_rows; i++) {
            if (emitted[i]) continue;
            market_memory_t memory;
            if (!inspect_snapshot_market_memory_from_item(&knowledge->items[i],
                                                          &memory)) {
                continue;
            }
            if (!inspect_snapshot_item_matches_job_proof(&knowledge->items[i],
                                                         npc, j)) {
                continue;
            }
            uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER +
                              row_count * INSPECT_SNAPSHOT_ROW];
            write_inspect_snapshot_market_diag_row(p, &memory,
                                                   &knowledge->items[i]);
            emitted[i] = 1;
            row_count++;
            break;
        }
    }
    return row_count;
}

static inline bool inspect_snapshot_chain_matches_job_proof(
    const cargo_receipt_chain_t *chain,
    const npc_ship_t *npc) {
    if (!chain || chain->len == 0 || chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return false;
    if (!npc) return false;
    uint8_t head[32];
    cargo_receipt_hash(&chain->links[chain->len - 1], head);
    int job_count = npc->job_diag_count;
    int job_cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
    if (job_count > job_cap) job_count = job_cap;
    for (int i = 0; i < job_count; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_NONE) continue;
        if (inspect_hash32_equal(head, npc->job_diag_proof_hash[i]))
            return true;
    }
    return false;
}

static inline void write_inspect_snapshot_receipt_link_row(
    uint8_t *p,
    const cargo_receipt_t *receipt,
    uint8_t link_idx,
    uint8_t link_count) {
    memset(p, 0, INSPECT_SNAPSHOT_ROW);
    if (!receipt) return;
    p[0] = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    p[1] = link_idx;
    p[2] = link_count;
    p[3] = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
    write_u64_le(&p[4], receipt->event_id);
    write_u16_le(&p[12], link_idx);
    memcpy(&p[14], receipt->cargo_pub, 32);
    cargo_receipt_hash(receipt, &p[46]);
    memcpy(&p[78], receipt->authoring_station, 32);
    memcpy(&p[110], receipt->recipient_pubkey, 32);
}

static inline int write_inspect_snapshot_matching_receipt_rows(
    uint8_t *buf,
    int row_count,
    int max_rows,
    const ship_t *ship,
    const ship_receipts_t *rcpts,
    const npc_ship_t *npc_diag) {
    if (!buf || !ship || !ship->manifest.units || !rcpts || !npc_diag)
        return row_count;
    uint16_t manifest_count = ship->manifest.count;
    for (uint16_t i = 0; i < manifest_count && row_count < max_rows; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (inspect_snapshot_unit_is_groupable(u)) continue;
        const cargo_receipt_chain_t *chain =
            (i < rcpts->count) ? &rcpts->chains[i] : NULL;
        if (!inspect_snapshot_chain_matches_job_proof(chain, npc_diag))
            continue;
        uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER +
                          row_count * INSPECT_SNAPSHOT_ROW];
        write_inspect_snapshot_row(p, u, u->commodity, u->grade, 1,
                                   chain, false,
                                   (uint8_t)INGOT_PREFIX_ANONYMOUS);
        row_count++;
        for (uint8_t link = 0; link < chain->len && row_count < max_rows; link++) {
            p = &buf[INSPECT_SNAPSHOT_HEADER +
                     row_count * INSPECT_SNAPSHOT_ROW];
            write_inspect_snapshot_receipt_link_row(
                p, &chain->links[link], (uint8_t)(link + 1), chain->len);
            row_count++;
        }
    }
    return row_count;
}

static inline int write_inspect_snapshot_matching_station_receipt_rows(
    uint8_t *buf,
    int row_count,
    int max_rows,
    const station_t *stations,
    int station_count,
    const npc_ship_t *npc_diag) {
    if (!buf || !stations || station_count <= 0 || !npc_diag)
        return row_count;
    for (int st = 0; st < station_count && row_count < max_rows; st++) {
        const station_t *station = &stations[st];
        if (!station->manifest.units) continue;
        const ship_receipts_t *rcpts = station_get_receipts_const(station);
        if (!rcpts) continue;
        uint16_t manifest_count = station->manifest.count;
        for (uint16_t i = 0; i < manifest_count && row_count < max_rows; i++) {
            const cargo_unit_t *u = &station->manifest.units[i];
            if (inspect_snapshot_unit_is_groupable(u)) continue;
            const cargo_receipt_chain_t *chain =
                (i < rcpts->count) ? &rcpts->chains[i] : NULL;
            if (!inspect_snapshot_chain_matches_job_proof(chain, npc_diag))
                continue;
            uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER +
                              row_count * INSPECT_SNAPSHOT_ROW];
            write_inspect_snapshot_row(p, u, u->commodity, u->grade, 1,
                                       chain, false,
                                       (uint8_t)INGOT_PREFIX_ANONYMOUS);
            p[3] |= INSPECT_ROW_STATION_RECEIPT;
            row_count++;
            for (uint8_t link = 0; link < chain->len && row_count < max_rows; link++) {
                p = &buf[INSPECT_SNAPSHOT_HEADER +
                         row_count * INSPECT_SNAPSHOT_ROW];
                write_inspect_snapshot_receipt_link_row(
                    p, &chain->links[link], (uint8_t)(link + 1), chain->len);
                row_count++;
            }
            return row_count;
        }
    }
    return row_count;
}

static inline int write_inspect_snapshot_matching_holder_receipt_rows(
    uint8_t *buf,
    int row_count,
    int max_rows,
    const ship_t *holder_ship,
    const npc_ship_t *npc_diag,
    uint8_t custody_flag) {
    if (!buf || !holder_ship || !holder_ship->manifest.units || !npc_diag)
        return row_count;
    const ship_receipts_t *rcpts = ship_get_receipts_const(holder_ship);
    if (!rcpts) return row_count;
    for (uint16_t i = 0; i < holder_ship->manifest.count &&
                         row_count < max_rows; i++) {
        const cargo_unit_t *u = &holder_ship->manifest.units[i];
        if (inspect_snapshot_unit_is_groupable(u)) continue;
        const cargo_receipt_chain_t *chain =
            (i < rcpts->count) ? &rcpts->chains[i] : NULL;
        if (!inspect_snapshot_chain_matches_job_proof(chain, npc_diag))
            continue;
        uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER +
                          row_count * INSPECT_SNAPSHOT_ROW];
        write_inspect_snapshot_row(p, u, u->commodity, u->grade, 1,
                                   chain, false,
                                   (uint8_t)INGOT_PREFIX_ANONYMOUS);
        p[3] |= custody_flag;
        row_count++;
        for (uint8_t link = 0; link < chain->len && row_count < max_rows; link++) {
            p = &buf[INSPECT_SNAPSHOT_HEADER +
                     row_count * INSPECT_SNAPSHOT_ROW];
            write_inspect_snapshot_receipt_link_row(
                p, &chain->links[link], (uint8_t)(link + 1), chain->len);
            row_count++;
        }
        return row_count;
    }
    return row_count;
}

static inline int write_inspect_snapshot_matching_relay_receipt_rows(
    uint8_t *buf,
    int row_count,
    int max_rows,
    const world_t *receipt_world,
    int exclude_npc_slot,
    const npc_ship_t *npc_diag) {
    if (!buf || !receipt_world || !npc_diag) return row_count;
    for (int pidx = 0; pidx < MAX_PLAYERS && row_count < max_rows; pidx++) {
        const server_player_t *sp = &receipt_world->players[pidx];
        if (!sp->connected) continue;
        row_count = write_inspect_snapshot_matching_holder_receipt_rows(
            buf, row_count, max_rows, &sp->ship, npc_diag,
            INSPECT_ROW_RELAY_RECEIPT);
        if (row_count >= max_rows) return row_count;
        if (row_count > 0 &&
            buf[INSPECT_SNAPSHOT_HEADER +
                (row_count - 1) * INSPECT_SNAPSHOT_ROW + 3] &
                INSPECT_ROW_HAS_RECEIPT) {
            return row_count;
        }
    }
    for (int nidx = 0; nidx < MAX_NPC_SHIPS && row_count < max_rows; nidx++) {
        if (nidx == exclude_npc_slot) continue;
        if (!receipt_world->npc_ships[nidx].active) continue;
        ship_t *ship = world_npc_ship_for((world_t *)receipt_world, nidx);
        if (!ship) continue;
        row_count = write_inspect_snapshot_matching_holder_receipt_rows(
            buf, row_count, max_rows, ship, npc_diag,
            INSPECT_ROW_RELAY_RECEIPT);
        if (row_count >= max_rows) return row_count;
        if (row_count > 0 &&
            buf[INSPECT_SNAPSHOT_HEADER +
                (row_count - 1) * INSPECT_SNAPSHOT_ROW + 3] &
                INSPECT_ROW_HAS_RECEIPT) {
            return row_count;
        }
    }
    return row_count;
}

static inline int write_inspect_snapshot_market_diag_rows(uint8_t *buf,
                                                          int row_count,
                                                          int max_rows,
                                                          const knowledge_view_t *knowledge,
                                                          const uint8_t emitted[KNOWLEDGE_VIEW_MAX_CAP]) {
    if (!buf || !knowledge || row_count >= max_rows) return row_count;
    int cap = knowledge->count;
    if (cap > KNOWLEDGE_VIEW_MAX_CAP) cap = KNOWLEDGE_VIEW_MAX_CAP;
    for (int i = 0; i < cap && row_count < max_rows && row_count < 4; i++) {
        if (emitted && emitted[i]) continue;
        market_memory_t memory;
        if (!inspect_snapshot_market_memory_from_item(&knowledge->items[i], &memory))
            continue;
        uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER + row_count * INSPECT_SNAPSHOT_ROW];
        write_inspect_snapshot_market_diag_row(p, &memory,
                                               &knowledge->items[i]);
        row_count++;
    }
    return row_count;
}

static inline uint64_t inspect_snapshot_job_diag_meta(const npc_ship_t *npc,
                                                      int idx) {
    if (!npc || idx < 0 || idx >= (int)(sizeof(npc->job_diag_kind) /
                                         sizeof(npc->job_diag_kind[0])))
        return 0;
    return (uint64_t)npc->job_diag_source[idx]
         | ((uint64_t)npc->job_diag_dest[idx] << 8)
         | ((uint64_t)npc->job_diag_kind[idx] << 16)
         | ((uint64_t)npc->job_diag_commodity[idx] << 24);
}

static inline void write_inspect_snapshot_job_diag_row(uint8_t *p,
                                                       const npc_ship_t *npc,
                                                       int idx) {
    memset(p, 0, INSPECT_SNAPSHOT_ROW);
    if (!npc || idx < 0 || idx >= npc->job_diag_count) return;
    p[0] = npc->job_diag_kind[idx];
    p[1] = npc->job_diag_score[idx];
    p[2] = npc->job_diag_selected[idx];
    p[3] = INSPECT_ROW_DIAGNOSTIC;
    write_u64_le(&p[4], inspect_snapshot_job_diag_meta(npc, idx));
    write_u16_le(&p[12], npc->job_diag_hint[idx]);
    p[14 + INSPECT_JOB_FACTOR_VALUE] = npc->job_diag_factor_value[idx];
    p[14 + INSPECT_JOB_FACTOR_DEMAND] = npc->job_diag_factor_demand[idx];
    p[14 + INSPECT_JOB_FACTOR_SUPPLY] = npc->job_diag_factor_supply[idx];
    p[14 + INSPECT_JOB_FACTOR_ROUTE] = npc->job_diag_factor_route[idx];
    p[14 + INSPECT_JOB_FACTOR_FRESHNESS] = npc->job_diag_factor_freshness[idx];
    p[14 + INSPECT_JOB_FACTOR_CAPABILITY] = npc->job_diag_factor_capability[idx];
    p[14 + INSPECT_JOB_FACTOR_PROOF] = npc->job_diag_factor_proof[idx];
    p[14 + INSPECT_JOB_FACTOR_HOLOGRAM] = npc->job_diag_factor_hologram[idx];
    p[14 + INSPECT_JOB_META_REASON] = npc->job_diag_reason[idx];
    p[14 + INSPECT_JOB_META_MEMORY_KIND] = npc->job_diag_memory_kind[idx];
    p[14 + INSPECT_JOB_META_HOPS] = npc->job_diag_memory_hops[idx];
    p[14 + INSPECT_JOB_META_AGE] = npc->job_diag_memory_age[idx];
    p[14 + INSPECT_JOB_META_SOURCE_STATION] = npc->job_diag_memory_station[idx];
    p[14 + INSPECT_JOB_META_PROOF_KIND] = npc->job_diag_proof_kind[idx];
    p[14 + INSPECT_JOB_META_PROOF0] = npc->job_diag_proof_prefix[idx][0];
    p[14 + INSPECT_JOB_META_PROOF1] = npc->job_diag_proof_prefix[idx][1];
    p[14 + INSPECT_JOB_META_PROOF2] = npc->job_diag_proof_prefix[idx][2];
    p[14 + INSPECT_JOB_META_PROOF3] = npc->job_diag_proof_prefix[idx][3];
    memcpy(&p[46], npc->job_diag_proof_hash[idx], 32);
}

static inline int write_inspect_snapshot_job_diag_rows(uint8_t *buf,
                                                       int row_count,
                                                       int max_rows,
                                                       const npc_ship_t *npc) {
    if (!buf || !npc || row_count >= max_rows) return row_count;
    int count = npc->job_diag_count;
    int cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
    if (count > cap) count = cap;
    for (int i = 0; i < count && row_count < max_rows && row_count < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_NONE) continue;
        uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER + row_count * INSPECT_SNAPSHOT_ROW];
        write_inspect_snapshot_job_diag_row(p, npc, i);
        row_count++;
    }
    return row_count;
}

static inline int serialize_inspect_snapshot_ship_manifest(uint8_t *buf,
                                                           int target_type,
                                                           uint8_t target_index,
                                                           int module_index,
                                                           uint8_t role,
                                                           uint8_t state,
                                                           uint8_t home_station,
                                                           uint8_t dest_station,
                                                           const ship_t *ship,
                                                           const knowledge_view_t *knowledge,
                                                           const npc_ship_t *npc_diag,
                                                           const station_t *receipt_stations,
                                                           int receipt_station_count,
                                                           const world_t *receipt_world,
                                                           int receipt_exclude_npc_slot) {
    if (!ship)
        return serialize_inspect_snapshot_target(buf, INSPECT_TARGET_NONE, -1, -1);

    (void)serialize_inspect_snapshot_target(buf, target_type,
                                            (int)target_index, module_index);
    buf[4] = role;
    buf[5] = state;
    buf[6] = home_station;
    buf[7] = dest_station;

    uint16_t manifest_count = ship->manifest.units ? ship->manifest.count : 0;
    write_u16_le(&buf[9], manifest_count);

    uint16_t bulk[COMMODITY_COUNT][MINING_GRADE_COUNT];
    memset(bulk, 0, sizeof(bulk));
    const ship_receipts_t *rcpts = ship_get_receipts_const(ship);
    for (uint16_t i = 0; i < manifest_count; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (inspect_snapshot_unit_is_groupable(u)) {
            if (bulk[u->commodity][u->grade] < 0xFFFF)
                bulk[u->commodity][u->grade]++;
        }
    }

    int row_count = write_inspect_snapshot_job_diag_rows(
        buf, 0, INSPECT_SNAPSHOT_MAX_ROWS, npc_diag);
    uint8_t emitted_market_rows[KNOWLEDGE_VIEW_MAX_CAP] = {0};
    row_count = write_inspect_snapshot_job_source_rows(
        buf, row_count, INSPECT_SNAPSHOT_MAX_ROWS, knowledge, npc_diag,
        emitted_market_rows);
    int receipt_row_start = row_count;
    row_count = write_inspect_snapshot_matching_receipt_rows(
        buf, row_count, INSPECT_SNAPSHOT_MAX_ROWS, ship, rcpts, npc_diag);
    if (row_count == receipt_row_start) {
        row_count = write_inspect_snapshot_matching_station_receipt_rows(
            buf, row_count, INSPECT_SNAPSHOT_MAX_ROWS, receipt_stations,
            receipt_station_count, npc_diag);
    }
    if (row_count == receipt_row_start) {
        row_count = write_inspect_snapshot_matching_relay_receipt_rows(
            buf, row_count, INSPECT_SNAPSHOT_MAX_ROWS, receipt_world,
            receipt_exclude_npc_slot, npc_diag);
    }
    row_count = write_inspect_snapshot_market_diag_rows(
        buf, row_count, INSPECT_SNAPSHOT_MAX_ROWS, knowledge,
        emitted_market_rows);
    for (int gr = 0; gr < MINING_GRADE_COUNT && row_count < INSPECT_SNAPSHOT_MAX_ROWS; gr++) {
        for (int c = 0; c < COMMODITY_COUNT && row_count < INSPECT_SNAPSHOT_MAX_ROWS; c++) {
            if (bulk[c][gr] > 0) {
                uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER + row_count * INSPECT_SNAPSHOT_ROW];
                write_inspect_snapshot_row(p, NULL, (uint8_t)c, (uint8_t)gr,
                                           bulk[c][gr], NULL, true,
                                           (uint8_t)INGOT_PREFIX_ANONYMOUS);
                row_count++;
            }

            for (uint16_t i = 0; i < manifest_count && row_count < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
                const cargo_unit_t *u = &ship->manifest.units[i];
                if (u->commodity != c || u->grade != gr) continue;
                if (inspect_snapshot_unit_is_groupable(u)) continue;
                const cargo_receipt_chain_t *chain =
                    (rcpts && i < rcpts->count) ? &rcpts->chains[i] : NULL;
                if (inspect_snapshot_chain_matches_job_proof(chain, npc_diag))
                    continue;
                uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER + row_count * INSPECT_SNAPSHOT_ROW];
                write_inspect_snapshot_row(p, u, u->commodity, u->grade, 1,
                                           chain, false,
                                           (uint8_t)INGOT_PREFIX_ANONYMOUS);
                row_count++;
            }
        }
    }

    buf[8] = (uint8_t)row_count;
    return INSPECT_SNAPSHOT_HEADER + row_count * INSPECT_SNAPSHOT_ROW;
}

static inline int serialize_inspect_snapshot_npc(uint8_t *buf,
                                                  uint8_t target_index,
                                                  const npc_ship_t *npc,
                                                  const ship_t *ship) {
    if (!npc || !npc->active || !ship)
        return serialize_inspect_snapshot_target(buf, INSPECT_TARGET_NONE, -1, -1);

    uint8_t home = (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
        ? (uint8_t)npc->home_station : 0xFFu;
    uint8_t dest = (npc->dest_station >= 0 && npc->dest_station < MAX_STATIONS)
        ? (uint8_t)npc->dest_station : 0xFFu;
    return serialize_inspect_snapshot_ship_manifest(
        buf, INSPECT_TARGET_NPC, target_index, -1,
        (uint8_t)npc->role, (uint8_t)npc->state, home, dest, ship,
        &npc->knowledge, npc, NULL, 0, NULL, -1);
}

static inline int serialize_inspect_snapshot_npc_with_station_receipts(
    uint8_t *buf,
    uint8_t target_index,
    const npc_ship_t *npc,
    const ship_t *ship,
    const station_t *stations,
    int station_count) {
    if (!npc || !npc->active || !ship)
        return serialize_inspect_snapshot_target(buf, INSPECT_TARGET_NONE, -1, -1);

    uint8_t home = (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
        ? (uint8_t)npc->home_station : 0xFFu;
    uint8_t dest = (npc->dest_station >= 0 && npc->dest_station < MAX_STATIONS)
        ? (uint8_t)npc->dest_station : 0xFFu;
    return serialize_inspect_snapshot_ship_manifest(
        buf, INSPECT_TARGET_NPC, target_index, -1,
        (uint8_t)npc->role, (uint8_t)npc->state, home, dest, ship,
        &npc->knowledge, npc, stations, station_count, NULL, -1);
}

static inline int serialize_inspect_snapshot_npc_with_world_receipts(
    uint8_t *buf,
    uint8_t target_index,
    const npc_ship_t *npc,
    const ship_t *ship,
    const world_t *receipt_world) {
    if (!npc || !npc->active || !ship)
        return serialize_inspect_snapshot_target(buf, INSPECT_TARGET_NONE, -1, -1);

    uint8_t home = (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
        ? (uint8_t)npc->home_station : 0xFFu;
    uint8_t dest = (npc->dest_station >= 0 && npc->dest_station < MAX_STATIONS)
        ? (uint8_t)npc->dest_station : 0xFFu;
    const station_t *stations = receipt_world ? receipt_world->stations : NULL;
    int station_count = receipt_world ? MAX_STATIONS : 0;
    return serialize_inspect_snapshot_ship_manifest(
        buf, INSPECT_TARGET_NPC, target_index, -1,
        (uint8_t)npc->role, (uint8_t)npc->state, home, dest, ship,
        &npc->knowledge, npc, stations, station_count, receipt_world,
        (int)target_index);
}

static inline int serialize_inspect_snapshot_player(uint8_t *buf,
                                                     uint8_t target_index,
                                                     const server_player_t *player) {
    if (!player || !player->connected)
        return serialize_inspect_snapshot_target(buf, INSPECT_TARGET_NONE, -1, -1);

    uint8_t near_station =
        (player->nearby_station >= 0 && player->nearby_station < MAX_STATIONS)
        ? (uint8_t)player->nearby_station : 0xFFu;
    uint8_t current_station =
        (player->current_station >= 0 && player->current_station < MAX_STATIONS)
        ? (uint8_t)player->current_station : near_station;
    float rounded_hull = player->ship.hull + 0.5f;
    if (rounded_hull < 0.0f) rounded_hull = 0.0f;
    if (rounded_hull > 255.0f) rounded_hull = 255.0f;

    return serialize_inspect_snapshot_ship_manifest(
        buf, INSPECT_TARGET_PLAYER, target_index, -1,
        (uint8_t)player->ship.hull_class, (uint8_t)rounded_hull,
        current_station, near_station, &player->ship, NULL, NULL, NULL, 0,
        NULL, -1);
}

/* Signal channel (#316) snapshot — the client dedupes by id so this
 * works as both the connect-time full sync and the per-post update. */
static inline int serialize_signal_channel(uint8_t *buf, const signal_channel_t *ch) {
    int n = ch->count;
    if (n > SIGNAL_CHANNEL_CAPACITY) n = SIGNAL_CHANNEL_CAPACITY;
    buf[0] = NET_MSG_SIGNAL_CHANNEL;
    write_u16_le(&buf[1], (uint16_t)n);
    int start = (ch->head - n + SIGNAL_CHANNEL_CAPACITY) % SIGNAL_CHANNEL_CAPACITY;
    for (int i = 0; i < n; i++) {
        int slot = (start + i) % SIGNAL_CHANNEL_CAPACITY;
        const signal_channel_msg_t *m = &ch->msgs[slot];
        uint8_t *p = &buf[3 + i * SIGNAL_CHANNEL_RECORD_SIZE];
        memset(p, 0, SIGNAL_CHANNEL_RECORD_SIZE);
        for (int k = 0; k < 8; k++) p[k] = (uint8_t)(m->id >> (8 * k));
        for (int k = 0; k < 4; k++) p[8 + k] = (uint8_t)(m->timestamp_ms >> (8 * k));
        p[12] = (uint8_t)(m->sender_station & 0xFF);
        p[13] = m->text_len;
        memcpy(&p[14], m->text, m->text_len);
        memcpy(&p[14 + 200], m->entry_hash, 32);
    }
    return 3 + n * SIGNAL_CHANNEL_RECORD_SIZE;
}

/*
 * WORLD_NPCS message:
 * [type:1][count:1] + count * NPC_RECORD_SIZE-byte records
 * (29 legacy pose/target/tint bytes + 8 session-token bytes + 1 home-station byte)
 */
static inline bool serialize_relevance_in_player_view(vec2 pos,
                                                      vec2 player_pos) {
    return v2_dist_sq(pos, player_pos) <= ASTEROID_VIEW_RADIUS_SQ;
}

static inline void serialize_one_npc(uint8_t *p, int index,
                                     const npc_ship_t *n) {
    p[0] = (uint8_t)index;
    p[1] = 1; /* active */
    p[1] |= (((uint8_t)n->role & 0x3) << 1);
    p[1] |= (((uint8_t)n->state & 0x7) << 3);
    if (n->thrusting) p[1] |= (1 << 6);
    write_f32_le(&p[2],  n->ship.pos.x);
    write_f32_le(&p[6],  n->ship.pos.y);
    write_f32_le(&p[10], n->ship.vel.x);
    write_f32_le(&p[14], n->ship.vel.y);
    write_f32_le(&p[18], n->ship.angle);
    uint16_t target = (n->target_asteroid >= 0 && n->target_asteroid < MAX_ASTEROIDS)
        ? (uint16_t)n->target_asteroid : 0xFFFFu;
    int towed_idx = npc_towed_fragment_index(n);
    uint16_t towed = (towed_idx >= 0) ? (uint16_t)towed_idx : 0xFFFFu;
    write_u16_le(&p[22], target);
    write_u16_le(&p[24], towed);
    p[26] = (uint8_t)(n->tint_r * 255.0f);
    p[27] = (uint8_t)(n->tint_g * 255.0f);
    p[28] = (uint8_t)(n->tint_b * 255.0f);
    memcpy(&p[29], n->session_token, sizeof(n->session_token));
    p[37] = (uint8_t)(n->home_station & 0xFF);
}

static inline int serialize_npcs(uint8_t *buf, const npc_ship_t *npcs) {
    int count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        serialize_one_npc(&buf[2 + count * NPC_RECORD_SIZE], i, &npcs[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPCS;
    buf[1] = (uint8_t)count;
    return 2 + count * NPC_RECORD_SIZE;
}

static inline int serialize_npcs_for_player(uint8_t *buf,
                                            const npc_ship_t *npcs,
                                            vec2 player_pos) {
    int count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;
        serialize_one_npc(&buf[2 + count * NPC_RECORD_SIZE], i, &npcs[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPCS;
    buf[1] = (uint8_t)count;
    return 2 + count * NPC_RECORD_SIZE;
}

/*
 * WORLD_STATIONS message:
 * [type:1][count:1] + count * [index:1][inventory: COMMODITY_COUNT×f32]
 * = 2 + count * STATION_RECORD_SIZE bytes
 */
/* STATION_RECORD_SIZE defined in shared/net_protocol.h */

/* Compile-time guard: if the record layout changes, update STATION_RECORD_SIZE
 * (in shared/net_protocol.h) and all buffers that depend on it. */
/* Compile-time guards: record sizes must match serialization layouts. */
_Static_assert(
    1 + 5 * 4 + 1 + 1 + 1 + 20 + 7 + 4 * 4 + 2 + 4 + 4 == PLAYER_RECORD_SIZE,
    "PLAYER_RECORD_SIZE must match serialized player state layout"
);
_Static_assert(
    2 + 1 + 7 * 4 + 1 + 1 + 1 + 1 == ASTEROID_RECORD_SIZE,  /* uint16 index + flags + 7 floats + smelt:u8 + grade:u8 + crystal_stage:u8 + phase:u8 */
    "ASTEROID_RECORD_SIZE must match serialized asteroid layout"
);
_Static_assert(
    4 + 6 * 4 + 2 + 2 + 2 + 1 + 1 + 2 == CARGO_POD_RECORD_SIZE,
    "CARGO_POD_RECORD_SIZE must match serialized cargo pod layout"
);
_Static_assert(
    2 + 5 * 4 + 2 + 2 + 3 + 8 + 1 == NPC_RECORD_SIZE,
    "NPC_RECORD_SIZE must match serialized NPC layout"
);
_Static_assert(
    1 + COMMODITY_COUNT * 4 + 4 == STATION_RECORD_SIZE,
    "STATION_RECORD_SIZE must match serialized station econ layout"
);
/* PLAYER_SHIP_SIZE is now a maximum (variable length due to path waypoints).
 * The fixed header is 16 + COMMODITY_COUNT*4 + 14 = 66 bytes, plus up to
 * 2 + 12*8 = 98 bytes of path data. */

static inline int serialize_stations(uint8_t *buf, const station_t *stations) {
    int count = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &stations[i];
        if (!station_exists(st)) continue;
        uint8_t *p = &buf[2 + count * STATION_RECORD_SIZE];
        p[0] = (uint8_t)i;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            write_f32_le(&p[1 + c * 4], st->_inventory_cache[c]);
        /* Derived from -Σ(ledger.balance); the field was removed but
         * the wire shape is preserved so old clients still parse. */
        write_f32_le(&p[1 + COMMODITY_COUNT * 4], station_credit_pool(st));
        count++;
    }
    buf[0] = NET_MSG_WORLD_STATIONS;
    buf[1] = (uint8_t)count;
    return 2 + count * STATION_RECORD_SIZE;
}

/*
 * STATION_IDENTITY message — structural/static fields for one station.
 * Sent on player join (for all active stations) and when structural data changes.
 * [type:1][index:1][reserved:1][services:4][pos_x:f32][pos_y:f32]
 * [radius:f32][dock_radius:f32][signal_range:f32][name:32] + fixed trailers.
 * Live per-module flow diagnostics are sent separately via STATION_DIAG.
 */
static inline int serialize_station_identity(uint8_t *buf, int index, const station_t *st) {
    buf[0] = NET_MSG_STATION_IDENTITY;
    buf[1] = (uint8_t)index;
    buf[2] = 0;
    if (st->scaffold) buf[2] |= 1;  /* bit 0: scaffold */
    if (st->planned)  buf[2] |= 2;  /* bit 1: planned */
    write_u32_le(&buf[3], st->services);
    write_f32_le(&buf[7], st->pos.x);
    write_f32_le(&buf[11], st->pos.y);
    write_f32_le(&buf[15], st->radius);
    write_f32_le(&buf[19], st->dock_radius);
    write_f32_le(&buf[23], st->signal_range);
    memset(&buf[27], 0, 32);
    { size_t n = strlen(st->name); if (n > 31) n = 31; memcpy(&buf[27], st->name, n); }
    for (int c = 0; c < COMMODITY_COUNT; c++)
        write_f32_le(&buf[59 + c * 4], st->base_price[c]);
    write_f32_le(&buf[59 + COMMODITY_COUNT * 4], st->scaffold_progress);
    int moff = 59 + COMMODITY_COUNT * 4 + 4;  /* after scaffold_progress */
    int module_count = st->module_count;
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION) module_count = MAX_MODULES_PER_STATION;
    buf[moff] = (uint8_t)module_count;
    moff++;
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        bool live = m < module_count;
        buf[moff]     = live ? (uint8_t)st->modules[m].type : 0;
        buf[moff + 1] = (live && st->modules[m].scaffold) ? 1 : 0;
        buf[moff + 2] = live ? st->modules[m].ring : 0;
        buf[moff + 3] = live ? st->modules[m].slot : 0;
        write_f32_le(&buf[moff + 4], live ? st->modules[m].build_progress : 0.0f);
        buf[moff + 8] = live ? st->modules[m].commodity : (uint8_t)COMMODITY_COUNT;
        moff += STATION_MODULE_RECORD_SIZE;
    }
    /* Ring rotation speeds + offsets */
    buf[moff] = (uint8_t)st->arm_count;
    moff++;
    for (int a = 0; a < MAX_ARMS; a++) {
        write_f32_le(&buf[moff], st->arm_speed[a]);
        moff += 4;
    }
    for (int a = 0; a < MAX_ARMS; a++) {
        write_f32_le(&buf[moff], st->ring_offset[a]);
        moff += 4;
    }
    for (int a = 0; a < MAX_ARMS; a++) {
        write_f32_le(&buf[moff], st->arm_rotation[a]);
        moff += 4;
    }
    for (int a = 0; a < MAX_ARMS; a++) {
        write_f32_le(&buf[moff], st->arm_omega[a]);
        moff += 4;
    }
    /* Placement plans (faction-shared blueprint slots) */
    int plan_n = st->placement_plan_count;
    if (plan_n > STATION_PLAN_RECORD_COUNT) plan_n = STATION_PLAN_RECORD_COUNT;
    buf[moff] = (uint8_t)plan_n;
    moff++;
    for (int p = 0; p < STATION_PLAN_RECORD_COUNT; p++) {
        if (p < plan_n) {
            buf[moff + 0] = (uint8_t)st->placement_plans[p].type;
            buf[moff + 1] = st->placement_plans[p].ring;
            buf[moff + 2] = st->placement_plans[p].slot;
            buf[moff + 3] = (uint8_t)st->placement_plans[p].owner;
        } else {
            buf[moff + 0] = 0;
            buf[moff + 1] = 0;
            buf[moff + 2] = 0;
            buf[moff + 3] = 0xFF;
        }
        moff += STATION_PLAN_RECORD_SIZE;
    }
    /* Pending shipyard orders (head-of-queue first). Count + fixed-size
     * trailer so the message size stays a compile-time constant. */
    int pend_n = st->pending_scaffold_count;
    if (pend_n > STATION_PENDING_SCAFFOLD_RECORD_COUNT) pend_n = STATION_PENDING_SCAFFOLD_RECORD_COUNT;
    buf[moff] = (uint8_t)pend_n;
    moff++;
    for (int p = 0; p < STATION_PENDING_SCAFFOLD_RECORD_COUNT; p++) {
        if (p < pend_n) {
            buf[moff + 0] = (uint8_t)st->pending_scaffolds[p].type;
            buf[moff + 1] = (uint8_t)st->pending_scaffolds[p].owner;
        } else {
            buf[moff + 0] = 0;
            buf[moff + 1] = 0xFF;
        }
        moff += STATION_PENDING_SCAFFOLD_RECORD_SIZE;
    }
    int ship_n = st->pending_ship_build_count;
    if (ship_n > STATION_PENDING_SHIP_RECORD_COUNT) ship_n = STATION_PENDING_SHIP_RECORD_COUNT;
    buf[moff] = (uint8_t)ship_n;
    moff++;
    for (int p = 0; p < STATION_PENDING_SHIP_RECORD_COUNT; p++) {
        if (p < ship_n) {
            buf[moff + 0] = (uint8_t)st->pending_ship_builds[p].hull_class;
            buf[moff + 1] = (uint8_t)st->pending_ship_builds[p].owner;
            write_f32_le(&buf[moff + 2], st->pending_ship_builds[p].build_progress);
        } else {
            buf[moff + 0] = 0;
            buf[moff + 1] = 0xFF;
            write_f32_le(&buf[moff + 2], 0.0f);
        }
        moff += STATION_PENDING_SHIP_RECORD_SIZE;
    }
    /* Operator-authored text trailers. Hail/MOTD is the normal station
     * response. Chatter arrays are station-specific NPC speech lines.
     * RATi hail is shown when the local player delivers RATi+ ore. */
    memset(&buf[moff], 0, STATION_IDENTITY_HAIL_MESSAGE_LEN);
    {
        size_t n = strlen(st->hail_message);
        if (n > STATION_IDENTITY_HAIL_MESSAGE_LEN - 1) n = STATION_IDENTITY_HAIL_MESSAGE_LEN - 1;
        memcpy(&buf[moff], st->hail_message, n);
    }
    moff += STATION_IDENTITY_HAIL_MESSAGE_LEN;
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        memset(&buf[moff], 0, STATION_IDENTITY_CHATTER_LINE_LEN);
        size_t n = strlen(st->miner_chatter[i]);
        if (n > STATION_IDENTITY_CHATTER_LINE_LEN - 1) n = STATION_IDENTITY_CHATTER_LINE_LEN - 1;
        memcpy(&buf[moff], st->miner_chatter[i], n);
        moff += STATION_IDENTITY_CHATTER_LINE_LEN;
    }
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        memset(&buf[moff], 0, STATION_IDENTITY_CHATTER_LINE_LEN);
        size_t n = strlen(st->hauler_chatter[i]);
        if (n > STATION_IDENTITY_CHATTER_LINE_LEN - 1) n = STATION_IDENTITY_CHATTER_LINE_LEN - 1;
        memcpy(&buf[moff], st->hauler_chatter[i], n);
        moff += STATION_IDENTITY_CHATTER_LINE_LEN;
    }
    memset(&buf[moff], 0, STATION_IDENTITY_RATI_HAIL_LEN);
    {
        size_t n = strlen(st->rati_hail_message);
        if (n > STATION_IDENTITY_RATI_HAIL_LEN - 1) n = STATION_IDENTITY_RATI_HAIL_LEN - 1;
        memcpy(&buf[moff], st->rati_hail_message, n);
    }
    moff += STATION_IDENTITY_RATI_HAIL_LEN;

    /* Currency name trailer — 32 bytes, zero-padded. Empty → client
     * shows "credits". AI-editable via /api/station/<id>/command. */
    memset(&buf[moff], 0, STATION_IDENTITY_CURRENCY_NAME_LEN);
    {
        size_t n = strlen(st->currency_name);
        if (n > STATION_IDENTITY_CURRENCY_NAME_LEN - 1) n = STATION_IDENTITY_CURRENCY_NAME_LEN - 1;
        memcpy(&buf[moff], st->currency_name, n);
    }
    moff += STATION_IDENTITY_CURRENCY_NAME_LEN;
    /* Layer B of #479: per-station Ed25519 pubkey. The matching
     * station_secret is operator-only and is NEVER written to the
     * wire. */
    memcpy(&buf[moff], st->station_pubkey, STATION_IDENTITY_PUBKEY_LEN);
    moff += STATION_IDENTITY_PUBKEY_LEN;
    for (int h = 0; h < HULL_CLASS_COUNT; h++)
        buf[moff++] = st->stored_hull_count[h];
    buf[moff++] = st->faction_id;
    buf[moff++] = st->faction_allegiance;
    buf[moff++] = st->faction_ideology;
    for (int f = 0; f < STATION_FACTION_COUNT; f++)
        buf[moff++] = (uint8_t)st->faction_relations[f];
    int policy_n = st->policy_card_count;
    if (policy_n < 0) policy_n = 0;
    if (policy_n > STATION_IDENTITY_POLICY_CARD_COUNT)
        policy_n = STATION_IDENTITY_POLICY_CARD_COUNT;
    buf[moff++] = (uint8_t)policy_n;
    for (int i = 0; i < STATION_IDENTITY_POLICY_CARD_COUNT; i++)
        buf[moff++] = (i < policy_n) ? st->policy_card_ids[i] : 0;
    (void)moff;
    return STATION_IDENTITY_SIZE;
}

/*
 * STATION_DIAG message — live per-module flow diagnostics.
 * [type:1][index:1][module_count:1][diag:MAX_MODULES_PER_STATION×u8]
 */
static inline int serialize_station_diag(uint8_t *buf, int index, const station_t *st) {
    buf[0] = NET_MSG_STATION_DIAG;
    buf[1] = (uint8_t)index;
    int module_count = st ? st->module_count : 0;
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION) module_count = MAX_MODULES_PER_STATION;
    buf[2] = (uint8_t)module_count;
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        station_flow_diag_t diag = STATION_FLOW_DIAG_NONE;
        if (st && m < module_count)
            diag = station_module_flow_diag(st, m);
        buf[3 + m] = (uint8_t)diag;
    }
    return STATION_DIAG_SIZE;
}

/*
 * WORLD_SCAFFOLDS message: active scaffold pool (NASCENT/LOOSE/TOWING/SNAPPING/PLACED).
 * [type:1][count:1] + count * SCAFFOLD_RECORD_SIZE
 * Per record: [id:1][state:1][module_type:1][owner:1][pos:2xf32][vel:2xf32][radius:f32][build_amount:f32]
 * = 28 bytes
 */
static inline void serialize_one_scaffold(uint8_t *p, int index, const scaffold_t *sc) {
    p[0] = (uint8_t)index;
    p[1] = (uint8_t)sc->state;
    p[2] = (uint8_t)sc->module_type;
    p[3] = (sc->owner < 0) ? 0xFFu : (uint8_t)sc->owner;
    write_f32_le(&p[4],  sc->pos.x);
    write_f32_le(&p[8],  sc->pos.y);
    write_f32_le(&p[12], sc->vel.x);
    write_f32_le(&p[16], sc->vel.y);
    write_f32_le(&p[20], sc->radius);
    write_f32_le(&p[24], sc->build_amount);
}

static inline int serialize_scaffolds(uint8_t *buf, const scaffold_t *scaffolds) {
    int count = 0;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!scaffolds[i].active) continue;
        serialize_one_scaffold(&buf[2 + count * SCAFFOLD_RECORD_SIZE], i, &scaffolds[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_SCAFFOLDS;
    buf[1] = (uint8_t)count;
    return 2 + count * SCAFFOLD_RECORD_SIZE;
}

static inline int serialize_scaffolds_for_player(uint8_t *buf,
                                                 const scaffold_t *scaffolds,
                                                 vec2 player_pos) {
    int count = 0;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!scaffolds[i].active) continue;
        if (!serialize_relevance_in_player_view(scaffolds[i].pos, player_pos))
            continue;
        serialize_one_scaffold(&buf[2 + count * SCAFFOLD_RECORD_SIZE],
                               i, &scaffolds[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_SCAFFOLDS;
    buf[1] = (uint8_t)count;
    return 2 + count * SCAFFOLD_RECORD_SIZE;
}

/*
 * WORLD_CARGO_PODS message: active engine-less towable cargo bodies.
 * [type:1][count:1] + count * CARGO_POD_RECORD_SIZE
 */
static inline void serialize_one_cargo_pod(uint8_t *p, int index, const cargo_pod_t *pod) {
    p[0] = (uint8_t)index;
    p[1] = (uint8_t)pod->kind;
    p[2] = (uint8_t)pod->commodity;
    p[3] = (pod->towed_by < 0) ? 0xFFu : (uint8_t)pod->towed_by;
    write_f32_le(&p[4],  pod->pos.x);
    write_f32_le(&p[8],  pod->pos.y);
    write_f32_le(&p[12], pod->vel.x);
    write_f32_le(&p[16], pod->vel.y);
    write_f32_le(&p[20], pod->radius);
    write_f32_le(&p[24], pod->rotation);
    write_u16_le(&p[28], pod->quantity);
    write_u16_le(&p[30], pod->manifest_count);
    write_u16_le(&p[32], pod->shipment_id);
    uint8_t flags = 0;
    uint8_t best_grade = (uint8_t)MINING_GRADE_COMMON;
    if (pod->shipment_id != 0)
        flags |= CARGO_POD_SUMMARY_SHIPMENT_BOUND;
    if (pod->manifest_count > 0 &&
        pod->manifest_count <= CARGO_POD_MANIFEST_CAP) {
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            uint8_t grade = pod->manifest_units[i].grade;
            if (grade < (uint8_t)MINING_GRADE_COUNT && grade > best_grade)
                best_grade = grade;
        }
    }
    if (cargo_pod_has_exact_manifest(pod, pod->commodity)) {
        flags |= CARGO_POD_SUMMARY_EXACT_MATERIAL;
    }
    p[34] = flags;
    p[35] = best_grade;
    p[36] = pod->tractor_station;
    p[37] = pod->tractor_module;
}

static inline int serialize_cargo_pods(uint8_t *buf, const cargo_pod_t *pods) {
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!pods[i].active) continue;
        serialize_one_cargo_pod(&buf[2 + count * CARGO_POD_RECORD_SIZE], i, &pods[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_CARGO_PODS;
    buf[1] = (uint8_t)count;
    return 2 + count * CARGO_POD_RECORD_SIZE;
}

static inline int serialize_cargo_pods_for_player(uint8_t *buf,
                                                  const cargo_pod_t *pods,
                                                  vec2 player_pos) {
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!pods[i].active) continue;
        if (!serialize_relevance_in_player_view(pods[i].pos, player_pos))
            continue;
        serialize_one_cargo_pod(&buf[2 + count * CARGO_POD_RECORD_SIZE],
                                i, &pods[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_CARGO_PODS;
    buf[1] = (uint8_t)count;
    return 2 + count * CARGO_POD_RECORD_SIZE;
}

typedef void (*server_packet_sink_fn)(void *user, const uint8_t *data, int len);
typedef void (*server_player_packet_sink_fn)(void *user, int player_slot,
                                             const uint8_t *data, int len);

typedef struct {
    uint8_t asteroids[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t players[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    uint8_t npcs[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    uint8_t scaffolds[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
    uint8_t cargo_pods[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    uint8_t world_time[5];
} server_world_snapshot_scratch_t;

static inline void server_emit_world_snapshot_for_player(
    world_t *w,
    int player_slot,
    bool include_player_states,
    server_packet_sink_fn send,
    void *send_user,
    server_world_snapshot_scratch_t *scratch) {
    if (!w || !send || !scratch) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &w->players[player_slot];
    if (!sp->connected) return;

    int alen = serialize_asteroids_for_player(
        scratch->asteroids, w->asteroids, sp->ship.pos, sp->asteroid_sent);
    if (alen > ASTEROID_MSG_HEADER)
        send(send_user, scratch->asteroids, alen);

    if (include_player_states) {
        int plen = serialize_all_player_states(
            scratch->players, w->players, w->tick);
        send(send_user, scratch->players, plen);
    }

    int nlen = serialize_npcs_for_player(
        scratch->npcs, w->npc_ships, sp->ship.pos);
    send(send_user, scratch->npcs, nlen);

    int slen = serialize_scaffolds_for_player(
        scratch->scaffolds, w->scaffolds, sp->ship.pos);
    send(send_user, scratch->scaffolds, slen);

    int clen = serialize_cargo_pods_for_player(
        scratch->cargo_pods, w->cargo_pods, sp->ship.pos);
    send(send_user, scratch->cargo_pods, clen);

    scratch->world_time[0] = NET_MSG_WORLD_TIME;
    write_f32_le(&scratch->world_time[1], w->time);
    send(send_user, scratch->world_time, (int)sizeof(scratch->world_time));
}

static inline void server_clear_asteroid_net_dirty(world_t *w) {
    if (!w) return;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        w->asteroids[i].net_dirty = false;
}

static inline bool server_player_slot_in_emit_range(int slot,
                                                    int only_player_slot) {
    if (slot < 0 || slot >= MAX_PLAYERS) return false;
    return only_player_slot < 0 || slot == only_player_slot;
}

static inline void server_emit_fracture_updates(
    world_t *w,
    int only_player_slot,
    server_player_packet_sink_fn send,
    void *send_user) {
    if (!w || !send) return;
    uint32_t now_ms = (uint32_t)(w->time * 1000.0f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        fracture_claim_state_t *state = &w->fracture_claims[i];
        if (state->challenge_dirty && state->fracture_id &&
            w->asteroids[i].active) {
            uint8_t buf[FRACTURE_CHALLENGE_SIZE];
            int len = serialize_fracture_challenge_for_world(buf, w, i);
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!server_player_slot_in_emit_range(p, only_player_slot))
                    continue;
                if (!w->players[p].connected) continue;
                if (server_fracture_player_in_range_for_world(w, p, i))
                    send(send_user, p, buf, len);
            }
            state->challenge_dirty = false;
        }
        if (state->resolved_dirty && state->fracture_id &&
            w->asteroids[i].active) {
            uint8_t buf[FRACTURE_RESOLVED_SIZE];
            int len = serialize_fracture_resolved_for_world(buf, w, i);
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!server_player_slot_in_emit_range(p, only_player_slot))
                    continue;
                if (!w->players[p].connected) continue;
                if (server_fracture_player_in_range_for_world(w, p, i))
                    send(send_user, p, buf, len);
            }
            state->resolved_dirty = false;
        }
    }

    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        pending_resolve_t *pr = &w->pending_resolves[p];
        if (!pr->active) continue;
        if (pr->tx_count > 0 &&
            now_ms < pr->last_tx_ms + FRACTURE_RESOLVE_RETRY_PERIOD_MS) {
            continue;
        }
        uint8_t buf[FRACTURE_RESOLVED_SIZE];
        int len = serialize_pending_fracture_resolved(buf, pr);
        for (int pi = 0; pi < MAX_PLAYERS; pi++) {
            if (!server_player_slot_in_emit_range(pi, only_player_slot))
                continue;
            if (!w->players[pi].connected) continue;
            send(send_user, pi, buf, len);
        }
        pr->tx_count++;
        pr->last_tx_ms = now_ms;
        if (pr->tx_count >= FRACTURE_RESOLVE_RETRY_COUNT)
            pr->active = false;
    }
}

/*
 * PLAYER_SHIP message:
 * [type:1][id:1][hull:f32][station_balance:f32][docked:1][station:1]
 * [mining_lvl:1][hold_lvl:1][tractor_lvl:1][flags:1]
 * [cargo: COMMODITY_COUNT × f32]
 * [nearby_frags:1][tractor_frags:1][towed_count:1][towed_frags:20]
 * towed_frags = 10 × uint16_t (little-endian), 0xFFFF = unused.
 */
static inline int serialize_player_ship_bal(uint8_t *buf, uint8_t id, const server_player_t *sp, float station_balance) {
    buf[0] = NET_MSG_PLAYER_SHIP;
    buf[1] = id;
    write_f32_le(&buf[2], sp->ship.hull);
    write_f32_le(&buf[6], station_balance);
    buf[10] = sp->docked ? 1 : 0;
    buf[11] = (uint8_t)sp->current_station;
    buf[12] = (uint8_t)sp->ship.mining_level;
    buf[13] = (uint8_t)sp->ship.hold_level;
    buf[14] = (uint8_t)sp->ship.tractor_level;
    buf[15] = sp->autopilot_mode; /* repurposed reserved byte */
    for (int c = 0; c < COMMODITY_COUNT; c++)
        write_f32_le(&buf[16 + c * 4], sp->ship.cargo[c]);
    int off = 16 + COMMODITY_COUNT * 4;
    buf[off++] = (uint8_t)(sp->nearby_fragments < 255 ? sp->nearby_fragments : 255);
    buf[off++] = (uint8_t)(sp->tractor_fragments < 255 ? sp->tractor_fragments : 255);
    buf[off++] = sp->ship.towed_count;
    for (int t = 0; t < 10; t++) {
        int16_t fi = (t < sp->ship.towed_count) ? sp->ship.towed_fragments[t] : -1;
        uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        buf[off++] = (uint8_t)(wire & 0xFFu);
        buf[off++] = (uint8_t)(wire >> 8);
    }
    /* Autopilot target asteroid index (0xFF = none). */
    buf[off++] = (sp->autopilot_target >= 0 && sp->autopilot_target < 255)
        ? (uint8_t)sp->autopilot_target : 0xFF;
    /* Autopilot A* path waypoints — the actual path the server is following.
     * [count:1][wp0_x:f32][wp0_y:f32][wp1_x:f32]... */
    {
        nav_path_t *path = nav_player_path(sp->id);
        int pc = (sp->autopilot_mode && path->count > 0) ? path->count : 0;
        if (pc > 12) pc = 12;
        buf[off++] = (uint8_t)pc;
        buf[off++] = (pc > 0) ? (uint8_t)path->current : 0;
        for (int i = 0; i < pc; i++) {
            write_f32_le(&buf[off], path->waypoints[i].x); off += 4;
            write_f32_le(&buf[off], path->waypoints[i].y); off += 4;
        }
    }
    return off;
}

/*
 * CONTRACTS message:
 * [type:1][count:1] + count * contract record.
 */
/* Wire layout per contract record:
 *   action(1) + station(1) + commodity(1) + required_grade(1)
 *   + proof_flags(1) + required_prefix_class(1) + required_recipe_id(u16)
 *   + quantity_needed(f32) + base_price(f32) + age(f32)
 *   + target.x(f32) + target.y(f32) + target_index(u32)
 *   + required_parent(32)
 *   + forbidden_origin_mask(u64)
 *   + target_pub(32)
 * Bumped from 72 when stable target identity was added. */
static inline int serialize_contracts(uint8_t *buf, const contract_t *contracts) {
    int count = 0;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!contracts[i].active) continue;
        uint8_t *p = &buf[2 + count * CONTRACT_RECORD_SIZE];
        p[0] = (uint8_t)contracts[i].action;
        p[1] = contracts[i].station_index;
        p[2] = (uint8_t)contracts[i].commodity;
        p[3] = contracts[i].required_grade;
        p[4] = contracts[i].proof_flags;
        p[5] = contracts[i].required_prefix_class;
        write_u16_le(&p[6], contracts[i].required_recipe_id);
        write_f32_le(&p[8],  contracts[i].quantity_needed);
        write_f32_le(&p[12], contracts[i].base_price);
        write_f32_le(&p[16], contracts[i].age);
        write_f32_le(&p[20], contracts[i].target_pos.x);
        write_f32_le(&p[24], contracts[i].target_pos.y);
        write_u32_le(&p[28], (uint32_t)contracts[i].target_index);
        memcpy(&p[32], contracts[i].required_parent, 32);
        write_u64_le(&p[64], contracts[i].forbidden_origin_mask);
        memcpy(&p[72], contracts[i].target_pub, 32);
        /* Note: claimed_by not sent — server-only field */
        count++;
    }
    buf[0] = NET_MSG_CONTRACTS;
    buf[1] = (uint8_t)count;
    return 2 + count * CONTRACT_RECORD_SIZE;
}

static inline int contract_compact_index_for_slot(const contract_t *contracts,
                                                  int slot) {
    if (!contracts || slot < 0 || slot >= MAX_CONTRACTS) return -1;
    if (!contracts[slot].active) return -1;
    int ordinal = 0;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!contracts[k].active) continue;
        if (k == slot) return ordinal;
        ordinal++;
    }
    return -1;
}

/* Compute station-local balance for a player at their current/nearby
 * station. This must match the buy/credit paths: pubkey ledger when the
 * identity is verified, session-token ledger otherwise. */
static inline float server_player_station_balance_in_world(
    const world_t *w,
    const server_player_t *sp) {
    if (!w || !sp) return 0.0f;
    int st = sp->docked ? sp->current_station : sp->nearby_station;
    if (st < 0 || st >= MAX_STATIONS) return 0.0f;
    if (server_player_can_use_pubkey_persistence(sp))
        return ledger_balance_by_pubkey(&w->stations[st], sp->pubkey);
    return ledger_balance(&w->stations[st], sp->session_token);
}

static inline int server_action_result_station_index(
    const server_player_t *sp) {
    if (!sp) return -1;
    return sp->docked ? sp->current_station : sp->nearby_station;
}

static inline int server_action_result_station_pending_count(
    const world_t *w,
    const server_player_t *sp) {
    if (!w || !sp) return -1;
    int st = sp->pending_action_before_station;
    if (st < 0 || st >= MAX_STATIONS) return -1;
    return w->stations[st].pending_scaffold_count;
}

static inline int server_action_result_station_pending_ship_build_count(
    const world_t *w,
    const server_player_t *sp) {
    if (!w || !sp) return -1;
    int st = sp->pending_action_before_station;
    if (st < 0 || st >= MAX_STATIONS) return -1;
    return w->stations[st].pending_ship_build_count;
}

static inline void server_begin_pending_action_result(
    const world_t *w,
    server_player_t *sp,
    uint16_t action_id,
    uint16_t input_seq,
    uint8_t action) {
    if (!sp || action_id == 0 || action == NET_ACTION_NONE) return;
    int st = server_action_result_station_index(sp);
    sp->pending_action_result_valid = true;
    sp->pending_action_result_action = action;
    sp->pending_action_result_id = action_id;
    sp->pending_action_result_input_seq = input_seq;
    sp->pending_action_before_docked = sp->docked;
    sp->pending_action_before_docking_approach = sp->docking_approach;
    sp->pending_action_before_station = st;
    sp->pending_action_before_autopilot_mode = sp->autopilot_mode;
    sp->pending_action_before_hull = sp->ship.hull;
    sp->pending_action_before_cargo_total = ship_total_cargo(&sp->ship);
    sp->pending_action_before_manifest_count = sp->ship.manifest.count;
    sp->pending_action_before_mining_level = (uint8_t)sp->ship.mining_level;
    sp->pending_action_before_hold_level = (uint8_t)sp->ship.hold_level;
    sp->pending_action_before_tractor_level = (uint8_t)sp->ship.tractor_level;
    sp->pending_action_before_towed_count = sp->ship.towed_count;
    sp->pending_action_before_towed_scaffold = sp->ship.towed_scaffold;
    sp->pending_action_before_station_pending_scaffold_count =
        (w && st >= 0 && st < MAX_STATIONS)
            ? w->stations[st].pending_scaffold_count
            : -1;
    sp->pending_action_before_station_pending_ship_build_count =
        (w && st >= 0 && st < MAX_STATIONS)
            ? w->stations[st].pending_ship_build_count
            : -1;
    sp->pending_action_before_station_balance =
        server_player_station_balance_in_world(w, sp);
}

static inline bool server_pending_action_state_changed(
    const world_t *w,
    const server_player_t *sp) {
    if (!sp || !sp->pending_action_result_valid) return false;
    if (sp->pending_action_before_docked != sp->docked) return true;
    if (sp->pending_action_before_docking_approach !=
        sp->docking_approach) {
        return true;
    }
    if (sp->pending_action_before_station !=
        server_action_result_station_index(sp)) {
        return true;
    }
    if (sp->pending_action_before_autopilot_mode !=
        sp->autopilot_mode) {
        return true;
    }
    if (fabsf(sp->pending_action_before_hull - sp->ship.hull) > 0.01f)
        return true;
    if (fabsf(sp->pending_action_before_cargo_total -
              ship_total_cargo(&sp->ship)) > 0.01f) {
        return true;
    }
    if (sp->pending_action_before_manifest_count != sp->ship.manifest.count)
        return true;
    if (sp->pending_action_before_mining_level !=
        (uint8_t)sp->ship.mining_level) {
        return true;
    }
    if (sp->pending_action_before_hold_level !=
        (uint8_t)sp->ship.hold_level) {
        return true;
    }
    if (sp->pending_action_before_tractor_level !=
        (uint8_t)sp->ship.tractor_level) {
        return true;
    }
    if (sp->pending_action_before_towed_count != sp->ship.towed_count)
        return true;
    if (sp->pending_action_before_towed_scaffold !=
        sp->ship.towed_scaffold) {
        return true;
    }
    if (sp->pending_action_before_station_pending_scaffold_count !=
        server_action_result_station_pending_count(w, sp)) {
        return true;
    }
    if (sp->pending_action_before_station_pending_ship_build_count !=
        server_action_result_station_pending_ship_build_count(w, sp)) {
        return true;
    }
    if (fabsf(sp->pending_action_before_station_balance -
              server_player_station_balance_in_world(w, sp)) > 0.01f) {
        return true;
    }
    return false;
}

static inline bool server_action_matches_event(uint8_t action,
                                               uint8_t event_type) {
    if (action == NET_ACTION_DOCK) return event_type == SIM_EVENT_DOCK;
    if (action == NET_ACTION_LAUNCH) return event_type == SIM_EVENT_LAUNCH;
    if (action == NET_ACTION_SELL_CARGO ||
        (action >= NET_ACTION_DELIVER_COMMODITY &&
         action < NET_ACTION_DELIVER_COMMODITY + COMMODITY_COUNT)) {
        return event_type == SIM_EVENT_SELL ||
               event_type == SIM_EVENT_CONTRACT_COMPLETE;
    }
    if (action == NET_ACTION_REPAIR) return event_type == SIM_EVENT_REPAIR;
    if (action == NET_ACTION_UPGRADE_MINING ||
        action == NET_ACTION_UPGRADE_HOLD ||
        action == NET_ACTION_UPGRADE_TRACTOR) {
        return event_type == SIM_EVENT_UPGRADE;
    }
    if (action == NET_ACTION_PLACE_OUTPOST)
        return event_type == SIM_EVENT_OUTPOST_PLACED;
    if (action == NET_ACTION_HAIL) return event_type == SIM_EVENT_HAIL_RESPONSE;
    if (action == NET_ACTION_RESET) return event_type == SIM_EVENT_DEATH;
    if (action >= NET_ACTION_BUY_PRODUCT &&
        action < NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT) {
        return event_type == SIM_EVENT_BUY;
    }
    if (action == NET_ACTION_BUY_INGOT) return event_type == SIM_EVENT_BUY;
    return false;
}

static inline uint8_t server_pending_action_result_status(
    const world_t *w,
    const server_player_t *sp,
    const sim_events_t *events) {
    bool matched_event = false;
    bool rejected = false;
    if (sp && events) {
        for (int i = 0; i < events->count; i++) {
            const sim_event_t *ev = &events->events[i];
            if (ev->player_id != sp->id) continue;
            if (ev->type == SIM_EVENT_ORDER_REJECTED) rejected = true;
            if (server_action_matches_event(
                    sp->pending_action_result_action, (uint8_t)ev->type)) {
                matched_event = true;
            }
        }
    }
    if (rejected) return NET_ACTION_RESULT_REJECTED;
    if (matched_event || server_pending_action_state_changed(w, sp))
        return NET_ACTION_RESULT_OK;
    return NET_ACTION_RESULT_NOOP;
}

static inline const char *server_action_result_status_name(uint8_t status) {
    switch (status) {
    case NET_ACTION_RESULT_OK:       return "ok";
    case NET_ACTION_RESULT_REJECTED: return "rejected";
    case NET_ACTION_RESULT_NOOP:     return "noop";
    default:                         return "unknown";
    }
}

static inline int serialize_player_ship_for_world(
    uint8_t *buf,
    uint8_t id,
    const world_t *w,
    const server_player_t *sp) {
    if (!buf || !sp) return 0;
    return serialize_player_ship_bal(
        buf, id, sp, server_player_station_balance_in_world(w, sp));
}

static inline int serialize_inspect_snapshot_for_world(
    uint8_t *buf,
    const world_t *w,
    const server_player_t *sp) {
    if (!buf) return 0;
    if (!w || !sp || !sp->scan_active ||
        sp->scan_target_type == INSPECT_TARGET_NONE) {
        return serialize_inspect_snapshot_target(
            buf, INSPECT_TARGET_NONE, -1, -1);
    }

    if (sp->scan_target_type == INSPECT_TARGET_NPC &&
        sp->scan_target_index >= 0 &&
        sp->scan_target_index < MAX_NPC_SHIPS) {
        const npc_ship_t *npc = &w->npc_ships[sp->scan_target_index];
        ship_t *ship = world_npc_ship_for((world_t *)w, sp->scan_target_index);
        return serialize_inspect_snapshot_npc_with_station_receipts(
            buf, (uint8_t)sp->scan_target_index, npc, ship,
            w->stations, MAX_STATIONS);
    }

    if (sp->scan_target_type == INSPECT_TARGET_PLAYER &&
        sp->scan_target_index >= 0 &&
        sp->scan_target_index < MAX_PLAYERS) {
        return serialize_inspect_snapshot_player(
            buf, (uint8_t)sp->scan_target_index,
            &w->players[sp->scan_target_index]);
    }

    return serialize_inspect_snapshot_target(buf, sp->scan_target_type,
                                             sp->scan_target_index,
                                             sp->scan_module_index);
}

static inline int serialize_hail_response_for_world(
    uint8_t *buf,
    const world_t *w,
    const sim_event_t *ev) {
    if (!buf || !w || !ev) return 0;
    buf[0] = NET_MSG_HAIL_RESPONSE;
    buf[1] = (uint8_t)ev->hail_response.station;
    write_f32_le(&buf[2], ev->hail_response.credits);
    int compact_ci = contract_compact_index_for_slot(
        w->contracts, ev->hail_response.contract_index);
    buf[6] = (compact_ci >= 0) ? (uint8_t)compact_ci : 0xFFu;
    return 7;
}

/* Per-player gossip-contract visibility mask. Bit i set iff compact
 * contract record i from NET_MSG_CONTRACTS matches a summary in the
 * player's ship known_contracts pool (by action + station_index +
 * commodity + provenance terms). The client stores NET_MSG_CONTRACTS compactly too, so
 * the mask must use the same ordinal space, not raw w->contracts[]
 * slots. Wire: [type:1][mask:u32]. */
static inline int serialize_player_known_contracts(uint8_t *buf,
                                                   const contract_t *contracts,
                                                   const ship_t *ship) {
    uint32_t mask = 0;
    if (ship) {
        int ordinal = 0;
        for (int k = 0; k < MAX_CONTRACTS && k < 32; k++) {
            if (!contracts[k].active) continue;
            for (int i = 0; i < ship->known_contract_count; i++) {
                const contract_summary_t *cs = &ship->known_contracts[i];
                if (!cs->active) continue;
                if (cs->action == (uint8_t)contracts[k].action &&
                    cs->station_index == contracts[k].station_index &&
                    cs->commodity == (uint8_t)contracts[k].commodity &&
                    cs->required_grade == contracts[k].required_grade &&
                    cs->proof_flags == contracts[k].proof_flags &&
                    cs->required_prefix_class == contracts[k].required_prefix_class &&
                    cs->required_recipe_id == contracts[k].required_recipe_id &&
                    memcmp(cs->required_parent, contracts[k].required_parent, 32) == 0 &&
                    cs->forbidden_origin_mask == contracts[k].forbidden_origin_mask &&
                    memcmp(cs->target_pub, contracts[k].target_pub, 32) == 0) {
                    mask |= (1u << ordinal);
                    break;
                }
            }
            ordinal++;
        }
    }
    buf[0] = NET_MSG_PLAYER_KNOWN_CONTRACTS;
    write_u32_le(&buf[1], mask);
    return 5;
}

static inline int serialize_delivery_ledger(uint8_t *buf,
                                            const world_t *w,
                                            uint8_t player_id) {
    int count = 0;
    buf[0] = NET_MSG_DELIVERY_LEDGER;
    if (!w) {
        buf[1] = 0;
        return DELIVERY_LEDGER_HEADER;
    }
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS &&
                    count < DELIVERY_LEDGER_MAX_RECORDS; i++) {
        const delivery_shipment_t *s = &w->delivery_shipments[i];
        if (!s->active) continue;
        if (s->debtor_player != player_id) continue;
        if (s->status == DELIVERY_SHIPMENT_CLEARED) continue;
        uint8_t *p = &buf[DELIVERY_LEDGER_HEADER +
                          count * DELIVERY_LEDGER_RECORD_SIZE];
        memset(p, 0, DELIVERY_LEDGER_RECORD_SIZE);
        write_u16_le(&p[0], s->shipment_id);
        p[2] = s->status;
        p[3] = s->origin_station;
        p[4] = s->destination_station;
        p[5] = s->contract_index;
        p[6] = s->commodity;
        write_u16_le(&p[7], s->quantity_total);
        write_u16_le(&p[9], s->quantity_delivered);
        write_u16_le(&p[11], s->quantity_bound);
        write_f32_le(&p[13], s->debt_principal);
        write_f32_le(&p[17], s->destination_payout);
        write_f32_le(&p[21], s->origin_completion_credit);
        write_u32_le(&p[25], s->due_tick);
        uint16_t held_bound = 0;
        if (player_id < MAX_PLAYERS) {
            const ship_t *ship = &w->players[player_id].ship;
            for (int t = 0; t < ship->towed_pod_count && t < 10; t++) {
                int pod_idx = ship->towed_pods[t];
                if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) continue;
                const cargo_pod_t *pod = &w->cargo_pods[pod_idx];
                if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
                if (pod->shipment_id != s->shipment_id) continue;
                held_bound = pod->quantity;
                break;
            }
        }
        write_u16_le(&p[29], held_bound);
        count++;
    }
    buf[1] = (uint8_t)count;
    return DELIVERY_LEDGER_HEADER + count * DELIVERY_LEDGER_RECORD_SIZE;
}

typedef struct {
    uint8_t player_ship[PLAYER_SHIP_SIZE + 4];
    uint8_t hold_ingots[HOLD_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
    uint8_t player_manifest[
        PLAYER_MANIFEST_HEADER +
        COMMODITY_COUNT * MINING_GRADE_COUNT * PLAYER_MANIFEST_ENTRY
    ];
    uint8_t inspect_snapshot[INSPECT_SNAPSHOT_MAX_SIZE];
    uint8_t known_contracts[5];
    uint8_t delivery_ledger[
        DELIVERY_LEDGER_HEADER +
        DELIVERY_LEDGER_MAX_RECORDS * DELIVERY_LEDGER_RECORD_SIZE
    ];
} server_private_snapshot_scratch_t;

typedef struct {
    uint8_t station_identity[STATION_IDENTITY_SIZE + 4];
    uint8_t station_diag[STATION_DIAG_SIZE];
    uint8_t station_ingots[
        STATION_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE
    ];
    uint8_t station_manifest[
        STATION_MANIFEST_HEADER +
        COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY
    ];
    uint8_t world_stations[2 + MAX_STATIONS * STATION_RECORD_SIZE];
} server_station_snapshot_scratch_t;

static inline void server_emit_station_snapshot(
    world_t *w,
    bool include_world_stations,
    server_packet_sink_fn send,
    void *send_user,
    server_station_snapshot_scratch_t *scratch) {
    if (!w || !send || !scratch) return;

    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;

        int id_len = serialize_station_identity(
            scratch->station_identity, s, st);
        send(send_user, scratch->station_identity, id_len);

        int diag_len = serialize_station_diag(scratch->station_diag, s, st);
        send(send_user, scratch->station_diag, diag_len);

        int ingot_len = serialize_station_ingots(
            scratch->station_ingots, s, st);
        send(send_user, scratch->station_ingots, ingot_len);

        int manifest_len = serialize_station_manifest(
            scratch->station_manifest, s, st);
        send(send_user, scratch->station_manifest, manifest_len);
    }

    if (include_world_stations) {
        int station_len = serialize_stations(
            scratch->world_stations, w->stations);
        send(send_user, scratch->world_stations, station_len);
    }
}

static inline void server_emit_private_snapshot_for_player(
    world_t *w,
    int player_slot,
    server_packet_sink_fn send,
    void *send_user,
    server_private_snapshot_scratch_t *scratch) {
    if (!w || !send || !scratch) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &w->players[player_slot];
    if (!sp->connected) return;

    int ship_len = serialize_player_ship_for_world(
        scratch->player_ship, (uint8_t)player_slot, w, sp);
    send(send_user, scratch->player_ship, ship_len);

    int hold_len = serialize_hold_ingots(scratch->hold_ingots, &sp->ship);
    send(send_user, scratch->hold_ingots, hold_len);

    int manifest_len = serialize_player_manifest(
        scratch->player_manifest, &sp->ship);
    send(send_user, scratch->player_manifest, manifest_len);

    int inspect_len = serialize_inspect_snapshot_for_world(
        scratch->inspect_snapshot, w, sp);
    send(send_user, scratch->inspect_snapshot, inspect_len);

    int known_len = serialize_player_known_contracts(
        scratch->known_contracts, w->contracts, &sp->ship);
    send(send_user, scratch->known_contracts, known_len);

    int delivery_len = serialize_delivery_ledger(
        scratch->delivery_ledger, w, (uint8_t)player_slot);
    send(send_user, scratch->delivery_ledger, delivery_len);
}

/* ------------------------------------------------------------------ */
/* Deserialisation (client -> server)                                 */
/* ------------------------------------------------------------------ */

/*
 * INPUT message (4, 5, 8, 12, 14, or 18 bytes):
 * [type:1][flags:1][action:1][mining_target:1][buy_grade:1 (optional)]
 * Older clients send 4 bytes — buy_grade is treated as MINING_GRADE_COUNT
 * ("any grade, FIFO"). Only meaningful when action is in the
 * NET_ACTION_BUY_PRODUCT range.
 *
 * Current clients send 18 bytes. Bytes 8..9 carry a client input sequence
 * number, bytes 10..11 carry a uint16 mining target (0xFFFF = none), and
 * bytes 14..17 carry the client-predicted sim tick for movement application.
 * Byte 3 remains the low byte / legacy target sentinel for old servers.
 * Newer clients append bytes 12..13 as a uint16 action id. The server
 * uses it to drop duplicate one-shot actions while still accepting the
 * packet's latest continuous movement flags.
 */
static inline uint16_t input_action_id(const uint8_t *data, int len) {
    if (!data || len < 14) return 0;
    return read_u16_le(&data[12]);
}

static inline uint32_t input_client_tick(const uint8_t *data, int len) {
    if (!data || len < 18) return 0;
    return read_u32_le(&data[14]);
}

static inline void parse_input(const uint8_t *data, int len, input_intent_t *intent) {
    if (len < 4) return;
    intent->mining_target_hint = -1;
    /* Default buy_grade each message; the BUY branch below overrides if
     * the 5th byte is a valid grade index. */
    intent->buy_grade = MINING_GRADE_COUNT;
    uint8_t flags = data[1];

    /* Overwrite continuous inputs every message. */
    if (flags & NET_INPUT_THRUST)
        intent->thrust = 1.0f;
    else if (flags & NET_INPUT_BRAKE)
        intent->thrust = -1.0f;
    else
        intent->thrust = 0.0f;
    intent->reverse_thrust = (flags & NET_INPUT_BRAKE) && (flags & NET_INPUT_REVERSE);
    intent->turn = 0.0f;
    if ((flags & NET_INPUT_LEFT) && !(flags & NET_INPUT_RIGHT))
        intent->turn = 1.0f;
    else if ((flags & NET_INPUT_RIGHT) && !(flags & NET_INPUT_LEFT))
        intent->turn = -1.0f;
    intent->mine = (flags & NET_INPUT_FIRE) != 0;
    intent->tractor_hold = (flags & NET_INPUT_TRACTOR) != 0;
    intent->boost = (flags & NET_INPUT_BOOST) != 0;

    /* One-shot actions — accumulate until the sim consumes them. */
    {
        uint8_t action = data[2];
        switch (action) {
        case NET_ACTION_DOCK:
            intent->dock = true;
            intent->interact = true;
            break;
        case NET_ACTION_LAUNCH:
            intent->launch = true;
            intent->interact = true;
            break;
        case NET_ACTION_SELL_CARGO:
            intent->service_sell = true;
            break;
        case NET_ACTION_REPAIR:
            intent->service_repair = true;
            break;
        case NET_ACTION_UPGRADE_MINING:
            intent->upgrade_mining = true;
            break;
        case NET_ACTION_UPGRADE_HOLD:
            intent->upgrade_hold = true;
            break;
        case NET_ACTION_UPGRADE_TRACTOR:
            intent->upgrade_tractor = true;
            break;
        case NET_ACTION_PLACE_OUTPOST:
            intent->place_outpost = true;
            /* Reticle target tuple rides at bytes 5..7 when the client
             * picked a specific (station, ring, slot). 0xFF means "no
             * target — let the server auto-snap" (the relay-founding
             * path). Older clients only send 5 bytes, in which case we
             * fall back to the auto-snap default. */
            if (len >= 8) {
                int8_t s = (int8_t)data[5];
                int8_t r = (int8_t)data[6];
                int8_t l = (int8_t)data[7];
                intent->place_target_station = s;
                intent->place_target_ring    = r;
                intent->place_target_slot    = l;
            } else {
                intent->place_target_station = -1;
                intent->place_target_ring    = -1;
                intent->place_target_slot    = -1;
            }
            break;
        case NET_ACTION_BUY_SCAFFOLD:
            intent->buy_scaffold_kit = true;
            break;
        case NET_ACTION_PLACE_MODULE:
            /* Legacy: no-op (module placement now via towed scaffold + reticle) */
            break;
        case NET_ACTION_HAIL:
            intent->hail = true;
            break;
        case NET_ACTION_RELEASE_TOW:
            intent->release_tow = true;
            break;
        case NET_ACTION_RESET:
            intent->reset = true;
            break;
        case NET_ACTION_AUTOPILOT_TOGGLE:
            intent->toggle_autopilot = true;
            break;
        default:
            /* NET_ACTION_BUILD_MODULE legacy: no-op (range collapsed) */
            /* NET_ACTION_BUY_PRODUCT + commodity (30..30+COMMODITY_COUNT) */
            if (action >= NET_ACTION_BUY_PRODUCT && action < NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT) {
                intent->buy_product = true;
                intent->buy_commodity = (commodity_t)(action - NET_ACTION_BUY_PRODUCT);
                if (len >= 5) {
                    uint8_t g = data[4];
                    /* Valid grades are 0..MINING_GRADE_COUNT-1; pass the
                     * sentinel MINING_GRADE_COUNT through so "any" stays
                     * distinct from a concrete grade in transfer helpers. */
                    if (g <= MINING_GRADE_COUNT) intent->buy_grade = (mining_grade_t)g;
                }
            }
            /* NET_ACTION_BUY_SCAFFOLD_TYPED + module_type (50..50+MODULE_COUNT) */
            else if (action >= NET_ACTION_BUY_SCAFFOLD_TYPED && action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) {
                intent->buy_scaffold_kit = true;
                intent->scaffold_kit_module = (module_type_t)(action - NET_ACTION_BUY_SCAFFOLD_TYPED);
            }
            /* NET_ACTION_DELIVER_COMMODITY + commodity (70..70+COMMODITY_COUNT)
             * — selective fulfillment. Two callers share this action:
             *   (a) Yard contracts tab [S] → sells the next matching
             *       pod or legacy cargo batch. 4-byte message: 5th byte
             *       defaults to MINING_GRADE_COUNT below.
             *   (b) Trade tab per-row sell click → sells one unit of a
             *       specific (commodity, grade). 5-byte message with
             *       the 5th byte holding a grade index < GRADE_COUNT.
             * The grade slot doubles as the sell-one switch: a real
             * grade ⇒ single-unit sell, MINING_GRADE_COUNT ⇒ bulk. */
            else if (action >= NET_ACTION_DELIVER_COMMODITY && action < NET_ACTION_DELIVER_COMMODITY + COMMODITY_COUNT) {
                intent->service_sell = true;
                intent->service_sell_only = (commodity_t)(action - NET_ACTION_DELIVER_COMMODITY);
                intent->service_sell_grade = MINING_GRADE_COUNT;
                intent->service_sell_one = false;
                if (len >= 5) {
                    uint8_t g = data[4];
                    if (g < MINING_GRADE_COUNT) {
                        intent->service_sell_grade = (mining_grade_t)g;
                        intent->service_sell_one = true;
                    }
                }
            }
            else if (action >= NET_ACTION_COMMISSION_SHIP &&
                     action < NET_ACTION_COMMISSION_SHIP + HULL_CLASS_COUNT) {
                intent->commission_ship = true;
                intent->commission_hull_class =
                    (hull_class_t)(action - NET_ACTION_COMMISSION_SHIP);
            }
            break;
        }
    }

    /* Mining target hint. V2 clients send uint16 at bytes 10..11 so all
     * MAX_ASTEROIDS slots fit; legacy clients only had byte 3. */
    {
        if (len >= 12) {
            uint16_t target = read_u16_le(&data[10]);
            intent->mining_target_hint =
                (target == 0xFFFFu || target >= MAX_ASTEROIDS) ? -1 : (int)target;
        } else {
            uint8_t target = data[3];
            intent->mining_target_hint = (target == 0xFF) ? -1 : (int)target;
        }
    }
}

/*
 * PLAN message (NET_PLAN_MSG_SIZE bytes):
 * [type:1][op:1][station:1][ring:1][slot:1][module_type:1][px:f32][py:f32]
 *
 * Accumulates onto the player's pending intent so the next sim step can
 * apply it. Plan ops can stack across messages without colliding with the
 * one-shot action byte in NET_MSG_INPUT.
 */
static inline void parse_plan(const uint8_t *data, int len, input_intent_t *intent) {
    if (len < NET_PLAN_MSG_SIZE) return;
    uint8_t op = data[1];
    int8_t station = (int8_t)data[2];
    int8_t ring = (int8_t)data[3];
    int8_t slot = (int8_t)data[4];
    uint8_t mtype = data[5];
    float px, py;
    memcpy(&px, &data[6], 4);
    memcpy(&py, &data[10], 4);

    switch (op) {
    case NET_PLAN_OP_CREATE_OUTPOST:
        intent->create_planned_outpost = true;
        intent->planned_outpost_pos.x = px;
        intent->planned_outpost_pos.y = py;
        break;
    case NET_PLAN_OP_ADD_SLOT:
        if ((int)mtype < MODULE_COUNT) {
            intent->add_plan = true;
            intent->plan_station = station;
            intent->plan_ring = ring;
            intent->plan_slot = slot;
            intent->plan_type = (module_type_t)mtype;
        }
        break;
    case NET_PLAN_OP_CANCEL_OUTPOST:
        intent->cancel_planned_outpost = true;
        intent->cancel_planned_station = station;
        break;
    case NET_PLAN_OP_CREATE_AND_ADD:
        /* Atomic create + first plan: born at (px,py), plan at ring/slot/type.
         * plan_station=-2 is a sentinel resolved at processing time. */
        intent->create_planned_outpost = true;
        intent->planned_outpost_pos.x = px;
        intent->planned_outpost_pos.y = py;
        if ((int)mtype < MODULE_COUNT) {
            intent->add_plan = true;
            intent->plan_station = -2; /* sentinel: just-created station */
            intent->plan_ring = ring;
            intent->plan_slot = slot;
            intent->plan_type = (module_type_t)mtype;
        }
        break;
    case NET_PLAN_OP_CANCEL_PLAN_SLOT:
        intent->cancel_plan_slot = true;
        intent->cancel_plan_st = station;
        intent->cancel_plan_ring = ring;
        intent->cancel_plan_sl = slot;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Event broadcast serialization                                       */
/* ------------------------------------------------------------------ */

/* Serialize all events from the current sim step into a NET_MSG_EVENTS
 * packet. HAIL_RESPONSE has its own per-recipient message and is
 * skipped here. DEATH is broadcast in stripped form (killer_token +
 * cause + victim id only) so the killer can render a kill confirm
 * even though the cinematic payload still goes to the victim via
 * NET_MSG_DEATH. Returns total packet length. */
static inline int serialize_events(uint8_t *buf, const sim_events_t *events) {
    int count = 0;
    for (int i = 0; i < events->count; i++) {
        const sim_event_t *ev = &events->events[i];
        /* Skip types that already have dedicated messages */
        if (ev->type == SIM_EVENT_HAIL_RESPONSE) continue;

        uint8_t *p = &buf[2 + count * NET_EVENT_RECORD_SIZE];
        memset(p, 0, NET_EVENT_RECORD_SIZE);
        p[0] = (uint8_t)ev->type;
        p[1] = (uint8_t)ev->player_id;

        switch (ev->type) {
        case SIM_EVENT_FRACTURE:
            p[2] = (uint8_t)ev->fracture.tier;
            break;
        case SIM_EVENT_PICKUP:
            write_f32_le(&p[2], ev->pickup.ore);
            p[6] = (uint8_t)ev->pickup.fragments;
            break;
        case SIM_EVENT_UPGRADE:
            p[2] = (uint8_t)ev->upgrade.upgrade;
            break;
        case SIM_EVENT_DAMAGE:
            write_f32_le(&p[2],  ev->damage.amount);
            write_f32_le(&p[6],  ev->damage.source_x);
            write_f32_le(&p[10], ev->damage.source_y);
            break;
        case SIM_EVENT_NPC_KILL:
            p[2] = ev->npc_kill.cause;
            p[3] = ev->npc_kill.npc_role;
            memcpy(&p[4], ev->npc_kill.killer_token, 8);
            break;
        case SIM_EVENT_DEATH:
            /* Broadcast the killer-attribution slice only; the
             * victim's cinematic payload travels in NET_MSG_DEATH. */
            p[2] = ev->death.cause;
            memcpy(&p[3], ev->death.killer_token, 8);
            break;
        case SIM_EVENT_OUTPOST_PLACED:
            p[2] = (uint8_t)ev->outpost_placed.slot;
            break;
        case SIM_EVENT_OUTPOST_ACTIVATED:
            p[2] = (uint8_t)ev->outpost_activated.slot;
            break;
        case SIM_EVENT_MODULE_ACTIVATED:
            p[2] = (uint8_t)ev->module_activated.station;
            p[3] = (uint8_t)ev->module_activated.module_idx;
            p[4] = (uint8_t)ev->module_activated.module_type;
            break;
        case SIM_EVENT_NPC_SPAWNED:
            p[2] = (uint8_t)ev->npc_spawned.slot;
            p[3] = (uint8_t)ev->npc_spawned.role;
            p[4] = (uint8_t)ev->npc_spawned.home_station;
            break;
        case SIM_EVENT_STATION_CONNECTED:
            p[2] = (uint8_t)ev->station_connected.connected_count;
            break;
        case SIM_EVENT_CONTRACT_COMPLETE:
            p[2] = (uint8_t)ev->contract_complete.action;
            break;
        case SIM_EVENT_SCAFFOLD_READY:
            p[2] = (uint8_t)ev->scaffold_ready.station;
            p[3] = (uint8_t)ev->scaffold_ready.module_type;
            break;
        case SIM_EVENT_SELL:
            p[2] = (uint8_t)ev->sell.station;
            p[3] = (uint8_t)ev->sell.grade;
            write_u32_le(&p[4], (uint32_t)ev->sell.base_cr);
            write_u32_le(&p[8], (uint32_t)ev->sell.bonus_cr);
            p[12] = ev->sell.by_contract;
            break;
        case SIM_EVENT_BUY:
            p[2] = (uint8_t)ev->buy.station;
            p[3] = ev->buy.commodity;
            p[4] = ev->buy.grade;
            write_u32_le(&p[5], (uint32_t)ev->buy.cost);
            write_u16_le(&p[9], ev->buy.quantity);
            break;
        case SIM_EVENT_ORDER_REJECTED:
            p[2] = ev->order_rejected.reason;
            break;
        default:
            /* MINING_TICK, DOCK, LAUNCH, REPAIR, SIGNAL_LOST:
             * type + player_id is sufficient */
            break;
        }
        count++;
    }
    buf[0] = NET_MSG_EVENTS;
    buf[1] = (uint8_t)count;
    return 2 + count * NET_EVENT_RECORD_SIZE;
}

#endif /* NET_PROTOCOL_H */
