#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "math_util.h"
#include "mining.h"

/*
 * ⚠️  ENTITY POOL CAPS — read this before bumping any MAX_* constant.  ⚠️
 *
 * These caps are not arbitrary tuning numbers. They are pinned by the
 * v1 wire protocol, which budgets entity identity at ONE BYTE per type:
 *
 *   - asteroid id is uint8 in WORLD_ASTEROIDS, NPC_RECORD, etc.
 *   - station id is uint8 in STATION_IDENTITY / WORLD_STATIONS records
 *   - npc id is uint8 in WORLD_NPCS records
 *
 * Bumping any cap past these limits requires a wire protocol revision —
 * tracked as #285 (streaming entity pool + protocol v2). Anything that
 * pushes the world past these caps is structurally a slice of #285, not
 * a tuning change. File it against #285 instead of editing here.
 *
 * Do NOT raise MAX_NPC_SHIPS or MAX_SCAFFOLDS without a paired
 * wire-protocol bump and a deserializer change in src/net.c.
 * MAX_STATIONS lifted to 64 (#285 Phase 1). MAX_ASTEROIDS lifted
 * to 2048 (#285 Phase 3) with uint16 wire indices.
 */
enum {
    KEY_COUNT = 512,
    MAX_ASTEROIDS = 2048, /* uint16 wire index; lifted from 255 in #285 Phase 3 */
    MAX_STARS = 120,
    MAX_STATIONS = 64,   /* lifted from 8 in #285 Phase 1; uint8 wire index supports 255 */
    MAX_NPC_SHIPS = 16,  /* uint8 index — see banner above (#285 to lift) */
    MAX_SCAFFOLDS = 16,  /* uint8 index — see banner above (#285 to lift) */
    AUDIO_VOICE_COUNT = 24,
    AUDIO_MIX_FRAMES = 512,
};

enum {
    /* bit 0 was STATION_SERVICE_ORE_BUYER — removed in #259 */
    STATION_SERVICE_REPAIR = 1 << 1,
    STATION_SERVICE_UPGRADE_LASER = 1 << 2,
    STATION_SERVICE_UPGRADE_HOLD = 1 << 3,
    STATION_SERVICE_UPGRADE_TRACTOR = 1 << 4,
    /* bit 5 was STATION_SERVICE_BLUEPRINT — removed in #280 */
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
    COMMODITY_REPAIR_KIT,        /* 1 kit = 1 HP at a dock; produced by docks
                                   * from 1 FRAME + 1 LASER_MODULE → 100 kits.
                                   * The end-of-chain demand sink that closes
                                   * the ferrite + cuprite production loops. */
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
    float cargo_capacity;
    float ingot_capacity;
    float mining_rate;
    float tractor_range;
    float ship_radius;
    float render_scale;
} hull_def_t;

extern const hull_def_t HULL_DEFS[HULL_CLASS_COUNT];

/* RATi mining v2 — class authorization encoded in the leading char(s)
 * of base58(pubkey). Determines what hull class an ingot can mint.
 *   M / H / T / S / F / K = single-letter classes
 *   RATi (4-char prefix)  = brand fleet
 *   anything else         = anonymous (bulk material only)
 * Reserved letters R/A/T/i (RATi disambiguation) and digits/lowercase
 * fall into anonymous. */
typedef enum {
    INGOT_PREFIX_ANONYMOUS = 0,
    INGOT_PREFIX_M,
    INGOT_PREFIX_H,
    INGOT_PREFIX_T,
    INGOT_PREFIX_S,
    INGOT_PREFIX_F,
    INGOT_PREFIX_K,
    INGOT_PREFIX_RATI,
    INGOT_PREFIX_COMMISSIONED,  /* reserved for v1.5 station bounties */
    INGOT_PREFIX_COUNT
} ingot_prefix_t;

/* A named ingot is a uniquely-identified unit of refined ore. The
 * pubkey IS the future hull's name; the prefix decides which hull
 * class can be minted from it. Provenance fields let any client trace
 * the ingot's history through the chain log. */
typedef struct {
    uint8_t  pubkey[32];      /* identity, set at smelt */
    uint8_t  prefix_class;    /* ingot_prefix_t */
    uint8_t  metal;           /* commodity_t — FERRITE/CUPRITE/CRYSTAL_INGOT */
    uint8_t  _pad[2];
    uint64_t mined_block;     /* chain block id at mint */
    uint8_t  origin_station;  /* refinery that smelted it */
    uint8_t  _pad2[7];
} named_ingot_t;

