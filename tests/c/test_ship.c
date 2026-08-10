#include "test_harness.h"
#include "sim_ship.h"
#include "fixpoint.h"

TEST(test_ship_hull_def_miner) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    const hull_def_t* hull = ship_hull_def(&ship);
    ASSERT_STR_EQ(hull->name, "Mining Cutter");
    ASSERT_EQ_FLOAT(hull->max_hull, 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->cargo_capacity, 24.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->mining_rate, 28.0f, 0.01f);
}

TEST(test_ship_hull_def_hauler) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_HAULER;
    const hull_def_t* hull = ship_hull_def(&ship);
    ASSERT_STR_EQ(hull->name, "Frame-2 Cargo Hauler");
    ASSERT_EQ_FLOAT(hull->ingot_capacity, 72.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->mining_rate, 0.0f, 0.01f);
}

TEST(test_ship_loadout_metadata_tracks_module_sockets) {
    ship_t player = {0};
    player.hull_class = HULL_CLASS_MINER;
    ASSERT_EQ_INT(ship_module_socket_count(&player), 3);
    ASSERT(ship_has_module(&player, SHIP_MODULE_TRACTOR));
    ASSERT(ship_has_module(&player, SHIP_MODULE_LASER));
    ASSERT(ship_has_module(&player, SHIP_MODULE_CARGO));

    ship_t worker = {0};
    worker.hull_class = HULL_CLASS_NPC_MINER;
    ASSERT_EQ_INT(ship_module_socket_count(&worker), 2);
    ASSERT(ship_has_module(&worker, SHIP_MODULE_TRACTOR));
    ASSERT(ship_has_module(&worker, SHIP_MODULE_LASER));
    ASSERT(!ship_has_module(&worker, SHIP_MODULE_CARGO));

    ship_t tug = {0};
    tug.hull_class = HULL_CLASS_DRONE_TRACTOR;
    ASSERT_EQ_INT(ship_module_socket_count(&tug), 1);
    ASSERT(ship_has_module(&tug, SHIP_MODULE_TRACTOR));
    ASSERT(!ship_has_module(&tug, SHIP_MODULE_LASER));
    ASSERT(!ship_has_module(&tug, SHIP_MODULE_CARGO));

    ship_t cutter = {0};
    cutter.hull_class = HULL_CLASS_DRONE_LASER;
    ASSERT_EQ_INT(ship_module_socket_count(&cutter), 1);
    ASSERT(!ship_has_module(&cutter, SHIP_MODULE_TRACTOR));
    ASSERT(ship_has_module(&cutter, SHIP_MODULE_LASER));
    ASSERT(!ship_has_module(&cutter, SHIP_MODULE_CARGO));

    ship_t courier = {0};
    courier.hull_class = HULL_CLASS_DRONE_CARGO;
    ASSERT_EQ_INT(ship_module_socket_count(&courier), 1);
    ASSERT(!ship_has_module(&courier, SHIP_MODULE_TRACTOR));
    ASSERT(!ship_has_module(&courier, SHIP_MODULE_LASER));
    ASSERT(ship_has_module(&courier, SHIP_MODULE_CARGO));
}

TEST(test_ship_max_hull) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ASSERT_EQ_FLOAT(ship_max_hull(&ship), 100.0f, 0.01f);
    ship.hull_class = HULL_CLASS_HAULER;
    ASSERT_EQ_FLOAT(ship_max_hull(&ship), 150.0f, 0.01f);
}

TEST(test_ship_cargo_capacity_with_upgrades) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hold_level = 0;
    ASSERT_EQ_FLOAT(ship_cargo_capacity(&ship), 24.0f, 0.01f);
    ship.hold_level = 2;
    ASSERT_EQ_FLOAT(ship_cargo_capacity(&ship), 24.0f + 2 * 8.0f, 0.01f);
}

