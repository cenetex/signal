/*
 * test_chain_log.c -- Layer C of #479: per-station signed event chain log.
 *
 * Covers emission + verifier round-trip, chain linkage, tamper
 * detection, wrong-station signature rejection, save/load continuity,
 * cross-station independence, and end-to-end integration with the
 * smelt and rock-fracture sim paths.
 */
#include "test_harness.h"

#include "chain_log.h"
#include "cargo_legacy_inventory.h"
#include "cargo_receipt_trust.h"
#include "station_authority.h"
#include "sim_asteroid.h"
#include "sim_production.h"
#include "game_sim.h"
#include "sha256.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#  include <direct.h>
#endif

/* Each test sets a unique chain dir under TMP() so concurrent test
 * shards don't trample each other and so a previous run's residue
 * doesn't poison the current pass. */
static void chain_test_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_chain_%s", TMP("clog"), suffix);
    chain_log_test_fault_clear();
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(path);
}

static void chain_test_teardown(void) {
    chain_log_test_fault_clear();
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(NULL);
}

static bool chain_test_make_dir(const char *path) {
#if defined(_WIN32)
    int result = _mkdir(path);
#else
    int result = mkdir(path, 0700);
#endif
    return result == 0 || errno == EEXIST;
}

static bool chain_test_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool chain_test_read_log(const station_t *station,
                                uint8_t *out,
                                size_t cap,
                                size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!station || !out || cap == 0 || !out_len) return false;
    char path[256];
    if (!chain_log_path_for(station->station_pubkey, path, sizeof(path)))
        return false;
    FILE *log = fopen(path, "rb");
    if (!log) return false;
    bool ok = fseek(log, 0, SEEK_END) == 0;
    long end = ok ? ftell(log) : -1;
    if (end < 0 || (size_t)end > cap) ok = false;
    if (ok && fseek(log, 0, SEEK_SET) != 0) ok = false;
    if (ok && end > 0 &&
        fread(out, 1, (size_t)end, log) != (size_t)end) {
        ok = false;
    }
    if (fclose(log) != 0) ok = false;
    if (ok) *out_len = (size_t)end;
    return ok;
}

/* Iterate the seeded stations and remove their chain log files for
 * the currently-configured dir. Cheap; the dir is a per-test unique
 * tmp path and we don't care about non-station files in there. */
static void chain_test_wipe_logs(world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w->stations[s]);
}

TEST(test_chain_log_emit_and_verify) {
    chain_test_setup("emit_verify");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9001u;
    world_reset(w);
    chain_test_wipe_logs(w);
    /* world_reset zero-ed chain state; the wipe also flushed any
     * pre-existing on-disk logs from previous test runs. */
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t payload[] = "smelt-payload";
    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                                 payload, sizeof(payload));
    ASSERT(id == 1);
    ASSERT_EQ_INT((int)w->stations[0].chain_event_count, 1);

    uint64_t walked = 0;
    uint8_t last_hash[32];
    bool ok = chain_log_verify(&w->stations[0], &walked, last_hash);
    ASSERT(ok);
    ASSERT_EQ_INT((int)walked, 1);
    ASSERT(memcmp(last_hash, w->stations[0].chain_last_hash, 32) == 0);

    chain_test_teardown();
}

TEST(test_chain_log_batch_commits_contiguous_verified_events) {
    chain_test_setup("batch_commit");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9021u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    const uint8_t smelt[] = "batch-smelt";
    const uint8_t transfer[] = "batch-transfer";
    const uint8_t trade[] = "batch-trade";
    const chain_log_batch_event_t events[] = {
        {
            .type = CHAIN_EVT_SMELT,
            .payload = smelt,
            .payload_len = sizeof(smelt),
        },
        {
            .type = CHAIN_EVT_TRANSFER,
            .payload = transfer,
            .payload_len = sizeof(transfer),
        },
        {
            .type = CHAIN_EVT_TRADE,
            .payload = trade,
            .payload_len = sizeof(trade),
        },
    };

    chain_log_append_result_t appended = chain_log_emit_batch(
        w, station, events, sizeof(events) / sizeof(events[0]));
    ASSERT_EQ_INT(appended.status, CHAIN_LOG_APPEND_OK);
    ASSERT(strcmp(chain_log_append_status_name(appended.status), "ok") == 0);
    ASSERT_EQ_INT(appended.event_count, 3);
    ASSERT(appended.first_event_id == 1);
    ASSERT(appended.last_event_id == 3);
    ASSERT(station->chain_event_count == 3);
    ASSERT(memcmp(appended.last_hash, station->chain_last_hash, 32) == 0);

    chain_log_verify_report_t report;
    ASSERT(chain_log_verify_station(station, NULL, NULL, &report));
    ASSERT(report.total_events == 3);
    ASSERT(report.event_type_counts[CHAIN_EVT_SMELT] == 1);
    ASSERT(report.event_type_counts[CHAIN_EVT_TRANSFER] == 1);
    ASSERT(report.event_type_counts[CHAIN_EVT_TRADE] == 1);

    const uint8_t wrapper_payload[] = "wrapper";
    ASSERT(chain_log_emit(w, station, CHAIN_EVT_LEDGER,
                          wrapper_payload,
                          sizeof(wrapper_payload)) == 4);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == 4);
    chain_test_teardown();
}

TEST(test_chain_log_batch_prevalidates_before_any_commit) {
    chain_test_setup("batch_prevalidate");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9022u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    const uint8_t payload[] = "valid";
    const chain_log_batch_event_t invalid[] = {
        {
            .type = CHAIN_EVT_SMELT,
            .payload = payload,
            .payload_len = sizeof(payload),
        },
        {
            .type = CHAIN_EVT_CRAFT,
            .payload = NULL,
            .payload_len = 1,
        },
    };
    uint8_t before_hash[32];
    memcpy(before_hash, station->chain_last_hash, sizeof(before_hash));

    chain_log_append_result_t rejected = chain_log_emit_batch(
        w, station, invalid, sizeof(invalid) / sizeof(invalid[0]));
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_BAD_ARGUMENTS);
    ASSERT_EQ_INT(rejected.event_count, 0);
    ASSERT(rejected.first_event_id == 0);
    ASSERT(rejected.last_event_id == 0);
    ASSERT(station->chain_event_count == 0);
    ASSERT(memcmp(station->chain_last_hash, before_hash, 32) == 0);
    ASSERT(!station->chain_append_blocked);

    chain_log_append_result_t too_large = chain_log_emit_batch(
        w, station, invalid, CHAIN_LOG_BATCH_MAX_EVENTS + 1u);
    ASSERT_EQ_INT(too_large.status, CHAIN_LOG_APPEND_BATCH_TOO_LARGE);
    ASSERT(station->chain_event_count == 0);

    char path[256];
    ASSERT(chain_log_path_for(station->station_pubkey,
                              path, sizeof(path)));
    FILE *log = fopen(path, "rb");
    ASSERT(log == NULL);
    chain_test_teardown();
}

TEST(test_chain_log_batch_disk_disabled_commits_atomically_in_memory) {
    chain_test_setup("batch_memory_only");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9026u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);
    chain_log_set_disk_enabled(false);

    const uint8_t smelt[] = "memory-smelt";
    const uint8_t transfer[] = "memory-transfer";
    const uint8_t trade[] = "memory-trade";
    const chain_log_batch_event_t events[] = {
        { CHAIN_EVT_SMELT, smelt, sizeof(smelt) },
        { CHAIN_EVT_TRANSFER, transfer, sizeof(transfer) },
        { CHAIN_EVT_TRADE, trade, sizeof(trade) },
    };
    chain_log_append_result_t appended = chain_log_emit_batch(
        w, station, events, sizeof(events) / sizeof(events[0]));
    ASSERT_EQ_INT(appended.status, CHAIN_LOG_APPEND_OK);
    ASSERT_EQ_INT(appended.event_count, 3);
    ASSERT(appended.first_event_id == 1);
    ASSERT(appended.last_event_id == 3);
    ASSERT(station->chain_event_count == 3);
    ASSERT(memcmp(station->chain_last_hash, appended.last_hash, 32) == 0);

    uint64_t committed_count = station->chain_event_count;
    uint8_t committed_hash[32];
    memcpy(committed_hash, station->chain_last_hash, sizeof(committed_hash));
    const chain_log_batch_event_t invalid[] = {
        { CHAIN_EVT_CRAFT, smelt, sizeof(smelt) },
        { CHAIN_EVT_TRANSFER, NULL, 1 },
    };
    chain_log_append_result_t rejected = chain_log_emit_batch(
        w, station, invalid, sizeof(invalid) / sizeof(invalid[0]));
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_BAD_ARGUMENTS);
    ASSERT_EQ_INT(rejected.event_count, 0);
    ASSERT(station->chain_event_count == committed_count);
    ASSERT(memcmp(station->chain_last_hash, committed_hash, 32) == 0);

    char path[256];
    ASSERT(chain_log_path_for(station->station_pubkey,
                              path, sizeof(path)));
    FILE *log = fopen(path, "rb");
    ASSERT(log == NULL);
    chain_test_teardown();
}

TEST(test_chain_log_batch_event_id_overflow_is_inert) {
    chain_test_setup("batch_event_id_overflow");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9027u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = UINT64_MAX - 1u;
    memset(station->chain_last_hash, 0xa5, 32);
    uint8_t before_hash[32];
    memcpy(before_hash, station->chain_last_hash, sizeof(before_hash));

    const uint8_t transfer[] = "overflow-transfer";
    const uint8_t trade[] = "overflow-trade";
    const chain_log_batch_event_t events[] = {
        { CHAIN_EVT_TRANSFER, transfer, sizeof(transfer) },
        { CHAIN_EVT_TRADE, trade, sizeof(trade) },
    };
    chain_log_append_result_t rejected = chain_log_emit_batch(
        w, station, events, sizeof(events) / sizeof(events[0]));
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_EVENT_ID_OVERFLOW);
    ASSERT_EQ_INT(rejected.event_count, 0);
    ASSERT(rejected.first_event_id == 0);
    ASSERT(rejected.last_event_id == 0);
    const uint8_t zero_hash[32] = {0};
    ASSERT(memcmp(rejected.last_hash, zero_hash,
                  sizeof(rejected.last_hash)) == 0);
    ASSERT(station->chain_event_count == UINT64_MAX - 1u);
    ASSERT(memcmp(station->chain_last_hash, before_hash, 32) == 0);
    ASSERT(!station->chain_append_blocked);

    char path[256];
    ASSERT(chain_log_path_for(station->station_pubkey,
                              path, sizeof(path)));
    FILE *log = fopen(path, "rb");
    ASSERT(log == NULL);
    chain_test_teardown();
}

