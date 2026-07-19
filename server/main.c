/*
 * main.c -- Headless authoritative game server for Signal Space Miner.
 *
 * Uses cesanta/mongoose for WebSocket handling.  Runs the full game
 * simulation and broadcasts state to browser clients.
 */
#include "mongoose.h"
#include "game_sim.h"
#include "gossip.h"
#include "highscore.h"
#include "manifest.h"
#include "mining.h"  /* mining_render_callsign for chain log copy */
#include "net_protocol.h"
#include "signal_crypto.h"
#include "sim_ai.h"
#include "sim_asteroid.h"
#include "sim_autopilot.h"
#include "signal_intelligence.h"
#include "chain_log.h"  /* signed event emission (#479 C) */
#include "cargo_receipt_issue.h"  /* portable cargo receipts (#479 D) */
#include "commodity.h"  /* station_*_price_unit (#prefix-pricing) */
#include "fixpoint.h"
#include "handoff_flow.h"
#include "sha256.h"
#include "station_authority.h"
#include "base64.h"
#include "route_history_labels.h"
#include "station_policy.h"
#include "station_util.h"
#include <math.h>       /* lroundf */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <float.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>       /* time() for fresh-boot belt_seed rotation */

#ifdef _WIN32
#include <direct.h>
#define MKDIR_PATH(p) _mkdir(p)
#define CHDIR_PATH(p) _chdir(p)
#define PATH_IS_DIR(m) (((m) & _S_IFDIR) != 0)
#else
#include <unistd.h>
#define MKDIR_PATH(p) mkdir((p), 0755)
#define CHDIR_PATH(p) chdir(p)
#define PATH_IS_DIR(m) S_ISDIR(m)
#endif

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static world_t world;
static bool running = true;
static const char *allowed_origin = NULL;
static const char *internal_token = NULL;


static const char *persistence_data_dir = ".";
static const char *static_root_dir = NULL;
static int server_bot_player_target = 0;
static int server_bot_players_spawned = 0;
static int server_bot_brain_mode = SERVER_BRAIN_MODE_NONE;
static const char *server_bot_brain_mode_name = "autopilot";
static const char *server_bot_brain_checkpoint = NULL;
static const char *server_bot_contract_brain_checkpoint = NULL;
static const char *server_bot_npc_worker_brain_checkpoint = NULL;
static int frontier_virtual_pilot_target = 0;
static uint32_t fresh_world_seed_override = 0;
static uint32_t fresh_world_seq_override = 0;
static bool trust_proxy_headers = false;

/* Shared HTTP response headers for API endpoints */
static char api_headers[256];

/* Layer A.3 of #479 — operational counters surfaced via /health.
 *   unsigned_action_count: state-changing actions rejected on legacy
 *     unsigned mutation channels from a connection that *has* a
 *     registered pubkey. A non-zero value means at least one client is
 *     still on the pre-A.3 unsigned codepath.
 *   signed_action_count: signed actions verified + dispatched.
 *   signed_action_reject_count: signed actions dropped (any reason).
 */
static uint64_t signed_action_count = 0;
static uint64_t signed_action_reject_count = 0;
static uint64_t unsigned_action_count = 0;





static bool read_u32_env(const char *name, uint32_t *out) {
    const char *text = getenv(name);
    if (!text || text[0] == '\0') return true;
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value == 0ul || value > 4294967295ul) {
        fprintf(stderr, "[FATAL] invalid %s=%s (use 1..4294967295)\n",
                name, text);
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static int live_player_connection_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].connected && world.players[i].connection->conn) count++;
    }
    return count;
}

typedef struct {
    uint32_t magic;
    uint32_t flags;
    uint64_t client_addr_key;
} server_conn_meta_t;

#define SERVER_CONN_META_MAGIC 0x53494743u /* SIGC */
#define SERVER_CONN_META_HAS_CLIENT_ADDR 0x01u

_Static_assert(sizeof(server_conn_meta_t) <= MG_DATA_SIZE,
               "server connection metadata must fit in mg_connection.data");

static bool env_truthy(const char *value) {
    return value && value[0] != '\0' &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0 &&
           strcmp(value, "no") != 0 &&
           strcmp(value, "NO") != 0;
}

static uint64_t server_client_addr_key_bytes(const void *data, size_t len) {
    uint64_t h = net_payload_hash((const uint8_t *)data, len);
    return h ? h : 1u;
}

static uint64_t server_peer_addr_key(const struct mg_connection *c) {
    if (!c) return 1u;
    return server_client_addr_key_bytes(&c->rem.addr, sizeof(c->rem.addr));
}

static bool copy_proxy_addr_token(struct mg_str value,
                                  char *out,
                                  size_t out_size) {
    if (!out || out_size == 0) return false;
    size_t i = 0;
    while (i < value.len && isspace((unsigned char)value.buf[i])) i++;
    if (i < value.len && value.buf[i] == '"') i++;

    size_t n = 0;
    for (; i < value.len && n + 1 < out_size; i++) {
        unsigned char c = (unsigned char)value.buf[i];
        if (c == ',' || c == ';' || c == '"' || isspace(c)) break;
        if (!(isalnum(c) || c == '.' || c == ':' || c == '-' ||
              c == '_' || c == '[' || c == ']')) {
            return false;
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return n > 0;
}

static bool mg_str_at_case_eq(struct mg_str s, size_t off, const char *lit) {
    for (size_t i = 0; lit[i]; i++) {
        if (off + i >= s.len) return false;
        unsigned char a = (unsigned char)s.buf[off + i];
        unsigned char b = (unsigned char)lit[i];
        if (tolower(a) != tolower(b)) return false;
    }
    return true;
}

static bool copy_forwarded_for_token(struct mg_str value,
                                     char *out,
                                     size_t out_size) {
    for (size_t i = 0; i + 4 <= value.len; i++) {
        if (!mg_str_at_case_eq(value, i, "for=")) continue;
        return copy_proxy_addr_token(mg_str_n(value.buf + i + 4,
                                              value.len - i - 4),
                                     out, out_size);
    }
    return false;
}

static bool server_proxy_client_addr_token(struct mg_http_message *hm,
                                           char *out,
                                           size_t out_size) {
    struct mg_str *h = mg_http_get_header(hm, "Fly-Client-IP");
    if (h && copy_proxy_addr_token(*h, out, out_size)) return true;

    h = mg_http_get_header(hm, "X-Forwarded-For");
    if (h && copy_proxy_addr_token(*h, out, out_size)) return true;

    h = mg_http_get_header(hm, "Forwarded");
    if (h && copy_forwarded_for_token(*h, out, out_size)) return true;

    return false;
}

static void server_note_ws_client_addr(struct mg_connection *c,
                                       struct mg_http_message *hm) {
    if (!c) return;
    server_conn_meta_t *meta = (server_conn_meta_t *)c->data;
    memset(meta, 0, sizeof(*meta));
    meta->magic = SERVER_CONN_META_MAGIC;
    if (!trust_proxy_headers || !hm) return;

    char token[96];
    if (!server_proxy_client_addr_token(hm, token, sizeof(token))) return;
    meta->client_addr_key = server_client_addr_key_bytes(
        token, strlen(token));
    meta->flags |= SERVER_CONN_META_HAS_CLIENT_ADDR;
}

static uint64_t server_connection_limit_key(const struct mg_connection *c) {
    if (!c) return 1u;
    const server_conn_meta_t *meta = (const server_conn_meta_t *)c->data;
    if (meta->magic == SERVER_CONN_META_MAGIC &&
        (meta->flags & SERVER_CONN_META_HAS_CLIENT_ADDR) &&
        meta->client_addr_key != 0u) {
        return meta->client_addr_key;
    }
    return server_peer_addr_key(c);
}

static uint64_t server_http_client_key(const struct mg_connection *c,
                                       struct mg_http_message *hm) {
    if (trust_proxy_headers && hm) {
        char token[96];
        if (server_proxy_client_addr_token(hm, token, sizeof(token))) {
            return server_client_addr_key_bytes(token, strlen(token));
        }
    }
    return server_peer_addr_key(c);
}

/* Dirty flags: only re-broadcast station identity when something changed */
static bool station_identity_dirty[MAX_STATIONS];
static uint8_t station_hull_inventory_last[MAX_STATIONS][HULL_CLASS_COUNT];
static bool station_diag_valid[MAX_STATIONS];
static uint64_t station_diag_last_sent_ms[MAX_STATIONS];
static uint8_t station_diag_last[MAX_STATIONS][MAX_MODULES_PER_STATION];
static bool station_price_anchor_valid[MAX_STATIONS][COMMODITY_COUNT];
static uint32_t station_price_anchor_station_id[MAX_STATIONS];
static float station_price_anchor[MAX_STATIONS][COMMODITY_COUNT];
static bool station_econ_dirty = true;   /* station inventories changed */
static bool contracts_dirty = true;       /* contract list changed */
static highscore_table_t highscores;
static bool highscores_dirty = true;      /* broadcast + persist pending */
static server_world_snapshot_scratch_t world_snapshot_scratch;
static server_private_snapshot_scratch_t private_snapshot_scratch;
static server_station_snapshot_scratch_t station_snapshot_scratch;
static uint64_t net_tx_packets_total = 0;
static uint64_t net_tx_bytes_total = 0;
static uint64_t net_tx_packets_by_msg[256];
static uint64_t net_tx_bytes_by_msg[256];
static uint64_t net_tx_suppressed_packets_total = 0;
static uint64_t net_tx_suppressed_bytes_total = 0;
static uint64_t net_tx_suppressed_packets_by_msg[256];
static uint64_t net_tx_suppressed_bytes_by_msg[256];
static uint64_t net_tx_backpressure_packets_total = 0;
static uint64_t net_tx_backpressure_bytes_total = 0;
static uint64_t net_tx_backpressure_packets_by_msg[256];
static uint64_t net_tx_backpressure_bytes_by_msg[256];
static uint64_t net_tx_last_packets_total = 0;
static uint64_t net_tx_last_bytes_total = 0;
static uint64_t net_tx_last_packets_by_msg[256];
static uint64_t net_tx_last_bytes_by_msg[256];
static uint64_t net_tx_last_suppressed_packets_total = 0;
static uint64_t net_tx_last_suppressed_bytes_total = 0;
static uint64_t net_tx_last_suppressed_packets_by_msg[256];
static uint64_t net_tx_last_suppressed_bytes_by_msg[256];
static uint64_t net_tx_last_backpressure_packets_total = 0;
static uint64_t net_tx_last_backpressure_bytes_total = 0;
static uint64_t net_tx_last_backpressure_packets_by_msg[256];
static uint64_t net_tx_last_backpressure_bytes_by_msg[256];
static uint64_t net_tx_last_emf_ms = 0;

/* Defined further down; forward-declared so the highscore helpers can
 * use the same send wrapper as every other broadcast in this file
 * (consistent with future send-queue / rate-limiting changes). */
static void ws_send(struct mg_connection *c, const void *data, size_t len);

static void send_cargo_receipt_chain(struct mg_connection *c,
                                     const cargo_receipt_chain_t *chain) {
    uint8_t buf[3 + CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE];
    int len = serialize_cargo_receipt_bundle(buf, chain);
    if (c && len > 0) ws_send(c, buf, (size_t)len);
}

static void ws_cargo_receipt_chain_sink(void *user,
                                        const cargo_receipt_chain_t *chain) {
    send_cargo_receipt_chain((struct mg_connection *)user, chain);
}

static void send_handoff_ticket_msg(struct mg_connection *c, uint8_t status,
                                    uint8_t source_station,
                                    uint8_t dest_station,
                                    const handoff_ticket_t *ticket) {
    uint8_t buf[4 + HANDOFF_TICKET_SIZE];
    int len = serialize_handoff_ticket(buf, status, source_station,
                                       dest_station, ticket);
    ws_send(c, buf, (size_t)len);
}

static void send_handoff_result_msg(struct mg_connection *c, uint8_t status,
                                    uint8_t reason, uint8_t dest_station,
                                    const uint8_t ticket_hash[32]) {
    uint8_t buf[NET_HANDOFF_RESULT_SIZE];
    int len = serialize_handoff_result(buf, status, reason, dest_station,
                                       ticket_hash);
    ws_send(c, buf, (size_t)len);
}

static void ws_handoff_ticket_sink(void *user, uint8_t status,
                                   uint8_t source_station,
                                   uint8_t dest_station,
                                   const handoff_ticket_t *ticket) {
    send_handoff_ticket_msg((struct mg_connection *)user, status,
                            source_station, dest_station, ticket);
}

static void ws_handoff_result_sink(void *user, uint8_t status,
                                   uint8_t reason, uint8_t dest_station,
                                   const uint8_t ticket_hash[32]) {
    send_handoff_result_msg((struct mg_connection *)user, status, reason,
                            dest_station, ticket_hash);
}

static const char *cargo_receipt_present_result_name(
    cargo_receipt_present_result_t result) {
    switch (result) {
    case CARGO_RECEIPT_PRESENT_OK: return "ok";
    case CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS: return "bad-args";
    case CARGO_RECEIPT_PRESENT_REJECT_NO_PLAYER_KEY: return "no-player-key";
    case CARGO_RECEIPT_PRESENT_REJECT_NOT_CARRIED: return "not-carried";
    case CARGO_RECEIPT_PRESENT_REJECT_VERIFY: return "verify";
    case CARGO_RECEIPT_PRESENT_REJECT_RECIPIENT: return "recipient";
    case CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH: return "existing-mismatch";
    case CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE: return "receipt-store";
    default: return "unknown";
    }
}

#define ANALYTICS_ACTIVE_WINDOW_MS 60000ull
#define ANALYTICS_METRIC_STALE_MS 120000ull
#define ANALYTICS_EMF_INTERVAL_MS 60000ull
#define ANALYTICS_USER_KEY_LEN 24

static const char *analytics_build_hash(void) {
#ifdef GIT_HASH
    return GIT_HASH;
#else
    return "dev";
#endif
}

static uint64_t analytics_epoch_ms(void) {
    return (uint64_t)time(NULL) * 1000ull;
}

static void analytics_hash_key(const char *prefix, const uint8_t *data,
                               size_t len, char out[ANALYTICS_USER_KEY_LEN]) {
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    sha256_bytes(data, len, digest);
    out[0] = prefix[0];
    out[1] = prefix[1];
    out[2] = '_';
    for (int i = 0; i < 8; i++) {
        out[3 + i * 2] = hex[digest[i] >> 4];
        out[4 + i * 2] = hex[digest[i] & 0x0Fu];
    }
    out[19] = '\0';
}

static void analytics_user_key(const server_player_t *sp,
                               char out[ANALYTICS_USER_KEY_LEN]) {
    if (server_player_can_use_pubkey_persistence(sp)) {
        analytics_hash_key("pk", sp->pubkey, sizeof(sp->pubkey), out);
        return;
    }
    if (sp && sp->session_ready) {
        analytics_hash_key("st", sp->session_token, sizeof(sp->session_token), out);
        return;
    }
    snprintf(out, ANALYTICS_USER_KEY_LEN, "anon");
}

static void analytics_record_activity(server_player_t *sp, uint64_t now_ms) {
    if (sp) sp->connection->analytics_last_activity_ms = now_ms;
}

static void analytics_log_player_event(const char *event, int pid,
                                       const server_player_t *sp,
                                       uint64_t now_ms,
                                       uint64_t duration_ms) {
    char user_key[ANALYTICS_USER_KEY_LEN];
    analytics_user_key(sp, user_key);
    bool connected = sp && sp->connected &&
                     strcmp(event, "player_disconnect") != 0;
    printf("{\"event\":\"%s\",\"service\":\"signal-relay\",\"build\":\"%s\","
           "\"ts_epoch_ms\":%llu,\"uptime_ms\":%llu,\"user_key\":\"%s\","
           "\"player_slot\":%d,\"session_ready\":%s,\"connected\":%s,"
           "\"duration_ms\":%llu}\n",
           event,
           analytics_build_hash(),
           (unsigned long long)analytics_epoch_ms(),
           (unsigned long long)now_ms,
           user_key,
           pid,
           (sp && sp->session_ready) ? "true" : "false",
           connected ? "true" : "false",
           (unsigned long long)duration_ms);
}

static void analytics_handle_client_metrics(int pid, server_player_t *sp,
                                            const uint8_t *data, int len,
                                            uint64_t now_ms) {
    if (!sp || !data || len < NET_CLIENT_METRICS_SIZE) return;
    sp->connection->analytics_metrics_seq = read_u32_le(&data[1]);
    sp->connection->analytics_ping_ms = read_u16_le(&data[5]);
    sp->connection->analytics_ack_ms = read_u16_le(&data[7]);
    sp->connection->analytics_ack_gap_ms = read_u16_le(&data[9]);
    sp->connection->analytics_server_turnaround_ms = read_u16_le(&data[11]);
    sp->connection->analytics_player_interval_ms = read_u16_le(&data[13]);
    sp->connection->analytics_unacked_inputs = read_u16_le(&data[15]);
    sp->connection->analytics_replay_depth = read_u16_le(&data[17]);
    sp->connection->analytics_action_queue_depth = data[19];
    sp->connection->analytics_recovery_flags = data[20];
    sp->connection->analytics_metrics_last_ms = now_ms;
    sp->connection->analytics_metrics_samples++;
    analytics_record_activity(sp, now_ms);

    char user_key[ANALYTICS_USER_KEY_LEN];
    analytics_user_key(sp, user_key);
    printf("{\"event\":\"player_metrics\",\"service\":\"signal-relay\","
           "\"build\":\"%s\",\"ts_epoch_ms\":%llu,\"uptime_ms\":%llu,"
           "\"user_key\":\"%s\",\"player_slot\":%d,\"session_ready\":%s,"
           "\"seq\":%u,\"ping_ms\":%u,\"ack_ms\":%u,\"ack_gap_ms\":%u,"
           "\"server_turnaround_ms\":%u,\"player_interval_ms\":%u,"
           "\"unacked_inputs\":%u,\"replay_depth\":%u,"
           "\"action_queue_depth\":%u,\"recovery_flags\":%u,"
           "\"sample_count\":%u}\n",
           analytics_build_hash(),
           (unsigned long long)analytics_epoch_ms(),
           (unsigned long long)now_ms,
           user_key,
           pid,
           sp->session_ready ? "true" : "false",
           (unsigned)sp->connection->analytics_metrics_seq,
           (unsigned)sp->connection->analytics_ping_ms,
           (unsigned)sp->connection->analytics_ack_ms,
           (unsigned)sp->connection->analytics_ack_gap_ms,
           (unsigned)sp->connection->analytics_server_turnaround_ms,
           (unsigned)sp->connection->analytics_player_interval_ms,
           (unsigned)sp->connection->analytics_unacked_inputs,
           (unsigned)sp->connection->analytics_replay_depth,
           (unsigned)sp->connection->analytics_action_queue_depth,
           (unsigned)sp->connection->analytics_recovery_flags,
           (unsigned)sp->connection->analytics_metrics_samples);
}

static void analytics_emit_emf(uint64_t now_ms) {
    int connected = 0;
    int ready = 0;
    int active_1m = 0;
    int metric_players = 0;
    uint64_t ping_sum = 0;
    uint64_t ack_sum = 0;
    uint64_t gap_sum = 0;
    uint16_t max_gap = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *sp = &world.players[i];
        if (!sp->connected || !sp->connection->conn) continue;
        connected++;
        if (sp->session_ready) ready++;
        if (sp->connection->analytics_last_activity_ms != 0 &&
            now_ms >= sp->connection->analytics_last_activity_ms &&
            now_ms - sp->connection->analytics_last_activity_ms <= ANALYTICS_ACTIVE_WINDOW_MS) {
            active_1m++;
        }
        if (sp->connection->analytics_metrics_samples == 0 ||
            sp->connection->analytics_metrics_last_ms == 0 ||
            now_ms < sp->connection->analytics_metrics_last_ms ||
            now_ms - sp->connection->analytics_metrics_last_ms > ANALYTICS_METRIC_STALE_MS) {
            continue;
        }
        metric_players++;
        ping_sum += sp->connection->analytics_ping_ms;
        ack_sum += sp->connection->analytics_ack_ms;
        gap_sum += sp->connection->analytics_ack_gap_ms;
        if (sp->connection->analytics_ack_gap_ms > max_gap) max_gap = sp->connection->analytics_ack_gap_ms;
    }

    double avg_ping = metric_players ? (double)ping_sum / (double)metric_players : 0.0;
    double avg_ack = metric_players ? (double)ack_sum / (double)metric_players : 0.0;
    double avg_gap = metric_players ? (double)gap_sum / (double)metric_players : 0.0;

    uint64_t tx_packets = net_tx_packets_total - net_tx_last_packets_total;
    uint64_t tx_bytes = net_tx_bytes_total - net_tx_last_bytes_total;
    uint64_t tx_suppressed_packets =
        net_tx_suppressed_packets_total -
        net_tx_last_suppressed_packets_total;
    uint64_t tx_suppressed_bytes =
        net_tx_suppressed_bytes_total -
        net_tx_last_suppressed_bytes_total;
    uint64_t tx_backpressure_packets =
        net_tx_backpressure_packets_total -
        net_tx_last_backpressure_packets_total;
    uint64_t tx_backpressure_bytes =
        net_tx_backpressure_bytes_total -
        net_tx_last_backpressure_bytes_total;
    uint64_t interval_ms = (net_tx_last_emf_ms != 0 && now_ms > net_tx_last_emf_ms)
        ? now_ms - net_tx_last_emf_ms
        : ANALYTICS_EMF_INTERVAL_MS;
    double tx_bytes_per_sec = interval_ms > 0
        ? ((double)tx_bytes * 1000.0) / (double)interval_ms
        : 0.0;
    uint64_t tx_world_players_packets =
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYERS] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYERS]) +
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTION] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTION]) +
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTION_Q] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTION_Q]) +
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTIOND_Q] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTIOND_Q]) +
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYER_POSED_Q] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYER_POSED_Q]) +
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTIONM_Q] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYER_MOTIONM_Q]) +
        (net_tx_packets_by_msg[NET_MSG_WORLD_PLAYER_DOCK_Q] -
         net_tx_last_packets_by_msg[NET_MSG_WORLD_PLAYER_DOCK_Q]);
    uint64_t tx_world_players_bytes =
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYERS] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYERS]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIOND_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIOND_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYER_POSED_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYER_POSED_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIONM_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIONM_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_PLAYER_DOCK_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_PLAYER_DOCK_Q]);
    uint64_t tx_world_players_suppressed_bytes =
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYERS] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYERS]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTION_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIOND_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIOND_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_POSED_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_POSED_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIONM_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_MOTIONM_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_DOCK_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_PLAYER_DOCK_Q]);
    uint64_t tx_state_packets =
        net_tx_packets_by_msg[NET_MSG_STATE] -
        net_tx_last_packets_by_msg[NET_MSG_STATE];
    uint64_t tx_state_bytes =
        net_tx_bytes_by_msg[NET_MSG_STATE] -
        net_tx_last_bytes_by_msg[NET_MSG_STATE];
    uint64_t tx_input_applied_packets =
        net_tx_packets_by_msg[NET_MSG_INPUT_APPLIED] -
        net_tx_last_packets_by_msg[NET_MSG_INPUT_APPLIED];
    uint64_t tx_input_applied_bytes =
        net_tx_bytes_by_msg[NET_MSG_INPUT_APPLIED] -
        net_tx_last_bytes_by_msg[NET_MSG_INPUT_APPLIED];
    uint64_t tx_world_asteroids_bytes =
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROIDS] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROIDS]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROIDS_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROIDS_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROIDS8_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROIDS8_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_REMOVE] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_REMOVE]);
    uint64_t tx_world_asteroid_motion_bytes =
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_MOTION] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_MOTION]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_MOTION_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_MOTION_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POS_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POS_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POS8_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POS8_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POSD_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POSD_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POSD8_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_POSD8_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_ASTEROID_STATE_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_ASTEROID_STATE_Q]);
    uint64_t tx_world_npcs_bytes =
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPCS] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPCS]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION8_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION8_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_POS_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_POS_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_POSE_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_POSE_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_LINEAR_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_LINEAR_Q]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS]) +
        (net_tx_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS8_Q] -
         net_tx_last_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS8_Q]);
    uint64_t tx_world_npcs_suppressed_bytes =
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPCS] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPCS]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION8_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_MOTION8_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_POS_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_POS_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_POSE_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_POSE_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_LINEAR_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_LINEAR_Q]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS8_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_NPC_STATUS8_Q]);
    uint64_t tx_world_scaffolds_suppressed_bytes =
        net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_SCAFFOLDS] -
        net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_SCAFFOLDS];
    uint64_t tx_world_cargo_pods_suppressed_bytes =
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_CARGO_PODS] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_CARGO_PODS]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_CARGO_PODS_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_CARGO_PODS_Q]);
    uint64_t tx_world_interactions_suppressed_bytes =
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_INTERACTIONS] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_INTERACTIONS]) +
        (net_tx_suppressed_bytes_by_msg[NET_MSG_WORLD_INTERACTIONS_Q] -
         net_tx_last_suppressed_bytes_by_msg[NET_MSG_WORLD_INTERACTIONS_Q]);

    printf("{\"_aws\":{\"Timestamp\":%llu,\"CloudWatchMetrics\":[{\"Namespace\":\"Signal\","
           "\"Dimensions\":[[\"Service\",\"Build\"]],\"Metrics\":["
           "{\"Name\":\"ConnectedPlayers\",\"Unit\":\"Count\"},"
           "{\"Name\":\"ReadyPlayers\",\"Unit\":\"Count\"},"
           "{\"Name\":\"ActiveUsers1m\",\"Unit\":\"Count\"},"
           "{\"Name\":\"MetricPlayers\",\"Unit\":\"Count\"},"
           "{\"Name\":\"AvgPingMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"AvgAckMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"AvgAckGapMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"MaxAckGapMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"TxPackets\",\"Unit\":\"Count\"},"
           "{\"Name\":\"TxBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxBytesPerSec\",\"Unit\":\"Bytes/Second\"},"
           "{\"Name\":\"TxSuppressedPackets\",\"Unit\":\"Count\"},"
           "{\"Name\":\"TxSuppressedBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxBackpressurePackets\",\"Unit\":\"Count\"},"
           "{\"Name\":\"TxBackpressureBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldPlayersPackets\",\"Unit\":\"Count\"},"
           "{\"Name\":\"TxWorldPlayersBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldPlayersSuppressedBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxStatePackets\",\"Unit\":\"Count\"},"
           "{\"Name\":\"TxStateBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxInputAppliedPackets\",\"Unit\":\"Count\"},"
           "{\"Name\":\"TxInputAppliedBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldAsteroidsBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldAsteroidMotionBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldNpcsBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldNpcsSuppressedBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldScaffoldsSuppressedBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldCargoPodsSuppressedBytes\",\"Unit\":\"Bytes\"},"
           "{\"Name\":\"TxWorldInteractionsSuppressedBytes\",\"Unit\":\"Bytes\"}]}]},"
           "\"Service\":\"signal-relay\",\"Build\":\"%s\","
           "\"ConnectedPlayers\":%d,\"ReadyPlayers\":%d,"
           "\"ActiveUsers1m\":%d,\"MetricPlayers\":%d,"
           "\"AvgPingMs\":%.2f,\"AvgAckMs\":%.2f,"
           "\"AvgAckGapMs\":%.2f,\"MaxAckGapMs\":%u,"
           "\"TxPackets\":%llu,\"TxBytes\":%llu,\"TxBytesPerSec\":%.2f,"
           "\"TxSuppressedPackets\":%llu,\"TxSuppressedBytes\":%llu,"
           "\"TxBackpressurePackets\":%llu,\"TxBackpressureBytes\":%llu,"
           "\"TxWorldPlayersPackets\":%llu,\"TxWorldPlayersBytes\":%llu,"
           "\"TxWorldPlayersSuppressedBytes\":%llu,"
           "\"TxStatePackets\":%llu,\"TxStateBytes\":%llu,"
           "\"TxInputAppliedPackets\":%llu,\"TxInputAppliedBytes\":%llu,"
           "\"TxWorldAsteroidsBytes\":%llu,"
           "\"TxWorldAsteroidMotionBytes\":%llu,"
           "\"TxWorldNpcsBytes\":%llu,"
           "\"TxWorldNpcsSuppressedBytes\":%llu,"
           "\"TxWorldScaffoldsSuppressedBytes\":%llu,"
           "\"TxWorldCargoPodsSuppressedBytes\":%llu,"
           "\"TxWorldInteractionsSuppressedBytes\":%llu}\n",
           (unsigned long long)analytics_epoch_ms(),
           analytics_build_hash(),
           connected,
           ready,
           active_1m,
           metric_players,
           avg_ping,
           avg_ack,
           avg_gap,
           (unsigned)max_gap,
           (unsigned long long)tx_packets,
           (unsigned long long)tx_bytes,
           tx_bytes_per_sec,
           (unsigned long long)tx_suppressed_packets,
           (unsigned long long)tx_suppressed_bytes,
           (unsigned long long)tx_backpressure_packets,
           (unsigned long long)tx_backpressure_bytes,
           (unsigned long long)tx_world_players_packets,
           (unsigned long long)tx_world_players_bytes,
           (unsigned long long)tx_world_players_suppressed_bytes,
           (unsigned long long)tx_state_packets,
           (unsigned long long)tx_state_bytes,
           (unsigned long long)tx_input_applied_packets,
           (unsigned long long)tx_input_applied_bytes,
           (unsigned long long)tx_world_asteroids_bytes,
           (unsigned long long)tx_world_asteroid_motion_bytes,
           (unsigned long long)tx_world_npcs_bytes,
           (unsigned long long)tx_world_npcs_suppressed_bytes,
           (unsigned long long)tx_world_scaffolds_suppressed_bytes,
           (unsigned long long)tx_world_cargo_pods_suppressed_bytes,
           (unsigned long long)tx_world_interactions_suppressed_bytes);

    net_tx_last_packets_total = net_tx_packets_total;
    net_tx_last_bytes_total = net_tx_bytes_total;
    net_tx_last_suppressed_packets_total = net_tx_suppressed_packets_total;
    net_tx_last_suppressed_bytes_total = net_tx_suppressed_bytes_total;
    net_tx_last_backpressure_packets_total = net_tx_backpressure_packets_total;
    net_tx_last_backpressure_bytes_total = net_tx_backpressure_bytes_total;
    memcpy(net_tx_last_packets_by_msg, net_tx_packets_by_msg,
           sizeof(net_tx_last_packets_by_msg));
    memcpy(net_tx_last_bytes_by_msg, net_tx_bytes_by_msg,
           sizeof(net_tx_last_bytes_by_msg));
    memcpy(net_tx_last_suppressed_packets_by_msg,
           net_tx_suppressed_packets_by_msg,
           sizeof(net_tx_last_suppressed_packets_by_msg));
    memcpy(net_tx_last_suppressed_bytes_by_msg,
           net_tx_suppressed_bytes_by_msg,
           sizeof(net_tx_last_suppressed_bytes_by_msg));
    memcpy(net_tx_last_backpressure_packets_by_msg,
           net_tx_backpressure_packets_by_msg,
           sizeof(net_tx_last_backpressure_packets_by_msg));
    memcpy(net_tx_last_backpressure_bytes_by_msg,
           net_tx_backpressure_bytes_by_msg,
           sizeof(net_tx_last_backpressure_bytes_by_msg));
    net_tx_last_emf_ms = now_ms;
}

static void broadcast_highscores(void) {
    uint8_t buf[HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE];
    int len = highscore_serialize(buf, &highscores);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!world.players[p].connected || !world.players[p].connection->conn) continue;
        ws_send(world.players[p].connection->conn, buf, (size_t)len);
    }
}

static void send_highscores_to(struct mg_connection *c) {
    if (!c) return;
    uint8_t buf[HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE];
    int len = highscore_serialize(buf, &highscores);
    ws_send(c, buf, (size_t)len);
}

#define STATION_IDENTITY_FALLBACK_MS 10000 /* reconciliation, not heartbeat */
#define STATION_DIAG_MIN_MS 300
static uint64_t last_station_identity = 0;
static uint64_t last_player_state_emit = 0;

/* Timing intervals in milliseconds */
#define SIM_TICK_MS   8     /* ~120 Hz poll gate; sim uses SIM_DT accumulator */
#define STATE_TICK_MS 50    /* 20 Hz player state broadcast */
#define PLAYER_MOTION_SEND_INTERVAL_MS (STATE_TICK_MS * 4u)
#define WORLD_TICK_MS 100   /* 10 Hz world state broadcast */
#define WORLD_NPC_MOTION_REPEAT_TICKS NPC_MOTION_NET_REPEAT_TICKS
#define WORLD_NPC_STATUS_REPEAT_TICKS NPC_STATUS_NET_REPEAT_TICKS
#define WORLD_CARGO_POD_MOTION_REPEAT_TICKS CARGO_POD_MOTION_NET_REPEAT_TICKS
#define WS_DEFERABLE_SEND_BUFFER_RESERVE 8192u /* keep room for PONG + authoritative acks */
#define SHIP_TICK_MS  250   /* 4 Hz full ship state (cargo, hull, etc.) */
#define MAX_SIM_STEPS 8     /* cap sub-steps per poll to prevent spiral */
#define SAVE_PATH "world.sav"
#define PLAYER_SAVE_DIR "saves"
#define STATION_CATALOG_DIR "stations"
#define AUTOSAVE_MS 30000   /* autosave every 30 seconds */

/* Truncate the build SHA (GIT_HASH at compile time, "dev" otherwise) to
 * 8 hex chars and parse as u32 for the leaderboard's build_id column.
 * The same value is emitted as a BUILD_INFO operator post at startup so
 * the chain replay walker can tag historical deaths with their build. */
