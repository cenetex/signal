#include "test_harness.h"

#include "cell_stress.h"
#include "sim_ship.h"

TEST(test_thrown_rock_impulse_shears_expected_non_core_cell) {
    cell_graph_t graph;
    cell_stress_state_t stress = {0};
    cell_shear_result_t result;
    ASSERT(cell_graph_authored(CELL_LAYOUT_LIGHT_FREIGHTER, &graph));
    uint64_t control = graph.nodes[0].identity;
    uint64_t carrier = graph.nodes[2].identity;
    graph.nodes[2].payload_units = 9;

    float rock_mass = 3.0f;
    float closing_velocity = 40.0f;
    cell_impact_t impact = {
        .impacted_identity = carrier,
        .impulse = rock_mass * closing_velocity,
        .normal = {1.0f, 0.0f},
        .point = {80.0f, 20.0f},
        .assembly_velocity = {5.0f, -2.0f},
        .assembly_rotation = 0.4f,
        .assembly_spin = 0.2f,
        .provenance = CELL_PROVENANCE_KNOWN,
    };
    memset(impact.shell_manifest_root, 0xA1, 32);
    memset(impact.payload_manifest_root, 0xB2, 32);
    ASSERT(cell_stress_apply_impact(&graph, &stress, &impact, &result));
    ASSERT(result.sheared);
    ASSERT_EQ_INT(result.remaining.count, 4);
    ASSERT(result.remaining.nodes[0].identity == control);
    ASSERT_EQ_INT(result.salvage.graph.count, 1);
    ASSERT(result.salvage.graph.nodes[0].identity == carrier);
    ASSERT_EQ_INT(result.salvage.graph.nodes[0].payload_units, 9);
    ASSERT_EQ_FLOAT(result.salvage.rotation, 0.4f, 0.001f);
    ASSERT_EQ_FLOAT(result.salvage.spin, 0.2f, 0.001f);
    ASSERT(result.salvage.vel.x > impact.assembly_velocity.x);
    ASSERT(memcmp(result.salvage.shell_manifest_root,
                  impact.shell_manifest_root, 32) == 0);
    ASSERT(memcmp(result.salvage.payload_manifest_root,
                  impact.payload_manifest_root, 32) == 0);
    ASSERT_EQ_INT(cell_graph_role_count(&result.remaining, CELL_ROLE_CARGO), 1);
}

TEST(test_reinforced_hub_join_fails_in_visible_stages) {
    cell_graph_t graph;
    cell_stress_state_t stress = {0};
    cell_shear_result_t result;
    ASSERT(cell_graph_authored(CELL_LAYOUT_STATION_HUB_7, &graph));
    cell_impact_t impact = {
        .impacted_identity = graph.nodes[1].identity,
        .impulse = 120.0f,
        .normal = {1.0f, 0.0f},
    };
    ASSERT(cell_stress_apply_impact(&graph, &stress, &impact, &result));
    ASSERT(!result.sheared);
    bool saw_stage_one = false;
    for (uint8_t i = 0; i < stress.join_count; i++)
        if (stress.joins[i].stage == 1) saw_stage_one = true;
    ASSERT(saw_stage_one);

    ASSERT(cell_stress_apply_impact(&graph, &stress, &impact, &result));
    bool saw_stage_two = false;
    for (uint8_t i = 0; i < stress.join_count; i++)
        if (stress.joins[i].stage == 2) saw_stage_two = true;
    ASSERT(saw_stage_two);

    bool saw_failed_spoke = false;
    for (int strike = 0; strike < 12 && !saw_failed_spoke; strike++) {
        ASSERT(cell_stress_apply_impact(&graph, &stress, &impact, &result));
        for (uint8_t i = 0; i < stress.join_count; i++) {
            bool touches_hub = stress.joins[i].a == graph.nodes[0].identity ||
                               stress.joins[i].b == graph.nodes[0].identity;
            if (touches_hub && stress.joins[i].failed &&
                stress.joins[i].stage == 2) saw_failed_spoke = true;
        }
    }
    ASSERT(saw_failed_spoke);
    /* The hub spoke cannot fail before its two visible damage stages. Any
     * remaining alternate side joins continue to keep the cell attached. */
}