TEST(test_chain_log_batch_partial_write_fault_rolls_back_exact_bytes) {
    chain_test_setup("batch_write_rollback");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9023u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    const uint8_t baseline_payload[] = "baseline";
    ASSERT(chain_log_emit(w, station, CHAIN_EVT_LEDGER,
                          baseline_payload,
                          sizeof(baseline_payload)) == 1);
    uint64_t before_count = station->chain_event_count;
    uint8_t before_hash[32];
    memcpy(before_hash, station->chain_last_hash, sizeof(before_hash));
    uint8_t before_bytes[2048];
    size_t before_len = 0;
    ASSERT(chain_test_read_log(station, before_bytes,
                               sizeof(before_bytes), &before_len));

    const uint8_t smelt[] = "smelt";
    const uint8_t craft_a[] = "craft-a";
    const uint8_t transfer[] = "transfer";
    const uint8_t craft_b[] = "craft-b";
    const chain_log_batch_event_t events[] = {
        { CHAIN_EVT_SMELT, smelt, sizeof(smelt) },
        { CHAIN_EVT_CRAFT, craft_a, sizeof(craft_a) },
        { CHAIN_EVT_TRANSFER, transfer, sizeof(transfer) },
        { CHAIN_EVT_CRAFT, craft_b, sizeof(craft_b) },
    };
    /* Fail immediately before the second CRAFT. The one physical write
     * therefore contains three complete staged entries and exercises a
     * deterministic partial-batch truncate. */
    chain_log_test_fault_inject(CHAIN_LOG_TEST_FAULT_WRITE,
                                CHAIN_EVT_CRAFT, 2);
    chain_log_append_result_t rejected = chain_log_emit_batch(
        w, station, events, sizeof(events) / sizeof(events[0]));
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_WRITE_FAILED);
    ASSERT(strcmp(chain_log_append_status_name(rejected.status),
                  "write_failed") == 0);
    ASSERT_EQ_INT(rejected.event_count, 0);
    ASSERT(station->chain_event_count == before_count);
    ASSERT(memcmp(station->chain_last_hash, before_hash, 32) == 0);
    ASSERT(station->chain_append_blocked);
    ASSERT_EQ_INT(station->chain_health_status, CHAIN_HEALTH_FAILED);
    ASSERT(strstr(station->chain_health_message, "write_failed") != NULL);

    uint8_t after_bytes[2048];
    size_t after_len = 0;
    ASSERT(chain_test_read_log(station, after_bytes,
                               sizeof(after_bytes), &after_len));
    ASSERT(after_len == before_len);
    ASSERT(memcmp(after_bytes, before_bytes, before_len) == 0);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == before_count);

    /* The injection is one-shot. Once an operator/test explicitly clears the
     * fail-closed health latch, the ordinary single-event wrapper continues
     * from the exact pre-fault hash. */
    chain_log_health_set(station, CHAIN_HEALTH_OK, false,
                         before_count, before_hash, NULL);
    ASSERT(chain_log_emit(w, station, CHAIN_EVT_LEDGER,
                          baseline_payload,
                          sizeof(baseline_payload)) == before_count + 1u);
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == before_count + 1u);
    chain_test_teardown();
}

TEST(test_chain_log_batch_flush_fault_rolls_back_exact_bytes) {
    chain_test_setup("batch_flush_rollback");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9024u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    const uint8_t baseline_payload[] = "baseline";
    ASSERT(chain_log_emit(w, station, CHAIN_EVT_LEDGER,
                          baseline_payload,
                          sizeof(baseline_payload)) == 1);
    uint64_t before_count = station->chain_event_count;
    uint8_t before_hash[32];
    memcpy(before_hash, station->chain_last_hash, sizeof(before_hash));
    uint8_t before_bytes[2048];
    size_t before_len = 0;
    ASSERT(chain_test_read_log(station, before_bytes,
                               sizeof(before_bytes), &before_len));

    const uint8_t craft_a[] = "craft-a";
    const uint8_t craft_b[] = "craft-b";
    const chain_log_batch_event_t events[] = {
        { CHAIN_EVT_CRAFT, craft_a, sizeof(craft_a) },
        { CHAIN_EVT_CRAFT, craft_b, sizeof(craft_b) },
    };
    chain_log_test_fault_inject(CHAIN_LOG_TEST_FAULT_FLUSH,
                                CHAIN_EVT_CRAFT, 1);
    chain_log_append_result_t rejected = chain_log_emit_batch(
        w, station, events, sizeof(events) / sizeof(events[0]));
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_FLUSH_FAILED);
    ASSERT(station->chain_event_count == before_count);
    ASSERT(memcmp(station->chain_last_hash, before_hash, 32) == 0);
    ASSERT(station->chain_append_blocked);

    uint8_t after_bytes[2048];
    size_t after_len = 0;
    ASSERT(chain_test_read_log(station, after_bytes,
                               sizeof(after_bytes), &after_len));
    ASSERT(after_len == before_len);
    ASSERT(memcmp(after_bytes, before_bytes, before_len) == 0);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == before_count);
    chain_test_teardown();
}

TEST(test_chain_log_batch_close_fault_rolls_back_exact_bytes) {
    chain_test_setup("batch_close_rollback");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9025u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    const uint8_t baseline_payload[] = "baseline";
    ASSERT(chain_log_emit(w, station, CHAIN_EVT_LEDGER,
                          baseline_payload,
                          sizeof(baseline_payload)) == 1);
    uint64_t before_count = station->chain_event_count;
    uint8_t before_hash[32];
    memcpy(before_hash, station->chain_last_hash, sizeof(before_hash));
    uint8_t before_bytes[2048];
    size_t before_len = 0;
    ASSERT(chain_test_read_log(station, before_bytes,
                               sizeof(before_bytes), &before_len));

    const uint8_t transfer[] = "transfer";
    const uint8_t trade[] = "trade";
    const chain_log_batch_event_t events[] = {
        { CHAIN_EVT_TRANSFER, transfer, sizeof(transfer) },
        { CHAIN_EVT_TRADE, trade, sizeof(trade) },
    };
    chain_log_test_fault_inject(CHAIN_LOG_TEST_FAULT_CLOSE,
                                CHAIN_EVT_TRADE, 1);
    chain_log_append_result_t rejected = chain_log_emit_batch(
        w, station, events, sizeof(events) / sizeof(events[0]));
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_CLOSE_FAILED);
    ASSERT(station->chain_event_count == before_count);
    ASSERT(memcmp(station->chain_last_hash, before_hash, 32) == 0);
    ASSERT(station->chain_append_blocked);

    uint8_t after_bytes[2048];
    size_t after_len = 0;
    ASSERT(chain_test_read_log(station, after_bytes,
                               sizeof(after_bytes), &after_len));
    ASSERT(after_len == before_len);
    ASSERT(memcmp(after_bytes, before_bytes, before_len) == 0);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == before_count);
    chain_test_teardown();
}

TEST(test_chain_log_first_append_dir_sync_fault_removes_new_file) {
    chain_test_setup("first_append_dir_sync_rollback");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9026u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    char path[256];
    ASSERT(chain_log_path_for(
        station->station_pubkey, path, sizeof(path)));
    FILE *before = fopen(path, "rb");
    ASSERT(before == NULL);

    const uint8_t payload[] = "first-durable-event";
    const chain_log_batch_event_t event = {
        CHAIN_EVT_CRAFT, payload, sizeof(payload),
    };
    chain_log_test_fault_inject(
        CHAIN_LOG_TEST_FAULT_DIR_SYNC, CHAIN_EVT_CRAFT, 1);
    chain_log_append_result_t rejected =
        chain_log_emit_batch(w, station, &event, 1);
    ASSERT_EQ_INT(
        rejected.status, CHAIN_LOG_APPEND_DIR_SYNC_FAILED);
    ASSERT(strcmp(chain_log_append_status_name(rejected.status),
                  "dir_sync_failed") == 0);
    ASSERT(station->chain_event_count == 0);
    uint8_t zero_hash[32] = {0};
    ASSERT(memcmp(station->chain_last_hash, zero_hash, 32) == 0);
    ASSERT(station->chain_append_blocked);

    FILE *after = fopen(path, "rb");
    ASSERT(after == NULL);
    uint64_t walked = 99;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == 0);
    chain_test_teardown();
}

TEST(test_chain_log_first_boot_parent_sync_fault_removes_new_tree) {
    char parent[256];
    char chain_dir[256];
    snprintf(parent, sizeof(parent), "%s_parent_sync_fault",
             TMP("clog"));
    snprintf(chain_dir, sizeof(chain_dir), "%s/chain", parent);
    ASSERT(chain_test_make_dir(parent));
    ASSERT(!chain_test_path_exists(chain_dir));
    chain_log_test_fault_clear();
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(chain_dir);

    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9028u;
    world_reset(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    char path[256];
    ASSERT(chain_log_path_for(
        station->station_pubkey, path, sizeof(path)));
    const uint8_t payload[] = "first-boot-parent-sync";
    const chain_log_batch_event_t event = {
        CHAIN_EVT_CRAFT, payload, sizeof(payload),
    };
    chain_log_test_fault_inject(
        CHAIN_LOG_TEST_FAULT_PARENT_DIR_SYNC,
        CHAIN_EVT_CRAFT, 1);
    chain_log_append_result_t rejected =
        chain_log_emit_batch(w, station, &event, 1);
    ASSERT_EQ_INT(
        rejected.status, CHAIN_LOG_APPEND_DIR_SYNC_FAILED);
    ASSERT(station->chain_event_count == 0);
    uint8_t zero_hash[32] = {0};
    ASSERT(memcmp(station->chain_last_hash, zero_hash, 32) == 0);
    ASSERT(station->chain_append_blocked);
    ASSERT(!chain_test_path_exists(path));
    ASSERT(!chain_test_path_exists(chain_dir));
    chain_test_teardown();
}

TEST(test_chain_log_first_boot_syncs_nested_dir_once) {
    char parent[256];
    char chain_dir[256];
    snprintf(parent, sizeof(parent), "%s_parent_sync_success",
             TMP("clog"));
    snprintf(chain_dir, sizeof(chain_dir), "%s/chain", parent);
    ASSERT(chain_test_make_dir(parent));
    ASSERT(!chain_test_path_exists(chain_dir));
    chain_log_test_fault_clear();
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(chain_dir);

    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9029u;
    world_reset(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    const uint8_t payload[] = "first-boot-parent-success";
    ASSERT(chain_log_emit(
        w, station, CHAIN_EVT_LEDGER,
        payload, sizeof(payload)) == 1);
    ASSERT(chain_test_path_exists(chain_dir));

    /* Once the directory entry is durable, ordinary appends skip the parent
     * sync. A parent-sync-only fault therefore has no step at which to fire. */
    chain_log_test_fault_inject(
        CHAIN_LOG_TEST_FAULT_PARENT_DIR_SYNC,
        CHAIN_EVT_LEDGER, 1);
    ASSERT(chain_log_emit(
        w, station, CHAIN_EVT_LEDGER,
        payload, sizeof(payload)) == 2);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT(walked == 2);
    chain_test_teardown();
}

TEST(test_chain_log_position_faults_remove_new_log_and_directory) {
    chain_test_setup("position_fault_rollback");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9030u;
    world_reset(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);
    const uint8_t payload[] = "position-fault";
    const chain_log_batch_event_t event = {
        CHAIN_EVT_LEDGER, payload, sizeof(payload),
    };
    char path[256];
    ASSERT(chain_log_path_for(
        station->station_pubkey, path, sizeof(path)));

    chain_log_test_fault_inject(
        CHAIN_LOG_TEST_FAULT_SEEK, CHAIN_EVT_LEDGER, 1);
    chain_log_append_result_t rejected =
        chain_log_emit_batch(w, station, &event, 1);
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_SEEK_FAILED);
    ASSERT(station->chain_event_count == 0);
    ASSERT(station->chain_append_blocked);
    ASSERT(!chain_test_path_exists(path));
    ASSERT(!chain_test_path_exists(chain_log_get_dir()));

    uint8_t zero_hash[32] = {0};
    chain_log_health_set(
        station, CHAIN_HEALTH_OK, false, 0, zero_hash, NULL);
    chain_log_test_fault_inject(
        CHAIN_LOG_TEST_FAULT_TELL, CHAIN_EVT_LEDGER, 1);
    rejected = chain_log_emit_batch(w, station, &event, 1);
    ASSERT_EQ_INT(rejected.status, CHAIN_LOG_APPEND_TELL_FAILED);
    ASSERT(station->chain_event_count == 0);
    ASSERT(memcmp(station->chain_last_hash, zero_hash, 32) == 0);
    ASSERT(station->chain_append_blocked);
    ASSERT(!chain_test_path_exists(path));
    ASSERT(!chain_test_path_exists(chain_log_get_dir()));
    chain_test_teardown();
}

TEST(test_chain_log_chain_linkage) {
    chain_test_setup("linkage");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9002u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    for (int i = 0; i < 10; i++) {
        uint8_t payload[16];
        memset(payload, (uint8_t)i, sizeof(payload));
        uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                                     payload, sizeof(payload));
        ASSERT(id == (uint64_t)(i + 1));
    }
    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT_EQ_INT((int)walked, 10);
    chain_test_teardown();
}