static uint32_t signal_build_id_u32(void) {
#ifdef GIT_HASH
    const char *hash = GIT_HASH;
#else
    const char *hash = "dev";
#endif
    uint32_t bid = 0;
    int n = 0;
    for (size_t i = 0; hash[i] && n < 8; i++) {
        char c = hash[i];
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
        if (v < 0) continue;
        bid = (bid << 4) | (uint32_t)v;
        n++;
    }
    return bid;
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                     */
/* ------------------------------------------------------------------ */

static void signal_handler(int sig) {
    (void)sig;
    running = false;
}

/* ------------------------------------------------------------------ */
/* Player management                                                  */
/* ------------------------------------------------------------------ */

static int alloc_player(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected) return i;
    }
    return -1;
}

static void reset_player_slot_for_reuse(int pid) {
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    (void)world_player_release_ship_asset(&world, pid);
    world_player_ship_slot_release(&world, pid);
    world_player_runtime_slot_reset(&world, pid);
}

static int server_bot_home_station_for(int bot_index) {
    int active[MAX_STATIONS];
    int n = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &world.stations[s];
        if (!station_is_active(st)) continue;
        if (!station_has_module(st, MODULE_DOCK)) continue;
        active[n++] = s;
    }
    if (n == 0) return 0;
    return active[bot_index % n];
}

static void spawn_server_bots(void) {
    server_bot_players_spawned = 0;
    if (server_bot_player_target <= 0) return;

    for (int i = 0; i < MAX_PLAYERS &&
         server_bot_players_spawned < server_bot_player_target; i++) {
        if (world.players[i].connected) continue;

        reset_player_slot_for_reuse(i);
        server_player_t *sp = &world.players[i];

        sp->connected = true;
        sp->id = (uint8_t)i;
        sp->connection->conn = NULL;
        sp->session_ready = true;
        sp->grace_period = false;
        sp->grace_timer = 0.0f;

        player_init_ship(sp, &world);
        unsigned bot_no = (unsigned)(server_bot_players_spawned % 900) + 1u;
        snprintf(sp->callsign, sizeof(sp->callsign), "BOT%03u", bot_no);

        signal_crypto_random_bytes(sp->session_token,
                                   sizeof(sp->session_token));

        int home_station = server_bot_home_station_for(server_bot_players_spawned);
        if (home_station >= 0 && home_station < MAX_STATIONS &&
            station_is_active(&world.stations[home_station])) {
            sp->current_station = home_station;
            sp->nearby_station = home_station;
            sp->docked = true;
            sp->in_dock_range = true;
            anchor_ship_in_station(sp, &world);
        }

        player_seed_credits(sp, &world);
        sp->autopilot_mode = 1;
        sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
        sp->autopilot_target = -1;
        sp->autopilot_timer = 0.0f;
        sp->autopilot_last_pos = sp->ship->pos;
        sp->autopilot_stuck_timer = 0.0f;
        sp->server_brain_mode = (uint8_t)server_bot_brain_mode;

        printf("[server] bot player %d spawned as %s brain=%s home=%d\n",
               i, sp->callsign, server_bot_brain_mode_name, sp->current_station);
        server_bot_players_spawned++;
    }

    if (server_bot_players_spawned < server_bot_player_target) {
        fprintf(stderr, "[WARN] SIGNAL_BOT_PLAYERS requested %d bot(s), "
                "but only %d player slot(s) were available\n",
                server_bot_player_target, server_bot_players_spawned);
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket send helpers                                             */
/* ------------------------------------------------------------------ */

static void ws_send(struct mg_connection *c, const void *data, size_t len) {
    if (c && data && len > 0) {
        uint8_t msg = ((const uint8_t *)data)[0];
        net_tx_packets_total++;
        net_tx_bytes_total += (uint64_t)len;
        net_tx_packets_by_msg[msg]++;
        net_tx_bytes_by_msg[msg] += (uint64_t)len;
    }
    mg_ws_send(c, data, len, WEBSOCKET_OP_BINARY);
}

static void net_tx_record_suppressed(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    uint8_t msg = data[0];
    net_tx_suppressed_packets_total++;
    net_tx_suppressed_bytes_total += (uint64_t)len;
    net_tx_suppressed_packets_by_msg[msg]++;
    net_tx_suppressed_bytes_by_msg[msg] += (uint64_t)len;
}

static void net_tx_record_backpressure_suppressed(const uint8_t *data,
                                                  size_t len) {
    if (!data || len == 0) return;
    uint8_t msg = data[0];
    net_tx_backpressure_packets_total++;
    net_tx_backpressure_bytes_total += (uint64_t)len;
    net_tx_backpressure_packets_by_msg[msg]++;
    net_tx_backpressure_bytes_by_msg[msg] += (uint64_t)len;
    net_tx_record_suppressed(data, len);
}

static bool ws_deferable_snapshot_backpressured(struct mg_connection *c,
                                                const uint8_t *data,
                                                size_t len) {
    if (!c || !data || len == 0) return false;
    return net_deferable_snapshot_would_backpressure(
        data[0], c->send.len, len, WS_DEFERABLE_SEND_BUFFER_RESERVE);
}

static bool ws_defer_snapshot_if_backpressured(struct mg_connection *c,
                                               const uint8_t *data,
                                               size_t len) {
    if (!ws_deferable_snapshot_backpressured(c, data, len)) return false;
    net_tx_record_backpressure_suppressed(data, len);
    return true;
}

static void ws_send_if_changed(struct mg_connection *c,
                               net_payload_cache_t *cache,
                               const uint8_t *data,
                               size_t len) {
    if (!c || !data) return;
    if (!cache || len > UINT16_MAX) {
        ws_send(c, data, len);
        return;
    }
    if (!net_payload_cache_should_send(cache, c, data, len)) {
        net_tx_record_suppressed(data, len);
        return;
    }
    ws_send(c, data, len);
}

static bool ws_send_player_states_if_changed(struct mg_connection *c,
                                             server_player_t *sp,
                                             const uint8_t *data,
                                             size_t len,
                                             uint64_t now) {
    if (!c || !sp || !data) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        return true;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return false;
    net_payload_cache_t *cache = &sp->replication->world_players_cache;
    uint64_t hash = net_world_players_semantic_hash(data, (int)len);
    bool heartbeat_due = world_players_semantic_heartbeat_due(
        sp->replication->world_players_last_sent_ms, now);
    if (cache->valid &&
        cache->conn == c &&
        cache->len == (uint16_t)len &&
        cache->hash == hash &&
        !heartbeat_due) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    ws_send(c, data, len);
    cache->valid = true;
    cache->conn = c;
    cache->len = (uint16_t)len;
    cache->hash = hash;
    sp->replication->world_players_last_sent_ms = now;
    return true;
}

static bool ws_send_player_motion_if_changed(struct mg_connection *c,
                                             server_player_t *sp,
                                             const uint8_t *data,
                                             size_t len) {
    if (!c || !sp || !data || len <= PLAYER_MOTION_MSG_HEADER) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        return true;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return false;
    net_payload_cache_t *cache = &sp->replication->world_player_motion_cache;
    if (!net_payload_cache_should_send(cache, c, data, len)) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    ws_send(c, data, len);
    return true;
}

static bool ws_send_player_motion_mixed(struct mg_connection *c,
                                        server_player_t *sp,
                                        const uint8_t *data,
                                        size_t len) {
    if (!c || !sp || !data || len <= PLAYER_MOTIONM_Q_MSG_HEADER)
        return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        return true;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return false;
    ws_send(c, data, len);
    return true;
}

static bool ws_send_player_dock_if_changed(struct mg_connection *c,
                                           server_player_t *sp,
                                           const uint8_t *data,
                                           size_t len) {
    if (!c || !sp || !data || len <= PLAYER_DOCK_MSG_HEADER) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        return true;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return false;
    net_payload_cache_t *cache = &sp->replication->world_player_dock_cache;
    if (!net_payload_cache_should_send(cache, c, data, len)) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    ws_send(c, data, len);
    return true;
}

static void ws_send_station_identity_if_changed(struct mg_connection *c,
                                                server_player_t *sp,
                                                const uint8_t *data,
                                                size_t len) {
    if (!c || !sp || !data) return;
    if (len > UINT16_MAX || len < 2 || data[0] != NET_MSG_STATION_IDENTITY) {
        ws_send(c, data, len);
        return;
    }
    int station_idx = data[1];
    if (station_idx < 0 || station_idx >= MAX_STATIONS) {
        ws_send(c, data, len);
        return;
    }
    uint8_t compact[STATION_IDENTITY_Q_MAX_SIZE];
    int compact_len = serialize_station_identity_q_from_full(
        compact, data, (int)len);
    const uint8_t *wire_data = data;
    size_t wire_len = len;
    if (compact_len > 0) {
        wire_data = compact;
        wire_len = (size_t)compact_len;
    }
    net_payload_cache_t *cache = &sp->replication->station_identity_cache[station_idx];
    uint64_t hash = net_station_identity_semantic_hash(data, (int)len);
    if (cache->valid &&
        cache->conn == c &&
        cache->len == (uint16_t)wire_len &&
        cache->hash == hash) {
        net_tx_record_suppressed(wire_data, wire_len);
        return;
    }
    ws_send(c, wire_data, wire_len);
    cache->valid = true;
    cache->conn = c;
    cache->len = (uint16_t)wire_len;
    cache->hash = hash;
}

static void ws_send_world_stations_if_changed(struct mg_connection *c,
                                              server_player_t *sp,
                                              const uint8_t *data,
                                              size_t len) {
    if (!c || !sp || !data) return;
    if (len > UINT16_MAX || len < 2 || data[0] != NET_MSG_WORLD_STATIONS) {
        ws_send(c, data, len);
        return;
    }
    uint8_t compact[STATION_Q_MAX_SIZE];
    int compact_len = serialize_stations_q_from_full(compact, data, (int)len);
    const uint8_t *wire_data = data;
    size_t wire_len = len;
    if (compact_len > 0 && (size_t)compact_len < len) {
        wire_data = compact;
        wire_len = (size_t)compact_len;
    }
    ws_send_if_changed(c, &sp->replication->world_stations_cache, wire_data, wire_len);
}

static void invalidate_player_authoritative_caches(server_player_t *sp) {
    if (!sp) return;
    server_player_reset_authoritative_ack_state(sp);
    sp->replication->player_ship_cache.valid = false;
    sp->replication->hold_ingots_cache.valid = false;
    sp->replication->player_manifest_cache.valid = false;
    sp->replication->inspect_snapshot_cache.valid = false;
    sp->replication->contracts_cache.valid = false;
    sp->replication->contracts_semantic_hash = 0;
    sp->replication->contracts_semantic_valid = false;
    sp->replication->contracts_last_sent_ms = 0;
    sp->replication->known_contracts_cache.valid = false;
    sp->replication->known_ledger_cache.valid = false;
    sp->replication->delivery_ledger_cache.valid = false;
    sp->replication->world_stations_cache.valid = false;
    sp->replication->world_players_cache.valid = false;
    server_player_motion_delta_clear_all(sp);
    sp->replication->world_player_dock_cache.valid = false;
    sp->replication->world_players_last_sent_ms = 0;
    sp->replication->world_player_motion_last_sent_ms = 0;
    sp->replication->world_time_sent = false;
    sp->replication->world_time_last_sent_tick = 0;
    sp->replication->world_npcs_cache.valid = false;
    sp->replication->world_npc_motion_cache.valid = false;
    sp->replication->world_npcs_semantic_hash = 0;
    sp->replication->world_npcs_semantic_valid = false;
    sp->replication->world_npcs_last_sent_tick = 0;
    sp->replication->world_npc_motion_last_sent_tick = 0;
    memset(sp->replication->npc_motion_sent_tick, 0,
           sizeof(sp->replication->npc_motion_sent_tick));
    memset(sp->replication->npc_motion_sent_flags, 0,
           sizeof(sp->replication->npc_motion_sent_flags));
    memset(sp->replication->npc_motion_sent_pos, 0,
           sizeof(sp->replication->npc_motion_sent_pos));
    memset(sp->replication->npc_motion_sent_vel, 0,
           sizeof(sp->replication->npc_motion_sent_vel));
    memset(sp->replication->npc_motion_sent_angle, 0,
           sizeof(sp->replication->npc_motion_sent_angle));
    sp->replication->world_npc_status_cache.valid = false;
    sp->replication->world_npc_status_last_sent_tick = 0;
    sp->replication->world_scaffolds_cache.valid = false;
    memset(sp->replication->scaffold_sent, 0, sizeof(sp->replication->scaffold_sent));
    memset(sp->replication->scaffold_sent_sig, 0,
           sizeof(sp->replication->scaffold_sent_sig));
    memset(sp->replication->scaffold_motion_sent_sig, 0,
           sizeof(sp->replication->scaffold_motion_sent_sig));
    sp->replication->world_cargo_pods_cache.valid = false;
    sp->replication->world_cargo_pod_motion_cache.valid = false;
    sp->replication->world_cargo_pods_semantic_hash = 0;
    sp->replication->world_cargo_pods_semantic_valid = false;
    sp->replication->world_cargo_pods_last_sent_tick = 0;
    sp->replication->world_cargo_pod_motion_last_sent_tick = 0;
    memset(sp->replication->cargo_pod_sent, 0, sizeof(sp->replication->cargo_pod_sent));
    memset(sp->replication->cargo_pod_sent_sig, 0,
           sizeof(sp->replication->cargo_pod_sent_sig));
    sp->replication->world_interactions_cache.valid = false;
    sp->replication->world_interaction_drift_cache.valid = false;
    sp->replication->world_interactions_semantic_hash = 0;
    sp->replication->world_interactions_semantic_valid = false;
    sp->replication->world_interactions_last_sent_tick = 0;
    sp->replication->world_interaction_drift_last_sent_tick = 0;
    sp->replication->world_interaction_drift_block_tick = 0;
    server_player_invalidate_asteroid_stream_caches(sp);
    memset(sp->replication->cargo_pod_motion_sent_tick, 0,
           sizeof(sp->replication->cargo_pod_motion_sent_tick));
    memset(sp->replication->cargo_pod_motion_sent_pos, 0,
           sizeof(sp->replication->cargo_pod_motion_sent_pos));
    memset(sp->replication->cargo_pod_motion_sent_vel, 0,
           sizeof(sp->replication->cargo_pod_motion_sent_vel));
    memset(sp->replication->cargo_pod_motion_sent_rotation, 0,
           sizeof(sp->replication->cargo_pod_motion_sent_rotation));
    memset(sp->replication->fracture_challenge_sent_id, 0,
           sizeof(sp->replication->fracture_challenge_sent_id));
    memset(sp->replication->fracture_resolved_sent_ids, 0,
           sizeof(sp->replication->fracture_resolved_sent_ids));
    sp->replication->fracture_resolved_sent_cursor = 0;
}

static void force_player_authoritative_resync(server_player_t *sp) {
    if (sp) sp->replication->force_authoritative_resync = true;
}

static void send_action_ack(struct mg_connection *c, uint16_t action_id,
                            uint16_t input_seq, uint8_t status,
                            uint8_t action) {
    uint8_t buf[NET_ACTION_ACK_SIZE];
    int len = serialize_action_ack(buf, action_id, input_seq, status, action);
    ws_send(c, buf, (size_t)len);
}

static void send_action_result(struct mg_connection *c, uint16_t action_id,
                               uint16_t input_seq, uint8_t status,
                               uint8_t action, uint32_t server_tick) {
    uint8_t buf[NET_ACTION_RESULT_SIZE];
    int len = serialize_action_result(buf, action_id, input_seq, status,
                                      action, server_tick);
    ws_send(c, buf, (size_t)len);
}

static void send_latency_pong(struct mg_connection *c, uint32_t seq,
                              uint32_t client_sent_ms,
                              uint32_t server_recv_ms) {
    uint8_t buf[NET_LATENCY_PONG_SIZE];
    uint32_t server_send_ms = (uint32_t)mg_millis();
    int len = serialize_latency_pong(buf, seq, client_sent_ms,
                                     server_recv_ms, server_send_ms,
                                     world.tick);
    ws_send(c, buf, (size_t)len);
}

static void send_pending_action_results(const sim_events_t *events) {
    uint32_t server_tick = world.tick;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (!sp->pending_action_result_valid) continue;
        uint8_t status = server_pending_action_result_status(&world, sp, events);
        force_player_authoritative_resync(sp);
        printf("[server] action-result player=%d id=%u input_seq=%u action=%u status=%s tick=%u resync=authoritative\n",
               sp->id,
               (unsigned)sp->pending_action_result_id,
               (unsigned)sp->pending_action_result_input_seq,
               (unsigned)sp->pending_action_result_action,
               server_action_result_status_name(status),
               (unsigned)server_tick);
        if (server_player_is_gameplay_ready(sp) && sp->connection->conn) {
            send_action_result(sp->connection->conn,
                               sp->pending_action_result_id,
                               sp->pending_action_result_input_seq,
                               status,
                               sp->pending_action_result_action,
                               server_tick);
        }
        sp->pending_action_result_valid = false;
    }
}

static void broadcast(const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!server_player_is_gameplay_ready(&world.players[i]) ||
            !world.players[i].connection->conn)
            continue;
        if (data && len >= 2 &&
            ((const uint8_t *)data)[0] == NET_MSG_STATION_IDENTITY) {
            ws_send_station_identity_if_changed(
                world.players[i].connection->conn,
                &world.players[i],
                (const uint8_t *)data,
                len);
        } else if (data && len >= 2 &&
                   ((const uint8_t *)data)[0] == NET_MSG_WORLD_STATIONS) {
            ws_send_world_stations_if_changed(
                world.players[i].connection->conn,
                &world.players[i],
                (const uint8_t *)data,
                len);
        } else {
            ws_send(world.players[i].connection->conn, data, len);
        }
    }
}

static void ws_send_contracts_if_changed(struct mg_connection *c,
                                         server_player_t *sp,
                                         const uint8_t *data,
                                         size_t len,
                                         uint64_t now_ms) {
    if (!c || !sp || !data) return;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        return;
    }
    uint64_t hash = net_contracts_semantic_hash(data, (int)len);
    bool semantic_changed = !sp->replication->contracts_semantic_valid ||
        sp->replication->contracts_semantic_hash != hash;
    bool refresh_due = contracts_age_refresh_due(
        sp->replication->contracts_last_sent_ms, now_ms);
    uint8_t compact[CONTRACT_Q_MAX_SIZE];
    int compact_len = serialize_contracts_q_from_full(compact, data, (int)len);
    const uint8_t *wire_data = data;
    size_t wire_len = len;
    if (compact_len > 0 && (size_t)compact_len < len) {
        wire_data = compact;
        wire_len = (size_t)compact_len;
    }
    if (!semantic_changed && !refresh_due) {
        net_tx_record_suppressed(wire_data, wire_len);
        return;
    }
    sp->replication->contracts_semantic_hash = hash;
    sp->replication->contracts_semantic_valid = true;
    sp->replication->contracts_last_sent_ms = now_ms;
    net_payload_cache_t *cache = &sp->replication->contracts_cache;
    uint64_t wire_hash = net_payload_hash(wire_data, wire_len);
    if (cache->valid &&
        cache->conn == c &&
        cache->len == (uint16_t)wire_len &&
        cache->hash == wire_hash) {
        net_tx_record_suppressed(wire_data, wire_len);
        return;
    }
    ws_send(c, wire_data, wire_len);
    cache->valid = true;
    cache->conn = c;
    cache->len = (uint16_t)wire_len;
    cache->hash = wire_hash;
}

static void broadcast_contracts_if_changed(const uint8_t *data, size_t len,
                                           uint64_t now_ms) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (server_player_is_gameplay_ready(sp) && sp->connection->conn)
            ws_send_contracts_if_changed(sp->connection->conn, sp, data, len, now_ms);
    }
}

static void ws_packet_sink(void *user, const uint8_t *data, int len) {
    struct mg_connection *c = (struct mg_connection *)user;
    if (!c || !data || len <= 0) return;
    if (data[0] == NET_MSG_INPUT_APPLIED && len >= NET_INPUT_APPLIED_SIZE) {
        uint8_t stamped[NET_INPUT_APPLIED_SIZE];
        memcpy(stamped, data, sizeof(stamped));
        write_u32_le(&stamped[19], (uint32_t)mg_millis());
        ws_send(c, stamped, sizeof(stamped));
        return;
    }
    if (data[0] == NET_MSG_STATE && len >= NET_STATE_AUTH_SIZE) {
        uint8_t stamped[NET_STATE_AUTH_SIZE];
        memcpy(stamped, data, sizeof(stamped));
        write_u32_le(&stamped[NET_STATE_AUTH_SERVER_SEND_MS_OFFSET],
                     (uint32_t)mg_millis());
        ws_send(c, stamped, sizeof(stamped));
        return;
    }
    ws_send(c, data, (size_t)len);
}

typedef struct {
    struct mg_connection *conn;
    server_player_t *player;
    uint32_t world_tick;
} ws_private_packet_sink_t;

static void ws_private_packet_sink(void *user, const uint8_t *data, int len) {
    ws_private_packet_sink_t *sink = (ws_private_packet_sink_t *)user;
    if (!sink || !sink->conn || !sink->player || !data || len <= 0) return;
    net_payload_cache_t *cache = NULL;
    switch (data[0]) {
    case NET_MSG_PLAYER_SHIP:
        cache = &sink->player->replication->player_ship_cache;
        break;
    case NET_MSG_HOLD_INGOTS:
        cache = &sink->player->replication->hold_ingots_cache;
        break;
    case NET_MSG_PLAYER_MANIFEST:
        cache = &sink->player->replication->player_manifest_cache;
        break;
    case NET_MSG_INSPECT_SNAPSHOT:
        cache = &sink->player->replication->inspect_snapshot_cache;
        break;
    case NET_MSG_PLAYER_KNOWN_CONTRACTS:
        cache = &sink->player->replication->known_contracts_cache;
        break;
    case NET_MSG_PLAYER_KNOWN_LEDGER:
        cache = &sink->player->replication->known_ledger_cache;
        break;
    case NET_MSG_DELIVERY_LEDGER:
        cache = &sink->player->replication->delivery_ledger_cache;
        break;
    default:
        break;
    }
    ws_send_if_changed(sink->conn, cache, data, (size_t)len);
}

static void ws_station_snapshot_packet_sink(void *user,
                                            const uint8_t *data,
                                            int len) {
    ws_private_packet_sink_t *sink = (ws_private_packet_sink_t *)user;
    if (!sink || !sink->conn || !sink->player || !data || len <= 0) return;
    if (data[0] == NET_MSG_STATION_IDENTITY && len >= 2) {
        ws_send_station_identity_if_changed(
            sink->conn,
            sink->player,
            data,
            (size_t)len);
        return;
    }
    if (data[0] == NET_MSG_WORLD_STATIONS && len >= 2) {
        ws_send_world_stations_if_changed(
            sink->conn,
            sink->player,
            data,
            (size_t)len);
        return;
    }
    ws_send_if_changed(sink->conn, NULL, data, (size_t)len);
}

static void send_initial_world_bundle_to_player(struct mg_connection *c,
                                                int pid) {
    if (!c || pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!server_player_is_gameplay_ready(sp) || sp->connection->conn != c) return;

    {
        ws_private_packet_sink_t station_sink = {
            .conn = c,
            .player = sp,
        };
        server_emit_station_snapshot(
            &world, true, ws_station_snapshot_packet_sink, &station_sink,
            &station_snapshot_scratch);
    }

    if (!sp->docked) {
        int asteroids_q_len = 0;
        int asteroids8_q_len = 0;
        int sync_len = serialize_asteroids_for_player_split_ext_state_budget_at_tick(
            world_snapshot_scratch.asteroids,
            world_snapshot_scratch.asteroids_q, &asteroids_q_len,
            world_snapshot_scratch.asteroids8_q, &asteroids8_q_len,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
            world.asteroids, sp->ship->pos,
            sp->replication->asteroid_sent, sp->replication->asteroid_motion_sent_tick,
            sp->replication->asteroid_motion_sent_pos,
            sp->replication->asteroid_motion_sent_vel, sp->replication->asteroid_identity_sent_sig,
            NULL, NULL, NULL,
            world.tick,
            asteroid_net_background_identity_budget_at_tick(world.tick));
        if (sync_len > ASTEROID_MSG_HEADER)
            ws_send(c, world_snapshot_scratch.asteroids, (size_t)sync_len);
        if (asteroids8_q_len > ASTEROID8_Q_MSG_HEADER)
            ws_send(c, world_snapshot_scratch.asteroids8_q,
                    (size_t)asteroids8_q_len);
        if (asteroids_q_len > ASTEROID_Q_MSG_HEADER)
            ws_send(c, world_snapshot_scratch.asteroids_q,
                    (size_t)asteroids_q_len);
    }

    send_highscores_to(c);

    if (world.signal_channel.count > 0) {
        size_t cap = (size_t)(3 +
            world.signal_channel.count * SIGNAL_CHANNEL_RECORD_SIZE);
        uint8_t *msg = (uint8_t *)malloc(cap);
        if (msg) {
            int len = serialize_signal_channel(msg, &world.signal_channel);
            ws_send(c, msg, (size_t)len);
            free(msg);
        }
    }
}

static void server_note_npc_identity_packet_sent(server_player_t *sp,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 uint32_t world_tick);

static bool ws_send_world_npcs_if_changed(struct mg_connection *c,
                                          server_player_t *sp,
                                          const uint8_t *data,
                                          size_t len,
                                          uint32_t world_tick) {
    if (!c || !sp || !data) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        server_note_npc_identity_packet_sent(sp, data, len, world_tick);
        sp->replication->world_npcs_last_sent_tick = world_tick;
        return true;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return false;
    uint64_t hash = net_world_npcs_semantic_hash(data, (int)len);
    bool semantic_changed = !sp->replication->world_npcs_semantic_valid ||
        sp->replication->world_npcs_semantic_hash != hash;
    bool refresh_due = npc_net_metadata_refresh_due(
        sp->replication->world_npcs_last_sent_tick, world_tick);
    if (!semantic_changed && !refresh_due) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    if (!net_payload_cache_should_send(&sp->replication->world_npcs_cache, c, data, len)) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    sp->replication->world_npcs_semantic_hash = hash;
    sp->replication->world_npcs_semantic_valid = true;
    sp->replication->world_npcs_last_sent_tick = world_tick;
    ws_send(c, data, len);
    server_note_npc_identity_packet_sent(sp, data, len, world_tick);
    return true;
}

typedef struct {
    uint8_t index;
    uint8_t flags;
    vec2 pos;
    vec2 vel;
    float angle;
} server_npc_motion_sample_t;

static bool server_decode_npc_motion_sample(const uint8_t *p,
                                            uint8_t type,
                                            server_npc_motion_sample_t *out) {
    if (!p || !out) return false;
    out->index = p[0];
    out->flags = p[1];
    if (type == NET_MSG_WORLD_NPC_MOTION_Q) {
        out->pos = v2((float)(int16_t)read_u16_le(&p[2]) *
                          NPC_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[4]) *
                          NPC_MOTION_Q_POS_SCALE);
        out->vel = v2((float)(int16_t)read_u16_le(&p[6]) *
                          NPC_MOTION_Q_VEL_SCALE,
                      (float)(int16_t)read_u16_le(&p[8]) *
                          NPC_MOTION_Q_VEL_SCALE);
        out->angle = (float)read_u16_le(&p[10]) *
            NPC_MOTION_Q_ANGLE_SCALE;
        return true;
    }
    if (type == NET_MSG_WORLD_NPC_MOTION8_Q) {
        out->pos = v2((float)(int16_t)read_u16_le(&p[2]) *
                          NPC_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[4]) *
                          NPC_MOTION_Q_POS_SCALE);
        out->vel = v2((float)(int8_t)p[6] * NPC_MOTION8_Q_VEL_SCALE,
                      (float)(int8_t)p[7] * NPC_MOTION8_Q_VEL_SCALE);
        out->angle = (float)p[8] * NPC_MOTION8_Q_ANGLE_SCALE;
        return true;
    }
    if (type == NET_MSG_WORLD_NPC_MOTION) {
        out->pos = v2(read_f32_le(&p[2]), read_f32_le(&p[6]));
        out->vel = v2(read_f32_le(&p[10]), read_f32_le(&p[14]));
        out->angle = read_f32_le(&p[18]);
        return true;
    }
    return false;
}

static size_t server_filter_npc_motion_packet(server_player_t *sp,
                                              const uint8_t *data,
                                              size_t len,
                                              uint8_t *out,
                                              uint8_t *pos_out,
                                              size_t *pos_len_out,
                                              uint8_t *pose_out,
                                              size_t *pose_len_out,
                                              uint8_t *linear_out,
                                              size_t *linear_len_out,
                                              uint32_t world_tick) {
    if (!sp || !data || !out || len < 2) return 0;
    if (pos_len_out) *pos_len_out = 0;
    if (pose_len_out) *pose_len_out = 0;
    if (linear_len_out) *linear_len_out = 0;
    uint8_t type = data[0];
    size_t header = 0;
    size_t record_size = 0;
    if (type == NET_MSG_WORLD_NPC_MOTION_Q) {
        header = NPC_MOTION_Q_MSG_HEADER;
        record_size = NPC_MOTION_Q_RECORD_SIZE;
    } else if (type == NET_MSG_WORLD_NPC_MOTION8_Q) {
        header = NPC_MOTION8_Q_MSG_HEADER;
        record_size = NPC_MOTION8_Q_RECORD_SIZE;
    } else if (type == NET_MSG_WORLD_NPC_MOTION) {
        header = NPC_MOTION_MSG_HEADER;
        record_size = NPC_MOTION_RECORD_SIZE;
    } else {
        return 0;
    }
    if (len < header) return 0;
    uint8_t count = data[1];
    size_t expected = header + (size_t)count * record_size;
    if (len < expected) return 0;

    out[0] = type;
    out[1] = 0;
    uint8_t pos_count = 0;
    if (pos_out) {
        pos_out[0] = NET_MSG_WORLD_NPC_POS_Q;
        pos_out[1] = 0;
    }
    uint8_t pose_count = 0;
    if (pose_out) {
        pose_out[0] = NET_MSG_WORLD_NPC_POSE_Q;
        pose_out[1] = 0;
    }
    uint8_t linear_count = 0;
    if (linear_out) {
        linear_out[0] = NET_MSG_WORLD_NPC_LINEAR_Q;
        linear_out[1] = 0;
    }
    uint8_t out_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *record = &data[header + (size_t)i * record_size];
        server_npc_motion_sample_t sample;
        if (!server_decode_npc_motion_sample(record, type, &sample))
            continue;
        if (!npc_motion_should_send(sp,
                                    sample.index,
                                    sample.flags,
                                    sample.pos,
                                    sample.vel,
                                    sample.angle,
                                    world_tick))
            continue;
        if (pos_out &&
            npc_motion_pos_q_eligible(sp,
                                      sample.index,
                                      sample.flags,
                                      sample.vel,
                                      sample.angle,
                                      world_tick)) {
            serialize_one_npc_pos_q(
                &pos_out[NPC_POS_Q_MSG_HEADER +
                         (size_t)pos_count * NPC_POS_Q_RECORD_SIZE],
                sample.index,
                sample.pos);
            pos_count++;
            continue;
        }
        if (pose_out &&
            npc_motion_pose_q_eligible(sp,
                                       sample.index,
                                       sample.flags,
                                       sample.vel,
                                       world_tick)) {
            serialize_one_npc_pose_q(
                &pose_out[NPC_POSE_Q_MSG_HEADER +
                          (size_t)pose_count * NPC_POSE_Q_RECORD_SIZE],
                sample.index,
                sample.pos,
                sample.angle);
            pose_count++;
            continue;
        }
        if (linear_out &&
            npc_motion_linear_q_eligible(sp,
                                         sample.index,
                                         sample.flags,
                                         sample.angle,
                                         world_tick)) {
            serialize_one_npc_linear_q(
                &linear_out[NPC_LINEAR_Q_MSG_HEADER +
                            (size_t)linear_count * NPC_LINEAR_Q_RECORD_SIZE],
                sample.index,
                sample.pos,
                sample.vel);
            linear_count++;
            continue;
        }
        memcpy(&out[header + (size_t)out_count * record_size],
               record,
               record_size);
        out_count++;
    }
    out[1] = out_count;
    if (pos_out) {
        pos_out[1] = pos_count;
        if (pos_len_out) {
            *pos_len_out = NPC_POS_Q_MSG_HEADER +
                (size_t)pos_count * NPC_POS_Q_RECORD_SIZE;
        }
    }
    if (pose_out) {
        pose_out[1] = pose_count;
        if (pose_len_out) {
            *pose_len_out = NPC_POSE_Q_MSG_HEADER +
                (size_t)pose_count * NPC_POSE_Q_RECORD_SIZE;
        }
    }
    if (linear_out) {
        linear_out[1] = linear_count;
        if (linear_len_out) {
            *linear_len_out = NPC_LINEAR_Q_MSG_HEADER +
                (size_t)linear_count * NPC_LINEAR_Q_RECORD_SIZE;
        }
    }
    return header + (size_t)out_count * record_size;
}

static void server_note_npc_motion_packet_sent(server_player_t *sp,
                                               const uint8_t *data,
                                               size_t len,
                                               uint32_t world_tick) {
    if (!sp || !data || len < 2) return;
    uint8_t type = data[0];
    size_t header = 0;
    size_t record_size = 0;
    if (type == NET_MSG_WORLD_NPC_MOTION_Q) {
        header = NPC_MOTION_Q_MSG_HEADER;
        record_size = NPC_MOTION_Q_RECORD_SIZE;
    } else if (type == NET_MSG_WORLD_NPC_MOTION8_Q) {
        header = NPC_MOTION8_Q_MSG_HEADER;
        record_size = NPC_MOTION8_Q_RECORD_SIZE;
    } else if (type == NET_MSG_WORLD_NPC_MOTION) {
        header = NPC_MOTION_MSG_HEADER;
        record_size = NPC_MOTION_RECORD_SIZE;
    } else {
        return;
    }
    uint8_t count = data[1];
    if (len < header + (size_t)count * record_size) return;
    for (uint8_t i = 0; i < count; i++) {
        server_npc_motion_sample_t sample;
        const uint8_t *record = &data[header + (size_t)i * record_size];
        if (!server_decode_npc_motion_sample(record, type, &sample))
            continue;
        npc_motion_note_sent(sp,
                             sample.index,
                             sample.flags,
                             sample.pos,
                             sample.vel,
                             sample.angle,
                             world_tick);
    }
}

