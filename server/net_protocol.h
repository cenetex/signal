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

static inline uint64_t net_fnv1a64_update(uint64_t h, uint8_t byte) {
    h ^= (uint64_t)byte;
    return h * 1099511628211ull;
}

static inline uint64_t net_payload_hash(const uint8_t *data, size_t len) {
    uint64_t h = 1469598103934665603ull;
    if (!data) return h;
    for (size_t i = 0; i < len; i++)
        h = net_fnv1a64_update(h, data[i]);
    return h;
}

static inline bool net_payload_cache_should_send(net_payload_cache_t *cache,
                                                 void *conn,
                                                 const uint8_t *data,
                                                 size_t len) {
    if (!cache || !data || len > UINT16_MAX)
        return true;
    uint64_t hash = net_payload_hash(data, len);
    if (cache->valid &&
        cache->conn == conn &&
        cache->len == (uint16_t)len &&
        cache->hash == hash) {
        return false;
    }
    cache->valid = true;
    cache->conn = conn;
    cache->len = (uint16_t)len;
    cache->hash = hash;
    return true;
}

static inline int net_station_identity_arm_rotation_offset(void) {
    return 59 + COMMODITY_COUNT * 4 + 4
        + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE
        + 1 + MAX_ARMS * 4 + MAX_ARMS * 4;
}

static inline int net_station_identity_arm_drift_end_offset(void) {
    return net_station_identity_arm_rotation_offset()
        + MAX_ARMS * 4 + MAX_ARMS * 4;
}

static inline uint8_t server_player_state_flags_for_wire(
    const server_player_t *sp) {
    if (!sp) return 0;
    uint8_t flags = 0;
    if (sp->actual_thrusting) flags |= 1;
    if (sp->beam_active) flags |= 2;
    if (sp->docked) flags |= 4;
    if (sp->scan_active) flags |= 8;
    if (sp->ship.tractor_active) flags |= 16;
    if (sp->beam_ineffective) flags |= 32;
    if (sp->beam_hit) flags |= 64;
    return flags;
}

static inline uint64_t net_station_identity_semantic_hash(const uint8_t *data,
                                                          int len) {
    if (!data || len <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    if (len < net_station_identity_arm_drift_end_offset() ||
        data[0] != NET_MSG_STATION_IDENTITY) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int drift_start = net_station_identity_arm_rotation_offset();
    int drift_end = net_station_identity_arm_drift_end_offset();
    for (int i = 0; i < len; i++) {
        if (i >= drift_start && i < drift_end)
            continue;
        h = net_fnv1a64_update(h, data[i]);
    }
    return h;
}

static inline bool net_msg_is_deferable_snapshot(uint8_t msg) {
    switch (msg) {
    case NET_MSG_WORLD_PLAYERS:
    case NET_MSG_WORLD_PLAYER_MOTION:
    case NET_MSG_WORLD_PLAYER_MOTION_Q:
    case NET_MSG_WORLD_PLAYER_MOTIOND_Q:
    case NET_MSG_WORLD_PLAYER_POSED_Q:
    case NET_MSG_WORLD_PLAYER_MOTIONM_Q:
    case NET_MSG_WORLD_PLAYER_DOCK_Q:
    case NET_MSG_WORLD_NPCS:
    case NET_MSG_WORLD_NPC_MOTION:
    case NET_MSG_WORLD_NPC_MOTION_Q:
    case NET_MSG_WORLD_NPC_MOTION8_Q:
    case NET_MSG_WORLD_NPC_POS_Q:
    case NET_MSG_WORLD_NPC_POSE_Q:
    case NET_MSG_WORLD_NPC_LINEAR_Q:
    case NET_MSG_WORLD_NPC_STATUS:
    case NET_MSG_WORLD_NPC_STATUS8_Q:
    case NET_MSG_WORLD_ASTEROIDS:
    case NET_MSG_WORLD_ASTEROIDS_Q:
    case NET_MSG_WORLD_ASTEROIDS8_Q:
    case NET_MSG_WORLD_ASTEROID_MOTION:
    case NET_MSG_WORLD_ASTEROID_MOTION_Q:
    case NET_MSG_WORLD_ASTEROID_POS_Q:
    case NET_MSG_WORLD_ASTEROID_POS8_Q:
    case NET_MSG_WORLD_ASTEROID_POSD_Q:
    case NET_MSG_WORLD_ASTEROID_POSD8_Q:
    case NET_MSG_WORLD_ASTEROID_STATE_Q:
    case NET_MSG_WORLD_TIME:
    case NET_MSG_WORLD_SCAFFOLDS:
    case NET_MSG_WORLD_SCAFFOLD_MOTION_Q:
    case NET_MSG_WORLD_CARGO_PODS:
    case NET_MSG_WORLD_CARGO_PODS_Q:
    case NET_MSG_WORLD_CARGO_POD_MOTION:
    case NET_MSG_WORLD_CARGO_POD_MOTION_Q:
    case NET_MSG_WORLD_CARGO_POD_LINEAR_Q:
    case NET_MSG_WORLD_INTERACTIONS:
    case NET_MSG_WORLD_INTERACTIONS_Q:
    case NET_MSG_WORLD_INTERACTION_DRIFT:
        return true;
    default:
        return false;
    }
}

static inline bool net_deferable_snapshot_would_backpressure(
    uint8_t msg,
    size_t queued_len,
    size_t frame_len,
    size_t reserve_len) {
    if (frame_len == 0 || !net_msg_is_deferable_snapshot(msg))
        return false;
    return queued_len + frame_len > reserve_len;
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
                                          uint32_t input_tick_ack,
                                          uint32_t client_sent_ms,
                                          uint32_t server_recv_ms,
                                          uint32_t server_send_ms) {
    buf[0] = NET_MSG_INPUT_APPLIED;
    write_u16_le(&buf[1], input_seq);
    write_u32_le(&buf[3], server_tick);
    write_u32_le(&buf[7], input_tick_ack);
    write_u32_le(&buf[11], client_sent_ms);
    write_u32_le(&buf[15], server_recv_ms);
    write_u32_le(&buf[19], server_send_ms);
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
                                         uint32_t server_send_ms,
                                         uint32_t server_tick) {
    buf[0] = NET_MSG_LATENCY_PONG;
    write_u32_le(&buf[1], seq);
    write_u32_le(&buf[5], client_sent_ms);
    write_u32_le(&buf[9], server_recv_ms);
    write_u32_le(&buf[13], server_send_ms);
    write_u32_le(&buf[17], server_tick);
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
    ADD_PROTOCOL_STREAM(NET_MSG_STATION_IDENTITY_Q, PROTOCOL_STREAM_CLASS_STATIC,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        STATION_IDENTITY_Q_HEADER_SIZE, 0, 1,
                        station_identity_fallback_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_STATION_DIAG, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_FIXED_SIZE,
                        3, 1, MAX_MODULES_PER_STATION, station_diag_min_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYERS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT,
                        2, PLAYER_RECORD_SIZE, MAX_PLAYERS, state_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYER_MOTION, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        PLAYER_MOTION_MSG_HEADER, PLAYER_MOTION_RECORD_SIZE,
                        MAX_PLAYERS, state_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYER_MOTION_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        PLAYER_MOTION_Q_MSG_HEADER,
                        PLAYER_MOTION_Q_RECORD_SIZE,
                        MAX_PLAYERS, state_tick_ms * 4u);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYER_MOTIOND_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        PLAYER_MOTIOND_Q_MSG_HEADER,
                        PLAYER_MOTIOND_Q_RECORD_SIZE,
                        MAX_PLAYERS, state_tick_ms * 4u);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYER_POSED_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        PLAYER_POSED_Q_MSG_HEADER,
                        PLAYER_POSED_Q_RECORD_SIZE,
                        MAX_PLAYERS, state_tick_ms * 4u);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYER_MOTIONM_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        PLAYER_MOTIONM_Q_MSG_HEADER, 0,
                        MAX_PLAYERS, state_tick_ms * 4u);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_PLAYER_DOCK_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        PLAYER_DOCK_MSG_HEADER, PLAYER_DOCK_RECORD_SIZE,
                        MAX_PLAYERS, state_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROIDS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        ASTEROID_MSG_HEADER, ASTEROID_RECORD_SIZE,
                        MAX_ASTEROIDS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROIDS_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        ASTEROID_Q_MSG_HEADER, ASTEROID_Q_RECORD_SIZE,
                        MAX_ASTEROIDS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROIDS8_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        ASTEROID8_Q_MSG_HEADER, ASTEROID8_Q_RECORD_SIZE,
                        256, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_MOTION, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        ASTEROID_MOTION_MSG_HEADER,
                        ASTEROID_MOTION_RECORD_SIZE, MAX_ASTEROIDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_MOTION_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        ASTEROID_MOTION_Q_MSG_HEADER,
                        ASTEROID_MOTION_Q_RECORD_SIZE, MAX_ASTEROIDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_POS_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        ASTEROID_POS_Q_MSG_HEADER,
                        ASTEROID_POS_Q_RECORD_SIZE, MAX_ASTEROIDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_POS8_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        ASTEROID_POS8_Q_MSG_HEADER,
                        ASTEROID_POS8_Q_RECORD_SIZE, 256,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_POSD_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        ASTEROID_POSD_Q_MSG_HEADER,
                        ASTEROID_POSD_Q_RECORD_SIZE, MAX_ASTEROIDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_POSD8_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        ASTEROID_POSD8_Q_MSG_HEADER,
                        ASTEROID_POSD8_Q_RECORD_SIZE, 256,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_REMOVE, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        ASTEROID_REMOVE_MSG_HEADER,
                        ASTEROID_REMOVE_RECORD_SIZE, MAX_ASTEROIDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_ASTEROID_STATE_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        ASTEROID_STATE_Q_MSG_HEADER,
                        ASTEROID_STATE_Q_RECORD_SIZE, MAX_ASTEROIDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPCS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, NPC_RECORD_SIZE, MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_MOTION, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_MOTION_MSG_HEADER, NPC_MOTION_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_MOTION_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_MOTION_Q_MSG_HEADER, NPC_MOTION_Q_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_MOTION8_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_MOTION8_Q_MSG_HEADER, NPC_MOTION8_Q_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_POS_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_POS_Q_MSG_HEADER, NPC_POS_Q_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_POSE_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_POSE_Q_MSG_HEADER, NPC_POSE_Q_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_LINEAR_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_LINEAR_Q_MSG_HEADER, NPC_LINEAR_Q_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_STATUS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_STATUS_MSG_HEADER, NPC_STATUS_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_NPC_STATUS8_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        NPC_STATUS8_MSG_HEADER, NPC_STATUS8_RECORD_SIZE,
                        MAX_NPC_SHIPS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_PODS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, CARGO_POD_RECORD_SIZE, MAX_CARGO_PODS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_PODS_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, CARGO_POD_Q_RECORD_SIZE, MAX_CARGO_PODS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_SCAFFOLDS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, SCAFFOLD_RECORD_SIZE, MAX_SCAFFOLDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_SCAFFOLD_MOTION_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        SCAFFOLD_MOTION_Q_MSG_HEADER,
                        SCAFFOLD_MOTION_Q_RECORD_SIZE, MAX_SCAFFOLDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_SCAFFOLD_REMOVE, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        SCAFFOLD_REMOVE_MSG_HEADER,
                        SCAFFOLD_REMOVE_RECORD_SIZE, MAX_SCAFFOLDS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_POD_MOTION, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        CARGO_POD_MOTION_MSG_HEADER,
                        CARGO_POD_MOTION_RECORD_SIZE, MAX_CARGO_PODS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_POD_MOTION_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        CARGO_POD_MOTION_Q_MSG_HEADER,
                        CARGO_POD_MOTION_Q_RECORD_SIZE, MAX_CARGO_PODS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_POD_LINEAR_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        CARGO_POD_LINEAR_Q_MSG_HEADER,
                        CARGO_POD_LINEAR_Q_RECORD_SIZE, MAX_CARGO_PODS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_CARGO_POD_REMOVE, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        CARGO_POD_REMOVE_MSG_HEADER,
                        CARGO_POD_REMOVE_RECORD_SIZE, MAX_CARGO_PODS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_INTERACTIONS, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, INTERACTION_RECORD_SIZE, SIM_MAX_INTERACTIONS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_INTERACTIONS_Q, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        2, INTERACTION_Q_RECORD_SIZE, SIM_MAX_INTERACTIONS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_INTERACTION_DRIFT, PROTOCOL_STREAM_CLASS_LIVE,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER,
                        INTERACTION_DRIFT_MSG_HEADER,
                        INTERACTION_DRIFT_RECORD_SIZE, SIM_MAX_INTERACTIONS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_PLAYER_SHIP, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        PLAYER_SHIP_SIZE, 0, 1, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_STATIONS, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        2, STATION_RECORD_SIZE, MAX_STATIONS, world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_WORLD_STATIONS_Q, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        STATION_Q_HEADER_SIZE, 0, MAX_STATIONS,
                        world_tick_ms);
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
    ADD_PROTOCOL_STREAM(NET_MSG_CONTRACTS_Q, PROTOCOL_STREAM_CLASS_ECON,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY,
                        CONTRACT_Q_HEADER_SIZE, 0, MAX_CONTRACTS,
                        world_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_DELIVERY_LEDGER, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        DELIVERY_LEDGER_HEADER, DELIVERY_LEDGER_RECORD_SIZE,
                        DELIVERY_LEDGER_MAX_RECORDS, ship_tick_ms);
    ADD_PROTOCOL_STREAM(NET_MSG_PLAYER_KNOWN_LEDGER, PROTOCOL_STREAM_CLASS_PLAYER,
                        PROTOCOL_STREAM_FLAG_SERVER_TO_CLIENT |
                        PROTOCOL_STREAM_FLAG_DIRTY_ONLY |
                        PROTOCOL_STREAM_FLAG_PER_PLAYER,
                        PLAYER_KNOWN_LEDGER_HEADER,
                        PLAYER_KNOWN_LEDGER_RECORD_SIZE,
                        PLAYER_KNOWN_LEDGER_MAX_RECORDS, ship_tick_ms);
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
 * STATE message (45 bytes, 55-byte legacy authoritative ack tail,
 * 67-byte current authoritative ack+transport tail):
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
    buf[22] = server_player_state_flags_for_wire(sp);
    buf[23] = (uint8_t)sp->ship.tractor_level;
    buf[24] = sp->ship.towed_count;
    for (int t = 0; t < 10; t++) {
        int16_t fi = (t < sp->ship.towed_count) ? sp->ship.towed_fragments[t] : -1;
        uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        buf[25 + t * 2]     = (uint8_t)(wire & 0xFFu);
        buf[25 + t * 2 + 1] = (uint8_t)(wire >> 8);
    }
    return NET_STATE_MSG_SIZE;
}

static inline int serialize_authoritative_player_state(uint8_t *buf,
                                                       uint8_t id,
                                                       const server_player_t *sp,
                                                       uint32_t server_tick) {
    (void)serialize_player_state(buf, id, sp);
    write_u16_le(&buf[NET_STATE_AUTH_INPUT_ACK_OFFSET],
                 sp->last_input_seq);
    write_u32_le(&buf[NET_STATE_AUTH_SERVER_TICK_OFFSET], server_tick);
    write_u32_le(&buf[NET_STATE_AUTH_INPUT_TICK_OFFSET],
                 sp->last_input_tick);
    write_u32_le(&buf[NET_STATE_AUTH_CLIENT_SENT_MS_OFFSET],
                 sp->last_input_client_sent_ms);
    write_u32_le(&buf[NET_STATE_AUTH_SERVER_RECV_MS_OFFSET],
                 sp->last_input_server_recv_ms);
    write_u32_le(&buf[NET_STATE_AUTH_SERVER_SEND_MS_OFFSET], 0);
    return NET_STATE_AUTH_SIZE;
}

/*
 * WORLD_PLAYERS message (batched):
 * [type:1][count:1] + count * PLAYER_RECORD_SIZE records
 * Per-recipient sends omit the recipient's own record during steady play;
 * private STATE carries the local authoritative baseline/corrections.
 * Each record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1]
 *              [tractor_lvl:1][towed_count:1][towed_frags:20][callsign:7]
 *              [beam_start_x:f32][beam_start_y:f32][beam_end_x:f32][beam_end_y:f32]
 *              [last_input_seq:u16][server_tick:u32][last_input_tick:u32]
 */
/* PLAYER_RECORD_SIZE defined in shared/net_protocol.h */
static inline void serialize_player_state_record(uint8_t *p,
                                                 const server_player_t *sp,
                                                 uint8_t id,
                                                 uint32_t server_tick) {
    p[0] = id;
    write_f32_le(&p[1],  sp->ship.pos.x);
    write_f32_le(&p[5],  sp->ship.pos.y);
    write_f32_le(&p[9],  sp->ship.vel.x);
    write_f32_le(&p[13], sp->ship.vel.y);
    write_f32_le(&p[17], sp->ship.angle);
    uint8_t flags = server_player_state_flags_for_wire(sp);
    /* bit 1 was "beam_active && beam_hit" -- now just "beam_active" so
     * the client can render the beam even when it's firing into empty
     * space (no rock target). beam_hit is implied by the beam_end
     * coords matching a target. */
    p[21] = flags;
    p[22] = (uint8_t)sp->ship.tractor_level;
    p[23] = sp->ship.towed_count;
    for (int t = 0; t < 10; t++) {
        int16_t fi = (t < sp->ship.towed_count) ? sp->ship.towed_fragments[t] : -1;
        uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        p[24 + t * 2]     = (uint8_t)(wire & 0xFFu);
        p[24 + t * 2 + 1] = (uint8_t)(wire >> 8);
    }
    /* Callsign: 7 bytes (e.g. "KRX-472") */
    memcpy(&p[44], sp->callsign, 7);
    /* Beam coordinates -- server-authoritative. The client mirrors these into
     * LOCAL_PLAYER.beam_start/end so autopilot laser visuals work. */
    write_f32_le(&p[51], sp->beam_start.x);
    write_f32_le(&p[55], sp->beam_start.y);
    write_f32_le(&p[59], sp->beam_end.x);
    write_f32_le(&p[63], sp->beam_end.y);
    write_u16_le(&p[67], sp->last_input_seq);
    write_u32_le(&p[69], server_tick);
    write_u32_le(&p[73], sp->last_input_tick);
}

static inline bool server_player_self_world_record_required(
    const server_player_t *sp) {
    return sp && sp->beam_active;
}

static inline int serialize_player_states_except_recipient(
    uint8_t *buf,
    const server_player_t *players,
    int recipient_slot,
    uint32_t server_tick) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == recipient_slot &&
            !server_player_self_world_record_required(&players[i])) {
            continue;
        }
        if (!server_player_is_gameplay_ready(&players[i])) continue;
        uint8_t *p = &buf[2 + count * PLAYER_RECORD_SIZE];
        serialize_player_state_record(p, &players[i], (uint8_t)i, server_tick);
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYERS;
    buf[1] = (uint8_t)count;
    return 2 + count * PLAYER_RECORD_SIZE;
}

static inline int serialize_all_player_states(uint8_t *buf, const server_player_t *players,
                                              uint32_t server_tick) {
    return serialize_player_states_except_recipient(
        buf, players, -1, server_tick);
}

static inline int serialize_player_motion_for_recipient(
    uint8_t *buf,
    const server_player_t *players,
    int recipient_slot) {
    int count = 0;
    if (!buf || !players) return 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == recipient_slot) continue;
        if (!server_player_is_gameplay_ready(&players[i])) continue;
        if (players[i].docked) continue;

        uint8_t *p = &buf[PLAYER_MOTION_MSG_HEADER +
                          count * PLAYER_MOTION_RECORD_SIZE];
        p[0] = (uint8_t)i;
        write_f32_le(&p[1],  players[i].ship.pos.x);
        write_f32_le(&p[5],  players[i].ship.pos.y);
        write_f32_le(&p[9],  players[i].ship.vel.x);
        write_f32_le(&p[13], players[i].ship.vel.y);
        write_f32_le(&p[17], players[i].ship.angle);
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYER_MOTION;
    buf[1] = (uint8_t)count;
    return PLAYER_MOTION_MSG_HEADER + count * PLAYER_MOTION_RECORD_SIZE;
}

static inline int16_t player_motion_q_encode(float value, float scale) {
    if (!isfinite(value) || scale <= 0.0f) return 0;
    float q = value / scale;
    if (q > 32767.0f) return 32767;
    if (q < -32768.0f) return -32768;
    return (int16_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
}

static inline uint8_t player_motion_q_angle(float angle) {
    if (!isfinite(angle)) return 0;
    float a = fmodf(angle, 2.0f * PI_F);
    if (a < 0.0f) a += 2.0f * PI_F;
    int q = (int)((a / (2.0f * PI_F)) * 256.0f + 0.5f);
    return (uint8_t)(q & 0xFF);
}

static inline bool player_motiond_q_encode_i8(float value,
                                               float scale,
                                               int8_t *out) {
    if (!out || !isfinite(value) || scale <= 0.0f) return false;
    float q = value / scale;
    if (q > 127.0f || q < -128.0f) return false;
    *out = (int8_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
    return true;
}

#define PLAYER_MOTION_NET_MIN_REPEAT_TICKS 24u /* 5 Hz while changing */
#define PLAYER_MOTION_NET_HEARTBEAT_TICKS 240u /* 0.5 Hz predicted safety refresh */
#define PLAYER_MOTION_NET_PREDICT_ERROR_SQ (12.0f * 12.0f)
#define PLAYER_MOTION_NET_ANGLE_STEP_THRESHOLD 4u
#define PLAYER_MOTION_NET_ANGLE_FAST_STEP_THRESHOLD 12u

static inline int serialize_player_motion_q_for_recipient(
    uint8_t *buf,
    const server_player_t *players,
    int recipient_slot) {
    int count = 0;
    if (!buf || !players) return 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == recipient_slot) continue;
        if (!server_player_is_gameplay_ready(&players[i])) continue;
        if (players[i].docked) continue;

        uint8_t *p = &buf[PLAYER_MOTION_Q_MSG_HEADER +
                          count * PLAYER_MOTION_Q_RECORD_SIZE];
        p[0] = (uint8_t)i;
        write_u16_le(&p[1], (uint16_t)player_motion_q_encode(
                         players[i].ship.pos.x, PLAYER_MOTION_Q_POS_SCALE));
        write_u16_le(&p[3], (uint16_t)player_motion_q_encode(
                         players[i].ship.pos.y, PLAYER_MOTION_Q_POS_SCALE));
        write_u16_le(&p[5], (uint16_t)player_motion_q_encode(
                         players[i].ship.vel.x, PLAYER_MOTION_Q_VEL_SCALE));
        write_u16_le(&p[7], (uint16_t)player_motion_q_encode(
                         players[i].ship.vel.y, PLAYER_MOTION_Q_VEL_SCALE));
        p[9] = player_motion_q_angle(players[i].ship.angle);
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYER_MOTION_Q;
    buf[1] = (uint8_t)count;
    return PLAYER_MOTION_Q_MSG_HEADER + count * PLAYER_MOTION_Q_RECORD_SIZE;
}

static inline void server_player_motion_delta_clear_all(server_player_t *sp) {
    if (!sp) return;
    memset(sp->player_motion_delta_valid, 0,
           sizeof(sp->player_motion_delta_valid));
    memset(sp->player_motion_delta_qx, 0,
           sizeof(sp->player_motion_delta_qx));
    memset(sp->player_motion_delta_qy, 0,
           sizeof(sp->player_motion_delta_qy));
    memset(sp->player_motion_delta_vel, 0,
           sizeof(sp->player_motion_delta_vel));
    memset(sp->player_motion_delta_angle, 0,
           sizeof(sp->player_motion_delta_angle));
    memset(sp->player_motion_delta_tick, 0,
           sizeof(sp->player_motion_delta_tick));
    sp->player_motion_delta_heartbeat_tick = 0u;
    sp->world_player_motion_cache.valid = false;
    sp->world_player_motion_delta_cache.valid = false;
    sp->world_player_motion_posed_cache.valid = false;
}

static inline void server_player_motion_delta_clear_slot(server_player_t *sp,
                                                         int slot) {
    if (!sp || slot < 0 || slot >= MAX_PLAYERS) return;
    if (!sp->player_motion_delta_valid[slot]) return;
    sp->player_motion_delta_valid[slot] = false;
    sp->player_motion_delta_qx[slot] = 0;
    sp->player_motion_delta_qy[slot] = 0;
    sp->player_motion_delta_vel[slot] = v2(0.0f, 0.0f);
    sp->player_motion_delta_angle[slot] = 0;
    sp->player_motion_delta_tick[slot] = 0u;
    sp->world_player_motion_cache.valid = false;
    sp->world_player_motion_delta_cache.valid = false;
    sp->world_player_motion_posed_cache.valid = false;
}

static inline void server_player_motion_delta_note_q(server_player_t *sp,
                                                     int slot,
                                                     int16_t qx,
                                                     int16_t qy,
                                                     vec2 vel,
                                                     uint8_t angle,
                                                     uint32_t server_tick) {
    if (!sp || slot < 0 || slot >= MAX_PLAYERS) return;
    sp->player_motion_delta_valid[slot] = true;
    sp->player_motion_delta_qx[slot] = qx;
    sp->player_motion_delta_qy[slot] = qy;
    sp->player_motion_delta_vel[slot] = vel;
    sp->player_motion_delta_angle[slot] = angle;
    sp->player_motion_delta_tick[slot] = server_tick;
}

static inline void server_player_motion_delta_note_abs_msg(
    server_player_t *sp,
    const uint8_t *data,
    size_t len,
    uint32_t server_tick) {
    if (!sp || !data || len < PLAYER_MOTION_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_PLAYER_MOTION_Q) {
        return;
    }
    int count = (int)data[1];
    size_t expected = PLAYER_MOTION_Q_MSG_HEADER +
        (size_t)count * PLAYER_MOTION_Q_RECORD_SIZE;
    if (len < expected) return;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = &data[PLAYER_MOTION_Q_MSG_HEADER +
                                 i * PLAYER_MOTION_Q_RECORD_SIZE];
        uint8_t id = p[0];
        if (id >= MAX_PLAYERS) continue;
        int16_t qx = (int16_t)read_u16_le(&p[1]);
        int16_t qy = (int16_t)read_u16_le(&p[3]);
        int16_t qvx = (int16_t)read_u16_le(&p[5]);
        int16_t qvy = (int16_t)read_u16_le(&p[7]);
        server_player_motion_delta_note_q(
            sp, id, qx, qy,
            v2((float)qvx * PLAYER_MOTION_Q_VEL_SCALE,
               (float)qvy * PLAYER_MOTION_Q_VEL_SCALE),
            p[9], server_tick);
    }
}

