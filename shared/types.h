#ifndef TYPES_H
#define TYPES_H

#include <stdbool.h>
#include <stddef.h>   /* offsetof — Layer B of #479 station_secret guard */
#include <stdint.h>
#include "actor_principal.h"
#include "cell_geometry.h"
#include "math_util.h"
#include "mining.h"
#include "holographic_nn.h"

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
 * wire-protocol bump and a deserializer change in client/net.c.
 * MAX_STATIONS lifted to 128 (#285 Phase 4a). MAX_ASTEROIDS lifted
 * to 2048 (#285 Phase 3) with uint16 wire indices.
 */
enum {
    KEY_COUNT = 512,
    MAX_ASTEROIDS = 2048, /* uint16 wire index; lifted from 255 in #285 Phase 3 */
    MAX_STARS = 120,
    MAX_STATIONS = 128,  /* lifted from 64 in #285 Phase 4a; uint8 wire index supports 255 */
    STATION_LEDGER_MAX = 64,
    MAX_NPC_SHIPS = 100,  /* uint8 index — see banner above (#285 to lift) */
    MAX_SCAFFOLDS = 16,  /* uint8 index — see banner above (#285 to lift) */
    MAX_CARGO_PODS = 64, /* uint8 wire index; towable engine-less cargo bodies */
    CARGO_POD_MANIFEST_CAP = 200, /* one rich ore fragment can become one full smelt pod */
    CARGO_POD_UNIT_CAPACITY = CELL_HEX_PAYLOAD_CAPACITY,
    AUDIO_VOICE_COUNT = 24,
    AUDIO_MIX_FRAMES = 512,
};

/* Generation-safe references for relationships between recyclable ECS
 * slots. Persistent identity (pubkeys, asset ids, cargo pubs) remains a
 * separate concern; this handle is strictly for live in-memory entities. */
typedef enum {
    ENTITY_KIND_NONE = 0,
    ENTITY_KIND_SHIP,
    ENTITY_KIND_ASTEROID,
    ENTITY_KIND_CARGO_POD,
    ENTITY_KIND_SCAFFOLD,
    ENTITY_KIND_STATION_MODULE,
} entity_kind_t;

typedef struct {
    uint8_t kind;       /* entity_kind_t */
    int16_t index;      /* pool slot; -1 when null */
    int16_t part;       /* module/attachment sub-slot; -1 when unused */
    uint16_t generation;
} entity_ref_t;

typedef enum {
    TOW_PROFILE_NONE = 0,
    TOW_PROFILE_SHIP_FRAGMENT,
    TOW_PROFILE_SHIP_POD,
    TOW_PROFILE_SHIP_SCAFFOLD,
    TOW_PROFILE_MODULE_POD,
} tow_profile_t;

typedef enum {
    TOW_LINK_INACTIVE = 0,
    TOW_LINK_CAPTURE,
    TOW_LINK_HELD,
    TOW_LINK_RELEASING,
} tow_link_state_t;

typedef struct {
    bool active;
    entity_ref_t source;
    entity_ref_t target;
    uint8_t profile;    /* tow_profile_t */
    uint8_t slot;       /* source-local formation/hold slot */
    uint8_t state;      /* tow_link_state_t */
    uint8_t _pad;
} tow_link_t;

static inline entity_ref_t entity_ref_none(void) {
    return (entity_ref_t){
        .kind = ENTITY_KIND_NONE,
        .index = -1,
        .part = -1,
        .generation = 0,
    };
}

static inline bool entity_ref_is_none(entity_ref_t ref) {
    return ref.kind == ENTITY_KIND_NONE || ref.index < 0 ||
           ref.generation == 0;
}

static inline bool entity_ref_equal(entity_ref_t a, entity_ref_t b) {
    return a.kind == b.kind && a.index == b.index && a.part == b.part &&
           a.generation == b.generation;
}

enum {
    SIGNAL_ROOT_STATION_COUNT = 3,
    SIGNAL_FREEPORT_STATION_INDEX = 3,
    SIGNAL_SEEDED_STATION_COUNT = 4,
    SIGNAL_FIRST_OUTPOST_INDEX = 4,
};

typedef enum {
    STATION_FACTION_UNALIGNED = 0,
    STATION_FACTION_PROSPECTOR_GUILD,
    STATION_FACTION_KEPLER_COMPACT,
    STATION_FACTION_HELIOS_CONSORTIUM,
    STATION_FACTION_BLACKGLASS_SYNDICATE,
    STATION_FACTION_COUNT,
} station_faction_id_t;

typedef enum {
    STATION_IDEOLOGY_PRAGMATIC = 0,
    STATION_IDEOLOGY_COOPERATIVE,
    STATION_IDEOLOGY_INDUSTRIAL,
    STATION_IDEOLOGY_EXPANSIONIST,
    STATION_IDEOLOGY_OPPORTUNIST,
    STATION_IDEOLOGY_COUNT,
} station_ideology_t;

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
    COMMODITY_REPAIR_KIT,        /* 1 kit = 1 HP at a dock; produced by shipyards
                                   * from 1 FRAME + 1 LASER + 1 TRACTOR → 100 kits.
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
    HULL_CLASS_DRONE_TRACTOR,
    HULL_CLASS_DRONE_LASER,
    HULL_CLASS_DRONE_CARGO,
    HULL_CLASS_COUNT,
} hull_class_t;

typedef enum {
    SHIP_MODULE_TRACTOR = 1 << 0,
    SHIP_MODULE_LASER   = 1 << 1,
    SHIP_MODULE_CARGO   = 1 << 2,
} ship_module_flags_t;

typedef enum {
    CHAIN_HEALTH_UNKNOWN = 0,
    CHAIN_HEALTH_FRESH = 1,
    CHAIN_HEALTH_OK = 2,
    CHAIN_HEALTH_EMPTY = 3,
    CHAIN_HEALTH_ADOPTED = 4,
    CHAIN_HEALTH_MISMATCH = 5,
    CHAIN_HEALTH_FAILED = 6,
} chain_health_status_t;

enum {
    STATION_AUTHORITY_REGISTRY_VERSION = 1,
    STATION_AUTHORITY_REGISTRY_CAP = 8,
};

/*
 * Public station-authority records deliberately keep verified lifecycle
 * separate from the local trust decision consumed by the receipt evaluator.
 * This mirrors the origin-proof contract without making shared/types.h depend
 * on cargo_receipt.h.
 */
typedef enum {
    STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED = 0,
    STATION_AUTHORITY_LIFECYCLE_CURRENT = 1,
    STATION_AUTHORITY_LIFECYCLE_ROTATED = 2,
    STATION_AUTHORITY_LIFECYCLE_REVOKED = 3,
} station_authority_lifecycle_state_t;

typedef enum {
    STATION_AUTHORITY_TRUST_UNKNOWN = 0,
    STATION_AUTHORITY_TRUST_CURRENT = 1,
    STATION_AUTHORITY_TRUST_ROTATED = 2,
    STATION_AUTHORITY_TRUST_UNTRUSTED = 3,
    STATION_AUTHORITY_TRUST_REVOKED = 4,
} station_authority_trust_state_t;

typedef struct {
    uint8_t pubkey[32];
    uint8_t lifecycle; /* station_authority_lifecycle_state_t */
    uint8_t trust;     /* station_authority_trust_state_t */
    uint8_t _pad[2];
} station_authority_record_t;

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
    uint8_t module_slots;
    uint8_t module_mask; /* ship_module_flags_t */
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
    /* Reserved compatibility kind for a hypothetical grouped raw-ore
     * manifest row. No live production path allocates raw ore as a
     * cargo_unit_t: physical fragments live as asteroid_t, and station
     * raw-ore buffers live in _inventory_cache[]. Keep the enum value
     * for wire/save stability and legacy tests. */
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
 * `quantity` is the count of items in this crate. Current production paths
 * mint individually addressable ingots, frames, lasers, tractors, and kits
 * with quantity == 1. Future grouped/anonymous crate rows may use quantity
 * > 1 when individual addressability carries no value. Raw ore does not
 * enter manifests in normal play. Cap is u8 (255) — beyond that the caller
 * pushes a new crate. */
typedef struct {
    uint8_t  kind;              /* cargo_kind_t */
    uint8_t  commodity;         /* commodity_t */
    uint8_t  grade;             /* mining_grade_t */
    uint8_t  prefix_class;      /* ingot_prefix_t (anonymous for non-ingot kinds) */
    uint16_t recipe_id;         /* recipe_id_t */
    uint8_t  origin_station;    /* refinery/fabricator that produced it */
    uint8_t  quantity;          /* items in this crate. 1 for current live
                                 * production; >1 reserved for grouped rows.
                                 * Was _pad pre-v45; legacy saves with
                                 * quantity == 0 migrate to 1 on load. */
    uint64_t mined_block;       /* mint tick/event marker when known; 0 = unknown/legacy */
    uint8_t  pub[32];           /* content hash */
    uint8_t  parent_merkle[32]; /* sorted-input merkle root */
} cargo_unit_t;                 /* 80 bytes */

typedef struct {
    uint16_t count;
    uint16_t cap;
    cargo_unit_t *units;
} manifest_t;

/* Transactional cargo ownership. A manifest row and its receipt chain are
 * one component and must be inserted, removed, cloned, and destroyed
 * together. receipts_opaque is a ship_receipts_t* (kept opaque here to
 * avoid the types.h <-> cargo_receipt.h cycle). */
typedef struct {
    manifest_t manifest;
    void *receipts_opaque;
} cargo_store_t;

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

/* Gossip-contract bounded memory caps. Ships and stations carry
 * snapshots of contracts they've learned about via dock contact;
 * new entries push out oldest (FIFO eviction). Information speed =
 * ship speed. Stations are gossip hubs (bigger pool, 10 = 2 pages);
 * ships are couriers (smaller pool, hull-class-dependent — for v0
 * NPCs and players share the same cap, future may scale by hull).
 * The full contract_t (declared later in this file) is the
 * authoritative storage at the issuing station. The summary is the
 * gossiped payload. */
enum {
    SHIP_KNOWN_CONTRACT_CAP = 3,
    STATION_KNOWN_CONTRACT_CAP = 10,
};

typedef struct {
    bool active;
    uint8_t action;          /* contract_action_t */
    uint8_t station_index;   /* destination/issuer */
    uint8_t commodity;       /* commodity_t */
    uint8_t required_grade;  /* mining_grade_t */
    uint8_t proof_flags;     /* contract_proof_flags_t */
    uint8_t required_prefix_class; /* ingot_prefix_t, iff REQUIRE_PREFIX */
    uint16_t required_recipe_id;   /* recipe_id_t, iff REQUIRE_RECIPE */
    uint8_t required_parent[32];   /* cargo parent_merkle, iff REQUIRE_PARENT */
    uint8_t target_pub[32];        /* stable target identity, when known */
    float quantity_needed;
    float base_price;
    float age_at_copy;       /* issuer's age at the moment this snapshot was taken */
    uint64_t forbidden_origin_mask; /* station-origin bitmask, iff FORBID_ORIGIN */
} contract_summary_t;        /* keep small — embedded in station_t and npc_ship_t arrays */

typedef enum {
    MARKET_MEMORY_NONE = 0,
    MARKET_MEMORY_DEMAND,
    MARKET_MEMORY_SUPPLY,
    MARKET_MEMORY_ROUTE_DANGER,
    MARKET_MEMORY_ROUTE_SUCCESS,
    MARKET_MEMORY_DELIVERY_RECEIPT,
    MARKET_MEMORY_ROUTE_REPUTATION,
    MARKET_MEMORY_ROUTE_RISK,
    MARKET_MEMORY_STATION_TRUST,
    MARKET_MEMORY_STATION_RISK,
    MARKET_MEMORY_ORE_PRESSURE,
    MARKET_MEMORY_SCAFFOLD_PRESSURE,
} market_memory_kind_t;

/* Fuzzy, portable market impression. This is not an authoritative
 * contract or ledger row; it is the small structured payload that lets
 * stations and ships exchange decaying economic pressure through the
 * generic knowledge_view_t path. */
typedef struct {
    bool active;
    uint8_t memory_kind;      /* market_memory_kind_t */
    uint8_t station_a;        /* primary station: demand/danger endpoint */
    uint8_t station_b;        /* secondary station/source, 0xff if none */
    uint8_t commodity;        /* commodity_t, COMMODITY_COUNT if generic */
    uint8_t action;           /* contract_action_t or 0xff if generic */
    uint8_t confidence;       /* 0..255 trust in the memory */
    uint8_t salience;         /* 0..255 decision pressure */
    uint16_t quantity_hint;   /* compact units, 0 = unknown */
    uint16_t value_hint;      /* compact payout/value, 0 = unknown */
    uint32_t observed_tick;   /* source observation tick, if known */
    uint64_t subject_nonce;   /* optional caller-provided disambiguator */
} market_memory_t;

typedef enum {
    KNOW_NONE = 0,
    KNOW_CONTRACT,
    KNOW_CARGO,
    KNOW_FRAGMENT,
    KNOW_STATION,
    KNOW_ROUTE,
    KNOW_PLAYER,
    KNOW_SHIP,
    KNOW_MODULE,
    KNOW_SCAFFOLD,
    KNOW_EVENT,
    KNOW_SIGNAL,
    KNOW_MARKET,
} knowledge_kind_t;

typedef enum {
    KNOW_PAYLOAD_NONE = 0,
    KNOW_PAYLOAD_CONTRACT_SUMMARY = 1,
    KNOW_PAYLOAD_MARKET_MEMORY = 2,
} knowledge_payload_kind_t;

enum {
    KNOWLEDGE_PAYLOAD_BYTES = 96,
    KNOWLEDGE_VIEW_MAX_CAP = 64,
    SHIP_KNOWN_ITEM_CAP = 16,
    STATION_KNOWN_ITEM_CAP = 64,
};

typedef struct {
    uint8_t kind;                         /* knowledge_kind_t; 0 = empty */
    uint8_t hops;
    uint8_t confidence;
    uint8_t salience;
    uint8_t payload_kind;                 /* knowledge_payload_kind_t */
    uint8_t _pad[3];
    uint8_t subject_hash[32];             /* cargo pub, route hash, contract key, etc. */
    uint8_t chain_anchor[32];             /* canonical receipt/event hash if known */
    uint8_t source_hash[32];              /* immediate source, if known */
    uint8_t witness_hash[32];             /* original witness, if known */
    uint64_t observed_tick;
    uint64_t learned_tick;
    uint8_t payload[KNOWLEDGE_PAYLOAD_BYTES];
} knowledge_item_t;

typedef struct {
    knowledge_item_t items[KNOWLEDGE_VIEW_MAX_CAP];
    uint8_t count;
    uint8_t capacity;                     /* active cap inside items[] */
    uint8_t _pad[6];
} knowledge_view_t;

_Static_assert(sizeof(contract_summary_t) <= KNOWLEDGE_PAYLOAD_BYTES,
               "contract_summary_t must fit in knowledge payload");
_Static_assert(sizeof(market_memory_t) <= KNOWLEDGE_PAYLOAD_BYTES,
               "market_memory_t must fit in knowledge payload");

typedef struct {
    recipe_id_t   id;
    const char   *name;
    cargo_kind_t  output_kind;
    commodity_t   output_commodity; /* COMMODITY_COUNT = caller supplies */
    uint16_t      output_count;      /* product units minted per recipe batch */
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
    int16_t towed_pods[10];       /* indices into cargo_pods, -1 = empty */
    uint8_t towed_pod_count;
    int16_t towed_scaffold;       /* scaffold index being towed, -1 = none */
    bool tractor_active;          /* true while Space held — drives tow collection */
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
    union {
        cargo_store_t cargo_store;
        struct {
            manifest_t manifest; /* compatibility view of cargo_store */
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
            void *receipts_opaque; /* compatibility view of cargo_store */
        };
    };

    /* Authoritative bounded situated knowledge for contract gossip,
     * market observations, and route memories. */
    knowledge_view_t knowledge;
} ship_t;

/* The authoritative ship component pool stores one component per live actor.
 * Player/NPC records are controllers and keep only a non-owning reference to
 * these stable slots. Generation zero is reserved for an inactive slot. */
typedef struct {
    bool active;
    uint16_t generation;
    ship_t component;
} ship_slot_t;

enum {
    SHIP_ASSET_ID_NONE = 0,
};

typedef enum {
    SHIP_ASSET_STATUS_STORED = 0,
    SHIP_ASSET_STATUS_ASSIGNED,
    SHIP_ASSET_STATUS_DESTROYED,
} ship_asset_status_t;

typedef enum {
    SHIP_ASSET_OPERATOR_NONE = 0,
    SHIP_ASSET_OPERATOR_PLAYER,
    SHIP_ASSET_OPERATOR_NPC,
} ship_asset_operator_kind_t;

typedef enum {
    SHIP_ASSET_PROVENANCE_GENESIS = 0,
    SHIP_ASSET_PROVENANCE_SHIPYARD,
    SHIP_ASSET_PROVENANCE_LEGACY,
    SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY,
} ship_asset_provenance_t;

typedef enum {
    PENDING_SHIP_BUILD_MODE_UNKNOWN = 0,
    PENDING_SHIP_BUILD_MODE_MATERIAL,
    PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY,
    PENDING_SHIP_BUILD_MODE_COUNT,
} pending_ship_build_mode_t;

enum {
    SHIP_BIRTH_PROOF_FRAGMENT_COUNT = 3,
    SHIP_BIRTH_PROOF_VERSION_V1 = 1,
};

typedef struct {
    bool active;
    uint32_t asset_id;
    hull_class_t hull_class;
    /* A hull asset is either dormant or live, never both. Stored assets own
     * their durable ship snapshot here. Assigned assets reference the single
     * authoritative component in world.ships; stored_ship is only refreshed
     * at the explicit assigned -> stored/destroyed lifecycle boundary. */
    ship_t stored_ship;
    entity_ref_t live_ship_ref;
    ship_t *ship; /* transient non-owning view; rebuild after load/world copy */
    actor_principal_t owner_principal;
    /* Stable foreign key to the exact inert ownership-quarantine row when
     * owner_principal is NONE. Zero for actionable owned assets. */
    uint64_t owner_quarantine_record_id;
    uint8_t status;          /* ship_asset_status_t */
    uint8_t operator_kind;   /* ship_asset_operator_kind_t */
    uint8_t provenance;      /* ship_asset_provenance_t */
    int16_t custody_station;
    int16_t operator_slot;
    int16_t build_station;
    bool loaner;
    bool destroyed;
    uint8_t birth_proof_version;
    uint8_t birth_fragment_grades[SHIP_BIRTH_PROOF_FRAGMENT_COUNT];
    uint8_t birth_soul_pub[32];
    uint8_t birth_material_root[32];
    uint8_t birth_fragment_pubs[SHIP_BIRTH_PROOF_FRAGMENT_COUNT][32];
} ship_asset_t;

typedef struct {
    hull_class_t hull_class;
    actor_principal_t owner_principal;
    /* Stable deny-latch foreign keys. Historical station/row locators are
     * diagnostic snapshots and must never authorize queue state. */
    uint64_t owner_quarantine_record_id;
    uint64_t mode_quarantine_record_id;
    float build_progress;
    uint8_t mode; /* pending_ship_build_mode_t */
} pending_ship_build_t;

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
    /* Single-type furnace: which ore it smelts is determined by the
     * module's per-instance commodity tag (FERRITE/CUPRITE/CRYSTAL
     * ingot), not by a station-wide furnace count. The old
     * MODULE_FURNACE_CU and MODULE_FURNACE_CR subtypes were collapsed
     * away — save migration in sim_save.c remaps both back to
     * MODULE_FURNACE and tags legacy instances. */
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
        case COMMODITY_LASER_MODULE:  return "laser modules";
        case COMMODITY_TRACTOR_MODULE:return "tractor modules";
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
     *     module_furnace_default_output() (FERRITE_INGOT, Prospect's
     *     starter refinery).
     *   - other module types: COMMODITY_COUNT (= "unset"). */
    uint8_t commodity;      /* 1 byte */
    uint8_t _pad[2];        /* explicit pad to 4-byte alignment */
    float   build_progress; /* 0.0 to 1.0 */
    /* Runtime/flow state belongs to the module slot. Keeping these fields
     * beside the module identity makes append/remove/compaction atomic and
     * prevents an old buffer or diagnostic from being inherited when an
     * array slot is reused. Save and wire formats still serialize the
     * compatible fields explicitly; transient fields remain transient. */
    float   input_buffer;
    float   output_buffer;
    float   active_pulse;
    float   craft_progress;
    uint8_t flow_diag;      /* station_flow_diag_t */
    uint8_t _runtime_pad[3];
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
    /* Explicit hopper buffers for raw ore. Finished-good slots are retired
     * compatibility storage and remain zero during live simulation; finished
     * stock is derived from cargo_store.manifest. */
    float _inventory_cache[COMMODITY_COUNT];
    /* Fractional production/consumption below one addressable cargo unit.
     * Whole finished units always live in cargo_store.manifest. */
    float _finished_residue[COMMODITY_COUNT];
    uint32_t services;
    /* Module system */
    station_module_t modules[MAX_MODULES_PER_STATION];
    int module_count;
    /* Ring rotation — all rings share one speed, each has a fixed angular offset */
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
    char miner_chatter[8][64];        /* station-authored short miner lines */
    char hauler_chatter[8][64];       /* station-authored short hauler lines */
    char rati_hail_message[256];      /* special hail when local player delivers RATi+ ore */
    char station_slug[32];            /* URL slug for CDN assets (e.g. "prospect") */
    char currency_name[32];           /* station-local currency label, e.g. "helios credits".
                                       * Empty string → HUD falls back to "credits". */
    /* Political identity. Factions are simulation state, not just lore:
     * policy cards, provenance screening, and future patrol/war behavior
     * read these compact fields. `faction_relations[faction]` is this
     * station's stance toward that faction, -100..100. */
    uint8_t faction_id;                /* station_faction_id_t */
    uint8_t faction_allegiance;        /* station_faction_id_t; can differ for vassal/outpost */
    uint8_t faction_ideology;          /* station_ideology_t */
    int8_t  faction_relations[STATION_FACTION_COUNT];
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
    } ledger[STATION_LEDGER_MAX];
    int ledger_count;
    /* Shipyard: pending scaffold orders awaiting materials */
    struct {
        module_type_t type;
        int8_t owner;  /* player id who placed the order, -1 = NPC/anyone */
    } pending_scaffolds[4];
    int pending_scaffold_count;
    /* Shipyard: hull/frame commissions. Ships are built by one active
     * shipyard from finished goods; station-module scaffolds are the
     * heavier two-shipyard fabrication path above. */
    pending_ship_build_t pending_ship_builds[4];
    int pending_ship_build_count;
    /* Transient/client-mirrored inventory summary. Derived from world_t
     * ship_assets by custody station; not persisted. */
    uint8_t stored_hull_count[HULL_CLASS_COUNT];
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
    /* Runtime-only station policy cache. The deterministic policy scorer
     * refreshes this once per sim tick; future neural scorers should write
     * the same card ids/scores so downstream contract logic stays auditable.
     * Not persisted and not included in station wire identity/econ records. */
    uint64_t policy_tick;
    uint32_t policy_generation;
    uint8_t policy_budget_trade;
    uint8_t policy_budget_construction;
    uint8_t policy_budget_finance;
    uint8_t policy_card_count;
    uint8_t policy_card_ids[8];
    uint8_t policy_card_domains[8];
    uint8_t policy_card_costs[8];
    float policy_card_scores[8];
    uint8_t policy_top_demand_commodity;
    uint8_t policy_pad[3];
    float policy_top_demand_severity;
    float policy_top_demand_price_mult;
    /* (credit_pool field removed — derived from -Σ(ledger.balance) via
     *  station_credit_pool() in server/game_sim.h. Conservation is
     *  structural now; there is no separate stored aggregate.) */
    /* Station cargo manifest — single source of identity for stocked
     * units (named ingots + fabricated goods). Refinery pushes a unit
     * per smelt; shipyards consume units to mint hulls bound to the
     * pub identity. The legacy named_ingots[] dual store was collapsed
     * in the "unify ingot identity" PR. `manifest_dirty` drives the
     * wire-push (server-only). */
    union {
        cargo_store_t cargo_store;
        struct {
            manifest_t manifest; /* compatibility view of cargo_store */
    /* Portable cargo receipt chains held by this station.
     *
     * Parallel to `manifest`: receipts.chains[i] is the receipt chain
     * attached to manifest.units[i], just like ship_t.receipts_opaque.
     * Stored as void* to keep types.h independent of cargo_receipt.h.
     * Mutate through station_manifest_* helpers so station inventory can
     * later dispatch cargo by extending the exact incoming chain head. */
            void *receipts_opaque; /* compatibility view of cargo_store */
        };
    };
    bool          manifest_dirty;
    /* Shipyard repair-kit fab cadence: server-only countdown. When it
     * reaches the period and the station has 1 frame + 1 laser + 1
     * tractor in its manifest, consume them, mint REPAIR_KIT_PER_BATCH
     * kits, and reset the timer. */
    float         repair_kit_fab_timer;
    /* Layer B of #479 — per-station Ed25519 identity.
     *
     * `station_pubkey` is the station's public identity, derived from
     * operator-held station authority secret material plus the world
     * seed (seeded stations 0/1/2) or plus (founder_pubkey ||
     * station_name || planted_tick) for player-planted outposts
     * (indices 3+). Public; baked into the world snapshot sent to
     * clients on connect. Persisted by the world save.
     *
     * `outpost_planted_tick` records the world.time *128 (tick) at
     * which the outpost was planted, used with the operator-held secret
     * to re-derive its keypair on save/load without persisting the
     * station secret. Zero for seeded stations and unfounded slots.
     *
     * `station_secret` is the operator-only Ed25519 private material
     * (seed||pub per the NaCl convention). It is NEVER serialized
     * over the wire and NEVER written to disk — both seeded and
     * outpost stations rederive it from the configured station authority
     * secret plus public provenance at load time. A save leak therefore
     * does not leak the private key.
     *
     * If you add fields between `outpost_planted_tick` and
     * `station_secret`, keep the secret LAST in the struct and
     * update the wire-format omit logic in serialize_station_identity
     * + write_station_session accordingly. */
    uint8_t  station_pubkey[32];
    uint8_t  outpost_founder_pubkey[32];
    uint64_t outpost_planted_tick;
    /*
     * Immutable public actor identity for durable ownership. Unlike the
     * station signing key above, this identifier never rotates when an
     * operator key changes. It is derived once from world/station creation
     * provenance, persisted explicitly, and never used as signing material.
     */
    uint8_t station_actor_id[ACTOR_PRINCIPAL_ID_SIZE];
    /*
     * Runtime-only provenance bit. Only a successfully validated v8+ catalog
     * may set this; reset/bootstrap actors and legacy catalogs are not an
     * independent attestation against the world snapshot.
     */
    bool station_actor_catalog_attested;
    /*
     * Versioned, bounded public authority history. Row zero is the live
     * current key for an occupied station. Later rows preserve historical
     * keys or explicit deny decisions. No private material belongs here.
     */
    uint8_t authority_registry_version;
    uint8_t authority_registry_count;
    uint8_t authority_registry_pad[6];
    station_authority_record_t
        authority_registry[STATION_AUTHORITY_REGISTRY_CAP];
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
     * of `world.sav`.
     *
     * The chain_health_* fields are runtime-only startup verification
     * state. They deliberately are not serialized: the next boot must
     * re-walk the chain logs and make a fresh append/no-append decision. */
    /* Station-local authoritative situated knowledge. Ephemeral and rebuilt
     * from local contracts/state at bootstrap; visiting ships exchange it. */
    knowledge_view_t knowledge;

    uint8_t  chain_last_hash[32];
    uint64_t chain_event_count;
    uint8_t  chain_health_status; /* chain_health_status_t */
    bool     chain_append_blocked;
    bool     chain_append_block_warned;
    uint8_t  chain_health_pad[5];
    uint64_t chain_verified_event_count;
    uint8_t  chain_verified_last_hash[32];
    char     chain_health_message[128];
    /* Holographic market-memory pool — accumulated from structured market
     * memories carried by neural workers who dock. Runtime-only, not
     * serialized. Kept separate from flight-control HNN experience so fuzzy
     * economic attention cannot cross-talk with pilot state->action memory. */
    hnn_memory_t hnn_market_memory;
    uint32_t hnn_market_version;
    uint32_t hnn_market_decay_tick;

    /* Holographic experience pool — accumulated from pilots who dock.
     * Runtime-only, not serialized. Bundled via VSA addition; every
     * docking holographic pilot contributes their memory to this pool.
     * Version increments on each write so ships know when to download. */
    hnn_memory_t hnn_experience;
    uint32_t hnn_experience_version;
    uint32_t hnn_experience_upload_count;
    uint32_t hnn_experience_download_count;
    uint8_t hnn_experience_last_source_station;
    uint8_t hnn_experience_pad[3];

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
_Static_assert(offsetof(station_t, station_secret) +
                   sizeof(((station_t *)0)->station_secret) ==
                   sizeof(station_t),
               "station_secret must be the final bytes of station_t");

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

/* Target-side tractor relationship shared by ship, NPC, and station-module
 * beams. source_generation is reserved for stable actor/module handles;
 * zero denotes the current wire/save compatibility generation. */
typedef enum {
    TRACTOR_SOURCE_NONE = 0,
    TRACTOR_SOURCE_PLAYER,
    TRACTOR_SOURCE_NPC,
    TRACTOR_SOURCE_STATION_MODULE,
} tractor_source_kind_t;

typedef struct {
    tractor_source_kind_t kind;
    int16_t source_index;
    int16_t source_part;
    uint16_t source_generation;
} tractor_binding_t;

static inline void tractor_binding_clear(tractor_binding_t *binding) {
    if (!binding) return;
    *binding = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_NONE,
        .source_index = -1,
        .source_part = -1,
    };
}

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
    tractor_binding_t tractor;
    /* Nascent state: built at station center while NASCENT */
    int built_at_station;       /* station building this scaffold (-1 if not nascent) */
    float build_amount;         /* material accumulated, complete at module_build_cost() */
} scaffold_t;