#define STATION_NAMED_INGOTS_MAX 64
#define SHIP_HOLD_INGOTS_MAX     8
#define SHIP_MANIFEST_DEFAULT_CAP    32
#define STATION_MANIFEST_DEFAULT_CAP 256

typedef enum {
    CARGO_KIND_INGOT      = 0,
    CARGO_KIND_FRAME      = 1,
    CARGO_KIND_LASER      = 2,
    CARGO_KIND_TRACTOR    = 3,
    CARGO_KIND_REPAIR_KIT = 4,
    CARGO_KIND_COUNT
} cargo_kind_t;

typedef struct {
    uint8_t  kind;              /* cargo_kind_t */
    uint8_t  commodity;         /* commodity_t */
    uint8_t  grade;             /* mining_grade_t */
    uint8_t  _pad;              /* reserved, zero */
    uint16_t recipe_id;         /* recipe_id_t */
    uint16_t _pad2;             /* reserved, zero */
    uint8_t  pub[32];           /* content hash */
    uint8_t  parent_merkle[32]; /* sorted-input merkle root */
} cargo_unit_t;

typedef struct {
    uint16_t count;
    uint16_t cap;
    cargo_unit_t *units;
} manifest_t;

typedef enum {
    RECIPE_SMELT = 0,
    RECIPE_FRAME_BASIC,
    RECIPE_LASER_BASIC,
    RECIPE_TRACTOR_COIL,
    RECIPE_REPAIR_KIT_FAB,    /* 1 frame + 1 laser → 100 repair kits, at any dock */
    RECIPE_LEGACY_MIGRATE,
    RECIPE_COUNT
} recipe_id_t;

/* RECIPE_INPUT_MAX bumped from 2 → 3 so the dock repair-kit recipe
 * (frame + laser + tractor → 100 kits) can fit. All recipes still
 * declare their actual input_count; the array slot is just sized
 * to the largest recipe in the table. */
#define RECIPE_INPUT_MAX 3

typedef struct {
    recipe_id_t   id;
    const char   *name;
    cargo_kind_t  output_kind;
    commodity_t   output_commodity; /* COMMODITY_COUNT = caller supplies */
    uint8_t       input_count;
    commodity_t   input_commodities[RECIPE_INPUT_MAX];
} recipe_def_t;

typedef struct {
    vec2 pos;
    vec2 vel;
    float angle;
    float hull;
    float cargo[COMMODITY_COUNT];
    hull_class_t hull_class;
    int mining_level;
    int hold_level;
    int tractor_level;
    /* Towed physical fragments (indices into asteroid array, -1 = empty) */
    int16_t towed_fragments[10];  /* max 10 with upgrades: 2 + 4*2 */
    uint8_t towed_count;
    int16_t towed_scaffold;       /* scaffold index being towed, -1 = none */
    bool tractor_active;          /* true while R held — drives fragment collection */
    float comm_range;             /* hail ping range (H pings stations within this). 0 = use default. */
    /* Tech tree: bit per module type. Set when the player orders a
     * scaffold of that type. Drives the order menu unlock check. */
    uint32_t unlocked_modules;
    /* Run stats (reset on death/respawn) */
    float stat_ore_mined;
    float stat_credits_earned;
    float stat_credits_spent;
    int stat_asteroids_fractured;
    /* Named ingots in the player's hold — carried between stations
     * for sale or hull construction. Bulk anonymous ingots still ride
     * in cargo[] as fungible counts; this list holds identified ones. */
    named_ingot_t hold_ingots[SHIP_HOLD_INGOTS_MAX];
    int           hold_ingots_count;
    manifest_t    manifest; /* ship cargo manifest; stays empty until transfer migration lands */
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
    MODULE_HOPPER,            /* ore intake + beam anchor for furnaces */
    MODULE_FURNACE,           /* smelts ferrite ore */
    MODULE_FURNACE_CU,        /* smelts cuprite ore */
    MODULE_FURNACE_CR,        /* smelts crystal ore */
    MODULE_REPAIR_BAY,
    MODULE_SIGNAL_RELAY,
    MODULE_FRAME_PRESS,
    MODULE_LASER_FAB,
    MODULE_TRACTOR_FAB,
    MODULE_ORE_SILO,
    MODULE_SHIPYARD,
    MODULE_CARGO_BAY,         /* generic large storage */
    MODULE_COUNT
} module_type_t;

