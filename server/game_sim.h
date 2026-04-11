/*
 * game_sim.h -- Headless game simulation types and API for the
 * Signal Space Miner authoritative server.
 *
 * Shared types (vec2, ship_t, station_t, etc.) come from shared/types.h.
 * Server-only types (server_player_t, world_t) are defined here.
 */
#ifndef GAME_SIM_H
#define GAME_SIM_H

#include <stdio.h>
#include <string.h>
#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "belt.h"
#include "ship.h"
#include "asteroid.h"
#include "economy.h"

/* ------------------------------------------------------------------ */
/* Constants (server-only)                                            */
/* ------------------------------------------------------------------ */

enum {
    MAX_PLAYERS = 32,
};

static const float WORLD_RADIUS = 50000.0f;  /* safety net; gameplay bounded by station signal_range */
static const float OUTPOST_CREDIT_COST = 500.0f;
static const float OUTPOST_RADIUS = 40.0f;
static const float OUTPOST_DOCK_RADIUS = 96.0f;
static const float OUTPOST_SIGNAL_RANGE = 6000.0f;
static const float OUTPOST_MIN_DISTANCE = 800.0f; /* min distance between stations */
static const float SIM_DT = 1.0f / 120.0f;
static const float MINING_RANGE = 170.0f;
static const float SHIP_BRAKE = 180.0f;
static const float FRAGMENT_TRACTOR_ACCEL = 380.0f;
static const float FRAGMENT_MAX_SPEED = 210.0f;
static const float FRAGMENT_NEARBY_RANGE = 220.0f;
static const int FIELD_ASTEROID_TARGET = 220;
static const float FIELD_ASTEROID_RESPAWN_DELAY = 0.2f;
static const float FRACTURE_CHILD_CLEANUP_AGE = 30.0f;
static const float FRACTURE_CHILD_CLEANUP_DISTANCE = 4000.0f;
static const float STATION_DOCK_APPROACH_OFFSET = 34.0f;
static const float SHIP_COLLISION_DAMAGE_THRESHOLD = 115.0f;
static const float SHIP_COLLISION_DAMAGE_SCALE = 0.12f;
static const float NPC_DOCK_TIME = 3.0f;
static const float HAULER_DOCK_TIME = 4.0f;
static const float HAULER_LOAD_TIME = 2.0f;
static const float COLLECTION_FEEDBACK_TIME = 1.1f;


/* ------------------------------------------------------------------ */
/* Spatial grid for O(1) neighbor lookups instead of O(N^2)           */
/* ------------------------------------------------------------------ */

#define SPATIAL_CELL_SIZE 800.0f
#define SPATIAL_GRID_DIM 128
#define SPATIAL_MAX_PER_CELL 16

typedef struct {
    int16_t indices[SPATIAL_MAX_PER_CELL];
    uint8_t count;
} spatial_cell_t;

/* Packed cell coordinate for the occupied-cell list. */
typedef struct {
    uint8_t x, y;
} spatial_cell_coord_t;

typedef struct {
    spatial_cell_t cells[SPATIAL_GRID_DIM][SPATIAL_GRID_DIM];
    float offset_x, offset_y;  /* world offset to center grid */
    /* Occupied cell tracking: only clear cells that had entries. */
    spatial_cell_coord_t occupied[MAX_ASTEROIDS];
    int occupied_count;
} spatial_grid_t;

/* Map a world position to a grid cell (clamped). Shared by game_sim.c
 * and sim_nav.c, so declared here as static inline. */
static inline void spatial_grid_cell(const spatial_grid_t *g, vec2 pos, int *cx, int *cy) {
    *cx = (int)((pos.x + g->offset_x) / SPATIAL_CELL_SIZE);
    *cy = (int)((pos.y + g->offset_y) / SPATIAL_CELL_SIZE);
    if (*cx < 0) *cx = 0;
    if (*cy < 0) *cy = 0;
    if (*cx >= SPATIAL_GRID_DIM) *cx = SPATIAL_GRID_DIM - 1;
    if (*cy >= SPATIAL_GRID_DIM) *cy = SPATIAL_GRID_DIM - 1;
}

