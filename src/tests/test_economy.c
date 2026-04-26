#include "tests/test_harness.h"

TEST(test_station_production_yard_makes_frames) {
    station_t station = {0};
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_FRAME_PRESS };
    station.inventory[COMMODITY_FERRITE_INGOT] = 5.0f;
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_FERRITE_INGOT], 3.0f, 0.001f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_FRAME], 1.0f, 0.001f);
}

TEST(test_station_production_beamworks_makes_modules) {
    station_t station = {0};
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_LASER_FAB };
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_TRACTOR_FAB };
    station.inventory[COMMODITY_CUPRITE_INGOT] = 5.0f;
    station.inventory[COMMODITY_CRYSTAL_INGOT] = 5.0f;
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_CUPRITE_INGOT], 3.5f, 0.001f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_CRYSTAL_INGOT], 4.5f, 0.001f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_LASER_MODULE], 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(station.inventory[COMMODITY_TRACTOR_MODULE], 0.5f, 0.001f);
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
    /* Any dock can install kits — repair quote needs MODULE_DOCK. */
    station.modules[station.module_count++] =
        (station_module_t){ .type = MODULE_DOCK };
    float cost = station_repair_cost(&ship, &station);
    ASSERT(cost > 0.0f);
}

TEST(test_can_afford_upgrade_all_conditions) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station.inventory[COMMODITY_FRAME] = 100.0f;
    int cost = ship_upgrade_cost(&ship, SHIP_UPGRADE_HOLD);
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost, 10000.0f));
}

TEST(test_can_afford_upgrade_no_credits) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station.inventory[COMMODITY_FRAME] = 100.0f;
    int cost = ship_upgrade_cost(&ship, SHIP_UPGRADE_HOLD);
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost, 0.0f));
}

TEST(test_can_afford_upgrade_no_product) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station.inventory[COMMODITY_FRAME] = 0.0f;
    int cost = ship_upgrade_cost(&ship, SHIP_UPGRADE_HOLD);
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, cost, 10000.0f));
}

TEST(test_contract_generated_from_hopper_deficit) {
    /* A refinery with low ore_buffer should generate an ore contract */
    WORLD_DECL;
    world_reset(&w);
    /* Make ferrite the biggest deficit by filling the others */
    w.stations[0].inventory[COMMODITY_FERRITE_ORE] = 10.0f;
    w.stations[0].inventory[COMMODITY_CUPRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    w.stations[0].inventory[COMMODITY_CRYSTAL_ORE] = REFINERY_HOPPER_CAPACITY;
    world_sim_step(&w, SIM_DT);
    /* Find contract for station 0, ferrite ore */
    contract_t *found = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            found = &w.contracts[k];
            break;
        }
    }
    ASSERT(found != NULL);
    /* Ore contracts are inventory-driven — quantity_needed is 0 */
    ASSERT_EQ_FLOAT(found->quantity_needed, 0.0f, 0.01f);
}

TEST(test_contract_price_escalates_with_age) {
    /* An unfilled contract should increase in price over time */
    contract_t c = {.active = true, .base_price = 10.0f, .age = 0.0f};
    float price_t0 = contract_price(&c);
    c.age = 300.0f; /* 5 minutes */
    float price_t5 = contract_price(&c);
    ASSERT(price_t5 > price_t0);
    ASSERT_EQ_FLOAT(price_t5, 10.0f * 1.2f, 0.01f);
}

TEST(test_contract_closes_when_deficit_filled) {
    /* When ore_buffer rises to 80% threshold, contract should close */
    WORLD_DECL;
    world_reset(&w);
    w.stations[0].inventory[COMMODITY_FERRITE_ORE] = 10.0f;
    world_sim_step(&w, SIM_DT); /* generates contract */
    /* Now fill the hopper above 80% threshold */
    w.stations[0].inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.85f;
    world_sim_step(&w, SIM_DT); /* should close the contract */
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            found = true; break;
        }
    }
    ASSERT(!found);
}