static inline int scaffold_tractor_player(const scaffold_t *scaffold) {
    return scaffold && scaffold->tractor.kind == TRACTOR_SOURCE_PLAYER
        ? scaffold->tractor.source_index : -1;
}

static inline int scaffold_tractor_npc(const scaffold_t *scaffold) {
    return scaffold && scaffold->tractor.kind == TRACTOR_SOURCE_NPC
        ? scaffold->tractor.source_index : -1;
}

static inline bool scaffold_has_tractor(const scaffold_t *scaffold) {
    return scaffold && scaffold->tractor.kind != TRACTOR_SOURCE_NONE;
}

static inline void scaffold_set_player_tractor(scaffold_t *scaffold,
                                                int player_idx) {
    if (!scaffold || player_idx < 0) return;
    scaffold->tractor = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_PLAYER,
        .source_index = (int16_t)player_idx,
        .source_part = -1,
    };
}

static inline void scaffold_set_npc_tractor(scaffold_t *scaffold,
                                             int npc_idx) {
    if (!scaffold || npc_idx < 0) return;
    scaffold->tractor = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_NPC,
        .source_index = (int16_t)npc_idx,
        .source_part = -1,
    };
}

static inline void scaffold_clear_tractor(scaffold_t *scaffold) {
    if (!scaffold) return;
    tractor_binding_clear(&scaffold->tractor);
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
    ASTEROID_PHASE_SOLID = 0,
    ASTEROID_PHASE_GAS_RICH = 1,
} asteroid_phase_t;

