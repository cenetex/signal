/*
 * station_util.h — Helpers for querying and computing station state.
 * Implementations live in station_util.c.
 *
 * Lifecycle predicates, module queries, dominant-module / primary-trade
 * derivations, ring rotation, module world position, ring-slot accounting.
 */
#ifndef STATION_UTIL_H
#define STATION_UTIL_H

#include <stdbool.h>
#include <stddef.h>
/* types.h includes this file at the bottom — don't re-include types.h.
 * All types (station_t, module_type_t, commodity_t, vec2, etc.) and the
 * STATION_NUM_RINGS / STATION_RING_RADIUS / STATION_RING_SLOTS constants
 * are available from the includer. Same pattern as station_geom.h. */

/* ----- Lifecycle predicates ----- */
bool          station_exists(const station_t *st);
bool          station_is_active(const station_t *st);
bool          station_provides_docking(const station_t *st);
bool          station_provides_signal(const station_t *st);
bool          station_collides(const station_t *st);

/* ----- Module queries ----- */
bool          station_has_module(const station_t *st, module_type_t type);
int           station_active_module_count(const station_t *st, module_type_t type);
int           station_active_shipyard_count(const station_t *st);
bool          station_can_order_scaffold(const station_t *st, module_type_t type);
int           station_max_ring(const station_t *st);
float         station_collision_envelope_radius(const station_t *st);
int           station_spawn_fee(const station_t *st);
bool          station_consumes(const station_t *st, commodity_t c);
bool          station_produces(const station_t *st, commodity_t c);
void          rebuild_station_services(station_t *st);
/* Atomic module-slot lifecycle. Runtime buffers and diagnostics live inside
 * station_module_t, so callers must use these helpers rather than changing
 * module_count or compacting modules[] by hand. */
void          station_module_clear_runtime(station_module_t *module);
void          station_modules_clear_runtime(station_t *st);
station_module_t *station_module_append(station_t *st, module_type_t type,
                                        uint8_t ring, uint8_t slot,
                                        bool scaffold, float build_progress,
                                        commodity_t commodity);
bool          station_module_remove(station_t *st, int module_index);
void          station_module_copy_identity(station_module_t *dst,
                                           const station_module_t *src);

/* ----- Construction yard state ----- */
int           station_nascent_scaffold_index(const scaffold_t *scaffolds,
                                             int scaffold_count,
                                             int station_idx);
int           station_construction_blocker_index(const station_t *st,
                                                 const scaffold_t *scaffolds,
                                                 int scaffold_count);
bool          station_construction_area_blocked(const station_t *st,
                                                const scaffold_t *scaffolds,
                                                int scaffold_count);
bool          station_planned_site_abandoned(const station_t *st);

typedef struct {
    bool          station_shell; /* true for an outpost scaffold, false for a module */
    int           module_index;  /* -1 when station_shell is true */
    module_type_t module_type;
    commodity_t   material;
    float         required;
    float         supplied;
    float         remaining;
} station_construction_need_t;

bool          station_construction_material_need(const station_t *st,
                                                 station_construction_need_t *out);

/* Count active (non-scaffold) MODULE_FURNACE modules. Kept for labels,
 * tests, and migration diagnostics; smelt capability itself is
 * tag/pair-based. */
int           station_furnace_count(const station_t *st);

/* Tagged furnace smelt capability, shared between sim + client. A station
 * can smelt an ore when it has an active FURNACE tagged for the matching
 * output ingot plus an active matching ore HOPPER on an adjacent ring.
 * Crystal is special: full crystal processing requires two distinct
 * crystal furnace+hopper pairs because the first pass creates a
 * tractorable intermediate and the second pass mints the ingot. */
bool          station_can_smelt(const station_t *st, commodity_t ore);

/* Display / trade derivations from the dominant module. */
module_type_t station_dominant_module(const station_t *st);
commodity_t   station_primary_buy(const station_t *st);
commodity_t   station_primary_sell(const station_t *st);

/* ----- Ring rotation / module world geometry ----- */
float         station_ring_rotation(const station_t *st, int ring);
vec2          module_world_pos_ring(const station_t *st, int ring, int slot);
vec2          station_ring_point_velocity(const station_t *st, int ring,
                                          vec2 point);
vec2          module_world_velocity_ring(const station_t *st, int ring,
                                         int slot);
float         module_angle_ring   (const station_t *st, int ring, int slot);
/* Point on a live module collision surface nearest `target_pos`. Beam and
 * flow renderers share this so no visual starts at the module center or
 * invents a different radius. */
bool          station_module_surface_point_toward(const station_t *st,
                                                  int module_idx,
                                                  vec2 target_pos,
                                                  vec2 *out_point);
