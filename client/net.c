/*
 * net.c — Multiplayer networking implementation for Signal Space Miner.
 *
 * WASM build: Uses emscripten WebSocket API.
 * Native build: Uses mongoose WebSocket client.
 */
#include "net.h"
#include "mining_client.h"
#include "mining.h"  /* mining_alphanumeric_callsign — pubkey-derived */
#include "manifest.h"
#include "pubkey_proof.h"
#include "signal_crypto.h"
#include "net_clock.h"
#include "wire_codec.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>  /* emscripten_date_now() */
#endif
#endif
#ifdef _WIN32
/* GetSystemTimePreciseAsFileTime — sub-microsecond wall clock on Win8+. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* ---------- Shared state ------------------------------------------------- */

#define NET_LATENCY_PING_TRACK_CAP 16

typedef struct {
    uint32_t seq;
    uint32_t sent_ms;
} net_latency_ping_track_t;

static struct {
    bool connected;
    uint8_t local_id;
    NetPlayerState players[NET_MAX_PLAYERS];
    NetCallbacks callbacks;
    char server_hash[12];
    NetProtocolInfo protocol_info;
    bool protocol_info_ready;
    uint8_t session_token[8];
    bool session_token_ready;
    uint8_t pubkey_challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    bool pubkey_challenge_ready;
    pubkey_proof_client_state_t pubkey_proof;
    char callsign[8];
    bool callsign_ready;
    char server_url[256];
    /* Layer A.2 of #479 — Ed25519 pubkey advertised in REGISTER_PUBKEY
     * on every connect/reconnect. Owned by the client at large
     * (game_t::identity); set via net_set_identity_pubkey before
     * net_init runs the WebSocket handshake. */
    uint8_t identity_pubkey[32];
    bool identity_pubkey_ready;
    /* Layer A.3 of #479 — Ed25519 secret for signing state-changing
     * actions. Owned by the client (game_t::identity); installed via
     * net_set_identity_secret. Never sent on the wire. */
    uint8_t identity_secret[64];
    bool identity_secret_ready;
    /* Monotonic per-process nonce high-water mark for signed actions.
     * Seeded on first use to current wall-clock microseconds so a
     * client-side restart still produces nonces that strictly exceed
     * the server's persisted last_signed_nonce in practice (server
     * also rejects strict-replay, so monotonicity is what matters). */
    uint64_t signed_action_nonce;
    uint32_t latency_ping_seq;
    net_latency_ping_track_t latency_pings[NET_LATENCY_PING_TRACK_CAP];
    bool player_motion_q_valid[NET_MAX_PLAYERS];
    int16_t player_motion_qx[NET_MAX_PLAYERS];
    int16_t player_motion_qy[NET_MAX_PLAYERS];
    bool asteroid_pos_q_valid[MAX_ASTEROIDS];
    int16_t asteroid_pos_qx[MAX_ASTEROIDS];
    int16_t asteroid_pos_qy[MAX_ASTEROIDS];
} net_state;

static bool net_loopback_active = false;
static net_loopback_send_fn net_loopback_send = NULL;
static void *net_loopback_user = NULL;
static uint32_t net_auth_transport_closes = 0;

static void net_player_state_clear_ack_transport(NetPlayerState *ps) {
    if (!ps) return;
    ps->ack_client_sent_ms = 0;
    ps->ack_server_recv_ms = 0;
    ps->ack_server_send_ms = 0;
}

/* Short aliases keep record layouts readable; implementations live in the
 * shared codec used by both endpoints. */
#define write_f32_le wire_write_f32_le
#define write_u32_le wire_write_u32_le
#define write_u16_le wire_write_u16_le
#define read_u32_le  wire_read_u32_le
#define read_u16_le  wire_read_u16_le
#define read_u64_le  wire_read_u64_le
#define read_f32_le  wire_read_f32_le

static int16_t net_player_motion_q_encode(float value) {
    if (!isfinite(value) || PLAYER_MOTION_Q_POS_SCALE <= 0.0f)
        return 0;
    float q = value / PLAYER_MOTION_Q_POS_SCALE;
    if (q > 32767.0f) return 32767;
    if (q < -32768.0f) return -32768;
    return (int16_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
}

static void net_player_motion_q_note(uint8_t id,
                                     int16_t qx,
                                     int16_t qy) {
    if (id >= NET_MAX_PLAYERS) return;
    net_state.player_motion_q_valid[id] = true;
    net_state.player_motion_qx[id] = qx;
    net_state.player_motion_qy[id] = qy;
}

static void net_player_motion_q_note_float(uint8_t id,
                                           float x,
                                           float y) {
    net_player_motion_q_note(id,
                             net_player_motion_q_encode(x),
                             net_player_motion_q_encode(y));
}

static int16_t net_asteroid_pos_q_encode(float value) {
    if (!isfinite(value) || ASTEROID_MOTION_Q_POS_SCALE <= 0.0f)
        return 0;
    float q = value / ASTEROID_MOTION_Q_POS_SCALE;
    if (q > 32767.0f) return 32767;
    if (q < -32768.0f) return -32768;
    return (int16_t)((q >= 0.0f) ? (q + 0.5f) : (q - 0.5f));
}

static void net_asteroid_pos_q_note(uint16_t index,
                                    int16_t qx,
                                    int16_t qy) {
    if (index >= MAX_ASTEROIDS) return;
    net_state.asteroid_pos_q_valid[index] = true;
    net_state.asteroid_pos_qx[index] = qx;
    net_state.asteroid_pos_qy[index] = qy;
}

static void net_asteroid_pos_q_note_float(uint16_t index,
                                          float x,
                                          float y) {
    net_asteroid_pos_q_note(index,
                            net_asteroid_pos_q_encode(x),
                            net_asteroid_pos_q_encode(y));
}

static void net_asteroid_pos_q_clear(uint16_t index) {
    if (index >= MAX_ASTEROIDS) return;
    net_state.asteroid_pos_q_valid[index] = false;
    net_state.asteroid_pos_qx[index] = 0;
    net_state.asteroid_pos_qy[index] = 0;
}

static float net_asteroid_identity_q_decode_value(uint16_t q) {
    return (float)q * ASTEROID_IDENTITY_Q_VALUE_SCALE;
}

static void net_asteroid_identity_q_unpack_detail(uint8_t detail,
                                                  uint8_t *grade,
                                                  uint8_t *crystal_stage,
                                                  uint8_t *phase) {
    if (grade) {
        uint8_t g = detail & 0x7u;
        *grade = (g < MINING_GRADE_COUNT) ? g : (uint8_t)MINING_GRADE_COMMON;
    }
    if (crystal_stage) *crystal_stage = (uint8_t)((detail >> 3) & 0x3u);
    if (phase) *phase = (uint8_t)((detail >> 5) & 0x3u);
}

static bool net_read_span(const uint8_t *data,
                          int len,
                          int *off,
                          void *dst,
                          int n) {
    if (!data || !off || n < 0 || *off < 0 || *off + n > len)
        return false;
    if (dst && n > 0) memcpy(dst, &data[*off], (size_t)n);
    *off += n;
    return true;
}

static bool net_read_lp_string(const uint8_t *data,
                               int len,
                               int *off,
                               char *dst,
                               size_t dst_cap) {
    if (!data || !off || !dst || dst_cap == 0 ||
        *off < 0 || *off >= len) {
        return false;
    }
    uint8_t n = data[(*off)++];
    if (*off + (int)n > len) return false;
    size_t copy_n = n;
    if (copy_n >= dst_cap) copy_n = dst_cap - 1;
    if (copy_n > 0) memcpy(dst, &data[*off], copy_n);
    dst[copy_n] = '\0';
    *off += n;
    return true;
}

static void net_decode_contract_base(contract_t *ct, const uint8_t *p) {
    if (!ct || !p) return;
    ct->active = true;
    ct->action =
        (p[0] <= CONTRACT_DELIVERY) ? (contract_action_t)p[0] :
        CONTRACT_TRACTOR;
    ct->station_index = (p[1] < MAX_STATIONS) ? p[1] : 0;
    ct->commodity =
        (p[2] < COMMODITY_COUNT) ? (commodity_t)p[2] :
        COMMODITY_FERRITE_ORE;
    ct->required_grade =
        (p[3] < MINING_GRADE_COUNT) ? p[3] :
        (uint8_t)MINING_GRADE_COMMON;
    ct->proof_flags = p[4] & 0x1fu;
    ct->required_prefix_class =
        (p[5] < INGOT_PREFIX_COUNT) ? p[5] :
        (uint8_t)INGOT_PREFIX_ANONYMOUS;
    ct->required_recipe_id = read_u16_le(&p[6]);
    if (ct->required_recipe_id >= RECIPE_COUNT)
        ct->proof_flags &=
            (uint8_t)(UINT8_MAX ^ CONTRACT_PROOF_REQUIRE_RECIPE);
    ct->quantity_needed = read_f32_le(&p[8]);
    ct->base_price = read_f32_le(&p[12]);
    ct->age = read_f32_le(&p[16]);
    ct->target_pos.x = read_f32_le(&p[20]);
    ct->target_pos.y = read_f32_le(&p[24]);
    ct->target_index = (int)(int32_t)read_u32_le(&p[28]);
    ct->claimed_by = -1;
}

static void net_finish_contract_decode(contract_t *ct) {
    if (!ct) return;
    if (ct->forbidden_origin_mask == 0)
        ct->proof_flags &=
            (uint8_t)(UINT8_MAX ^ CONTRACT_PROOF_FORBID_ORIGIN);
}

static bool decode_station_identity_q(NetStationIdentity *si,
                                      const uint8_t *data,
                                      int len) {
    if (!si || !data || len < STATION_IDENTITY_Q_HEADER_SIZE ||
        data[0] != NET_MSG_STATION_IDENTITY_Q) {
        return false;
    }
    memset(si, 0, sizeof(*si));
    si->index = data[1];
    si->flags = data[2];
    int off = STATION_IDENTITY_Q_HEADER_SIZE;
    if (off + 24 > len) return false;
    si->services = read_u32_le(&data[off]); off += 4;
    si->pos_x = read_f32_le(&data[off]); off += 4;
    si->pos_y = read_f32_le(&data[off]); off += 4;
    si->radius = read_f32_le(&data[off]); off += 4;
    si->dock_radius = read_f32_le(&data[off]); off += 4;
    si->signal_range = read_f32_le(&data[off]); off += 4;
    if (!net_read_lp_string(data, len, &off, si->name, sizeof(si->name)))
        return false;

    for (int c = 0; c < COMMODITY_COUNT; c++) {
        if (off + 4 > len) return false;
        si->base_price[c] = read_f32_le(&data[off]);
        off += 4;
    }
    if (off + 4 > len) return false;
    si->scaffold_progress = read_f32_le(&data[off]);
    off += 4;

    if (off >= len) return false;
    si->module_count = data[off++];
    if (si->module_count > MAX_MODULES_PER_STATION) return false;
    for (int m = 0; m < si->module_count; m++) {
        if (off + STATION_MODULE_RECORD_SIZE > len) return false;
        si->modules[m].type = (module_type_t)data[off];
        si->modules[m].scaffold = data[off + 1] != 0;
        si->modules[m].ring = data[off + 2];
        si->modules[m].slot = data[off + 3];
        si->modules[m].build_progress = read_f32_le(&data[off + 4]);
        si->modules[m].commodity = data[off + 8];
        if (si->modules[m].commodity > COMMODITY_COUNT)
            si->modules[m].commodity = (uint8_t)COMMODITY_COUNT;
        off += STATION_MODULE_RECORD_SIZE;
    }

    if (off >= len) return false;
    si->arm_count = data[off++];
    if (si->arm_count > MAX_ARMS) return false;
    for (int a = 0; a < si->arm_count; a++) {
        if (off + 16 > len) return false;
        si->arm_speed[a] = read_f32_le(&data[off]); off += 4;
        si->ring_offset[a] = read_f32_le(&data[off]); off += 4;
        si->arm_rotation[a] = read_f32_le(&data[off]); off += 4;
        si->arm_omega[a] = read_f32_le(&data[off]); off += 4;
    }

    if (off >= len) return false;
    si->plan_count = data[off++];
    if (si->plan_count > STATION_PLAN_RECORD_COUNT) return false;
    for (int p = 0; p < si->plan_count; p++) {
        if (off + STATION_PLAN_RECORD_SIZE > len) return false;
        si->plans[p].type = (module_type_t)data[off + 0];
        si->plans[p].ring = data[off + 1];
        si->plans[p].slot = data[off + 2];
        si->plans[p].owner = (int8_t)data[off + 3];
        off += STATION_PLAN_RECORD_SIZE;
    }

    if (off >= len) return false;
    si->pending_scaffold_count = data[off++];
    if (si->pending_scaffold_count > STATION_PENDING_SCAFFOLD_RECORD_COUNT)
        return false;
    for (int p = 0; p < si->pending_scaffold_count; p++) {
        if (off + STATION_PENDING_SCAFFOLD_RECORD_SIZE > len) return false;
        si->pending_scaffolds[p].type = (module_type_t)data[off + 0];
        si->pending_scaffolds[p].owner =
            (data[off + 1] == 0xFF) ? -1 : (int8_t)data[off + 1];
        off += STATION_PENDING_SCAFFOLD_RECORD_SIZE;
    }

    if (off >= len) return false;
    si->pending_ship_build_count = data[off++];
    if (si->pending_ship_build_count > STATION_PENDING_SHIP_RECORD_COUNT)
        return false;
    for (int p = 0; p < si->pending_ship_build_count; p++) {
        if (off + STATION_PENDING_SHIP_RECORD_SIZE > len) return false;
        si->pending_ship_builds[p].hull_class = (hull_class_t)data[off + 0];
        /* Byte 1 is the retired runtime-owner field. It remains reserved so
         * old and new peers agree on the fixed six-byte record size. */
        si->pending_ship_builds[p].build_progress = read_f32_le(&data[off + 2]);
        off += STATION_PENDING_SHIP_RECORD_SIZE;
    }

    if (!net_read_lp_string(data, len, &off, si->hail_message,
                            sizeof(si->hail_message))) {
        return false;
    }
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        if (!net_read_lp_string(data, len, &off, si->miner_chatter[i],
                                sizeof(si->miner_chatter[i]))) {
            return false;
        }
    }
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        if (!net_read_lp_string(data, len, &off, si->hauler_chatter[i],
                                sizeof(si->hauler_chatter[i]))) {
            return false;
        }
    }
    if (!net_read_lp_string(data, len, &off, si->rati_hail_message,
                            sizeof(si->rati_hail_message)) ||
        !net_read_lp_string(data, len, &off, si->currency_name,
                            sizeof(si->currency_name))) {
        return false;
    }
    if (!net_read_span(data, len, &off, si->station_pubkey,
                       STATION_IDENTITY_PUBKEY_LEN) ||
        !net_read_span(data, len, &off, si->stored_hull_count,
                       HULL_CLASS_COUNT)) {
        return false;
    }
    if (off + STATION_IDENTITY_FACTION_SIZE > len) return false;
    si->faction_id = data[off++];
    si->faction_allegiance = data[off++];
    si->faction_ideology = data[off++];
    for (int f = 0; f < STATION_FACTION_COUNT; f++)
        si->faction_relations[f] = (int8_t)data[off++];

    if (off >= len) return false;
    si->policy_card_count = data[off++];
    if (si->policy_card_count > STATION_IDENTITY_POLICY_CARD_COUNT)
        return false;
    if (!net_read_span(data, len, &off, si->policy_card_ids,
                       si->policy_card_count)) {
        return false;
    }
    return true;
}

/* Forward declarations — implemented per platform below. */
static bool ws_send_binary(const uint8_t* data, int len);
static void ws_close_authentication_failure(void);
static bool ensure_session_token(void);
static void ensure_callsign(void);
static bool send_register_pubkey(void);
static void send_pubkey_proof(void);
static bool send_session_token(void);
static void handle_message(const uint8_t* data, int len);

static void preserve_identity(uint8_t pubkey[32], uint8_t secret[64],
                              bool *pub_ready, bool *secret_ready) {
    if (pubkey) memcpy(pubkey, net_state.identity_pubkey, 32);
    if (secret) memcpy(secret, net_state.identity_secret, 64);
    if (pub_ready) *pub_ready = net_state.identity_pubkey_ready;
    if (secret_ready) *secret_ready = net_state.identity_secret_ready;
}

static void restore_identity(const uint8_t pubkey[32], const uint8_t secret[64],
                             bool pub_ready, bool secret_ready) {
    if (pubkey) memcpy(net_state.identity_pubkey, pubkey, 32);
    if (secret) memcpy(net_state.identity_secret, secret, 64);
    net_state.identity_pubkey_ready = pub_ready;
    net_state.identity_secret_ready = secret_ready;
}

static void clear_pubkey_challenge_state(bool clear_proof_latch) {
    memset(net_state.pubkey_challenge, 0,
           sizeof(net_state.pubkey_challenge));
    net_state.pubkey_challenge_ready = false;
    if (clear_proof_latch)
        pubkey_proof_client_state_reset(&net_state.pubkey_proof);
}

static bool transport_connected(const char *label) {
    net_state.connected = true;
    printf("[net] connected to %s\n", label ? label : "transport");
    clear_pubkey_challenge_state(true);
    if (!net_state.identity_pubkey_ready ||
        !net_state.identity_secret_ready ||
        !ensure_session_token()) {
        memset(net_state.session_token, 0,
               sizeof(net_state.session_token));
        net_state.session_token_ready = false;
        net_state.connected = false;
        fprintf(stderr,
                "[net] secure identity entropy unavailable; "
                "refusing authentication\n");
        return false;
    }
    ensure_callsign();
    /* Layer A.2 of #479 — pubkey registration MUST precede the session
     * handshake so the server can fold the pubkey into reconnect
     * resolution. The proof itself waits for the one-time server challenge. */
    if (!send_register_pubkey() || !send_session_token()) {
        net_state.connected = false;
        clear_pubkey_challenge_state(true);
        fprintf(stderr,
                "[net] authentication bootstrap send rejected\n");
        return false;
    }
    mining_client_set_session_token(net_state.session_token);
    return true;
}

static void transport_message(const uint8_t *data, int len) {
    handle_message(data, len);
}

static void transport_disconnected(const char *label) {
    printf("[net] disconnected from %s\n", label ? label : "transport");
    net_state.connected = false;
    clear_pubkey_challenge_state(true);
}

void net_set_loopback_send(net_loopback_send_fn send_fn, void *user) {
    net_loopback_send = send_fn;
    net_loopback_user = user;
}

bool net_init_loopback(const NetCallbacks* callbacks, uint8_t local_id) {
    uint8_t saved_pubkey[32];
    uint8_t saved_secret[64];
    bool saved_pub_ready = false;
    bool saved_secret_ready = false;
    preserve_identity(saved_pubkey, saved_secret,
                      &saved_pub_ready, &saved_secret_ready);

    memset(&net_state, 0, sizeof(net_state));
    restore_identity(saved_pubkey, saved_secret,
                     saved_pub_ready, saved_secret_ready);
    if (callbacks) net_state.callbacks = *callbacks;
    net_state.local_id = local_id;
    net_state.connected = true;
    net_loopback_active = true;
    net_state.server_hash[0] = '\0';
    if (local_id < NET_MAX_PLAYERS) {
        net_state.players[local_id].player_id = local_id;
        net_state.players[local_id].active = true;
    }

    if (!transport_connected("local server loopback")) {
        net_loopback_active = false;
        return false;
    }
    return true;
}

bool net_is_loopback(void) {
    return net_loopback_active;
}

void net_loopback_receive(const uint8_t *data, int len) {
    handle_message(data, len);
}

void net_send_present_receipt_chain(const uint8_t cargo_pub[32],
                                    const cargo_receipt_chain_t *chain) {
    if (!cargo_pub || !chain || chain->len == 0 ||
        chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
        return;
    }
    uint8_t buf[35 + CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE];
    buf[0] = NET_MSG_PRESENT_RECEIPT_CHAIN;
    memcpy(&buf[1], cargo_pub, 32);
    buf[33] = chain->len;
    buf[34] = 0;
    for (uint8_t i = 0; i < chain->len; i++)
        cargo_receipt_pack(&chain->links[i], &buf[35 + i * CARGO_RECEIPT_SIZE]);
    (void)ws_send_binary(
        buf, 35 + (int)chain->len * CARGO_RECEIPT_SIZE);
}