/* module_type_name moved to module_schema.h — reads from schema. */


/* Module build material/cost/fee lookups moved to module_schema.h
 * (included at the bottom of this file) to read from the schema table. */

static inline const char *commodity_short_label(commodity_t c) {
    switch (c) {
        case COMMODITY_FRAME:         return "frames";
        case COMMODITY_FERRITE_INGOT: return "fe ingots";
        case COMMODITY_CUPRITE_INGOT: return "cu ingots";
        case COMMODITY_CRYSTAL_INGOT: return "cr ingots";
        case COMMODITY_REPAIR_KIT:    return "repair kits";
        default:                      return "units";
    }
}

typedef struct {
    module_type_t type;
    uint8_t ring;           /* which ring tier (0xFF=core, 1=inner, 2=mid, 3=outer) */
    uint8_t slot;           /* position within ring (0..STATION_RING_SLOTS[ring]-1) */
    bool scaffold;          /* under construction */
    float build_progress;   /* 0.0 to 1.0 */
} station_module_t;

enum {
    MAX_MODULES_PER_STATION = 16,
    MAX_ARMS = 4,
    PLAYER_PLAN_TYPE_LIMIT = 2, /* max distinct planned module types per player */
};

typedef struct {
    uint32_t id;             /* stable ID, survives array slot changes (0 = unassigned) */
    char name[32];
    vec2 pos;
    float radius;
    float dock_radius;
    float signal_range;
    bool signal_connected;   /* true = can trace signal path to a root station */
    bool scaffold;           /* true = under construction, not yet active */
    bool planned;            /* true = design phase only, no physical presence */
    int8_t planned_owner;    /* player id who created the plan, -1 = system */
    float scaffold_progress; /* 0.0 to 1.0 */
    float base_price[COMMODITY_COUNT];
    float inventory[COMMODITY_COUNT]; /* unified storage for all commodities */
    uint32_t services;
    /* Module system */
    station_module_t modules[MAX_MODULES_PER_STATION];
    int module_count;
    /* Ring rotation — all rings share one speed, each has a fixed angular offset */
    int arm_count;                    /* number of active rings with rotation */
    float arm_rotation[MAX_ARMS];     /* per-ring rotation angle (radians) */
    float arm_speed[MAX_ARMS];        /* per-ring rotation speed (rad/s) — only [0] used */
    float ring_offset[MAX_ARMS];      /* fixed angular offset per ring (radians) */
    char hail_message[256];           /* AI-authored station message of the day */
    char station_slug[32];            /* URL slug for CDN assets (e.g. "prospect") */
    char currency_name[32];           /* station-local currency label, e.g. "helios credits".
                                       * Empty string → HUD falls back to "credits". */
    /* Economy ledger: per-player supply tracking for passive income */
    struct {
        uint8_t player_token[8];      /* session token of the supplier */
        float balance;                /* spendable station-local credits */
        float lifetime_supply;        /* total ore contributed */
    } ledger[16];
    int ledger_count;
    /* Shipyard: pending scaffold orders awaiting materials */
    struct {
        module_type_t type;
        int8_t owner;  /* player id who placed the order, -1 = NPC/anyone */
    } pending_scaffolds[4];
    int pending_scaffold_count;
    /* Placement plans: slots the player has reserved for a specific
     * module type. When a matching scaffold is towed near, the reticle
     * locks to the planned slot. Filled by planning-mode reticle. */
    struct {
        module_type_t type;
        uint8_t ring;
        uint8_t slot;
        int8_t owner; /* player id who planned it */
    } placement_plans[8];
    int placement_plan_count;
    /* Production layer v2: per-module input + output buffers.
     * Indexed parallel to modules[]. Producers fill output_buffer from
     * their inputs; the flow graph (step_module_flow) drains output_buffer
     * into downstream consumers' input_buffer. Storage modules use input
     * only (drains to downstream). Shipyards use input only (drains to
     * manufacture). Service modules leave both at 0. */
    float module_input[MAX_MODULES_PER_STATION];
    float module_output[MAX_MODULES_PER_STATION];
    /* Station credit pool: fixed money supply, no inflation.
     * Smelting pays from pool, player spending refills it. */
    float credit_pool;
    /* Named ingot stockpile (RATi v2). Refinery deposits here on smelt
     * when the winning pubkey carries a class prefix. Players buy from
     * this list at the MARKET tab; shipyards consume entries to mint
     * hulls bound to the ingot's pubkey identity. LRU evict on full. */
    named_ingot_t named_ingots[STATION_NAMED_INGOTS_MAX];
    int           named_ingots_count;
    bool          named_ingots_dirty;  /* server-only: drives wire push */
    manifest_t    manifest;            /* station cargo manifest; smelt/fab dual-write feeds it */
    /* Dock repair-kit fab cadence: server-only countdown. When it
     * reaches zero and the station has 1 frame + 1 laser + 1 tractor
     * in inventory, consume them, mint REPAIR_KIT_PER_BATCH kits, and
     * reset the timer to REPAIR_KIT_FAB_PERIOD. */
    float         repair_kit_fab_timer;
} station_t;

