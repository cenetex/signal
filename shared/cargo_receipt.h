/*
 * cargo_receipt.h -- Portable cargo provenance receipts (Layer D of #479).
 *
 * After Layer C every state-mutating event lands in the *origin
 * station's* signed chain log. That is enough for an auditor reading
 * the origin log, but a *destination* station that only sees the cargo
 * arrive has no proof of what produced it. Layer D fills that gap by
 * making cargo carry a portable, station-signed receipt chain.
 *
 *   cargo_receipt_t — one signed link in a receipt chain.
 *
 * When station A transfers cargo C to recipient R (a player or another
 * station), A signs a cargo_receipt_t binding (cargo, recipient, prior
 * link). The recipient holds onto that receipt. When the recipient
 * later transfers C to station B, B verifies the chain end-to-end
 * before accepting:
 *
 *   1. Each receipt's signature verifies against the claimed
 *      authoring_station pubkey.
 *   2. Each receipt's prev_receipt_hash equals SHA-256(prev_receipt).
 *   3. The chain bottoms out at an "origin" receipt whose
 *      prev_receipt_hash is the SHA-256 of the originating SMELT or
 *      CRAFT chain event header (so the origin point is itself
 *      verifiable in isolation against the authoring station's chain
 *      log — no need to read foreign logs at validate time).
 *
 * If any step fails the destination refuses the transfer. On accept,
 * the destination signs and returns its OWN receipt — the chain grows
 * by one hop.
 *
 * Cap: a chain may not exceed CARGO_RECEIPT_CHAIN_MAX_LEN links. Beyond
 * that the receiving station refuses with cap-exceeded; merkle
 * compaction of pruned receipts is a follow-up.
 *
 * IMPORTANT: this header is in shared/ because both the server (issues
 * + validates) and the client (carries + presents) speak the same wire
 * format. The crypto helpers wrap the existing signal_crypto + sha256
 * primitives; this file does not introduce any new dependency.
 */
#ifndef SHARED_CARGO_RECEIPT_H
#define SHARED_CARGO_RECEIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"  /* cargo_unit_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Wire-stable receipt record.
 *
 * Layout is exactly 208 bytes; the unsigned region (everything except
 * the trailing 64-byte signature) is the 144-byte span fed into
 * Ed25519. We pack the field order so the natural C99 layout matches
 * the on-wire byte order on every platform we ship to (32-byte
 * blocks first, then two u64 fields, then the prev-hash, then the
 * signature). A compile-time _Static_assert in cargo_receipt.c catches
 * any future drift. */
typedef struct {
    uint8_t  cargo_pub[32];          /* the cargo_unit_t.pub being transferred */
    uint8_t  authoring_station[32];  /* station that signed this receipt */
    uint8_t  recipient_pubkey[32];   /* player or station receiving */
    uint64_t event_id;               /* matches the EVT_TRANSFER event_id
                                      * in the authoring station's log;
                                      * 0 for synthetic receipts */
    uint64_t epoch;                  /* sim tick */
    uint8_t  prev_receipt_hash[32];  /* SHA-256 of previous receipt
                                      * header, OR — for an origin
                                      * receipt — SHA-256 of the
                                      * originating SMELT/CRAFT chain
                                      * event header (chain_event_header_t).
                                      * All-zero is invalid post-D. */
    uint8_t  signature[64];          /* Ed25519 over the unsigned span */
} cargo_receipt_t;

#define CARGO_RECEIPT_SIZE          208
#define CARGO_RECEIPT_UNSIGNED_SIZE 144 /* 208 - 64 */
#define CARGO_RECEIPT_CHAIN_MAX_LEN 16  /* hard cap; refuse beyond */

/* Validation result codes — surfaced from cargo_receipt_chain_verify so
 * the caller can log which invariant fired. Wire-stable for chain log
 * payloads. */
typedef enum {
    CARGO_RECEIPT_OK                      = 0,
    CARGO_RECEIPT_REJECT_EMPTY            = 1, /* zero-length chain */
    CARGO_RECEIPT_REJECT_TOO_LONG         = 2, /* > CHAIN_MAX_LEN */
    CARGO_RECEIPT_REJECT_BAD_SIGNATURE    = 3, /* sig fails verify */
    CARGO_RECEIPT_REJECT_BROKEN_LINKAGE   = 4, /* prev_receipt_hash mismatch */
    CARGO_RECEIPT_REJECT_CARGO_MISMATCH   = 5, /* receipt cargo_pub != requested cargo */
    CARGO_RECEIPT_REJECT_ZERO_AUTHORITY   = 6, /* authoring_station all zero */
    CARGO_RECEIPT_REJECT_ZERO_ORIGIN      = 7  /* origin receipt's prev hash is all zero */
} cargo_receipt_result_t;

/* Pack the unsigned span (the 168 bytes that get signed / hashed) into
 * a canonical little-endian byte buffer. Same mechanism as
 * chain_event_header_pack — independent of host endianness. */
void cargo_receipt_unsigned_pack(const cargo_receipt_t *r,
                                 uint8_t out[CARGO_RECEIPT_UNSIGNED_SIZE]);