/* ------------------------------------------------------------------ */
/* Cached signal strength grid — O(1) lookups instead of O(N_stations)*/
/* ------------------------------------------------------------------ */

#define SIGNAL_GRID_DIM  256
#define SIGNAL_CELL_SIZE 200.0f  /* covers ±25,600 units from origin */

typedef struct {
    float *strength;           /* heap-allocated SIGNAL_GRID_DIM² floats */
    float offset_x, offset_y;  /* world offset to center grid */
    bool  valid;                /* false = needs rebuild */
} signal_grid_t;

/* ------------------------------------------------------------------ */
/* Server-specific types                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    float turn;
    float thrust;
    bool mine;
    bool interact;
    bool service_sell;
    /* Selective delivery filter for service_sell. COMMODITY_COUNT means
     * "deliver everything that fits a contract or the primary buy slot"
     * (the default). Setting this to a specific commodity restricts the
     * delivery to that one commodity, so the player can keep e.g. their
     * crystal cargo while still delivering ferrite. */
    commodity_t service_sell_only;
    bool service_repair;
    bool upgrade_mining;
    bool upgrade_hold;
    bool upgrade_tractor;
    bool place_outpost;
    /* Optional explicit target for tow placement. If place_target_station >= 0,
     * the server places the towed scaffold at that ring/slot; otherwise it
     * auto-snaps to the closest valid slot or founds a new outpost. */
    int8_t place_target_station;
    int8_t place_target_ring;
    int8_t place_target_slot;
    /* Planning mode: add a placement plan to a station. */
    bool add_plan;
    int8_t plan_station;
    int8_t plan_ring;
    int8_t plan_slot;
    module_type_t plan_type;
    /* Create a new planned outpost (server-side ghost). */
    bool create_planned_outpost;
    vec2 planned_outpost_pos;
    /* Cancel a planned outpost (only the owner can). */
    bool cancel_planned_outpost;
    int8_t cancel_planned_station;
    /* Cancel a single placement plan on a station slot. */
    bool cancel_plan_slot;
    int8_t cancel_plan_st;
    int8_t cancel_plan_ring;
    int8_t cancel_plan_sl;
    bool buy_scaffold_kit;
    module_type_t scaffold_kit_module; /* what module type the kit builds */
    bool buy_product;
    commodity_t buy_commodity;
    int mining_target_hint;  /* client's hover_asteroid, -1 = none */
    bool hail;               /* collect pending credits from nearby station */
    bool release_tow;        /* drop all towed fragments */
    bool reset;
    bool toggle_autopilot;   /* one-shot: flip autopilot_mode on/off */
} input_intent_t;

typedef struct {
    bool connected;
    uint8_t id;
    void *conn;
    uint8_t session_token[8]; /* stable identity for save persistence */
    bool session_ready;       /* true once client sends SESSION message */
    bool grace_period;        /* true while waiting for reconnect after disconnect */
    float grace_timer;        /* seconds remaining in grace window */
    ship_t ship;
    input_intent_t input;
    int current_station;
    int nearby_station;
    bool docked;
    bool in_dock_range;
    bool docking_approach;  /* tractor pulling ship toward core berth */
    int dock_berth;         /* berth slot (0-3) when docked */
    bool beam_active;
    bool beam_hit;
    bool beam_ineffective; /* hitting a rock too tough for current laser level */
    bool scan_active;      /* laser scanning a non-asteroid target */
    int scan_target_type;  /* 0=none, 1=station_module, 2=npc, 3=player */
    int scan_target_index; /* index into stations/npc_ships/players array */
    int scan_module_index; /* module index within station (for type=1) */
    int hover_asteroid;
    vec2 beam_start;
    vec2 beam_end;
    float cargo_sale_value;
    int nearby_fragments;
    int tractor_fragments;
    bool was_in_signal;     /* previous frame's signal state, for edge detection */
    char callsign[8];       /* e.g. "KRX-472" */
    /* Autopilot — server-side AI driving the player's ship.
     * 0 = off (manual control)
     * 1 = mining loop: mine → tow → dock → sell → undock → repeat
     * Manual input (turn/thrust/mine) cancels the autopilot. */
    bool actual_thrusting;      /* true if the ship thrusted this tick (survives input restore) */
    uint8_t autopilot_mode;
    int autopilot_target;       /* asteroid idx or -1 */
    int autopilot_state;        /* internal state machine cursor */
    float autopilot_timer;
    vec2 autopilot_last_pos;    /* position snapshot for stuck detection */
    float autopilot_stuck_timer;/* seconds since meaningful movement */
} server_player_t;

