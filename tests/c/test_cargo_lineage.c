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
    /* Indices >= SIGNAL_FIRST_OUTPOST_INDEX are player-built outposts. They get a generic
     * "Outpost N" tag — distinct enough that two outposts in the
     * lineage display don't read as the same place. */
    const char *o4 = station_short_name(SIGNAL_FIRST_OUTPOST_INDEX);
    ASSERT(o4 != NULL);
    ASSERT(strstr(o4, "Outpost") != NULL);
    ASSERT(strstr(o4, "4") != NULL);

    const char *o63 = station_short_name(63);
    ASSERT(o63 != NULL);
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

    char serial[12], parent[8], origin[24], story[96];
    cargo_lineage_serial_label(&unit, serial, sizeof(serial));
    cargo_lineage_parent_label(&unit, parent, sizeof(parent));
    cargo_lineage_origin_label(&unit, origin, sizeof(origin));
    cargo_lineage_story_label(&unit, story, sizeof(story));

    ASSERT(strlen(serial) > 0);
    ASSERT(strlen(parent) > 0);
    ASSERT_STR_EQ(origin, "Kepler");
    ASSERT_STR_EQ(cargo_lineage_recipe_label(&unit), "smelt");
    ASSERT_STR_EQ(story, "smelted at Kepler from fragment ep 4422");
}

TEST(test_cargo_lineage_labels_legacy_parentless) {
    uint8_t origin_seed[8] = {'T','E','S','T','v','1',0,0};
    cargo_unit_t unit = {0};
    ASSERT(hash_legacy_migrate_unit(origin_seed, COMMODITY_FRAME, 3, &unit));
    unit.origin_station = 2;

    char parent[8], origin[24], story[96];
    cargo_lineage_parent_label(&unit, parent, sizeof(parent));
    cargo_lineage_origin_label(&unit, origin, sizeof(origin));
    cargo_lineage_story_label(&unit, story, sizeof(story));

    ASSERT_STR_EQ(parent, "none");
    ASSERT_STR_EQ(origin, "Helios");
    ASSERT_STR_EQ(cargo_lineage_recipe_label(&unit), "legacy");
    ASSERT_STR_EQ(story, "legacy cargo at Helios");
}

TEST(test_cargo_lineage_story_does_not_claim_product_input_proof) {
    uint8_t fragment[32] = {0};
    for (int i = 0; i < 32; i++) fragment[i] = (uint8_t)(0x40 + i);

    cargo_unit_t input = {0};
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                      fragment, 1, &input));

    cargo_unit_t product = {0};
    ASSERT(hash_product(RECIPE_FRAME_BASIC, &input, 1, 0, &product));
    product.origin_station = 0;

    char story[96];
    cargo_lineage_story_label(&product, story, sizeof(story));
    ASSERT_STR_EQ(
        story,
        "pressed at Prospect (input lineage unproven)");
}

void register_cargo_lineage_tests(void);
void register_cargo_lineage_tests(void) {
    TEST_SECTION("\nCargo lineage display:\n");
    RUN(test_station_short_name_founders);
    RUN(test_station_short_name_outposts);
    RUN(test_station_short_name_invalid_returns_sentinel);
    RUN(test_cargo_lineage_labels_ingot);
    RUN(test_cargo_lineage_labels_legacy_parentless);
    RUN(test_cargo_lineage_story_does_not_claim_product_input_proof);
}
