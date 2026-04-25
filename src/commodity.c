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
        case COMMODITY_REPAIR_KIT:
            return "Repair Kits";
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
        case COMMODITY_REPAIR_KIT:
            return "RK";
        case COMMODITY_COUNT:
        default:
            return "--";
    }
}

void commodity_color_u8(commodity_t commodity, uint8_t *r, uint8_t *g, uint8_t *b) {
    /* Warm-to-cool ladder for the three ores (matches the color dot on
     * the asteroid and the tow tether so the TRADE row reads the same).
     * Fabricated goods get their own signature so the market scans
     * quickly: frame = slate, laser = nav-blue, tractor = mint. */
    switch (commodity) {
    case COMMODITY_FERRITE_ORE:
    case COMMODITY_FERRITE_INGOT:
        *r = 217; *g = 127; *b =  90; return;  /* warm ferrite amber */
    case COMMODITY_CUPRITE_ORE:
    case COMMODITY_CUPRITE_INGOT:
        *r = 110; *g = 210; *b = 140; return;  /* cuprite oxide green */
    case COMMODITY_CRYSTAL_ORE:
    case COMMODITY_CRYSTAL_INGOT:
        *r = 180; *g = 140; *b = 255; return;  /* crystal violet */
    case COMMODITY_FRAME:
        *r = 190; *g = 200; *b = 220; return;  /* cool slate */
    case COMMODITY_LASER_MODULE:
        *r = 140; *g = 180; *b = 255; return;  /* nav blue */
    case COMMODITY_TRACTOR_MODULE:
        *r = 120; *g = 235; *b = 200; return;  /* mint */
    case COMMODITY_REPAIR_KIT:
        *r = 240; *g =  90; *b =  90; return;  /* med-cross red — repair signal */
    case COMMODITY_COUNT:
    default:
        *r = 200; *g = 220; *b = 230; return;  /* fallback cool white */
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
        case COMMODITY_REPAIR_KIT:
            return "Repair Kit";
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

float ship_cargo_amount(const ship_t* ship, commodity_t commodity) {
    return ship->cargo[commodity];
}

/* Price the station pays when BUYING from the player (ore, deliveries).
 * Scales down from base as station gets overstocked.
 * Empty=1× base, full=0.5× base. */
float station_buy_price(const station_t* station, commodity_t commodity) {
    if (!station) return 0.0f;
    float base = station->base_price[commodity];
    if (base < FLOAT_EPSILON) return 0.0f;
    float capacity = (commodity < COMMODITY_RAW_ORE_COUNT)
        ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
    float fill = station->inventory[commodity] / capacity;
    if (fill > 1.0f) fill = 1.0f;
    /* Buy cheaper when overstocked: 1.0× at empty, 0.5× at full */
    return base * (1.0f - fill * 0.5f);
}

/* Price the station charges when SELLING to the player (ingots, products).
 * Scales up from base as stock depletes.
 * Full=1× base, empty=2× base. */
float station_sell_price(const station_t* station, commodity_t commodity) {
    if (!station) return 0.0f;
    float base = station->base_price[commodity];
    if (base < FLOAT_EPSILON) return 0.0f;
    float capacity = (commodity < COMMODITY_RAW_ORE_COUNT)
        ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
    float fill = station->inventory[commodity] / capacity;
    if (fill > 1.0f) fill = 1.0f;
    float deficit = 1.0f - fill;
    /* Sell expensive when scarce: 1× at full, 2× at empty */
    return base * (1.0f + deficit * deficit);
}

float station_inventory_amount(const station_t* station, commodity_t commodity) {
    return station != NULL ? station->inventory[commodity] : 0.0f;
}
