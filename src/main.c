#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "ship.h"
#include "audio.h"
#include "economy.h"
#include "asteroid.h"
#include "npc.h"
#include "render.h"
#include "game_sim.h"

/* --- Multiplayer networking --- */
#include "net.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#define SOKOL_IMPL
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_GL_IMPL
#define SOKOL_DEBUGTEXT_IMPL
#define SOKOL_AUDIO_IMPL
#include "sokol_app.h"
#include "sokol_audio.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "sokol_log.h"

/* Types, enums, and math utilities are in types.h and math_util.h */

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

typedef struct {
    bool key_down[KEY_COUNT];
    bool key_pressed[KEY_COUNT];
} input_state_t;

/* input_intent_t defined in game_sim.h */

typedef struct {
    float accumulator;
} runtime_state_t;

/* audio_wave_t, audio_voice_t, audio_state_t: see types.h */

typedef struct {
    input_state_t input;
    ship_t ship;
    station_t stations[MAX_STATIONS];
    asteroid_t asteroids[MAX_ASTEROIDS];
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
    star_t stars[MAX_STARS];
    uint32_t rng;
    int current_station;
    int nearby_station;
    int hover_asteroid;
    bool beam_active;
    bool beam_hit;
    bool thrusting;
    bool in_dock_range;
    bool docked;
    vec2 beam_start;
    vec2 beam_end;
    char notice[128];
    float notice_timer;
    int nearby_fragments;
    int tractor_fragments;
    float collection_feedback_ore;
    int collection_feedback_fragments;
    float collection_feedback_timer;
    float time;
    float field_spawn_timer;
    runtime_state_t runtime;
    audio_state_t audio;
    sg_pass_action pass_action;
    /* --- Simulation --- */
    world_t world;
    /* --- Multiplayer --- */
    bool multiplayer_enabled;
    float net_send_timer;
    uint8_t pending_net_action; /* one-shot action queued for next net send */
} game_t;

static game_t g;

static const float HUD_MARGIN = 28.0f;
static const float HUD_TOP_PANEL_WIDTH = 332.0f;
static const float HUD_TOP_PANEL_HEIGHT = 78.0f;
static const float HUD_TOP_PANEL_COMPACT_WIDTH = 252.0f;
static const float HUD_TOP_PANEL_COMPACT_HEIGHT = 64.0f;
static const float HUD_BOTTOM_PANEL_HEIGHT = 32.0f;
static const float HUD_BOTTOM_PANEL_WIDTH = 560.0f;
static const float HUD_BOTTOM_PANEL_COMPACT_WIDTH = 344.0f;
static const float HUD_MESSAGE_PANEL_WIDTH = 320.0f;
static const float HUD_MESSAGE_PANEL_HEIGHT = 62.0f;
static const float HUD_MESSAGE_PANEL_COMPACT_WIDTH = 236.0f;
static const float HUD_MESSAGE_PANEL_COMPACT_HEIGHT = 56.0f;
static const float STATION_PANEL_WIDTH = 560.0f;
static const float STATION_PANEL_HEIGHT = 320.0f;
static const float STATION_PANEL_COMPACT_WIDTH = 520.0f;
static const float STATION_PANEL_COMPACT_HEIGHT = 224.0f;
static const float HUD_CELL = 8.0f;
static const float UI_SCALE_TIGHT = 1.85f;
static const float UI_SCALE_COMPACT = 1.60f;
static const float UI_SCALE_DEFAULT = 1.42f;
static const float UI_SCALE_WIDE = 1.28f;
static const int MAX_SIM_STEPS_PER_FRAME = 8;

/* HULL_DEFS defined in game_sim.c */

/* Audio system: see audio.h/c */

static float ui_window_width(void) {
    return sapp_widthf() / fmaxf(1.0f, sapp_dpi_scale());
}

static float ui_window_height(void) {
    return sapp_heightf() / fmaxf(1.0f, sapp_dpi_scale());
}

static float ui_scale(void) {
    float width = ui_window_width();
    float height = ui_window_height();
    if ((width < 900.0f) || (height < 620.0f)) {
        return UI_SCALE_TIGHT;
    }
    if ((width < 1280.0f) || (height < 780.0f)) {
        return UI_SCALE_COMPACT;
    }
    if ((width > 1800.0f) && (height > 980.0f)) {
        return UI_SCALE_WIDE;
    }
    return UI_SCALE_DEFAULT;
}

static float ui_screen_width(void) {
    return ui_window_width() / ui_scale();
}

static float ui_screen_height(void) {
    return ui_window_height() / ui_scale();
}

static bool ui_is_compact(void) {
    return (ui_window_width() < 1200.0f) || (ui_window_height() < 760.0f);
}

static float ui_text_zoom(void) {
    return 1.0f;
}

static float ui_text_pos(float pixel_value) {
    // Snap to the debugtext cell grid so scaled layouts don't self-overlap.
    return roundf(pixel_value / (HUD_CELL * ui_text_zoom()));
}

static uint32_t rng_next(void) {
    if (g.rng == 0) {
        g.rng = 0xA341316Cu;
    }
    uint32_t x = g.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.rng = x;
    return x;
}

static float randf(void) {
    return (float)(rng_next() & 0x00FFFFFFu) / 16777215.0f;
}

static float rand_range(float min_value, float max_value) {
    return lerpf(min_value, max_value, randf());
}

static int rand_int(int min_value, int max_value) {
    return min_value + (int)(rng_next() % (uint32_t)(max_value - min_value + 1));
}

// Drop transient and held input when the app loses focus.
static void clear_input_state(void) {
    memset(g.input.key_down, 0, sizeof(g.input.key_down));
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
}

static void consume_pressed_input(void) {
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
}

static void set_notice(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g.notice, sizeof(g.notice), fmt, args);
    va_end(args);
    g.notice_timer = 3.0f;
}

/* asteroid_next_tier, asteroid_is_collectible, asteroid_progress_ratio: see asteroid.h/c */

/* commodity_refined_form, commodity_name, commodity_code, commodity_short_name: see commodity.h/c */

static commodity_t random_raw_ore(void) {
    return (commodity_t)rand_int((int)COMMODITY_FERRITE_ORE, (int)COMMODITY_CRYSTAL_ORE);
}

/* ship_total_cargo, ship_raw_ore_total, ship_cargo_amount, station_buy_price, station_inventory_amount: see commodity.h/c */

static void clear_ship_cargo(void) {
    memset(g.ship.cargo, 0, sizeof(g.ship.cargo));
}

static void format_ore_manifest(char* text, size_t text_size) {
    int ferrite = (int)lroundf(ship_cargo_amount(&g.ship,COMMODITY_FERRITE_ORE));
    int cuprite = (int)lroundf(ship_cargo_amount(&g.ship,COMMODITY_CUPRITE_ORE));
    int crystal = (int)lroundf(ship_cargo_amount(&g.ship,COMMODITY_CRYSTAL_ORE));
    snprintf(text, text_size, "%s %d  %s %d  %s %d",
        commodity_code(COMMODITY_FERRITE_ORE), ferrite,
        commodity_code(COMMODITY_CUPRITE_ORE), cuprite,
        commodity_code(COMMODITY_CRYSTAL_ORE), crystal);
}

static void format_ore_hopper_line(const station_t* station, char* text, size_t text_size) {
    int cap = (int)lroundf(REFINERY_HOPPER_CAPACITY);
    int ferrite = (int)lroundf(station->ore_buffer[COMMODITY_FERRITE_ORE]);
    int cuprite = (int)lroundf(station->ore_buffer[COMMODITY_CUPRITE_ORE]);
    int crystal = (int)lroundf(station->ore_buffer[COMMODITY_CRYSTAL_ORE]);
    snprintf(text, text_size, "FE %d/%d  CU %d/%d  CR %d/%d", ferrite, cap, cuprite, cap, crystal, cap);
}

static void format_ingot_stock_line(const station_t* station, char* text, size_t text_size) {
    int frame = (int)lroundf(station_inventory_amount(station, COMMODITY_FRAME_INGOT));
    int conductor = (int)lroundf(station_inventory_amount(station, COMMODITY_CONDUCTOR_INGOT));
    int lens = (int)lroundf(station_inventory_amount(station, COMMODITY_LENS_INGOT));
    snprintf(text, text_size, "%s %d  %s %d  %s %d",
        commodity_code(COMMODITY_FRAME_INGOT), frame,
        commodity_code(COMMODITY_CONDUCTOR_INGOT), conductor,
        commodity_code(COMMODITY_LENS_INGOT), lens);
}

static void format_refinery_price_line(const station_t* station, char* text, size_t text_size) {
    int ferrite = (int)lroundf(station_buy_price(station, COMMODITY_FERRITE_ORE));
    int cuprite = (int)lroundf(station_buy_price(station, COMMODITY_CUPRITE_ORE));
    int crystal = (int)lroundf(station_buy_price(station, COMMODITY_CRYSTAL_ORE));
    snprintf(text, text_size, "FE %d  CU %d  CR %d", ferrite, cuprite, crystal);
}

/* ship_hull_def, npc_hull_def, ship_max_hull, ship_cargo_capacity, ship_mining_rate,
   ship_tractor_range, ship_collect_radius, ship_upgrade_level, ship_upgrade_maxed,
   ship_upgrade_cost, upgrade_required_product, upgrade_product_cost, product_name: see ship.h/c */

static const station_t* station_at(int station_index) {
    if ((station_index < 0) || (station_index >= MAX_STATIONS)) {
        return NULL;
    }
    return &g.stations[station_index];
}

static const station_t* current_station_ptr(void) {
    return station_at(g.current_station);
}

static const station_t* nearby_station_ptr(void) {
    return station_at(g.nearby_station);
}

static int nearest_station_index(vec2 pos) {
    float best_distance_sq = 0.0f;
    int best_index = -1;

    for (int i = 0; i < MAX_STATIONS; i++) {
        float distance_sq = v2_dist_sq(pos, g.stations[i].pos);
        if ((best_index < 0) || (distance_sq < best_distance_sq)) {
            best_distance_sq = distance_sq;
            best_index = i;
        }
    }

    return best_index;
}

static const station_t* navigation_station_ptr(void) {
    if (g.docked) {
        return current_station_ptr();
    }
    if (g.nearby_station >= 0) {
        return nearby_station_ptr();
    }
    return station_at(nearest_station_index(g.ship.pos));
}

static const char* station_role_name(station_role_t role) {
    switch (role) {
        case STATION_ROLE_REFINERY:
            return "Refinery";
        case STATION_ROLE_YARD:
            return "Yard";
        case STATION_ROLE_BEAMWORKS:
            return "Beamworks";
        default:
            return "Station";
    }
}

static const char* station_role_short_name(station_role_t role) {
    switch (role) {
        case STATION_ROLE_REFINERY:
            return "REF";
        case STATION_ROLE_YARD:
            return "YARD";
        case STATION_ROLE_BEAMWORKS:
            return "BEAM";
        default:
            return "STN";
    }
}

static bool station_has_service(uint32_t service);
/* station_cargo_sale_value, station_repair_cost: see economy.h/c */
static void get_flight_hud_rects(float* top_x, float* top_y, float* top_w, float* top_h,
    float* bottom_x, float* bottom_y, float* bottom_w, float* bottom_h);

static bool hud_should_draw_message_panel(void) {
    return !g.docked || (g.notice_timer > 0.0f) || (g.collection_feedback_timer > 0.0f);
}

static void get_hud_message_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = ui_screen_width();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float panel_w = compact ? HUD_MESSAGE_PANEL_COMPACT_WIDTH : HUD_MESSAGE_PANEL_WIDTH;
    float panel_h = compact ? HUD_MESSAGE_PANEL_COMPACT_HEIGHT : HUD_MESSAGE_PANEL_HEIGHT;
    float gap = compact ? 8.0f : 12.0f;

    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);
    panel_w = fminf(panel_w, screen_w - (hud_margin * 2.0f));

    *x = screen_w - hud_margin - panel_w;
    *y = bottom_y - gap - panel_h;
    *width = panel_w;
    *height = panel_h;
}

static void split_hud_message_lines(const char* text, int max_cols, char* line0, size_t line0_size, char* line1, size_t line1_size) {
    if ((text == NULL) || (text[0] == '\0')) {
        line0[0] = '\0';
        line1[0] = '\0';
        return;
    }

    if (max_cols < 8) {
        max_cols = 8;
    }

    size_t len = strlen(text);
    if ((int)len <= max_cols) {
        snprintf(line0, line0_size, "%s", text);
        line1[0] = '\0';
        return;
    }

    int split = max_cols;
    while ((split > (max_cols / 2)) && (text[split] != ' ')) {
        split--;
    }
    if (split <= (max_cols / 2)) {
        split = max_cols;
    }

    snprintf(line0, line0_size, "%.*s", split, text);

    const char* rest = text + split;
    while (*rest == ' ') {
        rest++;
    }

    if ((int)strlen(rest) <= max_cols) {
        snprintf(line1, line1_size, "%s", rest);
    } else if (max_cols > 3) {
        snprintf(line1, line1_size, "%.*s...", max_cols - 3, rest);
    } else {
        snprintf(line1, line1_size, "%s", rest);
    }
}

