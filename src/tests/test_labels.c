/* Tests for inline labelers in shared headers that aren't called by
 * sim code. The functions are reachable from client UI only, so the
 * coverage report shows them as n/a unless something in signal_test
 * calls them directly — which is what this file does. */
#include "tests/test_harness.h"
#include "station_util.h"
#include "signal_model.h"

TEST(test_signal_band_name_thresholds) {
    /* Band boundaries — verify each constant lands in the right name. */
    ASSERT_STR_EQ(signal_band_name(0.0f), "FRONTIER");
    ASSERT_STR_EQ(signal_band_name(SIGNAL_BAND_FRONTIER - 0.001f), "FRONTIER");
    ASSERT_STR_EQ(signal_band_name(SIGNAL_BAND_FRONTIER), "FRINGE");
    ASSERT_STR_EQ(signal_band_name(SIGNAL_BAND_FRINGE - 0.001f), "FRINGE");
    ASSERT_STR_EQ(signal_band_name(SIGNAL_BAND_FRINGE), "OPERATIONAL");
    ASSERT_STR_EQ(signal_band_name(SIGNAL_BAND_OPERATIONAL - 0.001f), "OPERATIONAL");
    ASSERT_STR_EQ(signal_band_name(SIGNAL_BAND_OPERATIONAL), "CORE");
    ASSERT_STR_EQ(signal_band_name(1.0f), "CORE");
}

TEST(test_mining_grade_label_all) {
    ASSERT_STR_EQ(mining_grade_label(MINING_GRADE_COMMON),       "common");
    ASSERT_STR_EQ(mining_grade_label(MINING_GRADE_FINE),         "fine");
    ASSERT_STR_EQ(mining_grade_label(MINING_GRADE_RARE),         "rare");
    ASSERT_STR_EQ(mining_grade_label(MINING_GRADE_RATI),         "RATi");
    ASSERT_STR_EQ(mining_grade_label(MINING_GRADE_COMMISSIONED), "commissioned");
    /* Default branch — invalid grade returns "?". */
    ASSERT_STR_EQ(mining_grade_label((mining_grade_t)99), "?");
}

TEST(test_commodity_short_label_all) {
    ASSERT_STR_EQ(commodity_short_label(COMMODITY_FRAME),         "frames");
    ASSERT_STR_EQ(commodity_short_label(COMMODITY_FERRITE_INGOT), "fe ingots");
    ASSERT_STR_EQ(commodity_short_label(COMMODITY_CUPRITE_INGOT), "cu ingots");
    ASSERT_STR_EQ(commodity_short_label(COMMODITY_CRYSTAL_INGOT), "cr ingots");
    /* Default branch covers ore + module commodities. */
    ASSERT_STR_EQ(commodity_short_label(COMMODITY_FERRITE_ORE), "units");
    ASSERT_STR_EQ(commodity_short_label(COMMODITY_LASER_MODULE), "units");
}

/* Helper: build a station with a single module of type `mt` so the
 * dominant-module priority walk has a deterministic answer. */
static void seed_single_module_station(station_t *st, module_type_t mt) {
    memset(st, 0, sizeof *st);
    st->modules[0].type = mt;
    st->modules[0].ring = 1;
    st->modules[0].slot = 0;
    st->module_count = 1;
}

TEST(test_station_dominant_module_priority) {
    station_t st = {0};

    /* Empty station → MODULE_DOCK fallback. */
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_DOCK);

    /* Single-module stations resolve to that module. */
    seed_single_module_station(&st, MODULE_FRAME_PRESS);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_FRAME_PRESS);
    seed_single_module_station(&st, MODULE_LASER_FAB);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_LASER_FAB);
    seed_single_module_station(&st, MODULE_TRACTOR_FAB);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_TRACTOR_FAB);
    seed_single_module_station(&st, MODULE_FURNACE);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_FURNACE);
    seed_single_module_station(&st, MODULE_FURNACE_CU);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_FURNACE_CU);
    seed_single_module_station(&st, MODULE_FURNACE_CR);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_FURNACE_CR);
    seed_single_module_station(&st, MODULE_SIGNAL_RELAY);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_SIGNAL_RELAY);
    seed_single_module_station(&st, MODULE_HOPPER);
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_HOPPER);

    /* Priority: furnaces beat presses/fabs/relays/hoppers. */
    memset(&st, 0, sizeof st);
    st.modules[0].type = MODULE_HOPPER;
    st.modules[1].type = MODULE_TRACTOR_FAB;
    st.modules[2].type = MODULE_FURNACE_CR;
    st.module_count = 3;
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_FURNACE_CR);

    /* Priority: FRAME_PRESS beats LASER_FAB beats TRACTOR_FAB beats SIGNAL_RELAY. */
    memset(&st, 0, sizeof st);
    st.modules[0].type = MODULE_SIGNAL_RELAY;
    st.modules[1].type = MODULE_TRACTOR_FAB;
    st.modules[2].type = MODULE_LASER_FAB;
    st.modules[3].type = MODULE_FRAME_PRESS;
    st.module_count = 4;
    ASSERT_EQ_INT(station_dominant_module(&st), MODULE_FRAME_PRESS);
}

