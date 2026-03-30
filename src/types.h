#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "math_util.h"

enum {
    KEY_COUNT = 512,
    MAX_ASTEROIDS = 48,
    MAX_STARS = 120,
    MAX_STATIONS = 3,
    MAX_NPC_SHIPS = 6,
    AUDIO_VOICE_COUNT = 24,
    AUDIO_MIX_FRAMES = 512,
};

enum {
    STATION_SERVICE_ORE_BUYER = 1 << 0,
    STATION_SERVICE_REPAIR = 1 << 1,
    STATION_SERVICE_UPGRADE_LASER = 1 << 2,
    STATION_SERVICE_UPGRADE_HOLD = 1 << 3,
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

typedef struct {
    const char* name;
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

extern const hull_def_t HULL_DEFS[HULL_CLASS_COUNT];

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
    vec2 pos;
    float depth;
    float size;
    float brightness;
} star_t;

#endif
