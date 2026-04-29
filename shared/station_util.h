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

#endif
