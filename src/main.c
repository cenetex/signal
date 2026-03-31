#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef enum {
    STATION_TAB_OVERVIEW = 0,
    STATION_TAB_SERVICES,
    STATION_TAB_ROLE,       /* role-specific: Refinery / Yard / Bench / Outpost */
    STATION_TAB_CONTRACTS,
    STATION_TAB_CONSTRUCTION,
    STATION_TAB_COUNT
} station_tab_t;

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
    float dock_predict_timer;   /* ignore server docked state while > 0 */
    float net_input_timer;      /* throttle input sends to ~30 Hz */
    station_tab_t station_tab;  /* active tab when docked */
    bool was_docked;            /* track dock transitions for tab reset */
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

static game_t g;
#define LOCAL_PLAYER (g.world.players[g.local_player_slot])

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
static const float STATION_PANEL_HEIGHT = 400.0f;
static const float STATION_PANEL_COMPACT_WIDTH = 520.0f;
static const float STATION_PANEL_COMPACT_HEIGHT = 290.0f;
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
    if (g.world.rng == 0) {
        g.world.rng = 0xA341316Cu;
    }
    uint32_t x = g.world.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.world.rng = x;
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
    memset(LOCAL_PLAYER.ship.cargo, 0, sizeof(LOCAL_PLAYER.ship.cargo));
}

static void format_ore_manifest(char* text, size_t text_size) {
    int ferrite = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship,COMMODITY_FERRITE_ORE));
    int cuprite = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship,COMMODITY_CUPRITE_ORE));
    int crystal = (int)lroundf(ship_cargo_amount(&LOCAL_PLAYER.ship,COMMODITY_CRYSTAL_ORE));
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
    return &g.world.stations[station_index];
}

static const station_t* current_station_ptr(void) {
    return station_at(LOCAL_PLAYER.current_station);
}

static const station_t* nearby_station_ptr(void) {
    return station_at(LOCAL_PLAYER.nearby_station);
}

static int nearest_station_index(vec2 pos) {
    float best_distance_sq = 0.0f;
    int best_index = -1;

    for (int i = 0; i < MAX_STATIONS; i++) {
        float distance_sq = v2_dist_sq(pos, g.world.stations[i].pos);
        if ((best_index < 0) || (distance_sq < best_distance_sq)) {
            best_distance_sq = distance_sq;
            best_index = i;
        }
    }

    return best_index;
}

static const station_t* navigation_station_ptr(void) {
    if (LOCAL_PLAYER.docked) {
        return current_station_ptr();
    }
    if (LOCAL_PLAYER.nearby_station >= 0) {
        return nearby_station_ptr();
    }
    return station_at(nearest_station_index(LOCAL_PLAYER.ship.pos));
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
    return !LOCAL_PLAYER.docked || (g.notice_timer > 0.0f) || (g.collection_feedback_timer > 0.0f);
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
    int cargo_units = (int)lroundf(ship_raw_ore_total(&LOCAL_PLAYER.ship));
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
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

    if (LOCAL_PLAYER.docked) {
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

    if ((cargo_units >= cargo_capacity) && (LOCAL_PLAYER.nearby_fragments > 0)) {
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

    if (LOCAL_PLAYER.in_dock_range) {
        snprintf(label, label_size, "DOCK");
        snprintf(message, message_size, "Inside the dock ring. Press E to dock.");
        *r = 112;
        *g0 = 255;
        *b = 214;
        return true;
    }

    if (LOCAL_PLAYER.nearby_fragments > 0) {
        snprintf(label, label_size, "TRACTOR");
        if (LOCAL_PLAYER.tractor_fragments > 0) {
            snprintf(message, message_size, "Sweep through the debris cloud and let the tractor finish the pull.");
        } else {
            snprintf(message, message_size, "Close in on the fragments and let the tractor catch them.");
        }
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
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
        case STATION_ROLE_OUTPOST:
            return "OUTPOST // relay hub";
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
        case STATION_ROLE_OUTPOST:
            return "OUTPOST";
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
        case STATION_ROLE_OUTPOST:
            return "OUTPOST";
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

    ui->hull_now = (int)lroundf(LOCAL_PLAYER.ship.hull);
    ui->hull_max = (int)lroundf(ship_max_hull(&LOCAL_PLAYER.ship));
    float ore_total = ship_raw_ore_total(&LOCAL_PLAYER.ship);
    float repair = station_repair_cost(&LOCAL_PLAYER.ship, current_station_ptr());
    ui->cargo_units = (int)lroundf(ore_total);
    ui->cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    ui->payout = (int)lroundf(station_cargo_sale_value(&LOCAL_PLAYER.ship, current_station_ptr()));
    ui->repair_cost = (int)lroundf(repair);
    ui->mining_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_MINING);
    ui->hold_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_HOLD);
    ui->tractor_cost = ship_upgrade_cost(&LOCAL_PLAYER.ship,SHIP_UPGRADE_TRACTOR);
    ui->can_sell = station_has_service(STATION_SERVICE_ORE_BUYER) && (ore_total > 0.01f);
    ui->can_repair = station_has_service(STATION_SERVICE_REPAIR) && (repair > 0.0f) && (LOCAL_PLAYER.ship.credits + 0.01f >= repair);
    ui->can_upgrade_mining = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_MINING, STATION_SERVICE_UPGRADE_LASER, ui->mining_cost);
    ui->can_upgrade_hold = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD, STATION_SERVICE_UPGRADE_HOLD, ui->hold_cost);
    ui->can_upgrade_tractor = can_afford_upgrade(ui->station, &LOCAL_PLAYER.ship, SHIP_UPGRADE_TRACTOR, STATION_SERVICE_UPGRADE_TRACTOR, ui->tractor_cost);
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
        if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_HOLD)) {
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
    if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_MINING)) {
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
    if (ship_upgrade_maxed(&LOCAL_PLAYER.ship,SHIP_UPGRADE_TRACTOR)) {
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
        case STATION_ROLE_OUTPOST:
            *r = 0.72f;
            *g0 = 0.92f;
            *b = 0.52f;
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
    return v2_add(station->pos, v2(0.0f, -(station->radius + ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius + STATION_DOCK_APPROACH_OFFSET)));
}

static bool station_has_service(uint32_t service) {
    const station_t* station = current_station_ptr();
    return (station != NULL) && ((station->services & service) != 0);
}

/* station_cargo_sale_value, station_repair_cost: see economy.h/c */

static float ship_cargo_space(void) {
    return fmaxf(0.0f, ship_cargo_capacity(&LOCAL_PLAYER.ship) - ship_total_cargo(&LOCAL_PLAYER.ship));
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
    if (roll < 0.03f) {
        return ASTEROID_TIER_XXL;
    }
    if (roll < 0.26f) {
        return ASTEROID_TIER_XL;
    }
    if (roll < 0.70f) {
        return ASTEROID_TIER_L;
    }
    return ASTEROID_TIER_M;
}

static float client_max_signal_range(void) {
    float best = 0.0f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (g.world.stations[i].signal_range > best) best = g.world.stations[i].signal_range;
    }
    return best > 0.0f ? best : WORLD_RADIUS;
}

