/*
 * test_signal_verify.c -- Layer E of #479: standalone verifier.
 *
 * Covers chain_log_verify_with_pubkey end-to-end:
 *   1. Round-trip: emit a log, verify it via the lifted API.
 *   2. Header-byte tamper detection.
 *   3. Signature corruption produces bad_signatures = 1.
 *   4. Mid-log truncation produces a linkage / monotonic violation.
 *   5. Multi-station provenance: matched cargo_pub on both sides.
 *   6. Fixture regression — `chain_log_verify_with_pubkey` accepts the
 *      committed test fixture under tests/fixtures/.
 */
#include "test_harness.h"

#include "chain_log.h"
#include "station_authority.h"
#include "game_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void sv_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_sv_%s", TMP("clog"), suffix);
    chain_log_set_dir(path);
}

static void sv_teardown(void) { chain_log_set_dir(NULL); }

static void sv_wipe(world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w->stations[s]);
}

static FILE *sv_open_log(const station_t *s) {
    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) return NULL;
    return fopen(path, "rb");
}

TEST(test_signal_verify_roundtrip) {
    sv_setup("roundtrip");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50001u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    /* Spread events across all 6 types so event_type_counts populates. */
    uint8_t pl[16] = "verify-pl";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT, pl, sizeof(pl)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_CRAFT, pl, sizeof(pl)) == 2);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_TRANSFER, pl, sizeof(pl)) == 3);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_TRADE, pl, sizeof(pl)) == 4);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, sizeof(pl)) == 5);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_ROCK_DESTROY, pl, sizeof(pl)) == 6);

    FILE *f = sv_open_log(&w->stations[0]);
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, w->stations[0].station_pubkey, &r);
    fclose(f);
    ASSERT(ok);
    ASSERT_EQ_INT((int)r.total_events, 6);
    ASSERT_EQ_INT((int)r.valid_events, 6);
    ASSERT_EQ_INT((int)r.bad_signatures, 0);
    ASSERT_EQ_INT((int)r.bad_linkage, 0);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_SMELT], 1);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_ROCK_DESTROY], 1);
    sv_teardown();
}

TEST(test_signal_verify_byte_tamper) {
    sv_setup("tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50002u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[8];
    memcpy(pl, "tamper--", 8);
    for (int i = 0; i < 5; i++)
        ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, sizeof(pl)) == (uint64_t)(i+1));

    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *fw = fopen(path, "r+b");
    ASSERT(fw != NULL);
    /* Flip byte 17 of event 3 — same as the chain_log test. */
    long entry_size = 184 + 2 + 8;
    fseek(fw, entry_size * 2 + 17, SEEK_SET);
    uint8_t b; ASSERT(fread(&b, 1, 1, fw) == 1);
    fseek(fw, entry_size * 2 + 17, SEEK_SET);
    b ^= 0xFF;
    ASSERT(fwrite(&b, 1, 1, fw) == 1);
    fclose(fw);

    FILE *f = sv_open_log(&w->stations[0]);
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, w->stations[0].station_pubkey, &r);
    fclose(f);
    ASSERT(!ok);
    ASSERT(r.first_fail_event_id != 0);
    ASSERT(r.first_fail_reason[0] != '\0');
    sv_teardown();
}

TEST(test_signal_verify_signature_corruption) {
    sv_setup("sig");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50003u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[8];
    memcpy(pl, "sig-test", 8);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, sizeof(pl)) == 1);

    /* Header layout: signature occupies the last 64 bytes of the
     * 184-byte header, so offset 120..183. Overwrite a few bytes
     * mid-signature with deterministic garbage so we don't randomly
     * land on a valid signature. */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *fw = fopen(path, "r+b");
    ASSERT(fw != NULL);
    fseek(fw, 120, SEEK_SET);
    uint8_t junk[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    ASSERT(fwrite(junk, sizeof(junk), 1, fw) == 1);
    fclose(fw);

    FILE *f = sv_open_log(&w->stations[0]);
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, w->stations[0].station_pubkey, &r);
    fclose(f);
    ASSERT(!ok);
    ASSERT_EQ_INT((int)r.bad_signatures, 1);
    sv_teardown();
}

TEST(test_signal_verify_mid_log_splice) {
    /* Splice out one event so prev_hash linkage breaks. Drop event 2:
     * the file becomes events {1, 3, 4, 5}. event 3's prev_hash points
     * at event 2 (now missing), so it can't link from event 1. */
    sv_setup("splice");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50004u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[8];
    memcpy(pl, "splice--", 8);
    for (int i = 0; i < 5; i++)
        ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_LEDGER, pl, sizeof(pl)) == (uint64_t)(i+1));

    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, path, sizeof(path)));
    FILE *fr = fopen(path, "rb");
    ASSERT(fr != NULL);
    fseek(fr, 0, SEEK_END);
    long total = ftell(fr);
    rewind(fr);
    unsigned char *buf = (unsigned char *)malloc((size_t)total);
    ASSERT(buf != NULL);
    ASSERT(fread(buf, (size_t)total, 1, fr) == 1);
    fclose(fr);

    long entry = 184 + 2 + 8;
    /* Rewrite the file as {event 1, event 3, event 4, event 5}. */
    FILE *fw = fopen(path, "wb");
    ASSERT(fw != NULL);
    ASSERT(fwrite(buf, (size_t)entry, 1, fw) == 1);                   /* event 1 */
    ASSERT(fwrite(buf + entry * 2, (size_t)(entry * 3), 1, fw) == 1); /* events 3-5 */
    fclose(fw);
    free(buf);

    FILE *f = sv_open_log(&w->stations[0]);
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, w->stations[0].station_pubkey, &r);
    fclose(f);
    ASSERT(!ok);
    /* Either prev_hash linkage or monotonic event_id breaks first.
     * One of those counters must be non-zero. */
    ASSERT(r.bad_linkage > 0 || r.monotonic_violations > 0);
    sv_teardown();
}

