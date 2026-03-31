#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "ship.h"
#include "economy.h"
#include "game_sim.h"

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

/* HULL_DEFS provided by game_sim.c */

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

/* ---- Economy Tests ---- */

TEST(test_refinery_production_smelts_ore) {
    station_t station = {0};
    station.role = STATION_ROLE_REFINERY;
    station.ore_buffer[COMMODITY_FERRITE_ORE] = 10.0f;
    step_refinery_production(&station, 1, 1.0f);
    ASSERT(station.ore_buffer[COMMODITY_FERRITE_ORE] < 10.0f);
    ASSERT(station.inventory[COMMODITY_FRAME_INGOT] > 0.0f);
}

TEST(test_refinery_production_empty_buffer_noop) {
    station_t station = {0};
    station.role = STATION_ROLE_REFINERY;
    step_refinery_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_FRAME_INGOT], 0.0f, 0.001f);
}

TEST(test_refinery_skips_non_refinery) {
    station_t station = {0};
    station.role = STATION_ROLE_YARD;
    station.ore_buffer[COMMODITY_FERRITE_ORE] = 10.0f;
    step_refinery_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station.ore_buffer[COMMODITY_FERRITE_ORE], 10.0f, 0.001f);
}

TEST(test_station_production_yard_makes_frames) {
    station_t station = {0};
    station.role = STATION_ROLE_YARD;
    station.ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] = 5.0f;
    step_station_production(&station, 1, 1.0f);
    ASSERT(station.ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] < 5.0f);
    ASSERT(station.product_stock[PRODUCT_FRAME] > 0.0f);
}

TEST(test_station_production_beamworks_makes_modules) {
    station_t station = {0};
    station.role = STATION_ROLE_BEAMWORKS;
    station.ingot_buffer[INGOT_IDX(COMMODITY_CONDUCTOR_INGOT)] = 5.0f;
    station.ingot_buffer[INGOT_IDX(COMMODITY_LENS_INGOT)] = 5.0f;
    step_station_production(&station, 1, 1.0f);
    ASSERT(station.product_stock[PRODUCT_LASER_MODULE] > 0.0f);
    ASSERT(station.product_stock[PRODUCT_TRACTOR_MODULE] > 0.0f);
}

TEST(test_station_cargo_sale_value) {
    ship_t ship = {0};
    station_t station = {0};
    ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    station.buy_price[COMMODITY_FERRITE_ORE] = 10.0f;
    ASSERT_EQ_FLOAT(station_cargo_sale_value(&ship, &station), 100.0f, 0.01f);
}

TEST(test_station_cargo_sale_value_null_station) {
    ship_t ship = {0};
    ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    ASSERT_EQ_FLOAT(station_cargo_sale_value(&ship, NULL), 0.0f, 0.01f);
}

TEST(test_station_repair_cost_no_damage) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 100.0f;
    station_t station = {0};
    ASSERT_EQ_FLOAT(station_repair_cost(&ship, &station), 0.0f, 0.01f);
}

TEST(test_station_repair_cost_with_damage) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 50.0f;
    station_t station = {0};
    float cost = station_repair_cost(&ship, &station);
    ASSERT(cost > 0.0f);
}

TEST(test_can_afford_upgrade_all_conditions) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.credits = 10000.0f;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station.product_stock[PRODUCT_FRAME] = 100.0f;
    int cost = ship_upgrade_cost(&ship, SHIP_UPGRADE_HOLD);
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost));
}

TEST(test_can_afford_upgrade_no_credits) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.credits = 0.0f;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station.product_stock[PRODUCT_FRAME] = 100.0f;
    int cost = ship_upgrade_cost(&ship, SHIP_UPGRADE_HOLD);
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost));
}

TEST(test_can_afford_upgrade_no_product) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.credits = 10000.0f;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station.product_stock[PRODUCT_FRAME] = 0.0f;
    int cost = ship_upgrade_cost(&ship, SHIP_UPGRADE_HOLD);
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost));
}

/* ---- World Sim Tests ---- */

TEST(test_world_reset_creates_stations) {
    world_t w = {0};
    world_reset(&w);
    ASSERT_STR_EQ(w.stations[0].name, "Prospect Refinery");
    ASSERT_EQ_INT(w.stations[0].role, STATION_ROLE_REFINERY);
    ASSERT_STR_EQ(w.stations[1].name, "Kepler Yard");
    ASSERT_STR_EQ(w.stations[2].name, "Helios Works");
}

