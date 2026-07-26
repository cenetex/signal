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
#include "cargo_receipt.h"
#include "sha256.h"
#include "signal_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
/* popen / pclose are POSIX, gated behind _POSIX_C_SOURCE on glibc.
 * test_harness.h pulled stdio.h before any define would matter, so
 * forward-declare them here for the subprocess test below. Both are
 * used from a single test (test_signal_verify_tower_chain_invariant_*)
 * and don't need to be visible elsewhere. */
extern FILE *popen(const char *command, const char *type);
extern int   pclose(FILE *stream);
#endif

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

TEST(test_signal_verify_operator_post_all_kinds) {
    /* Verify that OPERATOR_POST events with all three kind values can be
     * emitted and verified end-to-end. */
    sv_setup("operator_post_kinds");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50100u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    const char *texts[] = { "Hail message", "Contract flavor", "Rarity tier" };
    for (int kind = 0; kind < 3; kind++) {
        const char *text = texts[kind];
        size_t text_len = strlen(text);
        size_t payload_len = 38 + text_len;
        uint8_t *payload = (uint8_t *)calloc(1, payload_len);
        ASSERT(payload != NULL);
        payload[0] = (uint8_t)kind;
        payload[1] = (kind == 2) ? 2 : 0;  /* tier for RARITY_TIER = rare */
        payload[2] = (uint8_t)(10 + kind);
        payload[3] = 0;
        sha256_bytes((const uint8_t *)text, text_len, &payload[4]);
        payload[36] = (uint8_t)(text_len & 0xFF);
        payload[37] = (uint8_t)((text_len >> 8) & 0xFF);
        memcpy(&payload[38], text, text_len);
        ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                              payload, (uint16_t)payload_len) == (uint64_t)(kind + 1));
        free(payload);
    }

    FILE *f = sv_open_log(&w->stations[0]);
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    bool ok = chain_log_verify_with_pubkey(f, w->stations[0].station_pubkey, &r);
    fclose(f);
    ASSERT(ok);
    ASSERT_EQ_INT((int)r.total_events, 3);
    ASSERT_EQ_INT((int)r.valid_events, 3);
    ASSERT_EQ_INT((int)r.event_type_counts[CHAIN_EVT_OPERATOR_POST], 3);
    sv_teardown();
}

#ifndef _WIN32
static const char *sv_find_signal_verify_bin(void) {
    static const char *candidates[] = {
        "build-test/signal_verify",
        "build-coverage/signal_verify",
        "build/signal_verify",
        "build-verify/signal_verify",
        "./signal_verify",
        "../build-test/signal_verify",
        "../build-coverage/signal_verify",
        "../build/signal_verify",
        "../build-verify/signal_verify",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fclose(f);
        return candidates[i];
    }
    return NULL;
}

static const char *sv_find_signal_chain_assets_bin(void) {
    static const char *candidates[] = {
        "build-test/signal_chain_assets",
        "build-coverage/signal_chain_assets",
        "build/signal_chain_assets",
        "./signal_chain_assets",
        "../build-test/signal_chain_assets",
        "../build-coverage/signal_chain_assets",
        "../build/signal_chain_assets",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fclose(f);
        return candidates[i];
    }
    return NULL;
}

static const char *sv_find_signal_rati_receipt_bin(void) {
    static const char *candidates[] = {
        "build-test/signal_rati_receipt",
        "build-coverage/signal_rati_receipt",
        "build/signal_rati_receipt",
        "./signal_rati_receipt",
        "../build-test/signal_rati_receipt",
        "../build-coverage/signal_rati_receipt",
        "../build/signal_rati_receipt",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fclose(f);
        return candidates[i];
    }
    return NULL;
}

static const char *sv_find_signal_receipt_verify_bin(void) {
    static const char *candidates[] = {
        "build-test/signal_receipt_verify",
        "build-coverage/signal_receipt_verify",
        "build/signal_receipt_verify",
        "./signal_receipt_verify",
        "../build-test/signal_receipt_verify",
        "../build-coverage/signal_receipt_verify",
        "../build/signal_receipt_verify",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fclose(f);
        return candidates[i];
    }
    return NULL;
}

