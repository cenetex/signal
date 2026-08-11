#include "test_harness.h"

#include "cell_geometry.h"

#include <string.h>

TEST(test_cell_lattice_neighbors_and_world_positions_are_deterministic) {
    cell_coord_t origin = {0, 0};
    const cell_coord_t expected[CELL_ORIENTATION_COUNT] = {
        {1, 0}, {0, 1}, {-1, 1}, {-1, 0}, {0, -1}, {1, -1},
    };
    for (int o = 0; o < CELL_ORIENTATION_COUNT; o++) {
        cell_coord_t neighbor = cell_coord_neighbor(origin, o);
        ASSERT_EQ_INT(neighbor.q, expected[o].q);
        ASSERT_EQ_INT(neighbor.r, expected[o].r);
        ASSERT_EQ_INT(cell_coord_distance(origin, neighbor), 1);
    }
    ASSERT_EQ_INT(cell_orientation_normalize(-1), 5);
    ASSERT_EQ_INT(cell_orientation_normalize(7), 1);

    cell_point_t east = cell_coord_world(expected[0], CELL_EDGE_LENGTH);
    ASSERT_EQ_FLOAT(east.x, CELL_EDGE_LENGTH * 1.7320508f, 0.0001f);
    ASSERT_EQ_FLOAT(east.y, 0.0f, 0.0001f);
}

TEST(test_cell_complete_edge_and_triangle_mount_join_rules) {
    cell_node_t center = {
        .identity = 1, .shape = CELL_SHAPE_HEX, .role = CELL_ROLE_CONTROL,
    };
    cell_node_t east = {
        .identity = 2, .coord = {1, 0},
        .shape = CELL_SHAPE_HEX, .role = CELL_ROLE_CARGO,
    };
    cell_node_t far = east;
    far.identity = 3;
    far.coord.q = 2;
    cell_node_t engine = {
        .identity = 4, .shape = CELL_SHAPE_TRIANGLE,
        .role = CELL_ROLE_ENGINE, .orientation = 3,
    };
    ASSERT(cell_nodes_join(&center, &east));
    ASSERT(!cell_nodes_join(&center, &far));
    ASSERT(cell_nodes_join(&center, &engine));
    ASSERT(!cell_nodes_join(&east, &engine));
}

TEST(test_authored_cell_layouts_use_one_connected_grammar) {
    const int expected_counts[CELL_LAYOUT_COUNT] = {
        [CELL_LAYOUT_TUG] = 3,
        [CELL_LAYOUT_LIGHT_FREIGHTER] = 5,
        [CELL_LAYOUT_HEAVY_FREIGHTER] = 10,
        [CELL_LAYOUT_UTILITY] = 4,
        [CELL_LAYOUT_STATION_HUB_7] = 7,
        [CELL_LAYOUT_CARGO_DRONE] = 4,
        [CELL_LAYOUT_LASER_DRONE] = 3,
    };
    for (int kind = CELL_LAYOUT_TUG; kind < CELL_LAYOUT_COUNT; kind++) {
        cell_graph_t graph;
        ASSERT(cell_graph_authored((cell_layout_kind_t)kind, &graph));
        ASSERT(cell_graph_validate(&graph));
        ASSERT_EQ_INT(graph.count, expected_counts[kind]);
    }

    cell_graph_t light;
    cell_graph_totals_t totals;
    ASSERT(cell_graph_authored(CELL_LAYOUT_LIGHT_FREIGHTER, &light));
    cell_graph_totals(&light, &totals);
    ASSERT_EQ_INT(cell_graph_role_count(&light, CELL_ROLE_CARGO), 2);
    ASSERT_EQ_INT(totals.struts, 24); /* 3 hex rims + engine + tow triangles */
    ASSERT_EQ_INT(totals.cargo_capacity, 72);
    ASSERT_EQ_FLOAT(totals.thrust_units, 1.0f, 0.0001f);
    cell_matter_cost_t cost = cell_graph_matter_cost(&light);
    ASSERT_EQ_INT(cost.struts, 24);
    ASSERT_EQ_INT(cost.ingots_to_press, 6);
    ASSERT_EQ_INT(cost.fragments_to_smelt, 2);
    ASSERT_EQ_FLOAT(cost.fragment_equivalent, 1.5f, 0.0001f);

    cell_graph_t hub;
    ASSERT(cell_graph_authored(CELL_LAYOUT_STATION_HUB_7, &hub));
    cell_graph_totals(&hub, &totals);
    /* Complete-edge welds align two cell-owned rim struts.  They do not
     * delete one and make detachment invent matter later. */
    ASSERT_EQ_INT(totals.struts, 48);
    ASSERT_EQ_INT(totals.cargo_capacity, 156);
}

