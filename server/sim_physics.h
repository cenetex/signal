/*
 * sim_physics.h -- Asteroid physics: N-body gravity, asteroid-asteroid
 * collision, and asteroid-station collision.  Extracted from game_sim.c.
 */
#ifndef SIM_PHYSICS_H
#define SIM_PHYSICS_H

#include "game_sim.h"

/* Physics API — called from world_sim_step */
void step_asteroid_gravity(world_t *w, float dt);
void resolve_asteroid_collisions(world_t *w);
void resolve_asteroid_station_collisions(world_t *w);

#endif /* SIM_PHYSICS_H */