static void spawn_field_asteroid_of_tier(asteroid_t* asteroid, asteroid_tier_t tier) {
    float angle = rand_range(0.0f, TWO_PI_F);
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, random_raw_ore(), &g.world.rng);
    asteroid->fracture_child = false;
    /* Pick a random station and spawn within its signal range */
    int stn = rand_int(0, MAX_STATIONS - 1);
    float sr = g.world.stations[stn].signal_range;
    if (sr <= 0.0f) sr = client_max_signal_range();
    if (tier == ASTEROID_TIER_XXL) {
        vec2 center = g.world.stations[stn].pos;
        asteroid->pos = v2_add(center, v2(cosf(angle) * sr, sinf(angle) * sr));
        float inward_speed = rand_range(15.0f, 30.0f);
        asteroid->vel = v2(-cosf(angle) * inward_speed, -sinf(angle) * inward_speed);
    } else {
        vec2 center = g.world.stations[stn].pos;
        float distance = rand_range(420.0f, sr - 180.0f);
        asteroid->pos = v2_add(center, v2(cosf(angle) * distance, sinf(angle) * distance));
        asteroid->vel = v2(rand_range(-4.0f, 4.0f), rand_range(-4.0f, 4.0f));
    }
}

static void spawn_field_asteroid(asteroid_t* asteroid) {
    spawn_field_asteroid_of_tier(asteroid, random_field_asteroid_tier());
}

static void spawn_child_asteroid(asteroid_t* asteroid, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, commodity, &g.world.rng);
    asteroid->fracture_child = true;
    asteroid->pos = pos;
    asteroid->vel = vel;
}

static int desired_child_count(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL:
            return rand_int(8, 14);
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
        if (!g.world.asteroids[i].active) {
            if (*first_inactive_slot < 0) {
                *first_inactive_slot = i;
            }
            continue;
        }

        if (!g.world.asteroids[i].fracture_child) {
            (*seeded_count)++;
        }
    }
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
    world_reset(&g.world);
    player_init_ship(&LOCAL_PLAYER, &g.world);
    LOCAL_PLAYER.connected = true;

    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    g.asteroid_interp.interval = 0.1f;
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    g.npc_interp.interval = 0.1f;

    g.thrusting = false;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    audio_clear_voices(&g.audio);
    clear_collection_feedback();

    set_notice("%s online. Press E to launch.", g.world.stations[LOCAL_PLAYER.current_station].name);
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
    /* Skip empty station slots (no radius means not placed) */
    if (station->radius <= 0.0f && !station->scaffold) return;

    float role_r = 0.45f;
    float role_g = 0.85f;
    float role_b = 1.0f;
    float pulse = 0.35f + (sinf((g.world.time * 2.1f) + (float)station->role) * 0.15f);
    int spoke_count = 6;

    station_role_color(station->role, &role_r, &role_g, &role_b);
    if (station->role == STATION_ROLE_YARD) {
        spoke_count = 4;
    } else if (station->role == STATION_ROLE_BEAMWORKS) {
        spoke_count = 5;
    } else if (station->role == STATION_ROLE_OUTPOST) {
        spoke_count = 3;
    }

    /* Scaffold rendering: dashed outline, translucent */
    if (station->scaffold) {
        float alpha = 0.3f + 0.2f * sinf(g.world.time * 1.5f);
        float prog = station->scaffold_progress;
        /* Dashed dock circle */
        int dash_segs = 24;
        float step = TWO_PI_F / (float)dash_segs;
        for (int i = 0; i < dash_segs; i += 2) {
            float a0 = (float)i * step;
            float a1 = (float)(i + 1) * step;
            vec2 p0 = v2_add(station->pos, v2(cosf(a0) * station->dock_radius, sinf(a0) * station->dock_radius));
            vec2 p1 = v2_add(station->pos, v2(cosf(a1) * station->dock_radius, sinf(a1) * station->dock_radius));
            draw_segment(p0, p1, role_r * 0.5f, role_g * 0.5f, role_b * 0.5f, alpha);
        }
        /* Station body outline */
        draw_circle_outline(station->pos, station->radius, 18, role_r * 0.6f, role_g * 0.6f, role_b * 0.6f, alpha + 0.15f);
        /* Progress ring: filled portion */
        if (prog > 0.01f) {
            int filled_segs = (int)(prog * 24.0f);
            float fill_step = TWO_PI_F / 24.0f;
            for (int i = 0; i < filled_segs && i < 24; i++) {
                float a0 = (float)i * fill_step;
                float a1 = (float)(i + 1) * fill_step;
                vec2 p0 = v2_add(station->pos, v2(cosf(a0) * (station->radius + 12.0f), sinf(a0) * (station->radius + 12.0f)));
                vec2 p1 = v2_add(station->pos, v2(cosf(a1) * (station->radius + 12.0f), sinf(a1) * (station->radius + 12.0f)));
                draw_segment(p0, p1, role_r, role_g, role_b, 0.8f);
            }
        }
        return;
    }

    float dock_alpha = is_current ? 0.62f : (is_nearby ? 0.50f : 0.16f + pulse);
    draw_circle_outline(station->pos, station->dock_radius, 48, role_r * 0.72f, role_g, role_b, dock_alpha);
    draw_circle_filled(station->pos, station->radius, 28, 0.08f, 0.12f, 0.17f, 1.0f);
    draw_circle_outline(station->pos, station->radius + 8.0f, 28, role_r, role_g, role_b, 0.92f);
    draw_circle_filled(station->pos, 18.0f, 18, role_r * 0.68f, role_g * 0.88f, role_b * 0.96f, 1.0f);

    for (int i = 0; i < spoke_count; i++) {
        float angle = (TWO_PI_F / (float)spoke_count) * (float)i + g.world.time * (0.14f + ((float)station->role * 0.02f));
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
    } else if (station->role == STATION_ROLE_OUTPOST) {
        /* Triangle marker for activated outpost */
        float s = 14.0f;
        draw_segment(v2_add(station->pos, v2(0.0f, -s)), v2_add(station->pos, v2(s, s * 0.6f)), role_r, role_g, role_b, 0.75f);
        draw_segment(v2_add(station->pos, v2(s, s * 0.6f)), v2_add(station->pos, v2(-s, s * 0.6f)), role_r, role_g, role_b, 0.75f);
        draw_segment(v2_add(station->pos, v2(-s, s * 0.6f)), v2_add(station->pos, v2(0.0f, -s)), role_r, role_g, role_b, 0.75f);
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
        case ASTEROID_TIER_XXL:
            segments = 28;
            break;
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
    if (LOCAL_PLAYER.nearby_fragments <= 0) {
        return;
    }

    float pulse = 0.28f + (sinf(g.world.time * 7.0f) * 0.08f);
    draw_circle_outline(LOCAL_PLAYER.ship.pos, ship_tractor_range(&LOCAL_PLAYER.ship), 40, 0.24f, 0.86f, 1.0f, pulse);
    if (LOCAL_PLAYER.tractor_fragments > 0) {
        draw_circle_outline(LOCAL_PLAYER.ship.pos, ship_collect_radius(&LOCAL_PLAYER.ship) + 6.0f, 28, 0.50f, 1.0f, 0.82f, 0.75f);
    }
}

