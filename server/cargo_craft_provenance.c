#include "cargo_craft_provenance.h"

#include "cargo_legacy_classify.h"
#include "sha256.h"

#include <stdlib.h>
#include <string.h>

enum {
    CRAFT_V1_PAYLOAD_SIZE = 136,
    CRAFT_RECIPE_OFFSET = 0,
    CRAFT_INPUT_COUNT_OFFSET = 2,
    CRAFT_SEMANTICS_OFFSET = 3,
    CRAFT_OUTPUT_KIND_OFFSET = 4,
    CRAFT_OUTPUT_COMMODITY_OFFSET = 5,
    CRAFT_OUTPUT_GRADE_OFFSET = 6,
    CRAFT_OUTPUT_QUANTITY_OFFSET = 7,
    CRAFT_OUTPUT_PUB_OFFSET = 8,
    CRAFT_INPUT_PUBS_OFFSET = 40,
    CRAFT_PUB_SIZE = 32,
};

static const uint8_t CRAFT_IDENTITY_DOMAIN[8] = {
    'S', 'I', 'G', 'N', 'A', 'L', 'v', '1'
};

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] |
           (uint16_t)((uint16_t)p[1] << 8);
}

static bool pub_is_zero(const uint8_t pub[CRAFT_PUB_SIZE]) {
    static const uint8_t zero[CRAFT_PUB_SIZE] = {0};
    return memcmp(pub, zero, sizeof(zero)) == 0;
}

static bool kind_matches_commodity(uint8_t kind, uint8_t commodity) {
    switch ((cargo_kind_t)kind) {
    case CARGO_KIND_INGOT:
        return commodity == (uint8_t)COMMODITY_FERRITE_INGOT ||
               commodity == (uint8_t)COMMODITY_CUPRITE_INGOT ||
               commodity == (uint8_t)COMMODITY_CRYSTAL_INGOT;
    case CARGO_KIND_FRAME:
        return commodity == (uint8_t)COMMODITY_FRAME;
    case CARGO_KIND_LASER:
        return commodity == (uint8_t)COMMODITY_LASER_MODULE;
    case CARGO_KIND_TRACTOR:
        return commodity == (uint8_t)COMMODITY_TRACTOR_MODULE;
    case CARGO_KIND_REPAIR_KIT:
        return commodity == (uint8_t)COMMODITY_REPAIR_KIT;
    case CARGO_KIND_ENGINE:
        return commodity == (uint8_t)COMMODITY_ENGINE_MODULE;
    case CARGO_KIND_ORE:
    case CARGO_KIND_COUNT:
        return false;
    }
    return false;
}

bool cargo_craft_v1_recipe_shape(
    uint16_t recipe_id,
    cargo_craft_v1_recipe_shape_t *out) {
    cargo_craft_v1_recipe_shape_t shape = {0};
    if (!out) return false;
    switch ((recipe_id_t)recipe_id) {
    case RECIPE_FRAME_BASIC:
        shape = (cargo_craft_v1_recipe_shape_t){
            .recipe_id = (uint16_t)RECIPE_FRAME_BASIC,
            .output_count = 4,
            .input_count = 1,
            .output_kind = (uint8_t)CARGO_KIND_FRAME,
            .output_commodity = (uint8_t)COMMODITY_FRAME,
        };
        break;
    case RECIPE_LASER_BASIC:
        shape = (cargo_craft_v1_recipe_shape_t){
            .recipe_id = (uint16_t)RECIPE_LASER_BASIC,
            .output_count = 1,
            .input_count = 2,
            .output_kind = (uint8_t)CARGO_KIND_LASER,
            .output_commodity = (uint8_t)COMMODITY_LASER_MODULE,
        };
        break;
    case RECIPE_TRACTOR_COIL:
        shape = (cargo_craft_v1_recipe_shape_t){
            .recipe_id = (uint16_t)RECIPE_TRACTOR_COIL,
            .output_count = 1,
            .input_count = 2,
            .output_kind = (uint8_t)CARGO_KIND_TRACTOR,
            .output_commodity = (uint8_t)COMMODITY_TRACTOR_MODULE,
        };
        break;
    case RECIPE_REPAIR_KIT_FAB:
        shape = (cargo_craft_v1_recipe_shape_t){
            .recipe_id = (uint16_t)RECIPE_REPAIR_KIT_FAB,
            .output_count = 100,
            .input_count = 3,
            .output_kind = (uint8_t)CARGO_KIND_REPAIR_KIT,
            .output_commodity = (uint8_t)COMMODITY_REPAIR_KIT,
        };
        break;
    case RECIPE_ENGINE_BASIC:
        shape = (cargo_craft_v1_recipe_shape_t){
            .recipe_id = (uint16_t)RECIPE_ENGINE_BASIC,
            .output_count = 1,
            .input_count = 3,
            .output_kind = (uint8_t)CARGO_KIND_ENGINE,
            .output_commodity = (uint8_t)COMMODITY_ENGINE_MODULE,
        };
        break;
    case RECIPE_LEGACY_MIGRATE:
        shape = (cargo_craft_v1_recipe_shape_t){
            .recipe_id = (uint16_t)RECIPE_LEGACY_MIGRATE,
            .output_count = 0,
            .input_count = 0,
            .output_kind = (uint8_t)CARGO_KIND_COUNT,
            .output_commodity = (uint8_t)COMMODITY_COUNT,
            .output_commodity_is_dynamic = true,
        };
        break;
    case RECIPE_SMELT:
    case RECIPE_COUNT:
        return false;
    }
    *out = shape;
    return true;
}