typedef struct {
    station_t stations[MAX_STATIONS];
    asteroid_t asteroids[MAX_ASTEROIDS];
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
    scaffold_t scaffolds[MAX_SCAFFOLDS];
    server_player_t players[MAX_PLAYERS];
    uint32_t rng;
    float time;
    float field_spawn_timer;
    float gravity_accumulator;  /* runs gravity at reduced rate */
    sim_events_t events;
    contract_t contracts[MAX_CONTRACTS];
    bool player_only_mode;
    belt_field_t belt;
    spatial_grid_t asteroid_grid;
    signal_grid_t signal_cache;
} world_t;

/* ------------------------------------------------------------------ */
/* Hull definitions (declared in shared/types.h, defined in game_sim.c) */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Logging — define GAME_SIM_VERBOSE to enable [sim] printf chatter   */
/* ------------------------------------------------------------------ */

#ifdef GAME_SIM_VERBOSE
#define SIM_LOG(...) printf(__VA_ARGS__)
#else
#define SIM_LOG(...) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

float contract_price(const contract_t *c);
void world_reset(world_t *w);
void world_cleanup(world_t *w);
void world_sim_step(world_t *w, float dt);
void world_sim_step_player_only(world_t *w, int player_idx, float dt);
void player_init_ship(server_player_t *sp, world_t *w);
float signal_strength_at(const world_t *w, vec2 pos);
void spatial_grid_build(world_t *w);

/* Nav API — canonical declarations in sim_nav.h.
 * Repeated here because sim_nav.h includes game_sim.h (circular).
 * Client code (src/) includes game_sim.h but not sim_nav.h. */
int nav_get_player_path(int player_id, vec2 *out_waypoints, int max_count, int *out_current);
int nav_compute_path(const world_t *w, vec2 start, vec2 goal, float clearance,
                     vec2 *out_waypoints, int max_count);
bool nav_segment_clear(const world_t *w, vec2 start, vec2 goal, float clearance);
void station_rebuild_all_nav(const world_t *w);
void rebuild_signal_chain(world_t *w);
bool can_place_outpost(const world_t *w, vec2 pos);
void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type);
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int ring, int slot);
void step_module_delivery(world_t *w, station_t *st, int station_idx, ship_t *ship);
int spawn_scaffold(world_t *w, module_type_t type, vec2 pos, int owner);
bool world_save(const world_t *w, const char *path);
bool world_load(world_t *w, const char *path);
bool player_save(const server_player_t *sp, const char *dir, int slot);
bool player_load(server_player_t *sp, world_t *w, const char *dir, int slot);
bool player_load_by_token(server_player_t *sp, world_t *w, const char *dir,
                          const uint8_t token[8]);

/* Cross-module sim helpers — defined in game_sim.c, used by sim_*.c. */
void anchor_ship_in_station(server_player_t *sp, world_t *w);
asteroid_tier_t max_mineable_tier(int mining_level);
vec2 station_approach_target(const station_t *st, vec2 from);
void emit_event(world_t *w, sim_event_t ev);
void earn_credits(ship_t *s, float amount);
void fracture_asteroid(world_t *w, int idx, vec2 outward_dir, int8_t fractured_by);
void activate_outpost(world_t *w, int station_idx);

#define DOCK_APPROACH_RANGE 300.0f /* range to detect station for docking */

/* Hopper/furnace constants — shared between game_sim.c and sim_production.c */
#define HOPPER_PULL_RANGE 300.0f    /* furnace attracts fragments from this far */
#define HOPPER_PULL_ACCEL 500.0f    /* pull strength */

#endif /* GAME_SIM_H */
