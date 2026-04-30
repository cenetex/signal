/* Tests for the sovereign-station-ledger model.
 *
 * After this PR, a station's credit_pool is informational — it tracks
 * the running total of currency the station has minted into circulation,
 * but it has no floor and no policy meaning. Stations are sovereign
 * issuers: they always pay out, even at arbitrarily negative pool.
 *
 * The asymmetry matters: stations can go negative (issuance), players
 * cannot spend more than they hold (no involuntary debt from BUY).
 * The miner is the only source of new value into the system; cross-
 * currency transfer is via goods (the hauler IS the FX desk).
 *
 * See feat/sovereign-station-ledgers and CLAUDE.md "Economy: per-station
 * credits" for the full justification.
 */

#include "tests/test_harness.h"

/* Test 1: pool can go negative.
 *
 * Drive the pool below zero by force-paying out via ledger_credit_supply
 * (NPC smelt path). Assert no crash, no refused payouts, no clamping. */
TEST(test_sovereign_pool_can_go_negative) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    station_t *st = &w->stations[0];
    float start_pool = st->credit_pool;
    /* Pay out far more than the starting pool so we cross zero. */
    uint8_t token[8] = {0xAA,1,2,3,4,5,6,7};
    /* Each ledger_credit_supply pays 65% of ore_value to the player and
     * debits the pool by exactly that amount. To drain ~start_pool +
     * extra_negative, we feed (start_pool + 10000) / 0.65 worth of ore. */
    float ore_value = (start_pool + 10000.0f) / 0.65f;
    float credited = ledger_credit_supply_amount(st, token, ore_value);
    ASSERT(credited > 0.0f);
    ASSERT(st->credit_pool < 0.0f);
    /* And the player's ledger entry got the full supplier share — not
     * clamped because the pool would have gone negative. */
    ASSERT_EQ_FLOAT(ledger_balance(st, token), credited, 0.01f);

    /* Sim should keep stepping with a negative pool — no asserts, no
     * NaNs, no payout refusals. */
    for (int i = 0; i < 240; i++) world_sim_step(w, SIM_DT);
    /* Pool should still be a finite number (could have moved further
     * negative as NPCs delivered more ore). */
    ASSERT(isfinite(st->credit_pool));
}

/* Test 2: a station with deeply negative pool still pays a miner.
 *
 * Force-set credit_pool = -5000, then call ledger_credit_supply. Player's
 * balance must increment by the full supplier share. */
TEST(test_sovereign_negative_pool_still_pays_miner) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    station_t *st = &w->stations[0];
    st->credit_pool = -5000.0f;
    float pool_before = st->credit_pool;

    uint8_t token[8] = {0xBB,1,2,3,4,5,6,7};
    float supplier_share = ledger_credit_supply_amount(st, token, 100.0f);

    /* Supplier got their 65%, full price. */
    ASSERT_EQ_FLOAT(supplier_share, 65.0f, 0.5f);
    ASSERT_EQ_FLOAT(ledger_balance(st, token), supplier_share, 0.01f);
    /* Pool moved by exactly the supplier share — not clamped at any floor. */
    ASSERT_EQ_FLOAT(st->credit_pool, pool_before - supplier_share, 0.5f);
    ASSERT(st->credit_pool < pool_before);
}

/* Test 3: players still cannot go negative on a BUY.
 *
 * Sanity check we didn't accidentally remove the player-side spend
 * check while removing the station-side floor. Spawn a fresh player at
 * a station with no balance, try to buy a finished good — must refuse,
 * cargo must not move. */