static void server_note_npc_pos_packet_sent(server_player_t *sp,
                                            const uint8_t *data,
                                            size_t len,
                                            uint32_t world_tick) {
    if (!sp || !data || len < NPC_POS_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_NPC_POS_Q)
        return;
    uint8_t count = data[1];
    if (len < NPC_POS_Q_MSG_HEADER + (size_t)count * NPC_POS_Q_RECORD_SIZE)
        return;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p =
            &data[NPC_POS_Q_MSG_HEADER + (size_t)i * NPC_POS_Q_RECORD_SIZE];
        uint8_t idx = p[0];
        if (idx >= MAX_NPC_SHIPS || sp->replication->npc_motion_sent_tick[idx] == 0u)
            continue;
        vec2 pos = v2((float)(int16_t)read_u16_le(&p[1]) *
                          NPC_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[3]) *
                          NPC_MOTION_Q_POS_SCALE);
        npc_motion_note_sent(sp,
                             idx,
                             sp->replication->npc_motion_sent_flags[idx],
                             pos,
                             sp->replication->npc_motion_sent_vel[idx],
                             sp->replication->npc_motion_sent_angle[idx],
                             world_tick);
    }
}

static void server_note_npc_pose_packet_sent(server_player_t *sp,
                                             const uint8_t *data,
                                             size_t len,
                                             uint32_t world_tick) {
    if (!sp || !data || len < NPC_POSE_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_NPC_POSE_Q)
        return;
    uint8_t count = data[1];
    if (len < NPC_POSE_Q_MSG_HEADER + (size_t)count * NPC_POSE_Q_RECORD_SIZE)
        return;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p =
            &data[NPC_POSE_Q_MSG_HEADER + (size_t)i * NPC_POSE_Q_RECORD_SIZE];
        uint8_t idx = p[0];
        if (idx >= MAX_NPC_SHIPS || sp->replication->npc_motion_sent_tick[idx] == 0u)
            continue;
        vec2 pos = v2((float)(int16_t)read_u16_le(&p[1]) *
                          NPC_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[3]) *
                          NPC_MOTION_Q_POS_SCALE);
        float angle = (float)read_u16_le(&p[5]) *
            NPC_MOTION_Q_ANGLE_SCALE;
        npc_motion_note_sent(sp,
                             idx,
                             sp->replication->npc_motion_sent_flags[idx],
                             pos,
                             sp->replication->npc_motion_sent_vel[idx],
                             angle,
                             world_tick);
    }
}

static void server_note_npc_linear_packet_sent(server_player_t *sp,
                                               const uint8_t *data,
                                               size_t len,
                                               uint32_t world_tick) {
    if (!sp || !data || len < NPC_LINEAR_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_NPC_LINEAR_Q)
        return;
    uint8_t count = data[1];
    if (len < NPC_LINEAR_Q_MSG_HEADER +
              (size_t)count * NPC_LINEAR_Q_RECORD_SIZE)
        return;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p =
            &data[NPC_LINEAR_Q_MSG_HEADER +
                  (size_t)i * NPC_LINEAR_Q_RECORD_SIZE];
        uint8_t idx = p[0];
        if (idx >= MAX_NPC_SHIPS || sp->replication->npc_motion_sent_tick[idx] == 0u)
            continue;
        vec2 pos = v2((float)(int16_t)read_u16_le(&p[1]) *
                          NPC_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[3]) *
                          NPC_MOTION_Q_POS_SCALE);
        vec2 vel = v2((float)(int16_t)read_u16_le(&p[5]) *
                          NPC_MOTION_Q_VEL_SCALE,
                      (float)(int16_t)read_u16_le(&p[7]) *
                          NPC_MOTION_Q_VEL_SCALE);
        npc_motion_note_sent(sp,
                             idx,
                             sp->replication->npc_motion_sent_flags[idx],
                             pos,
                             vel,
                             sp->replication->npc_motion_sent_angle[idx],
                             world_tick);
    }
}

static void server_note_npc_identity_packet_sent(server_player_t *sp,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 uint32_t world_tick) {
    if (!sp || !data || len < 2 || data[0] != NET_MSG_WORLD_NPCS)
        return;
    uint8_t count = data[1];
    size_t expected = 2 + (size_t)count * NPC_RECORD_SIZE;
    if (len < expected) return;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = &data[2 + (size_t)i * NPC_RECORD_SIZE];
        npc_motion_note_sent(sp,
                             p[0],
                             p[1],
                             v2(read_f32_le(&p[2]),
                                read_f32_le(&p[6])),
                             v2(read_f32_le(&p[10]),
                                read_f32_le(&p[14])),
                             read_f32_le(&p[18]),
                             world_tick);
    }
}

static bool ws_send_npc_motion_payload(struct mg_connection *c,
                                       server_player_t *sp,
                                       const uint8_t *data,
                                       size_t len,
                                       uint32_t world_tick) {
    if (!c || !sp || !data || len <= NPC_MOTION_MSG_HEADER) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
    } else {
        if (ws_defer_snapshot_if_backpressured(c, data, len))
            return false;
        if (!net_payload_cache_should_send(&sp->replication->world_npc_motion_cache,
                                           c, data, len)) {
            net_tx_record_suppressed(data, len);
            return false;
        }
        ws_send(c, data, len);
    }
    if (data[0] == NET_MSG_WORLD_NPC_POS_Q)
        server_note_npc_pos_packet_sent(sp, data, len, world_tick);
    else if (data[0] == NET_MSG_WORLD_NPC_POSE_Q)
        server_note_npc_pose_packet_sent(sp, data, len, world_tick);
    else if (data[0] == NET_MSG_WORLD_NPC_LINEAR_Q)
        server_note_npc_linear_packet_sent(sp, data, len, world_tick);
    else
        server_note_npc_motion_packet_sent(sp, data, len, world_tick);
    sp->replication->world_npc_motion_last_sent_tick = world_tick;
    return true;
}

static void ws_send_world_npc_motion_if_changed(struct mg_connection *c,
                                                server_player_t *sp,
                                                const uint8_t *data,
                                                size_t len,
                                                uint32_t world_tick) {
    if (!c || !sp || !data || len <= NPC_MOTION_MSG_HEADER) return;
    if (sp->replication->world_npcs_last_sent_tick == world_tick) return;
    if (sp->replication->world_npc_motion_last_sent_tick != 0 &&
        (uint32_t)(world_tick - sp->replication->world_npc_motion_last_sent_tick) <
            WORLD_NPC_MOTION_REPEAT_TICKS) {
        net_tx_record_suppressed(data, len);
        return;
    }
    uint8_t filtered[NPC_MOTION_MSG_HEADER +
                     MAX_NPC_SHIPS * NPC_MOTION_RECORD_SIZE];
    uint8_t pos_filtered[NPC_POS_Q_MSG_HEADER +
                         MAX_NPC_SHIPS * NPC_POS_Q_RECORD_SIZE];
    uint8_t pose_filtered[NPC_POSE_Q_MSG_HEADER +
                          MAX_NPC_SHIPS * NPC_POSE_Q_RECORD_SIZE];
    uint8_t linear_filtered[NPC_LINEAR_Q_MSG_HEADER +
                            MAX_NPC_SHIPS * NPC_LINEAR_Q_RECORD_SIZE];
    const uint8_t *send_data = data;
    size_t send_len = len;
    size_t pos_len = 0;
    size_t pose_len = 0;
    size_t linear_len = 0;
    if (data[0] == NET_MSG_WORLD_NPC_MOTION ||
        data[0] == NET_MSG_WORLD_NPC_MOTION_Q ||
        data[0] == NET_MSG_WORLD_NPC_MOTION8_Q) {
        send_len = server_filter_npc_motion_packet(
            sp, data, len, filtered, pos_filtered, &pos_len,
            pose_filtered, &pose_len, linear_filtered, &linear_len,
            world_tick);
        send_data = filtered;
        if (send_len <= NPC_MOTION_MSG_HEADER &&
            pos_len <= NPC_POS_Q_MSG_HEADER &&
            pose_len <= NPC_POSE_Q_MSG_HEADER &&
            linear_len <= NPC_LINEAR_Q_MSG_HEADER) {
            net_tx_record_suppressed(data, len);
            return;
        }
    }
    bool sent_any = false;
    if (pos_len > NPC_POS_Q_MSG_HEADER) {
        sent_any |= ws_send_npc_motion_payload(
            c, sp, pos_filtered, pos_len, world_tick);
    }
    if (pose_len > NPC_POSE_Q_MSG_HEADER) {
        sent_any |= ws_send_npc_motion_payload(
            c, sp, pose_filtered, pose_len, world_tick);
    }
    if (linear_len > NPC_LINEAR_Q_MSG_HEADER) {
        sent_any |= ws_send_npc_motion_payload(
            c, sp, linear_filtered, linear_len, world_tick);
    }
    if (send_len > NPC_MOTION_MSG_HEADER) {
        sent_any |= ws_send_npc_motion_payload(
            c, sp, send_data, send_len, world_tick);
    }
    if (!sent_any)
        net_tx_record_suppressed(data, len);
}

static void ws_send_world_npc_status_if_changed(struct mg_connection *c,
                                                server_player_t *sp,
                                                const uint8_t *data,
                                                size_t len,
                                                uint32_t world_tick) {
    if (!c || !sp || !data || len <= NPC_STATUS_MSG_HEADER) return;
    if (sp->replication->world_npcs_last_sent_tick == world_tick) return;
    if (sp->replication->world_npc_status_last_sent_tick != 0 &&
        (uint32_t)(world_tick - sp->replication->world_npc_status_last_sent_tick) <
            WORLD_NPC_STATUS_REPEAT_TICKS) {
        net_tx_record_suppressed(data, len);
        return;
    }
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        sp->replication->world_npc_status_last_sent_tick = world_tick;
        return;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return;
    uint64_t hash = net_world_npc_status_semantic_hash(data, (int)len);
    net_payload_cache_t *cache = &sp->replication->world_npc_status_cache;
    if (cache->valid &&
        cache->conn == c &&
        cache->len == (uint16_t)len &&
        cache->hash == hash) {
        net_tx_record_suppressed(data, len);
        return;
    }
    ws_send(c, data, len);
    cache->valid = true;
    cache->conn = c;
    cache->len = (uint16_t)len;
    cache->hash = hash;
    sp->replication->world_npc_status_last_sent_tick = world_tick;
}

static bool ws_send_world_interactions_if_changed(struct mg_connection *c,
                                                  server_player_t *sp,
                                                  const uint8_t *data,
                                                  size_t len,
                                                  uint32_t world_tick) {
    if (!c || !sp || !data) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        sp->replication->world_interactions_last_sent_tick = world_tick;
        return true;
    }
    uint64_t hash = net_world_interactions_semantic_hash(data, (int)len);
    bool semantic_changed = !sp->replication->world_interactions_semantic_valid ||
        sp->replication->world_interactions_semantic_hash != hash;
    bool refresh_due = interaction_net_metadata_refresh_due(
        sp->replication->world_interactions_last_sent_tick, world_tick);
    if (!semantic_changed && !refresh_due) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len)) {
        if (semantic_changed)
            sp->replication->world_interaction_drift_block_tick = world_tick;
        return false;
    }
    if (!net_payload_cache_should_send(&sp->replication->world_interactions_cache,
                                       c, data, len)) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    sp->replication->world_interactions_semantic_hash = hash;
    sp->replication->world_interactions_semantic_valid = true;
    sp->replication->world_interactions_last_sent_tick = world_tick;
    ws_send(c, data, len);
    return true;
}

static void ws_send_world_interaction_drift_if_changed(struct mg_connection *c,
                                                       server_player_t *sp,
                                                       const uint8_t *data,
                                                       size_t len,
                                                       uint32_t world_tick) {
    if (!c || !sp || !data || len <= INTERACTION_DRIFT_MSG_HEADER) return;
    if (!sp->replication->world_interactions_semantic_valid ||
        sp->replication->world_interactions_last_sent_tick == 0) {
        net_tx_record_suppressed(data, len);
        return;
    }
    if (sp->replication->world_interactions_last_sent_tick == world_tick ||
        sp->replication->world_interaction_drift_block_tick == world_tick) {
        return;
    }
    if (!interaction_drift_repeat_due(
            sp->replication->world_interaction_drift_last_sent_tick, world_tick)) {
        net_tx_record_suppressed(data, len);
        return;
    }
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        sp->replication->world_interaction_drift_last_sent_tick = world_tick;
        return;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return;
    if (!net_payload_cache_should_send(&sp->replication->world_interaction_drift_cache,
                                       c, data, len)) {
        net_tx_record_suppressed(data, len);
        return;
    }
    ws_send(c, data, len);
    sp->replication->world_interaction_drift_last_sent_tick = world_tick;
}

typedef struct {
    uint8_t index;
    vec2 pos;
    vec2 vel;
    float rotation;
} server_cargo_pod_motion_sample_t;

static bool server_decode_cargo_pod_motion_sample(
    const uint8_t *p,
    uint8_t type,
    server_cargo_pod_motion_sample_t *out) {
    if (!p || !out) return false;
    out->index = p[0];
    if (type == NET_MSG_WORLD_CARGO_POD_MOTION_Q) {
        const float two_pi = 6.28318530717958647692f;
        out->pos = v2((float)(int16_t)read_u16_le(&p[1]) *
                          CARGO_POD_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[3]) *
                          CARGO_POD_MOTION_Q_POS_SCALE);
        out->vel = v2((float)(int16_t)read_u16_le(&p[5]) *
                          CARGO_POD_MOTION_Q_VEL_SCALE,
                      (float)(int16_t)read_u16_le(&p[7]) *
                          CARGO_POD_MOTION_Q_VEL_SCALE);
        out->rotation = ((float)read_u16_le(&p[9]) / 65536.0f) * two_pi;
        return true;
    }
    if (type == NET_MSG_WORLD_CARGO_POD_MOTION) {
        out->pos = v2(read_f32_le(&p[1]), read_f32_le(&p[5]));
        out->vel = v2(read_f32_le(&p[9]), read_f32_le(&p[13]));
        out->rotation = read_f32_le(&p[17]);
        return true;
    }
    return false;
}

static size_t server_filter_cargo_pod_motion_packet(
    server_player_t *sp,
    const uint8_t *data,
    size_t len,
    uint8_t *out,
    uint8_t *linear_out,
    size_t *linear_len_out,
    uint32_t world_tick) {
    if (!sp || !data || !out || len < 2) return 0;
    if (linear_len_out) *linear_len_out = 0;
    uint8_t type = data[0];
    size_t header = 0;
    size_t record_size = 0;
    if (type == NET_MSG_WORLD_CARGO_POD_MOTION_Q) {
        header = CARGO_POD_MOTION_Q_MSG_HEADER;
        record_size = CARGO_POD_MOTION_Q_RECORD_SIZE;
    } else if (type == NET_MSG_WORLD_CARGO_POD_MOTION) {
        header = CARGO_POD_MOTION_MSG_HEADER;
        record_size = CARGO_POD_MOTION_RECORD_SIZE;
    } else {
        return 0;
    }
    if (len < header) return 0;
    uint8_t count = data[1];
    size_t expected = header + (size_t)count * record_size;
    if (len < expected) return 0;

    out[0] = type;
    out[1] = 0;
    uint8_t linear_count = 0;
    if (linear_out) {
        linear_out[0] = NET_MSG_WORLD_CARGO_POD_LINEAR_Q;
        linear_out[1] = 0;
    }
    uint8_t out_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *record = &data[header + (size_t)i * record_size];
        server_cargo_pod_motion_sample_t sample;
        if (!server_decode_cargo_pod_motion_sample(record, type, &sample))
            continue;
        if (!cargo_pod_motion_should_send(sp,
                                          sample.index,
                                          sample.pos,
                                          sample.vel,
                                          sample.rotation,
                                          world_tick))
            continue;
        if (linear_out &&
            cargo_pod_motion_linear_q_eligible(sp,
                                               sample.index,
                                               sample.rotation,
                                               world_tick)) {
            serialize_one_cargo_pod_linear_q(
                &linear_out[CARGO_POD_LINEAR_Q_MSG_HEADER +
                            (size_t)linear_count *
                                CARGO_POD_LINEAR_Q_RECORD_SIZE],
                sample.index,
                sample.pos,
                sample.vel);
            linear_count++;
            continue;
        }
        memcpy(&out[header + (size_t)out_count * record_size],
               record,
               record_size);
        out_count++;
    }
    out[1] = out_count;
    if (linear_out) {
        linear_out[1] = linear_count;
        if (linear_len_out) {
            *linear_len_out = CARGO_POD_LINEAR_Q_MSG_HEADER +
                (size_t)linear_count * CARGO_POD_LINEAR_Q_RECORD_SIZE;
        }
    }
    return header + (size_t)out_count * record_size;
}

static void server_note_cargo_pod_motion_packet_sent(server_player_t *sp,
                                                     const uint8_t *data,
                                                     size_t len,
                                                     uint32_t world_tick) {
    if (!sp || !data || len < 2) return;
    uint8_t type = data[0];
    size_t header = 0;
    size_t record_size = 0;
    if (type == NET_MSG_WORLD_CARGO_POD_MOTION_Q) {
        header = CARGO_POD_MOTION_Q_MSG_HEADER;
        record_size = CARGO_POD_MOTION_Q_RECORD_SIZE;
    } else if (type == NET_MSG_WORLD_CARGO_POD_MOTION) {
        header = CARGO_POD_MOTION_MSG_HEADER;
        record_size = CARGO_POD_MOTION_RECORD_SIZE;
    } else {
        return;
    }
    uint8_t count = data[1];
    if (len < header + (size_t)count * record_size) return;
    for (uint8_t i = 0; i < count; i++) {
        server_cargo_pod_motion_sample_t sample;
        const uint8_t *record = &data[header + (size_t)i * record_size];
        if (!server_decode_cargo_pod_motion_sample(record, type, &sample))
            continue;
        cargo_pod_motion_note_sent(sp,
                                   sample.index,
                                   sample.pos,
                                   sample.vel,
                                   sample.rotation,
                                   world_tick);
    }
}

static void server_note_cargo_pod_linear_packet_sent(server_player_t *sp,
                                                     const uint8_t *data,
                                                     size_t len,
                                                     uint32_t world_tick) {
    if (!sp || !data || len < CARGO_POD_LINEAR_Q_MSG_HEADER ||
        data[0] != NET_MSG_WORLD_CARGO_POD_LINEAR_Q)
        return;
    uint8_t count = data[1];
    if (len < CARGO_POD_LINEAR_Q_MSG_HEADER +
              (size_t)count * CARGO_POD_LINEAR_Q_RECORD_SIZE)
        return;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p =
            &data[CARGO_POD_LINEAR_Q_MSG_HEADER +
                  (size_t)i * CARGO_POD_LINEAR_Q_RECORD_SIZE];
        uint8_t idx = p[0];
        if (idx >= MAX_CARGO_PODS ||
            sp->replication->cargo_pod_motion_sent_tick[idx] == 0u)
            continue;
        vec2 pos = v2((float)(int16_t)read_u16_le(&p[1]) *
                          CARGO_POD_MOTION_Q_POS_SCALE,
                      (float)(int16_t)read_u16_le(&p[3]) *
                          CARGO_POD_MOTION_Q_POS_SCALE);
        vec2 vel = v2((float)(int16_t)read_u16_le(&p[5]) *
                          CARGO_POD_MOTION_Q_VEL_SCALE,
                      (float)(int16_t)read_u16_le(&p[7]) *
                          CARGO_POD_MOTION_Q_VEL_SCALE);
        cargo_pod_motion_note_sent(sp,
                                   idx,
                                   pos,
                                   vel,
                                   sp->replication->cargo_pod_motion_sent_rotation[idx],
                                   world_tick);
    }
}

static void server_note_cargo_pod_identity_packet_sent(server_player_t *sp,
                                                       const uint8_t *data,
                                                       size_t len,
                                                       uint32_t world_tick) {
    if (!sp || !data || len < 2 ||
        (data[0] != NET_MSG_WORLD_CARGO_PODS &&
         data[0] != NET_MSG_WORLD_CARGO_PODS_Q)) {
        return;
    }
    uint8_t count = data[1];
    bool compact = data[0] == NET_MSG_WORLD_CARGO_PODS_Q;
    size_t record_size = compact ? CARGO_POD_Q_RECORD_SIZE :
        CARGO_POD_RECORD_SIZE;
    size_t expected = 2 + (size_t)count * record_size;
    if (len < expected) return;
    const float two_pi = 6.28318530717958647692f;
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = &data[2 + (size_t)i * record_size];
        vec2 pos;
        vec2 vel;
        float rotation;
        if (compact) {
            pos = v2((float)(int16_t)read_u16_le(&p[4]) *
                         CARGO_POD_MOTION_Q_POS_SCALE,
                     (float)(int16_t)read_u16_le(&p[6]) *
                         CARGO_POD_MOTION_Q_POS_SCALE);
            vel = v2((float)(int16_t)read_u16_le(&p[8]) *
                         CARGO_POD_MOTION_Q_VEL_SCALE,
                     (float)(int16_t)read_u16_le(&p[10]) *
                         CARGO_POD_MOTION_Q_VEL_SCALE);
            rotation = ((float)read_u16_le(&p[16]) / 65536.0f) * two_pi;
        } else {
            pos = v2(read_f32_le(&p[4]), read_f32_le(&p[8]));
            vel = v2(read_f32_le(&p[12]), read_f32_le(&p[16]));
            rotation = read_f32_le(&p[24]);
        }
        cargo_pod_motion_note_sent(sp, p[0], pos, vel, rotation, world_tick);
    }
}

static bool ws_send_world_cargo_pods_if_changed(struct mg_connection *c,
                                                server_player_t *sp,
                                                const uint8_t *data,
                                                size_t len,
                                                uint32_t world_tick) {
    if (!c || !sp || !data) return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
        server_note_cargo_pod_identity_packet_sent(sp, data, len, world_tick);
        sp->replication->world_cargo_pods_last_sent_tick = world_tick;
        return true;
    }
    if (ws_defer_snapshot_if_backpressured(c, data, len))
        return false;
    uint64_t hash = net_world_cargo_pods_semantic_hash(data, (int)len);
    bool semantic_changed = !sp->replication->world_cargo_pods_semantic_valid ||
        sp->replication->world_cargo_pods_semantic_hash != hash;
    bool refresh_due = cargo_pod_net_metadata_refresh_due(
        sp->replication->world_cargo_pods_last_sent_tick, world_tick);
    if (!semantic_changed && !refresh_due) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    if (!net_payload_cache_should_send(&sp->replication->world_cargo_pods_cache,
                                       c, data, len)) {
        net_tx_record_suppressed(data, len);
        return false;
    }
    sp->replication->world_cargo_pods_semantic_hash = hash;
    sp->replication->world_cargo_pods_semantic_valid = true;
    sp->replication->world_cargo_pods_last_sent_tick = world_tick;
    ws_send(c, data, len);
    server_note_cargo_pod_identity_packet_sent(sp, data, len, world_tick);
    return true;
}

static bool ws_send_cargo_pod_motion_payload(struct mg_connection *c,
                                             server_player_t *sp,
                                             const uint8_t *data,
                                             size_t len,
                                             uint32_t world_tick) {
    if (!c || !sp || !data || len <= CARGO_POD_MOTION_MSG_HEADER)
        return false;
    if (len > UINT16_MAX) {
        ws_send(c, data, len);
    } else {
        if (ws_defer_snapshot_if_backpressured(c, data, len))
            return false;
        if (!net_payload_cache_should_send(&sp->replication->world_cargo_pod_motion_cache,
                                           c, data, len)) {
            net_tx_record_suppressed(data, len);
            return false;
        }
        ws_send(c, data, len);
    }
    if (data[0] == NET_MSG_WORLD_CARGO_POD_LINEAR_Q)
        server_note_cargo_pod_linear_packet_sent(sp, data, len, world_tick);
    else
        server_note_cargo_pod_motion_packet_sent(sp, data, len, world_tick);
    sp->replication->world_cargo_pod_motion_last_sent_tick = world_tick;
    return true;
}

static void ws_send_world_cargo_pod_motion_if_changed(struct mg_connection *c,
                                                      server_player_t *sp,
                                                      const uint8_t *data,
                                                      size_t len,
                                                      uint32_t world_tick) {
    if (!c || !sp || !data || len <= CARGO_POD_MOTION_MSG_HEADER) return;
    if (sp->replication->world_cargo_pods_last_sent_tick == world_tick) return;
    if (sp->replication->world_cargo_pod_motion_last_sent_tick != 0 &&
        (uint32_t)(world_tick - sp->replication->world_cargo_pod_motion_last_sent_tick) <
            WORLD_CARGO_POD_MOTION_REPEAT_TICKS) {
        net_tx_record_suppressed(data, len);
        return;
    }
    uint8_t filtered[CARGO_POD_MOTION_MSG_HEADER +
                     MAX_CARGO_PODS * CARGO_POD_MOTION_RECORD_SIZE];
    uint8_t linear_filtered[CARGO_POD_LINEAR_Q_MSG_HEADER +
                            MAX_CARGO_PODS * CARGO_POD_LINEAR_Q_RECORD_SIZE];
    const uint8_t *send_data = data;
    size_t send_len = len;
    size_t linear_len = 0;
    if (data[0] == NET_MSG_WORLD_CARGO_POD_MOTION ||
        data[0] == NET_MSG_WORLD_CARGO_POD_MOTION_Q) {
        send_len = server_filter_cargo_pod_motion_packet(
            sp, data, len, filtered, linear_filtered, &linear_len, world_tick);
        send_data = filtered;
        if (send_len <= CARGO_POD_MOTION_MSG_HEADER &&
            linear_len <= CARGO_POD_LINEAR_Q_MSG_HEADER) {
            net_tx_record_suppressed(data, len);
            return;
        }
    }
    bool sent_any = false;
    if (linear_len > CARGO_POD_LINEAR_Q_MSG_HEADER) {
        sent_any |= ws_send_cargo_pod_motion_payload(
            c, sp, linear_filtered, linear_len, world_tick);
    }
    if (send_len > CARGO_POD_MOTION_MSG_HEADER) {
        sent_any |= ws_send_cargo_pod_motion_payload(
            c, sp, send_data, send_len, world_tick);
    }
    if (!sent_any)
        net_tx_record_suppressed(data, len);
}

