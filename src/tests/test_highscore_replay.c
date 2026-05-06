/*
 * test_highscore_replay.c — chain-log-as-canonical-leaderboard tests.
 *
 * Covers Phase 1 (death events round-trip through chain log into the
 * highscore view) and Phase 2 (world_id / build_id / killed_by columns
 * are projected correctly when BUILD_INFO + WORLD_INFO operator posts
 * precede the deaths).
 */
#include "test_harness.h"

#include "chain_log.h"
#include "highscore.h"
#include "station_authority.h"
#include "sha256.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#  include <dirent.h>
#else
#  include <direct.h>
#endif

static void hs_test_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_hs_%s", TMP("hs"), suffix);
    chain_log_set_dir(path);
    /* Best-effort mkdir; chain_log_emit also ensures the directory. */
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0775);
#endif
    /* Wipe any residue from previous runs of the same test. */
#ifndef _WIN32
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            char full[512];
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            remove(full);
        }
        closedir(d);
    }
#endif
}

static void hs_test_teardown(void) {
    chain_log_set_dir(NULL);
}

/* Emit a CHAIN_EVT_DEATH on station 0 with the given fields. */
static void emit_death(world_t *w,
                       const char *callsign,
                       const uint8_t session_token[8],
                       const uint8_t killer_token[8],
                       float credits_earned) {
    chain_payload_death_t d;
    memset(&d, 0, sizeof(d));
    if (session_token) memcpy(d.victim_session_token, session_token, 8);
    if (callsign) {
        size_t n = strlen(callsign);
        if (n > 8) n = 8;
        memcpy(d.victim_callsign, callsign, n);
    }
    if (killer_token) memcpy(d.killer_token, killer_token, 8);
    d.credits_earned = credits_earned;
    d.epoch_tick = (uint64_t)(w->time * 120.0);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_DEATH,
                         &d, (uint16_t)sizeof(d));
}

/* Emit a CHAIN_EVT_DEATH that already carries killed_by_callsign — the
 * server's normal path resolves the killer at emit time. */
static void emit_death_with_killer_callsign(world_t *w,
                                            const char *callsign,
                                            const uint8_t session_token[8],
                                            const uint8_t killer_token[8],
                                            const char *killer_callsign,
                                            float credits_earned) {
    chain_payload_death_t d;
    memset(&d, 0, sizeof(d));
    if (session_token) memcpy(d.victim_session_token, session_token, 8);
    if (callsign) {
        size_t n = strlen(callsign);
        if (n > 8) n = 8;
        memcpy(d.victim_callsign, callsign, n);
    }
    if (killer_token) memcpy(d.killer_token, killer_token, 8);
    if (killer_callsign) {
        size_t n = strlen(killer_callsign);
        if (n > 8) n = 8;
        memcpy(d.killed_by_callsign, killer_callsign, n);
    }
    d.credits_earned = credits_earned;
    d.epoch_tick = (uint64_t)(w->time * 120.0);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_DEATH,
                         &d, (uint16_t)sizeof(d));
}

/* Emit a BUILD_INFO operator post (kind=3, text=hex SHA). */
static void emit_build_info(world_t *w, const char *hex) {
    size_t hl = strlen(hex);
    if (hl > 64) hl = 64;
    uint8_t payload[38 + 64];
    memset(payload, 0, sizeof(payload));
    payload[0] = 3;
    sha256_bytes((const uint8_t *)hex, hl, &payload[4]);
    payload[36] = (uint8_t)(hl & 0xFF);
    payload[37] = (uint8_t)((hl >> 8) & 0xFF);
    memcpy(&payload[38], hex, hl);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                         payload, (uint16_t)(38 + hl));
}

/* Emit a WORLD_INFO operator post (kind=4, text=belt_seed LE |
 * world_seq LE | hex). */