static void sv_hex32(const uint8_t in[32], char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 0x0F];
    }
    out[64] = '\0';
}

TEST(test_signal_verify_tower_chain_invariant_detects_orphan) {
    /* End-to-end test of the new tower_chain_consistent invariant in
     * the signal_verify CLI: emit a chain log containing
     *   TOW(F1), SMELT(F1)  — paired, no violation
     *   TOW(F2), RELEASE(F2) — paired, no violation
     *   TOW(F3)              — orphan, +1 violation
     * Then run the standalone signal_verify binary against the log
     * and grep its --report=json output for the expected violation count.
     *
     * This tests the apply_invariants pipeline end-to-end, which can't
     * be tested via chain_log_verify_with_pubkey directly (the
     * invariant logic lives in tools/signal_verify.c, not the embedded
     * verifier). The subprocess approach is heavier than a unit test
     * but is the only way to exercise the CLI tool's static helpers
     * without a refactor. */
    sv_setup("tower_chain_orphan");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 51000u;
    world_reset(w);
    sv_wipe(w);
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }

    /* Three distinct fragment_pubs. */
    chain_payload_fragment_tow_t tow = {0};
    chain_payload_fragment_release_t rel = {0};
    chain_payload_smelt_t smelt = {0};

    /* Pair 1: TOW + SMELT. */
    for (int b = 0; b < 32; b++) tow.fragment_pub[b] = (uint8_t)(0x10 + b);
    tow.epoch_tick = 100;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_FRAGMENT_TOW,
                          &tow, sizeof(tow)) > 0);
    memset(&smelt, 0, sizeof(smelt));
    for (int b = 0; b < 32; b++) smelt.fragment_pub[b] = (uint8_t)(0x10 + b);
    for (int b = 0; b < 32; b++) smelt.ingot_pub[b]    = (uint8_t)(0xC0 + b);
    smelt.mined_block = 101;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) > 0);

    /* Pair 2: TOW + RELEASE. */
    memset(&tow, 0, sizeof(tow));
    for (int b = 0; b < 32; b++) tow.fragment_pub[b] = (uint8_t)(0x40 + b);
    tow.epoch_tick = 200;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_FRAGMENT_TOW,
                          &tow, sizeof(tow)) > 0);
    for (int b = 0; b < 32; b++) rel.fragment_pub[b] = (uint8_t)(0x40 + b);
    rel.epoch_tick = 201;
    rel.reason = (uint8_t)FRAGMENT_RELEASE_BAND_SNAP;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_FRAGMENT_RELEASE,
                          &rel, sizeof(rel)) > 0);

    /* Orphan: TOW with no matching resolution. */
    memset(&tow, 0, sizeof(tow));
    for (int b = 0; b < 32; b++) tow.fragment_pub[b] = (uint8_t)(0x70 + b);
    tow.epoch_tick = 300;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_FRAGMENT_TOW,
                          &tow, sizeof(tow)) > 0);

    /* Run signal_verify against the log when the standalone binary is
     * available. The coverage job builds only signal_test, so skip there;
     * test-basic builds all BUILD_TESTS_ONLY targets and exercises this. */
    const char *verify_bin = sv_find_signal_verify_bin();
    if (!verify_bin) {
        TEST_WARN("signal_verify binary not built; skipping CLI invariant subprocess check");
        sv_teardown();
        return;
    }

    char log_path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, log_path, sizeof(log_path)));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s --report=json %s 2>/dev/null", verify_bin, log_path);
    FILE *p = popen(cmd, "r");
    ASSERT(p != NULL);
    char output[4096] = {0};
    size_t got = fread(output, 1, sizeof(output) - 1, p);
    pclose(p);
    ASSERT(got > 0);

    /* The orphan TOW must surface as exactly 1 in the JSON. */
    ASSERT(strstr(output, "\"tower_chain_violations\":1") != NULL);
    /* And the per-type counts must reflect the 3 TOWs and 1 RELEASE. */
    ASSERT(strstr(output, "\"FRAGMENT_TOW\":3") != NULL);
    ASSERT(strstr(output, "\"FRAGMENT_RELEASE\":1") != NULL);

    sv_teardown();
}