static int compare_pub(const void *lhs, const void *rhs) {
    return memcmp(lhs, rhs, CRAFT_PUB_SIZE);
}

static bool parent_merkle(
    const uint8_t input_pubs[RECIPE_INPUT_MAX][CRAFT_PUB_SIZE],
    uint8_t input_count,
    uint8_t out[CRAFT_PUB_SIZE]) {
    uint8_t level[RECIPE_INPUT_MAX][CRAFT_PUB_SIZE] = {{0}};
    uint8_t next[RECIPE_INPUT_MAX][CRAFT_PUB_SIZE] = {{0}};
    size_t count = input_count;
    if (!input_pubs || !out || input_count == 0u ||
        input_count > RECIPE_INPUT_MAX) {
        return false;
    }
    memcpy(level, input_pubs,
           (size_t)input_count * CRAFT_PUB_SIZE);
    qsort(level, input_count, CRAFT_PUB_SIZE, compare_pub);
    while (count > 1u) {
        size_t next_count = 0;
        for (size_t i = 0; i < count; i += 2u) {
            uint8_t pair[CRAFT_PUB_SIZE * 2u];
            const uint8_t *right =
                i + 1u < count ? level[i + 1u] : level[i];
            memcpy(pair, level[i], CRAFT_PUB_SIZE);
            memcpy(&pair[CRAFT_PUB_SIZE], right,
                   CRAFT_PUB_SIZE);
            sha256_bytes(pair, sizeof(pair), next[next_count]);
            next_count++;
        }
        memcpy(level, next, next_count * CRAFT_PUB_SIZE);
        count = next_count;
    }
    memcpy(out, level[0], CRAFT_PUB_SIZE);
    return true;
}

static void derive_output_pub(
    uint16_t recipe_id,
    uint8_t output_grade,
    const uint8_t parent[CRAFT_PUB_SIZE],
    uint16_t output_index,
    uint8_t out[CRAFT_PUB_SIZE]) {
    uint8_t preimage[
        sizeof(CRAFT_IDENTITY_DOMAIN) + 2u + 1u +
        CRAFT_PUB_SIZE + 2u];
    size_t off = 0;
    memcpy(&preimage[off], CRAFT_IDENTITY_DOMAIN,
           sizeof(CRAFT_IDENTITY_DOMAIN));
    off += sizeof(CRAFT_IDENTITY_DOMAIN);
    preimage[off++] = (uint8_t)recipe_id;
    preimage[off++] = (uint8_t)(recipe_id >> 8);
    preimage[off++] = output_grade;
    memcpy(&preimage[off], parent, CRAFT_PUB_SIZE);
    off += CRAFT_PUB_SIZE;
    preimage[off++] = (uint8_t)output_index;
    preimage[off++] = (uint8_t)(output_index >> 8);
    sha256_bytes(preimage, off, out);
}

static cargo_craft_provenance_status_t set_status(
    cargo_craft_provenance_result_t *out,
    cargo_craft_provenance_status_t status) {
    if (out) out->status = status;
    return status;
}

