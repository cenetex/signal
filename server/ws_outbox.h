#ifndef SERVER_WS_OUTBOX_H
#define SERVER_WS_OUTBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Per-WebSocket application outbox.
 *
 * The payload arena is fixed-size so a non-reading peer cannot grow process
 * memory. Normal traffic cannot allocate the final 16 KiB (or the final 32
 * descriptors), preserving bounded capacity for close/error/action receipts.
 * Transport bytes already queued below this layer count against the same
 * logical ceiling at admission time.
 */
enum {
    WS_OUTBOX_HARD_BYTES = 256 * 1024,
    WS_OUTBOX_TRANSPORT_CONTROL_RESERVE_BYTES = 1024,
    WS_OUTBOX_APP_HARD_BYTES =
        WS_OUTBOX_HARD_BYTES -
        WS_OUTBOX_TRANSPORT_CONTROL_RESERVE_BYTES,
    WS_OUTBOX_CONTROL_RESERVE_BYTES = 16 * 1024,
    WS_OUTBOX_NORMAL_LIMIT_BYTES =
        WS_OUTBOX_APP_HARD_BYTES - WS_OUTBOX_CONTROL_RESERVE_BYTES,
    WS_OUTBOX_MAX_FRAME_BYTES = 80 * 1024,
    WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES = 96 * 1024,
    WS_OUTBOX_TRANSPORT_LIMIT_BYTES =
        WS_OUTBOX_TRANSPORT_HARD_LIMIT_BYTES -
        WS_OUTBOX_TRANSPORT_CONTROL_RESERVE_BYTES,
    WS_OUTBOX_PRESSURE_ENTER_BYTES = 64 * 1024,
    WS_OUTBOX_PRESSURE_RECOVER_BYTES = 32 * 1024,
    WS_OUTBOX_WARNING_BYTES = 128 * 1024,
    WS_OUTBOX_WARNING_SUSTAIN_MS = 2000,
    WS_OUTBOX_NO_PROGRESS_MS = 15000,
    WS_OUTBOX_PRESSURE_DISCONNECT_MS = 30000,
    WS_OUTBOX_CLOSE_DRAIN_MS = 1000,
    WS_OUTBOX_PAGE_BYTES = 256,
    WS_OUTBOX_PAGE_COUNT = WS_OUTBOX_HARD_BYTES / WS_OUTBOX_PAGE_BYTES,
    WS_OUTBOX_NORMAL_PAGE_COUNT =
        WS_OUTBOX_NORMAL_LIMIT_BYTES / WS_OUTBOX_PAGE_BYTES,
    WS_OUTBOX_MAX_FRAMES = 512,
    WS_OUTBOX_NORMAL_FRAME_COUNT = 480,
    WS_OUTBOX_SYNC_RATE_BYTES_PER_SEC = 512 * 1024,
    WS_OUTBOX_SYNC_BURST_BYTES = WS_OUTBOX_MAX_FRAME_BYTES,
    WS_BACKPRESSURE_FIXTURE_STATION_COUNT = 4,
    WS_BACKPRESSURE_FIXTURE_NAMED_INGOTS = 16,
    WS_BACKPRESSURE_FIXTURE_STATION0_FRAMES = 224,
    WS_BACKPRESSURE_FIXTURE_OTHER_STATION_FRAMES = 240,
    WS_BACKPRESSURE_FIXTURE_STATION0_DETAILS =
        WS_BACKPRESSURE_FIXTURE_NAMED_INGOTS +
        WS_BACKPRESSURE_FIXTURE_STATION0_FRAMES
};

#define WS_OUTBOX_PAGE_NONE UINT16_MAX

typedef enum {
    WS_OUTBOX_LANE_CONTROL = 0,
    WS_OUTBOX_LANE_RELIABLE = 1,
    WS_OUTBOX_LANE_REPLACEABLE = 2
} ws_outbox_lane_t;

typedef enum {
    WS_OUTBOX_ADMITTED = 0,
    WS_OUTBOX_COALESCED,
    WS_OUTBOX_SUPPRESSED,
    WS_OUTBOX_FATAL,
    WS_OUTBOX_CLOSED
} ws_outbox_result_t;

typedef enum {
    WS_OUTBOX_CLOSE_NONE = 0,
    WS_OUTBOX_CLOSE_QUEUE_HARD_LIMIT,
    WS_OUTBOX_CLOSE_CONTROL_HEADROOM_EXHAUSTED,
    WS_OUTBOX_CLOSE_NO_WRITE_PROGRESS,
    WS_OUTBOX_CLOSE_SUSTAINED_PRESSURE,
    WS_OUTBOX_CLOSE_FRAME_TOO_LARGE,
    WS_OUTBOX_CLOSE_DESCRIPTOR_EXHAUSTED,
    WS_OUTBOX_CLOSE_TRANSPORT_REJECTED
} ws_outbox_close_reason_t;

