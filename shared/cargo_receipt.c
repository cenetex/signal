/*
 * cargo_receipt.c -- Layer D of #479. See cargo_receipt.h for design.
 */
#include "cargo_receipt.h"

#include "sha256.h"
#include "signal_crypto.h"

#include <stdlib.h>
#include <string.h>

/* Compile-time guarantee that the on-wire size matches the struct
 * size. If anyone reorders fields or changes alignment this fires
 * before the byte format silently drifts on disk / wire. */
_Static_assert(sizeof(cargo_receipt_t) == CARGO_RECEIPT_SIZE,
               "cargo_receipt_t must be exactly 232 bytes");
_Static_assert(CARGO_RECEIPT_UNSIGNED_SIZE == CARGO_RECEIPT_SIZE - 64,
               "unsigned span = full size minus 64-byte signature");

/* ---------------- Pack ---------------------------------------------- */

void cargo_receipt_unsigned_pack(const cargo_receipt_t *r,
                                 uint8_t out[CARGO_RECEIPT_UNSIGNED_SIZE]) {
    size_t off = 0;
    memcpy(&out[off], r->cargo_pub, 32);          off += 32;
    memcpy(&out[off], r->authoring_station, 32);  off += 32;
    memcpy(&out[off], r->recipient_pubkey, 32);   off += 32;
    /* event_id u64 LE */
    for (int i = 0; i < 8; i++)
        out[off + i] = (uint8_t)(r->event_id >> (i * 8));
    off += 8;
    /* epoch u64 LE */
    for (int i = 0; i < 8; i++)
        out[off + i] = (uint8_t)(r->epoch >> (i * 8));
    off += 8;
    memcpy(&out[off], r->prev_receipt_hash, 32);  off += 32;
    /* off should now equal CARGO_RECEIPT_UNSIGNED_SIZE (144) */
    (void)off;
}

void cargo_receipt_pack(const cargo_receipt_t *r,
                        uint8_t out[CARGO_RECEIPT_SIZE]) {
    cargo_receipt_unsigned_pack(r, out);
    memcpy(&out[CARGO_RECEIPT_UNSIGNED_SIZE], r->signature, 64);
}

void cargo_receipt_hash(const cargo_receipt_t *r, uint8_t out[32]) {
    uint8_t packed[CARGO_RECEIPT_SIZE];
    cargo_receipt_pack(r, packed);
    sha256_bytes(packed, CARGO_RECEIPT_SIZE, out);
}

/* ---------------- Verify -------------------------------------------- */

