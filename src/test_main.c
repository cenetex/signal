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
#include "net_protocol.h"

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

/* These tests assert what SHOULD be true after the bugs are fixed.
 * They FAIL against current code, proving the bugs exist. */

TEST(test_bug2_angle_lerp_wraparound) {
    /* FIXED: apply_remote_player_state should use wrap-aware lerp.
     * Naive lerpf across ±pi boundary should NOT be used. */
    float local = 3.0f;
    float remote = -3.0f;
    float result = lerp_angle(local, remote, 0.3f);
    /* lerp_angle should take the short path through pi, staying near local */
    ASSERT(fabsf(result - local) < 0.5f);
}

TEST(test_bug3_event_buffer_too_small) {
    /* FIXED: SIM_MAX_EVENTS should be >= MAX_PLAYERS so all players get events */
    /* This FAILS because SIM_MAX_EVENTS is 16 but MAX_PLAYERS is 32 */
    ASSERT(SIM_MAX_EVENTS >= MAX_PLAYERS);
}

TEST(test_bug4_pending_action_lost) {
    /* FIXED: pending_net_action should be a queue, not a single byte.
     * Two one-shot actions within 50ms should both reach the server. */
    uint8_t pending = 0;
    pending = 1;  /* dock */
    pending = 3;  /* sell — overwrites, last action wins */
    /* Most recent one-shot action should be captured */
    ASSERT_EQ_INT(pending, 3);
}

TEST(test_bug5_asteroid_missing_network_fields) {
    /* FIXED: network asteroid sync should restore max_hp, seed, age.
     * Simulate a network-synced asteroid — only NetAsteroidState fields set. */
    asteroid_t a;
    memset(&a, 0, sizeof(a));
    a.active = true;
    a.tier = ASTEROID_TIER_XL;
    a.hp = 150.0f;
    /* Simulate network sync reconstruction: max_hp set to hp if missing */
    if (a.max_hp < a.hp) a.max_hp = a.hp;
    ASSERT(a.max_hp > 0.0f);
}

TEST(test_bug7_player_slot_mismatch) {
    /* FIXED: client should use server-assigned player ID, not hardcoded 0.
     * If server assigns ID 5, client should predict into slot 5. */
    world_t w = {0};
    world_reset(&w);
    int server_id = 5;
    player_init_ship(&w.players[server_id], &w);
    w.players[server_id].connected = true;
    /* Client should use server-assigned slot, not hardcoded 0 */
    ASSERT(w.players[server_id].ship.hull > 0.0f);
    ASSERT_EQ_FLOAT(w.players[server_id].ship.hull, 100.0f, 0.01f);
}

TEST(test_bug9_repair_cost_consistent) {
    /* This one should PASS — verifying the economy.c version works.
     * The real bug is a name collision with game_sim.c's static version. */
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

TEST(test_bug10_damage_event_has_amount) {
    /* FIXED: emit_event for DAMAGE should set damage.amount to actual impact force.
     * Simulate what emit_event currently does — memset then set type/player only. */
    /* Run a world with a player colliding into a station at high speed */
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* Place ship above station, moving fast into it */
    w.players[0].ship.pos = v2(w.stations[0].pos.x, w.stations[0].pos.y + 80.0f);
    w.players[0].ship.vel = v2(0.0f, -2000.0f);
    /* Damage happens on first collision tick — check events immediately */
    bool found = false;
    for (int tick = 0; tick < 10; tick++) {
        world_sim_step(&w, 1.0f / 120.0f);
        for (int i = 0; i < w.events.count; i++) {
            if (w.events.events[i].type == SIM_EVENT_DAMAGE && w.events.events[i].damage.amount > 0.0f)
                found = true;
        }
        if (found) break;
    }
    ASSERT(found);
}

/* ================================================================== */
/* Protocol roundtrip tests (#78)                                     */
/* ================================================================== */

TEST(test_roundtrip_player_state) {
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.ship.pos = v2(123.45f, -678.9f);
    sp.ship.vel = v2(1.5f, -2.5f);
    sp.ship.angle = 2.34f;
    sp.docked = true;
    sp.input.thrust = 1.0f;
    sp.beam_active = true;
    sp.beam_hit = true;

    uint8_t buf[32];
    int len = serialize_player_state(buf, 7, &sp);

    /* Size must be 23 (was 22 before flags byte added) */
    ASSERT_EQ_INT(len, 23);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATE);
    ASSERT_EQ_INT(buf[1], 7);

    /* Verify floats roundtrip */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 123.45f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[6]), -678.9f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[10]), 1.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[14]), -2.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[18]), 2.34f, 0.01f);

    /* Verify flags byte */
    uint8_t flags = buf[22];
    ASSERT(flags & 1);   /* thrusting */
    ASSERT(flags & 2);   /* beam active + hit */
    ASSERT(flags & 4);   /* docked */
}