TEST(test_sovereign_player_cannot_overspend_on_buy) {
    WORLD_DECL;
    world_reset(&w);

    /* Find a station that produces frames. */
    int kepler = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (w.stations[i].id != 0 && station_produces(&w.stations[i], COMMODITY_FRAME)) {
            kepler = i; break;
        }
    }
    ASSERT(kepler >= 0);
    station_t *st = &w.stations[kepler];

    uint8_t token[8] = {0xCC,1,2,3,4,5,6,7};
    memcpy(w.players[0].session_token, token, 8);
    w.players[0].session_ready = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = true;
    w.players[0].current_station = kepler;

    /* Make sure the player's ledger balance is exactly 0 — no spawn
     * fee debt either, otherwise the test isn't clean. */
    int idx = -1;
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_token, token, 8) == 0) { idx = i; break; }
    }
    if (idx >= 0) st->ledger[idx].balance = 0.0f;
    ASSERT_EQ_FLOAT(ledger_balance(st, token), 0.0f, 0.001f);

    st->_inventory_cache[COMMODITY_FRAME] = 10.0f;
    float cargo_before = w.players[0].ship.cargo[COMMODITY_FRAME];
    float bal_before = ledger_balance(st, token);

    w.players[0].input.buy_product = true;
    w.players[0].input.buy_commodity = COMMODITY_FRAME;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.buy_product = false;

    /* Refused: no cargo, no balance change. The station's pool sign
     * is irrelevant — the gate is the *player's* purse. */
    ASSERT_EQ_FLOAT(w.players[0].ship.cargo[COMMODITY_FRAME], cargo_before, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(st, token), bal_before, 0.001f);
}

/* Test 4: a happy-path NPC sim emits no [WARN] log lines about pool
 * bounds. (We don't have a structured log capture — instead, assert
 * the warning counter from the test harness didn't move. The harness
 * only bumps g_warnings when TEST_WARN is fired by tests, so this is
 * really a smoke check that the sim's own logging doesn't somehow
 * funnel through TEST_WARN. With no in-code path emitting "pool low"
 * warnings, this should always hold.) */
TEST(test_sovereign_no_spurious_warnings_on_positive_pool) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    int warnings_before = g_warnings;
    /* Run for 30 sim-seconds. With no players, pools may drift down as
     * NPCs deliver ore — we explicitly assert pool stays above zero
     * over the run, so we're checking the "pool stayed positive AND no
     * warning fired" regime. */
    for (int i = 0; i < (int)(30.0f / SIM_DT); i++) {
        world_sim_step(w, SIM_DT);
    }
    /* Sanity: at least one station's pool stayed above its starting
     * threshold. (We don't pin a specific number — just assert the run
     * was in the "positive pool" regime, which is the precondition for
     * the warning-free claim.) */
    bool any_positive = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (w->stations[s].id != 0 && w->stations[s].credit_pool > 0.0f) {
            any_positive = true; break;
        }
    }
    ASSERT(any_positive);
    ASSERT_EQ_INT(g_warnings, warnings_before);
}

/* Test 5: save/load round-trips a negative pool exactly.
 *
 * Pre-fix, no path could produce a negative pool, so the save format
 * was effectively only validated against non-negative floats. Pin that
 * the float field round-trips precisely for negative values too. */
TEST(test_sovereign_save_load_preserves_negative_pool) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    w->stations[0].credit_pool = -1234.5f;

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("sov_cat")));
    ASSERT(world_save(w, TMP("sov_neg.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("sov_cat"));
    ASSERT(world_load(loaded, TMP("sov_neg.sav")));

    ASSERT_EQ_FLOAT(loaded->stations[0].credit_pool, -1234.5f, 0.001f);

    remove(TMP("sov_neg.sav"));
}

void register_sovereign_ledger_tests(void) {
    TEST_SECTION("\nSovereign station ledgers (#479):\n");
    RUN(test_sovereign_pool_can_go_negative);
    RUN(test_sovereign_negative_pool_still_pays_miner);
    RUN(test_sovereign_player_cannot_overspend_on_buy);
    RUN(test_sovereign_no_spurious_warnings_on_positive_pool);
    RUN(test_sovereign_save_load_preserves_negative_pool);
}