static bool build_hud_message(char* label, size_t label_size, char* message, size_t message_size, uint8_t* r, uint8_t* g0, uint8_t* b) {
    int cargo_units = (int)lroundf(ship_raw_ore_total(&g.ship));
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&g.ship));
    const station_t* station = current_station_ptr();

    if (g.notice_timer > 0.0f) {
        snprintf(label, label_size, "NOTICE");
        snprintf(message, message_size, "%s", g.notice);
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if (g.collection_feedback_timer > 0.0f) {
        int recovered_ore = (int)lroundf(g.collection_feedback_ore);
        snprintf(label, label_size, "RECOVERY");
        if (g.collection_feedback_fragments > 0) {
            snprintf(message, message_size, "Recovered %d ore from %d fragment%s.", recovered_ore, g.collection_feedback_fragments, g.collection_feedback_fragments == 1 ? "" : "s");
        } else {
            snprintf(message, message_size, "Recovered %d ore.", recovered_ore);
        }
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if (g.docked) {
        if (station != NULL) {
            if (station->role == STATION_ROLE_REFINERY) {
                snprintf(label, label_size, "REFINERY");
                snprintf(message, message_size, "Sell raw ore here, repair up, then head back into the belt.");
            } else if (station->role == STATION_ROLE_YARD) {
                snprintf(label, label_size, "YARD");
                snprintf(message, message_size, "Patch the hull and refit hold racks before the next sortie.");
            } else {
                snprintf(label, label_size, "BEAMWORKS");
                snprintf(message, message_size, "Tune the laser or tractor, then get back on the run.");
            }
            *r = 164;
            *g0 = 177;
            *b = 205;
            return true;
        }
        return false;
    }

    if ((cargo_units >= cargo_capacity) && (g.nearby_fragments > 0)) {
        snprintf(label, label_size, "WARN");
        snprintf(message, message_size, "Hold full. Fragments are still drifting outside the scoop.");
        *r = 255;
        *g0 = 221;
        *b = 119;
        return true;
    }

    if (cargo_units >= cargo_capacity) {
        snprintf(label, label_size, "WARN");
        snprintf(message, message_size, "Hold full. Run the ore home to the refinery.");
        *r = 255;
        *g0 = 221;
        *b = 119;
        return true;
    }

    if (g.in_dock_range) {
        snprintf(label, label_size, "DOCK");
        snprintf(message, message_size, "Inside the dock ring. Press E to dock.");
        *r = 112;
        *g0 = 255;
        *b = 214;
        return true;
    }

    if (g.nearby_fragments > 0) {
        snprintf(label, label_size, "TRACTOR");
        if (g.tractor_fragments > 0) {
            snprintf(message, message_size, "Sweep through the debris cloud and let the tractor finish the pull.");
        } else {
            snprintf(message, message_size, "Close in on the fragments and let the tractor catch them.");
        }
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if ((g.hover_asteroid >= 0) && g.asteroids[g.hover_asteroid].active) {
        snprintf(label, label_size, "TIP");
        snprintf(message, message_size, "Hold the beam steady, crack the rock down, then sweep the fragments.");
        *r = 164;
        *g0 = 177;
        *b = 205;
        return true;
    }

    snprintf(label, label_size, "TIP");
    snprintf(message, message_size, "Crack rocks, sweep fragments, and run raw ore back to the refinery.");
    *r = 164;
    *g0 = 177;
    *b = 205;
    return true;
}

static const char* station_role_hub_label(station_role_t role) {
    switch (role) {
        case STATION_ROLE_REFINERY:
            return "REFINERY // ore intake";
        case STATION_ROLE_YARD:
            return "YARD // frame bay";
        case STATION_ROLE_BEAMWORKS:
            return "BEAMWORKS // field bench";
        default:
            return "STATION";
    }
}

static const char* station_role_market_title(station_role_t role) {
    switch (role) {
        case STATION_ROLE_REFINERY:
            return "ORE BOARD";
        case STATION_ROLE_YARD:
            return "FRAME BAY";
        case STATION_ROLE_BEAMWORKS:
            return "FIELD BENCH";
        default:
            return "MARKET";
    }
}

static const char* station_role_fit_title(station_role_t role) {
    switch (role) {
        case STATION_ROLE_REFINERY:
            return "HAUL";
        case STATION_ROLE_YARD:
            return "FIT";
        case STATION_ROLE_BEAMWORKS:
            return "TUNING";
        default:
            return "STATUS";
    }
}

/* can_afford_upgrade: see economy.h/c */

static void build_station_ui_state(station_ui_state_t* ui) {
    memset(ui, 0, sizeof(*ui));
    ui->station = current_station_ptr();
    if (ui->station == NULL) {
        return;
    }

    ui->hull_now = (int)lroundf(g.ship.hull);
    ui->hull_max = (int)lroundf(ship_max_hull(&g.ship));
    float ore_total = ship_raw_ore_total(&g.ship);
    float repair = station_repair_cost(&g.ship, current_station_ptr());
    ui->cargo_units = (int)lroundf(ore_total);
    ui->cargo_capacity = (int)lroundf(ship_cargo_capacity(&g.ship));
    ui->payout = (int)lroundf(station_cargo_sale_value(&g.ship, current_station_ptr()));
    ui->repair_cost = (int)lroundf(repair);
    ui->mining_cost = ship_upgrade_cost(&g.ship,SHIP_UPGRADE_MINING);
    ui->hold_cost = ship_upgrade_cost(&g.ship,SHIP_UPGRADE_HOLD);
    ui->tractor_cost = ship_upgrade_cost(&g.ship,SHIP_UPGRADE_TRACTOR);
    ui->can_sell = station_has_service(STATION_SERVICE_ORE_BUYER) && (ore_total > 0.01f);
    ui->can_repair = station_has_service(STATION_SERVICE_REPAIR) && (repair > 0.0f) && (g.ship.credits + 0.01f >= repair);
    ui->can_upgrade_mining = can_afford_upgrade(ui->station, &g.ship, SHIP_UPGRADE_MINING, STATION_SERVICE_UPGRADE_LASER, ui->mining_cost);
    ui->can_upgrade_hold = can_afford_upgrade(ui->station, &g.ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, ui->hold_cost);
    ui->can_upgrade_tractor = can_afford_upgrade(ui->station, &g.ship, SHIP_UPGRADE_TRACTOR, STATION_SERVICE_UPGRADE_TRACTOR, ui->tractor_cost);
}

static void format_station_header_badge(const station_ui_state_t* ui, char* text, size_t text_size) {
    if (ui->station == NULL) {
        snprintf(text, text_size, "STATION");
        return;
    }

    if (ui->station->role == STATION_ROLE_REFINERY) {
        snprintf(text, text_size, "ORE BOARD");
    } else if (ui->station->role == STATION_ROLE_YARD) {
        snprintf(text, text_size, "YARD BAY");
    } else {
        snprintf(text, text_size, "BEAM LAB");
    }
}

static void format_station_market_summary(const station_ui_state_t* ui, bool compact, char* text, size_t text_size) {
    if (ui->station == NULL) {
        text[0] = '\0';
        return;
    }

    if (ui->station->role == STATION_ROLE_REFINERY) {
        char manifest[64] = { 0 };
        format_ore_manifest(manifest, sizeof(manifest));
        if (compact) {
            snprintf(text, text_size, "%s %d/%d", manifest, ui->cargo_units, ui->cargo_capacity);
        } else {
            snprintf(text, text_size, "%s // %d/%d", manifest, ui->cargo_units, ui->cargo_capacity);
        }
    } else if (ui->station->role == STATION_ROLE_YARD) {
        snprintf(text, text_size, "%s", compact ? "Hull service + hold refit" : "Hull service and hold refits.");
    } else {
        snprintf(text, text_size, "%s", compact ? "Laser + tractor tuning" : "Laser and tractor tuning.");
    }
}

static void format_station_market_detail(const station_ui_state_t* ui, bool compact, char* text, size_t text_size) {
    if (ui->station == NULL) {
        text[0] = '\0';
        return;
    }

    if (ui->station->role == STATION_ROLE_REFINERY) {
        if (compact) {
            snprintf(text, text_size, "Haul %d cr", ui->payout);
        } else {
            char stock[64] = { 0 };
            format_ingot_stock_line(ui->station, stock, sizeof(stock));
            snprintf(text, text_size, "Value %d cr // Stock %s", ui->payout, stock);
        }
    } else if (ui->station->role == STATION_ROLE_YARD) {
        int buf = (int)lroundf(ui->station->ingot_buffer[INGOT_IDX(COMMODITY_FRAME_INGOT)]);
        int prod = (int)lroundf(ui->station->product_stock[PRODUCT_FRAME]);
        snprintf(text, text_size, "Ingots %d // Frames %d", buf, prod);
    } else {
        int lsr = (int)lroundf(ui->station->product_stock[PRODUCT_LASER_MODULE]);
        int trc = (int)lroundf(ui->station->product_stock[PRODUCT_TRACTOR_MODULE]);
        snprintf(text, text_size, "LSR %d  TRC %d", lsr, trc);
    }
}

static int build_station_service_lines(const station_ui_state_t* ui, station_service_line_t lines[3]) {
    if (ui->station == NULL) {
        return 0;
    }

    memset(lines, 0, sizeof(station_service_line_t) * 3);

    if (ui->station->role == STATION_ROLE_REFINERY) {
        lines[0].action = "[1] Sell ore";
        snprintf(lines[0].state, sizeof(lines[0].state), "%s", ui->cargo_units > 0 ? "ready" : "empty");
        lines[0].r = ui->can_sell ? 114 : 169;
        lines[0].g0 = ui->can_sell ? 255 : 179;
        lines[0].b = ui->can_sell ? 192 : 204;

        lines[1].action = "[2] Repair hull";
        if (ui->repair_cost > 0) {
            snprintf(lines[1].state, sizeof(lines[1].state), "%d cr", ui->repair_cost);
            lines[1].r = 255;
            lines[1].g0 = 221;
            lines[1].b = 119;
        } else {
            snprintf(lines[1].state, sizeof(lines[1].state), "nominal");
            lines[1].r = 169;
            lines[1].g0 = 179;
            lines[1].b = 204;
        }
        return 2;
    }

    lines[0].action = "[2] Repair hull";
    if (ui->repair_cost > 0) {
        snprintf(lines[0].state, sizeof(lines[0].state), "%d cr", ui->repair_cost);
        lines[0].r = 255;
        lines[0].g0 = 221;
        lines[0].b = 119;
    } else {
        snprintf(lines[0].state, sizeof(lines[0].state), "nominal");
        lines[0].r = 169;
        lines[0].g0 = 179;
        lines[0].b = 204;
    }

    if (ui->station->role == STATION_ROLE_YARD) {
        lines[1].action = "[4] Hold racks";
        if (ship_upgrade_maxed(&g.ship,SHIP_UPGRADE_HOLD)) {
            snprintf(lines[1].state, sizeof(lines[1].state), "maxed");
            lines[1].r = 169;
            lines[1].g0 = 179;
            lines[1].b = 204;
        } else {
            snprintf(lines[1].state, sizeof(lines[1].state), "%d cr", ui->hold_cost);
            lines[1].r = ui->can_upgrade_hold ? 203 : 169;
            lines[1].g0 = ui->can_upgrade_hold ? 220 : 179;
            lines[1].b = ui->can_upgrade_hold ? 248 : 204;
        }
        return 2;
    }

    lines[1].action = "[3] Laser array";
    if (ship_upgrade_maxed(&g.ship,SHIP_UPGRADE_MINING)) {
        snprintf(lines[1].state, sizeof(lines[1].state), "maxed");
        lines[1].r = 169;
        lines[1].g0 = 179;
        lines[1].b = 204;
    } else {
        snprintf(lines[1].state, sizeof(lines[1].state), "%d cr", ui->mining_cost);
        lines[1].r = ui->can_upgrade_mining ? 203 : 169;
        lines[1].g0 = ui->can_upgrade_mining ? 220 : 179;
        lines[1].b = ui->can_upgrade_mining ? 248 : 204;
    }

    lines[2].action = "[5] Tractor coil";
    if (ship_upgrade_maxed(&g.ship,SHIP_UPGRADE_TRACTOR)) {
        snprintf(lines[2].state, sizeof(lines[2].state), "maxed");
        lines[2].r = 169;
        lines[2].g0 = 179;
        lines[2].b = 204;
    } else {
        snprintf(lines[2].state, sizeof(lines[2].state), "%d cr", ui->tractor_cost);
        lines[2].r = ui->can_upgrade_tractor ? 203 : 169;
        lines[2].g0 = ui->can_upgrade_tractor ? 220 : 179;
        lines[2].b = ui->can_upgrade_tractor ? 248 : 204;
    }
    return 3;
}

static void draw_station_service_text_line(float x, float y, const station_service_line_t* line, bool compact) {
    sdtx_pos(ui_text_pos(x), ui_text_pos(y));
    sdtx_color3b(line->r, line->g0, line->b);
    if (compact) {
        sdtx_printf("%-16s %s", line->action, line->state);
    } else {
        sdtx_printf("%-26s %s", line->action, line->state);
    }
}

static void station_role_color(station_role_t role, float* r, float* g0, float* b) {
    switch (role) {
        case STATION_ROLE_REFINERY:
            *r = 0.34f;
            *g0 = 0.96f;
            *b = 0.76f;
            break;
        case STATION_ROLE_YARD:
            *r = 0.98f;
            *g0 = 0.74f;
            *b = 0.30f;
            break;
        case STATION_ROLE_BEAMWORKS:
            *r = 0.42f;
            *g0 = 0.86f;
            *b = 1.0f;
            break;
        default:
            *r = 0.45f;
            *g0 = 0.85f;
            *b = 1.0f;
            break;
    }
}

static uint32_t station_upgrade_service(ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return STATION_SERVICE_UPGRADE_LASER;
        case SHIP_UPGRADE_HOLD:
            return STATION_SERVICE_UPGRADE_HOLD;
        case SHIP_UPGRADE_TRACTOR:
            return STATION_SERVICE_UPGRADE_TRACTOR;
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

static vec2 station_dock_anchor(void) {
    const station_t* station = current_station_ptr();
    if (station == NULL) {
        return v2(0.0f, 0.0f);
    }
    return v2_add(station->pos, v2(0.0f, -(station->radius + ship_hull_def(&g.ship)->ship_radius + STATION_DOCK_APPROACH_OFFSET)));
}

static bool station_has_service(uint32_t service) {
    const station_t* station = current_station_ptr();
    return (station != NULL) && ((station->services & service) != 0);
}

/* station_cargo_sale_value, station_repair_cost: see economy.h/c */

static void apply_ship_damage(float damage);

static float ship_cargo_space(void) {
    return fmaxf(0.0f, ship_cargo_capacity(&g.ship) - ship_total_cargo(&g.ship));
}

static void clear_collection_feedback(void) {
    g.collection_feedback_ore = 0.0f;
    g.collection_feedback_fragments = 0;
    g.collection_feedback_timer = 0.0f;
}

static void push_collection_feedback(float recovered_ore, int recovered_fragments) {
    if (recovered_ore <= 0.0f) {
        return;
    }
    if (g.collection_feedback_timer <= 0.0f) {
        clear_collection_feedback();
    }
    g.collection_feedback_ore += recovered_ore;
    g.collection_feedback_fragments += recovered_fragments;
    g.collection_feedback_timer = COLLECTION_FEEDBACK_TIME;
}

static asteroid_tier_t random_field_asteroid_tier(void) {
    float roll = randf();
    if (roll < 0.24f) {
        return ASTEROID_TIER_XL;
    }
    if (roll < 0.70f) {
        return ASTEROID_TIER_L;
    }
    return ASTEROID_TIER_M;
}

static void spawn_field_asteroid_of_tier(asteroid_t* asteroid, asteroid_tier_t tier) {
    float distance = rand_range(420.0f, WORLD_RADIUS - 180.0f);
    float angle = rand_range(0.0f, TWO_PI_F);
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, random_raw_ore(), &g.rng);
    asteroid->fracture_child = false;
    asteroid->pos = v2(cosf(angle) * distance, sinf(angle) * distance);
    asteroid->vel = v2(rand_range(-4.0f, 4.0f), rand_range(-4.0f, 4.0f));
}

static void spawn_field_asteroid(asteroid_t* asteroid) {
    spawn_field_asteroid_of_tier(asteroid, random_field_asteroid_tier());
}

static void spawn_child_asteroid(asteroid_t* asteroid, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, commodity, &g.rng);
    asteroid->fracture_child = true;
    asteroid->pos = pos;
    asteroid->vel = vel;
}

static int desired_child_count(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return rand_int(2, 3);
        case ASTEROID_TIER_L:
            return rand_int(2, 3);
        case ASTEROID_TIER_M:
            return rand_int(2, 4);
        case ASTEROID_TIER_S:
        default:
            return 0;
    }
}

static void inspect_asteroid_field(int* seeded_count, int* first_inactive_slot) {
    *seeded_count = 0;
    *first_inactive_slot = -1;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.asteroids[i].active) {
            if (*first_inactive_slot < 0) {
                *first_inactive_slot = i;
            }
            continue;
        }

        if (!g.asteroids[i].fracture_child) {
            (*seeded_count)++;
        }
    }
}

static void fracture_asteroid(int asteroid_index, vec2 outward_dir) {
    asteroid_t parent = g.asteroids[asteroid_index];
    asteroid_tier_t child_tier = asteroid_next_tier(parent.tier);
    int desired = desired_child_count(parent.tier);
    int child_slots[4] = { asteroid_index, -1, -1, -1 };
    int child_count = 1;

    for (int i = 0; (i < MAX_ASTEROIDS) && (child_count < desired); i++) {
        if ((i == asteroid_index) || g.asteroids[i].active) {
            continue;
        }
        child_slots[child_count++] = i;
    }

    float base_angle = atan2f(outward_dir.y, outward_dir.x);
    for (int i = 0; i < child_count; i++) {
        float spread_t = (child_count == 1) ? 0.0f : (((float)i / (float)(child_count - 1)) - 0.5f);
        float child_angle = base_angle + (spread_t * 1.35f) + rand_range(-0.14f, 0.14f);
        vec2 dir = v2_from_angle(child_angle);
        vec2 tangent = v2_perp(dir);
        asteroid_t* child = &g.asteroids[child_slots[i]];
        spawn_child_asteroid(child, child_tier, parent.commodity, parent.pos, parent.vel);
        vec2 child_pos = v2_add(parent.pos, v2_scale(dir, (parent.radius * 0.28f) + (child->radius * 0.85f)));
        float drift = rand_range(22.0f, 56.0f);
        vec2 child_vel = v2_add(parent.vel, v2_add(v2_scale(dir, drift), v2_scale(tangent, rand_range(-10.0f, 10.0f))));
        child->pos = child_pos;
        child->vel = child_vel;
    }

    audio_play_fracture(&g.audio,parent.tier);
    set_notice("%s %s fractured into %d %s %s%s.",
        asteroid_tier_name(parent.tier),
        asteroid_tier_kind(parent.tier),
        child_count,
        asteroid_tier_name(child_tier),
        asteroid_tier_kind(child_tier),
        child_count == 1 ? "" : "s");
}