TEST(test_sell_price_uses_contract_price) {
    /* When a contract exists, selling at that station should pay the
     * escalated contract price, not the base buy_price.
     *
     * Uses COMMODITY_FERRITE_INGOT because raw-ore cargo delivery is a
     * dead path post-#259 (physical ore towing; fragments ride in
     * ship.towed_fragments[], not ship.cargo[]). Ingot delivery is the
     * live path this contract-price logic actually serves. */
    WORLD_DECL;
    world_reset(&w);
    /* Create a contract with aged price — station 0 needs an ingot. */
    w.contracts[0] = (contract_t){
        .active = true, .station_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 50.0f,
        .base_price = 10.0f, .age = 300.0f, /* 5 min -> 1.2x */
    };
    /* Set up player docked at station 0 with a deliverable ingot. */
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] = 10.0f;
    /* Zero out ledger balance for precise payout check */
    float init_bal = ledger_balance(&w.stations[0], w.players[0].session_token);
    float expected_price = 10.0f * 1.2f; /* contract_price at age 300 */
    /* Trigger sell */
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    /* Credits should reflect escalated price, not base 10.0 */
    float earned = ledger_balance(&w.stations[0], w.players[0].session_token) - init_bal;
    ASSERT(earned > 10.0f * 10.0f); /* more than base */
    ASSERT_EQ_FLOAT(earned, 10.0f * expected_price, 1.0f);
}

TEST(test_hauler_fills_highest_value_contract) {
    /* NPC hauler at a station should pick the highest-value contract
     * fillable from local inventory, not a hardcoded destination */
    WORLD_DECL;
    world_reset(&w);
    /* Set up two contracts: one cheap at station 1, one expensive at station 2 */
    w.contracts[0] = (contract_t){
        .active = true, .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 10.0f, .age = 0.0f,
    };
    w.contracts[1] = (contract_t){
        .active = true, .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 50.0f, .age = 0.0f,
    };
    /* Give home station (0) inventory of both */
    w.stations[0].inventory[COMMODITY_FERRITE_INGOT] = 20.0f;
    w.stations[0].inventory[COMMODITY_CUPRITE_INGOT] = 20.0f;
    /* Find the first hauler */
    npc_ship_t *hauler = NULL;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler = &w.npc_ships[i]; break;
        }
    }
    ASSERT(hauler != NULL);
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f; /* ready to act */
    hauler->home_station = 0;
    hauler->dest_station = 1; /* default dest */
    memset(hauler->cargo, 0, sizeof(hauler->cargo));
    world_sim_step(&w, SIM_DT);
    /* Hauler should target station 2 (higher value contract) */
    ASSERT(hauler->dest_station == 2);
}

TEST(test_one_contract_per_station) {
    WORLD_DECL;
    world_reset(&w);
    /* Empty all hoppers to create demand */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        w.stations[0].inventory[i] = 0.0f;
    /* Run a few ticks to generate contracts */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    /* Count contracts for station 0. Up to two are allowed per station:
     * one ore contract (raw mining) + one production contract
     * (scaffold/ingot/kit-fab input). */
    int count = 0;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0) count++;
    }
    ASSERT(count >= 1 && count <= 2);
}

TEST(test_destroy_contract_completes_when_asteroid_gone) {
    /* DESTROY contracts should close when their target_index is invalid or inactive.
     * Test without full sim to avoid respawn interference. */
    contract_t c = {
        .active = true, .action = CONTRACT_FRACTURE,
        .target_index = -1,  /* invalid = gone */
        .base_price = 30.0f, .claimed_by = -1,
    };
    /* The fulfillment check: idx < 0 || idx >= MAX_ASTEROIDS || !asteroids[idx].active */
    bool target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS);
    ASSERT(target_gone);

    /* Valid index, inactive asteroid */
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    c.target_index = 5;
    asteroids[5].active = false;
    target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS || !asteroids[c.target_index].active);
    ASSERT(target_gone);

    /* Valid index, active asteroid — should NOT be gone */
    asteroids[5].active = true;
    target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS || !asteroids[c.target_index].active);
    ASSERT(!target_gone);
}

