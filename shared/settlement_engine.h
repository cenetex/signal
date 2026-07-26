#ifndef SHARED_SETTLEMENT_ENGINE_H
#define SHARED_SETTLEMENT_ENGINE_H

#include <stdbool.h>
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
 * Stable semantic result for a segment import. Receipt-specific failures map
 * one-to-one from cargo_receipt_trust_verify() so callers never have to infer
 * whether a rejection came from cryptography, origin binding, or authority
 * policy.
 */
typedef enum {
    SETTL_IMPORT_OK = 0,
    SETTL_IMPORT_REJECT_BAD_ARGUMENTS,
    SETTL_IMPORT_REJECT_PAYLOAD_HASH,
    SETTL_IMPORT_REJECT_MISSING_EVIDENCE,
    SETTL_IMPORT_REJECT_EVIDENCE_COUNT,
    SETTL_IMPORT_REJECT_EVIDENCE_EVENT,
    SETTL_IMPORT_REJECT_EVIDENCE_CARGO,
    SETTL_IMPORT_REJECT_TRUST_BAD_ARGUMENTS,
    SETTL_IMPORT_REJECT_TRUST_CHAIN,
    SETTL_IMPORT_REJECT_TRUST_MISSING_ORIGIN,
    SETTL_IMPORT_REJECT_TRUST_ORIGIN_EVENT_TYPE,
    SETTL_IMPORT_REJECT_TRUST_ORIGIN_CARGO,
    SETTL_IMPORT_REJECT_TRUST_ORIGIN_PIN,
    SETTL_IMPORT_REJECT_TRUST_ORIGIN_AUTHORITY,
    SETTL_IMPORT_REJECT_TRUST_UNKNOWN_AUTHORITY,
    SETTL_IMPORT_REJECT_TRUST_UNTRUSTED_AUTHORITY,
    SETTL_IMPORT_REJECT_TRUST_REVOKED_AUTHORITY,
    SETTL_IMPORT_REJECT_EVENT
} settlement_import_status_t;

typedef struct {
    settlement_import_status_t status;
    uint32_t event_index;              /* UINT32_MAX when no event was reached */
    uint8_t cargo_index;               /* UINT8_MAX when not cargo-specific */
    cargo_receipt_result_t chain_result;
} settlement_import_result_t;

/*
 * Caller-owned, read-only evidence for one cargo identity in one settlement
 * event. The origin proof is embedded by value. receipt_chain is borrowed
 * only for the duration of settlement_apply_segment_trusted(); neither it nor
 * any origin address is retained in settlement state or checkpoint output.
 */
typedef struct {
    uint64_t event_id;
    uint8_t cargo_pub[32];
    const cargo_receipt_t *receipt_chain;
    uint8_t receipt_count;
    bool origin_present;
    cargo_receipt_origin_proof_t origin;
    cargo_receipt_authority_trust_t authority_trust;
} settlement_cargo_trust_evidence_t;

typedef struct {
    const settlement_cargo_trust_evidence_t *cargo;
    uint8_t cargo_count;
} settlement_event_trust_evidence_t;

void settlement_state_init(settlement_state_t *s);
void settlement_compute_root(const settlement_state_t *s, uint8_t root_out[32]);
const char *settlement_import_status_name(settlement_import_status_t status);

/*
 * Apply one non-provenance-sensitive settlement event. hdr points to a
 * chain_event_header_t (from server/chain_log.h — opaque here to avoid include
 * dependency). TRANSFER, SELL, and DELIVER_CONSTRUCTION_INPUT deliberately
 * reject here; import them through settlement_apply_segment_trusted() so trust
 * evidence cannot be bypassed.
 */
bool settlement_apply_event(settlement_state_t *s,
                            const void *hdr,
                            const uint8_t *payload,
                            uint16_t payload_len);

bool settlement_apply_segment(settlement_state_t *s,
                              const void *events,
                              const uint8_t **payloads,
                              const uint16_t *payload_lens,
                              uint32_t event_count,
                              settlement_checkpoint_t *cp_out);

/*
 * Receipt-aware atomic import. evidence is parallel to events and may be NULL
 * only when the segment contains no provenance-sensitive event. TRANSFER
 * (0x04), SELL (0x11), and DELIVER_CONSTRUCTION_INPUT (0x21) require exact
 * event/cargo-bound evidence. The full segment is preflighted before applying
 * to a private state copy, so rejection leaves both *s and *cp_out untouched.
 */
bool settlement_apply_segment_trusted(
    settlement_state_t *s,
    const void *events,
    const uint8_t **payloads,
    const uint16_t *payload_lens,
    const settlement_event_trust_evidence_t *evidence,
    uint32_t event_count,
    settlement_checkpoint_t *cp_out,
    settlement_import_result_t *result_out);

#ifdef __cplusplus
}
#endif

#endif
