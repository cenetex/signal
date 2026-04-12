/*
 * sim_autopilot.c — Player autopilot state machine for Signal Space Miner.
 * Extracted from game_sim.c (#272 slice).
 *
 * The autopilot drives a player's ship through a mining loop:
 *   find target → fly → mine → collect → return → dock → sell → launch → repeat
 */
#include "sim_autopilot.h"
#include "sim_nav.h"
#include "sim_flight.h"
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
 *   1. Nearest mineable rock with a clear direct approach
 *   2. Nearest mineable rock even if the final approach is cluttered
 * Fragments are NOT targeted — the tractor auto-collects nearby ones
 * during flight. Explicitly targeting fragments caused orbit loops.
 */
static int autopilot_find_mining_target(const world_t *w, const server_player_t *sp) {
    int best = -1;
    float best_d = 1e18f;

    /* Pass 0 removed: don't explicitly target fragments. The tractor
     * auto-collects nearby fragments during flight. Targeting fragments
     * caused the ship to orbit them endlessly near stations when the
     * tractor couldn't grab them (chase → timeout → re-target loop). */

    /* Pass 1: nearest mineable rock with a clear final approach. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (signal_strength_at(w, a->pos) < 0.5f) continue;
        if (!autopilot_clear_mining_approach(w, sp, a)) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) return best;

    /* Pass 2 removed: same fragment-orbiting issue as pass 0. */

    /* Pass 3: any mineable rock — A* can still get there, but this is
     * lower priority than rocks we can work cleanly or fragments already
     * nearby. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (signal_strength_at(w, a->pos) < 0.5f) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Tractor capacity = 2 + 2 × tractor_level (2/4/6/8/10).
 * The autopilot's mining loop is bounded by THIS, not by cargo capacity:
 * mined fragments live in the tow chain, not ship.cargo, and only
 * become credits when smelted at a station's furnace. */
/* True if the ship is near enough to any station that towed fragments
 * will auto-release and get smelted. Don't bail to RETURN in this case. */
static bool near_station_hopper(const world_t *w, vec2 pos) {
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_is_active(&w->stations[i])) continue;
        if (v2_dist_sq(pos, w->stations[i].pos) < 700.0f * 700.0f) return true;
    }
    return false;
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
    /* Autopilot holds tractor during mining/collecting/returning with cargo */
    sp->input.tractor_hold =
        (sp->autopilot_state == AUTOPILOT_STEP_MINE ||
         sp->autopilot_state == AUTOPILOT_STEP_COLLECT ||
         (sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY &&
          sp->ship.towed_count > 0));

    /* Mode 1: mining loop. */
    switch (sp->autopilot_state) {
    case AUTOPILOT_STEP_FIND_TARGET: {
        if (sp->docked) {
            sp->input.interact = true; /* launch */
            sp->autopilot_state = AUTOPILOT_STEP_LAUNCH;
            break;
        }
        /* Carrying fragments and NOT near a station = go deliver.
         * Near a station, fragments auto-release to the hopper so
         * don't bail (prevents pickup-release-bail oscillation). */
        if (sp->ship.towed_count > 0 && !near_station_hopper(w, sp->ship.pos)) {
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
        if (!a->active || asteroid_is_collectible(a)) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        /* Bail to delivery if carrying fragments away from a station. */
        if (sp->ship.towed_count > 0 && !near_station_hopper(w, sp->ship.pos)) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        /* Don't fly into weak signal — the target may have drifted. */
        if (signal_strength_at(w, sp->ship.pos) < 0.5f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            break;
        }
        /* Also check the target's signal — don't fly to a dead zone. */
        if (signal_strength_at(w, a->pos) < 0.3f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            break;
        }
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

        /* Transition to MINE/COLLECT once close enough AND slow enough
         * for the hover controller to manage. 30 u/s prevents the
         * overshoot-through-asteroid cycle. */
        float current_speed = sqrtf(v2_len_sq(sp->ship.vel));
        if (effective_dist < 30.0f && current_speed < 30.0f) {
            sp->input.thrust = 0.0f;
            if (a->tier == ASTEROID_TIER_S) {
                sp->autopilot_state = AUTOPILOT_STEP_COLLECT;
            } else {
                sp->autopilot_state = AUTOPILOT_STEP_MINE;
            }
            sp->autopilot_timer = 0.0f;
            break;
        }

        /* Follow A* path via flight controller. */
        nav_path_t *path = nav_player_path(sp->id);
        flight_cmd_t cmd = flight_steer_to(w, &sp->ship, path, a->pos,
                                            standoff, 150.0f, dt);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
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
        /* Don't mine while carrying fragments away from a station. */
        if (sp->ship.towed_count > 0 && !near_station_hopper(w, sp->ship.pos)) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const asteroid_t *a = &w->asteroids[sp->autopilot_target];
        if (!a->active || asteroid_is_collectible(a)) {
            /* Asteroid fractured, vanished, or became a fragment
             * (slot recycled). Re-evaluate. */
            sp->autopilot_state = AUTOPILOT_STEP_COLLECT;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* Hover near the rock at a safe standoff via flight controller. */
        float standoff = a->radius + 120.0f;
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, a->pos));

        /* If we drifted way out, return to FLY_TO_TARGET. */
        if (dist > standoff + 30.0f + 200.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FLY_TO_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* If gravity dragged us (and the asteroid) out of signal,
         * abandon this rock and find a new target closer to home. */
        if (signal_strength_at(w, sp->ship.pos) < 0.5f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }

        flight_cmd_t cmd = flight_hover_near(w, &sp->ship, a->pos, standoff);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
        /* Mine when roughly facing the rock within mining range.
         * MINING_RANGE is 170u, so anything within standoff+50 works.
         * Angle threshold widened to 0.35 rad (~20°) to prevent the
         * proportional turn from oscillating past the fire window. */
        vec2 to_a = v2_sub(a->pos, sp->ship.pos);
        float face = atan2f(to_a.y, to_a.x);
        float diff = wrap_angle(face - sp->ship.angle);
        if (dist < standoff + 50.0f && fabsf(diff) < 0.35f) {
            sp->input.mine = true;
            sp->input.mining_target_hint = sp->autopilot_target;
        } else {
            sp->input.mine = false;
        }
        break;
    }
    case AUTOPILOT_STEP_COLLECT: {
        /* Sweep nearby fragments only — DON'T chase fragments across
         * the world. The COLLECT state is for the cluster spawned by
         * the rock we just fractured. Bail out the instant the tractor
         * is full OR nothing's nearby OR we've been loitering too long. */
        sp->input.tractor_hold = true;
        sp->input.mine = false;
        /* Signal check — don't collect in weak signal. */
        if (signal_strength_at(w, sp->ship.pos) < 0.5f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* Carrying fragments away from station = go deliver. */
        if (sp->ship.towed_count > 0 && !near_station_hopper(w, sp->ship.pos)) {
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
        sp->input.turn = flight_face_heading(&sp->ship, desired);
        float diff = wrap_angle(desired - sp->ship.angle);
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

        /* Fragments stay towed — furnace smelting claims them directly.
         * Just fly close enough for furnace range. Standoff 200u
         * puts the ship inside furnace pull range from ring 1 modules. */
        vec2 fly_target = need_repair
            ? station_approach_target(st, sp->ship.pos)
            : st->pos;
        nav_path_t *path = nav_player_path(sp->id);
        flight_cmd_t cmd = flight_steer_to(w, &sp->ship, path, fly_target,
                                            need_repair ? 0.0f : 200.0f, 120.0f, dt);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
        sp->input.mine = false;
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, st->pos));

        /* Drop-and-leave path (no damage): once we've released the
         * tractor and are inside the hopper area, we don't need to
         * dock — the furnace beam smelts our fragments asynchronously
         * and credits us directly. Just turn around and find the next
         * mining target. */
        if (!need_repair && sp->ship.towed_count == 0 && dist < 500.0f) {
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