static void maintain_asteroid_field(float dt) {
    int seeded_count = 0;
    int first_inactive_slot = -1;
    inspect_asteroid_field(&seeded_count, &first_inactive_slot);

    if (seeded_count >= FIELD_ASTEROID_TARGET) {
        g.field_spawn_timer = 0.0f;
        return;
    }

    g.field_spawn_timer += dt;
    if (g.field_spawn_timer < FIELD_ASTEROID_RESPAWN_DELAY) {
        return;
    }

    if (first_inactive_slot >= 0) {
        spawn_field_asteroid(&g.asteroids[first_inactive_slot]);
    }
    g.field_spawn_timer = 0.0f;
}

static void init_starfield(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        float distance = rand_range(100.0f, WORLD_RADIUS * 2.0f);
        float angle = rand_range(0.0f, TWO_PI_F);
        g.stars[i].pos = v2(cosf(angle) * distance, sinf(angle) * distance);
        g.stars[i].depth = rand_range(0.16f, 0.9f);
        g.stars[i].size = rand_range(0.9f, 2.2f);
        g.stars[i].brightness = rand_range(0.45f, 1.0f);
    }
}

static void reset_world(void) {
    g.ship.hull_class = HULL_CLASS_MINER;
    g.ship.pos = v2(0.0f, -110.0f);
    g.ship.vel = v2(0.0f, 0.0f);
    g.ship.angle = PI_F * 0.5f;
    g.ship.hull = ship_max_hull(&g.ship);
    clear_ship_cargo();
    g.ship.credits = 0.0f;
    g.ship.mining_level = 0;
    g.ship.hold_level = 0;
    g.ship.tractor_level = 0;

    memset(g.stations, 0, sizeof(g.stations));

    snprintf(g.stations[0].name, sizeof(g.stations[0].name), "%s", "Prospect Refinery");
    g.stations[0].role = STATION_ROLE_REFINERY;
    g.stations[0].pos = v2(0.0f, -240.0f);
    g.stations[0].radius = 62.0f;
    g.stations[0].dock_radius = 132.0f;
    g.stations[0].buy_price[COMMODITY_FERRITE_ORE] = 10.0f;
    g.stations[0].buy_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    g.stations[0].buy_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    g.stations[0].services = STATION_SERVICE_ORE_BUYER | STATION_SERVICE_REPAIR;

    snprintf(g.stations[1].name, sizeof(g.stations[1].name), "%s", "Kepler Yard");
    g.stations[1].role = STATION_ROLE_YARD;
    g.stations[1].pos = v2(-320.0f, 230.0f);
    g.stations[1].radius = 56.0f;
    g.stations[1].dock_radius = 124.0f;
    g.stations[1].services = STATION_SERVICE_REPAIR | STATION_SERVICE_UPGRADE_HOLD;

    snprintf(g.stations[2].name, sizeof(g.stations[2].name), "%s", "Helios Works");
    g.stations[2].role = STATION_ROLE_BEAMWORKS;
    g.stations[2].pos = v2(320.0f, 230.0f);
    g.stations[2].radius = 56.0f;
    g.stations[2].dock_radius = 124.0f;
    g.stations[2].services = STATION_SERVICE_REPAIR | STATION_SERVICE_UPGRADE_LASER | STATION_SERVICE_UPGRADE_TRACTOR;

    g.current_station = 0;
    g.nearby_station = 0;

    g.hover_asteroid = -1;
    g.beam_active = false;
    g.beam_hit = false;
    g.thrusting = false;
    g.in_dock_range = true;
    g.docked = true;
    g.ship.pos = station_dock_anchor();
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    g.nearby_fragments = 0;
    g.tractor_fragments = 0;
    audio_clear_voices(&g.audio);
    clear_collection_feedback();
    g.field_spawn_timer = 0.0f;

    memset(g.asteroids, 0, sizeof(g.asteroids));
    if (FIELD_ASTEROID_TARGET > 0) {
        spawn_field_asteroid_of_tier(&g.asteroids[0], ASTEROID_TIER_XL);
    }
    if (FIELD_ASTEROID_TARGET > 1) {
        spawn_field_asteroid_of_tier(&g.asteroids[1], ASTEROID_TIER_L);
    }
    for (int i = 2; i < FIELD_ASTEROID_TARGET; i++) {
        spawn_field_asteroid(&g.asteroids[i]);
    }

    memset(g.npc_ships, 0, sizeof(g.npc_ships));
    for (int i = 0; i < 3; i++) {
        npc_ship_t* npc = &g.npc_ships[i];
        npc->active = true;
        npc->role = NPC_ROLE_MINER;
        npc->hull_class = HULL_CLASS_NPC_MINER;
        npc->state = NPC_STATE_DOCKED;
        npc->pos = v2_add(g.stations[0].pos, v2(30.0f * (float)(i - 1), -(g.stations[0].radius + HULL_DEFS[HULL_CLASS_NPC_MINER].ship_radius + 50.0f)));
        npc->vel = v2(0.0f, 0.0f);
        npc->angle = PI_F * 0.5f;
        npc->target_asteroid = -1;
        npc->home_station = 0;
        npc->state_timer = NPC_DOCK_TIME + (float)i * 2.0f;
        npc->thrusting = false;
    }

    for (int i = 0; i < 2; i++) {
        npc_ship_t* npc = &g.npc_ships[3 + i];
        npc->active = true;
        npc->role = NPC_ROLE_HAULER;
        npc->hull_class = HULL_CLASS_HAULER;
        npc->state = NPC_STATE_DOCKED;
        npc->pos = v2_add(g.stations[0].pos, v2(50.0f * (float)(i == 0 ? -1 : 1), -(g.stations[0].radius + HULL_DEFS[HULL_CLASS_HAULER].ship_radius + 70.0f)));
        npc->vel = v2(0.0f, 0.0f);
        npc->angle = PI_F * 0.5f;
        npc->target_asteroid = -1;
        npc->home_station = 0;
        npc->dest_station = 1 + i;
        npc->state_timer = HAULER_DOCK_TIME + (float)i * 3.0f;
        npc->thrusting = false;
    }

    set_notice("%s online. Press E to launch.", current_station_ptr()->name);
}

static float asteroid_profile(const asteroid_t* asteroid, float angle) {
    float bump1 = sinf(angle * 3.0f + asteroid->seed);
    float bump2 = sinf(angle * 7.0f + asteroid->seed * 1.71f);
    float bump3 = cosf(angle * 5.0f + asteroid->seed * 0.63f);
    float profile = 1.0f + (bump1 * 0.08f) + (bump2 * 0.06f) + (bump3 * 0.04f);
    return asteroid->radius * profile;
}

static void draw_background(vec2 camera) {
    for (int i = 0; i < MAX_STARS; i++) {
        const star_t* star = &g.stars[i];
        vec2 parallax_pos = v2_add(star->pos, v2_scale(camera, 1.0f - star->depth));
        float tint = star->brightness;
        draw_rect_centered(parallax_pos, star->size, star->size, 0.65f * tint, 0.75f * tint, tint, 0.9f);
    }
}

static void draw_station(const station_t* station, bool is_current, bool is_nearby) {
    float role_r = 0.45f;
    float role_g = 0.85f;
    float role_b = 1.0f;
    float pulse = 0.35f + (sinf((g.time * 2.1f) + (float)station->role) * 0.15f);
    int spoke_count = 6;

    station_role_color(station->role, &role_r, &role_g, &role_b);
    if (station->role == STATION_ROLE_YARD) {
        spoke_count = 4;
    } else if (station->role == STATION_ROLE_BEAMWORKS) {
        spoke_count = 5;
    }

    float dock_alpha = is_current ? 0.62f : (is_nearby ? 0.50f : 0.16f + pulse);
    draw_circle_outline(station->pos, station->dock_radius, 48, role_r * 0.72f, role_g, role_b, dock_alpha);
    draw_circle_filled(station->pos, station->radius, 28, 0.08f, 0.12f, 0.17f, 1.0f);
    draw_circle_outline(station->pos, station->radius + 8.0f, 28, role_r, role_g, role_b, 0.92f);
    draw_circle_filled(station->pos, 18.0f, 18, role_r * 0.68f, role_g * 0.88f, role_b * 0.96f, 1.0f);

    for (int i = 0; i < spoke_count; i++) {
        float angle = (TWO_PI_F / (float)spoke_count) * (float)i + g.time * (0.14f + ((float)station->role * 0.02f));
        vec2 inner = v2_add(station->pos, v2(cosf(angle) * 28.0f, sinf(angle) * 28.0f));
        vec2 outer = v2_add(station->pos, v2(cosf(angle) * 48.0f, sinf(angle) * 48.0f));
        draw_segment(inner, outer, role_r * 0.8f, role_g * 0.92f, role_b, 0.85f);

        if (station->role == STATION_ROLE_REFINERY) {
            vec2 mid = v2_add(station->pos, v2(cosf(angle) * 58.0f, sinf(angle) * 58.0f));
            draw_rect_centered(mid, 4.0f, 4.0f, role_r * 0.8f, role_g, role_b * 0.9f, 0.85f);
        }
    }

    if (station->role == STATION_ROLE_YARD) {
        draw_rect_outline(station->pos, 18.0f, 18.0f, role_r * 0.82f, role_g * 0.86f, role_b * 0.76f, 0.75f);
    } else if (station->role == STATION_ROLE_BEAMWORKS) {
        draw_segment(v2_add(station->pos, v2(-24.0f, 0.0f)), v2_add(station->pos, v2(24.0f, 0.0f)), role_r, role_g, role_b, 0.78f);
        draw_segment(v2_add(station->pos, v2(0.0f, -24.0f)), v2_add(station->pos, v2(0.0f, 24.0f)), role_r, role_g, role_b, 0.40f);
    }
}

static void draw_asteroid(const asteroid_t* asteroid, bool targeted) {
    float progress_ratio = asteroid_progress_ratio(asteroid);
    float body_r = 0.3f;
    float body_g = 0.3f;
    float body_b = 0.3f;
    int segments = 18;
    asteroid_body_color(asteroid->tier, asteroid->commodity, progress_ratio, &body_r, &body_g, &body_b);

    switch (asteroid->tier) {
        case ASTEROID_TIER_XL:
            segments = 22;
            break;
        case ASTEROID_TIER_L:
            segments = 18;
            break;
        case ASTEROID_TIER_M:
            segments = 15;
            break;
        case ASTEROID_TIER_S:
            segments = 12;
            break;
        default:
            break;
    }

    sgl_c4f(body_r, body_g, body_b, 1.0f);
    sgl_begin_triangles();
    {
        float step = TWO_PI_F / (float)segments;
        float a0 = asteroid->rotation;
        float r0 = asteroid_profile(asteroid, a0);
        float prev_x = asteroid->pos.x + cosf(a0) * r0;
        float prev_y = asteroid->pos.y + sinf(a0) * r0;
        for (int i = 1; i <= segments; i++) {
            float a1 = asteroid->rotation + (float)i * step;
            float r1 = asteroid_profile(asteroid, a1);
            float cx = asteroid->pos.x + cosf(a1) * r1;
            float cy = asteroid->pos.y + sinf(a1) * r1;
            sgl_v2f(asteroid->pos.x, asteroid->pos.y);
            sgl_v2f(prev_x, prev_y);
            sgl_v2f(cx, cy);
            prev_x = cx;
            prev_y = cy;
        }
    }
    sgl_end();

    float rim_r = targeted ? 0.45f : (body_r * 0.85f);
    float rim_g = targeted ? 0.94f : (body_g * 0.95f);
    float rim_b = targeted ? 1.0f : fminf(1.0f, body_b * 1.2f);
    float rim_a = targeted ? 1.0f : 0.8f;

    sgl_c4f(rim_r, rim_g, rim_b, rim_a);
    sgl_begin_line_strip();
    for (int i = 0; i <= segments; i++) {
        float angle = asteroid->rotation + ((float)i / (float)segments) * TWO_PI_F;
        float radius = asteroid_profile(asteroid, angle);
        sgl_v2f(asteroid->pos.x + cosf(angle) * radius, asteroid->pos.y + sinf(angle) * radius);
    }
    sgl_end();

    if (asteroid->tier == ASTEROID_TIER_S) {
        float cr, cg, cb;
        commodity_material_tint(asteroid->commodity, &cr, &cg, &cb);
        float glow_r = lerpf(0.48f, cr * 1.6f, 0.5f);
        float glow_g = lerpf(0.96f, cg * 1.6f, 0.5f);
        float glow_b = lerpf(0.78f, cb * 1.6f, 0.5f);
        draw_circle_filled(asteroid->pos, asteroid->radius * lerpf(0.14f, 0.24f, progress_ratio), 10, glow_r, glow_g, glow_b, lerpf(0.35f, 0.8f, progress_ratio));
    } else if (asteroid->tier == ASTEROID_TIER_M) {
        float cr, cg, cb;
        commodity_material_tint(asteroid->commodity, &cr, &cg, &cb);
        float glow_r = lerpf(0.36f, cr * 1.4f, 0.4f);
        float glow_g = lerpf(0.78f, cg * 1.4f, 0.4f);
        float glow_b = lerpf(0.98f, cb * 1.4f, 0.4f);
        draw_circle_filled(asteroid->pos, asteroid->radius * 0.16f, 8, glow_r, glow_g, glow_b, 0.4f);
    }

    if (targeted) {
        draw_circle_outline(asteroid->pos, asteroid->radius + 12.0f, 24, 0.35f, 1.0f, 0.92f, 0.75f);
    }
}

static void draw_ship_tractor_field(void) {
    if (g.nearby_fragments <= 0) {
        return;
    }

    float pulse = 0.28f + (sinf(g.time * 7.0f) * 0.08f);
    draw_circle_outline(g.ship.pos, ship_tractor_range(&g.ship), 40, 0.24f, 0.86f, 1.0f, pulse);
    if (g.tractor_fragments > 0) {
        draw_circle_outline(g.ship.pos, ship_collect_radius(&g.ship) + 6.0f, 28, 0.50f, 1.0f, 0.82f, 0.75f);
    }
}

static void draw_ship(void) {
    sgl_push_matrix();
    sgl_translate(g.ship.pos.x, g.ship.pos.y, 0.0f);
    sgl_rotate(g.ship.angle, 0.0f, 0.0f, 1.0f);

    if (g.thrusting) {
        float flicker = 10.0f + sinf(g.time * 42.0f) * 3.0f;
        sgl_c4f(1.0f, 0.74f, 0.24f, 0.95f);
        sgl_begin_triangles();
        sgl_v2f(-12.0f, 0.0f);
        sgl_v2f(-26.0f - flicker, 6.0f);
        sgl_v2f(-26.0f - flicker, -6.0f);
        sgl_end();
    }

    sgl_c4f(0.86f, 0.93f, 1.0f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(22.0f, 0.0f);
    sgl_v2f(-14.0f, 12.0f);
    sgl_v2f(-14.0f, -12.0f);
    sgl_end();

    sgl_c4f(0.12f, 0.20f, 0.28f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-5.0f, 5.5f);
    sgl_v2f(-5.0f, -5.5f);
    sgl_end();

    draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), 0.55f, 0.72f, 0.92f, 0.85f);
    draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), 0.55f, 0.72f, 0.92f, 0.85f);

    sgl_pop_matrix();
}