TEST(test_station_primary_buy_per_dominant_module) {
    station_t st = {0};
    seed_single_module_station(&st, MODULE_FRAME_PRESS);
    ASSERT_EQ_INT(station_primary_buy(&st), COMMODITY_FERRITE_INGOT);
    seed_single_module_station(&st, MODULE_LASER_FAB);
    ASSERT_EQ_INT(station_primary_buy(&st), COMMODITY_CUPRITE_INGOT);
    seed_single_module_station(&st, MODULE_TRACTOR_FAB);
    ASSERT_EQ_INT(station_primary_buy(&st), COMMODITY_CUPRITE_INGOT);
    /* Furnaces don't buy from players (ore arrives via fragment smelting). */
    seed_single_module_station(&st, MODULE_FURNACE);
    ASSERT_EQ_INT((int)station_primary_buy(&st), -1);
    /* No production module → no trade. */
    memset(&st, 0, sizeof st);
    ASSERT_EQ_INT((int)station_primary_buy(&st), -1);
}

TEST(test_station_primary_sell_per_dominant_module) {
    station_t st = {0};
    seed_single_module_station(&st, MODULE_FURNACE);
    ASSERT_EQ_INT(station_primary_sell(&st), COMMODITY_FERRITE_INGOT);
    seed_single_module_station(&st, MODULE_FURNACE_CU);
    ASSERT_EQ_INT(station_primary_sell(&st), COMMODITY_CUPRITE_INGOT);
    seed_single_module_station(&st, MODULE_FURNACE_CR);
    ASSERT_EQ_INT(station_primary_sell(&st), COMMODITY_CRYSTAL_INGOT);
    seed_single_module_station(&st, MODULE_FRAME_PRESS);
    ASSERT_EQ_INT(station_primary_sell(&st), COMMODITY_FRAME);
    seed_single_module_station(&st, MODULE_LASER_FAB);
    ASSERT_EQ_INT(station_primary_sell(&st), COMMODITY_LASER_MODULE);
    seed_single_module_station(&st, MODULE_TRACTOR_FAB);
    ASSERT_EQ_INT(station_primary_sell(&st), COMMODITY_TRACTOR_MODULE);
    seed_single_module_station(&st, MODULE_SIGNAL_RELAY);
    ASSERT_EQ_INT((int)station_primary_sell(&st), -1);
    memset(&st, 0, sizeof st);
    ASSERT_EQ_INT((int)station_primary_sell(&st), -1);
}

TEST(test_producer_module_for_commodity) {
    /* Pure switch — pin every branch so refactors of module priority
     * can't silently swap which furnace makes which ingot. */
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_FRAME),         MODULE_FRAME_PRESS);
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_FERRITE_INGOT), MODULE_FURNACE);
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_CUPRITE_INGOT), MODULE_FURNACE_CU);
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_CRYSTAL_INGOT), MODULE_FURNACE_CR);
    /* Default branch — raw ore inputs and module commodities have no
     * direct producer module; shipyard_intake_rate falls back to a
     * trickle when this returns MODULE_COUNT. */
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_FERRITE_ORE),    MODULE_COUNT);
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_LASER_MODULE),   MODULE_COUNT);
    ASSERT_EQ_INT(producer_module_for_commodity(COMMODITY_TRACTOR_MODULE), MODULE_COUNT);
}

void register_label_tests(void) {
    TEST_SECTION("\nShared header labelers:\n");
    RUN(test_signal_band_name_thresholds);
    RUN(test_mining_grade_label_all);
    RUN(test_commodity_short_label_all);
    RUN(test_station_dominant_module_priority);
    RUN(test_station_primary_buy_per_dominant_module);
    RUN(test_station_primary_sell_per_dominant_module);
    RUN(test_producer_module_for_commodity);
}
