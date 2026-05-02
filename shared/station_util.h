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
int           station_max_ring(const station_t *st);
int           station_spawn_fee(const station_t *st);
bool          station_consumes(const station_t *st, commodity_t c);
bool          station_produces(const station_t *st, commodity_t c);
void          rebuild_station_services(station_t *st);

/* Count active (non-scaffold) MODULE_FURNACE modules. The count drives
 * the smelt-tier rules below. */
int           station_furnace_count(const station_t *st);

/* Count-tier smelt capability, shared between sim + client (the client
 * uses it to label dock UI rows and station persona text):
 *   1 furnace  → ferrite only
 *   2 furnaces → ferrite + cuprite
 *   3+ furnaces → cuprite + crystal (ferrite blocked)
 * Always requires ≥1 hopper. */
bool          station_can_smelt(const station_t *st, commodity_t ore);

/* Display / trade derivations from the dominant module. */
module_type_t station_dominant_module(const station_t *st);
commodity_t   station_primary_buy(const station_t *st);
commodity_t   station_primary_sell(const station_t *st);

/* ----- Ring rotation / module world geometry ----- */
float         station_ring_rotation(const station_t *st, int ring);
vec2          module_world_pos_ring(const station_t *st, int ring, int slot);
float         module_angle_ring   (const station_t *st, int ring, int slot);
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
 * input commodities (per module_required_inputs) have a hopper
 * tagged with that commodity somewhere on the station. With
 * commodity-tagged hoppers, the station-wide check replaces the
 * earlier slot-specific cross-ring lookup. */
bool          station_pair_satisfied(const station_t *st, int ring, int slot,
                                     module_type_t type);

/* Find the index of the hopper module on `st` whose commodity tag
 * matches `commodity`. Returns -1 if none. */
int           station_find_hopper_for(const station_t *st, commodity_t commodity);

#endif