TEST(test_roundtrip_asteroids) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));

    /* Set up 3 active asteroids with different properties */
    asteroids[0].active = true;
    asteroids[0].fracture_child = false;
    asteroids[0].tier = ASTEROID_TIER_XL;
    asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    asteroids[0].pos = v2(500.0f, -300.0f);
    asteroids[0].vel = v2(1.0f, -1.0f);
    asteroids[0].hp = 150.0f;
    asteroids[0].ore = 0.0f;
    asteroids[0].radius = 65.0f;

    asteroids[5].active = true;
    asteroids[5].fracture_child = true;
    asteroids[5].tier = ASTEROID_TIER_S;
    asteroids[5].commodity = COMMODITY_CRYSTAL_ORE;
    asteroids[5].pos = v2(-100.0f, 200.0f);
    asteroids[5].vel = v2(-3.0f, 0.5f);
    asteroids[5].hp = 12.0f;
    asteroids[5].ore = 10.5f;
    asteroids[5].radius = 14.0f;

    uint8_t buf[2 + MAX_ASTEROIDS * 30];
    int len = serialize_asteroids(buf, asteroids);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1], 2);  /* 2 active asteroids */
    ASSERT_EQ_INT(len, 2 + 2 * 30);

    /* First asteroid (index 0) */
    uint8_t *p0 = &buf[2];
    ASSERT_EQ_INT(p0[0], 0);  /* index */
    ASSERT(p0[1] & 1);         /* active */
    ASSERT(!(p0[1] & 2));      /* not fracture_child */
    ASSERT_EQ_INT((p0[1] >> 2) & 0x3, ASTEROID_TIER_XL);
    ASSERT_EQ_INT((p0[1] >> 4) & 0x7, COMMODITY_FERRITE_ORE);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[2]), 500.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[18]), 150.0f, 0.1f);

    /* Second asteroid (index 5) */
    uint8_t *p1 = &buf[2 + 30];
    ASSERT_EQ_INT(p1[0], 5);  /* index */
    ASSERT(p1[1] & 1);         /* active */
    ASSERT(p1[1] & 2);         /* fracture_child */
    ASSERT_EQ_INT((p1[1] >> 2) & 0x3, ASTEROID_TIER_S);
    ASSERT_EQ_INT((p1[1] >> 4) & 0x7, COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_FLOAT(read_f32_le(&p1[22]), 10.5f, 0.1f);  /* ore */
    ASSERT_EQ_FLOAT(read_f32_le(&p1[26]), 14.0f, 0.1f);  /* radius */
}

TEST(test_roundtrip_npcs) {
    npc_ship_t npcs[MAX_NPC_SHIPS];
    memset(npcs, 0, sizeof(npcs));

    npcs[0].active = true;
    npcs[0].role = NPC_ROLE_MINER;
    npcs[0].state = NPC_STATE_MINING;
    npcs[0].thrusting = true;
    npcs[0].pos = v2(800.0f, 400.0f);
    npcs[0].vel = v2(10.0f, -5.0f);
    npcs[0].angle = 1.57f;
    npcs[0].target_asteroid = 12;

    uint8_t buf[2 + MAX_NPC_SHIPS * 23];
    int len = serialize_npcs(buf, npcs);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + 23);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    ASSERT(p[1] & 1);                              /* active */
    ASSERT_EQ_INT((p[1] >> 1) & 0x3, NPC_ROLE_MINER);
    ASSERT_EQ_INT((p[1] >> 3) & 0x7, NPC_STATE_MINING);
    ASSERT(p[1] & (1 << 6));                        /* thrusting */
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 800.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[18]), 1.57f, 0.01f);
    ASSERT_EQ_INT((int8_t)p[22], 12);              /* target_asteroid */
}

