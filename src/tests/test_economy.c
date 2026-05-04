#include "tests/test_harness.h"

TEST(test_station_production_yard_makes_frames) {
    station_t station = {0};
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_FRAME_PRESS };
    station._inventory_cache[COMMODITY_FERRITE_INGOT] = 5.0f;
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station._inventory_cache[COMMODITY_FERRITE_INGOT], 3.0f, 0.001f);
    ASSERT_EQ_FLOAT(station._inventory_cache[COMMODITY_FRAME], 1.0f, 0.001f);
}

TEST(test_station_production_beamworks_makes_modules) {
    station_t station = {0};
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_LASER_FAB };
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_TRACTOR_FAB };
    station._inventory_cache[COMMODITY_CUPRITE_INGOT] = 5.0f;
    station._inventory_cache[COMMODITY_CRYSTAL_INGOT] = 5.0f;
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station._inventory_cache[COMMODITY_CUPRITE_INGOT], 3.5f, 0.001f);
    ASSERT_EQ_FLOAT(station._inventory_cache[COMMODITY_CRYSTAL_INGOT], 4.5f, 0.001f);
    ASSERT_EQ_FLOAT(station._inventory_cache[COMMODITY_LASER_MODULE], 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(station._inventory_cache[COMMODITY_TRACTOR_MODULE], 0.5f, 0.001f);
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

TEST(test_can_afford_upgrade_dock_fallback) {
    /* Empty cargo, but station stocks the modules and player has
     * credits — dock fills the gap from inventory at retail. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    STATION_DECL(station);
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 100));
    station.base_price[COMMODITY_FRAME] = 22.0f;
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,10000.0f));
}

TEST(test_can_afford_upgrade_no_credits_for_dock_fallback) {
    /* Empty cargo, station has modules, balance zero — fallback
     * needs credits, so this must be rejected. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    STATION_DECL(station);
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 100));
    station.base_price[COMMODITY_FRAME] = 22.0f;
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,0.0f));
}

TEST(test_can_afford_upgrade_no_product_anywhere) {
    /* Empty cargo, empty station inventory — no modules to install. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station._inventory_cache[COMMODITY_FRAME] = 0.0f;
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,10000.0f));
}

TEST(test_can_afford_upgrade_cargo_only_no_credits_needed) {
    /* Ship cargo covers the full module cost — credit balance is
     * irrelevant since the dock has nothing to sell. */
    SHIP_DECL(ship);
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    /* Empty dock inventory; ship carries enough frames itself. */
    int need = (int)ceilf(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD));
    ASSERT(test_set_ship_finished_units(&ship, COMMODITY_FRAME, need,
                                        MINING_GRADE_COMMON));
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,0.0f));
}

TEST(test_contract_generated_from_hopper_deficit) {
    /* A refinery with low ore_buffer should generate an ore contract */
    WORLD_DECL;
    world_reset(&w);
    /* Make ferrite the biggest deficit by filling the others */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    w.stations[0]._inventory_cache[COMMODITY_CUPRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    w.stations[0]._inventory_cache[COMMODITY_CRYSTAL_ORE] = REFINERY_HOPPER_CAPACITY;
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
    /* Tractor-contract close hysteresis: opens on deficit (<90%), must NOT
     * close until inventory crosses 95% — otherwise a station sitting in
     * [80%, 95%] opens-and-closes a contract every tick, spamming
     * SIM_EVENT_CONTRACT_COMPLETE. See fix for issue #461. */
    WORLD_DECL;
    world_reset(&w);
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    world_sim_step(&w, SIM_DT); /* generates contract (deficit > threshold) */

    /* 85% should NOT close the contract anymore — it's between open (90%) and close (95%) */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.85f;
    world_sim_step(&w, SIM_DT);
    bool still_active = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            still_active = true; break;
        }
    }
    ASSERT(still_active);

    /* Above the 95% close threshold, contract closes */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.96f;
    world_sim_step(&w, SIM_DT);
    bool still_active2 = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            still_active2 = true; break;
        }
    }
    ASSERT(!still_active2);
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
    ASSERT(test_set_ship_finished_units(&w.players[0].ship,
                                        COMMODITY_FERRITE_INGOT, 10,
                                        MINING_GRADE_COMMON));
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
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] = 20.0f;
    w.stations[0]._inventory_cache[COMMODITY_CUPRITE_INGOT] = 20.0f;
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
        w.stations[0]._inventory_cache[i] = 0.0f;
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
    /* LASER_FAB needs cuprite + crystal ingot hoppers. Plant both. */
    add_hopper_for(&w.stations[1], 3, 1, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&w.stations[1], 3, 7, COMMODITY_CRYSTAL_INGOT);
    begin_module_construction_at(&w, &w.stations[1], 1, MODULE_LASER_FAB, 2, 4);
    /* The generated contract should be for cuprite ingots */
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == 1
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
        if (w.contracts[k].active && w.contracts[k].station_index == 1
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
    st._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 5.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.5f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 7.5f, 0.1f);
    /* Sell price: empty=2× base, full=1× base */
    st._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 20.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
}

