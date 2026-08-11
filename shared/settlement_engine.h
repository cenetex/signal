#ifndef SHARED_SETTLEMENT_ENGINE_H
#define SHARED_SETTLEMENT_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cargo_receipt.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t fragment_pub[32];
    uint8_t winner_pubkey[32];
    uint8_t rock_pub[32];
    uint8_t grade;
    uint64_t mined_block;
} settl_fragment_owner_t;

typedef struct {
    uint8_t  note_id[32];
    uint8_t  station_pubkey[32];
    uint8_t  player_pubkey[32];
    int64_t  amount;
    uint64_t nonce;
    uint64_t expiry_tick;
    bool     redeemed;
} settl_credit_note_t;

typedef struct {
    uint8_t  scaffold_id[32];
    uint8_t  station_pubkey[32];
    uint8_t  module_type;
    uint8_t  ring;
    uint8_t  slot;
    float    build_progress;
    bool     active;
} settl_construction_site_t;

typedef struct {
    uint8_t  player_pubkey[32];
    uint8_t  player_callsign[8];
    uint8_t  cause;
    uint64_t death_tick;
    float    credits_earned;
    float    credits_spent;
    float    ore_mined;
    uint32_t asteroids_fractured;
} settl_death_record_t;

typedef struct {
    uint8_t player_pubkey[32];
    float   balance;
    float   lifetime_supply;
} settl_ledger_entry_t;

enum {
    SETTL_MAX_FRAGMENT_OWNERS    = 512,
    SETTL_MAX_CREDIT_NOTES       = 64,
    SETTL_MAX_CONSTRUCTION_SITES = 32,
    SETTL_MAX_DEATH_RECORDS      = 128,
    SETTL_MAX_MANIFEST_UNITS     = 256,
    SETTL_MAX_LEDGER_ENTRIES     = 16,
};

typedef struct {
    cargo_unit_t         station_manifests[MAX_STATIONS][SETTL_MAX_MANIFEST_UNITS];
    uint16_t             station_manifest_counts[MAX_STATIONS];
    settl_ledger_entry_t station_ledgers[MAX_STATIONS][SETTL_MAX_LEDGER_ENTRIES];
    uint8_t              station_ledger_counts[MAX_STATIONS];

    settl_fragment_owner_t    fragment_owners[SETTL_MAX_FRAGMENT_OWNERS];
    uint16_t                  fragment_owner_count;
    settl_credit_note_t       credit_notes[SETTL_MAX_CREDIT_NOTES];
    uint16_t                  credit_note_count;
    settl_construction_site_t construction_sites[SETTL_MAX_CONSTRUCTION_SITES];
    uint16_t                  construction_site_count;
    settl_death_record_t      death_records[SETTL_MAX_DEATH_RECORDS];
    uint16_t                  death_record_count;

    uint32_t segment_index;
    uint64_t last_event_id;
} settlement_state_t;

typedef struct {
    uint32_t segment_index;
    uint8_t  state_root[32];
    uint8_t  prev_segment_root[32];
    uint64_t last_event_id;
    uint32_t event_count;
} settlement_checkpoint_t;

/*
 * Caller-owned provenance for one cargo identity referenced by a settlement
 * event.  The settlement engine borrows these pointers only for the duration
 * of settlement_apply_segment_trusted(); neither the pointers nor any receipt
 * bytes are retained in settlement_state_t.
 */
typedef struct {
    const cargo_receipt_t *receipt_chain;
    size_t receipt_count;
    const cargo_receipt_origin_proof_t *origin;
    cargo_receipt_authority_trust_t authority_trust;
} settlement_cargo_trust_evidence_t;

/*
 * Evidence is indexed exactly like the segment's event array.  Transfer and
 * sell events require one cargo entry; construction-input events require one
 * entry per input_pub in payload order.  Other events require zero entries.
 */
typedef struct {
    const settlement_cargo_trust_evidence_t *cargo;
    uint8_t cargo_count;
} settlement_event_trust_evidence_t;

typedef enum {
    SETTLEMENT_APPLY_OK = 0,
    SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS,
    SETTLEMENT_APPLY_REJECT_PAYLOAD_HASH,
    SETTLEMENT_APPLY_REJECT_MISSING_TRUST_EVIDENCE,
    SETTLEMENT_APPLY_REJECT_TRUST_EVIDENCE_COUNT,
    SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
    SETTLEMENT_APPLY_REJECT_RECEIPT_HOLDER,
    SETTLEMENT_APPLY_REJECT_RESOURCE,
    SETTLEMENT_APPLY_REJECT_EVENT,
    SETTLEMENT_APPLY_STATUS_COUNT
} settlement_apply_status_t;

/*
 * Stable semantic rejection detail.  For REJECT_CARGO_TRUST, cargo_trust
 * preserves the shared verifier's exact missing-origin, cargo-mismatch,
 * unknown, untrusted, revoked, lifecycle, and cryptographic verdict.
 */
typedef struct {
    settlement_apply_status_t status;
    uint32_t event_index;
    uint8_t cargo_index;
    uint8_t _reserved[3];
    cargo_receipt_trust_result_t cargo_trust;
} settlement_apply_result_t;

void settlement_state_init(settlement_state_t *s);
void settlement_compute_root(const settlement_state_t *s, uint8_t root_out[32]);

/*
 * Apply one non-provenance-sensitive settlement event. hdr points to a
 * chain_event_header_t (from server/chain_log.h — opaque here to avoid an
 * include dependency).  The payload hash is verified first.  Transfer, sell,
 * and construction-input events fail closed here because this legacy surface
 * has no trust-evidence parameter; import them through the trusted segment
 * API below.
 */
bool settlement_apply_event(settlement_state_t *s,
                            const void *hdr,
                            const uint8_t *payload,
                            uint16_t payload_len);

/*
 * Compatibility wrapper.  Segments without provenance-sensitive events keep
 * working; transfer, sell, and construction-input events reject because no
 * trust evidence is supplied.
 */
bool settlement_apply_segment(settlement_state_t *s,
                              const void *events,
                              const uint8_t **payloads,
                              const uint16_t *payload_lens,
                              uint32_t event_count,
                              settlement_checkpoint_t *cp_out);

/*
 * Validate every payload hash and every required cargo trust proof before
 * mutating state, then apply the segment transactionally.  On any rejection,
 * settlement state and cp_out remain byte-identical to their caller-provided
 * values.  result_out is optional and receives stable failure detail.
 */
bool settlement_apply_segment_trusted(
    settlement_state_t *s,
    const void *events,
    const uint8_t **payloads,
    const uint16_t *payload_lens,
    const settlement_event_trust_evidence_t *trust_evidence,
    uint32_t event_count,
    settlement_checkpoint_t *cp_out,
    settlement_apply_result_t *result_out);

const char *settlement_apply_status_name(settlement_apply_status_t status);

#ifdef __cplusplus
}
#endif

#endif
