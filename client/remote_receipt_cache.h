#ifndef REMOTE_RECEIPT_CACHE_H
#define REMOTE_RECEIPT_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "manifest.h"
#include "protocol.h"

/*
 * Receipt packets may arrive before the atomic player-manifest snapshot that
 * introduces their cargo identities. Retain one sidecar for every concrete
 * identity a snapshot can carry; a smaller ad-hoc bound silently strips
 * receipts again when the next snapshot rebuilds the local cargo store.
 */
enum {
    REMOTE_RECEIPT_CACHE_CAP = MANIFEST_DETAIL_MAX,
};

typedef struct {
    cargo_receipt_chain_t chains[REMOTE_RECEIPT_CACHE_CAP];
    uint8_t cargo_pubs[REMOTE_RECEIPT_CACHE_CAP][32];
    uint16_t count;
} remote_receipt_cache_t;

/*
 * A reconnect or authority switch starts a new receipt-delivery epoch.
 * Pending chains are session-local, and chains already attached to the live
 * read model must not be used as fallback sidecars for an equal cargo
 * identity from the next authority. Preserve manifest/receipt index parity
 * while forgetting every chain byte.
 */
static inline void remote_receipt_cache_reset_session(
    remote_receipt_cache_t *cache,
    ship_t *live_ship) {
    if (cache) memset(cache, 0, sizeof(*cache));
    if (!live_ship || !live_ship->receipts_opaque) return;
    ship_receipts_t *receipts = ship_get_receipts(live_ship);
    if (!receipts) return;
    (void)ship_receipts_clear_chains(receipts);
}

static inline bool remote_receipt_chain_cargo_pub(
    const cargo_receipt_chain_t *chain,
    uint8_t out[32]) {
    static const uint8_t zero32[32] = {0};
    if (!chain || !out || chain->len == 0 ||
        chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN ||
        memcmp(chain->links[0].cargo_pub, zero32, sizeof(zero32)) == 0) {
        return false;
    }
    memcpy(out, chain->links[0].cargo_pub, 32);
    return true;
}

static inline int remote_receipt_cache_find(
    const remote_receipt_cache_t *cache,
    const uint8_t cargo_pub[32]) {
    if (!cache || !cargo_pub) return -1;
    for (uint16_t i = 0; i < cache->count; i++) {
        if (memcmp(cache->cargo_pubs[i], cargo_pub, 32) == 0)
            return (int)i;
    }
    return -1;
}

static inline bool remote_receipt_cache_store(
    remote_receipt_cache_t *cache,
    const cargo_receipt_chain_t *chain) {
    uint8_t cargo_pub[32];
    if (!cache ||
        !remote_receipt_chain_cargo_pub(chain, cargo_pub)) {
        return false;
    }

    int idx = remote_receipt_cache_find(cache, cargo_pub);
    if (idx < 0) {
        if (cache->count >= REMOTE_RECEIPT_CACHE_CAP) {
            memmove(cache->cargo_pubs, &cache->cargo_pubs[1],
                    (REMOTE_RECEIPT_CACHE_CAP - 1u) *
                        sizeof(cache->cargo_pubs[0]));
            memmove(cache->chains, &cache->chains[1],
                    (REMOTE_RECEIPT_CACHE_CAP - 1u) *
                        sizeof(cache->chains[0]));
            idx = REMOTE_RECEIPT_CACHE_CAP - 1;
        } else {
            idx = (int)cache->count++;
        }
    }
    memcpy(cache->cargo_pubs[idx], cargo_pub, 32);
    cache->chains[idx] = *chain;
    return true;
}

static inline const cargo_receipt_chain_t *
remote_receipt_cache_lookup(
    const remote_receipt_cache_t *cache,
    const uint8_t cargo_pub[32]) {
    int idx = remote_receipt_cache_find(cache, cargo_pub);
    return idx >= 0 ? &cache->chains[idx] : NULL;
}

