#include "tests/test_harness.h"

TEST(test_econ_sim_npc_only_5min) {
    /* Run the world for 5 minutes with NO players — just NPCs.
     * Report: station credit pools, inventories, NPC activity. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    float pool0 = w->stations[0].credit_pool;
    float pool1 = w->stations[1].credit_pool;
    float pool2 = w->stations[2].credit_pool;
    printf("    t=0: pools [%.0f, %.0f, %.0f]\n", pool0, pool1, pool2);

    int ticks = (int)(300.0f / SIM_DT); /* 5 minutes */
    for (int i = 0; i < ticks; i++) {
        world_sim_step(w, SIM_DT);
        if ((i % (ticks / 5)) == 0 && i > 0) {
            float t = (float)i * SIM_DT;
            printf("    t=%.0fs: pools [%.0f, %.0f, %.0f]  ingots [FE=%.0f CU=%.0f CR=%.0f]  frames=%.0f\n",
                t,
                w->stations[0].credit_pool, w->stations[1].credit_pool, w->stations[2].credit_pool,
                w->stations[0].inventory[COMMODITY_FERRITE_INGOT],
                w->stations[2].inventory[COMMODITY_CUPRITE_INGOT],
                w->stations[2].inventory[COMMODITY_CRYSTAL_INGOT],
                w->stations[1].inventory[COMMODITY_FRAME]);
        }
    }
    printf("    t=300s: pools [%.0f, %.0f, %.0f]\n",
        w->stations[0].credit_pool, w->stations[1].credit_pool, w->stations[2].credit_pool);
    printf("    total credits: %.0f (started at %.0f)\n",
        w->stations[0].credit_pool + w->stations[1].credit_pool + w->stations[2].credit_pool,
        pool0 + pool1 + pool2);

    /* Invariant: total credits in station pools should not increase
     * (NPC smelting pays from pool, no player spending to refill) */
    float total_now = w->stations[0].credit_pool + w->stations[1].credit_pool + w->stations[2].credit_pool;
    ASSERT(total_now <= pool0 + pool1 + pool2 + 0.01f);
    /* w auto-freed by WORLD_HEAP cleanup */
}

