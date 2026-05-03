/*
 * test_cargo_lineage.c — coverage for the lineage display layer added in
 * the cargo-lineage-display PR.
 *
 * The trade-row UI (src/station_ui.c) is client-only and not linked
 * into signal_test, so build_trade_rows itself can't be unit-tested
 * here — the lineage population is exercised end-to-end by playing
 * the docked UI. What we *can* test in isolation is station_short_name,
 * which lives in shared/station_util and is the helper the row
 * renderer uses to format the "from <station>" suffix.
 */
#include "tests/test_harness.h"
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

void register_cargo_lineage_tests(void);
void register_cargo_lineage_tests(void) {
    TEST_SECTION("\nCargo lineage display:\n");
    RUN(test_station_short_name_founders);
    RUN(test_station_short_name_outposts);
    RUN(test_station_short_name_invalid_returns_sentinel);
}