static void draw_ship(void) {
    sgl_push_matrix();
    sgl_translate(LOCAL_PLAYER.ship.pos.x, LOCAL_PLAYER.ship.pos.y, 0.0f);
    sgl_rotate(LOCAL_PLAYER.ship.angle, 0.0f, 0.0f, 1.0f);

    if (g.thrusting) {
        float flicker = 10.0f + sinf(g.world.time * 42.0f) * 3.0f;
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
    /* Use accumulated ore tint — starts white, absorbs cargo colors over time */
    float hull_r = npc->tint_r;
    float hull_g = npc->tint_g;
    float hull_b = npc->tint_b;

    sgl_push_matrix();
    sgl_translate(npc->pos.x, npc->pos.y, 0.0f);
    sgl_rotate(npc->angle, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale, scale, 1.0f);

    if (npc->thrusting) {
        float flicker = 8.0f + sinf(g.world.time * 38.0f + npc->pos.x) * 2.5f;
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
    const asteroid_t* asteroid = &g.world.asteroids[npc->target_asteroid];
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
        if (!g.world.npc_ships[i].active) continue;
        draw_npc_ship(&g.world.npc_ships[i]);
        draw_npc_mining_beam(&g.world.npc_ships[i]);
    }
}

static void draw_beam(void) {
    if (!LOCAL_PLAYER.beam_active) {
        return;
    }

    if (LOCAL_PLAYER.beam_hit) {
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.45f, 1.0f, 0.92f, 0.95f);
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.12f, 0.78f, 1.0f, 0.35f);
    } else {
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.9f, 0.75f, 0.30f, 0.55f);
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

    if (LOCAL_PLAYER.docked) {
        float panel_x = 0.0f;
        float panel_y = 0.0f;
        float panel_w = 0.0f;
        float panel_h = 0.0f;
        bool compact = ui_is_compact();
        station_ui_state_t ui = { 0 };
        build_station_ui_state(&ui);
        get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
        draw_ui_scrim(0.34f);
        draw_ui_panel(panel_x, panel_y, panel_w, panel_h, 0.08f);

        float inner_x = panel_x + 18.0f;
        float inner_y = panel_y + 18.0f;
        float inner_w = panel_w - 36.0f;

        /* Header rule below station name */
        draw_ui_rule(inner_x, panel_x + panel_w - 18.0f, inner_y + 26.0f, 0.14f, 0.26f, 0.38f, 0.70f);

        /* Tab bar — scaffold: OVERVIEW + BUILD; normal: OVERVIEW + SERVICES + role + CONTRACTS */
        float tab_y = inner_y + 32.0f;
        float tab_h = compact ? 16.0f : 20.0f;
        station_tab_t visible_tabs[STATION_TAB_COUNT];
        int tab_count = 0;
        if (ui.station->scaffold) {
            visible_tabs[tab_count++] = STATION_TAB_OVERVIEW;
            visible_tabs[tab_count++] = STATION_TAB_CONSTRUCTION;
        } else {
            visible_tabs[tab_count++] = STATION_TAB_OVERVIEW;
            visible_tabs[tab_count++] = STATION_TAB_SERVICES;
            visible_tabs[tab_count++] = STATION_TAB_ROLE;
            visible_tabs[tab_count++] = STATION_TAB_CONTRACTS;
        }
        float tab_w = fminf(inner_w / (float)tab_count, 120.0f);

        for (int t = 0; t < tab_count; t++) {
            float tx = inner_x + (float)t * tab_w;
            bool active = (g.station_tab == visible_tabs[t]);
            float accent_a = active ? 0.92f : 0.20f;
            if (active) {
                draw_rect_centered(v2(tx + tab_w * 0.5f, tab_y + tab_h * 0.5f),
                    tab_w * 0.5f, tab_h * 0.5f, 0.06f, 0.12f, 0.18f, 0.95f);
            }
            draw_ui_rule(tx + 4.0f, tx + tab_w - 4.0f, tab_y + tab_h - 2.0f,
                0.30f, 0.85f, 1.0f, accent_a);
        }

        /* Tab content area */
        float content_y = tab_y + tab_h + 8.0f;
        float strip_h = compact ? 32.0f : 38.0f;
        float content_h = panel_y + panel_h - 18.0f - content_y - strip_h;
        draw_ui_panel(inner_x, content_y, inner_w, content_h, 0.03f);

        /* Service cards on the Services tab */
        if (g.station_tab == STATION_TAB_SERVICES && !ui.station->scaffold) {
            float card_gap = compact ? 4.0f : 6.0f;
            float card_h = compact ? 18.0f : 24.0f;
            float first_card_y = content_y + (compact ? 30.0f : 36.0f);
            float card_w = inner_w - 24.0f;

            if (ui.station->role == STATION_ROLE_REFINERY) {
                draw_service_card(inner_x + 12.0f, first_card_y, card_w, card_h, 0.24f, 0.90f, 0.70f, ui.can_sell);
                draw_service_card(inner_x + 12.0f, first_card_y + card_h + card_gap, card_w, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
            } else if (ui.station->role == STATION_ROLE_YARD) {
                draw_service_card(inner_x + 12.0f, first_card_y, card_w, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
                draw_service_card(inner_x + 12.0f, first_card_y + card_h + card_gap, card_w, card_h, 0.50f, 0.82f, 1.0f, ui.can_upgrade_hold);
            } else if (ui.station->role == STATION_ROLE_OUTPOST) {
                draw_service_card(inner_x + 12.0f, first_card_y, card_w, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
            } else {
                draw_service_card(inner_x + 12.0f, first_card_y, card_w, card_h, 0.98f, 0.72f, 0.26f, ui.can_repair);
                draw_service_card(inner_x + 12.0f, first_card_y + card_h + card_gap, card_w, card_h, 0.34f, 0.88f, 1.0f, ui.can_upgrade_mining);
                draw_service_card(inner_x + 12.0f, first_card_y + 2.0f * (card_h + card_gap), card_w, card_h, 0.42f, 1.0f, 0.86f, ui.can_upgrade_tractor);
            }
        }

        /* Ship status strip — always visible below the content area */
        {
            float strip_y = panel_y + panel_h - (compact ? 32.0f : 38.0f);
            float meter_x = inner_x + 4.0f;
            float meter_w = compact ? 80.0f : 100.0f;
            float pip_x = meter_x + meter_w * 2.0f + 36.0f;
            /* Hull + cargo meters side by side */
            draw_ui_meter(meter_x, strip_y + 4.0f, meter_w, 10.0f,
                LOCAL_PLAYER.ship.hull / ship_max_hull(&LOCAL_PLAYER.ship), 0.96f, 0.54f, 0.28f);
            draw_ui_meter(meter_x + meter_w + 8.0f, strip_y + 4.0f, meter_w, 10.0f,
                ship_total_cargo(&LOCAL_PLAYER.ship) / fmaxf(1.0f, ship_cargo_capacity(&LOCAL_PLAYER.ship)), 0.26f, 0.90f, 0.72f);
            /* Upgrade pips inline */
            if (!compact) {
                draw_upgrade_pips(pip_x, strip_y + 2.0f, LOCAL_PLAYER.ship.mining_level, 0.34f, 0.88f, 1.0f);
                draw_upgrade_pips(pip_x, strip_y + 14.0f, LOCAL_PLAYER.ship.tractor_level, 0.42f, 1.0f, 0.86f);
                draw_upgrade_pips(pip_x, strip_y + 26.0f, LOCAL_PLAYER.ship.hold_level, 0.50f, 0.82f, 1.0f);
            }
        }
    }

    if (hud_should_draw_message_panel()) {
        get_hud_message_panel_rect(&message_x, &message_y, &message_w, &message_h);
        draw_ui_panel(message_x, message_y, message_w, message_h, 0.05f);
    }
}

static void draw_station_services(const station_ui_state_t* ui) {
    if (!LOCAL_PLAYER.docked) return;

    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    bool compact = ui_is_compact();
    float tab_h = compact ? 16.0f : 20.0f;
    float inner_x = panel_x + 18.0f;
    float inner_y = panel_y + 18.0f;
    float inner_w = panel_w - 36.0f;
    float content_y = inner_y + 32.0f + tab_h + 8.0f;
    float cx = inner_x + 18.0f; /* content text x */
    float cy = content_y + 16.0f; /* content text start y */
    station_tab_t visible_tabs[STATION_TAB_COUNT];
    int tab_count = 0;
    if (ui->station->scaffold) {
        visible_tabs[tab_count++] = STATION_TAB_OVERVIEW;
        visible_tabs[tab_count++] = STATION_TAB_CONSTRUCTION;
    } else {
        visible_tabs[tab_count++] = STATION_TAB_OVERVIEW;
        visible_tabs[tab_count++] = STATION_TAB_SERVICES;
        visible_tabs[tab_count++] = STATION_TAB_ROLE;
        visible_tabs[tab_count++] = STATION_TAB_CONTRACTS;
    }
    float tab_w = fminf(inner_w / (float)tab_count, 120.0f);

    /* Station name + role header */
    sdtx_color3b(232, 241, 255);
    sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 16.0f));
    sdtx_puts(ui->station->name);
    if (ui->station->scaffold) {
        sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(255, 221, 119);
        sdtx_puts("UNDER CONSTRUCTION");
    } else {
        sdtx_pos(ui_text_pos(panel_x + 20.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(118, 255, 221);
        sdtx_puts(station_role_hub_label(ui->station->role));
    }

    /* Credits badge (right side of header) */
    if (panel_w >= 480.0f) {
        char header_badge[32] = { 0 };
        format_station_header_badge(ui, header_badge, sizeof(header_badge));
        sdtx_pos(ui_text_pos(panel_x + panel_w - 152.0f), ui_text_pos(panel_y + 16.0f));
        sdtx_color3b(203, 220, 248);
        sdtx_puts(header_badge);
        sdtx_pos(ui_text_pos(panel_x + panel_w - 152.0f), ui_text_pos(panel_y + 32.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("Tab: switch  E: launch");
    }

    /* Tab labels */
    {
        float tab_bar_y = inner_y + 32.0f;
        /* Role-specific label for the ROLE tab */
        const char* role_label = "STATION";
        if (ui->station->role == STATION_ROLE_REFINERY) role_label = "REFINERY";
        else if (ui->station->role == STATION_ROLE_YARD) role_label = "YARD";
        else if (ui->station->role == STATION_ROLE_BEAMWORKS) role_label = "BENCH";
        else if (ui->station->role == STATION_ROLE_OUTPOST) role_label = "OUTPOST";

        for (int t = 0; t < tab_count; t++) {
            float tx = inner_x + (float)t * tab_w;
            station_tab_t tid = visible_tabs[t];
            bool active = (g.station_tab == tid);
            sdtx_pos(ui_text_pos(tx + 8.0f), ui_text_pos(tab_bar_y + (compact ? 4.0f : 6.0f)));
            sdtx_color3b(active ? 130 : 100, active ? 255 : 120, active ? 235 : 145);
            switch (tid) {
                case STATION_TAB_OVERVIEW:     sdtx_puts("OVERVIEW"); break;
                case STATION_TAB_SERVICES:     sdtx_puts("SERVICES"); break;
                case STATION_TAB_ROLE:         sdtx_puts(role_label); break;
                case STATION_TAB_CONTRACTS:    sdtx_puts("CONTRACTS"); break;
                case STATION_TAB_CONSTRUCTION: sdtx_puts("BUILD"); break;
                default: break;
            }
        }
    }

    /* ---- Tab content ---- */
    switch (g.station_tab) {

    case STATION_TAB_OVERVIEW: {
        if (ui->station->scaffold) {
            int pct = (int)lroundf(ui->station->scaffold_progress * 100.0f);
            int frames_held = (int)lroundf(LOCAL_PLAYER.ship.cargo[COMMODITY_FRAME_INGOT]);
            float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - ui->station->scaffold_progress);
            int needed_int = (int)lroundf(needed);
            sdtx_color3b(203, 220, 248);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            sdtx_printf("Progress: %d%%", pct);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 16.0f));
            sdtx_printf("Need %d more frame ingots", needed_int);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 32.0f));
            sdtx_printf("You have: %d frame ingots", frames_held);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 56.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("Dock to auto-deliver frames.");
        } else {
            /* Station welcome — what this place does */
            sdtx_color3b(203, 220, 248);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
            if (ui->station->role == STATION_ROLE_REFINERY) {
                sdtx_puts("Ore intake and refining hub.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Sell raw ore for credits. Smelts ingots");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 34.0f));
                sdtx_puts("for station production.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 58.0f));
                sdtx_color3b(203, 220, 248);
                sdtx_printf("Haul value: %d cr", ui->payout);
            } else if (ui->station->role == STATION_ROLE_YARD) {
                sdtx_puts("Frame manufacturing yard.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Upgrades cargo hold capacity.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 34.0f));
                sdtx_puts("Produces frame components from ingots.");
            } else if (ui->station->role == STATION_ROLE_OUTPOST) {
                sdtx_puts("Signal relay outpost.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Extends signal range into new territory.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 44.0f));
                sdtx_color3b(203, 220, 248);
                sdtx_printf("Signal range: %.0f", ui->station->signal_range);
            } else {
                sdtx_puts("Field equipment works.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
                sdtx_color3b(145, 160, 188);
                sdtx_puts("Upgrades mining laser and tractor beam.");
                sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 34.0f));
                sdtx_puts("Fabricates modules from ingots.");
            }
            /* Common: repair status */
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 80.0f));
            sdtx_color3b(ui->repair_cost > 0 ? 255 : 145, ui->repair_cost > 0 ? 180 : 160, ui->repair_cost > 0 ? 80 : 188);
            sdtx_printf("Hull repair: %s", ui->repair_cost > 0 ? "needed" : "nominal");
        }
        break;
    }

    case STATION_TAB_SERVICES: {
        if (ui->station->scaffold) break;
        station_service_line_t service_lines[3] = { 0 };
        int service_line_count = build_station_service_lines(ui, service_lines);
        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("SERVICES");
        float card_gap = compact ? 4.0f : 6.0f;
        float line_h = compact ? 18.0f : 24.0f;
        float first_line_y = cy + 20.0f;
        for (int i = 0; i < service_line_count; i++) {
            float line_y = first_line_y + (float)i * (line_h + card_gap);
            draw_station_service_text_line(cx + 6.0f, line_y + (compact ? 5.0f : 8.0f), &service_lines[i], compact);
        }
        break;
    }

    case STATION_TAB_ROLE: {
        if (ui->station->scaffold) break;
        char market_summary[64] = { 0 };
        char market_detail[64] = { 0 };
        format_station_market_summary(ui, compact, market_summary, sizeof(market_summary));
        format_station_market_detail(ui, compact, market_detail, sizeof(market_detail));

        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts(station_role_market_title(ui->station->role));
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 16.0f));
        sdtx_color3b(203, 220, 248);
        sdtx_puts(market_summary);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 32.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts(market_detail);

        /* Role-specific production details */
        float dy = cy + 56.0f;
        sdtx_color3b(203, 220, 248);
        if (ui->station->role == STATION_ROLE_REFINERY) {
            char board_line[64] = { 0 };
            char hopper_line[64] = { 0 };
            char stock_line[64] = { 0 };
            format_refinery_price_line(ui->station, board_line, sizeof(board_line));
            format_ore_hopper_line(ui->station, hopper_line, sizeof(hopper_line));
            format_ingot_stock_line(ui->station, stock_line, sizeof(stock_line));
            sdtx_pos(ui_text_pos(cx), ui_text_pos(dy));
            sdtx_color3b(145, 160, 188);
            sdtx_puts(board_line);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(dy + 16.0f));
            sdtx_color3b(203, 220, 248);
            sdtx_printf("Hoppers  %s", hopper_line);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(dy + 32.0f));
            sdtx_printf("Stock    %s", stock_line);
        } else if (ui->station->role == STATION_ROLE_YARD) {
            int frames = (int)lroundf(ui->station->product_stock[PRODUCT_FRAME]);
            int need = (int)lroundf(upgrade_product_cost(&LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD));
            sdtx_pos(ui_text_pos(cx), ui_text_pos(dy));
            sdtx_color3b(145, 160, 188);
            sdtx_printf("Frames %d  Need %d", frames, ship_upgrade_maxed(&LOCAL_PLAYER.ship, SHIP_UPGRADE_HOLD) ? 0 : need);
        } else if (ui->station->role == STATION_ROLE_BEAMWORKS) {
            int lasers = (int)lroundf(ui->station->product_stock[PRODUCT_LASER_MODULE]);
            int tractors = (int)lroundf(ui->station->product_stock[PRODUCT_TRACTOR_MODULE]);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(dy));
            sdtx_color3b(145, 160, 188);
            sdtx_printf("LSR %d  TRC %d", lasers, tractors);
        } else if (ui->station->role == STATION_ROLE_OUTPOST) {
            sdtx_pos(ui_text_pos(cx), ui_text_pos(dy));
            sdtx_color3b(145, 160, 188);
            sdtx_printf("Signal range: %.0f", ui->station->signal_range);
        }
        break;
    }

    case STATION_TAB_CONTRACTS: {
        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("CONTRACTS");
        int shown = 0;
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->station_index < 0 || ct->station_index >= MAX_STATIONS) continue;
            if (g.world.stations[ct->station_index].name[0] == '\0') continue;
            float cprice = ct->base_price * (1.0f + ct->age / 300.0f * 0.2f);
            if (cprice < 0.01f) continue;
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f + (float)shown * 16.0f));
            sdtx_color3b(203, 220, 248);
            sdtx_printf("%s @ %s: %.0f cr/u",
                commodity_short_name(ct->commodity),
                g.world.stations[ct->station_index].name,
                cprice);
            if (++shown >= 8) break;
        }
        if (shown == 0) {
            sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
            sdtx_color3b(145, 160, 188);
            sdtx_puts("No active contracts.");
        }
        break;
    }

    case STATION_TAB_CONSTRUCTION: {
        if (!ui->station->scaffold) break;
        int pct = (int)lroundf(ui->station->scaffold_progress * 100.0f);
        int frames_held = (int)lroundf(LOCAL_PLAYER.ship.cargo[COMMODITY_FRAME_INGOT]);
        float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - ui->station->scaffold_progress);
        int needed_int = (int)lroundf(needed);

        sdtx_color3b(130, 255, 235);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy));
        sdtx_puts("CONSTRUCTION");

        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 20.0f));
        sdtx_color3b(255, 221, 119);
        sdtx_printf("Progress: %d%%", pct);

        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 40.0f));
        sdtx_color3b(203, 220, 248);
        sdtx_printf("Materials needed: %d frame ingots", needed_int);
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 56.0f));
        sdtx_printf("You carry: %d frame ingots", frames_held);

        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 80.0f));
        sdtx_color3b(145, 160, 188);
        sdtx_puts("Frame ingots auto-deliver on dock.");
        sdtx_pos(ui_text_pos(cx), ui_text_pos(cy + 96.0f));
        sdtx_printf("Signal range on activation: %.0f", OUTPOST_SIGNAL_RANGE);
        break;
    }

    default:
        break;
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
    int hull_units = (int)lroundf(LOCAL_PLAYER.ship.hull);
    int hull_capacity = (int)lroundf(ship_max_hull(&LOCAL_PLAYER.ship));
    int cargo_units = (int)lroundf(ship_raw_ore_total(&LOCAL_PLAYER.ship));
    int credits = (int)lroundf(LOCAL_PLAYER.ship.credits);
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    int payout_preview = (int)lroundf(station_cargo_sale_value(&LOCAL_PLAYER.ship, current_station_ptr()));
    const station_t* current_station = current_station_ptr();
    const station_t* navigation_station = navigation_station_ptr();
    station_ui_state_t ui = { 0 };
    if (LOCAL_PLAYER.docked) {
        build_station_ui_state(&ui);
    }
    int station_distance = 0;

    vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
    vec2 home = v2(0.0f, -1.0f);
    if (navigation_station != NULL) {
        station_distance = (int)lroundf(v2_len(v2_sub(navigation_station->pos, LOCAL_PLAYER.ship.pos)));
        home = v2_norm(v2_sub(navigation_station->pos, LOCAL_PLAYER.ship.pos));
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

    int sig_pct = (int)lroundf(signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos) * 100.0f);

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
        sdtx_printf("%s // CR %d", LOCAL_PLAYER.docked ? "RUN" : "SHIP", credits);

        sdtx_pos(top_text_x, top_row_1);
        if (sig_pct < 30) sdtx_color3b(255, 100, 100);
        else if (sig_pct < 60) sdtx_color3b(255, 221, 119);
        else sdtx_color3b(203, 220, 248);
        sdtx_printf("H %d/%d  C %d/%d  SIG %d%%", hull_units, hull_capacity, cargo_units, cargo_capacity, sig_pct);

        sdtx_pos(top_text_x, top_row_2);
        if (LOCAL_PLAYER.docked) {
            sdtx_color3b(112, 255, 214);
            sdtx_printf("%s // E launch", dock_role);
        } else if (LOCAL_PLAYER.in_dock_range) {
            sdtx_color3b(112, 255, 214);
            sdtx_puts("DOCK RING // E dock");
        } else {
            sdtx_color3b(199, 222, 255);
            sdtx_printf("%s %d u // %d %s", nav_role, station_distance, bearing_degrees, bearing_mark);
        }

        sdtx_pos(top_text_x, top_row_3);
        if (LOCAL_PLAYER.docked) {
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
        } else if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
            const asteroid_t* asteroid = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
            int integrity_left = (int)lroundf(asteroid->hp);
            sdtx_color3b(130, 255, 235);
            sdtx_printf("TGT %s // %s // %d HP", asteroid_tier_name(asteroid->tier), commodity_code(asteroid->commodity), integrity_left);
        } else if (LOCAL_PLAYER.nearby_fragments > 0) {
            sdtx_color3b(130, 255, 235);
            if (LOCAL_PLAYER.tractor_fragments > 0) {
                sdtx_printf("TRACTOR // %d FRAG", LOCAL_PLAYER.tractor_fragments);
            } else {
                sdtx_printf("FRAGMENTS // %d", LOCAL_PLAYER.nearby_fragments);
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
        if (LOCAL_PLAYER.docked) {
            sdtx_puts("Tab switch  E launch  Q back");
        } else {
            sdtx_puts("W/S thrust  A/D turn  SPC mine  E dock  6 outpost");
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
    sdtx_puts(LOCAL_PLAYER.docked ? "RUN STATUS" : "SHIP STATUS");

    sdtx_pos(top_text_x, top_row_1);
    if (sig_pct < 30) sdtx_color3b(255, 100, 100);
    else if (sig_pct < 60) sdtx_color3b(255, 221, 119);
    else sdtx_color3b(203, 220, 248);
    sdtx_printf("CR %d  H %d/%d  C %d/%d  SIG %d%%", credits, hull_units, hull_capacity, cargo_units, cargo_capacity, sig_pct);

    sdtx_pos(top_text_x, top_row_2);
    if (LOCAL_PLAYER.docked) {
        sdtx_color3b(112, 255, 214);
        sdtx_printf("%s // docked // E launch", current_station->name);
    } else if (LOCAL_PLAYER.in_dock_range) {
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
    if (LOCAL_PLAYER.docked) {
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
    } else if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        const asteroid_t* asteroid = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
        int integrity_left = (int)lroundf(asteroid->hp);
        sdtx_color3b(130, 255, 235);
        sdtx_printf("Target %s %s // %s // %d hp", asteroid_tier_name(asteroid->tier), asteroid_tier_kind(asteroid->tier), commodity_short_name(asteroid->commodity), integrity_left);
    } else if (LOCAL_PLAYER.nearby_fragments > 0) {
        sdtx_color3b(130, 255, 235);
        if (LOCAL_PLAYER.tractor_fragments > 0) {
            sdtx_printf("Tractor lock // %d frag%s", LOCAL_PLAYER.tractor_fragments, LOCAL_PLAYER.tractor_fragments == 1 ? "" : "s");
        } else {
            sdtx_printf("Nearby fragments // %d", LOCAL_PLAYER.nearby_fragments);
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
    if (LOCAL_PLAYER.docked) {
        sdtx_puts("Tab switch  Q back  E launch  R reset  ESC quit");
    } else {
        sdtx_puts("W/S thrust  A/D turn  SPACE mine  E dock  6 outpost  R reset  ESC quit");
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

    /* --- Multiplayer HUD indicator + version --- */
    {
        float info_x = ui_text_pos(screen_w - (compact ? 100.0f : 120.0f));
        float info_y = ui_text_pos(8.0f);
        sdtx_pos(info_x, info_y);
#ifdef GIT_HASH
        const char* client_hash = GIT_HASH;
#else
        const char* client_hash = "dev";
#endif
        if (g.multiplayer_enabled && net_is_connected()) {
            const char* srv = net_server_hash();
            bool match = srv[0] != '\0' && strcmp(client_hash, srv) == 0;
            sdtx_color3b(match ? 80 : 255, match ? 255 : 200, match ? 180 : 60);
            sdtx_printf("%s/%s MP:%d", client_hash, srv[0] ? srv : "?", net_remote_player_count() + 1);
        } else {
            sdtx_color3b(80, 90, 100);
            sdtx_puts(client_hash);
        }
    }

    draw_station_services(&ui);
}

static bool is_key_down(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_down[key];
}

static bool is_key_pressed(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_pressed[key];
}

static vec2 ship_forward(void) {
    return v2_from_angle(LOCAL_PLAYER.ship.angle);
}

static vec2 ship_muzzle(vec2 forward) {
    return v2_add(LOCAL_PLAYER.ship.pos, v2_scale(forward, ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius + 8.0f));
}

static void reset_step_feedback(void) {
    LOCAL_PLAYER.hover_asteroid = -1;
    LOCAL_PLAYER.beam_active = false;
    LOCAL_PLAYER.beam_hit = false;
    g.thrusting = false;
    LOCAL_PLAYER.nearby_fragments = 0;
    LOCAL_PLAYER.tractor_fragments = 0;
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
    intent.place_outpost = is_key_pressed(SAPP_KEYCODE_6);
    intent.reset = is_key_pressed(SAPP_KEYCODE_R);
    return intent;
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

/* step_refinery_production, step_station_production: see economy.h/c */

/* No sync_globals_to_world — world_t is the source of truth in single player. */

/* sync_world_to_globals removed — everything reads from g.world directly */

static void sim_step(float dt) {
    reset_step_feedback();
    audio_step(&g.audio, dt);

    input_intent_t intent = sample_input_intent();
    if (intent.reset) {
        reset_world();
        consume_pressed_input();
        return;
    }

    /* Tab switching while docked */
    if (LOCAL_PLAYER.docked && !g.was_docked) {
        /* Just docked — reset to overview (or construction for scaffolds) */
        const station_t* st = &g.world.stations[LOCAL_PLAYER.current_station];
        g.station_tab = st->scaffold ? STATION_TAB_CONSTRUCTION : STATION_TAB_OVERVIEW;
    }
    g.was_docked = LOCAL_PLAYER.docked;
    if (LOCAL_PLAYER.docked && (is_key_pressed(SAPP_KEYCODE_TAB) || is_key_pressed(SAPP_KEYCODE_Q))) {
        const station_t* st = &g.world.stations[LOCAL_PLAYER.current_station];
        station_tab_t vtabs[STATION_TAB_COUNT];
        int vtab_count = 0;
        if (st->scaffold) {
            vtabs[vtab_count++] = STATION_TAB_OVERVIEW;
            vtabs[vtab_count++] = STATION_TAB_CONSTRUCTION;
        } else {
            vtabs[vtab_count++] = STATION_TAB_OVERVIEW;
            vtabs[vtab_count++] = STATION_TAB_SERVICES;
            vtabs[vtab_count++] = STATION_TAB_ROLE;
            vtabs[vtab_count++] = STATION_TAB_CONTRACTS;
        }
        int cur = 0;
        for (int i = 0; i < vtab_count; i++) { if (vtabs[i] == g.station_tab) { cur = i; break; } }
        int dir = is_key_pressed(SAPP_KEYCODE_TAB) ? 1 : (vtab_count - 1);
        g.station_tab = vtabs[(cur + dir) % vtab_count];
    }

    LOCAL_PLAYER.input = intent;
    if (g.multiplayer_enabled && net_is_connected()) {
        /* Multiplayer: only predict local player movement.
         * Asteroids, NPCs, production are server-authoritative
         * and interpolated from server snapshots. */
        world_sim_step_player_only(&g.world, g.local_player_slot, dt);

        /* Mining beam visual only — no HP deduction */
        if (!LOCAL_PLAYER.docked && intent.mine) {
            vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
            vec2 muzzle = v2_add(LOCAL_PLAYER.ship.pos,
                v2_scale(forward, ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius + 8.0f));
            LOCAL_PLAYER.beam_active = true;
            LOCAL_PLAYER.beam_start = muzzle;
            if (LOCAL_PLAYER.hover_asteroid >= 0) {
                asteroid_t *a = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
                vec2 to_a = v2_sub(a->pos, muzzle);
                LOCAL_PLAYER.beam_end = v2_sub(a->pos, v2_scale(v2_norm(to_a), a->radius * 0.85f));
                LOCAL_PLAYER.beam_hit = true;
            } else {
                LOCAL_PLAYER.beam_end = v2_add(muzzle, v2_scale(forward, 170.0f));
            }
        }

        /* Advance interpolation timers */
        g.asteroid_interp.t += dt / fmaxf(g.asteroid_interp.interval, 0.01f);
        g.npc_interp.t += dt / fmaxf(g.npc_interp.interval, 0.01f);
    } else {
        /* Single player: full authoritative sim */
        world_sim_step(&g.world, dt);
    }

    g.thrusting = (intent.thrust > 0.0f) && !LOCAL_PLAYER.docked;

    /* Play audio from sim events */
    for (int i = 0; i < g.world.events.count; i++) {
        sim_event_t* ev = &g.world.events.events[i];
        switch (ev->type) {
            case SIM_EVENT_FRACTURE:
                audio_play_fracture(&g.audio, ev->fracture.tier);
                break;
            case SIM_EVENT_MINING_TICK:
                if (ev->player_id == g.local_player_slot) audio_play_mining_tick(&g.audio);
                break;
            case SIM_EVENT_DOCK:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_dock(&g.audio);
                    set_notice("Docked at %s.", g.world.stations[LOCAL_PLAYER.current_station].name);
                }
                break;
            case SIM_EVENT_LAUNCH:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_launch(&g.audio);
                    set_notice("Launch corridor clear.");
                }
                break;
            case SIM_EVENT_SELL:
                if (ev->player_id == g.local_player_slot) audio_play_sale(&g.audio);
                break;
            case SIM_EVENT_REPAIR:
                if (ev->player_id == g.local_player_slot) audio_play_repair(&g.audio);
                break;
            case SIM_EVENT_UPGRADE:
                if (ev->player_id == g.local_player_slot) audio_play_upgrade(&g.audio, ev->upgrade.upgrade);
                break;
            case SIM_EVENT_DAMAGE:
                if (ev->player_id == g.local_player_slot) audio_play_damage(&g.audio, ev->damage.amount);
                break;
            default:
                break;
        }
    }

    step_notice_timer(dt);
    if (g.dock_predict_timer > 0.0f)
        g.dock_predict_timer = fmaxf(0.0f, g.dock_predict_timer - dt);

    /* In multiplayer, also queue one-shot actions for network send. */
    if (g.multiplayer_enabled && net_is_connected()) {
        if (intent.interact) {
            g.pending_net_action = LOCAL_PLAYER.docked ? 2 : 1;
            g.dock_predict_timer = 0.5f;
        } else if (intent.service_sell)
            g.pending_net_action = 3;
        else if (intent.service_repair)
            g.pending_net_action = 4;
        else if (intent.upgrade_mining)
            g.pending_net_action = 5;
        else if (intent.upgrade_hold)
            g.pending_net_action = 6;
        else if (intent.upgrade_tractor)
            g.pending_net_action = 7;
        else if (intent.place_outpost)
            g.pending_net_action = 8;
    }

    consume_pressed_input();
}

/* Forward declarations for multiplayer callbacks (defined below init). */
static void apply_remote_asteroids(const NetAsteroidState* asteroids, int count);
static void apply_remote_npcs(const NetNpcState* npcs, int count);
static void apply_remote_stations(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock);
static void apply_remote_station_identity(uint8_t index, uint8_t role, uint32_t services,
    float pos_x, float pos_y, float radius, float dock_radius, float signal_range, const char* name);
static void apply_remote_player_state(const NetPlayerState* state);
static void apply_remote_player_ship(const NetPlayerShipState* state);
static void sync_local_player_slot_from_network(void);

static void init(void) {
    memset(&g, 0, sizeof(g));
    g.world.rng = 0xC0FFEE12u;

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
    {
        const char* server_url = NULL;
#ifdef __EMSCRIPTEN__
        server_url = emscripten_run_script_string(
            "(() => {"
            "  const p = new URLSearchParams(window.location.search);"
            "  return p.get('server') || 'wss://signal-ws.ratimics.com/ws';"
            "})()");
#else
        /* Native: check SIGNAL_SERVER environment variable or command line */
        server_url = getenv("SIGNAL_SERVER");
#endif
        if (server_url && server_url[0] != '\0') {
            NetCallbacks cbs = {0};
            cbs.on_state = apply_remote_player_state;
            cbs.on_asteroids = apply_remote_asteroids;
            cbs.on_npcs = apply_remote_npcs;
            cbs.on_stations = apply_remote_stations;
            cbs.on_station_identity = apply_remote_station_identity;
            cbs.on_player_ship = apply_remote_player_ship;
            g.multiplayer_enabled = net_init(server_url, &cbs);
        }
    }
}


/* --- Multiplayer: world state sync callbacks and broadcast --- */

static void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {
    /* Shift current -> previous for interpolation */
    memcpy(g.asteroid_interp.prev, g.asteroid_interp.curr, sizeof(g.asteroid_interp.prev));
    g.asteroid_interp.interval = fmaxf(g.asteroid_interp.t * g.asteroid_interp.interval, 0.05f);
    if (g.asteroid_interp.interval > 0.2f) g.asteroid_interp.interval = 0.1f;
    g.asteroid_interp.t = 0.0f;

    bool received[MAX_ASTEROIDS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;
        received[idx] = true;

        asteroid_t* a = &g.asteroid_interp.curr[idx];
        a->active = (asteroids[i].flags & 1) != 0;
        a->fracture_child = (asteroids[i].flags & (1 << 1)) != 0;
        a->tier = (asteroid_tier_t)((asteroids[i].flags >> 2) & 0x7);
        a->commodity = (commodity_t)((asteroids[i].flags >> 5) & 0x7);
        a->pos.x = asteroids[i].x;
        a->pos.y = asteroids[i].y;
        a->vel.x = asteroids[i].vx;
        a->vel.y = asteroids[i].vy;
        a->hp    = asteroids[i].hp;
        a->ore   = asteroids[i].ore;
        a->radius = asteroids[i].radius;
        if (a->max_hp < a->hp) a->max_hp = a->hp;
        if (a->max_ore < a->ore) a->max_ore = a->ore;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!received[i]) {
            g.asteroid_interp.curr[i].active = false;
        }
    }

    /* Also copy to world for game logic (targeting, beam hit checks) */
    memcpy(g.world.asteroids, g.asteroid_interp.curr, sizeof(g.world.asteroids));
}

static void apply_remote_npcs(const NetNpcState* npcs, int count) {
    memcpy(g.npc_interp.prev, g.npc_interp.curr, sizeof(g.npc_interp.prev));
    g.npc_interp.interval = fmaxf(g.npc_interp.t * g.npc_interp.interval, 0.05f);
    if (g.npc_interp.interval > 0.2f) g.npc_interp.interval = 0.1f;
    g.npc_interp.t = 0.0f;

    bool received[MAX_NPC_SHIPS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;
        received[idx] = true;

        npc_ship_t* n = &g.npc_interp.curr[idx];
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
        n->tint_r = (float)npcs[i].tint_r / 255.0f;
        n->tint_g = (float)npcs[i].tint_g / 255.0f;
        n->tint_b = (float)npcs[i].tint_b / 255.0f;
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!received[i]) {
            g.npc_interp.curr[i].active = false;
        }
    }

    memcpy(g.world.npc_ships, g.npc_interp.curr, sizeof(g.world.npc_ships));
}

static void apply_remote_stations(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        st->ore_buffer[i] = ore_buf[i];
    for (int i = 0; i < COMMODITY_COUNT; i++)
        st->inventory[i] = inventory[i];
    for (int i = 0; i < PRODUCT_COUNT; i++)
        st->product_stock[i] = product_stock[i];
    /* scaffold and scaffold_progress will be updated via a future network message */
}

static void apply_remote_station_identity(uint8_t index, uint8_t role, uint32_t services,
    float pos_x, float pos_y, float radius, float dock_radius, float signal_range,
    const char* name) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    st->role = (station_role_t)role;
    st->services = services;
    st->pos = v2(pos_x, pos_y);
    st->radius = radius;
    st->dock_radius = dock_radius;
    st->signal_range = signal_range;
    snprintf(st->name, sizeof(st->name), "%s", name);
}

static void apply_remote_player_state(const NetPlayerState* state) {
    /* Reconcile local prediction with server-authoritative position. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    sp->ship.pos.x = lerpf(sp->ship.pos.x, state->x, 0.2f);
    sp->ship.pos.y = lerpf(sp->ship.pos.y, state->y, 0.2f);
    sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, 0.2f);
    sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, 0.2f);
    sp->ship.angle = lerp_angle(sp->ship.angle, state->angle, 0.2f);
}

static void apply_remote_player_ship(const NetPlayerShipState* state) {
    /* Apply server-authoritative ship state for the local player. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    sp->ship.hull = state->hull;
    sp->ship.credits = state->credits;
    sp->ship.mining_level = (int)state->mining_level;
    sp->ship.hold_level = (int)state->hold_level;
    sp->ship.tractor_level = (int)state->tractor_level;
    sp->ship.cargo[COMMODITY_FERRITE_ORE] = state->cargo_ferrite;
    sp->ship.cargo[COMMODITY_CUPRITE_ORE] = state->cargo_cuprite;
    sp->ship.cargo[COMMODITY_CRYSTAL_ORE] = state->cargo_crystal;
    /* Dock-state reconciliation: the server is authoritative, but we
     * must not snap back to docked while the local sim has already
     * launched (the server may not have processed the action yet).
     * - Server says undocked → always accept.
     * - Server says docked  → only accept if we locally agree
     *   (still docked or within the dock-prediction guard window). */
    if (!state->docked) {
        sp->docked = false;
    } else if (sp->docked || g.dock_predict_timer <= 0.0f) {
        sp->docked = true;
        sp->current_station = (int)state->current_station;
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
    }
}

static void sync_local_player_slot_from_network(void) {
    uint8_t net_id = net_local_id();
    if (net_id == 0xFF || net_id >= MAX_PLAYERS) return;
    if (g.local_player_slot == (int)net_id) {
        LOCAL_PLAYER.connected = true;
        return;
    }

    server_player_t previous = g.world.players[g.local_player_slot];
    server_player_t* assigned = &g.world.players[net_id];
    memset(&g.world.players[g.local_player_slot], 0, sizeof(g.world.players[g.local_player_slot]));
    g.local_player_slot = (int)net_id;
    if (!assigned->connected && assigned->ship.hull <= 0.0f) {
        *assigned = previous;
    }
    LOCAL_PLAYER.id = net_id;
    LOCAL_PLAYER.connected = true;
    LOCAL_PLAYER.conn = NULL;
}

/* --- Multiplayer: draw remote players as colored triangles --- */
static void draw_remote_players(void) {
    if (!g.multiplayer_enabled) return;
    const NetPlayerState* players = net_get_players();
    static const float colors[][3] = {
        {1.0f, 0.45f, 0.25f},
        {0.25f, 1.0f, 0.55f},
        {0.55f, 0.35f, 1.0f},
        {1.0f, 0.85f, 0.15f},
        {0.15f, 0.85f, 1.0f},
        {1.0f, 0.35f, 0.75f},
    };
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        if (i == (int)net_local_id()) continue;
        int ci = i % 6;
        float cr = colors[ci][0], cg = colors[ci][1], cb = colors[ci][2];
        bool thrusting = (players[i].flags & 1) != 0;
        bool mining = (players[i].flags & 2) != 0;

        sgl_push_matrix();
        sgl_translate(players[i].x, players[i].y, 0.0f);
        sgl_rotate(players[i].angle, 0.0f, 0.0f, 1.0f);

        /* Thrust flame */
        if (thrusting) {
            float flicker = 10.0f + sinf(g.world.time * 42.0f + (float)i * 7.0f) * 3.0f;
            sgl_c4f(1.0f, 0.74f, 0.24f, 0.9f);
            sgl_begin_triangles();
            sgl_v2f(-12.0f, 0.0f);
            sgl_v2f(-26.0f - flicker, 6.0f);
            sgl_v2f(-26.0f - flicker, -6.0f);
            sgl_end();
        }

        /* Hull */
        sgl_c4f(cr, cg, cb, 0.9f);
        sgl_begin_triangles();
        sgl_v2f(22.0f, 0.0f);
        sgl_v2f(-14.0f, 12.0f);
        sgl_v2f(-14.0f, -12.0f);
        sgl_end();

        /* Cockpit */
        sgl_c4f(cr * 0.3f, cg * 0.3f, cb * 0.3f, 1.0f);
        sgl_begin_triangles();
        sgl_v2f(8.0f, 0.0f);
        sgl_v2f(-5.0f, 5.5f);
        sgl_v2f(-5.0f, -5.5f);
        sgl_end();

        /* Wing struts */
        draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), cr * 0.7f, cg * 0.7f, cb * 0.7f, 0.85f);
        draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), cr * 0.7f, cg * 0.7f, cb * 0.7f, 0.85f);

        sgl_pop_matrix();

        /* Mining beam */
        if (mining) {
            vec2 pos = v2(players[i].x, players[i].y);
            vec2 forward = v2_from_angle(players[i].angle);
            vec2 muzzle = v2_add(pos, v2_scale(forward, 24.0f));
            vec2 beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
            draw_segment(muzzle, beam_end, cr, cg, cb, 0.6f);
        }
    }
}

