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
#include "local_server.h"
#include "net.h"
#include "episode.h"
#include "music.h"

/* Sokol headers (declarations only -- SOKOL_IMPL is in main.c) */
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"

/* ------------------------------------------------------------------ */
/* Station docked view enum                                           */
/* ------------------------------------------------------------------ */
/* The docked UI is a verb-list action surface, not a tab system.
 * VERBS is the default screen players see on dock — every visible row
 * is an action with a real cost/payoff and a single-letter hotkey.
 * JOBS and BUILD are sub-screens entered with [J] / [B] and exited
 * with [Esc]. There is no STATUS / MARKET / NETWORK / GRADES tab —
 * persistent ship+ledger info lives in the always-visible header band,
 * and the deleted tabs were full of unactionable display data. */

typedef enum {
    STATION_VIEW_DOCK = 0,       /* ship bay: repair / refit / current ship state */
    STATION_VIEW_TRADE,          /* market: buy / sell cargo */
    STATION_VIEW_WORK,           /* dispatch: jobs / contracts */
    STATION_VIEW_YARD,           /* fabrication: kits + construction queue */
    STATION_VIEW_COUNT,
} station_view_t;

/* ------------------------------------------------------------------ */
/* Station UI state (computed per frame when docked)                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const station_t* station;
    int hull_now;
    int hull_max;
    int repair_cost;
    /* Per-upgrade module accounting. Real cost is the modules
     * themselves; credit_cost is what the dock charges to fill the
     * gap from its inventory. units_in_cargo + units_at_station
     * must be >= units_needed for the upgrade to fire. */
    int mining_units_needed, mining_units_in_cargo, mining_units_at_station, mining_credit_cost;
    int hold_units_needed,   hold_units_in_cargo,   hold_units_at_station,   hold_credit_cost;
    int tractor_units_needed,tractor_units_in_cargo,tractor_units_at_station,tractor_credit_cost;
    bool can_repair;
    bool can_upgrade_mining;
    bool can_upgrade_hold;
    bool can_upgrade_tractor;
    /* Repair-kit availability for the [R] row. ship_kits + station_kits
     * are floor counts (whole HP that can be applied); kits_short_by is
     * how many HP the local supply can't cover for a full repair. */
    int ship_kits;
    int station_kits;
    int kits_short_by;
} station_ui_state_t;

/* TRADE picker row — single source of truth shared by the picker
 * renderer (station_ui.c) and the input handler (input.c). Both walk
 * the SAME row list so a [1] keypress can never hit a different row
 * than the one drawn on screen. See build_trade_rows() below. */
typedef struct {
    uint8_t        kind;       /* 0 = BUY (station sells), 1 = SELL (station buys) */
    commodity_t    commodity;
    mining_grade_t grade;
    int            stock;      /* units available on the active side */
    int            unit_price; /* per-unit, already grade-multiplied */
    bool           actionable; /* player can do this transaction right now */
    bool           is_float_fallback; /* legacy float row (no manifest unit) */
    int            station_stock;   /* this commodity's inventory at station */
    int            station_capacity;/* MAX_PRODUCT_STOCK */
    int            held;            /* player's cargo of this (commodity,grade) */
    uint8_t        block_reason;    /* see TRADE_BLOCK_* below; 0 if actionable */
} trade_row_t;

/* Why an otherwise-valid row is non-actionable. Drives the status text
 * shown in place of the +/- price when actionable=false. */
enum {
    TRADE_BLOCK_NONE          = 0,
    TRADE_BLOCK_STATION_FULL  = 1, /* sell: station's hopper at capacity */
    TRADE_BLOCK_STATION_EMPTY = 2, /* buy:  station has none on the shelf */
    TRADE_BLOCK_NO_BUYER      = 3, /* sell: this station doesn't consume it */
    TRADE_BLOCK_NO_SELLER     = 4, /* buy:  this station doesn't produce it */
    TRADE_BLOCK_HOLD_FULL     = 5, /* buy:  ship cargo would overflow */
    TRADE_BLOCK_NO_FUNDS      = 6, /* buy:  ledger short for unit price */
    TRADE_BLOCK_NO_CARGO      = 7, /* sell: player carries none of this */
};

/* Pagination constants — input.c walks `g.trade_page * TRADE_ROWS_PER_PAGE`
 * to find the first row on the current page; the renderer wraps when
 * total_pages * TRADE_ROWS_PER_PAGE >= row_count. */
#define TRADE_ROWS_PER_PAGE 5
#define TRADE_MAX_ROWS      20

/* Build the unified row list for `st` against the player's `ship`.
 * Output is zero-or-more rows in `out[0..count-1]`, capped at `max`
 * (caller passes a TRADE_MAX_ROWS-sized buffer). Returns the count.
 * BUY rows come first, then SELL rows — matches the picker layout. */
