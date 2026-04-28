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
     * initial_pool_0 already reflects the spawn fee being added to the
     * pool by player_seed_credits — so the player's starting balance
     * is the negative of that fee, and conservation is the sum of
     * pools + the negative seed. */
    float initial_seed = -(float)station_spawn_fee(&w.stations[0]);
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

TEST(test_buy_finished_good_requires_manifest_unit) {
    /* Manifest authority for finished-good BUY (#340 slice A): if the
     * station's float inventory says a frame is available but no
     * manifest unit backs it, the buy must reject and not charge.
     * Pre-fix the player's ship.cargo[FRAME] would rise and the
     * ledger would deduct, leaving a phantom unbacked unit. */
    WORLD_DECL;
    world_reset(&w);
    int kepler = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (w.stations[i].id != 0 && station_produces(&w.stations[i], COMMODITY_FRAME)) {
            kepler = i; break;
        }
    }
    ASSERT(kepler >= 0);
    station_t *st = &w.stations[kepler];

    /* Float says 10 frames, manifest is empty — the drift the fix guards. */
    st->inventory[COMMODITY_FRAME] = 10.0f;
    /* Force-clear any frame manifest units the world reset seeded. */
    for (int i = (int)st->manifest.count - 1; i >= 0; i--) {
        if (st->manifest.units[i].commodity == (uint8_t)COMMODITY_FRAME) {
            cargo_unit_t tmp;
            (void)manifest_remove(&st->manifest, (uint16_t)i, &tmp);
        }
    }
    ASSERT_EQ_INT(manifest_count_by_commodity(&st->manifest, COMMODITY_FRAME), 0);

    uint8_t token[8] = {7,7,7,7,7,7,7,7};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = true;
    w.players[0].current_station = kepler;
    /* Fund the player so afford isn't the gate. */
    ledger_earn(st, token, 100000.0f);

    float bal_before = ledger_balance(st, token);
    float cargo_before = w.players[0].ship.cargo[COMMODITY_FRAME];

    w.players[0].input.buy_product = true;
    w.players[0].input.buy_commodity = COMMODITY_FRAME;
    w.players[0].input.buy_grade = MINING_GRADE_COUNT;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.buy_product = false;

    /* The buy must have rejected: no cargo, no charge. */
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_FRAME], cargo_before, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(st, token), bal_before, 0.001f);
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

    /* Now set the real token and seed — the spawn-fee debit should
     * land on the real token, push it into debt, and no zero-token
     * entry should exist. */
    uint8_t token[8] = {0x42,1,2,3,4,5,6,7};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    int fee = station_spawn_fee(&w.stations[0]);
    player_seed_credits(&w.players[0], &w);

    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0], token), -(float)fee, 0.001f);
    uint8_t zero[8] = {0};
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0], zero), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(w.stations[0].credit_pool, pool_before + (float)fee, 0.001f);

    /* Re-seed is idempotent (guard against double-charge on reconnect). */
    player_seed_credits(&w.players[0], &w);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0], token), -(float)fee, 0.001f);
    ASSERT_EQ_FLOAT(w.stations[0].credit_pool, pool_before + (float)fee, 0.001f);
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

/* Locks the contract that grade-aware sell pays a real bonus for high-
 * grade ingots AND that the units the pricing walk priced are the
 * units the manifest transfer actually moves. Compares two identical
 * runs that differ only in one ingot's grade — the bonus is the
 * delta, immune to dynamic-price scaling. */
