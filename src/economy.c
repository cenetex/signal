#include <math.h>
#include <stddef.h>
#include "economy.h"

/* Check if station can smelt a specific ore type */
static bool can_smelt_ore(const station_t* station, commodity_t ore) {
    switch (ore) {
        case COMMODITY_FERRITE_ORE: return station_has_module(station, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE: return station_has_module(station, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE: return station_has_module(station, MODULE_FURNACE_CR);
        default: return false;
    }
}

void step_refinery_production(station_t* stations, int count, float dt) {
    for (int s = 0; s < count; s++) {
        station_t* station = &stations[s];
        /* Need at least one furnace type */
        if (!station_has_module(station, MODULE_FURNACE)
            && !station_has_module(station, MODULE_FURNACE_CU)
            && !station_has_module(station, MODULE_FURNACE_CR)) continue;

        int active = 0;
        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            if (station->inventory[i] > FLOAT_EPSILON && can_smelt_ore(station, (commodity_t)i)) active++;
        }
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;

        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            commodity_t ore = (commodity_t)i;
            if (!can_smelt_ore(station, ore)) continue;
            if (station->inventory[ore] <= FLOAT_EPSILON) continue;
            float consume = fminf(station->inventory[ore], rate * dt);
            station->inventory[ore] -= consume;
            station->inventory[commodity_refined_form(ore)] += consume;
        }
    }
}

void step_station_production(station_t* stations, int count, float dt) {
    for (int s = 0; s < count; s++) {
        station_t* station = &stations[s];

        if (station_has_module(station, MODULE_FRAME_PRESS)) {
            if (station->inventory[COMMODITY_FRAME] < MAX_PRODUCT_STOCK) {
                float buf = station->inventory[COMMODITY_FERRITE_INGOT];
                if (buf > FLOAT_EPSILON) {
                    float room = MAX_PRODUCT_STOCK - station->inventory[COMMODITY_FRAME];
                    float consume = fminf(buf, fminf(STATION_PRODUCTION_RATE * dt, room));
                    station->inventory[COMMODITY_FERRITE_INGOT] -= consume;
                    station->inventory[COMMODITY_FRAME] += consume;
                }
            }
        }
        if (station_has_module(station, MODULE_LASER_FAB)) {
            if (station->inventory[COMMODITY_LASER_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_co = station->inventory[COMMODITY_CUPRITE_INGOT];
                if (buf_co > FLOAT_EPSILON) {
                    float room = MAX_PRODUCT_STOCK - station->inventory[COMMODITY_LASER_MODULE];
                    float consume = fminf(buf_co, fminf(STATION_PRODUCTION_RATE * dt, room));
                    station->inventory[COMMODITY_CUPRITE_INGOT] -= consume;
                    station->inventory[COMMODITY_LASER_MODULE] += consume;
                }
            }
        }
        if (station_has_module(station, MODULE_TRACTOR_FAB)) {
            if (station->inventory[COMMODITY_TRACTOR_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_ln = station->inventory[COMMODITY_CRYSTAL_INGOT];
                if (buf_ln > FLOAT_EPSILON) {
                    float room = MAX_PRODUCT_STOCK - station->inventory[COMMODITY_TRACTOR_MODULE];
                    float consume = fminf(buf_ln, fminf(STATION_PRODUCTION_RATE * dt, room));
                    station->inventory[COMMODITY_CRYSTAL_INGOT] -= consume;
                    station->inventory[COMMODITY_TRACTOR_MODULE] += consume;
                }
            }
        }
    }
}

float station_cargo_sale_value(const ship_t* ship, const station_t* station) {
    if (!station) return 0.0f;
    commodity_t buy = station_primary_buy(station);
    if ((int)buy < 0) return 0.0f;
    float held = ship_cargo_amount(ship, buy);
    if (held <= FLOAT_EPSILON) return 0.0f;
    float capacity = (buy < COMMODITY_RAW_ORE_COUNT)
        ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
    float space = capacity - station->inventory[buy];
    if (space < 0.0f) space = 0.0f;
    float sellable = fminf(held, space);
    return sellable * station_buy_price(station, buy);
}

float station_repair_cost(const ship_t* ship, const station_t* station) {
    if (!station) return 0.0f;
    if (!(station->services & STATION_SERVICE_REPAIR)) return 0.0f;
    float damage = ship_max_hull(ship) - ship->hull;
    if (damage <= 0.0f) return 0.0f;
    return damage * STATION_REPAIR_COST_PER_HULL;
}

bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, uint32_t service, int credit_cost) {
    if (!station || !(station->services & service)) return false;
    if (ship_upgrade_maxed(ship, upgrade)) return false;
    if (ship->credits + FLOAT_EPSILON < (float)credit_cost) return false;
    if (station->inventory[COMMODITY_FRAME + upgrade_required_product(upgrade)] + FLOAT_EPSILON < upgrade_product_cost(ship, upgrade)) return false;
    return true;
}