TEST(test_econ_sim_credit_circulation) {
    /* Simulate: player smelts at Prospect, collects credits, buys ingots,
     * hauls to Kepler, buys frames. Track credit flow. */
    WORLD_DECL;
    world_reset(&w);
    /* Set up session BEFORE player_init_ship so seed credits go to the right token */
    uint8_t token[8] = {1,2,3,4,5,6,7,8};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    player_init_ship(&w.players[0], &w);
    player_seed_credits(&w.players[0], &w);
    w.players[0].connected = true;

    float initial_pool_0 = w.stations[0].credit_pool;
    float initial_pool_1 = w.stations[1].credit_pool;

    float bal0 = ledger_balance(&w.stations[0], token);
    printf("    initial: bal@prospect=%.0f  prospect=%.0f  kepler=%.0f\n",
        bal0, initial_pool_0, initial_pool_1);

    /* Simulate smelting: put ore value into ledger (as if fragments smelted) */
    ledger_credit_supply(&w.stations[0], token, 100.0f); /* 100 cr ore value */

    printf("    after smelt: prospect pool=%.0f (paid out from pool)\n",
        w.stations[0].credit_pool);

    /* Hail at Prospect — informational only, no withdrawal */
    w.players[0].ship.pos = w.stations[0].pos;
    w.players[0].input.hail = true;
    w.players[0].docked = false;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.hail = false;

    bal0 = ledger_balance(&w.stations[0], token);
    printf("    after hail: bal@prospect=%.0f  prospect pool=%.0f\n",
        bal0, w.stations[0].credit_pool);

    /* Dock at Prospect and buy ferrite ingots (spends from station 0 ledger) */
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.stations[0].inventory[COMMODITY_FERRITE_INGOT] = 50.0f;
    w.players[0].input.buy_product = true;
    w.players[0].input.buy_commodity = COMMODITY_FERRITE_INGOT;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.buy_product = false;

    bal0 = ledger_balance(&w.stations[0], token);
    printf("    after buy at prospect: bal@prospect=%.0f  prospect pool=%.0f  cargo FE=%0.f\n",
        bal0, w.stations[0].credit_pool,
        w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT]);

    /* Deliver ingots to Kepler via contract */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 10.0f,
        .base_price = 24.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.players[0].docked = true;
    w.players[0].current_station = 1;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.service_sell = false;

    float bal1 = ledger_balance(&w.stations[1], token);
    printf("    after deliver to kepler: bal@kepler=%.0f  kepler pool=%.0f\n",
        bal1, w.stations[1].credit_pool);

    /* Buy frames from Kepler (spends from station 1 ledger) */
    w.stations[1].inventory[COMMODITY_FRAME] = 20.0f;
    w.players[0].input.buy_product = true;
    w.players[0].input.buy_commodity = COMMODITY_FRAME;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.buy_product = false;

    bal0 = ledger_balance(&w.stations[0], token);
    bal1 = ledger_balance(&w.stations[1], token);
    printf("    after buy frames: bal@kepler=%.0f  kepler pool=%.0f  cargo frames=%.0f\n",
        bal1, w.stations[1].credit_pool,
        w.players[0].ship.cargo[COMMODITY_FRAME]);

    /* Total system credits = all station pools + all player ledger balances */
    float total_credits = bal0 + bal1
        + w.stations[0].credit_pool
        + w.stations[1].credit_pool
        + w.stations[2].credit_pool;
    /* Total = all station pools + all player ledger balances.
     * initial_pool_0 already reflects the 50cr seed deduction (player_init_ship ran). */
    float initial_seed = 50.0f; /* from player_init_ship → ledger_earn at station 0 */
    float initial_total = initial_seed + initial_pool_0 + initial_pool_1 + w.stations[2].credit_pool;
    printf("    total system credits: %.0f (started at %.0f)\n",
        total_credits, initial_total);

    /* Key invariant: total credits in the system should be conserved */
    float expected_total = initial_total;
    ASSERT_EQ_FLOAT(total_credits, expected_total, 1.0f);
}

TEST(test_bug312_1_docked_buy_honors_spend_failure) {
    WORLD_DECL;
    world_reset(&w);
    /* Pick a station that produces frames (has MODULE_FRAME_PRESS). */
    int kepler = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (w.stations[i].id != 0 && station_produces(&w.stations[i], COMMODITY_FRAME)) {
            kepler = i; break;
        }
    }
    ASSERT(kepler >= 0);
    station_t *st = &w.stations[kepler];

    /* Fill the ledger with 16 dummy tokens so find_or_create rejects ours. */
    for (int i = 0; i < 16; i++) {
        uint8_t dummy[8] = { (uint8_t)(0xA0 + i), 0,0,0,0,0,0,0 };
        ledger_earn(st, dummy, 1.0f);
    }
    ASSERT_EQ_INT(st->ledger_count, 16);

    /* Player dockged at the station with no ledger entry available.
     * Give the station inventory and the player cargo space. */
    uint8_t token[8] = {9,9,9,9,9,9,9,9};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = true;
    w.players[0].current_station = kepler;
    st->inventory[COMMODITY_FRAME] = 10.0f;
    float pool_before = st->credit_pool;
    float cargo_before = w.players[0].ship.cargo[COMMODITY_FRAME];

    w.players[0].input.buy_product = true;
    w.players[0].input.buy_commodity = COMMODITY_FRAME;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.buy_product = false;

    /* Post-fix: no cargo delivered and no pool change. Skip inventory:
     * station production may trickle frames in or out during a sim step,
     * but cargo and pool are only touched by the buy path. Together they
     * fully prove the exploit (pre-fix: cargo would rise, pool unchanged
     * because ledger_spend still returns false — the whole point is
     * "cargo moved without payment"). */
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_FRAME], cargo_before, 0.001f);
    ASSERT_EQ_FLOAT(st->credit_pool, pool_before, 0.001f);
}