static void emit_world_info(world_t *w, uint32_t seed_u32,
                            uint32_t world_seq, const char *hex) {
    size_t hl = strlen(hex);
    if (hl > 56) hl = 56;
    uint8_t text[72];
    text[0] = (uint8_t)(seed_u32 & 0xFF);
    text[1] = (uint8_t)((seed_u32 >> 8) & 0xFF);
    text[2] = (uint8_t)((seed_u32 >> 16) & 0xFF);
    text[3] = (uint8_t)((seed_u32 >> 24) & 0xFF);
    text[4] = (uint8_t)(world_seq & 0xFF);
    text[5] = (uint8_t)((world_seq >> 8) & 0xFF);
    text[6] = (uint8_t)((world_seq >> 16) & 0xFF);
    text[7] = (uint8_t)((world_seq >> 24) & 0xFF);
    memcpy(&text[8], hex, hl);
    size_t tl = 8 + hl;
    uint8_t payload[38 + 72];
    memset(payload, 0, sizeof(payload));
    payload[0] = 4;
    sha256_bytes(text, tl, &payload[4]);
    payload[36] = (uint8_t)(tl & 0xFF);
    payload[37] = (uint8_t)((tl >> 8) & 0xFF);
    memcpy(&payload[38], text, tl);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_OPERATOR_POST,
                         payload, (uint16_t)(38 + tl));
}

TEST(test_chain_event_death_payload_size) {
    /* Compile-time pin already in chain_log.h; runtime memcmp roundtrip
     * confirms the layout matches what we serialize. */
    chain_payload_death_t a;
    memset(&a, 0, sizeof(a));
    for (int i = 0; i < 32; i++) a.victim_pubkey[i] = (uint8_t)(i + 1);
    for (int i = 0; i < 8; i++) a.victim_session_token[i] = (uint8_t)(i + 100);
    memcpy(a.victim_callsign, "ABCD-123", 8);
    for (int i = 0; i < 8; i++) a.killer_token[i] = (uint8_t)(i + 200);
    a.cause = 2;
    a.epoch_tick = 0xDEADBEEFCAFEBABEULL;
    a.credits_earned = 1234.5f;
    a.credits_spent = 500.0f;
    a.ore_mined = 78.25f;
    a.asteroids_fractured = 17;

    uint8_t buf[sizeof(a)];
    memcpy(buf, &a, sizeof(a));
    chain_payload_death_t b;
    memcpy(&b, buf, sizeof(b));

    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
    ASSERT(sizeof(chain_payload_death_t) == 96);
}

TEST(test_highscore_replay_from_chain) {
    hs_test_setup("replay_basic");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 7700u;
    world_reset(w);

    uint8_t tok_a[8] = { 0xAA, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t tok_b[8] = { 0xBB, 1, 2, 3, 4, 5, 6, 7 };
    emit_death(w, "ALPHA-1", tok_a, NULL, 100.0f);
    emit_death(w, "BRAVO-2", tok_b, NULL, 250.0f);
    emit_death(w, "ALPHA-1", tok_a, NULL, 150.0f);  /* upgrade */

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 2);
    /* BRAVO-2 leads (250), ALPHA-1's best is 150. */
    ASSERT(memcmp(t.entries[0].callsign, "BRAVO-2", 7) == 0);
    ASSERT_EQ_FLOAT(t.entries[0].credits_earned, 250.0f, 0.001f);
    ASSERT(memcmp(t.entries[1].callsign, "ALPHA-1", 7) == 0);
    ASSERT_EQ_FLOAT(t.entries[1].credits_earned, 150.0f, 0.001f);

    hs_test_teardown();
}

