/*
 * sim_asteroid.h -- Asteroid lifecycle: spawning, fracture, field
 * maintenance, and per-frame dynamics.  Extracted from game_sim.c.
 */
#ifndef SIM_ASTEROID_H
#define SIM_ASTEROID_H

#include <string.h>

#include "game_sim.h"
#include "chunk.h"

/* Asteroid API — called from world_sim_step and NPC code */
void sim_step_asteroid_dynamics(world_t *w, float dt);
void maintain_asteroid_field(world_t *w, float dt);
void step_fracture_claims(world_t *w);
bool submit_fracture_claim(world_t *w, int player_id, uint32_t fracture_id,
                           uint32_t burst_nonce, uint8_t claimed_grade);

/* Zero out a claim state. Shared between sim_asteroid.c (birth/clear)
 * and sim_production.c (smelt completion) so both call sites agree
 * the struct has no hidden reset semantics. */
static inline void fracture_claim_state_reset(fracture_claim_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

/* Chunk materialization — places a chunk_asteroid_t into a world slot */
void materialize_asteroid(world_t *w, int slot, const chunk_asteroid_t *ca,
                           int32_t cx, int32_t cy);

/* Field seeding (legacy — used by some tests) */
int  seed_asteroid_clump(world_t *w, int first_slot);
void seed_field_asteroid_of_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier);
void seed_random_field_asteroid(world_t *w, asteroid_t *a);

/*
 * Already declared in game_sim.h:
 *   fracture_asteroid
 */

#endif /* SIM_ASTEROID_H */
