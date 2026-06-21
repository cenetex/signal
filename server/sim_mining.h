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
    bool fired;          /* beam was in range, in cone, laser OK; damage applied this tick */
    bool ineffective;    /* beam hit a rock but laser tier/material gate refused damage */
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

/* True when the candidate asteroid is active, non-collectible, and the
 * mining ray intersects its surface within MINING_RANGE. Uses the same
 * surface-distance geometry as sim_mining_pick_target so server damage
 * and client NPC beam rendering agree on when a beam may exist. */
bool sim_mining_target_hit(vec2 muzzle, vec2 forward,
                           const asteroid_t *asteroid,
                           vec2 *out_hit, vec2 *out_normal);

/* Apply one tick of mining beam fire. Validates range/cone/laser gates, applies
 * signal-scaled damage to `world->asteroids[target_idx]`, fractures it
 * when hp drops to zero. `fracturer_id` is the player slot id used for
 * fracture-claim attribution; pass -1 for NPC fire. Returns beam render
 * state so the caller can publish beam_start/beam_end/beam_hit. */
mining_beam_t sim_mining_beam_step(world_t *w, vec2 muzzle, vec2 forward,
                                    int target_idx, int mining_level,
                                    float mining_rate, float signal_eff,
                                    int8_t fracturer_id, float dt);

/* Slack variant for explicit player mining target hints only. Multiplayer
 * hints are accepted with a small aim tolerance to absorb render/sim
 * desync on fast fracture children; NPC fire and renderer prediction
 * should use the strict wrapper above. */
mining_beam_t sim_mining_beam_step_with_aim_slack(world_t *w, vec2 muzzle,
                                                   vec2 forward, int target_idx,
                                                   int mining_level,
                                                   float mining_rate,
                                                   float signal_eff,
                                                   int8_t fracturer_id,
                                                   float dt,
                                                   float aim_slack);

#endif /* SIM_MINING_H */