TEST(test_chain_log_verify_accepts_clean_segment_reset) {
    chain_test_setup("segment_reset");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9020u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[] = "seg";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          pl, sizeof(pl)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          pl, sizeof(pl)) == 2);

    /* Simulate a fresh-world/session branch appended to the same
     * station file: the new segment starts from event_id=1 and
     * prev_hash=0, but the old bytes remain append-only on disk. */
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                          pl, sizeof(pl)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                          pl, sizeof(pl)) == 2);

    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    ASSERT(chain_log_verify_with_pubkey(f, w->stations[0].station_pubkey, &r));
    fclose(f);
    ASSERT_EQ_INT((int)r.total_events, 4);
    ASSERT_EQ_INT((int)r.valid_events, 4);
    ASSERT_EQ_INT((int)r.segment_count, 2);
    ASSERT_EQ_INT((int)r.segment_resets, 1);
    ASSERT_EQ_INT((int)r.tail_event_id, 2);
    ASSERT_EQ_INT((int)r.tail_valid_events, 2);
    ASSERT_EQ_INT((int)r.bad_linkage, 0);
    ASSERT_EQ_INT((int)r.monotonic_violations, 0);

    uint64_t walked = 0;
    uint8_t last_hash[32];
    ASSERT(chain_log_verify(&w->stations[0], &walked, last_hash));
    ASSERT_EQ_INT((int)walked, 2);
    ASSERT(memcmp(last_hash, w->stations[0].chain_last_hash, 32) == 0);
    chain_test_teardown();
}

TEST(test_chain_log_tampered_event_detected) {
    chain_test_setup("tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9003u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    for (int i = 0; i < 5; i++) {
        uint8_t pl[8];
        memset(pl, (uint8_t)(i + 0x10), sizeof(pl));
        ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_TRANSFER,
                              pl, sizeof(pl)) == (uint64_t)(i + 1));
    }
    /* Locate the log on disk and flip a byte inside event 3's header. */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "r+b");
    ASSERT(f != NULL);
    /* Each entry on disk = 184 (header) + 2 (payload_len) + 8 (payload).
     * Byte 17 of event 3 lives at offset (2 * 194) + 17 = 405. */
    long entry_size = 184 + 2 + 8;
    long target_off = entry_size * 2 + 17;
    fseek(f, target_off, SEEK_SET);
    uint8_t b;
    ASSERT(fread(&b, 1, 1, f) == 1);
    fseek(f, target_off, SEEK_SET);
    b ^= 0xFFu;
    ASSERT(fwrite(&b, 1, 1, f) == 1);
    fclose(f);

    uint64_t walked = 0;
    bool ok = chain_log_verify(&w->stations[0], &walked, NULL);
    ASSERT(!ok);
    /* Verifier may abort at event 3 itself or at event 4 (depending on
     * which invariant the flipped byte breaks first). Either way it
     * must NOT have walked the full five. */
    ASSERT(walked < 5);
    chain_test_teardown();
}

TEST(test_chain_log_wrong_station_signature_rejected) {
    chain_test_setup("wrong_station");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9004u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t payload[] = "evt";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                          payload, sizeof(payload)) == 1);
    /* Rewrite the authority field with Helios's pubkey on disk. */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "r+b");
    ASSERT(f != NULL);
    /* authority field starts at byte 8+8+1+7 = 24 of the header. */
    fseek(f, 24, SEEK_SET);
    ASSERT(fwrite(w->stations[2].station_pubkey, 32, 1, f) == 1);
    fclose(f);
    /* The verifier checks the on-disk authority equals the station's
     * pubkey AND that the signature is valid. Either check fires. */
    ASSERT(!chain_log_verify(&w->stations[0], NULL, NULL));
    chain_test_teardown();
}

TEST(test_chain_log_save_load_continuity) {
    chain_test_setup("savecontinuity");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9005u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[] = "abc";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, sizeof(pl)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, sizeof(pl)) == 2);

    uint64_t saved_count = w->stations[0].chain_event_count;
    uint8_t saved_last[32];
    memcpy(saved_last, w->stations[0].chain_last_hash, 32);

    ASSERT(world_save(w, TMP("clog_continuity.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("clog_continuity.sav")));

    ASSERT_EQ_INT((int)loaded->stations[0].chain_event_count, (int)saved_count);
    ASSERT(memcmp(loaded->stations[0].chain_last_hash, saved_last, 32) == 0);
    ASSERT_EQ_INT((int)loaded->stations[0].chain_health_status,
                  (int)CHAIN_HEALTH_OK);
    ASSERT(!loaded->stations[0].chain_append_blocked);

    /* Emit one more — its prev_hash must equal saved_last. */
    uint8_t pl2[] = "def";
    uint64_t id3 = chain_log_emit(loaded, &loaded->stations[0], CHAIN_EVT_LEDGER,
                                  pl2, sizeof(pl2));
    ASSERT(id3 == saved_count + 1);
    /* Walking the on-disk log must succeed for all three events. */
    uint64_t walked = 0;
    ASSERT(chain_log_verify(&loaded->stations[0], &walked, NULL));
    ASSERT_EQ_INT((int)walked, (int)id3);
    remove(TMP("clog_continuity.sav"));
    chain_test_teardown();
}

TEST(test_chain_log_emit_blocked_by_failed_health) {
    chain_test_setup("health_block_direct");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9016u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    chain_log_health_set(&w->stations[0], CHAIN_HEALTH_FAILED, true,
                         0, NULL, "test verifier failure");
    uint8_t pl[] = "blocked";
    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                                 pl, sizeof(pl));
    ASSERT(id == 0);
    ASSERT_EQ_INT((int)w->stations[0].chain_event_count, 0);

    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "rb");
    ASSERT(f == NULL);
    chain_test_teardown();
}

TEST(test_chain_log_health_repair_hints_are_operator_facing) {
    const char *ok = chain_log_health_repair_hint(CHAIN_HEALTH_OK, false);
    const char *failed = chain_log_health_repair_hint(CHAIN_HEALTH_FAILED, true);
    const char *mismatch = chain_log_health_repair_hint(CHAIN_HEALTH_MISMATCH, true);
    const char *adopted = chain_log_health_repair_hint(CHAIN_HEALTH_ADOPTED, false);

    ASSERT(ok != NULL && strstr(ok, "No repair needed") != NULL);
    ASSERT(failed != NULL && strstr(failed, "Preserve") != NULL);
    ASSERT(failed != NULL && strstr(failed, "signal_verify") != NULL);
    ASSERT(mismatch != NULL && strstr(mismatch, "Restore") != NULL);
    ASSERT(mismatch != NULL && strstr(mismatch, "do not append") != NULL);
    ASSERT(adopted != NULL && strstr(adopted, "Save") != NULL);
}

TEST(test_world_load_blocks_chain_appends_after_failed_verify) {
    chain_test_setup("load_failed_blocks");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9017u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[] = "abc";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          pl, sizeof(pl)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          pl, sizeof(pl)) == 2);
    ASSERT(world_save(w, TMP("clog_failed_blocks.sav")));

    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "r+b");
    ASSERT(f != NULL);
    fseek(f, CHAIN_EVENT_HEADER_SIZE + 2, SEEK_SET);
    uint8_t b = 0;
    ASSERT(fread(&b, 1, 1, f) == 1);
    fseek(f, CHAIN_EVENT_HEADER_SIZE + 2, SEEK_SET);
    b ^= 0xFFu;
    ASSERT(fwrite(&b, 1, 1, f) == 1);
    fclose(f);

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("clog_failed_blocks.sav")));
    ASSERT_EQ_INT((int)loaded->stations[0].chain_health_status,
                  (int)CHAIN_HEALTH_FAILED);
    ASSERT(loaded->stations[0].chain_append_blocked);
    ASSERT(strstr(loaded->stations[0].chain_health_message,
                  "payload_hash mismatch") != NULL);

    uint64_t before = loaded->stations[0].chain_event_count;
    uint64_t id = chain_log_emit(loaded, &loaded->stations[0],
                                 CHAIN_EVT_LEDGER, pl, sizeof(pl));
    ASSERT(id == 0);
    ASSERT(loaded->stations[0].chain_event_count == before);
    remove(TMP("clog_failed_blocks.sav"));
    chain_test_teardown();
}

TEST(test_world_load_blocks_chain_appends_after_missing_tail) {
    chain_test_setup("load_mismatch_blocks");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9018u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[] = "abc";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          pl, sizeof(pl)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          pl, sizeof(pl)) == 2);
    ASSERT(world_save(w, TMP("clog_missing_tail.sav")));

    chain_log_reset(&w->stations[0]);

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("clog_missing_tail.sav")));
    ASSERT_EQ_INT((int)loaded->stations[0].chain_health_status,
                  (int)CHAIN_HEALTH_MISMATCH);
    ASSERT(loaded->stations[0].chain_append_blocked);
    ASSERT(strstr(loaded->stations[0].chain_health_message,
                  "continuation mismatch") != NULL);

    uint64_t before = loaded->stations[0].chain_event_count;
    uint64_t id = chain_log_emit(loaded, &loaded->stations[0],
                                 CHAIN_EVT_LEDGER, pl, sizeof(pl));
    ASSERT(id == 0);
    ASSERT(loaded->stations[0].chain_event_count == before);
    remove(TMP("clog_missing_tail.sav"));
    chain_test_teardown();
}

