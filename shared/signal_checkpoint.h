/*
 * signal_checkpoint.h -- Signalspace checkpoint roots (v1).
 *
 * A checkpoint commits to every station's verified chain log at one moment,
 * without changing the log format. Each station contributes one leaf:
 *
 *   head_hash   SHA-256 of the last record header. prev_hash chaining makes
 *               it a commitment to the station's whole final segment.
 *   log_sha256  SHA-256 of the complete verified log bytes. This also covers
 *               earlier segments that a clean event_id=1 restart left behind
 *               the head.
 *
 * Leaves are ordered by station pubkey and combined in a Merkle tree. The
 * checkpoint root binds that tree to the previous checkpoint root, so
 * checkpoints form their own chain:
 *
 *   leaf  = SHA-256(0x00 || "SIGNAL:CHECKPOINT:LEAF:v1" || pubkey
 *                   || u64le total_events || u64le segment_count
 *                   || u64le tail_event_id || head_hash || log_sha256
 *                   || u64le log_bytes)
 *   node  = SHA-256(0x01 || left || right); an odd node moves up unchanged
 *   root  = SHA-256("SIGNAL:CHECKPOINT:v1" || prev_root
 *                   || u64le station_count || stations_root)
 *
 * The 0x00/0x01 prefixes keep a leaf from passing as an inner node. Moving
 * an odd node up, instead of pairing it with itself, keeps two different
 * station sets from sharing a root.
 *
 * Pure math, no I/O: the tool, the tests and any later server emitter use
 * the same bytes. Forge mint seeds commit to `root`.
 */
#ifndef SHARED_SIGNAL_CHECKPOINT_H
#define SHARED_SIGNAL_CHECKPOINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sha256.h"

#define SIGNAL_CHECKPOINT_VERSION 1
/* Depth bound for inclusion proofs; 2^32 stations is far beyond any world. */
#define SIGNAL_CHECKPOINT_PROOF_MAX 32

typedef struct {
    uint8_t  station_pubkey[32];
    uint64_t total_events;
    uint64_t segment_count;
    uint64_t tail_event_id;
    uint8_t  head_hash[32];
    uint8_t  log_sha256[32];
    uint64_t log_bytes;
} signal_checkpoint_station_t;

typedef struct {
    uint8_t sibling[32];
    uint8_t sibling_on_left; /* 1: node = H(0x01 || sibling || current) */
} signal_checkpoint_proof_step_t;

static inline void signal_checkpoint_u64le_(sha256_ctx_t *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (8 * i));
    sha256_update(ctx, bytes, sizeof(bytes));
}

static inline void signal_checkpoint_leaf(const signal_checkpoint_station_t *s,
                                          uint8_t out[32]) {
    static const char domain[] = "SIGNAL:CHECKPOINT:LEAF:v1";
    const uint8_t tag = 0x00;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, &tag, 1);
    sha256_update(&ctx, domain, sizeof(domain) - 1);
    sha256_update(&ctx, s->station_pubkey, 32);
    signal_checkpoint_u64le_(&ctx, s->total_events);
    signal_checkpoint_u64le_(&ctx, s->segment_count);
    signal_checkpoint_u64le_(&ctx, s->tail_event_id);
    sha256_update(&ctx, s->head_hash, 32);
    sha256_update(&ctx, s->log_sha256, 32);
    signal_checkpoint_u64le_(&ctx, s->log_bytes);
    sha256_final(&ctx, out);
}

static inline void signal_checkpoint_node_(const uint8_t left[32],
                                           const uint8_t right[32],
                                           uint8_t out[32]) {
    const uint8_t tag = 0x01;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, &tag, 1);
    sha256_update(&ctx, left, 32);
    sha256_update(&ctx, right, 32);
    sha256_final(&ctx, out);
}

/* Stations must be in strictly ascending pubkey order: sorted, no repeats. */
static inline bool signal_checkpoint_ordered(const signal_checkpoint_station_t *stations,
                                             size_t count) {
    for (size_t i = 1; i < count; i++) {
        if (memcmp(stations[i - 1].station_pubkey, stations[i].station_pubkey, 32) >= 0)
            return false;
    }
    return true;
}

/*
 * Merkle root over leaves, reducing `scratch` (count * 32 bytes, holding the
 * leaves) in place. If `proof` is non-NULL, also records the path for leaf
 * `index` and its length in *proof_len.
 */