void net_send_handoff_request(uint8_t source_station, uint8_t dest_station,
                              uint32_t ttl_ticks) {
    if (!net_state.connected) return;
    uint8_t buf[NET_HANDOFF_REQUEST_SIZE];
    buf[0] = NET_MSG_HANDOFF_REQUEST;
    buf[1] = source_station;
    buf[2] = dest_station;
    write_u32_le(&buf[3], ttl_ticks);
    (void)ws_send_binary(buf, NET_HANDOFF_REQUEST_SIZE);
}

void net_send_handoff_present(const handoff_ticket_t *ticket,
                              const ship_t *ship) {
    if (!net_state.connected || !ticket || !ship) return;
    size_t snapshot_len = handoff_ship_snapshot_size(ship);
    if (snapshot_len == 0 || snapshot_len > HANDOFF_SHIP_SNAPSHOT_MAX_SIZE)
        return;
    size_t len = 1u + HANDOFF_TICKET_SIZE + 4u + snapshot_len;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return;
    buf[0] = NET_MSG_HANDOFF_PRESENT;
    handoff_ticket_pack(ticket, &buf[1]);
    write_u32_le(&buf[1 + HANDOFF_TICKET_SIZE], (uint32_t)snapshot_len);
    if (!handoff_ship_snapshot_pack(ship,
                                    &buf[1 + HANDOFF_TICKET_SIZE + 4],
                                    snapshot_len, NULL)) {
        free(buf);
        return;
    }
    (void)ws_send_binary(buf, (int)len);
    free(buf);
}

void net_send_latency_ping(void) {
    if (!net_state.connected) return;
    uint8_t buf[NET_LATENCY_PING_SIZE];
    uint32_t seq = ++net_state.latency_ping_seq;
    if (seq == 0) seq = ++net_state.latency_ping_seq;
    uint32_t sent_ms = net_now_ms32();
    net_latency_ping_track_t *slot =
        &net_state.latency_pings[seq % NET_LATENCY_PING_TRACK_CAP];
    slot->seq = seq;
    slot->sent_ms = sent_ms;
    buf[0] = NET_MSG_LATENCY_PING;
    write_u32_le(&buf[1], seq);
    write_u32_le(&buf[5], sent_ms);
    if (!ws_send_binary(buf, NET_LATENCY_PING_SIZE)) {
        slot->seq = 0;
        slot->sent_ms = 0;
    }
}

static uint16_t metric_ms_u16(float ms) {
    if (!(ms > 0.0f)) return 0;
    if (ms >= 65535.0f) return 65535u;
    return (uint16_t)(ms + 0.5f);
}

void net_send_client_metrics(uint32_t seq,
                             float ping_rtt_ms,
                             float ack_ms,
                             float ack_gap_ms,
                             float server_turnaround_ms,
                             float player_interval_ms,
                             uint16_t unacked_inputs,
                             uint16_t replay_depth,
                             uint8_t action_queue_depth,
                             uint8_t recovery_flags) {
    if (!net_state.connected) return;
    uint8_t buf[NET_CLIENT_METRICS_SIZE];
    buf[0] = NET_MSG_CLIENT_METRICS;
    write_u32_le(&buf[1], seq);
    write_u16_le(&buf[5], metric_ms_u16(ping_rtt_ms));
    write_u16_le(&buf[7], metric_ms_u16(ack_ms));
    write_u16_le(&buf[9], metric_ms_u16(ack_gap_ms));
    write_u16_le(&buf[11], metric_ms_u16(server_turnaround_ms));
    write_u16_le(&buf[13], metric_ms_u16(player_interval_ms));
    write_u16_le(&buf[15], unacked_inputs);
    write_u16_le(&buf[17], replay_depth);
    buf[19] = action_queue_depth;
    buf[20] = recovery_flags;
    (void)ws_send_binary(buf, NET_CLIENT_METRICS_SIZE);
}

#ifdef __EMSCRIPTEN__
static void transport_error(const char *label) {
    printf("[net] %s error\n", label ? label : "transport");
    net_state.connected = false;
    clear_pubkey_challenge_state(true);
}
#endif

static void send_fracture_claim(uint32_t fracture_id, uint32_t burst_nonce,
                                mining_grade_t claimed_grade) {
    uint8_t buf[FRACTURE_CLAIM_SIZE];
    uint8_t payload[9];
    write_u32_le(&payload[0], fracture_id);
    write_u32_le(&payload[4], burst_nonce);
    payload[8] = (uint8_t)claimed_grade;
    if (net_has_identity_pubkey()) {
        /* An admission failure on the signed path must not downgrade an
         * identity-backed claim to the unsigned legacy packet. */
        (void)net_send_signed_action(
            SIGNED_ACTION_FRACTURE_CLAIM, payload, sizeof(payload));
        return;
    }
    buf[0] = NET_MSG_FRACTURE_CLAIM;
    buf[1] = (uint8_t)(fracture_id);
    buf[2] = (uint8_t)(fracture_id >> 8);
    buf[3] = (uint8_t)(fracture_id >> 16);
    buf[4] = (uint8_t)(fracture_id >> 24);
    buf[5] = (uint8_t)(burst_nonce);
    buf[6] = (uint8_t)(burst_nonce >> 8);
    buf[7] = (uint8_t)(burst_nonce >> 16);
    buf[8] = (uint8_t)(burst_nonce >> 24);
    buf[9] = (uint8_t)claimed_grade;
    (void)ws_send_binary(buf, sizeof(buf));
}

#ifdef __EMSCRIPTEN__
EM_JS(int, signal_session_token_load, (char *out, int cap), {
    try {
        var s = globalThis.localStorage.getItem('signal_session_token');
        if (!s) return 0;
        if (s.length !== 16 || s.length + 1 > cap) return -1;
        for (var i = 0; i < s.length; i++) {
            var code = s.charCodeAt(i);
            var isDigit = code >= 48 && code <= 57;
            var isUpper = code >= 65 && code <= 70;
            var isLower = code >= 97 && code <= 102;
            if (!isDigit && !isUpper && !isLower) return -1;
        }
        stringToUTF8(s, out, cap);
        return 1;
    } catch (e) {
        return -2;
    }
})

EM_JS(int, signal_session_token_save, (const char *value), {
    try {
        globalThis.localStorage.setItem(
            'signal_session_token', UTF8ToString(value));
        return 1;
    } catch (e) {
        return 0;
    }
})

static bool hex_nibble(char c, uint8_t *out) {
    if (!out) return false;
    if (c >= '0' && c <= '9') {
        *out = (uint8_t)(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (uint8_t)(c - 'a' + 10);
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (uint8_t)(c - 'A' + 10);
        return true;
    }
    *out = 0;
    return false;
}
#endif

static bool session_token_nonzero(const uint8_t token[8]) {
    if (!token) return false;
    uint8_t any = 0;
    for (int i = 0; i < 8; i++) any |= token[i];
    return any != 0;
}

static bool ensure_session_token(void) {
    if (net_state.session_token_ready)
        return session_token_nonzero(net_state.session_token);
    memset(net_state.session_token, 0, sizeof(net_state.session_token));
#ifdef __EMSCRIPTEN__
    char hex[17] = {0};
    int loaded = signal_session_token_load(hex, sizeof(hex));
    if (loaded == 1) {
        for (int i = 0; i < 8; i++) {
            uint8_t high = 0;
            uint8_t low = 0;
            if (!hex_nibble(hex[i * 2], &high) ||
                !hex_nibble(hex[i * 2 + 1], &low)) {
                memset(net_state.session_token, 0,
                       sizeof(net_state.session_token));
                return false;
            }
            net_state.session_token[i] = (uint8_t)((high << 4) | low);
        }
        if (!session_token_nonzero(net_state.session_token)) {
            memset(net_state.session_token, 0,
                   sizeof(net_state.session_token));
            /* Treat a persisted all-zero token as malformed. Generate a
             * replacement below, but do not overwrite storage unless the
             * CSPRNG succeeds. */
            loaded = -1;
        } else {
            net_state.session_token_ready = true;
            return true;
        }
    }
    /* A malformed entry may be replaced, but only after fresh entropy
     * succeeds. Storage access failure itself remains a hard failure. */
    if (loaded == -2) return false;
#endif

    if (!signal_crypto_random_bytes(net_state.session_token,
                                    sizeof(net_state.session_token)) ||
        !session_token_nonzero(net_state.session_token)) {
        memset(net_state.session_token, 0,
               sizeof(net_state.session_token));
        net_state.session_token_ready = false;
        return false;
    }

#ifdef __EMSCRIPTEN__
    static const char digits[] = "0123456789abcdef";
    char encoded[17];
    for (int i = 0; i < 8; i++) {
        encoded[i * 2] = digits[net_state.session_token[i] >> 4];
        encoded[i * 2 + 1] = digits[net_state.session_token[i] & 0x0Fu];
    }
    encoded[16] = '\0';
    if (!signal_session_token_save(encoded)) {
        memset(net_state.session_token, 0,
               sizeof(net_state.session_token));
        net_state.session_token_ready = false;
        return false;
    }
#endif
    net_state.session_token_ready = true;
    return true;
}

static void ensure_callsign(void) {
    if (net_state.callsign_ready) return;
    /* Callsign is now derived from the player's Ed25519 pubkey via
     * mining_alphanumeric_callsign(). Same pubkey → same callsign on
     * every machine forever, no localStorage cache needed. The legacy
     * random/localStorage path is gone. */
    if (!net_state.identity_pubkey_ready) {
        /* Pubkey not yet provided — defer; main.c installs it before
         * net_init via net_set_identity_pubkey(). Leave callsign as-is
         * (zeroed); ensure_callsign() will be called again. */
        return;
    }
    mining_alphanumeric_callsign(net_state.identity_pubkey, net_state.callsign);
    net_state.callsign_ready = true;
    printf("[net] callsign: %s\n", net_state.callsign);
}

/* Layer A.2 of #479 — send the persistent Ed25519 pubkey to the server
 * immediately on connect, BEFORE the SESSION handshake, so the server
 * can stage the pubkey assertion. Registry/persistence binding waits for
 * send_pubkey_proof() after the SESSION token is known. */
static bool send_register_pubkey(void) {
    if (!net_state.identity_pubkey_ready) return false;
    uint8_t buf[REGISTER_PUBKEY_MSG_SIZE];
    buf[0] = NET_MSG_REGISTER_PUBKEY;
    memcpy(&buf[1], net_state.identity_pubkey, 32);
    if (!ws_send_binary(buf, REGISTER_PUBKEY_MSG_SIZE))
        return false;
    printf("[net] sent pubkey registration (%02x%02x%02x%02x...)\n",
           net_state.identity_pubkey[0], net_state.identity_pubkey[1],
           net_state.identity_pubkey[2], net_state.identity_pubkey[3]);
    return true;
}

static void send_pubkey_proof_for_token(const uint8_t token[8]) {
    if (!token ||
        !net_state.identity_pubkey_ready ||
        !net_state.identity_secret_ready) {
        return;
    }
    pubkey_proof_scheme_t scheme =
        pubkey_proof_client_next_scheme(&net_state.pubkey_proof);
    if (scheme == PUBKEY_PROOF_SCHEME_NONE)
        return;
    if (scheme == PUBKEY_PROOF_SCHEME_CHALLENGE_V2 &&
        !net_state.pubkey_challenge_ready) {
        return;
    }

    uint8_t buf[PROVE_PUBKEY_MSG_SIZE];
    buf[0] = NET_MSG_PROVE_PUBKEY;
    memcpy(&buf[PROVE_PUBKEY_PUBKEY_OFFSET],
           net_state.identity_pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES);
    memcpy(&buf[PROVE_PUBKEY_TOKEN_OFFSET], token, 8);
    bool signed_ok = scheme == PUBKEY_PROOF_SCHEME_CHALLENGE_V2
        ? pubkey_proof_sign(
              &buf[PROVE_PUBKEY_SIG_OFFSET],
              net_state.identity_pubkey,
              net_state.identity_secret,
              token,
              net_state.pubkey_challenge)
        : pubkey_proof_v1_sign(
              &buf[PROVE_PUBKEY_SIG_OFFSET],
              net_state.identity_pubkey,
              net_state.identity_secret,
              token);
    if (!signed_ok)
        return;

    bool admitted = ws_send_binary(buf, PROVE_PUBKEY_MSG_SIZE);
    pubkey_proof_client_record_send(
        &net_state.pubkey_proof, scheme, admitted);
    if (!admitted) {
        /* Do not consume the challenge or latch an attempted proof. A failed
         * auth write has no guaranteed future trigger, so fail the transport
         * closed and let the normal reconnect path obtain a fresh challenge. */
        fprintf(stderr,
                "[net] pubkey proof send rejected; closing transport\n");
        ws_close_authentication_failure();
        return;
    }
    if (scheme == PUBKEY_PROOF_SCHEME_CHALLENGE_V2) {
        memset(net_state.pubkey_challenge, 0,
               sizeof(net_state.pubkey_challenge));
        net_state.pubkey_challenge_ready = false;
    }
    printf("[net] sent %s pubkey proof\n",
           scheme == PUBKEY_PROOF_SCHEME_CHALLENGE_V2
               ? "challenge-bound" : "legacy-v1");
}

static void send_pubkey_proof(void) {
    if (!net_state.session_token_ready) return;
    send_pubkey_proof_for_token(net_state.session_token);
}

void net_set_identity_pubkey(const uint8_t pubkey[32]) {
    if (!pubkey) {
        memset(net_state.identity_pubkey, 0,
               sizeof(net_state.identity_pubkey));
        net_state.identity_pubkey_ready = false;
        memset(net_state.callsign, 0, sizeof(net_state.callsign));
        net_state.callsign_ready = false;
        return;
    }
    memcpy(net_state.identity_pubkey, pubkey, 32);
    net_state.identity_pubkey_ready = true;
    /* Now that we have a pubkey, the callsign can be derived. If
     * ensure_callsign() ran earlier and bailed because the pubkey
     * wasn't set yet, the callsign[] is still zeroed — clear the
     * ready flag so the next ensure_callsign() call does the work. */
    net_state.callsign_ready = false;
}

void net_set_identity_secret(const uint8_t secret[64]) {
    if (!secret) {
        memset(net_state.identity_secret, 0, sizeof(net_state.identity_secret));
        net_state.identity_secret_ready = false;
        return;
    }
    memcpy(net_state.identity_secret, secret, 64);
    net_state.identity_secret_ready = true;
}

bool net_has_identity_secret(void) {
    return net_state.identity_secret_ready;
}

bool net_has_identity_pubkey(void) {
    return net_state.identity_pubkey_ready;
}

/* Allocate a strictly-increasing nonce. Seeded on first call to the
 * current wall-clock in microseconds so a process restart still beats
 * any nonce we used last run (the server's persisted last_signed_nonce
 * also gates this, but the client cooperating means fewer rejects). */
static uint64_t next_signed_action_nonce(void) {
    uint64_t now_us;
#ifdef __EMSCRIPTEN__
    /* MUST be wall-clock (Date.now), not performance.now (which is
     * monotonic-since-page-load and starts at zero). The server
     * persists last_signed_nonce in wall-clock microseconds; a
     * page-load-relative nonce always loses to a saved one and every
     * signed action gets rejected as a replay.
     *
     * Bug history: emscripten_get_now() returns performance.now() in
     * ms — small numbers (0..10^7-ish over a long session). The
     * comment USED to say "Date.now()" but the implementation called
     * a different function. emscripten_date_now() is the dedicated
     * wall-clock helper (Date.now() in ms). */
    now_us = (uint64_t)(emscripten_date_now() * 1000.0);
#elif defined(_WIN32)
    /* MSVC has no clock_gettime/CLOCK_REALTIME. GetSystemTimePreciseAsFileTime
     * returns 100-ns ticks since 1601-01-01 UTC; subtract the 1601→1970
     * delta and divide to microseconds since the Unix epoch. */
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t ticks_100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    now_us = (ticks_100ns - 116444736000000000ULL) / 10ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
#endif
    if (now_us <= net_state.signed_action_nonce)
        net_state.signed_action_nonce += 1;
    else
        net_state.signed_action_nonce = now_us;
    return net_state.signed_action_nonce;
}

bool net_send_signed_action(uint8_t action_type,
                            const uint8_t *payload, uint16_t payload_len) {
    if (!net_state.identity_secret_ready) return false;
    if (payload_len > SIGNED_ACTION_MAX_PAYLOAD) return false;

    /* On-stack scratch is fine: max-sized message is 12 + 256 + 64 = 332. */
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + SIGNED_ACTION_MAX_PAYLOAD +
                SIGNED_ACTION_SIG_SIZE];
    uint64_t nonce = next_signed_action_nonce();
    buf[0] = NET_MSG_SIGNED_ACTION;
    for (int i = 0; i < 8; i++) buf[1 + i] = (uint8_t)(nonce >> (i * 8));
    buf[9]  = action_type;
    buf[10] = (uint8_t)(payload_len & 0xFF);
    buf[11] = (uint8_t)(payload_len >> 8);
    if (payload && payload_len) memcpy(&buf[12], payload, payload_len);
    /* Sign (nonce || action_type || payload_len || payload) =
     * exactly bytes [1..12+payload_len). The leading message-type byte
     * and trailing signature are NOT part of the signed envelope. */
    signal_crypto_sign(&buf[12 + payload_len],
                       &buf[1], (size_t)(11 + (int)payload_len),
                       net_state.identity_secret);
    int total = SIGNED_ACTION_HEADER_SIZE + (int)payload_len +
                (int)SIGNED_ACTION_SIG_SIZE;
    return ws_send_binary(buf, total);
}

bool net_send_claim_legacy_save(const char *token_basename) {
    if (!token_basename || !net_state.identity_secret_ready) return false;
    size_t hex_len = strlen(token_basename);
    if (hex_len == 0 || hex_len > 64) return false;
    /* Sign domain || token_hex with the persistent identity. */
    const char *domain = CLAIM_LEGACY_SAVE_DOMAIN;
    size_t dlen = strlen(domain);
    uint8_t msg[64 + 64];
    if (dlen + hex_len > sizeof(msg)) return false;
    memcpy(msg, domain, dlen);
    memcpy(msg + dlen, token_basename, hex_len);
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, dlen + hex_len, net_state.identity_secret);

    uint8_t buf[2 + 64 + SIGNAL_CRYPTO_SIG_BYTES];
    buf[0] = NET_MSG_CLAIM_LEGACY_SAVE;
    buf[1] = (uint8_t)hex_len;
    memcpy(&buf[2], token_basename, hex_len);
    memcpy(&buf[2 + hex_len], sig, SIGNAL_CRYPTO_SIG_BYTES);
    if (!ws_send_binary(
            buf, (int)(2 + hex_len + SIGNAL_CRYPTO_SIG_BYTES))) {
        return false;
    }
    printf("[net] sent legacy-save claim for %s\n", token_basename);
    return true;
}

static bool send_session_token(void) {
    uint8_t buf[16]; /* type(1) + token(8) + callsign(7) */
    buf[0] = NET_MSG_SESSION;
    memcpy(&buf[1], net_state.session_token, 8);
    memcpy(&buf[9], net_state.callsign, 7);
    if (!ws_send_binary(buf, 16))
        return false;
    send_pubkey_proof();
    if (!net_state.connected)
        return false;
    printf("[net] sent session token + callsign %s\n", net_state.callsign);
    return true;
}

