/*
 * test_anchor.c — destroyed-rocks epoch anchor writer (#285 slice 3).
 *
 * Pins the on-disk anchor format and round-trips an MMR root through
 * the verifier so the on-chain Solana program (which mirrors
 * merkle_mmr_verify byte-for-byte) has a matching reference.
 */
#include "tests/test_harness.h"

/* sim_anchor.c is the lone TU that defines MERKLE_IMPL; pulling the
 * header in here gives us the public API without re-emitting bodies. */
#include "../../vendor/cenetex/merkle.h"

#include "sim_anchor.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>

/* Mirror anchor_hash_pair so the verifier path uses the same callback. */
static void anchor_hash_pair_test(const uint8_t l[32], const uint8_t r[32], uint8_t o[32]) {
    uint8_t b[64]; memcpy(b, l, 32); memcpy(b + 32, r, 32);
    sha256_bytes(b, 64, o);
}

/* Plant N destroyed rocks with synthetic pubs in *sorted ascending*
 * order. The slice-2 ledger stays sorted-by-pub on insert; tests
 * bypass mark_rock_destroyed and write the array directly so they're
 * not reliant on chunk materialization. */
static void seed_destroyed_rocks(world_t *w, int n) {
    w->destroyed_rock_count = (uint16_t)n;
    for (int i = 0; i < n; i++) {
        memset(w->destroyed_rocks[i].rock_pub, 0, 32);
        /* Use big-endian for the prefix so byte-1 increments → byte-0
         * stays — keeps lex order monotonic in i. */
        w->destroyed_rocks[i].rock_pub[1] = (uint8_t)i;
        w->destroyed_rocks[i].destroyed_at_ms = (uint64_t)(1000 + i * 17);
    }
}

TEST(test_anchor_leaf_hash_pinned) {
    /* Pin the leaf encoding so any verifier (Solana program, future
     * Rust port) can independently reproduce the leaf bytes. Drift
     * here breaks the on-chain anchor's verification path. */
    uint8_t pub[32] = {0};
    pub[1] = 0x05;
    uint8_t out[32];
    sim_anchor_leaf_hash(pub, 1085, out);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", out[i]);
    hex[64] = '\0';
    static const char *expected =
        "e8e6d8f3e42a89ab51fb7fba02214c2000daa510f6fc9c5019804fe744fee8b5";
    if (strcmp(hex, expected) != 0) {
        fprintf(stderr, "  leaf-hash drift: expected %s\n               got %s\n",
                expected, hex);
    }
    ASSERT_STR_EQ(hex, expected);
}

TEST(test_anchor_compute_root_empty_ledger) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->destroyed_rock_count = 0;
    uint8_t root[32];
    ASSERT(sim_anchor_compute_root(w, root));
    /* Empty MMR → root is all zeros (merkle_mmr_root contract). */
    for (int i = 0; i < 32; i++) ASSERT(root[i] == 0);
}

TEST(test_anchor_compute_root_changes_with_count) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    seed_destroyed_rocks(w, 5);
    uint8_t r5[32];
    ASSERT(sim_anchor_compute_root(w, r5));
    seed_destroyed_rocks(w, 6);
    uint8_t r6[32];
    ASSERT(sim_anchor_compute_root(w, r6));
    ASSERT(memcmp(r5, r6, 32) != 0);
}

TEST(test_anchor_proof_round_trip_via_root) {
    /* Build an MMR with the same leaf encoding the anchor uses, prove
     * a leaf, verify against the root the anchor would write. This is
     * the exact path the on-chain verifier walks. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    int n = 11;
    seed_destroyed_rocks(w, n);

    merkle_mmr_t *m = merkle_mmr_new(anchor_hash_pair_test);
    uint8_t leaves[16][32];
    for (int i = 0; i < n; i++) {
        sim_anchor_leaf_hash(w->destroyed_rocks[i].rock_pub,
                              w->destroyed_rocks[i].destroyed_at_ms,
                              leaves[i]);
        merkle_mmr_append(m, leaves[i]);
    }
    uint8_t root[32];
    merkle_mmr_root(m, root);

    uint8_t writer_root[32];
    ASSERT(sim_anchor_compute_root(w, writer_root));
    /* The writer and an independent local build must agree. */
    ASSERT(memcmp(root, writer_root, 32) == 0);

    for (int i = 0; i < n; i++) {
        uint8_t proof[MERKLE_MMR_MAX_PROOF_LEN][32];
        size_t peak_idx = 0;
        int len = merkle_mmr_proof(m, (size_t)i, proof, &peak_idx);
        ASSERT(len > 0);
        bool ok = merkle_mmr_verify(anchor_hash_pair_test, leaves[i], (size_t)i,
                                     (const uint8_t (*)[32])proof, (size_t)len,
                                     peak_idx, (size_t)n, root);
        if (!ok) {
            fprintf(stderr, "  proof verify FAILED for leaf %d\n", i);
        }
        ASSERT(ok);
    }
    merkle_mmr_free(m);
}

TEST(test_anchor_close_epoch_writes_pinned_format) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    seed_destroyed_rocks(w, 7);
    w->time = 42.5f;
    const char *path = TMP("test_anchor.bin");
    ASSERT(sim_anchor_close_epoch(w, /*epoch=*/3, path));

    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    uint8_t buf[SIM_ANCHOR_RECORD_SIZE];
    size_t got = fread(buf, 1, sizeof(buf), f);
    int extra = fgetc(f); /* should be EOF */
    fclose(f);
    ASSERT_EQ_INT((int)got, SIM_ANCHOR_RECORD_SIZE);
    ASSERT(extra == EOF);

    /* Decode and check fields. */
    uint32_t magic = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                     ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    ASSERT_EQ_INT((int)magic, (int)SIM_ANCHOR_MAGIC);
    uint32_t spec = (uint32_t)buf[4] | ((uint32_t)buf[5] << 8) |
                    ((uint32_t)buf[6] << 16) | ((uint32_t)buf[7] << 24);
    ASSERT_EQ_INT((int)spec, SIM_ANCHOR_SPEC_VERSION);
    /* epoch_number = 3, le64 */
    ASSERT_EQ_INT((int)buf[8], 3);
    for (int i = 9; i < 16; i++) ASSERT_EQ_INT((int)buf[i], 0);
    /* leaf_count = 7, le64 */
    ASSERT_EQ_INT((int)buf[16], 7);
    for (int i = 17; i < 24; i++) ASSERT_EQ_INT((int)buf[i], 0);
    /* mmr_root[32] at offset 24..55 — compare against compute_root. */
    uint8_t expected_root[32];
    ASSERT(sim_anchor_compute_root(w, expected_root));
    ASSERT(memcmp(buf + 24, expected_root, 32) == 0);
    /* closed_at_ms at 56..63 = 42500 */
    uint64_t closed = 0;
    for (int i = 0; i < 8; i++) closed |= (uint64_t)buf[56 + i] << (i * 8);
    ASSERT_EQ_INT((int)closed, 42500);

    remove(path);
}

void register_anchor_tests(void) {
    TEST_SECTION("\nDestroyed-rocks anchor (#285 slice 3):\n");
    RUN(test_anchor_leaf_hash_pinned);
    RUN(test_anchor_compute_root_empty_ledger);
    RUN(test_anchor_compute_root_changes_with_count);
    RUN(test_anchor_proof_round_trip_via_root);
    RUN(test_anchor_close_epoch_writes_pinned_format);
}