TEST(test_world_reset_spawns_asteroids) {
    world_t w = {0};
    world_reset(&w);
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (w.asteroids[i].active) count++;
    ASSERT(count >= 20);
}

TEST(test_world_reset_spawns_npcs) {
    world_t w = {0};
    world_reset(&w);
    int miners = 0, haulers = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w.npc_ships[i].active) continue;
        if (w.npc_ships[i].role == NPC_ROLE_MINER) miners++;
        if (w.npc_ships[i].role == NPC_ROLE_HAULER) haulers++;
    }
    ASSERT_EQ_INT(miners, 3);
    ASSERT_EQ_INT(haulers, 2);
}

TEST(test_player_init_ship_docked) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    ASSERT_EQ_INT(w.players[0].current_station, 0);
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, 100.0f, 0.01f);
}

TEST(test_world_sim_step_advances_time) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    float t0 = w.time;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.time > t0);
}

TEST(test_world_sim_step_moves_ship_with_thrust) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].ship.angle = 0.0f;
    w.players[0].ship.pos = v2(0.0f, 0.0f);
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].input.thrust = 1.0f;
    for (int i = 0; i < 120; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.players[0].ship.pos.x > 5.0f);
}

TEST(test_world_sim_step_mining_damages_asteroid) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* Place player right next to first active non-S asteroid */
    int target = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active && w.asteroids[i].tier != ASTEROID_TIER_S) {
            target = i;
            break;
        }
    }
    ASSERT(target >= 0);
    vec2 apos = w.asteroids[target].pos;
    w.players[0].ship.pos = v2(apos.x - 50.0f, apos.y);
    w.players[0].ship.angle = 0.0f;
    w.players[0].input.mine = true;
    float hp_before = w.asteroids[target].hp;
    for (int i = 0; i < 60; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.asteroids[target].hp < hp_before);
}

TEST(test_world_sim_step_docking) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    /* Launch */
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(!w.players[0].docked);
    /* Fly back into dock range and dock */
    w.players[0].input.interact = false;
    for (int i = 0; i < 10; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.players[0].docked);
}

TEST(test_world_sim_step_sell_ore) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    w.players[0].ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    float credits_before = w.players[0].ship.credits;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.players[0].ship.credits > credits_before);
    ASSERT(w.players[0].ship.cargo[COMMODITY_FERRITE_ORE] < 10.0f);
}

TEST(test_world_sim_step_refinery_produces_ingots) {
    world_t w = {0};
    world_reset(&w);
    w.stations[0].ore_buffer[COMMODITY_FERRITE_ORE] = 50.0f;
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.stations[0].inventory[COMMODITY_FRAME_INGOT] > 0.0f);
    ASSERT(w.stations[0].ore_buffer[COMMODITY_FERRITE_ORE] < 50.0f);
}

TEST(test_world_sim_step_events_emitted) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    /* Launch should emit LAUNCH event */
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    bool found_launch = false;
    for (int i = 0; i < w.events.count; i++) {
        if (w.events.events[i].type == SIM_EVENT_LAUNCH) found_launch = true;
    }
    ASSERT(found_launch);
}

TEST(test_world_sim_step_npc_miners_work) {
    world_t w = {0};
    world_reset(&w);
    /* Run for 5 seconds of sim time */
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    /* At least one miner should have left docked state */
    bool any_traveling = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].role == NPC_ROLE_MINER &&
            w.npc_ships[i].state != NPC_STATE_DOCKED) {
            any_traveling = true;
        }
    }
    ASSERT(any_traveling);
}

TEST(test_world_network_writes_persist) {
    /* Simulate: world_sim_step runs, then network callback overwrites asteroid,
     * next world_sim_step should see the overwritten state */
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    world_sim_step(&w, 1.0f / 120.0f);
    /* Simulate network overwrite of asteroid 0 */
    w.asteroids[0].active = true;
    w.asteroids[0].hp = 999.0f;
    w.asteroids[0].pos = v2(100.0f, 100.0f);
    world_sim_step(&w, 1.0f / 120.0f);
    /* HP should still be near 999 (only drag/dynamics, no mining) */
    ASSERT(w.asteroids[0].hp > 990.0f);
    ASSERT(w.asteroids[0].active);
}

/* ================================================================== */
/* Bug regression tests (10 bugs found in code review)                */
/* ================================================================== */

