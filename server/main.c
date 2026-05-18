/*
 * main.c -- Headless authoritative game server for Signal Space Miner.
 *
 * Uses cesanta/mongoose for WebSocket handling.  Runs the full game
 * simulation and broadcasts state to browser clients.
 */
#include "mongoose.h"
#include "game_sim.h"
#include "highscore.h"
#include "manifest.h"
#include "mining.h"  /* mining_render_callsign for chain log copy */
#include "net_protocol.h"
#include "pubkey_proof.h"
#include "signal_crypto.h"
#include "sim_ai.h"
#include "sim_asteroid.h"
#include "chain_log.h"  /* signed event emission (#479 C) */
#include "cargo_receipt_issue.h"  /* portable cargo receipts (#479 D) */
#include "commodity.h"  /* station_*_price_unit (#prefix-pricing) */
#include "sha256.h"
#include "station_authority.h"
#include <math.h>       /* lroundf */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
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

typedef enum {
    PERSISTENCE_LOCAL = 0,
    PERSISTENCE_EPHEMERAL,
    PERSISTENCE_EXTERNAL_S3,
} persistence_mode_t;

static persistence_mode_t persistence_mode = PERSISTENCE_LOCAL;
static const char *persistence_data_dir = ".";
static const char *persistence_state_uri = "";
static uint64_t idle_shutdown_after_ms = 0;
static uint64_t idle_shutdown_empty_since_ms = 0;
static bool idle_shutdown_armed = false;

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

static const char *persistence_mode_name(void) {
    switch (persistence_mode) {
    case PERSISTENCE_LOCAL: return "local";
    case PERSISTENCE_EPHEMERAL: return "ephemeral";
    case PERSISTENCE_EXTERNAL_S3: return "external_s3";
    default: return "unknown";
    }
}

static bool persistence_load_enabled(void) {
    return persistence_mode == PERSISTENCE_LOCAL ||
           persistence_mode == PERSISTENCE_EXTERNAL_S3;
}

static bool persistence_save_enabled(void) {
    return persistence_mode == PERSISTENCE_LOCAL ||
           persistence_mode == PERSISTENCE_EXTERNAL_S3;
}

static bool persistence_externalized(void) {
    return persistence_mode == PERSISTENCE_EXTERNAL_S3;
}

static int live_player_connection_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].connected && world.players[i].conn) count++;
    }
    return count;
}

/* Dirty flags: only re-broadcast station identity when something changed */
static bool station_identity_dirty[MAX_STATIONS];
static bool station_diag_valid[MAX_STATIONS];
static uint64_t station_diag_last_sent_ms[MAX_STATIONS];
static uint8_t station_diag_last[MAX_STATIONS][MAX_MODULES_PER_STATION];
static bool station_econ_dirty = true;   /* station inventories changed */
static bool contracts_dirty = true;       /* contract list changed */
static highscore_table_t highscores;
static bool highscores_dirty = true;      /* broadcast + persist pending */

/* Defined further down; forward-declared so the highscore helpers can
 * use the same send wrapper as every other broadcast in this file
 * (consistent with future send-queue / rate-limiting changes). */
static void ws_send(struct mg_connection *c, const void *data, size_t len);
static float player_station_balance(const server_player_t *sp);

static void send_cargo_receipt_chain(struct mg_connection *c,
                                     const cargo_receipt_chain_t *chain) {
    if (!c || !chain || chain->len == 0 ||
        chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
        return;
    }
    uint8_t buf[3 + CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE];
    buf[0] = NET_MSG_CARGO_RECEIPT_BUNDLE;
    buf[1] = chain->len;
    buf[2] = 0;
    for (uint8_t i = 0; i < chain->len; i++)
        cargo_receipt_pack(&chain->links[i], &buf[3 + i * CARGO_RECEIPT_SIZE]);
    ws_send(c, buf, 3u + (size_t)chain->len * CARGO_RECEIPT_SIZE);
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

static bool tick_after_u32(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static uint32_t server_input_apply_tick(uint32_t client_tick) {
    const uint32_t max_future_ticks = 12; /* 100ms at 120Hz */
    uint32_t next_tick = world.tick + 1u;
    if (client_tick == 0 || !tick_after_u32(client_tick, world.tick))
        return next_tick;
    if (tick_after_u32(client_tick, world.tick + max_future_ticks))
        return world.tick + max_future_ticks;
    return client_tick;
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
    if (sp) sp->analytics_last_activity_ms = now_ms;
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
    sp->analytics_metrics_seq = read_u32_le(&data[1]);
    sp->analytics_ping_ms = read_u16_le(&data[5]);
    sp->analytics_ack_ms = read_u16_le(&data[7]);
    sp->analytics_ack_gap_ms = read_u16_le(&data[9]);
    sp->analytics_server_turnaround_ms = read_u16_le(&data[11]);
    sp->analytics_player_interval_ms = read_u16_le(&data[13]);
    sp->analytics_unacked_inputs = read_u16_le(&data[15]);
    sp->analytics_replay_depth = read_u16_le(&data[17]);
    sp->analytics_action_queue_depth = data[19];
    sp->analytics_metrics_last_ms = now_ms;
    sp->analytics_metrics_samples++;
    analytics_record_activity(sp, now_ms);

    char user_key[ANALYTICS_USER_KEY_LEN];
    analytics_user_key(sp, user_key);
    printf("{\"event\":\"player_metrics\",\"service\":\"signal-relay\","
           "\"build\":\"%s\",\"ts_epoch_ms\":%llu,\"uptime_ms\":%llu,"
           "\"user_key\":\"%s\",\"player_slot\":%d,\"session_ready\":%s,"
           "\"seq\":%u,\"ping_ms\":%u,\"ack_ms\":%u,\"ack_gap_ms\":%u,"
           "\"server_turnaround_ms\":%u,\"player_interval_ms\":%u,"
           "\"unacked_inputs\":%u,\"replay_depth\":%u,"
           "\"action_queue_depth\":%u,\"sample_count\":%u}\n",
           analytics_build_hash(),
           (unsigned long long)analytics_epoch_ms(),
           (unsigned long long)now_ms,
           user_key,
           pid,
           sp->session_ready ? "true" : "false",
           (unsigned)sp->analytics_metrics_seq,
           (unsigned)sp->analytics_ping_ms,
           (unsigned)sp->analytics_ack_ms,
           (unsigned)sp->analytics_ack_gap_ms,
           (unsigned)sp->analytics_server_turnaround_ms,
           (unsigned)sp->analytics_player_interval_ms,
           (unsigned)sp->analytics_unacked_inputs,
           (unsigned)sp->analytics_replay_depth,
           (unsigned)sp->analytics_action_queue_depth,
           (unsigned)sp->analytics_metrics_samples);
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
        if (!sp->connected || !sp->conn) continue;
        connected++;
        if (sp->session_ready) ready++;
        if (sp->analytics_last_activity_ms != 0 &&
            now_ms >= sp->analytics_last_activity_ms &&
            now_ms - sp->analytics_last_activity_ms <= ANALYTICS_ACTIVE_WINDOW_MS) {
            active_1m++;
        }
        if (sp->analytics_metrics_samples == 0 ||
            sp->analytics_metrics_last_ms == 0 ||
            now_ms < sp->analytics_metrics_last_ms ||
            now_ms - sp->analytics_metrics_last_ms > ANALYTICS_METRIC_STALE_MS) {
            continue;
        }
        metric_players++;
        ping_sum += sp->analytics_ping_ms;
        ack_sum += sp->analytics_ack_ms;
        gap_sum += sp->analytics_ack_gap_ms;
        if (sp->analytics_ack_gap_ms > max_gap) max_gap = sp->analytics_ack_gap_ms;
    }

    double avg_ping = metric_players ? (double)ping_sum / (double)metric_players : 0.0;
    double avg_ack = metric_players ? (double)ack_sum / (double)metric_players : 0.0;
    double avg_gap = metric_players ? (double)gap_sum / (double)metric_players : 0.0;

    printf("{\"_aws\":{\"Timestamp\":%llu,\"CloudWatchMetrics\":[{\"Namespace\":\"Signal\","
           "\"Dimensions\":[[\"Service\",\"Build\"]],\"Metrics\":["
           "{\"Name\":\"ConnectedPlayers\",\"Unit\":\"Count\"},"
           "{\"Name\":\"ReadyPlayers\",\"Unit\":\"Count\"},"
           "{\"Name\":\"ActiveUsers1m\",\"Unit\":\"Count\"},"
           "{\"Name\":\"MetricPlayers\",\"Unit\":\"Count\"},"
           "{\"Name\":\"AvgPingMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"AvgAckMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"AvgAckGapMs\",\"Unit\":\"Milliseconds\"},"
           "{\"Name\":\"MaxAckGapMs\",\"Unit\":\"Milliseconds\"}]}]},"
           "\"Service\":\"signal-relay\",\"Build\":\"%s\","
           "\"ConnectedPlayers\":%d,\"ReadyPlayers\":%d,"
           "\"ActiveUsers1m\":%d,\"MetricPlayers\":%d,"
           "\"AvgPingMs\":%.2f,\"AvgAckMs\":%.2f,"
           "\"AvgAckGapMs\":%.2f,\"MaxAckGapMs\":%u}\n",
           (unsigned long long)analytics_epoch_ms(),
           analytics_build_hash(),
           connected,
           ready,
           active_1m,
           metric_players,
           avg_ping,
           avg_ack,
           avg_gap,
           (unsigned)max_gap);
}

static void merge_one_shot_input(input_intent_t *dst,
                                 const input_intent_t *src) {
    if (!dst || !src) return;
    if (src->dock) {
        dst->dock = true;
        dst->interact = true;
    }
    if (src->launch) {
        dst->launch = true;
        dst->interact = true;
    }
    if (src->interact) dst->interact = true;
    if (src->service_sell) {
        dst->service_sell = true;
        dst->service_sell_only = src->service_sell_only;
        dst->service_sell_grade = src->service_sell_grade;
        dst->service_sell_one = src->service_sell_one;
    }
    if (src->service_repair) dst->service_repair = true;
    if (src->upgrade_mining) dst->upgrade_mining = true;
    if (src->upgrade_hold) dst->upgrade_hold = true;
    if (src->upgrade_tractor) dst->upgrade_tractor = true;
    if (src->place_outpost) {
        dst->place_outpost = true;
        dst->place_target_station = src->place_target_station;
        dst->place_target_ring = src->place_target_ring;
        dst->place_target_slot = src->place_target_slot;
    }
    if (src->buy_scaffold_kit) {
        dst->buy_scaffold_kit = true;
        dst->scaffold_kit_module = src->scaffold_kit_module;
    }
    if (src->buy_product) {
        dst->buy_product = true;
        dst->buy_commodity = src->buy_commodity;
        dst->buy_grade = src->buy_grade;
    }
    if (src->hail) dst->hail = true;
    if (src->release_tow) dst->release_tow = true;
    if (src->reset) dst->reset = true;
    if (src->toggle_autopilot) dst->toggle_autopilot = true;
}

static void broadcast_highscores(void) {
    uint8_t buf[HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE];
    int len = highscore_serialize(buf, &highscores);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!world.players[p].connected || !world.players[p].conn) continue;
        ws_send(world.players[p].conn, buf, (size_t)len);
    }
}

static void send_highscores_to(struct mg_connection *c) {
    if (!c) return;
    uint8_t buf[HIGHSCORE_HEADER + HIGHSCORE_TOP_N * HIGHSCORE_ENTRY_SIZE];
    int len = highscore_serialize(buf, &highscores);
    ws_send(c, buf, (size_t)len);
}

#define STATION_IDENTITY_FALLBACK_MS 2000
#define STATION_DIAG_MIN_MS 300
static uint64_t last_station_identity = 0;

/* Timing intervals in milliseconds */
#define SIM_TICK_MS   8     /* ~120 Hz poll gate; sim uses SIM_DT accumulator */
#define STATE_TICK_MS 50    /* 20 Hz player state broadcast */
#define WORLD_TICK_MS 100   /* 10 Hz world state broadcast */
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

/* ------------------------------------------------------------------ */
/* WebSocket send helpers                                             */
/* ------------------------------------------------------------------ */

static void ws_send(struct mg_connection *c, const void *data, size_t len) {
    mg_ws_send(c, data, len, WEBSOCKET_OP_BINARY);
}

