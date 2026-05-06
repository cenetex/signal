#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>   /* offsetof — Layer B of #479 station_secret guard */
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

#define SHIP_MANIFEST_DEFAULT_CAP    32
#define STATION_MANIFEST_DEFAULT_CAP 256

typedef enum {
    CARGO_KIND_INGOT      = 0,
    CARGO_KIND_FRAME      = 1,
    CARGO_KIND_LASER      = 2,
    CARGO_KIND_TRACTOR    = 3,
    CARGO_KIND_REPAIR_KIT = 4,
    /* Raw mined fragment, pooled by quantity. Allocated by the kind
     * but no production path uses it yet — that lands in the next
     * slice (per-ore migration). Reserving the enum value here keeps
     * the wire and save formats stable through the slice. */
    CARGO_KIND_ORE        = 5,
    CARGO_KIND_COUNT
} cargo_kind_t;

/* Unified cargo identity. Carries the named-ingot fields (prefix_class,
 * mined_block, origin_station) so a single store covers raw ingots,
 * fabricated frames/lasers/tractors, and repair kits. The legacy
 * named_ingot_t / station.named_ingots[] / ship.hold_ingots[] dual store
 * was collapsed into the manifest; cargo_unit_t.pub is now the single
 * identity for both ingots and finished goods.
 *
 * For non-ingot kinds prefix_class is INGOT_PREFIX_ANONYMOUS, mined_block
 * is 0, and origin_station is the station that crafted the unit.
 *
 * `quantity` is the count of items in this crate. Finished goods (ingots,
 * frames, lasers, tractors, kits) always have quantity == 1: each unit is
 * individually addressable and tracked through the chain log. CARGO_KIND_ORE
 * (raw mined fragments) supports quantity > 1 so an ore crate pools many
 * fragments under one provenance signature without exploding the manifest.
 * Cap is u8 (255) — beyond that the caller pushes a new crate. */
typedef struct {
    uint8_t  kind;              /* cargo_kind_t */
    uint8_t  commodity;         /* commodity_t */
    uint8_t  grade;             /* mining_grade_t */
    uint8_t  prefix_class;      /* ingot_prefix_t (anonymous for non-ingot kinds) */
    uint16_t recipe_id;         /* recipe_id_t */
    uint8_t  origin_station;    /* refinery/fabricator that produced it */
    uint8_t  quantity;          /* items in this crate. 1 for finished goods,
                                 * 1..255 for ore. Was _pad pre-v45; legacy
                                 * saves with quantity == 0 migrate to 1 on
                                 * load. */
    uint64_t mined_block;       /* chain block id at mint (0 for non-ingot) */
    uint8_t  pub[32];           /* content hash */
    uint8_t  parent_merkle[32]; /* sorted-input merkle root */
} cargo_unit_t;                 /* 80 bytes */

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
    RECIPE_REPAIR_KIT_FAB,    /* 1 frame + 1 laser + 1 tractor -> 100 repair kits at shipyards */
    RECIPE_LEGACY_MIGRATE,
    RECIPE_COUNT
} recipe_id_t;