TEST(test_signal_chain_assets_lineage_cli_prints_craft_tree) {
    sv_setup("chain_assets_lineage");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 52000u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    chain_payload_smelt_t smelt_a = {0};
    chain_payload_smelt_t smelt_b = {0};
    chain_payload_craft_t craft = {0};
    for (int b = 0; b < 32; b++) {
        smelt_a.fragment_pub[b] = (uint8_t)(0x10 + b);
        smelt_a.ingot_pub[b] = (uint8_t)(0x40 + b);
        smelt_b.fragment_pub[b] = (uint8_t)(0x80 + b);
        smelt_b.ingot_pub[b] = (uint8_t)(0xB0 + b);
        craft.output_pub[b] = (uint8_t)(0xE0 + b);
    }
    smelt_a.prefix_class = INGOT_PREFIX_RATI;
    smelt_a.mined_block = 111;
    smelt_b.prefix_class = INGOT_PREFIX_K;
    smelt_b.mined_block = 222;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                          &smelt_a, sizeof(smelt_a)) == 1);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                          &smelt_b, sizeof(smelt_b)) == 2);

    craft.recipe_id = RECIPE_FRAME_BASIC;
    craft.input_count = 2;
    memcpy(craft.input_pubs[0], smelt_a.ingot_pub, 32);
    memcpy(craft.input_pubs[1], smelt_b.ingot_pub, 32);
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_CRAFT,
                          &craft, sizeof(craft)) == 3);

    chain_payload_construction_t construction = {0};
    memcpy(construction.cargo_pub, craft.output_pub, 32);
    construction.target_kind = CONSTRUCTION_TARGET_MODULE;
    construction.station_index = 0;
    construction.module_index = 7;
    construction.module_type = MODULE_SIGNAL_RELAY;
    construction.commodity = COMMODITY_FRAME;
    construction.target_id = 0;
    construction.contributed_units = 1.0f;
    construction.progress_after = 0.25f;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_CONSTRUCTION,
                          &construction, sizeof(construction)) == 4);

    chain_payload_construction_t station_construction = {0};
    memcpy(station_construction.cargo_pub, smelt_b.ingot_pub, 32);
    station_construction.target_kind = CONSTRUCTION_TARGET_STATION;
    station_construction.station_index = 0;
    station_construction.module_index = 0xff;
    station_construction.module_type = 0xff;
    station_construction.commodity = COMMODITY_FRAME;
    station_construction.target_id = 0;
    station_construction.contributed_units = 1.0f;
    station_construction.progress_after = 0.50f;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_CONSTRUCTION,
                          &station_construction, sizeof(station_construction)) == 5);

    const char *asset_bin = sv_find_signal_chain_assets_bin();
    if (!asset_bin) {
        TEST_WARN("signal_chain_assets binary not built; skipping CLI lineage subprocess check");
        sv_teardown();
        return;
    }

    char log_path[256];
    char output_hex[65];
    char input_hex[65];
    char fragment_hex[65];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, log_path, sizeof(log_path)));
    sv_hex32(craft.output_pub, output_hex);
    sv_hex32(smelt_a.ingot_pub, input_hex);
    sv_hex32(smelt_a.fragment_pub, fragment_hex);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s --no-signatures --lineage=%s %s 2>/dev/null",
             asset_bin, output_hex, log_path);
    FILE *p = popen(cmd, "r");
    ASSERT(p != NULL);
    char output[8192] = {0};
    size_t got = fread(output, 1, sizeof(output) - 1, p);
    pclose(p);
    ASSERT(got > 0);

    ASSERT(strstr(output, "Signal lineage") != NULL);
    ASSERT(strstr(output, "frame ") != NULL);
    ASSERT(strstr(output, "recipe=1:frame_basic") != NULL);
    ASSERT(strstr(output, input_hex) != NULL);
    ASSERT(strstr(output, fragment_hex) != NULL);
    ASSERT(strstr(output, "prefix=RATi") != NULL);
    ASSERT(strstr(output, "construction:") != NULL);
    ASSERT(strstr(output, "target=module") != NULL);
    ASSERT(strstr(output, "module_index=7") != NULL);
    ASSERT(strstr(output, "progress_after=0.250") != NULL);

    snprintf(cmd, sizeof(cmd), "%s --no-signatures --built-from=module:0:7 %s 2>/dev/null",
             asset_bin, log_path);
    p = popen(cmd, "r");
    ASSERT(p != NULL);
    memset(output, 0, sizeof(output));
    got = fread(output, 1, sizeof(output) - 1, p);
    pclose(p);
    ASSERT(got > 0);

    ASSERT(strstr(output, "Signal infrastructure lineage") != NULL);
    ASSERT(strstr(output, "target: module station_index=0 module_index=7") != NULL);
    ASSERT(strstr(output, "contributors: 1") != NULL);
    ASSERT(strstr(output, output_hex) != NULL);
    ASSERT(strstr(output, "recipe=1:frame_basic") != NULL);
    ASSERT(strstr(output, input_hex) != NULL);
    ASSERT(strstr(output, "prefix=RATi") != NULL);

    snprintf(cmd, sizeof(cmd), "%s --no-signatures --built-from=station:0 %s 2>/dev/null",
             asset_bin, log_path);
    p = popen(cmd, "r");
    ASSERT(p != NULL);
    memset(output, 0, sizeof(output));
    got = fread(output, 1, sizeof(output) - 1, p);
    pclose(p);
    ASSERT(got > 0);

    ASSERT(strstr(output, "Signal infrastructure lineage") != NULL);
    ASSERT(strstr(output, "target: station station_index=0") != NULL);
    ASSERT(strstr(output, "contributors: 1") != NULL);
    ASSERT(strstr(output, "prefix=K") != NULL);
    sv_teardown();
}

