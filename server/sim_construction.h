/*
 * sim_construction.h -- Station construction: module placement, activation,
 * and outpost founding.  Extracted from game_sim.c.
 */
#ifndef SIM_CONSTRUCTION_H
#define SIM_CONSTRUCTION_H

#include "game_sim.h"

/* Helpers shared with other sim_*.c files */
commodity_t module_build_material(module_type_t type);
float       module_build_cost(module_type_t type);
bool        station_sells_scaffold(const station_t *st, module_type_t type);

/* Construction API — called from world_sim_step and NPC code */
void add_module_at(station_t *st, module_type_t type, uint8_t arm, uint8_t chain_pos);
/* Place a hopper with an explicit commodity tag. Bypasses the
 * auto-solver — used by seed code and tests that need deterministic
 * hopper layouts. */
void add_hopper_for(station_t *st, uint8_t arm, uint8_t chain_pos, commodity_t c);
void step_module_activation(world_t *w, float dt);

/*
 * Already declared in game_sim.h (kept there for backward compat):
 *   activate_outpost, begin_module_construction, begin_module_construction_at
 *
 * rebuild_station_services lives in shared/station_util.h (inline).
 */

#endif /* SIM_CONSTRUCTION_H */