/* RECIPE_INPUT_MAX bumped from 2 -> 3 so the shipyard repair-kit recipe
 * (frame + laser + tractor -> 100 kits) can fit. All recipes still
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
    float comm_range;             /* local hail scan visual/tag range. 0 = use default. */
    /* Tech tree: bit per module type. Set when the player orders a
     * scaffold of that type. Drives the order menu unlock check. */
    uint32_t unlocked_modules;
    /* Run stats (reset on death/respawn) */
    float stat_ore_mined;
    float stat_credits_earned;
    float stat_credits_spent;
    int stat_asteroids_fractured;
    /* Ship cargo manifest — single source of identity for held units.
     * Named ingots and bulk finished goods both live here; the legacy
     * hold_ingots[] / named_ingot_t dual store was collapsed in the
     * "unify ingot identity" PR. */
    manifest_t    manifest;
    /* Layer D of #479 — portable cargo receipts.
     *
     * Parallel to `manifest`: receipts.chains[i] is the per-cargo-unit
     * receipt chain attached to manifest.units[i]. Mutated in lockstep
     * with the manifest by every BUY / SELL / DELIVER / TRANSFER path —
     * receipts.count must equal manifest.count after every consistent
     * op. Bootstrapped alongside the manifest (see ship_manifest_bootstrap).
     *
     * Stored as a void pointer to keep types.h independent of
     * cargo_receipt.h (avoids a header cycle); shared/cargo_receipt.h
     * defines the ship_receipts_t shape and shared/manifest.c casts
     * through it. The on-disk save format (v42+) round-trips through
     * the cargo_receipt_t wire layout. */
    void          *receipts_opaque; /* ship_receipts_t* — see cargo_receipt.h */
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
    MODULE_DOCK = 0,
    MODULE_HOPPER = 1,        /* ore intake + storage + smelt-unlock for furnaces.
                               * Absorbs the legacy ORE_SILO and CARGO_BAY storage
                               * roles — those subtypes were dropped in the
                               * silo cleanup. Save migration in sim_save.c
                               * remaps both back to MODULE_HOPPER. */
    /* Single-type furnace: which ores it can smelt is determined by the
     * station's furnace count, not the module subtype. 1 furnace ⇒
     * ferrite only; 2 ⇒ cuprite (ferrite blocked); 3 ⇒ cuprite + crystal
     * (ferrite still blocked). The MODULE_FURNACE_CU and MODULE_FURNACE_CR
     * subtypes were collapsed away in the count-tier rework — save
     * migration in sim_save.c remaps both back to MODULE_FURNACE. */
    MODULE_FURNACE = 2,
    MODULE_REPAIR_BAY = 3,
    MODULE_SIGNAL_RELAY = 4,
    MODULE_FRAME_PRESS = 5,
    MODULE_LASER_FAB = 6,
    MODULE_TRACTOR_FAB = 7,
    /* enum values 8 (was ORE_SILO) and 10 (was CARGO_BAY) are gone;
     * see SAVE_VERSION 44 migration. SHIPYARD pinned to its old value
     * to keep the migration table simple. */
    MODULE_SHIPYARD = 9,
    MODULE_COUNT = 10
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

/* Sentinel value for station_module_t::last_smelt_commodity meaning
 * "this furnace hasn't smelted anything yet." Renders as the static
 * white chunks-feeder color in middle-ring furnaces. */
#define LAST_SMELT_NONE 0xFFu