typedef enum {
    CARGO_POD_NONE = 0,
    CARGO_POD_GAS = 1,
    CARGO_POD_CARGO = 2,
} cargo_pod_kind_t;

typedef struct {
    bool active;
    cargo_pod_kind_t kind;
    commodity_t commodity;
    uint16_t quantity;
    uint16_t manifest_count;
    cargo_unit_t manifest_units[CARGO_POD_MANIFEST_CAP];
    bool has_shell_frame;
    cargo_unit_t shell_frame; /* folded COMMODITY_FRAME unit unfolded into this pod shell */
    uint16_t shipment_id; /* delivery_shipment_t::shipment_id when this wraps credit cargo */
    uint8_t summary_flags; /* live net summary flags; not persisted as authority */
    uint8_t summary_grade; /* live net best manifest grade; not persisted as authority */
    vec2 pos;
    vec2 vel;
    float radius;
    float rotation;
    float spin;
    float age;
    tractor_binding_t tractor;
    uint8_t tow_hardpoint_tag; /* 0 = none; 1..6 = complete-edge hardpoint */
    uint8_t custody_station; /* 0 = none; station index + 1 owns/charges this pod */
} cargo_pod_t;

static inline void cargo_pod_clear_module_tractor(cargo_pod_t *pod) {
    if (!pod) return;
    if (pod->tractor.kind == TRACTOR_SOURCE_STATION_MODULE) {
        tractor_binding_clear(&pod->tractor);
        pod->tow_hardpoint_tag = 0;
    }
}

