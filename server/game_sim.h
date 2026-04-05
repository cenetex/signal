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

typedef struct {
    spatial_cell_t cells[SPATIAL_GRID_DIM][SPATIAL_GRID_DIM];
    float offset_x, offset_y;  /* world offset to center grid */
} spatial_grid_t;

/* ------------------------------------------------------------------ */
/* Server-specific types                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    float turn;
    float thrust;
    bool mine;
    bool interact;
    bool service_sell;
    bool service_repair;
    bool upgrade_mining;
    bool upgrade_hold;
    bool upgrade_tractor;
    bool place_outpost;
    bool place_module;          /* attach scaffold kit as module at own station */
    bool buy_scaffold_kit;
    module_type_t scaffold_kit_module; /* what module type the kit builds */
    bool build_module;
    module_type_t build_module_type;
    uint8_t build_ring;       /* target ring (1 or 2) */
    uint8_t build_slot;       /* target port (0-7, or 0xFF for ring itself) */
    bool buy_product;
    commodity_t buy_commodity;
    int mining_target_hint;  /* client's hover_asteroid, -1 = none */
    bool hail;               /* collect pending credits from nearby station */
    bool release_tow;        /* drop all towed fragments */
    bool reset;
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
} server_player_t;

typedef struct {
    station_t stations[MAX_STATIONS];
    asteroid_t asteroids[MAX_ASTEROIDS];
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
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
} world_t;

/* ------------------------------------------------------------------ */
/* Hull definitions (declared in shared/types.h, defined in game_sim.c) */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

float contract_price(const contract_t *c);
void world_reset(world_t *w);
void world_sim_step(world_t *w, float dt);
void world_sim_step_player_only(world_t *w, int player_idx, float dt);
void player_init_ship(server_player_t *sp, world_t *w);
float signal_strength_at(const world_t *w, vec2 pos);
void rebuild_signal_chain(world_t *w);
bool can_place_outpost(const world_t *w, vec2 pos);
void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type);
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int ring, int slot);
void step_module_delivery(world_t *w, station_t *st, int station_idx, ship_t *ship);
int try_place_outpost(world_t *w, server_player_t *sp, vec2 pos);
bool world_save(const world_t *w, const char *path);
bool world_load(world_t *w, const char *path);
bool player_save(const server_player_t *sp, const char *dir, int slot);
bool player_load(server_player_t *sp, world_t *w, const char *dir, int slot);
bool player_load_by_token(server_player_t *sp, world_t *w, const char *dir,
                          const uint8_t token[8]);

#endif /* GAME_SIM_H */