TEST(test_bug312_2_ledger_balance_matches_by_token) {
    station_t st = {0};
    uint8_t alice[8] = {0xAA,0,0,0,0,0,0,0};
    uint8_t bob[8]   = {0xBB,0,0,0,0,0,0,0};
    uint8_t eve[8]   = {0xEE,0,0,0,0,0,0,0};

    ledger_earn(&st, alice, 100.0f);
    ledger_earn(&st, bob,   250.0f);

    /* Exact token match returns the right balance. */
    ASSERT_EQ_FLOAT(ledger_balance(&st, alice), 100.0f, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(&st, bob),   250.0f, 0.001f);

    /* An unknown token returns 0 — must NOT return the first
     * positive-balance entry (that was the pre-fix bug). */
    ASSERT_EQ_FLOAT(ledger_balance(&st, eve),   0.0f,   0.001f);

    /* Bob's balance is not leaked even after Alice spends down to zero. */
    uint8_t sink[8] = {1,2,3,4,5,6,7,8};
    ledger_earn(&st, sink, 50.0f);
    /* Manually drain alice via earn of negative? ledger_earn doesn't
     * clamp, so instead verify the "first positive" fallback is dead:
     * set Alice's balance to zero directly. */
    for (int i = 0; i < st.ledger_count; i++)
        if (memcmp(st.ledger[i].player_token, alice, 8) == 0)
            st.ledger[i].balance = 0.0f;
    ASSERT_EQ_FLOAT(ledger_balance(&st, alice), 0.0f,   0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(&st, bob),   250.0f, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(&st, eve),   0.0f,   0.001f);
}

TEST(test_bug312_3_init_ship_does_not_seed_with_zero_token) {
    WORLD_DECL;
    world_reset(&w);
    float pool_before = w.stations[0].credit_pool;
    int ledger_before = w.stations[0].ledger_count;

    /* Intentionally leave session_token zeroed — simulates the race
     * where player_init_ship runs before the handshake sets the token. */
    memset(w.players[0].session_token, 0, 8);
    w.players[0].session_ready = false;
    player_init_ship(&w.players[0], &w);

    /* Post-fix: no ledger entry, no pool withdrawal. */
    ASSERT_EQ_INT(w.stations[0].ledger_count, ledger_before);
    ASSERT_EQ_FLOAT(w.stations[0].credit_pool, pool_before, 0.001f);

    /* Now set the real token and seed — credits should land on the
     * real token, and no zero-token entry should exist. */
    uint8_t token[8] = {0x42,1,2,3,4,5,6,7};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    player_seed_credits(&w.players[0], &w);

    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0], token), 50.0f, 0.001f);
    uint8_t zero[8] = {0};
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0], zero), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(w.stations[0].credit_pool, pool_before - 50.0f, 0.001f);

    /* Double-seed is idempotent (guard against credit leak on reconnect). */
    player_seed_credits(&w.players[0], &w);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0], token), 50.0f, 0.001f);
    ASSERT_EQ_FLOAT(w.stations[0].credit_pool, pool_before - 50.0f, 0.001f);
}

