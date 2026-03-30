/*
 * game_sim.h -- Headless game simulation types and API for the
 * Signal Space Miner authoritative server.
 *
 * Extracted from src/main.c with all rendering / audio / sokol
 * dependencies removed.
 */
#ifndef GAME_SIM_H
#define GAME_SIM_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

enum {
    MAX_ASTEROIDS       = 48,
    MAX_STATIONS        = 3,
    MAX_NPC_SHIPS       = 6,
    MAX_PLAYERS         = 32,
};

#define PI_F      3.14159265359f
#define TWO_PI_F  6.28318530718f

#define WORLD_RADIUS                2200.0f
#define SIM_DT                      (1.0f / 120.0f)
#define MINING_RANGE                170.0f
#define SHIP_BRAKE                  180.0f
#define SHIP_HOLD_UPGRADE_STEP      24.0f
#define SHIP_MINING_UPGRADE_STEP    7.0f
#define SHIP_TRACTOR_UPGRADE_STEP   24.0f
#define SHIP_BASE_COLLECT_RADIUS    30.0f
#define SHIP_COLLECT_UPGRADE_STEP   5.0f
#define FRAGMENT_TRACTOR_ACCEL      380.0f
#define FRAGMENT_MAX_SPEED          210.0f
#define FRAGMENT_NEARBY_RANGE       220.0f
#define REFINERY_HOPPER_CAPACITY    100.0f
#define REFINERY_BASE_SMELT_RATE    0.5f
#define REFINERY_MAX_FURNACES       3
#define FIELD_ASTEROID_TARGET       32
#define FIELD_ASTEROID_RESPAWN_DELAY 0.6f
#define FRACTURE_CHILD_CLEANUP_AGE  22.0f
#define FRACTURE_CHILD_CLEANUP_DISTANCE 940.0f
#define COLLECTION_FEEDBACK_TIME    1.1f
#define SHIP_UPGRADE_MAX_LEVEL      4
#define STATION_REPAIR_COST_PER_HULL 2.0f
#define STATION_DOCK_APPROACH_OFFSET 34.0f
#define SHIP_COLLISION_DAMAGE_THRESHOLD 115.0f
#define SHIP_COLLISION_DAMAGE_SCALE 0.12f
#define NPC_DOCK_TIME               3.0f
#define HAULER_DOCK_TIME            4.0f
#define HAULER_LOAD_TIME            2.0f
#define STATION_PRODUCTION_RATE     0.3f
#define UPGRADE_BASE_PRODUCT        8.0f

/* ------------------------------------------------------------------ */
/* Enums                                                              */
/* ------------------------------------------------------------------ */

enum {
    STATION_SERVICE_ORE_BUYER       = 1 << 0,
    STATION_SERVICE_REPAIR          = 1 << 1,
    STATION_SERVICE_UPGRADE_LASER   = 1 << 2,
    STATION_SERVICE_UPGRADE_HOLD    = 1 << 3,
    STATION_SERVICE_UPGRADE_TRACTOR = 1 << 4,
};

typedef enum {
    COMMODITY_FERRITE_ORE,
    COMMODITY_CUPRITE_ORE,
    COMMODITY_CRYSTAL_ORE,
    COMMODITY_RAW_ORE_COUNT,
    COMMODITY_FRAME_INGOT = COMMODITY_RAW_ORE_COUNT,
    COMMODITY_CONDUCTOR_INGOT,
    COMMODITY_LENS_INGOT,
    COMMODITY_COUNT,
} commodity_t;

enum {
    INGOT_COUNT = COMMODITY_COUNT - COMMODITY_RAW_ORE_COUNT,
};

#define INGOT_IDX(c) ((c) - COMMODITY_RAW_ORE_COUNT)

typedef enum {
    HULL_CLASS_MINER,
    HULL_CLASS_HAULER,
    HULL_CLASS_NPC_MINER,
    HULL_CLASS_COUNT,
} hull_class_t;

typedef enum {
    STATION_ROLE_REFINERY,
    STATION_ROLE_YARD,
    STATION_ROLE_BEAMWORKS,
} station_role_t;

typedef enum {
    PRODUCT_FRAME,
    PRODUCT_LASER_MODULE,
    PRODUCT_TRACTOR_MODULE,
    PRODUCT_COUNT,
} product_t;

