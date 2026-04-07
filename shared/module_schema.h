/*
 * module_schema.h — Single source of truth for station module behavior.
 *
 * Each module type has a schema entry declaring its kind (service /
 * producer / storage / shipyard), input/output commodities, build cost,
 * order fee, valid rings, and provided services.
 *
 * Production code, plan-mode UI, the order menu, and the material flow
 * graph all read from this table. Adding a new module type means one
 * entry here, not edits across five files.
 *
 * Header-only — included by types.h. Inline functions for lookups so
 * client and server share without a separate .c file.
 */
#ifndef MODULE_SCHEMA_H
#define MODULE_SCHEMA_H

/* types.h includes this file at the bottom — don't re-include types.h.
 * All types (module_type_t, commodity_t, etc.) are available. */

typedef enum {
    MODULE_KIND_NONE,       /* deleted / unused enum value */
    MODULE_KIND_SERVICE,    /* dock, repair, relay — passive utility */
    MODULE_KIND_PRODUCER,   /* furnace, frame press, fab — active conversion */
    MODULE_KIND_STORAGE,    /* hopper, ore silo, cargo bay */
    MODULE_KIND_SHIPYARD,   /* manufactures scaffolds (special-cased) */
} module_kind_t;

/* Ring placement bitmask: bit N = ring N is valid */
#define MODULE_RING_0       (1 << 0)  /* core (dock/relay placeholders) */
#define MODULE_RING_1       (1 << 1)
#define MODULE_RING_2       (1 << 2)
#define MODULE_RING_3       (1 << 3)
#define MODULE_RINGS_ANY    (MODULE_RING_0 | MODULE_RING_1 | MODULE_RING_2 | MODULE_RING_3)
#define MODULE_RINGS_OUTER  (MODULE_RING_1 | MODULE_RING_2 | MODULE_RING_3)
#define MODULE_RINGS_INDUSTRIAL (MODULE_RING_2 | MODULE_RING_3)

typedef struct {
    const char    *name;          /* base display name (variants override) */
    module_kind_t  kind;
    commodity_t    input;         /* COMMODITY_COUNT = none */
    commodity_t    output;        /* COMMODITY_COUNT = none */
    float          rate;          /* units per second (PRODUCER only) */
    float          buffer_capacity; /* per-module local buffer size */
    float          build_material;  /* qty needed to manufacture scaffold */
    commodity_t    build_commodity; /* what material to deliver */
    int            order_fee;       /* shipyard deposit in credits */
    uint32_t       services;        /* STATION_SERVICE_* bitmask */
    uint16_t       valid_rings;     /* MODULE_RING_* bitmask */
    uint8_t        variant_count;   /* >0 if module has variants (furnace=3, fab=2) */
} module_schema_t;

/* The schema table. Indexed by module_type_t.
 * NONE entries are deleted/dead enum values that still exist for
 * backward compatibility but should not be used in new code. */