TEST(test_world_load_blocks_chain_appends_when_verified_tail_is_ahead) {
    chain_test_setup("load_ahead_blocks");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9019u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    uint8_t committed_payload[] = "committed";
    ASSERT(chain_log_emit(
               w, station, CHAIN_EVT_LEDGER,
               committed_payload, sizeof(committed_payload)) == 1);
    uint64_t saved_count = station->chain_event_count;
    uint8_t saved_last[32];
    memcpy(saved_last, station->chain_last_hash, sizeof(saved_last));
    ASSERT(world_save(w, TMP("clog_ahead_tail.sav")));

    /*
     * Model a process death after a second event became durable but before
     * its gameplay mutation and the next world generation were published.
     * The selected snapshot therefore knows only event 1 while the intact
     * on-disk chain verifies through event 2.
     */
    uint8_t unapplied_payload[] = "durable-but-unapplied";
    ASSERT(chain_log_emit(
               w, station, CHAIN_EVT_LEDGER,
               unapplied_payload, sizeof(unapplied_payload)) == 2);

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("clog_ahead_tail.sav")));
    station_t *recovered = &loaded->stations[0];
    ASSERT(recovered->chain_event_count == saved_count);
    ASSERT(memcmp(recovered->chain_last_hash,
                  saved_last, sizeof(saved_last)) == 0);
    ASSERT_EQ_INT((int)recovered->chain_health_status,
                  (int)CHAIN_HEALTH_MISMATCH);
    ASSERT(recovered->chain_append_blocked);
    ASSERT(recovered->chain_verified_event_count == 2);
    ASSERT(strstr(recovered->chain_health_message,
                  "verified tail ahead of snapshot") != NULL);
    ASSERT(strstr(recovered->chain_health_message,
                  "mutation not replayed") != NULL);

    uint64_t rejected = chain_log_emit(
        loaded, recovered, CHAIN_EVT_LEDGER,
        unapplied_payload, sizeof(unapplied_payload));
    ASSERT(rejected == 0);
    ASSERT(recovered->chain_event_count == saved_count);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(recovered, &walked, NULL));
    ASSERT(walked == 2);
    remove(TMP("clog_ahead_tail.sav"));
    chain_test_teardown();
}

TEST(test_chain_log_cross_station_independent) {
    chain_test_setup("cross");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9006u;
    world_reset(w);
    chain_test_wipe_logs(w);
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }

    /* Interleave emits across Prospect (0) and Helios (2). */
    uint8_t pl[] = "x";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, 1) == 1);
    ASSERT(chain_log_emit(w, &w->stations[2], CHAIN_EVT_LEDGER, pl, 1) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, 1) == 2);
    ASSERT(chain_log_emit(w, &w->stations[2], CHAIN_EVT_LEDGER, pl, 1) == 2);

    uint64_t walked0 = 0, walked2 = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked0, NULL));
    ASSERT(chain_log_verify(&w->stations[2], &walked2, NULL));
    ASSERT_EQ_INT((int)walked0, 2);
    ASSERT_EQ_INT((int)walked2, 2);
    /* The two stations' chain heads must be different (different keys,
     * different events). */
    ASSERT(memcmp(w->stations[0].chain_last_hash,
                  w->stations[2].chain_last_hash, 32) != 0);
    chain_test_teardown();
}

TEST(test_chain_log_hopper_smelt_path_retired) {
    /* Raw ore floats are preserved for old saves and pricing fixtures,
     * but they are no longer a smelt source. Ingot lineage must enter
     * through the physical fragment path, where fragment_pub is known. */
    chain_test_setup("hopper_retired");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9007u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    int manifest_before = w->stations[0].manifest.count;
    float ingots_before = station_inventory_amount(
        &w->stations[0], COMMODITY_FERRITE_INGOT);
    w->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 5.0f;

    sim_step_refinery_production(w, 30.0f);

    ASSERT_EQ_FLOAT(w->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE],
                    5.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w->stations[0], COMMODITY_FERRITE_INGOT),
                    ingots_before, 0.001f);
    ASSERT_EQ_INT(w->stations[0].manifest.count, manifest_before);
    ASSERT_EQ_INT((int)w->stations[0].chain_event_count, 0);
    ASSERT_EQ_INT((int)w->hopper_smelt_events, 0);
    ASSERT(w->hopper_smelt_units == 0.0);
    chain_test_teardown();
}

TEST(test_chain_log_smelt_emits_event_fragment_path) {
    /* The richer smelt path: spawn a physical fragment between a
     * furnace and an adjacent module, run the sim until the beam
     * smelts it, then verify the chain log gained an EVT_SMELT whose
     * fragment_pub matches the consumed asteroid's record. */
    chain_test_setup("smelt_fragment");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9100u;
    world_reset(w);
    chain_test_wipe_logs(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    /* Find Prospect's furnace + the FERRITE_ORE intake hopper. The
     * ingot output hopper added in the cargo-in-space schema work is
     * not the smelt anchor — pick the input hopper specifically. */
    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w->stations[0].module_count; m++) {
        if (w->stations[0].modules[m].type == MODULE_FURNACE) furnace_idx = m;
        if (w->stations[0].modules[m].type == MODULE_HOPPER &&
            w->stations[0].modules[m].commodity == (uint8_t)COMMODITY_FERRITE_ORE)
            silo_idx = m;
    }
    ASSERT(furnace_idx >= 0 && silo_idx >= 0);

    vec2 furnace_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[furnace_idx].ring,
        w->stations[0].modules[furnace_idx].slot);
    vec2 silo_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[silo_idx].ring,
        w->stations[0].modules[silo_idx].slot);
    vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);

    /* Place a fragment exactly on the midpoint. fracture_seed varies
     * so fragment_pub derivation produces a non-trivial value. */
    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 3.0f;
    a->max_ore = 3.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    for (int b = 0; b < 32; b++) a->fracture_seed[b] = (uint8_t)(b * 13 + 7);
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    a->pos = midpoint;
    a->vel = v2(0, 0);

    ASSERT(station_finished_mint(
        &w->stations[0], COMMODITY_FRAME, 1, NULL) == 1);
    cargo_unit_t *shell =
        &w->stations[0]
             .manifest.units[w->stations[0].manifest.count - 1u];
    chain_payload_craft_t shell_origin = {0};
    ASSERT(chain_payload_craft_bind_output(
        &shell_origin, NULL, 0, shell));
    ASSERT(chain_log_emit(
        w, &w->stations[0], CHAIN_EVT_CRAFT,
        &shell_origin, sizeof(shell_origin)) == 1);

    /* Run sim until the fragment smelts (smelt_progress accumulates
     * at ~0.5/s; cap at a generous 10 s of sim time). */
    for (int i = 0; i < 1200 && w->asteroids[slot].active; i++)
        world_sim_step(w, 1.0f / 120.0f);
    ASSERT(!w->asteroids[slot].active);

    /* Chain log must have gained EVT_SMELT events. */
    ASSERT(w->stations[0].chain_event_count >= 1);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT(walked == w->stations[0].chain_event_count);

    ASSERT_EQ_INT((int)w->hopper_smelt_events, 0);
    ASSERT(w->hopper_smelt_units == 0.0);

    /* Walk the on-disk log and confirm every EVT_SMELT carries a
     * non-zero fragment_pub. Zero-fragment smelt events were the retired
     * hopper-float compatibility behavior. */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey,
                              path, sizeof(path)));
    FILE *fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    bool saw_fragment_attributed = false;
    bool saw_zero_fragment_smelt = false;
    while (!feof(fp)) {
        chain_event_header_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) != 1) break;
        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, fp) != 1) break;
        if (hdr.type == CHAIN_EVT_SMELT && plen == sizeof(chain_payload_smelt_t)) {
            chain_payload_smelt_t pl;
            if (fread(&pl, sizeof(pl), 1, fp) != 1) break;
            uint8_t zero[32] = {0};
            if (memcmp(pl.fragment_pub, zero, 32) != 0) {
                saw_fragment_attributed = true;
            } else {
                saw_zero_fragment_smelt = true;
            }
        } else {
            fseek(fp, plen, SEEK_CUR);
        }
    }
    fclose(fp);
    ASSERT(saw_fragment_attributed);
    ASSERT(!saw_zero_fragment_smelt);

    chain_test_teardown();
}

TEST(test_chain_log_rock_destroy_emits_event) {
    chain_test_setup("rockdestroy");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9008u;
    world_reset(w);
    chain_test_wipe_logs(w);
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }
    /* Find an asteroid inside Prospect's signal range, fracture it,
     * then check that *some* station's chain log gained an event.
     * Materialize one explicitly so we don't depend on the pseudo-
     * random spawn of the field. */
    asteroid_t *a = NULL;
    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    /* Place at Prospect's location so the witness is unambiguous. */
    a->pos = w->stations[0].pos;
    a->radius = 30.0f;
    a->hp = a->max_hp = 100.0f;
    /* Stamp a non-zero rock_pub so mark_rock_destroyed records it. */
    for (int b = 0; b < 32; b++) a->rock_pub[b] = (uint8_t)(0x40 + b);

    fracture_asteroid(w, slot, v2(1.0f, 0.0f), -1);

    /* Prospect (station 0) is the closest station to its own pos, so
     * the witness picked by fracture_asteroid is index 0. */
    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT(walked >= 1);
    chain_test_teardown();
}

TEST(test_chain_log_claim_fragment_emits_event) {
    chain_test_setup("claim_fragment");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9009u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->radius = 8.0f;
    a->pos = w->stations[0].pos;
    for (int b = 0; b < 32; b++) a->fracture_seed[b] = (uint8_t)(0x50 + b);

    fracture_claim_state_t *claim = &w->fracture_claims[slot];
    fracture_claim_state_reset(claim);
    claim->active = true;
    claim->fracture_id = 44;
    claim->deadline_ms = 1;
    claim->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;
    w->time = 1.0;

    step_fracture_claims(w);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT(walked == 1);

    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey,
                              path, sizeof(path)));
    FILE *fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    bool saw_claim = false;
    chain_event_header_t hdr;
    while (fread(&hdr, sizeof(hdr), 1, fp) == 1) {
        uint16_t plen = 0;
        ASSERT(fread(&plen, sizeof(plen), 1, fp) == 1);
        if (hdr.type == CHAIN_EVT_CLAIM_FRAGMENT &&
            plen == sizeof(chain_payload_claim_fragment_t)) {
            chain_payload_claim_fragment_t pl;
            ASSERT(fread(&pl, sizeof(pl), 1, fp) == 1);
            uint8_t zero_pub[32] = {0};
            uint8_t expected_fragment[32];
            mining_fragment_pub_compute(a->fracture_seed, zero_pub,
                                        pl.burst_nonce, expected_fragment);
            ASSERT(memcmp(pl.fracture_seed, a->fracture_seed, 32) == 0);
            ASSERT(memcmp(pl.fragment_pub, expected_fragment, 32) == 0);
            ASSERT(memcmp(pl.fragment_pub, a->fragment_pub, 32) == 0);
            ASSERT(memcmp(pl.claimant_pubkey, zero_pub, 32) == 0);
            ASSERT_EQ_INT((int)pl.fracture_id, 44);
            ASSERT_EQ_INT((int)pl.burst_cap, FRACTURE_CHALLENGE_BURST_CAP);
            ASSERT_EQ_INT((int)pl.grade, (int)a->grade);
            ASSERT_EQ_INT((int)pl.asteroid_slot, slot);
            saw_claim = true;
        } else {
            fseek(fp, plen, SEEK_CUR);
        }
    }
    fclose(fp);
    ASSERT(saw_claim);

    chain_test_teardown();
}

TEST(test_chain_log_operator_post_emit) {
    chain_test_setup("operator_post_emit");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9100u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    /* Build operator post payload manually */
    const char *text = "Welcome to Prospect Refinery";
    size_t text_len = strlen(text);
    size_t payload_len = 38 + text_len;
    uint8_t *payload = calloc(1, payload_len);
    ASSERT(payload != NULL);

    payload[0] = 0;  /* kind=HAIL_MOTD */
    payload[1] = 0;  /* tier=0 */
    payload[2] = 1;  /* ref_id=1 (little-endian) */
    payload[3] = 0;
    sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
    payload[36] = (uint8_t)(text_len & 0xFF);
    payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
    memcpy(&payload[38], text, text_len);

    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                                  payload, (uint16_t)payload_len);
    ASSERT(id == 1);
    ASSERT_EQ_INT((int)w->stations[0].chain_event_count, 1);

    uint64_t walked = 0;
    uint8_t last_hash[32];
    bool ok = chain_log_verify(&w->stations[0], &walked, last_hash);
    ASSERT(ok);
    ASSERT_EQ_INT((int)walked, 1);
    ASSERT(memcmp(last_hash, w->stations[0].chain_last_hash, 32) == 0);

    free(payload);
    chain_test_teardown();
}

