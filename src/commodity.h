#ifndef COMMODITY_H
#define COMMODITY_H

#include "types.h"

commodity_t commodity_refined_form(commodity_t commodity);
commodity_t commodity_ore_form(commodity_t commodity);
const char* commodity_name(commodity_t commodity);
const char* commodity_code(commodity_t commodity);
const char* commodity_short_name(commodity_t commodity);

float ship_total_cargo(const ship_t* ship);
float ship_raw_ore_total(const ship_t* ship);
float ship_cargo_amount(const ship_t* ship, commodity_t commodity);

/* Station buys from player at this price (lower when overstocked) */
float station_buy_price(const station_t* station, commodity_t commodity);
/* Station sells to player at this price (higher when understocked) */
float station_sell_price(const station_t* station, commodity_t commodity);
float station_inventory_amount(const station_t* station, commodity_t commodity);

#endif