static void ws_world_packet_sink(void *user, const uint8_t *data, int len) {
    ws_private_packet_sink_t *sink = (ws_private_packet_sink_t *)user;
    if (!sink || !sink->conn || !sink->player || !data || len <= 0) return;
    if (data[0] == NET_MSG_WORLD_CARGO_PODS ||
        data[0] == NET_MSG_WORLD_CARGO_PODS_Q) {
        (void)ws_send_world_cargo_pods_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    if (data[0] == NET_MSG_WORLD_CARGO_POD_MOTION ||
        data[0] == NET_MSG_WORLD_CARGO_POD_MOTION_Q ||
        data[0] == NET_MSG_WORLD_CARGO_POD_LINEAR_Q) {
        ws_send_world_cargo_pod_motion_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    if (data[0] == NET_MSG_WORLD_INTERACTIONS ||
        data[0] == NET_MSG_WORLD_INTERACTIONS_Q) {
        (void)ws_send_world_interactions_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    if (data[0] == NET_MSG_WORLD_INTERACTION_DRIFT) {
        ws_send_world_interaction_drift_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    if (data[0] == NET_MSG_WORLD_NPCS) {
        ws_send_world_npcs_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    if (data[0] == NET_MSG_WORLD_NPC_MOTION ||
        data[0] == NET_MSG_WORLD_NPC_MOTION_Q ||
        data[0] == NET_MSG_WORLD_NPC_MOTION8_Q ||
        data[0] == NET_MSG_WORLD_NPC_POS_Q ||
        data[0] == NET_MSG_WORLD_NPC_POSE_Q ||
        data[0] == NET_MSG_WORLD_NPC_LINEAR_Q) {
        ws_send_world_npc_motion_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    if (data[0] == NET_MSG_WORLD_NPC_STATUS ||
        data[0] == NET_MSG_WORLD_NPC_STATUS8_Q) {
        ws_send_world_npc_status_if_changed(
            sink->conn, sink->player, data, (size_t)len, sink->world_tick);
        return;
    }
    net_payload_cache_t *cache = NULL;
    switch (data[0]) {
    case NET_MSG_WORLD_SCAFFOLDS:
        cache = &sink->player->replication->world_scaffolds_cache;
        break;
    case NET_MSG_WORLD_CARGO_PODS:
    case NET_MSG_WORLD_CARGO_PODS_Q:
        cache = &sink->player->replication->world_cargo_pods_cache;
        break;
    case NET_MSG_WORLD_INTERACTIONS:
    case NET_MSG_WORLD_INTERACTIONS_Q:
        cache = &sink->player->replication->world_interactions_cache;
        break;
    default:
        break;
    }
    if (ws_defer_snapshot_if_backpressured(sink->conn, data, (size_t)len))
        return;
    ws_send_if_changed(sink->conn, cache, data, (size_t)len);
}

static void ws_player_packet_sink(void *user, int player_slot,
                                  const uint8_t *data, int len) {
    (void)user;
    if (!data || len <= 0) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[player_slot];
    if (!server_player_is_gameplay_ready(sp) || !sp->connection->conn) return;
    ws_send(sp->connection->conn, data, (size_t)len);
}

static void broadcast_except(int exclude, const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude) continue;
        if (server_player_is_gameplay_ready(&world.players[i]) &&
            world.players[i].connection->conn)
            ws_send(world.players[i].connection->conn, data, len);
    }
}

static void broadcast_fracture_updates(void) {
    server_emit_fracture_updates(&world, -1, ws_player_packet_sink, NULL);
}

/* ------------------------------------------------------------------ */
/* WS message handler                                                 */
/* ------------------------------------------------------------------ */

/* Per-player WebSocket message rate limiting */
typedef struct {
    uint64_t window_start;
    int msg_count;
} ws_rate_bucket_t;

static ws_rate_bucket_t ws_rate[MAX_PLAYERS];
static ws_rate_bucket_t ws_ping_rate[MAX_PLAYERS];
#define WS_RATE_WINDOW_MS 1000
#define WS_RATE_LIMIT 140 /* 60Hz input + signed/plan bursts without drops */
#define WS_PING_RATE_LIMIT 6 /* Allows 4Hz recovery probes with frame jitter. */

static bool ws_rate_bucket_allow(ws_rate_bucket_t *bucket, uint64_t now,
                                 int limit) {
    if (!bucket || limit <= 0) return false;
    if (now - bucket->window_start > WS_RATE_WINDOW_MS) {
        bucket->window_start = now;
        bucket->msg_count = 0;
    }
    return ++bucket->msg_count <= limit;
}

static bool ws_message_allowed_before_session(uint8_t type) {
    switch (type) {
    case NET_MSG_LATENCY_PING:
    case NET_MSG_CLIENT_METRICS:
    case NET_MSG_REGISTER_PUBKEY:
    case NET_MSG_PROVE_PUBKEY:
    case NET_MSG_SESSION:
        return true;
    default:
        return false;
    }
}

static void finalize_verified_pubkey_identity(struct mg_connection *c, int pid,
                                              uint64_t now,
                                              bool preserve_live_state) {
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!server_player_can_use_pubkey_persistence(sp)) return;
    if (sp->pubkey_identity_finalized) return;

    const uint8_t *pk = sp->pubkey;
    bool transferred_live_state = false;
    int existing = registry_lookup_by_pubkey(&world, pk);
    if (existing >= 0 && existing != pid) {
        server_player_t *old = &world.players[existing];
        bool session_token_changed =
            memcmp(old->session_token, sp->session_token, 8) != 0;
        if (world_player_transfer_ship_state(&world, pid, existing)) {
            struct mg_connection *old_conn =
                (struct mg_connection *)old->connection->conn;
            if (session_token_changed) {
                uint8_t old_pseudo[32] = {0};
                uint8_t new_pseudo[32] = {0};
                memcpy(old_pseudo, old->session_token, 8);
                memcpy(new_pseudo, sp->session_token, 8);
                for (int s = 0; s < MAX_STATIONS; s++) {
                    station_t *st = &world.stations[s];
                    for (int e = 0; e < st->ledger_count; e++) {
                        if (memcmp(st->ledger[e].player_pubkey,
                                   old_pseudo, 32) == 0) {
                            memcpy(st->ledger[e].player_pubkey,
                                   new_pseudo, 32);
                        }
                    }
                }
            }

            old->connected = false;
            world_character_unbind_player(&world, existing);
            old->grace_period = false;
            old->connection->conn = NULL;
            server_player_clear_live_session_identity(old);
            server_player_clear_transient_input(old);
            if (old_conn && old_conn != c) {
                mg_ws_send(old_conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                old_conn->is_closing = 1;
            }
            uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)existing };
            broadcast(leave_old, 2);
            transferred_live_state = true;
            printf("[server] player %d: pubkey reconnect (was slot %d)\n",
                   pid, existing);
        }
    }

    (void)server_finalize_pubkey_identity(&world, pid);
    printf("[server] player %d: verified pubkey %02x%02x%02x%02x...\n",
           pid, pk[0], pk[1], pk[2], pk[3]);
    analytics_log_player_event("player_identity", pid, sp, now, 0);

    bool keep_live_state = preserve_live_state || transferred_live_state ||
                           sp->preserve_live_state_on_pubkey_finalize;
    sp->preserve_live_state_on_pubkey_finalize = false;

    if (keep_live_state) {
        server_player_reset_input_stream(sp);
        force_player_authoritative_resync(sp);
        printf("[server] player %d: kept live pubkey reconnect state\n", pid);
    } else if (player_load_by_pubkey(sp, &world, PLAYER_SAVE_DIR, pk)) {
        printf("[server] player %d: restored save by pubkey\n", pid);
    } else {
        char prefixes[LEGACY_SAVES_MAX_LIST][LEGACY_SAVES_PREFIX_LEN + 1];
        char names[LEGACY_SAVES_MAX_LIST][64];
        int n = player_save_list_legacy(PLAYER_SAVE_DIR, prefixes, names,
                                        LEGACY_SAVES_MAX_LIST);
        if (n > 0) {
            uint8_t buf[LEGACY_SAVES_HEADER +
                        LEGACY_SAVES_MAX_LIST * LEGACY_SAVES_PREFIX_LEN];
            buf[0] = NET_MSG_LEGACY_SAVES_AVAILABLE;
            buf[1] = (uint8_t)n;
            for (int i = 0; i < n; i++) {
                memcpy(&buf[LEGACY_SAVES_HEADER + i * LEGACY_SAVES_PREFIX_LEN],
                       prefixes[i], LEGACY_SAVES_PREFIX_LEN);
            }
            ws_send(c, buf,
                    (size_t)(LEGACY_SAVES_HEADER + n * LEGACY_SAVES_PREFIX_LEN));
            printf("[server] player %d: advertised %d legacy save(s)\n",
                   pid, n);
        }
    }
}

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    int pid = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].connection->conn == c) { pid = i; break; }
    }
    if (pid < 0) return;

    const uint8_t *data = (const uint8_t *)wm->data.buf;
    int len = (int)wm->data.len;
    if (len < 1) return;
    uint8_t type = data[0];

    /* Rate limit: silently drop excess messages. Latency probes have a
     * separate tiny bucket so input/action bursts cannot starve ping samples. */
    uint64_t now = mg_millis();
    if (type == NET_MSG_LATENCY_PING) {
        if (!ws_rate_bucket_allow(&ws_ping_rate[pid], now,
                                  WS_PING_RATE_LIMIT)) {
            return;
        }
    } else if (!ws_rate_bucket_allow(&ws_rate[pid], now, WS_RATE_LIMIT)) {
        return;
    }

    analytics_record_activity(&world.players[pid], now);
    if (!server_player_has_live_session(&world.players[pid]) &&
        !ws_message_allowed_before_session(type)) {
        return;
    }

    switch (type) {
    case NET_MSG_LATENCY_PING:
        if (len >= NET_LATENCY_PING_SIZE && c) {
            uint32_t seq = read_u32_le(&data[1]);
            uint32_t client_sent_ms = read_u32_le(&data[5]);
            send_latency_pong(c, seq, client_sent_ms, (uint32_t)now);
        }
        break;
    case NET_MSG_CLIENT_METRICS:
        analytics_handle_client_metrics(pid, &world.players[pid], data, len, now);
        break;
    case NET_MSG_INPUT:
    {
        server_player_t *sp = &world.players[pid];
        server_input_dispatch_result_t input_result;
        if (!server_dispatch_input_message(&world, pid, data, len,
                                           (uint32_t)now,
                                           &input_result)) {
            break;
        }
        if (input_result.rejected_unsigned_action) {
            unsigned_action_count++;
        }
        if (input_result.ack_status == NET_ACTION_ACK_RECEIVED &&
            input_result.action_id != 0) {
            server_begin_pending_action_result(&world, sp,
                                               input_result.action_id,
                                               input_result.input_seq,
                                               input_result.action);
        }
        server_merge_one_shot_input(&sp->input, &input_result.intent);
        /* Movement-only input acks ride private INPUT_APPLIED/STATE receipts.
         * ACTION_ACK is only for one-shot actions or rejections. */
        if (c) {
            if (input_result.ack_status != 0) {
                send_action_ack(c, input_result.action_id,
                                input_result.input_seq,
                                input_result.ack_status,
                                input_result.action);
                if (input_result.force_authoritative_resync) {
                    force_player_authoritative_resync(sp);
                    printf("[server] action-result player=%d id=%u input_seq=%u action=%u status=rejected tick=%u resync=unsigned-reject\n",
                           sp->id, (unsigned)input_result.action_id,
                           (unsigned)input_result.input_seq,
                           (unsigned)input_result.action,
                           (unsigned)world.tick);
                    send_action_result(c, input_result.action_id,
                                       input_result.input_seq,
                                       NET_ACTION_RESULT_REJECTED,
                                       input_result.action, world.tick);
                }
            }
        }
        /* If the player just queued a shipyard order, refresh that station's
         * identity on the next world tick so the SHIPYARD tab sees the new
         * pending count immediately instead of waiting for the 2s fallback. */
        if (input_result.station_identity_dirty >= 0 &&
            input_result.station_identity_dirty < MAX_STATIONS)
            station_identity_dirty[input_result.station_identity_dirty] = true;
        break;
    }
    case NET_MSG_PLAN:
    {
        server_unsigned_dispatch_result_t result;
        if (server_dispatch_legacy_plan_message(&world, pid, data, len,
                                                &result) &&
            result.rejected_unsigned_action) {
            unsigned_action_count++;
            printf("[server] legacy unsigned PLAN rejected for pubkey player %d; signed action required\n",
                   pid);
        }
        break;
    }
    case NET_MSG_STATE:
        /* Ignored -- server is authoritative. */
        break;
    case NET_MSG_MINING_ACTION:
        /* Legacy -- mining handled via INPUT flags now. */
        break;
    case NET_MSG_BUY_INGOT:
    {
        server_legacy_cargo_dispatch_result_t result;
        if (server_dispatch_legacy_buy_ingot_message(
                &world, pid, data, len, ws_cargo_receipt_chain_sink,
                c, &result) &&
            result.rejected_unsigned_action) {
            unsigned_action_count++;
            printf("[server] legacy unsigned BUY_INGOT rejected for pubkey player %d; signed action required\n",
                   pid);
        }
        break;
    }
    case NET_MSG_DELIVER_INGOT:
    {
        server_legacy_cargo_dispatch_result_t result;
        if (server_dispatch_legacy_deliver_ingot_message(
                &world, pid, data, len, ws_cargo_receipt_chain_sink,
                c, &result) &&
            result.rejected_unsigned_action) {
            unsigned_action_count++;
            printf("[server] legacy unsigned DELIVER_INGOT rejected for pubkey player %d; signed action required\n",
                   pid);
        }
        break;
    }
    case NET_MSG_PRESENT_RECEIPT_CHAIN:
    {
        server_receipt_presentation_dispatch_result_t result;
        if (server_dispatch_receipt_presentation_message(
                &world, pid, data, len, &result) &&
            result.evaluated &&
            result.result != CARGO_RECEIPT_PRESENT_OK) {
                printf("[server] receipt_present rejected player=%d reason=%s\n",
                       pid, cargo_receipt_present_result_name(result.result));
        }
        break;
    }
    case NET_MSG_HANDOFF_REQUEST:
        (void)server_dispatch_handoff_request(&world, pid, data, len,
                                              ws_handoff_ticket_sink, c);
        break;
    case NET_MSG_HANDOFF_PRESENT:
        (void)server_dispatch_handoff_present(&world, pid, data, len,
                                              ws_handoff_result_sink, c);
        break;
    case NET_MSG_FRACTURE_CLAIM:
    {
        server_unsigned_dispatch_result_t result;
        if (server_dispatch_fracture_claim_message(&world, pid, data, len,
                                                   &result) &&
            result.rejected_unsigned_action) {
            unsigned_action_count++;
        }
        break;
    }
    case NET_MSG_SIGNED_ACTION: {
        /* Layer A.3 of #479 — Ed25519-signed state-changing action. */
        uint8_t action_type = 0;
        uint64_t nonce = 0;
        const uint8_t *payload = NULL;
        uint16_t payload_len = 0;
        signed_action_result_t res = signed_action_verify(
            &world, pid, data, len,
            &action_type, &nonce, &payload, &payload_len);
        if (res != SIGNED_ACTION_OK) {
            signed_action_reject_count++;
            const char *reason = "unknown";
            switch (res) {
            case SIGNED_ACTION_REJECT_NO_PUBKEY:    reason = "no-pubkey"; break;
            case SIGNED_ACTION_REJECT_MALFORMED:    reason = "malformed"; break;
            case SIGNED_ACTION_REJECT_BAD_SIG:      reason = "bad-sig";   break;
            case SIGNED_ACTION_REJECT_REPLAY:       reason = "replay";    break;
            case SIGNED_ACTION_REJECT_UNKNOWN_TYPE: reason = "unk-type";  break;
            default: break;
            }
            const uint8_t *pk = world.players[pid].pubkey;
            printf("[server] signed-action rejected (%s) from player %d pk=%02x%02x%02x%02x...\n",
                   reason, pid, pk[0], pk[1], pk[2], pk[3]);
            break;
        }
        /* Verified — commit the nonce high-water mark BEFORE dispatch
         * so a faulting handler can't clear the replay protection. */
        world.players[pid].last_signed_nonce = nonce;
        signed_action_count++;
        server_signed_action_dispatch_result_t dispatch_result;
        if (server_dispatch_signed_action_payload(
                &world, pid, action_type, payload, payload_len,
                ws_cargo_receipt_chain_sink, c, &dispatch_result) &&
            dispatch_result.station_identity_dirty >= 0 &&
            dispatch_result.station_identity_dirty < MAX_STATIONS) {
            station_identity_dirty[dispatch_result.station_identity_dirty] =
                true;
        }
        break;
    }
    case NET_MSG_REGISTER_PUBKEY:
        /* Layer A.2 of #479: client asserts its persisted Ed25519 pubkey.
         * The assertion alone does not bind registry or persistence; that
         * waits for NET_MSG_PROVE_PUBKEY. */
    {
        server_pubkey_register_result_t result;
        if (server_dispatch_register_pubkey_message(&world, pid, data, len,
                                                    &result)) {
            if (result.same_pubkey) {
                finalize_verified_pubkey_identity(c, pid, now, false);
                break;
            }
            printf("[server] player %d: registered pubkey pending proof %02x%02x%02x%02x...\n",
                   pid, result.pubkey[0], result.pubkey[1],
                   result.pubkey[2], result.pubkey[3]);
        }
        break;
    }
    case NET_MSG_PROVE_PUBKEY:
    {
        server_pubkey_proof_result_t result;
        if (server_dispatch_pubkey_proof_message(&world, pid, data, len,
                                                 &result) &&
            result.verified) {
            finalize_verified_pubkey_identity(c, pid, now, false);
        } else if (result.status != SERVER_PUBKEY_PROOF_MALFORMED) {
            printf("[server] player %d: pubkey proof rejected (%s)\n",
                   pid, server_pubkey_proof_status_name(result.status));
        }
        break;
    }
    case NET_MSG_CLAIM_LEGACY_SAVE: {
        /* Layer A.4 of #479. Client supplies (token_hex, signature). We
         * verify sig against the registered pubkey, then rename the
         * legacy save to the pubkey-keyed path and load it. */
        if (len < 2) break;
        server_player_t *sp = &world.players[pid];
        if (!server_player_can_use_pubkey_persistence(sp)) break;
        uint8_t hex_len = data[1];
        if (hex_len == 0 || hex_len > 64) break;
        if (len < (int)(2 + hex_len + SIGNED_ACTION_SIG_SIZE)) break;
        const uint8_t *hex = &data[2];
        const uint8_t *sig = &data[2 + hex_len];

        /* Reject any non-hex byte to keep the basename safe for filesystem. */
        for (int i = 0; i < hex_len; i++) {
            uint8_t ch = hex[i];
            bool digit = (ch >= '0' && ch <= '9');
            bool lower = (ch >= 'a' && ch <= 'f');
            bool upper = (ch >= 'A' && ch <= 'F');
            if (!digit && !lower && !upper) {
                printf("[server] player %d: claim rejected (bad hex)\n", pid);
                goto claim_done;
            }
        }

        /* Reconstruct the signed message: domain || token_hex. */
        const char *domain = CLAIM_LEGACY_SAVE_DOMAIN;
        size_t dlen = strlen(domain);
        uint8_t msg[64 + 64];
        if (dlen + hex_len > sizeof(msg)) goto claim_done;
        memcpy(msg, domain, dlen);
        memcpy(msg + dlen, hex, hex_len);
        if (!signal_crypto_verify(sig, msg, dlen + hex_len, sp->pubkey)) {
            printf("[server] player %d: claim signature invalid\n", pid);
            goto claim_done;
        }

        /* The wire format carries the full token base name *without*
         * the "player_" prefix or the .sav suffix; legacy saves on disk
         * use either the "player_<hex>" form (token-keyed) or
         * "player_<slot>" form (anonymous slot fallback). Accept either:
         * try the literal name first, then with the historical prefix. */
        char basename[80];
        if ((size_t)hex_len + 1 > sizeof(basename)) goto claim_done;
        memcpy(basename, hex, hex_len);
        basename[hex_len] = '\0';

        bool ok = player_save_rename_legacy_to_pubkey(PLAYER_SAVE_DIR,
                                                       basename, sp->pubkey);
        const char *audit_name = basename;
        char prefixed[96];
        if (!ok) {
            snprintf(prefixed, sizeof(prefixed), "player_%s", basename);
            ok = player_save_rename_legacy_to_pubkey(PLAYER_SAVE_DIR,
                                                     prefixed, sp->pubkey);
            if (ok) audit_name = prefixed;
        }
        (void)player_save_audit_legacy_claim(PLAYER_SAVE_DIR, audit_name,
                                             sp->pubkey, ok,
                                             ok ? "renamed" : "missing-or-raced");
        if (!ok) {
            printf("[server] player %d: claim rename failed (race / missing)\n", pid);
            goto claim_done;
        }
        if (player_load_by_pubkey(sp, &world, PLAYER_SAVE_DIR, sp->pubkey)) {
            printf("[server] player %d: claimed legacy save\n", pid);
        }
    claim_done:
        break;
    }
    case NET_MSG_SESSION:
    {
        server_session_message_t session;
        if (server_parse_session_message(data, len, &session) &&
            !world.players[pid].session_ready) {
            const uint8_t *token = session.token;
            /* Extract callsign if present (bytes 9-15) */
            if (session.has_callsign) {
                printf("[server] player %d callsign: %s\n", pid, session.callsign);
            }
            /* Reattach a previous slot with the same browser token. Most
             * reconnects arrive after the old slot entered grace, but a page
             * reload can send SESSION before WebRTC/WebSocket close reaches
             * the server. Treat that active duplicate as a handoff too. */
            int reattach = server_find_session_reattach_slot(&world, pid, token);
            bool reattached_live_state = false;
            if (reattach >= 0) {
                /* Reattach: copy state from grace slot to new slot */
                server_player_t *old = &world.players[reattach];
                struct mg_connection *old_conn =
                    (struct mg_connection *)old->connection->conn;
                if (!world_player_transfer_ship_state(&world, pid, reattach))
                    break;
                if (!server_apply_session_message(&world, pid, &session))
                    break;
                /* Clear the grace slot and broadcast LEAVE so clients drop the ghost */
                old->connected = false;
                world_character_unbind_player(&world, reattach);
                old->grace_period = false;
                old->connection->conn = NULL;
                server_player_clear_live_session_identity(old);
                server_player_clear_transient_input(old);
                if (old_conn && old_conn != c) {
                    mg_ws_send(old_conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                    old_conn->is_closing = 1;
                }
                uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)reattach };
                broadcast(leave_old, 2);
                printf("[server] player %d: reconnected (was slot %d)\n", pid, reattach);
                reattached_live_state = true;
                world.players[pid].preserve_live_state_on_pubkey_finalize = true;
            } else {
                if (!server_apply_session_message(&world, pid, &session))
                    break;
                /* Try to restore saved state keyed by session token */
                if (true &&
                    player_load_by_token(&world.players[pid], &world,
                                         PLAYER_SAVE_DIR, token)) {
                    printf("[server] player %d: restored save by session\n", pid);
                } else {
                    printf("[server] player %d: no save for session, fresh ship\n", pid);
                }
                /* Seed starting credits now that session_token is set */
                player_seed_credits(&world.players[pid], &world);
            }
            finalize_verified_pubkey_identity(c, pid, now,
                                              reattached_live_state);
            server_player_t *ready_sp = &world.players[pid];
            server_player_reset_input_stream(ready_sp);
            invalidate_player_authoritative_caches(ready_sp);
            (void)server_emit_authoritative_player_state_snapshot(
                ready_sp, (uint8_t)pid, world.tick, ws_packet_sink, c);
            ready_sp->replication->force_authoritative_resync = false;
            ready_sp->pending_action_result_valid = false;
            send_initial_world_bundle_to_player(c, pid);
            uint8_t join_msg[] = { NET_MSG_JOIN, (uint8_t)pid };
            broadcast_except(pid, join_msg, 2);
            analytics_record_activity(&world.players[pid], now);
            analytics_log_player_event("player_session", pid, &world.players[pid],
                                       now, 0);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Station REST API                                                   */
/* ------------------------------------------------------------------ */

static const char *api_token = NULL;

static bool api_auth_ok(struct mg_http_message *hm) {
    if (!api_token || api_token[0] == '\0') return false;
    struct mg_str *auth = mg_http_get_header(hm, "Authorization");
    if (!auth) return false;
    /* Expect "Bearer <token>" */
    if (auth->len < 8) return false;
    const char *prefix = "Bearer ";
    if (strncmp(auth->buf, prefix, 7) != 0) return false;
    return strncmp(auth->buf + 7, api_token, auth->len - 7) == 0
        && strlen(api_token) == auth->len - 7;
}

static bool internal_auth_ok(struct mg_http_message *hm) {
    if (!internal_token || internal_token[0] == '\0') return false;
    struct mg_str *auth = mg_http_get_header(hm, "X-Internal-Token");
    if (!auth) return false;
    return strncmp(auth->buf, internal_token, auth->len) == 0
        && strlen(internal_token) == auth->len;
}

/* Validate that a byte sequence is valid UTF-8. */
static bool is_valid_utf8(const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i];
        if (c < 0x80) {
            /* ASCII */
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (i + 1 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if (i + 2 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if ((data[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            if (i + 3 >= len) return false;
            if ((data[i + 1] & 0xC0) != 0x80) return false;
            if ((data[i + 2] & 0xC0) != 0x80) return false;
            if ((data[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

static int parse_station_id(struct mg_http_message *hm) {
    /* Extract station index from /api/station/<id>/... */
    /* URI looks like /api/station/0/state or /api/station/2/command */
    const char *p = hm->uri.buf + 13; /* skip "/api/station/" */
    const char *end = hm->uri.buf + hm->uri.len;
    if (p >= end) return -1;
    /* Length-safe integer parse: only read digits up to next '/' or URI end */
    int id = 0;
    bool has_digit = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_digit = true;
        id = id * 10 + (*p - '0');
        if (id >= MAX_STATIONS) return -1;
        p++;
    }
    if (!has_digit) return -1;
    if (id < 0 || id >= MAX_STATIONS) return -1;
    if (!station_exists(&world.stations[id])) return -1;
    return id;
}

static void station_mutation_mark_identity(int station_idx) {
    if (station_idx >= 0 && station_idx < MAX_STATIONS)
        station_identity_dirty[station_idx] = true;
}

static void station_mutation_mark_contracts(void) {
    contracts_dirty = true;
}

static float station_mutation_price_baseline(int station_idx, long commodity,
                                             double requested) {
    station_t *st = &world.stations[station_idx];
    uint32_t station_id = st->id;
    if (station_price_anchor_station_id[station_idx] != station_id) {
        memset(station_price_anchor_valid[station_idx], 0,
               sizeof(station_price_anchor_valid[station_idx]));
        station_price_anchor_station_id[station_idx] = station_id;
    }

    int c = (int)commodity;
    if (!station_price_anchor_valid[station_idx][c]) {
        float baseline = st->base_price[c];
        if (!(baseline > 0.0f))
            baseline = (float)requested;
        if (!(baseline > 0.0f))
            baseline = 1.0f;
        station_price_anchor[station_idx][c] = baseline;
        station_price_anchor_valid[station_idx][c] = true;
    }
    return station_price_anchor[station_idx][c];
}

static size_t operator_post_field_cap(uint8_t kind) {
    switch ((operator_post_kind_t)kind) {
    case OPERATOR_POST_MINER_CHATTER:
    case OPERATOR_POST_HAULER_CHATTER:
        return sizeof(world.stations[0].miner_chatter[0]);
    case OPERATOR_POST_HAIL_MOTD:
        return sizeof(world.stations[0].hail_message);
    case OPERATOR_POST_RATI_DELIVERY:
        return sizeof(world.stations[0].rati_hail_message);
    default:
        return 257; /* chain payload cap + NUL; no materialized field */
    }
}

static void materialize_operator_post(station_t *st, uint8_t kind,
                                      uint16_t ref_id, const char *text) {
    switch ((operator_post_kind_t)kind) {
    case OPERATOR_POST_HAIL_MOTD:
        snprintf(st->hail_message, sizeof(st->hail_message), "%s", text);
        break;
    case OPERATOR_POST_MINER_CHATTER:
        if (ref_id < STATION_IDENTITY_CHATTER_LINES)
            snprintf(st->miner_chatter[ref_id], sizeof(st->miner_chatter[ref_id]), "%s", text);
        break;
    case OPERATOR_POST_HAULER_CHATTER:
        if (ref_id < STATION_IDENTITY_CHATTER_LINES)
            snprintf(st->hauler_chatter[ref_id], sizeof(st->hauler_chatter[ref_id]), "%s", text);
        break;
    case OPERATOR_POST_RATI_DELIVERY:
        snprintf(st->rati_hail_message, sizeof(st->rati_hail_message), "%s", text);
        break;
    default:
        break;
    }
}

static bool emit_operator_post_for_station(int station_idx, uint8_t kind,
                                           uint8_t tier, uint16_t ref_id,
                                           const char *text,
                                           uint64_t *out_event_id,
                                           const char **out_error) {
    if (out_event_id) *out_event_id = 0;
    if (out_error) *out_error = NULL;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) {
        if (out_error) *out_error = "invalid station_index";
        return false;
    }
    if (!station_exists(&world.stations[station_idx])) {
        if (out_error) *out_error = "station not found";
        return false;
    }
    if (!text || text[0] == '\0') {
        if (out_error) *out_error = "text missing";
        return false;
    }
    size_t text_len = strlen(text);
    if (text_len > 256) {
        if (out_error) *out_error = "text too long";
        return false;
    }
    size_t field_cap = operator_post_field_cap(kind);
    if (text_len >= field_cap) {
        if (out_error) *out_error = "text too long for target field";
        return false;
    }
    if ((kind == OPERATOR_POST_MINER_CHATTER ||
         kind == OPERATOR_POST_HAULER_CHATTER) &&
        ref_id >= STATION_IDENTITY_CHATTER_LINES) {
        if (out_error) *out_error = "invalid chatter slot";
        return false;
    }
    if (!is_valid_utf8((const uint8_t *)text, text_len)) {
        if (out_error) *out_error = "invalid utf-8";
        return false;
    }

    uint8_t payload[38 + 256];
    memset(payload, 0, sizeof(payload));
    payload[0] = kind;
    payload[1] = tier;
    payload[2] = (uint8_t)(ref_id & 0xFFu);
    payload[3] = (uint8_t)((ref_id >> 8) & 0xFFu);
    sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
    payload[36] = (uint8_t)(text_len & 0xFFu);
    payload[37] = (uint8_t)((text_len >> 8) & 0xFFu);
    memcpy(&payload[38], text, text_len);

    station_t *st = &world.stations[station_idx];
    uint64_t event_id = chain_log_emit(&world, st, CHAIN_EVT_OPERATOR_POST,
                                       payload, (uint16_t)(38 + text_len));
    if (event_id == 0) {
        if (out_error) *out_error = "failed to emit event";
        return false;
    }

    materialize_operator_post(st, kind, ref_id, text);
    station_mutation_mark_identity(station_idx);
    if (out_event_id) *out_event_id = event_id;
    return true;
}

static bool station_mutation_operator_text(int station_idx,
                                           operator_post_kind_t kind,
                                           long slot,
                                           const char *text,
                                           uint64_t *out_event_id,
                                           const char **out_error) {
    uint16_t ref_id = 0;
    if (kind == OPERATOR_POST_MINER_CHATTER ||
        kind == OPERATOR_POST_HAULER_CHATTER) {
        if (slot < 0 || slot >= STATION_IDENTITY_CHATTER_LINES) {
            if (out_event_id) *out_event_id = 0;
            if (out_error) *out_error = "invalid chatter slot";
            return false;
        }
        ref_id = (uint16_t)slot;
    }
    return emit_operator_post_for_station(station_idx, (uint8_t)kind, 0,
                                          ref_id, text,
                                          out_event_id, out_error);
}

static bool station_mutation_set_currency_name(int station_idx,
                                               const char *currency,
                                               char *out_value,
                                               size_t out_cap,
                                               const char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_value && out_cap > 0) out_value[0] = '\0';
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&world.stations[station_idx])) {
        if (out_error) *out_error = "station not found";
        return false;
    }
    if (!currency || currency[0] == '\0') {
        if (out_error) *out_error = "currency_name missing";
        return false;
    }

    /* ASCII-ish trim; drop anything that would mess with the HUD
     * renderer (control chars, quotes). 31 chars max so the wire
     * serializer's null terminator survives. */
    char sanitized[32] = {0};
    int out = 0;
    for (int i = 0; currency[i] && out < (int)sizeof(sanitized) - 1; i++) {
        unsigned char ch = (unsigned char)currency[i];
        if (ch < 0x20 || ch == 0x7F || ch == '"' || ch == '\\') continue;
        sanitized[out++] = (char)ch;
    }
    if (out == 0) {
        if (out_error) *out_error = "currency_name empty after sanitize";
        return false;
    }

    memcpy(world.stations[station_idx].currency_name, sanitized, sizeof(sanitized));
    station_mutation_mark_identity(station_idx);
    if (out_value && out_cap > 0)
        snprintf(out_value, out_cap, "%s", sanitized);
    return true;
}

static bool parse_query_long(const char *text, long *out) {
    char *tail = NULL;
    if (!text || !out) return false;
    errno = 0;
    long v = strtol(text, &tail, 10);
    if (errno != 0 || tail == text || *tail != '\0') return false;
    *out = v;
    return true;
}

static bool station_mutation_set_price(int station_idx, long commodity,
                                       double price_val, float *out_price,
                                       const char **out_error) {
    if (out_error) *out_error = NULL;
    if (out_price) *out_price = 0.0f;
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&world.stations[station_idx])) {
        if (out_error) *out_error = "station not found";
        return false;
    }
    if (commodity < 0 || commodity >= COMMODITY_COUNT) {
        if (out_error) *out_error = "invalid commodity";
        return false;
    }
    if (!(price_val > 0.0) || !isfinite(price_val) || price_val > FLT_MAX) {
        if (out_error) *out_error = "invalid price";
        return false;
    }

    station_t *st = &world.stations[station_idx];
    float baseline = station_mutation_price_baseline(station_idx, commodity,
                                                     price_val);
    float clamped = station_clamp_operator_price((float)price_val, baseline);
    st->base_price[commodity] = clamped;
    station_mutation_mark_identity(station_idx);
    if (out_price) *out_price = clamped;
    return true;
}

static bool station_mutation_build_module(int station_idx, long module_type,
                                          const char **out_error) {
    if (out_error) *out_error = NULL;
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&world.stations[station_idx])) {
        if (out_error) *out_error = "station not found";
        return false;
    }
    if (module_type < 0 || module_type >= MODULE_COUNT) {
        if (out_error) *out_error = "invalid module_type";
        return false;
    }

    station_t *st = &world.stations[station_idx];
    if (st->module_count >= MAX_MODULES_PER_STATION) {
        if (out_error) *out_error = "station full";
        return false;
    }

    int before_count = st->module_count;
    begin_module_construction(&world, st, station_idx, (module_type_t)module_type);
    if (st->module_count <= before_count) {
        if (out_error) *out_error = "no valid module slot";
        return false;
    }
    station_mutation_mark_identity(station_idx);
    station_mutation_mark_contracts();
    return true;
}

/* Append a JSON-escaped string to buf at *pos, respecting bufsz.
 * Escapes quotes, backslashes, and control characters. */
static void json_escape_append(char *buf, int *pos, int bufsz, const char *s) {
    int p = *pos;
    if (p >= bufsz - 1) return;
    for (; *s && p < bufsz - 1; s++) {
        char esc = 0;
        switch (*s) {
            case '"':  esc = '"';  break;
            case '\\': esc = '\\'; break;
            case '\n': esc = 'n';  break;
            case '\r': esc = 'r';  break;
            case '\t': esc = 't';  break;
        }
        if (esc) {
            if (p + 2 > bufsz - 1) break;
            buf[p++] = '\\';
            buf[p++] = esc;
        } else if ((unsigned char)*s < 0x20) {
            /* Other control chars: emit as \u00XX */
            if (p + 6 > bufsz - 1) break;
            p += snprintf(buf + p, (size_t)(bufsz - p), "\\u%04x", (unsigned char)*s);
        } else {
            buf[p++] = *s;
        }
    }
    *pos = p;
}

/* Safe snprintf append: clamp pos to bufsz before snprintf to avoid undefined behavior */
#define BUF_APPEND(pos, buf, bufsz, ...) do { \
    if ((pos) < (bufsz)) { \
        int _n = snprintf((buf) + (pos), (size_t)((bufsz) - (pos)), __VA_ARGS__); \
        if (_n > 0) (pos) += _n; \
        if ((pos) > (bufsz)) (pos) = (bufsz); \
    } \
} while (0)

static void append_callsign_json(char *buf, int *pos, int bufsz,
                                 const char callsign[8]) {
    char cs[9];
    memcpy(cs, callsign, 8);
    cs[8] = '\0';
    for (int k = 7; k >= 0 && (cs[k] == ' ' || cs[k] == '\0'); k--)
        cs[k] = '\0';
    json_escape_append(buf, pos, bufsz, cs);
}

static int collect_route_history_api_aggregates(
    route_history_aggregate_row_t *out,
    int cap)
{
    if (!out || cap <= 0) return 0;
    memset(out, 0, (size_t)cap * sizeof(out[0]));

    for (int si = 0; si < MAX_STATIONS; si++) {
        chain_route_history_tail_t tail[16];
        int count = chain_log_read_route_history_tail(&world.stations[si],
                                                      tail, 16);
        for (int i = 0; i < count; i++) {
            const chain_payload_route_history_t *p = &tail[i].payload;
            route_history_aggregate_add_fields(out,
                                               cap,
                                               p->memory_kind,
                                               p->origin_station,
                                               p->destination_station,
                                               p->commodity,
                                               p->action,
                                               p->evidence_count,
                                               p->confidence,
                                               p->salience,
                                               p->observed_tick);
        }
    }

    return route_history_aggregate_sort(out, cap);
}

static const char *route_history_api_filter_name(uint8_t filter)
{
    switch (filter) {
    case 1:  return "outbound";
    case 2:  return "inbound";
    case 3:  return "local";
    case 0:
    default: return "all";
    }
}

static uint8_t route_history_api_parse_filter(const char *s)
{
    if (!s || !*s) return 0;
    if (strcmp(s, "outbound") == 0 || strcmp(s, "origin") == 0) return 1;
    if (strcmp(s, "inbound") == 0 || strcmp(s, "destination") == 0) return 2;
    if (strcmp(s, "local") == 0 || strcmp(s, "events") == 0) return 3;
    return 0;
}

static bool route_history_api_filter_matches_aggregate(
    const route_history_aggregate_row_t *row,
    uint8_t filter,
    int station_idx)
{
    if (!row || !row->used) return false;
    switch (filter) {
    case 1: return station_idx >= 0 && row->origin_station == (uint8_t)station_idx;
    case 2: return station_idx >= 0 && row->destination_station == (uint8_t)station_idx;
    case 3: return false;
    case 0:
    default: return true;
    }
}

static void reply_bot_trace_weights(struct mg_connection *c) {
    enum { BUFSZ = 16384 };
    char *buf = (char *)malloc(BUFSZ);
    if (!buf) {
        mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
        return;
    }

    float reference = highscore_trace_reference_score(&highscores);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *sp = &world.players[i];
        if (!sp->connected) continue;
        if (sp->server_brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT) continue;
        if (isfinite(sp->ship->stat_credits_earned) &&
            sp->ship->stat_credits_earned > reference) {
            reference = sp->ship->stat_credits_earned;
        }
    }
    if (reference < 1.0f) reference = 1.0f;

    int pos = 0;
    BUF_APPEND(pos, buf, BUFSZ,
               "{\"schema\":\"signal.bot_trace_weights.v1\","
               "\"basis\":\"max(highscore,active_run_credits)\","
               "\"floor\":%.3f,\"ceiling\":%.3f,"
               "\"reference_credits\":%.3f,\"entries\":[",
               highscore_trace_weight_floor(),
               highscore_trace_weight_ceiling(),
               reference);
    bool first = true;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const server_player_t *sp = &world.players[i];
        if (!sp->connected) continue;
        if (sp->server_brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT) continue;

        char cs[9];
        memcpy(cs, sp->callsign, 8);
        cs[8] = '\0';
        for (int k = 7; k >= 0 && (cs[k] == ' ' || cs[k] == '\0'); k--)
            cs[k] = '\0';

        int rank = highscore_find_rank(&highscores, cs);
        float highscore_credits = rank >= 0 ? highscores.entries[rank].credits_earned : 0.0f;
        float highscore_weight = rank >= 0
            ? highscore_trace_weight_from_score(highscore_credits, reference)
            : highscore_trace_weight_floor();
        float active_credits = sp->ship->stat_credits_earned;
        float active_weight = highscore_trace_weight_from_score(active_credits, reference);
        float trace_weight = active_weight > highscore_weight
            ? active_weight
            : highscore_weight;
        const char *source = active_weight > highscore_weight ? "active_run" :
            (rank >= 0 ? "highscore" : "floor");

        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
                   "{\"player_id\":%d,\"callsign\":\"", i);
        append_callsign_json(buf, &pos, BUFSZ, sp->callsign);
        BUF_APPEND(pos, buf, BUFSZ,
                   "\",\"trace_weight\":%.3f,\"source\":\"%s\","
                   "\"highscore_rank\":%d,\"highscore_credits\":%.3f,"
                   "\"active_credits\":%.3f}",
                   trace_weight, source, rank >= 0 ? rank + 1 : 0,
                   highscore_credits, active_credits);
    }
    BUF_APPEND(pos, buf, BUFSZ, "]}");
    mg_http_reply(c, 200, api_headers, "%s", buf);
    free(buf);
}

