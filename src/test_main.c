#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "ship.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    tests_run++; \
    printf("  %s ... ", #name); \
    name(); \
    tests_passed++; \
    printf("ok\n"); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_FLOAT(a, b, eps) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) > (eps)) { \
        printf("FAIL\n    %s:%d: %s == %.4f, expected %.4f\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char* _a = (a); const char* _b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("FAIL\n    %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        return; \
    } \
} while(0)

/* Need HULL_DEFS definition for tests that use ship stats */
const hull_def_t HULL_DEFS[HULL_CLASS_COUNT] = {
    [HULL_CLASS_MINER] = {
        .name = "Mining Cutter",
        .max_hull = 100.0f,
        .accel = 300.0f,
        .turn_speed = 2.75f,
        .drag = 0.45f,
        .ore_capacity = 120.0f,
        .ingot_capacity = 0.0f,
        .mining_rate = 28.0f,
        .tractor_range = 150.0f,
        .ship_radius = 16.0f,
        .render_scale = 1.0f,
    },
    [HULL_CLASS_HAULER] = {
        .name = "Cargo Hauler",
        .max_hull = 150.0f,
        .accel = 140.0f,
        .turn_speed = 1.6f,
        .drag = 0.55f,
        .ore_capacity = 0.0f,
        .ingot_capacity = 40.0f,
        .mining_rate = 0.0f,
        .tractor_range = 0.0f,
        .ship_radius = 18.0f,
        .render_scale = 0.85f,
    },
    [HULL_CLASS_NPC_MINER] = {
        .name = "Mining Drone",
        .max_hull = 80.0f,
        .accel = 180.0f,
        .turn_speed = 2.0f,
        .drag = 0.5f,
        .ore_capacity = 60.0f,
        .ingot_capacity = 0.0f,
        .mining_rate = 14.0f,
        .tractor_range = 0.0f,
        .ship_radius = 12.0f,
        .render_scale = 0.7f,
    },
};

/* ---- Commodity Tests ---- */

TEST(test_refined_form_mapping) {
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_FERRITE_ORE), COMMODITY_FRAME_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CUPRITE_ORE), COMMODITY_CONDUCTOR_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CRYSTAL_ORE), COMMODITY_LENS_INGOT);
}

TEST(test_refined_form_ingots_return_self) {
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_FRAME_INGOT), COMMODITY_FRAME_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CONDUCTOR_INGOT), COMMODITY_CONDUCTOR_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_LENS_INGOT), COMMODITY_LENS_INGOT);
}

TEST(test_commodity_name) {
    ASSERT_STR_EQ(commodity_name(COMMODITY_FERRITE_ORE), "Ferrite Ore");
    ASSERT_STR_EQ(commodity_name(COMMODITY_FRAME_INGOT), "Frame Ingots");
    ASSERT_STR_EQ(commodity_name(COMMODITY_COUNT), "Cargo");
}

TEST(test_commodity_code) {
    ASSERT_STR_EQ(commodity_code(COMMODITY_FERRITE_ORE), "FE");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CUPRITE_ORE), "CU");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CRYSTAL_ORE), "CR");
    ASSERT_STR_EQ(commodity_code(COMMODITY_FRAME_INGOT), "FR");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CONDUCTOR_INGOT), "CO");
    ASSERT_STR_EQ(commodity_code(COMMODITY_LENS_INGOT), "LN");
}

TEST(test_commodity_short_name) {
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_FERRITE_ORE), "Ferrite");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_FRAME_INGOT), "Frame");
}

TEST(test_ship_raw_ore_total) {
    ship_t ship = {0};
    ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    ship.cargo[COMMODITY_CUPRITE_ORE] = 20.0f;
    ship.cargo[COMMODITY_CRYSTAL_ORE] = 5.0f;
    ship.cargo[COMMODITY_FRAME_INGOT] = 100.0f;
    ASSERT_EQ_FLOAT(ship_raw_ore_total(&ship), 35.0f, 0.01f);
}

TEST(test_ship_total_cargo) {
    ship_t ship = {0};
    ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    ship.cargo[COMMODITY_FRAME_INGOT] = 5.0f;
    ASSERT_EQ_FLOAT(ship_total_cargo(&ship), 15.0f, 0.01f);
}

TEST(test_ship_cargo_amount) {
    ship_t ship = {0};
    ship.cargo[COMMODITY_CUPRITE_ORE] = 42.0f;
    ASSERT_EQ_FLOAT(ship_cargo_amount(&ship, COMMODITY_CUPRITE_ORE), 42.0f, 0.01f);
    ASSERT_EQ_FLOAT(ship_cargo_amount(&ship, COMMODITY_FERRITE_ORE), 0.0f, 0.01f);
}

TEST(test_station_buy_price) {
    station_t station = {0};
    station.buy_price[COMMODITY_FERRITE_ORE] = 10.0f;
    station.buy_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_FERRITE_ORE), 10.0f, 0.01f);
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_CRYSTAL_ORE), 18.0f, 0.01f);
    ASSERT_EQ_FLOAT(station_buy_price(NULL, COMMODITY_FERRITE_ORE), 0.0f, 0.01f);
}

TEST(test_station_inventory_amount) {
    station_t station = {0};
    station.inventory[COMMODITY_FRAME_INGOT] = 25.0f;
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FRAME_INGOT), 25.0f, 0.01f);
    ASSERT_EQ_FLOAT(station_inventory_amount(NULL, COMMODITY_FRAME_INGOT), 0.0f, 0.01f);
}

