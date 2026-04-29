/*
 * sim_mining.h — Shared mining-beam primitives.
 *
 * Slice 2 of the #294 character_t unification: extract the per-tick mining
 * laser logic into one helper so the player path (`step_mining_system`)
 * and the NPC AI path (`NPC_STATE_MINING`) apply identical range, cone,
 * tier, signal, and damage rules. Before this, NPCs measured range from
 * rock CENTER without an aim cone and never exited MINING state on
 * distance — so a shoved hauler kept drawing a beam to its target across
 * the whole world.
 *
 * Caller still owns: target acquisition (player uses hover_asteroid +
 * scan fallback; NPC uses target_asteroid + steering), ship-state
 * mutations, and event emission. This module only owns "given a candidate
 * target, what does one tick of beam fire do?"
 */
#ifndef SIM_MINING_H
#define SIM_MINING_H

#include <stdbool.h>
#include "game_sim.h"  /* world_t lives here, not in shared/types.h */

typedef struct {
    bool fired;          /* beam was in range, in cone, tier OK; damage applied this tick */
    bool ineffective;    /* beam hit a rock but the laser tier is too low to chip it */
    bool fractured;      /* this tick drove hp to zero and called fracture_asteroid */
    bool hit;            /* beam_end terminates on a target (vs free space) */
    vec2 beam_end;       /* surface hit point, or muzzle + forward·MINING_RANGE when no hit */
    vec2 hit_normal;     /* outward normal at hit point (zero when !hit) */
} mining_beam_t;

/* Acquire the best in-cone in-range mineable asteroid from `origin`
 * facing `forward`. Returns its index or -1 if nothing's in the cone.
 *
 * Same semantics the player uses (surface distance ≤ MINING_RANGE,
 * perpendicular ≤ asteroid radius). The NPC path used to enter MINING
 * when center-distance < MINING_RANGE without a cone — much looser. */
int sim_mining_pick_target(const world_t *w, vec2 origin, vec2 forward);

/* Apply one tick of mining beam fire. Validates range/cone/tier, applies
 * signal-scaled damage to `world->asteroids[target_idx]`, fractures it
 * when hp drops to zero. `fracturer_id` is the player slot id used for
 * fracture-claim attribution; pass -1 for NPC fire. Returns beam render
 * state so the caller can publish beam_start/beam_end/beam_hit. */
mining_beam_t sim_mining_beam_step(world_t *w, vec2 muzzle, vec2 forward,
                                    int target_idx, int mining_level,
                                    float mining_rate, float signal_eff,
                                    int8_t fracturer_id, float dt);

#endif /* SIM_MINING_H */