TEST(test_chain_log_operator_post_all_kinds) {
    chain_test_setup("operator_post_kinds");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9101u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    const char *texts[] = {
        "Hail message",
        "Contract flavor",
        "Rarity tier"
    };

    for (int kind = 0; kind < 3; kind++) {
        const char *text = texts[kind];
        size_t text_len = strlen(text);
        size_t payload_len = 38 + text_len;
        uint8_t *payload = calloc(1, payload_len);
        ASSERT(payload != NULL);

        payload[0] = (uint8_t)kind;
        payload[1] = (kind == 2) ? 1 : 0;  /* tier for RARITY_TIER */
        payload[2] = (uint8_t)(10 + kind);
        payload[3] = 0;
        sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
        payload[36] = (uint8_t)(text_len & 0xFF);
        payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
        memcpy(&payload[38], text, text_len);

        uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                                      payload, (uint16_t)payload_len);
        ASSERT(id == (uint64_t)(kind + 1));

        free(payload);
    }

    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT_EQ_INT((int)walked, 3);
    chain_test_teardown();
}

TEST(test_chain_log_operator_post_replay_determinism) {
    chain_test_setup("operator_post_replay");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9102u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    const char *text = "Replay test";
    size_t text_len = strlen(text);
    size_t payload_len = 38 + text_len;
    uint8_t *payload = calloc(1, payload_len);
    ASSERT(payload != NULL);

    payload[0] = 0;
    payload[1] = 0;
    payload[2] = 5;
    payload[3] = 0;
    sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
    payload[36] = (uint8_t)(text_len & 0xFF);
    payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
    memcpy(&payload[38], text, text_len);

    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                                  payload, (uint16_t)payload_len);
    ASSERT(id == 1);

    uint64_t saved_count = w->stations[0].chain_event_count;
    uint8_t saved_last_hash[32];
    memcpy(saved_last_hash, w->stations[0].chain_last_hash, 32);

    free(payload);

    /* Round-trip the world via save/load — this is the actual replay
     * condition (server restart). world_save/load preserves
     * chain_event_count and chain_last_hash; the on-disk .log file is
     * untouched and world_load verifies it before future appends. */
    ASSERT(world_save(w, TMP("clog_op_replay.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, TMP("clog_op_replay.sav")));

    ASSERT_EQ_INT((int)loaded->stations[0].chain_event_count, (int)saved_count);
    ASSERT(memcmp(loaded->stations[0].chain_last_hash, saved_last_hash, 32) == 0);

    /* Walk the on-disk log via the loaded station — should still see
     * the one operator-post event. */
    uint64_t walked = 0;
    uint8_t loaded_last_hash[32];
    ASSERT(chain_log_verify(&loaded->stations[0], &walked, loaded_last_hash));
    ASSERT_EQ_INT((int)walked, 1);
    ASSERT(memcmp(loaded_last_hash, saved_last_hash, 32) == 0);

    remove(TMP("clog_op_replay.sav"));
    chain_test_teardown();
}

TEST(test_chain_log_operator_post_text_tamper) {
    chain_test_setup("operator_post_tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9104u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    const char *text = "Do not tamper";
    size_t text_len = strlen(text);
    size_t payload_len = 38 + text_len;
    uint8_t *payload = calloc(1, payload_len);
    ASSERT(payload != NULL);

    payload[0] = 0;
    payload[1] = 0;
    payload[2] = 99;
    payload[3] = 0;
    sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
    payload[36] = (uint8_t)(text_len & 0xFF);
    payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
    memcpy(&payload[38], text, text_len);

    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                                  payload, (uint16_t)payload_len);
    ASSERT(id == 1);
    free(payload);

    /* Tamper with the text on disk */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "r+b");
    ASSERT(f != NULL);
    /* Flip a byte in the text part. Entry layout: 184 (header) + 2
     * (payload_len) + payload_len; payload starts at file offset
     * 184 + 2; the variable-length text begins 38 bytes into the payload. */
    fseek(f, 184 + 2 + 38 + 2, SEEK_SET);
    uint8_t b;
    ASSERT(fread(&b, 1, 1, f) == 1);
    fseek(f, -1, SEEK_CUR);
    b ^= 0xFF;
    ASSERT(fwrite(&b, 1, 1, f) == 1);
    fclose(f);

    /* Verification should fail because the payload_hash won't match */
    uint64_t walked = 0;
    bool ok = chain_log_verify(&w->stations[0], &walked, NULL);
    ASSERT(!ok);
    ASSERT(walked < 1);

    chain_test_teardown();
}

TEST(test_chain_log_route_history_tail_reader) {
    chain_test_setup("route_history_tail");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9093u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t unrelated[] = "not-route-history";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER,
                          unrelated, sizeof(unrelated)) == 1);

    for (int i = 0; i < 3; i++) {
        chain_payload_route_history_t payload = {0};
        payload.memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION;
        payload.origin_station = (uint8_t)(1 + i);
        payload.destination_station = 3;
        payload.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
        payload.action = (uint8_t)CONTRACT_TRACTOR;
        payload.confidence = (uint8_t)(210 + i);
        payload.salience = (uint8_t)(200 + i);
        payload.evidence_count = (uint16_t)(4 + i);
        payload.value_hint = (uint16_t)(100 + i);
        payload.observed_tick = (uint32_t)(77 + i);
        payload.subject_nonce = 9000u + (uint64_t)i;
        ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_ROUTE_HISTORY,
                              &payload, sizeof(payload)) == (uint64_t)(i + 2));
    }

    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT_EQ_INT((int)walked, 4);

    chain_route_history_tail_t tail[2];
    memset(tail, 0, sizeof(tail));
    int count = chain_log_read_route_history_tail(&w->stations[0], tail, 2);
    ASSERT_EQ_INT(count, 2);
    ASSERT_EQ_INT((int)tail[0].event_id, 3);
    ASSERT_EQ_INT((int)tail[1].event_id, 4);
    ASSERT_EQ_INT(tail[0].payload.origin_station, 2);
    ASSERT_EQ_INT(tail[1].payload.origin_station, 3);
    ASSERT_EQ_INT(tail[1].payload.evidence_count, 6);
    ASSERT_EQ_INT(tail[1].payload.value_hint, 102);

    chain_test_teardown();
}

TEST(test_chain_log_cargo_transform_reader) {
    chain_test_setup("cargo_transform_reader");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9094u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    chain_payload_smelt_t smelt = {0};
    for (int i = 0; i < 32; i++) {
        smelt.fragment_pub[i] = (uint8_t)(0x20 + i);
        smelt.ingot_pub[i] = (uint8_t)(0x60 + i);
    }
    smelt.prefix_class = (uint8_t)INGOT_PREFIX_F;
    smelt.mined_block = 4422;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) == 1);
    uint8_t smelt_hash[32];
    memcpy(smelt_hash, w->stations[0].chain_last_hash, sizeof(smelt_hash));

    chain_payload_craft_t craft = {0};
    craft.recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    craft.input_count = 1;
    memcpy(craft.input_pubs[0], smelt.ingot_pub, 32);
    for (int i = 0; i < 32; i++)
        craft.output_pub[i] = (uint8_t)(0xA0 + i);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_CRAFT,
                          &craft, sizeof(craft)) == 2);
    uint8_t craft_hash[32];
    memcpy(craft_hash, w->stations[0].chain_last_hash, sizeof(craft_hash));

    chain_cargo_transform_t found = {0};
    ASSERT(chain_log_find_cargo_transform(&w->stations[0],
                                          craft.output_pub, &found));
    ASSERT_EQ_INT(found.type, CHAIN_EVT_CRAFT);
    ASSERT_EQ_INT((int)found.event_id, 2);
    ASSERT(memcmp(found.header_hash, craft_hash, sizeof(craft_hash)) == 0);
    ASSERT(memcmp(found.authority, w->stations[0].station_pubkey, 32) == 0);
    ASSERT_EQ_INT(found.craft.recipe_id, RECIPE_FRAME_BASIC);
    ASSERT(memcmp(found.craft.input_pubs[0], smelt.ingot_pub, 32) == 0);

    memset(&found, 0, sizeof(found));
    ASSERT(chain_log_find_cargo_transform(&w->stations[0],
                                          smelt.ingot_pub, &found));
    ASSERT_EQ_INT(found.type, CHAIN_EVT_SMELT);
    ASSERT_EQ_INT((int)found.event_id, 1);
    ASSERT(memcmp(found.header_hash, smelt_hash, sizeof(smelt_hash)) == 0);
    ASSERT(memcmp(found.authority, w->stations[0].station_pubkey, 32) == 0);
    ASSERT_EQ_INT((int)found.smelt.mined_block, 4422);
    ASSERT(memcmp(found.smelt.fragment_pub, smelt.fragment_pub, 32) == 0);

    uint8_t unknown[32] = {0xFF};
    ASSERT(!chain_log_find_cargo_transform(&w->stations[0], unknown, &found));

    chain_test_teardown();
}

typedef struct {
    int count;
    chain_cargo_transform_t transform;
} chain_snapshot_visit_capture_t;

static bool chain_snapshot_capture_transform(
    const chain_cargo_transform_t *transform,
    void *user) {
    chain_snapshot_visit_capture_t *capture =
        (chain_snapshot_visit_capture_t *)user;
    if (!transform || !capture) return false;
    capture->count++;
    capture->transform = *transform;
    return true;
}

TEST(test_chain_log_evidence_snapshot_freezes_verified_visit_bytes) {
    chain_test_setup("evidence_snapshot_exact");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9095u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    uint8_t fragment[32];
    for (int i = 0; i < 32; i++)
        fragment[i] = (uint8_t)(0x35 + i);
    cargo_unit_t ingot = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT,
        MINING_GRADE_FINE,
        fragment, 4u, &ingot));
    ingot.mined_block = 9095u;
    chain_payload_smelt_t payload = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &payload, fragment, 4u, &ingot));
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_SMELT,
            &payload, (uint16_t)sizeof(payload)),
        1);

    char path[256];
    ASSERT(chain_log_path_for(
        station->station_pubkey, path, sizeof(path)));
    FILE *source = fopen(path, "r+b");
    ASSERT(source != NULL);
    FILE *snapshot = NULL;
    ASSERT(chain_log_snapshot_evidence_file(
        source, &snapshot));
    ASSERT(snapshot != NULL);

    chain_log_verify_report_t report = {0};
    ASSERT(chain_log_verify_with_pubkey(
        snapshot, station->station_pubkey, &report));
    ASSERT_EQ_INT(report.valid_events, 1);

    /*
     * Corrupt the still-open path-backed descriptor after the snapshot was
     * taken. The verified anonymous snapshot must continue to expose the
     * exact original transform bytes on its interpretation pass.
     */
    long ingot_byte =
        (long)CHAIN_EVENT_HEADER_SIZE + 2L + 32L;
    ASSERT(fseek(source, ingot_byte, SEEK_SET) == 0);
    ASSERT(fputc((int)(payload.ingot_pub[0] ^ 0x80u),
                 source) != EOF);
    ASSERT(fflush(source) == 0);

    chain_snapshot_visit_capture_t capture = {0};
    size_t transform_count = 0;
    uint8_t visited_last_hash[32] = {0};
    ASSERT(chain_log_visit_cargo_transforms_from_verified_file(
        snapshot, report.valid_events,
        chain_snapshot_capture_transform, &capture,
        &transform_count, visited_last_hash));
    ASSERT_EQ_INT(transform_count, 1);
    ASSERT_EQ_INT(capture.count, 1);
    ASSERT_EQ_INT(
        capture.transform.output_semantics_version,
        CHAIN_CARGO_SEMANTICS_V1);
    ASSERT(memcmp(
        capture.transform.output_cargo.pub,
        ingot.pub, 32) == 0);
    ASSERT(memcmp(
        visited_last_hash,
        station->chain_last_hash, 32) == 0);
    ASSERT(fclose(snapshot) == 0);
    ASSERT(fclose(source) == 0);
    chain_test_teardown();
}

