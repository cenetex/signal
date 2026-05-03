/*
 * tractor.c — single-function implementation of the unified tractor
 * beam primitive. See tractor.h for the model.
 */
#include "tractor.h"
#include <math.h>

bool tractor_apply(const tractor_anchor_t *src,
                   const tractor_anchor_t *tgt,
                   const tractor_beam_t   *beam,
                   float dt) {
    if (!src || !tgt || !beam) return false;

    vec2 to_target = v2_sub(tgt->pos, src->pos);
    float d_sq = v2_len_sq(to_target);

    /* Range gate. Test in squared form to avoid sqrt when disengaged. */
    if (beam->range > 0.0f && d_sq > beam->range * beam->range) return false;

    /* Co-located bodies: report active but apply no force this tick.
     * Avoids divide-by-zero on the line-of-action unit vector. */
    float d = sqrtf(d_sq);
    const float epsilon = 1e-4f;
    if (d < epsilon) return true;

    vec2 dir = v2_scale(to_target, 1.0f / d);  /* unit src→tgt */

    /* Signed spring on stretch. Pull when d > rest, push when d < rest.
     * The two strength knobs let a beam be asymmetric (pure pull, pure
     * push, or any mix). spring_mag is signed in dir's frame: negative
     * = toward source (pull), positive = away from source (push). */
    float stretch = d - beam->rest_length;
    float spring_mag;
    if (stretch > 0.0f)       spring_mag = -beam->pull_strength * stretch;
    else if (stretch < 0.0f)  spring_mag = -beam->push_strength * stretch;
    else                      spring_mag = 0.0f;

    /* Optional linear falloff: strength scales by (1 - d/range). */
    if (beam->falloff == TRACTOR_FALLOFF_LINEAR && beam->range > 0.0f) {
        float scale = 1.0f - d / beam->range;
        if (scale < 0.0f) scale = 0.0f;
        spring_mag *= scale;
    }

    /* Target must be body-attached — there's nothing to do otherwise. */
    if (!tgt->vel) return true;

    /* Relative velocity along and perpendicular to the beam line.
     * src->vel may be NULL (world-pinned) — treat its velocity as zero. */
    vec2 src_vel = (src->vel) ? *src->vel : v2(0.0f, 0.0f);
    vec2 rel_vel = v2_sub(*tgt->vel, src_vel);
    float v_along = v2_dot(rel_vel, dir);
    vec2 v_tangent = v2_sub(rel_vel, v2_scale(dir, v_along));

    /* Axial: spring + axial damping. Tangent: damping only. */
    vec2 axial_force   = v2_scale(dir, spring_mag - beam->axial_damping * v_along);
    vec2 tangent_force = v2_scale(v_tangent, -beam->tangent_damping);
    vec2 force = v2_add(axial_force, tangent_force);

    /* Apply impulse to target. Caller treats target as unit mass —
     * tuning constants are accel directly, not force. */
    *tgt->vel = v2_add(*tgt->vel, v2_scale(force, dt));

    /* Speed cap (post-impulse, isotropic on target's absolute velocity). */
    if (beam->speed_cap > 0.0f) {
        float spd_sq = v2_len_sq(*tgt->vel);
        float cap_sq = beam->speed_cap * beam->speed_cap;
        if (spd_sq > cap_sq) {
            float spd = sqrtf(spd_sq);
            *tgt->vel = v2_scale(*tgt->vel, beam->speed_cap / spd);
        }
    }

    /* Newton's third on source when body-attached. inv_mass scales the
     * reaction so heavier sources feel less of it (matches the
     * apply_band_force precedent of dividing by BAND_SHIP_MASS). */
    if (src->vel && src->inv_mass > 0.0f) {
        *src->vel = v2_sub(*src->vel, v2_scale(force, dt * src->inv_mass));
    }
    return true;
}