static inline bool signal_checkpoint_merkle_(uint8_t (*scratch)[32], size_t count,
                                             size_t index,
                                             signal_checkpoint_proof_step_t *proof,
                                             size_t *proof_len,
                                             uint8_t out_root[32]) {
    if (count == 0) return false;
    size_t steps = 0;
    while (count > 1) {
        size_t next = 0;
        for (size_t i = 0; i < count; i += 2) {
            if (i + 1 < count) {
                if (proof && (index == i || index == i + 1)) {
                    if (steps >= SIGNAL_CHECKPOINT_PROOF_MAX) return false;
                    bool left = index == i + 1;
                    memcpy(proof[steps].sibling, scratch[left ? i : i + 1], 32);
                    proof[steps].sibling_on_left = left ? 1 : 0;
                    steps++;
                }
                signal_checkpoint_node_(scratch[i], scratch[i + 1], scratch[next]);
            } else {
                memmove(scratch[next], scratch[i], 32);
            }
            if (index == i || index == i + 1) index = next;
            next++;
        }
        count = next;
    }
    memcpy(out_root, scratch[0], 32);
    if (proof_len) *proof_len = steps;
    return true;
}

static inline void signal_checkpoint_bind_(const uint8_t prev_root[32],
                                           uint64_t station_count,
                                           const uint8_t stations_root[32],
                                           uint8_t out_root[32]) {
    static const char domain[] = "SIGNAL:CHECKPOINT:v1";
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain) - 1);
    sha256_update(&ctx, prev_root, 32);
    signal_checkpoint_u64le_(&ctx, station_count);
    sha256_update(&ctx, stations_root, 32);
    sha256_final(&ctx, out_root);
}

/*
 * Compute the stations Merkle root and the checkpoint root. `scratch` must
 * hold `count` 32-byte entries. prev_root is all zero for the first
 * checkpoint. Fails on an empty or unordered station list.
 */
static inline bool signal_checkpoint_root(const signal_checkpoint_station_t *stations,
                                          size_t count,
                                          const uint8_t prev_root[32],
                                          uint8_t (*scratch)[32],
                                          uint8_t out_stations_root[32],
                                          uint8_t out_root[32]) {
    if (!stations || !scratch || count == 0 ||
        !signal_checkpoint_ordered(stations, count))
        return false;
    for (size_t i = 0; i < count; i++) signal_checkpoint_leaf(&stations[i], scratch[i]);
    if (!signal_checkpoint_merkle_(scratch, count, 0, NULL, NULL, out_stations_root))
        return false;
    signal_checkpoint_bind_(prev_root, count, out_stations_root, out_root);
    return true;
}

/* Inclusion path for station `index`, for proving one station's head. */
static inline bool signal_checkpoint_proof(const signal_checkpoint_station_t *stations,
                                           size_t count, size_t index,
                                           uint8_t (*scratch)[32],
                                           signal_checkpoint_proof_step_t *proof,
                                           size_t *proof_len) {
    uint8_t root[32];
    if (!stations || !scratch || !proof || !proof_len || index >= count ||
        !signal_checkpoint_ordered(stations, count))
        return false;
    for (size_t i = 0; i < count; i++) signal_checkpoint_leaf(&stations[i], scratch[i]);
    return signal_checkpoint_merkle_(scratch, count, index, proof, proof_len, root);
}

/* True iff `station` is in the checkpoint with this root. */
static inline bool signal_checkpoint_verify_proof(
    const signal_checkpoint_station_t *station,
    const signal_checkpoint_proof_step_t *proof, size_t proof_len,
    const uint8_t prev_root[32], uint64_t station_count,
    const uint8_t expected_root[32]) {
    uint8_t node[32];
    uint8_t root[32];
    if (!station || (proof_len && !proof) || proof_len > SIGNAL_CHECKPOINT_PROOF_MAX)
        return false;
    signal_checkpoint_leaf(station, node);
    for (size_t i = 0; i < proof_len; i++) {
        if (proof[i].sibling_on_left > 1) return false;
        if (proof[i].sibling_on_left)
            signal_checkpoint_node_(proof[i].sibling, node, node);
        else
            signal_checkpoint_node_(node, proof[i].sibling, node);
    }
    signal_checkpoint_bind_(prev_root, station_count, node, root);
    return memcmp(root, expected_root, 32) == 0;
}

#endif