static void draw_npc_ship(const npc_ship_t* npc) {
    const hull_def_t* hull = npc_hull_def(npc);
    bool is_hauler = npc->hull_class == HULL_CLASS_HAULER;
    float scale = hull->render_scale;
    float hull_r = is_hauler ? 0.40f : 0.92f;
    float hull_g = is_hauler ? 0.72f : 0.68f;
    float hull_b = is_hauler ? 0.90f : 0.28f;

    sgl_push_matrix();
    sgl_translate(npc->pos.x, npc->pos.y, 0.0f);
    sgl_rotate(npc->angle, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale, scale, 1.0f);

    if (npc->thrusting) {
        float flicker = 8.0f + sinf(g.time * 38.0f + npc->pos.x) * 2.5f;
        sgl_c4f(1.0f, 0.6f, 0.15f, 0.9f);
        sgl_begin_triangles();
        sgl_v2f(-12.0f, 0.0f);
        sgl_v2f(-26.0f - flicker, 6.0f);
        sgl_v2f(-26.0f - flicker, -6.0f);
        sgl_end();
    }

    sgl_c4f(hull_r, hull_g, hull_b, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(22.0f, 0.0f);
    sgl_v2f(-14.0f, 12.0f);
    sgl_v2f(-14.0f, -12.0f);
    sgl_end();

    sgl_c4f(hull_r * 0.3f, hull_g * 0.3f, hull_b * 0.3f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-5.0f, 5.5f);
    sgl_v2f(-5.0f, -5.5f);
    sgl_end();

    draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), hull_r * 0.9f, hull_g * 0.8f, hull_b * 0.3f, 0.85f);
    draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), hull_r * 0.9f, hull_g * 0.8f, hull_b * 0.3f, 0.85f);

    sgl_pop_matrix();
}

static void draw_npc_mining_beam(const npc_ship_t* npc) {
    if (npc->state != NPC_STATE_MINING) return;
    if (npc->target_asteroid < 0) return;
    const asteroid_t* asteroid = &g.asteroids[npc->target_asteroid];
    if (!asteroid->active) return;

    vec2 forward = v2_from_angle(npc->angle);
    vec2 muzzle = v2_add(npc->pos, v2_scale(forward, npc_hull_def(npc)->ship_radius + 5.0f));
    vec2 to_target = v2_sub(asteroid->pos, muzzle);
    vec2 hit = v2_sub(asteroid->pos, v2_scale(v2_norm(to_target), asteroid->radius * 0.85f));

    draw_segment(muzzle, hit, 0.92f, 0.68f, 0.28f, 0.85f);
    draw_segment(muzzle, hit, 0.45f, 0.30f, 0.10f, 0.35f);
}

static void draw_npc_ships(void) {
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!g.npc_ships[i].active) continue;
        draw_npc_ship(&g.npc_ships[i]);
        draw_npc_mining_beam(&g.npc_ships[i]);
    }
}

static void draw_beam(void) {
    if (!g.beam_active) {
        return;
    }

    if (g.beam_hit) {
        draw_segment(g.beam_start, g.beam_end, 0.45f, 1.0f, 0.92f, 0.95f);
        draw_segment(g.beam_start, g.beam_end, 0.12f, 0.78f, 1.0f, 0.35f);
    } else {
        draw_segment(g.beam_start, g.beam_end, 0.9f, 0.75f, 0.30f, 0.55f);
    }
}

static void draw_ui_scanlines(float x, float y, float width, float height, float spacing, float alpha) {
    for (float scan_y = y + 10.0f; scan_y < (y + height - 10.0f); scan_y += spacing) {
        draw_segment(v2(x + 10.0f, scan_y), v2(x + width - 10.0f, scan_y), 0.08f, 0.14f, 0.20f, alpha);
    }
}

static void draw_ui_corner_brackets(float x, float y, float width, float height, float r, float g0, float b, float alpha) {
    float arm = fminf(26.0f, fminf(width, height) * 0.16f);
    float inset = 3.0f;

    draw_segment(v2(x + inset, y + arm), v2(x + inset, y + inset), r, g0, b, alpha);
    draw_segment(v2(x + inset, y + inset), v2(x + arm, y + inset), r, g0, b, alpha);

    draw_segment(v2(x + width - arm, y + inset), v2(x + width - inset, y + inset), r, g0, b, alpha);
    draw_segment(v2(x + width - inset, y + inset), v2(x + width - inset, y + arm), r, g0, b, alpha);

    draw_segment(v2(x + inset, y + height - arm), v2(x + inset, y + height - inset), r, g0, b, alpha);
    draw_segment(v2(x + inset, y + height - inset), v2(x + arm, y + height - inset), r, g0, b, alpha);

    draw_segment(v2(x + width - arm, y + height - inset), v2(x + width - inset, y + height - inset), r, g0, b, alpha);
    draw_segment(v2(x + width - inset, y + height - inset), v2(x + width - inset, y + height - arm), r, g0, b, alpha);
}

static void draw_ui_rule(float x0, float x1, float y, float r, float g0, float b, float alpha) {
    draw_segment(v2(x0, y), v2(x1, y), r, g0, b, alpha);
}

static void draw_ui_panel(float x, float y, float width, float height, float accent) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    float accent_r = 0.26f + (accent * 0.28f);
    float accent_g = 0.72f + (accent * 0.20f);
    float accent_b = 0.98f;

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.015f, 0.028f, 0.05f, 0.94f);
    draw_rect_centered(center, (width * 0.5f) - 2.0f, (height * 0.5f) - 2.0f, 0.018f, 0.044f, 0.072f, 0.86f);
    draw_ui_scanlines(x, y, width, height, 8.0f, 0.06f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.09f, 0.18f, 0.28f, 0.34f);
    draw_ui_corner_brackets(x, y, width, height, accent_r, accent_g, accent_b, 0.82f);
    draw_ui_rule(x + 14.0f, x + fminf(116.0f, width * 0.28f), y + 14.0f, accent_r, accent_g, accent_b, 0.82f);
    draw_ui_rule(x + width - fminf(56.0f, width * 0.18f), x + width - 14.0f, y + 14.0f, 0.18f, 0.28f, 0.38f, 0.55f);
}

static void get_station_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_height = compact ? 28.0f : HUD_BOTTOM_PANEL_HEIGHT;
    float top_height = compact ? HUD_TOP_PANEL_COMPACT_HEIGHT : HUD_TOP_PANEL_HEIGHT;
    float panel_width = fminf(compact ? STATION_PANEL_COMPACT_WIDTH : STATION_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float panel_height = fminf(compact ? STATION_PANEL_COMPACT_HEIGHT : STATION_PANEL_HEIGHT, screen_h - top_height - bottom_height - (hud_margin * 2.0f) - 20.0f);
    float panel_x = (screen_w - panel_width) * 0.5f;
    float min_y = hud_margin + top_height + 16.0f;
    float max_y = screen_h - hud_margin - bottom_height - panel_height - 14.0f;
    float panel_y = clampf((screen_h - panel_height) * 0.5f, min_y, fmaxf(min_y, max_y));

    *x = panel_x;
    *y = panel_y;
    *width = panel_width;
    *height = panel_height;
}

static void draw_ui_scrim(float alpha) {
    draw_rect_centered(v2(ui_screen_width() * 0.5f, ui_screen_height() * 0.5f), ui_screen_width() * 0.5f, ui_screen_height() * 0.5f, 0.01f, 0.03f, 0.06f, alpha);
}

static void draw_ui_meter(float x, float y, float width, float height, float fill, float r, float g0, float b) {
    float clamped_fill = clampf(fill, 0.0f, 1.0f);
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.03f, 0.06f, 0.10f, 0.98f);
    draw_ui_scanlines(x, y, width, height, 4.0f, 0.05f);
    if (clamped_fill > 0.0f) {
        float fill_width = width * clamped_fill;
        draw_rect_centered(v2(x + (fill_width * 0.5f), y + (height * 0.5f)), fill_width * 0.5f, height * 0.5f, r, g0, b, 0.92f);
        draw_ui_rule(x + 2.0f, x + fill_width - 2.0f, y + 2.0f, fminf(1.0f, r + 0.18f), fminf(1.0f, g0 + 0.18f), fminf(1.0f, b + 0.18f), 0.72f);
    }
    draw_ui_corner_brackets(x, y, width, height, 0.24f, 0.48f, 0.62f, 0.70f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.12f, 0.22f, 0.32f, 0.48f);
}

static void draw_upgrade_pips(float x, float y, int level, float r, float g0, float b) {
    const float pip_w = 16.0f;
    const float pip_h = 7.0f;
    const float gap = 6.0f;

    for (int i = 0; i < SHIP_UPGRADE_MAX_LEVEL; i++) {
        float px = x + ((pip_w + gap) * (float)i);
        float alpha = i < level ? 0.95f : 0.35f;
        draw_rect_centered(v2(px + (pip_w * 0.5f), y + (pip_h * 0.5f)), pip_w * 0.5f, pip_h * 0.5f,
            i < level ? r : 0.12f,
            i < level ? g0 : 0.18f,
            i < level ? b : 0.24f,
            alpha);
        draw_rect_outline(v2(px + (pip_w * 0.5f), y + (pip_h * 0.5f)), pip_w * 0.5f, pip_h * 0.5f, 0.18f, 0.30f, 0.42f, 0.88f);
    }
}

static void draw_service_card(float x, float y, float width, float height, float accent_r, float accent_g, float accent_b, bool hot) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    float border_a = hot ? 0.84f : 0.30f;
    float accent_a = hot ? 0.92f : 0.48f;
    float body_tint = hot ? 0.10f : 0.06f;
    float status_w = fminf(92.0f, width * 0.24f);

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.025f, body_tint, 0.09f, 0.94f);
    draw_rect_centered(v2(x + width - (status_w * 0.5f) - 8.0f, y + (height * 0.5f)), status_w * 0.5f, (height * 0.5f) - 5.0f, 0.02f, 0.05f, 0.08f, 0.92f);
    draw_rect_centered(v2(x + 4.0f, y + (height * 0.5f)), 3.0f, height * 0.5f, accent_r, accent_g, accent_b, accent_a);
    draw_ui_rule(x + 14.0f, x + fminf(90.0f, width * 0.20f), y + 8.0f, accent_r, accent_g, accent_b, hot ? 0.78f : 0.38f);
    draw_ui_rule(x + 10.0f, x + width - 10.0f, y + height - 2.0f, accent_r * 0.5f, accent_g * 0.5f, accent_b * 0.5f, hot ? 0.26f : 0.14f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, accent_r * 0.30f, accent_g * 0.30f, accent_b * 0.30f, border_a);
}

