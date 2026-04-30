/*
 * cenetex/merkle — Merkle Mountain Range, single-header C
 *
 * SPDX-License-Identifier: MIT-0
 *
 * Spec: SPEC.md (canonical-form contract). Read it before changing
 * anything in this file. Drift here = forks in any chain that anchors
 * roots produced by this implementation.
 *
 * Usage:
 *   In exactly one .c file:
 *     #define MERKLE_IMPL
 *     #include "merkle.h"
 *   Everywhere else: just #include "merkle.h".
 *
 * Hash function is caller-supplied via merkle_hash_pair_fn so the same
 * primitive serves SHA-256, Poseidon, Blake3, etc. without forks. The
 * canonical bytes hashed are exactly `left[32] || right[32]`; if you
 * need domain separation, do it inside your own hash callback.
 *
 * Storage model: caller-owned. merkle_mmr_new() malloc()s a parallel
 * dynamic array of {32-byte hash, 1-byte height} per node and grows it
 * on append. Storing height directly avoids the fiddly position-to-
 * height arithmetic and makes the implementation small enough to read
 * end-to-end.
 */

#ifndef CENETEX_MERKLE_H
#define CENETEX_MERKLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MERKLE_SPEC_VERSION 1

/* Maximum proof length in 32-byte hashes. Covers up to 2^64 leaves
 * (path height <= 63, peak count <= 64). 128 hashes is generous. */
#define MERKLE_MMR_MAX_PROOF_LEN 128

typedef void (*merkle_hash_pair_fn)(const uint8_t left[32],
                                     const uint8_t right[32],
                                     uint8_t out[32]);

typedef struct merkle_mmr merkle_mmr_t;

/* Construction / destruction. */
merkle_mmr_t *merkle_mmr_new(merkle_hash_pair_fn hash_pair);
void          merkle_mmr_free(merkle_mmr_t *m);

/* Append a leaf. Returns the 1-based MMR position of the leaf node. */
size_t        merkle_mmr_append(merkle_mmr_t *m, const uint8_t leaf[32]);

/* Number of leaves (zero-based count of leaves appended so far). */
size_t        merkle_mmr_leaf_count(const merkle_mmr_t *m);

/* Total nodes in the underlying array (leaves + internal). */
size_t        merkle_mmr_node_count(const merkle_mmr_t *m);

/* Compute the current root into out[32]. SPEC.md §5 (right-fold). */
void          merkle_mmr_root(const merkle_mmr_t *m, uint8_t out[32]);

/* Build an inclusion proof for the given 0-based leaf index.
 * Returns proof length in 32-byte hashes, or -1 on error.
 * Writes the leaf's peak index (left-to-right, 0-based) into *out_peak. */
int  merkle_mmr_proof(const merkle_mmr_t *m, size_t leaf_idx,
                      uint8_t proof_out[MERKLE_MMR_MAX_PROOF_LEN][32],
                      size_t *out_peak);

/* Stateless verifier — mirror this in your on-chain program. The
 * hash callback must agree with the producer. SPEC.md §6. */
bool merkle_mmr_verify(merkle_hash_pair_fn hash_pair,
                       const uint8_t leaf[32],
                       size_t leaf_idx,
                       const uint8_t (*proof)[32],
                       size_t proof_len,
                       size_t peak_idx,
                       size_t leaf_count,
                       const uint8_t expected_root[32]);

/* Helpers exposed for advanced users (verifier shims, golden-vector
 * tests). All match SPEC.md §2-3. */

/* Convert 0-based leaf index to 1-based MMR position. Closed form:
 *   pos = 2*leaf_idx + 1 - popcount(leaf_idx)
 * Derivation: leaf i is preceded by i other leaves and (i - popcount(i))
 * internal nodes, plus its own slot (1-based offset). */
size_t merkle_leaf_index_to_pos(uint64_t leaf_idx);

/* Number of peaks for a given leaf count = popcount(leaf_count). */
size_t merkle_peak_count(uint64_t leaf_count);

/* Fill `out` with peak heights left-to-right (largest first).
 * Returns the number written. `out` should hold at least 64 entries. */
size_t merkle_peak_heights(uint64_t leaf_count, uint8_t out[64]);

#ifdef __cplusplus
}
#endif

/* ===================================================================
 * Implementation. Define MERKLE_IMPL in exactly one TU.
 * =================================================================== */

#ifdef MERKLE_IMPL

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct merkle_mmr {
    merkle_hash_pair_fn hash_pair;
    uint8_t (*nodes)[32];   /* index 0 = MMR pos 1 (1-based logically) */
    uint8_t  *heights;       /* parallel array; heights[i] = node at pos i+1 */
    size_t    node_count;
    size_t    capacity;
    uint64_t  leaf_count;
};