/* Station lifecycle helpers, module queries, and ring/geometry helpers
 * moved to shared/station_util.h (#273), included at the bottom of this
 * file so existing dependents continue to compile. */

/* ------------------------------------------------------------------ */
/* Scaffolds — physical construction objects                          */
/* ------------------------------------------------------------------ */

typedef enum {
    SCAFFOLD_NASCENT,   /* under construction at station center */
    SCAFFOLD_LOOSE,     /* floating after manufacture, ready to tow */
    SCAFFOLD_TOWING,    /* attached to player/NPC tractor beam */
    SCAFFOLD_SNAPPING,  /* station tendrils pulling scaffold into ring slot */
    SCAFFOLD_PLACED,    /* locked to ring slot, awaiting supply → becomes module */
} scaffold_state_t;

typedef struct {
    bool active;
    module_type_t module_type;  /* what module this scaffold becomes */
    scaffold_state_t state;
    int owner;                  /* player ID who purchased, -1 = NPC-produced */
    vec2 pos;
    vec2 vel;
    float radius;               /* collision radius (~30-40) */
    float rotation;             /* visual spin */
    float spin;                 /* rotation speed */
    float age;                  /* time since spawned */
    int placed_station;         /* station index when PLACED, -1 otherwise */
    int placed_ring;
    int placed_slot;
    int towed_by;               /* player index towing this, -1 = none */
    /* Nascent state: built at station center while NASCENT */
    int built_at_station;       /* station building this scaffold (-1 if not nascent) */
    float build_amount;         /* material accumulated, complete at module_build_cost() */
} scaffold_t;

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
    /* TODO: retire the int8 player-id pair below once every consumer
     * (wire sync, smelt-time credit split) reads from *_token instead.
     * The token form survives disconnect/reconnect; the int8 form is
     * wire-legacy and goes stale if a player reconnects into a new slot. */
    int8_t last_towed_by;      /* player ID who last towed this, -1 = none */
    int8_t last_fractured_by;  /* player ID who fractured the parent, -1 = none */
    float smelt_progress;      /* 0.0-1.0: how far through smelting (in furnace beam) */
    bool net_dirty;   /* needs network sync (spawn, fracture, HP change, death) */
    /* Fragment provenance: fracture_seed is fixed at birth. fragment_pub
     * and grade stay zero/common until the fracture claim window resolves,
     * then become immutable inputs to smelt + downstream crafting. */
    uint8_t last_towed_token[8];      /* session token of last towing player, zero = none */
    uint8_t last_fractured_token[8];  /* session token of fracturer, zero = none */
    uint8_t fracture_seed[32];
    uint8_t fragment_pub[32];
    uint8_t grade;             /* mining_grade_t, cached resolved grade */
} asteroid_t;