static inline bool cargo_pod_has_module_tractor(const cargo_pod_t *pod) {
    return pod && pod->tractor.kind == TRACTOR_SOURCE_STATION_MODULE;
}

static inline bool cargo_pod_module_tractor_indices(const cargo_pod_t *pod,
                                                    int *out_station,
                                                    int *out_module) {
    if (!cargo_pod_has_module_tractor(pod)) return false;
    int station = pod->tractor.source_index;
    int module = pod->tractor.source_part;
    if (station < 0 || station >= MAX_STATIONS ||
        module < 0 || module >= MAX_MODULES_PER_STATION) {
        return false;
    }
    if (out_station) *out_station = station;
    if (out_module) *out_module = module;
    return true;
}

static inline bool cargo_pod_is_tractored_by_module(const cargo_pod_t *pod,
                                                    int station_idx,
                                                    int module_idx) {
    int ps = -1, pm = -1;
    return cargo_pod_module_tractor_indices(pod, &ps, &pm) &&
           ps == station_idx && pm == module_idx;
}

static inline void cargo_pod_set_module_tractor(cargo_pod_t *pod,
                                                int station_idx,
                                                int module_idx) {
    if (!pod || station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= MAX_MODULES_PER_STATION) {
        return;
    }
    pod->tractor = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_STATION_MODULE,
        .source_index = (int16_t)station_idx,
        .source_part = (int16_t)module_idx,
    };
    pod->tow_hardpoint_tag = 0;
}

