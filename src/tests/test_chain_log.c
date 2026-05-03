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
#include "sha256.h"

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

TEST(test_chain_log_smelt_emits_event_fragment_path) {
    /* The richer smelt path: spawn a physical fragment between a
     * furnace and an adjacent module, run the sim until the beam
     * smelts it, then verify the chain log gained an EVT_SMELT whose
     * fragment_pub matches the consumed asteroid's record. This is
     * what the (suspected-dead) hopper-float path could never do —
     * fragment-attributed lineage on the smelt event itself. */
    chain_test_setup("smelt_fragment");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    w->rng = 9100u;
    world_reset(w);
    chain_test_wipe_logs(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    w->stations[0].chain_event_count = 0;
    memset(w->stations[0].chain_last_hash, 0, 32);

    /* Find Prospect's furnace + an adjacent-ring module to anchor
     * the smelt midpoint. */
    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w->stations[0].module_count; m++) {
        if (w->stations[0].modules[m].type == MODULE_FURNACE) furnace_idx = m;
        if (w->stations[0].modules[m].type == MODULE_HOPPER) silo_idx = m;
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

    /* Walk the on-disk log and confirm at least one EVT_SMELT carries
     * a non-zero fragment_pub — that's the gap the fragment-tow path
     * just closed. The hopper-float path emits with fragment_pub = 0,
     * so any non-zero is positive proof the fragment-tow path fired. */
    char path[256];
    ASSERT(chain_log_path_for(w->stations[0].station_pubkey,
                              path, sizeof(path)));
    FILE *fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    bool saw_fragment_attributed = false;
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
                break;
            }
        } else {
            fseek(fp, plen, SEEK_CUR);
        }
    }
    fclose(fp);
    ASSERT(saw_fragment_attributed);

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
     * condition (server restart). We deliberately do NOT call
     * world_reset on a fresh world: that resets seeded stations'
     * chain logs (see chain_log_reset in world_reset, game_sim.c) and
     * would wipe the on-disk log we just wrote. world_save/load
     * preserves chain_event_count and chain_last_hash; the on-disk
     * .log file is untouched. */
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
    RUN(test_chain_log_smelt_emits_event_fragment_path);
    RUN(test_chain_log_rock_destroy_emits_event);
    RUN(test_chain_log_operator_post_emit);
    RUN(test_chain_log_operator_post_all_kinds);
    RUN(test_chain_log_operator_post_replay_determinism);
    RUN(test_chain_log_operator_post_text_tamper);
    RUN(test_chain_log_seed_rarity_tiers_have_real_content);
}