TEST(test_ship_cell_graph_derives_capacity_mass_and_thrust) {
    ship_t ship = {0};
    cell_graph_t graph;
    cell_graph_totals_t totals;

    ship.hull_class = HULL_CLASS_HAULER;
    ASSERT_EQ_INT(ship_cell_layout_kind(ship.hull_class),
                  CELL_LAYOUT_LIGHT_FREIGHTER);
    ASSERT(ship_cell_graph(&ship, &graph));
    ASSERT_EQ_INT(graph.count, 5);
    ship_cell_totals(&ship, &totals);
    ASSERT_EQ_INT(totals.struts, 24);
    ASSERT_EQ_INT(totals.cargo_capacity, 72);
    ASSERT_EQ_FLOAT(totals.shell_mass, 24.0f, 0.001f);
    ASSERT_EQ_FLOAT(totals.thrust_units, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(ship_cargo_capacity(&ship), 72.0f, 0.001f);
    vec2 empty_center = ship_cell_center_of_mass(&ship);
    ship.cargo[COMMODITY_FERRITE_ORE] = 72.0f;
    vec2 full_center = ship_cell_center_of_mass(&ship);
    ASSERT(full_center.x > empty_center.x);
    ship.cargo[COMMODITY_FERRITE_ORE] = 0.0f;

    ship.hull_class = HULL_CLASS_DRONE_TRACTOR;
    ASSERT_EQ_INT(ship_cell_layout_kind(ship.hull_class), CELL_LAYOUT_TUG);
    ASSERT_EQ_FLOAT(ship_cell_shell_mass(&ship), 12.0f, 0.001f);
    ASSERT_EQ_FLOAT(ship_cell_thrust_units(&ship), 1.0f, 0.001f);
}

TEST(test_ship_asset_cell_identities_survive_carrier_detach) {
    ship_t ship = {.hull_class = HULL_CLASS_HAULER};
    cell_graph_t graph;
    cell_node_t detached;
    ASSERT(ship_cell_graph_for_identity(&ship, 77, &graph));
    uint64_t control_id = graph.nodes[0].identity;
    uint64_t carrier_id = graph.nodes[2].identity;
    ASSERT(control_id == ((uint64_t)77 << 32) + 1);
    graph.nodes[2].payload_units = 9;
    ASSERT(cell_graph_remove_node(&graph, carrier_id, &detached));
    ASSERT(graph.nodes[0].identity == control_id);
    ASSERT(detached.identity == carrier_id);
    ASSERT_EQ_INT(detached.payload_units, 9);
}

TEST(test_ship_mining_rate_with_upgrades) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.mining_level = 0;
    ASSERT_EQ_FLOAT(ship_mining_rate(&ship), 28.0f, 0.01f);
    ship.mining_level = 3;
    ASSERT_EQ_FLOAT(ship_mining_rate(&ship), 28.0f + 3 * 7.0f, 0.01f);
}

TEST(test_ship_upgrade_maxed) {
    ship_t ship = {0};
    ship.mining_level = 3;
    ASSERT(!ship_upgrade_maxed(&ship, SHIP_UPGRADE_MINING));
    ship.mining_level = 4;
    ASSERT(ship_upgrade_maxed(&ship, SHIP_UPGRADE_MINING));
}

TEST(test_ship_upgrade_cost_escalates) {
    ship_t ship = {0};
    ship.mining_level = 0;
    int cost0 = ship_upgrade_cost(&ship, SHIP_UPGRADE_MINING);
    ship.mining_level = 1;
    int cost1 = ship_upgrade_cost(&ship, SHIP_UPGRADE_MINING);
    ship.mining_level = 2;
    int cost2 = ship_upgrade_cost(&ship, SHIP_UPGRADE_MINING);
    ASSERT(cost1 > cost0);
    ASSERT(cost2 > cost1);
}