static inline int cargo_pod_player_tractor(const cargo_pod_t *pod) {
    return pod && pod->tractor.kind == TRACTOR_SOURCE_PLAYER
        ? pod->tractor.source_index : -1;
}

static inline bool cargo_pod_has_player_tractor(const cargo_pod_t *pod) {
    return cargo_pod_player_tractor(pod) >= 0;
}

static inline void cargo_pod_set_player_tractor(cargo_pod_t *pod,
                                                 int player_idx) {
    if (!pod || player_idx < 0) return;
    pod->tractor = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_PLAYER,
        .source_index = (int16_t)player_idx,
        .source_part = -1,
    };
    pod->tow_hardpoint_tag = 0;
}

static inline void cargo_pod_clear_tractor(cargo_pod_t *pod) {
    if (!pod) return;
    tractor_binding_clear(&pod->tractor);
    pod->tow_hardpoint_tag = 0;
}

static inline int cargo_pod_custody_station(const cargo_pod_t *pod) {
    if (!pod || pod->custody_station == 0) return -1;
    int station = (int)pod->custody_station - 1;
    return (station >= 0 && station < MAX_STATIONS) ? station : -1;
}

static inline void cargo_pod_set_station_custody(cargo_pod_t *pod,
                                                 int station_idx) {
    if (!pod || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    pod->custody_station = (uint8_t)(station_idx + 1);
}

static inline void cargo_pod_clear_station_custody(cargo_pod_t *pod) {
    if (!pod) return;
    pod->custody_station = 0;
}

typedef enum {
    CRYSTAL_STAGE_RAW = 0,
    CRYSTAL_STAGE_INTERMEDIATE = 1,
} crystal_stage_t;

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
    /* Current tractor ownership. This is the authoritative live
     * relationship; ship tow arrays are source-side projections retained
     * for capacity and wire/save compatibility. */
    tractor_binding_t tractor;
    /* The int8 player-id pair below is retained only for exact legacy-save
     * compatibility and diagnostics. Live ownership reads `tractor`; payout
     * attribution reads the stable token form, which survives reconnects. */
    int8_t last_towed_by;      /* historical payout provenance; not live ownership */
    int8_t last_fractured_by;  /* player ID who fractured the parent, -1 = none */
    float smelt_progress;      /* 0.0-1.0: how far through smelting (in furnace beam) */
    /* Crystal ore has a two-stage smelt. Stage 0 is the raw fractured
     * crystal; the first crystal furnace converts it into a tractorable
     * intermediate fragment and records the source furnace. Stage 1 must
     * be dragged to a different crystal furnace before minting crystal
     * ingots. Non-crystal fragments keep stage 0 and ignore the
     * source fields. */
    uint8_t crystal_stage;             /* crystal_stage_t */
    uint8_t crystal_stage_station;     /* source station, 0xFF = unset */
    uint8_t crystal_stage_module;      /* source module index, 0xFF = unset */
    uint8_t phase;                     /* asteroid_phase_t */
    float gas_emit_timer;              /* gas-rich terrain rocks emit towable gas pods */
    bool net_dirty;   /* needs network sync (spawn, fracture, HP change, death) */
    /* Fragment provenance: fracture_seed is fixed at birth. fragment_pub
     * and grade stay zero/common until the fracture claim window resolves,
     * then become immutable inputs to smelt + downstream crafting. */
    uint8_t last_towed_token[8];      /* session token of last towing player, zero = none */
    uint8_t thrown_by_token[8];       /* ballistic owner, zero = drift / expired */
    uint8_t thrown_timer_q;           /* 0.1s units; 0 = not a thrown rock */
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

static inline int asteroid_tractor_player(const asteroid_t *asteroid) {
    return asteroid && asteroid->tractor.kind == TRACTOR_SOURCE_PLAYER
        ? asteroid->tractor.source_index : -1;
}

static inline int asteroid_tractor_npc(const asteroid_t *asteroid) {
    return asteroid && asteroid->tractor.kind == TRACTOR_SOURCE_NPC
        ? asteroid->tractor.source_index : -1;
}

static inline bool asteroid_has_tractor(const asteroid_t *asteroid) {
    return asteroid && asteroid->tractor.kind != TRACTOR_SOURCE_NONE;
}

static inline void asteroid_set_player_tractor(asteroid_t *asteroid,
                                                int player_idx) {
    if (!asteroid || player_idx < 0) return;
    asteroid->tractor = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_PLAYER,
        .source_index = (int16_t)player_idx,
        .source_part = -1,
    };
}

