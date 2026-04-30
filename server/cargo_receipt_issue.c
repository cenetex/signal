/*
 * cargo_receipt_issue.c -- Server-side cargo_receipt_t issuance + emit.
 *
 * See cargo_receipt_issue.h for the public contract. This file glues
 * shared/cargo_receipt.h (wire format + verify) to server-only
 * primitives: station_authority signing and chain_log_emit.
 */
#include "cargo_receipt_issue.h"

#include "game_sim.h"
#include "station_authority.h"

#include <string.h>

bool cargo_receipt_issue(const station_t *s,
                         uint64_t epoch,
                         uint64_t event_id,
                         const uint8_t cargo_pub[32],
                         const uint8_t recipient_pubkey[32],
                         const uint8_t prev_receipt_hash[32],
                         cargo_receipt_t *out) {
    if (!s || !out || !cargo_pub || !recipient_pubkey || !prev_receipt_hash)
        return false;
    static const uint8_t zero32[32] = {0};
    if (memcmp(s->station_pubkey, zero32, 32) == 0) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->cargo_pub, cargo_pub, 32);
    memcpy(out->authoring_station, s->station_pubkey, 32);
    memcpy(out->recipient_pubkey, recipient_pubkey, 32);
    out->event_id = event_id;
    out->epoch = epoch;
    memcpy(out->prev_receipt_hash, prev_receipt_hash, 32);

    uint8_t blob[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(out, blob);
    station_sign(s, blob, sizeof(blob), out->signature);
    return true;
}

uint64_t cargo_receipt_emit_transfer(world_t *w, station_t *s,
                                     const uint8_t from_pubkey[32],
                                     const uint8_t to_pubkey[32],
                                     const uint8_t cargo_pub[32],
                                     uint8_t cargo_kind,
                                     const uint8_t prev_receipt_hash[32],
                                     cargo_receipt_t *out_receipt) {
    if (!s || !out_receipt) return 0;
    /* Wire-stable EVT_TRANSFER payload — typedef'd in chain_log.h so
     * the on-disk byte format has a single source of truth across
     * every emit site. */
    chain_payload_transfer_t xfer = {0};
    if (from_pubkey)   memcpy(xfer.from_pubkey, from_pubkey, 32);
    if (to_pubkey)     memcpy(xfer.to_pubkey,   to_pubkey,   32);
    if (cargo_pub)     memcpy(xfer.cargo_pub,   cargo_pub,   32);
    xfer.kind = cargo_kind;

    uint64_t event_id = chain_log_emit(w, s, CHAIN_EVT_TRANSFER,
                                       &xfer, (uint16_t)sizeof(xfer));
    if (event_id == 0) {
        memset(out_receipt, 0, sizeof(*out_receipt));
        return 0;
    }
    /* Epoch in ticks — same convention chain_log_emit used. */
    uint64_t epoch_ticks = w ? (uint64_t)(w->time * 120.0) : 0;
    if (!cargo_receipt_issue(s, epoch_ticks, event_id, cargo_pub,
                             to_pubkey ? to_pubkey : (const uint8_t[32]){0},
                             prev_receipt_hash, out_receipt))
        return 0;
    return event_id;
}