static inline void server_player_motion_delta_note_delta_msg(
    server_player_t *sp,
    const uint8_t *data,
    size_t len,
    uint32_t server_tick) {
    if (!sp || !data || len < PLAYER_MOTIOND_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_PLAYER_MOTIOND_Q) {
        return;
    }
    int count = (int)data[1];
    size_t expected = PLAYER_MOTIOND_Q_MSG_HEADER +
        (size_t)count * PLAYER_MOTIOND_Q_RECORD_SIZE;
    if (len < expected) return;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = &data[PLAYER_MOTIOND_Q_MSG_HEADER +
                                 i * PLAYER_MOTIOND_Q_RECORD_SIZE];
        uint8_t id = p[0];
        if (id >= MAX_PLAYERS || !sp->player_motion_delta_valid[id])
            continue;
        int qx = (int)sp->player_motion_delta_qx[id] + (int)(int8_t)p[1];
        int qy = (int)sp->player_motion_delta_qy[id] + (int)(int8_t)p[2];
        if (qx < -32768 || qx > 32767 || qy < -32768 || qy > 32767) {
            server_player_motion_delta_clear_slot(sp, id);
            continue;
        }
        server_player_motion_delta_note_q(
            sp, id, (int16_t)qx, (int16_t)qy,
            v2((float)(int8_t)p[3] * PLAYER_MOTIOND_Q_VEL_SCALE,
               (float)(int8_t)p[4] * PLAYER_MOTIOND_Q_VEL_SCALE),
            p[5], server_tick);
    }
}

static inline void server_player_motion_delta_note_posed_msg(
    server_player_t *sp,
    const uint8_t *data,
    size_t len,
    uint32_t server_tick) {
    if (!sp || !data || len < PLAYER_POSED_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_PLAYER_POSED_Q) {
        return;
    }
    int count = (int)data[1];
    size_t expected = PLAYER_POSED_Q_MSG_HEADER +
        (size_t)count * PLAYER_POSED_Q_RECORD_SIZE;
    if (len < expected) return;
    for (int i = 0; i < count; i++) {
        const uint8_t *p = &data[PLAYER_POSED_Q_MSG_HEADER +
                                 i * PLAYER_POSED_Q_RECORD_SIZE];
        uint8_t id = p[0];
        if (id >= MAX_PLAYERS || !sp->player_motion_delta_valid[id])
            continue;
        int qx = (int)sp->player_motion_delta_qx[id] + (int)(int8_t)p[1];
        int qy = (int)sp->player_motion_delta_qy[id] + (int)(int8_t)p[2];
        if (qx < -32768 || qx > 32767 || qy < -32768 || qy > 32767) {
            server_player_motion_delta_clear_slot(sp, id);
            continue;
        }
        server_player_motion_delta_note_q(
            sp, id, (int16_t)qx, (int16_t)qy,
            sp->player_motion_delta_vel[id],
            p[3], server_tick);
    }
}

static inline void server_player_motion_delta_note_mixed_msg(
    server_player_t *sp,
    const uint8_t *data,
    size_t len,
    uint32_t server_tick) {
    if (!sp || !data || len < PLAYER_MOTIONM_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_PLAYER_MOTIONM_Q) {
        return;
    }
    int count = (int)data[1];
    size_t off = PLAYER_MOTIONM_Q_MSG_HEADER;
    for (int i = 0; i < count; i++) {
        if (off + PLAYER_MOTIONM_Q_POSE_RECORD_SIZE > len)
            return;
        uint8_t id_flags = data[off++];
        uint8_t id = id_flags & PLAYER_MOTIONM_Q_ID_MASK;
        bool has_velocity =
            (id_flags & PLAYER_MOTIONM_Q_FLAG_VEL) != 0;
        bool reserved = (id_flags & PLAYER_MOTIONM_Q_RESERVED_MASK) != 0;
        int8_t dx = (int8_t)data[off++];
        int8_t dy = (int8_t)data[off++];
        vec2 vel = v2(0.0f, 0.0f);
        if (has_velocity) {
            if (off + 3u > len)
                return;
            int8_t qvx = (int8_t)data[off++];
            int8_t qvy = (int8_t)data[off++];
            vel = v2((float)qvx * PLAYER_MOTIOND_Q_VEL_SCALE,
                     (float)qvy * PLAYER_MOTIOND_Q_VEL_SCALE);
        } else {
            if (off + 1u > len)
                return;
        }
        uint8_t angle = data[off++];
        if (reserved || id >= MAX_PLAYERS ||
            !sp->player_motion_delta_valid[id]) {
            continue;
        }
        int qx = (int)sp->player_motion_delta_qx[id] + (int)dx;
        int qy = (int)sp->player_motion_delta_qy[id] + (int)dy;
        if (qx < -32768 || qx > 32767 || qy < -32768 || qy > 32767) {
            server_player_motion_delta_clear_slot(sp, id);
            continue;
        }
        server_player_motion_delta_note_q(
            sp, id, (int16_t)qx, (int16_t)qy,
            has_velocity ? vel : sp->player_motion_delta_vel[id],
            angle, server_tick);
    }
}

static inline uint8_t player_motion_q_angle_delta(uint8_t a, uint8_t b) {
    uint8_t d = (uint8_t)(a - b);
    if (d > 128u) d = (uint8_t)(256u - d);
    return d;
}

static inline bool player_motion_prediction_should_send_impl(
    const server_player_t *recipient,
    int slot,
    int16_t qx,
    int16_t qy,
    uint8_t angle,
    uint32_t server_tick,
    bool heartbeat_due,
    bool coalesced_heartbeat) {
    if (!recipient || slot < 0 || slot >= MAX_PLAYERS)
        return true;
    if (!recipient->player_motion_delta_valid[slot])
        return true;
    uint32_t last_tick = recipient->player_motion_delta_tick[slot];
    if (last_tick == 0u)
        return true;

    uint32_t age_ticks = server_tick - last_tick;
    float dt = (float)age_ticks * SIM_DT;
    float predicted_x =
        (float)recipient->player_motion_delta_qx[slot] *
        PLAYER_MOTION_Q_POS_SCALE +
        recipient->player_motion_delta_vel[slot].x * dt;
    float predicted_y =
        (float)recipient->player_motion_delta_qy[slot] *
        PLAYER_MOTION_Q_POS_SCALE +
        recipient->player_motion_delta_vel[slot].y * dt;
    float current_x = (float)qx * PLAYER_MOTION_Q_POS_SCALE;
    float current_y = (float)qy * PLAYER_MOTION_Q_POS_SCALE;
    float dx = current_x - predicted_x;
    float dy = current_y - predicted_y;
    float error_sq = dx * dx + dy * dy;
    uint8_t angle_delta = player_motion_q_angle_delta(
        angle, recipient->player_motion_delta_angle[slot]);
    if (age_ticks < PLAYER_MOTION_NET_MIN_REPEAT_TICKS)
        return error_sq >= PLAYER_MOTION_NET_PREDICT_ERROR_SQ * 4.0f ||
            angle_delta >= PLAYER_MOTION_NET_ANGLE_FAST_STEP_THRESHOLD;

    if (error_sq >= PLAYER_MOTION_NET_PREDICT_ERROR_SQ)
        return true;

    if (angle_delta >= PLAYER_MOTION_NET_ANGLE_STEP_THRESHOLD)
        return true;

    if (coalesced_heartbeat) {
        if (heartbeat_due &&
            age_ticks >= PLAYER_MOTION_NET_HEARTBEAT_TICKS) {
            return true;
        }
        return false;
    }

    if (age_ticks >= PLAYER_MOTION_NET_HEARTBEAT_TICKS && heartbeat_due)
        return true;

    return false;
}

static inline bool player_motion_prediction_should_send(
    const server_player_t *recipient,
    int slot,
    int16_t qx,
    int16_t qy,
    uint8_t angle,
    uint32_t server_tick) {
    return player_motion_prediction_should_send_impl(
        recipient, slot, qx, qy, angle, server_tick, true, false);
}

static inline bool player_motion_prediction_should_send_coalesced(
    const server_player_t *recipient,
    int slot,
    int16_t qx,
    int16_t qy,
    uint8_t angle,
    uint32_t server_tick,
    bool heartbeat_due) {
    return player_motion_prediction_should_send_impl(
        recipient, slot, qx, qy, angle, server_tick,
        heartbeat_due, true);
}

static inline void serialize_player_motion_q_record(uint8_t *p,
                                                    uint8_t id,
                                                    const server_player_t *sp,
                                                    int16_t qx,
                                                    int16_t qy) {
    p[0] = id;
    write_u16_le(&p[1], (uint16_t)qx);
    write_u16_le(&p[3], (uint16_t)qy);
    write_u16_le(&p[5], (uint16_t)player_motion_q_encode(
                     sp->ship.vel.x, PLAYER_MOTION_Q_VEL_SCALE));
    write_u16_le(&p[7], (uint16_t)player_motion_q_encode(
                     sp->ship.vel.y, PLAYER_MOTION_Q_VEL_SCALE));
    p[9] = player_motion_q_angle(sp->ship.angle);
}

static inline int serialize_player_motion_split_q_for_recipient(
    uint8_t *abs_buf,
    int *abs_len_out,
    uint8_t *delta_buf,
    int *delta_len_out,
    uint8_t *posed_buf,
    int *posed_len_out,
    server_player_t *recipient,
    const server_player_t *players,
    int recipient_slot,
    uint32_t server_tick) {
    int abs_count = 0;
    int delta_count = 0;
    int posed_count = 0;
    if (!abs_buf || !delta_buf || !players) {
        if (abs_len_out) *abs_len_out = 0;
        if (delta_len_out) *delta_len_out = 0;
        if (posed_len_out) *posed_len_out = 0;
        return 0;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == recipient_slot) continue;
        if (!server_player_is_gameplay_ready(&players[i]) ||
            players[i].docked) {
            server_player_motion_delta_clear_slot(recipient, i);
            continue;
        }

        int16_t qx = player_motion_q_encode(
            players[i].ship.pos.x, PLAYER_MOTION_Q_POS_SCALE);
        int16_t qy = player_motion_q_encode(
            players[i].ship.pos.y, PLAYER_MOTION_Q_POS_SCALE);
        uint8_t angle = player_motion_q_angle(players[i].ship.angle);
        int8_t qvx8 = 0;
        int8_t qvy8 = 0;
        bool delta_velocity_ok =
            player_motiond_q_encode_i8(
                players[i].ship.vel.x, PLAYER_MOTIOND_Q_VEL_SCALE, &qvx8) &&
            player_motiond_q_encode_i8(
                players[i].ship.vel.y, PLAYER_MOTIOND_Q_VEL_SCALE, &qvy8);
        if (!player_motion_prediction_should_send(
                recipient, i, qx, qy, angle, server_tick)) {
            continue;
        }
        bool delta_ok = recipient &&
            recipient->player_motion_delta_valid[i] &&
            delta_velocity_ok;
        int dx = 0;
        int dy = 0;
        if (delta_ok) {
            dx = (int)qx - (int)recipient->player_motion_delta_qx[i];
            dy = (int)qy - (int)recipient->player_motion_delta_qy[i];
            delta_ok = dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127;
        }

        int8_t prev_qvx8 = 0;
        int8_t prev_qvy8 = 0;
        bool posed_ok = posed_buf && delta_ok &&
            player_motiond_q_encode_i8(
                recipient->player_motion_delta_vel[i].x,
                PLAYER_MOTIOND_Q_VEL_SCALE, &prev_qvx8) &&
            player_motiond_q_encode_i8(
                recipient->player_motion_delta_vel[i].y,
                PLAYER_MOTIOND_Q_VEL_SCALE, &prev_qvy8) &&
            prev_qvx8 == qvx8 && prev_qvy8 == qvy8;

        if (posed_ok) {
            uint8_t *p = &posed_buf[PLAYER_POSED_Q_MSG_HEADER +
                                    posed_count *
                                    PLAYER_POSED_Q_RECORD_SIZE];
            p[0] = (uint8_t)i;
            p[1] = (uint8_t)(int8_t)dx;
            p[2] = (uint8_t)(int8_t)dy;
            p[3] = angle;
            posed_count++;
        } else if (delta_ok) {
            uint8_t *p = &delta_buf[PLAYER_MOTIOND_Q_MSG_HEADER +
                                    delta_count *
                                    PLAYER_MOTIOND_Q_RECORD_SIZE];
            p[0] = (uint8_t)i;
            p[1] = (uint8_t)(int8_t)dx;
            p[2] = (uint8_t)(int8_t)dy;
            p[3] = (uint8_t)qvx8;
            p[4] = (uint8_t)qvy8;
            p[5] = angle;
            delta_count++;
        } else {
            uint8_t *p = &abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                                  abs_count * PLAYER_MOTION_Q_RECORD_SIZE];
            serialize_player_motion_q_record(
                p, (uint8_t)i, &players[i], qx, qy);
            abs_count++;
        }
    }
    abs_buf[0] = NET_MSG_WORLD_PLAYER_MOTION_Q;
    abs_buf[1] = (uint8_t)abs_count;
    delta_buf[0] = NET_MSG_WORLD_PLAYER_MOTIOND_Q;
    delta_buf[1] = (uint8_t)delta_count;
    if (posed_buf) {
        posed_buf[0] = NET_MSG_WORLD_PLAYER_POSED_Q;
        posed_buf[1] = (uint8_t)posed_count;
    }
    int abs_len = PLAYER_MOTION_Q_MSG_HEADER +
        abs_count * PLAYER_MOTION_Q_RECORD_SIZE;
    int delta_len = PLAYER_MOTIOND_Q_MSG_HEADER +
        delta_count * PLAYER_MOTIOND_Q_RECORD_SIZE;
    int posed_len = PLAYER_POSED_Q_MSG_HEADER +
        posed_count * PLAYER_POSED_Q_RECORD_SIZE;
    if (abs_len_out) *abs_len_out = abs_len;
    if (delta_len_out) *delta_len_out = delta_len;
    if (posed_len_out) *posed_len_out = posed_buf ? posed_len : 0;
    return abs_count + delta_count + posed_count;
}

static inline int serialize_player_motion_mixed_q_for_recipient(
    uint8_t *abs_buf,
    int *abs_len_out,
    uint8_t *mixed_buf,
    int *mixed_len_out,
    bool *heartbeat_due_out,
    server_player_t *recipient,
    const server_player_t *players,
    int recipient_slot,
    uint32_t server_tick) {
    int abs_count = 0;
    int mixed_count = 0;
    int mixed_len = PLAYER_MOTIONM_Q_MSG_HEADER;
    bool heartbeat_due = !recipient ||
        recipient->player_motion_delta_heartbeat_tick == 0u ||
        (uint32_t)(server_tick -
                   recipient->player_motion_delta_heartbeat_tick) >=
            PLAYER_MOTION_NET_HEARTBEAT_TICKS;
    if (heartbeat_due_out) *heartbeat_due_out = heartbeat_due;
    if (!abs_buf || !mixed_buf || !players) {
        if (abs_len_out) *abs_len_out = 0;
        if (mixed_len_out) *mixed_len_out = 0;
        return 0;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == recipient_slot) continue;
        if (!server_player_is_gameplay_ready(&players[i]) ||
            players[i].docked) {
            server_player_motion_delta_clear_slot(recipient, i);
            continue;
        }

        int16_t qx = player_motion_q_encode(
            players[i].ship.pos.x, PLAYER_MOTION_Q_POS_SCALE);
        int16_t qy = player_motion_q_encode(
            players[i].ship.pos.y, PLAYER_MOTION_Q_POS_SCALE);
        uint8_t angle = player_motion_q_angle(players[i].ship.angle);
        int8_t qvx8 = 0;
        int8_t qvy8 = 0;
        bool delta_velocity_ok =
            player_motiond_q_encode_i8(
                players[i].ship.vel.x, PLAYER_MOTIOND_Q_VEL_SCALE, &qvx8) &&
            player_motiond_q_encode_i8(
                players[i].ship.vel.y, PLAYER_MOTIOND_Q_VEL_SCALE, &qvy8);
        if (!player_motion_prediction_should_send_coalesced(
                recipient, i, qx, qy, angle, server_tick,
                heartbeat_due)) {
            continue;
        }
        bool delta_ok = recipient &&
            recipient->player_motion_delta_valid[i] &&
            delta_velocity_ok;
        int dx = 0;
        int dy = 0;
        if (delta_ok) {
            dx = (int)qx - (int)recipient->player_motion_delta_qx[i];
            dy = (int)qy - (int)recipient->player_motion_delta_qy[i];
            delta_ok = dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127;
        }

        int8_t prev_qvx8 = 0;
        int8_t prev_qvy8 = 0;
        bool posed_ok = delta_ok &&
            player_motiond_q_encode_i8(
                recipient->player_motion_delta_vel[i].x,
                PLAYER_MOTIOND_Q_VEL_SCALE, &prev_qvx8) &&
            player_motiond_q_encode_i8(
                recipient->player_motion_delta_vel[i].y,
                PLAYER_MOTIOND_Q_VEL_SCALE, &prev_qvy8) &&
            prev_qvx8 == qvx8 && prev_qvy8 == qvy8;

        if (delta_ok) {
            uint8_t *p = &mixed_buf[mixed_len];
            if (posed_ok) {
                p[0] = (uint8_t)i;
                p[1] = (uint8_t)(int8_t)dx;
                p[2] = (uint8_t)(int8_t)dy;
                p[3] = angle;
                mixed_len += PLAYER_MOTIONM_Q_POSE_RECORD_SIZE;
            } else {
                p[0] = (uint8_t)i | PLAYER_MOTIONM_Q_FLAG_VEL;
                p[1] = (uint8_t)(int8_t)dx;
                p[2] = (uint8_t)(int8_t)dy;
                p[3] = (uint8_t)qvx8;
                p[4] = (uint8_t)qvy8;
                p[5] = angle;
                mixed_len += PLAYER_MOTIONM_Q_VEL_RECORD_SIZE;
            }
            mixed_count++;
        } else {
            uint8_t *p = &abs_buf[PLAYER_MOTION_Q_MSG_HEADER +
                                  abs_count * PLAYER_MOTION_Q_RECORD_SIZE];
            serialize_player_motion_q_record(
                p, (uint8_t)i, &players[i], qx, qy);
            abs_count++;
        }
    }
    abs_buf[0] = NET_MSG_WORLD_PLAYER_MOTION_Q;
    abs_buf[1] = (uint8_t)abs_count;
    mixed_buf[0] = NET_MSG_WORLD_PLAYER_MOTIONM_Q;
    mixed_buf[1] = (uint8_t)mixed_count;
    int abs_len = PLAYER_MOTION_Q_MSG_HEADER +
        abs_count * PLAYER_MOTION_Q_RECORD_SIZE;
    if (abs_len_out) *abs_len_out = abs_len;
    if (mixed_len_out) *mixed_len_out = mixed_len;
    return abs_count + mixed_count;
}

static inline int serialize_player_dock_status_for_recipient(
    uint8_t *buf,
    const server_player_t *players,
    int recipient_slot) {
    int count = 0;
    if (!buf || !players) return 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == recipient_slot) continue;
        if (!server_player_is_gameplay_ready(&players[i])) continue;

        uint8_t *p = &buf[PLAYER_DOCK_MSG_HEADER +
                          count * PLAYER_DOCK_RECORD_SIZE];
        p[0] = (uint8_t)i;
        p[1] = server_player_state_flags_for_wire(&players[i]) &
            PLAYER_DOCK_STATUS_FLAGS_MASK;
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYER_DOCK_Q;
    buf[1] = (uint8_t)count;
    return PLAYER_DOCK_MSG_HEADER + count * PLAYER_DOCK_RECORD_SIZE;
}