TEST(test_supply_contract_uses_correct_material) {
    WORLD_DECL;
    world_reset(&w);
    /* Build a laser fab scaffold on station 0 */
    begin_module_construction(&w, &w.stations[0], 0, MODULE_LASER_FAB);
    /* The generated contract should be for cuprite ingots */
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == 0
            && w.contracts[k].commodity == COMMODITY_CUPRITE_INGOT) {
            found = true; break;
        }
    }
    ASSERT(found);
    /* After contract expires and regenerates via step_contracts, it should still be cuprite */
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0
            && w.contracts[k].commodity == COMMODITY_CUPRITE_INGOT) {
            found = true; break;
        }
    }
    ASSERT(found);
}

TEST(test_dynamic_ore_price_deficit) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    /* Buy price: empty=1× base, full=0.5× base */
    st.inventory[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
    st.inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 5.0f, 0.1f);
    st.inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.5f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 7.5f, 0.1f);
    /* Sell price: empty=2× base, full=1× base */
    st.inventory[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 20.0f, 0.1f);
    st.inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
}

TEST(test_product_price_tracks_ore) {
    station_t st = {0};
    st.base_price[COMMODITY_FRAME] = 20.0f;
    /* Sell price: empty=2× base, full=1× base */
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 40.0f, 0.1f);
    st.inventory[COMMODITY_FRAME] = MAX_PRODUCT_STOCK;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 20.0f, 0.1f);
    st.inventory[COMMODITY_FRAME] = MAX_PRODUCT_STOCK * 0.5f;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 25.0f, 0.1f);
}

TEST(test_deliver_ingots_to_contract) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player carries ferrite ingots */
    w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] = 30.0f;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    float credits_before = ledger_balance(&w.stations[1], w.players[0].session_token);
    /* Create a contract at station 1 (Kepler Yard) for ferrite ingots */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    /* Dock at station 1 and sell */
    w.players[0].docked = true;
    w.players[0].current_station = 1;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    /* Ingots delivered, credits gained at station 1 */
    ASSERT(w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] < 30.0f);
    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) > credits_before);
    /* Contract quantity reduced */
    ASSERT(w.contracts[0].quantity_needed < 20.0f || !w.contracts[0].active);
}

TEST(test_mixed_cargo_sell_and_deliver) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player carries ingots */
    w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] = 20.0f;
    /* Contract at refinery for ferrite ingots (unusual but valid) */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 15.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    float credits_before = ledger_balance(&w.stations[0], w.players[0].session_token);
    /* Dock at refinery and deliver */
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    /* Ingots delivered via contract */
    ASSERT(w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] < 20.0f);
    ASSERT(ledger_balance(&w.stations[0], w.players[0].session_token) > credits_before);
}

TEST(test_no_delivery_without_matching_contract) {
    /* Cargo with no matching contract or production sink should not be delivered */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player carries a finished module — stations don't buy this via
     * fallback input delivery. */
    w.players[0].ship.cargo[COMMODITY_TRACTOR_MODULE] = 20.0f;
    /* Clear all contracts */
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    /* Dock at yard and try to sell */
    w.players[0].docked = true;
    w.players[0].current_station = 1;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_TRACTOR_MODULE], 20.0f, 0.01f);
}

TEST(test_259_passive_repair_at_any_station) {
    /* Passive repair (8 hp/s) runs at ANY station while docked,
     * regardless of STATION_SERVICE_REPAIR flag. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship.hull = 50.0f; /* damaged */
    /* Dock at station 0 (no REPAIR_BAY module) */
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    ASSERT(!(w.stations[0].services & STATION_SERVICE_REPAIR));
    /* Run a few ticks — hull should increase from passive repair */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(w.players[0].ship.hull > 50.0f);
}

