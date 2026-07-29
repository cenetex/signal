#include "cargo_smelt_provenance.h"

#include "cargo_legacy_classify.h"
#include "sha256.h"

#include <string.h>

enum {
    SMELT_V1_PAYLOAD_SIZE = 80,
    SMELT_FRAGMENT_OFFSET = 0,
    SMELT_OUTPUT_OFFSET = 32,
    SMELT_PREFIX_OFFSET = 64,
    SMELT_SEMANTICS_OFFSET = 65,
    SMELT_COMMODITY_OFFSET = 66,
    SMELT_GRADE_OFFSET = 67,
    SMELT_OUTPUT_INDEX_OFFSET = 68,
    SMELT_RESERVED_OFFSET = 70,
    SMELT_RESERVED_SIZE = 2,
    SMELT_REFINERY_CONTEXT_OFFSET = 72,
    SMELT_PUB_SIZE = 32,
    SMELT_SEMANTICS_UNBOUND = 0,
    SMELT_SEMANTICS_V1 = 1,
};

static const uint8_t SMELT_IDENTITY_DOMAIN[8] = {
    'S', 'I', 'G', 'N', 'A', 'L', 'v', '1'
};

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] |
           (uint16_t)((uint16_t)p[1] << 8);
}

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8u; i++)
        value |= (uint64_t)p[i] << (i * 8u);
    return value;
}

static bool pub_is_zero(const uint8_t pub[SMELT_PUB_SIZE]) {
    static const uint8_t zero[SMELT_PUB_SIZE] = {0};
    return memcmp(pub, zero, sizeof(zero)) == 0;
}

static bool commodity_is_ingot(uint8_t commodity) {
    return commodity == (uint8_t)COMMODITY_FERRITE_INGOT ||
           commodity == (uint8_t)COMMODITY_CUPRITE_INGOT ||
           commodity == (uint8_t)COMMODITY_CRYSTAL_INGOT;
}

static void derive_ingot_pub_v1(
    uint8_t commodity,
    uint8_t grade,
    const uint8_t fragment_pub[SMELT_PUB_SIZE],
    uint16_t output_index,
    uint8_t out_pub[SMELT_PUB_SIZE]) {
    uint8_t preimage[8 + 2 + 1 + 1 + SMELT_PUB_SIZE + 2];
    memcpy(preimage, SMELT_IDENTITY_DOMAIN,
           sizeof(SMELT_IDENTITY_DOMAIN));
    preimage[8] = (uint8_t)((uint16_t)RECIPE_SMELT);
    preimage[9] = (uint8_t)((uint16_t)RECIPE_SMELT >> 8);
    preimage[10] = commodity;
    preimage[11] = grade;
    memcpy(&preimage[12], fragment_pub, SMELT_PUB_SIZE);
    preimage[44] = (uint8_t)output_index;
    preimage[45] = (uint8_t)(output_index >> 8);
    sha256_bytes(preimage, sizeof(preimage), out_pub);
}

static cargo_smelt_provenance_status_t set_status(
    cargo_smelt_provenance_result_t *out,
    cargo_smelt_provenance_status_t status) {
    out->status = status;
    return status;
}

static cargo_smelt_provenance_status_t evaluate_unbound_v0(
    const uint8_t *payload,
    size_t payload_len,
    cargo_smelt_provenance_result_t *out) {
    cargo_legacy_smelt_v0_result_t legacy;
    cargo_legacy_status_t status =
        cargo_legacy_classify_smelt_v0(payload, payload_len, &legacy);
    switch (status) {
    case CARGO_LEGACY_STATUS_UNBOUND_V0:
        out->semantics_version = SMELT_SEMANTICS_UNBOUND;
        out->output_index = legacy.output_index;
        out->output_index_known = true;
        memcpy(out->fragment_pub, legacy.fragment_pub,
               sizeof(out->fragment_pub));
        memcpy(out->output_pub, legacy.legacy_output_pub,
               sizeof(out->output_pub));
        out->prefix_class = legacy.legacy_prefix_class;
        out->refinery_context_tick = legacy.legacy_mined_block;
        return set_status(
            out, CARGO_SMELT_PROVENANCE_UNBOUND_V0);
    case CARGO_LEGACY_STATUS_MALFORMED:
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V0);
    case CARGO_LEGACY_STATUS_NONE:
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V0);
    case CARGO_LEGACY_STATUS_AMBIGUOUS:
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_AMBIGUOUS_V0);
    case CARGO_LEGACY_STATUS_RESOURCE:
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_RESOURCE);
    case CARGO_LEGACY_STATUS_BAD_ARGUMENTS:
    case CARGO_LEGACY_STATUS_CURRENT_V1:
    case CARGO_LEGACY_STATUS_COUNT:
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS);
    }
    return set_status(
        out, CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS);
}