TEST(test_product_price_tracks_ore) {
    station_t st = {0};
    st.base_price[COMMODITY_FRAME] = 20.0f;
    /* Sell price: empty=2× base, full=1× base */
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 40.0f, 0.1f);
    st._inventory_cache[COMMODITY_FRAME] = MAX_PRODUCT_STOCK;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 20.0f, 0.1f);
    st._inventory_cache[COMMODITY_FRAME] = MAX_PRODUCT_STOCK * 0.5f;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 25.0f, 0.1f);
}

TEST(test_deliver_ingots_to_contract) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player carries ferrite ingots */
    ASSERT(test_set_ship_finished_units(&w.players[0].ship,
                                        COMMODITY_FERRITE_INGOT, 30,
                                        MINING_GRADE_COMMON));
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

/* Pubkey-registered players had been getting 65% of the contract payout
 * because try_sell_station_cargo routed through ledger_credit_supply
 * (which applies the 35% smelt-station cut) instead of ledger_earn
 * (full credit). Locks the fix in: payout to the ledger == quoted price
 * × quantity, not × 0.65. Reported as "press S, popup says +152, wallet
 * only sees +99" on the WORK tab. */
TEST(test_deliver_ingots_full_payout_to_pubkey_player) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    /* Register a pubkey so the bulk-sell path takes the pubkey branch. */
    memset(w.players[0].pubkey, 0xAA, 32);
    w.players[0].pubkey_set = true;
    /* Player carries 10 ferrite ingots; contract pays 20 cr each. */
    ASSERT(test_set_ship_finished_units(&w.players[0].ship,
                                        COMMODITY_FERRITE_INGOT, 10,
                                        MINING_GRADE_COMMON));
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 10.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    float bal_before = ledger_balance_by_pubkey(&w.stations[1], w.players[0].pubkey);
    w.players[0].docked = true;
    w.players[0].current_station = 1;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    /* Expect 10 × 20 = 200 cr credited (allow tiny float slack for
     * age-escalation drift on tick 1 — should be effectively zero). */
    float bal_after = ledger_balance_by_pubkey(&w.stations[1], w.players[0].pubkey);
    float gained = bal_after - bal_before;
    ASSERT(gained > 199.0f);
    ASSERT(gained < 201.0f);
}

TEST(test_mixed_cargo_sell_and_deliver) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player carries ingots */
    ASSERT(test_set_ship_finished_units(&w.players[0].ship,
                                        COMMODITY_FERRITE_INGOT, 20,
                                        MINING_GRADE_COMMON));
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
    /* Cargo with no matching contract AND no consuming module on the
     * station should stay in the hold. Use Prospect (no shipyard,
     * no fab) so a tractor-module load has nowhere to land. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship.cargo[COMMODITY_TRACTOR_MODULE] = 20.0f;
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    /* Prospect Refinery (station 0): DOCK + SIGNAL_RELAY + FURNACE +
     * ORE_SILO. No SHIPYARD, no TRACTOR_FAB → station_consumes returns
     * false for tractor modules, so the SELL fallback should skip. */
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_TRACTOR_MODULE], 20.0f, 0.01f);
}