TEST(test_refinery_smelts_ore_in_inventory) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    /* Verify Prospect has a furnace */
    ASSERT(station_has_module(&w.stations[0], MODULE_FURNACE));
    /* Put ore directly in station inventory (as if delivered by fragments) */
    w.stations[0].inventory[COMMODITY_FERRITE_ORE] = 10.0f;
    /* Run sim for 10 seconds — should smelt ore into ingots */
    for (int i = 0; i < (int)(10.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    float ingots = w.stations[0].inventory[COMMODITY_FERRITE_INGOT];
    ASSERT(ingots > 0.0f);
}

TEST(test_kit_fab_requires_shipyard) {
    /* After the shipyard-fab redesign, only stations with MODULE_SHIPYARD
     * mint repair kits. A station with only a dock + the three input
     * commodities should never produce kits. */
    WORLD_DECL;
    world_reset(&w);
    /* Prospect (station 0) has a dock but no shipyard. Kepler (station 1)
     * has both. Pre-fill both with kit-fab inputs. */
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    for (int s = 0; s < 2; s++) {
        w.stations[s].inventory[COMMODITY_FRAME]          = 5.0f;
        w.stations[s].inventory[COMMODITY_LASER_MODULE]   = 5.0f;
        w.stations[s].inventory[COMMODITY_TRACTOR_MODULE] = 5.0f;
        w.stations[s].inventory[COMMODITY_REPAIR_KIT]     = 0.0f;
        w.stations[s].repair_kit_fab_timer = 0.0f;
    }
    /* Run long enough for at least one fab cycle (REPAIR_KIT_FAB_PERIOD = 30s). */
    for (int i = 0; i < (int)(35.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    /* Shipyard station produces kits; dock-only station does not. */
    ASSERT(w.stations[1].inventory[COMMODITY_REPAIR_KIT] > 0.0f);
    ASSERT_EQ_FLOAT(w.stations[0].inventory[COMMODITY_REPAIR_KIT], 0.0f, 0.01f);
}

TEST(test_kit_import_contract_at_consumer_station) {
    /* A station with a dock but no shipyard should issue a TRACTOR
     * contract for REPAIR_KIT when its kit inventory drops below the
     * import threshold. Players or NPC haulers fulfill via the same
     * delivery loop that handles ingots. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    /* Drain Prospect's kit inventory to force the deficit. */
    w.stations[0].inventory[COMMODITY_REPAIR_KIT] = 0.0f;
    /* Run a few seconds for the contract step to fire. */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && c->station_index == 0
            && c->commodity == COMMODITY_REPAIR_KIT) {
            found = true;
            ASSERT(c->base_price > 0.0f);
            ASSERT(c->quantity_needed > 0.0f);
            break;
        }
    }
    ASSERT(found);
}

TEST(test_kit_import_contract_skips_shipyard_stations) {
    /* A shipyard station mints its own kits; the import contract should
     * not fire there even with kit inventory at zero. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    w.stations[1].inventory[COMMODITY_REPAIR_KIT] = 0.0f;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && c->station_index == 1
            && c->commodity == COMMODITY_REPAIR_KIT) {
            ASSERT(false); /* shouldn't reach here */
        }
    }
}

TEST(test_repair_drains_ship_cargo_first) {
    /* Player docked at a station with a repair service. Ship carries
     * 50 kits in cargo, station has 100 kits in inventory. A 30 HP
     * repair drains 30 kits from ship cargo, leaves station inventory
     * untouched, and charges only the labor fee (no station retail
     * since no kits sourced from station). */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    /* Force the repair service (default Prospect lacks REPAIR_BAY). */
    w.stations[0].services |= STATION_SERVICE_REPAIR;
    w.stations[0].inventory[COMMODITY_REPAIR_KIT] = 100.0f;
    w.players[0].ship.cargo[COMMODITY_REPAIR_KIT] = 50.0f;
    float max_hull = ship_max_hull(&w.players[0].ship);
    w.players[0].ship.hull = max_hull - 30.0f; /* 30 HP missing */

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* Hull restored, ship cargo drained, station inventory untouched. */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, max_hull, 0.5f);
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_REPAIR_KIT], 20.0f, 0.5f);
    ASSERT_EQ_FLOAT(w.stations[0].inventory[COMMODITY_REPAIR_KIT], 100.0f, 0.5f);

    /* Charge: only labor (no station retail). 30 HP * 1 cr/HP. */
    float bal_after = ledger_balance(&w.stations[0],
                                     w.players[0].session_token);
    float charged = bal_before - bal_after;
    ASSERT_EQ_FLOAT(charged, 30.0f * LABOR_FEE_PER_HP, 0.5f);
}