static inline uint64_t net_world_players_semantic_hash(const uint8_t *data,
                                                       int len) {
    if (!data || len <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    if (len < 2 || data[0] != NET_MSG_WORLD_PLAYERS) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int count = data[1];
    int expected = 2 + count * PLAYER_RECORD_SIZE;
    if (len < expected) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    for (int i = 0; i < len; i++) {
        bool ignored_byte = false;
        if (i >= 2 && i < expected) {
            int record_off = (i - 2) % PLAYER_RECORD_SIZE;
            bool input_ack_tail_byte = record_off >= 67 && record_off < 77;
            bool pose_byte = record_off >= 1 && record_off < 21;
            ignored_byte = input_ack_tail_byte || pose_byte;
            if (record_off == 21) {
                h = net_fnv1a64_update(
                    h,
                    (uint8_t)(data[i] &
                              (uint8_t)~PLAYER_DOCK_STATUS_FLAGS_MASK));
                continue;
            }
        }
        if (!ignored_byte)
            h = net_fnv1a64_update(h, data[i]);
    }
    return h;
}

#define WORLD_PLAYERS_SEMANTIC_HEARTBEAT_MS 16000ull

static inline bool world_players_semantic_heartbeat_due(uint64_t last_sent_ms,
                                                        uint64_t now_ms) {
    return last_sent_ms == 0 ||
        now_ms - last_sent_ms >= WORLD_PLAYERS_SEMANTIC_HEARTBEAT_MS;
}

/*
 * WORLD_ASTEROIDS message (v2 — uint16 indices):
 * [type:1][count:2] + count * ASTEROID_RECORD_SIZE-byte records
 * Record: [index:2][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32]
 * [radius:f32][smelt:u8][grade:u8][crystal_stage:u8][phase:u8]
 */
/* Per-player asteroid delta serialization with relevance filtering. Static
 * already-seen rocks are omitted; clients dead-reckon moving rocks between
 * authoritative records and receive compact inactive-removal records on view
 * exit. */
#define ASTEROID_VIEW_RADIUS_SQ (3000.0f * 3000.0f)
#define ASTEROID_NET_MOVING_SPEED_SQ (0.05f * 0.05f)
#define ASTEROID_NET_CRAWL_SPEED_SQ (1.0f * 1.0f)
#define ASTEROID_NET_SLOW_SPEED_SQ (10.0f * 10.0f)
#define ASTEROID_NET_MOVING_REPEAT_TICKS 36u /* ~3.3 Hz at the 120 Hz sim tick */
#define ASTEROID_NET_TOWED_MOVING_REPEAT_TICKS 12u /* 10 Hz for tow-chain rocks */
#define ASTEROID_NET_CRAWL_MOVING_REPEAT_TICKS 240u /* 0.5 Hz for sub-pixel drift */
#define ASTEROID_NET_FAR_MOVING_REPEAT_TICKS 120u /* 1 Hz for far-field drift */
#define ASTEROID_NET_FAR_SLOW_MOVING_REPEAT_TICKS 240u /* 0.5 Hz ordinary far-field drift */
#define ASTEROID_NET_VERY_FAR_MOVING_REPEAT_TICKS 360u /* 0.33 Hz for fast edge-of-view drift */
#define ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS 1200u /* 0.1 Hz ordinary edge drift */
#define ASTEROID_NET_MOTION_HEARTBEAT_TICKS 240u /* 0.5 Hz safety refresh */
#define ASTEROID_NET_CRAWL_MOTION_HEARTBEAT_TICKS 4800u /* 0.025 Hz crawl safety refresh */
#define ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS 720u /* 0.17 Hz slow near safety refresh */
#define ASTEROID_NET_FAR_MOTION_HEARTBEAT_TICKS 720u /* 0.17 Hz far-field safety refresh */
#define ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS 1200u /* 0.1 Hz ordinary far safety refresh */
#define ASTEROID_NET_VERY_FAR_MOTION_HEARTBEAT_TICKS 2400u /* 0.05 Hz edge safety refresh */
#define ASTEROID_NET_INTERACTION_RADIUS_SQ (600.0f * 600.0f)
#define ASTEROID_NET_NEAR_RADIUS_SQ (1200.0f * 1200.0f)
#define ASTEROID_NET_FAST_RADIUS_SQ (1800.0f * 1800.0f)
#define ASTEROID_NET_VERY_FAR_RADIUS_SQ (2000.0f * 2000.0f)
#define ASTEROID_NET_BACKGROUND_IDENTITY_BUDGET_PER_BURST 8
#define ASTEROID_NET_BACKGROUND_IDENTITY_BURST_TICKS 4u
#define ASTEROID_NET_FAST_SPEED_SQ (30.0f * 30.0f)
#define ASTEROID_NET_NEAR_PREDICT_ERROR_SQ (16.0f * 16.0f)
#define ASTEROID_NET_TOWED_PREDICT_ERROR_SQ (5.0f * 5.0f)
#define ASTEROID_NET_NEAR_SLOW_PREDICT_ERROR_SQ (32.0f * 32.0f)
#define ASTEROID_NET_CRAWL_PREDICT_ERROR_SQ (64.0f * 64.0f)
#define ASTEROID_NET_FAR_PREDICT_ERROR_SQ (48.0f * 48.0f)
#define ASTEROID_NET_FAR_SLOW_PREDICT_ERROR_SQ (80.0f * 80.0f)
#define ASTEROID_NET_VERY_FAR_PREDICT_ERROR_SQ (128.0f * 128.0f)
#define ASTEROID_NET_POS_ONLY_DRIFT_WINDOW_SEC 0.5f
#define ASTEROID_NET_POS_ONLY_NEAR_DRIFT_SQ (4.0f * 4.0f)
#define ASTEROID_NET_POS_ONLY_FAR_DRIFT_SQ (8.0f * 8.0f)
#define ASTEROID_NET_POS_ONLY_VERY_FAR_DRIFT_SQ (12.0f * 12.0f)
#define ASTEROID_STATE_Q_NUMERIC_REPEAT_TICKS 240u /* 0.5 Hz hp/ore/smelt drift */
#define ASTEROID_STATE_Q_HEARTBEAT_TICKS 960u /* 8s exact-state refresh */

static inline bool asteroid_net_high_detail(float dist_sq, float speed_sq) {
    return dist_sq <= ASTEROID_NET_INTERACTION_RADIUS_SQ ||
        (speed_sq >= ASTEROID_NET_FAST_SPEED_SQ &&
         dist_sq <= ASTEROID_NET_FAST_RADIUS_SQ);
}

static inline bool asteroid_net_tow_lifecycle_high_detail(
    const asteroid_t *a,
    float dist_sq) {
    return a && a->active && a->tier == ASTEROID_TIER_S &&
        a->fracture_child && a->last_towed_by >= 0 &&
        dist_sq <= ASTEROID_NET_NEAR_RADIUS_SQ;
}

static inline uint32_t asteroid_net_moving_repeat_ticks(float dist_sq,
                                                        float speed_sq) {
    if (speed_sq < ASTEROID_NET_CRAWL_SPEED_SQ) {
        return ASTEROID_NET_CRAWL_MOVING_REPEAT_TICKS;
    }
    if (asteroid_net_high_detail(dist_sq, speed_sq)) {
        return ASTEROID_NET_MOVING_REPEAT_TICKS;
    }
    if (dist_sq >= ASTEROID_NET_VERY_FAR_RADIUS_SQ) {
        return speed_sq >= ASTEROID_NET_FAST_SPEED_SQ
            ? ASTEROID_NET_VERY_FAR_MOVING_REPEAT_TICKS
            : ASTEROID_NET_VERY_FAR_SLOW_MOVING_REPEAT_TICKS;
    }
    if (speed_sq < ASTEROID_NET_FAST_SPEED_SQ)
        return ASTEROID_NET_FAR_SLOW_MOVING_REPEAT_TICKS;
    return ASTEROID_NET_FAR_MOVING_REPEAT_TICKS;
}

static inline uint32_t asteroid_net_motion_heartbeat_ticks(float dist_sq,
                                                           float speed_sq) {
    if (speed_sq < ASTEROID_NET_CRAWL_SPEED_SQ) {
        return ASTEROID_NET_CRAWL_MOTION_HEARTBEAT_TICKS;
    }
    if (asteroid_net_high_detail(dist_sq, speed_sq)) {
        return speed_sq < ASTEROID_NET_SLOW_SPEED_SQ
            ? ASTEROID_NET_SLOW_MOTION_HEARTBEAT_TICKS
            : ASTEROID_NET_MOTION_HEARTBEAT_TICKS;
    }
    if (dist_sq >= ASTEROID_NET_VERY_FAR_RADIUS_SQ &&
        speed_sq < ASTEROID_NET_FAST_SPEED_SQ) {
        return ASTEROID_NET_VERY_FAR_MOTION_HEARTBEAT_TICKS;
    }
    if (speed_sq < ASTEROID_NET_FAST_SPEED_SQ)
        return ASTEROID_NET_FAR_SLOW_MOTION_HEARTBEAT_TICKS;
    return ASTEROID_NET_FAR_MOTION_HEARTBEAT_TICKS;
}

static inline float asteroid_net_predict_error_sq(float dist_sq,
                                                  float speed_sq) {
    if (speed_sq < ASTEROID_NET_CRAWL_SPEED_SQ) {
        return ASTEROID_NET_CRAWL_PREDICT_ERROR_SQ;
    }
    if (asteroid_net_high_detail(dist_sq, speed_sq)) {
        return speed_sq < ASTEROID_NET_SLOW_SPEED_SQ
            ? ASTEROID_NET_NEAR_SLOW_PREDICT_ERROR_SQ
            : ASTEROID_NET_NEAR_PREDICT_ERROR_SQ;
    }
    if (dist_sq >= ASTEROID_NET_VERY_FAR_RADIUS_SQ) {
        return ASTEROID_NET_VERY_FAR_PREDICT_ERROR_SQ;
    }
    if (speed_sq < ASTEROID_NET_FAST_SPEED_SQ)
        return ASTEROID_NET_FAR_SLOW_PREDICT_ERROR_SQ;
    return ASTEROID_NET_FAR_PREDICT_ERROR_SQ;
}

static inline int asteroid_net_background_identity_budget_at_tick(
    uint32_t server_tick) {
    return (server_tick % ASTEROID_NET_BACKGROUND_IDENTITY_BURST_TICKS) == 0u
        ? ASTEROID_NET_BACKGROUND_IDENTITY_BUDGET_PER_BURST
        : 0;
}

static inline int asteroid_net_background_identity_budget_at_tick_for_players(
    uint32_t server_tick,
    int live_asteroid_recipient_count) {
    int base = asteroid_net_background_identity_budget_at_tick(server_tick);
    if (base <= 0) return 0;
    if (live_asteroid_recipient_count <= 4) return base;
    if (live_asteroid_recipient_count <= 8) return (base + 1) / 2;
    if (live_asteroid_recipient_count <= 16) return (base + 3) / 4;
    return 1;
}

static inline int16_t asteroid_motion_q_encode(float value, float scale) {
    if (!isfinite(value) || scale <= 0.0f) return 0;
    float q = value / scale;
    if (q > 32767.0f) return 32767;
    if (q < -32768.0f) return -32768;
    return (int16_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
}

static inline void asteroid_motion_q_write_i16(uint8_t *buf, int16_t value) {
    write_u16_le(buf, (uint16_t)value);
}

static inline bool asteroid_motion_q_position_delta_i8(vec2 previous,
                                                       vec2 current,
                                                       int8_t *dx_out,
                                                       int8_t *dy_out) {
    if (!dx_out || !dy_out) return false;
    int16_t prev_x = asteroid_motion_q_encode(
        previous.x, ASTEROID_MOTION_Q_POS_SCALE);
    int16_t prev_y = asteroid_motion_q_encode(
        previous.y, ASTEROID_MOTION_Q_POS_SCALE);
    int16_t cur_x = asteroid_motion_q_encode(
        current.x, ASTEROID_MOTION_Q_POS_SCALE);
    int16_t cur_y = asteroid_motion_q_encode(
        current.y, ASTEROID_MOTION_Q_POS_SCALE);
    int dx = (int)cur_x - (int)prev_x;
    int dy = (int)cur_y - (int)prev_y;
    if (dx < -128 || dx > 127 || dy < -128 || dy > 127)
        return false;
    *dx_out = (int8_t)dx;
    *dy_out = (int8_t)dy;
    return true;
}

static inline bool asteroid_motion_q_velocity_matches(vec2 a, vec2 b) {
    return asteroid_motion_q_encode(a.x, ASTEROID_MOTION_Q_VEL_SCALE) ==
               asteroid_motion_q_encode(b.x, ASTEROID_MOTION_Q_VEL_SCALE) &&
           asteroid_motion_q_encode(a.y, ASTEROID_MOTION_Q_VEL_SCALE) ==
               asteroid_motion_q_encode(b.y, ASTEROID_MOTION_Q_VEL_SCALE);
}

static inline float asteroid_net_pos_only_drift_budget_sq(float dist_sq,
                                                          float speed_sq) {
    if (asteroid_net_high_detail(dist_sq, speed_sq))
        return ASTEROID_NET_POS_ONLY_NEAR_DRIFT_SQ;
    if (dist_sq >= ASTEROID_NET_VERY_FAR_RADIUS_SQ)
        return ASTEROID_NET_POS_ONLY_VERY_FAR_DRIFT_SQ;
    return ASTEROID_NET_POS_ONLY_FAR_DRIFT_SQ;
}

static inline bool asteroid_net_pos_only_velocity_eligible(vec2 current_vel,
                                                           vec2 client_vel,
                                                           float dist_sq,
                                                           float speed_sq) {
    if (asteroid_motion_q_velocity_matches(current_vel, client_vel))
        return true;
    vec2 drift = v2_scale(v2_sub(current_vel, client_vel),
                          ASTEROID_NET_POS_ONLY_DRIFT_WINDOW_SEC);
    return v2_len_sq(drift) <=
        asteroid_net_pos_only_drift_budget_sq(dist_sq, speed_sq);
}

static inline uint8_t asteroid_wire_flags(const asteroid_t *a) {
    uint8_t flags = 1;
    if (!a) return 0;
    if (a->fracture_child) flags |= (1 << 1);
    flags |= (((uint8_t)a->tier & 0x7) << 2);
    flags |= (((uint8_t)a->commodity & 0x7) << 5);
    return flags;
}

static inline uint8_t asteroid_wire_smelt_byte(const asteroid_t *a) {
    float sp_f = a ? a->smelt_progress : 0.0f;
    if (sp_f < 0.0f) sp_f = 0.0f;
    if (sp_f > 1.0f) sp_f = 1.0f;
    return (uint8_t)(sp_f * 255.0f);
}

static inline uint16_t asteroid_identity_q_encode_value(float value) {
    if (!isfinite(value) || value <= 0.0f ||
        ASTEROID_IDENTITY_Q_VALUE_SCALE <= 0.0f) {
        return 0;
    }
    float q = value / ASTEROID_IDENTITY_Q_VALUE_SCALE;
    if (q >= 65535.0f) return 65535u;
    return (uint16_t)(q + 0.5f);
}

static inline uint8_t asteroid_identity_q_detail_byte(const asteroid_t *a) {
    uint8_t grade = a ? a->grade : 0;
    uint8_t crystal = a ? a->crystal_stage : 0;
    uint8_t phase = a ? a->phase : 0;
    return (uint8_t)((grade & 0x7u) |
                     ((crystal & 0x3u) << 3) |
                     ((phase & 0x3u) << 5));
}

static inline void serialize_one_asteroid_q(uint8_t *p,
                                            uint16_t index,
                                            const asteroid_t *a,
                                            bool byte_index) {
    int off = 0;
    if (byte_index) {
        p[off++] = (uint8_t)index;
    } else {
        write_u16_le(&p[off], index);
        off += 2;
    }
    p[off++] = asteroid_wire_flags(a);
    asteroid_motion_q_write_i16(
        &p[off], asteroid_motion_q_encode(
                     a ? a->pos.x : 0.0f, ASTEROID_MOTION_Q_POS_SCALE));
    off += 2;
    asteroid_motion_q_write_i16(
        &p[off], asteroid_motion_q_encode(
                     a ? a->pos.y : 0.0f, ASTEROID_MOTION_Q_POS_SCALE));
    off += 2;
    asteroid_motion_q_write_i16(
        &p[off], asteroid_motion_q_encode(
                     a ? a->vel.x : 0.0f, ASTEROID_MOTION_Q_VEL_SCALE));
    off += 2;
    asteroid_motion_q_write_i16(
        &p[off], asteroid_motion_q_encode(
                     a ? a->vel.y : 0.0f, ASTEROID_MOTION_Q_VEL_SCALE));
    off += 2;
    write_u16_le(&p[off], asteroid_identity_q_encode_value(
                     a ? a->hp : 0.0f));
    off += 2;
    write_u16_le(&p[off], asteroid_identity_q_encode_value(
                     a ? a->ore : 0.0f));
    off += 2;
    write_u16_le(&p[off], asteroid_identity_q_encode_value(
                     a ? a->radius : 0.0f));
    off += 2;
    p[off++] = asteroid_wire_smelt_byte(a);
    p[off++] = asteroid_identity_q_detail_byte(a);
    (void)off;
}

static inline void serialize_one_asteroid_state_q(uint8_t *p,
                                                  uint16_t index,
                                                  const asteroid_t *a) {
    write_u16_le(&p[0], index);
    write_f32_le(&p[2], a->hp);
    write_f32_le(&p[6], a->ore);
    write_f32_le(&p[10], a->radius);
    {
        p[14] = asteroid_wire_smelt_byte(a);
    }
    p[15] = a->grade;
    p[16] = a->crystal_stage;
    p[17] = a->phase;
}

static inline uint32_t asteroid_state_q_hash_u32(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        h ^= (uint8_t)(v >> (8 * i));
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t asteroid_state_q_quantize_float(float value,
                                                       float step) {
    if (!isfinite(value) || step <= 0.0f)
        return 0x80000000u;
    float q = value / step;
    if (q > 1073741823.0f) q = 1073741823.0f;
    if (q < -1073741824.0f) q = -1073741824.0f;
    int32_t rounded = (int32_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
    return (uint32_t)rounded;
}

static inline uint8_t asteroid_state_q_smelt_byte(const asteroid_t *a) {
    float sp_f = a ? a->smelt_progress : 0.0f;
    if (sp_f < 0.0f) sp_f = 0.0f;
    if (sp_f > 1.0f) sp_f = 1.0f;
    return (uint8_t)(sp_f * 255.0f);
}

static inline uint32_t asteroid_state_q_semantic_signature(
    const asteroid_t *a) {
    uint32_t h = 2166136261u;
    if (!a) return h;
    h = asteroid_state_q_hash_u32(h, (uint32_t)a->grade);
    h = asteroid_state_q_hash_u32(h, (uint32_t)a->crystal_stage);
    h = asteroid_state_q_hash_u32(h, (uint32_t)a->phase);
    return h;
}

static inline uint32_t asteroid_state_q_numeric_signature(
    const asteroid_t *a) {
    uint32_t h = asteroid_state_q_semantic_signature(a);
    if (!a) return h;
    h = asteroid_state_q_hash_u32(
        h, asteroid_state_q_quantize_float(a->hp, 1.0f));
    h = asteroid_state_q_hash_u32(
        h, asteroid_state_q_quantize_float(a->ore, 1.0f));
    h = asteroid_state_q_hash_u32(
        h, asteroid_state_q_quantize_float(a->radius, 0.5f));
    h = asteroid_state_q_hash_u32(h, asteroid_state_q_smelt_byte(a));
    return h;
}

static inline bool asteroid_state_q_should_send(
    const asteroid_t *a,
    int index,
    const uint32_t *state_sent_tick,
    const uint32_t *state_sent_sig,
    const uint32_t *state_sent_semantic_sig,
    uint32_t server_tick) {
    if (!a || index < 0 || index >= MAX_ASTEROIDS) return false;
    if (!state_sent_tick || !state_sent_sig || !state_sent_semantic_sig)
        return true;

    uint32_t semantic_sig = asteroid_state_q_semantic_signature(a);
    uint32_t numeric_sig = asteroid_state_q_numeric_signature(a);
    bool initialized = state_sent_tick[index] != 0u ||
        state_sent_sig[index] != 0u ||
        state_sent_semantic_sig[index] != 0u;
    if (!initialized)
        return true;
    if (semantic_sig != state_sent_semantic_sig[index])
        return true;

    uint32_t age_ticks = server_tick - state_sent_tick[index];
    if (numeric_sig != state_sent_sig[index] &&
        age_ticks >= ASTEROID_STATE_Q_NUMERIC_REPEAT_TICKS)
        return true;
    return age_ticks >= ASTEROID_STATE_Q_HEARTBEAT_TICKS;
}

static inline void asteroid_state_q_note_sent(
    const asteroid_t *a,
    int index,
    uint32_t *state_sent_tick,
    uint32_t *state_sent_sig,
    uint32_t *state_sent_semantic_sig,
    uint32_t server_tick) {
    if (!a || index < 0 || index >= MAX_ASTEROIDS) return;
    if (!state_sent_tick || !state_sent_sig || !state_sent_semantic_sig)
        return;
    state_sent_tick[index] = server_tick;
    state_sent_sig[index] = asteroid_state_q_numeric_signature(a);
    state_sent_semantic_sig[index] =
        asteroid_state_q_semantic_signature(a);
}

static inline void asteroid_state_q_clear_sent(
    int index,
    uint32_t *state_sent_tick,
    uint32_t *state_sent_sig,
    uint32_t *state_sent_semantic_sig) {
    if (index < 0 || index >= MAX_ASTEROIDS) return;
    if (state_sent_tick) state_sent_tick[index] = 0u;
    if (state_sent_sig) state_sent_sig[index] = 0u;
    if (state_sent_semantic_sig) state_sent_semantic_sig[index] = 0u;
}

static inline bool asteroid_net_motion_should_send(
    const asteroid_t *a,
    float dist_sq,
    float speed_sq,
    uint32_t last_tick,
    vec2 last_pos,
    vec2 last_vel,
    uint32_t server_tick) {
    if (!a || last_tick == 0u) return true;
    uint32_t age_ticks = server_tick - last_tick;
    uint32_t repeat_ticks =
        asteroid_net_moving_repeat_ticks(dist_sq, speed_sq);
    if (asteroid_net_tow_lifecycle_high_detail(a, dist_sq) &&
        repeat_ticks > ASTEROID_NET_TOWED_MOVING_REPEAT_TICKS) {
        repeat_ticks = ASTEROID_NET_TOWED_MOVING_REPEAT_TICKS;
    }
    if (age_ticks < repeat_ticks)
        return false;
    if (age_ticks >= asteroid_net_motion_heartbeat_ticks(dist_sq, speed_sq))
        return true;

    float dt = (float)age_ticks * SIM_DT;
    vec2 predicted_pos = v2_add(last_pos, v2_scale(last_vel, dt));
    float error_sq = asteroid_net_predict_error_sq(dist_sq, speed_sq);
    if (asteroid_net_tow_lifecycle_high_detail(a, dist_sq) &&
        error_sq > ASTEROID_NET_TOWED_PREDICT_ERROR_SQ) {
        error_sq = ASTEROID_NET_TOWED_PREDICT_ERROR_SQ;
    }
    return v2_dist_sq(predicted_pos, a->pos) >= error_sq;
}

static inline int serialize_asteroids_for_player_split_ext_state_budget_at_tick(
    uint8_t *buf,
    uint8_t *asteroids_q_buf, int *asteroids_q_len_out,
    uint8_t *asteroids8_q_buf, int *asteroids8_q_len_out,
    uint8_t *motion_buf, int *motion_len_out,
    uint8_t *motion_q_buf, int *motion_q_len_out,
    uint8_t *posd_q_buf, int *posd_q_len_out,
    uint8_t *posd8_q_buf, int *posd8_q_len_out,
    uint8_t *pos_q_buf, int *pos_q_len_out,
    uint8_t *pos8_q_buf, int *pos8_q_len_out,
    uint8_t *state_q_buf, int *state_q_len_out,
    uint8_t *remove_buf, int *remove_len_out,
    const asteroid_t *asteroids, vec2 player_pos, bool *sent,
    uint32_t *motion_sent_tick, vec2 *motion_sent_pos,
    vec2 *motion_sent_vel, uint32_t *state_sent_tick,
    uint32_t *state_sent_sig, uint32_t *state_sent_semantic_sig,
    uint32_t server_tick,
    int background_identity_budget) {
    int count = 0;
    int asteroids_q_count = 0;
    int asteroids8_q_count = 0;
    int motion_count = 0;
    int motion_q_count = 0;
    int posd_q_count = 0;
    int posd8_q_count = 0;
    int pos_q_count = 0;
    int pos8_q_count = 0;
    int state_q_count = 0;
    int remove_count = 0;
    int background_identity_remaining = background_identity_budget;
    bool budget_background_identity = background_identity_budget >= 0;
    if (asteroids_q_len_out) *asteroids_q_len_out = 0;
    if (asteroids8_q_len_out) *asteroids8_q_len_out = 0;
    if (motion_len_out) *motion_len_out = 0;
    if (motion_q_len_out) *motion_q_len_out = 0;
    if (posd_q_len_out) *posd_q_len_out = 0;
    if (posd8_q_len_out) *posd8_q_len_out = 0;
    if (pos_q_len_out) *pos_q_len_out = 0;
    if (pos8_q_len_out) *pos8_q_len_out = 0;
    if (state_q_len_out) *state_q_len_out = 0;
    if (remove_len_out) *remove_len_out = 0;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &asteroids[i];
        float dist_sq = v2_dist_sq(a->pos, player_pos);
        bool in_view = a->active && dist_sq < ASTEROID_VIEW_RADIUS_SQ;

        if (in_view) {
            float speed_sq = v2_len_sq(a->vel);
            bool moving = speed_sq > ASTEROID_NET_MOVING_SPEED_SQ;
            bool was_sent_moving = motion_sent_tick && motion_sent_tick[i] != 0u;
            bool motion_only = false;
            bool settling_motion_only = false;
            bool state_q_only = false;
            if (sent[i] && !a->net_dirty) {
                if (moving) {
                    if (motion_sent_tick &&
                        !asteroid_net_motion_should_send(
                            a, dist_sq, speed_sq, motion_sent_tick[i],
                            motion_sent_pos ? motion_sent_pos[i] : a->pos,
                            motion_sent_vel ? motion_sent_vel[i] : a->vel,
                            server_tick))
                        continue;
                    motion_only = motion_buf != NULL && motion_sent_tick != NULL;
                } else if (was_sent_moving) {
                    motion_only = motion_buf != NULL && motion_sent_tick != NULL;
                    settling_motion_only = motion_only;
                } else if (!was_sent_moving) {
                    continue;
                }
            } else if (sent[i] && a->net_dirty && state_q_buf != NULL) {
                state_q_only = true;
                if (asteroid_state_q_should_send(
                        a, i, state_sent_tick, state_sent_sig,
                        state_sent_semantic_sig, server_tick)) {
                    uint8_t *p = &state_q_buf[
                        ASTEROID_STATE_Q_MSG_HEADER +
                        state_q_count * ASTEROID_STATE_Q_RECORD_SIZE];
                    serialize_one_asteroid_state_q(p, (uint16_t)i, a);
                    state_q_count++;
                    asteroid_state_q_note_sent(
                        a, i, state_sent_tick, state_sent_sig,
                        state_sent_semantic_sig, server_tick);
                }
                if (moving) {
                    if (motion_sent_tick &&
                        !asteroid_net_motion_should_send(
                            a, dist_sq, speed_sq, motion_sent_tick[i],
                            motion_sent_pos ? motion_sent_pos[i] : a->pos,
                            motion_sent_vel ? motion_sent_vel[i] : a->vel,
                            server_tick))
                        continue;
                    motion_only = motion_buf != NULL && motion_sent_tick != NULL;
                } else if (was_sent_moving) {
                    motion_only = motion_buf != NULL && motion_sent_tick != NULL;
                    settling_motion_only = motion_only;
                } else {
                    continue;
                }
            }

            if (motion_only) {
                bool quantized = motion_q_buf != NULL;
                bool sent_velocity = false;
                if (quantized) {
                    bool position_only =
                        !settling_motion_only && pos_q_buf != NULL &&
                        motion_sent_vel != NULL &&
                        asteroid_net_pos_only_velocity_eligible(
                            a->vel, motion_sent_vel[i], dist_sq, speed_sq);
                    if (position_only) {
                        int8_t dx = 0;
                        int8_t dy = 0;
                        bool wrote_position = false;
                        bool delta_fits =
                            motion_sent_pos != NULL &&
                            asteroid_motion_q_position_delta_i8(
                                motion_sent_pos[i], a->pos, &dx, &dy);
                        if (delta_fits && posd8_q_buf != NULL &&
                            i <= UINT8_MAX && posd8_q_count < UINT8_MAX) {
                            uint8_t *p = &posd8_q_buf[
                                ASTEROID_POSD8_Q_MSG_HEADER +
                                posd8_q_count * ASTEROID_POSD8_Q_RECORD_SIZE];
                            p[0] = (uint8_t)i;
                            p[1] = (uint8_t)dx;
                            p[2] = (uint8_t)dy;
                            posd8_q_count++;
                            wrote_position = true;
                        } else if (delta_fits && posd_q_buf != NULL) {
                            uint8_t *p = &posd_q_buf[
                                ASTEROID_POSD_Q_MSG_HEADER +
                                posd_q_count * ASTEROID_POSD_Q_RECORD_SIZE];
                            write_u16_le(&p[0], (uint16_t)i);
                            p[2] = (uint8_t)dx;
                            p[3] = (uint8_t)dy;
                            posd_q_count++;
                            wrote_position = true;
                        }
                        if (!wrote_position && pos8_q_buf != NULL &&
                            i <= UINT8_MAX && pos8_q_count < UINT8_MAX) {
                            uint8_t *p = &pos8_q_buf[
                                ASTEROID_POS8_Q_MSG_HEADER +
                                pos8_q_count * ASTEROID_POS8_Q_RECORD_SIZE];
                            p[0] = (uint8_t)i;
                            asteroid_motion_q_write_i16(
                                &p[1], asteroid_motion_q_encode(
                                           a->pos.x, ASTEROID_MOTION_Q_POS_SCALE));
                            asteroid_motion_q_write_i16(
                                &p[3], asteroid_motion_q_encode(
                                           a->pos.y, ASTEROID_MOTION_Q_POS_SCALE));
                            pos8_q_count++;
                            wrote_position = true;
                        }
                        if (!wrote_position) {
                            uint8_t *p = &pos_q_buf[
                                ASTEROID_POS_Q_MSG_HEADER +
                                pos_q_count * ASTEROID_POS_Q_RECORD_SIZE];
                            write_u16_le(&p[0], (uint16_t)i);
                            asteroid_motion_q_write_i16(
                                &p[2], asteroid_motion_q_encode(
                                           a->pos.x, ASTEROID_MOTION_Q_POS_SCALE));
                            asteroid_motion_q_write_i16(
                                &p[4], asteroid_motion_q_encode(
                                           a->pos.y, ASTEROID_MOTION_Q_POS_SCALE));
                            pos_q_count++;
                        }
                    } else {
                        sent_velocity = true;
                        uint8_t *p = &motion_q_buf[ASTEROID_MOTION_Q_MSG_HEADER +
                                                   motion_q_count * ASTEROID_MOTION_Q_RECORD_SIZE];
                        write_u16_le(&p[0], (uint16_t)i);
                        asteroid_motion_q_write_i16(
                            &p[2], asteroid_motion_q_encode(
                                       a->pos.x, ASTEROID_MOTION_Q_POS_SCALE));
                        asteroid_motion_q_write_i16(
                            &p[4], asteroid_motion_q_encode(
                                       a->pos.y, ASTEROID_MOTION_Q_POS_SCALE));
                        asteroid_motion_q_write_i16(
                            &p[6], asteroid_motion_q_encode(
                                       a->vel.x, ASTEROID_MOTION_Q_VEL_SCALE));
                        asteroid_motion_q_write_i16(
                            &p[8], asteroid_motion_q_encode(
                                       a->vel.y, ASTEROID_MOTION_Q_VEL_SCALE));
                        motion_q_count++;
                    }
                } else {
                    sent_velocity = true;
                    uint8_t *p = &motion_buf[ASTEROID_MOTION_MSG_HEADER +
                                             motion_count * ASTEROID_MOTION_RECORD_SIZE];
                    write_u16_le(&p[0], (uint16_t)i);
                    write_f32_le(&p[2],  a->pos.x);
                    write_f32_le(&p[6],  a->pos.y);
                    write_f32_le(&p[10], a->vel.x);
                    write_f32_le(&p[14], a->vel.y);
                    motion_count++;
                }
                motion_sent_tick[i] = settling_motion_only ? 0u : server_tick;
                if (motion_sent_pos) motion_sent_pos[i] = a->pos;
                if (motion_sent_vel && sent_velocity) motion_sent_vel[i] = a->vel;
                continue;
            }
            if (state_q_only)
                continue;

            if (!sent[i] && dist_sq >= ASTEROID_NET_NEAR_RADIUS_SQ &&
                budget_background_identity) {
                if (background_identity_remaining <= 0)
                    continue;
                background_identity_remaining--;
            }

            bool wrote_identity = false;
            if (asteroids8_q_buf != NULL && i <= UINT8_MAX &&
                asteroids8_q_count < UINT8_MAX) {
                uint8_t *p = &asteroids8_q_buf[
                    ASTEROID8_Q_MSG_HEADER +
                    asteroids8_q_count * ASTEROID8_Q_RECORD_SIZE];
                serialize_one_asteroid_q(p, (uint16_t)i, a, true);
                asteroids8_q_count++;
                wrote_identity = true;
            } else if (asteroids_q_buf != NULL) {
                uint8_t *p = &asteroids_q_buf[
                    ASTEROID_Q_MSG_HEADER +
                    asteroids_q_count * ASTEROID_Q_RECORD_SIZE];
                serialize_one_asteroid_q(p, (uint16_t)i, a, false);
                asteroids_q_count++;
                wrote_identity = true;
            }
            if (!wrote_identity) {
                uint8_t *p = &buf[
                    ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
                write_u16_le(&p[0], (uint16_t)i);
                p[2] = asteroid_wire_flags(a);
                write_f32_le(&p[3],  a->pos.x);
                write_f32_le(&p[7],  a->pos.y);
                write_f32_le(&p[11], a->vel.x);
                write_f32_le(&p[15], a->vel.y);
                write_f32_le(&p[19], a->hp);
                write_f32_le(&p[23], a->ore);
                write_f32_le(&p[27], a->radius);
                /* smelt_progress: 0.0-1.0 quantized to uint8 so the client
                 * furnace-glow + laser-beam visuals fire in multiplayer. */
                p[31] = asteroid_wire_smelt_byte(a);
                p[32] = a->grade;
                p[33] = a->crystal_stage;
                p[34] = a->phase;
                count++;
            }
            sent[i] = true;
            if (motion_sent_tick) {
                motion_sent_tick[i] = moving ? server_tick : 0u;
                if (motion_sent_pos) motion_sent_pos[i] = a->pos;
                if (motion_sent_vel) motion_sent_vel[i] = a->vel;
            }
            asteroid_state_q_note_sent(
                a, i, state_sent_tick, state_sent_sig,
                state_sent_semantic_sig, server_tick);
        } else if (sent[i] && !in_view) {
            if (remove_buf) {
                uint8_t *p = &remove_buf[
                    ASTEROID_REMOVE_MSG_HEADER +
                    remove_count * ASTEROID_REMOVE_RECORD_SIZE];
                write_u16_le(p, (uint16_t)i);
                remove_count++;
            } else {
                uint8_t *p = &buf[
                    ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
                memset(p, 0, ASTEROID_RECORD_SIZE);
                write_u16_le(&p[0], (uint16_t)i);
                p[2] = 0; /* active = false */
                count++;
            }
            sent[i] = false;
            if (motion_sent_tick) {
                motion_sent_tick[i] = 0u;
                if (motion_sent_pos) motion_sent_pos[i] = v2(0.0f, 0.0f);
                if (motion_sent_vel) motion_sent_vel[i] = v2(0.0f, 0.0f);
            }
            asteroid_state_q_clear_sent(
                i, state_sent_tick, state_sent_sig,
                state_sent_semantic_sig);
        }
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    write_u16_le(&buf[1], (uint16_t)count);
    if (asteroids_q_buf) {
        asteroids_q_buf[0] = NET_MSG_WORLD_ASTEROIDS_Q;
        write_u16_le(&asteroids_q_buf[1], (uint16_t)asteroids_q_count);
        if (asteroids_q_len_out) {
            *asteroids_q_len_out = ASTEROID_Q_MSG_HEADER +
                asteroids_q_count * ASTEROID_Q_RECORD_SIZE;
        }
    }
    if (asteroids8_q_buf) {
        asteroids8_q_buf[0] = NET_MSG_WORLD_ASTEROIDS8_Q;
        asteroids8_q_buf[1] = (uint8_t)asteroids8_q_count;
        if (asteroids8_q_len_out) {
            *asteroids8_q_len_out = ASTEROID8_Q_MSG_HEADER +
                asteroids8_q_count * ASTEROID8_Q_RECORD_SIZE;
        }
    }
    if (remove_buf) {
        remove_buf[0] = NET_MSG_WORLD_ASTEROID_REMOVE;
        write_u16_le(&remove_buf[1], (uint16_t)remove_count);
        if (remove_len_out) {
            *remove_len_out = ASTEROID_REMOVE_MSG_HEADER +
                remove_count * ASTEROID_REMOVE_RECORD_SIZE;
        }
    }
    if (motion_buf) {
        motion_buf[0] = NET_MSG_WORLD_ASTEROID_MOTION;
        write_u16_le(&motion_buf[1], (uint16_t)motion_count);
        if (motion_len_out) {
            *motion_len_out = ASTEROID_MOTION_MSG_HEADER +
                motion_count * ASTEROID_MOTION_RECORD_SIZE;
        }
    }
    if (motion_q_buf) {
        motion_q_buf[0] = NET_MSG_WORLD_ASTEROID_MOTION_Q;
        write_u16_le(&motion_q_buf[1], (uint16_t)motion_q_count);
        if (motion_q_len_out) {
            *motion_q_len_out = ASTEROID_MOTION_Q_MSG_HEADER +
                motion_q_count * ASTEROID_MOTION_Q_RECORD_SIZE;
        }
    }
    if (posd_q_buf) {
        posd_q_buf[0] = NET_MSG_WORLD_ASTEROID_POSD_Q;
        write_u16_le(&posd_q_buf[1], (uint16_t)posd_q_count);
        if (posd_q_len_out) {
            *posd_q_len_out = ASTEROID_POSD_Q_MSG_HEADER +
                posd_q_count * ASTEROID_POSD_Q_RECORD_SIZE;
        }
    }
    if (posd8_q_buf) {
        posd8_q_buf[0] = NET_MSG_WORLD_ASTEROID_POSD8_Q;
        posd8_q_buf[1] = (uint8_t)posd8_q_count;
        if (posd8_q_len_out) {
            *posd8_q_len_out = ASTEROID_POSD8_Q_MSG_HEADER +
                posd8_q_count * ASTEROID_POSD8_Q_RECORD_SIZE;
        }
    }
    if (pos_q_buf) {
        pos_q_buf[0] = NET_MSG_WORLD_ASTEROID_POS_Q;
        write_u16_le(&pos_q_buf[1], (uint16_t)pos_q_count);
        if (pos_q_len_out) {
            *pos_q_len_out = ASTEROID_POS_Q_MSG_HEADER +
                pos_q_count * ASTEROID_POS_Q_RECORD_SIZE;
        }
    }
    if (pos8_q_buf) {
        pos8_q_buf[0] = NET_MSG_WORLD_ASTEROID_POS8_Q;
        pos8_q_buf[1] = (uint8_t)pos8_q_count;
        if (pos8_q_len_out) {
            *pos8_q_len_out = ASTEROID_POS8_Q_MSG_HEADER +
                pos8_q_count * ASTEROID_POS8_Q_RECORD_SIZE;
        }
    }
    if (state_q_buf) {
        state_q_buf[0] = NET_MSG_WORLD_ASTEROID_STATE_Q;
        write_u16_le(&state_q_buf[1], (uint16_t)state_q_count);
        if (state_q_len_out) {
            *state_q_len_out = ASTEROID_STATE_Q_MSG_HEADER +
                state_q_count * ASTEROID_STATE_Q_RECORD_SIZE;
        }
    }
    return ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE;
}

static inline int serialize_asteroids_for_player_split_ext_state_at_tick(
    uint8_t *buf,
    uint8_t *asteroids_q_buf, int *asteroids_q_len_out,
    uint8_t *asteroids8_q_buf, int *asteroids8_q_len_out,
    uint8_t *motion_buf, int *motion_len_out,
    uint8_t *motion_q_buf, int *motion_q_len_out,
    uint8_t *posd_q_buf, int *posd_q_len_out,
    uint8_t *posd8_q_buf, int *posd8_q_len_out,
    uint8_t *pos_q_buf, int *pos_q_len_out,
    uint8_t *pos8_q_buf, int *pos8_q_len_out,
    uint8_t *state_q_buf, int *state_q_len_out,
    uint8_t *remove_buf, int *remove_len_out,
    const asteroid_t *asteroids, vec2 player_pos, bool *sent,
    uint32_t *motion_sent_tick, vec2 *motion_sent_pos,
    vec2 *motion_sent_vel, uint32_t *state_sent_tick,
    uint32_t *state_sent_sig, uint32_t *state_sent_semantic_sig,
    uint32_t server_tick) {
    return serialize_asteroids_for_player_split_ext_state_budget_at_tick(
        buf, asteroids_q_buf, asteroids_q_len_out,
        asteroids8_q_buf, asteroids8_q_len_out,
        motion_buf, motion_len_out, motion_q_buf, motion_q_len_out,
        posd_q_buf, posd_q_len_out, posd8_q_buf, posd8_q_len_out,
        pos_q_buf, pos_q_len_out, pos8_q_buf, pos8_q_len_out,
        state_q_buf, state_q_len_out, remove_buf, remove_len_out,
        asteroids, player_pos, sent, motion_sent_tick, motion_sent_pos,
        motion_sent_vel, state_sent_tick, state_sent_sig,
        state_sent_semantic_sig, server_tick, -1);
}

static inline int serialize_asteroids_for_player_split_ext_at_tick(
    uint8_t *buf, uint8_t *motion_buf, int *motion_len_out,
    uint8_t *motion_q_buf, int *motion_q_len_out,
    uint8_t *pos_q_buf, int *pos_q_len_out,
    uint8_t *pos8_q_buf, int *pos8_q_len_out,
    uint8_t *state_q_buf, int *state_q_len_out,
    const asteroid_t *asteroids, vec2 player_pos, bool *sent,
    uint32_t *motion_sent_tick, vec2 *motion_sent_pos,
    vec2 *motion_sent_vel, uint32_t server_tick) {
    return serialize_asteroids_for_player_split_ext_state_at_tick(
        buf, NULL, NULL, NULL, NULL,
        motion_buf, motion_len_out, motion_q_buf, motion_q_len_out,
        NULL, NULL, NULL, NULL, pos_q_buf, pos_q_len_out,
        pos8_q_buf, pos8_q_len_out,
        state_q_buf, state_q_len_out,
        NULL, NULL, asteroids, player_pos, sent, motion_sent_tick,
        motion_sent_pos, motion_sent_vel, NULL, NULL, NULL, server_tick);
}

static inline int serialize_asteroids_for_player_split_at_tick(
    uint8_t *buf, uint8_t *motion_buf, int *motion_len_out,
    uint8_t *motion_q_buf, int *motion_q_len_out,
    uint8_t *state_q_buf, int *state_q_len_out,
    const asteroid_t *asteroids, vec2 player_pos, bool *sent,
    uint32_t *motion_sent_tick, vec2 *motion_sent_pos,
    vec2 *motion_sent_vel, uint32_t server_tick) {
    return serialize_asteroids_for_player_split_ext_at_tick(
        buf, motion_buf, motion_len_out, motion_q_buf, motion_q_len_out,
        NULL, NULL, NULL, NULL, state_q_buf, state_q_len_out,
        asteroids, player_pos, sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel, server_tick);
}

static inline int serialize_asteroids_for_player_at_tick(
    uint8_t *buf, const asteroid_t *asteroids, vec2 player_pos, bool *sent,
    uint32_t *motion_sent_tick, vec2 *motion_sent_pos,
    vec2 *motion_sent_vel, uint32_t server_tick) {
    return serialize_asteroids_for_player_split_at_tick(
        buf, NULL, NULL, NULL, NULL, NULL, NULL, asteroids, player_pos, sent,
        motion_sent_tick, motion_sent_pos, motion_sent_vel, server_tick);
}

static inline int serialize_asteroids_for_player(
    uint8_t *buf, const asteroid_t *asteroids, vec2 player_pos, bool *sent) {
    return serialize_asteroids_for_player_at_tick(
        buf, asteroids, player_pos, sent, NULL, NULL, NULL, 0u);
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

static inline uint8_t inspect_snapshot_compact_unit(float value) {
    if (!isfinite(value) || value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static inline uint8_t inspect_snapshot_compact_signed_unit(float value) {
    if (!isfinite(value)) value = 0.0f;
    if (value < -1.0f) value = -1.0f;
    if (value > 1.0f) value = 1.0f;
    return inspect_snapshot_compact_unit(value * 0.5f + 0.5f);
}

static inline uint8_t inspect_snapshot_compact_snr(float fidelity) {
    if (!isfinite(fidelity) || fidelity <= 0.0f) return 0;
    if (fidelity >= 0.995f) return 255;
    float snr = fidelity / (1.0f - fidelity);
    if (!isfinite(snr) || snr <= 0.0f) return 0;
    if (snr >= 8.0f) return 255;
    return (uint8_t)(snr * (255.0f / 8.0f) + 0.5f);
}

static inline uint8_t inspect_snapshot_hnn_trace_flags(
    const hnn_memory_contract_t *contract) {
    if (!contract || contract->stored_count <= 0)
        return (uint8_t)INSPECT_HNN_TRACE_WARN_UNTRAINED;
    uint8_t flags = 0;
    if (contract->capacity_load >= 0.85f ||
        contract->fidelity_estimate < 0.35f) {
        flags |= (uint8_t)INSPECT_HNN_TRACE_WARN_NOISY;
    }
    if (contract->last_margin < 0.05f)
        flags |= (uint8_t)INSPECT_HNN_TRACE_WARN_LOW_MARGIN;
    return flags;
}

static inline int write_inspect_snapshot_hnn_trace_row(uint8_t *buf,
                                                       int row_count,
                                                       int max_rows,
                                                       const npc_ship_t *npc) {
    if (!buf || !npc || row_count >= max_rows) return row_count;
    if (npc->brain_mode != SERVER_BRAIN_MODE_HOLOGRAPHIC &&
        npc->hnn_mem.experience_count <= 0) {
        return row_count;
    }

    hnn_memory_contract_t contract = hnn_memory_contract(&npc->hnn_mem);
    uint8_t *p =
        &buf[INSPECT_SNAPSHOT_HEADER + row_count * INSPECT_SNAPSHOT_ROW];
    memset(p, 0, INSPECT_SNAPSHOT_ROW);
    p[0] = (uint8_t)INSPECT_DIAG_HNN_TRACE;
    p[1] = inspect_snapshot_compact_unit(contract.capacity_load);
    p[2] = inspect_snapshot_compact_unit(contract.fidelity_estimate);
    p[3] = INSPECT_ROW_DIAGNOSTIC;
    write_u64_le(&p[4], contract.action_vocabulary_hash);
    uint16_t stored = contract.stored_count > 0xFFFF
        ? 0xFFFFu : (uint16_t)contract.stored_count;
    write_u16_le(&p[12], stored);
    p[14 + INSPECT_HNN_TRACE_LOAD] = p[1];
    p[14 + INSPECT_HNN_TRACE_FIDELITY] = p[2];
    p[14 + INSPECT_HNN_TRACE_MARGIN] =
        inspect_snapshot_compact_signed_unit(contract.last_margin);
    p[14 + INSPECT_HNN_TRACE_SNR] =
        inspect_snapshot_compact_snr(contract.fidelity_estimate);
    p[14 + INSPECT_HNN_TRACE_FLAGS] =
        inspect_snapshot_hnn_trace_flags(&contract);
    p[14 + INSPECT_HNN_TRACE_KEYGEN_VERSION] =
        (uint8_t)(contract.keygen_version > 255u
            ? 255u : contract.keygen_version);
    p[14 + INSPECT_HNN_TRACE_ENCODER_VERSION] =
        (uint8_t)(contract.encoder_version > 255u
            ? 255u : contract.encoder_version);
    p[14 + INSPECT_HNN_TRACE_FORMAT_VERSION] =
        (uint8_t)(contract.trace_format_version > 255u
            ? 255u : contract.trace_format_version);
    p[14 + INSPECT_HNN_TRACE_CAPACITY_LO] =
        (uint8_t)(HNN_TRACE_CAPACITY & 0xFFu);
    p[14 + INSPECT_HNN_TRACE_CAPACITY_HI] =
        (uint8_t)((HNN_TRACE_CAPACITY >> 8) & 0xFFu);
    p[14 + INSPECT_HNN_TRACE_DIM_LO] =
        (uint8_t)((uint32_t)contract.dim & 0xFFu);
    p[14 + INSPECT_HNN_TRACE_DIM_HI] =
        (uint8_t)(((uint32_t)contract.dim >> 8) & 0xFFu);
    write_u64_le(&p[46], contract.seed);
    write_u64_le(&p[54], contract.action_vocabulary_hash);
    write_u32_le(&p[62], (uint32_t)contract.dim);
    return row_count + 1;
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
    row_count = write_inspect_snapshot_hnn_trace_row(
        buf, row_count, INSPECT_SNAPSHOT_MAX_ROWS, npc_diag);
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
#define NPC_NET_METADATA_HEARTBEAT_TICKS 2400u /* 0.05 Hz metadata reconciliation */

static inline bool npc_net_metadata_refresh_due(uint32_t last_sent_tick,
                                                uint32_t world_tick) {
    return last_sent_tick == 0 ||
        (uint32_t)(world_tick - last_sent_tick) >=
            NPC_NET_METADATA_HEARTBEAT_TICKS;
}

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

static inline int serialize_npc_motion_for_player(uint8_t *buf,
                                                  const npc_ship_t *npcs,
                                                  vec2 player_pos) {
    int count = 0;
    if (!buf || !npcs) return 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;

        uint8_t *p = &buf[NPC_MOTION_MSG_HEADER +
                          count * NPC_MOTION_RECORD_SIZE];
        p[0] = (uint8_t)i;
        p[1] = 1;
        if (npcs[i].thrusting) p[1] |= (1 << 6);
        write_f32_le(&p[2],  npcs[i].ship.pos.x);
        write_f32_le(&p[6],  npcs[i].ship.pos.y);
        write_f32_le(&p[10], npcs[i].ship.vel.x);
        write_f32_le(&p[14], npcs[i].ship.vel.y);
        write_f32_le(&p[18], npcs[i].ship.angle);
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPC_MOTION;
    buf[1] = (uint8_t)count;
    return NPC_MOTION_MSG_HEADER + count * NPC_MOTION_RECORD_SIZE;
}

static inline uint16_t npc_motion_q_encode_angle(float angle) {
    if (!isfinite(angle)) return 0;
    float wrapped = fmodf(angle, 6.28318530717958647692f);
    if (wrapped < 0.0f) wrapped += 6.28318530717958647692f;
    float q = wrapped / NPC_MOTION_Q_ANGLE_SCALE;
    uint32_t qi = (uint32_t)(q + 0.5f);
    return (uint16_t)(qi & 0xFFFFu);
}

static inline uint8_t npc_motion8_q_encode_angle(float angle) {
    if (!isfinite(angle)) return 0;
    float wrapped = fmodf(angle, 6.28318530717958647692f);
    if (wrapped < 0.0f) wrapped += 6.28318530717958647692f;
    float q = wrapped / NPC_MOTION8_Q_ANGLE_SCALE;
    uint32_t qi = (uint32_t)(q + 0.5f);
    return (uint8_t)(qi & 0xFFu);
}

static inline int8_t npc_motion8_q_encode_vel(float value) {
    if (!isfinite(value)) return 0;
    float q = value / NPC_MOTION8_Q_VEL_SCALE;
    if (q > 127.0f) return 127;
    if (q < -128.0f) return -128;
    return (int8_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
}

static inline int serialize_npc_motion_q_for_player(uint8_t *buf,
                                                    const npc_ship_t *npcs,
                                                    vec2 player_pos) {
    int count = 0;
    if (!buf || !npcs) return 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;

        uint8_t *p = &buf[NPC_MOTION_Q_MSG_HEADER +
                          count * NPC_MOTION_Q_RECORD_SIZE];
        p[0] = (uint8_t)i;
        p[1] = 1;
        if (npcs[i].thrusting) p[1] |= (1 << 6);
        asteroid_motion_q_write_i16(
            &p[2], asteroid_motion_q_encode(
                       npcs[i].ship.pos.x, NPC_MOTION_Q_POS_SCALE));
        asteroid_motion_q_write_i16(
            &p[4], asteroid_motion_q_encode(
                       npcs[i].ship.pos.y, NPC_MOTION_Q_POS_SCALE));
        asteroid_motion_q_write_i16(
            &p[6], asteroid_motion_q_encode(
                       npcs[i].ship.vel.x, NPC_MOTION_Q_VEL_SCALE));
        asteroid_motion_q_write_i16(
            &p[8], asteroid_motion_q_encode(
                       npcs[i].ship.vel.y, NPC_MOTION_Q_VEL_SCALE));
        write_u16_le(&p[10], npc_motion_q_encode_angle(npcs[i].ship.angle));
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPC_MOTION_Q;
    buf[1] = (uint8_t)count;
    return NPC_MOTION_Q_MSG_HEADER + count * NPC_MOTION_Q_RECORD_SIZE;
}

static inline int serialize_npc_motion8_q_for_player(uint8_t *buf,
                                                     const npc_ship_t *npcs,
                                                     vec2 player_pos) {
    int count = 0;
    if (!buf || !npcs) return 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;

        uint8_t *p = &buf[NPC_MOTION8_Q_MSG_HEADER +
                          count * NPC_MOTION8_Q_RECORD_SIZE];
        p[0] = (uint8_t)i;
        p[1] = 1;
        if (npcs[i].thrusting) p[1] |= (1 << 6);
        asteroid_motion_q_write_i16(
            &p[2], asteroid_motion_q_encode(
                       npcs[i].ship.pos.x, NPC_MOTION_Q_POS_SCALE));
        asteroid_motion_q_write_i16(
            &p[4], asteroid_motion_q_encode(
                       npcs[i].ship.pos.y, NPC_MOTION_Q_POS_SCALE));
        p[6] = (uint8_t)npc_motion8_q_encode_vel(npcs[i].ship.vel.x);
        p[7] = (uint8_t)npc_motion8_q_encode_vel(npcs[i].ship.vel.y);
        p[8] = npc_motion8_q_encode_angle(npcs[i].ship.angle);
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPC_MOTION8_Q;
    buf[1] = (uint8_t)count;
    return NPC_MOTION8_Q_MSG_HEADER + count * NPC_MOTION8_Q_RECORD_SIZE;
}

static inline void serialize_one_npc_pos_q(uint8_t *p,
                                           int index,
                                           vec2 pos) {
    p[0] = (uint8_t)index;
    asteroid_motion_q_write_i16(
        &p[1], asteroid_motion_q_encode(pos.x, NPC_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[3], asteroid_motion_q_encode(pos.y, NPC_MOTION_Q_POS_SCALE));
}

static inline void serialize_one_npc_pose_q(uint8_t *p,
                                            int index,
                                            vec2 pos,
                                            float angle) {
    serialize_one_npc_pos_q(p, index, pos);
    write_u16_le(&p[5], npc_motion_q_encode_angle(angle));
}

static inline void serialize_one_npc_linear_q(uint8_t *p,
                                              int index,
                                              vec2 pos,
                                              vec2 vel) {
    serialize_one_npc_pos_q(p, index, pos);
    asteroid_motion_q_write_i16(
        &p[5], asteroid_motion_q_encode(vel.x, NPC_MOTION_Q_VEL_SCALE));
    asteroid_motion_q_write_i16(
        &p[7], asteroid_motion_q_encode(vel.y, NPC_MOTION_Q_VEL_SCALE));
}

#define NPC_MOTION_NET_REPEAT_TICKS 240u /* ~0.5 Hz minimum eligibility */
#define NPC_STATUS_NET_REPEAT_TICKS 240u /* ~0.5 Hz visual status refresh */
#define NPC_MOTION_PREDICT_ERROR_SQ (32.0f * 32.0f)
#define NPC_MOTION_VEL_ERROR_SQ (8.0f * 8.0f)
#define NPC_MOTION_ANGLE_ERROR 1.25f
#define NPC_MOTION_HEARTBEAT_TICKS 720u /* 0.17 Hz clean-motion safety refresh */
#define NPC_MOTION_VISUAL_FLAGS_MASK (1u << 6)

static inline float npc_motion_angle_delta(float a, float b) {
    const float two_pi = 6.28318530717958647692f;
    if (!isfinite(a) || !isfinite(b)) return NPC_MOTION_ANGLE_ERROR;
    float d = fabsf(a - b);
    while (d > two_pi) d -= two_pi;
    if (d < 0.0f) d = -d;
    if (d > 3.14159265358979323846f) d = two_pi - d;
    return d;
}

static inline bool npc_motion_should_send(
    const server_player_t *sp,
    uint8_t index,
    uint8_t flags,
    vec2 pos,
    vec2 vel,
    float angle,
    uint32_t server_tick) {
    if (!sp || index >= MAX_NPC_SHIPS) return false;
    uint32_t last_tick = sp->npc_motion_sent_tick[index];
    if (last_tick == 0u) return true;
    if (((sp->npc_motion_sent_flags[index] ^ flags) &
         NPC_MOTION_VISUAL_FLAGS_MASK) != 0)
        return true;
    uint32_t age_ticks = server_tick - last_tick;
    if (age_ticks >= NPC_MOTION_HEARTBEAT_TICKS)
        return true;
    float dt = (float)age_ticks * SIM_DT;
    vec2 predicted = v2_add(
        sp->npc_motion_sent_pos[index],
        v2_scale(sp->npc_motion_sent_vel[index], dt));
    if (v2_dist_sq(predicted, pos) >= NPC_MOTION_PREDICT_ERROR_SQ)
        return true;
    if (v2_dist_sq(sp->npc_motion_sent_vel[index], vel) >=
        NPC_MOTION_VEL_ERROR_SQ)
        return true;
    return npc_motion_angle_delta(
        sp->npc_motion_sent_angle[index], angle) >= NPC_MOTION_ANGLE_ERROR;
}

static inline bool npc_motion_pos_q_eligible(
    const server_player_t *sp,
    uint8_t index,
    uint8_t flags,
    vec2 vel,
    float angle,
    uint32_t server_tick) {
    if (!sp || index >= MAX_NPC_SHIPS) return false;
    uint32_t last_tick = sp->npc_motion_sent_tick[index];
    if (last_tick == 0u) return false;
    if (((sp->npc_motion_sent_flags[index] ^ flags) &
         NPC_MOTION_VISUAL_FLAGS_MASK) != 0)
        return false;
    uint32_t age_ticks = server_tick - last_tick;
    if (age_ticks >= NPC_MOTION_HEARTBEAT_TICKS)
        return false;
    if (v2_dist_sq(sp->npc_motion_sent_vel[index], vel) >=
        NPC_MOTION_VEL_ERROR_SQ)
        return false;
    return npc_motion_angle_delta(
        sp->npc_motion_sent_angle[index], angle) < NPC_MOTION_ANGLE_ERROR;
}

static inline bool npc_motion_pose_q_eligible(
    const server_player_t *sp,
    uint8_t index,
    uint8_t flags,
    vec2 vel,
    uint32_t server_tick) {
    if (!sp || index >= MAX_NPC_SHIPS) return false;
    uint32_t last_tick = sp->npc_motion_sent_tick[index];
    if (last_tick == 0u) return false;
    if (((sp->npc_motion_sent_flags[index] ^ flags) &
         NPC_MOTION_VISUAL_FLAGS_MASK) != 0)
        return false;
    uint32_t age_ticks = server_tick - last_tick;
    if (age_ticks >= NPC_MOTION_HEARTBEAT_TICKS)
        return false;
    return v2_dist_sq(sp->npc_motion_sent_vel[index], vel) <
        NPC_MOTION_VEL_ERROR_SQ;
}

static inline bool npc_motion_linear_q_eligible(
    const server_player_t *sp,
    uint8_t index,
    uint8_t flags,
    float angle,
    uint32_t server_tick) {
    if (!sp || index >= MAX_NPC_SHIPS) return false;
    uint32_t last_tick = sp->npc_motion_sent_tick[index];
    if (last_tick == 0u) return false;
    if (((sp->npc_motion_sent_flags[index] ^ flags) &
         NPC_MOTION_VISUAL_FLAGS_MASK) != 0)
        return false;
    uint32_t age_ticks = server_tick - last_tick;
    if (age_ticks >= NPC_MOTION_HEARTBEAT_TICKS)
        return false;
    return npc_motion_angle_delta(
        sp->npc_motion_sent_angle[index], angle) < NPC_MOTION_ANGLE_ERROR;
}

static inline void npc_motion_note_sent(server_player_t *sp,
                                        uint8_t index,
                                        uint8_t flags,
                                        vec2 pos,
                                        vec2 vel,
                                        float angle,
                                        uint32_t server_tick) {
    if (!sp || index >= MAX_NPC_SHIPS) return;
    sp->npc_motion_sent_tick[index] = server_tick;
    sp->npc_motion_sent_flags[index] = flags;
    sp->npc_motion_sent_pos[index] = pos;
    sp->npc_motion_sent_vel[index] = vel;
    sp->npc_motion_sent_angle[index] = angle;
}

static inline int serialize_npc_status_for_player(uint8_t *buf,
                                                  const npc_ship_t *npcs,
                                                  vec2 player_pos) {
    int count = 0;
    if (!buf || !npcs) return 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;

        uint8_t *p = &buf[NPC_STATUS_MSG_HEADER +
                          count * NPC_STATUS_RECORD_SIZE];
        uint16_t target =
            (npcs[i].target_asteroid >= 0 &&
             npcs[i].target_asteroid < MAX_ASTEROIDS)
            ? (uint16_t)npcs[i].target_asteroid : 0xFFFFu;
        int towed_idx = npc_towed_fragment_index(&npcs[i]);
        uint16_t towed = (towed_idx >= 0) ? (uint16_t)towed_idx : 0xFFFFu;
        p[0] = (uint8_t)i;
        p[1] = 1;
        p[1] |= (((uint8_t)npcs[i].role & 0x3) << 1);
        p[1] |= (((uint8_t)npcs[i].state & 0x7) << 3);
        if (npcs[i].thrusting) p[1] |= (1 << 6);
        write_u16_le(&p[2], target);
        write_u16_le(&p[4], towed);
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPC_STATUS;
    buf[1] = (uint8_t)count;
    return NPC_STATUS_MSG_HEADER + count * NPC_STATUS_RECORD_SIZE;
}

static inline uint8_t npc_status8_encode_ref(int index) {
    return (index >= 0 && index < 255) ? (uint8_t)index : 0xFFu;
}

static inline bool npc_status8_ref_representable(int index) {
    return index < 0 || index < 255;
}

static inline int serialize_npc_status8_for_player(uint8_t *buf,
                                                   const npc_ship_t *npcs,
                                                   vec2 player_pos) {
    if (!buf || !npcs) return 0;
    int count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;
        int target =
            (npcs[i].target_asteroid >= 0 &&
             npcs[i].target_asteroid < MAX_ASTEROIDS)
            ? npcs[i].target_asteroid : -1;
        int towed = npc_towed_fragment_index(&npcs[i]);
        if (!npc_status8_ref_representable(target) ||
            !npc_status8_ref_representable(towed))
            return 0;
        count++;
    }

    buf[0] = NET_MSG_WORLD_NPC_STATUS8_Q;
    buf[1] = (uint8_t)count;
    count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        if (!serialize_relevance_in_player_view(npcs[i].ship.pos, player_pos))
            continue;

        uint8_t *p = &buf[NPC_STATUS8_MSG_HEADER +
                          count * NPC_STATUS8_RECORD_SIZE];
        int target =
            (npcs[i].target_asteroid >= 0 &&
             npcs[i].target_asteroid < MAX_ASTEROIDS)
            ? npcs[i].target_asteroid : -1;
        int towed = npc_towed_fragment_index(&npcs[i]);
        p[0] = (uint8_t)i;
        p[1] = 1;
        p[1] |= (((uint8_t)npcs[i].role & 0x3) << 1);
        p[1] |= (((uint8_t)npcs[i].state & 0x7) << 3);
        if (npcs[i].thrusting) p[1] |= (1 << 6);
        p[2] = npc_status8_encode_ref(target);
        p[3] = npc_status8_encode_ref(towed);
        count++;
    }
    return NPC_STATUS8_MSG_HEADER + count * NPC_STATUS8_RECORD_SIZE;
}

static inline uint64_t net_world_npc_status_semantic_hash(const uint8_t *data,
                                                          int len) {
    uint64_t h = 1469598103934665603ull;
    if (!data || len <= 0) return h;
    int header = 0;
    int record_size = 0;
    if (len >= NPC_STATUS_MSG_HEADER && data[0] == NET_MSG_WORLD_NPC_STATUS) {
        header = NPC_STATUS_MSG_HEADER;
        record_size = NPC_STATUS_RECORD_SIZE;
    } else if (len >= NPC_STATUS8_MSG_HEADER &&
               data[0] == NET_MSG_WORLD_NPC_STATUS8_Q) {
        header = NPC_STATUS8_MSG_HEADER;
        record_size = NPC_STATUS8_RECORD_SIZE;
    }
    if (header == 0 || record_size == 0) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int count = data[1];
    int expected = header + count * record_size;
    if (len < expected) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    for (int i = 0; i < len; i++) {
        uint8_t v = data[i];
        if (i >= header && i < expected) {
            int record_off = (i - header) % record_size;
            if (record_off == 1)
                v = (uint8_t)(v & ~NPC_MOTION_VISUAL_FLAGS_MASK);
        }
        h = net_fnv1a64_update(h, v);
    }
    return h;
}

static inline uint64_t net_world_npcs_semantic_hash(const uint8_t *data,
                                                    int len) {
    if (!data || len <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    if (len < 2 || data[0] != NET_MSG_WORLD_NPCS) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int count = data[1];
    int expected = 2 + count * NPC_RECORD_SIZE;
    if (len < expected) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    for (int i = 0; i < len; i++) {
        bool ignored_byte = false;
        if (i >= 2 && i < expected) {
            int record_off = (i - 2) % NPC_RECORD_SIZE;
            ignored_byte =
                (record_off >= 2 && record_off < 18) ||  /* pos + vel */
                (record_off >= 18 && record_off < 22) ||  /* angle */
                (record_off >= 22 && record_off < 26) ||  /* status target/tow */
                (record_off >= 26 && record_off < 29);    /* rarity tint */
        }
        if (!ignored_byte) {
            uint8_t v = data[i];
            if (i >= 2 && i < expected &&
                (i - 2) % NPC_RECORD_SIZE == 1) {
                v = (uint8_t)(v & ~((7u << 3) | (1u << 6)));
            }
            h = net_fnv1a64_update(h, v);
        }
    }
    return h;
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
    1 + 5 * 4 == PLAYER_MOTION_RECORD_SIZE,
    "PLAYER_MOTION_RECORD_SIZE must match serialized player motion layout"
);
_Static_assert(
    1 + 2 + 2 + 2 + 2 + 1 == PLAYER_MOTION_Q_RECORD_SIZE,
    "PLAYER_MOTION_Q_RECORD_SIZE must match serialized quantized player motion layout"
);
_Static_assert(
    1 + 1 + 1 + 1 + 1 + 1 == PLAYER_MOTIOND_Q_RECORD_SIZE,
    "PLAYER_MOTIOND_Q_RECORD_SIZE must match serialized player motion delta layout"
);
_Static_assert(
    1 + 1 + 1 + 1 == PLAYER_POSED_Q_RECORD_SIZE,
    "PLAYER_POSED_Q_RECORD_SIZE must match serialized player pose delta layout"
);
_Static_assert(
    1 + 1 + 1 + 1 == PLAYER_MOTIONM_Q_POSE_RECORD_SIZE,
    "PLAYER_MOTIONM_Q_POSE_RECORD_SIZE must match mixed pose delta layout"
);
_Static_assert(
    1 + 1 + 1 + 1 + 1 + 1 == PLAYER_MOTIONM_Q_VEL_RECORD_SIZE,
    "PLAYER_MOTIONM_Q_VEL_RECORD_SIZE must match mixed velocity delta layout"
);
_Static_assert(
    1 + 1 == PLAYER_DOCK_RECORD_SIZE,
    "PLAYER_DOCK_RECORD_SIZE must match serialized player dock layout"
);
_Static_assert(
    2 + 1 + 7 * 4 + 1 + 1 + 1 + 1 == ASTEROID_RECORD_SIZE,  /* uint16 index + flags + 7 floats + smelt:u8 + grade:u8 + crystal_stage:u8 + phase:u8 */
    "ASTEROID_RECORD_SIZE must match serialized asteroid layout"
);
_Static_assert(
    2 + 1 + 4 * 2 + 3 * 2 + 1 + 1 == ASTEROID_Q_RECORD_SIZE,
    "ASTEROID_Q_RECORD_SIZE must match serialized compact asteroid layout"
);
_Static_assert(
    1 + 1 + 4 * 2 + 3 * 2 + 1 + 1 == ASTEROID8_Q_RECORD_SIZE,
    "ASTEROID8_Q_RECORD_SIZE must match serialized byte-index compact asteroid layout"
);
_Static_assert(
    2 + 4 * 4 == ASTEROID_MOTION_RECORD_SIZE,
    "ASTEROID_MOTION_RECORD_SIZE must match serialized asteroid motion layout"
);
_Static_assert(
    2 + 4 * 2 == ASTEROID_MOTION_Q_RECORD_SIZE,
    "ASTEROID_MOTION_Q_RECORD_SIZE must match serialized quantized asteroid motion layout"
);
_Static_assert(
    2 + 2 * 2 == ASTEROID_POS_Q_RECORD_SIZE,
    "ASTEROID_POS_Q_RECORD_SIZE must match serialized quantized asteroid position layout"
);
_Static_assert(
    1 + 2 * 2 == ASTEROID_POS8_Q_RECORD_SIZE,
    "ASTEROID_POS8_Q_RECORD_SIZE must match serialized byte-index asteroid position layout"
);
_Static_assert(
    2 + 2 == ASTEROID_POSD_Q_RECORD_SIZE,
    "ASTEROID_POSD_Q_RECORD_SIZE must match serialized asteroid position delta layout"
);
_Static_assert(
    1 + 2 == ASTEROID_POSD8_Q_RECORD_SIZE,
    "ASTEROID_POSD8_Q_RECORD_SIZE must match serialized byte-index asteroid position delta layout"
);
_Static_assert(
    2 == ASTEROID_REMOVE_RECORD_SIZE,
    "ASTEROID_REMOVE_RECORD_SIZE must match serialized asteroid removal layout"
);
_Static_assert(
    2 + 3 * 4 + 4 == ASTEROID_STATE_Q_RECORD_SIZE,
    "ASTEROID_STATE_Q_RECORD_SIZE must match serialized compact asteroid state layout"
);
_Static_assert(
    4 + 6 * 4 == SCAFFOLD_RECORD_SIZE,
    "SCAFFOLD_RECORD_SIZE must match serialized scaffold layout"
);
_Static_assert(
    1 == SCAFFOLD_REMOVE_RECORD_SIZE,
    "SCAFFOLD_REMOVE_RECORD_SIZE must match serialized scaffold removal layout"
);
_Static_assert(
    1 + 4 * 2 == SCAFFOLD_MOTION_Q_RECORD_SIZE,
    "SCAFFOLD_MOTION_Q_RECORD_SIZE must match serialized scaffold q motion layout"
);
_Static_assert(
    4 + 6 * 4 + 2 + 2 + 2 + 1 + 1 + 2 == CARGO_POD_RECORD_SIZE,
    "CARGO_POD_RECORD_SIZE must match serialized cargo pod layout"
);
_Static_assert(
    1 + 5 * 4 == CARGO_POD_MOTION_RECORD_SIZE,
    "CARGO_POD_MOTION_RECORD_SIZE must match serialized cargo pod motion layout"
);
_Static_assert(
    1 + 4 * 2 + 2 == CARGO_POD_MOTION_Q_RECORD_SIZE,
    "CARGO_POD_MOTION_Q_RECORD_SIZE must match serialized cargo pod q motion layout"
);
_Static_assert(
    1 + 4 * 2 == CARGO_POD_LINEAR_Q_RECORD_SIZE,
    "CARGO_POD_LINEAR_Q_RECORD_SIZE must match serialized cargo pod linear q layout"
);
_Static_assert(
    1 == CARGO_POD_REMOVE_RECORD_SIZE,
    "CARGO_POD_REMOVE_RECORD_SIZE must match serialized cargo pod removal layout"
);
_Static_assert(
    1 + 4 * 2 + 2 + 1 == INTERACTION_DRIFT_RECORD_SIZE,
    "INTERACTION_DRIFT_RECORD_SIZE must match serialized interaction drift layout"
);
_Static_assert(
    4 + 5 + 5 + 4 * 2 + 2 + 1 == INTERACTION_Q_RECORD_SIZE,
    "INTERACTION_Q_RECORD_SIZE must match serialized compact interaction layout"
);
_Static_assert(
    2 + 5 * 4 + 2 + 2 + 3 + 8 + 1 == NPC_RECORD_SIZE,
    "NPC_RECORD_SIZE must match serialized NPC layout"
);
_Static_assert(
    1 + 1 + 5 * 4 == NPC_MOTION_RECORD_SIZE,
    "NPC_MOTION_RECORD_SIZE must match serialized NPC motion layout"
);
_Static_assert(
    1 + 1 + 5 * 2 == NPC_MOTION_Q_RECORD_SIZE,
    "NPC_MOTION_Q_RECORD_SIZE must match serialized quantized NPC motion layout"
);
_Static_assert(
    1 + 1 + 2 * 2 + 2 + 1 == NPC_MOTION8_Q_RECORD_SIZE,
    "NPC_MOTION8_Q_RECORD_SIZE must match serialized compact NPC motion layout"
);
_Static_assert(
    1 + 2 * 2 == NPC_POS_Q_RECORD_SIZE,
    "NPC_POS_Q_RECORD_SIZE must match serialized quantized NPC position layout"
);
_Static_assert(
    1 + 2 * 2 + 2 == NPC_POSE_Q_RECORD_SIZE,
    "NPC_POSE_Q_RECORD_SIZE must match serialized quantized NPC pose layout"
);
_Static_assert(
    1 + 2 * 2 + 2 * 2 == NPC_LINEAR_Q_RECORD_SIZE,
    "NPC_LINEAR_Q_RECORD_SIZE must match serialized quantized NPC linear layout"
);
_Static_assert(
    1 + 1 + 2 + 2 == NPC_STATUS_RECORD_SIZE,
    "NPC_STATUS_RECORD_SIZE must match serialized NPC status layout"
);
_Static_assert(
    1 + 1 + 1 + 1 == NPC_STATUS8_RECORD_SIZE,
    "NPC_STATUS8_RECORD_SIZE must match serialized compact NPC status layout"
);
_Static_assert(
    1 + COMMODITY_COUNT * 4 + 4 == STATION_RECORD_SIZE,
    "STATION_RECORD_SIZE must match serialized station econ layout"
);
_Static_assert(
    COMMODITY_COUNT < 15,
    "STATION_Q uses uint16 bits for commodities plus a credit-pool flag"
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

static inline bool station_q_value_nonzero(const uint8_t *p) {
    if (!p) return false;
    float value = read_f32_le(p);
    return value != 0.0f || isnan(value);
}

static inline int serialize_stations_q_from_full(uint8_t *buf,
                                                 const uint8_t *full,
                                                 int full_len) {
    if (!buf || !full || full_len < 2 || full[0] != NET_MSG_WORLD_STATIONS)
        return 0;
    uint8_t count = full[1];
    int expected = 2 + (int)count * STATION_RECORD_SIZE;
    if (full_len < expected || expected < 2)
        return 0;

    int off = STATION_Q_HEADER_SIZE;
    buf[0] = NET_MSG_WORLD_STATIONS_Q;
    buf[1] = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *src = &full[2 + (int)i * STATION_RECORD_SIZE];
        uint16_t mask = 0;
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            if (station_q_value_nonzero(&src[1 + c * 4]))
                mask |= (uint16_t)(1u << c);
        }
        if (station_q_value_nonzero(&src[1 + COMMODITY_COUNT * 4]))
            mask |= STATION_Q_CREDIT_POOL_MASK;

        int present = 0;
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            if (mask & (uint16_t)(1u << c)) present++;
        }
        int need = 1 + 2 + present * 4 +
            ((mask & STATION_Q_CREDIT_POOL_MASK) ? 4 : 0);
        if (off + need > STATION_Q_MAX_SIZE)
            return 0;

        buf[off++] = src[0];
        write_u16_le(&buf[off], mask);
        off += 2;
        for (int c = 0; c < COMMODITY_COUNT; c++) {
            if ((mask & (uint16_t)(1u << c)) == 0)
                continue;
            memcpy(&buf[off], &src[1 + c * 4], 4);
            off += 4;
        }
        if (mask & STATION_Q_CREDIT_POOL_MASK) {
            memcpy(&buf[off], &src[1 + COMMODITY_COUNT * 4], 4);
            off += 4;
        }
    }
    return off;
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

static inline bool station_identity_q_copy(uint8_t *buf,
                                           int *off,
                                           const uint8_t *src,
                                           int len) {
    if (!buf || !off || !src || len < 0) return false;
    if (*off < 0 || *off + len > STATION_IDENTITY_Q_MAX_SIZE)
        return false;
    memcpy(&buf[*off], src, (size_t)len);
    *off += len;
    return true;
}

static inline bool station_identity_q_put_string(uint8_t *buf,
                                                 int *off,
                                                 const uint8_t *src,
                                                 int cap_without_nul) {
    if (!buf || !off || !src || cap_without_nul < 0 ||
        cap_without_nul > 255) {
        return false;
    }
    int n = 0;
    while (n < cap_without_nul && src[n] != 0) n++;
    if (*off < 0 || *off + 1 + n > STATION_IDENTITY_Q_MAX_SIZE)
        return false;
    buf[(*off)++] = (uint8_t)n;
    if (n > 0) {
        memcpy(&buf[*off], src, (size_t)n);
        *off += n;
    }
    return true;
}

static inline int serialize_station_identity_q_from_full(uint8_t *buf,
                                                         const uint8_t *full,
                                                         int full_len) {
    if (!buf || !full || full_len < STATION_IDENTITY_SIZE ||
        full[0] != NET_MSG_STATION_IDENTITY) {
        return 0;
    }

    int off = 0;
    buf[off++] = NET_MSG_STATION_IDENTITY_Q;
    buf[off++] = full[1];
    buf[off++] = full[2];
    if (!station_identity_q_copy(buf, &off, &full[3], 24)) return 0;
    if (!station_identity_q_put_string(buf, &off, &full[27], 31)) return 0;

    int src = 59;
    int price_bytes = COMMODITY_COUNT * 4 + 4;
    if (!station_identity_q_copy(buf, &off, &full[src], price_bytes))
        return 0;
    src += price_bytes;

    int module_count = full[src];
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    if (off + 1 + module_count * STATION_MODULE_RECORD_SIZE >
        STATION_IDENTITY_Q_MAX_SIZE) {
        return 0;
    }
    buf[off++] = (uint8_t)module_count;
    if (module_count > 0 &&
        !station_identity_q_copy(buf, &off, &full[src + 1],
                                 module_count * STATION_MODULE_RECORD_SIZE)) {
        return 0;
    }
    src += 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE;

    int arm_count = full[src];
    if (arm_count < 0) arm_count = 0;
    if (arm_count > MAX_ARMS) arm_count = MAX_ARMS;
    if (off + 1 + arm_count * 16 > STATION_IDENTITY_Q_MAX_SIZE)
        return 0;
    buf[off++] = (uint8_t)arm_count;
    const uint8_t *arm_speed = &full[src + 1];
    const uint8_t *ring_offset = arm_speed + MAX_ARMS * 4;
    const uint8_t *arm_rotation = ring_offset + MAX_ARMS * 4;
    const uint8_t *arm_omega = arm_rotation + MAX_ARMS * 4;
    for (int a = 0; a < arm_count; a++) {
        if (!station_identity_q_copy(buf, &off, &arm_speed[a * 4], 4) ||
            !station_identity_q_copy(buf, &off, &ring_offset[a * 4], 4) ||
            !station_identity_q_copy(buf, &off, &arm_rotation[a * 4], 4) ||
            !station_identity_q_copy(buf, &off, &arm_omega[a * 4], 4)) {
            return 0;
        }
    }
    src += 1 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4;

    int plan_count = full[src];
    if (plan_count < 0) plan_count = 0;
    if (plan_count > STATION_PLAN_RECORD_COUNT)
        plan_count = STATION_PLAN_RECORD_COUNT;
    if (off + 1 + plan_count * STATION_PLAN_RECORD_SIZE >
        STATION_IDENTITY_Q_MAX_SIZE) {
        return 0;
    }
    buf[off++] = (uint8_t)plan_count;
    if (plan_count > 0 &&
        !station_identity_q_copy(buf, &off, &full[src + 1],
                                 plan_count * STATION_PLAN_RECORD_SIZE)) {
        return 0;
    }
    src += 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE;

    int pending_scaffold_count = full[src];
    if (pending_scaffold_count < 0) pending_scaffold_count = 0;
    if (pending_scaffold_count > STATION_PENDING_SCAFFOLD_RECORD_COUNT)
        pending_scaffold_count = STATION_PENDING_SCAFFOLD_RECORD_COUNT;
    if (off + 1 + pending_scaffold_count * STATION_PENDING_SCAFFOLD_RECORD_SIZE >
        STATION_IDENTITY_Q_MAX_SIZE) {
        return 0;
    }
    buf[off++] = (uint8_t)pending_scaffold_count;
    if (pending_scaffold_count > 0 &&
        !station_identity_q_copy(
            buf, &off, &full[src + 1],
            pending_scaffold_count * STATION_PENDING_SCAFFOLD_RECORD_SIZE)) {
        return 0;
    }
    src += 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT *
        STATION_PENDING_SCAFFOLD_RECORD_SIZE;

    int pending_ship_count = full[src];
    if (pending_ship_count < 0) pending_ship_count = 0;
    if (pending_ship_count > STATION_PENDING_SHIP_RECORD_COUNT)
        pending_ship_count = STATION_PENDING_SHIP_RECORD_COUNT;
    if (off + 1 + pending_ship_count * STATION_PENDING_SHIP_RECORD_SIZE >
        STATION_IDENTITY_Q_MAX_SIZE) {
        return 0;
    }
    buf[off++] = (uint8_t)pending_ship_count;
    if (pending_ship_count > 0 &&
        !station_identity_q_copy(
            buf, &off, &full[src + 1],
            pending_ship_count * STATION_PENDING_SHIP_RECORD_SIZE)) {
        return 0;
    }
    src += 1 + STATION_PENDING_SHIP_RECORD_COUNT *
        STATION_PENDING_SHIP_RECORD_SIZE;

    if (!station_identity_q_put_string(
            buf, &off, &full[src], STATION_IDENTITY_HAIL_MESSAGE_LEN - 1)) {
        return 0;
    }
    src += STATION_IDENTITY_HAIL_MESSAGE_LEN;
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        if (!station_identity_q_put_string(
                buf, &off, &full[src],
                STATION_IDENTITY_CHATTER_LINE_LEN - 1)) {
            return 0;
        }
        src += STATION_IDENTITY_CHATTER_LINE_LEN;
    }
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        if (!station_identity_q_put_string(
                buf, &off, &full[src],
                STATION_IDENTITY_CHATTER_LINE_LEN - 1)) {
            return 0;
        }
        src += STATION_IDENTITY_CHATTER_LINE_LEN;
    }
    if (!station_identity_q_put_string(
            buf, &off, &full[src], STATION_IDENTITY_RATI_HAIL_LEN - 1)) {
        return 0;
    }
    src += STATION_IDENTITY_RATI_HAIL_LEN;
    if (!station_identity_q_put_string(
            buf, &off, &full[src], STATION_IDENTITY_CURRENCY_NAME_LEN - 1)) {
        return 0;
    }
    src += STATION_IDENTITY_CURRENCY_NAME_LEN;

    if (!station_identity_q_copy(buf, &off, &full[src],
                                 STATION_IDENTITY_PUBKEY_LEN)) {
        return 0;
    }
    src += STATION_IDENTITY_PUBKEY_LEN;
    if (!station_identity_q_copy(buf, &off, &full[src], HULL_CLASS_COUNT))
        return 0;
    src += HULL_CLASS_COUNT;
    if (!station_identity_q_copy(buf, &off, &full[src],
                                 STATION_IDENTITY_FACTION_SIZE)) {
        return 0;
    }
    src += STATION_IDENTITY_FACTION_SIZE;

    int policy_count = full[src];
    if (policy_count < 0) policy_count = 0;
    if (policy_count > STATION_IDENTITY_POLICY_CARD_COUNT)
        policy_count = STATION_IDENTITY_POLICY_CARD_COUNT;
    if (off + 1 + policy_count > STATION_IDENTITY_Q_MAX_SIZE)
        return 0;
    buf[off++] = (uint8_t)policy_count;
    if (policy_count > 0 &&
        !station_identity_q_copy(buf, &off, &full[src + 1], policy_count)) {
        return 0;
    }
    return off;
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

static inline uint64_t scaffold_net_sig(int index, const scaffold_t *sc) {
    uint8_t rec[SCAFFOLD_RECORD_SIZE];
    serialize_one_scaffold(rec, index, sc);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < SCAFFOLD_RECORD_SIZE; i++) {
        bool ignored_byte = i >= 4 && i < 20; /* pos + vel */
        if (!ignored_byte)
            h = net_fnv1a64_update(h, rec[i]);
    }
    return h;
}

static inline void serialize_one_scaffold_motion_q(uint8_t *p,
                                                   int index,
                                                   const scaffold_t *sc) {
    p[0] = (uint8_t)index;
    asteroid_motion_q_write_i16(
        &p[1], asteroid_motion_q_encode(sc->pos.x,
                                        SCAFFOLD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[3], asteroid_motion_q_encode(sc->pos.y,
                                        SCAFFOLD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[5], asteroid_motion_q_encode(sc->vel.x,
                                        SCAFFOLD_MOTION_Q_VEL_SCALE));
    asteroid_motion_q_write_i16(
        &p[7], asteroid_motion_q_encode(sc->vel.y,
                                        SCAFFOLD_MOTION_Q_VEL_SCALE));
}

static inline uint64_t scaffold_motion_q_sig(int index,
                                             const scaffold_t *sc) {
    uint8_t rec[SCAFFOLD_MOTION_Q_RECORD_SIZE];
    serialize_one_scaffold_motion_q(rec, index, sc);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < SCAFFOLD_MOTION_Q_RECORD_SIZE; i++)
        h = net_fnv1a64_update(h, rec[i]);
    return h;
}

static inline int serialize_scaffolds_for_player_delta(
    uint8_t *buf,
    uint8_t *remove_buf,
    int *remove_len_out,
    const scaffold_t *scaffolds,
    vec2 player_pos,
    bool *sent,
    uint64_t *sent_sig,
    uint64_t *motion_sent_sig) {
    int count = 0;
    int remove_count = 0;
    if (remove_len_out) *remove_len_out = 0;
    if (!buf || !scaffolds || !sent || !sent_sig) return 0;

    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &scaffolds[i];
        bool in_view = sc->active &&
            serialize_relevance_in_player_view(sc->pos, player_pos);
        if (in_view) {
            uint64_t sig = scaffold_net_sig(i, sc);
            if (!sent[i] || sent_sig[i] != sig) {
                serialize_one_scaffold(
                    &buf[2 + count * SCAFFOLD_RECORD_SIZE], i, sc);
                count++;
                sent[i] = true;
                sent_sig[i] = sig;
                if (motion_sent_sig)
                    motion_sent_sig[i] = scaffold_motion_q_sig(i, sc);
            }
        } else if (sent[i]) {
            if (remove_buf) {
                remove_buf[SCAFFOLD_REMOVE_MSG_HEADER +
                           remove_count * SCAFFOLD_REMOVE_RECORD_SIZE] =
                    (uint8_t)i;
                remove_count++;
            }
            sent[i] = false;
            sent_sig[i] = 0;
            if (motion_sent_sig) motion_sent_sig[i] = 0;
        }
    }

    buf[0] = NET_MSG_WORLD_SCAFFOLDS;
    buf[1] = (uint8_t)count;
    if (remove_buf) {
        remove_buf[0] = NET_MSG_WORLD_SCAFFOLD_REMOVE;
        remove_buf[1] = (uint8_t)remove_count;
        if (remove_len_out) {
            *remove_len_out = SCAFFOLD_REMOVE_MSG_HEADER +
                remove_count * SCAFFOLD_REMOVE_RECORD_SIZE;
        }
    }
    return 2 + count * SCAFFOLD_RECORD_SIZE;
}

static inline int serialize_scaffold_motion_q_for_player_delta(
    uint8_t *buf,
    const scaffold_t *scaffolds,
    vec2 player_pos,
    const bool *sent,
    uint64_t *motion_sent_sig) {
    int count = 0;
    if (!buf || !scaffolds || !sent || !motion_sent_sig) return 0;

    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &scaffolds[i];
        if (!sent[i]) continue;
        if (!sc->active ||
            !serialize_relevance_in_player_view(sc->pos, player_pos)) {
            motion_sent_sig[i] = 0;
            continue;
        }
        uint64_t sig = scaffold_motion_q_sig(i, sc);
        if (motion_sent_sig[i] == sig) continue;
        serialize_one_scaffold_motion_q(
            &buf[SCAFFOLD_MOTION_Q_MSG_HEADER +
                 count * SCAFFOLD_MOTION_Q_RECORD_SIZE],
            i, sc);
        motion_sent_sig[i] = sig;
        count++;
    }

    buf[0] = NET_MSG_WORLD_SCAFFOLD_MOTION_Q;
    buf[1] = (uint8_t)count;
    return SCAFFOLD_MOTION_Q_MSG_HEADER +
           count * SCAFFOLD_MOTION_Q_RECORD_SIZE;
}

/*
 * WORLD_CARGO_PODS message: active engine-less towable cargo bodies.
 * [type:1][count:1] + count * CARGO_POD_RECORD_SIZE
 */
static inline void cargo_pod_summary_fields(const cargo_pod_t *pod,
                                            uint8_t *flags_out,
                                            uint8_t *best_grade_out) {
    uint8_t flags = 0;
    uint8_t best_grade = (uint8_t)MINING_GRADE_COMMON;
    if (pod) {
        if (pod->shipment_id != 0)
            flags |= CARGO_POD_SUMMARY_SHIPMENT_BOUND;
        if (pod->manifest_count > 0 &&
            pod->manifest_count <= CARGO_POD_MANIFEST_CAP) {
            for (uint16_t i = 0; i < pod->manifest_count; i++) {
                uint8_t grade = pod->manifest_units[i].grade;
                if (grade < (uint8_t)MINING_GRADE_COUNT &&
                    grade > best_grade) {
                    best_grade = grade;
                }
            }
        }
        if (cargo_pod_has_exact_manifest(pod, pod->commodity))
            flags |= CARGO_POD_SUMMARY_EXACT_MATERIAL;
    }
    if (flags_out) *flags_out = flags;
    if (best_grade_out) *best_grade_out = best_grade;
}

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
    uint8_t best_grade = 0;
    cargo_pod_summary_fields(pod, &flags, &best_grade);
    p[34] = flags;
    p[35] = best_grade;
    p[36] = pod->tractor_station;
    p[37] = pod->tractor_module;
}

static inline uint16_t cargo_pod_motion_q_encode_rotation(float rotation);

static inline void serialize_one_cargo_pod_q(uint8_t *p,
                                             int index,
                                             const cargo_pod_t *pod) {
    p[0] = (uint8_t)index;
    p[1] = (uint8_t)pod->kind;
    p[2] = (uint8_t)pod->commodity;
    p[3] = (pod->towed_by < 0) ? 0xFFu : (uint8_t)pod->towed_by;
    asteroid_motion_q_write_i16(
        &p[4], asteroid_motion_q_encode(pod->pos.x,
                                        CARGO_POD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[6], asteroid_motion_q_encode(pod->pos.y,
                                        CARGO_POD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[8], asteroid_motion_q_encode(pod->vel.x,
                                        CARGO_POD_MOTION_Q_VEL_SCALE));
    asteroid_motion_q_write_i16(
        &p[10], asteroid_motion_q_encode(pod->vel.y,
                                         CARGO_POD_MOTION_Q_VEL_SCALE));
    write_f32_le(&p[12], pod->radius);
    write_u16_le(&p[16], cargo_pod_motion_q_encode_rotation(pod->rotation));
    write_u16_le(&p[18], pod->quantity);
    write_u16_le(&p[20], pod->manifest_count);
    write_u16_le(&p[22], pod->shipment_id);
    uint8_t flags = 0;
    uint8_t best_grade = 0;
    cargo_pod_summary_fields(pod, &flags, &best_grade);
    p[24] = flags;
    p[25] = best_grade;
    p[26] = pod->tractor_station;
    p[27] = pod->tractor_module;
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

static inline int serialize_cargo_pods_q(uint8_t *buf,
                                         const cargo_pod_t *pods) {
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!pods[i].active) continue;
        serialize_one_cargo_pod_q(&buf[2 + count * CARGO_POD_Q_RECORD_SIZE],
                                  i, &pods[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_CARGO_PODS_Q;
    buf[1] = (uint8_t)count;
    return 2 + count * CARGO_POD_Q_RECORD_SIZE;
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

static inline int serialize_cargo_pods_q_for_player(uint8_t *buf,
                                                    const cargo_pod_t *pods,
                                                    vec2 player_pos) {
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!pods[i].active) continue;
        if (!serialize_relevance_in_player_view(pods[i].pos, player_pos))
            continue;
        serialize_one_cargo_pod_q(&buf[2 + count * CARGO_POD_Q_RECORD_SIZE],
                                  i, &pods[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_CARGO_PODS_Q;
    buf[1] = (uint8_t)count;
    return 2 + count * CARGO_POD_Q_RECORD_SIZE;
}

static inline uint64_t cargo_pod_net_semantic_sig(
    int index,
    const cargo_pod_t *pod) {
    uint8_t rec[CARGO_POD_RECORD_SIZE];
    serialize_one_cargo_pod(rec, index, pod);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < CARGO_POD_RECORD_SIZE; i++) {
        bool ignored_byte =
            (i >= 4 && i < 20) ||  /* pos + vel */
            (i >= 24 && i < 28);   /* rotation */
        if (!ignored_byte)
            h = net_fnv1a64_update(h, rec[i]);
    }
    return h;
}

static inline int serialize_cargo_pods_for_player_delta(
    uint8_t *buf,
    uint8_t *remove_buf,
    int *remove_len_out,
    const cargo_pod_t *pods,
    vec2 player_pos,
    bool *sent,
    uint64_t *sent_sig,
    bool refresh_all_known) {
    int count = 0;
    int remove_count = 0;
    if (remove_len_out) *remove_len_out = 0;
    if (!buf || !pods || !sent || !sent_sig) return 0;

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &pods[i];
        bool in_view = pod->active &&
            serialize_relevance_in_player_view(pod->pos, player_pos);
        if (in_view) {
            uint64_t sig = cargo_pod_net_semantic_sig(i, pod);
            if (!sent[i] || sent_sig[i] != sig || refresh_all_known) {
                serialize_one_cargo_pod(
                    &buf[2 + count * CARGO_POD_RECORD_SIZE], i, pod);
                count++;
                sent[i] = true;
                sent_sig[i] = sig;
            }
        } else if (sent[i]) {
            if (remove_buf) {
                remove_buf[CARGO_POD_REMOVE_MSG_HEADER +
                           remove_count * CARGO_POD_REMOVE_RECORD_SIZE] =
                    (uint8_t)i;
                remove_count++;
            }
            sent[i] = false;
            sent_sig[i] = 0;
        }
    }

    buf[0] = NET_MSG_WORLD_CARGO_PODS;
    buf[1] = (uint8_t)count;
    if (remove_buf) {
        remove_buf[0] = NET_MSG_WORLD_CARGO_POD_REMOVE;
        remove_buf[1] = (uint8_t)remove_count;
        if (remove_len_out) {
            *remove_len_out = CARGO_POD_REMOVE_MSG_HEADER +
                remove_count * CARGO_POD_REMOVE_RECORD_SIZE;
        }
    }
    return 2 + count * CARGO_POD_RECORD_SIZE;
}

static inline int serialize_cargo_pods_q_for_player_delta(
    uint8_t *buf,
    uint8_t *remove_buf,
    int *remove_len_out,
    const cargo_pod_t *pods,
    vec2 player_pos,
    bool *sent,
    uint64_t *sent_sig,
    bool refresh_all_known) {
    int count = 0;
    int remove_count = 0;
    if (remove_len_out) *remove_len_out = 0;
    if (!buf || !pods || !sent || !sent_sig) return 0;

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &pods[i];
        bool in_view = pod->active &&
            serialize_relevance_in_player_view(pod->pos, player_pos);
        if (in_view) {
            uint64_t sig = cargo_pod_net_semantic_sig(i, pod);
            if (!sent[i] || sent_sig[i] != sig || refresh_all_known) {
                serialize_one_cargo_pod_q(
                    &buf[2 + count * CARGO_POD_Q_RECORD_SIZE], i, pod);
                count++;
                sent[i] = true;
                sent_sig[i] = sig;
            }
        } else if (sent[i]) {
            if (remove_buf) {
                remove_buf[CARGO_POD_REMOVE_MSG_HEADER +
                           remove_count * CARGO_POD_REMOVE_RECORD_SIZE] =
                    (uint8_t)i;
                remove_count++;
            }
            sent[i] = false;
            sent_sig[i] = 0;
        }
    }

    buf[0] = NET_MSG_WORLD_CARGO_PODS_Q;
    buf[1] = (uint8_t)count;
    if (remove_buf) {
        remove_buf[0] = NET_MSG_WORLD_CARGO_POD_REMOVE;
        remove_buf[1] = (uint8_t)remove_count;
        if (remove_len_out) {
            *remove_len_out = CARGO_POD_REMOVE_MSG_HEADER +
                remove_count * CARGO_POD_REMOVE_RECORD_SIZE;
        }
    }
    return 2 + count * CARGO_POD_Q_RECORD_SIZE;
}

static inline void serialize_one_cargo_pod_motion(uint8_t *p,
                                                  int index,
                                                  const cargo_pod_t *pod) {
    p[0] = (uint8_t)index;
    write_f32_le(&p[1], pod->pos.x);
    write_f32_le(&p[5], pod->pos.y);
    write_f32_le(&p[9], pod->vel.x);
    write_f32_le(&p[13], pod->vel.y);
    write_f32_le(&p[17], pod->rotation);
}

static inline int serialize_cargo_pod_motion_for_player(uint8_t *buf,
                                                        const cargo_pod_t *pods,
                                                        vec2 player_pos) {
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!pods[i].active) continue;
        if (!serialize_relevance_in_player_view(pods[i].pos, player_pos))
            continue;
        serialize_one_cargo_pod_motion(
            &buf[CARGO_POD_MOTION_MSG_HEADER +
                 count * CARGO_POD_MOTION_RECORD_SIZE],
            i, &pods[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_CARGO_POD_MOTION;
    buf[1] = (uint8_t)count;
    return CARGO_POD_MOTION_MSG_HEADER +
           count * CARGO_POD_MOTION_RECORD_SIZE;
}

static inline uint16_t cargo_pod_motion_q_encode_rotation(float rotation) {
    const float two_pi = 6.28318530717958647692f;
    if (!isfinite(rotation)) return 0;
    float wrapped = fmodf(rotation, two_pi);
    if (wrapped < 0.0f) wrapped += two_pi;
    float q = (wrapped / two_pi) * 65536.0f;
    if (q >= 65536.0f) q = 0.0f;
    return (uint16_t)q;
}

static inline void serialize_one_cargo_pod_motion_q(uint8_t *p,
                                                    int index,
                                                    const cargo_pod_t *pod) {
    p[0] = (uint8_t)index;
    asteroid_motion_q_write_i16(
        &p[1], asteroid_motion_q_encode(pod->pos.x,
                                        CARGO_POD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[3], asteroid_motion_q_encode(pod->pos.y,
                                        CARGO_POD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[5], asteroid_motion_q_encode(pod->vel.x,
                                        CARGO_POD_MOTION_Q_VEL_SCALE));
    asteroid_motion_q_write_i16(
        &p[7], asteroid_motion_q_encode(pod->vel.y,
                                        CARGO_POD_MOTION_Q_VEL_SCALE));
    write_u16_le(&p[9], cargo_pod_motion_q_encode_rotation(pod->rotation));
}

static inline void serialize_one_cargo_pod_linear_q(uint8_t *p,
                                                    int index,
                                                    vec2 pos,
                                                    vec2 vel) {
    p[0] = (uint8_t)index;
    asteroid_motion_q_write_i16(
        &p[1], asteroid_motion_q_encode(pos.x,
                                        CARGO_POD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[3], asteroid_motion_q_encode(pos.y,
                                        CARGO_POD_MOTION_Q_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[5], asteroid_motion_q_encode(vel.x,
                                        CARGO_POD_MOTION_Q_VEL_SCALE));
    asteroid_motion_q_write_i16(
        &p[7], asteroid_motion_q_encode(vel.y,
                                        CARGO_POD_MOTION_Q_VEL_SCALE));
}

static inline int serialize_cargo_pod_motion_q_for_player(
    uint8_t *buf,
    const cargo_pod_t *pods,
    vec2 player_pos) {
    int count = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!pods[i].active) continue;
        if (!serialize_relevance_in_player_view(pods[i].pos, player_pos))
            continue;
        serialize_one_cargo_pod_motion_q(
            &buf[CARGO_POD_MOTION_Q_MSG_HEADER +
                 count * CARGO_POD_MOTION_Q_RECORD_SIZE],
            i, &pods[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_CARGO_POD_MOTION_Q;
    buf[1] = (uint8_t)count;
    return CARGO_POD_MOTION_Q_MSG_HEADER +
           count * CARGO_POD_MOTION_Q_RECORD_SIZE;
}

#define CARGO_POD_MOTION_NET_REPEAT_TICKS 240u /* ~0.5 Hz visual correction */
#define CARGO_POD_MOTION_PREDICT_ERROR_SQ (32.0f * 32.0f)
#define CARGO_POD_MOTION_VEL_ERROR_SQ (8.0f * 8.0f)
#define CARGO_POD_MOTION_ROT_ERROR 1.25f
#define CARGO_POD_MOTION_HEARTBEAT_TICKS 720u /* 0.17 Hz clean-motion safety refresh */
#define CARGO_POD_NET_METADATA_HEARTBEAT_TICKS 2400u /* 0.05 Hz metadata reconciliation */

static inline bool cargo_pod_net_metadata_refresh_due(uint32_t last_sent_tick,
                                                      uint32_t world_tick) {
    return last_sent_tick == 0 ||
        (uint32_t)(world_tick - last_sent_tick) >=
            CARGO_POD_NET_METADATA_HEARTBEAT_TICKS;
}

static inline float cargo_pod_motion_rotation_delta(float a, float b) {
    const float two_pi = 6.28318530717958647692f;
    if (!isfinite(a) || !isfinite(b)) return CARGO_POD_MOTION_ROT_ERROR;
    float d = fabsf(a - b);
    while (d > two_pi) d -= two_pi;
    if (d < 0.0f) d = -d;
    if (d > 3.14159265358979323846f) d = two_pi - d;
    return d;
}

static inline bool cargo_pod_motion_should_send(
    const server_player_t *sp,
    uint8_t index,
    vec2 pos,
    vec2 vel,
    float rotation,
    uint32_t server_tick) {
    if (!sp || index >= MAX_CARGO_PODS) return false;
    uint32_t last_tick = sp->cargo_pod_motion_sent_tick[index];
    if (last_tick == 0u) return true;
    uint32_t age_ticks = server_tick - last_tick;
    if (age_ticks >= CARGO_POD_MOTION_HEARTBEAT_TICKS)
        return true;

    float dt = (float)age_ticks * SIM_DT;
    vec2 predicted_pos = v2_add(
        sp->cargo_pod_motion_sent_pos[index],
        v2_scale(sp->cargo_pod_motion_sent_vel[index], dt));
    if (v2_dist_sq(predicted_pos, pos) >= CARGO_POD_MOTION_PREDICT_ERROR_SQ)
        return true;
    if (v2_dist_sq(sp->cargo_pod_motion_sent_vel[index], vel) >=
        CARGO_POD_MOTION_VEL_ERROR_SQ)
        return true;
    return cargo_pod_motion_rotation_delta(
        sp->cargo_pod_motion_sent_rotation[index], rotation) >=
        CARGO_POD_MOTION_ROT_ERROR;
}

static inline bool cargo_pod_motion_linear_q_eligible(
    const server_player_t *sp,
    uint8_t index,
    float rotation,
    uint32_t server_tick) {
    if (!sp || index >= MAX_CARGO_PODS) return false;
    uint32_t last_tick = sp->cargo_pod_motion_sent_tick[index];
    if (last_tick == 0u) return false;
    uint32_t age_ticks = server_tick - last_tick;
    if (age_ticks >= CARGO_POD_MOTION_HEARTBEAT_TICKS)
        return false;
    return cargo_pod_motion_rotation_delta(
        sp->cargo_pod_motion_sent_rotation[index], rotation) <
        CARGO_POD_MOTION_ROT_ERROR;
}

static inline void cargo_pod_motion_note_sent(server_player_t *sp,
                                              uint8_t index,
                                              vec2 pos,
                                              vec2 vel,
                                              float rotation,
                                              uint32_t server_tick) {
    if (!sp || index >= MAX_CARGO_PODS) return;
    sp->cargo_pod_motion_sent_tick[index] = server_tick;
    sp->cargo_pod_motion_sent_pos[index] = pos;
    sp->cargo_pod_motion_sent_vel[index] = vel;
    sp->cargo_pod_motion_sent_rotation[index] = rotation;
}

static inline uint64_t net_world_cargo_pods_semantic_hash(const uint8_t *data,
                                                          int len) {
    if (!data || len <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    if (len < 2 ||
        (data[0] != NET_MSG_WORLD_CARGO_PODS &&
         data[0] != NET_MSG_WORLD_CARGO_PODS_Q)) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int count = data[1];
    bool compact = data[0] == NET_MSG_WORLD_CARGO_PODS_Q;
    int record_size = compact ? CARGO_POD_Q_RECORD_SIZE :
        CARGO_POD_RECORD_SIZE;
    int expected = 2 + count * record_size;
    if (len < expected) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    for (int i = 0; i < len; i++) {
        bool ignored_byte = false;
        if (i >= 2 && i < expected) {
            int record_off = (i - 2) % record_size;
            if (compact) {
                ignored_byte =
                    (record_off >= 4 && record_off < 12) ||  /* pos + vel */
                    (record_off >= 16 && record_off < 18);    /* rotation */
            } else {
                ignored_byte =
                    (record_off >= 4 && record_off < 20) ||  /* pos + vel */
                    (record_off >= 24 && record_off < 28);    /* rotation */
            }
        }
        if (!ignored_byte)
            h = net_fnv1a64_update(h, data[i]);
    }
    return h;
}

static inline void serialize_one_interaction(uint8_t *p,
                                             const sim_interaction_t *it) {
    p[0] = it->type;
    p[1] = it->visual;
    p[2] = it->commodity;
    p[3] = it->flags;
    p[4] = it->source.type;
    write_u16_le(&p[5], (uint16_t)(int16_t)it->source.index);
    write_u16_le(&p[7], (uint16_t)(int16_t)it->source.aux);
    p[9] = it->target.type;
    write_u16_le(&p[10], (uint16_t)(int16_t)it->target.index);
    write_u16_le(&p[12], (uint16_t)(int16_t)it->target.aux);
    write_f32_le(&p[14], it->source_pos.x);
    write_f32_le(&p[18], it->source_pos.y);
    write_f32_le(&p[22], it->target_pos.x);
    write_f32_le(&p[26], it->target_pos.y);
    write_f32_le(&p[30], it->range);
    write_f32_le(&p[34], it->intensity);
}

static inline uint16_t interaction_drift_encode_u16(float value,
                                                    float scale) {
    if (!isfinite(value) || scale <= 0.0f || value <= 0.0f) return 0;
    float q = value / scale;
    if (q > 65535.0f) return 65535u;
    return (uint16_t)(q + 0.5f);
}

static inline uint8_t interaction_drift_encode_u8(float value) {
    if (!isfinite(value) || value <= 0.0f) return 0;
    if (value >= 1.0f) return 255u;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static inline void serialize_one_interaction_q(uint8_t *p,
                                               const sim_interaction_t *it) {
    p[0] = it->type;
    p[1] = it->visual;
    p[2] = it->commodity;
    p[3] = it->flags;
    p[4] = it->source.type;
    write_u16_le(&p[5], (uint16_t)(int16_t)it->source.index);
    write_u16_le(&p[7], (uint16_t)(int16_t)it->source.aux);
    p[9] = it->target.type;
    write_u16_le(&p[10], (uint16_t)(int16_t)it->target.index);
    write_u16_le(&p[12], (uint16_t)(int16_t)it->target.aux);
    asteroid_motion_q_write_i16(
        &p[14], asteroid_motion_q_encode(it->source_pos.x,
                                         INTERACTION_DRIFT_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[16], asteroid_motion_q_encode(it->source_pos.y,
                                         INTERACTION_DRIFT_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[18], asteroid_motion_q_encode(it->target_pos.x,
                                         INTERACTION_DRIFT_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[20], asteroid_motion_q_encode(it->target_pos.y,
                                         INTERACTION_DRIFT_POS_SCALE));
    write_u16_le(&p[22],
                 interaction_drift_encode_u16(it->range,
                                              INTERACTION_DRIFT_RANGE_SCALE));
    p[24] = interaction_drift_encode_u8(it->intensity);
}

static inline int serialize_interactions(uint8_t *buf,
                                         const sim_interactions_t *interactions) {
    int count = 0;
    if (interactions) {
        for (int i = 0; i < interactions->count &&
             count < SIM_MAX_INTERACTIONS; i++) {
            const sim_interaction_t *it = &interactions->items[i];
            if (it->type == SIM_INTERACTION_NONE) continue;
            serialize_one_interaction(
                &buf[2 + count * INTERACTION_RECORD_SIZE], it);
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_INTERACTIONS;
    buf[1] = (uint8_t)count;
    return 2 + count * INTERACTION_RECORD_SIZE;
}

static inline int serialize_interactions_q(uint8_t *buf,
                                           const sim_interactions_t *interactions) {
    int count = 0;
    if (interactions) {
        for (int i = 0; i < interactions->count &&
             count < SIM_MAX_INTERACTIONS; i++) {
            const sim_interaction_t *it = &interactions->items[i];
            if (it->type == SIM_INTERACTION_NONE) continue;
            serialize_one_interaction_q(
                &buf[2 + count * INTERACTION_Q_RECORD_SIZE], it);
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_INTERACTIONS_Q;
    buf[1] = (uint8_t)count;
    return 2 + count * INTERACTION_Q_RECORD_SIZE;
}

static inline bool serialize_interaction_relevant_to_player(
    const sim_interaction_t *it,
    vec2 player_pos) {
    if (!it || it->type == SIM_INTERACTION_NONE) return false;
    if (serialize_relevance_in_player_view(it->source_pos, player_pos))
        return true;
    if (serialize_relevance_in_player_view(it->target_pos, player_pos))
        return true;
    vec2 mid = v2((it->source_pos.x + it->target_pos.x) * 0.5f,
                  (it->source_pos.y + it->target_pos.y) * 0.5f);
    return serialize_relevance_in_player_view(mid, player_pos);
}

static inline int serialize_interactions_for_player(
    uint8_t *buf,
    const sim_interactions_t *interactions,
    vec2 player_pos) {
    int count = 0;
    if (interactions) {
        for (int i = 0; i < interactions->count &&
             count < SIM_MAX_INTERACTIONS; i++) {
            const sim_interaction_t *it = &interactions->items[i];
            if (!serialize_interaction_relevant_to_player(it, player_pos))
                continue;
            serialize_one_interaction(
                &buf[2 + count * INTERACTION_RECORD_SIZE], it);
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_INTERACTIONS;
    buf[1] = (uint8_t)count;
    return 2 + count * INTERACTION_RECORD_SIZE;
}

static inline int serialize_interactions_q_for_player(
    uint8_t *buf,
    const sim_interactions_t *interactions,
    vec2 player_pos) {
    int count = 0;
    if (interactions) {
        for (int i = 0; i < interactions->count &&
             count < SIM_MAX_INTERACTIONS; i++) {
            const sim_interaction_t *it = &interactions->items[i];
            if (!serialize_interaction_relevant_to_player(it, player_pos))
                continue;
            serialize_one_interaction_q(
                &buf[2 + count * INTERACTION_Q_RECORD_SIZE], it);
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_INTERACTIONS_Q;
    buf[1] = (uint8_t)count;
    return 2 + count * INTERACTION_Q_RECORD_SIZE;
}

#define INTERACTION_DRIFT_NET_REPEAT_TICKS 1200u /* 0.1 Hz visual drift safety refresh */
#define INTERACTION_NET_METADATA_HEARTBEAT_TICKS 2400u /* 0.05 Hz metadata reconciliation */

static inline bool interaction_drift_repeat_due(uint32_t last_tick,
                                                uint32_t world_tick) {
    return last_tick == 0 ||
        (uint32_t)(world_tick - last_tick) >=
            INTERACTION_DRIFT_NET_REPEAT_TICKS;
}

static inline bool interaction_net_metadata_refresh_due(uint32_t last_sent_tick,
                                                        uint32_t world_tick) {
    return last_sent_tick == 0 ||
        (uint32_t)(world_tick - last_sent_tick) >=
            INTERACTION_NET_METADATA_HEARTBEAT_TICKS;
}

static inline void serialize_one_interaction_drift(uint8_t *p,
                                                   int index,
                                                   const sim_interaction_t *it) {
    p[0] = (uint8_t)index;
    asteroid_motion_q_write_i16(
        &p[1], asteroid_motion_q_encode(it->source_pos.x,
                                        INTERACTION_DRIFT_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[3], asteroid_motion_q_encode(it->source_pos.y,
                                        INTERACTION_DRIFT_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[5], asteroid_motion_q_encode(it->target_pos.x,
                                        INTERACTION_DRIFT_POS_SCALE));
    asteroid_motion_q_write_i16(
        &p[7], asteroid_motion_q_encode(it->target_pos.y,
                                        INTERACTION_DRIFT_POS_SCALE));
    write_u16_le(&p[9],
                 interaction_drift_encode_u16(it->range,
                                              INTERACTION_DRIFT_RANGE_SCALE));
    p[11] = interaction_drift_encode_u8(it->intensity);
}

static inline int serialize_interaction_drift(
    uint8_t *buf,
    const sim_interactions_t *interactions) {
    int count = 0;
    if (interactions) {
        for (int i = 0; i < interactions->count &&
             count < SIM_MAX_INTERACTIONS; i++) {
            const sim_interaction_t *it = &interactions->items[i];
            if (it->type == SIM_INTERACTION_NONE) continue;
            serialize_one_interaction_drift(
                &buf[INTERACTION_DRIFT_MSG_HEADER +
                     count * INTERACTION_DRIFT_RECORD_SIZE],
                count, it);
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_INTERACTION_DRIFT;
    buf[1] = (uint8_t)count;
    return INTERACTION_DRIFT_MSG_HEADER +
           count * INTERACTION_DRIFT_RECORD_SIZE;
}

static inline int serialize_interaction_drift_for_player(
    uint8_t *buf,
    const sim_interactions_t *interactions,
    vec2 player_pos) {
    int count = 0;
    if (interactions) {
        for (int i = 0; i < interactions->count &&
             count < SIM_MAX_INTERACTIONS; i++) {
            const sim_interaction_t *it = &interactions->items[i];
            if (!serialize_interaction_relevant_to_player(it, player_pos))
                continue;
            serialize_one_interaction_drift(
                &buf[INTERACTION_DRIFT_MSG_HEADER +
                     count * INTERACTION_DRIFT_RECORD_SIZE],
                count, it);
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_INTERACTION_DRIFT;
    buf[1] = (uint8_t)count;
    return INTERACTION_DRIFT_MSG_HEADER +
           count * INTERACTION_DRIFT_RECORD_SIZE;
}

static inline uint64_t net_world_interactions_semantic_hash(const uint8_t *data,
                                                            int len) {
    if (!data || len <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    if (len < 2 ||
        (data[0] != NET_MSG_WORLD_INTERACTIONS &&
         data[0] != NET_MSG_WORLD_INTERACTIONS_Q)) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int count = data[1];
    int record_size = data[0] == NET_MSG_WORLD_INTERACTIONS_Q
        ? INTERACTION_Q_RECORD_SIZE
        : INTERACTION_RECORD_SIZE;
    int expected = 2 + count * record_size;
    if (len < expected) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    for (int i = 0; i < len; i++) {
        bool ignored_byte = false;
        if (i >= 2 && i < expected) {
            int record_off = (i - 2) % record_size;
            ignored_byte = record_off >= 14 && record_off < record_size;
        }
        if (!ignored_byte)
            h = net_fnv1a64_update(h, data[i]);
    }
    return h;
}

typedef void (*server_packet_sink_fn)(void *user, const uint8_t *data, int len);
typedef void (*server_player_packet_sink_fn)(void *user, int player_slot,
                                             const uint8_t *data, int len);

#define WORLD_TIME_REPEAT_TICKS 240u /* 0.5 Hz reconciliation at 8 ms sim tick */

#define INPUT_ACK_STATE_HEARTBEAT_TICKS 960u /* 8 s at 120 Hz */
#define INPUT_ACK_STATE_POS_ERROR_SQ (64.0f * 64.0f)
#define INPUT_ACK_STATE_VEL_ERROR_SQ (180.0f * 180.0f)
#define INPUT_ACK_STATE_ANGLE_ERROR 0.35f

static inline float input_ack_state_angle_error(float a, float b) {
    float d = a - b;
    while (d > PI_F) d -= TWO_PI_F;
    while (d < -PI_F) d += TWO_PI_F;
    return fabsf(d);
}

static inline void server_player_reset_authoritative_ack_state(
    server_player_t *sp) {
    if (!sp) return;
    sp->input_ack_state_valid = false;
    sp->input_ack_state_tick = 0;
    sp->input_ack_state_pos = v2(0.0f, 0.0f);
    sp->input_ack_state_vel = v2(0.0f, 0.0f);
    sp->input_ack_state_angle = 0.0f;
    sp->input_ack_state_flags = 0;
    sp->input_ack_state_tractor_level = 0;
    sp->input_ack_state_towed_count = 0;
    for (int i = 0; i < 10; i++)
        sp->input_ack_state_towed_fragments[i] = 0xFFFFu;
}

static inline void server_player_note_authoritative_ack_state(
    server_player_t *sp,
    uint32_t server_tick) {
    if (!sp) return;
    sp->input_ack_state_valid = true;
    sp->input_ack_state_tick = server_tick;
    sp->input_ack_state_pos = sp->ship.pos;
    sp->input_ack_state_vel = sp->ship.vel;
    sp->input_ack_state_angle = sp->ship.angle;
    sp->input_ack_state_flags = server_player_state_flags_for_wire(sp);
    sp->input_ack_state_tractor_level = (uint8_t)sp->ship.tractor_level;
    sp->input_ack_state_towed_count = sp->ship.towed_count;
    for (int i = 0; i < 10; i++) {
        int16_t fi = (i < sp->ship.towed_count)
            ? sp->ship.towed_fragments[i] : -1;
        sp->input_ack_state_towed_fragments[i] =
            (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
    }
}

static inline bool server_emit_authoritative_player_state_snapshot(
    server_player_t *sp,
    uint8_t player_slot,
    uint32_t server_tick,
    server_packet_sink_fn send,
    void *user) {
    if (!sp || !send || !server_player_is_gameplay_ready(sp))
        return false;
    uint8_t buf[NET_STATE_AUTH_SIZE];
    int len = serialize_authoritative_player_state(
        buf, player_slot, sp, server_tick);
    send(user, buf, len);
    server_player_note_authoritative_ack_state(sp, server_tick);
    return true;
}

static inline bool server_player_authoritative_ack_state_required(
    const server_player_t *sp,
    uint32_t server_tick,
    bool force_state) {
    if (!sp) return false;
    if (force_state || !sp->input_ack_state_valid) return true;
    if ((uint32_t)(server_tick - sp->input_ack_state_tick) >=
        INPUT_ACK_STATE_HEARTBEAT_TICKS) {
        return true;
    }
    vec2 dp = v2_sub(sp->ship.pos, sp->input_ack_state_pos);
    if (v2_len_sq(dp) > INPUT_ACK_STATE_POS_ERROR_SQ) return true;
    vec2 dv = v2_sub(sp->ship.vel, sp->input_ack_state_vel);
    if (v2_len_sq(dv) > INPUT_ACK_STATE_VEL_ERROR_SQ) return true;
    if (input_ack_state_angle_error(
            sp->ship.angle, sp->input_ack_state_angle) >
        INPUT_ACK_STATE_ANGLE_ERROR) {
        return true;
    }
    if (server_player_state_flags_for_wire(sp) != sp->input_ack_state_flags)
        return true;
    if ((uint8_t)sp->ship.tractor_level !=
        sp->input_ack_state_tractor_level) {
        return true;
    }
    if (sp->ship.towed_count != sp->input_ack_state_towed_count)
        return true;
    for (int i = 0; i < 10; i++) {
        int16_t fi = (i < sp->ship.towed_count)
            ? sp->ship.towed_fragments[i] : -1;
        uint16_t wire =
            (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        if (wire != sp->input_ack_state_towed_fragments[i])
            return true;
    }
    return false;
}

typedef struct {
    bool pending;
    uint16_t previous_input_seq;
    uint32_t server_tick;
} server_pending_input_ack_t;

static inline void server_pending_input_ack_reset(
    server_pending_input_ack_t *pending) {
    if (!pending) return;
    pending->pending = false;
    pending->previous_input_seq = 0;
    pending->server_tick = 0;
}

static inline bool server_pending_input_ack_note(
    server_pending_input_ack_t *pending,
    const server_player_t *sp,
    uint16_t previous_input_seq,
    uint32_t server_tick) {
    if (!pending || !sp || sp->last_input_seq == 0 ||
        sp->last_input_seq == previous_input_seq) {
        return false;
    }
    if (!pending->pending)
        pending->previous_input_seq = previous_input_seq;
    pending->server_tick = server_tick;
    pending->pending = true;
    return true;
}

static inline bool server_emit_input_applied_if_changed(
    const server_player_t *sp,
    uint16_t previous_input_seq,
    uint32_t server_tick,
    server_packet_sink_fn send,
    void *user) {
    if (!sp || !send || sp->last_input_seq == 0 ||
        sp->last_input_seq == previous_input_seq) {
        return false;
    }
    uint8_t buf[NET_INPUT_APPLIED_SIZE];
    int len = serialize_input_applied(buf, sp->last_input_seq, server_tick,
                                      sp->last_input_tick,
                                      sp->last_input_client_sent_ms,
                                      sp->last_input_server_recv_ms,
                                      0);
    send(user, buf, len);
    return true;
}

static inline bool server_emit_pending_input_applied(
    server_pending_input_ack_t *pending,
    const server_player_t *sp,
    server_packet_sink_fn send,
    void *user) {
    if (!pending || !pending->pending)
        return false;
    uint16_t previous_input_seq = pending->previous_input_seq;
    uint32_t server_tick = pending->server_tick;
    server_pending_input_ack_reset(pending);
    return server_emit_input_applied_if_changed(
        sp, previous_input_seq, server_tick, send, user);
}

static inline bool server_emit_authoritative_player_state_if_changed(
    const server_player_t *sp,
    uint8_t player_slot,
    uint16_t previous_input_seq,
    uint32_t server_tick,
    server_packet_sink_fn send,
    void *user) {
    if (!sp || !send || sp->last_input_seq == 0 ||
        sp->last_input_seq == previous_input_seq) {
        return false;
    }
    uint8_t buf[NET_STATE_AUTH_SIZE];
    int len = serialize_authoritative_player_state(buf, player_slot, sp,
                                                   server_tick);
    send(user, buf, len);
    return true;
}

static inline bool server_emit_pending_authoritative_player_state(
    server_pending_input_ack_t *pending,
    const server_player_t *sp,
    uint8_t player_slot,
    server_packet_sink_fn send,
    void *user) {
    if (!pending || !pending->pending)
        return false;
    uint16_t previous_input_seq = pending->previous_input_seq;
    uint32_t server_tick = pending->server_tick;
    server_pending_input_ack_reset(pending);
    return server_emit_authoritative_player_state_if_changed(
        sp, player_slot, previous_input_seq, server_tick, send, user);
}

static inline bool server_emit_pending_input_ack_adaptive(
    server_pending_input_ack_t *pending,
    server_player_t *sp,
    uint8_t player_slot,
    bool force_state,
    server_packet_sink_fn send,
    void *user) {
    if (!pending || !pending->pending)
        return false;
    uint16_t previous_input_seq = pending->previous_input_seq;
    uint32_t server_tick = pending->server_tick;
    bool send_state = server_player_authoritative_ack_state_required(
        sp, server_tick, force_state);
    server_pending_input_ack_reset(pending);
    if (send_state) {
        bool sent = server_emit_authoritative_player_state_if_changed(
            sp, player_slot, previous_input_seq, server_tick, send, user);
        if (sent)
            server_player_note_authoritative_ack_state(sp, server_tick);
        return sent;
    }
    return server_emit_input_applied_if_changed(
        sp, previous_input_seq, server_tick, send, user);
}

typedef struct {
    uint8_t asteroids[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    uint8_t asteroids_q[ASTEROID_Q_MSG_HEADER +
                        MAX_ASTEROIDS * ASTEROID_Q_RECORD_SIZE];
    uint8_t asteroids8_q[ASTEROID8_Q_MSG_HEADER +
                         256 * ASTEROID8_Q_RECORD_SIZE];
    uint8_t asteroid_motion[ASTEROID_MOTION_MSG_HEADER +
                            MAX_ASTEROIDS * ASTEROID_MOTION_RECORD_SIZE];
    uint8_t asteroid_motion_q[ASTEROID_MOTION_Q_MSG_HEADER +
                              MAX_ASTEROIDS * ASTEROID_MOTION_Q_RECORD_SIZE];
    uint8_t asteroid_posd_q[ASTEROID_POSD_Q_MSG_HEADER +
                            MAX_ASTEROIDS * ASTEROID_POSD_Q_RECORD_SIZE];
    uint8_t asteroid_posd8_q[ASTEROID_POSD8_Q_MSG_HEADER +
                             256 * ASTEROID_POSD8_Q_RECORD_SIZE];
    uint8_t asteroid_pos_q[ASTEROID_POS_Q_MSG_HEADER +
                           MAX_ASTEROIDS * ASTEROID_POS_Q_RECORD_SIZE];
    uint8_t asteroid_pos8_q[ASTEROID_POS8_Q_MSG_HEADER +
                            256 * ASTEROID_POS8_Q_RECORD_SIZE];
    uint8_t asteroid_state_q[ASTEROID_STATE_Q_MSG_HEADER +
                             MAX_ASTEROIDS * ASTEROID_STATE_Q_RECORD_SIZE];
    uint8_t asteroid_remove[ASTEROID_REMOVE_MSG_HEADER +
                            MAX_ASTEROIDS * ASTEROID_REMOVE_RECORD_SIZE];
    uint8_t players[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    uint8_t npcs[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    uint8_t npc_motion[NPC_MOTION_MSG_HEADER +
                       MAX_NPC_SHIPS * NPC_MOTION_RECORD_SIZE];
    uint8_t npc_motion_q[NPC_MOTION_Q_MSG_HEADER +
                         MAX_NPC_SHIPS * NPC_MOTION_Q_RECORD_SIZE];
    uint8_t npc_motion8_q[NPC_MOTION8_Q_MSG_HEADER +
                          MAX_NPC_SHIPS * NPC_MOTION8_Q_RECORD_SIZE];
    uint8_t npc_status[NPC_STATUS_MSG_HEADER +
                       MAX_NPC_SHIPS * NPC_STATUS_RECORD_SIZE];
    uint8_t npc_status8[NPC_STATUS8_MSG_HEADER +
                        MAX_NPC_SHIPS * NPC_STATUS8_RECORD_SIZE];
    uint8_t scaffolds[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
    uint8_t scaffold_motion_q[SCAFFOLD_MOTION_Q_MSG_HEADER +
                              MAX_SCAFFOLDS * SCAFFOLD_MOTION_Q_RECORD_SIZE];
    uint8_t scaffold_remove[SCAFFOLD_REMOVE_MSG_HEADER +
                            MAX_SCAFFOLDS * SCAFFOLD_REMOVE_RECORD_SIZE];
    uint8_t cargo_pods[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    uint8_t cargo_pods_q[2 + MAX_CARGO_PODS * CARGO_POD_Q_RECORD_SIZE];
    uint8_t cargo_pod_remove[CARGO_POD_REMOVE_MSG_HEADER +
                             MAX_CARGO_PODS * CARGO_POD_REMOVE_RECORD_SIZE];
    uint8_t cargo_pod_motion[CARGO_POD_MOTION_MSG_HEADER +
                             MAX_CARGO_PODS * CARGO_POD_MOTION_RECORD_SIZE];
    uint8_t cargo_pod_motion_q[CARGO_POD_MOTION_Q_MSG_HEADER +
                               MAX_CARGO_PODS * CARGO_POD_MOTION_Q_RECORD_SIZE];
    uint8_t interactions[2 + SIM_MAX_INTERACTIONS * INTERACTION_RECORD_SIZE];
    uint8_t interactions_q[2 + SIM_MAX_INTERACTIONS * INTERACTION_Q_RECORD_SIZE];
    uint8_t interaction_drift[INTERACTION_DRIFT_MSG_HEADER +
                              SIM_MAX_INTERACTIONS * INTERACTION_DRIFT_RECORD_SIZE];
    uint8_t world_time[5];
} server_world_snapshot_scratch_t;

static inline int server_live_asteroid_recipient_count(const world_t *w) {
    if (!w) return 0;
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *sp = &w->players[i];
        if (!server_player_is_gameplay_ready(sp)) continue;
        if (sp->docked) continue;
        count++;
    }
    return count;
}

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
    bool emit_live_world_drift = !sp->docked;

    if (emit_live_world_drift) {
        int asteroids_q_len = 0;
        int asteroids8_q_len = 0;
        int motion_len = 0;
        int motion_q_len = 0;
        int posd_q_len = 0;
        int posd8_q_len = 0;
        int pos_q_len = 0;
        int pos8_q_len = 0;
        int state_q_len = 0;
        int remove_len = 0;
        int background_identity_budget =
            asteroid_net_background_identity_budget_at_tick_for_players(
                w->tick, server_live_asteroid_recipient_count(w));
        int alen = serialize_asteroids_for_player_split_ext_state_budget_at_tick(
            scratch->asteroids,
            scratch->asteroids_q, &asteroids_q_len,
            scratch->asteroids8_q, &asteroids8_q_len,
            scratch->asteroid_motion, &motion_len,
            scratch->asteroid_motion_q, &motion_q_len,
            scratch->asteroid_posd_q, &posd_q_len,
            scratch->asteroid_posd8_q, &posd8_q_len,
            scratch->asteroid_pos_q, &pos_q_len,
            scratch->asteroid_pos8_q, &pos8_q_len,
            scratch->asteroid_state_q, &state_q_len,
            scratch->asteroid_remove, &remove_len,
            w->asteroids, sp->ship.pos, sp->asteroid_sent,
            sp->asteroid_motion_sent_tick, sp->asteroid_motion_sent_pos,
            sp->asteroid_motion_sent_vel, sp->asteroid_state_sent_tick,
            sp->asteroid_state_sent_sig, sp->asteroid_state_sent_semantic_sig,
            w->tick, background_identity_budget);
        if (alen > ASTEROID_MSG_HEADER)
            send(send_user, scratch->asteroids, alen);
        if (asteroids8_q_len > ASTEROID8_Q_MSG_HEADER)
            send(send_user, scratch->asteroids8_q, asteroids8_q_len);
        if (asteroids_q_len > ASTEROID_Q_MSG_HEADER)
            send(send_user, scratch->asteroids_q, asteroids_q_len);
        if (remove_len > ASTEROID_REMOVE_MSG_HEADER)
            send(send_user, scratch->asteroid_remove, remove_len);
        if (state_q_len > ASTEROID_STATE_Q_MSG_HEADER)
            send(send_user, scratch->asteroid_state_q, state_q_len);
        if (motion_len > ASTEROID_MOTION_MSG_HEADER)
            send(send_user, scratch->asteroid_motion, motion_len);
        if (motion_q_len > ASTEROID_MOTION_Q_MSG_HEADER)
            send(send_user, scratch->asteroid_motion_q, motion_q_len);
        if (posd8_q_len > ASTEROID_POSD8_Q_MSG_HEADER)
            send(send_user, scratch->asteroid_posd8_q, posd8_q_len);
        if (posd_q_len > ASTEROID_POSD_Q_MSG_HEADER)
            send(send_user, scratch->asteroid_posd_q, posd_q_len);
        if (pos8_q_len > ASTEROID_POS8_Q_MSG_HEADER)
            send(send_user, scratch->asteroid_pos8_q, pos8_q_len);
        if (pos_q_len > ASTEROID_POS_Q_MSG_HEADER)
            send(send_user, scratch->asteroid_pos_q, pos_q_len);
    }

    if (include_player_states) {
        int plen = serialize_player_states_except_recipient(
            scratch->players, w->players, player_slot, w->tick);
        send(send_user, scratch->players, plen);
    }

    int nlen = serialize_npcs_for_player(
        scratch->npcs, w->npc_ships, sp->ship.pos);
    send(send_user, scratch->npcs, nlen);
    if (emit_live_world_drift) {
        int nmotion8_q_len = serialize_npc_motion8_q_for_player(
            scratch->npc_motion8_q, w->npc_ships, sp->ship.pos);
        if (nmotion8_q_len > NPC_MOTION8_Q_MSG_HEADER)
            send(send_user, scratch->npc_motion8_q, nmotion8_q_len);
        int nstatus8_len = serialize_npc_status8_for_player(
            scratch->npc_status8, w->npc_ships, sp->ship.pos);
        if (nstatus8_len > NPC_STATUS8_MSG_HEADER) {
            send(send_user, scratch->npc_status8, nstatus8_len);
        } else {
            int nstatus_len = serialize_npc_status_for_player(
                scratch->npc_status, w->npc_ships, sp->ship.pos);
            if (nstatus_len > NPC_STATUS_MSG_HEADER)
                send(send_user, scratch->npc_status, nstatus_len);
        }
    }

    int scaffold_remove_len = 0;
    int slen = serialize_scaffolds_for_player_delta(
        scratch->scaffolds, scratch->scaffold_remove, &scaffold_remove_len,
        w->scaffolds, sp->ship.pos, sp->scaffold_sent,
        sp->scaffold_sent_sig, sp->scaffold_motion_sent_sig);
    if (slen > 2)
        send(send_user, scratch->scaffolds, slen);
    if (scaffold_remove_len > SCAFFOLD_REMOVE_MSG_HEADER)
        send(send_user, scratch->scaffold_remove, scaffold_remove_len);
    if (emit_live_world_drift) {
        int smotion_q_len = serialize_scaffold_motion_q_for_player_delta(
            scratch->scaffold_motion_q, w->scaffolds, sp->ship.pos,
            sp->scaffold_sent, sp->scaffold_motion_sent_sig);
        if (smotion_q_len > SCAFFOLD_MOTION_Q_MSG_HEADER)
            send(send_user, scratch->scaffold_motion_q, smotion_q_len);
    }

    int cargo_remove_len = 0;
    bool cargo_refresh_due = cargo_pod_net_metadata_refresh_due(
        sp->world_cargo_pods_last_sent_tick, w->tick);
    int clen = serialize_cargo_pods_q_for_player_delta(
        scratch->cargo_pods_q, scratch->cargo_pod_remove, &cargo_remove_len,
        w->cargo_pods, sp->ship.pos, sp->cargo_pod_sent,
        sp->cargo_pod_sent_sig, cargo_refresh_due);
    if (clen > 2)
        send(send_user, scratch->cargo_pods_q, clen);
    if (cargo_remove_len > CARGO_POD_REMOVE_MSG_HEADER)
        send(send_user, scratch->cargo_pod_remove, cargo_remove_len);
    if (emit_live_world_drift) {
        int cmotion_q_len = serialize_cargo_pod_motion_q_for_player(
            scratch->cargo_pod_motion_q, w->cargo_pods, sp->ship.pos);
        if (cmotion_q_len > CARGO_POD_MOTION_Q_MSG_HEADER)
            send(send_user, scratch->cargo_pod_motion_q, cmotion_q_len);
    }

    int ilen = serialize_interactions_q_for_player(
        scratch->interactions_q, &w->interactions, sp->ship.pos);
    send(send_user, scratch->interactions_q, ilen);
    if (emit_live_world_drift) {
        int idrift_len = serialize_interaction_drift_for_player(
            scratch->interaction_drift, &w->interactions, sp->ship.pos);
        if (idrift_len > INTERACTION_DRIFT_MSG_HEADER)
            send(send_user, scratch->interaction_drift, idrift_len);
    }

    if (!sp->world_time_sent ||
        (uint32_t)(w->tick - sp->world_time_last_sent_tick) >=
            WORLD_TIME_REPEAT_TICKS) {
        scratch->world_time[0] = NET_MSG_WORLD_TIME;
        write_f32_le(&scratch->world_time[1], w->time);
        send(send_user, scratch->world_time, (int)sizeof(scratch->world_time));
        sp->world_time_sent = true;
        sp->world_time_last_sent_tick = w->tick;
    }
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

static inline bool server_player_fracture_resolved_sent(
    const server_player_t *sp,
    uint32_t fracture_id) {
    if (!sp || fracture_id == 0u) return false;
    for (int i = 0; i < MAX_PENDING_RESOLVES; i++) {
        if (sp->fracture_resolved_sent_ids[i] == fracture_id)
            return true;
    }
    return false;
}

static inline void server_player_mark_fracture_resolved_sent(
    server_player_t *sp,
    uint32_t fracture_id) {
    if (!sp || fracture_id == 0u ||
        server_player_fracture_resolved_sent(sp, fracture_id)) {
        return;
    }
    sp->fracture_resolved_sent_ids[
        sp->fracture_resolved_sent_cursor % MAX_PENDING_RESOLVES] = fracture_id;
    sp->fracture_resolved_sent_cursor =
        (uint8_t)((sp->fracture_resolved_sent_cursor + 1u) %
                  MAX_PENDING_RESOLVES);
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
                server_player_t *sp = &w->players[p];
                if (!sp->connected) continue;
                if (sp->fracture_challenge_sent_id[i] == state->fracture_id)
                    continue;
                if (server_fracture_player_in_range_for_world(w, p, i)) {
                    send(send_user, p, buf, len);
                    sp->fracture_challenge_sent_id[i] = state->fracture_id;
                }
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
                server_player_t *sp = &w->players[p];
                if (!sp->connected) continue;
                if (server_player_fracture_resolved_sent(sp,
                                                         state->fracture_id))
                    continue;
                if (server_fracture_player_in_range_for_world(w, p, i)) {
                    send(send_user, p, buf, len);
                    server_player_mark_fracture_resolved_sent(
                        sp, state->fracture_id);
                }
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
            server_player_t *sp = &w->players[pi];
            if (!sp->connected) continue;
            if (server_player_fracture_resolved_sent(sp, pr->fracture_id))
                continue;
            send(send_user, pi, buf, len);
            server_player_mark_fracture_resolved_sent(sp, pr->fracture_id);
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

static inline bool contract_q_tail_nonzero(const uint8_t *p, int n) {
    if (!p || n <= 0) return false;
    for (int i = 0; i < n; i++) {
        if (p[i] != 0) return true;
    }
    return false;
}

static inline int serialize_contracts_q_from_full(uint8_t *buf,
                                                  const uint8_t *full,
                                                  int full_len) {
    if (!buf || !full || full_len < 2 || full[0] != NET_MSG_CONTRACTS)
        return 0;
    uint8_t count = full[1];
    int expected = 2 + (int)count * CONTRACT_RECORD_SIZE;
    if (full_len < expected || expected < 2)
        return 0;

    int off = CONTRACT_Q_HEADER_SIZE;
    buf[0] = NET_MSG_CONTRACTS_Q;
    buf[1] = count;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *src = &full[2 + (int)i * CONTRACT_RECORD_SIZE];
        uint8_t flags = 0;
        if (contract_q_tail_nonzero(&src[32], 32))
            flags |= CONTRACT_Q_FLAG_PARENT;
        if (contract_q_tail_nonzero(&src[64], 8))
            flags |= CONTRACT_Q_FLAG_ORIGIN_MASK;
        if (contract_q_tail_nonzero(&src[72], 32))
            flags |= CONTRACT_Q_FLAG_TARGET_PUB;

        int need = 1 + CONTRACT_Q_BASE_SIZE;
        if (flags & CONTRACT_Q_FLAG_PARENT) need += 32;
        if (flags & CONTRACT_Q_FLAG_ORIGIN_MASK) need += 8;
        if (flags & CONTRACT_Q_FLAG_TARGET_PUB) need += 32;
        if (off + need > CONTRACT_Q_MAX_SIZE)
            return 0;

        buf[off++] = flags;
        memcpy(&buf[off], src, CONTRACT_Q_BASE_SIZE);
        off += CONTRACT_Q_BASE_SIZE;
        if (flags & CONTRACT_Q_FLAG_PARENT) {
            memcpy(&buf[off], &src[32], 32);
            off += 32;
        }
        if (flags & CONTRACT_Q_FLAG_ORIGIN_MASK) {
            memcpy(&buf[off], &src[64], 8);
            off += 8;
        }
        if (flags & CONTRACT_Q_FLAG_TARGET_PUB) {
            memcpy(&buf[off], &src[72], 32);
            off += 32;
        }
    }
    return off;
}

static inline uint64_t net_contracts_semantic_hash(const uint8_t *data,
                                                   int len) {
    if (!data || len <= 0) return 0;
    uint64_t h = 1469598103934665603ull;
    if (len < 2 || data[0] != NET_MSG_CONTRACTS) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    int count = data[1];
    int expected = 2 + count * CONTRACT_RECORD_SIZE;
    if (len < expected) {
        for (int i = 0; i < len; i++)
            h = net_fnv1a64_update(h, data[i]);
        return h;
    }
    for (int i = 0; i < len; i++) {
        bool ignored_byte = false;
        if (i >= 2 && i < expected) {
            int record_off = (i - 2) % CONTRACT_RECORD_SIZE;
            ignored_byte = record_off >= 16 && record_off < 20; /* age */
        }
        if (!ignored_byte)
            h = net_fnv1a64_update(h, data[i]);
    }
    return h;
}

#define CONTRACTS_AGE_REFRESH_MS 30000ull /* age-only price drift refresh */

static inline bool contracts_age_refresh_due(uint64_t last_sent_ms,
                                             uint64_t now_ms) {
    return last_sent_ms == 0 ||
        now_ms - last_sent_ms >= CONTRACTS_AGE_REFRESH_MS;
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
    write_u32_le(&buf[7], ev->hail_response.decision_flags);
    write_f32_le(&buf[11], ev->hail_response.decision_signal_quality);
    buf[15] = ev->hail_response.decision_candidate_count;
    buf[16] = ev->hail_response.decision_mode;
    write_u64_le(&buf[17], ev->hail_response.decision_source_id);
    return NET_HAIL_RESPONSE_REASON_SIZE;
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

static inline int serialize_player_known_ledger(uint8_t *buf,
                                                const world_t *w,
                                                const server_player_t *sp) {
    int count = 0;
    buf[0] = NET_MSG_PLAYER_KNOWN_LEDGER;
    if (!w || !sp) {
        buf[1] = 0;
        return PLAYER_KNOWN_LEDGER_HEADER;
    }
    for (int s = 0; s < MAX_STATIONS &&
                    count < PLAYER_KNOWN_LEDGER_MAX_RECORDS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        float balance = server_player_can_use_pubkey_persistence(sp)
            ? ledger_balance_by_pubkey(st, sp->pubkey)
            : ledger_balance(st, sp->session_token);
        if (fabsf(balance) < 0.001f) continue;
        uint8_t *p = &buf[PLAYER_KNOWN_LEDGER_HEADER +
                          count * PLAYER_KNOWN_LEDGER_RECORD_SIZE];
        p[0] = (uint8_t)s;
        write_f32_le(&p[1], balance);
        count++;
    }
    buf[1] = (uint8_t)count;
    return PLAYER_KNOWN_LEDGER_HEADER +
           count * PLAYER_KNOWN_LEDGER_RECORD_SIZE;
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
    uint8_t known_ledger[
        PLAYER_KNOWN_LEDGER_HEADER +
        PLAYER_KNOWN_LEDGER_MAX_RECORDS * PLAYER_KNOWN_LEDGER_RECORD_SIZE
    ];
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

    if (server_player_is_gameplay_ready(sp) && !sp->input_ack_state_valid) {
        (void)server_emit_authoritative_player_state_snapshot(
            sp, (uint8_t)player_slot, w->tick, send, send_user);
    }

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

    int ledger_len = serialize_player_known_ledger(
        scratch->known_ledger, w, sp);
    send(send_user, scratch->known_ledger, ledger_len);

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
 * bytes 18..21 carry client_sent_ms, a client monotonic timestamp echoed by
 * INPUT_APPLIED so active ack receipts can also refresh transport RTT.
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

static inline uint32_t input_client_sent_ms(const uint8_t *data, int len) {
    if (!data || len < NET_INPUT_MSG_SIZE) return 0;
    return read_u32_le(&data[18]);
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
/* Event transport side-effect routing                                 */
/* ------------------------------------------------------------------ */

enum {
    SERVER_SIM_EVENT_EFFECT_OUTPOST_PLACED    = 1u << 0,
    SERVER_SIM_EVENT_EFFECT_PLAYER_STATE      = 1u << 1,
    SERVER_SIM_EVENT_EFFECT_DEATH             = 1u << 2,
    SERVER_SIM_EVENT_EFFECT_CONTRACT_COMPLETE = 1u << 3,
    SERVER_SIM_EVENT_EFFECT_HAIL_RESPONSE     = 1u << 4,
    SERVER_SIM_EVENT_EFFECT_STRUCTURE_DIRTY   = 1u << 5,
};

typedef void (*server_sim_event_hook_fn)(void *user,
                                         const sim_event_t *ev);

typedef struct {
    server_sim_event_hook_fn outpost_placed;
    server_sim_event_hook_fn player_state_change;
    server_sim_event_hook_fn death;
    server_sim_event_hook_fn contract_complete;
    server_sim_event_hook_fn hail_response;
    server_sim_event_hook_fn structure_dirty;
} server_sim_event_hooks_t;

static inline uint32_t server_sim_event_effects(const sim_event_t *ev) {
    if (!ev) return 0;
    uint32_t effects = 0;
    if (ev->type == SIM_EVENT_OUTPOST_PLACED)
        effects |= SERVER_SIM_EVENT_EFFECT_OUTPOST_PLACED;
    if (ev->type == SIM_EVENT_SELL ||
        ev->type == SIM_EVENT_BUY ||
        ev->type == SIM_EVENT_REPAIR ||
        ev->type == SIM_EVENT_UPGRADE ||
        ev->type == SIM_EVENT_DOCK ||
        ev->type == SIM_EVENT_LAUNCH) {
        effects |= SERVER_SIM_EVENT_EFFECT_PLAYER_STATE;
    }
    if (ev->type == SIM_EVENT_DEATH)
        effects |= SERVER_SIM_EVENT_EFFECT_DEATH;
    if (ev->type == SIM_EVENT_CONTRACT_COMPLETE)
        effects |= SERVER_SIM_EVENT_EFFECT_CONTRACT_COMPLETE;
    if (ev->type == SIM_EVENT_HAIL_RESPONSE)
        effects |= SERVER_SIM_EVENT_EFFECT_HAIL_RESPONSE;
    if (ev->type == SIM_EVENT_OUTPOST_PLACED ||
        ev->type == SIM_EVENT_OUTPOST_ACTIVATED ||
        ev->type == SIM_EVENT_MODULE_ACTIVATED ||
        ev->type == SIM_EVENT_SCAFFOLD_READY) {
        effects |= SERVER_SIM_EVENT_EFFECT_STRUCTURE_DIRTY;
    }
    return effects;
}

static inline void server_process_sim_event_transport(
    const sim_event_t *ev,
    const server_sim_event_hooks_t *hooks,
    void *user) {
    if (!ev || !hooks) return;
    uint32_t effects = server_sim_event_effects(ev);
    if ((effects & SERVER_SIM_EVENT_EFFECT_OUTPOST_PLACED) &&
        hooks->outpost_placed) {
        hooks->outpost_placed(user, ev);
    }
    if ((effects & SERVER_SIM_EVENT_EFFECT_PLAYER_STATE) &&
        hooks->player_state_change) {
        hooks->player_state_change(user, ev);
    }
    if ((effects & SERVER_SIM_EVENT_EFFECT_DEATH) && hooks->death)
        hooks->death(user, ev);
    if ((effects & SERVER_SIM_EVENT_EFFECT_CONTRACT_COMPLETE) &&
        hooks->contract_complete) {
        hooks->contract_complete(user, ev);
    }
    if ((effects & SERVER_SIM_EVENT_EFFECT_HAIL_RESPONSE) &&
        hooks->hail_response) {
        hooks->hail_response(user, ev);
    }
    if ((effects & SERVER_SIM_EVENT_EFFECT_STRUCTURE_DIRTY) &&
        hooks->structure_dirty) {
        hooks->structure_dirty(user, ev);
    }
}

/* ------------------------------------------------------------------ */
/* Event broadcast serialization                                       */
/* ------------------------------------------------------------------ */

static inline bool sim_event_is_local_only_for_wire(const sim_event_t *ev) {
    if (!ev) return false;
    switch (ev->type) {
    case SIM_EVENT_PICKUP:
    case SIM_EVENT_MINING_TICK:
    case SIM_EVENT_DOCK:
    case SIM_EVENT_LAUNCH:
    case SIM_EVENT_SELL:
    case SIM_EVENT_BUY:
    case SIM_EVENT_REPAIR:
    case SIM_EVENT_UPGRADE:
    case SIM_EVENT_DAMAGE:
    case SIM_EVENT_OUTPOST_PLACED:
    case SIM_EVENT_SIGNAL_LOST:
    case SIM_EVENT_CONTRACT_COMPLETE:
    case SIM_EVENT_ORDER_REJECTED:
        return true;
    default:
        return false;
    }
}

static inline bool sim_event_visible_to_recipient(const sim_event_t *ev,
                                                  int recipient_slot) {
    if (!ev || ev->type == SIM_EVENT_HAIL_RESPONSE) return false;
    if (!sim_event_is_local_only_for_wire(ev)) return true;
    return recipient_slot >= 0 && ev->player_id == recipient_slot;
}

static inline void serialize_event_record(uint8_t *p,
                                          const sim_event_t *ev) {
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
}

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
        serialize_event_record(p, ev);
        count++;
    }
    buf[0] = NET_MSG_EVENTS;
    buf[1] = (uint8_t)count;
    return 2 + count * NET_EVENT_RECORD_SIZE;
}

static inline int serialize_events_for_recipient(uint8_t *buf,
                                                 const sim_events_t *events,
                                                 int recipient_slot) {
    int count = 0;
    if (!buf || !events) return 0;
    for (int i = 0; i < events->count; i++) {
        const sim_event_t *ev = &events->events[i];
        if (!sim_event_visible_to_recipient(ev, recipient_slot))
            continue;
        uint8_t *p = &buf[2 + count * NET_EVENT_RECORD_SIZE];
        serialize_event_record(p, ev);
        count++;
    }
    buf[0] = NET_MSG_EVENTS;
    buf[1] = (uint8_t)count;
    return 2 + count * NET_EVENT_RECORD_SIZE;
}

#endif /* NET_PROTOCOL_H */