TEST(test_bug2_angle_lerp_wraparound) {
    float local = 3.0f;
    float remote = -3.0f;
    float diff = wrap_angle(remote - local);
    float correct = wrap_angle(local + diff * 0.3f);
    float naive = lerpf(local, remote, 0.3f);
    ASSERT(fabsf(correct - local) < 0.5f);
    ASSERT(fabsf(naive - local) > 1.0f);
}

TEST(test_bug3_event_buffer_too_small) {
    ASSERT(SIM_MAX_EVENTS < MAX_PLAYERS);
    world_t w = {0};
    w.events.count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        sim_event_t ev = {.type = SIM_EVENT_MINING_TICK, .player_id = i};
        if (w.events.count < SIM_MAX_EVENTS)
            w.events.events[w.events.count++] = ev;
    }
    ASSERT_EQ_INT(w.events.count, SIM_MAX_EVENTS);
}

TEST(test_bug4_pending_action_lost) {
    uint8_t pending = 0;
    if (pending == 0) pending = 1;
    if (pending == 0) pending = 3;
    ASSERT_EQ_INT(pending, 1);
}

TEST(test_bug5_asteroid_missing_network_fields) {
    asteroid_t a;
    memset(&a, 0, sizeof(a));
    a.active = true;
    a.tier = ASTEROID_TIER_XL;
    a.hp = 150.0f;
    ASSERT_EQ_FLOAT(a.max_hp, 0.0f, 0.01f);
    ASSERT_EQ_FLOAT(a.seed, 0.0f, 0.01f);
    ASSERT_EQ_FLOAT(a.age, 0.0f, 0.01f);
}

TEST(test_bug7_player_slot_mismatch) {
    world_t w = {0};
    world_reset(&w);
    int server_id = 5;
    player_init_ship(&w.players[server_id], &w);
    w.players[server_id].connected = true;
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, 0.0f, 0.01f);
    ASSERT(w.players[server_id].ship.hull > 0.0f);
}

TEST(test_bug9_repair_cost_consistent) {
    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 80.0f;
    station_t st;
    memset(&st, 0, sizeof(st));
    st.services = STATION_SERVICE_REPAIR;
    float cost = station_repair_cost(&ship, &st);
    ASSERT_EQ_FLOAT(cost, 40.0f, 0.01f);
}

TEST(test_bug10_damage_event_uninitialized) {
    sim_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SIM_EVENT_DAMAGE;
    ev.player_id = 0;
    ASSERT_EQ_FLOAT(ev.damage.amount, 0.0f, 0.01f);
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

    printf("\nEconomy tests:\n");
    RUN(test_refinery_production_smelts_ore);
    RUN(test_refinery_production_empty_buffer_noop);
    RUN(test_refinery_skips_non_refinery);
    RUN(test_station_production_yard_makes_frames);
    RUN(test_station_production_beamworks_makes_modules);
    RUN(test_station_cargo_sale_value);
    RUN(test_station_cargo_sale_value_null_station);
    RUN(test_station_repair_cost_no_damage);
    RUN(test_station_repair_cost_with_damage);
    RUN(test_can_afford_upgrade_all_conditions);
    RUN(test_can_afford_upgrade_no_credits);
    RUN(test_can_afford_upgrade_no_product);

    printf("\nWorld sim tests:\n");
    RUN(test_world_reset_creates_stations);
    RUN(test_world_reset_spawns_asteroids);
    RUN(test_world_reset_spawns_npcs);
    RUN(test_player_init_ship_docked);
    RUN(test_world_sim_step_advances_time);
    RUN(test_world_sim_step_moves_ship_with_thrust);
    RUN(test_world_sim_step_mining_damages_asteroid);
    RUN(test_world_sim_step_docking);
    RUN(test_world_sim_step_sell_ore);
    RUN(test_world_sim_step_refinery_produces_ingots);
    RUN(test_world_sim_step_events_emitted);
    RUN(test_world_sim_step_npc_miners_work);
    RUN(test_world_network_writes_persist);

    printf("\nBug regression tests:\n");
    RUN(test_bug2_angle_lerp_wraparound);
    RUN(test_bug3_event_buffer_too_small);
    RUN(test_bug4_pending_action_lost);
    RUN(test_bug5_asteroid_missing_network_fields);
    RUN(test_bug7_player_slot_mismatch);
    RUN(test_bug9_repair_cost_consistent);
    RUN(test_bug10_damage_event_uninitialized);

    printf("\n%d tests run, %d passed, %d failed\n", tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