static void handle_message(const uint8_t* data, int len) {
    if (len < 1) return;

    switch (data[0]) {
    case NET_MSG_PUBKEY_CHALLENGE:
        if (len != PUBKEY_CHALLENGE_MSG_SIZE ||
            net_state.pubkey_challenge_ready ||
            (net_state.pubkey_proof.proof_admitted &&
             net_state.pubkey_proof.admitted_scheme ==
                 PUBKEY_PROOF_SCHEME_CHALLENGE_V2)) {
            break;
        }
        {
            uint8_t any = 0;
            for (int i = 1; i < PUBKEY_CHALLENGE_MSG_SIZE; i++)
                any |= data[i];
            if (any == 0) {
                memset(net_state.pubkey_challenge, 0,
                       sizeof(net_state.pubkey_challenge));
                net_state.pubkey_challenge_ready = false;
                break;
            }
            memcpy(net_state.pubkey_challenge, &data[1],
                   sizeof(net_state.pubkey_challenge));
            net_state.pubkey_challenge_ready = true;
            /* Challenge receipt wins over an earlier legacy fallback. A
             * protocol-v3 proof is sent even if PROTOCOL_INFO is delayed or
             * (incorrectly) advertises an older version. */
            pubkey_proof_client_note_challenge(&net_state.pubkey_proof);
            send_pubkey_proof();
        }
        break;

    case NET_MSG_JOIN:
        if (len < 2) break;
        {
            uint8_t id = data[1];
            if (net_state.local_id == 0xFF) {
                net_state.local_id = id;
                printf("[net] assigned player id %d\n", id);
            } else if (id != net_state.local_id) {
                if (id < NET_MAX_PLAYERS) {
                    net_state.players[id].player_id = id;
                    net_state.players[id].active = true;
                }
                if (net_state.callbacks.on_join) {
                    net_state.callbacks.on_join(id);
                }
                printf("[net] player %d joined\n", id);
            }
        }
        break;

    case NET_MSG_LEAVE:
        if (len < 2) break;
        {
            uint8_t id = data[1];
            if (id < NET_MAX_PLAYERS) {
                net_state.players[id].active = false;
                net_state.player_motion_q_valid[id] = false;
            }
            if (net_state.callbacks.on_leave) {
                net_state.callbacks.on_leave(id);
            }
            printf("[net] player %d left\n", id);
        }
        break;

    case NET_MSG_ACTION_ACK:
        if (len < NET_ACTION_ACK_SIZE) break;
        {
            uint16_t action_id = read_u16_le(&data[1]);
            uint16_t input_seq = read_u16_le(&data[3]);
            uint8_t status = data[5];
            uint8_t action = data[6];
            if (net_state.callbacks.on_action_ack) {
                net_state.callbacks.on_action_ack(action_id, input_seq,
                                                  status, action);
            }
        }
        break;

    case NET_MSG_ACTION_RESULT:
        if (len < NET_ACTION_RESULT_SIZE) break;
        {
            uint16_t action_id = read_u16_le(&data[1]);
            uint16_t input_seq = read_u16_le(&data[3]);
            uint8_t status = data[5];
            uint8_t action = data[6];
            uint32_t server_tick = read_u32_le(&data[7]);
            if (net_state.callbacks.on_action_result) {
                net_state.callbacks.on_action_result(action_id, input_seq,
                                                     status, action,
                                                     server_tick);
            }
        }
        break;

    case NET_MSG_INPUT_APPLIED:
        if (len < NET_INPUT_APPLIED_LEGACY_SIZE) break;
        {
            uint16_t input_seq = read_u16_le(&data[1]);
            uint32_t server_tick = read_u32_le(&data[3]);
            uint32_t input_tick_ack = read_u32_le(&data[7]);
            uint32_t client_sent_ms = len >= NET_INPUT_APPLIED_SIZE
                ? read_u32_le(&data[11]) : 0;
            uint32_t server_recv_ms = len >= NET_INPUT_APPLIED_SIZE
                ? read_u32_le(&data[15]) : 0;
            uint32_t server_send_ms = len >= NET_INPUT_APPLIED_SIZE
                ? read_u32_le(&data[19]) : 0;
            if (net_state.callbacks.on_input_applied) {
                net_state.callbacks.on_input_applied(input_seq, server_tick,
                                                     input_tick_ack,
                                                     client_sent_ms,
                                                     server_recv_ms,
                                                     server_send_ms);
            }
        }
        break;

    case NET_MSG_HANDOFF_TICKET:
        if ((size_t)len < 4u + HANDOFF_TICKET_SIZE) break;
        {
            uint8_t status = data[1];
            uint8_t source_station = data[2];
            uint8_t dest_station = data[3];
            handoff_ticket_t ticket;
            memset(&ticket, 0, sizeof(ticket));
            (void)handoff_ticket_unpack(&data[4], &ticket);
            if (net_state.callbacks.on_handoff_ticket) {
                net_state.callbacks.on_handoff_ticket(
                    status, source_station, dest_station,
                    status == NET_HANDOFF_STATUS_OK ? &ticket : NULL);
            }
        }
        break;

    case NET_MSG_HANDOFF_RESULT:
        if (len < NET_HANDOFF_RESULT_SIZE) break;
        {
            uint8_t status = data[1];
            uint8_t reason = data[2];
            uint8_t dest_station = data[3];
            if (net_state.callbacks.on_handoff_result) {
                net_state.callbacks.on_handoff_result(status, reason,
                                                      dest_station,
                                                      &data[4]);
            }
        }
        break;

    case NET_MSG_LATENCY_PONG:
        if (len < NET_LATENCY_PONG_LEGACY_SIZE) break;
        {
            uint32_t seq = read_u32_le(&data[1]);
            uint32_t client_sent_ms = read_u32_le(&data[5]);
            uint32_t server_recv_ms = read_u32_le(&data[9]);
            uint32_t server_send_ms = read_u32_le(&data[13]);
            uint32_t server_tick = len >= NET_LATENCY_PONG_SIZE ?
                read_u32_le(&data[17]) : 0;
            if (seq == 0) break;
            net_latency_ping_track_t *slot =
                &net_state.latency_pings[seq % NET_LATENCY_PING_TRACK_CAP];
            if (slot->seq != seq || slot->sent_ms != client_sent_ms) break;
            uint32_t now_ms = net_now_ms32();
            float rtt_ms = (float)(uint32_t)(now_ms - slot->sent_ms);
            float server_turnaround_ms =
                (float)(uint32_t)(server_send_ms - server_recv_ms);
            slot->seq = 0;
            slot->sent_ms = 0;
            if (net_state.callbacks.on_latency_sample) {
                net_state.callbacks.on_latency_sample(seq, rtt_ms,
                                                      server_turnaround_ms,
                                                      server_tick);
            }
        }
        break;

    case NET_MSG_STATE:
        if (len < 22) break;
        {
            uint8_t id = data[1];
            if (id >= NET_MAX_PLAYERS) break;

            NetPlayerState* ps = &net_state.players[id];
            ps->player_id = id;
            ps->x     = read_f32_le(&data[2]);
            ps->y     = read_f32_le(&data[6]);
            ps->vx    = read_f32_le(&data[10]);
            ps->vy    = read_f32_le(&data[14]);
            ps->angle = read_f32_le(&data[18]);
            ps->flags = (len >= 23) ? data[22] : 0;
            ps->tractor_level = (len >= 24) ? data[23] : 0;
            ps->towed_count = (len >= 25) ? data[24] : 0;
            if (len >= 45) {
                for (int t = 0; t < 10; t++)
                    ps->towed_fragments[t] = (uint16_t)data[25 + t * 2]
                                           | ((uint16_t)data[25 + t * 2 + 1] << 8);
            } else {
                for (int t = 0; t < 10; t++) ps->towed_fragments[t] = 0xFFFFu;
            }
            if (len >= NET_STATE_AUTH_LEGACY_SIZE) {
                ps->input_seq_ack =
                    read_u16_le(&data[NET_STATE_AUTH_INPUT_ACK_OFFSET]);
                ps->server_tick =
                    read_u32_le(&data[NET_STATE_AUTH_SERVER_TICK_OFFSET]);
                ps->input_tick_ack =
                    read_u32_le(&data[NET_STATE_AUTH_INPUT_TICK_OFFSET]);
                ps->has_input_tick_ack = true;
                if (len >= NET_STATE_AUTH_SIZE) {
                    ps->ack_client_sent_ms = read_u32_le(
                        &data[NET_STATE_AUTH_CLIENT_SENT_MS_OFFSET]);
                    ps->ack_server_recv_ms = read_u32_le(
                        &data[NET_STATE_AUTH_SERVER_RECV_MS_OFFSET]);
                    ps->ack_server_send_ms = read_u32_le(
                        &data[NET_STATE_AUTH_SERVER_SEND_MS_OFFSET]);
                } else {
                    net_player_state_clear_ack_transport(ps);
                }
            } else {
                ps->input_seq_ack = 0;
                ps->server_tick = 0;
                ps->input_tick_ack = 0;
                ps->has_input_tick_ack = false;
                net_player_state_clear_ack_transport(ps);
            }
            ps->active = true;
            net_player_motion_q_note_float(id, ps->x, ps->y);

            if (net_state.callbacks.on_state) {
                net_state.callbacks.on_state(ps);
            }
        }
        break;

    case NET_MSG_WORLD_PLAYERS:
        if (len < 2) break;
        {
            int count = (int)data[1];
            int record_size = PLAYER_RECORD_SIZE;
            int expected = 2 + count * record_size;
            if (len < expected && len >= 2 + count * 73) {
                record_size = 73; /* pre-input-tick-ack server */
                expected = 2 + count * record_size;
            }
            if (len < expected && len >= 2 + count * 67) {
                record_size = 67; /* pre-input-ack server */
                expected = 2 + count * record_size;
            }
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[2 + i * record_size];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS) continue;
                NetPlayerState* ps = &net_state.players[id];
                ps->player_id = id;
                ps->x     = read_f32_le(&p[1]);
                ps->y     = read_f32_le(&p[5]);
                ps->vx    = read_f32_le(&p[9]);
                ps->vy    = read_f32_le(&p[13]);
                ps->angle = read_f32_le(&p[17]);
                ps->flags = p[21];
                ps->tractor_level = p[22];
                ps->towed_count = p[23];
                for (int t = 0; t < 10; t++)
                    ps->towed_fragments[t] = (uint16_t)p[24 + t * 2]
                                           | ((uint16_t)p[24 + t * 2 + 1] << 8);
                memcpy(ps->callsign, &p[44], 7);
                ps->callsign[7] = '\0';
                ps->beam_start_x = read_f32_le(&p[51]);
                ps->beam_start_y = read_f32_le(&p[55]);
                ps->beam_end_x   = read_f32_le(&p[59]);
                ps->beam_end_y   = read_f32_le(&p[63]);
                if (record_size >= 73) {
                    ps->input_seq_ack = read_u16_le(&p[67]);
                    ps->server_tick   = read_u32_le(&p[69]);
                } else {
                    ps->input_seq_ack = 0;
                    ps->server_tick = 0;
                }
                ps->input_tick_ack =
                    (record_size >= 77) ? read_u32_le(&p[73]) : 0;
                ps->has_input_tick_ack = record_size >= 77;
                net_player_state_clear_ack_transport(ps);
                ps->active = true;
                net_player_motion_q_note_float(id, ps->x, ps->y);
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_PLAYER_MOTION:
        if (len < PLAYER_MOTION_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = PLAYER_MOTION_MSG_HEADER +
                count * PLAYER_MOTION_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PLAYER_MOTION_MSG_HEADER +
                                         i * PLAYER_MOTION_RECORD_SIZE];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS || id == net_state.local_id)
                    continue;
                NetPlayerState* ps = &net_state.players[id];
                ps->player_id = id;
                ps->x     = read_f32_le(&p[1]);
                ps->y     = read_f32_le(&p[5]);
                ps->vx    = read_f32_le(&p[9]);
                ps->vy    = read_f32_le(&p[13]);
                ps->angle = read_f32_le(&p[17]);
                ps->active = true;
                net_player_motion_q_note_float(id, ps->x, ps->y);
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_PLAYER_MOTION_Q:
        if (len < PLAYER_MOTION_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = PLAYER_MOTION_Q_MSG_HEADER +
                count * PLAYER_MOTION_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PLAYER_MOTION_Q_MSG_HEADER +
                                         i * PLAYER_MOTION_Q_RECORD_SIZE];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS || id == net_state.local_id)
                    continue;
                NetPlayerState* ps = &net_state.players[id];
                ps->player_id = id;
                int16_t qx = (int16_t)read_u16_le(&p[1]);
                int16_t qy = (int16_t)read_u16_le(&p[3]);
                ps->x = (float)qx *
                        PLAYER_MOTION_Q_POS_SCALE;
                ps->y = (float)qy *
                        PLAYER_MOTION_Q_POS_SCALE;
                ps->vx = (float)(int16_t)read_u16_le(&p[5]) *
                         PLAYER_MOTION_Q_VEL_SCALE;
                ps->vy = (float)(int16_t)read_u16_le(&p[7]) *
                         PLAYER_MOTION_Q_VEL_SCALE;
                ps->angle = ((float)p[9] / 256.0f) * 6.28318530718f;
                ps->active = true;
                net_player_motion_q_note(id, qx, qy);
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_PLAYER_MOTIOND_Q:
        if (len < PLAYER_MOTIOND_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = PLAYER_MOTIOND_Q_MSG_HEADER +
                count * PLAYER_MOTIOND_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PLAYER_MOTIOND_Q_MSG_HEADER +
                                         i * PLAYER_MOTIOND_Q_RECORD_SIZE];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS || id == net_state.local_id ||
                    !net_state.player_motion_q_valid[id]) {
                    continue;
                }
                int qx = (int)net_state.player_motion_qx[id] +
                    (int)(int8_t)p[1];
                int qy = (int)net_state.player_motion_qy[id] +
                    (int)(int8_t)p[2];
                if (qx < -32768 || qx > 32767 ||
                    qy < -32768 || qy > 32767) {
                    net_state.player_motion_q_valid[id] = false;
                    continue;
                }
                int16_t next_qx = (int16_t)qx;
                int16_t next_qy = (int16_t)qy;
                NetPlayerState* ps = &net_state.players[id];
                ps->player_id = id;
                ps->x = (float)next_qx * PLAYER_MOTION_Q_POS_SCALE;
                ps->y = (float)next_qy * PLAYER_MOTION_Q_POS_SCALE;
                ps->vx = (float)(int8_t)p[3] *
                    PLAYER_MOTIOND_Q_VEL_SCALE;
                ps->vy = (float)(int8_t)p[4] *
                    PLAYER_MOTIOND_Q_VEL_SCALE;
                ps->angle = ((float)p[5] / 256.0f) * 6.28318530718f;
                ps->active = true;
                net_player_motion_q_note(id, next_qx, next_qy);
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_PLAYER_POSED_Q:
        if (len < PLAYER_POSED_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = PLAYER_POSED_Q_MSG_HEADER +
                count * PLAYER_POSED_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PLAYER_POSED_Q_MSG_HEADER +
                                         i * PLAYER_POSED_Q_RECORD_SIZE];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS || id == net_state.local_id ||
                    !net_state.player_motion_q_valid[id]) {
                    continue;
                }
                int qx = (int)net_state.player_motion_qx[id] +
                    (int)(int8_t)p[1];
                int qy = (int)net_state.player_motion_qy[id] +
                    (int)(int8_t)p[2];
                if (qx < -32768 || qx > 32767 ||
                    qy < -32768 || qy > 32767) {
                    net_state.player_motion_q_valid[id] = false;
                    continue;
                }
                int16_t next_qx = (int16_t)qx;
                int16_t next_qy = (int16_t)qy;
                NetPlayerState *ps = &net_state.players[id];
                ps->player_id = id;
                ps->x = (float)next_qx * PLAYER_MOTION_Q_POS_SCALE;
                ps->y = (float)next_qy * PLAYER_MOTION_Q_POS_SCALE;
                ps->angle = ((float)p[3] / 256.0f) * 6.28318530718f;
                ps->active = true;
                net_player_motion_q_note(id, next_qx, next_qy);
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_PLAYER_MOTIONM_Q:
        if (len < PLAYER_MOTIONM_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int off = PLAYER_MOTIONM_Q_MSG_HEADER;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                if (off + PLAYER_MOTIONM_Q_POSE_RECORD_SIZE > len)
                    break;
                uint8_t id_flags = data[off++];
                uint8_t id = id_flags & PLAYER_MOTIONM_Q_ID_MASK;
                bool has_velocity =
                    (id_flags & PLAYER_MOTIONM_Q_FLAG_VEL) != 0;
                bool reserved =
                    (id_flags & PLAYER_MOTIONM_Q_RESERVED_MASK) != 0;
                int8_t dx = (int8_t)data[off++];
                int8_t dy = (int8_t)data[off++];
                int8_t qvx = 0;
                int8_t qvy = 0;
                if (has_velocity) {
                    if (off + 3 > len)
                        break;
                    qvx = (int8_t)data[off++];
                    qvy = (int8_t)data[off++];
                } else {
                    if (off + 1 > len)
                        break;
                }
                uint8_t angle = data[off++];
                if (reserved || id >= NET_MAX_PLAYERS ||
                    id == net_state.local_id ||
                    !net_state.player_motion_q_valid[id]) {
                    continue;
                }
                int qx = (int)net_state.player_motion_qx[id] + (int)dx;
                int qy = (int)net_state.player_motion_qy[id] + (int)dy;
                if (qx < -32768 || qx > 32767 ||
                    qy < -32768 || qy > 32767) {
                    net_state.player_motion_q_valid[id] = false;
                    continue;
                }
                int16_t next_qx = (int16_t)qx;
                int16_t next_qy = (int16_t)qy;
                NetPlayerState *ps = &net_state.players[id];
                ps->player_id = id;
                ps->x = (float)next_qx * PLAYER_MOTION_Q_POS_SCALE;
                ps->y = (float)next_qy * PLAYER_MOTION_Q_POS_SCALE;
                if (has_velocity) {
                    ps->vx = (float)qvx * PLAYER_MOTIOND_Q_VEL_SCALE;
                    ps->vy = (float)qvy * PLAYER_MOTIOND_Q_VEL_SCALE;
                }
                ps->angle = ((float)angle / 256.0f) * 6.28318530718f;
                ps->active = true;
                net_player_motion_q_note(id, next_qx, next_qy);
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_PLAYER_DOCK_Q:
        if (len < PLAYER_DOCK_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = PLAYER_DOCK_MSG_HEADER +
                count * PLAYER_DOCK_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PLAYER_DOCK_MSG_HEADER +
                                         i * PLAYER_DOCK_RECORD_SIZE];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS || id == net_state.local_id)
                    continue;
                NetPlayerState* ps = &net_state.players[id];
                ps->player_id = id;
                uint8_t status_flags =
                    p[1] & PLAYER_DOCK_STATUS_FLAGS_MASK;
                ps->flags = (uint8_t)(
                    (ps->flags &
                     (uint8_t)(UINT8_MAX ^ PLAYER_DOCK_STATUS_FLAGS_MASK)) |
                    status_flags);
                if ((status_flags & 0x04u) != 0u) {
                    ps->vx = 0.0f;
                    ps->vy = 0.0f;
                }
                if (ps->active && net_state.callbacks.on_state) {
                    NetPlayerState status = *ps;
                    status.flags |= NET_PLAYER_STATE_STATUS_ONLY;
                    net_state.callbacks.on_state(&status);
                }
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROIDS:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroids) {
                /* File-scope buffer sized to MAX_ASTEROIDS so a dense belt
                 * view (which can now exceed 512 rocks post-#285) isn't
                 * truncated. Stack allocation would blow the WASM main
                 * thread's 64KB stack. */
                static NetAsteroidState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[3 + i * ASTEROID_RECORD_SIZE];
                    arr[i].index  = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].flags  = p[2];
                    arr[i].x      = read_f32_le(&p[3]);
                    arr[i].y      = read_f32_le(&p[7]);
                    arr[i].vx     = read_f32_le(&p[11]);
                    arr[i].vy     = read_f32_le(&p[15]);
                    arr[i].hp     = read_f32_le(&p[19]);
                    arr[i].ore    = read_f32_le(&p[23]);
                    arr[i].radius = read_f32_le(&p[27]);
                    arr[i].smelt_progress = (float)p[31] / 255.0f;
                    arr[i].grade = p[32];
                    arr[i].crystal_stage = p[33];
                    arr[i].phase = p[34];
                    if (arr[i].flags & 1)
                        net_asteroid_pos_q_note_float(
                            arr[i].index, arr[i].x, arr[i].y);
                    else
                        net_asteroid_pos_q_clear(arr[i].index);
                }
                net_state.callbacks.on_asteroids(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROIDS_Q:
        if (len < ASTEROID_Q_MSG_HEADER) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = ASTEROID_Q_MSG_HEADER +
                count * ASTEROID_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroids) {
                static NetAsteroidState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[ASTEROID_Q_MSG_HEADER +
                              i * ASTEROID_Q_RECORD_SIZE];
                    int16_t qx = (int16_t)(p[3] | ((uint16_t)p[4] << 8));
                    int16_t qy = (int16_t)(p[5] | ((uint16_t)p[6] << 8));
                    int16_t qvx = (int16_t)(p[7] | ((uint16_t)p[8] << 8));
                    int16_t qvy = (int16_t)(p[9] | ((uint16_t)p[10] << 8));
                    arr[i].index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].flags = p[2];
                    arr[i].x = (float)qx * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].y = (float)qy * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].vx = (float)qvx * ASTEROID_MOTION_Q_VEL_SCALE;
                    arr[i].vy = (float)qvy * ASTEROID_MOTION_Q_VEL_SCALE;
                    arr[i].hp = net_asteroid_identity_q_decode_value(
                        read_u16_le(&p[11]));
                    arr[i].ore = net_asteroid_identity_q_decode_value(
                        read_u16_le(&p[13]));
                    arr[i].radius = net_asteroid_identity_q_decode_value(
                        read_u16_le(&p[15]));
                    arr[i].smelt_progress = (float)p[17] / 255.0f;
                    net_asteroid_identity_q_unpack_detail(
                        p[18], &arr[i].grade, &arr[i].crystal_stage,
                        &arr[i].phase);
                    if (arr[i].flags & 1)
                        net_asteroid_pos_q_note(arr[i].index, qx, qy);
                    else
                        net_asteroid_pos_q_clear(arr[i].index);
                }
                net_state.callbacks.on_asteroids(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROIDS8_Q:
        if (len < ASTEROID8_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = ASTEROID8_Q_MSG_HEADER +
                count * ASTEROID8_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroids) {
                static NetAsteroidState arr[MAX_ASTEROIDS];
                for (int i = 0; i < count; i++) {
                    const uint8_t* p =
                        &data[ASTEROID8_Q_MSG_HEADER +
                              i * ASTEROID8_Q_RECORD_SIZE];
                    int16_t qx = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
                    int16_t qy = (int16_t)(p[4] | ((uint16_t)p[5] << 8));
                    int16_t qvx = (int16_t)(p[6] | ((uint16_t)p[7] << 8));
                    int16_t qvy = (int16_t)(p[8] | ((uint16_t)p[9] << 8));
                    arr[i].index = p[0];
                    arr[i].flags = p[1];
                    arr[i].x = (float)qx * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].y = (float)qy * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].vx = (float)qvx * ASTEROID_MOTION_Q_VEL_SCALE;
                    arr[i].vy = (float)qvy * ASTEROID_MOTION_Q_VEL_SCALE;
                    arr[i].hp = net_asteroid_identity_q_decode_value(
                        read_u16_le(&p[10]));
                    arr[i].ore = net_asteroid_identity_q_decode_value(
                        read_u16_le(&p[12]));
                    arr[i].radius = net_asteroid_identity_q_decode_value(
                        read_u16_le(&p[14]));
                    arr[i].smelt_progress = (float)p[16] / 255.0f;
                    net_asteroid_identity_q_unpack_detail(
                        p[17], &arr[i].grade, &arr[i].crystal_stage,
                        &arr[i].phase);
                    if (arr[i].flags & 1)
                        net_asteroid_pos_q_note(arr[i].index, qx, qy);
                    else
                        net_asteroid_pos_q_clear(arr[i].index);
                }
                net_state.callbacks.on_asteroids(arr, count);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_REMOVE:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_REMOVE_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroids) {
                static NetAsteroidState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[3 + i * ASTEROID_REMOVE_RECORD_SIZE];
                    memset(&arr[i], 0, sizeof(arr[i]));
                    arr[i].index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].flags = 0;
                    net_asteroid_pos_q_clear(arr[i].index);
                }
                net_state.callbacks.on_asteroids(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_MOTION:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_MOTION_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_motion) {
                static NetAsteroidMotionState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[3 + i * ASTEROID_MOTION_RECORD_SIZE];
                    arr[i].index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].x     = read_f32_le(&p[2]);
                    arr[i].y     = read_f32_le(&p[6]);
                    arr[i].vx    = read_f32_le(&p[10]);
                    arr[i].vy    = read_f32_le(&p[14]);
                    net_asteroid_pos_q_note_float(
                        arr[i].index, arr[i].x, arr[i].y);
                }
                net_state.callbacks.on_asteroid_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_MOTION_Q:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_MOTION_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_motion) {
                static NetAsteroidMotionState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[3 + i * ASTEROID_MOTION_Q_RECORD_SIZE];
                    int16_t qx = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
                    int16_t qy = (int16_t)(p[4] | ((uint16_t)p[5] << 8));
                    int16_t qvx = (int16_t)(p[6] | ((uint16_t)p[7] << 8));
                    int16_t qvy = (int16_t)(p[8] | ((uint16_t)p[9] << 8));
                    arr[i].index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].x     = (float)qx * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].y     = (float)qy * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].vx    = (float)qvx * ASTEROID_MOTION_Q_VEL_SCALE;
                    arr[i].vy    = (float)qvy * ASTEROID_MOTION_Q_VEL_SCALE;
                    net_asteroid_pos_q_note(arr[i].index, qx, qy);
                }
                net_state.callbacks.on_asteroid_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_POS_Q:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_POS_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_motion) {
                static NetAsteroidMotionState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[3 + i * ASTEROID_POS_Q_RECORD_SIZE];
                    int16_t qx = (int16_t)(p[2] | ((uint16_t)p[3] << 8));
                    int16_t qy = (int16_t)(p[4] | ((uint16_t)p[5] << 8));
                    arr[i].index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].x     = (float)qx * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].y     = (float)qy * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].vx    = NAN;
                    arr[i].vy    = NAN;
                    net_asteroid_pos_q_note(arr[i].index, qx, qy);
                }
                net_state.callbacks.on_asteroid_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_POS8_Q:
        if (len < ASTEROID_POS8_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = ASTEROID_POS8_Q_MSG_HEADER +
                count * ASTEROID_POS8_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_motion) {
                static NetAsteroidMotionState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[ASTEROID_POS8_Q_MSG_HEADER +
                              i * ASTEROID_POS8_Q_RECORD_SIZE];
                    int16_t qx = (int16_t)(p[1] | ((uint16_t)p[2] << 8));
                    int16_t qy = (int16_t)(p[3] | ((uint16_t)p[4] << 8));
                    arr[i].index = p[0];
                    arr[i].x     = (float)qx * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].y     = (float)qy * ASTEROID_MOTION_Q_POS_SCALE;
                    arr[i].vx    = NAN;
                    arr[i].vy    = NAN;
                    net_asteroid_pos_q_note(arr[i].index, qx, qy);
                }
                net_state.callbacks.on_asteroid_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_POSD_Q:
        if (len < ASTEROID_POSD_Q_MSG_HEADER) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = ASTEROID_POSD_Q_MSG_HEADER +
                count * ASTEROID_POSD_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_motion) {
                static NetAsteroidMotionState arr[MAX_ASTEROIDS];
                int limit = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                int decoded = 0;
                for (int i = 0; i < limit; i++) {
                    const uint8_t* p =
                        &data[ASTEROID_POSD_Q_MSG_HEADER +
                              i * ASTEROID_POSD_Q_RECORD_SIZE];
                    uint16_t index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    if (index >= MAX_ASTEROIDS ||
                        !net_state.asteroid_pos_q_valid[index])
                        continue;
                    int qx = (int)net_state.asteroid_pos_qx[index] +
                        (int)(int8_t)p[2];
                    int qy = (int)net_state.asteroid_pos_qy[index] +
                        (int)(int8_t)p[3];
                    if (qx < -32768 || qx > 32767 ||
                        qy < -32768 || qy > 32767)
                        continue;
                    int16_t next_qx = (int16_t)qx;
                    int16_t next_qy = (int16_t)qy;
                    net_asteroid_pos_q_note(index, next_qx, next_qy);
                    arr[decoded].index = index;
                    arr[decoded].x = (float)next_qx *
                        ASTEROID_MOTION_Q_POS_SCALE;
                    arr[decoded].y = (float)next_qy *
                        ASTEROID_MOTION_Q_POS_SCALE;
                    arr[decoded].vx = NAN;
                    arr[decoded].vy = NAN;
                    decoded++;
                }
                if (decoded > 0)
                    net_state.callbacks.on_asteroid_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_POSD8_Q:
        if (len < ASTEROID_POSD8_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = ASTEROID_POSD8_Q_MSG_HEADER +
                count * ASTEROID_POSD8_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_motion) {
                static NetAsteroidMotionState arr[MAX_ASTEROIDS];
                int decoded = 0;
                for (int i = 0; i < count; i++) {
                    const uint8_t* p =
                        &data[ASTEROID_POSD8_Q_MSG_HEADER +
                              i * ASTEROID_POSD8_Q_RECORD_SIZE];
                    uint16_t index = p[0];
                    if (!net_state.asteroid_pos_q_valid[index])
                        continue;
                    int qx = (int)net_state.asteroid_pos_qx[index] +
                        (int)(int8_t)p[1];
                    int qy = (int)net_state.asteroid_pos_qy[index] +
                        (int)(int8_t)p[2];
                    if (qx < -32768 || qx > 32767 ||
                        qy < -32768 || qy > 32767)
                        continue;
                    int16_t next_qx = (int16_t)qx;
                    int16_t next_qy = (int16_t)qy;
                    net_asteroid_pos_q_note(index, next_qx, next_qy);
                    arr[decoded].index = index;
                    arr[decoded].x = (float)next_qx *
                        ASTEROID_MOTION_Q_POS_SCALE;
                    arr[decoded].y = (float)next_qy *
                        ASTEROID_MOTION_Q_POS_SCALE;
                    arr[decoded].vx = NAN;
                    arr[decoded].vy = NAN;
                    decoded++;
                }
                if (decoded > 0)
                    net_state.callbacks.on_asteroid_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROID_STATE_Q:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_STATE_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroid_state_q) {
                static NetAsteroidStateQ arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p =
                        &data[3 + i * ASTEROID_STATE_Q_RECORD_SIZE];
                    arr[i].index = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].hp = read_f32_le(&p[2]);
                    arr[i].ore = read_f32_le(&p[6]);
                    arr[i].radius = read_f32_le(&p[10]);
                    arr[i].smelt_progress = (float)p[14] / 255.0f;
                    arr[i].grade = p[15];
                    arr[i].crystal_stage = p[16];
                    arr[i].phase = p[17];
                }
                net_state.callbacks.on_asteroid_state_q(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPCS:
        if (len < 2) break;
        {
            int count = (int)data[1];
            int expected = 2 + count * NPC_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npcs) {
                NetNpcState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[2 + i * NPC_RECORD_SIZE];
                    arr[i].index            = p[0];
                    arr[i].flags            = p[1];
                    arr[i].x                = read_f32_le(&p[2]);
                    arr[i].y                = read_f32_le(&p[6]);
                    arr[i].vx               = read_f32_le(&p[10]);
                    arr[i].vy               = read_f32_le(&p[14]);
                    arr[i].angle            = read_f32_le(&p[18]);
                    uint16_t target = read_u16_le(&p[22]);
                    uint16_t towed  = read_u16_le(&p[24]);
                    arr[i].target_asteroid  = (target == 0xFFFFu) ? -1 : (int)target;
                    arr[i].towed_fragment   = (towed == 0xFFFFu) ? -1 : (int)towed;
                    arr[i].tint_r           = p[26];
                    arr[i].tint_g           = p[27];
                    arr[i].tint_b           = p[28];
                    memcpy(arr[i].session_token, &p[29], sizeof(arr[i].session_token));
                    arr[i].home_station     = p[37];
                }
                net_state.callbacks.on_npcs(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_MOTION:
        if (len < NPC_MOTION_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_MOTION_MSG_HEADER +
                count * NPC_MOTION_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_motion) {
                NetNpcMotionState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_MOTION_MSG_HEADER +
                                             i * NPC_MOTION_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].flags = p[1];
                    arr[i].x     = read_f32_le(&p[2]);
                    arr[i].y     = read_f32_le(&p[6]);
                    arr[i].vx    = read_f32_le(&p[10]);
                    arr[i].vy    = read_f32_le(&p[14]);
                    arr[i].angle = read_f32_le(&p[18]);
                }
                net_state.callbacks.on_npc_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_MOTION_Q:
        if (len < NPC_MOTION_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_MOTION_Q_MSG_HEADER +
                count * NPC_MOTION_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_motion) {
                NetNpcMotionState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_MOTION_Q_MSG_HEADER +
                                             i * NPC_MOTION_Q_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].flags = p[1];
                    arr[i].x     = (float)(int16_t)read_u16_le(&p[2]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].y     = (float)(int16_t)read_u16_le(&p[4]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].vx    = (float)(int16_t)read_u16_le(&p[6]) *
                        NPC_MOTION_Q_VEL_SCALE;
                    arr[i].vy    = (float)(int16_t)read_u16_le(&p[8]) *
                        NPC_MOTION_Q_VEL_SCALE;
                    arr[i].angle = (float)read_u16_le(&p[10]) *
                        NPC_MOTION_Q_ANGLE_SCALE;
                }
                net_state.callbacks.on_npc_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_MOTION8_Q:
        if (len < NPC_MOTION8_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_MOTION8_Q_MSG_HEADER +
                count * NPC_MOTION8_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_motion) {
                NetNpcMotionState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_MOTION8_Q_MSG_HEADER +
                                             i * NPC_MOTION8_Q_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].flags = p[1];
                    arr[i].x     = (float)(int16_t)read_u16_le(&p[2]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].y     = (float)(int16_t)read_u16_le(&p[4]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].vx    = (float)(int8_t)p[6] *
                        NPC_MOTION8_Q_VEL_SCALE;
                    arr[i].vy    = (float)(int8_t)p[7] *
                        NPC_MOTION8_Q_VEL_SCALE;
                    arr[i].angle = (float)p[8] *
                        NPC_MOTION8_Q_ANGLE_SCALE;
                }
                net_state.callbacks.on_npc_motion(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_POS_Q:
        if (len < NPC_POS_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_POS_Q_MSG_HEADER +
                count * NPC_POS_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_pos) {
                NetNpcPosState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_POS_Q_MSG_HEADER +
                                             i * NPC_POS_Q_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].x = (float)(int16_t)read_u16_le(&p[1]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].y = (float)(int16_t)read_u16_le(&p[3]) *
                        NPC_MOTION_Q_POS_SCALE;
                }
                net_state.callbacks.on_npc_pos(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_POSE_Q:
        if (len < NPC_POSE_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_POSE_Q_MSG_HEADER +
                count * NPC_POSE_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_pose) {
                NetNpcPoseState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_POSE_Q_MSG_HEADER +
                                             i * NPC_POSE_Q_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].x = (float)(int16_t)read_u16_le(&p[1]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].y = (float)(int16_t)read_u16_le(&p[3]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].angle = (float)read_u16_le(&p[5]) *
                        NPC_MOTION_Q_ANGLE_SCALE;
                }
                net_state.callbacks.on_npc_pose(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_LINEAR_Q:
        if (len < NPC_LINEAR_Q_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_LINEAR_Q_MSG_HEADER +
                count * NPC_LINEAR_Q_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_linear) {
                NetNpcLinearState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_LINEAR_Q_MSG_HEADER +
                                             i * NPC_LINEAR_Q_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].x = (float)(int16_t)read_u16_le(&p[1]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].y = (float)(int16_t)read_u16_le(&p[3]) *
                        NPC_MOTION_Q_POS_SCALE;
                    arr[i].vx = (float)(int16_t)read_u16_le(&p[5]) *
                        NPC_MOTION_Q_VEL_SCALE;
                    arr[i].vy = (float)(int16_t)read_u16_le(&p[7]) *
                        NPC_MOTION_Q_VEL_SCALE;
                }
                net_state.callbacks.on_npc_linear(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_STATUS:
        if (len < NPC_STATUS_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_STATUS_MSG_HEADER +
                count * NPC_STATUS_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_status) {
                NetNpcStatusState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_STATUS_MSG_HEADER +
                                             i * NPC_STATUS_RECORD_SIZE];
                    uint16_t target = read_u16_le(&p[2]);
                    uint16_t towed = read_u16_le(&p[4]);
                    arr[i].index = p[0];
                    arr[i].flags = p[1];
                    arr[i].target_asteroid =
                        (target == 0xFFFFu) ? -1 : (int)target;
                    arr[i].towed_fragment =
                        (towed == 0xFFFFu) ? -1 : (int)towed;
                }
                net_state.callbacks.on_npc_status(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPC_STATUS8_Q:
        if (len < NPC_STATUS8_MSG_HEADER) break;
        {
            int count = (int)data[1];
            int expected = NPC_STATUS8_MSG_HEADER +
                count * NPC_STATUS8_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npc_status) {
                NetNpcStatusState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[NPC_STATUS8_MSG_HEADER +
                                             i * NPC_STATUS8_RECORD_SIZE];
                    arr[i].index = p[0];
                    arr[i].flags = p[1];
                    arr[i].target_asteroid =
                        (p[2] == 0xFFu) ? -1 : (int)p[2];
                    arr[i].towed_fragment =
                        (p[3] == 0xFFu) ? -1 : (int)p[3];
                }
                net_state.callbacks.on_npc_status(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_STATIONS:
        if (len < 2) break;
        {
            uint8_t count = data[1];
            if (len < 2 + count * STATION_RECORD_SIZE) break;
            if (net_state.callbacks.on_stations) {
                for (int i = 0; i < count; i++) {
                    const uint8_t *p = &data[2 + i * STATION_RECORD_SIZE];
                    uint8_t idx = p[0];
                    float inv[COMMODITY_COUNT];
                    for (int j = 0; j < COMMODITY_COUNT; j++)
                        inv[j] = read_f32_le(&p[1 + j * 4]);
                    float pool = read_f32_le(&p[1 + COMMODITY_COUNT * 4]);
                    net_state.callbacks.on_stations(idx, inv, pool);
                }
            }
        }
        break;

    case NET_MSG_WORLD_STATIONS_Q:
        if (len < STATION_Q_HEADER_SIZE) break;
        {
            uint8_t count = data[1];
            int off = STATION_Q_HEADER_SIZE;
            if (!net_state.callbacks.on_stations) break;
            for (int i = 0; i < count; i++) {
                if (off + 3 > len) break;
                uint8_t idx = data[off++];
                uint16_t mask = read_u16_le(&data[off]);
                off += 2;
                if ((mask & (uint16_t)(UINT16_MAX ^
                                       (STATION_Q_COMMODITY_MASK |
                                        STATION_Q_CREDIT_POOL_MASK))) != 0) {
                    break;
                }
                float inv[COMMODITY_COUNT];
                memset(inv, 0, sizeof(inv));
                bool ok = true;
                for (int c = 0; c < COMMODITY_COUNT; c++) {
                    if ((mask & (uint16_t)(1u << c)) == 0)
                        continue;
                    if (off + 4 > len) {
                        ok = false;
                        break;
                    }
                    inv[c] = read_f32_le(&data[off]);
                    off += 4;
                }
                if (!ok) break;
                float pool = 0.0f;
                if (mask & STATION_Q_CREDIT_POOL_MASK) {
                    if (off + 4 > len) break;
                    pool = read_f32_le(&data[off]);
                    off += 4;
                }
                net_state.callbacks.on_stations(idx, inv, pool);
            }
        }
        break;

    case NET_MSG_PLAYER_SHIP:
        if (len < 16 + COMMODITY_COUNT * 4) break;
        {
            uint8_t id = data[1];
            if (id != net_state.local_id) break;
            if (net_state.callbacks.on_player_ship) {
                NetPlayerShipState pss = {0};
                pss.player_id       = id;
                pss.hull            = read_f32_le(&data[2]);
                pss.station_balance  = read_f32_le(&data[6]);
                pss.docked          = data[10] != 0;
                pss.current_station = data[11];
                pss.mining_level    = data[12];
                pss.hold_level      = data[13];
                pss.tractor_level   = data[14];
                pss.autopilot_mode  = data[15]; /* repurposed reserved byte */
                for (int c = 0; c < COMMODITY_COUNT; c++)
                    pss.cargo[c] = read_f32_le(&data[16 + c * 4]);
                int off = 16 + COMMODITY_COUNT * 4;
                if (len >= off + 23) {
                    pss.nearby_fragments = data[off];
                    pss.tractor_fragments = data[off + 1];
                    pss.towed_count = data[off + 2];
                    for (int t = 0; t < 10; t++)
                        pss.towed_fragments[t] = (uint16_t)data[off + 3 + t * 2]
                                               | ((uint16_t)data[off + 3 + t * 2 + 1] << 8);
                    pss.autopilot_target = (len >= off + 24) ? data[off + 23] : 0xFF;
                    /* A* path waypoints from server. */
                    int path_off = off + 24;
                    if (len >= path_off + 2) {
                        pss.path_count = data[path_off];
                        pss.path_current = data[path_off + 1];
                        if (pss.path_count > 12) pss.path_count = 12;
                        for (int i = 0; i < pss.path_count && path_off + 2 + i * 8 + 8 <= len; i++) {
                            pss.path_x[i] = read_f32_le(&data[path_off + 2 + i * 8]);
                            pss.path_y[i] = read_f32_le(&data[path_off + 2 + i * 8 + 4]);
                        }
                    }
                } else {
                    for (int t = 0; t < 10; t++) pss.towed_fragments[t] = 0xFFFFu;
                    pss.autopilot_target = 0xFF;
                }
                net_state.callbacks.on_player_ship(&pss);
            }
        }
        break;

    case NET_MSG_STATION_IDENTITY:
        if (len >= STATION_IDENTITY_V1_SIZE && net_state.callbacks.on_station_identity) {
            NetStationIdentity si = {0};
            si.index = data[1];
            si.flags = data[2];
            si.services = read_u32_le(&data[3]);
            si.pos_x = read_f32_le(&data[7]);
            si.pos_y = read_f32_le(&data[11]);
            si.radius = read_f32_le(&data[15]);
            si.dock_radius = read_f32_le(&data[19]);
            si.signal_range = read_f32_le(&data[23]);
            memcpy(si.name, &data[27], 31);
            si.name[31] = '\0';
            for (int c = 0; c < COMMODITY_COUNT; c++)
                si.base_price[c] = read_f32_le(&data[59 + c * 4]);
            si.scaffold_progress = read_f32_le(&data[59 + COMMODITY_COUNT * 4]);
            int moff = 59 + COMMODITY_COUNT * 4 + 4;
            si.module_count = data[moff];
            if (si.module_count > MAX_MODULES_PER_STATION)
                si.module_count = MAX_MODULES_PER_STATION;
            moff++;
            for (int m = 0; m < si.module_count; m++) {
                si.modules[m].type = (module_type_t)data[moff];
                si.modules[m].scaffold = data[moff + 1] != 0;
                si.modules[m].ring = data[moff + 2];
                si.modules[m].slot = data[moff + 3];
                si.modules[m].build_progress = read_f32_le(&data[moff + 4]);
                si.modules[m].commodity = data[moff + 8];
                if (si.modules[m].commodity > COMMODITY_COUNT)
                    si.modules[m].commodity = (uint8_t)COMMODITY_COUNT;
                moff += STATION_MODULE_RECORD_SIZE;
            }
            /* Skip over unused module record slots to reach arm data */
            moff = 59 + COMMODITY_COUNT * 4 + 4 + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE;
            si.arm_count = data[moff];
            if (si.arm_count > MAX_ARMS) si.arm_count = MAX_ARMS;
            moff++;
            for (int a = 0; a < MAX_ARMS; a++)
                si.arm_speed[a] = read_f32_le(&data[moff + a * 4]);
            moff += MAX_ARMS * 4;
            for (int a = 0; a < MAX_ARMS; a++)
                si.ring_offset[a] = read_f32_le(&data[moff + a * 4]);
            moff += MAX_ARMS * 4;
            for (int a = 0; a < MAX_ARMS; a++)
                si.arm_rotation[a] = read_f32_le(&data[moff + a * 4]);
            moff += MAX_ARMS * 4;
            for (int a = 0; a < MAX_ARMS; a++)
                si.arm_omega[a] = read_f32_le(&data[moff + a * 4]);
            moff += MAX_ARMS * 4;
            /* Placement plans */
            si.plan_count = data[moff];
            if (si.plan_count > STATION_PLAN_RECORD_COUNT) si.plan_count = STATION_PLAN_RECORD_COUNT;
            moff++;
            for (int p = 0; p < STATION_PLAN_RECORD_COUNT; p++) {
                si.plans[p].type  = (module_type_t)data[moff + 0];
                si.plans[p].ring  = data[moff + 1];
                si.plans[p].slot  = data[moff + 2];
                si.plans[p].owner = (int8_t)data[moff + 3];
                moff += STATION_PLAN_RECORD_SIZE;
            }
            /* Pending shipyard orders */
            si.pending_scaffold_count = data[moff];
            if (si.pending_scaffold_count > STATION_PENDING_SCAFFOLD_RECORD_COUNT)
                si.pending_scaffold_count = STATION_PENDING_SCAFFOLD_RECORD_COUNT;
            moff++;
            for (int p = 0; p < STATION_PENDING_SCAFFOLD_RECORD_COUNT; p++) {
                si.pending_scaffolds[p].type  = (module_type_t)data[moff + 0];
                int8_t owner = (int8_t)data[moff + 1];
                si.pending_scaffolds[p].owner = (data[moff + 1] == 0xFF) ? -1 : owner;
                moff += STATION_PENDING_SCAFFOLD_RECORD_SIZE;
            }
            si.pending_ship_build_count = data[moff];
            if (si.pending_ship_build_count > STATION_PENDING_SHIP_RECORD_COUNT)
                si.pending_ship_build_count = STATION_PENDING_SHIP_RECORD_COUNT;
            moff++;
            for (int p = 0; p < STATION_PENDING_SHIP_RECORD_COUNT; p++) {
                si.pending_ship_builds[p].hull_class = (hull_class_t)data[moff + 0];
                /* moff + 1 is the retired runtime-owner byte. */
                si.pending_ship_builds[p].build_progress = read_f32_le(&data[moff + 2]);
                moff += STATION_PENDING_SHIP_RECORD_SIZE;
            }
            memcpy(si.hail_message, &data[moff], STATION_IDENTITY_HAIL_MESSAGE_LEN - 1);
            si.hail_message[STATION_IDENTITY_HAIL_MESSAGE_LEN - 1] = '\0';
            moff += STATION_IDENTITY_HAIL_MESSAGE_LEN;
            for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
                memcpy(si.miner_chatter[i], &data[moff], STATION_IDENTITY_CHATTER_LINE_LEN - 1);
                si.miner_chatter[i][STATION_IDENTITY_CHATTER_LINE_LEN - 1] = '\0';
                moff += STATION_IDENTITY_CHATTER_LINE_LEN;
            }
            for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
                memcpy(si.hauler_chatter[i], &data[moff], STATION_IDENTITY_CHATTER_LINE_LEN - 1);
                si.hauler_chatter[i][STATION_IDENTITY_CHATTER_LINE_LEN - 1] = '\0';
                moff += STATION_IDENTITY_CHATTER_LINE_LEN;
            }
            memcpy(si.rati_hail_message, &data[moff], STATION_IDENTITY_RATI_HAIL_LEN - 1);
            si.rati_hail_message[STATION_IDENTITY_RATI_HAIL_LEN - 1] = '\0';
            moff += STATION_IDENTITY_RATI_HAIL_LEN;
            /* Currency name trailer — 32 bytes, null-padded. */
            memcpy(si.currency_name, &data[moff], STATION_IDENTITY_CURRENCY_NAME_LEN - 1);
            si.currency_name[STATION_IDENTITY_CURRENCY_NAME_LEN - 1] = '\0';
            moff += STATION_IDENTITY_CURRENCY_NAME_LEN;
            /* Station Ed25519 pubkey (#479 B). The server only sends the
             * pubkey; private material stays operator-side. */
            memcpy(si.station_pubkey, &data[moff], STATION_IDENTITY_PUBKEY_LEN);
            moff += STATION_IDENTITY_PUBKEY_LEN;
            if (len >= STATION_IDENTITY_HULL_SIZE) {
                for (int h = 0; h < HULL_CLASS_COUNT; h++)
                    si.stored_hull_count[h] = data[moff++];
            }
            if (len >= STATION_IDENTITY_FACTION_TRAILER_SIZE) {
                si.faction_id = data[moff++];
                si.faction_allegiance = data[moff++];
                si.faction_ideology = data[moff++];
                for (int f = 0; f < STATION_FACTION_COUNT; f++)
                    si.faction_relations[f] = (int8_t)data[moff++];
            }
            if (len >= STATION_IDENTITY_SIZE) {
                uint8_t policy_n = data[moff++];
                si.policy_card_count = policy_n > STATION_IDENTITY_POLICY_CARD_COUNT
                    ? STATION_IDENTITY_POLICY_CARD_COUNT
                    : policy_n;
                for (int i = 0; i < STATION_IDENTITY_POLICY_CARD_COUNT; i++)
                    si.policy_card_ids[i] = data[moff++];
            }
            (void)moff;
            net_state.callbacks.on_station_identity(&si);
        }
        break;

    case NET_MSG_STATION_IDENTITY_Q:
        if (net_state.callbacks.on_station_identity) {
            NetStationIdentity si;
            if (decode_station_identity_q(&si, data, len))
                net_state.callbacks.on_station_identity(&si);
        }
        break;

    case NET_MSG_STATION_DIAG:
        if (len >= STATION_DIAG_SIZE && net_state.callbacks.on_station_diag) {
            uint8_t station_id = data[1];
            int module_count = data[2];
            if (module_count > MAX_MODULES_PER_STATION)
                module_count = MAX_MODULES_PER_STATION;
            net_state.callbacks.on_station_diag(station_id, &data[3], module_count);
        }
        break;

    case NET_MSG_WORLD_SCAFFOLDS:
        if (len >= 2 && net_state.callbacks.on_scaffolds) {
            int count = data[1];
            if (count < 0) count = 0;
            if (count * SCAFFOLD_RECORD_SIZE + 2 > len)
                count = (len - 2) / SCAFFOLD_RECORD_SIZE;
            NetScaffoldState scaffolds[16];
            int max = (count > 16) ? 16 : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p = &data[2 + i * SCAFFOLD_RECORD_SIZE];
                scaffolds[i].index       = p[0];
                scaffolds[i].state       = p[1];
                scaffolds[i].module_type = p[2];
                scaffolds[i].owner       = (p[3] == 0xFF) ? -1 : (int8_t)p[3];
                scaffolds[i].pos_x       = read_f32_le(&p[4]);
                scaffolds[i].pos_y       = read_f32_le(&p[8]);
                scaffolds[i].vel_x       = read_f32_le(&p[12]);
                scaffolds[i].vel_y       = read_f32_le(&p[16]);
                scaffolds[i].radius      = read_f32_le(&p[20]);
                scaffolds[i].build_amount= read_f32_le(&p[24]);
                scaffolds[i].built_at_station =
                    (p[28] == 0xFFu) ? -1 : (int16_t)p[28];
            }
            net_state.callbacks.on_scaffolds(scaffolds, max);
        }
        break;

    case NET_MSG_WORLD_SCAFFOLD_REMOVE:
        if (len >= SCAFFOLD_REMOVE_MSG_HEADER &&
            net_state.callbacks.on_scaffold_remove) {
            int count = data[1];
            int expected = SCAFFOLD_REMOVE_MSG_HEADER +
                count * SCAFFOLD_REMOVE_RECORD_SIZE;
            if (len < expected) break;
            uint8_t indices[MAX_SCAFFOLDS];
            int max = count > MAX_SCAFFOLDS ? MAX_SCAFFOLDS : count;
            for (int i = 0; i < max; i++) {
                indices[i] = data[SCAFFOLD_REMOVE_MSG_HEADER +
                                  i * SCAFFOLD_REMOVE_RECORD_SIZE];
            }
            net_state.callbacks.on_scaffold_remove(indices, max);
        }
        break;

    case NET_MSG_WORLD_SCAFFOLD_MOTION_Q:
        if (len >= SCAFFOLD_MOTION_Q_MSG_HEADER &&
            net_state.callbacks.on_scaffold_motion) {
            int count = data[1];
            int expected = SCAFFOLD_MOTION_Q_MSG_HEADER +
                count * SCAFFOLD_MOTION_Q_RECORD_SIZE;
            if (len < expected) break;
            NetScaffoldMotionState scaffolds[MAX_SCAFFOLDS];
            int max = count > MAX_SCAFFOLDS ? MAX_SCAFFOLDS : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p =
                    &data[SCAFFOLD_MOTION_Q_MSG_HEADER +
                          i * SCAFFOLD_MOTION_Q_RECORD_SIZE];
                int16_t qx = (int16_t)(p[1] | ((uint16_t)p[2] << 8));
                int16_t qy = (int16_t)(p[3] | ((uint16_t)p[4] << 8));
                int16_t qvx = (int16_t)(p[5] | ((uint16_t)p[6] << 8));
                int16_t qvy = (int16_t)(p[7] | ((uint16_t)p[8] << 8));
                scaffolds[i].index = p[0];
                scaffolds[i].pos_x = (float)qx * SCAFFOLD_MOTION_Q_POS_SCALE;
                scaffolds[i].pos_y = (float)qy * SCAFFOLD_MOTION_Q_POS_SCALE;
                scaffolds[i].vel_x = (float)qvx * SCAFFOLD_MOTION_Q_VEL_SCALE;
                scaffolds[i].vel_y = (float)qvy * SCAFFOLD_MOTION_Q_VEL_SCALE;
            }
            net_state.callbacks.on_scaffold_motion(scaffolds, max);
        }
        break;

    case NET_MSG_WORLD_CARGO_PODS:
        if (len >= 2 && net_state.callbacks.on_cargo_pods) {
            int count = data[1];
            int expected = 2 + count * CARGO_POD_RECORD_SIZE;
            if (len < expected) break;
            NetCargoPodState pods[MAX_CARGO_PODS];
            int max = count > MAX_CARGO_PODS ? MAX_CARGO_PODS : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p = &data[2 + i * CARGO_POD_RECORD_SIZE];
                pods[i].index = p[0];
                pods[i].kind = p[1];
                pods[i].commodity = p[2];
                pods[i].tractor_player =
                    (p[3] == 0xFF) ? -1 : (int8_t)p[3];
                pods[i].pos_x = read_f32_le(&p[4]);
                pods[i].pos_y = read_f32_le(&p[8]);
                pods[i].vel_x = read_f32_le(&p[12]);
                pods[i].vel_y = read_f32_le(&p[16]);
                pods[i].radius = read_f32_le(&p[20]);
                pods[i].rotation = read_f32_le(&p[24]);
                pods[i].quantity = (uint16_t)p[28] | ((uint16_t)p[29] << 8);
                pods[i].manifest_count = read_u16_le(&p[30]);
                pods[i].shipment_id = read_u16_le(&p[32]);
                pods[i].summary_flags = p[34];
                pods[i].summary_grade = p[35];
                pods[i].tractor_station = p[36];
                pods[i].tractor_module = p[37];
                pods[i].tow_hardpoint_tag = p[38];
            }
            net_state.callbacks.on_cargo_pods(pods, max);
        }
        break;

    case NET_MSG_WORLD_CARGO_PODS_Q:
        if (len >= 2 && net_state.callbacks.on_cargo_pods) {
            int count = data[1];
            int expected = 2 + count * CARGO_POD_Q_RECORD_SIZE;
            if (len < expected) break;
            NetCargoPodState pods[MAX_CARGO_PODS];
            int max = count > MAX_CARGO_PODS ? MAX_CARGO_PODS : count;
            const float two_pi = 6.28318530717958647692f;
            for (int i = 0; i < max; i++) {
                const uint8_t *p = &data[2 + i * CARGO_POD_Q_RECORD_SIZE];
                int16_t qx = (int16_t)(p[4] | ((uint16_t)p[5] << 8));
                int16_t qy = (int16_t)(p[6] | ((uint16_t)p[7] << 8));
                int16_t qvx = (int16_t)(p[8] | ((uint16_t)p[9] << 8));
                int16_t qvy = (int16_t)(p[10] | ((uint16_t)p[11] << 8));
                uint16_t qrot = (uint16_t)(p[16] | ((uint16_t)p[17] << 8));
                pods[i].index = p[0];
                pods[i].kind = p[1];
                pods[i].commodity = p[2];
                pods[i].tractor_player =
                    (p[3] == 0xFF) ? -1 : (int8_t)p[3];
                pods[i].pos_x = (float)qx * CARGO_POD_MOTION_Q_POS_SCALE;
                pods[i].pos_y = (float)qy * CARGO_POD_MOTION_Q_POS_SCALE;
                pods[i].vel_x = (float)qvx * CARGO_POD_MOTION_Q_VEL_SCALE;
                pods[i].vel_y = (float)qvy * CARGO_POD_MOTION_Q_VEL_SCALE;
                pods[i].radius = read_f32_le(&p[12]);
                pods[i].rotation = ((float)qrot / 65536.0f) * two_pi;
                pods[i].quantity = read_u16_le(&p[18]);
                pods[i].manifest_count = read_u16_le(&p[20]);
                pods[i].shipment_id = read_u16_le(&p[22]);
                pods[i].summary_flags = p[24];
                pods[i].summary_grade = p[25];
                pods[i].tractor_station = p[26];
                pods[i].tractor_module = p[27];
                pods[i].tow_hardpoint_tag = p[28];
            }
            net_state.callbacks.on_cargo_pods(pods, max);
        }
        break;

    case NET_MSG_WORLD_CARGO_POD_REMOVE:
        if (len >= CARGO_POD_REMOVE_MSG_HEADER &&
            net_state.callbacks.on_cargo_pod_remove) {
            int count = data[1];
            int expected = CARGO_POD_REMOVE_MSG_HEADER +
                count * CARGO_POD_REMOVE_RECORD_SIZE;
            if (len < expected) break;
            uint8_t indices[MAX_CARGO_PODS];
            int max = count > MAX_CARGO_PODS ? MAX_CARGO_PODS : count;
            for (int i = 0; i < max; i++) {
                indices[i] = data[CARGO_POD_REMOVE_MSG_HEADER +
                                  i * CARGO_POD_REMOVE_RECORD_SIZE];
            }
            net_state.callbacks.on_cargo_pod_remove(indices, max);
        }
        break;

    case NET_MSG_WORLD_CARGO_POD_MOTION:
        if (len >= CARGO_POD_MOTION_MSG_HEADER &&
            net_state.callbacks.on_cargo_pod_motion) {
            int count = data[1];
            int expected = CARGO_POD_MOTION_MSG_HEADER +
                count * CARGO_POD_MOTION_RECORD_SIZE;
            if (len < expected) break;
            NetCargoPodMotionState pods[MAX_CARGO_PODS];
            int max = count > MAX_CARGO_PODS ? MAX_CARGO_PODS : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p =
                    &data[CARGO_POD_MOTION_MSG_HEADER +
                          i * CARGO_POD_MOTION_RECORD_SIZE];
                pods[i].index = p[0];
                pods[i].pos_x = read_f32_le(&p[1]);
                pods[i].pos_y = read_f32_le(&p[5]);
                pods[i].vel_x = read_f32_le(&p[9]);
                pods[i].vel_y = read_f32_le(&p[13]);
                pods[i].rotation = read_f32_le(&p[17]);
            }
            net_state.callbacks.on_cargo_pod_motion(pods, max);
        }
        break;

    case NET_MSG_WORLD_CARGO_POD_MOTION_Q:
        if (len >= CARGO_POD_MOTION_Q_MSG_HEADER &&
            net_state.callbacks.on_cargo_pod_motion) {
            int count = data[1];
            int expected = CARGO_POD_MOTION_Q_MSG_HEADER +
                count * CARGO_POD_MOTION_Q_RECORD_SIZE;
            if (len < expected) break;
            NetCargoPodMotionState pods[MAX_CARGO_PODS];
            int max = count > MAX_CARGO_PODS ? MAX_CARGO_PODS : count;
            const float two_pi = 6.28318530717958647692f;
            for (int i = 0; i < max; i++) {
                const uint8_t *p =
                    &data[CARGO_POD_MOTION_Q_MSG_HEADER +
                          i * CARGO_POD_MOTION_Q_RECORD_SIZE];
                int16_t qx = (int16_t)(p[1] | ((uint16_t)p[2] << 8));
                int16_t qy = (int16_t)(p[3] | ((uint16_t)p[4] << 8));
                int16_t qvx = (int16_t)(p[5] | ((uint16_t)p[6] << 8));
                int16_t qvy = (int16_t)(p[7] | ((uint16_t)p[8] << 8));
                uint16_t qrot = (uint16_t)(p[9] | ((uint16_t)p[10] << 8));
                pods[i].index = p[0];
                pods[i].pos_x = (float)qx * CARGO_POD_MOTION_Q_POS_SCALE;
                pods[i].pos_y = (float)qy * CARGO_POD_MOTION_Q_POS_SCALE;
                pods[i].vel_x = (float)qvx * CARGO_POD_MOTION_Q_VEL_SCALE;
                pods[i].vel_y = (float)qvy * CARGO_POD_MOTION_Q_VEL_SCALE;
                pods[i].rotation = ((float)qrot / 65536.0f) * two_pi;
            }
            net_state.callbacks.on_cargo_pod_motion(pods, max);
        }
        break;

    case NET_MSG_WORLD_CARGO_POD_LINEAR_Q:
        if (len >= CARGO_POD_LINEAR_Q_MSG_HEADER &&
            net_state.callbacks.on_cargo_pod_linear) {
            int count = data[1];
            int expected = CARGO_POD_LINEAR_Q_MSG_HEADER +
                count * CARGO_POD_LINEAR_Q_RECORD_SIZE;
            if (len < expected) break;
            NetCargoPodLinearState pods[MAX_CARGO_PODS];
            int max = count > MAX_CARGO_PODS ? MAX_CARGO_PODS : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p =
                    &data[CARGO_POD_LINEAR_Q_MSG_HEADER +
                          i * CARGO_POD_LINEAR_Q_RECORD_SIZE];
                int16_t qx = (int16_t)(p[1] | ((uint16_t)p[2] << 8));
                int16_t qy = (int16_t)(p[3] | ((uint16_t)p[4] << 8));
                int16_t qvx = (int16_t)(p[5] | ((uint16_t)p[6] << 8));
                int16_t qvy = (int16_t)(p[7] | ((uint16_t)p[8] << 8));
                pods[i].index = p[0];
                pods[i].pos_x = (float)qx * CARGO_POD_MOTION_Q_POS_SCALE;
                pods[i].pos_y = (float)qy * CARGO_POD_MOTION_Q_POS_SCALE;
                pods[i].vel_x = (float)qvx * CARGO_POD_MOTION_Q_VEL_SCALE;
                pods[i].vel_y = (float)qvy * CARGO_POD_MOTION_Q_VEL_SCALE;
            }
            net_state.callbacks.on_cargo_pod_linear(pods, max);
        }
        break;

    case NET_MSG_WORLD_INTERACTIONS:
        if (len >= 2 && net_state.callbacks.on_interactions) {
            int count = data[1];
            int expected = 2 + count * INTERACTION_RECORD_SIZE;
            if (len < expected) break;
            sim_interaction_t items[SIM_MAX_INTERACTIONS];
            int max = count > SIM_MAX_INTERACTIONS ? SIM_MAX_INTERACTIONS : count;
            memset(items, 0, sizeof(items));
            for (int i = 0; i < max; i++) {
                const uint8_t *p = &data[2 + i * INTERACTION_RECORD_SIZE];
                sim_interaction_t *it = &items[i];
                it->type = p[0];
                it->visual = p[1];
                it->commodity = p[2];
                it->flags = p[3];
                it->source.type = p[4];
                it->source.index = (int16_t)read_u16_le(&p[5]);
                it->source.aux = (int16_t)read_u16_le(&p[7]);
                it->target.type = p[9];
                it->target.index = (int16_t)read_u16_le(&p[10]);
                it->target.aux = (int16_t)read_u16_le(&p[12]);
                it->source_pos.x = read_f32_le(&p[14]);
                it->source_pos.y = read_f32_le(&p[18]);
                it->target_pos.x = read_f32_le(&p[22]);
                it->target_pos.y = read_f32_le(&p[26]);
                it->range = read_f32_le(&p[30]);
                it->intensity = read_f32_le(&p[34]);
            }
            net_state.callbacks.on_interactions(items, max);
        }
        break;

    case NET_MSG_WORLD_INTERACTIONS_Q:
        if (len >= 2 && net_state.callbacks.on_interactions) {
            int count = data[1];
            int expected = 2 + count * INTERACTION_Q_RECORD_SIZE;
            if (len < expected) break;
            sim_interaction_t items[SIM_MAX_INTERACTIONS];
            int max = count > SIM_MAX_INTERACTIONS ? SIM_MAX_INTERACTIONS : count;
            memset(items, 0, sizeof(items));
            for (int i = 0; i < max; i++) {
                const uint8_t *p = &data[2 + i * INTERACTION_Q_RECORD_SIZE];
                sim_interaction_t *it = &items[i];
                it->type = p[0];
                it->visual = p[1];
                it->commodity = p[2];
                it->flags = p[3];
                it->source.type = p[4];
                it->source.index = (int16_t)read_u16_le(&p[5]);
                it->source.aux = (int16_t)read_u16_le(&p[7]);
                it->target.type = p[9];
                it->target.index = (int16_t)read_u16_le(&p[10]);
                it->target.aux = (int16_t)read_u16_le(&p[12]);
                it->source_pos.x = (float)(int16_t)read_u16_le(&p[14]) *
                    INTERACTION_DRIFT_POS_SCALE;
                it->source_pos.y = (float)(int16_t)read_u16_le(&p[16]) *
                    INTERACTION_DRIFT_POS_SCALE;
                it->target_pos.x = (float)(int16_t)read_u16_le(&p[18]) *
                    INTERACTION_DRIFT_POS_SCALE;
                it->target_pos.y = (float)(int16_t)read_u16_le(&p[20]) *
                    INTERACTION_DRIFT_POS_SCALE;
                it->range = (float)read_u16_le(&p[22]) *
                    INTERACTION_DRIFT_RANGE_SCALE;
                it->intensity = (float)p[24] / 255.0f;
            }
            net_state.callbacks.on_interactions(items, max);
        }
        break;

    case NET_MSG_WORLD_INTERACTION_DRIFT:
        if (len >= INTERACTION_DRIFT_MSG_HEADER &&
            net_state.callbacks.on_interaction_drift) {
            int count = data[1];
            int expected = INTERACTION_DRIFT_MSG_HEADER +
                count * INTERACTION_DRIFT_RECORD_SIZE;
            if (len < expected) break;
            NetInteractionDriftState items[SIM_MAX_INTERACTIONS];
            int max = count > SIM_MAX_INTERACTIONS
                ? SIM_MAX_INTERACTIONS : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p =
                    &data[INTERACTION_DRIFT_MSG_HEADER +
                          i * INTERACTION_DRIFT_RECORD_SIZE];
                items[i].index = p[0];
                items[i].source_x =
                    (float)(int16_t)read_u16_le(&p[1]) *
                    INTERACTION_DRIFT_POS_SCALE;
                items[i].source_y =
                    (float)(int16_t)read_u16_le(&p[3]) *
                    INTERACTION_DRIFT_POS_SCALE;
                items[i].target_x =
                    (float)(int16_t)read_u16_le(&p[5]) *
                    INTERACTION_DRIFT_POS_SCALE;
                items[i].target_y =
                    (float)(int16_t)read_u16_le(&p[7]) *
                    INTERACTION_DRIFT_POS_SCALE;
                items[i].range =
                    (float)read_u16_le(&p[9]) *
                    INTERACTION_DRIFT_RANGE_SCALE;
                items[i].intensity = (float)p[11] / 255.0f;
            }
            net_state.callbacks.on_interaction_drift(items, max);
        }
        break;

    case NET_MSG_HAIL_RESPONSE:
        if (len >= 6 && net_state.callbacks.on_hail_response) {
            uint8_t station = data[1];
            float credits = read_f32_le(&data[2]);
            /* Contract idx added in the hail-as-quest change; old servers
             * that don't send it decode as "no contract" (0xFF). */
            int contract_index = (len >= 7 && data[6] != 0xFF) ? (int)data[6] : -1;
            NetHailReason reason = {0};
            if (len >= NET_HAIL_RESPONSE_REASON_SIZE) {
                reason.flags = read_u32_le(&data[7]);
                reason.signal_quality = read_f32_le(&data[11]);
                reason.candidate_count = data[15];
                reason.mode = data[16];
                reason.source_id = read_u64_le(&data[17]);
            }
            net_state.callbacks.on_hail_response(
                station, credits, contract_index, &reason);
        }
        break;

    case NET_MSG_EVENTS:
        if (len >= 2 && net_state.callbacks.on_events) {
            int ecount = data[1];
            if (ecount > SIM_MAX_EVENTS) ecount = SIM_MAX_EVENTS;
            if ((int)len < 2 + ecount * NET_EVENT_RECORD_SIZE) break;
            sim_event_t evbuf[SIM_MAX_EVENTS];
            for (int i = 0; i < ecount; i++) {
                const uint8_t *p = &data[2 + i * NET_EVENT_RECORD_SIZE];
                sim_event_t *ev = &evbuf[i];
                memset(ev, 0, sizeof(*ev));
                ev->type = (sim_event_type_t)p[0];
                ev->player_id = (int)p[1];
                switch (ev->type) {
                case SIM_EVENT_FRACTURE:
                    ev->fracture.tier = (asteroid_tier_t)p[2]; break;
                case SIM_EVENT_PICKUP:
                    ev->pickup.ore = read_f32_le(&p[2]);
                    ev->pickup.fragments = (int)p[6]; break;
                case SIM_EVENT_UPGRADE:
                    ev->upgrade.upgrade = (ship_upgrade_t)p[2]; break;
                case SIM_EVENT_DAMAGE:
                    ev->damage.amount   = read_f32_le(&p[2]);
                    ev->damage.source_x = read_f32_le(&p[6]);
                    ev->damage.source_y = read_f32_le(&p[10]); break;
                case SIM_EVENT_NPC_KILL:
                    ev->npc_kill.cause    = p[2];
                    ev->npc_kill.npc_role = p[3];
                    memcpy(ev->npc_kill.killer_token, &p[4], 8); break;
                case SIM_EVENT_DEATH:
                    /* Broadcast slice only (cinematic fields stay
                     * zero — the victim gets the full payload via
                     * NET_MSG_DEATH; non-victims just need to know a
                     * death happened so they can render a kill
                     * confirm + scoreboard tally). */
                    ev->death.cause = p[2];
                    memcpy(ev->death.killer_token, &p[3], 8); break;
                case SIM_EVENT_OUTPOST_PLACED:
                    ev->outpost_placed.slot = (int)p[2]; break;
                case SIM_EVENT_OUTPOST_ACTIVATED:
                    ev->outpost_activated.slot = (int)p[2]; break;
                case SIM_EVENT_MODULE_ACTIVATED:
                    ev->module_activated.station = (int)p[2];
                    ev->module_activated.module_idx = (int)p[3];
                    ev->module_activated.module_type = (int)p[4]; break;
                case SIM_EVENT_NPC_SPAWNED:
                    ev->npc_spawned.slot = (int)p[2];
                    ev->npc_spawned.role = (npc_role_t)p[3];
                    ev->npc_spawned.home_station = (int)p[4]; break;
                case SIM_EVENT_STATION_CONNECTED:
                    ev->station_connected.connected_count = (int)p[2]; break;
                case SIM_EVENT_CONTRACT_COMPLETE:
                    ev->contract_complete.action = (contract_action_t)p[2]; break;
                case SIM_EVENT_SCAFFOLD_READY:
                    ev->scaffold_ready.station = (int)p[2];
                    ev->scaffold_ready.module_type = (int)p[3]; break;
                case SIM_EVENT_SELL:
                    ev->sell.station     = (int)p[2];
                    ev->sell.grade       = p[3];
                    ev->sell.base_cr     = (int)read_u32_le(&p[4]);
                    ev->sell.bonus_cr    = (int)read_u32_le(&p[8]);
                    ev->sell.by_contract = p[12];
                    break;
                case SIM_EVENT_BUY:
                    ev->buy.station   = (int)p[2];
                    ev->buy.commodity = p[3];
                    ev->buy.grade     = p[4];
                    ev->buy.cost      = (int)read_u32_le(&p[5]);
                    ev->buy.quantity  = read_u16_le(&p[9]);
                    break;
                case SIM_EVENT_ORDER_REJECTED:
                    ev->order_rejected.reason = p[2];
                    break;
                default: break;
                }
            }
            net_state.callbacks.on_events(evbuf, ecount);
        }
        break;

    case NET_MSG_HIGHSCORES:
        if (len >= HIGHSCORE_HEADER && net_state.callbacks.on_highscores) {
            int count = data[1];
            int expected = HIGHSCORE_HEADER + count * HIGHSCORE_ENTRY_SIZE;
            if (len < expected) break;
            if (count > HIGHSCORE_TOP_N) count = HIGHSCORE_TOP_N;
            static NetHighscoreEntry scratch[HIGHSCORE_TOP_N];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[HIGHSCORE_HEADER + i * HIGHSCORE_ENTRY_SIZE];
                memcpy(scratch[i].callsign, p, 8);
                scratch[i].credits_earned = read_f32_le(&p[8]);
                scratch[i].world_id   = read_u32_le(&p[12]);
                scratch[i].world_seq  = read_u32_le(&p[16]);
                scratch[i].build_id   = read_u32_le(&p[20]);
                scratch[i].epoch_tick = read_u64_le(&p[24]);
                memcpy(scratch[i].killed_by, &p[32], 8);
            }
            net_state.callbacks.on_highscores(scratch, count);
        }
        break;

    case NET_MSG_PLAYER_MANIFEST:
        if (len >= PLAYER_MANIFEST_HEADER && net_state.callbacks.on_player_manifest) {
            int summary_count = (int)read_u16_le(&data[1]);
            int detail_count = (int)read_u16_le(&data[3]);
            if (summary_count > COMMODITY_COUNT * MINING_GRADE_COUNT ||
                detail_count > MANIFEST_DETAIL_MAX) {
                break;
            }
            int detail_offset = PLAYER_MANIFEST_HEADER +
                                summary_count * MANIFEST_SUMMARY_ENTRY;
            int expected = detail_offset +
                           detail_count * MANIFEST_DETAIL_ENTRY;
            if (len < expected) break;
            static NetManifestSummaryEntry
                summary[COMMODITY_COUNT * MINING_GRADE_COUNT];
            static cargo_unit_t details[MANIFEST_DETAIL_MAX];
            for (int i = 0; i < summary_count; i++) {
                const uint8_t *p = &data[PLAYER_MANIFEST_HEADER +
                                         i * MANIFEST_SUMMARY_ENTRY];
                summary[i].commodity = p[0];
                summary[i].grade = p[1];
                summary[i].count = read_u16_le(&p[2]);
            }
            for (int i = 0; i < detail_count; i++) {
                cargo_unit_wire_unpack(
                    &data[detail_offset + i * MANIFEST_DETAIL_ENTRY],
                    &details[i]);
            }
            net_state.callbacks.on_player_manifest(
                summary, summary_count, details, detail_count);
        }
        break;

    case NET_MSG_CARGO_RECEIPT_BUNDLE:
        if (len >= 3 && net_state.callbacks.on_cargo_receipt_bundle) {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * CARGO_RECEIPT_SIZE;
            if (count <= 0 || count > CARGO_RECEIPT_CHAIN_MAX_LEN) break;
            if (len < expected) break;
            cargo_receipt_t receipts[CARGO_RECEIPT_CHAIN_MAX_LEN];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[3 + i * CARGO_RECEIPT_SIZE];
                (void)cargo_receipt_unpack(p, &receipts[i]);
            }
            net_state.callbacks.on_cargo_receipt_bundle(receipts, count);
        }
        break;

    case NET_MSG_STATION_MANIFEST:
        if (len >= STATION_MANIFEST_HEADER && net_state.callbacks.on_station_manifest) {
            uint8_t station_id = data[1];
            int summary_count = (int)read_u16_le(&data[2]);
            int detail_count = (int)read_u16_le(&data[4]);
            if (summary_count > COMMODITY_COUNT * MINING_GRADE_COUNT ||
                detail_count > MANIFEST_DETAIL_MAX) {
                break;
            }
            int detail_offset = STATION_MANIFEST_HEADER +
                                summary_count * MANIFEST_SUMMARY_ENTRY;
            int expected = detail_offset +
                           detail_count * MANIFEST_DETAIL_ENTRY;
            if (len < expected) break;
            static NetManifestSummaryEntry
                summary[COMMODITY_COUNT * MINING_GRADE_COUNT];
            static cargo_unit_t details[MANIFEST_DETAIL_MAX];
            for (int i = 0; i < summary_count; i++) {
                const uint8_t *p = &data[STATION_MANIFEST_HEADER +
                                         i * MANIFEST_SUMMARY_ENTRY];
                summary[i].commodity = p[0];
                summary[i].grade = p[1];
                summary[i].count = read_u16_le(&p[2]);
            }
            for (int i = 0; i < detail_count; i++) {
                cargo_unit_wire_unpack(
                    &data[detail_offset + i * MANIFEST_DETAIL_ENTRY],
                    &details[i]);
            }
            net_state.callbacks.on_station_manifest(
                station_id, summary, summary_count, details, detail_count);
        }
        break;

    case NET_MSG_INSPECT_SNAPSHOT:
        if (len >= INSPECT_SNAPSHOT_HEADER && net_state.callbacks.on_inspect_snapshot) {
            int wire_count = data[8];
            int expected = INSPECT_SNAPSHOT_HEADER + wire_count * INSPECT_SNAPSHOT_ROW;
            if (len < expected) break;
            if (wire_count > INSPECT_SNAPSHOT_MAX_ROWS)
                wire_count = INSPECT_SNAPSHOT_MAX_ROWS;

            NetInspectSnapshot snap;
            memset(&snap, 0, sizeof(snap));
            snap.target_type = data[1];
            snap.target_index = data[2];
            snap.module_index = data[3];
            snap.role = data[4];
            snap.state = data[5];
            snap.home_station = data[6];
            snap.dest_station = data[7];
            snap.row_count = wire_count;
            snap.manifest_count = read_u16_le(&data[9]);

            for (int i = 0; i < wire_count; i++) {
                const uint8_t *p = &data[INSPECT_SNAPSHOT_HEADER + i * INSPECT_SNAPSHOT_ROW];
                NetInspectSnapshotRow *row = &snap.rows[i];
                row->commodity = p[0];
                row->grade = p[1];
                row->chain_len = p[2];
                row->flags = p[3];
                row->event_id = read_u64_le(&p[4]);
                row->quantity = read_u16_le(&p[12]);
                memcpy(row->cargo_pub, &p[14], 32);
                memcpy(row->receipt_head, &p[46], 32);
                memcpy(row->origin_station, &p[78], 32);
                memcpy(row->latest_station, &p[110], 32);
            }
            net_state.callbacks.on_inspect_snapshot(&snap);
        }
        break;

    case NET_MSG_SIGNAL_CHANNEL:
        if (len >= 3 && net_state.callbacks.on_signal_channel) {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * SIGNAL_CHANNEL_RECORD_SIZE;
            if (len < expected) break;
            /* Cap at CAPACITY so we don't blow the static buffer if a
             * server version sends more records than we expect. */
            if (count > 100) count = 100;
            static NetSignalChannelMsg msgs[100];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[3 + i * SIGNAL_CHANNEL_RECORD_SIZE];
                uint64_t id = 0;
                for (int k = 0; k < 8; k++) id |= ((uint64_t)p[k]) << (8 * k);
                uint32_t ts = 0;
                for (int k = 0; k < 4; k++) ts |= ((uint32_t)p[8 + k]) << (8 * k);
                msgs[i].id = id;
                msgs[i].timestamp_ms = ts;
                msgs[i].sender_station = (int8_t)p[12];
                int tlen = p[13];
                if (tlen >= SIGNAL_CHANNEL_TEXT_MAX) tlen = SIGNAL_CHANNEL_TEXT_MAX - 1;
                memcpy(msgs[i].text, &p[14], tlen);
                msgs[i].text[tlen] = '\0';
                memcpy(msgs[i].entry_hash, &p[14 + 200], 32);
            }
            net_state.callbacks.on_signal_channel(msgs, count);
        }
        break;

    case NET_MSG_SERVER_INFO:
        if (len >= 2) {
            int hash_len = len - 1;
            if (hash_len > 11) hash_len = 11;
            memcpy(net_state.server_hash, &data[1], (size_t)hash_len);
            net_state.server_hash[hash_len] = '\0';
            printf("[net] server version: %s\n", net_state.server_hash);
        }
        break;

    case NET_MSG_PROTOCOL_INFO:
        if (len >= PROTOCOL_INFO_HEADER_SIZE) {
            NetProtocolInfo info;
            memset(&info, 0, sizeof(info));
            info.version = read_u16_le(&data[1]);
            info.capabilities = read_u32_le(&data[3]);
            int count = data[7];
            int max_by_len = (len - PROTOCOL_INFO_HEADER_SIZE) /
                             PROTOCOL_INFO_STREAM_RECORD_SIZE;
            if (count > max_by_len) count = max_by_len;
            if (count > PROTOCOL_INFO_STREAM_CAPACITY)
                count = PROTOCOL_INFO_STREAM_CAPACITY;
            info.stream_count = count;
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PROTOCOL_INFO_HEADER_SIZE +
                                         i * PROTOCOL_INFO_STREAM_RECORD_SIZE];
                info.streams[i].msg_type = p[0];
                info.streams[i].stream_class = p[1];
                info.streams[i].flags = read_u16_le(&p[2]);
                info.streams[i].header_size = read_u16_le(&p[4]);
                info.streams[i].record_size = read_u16_le(&p[6]);
                info.streams[i].max_records = read_u16_le(&p[8]);
                info.streams[i].cadence_ms = read_u16_le(&p[10]);
            }
            net_state.protocol_info = info;
            net_state.protocol_info_ready = true;
            pubkey_proof_client_note_protocol(
                &net_state.pubkey_proof, info.version);
            /* A v2-or-older advertisement is the only permission to use the
             * legacy unchallenged proof. Protocol v3 waits for its challenge;
             * a challenge received before this packet already selected v2. */
            send_pubkey_proof();
            if (net_state.callbacks.on_protocol_info)
                net_state.callbacks.on_protocol_info(&net_state.protocol_info);
        }
        break;

    case NET_MSG_DEATH:
        if (len >= NET_DEATH_MSG_SIZE && net_state.callbacks.on_death) {
            uint8_t pid = data[1];
            float px = read_f32_le(&data[2]);
            float py = read_f32_le(&data[6]);
            float vx = read_f32_le(&data[10]);
            float vy = read_f32_le(&data[14]);
            float ang = read_f32_le(&data[18]);
            float ore = read_f32_le(&data[22]);
            float earned = read_f32_le(&data[26]);
            float spent = read_f32_le(&data[30]);
            int asteroids = (int)read_f32_le(&data[34]);
            uint8_t rs = data[38];
            float fee = read_f32_le(&data[39]);
            net_state.callbacks.on_death(pid, px, py, vx, vy, ang,
                                         ore, earned, spent, asteroids, rs, fee);
        } else if (len >= 38 && net_state.callbacks.on_death) {
            /* Legacy 38-byte packet — no respawn-station/fee yet. */
            uint8_t pid = data[1];
            float px = read_f32_le(&data[2]);
            float py = read_f32_le(&data[6]);
            float vx = read_f32_le(&data[10]);
            float vy = read_f32_le(&data[14]);
            float ang = read_f32_le(&data[18]);
            float ore = read_f32_le(&data[22]);
            float earned = read_f32_le(&data[26]);
            float spent = read_f32_le(&data[30]);
            int asteroids = (int)read_f32_le(&data[34]);
            net_state.callbacks.on_death(pid, px, py, vx, vy, ang,
                                         ore, earned, spent, asteroids, 0, 0.0f);
        } else if (len >= 2 && net_state.callbacks.on_death) {
            /* Very-legacy short packet — position-less. */
            net_state.callbacks.on_death(data[1], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f);
        }
        break;

    case NET_MSG_WORLD_TIME:
        if (len >= 5 && net_state.callbacks.on_world_time) {
            float server_time = read_f32_le(&data[1]);
            net_state.callbacks.on_world_time(server_time);
        }
        break;

    case NET_MSG_FRACTURE_CHALLENGE:
        if (len >= FRACTURE_CHALLENGE_SIZE) {
            mining_client_claim_t claim = {0};
            uint32_t fracture_id = read_u32_le(&data[1]);
            uint32_t deadline_ms = read_u32_le(&data[37]);
            uint16_t burst_cap = read_u16_le(&data[41]);
            /* Server rebroadcasts challenges every 100ms while the
             * window is open so late joiners can race. Skip the work
             * if we already searched this fracture_id — the sha256
             * burst would be redundant and would re-send our claim. */
            const mining_client_t *mc = mining_client_get();
            if (mc->fracture_search_id == fracture_id) break;
            if (mining_client_search_fracture(fracture_id, &data[5], deadline_ms,
                                              burst_cap, &claim)) {
                send_fracture_claim(claim.fracture_id, claim.burst_nonce,
                                    claim.claimed_grade);
            }
        }
        break;

    case NET_MSG_FRACTURE_RESOLVED:
        if (len >= FRACTURE_RESOLVED_SIZE) {
            mining_client_resolve_fracture(read_u32_le(&data[1]),
                                           (mining_grade_t)data[69]);
        }
        break;

    case NET_MSG_CONTRACTS:
        if (len >= 2 && net_state.callbacks.on_contracts) {
            uint8_t count = data[1];
            if (len >= 2 + count * CONTRACT_RECORD_SIZE) {
                contract_t contracts[MAX_CONTRACTS];
                memset(contracts, 0, sizeof(contracts));
                int n = count < MAX_CONTRACTS ? count : MAX_CONTRACTS;
                for (int i = 0; i < n; i++) {
                    const uint8_t *p = &data[2 + i * CONTRACT_RECORD_SIZE];
                    net_decode_contract_base(&contracts[i], p);
                    memcpy(contracts[i].required_parent, &p[32], 32);
                    contracts[i].forbidden_origin_mask = read_u64_le(&p[64]);
                    memcpy(contracts[i].target_pub, &p[72], 32);
                    net_finish_contract_decode(&contracts[i]);
                }
                net_state.callbacks.on_contracts(contracts, n);
            }
        }
        break;

    case NET_MSG_CONTRACTS_Q:
        if (len >= CONTRACT_Q_HEADER_SIZE && net_state.callbacks.on_contracts) {
            uint8_t count = data[1];
            contract_t contracts[MAX_CONTRACTS];
            memset(contracts, 0, sizeof(contracts));
            int n = count < MAX_CONTRACTS ? count : MAX_CONTRACTS;
            int off = CONTRACT_Q_HEADER_SIZE;
            bool ok = true;
            for (int i = 0; i < count; i++) {
                if (off >= len) {
                    ok = false;
                    break;
                }
                uint8_t flags = data[off++];
                if ((flags &
                     (uint8_t)(UINT8_MAX ^ CONTRACT_Q_FLAG_MASK)) != 0 ||
                    off + CONTRACT_Q_BASE_SIZE > len) {
                    ok = false;
                    break;
                }

                contract_t *ct = i < n ? &contracts[i] : NULL;
                if (ct) net_decode_contract_base(ct, &data[off]);
                off += CONTRACT_Q_BASE_SIZE;

                if (flags & CONTRACT_Q_FLAG_PARENT) {
                    if (off + 32 > len) {
                        ok = false;
                        break;
                    }
                    if (ct) memcpy(ct->required_parent, &data[off], 32);
                    off += 32;
                }
                if (flags & CONTRACT_Q_FLAG_ORIGIN_MASK) {
                    if (off + 8 > len) {
                        ok = false;
                        break;
                    }
                    if (ct) ct->forbidden_origin_mask = read_u64_le(&data[off]);
                    off += 8;
                }
                if (flags & CONTRACT_Q_FLAG_TARGET_PUB) {
                    if (off + 32 > len) {
                        ok = false;
                        break;
                    }
                    if (ct) memcpy(ct->target_pub, &data[off], 32);
                    off += 32;
                }
                if (ct) net_finish_contract_decode(ct);
            }
            if (ok) net_state.callbacks.on_contracts(contracts, n);
        }
        break;

    case NET_MSG_DELIVERY_LEDGER:
        if (len >= DELIVERY_LEDGER_HEADER && net_state.callbacks.on_delivery_ledger) {
            uint8_t count = data[1];
            if (len >= DELIVERY_LEDGER_HEADER +
                       count * DELIVERY_LEDGER_RECORD_SIZE) {
                NetDeliveryLedgerEntry entries[DELIVERY_LEDGER_MAX_RECORDS];
                memset(entries, 0, sizeof(entries));
                int n = count < DELIVERY_LEDGER_MAX_RECORDS
                    ? count : DELIVERY_LEDGER_MAX_RECORDS;
                for (int i = 0; i < n; i++) {
                    const uint8_t *p = &data[DELIVERY_LEDGER_HEADER +
                                             i * DELIVERY_LEDGER_RECORD_SIZE];
                    entries[i].shipment_id = read_u16_le(&p[0]);
                    entries[i].status = p[2];
                    entries[i].origin_station = p[3];
                    entries[i].destination_station = p[4];
                    entries[i].contract_index = p[5];
                    entries[i].commodity = p[6];
                    entries[i].quantity_total = read_u16_le(&p[7]);
                    entries[i].quantity_delivered = read_u16_le(&p[9]);
                    entries[i].quantity_bound = read_u16_le(&p[11]);
                    entries[i].debt_principal = read_f32_le(&p[13]);
                    entries[i].destination_payout = read_f32_le(&p[17]);
                    entries[i].origin_completion_credit = read_f32_le(&p[21]);
                    entries[i].due_tick = read_u32_le(&p[25]);
                    entries[i].held_bound = read_u16_le(&p[29]);
                }
                net_state.callbacks.on_delivery_ledger(entries, n);
            }
        }
        break;

    case NET_MSG_PLAYER_KNOWN_LEDGER:
        if (len >= PLAYER_KNOWN_LEDGER_HEADER &&
            net_state.callbacks.on_player_known_ledger) {
            uint8_t count = data[1];
            if (len >= PLAYER_KNOWN_LEDGER_HEADER +
                       count * PLAYER_KNOWN_LEDGER_RECORD_SIZE) {
                NetKnownLedgerEntry entries[PLAYER_KNOWN_LEDGER_MAX_RECORDS];
                memset(entries, 0, sizeof(entries));
                int n = count < PLAYER_KNOWN_LEDGER_MAX_RECORDS
                    ? count : PLAYER_KNOWN_LEDGER_MAX_RECORDS;
                for (int i = 0; i < n; i++) {
                    const uint8_t *p = &data[PLAYER_KNOWN_LEDGER_HEADER +
                                             i * PLAYER_KNOWN_LEDGER_RECORD_SIZE];
                    entries[i].station = p[0];
                    entries[i].balance = read_f32_le(&p[1]);
                }
                net_state.callbacks.on_player_known_ledger(entries, n);
            }
        }
        break;

    case NET_MSG_PLAYER_MARKET_MEMORIES:
        if (len >= PLAYER_MARKET_MEMORIES_HEADER &&
            net_state.callbacks.on_player_market_memories) {
            uint8_t count = data[1];
            if (len >= PLAYER_MARKET_MEMORIES_HEADER +
                       count * PLAYER_MARKET_MEMORY_RECORD_SIZE) {
                NetMarketMemoryEntry entries[
                    PLAYER_MARKET_MEMORY_MAX_RECORDS];
                memset(entries, 0, sizeof(entries));
                int n = count < PLAYER_MARKET_MEMORY_MAX_RECORDS
                    ? count : PLAYER_MARKET_MEMORY_MAX_RECORDS;
                for (int i = 0; i < n; i++) {
                    const uint8_t *p = &data[PLAYER_MARKET_MEMORIES_HEADER +
                                             i * PLAYER_MARKET_MEMORY_RECORD_SIZE];
                    market_memory_t *memory = &entries[i].memory;
                    memory->active = true;
                    memory->memory_kind = p[0];
                    memory->station_a = p[1];
                    memory->station_b = p[2];
                    memory->commodity = p[3];
                    memory->action = p[4];
                    memory->confidence = p[5];
                    memory->salience = p[6];
                    entries[i].hops = p[7];
                    memory->quantity_hint = read_u16_le(&p[8]);
                    memory->value_hint = read_u16_le(&p[10]);
                    memory->observed_tick = read_u32_le(&p[12]);
                    memory->subject_nonce = read_u64_le(&p[16]);
                }
                net_state.callbacks.on_player_market_memories(entries, n);
            }
        }
        break;

    case NET_MSG_PLAYER_KNOWN_CONTRACTS:
        if (len >= 5 && net_state.callbacks.on_player_known_contracts) {
            uint32_t mask = read_u32_le(&data[1]);
            net_state.callbacks.on_player_known_contracts(mask);
        }
        break;

    case NET_MSG_LEGACY_SAVES_AVAILABLE:
        /* Layer A.4 of #479 — server reports legacy saves the player
         * could claim. For now we just log; a docked-UI integration is
         * a follow-up issue. Operators can trigger
         * net_send_claim_legacy_save() manually for a stranded player. */
        if (len >= LEGACY_SAVES_HEADER) {
            int count = data[1];
            int max = (len - LEGACY_SAVES_HEADER) / LEGACY_SAVES_PREFIX_LEN;
            if (count > max) count = max;
            if (count > LEGACY_SAVES_MAX_LIST) count = LEGACY_SAVES_MAX_LIST;
            printf("[net] %d legacy save(s) available — import via "
                   "net_send_claim_legacy_save():\n", count);
            for (int i = 0; i < count; i++) {
                char prefix[LEGACY_SAVES_PREFIX_LEN + 1];
                memcpy(prefix,
                       &data[LEGACY_SAVES_HEADER + i * LEGACY_SAVES_PREFIX_LEN],
                       LEGACY_SAVES_PREFIX_LEN);
                prefix[LEGACY_SAVES_PREFIX_LEN] = '\0';
                printf("[net]   [%d] %s...\n", i, prefix);
            }
            /* TODO(#479-A.5): surface this in the docked HUD as a one-tap
             * import prompt. Today the operator drives the claim. */
        }
        break;

    default:
        break;
    }
}

