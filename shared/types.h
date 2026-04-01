#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "math_util.h"

enum {
    KEY_COUNT = 512,
    MAX_ASTEROIDS = 512,
    MAX_STARS = 120,
    MAX_STATIONS = 8,
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
    STATION_SERVICE_BLUEPRINT = 1 << 5,
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
    PRODUCT_FRAME,
    PRODUCT_LASER_MODULE,
    PRODUCT_TRACTOR_MODULE,
    PRODUCT_COUNT,
} product_t;

/* ------------------------------------------------------------------ */
/* Station modules                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    MODULE_DOCK,
    MODULE_ORE_BUYER,
    MODULE_FURNACE,
    MODULE_INGOT_SELLER,
    MODULE_REPAIR_BAY,
    MODULE_SIGNAL_RELAY,
    MODULE_FRAME_PRESS,
    MODULE_LASER_FAB,
    MODULE_TRACTOR_FAB,
    MODULE_CONTRACT_BOARD,
    MODULE_ORE_SILO,
    MODULE_BLUEPRINT_DESK,
    MODULE_COUNT
} module_type_t;

typedef struct {
    module_type_t type;
    bool scaffold;          /* under construction */
    float build_progress;   /* 0.0 to 1.0 */
} station_module_t;

enum {
    MAX_MODULES_PER_STATION = 16,
};

typedef struct {
    char name[32];
    vec2 pos;
    float radius;
    float dock_radius;
    float signal_range;
    bool signal_connected;   /* true = can trace signal path to a root station */
    bool scaffold;           /* true = under construction, not yet active */
    float scaffold_progress; /* 0.0 to 1.0 */
    float buy_price[COMMODITY_COUNT];
    float inventory[COMMODITY_COUNT];
    float ore_buffer[COMMODITY_RAW_ORE_COUNT];
    float ingot_buffer[INGOT_COUNT];
    float product_stock[PRODUCT_COUNT];
    uint32_t services;
    /* Module system */
    station_module_t modules[MAX_MODULES_PER_STATION];
    int module_count;
} station_t;

/* ------------------------------------------------------------------ */
/* Station lifecycle helpers                                           */
/* ------------------------------------------------------------------ */

/* A station slot is in use if it has signal range, is under construction,
 * or has a dock radius.  Empty/zeroed slots return false. */
static inline bool station_exists(const station_t *st) {
    return st->signal_range > 0.0f || st->scaffold || st->dock_radius > 0.0f;
}

/* A station is active (fully built and operational). */
static inline bool station_is_active(const station_t *st) {
    return st->signal_range > 0.0f && !st->scaffold;
}

/* Should this station provide a dock ring? */
static inline bool station_provides_docking(const station_t *st) {
    return st->dock_radius > 0.0f;
}

/* Should this station contribute to signal coverage? */
static inline bool station_provides_signal(const station_t *st) {
    return st->signal_range > 0.0f && st->signal_connected;
}

/* Should this station participate in collision? */
static inline bool station_collides(const station_t *st) {
    return st->radius > 0.0f;
}

typedef enum {
    ASTEROID_TIER_XXL,
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
    float tint_r, tint_g, tint_b;  /* accumulated ore color (starts white) */
} npc_ship_t;

typedef struct {
    vec2 pos;
    float depth;
    float size;
    float brightness;
} star_t;

typedef enum {
    AUDIO_WAVE_SINE,
    AUDIO_WAVE_TRIANGLE,
    AUDIO_WAVE_SQUARE,
    AUDIO_WAVE_NOISE,
} audio_wave_t;

typedef struct {
    bool active;
    audio_wave_t wave;
    float phase;
    float frequency;
    float sweep;
    float gain;
    float pan;
    float pan_l;
    float pan_r;
    float duration;
    float age;
    float noise_mix;
} audio_voice_t;

typedef struct {
    bool valid;
    uint32_t rng;
    int sample_rate;
    int channels;
    float mining_tick_cooldown;
    audio_voice_t voices[AUDIO_VOICE_COUNT];
    float mix_buffer[AUDIO_MIX_FRAMES * 2];
} audio_state_t;