/* --- popcount fallback ----------------------------------------- */

static inline int merkle__popcount64(uint64_t x) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll((unsigned long long)x);
#else
    int n = 0;
    while (x) { n += (int)(x & 1u); x >>= 1; }
    return n;
#endif
}

/* --- SPEC §2: closed-form leaf-index to 1-based MMR position --- */

size_t merkle_leaf_index_to_pos(uint64_t leaf_idx) {
    /* Position of leaf i (0-based) = 2*i + 1 - popcount(i).
     * Sanity check (leaf, pos): (0,1) (1,2) (2,4) (3,5) (4,8) (5,9)
     *                            (6,11) (7,12) (8,16) (9,17). */
    return (size_t)(2u * leaf_idx + 1u - (uint64_t)merkle__popcount64(leaf_idx));
}

size_t merkle_peak_count(uint64_t leaf_count) {
    return (size_t)merkle__popcount64(leaf_count);
}

size_t merkle_peak_heights(uint64_t leaf_count, uint8_t out[64]) {
    /* Heights left-to-right (largest first) = bit positions set in
     * leaf_count from MSB to LSB. */
    size_t n = 0;
    for (int b = 63; b >= 0; b--) {
        if ((leaf_count >> b) & 1u) {
            out[n++] = (uint8_t)b;
        }
    }
    return n;
}

/* Position (1-based) of the kth peak, left-to-right. Each peak of
 * height h ends at the cumulative-end-of-its-subtree mark, which is
 * the running sum of (2^(h+1) - 1) over peaks already counted. */
static uint64_t merkle__peak_pos(uint64_t leaf_count, size_t peak_idx) {
    uint64_t pos = 0;
    size_t k = 0;
    for (int b = 63; b >= 0; b--) {
        if (((leaf_count >> b) & 1u) == 0u) continue;
        pos += ((uint64_t)1 << (b + 1)) - 1;
        if (k == peak_idx) return pos;
        k++;
    }
    return 0;
}

/* Find which peak a 0-based leaf belongs to (left-to-right idx). */
static size_t merkle__peak_for_leaf(uint64_t leaf_count, uint64_t leaf_idx) {
    /* Walk the peaks from leftmost (height = MSB of leaf_count) and
     * count how many leaves fit in each. The leftmost peak of height h
     * covers 2^h leaves. */
    uint64_t consumed = 0;
    size_t k = 0;
    for (int b = 63; b >= 0; b--) {
        if (((leaf_count >> b) & 1u) == 0u) continue;
        uint64_t leaves_in_peak = (uint64_t)1 << b;
        if (leaf_idx < consumed + leaves_in_peak) return k;
        consumed += leaves_in_peak;
        k++;
    }
    return (size_t)-1;
}

/* --- Public API: lifecycle -------------------------------------- */

