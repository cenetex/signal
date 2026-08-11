#include "test_harness.h"

#include "cargo_craft_provenance.h"
#include "cargo_legacy_classify.h"
#include "chain_log.h"
#include "manifest.h"

#include <string.h>

static void make_fragment(
    uint8_t out[32],
    uint8_t seed) {
    for (size_t i = 0; i < 32; i++)
        out[i] = (uint8_t)(seed + i);
}

TEST(test_craft_v1_verified_is_station_attested_only) {
    uint8_t fragment[32];
    make_fragment(fragment, 0x20u);
    cargo_unit_t input = {0};
    cargo_unit_t output = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT,
        MINING_GRADE_RARE,
        fragment, 7u, &input));
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &input, 1u, 3u, &output));

    chain_payload_craft_t payload = {0};
    ASSERT(chain_payload_craft_bind_output(
        &payload, &input, 1u, &output));
    cargo_craft_provenance_result_t result;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1);
    ASSERT(result.station_attested);
    ASSERT(!result.input_lineage_proven);
    ASSERT(!result.conservation_proven);
    ASSERT(result.output_index_known);
    ASSERT_EQ_INT(result.output_index, 3);
    ASSERT(memcmp(
        result.parent_merkle,
        output.parent_merkle, 32) == 0);

    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            false, &result),
        CARGO_CRAFT_PROVENANCE_STRUCTURAL_V1_UNVERIFIED);
    ASSERT(!result.station_attested);
    ASSERT(!result.input_lineage_proven);
    ASSERT(!result.conservation_proven);
}

TEST(test_craft_v1_duplicate_zero_and_unused_inputs_fail_closed) {
    uint8_t crystal_fragment[32];
    uint8_t ferrite_fragment[32];
    make_fragment(crystal_fragment, 0x31u);
    make_fragment(ferrite_fragment, 0x71u);

    cargo_unit_t crystal = {0};
    cargo_unit_t ferrite = {0};
    cargo_unit_t frame = {0};
    cargo_unit_t laser_inputs[2] = {{0}};
    cargo_unit_t laser = {0};
    ASSERT(hash_ingot(
        COMMODITY_CRYSTAL_INGOT, MINING_GRADE_FINE,
        crystal_fragment, 0u, &crystal));
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
        ferrite_fragment, 0u, &ferrite));
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &ferrite, 1u, 0u, &frame));
    laser_inputs[0] = crystal;
    laser_inputs[1] = frame;
    ASSERT(hash_product(
        RECIPE_LASER_BASIC, laser_inputs, 2u, 0u, &laser));

    chain_payload_craft_t payload = {0};
    ASSERT(chain_payload_craft_bind_output(
        &payload, laser_inputs, 2u, &laser));
    memcpy(payload.input_pubs[1], payload.input_pubs[0], 32);
    cargo_craft_provenance_result_t result;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_REJECT_DUPLICATE_INPUT);

    ASSERT(chain_payload_craft_bind_output(
        &payload, laser_inputs, 2u, &laser));
    memset(payload.input_pubs[0], 0, 32);
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_REJECT_ZERO_INPUT);

    ASSERT(chain_payload_craft_bind_output(
        &payload, &ferrite, 1u, &frame));
    payload.input_pubs[1][0] = 1u;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_REJECT_UNUSED_INPUT);
}

TEST(test_craft_v1_malformed_shape_and_identity_fail_closed) {
    uint8_t fragment[32];
    make_fragment(fragment, 0x44u);
    cargo_unit_t input = {0};
    cargo_unit_t output = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT,
        MINING_GRADE_COMMON,
        fragment, 0u, &input));
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &input, 1u, 0u, &output));
    chain_payload_craft_t payload = {0};
    ASSERT(chain_payload_craft_bind_output(
        &payload, &input, 1u, &output));

    cargo_craft_provenance_result_t result;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload) - 1u,
            true, &result),
        CARGO_CRAFT_PROVENANCE_REJECT_PAYLOAD_LENGTH);

    ASSERT(chain_payload_craft_bind_output(
        &payload, &input, 1u, &output));
    payload.semantics_version = 2u;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_REJECT_SEMANTICS_VERSION);

    ASSERT(chain_payload_craft_bind_output(
        &payload, &input, 1u, &output));
    payload.output_pub[0] ^= 0x80u;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_REJECT_OUTPUT_IDENTITY);
}

TEST(test_craft_v0_remains_audit_only) {
    chain_payload_craft_t payload = {0};
    payload.recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    payload.input_count = 1u;
    make_fragment(payload.input_pubs[0], 0x55u);
    ASSERT(cargo_legacy_identity_v0_derive(
        payload.recipe_id, payload.input_pubs[0],
        2u, payload.output_pub));

    cargo_craft_provenance_result_t result;
    ASSERT_EQ_INT(
        cargo_craft_provenance_evaluate(
            (const uint8_t *)&payload, sizeof(payload),
            true, &result),
        CARGO_CRAFT_PROVENANCE_UNBOUND_V0);
    ASSERT(!result.station_attested);
    ASSERT(!result.input_lineage_proven);
    ASSERT(!result.conservation_proven);
    ASSERT(result.output_index_known);
    ASSERT_EQ_INT(result.output_index, 2);
}

TEST(test_craft_v1_frozen_recipe_shapes_match_live_catalog) {
    for (int recipe_id = RECIPE_FRAME_BASIC;
         recipe_id <= RECIPE_REPAIR_KIT_FAB;
         recipe_id++) {
        cargo_craft_v1_recipe_shape_t frozen;
        const recipe_def_t *live =
            recipe_get((recipe_id_t)recipe_id);
        ASSERT(live != NULL);
        ASSERT(cargo_craft_v1_recipe_shape(
            (uint16_t)recipe_id, &frozen));
        ASSERT_EQ_INT(frozen.recipe_id, live->id);
        ASSERT_EQ_INT(frozen.output_count, live->output_count);
        ASSERT_EQ_INT(frozen.input_count, live->input_count);
        ASSERT_EQ_INT(frozen.output_kind, live->output_kind);
        ASSERT_EQ_INT(
            frozen.output_commodity,
            live->output_commodity);
        ASSERT(!frozen.output_commodity_is_dynamic);
    }
}

void register_cargo_craft_provenance_tests(void);
void register_cargo_craft_provenance_tests(void) {
    TEST_SECTION("\n--- CRAFT provenance V1 (#679 P0) ---\n");
    RUN(test_craft_v1_verified_is_station_attested_only);
    RUN(test_craft_v1_duplicate_zero_and_unused_inputs_fail_closed);
    RUN(test_craft_v1_malformed_shape_and_identity_fail_closed);
    RUN(test_craft_v0_remains_audit_only);
    RUN(test_craft_v1_frozen_recipe_shapes_match_live_catalog);
}
