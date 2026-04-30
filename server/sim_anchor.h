/*
 * sim_anchor.h — destroyed-rocks epoch anchor (#285 slice 3)
 *
 * At each epoch boundary the live destroyed_rocks ledger is frozen
 * into an immutable artifact:
 *
 *   1. A Merkle Mountain Range over the sorted destroyed rock_pubs.
 *      Each leaf is SHA-256(rock_pub[32] || destroyed_at_ms_le8) so
 *      the leaf encoding doubles as the canonical "destroyed at this
 *      time" proof input.
 *   2. The MMR root + epoch metadata are written to
 *        chain/destroyed_<epoch>.anchor
 *      via the same relative-cwd convention the signal_chain log uses
 *      (the docker entrypoint sets cwd to /app/data).
 *
 * Slice 3 ships only the writer + the file format. Posting the root
 * to a Solana program is a follow-up (sim 480) once the on-chain
 * verifier program lands; slice 3 produces the artifact that
 * verifier will consume.
 *
 * The Binary Fuse filter (vendored under vendor/fastfilter) is NOT
 * used yet — the live ledger's 256-entry cap makes bsearch
 * everywhere strictly faster than building a filter. The filter
 * lights up when cardinality climbs past where bsearch hurts, which
 * is also the slice that swaps the inline ledger for the side-file.
 */
#ifndef SIM_ANCHOR_H
#define SIM_ANCHOR_H

#include <stdbool.h>
#include <stdint.h>

#include "game_sim.h"

#define SIM_ANCHOR_MAGIC        0x52414e44  /* "DNAR" little-endian = "RAND" */
#define SIM_ANCHOR_SPEC_VERSION 1

/* On-disk anchor record (little-endian throughout):
 *
 *   uint32  magic           = SIM_ANCHOR_MAGIC
 *   uint32  spec_version    = SIM_ANCHOR_SPEC_VERSION
 *   uint64  epoch_number    (caller-provided; expected to be monotonic)
 *   uint64  leaf_count      (number of destroyed rocks at close time)
 *   uint8   mmr_root[32]
 *   uint64  closed_at_ms    (world clock at close)
 *
 * 4 + 4 + 8 + 8 + 32 + 8 = 64 bytes. No signature in this layer; the
 * signing pass that wraps the file lives at the chain-anchor layer
 * (#479 / #480 once those land). */
#define SIM_ANCHOR_RECORD_SIZE 64

/* Build the MMR over the current destroyed_rocks ledger and write the
 * anchor record to `out_path` (truncates / overwrites if present).
 * Returns true on success, false on I/O or build failure. The world
 * is not mutated — close-epoch resets are a separate policy choice
 * left to the caller for now. */
bool sim_anchor_close_epoch(const world_t *w,
                            uint64_t epoch_number,
                            const char *out_path);

/* Compute the canonical leaf hash for a destroyed_rocks entry.
 * Exposed for tests and the on-chain verifier mirror.
 *
 *   leaf_hash = SHA-256(rock_pub[32] || destroyed_at_ms_le8) */
void sim_anchor_leaf_hash(const uint8_t rock_pub[32],
                          uint64_t destroyed_at_ms,
                          uint8_t out[32]);

/* Compute just the MMR root (no file I/O). Useful for tests and for
 * future "what would the anchor be" probes. Returns true on success. */
bool sim_anchor_compute_root(const world_t *w, uint8_t root_out[32]);

#endif /* SIM_ANCHOR_H */