TEST(test_highscores_survive_world_reset) {
    hs_test_setup("survive_reset");

    /* World A: belt_seed=11111. */
    WORLD_HEAP wa = calloc(1, sizeof(world_t));
    ASSERT(wa != NULL);
    wa->rng = 11111u;
    world_reset(wa);
    /* Emit WORLD_INFO so the entry carries an explicit world_id. */
    emit_world_info(wa, wa->belt_seed, 100u, "aabbccdd");
    uint8_t tok_a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    emit_death(wa, "AYE", tok_a, NULL, 500.0f);

    /* World B: belt_seed=22222 → distinct station-0 pubkey → distinct
     * chain log file on disk. World A's file lingers as an orphan. */
    WORLD_HEAP wb = calloc(1, sizeof(world_t));
    ASSERT(wb != NULL);
    wb->rng = 22222u;
    world_reset(wb);
    emit_world_info(wb, wb->belt_seed, 200u, "11223344");
    uint8_t tok_b[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    emit_death(wb, "BEE", tok_b, NULL, 800.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 2);
    bool found_aye = false, found_bee = false;
    uint32_t aye_world = 0, bee_world = 0;
    for (int i = 0; i < t.count; i++) {
        if (memcmp(t.entries[i].callsign, "AYE", 3) == 0) {
            found_aye = true;
            aye_world = t.entries[i].world_id;
        }
        if (memcmp(t.entries[i].callsign, "BEE", 3) == 0) {
            found_bee = true;
            bee_world = t.entries[i].world_id;
        }
    }
    ASSERT(found_aye && found_bee);
    ASSERT(aye_world == 11111u);
    ASSERT(bee_world == 22222u);
    ASSERT(aye_world != bee_world);

    hs_test_teardown();
}

TEST(test_killer_callsign_resolved) {
    hs_test_setup("killer_resolve");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 8800u;
    world_reset(w);

    /* Player A dies first (so the map collects A's session_token →
     * "AAAAA-1" mapping). Then player B dies with killer_token = A's
     * token; replay should resolve killed_by to "AAAAA-1". */
    uint8_t tok_a[8] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8 };
    uint8_t tok_b[8] = { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8 };
    emit_death(w, "AAAAA-1", tok_a, NULL,  50.0f);
    emit_death(w, "BBBBB-2", tok_b, tok_a, 200.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 2);
    /* BBBBB-2 leads (200). Its killed_by should be "AAAAA-1". */
    ASSERT(memcmp(t.entries[0].callsign, "BBBBB-2", 7) == 0);
    char kb[9] = {0};
    memcpy(kb, t.entries[0].killed_by, 8);
    ASSERT_STR_EQ(kb, "AAAAA-1");

    /* AAAAA-1 has no killer; killed_by stays zero. */
    static const uint8_t zero[8] = {0};
    ASSERT(memcmp(t.entries[1].killed_by, zero, 8) == 0);

    hs_test_teardown();
}

