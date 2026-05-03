/*
 * laser.c — geometry + effect helpers for the unified laser primitive.
 * See laser.h for the conceptual model.
 */
#include "laser.h"
#include <math.h>

bool laser_target_in_beam(const laser_ray_t *ray,
                          vec2 target_pos,
                          float target_radius,
                          vec2 *out_hit_pos,
                          float *out_along_dist) {
    if (!ray) return false;

    vec2 to_target = v2_sub(target_pos, ray->source_pos);
    float along = v2_dot(to_target, ray->source_dir);

    /* Behind the source — never a hit. */
    if (along < 0.0f) return false;

    /* Range gate. Clip on the closest approach point (along the line
     * of action), not the target's center, so a wide target straddling
     * the range edge still registers. */
    float effective_range = ray->range + target_radius;
    if (along > effective_range) return false;

    /* Perpendicular distance from the ray axis to the target center. */
    vec2 axis_point  = v2_add(ray->source_pos, v2_scale(ray->source_dir, along));
    vec2 perp        = v2_sub(target_pos, axis_point);
    float perp_dist  = sqrtf(v2_len_sq(perp));

    /* Cone test. A thin ray (half_angle == 0) collapses to a strict
     * "perpendicular distance ≤ target_radius" check; a cone widens
     * the tolerance with distance from source. */
    float allowed_perp = target_radius;
    if (ray->cone_half_angle > 0.0f) {
        allowed_perp += along * tanf(ray->cone_half_angle);
    }
    if (perp_dist > allowed_perp) return false;

    /* Hit. Termination point biased 85% into the target so the visible
     * beam clearly lands on its surface rather than passing through. */
    if (out_hit_pos) {
        vec2 to_target_unit = (along > 1e-6f)
            ? v2_scale(to_target, 1.0f / sqrtf(v2_len_sq(to_target)))
            : ray->source_dir;
        *out_hit_pos = v2_sub(target_pos,
                              v2_scale(to_target_unit, target_radius * 0.85f));
    }
    if (out_along_dist) *out_along_dist = along;
    return true;
}

void laser_apply_effect(float *target_field,
                        float effect_per_sec,
                        float max_value,
                        float dt) {
    if (!target_field) return;
    *target_field += effect_per_sec * dt;
    if (*target_field < 0.0f) *target_field = 0.0f;
    if (max_value > 0.0f && *target_field > max_value) {
        *target_field = max_value;
    }
}
