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
 * event_id, cargo_pub, recipient_pubkey, and prev_receipt_hash must all be
 * non-zero. Returns true on success; on failure (e.g. unkeyed station) the
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
    CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND,
    CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS,
    CARGO_RECEIPT_ORIGIN_RESOLVE_STATUS_COUNT
} cargo_receipt_origin_resolve_status_t;

/*
 * Resolve a cargo-producing SMELT/CRAFT event from the verified current or
 * preserved historical chain identities in one station's public registry.
 * Missing or disabled history, invalid history, and verified histories without
 * this cargo are distinct outcomes. The proof carries the registry's verified
 * lifecycle for the matching authority; the evaluator's trust decision stays
 * separate and comes from station_authority_trust_for_pubkey.
 */
cargo_receipt_origin_resolve_status_t cargo_receipt_resolve_local_origin(
    const station_t *station,
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof);

/*
 * Resolve against one exact current or historical authority in `station`'s
 * validated registry. This is the composed evaluator's origin path: the first
 * receipt author selects the immutable log identity, so a coincidentally equal
 * cargo pubkey in another preserved history cannot satisfy the proof.
 */
cargo_receipt_origin_resolve_status_t
cargo_receipt_resolve_origin_for_authority(
    const station_t *station,
    const uint8_t authority[32],
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof);

/*
 * Receipt-backed exact lookup. `event_hash_pin` is the first receipt's
 * prev_receipt_hash and selects one signed production event when a malicious
 * or corrupt history contains duplicate output pubkeys. The unpinned APIs
 * reject such duplicates as physically ambiguous.
 */
cargo_receipt_origin_resolve_status_t
cargo_receipt_resolve_origin_for_authority_pinned(
    const station_t *station,
    const uint8_t authority[32],
    const uint8_t cargo_pub[32],
    const uint8_t event_hash_pin[32],
    cargo_receipt_origin_proof_t *out_proof);

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t full_verifications;
    uint64_t index_builds;
    /* Windows uses native O(1) file metadata whenever the filesystem and
     * runtime support it. Full-file digest scans are the compatibility
     * fallback and stay zero on native-metadata cache hits. */
    uint64_t file_native_metadata_queries;
    uint64_t file_digest_scans;
    /* Compatibility counter: growable indexes must keep this at zero. */
    uint64_t fallback_scans;
    uint64_t evictions;
} cargo_receipt_origin_cache_stats_t;

/* Deterministic cache observability used by regression/soak tests. */
void cargo_receipt_origin_cache_reset(void);
cargo_receipt_origin_cache_stats_t
cargo_receipt_origin_cache_stats(void);

#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
/* Deterministic test-only seam for a chain mutation between cache
 * verification and the final file-state check. The hook is one-shot and
 * cleared before it runs, so it may safely append through chain_log_emit(). */
typedef void (*cargo_receipt_origin_cache_test_build_hook_t)(void *user);
void cargo_receipt_origin_cache_test_set_build_hook(
    cargo_receipt_origin_cache_test_build_hook_t hook,
    void *user);

/* Deterministically replace the pathname between the initial state query and
 * snapshot open, then restore it after open. The hook remains installed for
 * both phases and is cleared before the AFTER_OPEN callback. */
typedef enum {
    CARGO_RECEIPT_ORIGIN_CACHE_TEST_BEFORE_SNAPSHOT_OPEN = 0,
    CARGO_RECEIPT_ORIGIN_CACHE_TEST_AFTER_SNAPSHOT_OPEN,
} cargo_receipt_origin_cache_test_snapshot_phase_t;
typedef void (*cargo_receipt_origin_cache_test_snapshot_hook_t)(
    cargo_receipt_origin_cache_test_snapshot_phase_t phase,
    void *user);
void cargo_receipt_origin_cache_test_set_snapshot_hook(
    cargo_receipt_origin_cache_test_snapshot_hook_t hook,
    void *user);

/* Zero keeps the production limit. A non-zero lower limit deterministically
 * exercises fail-closed index-growth handling without host OOM dependence. */
void cargo_receipt_origin_cache_test_set_record_limit(
    size_t max_records);

/* One-shot transient failure seams. Neither failure may leave a negative
 * cache entry: the following lookup must retry the snapshot/index build. */
void cargo_receipt_origin_cache_test_fail_next_record_allocation(void);
void cargo_receipt_origin_cache_test_fail_next_snapshot_open(void);
#endif

const char *cargo_receipt_origin_resolve_status_name(
    cargo_receipt_origin_resolve_status_t status);

typedef enum {
    CARGO_RECEIPT_TRANSFER_LINK_READY = 0,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN_FULL,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN_AUTHORITY,
    CARGO_RECEIPT_TRANSFER_LINK_REJECT_TRUST
} cargo_receipt_transfer_link_status_t;