static void reply_station_policy_trace(struct mg_connection *c) {
    enum { BUFSZ = 131072 };
    char *buf = (char *)malloc(BUFSZ);
    if (!buf) {
        mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
        return;
    }

    int pos = 0;
    BUF_APPEND(pos, buf, BUFSZ,
               "{\"schema\":\"signal.station_policy_trace.v1\","
               "\"tick\":%u,\"stations\":[",
               world.tick);
    bool first_station = true;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &world.stations[s];
        if (!station_exists(st)) continue;
        station_policy_refresh(st, s, world.tick);

        if (!first_station) BUF_APPEND(pos, buf, BUFSZ, ",");
        first_station = false;
        BUF_APPEND(pos, buf, BUFSZ,
                   "{\"station\":%d,\"name\":\"", s);
        json_escape_append(buf, &pos, BUFSZ, st->name);
        commodity_t top = (commodity_t)st->policy_top_demand_commodity;
        BUF_APPEND(pos, buf, BUFSZ,
                   "\",\"generation\":%u,\"policy_tick\":%llu,"
                   "\"budget\":{\"trade\":%u,\"construction\":%u,\"finance\":%u},"
                   "\"top_demand\":{\"commodity\":%u,\"code\":\"%s\","
                   "\"severity\":%.3f,\"price_mult\":%.3f},"
                   "\"cards\":[",
                   st->policy_generation,
                   (unsigned long long)st->policy_tick,
                   st->policy_budget_trade,
                   st->policy_budget_construction,
                   st->policy_budget_finance,
                   (unsigned)top,
                   commodity_code(top),
                   st->policy_top_demand_severity,
                   st->policy_top_demand_price_mult);
        for (int i = 0; i < st->policy_card_count &&
                        i < STATION_POLICY_MAX_ACTIVE_CARDS; i++) {
            station_policy_card_id_t id =
                (station_policy_card_id_t)st->policy_card_ids[i];
            station_policy_domain_t domain =
                (station_policy_domain_t)st->policy_card_domains[i];
            if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ,
                       "{\"id\":%u,\"name\":\"",
                       (unsigned)id);
            json_escape_append(buf, &pos, BUFSZ, station_policy_card_name(id));
            BUF_APPEND(pos, buf, BUFSZ,
                       "\",\"domain\":\"%s\",\"score\":%.3f,\"budget_cost\":%u}",
                       station_policy_domain_name(domain),
                       st->policy_card_scores[i],
                       st->policy_card_costs[i]);
        }
        BUF_APPEND(pos, buf, BUFSZ, "],\"price_modifiers\":[");
        bool first_mod = true;
        for (int cidx = 0; cidx < COMMODITY_COUNT; cidx++) {
            commodity_t cmod = (commodity_t)cidx;
            float mult = station_policy_trade_price_multiplier(st, cmod);
            if (fabsf(mult - 1.0f) <= 0.001f) continue;
            if (!first_mod) BUF_APPEND(pos, buf, BUFSZ, ",");
            first_mod = false;
            BUF_APPEND(pos, buf, BUFSZ,
                       "{\"commodity\":%d,\"code\":\"%s\",\"mult\":%.3f}",
                       cidx, commodity_code(cmod), mult);
        }
        BUF_APPEND(pos, buf, BUFSZ, "]}");
    }
    BUF_APPEND(pos, buf, BUFSZ, "]}");
    mg_http_reply(c, 200, api_headers, "%s", buf);
    free(buf);
}

typedef struct {
    uint64_t event_id;
    uint8_t kind;
    uint8_t tier;
    uint16_t ref_id;
    char text[257];
} operator_post_tail_t;

static const char *operator_post_kind_name(uint8_t kind) {
    switch (kind) {
    case OPERATOR_POST_HAIL_MOTD:       return "hail_motd";
    case OPERATOR_POST_CONTRACT_FLAVOR: return "contract_flavor";
    case OPERATOR_POST_RARITY_TIER:     return "rarity_tier";
    case OPERATOR_POST_BUILD_INFO:      return "build_info";
    case OPERATOR_POST_WORLD_INFO:      return "world_info";
    case OPERATOR_POST_MINER_CHATTER:   return "miner_chatter";
    case OPERATOR_POST_HAULER_CHATTER:  return "hauler_chatter";
    case OPERATOR_POST_RATI_DELIVERY:   return "rati_delivery";
    default:                            return "unknown";
    }
}

static int read_operator_post_tail(const station_t *st,
                                   operator_post_tail_t out[16]) {
    if (!st || !out) return 0;
    char path[256];
    if (!chain_log_path_for(st->station_pubkey, path, sizeof(path))) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    int count = 0;
    while (!feof(f)) {
        chain_event_header_t hdr;
        uint16_t plen = 0;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (fread(&plen, sizeof(plen), 1, f) != 1) break;
        if (hdr.type == CHAIN_EVT_OPERATOR_POST && plen >= 38) {
            uint8_t prefix[38];
            if (fread(prefix, sizeof(prefix), 1, f) != 1) break;
            uint16_t text_len = (uint16_t)(prefix[36] | ((uint16_t)prefix[37] << 8));
            uint16_t body_len = (uint16_t)(plen - 38);
            if (text_len > body_len) text_len = body_len;
            if (text_len > 256) text_len = 256;

            operator_post_tail_t item = {
                .event_id = hdr.event_id,
                .kind = prefix[0],
                .tier = prefix[1],
                .ref_id = (uint16_t)(prefix[2] | ((uint16_t)prefix[3] << 8)),
            };
            if (text_len > 0) {
                if (fread(item.text, 1, text_len, f) != text_len) break;
                item.text[text_len] = '\0';
            }
            if (body_len > text_len)
                fseek(f, (long)(body_len - text_len), SEEK_CUR);

            out[count % 16] = item;
            count++;
        } else {
            fseek(f, plen, SEEK_CUR);
        }
    }
    fclose(f);
    int n = count < 16 ? count : 16;
    if (count > 16) {
        operator_post_tail_t ordered[16];
        int start = count % 16;
        for (int i = 0; i < 16; i++)
            ordered[i] = out[(start + i) % 16];
        memcpy(out, ordered, sizeof(ordered));
    }
    return n;
}

/* Cap visible_asteroids in the agent-facing JSON. Populated stations
 * at 15-18k signal range can see 1000+ rocks once chunk gen has run,
 * and at ~80 bytes/record that explodes past any reasonable response
 * buffer. Agents don't need 1000 rocks — the nearest N is plenty. */
#define STATION_API_MAX_ASTEROIDS 150
/* Safety margin left in the buffer after the asteroid loop so the
 * trailing players/stations/contracts/hail fields and their braces
 * always fit and the JSON closes cleanly. */
#define STATION_API_TAIL_MARGIN   2048

typedef struct {
    int index;
    float dist_sq;
    const asteroid_t *asteroid;
} station_api_ore_target_t;

static void station_api_offer_ore_target(station_api_ore_target_t *targets,
                                         int cap,
                                         int index,
                                         const asteroid_t *asteroid,
                                         float dist_sq) {
    if (!targets || cap <= 0 || !asteroid) return;
    for (int i = 0; i < cap; i++) {
        if (targets[i].index >= 0 && dist_sq >= targets[i].dist_sq) continue;
        for (int j = cap - 1; j > i; j--)
            targets[j] = targets[j - 1];
        targets[i].index = index;
        targets[i].dist_sq = dist_sq;
        targets[i].asteroid = asteroid;
        return;
    }
}

static void handle_station_state(struct mg_connection *c, int sid, struct mg_http_message *hm) {
    const station_t *st = &world.stations[sid];

    /* Parse query params */
    int include_activity = 0;
    int include_chain_history = 0;
    uint8_t route_history_filter = 0;
    char tmp[96];
    if (hm && mg_http_get_var(&hm->query, "include", tmp, sizeof(tmp)) > 0) {
        include_activity = strstr(tmp, "activity_history") != NULL || strcmp(tmp, "all") == 0;
        include_chain_history = strstr(tmp, "chain_history") != NULL || strcmp(tmp, "all") == 0;
    }
    if (hm && mg_http_get_var(&hm->query, "history_filter",
                              tmp, sizeof(tmp)) > 0) {
        route_history_filter = route_history_api_parse_filter(tmp);
    }
    /* Heap-allocated so we aren't bound by the event-loop thread's
     * stack (alpine musl main stack is ~80KB by default). */
    enum { BUFSZ = 131072 };
    char *buf = (char *)malloc(BUFSZ);
    if (!buf) {
        mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
        return;
    }
    int pos = 0;

    /* Station info */
    BUF_APPEND(pos, buf, BUFSZ,
        "{\"station\":{\"index\":%d,\"name\":\"", sid);
    json_escape_append(buf, &pos, BUFSZ, st->name);
    BUF_APPEND(pos, buf, BUFSZ,
        "\",\"signal_range\":%.1f,\"scaffold\":%s,"
        "\"chain_event_count\":%llu,\"station_pubkey\":\"",
        st->signal_range, st->scaffold ? "true" : "false",
        (unsigned long long)st->chain_event_count);
    for (int k = 0; k < 32; k++) BUF_APPEND(pos, buf, BUFSZ, "%02x", st->station_pubkey[k]);
    BUF_APPEND(pos, buf, BUFSZ, "\",\"chain_last_hash\":\"");
    for (int k = 0; k < 32; k++) BUF_APPEND(pos, buf, BUFSZ, "%02x", st->chain_last_hash[k]);
    BUF_APPEND(pos, buf, BUFSZ,
        "\",\"chain_health\":\"%s\",\"chain_append_blocked\":%s,"
        "\"chain_verified_event_count\":%llu,\"chain_verified_last_hash\":\"",
        chain_log_health_status_name((chain_health_status_t)st->chain_health_status),
        st->chain_append_blocked ? "true" : "false",
        (unsigned long long)st->chain_verified_event_count);
    for (int k = 0; k < 32; k++) BUF_APPEND(pos, buf, BUFSZ, "%02x", st->chain_verified_last_hash[k]);
    BUF_APPEND(pos, buf, BUFSZ, "\",\"chain_health_message\":\"");
    json_escape_append(buf, &pos, BUFSZ, st->chain_health_message);
    BUF_APPEND(pos, buf, BUFSZ, "\",\"chain_repair_hint\":\"");
    json_escape_append(buf, &pos, BUFSZ,
                       chain_log_health_repair_hint(
                           (chain_health_status_t)st->chain_health_status,
                           st->chain_append_blocked));
    BUF_APPEND(pos, buf, BUFSZ, "\",\"inventory\":{");

    static const char *cnames[] = {
        "ferrite_ore","cuprite_ore","crystal_ore",
        "ferrite_ingot","cuprite_ingot","crystal_ingot",
        "frame","laser_module","tractor_module",
        "repair_kit",
    };
    _Static_assert(sizeof(cnames)/sizeof(cnames[0]) == COMMODITY_COUNT,
                   "cnames must stay in sync with commodity_t");
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "\"%s\":%.1f", cnames[i],
            station_inventory_amount(st, (commodity_t)i));
    }
    BUF_APPEND(pos, buf, BUFSZ, "},\"modules\":[");
    for (int m = 0; m < st->module_count; m++) {
        if (m > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"type\":\"%s\",\"ring\":%d,\"slot\":%d,\"scaffold\":%s,\"progress\":%.2f}",
            module_type_name(st->modules[m].type),
            st->modules[m].ring, st->modules[m].slot,
            st->modules[m].scaffold ? "true" : "false",
            st->modules[m].build_progress);
    }

    BUF_APPEND(pos, buf, BUFSZ, "]},");

    float sr_sq = st->signal_range * st->signal_range;
    int asteroid_commodity_counts[COMMODITY_RAW_ORE_COUNT] = {0};
    int asteroid_fragment_counts[COMMODITY_RAW_ORE_COUNT] = {0};
    enum { ORE_TARGETS_PER_COMMODITY = 3 };
    station_api_ore_target_t ore_targets[COMMODITY_RAW_ORE_COUNT][ORE_TARGETS_PER_COMMODITY];
    for (int commodity_index = 0; commodity_index < COMMODITY_RAW_ORE_COUNT; commodity_index++) {
        for (int j = 0; j < ORE_TARGETS_PER_COMMODITY; j++) {
            ore_targets[commodity_index][j].index = -1;
            ore_targets[commodity_index][j].dist_sq = FLT_MAX;
            ore_targets[commodity_index][j].asteroid = NULL;
        }
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &world.asteroids[i];
        if (!a->active) continue;
        float d_sq = v2_dist_sq(a->pos, st->pos);
        if (d_sq > sr_sq) continue;
        if (a->commodity < COMMODITY_RAW_ORE_COUNT) {
            asteroid_commodity_counts[a->commodity]++;
            if (a->tier == ASTEROID_TIER_S)
                asteroid_fragment_counts[a->commodity]++;
            else
                station_api_offer_ore_target(ore_targets[a->commodity],
                                             ORE_TARGETS_PER_COMMODITY,
                                             i, a, d_sq);
        }
    }
    BUF_APPEND(pos, buf, BUFSZ,
        "\"asteroid_counts\":{\"ferrite\":%d,\"cuprite\":%d,\"crystal\":%d,"
        "\"fragments\":{\"ferrite\":%d,\"cuprite\":%d,\"crystal\":%d}},",
        asteroid_commodity_counts[COMMODITY_FERRITE_ORE],
        asteroid_commodity_counts[COMMODITY_CUPRITE_ORE],
        asteroid_commodity_counts[COMMODITY_CRYSTAL_ORE],
        asteroid_fragment_counts[COMMODITY_FERRITE_ORE],
        asteroid_fragment_counts[COMMODITY_CUPRITE_ORE],
        asteroid_fragment_counts[COMMODITY_CRYSTAL_ORE]);

    BUF_APPEND(pos, buf, BUFSZ, "\"ore_targets\":[");
    for (int commodity_index = 0; commodity_index < COMMODITY_RAW_ORE_COUNT; commodity_index++) {
        if (commodity_index > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"commodity\":%d,\"need\":%.3f,\"visible\":%d,\"fragments\":%d,\"nearest\":[",
            commodity_index, station_raw_ore_need_score(st, (commodity_t)commodity_index),
            asteroid_commodity_counts[commodity_index],
            asteroid_fragment_counts[commodity_index]);
        for (int j = 0; j < ORE_TARGETS_PER_COMMODITY; j++) {
            const station_api_ore_target_t *target = &ore_targets[commodity_index][j];
            const asteroid_t *a = target->asteroid;
            if (target->index < 0 || !a) break;
            if (j > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ,
                "{\"index\":%d,\"tier\":%d,\"x\":%.0f,\"y\":%.0f,"
                "\"distance\":%.0f,\"hp\":%.0f}",
                target->index, a->tier, a->pos.x, a->pos.y,
                fixp_sqrtf(target->dist_sq), a->hp);
        }
        BUF_APPEND(pos, buf, BUFSZ, "]}");
    }
    BUF_APPEND(pos, buf, BUFSZ, "],");

    /* Visible asteroids within signal range. Capped at
     * STATION_API_MAX_ASTEROIDS — agents don't need 1000+ rocks and
     * serializing them all blew past 32KB and truncated the tail of
     * the JSON mid-field (prod bug, April 2026). */
    BUF_APPEND(pos, buf, BUFSZ, "\"visible_asteroids\":[");
    bool first = true;
    int asteroid_count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &world.asteroids[i];
        if (!a->active) continue;
        if (v2_dist_sq(a->pos, st->pos) > sr_sq) continue;
        if (asteroid_count >= STATION_API_MAX_ASTEROIDS) break;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"tier\":%d,\"commodity\":%d,\"x\":%.0f,\"y\":%.0f,\"hp\":%.0f}",
            i, a->tier, a->commodity, a->pos.x, a->pos.y, a->hp);
        asteroid_count++;
        if (pos > BUFSZ - STATION_API_TAIL_MARGIN) break;
    }

    /* Visible players within signal range */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_players\":[");
    first = true;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!server_player_is_gameplay_ready(&world.players[i])) continue;
        if (v2_dist_sq(world.players[i].ship->pos, st->pos) > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"id\":%d,\"x\":%.0f,\"y\":%.0f,\"docked\":%s}",
            i, world.players[i].ship->pos.x, world.players[i].ship->pos.y,
            world.players[i].docked ? "true" : "false");
    }

    /* Visible NPCs within signal range. Kept compact for operator/debug
     * reads: role/state are enum ordinals matching shared/types.h. */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_npcs\":[");
    first = true;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &world.npc_ships[i];
        if (!npc->active) continue;
        if (v2_dist_sq(npc->ship->pos, st->pos) > sr_sq) continue;
        float cargo_total = ship_total_cargo(npc->ship);
        const asteroid_t *target = NULL;
        if (npc->target_asteroid >= 0 && npc->target_asteroid < MAX_ASTEROIDS) {
            const asteroid_t *candidate = &world.asteroids[npc->target_asteroid];
            if (candidate->active) target = candidate;
        }
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"slot\":%d,\"role\":%d,\"state\":%d,\"home\":%d,\"dest\":%d,"
            "\"target\":%d,\"target_commodity\":%d,\"towed\":%d,"
            "\"cargo\":%.0f,\"x\":%.1f,\"y\":%.1f,\"vx\":%.2f,\"vy\":%.2f}",
            i, npc->role, npc->state, npc->home_station, npc->dest_station,
            npc->target_asteroid, target ? (int)target->commodity : -1,
            npc_towed_fragment_index(npc), cargo_total,
            npc->ship->pos.x, npc->ship->pos.y, npc->ship->vel.x, npc->ship->vel.y);
    }

    /* Visible stations within signal range */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_stations\":[");
    first = true;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (i == sid || !station_exists(&world.stations[i])) continue;
        float d_sq = v2_dist_sq(world.stations[i].pos, st->pos);
        if (d_sq > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        float overlap = st->signal_range + world.stations[i].signal_range - fixp_sqrtf(d_sq);
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"name\":\"", i);
        json_escape_append(buf, &pos, BUFSZ, world.stations[i].name);
        BUF_APPEND(pos, buf, BUFSZ,
            "\",\"x\":%.0f,\"y\":%.0f,\"signal_overlap\":%s}",
            world.stations[i].pos.x, world.stations[i].pos.y,
            overlap > 0.0f ? "true" : "false");
    }

    /* Active contracts */
    BUF_APPEND(pos, buf, BUFSZ, "],\"active_contracts\":[");
    first = true;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        const contract_t *ct = &world.contracts[i];
        if (!ct->active || ct->station_index != sid) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"action\":%d,\"commodity\":%d,\"quantity\":%.0f,\"base_price\":%.1f,\"age\":%.0f}",
            i, ct->action, ct->commodity, ct->quantity_needed, ct->base_price, ct->age);
    }

    /* Top N most-recent dockers (relationships, #257) — bounded for prompt context.
     * Sort by last_dock_tick DESC to surface recent visitors first. */
    BUF_APPEND(pos, buf, BUFSZ, "],\"relationships\":[");
    enum { MAX_RELATIONSHIPS_IN_API = 8 };
    /* Find indices with non-zero last_dock_tick, sort by tick descending */
    int rel_indices[16];
    int rel_count = 0;
    for (int i = 0; i < st->ledger_count; i++) {
        if (st->ledger[i].last_dock_tick > 0) {
            rel_indices[rel_count++] = i;
        }
    }
    /* Simple sort — bubble sort for small N */
    for (int i = 0; i < rel_count - 1; i++) {
        for (int j = 0; j < rel_count - 1 - i; j++) {
            if (st->ledger[rel_indices[j]].last_dock_tick < st->ledger[rel_indices[j+1]].last_dock_tick) {
                int swap = rel_indices[j];
                rel_indices[j] = rel_indices[j+1];
                rel_indices[j+1] = swap;
            }
        }
    }
    first = true;
    int rel_output = 0;
    for (int i = 0; i < rel_count && rel_output < MAX_RELATIONSHIPS_IN_API; i++) {
        int idx = rel_indices[i];
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"pubkey\":\"");
        /* Encode pubkey as hex for JSON */
        for (int j = 0; j < 32; j++)
            BUF_APPEND(pos, buf, BUFSZ, "%02x", st->ledger[idx].player_pubkey[j]);
        BUF_APPEND(pos, buf, BUFSZ,
            "\",\"first_dock_tick\":%llu,\"last_dock_tick\":%llu,"
            "\"total_docks\":%u,\"lifetime_ore_units\":%u,"
            "\"lifetime_credits_in\":%u,\"lifetime_credits_out\":%u,"
            "\"top_commodity\":%u}",
            (unsigned long long)st->ledger[idx].first_dock_tick,
            (unsigned long long)st->ledger[idx].last_dock_tick,
            st->ledger[idx].total_docks,
            st->ledger[idx].lifetime_ore_units,
            st->ledger[idx].lifetime_credits_in,
            st->ledger[idx].lifetime_credits_out,
            st->ledger[idx].top_commodity);
        rel_output++;
        if (pos > BUFSZ - STATION_API_TAIL_MARGIN) break;
    }

    /* Close the "relationships" array opened above. The original
     * single-call form ("],\"hail\":...") got split across the optional
     * activity_history block; the close now fires unconditionally so the
     * JSON stays well-formed regardless of include_activity. */
    BUF_APPEND(pos, buf, BUFSZ, "]");

    /* Activity history (24-hour window, if requested) */
    if (include_activity) {
        double window_start = world.time - 86400.0;
        double ore_sum = 0.0;
        int recent_docks = 0;

        for (int i = 0; i < st->ledger_count; i++) {
            if (st->ledger[i].last_dock_tick > window_start) {
                ore_sum += st->ledger[i].lifetime_ore_units;
                recent_docks++;
            }
        }

        /* Top haulers: up to 3 players by lifetime ore contributed */
        int top_indices[3] = {-1, -1, -1};
        for (int i = 0; i < st->ledger_count; i++) {
            for (int j = 0; j < 3; j++) {
                if (top_indices[j] < 0 ||
                    st->ledger[i].lifetime_ore_units > st->ledger[top_indices[j]].lifetime_ore_units) {
                    /* Shift down */
                    for (int k = 2; k > j; k--) top_indices[k] = top_indices[k-1];
                    top_indices[j] = i;
                    break;
                }
            }
        }

        BUF_APPEND(pos, buf, BUFSZ, ",\"activity_history\":{\"ore_processed_24h\":%.0f,"
            "\"ships_docked_24h\":%d,\"top_haulers\":[",
            ore_sum, recent_docks);

        for (int j = 0; j < 3; j++) {
            if (top_indices[j] < 0) break;
            if (j > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ, "\"");
            for (int k = 0; k < 32; k++) {
                BUF_APPEND(pos, buf, BUFSZ, "%02x", st->ledger[top_indices[j]].player_pubkey[k]);
            }
            BUF_APPEND(pos, buf, BUFSZ, "\"");
        }

        BUF_APPEND(pos, buf, BUFSZ, "]}");
    }

    /* Recent signed station-operator content, bounded for avatar prompt context.
     * This is a context feed, not a verifier; authority still lives in the
     * append-only chain log and its signed event records. */
    if (include_chain_history) {
        operator_post_tail_t posts[16];
        int post_count = read_operator_post_tail(st, posts);
        chain_route_history_tail_t route_history[16];
        int route_count = chain_log_read_route_history_tail(st, route_history,
                                                            16);
        BUF_APPEND(pos, buf, BUFSZ,
                   ",\"chain_history\":{\"history_filter\":\"%s\","
                   "\"operator_posts\":[",
                   route_history_api_filter_name(route_history_filter));
        for (int i = 0; i < post_count; i++) {
            if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ,
                "{\"event_id\":%llu,\"kind\":%u,\"kind_name\":\"%s\","
                "\"tier\":%u,\"ref_id\":%u,\"text\":\"",
                (unsigned long long)posts[i].event_id,
                (unsigned)posts[i].kind,
                operator_post_kind_name(posts[i].kind),
                (unsigned)posts[i].tier,
                (unsigned)posts[i].ref_id);
            json_escape_append(buf, &pos, BUFSZ, posts[i].text);
            BUF_APPEND(pos, buf, BUFSZ, "\"}");
        }
        BUF_APPEND(pos, buf, BUFSZ, "],\"route_history_aggregate\":[");
        route_history_aggregate_row_t aggregate[8];
        int aggregate_count = collect_route_history_api_aggregates(
            aggregate, (int)(sizeof(aggregate) / sizeof(aggregate[0])));
        int aggregate_written = 0;
        for (int i = 0; i < aggregate_count; i++) {
            const route_history_aggregate_row_t *row = &aggregate[i];
            if (!row->used) continue;
            if (!route_history_api_filter_matches_aggregate(
                    row, route_history_filter, sid)) {
                continue;
            }
            char origin_name[24];
            char destination_name[24];
            char title[96];
            char evidence[112];
            char freshness[96];
            route_history_station_label(row->origin_station,
                                        origin_name, sizeof(origin_name));
            route_history_station_label(row->destination_station,
                                        destination_name,
                                        sizeof(destination_name));
            route_history_aggregate_fields(row->memory_kind,
                                           row->origin_station,
                                           row->destination_station,
                                           row->commodity,
                                           row->action,
                                           row->event_count,
                                           row->evidence_sum,
                                           row->confidence_peak,
                                           row->salience_peak,
                                           row->latest_tick,
                                           title, sizeof(title),
                                           evidence, sizeof(evidence),
                                           freshness, sizeof(freshness));
            if (aggregate_written++ > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ,
                "{\"memory_kind\":%u,\"memory_kind_name\":\"%s\","
                "\"origin_station\":%u,\"origin_station_name\":\"",
                (unsigned)row->memory_kind,
                route_history_memory_kind_label(row->memory_kind),
                (unsigned)row->origin_station);
            json_escape_append(buf, &pos, BUFSZ, origin_name);
            BUF_APPEND(pos, buf, BUFSZ,
                "\",\"destination_station\":%u,"
                "\"destination_station_name\":\"",
                (unsigned)row->destination_station);
            json_escape_append(buf, &pos, BUFSZ, destination_name);
            BUF_APPEND(pos, buf, BUFSZ,
                "\",\"commodity\":%u,\"commodity_code\":\"%s\","
                "\"action\":%u,\"action_name\":\"%s\","
                "\"signed_row_count\":%u,\"evidence_sum\":%u,"
                "\"confidence_peak\":%u,\"salience_peak\":%u,"
                "\"latest_tick\":%u,\"title\":\"",
                (unsigned)row->commodity,
                row->commodity < COMMODITY_COUNT
                    ? commodity_code((commodity_t)row->commodity)
                    : "UNK",
                (unsigned)row->action,
                route_history_action_label(row->action),
                (unsigned)row->event_count,
                (unsigned)row->evidence_sum,
                (unsigned)row->confidence_peak,
                (unsigned)row->salience_peak,
                (unsigned)row->latest_tick);
            json_escape_append(buf, &pos, BUFSZ, title);
            BUF_APPEND(pos, buf, BUFSZ, "\",\"evidence\":\"");
            json_escape_append(buf, &pos, BUFSZ, evidence);
            BUF_APPEND(pos, buf, BUFSZ, "\",\"freshness\":\"");
            json_escape_append(buf, &pos, BUFSZ, freshness);
            BUF_APPEND(pos, buf, BUFSZ, "\"}");
        }
        BUF_APPEND(pos, buf, BUFSZ, "],\"route_history\":[");
        for (int i = 0; i < route_count; i++) {
            const chain_route_history_tail_t *row = &route_history[i];
            const chain_payload_route_history_t *p = &row->payload;
            char origin_name[24];
            char destination_name[24];
            char summary[160];
            route_history_station_label(p->origin_station,
                                        origin_name, sizeof(origin_name));
            route_history_station_label(p->destination_station,
                                        destination_name, sizeof(destination_name));
            route_history_summary_fields(p->memory_kind,
                                         p->origin_station,
                                         p->destination_station,
                                         p->commodity,
                                         p->action,
                                         p->evidence_count,
                                         p->confidence,
                                         summary, sizeof(summary));
            if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
            BUF_APPEND(pos, buf, BUFSZ,
                "{\"event_id\":%llu,\"epoch\":%llu,"
                "\"memory_kind\":%u,\"memory_kind_name\":\"%s\","
                "\"origin_station\":%u,\"origin_station_name\":\"",
                (unsigned long long)row->event_id,
                (unsigned long long)row->epoch,
                (unsigned)p->memory_kind,
                route_history_memory_kind_label(p->memory_kind),
                (unsigned)p->origin_station);
            json_escape_append(buf, &pos, BUFSZ, origin_name);
            BUF_APPEND(pos, buf, BUFSZ,
                "\",\"destination_station\":%u,"
                "\"destination_station_name\":\"",
                (unsigned)p->destination_station);
            json_escape_append(buf, &pos, BUFSZ, destination_name);
            BUF_APPEND(pos, buf, BUFSZ,
                "\",\"commodity\":%u,\"commodity_code\":\"%s\","
                "\"action\":%u,\"action_name\":\"%s\","
                "\"confidence\":%u,\"salience\":%u,"
                "\"evidence_count\":%u,\"value_hint\":%u,"
                "\"observed_tick\":%u,\"subject_nonce\":%llu,"
                "\"summary\":\"",
                (unsigned)p->commodity,
                p->commodity < COMMODITY_COUNT
                    ? commodity_code((commodity_t)p->commodity)
                    : "UNK",
                (unsigned)p->action,
                route_history_action_label(p->action),
                (unsigned)p->confidence,
                (unsigned)p->salience,
                (unsigned)p->evidence_count,
                (unsigned)p->value_hint,
                (unsigned)p->observed_tick,
                (unsigned long long)p->subject_nonce);
            json_escape_append(buf, &pos, BUFSZ, summary);
            BUF_APPEND(pos, buf, BUFSZ, "\"}");
        }
        BUF_APPEND(pos, buf, BUFSZ, "]}");
    }

    /* Hail message */
    BUF_APPEND(pos, buf, BUFSZ, ",\"hail\":\"");
    json_escape_append(buf, &pos, BUFSZ, st->hail_message);
    BUF_APPEND(pos, buf, BUFSZ, "\",\"rati_hail\":\"");
    json_escape_append(buf, &pos, BUFSZ, st->rati_hail_message);
    BUF_APPEND(pos, buf, BUFSZ, "\",\"miner_chatter\":[");
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ, "\"");
        json_escape_append(buf, &pos, BUFSZ, st->miner_chatter[i]);
        BUF_APPEND(pos, buf, BUFSZ, "\"");
    }
    BUF_APPEND(pos, buf, BUFSZ, "],\"hauler_chatter\":[");
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ, "\"");
        json_escape_append(buf, &pos, BUFSZ, st->hauler_chatter[i]);
        BUF_APPEND(pos, buf, BUFSZ, "\"");
    }
    BUF_APPEND(pos, buf, BUFSZ, "]}");

    mg_http_reply(c, 200, api_headers, "%s", buf);
    free(buf);
}

static const char *api_npc_role_label(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return "miner";
    case NPC_ROLE_HAULER: return "hauler";
    case NPC_ROLE_TOW:    return "tow";
    default:              return "worker";
    }
}

static const char *api_npc_state_label(npc_state_t state) {
    switch (state) {
    case NPC_STATE_IDLE:               return "idle";
    case NPC_STATE_TRAVEL_TO_ASTEROID: return "travel_to_asteroid";
    case NPC_STATE_MINING:             return "mining";
    case NPC_STATE_RETURN_TO_STATION:  return "return_to_station";
    case NPC_STATE_DOCKED:             return "docked";
    case NPC_STATE_TRAVEL_TO_DEST:     return "travel_to_destination";
    case NPC_STATE_UNLOADING:          return "unloading";
    default:                           return "unknown";
    }
}

static int api_parse_npc_role(const char *text) {
    if (!text || text[0] == '\0' || strcmp(text, "any") == 0) return -1;
    if (strcmp(text, "miner") == 0) return (int)NPC_ROLE_MINER;
    if (strcmp(text, "hauler") == 0) return (int)NPC_ROLE_HAULER;
    if (strcmp(text, "tow") == 0) return (int)NPC_ROLE_TOW;
    return -2;
}

static void api_append_station_ref(char *buf, int *pos, int bufsz,
                                   const char *key, int station_idx) {
    BUF_APPEND(*pos, buf, bufsz, "\"%s\":%d,\"%s_name\":\"",
               key, station_idx, key);
    if (station_idx >= 0 && station_idx < MAX_STATIONS &&
        station_exists(&world.stations[station_idx])) {
        json_escape_append(buf, pos, bufsz, world.stations[station_idx].name);
    }
    BUF_APPEND(*pos, buf, bufsz, "\"");
}