TEST(test_upgrade_required_product) {
    ASSERT_EQ_INT(upgrade_required_product(SHIP_UPGRADE_HOLD), PRODUCT_FRAME);
    ASSERT_EQ_INT(upgrade_required_product(SHIP_UPGRADE_MINING), PRODUCT_LASER_MODULE);
    ASSERT_EQ_INT(upgrade_required_product(SHIP_UPGRADE_TRACTOR), PRODUCT_TRACTOR_MODULE);
}

TEST(test_upgrade_product_cost_scales_with_level) {
    ship_t ship = {0};
    ship.hold_level = 0;
    ASSERT_EQ_FLOAT(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD), UPGRADE_BASE_PRODUCT * 1.0f, 0.01f);
    ship.hold_level = 1;
    ASSERT_EQ_FLOAT(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD), UPGRADE_BASE_PRODUCT * 2.0f, 0.01f);
    ship.hold_level = 3;
    ASSERT_EQ_FLOAT(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD), UPGRADE_BASE_PRODUCT * 4.0f, 0.01f);
}

TEST(test_npc_hull_def) {
    ship_t ship = {0};
    npc_ship_t npc = {.ship = &ship};
    npc.ship->hull_class = HULL_CLASS_NPC_MINER;
    const hull_def_t* hull = npc_hull_def(&npc);
    ASSERT_STR_EQ(hull->name, "Frame-2 Mining Workboat");
    ASSERT_EQ_FLOAT(hull->cargo_capacity, 24.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->mining_rate, 28.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->tractor_range, 150.0f, 0.01f);
}

TEST(test_npc_tow_uses_embedded_ship_only) {
    ship_t ship = {0};
    npc_ship_t npc = {.ship = &ship};
    npc_clear_towed_fragment(&npc);

    npc.ship->towed_fragments[0] = 42;
    npc.ship->towed_count = 1;
    ASSERT_EQ_INT(npc_towed_fragment_index(&npc), 42);

    npc_set_towed_fragment_index(&npc, 7);
    ASSERT_EQ_INT(npc_towed_fragment_index(&npc), 7);
    ASSERT_EQ_INT(npc.ship->towed_fragments[0], 7);

    npc_clear_towed_fragment(&npc);
    ASSERT_EQ_INT(npc_towed_fragment_index(&npc), -1);
    ASSERT_EQ_INT(npc.ship->towed_count, 0);
    ASSERT_EQ_INT(npc.ship->towed_fragments[0], -1);
}

TEST(test_ship_boost_curve_uses_deterministic_exp) {
    float boosted = ship_boost_thrust_mult(true, 0.5f);
    float expected = 1.6f + 0.4f * fixp_expf(-1.5f);
    ASSERT_EQ_FLOAT(boosted, expected, 0.0001f);
    ASSERT_EQ_FLOAT(ship_boost_thrust_mult(false, 0.5f), 1.0f, 0.0001f);
}

TEST(test_ship_circle_pushback_deterministic_reference) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.pos = v2(6.0f, 8.0f);
    ship.vel = v2(-3.0f, -4.0f);

    float impact = resolve_ship_circle_pushback(&ship, v2(0.0f, 0.0f), 10.0f);
    float pushed = 10.0f + ship_hull_def(&ship)->ship_radius + SHIP_COLLISION_SKIN;
    ASSERT_EQ_FLOAT(impact, 5.0f, 0.001f);
    ASSERT_EQ_FLOAT(v2_len(ship.pos), pushed, 0.001f);
    ASSERT_EQ_FLOAT(ship.pos.x, pushed * 0.6f, 0.001f);
    ASSERT_EQ_FLOAT(ship.pos.y, pushed * 0.8f, 0.001f);
    ASSERT_EQ_FLOAT(v2_len(ship.vel), 0.0f, 0.001f);
}

