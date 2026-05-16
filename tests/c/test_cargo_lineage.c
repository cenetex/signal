/*
 * test_cargo_lineage.c — coverage for the lineage display layer added in
 * the cargo-lineage-display PR.
 *
 * The trade-row UI (client/station_ui.c) is client-only and not linked
 * into signal_test, so build_trade_rows itself can't be unit-tested
 * here — the lineage population is exercised end-to-end by playing
 * the docked UI. What we *can* test in isolation is station_short_name,
 * which lives in shared/station_util and is the helper the row
 * renderer uses to format the "from <station>" suffix.
 */
#include "test_harness.h"
#include "cargo_lineage.h"
#include "station_util.h"

TEST(test_station_short_name_founders) {
    /* The three founding stations have stable, well-known short names
     * matching the in-fiction identity. These are surfaced in dock UI
     * lineage tags ("from Prospect, ep 4422") and worth pinning so a
     * rename here doesn't silently change the player-facing display. */
    ASSERT_STR_EQ(station_short_name(0), "Prospect");
    ASSERT_STR_EQ(station_short_name(1), "Kepler");
    ASSERT_STR_EQ(station_short_name(2), "Helios");
}

TEST(test_station_short_name_outposts) {
    /* Indices >= 3 are player-built outposts. They get a generic
     * "Outpost N" tag — distinct enough that two outposts in the
     * lineage display don't read as the same place. */
    const char *o3 = station_short_name(3);
    const char *o63 = station_short_name(63);

    ASSERT(o3 != NULL && o63 != NULL);
    /* Each should contain "Outpost" and the index. */
    ASSERT(strstr(o3, "Outpost") != NULL);
    ASSERT(strstr(o3, "3") != NULL);
    ASSERT(strstr(o63, "Outpost") != NULL);
    ASSERT(strstr(o63, "63") != NULL);
}

TEST(test_station_short_name_invalid_returns_sentinel) {
    /* Negative or out-of-range indices fall through to a sentinel.
     * Caller code that drops in a malformed origin_station byte
     * (corrupt save, garbled wire packet) should still render
     * SOMETHING rather than crash. */
    ASSERT_STR_EQ(station_short_name(-1), "?");
    ASSERT_STR_EQ(station_short_name(MAX_STATIONS), "?");
    ASSERT_STR_EQ(station_short_name(MAX_STATIONS + 100), "?");
}

TEST(test_cargo_lineage_labels_ingot) {
    uint8_t fragment[32] = {0};
    for (int i = 0; i < 32; i++) fragment[i] = (uint8_t)(0x20 + i);

    cargo_unit_t unit = {0};
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment, 2, &unit));
    unit.origin_station = 1;
    unit.mined_block = 4422;

    char serial[12], parent[8], origin[24];
    cargo_lineage_serial_label(&unit, serial, sizeof(serial));
    cargo_lineage_parent_label(&unit, parent, sizeof(parent));
    cargo_lineage_origin_label(&unit, origin, sizeof(origin));

    ASSERT(strlen(serial) > 0);
    ASSERT(strlen(parent) > 0);
    ASSERT_STR_EQ(origin, "Kepler");
    ASSERT_STR_EQ(cargo_lineage_recipe_label(&unit), "smelt");
}

TEST(test_cargo_lineage_labels_legacy_parentless) {
    uint8_t origin_seed[8] = {'T','E','S','T','v','1',0,0};
    cargo_unit_t unit = {0};
    ASSERT(hash_legacy_migrate_unit(origin_seed, COMMODITY_FRAME, 3, &unit));
    unit.origin_station = 2;

    char parent[8], origin[24];
    cargo_lineage_parent_label(&unit, parent, sizeof(parent));
    cargo_lineage_origin_label(&unit, origin, sizeof(origin));

    ASSERT_STR_EQ(parent, "none");
    ASSERT_STR_EQ(origin, "Helios");
    ASSERT_STR_EQ(cargo_lineage_recipe_label(&unit), "legacy");
}

void register_cargo_lineage_tests(void);
void register_cargo_lineage_tests(void) {
    TEST_SECTION("\nCargo lineage display:\n");
    RUN(test_station_short_name_founders);
    RUN(test_station_short_name_outposts);
    RUN(test_station_short_name_invalid_returns_sentinel);
    RUN(test_cargo_lineage_labels_ingot);
    RUN(test_cargo_lineage_labels_legacy_parentless);
}
