/*
 * tractor.h — unified tractor-beam primitive.
 *
 * One function (tractor_apply) replaces six hand-rolled "apply force
 * between two anchor points" sites that previously lived in sim_ai,
 * game_sim, and sim_production. Lives in shared/ so client predictive
 * simulation can call the same code as the server's authoritative tick.
 *
 * Two strength components per side (each can be zero):
 *   - *_strength: spring term, force per unit of stretch from rest.
 *     Linear-in-distance, settles to equilibrium at rest_length.
 *   - *_constant: always-on term, fixed-magnitude force whenever the
 *     beam is on the corresponding side of rest. Models a "thruster
 *     on the rope" — fragment yanks in regardless of distance.
 *
 * Two damping components, decoupled:
 *   - axial_damping: opposes velocity along the beam line.
 *   - tangent_damping: opposes velocity perpendicular to it.
 *
 * Plus: range gate (d > range disables the whole beam, including
 * damping — set range=0 if you want damping unconditional), optional
 * speed cap on target, and TRACTOR_FALLOFF_LINEAR for `(1 - d/range)`
 * scaling of strength.
 *
 * Newton's third applies automatically when the source anchor is
 * body-attached (vel pointer + nonzero inv_mass); world-pinned
 * anchors (NULL vel, inv_mass = 0) act as infinite-mass attachment
 * points and skip reaction.
 *
 * MIGRATED SITES:
 *   server/game_sim.c::apply_band_force         (player tow)
 *   server/sim_ai.c   step_npc_ships RETURN     (NPC fragment tow)
 *   server/sim_ai.c   step_scaffold_tow_contract (worker scaffold tow)
 *   server/sim_production.c::step_furnace_smelting   (smelt beam pull)
 *   server/game_sim.c::step_station_cargo_pod_tractors (module pod tow)
 *   server/game_sim.c::step_scaffolds LOOSE     (planned blueprint pull)
 *   server/game_sim.c::step_scaffolds SNAPPING  (module slot snap)
 *
 * DELIBERATELY NOT MIGRATED (different shape, future work):
 *   - step_scaffolds orbital vortex (loose-near-station orbit) —
 *     tangential orbit + radial pull, not a point-anchor primitive.
 *   - step_station_ring_dynamics spokes — angular variant; needs
 *     rings-as-bodies first.
 *   - step_fragment_collection (player tractor pickup detection) —
 *     state machine deciding which fragments to attach, not a force
 *     application.
 *   - All laser sites (mining laser, smelt_progress accumulator,
 *     future damage lasers) — the laser primitive (energy delivery
 *     along a ray, no momentum) is a separate refactor.
 *
 * TUNING NOTE (tangent_damping):
 *   The user's design intent for the 1D-damping refactor was
 *   "axial damping along the rope only, with small or zero tangent
 *   damping for natural swing." In practice, several migrated sites
 *   had to keep tangent_damping near axial value to satisfy existing
 *   integration tests that were tuned around the legacy isotropic
 *   drag (`vel *= 1/(1+k*dt)`). Specifically: NPC pickup tow,
 *   blueprint pull, and slot snap keep specialized profiles. Standard
 *   ship, hopper, furnace, and cargo-pod tow bands share the preset below
 *   at axial=1.8 and tangent=1.1. Heavy scaffolds use this same profile
 *   with body-specific drag and tow-speed limits.
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
 * anchor is world-pinned. Kinematic station anchors still provide their
 * live ring velocity here, with zero inverse mass. When non-NULL and
 * `inv_mass > 0`, the anchor receives Newton's-third reaction force.
 *
 * `inv_mass` = 1 / mass. Zero models an immovable / infinite-mass
 * anchor. Both dynamic endpoints scale the shared beam force by their
 * inverse mass, so unequal bodies receive equal-and-opposite momentum. */
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
 *   - *_strength is the spring term: force per unit of stretch from
 *     rest. Linear in distance, settles to equilibrium at rest.
 *   - *_constant is the always-on term: a fixed force that engages
 *     whenever the beam is on the corresponding side of rest. Models
 *     a "thruster on the rope" — fragment yanks in regardless of how
 *     far away it is.
 * A site can use either or both. Player tow is pure spring; NPC
 * fragment pickup is pure constant. */
typedef struct {
    float rest_length;       /* happy distance; 0 = pull-toward-source */
    float pull_strength;     /* spring force per unit (d - rest) when d > rest */
    float push_strength;     /* spring force per unit (rest - d) when d < rest */
    float pull_constant;     /* constant pull force when d > rest */
    float push_constant;     /* constant push force when d < rest */
    float range;             /* d > range → no force at all this tick */
    float axial_damping;     /* force per unit along-beam relative velocity */
    float tangent_damping;   /* force per unit perpendicular relative velocity */
    float speed_cap;         /* optional |target.vel| cap after impulse; 0 = no cap */
    tractor_falloff_t falloff;
} tractor_beam_t;

