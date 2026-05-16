/*
 * cargo_receipt_issue.h -- Server-side issuance of cargo_receipt_t (Layer D of #479).
 *
 * Bridges the wire-stable cargo_receipt_t format (shared/cargo_receipt.h)
 * with station-side signing (server/station_authority.h) and chain log
 * anchoring (server/chain_log.h).
 *
 * This file lives in server/ — clients only verify receipts; they never
 * sign new ones. The TweetNaCl signing key lives only on the server.
 */
#ifndef SERVER_CARGO_RECEIPT_ISSUE_H
#define SERVER_CARGO_RECEIPT_ISSUE_H

#include "cargo_receipt.h"
#include "chain_log.h"  /* world_t, station_t, chain_event_type_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Issue a fresh cargo_receipt_t for a transfer authored by station `s`.
 *
 * Computes the unsigned span, signs with station_secret, fills out the
 * signature field, and returns the result by value. The caller is
 * responsible for forming the prev_receipt_hash:
 *   - For the FIRST hop after smelt/craft, pass the SHA-256 of the
 *     originating EVT_SMELT or EVT_CRAFT chain_event_header_t (use
 *     chain_event_header_hash on the just-emitted header).
 *   - For subsequent hops, pass cargo_receipt_hash() of the previous
 *     receipt in the chain.
 *
 * `event_id` is the EVT_TRANSFER event_id from chain_log_emit (so the
 * receipt and the chain event are stitched together).
 *
 * Returns true on success; on failure (e.g. unkeyed station) the
 * receipt is zeroed and false is returned. */
bool cargo_receipt_issue(const station_t *s,
                         uint64_t epoch,
                         uint64_t event_id,
                         const uint8_t cargo_pub[32],
                         const uint8_t recipient_pubkey[32],
                         const uint8_t prev_receipt_hash[32],
                         cargo_receipt_t *out);

/* Convenience: emit an EVT_TRANSFER and produce the matching receipt
 * in one call. The returned receipt's event_id matches the emitted
 * event's id. The transfer payload is the canonical
 * (from, to, cargo_pub, kind) shape used elsewhere in main.c.
 *
 * `prev_receipt_hash` follows the same rule as cargo_receipt_issue
 * above. The caller chooses whether this transfer is "origin" (anchor
 * to a SMELT/CRAFT event hash) or "hop N" (anchor to the previous
 * receipt's hash).
 *
 * Returns the new chain event_id (>= 1) and writes the receipt
 * through `out_receipt`. Returns 0 on failure (chain_log_emit failed
 * or station unkeyed). */
uint64_t cargo_receipt_emit_transfer(world_t *w, station_t *s,
                                     const uint8_t from_pubkey[32],
                                     const uint8_t to_pubkey[32],
                                     const uint8_t cargo_pub[32],
                                     uint8_t cargo_kind,
                                     const uint8_t prev_receipt_hash[32],
                                     cargo_receipt_t *out_receipt);

typedef enum {
    CARGO_RECEIPT_PRESENT_OK = 0,
    CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS,
    CARGO_RECEIPT_PRESENT_REJECT_NO_PLAYER_KEY,
    CARGO_RECEIPT_PRESENT_REJECT_NOT_CARRIED,
    CARGO_RECEIPT_PRESENT_REJECT_VERIFY,
    CARGO_RECEIPT_PRESENT_REJECT_RECIPIENT,
    CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH,
    CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE
} cargo_receipt_present_result_t;

/* Accept a peer-presented receipt chain for cargo currently carried by `sp`.
 *
 * The chain must verify for `cargo_pub`, its head recipient must be the
 * player's registered pubkey, and any already-attached local chain must be
 * an exact prefix of the presented chain. On success the chain is stored in
 * the ship's parallel receipt store at the matching manifest index. */
cargo_receipt_present_result_t cargo_receipt_present_to_ship(
    server_player_t *sp,
    const uint8_t cargo_pub[32],
    const cargo_receipt_t *chain,
    uint8_t chain_len);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_RECEIPT_ISSUE_H */