typedef enum {
    ASTEROID_TIER_XL,
    ASTEROID_TIER_L,
    ASTEROID_TIER_M,
    ASTEROID_TIER_S,
    ASTEROID_TIER_COUNT,
} asteroid_tier_t;

typedef enum {
    SHIP_UPGRADE_MINING,
    SHIP_UPGRADE_HOLD,
    SHIP_UPGRADE_TRACTOR,
    SHIP_UPGRADE_COUNT,
} ship_upgrade_t;

typedef enum {
    NPC_ROLE_MINER,
    NPC_ROLE_HAULER,
} npc_role_t;

typedef enum {
    NPC_STATE_IDLE,
    NPC_STATE_TRAVEL_TO_ASTEROID,
    NPC_STATE_MINING,
    NPC_STATE_RETURN_TO_STATION,
    NPC_STATE_DOCKED,
    NPC_STATE_TRAVEL_TO_DEST,
    NPC_STATE_UNLOADING,
} npc_state_t;

/* ------------------------------------------------------------------ */
/* Structs                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    float x;
    float y;
} vec2;

typedef struct {
    const char *name;
    float max_hull;
    float accel;
    float turn_speed;
    float drag;
    float ore_capacity;
    float ingot_capacity;
    float mining_rate;
    float tractor_range;
    float ship_radius;
    float render_scale;
} hull_def_t;

typedef struct {
    vec2 pos;
    vec2 vel;
    float angle;
    float hull;
    float cargo[COMMODITY_COUNT];
    float credits;
    hull_class_t hull_class;
    int mining_level;
    int hold_level;
    int tractor_level;
} ship_t;

typedef struct {
    char name[32];
    station_role_t role;
    vec2 pos;
    float radius;
    float dock_radius;
    float buy_price[COMMODITY_COUNT];
    float inventory[COMMODITY_COUNT];
    float ore_buffer[COMMODITY_RAW_ORE_COUNT];
    float ingot_buffer[INGOT_COUNT];
    float product_stock[PRODUCT_COUNT];
    uint32_t services;
} station_t;

typedef struct {
    bool active;
    bool fracture_child;
    asteroid_tier_t tier;
    vec2 pos;
    vec2 vel;
    float radius;
    float hp;
    float max_hp;
    float ore;
    float max_ore;
    commodity_t commodity;
    float rotation;
    float spin;
    float seed;
    float age;
} asteroid_t;

typedef struct {
    bool active;
    npc_role_t role;
    hull_class_t hull_class;
    npc_state_t state;
    vec2 pos;
    vec2 vel;
    float angle;
    float cargo[COMMODITY_RAW_ORE_COUNT];
    float ingots[INGOT_COUNT];
    int target_asteroid;
    int home_station;
    int dest_station;
    float state_timer;
    bool thrusting;
} npc_ship_t;

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
    bool reset;
} input_intent_t;

/* ------------------------------------------------------------------ */
/* Server-specific types                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool connected;
    uint8_t id;
    void *conn;                     /* mg_connection pointer */
    ship_t ship;
    input_intent_t input;
    int current_station;
    int nearby_station;
    bool docked;
    bool in_dock_range;
    bool beam_active;
    bool beam_hit;
    int hover_asteroid;
    vec2 beam_start;
    vec2 beam_end;
    float cargo_sale_value;
    int nearby_fragments;
    int tractor_fragments;
} server_player_t;

typedef struct {
    station_t stations[MAX_STATIONS];
    asteroid_t asteroids[MAX_ASTEROIDS];
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
    server_player_t players[MAX_PLAYERS];
    uint32_t rng;
    float time;
    float field_spawn_timer;
} world_t;

/* ------------------------------------------------------------------ */
/* Hull definitions (declared in game_sim.c)                          */
/* ------------------------------------------------------------------ */

extern const hull_def_t HULL_DEFS[HULL_CLASS_COUNT];

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* World lifecycle */
void world_reset(world_t *w);
void world_sim_step(world_t *w, float dt);

/* Player lifecycle */
void player_init_ship(server_player_t *sp, world_t *w);

/* Math helpers (used by net_protocol.c too) */
vec2 v2(float x, float y);

#endif /* GAME_SIM_H */