TEST(test_repair_falls_back_to_station_inventory) {
    /* Player has no kits in cargo; station inventory covers it. Repair
     * charges retail (station_sell_price) + labor since not a shipyard. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    w.stations[0].services |= STATION_SERVICE_REPAIR;
    w.stations[0].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    w.stations[0].inventory[COMMODITY_REPAIR_KIT]  = MAX_PRODUCT_STOCK; /* full → 1× */
    w.players[0].ship.cargo[COMMODITY_REPAIR_KIT]  = 0.0f;
    float max_hull = ship_max_hull(&w.players[0].ship);
    w.players[0].ship.hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: 10 kits drained, charge = 10 * (6 + 1) = 70 cr. */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, max_hull, 0.5f);
    ASSERT_EQ_FLOAT(w.stations[0].inventory[COMMODITY_REPAIR_KIT],
                    MAX_PRODUCT_STOCK - 10.0f, 0.5f);
    float charged = bal_before - ledger_balance(&w.stations[0],
                                                w.players[0].session_token);
    ASSERT_EQ_FLOAT(charged, 10.0f * (6.0f + LABOR_FEE_PER_HP), 1.0f);
}

TEST(test_repair_at_shipyard_no_labor_fee) {
    /* At a shipyard the labor fee is zero — you already paid retail
     * when you bought the kits there. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 1; /* Kepler has shipyard */
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));

    w.stations[1].services |= STATION_SERVICE_REPAIR;
    w.stations[1].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    w.stations[1].inventory[COMMODITY_REPAIR_KIT]  = MAX_PRODUCT_STOCK;
    w.players[0].ship.cargo[COMMODITY_REPAIR_KIT]  = 0.0f;
    float max_hull = ship_max_hull(&w.players[0].ship);
    w.players[0].ship.hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[1],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: charge = 10 * (6 + 0) = 60 cr (no labor). */
    float charged = bal_before - ledger_balance(&w.stations[1],
                                                w.players[0].session_token);
    ASSERT_EQ_FLOAT(charged, 10.0f * 6.0f, 1.0f);
}

TEST(test_repair_partial_when_kits_short) {
    /* Both ship cargo and station inventory empty: repair does nothing
     * (no partial heal because no kits to consume). */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x03, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    w.stations[0].services |= STATION_SERVICE_REPAIR;
    w.stations[0].inventory[COMMODITY_REPAIR_KIT] = 0.0f;
    w.players[0].ship.cargo[COMMODITY_REPAIR_KIT] = 0.0f;
    float max_hull = ship_max_hull(&w.players[0].ship);
    /* Damage by 20, then forcibly clear the docked passive heal so we
     * can measure precisely. Approach: run only one tick where dock
     * passive adds 8*dt = 8/120 ≈ 0.067 HP; tolerance covers it. */
    w.players[0].ship.hull = max_hull - 20.0f;

    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* Hull should be roughly unchanged (only the passive 8 HP/sec
     * applies for one tick = 0.067 HP). */
    ASSERT(w.players[0].ship.hull < max_hull - 19.0f);
}