/* ========================================================================= */
/* Platform-specific implementations                                        */
/* ========================================================================= */

#ifdef __EMSCRIPTEN__

/* ========================================================================= */
/* WASM implementation using emscripten WebSocket API                        */
/* ========================================================================= */

#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

static EMSCRIPTEN_WEBSOCKET_T ws_socket = 0;
static bool wasm_use_webrtc = false;

/* emscripten_websocket_send_binary currently reports success immediately
 * after calling WebSocket.send() and does not catch a send exception. Keep the
 * admission boundary explicit so auth state advances only when an OPEN socket
 * accepted the bytes without throwing. */
EM_JS(int, signal_websocket_send_checked_js,
      (EMSCRIPTEN_WEBSOCKET_T socket_id, const uint8_t *data, int len), {
    const socket = WS.getSocket(socket_id);
    if (!socket || socket.readyState !== WebSocket.OPEN) return 0;
    // Browser smoke fault injection is intentionally scoped to an explicit
    // test global plus ?smoke=1. It can only reject this page's own auth write.
    const smokeFault =
        globalThis.SIGNAL_TEST_REJECT_AUTH_PROOF_SEND === true &&
        globalThis.location &&
        new URLSearchParams(globalThis.location.search).has('smoke') &&
        len > 0 && HEAPU8[data] === 0x3f;
    if (smokeFault) {
        globalThis.SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES =
            (globalThis.SIGNAL_TEST_AUTH_PROOF_SEND_FAILURES || 0) + 1;
        return 0;
    }
    try {
        socket.send(HEAPU8.slice(data, data + len));
        return 1;
    } catch (e) {
        console.error('[net/websocket] send rejected', e);
        return 0;
    }
})