merkle_mmr_t *merkle_mmr_new(merkle_hash_pair_fn hash_pair) {
    if (!hash_pair) return NULL;
    merkle_mmr_t *m = (merkle_mmr_t *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->hash_pair = hash_pair;
    return m;
}

void merkle_mmr_free(merkle_mmr_t *m) {
    if (!m) return;
    free(m->nodes);
    free(m->heights);
    free(m);
}

size_t merkle_mmr_leaf_count(const merkle_mmr_t *m) {
    return m ? (size_t)m->leaf_count : 0;
}

size_t merkle_mmr_node_count(const merkle_mmr_t *m) {
    return m ? m->node_count : 0;
}

/* Grow both parallel arrays. */
static bool merkle__reserve(merkle_mmr_t *m, size_t need) {
    if (m->capacity >= need) return true;
    size_t cap = m->capacity ? m->capacity : 32;
    while (cap < need) cap *= 2;
    void *p = realloc(m->nodes, cap * 32);
    if (!p) return false;
    m->nodes = (uint8_t (*)[32])p;
    void *h = realloc(m->heights, cap);
    if (!h) return false;
    m->heights = (uint8_t *)h;
    m->capacity = cap;
    return true;
}

/* --- Public API: append + root ---------------------------------- */

size_t merkle_mmr_append(merkle_mmr_t *m, const uint8_t leaf[32]) {
    if (!m || !leaf) return 0;
    if (!merkle__reserve(m, m->node_count + 1)) return 0;
    size_t leaf_node_idx = m->node_count;       /* 0-based */
    memcpy(m->nodes[leaf_node_idx], leaf, 32);
    m->heights[leaf_node_idx] = 0;
    m->node_count++;
    m->leaf_count++;

    /* Walk up: while the previous-at-this-height node is a sibling
     * waiting for a parent, hash and emit. The classic "carry" loop:
     * after appending leaf K, we keep merging while the new node's
     * height equals the height of the node immediately to its left
     * (which would be its sibling). */
    while (m->node_count >= 2) {
        size_t right = m->node_count - 1;
        uint8_t h = m->heights[right];
        /* The left sibling of a node at height h sits at index
         * `right - ((1 << (h+1)) - 1)`. If both have the same height,
         * we can merge them into a parent at height h+1. */
        uint64_t span = ((uint64_t)1 << (h + 1)) - 1;
        if ((uint64_t)right < span) break;
        size_t left = right - (size_t)span;
        if (m->heights[left] != h) break;
        /* Emit parent. */
        if (!merkle__reserve(m, m->node_count + 1)) return 0;
        m->hash_pair(m->nodes[left], m->nodes[right],
                     m->nodes[m->node_count]);
        m->heights[m->node_count] = (uint8_t)(h + 1);
        m->node_count++;
    }
    /* 1-based position of the leaf we just appended. */
    return leaf_node_idx + 1;
}

void merkle_mmr_root(const merkle_mmr_t *m, uint8_t out[32]) {
    if (!m || !out) return;
    if (m->leaf_count == 0) {
        memset(out, 0, 32);
        return;
    }
    size_t pcount = merkle_peak_count(m->leaf_count);
    if (pcount == 1) {
        uint64_t peak = merkle__peak_pos(m->leaf_count, 0);
        memcpy(out, m->nodes[peak - 1], 32);
        return;
    }
    /* Right-fold bag: H(P_0, H(P_1, ... H(P_{k-2}, P_{k-1}))) */
    uint8_t acc[32];
    uint64_t last = merkle__peak_pos(m->leaf_count, pcount - 1);
    memcpy(acc, m->nodes[last - 1], 32);
    for (size_t i = pcount - 1; i > 0; i--) {
        uint64_t prev = merkle__peak_pos(m->leaf_count, i - 1);
        uint8_t tmp[32];
        m->hash_pair(m->nodes[prev - 1], acc, tmp);
        memcpy(acc, tmp, 32);
    }
    memcpy(out, acc, 32);
}

/* --- Inclusion proof construction ------------------------------ */

/* Walk leaf_idx up to its peak through the producer's stored array.
 * Each step picks the sibling using the height array, recording (a)
 * the sibling's hash and (b) which side (left=0, right=1) the sibling
 * was on relative to the climbing accumulator.
 *
 * Termination: when the accumulator's node has no same-height sibling
 * in the array (i.e. it's a peak), stop. */
int merkle_mmr_proof(const merkle_mmr_t *m, size_t leaf_idx,
                     uint8_t proof_out[MERKLE_MMR_MAX_PROOF_LEN][32],
                     size_t *out_peak) {
    if (!m || !proof_out) return -1;
    if (leaf_idx >= m->leaf_count) return -1;

    /* Convert leaf index to 0-based array position. */
    size_t pos = (size_t)merkle_leaf_index_to_pos((uint64_t)leaf_idx) - 1;

    size_t off = 0;
    while (1) {
        uint8_t h = m->heights[pos];
        /* Try right-sibling: if there's a same-height node at
         * pos + (2^(h+1) - 1), we are a left child; sibling is to the
         * right. Parent sits at sibling + 1. */
        uint64_t span = ((uint64_t)1 << (h + 1)) - 1;
        size_t right_sib = pos + (size_t)span;
        if (right_sib < m->node_count && m->heights[right_sib] == h) {
            if (off >= MERKLE_MMR_MAX_PROOF_LEN) return -1;
            memcpy(proof_out[off++], m->nodes[right_sib], 32);
            pos = right_sib + 1; /* parent is right-sibling+1 */
            continue;
        }
        /* Otherwise try left-sibling (we are a right child). */
        if (pos >= span) {
            size_t left_sib = pos - (size_t)span;
            if (m->heights[left_sib] == h) {
                if (off >= MERKLE_MMR_MAX_PROOF_LEN) return -1;
                memcpy(proof_out[off++], m->nodes[left_sib], 32);
                pos = pos + 1; /* parent is right-child+1 */
                /* But the parent is at our pos+1 in *array order* —
                 * which is the next slot after `right` in the merger
                 * (we are `right`). */
                continue;
            }
        }
        /* No sibling at this height → we are a peak. */
        break;
    }

    /* Append all peaks except this one, in left-to-right order. */
    size_t peak_idx = merkle__peak_for_leaf(m->leaf_count,
                                             (uint64_t)leaf_idx);
    if (peak_idx == (size_t)-1) return -1;
    size_t pcount = merkle_peak_count(m->leaf_count);
    for (size_t k = 0; k < pcount; k++) {
        if (k == peak_idx) continue;
        if (off >= MERKLE_MMR_MAX_PROOF_LEN) return -1;
        uint64_t pp = merkle__peak_pos(m->leaf_count, k);
        memcpy(proof_out[off++], m->nodes[pp - 1], 32);
    }
    if (out_peak) *out_peak = peak_idx;
    return (int)off;
}

/* --- Stateless verifier ---------------------------------------- */

/* Replay the producer's path-walk decisions for a given leaf index,
 * filling out_dirs[i] with 0 = sibling-on-right (we were left child)
 * and 1 = sibling-on-left (we were right child). Terminates after
 * peak_height steps; returns the count written, or -1 on shape error. */
static int merkle__verify_path_dirs(uint64_t leaf_count, uint64_t leaf_idx,
                                     uint8_t peak_height, int out_dirs[64]) {
    /* The deterministic rule: at height h, the leaf belongs to a
     * peak-subtree of height ≥ h. The parity of "(leaf_offset_within_
     * peak) >> h & 1" tells us whether we are a left (bit=0) or right
     * (bit=1) child at that height.
     *
     * leaf_offset_within_peak = leaf_idx - sum(2^h_i for peaks before
     * the leaf's peak). */
    /* Find the leaf's peak and its offset. */
    uint64_t consumed = 0;
    int found_peak_height = -1;
    for (int b = 63; b >= 0; b--) {
        if (((leaf_count >> b) & 1u) == 0u) continue;
        uint64_t leaves_in_peak = (uint64_t)1 << b;
        if (leaf_idx < consumed + leaves_in_peak) {
            found_peak_height = b;
            break;
        }
        consumed += leaves_in_peak;
    }
    if (found_peak_height < 0) return -1;
    if ((uint8_t)found_peak_height != peak_height) return -1;
    uint64_t offset = leaf_idx - consumed;
    for (int h = 0; h < (int)peak_height; h++) {
        out_dirs[h] = (int)((offset >> h) & 1u);
    }
    return (int)peak_height;
}

bool merkle_mmr_verify(merkle_hash_pair_fn hash_pair,
                       const uint8_t leaf[32],
                       size_t leaf_idx,
                       const uint8_t (*proof)[32],
                       size_t proof_len,
                       size_t peak_idx,
                       size_t leaf_count,
                       const uint8_t expected_root[32]) {
    if (!hash_pair || !leaf || (!proof && proof_len != 0) ||
        !expected_root) return false;
    if (leaf_idx >= leaf_count) return false;

    uint8_t peaks_h[64];
    size_t pcount = merkle_peak_heights((uint64_t)leaf_count, peaks_h);
    if (peak_idx >= pcount) return false;

    uint8_t peak_height = peaks_h[peak_idx];
    if (proof_len != (size_t)peak_height + (pcount - 1)) return false;

    /* Climb the leaf's peak. */
    uint8_t acc[32];
    memcpy(acc, leaf, 32);
    int dirs[64];
    int d = merkle__verify_path_dirs((uint64_t)leaf_count,
                                     (uint64_t)leaf_idx,
                                     peak_height, dirs);
    if (d != (int)peak_height) return false;
    for (int i = 0; i < peak_height; i++) {
        uint8_t tmp[32];
        if (dirs[i] == 0) {
            /* We were a left child; sibling is on the right. */
            hash_pair(acc, proof[i], tmp);
        } else {
            /* We were a right child; sibling is on the left. */
            hash_pair(proof[i], acc, tmp);
        }
        memcpy(acc, tmp, 32);
    }

    /* Reconstruct the full peak set. */
    uint8_t peaks[64][32];
    size_t off = (size_t)peak_height;
    for (size_t k = 0; k < pcount; k++) {
        if (k == peak_idx) {
            memcpy(peaks[k], acc, 32);
        } else {
            if (off >= proof_len) return false;
            memcpy(peaks[k], proof[off++], 32);
        }
    }
    if (off != proof_len) return false;

    /* Right-fold bag, mirroring merkle_mmr_root. */
    uint8_t root[32];
    if (pcount == 1) {
        memcpy(root, peaks[0], 32);
    } else {
        memcpy(root, peaks[pcount - 1], 32);
        for (size_t i = pcount - 1; i > 0; i--) {
            uint8_t tmp[32];
            hash_pair(peaks[i - 1], root, tmp);
            memcpy(root, tmp, 32);
        }
    }
    return memcmp(root, expected_root, 32) == 0;
}

#endif /* MERKLE_IMPL */

#endif /* CENETEX_MERKLE_H */
