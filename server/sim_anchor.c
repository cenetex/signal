/*
 * sim_anchor.c — see sim_anchor.h.
 *
 * Builds the destroyed-rocks MMR using vendor/cenetex/merkle.h and
 * writes the closed-epoch anchor file. Pure I/O glue around the
 * vendored primitive — no policy here. Triggering (when epochs close)
 * is the caller's call.
 */
#define MERKLE_IMPL
#include "../vendor/cenetex/merkle.h"

#include "sim_anchor.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* Hash callback for the MMR — SPEC.md §1: out = SHA-256(left || right). */
static void anchor_hash_pair(const uint8_t left[32],
                              const uint8_t right[32],
                              uint8_t out[32]) {
    uint8_t buf[64];
    memcpy(buf, left, 32);
    memcpy(buf + 32, right, 32);
    sha256_bytes(buf, 64, out);
}

void sim_anchor_leaf_hash(const uint8_t rock_pub[32],
                          uint64_t destroyed_at_ms,
                          uint8_t out[32]) {
    uint8_t buf[40];
    memcpy(buf, rock_pub, 32);
    /* Little-endian — matches every other multi-byte int in the
     * anchor record + the verifier's calldata layout. */
    for (int i = 0; i < 8; i++) {
        buf[32 + i] = (uint8_t)((destroyed_at_ms >> (i * 8)) & 0xFF);
    }
    sha256_bytes(buf, sizeof(buf), out);
}

/* Build the MMR for a snapshot. Caller frees with merkle_mmr_free. */
static merkle_mmr_t *anchor_build_mmr(const world_t *w) {
    merkle_mmr_t *m = merkle_mmr_new(anchor_hash_pair);
    if (!m) return NULL;
    /* The destroyed_rocks ledger is already sorted ascending by
     * rock_pub (slice 2 invariant); MMR leaf order = ledger order so
     * leaf indices are stable across reads of the same ledger. */
    for (uint16_t i = 0; i < w->destroyed_rock_count; i++) {
        uint8_t leaf[32];
        sim_anchor_leaf_hash(w->destroyed_rocks[i].rock_pub,
                             w->destroyed_rocks[i].destroyed_at_ms,
                             leaf);
        if (merkle_mmr_append(m, leaf) == 0) {
            merkle_mmr_free(m);
            return NULL;
        }
    }
    return m;
}

bool sim_anchor_compute_root(const world_t *w, uint8_t root_out[32]) {
    if (!w || !root_out) return false;
    merkle_mmr_t *m = anchor_build_mmr(w);
    if (!m) return false;
    merkle_mmr_root(m, root_out);
    merkle_mmr_free(m);
    return true;
}

/* Little-endian writers; the anchor format is LE everywhere. */
static bool write_u32_le(FILE *f, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; i++) b[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    return fwrite(b, 1, 4, f) == 4;
}

static bool write_u64_le(FILE *f, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    return fwrite(b, 1, 8, f) == 8;
}

bool sim_anchor_close_epoch(const world_t *w,
                            uint64_t epoch_number,
                            const char *out_path) {
    if (!w || !out_path) return false;

    /* Build the MMR + compute the root before opening the file so an
     * allocation failure doesn't leave a partial anchor on disk. */
    merkle_mmr_t *m = anchor_build_mmr(w);
    if (!m) return false;
    uint8_t root[32];
    merkle_mmr_root(m, root);
    uint64_t leaf_count = (uint64_t)merkle_mmr_leaf_count(m);
    merkle_mmr_free(m);

    FILE *f = fopen(out_path, "wb");
    if (!f) return false;
    bool ok =
        write_u32_le(f, SIM_ANCHOR_MAGIC) &&
        write_u32_le(f, SIM_ANCHOR_SPEC_VERSION) &&
        write_u64_le(f, epoch_number) &&
        write_u64_le(f, leaf_count) &&
        (fwrite(root, 1, 32, f) == 32) &&
        write_u64_le(f, (uint64_t)(w->time * 1000.0f));
    fclose(f);
    return ok;
}