static const module_schema_t MODULE_SCHEMA[MODULE_COUNT] = {
    [MODULE_DOCK] = {
        .name = "Dock",
        .kind = MODULE_KIND_SERVICE,
        .input = COMMODITY_COUNT, .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 0.0f,
        .build_material = 20.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 25, /* 100/4 */
        .services = 0,
        .valid_rings = MODULE_RINGS_ANY,
        .variant_count = 0,
    },
    [MODULE_ORE_BUYER] = {
        .name = "Hopper", /* renamed from "Ore Buyer" — now hopper-style */
        .kind = MODULE_KIND_STORAGE,
        .input = COMMODITY_FERRITE_ORE, /* primary; accepts all ore types */
        .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 30.0f,
        .build_material = 40.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 37, /* 150/4 */
        .services = STATION_SERVICE_ORE_BUYER,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
    [MODULE_FURNACE] = {
        .name = "Iron Furnace",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_FERRITE_ORE,
        .output = COMMODITY_FERRITE_INGOT,
        .rate = 1.0f, .buffer_capacity = 12.0f,
        .build_material = 60.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 50, /* 200/4 */
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
    [MODULE_FURNACE_CU] = {
        .name = "Copper Furnace",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_CUPRITE_ORE,
        .output = COMMODITY_CUPRITE_INGOT,
        .rate = 0.8f, .buffer_capacity = 12.0f,
        .build_material = 100.0f, .build_commodity = COMMODITY_CUPRITE_INGOT,
        .order_fee = 100,
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
    [MODULE_FURNACE_CR] = {
        .name = "Crystal Furnace",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_CRYSTAL_ORE,
        .output = COMMODITY_CRYSTAL_INGOT,
        .rate = 0.6f, .buffer_capacity = 12.0f,
        .build_material = 140.0f, .build_commodity = COMMODITY_CRYSTAL_INGOT,
        .order_fee = 125,
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
    [MODULE_INGOT_SELLER] = {
        /* DEAD — to be deleted in commit 6 */
        .name = "Ingot Seller",
        .kind = MODULE_KIND_NONE,
    },
    [MODULE_REPAIR_BAY] = {
        .name = "Repair Bay",
        .kind = MODULE_KIND_SERVICE,
        .input = COMMODITY_COUNT, .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 0.0f,
        .build_material = 30.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 50,
        .services = STATION_SERVICE_REPAIR,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
    [MODULE_SIGNAL_RELAY] = {
        .name = "Signal Relay",
        .kind = MODULE_KIND_SERVICE,
        .input = COMMODITY_COUNT, .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 0.0f,
        .build_material = 40.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 37, /* 150/4 */
        .services = 0,
        .valid_rings = MODULE_RINGS_ANY,
        .variant_count = 0,
    },
    [MODULE_FRAME_PRESS] = {
        .name = "Frame Press",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_FERRITE_INGOT,
        .output = COMMODITY_FRAME,
        .rate = 1.0f, .buffer_capacity = 12.0f,
        .build_material = 80.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 75,
        .services = 0,
        .valid_rings = MODULE_RINGS_INDUSTRIAL,
        .variant_count = 0,
    },
    [MODULE_LASER_FAB] = {
        .name = "Laser Fab",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_CUPRITE_INGOT,
        .output = COMMODITY_LASER_MODULE,
        .rate = 0.5f, .buffer_capacity = 12.0f,
        .build_material = 80.0f, .build_commodity = COMMODITY_CUPRITE_INGOT,
        .order_fee = 100,
        .services = STATION_SERVICE_UPGRADE_LASER,
        .valid_rings = MODULE_RINGS_INDUSTRIAL,
        .variant_count = 0,
    },
    [MODULE_TRACTOR_FAB] = {
        .name = "Tractor Fab",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_CRYSTAL_INGOT,
        .output = COMMODITY_TRACTOR_MODULE,
        .rate = 0.5f, .buffer_capacity = 12.0f,
        .build_material = 80.0f, .build_commodity = COMMODITY_CRYSTAL_INGOT,
        .order_fee = 100,
        .services = STATION_SERVICE_UPGRADE_TRACTOR,
        .valid_rings = MODULE_RINGS_INDUSTRIAL,
        .variant_count = 0,
    },
    [MODULE_CONTRACT_BOARD] = {
        /* DEAD — to be deleted in commit 6 */
        .name = "Contract Board",
        .kind = MODULE_KIND_NONE,
        .build_material = 20.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 25,
    },
    [MODULE_ORE_SILO] = {
        .name = "Ore Silo",
        .kind = MODULE_KIND_STORAGE,
        .input = COMMODITY_FERRITE_ORE, /* primary; accepts all ore */
        .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 60.0f, /* big buffer */
        .build_material = 30.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 25,
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
    [MODULE_BLUEPRINT_DESK] = {
        /* DEAD — to be deleted in commit 6 */
        .name = "Blueprint Desk",
        .kind = MODULE_KIND_NONE,
        .services = STATION_SERVICE_BLUEPRINT,
    },
    [MODULE_RING] = {
        /* DEAD — rings are structural, not modules. To be removed. */
        .name = "Ring Truss",
        .kind = MODULE_KIND_NONE,
    },
    [MODULE_SHIPYARD] = {
        .name = "Shipyard",
        .kind = MODULE_KIND_SHIPYARD,
        .input = COMMODITY_FRAME, /* default; consumes whatever the order needs */
        .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 60.0f,
        .build_material = 120.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 125,
        .services = 0,
        .valid_rings = MODULE_RINGS_INDUSTRIAL,
        .variant_count = 0,
    },
    [MODULE_CARGO_BAY] = {
        .name = "Cargo Bay",
        .kind = MODULE_KIND_STORAGE,
        .input = COMMODITY_FERRITE_INGOT, /* generic — accepts any commodity in flow graph */
        .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 120.0f, /* large storage */
        .build_material = 60.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 60,
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
    },
};

/* ----- Lookup helpers -----
 * All return safe defaults for out-of-range or NONE entries. */

static inline const module_schema_t *module_schema(module_type_t type) {
    if ((unsigned)type >= MODULE_COUNT) return &MODULE_SCHEMA[MODULE_DOCK];
    return &MODULE_SCHEMA[type];
}

static inline module_kind_t module_kind(module_type_t type) {
    return module_schema(type)->kind;
}

static inline bool module_is_producer(module_type_t type) {
    return module_kind(type) == MODULE_KIND_PRODUCER;
}

static inline bool module_is_service(module_type_t type) {
    return module_kind(type) == MODULE_KIND_SERVICE;
}

static inline bool module_is_storage(module_type_t type) {
    return module_kind(type) == MODULE_KIND_STORAGE;
}

static inline bool module_is_shipyard(module_type_t type) {
    return module_kind(type) == MODULE_KIND_SHIPYARD;
}

static inline bool module_is_dead(module_type_t type) {
    return module_kind(type) == MODULE_KIND_NONE;
}

static inline commodity_t module_schema_input(module_type_t type) {
    return module_schema(type)->input;
}

static inline commodity_t module_schema_output(module_type_t type) {
    return module_schema(type)->output;
}

static inline float module_production_rate(module_type_t type) {
    return module_schema(type)->rate;
}

static inline float module_buffer_capacity(module_type_t type) {
    return module_schema(type)->buffer_capacity;
}

/* True if this module type can be placed on the given ring (1..3 or 0=core) */
static inline bool module_valid_on_ring(module_type_t type, int ring) {
    if (ring < 0 || ring > 3) return false;
    return (module_schema(type)->valid_rings & (1u << ring)) != 0;
}

/* Display name for a module type — single source of truth. */
static inline const char *module_type_name(module_type_t type) {
    return module_schema(type)->name;
}

/* ----- Legacy lookup helpers (now schema-backed) -----
 * Used by client UI and onboarding. Internally delegate to the schema. */

static inline commodity_t module_build_material_lookup(module_type_t type) {
    return module_schema(type)->build_commodity;
}

static inline float module_build_cost_lookup(module_type_t type) {
    return module_schema(type)->build_material;
}

static inline int scaffold_order_fee(module_type_t type) {
    return module_schema(type)->order_fee;
}

#endif /* MODULE_SCHEMA_H */