TEST(test_ship_asteroid_pushback_deterministic_reference) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.pos = v2(9.0f, 12.0f);
    ship.vel = v2(-6.0f, -8.0f);

    asteroid_t rock = {0};
    rock.active = true;
    rock.pos = v2(0.0f, 0.0f);
    rock.vel = v2(0.0f, 0.0f);
    rock.radius = 12.0f;

    float impact = resolve_ship_asteroid_pushback(&ship, &rock);
    float pushed = rock.radius + ship_hull_def(&ship)->ship_radius + SHIP_COLLISION_SKIN;
    ASSERT_EQ_FLOAT(impact, 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(v2_len(ship.pos), pushed, 0.001f);
    ASSERT_EQ_FLOAT(ship.vel.x, -3.0f, 0.001f);
    ASSERT_EQ_FLOAT(ship.vel.y, -4.0f, 0.001f);
    ASSERT_EQ_FLOAT(rock.vel.x, -3.0f, 0.001f);
    ASSERT_EQ_FLOAT(rock.vel.y, -4.0f, 0.001f);
    ASSERT(rock.net_dirty);
}

TEST(test_ship_annular_pushback_uses_deterministic_angle_margin) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.pos = v2(50.0f, 0.0f);
    ship.vel = v2(-7.0f, 0.0f);

    float impact = resolve_ship_annular_pushback(&ship, v2(0.0f, 0.0f),
                                                 50.0f, -0.2f, 0.4f);
    float outer = 50.0f + STATION_CORRIDOR_HW +
                  ship_hull_def(&ship)->ship_radius + SHIP_COLLISION_SKIN;
    ASSERT(fixp_asinf(ship_hull_def(&ship)->ship_radius / 50.0f) > 0.0f);
    ASSERT_EQ_FLOAT(impact, 7.0f, 0.001f);
    ASSERT_EQ_FLOAT(v2_len(ship.pos), outer, 0.001f);
    ASSERT_EQ_FLOAT(v2_len(ship.vel), 0.0f, 0.001f);
}

TEST(test_ship_fragment_tow_applies_ship_reaction) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.pos = v2(0.0f, 0.0f);
    ship.vel = v2(0.0f, 0.0f);

    asteroid_t fragment = {0};
    fragment.active = true;
    fragment.tier = ASTEROID_TIER_S;
    fragment.pos = v2(200.0f, 0.0f);
    fragment.vel = v2(0.0f, 0.0f);

    ship_apply_fragment_tow(&ship, &fragment, 1.0f / 60.0f);

    ASSERT(fragment.vel.x < 0.0f);
    ASSERT(ship.vel.x > 0.0f);
}

TEST(test_ship_tow_applies_to_ship_like_body) {
    ship_t tractor = {0};
    tractor.hull_class = HULL_CLASS_MINER;
    tractor.pos = v2(0.0f, 0.0f);
    tractor.vel = v2(0.0f, 0.0f);

    ship_t pod = {0};
    pod.hull_class = HULL_CLASS_MINER;
    pod.pos = v2(220.0f, 0.0f);
    pod.vel = v2(0.0f, 0.0f);

    towable_body_t body = {
        .pos = &pod.pos,
        .vel = &pod.vel,
        .inv_mass = 1.0f,
    };
    ship_apply_body_tow(&tractor, &body, 1.0f / 60.0f);

    ASSERT(pod.vel.x < 0.0f);
    ASSERT(tractor.vel.x > 0.0f);
}

TEST(test_ship_tow_release_is_body_agnostic) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.pos = v2(20.0f, 10.0f);
    ship.vel = v2(30.0f, -5.0f);

    vec2 fragment_vel = v2(-100.0f, 20.0f);
    vec2 pod_vel = v2(90.0f, -40.0f);
    vec2 body_pos = v2(-80.0f, 10.0f);
    ship_release_body_tow(&ship, body_pos, &fragment_vel);
    ship_release_body_tow(&ship, body_pos, &pod_vel);

    ASSERT_EQ_FLOAT(fragment_vel.x, pod_vel.x, 0.001f);
    ASSERT_EQ_FLOAT(fragment_vel.y, pod_vel.y, 0.001f);
    ASSERT_EQ_FLOAT(fragment_vel.x, 110.0f, 0.001f);
    ASSERT_EQ_FLOAT(fragment_vel.y, -5.0f, 0.001f);
}

