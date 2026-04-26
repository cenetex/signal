#ifndef COMMODITY_H
#define COMMODITY_H

#include "types.h"

commodity_t commodity_refined_form(commodity_t commodity);
commodity_t commodity_ore_form(commodity_t commodity);
const char* commodity_name(commodity_t commodity);
const char* commodity_code(commodity_t commodity);
const char* commodity_short_name(commodity_t commodity);

/* Commodity tint for UI amounts/chips (TRADE deltas, HUD hints, etc).
 * Ores get a warm-to-cool ladder (ferrite amber → cuprite green →
 * crystal violet); fabricated goods use their own distinctive tones.
 * Kept here next to commodity_short_name so UIs have one stop. */
void commodity_color_u8(commodity_t commodity, uint8_t *r, uint8_t *g, uint8_t *b);

float ship_total_cargo(const ship_t* ship);
float ship_cargo_amount(const ship_t* ship, commodity_t commodity);

/* Cargo space taken by one unit of commodity. 1.0 for most goods;
 * REPAIR_KIT is dense at REPAIR_KIT_CARGO_DENSITY. Used by space
 * checks so a ship can carry many kits in a small hold. */
float commodity_volume(commodity_t commodity);

/* Station buys from player at this price (lower when overstocked) */
float station_buy_price(const station_t* station, commodity_t commodity);
/* Station sells to player at this price (higher when understocked) */
float station_sell_price(const station_t* station, commodity_t commodity);
float station_inventory_amount(const station_t* station, commodity_t commodity);

#endif
