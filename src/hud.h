#ifndef HUD_H
#define HUD_H

#include "types.h"
#include "ship.h"
#include "commodity.h"
#include "economy.h"

/* ================================================================
 * Message priority levels — defines the fallthrough order in
 * build_hud_message(). Lower enum value = higher priority.
 * ================================================================ */
typedef enum {
    MSG_HULL_CRITICAL = 0,  /* P0: Hull integrity < 20% */
    MSG_NOTICE,             /* P1: Transient notice (reconnect, connection lost) */
    MSG_PLAN_MODE,          /* P2: Active planning mode */
    MSG_AUTOPILOT,          /* P2: Autopilot mining loop */
    MSG_ONBOARDING,         /* P3: Onboarding checklist progress */
    MSG_TOW,                /* P4: Scaffold tow active */
    MSG_CARGO,              /* P5: Hold full warning */
    MSG_DOCKING,            /* P6: Docking approach / dock ring hot */
    MSG_SCOOP,              /* P7: Collection feedback (+ore) */
    MSG_COUNT               /* Sentinel — not a real message */
} hud_message_t;

float ui_window_width(void);
float ui_window_height(void);
float ui_scale(void);
float ui_screen_width(void);
float ui_screen_height(void);
bool ui_is_compact(void);
float ui_text_pos(float pixel_value);

const char* station_role_name(const station_t* station);
const char* station_role_short_name(const station_t* station);
const char* station_role_hub_label(const station_t* station);
const char* station_role_market_title(const station_t* station);
const char* station_role_fit_title(const station_t* station);
void station_role_color(const station_t* station, float* r, float* g0, float* b);

void build_station_ui_state(station_ui_state_t* ui);

void format_ore_manifest(const ship_t* ship, char* text, size_t text_size);
void format_ore_hopper_line(const station_t* station, char* text, size_t text_size);
void format_ingot_stock_line(const station_t* station, char* text, size_t text_size);
void format_refinery_price_line(const station_t* station, char* text, size_t text_size);

#endif