typedef enum {
    WS_REPLICATION_CYCLE_PRIVATE = 0,
    WS_REPLICATION_CYCLE_BULK
} ws_replication_cycle_t;

typedef enum {
    WS_OUTBOX_FAMILY_NONE = 0,
    WS_OUTBOX_FAMILY_STATION_IDENTITY,
    WS_OUTBOX_FAMILY_STATION_DIAG,
    WS_OUTBOX_FAMILY_STATION_MANIFEST,
    WS_OUTBOX_FAMILY_WORLD_STATIONS,
    WS_OUTBOX_FAMILY_CONTRACTS,
    WS_OUTBOX_FAMILY_PLAYER_SHIP,
    WS_OUTBOX_FAMILY_PLAYER_MANIFEST,
    WS_OUTBOX_FAMILY_WORLD_PLAYERS,
    WS_OUTBOX_FAMILY_WORLD_PLAYER_DOCK,
    WS_OUTBOX_FAMILY_WORLD_TIME,
    WS_OUTBOX_FAMILY_WORLD_TOW_LINKS,
    WS_OUTBOX_FAMILY_HIGHSCORES,
    WS_OUTBOX_FAMILY_SIGNAL_CHANNEL,
    WS_OUTBOX_FAMILY_KNOWN_CONTRACTS,
    WS_OUTBOX_FAMILY_MARKET_MEMORIES,
    WS_OUTBOX_FAMILY_KNOWN_LEDGER,
    WS_OUTBOX_FAMILY_DELIVERY_LEDGER,
    WS_OUTBOX_FAMILY_INSPECT_SNAPSHOT
} ws_outbox_family_t;

typedef struct {
    uint16_t family;
    uint32_t object_id;
} ws_outbox_key_t;

typedef struct {
    ws_outbox_lane_t lane;
    ws_outbox_key_t key;
    uint8_t message_type;
} ws_outbox_policy_t;

typedef struct {
    uint64_t admitted_packets;
    uint64_t admitted_payload_bytes;
    uint64_t coalesced_packets;
    uint64_t coalesced_payload_bytes;
    uint64_t suppressed_packets;
    uint64_t suppressed_payload_bytes;
    uint64_t sent_packets;
    uint64_t sent_payload_bytes;
    uint64_t sent_wire_bytes;
    uint64_t warning_events;
    uint64_t recovery_events;
    uint64_t disconnect_events;
    uint64_t critical_failures;
    size_t queue_payload_bytes;
    size_t queue_wire_bytes;
    size_t high_water_bytes;
    size_t high_water_frames;
} ws_outbox_stats_t;

typedef struct {
    bool active;
    uint8_t lane;
    uint8_t message_type;
    uint16_t first_page;
    uint16_t page_count;
    uint16_t family;
    uint32_t object_id;
    uint32_t payload_len;
    uint32_t wire_bytes;
    uint64_t sequence;
} ws_outbox_frame_t;

typedef struct {
    uint8_t pages[WS_OUTBOX_PAGE_COUNT][WS_OUTBOX_PAGE_BYTES];
    uint16_t page_next[WS_OUTBOX_PAGE_COUNT];
    uint8_t page_used[WS_OUTBOX_PAGE_COUNT];
    ws_outbox_frame_t frames[WS_OUTBOX_MAX_FRAMES];
    ws_outbox_stats_t stats;
    uint64_t next_sequence;
    uint64_t pressure_enter_ms;
    uint64_t last_write_progress_ms;
    size_t frame_count;
    bool pressure_active;
    bool warning_active;
    bool needs_resync;
    ws_outbox_close_reason_t close_reason;
} ws_outbox_t;

typedef struct {
    uint64_t credit_byte_ms;
    uint64_t last_refill_ms;
} ws_sync_pacer_t;

typedef enum {
    WS_INITIAL_SYNC_NONE = 0,
    WS_INITIAL_SYNC_PRIVATE,
    WS_INITIAL_SYNC_STATION_IDENTITY,
    WS_INITIAL_SYNC_STATION_DIAG,
    WS_INITIAL_SYNC_STATION_MANIFEST,
    WS_INITIAL_SYNC_WORLD_STATIONS,
    WS_INITIAL_SYNC_CONTRACTS,
    WS_INITIAL_SYNC_ASTEROIDS,
    WS_INITIAL_SYNC_HIGHSCORES,
    WS_INITIAL_SYNC_SIGNAL_CHANNEL,
    WS_INITIAL_SYNC_CATCHUP,
    WS_INITIAL_SYNC_DONE
} ws_initial_sync_step_t;

typedef struct {
    ws_sync_pacer_t pacer;
    uint64_t started_ms;
    uint64_t completed_ms;
    uint64_t catchup_suppressed_packets;
    uint64_t catchup_snapshot_generation;
    uint16_t station_index;
    uint16_t station_count;
    uint8_t step;
    uint8_t substep;
    bool active;
    bool reconcile_pending;
    bool catchup_snapshot_admitted;
} ws_initial_sync_t;