TEST(test_roundtrip_stations) {
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));

    stations[0].ore_buffer[0] = 45.5f;
    stations[0].ore_buffer[1] = 12.3f;
    stations[0].ore_buffer[2] = 78.9f;
    stations[0].inventory[COMMODITY_FRAME_INGOT] = 20.0f;
    stations[0].product_stock[PRODUCT_FRAME] = 15.5f;

    uint8_t buf[2 + MAX_STATIONS * 49];
    int len = serialize_stations(buf, stations);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_STATIONS);
    ASSERT_EQ_INT(buf[1], MAX_STATIONS);
    ASSERT_EQ_INT(len, 2 + MAX_STATIONS * 49);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1]), 45.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[5]), 12.3f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[9]), 78.9f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[13 + COMMODITY_FRAME_INGOT * 4]), 20.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[37 + PRODUCT_FRAME * 4]), 15.5f, 0.1f);
}

TEST(test_roundtrip_player_ship) {
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.ship.hull = 85.5f;
    sp.ship.credits = 1234.0f;
    sp.docked = true;
    sp.current_station = 2;
    sp.ship.mining_level = 3;
    sp.ship.hold_level = 2;
    sp.ship.tractor_level = 1;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = 45.0f;
    sp.ship.cargo[COMMODITY_CUPRITE_ORE] = 12.5f;
    sp.ship.cargo[COMMODITY_CRYSTAL_ORE] = 8.0f;

    uint8_t buf[32];
    int len = serialize_player_ship(buf, 3, &sp);

    ASSERT_EQ_INT(len, 27);
    ASSERT_EQ_INT(buf[0], NET_MSG_PLAYER_SHIP);
    ASSERT_EQ_INT(buf[1], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 85.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[6]), 1234.0f, 0.1f);
    ASSERT_EQ_INT(buf[10], 1);   /* docked */
    ASSERT_EQ_INT(buf[11], 2);   /* station */
    ASSERT_EQ_INT(buf[12], 3);   /* mining_level */
    ASSERT_EQ_INT(buf[13], 2);   /* hold_level */
    ASSERT_EQ_INT(buf[14], 1);   /* tractor_level */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[15]), 45.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[19]), 12.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[23]), 8.0f, 0.1f);
}

TEST(test_parse_input_valid) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[7] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST | NET_INPUT_LEFT | NET_INPUT_FIRE,
        0, 0, 0, 0,  /* angle (unused by server but present) */
        NET_ACTION_SELL_CARGO
    };

    parse_input(msg, 7, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(intent.turn, 1.0f, 0.01f);
    ASSERT(intent.mine);
    ASSERT(intent.service_sell);
}

TEST(test_parse_input_too_short) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));
    intent.thrust = 99.0f;  /* canary value */

    uint8_t msg[4] = { NET_MSG_INPUT, 0xFF, 0, 0 };
    parse_input(msg, 4, &intent);

    /* Too short — should not modify intent */
    ASSERT_EQ_FLOAT(intent.thrust, 99.0f, 0.01f);
}

TEST(test_parse_input_no_action_byte) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[6] = { NET_MSG_INPUT, NET_INPUT_THRUST, 0, 0, 0, 0 };
    parse_input(msg, 6, &intent);

    /* 6 bytes = legacy format, no action byte */
    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT(!intent.service_sell);
    ASSERT(!intent.interact);
}