int build_trade_rows(const station_t *st, const ship_t *ship,
                     trade_row_t out[], int max);

/* ------------------------------------------------------------------ */
/* Client game state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    bool key_down[KEY_COUNT];
    bool key_pressed[KEY_COUNT];
    float tractor_press_time;  /* world time when R was pressed, 0 = not held */
    float self_destruct_hold_time; /* world time when X press began; 0 = not held */
} input_state_t;

typedef struct {
    float accumulator;
} runtime_state_t;

typedef struct {
    input_state_t input;
    star_t stars[MAX_STARS];
    bool thrusting;
    bool server_thrusting;  /* server-authoritative thrust (for autopilot flames) */
    /* Transient notice for the bottom-center subtitle. Station hails
     * can be long ("Station name: ...MOTD... (balance N cur)"), so the
     * buffer is generous. The HUD wraps it into multiple lines. */
    char notice[320];
    float notice_timer;
    float collection_feedback_ore;
    int collection_feedback_fragments;
    float collection_feedback_timer;
    /* Floating "+$N" popups spawned on SIM_EVENT_SELL. World-space text
     * that rises and fades over ~1.5s. Yellow for contract-priced sales,
     * grade-tinted otherwise. */
    struct {
        vec2 pos;
        float age;          /* seconds since spawn */
        float life;         /* seconds to live (0 = unused) */
        uint8_t r, g, b;
        char text[16];
    } sell_fx[16];
    /* Floating "-N" damage popups spawned on SIM_EVENT_DAMAGE. World-space
     * red text that rises and fades over ~1.0s. Smaller pool than sell_fx
     * because hits are less frequent than sales but burst-y during
     * combat. */
    struct {
        vec2 pos;
        float age;
        float life;
        char text[16];   /* "-2147483648" worst case + null + slack */
    } damage_fx[8];
    /* Hit vignette: red border pulse on the local player's HUD when they
     * take damage. Set on SIM_EVENT_DAMAGE, decays each frame. */
    float damage_flash_timer;
    /* Directional hit indicator. dir_x/dir_y is a unit vector in WORLD
     * space pointing from the ship toward the source of the most
     * recent damage event. Renderer projects it onto the screen edge.
     * timer counts down from ~1.5 s. (0,0) = no source / unknown. */
    float damage_dir_x;
    float damage_dir_y;
    float damage_dir_timer;
    /* PvP kill-feed: short text shown at top-center for ~3 s when
     * SIM_EVENT_NPC_KILL fires (eventually also when a remote player
     * dies). Text + remaining lifetime; 0 = empty. */
    char  kill_feed_text[64];
    float kill_feed_timer;
    /* Per-station manifest summary — [commodity][grade] unit counts.
     * Unified read path for the TRADE UI whether we're in singleplayer
     * (populated every frame from g.world.stations[s].manifest) or
     * multiplayer (populated from the server's manifest-summary broadcast
     * — see Phase 2 wire TODO in server/main.c / src/net.c). Kept off
     * station_t because station_t is the authoritative save/wire format
     * and this is derived. */
    uint16_t station_manifest_summary[MAX_STATIONS][COMMODITY_COUNT][MINING_GRADE_COUNT];

    /* Batched sell summary for the bottom-right hint bar — every payout
     * flashes "[ +$N common xA fine xB ... ]" even when the station is
     * off-camera. Events within settle_timer (~0.6s) accumulate into one
     * batch; once idle, display_timer holds the result on-screen (~3s)
     * so the HUD can render it with per-grade colors. */
    struct {
        bool active;          /* accumulating new events */
        float settle_timer;   /* time left before the batch "flushes" to display */
        float display_timer;  /* time left showing the flushed summary in the HUD */
        int total_cr;
        int grade_counts[MINING_GRADE_COUNT];
        bool any_by_contract;
    } sell_batch;
    runtime_state_t runtime;
    audio_state_t audio;
    sg_pass_action pass_action;
    /* --- Simulation --- */
    world_t world;
    local_server_t local_server;
    int local_player_slot;
    /* --- Multiplayer --- */
    bool multiplayer_enabled;
    float net_send_timer;
    uint8_t pending_net_action;
    /* Rides alongside pending_net_action: which grade did the player
     * pick for a BUY_PRODUCT action? Default = MINING_GRADE_COUNT ("any"
     * → server does FIFO). Set by the grade-picker in TRADE. */
    uint8_t pending_net_buy_grade;
    /* Rides alongside pending_net_action when the action is
     * NET_ACTION_PLACE_OUTPOST: which (station, ring, slot) did the
     * client's reticle pick? -1 means "let the server auto-snap" (the
     * old auto-place path for SIGNAL_RELAY founding moments). Without
     * these, MP would snap to default 0/0/0 and the slot-taken check
     * would silently fail. */
    int8_t  pending_net_place_station;
    int8_t  pending_net_place_ring;
    int8_t  pending_net_place_slot;
    float action_predict_timer;
    float net_input_timer;
    station_view_t station_view;
    /* TRADE tab pagination: [F] cycles through pages of 5 rows each.
     * Page 0 is rows 0..4, page 1 is 5..9, etc. Wraps when > 9 rows. */
    uint8_t trade_page;
    bool was_docked;
    bool was_autopilot;
    float dock_settle_timer;  /* delay before showing station panel after dock */
    /* --- Onboarding (first-run progression hints) --- */
    struct {
        bool moved;          /* pressed a movement key */
        bool fractured;      /* broke an asteroid */
        bool tractored;      /* collected ore fragments */
        bool hailed;         /* pressed H to hail a station */
        bool boosted;        /* held SHIFT outside core signal */
        bool complete;       /* all 5 steps done — stations take over */
        bool welcomed;       /* completion message shown */
        bool loaded;         /* state loaded from localStorage */
    } onboarding;
    /* --- Module activation effect --- */
    float commission_timer;     /* countdown for activation flash */
    vec2 commission_pos;        /* world position of activated module */
    float commission_cr, commission_cg, commission_cb; /* module color */
    /* --- Death screen --- */
    float death_screen_timer;       /* legacy countdown — unused while cinematic.active */
    float death_screen_max;
    float death_ore_mined;
    float death_credits_earned;
    float death_credits_spent;
    int death_asteroids_fractured;
    /* Global leaderboard from server (top-N by credits earned at death).
     * Populated on join + after every death in MP. SP leaves it empty. */
    struct {
        char  callsign[8];
        float credits_earned;
    } highscores[10 /* HIGHSCORE_TOP_N */];
    int highscore_count;
    /* Smoothed fog intensity (0..1). Tracks 1 - (hull/max_hull) but
     * eases in/out so the vignette rolls smoothly instead of snapping. */
    float fog_intensity;
    /* Death cinematic — anchors the camera to a shattered wreckage at
     * the place the player died, regardless of where the server has
     * respawned the actual ship. Phases:
     *   0 = drift (aftermath, no UI)
     *   1 = stats (death menu visible, prompt to launch)
     *   2 = closing (cinematic releases, normal flow resumes) */
    struct {
        bool active;
        int phase;
        vec2 pos;
        vec2 vel;
        float angle;
        float spin;
        float age;
        float menu_alpha;  /* eased toward 1 in phase 1 */
        float fragments[8][6]; /* per-shard: dx, dy, vx, vy, angle, spin */
    } death_cinematic;
    /* --- Episode & Music --- */
    episode_state_t episode;
    music_state_t music;
    /* --- Scaffold placement reticle (when towing a scaffold) --- */
    bool placement_reticle_active;
    int placement_target_station;  /* station index, -1 = none */
    int placement_target_ring;     /* 1..STATION_NUM_RINGS */
    int placement_target_slot;     /* 0..STATION_RING_SLOTS[ring]-1 */
    /* --- Plan mode (B near outpost or planned outpost, not towing) --- */
    bool plan_mode_active;
    int plan_type;                 /* module_type_t cycled with R */
    int plan_target_station;       /* server-side station index being planned (-1 = ghost, >=3 = real) */
    /* Grace window after pressing B in empty space: stay in plan mode
     * until the server-created planned outpost shows up in reticle
     * targets. Without this, the user has to press B twice — once to
     * create, once to actually enter plan mode after the ghost arrives. */
    float plan_mode_grace_until;
    /* Lock effect: flash/pulse at the position where a planned outpost
     * is locked by its first placement plan. */
    float outpost_lock_timer;
    vec2  outpost_lock_pos;
    /* CONTRACTS tab selective delivery: -1 = no selection (E delivers
     * everything matching), otherwise the contract index whose
     * commodity will be the only one delivered on next E press. */
    int selected_contract;
    /* --- Module interaction --- */
    int target_station;      /* station index of targeted module, -1 = none */
    int target_module;       /* module index within station, -1 = none */
    int inspect_station;     /* module info pane: station index, -1 = closed */
    int inspect_module;      /* module info pane: module index */
    /* --- Hail overlay --- */
    float hail_timer;            /* countdown for hail display */
    char hail_station[64];       /* station name */
    char hail_message[256];      /* station MOTD */
    float hail_credits;          /* balance at hailed station */
    float station_balance;       /* balance at current/nearby station (multiplayer) */
    int hail_station_index;      /* which station was hailed (-1 = none) */
    /* Hail ping visual: expanding ring from the ship on H-press. Driven
     * locally from input.c so the click has immediate feedback even if
     * the server takes a frame to respond. */
    float hail_ping_timer;       /* seconds since last ping, 0 = inactive */
    vec2  hail_ping_origin;      /* world-space origin (ship pos at press) */
    float hail_ping_range;       /* ship comm_range at press time */
    /* --- Camera --- */
    vec2 camera_pos;         /* smoothed camera position */
    bool camera_initialized;
    /* Boost camera: eases in + recenters on the ship while SHIFT is
     * held, releases smoothly on let-go. Inverse of the hail ping zoom.
     * boost_zoom multiplies camera half-extents (<1 = zoomed in).
     * boost_center_blend blends extra centering on top of the deadzone. */
    float boost_zoom;
    float boost_center_blend;
    float camera_drift_timer; /* seconds the ship has been outside the deadzone — drives lazy recenter */
    int camera_station_side;  /* +1 = anchor station on right, -1 = left, 0 = unset */
    int camera_station_v_side; /* +1 = bottom, -1 = top, 0 = unset */
    int camera_station_index;  /* which station the cinematic is anchored to */
    /* Screen shake on damage. amplitude decays exponentially each frame. */
    float screen_shake;      /* current shake amplitude in world units */
    float screen_shake_seed; /* monotonic phase for noise lookup */
    /* --- Autopilot path preview (dotted line showing next waypoints) --- */
    vec2 autopilot_path[12];
    int  autopilot_path_count;
    int  autopilot_path_current;
    /* --- Contract tracking --- */
    int tracked_contract;    /* index into world.contracts, -1 = none */
    /* --- Navigation breadcrumb (last docked station or placed blueprint) --- */
    bool nav_pip_active;
    vec2 nav_pip_pos;
    bool nav_pip_is_blueprint;  /* false = station, true = placed blueprint */
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
    struct {
        NetPlayerState prev[NET_MAX_PLAYERS];
        NetPlayerState curr[NET_MAX_PLAYERS];
        float t;
        float interval;
    } player_interp;
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
#define STATION_PANEL_HEIGHT 470.0f
#define STATION_PANEL_COMPACT_WIDTH 520.0f
#define STATION_PANEL_COMPACT_HEIGHT 360.0f
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

/* Safari has been seen to return NaN briefly from sapp_widthf /
 * sapp_heightf / sapp_dpi_scale on the frame the audio context resumes
 * (the gesture that also fires LAUNCH). NaN propagates into canvas /
 * camera math and trips a sokol hard assert (!isnan in sdtx_canvas).
 * Clamp each sokol reading at the source. */
static inline float ui_safe_positive(float v, float fallback) {
    return (isfinite(v) && v > 0.0f) ? v : fallback;
}

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
void draw_hail_ping(void);
float hail_ping_camera_zoom(void);
float player_current_balance(void);

/* Pre-bake the radial fog vignette textures (one per damage tier).
 * Must be called once after sg_setup. */
void hull_fog_init(void);

/* ------------------------------------------------------------------ */
/* Station UI functions (implemented in station_ui.c)                 */
/* ------------------------------------------------------------------ */

void build_station_ui_state(station_ui_state_t* ui);
void draw_station_services(const station_ui_state_t* ui);

/* WORK-tab contract slot ordering. Single source of truth shared
 * between draw_jobs_view (renders the rows) and the input layer
 * (handles [1]/[2]/[3] selection) so the keypress always maps to the
 * row the player sees. Up to 3 slots filled in this order:
 *   1. Active TRACTOR contracts at this station the player can
 *      currently fulfill (carries the commodity in cargo or towed
 *      raw-ore fragments). Marked fulfillable=true.
 *   2. Nearest active contracts (any station) by squared distance
 *      from `here`, used to fill remaining slots. fulfillable=false.
 *
 * Returns the number of slots filled (0..3). out_held holds the
 * integer-rounded carry quantity for the fulfillable slots and 0 for
 * the nearest-fill slots. Pass `here_idx = -1` if not docked. */
int build_work_slots(int here_idx, vec2 here_pos,
                     int out_contracts[3],
                     bool out_fulfillable[3],
                     int out_held[3]);

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
int nearest_signal_station(vec2 pos);
const station_t* navigation_station_ptr(void);

/* (Old MARKET / STATUS formatter helpers retired in the docked-UI
 * redesign — the verb-list view computes its rows inline from station
 * + ship state and doesn't need separate format helpers.) */

/* ------------------------------------------------------------------ */
/* Plan helpers — module types the local player has currently planned */
/* across all stations. Used by plan-mode (R cycling) and the shipyard */
/* order menu so kits only appear for things the player actually plans. */
/* PLAYER_PLAN_TYPE_LIMIT is defined in shared/types.h. */
/* ------------------------------------------------------------------ */

/* Returns count of distinct planned module types for the local player.
 * Writes them into out (deduped, capped at max). */
int player_planned_types(module_type_t *out, int max);

#endif /* CLIENT_H */
