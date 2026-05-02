/*
 * module_schema.c — table + lookups for station module behavior.
 *
 * Split from module_schema.h so editing the schema (frequent during
 * gameplay tuning) doesn't recompile every TU that includes types.h.
 *
 * The table is defined here once and referenced by every caller via
 * the lookup helpers. Callers pay one extra indirect call per query
 * (was an inline-resolved table read); none of these lookups are in
 * inner loops, so the cost is invisible.
 */
#include "types.h"
#include "module_schema.h"

const module_schema_t MODULE_SCHEMA[MODULE_COUNT] = {
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
        .prerequisite = MODULE_SIGNAL_RELAY, /* tier 1 */
        .pair_intake = MODULE_COUNT,
    },
    [MODULE_HOPPER] = {
        .name = "Hopper", /* ore intake + beam anchor for furnaces */
        .kind = MODULE_KIND_STORAGE,
        .input = COMMODITY_FERRITE_ORE, /* primary; accepts all ore types */
        .output = COMMODITY_COUNT,
        .rate = 0.0f, .buffer_capacity = 30.0f,
        .build_material = 40.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 37, /* 150/4 */
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER,
        .variant_count = 0,
        .prerequisite = MODULE_SIGNAL_RELAY, /* tier 1 */
        .pair_intake = MODULE_COUNT,
    },
    [MODULE_FURNACE] = {
        .name = "Furnace",
        .kind = MODULE_KIND_PRODUCER,
        /* The schema input/output reflects this module's *primary* role
         * in the count-tier system: a single furnace smelts ferrite, so
         * that's what the producer-recipe path references. The dynamic
         * tier rules in sim_can_smelt_ore (1=Fe, 2=Fe+Cu, 3=Cu+Cr) live
         * in sim_production.c and run regardless of this static label. */
        .input = COMMODITY_FERRITE_ORE,
        .output = COMMODITY_FERRITE_INGOT,
        .rate = 1.0f, .buffer_capacity = 12.0f,
        .build_material = 60.0f, .build_commodity = COMMODITY_FRAME,
        .order_fee = 50, /* 200/4 */
        .services = 0,
        .valid_rings = MODULE_RINGS_OUTER, /* cross-ring pair rule
                                            * lets furnaces sit on
                                            * any outer ring as long
                                            * as an adjacent ring has
                                            * the paired hopper. */
        .variant_count = 0,
        .prerequisite = MODULE_HOPPER, /* tier 2 — needs hopper */
        .pair_intake = MODULE_HOPPER,
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
        .prerequisite = MODULE_DOCK, /* tier 2 — needs ships docking first */
        .pair_intake = MODULE_COUNT,
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
        .prerequisite = MODULE_COUNT, /* root — always available */
        .pair_intake = MODULE_COUNT,
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
        .prerequisite = MODULE_FURNACE, /* tier 3 — needs ingots */
        .pair_intake = MODULE_HOPPER,
    },
    [MODULE_LASER_FAB] = {
        .name = "Laser Fab",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_CUPRITE_INGOT, /* plus crystal ingots */
        .output = COMMODITY_LASER_MODULE,
        .rate = 0.5f, .buffer_capacity = 12.0f,
        .build_material = 80.0f, .build_commodity = COMMODITY_CUPRITE_INGOT,
        .order_fee = 100,
        .services = STATION_SERVICE_UPGRADE_LASER,
        .valid_rings = MODULE_RINGS_INDUSTRIAL,
        .variant_count = 0,
        .prerequisite = MODULE_FURNACE, /* tier 5 — needs cu ingots from a 2+ furnace stack */
        .pair_intake = MODULE_HOPPER,
    },
    [MODULE_TRACTOR_FAB] = {
        .name = "Tractor Fab",
        .kind = MODULE_KIND_PRODUCER,
        .input = COMMODITY_CUPRITE_INGOT,
        .output = COMMODITY_TRACTOR_MODULE,
        .rate = 0.5f, .buffer_capacity = 12.0f,
        .build_material = 80.0f, .build_commodity = COMMODITY_CRYSTAL_INGOT,
        .order_fee = 100,
        .services = STATION_SERVICE_UPGRADE_TRACTOR,
        .valid_rings = MODULE_RINGS_INDUSTRIAL,
        .variant_count = 0,
        .prerequisite = MODULE_FURNACE, /* tier 5 — needs cr ingots from a 3+ furnace stack */
        .pair_intake = MODULE_HOPPER,
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
        .prerequisite = MODULE_FRAME_PRESS, /* tier 4 — needs frames */
        .pair_intake = MODULE_HOPPER,
    },
};