static void assert_origin_metadata_rejected(
    const world_t *w,
    int station_idx,
    const cargo_unit_t *unit) {
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, station_idx, unit, NULL);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(
        evaluated.origin_status,
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(
        evaluated.trust.status,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA);
    ASSERT_EQ_INT(
        evaluated.legality.status,
        CARGO_LEGALITY_CONTRABAND);
}

static void assert_each_cargo_metadata_tamper_rejected(
    const world_t *w,
    int station_idx,
    const cargo_unit_t *original) {
    for (int field = 0; field < 9; field++) {
        cargo_unit_t tampered = *original;
        switch (field) {
            case 0:
                tampered.kind =
                    original->kind == (uint8_t)CARGO_KIND_FRAME
                        ? (uint8_t)CARGO_KIND_LASER
                        : (uint8_t)CARGO_KIND_FRAME;
                break;
            case 1:
                tampered.commodity =
                    original->commodity ==
                            (uint8_t)COMMODITY_CUPRITE_INGOT
                        ? (uint8_t)COMMODITY_FERRITE_INGOT
                        : (uint8_t)COMMODITY_CUPRITE_INGOT;
                break;
            case 2:
                tampered.grade =
                    original->grade ==
                            (uint8_t)MINING_GRADE_FINE
                        ? (uint8_t)MINING_GRADE_RARE
                        : (uint8_t)MINING_GRADE_FINE;
                break;
            case 3:
                tampered.prefix_class =
                    original->prefix_class ==
                            (uint8_t)INGOT_PREFIX_M
                        ? (uint8_t)INGOT_PREFIX_H
                        : (uint8_t)INGOT_PREFIX_M;
                break;
            case 4:
                tampered.recipe_id =
                    original->recipe_id ==
                            (uint16_t)RECIPE_FRAME_BASIC
                        ? (uint16_t)RECIPE_LASER_BASIC
                        : (uint16_t)RECIPE_FRAME_BASIC;
                break;
            case 5:
                tampered.origin_station =
                    (uint8_t)(station_idx == 0 ? 1 : 0);
                break;
            case 6:
                tampered.quantity =
                    original->quantity == 2u ? 3u : 2u;
                break;
            case 7:
                tampered.mined_block =
                    original->mined_block + 1u;
                break;
            case 8:
                tampered.parent_merkle[0] ^= 0x80u;
                break;
            default:
                ASSERT(false);
        }
        assert_origin_metadata_rejected(
            w, station_idx, &tampered);
    }
}

TEST(test_cargo_origin_semantics_bind_every_manifest_trait) {
    chain_test_setup("cargo_semantic_binding");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 17030u;
    world_reset(w);
    chain_test_wipe_logs(w);
    station_t *station = &w->stations[0];
    station->chain_event_count = 0;
    memset(station->chain_last_hash, 0, 32);

    uint8_t fragment_pub[32];
    for (int i = 0; i < 32; i++)
        fragment_pub[i] = (uint8_t)(0x21 + i);
    cargo_unit_t ingot = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
        fragment_pub, 0, &ingot));
    ingot.origin_station = 0;
    ingot.mined_block = 4422u;
    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, fragment_pub, 0, &ingot));
    chain_payload_smelt_t rejected_smelt = {0};
    ASSERT(!chain_payload_smelt_bind_output(
        &rejected_smelt, fragment_pub, 1, &ingot));
    cargo_unit_t arbitrary_ingot = ingot;
    arbitrary_ingot.pub[0] ^= 0x80u;
    arbitrary_ingot.prefix_class =
        (uint8_t)mining_pubkey_class(arbitrary_ingot.pub);
    ASSERT(!chain_payload_smelt_bind_output(
        &rejected_smelt, fragment_pub, 0,
        &arbitrary_ingot));
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_SMELT,
            &smelt, (uint16_t)sizeof(smelt)),
        1);

    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(w, 0, &ingot, NULL);
    ASSERT(evaluated.accepted);
    ASSERT(evaluated.local_origin_without_receipt);
    ASSERT_EQ_INT(
        evaluated.craft_provenance,
        CARGO_CRAFT_PROVENANCE_NOT_CRAFT);
    assert_each_cargo_metadata_tamper_rejected(
        w, 0, &ingot);

    cargo_unit_t frame = {0};
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &ingot, 1, 0, &frame));
    frame.origin_station = 0;
    chain_payload_craft_t craft = {0};
    ASSERT(chain_payload_craft_bind_output(
        &craft, &ingot, 1, &frame));
    chain_payload_craft_t rejected_craft = {0};
    cargo_unit_t mislabeled_input = ingot;
    mislabeled_input.commodity =
        (uint8_t)COMMODITY_CUPRITE_INGOT;
    ASSERT(!chain_payload_craft_bind_output(
        &rejected_craft, &mislabeled_input, 1, &frame));
    cargo_unit_t arbitrary_frame = frame;
    arbitrary_frame.pub[0] ^= 0x40u;
    ASSERT(!chain_payload_craft_bind_output(
        &rejected_craft, &ingot, 1, &arbitrary_frame));
    cargo_unit_t grouped_frame = frame;
    grouped_frame.quantity = 2u;
    ASSERT(!chain_payload_craft_bind_output(
        &rejected_craft, &ingot, 1, &grouped_frame));
    const uint8_t legacy_salt[8] = {
        'S', 'E', 'M', 'V', '1', 'T', 'S', 'T'
    };
    cargo_unit_t grouped_legacy = {0};
    ASSERT(hash_legacy_migrate_unit(
        legacy_salt, COMMODITY_FRAME, 0,
        &grouped_legacy));
    grouped_legacy.quantity = 2u;
    ASSERT(!chain_payload_craft_bind_output(
        &rejected_craft, NULL, 0, &grouped_legacy));
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_CRAFT,
            &craft, (uint16_t)sizeof(craft)),
        2);
    evaluated =
        cargo_receipt_evaluate_at_station(w, 0, &frame, NULL);
    ASSERT(evaluated.accepted);
    ASSERT(evaluated.local_origin_without_receipt);
    ASSERT_EQ_INT(
        evaluated.craft_provenance,
        CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1);
    ASSERT(!evaluated.craft_input_lineage_proven);
    ASSERT(!evaluated.craft_conservation_proven);
    assert_each_cargo_metadata_tamper_rejected(
        w, 0, &frame);

    /*
     * Version-zero payloads remain readable as history, but their former
     * padding cannot silently become proof of a cargo label.
     */
    cargo_unit_t legacy_unbound = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
        fragment_pub, 1, &legacy_unbound));
    legacy_unbound.origin_station = 0;
    legacy_unbound.mined_block = 4423u;
    chain_payload_smelt_t unbound = {0};
    memcpy(unbound.fragment_pub, fragment_pub, 32);
    memcpy(unbound.ingot_pub, legacy_unbound.pub, 32);
    unbound.prefix_class = legacy_unbound.prefix_class;
    unbound.mined_block = legacy_unbound.mined_block;
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_SMELT,
            &unbound, (uint16_t)sizeof(unbound)),
        3);
    assert_origin_metadata_rejected(
        w, 0, &legacy_unbound);

    /*
     * A signed V1 record with recipe/output contradictions is also
     * semantically unbound. Trust in an authority is not permission for a
     * malformed imported history to redefine the cargo grammar.
     */
    cargo_unit_t malformed = frame;
    memset(malformed.pub, 0xA5, sizeof(malformed.pub));
    malformed.kind = (uint8_t)CARGO_KIND_LASER;
    malformed.commodity =
        (uint8_t)COMMODITY_LASER_MODULE;
    malformed.recipe_id =
        (uint16_t)RECIPE_FRAME_BASIC;
    chain_payload_craft_t malformed_payload = {0};
    malformed_payload.recipe_id =
        (uint16_t)RECIPE_FRAME_BASIC;
    malformed_payload.input_count = 1u;
    malformed_payload.semantics_version =
        CHAIN_CARGO_SEMANTICS_V1;
    malformed_payload.output_kind = malformed.kind;
    malformed_payload.output_commodity =
        malformed.commodity;
    malformed_payload.output_grade = malformed.grade;
    malformed_payload.output_quantity =
        malformed.quantity;
    memcpy(malformed_payload.output_pub,
           malformed.pub, 32);
    memcpy(malformed_payload.input_pubs[0],
           ingot.pub, 32);
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_CRAFT,
            &malformed_payload,
            (uint16_t)sizeof(malformed_payload)),
        4);
    assert_origin_metadata_rejected(
        w, 0, &malformed);

    /* A valid pub paired with the wrong signed SMELT output index cannot
     * become an origin proof. */
    cargo_unit_t wrong_index_ingot = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
        fragment_pub, 2, &wrong_index_ingot));
    wrong_index_ingot.origin_station = 0;
    wrong_index_ingot.mined_block = 4424u;
    chain_payload_smelt_t wrong_index_payload = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &wrong_index_payload, fragment_pub, 2,
        &wrong_index_ingot));
    wrong_index_payload.output_index = 3u;
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_SMELT,
            &wrong_index_payload,
            (uint16_t)sizeof(wrong_index_payload)),
        5);
    assert_origin_metadata_rejected(
        w, 0, &wrong_index_ingot);

    /* Correct recipe labels do not rescue a caller-selected arbitrary pub. */
    cargo_unit_t arbitrary_signed_frame = frame;
    arbitrary_signed_frame.pub[0] ^= 0x20u;
    chain_payload_craft_t arbitrary_pub_payload = craft;
    memcpy(arbitrary_pub_payload.output_pub,
           arbitrary_signed_frame.pub, 32);
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_CRAFT,
            &arbitrary_pub_payload,
            (uint16_t)sizeof(arbitrary_pub_payload)),
        6);
    assert_origin_metadata_rejected(
        w, 0, &arbitrary_signed_frame);

    /* Likewise, editing an input identity invalidates the derived output
     * pub even when every visible output label remains plausible. */
    cargo_unit_t input_tamper_frame = {0};
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &ingot, 1, 1,
        &input_tamper_frame));
    input_tamper_frame.origin_station = 0;
    chain_payload_craft_t input_tamper_payload = {0};
    ASSERT(chain_payload_craft_bind_output(
        &input_tamper_payload, &ingot, 1,
        &input_tamper_frame));
    input_tamper_payload.input_pubs[0][0] ^= 0x10u;
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_CRAFT,
            &input_tamper_payload,
            (uint16_t)sizeof(input_tamper_payload)),
        7);
    assert_origin_metadata_rejected(
        w, 0, &input_tamper_frame);

    /*
     * The signed grade is an input to the fabricated pub derivation. A
     * grade-only relabel therefore cannot preserve a product identity.
     */
    cargo_unit_t grade_tamper_frame = {0};
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &ingot, 1, 2,
        &grade_tamper_frame));
    grade_tamper_frame.origin_station = 0;
    chain_payload_craft_t grade_tamper_payload = {0};
    ASSERT(chain_payload_craft_bind_output(
        &grade_tamper_payload, &ingot, 1,
        &grade_tamper_frame));
    cargo_unit_t relabeled_grade_frame =
        grade_tamper_frame;
    relabeled_grade_frame.grade =
        (uint8_t)MINING_GRADE_FINE;
    grade_tamper_payload.output_grade =
        relabeled_grade_frame.grade;
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_CRAFT,
            &grade_tamper_payload,
            (uint16_t)sizeof(grade_tamper_payload)),
        8);
    assert_origin_metadata_rejected(
        w, 0, &relabeled_grade_frame);

    /*
     * Unused fixed-width input slots are signed canonical zeroes, not an
     * extension channel for alternate recipe preimages.
     */
    cargo_unit_t unused_input_frame = {0};
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &ingot, 1, 3,
        &unused_input_frame));
    unused_input_frame.origin_station = 0;
    chain_payload_craft_t unused_input_payload = {0};
    ASSERT(chain_payload_craft_bind_output(
        &unused_input_payload, &ingot, 1,
        &unused_input_frame));
    unused_input_payload.input_pubs[1][0] = 0x5Au;
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_CRAFT,
            &unused_input_payload,
            (uint16_t)sizeof(unused_input_payload)),
        9);
    assert_origin_metadata_rejected(
        w, 0, &unused_input_frame);

    /*
     * V1 SMELT reserves two authenticated bytes for future schema growth.
     * Until a later version assigns them, non-zero values are noncanonical.
     */
    cargo_unit_t reserved_smelt_ingot = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
        fragment_pub, 4, &reserved_smelt_ingot));
    reserved_smelt_ingot.origin_station = 0;
    reserved_smelt_ingot.mined_block = 4425u;
    chain_payload_smelt_t reserved_smelt_payload = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &reserved_smelt_payload, fragment_pub, 4,
        &reserved_smelt_ingot));
    reserved_smelt_payload._reserved[0] = 0x5Au;
    ASSERT_EQ_INT(
        chain_log_emit(
            w, station, CHAIN_EVT_SMELT,
            &reserved_smelt_payload,
            (uint16_t)sizeof(reserved_smelt_payload)),
        10);
    assert_origin_metadata_rejected(
        w, 0, &reserved_smelt_ingot);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(station, &walked, NULL));
    ASSERT_EQ_INT((int)walked, 10);
    chain_test_teardown();
}