static uint64_t wire_payload_hash(const uint8_t *data, size_t len) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 1099511628211ull;
    }
    return h;
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
    uint64_t hash = wire_payload_hash(data, len);
    if (cache->valid &&
        cache->conn == c &&
        cache->len == (uint16_t)len &&
        cache->hash == hash) {
        return;
    }
    ws_send(c, data, len);
    cache->valid = true;
    cache->conn = c;
    cache->len = (uint16_t)len;
    cache->hash = hash;
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
                                     server_recv_ms, server_send_ms);
    ws_send(c, buf, (size_t)len);
}

static int action_result_station_index(const server_player_t *sp) {
    return sp->docked ? sp->current_station : sp->nearby_station;
}

static int action_result_station_pending_count(const server_player_t *sp) {
    int st = sp->pending_action_before_station;
    if (st < 0 || st >= MAX_STATIONS) return -1;
    (void)sp;
    return world.stations[st].pending_scaffold_count;
}

static void begin_pending_action_result(server_player_t *sp,
                                        uint16_t action_id,
                                        uint16_t input_seq,
                                        uint8_t action) {
    if (!sp || action_id == 0 || action == NET_ACTION_NONE) return;
    int st = action_result_station_index(sp);
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
        (st >= 0 && st < MAX_STATIONS) ? world.stations[st].pending_scaffold_count : -1;
    sp->pending_action_before_station_balance = player_station_balance(sp);
}

static bool pending_action_state_changed(const server_player_t *sp) {
    if (!sp || !sp->pending_action_result_valid) return false;
    if (sp->pending_action_before_docked != sp->docked) return true;
    if (sp->pending_action_before_docking_approach != sp->docking_approach) return true;
    if (sp->pending_action_before_station != action_result_station_index(sp)) return true;
    if (sp->pending_action_before_autopilot_mode != sp->autopilot_mode) return true;
    if (fabsf(sp->pending_action_before_hull - sp->ship.hull) > 0.01f) return true;
    if (fabsf(sp->pending_action_before_cargo_total - ship_total_cargo(&sp->ship)) > 0.01f) return true;
    if (sp->pending_action_before_manifest_count != sp->ship.manifest.count) return true;
    if (sp->pending_action_before_mining_level != (uint8_t)sp->ship.mining_level) return true;
    if (sp->pending_action_before_hold_level != (uint8_t)sp->ship.hold_level) return true;
    if (sp->pending_action_before_tractor_level != (uint8_t)sp->ship.tractor_level) return true;
    if (sp->pending_action_before_towed_count != sp->ship.towed_count) return true;
    if (sp->pending_action_before_towed_scaffold != sp->ship.towed_scaffold) return true;
    if (sp->pending_action_before_station_pending_scaffold_count !=
        action_result_station_pending_count(sp)) {
        return true;
    }
    if (fabsf(sp->pending_action_before_station_balance -
              player_station_balance(sp)) > 0.01f) {
        return true;
    }
    return false;
}

static bool action_matches_event(uint8_t action, uint8_t event_type) {
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
    if (action == NET_ACTION_PLACE_OUTPOST) return event_type == SIM_EVENT_OUTPOST_PLACED;
    if (action == NET_ACTION_HAIL) return event_type == SIM_EVENT_HAIL_RESPONSE;
    if (action == NET_ACTION_RESET) return event_type == SIM_EVENT_DEATH;
    if (action >= NET_ACTION_BUY_PRODUCT &&
        action < NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT) {
        return event_type == SIM_EVENT_SELL;
    }
    return false;
}

static uint8_t pending_action_result_status(const server_player_t *sp,
                                            const sim_events_t *events) {
    bool matched_event = false;
    bool rejected = false;
    if (sp && events) {
        for (int i = 0; i < events->count; i++) {
            const sim_event_t *ev = &events->events[i];
            if (ev->player_id != sp->id) continue;
            if (ev->type == SIM_EVENT_ORDER_REJECTED) rejected = true;
            if (action_matches_event(sp->pending_action_result_action,
                                     (uint8_t)ev->type)) {
                matched_event = true;
            }
        }
    }
    if (rejected) return NET_ACTION_RESULT_REJECTED;
    if (matched_event || pending_action_state_changed(sp)) return NET_ACTION_RESULT_OK;
    return NET_ACTION_RESULT_NOOP;
}

static void send_pending_action_results(const sim_events_t *events) {
    uint32_t server_tick = world.tick;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (!sp->pending_action_result_valid) continue;
        uint8_t status = pending_action_result_status(sp, events);
        if (sp->connected && sp->conn) {
            send_action_result(sp->conn,
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
        if (world.players[i].connected && world.players[i].session_ready && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static void broadcast_except(int exclude, const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude) continue;
        if (world.players[i].connected && world.players[i].session_ready && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static float fracture_signal_radius(vec2 pos) {
    float radius = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &world.stations[s];
        if (!station_provides_signal(st)) continue;
        if (v2_dist_sq(pos, st->pos) <= st->signal_range * st->signal_range &&
            st->signal_range > radius)
            radius = st->signal_range;
    }
    return radius;
}

static bool fracture_player_in_range(int player_id, int asteroid_idx) {
    float radius;
    if (player_id < 0 || player_id >= MAX_PLAYERS ||
        asteroid_idx < 0 || asteroid_idx >= MAX_ASTEROIDS)
        return false;
    if (!world.players[player_id].connected ||
        !world.players[player_id].session_ready ||
        !world.players[player_id].conn ||
        !world.asteroids[asteroid_idx].active)
        return false;
    radius = fracture_signal_radius(world.asteroids[asteroid_idx].pos);
    if (radius <= 0.0f) return false;
    return v2_dist_sq(world.players[player_id].ship.pos, world.asteroids[asteroid_idx].pos) <= radius * radius;
}

static void broadcast_fracture_updates(void) {
    uint32_t now_ms = (uint32_t)(world.time * 1000.0f);

    /* Challenges: re-broadcast to each in-range player while the window
     * is open (challenge_dirty is re-armed in step_fracture_claims).
     * Clients dedupe by fracture_id so duplicate challenges are cheap. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        fracture_claim_state_t *state = &world.fracture_claims[i];
        if (state->challenge_dirty && state->fracture_id && world.asteroids[i].active) {
            uint8_t buf[FRACTURE_CHALLENGE_SIZE];
            buf[0] = NET_MSG_FRACTURE_CHALLENGE;
            write_u32_le(&buf[1], state->fracture_id);
            memcpy(&buf[5], world.asteroids[i].fracture_seed, 32);
            write_u32_le(&buf[37], state->deadline_ms);
            write_u16_le(&buf[41], state->burst_cap);
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!fracture_player_in_range(p, i)) continue;
                ws_send(world.players[p].conn, buf, sizeof(buf));
            }
            state->challenge_dirty = false;
        }
        /* The legacy per-state resolved_dirty path is preserved for the
         * common "asteroid still alive at resolve time" case — cheap
         * and range-filtered. The pending_resolves queue below covers
         * the gnarly case (resolve + smelt in same tick). */
        if (state->resolved_dirty && state->fracture_id && world.asteroids[i].active) {
            uint8_t buf[FRACTURE_RESOLVED_SIZE];
            buf[0] = NET_MSG_FRACTURE_RESOLVED;
            write_u32_le(&buf[1], state->fracture_id);
            memcpy(&buf[5], world.asteroids[i].fragment_pub, 32);
            memcpy(&buf[37], state->best_player_pub, 32);
            buf[69] = state->best_grade;
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!fracture_player_in_range(p, i)) continue;
                ws_send(world.players[p].conn, buf, sizeof(buf));
            }
            state->resolved_dirty = false;
        }
    }

    /* Pending resolves: fracture_commit_resolution pushes here so
     * deliveries survive asteroid clear. Broadcast to every connected
     * player rather than range-filtering — the asteroid may be gone
     * so we can't compute range, and clients that never got the
     * matching challenge drop the resolve in mining_client_resolve_fracture. */
    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        pending_resolve_t *pr = &world.pending_resolves[p];
        if (!pr->active) continue;
        if (pr->tx_count > 0 && now_ms < pr->last_tx_ms + FRACTURE_RESOLVE_RETRY_PERIOD_MS)
            continue;
        uint8_t buf[FRACTURE_RESOLVED_SIZE];
        buf[0] = NET_MSG_FRACTURE_RESOLVED;
        write_u32_le(&buf[1], pr->fracture_id);
        memcpy(&buf[5], pr->fragment_pub, 32);
        memcpy(&buf[37], pr->winner_pub, 32);
        buf[69] = pr->grade;
        for (int pi = 0; pi < MAX_PLAYERS; pi++) {
            if (!world.players[pi].connected || !world.players[pi].conn) continue;
            ws_send(world.players[pi].conn, buf, sizeof(buf));
        }
        pr->tx_count++;
        pr->last_tx_ms = now_ms;
        if (pr->tx_count >= FRACTURE_RESOLVE_RETRY_COUNT) pr->active = false;
    }
}

/* ------------------------------------------------------------------ */
/* WS message handler                                                 */
/* ------------------------------------------------------------------ */

/* Per-player WebSocket message rate limiting */
static struct { uint64_t window_start; int msg_count; } ws_rate[MAX_PLAYERS];
#define WS_RATE_WINDOW_MS 1000
#define WS_RATE_LIMIT 140 /* 60Hz input + signed/plan bursts without drops */

static void apply_signed_input_action(int pid, const uint8_t *payload,
                                      uint16_t payload_len) {
    if (pid < 0 || pid >= MAX_PLAYERS || !payload || payload_len < 5) return;
    server_player_t *sp = &world.players[pid];
    uint8_t buf[8] = {
        NET_MSG_INPUT,
        0,
        payload[0],
        0xFF,
        payload[1],
        payload[2],
        payload[3],
        payload[4],
    };
    input_intent_t parsed = {0};
    parsed.mining_target_hint = -1;
    parsed.buy_grade = MINING_GRADE_COUNT;
    parsed.service_sell_only = COMMODITY_COUNT;
    parsed.service_sell_grade = MINING_GRADE_COUNT;
    parse_input(buf, (int)sizeof(buf), &parsed);
    merge_one_shot_input(&sp->input, &parsed);

    if ((payload[0] >= NET_ACTION_BUY_SCAFFOLD_TYPED &&
         payload[0] < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) ||
        payload[0] == NET_ACTION_BUY_SCAFFOLD) {
        int s = sp->current_station;
        if (s >= 0 && s < MAX_STATIONS) station_identity_dirty[s] = true;
    }
}

static void apply_signed_plan(int pid, const uint8_t *payload,
                              uint16_t payload_len) {
    if (pid < 0 || pid >= MAX_PLAYERS || !payload) return;
    if (payload_len != NET_PLAN_MSG_SIZE - 1) return;
    uint8_t buf[NET_PLAN_MSG_SIZE];
    buf[0] = NET_MSG_PLAN;
    memcpy(&buf[1], payload, NET_PLAN_MSG_SIZE - 1);
    parse_plan(buf, NET_PLAN_MSG_SIZE, &world.players[pid].input);
}