EM_JS(void, signal_webrtc_close_js, (), {
    const t = Module.signalWebRTCTransport;
    if (!t) return;
    if (t.dc) t.dc.close();
    if (t.pc) t.pc.close();
    if (t.ws) t.ws.close();
    Module.signalWebRTCTransport = null;
})

EMSCRIPTEN_KEEPALIVE void signal_net_transport_open(void) {
    if (!transport_connected("webrtc datachannel")) {
        transport_error("WebRTC authentication bootstrap");
        /* A live data channel with authentication disabled is ambiguous to
         * both peers. Tear down the rendezvous and transport immediately so
         * failure is final and observable. */
        signal_webrtc_close_js();
    }
}

EMSCRIPTEN_KEEPALIVE void signal_net_transport_message(uintptr_t ptr, int len) {
    if (!ptr || len <= 0) return;
    transport_message((const uint8_t *)ptr, len);
}

EMSCRIPTEN_KEEPALIVE void signal_net_transport_close(void) {
    transport_disconnected("webrtc datachannel");
}

EMSCRIPTEN_KEEPALIVE void signal_net_transport_error(void) {
    transport_error("webrtc datachannel");
}

EM_JS(int, signal_webrtc_connect_js, (const char *url_ptr), {
    const rawUrl = UTF8ToString(url_ptr);
    // This peer id is rendezvous routing metadata, not an authentication
    // credential. Identity, session tokens, and challenges are generated by
    // the checked C crypto wrapper. Still refuse WebRTC setup rather than
    // silently falling back to a predictable peer id if WebCrypto fails.
    function randomId() {
        const source = globalThis.crypto;
        if (!source) return null;
        if (typeof source.randomUUID === 'function') {
            try {
                return source.randomUUID();
            } catch (e) {}
        }
        if (typeof source.getRandomValues !== 'function') return null;
        try {
            const a = new Uint8Array(16);
            source.getRandomValues(a);
            return Array.from(
                a, x => x.toString(16).padStart(2, '0')).join("");
        } catch (e) {
            return null;
        }
    }
    function signalingUrl(raw) {
        if (raw.startsWith('rtc://')) return 'ws://' + raw.slice('rtc://'.length);
        if (raw.startsWith('rtcs://')) return 'wss://' + raw.slice('rtcs://'.length);
        if (raw.startsWith('webrtc+ws://')) return 'ws://' + raw.slice('webrtc+ws://'.length);
        if (raw.startsWith('webrtc+wss://')) return 'wss://' + raw.slice('webrtc+wss://'.length);
        return raw;
    }
    const sigUrl = signalingUrl(rawUrl);
    let parsed;
    try {
        parsed = new URL(sigUrl, globalThis.location ? location.href : undefined);
    } catch (e) {
        console.error('[net/webrtc] bad rendezvous url', rawUrl, e);
        return 0;
    }
    const roomFromQuery = parsed.searchParams.get('room');
    const roomFromPath = parsed.pathname && parsed.pathname !== '/'
        ? parsed.pathname.replace(new RegExp("^/+"), "")
        : "";
    const room = roomFromQuery || roomFromPath || 'signal-main';
    let nodeId = null;
    try {
        nodeId = localStorage.getItem('signal_node_id');
    } catch (e) {}
    if (!nodeId) {
        nodeId = randomId();
        if (!nodeId) {
            console.error(
                '[net/webrtc] secure rendezvous peer id unavailable');
            return 0;
        }
        try {
            localStorage.setItem('signal_node_id', nodeId);
        } catch (e) {
            // An ephemeral routing id is sufficient; it is not auth state.
        }
    }

    const state = {
        ws: null,
        pc: null,
        dc: null,
        room,
        nodeId,
        remotePeer: null,
        opened: false
    };
    Module.signalWebRTCTransport = state;

    function sendSignal(to, data) {
        if (!state.ws || state.ws.readyState !== WebSocket.OPEN) return;
        state.ws.send(JSON.stringify({ type: 'signal', room, from: nodeId, to, data }));
    }

    function attachDataChannel(dc) {
        state.dc = dc;
        dc.binaryType = 'arraybuffer';
        dc.onopen = () => {
            if (!state.opened) {
                state.opened = true;
                Module.ccall('signal_net_transport_open');
            }
        };
        dc.onclose = () => {
            state.opened = false;
            Module.ccall('signal_net_transport_close');
        };
        dc.onerror = () => Module.ccall('signal_net_transport_error');
        dc.onmessage = (ev) => {
            let bytes;
            if (ev.data instanceof ArrayBuffer) {
                bytes = new Uint8Array(ev.data);
            } else if (ev.data && ev.data.arrayBuffer) {
                ev.data.arrayBuffer().then((buf) => dc.onmessage({ data: buf }));
                return;
            } else {
                return;
            }
            const ptr = Module._malloc(bytes.length);
            HEAPU8.set(bytes, ptr);
            Module.ccall('signal_net_transport_message', null,
                         ['number', 'number'], [ptr, bytes.length]);
            Module._free(ptr);
        };
    }

    function ensurePeer(peerId, initiator) {
        if (state.pc) return state.pc;
        state.remotePeer = peerId;
        const pc = new RTCPeerConnection({
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }]
        });
        state.pc = pc;
        pc.onicecandidate = (ev) => {
            if (ev.candidate) sendSignal(peerId, {
                type: 'candidate',
                candidate: ev.candidate
            });
        };
        pc.onconnectionstatechange = () => {
            if (pc.connectionState === 'failed' ||
                pc.connectionState === 'disconnected' ||
                pc.connectionState === 'closed') {
                state.opened = false;
                Module.ccall('signal_net_transport_close');
            }
        };
        pc.ondatachannel = (ev) => attachDataChannel(ev.channel);
        if (initiator) attachDataChannel(pc.createDataChannel('signal', { ordered: true }));
        return pc;
    }

    async function createOffer(peerId) {
        const pc = ensurePeer(peerId, true);
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        sendSignal(peerId, { type: 'offer', sdp: pc.localDescription });
    }

    async function handleSignal(msg) {
        const data = msg.data || {};
        const peerId = msg.from;
        if (!peerId || peerId === nodeId) return;
        const pc = ensurePeer(peerId, false);
        if (data.type === 'offer') {
            await pc.setRemoteDescription(data.sdp);
            const answer = await pc.createAnswer();
            await pc.setLocalDescription(answer);
            sendSignal(peerId, { type: 'answer', sdp: pc.localDescription });
        } else if (data.type === 'answer') {
            await pc.setRemoteDescription(data.sdp);
        } else if (data.type === 'candidate' && data.candidate) {
            try {
                await pc.addIceCandidate(data.candidate);
            } catch (e) {
                console.warn('[net/webrtc] ICE candidate rejected', e);
            }
        }
    }

    const ws = new WebSocket(sigUrl);
    state.ws = ws;
    ws.onopen = () => ws.send(JSON.stringify({ type: 'join', room, peer: nodeId }));
    ws.onerror = () => Module.ccall('signal_net_transport_error');
    ws.onclose = () => {
        state.opened = false;
        Module.ccall('signal_net_transport_close');
    };
    ws.onmessage = (ev) => {
        let msg;
        try { msg = JSON.parse(ev.data); } catch (_) { return; }
        if (msg.type === 'peers' && Array.isArray(msg.peers) && msg.peers.length > 0) {
            createOffer(msg.peers[0]).catch((e) => {
                console.error('[net/webrtc] offer failed', e);
                Module.ccall('signal_net_transport_error');
            });
        } else if (msg.type === 'signal') {
            handleSignal(msg).catch((e) => {
                console.error('[net/webrtc] signal failed', e);
                Module.ccall('signal_net_transport_error');
            });
        }
    };
    console.log('[net/webrtc] rendezvous', sigUrl, 'room', room, 'peer', nodeId);
    return 1;
})