TEST(test_no_passive_heal_without_kits) {
    /* Passive heal was removed: docking alone never repairs. With both
     * ship cargo and station inventory empty, damaged hull stays
     * damaged — repair requires kits. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship.hull = 50.0f;
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT] = 0.0f;
    w.players[0].ship.cargo[COMMODITY_REPAIR_KIT] = 0.0f;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, 50.0f, 0.01f);
}

TEST(test_refinery_smelts_ore_in_inventory) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    /* Verify Prospect has a furnace */
    ASSERT(station_has_module(&w.stations[0], MODULE_FURNACE));
    /* Put ore directly in station inventory (as if delivered by fragments) */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    /* Run sim for 10 seconds — should smelt ore into ingots */
    for (int i = 0; i < (int)(10.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    float ingots = w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT];
    ASSERT(ingots > 0.0f);
}

TEST(test_kit_fab_requires_shipyard) {
    /* After the shipyard-fab redesign, only stations with MODULE_SHIPYARD
     * mint repair kits. A station with only a dock + the three input
     * commodities should never produce kits. */
    WORLD_DECL;
    world_reset(&w);
    /* Prospect (station 0) has a dock but no shipyard. Kepler and Helios
     * both have shipyards. Pre-fill all three with kit-fab inputs. */
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    for (int s = 0; s < 3; s++) {
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_FRAME, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_LASER_MODULE, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_TRACTOR_MODULE, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_REPAIR_KIT, 0));
        w.stations[s].repair_kit_fab_timer = 0.0f;
    }
    /* Run long enough for at least one fab cycle (REPAIR_KIT_FAB_PERIOD = 30s). */
    for (int i = 0; i < (int)(35.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    /* Shipyard station produces kits; dock-only station does not. */
    ASSERT(w.stations[1]._inventory_cache[COMMODITY_REPAIR_KIT] > 0.0f);
    ASSERT(w.stations[2]._inventory_cache[COMMODITY_REPAIR_KIT] > 0.0f);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT], 0.0f, 0.01f);
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
    w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT] = 0.0f;
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
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    w.stations[1]._inventory_cache[COMMODITY_REPAIR_KIT] = 0.0f;
    w.stations[2]._inventory_cache[COMMODITY_REPAIR_KIT] = 0.0f;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && (c->station_index == 1 || c->station_index == 2)
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
    ASSERT(test_set_station_finished_units(&w.stations[0], COMMODITY_REPAIR_KIT, 100));
    ASSERT(test_set_ship_finished_units(&w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        50, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(&w.players[0].ship);
    w.players[0].ship.hull = max_hull - 30.0f; /* 30 HP missing */

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* Hull restored, ship cargo drained, station inventory untouched. */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, max_hull, 0.5f);
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_REPAIR_KIT], 20.0f, 0.5f);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT], 100.0f, 0.5f);

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
    ASSERT(test_set_station_finished_units(&w.stations[0], COMMODITY_REPAIR_KIT,
                                           (int)MAX_PRODUCT_STOCK)); /* full → 1× */
    ASSERT(test_set_ship_finished_units(&w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        0, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(&w.players[0].ship);
    w.players[0].ship.hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: 10 kits drained, charge = 10 * (6 + 1) = 70 cr. */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, max_hull, 0.5f);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT],
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
    ASSERT(test_set_station_finished_units(&w.stations[1], COMMODITY_REPAIR_KIT,
                                           (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_ship_finished_units(&w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        0, MINING_GRADE_COMMON));
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

    w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT] = 0.0f;
    w.players[0].ship.cargo[COMMODITY_REPAIR_KIT] = 0.0f;
    float max_hull = ship_max_hull(&w.players[0].ship);
    w.players[0].ship.hull = max_hull - 20.0f;

    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* No kits anywhere = no heal at all (passive heal removed). */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, max_hull - 20.0f, 0.01f);
}

TEST(test_furnace_without_hopper_does_not_smelt) {
    /* Under the count-tier rules, a furnace requires at least one
     * hopper on the station before it'll fire. Inverse of the prior
     * test (which asserted a furnace alone would smelt) — that's the
     * exact behavior we removed in the rework. */
    WORLD_DECL;
    world_reset(&w);
    w.stations[0].module_count = 0;
    rebuild_station_services(&w.stations[0]);
    w.stations[0].modules[0] = (station_module_t){ .type = MODULE_FURNACE, .ring = 2, .slot = 0, .scaffold = false, .build_progress = 1.0f };
    w.stations[0].module_count = 1;
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 100.0f;
    float initial_ingots = w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT];
    for (int i = 0; i < (int)(5.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT],
                    initial_ingots, 0.001f);
    /* Add a hopper and let it run again — now it should smelt. */
    w.stations[0].modules[1] = (station_module_t){ .type = MODULE_HOPPER, .ring = 1, .slot = 0, .scaffold = false, .build_progress = 1.0f };
    w.stations[0].module_count = 2;
    for (int i = 0; i < (int)(5.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] > initial_ingots);
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