static void render_world(void) {
    vec2 camera = LOCAL_PLAYER.ship.pos;
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
        bool is_current = LOCAL_PLAYER.docked && (i == LOCAL_PLAYER.current_station);
        bool is_nearby = (!LOCAL_PLAYER.docked) && (i == LOCAL_PLAYER.nearby_station);
        draw_station(&g.world.stations[i], is_current, is_nearby);
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.world.asteroids[i].active) {
            continue;
        }
        draw_asteroid(&g.world.asteroids[i], i == LOCAL_PLAYER.hover_asteroid);
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

static void interpolate_world_for_render(void) {
    if (!g.multiplayer_enabled || !net_is_connected()) return;
    float t = clampf(g.asteroid_interp.t, 0.0f, 1.2f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *dst = &g.world.asteroids[i];
        const asteroid_t *prev = &g.asteroid_interp.prev[i];
        const asteroid_t *curr = &g.asteroid_interp.curr[i];
        /* Use current state for everything except position */
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, t);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, t);
            dst->rotation = prev->rotation + (curr->rotation - prev->rotation) * t;
        }
    }

    float nt = clampf(g.npc_interp.t, 0.0f, 1.2f);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *dst = &g.world.npc_ships[i];
        const npc_ship_t *prev = &g.npc_interp.prev[i];
        const npc_ship_t *curr = &g.npc_interp.curr[i];
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, nt);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, nt);
            dst->angle = lerp_angle(prev->angle, curr->angle, nt);
        }
    }
}