EM_JS(int, signal_webrtc_send_js, (const uint8_t *data, int len), {
    const t = Module.signalWebRTCTransport;
    if (!t || !t.dc || t.dc.readyState !== 'open') return 0;
    const bytes = HEAPU8.slice(data, data + len);
    try {
        t.dc.send(bytes);
        return 1;
    } catch (e) {
        console.error('[net/webrtc] datachannel send rejected', e);
        return 0;
    }
})

static EM_BOOL on_ws_open(int eventType, const EmscriptenWebSocketOpenEvent* event, void* userData) {
    (void)eventType; (void)event; (void)userData;
    if (!transport_connected("websocket relay") && ws_socket > 0)
        emscripten_websocket_close(ws_socket, 1011,
                                   "secure entropy unavailable");
    return EM_TRUE;
}

static EM_BOOL on_ws_message(int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData) {
    (void)eventType; (void)userData;
    if (event->isText) return EM_TRUE;
    transport_message((const uint8_t*)event->data, (int)event->numBytes);
    return EM_TRUE;
}

static EM_BOOL on_ws_error(int eventType, const EmscriptenWebSocketErrorEvent* event, void* userData) {
    (void)eventType; (void)event; (void)userData;
    transport_error("websocket");
    return EM_TRUE;
}

static EM_BOOL on_ws_close(int eventType, const EmscriptenWebSocketCloseEvent* event, void* userData) {
    (void)eventType; (void)event; (void)userData;
    transport_disconnected("websocket relay");
    ws_socket = 0;
    return EM_TRUE;
}