static void handle_deliver_ingot_index(struct mg_connection *c, int pid,
                                       uint8_t target) {
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!sp->docked) return;
    int sidx = sp->current_station;
    if (sidx < 0 || sidx >= MAX_STATIONS) return;
    station_t *st = &world.stations[sidx];
    ship_t *ship = &sp->ship;
    int hidx = -1;
    int seen = 0;
    for (uint16_t u = 0; u < ship->manifest.count; u++) {
        const cargo_unit_t *cu = &ship->manifest.units[u];
        if ((cargo_kind_t)cu->kind != CARGO_KIND_INGOT) continue;
        if ((ingot_prefix_t)cu->prefix_class == INGOT_PREFIX_ANONYMOUS) continue;
        if (seen == target) { hidx = (int)u; break; }
        seen++;
    }
    if (hidx < 0) return;
    cargo_unit_t copy = ship->manifest.units[hidx];
    cargo_receipt_chain_t attached_chain = {0};
    ship_receipts_t *rcpts = ship_get_receipts(ship);
    if (rcpts && hidx < (int)rcpts->count) {
        const cargo_receipt_chain_t *attached = &rcpts->chains[hidx];
        attached_chain = *attached;
        if (attached->len > 0) {
            cargo_receipt_result_t vr = cargo_receipt_chain_verify(
                attached->links, attached->len, copy.pub);
            if (vr != CARGO_RECEIPT_OK) {
                printf("[server] receipt_chain_invalid: deliver from player %d, reason=%d\n",
                       pid, (int)vr);
                return;
            }
            if (attached->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
                printf("[server] receipt_chain_cap_exceeded: deliver from player %d\n", pid);
                return;
            }
        }
    }
    if (st->manifest.count >= st->manifest.cap) {
        cargo_unit_t evicted = {0};
        if (station_manifest_remove_with_chain(st, 0, &evicted, NULL) &&
            (ingot_prefix_t)evicted.prefix_class != INGOT_PREFIX_ANONYMOUS) {
            char ev_cs[12]; mining_render_callsign(evicted.pub, ev_cs);
            char ev_msg[96];
            snprintf(ev_msg, sizeof(ev_msg), "stockpile full - voided %s", ev_cs);
            signal_channel_post(&world, sidx, ev_msg, "");
        }
    }
    uint8_t prev_hash[32] = {0};
    bool have_prev = false;
    if (attached_chain.len > 0) {
        cargo_receipt_hash(&attached_chain.links[attached_chain.len - 1], prev_hash);
        have_prev = true;
    }
    cargo_receipt_chain_t removed_chain = {0};
    if (!ship_manifest_remove_with_chain(ship, (uint16_t)hidx,
                                         &copy, &removed_chain)) {
        return;
    }

    cargo_receipt_t receipt = {0};
    cargo_receipt_chain_t station_chain = removed_chain;
    uint64_t xfer_id = cargo_receipt_emit_transfer(
        &world, st,
        sp->pubkey,
        st->station_pubkey,
        copy.pub,
        (uint8_t)CARGO_KIND_INGOT,
        have_prev ? prev_hash : st->chain_last_hash,
        &receipt);
    if (xfer_id != 0 && station_chain.len < CARGO_RECEIPT_CHAIN_MAX_LEN)
        station_chain.links[station_chain.len++] = receipt;

    if (!station_manifest_push_with_chain(st, &copy, &station_chain)) {
        (void)ship_manifest_push_with_chain(ship, &copy, &removed_chain);
        return;
    }

    float delivery_f = station_buy_price_unit(st, &copy);
    float floor_f = (float)INGOT_DELIVERY_CREDIT;
    if (delivery_f < floor_f) delivery_f = floor_f;
    int delivery_int = (int)lroundf(delivery_f);
    if (server_player_can_use_pubkey_persistence(sp)) {
        ledger_credit_supply_by_pubkey(st, sp->pubkey, (float)delivery_int);
    } else {
        ledger_credit_supply(st, sp->session_token, (float)delivery_int);
    }
    if (xfer_id != 0) {
        send_cargo_receipt_chain(c, &station_chain);
        chain_payload_trade_t trade = {0};
        trade.transfer_event_id = xfer_id;
        trade.ledger_delta_signed = (int64_t)delivery_int;
        memcpy(trade.ledger_pubkey, sp->pubkey, 32);
        (void)chain_log_emit(&world, st, CHAIN_EVT_TRADE,
                             &trade, (uint16_t)sizeof(trade));
    }
    char cs[12]; mining_render_callsign(copy.pub, cs);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s delivered %s", sp->callsign, cs);
    signal_channel_post(&world, sidx, msg, "");
}