TEST(test_signal_verify_wrong_pubkey_rejected) {
    /* Verify station 0's log against station 2's pubkey — the
     * authority field on every event mismatches. */
    sv_setup("wrong_pub");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50005u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t pl[4] = "abc";
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT, pl, sizeof(pl)) == 1);

    FILE *f = sv_open_log(&w->stations[0]);
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, w->stations[2].station_pubkey, &r);
    fclose(f);
    ASSERT(!ok);
    ASSERT_EQ_INT((int)r.bad_authority, 1);
    sv_teardown();
}

TEST(test_signal_verify_multi_station_independent) {
    /* Two stations with separate chain logs. Each verifies cleanly
     * against its own pubkey but fails against the other's. */
    sv_setup("multi");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50006u;
    world_reset(w);
    sv_wipe(w);
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }

    /* Same cargo_pub appears on both station 0 (sender TRANSFER) and
     * station 2 (receiver TRANSFER) — what cross-station provenance
     * looks like in real traffic. */
    chain_payload_transfer_t xfer = {0};
    for (int b = 0; b < 32; b++) xfer.cargo_pub[b] = (uint8_t)(0x80 + b);
    memcpy(xfer.from_pubkey, w->stations[0].station_pubkey, 32);
    memcpy(xfer.to_pubkey, w->stations[2].station_pubkey, 32);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_TRANSFER, &xfer, sizeof(xfer)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[2], CHAIN_EVT_TRANSFER, &xfer, sizeof(xfer)) == 1);

    FILE *f0 = sv_open_log(&w->stations[0]);
    FILE *f2 = sv_open_log(&w->stations[2]);
    ASSERT(f0 != NULL && f2 != NULL);
    chain_log_verify_report_t r0, r2;
    ASSERT(chain_log_verify_with_pubkey(f0, w->stations[0].station_pubkey, &r0));
    ASSERT(chain_log_verify_with_pubkey(f2, w->stations[2].station_pubkey, &r2));
    rewind(f0);
    rewind(f2);
    /* Cross-pubkey verification must fail. */
    chain_log_verify_report_t bad;
    ASSERT(!chain_log_verify_with_pubkey(f0, w->stations[2].station_pubkey, &bad));
    fclose(f0); fclose(f2);
    sv_teardown();
}

TEST(test_signal_verify_committed_fixture) {
    /* Walks the committed canned fixture under tests/fixtures/ and
     * confirms it still parses + verifies cleanly against its
     * filename-derived pubkey. Failure here means either:
     *   - the generator drifted and the committed fixture is stale, or
     *   - chain_log_verify_with_pubkey itself broke.
     *
     * tests/fixtures/README.md documents the regeneration recipe; this
     * test is the gate that forces an intentional re-commit. */
    const char *path =
        "tests/fixtures/3F5qRPtKg8GhGNnbd3qCj6nVJxWsGxq7pvH84okYLAqf.log";
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Allow running the test binary from build-test/, build-san/,
         * etc — the fixtures live one dir up. */
        path = "../tests/fixtures/3F5qRPtKg8GhGNnbd3qCj6nVJxWsGxq7pvH84okYLAqf.log";
        f = fopen(path, "rb");
    }
    if (!f) {
        TEST_WARN("fixture file not found in cwd or parent; skipping");
        return;
    }
    /* Decode the b58 stem of the filename. We hardcode the expected
     * pubkey as a small static array here so this test doesn't have
     * to pull in the base58 decoder. */
    static const uint8_t expected_pub[32] = {
        0x21, 0x52, 0xf8, 0xd1, 0x9b, 0x79, 0x1d, 0x24,
        0x45, 0x32, 0x42, 0xe1, 0x5f, 0x2e, 0xab, 0x6c,
        0xb7, 0xcf, 0xfa, 0x7b, 0x6a, 0x5e, 0xd3, 0x00,
        0x97, 0x96, 0x0e, 0x06, 0x98, 0x81, 0xdb, 0x12
    };
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, expected_pub, &r);
    fclose(f);
    ASSERT(ok);
    ASSERT_EQ_INT((int)r.total_events, 48);
    ASSERT_EQ_INT((int)r.valid_events, 48);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_SMELT], 8);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_CRAFT], 8);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_TRANSFER], 8);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_TRADE], 8);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_LEDGER], 8);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_ROCK_DESTROY], 8);
}

void register_signal_verify_tests(void);
void register_signal_verify_tests(void) {
    TEST_SECTION("\n--- Signal Verify (#479 E) ---\n");
    RUN(test_signal_verify_roundtrip);
    RUN(test_signal_verify_byte_tamper);
    RUN(test_signal_verify_signature_corruption);
    RUN(test_signal_verify_mid_log_splice);
    RUN(test_signal_verify_wrong_pubkey_rejected);
    RUN(test_signal_verify_multi_station_independent);
    RUN(test_signal_verify_committed_fixture);
}