static void api_append_npc_contracts(char *buf, int *pos, int bufsz,
                                     const npc_ship_t *npc) {
    BUF_APPEND(*pos, buf, bufsz, "\"known_contracts\":[");
    contract_summary_t known[SHIP_KNOWN_ITEM_CAP];
    int count = knowledge_view_collect_contracts(
        &npc->ship->knowledge, known, SHIP_KNOWN_ITEM_CAP);
    for (int i = 0; i < count; i++) {
        const contract_summary_t *cs = &known[i];
        if (i > 0) BUF_APPEND(*pos, buf, bufsz, ",");
        BUF_APPEND(*pos, buf, bufsz,
                   "{\"action\":%u,\"action_name\":\"%s\","
                   "\"station\":%u,\"station_name\":\"",
                   (unsigned)cs->action,
                   route_history_action_label(cs->action),
                   (unsigned)cs->station_index);
        if (cs->station_index < MAX_STATIONS &&
            station_exists(&world.stations[cs->station_index])) {
            json_escape_append(buf, pos, bufsz,
                               world.stations[cs->station_index].name);
        }
        BUF_APPEND(*pos, buf, bufsz,
                   "\",\"commodity\":%u,\"commodity_code\":\"%s\","
                   "\"quantity\":%u,\"price\":%u,\"age\":%u}",
                   (unsigned)cs->commodity,
                   cs->commodity < COMMODITY_COUNT
                       ? commodity_code((commodity_t)cs->commodity)
                       : "UNK",
                   (unsigned)cs->quantity_needed,
                   (unsigned)cs->base_price,
                   (unsigned)cs->age_at_copy);
    }
    BUF_APPEND(*pos, buf, bufsz, "]");
}

static void api_append_npc_market_memories(char *buf, int *pos, int bufsz,
                                           const npc_ship_t *npc) {
    BUF_APPEND(*pos, buf, bufsz, "\"market_memories\":[");
    int written = 0;
    int item_count = npc->ship->knowledge.count;
    if (item_count > KNOWLEDGE_VIEW_MAX_CAP) item_count = KNOWLEDGE_VIEW_MAX_CAP;
    for (int i = 0; i < item_count; i++) {
        const knowledge_item_t *item = &npc->ship->knowledge.items[i];
        market_memory_t memory;
        memset(&memory, 0, sizeof(memory));
        if (!inspect_snapshot_market_memory_from_item(item, &memory))
            continue;
        if (written++ > 0) BUF_APPEND(*pos, buf, bufsz, ",");
        BUF_APPEND(*pos, buf, bufsz,
                   "{\"kind\":%u,\"kind_name\":\"%s\","
                   "\"station_a\":%u,\"station_a_name\":\"",
                   (unsigned)memory.memory_kind,
                   route_history_memory_kind_label(memory.memory_kind),
                   (unsigned)memory.station_a);
        if (memory.station_a < MAX_STATIONS &&
            station_exists(&world.stations[memory.station_a])) {
            json_escape_append(buf, pos, bufsz,
                               world.stations[memory.station_a].name);
        }
        BUF_APPEND(*pos, buf, bufsz,
                   "\",\"station_b\":%u,\"station_b_name\":\"",
                   (unsigned)memory.station_b);
        if (memory.station_b < MAX_STATIONS &&
            station_exists(&world.stations[memory.station_b])) {
            json_escape_append(buf, pos, bufsz,
                               world.stations[memory.station_b].name);
        }
        BUF_APPEND(*pos, buf, bufsz,
                   "\",\"commodity\":%u,\"commodity_code\":\"%s\","
                   "\"action\":%u,\"action_name\":\"%s\","
                   "\"confidence\":%u,\"salience\":%u,"
                   "\"quantity_hint\":%u,\"value_hint\":%u,"
                   "\"observed_tick\":%u,\"learned_tick\":%llu,"
                   "\"hops\":%u,\"subject_nonce\":%llu}",
                   (unsigned)memory.commodity,
                   memory.commodity < COMMODITY_COUNT
                       ? commodity_code((commodity_t)memory.commodity)
                       : "UNK",
                   (unsigned)memory.action,
                   route_history_action_label(memory.action),
                   (unsigned)memory.confidence,
                   (unsigned)memory.salience,
                   (unsigned)memory.quantity_hint,
                   (unsigned)memory.value_hint,
                   (unsigned)memory.observed_tick,
                   (unsigned long long)item->learned_tick,
                   (unsigned)item->hops,
                   (unsigned long long)memory.subject_nonce);
    }
    BUF_APPEND(*pos, buf, bufsz, "]");
}

static void api_append_npc_job_diagnostics(char *buf, int *pos, int bufsz,
                                           const npc_ship_t *npc) {
    BUF_APPEND(*pos, buf, bufsz, "\"job_diagnostics\":[");
    int count = npc->job_diag_count;
    int cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
    if (count > cap) count = cap;
    for (int i = 0; i < count; i++) {
        if (i > 0) BUF_APPEND(*pos, buf, bufsz, ",");
        BUF_APPEND(*pos, buf, bufsz,
                   "{\"kind\":%u,\"score\":%u,\"selected\":%s,"
                   "\"source\":%u,\"source_name\":\"",
                   (unsigned)npc->job_diag_kind[i],
                   (unsigned)npc->job_diag_score[i],
                   npc->job_diag_selected[i] ? "true" : "false",
                   (unsigned)npc->job_diag_source[i]);
        if (npc->job_diag_source[i] < MAX_STATIONS &&
            station_exists(&world.stations[npc->job_diag_source[i]])) {
            json_escape_append(buf, pos, bufsz,
                               world.stations[npc->job_diag_source[i]].name);
        }
        BUF_APPEND(*pos, buf, bufsz,
                   "\",\"dest\":%u,\"dest_name\":\"",
                   (unsigned)npc->job_diag_dest[i]);
        if (npc->job_diag_dest[i] < MAX_STATIONS &&
            station_exists(&world.stations[npc->job_diag_dest[i]])) {
            json_escape_append(buf, pos, bufsz,
                               world.stations[npc->job_diag_dest[i]].name);
        }
        BUF_APPEND(*pos, buf, bufsz,
                   "\",\"commodity\":%u,\"commodity_code\":\"%s\","
                   "\"hint\":%u,\"reason\":%u,"
                   "\"memory_kind\":%u,\"memory_kind_name\":\"%s\","
                   "\"memory_hops\":%u,\"memory_age\":%u,"
                   "\"memory_station\":%u}",
                   (unsigned)npc->job_diag_commodity[i],
                   npc->job_diag_commodity[i] < COMMODITY_COUNT
                       ? commodity_code((commodity_t)npc->job_diag_commodity[i])
                       : "UNK",
                   (unsigned)npc->job_diag_hint[i],
                   (unsigned)npc->job_diag_reason[i],
                   (unsigned)npc->job_diag_memory_kind[i],
                   route_history_memory_kind_label(npc->job_diag_memory_kind[i]),
                   (unsigned)npc->job_diag_memory_hops[i],
                   (unsigned)npc->job_diag_memory_age[i],
                   (unsigned)npc->job_diag_memory_station[i]);
    }
    BUF_APPEND(*pos, buf, bufsz, "]");
}

static void api_append_npc_chatter_record(char *buf, int *pos, int bufsz,
                                          int slot, const npc_ship_t *npc) {
    float cargo_total = ship_total_cargo(npc->ship);

    BUF_APPEND(*pos, buf, bufsz,
               "{\"slot\":%d,\"role\":\"%s\",\"state\":\"%s\",",
               slot, api_npc_role_label(npc->role),
               api_npc_state_label(npc->state));
    api_append_station_ref(buf, pos, bufsz, "home_station", npc->home_station);
    BUF_APPEND(*pos, buf, bufsz, ",");
    api_append_station_ref(buf, pos, bufsz, "dest_station", npc->dest_station);
    BUF_APPEND(*pos, buf, bufsz, ",");
    api_append_station_ref(buf, pos, bufsz, "pickup_station", npc->pickup_station);
    BUF_APPEND(*pos, buf, bufsz,
               ",\"position\":{\"x\":%.1f,\"y\":%.1f},"
               "\"velocity\":{\"x\":%.2f,\"y\":%.2f},"
               "\"hull\":%.1f,\"cargo_total\":%.1f,"
               "\"target_asteroid\":%d,\"towed_fragment\":%d,"
               "\"towed_scaffold\":%d,\"cargo\":[",
               npc->ship->pos.x, npc->ship->pos.y,
               npc->ship->vel.x, npc->ship->vel.y,
               npc->ship->hull, cargo_total, npc->target_asteroid,
               npc_towed_fragment_index(npc), npc->ship->towed_scaffold);
    int cargo_written = 0;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        float amount = ship_cargo_amount(npc->ship, (commodity_t)c);
        if (amount <= 0.0f) continue;
        if (cargo_written++ > 0) BUF_APPEND(*pos, buf, bufsz, ",");
        BUF_APPEND(*pos, buf, bufsz,
                   "{\"commodity\":%d,\"commodity_code\":\"%s\",\"amount\":%.1f}",
                   c, commodity_code((commodity_t)c), amount);
    }
    BUF_APPEND(*pos, buf, bufsz, "],");
    api_append_npc_job_diagnostics(buf, pos, bufsz, npc);
    BUF_APPEND(*pos, buf, bufsz, ",");
    api_append_npc_market_memories(buf, pos, bufsz, npc);
    BUF_APPEND(*pos, buf, bufsz, ",");
    api_append_npc_contracts(buf, pos, bufsz, npc);
    BUF_APPEND(*pos, buf, bufsz, "}");
}

static void handle_npc_chatter_context(struct mg_connection *c,
                                       struct mg_http_message *hm) {
    char tmp[64];
    long slot_filter = -1;
    long station_filter = -1;
    long limit = 3;
    int role_filter = -1;

    if (hm && mg_http_get_var(&hm->query, "slot", tmp, sizeof(tmp)) > 0)
        slot_filter = strtol(tmp, NULL, 10);
    if (hm && mg_http_get_var(&hm->query, "station", tmp, sizeof(tmp)) > 0)
        station_filter = strtol(tmp, NULL, 10);
    if (hm && mg_http_get_var(&hm->query, "limit", tmp, sizeof(tmp)) > 0)
        limit = strtol(tmp, NULL, 10);
    if (hm && mg_http_get_var(&hm->query, "role", tmp, sizeof(tmp)) > 0)
        role_filter = api_parse_npc_role(tmp);

    if (role_filter == -2) {
        mg_http_reply(c, 400, api_headers, "{\"error\":\"invalid role\"}");
        return;
    }
    if (limit <= 0) limit = 1;
    if (limit > 16) limit = 16;

    enum { BUFSZ = 131072 };
    char *buf = (char *)malloc(BUFSZ);
    if (!buf) {
        mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
        return;
    }
    int pos = 0;
    int written = 0;

    BUF_APPEND(pos, buf, BUFSZ,
               "{\"world\":{\"tick\":%u,\"time\":%.3f,"
               "\"belt_seed\":%u,\"world_seq\":%u},\"npcs\":[",
               world.tick, world.time, world.belt_seed, world.world_seq);
    for (int i = 0; i < MAX_NPC_SHIPS && written < limit; i++) {
        const npc_ship_t *npc = &world.npc_ships[i];
        if (!npc->active) continue;
        if (slot_filter >= 0 && slot_filter != i) continue;
        if (role_filter >= 0 && (int)npc->role != role_filter) continue;
        if (station_filter >= 0 &&
            npc->home_station != station_filter &&
            npc->dest_station != station_filter &&
            npc->pickup_station != station_filter) {
            continue;
        }
        if (written++ > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        api_append_npc_chatter_record(buf, &pos, BUFSZ, i, npc);
    }
    BUF_APPEND(pos, buf, BUFSZ, "]}");
    mg_http_reply(c, 200, api_headers, "%s", buf);
    free(buf);
}

static void handle_station_command(struct mg_connection *c, struct mg_http_message *hm, int sid) {
    struct mg_str body = hm->body;
    char *action = mg_json_get_str(body, "$.action");
    long commodity = mg_json_get_long(body, "$.commodity", -1);
    double price_val = 0;
    mg_json_get_num(body, "$.price", &price_val);
    long module_type = mg_json_get_long(body, "$.module_type", -1);
    long slot = mg_json_get_long(body, "$.slot", -1);
    char *hail = mg_json_get_str(body, "$.hail");
    char *message = mg_json_get_str(body, "$.message");
    char *currency = mg_json_get_str(body, "$.currency_name");

    if (!action) {
        mg_http_reply(c, 400, api_headers,
                      "{\"ok\":false,\"error\":\"missing action\"}");
        free(hail);
        free(message);
        free(currency);
        return;
    }

    if (strcmp(action, "set_hail") == 0) {
        uint64_t event_id = 0;
        const char *err = NULL;
        if (station_mutation_operator_text(sid, OPERATOR_POST_HAIL_MOTD, 0,
                                           hail, &event_id, &err)) {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_hail\",\"audited\":true,\"event_id\":%llu}",
                          (unsigned long long)event_id);
        } else {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}", err ? err : "operator post failed");
        }
    } else if (strcmp(action, "set_miner_chatter") == 0) {
        uint64_t event_id = 0;
        const char *err = NULL;
        if (station_mutation_operator_text(sid, OPERATOR_POST_MINER_CHATTER,
                                           slot, message, &event_id, &err)) {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_miner_chatter\",\"slot\":%ld,\"audited\":true,\"event_id\":%llu}",
                          slot, (unsigned long long)event_id);
        } else {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}", err ? err : "operator post failed");
        }
    } else if (strcmp(action, "set_hauler_chatter") == 0) {
        uint64_t event_id = 0;
        const char *err = NULL;
        if (station_mutation_operator_text(sid, OPERATOR_POST_HAULER_CHATTER,
                                           slot, message, &event_id, &err)) {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_hauler_chatter\",\"slot\":%ld,\"audited\":true,\"event_id\":%llu}",
                          slot, (unsigned long long)event_id);
        } else {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}", err ? err : "operator post failed");
        }
    } else if (strcmp(action, "set_rati_hail") == 0) {
        uint64_t event_id = 0;
        const char *err = NULL;
        if (station_mutation_operator_text(sid, OPERATOR_POST_RATI_DELIVERY, 0,
                                           message, &event_id, &err)) {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_rati_hail\",\"audited\":true,\"event_id\":%llu}",
                          (unsigned long long)event_id);
        } else {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}", err ? err : "operator post failed");
        }
    } else if (strcmp(action, "set_currency_name") == 0) {
        char sanitized[32];
        const char *err = NULL;
        if (!station_mutation_set_currency_name(sid, currency, sanitized,
                                                sizeof(sanitized), &err)) {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}",
                          err ? err : "currency_name rejected");
        } else {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_currency_name\",\"audited\":false,\"value\":\"%s\"}", sanitized);
        }
    } else if (strcmp(action, "set_price") == 0) {
        float clamped = 0.0f;
        const char *err = NULL;
        if (!station_mutation_set_price(sid, commodity, price_val,
                                        &clamped, &err)) {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}",
                          err ? err : "price rejected");
        } else {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"set_price\",\"audited\":false,\"commodity\":%ld,\"price\":%.1f}",
                          commodity, clamped);
        }
    } else if (strcmp(action, "build_module") == 0) {
        const char *err = NULL;
        if (!station_mutation_build_module(sid, module_type, &err)) {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"%s\"}",
                          err ? err : "module build rejected");
        } else {
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"build_module\",\"audited\":false,\"type\":%ld}", module_type);
        }
    } else {
        mg_http_reply(c, 400, api_headers,
                      "{\"ok\":false,\"error\":\"unknown action\"}");
    }
    free(action);
    free(hail);
    free(message);
    free(currency);
}

/* ------------------------------------------------------------------ */
/* Mongoose event handler                                             */
/* ------------------------------------------------------------------ */

/* Per-client REST API token buckets. A single process-wide bucket allowed an
 * unauthenticated caller to starve every operator endpoint. Keep this table
 * bounded and evict the least-recently-used entry when it fills. */
#define API_RATE_CLIENTS 128
#define API_RATE_REFILL_PER_SEC 20
#define API_RATE_BUCKET_MAX 40

typedef struct {
    uint64_t key;
    uint64_t last_refill;
    uint64_t last_seen;
    int tokens;
    bool in_use;
} api_rate_bucket_t;

static api_rate_bucket_t api_rate_buckets[API_RATE_CLIENTS];

static api_rate_bucket_t *api_rate_bucket_for(uint64_t key, uint64_t now) {
    api_rate_bucket_t *free_slot = NULL;
    api_rate_bucket_t *oldest = NULL;
    for (int i = 0; i < API_RATE_CLIENTS; i++) {
        api_rate_bucket_t *bucket = &api_rate_buckets[i];
        if (bucket->in_use && bucket->key == key) return bucket;
        if (!bucket->in_use && !free_slot) free_slot = bucket;
        if (bucket->in_use && (!oldest || bucket->last_seen < oldest->last_seen))
            oldest = bucket;
    }
    api_rate_bucket_t *bucket = free_slot ? free_slot : oldest;
    if (!bucket) return NULL;
    *bucket = (api_rate_bucket_t){
        .key = key,
        .last_refill = now,
        .last_seen = now,
        .tokens = API_RATE_BUCKET_MAX,
        .in_use = true,
    };
    return bucket;
}

static bool api_rate_check(struct mg_connection *c,
                           struct mg_http_message *hm) {
    uint64_t now = mg_millis();
    api_rate_bucket_t *bucket = api_rate_bucket_for(
        server_http_client_key(c, hm), now);
    if (!bucket) return false;
    uint64_t elapsed = now - bucket->last_refill;
    if (elapsed >= 50) {  /* refill every 50ms to smooth out bursts */
        int refill = (int)(elapsed * API_RATE_REFILL_PER_SEC / 1000);
        if (refill > 0) {
            bucket->tokens += refill;
            if (bucket->tokens > API_RATE_BUCKET_MAX)
                bucket->tokens = API_RATE_BUCKET_MAX;
            bucket->last_refill = now;
        }
    }
    bucket->last_seen = now;
    if (bucket->tokens <= 0) return false;
    bucket->tokens--;
    return true;
}

static const char *protocol_msg_name(uint8_t msg) {
    switch (msg) {
    case NET_MSG_SERVER_INFO: return "SERVER_INFO";
    case NET_MSG_INPUT: return "INPUT";
    case NET_MSG_LATENCY_PING: return "LATENCY_PING";
    case NET_MSG_LATENCY_PONG: return "LATENCY_PONG";
    case NET_MSG_CLIENT_METRICS: return "CLIENT_METRICS";
    case NET_MSG_INPUT_APPLIED: return "INPUT_APPLIED";
    case NET_MSG_STATION_IDENTITY: return "STATION_IDENTITY";
    case NET_MSG_STATION_IDENTITY_Q: return "STATION_IDENTITY_Q";
    case NET_MSG_STATION_DIAG: return "STATION_DIAG";
    case NET_MSG_WORLD_PLAYERS: return "WORLD_PLAYERS";
    case NET_MSG_WORLD_PLAYER_MOTION: return "WORLD_PLAYER_MOTION";
    case NET_MSG_WORLD_PLAYER_MOTION_Q: return "WORLD_PLAYER_MOTION_Q";
    case NET_MSG_WORLD_PLAYER_MOTIOND_Q: return "WORLD_PLAYER_MOTIOND_Q";
    case NET_MSG_WORLD_PLAYER_POSED_Q: return "WORLD_PLAYER_POSED_Q";
    case NET_MSG_WORLD_PLAYER_MOTIONM_Q: return "WORLD_PLAYER_MOTIONM_Q";
    case NET_MSG_WORLD_PLAYER_DOCK_Q: return "WORLD_PLAYER_DOCK_Q";
    case NET_MSG_WORLD_ASTEROIDS: return "WORLD_ASTEROIDS";
    case NET_MSG_WORLD_ASTEROIDS_Q: return "WORLD_ASTEROIDS_Q";
    case NET_MSG_WORLD_ASTEROIDS8_Q: return "WORLD_ASTEROIDS8_Q";
    case NET_MSG_WORLD_ASTEROID_MOTION: return "WORLD_ASTEROID_MOTION";
    case NET_MSG_WORLD_ASTEROID_MOTION_Q: return "WORLD_ASTEROID_MOTION_Q";
    case NET_MSG_WORLD_ASTEROID_POS_Q: return "WORLD_ASTEROID_POS_Q";
    case NET_MSG_WORLD_ASTEROID_POS8_Q: return "WORLD_ASTEROID_POS8_Q";
    case NET_MSG_WORLD_ASTEROID_POSD_Q: return "WORLD_ASTEROID_POSD_Q";
    case NET_MSG_WORLD_ASTEROID_POSD8_Q: return "WORLD_ASTEROID_POSD8_Q";
    case NET_MSG_WORLD_ASTEROID_STATE_Q: return "WORLD_ASTEROID_STATE_Q";
    case NET_MSG_WORLD_ASTEROID_REMOVE: return "WORLD_ASTEROID_REMOVE";
    case NET_MSG_WORLD_SCAFFOLDS: return "WORLD_SCAFFOLDS";
    case NET_MSG_WORLD_SCAFFOLD_REMOVE: return "WORLD_SCAFFOLD_REMOVE";
    case NET_MSG_WORLD_SCAFFOLD_MOTION_Q: return "WORLD_SCAFFOLD_MOTION_Q";
    case NET_MSG_WORLD_NPC_MOTION: return "WORLD_NPC_MOTION";
    case NET_MSG_WORLD_NPC_MOTION_Q: return "WORLD_NPC_MOTION_Q";
    case NET_MSG_WORLD_NPC_MOTION8_Q: return "WORLD_NPC_MOTION8_Q";
    case NET_MSG_WORLD_NPC_POS_Q: return "WORLD_NPC_POS_Q";
    case NET_MSG_WORLD_NPC_POSE_Q: return "WORLD_NPC_POSE_Q";
    case NET_MSG_WORLD_NPC_LINEAR_Q: return "WORLD_NPC_LINEAR_Q";
    case NET_MSG_WORLD_NPC_STATUS: return "WORLD_NPC_STATUS";
    case NET_MSG_WORLD_NPC_STATUS8_Q: return "WORLD_NPC_STATUS8_Q";
    case NET_MSG_WORLD_CARGO_POD_MOTION: return "WORLD_CARGO_POD_MOTION";
    case NET_MSG_WORLD_CARGO_POD_MOTION_Q: return "WORLD_CARGO_POD_MOTION_Q";
    case NET_MSG_WORLD_CARGO_POD_LINEAR_Q: return "WORLD_CARGO_POD_LINEAR_Q";
    case NET_MSG_WORLD_CARGO_POD_REMOVE: return "WORLD_CARGO_POD_REMOVE";
    case NET_MSG_WORLD_CARGO_PODS: return "WORLD_CARGO_PODS";
    case NET_MSG_WORLD_CARGO_PODS_Q: return "WORLD_CARGO_PODS_Q";
    case NET_MSG_WORLD_INTERACTIONS: return "WORLD_INTERACTIONS";
    case NET_MSG_WORLD_INTERACTIONS_Q: return "WORLD_INTERACTIONS_Q";
    case NET_MSG_WORLD_INTERACTION_DRIFT: return "WORLD_INTERACTION_DRIFT";
    case NET_MSG_PLAYER_SHIP: return "PLAYER_SHIP";
    case NET_MSG_PLAYER_KNOWN_CONTRACTS: return "PLAYER_KNOWN_CONTRACTS";
    case NET_MSG_DELIVERY_LEDGER: return "DELIVERY_LEDGER";
    case NET_MSG_PLAYER_KNOWN_LEDGER: return "PLAYER_KNOWN_LEDGER";
    case NET_MSG_WORLD_STATIONS: return "WORLD_STATIONS";
    case NET_MSG_WORLD_STATIONS_Q: return "WORLD_STATIONS_Q";
    case NET_MSG_STATION_MANIFEST: return "STATION_MANIFEST";
    case NET_MSG_PLAYER_MANIFEST: return "PLAYER_MANIFEST";
    case NET_MSG_STATION_INGOTS: return "STATION_INGOTS";
    case NET_MSG_HOLD_INGOTS: return "HOLD_INGOTS";
    case NET_MSG_FRACTURE_CHALLENGE: return "FRACTURE_CHALLENGE";
    case NET_MSG_FRACTURE_CLAIM: return "FRACTURE_CLAIM";
    case NET_MSG_FRACTURE_RESOLVED: return "FRACTURE_RESOLVED";
    case NET_MSG_CONTRACTS: return "CONTRACTS";
    case NET_MSG_CONTRACTS_Q: return "CONTRACTS_Q";
    case NET_MSG_INSPECT_SNAPSHOT: return "INSPECT_SNAPSHOT";
    case NET_MSG_CARGO_RECEIPT_BUNDLE: return "CARGO_RECEIPT_BUNDLE";
    case NET_MSG_PRESENT_RECEIPT_CHAIN: return "PRESENT_RECEIPT_CHAIN";
    case NET_MSG_HANDOFF_REQUEST: return "HANDOFF_REQUEST";
    case NET_MSG_HANDOFF_TICKET: return "HANDOFF_TICKET";
    case NET_MSG_HANDOFF_PRESENT: return "HANDOFF_PRESENT";
    case NET_MSG_HANDOFF_RESULT: return "HANDOFF_RESULT";
    default: return "UNKNOWN";
    }
}

static const char *protocol_stream_class_name(uint8_t stream_class) {
    switch (stream_class) {
    case PROTOCOL_STREAM_CLASS_STATIC: return "static";
    case PROTOCOL_STREAM_CLASS_LIVE: return "live";
    case PROTOCOL_STREAM_CLASS_ECON: return "econ";
    case PROTOCOL_STREAM_CLASS_PLAYER: return "player";
    case PROTOCOL_STREAM_CLASS_EVENT: return "event";
    case PROTOCOL_STREAM_CLASS_AUTH: return "auth";
    default: return "unknown";
    }
}

static void handle_protocol_info_http(struct mg_connection *c) {
    uint8_t wire[PROTOCOL_INFO_SIZE];
    int wire_len = serialize_protocol_info(
        wire, SIM_TICK_MS, STATE_TICK_MS, WORLD_TICK_MS,
        SHIP_TICK_MS, STATION_DIAG_MIN_MS,
        STATION_IDENTITY_FALLBACK_MS);
    if (wire_len < PROTOCOL_INFO_HEADER_SIZE) {
        mg_http_reply(c, 500, api_headers, "{\"error\":\"protocol_info_overflow\"}");
        return;
    }

    enum { PROTOCOL_JSON_BUFSZ = 16384 };
    char out[PROTOCOL_JSON_BUFSZ];
    int pos = 0;
    uint16_t version = read_u16_le(&wire[1]);
    uint32_t caps = read_u32_le(&wire[3]);
    int count = wire[7];
    int max_by_len = (wire_len - PROTOCOL_INFO_HEADER_SIZE) /
                     PROTOCOL_INFO_STREAM_RECORD_SIZE;
    if (count > max_by_len) count = max_by_len;

    BUF_APPEND(pos, out, PROTOCOL_JSON_BUFSZ,
               "{\"version\":%u,\"capabilities\":%u,\"stream_count\":%d,"
               "\"streams\":[",
               (unsigned)version, (unsigned)caps, count);
    for (int i = 0; i < count; i++) {
        const uint8_t *p = &wire[PROTOCOL_INFO_HEADER_SIZE +
                                 i * PROTOCOL_INFO_STREAM_RECORD_SIZE];
        uint8_t msg = p[0];
        uint8_t stream_class = p[1];
        uint16_t flags = read_u16_le(&p[2]);
        uint16_t header_size = read_u16_le(&p[4]);
        uint16_t record_size = read_u16_le(&p[6]);
        uint16_t max_records = read_u16_le(&p[8]);
        uint16_t cadence_ms = read_u16_le(&p[10]);
        if (i > 0) BUF_APPEND(pos, out, PROTOCOL_JSON_BUFSZ, ",");
        BUF_APPEND(pos, out, PROTOCOL_JSON_BUFSZ,
                   "{\"msg\":%u,\"name\":\"%s\",\"class\":\"%s\","
                   "\"flags\":%u,\"header_size\":%u,\"record_size\":%u,"
                   "\"max_records\":%u,\"cadence_ms\":%u}",
                   (unsigned)msg, protocol_msg_name(msg),
                   protocol_stream_class_name(stream_class),
                   (unsigned)flags, (unsigned)header_size,
                   (unsigned)record_size, (unsigned)max_records,
                   (unsigned)cadence_ms);
    }
    BUF_APPEND(pos, out, PROTOCOL_JSON_BUFSZ, "]}");
    if (pos >= PROTOCOL_JSON_BUFSZ) {
        mg_http_reply(c, 500, api_headers,
                      "{\"error\":\"protocol_info_json_overflow\"}");
        return;
    }
    mg_http_reply(c, 200, api_headers, "%s", out);
}

static bool redirect_static_alias(struct mg_connection *c,
                                  struct mg_http_message *hm,
                                  const char *path,
                                  const char *target) {
    if (!mg_match(hm->uri, mg_str(path), NULL)) return false;
    char headers[1024];
    if (hm->query.len > 0) {
        snprintf(headers, sizeof(headers),
                 "Location: %s?%.*s\r\nCache-Control: no-store\r\n",
                 target, (int)hm->query.len, hm->query.buf);
    } else {
        snprintf(headers, sizeof(headers),
                 "Location: %s\r\nCache-Control: no-store\r\n", target);
    }
    mg_http_reply(c, 302, headers, "");
    return true;
}

