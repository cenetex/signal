#include "cargo_legacy_classify.h"

#include "base58.h"
#include "sha256.h"

#include <string.h>

enum {
    CARGO_LEGACY_SMELT_FRAGMENT_OFFSET = 0,
    CARGO_LEGACY_SMELT_OUTPUT_OFFSET = 32,
    CARGO_LEGACY_SMELT_PREFIX_OFFSET = 64,
    CARGO_LEGACY_SMELT_SEMANTICS_OFFSET = 65,
    CARGO_LEGACY_SMELT_V0_PAD_END = 72,
    CARGO_LEGACY_SMELT_MINED_BLOCK_OFFSET = 72,

    CARGO_LEGACY_CRAFT_RECIPE_OFFSET = 0,
    CARGO_LEGACY_CRAFT_INPUT_COUNT_OFFSET = 2,
    CARGO_LEGACY_CRAFT_SEMANTICS_OFFSET = 3,
    CARGO_LEGACY_CRAFT_V0_PAD_END = 8,
    CARGO_LEGACY_CRAFT_OUTPUT_OFFSET = 8,
    CARGO_LEGACY_CRAFT_INPUTS_OFFSET = 40,

    CARGO_LEGACY_SEMANTICS_V0 = 0,
    CARGO_LEGACY_SEMANTICS_V1 = 1,
    CARGO_LEGACY_BASE58_CAP = 46,
};

static const uint8_t CARGO_LEGACY_IDENTITY_DOMAIN[8] = {
    'S', 'I', 'G', 'N', 'A', 'L', 'v', '1'
};

static bool cargo_legacy_pub_is_zero(
    const uint8_t pub[CARGO_LEGACY_PUBKEY_SIZE]) {
    static const uint8_t zero[CARGO_LEGACY_PUBKEY_SIZE] = {0};
    return memcmp(pub, zero, sizeof(zero)) == 0;
}

static uint16_t cargo_legacy_read_u16_le(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}

static uint64_t cargo_legacy_read_u64_le(const uint8_t *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++)
        value |= (uint64_t)bytes[i] << (i * 8u);
    return value;
}

static bool cargo_legacy_v0_pad_is_zero(
    const uint8_t *payload,
    size_t begin,
    size_t end) {
    for (size_t i = begin; i < end; i++) {
        if (payload[i] != 0u) return false;
    }
    return true;
}

/*
 * The historical merkle helper sorted the leaves lexicographically, hashed
 * adjacent pairs, and duplicated an unpaired final leaf.  V0 CRAFT supported
 * at most three inputs, so fixed stack storage reproduces it without qsort or
 * allocation.
 */