static void finalize_verified_pubkey_identity(struct mg_connection *c, int pid,
                                              uint64_t now) {
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    if (!server_player_can_use_pubkey_persistence(sp)) return;
    if (sp->pubkey_identity_finalized) return;

    const uint8_t *pk = sp->pubkey;
    int existing = registry_lookup_by_pubkey(&world, pk);
    if (existing >= 0 && existing != pid) {
        server_player_t *old = &world.players[existing];
        if (memcmp(old->session_token, sp->session_token, 8) != 0) {
            if (ship_copy(&sp->ship, &old->ship)) {
                sp->current_station = old->current_station;
                sp->nearby_station = old->nearby_station;
                sp->docked = old->docked;
                sp->in_dock_range = old->in_dock_range;

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

                old->connected = false;
                old->grace_period = false;
                old->conn = NULL;
                memset(old->session_token, 0, 8);
                old->session_ready = false;
                old->pubkey_proof_ok = false;
                old->pubkey_identity_finalized = false;
                uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)existing };
                broadcast(leave_old, 2);
                printf("[server] player %d: pubkey reconnect (was slot %d)\n",
                       pid, existing);
            }
        }
    }

    (void)registry_register_pubkey(&world, pk, sp->session_token);
    sp->pubkey_identity_finalized = true;
    printf("[server] player %d: verified pubkey %02x%02x%02x%02x...\n",
           pid, pk[0], pk[1], pk[2], pk[3]);
    analytics_log_player_event("player_identity", pid, sp, now, 0);

    if (persistence_load_enabled() &&
        player_load_by_pubkey(sp, &world, PLAYER_SAVE_DIR, pk)) {
        printf("[server] player %d: restored save by pubkey\n", pid);
    } else if (persistence_load_enabled()) {
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
    } else {
        printf("[server] player %d: persistence disabled, fresh pubkey session\n",
               pid);
    }
}

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    int pid = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].conn == c) { pid = i; break; }
    }
    if (pid < 0) return;

    /* Rate limit: silently drop excess messages */
    uint64_t now = mg_millis();
    if (now - ws_rate[pid].window_start > WS_RATE_WINDOW_MS) {
        ws_rate[pid].window_start = now;
        ws_rate[pid].msg_count = 0;
    }
    if (++ws_rate[pid].msg_count > WS_RATE_LIMIT) return;

    const uint8_t *data = (const uint8_t *)wm->data.buf;
    int len = (int)wm->data.len;
    if (len < 1 || pid < 0 || pid >= MAX_PLAYERS) return;
    analytics_record_activity(&world.players[pid], now);

    switch (data[0]) {
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
        const uint8_t *input_data = data;
        uint8_t input_copy[32];
        int input_len = len;
        uint8_t action = (len >= 3) ? data[2] : NET_ACTION_NONE;
        uint8_t ack_status = 0;
        uint16_t action_id = 0;
        uint16_t input_seq = (len >= 10)
            ? (uint16_t)data[8] | ((uint16_t)data[9] << 8)
            : 0;
        uint32_t client_tick = input_client_tick(data, len);
        server_player_t *sp = &world.players[pid];
        if (action != NET_ACTION_NONE && sp->pubkey_set) {
            unsigned_action_count++;
            ack_status = NET_ACTION_ACK_REJECTED;
            size_t copy_len = (size_t)len;
            if (copy_len > sizeof(input_copy)) copy_len = sizeof(input_copy);
            if (copy_len >= 3) {
                memcpy(input_copy, data, copy_len);
                input_copy[2] = NET_ACTION_NONE;
                input_data = input_copy;
                input_len = (int)copy_len;
                action = NET_ACTION_NONE;
            }
            if (len >= 14)
                action_id = input_action_id(data, len);
        } else if (len >= 14 && action != NET_ACTION_NONE) {
            action_id = input_action_id(data, len);
            if (action_id != 0 && sp->last_input_action_id_valid &&
                sp->last_input_action_id == action_id) {
                ack_status = NET_ACTION_ACK_DUPLICATE;
                size_t copy_len = (size_t)len;
                if (copy_len > sizeof(input_copy)) copy_len = sizeof(input_copy);
                if (copy_len >= 3) {
                    memcpy(input_copy, data, copy_len);
                    input_copy[2] = NET_ACTION_NONE;
                    input_data = input_copy;
                    input_len = (int)copy_len;
                    action = NET_ACTION_NONE;
                }
            } else if (action_id != 0) {
                sp->last_input_action_id = action_id;
                sp->last_input_action_id_valid = true;
                ack_status = NET_ACTION_ACK_RECEIVED;
                begin_pending_action_result(sp, action_id, input_seq, action);
            }
        }
        if (len >= 4) {
            input_intent_t parsed = {0};
            parsed.mining_target_hint = -1;
            parsed.buy_grade = MINING_GRADE_COUNT;
            parsed.service_sell_only = COMMODITY_COUNT;
            parsed.service_sell_grade = MINING_GRADE_COUNT;
            parse_input(input_data, input_len, &parsed);
            server_player_queue_movement_input(
                sp, &parsed, input_seq, server_input_apply_tick(client_tick));
            merge_one_shot_input(&sp->input, &parsed);
        }
        if (c) {
            if (ack_status != 0) {
                send_action_ack(c, action_id, input_seq, ack_status, data[2]);
            } else if (input_seq != 0) {
                send_action_ack(c, 0, input_seq, NET_ACTION_ACK_RECEIVED,
                                NET_ACTION_NONE);
            }
        }
        /* If the player just queued a shipyard order, refresh that station's
         * identity on the next world tick so the SHIPYARD tab sees the new
         * pending count immediately instead of waiting for the 2s fallback. */
        if (len >= 3) {
            if ((action >= NET_ACTION_BUY_SCAFFOLD_TYPED &&
                 action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) ||
                action == NET_ACTION_BUY_SCAFFOLD) {
                int s = world.players[pid].current_station;
                if (s >= 0 && s < MAX_STATIONS) station_identity_dirty[s] = true;
            }
        }
        break;
    }
    case NET_MSG_PLAN:
        if (world.players[pid].pubkey_set) {
            unsigned_action_count++;
            break;
        }
        parse_plan(data, len, &world.players[pid].input);
        break;
    case NET_MSG_STATE:
        /* Ignored -- server is authoritative. */
        break;
    case NET_MSG_MINING_ACTION:
        /* Legacy -- mining handled via INPUT flags now. */
        break;
    case NET_MSG_BUY_INGOT:
        if (world.players[pid].pubkey_set) {
            unsigned_action_count++;
            break;
        }
        /* RATi v2: purchase a specific named ingot from the docked
         * station's manifest. Payload: [type:1][pubkey:32]. The unit
         * is transferred from station.manifest to ship.manifest with
         * its full provenance (prefix_class, origin_station,
         * mined_block, parent_merkle) preserved. */
        if (len >= 33 && world.players[pid].docked) {
            int sidx = world.players[pid].current_station;
            if (sidx < 0 || sidx >= MAX_STATIONS) break;
            server_player_t *sp = &world.players[pid];
            station_t *st = &world.stations[sidx];
            ship_t *ship = &sp->ship;
            const uint8_t *pk = &data[1];
            int slot = manifest_find(&st->manifest, pk);
            if (slot < 0) break;
            cargo_unit_t *src = &st->manifest.units[slot];
            if ((cargo_kind_t)src->kind != CARGO_KIND_INGOT) break;
            /* Prefix-class price multipliers (#prefix-pricing): the
             * specific unit's sale price scales by both the dynamic
             * stock curve and the unit's prefix_class. Anonymous
             * ingots aren't purchasable through this path — they're
             * bulk material and have no named-collectible premium. */
            if ((ingot_prefix_t)src->prefix_class == INGOT_PREFIX_ANONYMOUS) break;
            int price = (int)lroundf(station_sell_price_unit(st, src));
            if (price <= 0) break;
            if (!station_manifest_bootstrap(st)) break;
            if (!ship_manifest_bootstrap(ship)) break;
            ship_receipts_t *station_receipts = station_get_receipts(st);
            cargo_receipt_chain_t station_chain = {0};
            if (station_receipts && slot < (int)station_receipts->count)
                station_chain = station_receipts->chains[slot];
            if (station_chain.len >= CARGO_RECEIPT_CHAIN_MAX_LEN) break;
            /* Use ledger_spend so the credit pool stays conserved. */
            bool spent = server_player_can_use_pubkey_persistence(sp)
                ? ledger_spend_by_pubkey(st, sp->pubkey, (float)price, ship)
                : ledger_spend(st, sp->session_token, (float)price, ship);
            if (!spent) break;
            cargo_unit_t copy = {0};
            if (!station_manifest_remove_with_chain(st, (uint16_t)slot,
                                                    &copy, &station_chain)) {
                break;
            }
            /* Layer C of #479: emit EVT_TRANSFER + EVT_TRADE. The two
             * are linked by transfer_event_id so a verifier can stitch
             * them back into a single atomic move.
             * Layer D of #479: also issue a portable cargo_receipt_t the
             * player carries with the cargo. The origin receipt's
             * prev_receipt_hash is the station's chain_last_hash AFTER
             * the EVT_TRANSFER emit — verifiable in isolation by
             * walking the station's chain log to that exact event. */
            {
                cargo_receipt_t receipt;
                uint8_t prev_hash[32] = {0};
                cargo_receipt_chain_t outgoing_chain = station_chain;
                if (station_chain.len > 0)
                    cargo_receipt_hash(&station_chain.links[station_chain.len - 1],
                                       prev_hash);
                uint64_t xfer_id = cargo_receipt_emit_transfer(
                    &world, st,
                    st->station_pubkey,
                    world.players[pid].pubkey,
                    copy.pub,
                    (uint8_t)CARGO_KIND_INGOT,
                    station_chain.len > 0 ? prev_hash : st->chain_last_hash,
                    &receipt);
                if (xfer_id != 0 && outgoing_chain.len < CARGO_RECEIPT_CHAIN_MAX_LEN)
                    outgoing_chain.links[outgoing_chain.len++] = receipt;
                if (!ship_manifest_push_with_chain(ship, &copy, &outgoing_chain)) {
                    (void)station_manifest_push_with_chain(st, &copy, &station_chain);
                    break;
                }
                if (xfer_id != 0) {
                    send_cargo_receipt_chain(c, &outgoing_chain);
                    chain_payload_trade_t trade = {0};
                    trade.transfer_event_id = xfer_id;
                    trade.ledger_delta_signed = -(int64_t)price;
                    memcpy(trade.ledger_pubkey, world.players[pid].pubkey, 32);
                    (void)chain_log_emit(&world, st, CHAIN_EVT_TRADE,
                                         &trade, (uint16_t)sizeof(trade));
                }
            }
            char cs[12]; mining_render_callsign(copy.pub, cs);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s purchased %s for %d", world.players[pid].callsign, cs, price);
            signal_channel_post(&world, sidx, msg, "");
        }
        break;
    case NET_MSG_DELIVER_INGOT:
        if (world.players[pid].pubkey_set) {
            unsigned_action_count++;
            break;
        }
        /* RATi v2: deposit a specific hold ingot into the docked
         * station's manifest. Payload: [type:1][hold_index:1]. The
         * index is into ship.manifest filtered by named ingots
         * (kind == INGOT && prefix != ANONYMOUS). */
        if (len >= 2 && world.players[pid].docked) {
            int sidx = world.players[pid].current_station;
            if (sidx < 0 || sidx >= MAX_STATIONS) break;
            station_t *st = &world.stations[sidx];
            ship_t *ship = &world.players[pid].ship;
            int target = data[1];
            int hidx = -1;
            int seen = 0;
            for (uint16_t u = 0; u < ship->manifest.count; u++) {
                const cargo_unit_t *cu = &ship->manifest.units[u];
                if ((cargo_kind_t)cu->kind != CARGO_KIND_INGOT) continue;
                if ((ingot_prefix_t)cu->prefix_class == INGOT_PREFIX_ANONYMOUS) continue;
                if (seen == target) { hidx = (int)u; break; }
                seen++;
            }
            if (hidx < 0) break;
            cargo_unit_t copy = ship->manifest.units[hidx];
            cargo_receipt_chain_t attached_chain = {0};
            /* Layer D of #479: validate any attached receipt chain
             * before accepting. If the chain fails verification, refuse
             * the deliver — federation invariant: a station only takes
             * cargo whose lineage is signed all the way back. If no
             * receipt chain is attached (legacy / pre-D save / cargo
             * smelted on-station with no transfer history), accept
             * unconditionally — the station treats the loading-state
             * cargo as origin-attested and signs a fresh receipt. */
            ship_receipts_t *rcpts = ship_get_receipts(ship);
            if (rcpts && hidx < (int)rcpts->count) {
                const cargo_receipt_chain_t *attached = &rcpts->chains[hidx];
                attached_chain = *attached;
                if (attached->len > 0) {
                    cargo_receipt_result_t vr = cargo_receipt_chain_verify(
                        attached->links, attached->len, copy.pub);
                    if (vr != CARGO_RECEIPT_OK) {
                        printf("[server] receipt_chain_invalid: deliver from player %d, reason=%d\n",
                               pid, (int)vr);
                        break; /* refuse the deliver */
                    }
                    if (attached->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
                        printf("[server] receipt_chain_cap_exceeded: deliver from player %d\n", pid);
                        break;
                    }
                }
            }
            /* FIFO-evict the oldest manifest entry on full station, mirroring
             * the smelt rotation path. The evicted unit's pubkey is voided
             * so it can never be re-deposited. */
            if (st->manifest.count >= st->manifest.cap) {
                cargo_unit_t evicted = {0};
                if (station_manifest_remove_with_chain(st, 0, &evicted, NULL) &&
                    (ingot_prefix_t)evicted.prefix_class != INGOT_PREFIX_ANONYMOUS) {
                    char ev_cs[12]; mining_render_callsign(evicted.pub, ev_cs);
                    char ev_msg[96];
                    snprintf(ev_msg, sizeof(ev_msg), "stockpile full — voided %s", ev_cs);
                    signal_channel_post(&world, sidx, ev_msg, "");
                }
            }
            /* Capture last receipt hash (for the new station-issued
             * receipt's prev_receipt_hash) BEFORE removing. */
            uint8_t prev_hash[32] = {0};
            bool have_prev = false;
            if (attached_chain.len > 0) {
                cargo_receipt_hash(&attached_chain.links[attached_chain.len - 1], prev_hash);
                have_prev = true;
            }
            cargo_receipt_chain_t removed_chain = {0};
            if (!ship_manifest_remove_with_chain(ship, (uint16_t)hidx,
                                                 &copy, &removed_chain)) {
                break;
            }
            /* Layer C of #479: emit EVT_TRANSFER (player -> station) +
             * EVT_TRADE (delivery credit accrual on the station's
             * ledger).
             * Layer D of #479: also issue station's own receipt. The
             * receipt's prev_receipt_hash is the SHA-256 of the player's
             * presented chain head if there was one — this hop closes
             * the chain at the destination station. If no prior chain,
             * anchor to the station's own chain_last_hash post-emit. */
            cargo_receipt_t receipt = {0};
            cargo_receipt_chain_t station_chain = removed_chain;
            uint64_t xfer_id = cargo_receipt_emit_transfer(
                &world, st,
                world.players[pid].pubkey,
                st->station_pubkey,
                copy.pub,
                (uint8_t)CARGO_KIND_INGOT,
                have_prev ? prev_hash : st->chain_last_hash,
                &receipt);
            if (xfer_id != 0 && station_chain.len < CARGO_RECEIPT_CHAIN_MAX_LEN)
                station_chain.links[station_chain.len++] = receipt;

            if (!station_manifest_push_with_chain(st, &copy, &station_chain)) {
                (void)ship_manifest_push_with_chain(ship, &copy, &removed_chain);
                break;
            }

            /* Pay delivery credit through the ledger so supply stays
             * balanced. Prefix-class price multipliers (#501): a specific
             * delivered unit pays station_buy_price_unit, so M-class
             * ingots pay 2× and RATi pays 50×. INGOT_DELIVERY_CREDIT is
             * kept as the floor for low-base-price edge cases. */
            float delivery_f = station_buy_price_unit(st, &copy);
            float floor_f = (float)INGOT_DELIVERY_CREDIT;
            if (delivery_f < floor_f) delivery_f = floor_f;
            int delivery_int = (int)lroundf(delivery_f);
            if (server_player_can_use_pubkey_persistence(&world.players[pid])) {
                ledger_credit_supply_by_pubkey(st, world.players[pid].pubkey, (float)delivery_int);
            } else {
                ledger_credit_supply(st, world.players[pid].session_token, (float)delivery_int);
            }
            {
                if (xfer_id != 0) {
                    /* The cargo is now in station custody, but returning
                     * the full reissued chain keeps the client as a
                     * potential bearer if this same cargo later comes
                     * back to the ship through a different authority. */
                    send_cargo_receipt_chain(c, &station_chain);

                    /* Trade event records the actual prefix-scaled
                     * delivery amount (#501), not a flat constant. */
                    chain_payload_trade_t trade = {0};
                    trade.transfer_event_id = xfer_id;
                    trade.ledger_delta_signed = (int64_t)delivery_int;
                    memcpy(trade.ledger_pubkey, world.players[pid].pubkey, 32);
                    (void)chain_log_emit(&world, st, CHAIN_EVT_TRADE,
                                         &trade, (uint16_t)sizeof(trade));
                }
            }
            char cs[12]; mining_render_callsign(copy.pub, cs);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s delivered %s", world.players[pid].callsign, cs);
            signal_channel_post(&world, sidx, msg, "");
        }
        break;
    case NET_MSG_PRESENT_RECEIPT_CHAIN:
        if (len >= 35) {
            const uint8_t *cargo_pub = &data[1];
            uint16_t chain_len = read_u16_le(&data[33]);
            size_t expected = 35u + (size_t)chain_len * CARGO_RECEIPT_SIZE;
            if (chain_len == 0 || chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN)
                break;
            if ((size_t)len < expected) break;

            cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN];
            for (uint16_t i = 0; i < chain_len; i++) {
                const uint8_t *p = &data[35u + (size_t)i * CARGO_RECEIPT_SIZE];
                (void)cargo_receipt_unpack(p, &chain[i]);
            }

            cargo_receipt_present_result_t pr = cargo_receipt_present_to_ship(
                &world.players[pid], cargo_pub, chain, (uint8_t)chain_len);
            if (pr != CARGO_RECEIPT_PRESENT_OK) {
                printf("[server] receipt_present rejected player=%d reason=%s\n",
                       pid, cargo_receipt_present_result_name(pr));
            }
        }
        break;
    case NET_MSG_FRACTURE_CLAIM:
        if (world.players[pid].pubkey_set) {
            unsigned_action_count++;
            break;
        }
        if (len >= FRACTURE_CLAIM_SIZE) {
            uint32_t fracture_id = read_u32_le(&data[1]);
            uint32_t burst_nonce = read_u32_le(&data[5]);
            uint8_t claimed_grade = data[9];
            (void)submit_fracture_claim(&world, pid, fracture_id, burst_nonce, claimed_grade);
        }
        break;
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
        server_player_t *sp = &world.players[pid];
        switch ((signed_action_type_t)action_type) {
        case SIGNED_ACTION_BUY_PRODUCT:
            /* Payload: [commodity:1][grade:1] — same fields the unsigned
             * NET_MSG_INPUT.action path produces. We just stuff them into
             * the same intent slot the sim already consumes. */
            if (payload_len >= 2) {
                uint8_t commodity = payload[0];
                uint8_t grade     = payload[1];
                if (commodity < COMMODITY_COUNT) {
                    sp->input.buy_product = true;
                    sp->input.buy_commodity = (commodity_t)commodity;
                    if (grade <= MINING_GRADE_COUNT)
                        sp->input.buy_grade = (mining_grade_t)grade;
                    else
                        sp->input.buy_grade = MINING_GRADE_COUNT;
                }
            }
            break;
        case SIGNED_ACTION_BUY_INGOT:
            /* Payload: [pubkey:32]. Mirrors the legacy NET_MSG_BUY_INGOT
             * transfer path for identity-backed clients. */
            if (payload_len >= 32 && sp->docked) {
                int sidx = sp->current_station;
                if (sidx >= 0 && sidx < MAX_STATIONS) {
                    station_t *st = &world.stations[sidx];
                    ship_t *ship = &sp->ship;
                    int slot = manifest_find(&st->manifest, payload);
                    if (slot >= 0) {
                        cargo_unit_t *src = &st->manifest.units[slot];
                        if ((cargo_kind_t)src->kind == CARGO_KIND_INGOT &&
                            (ingot_prefix_t)src->prefix_class != INGOT_PREFIX_ANONYMOUS) {
                            /* Prefix-class price multipliers (#prefix-pricing):
                             * mirror the unsigned BUY_INGOT path above. */
                            int price = (int)lroundf(station_sell_price_unit(st, src));
                            if (!station_manifest_bootstrap(st) ||
                                !ship_manifest_bootstrap(ship)) {
                                break;
                            }
                            bool spent = price > 0 && (server_player_can_use_pubkey_persistence(sp)
                                ? ledger_spend_by_pubkey(st, sp->pubkey, (float)price, ship)
                                : ledger_spend(st, sp->session_token, (float)price, ship));
                            if (spent) {
                                cargo_unit_t copy = {0};
                                cargo_receipt_chain_t station_chain = {0};
                                if (station_manifest_remove_with_chain(st, (uint16_t)slot,
                                                                       &copy, &station_chain)) {
                                    cargo_receipt_t receipt = {0};
                                    uint8_t prev_hash[32] = {0};
                                    cargo_receipt_chain_t outgoing_chain = station_chain;
                                    if (station_chain.len > 0) {
                                        cargo_receipt_hash(&station_chain.links[station_chain.len - 1],
                                                           prev_hash);
                                    }
                                    uint64_t xfer_id = cargo_receipt_emit_transfer(
                                        &world, st,
                                        st->station_pubkey,
                                        sp->pubkey,
                                        copy.pub,
                                        (uint8_t)CARGO_KIND_INGOT,
                                        station_chain.len > 0 ? prev_hash : st->chain_last_hash,
                                        &receipt);
                                    if (xfer_id != 0 &&
                                        outgoing_chain.len < CARGO_RECEIPT_CHAIN_MAX_LEN) {
                                        outgoing_chain.links[outgoing_chain.len++] = receipt;
                                    }
                                    if (!ship_manifest_push_with_chain(ship, &copy, &outgoing_chain)) {
                                        (void)station_manifest_push_with_chain(st, &copy, &station_chain);
                                    } else if (xfer_id != 0) {
                                        send_cargo_receipt_chain(c, &outgoing_chain);
                                        chain_payload_trade_t trade = {0};
                                        trade.transfer_event_id = xfer_id;
                                        trade.ledger_delta_signed = -(int64_t)price;
                                        memcpy(trade.ledger_pubkey, sp->pubkey, 32);
                                        (void)chain_log_emit(&world, st, CHAIN_EVT_TRADE,
                                                             &trade, (uint16_t)sizeof(trade));
                                        char cs[12]; mining_render_callsign(copy.pub, cs);
                                        char msg[96];
                                        snprintf(msg, sizeof(msg), "%s purchased %s for %d",
                                                 sp->callsign, cs, price);
                                        signal_channel_post(&world, sidx, msg, "");
                                    }
                                }
                            }
                        }
                    }
                }
            }
            break;
        case SIGNED_ACTION_SELL_CARGO:
            /* Payload: [commodity:1][grade:1]. commodity==COMMODITY_COUNT
             * means "sell all" (legacy bulk path). */
            if (payload_len >= 2) {
                uint8_t commodity = payload[0];
                uint8_t grade     = payload[1];
                sp->input.service_sell = true;
                sp->input.service_sell_only =
                    (commodity < COMMODITY_COUNT)
                    ? (commodity_t)commodity : COMMODITY_COUNT;
                if (grade < MINING_GRADE_COUNT) {
                    sp->input.service_sell_grade = (mining_grade_t)grade;
                    sp->input.service_sell_one = true;
                } else {
                    sp->input.service_sell_grade = MINING_GRADE_COUNT;
                    sp->input.service_sell_one = false;
                }
            }
            break;
        case SIGNED_ACTION_PLACE_OUTPOST:
            if (payload_len >= 3) {
                sp->input.place_outpost = true;
                sp->input.place_target_station = (int8_t)payload[0];
                sp->input.place_target_ring    = (int8_t)payload[1];
                sp->input.place_target_slot    = (int8_t)payload[2];
            }
            break;
        case SIGNED_ACTION_FRACTURE_CLAIM:
            if (payload_len >= 9) {
                uint32_t fracture_id = (uint32_t)payload[0]
                                     | ((uint32_t)payload[1] << 8)
                                     | ((uint32_t)payload[2] << 16)
                                     | ((uint32_t)payload[3] << 24);
                uint32_t burst_nonce = (uint32_t)payload[4]
                                     | ((uint32_t)payload[5] << 8)
                                     | ((uint32_t)payload[6] << 16)
                                     | ((uint32_t)payload[7] << 24);
                uint8_t claimed_grade = payload[8];
                (void)submit_fracture_claim(&world, pid, fracture_id,
                                            burst_nonce, claimed_grade);
            }
            break;
        case SIGNED_ACTION_DELIVER:
            if (payload_len >= 1) {
                handle_deliver_ingot_index(c, pid, payload[0]);
            }
            break;
        case SIGNED_ACTION_INPUT_ACTION:
            apply_signed_input_action(pid, payload, payload_len);
            break;
        case SIGNED_ACTION_PLAN:
            apply_signed_plan(pid, payload, payload_len);
            break;
        case SIGNED_ACTION_CLAIM_CONTRACT:
        case SIGNED_ACTION_CANCEL_CONTRACT:
            /* Reserved action types; no current client path dispatches
             * contract claim/cancel mutations. */
            break;
        case SIGNED_ACTION_COUNT:
        default:
            /* unreachable — verify rejected unknown types */
            break;
        }
        break;
    }
    case NET_MSG_REGISTER_PUBKEY:
        /* Layer A.2 of #479: client asserts its persisted Ed25519 pubkey.
         * The assertion alone does not bind registry or persistence; that
         * waits for NET_MSG_PROVE_PUBKEY. */
        if (len >= REGISTER_PUBKEY_MSG_SIZE) {
            const uint8_t *pk = &data[1];
            server_player_t *sp = &world.players[pid];
            if (sp->pubkey_set && memcmp(sp->pubkey, pk, 32) == 0) {
                finalize_verified_pubkey_identity(c, pid, now);
                break;
            }
            memcpy(sp->pubkey, pk, 32);
            sp->pubkey_set = true;
            sp->pubkey_proof_ok = false;
            sp->pubkey_identity_finalized = false;
            printf("[server] player %d: registered pubkey pending proof %02x%02x%02x%02x...\n",
                   pid, pk[0], pk[1], pk[2], pk[3]);
        }
        break;
    case NET_MSG_PROVE_PUBKEY:
        if (len >= PROVE_PUBKEY_MSG_SIZE) {
            server_player_t *sp = &world.players[pid];
            const uint8_t *pk = &data[PROVE_PUBKEY_PUBKEY_OFFSET];
            const uint8_t *token = &data[PROVE_PUBKEY_TOKEN_OFFSET];
            const uint8_t *sig = &data[PROVE_PUBKEY_SIG_OFFSET];
            if (!sp->pubkey_set || !sp->session_ready) break;
            if (memcmp(pk, sp->pubkey, 32) != 0) {
                printf("[server] player %d: pubkey proof rejected (pubkey mismatch)\n", pid);
                break;
            }
            if (memcmp(token, sp->session_token, 8) != 0) {
                printf("[server] player %d: pubkey proof rejected (session mismatch)\n", pid);
                break;
            }
            if (!pubkey_proof_verify(pk, token, sig)) {
                printf("[server] player %d: pubkey proof rejected (bad signature)\n", pid);
                break;
            }
            sp->pubkey_proof_ok = true;
            finalize_verified_pubkey_identity(c, pid, now);
        }
        break;
    case NET_MSG_CLAIM_LEGACY_SAVE: {
        /* Layer A.4 of #479. Client supplies (token_hex, signature). We
         * verify sig against the registered pubkey, then rename the
         * legacy save to the pubkey-keyed path and load it. */
        if (!persistence_load_enabled() || !persistence_save_enabled()) {
            printf("[server] player %d: legacy save claim ignored in %s mode\n",
                   pid, persistence_mode_name());
            break;
        }
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
        if (!ok) {
            char prefixed[96];
            snprintf(prefixed, sizeof(prefixed), "player_%s", basename);
            ok = player_save_rename_legacy_to_pubkey(PLAYER_SAVE_DIR,
                                                     prefixed, sp->pubkey);
        }
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
        if (len >= 9 && !world.players[pid].session_ready) {
            const uint8_t *token = &data[1];
            /* Extract callsign if present (bytes 9-15) */
            if (len >= 16) {
                memcpy(world.players[pid].callsign, &data[9], 7);
                world.players[pid].callsign[7] = '\0';
                printf("[server] player %d callsign: %s\n", pid, world.players[pid].callsign);
            }
            /* Check for existing grace-period player with same token */
            int reattach = -1;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (i == pid) continue;
                if (world.players[i].connected && world.players[i].grace_period &&
                    world.players[i].session_ready &&
                    memcmp(world.players[i].session_token, token, 8) == 0) {
                    reattach = i;
                    break;
                }
            }
            if (reattach >= 0) {
                /* Reattach: copy state from grace slot to new slot */
                server_player_t *old = &world.players[reattach];
                server_player_t *sp = &world.players[pid];
                if (!ship_copy(&sp->ship, &old->ship)) break;
                sp->current_station = old->current_station;
                sp->nearby_station = old->nearby_station;
                sp->docked = old->docked;
                sp->in_dock_range = old->in_dock_range;
                memcpy(sp->session_token, token, 8);
                sp->session_ready = true;
                /* Clear the grace slot and broadcast LEAVE so clients drop the ghost */
                old->connected = false;
                old->grace_period = false;
                old->conn = NULL;
                old->pubkey_proof_ok = false;
                old->pubkey_identity_finalized = false;
                uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)reattach };
                broadcast(leave_old, 2);
                printf("[server] player %d: reconnected (was slot %d)\n", pid, reattach);
            } else {
                memcpy(world.players[pid].session_token, token, 8);
                world.players[pid].session_ready = true;
                /* Try to restore saved state keyed by session token */
                if (persistence_load_enabled() &&
                    player_load_by_token(&world.players[pid], &world,
                                         PLAYER_SAVE_DIR, token)) {
                    printf("[server] player %d: restored save by session\n", pid);
                } else {
                    printf("[server] player %d: no save for session, fresh ship\n", pid);
                }
                /* Seed starting credits now that session_token is set */
                player_seed_credits(&world.players[pid], &world);
            }
            world.players[pid].last_input_action_id = 0;
            world.players[pid].last_input_action_id_valid = false;
            world.players[pid].pending_action_result_valid = false;
            finalize_verified_pubkey_identity(c, pid, now);
            analytics_record_activity(&world.players[pid], now);
            analytics_log_player_event("player_session", pid, &world.players[pid],
                                       now, 0);
            idle_shutdown_armed = true;
        }
        break;
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
    if (!(price_val > 0.0)) {
        if (out_error) *out_error = "invalid price";
        return false;
    }

    station_t *st = &world.stations[station_idx];
    float default_price = st->base_price[commodity];
    float clamped = (float)price_val;
    if (clamped < default_price * 0.5f) clamped = default_price * 0.5f;
    if (clamped > default_price * 2.0f) clamped = default_price * 2.0f;
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