static bool serve_static_http(struct mg_connection *c,
                              struct mg_http_message *hm) {
    if (!static_root_dir || static_root_dir[0] == '\0') return false;
    if (redirect_static_alias(c, hm, "/play", "/play.html") ||
        redirect_static_alias(c, hm, "/play/", "/play.html") ||
        redirect_static_alias(c, hm, "/signal.html", "/play.html") ||
        redirect_static_alias(c, hm, "/ost", "/ost.html") ||
        redirect_static_alias(c, hm, "/ost/", "/ost.html") ||
        redirect_static_alias(c, hm, "/mine", "/mine.html") ||
        redirect_static_alias(c, hm, "/mine/", "/mine.html")) {
        return true;
    }
    struct mg_http_serve_opts opts = {
        .root_dir = static_root_dir,
        .extra_headers =
            "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "X-Content-Type-Options: nosniff\r\n",
        .mime_types =
            "wasm=application/wasm,"
            "js=text/javascript,"
            "mjs=text/javascript,"
            "html=text/html; charset=utf-8",
    };
    mg_http_serve_dir(c, hm, &opts);
    return true;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            server_note_ws_client_addr(c, hm);
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/protocol"), NULL)) {
            if (!api_rate_check(c, hm)) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                handle_protocol_info_http(c);
            }
        } else if (mg_match(hm->uri, mg_str("/api/npc_chatter_context"), NULL)) {
            if (!api_rate_check(c, hm)) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                handle_npc_chatter_context(c, hm);
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/state"), NULL)) {
            if (!api_rate_check(c, hm)) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else if (sid >= SIGNAL_FIRST_OUTPOST_INDEX && !api_auth_ok(hm)) {
                    /* Seeded stations are read-only without auth;
                     * player-built outposts require auth. */
                    mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
                } else {
                    handle_station_state(c, sid, hm);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/command"), NULL)) {
            if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else if (!api_rate_check(c, hm)) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else {
                    handle_station_command(c, hm, sid);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/signal_channel"), NULL)) {
            /* Station posts to the broadcast log (#316). */
            if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else if (!api_rate_check(c, hm)) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else {
                    char *text = mg_json_get_str(hm->body, "$.text");
                    char *audio = mg_json_get_str(hm->body, "$.audio_url");
                    if (!text || text[0] == '\0' || strlen(text) > SIGNAL_CHANNEL_TEXT_MAX - 1) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"ok\":false,\"error\":\"text missing or >200 chars\"}");
                    } else if (audio && audio[0] && strncmp(audio, "https://", 8) != 0) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"ok\":false,\"error\":\"audio_url must be https\"}");
                    } else if (audio && strlen(audio) > SIGNAL_CHANNEL_AUDIO_MAX - 1) {
                        mg_http_reply(c, 400, api_headers,
                                      "{\"ok\":false,\"error\":\"audio_url too long\"}");
                    } else {
                        uint64_t id = signal_channel_post(&world, sid, text, audio ? audio : "");
                        uint32_t ts = (uint32_t)(world.time * 1000.0f);
                        /* Push snapshot to every connected ship so the
                         * Network tab updates in-game without polling. */
                        {
                            size_t cap = (size_t)(3 + world.signal_channel.count * SIGNAL_CHANNEL_RECORD_SIZE);
                            uint8_t *msg = (uint8_t *)malloc(cap);
                            if (msg) {
                                int len = serialize_signal_channel(msg, &world.signal_channel);
                                broadcast(msg, (size_t)len);
                                free(msg);
                            }
                        }
                        mg_http_reply(c, 200, api_headers,
                                      "{\"ok\":true,\"id\":%llu,\"timestamp\":%u}",
                                      (unsigned long long)id, ts);
                    }
                    free(text);
                    free(audio);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/signal_channel/messages"), NULL)) {
            if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else if (!api_rate_check(c, hm)) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                /* Parse ?since=<id>&limit=<1..100> — crude query scan
                 * since mongoose gives us hm->query as a raw string. */
                long since = 0, limit = 50;
                char tmp[32];
                if (mg_http_get_var(&hm->query, "since", tmp, sizeof(tmp)) > 0)
                    (void)parse_query_long(tmp, &since);
                if (mg_http_get_var(&hm->query, "limit", tmp, sizeof(tmp)) > 0)
                    (void)parse_query_long(tmp, &limit);
                if (limit < 1) limit = 1;
                if (limit > 100) limit = 100;

                /* Response sized for worst case: 100 × 440 bytes + framing ≈ 50KB. */
                enum { RESP_BUFSZ = 65536 };
                char *out = (char *)malloc(RESP_BUFSZ);
                if (!out) {
                    mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
                } else {
                    int pos = 0;
                    BUF_APPEND(pos, out, RESP_BUFSZ, "{\"messages\":[");
                    bool first = true;
                    int emitted = 0;
                    for (int i = 0; i < world.signal_channel.count && emitted < (int)limit; i++) {
                        const signal_channel_msg_t *m = signal_channel_at(&world, i);
                        if (!m || (long long)m->id <= since) continue;
                        if (!first) BUF_APPEND(pos, out, RESP_BUFSZ, ",");
                        first = false;
                        BUF_APPEND(pos, out, RESP_BUFSZ,
                            "{\"id\":%llu,\"timestamp\":%u,\"sender_station_id\":%d,\"text\":\"",
                            (unsigned long long)m->id, m->timestamp_ms, (int)m->sender_station);
                        json_escape_append(out, &pos, RESP_BUFSZ, m->text);
                        BUF_APPEND(pos, out, RESP_BUFSZ, "\"");
                        if (m->audio_url[0]) {
                            BUF_APPEND(pos, out, RESP_BUFSZ, ",\"audio_url\":\"");
                            json_escape_append(out, &pos, RESP_BUFSZ, m->audio_url);
                            BUF_APPEND(pos, out, RESP_BUFSZ, "\"");
                        }
                        BUF_APPEND(pos, out, RESP_BUFSZ, "}");
                        emitted++;
                    }
                    BUF_APPEND(pos, out, RESP_BUFSZ, "]}");
                    mg_http_reply(c, 200, api_headers, "%s", out);
                    free(out);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/training/v1/station-policy-trace"), NULL)) {
            reply_station_policy_trace(c);
        } else if (mg_match(hm->uri, mg_str("/training/v1/bot-trace-weights"), NULL)) {
            reply_bot_trace_weights(c);
        } else if (mg_match(hm->uri, mg_str("/health"), NULL)) {
            int count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (world.players[i].connected) count++;
            int live_connections = live_player_connection_count();
            static const uint8_t zero_pub[32] = {0};
            int chain_station_count = 0;
            int chain_blocked_count = 0;
            int chain_warning_count = 0;
            for (int i = 0; i < MAX_STATIONS; i++) {
                const station_t *st = &world.stations[i];
                if (memcmp(st->station_pubkey, zero_pub, 32) == 0) continue;
                chain_station_count++;
                chain_health_status_t status =
                    (chain_health_status_t)st->chain_health_status;
                if (st->chain_append_blocked) {
                    chain_blocked_count++;
                    chain_warning_count++;
                } else if (status == CHAIN_HEALTH_UNKNOWN ||
                           status == CHAIN_HEALTH_ADOPTED) {
                    chain_warning_count++;
                }
            }
            const char *chain_health =
                chain_blocked_count > 0 ? "blocked" :
                chain_warning_count > 0 ? "warning" : "ok";
#ifdef GIT_HASH
            const char *version = GIT_HASH;
#else
            const char *version = "dev";
#endif
            enum { HEALTH_BUFSZ = 65536 };
            char *buf = (char *)malloc(HEALTH_BUFSZ);
            if (!buf) {
                mg_http_reply(c, 500, api_headers, "{\"error\":\"out of memory\"}");
                return;
            }
            int pos = 0;
            BUF_APPEND(pos, buf, HEALTH_BUFSZ,
                       "{\"status\":\"ok\",\"players\":%d,\"live_connections\":%d,"
                       "\"server_bot_players\":%d,"
                       "\"frontier_virtual_pilots\":%d,"
                       "\"frontier_plans_created\":%u,"
                       "\"frontier_scaffold_orders\":%u,"
                       "\"frontier_module_plans_created\":%u,"
                       "\"frontier_module_scaffold_orders\":%u,"
                       "\"frontier_virtual_scaffolds_manufactured\":%u,"
                       "\"frontier_virtual_scaffold_deliveries\":%u,"
                       "\"frontier_virtual_supply_deliveries\":%u,"
                       "\"server_bot_brain_mode\":\"%s\","
                       "\"server_intelligence_backend\":\"%s\","
                       "\"server_intelligence_flight_builtin\":%s,"
                       "\"server_intelligence_flight_feature_set\":\"%s\","
                       "\"server_intelligence_flight_encoder_version\":%u,"
                       "\"server_intelligence_flight_checkpoint_hash\":\"%s\","
                       "\"server_brain_loaded\":%s,"
                       "\"server_brain_inferences\":%llu,"
                       "\"server_contract_brain_loaded\":%s,"
                       "\"server_contract_brain_inferences\":%llu,"
                       "\"server_contract_brain_decisions\":%llu,"
                       "\"server_contract_brain_teacher_decisions\":%llu,"
                       "\"server_npc_worker_brain_loaded\":%s,"
                       "\"server_npc_worker_brain_inferences\":%llu,"
                       "\"server_npc_worker_brain_decisions\":%llu,"
                       "\"server_npc_worker_brain_teacher_decisions\":%llu,"
                       "\"version\":\"%s\","
                       "\"persistence\":{\"mode\":\"%s\","
                       "\"load_enabled\":%s,\"save_enabled\":%s,"
                       "\"externalized\":%s,\"external_store\":\"%s\","
                       "\"state_uri\":\"",
                       count, live_connections, server_bot_players_spawned,
                       world.frontier_virtual_pilots,
                       world.frontier_plans_created,
                       world.frontier_scaffold_orders,
                       world.frontier_module_plans_created,
                       world.frontier_module_scaffold_orders,
                       world.frontier_virtual_scaffolds_manufactured,
                       world.frontier_virtual_scaffold_deliveries,
                       world.frontier_virtual_supply_deliveries,
                       server_bot_brain_mode_name,
                       signal_intelligence_backend_name(),
                       signal_intelligence_flight_builtin_available() ? "true" : "false",
                       signal_intelligence_flight_feature_set(),
                       (unsigned)signal_intelligence_flight_feature_encoder_version(),
                       signal_intelligence_flight_checkpoint_hash(),
                       signal_intelligence_flight_loaded() ? "true" : "false",
                       (unsigned long long)signal_intelligence_flight_inference_count(),
                       signal_intelligence_contract_loaded() ? "true" : "false",
                       (unsigned long long)signal_intelligence_contract_inference_count(),
                       (unsigned long long)signal_intelligence_contract_decision_count(),
                       (unsigned long long)signal_intelligence_contract_teacher_decision_count(),
                       signal_intelligence_npc_worker_loaded() ? "true" : "false",
                       (unsigned long long)signal_intelligence_npc_worker_inference_count(),
                       (unsigned long long)signal_intelligence_npc_worker_decision_count(),
                       (unsigned long long)signal_intelligence_npc_worker_teacher_decision_count(),
                       version,
                       "local",
                       "true",
                       "true",
                       "false",
                       "none");
            BUF_APPEND(pos, buf, HEALTH_BUFSZ, "\",\"data_dir\":\"");
            BUF_APPEND(pos, buf, HEALTH_BUFSZ,
                       "\"},"
                       "\"signed_action_count\":%llu,"
                       "\"signed_action_reject_count\":%llu,"
                       "\"unsigned_action_count\":%llu,"
                       "\"hopper_smelt_events\":%llu,"
                       "\"hopper_smelt_units\":%.3f,"
                       "\"chain\":{\"status\":\"%s\",\"chain_dir\":\"",
                       (unsigned long long)signed_action_count,
                       (unsigned long long)signed_action_reject_count,
                       (unsigned long long)unsigned_action_count,
                       (unsigned long long)world.hopper_smelt_events,
                       world.hopper_smelt_units,
                       chain_health);
            json_escape_append(buf, &pos, HEALTH_BUFSZ, chain_log_get_dir());
            BUF_APPEND(pos, buf, HEALTH_BUFSZ,
                       "\",\"stations\":%d,\"blocked_stations\":%d,"
                       "\"warning_stations\":%d,\"station_status\":[",
                       chain_station_count, chain_blocked_count,
                       chain_warning_count);
            bool first_chain_station = true;
            for (int i = 0; i < MAX_STATIONS; i++) {
                const station_t *st = &world.stations[i];
                if (memcmp(st->station_pubkey, zero_pub, 32) == 0) continue;
                if (!first_chain_station) BUF_APPEND(pos, buf, HEALTH_BUFSZ, ",");
                first_chain_station = false;
                BUF_APPEND(pos, buf, HEALTH_BUFSZ,
                           "{\"index\":%d,\"name\":\"", i);
                json_escape_append(buf, &pos, HEALTH_BUFSZ, st->name);
                BUF_APPEND(pos, buf, HEALTH_BUFSZ,
                           "\",\"health\":\"%s\",\"append_blocked\":%s,"
                           "\"event_count\":%llu,\"verified_event_count\":%llu,"
                           "\"message\":\"",
                           chain_log_health_status_name(
                               (chain_health_status_t)st->chain_health_status),
                           st->chain_append_blocked ? "true" : "false",
                           (unsigned long long)st->chain_event_count,
                           (unsigned long long)st->chain_verified_event_count);
                json_escape_append(buf, &pos, HEALTH_BUFSZ, st->chain_health_message);
                BUF_APPEND(pos, buf, HEALTH_BUFSZ, "\",\"repair_hint\":\"");
                json_escape_append(buf, &pos, HEALTH_BUFSZ,
                                   chain_log_health_repair_hint(
                                       (chain_health_status_t)st->chain_health_status,
                                       st->chain_append_blocked));
                BUF_APPEND(pos, buf, HEALTH_BUFSZ, "\"}");
            }
            BUF_APPEND(pos, buf, HEALTH_BUFSZ, "]}}");
            mg_http_reply(c, 200, api_headers, "%s", buf);
            free(buf);
        } else if (mg_match(hm->uri, mg_str("/internal/v1/operator-post"), NULL)) {
            if (!internal_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                /* Parse JSON payload: { station_index, kind, tier, ref_id, text } */
                double station_idx_val = 0;
                mg_json_get_num(hm->body, "$.station_index", &station_idx_val);
                int station_idx = (int)station_idx_val;

                double kind_val = 0;
                mg_json_get_num(hm->body, "$.kind", &kind_val);
                uint8_t kind = (uint8_t)kind_val;

                double tier_val = 0;
                mg_json_get_num(hm->body, "$.tier", &tier_val);
                uint8_t tier = (uint8_t)tier_val;

                double ref_id_val = 0;
                mg_json_get_num(hm->body, "$.ref_id", &ref_id_val);
                uint16_t ref_id = (uint16_t)ref_id_val;

                char *text = mg_json_get_str(hm->body, "$.text");

                uint64_t event_id = 0;
                const char *err = NULL;
                if (emit_operator_post_for_station(station_idx, kind, tier, ref_id,
                                                   text, &event_id, &err)) {
                    mg_http_reply(c, 200, api_headers,
                                  "{\"ok\":true,\"event_id\":%llu,\"prev_hash\":\"%lX\"}",
                                  (unsigned long long)event_id,
                                  (unsigned long)(world.stations[station_idx].chain_last_hash[0]));
                } else {
                    mg_http_reply(c, 400, api_headers,
                                  "{\"error\":\"%s\"}", err ? err : "operator post failed");
                }
                if (text) free(text);
            }
        } else if (!serve_static_http(c, hm)) {
            mg_http_reply(c, 404, "", "Not found");
        }
    } else if (ev == MG_EV_WS_OPEN) {
        /* Per-IP connection limit to mitigate slot exhaustion */
        #define MAX_CONNS_PER_IP 4
        {
            int ip_count = 0;
            uint64_t client_addr_key = server_connection_limit_key(c);
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (world.players[i].connected &&
                    world.players[i].connection->conn &&
                    world.players[i].connection->client_addr_key_valid &&
                    world.players[i].connection->client_addr_key == client_addr_key) {
                    ip_count++;
                }
            }
            if (ip_count >= MAX_CONNS_PER_IP) {
                printf("[server] per-IP limit reached for connection, rejecting\n");
                mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
                return;
            }
        }
        int pid = alloc_player();
        if (pid < 0) {
            mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
            return;
        }
        server_player_t *sp = &world.players[pid];
        reset_player_slot_for_reuse(pid);
        sp->connected = true;
        sp->id = (uint8_t)pid;
        sp->connection->conn = c;
        sp->connection->client_addr_key = server_connection_limit_key(c);
        sp->connection->client_addr_key_valid = true;
        sp->session_ready = false;
        sp->grace_timer = 5.0f;  /* Must send SESSION within 5 seconds */
        sp->connection->analytics_connected_ms = mg_millis();
        sp->connection->analytics_last_activity_ms = sp->connection->analytics_connected_ms;
        /* Start with fresh ship — save is loaded when client sends SESSION */
        player_init_ship(sp, &world);
        printf("[server] player %d: awaiting session token\n", pid);
        analytics_log_player_event("player_connect", pid, sp,
                                   sp->connection->analytics_connected_ms, 0);

        /* Send JOIN to new player (their own ID). */
        uint8_t join_msg[] = { NET_MSG_JOIN, (uint8_t)pid };
        ws_send(c, join_msg, 2);

        /* Send protocol discovery before the large world snapshots so
         * clients/tools can validate stream sizes and cadences up front. */
        {
            uint8_t proto_msg[PROTOCOL_INFO_SIZE];
            int proto_len = serialize_protocol_info(
                proto_msg, SIM_TICK_MS, STATE_TICK_MS, WORLD_TICK_MS,
                SHIP_TICK_MS, STATION_DIAG_MIN_MS,
                STATION_IDENTITY_FALLBACK_MS);
            if (proto_len >= PROTOCOL_INFO_HEADER_SIZE)
                ws_send(c, proto_msg, (size_t)proto_len);
        }

        /* Tell new player about existing gameplay-ready players. Others learn
         * about this slot after SESSION is accepted. */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pid || !server_player_is_gameplay_ready(&world.players[i]))
                continue;
            uint8_t exist_msg[] = { NET_MSG_JOIN, (uint8_t)i };
            ws_send(c, exist_msg, 2);
        }

        /* Send server version hash. */
        {
#ifdef GIT_HASH
            const char *hash = GIT_HASH;
#else
            const char *hash = "dev";
#endif
            size_t hlen = strlen(hash);
            uint8_t info_msg[12] = { NET_MSG_SERVER_INFO };
            if (hlen > 11) hlen = 11;
            memcpy(&info_msg[1], hash, hlen);
            ws_send(c, info_msg, 1 + hlen);
        }

        printf("[server] player %d joined\n", pid);
    } else if (ev == MG_EV_WS_MSG) {
        handle_ws_message(c, ev_data);
    } else if (ev == MG_EV_CLOSE) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (world.players[i].connection->conn == c) {
                uint64_t now_ms = mg_millis();
                uint64_t duration_ms =
                    (world.players[i].connection->analytics_connected_ms != 0 &&
                     now_ms >= world.players[i].connection->analytics_connected_ms)
                    ? now_ms - world.players[i].connection->analytics_connected_ms
                    : 0;
                analytics_log_player_event("player_disconnect", i,
                                           &world.players[i], now_ms,
                                           duration_ms);
                if (world.players[i].session_ready)
                    player_save(&world.players[i], PLAYER_SAVE_DIR, i);
                world.players[i].connection->conn = NULL;
                if (world.players[i].session_ready) {
                    /* Keep slot alive for reconnect grace window */
                    world.players[i].grace_period = true;
                    world.players[i].grace_timer = 30.0f;
                    printf("[server] player %d disconnected, grace window 30s\n", i);
                } else {
                    /* No session — immediate full disconnect */
                    (void)world_player_release_ship_asset(&world, i);
                    world.players[i].connected = false;
                    world_character_unbind_player(&world, i);
                    server_player_clear_live_session_identity(&world.players[i]);
                    server_player_clear_transient_input(&world.players[i]);
                    uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                    broadcast(leave_msg, 2);
                    printf("[server] player %d left (no session)\n", i);
                }
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Broadcast helpers                                                  */
/* ------------------------------------------------------------------ */

static void broadcast_player_states(uint64_t now) {
    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    uint8_t dock_buf[PLAYER_DOCK_MSG_HEADER +
                     MAX_PLAYERS * PLAYER_DOCK_RECORD_SIZE];
    uint8_t motion_buf[PLAYER_MOTION_Q_MSG_HEADER +
                       MAX_PLAYERS * PLAYER_MOTION_Q_RECORD_SIZE];
    uint8_t motion_mixed_buf[PLAYER_MOTIONM_Q_MSG_HEADER +
                             MAX_PLAYERS *
                             PLAYER_MOTIONM_Q_MAX_RECORD_SIZE];
    uint32_t server_tick = world.tick;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (!server_player_is_gameplay_ready(sp) || !sp->connection->conn) continue;
        int len = serialize_player_states_except_recipient(
            buf, world.players, i, server_tick);
        bool sent_full = ws_send_player_states_if_changed(
            sp->connection->conn, sp, buf, (size_t)len, now);
        if (sent_full) {
            server_player_motion_delta_clear_all(sp);
        }
        int dock_len = serialize_player_dock_status_for_recipient(
            dock_buf, world.players, i);
        ws_send_player_dock_if_changed(
            sp->connection->conn, sp, dock_buf, (size_t)dock_len);
        if (sent_full) {
            sp->replication->world_player_motion_last_sent_ms = now;
        } else if (sp->replication->world_player_motion_last_sent_ms == 0 ||
                   now - sp->replication->world_player_motion_last_sent_ms >=
                       PLAYER_MOTION_SEND_INTERVAL_MS) {
            int motion_len = 0;
            int motion_mixed_len = 0;
            bool motion_heartbeat_due = false;
            serialize_player_motion_mixed_q_for_recipient(
                motion_buf, &motion_len,
                motion_mixed_buf, &motion_mixed_len,
                &motion_heartbeat_due,
                sp, world.players, i, server_tick);
            bool empty_motion =
                motion_len <= PLAYER_MOTION_Q_MSG_HEADER &&
                motion_mixed_len <= PLAYER_MOTIONM_Q_MSG_HEADER;
            bool sent_motion = false;
            if (ws_send_player_motion_if_changed(
                    sp->connection->conn, sp, motion_buf, (size_t)motion_len)) {
                server_player_motion_delta_note_abs_msg(
                    sp, motion_buf, (size_t)motion_len, server_tick);
                sp->replication->world_player_motion_delta_cache.valid = false;
                sp->replication->world_player_motion_posed_cache.valid = false;
                sp->replication->player_motion_delta_heartbeat_tick = server_tick;
                sent_motion = true;
            }
            if (ws_send_player_motion_mixed(
                    sp->connection->conn, sp, motion_mixed_buf,
                    (size_t)motion_mixed_len)) {
                server_player_motion_delta_note_mixed_msg(
                    sp, motion_mixed_buf, (size_t)motion_mixed_len,
                    server_tick);
                if (motion_heartbeat_due)
                    sp->replication->player_motion_delta_heartbeat_tick = server_tick;
                sent_motion = true;
            }
            if (sent_motion || empty_motion) {
                sp->replication->world_player_motion_last_sent_ms = now;
            }
        }
    }
}

/* mark_visible_asteroids_dirty removed — per-player relevance filtering
 * in serialize_asteroids_for_player handles viewport culling. */

static void broadcast_world(void) {
    for (int p = 0; p < MAX_PLAYERS; p++) {
        server_player_t *sp = &world.players[p];
        if (!sp->connected || !sp->session_ready || !sp->connection->conn) continue;
        ws_private_packet_sink_t sink = {
            .conn = sp->connection->conn,
            .player = sp,
            .world_tick = world.tick,
        };
        server_emit_world_snapshot_for_player(
            &world, p, false, ws_world_packet_sink, &sink,
            &world_snapshot_scratch);
    }
    server_clear_asteroid_net_dirty(&world);
}

static int send_player_ship(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    return serialize_player_ship_for_world(buf, id, &world, sp);
}

static void broadcast_ship_states(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (!sp->connected || !sp->session_ready || !sp->connection->conn) continue;
        if (sp->replication->force_authoritative_resync)
            invalidate_player_authoritative_caches(sp);
        ws_private_packet_sink_t sink = {
            .conn = sp->connection->conn,
            .player = sp,
        };
        server_emit_private_snapshot_for_player(
            &world, i, true, ws_private_packet_sink, &sink,
            &private_snapshot_scratch);
        sp->replication->force_authoritative_resync = false;
    }

    if (station_econ_dirty) {
        uint8_t sbuf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
        int slen = serialize_stations(sbuf, world.stations);
        broadcast(sbuf, (size_t)slen);
        station_econ_dirty = false;
    }

    if (contracts_dirty) {
        uint8_t cbuf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
        int clen = serialize_contracts(cbuf, world.contracts);
        broadcast_contracts_if_changed(cbuf, (size_t)clen, mg_millis());
        contracts_dirty = false;
    }
}

/* ================================================================== */
/* sim_event handlers — per-event broadcast logic invoked inside the   */
/* main sim loop. Each takes the live event by const pointer.          */
/* ================================================================== */

static void srv_on_outpost_placed(const sim_event_t *ev) {
    int slot = ev->outpost_placed.slot;
    uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
    int id_len = serialize_station_identity(id_buf, slot, &world.stations[slot]);
    broadcast(id_buf, (size_t)id_len);
    station_identity_dirty[slot] = true;
    station_econ_dirty = true;
    contracts_dirty = true;
}

/* SELL / REPAIR / UPGRADE / DOCK / LAUNCH all need the player's ship +
 * current-station record pushed immediately so cargo / credits / hull /
 * dock status don't sit stale for the SHIP_TICK_MS window. */
static void srv_on_player_state_change(const sim_event_t *ev) {
    int pid = ev->player_id;
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->connected || !sp->connection->conn) {
        station_econ_dirty = true;
        contracts_dirty = true;
        return;
    }
    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->connection->conn, buf, (size_t)len);

    int st_idx = sp->current_station;
    if (st_idx >= 0 && st_idx < MAX_STATIONS) {
        uint8_t sbuf[2 + STATION_RECORD_SIZE];
        sbuf[0] = NET_MSG_WORLD_STATIONS;
        sbuf[1] = 1;
        uint8_t *p = &sbuf[2];
        p[0] = (uint8_t)st_idx;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            write_f32_le(&p[1 + c * 4],
                         station_inventory_amount(&world.stations[st_idx],
                                                  (commodity_t)c));
        ws_send(sp->connection->conn, sbuf, (size_t)(2 + STATION_RECORD_SIZE));
    }
    station_econ_dirty = true;
    contracts_dirty = true;
}

/* SIM_EVENT_DEATH: emit signed CHAIN_EVT_DEATH (highscore is replayed
 * from the chain log) + highscore submission + per-player death packet
 * (carries pos/vel/stats so the client cinematic anchors at the
 * wreckage before the server respawn moves the ship) + fresh ship
 * state so the post-respawn hull/dock is visible immediately. */
static void srv_on_death(const sim_event_t *ev) {
    int pid = ev->player_id;
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->connected) return;

    /* Resolve killer callsign by token against currently-connected
     * peers BEFORE chain emit so the killed_by_callsign rides along on
     * the persisted DEATH event. The replay walker reads it directly;
     * legacy events without this field fall back to the victim-callsign
     * map. */
    uint8_t killed_by[8] = {0};
    {
        static const uint8_t zero[8] = {0};
        if (memcmp(ev->death.killer_token, zero, 8) != 0) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (i == pid) continue;
                if (memcmp(world.players[i].session_token,
                           ev->death.killer_token, 8) == 0) {
                    memcpy(killed_by, world.players[i].callsign, 8);
                    break;
                }
            }
        }
    }

    /* Append the run to the chain log first so a crash between emit
     * and submit re-projects on next boot. Station 0 (Prospect) is the
     * deterministic authority for player-life events — the chain log
     * is a single-stream view of the world's history per station, and
     * deaths aren't per-station the way smelts are. */
    {
        chain_payload_death_t dp;
        memset(&dp, 0, sizeof(dp));
        if (sp->pubkey_set) memcpy(dp.victim_pubkey, sp->pubkey, 32);
        memcpy(dp.victim_session_token, sp->session_token, 8);
        memcpy(dp.victim_callsign, sp->callsign, 8);
        memcpy(dp.killer_token, ev->death.killer_token, 8);
        dp.cause = ev->death.cause;
        dp.epoch_tick = (uint64_t)world.tick;
        dp.credits_earned = ev->death.credits_earned;
        dp.credits_spent = ev->death.credits_spent;
        dp.ore_mined = ev->death.ore_mined;
        dp.asteroids_fractured = (uint32_t)ev->death.asteroids_fractured;
        memcpy(dp.killed_by_callsign, killed_by, 8);
        if (station_exists(&world.stations[0]))
            (void)chain_log_emit(&world, &world.stations[0],
                                 CHAIN_EVT_DEATH, &dp, (uint16_t)sizeof(dp));
    }

    const char *cs = sp->callsign;
    uint32_t world_id = world.belt_seed;
    uint32_t world_seq = world.world_seq;
    uint32_t build_id = signal_build_id_u32();
    uint64_t epoch_tick = (uint64_t)world.tick;
    bool qualified = highscore_submit(&highscores, cs, ev->death.credits_earned,
                                      world_id, world_seq, build_id,
                                      epoch_tick, killed_by);
    printf("[server] death pid=%d cs=%s earned=%.0f cr -> %s (top=%d)\n",
           pid, cs[0] ? cs : "?", ev->death.credits_earned,
           qualified ? "qualified" : "skipped", highscores.count);
    if (qualified) highscores_dirty = true;

    if (!sp->connection->conn) return;

    uint8_t msg[NET_DEATH_MSG_SIZE];
    int death_len = serialize_death(msg, (uint8_t)pid, ev);
    ws_send(sp->connection->conn, msg, (size_t)death_len);

    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->connection->conn, buf, (size_t)len);
}

static void srv_on_contract_complete(const sim_event_t *ev) {
    (void)ev;
    station_econ_dirty = true;
    contracts_dirty = true;
}

static void srv_on_hail_response(const sim_event_t *ev) {
    int pid = ev->player_id;
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->connected || !sp->connection->conn) return;

    uint8_t msg[NET_HAIL_RESPONSE_REASON_SIZE];
    int msg_len = serialize_hail_response_for_world(msg, &world, ev);
    if (msg_len > 0) ws_send(sp->connection->conn, msg, (size_t)msg_len);

    contracts_dirty = true;
    /* Push fresh ship state so the credit bump is visible immediately. */
    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->connection->conn, buf, (size_t)len);
}

/* OUTPOST_PLACED / OUTPOST_ACTIVATED / MODULE_ACTIVATED / SCAFFOLD_READY
 * all need station identity refreshed so the client sees updated
 * module / pending lists. */
static void srv_mark_all_stations_identity_dirty(void) {
    for (int s = 0; s < MAX_STATIONS; s++) station_identity_dirty[s] = true;
}

static void srv_hook_outpost_placed(void *user, const sim_event_t *ev) {
    (void)user;
    srv_on_outpost_placed(ev);
}

static void srv_hook_player_state_change(void *user, const sim_event_t *ev) {
    (void)user;
    srv_on_player_state_change(ev);
}

static void srv_hook_death(void *user, const sim_event_t *ev) {
    (void)user;
    srv_on_death(ev);
}

static void srv_hook_contract_complete(void *user, const sim_event_t *ev) {
    (void)user;
    srv_on_contract_complete(ev);
}

static void srv_hook_hail_response(void *user, const sim_event_t *ev) {
    (void)user;
    srv_on_hail_response(ev);
}

static void srv_hook_structure_dirty(void *user, const sim_event_t *ev) {
    (void)user;
    (void)ev;
    srv_mark_all_stations_identity_dirty();
}

static const server_sim_event_hooks_t srv_sim_event_hooks = {
    .outpost_placed = srv_hook_outpost_placed,
    .player_state_change = srv_hook_player_state_change,
    .death = srv_hook_death,
    .contract_complete = srv_hook_contract_complete,
    .hail_response = srv_hook_hail_response,
    .structure_dirty = srv_hook_structure_dirty,
};

/* Fan a single sim event out to its per-type broadcaster(s). Shared
 * routing keeps MP and loopback aligned on which events have extra
 * transport side effects; multiple hook buckets may fire for one event. */
static void srv_dispatch_sim_event(const sim_event_t *ev) {
    server_process_sim_event_transport(ev, &srv_sim_event_hooks, NULL);
}

/* ================================================================== */
/* Bootstrap helpers — pulled out of main() for clarity.               */
/* ================================================================== */

/* Read PORT, SIGNAL_API_TOKEN, SIGNAL_ALLOWED_ORIGIN, SIGNAL_REQUIRE_API_TOKEN
 * env vars and stamp the listen URL + CORS headers. Returns false (and
 * caller should exit nonzero) if SIGNAL_REQUIRE_API_TOKEN is set without
 * a token. listen_url is sized by the caller. */
