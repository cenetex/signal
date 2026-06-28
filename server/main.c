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
        if (world.players[i].connected && world.players[i].conn) count++;
    }
    return count;
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
static uint64_t last_player_state_emit = 0;

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

static void reset_player_slot_for_reuse(int pid) {
    if (pid < 0 || pid >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[pid];
    (void)world_player_release_ship_asset(&world, pid);
    ship_cleanup(&sp->ship);
    memset(sp, 0, sizeof(*sp));
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
        sp->conn = NULL;
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
        sp->autopilot_last_pos = sp->ship.pos;
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

static void invalidate_player_authoritative_caches(server_player_t *sp) {
    if (!sp) return;
    sp->player_ship_cache.valid = false;
    sp->hold_ingots_cache.valid = false;
    sp->player_manifest_cache.valid = false;
    sp->inspect_snapshot_cache.valid = false;
    sp->known_contracts_cache.valid = false;
    sp->known_ledger_cache.valid = false;
    sp->delivery_ledger_cache.valid = false;
}

static void force_player_authoritative_resync(server_player_t *sp) {
    if (sp) sp->force_authoritative_resync = true;
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
        if (server_player_is_gameplay_ready(sp) && sp->conn) {
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
        if (server_player_is_gameplay_ready(&world.players[i]) &&
            world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static void ws_packet_sink(void *user, const uint8_t *data, int len) {
    struct mg_connection *c = (struct mg_connection *)user;
    if (c && data && len > 0) ws_send(c, data, (size_t)len);
}

typedef struct {
    struct mg_connection *conn;
    server_player_t *player;
} ws_private_packet_sink_t;

static void ws_private_packet_sink(void *user, const uint8_t *data, int len) {
    ws_private_packet_sink_t *sink = (ws_private_packet_sink_t *)user;
    if (!sink || !sink->conn || !sink->player || !data || len <= 0) return;
    net_payload_cache_t *cache = NULL;
    switch (data[0]) {
    case NET_MSG_PLAYER_SHIP:
        cache = &sink->player->player_ship_cache;
        break;
    case NET_MSG_HOLD_INGOTS:
        cache = &sink->player->hold_ingots_cache;
        break;
    case NET_MSG_PLAYER_MANIFEST:
        cache = &sink->player->player_manifest_cache;
        break;
    case NET_MSG_INSPECT_SNAPSHOT:
        cache = &sink->player->inspect_snapshot_cache;
        break;
    case NET_MSG_PLAYER_KNOWN_CONTRACTS:
        cache = &sink->player->known_contracts_cache;
        break;
    case NET_MSG_PLAYER_KNOWN_LEDGER:
        cache = &sink->player->known_ledger_cache;
        break;
    case NET_MSG_DELIVERY_LEDGER:
        cache = &sink->player->delivery_ledger_cache;
        break;
    default:
        break;
    }
    ws_send_if_changed(sink->conn, cache, data, (size_t)len);
}

static void ws_player_packet_sink(void *user, int player_slot,
                                  const uint8_t *data, int len) {
    (void)user;
    if (!data || len <= 0) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &world.players[player_slot];
    if (!server_player_is_gameplay_ready(sp) || !sp->conn) return;
    ws_send(sp->conn, data, (size_t)len);
}

static void broadcast_except(int exclude, const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude) continue;
        if (server_player_is_gameplay_ready(&world.players[i]) &&
            world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static void broadcast_fracture_updates(void) {
    server_emit_fracture_updates(&world, -1, ws_player_packet_sink, NULL);
}

/* ------------------------------------------------------------------ */
/* WS message handler                                                 */
/* ------------------------------------------------------------------ */

/* Per-player WebSocket message rate limiting */
static struct { uint64_t window_start; int msg_count; } ws_rate[MAX_PLAYERS];
#define WS_RATE_WINDOW_MS 1000
#define WS_RATE_LIMIT 140 /* 60Hz input + signed/plan bursts without drops */

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
                (struct mg_connection *)old->conn;
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
            old->grace_period = false;
            old->conn = NULL;
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
    } else if (true &&
        player_load_by_pubkey(sp, &world, PLAYER_SAVE_DIR, pk)) {
        printf("[server] player %d: restored save by pubkey\n", pid);
    } else if (true) {
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
    if (!server_player_has_live_session(&world.players[pid]) &&
        !ws_message_allowed_before_session(data[0])) {
        return;
    }

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
        server_player_t *sp = &world.players[pid];
        server_input_dispatch_result_t input_result;
        if (!server_dispatch_input_message(&world, pid, data, len,
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
        /* Movement-only input acks ride the authoritative WORLD_PLAYERS
         * stream. ACTION_ACK is only for one-shot actions or rejections. */
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
        if (!true || !true) {
            printf("[server] player %d: legacy save claim ignored in %s mode\n",
                   pid, "local");
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
            bool reattached_live_state = false;
            if (reattach >= 0) {
                /* Reattach: copy state from grace slot to new slot */
                server_player_t *old = &world.players[reattach];
                if (!world_player_transfer_ship_state(&world, pid, reattach))
                    break;
                if (!server_apply_session_message(&world, pid, &session))
                    break;
                /* Clear the grace slot and broadcast LEAVE so clients drop the ghost */
                old->connected = false;
                old->grace_period = false;
                old->conn = NULL;
                server_player_clear_live_session_identity(old);
                server_player_clear_transient_input(old);
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
            server_player_reset_input_stream(&world.players[pid]);
            force_player_authoritative_resync(&world.players[pid]);
            world.players[pid].pending_action_result_valid = false;
            finalize_verified_pubkey_identity(c, pid, now,
                                              reattached_live_state);
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
        if (isfinite(sp->ship.stat_credits_earned) &&
            sp->ship.stat_credits_earned > reference) {
            reference = sp->ship.stat_credits_earned;
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
        float active_credits = sp->ship.stat_credits_earned;
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
            npc_towed_fragment_index(npc), cargo_total,
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
    int count = npc->known_contract_count;
    if (count > SHIP_KNOWN_CONTRACT_CAP) count = SHIP_KNOWN_CONTRACT_CAP;
    for (int i = 0; i < count; i++) {
        const contract_summary_t *cs = &npc->known_contracts[i];
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
    int item_count = npc->knowledge.count;
    if (item_count > KNOWLEDGE_VIEW_MAX_CAP) item_count = KNOWLEDGE_VIEW_MAX_CAP;
    for (int i = 0; i < item_count; i++) {
        const knowledge_item_t *item = &npc->knowledge.items[i];
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
    float cargo_total = 0.0f;
    for (int c = 0; c < COMMODITY_COUNT; c++)
        cargo_total += npc->cargo[c];

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
               npc->ship.pos.x, npc->ship.pos.y,
               npc->ship.vel.x, npc->ship.vel.y,
               npc->hull, cargo_total, npc->target_asteroid,
               npc_towed_fragment_index(npc), npc->towed_scaffold);
    int cargo_written = 0;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        if (npc->cargo[c] <= 0.0f) continue;
        if (cargo_written++ > 0) BUF_APPEND(*pos, buf, bufsz, ",");
        BUF_APPEND(*pos, buf, bufsz,
                   "{\"commodity\":%d,\"commodity_code\":\"%s\",\"amount\":%.1f}",
                   c, commodity_code((commodity_t)c), npc->cargo[c]);
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
    case NET_MSG_INPUT_APPLIED: return "INPUT_APPLIED";
    case NET_MSG_STATION_IDENTITY: return "STATION_IDENTITY";
    case NET_MSG_STATION_DIAG: return "STATION_DIAG";
    case NET_MSG_WORLD_PLAYERS: return "WORLD_PLAYERS";
    case NET_MSG_WORLD_CARGO_PODS: return "WORLD_CARGO_PODS";
    case NET_MSG_WORLD_INTERACTIONS: return "WORLD_INTERACTIONS";
    case NET_MSG_PLAYER_SHIP: return "PLAYER_SHIP";
    case NET_MSG_PLAYER_KNOWN_LEDGER: return "PLAYER_KNOWN_LEDGER";
    case NET_MSG_WORLD_STATIONS: return "WORLD_STATIONS";
    case NET_MSG_STATION_MANIFEST: return "STATION_MANIFEST";
    case NET_MSG_PLAYER_MANIFEST: return "PLAYER_MANIFEST";
    case NET_MSG_STATION_INGOTS: return "STATION_INGOTS";
    case NET_MSG_HOLD_INGOTS: return "HOLD_INGOTS";
    case NET_MSG_CONTRACTS: return "CONTRACTS";
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
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/protocol"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                handle_protocol_info_http(c);
            }
        } else if (mg_match(hm->uri, mg_str("/api/npc_chatter_context"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else {
                handle_npc_chatter_context(c, hm);
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/state"), NULL)) {
            if (!api_rate_check()) {
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
                       true ? "true" : "false",
                       true ? "true" : "false",
                       false ? "true" : "false",
                       false ? "s3" : "none");
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
        reset_player_slot_for_reuse(pid);
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

        /* Tell new player about existing gameplay-ready players. Others learn
         * about this slot after SESSION is accepted. */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pid || !server_player_is_gameplay_ready(&world.players[i]))
                continue;
            uint8_t exist_msg[] = { NET_MSG_JOIN, (uint8_t)i };
            ws_send(c, exist_msg, 2);
        }

        /* Send the same station snapshot bundle local loopback uses. */
        server_emit_station_snapshot(
            &world, true, ws_packet_sink, c, &station_snapshot_scratch);

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
                if (world.players[i].session_ready)
                    player_save(&world.players[i], PLAYER_SAVE_DIR, i);
                world.players[i].conn = NULL;
                if (world.players[i].session_ready) {
                    /* Keep slot alive for reconnect grace window */
                    world.players[i].grace_period = true;
                    world.players[i].grace_timer = 30.0f;
                    printf("[server] player %d disconnected, grace window 30s\n", i);
                } else {
                    /* No session — immediate full disconnect */
                    (void)world_player_release_ship_asset(&world, i);
                    world.players[i].connected = false;
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
    for (int p = 0; p < MAX_PLAYERS; p++) {
        server_player_t *sp = &world.players[p];
        if (!sp->connected || !sp->session_ready || !sp->conn) continue;
        server_emit_world_snapshot_for_player(
            &world, p, false, ws_packet_sink, sp->conn,
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
        if (!sp->connected || !sp->session_ready || !sp->conn) continue;
        if (sp->force_authoritative_resync)
            invalidate_player_authoritative_caches(sp);
        ws_private_packet_sink_t sink = {
            .conn = sp->conn,
            .player = sp,
        };
        server_emit_private_snapshot_for_player(
            &world, i, ws_private_packet_sink, &sink,
            &private_snapshot_scratch);
        sp->force_authoritative_resync = false;
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

    uint8_t msg[NET_DEATH_MSG_SIZE];
    int death_len = serialize_death(msg, (uint8_t)pid, ev);
    ws_send(sp->conn, msg, (size_t)death_len);

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

    uint8_t msg[NET_HAIL_RESPONSE_REASON_SIZE];
    int msg_len = serialize_hail_response_for_world(msg, &world, ev);
    if (msg_len > 0) ws_send(sp->conn, msg, (size_t)msg_len);

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
    printf("[server] Persistence: local (data_dir=%s)\n", persistence_data_dir);
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
        } else if (
                   getenv("SIGNAL_REQUIRE_STATION_AUTH_SECRET")) {
            fprintf(stderr, "[FATAL] station authority requires SIGNAL_STATION_AUTH_SECRET "
                            "or SIGNAL_API_TOKEN\n");
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
    if (!true && !true) return;
    MKDIR_PATH(PLAYER_SAVE_DIR);
    MKDIR_PATH(STATION_CATALOG_DIR);
    /* Layer A.4 of #479: ensure pubkey/ + legacy/ subdirs exist and any
     * existing top-level *.sav files (v39 and earlier layout) get moved
     * into legacy/ so the new path layout takes effect. Idempotent. */
    player_save_migrate_legacy_layout(PLAYER_SAVE_DIR);
}

static bool enter_persistence_data_dir(void) {
    if (!true && !true) return true;
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
    if (npc->known_contract_count >= SHIP_KNOWN_CONTRACT_CAP) return;
    npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)ct->action,
        .station_index = ct->station_index,
        .commodity = (uint8_t)ct->commodity,
        .required_grade = ct->required_grade,
        .proof_flags = ct->proof_flags,
        .required_prefix_class = ct->required_prefix_class,
        .required_recipe_id = ct->required_recipe_id,
        .quantity_needed = ct->quantity_needed,
        .base_price = ct->base_price,
        .age_at_copy = ct->age,
        .forbidden_origin_mask = ct->forbidden_origin_mask,
    };
    memcpy(npc->known_contracts[npc->known_contract_count - 1].required_parent,
           ct->required_parent,
           sizeof(ct->required_parent));
    memcpy(npc->known_contracts[npc->known_contract_count - 1].target_pub,
           ct->target_pub,
           sizeof(ct->target_pub));
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
    npc->known_contract_count = 0;
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
static void load_world_state(void) {
    /* Belt seed is persistent across normal restarts: rotate only when
     * world.sav is absent (true first boot of a fresh world). On a
     * resume, world_load below overwrites belt_seed and world_seq with
     * the persisted values so asteroid layout, station Ed25519 pubkeys,
     * and the leaderboard's world ordering all stay stable. */
    bool fresh_world = true;
    if (true) {
        FILE *probe = fopen(SAVE_PATH, "rb");
        if (probe) {
            fclose(probe);
            fresh_world = false;
        }
    } else {
        printf("[server] %s mode: skipping local world/catalog/player load\n",
               "local");
    }
    if (fresh_world) {
        world.rng = fresh_world_seed_override
            ? fresh_world_seed_override
            : (uint32_t)time(NULL);
        if (!world.rng) world.rng = 2037u;
    }
    world_reset(&world);

    if (true) {
        int catalog_count = station_catalog_load_all(world.stations, MAX_STATIONS,
                                                     STATION_CATALOG_DIR);
        if (catalog_count > 0)
            printf("[server] loaded %d station(s) from catalog\n", catalog_count);
    }

    if (true && world_load(&world, SAVE_PATH)) {
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
    if (true)
        signal_chain_load(&world);

    /* Highscores are now a *view* of the chain log: walk every
     * chain/<base58>.log file and project CHAIN_EVT_DEATH events
     * through highscore_submit. Old chain files from prior worlds
     * survive as orphans (their station pubkeys differ once
     * belt_seed rotates) and contribute alongside the current
     * world's runs — each row carries its world_id. */
    if (true) {
        highscore_replay_from_chain(&highscores, chain_log_get_dir());
        if (highscores.count > 0)
            printf("[server] replayed %d highscore(s) from chain log\n",
                   highscores.count);
    }

    /* Anchor the current world's identity in every station chain. The
     * BUILD_INFO + WORLD_INFO operator posts let replay/analyzer walks
     * tag subsequent events with this world's belt_seed, world_seq, and
     * build SHA. emit_world_identity_anchor is below. */
    emit_world_identity_anchor();
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

/* Run as many fixed-step sim ticks as `sim_accum` covers, up to
 * MAX_SIM_STEPS, broadcasting per-event side effects after each tick.
 * Caller passes the running accumulator + the elapsed-since-last-call
 * seconds. Returns true if it emitted an immediate player-state flush. */
static bool run_sim_ticks(float *sim_accum, float elapsed, uint64_t now) {
    uint16_t input_ack_before[MAX_PLAYERS];
    for (int i = 0; i < MAX_PLAYERS; i++)
        input_ack_before[i] = world.players[i].last_input_seq;

    *sim_accum += elapsed;
    int steps = 0;
    (void)now;
    while (*sim_accum >= SIM_DT && steps < MAX_SIM_STEPS) {
        world_sim_step(&world, SIM_DT);
        mark_station_identity_dirty_for_hull_inventory_changes();
        for (int p = 0; p < MAX_PLAYERS; p++) {
            const server_player_t *sp = &world.players[p];
            if (!server_player_is_gameplay_ready(sp)) continue;
            if (server_emit_input_applied_if_changed(
                    sp, input_ack_before[p], world.tick,
                    ws_packet_sink, sp->conn)) {
                input_ack_before[p] = sp->last_input_seq;
            }
        }
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
                if (sp->conn)
                    mg_ws_send(sp->conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                (void)world_player_release_ship_asset(&world, i);
                sp->connected = false;
                sp->conn = NULL;
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
                !world.players[p].conn) continue;
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
            if (!server_player_is_gameplay_ready(&world.players[p]) ||
                !world.players[p].conn) continue;
            ws_send(world.players[p].conn, buf, (size_t)len);
        }
        uint8_t mbuf[STATION_MANIFEST_HEADER +
                     COMMODITY_COUNT * MINING_GRADE_COUNT * STATION_MANIFEST_ENTRY];
        int mlen = serialize_station_manifest(mbuf, s, &world.stations[s]);
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!server_player_is_gameplay_ready(&world.players[p]) ||
                !world.players[p].conn) continue;
            ws_send(world.players[p].conn, mbuf, (size_t)mlen);
        }
        world.stations[s].manifest_dirty = false;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

/* aws-swarm avatar keypair (imported at startup) */
static uint8_t g_avatar_nacl_secret[64];
static bool g_has_avatar_keypair = false;

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
    load_world_state();
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
     * When SIGNAL_AVATAR_KEYPAIR_B64 is set, use the avatar's Ed25519
     * keypair for the player's station identity instead of deriving one
     * from the operator secret. The keypair is base64-encoded NaCl format
     * (seed || pubkey = 64 bytes).
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
                memcpy(g_avatar_nacl_secret, nacl_secret, 64);
                g_has_avatar_keypair = true;
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
        mg_mgr_poll(&mgr, 1);
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
            broadcast_player_states();
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
            if (true) {
                station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
                world_save(&world, SAVE_PATH);
            }
            last_save = now;
        }
    }

    mg_mgr_free(&mgr);
    if (true) {
        station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
        world_save(&world, SAVE_PATH);
        printf("[server] world saved\n");
    } else {
        printf("[server] world save skipped (%s mode)\n", "local");
    }
    printf("[server] shutdown\n");
    return 0;
}