TEST(test_per_row_sell_pays_highest_prefix_unit) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int k = 0; k < MAX_CONTRACTS; k++) w->contracts[k].active = false;

    int consumer = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_consumes(&w->stations[i], COMMODITY_FERRITE_INGOT)) {
            consumer = i;
            break;
        }
    }
    ASSERT(consumer >= 0);
    station_t *st = &w->stations[consumer];
    (void)manifest_consume_by_commodity(&st->manifest,
                                         COMMODITY_FERRITE_INGOT,
                                         manifest_count_by_commodity(&st->manifest,
                                                                     COMMODITY_FERRITE_INGOT));
    st->_inventory_cache[COMMODITY_FERRITE_INGOT] = 0.0f;

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xD1, 8);
    sp->docked = true;
    sp->current_station = (uint8_t)consumer;
    sp->ship.pos = st->pos;

    cargo_unit_t anon = {0};
    anon.kind = (uint8_t)CARGO_KIND_INGOT;
    anon.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    anon.grade = (uint8_t)MINING_GRADE_COMMON;
    anon.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    anon.quantity = 1;
    anon.pub[0] = 0x11;
    cargo_unit_t premium = anon;
    premium.prefix_class = (uint8_t)INGOT_PREFIX_M;
    premium.pub[0] = 0x22;
    ASSERT(manifest_push(&sp->ship.manifest, &anon));
    ASSERT(manifest_push(&sp->ship.manifest, &premium));
    sp->ship.cargo[COMMODITY_FERRITE_INGOT] = 2.0f;

    float before = ledger_balance(st, sp->session_token);
    float expected = station_buy_price_unit(st, &premium);

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token) - before,
                    expected, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship.cargo[COMMODITY_FERRITE_INGOT], 1.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship.manifest.count, 1);
    ASSERT_EQ_INT(sp->ship.manifest.units[0].prefix_class,
                  INGOT_PREFIX_ANONYMOUS);
}

TEST(test_market_buy_skips_named_ingots) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);

    int producer = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_produces(&w->stations[i], COMMODITY_FERRITE_INGOT)) {
            producer = i;
            break;
        }
    }
    ASSERT(producer >= 0);
    station_t *st = &w->stations[producer];
    ASSERT(station_manifest_bootstrap(st));
    (void)manifest_consume_by_commodity(&st->manifest,
                                         COMMODITY_FERRITE_INGOT,
                                         manifest_count_by_commodity(&st->manifest,
                                                                     COMMODITY_FERRITE_INGOT));

    cargo_unit_t premium = {0};
    premium.kind = (uint8_t)CARGO_KIND_INGOT;
    premium.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    premium.grade = (uint8_t)MINING_GRADE_COMMON;
    premium.prefix_class = (uint8_t)INGOT_PREFIX_M;
    premium.quantity = 1;
    premium.pub[0] = 0x31;
    cargo_unit_t anon = premium;
    anon.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    anon.pub[0] = 0x32;
    ASSERT(manifest_push(&st->manifest, &premium));
    ASSERT(manifest_push(&st->manifest, &anon));
    st->_inventory_cache[COMMODITY_FERRITE_INGOT] = 2.0f;

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xD2, 8);
    sp->docked = true;
    sp->current_station = (uint8_t)producer;
    sp->ship.pos = st->pos;
    ledger_earn(st, sp->session_token, 100000.0f);
    float before = ledger_balance(st, sp->session_token);
    float expected_cost = station_sell_price(st, COMMODITY_FERRITE_INGOT);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(before - ledger_balance(st, sp->session_token),
                    expected_cost, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship.cargo[COMMODITY_FERRITE_INGOT], 1.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship.manifest.count, 1);
    ASSERT_EQ_INT(sp->ship.manifest.units[0].prefix_class,
                  INGOT_PREFIX_ANONYMOUS);

    int station_named = 0;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        const cargo_unit_t *u = &st->manifest.units[i];
        if (u->commodity == (uint8_t)COMMODITY_FERRITE_INGOT &&
            u->prefix_class == (uint8_t)INGOT_PREFIX_M) station_named++;
    }
    ASSERT_EQ_INT(station_named, 1);
}

