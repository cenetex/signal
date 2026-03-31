#include <math.h>
#include <stddef.h>
#include "economy.h"

void step_refinery_production(station_t* stations, int count, float dt) {
    for (int s = 0; s < count; s++) {
        station_t* station = &stations[s];
        if (station->role != STATION_ROLE_REFINERY) continue;

        int active = 0;
        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            if (station->ore_buffer[i] > 0.01f) active++;
        }
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;

        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            commodity_t ore = (commodity_t)i;
            if (station->ore_buffer[ore] <= 0.01f) continue;
            float consume = fminf(station->ore_buffer[ore], rate * dt);
            station->ore_buffer[ore] -= consume;
            station->inventory[commodity_refined_form(ore)] += consume;
        }
    }
}

void step_station_production(station_t* stations, int count, float dt) {
    for (int s = 0; s < count; s++) {
        station_t* station = &stations[s];

        if (station->role == STATION_ROLE_YARD) {
            if (station->product_stock[PRODUCT_FRAME] < MAX_PRODUCT_STOCK) {
                float buf = station->ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)];
                if (buf > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - station->product_stock[PRODUCT_FRAME];
                    float consume = fminf(buf, fminf(STATION_PRODUCTION_RATE * dt, room));
                    station->ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)] -= consume;
                    station->product_stock[PRODUCT_FRAME] += consume;
                }
            }
        } else if (station->role == STATION_ROLE_BEAMWORKS) {
            if (station->product_stock[PRODUCT_LASER_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_co = station->ingot_buffer[INGOT_IDX(COMMODITY_CONDUCTOR_INGOT)];
                if (buf_co > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - station->product_stock[PRODUCT_LASER_MODULE];
                    float consume = fminf(buf_co, fminf(STATION_PRODUCTION_RATE * dt, room));
                    station->ingot_buffer[INGOT_IDX(COMMODITY_CONDUCTOR_INGOT)] -= consume;
                    station->product_stock[PRODUCT_LASER_MODULE] += consume;
                }
            }
            if (station->product_stock[PRODUCT_TRACTOR_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_ln = station->ingot_buffer[INGOT_IDX(COMMODITY_LENS_INGOT)];
                if (buf_ln > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - station->product_stock[PRODUCT_TRACTOR_MODULE];
                    float consume = fminf(buf_ln, fminf(STATION_PRODUCTION_RATE * dt, room));
                    station->ingot_buffer[INGOT_IDX(COMMODITY_LENS_INGOT)] -= consume;
                    station->product_stock[PRODUCT_TRACTOR_MODULE] += consume;
                }
            }
        }
    }
}

float station_cargo_sale_value(const ship_t* ship, const station_t* station) {
    float total = 0.0f;
    if (station == NULL) return 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
        commodity_t commodity = (commodity_t)i;
        total += ship_cargo_amount(ship, commodity) * station_buy_price(station, commodity);
    }
    return total;
}

float station_repair_cost(const ship_t* ship, const station_t* station) {
    if (station == NULL) return 0.0f;
    if (!(station->services & STATION_SERVICE_REPAIR)) return 0.0f;
    float damage = ship_max_hull(ship) - ship->hull;
    if (damage <= 0.0f) return 0.0f;
    return damage * STATION_REPAIR_COST_PER_HULL;
}

bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, uint32_t service, int credit_cost) {
    if (!station || !(station->services & service)) return false;
    if (ship_upgrade_maxed(ship, upgrade)) return false;
    if (ship->credits + 0.01f < (float)credit_cost) return false;
    if (station->product_stock[upgrade_required_product(upgrade)] + 0.01f < upgrade_product_cost(ship, upgrade)) return false;
    return true;
}
