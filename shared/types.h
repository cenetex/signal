#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "math_util.h"

enum {
    KEY_COUNT = 512,
    MAX_ASTEROIDS = 255, /* limited by uint8 index in network protocol */
    MAX_STARS = 120,
    MAX_STATIONS = 8,
    MAX_NPC_SHIPS = 16,
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
    COMMODITY_FERRITE_INGOT = COMMODITY_RAW_ORE_COUNT,
    COMMODITY_CUPRITE_INGOT,
    COMMODITY_CRYSTAL_INGOT,
    COMMODITY_FRAME,
    COMMODITY_LASER_MODULE,
    COMMODITY_TRACTOR_MODULE,
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
    bool has_scaffold_kit;
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
    MODULE_FURNACE,         /* smelts ferrite ore only */
    MODULE_FURNACE_CU,      /* smelts cuprite ore */
    MODULE_FURNACE_CR,      /* smelts crystal ore */
    MODULE_INGOT_SELLER,
    MODULE_REPAIR_BAY,
    MODULE_SIGNAL_RELAY,
    MODULE_FRAME_PRESS,
    MODULE_LASER_FAB,
    MODULE_TRACTOR_FAB,
    MODULE_CONTRACT_BOARD,
    MODULE_ORE_SILO,
    MODULE_BLUEPRINT_DESK,
    MODULE_RING,            /* physical ring truss structure */
    MODULE_COUNT
} module_type_t;

typedef struct {
    module_type_t type;
    uint8_t arm;            /* which radial arm (0, 1, 2...) */
    uint8_t chain_pos;      /* position along arm chain (0 = nearest core) */
    bool scaffold;          /* under construction */
    float build_progress;   /* 0.0 to 1.0 */
} station_module_t;

enum {
    MAX_MODULES_PER_STATION = 16,
    MAX_ARMS = 4,
    MAX_RING_COUNT = 3,     /* legacy compat — used by save format */
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
    float base_price[COMMODITY_COUNT];
    float inventory[COMMODITY_COUNT]; /* unified storage for all commodities */
    uint32_t services;
    /* Module system */
    station_module_t modules[MAX_MODULES_PER_STATION];
    int module_count;
    /* Arm rotation */
    int arm_count;
    float arm_rotation[MAX_ARMS];
    float arm_speed[MAX_ARMS];
    /* Legacy compat (save format) */
    float ring_rotation[MAX_RING_COUNT];
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
    bool net_dirty;   /* needs network sync (spawn, fracture, HP change, death) */
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
static const float HAULER_RESERVE = 8.0f;  /* keep 20% stock for player purchases */

/* Module helpers */
static inline bool station_has_module(const station_t *st, module_type_t type) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].type == type && !st->modules[i].scaffold) return true;
    return false;
}

/* Returns true if the station consumes this commodity as production input. */
static inline bool station_consumes(const station_t *st, commodity_t c) {
    switch (c) {
        case COMMODITY_FERRITE_ORE:   return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE:   return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE:   return station_has_module(st, MODULE_FURNACE_CR);
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_CUPRITE_INGOT: return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_CRYSTAL_INGOT: return station_has_module(st, MODULE_TRACTOR_FAB);
        default: return false;
    }
}

/* Returns true if the station produces this commodity (has the right module). */
static inline bool station_produces(const station_t *st, commodity_t c) {
    switch (c) {
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_INGOT: return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_INGOT: return station_has_module(st, MODULE_FURNACE_CR);
        case COMMODITY_FRAME:         return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_LASER_MODULE:  return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_TRACTOR_MODULE:return station_has_module(st, MODULE_TRACTOR_FAB);
        default: return false;
    }
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
        MODULE_FURNACE, MODULE_FURNACE_CU, MODULE_FURNACE_CR,
        MODULE_FRAME_PRESS, MODULE_LASER_FAB,
        MODULE_TRACTOR_FAB, MODULE_SIGNAL_RELAY, MODULE_ORE_BUYER,
    };
    for (int p = 0; p < (int)(sizeof(priority) / sizeof(priority[0])); p++) {
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].type == priority[p]) return priority[p];
        }
    }
    return MODULE_DOCK;
}

/* Primary trade slot: the one commodity this station buys from players.
 * Derived from the dominant production module. Returns -1 if none. */
static inline commodity_t station_primary_buy(const station_t *st) {
    module_type_t dom = station_dominant_module(st);
    switch (dom) {
        case MODULE_FURNACE:     return COMMODITY_FERRITE_ORE;
        case MODULE_FURNACE_CU:  return COMMODITY_CUPRITE_ORE;
        case MODULE_FURNACE_CR:  return COMMODITY_CRYSTAL_ORE;
        case MODULE_FRAME_PRESS: return COMMODITY_FERRITE_INGOT;
        case MODULE_LASER_FAB:   return COMMODITY_CUPRITE_INGOT;
        case MODULE_TRACTOR_FAB: return COMMODITY_CRYSTAL_INGOT;
        default: break;
    }
    if (station_has_module(st, MODULE_ORE_BUYER)) return COMMODITY_FERRITE_ORE;
    return (commodity_t)-1;
}

