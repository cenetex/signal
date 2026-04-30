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
#include "station_authority.h"
#include "sim_asteroid.h"
#include "game_sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each test sets a unique chain dir under TMP() so concurrent test
 * shards don't trample each other and so a previous run's residue
 * doesn't poison the current pass. */
static void chain_test_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_chain_%s", TMP("clog"), suffix);
    chain_log_set_dir(path);
}

static void chain_test_teardown(void) {
    chain_log_set_dir(NULL);
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

TEST(test_chain_log_smelt_emits_event) {
    /* End-to-end: drop ferrite ore into Prospect's hopper, run the
     * sim until the refinery mints an ingot, then verify Prospect's
     * chain log gained an EVT_SMELT for it. */
    chain_test_setup("smelt_e2e");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9007u;
    world_reset(w);
    chain_test_wipe_logs(w);
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    /* Force a guaranteed smelt: dump ore directly into Prospect's
     * inventory cache — refinery production consumes from here. */
    w->stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 5.0f;
    /* Step the sim long enough for the refinery to consume an integer
     * crossing's worth of ore (REFINERY_BASE_SMELT_RATE is small).
     * 30 seconds at SIM_DT is well above the worst-case smelt cycle. */
    for (int i = 0; i < (int)(30.0f / SIM_DT); i++)
        world_sim_step(w, SIM_DT);

    /* Walk Prospect's log; if a smelt happened it must verify. */
    uint64_t walked = 0;
    ASSERT(chain_log_verify(&w->stations[0], &walked, NULL));
    /* At least one event must have landed (the refinery WILL produce
     * an ingot in 30 s with 5.0 input ore). */
    ASSERT(walked >= 1);
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

void register_chain_log_tests(void);
void register_chain_log_tests(void) {
    TEST_SECTION("\n--- Chain Log (#479 C) ---\n");
    RUN(test_chain_log_emit_and_verify);
    RUN(test_chain_log_chain_linkage);
    RUN(test_chain_log_tampered_event_detected);
    RUN(test_chain_log_wrong_station_signature_rejected);
    RUN(test_chain_log_save_load_continuity);
    RUN(test_chain_log_cross_station_independent);
    RUN(test_chain_log_smelt_emits_event);
    RUN(test_chain_log_rock_destroy_emits_event);
}