static cargo_craft_provenance_status_t evaluate_unbound_v0(
    const uint8_t *payload,
    const cargo_craft_v1_recipe_shape_t *shape,
    cargo_craft_provenance_result_t *out) {
    if (shape->recipe_id == (uint16_t)RECIPE_LEGACY_MIGRATE) {
        /*
         * Legacy-migrate identity needs an origin salt absent from CRAFT
         * bytes.  Keep it readable but unbound after enforcing the frozen
         * zero semantic bytes and zero input slots.
         */
        if (payload[CRAFT_INPUT_COUNT_OFFSET] != 0u ||
            !pub_is_zero(&payload[CRAFT_INPUT_PUBS_OFFSET]) ||
            !pub_is_zero(&payload[CRAFT_INPUT_PUBS_OFFSET + 32u]) ||
            !pub_is_zero(&payload[CRAFT_INPUT_PUBS_OFFSET + 64u]) ||
            pub_is_zero(&payload[CRAFT_OUTPUT_PUB_OFFSET])) {
            return set_status(
                out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS);
        }
        return set_status(out, CARGO_CRAFT_PROVENANCE_UNBOUND_V0);
    }

    cargo_legacy_recipe_shape_t legacy_shape = {
        .recipe_id = shape->recipe_id,
        .input_count = shape->input_count,
        .output_count = shape->output_count,
    };
    cargo_legacy_craft_v0_result_t legacy = {0};
    cargo_legacy_status_t legacy_status =
        cargo_legacy_classify_craft_v0(
            payload, CRAFT_V1_PAYLOAD_SIZE,
            &legacy_shape, &legacy);
    if (legacy_status != CARGO_LEGACY_STATUS_UNBOUND_V0) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_IDENTITY);
    }
    out->output_index = legacy.output_index;
    out->output_index_known = true;
    memcpy(out->parent_merkle, legacy.parent_merkle,
           sizeof(out->parent_merkle));
    return set_status(out, CARGO_CRAFT_PROVENANCE_UNBOUND_V0);
}

cargo_craft_provenance_status_t cargo_craft_provenance_evaluate(
    const uint8_t *payload,
    size_t payload_len,
    bool station_event_verified,
    cargo_craft_provenance_result_t *out) {
    cargo_craft_v1_recipe_shape_t shape;
    uint8_t input_pubs[RECIPE_INPUT_MAX][CRAFT_PUB_SIZE] = {{0}};
    if (!out) return CARGO_CRAFT_PROVENANCE_REJECT_BAD_ARGUMENTS;
    memset(out, 0, sizeof(*out));
    out->status = CARGO_CRAFT_PROVENANCE_REJECT_BAD_ARGUMENTS;
    if (!payload)
        return out->status;
    if (payload_len != CRAFT_V1_PAYLOAD_SIZE) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_PAYLOAD_LENGTH);
    }

    out->recipe_id = read_u16_le(&payload[CRAFT_RECIPE_OFFSET]);
    out->input_count = payload[CRAFT_INPUT_COUNT_OFFSET];
    out->semantics_version = payload[CRAFT_SEMANTICS_OFFSET];
    if (!cargo_craft_v1_recipe_shape(out->recipe_id, &shape)) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_RECIPE);
    }

    if (out->semantics_version == 0u) {
        for (size_t i = CRAFT_SEMANTICS_OFFSET;
             i <= CRAFT_OUTPUT_QUANTITY_OFFSET; i++) {
            if (payload[i] != 0u) {
                return set_status(
                    out,
                    CARGO_CRAFT_PROVENANCE_REJECT_SEMANTICS_VERSION);
            }
        }
        return evaluate_unbound_v0(payload, &shape, out);
    }
    if (out->semantics_version != 1u) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_SEMANTICS_VERSION);
    }
    if (out->input_count != shape.input_count ||
        out->input_count > RECIPE_INPUT_MAX) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_INPUT_COUNT);
    }
    if (pub_is_zero(&payload[CRAFT_OUTPUT_PUB_OFFSET])) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS);
    }
    if (payload[CRAFT_OUTPUT_GRADE_OFFSET] >=
            (uint8_t)MINING_GRADE_COUNT ||
        payload[CRAFT_OUTPUT_QUANTITY_OFFSET] != 1u) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS);
    }
    if (shape.output_commodity_is_dynamic) {
        if (!kind_matches_commodity(
                payload[CRAFT_OUTPUT_KIND_OFFSET],
                payload[CRAFT_OUTPUT_COMMODITY_OFFSET])) {
            return set_status(
                out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS);
        }
    } else if (payload[CRAFT_OUTPUT_KIND_OFFSET] !=
                   shape.output_kind ||
               payload[CRAFT_OUTPUT_COMMODITY_OFFSET] !=
                   shape.output_commodity) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS);
    }

    for (size_t i = 0; i < RECIPE_INPUT_MAX; i++) {
        const uint8_t *input =
            &payload[CRAFT_INPUT_PUBS_OFFSET +
                     i * CRAFT_PUB_SIZE];
        if (i < out->input_count) {
            if (pub_is_zero(input)) {
                return set_status(
                    out, CARGO_CRAFT_PROVENANCE_REJECT_ZERO_INPUT);
            }
            for (size_t j = 0; j < i; j++) {
                if (memcmp(input, input_pubs[j],
                           CRAFT_PUB_SIZE) == 0) {
                    return set_status(
                        out,
                        CARGO_CRAFT_PROVENANCE_REJECT_DUPLICATE_INPUT);
                }
            }
            memcpy(input_pubs[i], input, CRAFT_PUB_SIZE);
        } else if (!pub_is_zero(input)) {
            return set_status(
                out, CARGO_CRAFT_PROVENANCE_REJECT_UNUSED_INPUT);
        }
    }

    if (shape.output_commodity_is_dynamic) {
        /*
         * V1 legacy-migrate bytes do not carry the origin salt needed to
         * recompute their pub.  The station attests the visible labels, but
         * no output index or input lineage can be proven from this payload.
         */
        out->station_attested = station_event_verified;
        return set_status(
            out,
            station_event_verified
                ? CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1
                : CARGO_CRAFT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED);
    }

    if (!parent_merkle(
            (const uint8_t (*)[CRAFT_PUB_SIZE])input_pubs,
            out->input_count, out->parent_merkle)) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_INPUT_COUNT);
    }

    unsigned matches = 0;
    uint16_t matched_index = 0;
    for (uint32_t index = 0; index < shape.output_count; index++) {
        uint8_t candidate[CRAFT_PUB_SIZE];
        derive_output_pub(
            out->recipe_id,
            payload[CRAFT_OUTPUT_GRADE_OFFSET],
            out->parent_merkle, (uint16_t)index, candidate);
        if (memcmp(candidate, &payload[CRAFT_OUTPUT_PUB_OFFSET],
                   sizeof(candidate)) == 0) {
            matches++;
            matched_index = (uint16_t)index;
        }
    }
    if (matches == 0u) {
        return set_status(
            out, CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_IDENTITY);
    }
    if (matches != 1u) {
        return set_status(
            out,
            CARGO_CRAFT_PROVENANCE_REJECT_AMBIGUOUS_OUTPUT_IDENTITY);
    }

    out->output_index = matched_index;
    out->output_index_known = true;
    out->station_attested = station_event_verified;
    return set_status(
        out,
        station_event_verified
            ? CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1
            : CARGO_CRAFT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED);
}

