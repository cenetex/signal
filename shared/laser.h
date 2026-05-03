/*
 * laser.h — unified laser-beam primitive.
 *
 * Companion to shared/tractor.h. Where a tractor transfers momentum
 * (force between two anchor points), a laser transfers energy: a
 * directed ray from a source delivers a per-second effect on whatever
 * it hits, with no force on the target and no recoil on the source.
 *
 * The primitive separates two concerns that are conflated today:
 *   - Geometry: is a target inside the beam's range / cone? Where
 *     does the beam visually terminate on the target's surface?
 *   - Effect: when the beam is on, accumulate per-second value on a
 *     scalar field (hp, smelt_progress, etc.); when the beam is off,
 *     optionally decay back toward zero or hold.
 *
 * Both server and client can call this — geometry is shareable for
 * predictive simulation (client extrapolates "where the beam will
 * land next tick" between snapshots) and the effect helper is pure
 * float math.
 *
 * MIGRATED SITES:
 *   server/sim_production.c::step_furnace_smelting   smelt_progress
 *                                                    accumulator + decay
 *   server/sim_mining.c::sim_mining_beam_step        per-tick HP
 *                                                    damage on asteroids
 *
 * DELIBERATELY NOT MIGRATED:
 *   - server/game_sim.c::find_scan_target — pure ray-cast for HUD
 *     targeting. No effect accumulation; only target identification.
 *     Could share laser_target_in_cone() if the geometry duplicates
 *     enough to be worth folding in (R3+).
 *   - server/game_sim.c scaffold-snap trigger — checks proximity at
 *     beam_end to flip a state machine. Not an effect accumulator.
 *
 * This refactor does NOT add new gameplay (combat lasers, heat,
 * jamming, etc.) — it just consolidates the existing laser-shaped
 * code paths so future additions plug into one model.
 */
#ifndef SHARED_LASER_H
#define SHARED_LASER_H

#include <stdbool.h>
#include "math_util.h"

/* Geometry of a single laser beam.
 *
 * The ray fires from `source_pos` along `source_dir` (must be a unit
 * vector). `range` clamps how far the beam reaches. `cone_half_angle`
 * controls beam width: 0 = thin ray (only targets exactly on the line
 * register a hit), >0 = cone with a tolerance window.
 *
 * Helpers below treat targets as circles (pos + radius). For a thin
 * ray, the test is "perpendicular distance from line ≤ target radius".
 * For a cone, the test is "angle between source_dir and source→target
 * is within cone_half_angle". Either way, the target must also be
 * within `range` along the line of action. */
typedef struct {
    vec2  source_pos;
    vec2  source_dir;          /* unit vector */
    float range;
    float cone_half_angle;     /* radians; 0 = thin ray */
} laser_ray_t;

/* True if a target circle (`target_pos`, `target_radius`) is inside
 * the beam's range and cone. When non-NULL, `*out_hit_pos` is filled
 * with the visible-beam termination point (target surface, biased
 * toward the source by 85% of target_radius for a clean visual).
 *
 * Doesn't apply any effect — pure geometry check. The caller decides
 * what to do with the hit: accumulate damage, advance progress, etc. */
bool laser_target_in_beam(const laser_ray_t *ray,
                          vec2 target_pos,
                          float target_radius,
                          vec2 *out_hit_pos);

/* Per-tick effect accumulation on a scalar field (hp, smelt_progress,
 * fab_charge, etc.).
 *
 *   *target_field += effect_per_sec * dt
 *
 * Then clamps the result to [0, max_value] iff `max_value > 0`. Pass
 * `max_value = 0` for unclamped accumulation (e.g. taking damage off
 * an HP value where the floor matters but no upper bound is defined).
 *
 * Negative `effect_per_sec` decays the field toward zero — used by
 * smelt_progress when a fragment leaves the beam. */
void laser_apply_effect(float *target_field,
                        float effect_per_sec,
                        float max_value,
                        float dt);

#endif /* SHARED_LASER_H */