static bool read_env_config(char *listen_url, size_t listen_url_size) {
    const char *port = getenv("PORT");
    if (!port || port[0] == '\0') port = "8080";
    static_root_dir = getenv("SIGNAL_STATIC_DIR");
    persistence_data_dir = getenv("SIGNAL_DATA_DIR");
    if (!persistence_data_dir || persistence_data_dir[0] == '\0') 
        persistence_data_dir = ".";
    trust_proxy_headers = env_truthy(getenv("SIGNAL_TRUST_PROXY_HEADERS"));
    printf("[server] Persistence: local (data_dir=%s)\n", persistence_data_dir);
    if (trust_proxy_headers)
        printf("[server] Trusted proxy client-IP headers enabled\n");
    if (static_root_dir && static_root_dir[0] != '\0')
        printf("[server] Static web root: %s\n", static_root_dir);
    if (!read_u32_env("SIGNAL_WORLD_SEED", &fresh_world_seed_override) ||
        !read_u32_env("SIGNAL_WORLD_SEQ", &fresh_world_seq_override)) {
        return false;
    }
    if (fresh_world_seed_override) {
        printf("[server] Fresh world seed override: %u\n",
               fresh_world_seed_override);
    }
    if (fresh_world_seq_override) {
        printf("[server] Fresh world sequence override: %u\n",
               fresh_world_seq_override);
    }
    {
    }
    {
        const char *bots = getenv("SIGNAL_BOT_PLAYERS");
        if (bots && bots[0] != '\0') {
            char *end = NULL;
            errno = 0;
            unsigned long count = strtoul(bots, &end, 10);
            if (errno != 0 || end == bots || *end != '\0' ||
                count > (unsigned long)MAX_PLAYERS) {
                fprintf(stderr, "[FATAL] invalid SIGNAL_BOT_PLAYERS=%s "
                                "(use 0..%d)\n",
                        bots, MAX_PLAYERS);
                return false;
            }
            server_bot_player_target = (int)count;
        }
        if (server_bot_player_target > 0) {
            printf("[server] Server bot players: requested %d/%d slot(s)\n",
                   server_bot_player_target, MAX_PLAYERS);
        }
    }
    {
        const char *frontier = getenv("SIGNAL_FRONTIER_VIRTUAL_PILOTS");
        if (frontier && frontier[0] != '\0') {
            char *end = NULL;
            errno = 0;
            unsigned long count = strtoul(frontier, &end, 10);
            if (errno != 0 || end == frontier || *end != '\0' ||
                count > (unsigned long)SIGNAL_FRONTIER_VIRTUAL_PILOTS_MAX) {
                fprintf(stderr, "[FATAL] invalid SIGNAL_FRONTIER_VIRTUAL_PILOTS=%s "
                                "(use 0..%d)\n",
                        frontier, SIGNAL_FRONTIER_VIRTUAL_PILOTS_MAX);
                return false;
            }
            frontier_virtual_pilot_target = (int)count;
        }
        if (frontier_virtual_pilot_target > 0) {
            printf("[server] Frontier virtual pilots: %d aggregate pilot(s)\n",
                   frontier_virtual_pilot_target);
        }
    }
    {
        const char *mode_name = getenv("SIGNAL_BOT_BRAIN_MODE");
        if (!mode_name || mode_name[0] == '\0') mode_name = "autopilot";
        if (strcmp(mode_name, "autopilot") == 0) {
            server_bot_brain_mode = SERVER_BRAIN_MODE_NONE;
            server_bot_brain_mode_name = "autopilot";
        } else if (strcmp(mode_name, "heuristic") == 0) {
            server_bot_brain_mode = SERVER_BRAIN_MODE_HEURISTIC_LOGISTICS;
            server_bot_brain_mode_name = "heuristic";
        } else if (strcmp(mode_name, "neural") == 0) {
            server_bot_brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
            server_bot_brain_mode_name = "neural";
        } else {
            fprintf(stderr, "[FATAL] invalid SIGNAL_BOT_BRAIN_MODE=%s "
                            "(use autopilot, heuristic, or neural)\n",
                    mode_name);
            return false;
        }
        server_bot_brain_checkpoint = getenv("SIGNAL_BOT_BRAIN_CHECKPOINT");
        server_bot_contract_brain_checkpoint = getenv("SIGNAL_BOT_CONTRACT_BRAIN_CHECKPOINT");
        server_bot_npc_worker_brain_checkpoint =
            getenv("SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT");
        if (server_bot_brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT &&
            (!server_bot_brain_checkpoint || server_bot_brain_checkpoint[0] == '\0') &&
            !signal_intelligence_flight_builtin_available()) {
            fprintf(stderr, "[FATAL] SIGNAL_BOT_BRAIN_MODE=neural requires "
                            "SIGNAL_BOT_BRAIN_CHECKPOINT or a linked "
                            "CRLPLRIMES static flight bundle\n");
            return false;
        }
        if (server_bot_player_target > 0) {
            printf("[server] Server bot brain mode: %s\n", server_bot_brain_mode_name);
            if (server_bot_brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT) {
                if (server_bot_brain_checkpoint &&
                    server_bot_brain_checkpoint[0] != '\0') {
                    printf("[server] Server bot flight brain checkpoint: %s\n",
                           server_bot_brain_checkpoint);
                } else if (signal_intelligence_flight_builtin_available()) {
                    printf("[server] Server bot flight brain: built-in CRLPLRIMES "
                           "%s encoder=%u hash=%.12s\n",
                           signal_intelligence_flight_feature_set(),
                           (unsigned)signal_intelligence_flight_feature_encoder_version(),
                           signal_intelligence_flight_checkpoint_hash());
                }
            }
            if (server_bot_contract_brain_checkpoint &&
                server_bot_contract_brain_checkpoint[0] != '\0') {
                printf("[server] Server bot contract brain checkpoint: %s\n",
                       server_bot_contract_brain_checkpoint);
            } else if (server_bot_brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT) {
                printf("[server] Server bot contract brain: teacher fallback\n");
            }
            if (server_bot_npc_worker_brain_checkpoint &&
                server_bot_npc_worker_brain_checkpoint[0] != '\0') {
                printf("[server] Server bot NPC worker brain checkpoint: %s\n",
                       server_bot_npc_worker_brain_checkpoint);
            }
        }
    }
    api_token = getenv("SIGNAL_API_TOKEN");
    if (api_token && api_token[0] != '\0') {
        printf("[server] Station API enabled (token set)\n");
    } else {
        fprintf(stderr, "[WARN] SIGNAL_API_TOKEN is unset -- REST API will reject all requests\n");
        if (getenv("SIGNAL_REQUIRE_API_TOKEN")) {
            fprintf(stderr, "[FATAL] SIGNAL_REQUIRE_API_TOKEN set but no token provided\n");
            return false;
        }
    }
    {
        const char *station_auth_secret = getenv("SIGNAL_STATION_AUTH_SECRET");
        if (station_auth_secret && station_auth_secret[0] != '\0') {
            station_authority_configure_secret(station_auth_secret);
            printf("[server] Station authority secret configured from SIGNAL_STATION_AUTH_SECRET\n");
        } else if (api_token && api_token[0] != '\0') {
            station_authority_configure_secret(api_token);
            printf("[server] Station authority secret derived from SIGNAL_API_TOKEN\n");
        } else {
            bool allow_dev = env_truthy(
                getenv("SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET"));
            if (!allow_dev ||
                env_truthy(getenv("SIGNAL_REQUIRE_STATION_AUTH_SECRET"))) {
                fprintf(stderr,
                        "[FATAL] station authority requires "
                        "SIGNAL_STATION_AUTH_SECRET or SIGNAL_API_TOKEN; "
                        "set SIGNAL_ALLOW_DEV_STATION_AUTH_SECRET=1 only for "
                        "disposable local worlds\n");
                return false;
            }
            station_authority_use_dev_secret();
            fprintf(stderr, "[WARN] using deterministic development station authority secret\n");
        }
    }
    internal_token = getenv("SIGNAL_INTERNAL_SHARED_KEY");
    if (internal_token && internal_token[0] != '\0') {
        printf("[server] Internal operator-post endpoint enabled (token set)\n");
    } else {
        fprintf(stderr, "[WARN] SIGNAL_INTERNAL_SHARED_KEY is unset -- /internal/v1/operator-post will reject all requests\n");
    }
    allowed_origin = getenv("SIGNAL_ALLOWED_ORIGIN");
    if (!allowed_origin) allowed_origin = "*";
    snprintf(api_headers, sizeof(api_headers),
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Cache-Control: no-store\r\n", allowed_origin);
    printf("[server] CORS origin: %s\n", allowed_origin);
    snprintf(listen_url, listen_url_size, "http://0.0.0.0:%s", port);
    return true;
}

static void ensure_persistence_dirs(void) {
    MKDIR_PATH(PLAYER_SAVE_DIR);
    MKDIR_PATH(STATION_CATALOG_DIR);
    /* Layer A.4 of #479: ensure pubkey/ + legacy/ subdirs exist and any
     * existing top-level *.sav files (v39 and earlier layout) get moved
     * into legacy/ so the new path layout takes effect. Idempotent. */
    player_save_migrate_legacy_layout(PLAYER_SAVE_DIR);
}

static bool enter_persistence_data_dir(void) {
    if (!persistence_data_dir || persistence_data_dir[0] == '\0' ||
        strcmp(persistence_data_dir, ".") == 0) {
        return true;
    }

    struct stat st;
    if (stat(persistence_data_dir, &st) != 0) {
        if (MKDIR_PATH(persistence_data_dir) != 0 && errno != EEXIST) {
            fprintf(stderr, "[FATAL] could not create SIGNAL_DATA_DIR=%s: %s\n",
                    persistence_data_dir, strerror(errno));
            return false;
        }
    } else if (!PATH_IS_DIR(st.st_mode)) {
        fprintf(stderr, "[FATAL] SIGNAL_DATA_DIR=%s is not a directory\n",
                persistence_data_dir);
        return false;
    }

    if (CHDIR_PATH(persistence_data_dir) != 0) {
        fprintf(stderr, "[FATAL] could not enter SIGNAL_DATA_DIR=%s: %s\n",
                persistence_data_dir, strerror(errno));
        return false;
    }
    printf("[server] Persistence data cwd: %s\n", persistence_data_dir);
    return true;
}

static void emit_world_identity_anchor(void);

static void server_seed_worker_trace_contract(npc_ship_t *npc,
                                              const contract_t *ct) {
    if (!npc || !ct || !ct->active) return;
    contract_summary_t summary = contract_summary_make(ct);
    knowledge_view_configure(&npc->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    (void)knowledge_view_insert_contract(&npc->ship->knowledge, &summary);
}

static void server_seed_worker_trace_npc(int station_idx,
                                         npc_role_t role,
                                         const contract_t *ct0,
                                         const contract_t *ct1) {
    int slot = spawn_npc(&world, station_idx, role);
    if (slot < 0) return;
    npc_ship_t *npc = &world.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->dest_station = station_idx;
    npc->pickup_station = -1;
    npc->pickup_commodity = COMMODITY_COUNT;
    npc->brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    memset(&npc->ship->knowledge, 0, sizeof(npc->ship->knowledge));
    knowledge_view_configure(&npc->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    server_seed_worker_trace_contract(npc, ct0);
    server_seed_worker_trace_contract(npc, ct1);
    if (station_idx >= 0 && station_idx < MAX_STATIONS)
        ledger_earn(&world.stations[station_idx], npc->session_token, 1200.0f);
}

static void server_apply_npc_worker_trace_fixture(void) {
    const char *scenario = getenv("SIGNAL_NPC_WORKER_TRACE_SCENARIO");
    if (!scenario || strcmp(scenario, "rich") != 0) return;

    uint8_t origin[8] = { 'W','O','R','K','E','R','v','1' };
    (void)station_finished_mint(&world.stations[0], COMMODITY_FERRITE_INGOT, 28, origin);
    (void)station_finished_mint(&world.stations[0], COMMODITY_FRAME, 16, origin);
    (void)station_finished_mint(&world.stations[2], COMMODITY_LASER_MODULE, 16, origin);
    (void)station_finished_mint(&world.stations[2], COMMODITY_TRACTOR_MODULE, 16, origin);
    world.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 80.0f;
    world.stations[2]._inventory_cache[COMMODITY_CUPRITE_ORE] = 80.0f;
    world.stations[2]._inventory_cache[COMMODITY_CRYSTAL_ORE] = 80.0f;

    memset(world.contracts, 0, sizeof(world.contracts));
    world.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 6.0f,
        .base_price = 38.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    world.contracts[1] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_LASER_MODULE,
        .quantity_needed = 4.0f,
        .base_price = 72.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    world.contracts[2] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_TRACTOR_MODULE,
        .quantity_needed = 4.0f,
        .base_price = 78.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    server_seed_worker_trace_npc(0, NPC_ROLE_HAULER, &world.contracts[0], NULL);
    server_seed_worker_trace_npc(1, NPC_ROLE_HAULER, &world.contracts[0],
                                 &world.contracts[2]);
    server_seed_worker_trace_npc(2, NPC_ROLE_HAULER, &world.contracts[1],
                                 &world.contracts[2]);
    server_seed_worker_trace_npc(0, NPC_ROLE_MINER, &world.contracts[0], NULL);
    server_seed_worker_trace_npc(2, NPC_ROLE_MINER, &world.contracts[1], NULL);

    printf("[server] seeded rich NPC worker trace scenario\n");
}

/* Layered persistence (#314):
 *   1. world_reset() seeds starter stations + belt field
 *   2. Catalog overwrites identity for any persisted stations
 *   3. Session snapshot overlays economy state
 *   4. Rebuild derived structures (signal chain, station nav, hash chain) */
static bool load_world_state(void) {
    /* Belt seed is persistent across normal restarts: rotate only when
     * world.sav is absent (true first boot of a fresh world). On a
     * resume, world_load below overwrites belt_seed and world_seq with
     * the persisted values so asteroid layout, station Ed25519 pubkeys,
     * and the leaderboard's world ordering all stay stable. */
    bool fresh_world = true;
    FILE *probe = fopen(SAVE_PATH, "rb");
    if (probe) {
        fclose(probe);
        fresh_world = false;
    }
    if (fresh_world) {
        world.rng = fresh_world_seed_override
            ? fresh_world_seed_override
            : (uint32_t)time(NULL);
        if (!world.rng) world.rng = 2037u;
    }
    world_reset(&world);

    int catalog_count = station_catalog_load_all(world.stations, MAX_STATIONS,
                                                 STATION_CATALOG_DIR);
    if (catalog_count > 0)
        printf("[server] loaded %d station(s) from catalog\n", catalog_count);

    if (!fresh_world) {
        if (!world_load(&world, SAVE_PATH)) {
            fprintf(stderr,
                    "[FATAL] %s exists but is invalid or unreadable; refusing "
                    "to replace it with a fresh world\n",
                    SAVE_PATH);
            return false;
        }
        printf("[server] loaded session from %s (belt_seed=%u world_seq=%u)\n",
               SAVE_PATH, world.belt_seed, world.world_seq);
    } else {
        /* fresh_world above already rotated rng; stamp world_seq from the
         * wall clock so cross-wipe ordering is monotonic too. */
        world.world_seq = fresh_world_seq_override
            ? fresh_world_seq_override
            : (uint32_t)time(NULL);
        if (!world.world_seq) world.world_seq = 1u;
        printf("[server] no session save -- fresh economy (belt_seed=%u world_seq=%u)\n",
               world.belt_seed, world.world_seq);
        /* Stations are sovereign currency issuers; no seed pool. The
         * pool just tracks net issuance from genesis. */
        world_seed_station_manifests(&world);
        /* Emit per-station MOTD + rarity-tier genesis events. Only on
         * fresh-world boots — a resumed world's chain history already
         * contains them from its original genesis. */
        world_seed_station_chain_genesis(&world);
    }

    /* Assign stable IDs to any stations loaded from v1 catalogs (id == 0). */
    if (world.next_station_id == 0) world.next_station_id = 1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_exists(&world.stations[i]) && world.stations[i].id == 0)
            world.stations[i].id = world.next_station_id++;
    }

    /* The station catalog format doesn't persist currency_name (yet)
     * and the catalog loader memsets the whole struct, so the defaults
     * set by world_reset() get wiped. Re-stamp the names for the seeded
     * stations whenever they come back empty. */
    static const char *defaults[SIGNAL_SEEDED_STATION_COUNT] = {
        "prospect vouchers", "kepler bonds", "helios credits", "freeport scrip",
    };
    for (int i = 0; i < SIGNAL_SEEDED_STATION_COUNT && i < MAX_STATIONS; i++) {
        if (!station_exists(&world.stations[i])) continue;
        if (world.stations[i].currency_name[0] == '\0') {
            snprintf(world.stations[i].currency_name,
                     sizeof(world.stations[i].currency_name),
                     "%s", defaults[i]);
            station_identity_dirty[i] = true;
        }
    }

    rebuild_signal_chain(&world);
    station_rebuild_all_nav(&world);
    for (int i = 0; i < MAX_STATIONS; i++) station_identity_dirty[i] = true;

    /* Replay the on-disk hash chain so the Network tab survives a
     * server restart and the chain links continue from where we left
     * off (no fork at the genesis block). */
    signal_chain_load(&world);

    /* Highscores are now a *view* of the chain log: walk every
     * chain/<base58>.log file and project CHAIN_EVT_DEATH events
     * through highscore_submit. Old chain files from prior worlds
     * survive as orphans (their station pubkeys differ once
     * belt_seed rotates) and contribute alongside the current
     * world's runs — each row carries its world_id. */
    highscore_replay_from_chain(&highscores, chain_log_get_dir());
    if (highscores.count > 0)
        printf("[server] replayed %d highscore(s) from chain log\n",
               highscores.count);

    /* Anchor the current world's identity in every station chain. The
     * BUILD_INFO + WORLD_INFO operator posts let replay/analyzer walks
     * tag subsequent events with this world's belt_seed, world_seq, and
     * build SHA. emit_world_identity_anchor is below. */
    emit_world_identity_anchor();
    return true;
}

/* Forward-declared above load_world_state but defined here so it can
 * use chain_log_emit + the static GIT_HASH constant. Idempotent: emits
 * one BUILD_INFO and one WORLD_INFO per station every server start
 * (the chain log grows by 2 events per station per restart, which is
 * fine and gives every station log its own world cursor). */
static void emit_world_identity_anchor_for_station(station_t *st) {
    if (!st || !station_exists(st)) return;
#ifdef GIT_HASH
    const char *hash = GIT_HASH;
#else
    const char *hash = "dev";
#endif
    size_t hash_len = strlen(hash);
    if (hash_len > 64) hash_len = 64;

    /* BUILD_INFO: text = build SHA. */
    {
        uint8_t payload[38 + 64];
        memset(payload, 0, sizeof(payload));
        payload[0] = 3; /* kind = BUILD_INFO */
        sha256_bytes((const uint8_t *)hash, hash_len, &payload[4]);
        payload[36] = (uint8_t)(hash_len & 0xFF);
        payload[37] = (uint8_t)((hash_len >> 8) & 0xFF);
        memcpy(&payload[38], hash, hash_len);
        (void)chain_log_emit(&world, st,
                             CHAIN_EVT_OPERATOR_POST,
                             payload, (uint16_t)(38 + hash_len));
    }
    /* WORLD_INFO: text = belt_seed (4 LE) || world_seq (4 LE) || build SHA.
     * world_seq lets the highscore replay pick the most-recent world's
     * runs over older ones. Pre-v52 emits had only belt_seed; the parser
     * detects the legacy form by text_len < 8 and defaults world_seq=0. */
    {
        uint8_t payload[38 + 8 + 64];
        memset(payload, 0, sizeof(payload));
        payload[0] = 4; /* kind = WORLD_INFO */
        size_t text_len = 8 + hash_len;
        uint8_t text[8 + 64];
        text[0] = (uint8_t)(world.belt_seed & 0xFF);
        text[1] = (uint8_t)((world.belt_seed >> 8) & 0xFF);
        text[2] = (uint8_t)((world.belt_seed >> 16) & 0xFF);
        text[3] = (uint8_t)((world.belt_seed >> 24) & 0xFF);
        text[4] = (uint8_t)(world.world_seq & 0xFF);
        text[5] = (uint8_t)((world.world_seq >> 8) & 0xFF);
        text[6] = (uint8_t)((world.world_seq >> 16) & 0xFF);
        text[7] = (uint8_t)((world.world_seq >> 24) & 0xFF);
        memcpy(&text[8], hash, hash_len);
        sha256_bytes(text, text_len, &payload[4]);
        payload[36] = (uint8_t)(text_len & 0xFF);
        payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
        memcpy(&payload[38], text, text_len);
        (void)chain_log_emit(&world, st,
                             CHAIN_EVT_OPERATOR_POST,
                             payload, (uint16_t)(38 + text_len));
    }
}

static void emit_world_identity_anchor(void) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        emit_world_identity_anchor_for_station(&world.stations[s]);
    }
}

static void mark_station_identity_dirty_for_hull_inventory_changes(void) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        uint8_t current[HULL_CLASS_COUNT] = {0};
        if (station_exists(&world.stations[s])) {
            memcpy(current, world.stations[s].stored_hull_count,
                   sizeof(current));
        }
        if (memcmp(station_hull_inventory_last[s], current,
                   sizeof(current)) == 0) {
            continue;
        }
        memcpy(station_hull_inventory_last[s], current, sizeof(current));
        if (station_exists(&world.stations[s]))
            station_identity_dirty[s] = true;
    }
}

static bool flush_pending_input_ack_for_player(
    server_pending_input_ack_t pending_acks[MAX_PLAYERS],
    int player_slot) {
    if (!pending_acks || player_slot < 0 || player_slot >= MAX_PLAYERS)
        return false;
    server_pending_input_ack_t *pending = &pending_acks[player_slot];
    if (!pending->pending) return false;
    server_player_t *sp = &world.players[player_slot];
    if (!server_player_is_gameplay_ready(sp) || !sp->connection->conn) {
        server_pending_input_ack_reset(pending);
        return false;
    }
    if (!server_emit_pending_input_ack_adaptive(
            pending, sp, (uint8_t)player_slot,
            sp->replication->force_authoritative_resync,
            ws_packet_sink, sp->connection->conn)) {
        return false;
    }
    return true;
}

static void flush_pending_input_acks(
    server_pending_input_ack_t pending_acks[MAX_PLAYERS]) {
    if (!pending_acks) return;
    for (int p = 0; p < MAX_PLAYERS; p++)
        (void)flush_pending_input_ack_for_player(pending_acks, p);
}

static bool run_sim_tick_has_control_barrier(void) {
    if (world.events.count > 0) return true;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (world.players[p].pending_action_result_valid)
            return true;
    }
    return false;
}

static void send_sim_events_to_recipients(const sim_events_t *events) {
    if (!events || events->count <= 0) return;
    uint8_t ebuf[2 + SIM_MAX_EVENTS * NET_EVENT_RECORD_SIZE];
    for (int p = 0; p < MAX_PLAYERS; p++) {
        server_player_t *sp = &world.players[p];
        if (!server_player_is_gameplay_ready(sp) || !sp->connection->conn) continue;
        int elen = serialize_events_for_recipient(ebuf, events, p);
        if (elen > 2)
            ws_send(sp->connection->conn, ebuf, (size_t)elen);
    }
}

/* Run as many fixed-step sim ticks as `sim_accum` covers, up to
 * MAX_SIM_STEPS, broadcasting per-event side effects after each tick.
 * Caller passes the running accumulator + the elapsed-since-last-call
 * seconds. Returns true if it emitted a full player-state flush. */
static bool run_sim_ticks(float *sim_accum, float elapsed, uint64_t now) {
    (void)now;
    uint16_t input_ack_before[MAX_PLAYERS];
    server_pending_input_ack_t pending_input_acks[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        input_ack_before[i] = world.players[i].last_input_seq;
        server_pending_input_ack_reset(&pending_input_acks[i]);
    }

    *sim_accum += elapsed;
    int steps = 0;
    while (*sim_accum >= SIM_DT && steps < MAX_SIM_STEPS) {
        world_sim_step(&world, SIM_DT);
        mark_station_identity_dirty_for_hull_inventory_changes();
        for (int p = 0; p < MAX_PLAYERS; p++) {
            const server_player_t *sp = &world.players[p];
            if (!server_player_is_gameplay_ready(sp)) continue;
            if (server_pending_input_ack_note(
                    &pending_input_acks[p], sp, input_ack_before[p],
                    world.tick)) {
                input_ack_before[p] = sp->last_input_seq;
            }
        }
        if (run_sim_tick_has_control_barrier())
            flush_pending_input_acks(pending_input_acks);
        for (int e = 0; e < world.events.count; e++)
            srv_dispatch_sim_event(&world.events.events[e]);
        send_sim_events_to_recipients(&world.events);
        send_pending_action_results(&world.events);
        broadcast_fracture_updates();
        *sim_accum -= SIM_DT;
        steps++;
    }
    flush_pending_input_acks(pending_input_acks);
    if (*sim_accum > SIM_DT) *sim_accum = 0.0f; /* prevent spiral */
    return false;
}

/* Tick down per-player grace timers and session-auth timeouts. */
static void tick_session_timers(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (sp->connected && sp->grace_period) {
            sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
            if (sp->grace_timer <= 0.0f) {
                (void)world_player_release_ship_asset(&world, i);
                sp->connected = false;
                world_character_unbind_player(&world, i);
                sp->grace_period = false;
                server_player_clear_live_session_identity(sp);
                server_player_clear_transient_input(sp);
                uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                broadcast(leave_msg, 2);
                printf("[server] player %d grace expired, fully disconnected\n", i);
            }
        }
        /* Kick clients that never sent SESSION within the auth window. */
        if (sp->connected && !sp->session_ready && !sp->grace_period) {
            sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
            if (sp->grace_timer <= 0.0f) {
                printf("[server] player %d: session timeout, disconnecting\n", i);
                if (sp->connection->conn)
                    mg_ws_send(sp->connection->conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                (void)world_player_release_ship_asset(&world, i);
                sp->connected = false;
                world_character_unbind_player(&world, i);
                sp->connection->conn = NULL;
                server_player_clear_live_session_identity(sp);
                server_player_clear_transient_input(sp);
                uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                broadcast(leave_msg, 2);
            }
        }
    }
}


static bool station_diag_changed(int station_idx) {
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    station_t *st = &world.stations[station_idx];
    if (!station_exists(st)) {
        station_diag_valid[station_idx] = false;
        memset(station_diag_last[station_idx], 0,
               sizeof(station_diag_last[station_idx]));
        return false;
    }

    uint8_t current[MAX_MODULES_PER_STATION] = {0};
    int module_count = st->module_count;
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    for (int m = 0; m < module_count; m++)
        current[m] = (uint8_t)station_module_flow_diag(st, m);

    if (station_diag_valid[station_idx] &&
        memcmp(station_diag_last[station_idx], current,
               sizeof(current)) == 0) {
        return false;
    }

    memcpy(station_diag_last[station_idx], current, sizeof(current));
    station_diag_valid[station_idx] = true;
    return true;
}

static void broadcast_dirty_station_diag(uint64_t now) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&world.stations[s])) {
            station_diag_valid[s] = false;
            station_diag_last_sent_ms[s] = 0;
            continue;
        }
        uint64_t last_diag = station_diag_last_sent_ms[s];
        if (last_diag != 0 && now - last_diag < STATION_DIAG_MIN_MS)
            continue;
        if (!station_diag_changed(s)) continue;
        uint8_t diag_buf[STATION_DIAG_SIZE];
        int diag_len = serialize_station_diag(diag_buf, s, &world.stations[s]);
        broadcast(diag_buf, (size_t)diag_len);
        station_diag_last_sent_ms[s] = now;
    }
}

/* WORLD_TICK_MS broadcast: dirty station identities + named-ingot
 * stockpiles + manifest summaries. */
static void broadcast_dirty_station_data(uint64_t now, uint64_t *last_station_identity_p) {
    if (now - *last_station_identity_p >= STATION_IDENTITY_FALLBACK_MS) {
        for (int s = 0; s < MAX_STATIONS; s++) station_identity_dirty[s] = true;
        *last_station_identity_p = now;
    }
    broadcast_dirty_station_diag(now);
    /* Re-broadcast dirty station identities only to players in signal range. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_identity_dirty[s]) continue;
        if (!station_exists(&world.stations[s])) continue;
        uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
        int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
        float sr_sq = world.stations[s].signal_range * world.stations[s].signal_range;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!server_player_is_gameplay_ready(&world.players[p]) ||
                !world.players[p].connection->conn) continue;
            if (v2_dist_sq(world.players[p].ship->pos, world.stations[s].pos) <= sr_sq) {
                ws_send_station_identity_if_changed(
                    world.players[p].connection->conn,
                    &world.players[p],
                    id_buf,
                    (size_t)id_len);
            }
        }
        station_identity_dirty[s] = false;
    }
    /* RATi v2: per-station named-ingot snapshot (derived from the
     * unified manifest) + per-(commodity, grade) manifest summary.
     * Smaller payload than identity (~3KB worst case) so we send to
     * everyone regardless of signal range — MARKET is global. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!world.stations[s].manifest_dirty) continue;
        if (!station_exists(&world.stations[s])) continue;
        uint8_t buf[STATION_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
        int len = serialize_station_ingots(buf, s, &world.stations[s]);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!server_player_is_gameplay_ready(&world.players[p]) ||
                !world.players[p].connection->conn) continue;
            ws_send(world.players[p].connection->conn, buf, (size_t)len);
        }
        uint8_t mbuf[STATION_MANIFEST_HEADER +
                     COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY];
        int mlen = serialize_station_manifest(mbuf, s, &world.stations[s]);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!server_player_is_gameplay_ready(&world.players[p]) ||
                !world.players[p].connection->conn) continue;
            ws_send(world.players[p].connection->conn, mbuf, (size_t)mlen);
        }
        world.stations[s].manifest_dirty = false;
    }
}

static uint64_t min_due_ms(uint64_t current, uint64_t last, uint64_t interval) {
    uint64_t due = last + interval;
    return due < current ? due : current;
}

static int server_poll_timeout_ms(uint64_t now,
                                  uint64_t last_sim,
                                  uint64_t last_state,
                                  uint64_t last_world,
                                  uint64_t last_ship,
                                  uint64_t last_analytics,
                                  uint64_t last_save) {
    if (highscores_dirty) return 0;
    uint64_t next = last_sim + SIM_TICK_MS;
    next = min_due_ms(next, last_state, STATE_TICK_MS);
    next = min_due_ms(next, last_world, WORLD_TICK_MS);
    next = min_due_ms(next, last_ship, SHIP_TICK_MS);
    next = min_due_ms(next, last_analytics, ANALYTICS_EMF_INTERVAL_MS);
    next = min_due_ms(next, last_save, AUTOSAVE_MS);
    if (next <= now) return 0;
    uint64_t wait = next - now;
    if (wait > SIM_TICK_MS) wait = SIM_TICK_MS;
    return (int)wait;
}

static bool save_active_players(void) {
    bool ok = true;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (!sp->session_ready) continue;
        if (!player_save(sp, PLAYER_SAVE_DIR, i)) {
            fprintf(stderr, "[save] player %d autosave failed: %s\n",
                    i, strerror(errno));
            ok = false;
        }
    }
    return ok;
}

static bool save_persistent_state(void) {
    bool catalogs_ok = station_catalog_save_all(
        world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
    bool world_ok = world_save(&world, SAVE_PATH);
    bool players_ok = save_active_players();
    if (!catalogs_ok || !world_ok || !players_ok) {
        fprintf(stderr,
                "[save] persistence snapshot incomplete "
                "(catalogs=%s world=%s players=%s)\n",
                catalogs_ok ? "ok" : "failed",
                world_ok ? "ok" : "failed",
                players_ok ? "ok" : "failed");
    }
    return catalogs_ok && world_ok && players_ok;
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Line-buffer stdout and unbuffer stderr so `docker compose logs`
     * sees server output in real time. Without this, fully-buffered
     * stdout holds [server] printf lines until a 4KB page fills. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  /* WebSocket disconnect during write */

    char listen_url[64];
    if (!read_env_config(listen_url, sizeof(listen_url))) return 1;

    chain_log_set_disk_enabled(true);
    signal_chain_set_disk_enabled(true);
    if (!enter_persistence_data_dir()) return 1;
    ensure_persistence_dirs();
    if (!load_world_state()) return 1;
    server_apply_npc_worker_trace_fixture();
    frontier_virtual_pilots_set(&world, frontier_virtual_pilot_target);
    signal_intelligence_holographic_init();
    if (server_bot_brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT) {
        if (server_bot_brain_checkpoint &&
            server_bot_brain_checkpoint[0] != '\0') {
            char err[256];
            if (!signal_intelligence_load_flight_checkpoint(
                    server_bot_brain_checkpoint, err, sizeof(err))) {
                fprintf(stderr, "[FATAL] failed to load SIGNAL_BOT_BRAIN_CHECKPOINT=%s: %s\n",
                        server_bot_brain_checkpoint, err);
                return 1;
            }
            printf("[server] loaded neural bot brain checkpoint: %s\n",
                   server_bot_brain_checkpoint);
        } else if (signal_intelligence_flight_builtin_available()) {
            printf("[server] using built-in CRLPLRIMES flight brain: "
                   "%s encoder=%u hash=%.12s\n",
                   signal_intelligence_flight_feature_set(),
                   (unsigned)signal_intelligence_flight_feature_encoder_version(),
                   signal_intelligence_flight_checkpoint_hash());
        } else {
            fprintf(stderr, "[FATAL] no neural flight backend available\n");
            return 1;
        }
        if (server_bot_contract_brain_checkpoint &&
            server_bot_contract_brain_checkpoint[0] != '\0') {
            char contract_err[256];
            if (!signal_intelligence_load_contract_checkpoint(
                    server_bot_contract_brain_checkpoint,
                    contract_err,
                    sizeof(contract_err))) {
                fprintf(stderr, "[FATAL] failed to load "
                                "SIGNAL_BOT_CONTRACT_BRAIN_CHECKPOINT=%s: %s\n",
                        server_bot_contract_brain_checkpoint, contract_err);
                return 1;
            }
            printf("[server] loaded neural contract brain checkpoint: %s\n",
                   server_bot_contract_brain_checkpoint);
        }
    }
    if (server_bot_npc_worker_brain_checkpoint &&
        server_bot_npc_worker_brain_checkpoint[0] != '\0') {
        char worker_err[256];
        if (!signal_intelligence_load_npc_worker_checkpoint(
                server_bot_npc_worker_brain_checkpoint,
                worker_err,
                sizeof(worker_err))) {
            fprintf(stderr, "[FATAL] failed to load "
                            "SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT=%s: %s\n",
                    server_bot_npc_worker_brain_checkpoint, worker_err);
            return 1;
        }
        printf("[server] loaded NPC worker brain checkpoint: %s\n",
               server_bot_npc_worker_brain_checkpoint);
    }

    /* ── aws-swarm avatar keypair import ────────────────────────────
     * When SIGNAL_AVATAR_KEYPAIR_B64 is set, decode the avatar's Ed25519
     * keypair (base64-encoded 32-byte seed) and print the station
     * founding position derived from its pubkey.
     *
     * NOTE: the decoded keypair is intentionally NOT stored. Commit
     * 18f563b announced wiring it into outpost founding (station signing
     * identity), but that consumption never landed — the globals it
     * added were write-only and have been removed. Outpost identity
     * derivation goes through station_authority.h as usual. If the
     * avatar-identity wiring is revived, thread the keypair through
     * world_t or an explicit operator context — not file-scope globals.
     */
    {
        const char *avatar_keypair_b64 = getenv("SIGNAL_AVATAR_KEYPAIR_B64");
        if (avatar_keypair_b64 && avatar_keypair_b64[0]) {
            /* Decode base64 → 32-byte seed */
            uint8_t seed[32];
            int seed_len = base64_decode(avatar_keypair_b64, seed, 32);
            if (seed_len == 32) {
                uint8_t nacl_secret[64];
                signal_crypto_keypair_from_seed(seed, nacl_secret + 32, nacl_secret);
                printf("[server] imported avatar keypair from SIGNAL_AVATAR_KEYPAIR_B64\n");
                
                /* Derive station position from pubkey */
                sha256_ctx_t pctx;
                uint8_t phash[32];
                sha256_init(&pctx);
                sha256_update(&pctx, nacl_secret + 32, 32);  /* pubkey */
                sha256_update(&pctx, (const uint8_t*)"station", 7);
                sha256_final(&pctx, phash);
                
                int32_t px = (int32_t)((phash[0] << 24 | phash[1] << 16 | phash[2] << 8 | phash[3]) % 20000 - 10000);
                int32_t py = (int32_t)((phash[4] << 24 | phash[5] << 16 | phash[6] << 8 | phash[7]) % 20000 - 10000);
                printf("[server] avatar station position: (%d, %d)\n", px, py);
                printf("[server] tow a SIGNAL RELAY scaffold here and press E to found your station\n");
            } else {
                fprintf(stderr, "[server] WARNING: SIGNAL_AVATAR_KEYPAIR_B64 decode failed (got %d bytes, expected 32)\n", seed_len);
            }
        }
    }

    spawn_server_bots();

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    struct mg_connection *listener =
        mg_http_listen(&mgr, listen_url, ev_handler, NULL);
    if (!listener) {
        fprintf(stderr, "[FATAL] failed to listen on %s\n", listen_url);
        mg_mgr_free(&mgr);
        return 1;
    }
#ifdef GIT_HASH
    printf("[server] SIGNAL alpha %s on %s\n", GIT_HASH, listen_url);
#else
    printf("[server] SIGNAL alpha on %s\n", listen_url);
#endif
    printf("[server] ALPHA BUILD -- world may reset without notice\n");

    uint64_t start_ms = mg_millis();
    uint64_t last_sim = start_ms, last_state = start_ms, last_world = start_ms;
    uint64_t last_ship = start_ms, last_save = start_ms;
    uint64_t last_analytics = start_ms;
    uint64_t last_econ_dirty = start_ms;
    last_station_identity = start_ms;
    float sim_accum = 0.0f;

    while (running) {
        uint64_t poll_now = mg_millis();
        mg_mgr_poll(&mgr, server_poll_timeout_ms(
            poll_now, last_sim, last_state, last_world, last_ship,
            last_analytics, last_save));
        uint64_t now = mg_millis();

        if (now - last_sim >= SIM_TICK_MS) {
            float elapsed = (float)(now - last_sim) / 1000.0f;
            last_sim = now;
            if (run_sim_ticks(&sim_accum, elapsed, now))
                last_state = now;
            tick_session_timers();
            /* Mark econ dirty every ~1s as fallback for production changes. */
            if (now - last_econ_dirty >= 1000) {
                station_econ_dirty = true;
                contracts_dirty = true;
                last_econ_dirty = now;
            }
        }
        if (now - last_state >= STATE_TICK_MS) {
            broadcast_player_states(now);
            last_state = now;
            last_player_state_emit = now;
        }
        if (now - last_world >= WORLD_TICK_MS) {
            broadcast_world();
            broadcast_dirty_station_data(now, &last_station_identity);
            last_world = now;
        }
        if (now - last_ship >= SHIP_TICK_MS) {
            broadcast_ship_states();
            last_ship = now;
        }
        if (highscores_dirty) {
            broadcast_highscores();
            highscores_dirty = false;
        }
        if (now - last_analytics >= ANALYTICS_EMF_INTERVAL_MS) {
            analytics_emit_emf(now);
            last_analytics = now;
        }
        if (now - last_save >= AUTOSAVE_MS) {
            (void)save_persistent_state();
            last_save = now;
        }
    }

    mg_mgr_free(&mgr);
    if (save_persistent_state()) {
        printf("[server] world saved\n");
    } else {
        fprintf(stderr, "[server] shutdown persistence failed\n");
        return 1;
    }
    printf("[server] shutdown\n");
    return 0;
}
