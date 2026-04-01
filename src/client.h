/*
 * client.h -- Shared types and declarations for the Signal Space Miner
 * game client.  Included by main.c, station_ui.c, and hud.c.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "ship.h"
#include "economy.h"
#include "asteroid.h"
#include "game_sim.h"

/* Sokol headers (declarations only -- SOKOL_IMPL is in main.c) */
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"

/* ------------------------------------------------------------------ */
/* Station tab enum                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    STATION_TAB_OVERVIEW = 0,
    STATION_TAB_SERVICES,
    STATION_TAB_ROLE,
    STATION_TAB_CONTRACTS,
    STATION_TAB_CONSTRUCTION,
    STATION_TAB_COUNT
} station_tab_t;

/* ------------------------------------------------------------------ */
/* Station UI state (computed per frame when docked)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const station_t* station;
    int hull_now;
    int hull_max;
    int cargo_units;
    int cargo_capacity;
    int payout;
    int repair_cost;
    int mining_cost;
    int hold_cost;
    int tractor_cost;
    bool can_sell;
    bool can_repair;
    bool can_upgrade_mining;
    bool can_upgrade_hold;
    bool can_upgrade_tractor;
} station_ui_state_t;

typedef struct {
    const char* action;
    char state[32];
    uint8_t r;
    uint8_t g0;
    uint8_t b;
} station_service_line_t;

/* ------------------------------------------------------------------ */
/* Client game state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    bool key_down[KEY_COUNT];
    bool key_pressed[KEY_COUNT];
} input_state_t;

typedef struct {
    float accumulator;
} runtime_state_t;

typedef struct {
    input_state_t input;
    star_t stars[MAX_STARS];
    bool thrusting;
    char notice[128];
    float notice_timer;
    float collection_feedback_ore;
    int collection_feedback_fragments;
    float collection_feedback_timer;
    runtime_state_t runtime;
    audio_state_t audio;
    sg_pass_action pass_action;
    /* --- Simulation --- */
    world_t world;
    int local_player_slot;
    /* --- Multiplayer --- */
    bool multiplayer_enabled;
    float net_send_timer;
    uint8_t pending_net_action;
    float dock_predict_timer;
    float net_input_timer;
    station_tab_t station_tab;
    bool was_docked;
    /* --- Outpost placement mode --- */
    bool placing_outpost;
    /* --- Interpolation (multiplayer) --- */
    struct {
        asteroid_t prev[MAX_ASTEROIDS];
        asteroid_t curr[MAX_ASTEROIDS];
        float t;
        float interval;
    } asteroid_interp;
    struct {
        npc_ship_t prev[MAX_NPC_SHIPS];
        npc_ship_t curr[MAX_NPC_SHIPS];
        float t;
        float interval;
    } npc_interp;
} game_t;

extern game_t g;
#define LOCAL_PLAYER (g.world.players[g.local_player_slot])

/* ------------------------------------------------------------------ */
/* HUD layout constants                                               */
/* ------------------------------------------------------------------ */

#define HUD_MARGIN 28.0f
#define HUD_TOP_PANEL_WIDTH 332.0f
#define HUD_TOP_PANEL_HEIGHT 78.0f
#define HUD_TOP_PANEL_COMPACT_WIDTH 252.0f
#define HUD_TOP_PANEL_COMPACT_HEIGHT 64.0f
#define HUD_BOTTOM_PANEL_HEIGHT 32.0f
#define HUD_BOTTOM_PANEL_WIDTH 560.0f
#define HUD_BOTTOM_PANEL_COMPACT_WIDTH 344.0f
#define HUD_MESSAGE_PANEL_WIDTH 320.0f
#define HUD_MESSAGE_PANEL_HEIGHT 62.0f
#define HUD_MESSAGE_PANEL_COMPACT_WIDTH 236.0f
#define HUD_MESSAGE_PANEL_COMPACT_HEIGHT 56.0f
#define STATION_PANEL_WIDTH 560.0f
#define STATION_PANEL_HEIGHT 400.0f
#define STATION_PANEL_COMPACT_WIDTH 520.0f
#define STATION_PANEL_COMPACT_HEIGHT 290.0f
#define HUD_CELL_SIZE 8.0f
#define HUD_CELL HUD_CELL_SIZE

/* ------------------------------------------------------------------ */
/* UI utility functions (implemented in hud.c)                        */
/* ------------------------------------------------------------------ */

float ui_window_width(void);
float ui_window_height(void);
float ui_scale(void);
float ui_screen_width(void);
float ui_screen_height(void);
bool ui_is_compact(void);
float ui_text_zoom(void);
float ui_text_pos(float pixel_value);

/* UI drawing primitives */
void draw_ui_scanlines(float x, float y, float width, float height, float spacing, float alpha);
void draw_ui_corner_brackets(float x, float y, float width, float height, float r, float g0, float b, float alpha);
void draw_ui_rule(float x0, float x1, float y, float r, float g0, float b, float alpha);
void draw_ui_panel(float x, float y, float width, float height, float accent);
void draw_ui_scrim(float alpha);
void draw_ui_meter(float x, float y, float width, float height, float fill, float r, float g0, float b);
void draw_upgrade_pips(float x, float y, int level, float r, float g0, float b);
void draw_service_card(float x, float y, float width, float height, float accent_r, float accent_g, float accent_b, bool hot);

/* HUD layout helpers */
void get_flight_hud_rects(float* top_x, float* top_y, float* top_w, float* top_h,
    float* bottom_x, float* bottom_y, float* bottom_w, float* bottom_h);
bool hud_should_draw_message_panel(void);
void get_hud_message_panel_rect(float* x, float* y, float* width, float* height);
void get_station_panel_rect(float* x, float* y, float* width, float* height);

/* HUD drawing (call from render_ui) */
void draw_hud_panels(void);
void draw_hud(void);

/* ------------------------------------------------------------------ */
/* Station UI functions (implemented in station_ui.c)                 */
/* ------------------------------------------------------------------ */

void build_station_ui_state(station_ui_state_t* ui);
void draw_station_services(const station_ui_state_t* ui);

/* Station label/color helpers */
const char* station_role_hub_label(const station_t* station);
const char* station_role_market_title(const station_t* station);
const char* station_role_fit_title(const station_t* station);
const char* station_role_name(const station_t* station);
const char* station_role_short_name(const station_t* station);
void station_role_color(const station_t* station, float* r, float* g0, float* b);

/* Station lookup helpers (implemented in station_ui.c) */
const station_t* station_at(int station_index);
const station_t* current_station_ptr(void);
const station_t* nearby_station_ptr(void);
int nearest_station_index(vec2 pos);
const station_t* navigation_station_ptr(void);
bool station_has_service(uint32_t service);
uint32_t station_upgrade_service(ship_upgrade_t upgrade);

/* Formatting helpers (implemented in station_ui.c) */
void format_ore_manifest(char* text, size_t text_size);
void format_ore_hopper_line(const station_t* station, char* text, size_t text_size);
void format_ingot_stock_line(const station_t* station, char* text, size_t text_size);
void format_refinery_price_line(const station_t* station, char* text, size_t text_size);
void format_station_header_badge(const station_ui_state_t* ui, char* text, size_t text_size);
void format_station_market_summary(const station_ui_state_t* ui, bool compact, char* text, size_t text_size);
void format_station_market_detail(const station_ui_state_t* ui, bool compact, char* text, size_t text_size);
int build_station_service_lines(const station_ui_state_t* ui, station_service_line_t lines[3]);
void draw_station_service_text_line(float x, float y, const station_service_line_t* line, bool compact);

#endif /* CLIENT_H */