bool net_init(const char* url, const NetCallbacks* callbacks) {
    /* Preserve identity fields across the reset — main.c installs the
     * pubkey + secret BEFORE net_init so the first wire SESSION packet
     * carries the pubkey-derived alphanumeric callsign. A blanket memset
     * would zero them and the on-connect handshake would send an empty
     * callsign (server stores ""; HUD shows "SHIP"; highscores blank). */
    uint8_t saved_pubkey[32];
    uint8_t saved_secret[64];
    bool    saved_pub_ready    = net_state.identity_pubkey_ready;
    bool    saved_secret_ready = net_state.identity_secret_ready;
    memcpy(saved_pubkey, net_state.identity_pubkey, sizeof(saved_pubkey));
    memcpy(saved_secret, net_state.identity_secret, sizeof(saved_secret));

    memset(&net_state, 0, sizeof(net_state));
    net_loopback_active = false;
    net_state.local_id = 0xFF;
    if (callbacks) net_state.callbacks = *callbacks;

    memcpy(net_state.identity_pubkey, saved_pubkey, sizeof(saved_pubkey));
    memcpy(net_state.identity_secret, saved_secret, sizeof(saved_secret));
    net_state.identity_pubkey_ready = saved_pub_ready;
    net_state.identity_secret_ready = saved_secret_ready;

    if (!url || url[0] == '\0') {
        printf("[net] no server URL provided, multiplayer disabled\n");
        return false;
    }
    if (!net_state.identity_pubkey_ready ||
        !net_state.identity_secret_ready ||
        !ensure_session_token()) {
        memset(net_state.session_token, 0,
               sizeof(net_state.session_token));
        net_state.session_token_ready = false;
        fprintf(stderr,
                "[net] secure authentication bootstrap unavailable\n");
        return false;
    }
    snprintf(net_state.server_url, sizeof(net_state.server_url), "%s", url);
    wasm_use_webrtc = (strncmp(url, "rtc://", 6) == 0 ||
                       strncmp(url, "rtcs://", 7) == 0 ||
                       strncmp(url, "webrtc+ws://", 13) == 0 ||
                       strncmp(url, "webrtc+wss://", 14) == 0);
    if (wasm_use_webrtc) {
        if (!signal_webrtc_connect_js(url)) {
            printf("[net] failed to start WebRTC rendezvous transport\n");
            wasm_use_webrtc = false;
            return false;
        }
        printf("[net] connecting via WebRTC rendezvous %s\n", url);
        return true;
    }
    if (!emscripten_websocket_is_supported()) {
        printf("[net] WebSocket not supported in this browser\n");
        return false;
    }

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url;
    attr.protocols = NULL;
    attr.createOnMainThread = EM_TRUE;

    ws_socket = emscripten_websocket_new(&attr);
    if (ws_socket <= 0) {
        printf("[net] failed to create WebSocket\n");
        return false;
    }

    emscripten_websocket_set_onopen_callback(ws_socket, NULL, on_ws_open);
    emscripten_websocket_set_onmessage_callback(ws_socket, NULL, on_ws_message);
    emscripten_websocket_set_onerror_callback(ws_socket, NULL, on_ws_error);
    emscripten_websocket_set_onclose_callback(ws_socket, NULL, on_ws_close);

    printf("[net] connecting to %s\n", url);
    return true;
}

