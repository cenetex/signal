/*
 * sim_mining.c — see sim_mining.h.
 *
 * Single source of truth for mining-beam range/cone/tier/damage rules.
 * Both `step_mining_system` (player) and the NPC `NPC_STATE_MINING`
 * branch funnel through `sim_mining_beam_step`; the previous parallel
 * implementations diverged on:
 *   - range metric: surface distance (player) vs center distance (NPC)
 *   - aim cone:     enforced (player) vs none (NPC)
 *   - signal scaling: applied (player) vs ignored (NPC)
 *   - tier gate:   enforced (player) vs ignored (NPC)
 * NPCs were therefore mining at any distance, beaming through walls,
 * and ignoring weak signal — visible as "NPC ship lasering very far".
 */
#include "sim_mining.h"
#include "laser.h"
#include "game_sim.h"      /* MINING_RANGE, mining gates, fracture_asteroid */
#include "sim_asteroid.h"  /* asteroid_is_collectible */
#include <math.h>

int sim_mining_pick_target(const world_t *w, vec2 origin, vec2 forward) {
    int best = -1;
    float best_dist = MINING_RANGE + 1.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || asteroid_is_collectible(a)) continue;
        vec2 to_a = v2_sub(a->pos, origin);
        float proj = v2_dot(to_a, forward);
        float perp = fabsf(v2_cross(to_a, forward));
        if (perp > a->radius) continue;
        float surface_dist = proj - fixp_sqrtf(fmaxf(0.0f, a->radius * a->radius - perp * perp));
        if (surface_dist < -a->radius) continue;
        if (surface_dist > MINING_RANGE) continue;
        if (surface_dist < best_dist) { best_dist = surface_dist; best = i; }
    }
    return best;
}

static bool mining_target_hit_with_slack(vec2 muzzle, vec2 forward,
                                         const asteroid_t *a,
                                         float aim_slack,
                                         vec2 *out_hit,
                                         vec2 *out_normal) {
    if (!a || !a->active || asteroid_is_collectible(a)) return false;
    float effective_radius = a->radius + fmaxf(0.0f, aim_slack);

    vec2 to_a = v2_sub(a->pos, muzzle);
    float proj = v2_dot(to_a, forward);
    float perp = fabsf(v2_cross(to_a, forward));
    if (perp > effective_radius) return false;

    float chord = fixp_sqrtf(fmaxf(0.0f, effective_radius * effective_radius - perp * perp));
    float surface_dist = proj - chord;
    if (surface_dist < -effective_radius) return false;
    if (surface_dist > MINING_RANGE) return false;

    vec2 normal = v2_norm(to_a);
    if (out_hit) {
        *out_hit = v2_sub(a->pos, v2_scale(normal, a->radius * 0.85f));
    }
    if (out_normal) *out_normal = normal;
    return true;
}

bool sim_mining_target_hit(vec2 muzzle, vec2 forward,
                           const asteroid_t *a,
                           vec2 *out_hit, vec2 *out_normal) {
    return mining_target_hit_with_slack(muzzle, forward, a, 0.0f,
                                        out_hit, out_normal);
}

mining_beam_t sim_mining_beam_step_with_aim_slack(world_t *w, vec2 muzzle,
                                                   vec2 forward, int target_idx,
                                                   int mining_level,
                                                   float mining_rate,
                                                   float signal_eff,
                                                   int8_t fracturer_id,
                                                   float dt,
                                                   float aim_slack) {
    mining_beam_t r = {
        .fired = false, .ineffective = false, .fractured = false,
        .hit = false,
        .beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE)),
        .hit_normal = v2(0.0f, 0.0f),
    };

    if (target_idx < 0 || target_idx >= MAX_ASTEROIDS) return r;
    asteroid_t *a = &w->asteroids[target_idx];
    if (!a->active || asteroid_is_collectible(a)) return r;

    if (!mining_target_hit_with_slack(muzzle, forward, a, aim_slack,
                                      &r.beam_end, &r.hit_normal))
        return r;
    r.hit = true;

    /* Laser gate: beam too weak to chip this rock. Beam still hits (so
     * the visual tells the player "I'm pointed at it") but applies no
     * damage and reports back so the caller can flash the warning. */
    if (!mining_level_can_fracture_asteroid(mining_level, a)) {
        r.ineffective = true;
        return r;
    }

    /* Damage delivered as a negative laser_apply_effect — laser_apply
     * floors at zero, so we don't need the explicit fminf clamp the
     * legacy code carried. Signal efficiency scales output the same
     * way for everyone so weak-signal mining feels weak whether
     * you're a player or AI. */
    float pre_hp = a->hp;
    laser_apply_effect(&a->hp, -mining_rate * signal_eff, 0.0f, dt);
    if (a->hp < pre_hp) {
        a->net_dirty = true;
        r.fired = true;
    }

    if (a->hp <= 0.01f) {
        fracture_asteroid(w, target_idx, r.hit_normal, fracturer_id);
        r.fractured = true;
    }
    return r;
}

mining_beam_t sim_mining_beam_step(world_t *w, vec2 muzzle, vec2 forward,
                                    int target_idx, int mining_level,
                                    float mining_rate, float signal_eff,
                                    int8_t fracturer_id, float dt) {
    return sim_mining_beam_step_with_aim_slack(w, muzzle, forward, target_idx,
        mining_level, mining_rate, signal_eff, fracturer_id, dt, 0.0f);
}