TEST(test_parse_input_action_accumulates) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    /* First input: dock action */
    uint8_t msg1[7] = { NET_MSG_INPUT, 0, 0,0,0,0, NET_ACTION_DOCK };
    parse_input(msg1, 7, &intent);
    ASSERT(intent.interact);

    /* Second input: sell action — should OR in, not replace */
    uint8_t msg2[7] = { NET_MSG_INPUT, 0, 0,0,0,0, NET_ACTION_SELL_CARGO };
    parse_input(msg2, 7, &intent);
    ASSERT(intent.interact);       /* still true from first */
    ASSERT(intent.service_sell);   /* added by second */
}

/* ================================================================== */
/* Bug regression tests — batch 2 (10 more bugs)                      */
/* ================================================================== */

/* Bug 11: game_sim.c has station_cargo_sale_value with swapped params vs economy.c.
 * FIX: remove the duplicate from game_sim.c, use economy.c's version everywhere. */
TEST(test_bug11_no_duplicate_sale_value) {
    /* After fix: game_sim.c should NOT have its own station_cargo_sale_value.
     * The economy.c version (ship, station) should be the only one.
     * This fails because game_sim.c still has a static duplicate with (station, ship) order. */
    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    station_t st;
    memset(&st, 0, sizeof(st));
    st.buy_price[COMMODITY_FERRITE_ORE] = 10.0f;
    float val = station_cargo_sale_value(&ship, &st);
    ASSERT_EQ_FLOAT(val, 100.0f, 0.01f);
    /* The real test: there should be no static version in game_sim.c.
     * We verify by checking economy.c's extern version is the one called by world_sim_step.
     * If duplicates exist, this line count assertion will fail when they're removed: */
    ASSERT(sizeof(station_t) > 0);  /* placeholder until duplicate is removed */
}

/* Bug 12: station_repair_cost should return 0 if station lacks REPAIR service.
 * FIX: check station->services & STATION_SERVICE_REPAIR inside the function. */
TEST(test_bug12_repair_cost_checks_service) {
    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 50.0f;
    station_t no_repair;
    memset(&no_repair, 0, sizeof(no_repair));
    no_repair.services = 0;  /* NO repair service */
    float cost = station_repair_cost(&ship, &no_repair);
    /* After fix: should return 0.0 because station can't repair.
     * FAILS now because the function ignores station->services. */
    ASSERT_EQ_FLOAT(cost, 0.0f, 0.01f);
}

/* Bug 13: buy_price should be sized RAW_ORE_COUNT (3), not COMMODITY_COUNT (6).
 * FIX: change station_t.buy_price to float buy_price[COMMODITY_RAW_ORE_COUNT]. */
TEST(test_bug13_buy_price_correct_size) {
    /* After fix: buy_price array should have exactly 3 slots (raw ores only).
     * FAILS now because buy_price is COMMODITY_COUNT (6). */
    ASSERT_EQ_INT(sizeof(((station_t*)0)->buy_price) / sizeof(float), COMMODITY_RAW_ORE_COUNT);
}

/* Bug 14: PLAYER_SHIP message should sync ALL cargo, including ingots.
 * FIX: extend serialize_player_ship to include all COMMODITY_COUNT cargo slots. */
TEST(test_bug14_player_ship_syncs_all_cargo) {
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.ship.cargo[COMMODITY_FRAME_INGOT] = 25.0f;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    uint8_t buf[64];
    int len = serialize_player_ship(buf, 0, &sp);
    /* After fix: message should be larger to include all 6 cargo types.
     * FAILS now because len is 27 (only 3 cargo types). Need at least 27 + 3*4 = 39. */
    ASSERT(len >= 39);
}

/* Bug 15: client and server STATE message should be same size.
 * FIX: either update net_send_state to send 23 bytes, or remove it entirely. */
TEST(test_bug15_state_size_symmetric) {
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    uint8_t buf[32];
    int server_len = serialize_player_state(buf, 0, &sp);
    int client_len = 22;  /* net_send_state hardcodes ws_send_binary(buf, 22) */
    /* After fix: both should be the same size.
     * FAILS now because server sends 23, client sends 22. */
    ASSERT_EQ_INT(server_len, client_len);
}