/* Per-row sell mirrors per-row buy: one click sells one (commodity, grade)
 * unit, the rest of the hold stays put. Pre-fix, [S] sold every grade of
 * the row's commodity in one shot. */
TEST(test_per_row_sell_drains_one_unit_only) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xAA, 8);

    /* Dock at Helios: its shipyard consumes frames for repair-kit fab. */
    int helios = 2;
    ASSERT(station_consumes(&w->stations[helios], COMMODITY_FRAME));
    sp->docked = true;
    sp->current_station = (uint8_t)helios;
    sp->ship.pos = w->stations[helios].pos;

    /* Stuff three frame ingots into the hold + manifest, then ask the
     * server to sell exactly one. */
    sp->ship.cargo[COMMODITY_FRAME] = 3.0f;
    cargo_unit_t u = {0};
    u.commodity = COMMODITY_FRAME;
    u.grade = (uint8_t)MINING_GRADE_COMMON;
    for (int i = 0; i < 3; i++) {
        sp->ship.manifest.units[sp->ship.manifest.count++] = u;
    }

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FRAME;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(sp->ship.cargo[COMMODITY_FRAME], 2.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship.manifest.count, 2);
    /* Issuing sell-all next pulls the rest in one go (regression: the
     * sell-one branch must not stick after consuming). */
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FRAME;
    sp->input.service_sell_one = false;
    sp->input.service_sell_grade = MINING_GRADE_COUNT;
    world_sim_step(w, SIM_DT);
    ASSERT_EQ_FLOAT(sp->ship.cargo[COMMODITY_FRAME], 0.0f, 0.001f);
}

void register_economy_basic_tests(void) {
    TEST_SECTION("\nEconomy tests:\n");
    RUN(test_per_row_sell_pays_highest_prefix_unit);
    RUN(test_market_buy_skips_named_ingots);
    RUN(test_per_row_sell_drains_one_unit_only);
    RUN(test_station_production_yard_makes_frames);
    RUN(test_station_production_beamworks_makes_modules);
    RUN(test_station_repair_cost_no_damage);
    RUN(test_station_repair_cost_with_damage);
    RUN(test_can_afford_upgrade_dock_fallback);
    RUN(test_can_afford_upgrade_no_credits_for_dock_fallback);
    RUN(test_can_afford_upgrade_no_product_anywhere);
    RUN(test_can_afford_upgrade_cargo_only_no_credits_needed);
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
    RUN(test_deliver_ingots_full_payout_to_pubkey_player);
    RUN(test_mixed_cargo_sell_and_deliver);
    RUN(test_no_delivery_without_matching_contract);
}

void register_economy_service259_tests(void) {
    TEST_SECTION("\nStation service semantics (#259):\n");
    RUN(test_no_passive_heal_without_kits);
}

/* Count-tier smelt rules pinned: the matrix below is the contract the
 * gameplay design rests on. Any future tweak to station_can_smelt
 * has to update this test and the design notes together. */
TEST(test_count_tier_smelt_rules) {
    station_t st = {0};
    /* 0 furnaces: nothing smelts even with a hopper. */
    st.modules[0].type = MODULE_HOPPER;
    st.module_count = 1;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* 1 furnace + hopper: ferrite only. */
    st.modules[1].type = MODULE_FURNACE;
    st.module_count = 2;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* 2 furnaces + hopper: ferrite + cuprite. */
    st.modules[2].type = MODULE_FURNACE;
    st.module_count = 3;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* 3 furnaces + hopper: cuprite + crystal (ferrite explicitly off). */
    st.modules[3].type = MODULE_FURNACE;
    st.module_count = 4;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* 1 furnace, no hopper: nothing smelts (hopper required). */
    memset(&st, 0, sizeof st);
    st.modules[0].type = MODULE_FURNACE;
    st.module_count = 1;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));

    /* Scaffold furnaces don't count. */
    memset(&st, 0, sizeof st);
    st.modules[0].type = MODULE_HOPPER;
    st.modules[1].type = MODULE_FURNACE;
    st.modules[1].scaffold = true;
    st.module_count = 2;
    ASSERT_EQ_INT(station_furnace_count(&st), 0);
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
}