typedef enum {
    NPC_ROLE_MINER,
    NPC_ROLE_HAULER,
    /* NPC_ROLE_TOW: reserved for autonomous scaffold delivery (#277 step 6).
     * Picks up loose scaffolds near a shipyard and tows them to placement
     * targets. Not yet wired in step_npc_ships — currently never spawned.
     * The wire protocol packs role into 2 bits so adding a value here is
     * forward-compatible: clients that don't recognize the role render it
     * as a hauler. */
    NPC_ROLE_TOW,
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
    float cargo[COMMODITY_COUNT];
    int target_asteroid;
    int home_station;
    int dest_station;
    float state_timer;
    bool thrusting;
    float tint_r, tint_g, tint_b;  /* accumulated ore color (starts white) */
    int towed_fragment;             /* asteroid index being towed, -1 = none */
    int towed_scaffold;             /* scaffold index being towed (NPC_ROLE_TOW), -1 = none */
    /* Hull HP. Decrements on collision (asteroid / station / ship). When
     * the NPC docks at home, the dock auto-repairs by consuming repair
     * kits from station inventory (1 kit per HP). If hull <= 0 the
     * NPC despawns; sim_ai's spawn loop replaces it on the next tick.
     * Added in save v32 — earlier saves default to npc_max_hull on load. */
    float hull;
} npc_ship_t;

/* ------------------------------------------------------------------ */
/* character_t — controller layer (#294 Slice 1: types only)            */
/* ------------------------------------------------------------------ */
/*
 * A character_t is the AI brain or human pilot binding sitting on top
 * of a ship_t. The unification target (#294) is:
 *
 *   ship_t       — physics + cargo + manifest + upgrades + hull HP
 *   character_t  — kind, brain state, target, home/dest station,
 *                  state timer; indexes a ship by id
 *   world_t      — ships[]      unified pool (players + NPCs)
 *                  characters[] controller pool
 *
 * Slice 1 introduces the type and an empty `characters[]` pool on
 * world_t so later slices can populate them without a flag-day rename.
 * Nothing reads or writes the pool yet — npc_ship_t and
 * server_player_t still own physics + brain state today.
 */
typedef enum {
    CHARACTER_KIND_NONE = 0,
    CHARACTER_KIND_PLAYER,
    CHARACTER_KIND_NPC_MINER,
    CHARACTER_KIND_NPC_HAULER,
    CHARACTER_KIND_NPC_TOW,
} character_kind_t;

typedef struct {
    bool active;
    character_kind_t kind;
    int ship_idx;             /* index into world.ships[]; -1 = unbound */
    /* For NPC kinds: index into world.npc_ships[]. For PLAYER: index
     * into world.players[]. -1 = unbound. Distinct from ship_idx —
     * those address different pools. Used by character_for_npc_slot
     * et al. so the lookup is unambiguous regardless of how the
     * ships[] free-slot allocator handed out indices. */
    int npc_slot;
    /* Brain state — meaningful for NPC kinds. Players carry these in
     * server_player_t for now; converging is a later slice. */
    npc_state_t state;
    int target_asteroid;      /* -1 = none */
    int home_station;
    int dest_station;
    float state_timer;
    int towed_fragment;       /* -1 = none */
    int towed_scaffold;       /* -1 = none */
} character_t;

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

/* Callback for mixing external audio (music, video) into the output buffer.
 * Called once per mix chunk with the buffer after SFX voices are mixed.
 * Should ADD samples (not overwrite). frames = sample frames, channels = 1 or 2. */
typedef void (*audio_mix_callback_t)(float *buffer, int frames, int channels, void *user);

typedef struct {
    bool valid;
    uint32_t rng;
    int sample_rate;
    int channels;
    float mining_tick_cooldown;
    audio_voice_t voices[AUDIO_VOICE_COUNT];
    float mix_buffer[AUDIO_MIX_FRAMES * 2];
    /* External audio sources mixed after SFX voices */
    audio_mix_callback_t mix_callback;
    void *mix_callback_user;
} audio_state_t;

/* Station geometry constants
 * Ring 1: 3 modules (triangle),  Ring 2: 6 (hexagon),  Ring 3: 9 (nonagon)
 * Total capacity: 18 outer modules. */
static const float STATION_CORE_RADIUS    = 60.0f;
static const float STATION_RING_RADIUS[]  = { 0.0f, 180.0f, 340.0f, 520.0f };
static const int   STATION_RING_SLOTS[]   = { 0, 3, 6, 9 };
static const float STATION_RING_SPEED     = 0.04f;
enum { STATION_NUM_RINGS = 3 };

/* Station query/geometry helpers moved to shared/station_util.h (#273).
 * Economy and ship-upgrade constants moved to shared/economy_const.h. */

/* Signal channel — station broadcast log. Fixed-size ring buffer of
 * recent messages; stations post via REST, everyone reads. Sized to
 * 100 × ~440B ≈ 44KB which fits save + wire snapshot budgets. */