bool cargo_craft_provenance_is_structurally_valid(
    cargo_craft_provenance_status_t status) {
    return status ==
               CARGO_CRAFT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED ||
           status ==
               CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1;
}

bool cargo_craft_provenance_is_rejection(
    cargo_craft_provenance_status_t status) {
    return status >=
               CARGO_CRAFT_PROVENANCE_REJECT_BAD_ARGUMENTS &&
           status <
               CARGO_CRAFT_PROVENANCE_STATUS_COUNT;
}

const char *cargo_craft_provenance_status_name(
    cargo_craft_provenance_status_t status) {
    switch (status) {
    case CARGO_CRAFT_PROVENANCE_NOT_CRAFT:
        return "not_craft";
    case CARGO_CRAFT_PROVENANCE_UNBOUND_V0:
        return "unbound_v0";
    case CARGO_CRAFT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED:
        return "structural_v1_unverified";
    case CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1:
        return "station_attested_v1";
    case CARGO_CRAFT_PROVENANCE_REJECT_BAD_ARGUMENTS:
        return "reject_bad_arguments";
    case CARGO_CRAFT_PROVENANCE_REJECT_PAYLOAD_LENGTH:
        return "reject_payload_length";
    case CARGO_CRAFT_PROVENANCE_REJECT_SEMANTICS_VERSION:
        return "reject_semantics_version";
    case CARGO_CRAFT_PROVENANCE_REJECT_RECIPE:
        return "reject_recipe";
    case CARGO_CRAFT_PROVENANCE_REJECT_INPUT_COUNT:
        return "reject_input_count";
    case CARGO_CRAFT_PROVENANCE_REJECT_ZERO_INPUT:
        return "reject_zero_input";
    case CARGO_CRAFT_PROVENANCE_REJECT_DUPLICATE_INPUT:
        return "reject_duplicate_input";
    case CARGO_CRAFT_PROVENANCE_REJECT_UNUSED_INPUT:
        return "reject_unused_input";
    case CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_SEMANTICS:
        return "reject_output_semantics";
    case CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_IDENTITY:
        return "reject_output_identity";
    case CARGO_CRAFT_PROVENANCE_REJECT_AMBIGUOUS_OUTPUT_IDENTITY:
        return "reject_ambiguous_output_identity";
    case CARGO_CRAFT_PROVENANCE_STATUS_COUNT:
        break;
    }
    return "unknown";
}