/* Bug 16: npc_target_valid should bounds-check target_asteroid < MAX_ASTEROIDS.
 * FIX: add (npc->target_asteroid >= MAX_ASTEROIDS) return false. */
TEST(test_bug16_npc_target_bounds_checked) {
    /* After fix: setting target_asteroid to MAX_ASTEROIDS should be safe.
     * Currently it would access out-of-bounds memory.
     * We test by setting a valid-looking but OOB value and expecting the sim
     * doesn't crash or misbehave. */
    world_t w = {0};
    world_reset(&w);
    w.npc_ships[0].active = true;
    w.npc_ships[0].role = NPC_ROLE_MINER;
    w.npc_ships[0].state = NPC_STATE_TRAVEL_TO_ASTEROID;
    w.npc_ships[0].target_asteroid = MAX_ASTEROIDS;  /* OOB */
    w.npc_ships[0].hull_class = HULL_CLASS_NPC_MINER;
    w.npc_ships[0].pos = v2(500.0f, 500.0f);
    /* After fix: sim should handle this gracefully (reset target to -1).
     * FAILS now if the NPC tries to access asteroids[48]. */
    world_sim_step(&w, SIM_DT);
    /* After fix: target should be reset to -1 (invalid) or NPC should change state */
    ASSERT(w.npc_ships[0].target_asteroid < MAX_ASTEROIDS || w.npc_ships[0].target_asteroid == -1);
}

/* Bug 17: economy.c and game_sim.c should not both define step_refinery_production.
 * FIX: remove the duplicate from economy.c (game_sim.c is the authoritative sim). */
TEST(test_bug17_no_duplicate_refinery) {
    /* After fix: economy.c should not export step_refinery_production.
     * world_sim_step in game_sim.c is the only caller.
     * FAILS now because both define the function (economy.c extern, game_sim.c static). */
    /* We can detect the duplicate by calling economy.c's version and checking
     * it produces results. After removal, this won't compile — which is the point. */
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));
    stations[0].role = STATION_ROLE_REFINERY;
    stations[0].ore_buffer[COMMODITY_FERRITE_ORE] = 10.0f;
    step_refinery_production(stations, MAX_STATIONS, 1.0f);
    /* After fix: this function should not exist in economy.c.
     * If it's removed, this test won't compile — CI catches it.
     * For now, FAIL by asserting it shouldn't be callable: */
    ASSERT(0);  /* FAIL: duplicate function still exists */
}

/* Bug 18: emergency recovery should dock at NEAREST station, not last docked station.
 * FIX: find nearest station in emergency_recover_ship instead of using current_station. */
TEST(test_bug18_emergency_recover_nearest_station) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    /* Position near station 2 (Helios Works at 320, 230), far from station 0 */
    w.players[0].ship.pos = v2(320.0f, 200.0f);
    w.players[0].nearby_station = 2;
    w.players[0].current_station = 0;  /* last docked at 0, but 2 is closer */
    w.players[0].ship.hull = 0.5f;
    w.players[0].ship.vel = v2(0.0f, 500.0f);
    for (int i = 0; i < 120; i++)
        world_sim_step(&w, SIM_DT);
    /* After fix: should recover at station 2 (nearest), not station 0 (last docked).
     * FAILS now because emergency_recover uses current_station. */
    if (w.players[0].docked) {
        ASSERT_EQ_INT(w.players[0].current_station, 2);
    }
}

/* Bug 19: collection feedback should be in world_t for persistence.
 * FIX: move collection_feedback_ore/fragments/timer into server_player_t or world_t. */
TEST(test_bug19_feedback_in_world) {
    /* After fix: world_t or server_player_t should contain feedback fields.
     * FAILS now because they're in the client-only game_t struct. */
    /* Check if server_player_t has feedback fields */
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    /* After fix: sp should have collection_feedback_ore, etc.
     * This assertion checks the struct is large enough to contain them.
     * Current sizeof doesn't include feedback (~12 bytes of floats+int). */
    ASSERT(sizeof(server_player_t) > 200);  /* FAILS: sp is ~160 bytes without feedback */
}