enum {
    SIGNAL_CHANNEL_TEXT_MAX   = 200,   /* chars incl. null terminator */
    SIGNAL_CHANNEL_AUDIO_MAX  = 256,   /* https URL — 256 keeps wire tight */
    SIGNAL_CHANNEL_CAPACITY   = 100,   /* ring slots; spec calls for 200,
                                          we ship 100 for V1 save footprint */
};

typedef struct {
    uint64_t id;                         /* monotonic, never resets (0 = empty slot) */
    uint32_t timestamp_ms;               /* server world_time * 1000 at post */
    int16_t  sender_station;             /* source station index, -1 = system */
    uint8_t  text_len;
    uint8_t  audio_len;
    char     text[SIGNAL_CHANNEL_TEXT_MAX];
    char     audio_url[SIGNAL_CHANNEL_AUDIO_MAX];
    /* Hash chain: entry_hash = sha256(prev_entry_hash || id || timestamp_ms ||
     * sender_station || text_len || text). Genesis block uses zeroes for the
     * previous hash. Server-side only — populated by signal_channel_post and
     * persisted to disk; not sent on the wire (clients trust the snapshot
     * they get and don't reverify in V1). */
    uint8_t  entry_hash[32];
} signal_channel_msg_t;

typedef struct {
    signal_channel_msg_t msgs[SIGNAL_CHANNEL_CAPACITY];
    int      head;      /* next write slot (0..CAPACITY-1) */
    int      count;     /* active slots (0..CAPACITY) */
    uint64_t next_id;   /* next id to assign (monotonic, survives wrap) */
} signal_channel_t;

typedef enum {
    /* TRACTOR: tow / deliver thing(s) to a destination.
     * - target_index >= 0  → specific entity (scaffold, fragment) to destination
     * - target_index == -1 → quota of `commodity` to station_index
     * Replaces the old SUPPLY (deliver N units of X). */
    CONTRACT_TRACTOR = 0,
    /* FRACTURE: laser-break thing(s) into fragments.
     * - target_index >= 0  → specific asteroid to destroy
     * - target_index == -1 → quota of asteroid type (mining contract)
     * Replaces DESTROY and absorbs SCAN. */
    CONTRACT_FRACTURE = 1,
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
    SIM_EVENT_OUTPOST_ACTIVATED,
    SIM_EVENT_NPC_SPAWNED,
    SIM_EVENT_SIGNAL_LOST,
    SIM_EVENT_HAIL_RESPONSE,
    SIM_EVENT_MODULE_ACTIVATED,
    SIM_EVENT_STATION_CONNECTED,
    SIM_EVENT_CONTRACT_COMPLETE,
    SIM_EVENT_DEATH,
    SIM_EVENT_SCAFFOLD_READY,
    SIM_EVENT_ORDER_REJECTED,
    SIM_EVENT_NPC_KILL,
    SIM_EVENT_COUNT,        /* sentinel — keep last; sized for dispatch tables */
} sim_event_type_t;

/* What killed a ship. Stable wire values — keep additions append-only.
 * Used both for player death cinematic copy ("killed by KRX-472 — thrown
 * rock") and for the NPC kill-feed when a player kills an NPC. */
typedef enum {
    DEATH_CAUSE_UNKNOWN     = 0,  /* env / unknown */
    DEATH_CAUSE_RAM         = 1,  /* player-vs-player ramming */
    DEATH_CAUSE_THROWN_ROCK = 2,  /* asteroid attributed via last_towed_token */
    DEATH_CAUSE_ASTEROID    = 3,  /* unattributed asteroid collision */
    DEATH_CAUSE_STATION     = 4,  /* corridor / module crush */
    DEATH_CAUSE_SELF        = 5,  /* X-key reset / self-destruct */
} death_cause_t;