static void get_flight_hud_rects(float* top_x, float* top_y, float* top_w, float* top_h,
    float* bottom_x, float* bottom_y, float* bottom_w, float* bottom_h) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float top_width = fminf(compact ? HUD_TOP_PANEL_COMPACT_WIDTH : HUD_TOP_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float top_height = compact ? HUD_TOP_PANEL_COMPACT_HEIGHT : HUD_TOP_PANEL_HEIGHT;
    float bottom_width = fminf(compact ? HUD_BOTTOM_PANEL_COMPACT_WIDTH : HUD_BOTTOM_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float bottom_height = compact ? 28.0f : HUD_BOTTOM_PANEL_HEIGHT;

    *top_x = hud_margin;
    *top_y = hud_margin;
    *top_w = top_width;
    *top_h = top_height;
    *bottom_x = hud_margin;
    *bottom_y = screen_h - hud_margin - bottom_height;
    *bottom_w = bottom_width;
    *bottom_h = bottom_height;
}

static void draw_hud_panels(void) {
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float message_x = 0.0f;
    float message_y = 0.0f;
    float message_w = 0.0f;
    float message_h = 0.0f;
    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);

    draw_ui_panel(top_x, top_y, top_w, top_h, 0.03f);
    draw_ui_panel(bottom_x, bottom_y, bottom_w, bottom_h, 0.02f);

    if (g.docked) {
        float panel_x = 0.0f;
        float panel_y = 0.0f;
        float panel_w = 0.0f;
        float panel_h = 0.0f;
        float inner_x = 0.0f;
        float inner_y = 0.0f;
        float inner_w = 0.0f;
        float inner_h = 0.0f;
        float market_x = 0.0f;
        float market_y = 0.0f;
        float market_w = 0.0f;
        float market_h = 0.0f;
        float services_x = 0.0f;
        float services_y = 0.0f;
        float services_w = 0.0f;
        float services_h = 0.0f;
        float fit_x = 0.0f;
        float fit_y = 0.0f;
        float fit_w = 0.0f;
        float fit_h = 0.0f;
        bool compact = ui_is_compact();
        bool show_fit_panel = !compact;
        float card_gap = compact ? 4.0f : 6.0f;
        float card_h = compact ? 18.0f : 24.0f;
        float sell_y = 0.0f;
        float repair_y = 0.0f;
        float mining_y = 0.0f;
        station_ui_state_t ui = { 0 };

        build_station_ui_state(&ui);

        get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
        draw_ui_scrim(0.34f);
        draw_ui_panel(panel_x, panel_y, panel_w, panel_h, 0.08f);

        inner_x = panel_x + 18.0f;
        inner_y = panel_y + 18.0f;
        inner_w = panel_w - 36.0f;
        inner_h = panel_h - 36.0f;

        market_x = inner_x;
        services_x = inner_x;
        services_w = inner_w;

        if (show_fit_panel) {
            fit_w = 180.0f;
            fit_x = panel_x + panel_w - fit_w - 18.0f;
            fit_y = inner_y + 42.0f;
            fit_h = inner_h - 42.0f;

            market_y = fit_y;
            market_w = fit_x - inner_x - 12.0f;
            market_h = 72.0f;

            services_y = market_y + market_h + 16.0f;
            services_w = market_w;
            services_h = panel_y + panel_h - 18.0f - services_y;
        } else {
            market_y = inner_y + 42.0f;
            market_w = inner_w;
            market_h = 54.0f;

            services_y = market_y + market_h + 12.0f;
            services_h = 92.0f;
        }

        draw_ui_rule(inner_x, panel_x + panel_w - 18.0f, inner_y + 26.0f, 0.14f, 0.26f, 0.38f, 0.70f);
        if (show_fit_panel) {
            draw_segment(v2(fit_x - 10.0f, inner_y + 38.0f), v2(fit_x - 10.0f, panel_y + panel_h - 18.0f), 0.10f, 0.22f, 0.32f, 0.60f);
        }

        draw_ui_panel(market_x, market_y, market_w, market_h, 0.04f);
        draw_ui_panel(services_x, services_y, services_w, services_h, 0.03f);
        if (show_fit_panel) {
            draw_ui_panel(fit_x, fit_y, fit_w, fit_h, 0.05f);
        }

        sell_y = services_y + (compact ? 30.0f : 36.0f);
        repair_y = sell_y + card_h + card_gap;
        mining_y = repair_y + card_h + card_gap;

        if (ui.station->role == STATION_ROLE_REFINERY) {
            draw_service_card(services_x + 12.0f, sell_y, services_w - 24.0f, card_h, 0.24f, 0.90f, 0.70f, ui.can_sell);
            draw_service_card(services_x + 12.0f, repair_y, services_w - 24.0f, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
        } else if (ui.station->role == STATION_ROLE_YARD) {
            draw_service_card(services_x + 12.0f, sell_y, services_w - 24.0f, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
            draw_service_card(services_x + 12.0f, repair_y, services_w - 24.0f, card_h, 0.50f, 0.82f, 1.0f, ui.can_upgrade_hold);
        } else {
            draw_service_card(services_x + 12.0f, sell_y, services_w - 24.0f, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
            draw_service_card(services_x + 12.0f, repair_y, services_w - 24.0f, card_h, 0.34f, 0.88f, 1.0f, ui.can_upgrade_mining);
            draw_service_card(services_x + 12.0f, mining_y, services_w - 24.0f, card_h, 0.42f, 1.0f, 0.86f, ui.can_upgrade_tractor);
        }

        if (show_fit_panel) {
            draw_ui_meter(fit_x + 16.0f, fit_y + 54.0f, fit_w - 32.0f, 12.0f, g.ship.hull / ship_max_hull(&g.ship), 0.96f, 0.54f, 0.28f);
            draw_ui_meter(fit_x + 16.0f, fit_y + 94.0f, fit_w - 32.0f, 12.0f, ship_total_cargo(&g.ship) / fmaxf(1.0f, ship_cargo_capacity(&g.ship)), 0.26f, 0.90f, 0.72f);
            if (ui.station->role == STATION_ROLE_YARD) {
                draw_upgrade_pips(fit_x + 18.0f, fit_y + 184.0f, g.ship.hold_level, 0.50f, 0.82f, 1.0f);
            } else if (ui.station->role == STATION_ROLE_BEAMWORKS) {
                draw_upgrade_pips(fit_x + 18.0f, fit_y + 146.0f, g.ship.mining_level, 0.34f, 0.88f, 1.0f);
                draw_upgrade_pips(fit_x + 18.0f, fit_y + 184.0f, g.ship.tractor_level, 0.42f, 1.0f, 0.86f);
            }
        }
    }

    if (hud_should_draw_message_panel()) {
        get_hud_message_panel_rect(&message_x, &message_y, &message_w, &message_h);
        draw_ui_panel(message_x, message_y, message_w, message_h, 0.05f);
    }
}

static void draw_station_services(const station_ui_state_t* ui) {
    if (!g.docked) {
        return;
    }

    float panel_x = 0.0f;
    float panel_y = 0.0f;
    float panel_w = 0.0f;
    float panel_h = 0.0f;
    float inner_y = 0.0f;
    float market_x = 0.0f;
    float market_y = 0.0f;
    float market_h = 0.0f;
    float services_x = 0.0f;
    float services_y = 0.0f;
    float fit_x = 0.0f;
    float fit_y = 0.0f;
    float fit_w = 0.0f;
    float card_gap = 0.0f;
    float card_h = 0.0f;
    float sell_y = 0.0f;
    float repair_y = 0.0f;
    float mining_y = 0.0f;
    station_service_line_t service_lines[3] = { 0 };
    char header_badge[32] = { 0 };
    char market_summary[64] = { 0 };
    char market_detail[64] = { 0 };
    int service_line_count = 0;
    bool compact = ui_is_compact();
    bool show_fit_panel = !compact;
    format_station_header_badge(ui, header_badge, sizeof(header_badge));
    format_station_market_summary(ui, compact, market_summary, sizeof(market_summary));
    format_station_market_detail(ui, compact, market_detail, sizeof(market_detail));
    service_line_count = build_station_service_lines(ui, service_lines);

    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    inner_y = panel_y + 18.0f;
    market_x = panel_x + 18.0f;
    services_x = panel_x + 18.0f;
    card_gap = compact ? 4.0f : 6.0f;
    card_h = compact ? 18.0f : 24.0f;

    if (show_fit_panel) {
        fit_w = 180.0f;
        fit_x = panel_x + panel_w - fit_w - 18.0f;
        fit_y = inner_y + 42.0f;
        market_y = fit_y;
        market_h = 78.0f;
        services_y = market_y + market_h + 16.0f;
    } else {
        market_y = inner_y + 42.0f;
        market_h = 54.0f;
        services_y = market_y + market_h + 12.0f;
    }

    sell_y = services_y + (compact ? 30.0f : 36.0f);
    repair_y = sell_y + card_h + card_gap;
    mining_y = repair_y + card_h + card_gap;

    sdtx_color3b(232, 241, 255);
    sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 16.0f));
    sdtx_puts(ui->station->name);
    sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 32.0f));
    sdtx_color3b(118, 255, 221);
    sdtx_puts(station_role_hub_label(ui->station->role));

    if (panel_w >= 480.0f) {
        sdtx_pos(ui_text_pos(panel_x + panel_w - 152.0f), ui_text_pos(panel_y + 16.0f));
        sdtx_color3b(203, 220, 248);
        sdtx_puts(header_badge);
        sdtx_pos(ui_text_pos(panel_x + panel_w - 152.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("Press E to launch");
    }

    sdtx_pos(ui_text_pos(market_x + 18.0f), ui_text_pos(market_y + 16.0f));
    sdtx_color3b(130, 255, 235);
    sdtx_puts(station_role_market_title(ui->station->role));
    sdtx_pos(ui_text_pos(market_x + 18.0f), ui_text_pos(market_y + 32.0f));
    sdtx_color3b(203, 220, 248);
    sdtx_puts(market_summary);
    sdtx_pos(ui_text_pos(market_x + 18.0f), ui_text_pos(market_y + 48.0f));
    sdtx_color3b(145, 160, 188);
    sdtx_puts(market_detail);

    sdtx_pos(ui_text_pos(services_x + 18.0f), ui_text_pos(services_y + 16.0f));
    sdtx_color3b(130, 255, 235);
    sdtx_puts("SERVICES");

    for (int i = 0; i < service_line_count; i++) {
        float line_y = (i == 0) ? sell_y : ((i == 1) ? repair_y : mining_y);
        draw_station_service_text_line(services_x + 24.0f, line_y + (compact ? 5.0f : 8.0f), &service_lines[i], compact);
    }

    if (show_fit_panel) {
        sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 16.0f));
        sdtx_color3b(130, 255, 235);
        sdtx_puts(station_role_fit_title(ui->station->role));

        sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 32.0f));
        sdtx_color3b(203, 220, 248);
        if (ui->station->role == STATION_ROLE_REFINERY) {
            char board_line[64] = { 0 };
            char hopper_line[64] = { 0 };
            char stock_line[64] = { 0 };
            format_refinery_price_line(ui->station, board_line, sizeof(board_line));
            format_ore_hopper_line(ui->station, hopper_line, sizeof(hopper_line));
            format_ingot_stock_line(ui->station, stock_line, sizeof(stock_line));
            sdtx_printf("Hull %d/%d", ui->hull_now, ui->hull_max);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 56.0f));
            sdtx_printf("Ore %d/%d  Haul %d cr", ui->cargo_units, ui->cargo_capacity, ui->payout);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 88.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_puts(board_line);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 120.0f));
            sdtx_color3b(203, 220, 248);
            sdtx_printf("Hoppers  %s", hopper_line);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 152.0f));
            sdtx_printf("Stock    %s", stock_line);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 184.0f));
            sdtx_printf("Repair %s", ui->repair_cost > 0 ? "available" : "nominal");
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 216.0f));
            sdtx_color3b(169, 179, 204);
            sdtx_puts("Ore only sells here.");
        } else if (ui->station->role == STATION_ROLE_YARD) {
            int frames = (int)lroundf(ui->station->product_stock[PRODUCT_FRAME]);
            int need = (int)lroundf(upgrade_product_cost(&g.ship,SHIP_UPGRADE_HOLD));
            sdtx_printf("Hold %d ore  Lv %d/%d", ui->cargo_capacity, g.ship.hold_level, SHIP_UPGRADE_MAX_LEVEL);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 56.0f));
            sdtx_printf("Hull %d/%d", ui->hull_now, ui->hull_max);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 88.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_printf("Frames %d  Need %d", frames, ship_upgrade_maxed(&g.ship,SHIP_UPGRADE_HOLD) ? 0 : need);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 120.0f));
            sdtx_printf("Repair %s", ui->repair_cost > 0 ? "available" : "nominal");
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 152.0f));
            sdtx_color3b(169, 179, 204);
            sdtx_puts("Frame manufacturing.");
        } else {
            int lasers = (int)lroundf(ui->station->product_stock[PRODUCT_LASER_MODULE]);
            int tractors = (int)lroundf(ui->station->product_stock[PRODUCT_TRACTOR_MODULE]);
            sdtx_printf("Laser Lv %d/%d  Tractor Lv %d/%d", g.ship.mining_level, SHIP_UPGRADE_MAX_LEVEL, g.ship.tractor_level, SHIP_UPGRADE_MAX_LEVEL);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 56.0f));
            sdtx_printf("Hull %d/%d", ui->hull_now, ui->hull_max);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 88.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_printf("LSR %d  TRC %d", lasers, tractors);
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 120.0f));
            sdtx_printf("Repair %s", ui->repair_cost > 0 ? "available" : "nominal");
            sdtx_pos(ui_text_pos(fit_x + 18.0f), ui_text_pos(fit_y + 152.0f));
            sdtx_color3b(169, 179, 204);
            sdtx_puts("Module fabrication.");
        }
    }
}

static void draw_hud(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float message_x = 0.0f;
    float message_y = 0.0f;
    float message_w = 0.0f;
    float message_h = 0.0f;
    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);
    float top_text_x = ui_text_pos(top_x + 16.0f);
    float top_row_0 = ui_text_pos(top_y + 16.0f);
    float top_row_1 = ui_text_pos(top_y + (compact ? 24.0f : 30.0f));
    float top_row_2 = ui_text_pos(top_y + (compact ? 32.0f : 44.0f));
    float top_row_3 = ui_text_pos(top_y + (compact ? 40.0f : 58.0f));
    float bottom_text_x = ui_text_pos(bottom_x + 16.0f);
    float bottom_text_y = ui_text_pos(bottom_y + 8.0f);
    char message_label[16] = { 0 };
    char message_text[160] = { 0 };
    char message_line0[96] = { 0 };
    char message_line1[96] = { 0 };
    uint8_t message_r = 164;
    uint8_t message_g = 177;
    uint8_t message_b = 205;
    int hull_units = (int)lroundf(g.ship.hull);
    int hull_capacity = (int)lroundf(ship_max_hull(&g.ship));
    int cargo_units = (int)lroundf(ship_raw_ore_total(&g.ship));
    int credits = (int)lroundf(g.ship.credits);
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&g.ship));
    int payout_preview = (int)lroundf(station_cargo_sale_value(&g.ship, current_station_ptr()));
    const station_t* current_station = current_station_ptr();
    const station_t* navigation_station = navigation_station_ptr();
    station_ui_state_t ui = { 0 };
    if (g.docked) {
        build_station_ui_state(&ui);
    }
    int station_distance = 0;

    vec2 forward = v2_from_angle(g.ship.angle);
    vec2 home = v2(0.0f, -1.0f);
    if (navigation_station != NULL) {
        station_distance = (int)lroundf(v2_len(v2_sub(navigation_station->pos, g.ship.pos)));
        home = v2_norm(v2_sub(navigation_station->pos, g.ship.pos));
    }
    float bearing = atan2f(v2_cross(forward, home), v2_dot(forward, home));
    int bearing_degrees = (int)lroundf(fabsf(bearing) * (180.0f / PI_F));
    const char* bearing_side = "ahead";
    if (bearing > 0.12f) {
        bearing_side = "left";
    } else if (bearing < -0.12f) {
        bearing_side = "right";
    } else {
        bearing_degrees = 0;
    }

    sdtx_canvas(screen_w / ui_text_zoom(), screen_h / ui_text_zoom());
    sdtx_font(0);
    sdtx_origin(0.0f, 0.0f);
    sdtx_home();
    if (hud_should_draw_message_panel()) {
        int message_cols = 0;
        get_hud_message_panel_rect(&message_x, &message_y, &message_w, &message_h);
        build_hud_message(message_label, sizeof(message_label), message_text, sizeof(message_text), &message_r, &message_g, &message_b);
        message_cols = (int)((message_w - 28.0f) / (HUD_CELL * ui_text_zoom()));
        split_hud_message_lines(message_text, message_cols, message_line0, sizeof(message_line0), message_line1, sizeof(message_line1));
    }

    if (compact) {
        const char* nav_role = navigation_station != NULL ? station_role_short_name(navigation_station->role) : "STN";
        const char* dock_role = current_station != NULL ? station_role_short_name(current_station->role) : "STN";
        const char* bearing_mark = "A";
        if (bearing > 0.12f) {
            bearing_mark = "L";
        } else if (bearing < -0.12f) {
            bearing_mark = "R";
        }

        sdtx_pos(top_text_x, top_row_0);
        sdtx_color3b(232, 241, 255);
        sdtx_printf("%s // CR %d", g.docked ? "RUN" : "SHIP", credits);

        sdtx_pos(top_text_x, top_row_1);
        sdtx_color3b(203, 220, 248);
        sdtx_printf("H %d/%d  C %d/%d", hull_units, hull_capacity, cargo_units, cargo_capacity);

        sdtx_pos(top_text_x, top_row_2);
        if (g.docked) {
            sdtx_color3b(112, 255, 214);
            sdtx_printf("%s // E launch", dock_role);
        } else if (g.in_dock_range) {
            sdtx_color3b(112, 255, 214);
            sdtx_puts("DOCK RING // E dock");
        } else {
            sdtx_color3b(199, 222, 255);
            sdtx_printf("%s %d u // %d %s", nav_role, station_distance, bearing_degrees, bearing_mark);
        }

        sdtx_pos(top_text_x, top_row_3);
        if (g.docked) {
            sdtx_color3b(130, 255, 235);
            if (station_has_service(STATION_SERVICE_ORE_BUYER)) {
                if (cargo_units > 0) {
                    sdtx_printf("ORE BOARD // HAUL %d", payout_preview);
                } else {
                    sdtx_puts("ORE BOARD // HOLD EMPTY");
                }
            } else {
                sdtx_printf("%s CONSOLE", dock_role);
            }
        } else if ((g.hover_asteroid >= 0) && g.asteroids[g.hover_asteroid].active) {
            const asteroid_t* asteroid = &g.asteroids[g.hover_asteroid];
            int integrity_left = (int)lroundf(asteroid->hp);
            sdtx_color3b(130, 255, 235);
            sdtx_printf("TGT %s // %s // %d HP", asteroid_tier_name(asteroid->tier), commodity_code(asteroid->commodity), integrity_left);
        } else if (g.nearby_fragments > 0) {
            sdtx_color3b(130, 255, 235);
            if (g.tractor_fragments > 0) {
                sdtx_printf("TRACTOR // %d FRAG", g.tractor_fragments);
            } else {
                sdtx_printf("FRAGMENTS // %d", g.nearby_fragments);
            }
        } else if (cargo_units >= cargo_capacity) {
            sdtx_color3b(255, 221, 119);
            sdtx_puts("HOLD FULL // RETURN");
        } else {
            sdtx_color3b(169, 179, 204);
            sdtx_puts("FIELD CLEAR // SCAN");
        }

        sdtx_pos(bottom_text_x, bottom_text_y);
        sdtx_color3b(145, 160, 188);
        if (g.docked) {
            if (current_station->role == STATION_ROLE_REFINERY) {
                sdtx_puts("1 sell  2 repair  E launch");
            } else if (current_station->role == STATION_ROLE_YARD) {
                sdtx_puts("2 repair  4 hold  E launch");
            } else {
                sdtx_puts("2 repair  3 laser  5 tractor");
            }
        } else {
            sdtx_puts("W/S thrust  A/D turn  SPC mine  E dock");
        }

        if (hud_should_draw_message_panel()) {
            float message_text_x = ui_text_pos(message_x + 16.0f);
            float message_row_0 = ui_text_pos(message_y + 14.0f);
            float message_row_1 = ui_text_pos(message_y + 24.0f);
            float message_row_2 = ui_text_pos(message_y + 34.0f);

            sdtx_pos(message_text_x, message_row_0);
            sdtx_color3b(message_r, message_g, message_b);
            sdtx_puts(message_label);

            sdtx_pos(message_text_x, message_row_1);
            sdtx_color3b(232, 241, 255);
            sdtx_puts(message_line0);

            if (message_line1[0] != '\0') {
                sdtx_pos(message_text_x, message_row_2);
                sdtx_color3b(169, 179, 204);
                sdtx_puts(message_line1);
            }
        }

        draw_station_services(&ui);
        return;
    }

    sdtx_pos(top_text_x, top_row_0);
    sdtx_color3b(232, 241, 255);
    sdtx_puts(g.docked ? "RUN STATUS" : "SHIP STATUS");

    sdtx_pos(top_text_x, top_row_1);
    sdtx_color3b(203, 220, 248);
    sdtx_printf("CR %d  H %d/%d  C %d/%d", credits, hull_units, hull_capacity, cargo_units, cargo_capacity);

    sdtx_pos(top_text_x, top_row_2);
    if (g.docked) {
        sdtx_color3b(112, 255, 214);
        sdtx_printf("%s // docked // E launch", current_station->name);
    } else if (g.in_dock_range) {
        sdtx_color3b(112, 255, 214);
        sdtx_puts("Dock ring hot // E to dock");
    } else {
        sdtx_color3b(199, 222, 255);
        sdtx_printf("%s %d u // %d deg %s",
            navigation_station != NULL ? navigation_station->name : "Station",
            station_distance,
            bearing_degrees,
            bearing_side);
    }

    sdtx_pos(top_text_x, top_row_3);
    if (g.docked) {
        sdtx_color3b(130, 255, 235);
        if (station_has_service(STATION_SERVICE_ORE_BUYER)) {
            if (cargo_units > 0) {
                sdtx_printf("Ore board // haul %d", payout_preview);
            } else {
                sdtx_puts("Ore board // hold empty");
            }
        } else {
            sdtx_printf("%s console", station_role_name(current_station->role));
        }
    } else if ((g.hover_asteroid >= 0) && g.asteroids[g.hover_asteroid].active) {
        const asteroid_t* asteroid = &g.asteroids[g.hover_asteroid];
        int integrity_left = (int)lroundf(asteroid->hp);
        sdtx_color3b(130, 255, 235);
        sdtx_printf("Target %s %s // %s // %d hp", asteroid_tier_name(asteroid->tier), asteroid_tier_kind(asteroid->tier), commodity_short_name(asteroid->commodity), integrity_left);
    } else if (g.nearby_fragments > 0) {
        sdtx_color3b(130, 255, 235);
        if (g.tractor_fragments > 0) {
            sdtx_printf("Tractor lock // %d frag%s", g.tractor_fragments, g.tractor_fragments == 1 ? "" : "s");
        } else {
            sdtx_printf("Nearby fragments // %d", g.nearby_fragments);
        }
    } else if (cargo_units >= cargo_capacity) {
        sdtx_color3b(255, 221, 119);
        sdtx_puts("Hold full // return run");
    } else {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("No target // line up a rock");
    }

    sdtx_pos(bottom_text_x, bottom_text_y);
    sdtx_color3b(145, 160, 188);
    if (g.docked) {
        if (current_station->role == STATION_ROLE_REFINERY) {
            sdtx_puts("1 sell  2 repair  E launch  R reset  ESC quit");
        } else if (current_station->role == STATION_ROLE_YARD) {
            sdtx_puts("2 repair  4 hold  E launch  R reset  ESC quit");
        } else {
            sdtx_puts("2 repair  3 laser  5 tractor  E launch  R reset  ESC quit");
        }
    } else {
        sdtx_puts("W/S thrust  A/D turn  SPACE mine  E dock  R reset  ESC quit");
    }

    if (hud_should_draw_message_panel()) {
        float message_text_x = ui_text_pos(message_x + 16.0f);
        float message_row_0 = ui_text_pos(message_y + 16.0f);
        float message_row_1 = ui_text_pos(message_y + 30.0f);
        float message_row_2 = ui_text_pos(message_y + 42.0f);

        sdtx_pos(message_text_x, message_row_0);
        sdtx_color3b(message_r, message_g, message_b);
        sdtx_puts(message_label);

        sdtx_pos(message_text_x, message_row_1);
        sdtx_color3b(232, 241, 255);
        sdtx_puts(message_line0);

        if (message_line1[0] != '\0') {
            sdtx_pos(message_text_x, message_row_2);
            sdtx_color3b(169, 179, 204);
            sdtx_puts(message_line1);
        }
    }

    /* --- Multiplayer HUD indicator --- */
    if (g.multiplayer_enabled && net_is_connected()) {
        float mp_x = ui_text_pos(screen_w - (compact ? 100.0f : 120.0f));
        float mp_y = ui_text_pos(8.0f);
        sdtx_pos(mp_x, mp_y);
        sdtx_color3b(80, 255, 180);
        sdtx_printf("MP:%d", net_remote_player_count() + 1);
    }

    draw_station_services(&ui);
}