/* Economy constants (shared between client and server) */
static const float REFINERY_HOPPER_CAPACITY = 500.0f;
static const float REFINERY_BASE_SMELT_RATE = 2.0f;
static const int REFINERY_MAX_FURNACES = 3;
static const float STATION_PRODUCTION_RATE = 0.3f;
static const float STATION_REPAIR_COST_PER_HULL = 2.0f;
static const float MAX_PRODUCT_STOCK = 40.0f;

/* Module helpers */
static inline bool station_has_module(const station_t *st, module_type_t type) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].type == type) return true;
    return false;
}

static inline void rebuild_station_services(station_t *st) {
    st->services = 0;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].scaffold) continue;
        switch (st->modules[i].type) {
            case MODULE_ORE_BUYER:      st->services |= STATION_SERVICE_ORE_BUYER; break;
            case MODULE_REPAIR_BAY:     st->services |= STATION_SERVICE_REPAIR; break;
            case MODULE_LASER_FAB:      st->services |= STATION_SERVICE_UPGRADE_LASER; break;
            case MODULE_TRACTOR_FAB:    st->services |= STATION_SERVICE_UPGRADE_TRACTOR; break;
            case MODULE_FRAME_PRESS:    st->services |= STATION_SERVICE_UPGRADE_HOLD; break;
            case MODULE_BLUEPRINT_DESK: st->services |= STATION_SERVICE_BLUEPRINT; break;
            default: break;
        }
    }
}

/* Return the dominant module type for display purposes (name, color, visual).
 * Priority: FURNACE > FRAME_PRESS > LASER_FAB > TRACTOR_FAB > SIGNAL_RELAY > others.
 * Returns MODULE_DOCK as fallback. */
static inline module_type_t station_dominant_module(const station_t *st) {
    static const module_type_t priority[] = {
        MODULE_FURNACE, MODULE_FRAME_PRESS, MODULE_LASER_FAB,
        MODULE_TRACTOR_FAB, MODULE_SIGNAL_RELAY, MODULE_ORE_BUYER,
    };
    for (int p = 0; p < (int)(sizeof(priority) / sizeof(priority[0])); p++) {
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].type == priority[p]) return priority[p];
        }
    }
    return MODULE_DOCK;
}

/* Outpost construction constants (client-shared) */
static const float SCAFFOLD_MATERIAL_NEEDED = 100.0f;  /* total units of frame ingots */

/* Ship upgrade constants (shared between client and server) */
static const float SHIP_HOLD_UPGRADE_STEP = 24.0f;
static const float SHIP_MINING_UPGRADE_STEP = 7.0f;
static const float SHIP_TRACTOR_UPGRADE_STEP = 24.0f;
static const float SHIP_BASE_COLLECT_RADIUS = 30.0f;
static const float SHIP_COLLECT_UPGRADE_STEP = 5.0f;
static const float UPGRADE_BASE_PRODUCT = 16.0f;
static const int SHIP_UPGRADE_MAX_LEVEL = 4;

enum { SIM_MAX_EVENTS = 64 };

typedef enum {
    SIM_EVENT_FRACTURE,
    SIM_EVENT_PICKUP,
    SIM_EVENT_MINING_TICK,
    SIM_EVENT_DOCK,
    SIM_EVENT_LAUNCH,
    SIM_EVENT_SELL,
    SIM_EVENT_REPAIR,
    SIM_EVENT_UPGRADE,
    SIM_EVENT_DAMAGE,
    SIM_EVENT_OUTPOST_PLACED,
} sim_event_type_t;

typedef struct {
    sim_event_type_t type;
    int player_id;
    union {
        struct { asteroid_tier_t tier; } fracture;
        struct { float ore; int fragments; } pickup;
        struct { ship_upgrade_t upgrade; } upgrade;
        struct { float amount; } damage;
        struct { int slot; } outpost_placed;
    };
} sim_event_t;

typedef struct {
    sim_event_t events[SIM_MAX_EVENTS];
    int count;
} sim_events_t;

enum { MAX_CONTRACTS = 16 };

typedef struct {
    bool active;
    uint8_t station_index;
    commodity_t commodity;
    float quantity_needed;
    float base_price;
    float age;
} contract_t;

#endif