static inline void asteroid_set_npc_tractor(asteroid_t *asteroid,
                                             int npc_idx) {
    if (!asteroid || npc_idx < 0) return;
    asteroid->tractor = (tractor_binding_t){
        .kind = TRACTOR_SOURCE_NPC,
        .source_index = (int16_t)npc_idx,
        .source_part = -1,
    };
}

static inline void asteroid_clear_tractor(asteroid_t *asteroid) {
    if (!asteroid) return;
    tractor_binding_clear(&asteroid->tractor);
}

typedef enum {
    NPC_ROLE_MINER,
    NPC_ROLE_HAULER,
    /* NPC_ROLE_TOW: legacy save/network role. New scaffold delivery work is
     * selected as a neural worker tow contract and executed by hauler-class
     * workers. */
    NPC_ROLE_TOW,
} npc_role_t;

static inline hull_class_t npc_default_hull_class_for_role(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return HULL_CLASS_NPC_MINER;
    case NPC_ROLE_HAULER: return HULL_CLASS_HAULER;
    case NPC_ROLE_TOW:    return HULL_CLASS_DRONE_TRACTOR;
    default:              return HULL_CLASS_DRONE_LASER;
    }
}

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
    /* Explicit network one-shots. The legacy/local interact field is
     * context-sensitive, but wire actions must stay semantic so a delayed
     * or duplicated LAUNCH cannot turn into a DOCK after state changes. */
    bool dock;
    bool launch;
    bool interact;
    bool service_sell;
    /* Selective delivery filter for service_sell. COMMODITY_COUNT means
     * "deliver everything that fits a contract or the primary buy slot"
     * (the default). Setting this to a specific commodity restricts the
     * delivery to that one commodity, so the player can keep e.g. their
     * crystal cargo while still delivering ferrite. */
    commodity_t service_sell_only;
    /* Per-row delivery mirror of the buy path. Towed pod rows leave
     * `service_sell_one` false and hand off a whole physical crate.
     * `service_sell_one` is reserved for selective bound-cargo /
     * black-market delivery paths, where `service_sell_grade` narrows
     * the matching unit; MINING_GRADE_COUNT means "any grade". */
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
    bool commission_ship;
    hull_class_t commission_hull_class;
    bool buy_product;
    commodity_t buy_commodity;
    bool buy_station_pod;
    uint16_t buy_station_pod_index;
    /* Optional grade hint for manifest-first buys. MINING_GRADE_COUNT =
     * "any grade available, FIFO"; a specific grade means "only transfer
     * a unit of this grade — if none exist, the float path still runs
     * as a legacy common row". */
    mining_grade_t buy_grade;
    int mining_target_hint;  /* client's hover_asteroid, -1 = none */
    bool hail;               /* hail/scan nearby signal contacts */
    bool tractor_hold;       /* Space held — tractor active this frame */
    bool release_tow;        /* Space tapped — release towed bodies */
    bool reset;
    bool toggle_autopilot;   /* one-shot: flip autopilot_mode on/off */
    bool boost;              /* Shift held — thrust multiplier + hull drain */
    bool reverse_thrust;     /* S/Down fresh-pressed from stop: allow backing up */
} input_intent_t;