static void resolve_ship_circle(vec2 center, float radius) {
    vec2 delta = v2_sub(g.ship.pos, center);
    float minimum = radius + ship_hull_def(&g.ship)->ship_radius;
    float distance_sq = v2_len_sq(delta);
    float minimum_sq = minimum * minimum;
    if (distance_sq >= minimum_sq) {
        return;
    }

    float distance = sqrtf(distance_sq);
    vec2 normal = distance > 0.00001f ? v2_scale(delta, 1.0f / distance) : v2(1.0f, 0.0f);
    g.ship.pos = v2_add(center, v2_scale(normal, minimum));

    float velocity_towards_surface = v2_dot(g.ship.vel, normal);
    if (velocity_towards_surface < 0.0f) {
        float impact_speed = -velocity_towards_surface;
        if (!g.docked && (impact_speed > SHIP_COLLISION_DAMAGE_THRESHOLD)) {
            apply_ship_damage((impact_speed - SHIP_COLLISION_DAMAGE_THRESHOLD) * SHIP_COLLISION_DAMAGE_SCALE);
        }
        g.ship.vel = v2_sub(g.ship.vel, v2_scale(normal, velocity_towards_surface * 1.2f));
    }
}

static bool is_key_down(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_down[key];
}

static bool is_key_pressed(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_pressed[key];
}

static vec2 ship_forward(void) {
    return v2_from_angle(g.ship.angle);
}

static vec2 ship_muzzle(vec2 forward) {
    return v2_add(g.ship.pos, v2_scale(forward, ship_hull_def(&g.ship)->ship_radius + 8.0f));
}

static bool try_spend_credits(float amount) {
    if (amount <= 0.0f) {
        return true;
    }
    if (g.ship.credits + 0.01f < amount) {
        return false;
    }
    g.ship.credits = fmaxf(0.0f, g.ship.credits - amount);
    return true;
}

static void anchor_ship_in_station(void) {
    g.ship.pos = station_dock_anchor();
    g.ship.vel = v2(0.0f, 0.0f);
}

static void dock_ship(void) {
    if (g.nearby_station >= 0) {
        g.current_station = g.nearby_station;
    }
    g.docked = true;
    g.in_dock_range = true;
    anchor_ship_in_station();
    audio_play_dock(&g.audio);
    set_notice("Docked at %s.", current_station_ptr()->name);
}

static void launch_ship(void) {
    g.docked = false;
    g.nearby_station = g.current_station;
    g.in_dock_range = true;
    anchor_ship_in_station();
    audio_play_launch(&g.audio);
    set_notice("Launch corridor clear. Press thrust when ready.");
}

static void emergency_recover_ship(void) {
    int lost_units = (int)lroundf(ship_raw_ore_total(&g.ship));
    clear_ship_cargo();
    g.ship.hull = ship_max_hull(&g.ship);
    g.ship.angle = PI_F * 0.5f;
    dock_ship();
    if (lost_units > 0) {
        set_notice("Tow drones recovered the hull. Lost %d ore in the breach.", lost_units);
    } else {
        set_notice("Tow drones recovered the hull. Repair crews have you docked.");
    }
}

static void apply_ship_damage(float damage) {
    if (damage <= 0.0f) {
        return;
    }

    audio_play_damage(&g.audio,damage);
    g.ship.hull = fmaxf(0.0f, g.ship.hull - damage);
    if (g.ship.hull <= 0.01f) {
        emergency_recover_ship();
    }
}

static void try_sell_station_cargo(void) {
    station_t* station = &g.stations[g.current_station];
    float payout_total = 0.0f;
    int sold_units = 0;
    int sold_types = 0;
    commodity_t sold_commodity = COMMODITY_COUNT;
    if (!station_has_service(STATION_SERVICE_ORE_BUYER)) {
        set_notice("%s doesn't buy raw ore.", station->name);
        return;
    }
    if (ship_raw_ore_total(&g.ship) <= 0.01f) {
        set_notice("Cargo hold empty.");
        return;
    }

    for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
        commodity_t ore = (commodity_t)i;
        float amount = ship_cargo_amount(&g.ship,ore);
        if (amount <= 0.01f) {
            continue;
        }
        float hopper_space = REFINERY_HOPPER_CAPACITY - station->ore_buffer[ore];
        if (hopper_space <= 0.01f) {
            continue;
        }
        float accepted = fminf(amount, hopper_space);
        payout_total += accepted * station_buy_price(station, ore);
        station->ore_buffer[ore] += accepted;
        sold_units += (int)lroundf(accepted);
        sold_types++;
        sold_commodity = ore;
        g.ship.cargo[ore] -= accepted;
    }

    if (sold_units == 0) {
        set_notice("Hoppers full. Smelting in progress.");
        return;
    }

    int payout = (int)lroundf(payout_total);
    g.ship.credits += payout_total;
    audio_play_sale(&g.audio);
    if (sold_types > 1) {
        set_notice("Sold %d ore across %d veins for %d cr.", sold_units, sold_types, payout);
    } else {
        set_notice("Sold %d %s for %d cr.", sold_units, commodity_name(sold_commodity), payout);
    }
}

static void try_repair_ship(void) {
    const station_t* station = current_station_ptr();
    float repair_cost = station_repair_cost(&g.ship, current_station_ptr());
    if (!station_has_service(STATION_SERVICE_REPAIR)) {
        set_notice("%s repair crews are offline.", station->name);
        return;
    }
    if (repair_cost <= 0.0f) {
        set_notice("Hull already nominal.");
        return;
    }
    if (!try_spend_credits(repair_cost)) {
        set_notice("Need %d cr for hull repair.", (int)lroundf(repair_cost));
        return;
    }

    g.ship.hull = ship_max_hull(&g.ship);
    audio_play_repair(&g.audio);
    set_notice("Hull restored for %d cr.", (int)lroundf(repair_cost));
}

static void try_apply_ship_upgrade(ship_upgrade_t upgrade) {
    station_t* station = &g.stations[g.current_station];
    uint32_t required_service = station_upgrade_service(upgrade);

    if (!station_has_service(required_service)) {
        switch (upgrade) {
            case SHIP_UPGRADE_MINING:
                set_notice("%s doesn't tune laser arrays.", station->name);
                break;
            case SHIP_UPGRADE_HOLD:
                set_notice("%s doesn't refit hold racks.", station->name);
                break;
            case SHIP_UPGRADE_TRACTOR:
                set_notice("%s doesn't tune tractor coils.", station->name);
                break;
            case SHIP_UPGRADE_COUNT:
            default:
                break;
        }
        return;
    }
    if (ship_upgrade_maxed(&g.ship,upgrade)) {
        switch (upgrade) {
            case SHIP_UPGRADE_MINING:
                set_notice("Laser array already tuned to spec.");
                break;
            case SHIP_UPGRADE_HOLD:
                set_notice("Hold racks already at station limit.");
                break;
            case SHIP_UPGRADE_TRACTOR:
                set_notice("Tractor coil already at station limit.");
                break;
            case SHIP_UPGRADE_COUNT:
            default:
                break;
        }
        return;
    }

    product_t required = upgrade_required_product(upgrade);
    float product_cost = upgrade_product_cost(&g.ship,upgrade);
    if (station->product_stock[required] < product_cost - 0.01f) {
        int have = (int)lroundf(station->product_stock[required]);
        int need = (int)lroundf(product_cost);
        set_notice("Need %d %s, station has %d. Awaiting supply.", need, product_name(required), have);
        return;
    }

    int cost = ship_upgrade_cost(&g.ship,upgrade);
    if (!try_spend_credits((float)cost)) {
        set_notice("Need %d cr for this upgrade.", cost);
        return;
    }

    station->product_stock[required] -= product_cost;

    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            g.ship.mining_level++;
            audio_play_upgrade(&g.audio,upgrade);
            set_notice("Laser array tuned. Output now %d ore/sec.", (int)lroundf(ship_mining_rate(&g.ship)));
            break;
        case SHIP_UPGRADE_HOLD:
            g.ship.hold_level++;
            audio_play_upgrade(&g.audio,upgrade);
            set_notice("Hold racks expanded. Capacity now %d ore.", (int)lroundf(ship_cargo_capacity(&g.ship)));
            break;
        case SHIP_UPGRADE_TRACTOR:
            g.ship.tractor_level++;
            audio_play_upgrade(&g.audio,upgrade);
            set_notice("Tractor coil widened to %d units.", (int)lroundf(ship_tractor_range(&g.ship)));
            break;
        case SHIP_UPGRADE_COUNT:
        default:
            break;
    }
}

static void reset_step_feedback(void) {
    g.hover_asteroid = -1;
    g.beam_active = false;
    g.beam_hit = false;
    g.thrusting = false;
    g.nearby_fragments = 0;
    g.tractor_fragments = 0;
}

static input_intent_t sample_input_intent(void) {
    input_intent_t intent = { 0 };

    if (is_key_down(SAPP_KEYCODE_A) || is_key_down(SAPP_KEYCODE_LEFT)) {
        intent.turn += 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_D) || is_key_down(SAPP_KEYCODE_RIGHT)) {
        intent.turn -= 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_W) || is_key_down(SAPP_KEYCODE_UP)) {
        intent.thrust += 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_S) || is_key_down(SAPP_KEYCODE_DOWN)) {
        intent.thrust -= 1.0f;
    }

    intent.mine = is_key_down(SAPP_KEYCODE_SPACE);
    intent.interact = is_key_pressed(SAPP_KEYCODE_E);
    intent.service_sell = is_key_pressed(SAPP_KEYCODE_1);
    intent.service_repair = is_key_pressed(SAPP_KEYCODE_2);
    intent.upgrade_mining = is_key_pressed(SAPP_KEYCODE_3);
    intent.upgrade_hold = is_key_pressed(SAPP_KEYCODE_4);
    intent.upgrade_tractor = is_key_pressed(SAPP_KEYCODE_5);
    intent.reset = is_key_pressed(SAPP_KEYCODE_R);
    return intent;
}

static void step_ship_rotation(float dt, float turn_input) {
    g.ship.angle = wrap_angle(g.ship.angle + (turn_input * ship_hull_def(&g.ship)->turn_speed * dt));
}

