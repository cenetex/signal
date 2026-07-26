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

typedef enum {
    CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED = 0,
    CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED,
    CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS,
    CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE,
    CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID,
    CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND
} cargo_receipt_origin_resolve_status_t;

/* Resolve a cargo-producing SMELT/CRAFT event from one station's verified
 * local history. Missing history, invalid history, and a verified history
 * without this cargo are distinct outcomes; none may be treated as proof. */
cargo_receipt_origin_resolve_status_t cargo_receipt_resolve_local_origin(
    const station_t *station,
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof);

/* Resolve an origin against the exact authority key that authored the first
 * receipt. This is the historical-key form of the resolver: rotated station
 * keys have their own immutable log path and must not be looked up through a
 * station's newer current key. */
cargo_receipt_origin_resolve_status_t cargo_receipt_resolve_origin_for_authority(
    const uint8_t authority[32],
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof);

const char *cargo_receipt_origin_resolve_status_name(
    cargo_receipt_origin_resolve_status_t status);

typedef enum {
    CARGO_RECEIPT_TRANSFER_LINK_READY = 0,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN_FULL,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN
} cargo_receipt_transfer_link_status_t;

typedef struct {
    cargo_receipt_transfer_link_status_t status;
    cargo_receipt_result_t chain_result;
    cargo_receipt_origin_resolve_status_t origin_status;
    uint8_t prev_receipt_hash[32];
} cargo_receipt_transfer_link_t;

/* Prepare the immutable linkage for a transfer before any cargo, balance, or
 * ownership mutation. A non-empty incoming chain links to its final receipt.
 * An empty chain must resolve an exact verified local production event. */
cargo_receipt_transfer_link_t cargo_receipt_prepare_transfer_link(
    const station_t *station,
    const uint8_t cargo_pub[32],
    const cargo_receipt_chain_t *incoming_chain);

/* Convenience: emit an EVT_TRANSFER and produce the matching receipt.
 * Callers pass the incoming receipt chain, never an arbitrary hash. Empty
 * chains are accepted only when the station's verified local history contains
 * the exact SMELT/CRAFT output; multi-hop chains derive their link from the
 * final receipt. The returned receipt's event_id matches the emitted event. */
uint64_t cargo_receipt_emit_transfer(world_t *w, station_t *s,
                                     const uint8_t from_pubkey[32],
                                     const uint8_t to_pubkey[32],
                                     const uint8_t cargo_pub[32],
                                     uint8_t cargo_kind,
                                     const cargo_receipt_chain_t *incoming_chain,
                                     cargo_receipt_t *out_receipt);

typedef enum {
    CARGO_RECEIPT_PRESENT_OK = 0,
    CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS,
    CARGO_RECEIPT_PRESENT_REJECT_NO_PLAYER_KEY,
    CARGO_RECEIPT_PRESENT_REJECT_NOT_CARRIED,
    CARGO_RECEIPT_PRESENT_REJECT_VERIFY,
    CARGO_RECEIPT_PRESENT_REJECT_RECIPIENT,
    CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH,
    CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE,
    CARGO_RECEIPT_PRESENT_REJECT_TRUST
} cargo_receipt_present_result_t;

/* Accept a peer-presented receipt chain for cargo currently carried by `sp`.
 *
 * The chain must verify for `cargo_pub`, its head recipient must be the
 * player's registered pubkey, pass the named station's composed trust
 * evaluator, and extend any already-attached exact prefix. Installation is
 * staged in a cloned cargo store, so every rejection leaves the manifest and
 * receipt store byte-identical. */
cargo_receipt_present_result_t cargo_receipt_present_to_ship(
    const world_t *world,
    int evaluating_station,
    server_player_t *sp,
    const uint8_t cargo_pub[32],
    const cargo_receipt_t *chain,
    uint8_t chain_len);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_RECEIPT_ISSUE_H */