typedef struct {
    sim_event_type_t type;
    int player_id;
    union {
        struct { asteroid_tier_t tier; int asteroid_id; } fracture;
        struct { float ore; int fragments; } pickup;
        struct { ship_upgrade_t upgrade; } upgrade;
        struct { float amount; } damage;
        /* SIM_EVENT_SELL: populated when a fragment is smelted. grade
         * is mining_grade_t; base_cr is ore * station_buy_price;
         * bonus_cr is the extra credits the multiplier added on top.
         * by_contract = true when an active CONTRACT_TRACTOR at this
         * station raised the price — client uses this to color the
         * floating "+$N" popup yellow instead of grade-tinted. */
        struct { int station; uint8_t grade; int base_cr; int bonus_cr;
                 uint8_t by_contract; } sell;
        struct { int slot; } outpost_placed;
        struct { int station; float credits; int contract_index; } hail_response;
        struct { int slot; } outpost_activated;
        struct { int station; int module_idx; int module_type; } module_activated;
        struct { int slot; npc_role_t role; int home_station; } npc_spawned;
        struct { int connected_count; } station_connected;
        struct { contract_action_t action; } contract_complete;
        struct {
            float ore_mined;
            float credits_earned;
            float credits_spent;
            int asteroids_fractured;
            float pos_x, pos_y;     /* where the ship died (pre-respawn) */
            float vel_x, vel_y;     /* velocity at moment of death */
            float angle;            /* hull orientation at moment of death */
            uint8_t killer_token[8]; /* zero = no attributed killer */
            uint8_t cause;          /* death_cause_t */
        } death;
        /* SIM_EVENT_NPC_KILL: a player killed an NPC by collision. The
         * NPC slot is going to despawn next tick; clients should surface
         * a kill-feed line. killer_token attributes via the asteroid's
         * last_towed_token (i.e. the player who threw the rock that hit
         * the NPC, or the player whose ship rammed it). */
        struct {
            uint8_t killer_token[8];
            uint8_t cause;          /* death_cause_t */
            uint8_t npc_role;       /* npc_role_t — for kill-feed copy */
            uint8_t _pad;
        } npc_kill;
        struct { int station; int module_type; } scaffold_ready;
        /* SIM_EVENT_ORDER_REJECTED: reason code lets the client surface
         * a useful notice ("out of signal range", "no slot here", etc.)
         * instead of a generic "rejected." Numbers here are stable
         * across builds — keep additions append-only. */
        struct { uint8_t reason; } order_rejected;
    };
} sim_event_t;

/* Reason codes for SIM_EVENT_ORDER_REJECTED. Stable wire values. */
enum {
    ORDER_REJECT_GENERIC = 0,
    ORDER_REJECT_SCAFFOLD_PLACEMENT_NO_SIGNAL = 1, /* outside signal coverage */
    ORDER_REJECT_SCAFFOLD_PLACEMENT_TOO_CLOSE = 2, /* inside another station's bubble or overlap */
    ORDER_REJECT_SCAFFOLD_PLACEMENT_NEEDS_RELAY = 3, /* tried to place a non-relay scaffold without a nearby outpost */
    ORDER_REJECT_SCAFFOLD_PLACEMENT_NO_SLOT = 4,    /* station-slot table full */
    ORDER_REJECT_SHIPYARD_NOT_SOLD = 5,             /* this shipyard doesn't sell that scaffold type */
    ORDER_REJECT_SHIPYARD_QUEUE_FULL = 6,           /* pending queue full */
    ORDER_REJECT_SHIPYARD_LOCKED = 7,               /* tech tree gate */
    ORDER_REJECT_SHIPYARD_NO_FUNDS = 8,             /* ledger spend failed */
    ORDER_REJECT_SELL_NOT_ACCEPTED = 9,             /* this station has no consumer for the picked commodity */
};

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
    /* Minimum grade accepted for fulfillment. MINING_GRADE_COMMON = any.
     * Rare/RATi/commissioned contracts demand matching or better quality
     * and pay correspondingly (via contract_price × multiplier). Older
     * saves default to COMMON on load because the field is zero-init. */
    uint8_t required_grade;
    float quantity_needed;  /* amount (SUPPLY) or radius (SCAN) */
    float base_price;
    float age;
    vec2 target_pos;        /* world position (DESTROY/SCAN target) */
    int target_index;       /* asteroid slot (DESTROY) or -1 */
    int8_t claimed_by;      /* player/NPC id, -1 = open */
} contract_t;

/* Station query/geometry helpers — must come after station_t */
#include "station_util.h"

/* Unified station collision/render geometry — must come after all station types */
#include "station_geom.h"

/* Module schema table — must come after all module/commodity types */
#include "module_schema.h"

/* Economy / ship-upgrade tuning constants */
#include "economy_const.h"

#endif