static float run_sell_with_grades(int g0, int g1, int g2,
                                  int *out_common, int *out_rare,
                                  float *out_ship_inventory_remaining) {
    /* WORLD_HEAP attaches __attribute__((cleanup)) so the world is
     * world_cleanup()'d + freed on every return path — including the
     * early returns below. Previously a raw calloc leaked the per-
     * player manifests on every test invocation, which only blew up
     * loudly under AddressSanitizer. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    if (!w) return -1.0f;
    world_reset(w);
    /* Drain auto-spawned contracts so the fab-fallback branch handles
     * the sell (deterministic price = station_buy_price, not contract). */
    for (int k = 0; k < MAX_CONTRACTS; k++) w->contracts[k].active = false;
    int kepler = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (w->stations[i].id == 0) continue;
        if (station_consumes(&w->stations[i], COMMODITY_FERRITE_INGOT)) { kepler = i; break; }
    }
    if (kepler < 0) return -1.0f; /* WORLD_HEAP cleanup frees w. */
    station_t *st = &w->stations[kepler];
    st->inventory[COMMODITY_FERRITE_INGOT] = 0.0f;

    uint8_t token[8] = {7,7,7,7,7,7,7,7};
    memcpy(w->players[0].session_token, token, 8);
    w->players[0].session_ready = true;
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    w->players[0].docked = true;
    w->players[0].current_station = kepler;
    w->players[0].ship.cargo[COMMODITY_FERRITE_INGOT] = 3.0f;
    ship_manifest_bootstrap(&w->players[0].ship);
    cargo_unit_t u; memset(&u, 0, sizeof(u));
    u.commodity = COMMODITY_FERRITE_INGOT; u.kind = CARGO_KIND_INGOT;
    u.grade = (uint8_t)g0; u.pub[0] = 1; manifest_push(&w->players[0].ship.manifest, &u);
    u.grade = (uint8_t)g1; u.pub[0] = 2; manifest_push(&w->players[0].ship.manifest, &u);
    u.grade = (uint8_t)g2; u.pub[0] = 3; manifest_push(&w->players[0].ship.manifest, &u);

    float bal_before = ledger_balance(st, token);
    w->players[0].input.service_sell = true;
    world_sim_step(w, SIM_DT);
    w->players[0].input.service_sell = false;
    float earned = ledger_balance(st, token) - bal_before;

    int common = 0, rare = 0;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        if (st->manifest.units[i].commodity != COMMODITY_FERRITE_INGOT) continue;
        if (st->manifest.units[i].grade == MINING_GRADE_COMMON) common++;
        if (st->manifest.units[i].grade == MINING_GRADE_RARE)   rare++;
    }
    if (out_common) *out_common = common;
    if (out_rare)   *out_rare   = rare;
    if (out_ship_inventory_remaining)
        *out_ship_inventory_remaining = w->players[0].ship.cargo[COMMODITY_FERRITE_INGOT];
    return earned;
    /* WORLD_HEAP cleanup attribute frees w on scope exit. */
}

TEST(test_grade_aware_sell_pays_per_unit_grade) {
    int common_a = 0, rare_a = 0, common_b = 0, rare_b = 0;
    float remain_a = 0, remain_b = 0;

    /* Run A: 3 commons. Run B: 2 commons + 1 rare in the middle. */
    float earned_all_common = run_sell_with_grades(
        MINING_GRADE_COMMON, MINING_GRADE_COMMON, MINING_GRADE_COMMON,
        &common_a, &rare_a, &remain_a);
    float earned_with_rare = run_sell_with_grades(
        MINING_GRADE_COMMON, MINING_GRADE_RARE, MINING_GRADE_COMMON,
        &common_b, &rare_b, &remain_b);

    /* Both runs delivered all 3 units to station manifest with grades
     * preserved. Proves the transfer walked the same units the pricing
     * walk priced — if it had reordered, station_rare would land in
     * the wrong slot or stay 0. */
    ASSERT_EQ_FLOAT(remain_a, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(remain_b, 0.0f, 0.001f);
    ASSERT_EQ_INT(common_a, 3);
    ASSERT_EQ_INT(rare_a,   0);
    ASSERT_EQ_INT(common_b, 2);
    ASSERT_EQ_INT(rare_b,   1);

    /* Run B = run A + (rare_mult - 1.0) * base on the rare unit. With
     * rare_mult = 2.0, the bonus equals one common's pay — earned_b
     * must be ~4/3 of earned_a (within 5% to absorb price-curve noise
     * from the extra inventory unit on the second run). */
    ASSERT(earned_with_rare > earned_all_common + 0.5f);
    float ratio = earned_with_rare / earned_all_common;
    ASSERT(ratio > 1.25f && ratio < 1.45f);
}

/* ================================================================== */
/* Kit-economy e2e: run the sim long enough for the kit chain to       */
/* visibly converge, NPC damage to drive demand, contracts to cycle.   */
/* ================================================================== */

TEST(test_e2e_kit_chain_converges) {
    /* Pre-seed Kepler with the three fab inputs so kit fab can fire
     * without first bootstrapping the entire upstream chain (smelting,
     * pressing, etc — not what this test is about). 5 sim-minutes is
     * 10 fab cycles at REPAIR_KIT_FAB_PERIOD = 30s; should produce
     * many full batches if the gate works. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    /* Find the shipyard station. */
    int shipyard = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w->stations[s], MODULE_SHIPYARD)) {
            shipyard = s; break;
        }
    }
    ASSERT(shipyard >= 0);
    /* Big buffer of inputs so fab never starves. */
    w->stations[shipyard].inventory[COMMODITY_FRAME]          = 50.0f;
    w->stations[shipyard].inventory[COMMODITY_LASER_MODULE]   = 50.0f;
    w->stations[shipyard].inventory[COMMODITY_TRACTOR_MODULE] = 50.0f;
    w->stations[shipyard].inventory[COMMODITY_REPAIR_KIT]     = 0.0f;
    w->stations[shipyard].repair_kit_fab_timer = 0.0f;

    int ticks = (int)(300.0f / SIM_DT);
    for (int i = 0; i < ticks; i++) world_sim_step(w, SIM_DT);

    float kits_now = w->stations[shipyard].inventory[COMMODITY_REPAIR_KIT];
    printf("    shipyard %d kits after 300s: %.0f (expect > 0)\n",
           shipyard, kits_now);
    ASSERT(kits_now > 0.0f);

    /* Inputs should also be visibly drawn down — at least one batch
     * consumed of each input commodity (fewer than seed, > 0 means
     * something was minted but not all 50). */
    ASSERT(w->stations[shipyard].inventory[COMMODITY_FRAME]        < 50.0f);
    ASSERT(w->stations[shipyard].inventory[COMMODITY_LASER_MODULE] < 50.0f);
    ASSERT(w->stations[shipyard].inventory[COMMODITY_TRACTOR_MODULE] < 50.0f);
}

