#include "tests/test_harness.h"

TEST(test_relationship_dock_dock_ticking) {
    /* Verify dock events increment total_docks and set first/last dock ticks. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[0];

    /* Create a ledger entry with a pubkey */
    uint8_t player_pubkey[32];
    memset(player_pubkey, 0x42, 32);

    /* Dock the player (record first dock). w->time is float; cast
     * explicitly to keep MSVC's strict mode quiet. */
    uint64_t tick1 = (uint64_t)w->time;
    ledger_record_dock(st, player_pubkey, tick1);
    int idx = ledger_find_or_create_by_pubkey(st, player_pubkey);
    ASSERT(idx >= 0);
    ASSERT(st->ledger[idx].first_dock_tick == tick1);
    ASSERT(st->ledger[idx].last_dock_tick == tick1);
    ASSERT(st->ledger[idx].total_docks == 1);

    /* Dock again later */
    uint64_t tick2 = tick1 + 500;
    ledger_record_dock(st, player_pubkey, tick2);
    ASSERT(st->ledger[idx].first_dock_tick == tick1);
    ASSERT(st->ledger[idx].last_dock_tick == tick2);
    ASSERT(st->ledger[idx].total_docks == 2);

    printf("    dock 1: tick=%llu  dock 2: tick=%llu  total_docks=%u\n",
        (unsigned long long)tick1, (unsigned long long)tick2, st->ledger[idx].total_docks);
}

TEST(test_relationship_ore_tracking) {
    /* Verify ore sales increment lifetime_ore_units and set top_commodity. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[0];

    uint8_t player_pubkey[32];
    memset(player_pubkey, 0x55, 32);

    /* Record some ore sales */
    ledger_record_ore_sold(st, player_pubkey, 100, COMMODITY_FERRITE_ORE);
    ledger_record_ore_sold(st, player_pubkey, 50, COMMODITY_FERRITE_ORE);
    ledger_record_ore_sold(st, player_pubkey, 25, COMMODITY_CUPRITE_ORE);

    int idx = ledger_find_or_create_by_pubkey(st, player_pubkey);
    ASSERT(idx >= 0);
    ASSERT(st->ledger[idx].lifetime_ore_units == 175);
    ASSERT(st->ledger[idx].top_commodity == COMMODITY_CUPRITE_ORE);

    printf("    ore units: %u  top commodity: %u\n", st->ledger[idx].lifetime_ore_units, st->ledger[idx].top_commodity);
}

TEST(test_relationship_credits_in_out) {
    /* Verify credit tracking increments lifetime_credits_in and out. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[0];

    uint8_t player_pubkey[32];
    memset(player_pubkey, 0x77, 32);

    /* Credit the player (ore sale). Station keeps a 35% smelt cut, so
     * the supplier share is 250 * 0.65 = 162.5 → 162 after uint32_t
     * truncation in lifetime_credits_in. */
    ledger_credit_supply_by_pubkey(st, player_pubkey, 250.0f);
    int idx = ledger_find_or_create_by_pubkey(st, player_pubkey);
    ASSERT(idx >= 0);
    ASSERT(st->ledger[idx].lifetime_credits_in == 162);

    /* Spend from the ledger */
    ship_t dummy_ship = {0};
    ledger_spend_by_pubkey(st, player_pubkey, 75.0f, &dummy_ship);
    ASSERT(st->ledger[idx].lifetime_credits_out == 75);

    printf("    credits in: %u  credits out: %u\n", st->ledger[idx].lifetime_credits_in, st->ledger[idx].lifetime_credits_out);
}

TEST(test_relationship_save_load) {
    /* Verify relationship data survives save/load round-trip. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[0];

    uint8_t player_pubkey[32];
    memset(player_pubkey, 0x99, 32);

    /* Set up relationship data */
    ledger_record_dock(st, player_pubkey, 1000);
    ledger_credit_supply_by_pubkey(st, player_pubkey, 500.0f);
    ledger_record_ore_sold(st, player_pubkey, 200, COMMODITY_CRYSTAL_ORE);
    ledger_spend_by_pubkey(st, player_pubkey, 100.0f, NULL);

    int idx = ledger_find_or_create_by_pubkey(st, player_pubkey);
    uint64_t first_dock_before = st->ledger[idx].first_dock_tick;
    uint64_t last_dock_before = st->ledger[idx].last_dock_tick;
    uint32_t total_docks_before = st->ledger[idx].total_docks;
    uint32_t ore_before = st->ledger[idx].lifetime_ore_units;
    uint32_t credits_in_before = st->ledger[idx].lifetime_credits_in;
    uint32_t credits_out_before = st->ledger[idx].lifetime_credits_out;

    /* Save and reload */
    const char *tmppath = "/tmp/test_relationship_save.sav";
    bool saved = world_save(w, tmppath);
    ASSERT(saved);
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    bool loaded = world_load(w2, tmppath);
    unlink(tmppath);
    ASSERT(loaded);

    /* Verify data is preserved */
    station_t *st2 = &w2->stations[0];
    int idx2 = ledger_find_or_create_by_pubkey(st2, player_pubkey);
    ASSERT(idx2 >= 0);
    ASSERT(st2->ledger[idx2].first_dock_tick == first_dock_before);
    ASSERT(st2->ledger[idx2].last_dock_tick == last_dock_before);
    ASSERT(st2->ledger[idx2].total_docks == total_docks_before);
    ASSERT(st2->ledger[idx2].lifetime_ore_units == ore_before);
    ASSERT(st2->ledger[idx2].lifetime_credits_in == credits_in_before);
    ASSERT(st2->ledger[idx2].lifetime_credits_out == credits_out_before);

    printf("    save/load preserved: docks=%u ore=%u credits_in=%u\n",
        st2->ledger[idx2].total_docks, st2->ledger[idx2].lifetime_ore_units, st2->ledger[idx2].lifetime_credits_in);

}

TEST(test_relationship_anonymous_pubkey_ignored) {
    /* Verify zero-pubkey (anonymous) entries return -1 from ledger_find_or_create_by_pubkey. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[0];

    uint8_t anon_pubkey[32];
    memset(anon_pubkey, 0, 32);

    /* Try to record dock for anonymous — should return -1 */
    int idx = ledger_find_or_create_by_pubkey(st, anon_pubkey);
    ASSERT(idx == -1);

    printf("    anonymous pubkey correctly returns -1\n");
}

void register_relationship_tests(void);
void register_relationship_tests(void) {
    TEST_SECTION("\n--- Station-player relationship (#257) ---\n");
    RUN(test_relationship_dock_dock_ticking);
    RUN(test_relationship_ore_tracking);
    RUN(test_relationship_credits_in_out);
    RUN(test_relationship_save_load);
    RUN(test_relationship_anonymous_pubkey_ignored);
}