static void handle_station_state(struct mg_connection *c, int sid, struct mg_http_message *hm) {
    const station_t *st = &world.stations[sid];

    /* Parse query params */
    int include_activity = 0;
    int include_chain_history = 0;
    char tmp[96];
    if (hm && mg_http_get_var(&hm->query, "include", tmp, sizeof(tmp)) > 0) {
        include_activity = strstr(tmp, "activity_history") != NULL || strcmp(tmp, "all") == 0;
        include_chain_history = strstr(tmp, "chain_history") != NULL || strcmp(tmp, "all") == 0;
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
            "\"%s\":%.1f", cnames[i], st->_inventory_cache[i]);
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
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &world.asteroids[i];
        if (!a->active) continue;
        if (v2_dist_sq(a->pos, st->pos) > sr_sq) continue;
        if (a->commodity < COMMODITY_RAW_ORE_COUNT) {
            asteroid_commodity_counts[a->commodity]++;
            if (a->tier == ASTEROID_TIER_S)
                asteroid_fragment_counts[a->commodity]++;
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
        if (!world.players[i].connected || world.players[i].grace_period) continue;
        if (v2_dist_sq(world.players[i].ship.pos, st->pos) > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"id\":%d,\"x\":%.0f,\"y\":%.0f,\"docked\":%s}",
            i, world.players[i].ship.pos.x, world.players[i].ship.pos.y,
            world.players[i].docked ? "true" : "false");
    }

    /* Visible NPCs within signal range. Kept compact for operator/debug
     * reads: role/state are enum ordinals matching shared/types.h. */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_npcs\":[");
    first = true;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &world.npc_ships[i];
        if (!npc->active) continue;
        if (v2_dist_sq(npc->ship.pos, st->pos) > sr_sq) continue;
        float cargo_total = 0.0f;
        for (int cc = 0; cc < COMMODITY_COUNT; cc++)
            cargo_total += npc->cargo[cc];
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
            npc->towed_fragment, cargo_total,
            npc->ship.pos.x, npc->ship.pos.y, npc->ship.vel.x, npc->ship.vel.y);
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
        float overlap = st->signal_range + world.stations[i].signal_range - sqrtf(d_sq);
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
        BUF_APPEND(pos, buf, BUFSZ, ",\"chain_history\":{\"operator_posts\":[");
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
                          "{\"ok\":true,\"action\":\"set_hail\",\"event_id\":%llu}",
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
                          "{\"ok\":true,\"action\":\"set_miner_chatter\",\"slot\":%ld,\"event_id\":%llu}",
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
                          "{\"ok\":true,\"action\":\"set_hauler_chatter\",\"slot\":%ld,\"event_id\":%llu}",
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
                          "{\"ok\":true,\"action\":\"set_rati_hail\",\"event_id\":%llu}",
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
                          "{\"ok\":true,\"action\":\"set_currency_name\",\"value\":\"%s\"}", sanitized);
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
                          "{\"ok\":true,\"action\":\"set_price\",\"commodity\":%ld,\"price\":%.1f}",
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
                          "{\"ok\":true,\"action\":\"build_module\",\"type\":%ld}", module_type);
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