/* ---- Math Tests ---- */

TEST(test_v2_add) {
    vec2 a = v2(1.0f, 2.0f);
    vec2 b = v2(3.0f, 4.0f);
    vec2 c = v2_add(a, b);
    ASSERT_EQ_FLOAT(c.x, 4.0f, 0.001f);
    ASSERT_EQ_FLOAT(c.y, 6.0f, 0.001f);
}

TEST(test_v2_len) {
    vec2 a = v2(3.0f, 4.0f);
    ASSERT_EQ_FLOAT(v2_len(a), 5.0f, 0.001f);
}

TEST(test_v2_norm) {
    vec2 a = v2(0.0f, 5.0f);
    vec2 n = v2_norm(a);
    ASSERT_EQ_FLOAT(n.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(n.y, 1.0f, 0.001f);
}

TEST(test_v2_norm_zero) {
    vec2 a = v2(0.0f, 0.0f);
    vec2 n = v2_norm(a);
    ASSERT_EQ_FLOAT(n.x, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(n.y, 0.0f, 0.001f);
}

TEST(test_wrap_angle) {
    ASSERT_EQ_FLOAT(wrap_angle(0.0f), 0.0f, 0.001f);
    ASSERT(wrap_angle(4.0f) < PI_F);
    ASSERT(wrap_angle(-4.0f) > -PI_F);
}

TEST(test_clampf) {
    ASSERT_EQ_FLOAT(clampf(0.5f, 0.0f, 1.0f), 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(clampf(-1.0f, 0.0f, 1.0f), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(clampf(2.0f, 0.0f, 1.0f), 1.0f, 0.001f);
}

TEST(test_lerpf) {
    ASSERT_EQ_FLOAT(lerpf(0.0f, 10.0f, 0.5f), 5.0f, 0.001f);
    ASSERT_EQ_FLOAT(lerpf(0.0f, 10.0f, 0.0f), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(lerpf(0.0f, 10.0f, 1.0f), 10.0f, 0.001f);
}

/* ---- INGOT_IDX Tests ---- */

TEST(test_ingot_idx) {
    ASSERT_EQ_INT(INGOT_IDX(COMMODITY_FRAME_INGOT), 0);
    ASSERT_EQ_INT(INGOT_IDX(COMMODITY_CONDUCTOR_INGOT), 1);
    ASSERT_EQ_INT(INGOT_IDX(COMMODITY_LENS_INGOT), 2);
    ASSERT_EQ_INT(INGOT_COUNT, 3);
}

/* ---- Ship Tests ---- */

TEST(test_ship_hull_def_miner) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    const hull_def_t* hull = ship_hull_def(&ship);
    ASSERT_STR_EQ(hull->name, "Mining Cutter");
    ASSERT_EQ_FLOAT(hull->max_hull, 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(hull->ore_capacity, 120.0f, 0.01f);
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
    ASSERT_EQ_FLOAT(ship_cargo_capacity(&ship), 120.0f, 0.01f);
    ship.hold_level = 2;
    ASSERT_EQ_FLOAT(ship_cargo_capacity(&ship), 120.0f + 2 * 24.0f, 0.01f);
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
    ASSERT_EQ_FLOAT(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD), 8.0f, 0.01f);
    ship.hold_level = 1;
    ASSERT_EQ_FLOAT(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD), 16.0f, 0.01f);
    ship.hold_level = 3;
    ASSERT_EQ_FLOAT(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD), 32.0f, 0.01f);
}

TEST(test_npc_hull_def) {
    npc_ship_t npc = {0};
    npc.hull_class = HULL_CLASS_NPC_MINER;
    const hull_def_t* hull = npc_hull_def(&npc);
    ASSERT_STR_EQ(hull->name, "Mining Drone");
    ASSERT_EQ_FLOAT(hull->ore_capacity, 60.0f, 0.01f);
}

TEST(test_product_name) {
    ASSERT_STR_EQ(product_name(PRODUCT_FRAME), "Frames");
    ASSERT_STR_EQ(product_name(PRODUCT_LASER_MODULE), "Laser Modules");
    ASSERT_STR_EQ(product_name(PRODUCT_TRACTOR_MODULE), "Tractor Modules");
}

/* ---- Runner ---- */

int main(void) {
    printf("Commodity tests:\n");
    RUN(test_refined_form_mapping);
    RUN(test_refined_form_ingots_return_self);
    RUN(test_commodity_name);
    RUN(test_commodity_code);
    RUN(test_commodity_short_name);
    RUN(test_ship_raw_ore_total);
    RUN(test_ship_total_cargo);
    RUN(test_ship_cargo_amount);
    RUN(test_station_buy_price);
    RUN(test_station_inventory_amount);

    printf("\nMath tests:\n");
    RUN(test_v2_add);
    RUN(test_v2_len);
    RUN(test_v2_norm);
    RUN(test_v2_norm_zero);
    RUN(test_wrap_angle);
    RUN(test_clampf);
    RUN(test_lerpf);

    printf("\nType tests:\n");
    RUN(test_ingot_idx);

    printf("\nShip tests:\n");
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

    printf("\n%d tests run, %d passed, %d failed\n", tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