TEST(test_chain_log_seed_rarity_tiers_have_real_content) {
    /* Regression guard: world_reset's tier seed events must carry real
     * flavor text bound by SHA, not the literal placeholder strings
     * "common" / "uncommon" / "rare" / "ultra_rare" that early
     * iterations of this code emitted. The chain log is the source
     * of truth for tier content; events that hash to the placeholder
     * names are theater (every station's chain would be identical). */
    chain_test_setup("seed_real_content");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9700u;
    chain_test_wipe_logs(w);
    world_reset(w);
    world_seed_station_chain_genesis(w);

    /* Compute SHA-256 of each placeholder string for the negative match. */
    static const char *placeholders[4] = {
        "common", "uncommon", "rare", "ultra_rare"
    };
    uint8_t placeholder_sha[4][32];
    for (int i = 0; i < 4; i++) {
        sha256_bytes((const uint8_t *)placeholders[i],
                     strlen(placeholders[i]),
                     placeholder_sha[i]);
    }

    /* Walk station 0's on-disk log and pull every RARITY_TIER event
     * (operator_post payload kind == 2). For each tier 0-3, assert
     * payload SHA != placeholder SHA AND text length > strlen("common"). */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *fp = fopen(path, "rb");
    ASSERT(fp != NULL);

    int tiers_seen[4] = {0, 0, 0, 0};
    while (!feof(fp)) {
        chain_event_header_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, fp) != 1) break;
        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, fp) != 1) break;
        if (hdr.type == CHAIN_EVT_OPERATOR_POST && plen >= 38) {
            uint8_t prefix[38];
            if (fread(prefix, sizeof(prefix), 1, fp) != 1) break;
            uint8_t kind = prefix[0];
            uint8_t tier = prefix[1];
            /* Skip the body bytes for non-RARITY_TIER kinds. */
            uint16_t body_len = (uint16_t)(plen - 38);
            uint8_t body[256];
            if (body_len > 0) {
                if (body_len > sizeof(body)) {
                    fseek(fp, body_len, SEEK_CUR);
                    continue;
                }
                if (fread(body, body_len, 1, fp) != 1) break;
            }
            if (kind == 2 /* RARITY_TIER */ && tier < 4) {
                /* Tier text must NOT be the placeholder string. */
                ASSERT(memcmp(prefix + 4, placeholder_sha[tier], 32) != 0);
                /* Body length must exceed the placeholder length —
                 * proves the text is something more than just
                 * "common"/"uncommon"/etc. */
                ASSERT((int)body_len > (int)strlen(placeholders[tier]));
                tiers_seen[tier]++;
            }
        } else {
            fseek(fp, plen, SEEK_CUR);
        }
    }
    fclose(fp);

    /* All four tiers must have been emitted. */
    for (int i = 0; i < 4; i++) {
        ASSERT(tiers_seen[i] >= 1);
    }

    chain_test_teardown();
}

TEST(test_fresh_genesis_anchors_legacy_station_cargo_before_motd) {
    chain_test_setup("legacy_cargo_genesis");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 17031u;
    chain_test_wipe_logs(w);
    world_reset(w);

    station_t *kepler = &w->stations[1];
    const uint8_t origin[8] =
        {'A','N','C','H','O','R','0','1'};
    int before = kepler->manifest.count;
    ASSERT_EQ_INT(station_finished_mint(
                      kepler, COMMODITY_FRAME, 1, origin),
                  1);
    ASSERT_EQ_INT(kepler->manifest.count, before + 1);
    cargo_unit_t *unit = &kepler->manifest.units[before];
    ASSERT_EQ_INT(unit->recipe_id, RECIPE_LEGACY_MIGRATE);
    /* station_finished_mint has no world/index parameter, so the explicit
     * bootstrap boundary is responsible for stamping the real author. */
    ASSERT_EQ_INT(unit->origin_station, 0);

    cargo_unit_t before_inventory = *unit;
    cargo_legacy_inventory_report_t inventory = {0};
    ASSERT(cargo_legacy_inventory_scan_world(w, &inventory));
    ASSERT(inventory.legacy_candidates >= 1u);
    ASSERT(inventory.holder_candidate_count[
               CARGO_LEGACY_HOLDER_STATION_MANIFEST] >= 1u);
    ASSERT(memcmp(unit, &before_inventory, sizeof(*unit)) == 0);

    /* Fresh genesis remains the one explicit server-authored bootstrap. */
    world_seed_station_chain_genesis(w);

    ASSERT(!kepler->chain_append_blocked);
    ASSERT_EQ_INT(unit->origin_station, 1);
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(
            kepler, unit->pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_CRAFT);
    ASSERT_EQ_INT(proof.craft_recipe_id, RECIPE_LEGACY_MIGRATE);
    ASSERT_EQ_INT(proof.craft_input_count, 0);
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(w, 1, unit, NULL);
    ASSERT(evaluated.accepted);
    ASSERT(evaluated.local_origin_without_receipt);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(kepler, &walked, NULL));
    ASSERT_EQ_INT((int)walked, (int)kepler->chain_event_count);
    chain_test_teardown();
}

TEST(test_legacy_cargo_anchor_append_failure_leaves_unit_unchanged) {
    chain_test_setup("legacy_cargo_anchor_failure");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 17032u;
    chain_test_wipe_logs(w);
    world_reset(w);

    const uint8_t origin[8] =
        {'A','N','C','H','F','A','I','L'};
    cargo_unit_t unit = {0};
    ASSERT(hash_legacy_migrate_unit(
        origin, COMMODITY_FRAME, 0, &unit));
    cargo_unit_t before = unit;
    cargo_unit_t *units[] = {&unit};

    chain_log_test_fault_inject(
        CHAIN_LOG_TEST_FAULT_WRITE, CHAIN_EVT_CRAFT, 1);
    ASSERT(!world_anchor_legacy_cargo_origins(
        w, 1, units, 1));
    ASSERT(memcmp(&unit, &before, sizeof(unit)) == 0);
    ASSERT_EQ_INT((int)w->stations[1].chain_event_count, 0);
    ASSERT(w->stations[1].chain_append_blocked);
    chain_test_teardown();
}

TEST(test_legacy_cargo_anchor_rejects_nonmigration_craft_origin) {
    chain_test_setup("legacy_cargo_anchor_wrong_recipe");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 17033u;
    chain_test_wipe_logs(w);
    world_reset(w);

    const uint8_t origin[8] =
        {'A','N','C','H','W','R','N','G'};
    cargo_unit_t unit = {0};
    ASSERT(hash_legacy_migrate_unit(
        origin, COMMODITY_FRAME, 0, &unit));
    cargo_unit_t before = unit;

    chain_payload_craft_t conflicting = {
        .recipe_id = (uint16_t)RECIPE_FRAME_BASIC,
        .input_count = 1,
    };
    memcpy(conflicting.output_pub, unit.pub,
           sizeof(conflicting.output_pub));
    memset(conflicting.input_pubs[0], 0x5a,
           sizeof(conflicting.input_pubs[0]));
    ASSERT_EQ_INT(
        chain_log_emit(
            w, &w->stations[1], CHAIN_EVT_CRAFT,
            &conflicting, (uint16_t)sizeof(conflicting)),
        1);

    cargo_unit_t *units[] = {&unit};
    ASSERT(!world_anchor_legacy_cargo_origins(
        w, 1, units, 1));
    ASSERT(memcmp(&unit, &before, sizeof(unit)) == 0);
    ASSERT_EQ_INT((int)w->stations[1].chain_event_count, 1);
    ASSERT(!w->stations[1].chain_append_blocked);
    chain_test_teardown();
}

TEST(test_world_reset_does_not_emit_to_chain_log) {
    /* world_reset used to emit per-station MOTD + rarity-tier events
     * directly, which corrupted the chain on every server restart:
     * load_world_state runs world_reset before world_load restores
     * the saved seed, so each restart appended new prev_hash=0
     * MOTDs to the default-seed pubkey's log file forever. Genesis
     * seeding has been moved to world_seed_station_chain_genesis,
     * called only on fresh-world boots. This test guards the move. */
    chain_test_setup("no_implicit_emit");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 14143u;
    chain_test_wipe_logs(w);
    world_reset(w);

    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT((int)w->stations[i].chain_event_count, 0);
        char path[256];
        ASSERT(chain_log_path_for(w->stations[i].station_pubkey,
                                   path, sizeof(path)));
        FILE *fp = fopen(path, "rb");
        /* Either the file doesn't exist, or it's empty. */
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long size = ftell(fp);
            fclose(fp);
            ASSERT_EQ_INT((int)size, 0);
        }
    }

    chain_test_teardown();
}

