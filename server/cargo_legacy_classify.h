#ifndef SERVER_CARGO_LEGACY_CLASSIFY_H
#define SERVER_CARGO_LEGACY_CLASSIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CARGO_LEGACY_PUBKEY_SIZE = 32,
    CARGO_LEGACY_RECIPE_INPUT_MAX = 3,
    CARGO_LEGACY_RECIPE_SMELT_ID = 0,
    CARGO_LEGACY_SMELT_V0_PAYLOAD_SIZE = 80,
    CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE = 136,
};

/*
 * Frozen V0 mining_pubkey_class() values.  COMMISSIONED existed as a
 * reserved enum value but the historical pubkey-derived classifier could
 * never emit it.
 */
enum {
    CARGO_LEGACY_PREFIX_ANONYMOUS = 0,
    CARGO_LEGACY_PREFIX_M = 1,
    CARGO_LEGACY_PREFIX_H = 2,
    CARGO_LEGACY_PREFIX_T = 3,
    CARGO_LEGACY_PREFIX_S = 4,
    CARGO_LEGACY_PREFIX_F = 5,
    CARGO_LEGACY_PREFIX_K = 6,
    CARGO_LEGACY_PREFIX_RATI = 7,
    CARGO_LEGACY_PREFIX_COMMISSIONED = 8,
};

#define CARGO_LEGACY_OUTPUT_INDEX_COUNT 65536u

/*
 * Stable migration/audit verdicts.  UNBOUND_V0 is deliberately not an
 * acceptance or trust verdict: it says only that the frozen V0 bytes are
 * well-formed and their legacy output index has one unique pre-binding hash
 * match.  The material/grade labels omitted by that hash remain unproven.
 *
 * CURRENT_V1 is likewise only a routing verdict.  Current payloads still need
 * the normal strict V1 semantic validator.
 */
typedef enum {
    CARGO_LEGACY_STATUS_BAD_ARGUMENTS = 0,
    CARGO_LEGACY_STATUS_MALFORMED = 1,
    CARGO_LEGACY_STATUS_CURRENT_V1 = 2,
    CARGO_LEGACY_STATUS_UNBOUND_V0 = 3,
    CARGO_LEGACY_STATUS_NONE = 4,
    CARGO_LEGACY_STATUS_AMBIGUOUS = 5,
    CARGO_LEGACY_STATUS_RESOURCE = 6,
    CARGO_LEGACY_STATUS_COUNT = 7,
} cargo_legacy_status_t;

const char *cargo_legacy_status_name(cargo_legacy_status_t status);

/*
 * An authoritative historical recipe-catalog view for the source save/event
 * schema, supplied by the future inventory scanner.  It is intentionally
 * data-only so this classifier has no mutable world, persistence, signing,
 * allocation, or recipe-table dependency.
 */
typedef struct {
    uint16_t recipe_id;
    uint8_t input_count;
    uint8_t _reserved;
    uint32_t output_count;
} cargo_legacy_recipe_shape_t;

typedef struct {
    uint8_t fragment_pub[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t legacy_output_pub[CARGO_LEGACY_PUBKEY_SIZE];
    uint16_t output_index;
    uint8_t legacy_prefix_class;
    uint8_t _reserved;
    uint64_t legacy_mined_block;
} cargo_legacy_smelt_v0_result_t;

typedef struct {
    uint16_t recipe_id;
    uint16_t output_index;
    uint8_t input_count;
    uint8_t _reserved[3];
    uint8_t parent_merkle[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t legacy_output_pub[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t input_pubs[CARGO_LEGACY_RECIPE_INPUT_MAX]
                           [CARGO_LEGACY_PUBKEY_SIZE];
} cargo_legacy_craft_v0_result_t;

/*
 * Frozen pre-binding identity:
 *
 * SHA256("SIGNALv1" || recipe_id_le16 || parent_or_root[32] ||
 *        output_index_le16)
 *
 * This formula did not bind SMELT commodity/grade or CRAFT output grade.
 */
bool cargo_legacy_identity_v0_derive(
    uint16_t recipe_id,
    const uint8_t parent_or_root[CARGO_LEGACY_PUBKEY_SIZE],
    uint16_t output_index,
    uint8_t out_pub[CARGO_LEGACY_PUBKEY_SIZE]);

/*
 * Reproduce the historical base58-leading-character prefix classifier
 * without consulting mutable game state.  The returned class is always in
 * ANONYMOUS..RATI; COMMISSIONED is intentionally unreachable.  out_class is
 * unchanged on bad arguments.
 */
bool cargo_legacy_prefix_class_v0_derive(
    const uint8_t pub[CARGO_LEGACY_PUBKEY_SIZE],
    uint8_t *out_class);

/*
 * Search exactly candidate_count indices starting at zero.  The range is
 * capped at the complete uint16_t space.  A unique match returns
 * UNBOUND_V0; zero or multiple matches return NONE or AMBIGUOUS.  out_index
 * is unchanged on every non-UNBOUND_V0 result.
 */
cargo_legacy_status_t cargo_legacy_recover_output_index_v0(
    uint16_t recipe_id,
    const uint8_t parent_or_root[CARGO_LEGACY_PUBKEY_SIZE],
    const uint8_t legacy_output_pub[CARGO_LEGACY_PUBKEY_SIZE],
    uint32_t candidate_count,
    uint16_t *out_index);

/*
 * Classify the exact frozen 80-byte SMELT payload:
 * fragment[0..31], output[32..63], prefix[64], V0 zero pad[65..71],
 * mined_block_le64[72..79].  Byte 65 equal to one identifies the current V1
 * layout for handoff to the current validator.
 *
 * A valid V0 scan covers the full uint16_t output-index space.  The prefix
 * byte must exactly equal the frozen pubkey-derived class (0..7);
 * COMMISSIONED=8 and out-of-range/conflicting labels are malformed.
 * mined_block is preserved as historical evidence but is not a legacy cargo
 * hash input.  out is written only for UNBOUND_V0.
 */
cargo_legacy_status_t cargo_legacy_classify_smelt_v0(
    const uint8_t *payload,
    size_t payload_len,
    cargo_legacy_smelt_v0_result_t *out);

/*
 * Classify the exact frozen 136-byte CRAFT payload:
 * recipe_le16[0..1], input_count[2], V0 zero pad[3..7], output[8..39],
 * input pubs[40..135].  Byte 3 equal to one identifies the current V1
 * layout for handoff to the current validator.
 *
 * shape must be the trusted historical recipe-catalog entry for the payload's
 * source schema.  Active input pubs must be non-zero and distinct; every
 * unused slot must be zero.  The legacy identity is recovered over
 * shape->output_count.  out is written only for UNBOUND_V0.
 */
cargo_legacy_status_t cargo_legacy_classify_craft_v0(
    const uint8_t *payload,
    size_t payload_len,
    const cargo_legacy_recipe_shape_t *shape,
    cargo_legacy_craft_v0_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_LEGACY_CLASSIFY_H */