cargo_smelt_provenance_status_t cargo_smelt_provenance_evaluate(
    const uint8_t *payload,
    size_t payload_len,
    bool station_event_verified,
    cargo_smelt_provenance_result_t *out) {
    uint8_t derived_pub[SMELT_PUB_SIZE];
    cargo_unit_t output = {0};

    if (!out)
        return CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS;
    memset(out, 0, sizeof(*out));
    out->status = CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS;
    if (!payload)
        return out->status;
    if (payload_len != SMELT_V1_PAYLOAD_SIZE) {
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_PAYLOAD_LENGTH);
    }

    out->semantics_version = payload[SMELT_SEMANTICS_OFFSET];
    if (out->semantics_version == SMELT_SEMANTICS_UNBOUND)
        return evaluate_unbound_v0(payload, payload_len, out);
    if (out->semantics_version != SMELT_SEMANTICS_V1) {
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_SEMANTICS_VERSION);
    }

    const uint8_t *fragment_pub = &payload[SMELT_FRAGMENT_OFFSET];
    const uint8_t *output_pub = &payload[SMELT_OUTPUT_OFFSET];
    uint8_t prefix_class = payload[SMELT_PREFIX_OFFSET];
    uint8_t commodity = payload[SMELT_COMMODITY_OFFSET];
    uint8_t grade = payload[SMELT_GRADE_OFFSET];
    uint16_t output_index =
        read_u16_le(&payload[SMELT_OUTPUT_INDEX_OFFSET]);
    uint64_t refinery_context =
        read_u64_le(&payload[SMELT_REFINERY_CONTEXT_OFFSET]);

    if (payload[SMELT_RESERVED_OFFSET] != 0u ||
        payload[SMELT_RESERVED_OFFSET + SMELT_RESERVED_SIZE - 1u] != 0u ||
        pub_is_zero(fragment_pub) ||
        pub_is_zero(output_pub) ||
        !commodity_is_ingot(commodity) ||
        grade >= (uint8_t)MINING_GRADE_COUNT) {
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);
    }

    derive_ingot_pub_v1(
        commodity, grade, fragment_pub, output_index, derived_pub);
    if (memcmp(derived_pub, output_pub, sizeof(derived_pub)) != 0) {
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V1);
    }
    if (prefix_class !=
        (uint8_t)mining_pubkey_class(derived_pub)) {
        return set_status(
            out, CARGO_SMELT_PROVENANCE_REJECT_PREFIX_V1);
    }

    output.kind = (uint8_t)CARGO_KIND_INGOT;
    output.commodity = commodity;
    output.grade = grade;
    output.prefix_class = prefix_class;
    output.recipe_id = (uint16_t)RECIPE_SMELT;
    output.quantity = 1u;
    output.mined_block = refinery_context;
    memcpy(output.pub, output_pub, sizeof(output.pub));
    memcpy(output.parent_merkle, fragment_pub,
           sizeof(output.parent_merkle));

    out->output_index = output_index;
    out->output_index_known = true;
    memcpy(out->fragment_pub, fragment_pub,
           sizeof(out->fragment_pub));
    memcpy(out->output_pub, output_pub, sizeof(out->output_pub));
    out->prefix_class = prefix_class;
    out->commodity = commodity;
    out->grade = grade;
    out->refinery_context_tick = refinery_context;
    out->output_cargo = output;
    out->station_attested = station_event_verified;
    return set_status(
        out,
        station_event_verified
            ? CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1
            : CARGO_SMELT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED);
}

bool cargo_smelt_provenance_is_structurally_valid(
    cargo_smelt_provenance_status_t status) {
    return status ==
               CARGO_SMELT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED ||
           status ==
               CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1;
}

bool cargo_smelt_provenance_is_rejection(
    cargo_smelt_provenance_status_t status) {
    return status >=
               CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS &&
           status <
               CARGO_SMELT_PROVENANCE_STATUS_COUNT;
}

const char *cargo_smelt_provenance_status_name(
    cargo_smelt_provenance_status_t status) {
    switch (status) {
    case CARGO_SMELT_PROVENANCE_NOT_SMELT:
        return "not_smelt";
    case CARGO_SMELT_PROVENANCE_UNBOUND_V0:
        return "unbound_v0";
    case CARGO_SMELT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED:
        return "structural_v1_unverified";
    case CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1:
        return "station_attested_v1";
    case CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS:
        return "internal_error";
    case CARGO_SMELT_PROVENANCE_REJECT_PAYLOAD_LENGTH:
        return "reject_smelt_payload_length";
    case CARGO_SMELT_PROVENANCE_REJECT_SEMANTICS_VERSION:
        return "reject_smelt_semantics_version";
    case CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V0:
        return "reject_malformed_v0";
    case CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V0:
        return "reject_identity_v0";
    case CARGO_SMELT_PROVENANCE_REJECT_AMBIGUOUS_V0:
        return "reject_ambiguous_v0";
    case CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1:
        return "reject_malformed_v1";
    case CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V1:
        return "reject_identity_v1";
    case CARGO_SMELT_PROVENANCE_REJECT_PREFIX_V1:
        return "reject_prefix_v1";
    case CARGO_SMELT_PROVENANCE_REJECT_RESOURCE:
        return "resource_exhausted";
    case CARGO_SMELT_PROVENANCE_STATUS_COUNT:
        break;
    }
    return "unknown";
}