TEST(test_hex_pod_mass_hardpoints_and_polygon_are_shape_aware) {
    cargo_pod_t pod = {
        .active = true,
        .kind = CARGO_POD_CARGO,
        .quantity = 24,
        .pos = {10.0f, 20.0f},
        .radius = 20.0f,
    };
    ASSERT_EQ_FLOAT(cargo_pod_shell_mass(&pod), 6.0f, 0.001f);
    ASSERT_EQ_FLOAT(cargo_pod_payload_mass(&pod), 6.0f, 0.001f);
    ASSERT_EQ_FLOAT(cargo_pod_total_mass(&pod), 12.0f, 0.001f);
    ASSERT_EQ_FLOAT(cargo_pod_inverse_mass(&pod), 1.0f / 12.0f, 0.0001f);

    int east = cargo_pod_select_hardpoint(&pod, v2(100.0f, 20.0f));
    ASSERT_EQ_INT(east, 0);
    ASSERT_STR_EQ(cargo_pod_hardpoint_name(east), "east");
    vec2 hardpoint = cargo_pod_hardpoint_world(&pod, east);
    ASSERT(hardpoint.x > pod.pos.x);
    ASSERT_EQ_FLOAT(hardpoint.y, pod.pos.y, 0.001f);

    ASSERT(cargo_pod_contains_point(&pod, pod.pos));
    ASSERT(cargo_pod_contains_point(&pod, v2(10.0f, 37.9f)));
    ASSERT(!cargo_pod_contains_point(&pod, v2(28.0f, 20.0f)));
    ASSERT_EQ_FLOAT(cargo_pod_support_radius(&pod, v2(0.0f, 1.0f)),
                    18.0f, 0.001f);
    ASSERT_EQ_FLOAT(cargo_pod_support_radius(&pod, v2(1.0f, 0.0f)),
                    18.0f * 0.8660254f, 0.001f);
}

TEST(test_off_center_tow_applies_angular_impulse) {
    ship_t tractor = {.hull_class = HULL_CLASS_MINER};
    tractor.pos = v2(0.0f, 0.0f);
    vec2 body_pos = v2(220.0f, 0.0f);
    vec2 body_vel = v2(0.0f, 0.0f);
    float angle = 0.0f;
    float spin = 0.0f;
    towable_body_t body = {
        .pos = &body_pos,
        .vel = &body_vel,
        .inv_mass = 0.5f,
        .attachment_offset = {0.0f, 10.0f},
        .angle = &angle,
        .spin = &spin,
        .inv_inertia = 0.02f,
    };
    ship_apply_body_tow(&tractor, &body, 1.0f / 60.0f);
    ASSERT(body_vel.x < 0.0f);
    ASSERT(spin > 0.0f);
}

TEST(test_cargo_pod_tow_aligns_selected_edge_without_spin) {
    ship_t tractor = {.hull_class = HULL_CLASS_MINER};
    tractor.pos = v2(0.0f, 0.0f);
    tractor.angle = 0.35f;
    cargo_pod_t pod = {
        .active = true,
        .kind = CARGO_POD_CARGO,
        .quantity = 24,
        .radius = 20.0f,
        .pos = {220.0f, 40.0f},
        .rotation = 1.7f,
        .spin = 9.0f,
    };
    const int hardpoint = 2;
    const float dt = 1.0f / 120.0f;

    /* A full minute of changing tow geometry must not accumulate angular
     * velocity. The selected edge follows the source as an aligned joint. */
    for (int tick = 0; tick < 120 * 60; tick++) {
        tractor.pos.x += tractor.vel.x * dt;
        tractor.pos.y += tractor.vel.y * dt;
        pod.pos.x += pod.vel.x * dt;
        pod.pos.y += pod.vel.y * dt;
        pod.vel = v2_scale(pod.vel, 1.0f / (1.0f + 0.35f * dt));

        /* Exercise alignment changes instead of a single static axis. */
        tractor.pos.y = fixp_sinf((float)tick * 0.0025f) * 70.0f;
        ship_apply_cargo_pod_tow(&tractor, &pod, hardpoint, dt);

        vec2 source = ship_tow_hardpoint_world(&tractor);
        vec2 to_source = v2_sub(source, pod.pos);
        float expected = wrap_angle(
            fixp_atan2f(to_source.y, to_source.x) -
            (float)hardpoint * 1.0471975511965976f);
        ASSERT_EQ_FLOAT(pod.spin, 0.0f, 0.000001f);
        ASSERT_EQ_FLOAT(pod.rotation, expected, 0.00001f);
    }
}