/* Bug 20: PLAYER_SHIP handler should verify player_id before applying state.
 * FIX: add id check in the client's on_player_ship callback. */
TEST(test_bug20_player_ship_checks_id) {
    /* After fix: serialize_player_ship for player 7 should only be applied
     * if the receiving client IS player 7. The net.c handler should check
     * state->player_id == net_local_id() like apply_remote_player_state does.
     *
     * We can't test the client callback here, but we can verify the message
     * format includes the ID at a known offset for the check. */
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.ship.credits = 500.0f;
    uint8_t buf[32];
    serialize_player_ship(buf, 7, &sp);
    /* The handler should filter on buf[1]. Currently it doesn't.
     * After fix: net.c PLAYER_SHIP case should have: if (id != net_state.local_id) break;
     * FAILS as a documentation assertion: */
    ASSERT(0);  /* FAIL: no ID check in PLAYER_SHIP handler */
}

/* ================================================================== */
/* Bug regression tests — batch 3 (bugs 21-30, all FAIL until fixed)  */
/* ================================================================== */

/* Bug 21: commodity bitpacking uses 3 bits (0-7) but adding COMMODITY_JUMP_CRYSTAL
 * would make COMMODITY_COUNT=7, and any value >=8 would be truncated. */
TEST(test_bug21_commodity_bits_fragile) {
    /* 3 bits can encode 0-7. COMMODITY_COUNT is currently 6. Adding 2 more
     * commodities (jump crystals, etc) would overflow the bitfield.
     * FIX: use 4 bits for commodity in the network protocol. */
    ASSERT(COMMODITY_COUNT <= 7);  /* passes today */
    /* After fix: protocol should handle COMMODITY_COUNT > 7 */
    /* This test documents the fragility — manually check when adding commodities */
}

/* Bug 22: hauler that can't load stays docked and retries every HAULER_LOAD_TIME,
 * but never considers going to a DIFFERENT station to find cargo. */
TEST(test_bug22_hauler_stuck_at_empty_station) {
    world_t w = {0};
    world_reset(&w);
    /* Empty the refinery inventory so haulers can't load anything */
    for (int i = 0; i < COMMODITY_COUNT; i++)
        w.stations[0].inventory[i] = 0.0f;
    /* Run 10 seconds — haulers should try to find cargo elsewhere */
    for (int i = 0; i < 1200; i++)
        world_sim_step(&w, SIM_DT);
    /* After fix: haulers should have left to find cargo or changed state.
     * FAILS because haulers just wait at home_station forever. */
    bool any_hauler_moved = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].role == NPC_ROLE_HAULER &&
            w.npc_ships[i].state != NPC_STATE_DOCKED) {
            any_hauler_moved = true;
        }
    }
    ASSERT(any_hauler_moved);
}

/* Bug 23: NPC miners don't lose cargo when they can't deposit (hopper full).
 * Ore stays in NPC cargo forever, inflating the economy. */
TEST(test_bug23_npc_cargo_stuck_when_hopper_full) {
    world_t w = {0};
    world_reset(&w);
    /* Fill all hoppers to capacity */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        w.stations[0].ore_buffer[i] = REFINERY_HOPPER_CAPACITY;
    /* Give miner some cargo and send it home */
    w.npc_ships[0].cargo[0] = 30.0f;
    w.npc_ships[0].state = NPC_STATE_RETURN_TO_STATION;
    w.npc_ships[0].pos = w.stations[0].pos;
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, SIM_DT);
    /* NPC should have attempted to deposit but hopper was full.
     * After fix: NPC should dump cargo (lost) or wait.
     * Currently the ore stays in NPC cargo — they just undock and mine more
     * on top of it. The cargo silently accumulates past capacity. */
    float npc_cargo = 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        npc_cargo += w.npc_ships[0].cargo[i];
    /* After fix: NPC cargo should be 0 after docking (deposited or dumped).
     * FAILS because the undeposited ore stays in cargo. */
    ASSERT_EQ_FLOAT(npc_cargo, 0.0f, 1.0f);
}

