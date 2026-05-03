/*
 * tractor.h — unified tractor-beam primitive.
 *
 * One function (tractor_apply) replaces the half-dozen hand-rolled
 * "apply force between two anchor points" sites scattered across
 * sim_ai, game_sim, and sim_production. Lives in shared/ so client
 * predictive simulation can call the same code as the server's
 * authoritative tick.
 *
 * The shape: each beam has a rest length, a pull strength (force per
 * unit stretch when d > rest), a push strength (force per unit
 * compression when d < rest), an outer range gate, axial damping
 * along the beam line, tangent damping perpendicular to it, an
 * optional speed cap, and an optional linear-falloff curve. Newton's
 * third applies automatically when the source anchor is body-attached
 * (vel pointer + nonzero inv_mass); world-pinned anchors (NULL vel,
 * inv_mass = 0) act as infinite-mass attachment points.
 *
 * Migration sites (filled in as each commit lands):
 *   - server/game_sim.c::apply_band_force        (player tow)
 *   - server/sim_ai.c   NPC_STATE_RETURN_TO_STATION (NPC fragment tow)
 *   - server/sim_ai.c   step_tow_drone NPC_STATE_TRAVEL_TO_DEST (scaffold tow)
 *   - server/sim_production.c::step_furnace_smelting (smelt beam pull)
 *   - server/game_sim.c::step_scaffolds blueprint pull
 *   - server/game_sim.c::step_scaffolds module slot snap
 *
 * Deliberately NOT migrated (different shape, future work):
 *   - step_scaffolds orbital vortex      — tangential orbit + radial
 *                                          pull, not a point-anchor.
 *   - step_station_ring_dynamics spokes  — angular variant; needs
 *                                          rings-as-bodies first.
 *   - step_fragment_collection           — state machine, not a
 *                                          force application.
 *   - all laser sites (mining, smelt-progress, future damage) — the
 *                                          laser primitive is a
 *                                          separate refactor.
 */
#ifndef SHARED_TRACTOR_H
#define SHARED_TRACTOR_H

#include <stdbool.h>
#include "math_util.h"

/* An anchor point for one end of a tractor beam.
 *
 * `pos` is in world coordinates. The caller is responsible for
 * computing it from any body+offset transform (rotation-aware
 * computation lives at the call site, not here).
 *
 * `vel` is a pointer into the body's velocity vector. NULL means the
 * anchor is world-pinned (e.g. the smelt beam's furnace-silo midpoint,
 * which doesn't belong to any single body). When non-NULL and
 * `inv_mass > 0`, the anchor receives Newton's-third reaction force.
 *
 * `inv_mass` = 1 / mass. Zero models an immovable / infinite-mass
 * anchor. */
typedef struct {
    vec2  pos;
    vec2 *vel;
    float inv_mass;
} tractor_anchor_t;

/* How beam strength scales with distance inside the active range. */
typedef enum {
    TRACTOR_FALLOFF_CONSTANT = 0,   /* uniform: force = strength · stretch */
    TRACTOR_FALLOFF_LINEAR   = 1,   /* force scaled by (1 - d/range) */
} tractor_falloff_t;

/* The beam itself — a tuning bundle with no per-call mutable state.
 * Sites typically declare a static const beam config near the call.
 *
 * Two strength components on each side, summed:
 *   - *_strength is the spring term: accel per unit of stretch from
 *     rest. Linear in distance, settles to equilibrium at rest.
 *   - *_constant is the always-on term: a fixed accel that engages
 *     whenever the beam is on the corresponding side of rest. Models
 *     a "thruster on the rope" — fragment yanks in regardless of how
 *     far away it is.
 * A site can use either or both. Player tow is pure spring; NPC
 * fragment pickup is pure constant. */
typedef struct {
    float rest_length;       /* happy distance; 0 = pull-toward-source */
    float pull_strength;     /* spring: accel per unit (d - rest) when d > rest */
    float push_strength;     /* spring: accel per unit (rest - d) when d < rest */
    float pull_constant;     /* constant pull magnitude when d > rest */
    float push_constant;     /* constant push magnitude when d < rest */
    float range;             /* d > range → no force at all this tick */
    float axial_damping;     /* accel per unit along-beam relative velocity */
    float tangent_damping;   /* accel per unit perpendicular-to-beam relative velocity */
    float speed_cap;         /* optional |target.vel| cap after impulse; 0 = no cap */
    tractor_falloff_t falloff;
} tractor_beam_t;

/* Apply one beam tick. Returns true iff the beam was active
 * (target was within `range`). Mutates tgt->vel always; mutates
 * src->vel iff src->vel != NULL && src->inv_mass > 0. */
bool tractor_apply(const tractor_anchor_t *src,
                   const tractor_anchor_t *tgt,
                   const tractor_beam_t   *beam,
                   float dt);

#endif /* SHARED_TRACTOR_H */