static void render_frame(void) {
    interpolate_world_for_render();
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

    /* --- Multiplayer: poll incoming and send input BEFORE sim --- */
    if (g.multiplayer_enabled) {
        bool was_connected = net_is_connected();
        net_poll();
        sync_local_player_slot_from_network();
        if (was_connected && !net_is_connected()) {
            set_notice("Connection lost. Continuing offline.");
        }
        /* Send input at ~30 Hz, or immediately if there's a one-shot action. */
        {
            uint8_t action = g.pending_net_action;
            g.net_input_timer -= frame_dt;
            if (g.net_input_timer <= 0.0f || action != 0) {
                g.net_input_timer = 1.0f / 30.0f;
                uint8_t flags = 0;
                if (g.input.key_down[SAPP_KEYCODE_W] || g.input.key_down[SAPP_KEYCODE_UP])
                    flags |= NET_INPUT_THRUST;
                if (g.input.key_down[SAPP_KEYCODE_S] || g.input.key_down[SAPP_KEYCODE_DOWN])
                    flags |= NET_INPUT_BRAKE;
                if (g.input.key_down[SAPP_KEYCODE_A] || g.input.key_down[SAPP_KEYCODE_LEFT])
                    flags |= NET_INPUT_LEFT;
                if (g.input.key_down[SAPP_KEYCODE_D] || g.input.key_down[SAPP_KEYCODE_RIGHT])
                    flags |= NET_INPUT_RIGHT;
                if (g.input.key_down[SAPP_KEYCODE_SPACE])
                    flags |= NET_INPUT_FIRE;
                g.pending_net_action = 0;
                net_send_input(flags, action);
            }
        }
    }

    advance_simulation_frame(frame_dt);
    audio_generate_stream(&g.audio);

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
