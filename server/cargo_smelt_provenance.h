/*
 * cargo_smelt_provenance.h -- Truthful, bounded SMELT V0/V1 interpretation.
 *
 * SMELT V1 binds a verified station event to one canonical ingot identity,
 * its visible commodity/grade labels, output index, and refinery context.
 * It does not prove how the fragment was mined or independently verify its
 * grade. Keep that distinction in one vocabulary shared by live lineage and
 * offline receipt tooling.
 */
#ifndef SERVER_CARGO_SMELT_PROVENANCE_H
#define SERVER_CARGO_SMELT_PROVENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CARGO_SMELT_PROVENANCE_NOT_SMELT = 0,
    CARGO_SMELT_PROVENANCE_UNBOUND_V0,
    CARGO_SMELT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED,
    CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1,
    CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS,
    CARGO_SMELT_PROVENANCE_REJECT_PAYLOAD_LENGTH,
    CARGO_SMELT_PROVENANCE_REJECT_SEMANTICS_VERSION,
    CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V0,
    CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V0,
    CARGO_SMELT_PROVENANCE_REJECT_AMBIGUOUS_V0,
    CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1,
    CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V1,
    CARGO_SMELT_PROVENANCE_REJECT_PREFIX_V1,
    CARGO_SMELT_PROVENANCE_REJECT_RESOURCE,
    CARGO_SMELT_PROVENANCE_STATUS_COUNT,
} cargo_smelt_provenance_status_t;

typedef struct {
    cargo_smelt_provenance_status_t status;
    uint8_t semantics_version;
    bool station_attested;
    /*
     * These stay false for every V0/V1 result. A station signature and a
     * canonical ingot identity do not bind the fragment to a legitimate
     * CLAIM_FRAGMENT mining proof.
     */
    bool mining_proven;
    bool grade_verified;
    bool output_index_known;
    uint16_t output_index;
    uint8_t fragment_pub[32];
    uint8_t output_pub[32];
    uint8_t prefix_class;
    uint8_t commodity;
    uint8_t grade;
    uint64_t refinery_context_tick;
    /*
     * Populated only for structurally valid V1. V0 omitted the material and
     * grade hash inputs and therefore cannot reconstruct canonical cargo.
     */
    cargo_unit_t output_cargo;
} cargo_smelt_provenance_result_t;

/*
 * Evaluate one exact 80-byte SMELT payload using explicit byte offsets and
 * little-endian reads; the packed wire bytes are never cast to a C struct.
 *
 * `station_event_verified` means the containing event's authority,
 * signature, payload hash, linkage, and event ordering were verified.
 * Without it, a canonical V1 payload is STRUCTURAL_V1_UNVERIFIED. A verified
 * V0 event remains UNBOUND_V0 because its identity omitted commodity/grade.
 *
 * V0 evaluation performs the frozen, bounded uint16 output-index recovery.
 * Callers walking large live histories may skip V0 when they do not need
 * audit-only recovery.
 */
cargo_smelt_provenance_status_t cargo_smelt_provenance_evaluate(
    const uint8_t *payload,
    size_t payload_len,
    bool station_event_verified,
    cargo_smelt_provenance_result_t *out);

bool cargo_smelt_provenance_is_structurally_valid(
    cargo_smelt_provenance_status_t status);
bool cargo_smelt_provenance_is_rejection(
    cargo_smelt_provenance_status_t status);
const char *cargo_smelt_provenance_status_name(
    cargo_smelt_provenance_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_SMELT_PROVENANCE_H */
