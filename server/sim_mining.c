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
#include "game_sim.h"      /* MINING_RANGE, max_mineable_tier, fracture_asteroid */
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
        float surface_dist = proj - sqrtf(fmaxf(0.0f, a->radius * a->radius - perp * perp));
        if (surface_dist < -a->radius) continue;
        if (surface_dist > MINING_RANGE) continue;
        if (surface_dist < best_dist) { best_dist = surface_dist; best = i; }
    }
    return best;
}

mining_beam_t sim_mining_beam_step(world_t *w, vec2 muzzle, vec2 forward,
                                    int target_idx, int mining_level,
                                    float mining_rate, float signal_eff,
                                    int8_t fracturer_id, float dt) {
    mining_beam_t r = {
        .fired = false, .ineffective = false, .fractured = false,
        .hit = false,
        .beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE)),
        .hit_normal = v2(0.0f, 0.0f),
    };

    if (target_idx < 0 || target_idx >= MAX_ASTEROIDS) return r;
    asteroid_t *a = &w->asteroids[target_idx];
    if (!a->active || asteroid_is_collectible(a)) return r;

    /* Target validation (range / cone / clear-line-of-fire) is the
     * caller's responsibility — see `sim_mining_pick_target` and the
     * player hint path in `update_targeting_state`. The helper trusts
     * the caller's selection: given a target, apply one tick of fire.
     * (forward is unused here — kept in the signature for the future
     *  occluder check that #294 slice 4 will add.) */
    (void)forward;

    vec2 to_a = v2_sub(a->pos, muzzle);
    vec2 normal = v2_norm(to_a);
    r.beam_end = v2_sub(a->pos, v2_scale(normal, a->radius * 0.85f));
    r.hit_normal = normal;
    r.hit = true;

    /* Tier gate: laser too weak to chip this rock. Beam still hits (so
     * the visual tells the player "I'm pointed at it") but applies no
     * damage and reports back so the caller can flash the warning. */
    asteroid_tier_t max_tier = max_mineable_tier(mining_level);
    if (a->tier < max_tier) {
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
        fracture_asteroid(w, target_idx, normal, fracturer_id);
        r.fractured = true;
    }
    return r;
}