/* ----- Lookup helpers ----- */

const module_schema_t *module_schema(module_type_t type) {
    if ((unsigned)type >= MODULE_COUNT) return &MODULE_SCHEMA[MODULE_DOCK];
    return &MODULE_SCHEMA[type];
}

module_kind_t module_kind(module_type_t type) {
    return module_schema(type)->kind;
}

bool module_is_producer(module_type_t type) { return module_kind(type) == MODULE_KIND_PRODUCER; }
bool module_is_service (module_type_t type) { return module_kind(type) == MODULE_KIND_SERVICE; }
bool module_is_storage (module_type_t type) { return module_kind(type) == MODULE_KIND_STORAGE; }
bool module_is_shipyard(module_type_t type) { return module_kind(type) == MODULE_KIND_SHIPYARD; }
bool module_is_dead    (module_type_t type) { return module_kind(type) == MODULE_KIND_NONE; }

commodity_t module_schema_input (module_type_t type) { return module_schema(type)->input; }
commodity_t module_schema_output(module_type_t type) { return module_schema(type)->output; }
float       module_production_rate(module_type_t type) { return module_schema(type)->rate; }
float       module_buffer_capacity(module_type_t type) { return module_schema(type)->buffer_capacity; }

bool module_valid_on_ring(module_type_t type, int ring) {
    if (ring < 0 || ring > 3) return false;
    return (module_schema(type)->valid_rings & (1u << ring)) != 0;
}

module_type_t module_pair_intake(module_type_t type) {
    int v = module_schema(type)->pair_intake;
    if (v < 0 || v >= MODULE_COUNT) return MODULE_COUNT;
    return (module_type_t)v;
}

bool module_requires_pair(module_type_t type) {
    return module_pair_intake(type) != MODULE_COUNT;
}

bool module_unlocked_for_player(uint32_t unlocked_mask, module_type_t type) {
    int prereq = module_schema(type)->prerequisite;
    if (prereq < 0 || prereq >= MODULE_COUNT) return true;
    return (unlocked_mask & (1u << prereq)) != 0;
}

const char *module_type_name(module_type_t type) {
    return module_schema(type)->name;
}

commodity_t module_build_material_lookup(module_type_t type) {
    return module_schema(type)->build_commodity;
}

float module_build_cost_lookup(module_type_t type) {
    return module_schema(type)->build_material;
}

int scaffold_order_fee(module_type_t type) {
    return module_schema(type)->order_fee;
}

module_build_state_t module_build_state(const station_module_t *m) {
    if (!m->scaffold) return MODULE_BUILD_COMPLETE;
    if (m->build_progress < 1.0f) return MODULE_BUILD_AWAITING_SUPPLY;
    return MODULE_BUILD_BUILDING;
}

bool module_is_complete(const station_module_t *m) {
    return !m->scaffold;
}

bool module_is_fully_supplied(const station_module_t *m) {
    return !m->scaffold || m->build_progress >= 1.0f;
}

float module_supply_fraction(const station_module_t *m) {
    if (!m->scaffold) return 1.0f;
    float p = m->build_progress;
    if (p > 1.0f) p = 1.0f;
    if (p < 0.0f) p = 0.0f;
    return p;
}

float module_build_timer_fraction(const station_module_t *m) {
    if (!m->scaffold) return 1.0f;
    float t = m->build_progress - 1.0f;
    if (t < 0.0f) return 0.0f;
    if (t > 1.0f) return 1.0f;
    return t;
}