static inline bool remote_receipt_cache_remove(
    remote_receipt_cache_t *cache,
    const uint8_t cargo_pub[32]) {
    int idx = remote_receipt_cache_find(cache, cargo_pub);
    if (!cache || idx < 0) return false;
    uint16_t tail = (uint16_t)idx + 1u;
    if (tail < cache->count) {
        memmove(
            &cache->cargo_pubs[idx],
            &cache->cargo_pubs[tail],
            (cache->count - tail) * sizeof(cache->cargo_pubs[0]));
        memmove(
            &cache->chains[idx],
            &cache->chains[tail],
            (cache->count - tail) * sizeof(cache->chains[0]));
    }
    cache->count--;
    memset(cache->cargo_pubs[cache->count], 0,
           sizeof(cache->cargo_pubs[0]));
    memset(&cache->chains[cache->count], 0,
           sizeof(cache->chains[0]));
    return true;
}

static inline bool remote_receipt_cache_attach(
    const remote_receipt_cache_t *cache,
    ship_t *ship,
    const uint8_t cargo_pub[32]) {
    if (!cache || !ship || !cargo_pub ||
        !ship->manifest.units || !ship->receipts_opaque) {
        return false;
    }
    int cache_idx = remote_receipt_cache_find(cache, cargo_pub);
    int manifest_idx = manifest_find(&ship->manifest, cargo_pub);
    ship_receipts_t *receipts = ship_get_receipts(ship);
    if (cache_idx < 0 || manifest_idx < 0 || !receipts ||
        manifest_idx >= (int)receipts->count) {
        return false;
    }
    return ship_receipts_set_chain(
        receipts, (uint16_t)manifest_idx,
        &cache->chains[cache_idx]);
}

/*
 * The cache contains only chains whose cargo identity is not in the current
 * live manifest. A chain already attached to a live unit belongs to that
 * atomic cargo store and must not compete for this bounded future-arrival
 * buffer.
 */
static inline bool remote_receipt_cache_store_unmatched(
    remote_receipt_cache_t *cache,
    ship_t *ship,
    const cargo_receipt_chain_t *chain) {
    uint8_t cargo_pub[32];
    if (!cache ||
        !remote_receipt_chain_cargo_pub(chain, cargo_pub)) {
        return false;
    }
    if (ship && ship->manifest.units &&
        ship->receipts_opaque) {
        int manifest_idx =
            manifest_find(&ship->manifest, cargo_pub);
        ship_receipts_t *receipts =
            ship_get_receipts(ship);
        if (manifest_idx >= 0 && receipts &&
            manifest_idx < (int)receipts->count) {
            if (!ship_receipts_set_chain(
                    receipts, (uint16_t)manifest_idx, chain)) {
                return false;
            }
            (void)remote_receipt_cache_remove(
                cache, cargo_pub);
            return true;
        }
    }
    return remote_receipt_cache_store(cache, chain);
}

static inline const cargo_receipt_chain_t *
remote_receipt_chain_for_snapshot(
    const remote_receipt_cache_t *cache,
    const ship_t *current_ship,
    const uint8_t cargo_pub[32]) {
    const cargo_receipt_chain_t *pending =
        remote_receipt_cache_lookup(cache, cargo_pub);
    if (pending) return pending;
    if (!current_ship || !current_ship->manifest.units ||
        !current_ship->receipts_opaque) {
        return NULL;
    }
    int current_index =
        manifest_find(&current_ship->manifest, cargo_pub);
    const ship_receipts_t *current_receipts =
        ship_get_receipts_const(current_ship);
    if (current_index < 0 || !current_receipts ||
        current_index >= (int)current_receipts->count ||
        current_receipts->chains[current_index].len == 0) {
        return NULL;
    }
    return &current_receipts->chains[current_index];
}

#endif /* REMOTE_RECEIPT_CACHE_H */
