/*
 * sim_autopilot.c — Player autopilot state machine for Signal Space Miner.
 * Extracted from game_sim.c (#272 slice).
 *
 * The autopilot drives a player's ship through a mining loop:
 *   find target → fly → mine → collect → return → dock → sell → launch → repeat
 */
#include "sim_autopilot.h"
#include "sim_nav.h"
#include "signal_model.h"

/* ================================================================== */
/* Player autopilot — server-side AI driving the player's own ship    */
/* ================================================================== */

/* Find the nearest active station with an ore-buyer module — that's
 * where the autopilot will sell. */
static int autopilot_find_refinery(const world_t *w, vec2 pos) {
    int best = -1;
    float best_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        if (!station_has_module(st, MODULE_DOCK)) continue;
        if (!station_has_module(st, MODULE_ORE_BUYER) &&
            !station_has_module(st, MODULE_FURNACE) &&
            !station_has_module(st, MODULE_FURNACE_CU) &&
            !station_has_module(st, MODULE_FURNACE_CR)) continue;
        float d = v2_dist_sq(pos, st->pos);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}


static bool autopilot_can_mine_asteroid(const server_player_t *sp, const asteroid_t *a) {
    if (!a->active || asteroid_is_collectible(a)) return false;
    return (int)a->tier >= (int)max_mineable_tier(sp->ship.mining_level);
}

static bool autopilot_clear_mining_approach(const world_t *w, const server_player_t *sp,
                                            const asteroid_t *a) {
    const hull_def_t *hull = ship_hull_def(&sp->ship);
    vec2 from_rock = v2_sub(sp->ship.pos, a->pos);
    float from_len = v2_len(from_rock);
    if (from_len < 1.0f) return true;
    vec2 outward = v2_scale(from_rock, 1.0f / from_len);
    vec2 approach = v2_add(a->pos, v2_scale(outward, a->radius + 120.0f));
    return nav_segment_clear(w, sp->ship.pos, approach, hull->ship_radius + 30.0f);
}

/* Pick the most autopilot-friendly mining target.
 *
 * Priority order:
 *   0. Fragments already inside the ship's local working radius
 *   1. Nearest mineable rock with a clear direct approach
 *   2. Nearby drifting fragments worth scooping up immediately
 *   3. Nearest mineable rock even if the final approach is cluttered
 */