TEST(test_build_info_tagged) {
    hs_test_setup("build_info");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9900u;
    world_reset(w);

    /* Emit BUILD_INFO BEFORE the death — the replay walker maintains
     * a build cursor that tags each subsequent death event. */
    emit_build_info(w, "deadbeef");
    uint8_t tok[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    emit_death(w, "CHARLIE", tok, NULL, 999.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 1);
    /* "deadbeef" → 0xdeadbeef in the build_id u32. */
    ASSERT(t.entries[0].build_id == 0xdeadbeefu);

    hs_test_teardown();
}

TEST(test_killer_resolved_at_emit) {
    /* Death events emitted by the server now carry killed_by_callsign in
     * the payload — replay reads it directly with no fallback map
     * lookup. Confirm by emitting a single DEATH whose killer never
     * dies (so the fallback map can't help) and asserting the killer
     * callsign still surfaces. */
    hs_test_setup("killer_at_emit");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 1212u;
    world_reset(w);

    uint8_t victim_tok[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    uint8_t killer_tok[8] = { 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8 };
    emit_death_with_killer_callsign(w, "VICT-1", victim_tok, killer_tok,
                                    "KILLR-9", 333.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    ASSERT_EQ_INT(t.count, 1);
    char kb[9] = {0};
    memcpy(kb, t.entries[0].killed_by, 8);
    ASSERT_STR_EQ(kb, "KILLR-9");

    hs_test_teardown();
}

TEST(test_most_recent_world_wins) {
    hs_test_setup("recent_world_wins");

    /* World A: an older world (smaller world_seq). Emit a high-credit
     * run for ALPHA-9. */
    WORLD_HEAP wa = calloc(1, sizeof(world_t));
    ASSERT(wa != NULL);
    wa->rng = 31415u;
    world_reset(wa);
    emit_world_info(wa, wa->belt_seed, 100u, "aabbccdd");
    uint8_t tok_a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    emit_death(wa, "ALPHA-9", tok_a, NULL, 5000.0f);

    /* World B: newer world (greater world_seq). ALPHA-9 dies for less
     * but the newer world should still take precedence. */
    WORLD_HEAP wb = calloc(1, sizeof(world_t));
    ASSERT(wb != NULL);
    wb->rng = 27182u;
    world_reset(wb);
    emit_world_info(wb, wb->belt_seed, 200u, "11223344");
    uint8_t tok_b[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    emit_death(wb, "ALPHA-9", tok_b, NULL, 100.0f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());

    /* Exactly one ALPHA-9 row, from world B (newer), even though its
     * credits are lower — newer-world-wins. */
    ASSERT_EQ_INT(t.count, 1);
    ASSERT(memcmp(t.entries[0].callsign, "ALPHA-9", 7) == 0);
    ASSERT_EQ_FLOAT(t.entries[0].credits_earned, 100.0f, 0.001f);
    ASSERT(t.entries[0].world_seq == 200u);
    ASSERT(t.entries[0].world_id == wb->belt_seed);

    hs_test_teardown();
}

TEST(test_world_seed_persists_across_restart) {
    /* world.sav round-trip preserves belt_seed and world_seq so a
     * normal server restart keeps the same belt layout, station Ed25519
     * pubkeys, and leaderboard ordering. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 4242u;
    world_reset(w);
    w->world_seq = 0xDEADBEEFu;
    uint32_t expected_seed = w->belt_seed;
    uint32_t expected_seq = w->world_seq;
    ASSERT(world_save(w, TMP("ws_seed_persist.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    loaded->rng = 999999u; /* deliberately different to prove load wins */
    world_reset(loaded);
    ASSERT(world_load(loaded, TMP("ws_seed_persist.sav")));

    ASSERT(loaded->belt_seed == expected_seed);
    ASSERT(loaded->world_seq == expected_seq);

    remove(TMP("ws_seed_persist.sav"));
}

TEST(test_corrupt_chain_log_rejected_by_replay) {
    /* P1 regression: highscore_replay_from_chain must verify each
     * chain log against the pubkey decoded from its filename before
     * projecting any DEATH events. A truncated or tampered file —
     * here, an arbitrary blob written under a base58-named filename
     * that does not link to a valid signed chain — must contribute
     * zero entries to the leaderboard view. */
    hs_test_setup("corrupt_log");

    /* Drop a file named like a chain log but containing junk bytes.
     * Filename uses the chain dir established by hs_test_setup. */
    const char *dir = chain_log_get_dir();
    char path[512];
    snprintf(path, sizeof(path),
             "%s/3F5qRPtKg8GhGNnbd3qCj6nVJxWsGxq7pvH84okYLAqf.log", dir);
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    /* Write a header-shaped run of zeros big enough to look like one
     * event, plus a payload-len prefix. The signature won't verify. */
    uint8_t junk[200];
    memset(junk, 0, sizeof(junk));
    fwrite(junk, sizeof(junk), 1, f);
    fclose(f);

    highscore_table_t t;
    highscore_replay_from_chain(&t, dir);
    ASSERT_EQ_INT(t.count, 0);

    hs_test_teardown();
}

TEST(test_legacy_death_payload_replays) {
    /* P2 regression: 88-byte DEATH events written before the
     * killed_by_callsign field was added must still feed the
     * leaderboard view. Replay accepts payloads down to
     * offsetof(killed_by_callsign) and falls back to the victim-token
     * map for killer attribution. */
    hs_test_setup("legacy_payload");

    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 88880u;
    world_reset(w);

    /* Build a payload truncated at the legacy boundary (88 bytes —
     * everything before killed_by_callsign). */
    chain_payload_death_t d;
    memset(&d, 0, sizeof(d));
    uint8_t vt[8] = { 'L','E','G','A','C','Y','-','1' };
    memcpy(d.victim_session_token, vt, 8);
    memcpy(d.victim_callsign, "LEGACY-1", 8);
    d.credits_earned = 4242.0f;
    d.epoch_tick = 1234ULL;
    uint16_t legacy_len = (uint16_t)offsetof(chain_payload_death_t, killed_by_callsign);
    ASSERT_EQ_INT(legacy_len, 88);
    (void)chain_log_emit(w, &w->stations[0], CHAIN_EVT_DEATH, &d, legacy_len);

    highscore_table_t t;
    highscore_replay_from_chain(&t, chain_log_get_dir());
    ASSERT_EQ_INT(t.count, 1);
    ASSERT(memcmp(t.entries[0].callsign, "LEGACY-1", 8) == 0);
    ASSERT_EQ_FLOAT(t.entries[0].credits_earned, 4242.0f, 0.001f);

    hs_test_teardown();
}

void register_highscore_replay_tests(void) {
    TEST_SECTION("\nhighscore replay tests:\n");
    RUN(test_chain_event_death_payload_size);
    RUN(test_highscore_replay_from_chain);
    RUN(test_highscores_survive_world_reset);
    RUN(test_killer_callsign_resolved);
    RUN(test_killer_resolved_at_emit);
    RUN(test_most_recent_world_wins);
    RUN(test_world_seed_persists_across_restart);
    RUN(test_build_info_tagged);
    RUN(test_corrupt_chain_log_rejected_by_replay);
    RUN(test_legacy_death_payload_replays);
}
