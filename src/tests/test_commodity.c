#include "tests/test_harness.h"

TEST(test_refined_form_mapping) {
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_FERRITE_ORE), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CUPRITE_ORE), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CRYSTAL_ORE), COMMODITY_CRYSTAL_INGOT);
}

TEST(test_refined_form_ingots_return_self) {
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_FERRITE_INGOT), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CUPRITE_INGOT), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(commodity_refined_form(COMMODITY_CRYSTAL_INGOT), COMMODITY_CRYSTAL_INGOT);
}

TEST(test_commodity_name) {
    /* Hit every branch — the labeler is touched by HUD code only, so
     * tests are the only way it exercises each case in coverage. */
    ASSERT_STR_EQ(commodity_name(COMMODITY_FERRITE_ORE), "Ferrite Ore");
    ASSERT_STR_EQ(commodity_name(COMMODITY_CUPRITE_ORE), "Cuprite Ore");
    ASSERT_STR_EQ(commodity_name(COMMODITY_CRYSTAL_ORE), "Crystal Ore");
    ASSERT_STR_EQ(commodity_name(COMMODITY_FERRITE_INGOT), "Ferrite Ingots");
    ASSERT_STR_EQ(commodity_name(COMMODITY_CUPRITE_INGOT), "Cuprite Ingots");
    ASSERT_STR_EQ(commodity_name(COMMODITY_CRYSTAL_INGOT), "Crystal Ingots");
    ASSERT_STR_EQ(commodity_name(COMMODITY_FRAME), "Frames");
    ASSERT_STR_EQ(commodity_name(COMMODITY_LASER_MODULE), "Laser Modules");
    ASSERT_STR_EQ(commodity_name(COMMODITY_TRACTOR_MODULE), "Tractor Modules");
    ASSERT_STR_EQ(commodity_name(COMMODITY_COUNT), "Cargo");
}

TEST(test_commodity_code) {
    ASSERT_STR_EQ(commodity_code(COMMODITY_FERRITE_ORE), "FE");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CUPRITE_ORE), "CU");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CRYSTAL_ORE), "CR");
    ASSERT_STR_EQ(commodity_code(COMMODITY_FERRITE_INGOT), "FR");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CUPRITE_INGOT), "CO");
    ASSERT_STR_EQ(commodity_code(COMMODITY_CRYSTAL_INGOT), "LN");
}

TEST(test_commodity_short_name) {
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_FERRITE_ORE), "Ferrite");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_CUPRITE_ORE), "Cuprite");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_CRYSTAL_ORE), "Crystal");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_FERRITE_INGOT), "FE Ingot");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_CUPRITE_INGOT), "CU Ingot");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_CRYSTAL_INGOT), "CR Ingot");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_FRAME), "Frame");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_LASER_MODULE), "Laser Mod");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_TRACTOR_MODULE), "Tractor Mod");
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_COUNT), "Unknown");
}

TEST(test_commodity_ore_form) {
    /* Ingots and fab products map back to their source ore. Already-ore
     * commodities and the COUNT sentinel pass through unchanged. */
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_FERRITE_INGOT), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_CUPRITE_INGOT), COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_CRYSTAL_INGOT), COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_FRAME), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_LASER_MODULE), COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_TRACTOR_MODULE), COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_FERRITE_ORE), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_CUPRITE_ORE), COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_CRYSTAL_ORE), COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_INT(commodity_ore_form(COMMODITY_COUNT), COMMODITY_COUNT);
}

TEST(test_commodity_code_full) {
    ASSERT_STR_EQ(commodity_code(COMMODITY_FRAME), "FM");
    ASSERT_STR_EQ(commodity_code(COMMODITY_LASER_MODULE), "LM");
    ASSERT_STR_EQ(commodity_code(COMMODITY_TRACTOR_MODULE), "TM");
    ASSERT_STR_EQ(commodity_code(COMMODITY_COUNT), "--");
}