static int autopilot_find_mining_target(const world_t *w, const server_player_t *sp) {
    int best = -1;
    float best_d = 1e18f;
    const float immediate_frag_range = ship_tractor_range(&sp->ship) + 120.0f;
    const float immediate_frag_sq = immediate_frag_range * immediate_frag_range;
    const float frag_pickup_sq = 600.0f * 600.0f;

    /* Pass 0: fragments already in our local scoop radius. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        if (a->tier != ASTEROID_TIER_S) continue;
        if (signal_strength_at(w, a->pos) <= 0.0f) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d > immediate_frag_sq) continue;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) return best;

    /* Pass 1: nearest mineable rock with a clear final approach. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (signal_strength_at(w, a->pos) <= 0.0f) continue;
        if (!autopilot_clear_mining_approach(w, sp, a)) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) return best;

    /* Pass 2: nearby drifting fragments only — don't cruise across the
     * belt scavenging, but do scoop up fragments already in our orbit. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        if (a->tier != ASTEROID_TIER_S) continue;
        if (signal_strength_at(w, a->pos) <= 0.0f) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d > frag_pickup_sq) continue;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) return best;

    /* Pass 3: any mineable rock — A* can still get there, but this is
     * lower priority than rocks we can work cleanly or fragments already
     * nearby. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (signal_strength_at(w, a->pos) <= 0.0f) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Tractor capacity = 2 + 2 × tractor_level (2/4/6/8/10).
 * The autopilot's mining loop is bounded by THIS, not by cargo capacity:
 * mined fragments live in the tow chain, not ship.cargo, and only
 * become credits when smelted at a station's furnace. */
static int autopilot_tractor_capacity(const ship_t *s) {
    return 2 + s->tractor_level * 2;
}

static bool autopilot_tractor_full(const ship_t *s) {
    return s->towed_count >= (uint8_t)autopilot_tractor_capacity(s);
}

/* True if the ship is damaged enough that the autopilot should bail
 * out of mining and return for repair. Also returns true any time
 * we've ALREADY started returning (so the threshold doesn't oscillate
 * if the hull regenerates back to 80%+ momentarily). */
static bool autopilot_needs_repair(const ship_t *s) {
    float max = ship_max_hull(s);
    if (max <= 0.0f) return false;
    return (s->hull / max) < 0.80f;
}

static bool autopilot_hull_full(const ship_t *s) {
    float max = ship_max_hull(s);
    if (max <= 0.0f) return true;
    return s->hull >= max - 0.5f;
}

/* Drive the player's ship via simulated input. The autopilot writes
 * sp->input each tick, and the existing physics/mining/dock systems
 * consume those intents like they would for a human player. */
void step_autopilot(world_t *w, server_player_t *sp, float dt) {
    if (sp->autopilot_mode == 0) return;

    sp->autopilot_timer += dt;

    /* Stuck detection: if the ship hasn't moved >50u in 8 seconds
     * while in a transit state, pick a new target. This breaks
     * deadlocks where avoidance oscillates against station walls. */
    if (sp->autopilot_state == AUTOPILOT_STEP_FLY_TO_TARGET ||
        sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY) {
        float moved = v2_dist_sq(sp->ship.pos, sp->autopilot_last_pos);
        if (moved > 50.0f * 50.0f) {
            sp->autopilot_last_pos = sp->ship.pos;
            sp->autopilot_stuck_timer = 0.0f;
        } else {
            sp->autopilot_stuck_timer += dt;
            if (sp->autopilot_stuck_timer > 8.0f) {
                SIM_LOG("[autopilot] player %d stuck for 8s, re-planning\n", sp->id);
                /* If carrying fragments, stay in RETURN_TO_REFINERY but
                 * force a path recompute (clear path age). Don't abandon
                 * the delivery — that causes the ship to tow rocks away
                 * from the station toward a new mining target. */
                if (sp->ship.towed_count > 0 &&
                    sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY) {
                    nav_force_replan(nav_player_path(sp->id));
                } else {
                    sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
                }
                sp->autopilot_target = -1;
                sp->autopilot_timer = 0.0f;
                sp->autopilot_stuck_timer = 0.0f;
                sp->autopilot_last_pos = sp->ship.pos;
            }
        }
    } else {
        sp->autopilot_last_pos = sp->ship.pos;
        sp->autopilot_stuck_timer = 0.0f;
    }

    /* Damage check: if hull dropped below 80%, bail out of mining and
     * return to a refinery for repair. The ship will hold in dock
     * until hull is at 100% before relaunching (handled in SELL state).
     * Skip the bail if we're already heading home or docked. */
    if (autopilot_needs_repair(&sp->ship) &&
        sp->autopilot_state != AUTOPILOT_STEP_RETURN_TO_REFINERY &&
        sp->autopilot_state != AUTOPILOT_STEP_DOCK &&
        sp->autopilot_state != AUTOPILOT_STEP_SELL &&
        sp->autopilot_state != AUTOPILOT_STEP_LAUNCH) {
        sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
        sp->autopilot_target = -1;
        sp->autopilot_timer = 0.0f;
    }

    /* Tractor management: ON when mining, collecting, or hauling home.
     * OFF during FIND_TARGET and FLY_TO_TARGET (transit without cargo).
     * RETURN_TO_REFINERY keeps tractor ON so spring physics pull towed
     * fragments along — this matters when the user toggles autopilot ON
     * while already carrying fragments. */
    if (sp->autopilot_state == AUTOPILOT_STEP_MINE ||
        sp->autopilot_state == AUTOPILOT_STEP_COLLECT ||
        (sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY &&
         sp->ship.towed_count > 0)) {
        sp->ship.tractor_active = true;
    } else if (sp->autopilot_state == AUTOPILOT_STEP_FIND_TARGET ||
               sp->autopilot_state == AUTOPILOT_STEP_FLY_TO_TARGET) {
        sp->ship.tractor_active = false;
    }

    /* Mode 1: mining loop. */
    switch (sp->autopilot_state) {
    case AUTOPILOT_STEP_FIND_TARGET: {
        if (sp->docked) {
            sp->input.interact = true; /* launch */
            sp->autopilot_state = AUTOPILOT_STEP_LAUNCH;
            break;
        }
        /* Carrying anything = deliver first. Don't mine with a loaded
         * tow chain — fragments trail behind looking broken and cause
         * edge cases where the ship flies away from the station. */
        if (sp->ship.towed_count > 0) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        int t = autopilot_find_mining_target(w, sp);
        if (t < 0) {
            /* Nothing minable — if near a station, dock and wait.
             * Otherwise head to the refinery. This prevents the
             * FIND→RETURN→FIND oscillation loop. */
            if (sp->in_dock_range && sp->nearby_station >= 0) {
                sp->input.interact = true; /* dock */
                sp->autopilot_state = AUTOPILOT_STEP_SELL;
                sp->autopilot_timer = 0.0f;
            } else {
                sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            }
            break;
        }
        sp->autopilot_target = t;
        sp->autopilot_state = AUTOPILOT_STEP_FLY_TO_TARGET;
        sp->autopilot_timer = 0.0f;
        /* Compute A* path to the mining target */
        nav_find_path(w, sp->ship.pos, w->asteroids[t].pos,
                      ship_hull_def(&sp->ship)->ship_radius + 30.0f,
                      nav_player_path(sp->id));
        break;
    }
    case AUTOPILOT_STEP_FLY_TO_TARGET: {
        if (sp->autopilot_target < 0 || sp->autopilot_target >= MAX_ASTEROIDS) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        const asteroid_t *a = &w->asteroids[sp->autopilot_target];
        if (!a->active) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        /* Bail to delivery if we picked up fragments in transit
         * (e.g., passed through fragments from another miner). */
        if (sp->ship.towed_count > 0) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        const hull_def_t *hull = ship_hull_def(&sp->ship);
        /* Standoff distance: where the autopilot wants the ship to "park"
         * relative to the asteroid surface. Laser MINING_RANGE is 170u
         * so anywhere within radius+170 reaches; we sit at radius+120
         * which gives ~100u of clearance from the surface (after the
         * 16u ship_radius) — enough to absorb fracture spawn velocity,
         * tracking jitter, and gravity perturbations without grinding
         * the hull on the rock. Fragments (S-tier) need to be at the
         * ship itself for tractor pickup. */
        float standoff = (a->tier == ASTEROID_TIER_S)
            ? 0.0f
            : (a->radius + 120.0f);
        float dist_to_a = sqrtf(v2_dist_sq(sp->ship.pos, a->pos));
        float effective_dist = fmaxf(0.0f, dist_to_a - standoff);

        /* Transition to MINE/COLLECT once close enough AND moving slowly. */
        float current_speed = sqrtf(v2_len_sq(sp->ship.vel));
        if (effective_dist < 30.0f && current_speed < 80.0f) {
            sp->input.thrust = 0.0f;
            if (a->tier == ASTEROID_TIER_S) {
                sp->autopilot_state = AUTOPILOT_STEP_COLLECT;
            } else {
                sp->autopilot_state = AUTOPILOT_STEP_MINE;
            }
            sp->autopilot_timer = 0.0f;
            break;
        }

        /* Follow A* path via shared helpers. */
        nav_path_t *path = nav_player_path(sp->id);
        nav_follow_path(w, path, sp->ship.pos, a->pos, hull->ship_radius + 30.0f, dt);
        nav_steer_t st = nav_steer_toward_waypoint(path, sp->ship.pos, a->pos, dt);
        float diff = wrap_angle(st.desired_heading - sp->ship.angle);
        sp->input.turn = (diff > 0.05f) ? 1.0f : (diff < -0.05f ? -1.0f : 0.0f);
        float facing = cosf(diff);

        /* Velocity-controlled approach. */
        float target_speed = nav_approach_speed(effective_dist, 150.0f);
        if (st.wp_dist < 200.0f && st.at_intermediate) {
            float wp_speed = nav_approach_speed(st.wp_dist, 80.0f);
            if (wp_speed < target_speed) target_speed = wp_speed;
        }
        vec2 to_target_dir = (dist_to_a > 0.5f)
            ? v2_scale(v2_sub(a->pos, sp->ship.pos), 1.0f / dist_to_a)
            : v2(cosf(sp->ship.angle), sinf(sp->ship.angle));
        float approach_v = v2_dot(sp->ship.vel, to_target_dir);
        float thrust_cmd = nav_speed_control(approach_v, target_speed);
        if (facing < 0.5f && thrust_cmd > 0.0f) thrust_cmd = 0.0f;
        /* Brake if an asteroid is dead ahead (between A* waypoints). */
        float fwd_clear = nav_forward_clearance(w, sp->ship.pos, sp->ship.vel,
                                                 hull->ship_radius, sp->ship.angle);
        if (fwd_clear < 1.0f && thrust_cmd > 0.0f)
            thrust_cmd *= fwd_clear;
        sp->input.thrust = thrust_cmd;
        sp->input.mine = false;

        /* Stuck-fly safety: if we've been flying >60s and haven't arrived,
         * pick a new target. */
        if (sp->autopilot_timer > 60.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
        }
        break;
    }
    case AUTOPILOT_STEP_MINE: {
        if (sp->autopilot_target < 0 || sp->autopilot_target >= MAX_ASTEROIDS) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        /* Don't laser with a full tow — go dump first. */
        if (autopilot_tractor_full(&sp->ship)) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const asteroid_t *a = &w->asteroids[sp->autopilot_target];
        if (!a->active) {
            /* Asteroid fractured or vanished — go collect fragments. */
            sp->autopilot_state = AUTOPILOT_STEP_COLLECT;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* Hover near the rock at a safe standoff. Same logic as the NPC
         * miner: if too close, push away; if too far, pull in; mine when
         * we're in the sweet spot AND facing the rock. Standoff matches
         * the FLY_TO_TARGET arrival distance so transitions are smooth. */
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, a->pos));
        float standoff = a->radius + 120.0f;
        float sweet_min = standoff - 15.0f;
        float sweet_max = standoff + 30.0f;

        /* If we drifted way out, return to FLY_TO_TARGET (which handles
         * the fast-cruise approach properly). */
        if (dist > sweet_max + 200.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FLY_TO_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }

        if (dist < sweet_min) {
            /* Too close — turn AWAY from the rock and burn forward to escape. */
            vec2 away = v2_sub(sp->ship.pos, a->pos);
            float push_angle = atan2f(away.y, away.x);
            float diff = wrap_angle(push_angle - sp->ship.angle);
            sp->input.turn = (diff > 0.05f) ? 1.0f : (diff < -0.05f ? -1.0f : 0.0f);
            sp->input.thrust = (cosf(diff) > 0.6f) ? 0.6f : 0.0f;
            sp->input.mine = false;
        } else if (dist > sweet_max) {
            /* Drifted out — close in slowly. */
            vec2 to_a = v2_sub(a->pos, sp->ship.pos);
            float face = atan2f(to_a.y, to_a.x);
            float diff = wrap_angle(face - sp->ship.angle);
            sp->input.turn = (diff > 0.05f) ? 1.0f : (diff < -0.05f ? -1.0f : 0.0f);
            float approach_v = v2_dot(sp->ship.vel, v2_scale(to_a, 1.0f / dist));
            /* Hold approach speed at ~50 u/s */
            sp->input.thrust = nav_speed_control(approach_v, 50.0f);
            if (cosf(diff) < 0.5f) sp->input.thrust = 0.0f;
            sp->input.mine = false;
        } else {
            /* In the sweet spot — face the rock and fire. */
            vec2 to_a = v2_sub(a->pos, sp->ship.pos);
            float face = atan2f(to_a.y, to_a.x);
            float diff = wrap_angle(face - sp->ship.angle);
            sp->input.turn = (diff > 0.05f) ? 1.0f : (diff < -0.05f ? -1.0f : 0.0f);
            /* Brake any residual velocity so we hover. */
            float speed = sqrtf(v2_len_sq(sp->ship.vel));
            if (speed > 30.0f) {
                /* Reverse thrust along current motion direction to bleed speed. */
                vec2 vel_dir = v2_scale(sp->ship.vel, 1.0f / speed);
                vec2 fwd = v2(cosf(sp->ship.angle), sinf(sp->ship.angle));
                float vel_along_fwd = v2_dot(vel_dir, fwd) * speed;
                if (vel_along_fwd > 30.0f) sp->input.thrust = -1.0f;
                else if (vel_along_fwd < -30.0f) sp->input.thrust = 1.0f;
                else sp->input.thrust = 0.0f;
            } else {
                sp->input.thrust = 0.0f;
            }
            sp->input.mine = (fabsf(diff) < 0.20f);
            sp->input.mining_target_hint = sp->autopilot_target;
        }
        break;
    }
    case AUTOPILOT_STEP_COLLECT: {
        /* Sweep nearby fragments only — DON'T chase fragments across
         * the world. The COLLECT state is for the cluster spawned by
         * the rock we just fractured. Bail out the instant the tractor
         * is full OR nothing's nearby OR we've been loitering too long. */
        sp->ship.tractor_active = true;
        sp->input.mine = false;
        if (autopilot_tractor_full(&sp->ship)) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const float collect_range_sq = 600.0f * 600.0f;
        int best = -1;
        float best_d = 1e18f;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            const asteroid_t *a = &w->asteroids[i];
            if (!a->active || !asteroid_is_collectible(a)) continue;
            float d = v2_dist_sq(sp->ship.pos, a->pos);
            if (d > collect_range_sq) continue;
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best < 0) {
            /* No more fragments in range. If we're carrying anything,
             * dump it at the nearest refinery; otherwise look for a
             * new mining target. */
            sp->autopilot_state = (sp->ship.towed_count > 0)
                ? AUTOPILOT_STEP_RETURN_TO_REFINERY
                : AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const asteroid_t *frag = &w->asteroids[best];
        vec2 to = v2_sub(frag->pos, sp->ship.pos);
        float desired = atan2f(to.y, to.x);
        float diff = wrap_angle(desired - sp->ship.angle);
        sp->input.turn = (diff > 0.05f) ? 1.0f : (diff < -0.05f ? -1.0f : 0.0f);
        sp->input.thrust = (cosf(diff) > 0.5f) ? 0.6f : 0.0f;
        if (sp->autopilot_timer > 8.0f) {
            sp->autopilot_state = (sp->ship.towed_count > 0)
                ? AUTOPILOT_STEP_RETURN_TO_REFINERY
                : AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_RETURN_TO_REFINERY: {
        if (sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_SELL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        int s = autopilot_find_refinery(w, sp->ship.pos);
        if (s < 0) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        const station_t *st = &w->stations[s];
        sp->autopilot_target = s;
        /* Damage routing: if hull is below the repair threshold, this
         * is a "dock for repair" run; we approach the dock berth and
         * trigger interact when close. Otherwise it's a "drop fragments
         * at the hopper" run; we just need to get within hopper-pull
         * range, release the tractor, and head out again. */
        bool need_repair = autopilot_needs_repair(&sp->ship);

        /* Once we're inside hopper-pull range of the destination, release
         * the tractor so towed fragments drop free and get caught by the
         * station's smelt beams. */
        float station_dist_sq = v2_dist_sq(sp->ship.pos, st->pos);
        if (station_dist_sq < 600.0f * 600.0f &&
            sp->ship.tractor_active && sp->ship.towed_count > 0) {
            sp->input.release_tow = true;
        }
        /* A* path toward the dock approach point. */
        vec2 dock_target = station_approach_target(st, sp->ship.pos);
        /* Follow A* path via shared helpers. */
        const hull_def_t *hull = ship_hull_def(&sp->ship);
        nav_path_t *path = nav_player_path(sp->id);
        nav_follow_path(w, path, sp->ship.pos, dock_target, hull->ship_radius + 30.0f, dt);
        nav_steer_t st2 = nav_steer_toward_waypoint(path, sp->ship.pos, dock_target, dt);
        float diff = wrap_angle(st2.desired_heading - sp->ship.angle);
        sp->input.turn = (diff > 0.05f) ? 1.0f : (diff < -0.05f ? -1.0f : 0.0f);
        float facing = cosf(diff);
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, st->pos));

        float target_speed = nav_approach_speed(dist, 120.0f);
        if (st2.wp_dist < 200.0f && st2.at_intermediate) {
            float wp_speed = nav_approach_speed(st2.wp_dist, 80.0f);
            if (wp_speed < target_speed) target_speed = wp_speed;
        }
        vec2 to_st_dir = (dist > 0.5f)
            ? v2_scale(v2_sub(st->pos, sp->ship.pos), 1.0f / dist)
            : v2(cosf(sp->ship.angle), sinf(sp->ship.angle));
        float approach_v = v2_dot(sp->ship.vel, to_st_dir);
        float thrust_cmd = nav_speed_control(approach_v, target_speed);
        if (facing < 0.5f && thrust_cmd > 0.0f) thrust_cmd = 0.0f;
        float fwd_clear = nav_forward_clearance(w, sp->ship.pos, sp->ship.vel,
                                                 hull->ship_radius, sp->ship.angle);
        if (fwd_clear < 1.0f && thrust_cmd > 0.0f)
            thrust_cmd *= fwd_clear;
        sp->input.thrust = thrust_cmd;
        sp->input.mine = false;

        /* Drop-and-leave path (no damage): once we've released the
         * tractor and are inside the hopper area, we don't need to
         * dock — the furnace beam smelts our fragments asynchronously
         * and credits us directly. Just turn around and find the next
         * mining target. */
        if (!need_repair && sp->ship.towed_count == 0 && dist < 700.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }

        /* Damage path: dock for passive heal once close enough. */
        if (need_repair && dist < DOCK_APPROACH_RANGE && sp->in_dock_range) {
            sp->input.interact = true;
            sp->autopilot_state = AUTOPILOT_STEP_DOCK;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_DOCK: {
        if (sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_SELL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->autopilot_timer > 6.0f) {
            /* Approach didn't snap. Re-issue interact and re-aim. */
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_SELL: {
        if (!sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        /* Phase 1 (first ~0.6s): trigger sell + repair, then hold while
         * the docked passive heal brings hull back up. The 0.6s gives
         * the sim a few sub-steps to process the sell action and the
         * repair action before we start checking hull. */
        if (sp->autopilot_timer < 0.6f) {
            sp->input.service_sell = true;
            sp->input.service_sell_only = COMMODITY_COUNT;
            /* If a repair bay exists at this station, also pay for an
             * instant repair. Falls back to passive heal if not. */
            sp->input.service_repair = true;
            break;
        }
        /* Phase 2: wait until hull is full before launching. The dock
         * passive heal is 8 hp/sec — even from 0%, that's <15s for the
         * miner hull (100 max). The autopilot just sits in dock. */
        if (!autopilot_hull_full(&sp->ship)) {
            /* Stay docked, no action needed. Loop again next tick. */
            break;
        }
        /* Hull repaired AND cargo sold — launch back into the field. */
        sp->input.interact = true;
        sp->autopilot_state = AUTOPILOT_STEP_LAUNCH;
        sp->autopilot_timer = 0.0f;
        break;
    }
    case AUTOPILOT_STEP_LAUNCH: {
        if (!sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
        } else if (sp->autopilot_timer > 2.0f) {
            sp->input.interact = true; /* re-issue */
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    default:
        sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
        break;
    }
}