bool net_reconnect(void) {
    if (net_state.server_url[0] == '\0') return false;
    if (wasm_use_webrtc) {
        signal_webrtc_close_js();
        net_state.connected = false;
        net_state.local_id = 0xFF;
        net_state.server_hash[0] = '\0';
        net_state.protocol_info_ready = false;
        memset(net_state.players, 0, sizeof(net_state.players));
        printf("[net] reconnecting via WebRTC rendezvous %s\n", net_state.server_url);
        return signal_webrtc_connect_js(net_state.server_url) != 0;
    }
    if (ws_socket > 0) {
        emscripten_websocket_delete(ws_socket);
        ws_socket = 0;
    }
    /* Preserve callbacks and session token, reset connection state */
    net_state.connected = false;
    net_state.local_id = 0xFF;
    net_state.server_hash[0] = '\0';
    net_state.protocol_info_ready = false;
    memset(net_state.players, 0, sizeof(net_state.players));

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = net_state.server_url;
    attr.protocols = NULL;
    attr.createOnMainThread = EM_TRUE;

    ws_socket = emscripten_websocket_new(&attr);
    if (ws_socket <= 0) {
        printf("[net] reconnect failed\n");
        return false;
    }
    emscripten_websocket_set_onopen_callback(ws_socket, NULL, on_ws_open);
    emscripten_websocket_set_onmessage_callback(ws_socket, NULL, on_ws_message);
    emscripten_websocket_set_onerror_callback(ws_socket, NULL, on_ws_error);
    emscripten_websocket_set_onclose_callback(ws_socket, NULL, on_ws_close);
    printf("[net] reconnecting to %s\n", net_state.server_url);
    return true;
}

void net_shutdown(void) {
    net_loopback_active = false;
    clear_pubkey_challenge_state(true);
    if (wasm_use_webrtc) {
        signal_webrtc_close_js();
        net_state.connected = false;
        wasm_use_webrtc = false;
        return;
    }
    if (ws_socket > 0) {
        emscripten_websocket_close(ws_socket, 1000, "shutdown");
        emscripten_websocket_delete(ws_socket);
        ws_socket = 0;
    }
    net_state.connected = false;
}

static bool ws_send_binary(const uint8_t* data, int len) {
    if (!data || len <= 0) return false;
    if (net_loopback_active) {
        return net_loopback_send
            ? net_loopback_send(data, len, net_loopback_user)
            : false;
    }
    if (wasm_use_webrtc) {
        if (!net_state.connected) return false;
        return signal_webrtc_send_js(data, len) != 0;
    }
    if (!net_state.connected || ws_socket <= 0) return false;
    return signal_websocket_send_checked_js(
               ws_socket, data, len) != 0;
}

static void ws_close_authentication_failure(void) {
    net_auth_transport_closes++;
    if (net_loopback_active) {
        net_loopback_active = false;
        transport_disconnected("local authentication failure");
        return;
    }
    if (wasm_use_webrtc) {
        signal_webrtc_close_js();
        transport_disconnected("WebRTC authentication failure");
        return;
    }
    if (ws_socket > 0)
        emscripten_websocket_close(
            ws_socket, 1011, "authentication send rejected");
    transport_disconnected("websocket authentication failure");
}

void net_poll(void) {
    /* Emscripten WebSocket callbacks fire on the main thread automatically. */
}

#else

/* ========================================================================= */
/* Native implementation using mongoose WebSocket client                     */
/* ========================================================================= */

#include "mongoose.h"

static struct mg_mgr net_mgr;
static struct mg_connection *ws_conn = NULL;
static bool mgr_initialized = false;

static bool ws_send_binary(const uint8_t* data, int len) {
    if (!data || len <= 0) return false;
    if (net_loopback_active) {
        return net_loopback_send
            ? net_loopback_send(data, len, net_loopback_user)
            : false;
    }
    if (!net_state.connected || !ws_conn ||
        ws_conn->is_closing || ws_conn->is_draining) {
        return false;
    }
    return mg_ws_send(
               ws_conn, data, (size_t)len, WEBSOCKET_OP_BINARY) != 0;
}

static void ws_close_authentication_failure(void) {
    net_auth_transport_closes++;
    if (net_loopback_active) {
        net_loopback_active = false;
        transport_disconnected("local authentication failure");
        return;
    }
    if (ws_conn) ws_conn->is_closing = 1;
    transport_disconnected("websocket authentication failure");
}

static void net_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_WS_OPEN) {
        ws_conn = c;
        if (!transport_connected("websocket server")) {
            mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
            c->is_closing = 1;
        }
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        transport_message((const uint8_t *)wm->data.buf, (int)wm->data.len);
    } else if (ev == MG_EV_ERROR) {
        printf("[net] connection error: %s\n", (char *)ev_data);
        net_state.connected = false;
        clear_pubkey_challenge_state(true);
        ws_conn = NULL;
    } else if (ev == MG_EV_CLOSE) {
        transport_disconnected("websocket server");
        ws_conn = NULL;
    }
}

bool net_init(const char* url, const NetCallbacks* callbacks) {
    /* Preserve identity fields across the reset — main.c installs the
     * pubkey + secret BEFORE net_init so the first wire SESSION packet
     * carries the pubkey-derived alphanumeric callsign. A blanket memset
     * would zero them and the on-connect handshake would send an empty
     * callsign (server stores ""; HUD shows "SHIP"; highscores blank). */
    uint8_t saved_pubkey[32];
    uint8_t saved_secret[64];
    bool    saved_pub_ready    = net_state.identity_pubkey_ready;
    bool    saved_secret_ready = net_state.identity_secret_ready;
    memcpy(saved_pubkey, net_state.identity_pubkey, sizeof(saved_pubkey));
    memcpy(saved_secret, net_state.identity_secret, sizeof(saved_secret));

    memset(&net_state, 0, sizeof(net_state));
    net_loopback_active = false;
    net_state.local_id = 0xFF;
    if (callbacks) net_state.callbacks = *callbacks;

    memcpy(net_state.identity_pubkey, saved_pubkey, sizeof(saved_pubkey));
    memcpy(net_state.identity_secret, saved_secret, sizeof(saved_secret));
    net_state.identity_pubkey_ready = saved_pub_ready;
    net_state.identity_secret_ready = saved_secret_ready;

    if (!url || url[0] == '\0') {
        printf("[net] no server URL provided, multiplayer disabled\n");
        return false;
    }
    if (!net_state.identity_pubkey_ready ||
        !net_state.identity_secret_ready ||
        !ensure_session_token()) {
        memset(net_state.session_token, 0,
               sizeof(net_state.session_token));
        net_state.session_token_ready = false;
        fprintf(stderr,
                "[net] secure authentication bootstrap unavailable\n");
        return false;
    }
    if (strncmp(url, "rtc://", 6) == 0 ||
        strncmp(url, "rtcs://", 7) == 0 ||
        strncmp(url, "webrtc+ws://", 13) == 0 ||
        strncmp(url, "webrtc+wss://", 14) == 0) {
        printf("[net] WebRTC transport is only available in browser builds\n");
        return false;
    }
    snprintf(net_state.server_url, sizeof(net_state.server_url), "%s", url);

    mg_mgr_init(&net_mgr);
    mgr_initialized = true;

    struct mg_connection *c = mg_ws_connect(&net_mgr, url, net_ev_handler, NULL, NULL);
    if (!c) {
        printf("[net] failed to connect to %s\n", url);
        mg_mgr_free(&net_mgr);
        mgr_initialized = false;
        return false;
    }

    printf("[net] connecting to %s\n", url);
    return true;
}

void net_shutdown(void) {
    net_loopback_active = false;
    clear_pubkey_challenge_state(true);
    if (ws_conn) {
        mg_ws_send(ws_conn, "", 0, WEBSOCKET_OP_CLOSE);
        ws_conn = NULL;
    }
    if (mgr_initialized) {
        mg_mgr_free(&net_mgr);
        mgr_initialized = false;
    }
    net_state.connected = false;
}

bool net_reconnect(void) {
    /* Native: reconnect via mongoose */
    if (net_state.server_url[0] == '\0') return false;
    if (ws_conn) { ws_conn->is_closing = 1; ws_conn = NULL; }
    net_state.connected = false;
    net_state.local_id = 0xFF;
    net_state.server_hash[0] = '\0';
    net_state.protocol_info_ready = false;
    memset(net_state.players, 0, sizeof(net_state.players));
    ws_conn = mg_ws_connect(&net_mgr, net_state.server_url, net_ev_handler, NULL, NULL);
    printf("[net] reconnecting to %s\n", net_state.server_url);
    return ws_conn != NULL;
}

void net_poll(void) {
    if (mgr_initialized) {
        mg_mgr_poll(&net_mgr, 0);  /* non-blocking */
    }
}

#endif /* __EMSCRIPTEN__ */

uint32_t net_send_input(uint8_t flags, uint8_t action, uint16_t input_seq,
                        uint16_t mining_target,
                        uint8_t buy_grade, int8_t place_station,
                        int8_t place_ring, int8_t place_slot,
                        uint16_t action_id, uint32_t input_tick) {
    uint8_t buf[NET_INPUT_MSG_SIZE];
    uint32_t sent_ms = net_now_ms32();
    buf[0] = NET_MSG_INPUT;
    buf[1] = flags;
    buf[2] = action;
    buf[3] = (mining_target == 0xFFFFu) ? 0xFFu : (uint8_t)(mining_target & 0xFFu);
    buf[4] = buy_grade;
    /* int8 -> uint8 round-trip preserves sentinel -1 (=> 0xFF). */
    buf[5] = (uint8_t)place_station;
    buf[6] = (uint8_t)place_ring;
    buf[7] = (uint8_t)place_slot;
    buf[8] = (uint8_t)(input_seq & 0xFFu);
    buf[9] = (uint8_t)(input_seq >> 8);
    buf[10] = (uint8_t)(mining_target & 0xFFu);
    buf[11] = (uint8_t)(mining_target >> 8);
    buf[12] = (uint8_t)(action_id & 0xFFu);
    buf[13] = (uint8_t)(action_id >> 8);
    write_u32_le(&buf[14], input_tick);
    write_u32_le(&buf[18], sent_ms);
    (void)ws_send_binary(buf, NET_INPUT_MSG_SIZE);
    return sent_ms;
}

bool net_send_plan(uint8_t op, int8_t station, int8_t ring, int8_t slot,
                   uint8_t module_type, float px, float py) {
    uint8_t buf[NET_PLAN_MSG_SIZE];
    buf[0] = NET_MSG_PLAN;
    buf[1] = op;
    buf[2] = (uint8_t)station;
    buf[3] = (uint8_t)ring;
    buf[4] = (uint8_t)slot;
    buf[5] = module_type;
    write_f32_le(&buf[6], px);
    write_f32_le(&buf[10], py);
    if (net_has_identity_pubkey() && !net_has_identity_secret()) {
        fprintf(stderr,
                "[net-plan] blocked op=%u: identity-backed client missing signing secret\n",
                (unsigned)op);
        return false;
    }
    if (net_send_signed_action(SIGNED_ACTION_PLAN,
                               &buf[1], NET_PLAN_MSG_SIZE - 1)) {
        return true;
    }
    if (net_has_identity_pubkey()) {
        fprintf(stderr,
                "[net-plan] blocked op=%u: signed plan path rejected\n",
                (unsigned)op);
        return false;
    }
    return ws_send_binary(buf, NET_PLAN_MSG_SIZE);
}

/* ---------- Common accessors --------------------------------------------- */

bool net_is_connected(void) {
    return net_state.connected;
}

uint8_t net_local_id(void) {
    return net_state.local_id;
}

const char* net_local_callsign(void) {
    ensure_callsign();
    return net_state.callsign;
}

const NetPlayerState* net_get_players(void) {
    return net_state.players;
}

const char* net_server_hash(void) {
    return net_state.server_hash;
}

const NetProtocolInfo *net_protocol_info(void) {
    return net_state.protocol_info_ready ? &net_state.protocol_info : NULL;
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE uint32_t signal_debug_auth_transport_closes(void) {
    return net_auth_transport_closes;
}
#endif