typedef struct {
    bool active;
    npc_role_t role;
    npc_state_t state;
    uint32_t ship_asset_id;
    /* Physics body. Sim_ship primitives mutate this directly so NPCs
     * and players run through the same code with the same shape.
     * Slice 5 of #294 dropped the npc-side duplicate fields (pos, vel,
     * angle, hull_class) — every reader migrated to `npc->ship->*`.
     * Save format v50+ serializes ship.{pos,vel,angle,hull_class}
     * directly; v49 saves load by remap into the embedded body. */
    entity_ref_t ship_ref;
    ship_t *ship; /* non-owning view of world.ships[ship_ref.index].component */
    /* Per-NPC input intent, the same shape sp->input has on the player
     * side. AI brain writes turn / thrust / boost / mine each tick via
     * npc_set_intent; the apply path reads from here. The
     * player-specific UI fields (place_outpost, plan_*, buy_*, etc.)
     * are unused on NPCs today — they exist on the struct because the
     * unified shape lets a future autopilot/agent path drive a ship
     * through the same dispatch as a human player. */
    input_intent_t input;
    int target_asteroid;
    int home_station;
    int dest_station;
    /* Runtime-only hauler route. Home remains the owner/maintenance station;
     * pickup_station can temporarily point at a remote source learned through
     * supply gossip before final delivery to dest_station. Not serialized. */
    int pickup_station;
    commodity_t pickup_commodity;
    uint8_t pickup_action;
    float state_timer;
    bool thrusting;
    float tint_r, tint_g, tint_b;  /* manifest rarity display tint */
    /* Per-NPC economic identity. Stamped onto towed asteroid fragments
     * (a->last_towed_token) so the smelt-payout credits the NPC's
     * ledger entry at the home station, and used by haulers to receive
     * contract payment at the delivery station. Added in save v33 —
     * v32 saves regenerate tokens at load via the next_npc_token
     * counter (the dead ledger entries belonging to the old token
     * just sit until the 16-slot LRU evicts them). */
    uint8_t session_token[8];

    /* Runtime-only inspect diagnostics for the latest job-offer comparison.
     * kind is inspect_diag_kind_t; score/selected are compact UI values.
     * These are not authority and are not serialized. */
    uint8_t job_diag_count;
    uint8_t job_diag_kind[4];
    uint8_t job_diag_score[4];
    uint8_t job_diag_selected[4];
    uint8_t job_diag_source[4];
    uint8_t job_diag_dest[4];
    uint8_t job_diag_commodity[4];
    uint16_t job_diag_hint[4];
    uint8_t job_diag_factor_value[4];
    uint8_t job_diag_factor_demand[4];
    uint8_t job_diag_factor_supply[4];
    uint8_t job_diag_factor_route[4];
    uint8_t job_diag_factor_freshness[4];
    uint8_t job_diag_factor_capability[4];
    uint8_t job_diag_factor_proof[4];
    uint8_t job_diag_factor_hologram[4];
    uint8_t job_diag_reason[4];
    uint8_t job_diag_memory_kind[4];
    uint8_t job_diag_memory_hops[4];
    uint8_t job_diag_memory_age[4];
    uint8_t job_diag_memory_station[4];
    uint8_t job_diag_proof_kind[4];
    uint8_t job_diag_proof_prefix[4][4];
    uint8_t job_diag_proof_hash[4][32];

    /* Neural brain mode: 0 = heuristic (legacy), 1 = neural flight.
     * When set, the step_npc_ships loop delegates flight control to
     * signal_brain_drive_npc instead of the role-specific state machine. */
    uint8_t brain_mode;

    /* Holographic market-memory trace for worker economic resonance.
     * Runtime-only and separate from hnn_mem so market pressure cannot
     * interfere with flight-control state->action associations. */
    hnn_memory_t hnn_market_mem;
    uint32_t hnn_market_version; /* station market version last synced */
    uint8_t hnn_market_station;  /* station index of the synced version */
    uint8_t hnn_market_pad[3];
    uint32_t hnn_market_decay_tick;

    /* Holographic associative memory for VSA-based flight control.
     * Stores bundled (state -> action) associations in a 1024-dim
     * hypersphere vector. Runtime-only — not serialized. */
    hnn_memory_t hnn_mem;
    uint32_t hnn_experience_version; /* source station version carried, 0 = none */
    uint32_t hnn_experience_local_version; /* increments on local flight stores */
    uint32_t hnn_experience_uploaded_local_version;
    uint32_t hnn_experience_uploaded_source_version;
    uint8_t hnn_experience_station; /* source station of carried pool, 0xff = none */
    uint8_t hnn_experience_uploaded_station;
    uint8_t hnn_experience_uploaded_source_station;
    uint8_t hnn_experience_pad[1];
} npc_ship_t;

/* NPCs use the same ship tow component as players. */
static inline int npc_towed_fragment_index(const npc_ship_t *npc) {
    if (!npc) return -1;
    if (npc->ship->towed_count > 0) {
        int idx = npc->ship->towed_fragments[0];
        if (idx >= 0 && idx < MAX_ASTEROIDS) return idx;
    }
    return -1;
}

static inline void npc_clear_towed_fragment(npc_ship_t *npc) {
    if (!npc) return;
    npc->ship->towed_count = 0;
    for (int i = 0; i < (int)(sizeof(npc->ship->towed_fragments) /
                              sizeof(npc->ship->towed_fragments[0])); i++) {
        npc->ship->towed_fragments[i] = -1;
    }
}