TEST(test_chain_log_fragment_tow_payload_size) {
    /* Wire-format guard. Pin the payload sizes that chain_log.h's
     * static_asserts already enforce — duplicating the check at the
     * test layer ensures any reviewer sees the size pinned in two
     * places when they're tempted to grow the struct. */
    ASSERT_EQ_INT((int)sizeof(chain_payload_fragment_tow_t), 80);
    ASSERT_EQ_INT((int)sizeof(chain_payload_fragment_release_t), 88);
}

TEST(test_chain_log_fragment_tow_emit_and_verify) {
    /* Round-trip: emit a hand-crafted FRAGMENT_TOW event, walk the
     * log, and confirm the verifier accepts it AND the per-type
     * counter increments. Same shape as the existing per-event-type
     * round-trips above. */
    chain_test_setup("frag_tow_emit");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9300u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    chain_payload_fragment_tow_t payload = {0};
    /* Plausible-looking content — any non-zero bytes will do. */
    for (int b = 0; b < 32; b++) payload.fragment_pub[b] = (uint8_t)(0x10 + b);
    for (int b = 0; b < 32; b++) payload.tower_player_pub[b] = (uint8_t)(0x80 + b);
    for (int b = 0; b < 8; b++) payload.tower_session_token[b] = (uint8_t)(0xA0 + b);
    payload.epoch_tick = 12345u;

    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_FRAGMENT_TOW,
                                 &payload, (uint16_t)sizeof(payload));
    ASSERT(id == 1);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT_EQ_INT((int)walked, 1);

    chain_test_teardown();
}

TEST(test_chain_log_fragment_release_emit_and_verify) {
    /* Same shape as the TOW round-trip. The reason byte exercises a
     * code path the TOW payload doesn't (release-specific). */
    chain_test_setup("frag_release_emit");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9301u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    chain_payload_fragment_release_t payload = {0};
    for (int b = 0; b < 32; b++) payload.fragment_pub[b] = (uint8_t)(0x20 + b);
    for (int b = 0; b < 32; b++) payload.tower_player_pub[b] = (uint8_t)(0x60 + b);
    payload.epoch_tick = 99999u;
    payload.reason = (uint8_t)FRAGMENT_RELEASE_BAND_SNAP;

    uint64_t id = chain_log_emit(w, &w->stations[0], CHAIN_EVT_FRAGMENT_RELEASE,
                                 &payload, (uint16_t)sizeof(payload));
    ASSERT(id == 1);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT_EQ_INT((int)walked, 1);

    chain_test_teardown();
}

TEST(test_chain_log_fragment_lifecycle_e2e) {
    /* End-to-end through the sim: spawn a fragment in signal range,
     * have a player tractor-grab it, then yank them past 1.5x tractor
     * range to trigger a band-snap release. The chain log should
     * gain a TOW event at grab-time and a RELEASE event at snap-time,
     * both signed by the witnessing station. */
    chain_test_setup("frag_lifecycle");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9400u;
    world_reset(w);
    chain_test_wipe_logs(w);
    /* The wipe deleted any previous on-disk files for this test dir.
     * Zero the in-memory state so newly-emitted events chain from a
     * clean slate that matches the empty on-disk file. */
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    w->players[0].docked = false;
    w->players[0].session_ready = true;
    memset(w->players[0].session_token, 0x77, 8);
    w->players[0].pubkey_set = true;
    w->players[0].pubkey_proof_ok = true;
    w->players[0].pubkey_challenge_consumed = true;
    w->players[0].pubkey_identity_finalized = true;
    memset(w->players[0].pubkey, 0x88,
           sizeof(w->players[0].pubkey));

    /* Place a fragment near Prospect (station 0) so the witness picker
     * picks it up. ~200 units offset is well inside Prospect's signal
     * range. Spawn close to the player's hull so the tractor-pulse
     * snaps it on the next sim step. */
    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 5.0f;
    a->max_ore = 5.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    /* Non-zero fragment_pub so the tow event payload carries something
     * recognizable in the log. */
    for (int b = 0; b < 32; b++) a->fragment_pub[b] = (uint8_t)(0x33 + b);

    /* Position: 50 units from Prospect's center (well within signal),
     * with the player 30 units away (well within tractor range). */
    a->pos = v2(w->stations[0].pos.x + 50.0f, w->stations[0].pos.y);
    a->vel = v2(0, 0);
    w->players[0].ship->pos = v2(a->pos.x + 30.0f, a->pos.y);
    w->players[0].ship->vel = v2(0, 0);
    /* Tractor is gated by input.tractor_hold (synced into ship.tractor_active
     * each tick from sample_input_intent). Set the input directly — setting
     * the cached ship flag would be clobbered on the next sim step. */
    w->players[0].input.tractor_hold = true;

    uint64_t before = w->stations[0].chain_event_count;
    /* One sim step is enough for the tractor pulse to grab it. */
    world_sim_step(w, 1.0f / 120.0f);
    /* The chain log gained AT LEAST a TOW event (the tractor-grab fires
     * synchronously inside step_fragment_collection). The witnessing
     * station for a fragment 50 units from Prospect is Prospect itself. */
    ASSERT(w->stations[0].chain_event_count > before);

    /* Now yank the player far past tractor range to force a band snap.
     * Tractor range scales with tractor_level; default ship is well
     * under 1000 units so a 5000-unit jump is unambiguous. */
    w->players[0].ship->pos = v2(a->pos.x + 5000.0f, a->pos.y);

    uint64_t mid = w->stations[0].chain_event_count;
    /* step_leashed_fragments runs when tractor_hold is off. The
     * band-snap branch fires when fragment distance > 1.5 * tractor_range. */
    w->players[0].input.tractor_hold = false;
    world_sim_step(w, 1.0f / 120.0f);

    /* RELEASE event must have landed; verify count grew further. */
    ASSERT(w->stations[0].chain_event_count > mid);

    /* Walk the log and confirm full chain integrity. */
    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    ASSERT(walked == w->stations[0].chain_event_count);

    /*
     * Byte-level credential exclusion: production tow/release emitters retain
     * the verified public identity but the retired token slots stay zero.
     */
    uint8_t raw[4096];
    size_t raw_len = 0;
    ASSERT(chain_test_read_log(
        &w->stations[0], raw, sizeof(raw), &raw_len));
    size_t offset = 0;
    int tow_records = 0;
    int release_records = 0;
    static const uint8_t zero_token[8] = {0};
    while (offset + CHAIN_EVENT_HEADER_SIZE + 2u <= raw_len) {
        uint8_t type = raw[offset + 16u];
        size_t length_offset = offset + CHAIN_EVENT_HEADER_SIZE;
        uint16_t payload_len =
            (uint16_t)raw[length_offset] |
            (uint16_t)((uint16_t)raw[length_offset + 1u] << 8u);
        size_t payload_offset = length_offset + 2u;
        ASSERT(payload_offset + payload_len <= raw_len);
        if (type == CHAIN_EVT_FRAGMENT_TOW) {
            chain_payload_fragment_tow_t payload;
            ASSERT_EQ_INT(payload_len, (int)sizeof(payload));
            memcpy(&payload, &raw[payload_offset], sizeof(payload));
            ASSERT(memcmp(payload.tower_player_pub,
                          w->players[0].pubkey, 32) == 0);
            ASSERT(memcmp(payload.tower_session_token,
                          zero_token, sizeof(zero_token)) == 0);
            tow_records++;
        } else if (type == CHAIN_EVT_FRAGMENT_RELEASE) {
            chain_payload_fragment_release_t payload;
            ASSERT_EQ_INT(payload_len, (int)sizeof(payload));
            memcpy(&payload, &raw[payload_offset], sizeof(payload));
            ASSERT(memcmp(payload.tower_player_pub,
                          w->players[0].pubkey, 32) == 0);
            ASSERT(memcmp(payload.tower_session_token,
                          zero_token, sizeof(zero_token)) == 0);
            release_records++;
        }
        offset = payload_offset + payload_len;
    }
    ASSERT(offset == raw_len);
    ASSERT_EQ_INT(tow_records, 1);
    ASSERT_EQ_INT(release_records, 1);

    chain_test_teardown();
}

void register_chain_log_tests(void);
void register_chain_log_tests(void) {
    TEST_SECTION("\n--- Chain Log (#479 C) ---\n");
    RUN(test_chain_log_emit_and_verify);
    RUN(test_chain_log_batch_commits_contiguous_verified_events);
    RUN(test_chain_log_batch_prevalidates_before_any_commit);
    RUN(test_chain_log_batch_disk_disabled_commits_atomically_in_memory);
    RUN(test_chain_log_batch_event_id_overflow_is_inert);
    RUN(test_chain_log_batch_partial_write_fault_rolls_back_exact_bytes);
    RUN(test_chain_log_batch_flush_fault_rolls_back_exact_bytes);
    RUN(test_chain_log_batch_close_fault_rolls_back_exact_bytes);
    RUN(test_chain_log_first_append_dir_sync_fault_removes_new_file);
    RUN(test_chain_log_first_boot_parent_sync_fault_removes_new_tree);
    RUN(test_chain_log_first_boot_syncs_nested_dir_once);
    RUN(test_chain_log_position_faults_remove_new_log_and_directory);
    RUN(test_chain_log_chain_linkage);
    RUN(test_chain_log_verify_accepts_clean_segment_reset);
    RUN(test_chain_log_tampered_event_detected);
    RUN(test_chain_log_wrong_station_signature_rejected);
    RUN(test_chain_log_save_load_continuity);
    RUN(test_chain_log_emit_blocked_by_failed_health);
    RUN(test_chain_log_health_repair_hints_are_operator_facing);
    RUN(test_world_load_blocks_chain_appends_after_failed_verify);
    RUN(test_world_load_blocks_chain_appends_after_missing_tail);
    RUN(test_world_load_blocks_chain_appends_when_verified_tail_is_ahead);
    RUN(test_chain_log_cross_station_independent);
    RUN(test_chain_log_hopper_smelt_path_retired);
    RUN(test_chain_log_smelt_emits_event_fragment_path);
    RUN(test_chain_log_rock_destroy_emits_event);
    RUN(test_chain_log_claim_fragment_emits_event);
    RUN(test_chain_log_operator_post_emit);
    RUN(test_chain_log_operator_post_all_kinds);
    RUN(test_chain_log_operator_post_replay_determinism);
    RUN(test_chain_log_operator_post_text_tamper);
    RUN(test_chain_log_route_history_tail_reader);
    RUN(test_chain_log_cargo_transform_reader);
    RUN(test_chain_log_evidence_snapshot_freezes_verified_visit_bytes);
    RUN(test_cargo_origin_semantics_bind_every_manifest_trait);
    RUN(test_chain_log_seed_rarity_tiers_have_real_content);
    RUN(test_fresh_genesis_anchors_legacy_station_cargo_before_motd);
    RUN(test_legacy_cargo_anchor_append_failure_leaves_unit_unchanged);
    RUN(test_legacy_cargo_anchor_rejects_nonmigration_craft_origin);
    RUN(test_world_reset_does_not_emit_to_chain_log);
    RUN(test_chain_log_fragment_tow_payload_size);
    RUN(test_chain_log_fragment_tow_emit_and_verify);
    RUN(test_chain_log_fragment_release_emit_and_verify);
    RUN(test_chain_log_fragment_lifecycle_e2e);
}
