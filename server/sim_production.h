/*
 * sim_production.h -- Material flow, smelting, and station production.
 * Extracted from game_sim.c.
 */
#ifndef SIM_PRODUCTION_H
#define SIM_PRODUCTION_H

#include "game_sim.h"
#include "mining.h"

/* Production API — called from world_sim_step */
void step_furnace_smelting(world_t *w, float dt);
void sim_step_refinery_production(world_t *w, float dt);
void sim_step_station_production(world_t *w, float dt);
void step_module_flow(world_t *w, float dt);

/* Helpers used by contract system in game_sim.c */
bool sim_can_smelt_ore(const station_t *st, commodity_t ore);

/* Roll a fragment's RATi grade. Universe-keyed, called once at the
 * moment of fracture from sim_asteroid.c. The result is public and
 * stable for the rock's lifetime — the in-world dot reveals it. */
mining_grade_t sim_roll_fragment_grade(const asteroid_t *a);

/*
 * Already declared in game_sim.h:
 *   step_module_delivery
 */

#endif /* SIM_PRODUCTION_H */