typedef struct {
    module_type_t type;     /* 4 bytes — int enum */
    uint8_t ring;           /* 1: which ring tier (0xFF=core, 1=inner, 2=mid, 3=outer) */
    uint8_t slot;           /* 1: position within ring (0..STATION_RING_SLOTS[ring]-1) */
    bool    scaffold;       /* 1: under construction */
    /* Most recent smelt-input commodity processed by this module, or
     * LAST_SMELT_NONE if it's never smelted. Drives the middle-ring
     * furnace glow (see station_palette.h::station_palette_furnace_color):
     * cuprite-input → blue, crystal-input → green, otherwise white. */
    uint8_t last_smelt_commodity; /* 1 byte */
    /* Tag commodity, used by:
     *   - MODULE_HOPPER: which commodity this hopper buffers. Each
     *     hopper holds exactly one commodity; producers needing that
     *     commodity draw from any matching hopper on the station.
     *   - MODULE_FURNACE: which ingot this furnace produces (and, by
     *     symmetry, which ore it smelts). Set at build/order time.
     *     COMMODITY_COUNT on legacy/untagged furnaces falls back to
     *     module_furnace_default_output() (FERRITE_INGOT — Prospect's
     *     1-furnace tier).
     *   - other module types: COMMODITY_COUNT (= "unset"). */
    uint8_t commodity;      /* 1 byte */
    uint8_t _pad[2];        /* explicit pad to 4-byte alignment */
    float   build_progress; /* 0.0 to 1.0 */
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
    /* Soft repulsion velocity from other stations. Each tick the
     * jostle stepper sums pairwise pushes when stations crowd into
     * each other's personal space and integrates this onto pos.
     * High drag → settles within seconds, no persistent oscillation.
     * Transient state — not persisted in saves. */
    vec2 jostle_vel;
    float radius;
    float dock_radius;
    float signal_range;
    bool signal_connected;   /* true = can trace signal path to a root station */
    bool scaffold;           /* true = under construction, not yet active */
    bool planned;            /* true = design phase only, no physical presence */
    int8_t planned_owner;    /* player id who created the plan, -1 = system */
    float scaffold_progress; /* 0.0 to 1.0 */
    float base_price[COMMODITY_COUNT];
    /* Unified storage for all commodities. Treat as the float cache of
     * the manifest for finished goods (c >= COMMODITY_RAW_ORE_COUNT) —
     * direct writes to those slots silently break the manifest invariant.
     * Use station_finished_{mint,drain,accumulate} (see shared/manifest.h)
     * instead. Raw ore slots (c < COMMODITY_RAW_ORE_COUNT) live only here
     * and may be read/written directly. The leading underscore signals
     * "private — go through the accessors". */
    float _inventory_cache[COMMODITY_COUNT];
    uint32_t services;
    /* Module system */
    station_module_t modules[MAX_MODULES_PER_STATION];
    int module_count;
    /* Ring rotation — all rings share one speed, each has a fixed angular offset */
    /* Per-module activity pulse — set to 1.0 each tick a producer
     * actually consumes input + emits output, decays linearly toward
     * 0 over RING_PULSE_LINGER_SEC. The geom emitter reads this to
     * mark spokes as active (visible tractor beam) and the ring
     * dynamics scales spring stiffness by it: when a hopper drains
     * dry and the producer idles, the spoke "turns off" and the
     * passive ring re-equilibriums. Transient runtime state — not
     * persisted in saves. */
    float module_active_pulse[MAX_MODULES_PER_STATION];

    int arm_count;                    /* number of active rings with rotation */
    float arm_rotation[MAX_ARMS];     /* per-ring rotation angle (radians) */
    float arm_speed[MAX_ARMS];        /* DRIVER ring nominal angular velocity
                                       * (rad/s). Passive rings ignore this; their
                                       * speed is driven by spoke spring + drag in
                                       * step_station_ring_dynamics. */
    float arm_omega[MAX_ARMS];        /* passive ring angular velocity state — only
                                       * touched by step_station_ring_dynamics. */
    float ring_offset[MAX_ARMS];      /* fixed angular offset per ring (radians) —
                                       * legacy; new stations leave at 0 and let
                                       * spoke dynamics determine relative phase. */
    char hail_message[256];           /* AI-authored station message of the day */
    char station_slug[32];            /* URL slug for CDN assets (e.g. "prospect") */
    char currency_name[32];           /* station-local currency label, e.g. "helios credits".
                                       * Empty string → HUD falls back to "credits". */
    /* Economy ledger: per-player supply tracking for passive income.
     * Keyed by player_pubkey (Layer A.1/A.2 of #479); legacy session_token
     * entries are migrated to pubkey on load (see sim_save.c v45+ migration). */
    struct {
        uint8_t player_pubkey[32];    /* Ed25519 pubkey of the supplier */
        float balance;                /* spendable station-local credits */
        float lifetime_supply;        /* total ore contributed */
        /* Station-player relationship data (#257) — tracks dock history,
         * trade volume, and absence for AI personality generation. */
        uint64_t first_dock_tick;     /* sim tick of first dock at this station; 0 = never */
        uint64_t last_dock_tick;      /* sim tick of most recent dock; 0 = never */
        uint32_t total_docks;
        uint32_t lifetime_ore_units;  /* sum of ore sold here, all commodities */
        uint32_t lifetime_credits_in; /* total credits issued by this station to bearer */
        uint32_t lifetime_credits_out;/* total credits redeemed against this station's ledger */
        uint8_t top_commodity;        /* most-frequent ore commodity index, for prompt flavor */
        uint8_t _pad[3];
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
    /* (credit_pool field removed — derived from -Σ(ledger.balance) via
     *  station_credit_pool() in server/game_sim.h. Conservation is
     *  structural now; there is no separate stored aggregate.) */
    /* Station cargo manifest — single source of identity for stocked
     * units (named ingots + fabricated goods). Refinery pushes a unit
     * per smelt; shipyards consume units to mint hulls bound to the
     * pub identity. The legacy named_ingots[] dual store was collapsed
     * in the "unify ingot identity" PR. `manifest_dirty` drives the
     * wire-push (server-only). */
    manifest_t    manifest;
    /* Portable cargo receipt chains held by this station.
     *
     * Parallel to `manifest`: receipts.chains[i] is the receipt chain
     * attached to manifest.units[i], just like ship_t.receipts_opaque.
     * Stored as void* to keep types.h independent of cargo_receipt.h.
     * Mutate through station_manifest_* helpers so station inventory can
     * later dispatch cargo by extending the exact incoming chain head. */
    void          *receipts_opaque; /* ship_receipts_t* -- see cargo_receipt.h */
    bool          manifest_dirty;
    /* Shipyard repair-kit fab cadence: server-only countdown. When it
     * reaches the period and the station has 1 frame + 1 laser + 1
     * tractor in its manifest, consume them, mint REPAIR_KIT_PER_BATCH
     * kits, and reset the timer. */
    float         repair_kit_fab_timer;
    /* Layer B of #479 — per-station Ed25519 identity.
     *
     * `station_pubkey` is the station's public identity, derived
     * deterministically from the world seed (seeded stations 0/1/2)
     * or from (founder_pubkey || station_name || planted_tick) for
     * player-planted outposts (indices 3+). Public; baked into the
     * world snapshot sent to clients on connect. Persisted by the
     * world save.
     *
     * `outpost_planted_tick` records the world.time *128 (tick) at
     * which the outpost was planted, used to re-derive its keypair
     * on save/load without persisting the secret. Zero for seeded
     * stations and unfounded slots.
     *
     * `station_secret` is the operator-only Ed25519 private material
     * (seed||pub per the NaCl convention). It is NEVER serialized
     * over the wire and NEVER written to disk — both seeded and
     * outpost stations rederive it from the world seed (or saved
     * founder + name + tick) at load time. A save leak therefore
     * does not leak the private key.
     *
     * If you add fields between `outpost_planted_tick` and
     * `station_secret`, keep the secret LAST in the struct and
     * update the wire-format omit logic in serialize_station_identity
     * + write_station_session accordingly. */
    uint8_t  station_pubkey[32];
    uint8_t  outpost_founder_pubkey[32];
    uint64_t outpost_planted_tick;
    /* Layer C of #479 — signed event chain log state.
     *
     * `chain_last_hash` is the SHA256 of the most recent event header
     * authored by this station (or all zero if no event has been
     * emitted yet). The next event's `prev_hash` field is set to this
     * value, linking the log into a hash chain.
     *
     * `chain_event_count` is the monotonic per-station event counter,
     * stamped into `event_id` and incremented on every emit.
     *
     * Both are persisted by the save (v41+) so the chain survives a
     * server restart. The actual event records live in side files
     * under `chain/<base58(station_pubkey)>.log` — they are NOT part
     * of `world.sav`. */
    uint8_t  chain_last_hash[32];
    uint64_t chain_event_count;
    uint8_t  station_secret[64];   /* MUST stay last — never serialized */
} station_t;

/* Layer B of #479: the wire-format and on-disk serializers for station_t
 * deliberately omit station_secret. Keeping it the LAST field of the
 * struct lets a careful "everything up to station_secret" memcpy stay
 * safe by construction; if you add new fields to station_t, put them
 * BEFORE station_secret and audit serialize_station_identity +
 * write_station_session for new omissions. The static_assert below
 * makes a sneaky reorder loud at compile time. */
_Static_assert(offsetof(station_t, station_secret) >
               offsetof(station_t, station_pubkey),
               "station_secret must be located after station_pubkey "
               "in station_t (Layer B of #479) — keep it the last field");

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
    /* rock_pub: stable identity for terrain (seed-origin) asteroids,
     * computed at first-contact materialization as
     *   SHA256("rock-v1" || belt_seed || cx || cy || slot).
     * Zero on fracture children — they're identified by fragment_pub
     * once their claim resolves. rock_pub is what the destroyed-records
     * ledger keys on, and it's an explicit input to fracture_seed at
     * birth so every downstream hash (fragment_pub, cargo_unit.pub,
     * frame.pub, hull.pub) traces back to a unique (chunk, slot)
     * coordinate in the belt. */
    uint8_t rock_pub[32];
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

/* Input intent — the per-tick command shape that drives a ship. Both
 * the player path (sp->input populated by client keyboard sample) and
 * the NPC path (npc->input populated by the AI brain) feed into the
 * same step_player / sim_ship pipeline. Most fields are player-only
 * UI controls; only turn / thrust / boost / mine are wired on the NPC
 * side today. (Slice 2 of #294.) */
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
    /* Per-row sell mirror of the buy path. When `service_sell_one` is
     * true the server sells exactly one (commodity, grade) unit per
     * input message — matching the [1]/[2]/… buy hotkeys. Bulk paths
     * (sell-all hotkey [S], contract delivery from the yard tab) leave
     * `service_sell_one` false and continue draining everything that
     * fits. `service_sell_grade` selects which manifest unit is
     * dequeued; MINING_GRADE_COUNT means "any grade, FIFO". */
    mining_grade_t service_sell_grade;
    bool service_sell_one;
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
    /* Optional grade hint for manifest-first buys. MINING_GRADE_COUNT =
     * "any grade available, FIFO"; a specific grade means "only transfer
     * a unit of this grade — if none exist, the float path still runs
     * as a legacy common row". */
    mining_grade_t buy_grade;
    int mining_target_hint;  /* client's hover_asteroid, -1 = none */
    bool hail;               /* hail/scan nearby signal contacts */
    bool tractor_hold;       /* R held — tractor active this frame */
    bool release_tow;        /* R tapped — drop all towed fragments */
    bool reset;
    bool toggle_autopilot;   /* one-shot: flip autopilot_mode on/off */
    bool boost;              /* Shift held — thrust multiplier + hull drain */
    bool reverse_thrust;     /* S/Down fresh-pressed from stop: allow backing up */
} input_intent_t;