TEST(test_econ_invariant_npc_only_conservation) {
    /* Run the NPC-only sim for 2 sim-minutes and assert the total
     * credit supply is conserved at every tick. Catches any future
     * leak in production, contract payout, or construction paths. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    double initial = econ_total_credits(w);

    int ticks = (int)(120.0f / SIM_DT); /* 2 minutes of sim time */
    double min_seen = initial, max_seen = initial;
    for (int i = 0; i < ticks; i++) {
        world_sim_step(w, SIM_DT);
        double total = econ_total_credits(w);
        if (total < min_seen) min_seen = total;
        if (total > max_seen) max_seen = total;
        /* Strict conservation — 1cr slack for float roundoff across
         * thousands of sub-cent operations. */
        if (fabs(total - initial) > 1.0) {
            printf("FAIL\n    tick %d: total=%.4f initial=%.4f drift=%.4f\n",
                i, total, initial, total - initial);
            tests_failed++;
            return;
        }
    }
    printf("[conserved over %d ticks, drift range [%.4f, %.4f]] ",
        ticks, min_seen - initial, max_seen - initial);
}

TEST(test_econ_invariant_player_session_conservation) {
    /* Script a player through every credit-touching action (sell via
     * contract, sell via fallback, buy product, repair, upgrade) and
     * assert conservation at each step. */
    WORLD_DECL;
    world_reset(&w);
    uint8_t token[8] = {0x42, 1, 2, 3, 4, 5, 6, 7};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    player_init_ship(&w.players[0], &w);
    player_seed_credits(&w.players[0], &w);
    w.players[0].connected = true;
    double initial = econ_total_credits(&w);

    #define ASSERT_CONSERVED(label) do { \
        double _t = econ_total_credits(&w); \
        if (fabs(_t - initial) > 1.0) { \
            printf("FAIL\n    %s: total=%.4f initial=%.4f drift=%.4f\n", \
                label, _t, initial, _t - initial); \
            tests_failed++; return; \
    } \
} while (0)

    /* Step 1: NPC activity for a while (proxy for belt traffic) */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);
    ASSERT_CONSERVED("after 5s NPC activity");

    /* Step 2: contract sell path — deliver ferrite ingots to Kepler */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 5.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.players[0].docked = true;
    w.players[0].current_station = 1;
    w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] = 5.0f;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.service_sell = false;
    ASSERT_CONSERVED("after contract sell");

    /* Step 3: buy-product path — buy frames from Kepler */
    w.stations[1].inventory[COMMODITY_FRAME] = 20.0f;
    w.players[0].input.buy_product = true;
    w.players[0].input.buy_commodity = COMMODITY_FRAME;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.buy_product = false;
    ASSERT_CONSERVED("after buy product");

    /* Step 4: repair path — damage the ship, dock at a repair-capable
     * station, trigger service_repair. Passive repair also runs, but
     * the invariant holds either way (repair fee is pool->ledger only
     * if the ledger had enough; otherwise it's a no-op). */
    w.players[0].ship.hull = 50.0f;
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.service_repair = false;
    ASSERT_CONSERVED("after repair");

    /* Step 5: supply credit path — simulate NPC smelting paying out */
    ledger_credit_supply(&w.stations[0], token, 50.0f);
    ASSERT_CONSERVED("after supply credit");

    /* Step 6: long tail — run another 5s of sim with all the activity
     * above baked in, verify no drift accumulates. */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);
    ASSERT_CONSERVED("after 5s post-action sim");

    #undef ASSERT_CONSERVED
}

void register_econ_sim_sim_tests(void) {
    TEST_SECTION("\nEconomy simulations:\n");
    RUN(test_econ_sim_npc_only_5min);
    RUN(test_econ_sim_credit_circulation);
}

void register_econ_sim_bug312_tests(void) {
    TEST_SECTION("\n#312 4-bug-fix regressions:\n");
    RUN(test_bug312_1_docked_buy_honors_spend_failure);
    RUN(test_bug312_2_ledger_balance_matches_by_token);
    RUN(test_bug312_3_init_ship_does_not_seed_with_zero_token);
}

void register_econ_sim_invariant_tests(void) {
    TEST_SECTION("\nEconomy invariant (conservation):\n");
    RUN(test_econ_invariant_npc_only_conservation);
    RUN(test_econ_invariant_player_session_conservation);
}

