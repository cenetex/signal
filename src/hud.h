#ifndef HUD_H
#define HUD_H

#include "types.h"
#include "ship.h"
#include "commodity.h"
#include "economy.h"

float ui_window_width(void);
float ui_window_height(void);
float ui_scale(void);
float ui_screen_width(void);
float ui_screen_height(void);
bool ui_is_compact(void);
float ui_text_pos(float pixel_value);

const char* station_role_name(station_role_t role);
const char* station_role_short_name(station_role_t role);
const char* station_role_hub_label(station_role_t role);
const char* station_role_market_title(station_role_t role);
const char* station_role_fit_title(station_role_t role);
void station_role_color(station_role_t role, float* r, float* g0, float* b);

void build_station_ui_state(station_ui_state_t* ui, const ship_t* ship, const station_t* station);

void format_ore_manifest(const ship_t* ship, char* text, size_t text_size);
void format_ore_hopper_line(const station_t* station, char* text, size_t text_size);
void format_ingot_stock_line(const station_t* station, char* text, size_t text_size);
void format_refinery_price_line(const station_t* station, char* text, size_t text_size);

#endif
