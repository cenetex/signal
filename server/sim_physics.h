/*
 * sim_physics.h -- Asteroid physics: N-body gravity, asteroid-asteroid
 * collision, and asteroid-station collision.  Extracted from game_sim.c.
 */
#ifndef SIM_PHYSICS_H
#define SIM_PHYSICS_H

#include "game_sim.h"

/* Rock throws keep combat ownership for a short, quantized window.
 * Smelt provenance remains on last_towed_token; only this state credits
 * thrown-rock damage and kills. */
#define ROCK_THROW_BALLISTIC_SECONDS 6.0f
#define ASTEROID_THROW_TIMER_TICKS 12u  /* 120 Hz sim -> 0.1s timer quantum */

/* Physics API — called from world_sim_step */
void step_asteroid_gravity(world_t *w, float dt);
void resolve_asteroid_collisions(world_t *w);
void resolve_asteroid_station_collisions(world_t *w);

void asteroid_mark_thrown(asteroid_t *a, const uint8_t token[8], float seconds);
void asteroid_clear_thrown(asteroid_t *a);
bool asteroid_is_ballistic(const asteroid_t *a);
void asteroid_step_thrown_state(world_t *w);

#endif /* SIM_PHYSICS_H */
