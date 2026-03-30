#include <stddef.h>
#include "commodity.h"

commodity_t commodity_refined_form(commodity_t commodity) {
    switch (commodity) {
        case COMMODITY_FERRITE_ORE:
            return COMMODITY_FRAME_INGOT;
        case COMMODITY_CUPRITE_ORE:
            return COMMODITY_CONDUCTOR_INGOT;
        case COMMODITY_CRYSTAL_ORE:
            return COMMODITY_LENS_INGOT;
        case COMMODITY_FRAME_INGOT:
        case COMMODITY_CONDUCTOR_INGOT:
        case COMMODITY_LENS_INGOT:
        case COMMODITY_COUNT:
        default:
            return commodity;
    }
}

const char* commodity_name(commodity_t commodity) {
    switch (commodity) {
        case COMMODITY_FERRITE_ORE:
            return "Ferrite Ore";
        case COMMODITY_CUPRITE_ORE:
            return "Cuprite Ore";
        case COMMODITY_CRYSTAL_ORE:
            return "Crystal Ore";
        case COMMODITY_FRAME_INGOT:
            return "Frame Ingots";
        case COMMODITY_CONDUCTOR_INGOT:
            return "Conductor Ingots";
        case COMMODITY_LENS_INGOT:
            return "Lens Ingots";
        case COMMODITY_COUNT:
        default:
            return "Cargo";
    }
}

const char* commodity_code(commodity_t commodity) {
    switch (commodity) {
        case COMMODITY_FERRITE_ORE:
            return "FE";
        case COMMODITY_CUPRITE_ORE:
            return "CU";
        case COMMODITY_CRYSTAL_ORE:
            return "CR";
        case COMMODITY_FRAME_INGOT:
            return "FR";
        case COMMODITY_CONDUCTOR_INGOT:
            return "CO";
        case COMMODITY_LENS_INGOT:
            return "LN";
        case COMMODITY_COUNT:
        default:
            return "--";
    }
}

const char* commodity_short_name(commodity_t commodity) {
    switch (commodity) {
        case COMMODITY_FERRITE_ORE:
            return "Ferrite";
        case COMMODITY_CUPRITE_ORE:
            return "Cuprite";
        case COMMODITY_CRYSTAL_ORE:
            return "Crystal";
        case COMMODITY_FRAME_INGOT:
            return "Frame";
        case COMMODITY_CONDUCTOR_INGOT:
            return "Conductor";
        case COMMODITY_LENS_INGOT:
            return "Lens";
        case COMMODITY_COUNT:
        default:
            return "Unknown";
    }
}

float ship_total_cargo(const ship_t* ship) {
    float total = 0.0f;
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        total += ship->cargo[i];
    }
    return total;
}

float ship_raw_ore_total(const ship_t* ship) {
    float total = 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
        total += ship->cargo[i];
    }
    return total;
}

float ship_cargo_amount(const ship_t* ship, commodity_t commodity) {
    return ship->cargo[commodity];
}

float station_buy_price(const station_t* station, commodity_t commodity) {
    return station != NULL ? station->buy_price[commodity] : 0.0f;
}

float station_inventory_amount(const station_t* station, commodity_t commodity) {
    return station != NULL ? station->inventory[commodity] : 0.0f;
}
