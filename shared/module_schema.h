/*
 * module_schema.h — Single source of truth for station module behavior.
 *
 * Each module type has a schema entry declaring its kind (service /
 * producer / storage / shipyard), input/output commodities, build cost,
 * order fee, valid rings, and provided services.
 *
 * Implementations + the table itself live in module_schema.c. The header
 * is declarations + small structs only, so editing the table doesn't
 * fan out to every TU that includes types.h.
 *
 * Variant question (#280): commodity-specialized producers (furnaces
 * MODULE_FURNACE / _CU / _CR, fabs MODULE_FRAME_PRESS / _LASER_FAB /
 * _TRACTOR_FAB) are encoded as separate enum values rather than
 * collapsed into a single `MODULE_FURNACE` carrying a `commodity_t
 * variant` field. **The current model is final.** Reasons:
 *   - Each variant has independent build cost, prerequisite chain, and
 *     order-menu UX, all of which read cleanly from a single schema row.
 *   - Variants need to address each other (frame press requires iron
 *     furnace as prerequisite); the enum is the natural foreign key.
 *   - On-disk and on-wire representations already serialize the enum;
 *     collapsing would force a save/wire bump for no behavioral win.
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
    commodity_t    input;         /* primary input commodity; some recipes also
                                   * consume a secondary inventory ingredient */
    commodity_t    output;        /* COMMODITY_COUNT = none */
    float          rate;          /* units per second (PRODUCER only) */
    float          buffer_capacity; /* per-module local buffer size */
    float          build_material;  /* qty needed to manufacture scaffold */
    commodity_t    build_commodity; /* what material to deliver */
    int            order_fee;       /* shipyard deposit in credits */
    uint32_t       services;        /* STATION_SERVICE_* bitmask */
    uint16_t       valid_rings;     /* MODULE_RING_* bitmask */
    uint8_t        variant_count;   /* >0 if module has variants */
    int            prerequisite;    /* module type that must be built first
                                     * (MODULE_COUNT = no prerequisite, root) */
    int            pair_intake;     /* required intake module at the paired
                                     * slot on the same ring (canonical 180°
                                     * opposite). MODULE_COUNT = no pairing
                                     * required. Producers feed from this
                                     * slot; placement is rejected unless
                                     * the paired slot already holds it. */
} module_schema_t;

/* The schema table — defined in module_schema.c. */
extern const module_schema_t MODULE_SCHEMA[MODULE_COUNT];

/* ----- Lookup helpers — all return safe defaults for OOR / NONE ----- */
const module_schema_t *module_schema(module_type_t type);
module_kind_t          module_kind(module_type_t type);
bool                   module_is_producer(module_type_t type);
bool                   module_is_service (module_type_t type);
bool                   module_is_storage (module_type_t type);
bool                   module_is_shipyard(module_type_t type);
bool                   module_is_dead    (module_type_t type);
commodity_t            module_schema_input (module_type_t type);
commodity_t            module_schema_output(module_type_t type);
float                  module_production_rate(module_type_t type);
float                  module_buffer_capacity(module_type_t type);
bool                   module_valid_on_ring(module_type_t type, int ring);

/* Pair-intake helpers (#XYZ — pair-based station construction).
 * Producers (and shipyard) require a specific intake module sitting at
 * the paired slot on the same ring. See station_util's
 * station_pair_slot for the slot-pairing geometry. */
module_type_t          module_pair_intake(module_type_t type);
bool                   module_requires_pair(module_type_t type);

/* Per-producer input commodities. Each producer module type consumes
 * a fixed list of commodities (drawn from the recipe table). Hoppers
 * are commodity-tagged buffers; a producer's pair-rule is satisfied
 * iff a hopper exists on the station for each of its required input
 * commodities.
 *
 * FURNACE is special at the SCHEMA level: it accepts ANY ore (the
 * count-tier rules in sim_production gate which ones actually smelt).
 * The list returns all three ore commodities but with `.any_satisfies
 * = true` so the validator only requires one of them. At the INSTANCE
 * level, a furnace tagged with an ingot output commodity (Slice 1 of
 * the cargo-in-space redesign) consumes the matching ore — see
 * module_instance_input_ore().
 *
 * `out` must be sized at least 3. Returns count written. */
typedef struct {
    int         count;
    commodity_t commodities[3];
    bool        any_satisfies; /* true → FURNACE-style "one of these"; false → ALL required */
} module_inputs_t;
module_inputs_t        module_required_inputs(module_type_t type);

/* Per-producer output commodity. Each non-shipyard producer emits one
 * commodity into the station's flow graph; SHIPYARD is exempt (its
 * output is a physical scaffold body, not a commodity).
 *
 * FURNACE at the schema level returns COMMODITY_FERRITE_INGOT (the
 * default Prospect role); per-instance, the output commodity comes
 * from the furnace's `station_module_t.commodity` tag — see
 * module_instance_output(). Returns COMMODITY_COUNT for non-producers
 * and SHIPYARD. */
commodity_t            module_required_output(module_type_t type);

/* Instance-aware accessors. For producers whose I/O depends on
 * per-instance configuration (currently only FURNACE, which tags its
 * output ingot via station_module_t.commodity), these read the
 * instance state and fall back to the schema default for legacy /
 * untagged modules. */
commodity_t            module_instance_output(const station_module_t *m);
commodity_t            module_instance_input_ore(const station_module_t *m);

/* Default output commodity for an untagged FURNACE — matches Prospect
 * (1-furnace tier, ferrite-only). Used by save migration and seeds. */
commodity_t            module_furnace_default_output(void);

/* Map an ingot commodity to its source ore (and back). Returns
 * COMMODITY_COUNT if the input isn't a known ingot/ore. */
commodity_t            commodity_ore_for_ingot(commodity_t ingot);
commodity_t            commodity_ingot_for_ore(commodity_t ore);

/* Tech-tree gate: a module is unlocked when its prerequisite has been
 * built at least once. Roots (prerequisite = MODULE_COUNT) are always
 * available. */
bool                   module_unlocked_for_player(uint32_t unlocked_mask, module_type_t type);

/* Display name + legacy lookup wrappers. */
const char            *module_type_name(module_type_t type);
commodity_t            module_build_material_lookup(module_type_t type);
float                  module_build_cost_lookup(module_type_t type);
int                    scaffold_order_fee(module_type_t type);

/* ----- Module build lifecycle (#307) -----
 *
 * `station_module_t.build_progress` is a single float that overloads two
 * domains, gated by `scaffold`:
 *
 *   scaffold=true,  0.0 <= build_progress < 1.0  : awaiting supply
 *   scaffold=true,  1.0 <= build_progress < 2.0  : supplied, build timer running
 *   scaffold=false, build_progress == 1.0        : complete
 */
typedef enum {
    MODULE_BUILD_AWAITING_SUPPLY = 0,
    MODULE_BUILD_BUILDING,
    MODULE_BUILD_COMPLETE,
} module_build_state_t;

/* Wall-clock seconds between full supply and module activation. */
#define MODULE_BUILD_TIME_SEC 10.0f

module_build_state_t   module_build_state(const station_module_t *m);
bool                   module_is_complete(const station_module_t *m);
bool                   module_is_fully_supplied(const station_module_t *m);
float                  module_supply_fraction(const station_module_t *m);
float                  module_build_timer_fraction(const station_module_t *m);

#endif /* MODULE_SCHEMA_H */
