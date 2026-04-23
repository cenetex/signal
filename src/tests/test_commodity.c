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
    ASSERT_STR_EQ(commodity_name(COMMODITY_FERRITE_ORE), "Ferrite Ore");
    ASSERT_STR_EQ(commodity_name(COMMODITY_FERRITE_INGOT), "Ferrite Ingots");
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
    ASSERT_STR_EQ(commodity_short_name(COMMODITY_FERRITE_INGOT), "FE Ingot");
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
    station.inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_FERRITE_ORE), 5.0f, 0.01f);
    /* Half full: 1 - 0.5*0.5 = 0.75× base */
    station.inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.5f;
    ASSERT_EQ_FLOAT(station_buy_price(&station, COMMODITY_FERRITE_ORE), 7.5f, 0.01f);
    ASSERT_EQ_FLOAT(station_buy_price(NULL, COMMODITY_FERRITE_ORE), 0.0f, 0.01f);
    /* Sell price: empty = 2× base, full = 1× base */
    station.inventory[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_sell_price(&station, COMMODITY_FERRITE_ORE), 20.0f, 0.01f);
    station.inventory[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_sell_price(&station, COMMODITY_FERRITE_ORE), 10.0f, 0.01f);
}

TEST(test_station_inventory_amount) {
    station_t station = {0};
    station.inventory[COMMODITY_FERRITE_INGOT] = 25.0f;
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FERRITE_INGOT), 25.0f, 0.01f);
    ASSERT_EQ_FLOAT(station_inventory_amount(NULL, COMMODITY_FERRITE_INGOT), 0.0f, 0.01f);
}

void register_commodity_tests(void) {
    printf("Commodity tests:\n");
    RUN(test_refined_form_mapping);
    RUN(test_refined_form_ingots_return_self);
    RUN(test_commodity_name);
    RUN(test_commodity_code);
    RUN(test_commodity_short_name);
    RUN(test_ship_total_cargo);
    RUN(test_ship_cargo_amount);
    RUN(test_station_buy_price);
    RUN(test_station_inventory_amount);
}