TEST(test_e2e_npc_dock_auto_repair_drains_kits) {
    /* A damaged hauler returning home to a kit-stocked dock should heal
     * AND drain station kit inventory. Verifies PR #375 end-to-end:
     * NPC hull damage exists, hauler dock transition fires the repair,
     * kits actually flow out of station inventory.
     *
     * Setup the hauler in RETURN_TO_STATION near home so a single sim
     * step triggers the dock-arrival branch and the kit drain. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    int shipyard = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w->stations[s], MODULE_SHIPYARD)) {
            shipyard = s; break;
        }
    }
    ASSERT(shipyard >= 0);
    /* Stock the dock with kits so the auto-repair has something to drain. */
    w->stations[shipyard].inventory[COMMODITY_REPAIR_KIT] = 100.0f;

    /* Pick the first hauler that's currently homed at the shipyard,
     * wound it, and drop it just outside the dock approach radius. */
    npc_ship_t *hauler = NULL;
    int hauler_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_HAULER) continue;
        w->npc_ships[n].home_station = shipyard;
        hauler = &w->npc_ships[n];
        hauler_slot = n;
        break;
    }
    ASSERT(hauler != NULL);
    float max_h = npc_max_hull(hauler);
    /* Damage through the public ship-layer helper so ship.hull stays
     * authoritative (#294 Slice 11). Writing hauler->hull directly
     * would leave ship.hull at max_h and the auto-repair would see
     * nothing to fix. */
    apply_npc_ship_damage(w, hauler_slot, 20.0f);
    hauler->state = NPC_STATE_RETURN_TO_STATION;
    /* Drop the hauler well inside the home station's dock approach
     * radius so the next sim_step's RETURN_TO_STATION branch trips
     * the dock-arrival condition (dist < dock_radius * 0.7). */
    hauler->pos = w->stations[shipyard].pos;
    hauler->vel = v2(0.0f, 0.0f);
    /* Slice 13: physics is ship-authoritative going into the tick — write
     * the paired ship_t too so the pre-mirror doesn't overwrite the npc
     * fields with a stale ship snapshot. */
    {
        ship_t *hauler_ship = world_npc_ship_for(w, hauler_slot);
        ASSERT(hauler_ship != NULL);
        hauler_ship->pos = w->stations[shipyard].pos;
        hauler_ship->vel = v2(0.0f, 0.0f);
    }

    float kits_before = w->stations[shipyard].inventory[COMMODITY_REPAIR_KIT];
    /* A handful of ticks — first one should land it at the berth and
     * fire the repair branch; subsequent ticks just sit at DOCKED. */
    for (int i = 0; i < 5; i++) world_sim_step(w, SIM_DT);

    float kits_after = w->stations[shipyard].inventory[COMMODITY_REPAIR_KIT];
    printf("    hauler hull: %.1f -> %.1f (max %.1f), station kits %.0f -> %.0f\n",
           max_h - 20.0f, hauler->hull, max_h, kits_before, kits_after);
    /* Healed — could be partial if repaired tick fell short, but should be
     * visibly higher than the starting wound. */
    ASSERT(hauler->hull > max_h - 20.0f);
    /* Station drained kits to do the repair. */
    ASSERT(kits_after < kits_before);
}