TEST(test_cell_salvage_and_stress_round_trip_then_reattach_without_matter_loss) {
    cell_graph_t graph;
    cell_stress_state_t stress = {0}, decoded_stress;
    cell_shear_result_t result;
    ASSERT(cell_graph_authored(CELL_LAYOUT_LIGHT_FREIGHTER, &graph));
    cell_impact_t impact = {
        .impacted_identity = graph.nodes[2].identity,
        .impulse = 120.0f,
        .normal = {1.0f, 0.0f},
        .point = {2.0f, 3.0f},
    };
    ASSERT(cell_stress_apply_impact(&graph, &stress, &impact, &result));
    ASSERT(result.sheared);

    uint8_t stress_bytes[2048], salvage_bytes[1024];
    size_t stress_len = 0, salvage_len = 0, consumed = 0;
    ASSERT(cell_stress_encode(&stress, stress_bytes, sizeof(stress_bytes),
                              &stress_len));
    ASSERT(cell_stress_decode(stress_bytes, stress_len, &decoded_stress,
                              &consumed));
    ASSERT_EQ_INT(consumed, stress_len);
    ASSERT_EQ_INT(decoded_stress.join_count, stress.join_count);
    ASSERT(cell_salvage_encode(&result.salvage, salvage_bytes,
                               sizeof(salvage_bytes), &salvage_len));
    cell_salvage_t decoded_salvage;
    ASSERT(cell_salvage_decode(salvage_bytes, salvage_len, &decoded_salvage,
                               &consumed));
    ASSERT_EQ_INT(consumed, salvage_len);
    ASSERT(decoded_salvage.graph.nodes[0].identity ==
           result.salvage.graph.nodes[0].identity);

    cell_graph_t repaired = result.remaining;
    ASSERT(cell_stress_reattach(&repaired, &decoded_salvage, &decoded_stress));
    ASSERT_EQ_INT(repaired.count, graph.count);
    cell_graph_totals_t before, after;
    cell_graph_totals(&graph, &before);
    cell_graph_totals(&repaired, &after);
    ASSERT_EQ_INT(after.struts, before.struts);
    ASSERT_EQ_INT(after.cargo_capacity, before.cargo_capacity);
}

TEST(test_detached_cell_uses_ordinary_shape_aware_tow_path) {
    cell_salvage_t salvage = {.active = true};
    ASSERT(cell_graph_authored(CELL_LAYOUT_TUG, &salvage.graph));
    ship_t ship = {.hull_class = HULL_CLASS_MINER};
    ship.pos = v2(0.0f, 0.0f);
    salvage.pos = v2(220.0f, 0.0f);
    ship_apply_cell_salvage_tow(&ship, &salvage, 1, 1.0f / 60.0f);
    ASSERT(salvage.vel.x < 0.0f);
    ASSERT(ship.vel.x > 0.0f);
    ASSERT(fabsf(salvage.spin) > 0.0001f);
}

TEST(test_multi_cell_repair_is_independent_of_salvage_node_order) {
    cell_graph_t original;
    ASSERT(cell_graph_authored(CELL_LAYOUT_LIGHT_FREIGHTER, &original));
    cell_graph_t remaining = {
        .version = 1,
        .kind = CELL_LAYOUT_NONE,
        .count = 3,
        .nodes = {original.nodes[0], original.nodes[3], original.nodes[4]},
    };
    cell_salvage_t salvage = {
        .active = true,
        .graph = {
            .version = 1,
            .kind = CELL_LAYOUT_NONE,
            .count = 2,
            /* Free end first: node 2 cannot attach until node 1 is also in
             * the atomic weld transaction. */
            .nodes = {original.nodes[2], original.nodes[1]},
        },
    };
    cell_stress_state_t repaired_stress;
    ASSERT(cell_graph_validate(&remaining));
    ASSERT(cell_graph_validate(&salvage.graph));
    ASSERT(cell_stress_reattach(&remaining, &salvage, &repaired_stress));
    ASSERT_EQ_INT(remaining.count, original.count);
    ASSERT_EQ_INT(repaired_stress.version, CELL_STRESS_VERSION);
}

void register_cell_stress_tests(void) {
    TEST_SECTION("\nCell stress and salvage tests:\n");
    RUN(test_thrown_rock_impulse_shears_expected_non_core_cell);
    RUN(test_reinforced_hub_join_fails_in_visible_stages);
    RUN(test_cell_salvage_and_stress_round_trip_then_reattach_without_matter_loss);
    RUN(test_detached_cell_uses_ordinary_shape_aware_tow_path);
    RUN(test_multi_cell_repair_is_independent_of_salvage_node_order);
}