TEST(test_signal_rati_receipt_cli_emits_smelt_receipt) {
    sv_setup("rati_receipt");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 50002u;
    world_reset(w);
    sv_wipe(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    uint8_t fracture_seed[32];
    uint8_t claimant_pubkey[32];
    uint32_t burst_nonce = 0;
    mining_grade_t grade = MINING_GRADE_COMMON;
    for (int b = 0; b < 32; b++) {
        fracture_seed[b] = (uint8_t)(0x20 + b);
        claimant_pubkey[b] = (uint8_t)(0xA0 + b);
    }
    for (; burst_nonce < 65535u; burst_nonce++) {
        mining_keypair_t kp;
        char callsign[8];
        mining_keypair_derive(fracture_seed, claimant_pubkey,
                              burst_nonce, &kp);
        mining_callsign_from_pubkey(kp.pub, callsign);
        grade = mining_classify_base58(callsign);
        if (grade == MINING_GRADE_RATI) break;
    }
    ASSERT(grade == MINING_GRADE_RATI);

    chain_payload_claim_fragment_t claim = {0};
    memcpy(claim.fracture_seed, fracture_seed, 32);
    memcpy(claim.claimant_pubkey, claimant_pubkey, 32);
    mining_fragment_pub_compute(fracture_seed, claimant_pubkey,
                                burst_nonce, claim.fragment_pub);
    claim.fracture_id = 1234;
    claim.burst_nonce = burst_nonce;
    claim.burst_cap = 65535u;
    claim.grade = (uint8_t)grade;
    claim.asteroid_slot = 7;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_CLAIM_FRAGMENT,
                          &claim, sizeof(claim)) == 1);

    chain_payload_smelt_t smelt = {0};
    for (int b = 0; b < 32; b++) {
        smelt.ingot_pub[b] = (uint8_t)(0x60 + b);
    }
    memcpy(smelt.fragment_pub, claim.fragment_pub, 32);
    smelt.prefix_class = INGOT_PREFIX_RATI;
    smelt.mined_block = 777;
    ASSERT(chain_log_emit(w, &w->stations[0], CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) == 2);

    const char *receipt_bin = sv_find_signal_rati_receipt_bin();
    if (!receipt_bin) {
        TEST_WARN("signal_rati_receipt binary not built; skipping CLI receipt subprocess check");
        sv_teardown();
        return;
    }

    char log_path[256];
    char fragment_hex[65];
    char ingot_hex[65];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey, log_path, sizeof(log_path)));
    sv_hex32(smelt.fragment_pub, fragment_hex);
    sv_hex32(smelt.ingot_pub, ingot_hex);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s --min-prefix=RATi %s 2>/dev/null",
             receipt_bin, log_path);
    FILE *p = popen(cmd, "r");
    ASSERT(p != NULL);
    char output[8192] = {0};
    size_t got = fread(output, 1, sizeof(output) - 1, p);
    pclose(p);
    ASSERT(got > 0);

    ASSERT(strstr(output, "\"schema\":\"signal.rati_mining_receipts.v1\"") != NULL);
    ASSERT(strstr(output, "\"receipt_count\":1") != NULL);
    ASSERT(strstr(output, "\"version\":\"rati_mining_receipt_v1\"") != NULL);
    ASSERT(strstr(output, "\"kind\":\"CHAIN_EVT_SMELT\"") != NULL);
    ASSERT(strstr(output, "\"kind\":\"CHAIN_EVT_CLAIM_FRAGMENT\"") != NULL);
    ASSERT(strstr(output, "\"prefix_class\":\"RATi\"") != NULL);
    ASSERT(strstr(output, "\"grade\":\"RATi\"") != NULL);
    ASSERT(strstr(output, "\"grade_verified\":true") != NULL);
    ASSERT(strstr(output, fragment_hex) != NULL);
    ASSERT(strstr(output, ingot_hex) != NULL);

    sv_teardown();
}