/* A fully resolved tractor connection for one simulation tick.  Keeping the
 * endpoints and force profile together gives ships, stations, hoppers, docks,
 * and furnaces one physical contract: both endpoints have world position and
 * velocity, even when the source has infinite mass. */
typedef struct {
    tractor_anchor_t source;
    tractor_anchor_t target;
    tractor_beam_t beam;
} tractor_link_t;

/* Standard tractor-beam force profile for ship and station elastic tows.
 * Callers may choose the attachment geometry (range and rest length), but
 * ships, hoppers, docks, and production modules all share the same spring,
 * damping, and falloff. A rest length of zero makes a fixed station anchor
 * pull a pod directly onto its assigned hold point. */
#define TRACTOR_TOW_BAND_REST_LENGTH      80.0f
#define TRACTOR_TOW_BAND_SPRING_K          4.0f
#define TRACTOR_TOW_BAND_AXIAL_DAMPING     1.8f
#define TRACTOR_TOW_BAND_TANGENT_DAMPING   1.1f

/* A taut field line still carries a visible traveling ripple. Fully
 * flattening the wave made cargo pods, which normally ride near the outer
 * tow band, look like they had fallen back to the old straight cable even
 * though they were using the canonical renderer. Tension is communicated by
 * the brighter straight core; it must not erase the tractor signature. */
#define TRACTOR_TETHER_TAUT_WAVE_FLOOR      0.55f

static inline float tractor_tether_wave_scale(float tautness) {
    float taut = clampf(tautness, 0.0f, 1.0f);
    return 1.0f - taut * (1.0f - TRACTOR_TETHER_TAUT_WAVE_FLOOR);
}

static inline tractor_beam_t tractor_tow_beam(float range,
                                               float rest_length) {
    return (tractor_beam_t){
        .rest_length     = rest_length,
        .pull_strength   = TRACTOR_TOW_BAND_SPRING_K,
        .push_strength   = TRACTOR_TOW_BAND_SPRING_K,
        .pull_constant   = 0.0f,
        .push_constant   = 0.0f,
        .range           = range,
        .axial_damping   = TRACTOR_TOW_BAND_AXIAL_DAMPING,
        .tangent_damping = TRACTOR_TOW_BAND_TANGENT_DAMPING,
        .speed_cap       = 0.0f,
        .falloff         = TRACTOR_FALLOFF_CONSTANT,
    };
}

static inline bool tractor_beam_points_in_range(vec2 src,
                                                vec2 tgt,
                                                const tractor_beam_t *beam) {
    if (!beam) return false;
    if (beam->range <= 0.0f) return true;
    return v2_dist_sq(src, tgt) <= beam->range * beam->range;
}

static inline float tractor_beam_range_fraction(vec2 src,
                                                vec2 tgt,
                                                const tractor_beam_t *beam) {
    if (!beam) return 0.0f;
    if (beam->range <= 0.0f) return 1.0f;
    float d_sq = v2_dist_sq(src, tgt);
    float range_sq = beam->range * beam->range;
    if (d_sq >= range_sq) return 0.0f;
    float d = v2_len(v2_sub(tgt, src));
    return clampf(1.0f - d / beam->range, 0.0f, 1.0f);
}

/* Visual/load tension for an elastic tow: slack at or inside rest length,
 * fully taut at range. Keeping this beside the force profile prevents ship
 * and fixed-module renderers from inventing different beam semantics. */
static inline float tractor_beam_tautness(vec2 src,
                                          vec2 tgt,
                                          const tractor_beam_t *beam) {
    if (!beam) return 0.0f;
    float d = v2_len(v2_sub(tgt, src));
    float span = beam->range - beam->rest_length;
    if (span <= 0.0f) return d > beam->rest_length ? 1.0f : 0.0f;
    return clampf((d - beam->rest_length) / span, 0.0f, 1.0f);
}

/* Apply one beam tick. Returns true iff the beam was active
 * (target was within `range`). Mutates a velocity only when its anchor has
 * a non-NULL vel pointer and positive inv_mass. */
bool tractor_apply(const tractor_anchor_t *src,
                   const tractor_anchor_t *tgt,
                   const tractor_beam_t   *beam,
                   float dt);

/* Apply a resolved link. This is the canonical entry point for gameplay
 * tractors; tractor_apply remains available for low-level tests and custom
 * force profiles. */
bool tractor_link_apply(const tractor_link_t *link, float dt);

#endif /* SHARED_TRACTOR_H */
