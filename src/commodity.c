#include <stddef.h>
#include "commodity.h"

commodity_t commodity_ore_form(commodity_t commodity) {
    switch (commodity) {
        case COMMODITY_FERRITE_INGOT:     return COMMODITY_FERRITE_ORE;
        case COMMODITY_CUPRITE_INGOT:     return COMMODITY_CUPRITE_ORE;
        case COMMODITY_CRYSTAL_INGOT:     return COMMODITY_CRYSTAL_ORE;
        case COMMODITY_FRAME:             return COMMODITY_FERRITE_ORE;
        case COMMODITY_LASER_MODULE:      return COMMODITY_CUPRITE_ORE;
        case COMMODITY_TRACTOR_MODULE:    return COMMODITY_CRYSTAL_ORE;
        default:                          return commodity;
    }
}

commodity_t commodity_refined_form(commodity_t commodity) {
    switch (commodity) {
        case COMMODITY_FERRITE_ORE:
            return COMMODITY_FERRITE_INGOT;
        case COMMODITY_CUPRITE_ORE:
            return COMMODITY_CUPRITE_INGOT;
        case COMMODITY_CRYSTAL_ORE:
            return COMMODITY_CRYSTAL_INGOT;
        case COMMODITY_FERRITE_INGOT:
        case COMMODITY_CUPRITE_INGOT:
        case COMMODITY_CRYSTAL_INGOT:
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
        case COMMODITY_FERRITE_INGOT:
            return "Ferrite Ingots";
        case COMMODITY_CUPRITE_INGOT:
            return "Cuprite Ingots";
        case COMMODITY_CRYSTAL_INGOT:
            return "Crystal Ingots";
        case COMMODITY_FRAME:
            return "Frames";
        case COMMODITY_LASER_MODULE:
            return "Laser Modules";
        case COMMODITY_TRACTOR_MODULE:
            return "Tractor Modules";
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
        case COMMODITY_FERRITE_INGOT:
            return "FR";
        case COMMODITY_CUPRITE_INGOT:
            return "CO";
        case COMMODITY_CRYSTAL_INGOT:
            return "LN";
        case COMMODITY_FRAME:
            return "FM";
        case COMMODITY_LASER_MODULE:
            return "LM";
        case COMMODITY_TRACTOR_MODULE:
            return "TM";
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
        case COMMODITY_FERRITE_INGOT:
            return "FE Ingot";
        case COMMODITY_CUPRITE_INGOT:
            return "CU Ingot";
        case COMMODITY_CRYSTAL_INGOT:
            return "CR Ingot";
        case COMMODITY_FRAME:
            return "Frame";
        case COMMODITY_LASER_MODULE:
            return "Laser Mod";
        case COMMODITY_TRACTOR_MODULE:
            return "Tractor Mod";
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
    if (!station) return 0.0f;
    float base = station->buy_price[commodity];
    if (base < 0.01f) return 0.0f;
    /* Dynamic pricing: price curves up to 2× base as stock empties.
     * Uses squared deficit so price stays near base until stock gets low.
     * Full=1×, half=1.25×, quarter=1.56×, empty=2×. */
    float capacity = (commodity < COMMODITY_RAW_ORE_COUNT)
        ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
    float fill = station->inventory[commodity] / capacity;
    if (fill > 1.0f) fill = 1.0f;
    float deficit = 1.0f - fill;
    return base * (1.0f + deficit * deficit);
}

float station_inventory_amount(const station_t* station, commodity_t commodity) {
    return station != NULL ? station->inventory[commodity] : 0.0f;
}