/* REST API token-bucket rate limiter: 20 tokens/sec, 40 burst cap */
static uint64_t api_rate_last_refill = 0;
static int api_rate_bucket = 40;
#define API_RATE_REFILL_PER_SEC 20
#define API_RATE_BUCKET_MAX 40

static bool api_rate_check(void) {
    uint64_t now = mg_millis();
    uint64_t elapsed = now - api_rate_last_refill;
    if (elapsed >= 50) {  /* refill every 50ms to smooth out bursts */
        int refill = (int)(elapsed * API_RATE_REFILL_PER_SEC / 1000);
        if (refill > 0) {
            api_rate_bucket += refill;
            if (api_rate_bucket > API_RATE_BUCKET_MAX)
                api_rate_bucket = API_RATE_BUCKET_MAX;
            api_rate_last_refill = now;
        }
    }
    if (api_rate_bucket <= 0) return false;
    api_rate_bucket--;
    return true;
}

static const char *protocol_msg_name(uint8_t msg) {
    switch (msg) {
    case NET_MSG_SERVER_INFO: return "SERVER_INFO";
    case NET_MSG_INPUT: return "INPUT";
    case NET_MSG_LATENCY_PING: return "LATENCY_PING";
    case NET_MSG_LATENCY_PONG: return "LATENCY_PONG";
    case NET_MSG_CLIENT_METRICS: return "CLIENT_METRICS";
    case NET_MSG_STATION_IDENTITY: return "STATION_IDENTITY";
    case NET_MSG_STATION_DIAG: return "STATION_DIAG";
    case NET_MSG_WORLD_PLAYERS: return "WORLD_PLAYERS";
    case NET_MSG_PLAYER_SHIP: return "PLAYER_SHIP";
    case NET_MSG_WORLD_STATIONS: return "WORLD_STATIONS";
    case NET_MSG_STATION_MANIFEST: return "STATION_MANIFEST";
    case NET_MSG_PLAYER_MANIFEST: return "PLAYER_MANIFEST";
    case NET_MSG_STATION_INGOTS: return "STATION_INGOTS";
    case NET_MSG_HOLD_INGOTS: return "HOLD_INGOTS";
    case NET_MSG_CONTRACTS: return "CONTRACTS";
    case NET_MSG_INSPECT_SNAPSHOT: return "INSPECT_SNAPSHOT";
    case NET_MSG_CARGO_RECEIPT_BUNDLE: return "CARGO_RECEIPT_BUNDLE";
    case NET_MSG_PRESENT_RECEIPT_CHAIN: return "PRESENT_RECEIPT_CHAIN";
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

    enum { PROTOCOL_JSON_BUFSZ = 8192 };
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
    mg_http_reply(c, 200, api_headers, "%s", out);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/protocol"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                handle_protocol_info_http(c);
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/state"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else if (sid >= 3 && !api_auth_ok(hm)) {
                    /* Seeded stations (0-2) are read-only without auth;
                     * player-built outposts (3+) require auth. */
                    mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
                } else {
                    handle_station_state(c, sid, hm);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/command"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
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
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
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
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                /* Parse ?since=<id>&limit=<1..100> — crude query scan
                 * since mongoose gives us hm->query as a raw string. */
                long since = 0, limit = 50;
                char tmp[32];
                if (mg_http_get_var(&hm->query, "since", tmp, sizeof(tmp)) > 0) since = atol(tmp);
                if (mg_http_get_var(&hm->query, "limit", tmp, sizeof(tmp)) > 0) limit = atol(tmp);
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
        } else if (mg_match(hm->uri, mg_str("/health"), NULL)) {
            int count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (world.players[i].connected) count++;
            int live_connections = live_player_connection_count();
            uint64_t idle_empty_for_ms = 0;
            if (idle_shutdown_empty_since_ms != 0) {
                uint64_t health_now = mg_millis();
                if (health_now >= idle_shutdown_empty_since_ms)
                    idle_empty_for_ms = health_now - idle_shutdown_empty_since_ms;
            }
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
                       "\"version\":\"%s\","
                       "\"persistence\":{\"mode\":\"%s\","
                       "\"load_enabled\":%s,\"save_enabled\":%s,"
                       "\"externalized\":%s,\"external_store\":\"%s\","
                       "\"state_uri\":\"",
                       count, live_connections, version,
                       persistence_mode_name(),
                       persistence_load_enabled() ? "true" : "false",
                       persistence_save_enabled() ? "true" : "false",
                       persistence_externalized() ? "true" : "false",
                       persistence_externalized() ? "s3" : "none");
            json_escape_append(buf, &pos, HEALTH_BUFSZ, persistence_state_uri);
            BUF_APPEND(pos, buf, HEALTH_BUFSZ, "\",\"data_dir\":\"");
            json_escape_append(buf, &pos, HEALTH_BUFSZ, persistence_data_dir);
            BUF_APPEND(pos, buf, HEALTH_BUFSZ,
                       "\"},"
                       "\"idle_shutdown\":{\"enabled\":%s,\"armed\":%s,"
                       "\"after_sec\":%llu,\"empty_for_ms\":%llu},"
                       "\"signed_action_count\":%llu,"
                       "\"signed_action_reject_count\":%llu,"
                       "\"unsigned_action_count\":%llu,"
                       "\"hopper_smelt_events\":%llu,"
                       "\"hopper_smelt_units\":%.3f,"
                       "\"chain\":{\"status\":\"%s\",\"chain_dir\":\"",
                       idle_shutdown_after_ms > 0 ? "true" : "false",
                       idle_shutdown_armed ? "true" : "false",
                       (unsigned long long)(idle_shutdown_after_ms / 1000ull),
                       (unsigned long long)idle_empty_for_ms,
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
        } else {
            mg_http_reply(c, 404, "", "Not found");
        }
    } else if (ev == MG_EV_WS_OPEN) {
        /* Per-IP connection limit to mitigate slot exhaustion */
        #define MAX_CONNS_PER_IP 4
        {
            int ip_count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (world.players[i].connected && world.players[i].conn) {
                    struct mg_connection *pc = (struct mg_connection *)world.players[i].conn;
                    if (memcmp(&pc->rem.addr, &c->rem.addr, sizeof(c->rem.addr)) == 0)
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
        memset(sp, 0, sizeof(*sp));
        sp->connected = true;
        sp->id = (uint8_t)pid;
        sp->conn = c;
        sp->session_ready = false;
        sp->grace_timer = 5.0f;  /* Must send SESSION within 5 seconds */
        sp->analytics_connected_ms = mg_millis();
        sp->analytics_last_activity_ms = sp->analytics_connected_ms;
        /* Start with fresh ship — save is loaded when client sends SESSION */
        player_init_ship(sp, &world);
        printf("[server] player %d: awaiting session token\n", pid);
        analytics_log_player_event("player_connect", pid, sp,
                                   sp->analytics_connected_ms, 0);

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

        /* Notify others and tell new player about existing players. */
        broadcast_except(pid, join_msg, 2);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pid || !world.players[i].connected) continue;
            if (world.players[i].grace_period) continue; /* skip ghosts */
            uint8_t exist_msg[] = { NET_MSG_JOIN, (uint8_t)i };
            ws_send(c, exist_msg, 2);
        }

        /* Send station identity for all active stations. */
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&world.stations[s])) continue;
            uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
            int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
            ws_send(c, id_buf, (size_t)id_len);
            uint8_t diag_buf[STATION_DIAG_SIZE];
            int diag_len = serialize_station_diag(diag_buf, s, &world.stations[s]);
            ws_send(c, diag_buf, (size_t)diag_len);
        }

        /* Send the same relevance-filtered asteroid view used by the
         * periodic world tick. A full-belt join burst can be tens of KB;
         * behind the production ALB that was enough to delay inbound
         * REGISTER_PUBKEY / SESSION processing until the auth timer fired. */
        {
            uint8_t sync_buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
            server_player_t *new_sp = &world.players[pid];
            int sync_len = serialize_asteroids_for_player(
                sync_buf, world.asteroids, new_sp->ship.pos, new_sp->asteroid_sent);
            if (sync_len > ASTEROID_MSG_HEADER)
                ws_send(c, sync_buf, (size_t)sync_len);
        }

        /* Global highscores: newcomer gets the current leaderboard so the
         * death cinematic can render it before they've played a run. */
        send_highscores_to(c);

        /* Signal channel snapshot (#316): newcomer gets the full ring
         * buffer so the Network tab has content immediately. */
        if (world.signal_channel.count > 0) {
            size_t cap = (size_t)(3 + world.signal_channel.count * SIGNAL_CHANNEL_RECORD_SIZE);
            uint8_t *msg = (uint8_t *)malloc(cap);
            if (msg) {
                int len = serialize_signal_channel(msg, &world.signal_channel);
                ws_send(c, msg, (size_t)len);
                free(msg);
            }
        }

        /* RATi v2: per-station named-ingot snapshot, derived from the
         * unified manifest. New client sees what's currently on offer
         * at every station so the MARKET stockpile UI is populated
         * immediately. Wire shape unchanged. */
        for (int sidx = 0; sidx < MAX_STATIONS; sidx++) {
            if (!station_exists(&world.stations[sidx])) continue;
            uint8_t buf[STATION_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
            int len = serialize_station_ingots(buf, sidx, &world.stations[sidx]);
            if (len <= STATION_INGOTS_HEADER) continue;
            ws_send(c, buf, (size_t)len);
        }

        /* Phase 2 station manifest summary — same rationale, different
         * payload: grade-grouped counts for the TRADE BUY rows. Sent for
         * every station (empty manifest is legal — body will be just the
         * 4-byte header with entry_count=0). */
        for (int sidx = 0; sidx < MAX_STATIONS; sidx++) {
            if (!station_exists(&world.stations[sidx])) continue;
            uint8_t mbuf[STATION_MANIFEST_HEADER +
                         COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY];
            int mlen = serialize_station_manifest(mbuf, sidx, &world.stations[sidx]);
            ws_send(c, mbuf, (size_t)mlen);
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
            if (world.players[i].conn == c) {
                uint64_t now_ms = mg_millis();
                uint64_t duration_ms =
                    (world.players[i].analytics_connected_ms != 0 &&
                     now_ms >= world.players[i].analytics_connected_ms)
                    ? now_ms - world.players[i].analytics_connected_ms
                    : 0;
                analytics_log_player_event("player_disconnect", i,
                                           &world.players[i], now_ms,
                                           duration_ms);
                if (persistence_save_enabled())
                    player_save(&world.players[i], PLAYER_SAVE_DIR, i);
                world.players[i].conn = NULL;
                if (world.players[i].session_ready) {
                    /* Keep slot alive for reconnect grace window */
                    world.players[i].grace_period = true;
                    world.players[i].grace_timer = 30.0f;
                    printf("[server] player %d disconnected, grace window 30s\n", i);
                } else {
                    /* No session — immediate full disconnect */
                    world.players[i].connected = false;
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

static void broadcast_player_states(void) {
    /* Batch all connected player states into one message, send once per client.
     * This is O(N) sends instead of O(N^2). */
    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    uint32_t server_tick = world.tick;
    int len = serialize_all_player_states(buf, world.players, server_tick);
    broadcast(buf, (size_t)len);
}

/* mark_visible_asteroids_dirty removed — per-player relevance filtering
 * in serialize_asteroids_for_player handles viewport culling. */

static void broadcast_world(void) {
    /* Asteroids: per-player relevance filtering.
     * Each player gets only asteroids in their view radius.
     * Deactivation records sent when asteroids leave a player's view. */
    {
        uint8_t abuf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->session_ready || !sp->conn) continue;
            int alen = serialize_asteroids_for_player(
                abuf, world.asteroids, sp->ship.pos, sp->asteroid_sent);
            if (alen > 2) /* skip empty messages */
                ws_send(sp->conn, abuf, (size_t)alen);
        }
        /* Bulk clear dirty flags after all players served */
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            world.asteroids[i].net_dirty = false;
    }

    /* NPCs: per-player view filtering (same radius as asteroids) */
    {
        uint8_t nbuf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->session_ready || !sp->conn) continue;
            int count = 0;
            for (int i = 0; i < MAX_NPC_SHIPS; i++) {
                if (!world.npc_ships[i].active) continue;
                if (v2_dist_sq(world.npc_ships[i].ship.pos, sp->ship.pos) > ASTEROID_VIEW_RADIUS_SQ)
                    continue;
                const npc_ship_t *n = &world.npc_ships[i];
                uint8_t *q = &nbuf[2 + count * NPC_RECORD_SIZE];
                q[0] = (uint8_t)i;
                q[1] = 1;
                q[1] |= (((uint8_t)n->role & 0x3) << 1);
                q[1] |= (((uint8_t)n->state & 0x7) << 3);
                if (n->thrusting) q[1] |= (1 << 6);
                write_f32_le(&q[2],  n->ship.pos.x);
                write_f32_le(&q[6],  n->ship.pos.y);
                write_f32_le(&q[10], n->ship.vel.x);
                write_f32_le(&q[14], n->ship.vel.y);
                write_f32_le(&q[18], n->ship.angle);
                uint16_t target = (n->target_asteroid >= 0 && n->target_asteroid < MAX_ASTEROIDS)
                    ? (uint16_t)n->target_asteroid : 0xFFFFu;
                uint16_t towed = (n->towed_fragment >= 0 && n->towed_fragment < MAX_ASTEROIDS)
                    ? (uint16_t)n->towed_fragment : 0xFFFFu;
                write_u16_le(&q[22], target);
                write_u16_le(&q[24], towed);
                q[26] = (uint8_t)(n->tint_r * 255.0f);
                q[27] = (uint8_t)(n->tint_g * 255.0f);
                q[28] = (uint8_t)(n->tint_b * 255.0f);
                count++;
            }
            nbuf[0] = NET_MSG_WORLD_NPCS;
            nbuf[1] = (uint8_t)count;
            ws_send(sp->conn, nbuf, (size_t)(2 + count * NPC_RECORD_SIZE));
        }
    }

    /* Scaffolds: per-player view filtering */
    {
        uint8_t scbuf[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->session_ready || !sp->conn) continue;
            int count = 0;
            for (int i = 0; i < MAX_SCAFFOLDS; i++) {
                if (!world.scaffolds[i].active) continue;
                if (v2_dist_sq(world.scaffolds[i].pos, sp->ship.pos) > ASTEROID_VIEW_RADIUS_SQ)
                    continue;
                serialize_one_scaffold(&scbuf[2 + count * SCAFFOLD_RECORD_SIZE], i, &world.scaffolds[i]);
                count++;
            }
            scbuf[0] = NET_MSG_WORLD_SCAFFOLDS;
            scbuf[1] = (uint8_t)count;
            if (count > 0)
                ws_send(sp->conn, scbuf, (size_t)(2 + count * SCAFFOLD_RECORD_SIZE));
        }
    }

    /* World time sync (5 bytes: type + float) */
    uint8_t tbuf[5];
    tbuf[0] = NET_MSG_WORLD_TIME;
    write_f32_le(&tbuf[1], world.time);
    broadcast(tbuf, 5);
}