float         station_dock_lane_angle(const station_t *st, int ring, int slot);
vec2          station_dock_lane_pos(const station_t *st, int ring, int slot,
                                    float radius);
/* The open station roadway for a ring: the middle of the intentionally
 * unconnected wrap gap between the highest occupied slot and lowest
 * occupied slot. Returns false for empty rings. */
bool          station_ring_open_gap_lane(const station_t *st, int ring,
                                         int *out_slot, float *out_offset);
float         station_ring_open_gap_angle(const station_t *st, int ring);
vec2          station_ring_open_gap_lane_pos(const station_t *st, int ring,
                                             float radius);
int           ring_module_count(const station_t *st, int ring);
bool          station_has_ring(const station_t *st, int ring);
bool          ring_has_dock(const station_t *st, int ring);
int           station_ring_free_slot(const station_t *st, int ring, int port_count);

/* ----- Slot pairing: "across-the-ring-gap on an adjacent ring" -----
 *
 * Producers (FURNACE / FRAME_PRESS / LASER_FAB / TRACTOR_FAB /
 * SHIPYARD) require a HOPPER directly across the ring gap on an
 * adjacent ring. Geometry: for a producer on ring N at slot S (canonical
 * angle θ = TWO_PI * S / SLOTS[N]), the paired hopper sits on ring N+1
 * (or N-1) at the slot whose canonical angle is closest to θ. The
 * cross-ring beam this implies is the visual signature of every
 * station — see CLAUDE.md ("cross-ring tractor beams are the
 * signature").
 *
 * station_pair_neighbors fills `out` with up to two candidates — one
 * per adjacent ring — sorted ring+1 first, then ring-1. The validator
 * accepts the producer if EITHER candidate slot already holds the
 * required intake. */
typedef struct {
    int ring;   /* adjacent ring (N-1 or N+1) */
    int slot;   /* closest-angle slot on that ring */
} station_slot_pair_t;

/* Returns the count of pair candidates written to `out` (0..2). */
int           station_pair_neighbors(int ring, int slot,
                                     station_slot_pair_t out[2]);

/* Look up the module installed at a given (ring, slot). Returns
 * MODULE_COUNT when the slot is empty, holds a scaffold-only module,
 * or args are out of range. */
module_type_t station_module_at(const station_t *st, int ring, int slot);

/* True when `type` has no pairing requirement, or all its required
 * input commodities (per module_required_inputs) have a compatible
 * tagged hopper. Furnaces are stricter than other producers: the
 * matching ore hopper must be on an adjacent ring, because physical
 * smelting uses the furnace/hopper cross-ring beam. */
bool          station_pair_satisfied(const station_t *st, int ring, int slot,
                                     module_type_t type);

/* Find the index of the hopper module on `st` whose commodity tag
 * matches `commodity`. Returns -1 if none. */
int           station_find_hopper_for(const station_t *st, commodity_t commodity);

/* Find the output hopper that buffers the producer module `m`'s
 * output commodity. Returns the hopper module index on `st`, or -1
 * if no matching tagged hopper exists, or if `m` is not a producer
 * (services, hoppers, shipyards). FURNACEs read their per-instance
 * commodity tag — see module_instance_output(). */
int           station_find_output_hopper_for_module(const station_t *st,
                                                    const station_module_t *m);

/* Layout-validation status for a single module on a station. Slice 1
 * surfaces this informationally — production keeps running even on a
 * "missing output hopper" layout — so the renderer / order menu can
 * badge the module without breaking existing stations. Slice 5 will
 * promote MISSING_OUTPUT_HOPPER into a hard placement reject. */
typedef enum {
    STATION_LAYOUT_OK = 0,
    STATION_LAYOUT_MISSING_INPUT_HOPPER,
    STATION_LAYOUT_MISSING_OUTPUT_HOPPER,
} station_layout_status_t;

station_layout_status_t station_module_layout_status(const station_t *st,
                                                     const station_module_t *m);
/* Commodity tag assigned when a module is first seeded or enters scaffold
 * construction. Hoppers auto-pick the first uncovered producer input;
 * furnaces are explicitly tagged to their default ingot output. */
commodity_t station_default_module_commodity(const station_t *st,
                                             module_type_t type);

/* Flow diagnostics for a single module. This is intentionally compact
 * because it is mirrored over STATION_DIAG as one byte per module:
 * enough for the dock UI to say why production is stuck without exposing
 * raw sim buffers on the wire. */
typedef enum {
    STATION_FLOW_DIAG_NONE = 0,
    STATION_FLOW_DIAG_RUNNING,
    STATION_FLOW_DIAG_NO_INPUT,
    STATION_FLOW_DIAG_OUTPUT_FULL,
    STATION_FLOW_DIAG_NO_CONSUMER,
    STATION_FLOW_DIAG_CONSUMER_FULL,
    STATION_FLOW_DIAG_SLOW_FEED,
    STATION_FLOW_DIAG_AWAITING_SUPPLY,
} station_flow_diag_t;