/* Pack the FULL 232-byte record (unsigned span + 64-byte signature). */
void cargo_receipt_pack(const cargo_receipt_t *r,
                        uint8_t out[CARGO_RECEIPT_SIZE]);

/* SHA-256 of the full 232-byte packed record. This is what the NEXT
 * receipt's prev_receipt_hash must equal. */
void cargo_receipt_hash(const cargo_receipt_t *r, uint8_t out[32]);

/* Verify a single receipt's Ed25519 signature using the
 * authoring_station field as the public key. Does NOT walk the chain. */
bool cargo_receipt_verify_signature(const cargo_receipt_t *r);

/* Walk a presented chain and verify every link.
 *
 * `chain` is in chronological order — chain[0] is the origin (oldest)
 * receipt, chain[count-1] is the most recent.
 *
 * Each receipt[i].prev_receipt_hash for i > 0 must equal
 * SHA-256(receipt[i-1]). receipt[0].prev_receipt_hash is the "origin
 * pin" — it's expected to be SHA-256 of the originating SMELT/CRAFT
 * chain event header. This function does NOT consult any chain log on
 * disk; the caller is responsible for cross-checking the origin pin
 * against an EVT_SMELT/EVT_CRAFT entry if it has access to the
 * authoring station's log. The chain itself, however, is fully
 * verifiable in isolation: signatures + linkage are checked.
 *
 * `expected_cargo_pub` (32 bytes, may be NULL): if non-NULL every
 * receipt in the chain must reference this exact cargo_pub. NULL skips
 * that check (used by tests + tooling that already trust the cargo
 * binding). */
cargo_receipt_result_t cargo_receipt_chain_verify(
    const cargo_receipt_t *chain, size_t count,
    const uint8_t *expected_cargo_pub /* nullable, 32 bytes */);

/* ---------------- ship_receipts_t ---------------------------------- */

/* Per-cargo receipt store running parallel to ship_t.manifest.
 *
 *   entries[i] is the most-recent receipt for manifest unit i.
 *   The full chain is walked by chasing prev_receipt_hash backwards
 *   *if* the bearer is asked to present it; in this layer we keep the
 *   last-N receipts inline because cargo_unit_t has no room for a
 *   pointer-to-chain. The *server* validates a full chain at the
 *   bearer's presentation time (NET_MSG_PRESENT_RECEIPT_CHAIN sends
 *   the ordered chain, not just one entry).
 *
 *   For the in-process server (singleplayer + tests) we keep the full
 *   ordered chain attached on the *ship* side keyed by manifest index.
 *   Multi-link chains are stored in `chains[]` — each entry is a
 *   small sequence whose final element matches `entries[i]`. This is
 *   memory-cheap (capacity == manifest.cap, MAX 16 receipts per
 *   chain), persists alongside the manifest, and gives us the bytes
 *   we ship in NET_MSG_PRESENT_RECEIPT_CHAIN with no extra round-trip.
 *
 * Invariant after a consistent op: count == ship->manifest.count, and
 * for every i in [0, count) chains[i].len in [1, CHAIN_MAX_LEN].
 *
 * "Consistent op" means: any code path that pushes a cargo_unit_t into
 * ship.manifest must also push the matching receipt into ship.receipts
 * (and vice versa for remove). The verifier asserts this in tests. */
typedef struct {
    cargo_receipt_t links[CARGO_RECEIPT_CHAIN_MAX_LEN];
    uint8_t         len; /* in [0, CHAIN_MAX_LEN]; 0 = no chain attached */
} cargo_receipt_chain_t;

typedef struct {
    cargo_receipt_chain_t *chains; /* heap-allocated, capacity == cap */
    uint16_t count;                /* must mirror manifest.count */
    uint16_t cap;
} ship_receipts_t;

/* Lifecycle helpers. ship_receipts_init/_free are pure ship_receipts_t
 * operations; the ship-level wrappers in shared/manifest.h call them
 * inside ship_manifest_bootstrap / ship_cleanup so the receipt store
 * always exists alongside the manifest. */
bool ship_receipts_init(ship_receipts_t *r, uint16_t cap);
void ship_receipts_free(ship_receipts_t *r);
void ship_receipts_clear(ship_receipts_t *r);
bool ship_receipts_reserve(ship_receipts_t *r, uint16_t cap);
bool ship_receipts_clone(ship_receipts_t *dst, const ship_receipts_t *src);

/* Append a chain (1..CHAIN_MAX_LEN receipts) to the receipts store.
 * Mirrors manifest_push. Returns false if cap would overflow or `len`
 * is 0/too long. */
bool ship_receipts_push_chain(ship_receipts_t *r,
                              const cargo_receipt_t *chain, uint8_t len);

/* Remove receipts at index, mirroring manifest_remove. If `out_chain`
 * is non-NULL the removed chain is copied there. */
bool ship_receipts_remove(ship_receipts_t *r, uint16_t index,
                          cargo_receipt_chain_t *out_chain);

/* Append a single new receipt to the chain at index, growing it by one
 * hop. Returns false if cap-exceeded (the chain has hit
 * CARGO_RECEIPT_CHAIN_MAX_LEN already). */
bool ship_receipts_extend(ship_receipts_t *r, uint16_t index,
                          const cargo_receipt_t *next);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_CARGO_RECEIPT_H */