TEST(test_cell_layout_serialization_round_trips_canonically) {
    cell_graph_t source, decoded;
    uint8_t first[512] = {0};
    uint8_t second[512] = {0};
    size_t first_len = 0, second_len = 0, consumed = 0;
    ASSERT(cell_graph_authored(CELL_LAYOUT_HEAVY_FREIGHTER, &source));
    source.nodes[1].payload_units = 7;
    ASSERT(cell_graph_encode(&source, first, sizeof(first), &first_len));
    ASSERT(cell_graph_encode(&source, second, sizeof(second), &second_len));
    ASSERT_EQ_INT(first_len, second_len);
    ASSERT(memcmp(first, second, first_len) == 0);
    ASSERT(memcmp(first, "CELL", 4) == 0);

    ASSERT(cell_graph_decode(first, first_len, &decoded, &consumed));
    ASSERT_EQ_INT(consumed, first_len);
    ASSERT_EQ_INT(decoded.kind, CELL_LAYOUT_HEAVY_FREIGHTER);
    ASSERT_EQ_INT(decoded.count, source.count);
    ASSERT_EQ_INT(decoded.nodes[1].payload_units, 7);
    ASSERT_EQ_INT(decoded.nodes[9].orientation, source.nodes[9].orientation);

    first[4] = 99;
    ASSERT(!cell_graph_decode(first, first_len, &decoded, NULL));
}

TEST(test_cell_detach_preserves_unaffected_identity_and_payload) {
    cell_graph_t graph;
    cell_node_t detached;
    ASSERT(cell_graph_authored(CELL_LAYOUT_LIGHT_FREIGHTER, &graph));
    uint64_t control_id = graph.nodes[0].identity;
    uint64_t carrier_id = graph.nodes[2].identity;
    graph.nodes[2].payload_units = 11;
    ASSERT(cell_graph_remove_node(&graph, carrier_id, &detached));
    ASSERT_EQ_INT(graph.count, 4);
    ASSERT(graph.nodes[0].identity == control_id);
    ASSERT(detached.identity == carrier_id);
    ASSERT_EQ_INT(detached.payload_units, 11);
    ASSERT(cell_graph_validate(&graph));

    /* Removing the reinforced center would disconnect a seven-cell hub. */
    ASSERT(cell_graph_authored(CELL_LAYOUT_STATION_HUB_7, &graph));
    ASSERT(!cell_graph_remove_node(&graph, graph.nodes[0].identity, NULL));
    ASSERT_EQ_INT(graph.count, 7);
}

TEST(test_cell_matter_algebra_and_shape_balance) {
    ASSERT_EQ_INT(CELL_INGOTS_PER_FRAGMENT, 4);
    ASSERT_EQ_INT(CELL_STRUTS_PER_INGOT, 4);
    ASSERT_EQ_INT(CELL_STRUTS_PER_FRAGMENT, 16);
    ASSERT_EQ_INT(cell_shape_strut_cost(CELL_SHAPE_TRIANGLE), 3);
    ASSERT_EQ_INT(cell_shape_strut_cost(CELL_SHAPE_HEX), 6);
    ASSERT_EQ_INT(cell_shape_strut_cost(CELL_SHAPE_REINFORCED_HEX), 12);
    ASSERT_EQ_INT(cell_shape_payload_capacity(CELL_SHAPE_HEX), 24);
    ASSERT_EQ_INT(cell_shape_payload_capacity(CELL_SHAPE_REINFORCED_HEX), 12);
}

TEST(test_triangle_rotation_changes_functional_vector_in_sixty_degree_steps) {
    cell_node_t engine = {
        .identity = 1,
        .shape = CELL_SHAPE_TRIANGLE,
        .role = CELL_ROLE_ENGINE,
        .orientation = 3,
    };
    cell_point_t thrust = cell_triangle_active_vector(&engine);
    ASSERT_EQ_FLOAT(thrust.x, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(thrust.y, 0.0f, 0.0001f);
    engine.orientation = 4;
    thrust = cell_triangle_active_vector(&engine);
    ASSERT_EQ_FLOAT(thrust.x, 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(thrust.y, 0.8660254f, 0.0001f);

    engine.role = CELL_ROLE_TOW;
    engine.orientation = 0;
    cell_point_t tow = cell_triangle_active_vector(&engine);
    ASSERT_EQ_FLOAT(tow.x, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(tow.y, 0.0f, 0.0001f);
}

void register_cell_geometry_tests(void) {
    RUN(test_cell_lattice_neighbors_and_world_positions_are_deterministic);
    RUN(test_cell_complete_edge_and_triangle_mount_join_rules);
    RUN(test_authored_cell_layouts_use_one_connected_grammar);
    RUN(test_cell_layout_serialization_round_trips_canonically);
    RUN(test_cell_detach_preserves_unaffected_identity_and_payload);
    RUN(test_cell_matter_algebra_and_shape_balance);
    RUN(test_triangle_rotation_changes_functional_vector_in_sixty_degree_steps);
}