/* Primary trade slot: the one commodity this station sells to players.
 * Derived from the dominant production module. Returns -1 if none. */
static inline commodity_t station_primary_sell(const station_t *st) {
    module_type_t dom = station_dominant_module(st);
    switch (dom) {
        case MODULE_FURNACE:     return COMMODITY_FERRITE_INGOT;
        case MODULE_FURNACE_CU:  return COMMODITY_CUPRITE_INGOT;
        case MODULE_FURNACE_CR:  return COMMODITY_CRYSTAL_INGOT;
        case MODULE_FRAME_PRESS: return COMMODITY_FRAME;
        case MODULE_LASER_FAB:   return COMMODITY_LASER_MODULE;
        case MODULE_TRACTOR_FAB: return COMMODITY_TRACTOR_MODULE;
        default: break;
    }
    return (commodity_t)-1;
}

/* Station geometry constants */
static const float STATION_CORE_RADIUS   = 60.0f;
static const float STATION_ARM_OFFSET    = 80.0f;   /* radial distance: core edge to first module */
static const float STATION_MODULE_SPACING = 50.0f;  /* lateral spacing between chained modules */
static const float STATION_DEFAULT_ARM_SPEED = 0.04f; /* rad/s */

/* Arm base angle: arms are evenly spaced around the circle.
 * First arm starts at 0 deg (right), others distribute evenly. */
static inline float arm_base_angle(int arm, int arm_count) {
    if (arm_count <= 0) return 0.0f;
    return TWO_PI_F * (float)arm / (float)arm_count;
}

/* World-space position of a module on an arm chain. */
static inline vec2 module_world_pos_arm(const station_t *st, int arm, int chain_pos) {
    if (arm < 0 || arm >= st->arm_count) return st->pos;
    float base = arm_base_angle(arm, st->arm_count) + st->arm_rotation[arm];
    vec2 radial = v2(cosf(base), sinf(base));
    vec2 lateral = v2(-radial.y, radial.x); /* perpendicular, CCW */
    /* Radial: core edge outward */
    vec2 pos = v2_add(st->pos, v2_scale(radial, STATION_CORE_RADIUS + STATION_ARM_OFFSET));
    /* Lateral: chain extends sideways, centered on chain_pos 0 */
    float lateral_off = (float)chain_pos * STATION_MODULE_SPACING;
    pos = v2_add(pos, v2_scale(lateral, lateral_off));
    return pos;
}

/* Count modules on a given arm. */
static inline int arm_module_count(const station_t *st, int arm) {
    int count = 0;
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].arm == arm) count++;
    return count;
}

/* Legacy shims for callers not yet migrated */
static const int   RING_PORT_COUNT[] = { 0, 5, 8 };
static const float RING_RADIUS[]     = { 60.0f, 200.0f, 320.0f };
static const float RING_SPEED[]      = { 0.0f, 0.06f, 0.03f };
static const float RING_GAP_CENTER   = 4.712389f;
static const float RING_GAP_WIDTH    = 0.698132f;

static inline bool station_has_ring(const station_t *st, int ring) {
    (void)st; (void)ring;
    return ring <= 0;
}
static inline float ring_port_angle(int ring, int slot) {
    (void)ring; (void)slot;
    return 0.0f;
}
static inline int station_ring_free_slot(const station_t *st, int ring, int port_count) {
    (void)ring; (void)port_count;
    return st->module_count;
}

/* Outpost construction constants (client-shared) */
static const float SCAFFOLD_MATERIAL_NEEDED = 60.0f;   /* total frames needed */

/* Ship upgrade constants (shared between client and server) */
static const float SHIP_HOLD_UPGRADE_STEP = 24.0f;
static const float SHIP_MINING_UPGRADE_STEP = 7.0f;
static const float SHIP_TRACTOR_UPGRADE_STEP = 24.0f;
static const float SHIP_BASE_COLLECT_RADIUS = 30.0f;
static const float SHIP_COLLECT_UPGRADE_STEP = 5.0f;
static const float UPGRADE_BASE_PRODUCT = 8.0f;
static const int SHIP_UPGRADE_MAX_LEVEL = 4;

typedef enum {
    CONTRACT_SUPPLY,
    CONTRACT_DESTROY,
    CONTRACT_SCAN,
} contract_action_t;

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
    SIM_EVENT_CONTRACT_COMPLETE,
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
        struct { contract_action_t action; } contract_complete;
    };
} sim_event_t;

typedef struct {
    sim_event_t events[SIM_MAX_EVENTS];
    int count;
} sim_events_t;

enum { MAX_CONTRACTS = 24 };

typedef struct {
    bool active;
    contract_action_t action;
    uint8_t station_index;  /* destination (SUPPLY) or issuer (DESTROY/SCAN) */
    commodity_t commodity;  /* what to supply (SUPPLY only) */
    float quantity_needed;  /* amount (SUPPLY) or radius (SCAN) */
    float base_price;
    float age;
    vec2 target_pos;        /* world position (DESTROY/SCAN target) */
    int target_index;       /* asteroid slot (DESTROY) or -1 */
    int8_t claimed_by;      /* player/NPC id, -1 = open */
} contract_t;

#endif
