#include "test_harness.h"

#include "cargo_legacy_classify.h"
#include "cargo_smelt_provenance.h"
#include "manifest.h"

#include <string.h>

enum {
    TEST_SMELT_PAYLOAD_SIZE = 80,
    TEST_SMELT_FRAGMENT_OFFSET = 0,
    TEST_SMELT_OUTPUT_OFFSET = 32,
    TEST_SMELT_PREFIX_OFFSET = 64,
    TEST_SMELT_SEMANTICS_OFFSET = 65,
    TEST_SMELT_COMMODITY_OFFSET = 66,
    TEST_SMELT_GRADE_OFFSET = 67,
    TEST_SMELT_OUTPUT_INDEX_OFFSET = 68,
    TEST_SMELT_RESERVED_OFFSET = 70,
    TEST_SMELT_CONTEXT_OFFSET = 72,
};

static void smelt_test_write_u16_le(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void smelt_test_write_u64_le(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8u; i++)
        p[i] = (uint8_t)(value >> (i * 8u));
}

static void smelt_test_fragment(uint8_t out[32], uint8_t seed) {
    for (size_t i = 0; i < 32u; i++)
        out[i] = (uint8_t)(seed + i);
}

static void smelt_test_build_v1(
    uint8_t payload[TEST_SMELT_PAYLOAD_SIZE],
    cargo_unit_t *canonical,
    commodity_t commodity,
    mining_grade_t grade,
    uint16_t output_index,
    uint64_t refinery_context) {
    uint8_t fragment[32];
    smelt_test_fragment(fragment, 0x31u);
    memset(payload, 0, TEST_SMELT_PAYLOAD_SIZE);
    ASSERT(hash_ingot(
        commodity,
        grade,
        fragment, output_index, canonical));
    memcpy(&payload[TEST_SMELT_FRAGMENT_OFFSET],
           fragment, sizeof(fragment));
    memcpy(&payload[TEST_SMELT_OUTPUT_OFFSET],
           canonical->pub, sizeof(canonical->pub));
    payload[TEST_SMELT_PREFIX_OFFSET] = canonical->prefix_class;
    payload[TEST_SMELT_SEMANTICS_OFFSET] = 1u;
    payload[TEST_SMELT_COMMODITY_OFFSET] = canonical->commodity;
    payload[TEST_SMELT_GRADE_OFFSET] = canonical->grade;
    smelt_test_write_u16_le(
        &payload[TEST_SMELT_OUTPUT_INDEX_OFFSET], output_index);
    smelt_test_write_u64_le(
        &payload[TEST_SMELT_CONTEXT_OFFSET], refinery_context);
    canonical->mined_block = refinery_context;
}

static void smelt_test_assert_never_mining_proof(
    const cargo_smelt_provenance_result_t *result) {
    ASSERT(!result->mining_proven);
    ASSERT(!result->grade_verified);
}

TEST(test_smelt_v1_verified_is_station_attested_only) {
    static const uint16_t output_index = 0x1234u;
    static const uint64_t refinery_context =
        UINT64_C(0x0102030405060708);
    uint8_t payload[TEST_SMELT_PAYLOAD_SIZE];
    cargo_unit_t canonical = {0};
    cargo_smelt_provenance_result_t result;
    smelt_test_build_v1(
        payload, &canonical,
        COMMODITY_CUPRITE_INGOT, MINING_GRADE_RARE,
        output_index, refinery_context);
    ASSERT_EQ_INT(payload[TEST_SMELT_OUTPUT_INDEX_OFFSET], 0x34);
    ASSERT_EQ_INT(payload[TEST_SMELT_OUTPUT_INDEX_OFFSET + 1u], 0x12);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET], 0x08);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 1u], 0x07);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 2u], 0x06);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 3u], 0x05);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 4u], 0x04);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 5u], 0x03);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 6u], 0x02);
    ASSERT_EQ_INT(payload[TEST_SMELT_CONTEXT_OFFSET + 7u], 0x01);

    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1);
    ASSERT(result.station_attested);
    smelt_test_assert_never_mining_proof(&result);
    ASSERT(result.output_index_known);
    ASSERT_EQ_INT(result.output_index, output_index);
    ASSERT_EQ_INT(result.semantics_version, 1);
    ASSERT_EQ_INT(result.commodity, canonical.commodity);
    ASSERT_EQ_INT(result.grade, canonical.grade);
    ASSERT_EQ_INT(result.prefix_class, canonical.prefix_class);
    ASSERT(result.refinery_context_tick == refinery_context);
    ASSERT(memcmp(result.fragment_pub,
                  &payload[TEST_SMELT_FRAGMENT_OFFSET], 32) == 0);
    ASSERT(memcmp(result.output_pub, canonical.pub, 32) == 0);
    ASSERT(memcmp(&result.output_cargo, &canonical,
                  sizeof(canonical)) == 0);

    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), false, &result),
        CARGO_SMELT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED);
    ASSERT(!result.station_attested);
    smelt_test_assert_never_mining_proof(&result);
    ASSERT(result.output_index_known);
}