/* Compute station-local balance for a player at their current/nearby
 * station. Must read the same ledger entry the buy/credit paths use:
 * pubkey when verified, session-token-pseudokey otherwise. Reading
 * the wrong entry was the visible-bug-symptom that motivated the
 * earlier identity-fix series — broadcast balance came from a stale
 * (often negative) session-token entry while real earnings sat on
 * the pubkey entry. */
static float player_station_balance(const server_player_t *sp) {
    int st = sp->docked ? sp->current_station : sp->nearby_station;
    if (st < 0 || st >= MAX_STATIONS) return 0.0f;
    if (server_player_can_use_pubkey_persistence(sp))
        return ledger_balance_by_pubkey(&world.stations[st], sp->pubkey);
    return ledger_balance(&world.stations[st], sp->session_token);
}

static int send_player_ship(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    return serialize_player_ship_bal(buf, id, sp, player_station_balance(sp));
}

static int send_inspect_snapshot(uint8_t *buf, const server_player_t *sp) {
    if (!sp || !sp->scan_active || sp->scan_target_type == INSPECT_TARGET_NONE)
        return serialize_inspect_snapshot_target(buf, INSPECT_TARGET_NONE, -1, -1);

    if (sp->scan_target_type == INSPECT_TARGET_NPC &&
        sp->scan_target_index >= 0 &&
        sp->scan_target_index < MAX_NPC_SHIPS) {
        const npc_ship_t *npc = &world.npc_ships[sp->scan_target_index];
        ship_t *ship = world_npc_ship_for(&world, sp->scan_target_index);
        return serialize_inspect_snapshot_npc(buf, (uint8_t)sp->scan_target_index,
                                              npc, ship);
    }

    return serialize_inspect_snapshot_target(buf, sp->scan_target_type,
                                             sp->scan_target_index,
                                             sp->scan_module_index);
}

static void broadcast_ship_states(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (!sp->connected || !sp->session_ready || !sp->conn) continue;
        uint8_t buf[PLAYER_SHIP_SIZE + 4]; /* +4 headroom */
        int len = send_player_ship(buf, (uint8_t)i, sp);
        /* Full ship state sent only to the owning player. */
        ws_send_if_changed(sp->conn, &sp->player_ship_cache, buf, (size_t)len);

        /* RATi v2: also push hold-ingot snapshot, derived from the
         * ship manifest. Wire shape unchanged. Sized for the wire cap
         * (u8 count) so an unusually full hold can't truncate. */
        uint8_t hbuf[HOLD_INGOTS_HEADER + 255 * NAMED_INGOT_RECORD_SIZE];
        int hlen = serialize_hold_ingots(hbuf, &sp->ship);
        ws_send_if_changed(sp->conn, &sp->hold_ingots_cache, hbuf, (size_t)hlen);

        /* Player manifest summary — keeps the trade UI's SELL rows in
         * sync with server-authoritative manifest mutations (buy/sell/
         * smelt move units across the player's manifest server-side, but
         * PLAYER_SHIP only carries the float cargo). Worst case is
         * COMMODITY_COUNT * MINING_GRADE_COUNT entries; we cap header
         * + entries with a generous bound. */
        uint8_t pmbuf[PLAYER_MANIFEST_HEADER
                      + COMMODITY_COUNT * MINING_GRADE_COUNT * PLAYER_MANIFEST_ENTRY];
        int pmlen = serialize_player_manifest(pmbuf, &sp->ship);
        ws_send_if_changed(sp->conn, &sp->player_manifest_cache, pmbuf, (size_t)pmlen);

        uint8_t ibuf[INSPECT_SNAPSHOT_MAX_SIZE];
        int ilen = send_inspect_snapshot(ibuf, sp);
        ws_send_if_changed(sp->conn, &sp->inspect_snapshot_cache, ibuf, (size_t)ilen);

        /* Gossip-contract visibility mask. The dock UI on the client
         * reads NET_MSG_CONTRACTS for the global authoritative array
         * but filters by this per-player mask so the player only sees
         * contracts they've heard about via dock contact. 5 bytes. */
        uint8_t kbuf[5];
        int klen = serialize_player_known_contracts(kbuf, world.contracts,
                                                    &sp->ship);
        ws_send_if_changed(sp->conn, &sp->known_contracts_cache, kbuf, (size_t)klen);
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
        broadcast(cbuf, (size_t)clen);
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
    if (!sp->connected || !sp->conn) {
        station_econ_dirty = true;
        contracts_dirty = true;
        return;
    }
    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->conn, buf, (size_t)len);

    int st_idx = sp->current_station;
    if (st_idx >= 0 && st_idx < MAX_STATIONS) {
        uint8_t sbuf[2 + STATION_RECORD_SIZE];
        sbuf[0] = NET_MSG_WORLD_STATIONS;
        sbuf[1] = 1;
        uint8_t *p = &sbuf[2];
        p[0] = (uint8_t)st_idx;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            write_f32_le(&p[1 + c * 4], world.stations[st_idx]._inventory_cache[c]);
        ws_send(sp->conn, sbuf, (size_t)(2 + STATION_RECORD_SIZE));
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

    if (!sp->conn) return;

    /* Death packet: [type:1][pid:1][px:f32][py:f32][vx:f32][vy:f32]
     * [ang:f32][ore:f32][earned:f32][spent:f32][asteroids:f32]
     * [respawn_station:u8][respawn_fee:f32] = 43 bytes */
    uint8_t msg[43];
    msg[0] = NET_MSG_DEATH;
    msg[1] = (uint8_t)pid;
    write_f32_le(&msg[2],  ev->death.pos_x);
    write_f32_le(&msg[6],  ev->death.pos_y);
    write_f32_le(&msg[10], ev->death.vel_x);
    write_f32_le(&msg[14], ev->death.vel_y);
    write_f32_le(&msg[18], ev->death.angle);
    write_f32_le(&msg[22], ev->death.ore_mined);
    write_f32_le(&msg[26], ev->death.credits_earned);
    write_f32_le(&msg[30], ev->death.credits_spent);
    write_f32_le(&msg[34], (float)ev->death.asteroids_fractured);
    msg[38] = ev->death.respawn_station;
    write_f32_le(&msg[39], ev->death.respawn_fee);
    ws_send(sp->conn, msg, sizeof(msg));

    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->conn, buf, (size_t)len);
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
    if (!sp->connected || !sp->conn) return;

    uint8_t msg[7];
    msg[0] = NET_MSG_HAIL_RESPONSE;
    msg[1] = (uint8_t)ev->hail_response.station;
    write_f32_le(&msg[2], ev->hail_response.credits);
    int ci = ev->hail_response.contract_index;
    int compact_ci = contract_compact_index_for_slot(world.contracts, ci);
    msg[6] = (compact_ci >= 0) ? (uint8_t)compact_ci : 0xFF;
    ws_send(sp->conn, msg, sizeof(msg));

    contracts_dirty = true;
    /* Push fresh ship state so the credit bump is visible immediately. */
    uint8_t buf[PLAYER_SHIP_SIZE + 4];
    int len = send_player_ship(buf, (uint8_t)pid, sp);
    ws_send(sp->conn, buf, (size_t)len);
}

/* OUTPOST_PLACED / OUTPOST_ACTIVATED / MODULE_ACTIVATED / SCAFFOLD_READY
 * all need station identity refreshed so the client sees updated
 * module / pending lists. */
static void srv_mark_all_stations_identity_dirty(void) {
    for (int s = 0; s < MAX_STATIONS; s++) station_identity_dirty[s] = true;
}

/* Fan a single sim event out to its per-type broadcaster(s). Multiple
 * "if" branches on event type (rather than a switch) so events that
 * fall into more than one bucket — OUTPOST_PLACED touches both
 * srv_on_outpost_placed AND the structure-event identity refresh —
 * all run. */
