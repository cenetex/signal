#include "tests/test_harness.h"

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
    ASSERT_STR_EQ(hull->name, "Cargo Hauler");
    ASSERT_EQ_FLOAT(hull->ingot_capacity, 40.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->mining_rate, 0.0f, 0.01f);
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
    npc_ship_t npc = {0};
    npc.hull_class = HULL_CLASS_NPC_MINER;
    const hull_def_t* hull = npc_hull_def(&npc);
    ASSERT_STR_EQ(hull->name, "Mining Drone");
    ASSERT_EQ_FLOAT(hull->cargo_capacity, 16.0f, 0.01f);
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
    RUN(test_ship_max_hull);
    RUN(test_ship_cargo_capacity_with_upgrades);
    RUN(test_ship_mining_rate_with_upgrades);
    RUN(test_ship_upgrade_maxed);
    RUN(test_ship_upgrade_cost_escalates);
    RUN(test_upgrade_required_product);
    RUN(test_upgrade_product_cost_scales_with_level);
    RUN(test_npc_hull_def);
    RUN(test_product_name);
}