TEST(test_smelt_v1_tampering_fails_closed) {
    uint8_t payload[TEST_SMELT_PAYLOAD_SIZE];
    uint8_t valid[TEST_SMELT_PAYLOAD_SIZE];
    cargo_unit_t canonical = {0};
    cargo_smelt_provenance_result_t result;
    smelt_test_build_v1(
        valid, &canonical,
        COMMODITY_CUPRITE_INGOT, MINING_GRADE_RARE,
        7u, UINT64_C(55));

    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            valid, sizeof(valid) - 1u, true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_PAYLOAD_LENGTH);
    smelt_test_assert_never_mining_proof(&result);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_SEMANTICS_OFFSET] = 2u;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_SEMANTICS_VERSION);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_RESERVED_OFFSET] = 1u;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_RESERVED_OFFSET + 1u] = 1u;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);

    memcpy(payload, valid, sizeof(payload));
    memset(&payload[TEST_SMELT_FRAGMENT_OFFSET], 0, 32);
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);

    memcpy(payload, valid, sizeof(payload));
    memset(&payload[TEST_SMELT_OUTPUT_OFFSET], 0, 32);
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_COMMODITY_OFFSET] =
        (uint8_t)COMMODITY_FERRITE_ORE;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_GRADE_OFFSET] = (uint8_t)MINING_GRADE_COUNT;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_OUTPUT_OFFSET] ^= 0x80u;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V1);
    ASSERT(!result.station_attested);
    ASSERT(!result.output_index_known);
    smelt_test_assert_never_mining_proof(&result);

    memcpy(payload, valid, sizeof(payload));
    payload[TEST_SMELT_PREFIX_OFFSET] =
        (uint8_t)((payload[TEST_SMELT_PREFIX_OFFSET] + 1u) %
                  (uint8_t)INGOT_PREFIX_COUNT);
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_REJECT_PREFIX_V1);
}

TEST(test_smelt_v1_all_ingot_commodities_and_grade_bounds) {
    static const commodity_t commodities[] = {
        COMMODITY_FERRITE_INGOT,
        COMMODITY_CUPRITE_INGOT,
        COMMODITY_CRYSTAL_INGOT,
    };
    static const mining_grade_t boundary_grades[] = {
        MINING_GRADE_COMMON,
        MINING_GRADE_COMMISSIONED,
    };
    for (size_t commodity_index = 0;
         commodity_index <
             sizeof(commodities) / sizeof(commodities[0]);
         commodity_index++) {
        for (size_t grade_index = 0;
             grade_index <
                 sizeof(boundary_grades) /
                     sizeof(boundary_grades[0]);
             grade_index++) {
            uint8_t payload[TEST_SMELT_PAYLOAD_SIZE];
            cargo_unit_t canonical = {0};
            cargo_smelt_provenance_result_t result;
            smelt_test_build_v1(
                payload, &canonical,
                commodities[commodity_index],
                boundary_grades[grade_index],
                (uint16_t)(0x200u + commodity_index * 2u +
                           grade_index),
                UINT64_C(0x8877665544332211));
            ASSERT_EQ_INT(
                cargo_smelt_provenance_evaluate(
                    payload, sizeof(payload), true, &result),
                CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1);
            ASSERT_EQ_INT(
                result.commodity, commodities[commodity_index]);
            ASSERT_EQ_INT(
                result.grade, boundary_grades[grade_index]);
            ASSERT(memcmp(
                &result.output_cargo, &canonical,
                sizeof(canonical)) == 0);
            smelt_test_assert_never_mining_proof(&result);
        }
    }
}