bool cargo_receipt_verify_signature(const cargo_receipt_t *r) {
    static const uint8_t zero32[32] = {0};
    if (memcmp(r->authoring_station, zero32, 32) == 0) return false;
    uint8_t blob[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(r, blob);
    return signal_crypto_verify(r->signature, blob, sizeof(blob),
                                r->authoring_station);
}

cargo_receipt_result_t cargo_receipt_chain_verify(
    const cargo_receipt_t *chain, size_t count,
    const uint8_t *expected_cargo_pub) {
    static const uint8_t zero32[32] = {0};
    if (count == 0 || !chain) return CARGO_RECEIPT_REJECT_EMPTY;
    if (count > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return CARGO_RECEIPT_REJECT_TOO_LONG;

    /* The origin (oldest) receipt's prev_receipt_hash must NOT be all
     * zero — that field carries the SHA-256 of the originating
     * SMELT/CRAFT chain event header, which is non-zero by
     * construction (SHA-256 of any input is overwhelmingly non-zero).
     * An all-zero pin means the receipt was not anchored to anything,
     * so reject. */
    if (memcmp(chain[0].prev_receipt_hash, zero32, 32) == 0)
        return CARGO_RECEIPT_REJECT_ZERO_ORIGIN;

    for (size_t i = 0; i < count; i++) {
        const cargo_receipt_t *r = &chain[i];
        if (memcmp(r->authoring_station, zero32, 32) == 0)
            return CARGO_RECEIPT_REJECT_ZERO_AUTHORITY;
        if (expected_cargo_pub &&
            memcmp(r->cargo_pub, expected_cargo_pub, 32) != 0)
            return CARGO_RECEIPT_REJECT_CARGO_MISMATCH;
        if (!cargo_receipt_verify_signature(r))
            return CARGO_RECEIPT_REJECT_BAD_SIGNATURE;
        if (i > 0) {
            uint8_t prev_hash[32];
            cargo_receipt_hash(&chain[i - 1], prev_hash);
            if (memcmp(prev_hash, r->prev_receipt_hash, 32) != 0)
                return CARGO_RECEIPT_REJECT_BROKEN_LINKAGE;
        }
    }
    return CARGO_RECEIPT_OK;
}

/* ---------------- ship_receipts_t storage --------------------------- */

bool ship_receipts_init(ship_receipts_t *r, uint16_t cap) {
    if (!r) return false;
    memset(r, 0, sizeof(*r));
    if (cap == 0) return true;
    r->chains = (cargo_receipt_chain_t *)calloc(cap, sizeof(*r->chains));
    if (!r->chains) return false;
    r->cap = cap;
    return true;
}

void ship_receipts_free(ship_receipts_t *r) {
    if (!r) return;
    free(r->chains);
    r->chains = NULL;
    r->count = 0;
    r->cap = 0;
}

void ship_receipts_clear(ship_receipts_t *r) {
    if (!r) return;
    if (r->chains && r->cap > 0)
        memset(r->chains, 0, r->cap * sizeof(*r->chains));
    r->count = 0;
}

bool ship_receipts_reserve(ship_receipts_t *r, uint16_t cap) {
    if (!r) return false;
    if (cap <= r->cap) return true;
    cargo_receipt_chain_t *grown =
        (cargo_receipt_chain_t *)realloc(r->chains,
                                         (size_t)cap * sizeof(*grown));
    if (!grown) return false;
    /* Zero the new tail so undefined fields don't leak. */
    memset(&grown[r->cap], 0,
           (size_t)(cap - r->cap) * sizeof(*grown));
    r->chains = grown;
    r->cap = cap;
    return true;
}

bool ship_receipts_clone(ship_receipts_t *dst, const ship_receipts_t *src) {
    if (!dst || !src) return false;
    if (dst == src) return true;
    ship_receipts_t tmp = {0};
    if (!ship_receipts_init(&tmp, src->cap > 0 ? src->cap : 1)) return false;
    if (src->count > 0 && src->chains)
        memcpy(tmp.chains, src->chains,
               (size_t)src->count * sizeof(*src->chains));
    tmp.count = src->count;
    ship_receipts_free(dst);
    *dst = tmp;
    return true;
}

bool ship_receipts_push_chain(ship_receipts_t *r,
                              const cargo_receipt_t *chain, uint8_t len) {
    if (!r) return false;
    if (len == 0 || len > CARGO_RECEIPT_CHAIN_MAX_LEN) return false;
    if (!chain) return false;
    if (r->count >= r->cap) {
        uint16_t new_cap = r->cap > 0 ? (uint16_t)(r->cap * 2u) : 32;
        if (new_cap <= r->cap) return false;
        if (!ship_receipts_reserve(r, new_cap)) return false;
    }
    cargo_receipt_chain_t *slot = &r->chains[r->count];
    memset(slot, 0, sizeof(*slot));
    memcpy(slot->links, chain, (size_t)len * sizeof(*chain));
    slot->len = len;
    r->count++;
    return true;
}

bool ship_receipts_push_empty(ship_receipts_t *r) {
    if (!r) return false;
    if (r->count >= r->cap) {
        uint16_t new_cap = r->cap > 0 ? (uint16_t)(r->cap * 2u) : 32;
        if (new_cap <= r->cap) return false;
        if (!ship_receipts_reserve(r, new_cap)) return false;
    }
    memset(&r->chains[r->count], 0, sizeof(r->chains[r->count]));
    r->count++;
    return true;
}

bool ship_receipts_remove(ship_receipts_t *r, uint16_t index,
                          cargo_receipt_chain_t *out_chain) {
    if (!r || index >= r->count || !r->chains) return false;
    if (out_chain) *out_chain = r->chains[index];
    if ((uint16_t)(index + 1) < r->count) {
        memmove(&r->chains[index], &r->chains[index + 1],
                (size_t)(r->count - index - 1) * sizeof(*r->chains));
    }
    r->count--;
    /* Zero the now-unused tail so stale data doesn't surface in a
     * later push that doesn't fully overwrite the slot. */
    memset(&r->chains[r->count], 0, sizeof(*r->chains));
    return true;
}

bool ship_receipts_extend(ship_receipts_t *r, uint16_t index,
                          const cargo_receipt_t *next) {
    if (!r || !next || index >= r->count || !r->chains) return false;
    cargo_receipt_chain_t *slot = &r->chains[index];
    if (slot->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) return false;
    slot->links[slot->len++] = *next;
    return true;
}