TEST(test_commodity_color_u8_all_branches) {
    /* The HUD color ladder — exercising every branch keeps the renderer
     * fallback honest so a new commodity can't silently inherit white. */
    uint8_t r, g, b;
    commodity_color_u8(COMMODITY_FERRITE_ORE, &r, &g, &b);
    ASSERT_EQ_INT(r, 217); ASSERT_EQ_INT(g, 127); ASSERT_EQ_INT(b, 90);
    commodity_color_u8(COMMODITY_CUPRITE_ORE, &r, &g, &b);
    ASSERT_EQ_INT(r, 110); ASSERT_EQ_INT(g, 210); ASSERT_EQ_INT(b, 140);
    commodity_color_u8(COMMODITY_CRYSTAL_ORE, &r, &g, &b);
    ASSERT_EQ_INT(r, 180); ASSERT_EQ_INT(g, 140); ASSERT_EQ_INT(b, 255);
    commodity_color_u8(COMMODITY_FERRITE_INGOT, &r, &g, &b);
    ASSERT_EQ_INT(r, 217);
    commodity_color_u8(COMMODITY_CUPRITE_INGOT, &r, &g, &b);
    ASSERT_EQ_INT(r, 110);
    commodity_color_u8(COMMODITY_CRYSTAL_INGOT, &r, &g, &b);
    ASSERT_EQ_INT(r, 180);
    commodity_color_u8(COMMODITY_FRAME, &r, &g, &b);
    ASSERT_EQ_INT(r, 190);
    commodity_color_u8(COMMODITY_LASER_MODULE, &r, &g, &b);
    ASSERT_EQ_INT(r, 140);
    commodity_color_u8(COMMODITY_TRACTOR_MODULE, &r, &g, &b);
    ASSERT_EQ_INT(r, 120);
    commodity_color_u8(COMMODITY_COUNT, &r, &g, &b);
    ASSERT_EQ_INT(r, 200); ASSERT_EQ_INT(g, 220); ASSERT_EQ_INT(b, 230);
}

TEST(test_ship_total_cargo) {
    ship_t ship = {0};
    ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    ship.cargo[COMMODITY_FERRITE_INGOT] = 5.0f;
    ASSERT_EQ_FLOAT(ship_total_cargo(&ship), 15.0f, 0.01f);
}

TEST(test_ship_cargo_amount) {
    ship_t ship = {0};
    ship.cargo[COMMODITY_CUPRITE_ORE] = 42.0f;
    ASSERT_EQ_FLOAT(ship_cargo_amount(&ship, COMMODITY_CUPRITE_ORE), 42.0f, 0.01f);
    ASSERT_EQ_FLOAT(ship_cargo_amount(&ship, COMMODITY_FERRITE_ORE), 0.0f, 0.01f);
}

TEST(test_station_buy_price) {
    station_t station = {0};
    station.base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    station.base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    /* Empty hopper = 1× base (station pays full price to attract sellers) */
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_FERRITE_ORE), 10.0f, 0.01f);
    /* Full hopper = 0.5× base (overstocked, pays less) */
    station._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_FERRITE_ORE), 5.0f, 0.01f);
    /* Half full: 1 - 0.5*0.5 = 0.75× base */
    station._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.5f;
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_FERRITE_ORE), 7.5f, 0.01f);
    ASSERT_EQ_FLOAT(station_buy_price(NULL, COMMODITY_FERRITE_ORE), 0.0f, 0.01f);
    /* Sell price: empty = 2× base, full = 1× base */
    station._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_sell_price(&station, COMMODITY_FERRITE_ORE), 20.0f, 0.01f);
    station._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_sell_price(&station, COMMODITY_FERRITE_ORE), 10.0f, 0.01f);
}

TEST(test_station_inventory_amount) {
    station_t station = {0};
    station._inventory_cache[COMMODITY_FERRITE_INGOT] = 25.0f;
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FERRITE_INGOT), 25.0f, 0.01f);
    ASSERT_EQ_FLOAT(station_inventory_amount(NULL, COMMODITY_FERRITE_INGOT), 0.0f, 0.01f);
}

void register_commodity_tests(void) {
    TEST_SECTION("Commodity tests:\n");
    RUN(test_refined_form_mapping);
    RUN(test_refined_form_ingots_return_self);
    RUN(test_commodity_name);
    RUN(test_commodity_code);
    RUN(test_commodity_short_name);
    RUN(test_commodity_ore_form);
    RUN(test_commodity_code_full);
    RUN(test_commodity_color_u8_all_branches);
    RUN(test_ship_total_cargo);
    RUN(test_ship_cargo_amount);
    RUN(test_station_buy_price);
    RUN(test_station_inventory_amount);
}