static void step_ship_thrust(float dt, float thrust_input, vec2 forward) {
    const hull_def_t* hull = ship_hull_def(&g.ship);
    if (thrust_input > 0.0f) {
        g.ship.vel = v2_add(g.ship.vel, v2_scale(forward, hull->accel * thrust_input * dt));
        g.thrusting = true;
    } else if (thrust_input < 0.0f) {
        g.ship.vel = v2_add(g.ship.vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
    }
}

static void step_ship_motion(float dt) {
    g.ship.vel = v2_scale(g.ship.vel, 1.0f / (1.0f + (ship_hull_def(&g.ship)->drag * dt)));
    g.ship.pos = v2_add(g.ship.pos, v2_scale(g.ship.vel, dt));

    float world_distance_sq = v2_len_sq(g.ship.pos);
    float world_radius_sq = WORLD_RADIUS * WORLD_RADIUS;
    if (world_distance_sq > world_radius_sq) {
        float world_distance = sqrtf(world_distance_sq);
        vec2 push_dir = v2_scale(g.ship.pos, 1.0f / world_distance);
        vec2 push_home = v2_scale(push_dir, -(world_distance - WORLD_RADIUS) * 0.08f);
        g.ship.vel = v2_add(g.ship.vel, push_home);
    }
}

static void resolve_world_collisions(void) {
    for (int i = 0; i < MAX_STATIONS; i++) {
        resolve_ship_circle(g.stations[i].pos, g.stations[i].radius + 4.0f);
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.asteroids[i].active || asteroid_is_collectible(&g.asteroids[i])) {
            continue;
        }
        resolve_ship_circle(g.asteroids[i].pos, g.asteroids[i].radius);
    }
}

static void update_docking_state(float dt) {
    if (g.docked) {
        g.in_dock_range = true;
        g.nearby_station = g.current_station;
        anchor_ship_in_station();
        return;
    }

    float best_distance_sq = 0.0f;
    g.nearby_station = -1;

    for (int i = 0; i < MAX_STATIONS; i++) {
        float dock_radius_sq = g.stations[i].dock_radius * g.stations[i].dock_radius;
        float distance_sq = v2_dist_sq(g.ship.pos, g.stations[i].pos);
        if ((distance_sq <= dock_radius_sq) && ((g.nearby_station < 0) || (distance_sq < best_distance_sq))) {
            best_distance_sq = distance_sq;
            g.nearby_station = i;
        }
    }

    g.in_dock_range = g.nearby_station >= 0;
    if (g.in_dock_range) {
        g.ship.vel = v2_scale(g.ship.vel, 1.0f / (1.0f + (dt * 2.2f)));
    }
}

static void update_targeting_state(vec2 forward) {
    g.hover_asteroid = find_mining_target(g.asteroids, MAX_ASTEROIDS, ship_muzzle(forward), forward, MINING_RANGE);
}

static void step_fragment_collection(float dt) {
    float nearby_range_sq = FRAGMENT_NEARBY_RANGE * FRAGMENT_NEARBY_RANGE;
    float tractor_range = ship_tractor_range(&g.ship);
    float tractor_range_sq = tractor_range * tractor_range;
    float cargo_space = ship_cargo_space();
    float collected_ore = 0.0f;
    int collected_fragments = 0;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t* asteroid = &g.asteroids[i];
        if (!asteroid_is_collectible(asteroid)) {
            continue;
        }

        vec2 to_ship = v2_sub(g.ship.pos, asteroid->pos);
        float distance_sq = v2_len_sq(to_ship);
        if (distance_sq <= nearby_range_sq) {
            g.nearby_fragments++;
        }

        if (cargo_space <= 0.0f) {
            continue;
        }

        if (distance_sq <= tractor_range_sq) {
            float distance = sqrtf(distance_sq);
            float pull_ratio = 1.0f - clampf(distance / tractor_range, 0.0f, 1.0f);
            vec2 pull_dir = distance > 0.001f ? v2_scale(to_ship, 1.0f / distance) : ship_forward();
            g.tractor_fragments++;
            asteroid->vel = v2_add(asteroid->vel, v2_scale(pull_dir, FRAGMENT_TRACTOR_ACCEL * lerpf(0.35f, 1.0f, pull_ratio) * dt));
            float speed = v2_len(asteroid->vel);
            if (speed > FRAGMENT_MAX_SPEED) {
                asteroid->vel = v2_scale(v2_norm(asteroid->vel), FRAGMENT_MAX_SPEED);
            }
        }

        float collect_radius = ship_collect_radius(&g.ship) + asteroid->radius;
        if (distance_sq <= (collect_radius * collect_radius)) {
            float recovered = fminf(asteroid->ore, cargo_space);
            if (recovered <= 0.0f) {
                continue;
            }

            g.ship.cargo[asteroid->commodity] += recovered;
            cargo_space -= recovered;
            asteroid->ore -= recovered;
            collected_ore += recovered;

            if (asteroid->ore <= 0.01f) {
                clear_asteroid(asteroid);
                collected_fragments++;
            } else if (asteroid->max_ore > 0.0f) {
                asteroid->radius = lerpf(asteroid_radius_min(ASTEROID_TIER_S) * 0.72f, asteroid_radius_max(ASTEROID_TIER_S), asteroid_progress_ratio(asteroid));
            }
        }
    }

    if (collected_ore > 0.0f) {
        audio_play_pickup(&g.audio,collected_ore, collected_fragments);
    }
    push_collection_feedback(collected_ore, collected_fragments);
}

static void step_mining_system(float dt, bool mining, vec2 forward) {
    if (!mining) {
        return;
    }

    vec2 muzzle = ship_muzzle(forward);
    g.beam_active = true;
    g.beam_start = muzzle;

    if (g.hover_asteroid >= 0) {
        asteroid_t* asteroid = &g.asteroids[g.hover_asteroid];
        vec2 to_asteroid = v2_sub(asteroid->pos, muzzle);
        vec2 normal = v2_norm(to_asteroid);
        g.beam_end = v2_sub(asteroid->pos, v2_scale(normal, asteroid->radius * 0.85f));
        g.beam_hit = true;
        audio_play_mining_tick(&g.audio);

        /* Apply damage locally (single-player) or as prediction (multiplayer).
         * Server corrects via asteroid state snapshots. */
        float mined = fminf(ship_mining_rate(&g.ship) * dt, asteroid->hp);
        asteroid->hp -= mined;
        if (asteroid->hp <= 0.01f) {
            fracture_asteroid(g.hover_asteroid, normal);
        }
    } else {
        g.beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
    }
}

static void step_station_interaction_system(const input_intent_t* intent) {
    if (intent->interact) {
        if (g.docked) {
            launch_ship();
            return;
        }
        if (!g.in_dock_range) {
            set_notice("Enter station ring to dock.");
            return;
        }
        dock_ship();
        return;
    }

    if (!g.docked) {
        return;
    }

    if (intent->service_sell) {
        try_sell_station_cargo();
    } else if (intent->service_repair) {
        try_repair_ship();
    } else if (intent->upgrade_mining) {
        try_apply_ship_upgrade(SHIP_UPGRADE_MINING);
    } else if (intent->upgrade_hold) {
        try_apply_ship_upgrade(SHIP_UPGRADE_HOLD);
    } else if (intent->upgrade_tractor) {
        try_apply_ship_upgrade(SHIP_UPGRADE_TRACTOR);
    }
}

static void step_notice_timer(float dt) {
    if (g.notice_timer > 0.0f) {
        g.notice_timer = fmaxf(0.0f, g.notice_timer - dt);
    }

    if (g.collection_feedback_timer > 0.0f) {
        g.collection_feedback_timer = fmaxf(0.0f, g.collection_feedback_timer - dt);
        if (g.collection_feedback_timer <= 0.0f) {
            clear_collection_feedback();
        }
    }
}

static void step_hauler(npc_ship_t* npc, int n, float dt) {
    const hull_def_t* hull = npc_hull_def(npc);
    switch (npc->state) {
        case NPC_STATE_DOCKED: {
            npc->state_timer -= dt;
            npc->vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
                station_t* home = &g.stations[npc->home_station];
                station_t* dest = &g.stations[npc->dest_station];
                float space = hull->ingot_capacity;
                bool loaded = false;
                if (dest->role == STATION_ROLE_YARD) {
                    commodity_t ingot = COMMODITY_FRAME_INGOT;
                    float take = fminf(home->inventory[ingot], space);
                    if (take > 0.5f) {
                        npc->ingots[INGOT_IDX(ingot)] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
                    }
                } else if (dest->role == STATION_ROLE_BEAMWORKS) {
                    commodity_t ingots[2] = { COMMODITY_CONDUCTOR_INGOT, COMMODITY_LENS_INGOT };
                    for (int i = 0; i < 2; i++) {
                        float take = fminf(home->inventory[ingots[i]], space / 2.0f);
                        if (take > 0.01f) {
                            npc->ingots[INGOT_IDX(ingots[i])] += take;
                            home->inventory[ingots[i]] -= take;
                            space -= take;
                            loaded = true;
                        }
                    }
                }
                if (loaded) {
                    npc->state = NPC_STATE_TRAVEL_TO_DEST;
                } else {
                    npc->state_timer = HAULER_LOAD_TIME;
                }
            }
            break;
        }

        case NPC_STATE_TRAVEL_TO_DEST: {
            station_t* dest = &g.stations[npc->dest_station];
            npc_steer_toward(npc, dest->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt);

            float dock_dist_sq = v2_dist_sq(npc->pos, dest->pos);
            float dock_r = dest->dock_radius * 0.7f;
            if (dock_dist_sq < (dock_r * dock_r)) {
                npc->vel = v2(0.0f, 0.0f);
                npc->pos = v2_add(dest->pos, v2(30.0f * (float)(n % 2 == 0 ? -1 : 1), -(dest->radius + hull->ship_radius + 50.0f)));
                npc->state = NPC_STATE_UNLOADING;
                npc->state_timer = HAULER_LOAD_TIME;
            }
            break;
        }

        case NPC_STATE_UNLOADING: {
            npc->state_timer -= dt;
            npc->vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
                station_t* dest = &g.stations[npc->dest_station];
                for (int i = 0; i < INGOT_COUNT; i++) {
                    dest->ingot_buffer[i] += npc->ingots[i];
                    npc->ingots[i] = 0.0f;
                }
                npc->state = NPC_STATE_RETURN_TO_STATION;
            }
            break;
        }

        case NPC_STATE_RETURN_TO_STATION: {
            station_t* home = &g.stations[npc->home_station];
            npc_steer_toward(npc, home->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt);

            float dock_dist_sq = v2_dist_sq(npc->pos, home->pos);
            float dock_r = home->dock_radius * 0.7f;
            if (dock_dist_sq < (dock_r * dock_r)) {
                npc->vel = v2(0.0f, 0.0f);
                npc->pos = v2_add(home->pos, v2(50.0f * (float)(n % 2 == 0 ? -1 : 1), -(home->radius + hull->ship_radius + 70.0f)));
                npc->state = NPC_STATE_DOCKED;
                npc->state_timer = HAULER_DOCK_TIME;
            }
            break;
        }

        default:
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
    }
}

static void step_npc_ships(float dt) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t* npc = &g.npc_ships[n];
        if (!npc->active) continue;
        npc->thrusting = false;

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(npc, n, dt);
            continue;
        }

        const hull_def_t* hull = npc_hull_def(npc);
        switch (npc->state) {
            case NPC_STATE_DOCKED: {
                npc->state_timer -= dt;
                npc->vel = v2(0.0f, 0.0f);
                if (npc->state_timer <= 0.0f) {
                    int target = npc_find_mineable_asteroid(npc, g.asteroids, MAX_ASTEROIDS);
                    if (target >= 0) {
                        npc->target_asteroid = target;
                        npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                    } else {
                        npc->state = NPC_STATE_IDLE;
                        npc->state_timer = 2.0f;
                    }
                }
                break;
            }

            case NPC_STATE_TRAVEL_TO_ASTEROID: {
                if (!npc_target_valid(npc, g.asteroids, MAX_ASTEROIDS)) {
                    int target = npc_find_mineable_asteroid(npc, g.asteroids, MAX_ASTEROIDS);
                    if (target >= 0) {
                        npc->target_asteroid = target;
                    } else {
                        npc->state = NPC_STATE_RETURN_TO_STATION;
                        break;
                    }
                }

                asteroid_t* asteroid = &g.asteroids[npc->target_asteroid];
                npc_steer_toward(npc, asteroid->pos, hull->accel, hull->turn_speed, dt);
                npc_apply_physics(npc, hull->drag, dt);

                float dist_sq = v2_dist_sq(npc->pos, asteroid->pos);
                if (dist_sq < (MINING_RANGE * MINING_RANGE)) {
                    npc->state = NPC_STATE_MINING;
                }
                break;
            }

            case NPC_STATE_MINING: {
                if (!npc_target_valid(npc, g.asteroids, MAX_ASTEROIDS)) {
                    if (npc_total_cargo(npc) > 0.5f) {
                        npc->state = NPC_STATE_RETURN_TO_STATION;
                    } else {
                        int target = npc_find_mineable_asteroid(npc, g.asteroids, MAX_ASTEROIDS);
                        if (target >= 0) {
                            npc->target_asteroid = target;
                            npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                        } else {
                            npc->state = NPC_STATE_RETURN_TO_STATION;
                        }
                    }
                    break;
                }

                asteroid_t* asteroid = &g.asteroids[npc->target_asteroid];
                float dist_sq = v2_dist_sq(npc->pos, asteroid->pos);
                float standoff = asteroid->radius + 60.0f;
                float approach = standoff + 20.0f;

                if (dist_sq > approach * approach) {
                    npc_steer_toward(npc, asteroid->pos, hull->accel, hull->turn_speed, dt);
                    npc_apply_physics(npc, hull->drag, dt);
                    break;
                }

                vec2 face_dir = v2_sub(asteroid->pos, npc->pos);
                float desired_angle = atan2f(face_dir.y, face_dir.x);
                float diff = wrap_angle(desired_angle - npc->angle);
                float max_turn = hull->turn_speed * dt;
                if (diff > max_turn) diff = max_turn;
                else if (diff < -max_turn) diff = -max_turn;
                npc->angle = wrap_angle(npc->angle + diff);

                if (dist_sq < standoff * standoff) {
                    vec2 away = v2_norm(v2_sub(npc->pos, asteroid->pos));
                    npc->vel = v2_add(npc->vel, v2_scale(away, hull->accel * 0.5f * dt));
                }
                npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (4.0f * dt)));
                npc_apply_physics(npc, hull->drag, dt);

                float mined = hull->mining_rate * dt;
                mined = fminf(mined, asteroid->hp);
                asteroid->hp -= mined;

                float cargo_space = hull->ore_capacity - npc_total_cargo(npc);
                float ore_gained = mined * 0.15f;
                ore_gained = fminf(ore_gained, cargo_space);
                if (ore_gained > 0.0f) {
                    npc->cargo[asteroid->commodity] += ore_gained;
                }

                if (asteroid->hp <= 0.01f) {
                    vec2 outward = v2_norm(v2_sub(asteroid->pos, npc->pos));
                    fracture_asteroid(npc->target_asteroid, outward);
                    npc->target_asteroid = -1;
                }

                if (cargo_space <= 0.5f) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    npc->target_asteroid = -1;
                }
                break;
            }

            case NPC_STATE_RETURN_TO_STATION: {
                station_t* home = &g.stations[npc->home_station];
                npc_steer_toward(npc, home->pos, hull->accel, hull->turn_speed, dt);
                npc_apply_physics(npc, hull->drag, dt);

                float dock_dist_sq = v2_dist_sq(npc->pos, home->pos);
                float dock_r = home->dock_radius * 0.7f;
                if (dock_dist_sq < (dock_r * dock_r)) {
                    npc->vel = v2(0.0f, 0.0f);
                    npc->pos = v2_add(home->pos, v2(30.0f * (float)(n % 3 - 1), -(home->radius + hull->ship_radius + 50.0f)));

                    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
                        float space = REFINERY_HOPPER_CAPACITY - home->ore_buffer[i];
                        float deposit = fminf(npc->cargo[i], fmaxf(0.0f, space));
                        home->ore_buffer[i] += deposit;
                        npc->cargo[i] -= deposit;
                    }

                    npc->state = NPC_STATE_DOCKED;
                    npc->state_timer = NPC_DOCK_TIME;
                    npc->target_asteroid = -1;
                }
                break;
            }

            case NPC_STATE_IDLE: {
                npc->state_timer -= dt;
                if (npc->state_timer <= 0.0f) {
                    int target = npc_find_mineable_asteroid(npc, g.asteroids, MAX_ASTEROIDS);
                    if (target >= 0) {
                        npc->target_asteroid = target;
                        npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                    } else {
                        npc->state_timer = 3.0f;
                    }
                }
                break;
            }

            case NPC_STATE_TRAVEL_TO_DEST:
            case NPC_STATE_UNLOADING:
                break;
        }
    }
}