static void srv_dispatch_sim_event(const sim_event_t *ev) {
    if (ev->type == SIM_EVENT_OUTPOST_PLACED) srv_on_outpost_placed(ev);
    if (ev->type == SIM_EVENT_SELL ||
        ev->type == SIM_EVENT_REPAIR ||
        ev->type == SIM_EVENT_UPGRADE ||
        ev->type == SIM_EVENT_DOCK ||
        ev->type == SIM_EVENT_LAUNCH) {
        srv_on_player_state_change(ev);
    }
    if (ev->type == SIM_EVENT_DEATH)              srv_on_death(ev);
    if (ev->type == SIM_EVENT_CONTRACT_COMPLETE)  srv_on_contract_complete(ev);
    if (ev->type == SIM_EVENT_HAIL_RESPONSE)      srv_on_hail_response(ev);
    if (ev->type == SIM_EVENT_OUTPOST_PLACED ||
        ev->type == SIM_EVENT_OUTPOST_ACTIVATED ||
        ev->type == SIM_EVENT_MODULE_ACTIVATED ||
        ev->type == SIM_EVENT_SCAFFOLD_READY) {
        srv_mark_all_stations_identity_dirty();
    }
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
    if (!port) port = "8080";
    const char *mode = getenv("SIGNAL_PERSISTENCE_MODE");
    if (!mode || mode[0] == '\0') mode = "local";
    if (strcmp(mode, "local") == 0) {
        persistence_mode = PERSISTENCE_LOCAL;
    } else if (strcmp(mode, "ephemeral") == 0) {
        persistence_mode = PERSISTENCE_EPHEMERAL;
    } else if (strcmp(mode, "external_s3") == 0) {
        persistence_mode = PERSISTENCE_EXTERNAL_S3;
    } else {
        fprintf(stderr, "[FATAL] invalid SIGNAL_PERSISTENCE_MODE=%s (use local, ephemeral, or external_s3)\n",
                mode);
        return false;
    }
    persistence_data_dir = getenv("SIGNAL_DATA_DIR");
    if (!persistence_data_dir || persistence_data_dir[0] == '\0')
        persistence_data_dir = ".";
    persistence_state_uri = getenv("SIGNAL_STATE_S3_URI");
    if (!persistence_state_uri) persistence_state_uri = "";
    if (persistence_mode == PERSISTENCE_EXTERNAL_S3 &&
        persistence_state_uri[0] == '\0') {
        fprintf(stderr, "[FATAL] external_s3 persistence requires SIGNAL_STATE_S3_URI\n");
        return false;
    }
    printf("[server] Persistence mode: %s (data_dir=%s state_uri=%s)\n",
           persistence_mode_name(), persistence_data_dir,
           persistence_state_uri[0] ? persistence_state_uri : "none");
    if (persistence_mode == PERSISTENCE_EPHEMERAL) {
        printf("[server] Ephemeral persistence: local save/catalog/player files are ignored and not written\n");
    }
    {
        const char *idle = getenv("SIGNAL_IDLE_SHUTDOWN_AFTER_SEC");
        if (idle && idle[0] != '\0') {
            char *end = NULL;
            errno = 0;
            unsigned long seconds = strtoul(idle, &end, 10);
            if (errno != 0 || end == idle || *end != '\0') {
                fprintf(stderr, "[FATAL] invalid SIGNAL_IDLE_SHUTDOWN_AFTER_SEC=%s\n", idle);
                return false;
            }
            idle_shutdown_after_ms = (uint64_t)seconds * 1000ull;
        }
        if (idle_shutdown_after_ms > 0) {
            printf("[server] Idle shutdown: enabled after %llu second(s) with no live player connections\n",
                   (unsigned long long)(idle_shutdown_after_ms / 1000ull));
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
        } else if (persistence_mode == PERSISTENCE_EXTERNAL_S3 ||
                   getenv("SIGNAL_REQUIRE_STATION_AUTH_SECRET")) {
            fprintf(stderr, "[FATAL] station authority requires SIGNAL_STATION_AUTH_SECRET "
                            "or SIGNAL_API_TOKEN for this persistence mode\n");
            return false;
        } else {
            station_authority_use_dev_secret();
            fprintf(stderr, "[WARN] using deterministic development station authority secret "
                            "(set SIGNAL_STATION_AUTH_SECRET in production)\n");
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
    if (!persistence_load_enabled() && !persistence_save_enabled()) return;
    MKDIR_PATH(PLAYER_SAVE_DIR);
    MKDIR_PATH(STATION_CATALOG_DIR);
    /* Layer A.4 of #479: ensure pubkey/ + legacy/ subdirs exist and any
     * existing top-level *.sav files (v39 and earlier layout) get moved
     * into legacy/ so the new path layout takes effect. Idempotent. */
    player_save_migrate_legacy_layout(PLAYER_SAVE_DIR);
}

static bool enter_persistence_data_dir(void) {
    if (!persistence_load_enabled() && !persistence_save_enabled()) return true;
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

/* Layered persistence (#314):
 *   1. world_reset() seeds starter stations + belt field
 *   2. Catalog overwrites identity for any persisted stations
 *   3. Session snapshot overlays economy state
 *   4. Rebuild derived structures (signal chain, station nav, hash chain) */
static void load_world_state(void) {
    /* Belt seed is persistent across normal restarts: rotate only when
     * world.sav is absent (true first boot of a fresh world). On a
     * resume, world_load below overwrites belt_seed and world_seq with
     * the persisted values so asteroid layout, station Ed25519 pubkeys,
     * and the leaderboard's world ordering all stay stable. */
    bool fresh_world = true;
    if (persistence_load_enabled()) {
        FILE *probe = fopen(SAVE_PATH, "rb");
        if (probe) {
            fclose(probe);
            fresh_world = false;
        }
    } else {
        printf("[server] %s mode: skipping local world/catalog/player load\n",
               persistence_mode_name());
    }
    if (fresh_world) {
        world.rng = (uint32_t)time(NULL);
        if (!world.rng) world.rng = 2037u;
    }
    world_reset(&world);

    if (persistence_load_enabled()) {
        int catalog_count = station_catalog_load_all(world.stations, MAX_STATIONS,
                                                     STATION_CATALOG_DIR);
        if (catalog_count > 0)
            printf("[server] loaded %d station(s) from catalog\n", catalog_count);
    }

    if (persistence_load_enabled() && world_load(&world, SAVE_PATH)) {
        printf("[server] loaded session from %s (belt_seed=%u world_seq=%u)\n",
               SAVE_PATH, world.belt_seed, world.world_seq);
    } else {
        /* fresh_world above already rotated rng; stamp world_seq from the
         * wall clock so cross-wipe ordering is monotonic too. */
        world.world_seq = (uint32_t)time(NULL);
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
     * set by world_reset() get wiped. Re-stamp the names for the three
     * starter stations whenever they come back empty. */
    static const char *defaults[3] = {
        "prospect vouchers", "kepler bonds", "helios credits",
    };
    for (int i = 0; i < 3 && i < MAX_STATIONS; i++) {
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
    if (persistence_load_enabled())
        signal_chain_load(&world);

    /* Highscores are now a *view* of the chain log: walk every
     * chain/<base58>.log file and project CHAIN_EVT_DEATH events
     * through highscore_submit. Old chain files from prior worlds
     * survive as orphans (their station pubkeys differ once
     * belt_seed rotates) and contribute alongside the current
     * world's runs — each row carries its world_id. */
    if (persistence_load_enabled()) {
        highscore_replay_from_chain(&highscores, chain_log_get_dir());
        if (highscores.count > 0)
            printf("[server] replayed %d highscore(s) from chain log\n",
                   highscores.count);
    }

    /* Anchor the current world's identity in station 0's chain. The
     * BUILD_INFO + WORLD_INFO operator posts let the replay walker
     * tag every subsequent DEATH event with this world's belt_seed
     * and build SHA. emit_world_identity_anchor is below. */
    emit_world_identity_anchor();
}

/* Forward-declared above load_world_state but defined here so it can
 * use chain_log_emit + the static GIT_HASH constant. Idempotent: emits
 * one BUILD_INFO and one WORLD_INFO every server start (the chain log
 * grows by 2 events per restart, which is fine). */
static void emit_world_identity_anchor(void) {
    if (!station_exists(&world.stations[0])) return;
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
        (void)chain_log_emit(&world, &world.stations[0],
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
        (void)chain_log_emit(&world, &world.stations[0],
                             CHAIN_EVT_OPERATOR_POST,
                             payload, (uint16_t)(38 + text_len));
    }
}

/* Run as many fixed-step sim ticks as `sim_accum` covers, up to
 * MAX_SIM_STEPS, broadcasting per-event side effects after each tick.
 * Caller passes the running accumulator + the elapsed-since-last-call
 * seconds. Returns when steps run out or the accumulator is empty. */
static void run_sim_ticks(float *sim_accum, float elapsed) {
    *sim_accum += elapsed;
    int steps = 0;
    while (*sim_accum >= SIM_DT && steps < MAX_SIM_STEPS) {
        world_sim_step(&world, SIM_DT);
        for (int e = 0; e < world.events.count; e++)
            srv_dispatch_sim_event(&world.events.events[e]);
        if (world.events.count > 0) {
            uint8_t ebuf[2 + SIM_MAX_EVENTS * NET_EVENT_RECORD_SIZE];
            int elen = serialize_events(ebuf, &world.events);
            if (elen > 2) broadcast(ebuf, (size_t)elen);
        }
        send_pending_action_results(&world.events);
        broadcast_fracture_updates();
        *sim_accum -= SIM_DT;
        steps++;
    }
    if (*sim_accum > SIM_DT) *sim_accum = 0.0f; /* prevent spiral */
}

/* Tick down per-player grace timers and session-auth timeouts. */
static void tick_session_timers(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &world.players[i];
        if (sp->connected && sp->grace_period) {
            sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
            if (sp->grace_timer <= 0.0f) {
                sp->connected = false;
                sp->grace_period = false;
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
                mg_ws_send(sp->conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                sp->connected = false;
                sp->conn = NULL;
                uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                broadcast(leave_msg, 2);
            }
        }
    }
}

static void tick_idle_shutdown(uint64_t now) {
    if (idle_shutdown_after_ms == 0 || !idle_shutdown_armed) return;

    int live_connections = live_player_connection_count();
    if (live_connections > 0) {
        idle_shutdown_empty_since_ms = 0;
        return;
    }

    if (idle_shutdown_empty_since_ms == 0) {
        idle_shutdown_empty_since_ms = now;
        printf("[server] no live player connections; idle shutdown in %llu second(s)\n",
               (unsigned long long)(idle_shutdown_after_ms / 1000ull));
        return;
    }

    if (now >= idle_shutdown_empty_since_ms &&
        now - idle_shutdown_empty_since_ms >= idle_shutdown_after_ms) {
        printf("[server] idle shutdown after %llu second(s) with no live player connections\n",
               (unsigned long long)(idle_shutdown_after_ms / 1000ull));
        running = false;
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
            if (!world.players[p].connected || !world.players[p].conn) continue;
            if (v2_dist_sq(world.players[p].ship.pos, world.stations[s].pos) <= sr_sq)
                ws_send(world.players[p].conn, id_buf, (size_t)id_len);
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
            if (!world.players[p].connected || !world.players[p].conn) continue;
            ws_send(world.players[p].conn, buf, (size_t)len);
        }
        uint8_t mbuf[STATION_MANIFEST_HEADER +
                     COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY];
        int mlen = serialize_station_manifest(mbuf, s, &world.stations[s]);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!world.players[p].connected || !world.players[p].conn) continue;
            ws_send(world.players[p].conn, mbuf, (size_t)mlen);
        }
        world.stations[s].manifest_dirty = false;
    }
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

    char listen_url[64];
    if (!read_env_config(listen_url, sizeof(listen_url))) return 1;

    chain_log_set_disk_enabled(persistence_save_enabled());
    signal_chain_set_disk_enabled(persistence_save_enabled());
    if (!enter_persistence_data_dir()) return 1;
    ensure_persistence_dirs();
    load_world_state();

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, listen_url, ev_handler, NULL);
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
        mg_mgr_poll(&mgr, 1);
        uint64_t now = mg_millis();

        if (now - last_sim >= SIM_TICK_MS) {
            float elapsed = (float)(now - last_sim) / 1000.0f;
            last_sim = now;
            run_sim_ticks(&sim_accum, elapsed);
            tick_session_timers();
            /* Mark econ dirty every ~1s as fallback for production changes. */
            if (now - last_econ_dirty >= 1000) {
                station_econ_dirty = true;
                contracts_dirty = true;
                last_econ_dirty = now;
            }
        }
        if (now - last_state >= STATE_TICK_MS) {
            broadcast_player_states();
            last_state = now;
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
        tick_idle_shutdown(now);
        if (now - last_save >= AUTOSAVE_MS) {
            if (persistence_save_enabled()) {
                station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
                world_save(&world, SAVE_PATH);
            }
            last_save = now;
        }
    }

    mg_mgr_free(&mgr);
    if (persistence_save_enabled()) {
        station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
        world_save(&world, SAVE_PATH);
        printf("[server] world saved\n");
    } else {
        printf("[server] world save skipped (%s mode)\n", persistence_mode_name());
    }
    printf("[server] shutdown\n");
    return 0;
}