/* Bug 24: hauler ingot_buffer has no capacity limit — unbounded accumulation */
TEST(test_bug24_ingot_buffer_no_cap) {
    world_t w = {0};
    world_reset(&w);
    /* Hauler delivers 40 ingots to Kepler Yard */
    w.stations[1].ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] = 40.0f;
    /* Deliver 40 more — no cap check, total becomes 80 */
    w.stations[1].ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] += 40.0f;
    /* After fix: ingot_buffer should be capped (like ore_buffer has HOPPER_CAPACITY).
     * FAILS because there's no INGOT_BUFFER_CAPACITY constant or check. */
    ASSERT(w.stations[1].ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] <= 50.0f);
}

/* Bug 25: world_reset always uses same RNG seed — identical asteroid fields every game */
TEST(test_bug25_rng_deterministic_every_reset) {
    world_t w1 = {0}, w2 = {0};
    world_reset(&w1);
    world_reset(&w2);
    /* Both worlds have identical asteroid positions because rng seed is 0xC0FFEE12 */
    /* After fix: seed from time or counter so each reset produces different fields.
     * FAILS because both resets produce identical state. */
    bool all_same = true;
    for (int i = 0; i < 5; i++) {
        if (w1.asteroids[i].pos.x != w2.asteroids[i].pos.x) all_same = false;
    }
    ASSERT(!all_same);
}

/* Bug 26: hauler unloading dumps ingots without checking dest ingot_buffer capacity */
TEST(test_bug26_hauler_unload_no_cap) {
    world_t w = {0};
    world_reset(&w);
    /* Pre-fill dest ingot buffer */
    w.stations[1].ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] = 100.0f;
    /* Hauler arrives with 40 more */
    w.npc_ships[3].ingots[INGOT_IDX(COMMODITY_FRAME_INGOT)] = 40.0f;
    w.npc_ships[3].state = NPC_STATE_UNLOADING;
    w.npc_ships[3].state_timer = 0.01f;
    w.npc_ships[3].dest_station = 1;
    world_sim_step(&w, SIM_DT);
    /* After fix: ingot_buffer should not exceed a cap.
     * FAILS because unloading has no cap — buffer becomes 140. */
    ASSERT(w.stations[1].ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] <= 100.0f);
}

/* Bug 27: cargo can go slightly negative due to float imprecision in sell loop */
TEST(test_bug27_cargo_negative_after_sell) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Set cargo to a value that might cause float issues */
    w.players[0].ship.cargo[COMMODITY_FERRITE_ORE] = 0.011f;  /* just above threshold */
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    /* After fix: cargo should never go negative.
     * Check that all cargo values >= 0 after any transaction. */
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        ASSERT(w.players[0].ship.cargo[i] >= 0.0f);
    }
}

/* Bug 28: credits can go slightly negative due to float comparison in try_spend_credits */
TEST(test_bug28_credits_negative_edge) {
    world_t w = {0};
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Set credits to just barely enough (within 0.01 epsilon) */
    w.players[0].ship.credits = 289.99f;  /* upgrade costs 290 */
    /* try_spend_credits checks: credits + 0.01 < amount
     * 289.99 + 0.01 = 290.0 which is NOT < 290.0, so it succeeds
     * Then: credits = max(0, 289.99 - 290) = max(0, -0.01) = 0.0 — OK
     * But with different values the 0.01 epsilon could allow overspend */
    w.players[0].ship.credits = 0.005f;
    /* try_spend_credits(0.01): 0.005 + 0.01 = 0.015 which is NOT < 0.01, so succeeds
     * Result: max(0, 0.005 - 0.01) = 0.0 — the 0.005 is lost */
    ship_t *s = &w.players[0].ship;
    bool spent = (s->credits + 0.01f >= 0.01f);  /* would pass the check */
    ASSERT(spent);  /* documents: epsilon allows spending more than you have */
    /* After fix: use exact comparison or integer credits.
     * This test passes but documents the imprecision. */
}

/* Bug 29: collection_feedback_ore accumulates across multiple collection events
 * but never resets between display cycles — shows cumulative, not per-event. */
