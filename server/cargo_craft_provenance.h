/*
 * cargo_craft_provenance.h -- Truthful, bounded CRAFT V1 interpretation.
 *
 * CRAFT V1 binds a station signature to a canonical output description and
 * to distinct input pubkey bytes.  It does not prove that those inputs had a
 * verified origin, carried the recipe labels, were in station custody, or
 * were consumed exactly once.  Keep that distinction in one vocabulary used
 * by live trust decisions and offline audit/lineage tools.
 */
#ifndef SERVER_CARGO_CRAFT_PROVENANCE_H
#define SERVER_CARGO_CRAFT_PROVENANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CARGO_CRAFT_PROVENANCE_NOT_CRAFT = 0,
    CARGO_CRAFT_PROVENANCE_UNBOUND_V0,
    CARGO_CRAFT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED,
    CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1,
    CARGO_CRAFT_PROVENANCE_REJECT_BAD_ARGUMENTS,
    CARGO_CRAFT_PROVENANCE_REJECT_PAYLOAD_LENGTH,
    CARGO_CRAFT_PROVENANCE_REJECT_SEMANTICS_VERSION,
    CARGO_CRAFT_PROVENANCE_REJECT_RECIPE,
    CARGO_CRAFT_PROVENANCE_REJECT_INPUT_COUNT,
    CARGO_CRAFT_PROVENANCE_REJECT_ZERO_INPUT,
    CARGO_CRAFT_PROVENANCE_REJECT_DUPLICATE_INPUT,
    CARGO_CRAFT_PROVENANCE_REJECT_UNUSED_INPUT,
    CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS,
    CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_IDENTITY,
    CARGO_CRAFT_PROVENANCE_REJECT_AMBIGUOUS_OUTPUT_IDENTITY,
    CARGO_CRAFT_PROVENANCE_STATUS_COUNT,
} cargo_craft_provenance_status_t;

typedef struct {
    uint16_t recipe_id;
    uint16_t output_count;
    uint8_t input_count;
    uint8_t output_kind;
    uint8_t output_commodity;
    bool output_commodity_is_dynamic;
} cargo_craft_v1_recipe_shape_t;

typedef struct {
    cargo_craft_provenance_status_t status;
    uint16_t recipe_id;
    uint16_t output_index;
    uint8_t semantics_version;
    uint8_t input_count;
    bool output_index_known;
    bool station_attested;
    /*
     * These stay false for every V0/V1 result.  They are explicit fields so
     * callers cannot accidentally infer stronger proof from a valid output
     * identity or from the presence of input pubkey bytes.
     */
    bool input_lineage_proven;
    bool conservation_proven;
    uint8_t parent_merkle[32];
} cargo_craft_provenance_result_t;

/*
 * Return the frozen CRAFT V1 recipe shape.  This is deliberately versioned
 * rather than consulting a mutable recipe table while auditing historical
 * bytes.  Tests compare it with the current live catalog so a future recipe
 * change must introduce a new CRAFT semantics version instead of silently
 * reinterpreting V1.
 */
bool cargo_craft_v1_recipe_shape(
    uint16_t recipe_id,
    cargo_craft_v1_recipe_shape_t *out);

/*
 * Evaluate one exact 136-byte CRAFT payload.  `station_event_verified` means
 * the containing event's authority, signature, payload hash, linkage, and
 * event ordering were verified.  Without it, a canonical V1 payload is only
 * STRUCTURAL_V1_UNVERIFIED and must never be presented as an attestation.
 *
 * A STATION_ATTESTED_V1 result proves only station-signed structural
 * consistency.  input_lineage_proven and conservation_proven are always
 * false until a future, separately versioned proof schema implements them.
 */
cargo_craft_provenance_status_t cargo_craft_provenance_evaluate(
    const uint8_t *payload,
    size_t payload_len,
    bool station_event_verified,
    cargo_craft_provenance_result_t *out);

bool cargo_craft_provenance_is_structurally_valid(
    cargo_craft_provenance_status_t status);
bool cargo_craft_provenance_is_rejection(
    cargo_craft_provenance_status_t status);
const char *cargo_craft_provenance_status_name(
    cargo_craft_provenance_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_CRAFT_PROVENANCE_H */