TEST(test_signal_receipt_verify_cli_semantic_vectors) {
    const char *verify_bin = sv_find_signal_receipt_verify_bin();
    if (!verify_bin) {
        TEST_WARN("signal_receipt_verify binary not built; skipping CLI vectors");
        return;
    }

    uint8_t seed[32];
    uint8_t cargo_pub[32];
    for (int i = 0; i < 32; i++) {
        seed[i] = (uint8_t)(0x21 + i);
        cargo_pub[i] = (uint8_t)(0x71 + i);
    }
    uint8_t authority[32], secret[64], origin_hash[32];
    signal_crypto_keypair_from_seed(seed, authority, secret);
    sha256_bytes(cargo_pub, sizeof(cargo_pub), origin_hash);

    cargo_receipt_t receipt = {0};
    memcpy(receipt.cargo_pub, cargo_pub, 32);
    memcpy(receipt.authoring_station, authority, 32);
    memset(receipt.recipient_pubkey, 0x44, 32);
    receipt.event_id = 8;
    receipt.epoch = 12;
    memcpy(receipt.prev_receipt_hash, origin_hash, 32);
    uint8_t unsigned_receipt[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(&receipt, unsigned_receipt);
    signal_crypto_sign(receipt.signature, unsigned_receipt,
                       sizeof(unsigned_receipt), secret);

    char chain_path[256];
    snprintf(chain_path, sizeof(chain_path), "%s_receipt-trust.bin",
             TMP("receipt"));
    uint8_t packed[CARGO_RECEIPT_SIZE];
    cargo_receipt_pack(&receipt, packed);
    FILE *chain_file = fopen(chain_path, "wb");
    ASSERT(chain_file != NULL);
    ASSERT(fwrite(packed, 1, sizeof(packed), chain_file) == sizeof(packed));
    ASSERT(fclose(chain_file) == 0);

    char cargo_hex[65], origin_hex[65], authority_hex[65];
    sv_hex32(cargo_pub, cargo_hex);
    sv_hex32(origin_hash, origin_hex);
    sv_hex32(authority, authority_hex);
    char base[1536];
    snprintf(base, sizeof(base),
             "%s --report=json --cargo-pub=%s --origin-event=smelt "
             "--origin-hash=%s --origin-authority=%s "
             "--origin-event-id=7 --origin-epoch=11",
             verify_bin, cargo_hex, origin_hex, authority_hex);

    static const struct {
        const char *policy;
        unsigned status_code;
        bool accepted;
        const char *semantic;
    } policy_vectors[] = {
        {"current", 0, true, "accepted/trusted"},
        {"rotated", 1, true, "accepted/rotated"},
        {"unknown", 9, false, "rejected/unknown"},
        {"untrusted", 10, false, "rejected/untrusted"},
        {"revoked", 11, false, "rejected/revoked"},
    };
    for (size_t i = 0;
         i < sizeof(policy_vectors) / sizeof(policy_vectors[0]); i++) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "%s --authority-trust=%s %s 2>/dev/null",
                 base, policy_vectors[i].policy, chain_path);
        FILE *p = popen(cmd, "r");
        ASSERT(p != NULL);
        char output[8192] = {0};
        ASSERT(fread(output, 1, sizeof(output) - 1, p) > 0);
        (void)pclose(p);
        char status_field[64];
        snprintf(status_field, sizeof(status_field),
                 "\"status_code\":%u", policy_vectors[i].status_code);
        ASSERT(strstr(output,
                      "\"schema\":\"signal.cargo_receipt_trust.v1\"") != NULL);
        ASSERT(strstr(output, status_field) != NULL);
        ASSERT(strstr(output, policy_vectors[i].accepted
                                 ? "\"accepted\":true"
                                 : "\"accepted\":false") != NULL);
        ASSERT(strstr(output, policy_vectors[i].semantic) != NULL);
        ASSERT(strstr(output, cargo_hex) != NULL);
        ASSERT(strstr(output, origin_hex) != NULL);
        ASSERT(strstr(output, authority_hex) != NULL);
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "%s --report=json --cargo-pub=%s --origin-event=missing "
             "--authority-trust=current %s 2>/dev/null",
             verify_bin, cargo_hex, chain_path);
    FILE *p = popen(cmd, "r");
    ASSERT(p != NULL);
    char output[8192] = {0};
    ASSERT(fread(output, 1, sizeof(output) - 1, p) > 0);
    (void)pclose(p);
    ASSERT(strstr(output, "\"status_code\":4") != NULL);
    ASSERT(strstr(output, "rejected/no-origin") != NULL);
    ASSERT(strstr(output, "\"origin_hash\":null") != NULL);

    receipt.signature[0] ^= 0x01u;
    cargo_receipt_pack(&receipt, packed);
    chain_file = fopen(chain_path, "wb");
    ASSERT(chain_file != NULL);
    ASSERT(fwrite(packed, 1, sizeof(packed), chain_file) == sizeof(packed));
    ASSERT(fclose(chain_file) == 0);
    snprintf(cmd, sizeof(cmd), "%s --authority-trust=current %s 2>/dev/null",
             base, chain_path);
    p = popen(cmd, "r");
    ASSERT(p != NULL);
    memset(output, 0, sizeof(output));
    ASSERT(fread(output, 1, sizeof(output) - 1, p) > 0);
    (void)pclose(p);
    ASSERT(strstr(output, "\"status_code\":3") != NULL);
    ASSERT(strstr(output, "\"chain_code\":3") != NULL);
    ASSERT(strstr(output, "rejected/witness") != NULL);
    memset(secret, 0, sizeof(secret));
}
#endif

void register_signal_verify_tests(void);
void register_signal_verify_tests(void) {
    TEST_SECTION("\n--- Signal Verify (#479 E) ---\n");
    RUN(test_signal_verify_roundtrip);
    RUN(test_signal_verify_byte_tamper);
    RUN(test_signal_verify_signature_corruption);
    RUN(test_signal_verify_mid_log_splice);
    RUN(test_signal_verify_wrong_pubkey_rejected);
    RUN(test_signal_verify_multi_station_independent);
    RUN(test_signal_verify_operator_post_all_kinds);
    RUN(test_signal_verify_committed_fixture);
#ifndef _WIN32
    RUN(test_signal_verify_tower_chain_invariant_detects_orphan);
    RUN(test_signal_chain_assets_lineage_cli_prints_craft_tree);
    RUN(test_signal_rati_receipt_cli_emits_smelt_receipt);
    RUN(test_signal_receipt_verify_cli_semantic_vectors);
#endif
}