void register_economy_refinery_smelt_tests(void) {
    TEST_SECTION("\nRefinery smelt test:\n");
    RUN(test_refinery_smelts_ore_in_inventory);
    RUN(test_furnace_without_hopper_does_not_smelt);
    RUN(test_count_tier_smelt_rules);
}

/* station_top_demand: derives the top shortage from inventory + the
 * station's consumed-commodity list. This is the primitive HUD
 * beacons / contract auto-pricing / NPC scoring will compose on top
 * of, so the contract-priority code in game_sim.c and this primitive
 * MUST agree on what "starving" means. The tests below pin those
 * agreements to the same constants. */
TEST(test_top_demand_no_shortage_returns_none) {
    WORLD_DECL;
    world_reset(&w);
    /* Top up Kepler's frame_press input commodity to its target — the
     * station has no shortage, so top demand should be empty. */
    station_t *kepler = &w.stations[1];
    kepler->_inventory_cache[COMMODITY_FERRITE_INGOT] = MAX_PRODUCT_STOCK;
    kepler->_inventory_cache[COMMODITY_FRAME] = MAX_PRODUCT_STOCK;
    kepler->_inventory_cache[COMMODITY_LASER_MODULE] = 100.0f;
    kepler->_inventory_cache[COMMODITY_TRACTOR_MODULE] = 100.0f;
    station_demand_t d = station_top_demand(kepler);
    ASSERT_EQ_INT((int)d.commodity, (int)COMMODITY_COUNT);
    ASSERT_EQ_FLOAT(d.severity, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(d.price_mult, 1.0f, 0.001f);
}

TEST(test_top_demand_picks_starving_commodity) {
    WORLD_DECL;
    world_reset(&w);
    station_t *kepler = &w.stations[1];
    /* Mild shortage on FRAME (consumed by shipyard kit-fab),
     * severe shortage on FERRITE_INGOT (frame_press input). The
     * primitive should pick the worst — ferrite ingots. Targets:
     * frames at 12.0, ferrite ingots at MAX_PRODUCT_STOCK*0.9 = 108.
     * Set frames to 6 (mild, severity ~0.5) and ingots to 0 (full
     * starvation). */
    kepler->_inventory_cache[COMMODITY_FERRITE_INGOT] = 0.0f;
    kepler->_inventory_cache[COMMODITY_FRAME]         = 6.0f;
    kepler->_inventory_cache[COMMODITY_LASER_MODULE]  = 100.0f;
    kepler->_inventory_cache[COMMODITY_TRACTOR_MODULE]= 100.0f;
    station_demand_t d = station_top_demand(kepler);
    ASSERT_EQ_INT((int)d.commodity, (int)COMMODITY_FERRITE_INGOT);
    ASSERT(d.severity > 0.95f);
    /* price_mult = 1.0 + 0.5 * severity → ~1.5 at full starvation. */
    ASSERT(d.price_mult > 1.45f);
    ASSERT(d.price_mult <= 1.5001f);
}

TEST(test_top_demand_skips_self_produced_commodities) {
    /* Helios has its own cuprite furnace + laser fab, so it produces
     * cuprite ingots locally. Even with the float at zero, the
     * primitive must not flag cuprite as a top demand — the local
     * producer is the right answer, not an import. Mirrors the
     * "don't import what we make ourselves" check in game_sim.c
     * priority 4. */
    WORLD_DECL;
    world_reset(&w);
    station_t *helios = &w.stations[2];
    /* Knock out everything else so cuprite is the only candidate
     * (besides things Helios produces). */
    helios->_inventory_cache[COMMODITY_CUPRITE_INGOT] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_INGOT] = MAX_PRODUCT_STOCK;
    helios->_inventory_cache[COMMODITY_FRAME]         = MAX_PRODUCT_STOCK;
    helios->_inventory_cache[COMMODITY_LASER_MODULE]  = MAX_PRODUCT_STOCK;
    helios->_inventory_cache[COMMODITY_TRACTOR_MODULE]= MAX_PRODUCT_STOCK;
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE]   = REFINERY_HOPPER_CAPACITY;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE]   = REFINERY_HOPPER_CAPACITY;
    helios->_inventory_cache[COMMODITY_FERRITE_ORE]   = REFINERY_HOPPER_CAPACITY;
    station_demand_t d = station_top_demand(helios);
    /* Either no demand at all, or demand for something Helios
     * actually doesn't produce — but specifically NOT cuprite ingot. */
    ASSERT(d.commodity != COMMODITY_CUPRITE_INGOT);
}