TEST(test_smelt_v0_remains_audit_only) {
    uint8_t payload[TEST_SMELT_PAYLOAD_SIZE] = {0};
    uint8_t fragment[32];
    uint8_t prefix = 0;
    smelt_test_fragment(fragment, 0x71u);
    memcpy(&payload[TEST_SMELT_FRAGMENT_OFFSET],
           fragment, sizeof(fragment));
    ASSERT(cargo_legacy_identity_v0_derive(
        CARGO_LEGACY_RECIPE_SMELT_ID, fragment, 19u,
        &payload[TEST_SMELT_OUTPUT_OFFSET]));
    ASSERT(cargo_legacy_prefix_class_v0_derive(
        &payload[TEST_SMELT_OUTPUT_OFFSET], &prefix));
    payload[TEST_SMELT_PREFIX_OFFSET] = prefix;
    smelt_test_write_u64_le(
        &payload[TEST_SMELT_CONTEXT_OFFSET], UINT64_C(987654321));

    cargo_smelt_provenance_result_t result;
    ASSERT_EQ_INT(
        cargo_smelt_provenance_evaluate(
            payload, sizeof(payload), true, &result),
        CARGO_SMELT_PROVENANCE_UNBOUND_V0);
    ASSERT(!result.station_attested);
    smelt_test_assert_never_mining_proof(&result);
    ASSERT(result.output_index_known);
    ASSERT_EQ_INT(result.output_index, 19);
    ASSERT_EQ_INT(result.semantics_version, 0);
    ASSERT(result.refinery_context_tick == UINT64_C(987654321));
    ASSERT_EQ_INT(result.output_cargo.quantity, 0);
}

TEST(test_smelt_provenance_status_names_are_stable) {
    static const struct {
        cargo_smelt_provenance_status_t status;
        const char *name;
    } cases[] = {
        {CARGO_SMELT_PROVENANCE_NOT_SMELT, "not_smelt"},
        {CARGO_SMELT_PROVENANCE_UNBOUND_V0, "unbound_v0"},
        {CARGO_SMELT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED,
         "structural_v1_unverified"},
        {CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1,
         "station_attested_v1"},
        {CARGO_SMELT_PROVENANCE_REJECT_BAD_ARGUMENTS,
         "internal_error"},
        {CARGO_SMELT_PROVENANCE_REJECT_PAYLOAD_LENGTH,
         "reject_smelt_payload_length"},
        {CARGO_SMELT_PROVENANCE_REJECT_SEMANTICS_VERSION,
         "reject_smelt_semantics_version"},
        {CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V0,
         "reject_malformed_v0"},
        {CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V0,
         "reject_identity_v0"},
        {CARGO_SMELT_PROVENANCE_REJECT_AMBIGUOUS_V0,
         "reject_ambiguous_v0"},
        {CARGO_SMELT_PROVENANCE_REJECT_MALFORMED_V1,
         "reject_malformed_v1"},
        {CARGO_SMELT_PROVENANCE_REJECT_IDENTITY_V1,
         "reject_identity_v1"},
        {CARGO_SMELT_PROVENANCE_REJECT_PREFIX_V1,
         "reject_prefix_v1"},
        {CARGO_SMELT_PROVENANCE_REJECT_RESOURCE,
         "resource_exhausted"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_STR_EQ(
            cargo_smelt_provenance_status_name(cases[i].status),
            cases[i].name);
    }
    ASSERT(cargo_smelt_provenance_is_structurally_valid(
        CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1));
    ASSERT(!cargo_smelt_provenance_is_structurally_valid(
        CARGO_SMELT_PROVENANCE_UNBOUND_V0));
    ASSERT(cargo_smelt_provenance_is_rejection(
        CARGO_SMELT_PROVENANCE_REJECT_PREFIX_V1));
    ASSERT(!cargo_smelt_provenance_is_rejection(
        CARGO_SMELT_PROVENANCE_STATION_ATTESTED_V1));
}

void register_cargo_smelt_provenance_tests(void);
void register_cargo_smelt_provenance_tests(void) {
    TEST_SECTION("\n--- SMELT provenance V1 (#677 P0) ---\n");
    RUN(test_smelt_v1_verified_is_station_attested_only);
    RUN(test_smelt_v1_tampering_fails_closed);
    RUN(test_smelt_v1_all_ingot_commodities_and_grade_bounds);
    RUN(test_smelt_v0_remains_audit_only);
    RUN(test_smelt_provenance_status_names_are_stable);
}