TEST(test_tow_mass_centering_and_sixty_degree_rotation) {
    cargo_pod_t empty = {
        .active = true,
        .kind = CARGO_POD_CARGO,
        .quantity = 0,
        .radius = 20.0f,
    };
    cargo_pod_t loaded = empty;
    loaded.quantity = 24;

    ship_t light_tractor = {.hull_class = HULL_CLASS_MINER};
    ship_t heavy_tractor = light_tractor;
    vec2 light_pos = v2(220.0f, 0.0f), light_vel = v2(0.0f, 0.0f);
    vec2 heavy_pos = light_pos, heavy_vel = light_vel;
    towable_body_t light_body = {
        .pos = &light_pos,
        .vel = &light_vel,
        .inv_mass = cargo_pod_inverse_mass(&empty),
    };
    towable_body_t heavy_body = {
        .pos = &heavy_pos,
        .vel = &heavy_vel,
        .inv_mass = cargo_pod_inverse_mass(&loaded),
    };
    ship_apply_body_tow(&light_tractor, &light_body, 1.0f / 60.0f);
    ship_apply_body_tow(&heavy_tractor, &heavy_body, 1.0f / 60.0f);
    ASSERT(v2_len(light_vel) > v2_len(heavy_vel));

    /* A centered edge attachment lies on the band axis and therefore cannot
     * manufacture angular momentum. */
    ship_t centered_tractor = {.hull_class = HULL_CLASS_MINER};
    vec2 centered_pos = v2(220.0f, 0.0f), centered_vel = v2(0.0f, 0.0f);
    float centered_angle = 0.0f, centered_spin = 0.0f;
    vec2 centered_axis = v2_norm(v2_sub(
        ship_tow_hardpoint_world(&centered_tractor), centered_pos));
    towable_body_t centered = {
        .pos = &centered_pos,
        .vel = &centered_vel,
        .inv_mass = cargo_pod_inverse_mass(&loaded),
        .attachment_offset = {
            centered_axis.x * 10.0f, centered_axis.y * 10.0f,
        },
        .angle = &centered_angle,
        .spin = &centered_spin,
        .inv_inertia = cargo_pod_inverse_inertia(&loaded),
    };
    ship_apply_body_tow(&centered_tractor, &centered, 1.0f / 60.0f);
    ASSERT_EQ_FLOAT(centered_spin, 0.0f, 0.0001f);

    /* Rotating the complete ship/pod/hardpoint scene by one grammar step
     * rotates the linear result by 60 degrees while preserving yaw. */
    const float c60 = 0.5f, s60 = 0.8660254037844386f;
    ship_t tractor_a = {.hull_class = HULL_CLASS_MINER};
    ship_t tractor_b = tractor_a;
    tractor_b.angle = 1.0471975511965976f;
    cargo_pod_t pod_a = loaded;
    pod_a.pos = v2(220.0f, 40.0f);
    pod_a.rotation = 0.2f;
    cargo_pod_set_tow_hardpoint(&pod_a, 2);
    cargo_pod_t pod_b = pod_a;
    pod_b.pos = v2(pod_a.pos.x * c60 - pod_a.pos.y * s60,
                   pod_a.pos.x * s60 + pod_a.pos.y * c60);
    pod_b.rotation += 1.0471975511965976f;
    towable_body_t body_a = {
        .pos = &pod_a.pos,
        .vel = &pod_a.vel,
        .inv_mass = cargo_pod_inverse_mass(&pod_a),
        .attachment_offset = cargo_pod_hardpoint_offset(
            &pod_a, cargo_pod_tow_hardpoint(&pod_a)),
        .angle = &pod_a.rotation,
        .spin = &pod_a.spin,
        .inv_inertia = cargo_pod_inverse_inertia(&pod_a),
    };
    towable_body_t body_b = {
        .pos = &pod_b.pos,
        .vel = &pod_b.vel,
        .inv_mass = cargo_pod_inverse_mass(&pod_b),
        .attachment_offset = cargo_pod_hardpoint_offset(
            &pod_b, cargo_pod_tow_hardpoint(&pod_b)),
        .angle = &pod_b.rotation,
        .spin = &pod_b.spin,
        .inv_inertia = cargo_pod_inverse_inertia(&pod_b),
    };
    ship_apply_body_tow(&tractor_a, &body_a, 1.0f / 60.0f);
    ship_apply_body_tow(&tractor_b, &body_b, 1.0f / 60.0f);
    vec2 rotated_a = v2(pod_a.vel.x * c60 - pod_a.vel.y * s60,
                        pod_a.vel.x * s60 + pod_a.vel.y * c60);
    ASSERT_EQ_FLOAT(pod_b.vel.x, rotated_a.x, 0.001f);
    ASSERT_EQ_FLOAT(pod_b.vel.y, rotated_a.y, 0.001f);
    ASSERT_EQ_FLOAT(pod_b.spin, pod_a.spin, 0.001f);
}