station_flow_diag_t station_module_flow_diag(const station_t *st,
                                             int module_index);
const char *station_flow_diag_label(station_flow_diag_t diag);
void station_reconcile_module_diag_for_identity(station_t *st,
                                                const station_module_t *modules,
                                                int module_count);

typedef struct {
    station_flow_diag_t diag;
    int                 module_index;  /* -1 for aggregate summaries */
    module_type_t       module_type;
    int                 active_count;
} station_flow_summary_t;

typedef enum {
    STATION_PLAN_FLOW_ROLE_NONE = 0,
    STATION_PLAN_FLOW_ROLE_INPUT,
    STATION_PLAN_FLOW_ROLE_OUTPUT,
} station_plan_flow_role_t;

typedef struct {
    station_flow_diag_t      diag;
    station_plan_flow_role_t role;
    module_type_t            peer_type;
    commodity_t              commodity;
    float                    rate;
} station_plan_flow_hint_t;

/* Display-facing flow summary. `mirrored_authoritative` means callers
 * should trust station_module_t.flow_diag exactly, as multiplayer clients do.
 * Otherwise the helper uses any mirrored non-idle byte when present and
 * falls back to deriving local sim state. */
station_flow_diag_t station_module_flow_diag_view(const station_t *st,
                                                  int module_index,
                                                  bool mirrored_authoritative);
bool station_flow_summary(const station_t *st, bool mirrored_authoritative,
                          station_flow_summary_t *out);
bool station_flow_summary_format(const station_flow_summary_t *summary,
                                 char *out, size_t cap);
bool station_plan_flow_hint(const station_t *st, module_type_t type,
                            int ring, int slot,
                            station_plan_flow_hint_t *out);
bool station_plan_flow_hint_format(const station_plan_flow_hint_t *hint,
                                   char *out, size_t cap);
float station_clamp_operator_price(float requested, float baseline);

/* ----- Demand: what is this station starving for, right now? -----
 *
 * Pure derived state — there is no stored demand field. The primitive
 * scans every commodity the station consumes (per station_consumes)
 * and reports the one with the worst supply-vs-target deficit. Output
 * is shared across the wire so HUD beacons, NPC haulers, contract
 * auto-pricing, and station-side dock UIs can all read the same
 * "starving for X" signal.
 *
 * `severity` is in [0, 1]: 0 = supply meets/exceeds the target,
 * 1 = total starvation (zero supply on a station that needs it most).
 * `price_mult` is the recommended pay multiplier vs. base_price for
 * filling the shortage — currently 1.0 + 0.5 * severity, capped at
 * 1.5×. Callers are free to apply or ignore it.
 *
 * `commodity` is COMMODITY_COUNT when the station has no demand at all
 * (every consumed commodity is at or above its target). Callers should
 * gate on `severity > 0` rather than checking the commodity sentinel
 * alone — both happen together but the explicit float check is more
 * obvious in branch-heavy callsites. */
typedef struct {
    commodity_t commodity;   /* COMMODITY_COUNT when nothing is short */
    float       severity;    /* 0..1 */
    float       price_mult;  /* 1.0..1.5 */
} station_demand_t;

station_demand_t station_top_demand(const station_t *st);

/* Per-commodity variant. Returns the demand for `c` specifically.
 * `commodity` field on the result is set to `c` if there's any
 * shortage, else COMMODITY_COUNT — same convention as
 * station_top_demand. Use this from contract pricing where the
 * commodity is already known and the question is "how starved are
 * they for this one?". */
station_demand_t station_demand_for(const station_t *st, commodity_t c);

/* Downstream raw-ore demand, ignoring the current raw hopper fill. This
 * answers whether an ore is still useful at all once it reaches the
 * furnace. */
float station_raw_ore_chain_need_score(const station_t *st, commodity_t ore);

/* Raw-ore demand for miners and ore contracts. This looks through the
 * furnace output into the downstream local chain: a refinery with a full
 * ingot shelf does not need more ore, and a multi-ore station prefers
 * the ore whose module output is actually starved. Returns 0 when
 * mining that ore would just add stuck mass. */
float station_raw_ore_need_score(const station_t *st, commodity_t ore);

/* Short, human-readable display name for a station index — "Prospect",
 * "Kepler", "Helios" for the three founding stations, or the actual
 * station name truncated to the first word for outposts. Returns "?"
 * for invalid / negative indices.
 *
 * Used by the docked trade UI's lineage display ("from Prospect ep N").
 * Falls back gracefully when world state isn't available (e.g., tests
 * that call this without a populated world_t). */
const char *  station_short_name(int station_idx);

#endif