static inline void npc_set_towed_fragment_index(npc_ship_t *npc, int idx) {
    if (!npc) return;
    npc_clear_towed_fragment(npc);
    if (idx < 0 || idx >= MAX_ASTEROIDS) return;
    npc->ship->towed_fragments[0] = (int16_t)idx;
    npc->ship->towed_count = 1;
}

/* ------------------------------------------------------------------ */
/* character_t — actor/controller registry                            */
/* ------------------------------------------------------------------ */
/*
 * This is deliberately a registry, not another component store. NPC and
 * player actor records each contain exactly one authoritative ship_t;
 * character_t identifies the controller and actor slot without mirroring
 * physics, AI state, targets, or tow ownership.
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
    /* Resolves directly to world.npc_ships[slot] for NPC kinds. */
    int actor_slot;
    entity_ref_t ship_ref;
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
    uint8_t  last_hash[32]; /* latest durable entry_hash, even if ring truncated */
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
    /* DELIVERY: recourse shipment credit.
     * - station_index is the destination station that pays on delivery.
     * - target_index is the origin/source station that releases cargo on
     *   credit and clears the debt when destination proof returns. */
    CONTRACT_DELIVERY = 2,
} contract_action_t;

typedef enum {
    CONTRACT_PROOF_REQUIRE_PROOF  = 1u << 0, /* cargo carries identity/provenance bytes */
    CONTRACT_PROOF_REQUIRE_RECIPE = 1u << 1, /* required_recipe_id must match */
    CONTRACT_PROOF_REQUIRE_PREFIX = 1u << 2, /* required_prefix_class must match */
    CONTRACT_PROOF_REQUIRE_PARENT = 1u << 3, /* required_parent must match parent_merkle */
    CONTRACT_PROOF_FORBID_ORIGIN  = 1u << 4, /* origin_station bit must not be in forbidden_origin_mask */
} contract_proof_flags_t;

typedef enum {
    DELIVERY_SHIPMENT_OFFERED = 0,
    DELIVERY_SHIPMENT_PICKED_UP,
    DELIVERY_SHIPMENT_DELIVERED,
    DELIVERY_SHIPMENT_CLEARED,
    DELIVERY_SHIPMENT_BLACK_MARKET_SOLD,
    DELIVERY_SHIPMENT_DEFAULTED,
} delivery_shipment_status_t;


enum { SIM_MAX_EVENTS = 64 };

typedef enum {
    SIM_EVENT_FRACTURE,
    SIM_EVENT_PICKUP,
    SIM_EVENT_MINING_TICK,
    SIM_EVENT_DOCK,
    SIM_EVENT_LAUNCH,
    SIM_EVENT_SELL,
    SIM_EVENT_BUY,
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
    DEATH_CAUSE_THROWN_ROCK = 2,  /* asteroid attributed via thrown_by_token */
    DEATH_CAUSE_ASTEROID    = 3,  /* unattributed asteroid collision */
    DEATH_CAUSE_STATION     = 4,  /* corridor / module crush */
    DEATH_CAUSE_SELF        = 5,  /* X-key reset / self-destruct */
} death_cause_t;

typedef enum {
    HAIL_DECISION_MODE_NONE = 0,
    HAIL_DECISION_MODE_DOCKED = 1,
    HAIL_DECISION_MODE_DOCK_RANGE = 2,
    HAIL_DECISION_MODE_SIGNAL_RANGE = 3,
} hail_decision_mode_t;

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
        struct { int station; uint8_t commodity; uint8_t grade; int cost;
                 uint16_t quantity; } buy;
        struct { int slot; } outpost_placed;
        struct {
            int station;
            float credits;
            int contract_index;
            uint32_t decision_flags;
            uint64_t decision_source_id;
            float decision_signal_quality;
            uint8_t decision_candidate_count;
            uint8_t decision_mode;
        } hail_response;
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
         * a kill-feed line. killer_token attributes to the player whose
         * ship rammed it or whose ballistic rock hit during its TTL. */
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

typedef enum {
    SIM_INTERACTION_NONE = 0,
    SIM_INTERACTION_TRACTOR_BEAM = 1,
} sim_interaction_type_t;

typedef enum {
    SIM_INTERACTION_ENTITY_NONE = 0,
    SIM_INTERACTION_ENTITY_STATION_MODULE = 1,
    SIM_INTERACTION_ENTITY_CARGO_POD = 2,
    SIM_INTERACTION_ENTITY_PLAYER_SHIP = 3,
    SIM_INTERACTION_ENTITY_ASTEROID = 4,
    SIM_INTERACTION_ENTITY_SCAFFOLD = 5,
} sim_interaction_entity_type_t;

typedef enum {
    SIM_INTERACTION_VISUAL_DEFAULT_TRACTOR = 0,
    SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR = 1,
    SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR = 2,
} sim_interaction_visual_t;

typedef struct {
    uint8_t type;      /* sim_interaction_type_t */
    int16_t index;    /* entity index; for station modules, station index */
    int16_t aux;      /* extra index; for station modules, module index */
} sim_interaction_entity_ref_t;

typedef struct {
    uint8_t type;      /* sim_interaction_type_t */
    uint8_t visual;    /* sim_interaction_visual_t */
    uint8_t commodity; /* COMMODITY_COUNT when not commodity-tinted */
    uint8_t flags;
    sim_interaction_entity_ref_t source;
    sim_interaction_entity_ref_t target;
    vec2 source_pos;
    vec2 target_pos;
    float range;
    float intensity;
} sim_interaction_t;

#define SIM_MAX_INTERACTIONS 128
typedef struct {
    sim_interaction_t items[SIM_MAX_INTERACTIONS];
    int count;
} sim_interactions_t;

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
    /* Optional provenance restrictions. Zero means legacy behavior:
     * commodity + minimum grade only. Heritage contracts set one or more
     * proof_flags so cargo history can become part of the job. */
    uint8_t proof_flags;
    uint8_t required_prefix_class;
    uint16_t required_recipe_id;
    uint8_t required_parent[32];
    uint8_t target_pub[32];
    uint64_t forbidden_origin_mask;
    float quantity_needed;  /* amount (SUPPLY) or radius (SCAN) */
    float base_price;
    float age;
    vec2 target_pos;        /* world position (DESTROY/SCAN target) */
    int target_index;       /* asteroid slot (DESTROY) or -1 */
    int8_t claimed_by;      /* player/NPC id, -1 = open */
} contract_t;

/* Physical one-size carrier mass, complete-edge hardpoints, and hex hull. */
#include "cargo_pod_geometry.h"

/* Station query/geometry helpers — must come after station_t */
#include "station_util.h"

/* Module schema table — must come after all module/commodity types.
 * Must precede station_geom.h: the geom emitter consults
 * module_pair_intake to emit cross-ring spokes. */
#include "module_schema.h"

/* Deterministic station module -> axial cell projection. */
#include "station_cells.h"

/* Unified station collision/render geometry — must come after all
 * station types AND after module_schema.h. */
#include "station_geom.h"

/* Economy / ship-upgrade tuning constants */
#include "economy_const.h"

#endif