static bool cargo_legacy_parent_merkle_v0(
    const uint8_t input_pubs[CARGO_LEGACY_RECIPE_INPUT_MAX]
                            [CARGO_LEGACY_PUBKEY_SIZE],
    uint8_t input_count,
    uint8_t out_root[CARGO_LEGACY_PUBKEY_SIZE]) {
    uint8_t level[CARGO_LEGACY_RECIPE_INPUT_MAX]
                 [CARGO_LEGACY_PUBKEY_SIZE] = {{0}};
    uint8_t next[CARGO_LEGACY_RECIPE_INPUT_MAX]
                [CARGO_LEGACY_PUBKEY_SIZE] = {{0}};
    uint8_t swap[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t pair[CARGO_LEGACY_PUBKEY_SIZE * 2];
    size_t level_count = input_count;

    if (!input_pubs || !out_root ||
        input_count == 0u ||
        input_count > CARGO_LEGACY_RECIPE_INPUT_MAX) {
        return false;
    }

    memcpy(level, input_pubs,
           (size_t)input_count * CARGO_LEGACY_PUBKEY_SIZE);
    for (size_t i = 1; i < input_count; i++) {
        size_t j = i;
        while (j > 0 &&
               memcmp(level[j - 1], level[j],
                      CARGO_LEGACY_PUBKEY_SIZE) > 0) {
            memcpy(swap, level[j - 1], sizeof(swap));
            memcpy(level[j - 1], level[j], sizeof(swap));
            memcpy(level[j], swap, sizeof(swap));
            j--;
        }
    }

    while (level_count > 1u) {
        size_t next_count = 0;
        for (size_t i = 0; i < level_count; i += 2u) {
            const uint8_t *right =
                i + 1u < level_count ? level[i + 1u] : level[i];
            memcpy(pair, level[i], CARGO_LEGACY_PUBKEY_SIZE);
            memcpy(&pair[CARGO_LEGACY_PUBKEY_SIZE], right,
                   CARGO_LEGACY_PUBKEY_SIZE);
            sha256_bytes(pair, sizeof(pair), next[next_count]);
            next_count++;
        }
        memcpy(level, next,
               next_count * CARGO_LEGACY_PUBKEY_SIZE);
        level_count = next_count;
    }

    memcpy(out_root, level[0], CARGO_LEGACY_PUBKEY_SIZE);
    return true;
}

const char *cargo_legacy_status_name(cargo_legacy_status_t status) {
    switch (status) {
        case CARGO_LEGACY_STATUS_BAD_ARGUMENTS:
            return "bad_arguments";
        case CARGO_LEGACY_STATUS_MALFORMED:
            return "malformed";
        case CARGO_LEGACY_STATUS_CURRENT_V1:
            return "current_v1";
        case CARGO_LEGACY_STATUS_UNBOUND_V0:
            return "unbound_v0";
        case CARGO_LEGACY_STATUS_NONE:
            return "none";
        case CARGO_LEGACY_STATUS_AMBIGUOUS:
            return "ambiguous";
        case CARGO_LEGACY_STATUS_RESOURCE:
            return "resource";
        case CARGO_LEGACY_STATUS_COUNT:
            break;
    }
    return "unknown";
}

bool cargo_legacy_identity_v0_derive(
    uint16_t recipe_id,
    const uint8_t parent_or_root[CARGO_LEGACY_PUBKEY_SIZE],
    uint16_t output_index,
    uint8_t out_pub[CARGO_LEGACY_PUBKEY_SIZE]) {
    uint8_t preimage[8 + 2 + CARGO_LEGACY_PUBKEY_SIZE + 2];
    uint8_t digest[CARGO_LEGACY_PUBKEY_SIZE];

    if (!parent_or_root || !out_pub) return false;

    memcpy(preimage, CARGO_LEGACY_IDENTITY_DOMAIN,
           sizeof(CARGO_LEGACY_IDENTITY_DOMAIN));
    preimage[8] = (uint8_t)recipe_id;
    preimage[9] = (uint8_t)(recipe_id >> 8);
    memcpy(&preimage[10], parent_or_root,
           CARGO_LEGACY_PUBKEY_SIZE);
    preimage[42] = (uint8_t)output_index;
    preimage[43] = (uint8_t)(output_index >> 8);
    sha256_bytes(preimage, sizeof(preimage), digest);
    memcpy(out_pub, digest, sizeof(digest));
    return true;
}

bool cargo_legacy_prefix_class_v0_derive(
    const uint8_t pub[CARGO_LEGACY_PUBKEY_SIZE],
    uint8_t *out_class) {
    char encoded[CARGO_LEGACY_BASE58_CAP];
    uint8_t result = CARGO_LEGACY_PREFIX_ANONYMOUS;
    size_t encoded_len;

    if (!pub || !out_class) return false;
    encoded_len = base58_encode(
        pub, CARGO_LEGACY_PUBKEY_SIZE, encoded, sizeof(encoded));
    if (encoded_len == 0u) return false;

    if (encoded_len < 4u) {
        result = CARGO_LEGACY_PREFIX_ANONYMOUS;
    } else if (encoded[0] == 'R' &&
               encoded[1] == 'A' &&
               encoded[2] == 'T' &&
               encoded[3] == 'i') {
        result = CARGO_LEGACY_PREFIX_RATI;
    } else {
        switch (encoded[0]) {
            case 'M':
                result = CARGO_LEGACY_PREFIX_M;
                break;
            case 'H':
                result = CARGO_LEGACY_PREFIX_H;
                break;
            case 'T':
                result = CARGO_LEGACY_PREFIX_T;
                break;
            case 'S':
                result = CARGO_LEGACY_PREFIX_S;
                break;
            case 'F':
                result = CARGO_LEGACY_PREFIX_F;
                break;
            case 'K':
                result = CARGO_LEGACY_PREFIX_K;
                break;
            default:
                result = CARGO_LEGACY_PREFIX_ANONYMOUS;
                break;
        }
    }

    *out_class = result;
    return true;
}

cargo_legacy_status_t cargo_legacy_recover_output_index_v0(
    uint16_t recipe_id,
    const uint8_t parent_or_root[CARGO_LEGACY_PUBKEY_SIZE],
    const uint8_t legacy_output_pub[CARGO_LEGACY_PUBKEY_SIZE],
    uint32_t candidate_count,
    uint16_t *out_index) {
    uint8_t candidate[CARGO_LEGACY_PUBKEY_SIZE];
    uint16_t found_index = 0;
    uint32_t matches = 0;

    if (!parent_or_root || !legacy_output_pub || !out_index ||
        candidate_count == 0u) {
        return CARGO_LEGACY_STATUS_BAD_ARGUMENTS;
    }
    if (candidate_count > CARGO_LEGACY_OUTPUT_INDEX_COUNT)
        return CARGO_LEGACY_STATUS_RESOURCE;

    for (uint32_t i = 0; i < candidate_count; i++) {
        if (!cargo_legacy_identity_v0_derive(
                recipe_id, parent_or_root, (uint16_t)i,
                candidate)) {
            return CARGO_LEGACY_STATUS_BAD_ARGUMENTS;
        }
        if (memcmp(candidate, legacy_output_pub,
                   CARGO_LEGACY_PUBKEY_SIZE) != 0) {
            continue;
        }
        if (matches == 0u) found_index = (uint16_t)i;
        matches++;
        if (matches > 1u)
            return CARGO_LEGACY_STATUS_AMBIGUOUS;
    }

    if (matches == 0u) return CARGO_LEGACY_STATUS_NONE;
    *out_index = found_index;
    return CARGO_LEGACY_STATUS_UNBOUND_V0;
}

cargo_legacy_status_t cargo_legacy_classify_smelt_v0(
    const uint8_t *payload,
    size_t payload_len,
    cargo_legacy_smelt_v0_result_t *out) {
    cargo_legacy_smelt_v0_result_t result = {0};
    cargo_legacy_status_t recovered;
    uint8_t derived_prefix;

    if (!payload || !out)
        return CARGO_LEGACY_STATUS_BAD_ARGUMENTS;
    if (payload_len != CARGO_LEGACY_SMELT_V0_PAYLOAD_SIZE)
        return CARGO_LEGACY_STATUS_MALFORMED;

    if (payload[CARGO_LEGACY_SMELT_SEMANTICS_OFFSET] ==
        CARGO_LEGACY_SEMANTICS_V1) {
        return CARGO_LEGACY_STATUS_CURRENT_V1;
    }
    if (payload[CARGO_LEGACY_SMELT_SEMANTICS_OFFSET] !=
            CARGO_LEGACY_SEMANTICS_V0 ||
        !cargo_legacy_v0_pad_is_zero(
            payload, CARGO_LEGACY_SMELT_SEMANTICS_OFFSET,
            CARGO_LEGACY_SMELT_V0_PAD_END)) {
        return CARGO_LEGACY_STATUS_MALFORMED;
    }

    if (cargo_legacy_pub_is_zero(
            &payload[CARGO_LEGACY_SMELT_FRAGMENT_OFFSET]) ||
        cargo_legacy_pub_is_zero(
            &payload[CARGO_LEGACY_SMELT_OUTPUT_OFFSET])) {
        return CARGO_LEGACY_STATUS_MALFORMED;
    }
    if (!cargo_legacy_prefix_class_v0_derive(
            &payload[CARGO_LEGACY_SMELT_OUTPUT_OFFSET],
            &derived_prefix) ||
        derived_prefix > CARGO_LEGACY_PREFIX_RATI ||
        payload[CARGO_LEGACY_SMELT_PREFIX_OFFSET] != derived_prefix ||
        payload[CARGO_LEGACY_SMELT_PREFIX_OFFSET] ==
            CARGO_LEGACY_PREFIX_COMMISSIONED) {
        return CARGO_LEGACY_STATUS_MALFORMED;
    }

    memcpy(result.fragment_pub,
           &payload[CARGO_LEGACY_SMELT_FRAGMENT_OFFSET],
           CARGO_LEGACY_PUBKEY_SIZE);
    memcpy(result.legacy_output_pub,
           &payload[CARGO_LEGACY_SMELT_OUTPUT_OFFSET],
           CARGO_LEGACY_PUBKEY_SIZE);
    result.legacy_prefix_class =
        payload[CARGO_LEGACY_SMELT_PREFIX_OFFSET];
    result.legacy_mined_block = cargo_legacy_read_u64_le(
        &payload[CARGO_LEGACY_SMELT_MINED_BLOCK_OFFSET]);

    recovered = cargo_legacy_recover_output_index_v0(
        CARGO_LEGACY_RECIPE_SMELT_ID,
        result.fragment_pub,
        result.legacy_output_pub,
        CARGO_LEGACY_OUTPUT_INDEX_COUNT,
        &result.output_index);
    if (recovered != CARGO_LEGACY_STATUS_UNBOUND_V0)
        return recovered;

    *out = result;
    return CARGO_LEGACY_STATUS_UNBOUND_V0;
}

cargo_legacy_status_t cargo_legacy_classify_craft_v0(
    const uint8_t *payload,
    size_t payload_len,
    const cargo_legacy_recipe_shape_t *shape,
    cargo_legacy_craft_v0_result_t *out) {
    cargo_legacy_craft_v0_result_t result = {0};
    cargo_legacy_status_t recovered;
    uint16_t payload_recipe;
    uint8_t payload_input_count;

    if (!payload || !shape || !out)
        return CARGO_LEGACY_STATUS_BAD_ARGUMENTS;
    if (payload_len != CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE)
        return CARGO_LEGACY_STATUS_MALFORMED;

    if (payload[CARGO_LEGACY_CRAFT_SEMANTICS_OFFSET] ==
        CARGO_LEGACY_SEMANTICS_V1) {
        return CARGO_LEGACY_STATUS_CURRENT_V1;
    }
    if (payload[CARGO_LEGACY_CRAFT_SEMANTICS_OFFSET] !=
            CARGO_LEGACY_SEMANTICS_V0 ||
        !cargo_legacy_v0_pad_is_zero(
            payload, CARGO_LEGACY_CRAFT_SEMANTICS_OFFSET,
            CARGO_LEGACY_CRAFT_V0_PAD_END)) {
        return CARGO_LEGACY_STATUS_MALFORMED;
    }

    if (shape->input_count == 0u ||
        shape->input_count > CARGO_LEGACY_RECIPE_INPUT_MAX ||
        shape->output_count == 0u) {
        return CARGO_LEGACY_STATUS_BAD_ARGUMENTS;
    }
    if (shape->output_count > CARGO_LEGACY_OUTPUT_INDEX_COUNT)
        return CARGO_LEGACY_STATUS_RESOURCE;

    payload_recipe = cargo_legacy_read_u16_le(
        &payload[CARGO_LEGACY_CRAFT_RECIPE_OFFSET]);
    payload_input_count =
        payload[CARGO_LEGACY_CRAFT_INPUT_COUNT_OFFSET];
    if (payload_recipe == CARGO_LEGACY_RECIPE_SMELT_ID ||
        payload_recipe != shape->recipe_id ||
        payload_input_count != shape->input_count ||
        payload_input_count == 0u ||
        payload_input_count > CARGO_LEGACY_RECIPE_INPUT_MAX ||
        cargo_legacy_pub_is_zero(
            &payload[CARGO_LEGACY_CRAFT_OUTPUT_OFFSET])) {
        return CARGO_LEGACY_STATUS_MALFORMED;
    }

    for (size_t i = 0; i < CARGO_LEGACY_RECIPE_INPUT_MAX; i++) {
        const uint8_t *input =
            &payload[CARGO_LEGACY_CRAFT_INPUTS_OFFSET +
                     i * CARGO_LEGACY_PUBKEY_SIZE];
        if (i < payload_input_count) {
            if (cargo_legacy_pub_is_zero(input))
                return CARGO_LEGACY_STATUS_MALFORMED;
            for (size_t j = 0; j < i; j++) {
                const uint8_t *prior =
                    &payload[CARGO_LEGACY_CRAFT_INPUTS_OFFSET +
                             j * CARGO_LEGACY_PUBKEY_SIZE];
                if (memcmp(input, prior,
                           CARGO_LEGACY_PUBKEY_SIZE) == 0) {
                    return CARGO_LEGACY_STATUS_MALFORMED;
                }
            }
        } else if (!cargo_legacy_pub_is_zero(input)) {
            return CARGO_LEGACY_STATUS_MALFORMED;
        }
    }

    result.recipe_id = payload_recipe;
    result.input_count = payload_input_count;
    memcpy(result.legacy_output_pub,
           &payload[CARGO_LEGACY_CRAFT_OUTPUT_OFFSET],
           CARGO_LEGACY_PUBKEY_SIZE);
    memcpy(result.input_pubs,
           &payload[CARGO_LEGACY_CRAFT_INPUTS_OFFSET],
           sizeof(result.input_pubs));
    if (!cargo_legacy_parent_merkle_v0(
            (const uint8_t (*)[CARGO_LEGACY_PUBKEY_SIZE])
                result.input_pubs,
            result.input_count,
            result.parent_merkle)) {
        return CARGO_LEGACY_STATUS_MALFORMED;
    }

    recovered = cargo_legacy_recover_output_index_v0(
        result.recipe_id,
        result.parent_merkle,
        result.legacy_output_pub,
        shape->output_count,
        &result.output_index);
    if (recovered != CARGO_LEGACY_STATUS_UNBOUND_V0)
        return recovered;

    *out = result;
    return CARGO_LEGACY_STATUS_UNBOUND_V0;
}