TEST(test_product_name) {
    ASSERT_STR_EQ(product_name(PRODUCT_FRAME), "Frames");
    ASSERT_STR_EQ(product_name(PRODUCT_LASER_MODULE), "Laser Modules");
    ASSERT_STR_EQ(product_name(PRODUCT_TRACTOR_MODULE), "Tractor Modules");
}

void register_ship_tests(void) {
    TEST_SECTION("\nShip tests:\n");
    RUN(test_ship_hull_def_miner);
    RUN(test_ship_hull_def_hauler);
    RUN(test_ship_loadout_metadata_tracks_module_sockets);
    RUN(test_ship_max_hull);
    RUN(test_ship_cargo_capacity_with_upgrades);
    RUN(test_ship_cell_graph_derives_capacity_mass_and_thrust);
    RUN(test_ship_asset_cell_identities_survive_carrier_detach);
    RUN(test_ship_mining_rate_with_upgrades);
    RUN(test_ship_upgrade_maxed);
    RUN(test_ship_upgrade_cost_escalates);
    RUN(test_upgrade_required_product);
    RUN(test_upgrade_product_cost_scales_with_level);
    RUN(test_npc_hull_def);
    RUN(test_npc_tow_uses_embedded_ship_only);
    RUN(test_ship_boost_curve_uses_deterministic_exp);
    RUN(test_ship_circle_pushback_deterministic_reference);
    RUN(test_ship_asteroid_pushback_deterministic_reference);
    RUN(test_ship_annular_pushback_uses_deterministic_angle_margin);
    RUN(test_ship_fragment_tow_applies_ship_reaction);
    RUN(test_ship_tow_applies_to_ship_like_body);
    RUN(test_ship_tow_release_is_body_agnostic);
    RUN(test_hex_pod_mass_hardpoints_and_polygon_are_shape_aware);
    RUN(test_off_center_tow_applies_angular_impulse);
    RUN(test_cargo_pod_tow_aligns_selected_edge_without_spin);
    RUN(test_tow_mass_centering_and_sixty_degree_rotation);
    RUN(test_product_name);
}