typedef struct {
    bool active;
    npc_role_t role;
    npc_state_t state;
    /* Physics body. Sim_ship primitives mutate this directly so NPCs
     * and players run through the same code with the same shape.
     * Slice 5 of #294 dropped the npc-side duplicate fields (pos, vel,
     * angle, hull_class) — every reader migrated to `npc->ship.*`.
     * Save format v50+ serializes ship.{pos,vel,angle,hull_class}
     * directly; v49 saves load by remap into the embedded body. */
    ship_t ship;
    /* Per-NPC input intent, the same shape sp->input has on the player
     * side. AI brain writes turn / thrust / boost / mine each tick via
     * npc_set_intent; the apply path reads from here. The
     * player-specific UI fields (place_outpost, plan_*, buy_*, etc.)
     * are unused on NPCs today — they exist on the struct because the
     * unified shape lets a future autopilot/agent path drive a ship
     * through the same dispatch as a human player. */
    input_intent_t input;
    float cargo[COMMODITY_COUNT];
    int target_asteroid;
    int home_station;
    int dest_station;
    float state_timer;
    bool thrusting;
    float tint_r, tint_g, tint_b;  /* manifest rarity display tint */
    int towed_fragment;             /* asteroid index being towed, -1 = none */
    int towed_scaffold;             /* scaffold index being towed (NPC_ROLE_TOW), -1 = none */
    /* Hull HP. Decrements on collision (asteroid / station / ship). When
     * the NPC docks at home, the dock auto-repairs by consuming repair
     * kits from station inventory (1 kit per HP). If hull <= 0 the
     * NPC despawns; sim_ai's spawn loop replaces it on the next tick.
     * Added in save v32 — earlier saves default to npc_max_hull on load. */
    float hull;
    /* Per-NPC economic identity. Stamped onto towed asteroid fragments
     * (a->last_towed_token) so the smelt-payout credits the NPC's
     * ledger entry at the home station, and used by haulers to receive
     * contract payment at the delivery station. Added in save v33 —
     * v32 saves regenerate tokens at load via the next_npc_token
     * counter (the dead ledger entries belonging to the old token
     * just sit until the 16-slot LRU evicts them). */
    uint8_t session_token[8];
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
    float music_duck_target; /* 0.0 = full duck, 1.0 = no duck; smoothly interpolated */
    float music_duck_current;
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
    SIM_EVENT_OPERATOR_POST,  /* Station operator (persona) authored message: e.g., motd */
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
        /* SIM_EVENT_DAMAGE: source_x/source_y let the client pick a
         * world-space direction for the directional hit indicator
         * (chevron at the screen edge pointing toward the threat).
         * Both zero = unknown source (legacy callers, environmental
         * hits) — client renders a center-screen pulse instead. */
        struct { float amount; float source_x; float source_y; } damage;
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
            uint8_t respawn_station; /* index of station the player respawned at */
            float respawn_fee;      /* spawn fee debited at respawn_station */
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
        /* SIM_EVENT_OPERATOR_POST: station operator authored content (e.g. motd).
         * text field carries the posted text content. */
        struct { int station; char text[256]; } operator_post;
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
    ORDER_REJECT_SELL_STATION_BROKE = 10,           /* station ran out of credit pool mid-sale */
    ORDER_REJECT_SELL_INVENTORY_FULL = 11,          /* consumer here but its hopper is full */
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

/* Module schema table — must come after all module/commodity types.
 * Must precede station_geom.h: the geom emitter consults
 * module_pair_intake to emit cross-ring spokes. */
#include "module_schema.h"

/* Unified station collision/render geometry — must come after all
 * station types AND after module_schema.h. */
#include "station_geom.h"

/* Economy / ship-upgrade tuning constants */
#include "economy_const.h"

#endif