typedef bool (*ws_outbox_transport_send_fn)(void *user,
                                            const uint8_t *payload,
                                            size_t payload_len);

void ws_outbox_init(ws_outbox_t *outbox, uint64_t now_ms);
void ws_outbox_reset(ws_outbox_t *outbox, uint64_t now_ms);

ws_outbox_policy_t ws_outbox_classify(const uint8_t *payload,
                                      size_t payload_len);
ws_outbox_result_t ws_outbox_enqueue(ws_outbox_t *outbox,
                                     const uint8_t *payload,
                                     size_t payload_len,
                                     size_t transport_bytes,
                                     uint64_t now_ms);
size_t ws_outbox_wire_bytes(size_t payload_len);
bool ws_outbox_can_admit_control_frame(
    const ws_outbox_t *outbox,
    const uint8_t *payload,
    size_t payload_len,
    size_t transport_bytes);
bool ws_outbox_can_admit_reliable_batch(
    const ws_outbox_t *outbox,
    const uint8_t *const *payloads,
    const size_t *payload_lengths,
    size_t payload_count,
    size_t transport_bytes);

/*
 * Pumps all available control frames before at most one non-control frame.
 * The caller supplies a contiguous scratch buffer because arena pages are
 * deliberately non-contiguous.
 */
size_t ws_outbox_pump(ws_outbox_t *outbox,
                      size_t transport_bytes,
                      size_t transport_limit_bytes,
                      uint64_t now_ms,
                      uint8_t *scratch,
                      size_t scratch_capacity,
                      ws_outbox_transport_send_fn send,
                      void *send_user);

void ws_outbox_note_write_progress(ws_outbox_t *outbox,
                                   uint64_t now_ms,
                                   size_t bytes_written);
void ws_outbox_note_suppressed(ws_outbox_t *outbox, size_t payload_len);
ws_outbox_close_reason_t ws_outbox_check_timeouts(ws_outbox_t *outbox,
                                                  uint64_t now_ms,
                                                  size_t transport_bytes);

size_t ws_outbox_total_bytes(const ws_outbox_t *outbox,
                             size_t transport_bytes);
bool ws_outbox_should_suppress(const ws_outbox_t *outbox,
                               size_t transport_bytes);
bool ws_outbox_needs_resync(const ws_outbox_t *outbox);
void ws_outbox_mark_resynced(ws_outbox_t *outbox);
const char *ws_outbox_close_reason_name(ws_outbox_close_reason_t reason);
size_t ws_outbox_resident_capacity_bytes(void);

/* Private authoritative state wins coincident replication deadlines so a
 * bulk snapshot cannot cross the pressure watermark and starve it first. */
size_t ws_replication_cycle_order(
    bool private_due,
    bool bulk_due,
    ws_replication_cycle_t order[2]);
bool ws_backpressure_fixture_enabled(const char *value);

void ws_sync_pacer_init(ws_sync_pacer_t *pacer, uint64_t now_ms);
bool ws_sync_pacer_can_send(ws_sync_pacer_t *pacer,
                            uint64_t now_ms,
                            size_t wire_bytes);
bool ws_sync_pacer_charge(ws_sync_pacer_t *pacer,
                          uint64_t now_ms,
                          size_t wire_bytes);
bool ws_sync_pacer_allow(ws_sync_pacer_t *pacer,
                         uint64_t now_ms,
                         size_t wire_bytes);

void ws_initial_sync_begin(ws_initial_sync_t *sync,
                           uint64_t now_ms,
                           uint16_t station_count);
ws_initial_sync_step_t ws_initial_sync_current(
    const ws_initial_sync_t *sync,
    uint16_t *station_index);
uint8_t ws_initial_sync_substep(const ws_initial_sync_t *sync);
void ws_initial_sync_commit_substep(ws_initial_sync_t *sync,
                                    uint8_t substep_count,
                                    uint64_t now_ms);
void ws_initial_sync_skip_station(ws_initial_sync_t *sync);
void ws_initial_sync_commit(ws_initial_sync_t *sync, uint64_t now_ms);
bool ws_initial_sync_active(const ws_initial_sync_t *sync);
bool ws_initial_sync_take_reconcile(ws_initial_sync_t *sync);
bool ws_initial_sync_catchup_needs_snapshot(
    const ws_initial_sync_t *sync);
void ws_initial_sync_catchup_note_snapshot(
    ws_initial_sync_t *sync,
    uint64_t suppressed_packets,
    uint64_t snapshot_generation);
bool ws_initial_sync_catchup_snapshot_current(
    ws_initial_sync_t *sync,
    uint64_t suppressed_packets,
    uint64_t snapshot_generation);

#endif