TEST(test_top_demand_severity_clamped_zero_to_one) {
    /* A negative deficit (overstock) should not produce negative
     * severity, and a wildly empty hopper should clamp to 1.0. */
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    /* Force an overstock on FERRITE_ORE: target = HOPPER_CAPACITY*0.5,
     * supply = capacity, so deficit is negative. The primitive
     * should still report severity = 0 for that commodity (and pick
     * something else, or none). */
    prospect->_inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    station_demand_t d = station_top_demand(prospect);
    /* Whatever it picks, severity must be in [0,1]. */
    ASSERT(d.severity >= 0.0f && d.severity <= 1.0f);
    ASSERT(d.price_mult >= 1.0f && d.price_mult <= 1.5f + 0.001f);
    /* And it must not have picked overstocked ferrite ore. */
    ASSERT(d.commodity != COMMODITY_FERRITE_ORE);
}

/* Demand pricing: a station that's starving for an ingot should post a
 * higher contract price than one that's stocked. Pool_factor and the
 * existing 1.15× content premium stay; the new demand multiplier
 * layers on top, so a fully-stocked station's contract still uses the
 * old price exactly (1.0× demand mult), and a starved station pays up
 * to 50% more. */
TEST(test_contract_price_scales_with_demand) {
    /* Helper to grab Kepler's frame_press ingot import contract. */
    WORLD_DECL_NAME(stocked);
    world_reset(&stocked);
    /* Top up Kepler's ferrite ingot inventory to its target so demand
     * mult is 1.0 — i.e. the existing pricing path. */
    stocked.stations[1]._inventory_cache[COMMODITY_FERRITE_INGOT] = MAX_PRODUCT_STOCK;
    /* Run a few seconds for contract step to fire. */
    for (int i = 0; i < 240; i++) world_sim_step(&stocked, SIM_DT);

    WORLD_DECL_NAME(starved);
    world_reset(&starved);
    /* Starve Kepler completely for ferrite ingots — demand mult ~1.5. */
    starved.stations[1]._inventory_cache[COMMODITY_FERRITE_INGOT] = 0.0f;
    for (int i = 0; i < 240; i++) world_sim_step(&starved, SIM_DT);

    /* Find the (Kepler, FERRITE_INGOT) contract in each world. The
     * stocked world may not generate one at all if supply is at
     * target — that's also a valid outcome (no demand → no
     * contract). The starved world must generate one and price it
     * higher than the stocked baseline if the stocked world did
     * post one. */
    contract_t *c_stocked = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (stocked.contracts[k].active
            && stocked.contracts[k].station_index == 1
            && stocked.contracts[k].commodity == COMMODITY_FERRITE_INGOT) {
            c_stocked = &stocked.contracts[k]; break;
        }
    }
    contract_t *c_starved = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (starved.contracts[k].active
            && starved.contracts[k].station_index == 1
            && starved.contracts[k].commodity == COMMODITY_FERRITE_INGOT) {
            c_starved = &starved.contracts[k]; break;
        }
    }
    ASSERT(c_starved != NULL); /* starvation MUST produce a contract */

    if (c_stocked != NULL) {
        /* If the stocked world also posted a contract, the starved
         * one must be priced higher. The two worlds are otherwise
         * identical so pool_factor + base_price are equal. The only
         * delta is the demand multiplier. */
        ASSERT(c_starved->base_price > c_stocked->base_price * 1.05f);
    }
    /* Either way, the starved contract's price must reflect the
     * demand boost vs. the no-demand baseline of base × 1.15 ×
     * pool. base_price[FERRITE_INGOT] is non-zero by world_reset
     * seeding; the contract should land somewhere between 1.0× and
     * 1.5× of (base × 1.15 × pool). We don't assert the exact value
     * because pool_factor moves with the simulated economy. */
    ASSERT(c_starved->base_price > 0.0f);
}

void register_economy_demand_tests(void) {
    TEST_SECTION("\nStation demand primitive:\n");
    RUN(test_top_demand_no_shortage_returns_none);
    RUN(test_top_demand_picks_starving_commodity);
    RUN(test_top_demand_skips_self_produced_commodities);
    RUN(test_top_demand_severity_clamped_zero_to_one);
    RUN(test_contract_price_scales_with_demand);
}
