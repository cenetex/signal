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
    /* Resource-family ladder: ore is full resource color; ingots are that
     * resource blended with gray metal; products are mostly metal with a
     * source-resource accent. Keep this in sync with src/palette.h. */
    switch (commodity) {
    case COMMODITY_FERRITE_ORE:
        *r = 217; *g =  77; *b =  51; return;
    case COMMODITY_CUPRITE_ORE:
        *r =  64; *g = 128; *b = 230; return;
    case COMMODITY_CRYSTAL_ORE:
        *r =  77; *g = 204; *b =  89; return;
    case COMMODITY_FERRITE_INGOT:
        *r = 199; *g = 112; *b =  94; return;
    case COMMODITY_CUPRITE_INGOT:
        *r = 107; *g = 150; *b = 207; return;
    case COMMODITY_CRYSTAL_INGOT:
        *r = 112; *g = 191; *b = 120; return;
    case COMMODITY_FRAME:
        *r = 163; *g = 135; *b = 135; return;
    case COMMODITY_LASER_MODULE:
        *r = 135; *g = 156; *b = 191; return;
    case COMMODITY_TRACTOR_MODULE:
        *r = 130; *g = 176; *b = 143; return;
    case COMMODITY_REPAIR_KIT:
        *r = 184; *g = 140; *b = 140; return;
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

float commodity_volume(commodity_t commodity) {
    if (commodity == COMMODITY_REPAIR_KIT) return REPAIR_KIT_CARGO_DENSITY;
    return 1.0f;
}

float ship_total_cargo(const ship_t* ship) {
    float total = 0.0f;
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        total += ship->cargo[i] * commodity_volume((commodity_t)i);
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
    float fill = station->_inventory_cache[commodity] / capacity;
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
    float fill = station->_inventory_cache[commodity] / capacity;
    if (fill > 1.0f) fill = 1.0f;
    float deficit = 1.0f - fill;
    /* Sell expensive when scarce: 1× at full, 2× at empty */
    return base * (1.0f + deficit * deficit);
}

float station_inventory_amount(const station_t* station, commodity_t commodity) {
    return station != NULL ? station->_inventory_cache[commodity] : 0.0f;
}

/* Unit-aware variants: same dynamic stock curve as above, scaled by
 * the unit's prefix_class. See PREFIX_CLASS_PRICE_MULTIPLIER in
 * shared/economy_const.h for the table and rationale.
 *
 * Implementation pattern: chain through the commodity-only function
 * so the stock-fill curve and base_price logic stay the single source
 * of truth. The only thing layered on top is the prefix multiplier. */
float station_buy_price_unit(const station_t* station, const cargo_unit_t* unit) {
    if (!station || !unit) return 0.0f;
    float base = station_buy_price(station, (commodity_t)unit->commodity);
    return base * prefix_class_price_multiplier((int)unit->prefix_class);
}

float station_sell_price_unit(const station_t* station, const cargo_unit_t* unit) {
    if (!station || !unit) return 0.0f;
    float base = station_sell_price(station, (commodity_t)unit->commodity);
    return base * prefix_class_price_multiplier((int)unit->prefix_class);
}