/* step_refinery_production, step_station_production: see economy.h/c */

static void sync_globals_to_world(void) {
    server_player_t* sp = &g.world.players[0];
    sp->connected = true;
    sp->id = 0;
    sp->ship = g.ship;
    sp->input = (input_intent_t){0};
    sp->current_station = g.current_station;
    sp->nearby_station = g.nearby_station;
    sp->docked = g.docked;
    sp->in_dock_range = g.in_dock_range;
    sp->hover_asteroid = g.hover_asteroid;
    sp->beam_active = g.beam_active;
    sp->beam_hit = g.beam_hit;
    sp->beam_start = g.beam_start;
    sp->beam_end = g.beam_end;
    sp->nearby_fragments = g.nearby_fragments;
    sp->tractor_fragments = g.tractor_fragments;
    memcpy(g.world.stations, g.stations, sizeof(g.stations));
    memcpy(g.world.asteroids, g.asteroids, sizeof(g.asteroids));
    memcpy(g.world.npc_ships, g.npc_ships, sizeof(g.npc_ships));
    g.world.rng = g.rng;
    g.world.time = g.time;
    g.world.field_spawn_timer = g.field_spawn_timer;
}

static void sync_world_to_globals(void) {
    server_player_t* sp = &g.world.players[0];
    g.ship = sp->ship;
    g.current_station = sp->current_station;
    g.nearby_station = sp->nearby_station;
    g.docked = sp->docked;
    g.in_dock_range = sp->in_dock_range;
    g.hover_asteroid = sp->hover_asteroid;
    g.beam_active = sp->beam_active;
    g.beam_hit = sp->beam_hit;
    g.beam_start = sp->beam_start;
    g.beam_end = sp->beam_end;
    g.nearby_fragments = sp->nearby_fragments;
    g.tractor_fragments = sp->tractor_fragments;
    memcpy(g.stations, g.world.stations, sizeof(g.stations));
    memcpy(g.asteroids, g.world.asteroids, sizeof(g.asteroids));
    memcpy(g.npc_ships, g.world.npc_ships, sizeof(g.npc_ships));
    g.rng = g.world.rng;
    g.time = g.world.time;
    g.field_spawn_timer = g.world.field_spawn_timer;
}

static void sim_step(float dt) {
    reset_step_feedback();
    audio_step(&g.audio, dt);

    input_intent_t intent = sample_input_intent();
    if (intent.reset) {
        reset_world();
        consume_pressed_input();
        return;
    }

    if (!g.multiplayer_enabled || !net_is_connected()) {
        /* Single player: run authoritative sim locally */
        sync_globals_to_world();
        g.world.players[0].input = intent;
        world_sim_step(&g.world, dt);
        sync_world_to_globals();
    } else {
        /* Multiplayer: server is authoritative for world state.
         * Run local player physics for responsiveness.
         * World state arrives via network callbacks. */
        g.time += dt;
        step_asteroid_dynamics(g.asteroids, MAX_ASTEROIDS, g.ship.pos, dt);
        step_npc_ships(dt);

        if (!g.docked) {
            step_ship_rotation(dt, intent.turn);
            vec2 forward = ship_forward();
            step_ship_thrust(dt, intent.thrust, forward);
            step_ship_motion(dt);
            resolve_world_collisions();
            update_docking_state(dt);
            if (!g.docked) {
                update_targeting_state(forward);
                step_mining_system(dt, intent.mine, forward);
                step_fragment_collection(dt);
            }
        } else {
            update_docking_state(dt);
        }
    }

    step_notice_timer(dt);

    /* Queue one-shot actions for the next net send (before key_pressed is consumed). */
    if (g.multiplayer_enabled && net_is_connected() && g.pending_net_action == 0) {
        if (is_key_pressed(SAPP_KEYCODE_E))
            g.pending_net_action = g.docked ? 2 : 1;
        else if (is_key_pressed(SAPP_KEYCODE_1))
            g.pending_net_action = 3;
        else if (is_key_pressed(SAPP_KEYCODE_2))
            g.pending_net_action = 4;
        else if (is_key_pressed(SAPP_KEYCODE_3))
            g.pending_net_action = 5;
        else if (is_key_pressed(SAPP_KEYCODE_4))
            g.pending_net_action = 6;
        else if (is_key_pressed(SAPP_KEYCODE_5))
            g.pending_net_action = 7;
    }

    consume_pressed_input();
}

/* Forward declarations for multiplayer callbacks (defined below init). */
static void apply_remote_asteroids(const NetAsteroidState* asteroids, int count);
static void apply_remote_npcs(const NetNpcState* npcs, int count);
static void apply_remote_stations(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock);
static void apply_remote_player_state(const NetPlayerState* state);
static void apply_remote_player_ship(const NetPlayerShipState* state);

static void init(void) {
    memset(&g, 0, sizeof(g));
    g.rng = 0xC0FFEE12u;

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    sgl_setup(&(sgl_desc_t){
        .logger.func = slog_func,
    });

    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_oric(),
        .logger.func = slog_func,
    });

    audio_init(&g.audio);

    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value = (sg_color){ 0.018f, 0.024f, 0.045f, 1.0f };

    init_starfield();
    reset_world();

    /* --- Multiplayer: auto-connect if server URL is available --- */
#ifdef __EMSCRIPTEN__
    {
        const char* server_url = emscripten_run_script_string(
            "(() => {"
            "  const p = new URLSearchParams(window.location.search);"
            "  return p.get('server') || 'wss://signal-ws.ratimics.com/ws';"
            "})()");
        if (server_url && server_url[0] != '\0') {
            NetCallbacks cbs = {0};
            cbs.on_state = apply_remote_player_state;
            cbs.on_asteroids = apply_remote_asteroids;
            cbs.on_npcs = apply_remote_npcs;
            cbs.on_stations = apply_remote_stations;
            cbs.on_player_ship = apply_remote_player_ship;
            g.multiplayer_enabled = net_init(server_url, &cbs);
        }
    }
#endif
}


/* --- Multiplayer: world state sync callbacks and broadcast --- */

static void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {

    /* Mark which indices were received. */
    bool received[MAX_ASTEROIDS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;
        received[idx] = true;

        asteroid_t* a = &g.asteroids[idx];
        a->active = (asteroids[i].flags & 1) != 0;
        a->fracture_child = (asteroids[i].flags & (1 << 1)) != 0;
        a->tier = (asteroid_tier_t)((asteroids[i].flags >> 2) & 0x3);
        a->commodity = (commodity_t)((asteroids[i].flags >> 4) & 0x7);
        a->pos.x = asteroids[i].x;
        a->pos.y = asteroids[i].y;
        a->vel.x = asteroids[i].vx;
        a->vel.y = asteroids[i].vy;
        a->hp    = asteroids[i].hp;
        a->ore   = asteroids[i].ore;
        a->radius = asteroids[i].radius;
    }

    /* Deactivate asteroids not in the update. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!received[i]) {
            g.asteroids[i].active = false;
        }
    }
}

static void apply_remote_npcs(const NetNpcState* npcs, int count) {
    bool received[MAX_NPC_SHIPS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;
        received[idx] = true;

        npc_ship_t* n = &g.npc_ships[idx];
        n->active = (npcs[i].flags & 1) != 0;
        n->role = (npc_role_t)((npcs[i].flags >> 1) & 0x3);
        n->state = (npc_state_t)((npcs[i].flags >> 3) & 0x7);
        n->thrusting = (npcs[i].flags & (1 << 6)) != 0;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->vel.x = npcs[i].vx;
        n->vel.y = npcs[i].vy;
        n->angle = npcs[i].angle;
        n->target_asteroid = (int)npcs[i].target_asteroid;
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!received[i]) {
            g.npc_ships[i].active = false;
        }
    }
}

static void apply_remote_stations(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.stations[index];
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        st->ore_buffer[i] = ore_buf[i];
    for (int i = 0; i < COMMODITY_COUNT; i++)
        st->inventory[i] = inventory[i];
    for (int i = 0; i < PRODUCT_COUNT; i++)
        st->product_stock[i] = product_stock[i];
}

static void apply_remote_player_state(const NetPlayerState* state) {
    /* Apply server-authoritative position for the local player. */
    if (state->player_id == net_local_id()) {
        /* Blend all values for smooth correction. */
        g.ship.pos.x = lerpf(g.ship.pos.x, state->x, 0.3f);
        g.ship.pos.y = lerpf(g.ship.pos.y, state->y, 0.3f);
        g.ship.vel.x = lerpf(g.ship.vel.x, state->vx, 0.3f);
        g.ship.vel.y = lerpf(g.ship.vel.y, state->vy, 0.3f);
        g.ship.angle = lerpf(g.ship.angle, state->angle, 0.3f);
    }
}

static void apply_remote_player_ship(const NetPlayerShipState* state) {
    /* Apply server-authoritative ship state for the local player. */
    g.ship.hull = state->hull;
    g.ship.credits = state->credits;
    g.ship.mining_level = (int)state->mining_level;
    g.ship.hold_level = (int)state->hold_level;
    g.ship.tractor_level = (int)state->tractor_level;
    g.ship.cargo[COMMODITY_FERRITE_ORE] = state->cargo_ferrite;
    g.ship.cargo[COMMODITY_CUPRITE_ORE] = state->cargo_cuprite;
    g.ship.cargo[COMMODITY_CRYSTAL_ORE] = state->cargo_crystal;
    g.docked = state->docked;
    g.current_station = (int)state->current_station;
    if (g.docked) {
        g.in_dock_range = true;
        g.nearby_station = g.current_station;
    }
}

/* --- Multiplayer: draw remote players as colored triangles --- */
static void draw_remote_players(void) {
    if (!g.multiplayer_enabled) return;
    const NetPlayerState* players = net_get_players();
    /* Color palette for remote players (6 distinct colors). */
    static const float colors[][3] = {
        {1.0f, 0.45f, 0.25f}, /* orange */
        {0.25f, 1.0f, 0.55f}, /* green */
        {0.55f, 0.35f, 1.0f}, /* purple */
        {1.0f, 0.85f, 0.15f}, /* yellow */
        {0.15f, 0.85f, 1.0f}, /* cyan */
        {1.0f, 0.35f, 0.75f}, /* pink */
    };
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        if (i == (int)net_local_id()) continue;
        int ci = i % 6;
        sgl_push_matrix();
        sgl_translate(players[i].x, players[i].y, 0.0f);
        sgl_rotate(players[i].angle, 0.0f, 0.0f, 1.0f);
        sgl_c4f(colors[ci][0], colors[ci][1], colors[ci][2], 0.85f);
        sgl_begin_triangles();
        sgl_v2f(18.0f, 0.0f);
        sgl_v2f(-12.0f, 10.0f);
        sgl_v2f(-12.0f, -10.0f);
        sgl_end();
        sgl_pop_matrix();
    }
}

static void render_world(void) {
    vec2 camera = g.ship.pos;
    float half_w = sapp_widthf() * 0.5f;
    float half_h = sapp_heightf() * 0.5f;

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(camera.x - half_w, camera.x + half_w, camera.y - half_h, camera.y + half_h, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    draw_background(camera);
    for (int i = 0; i < MAX_STATIONS; i++) {
        bool is_current = g.docked && (i == g.current_station);
        bool is_nearby = (!g.docked) && (i == g.nearby_station);
        draw_station(&g.stations[i], is_current, is_nearby);
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.asteroids[i].active) {
            continue;
        }
        draw_asteroid(&g.asteroids[i], i == g.hover_asteroid);
    }
    draw_beam();
    draw_ship_tractor_field();
    draw_ship();
    draw_npc_ships();
    draw_remote_players(); /* Multiplayer: remote player ships */
}

static void render_ui(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, screen_w, screen_h, 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    draw_hud_panels();
    draw_hud();
}

static void render_frame(void) {
    render_world();
    render_ui();

    sg_begin_pass(&(sg_pass){
        .action = g.pass_action,
        .swapchain = sglue_swapchain(),
    });
    sgl_draw();
    sdtx_draw();
    sg_end_pass();
    sg_commit();
}

static void advance_simulation_frame(float frame_dt) {
    g.runtime.accumulator += frame_dt;

    int sim_steps = 0;
    while ((g.runtime.accumulator >= SIM_DT) && (sim_steps < MAX_SIM_STEPS_PER_FRAME)) {
        sim_step(SIM_DT);
        g.runtime.accumulator -= SIM_DT;
        sim_steps++;
    }

    if (g.runtime.accumulator >= SIM_DT) {
        g.runtime.accumulator = 0.0f;
    }
}

static void frame(void) {
    float max_frame_dt = SIM_DT * (float)MAX_SIM_STEPS_PER_FRAME;
    float frame_dt = clampf((float)sapp_frame_duration(), 0.0f, max_frame_dt);
    advance_simulation_frame(frame_dt);
    audio_generate_stream(&g.audio);

    /* --- Multiplayer: poll and send state --- */
    if (g.multiplayer_enabled) {
        net_poll();
        /* Send local state at ~20 Hz (every 50ms). */
        g.net_send_timer += frame_dt;
        if (g.net_send_timer >= 0.05f) {
            g.net_send_timer -= 0.05f;
            net_send_state(g.ship.pos.x, g.ship.pos.y,
                           g.ship.vel.x, g.ship.vel.y,
                           g.ship.angle);
            /* Also send input flags + station action. */
            uint8_t flags = 0;
            uint8_t action = 0;
            if (g.thrusting) flags |= NET_INPUT_THRUST;
            if (g.input.key_down[SAPP_KEYCODE_A] || g.input.key_down[SAPP_KEYCODE_LEFT])
                flags |= NET_INPUT_LEFT;
            if (g.input.key_down[SAPP_KEYCODE_D] || g.input.key_down[SAPP_KEYCODE_RIGHT])
                flags |= NET_INPUT_RIGHT;
            if (g.input.key_down[SAPP_KEYCODE_SPACE])
                flags |= NET_INPUT_FIRE;
            /* Use queued one-shot action from sim_step (where key_pressed is valid). */
            action = g.pending_net_action;
            g.pending_net_action = 0;
            net_send_input(flags, g.ship.angle, action);
        }

    }

    render_frame();
}

static void cleanup(void) {
    if (g.multiplayer_enabled) {
        net_shutdown();
    }
    saudio_shutdown();
    sdtx_shutdown();
    sgl_shutdown();
    sg_shutdown();
}

static void event(const sapp_event* event) {
    switch (event->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
            if ((event->key_code >= 0) && (event->key_code < KEY_COUNT)) {
                g.input.key_down[event->key_code] = true;
                if (!event->key_repeat) {
                    g.input.key_pressed[event->key_code] = true;
                }
            }
            if (event->key_code == SAPP_KEYCODE_ESCAPE) {
                sapp_request_quit();
            }
            break;

        case SAPP_EVENTTYPE_KEY_UP:
            if ((event->key_code >= 0) && (event->key_code < KEY_COUNT)) {
                g.input.key_down[event->key_code] = false;
            }
            break;

        case SAPP_EVENTTYPE_UNFOCUSED:
        case SAPP_EVENTTYPE_SUSPENDED:
        case SAPP_EVENTTYPE_ICONIFIED:
            clear_input_state();
            break;

        default:
            break;
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 1600,
        .height = 900,
        .sample_count = 4,
        .high_dpi = true,
        .window_title = "Sokol Space Miner",
        .logger.func = slog_func,
    };
}