typedef struct {
    cargo_receipt_transfer_link_status_t status;
    cargo_receipt_result_t chain_result;
    cargo_receipt_origin_resolve_status_t origin_status;
    cargo_receipt_authority_lifecycle_t origin_lifecycle;
    cargo_receipt_authority_trust_t origin_trust;
    uint8_t prev_receipt_hash[32];
} cargo_receipt_transfer_link_t;

/* Prepare immutable linkage before any cargo, balance, or ownership mutation.
 * A non-empty incoming chain links to its final receipt only when that
 * receipt names `from_pubkey` as its recipient. This custody check prevents a
 * valid chain copied from another holder from being extended by a thief.
 * An empty chain additionally requires `from_pubkey` to equal the live
 * station key and must resolve an exact verified local production event
 * under that authority. A historical proof remains available for validation,
 * but cannot be used to mint a first receipt because its signing secret is no
 * longer the station's live key. */
cargo_receipt_transfer_link_t cargo_receipt_prepare_transfer_link(
    const station_t *station,
    const uint8_t from_pubkey[32],
    const uint8_t cargo_pub[32],
    const cargo_receipt_chain_t *incoming_chain);

/* Convenience: validate a complete cargo unit, emit an EVT_TRANSFER, and
 * produce the matching receipt. Callers pass the incoming receipt chain,
 * never an arbitrary hash. Empty chains are accepted only when the station's
 * verified local history binds every semantic field of `unit`; multi-hop
 * chains undergo the same full origin/policy evaluation before deriving their
 * link from the final receipt. The returned receipt's event_id matches the
 * emitted event. */
uint64_t cargo_receipt_emit_transfer(world_t *w, station_t *s,
                                     const uint8_t from_pubkey[32],
                                     const uint8_t to_pubkey[32],
                                     const cargo_unit_t *unit,
                                     const cargo_receipt_chain_t *incoming_chain,
                                     cargo_receipt_t *out_receipt);

/*
 * Result of a station-authored transfer transaction. `receipt` is populated
 * only when the entire append succeeds durably. A trade request is emitted in
 * the same batch immediately after TRANSFER and is pinned to
 * `append.first_event_id`.
 */
typedef struct {
    cargo_receipt_transfer_link_status_t link_status;
    chain_log_append_result_t append;
    cargo_receipt_t receipt;
} cargo_receipt_transfer_commit_result_t;

typedef struct {
    cargo_receipt_transfer_link_status_t link_status;
    chain_log_append_status_t preflight_status;
    int station_index;
    uint8_t event_count;
    uint64_t expected_chain_event_count;
    uint8_t expected_chain_last_hash[32];
    chain_payload_transfer_t transfer;
    chain_payload_trade_t trade;
    cargo_receipt_t receipt;
    /* Internal prepare/commit integrity seal. Not serialized or trusted
     * across processes; it catches accidental mutation between phases. */
    uint8_t preparation_digest[32];
} cargo_receipt_prepared_transfer_t;

/*
 * Split prepare/commit form for callers that must stage allocation-backed
 * gameplay state around the exact receipt bytes. Prepare performs trust,
 * origin, capacity, ID, and signature checks without mutation. Commit rejects
 * a stale station chain head and performs only the durable batch append.
 */
cargo_receipt_prepared_transfer_t cargo_receipt_prepare_transfer(
    const world_t *w,
    int evaluating_station,
    const uint8_t from_pubkey[32],
    const uint8_t to_pubkey[32],
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *incoming_chain,
    bool include_trade,
    int64_t ledger_delta_signed,
    const uint8_t ledger_pubkey[32]);

chain_log_append_result_t cargo_receipt_commit_prepared_transfer(
    world_t *w,
    const cargo_receipt_prepared_transfer_t *prepared);

/*
 * Read-only trust evaluation and durable evidence commit for a gameplay
 * transfer. No manifest, receipt store, balance, contract, or ownership state
 * is changed here. The caller must preflight those destinations, call this
 * function, and only then perform an allocation-free gameplay commit.
 *
 * When `include_trade` is true, TRANSFER and TRADE are one same-station
 * durability unit. `ledger_pubkey` is required in that case.
 */
cargo_receipt_transfer_commit_result_t cargo_receipt_commit_transfer(
    world_t *w,
    int evaluating_station,
    const uint8_t from_pubkey[32],
    const uint8_t to_pubkey[32],
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *incoming_chain,
    bool include_trade,
    int64_t ledger_delta_signed,
    const uint8_t ledger_pubkey[32]);

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
 * player's registered pubkey, and any already-attached local chain must be
 * an exact prefix of the presented chain. On success the chain is stored in
 * the ship's parallel receipt store at the matching manifest index. */
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