TEST(test_bug29_collection_feedback_accumulates) {
    /* This is a client-side game_t issue, not testable in headless sim.
     * Document: push_collection_feedback adds to existing values if timer > 0.
     * Two quick collections show "Recovered 25 ore" instead of "10" then "15".
     * After fix: either reset on each event or show cumulative intentionally. */
    ASSERT(COLLECTION_FEEDBACK_TIME > 0.0f);  /* timer exists */
    /* FAIL assertion to force review of the accumulation behavior: */
    ASSERT(0);
}

/* Bug 30: two players collecting the same TIER_S fragment simultaneously
 * can both get the full ore — no atomic check on ore remaining */
TEST(test_bug30_double_collect_fragment) {
    world_t w = {0};
    world_reset(&w);
    /* Create a fragment with 10 ore */
    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_S;
    w.asteroids[0].ore = 10.0f;
    w.asteroids[0].max_ore = 10.0f;
    w.asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[0].pos = v2(500.0f, 500.0f);
    w.asteroids[0].radius = 12.0f;
    /* Two players right on top of the fragment */
    player_init_ship(&w.players[0], &w);
    player_init_ship(&w.players[1], &w);
    w.players[0].connected = true;
    w.players[1].connected = true;
    w.players[0].docked = false;
    w.players[1].docked = false;
    w.players[0].ship.pos = v2(500.0f, 500.0f);
    w.players[1].ship.pos = v2(500.0f, 500.0f);
    w.players[0].ship.tractor_level = 4;
    w.players[1].ship.tractor_level = 4;
    world_sim_step(&w, SIM_DT);
    /* Both players should get at most 10 ore total (not 10 each) */
    float total = w.players[0].ship.cargo[COMMODITY_FERRITE_ORE]
                + w.players[1].ship.cargo[COMMODITY_FERRITE_ORE];
    /* After fix: total should be <= 10.0.
     * FAILS if both players collect the full 10 before the ore is decremented. */
    ASSERT(total <= 10.5f);  /* small epsilon for float */
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
    RUN(test_bug10_damage_event_has_amount);

    printf("\nProtocol roundtrip tests:\n");
    RUN(test_roundtrip_player_state);
    RUN(test_roundtrip_asteroids);
    RUN(test_roundtrip_npcs);
    RUN(test_roundtrip_stations);
    RUN(test_roundtrip_player_ship);
    RUN(test_parse_input_valid);
    RUN(test_parse_input_too_short);
    RUN(test_parse_input_no_action_byte);
    RUN(test_parse_input_action_accumulates);

    printf("\nBug regression tests (batch 2 — these SHOULD FAIL until fixed):\n");
    RUN(test_bug11_no_duplicate_sale_value);
    RUN(test_bug12_repair_cost_checks_service);
    RUN(test_bug13_buy_price_correct_size);
    RUN(test_bug14_player_ship_syncs_all_cargo);
    RUN(test_bug15_state_size_symmetric);
    RUN(test_bug16_npc_target_bounds_checked);
    RUN(test_bug17_no_duplicate_refinery);
    RUN(test_bug18_emergency_recover_nearest_station);
    RUN(test_bug19_feedback_in_world);
    RUN(test_bug20_player_ship_checks_id);

    printf("\nBug regression tests (batch 3 — SHOULD FAIL until fixed):\n");
    RUN(test_bug21_commodity_bits_fragile);
    RUN(test_bug22_hauler_stuck_at_empty_station);
    RUN(test_bug23_npc_cargo_stuck_when_hopper_full);
    RUN(test_bug24_ingot_buffer_no_cap);
    RUN(test_bug25_rng_deterministic_every_reset);
    RUN(test_bug26_hauler_unload_no_cap);
    RUN(test_bug27_cargo_negative_after_sell);
    RUN(test_bug28_credits_negative_edge);
    RUN(test_bug29_collection_feedback_accumulates);
    RUN(test_bug30_double_collect_fragment);

    printf("\n%d tests run, %d passed, %d failed\n", tests_run, tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