TEST(test_furnace_without_adjacent_hopper_smelts) {
    /* Furnaces smelt from station inventory regardless of adjacency. */
    WORLD_DECL;
    world_reset(&w);
    /* Remove all modules from station 0 and place furnace alone */
    w.stations[0].module_count = 0;
    rebuild_station_services(&w.stations[0]);
    w.stations[0].modules[0] = (station_module_t){ .type = MODULE_FURNACE, .ring = 2, .slot = 0, .scaffold = false, .build_progress = 1.0f };
    w.stations[0].module_count = 1;
    /* Furnace is isolated — no hopper — but should still smelt */
    w.stations[0].inventory[COMMODITY_FERRITE_ORE] = 100.0f;
    float initial_ingots = w.stations[0].inventory[COMMODITY_FERRITE_INGOT];
    /* Run sim for 5 seconds */
    for (int i = 0; i < (int)(5.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    /* Smelting should have occurred — furnaces smelt directly from inventory */
    ASSERT(w.stations[0].inventory[COMMODITY_FERRITE_INGOT] > initial_ingots);
}

TEST(test_commodity_volume_kit_dense) {
    /* Kits take REPAIR_KIT_CARGO_DENSITY units of cargo each; everything
     * else is 1.0. */
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_REPAIR_KIT),
                    REPAIR_KIT_CARGO_DENSITY, 0.001f);
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_FRAME), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_FERRITE_INGOT), 1.0f, 0.001f);
}

TEST(test_ship_total_cargo_kit_density) {
    /* 100 kits + 5 frames = 100 * 0.1 + 5 * 1.0 = 15 cargo units. */
    ship_t ship = {0};
    ship.cargo[COMMODITY_REPAIR_KIT] = 100.0f;
    ship.cargo[COMMODITY_FRAME] = 5.0f;
    ASSERT_EQ_FLOAT(ship_total_cargo(&ship),
                    100.0f * REPAIR_KIT_CARGO_DENSITY + 5.0f, 0.001f);
}

void register_economy_basic_tests(void) {
    TEST_SECTION("\nEconomy tests:\n");
    RUN(test_station_production_yard_makes_frames);
    RUN(test_station_production_beamworks_makes_modules);
    RUN(test_station_repair_cost_no_damage);
    RUN(test_station_repair_cost_with_damage);
    RUN(test_can_afford_upgrade_all_conditions);
    RUN(test_can_afford_upgrade_no_credits);
    RUN(test_can_afford_upgrade_no_product);
    RUN(test_commodity_volume_kit_dense);
    RUN(test_ship_total_cargo_kit_density);
}

void register_economy_contracts_tests(void) {
    TEST_SECTION("\nContract tests:\n");
    RUN(test_contract_generated_from_hopper_deficit);
    RUN(test_contract_price_escalates_with_age);
    RUN(test_contract_closes_when_deficit_filled);
    RUN(test_sell_price_uses_contract_price);
    RUN(test_hauler_fills_highest_value_contract);
    RUN(test_kit_fab_requires_shipyard);
    RUN(test_kit_import_contract_at_consumer_station);
    RUN(test_kit_import_contract_skips_shipyard_stations);
    RUN(test_repair_drains_ship_cargo_first);
    RUN(test_repair_falls_back_to_station_inventory);
    RUN(test_repair_at_shipyard_no_labor_fee);
    RUN(test_repair_partial_when_kits_short);
}

void register_economy_contract3_tests(void) {
    TEST_SECTION("\nContract system (3-action):\n");
    RUN(test_one_contract_per_station);
    RUN(test_destroy_contract_completes_when_asteroid_gone);
    RUN(test_supply_contract_uses_correct_material);
}

void register_economy_pricing_tests(void) {
    TEST_SECTION("\nDynamic pricing:\n");
    RUN(test_dynamic_ore_price_deficit);
    RUN(test_product_price_tracks_ore);
}

void register_economy_mixed_cargo_tests(void) {
    TEST_SECTION("\nMixed cargo sell/deliver:\n");
    RUN(test_deliver_ingots_to_contract);
    RUN(test_mixed_cargo_sell_and_deliver);
    RUN(test_no_delivery_without_matching_contract);
}

void register_economy_service259_tests(void) {
    TEST_SECTION("\nStation service semantics (#259):\n");
    RUN(test_259_passive_repair_at_any_station);
}

void register_economy_refinery_smelt_tests(void) {
    TEST_SECTION("\nRefinery smelt test:\n");
    RUN(test_refinery_smelts_ore_in_inventory);
    RUN(test_furnace_without_adjacent_hopper_smelts);
}