TEST(test_e2e_kit_import_contract_lifecycle) {
    /* Kit-import contract at Prospect (no shipyard) should:
     *   (a) appear when kit inventory drops below 25% of cap,
     *   (b) close when inventory rises above the close threshold. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    int prospect = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w->stations[s], MODULE_DOCK) &&
            !station_has_module(&w->stations[s], MODULE_SHIPYARD)) {
            prospect = s; break;
        }
    }
    ASSERT(prospect >= 0);

    /* Phase 1: drain kits and run until a kit import contract is issued. */
    w->stations[prospect].inventory[COMMODITY_REPAIR_KIT] = 0.0f;
    bool found_open = false;
    for (int i = 0; i < (int)(120.0f / SIM_DT); i++) {
        world_sim_step(w, SIM_DT);
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            const contract_t *c = &w->contracts[k];
            if (c->active && c->action == CONTRACT_TRACTOR
                && c->station_index == prospect
                && c->commodity == COMMODITY_REPAIR_KIT) {
                found_open = true; break;
            }
        }
        if (found_open) break;
    }
    ASSERT(found_open);

    /* Phase 2: refill enough to satisfy BOTH close and issue checks.
     * Close fires above 0.8 * MAX_PRODUCT_STOCK (96), but P6 re-issues
     * below 0.25 * REPAIR_KIT_STOCK_CAP (250) — the gap between them
     * is unstable (closes and re-issues every tick). Filling above the
     * higher bound is the only stable closed state. The fact that
     * those two thresholds don't agree is a real bug worth a separate
     * PR; this test pins current behaviour for now. */
    w->stations[prospect].inventory[COMMODITY_REPAIR_KIT] = REPAIR_KIT_STOCK_CAP * 0.5f;
    bool found_after_fill = true;
    for (int i = 0; i < (int)(60.0f / SIM_DT); i++) {
        world_sim_step(w, SIM_DT);
        found_after_fill = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            const contract_t *c = &w->contracts[k];
            if (c->active && c->action == CONTRACT_TRACTOR
                && c->station_index == prospect
                && c->commodity == COMMODITY_REPAIR_KIT) {
                found_after_fill = true; break;
            }
        }
        if (!found_after_fill) break;
    }
    ASSERT(!found_after_fill);
}

void register_econ_sim_sim_tests(void) {
    TEST_SECTION("\nEconomy simulations:\n");
    RUN(test_econ_sim_npc_only_5min);
    RUN(test_econ_sim_credit_circulation);
    RUN(test_grade_aware_sell_pays_per_unit_grade);
    RUN(test_e2e_kit_chain_converges);
    RUN(test_e2e_npc_dock_auto_repair_drains_kits);
    RUN(test_e2e_kit_import_contract_lifecycle);
}

void register_econ_sim_bug312_tests(void) {
    TEST_SECTION("\n#312 4-bug-fix regressions:\n");
    RUN(test_bug312_1_docked_buy_honors_spend_failure);
    RUN(test_buy_finished_good_requires_manifest_unit);
    RUN(test_bug312_2_ledger_balance_matches_by_token);
    RUN(test_bug312_3_init_ship_does_not_seed_with_zero_token);
}

void register_econ_sim_invariant_tests(void) {
    TEST_SECTION("\nEconomy invariant (conservation):\n");
    RUN(test_econ_invariant_npc_only_conservation);
    RUN(test_econ_invariant_player_session_conservation);
}

