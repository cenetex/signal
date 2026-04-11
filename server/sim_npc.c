/*
 * sim_npc.c -- NPC ship subsystem.
 * Extracted from game_sim.c: target finding, steering, physics,
 * spawn, state machines (MINER / HAULER / TOW), and the per-tick
 * step_npc_ships() dispatcher.
 */
#include "sim_npc.h"
#include "sim_nav.h"
#include "signal_model.h"
#include <math.h>
#include <string.h>

/* ================================================================== */
/* NPC ships                                                          */
/* ================================================================== */

/* Spawn an NPC at a station. Returns slot index or -1 if full. */
int spawn_npc(world_t *w, int station_idx, npc_role_t role) {
    int slot = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w->npc_ships[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    station_t *st = &w->stations[station_idx];
    hull_class_t hc;
    switch (role) {
    case NPC_ROLE_MINER: hc = HULL_CLASS_NPC_MINER; break;
    case NPC_ROLE_HAULER: hc = HULL_CLASS_HAULER; break;
    case NPC_ROLE_TOW:    hc = HULL_CLASS_HAULER; break; /* tow drone uses hauler hull */
    default: hc = HULL_CLASS_NPC_MINER; break;
    }
    npc_ship_t *npc = &w->npc_ships[slot];
    memset(npc, 0, sizeof(*npc));
    npc->active = true;
    npc->role = role;
    npc->hull_class = hc;
    npc->state = NPC_STATE_DOCKED;
    npc->pos = v2_add(st->pos, v2(30.0f * (float)(slot % 3 - 1), -(st->radius + HULL_DEFS[hc].ship_radius + 50.0f)));
    npc->angle = PI_F * 0.5f;
    npc->target_asteroid = -1;
    npc->towed_fragment = -1;
    npc->towed_scaffold = -1;
    npc->home_station = station_idx;
    npc->dest_station = station_idx;
    npc->state_timer = (role == NPC_ROLE_MINER) ? NPC_DOCK_TIME : HAULER_DOCK_TIME;
    npc->tint_r = 1.0f; npc->tint_g = 1.0f; npc->tint_b = 1.0f;
    /* Tow drones get a distinct yellow-amber tint */
    if (role == NPC_ROLE_TOW) {
        npc->tint_r = 1.0f; npc->tint_g = 0.85f; npc->tint_b = 0.30f;
    }
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_NPC_SPAWNED,
        .npc_spawned = { .slot = slot, .role = role, .home_station = station_idx },
    });
    SIM_LOG("[sim] spawned %s at station %d (slot %d)\n",
            role == NPC_ROLE_MINER ? "miner" :
            role == NPC_ROLE_HAULER ? "hauler" : "tow drone",
            station_idx, slot);
    return slot;
}

static bool npc_target_valid(const world_t *w, const npc_ship_t *npc) {
    if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_ASTEROIDS) return false;
    const asteroid_t *a = &w->asteroids[npc->target_asteroid];
    return a->active && a->tier != ASTEROID_TIER_S;
}

static int npc_find_mineable_asteroid(const world_t *w, const npc_ship_t *npc) {
    /* Priority: DESTROY contract targets first */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active || w->contracts[k].action != CONTRACT_FRACTURE) continue;
        int idx = w->contracts[k].target_index;
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
        /* Check not already taken by another miner */
        bool taken = false;
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            if (&w->npc_ships[n] == npc) continue;
            if (w->npc_ships[n].active && w->npc_ships[n].role == NPC_ROLE_MINER &&
                w->npc_ships[n].target_asteroid == idx) { taken = true; break; }
        }
        if (!taken) return idx;
    }

    /* Normal: find nearest mineable asteroid */
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        if (signal_npc_confidence(signal_strength_at(w, a->pos)) < 0.1f) continue;
        /* Skip asteroids already targeted by another miner */
        bool taken = false;
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            if (&w->npc_ships[n] == npc) continue;
            if (w->npc_ships[n].active && w->npc_ships[n].role == NPC_ROLE_MINER &&
                w->npc_ships[n].target_asteroid == i) { taken = true; break; }
        }
        if (taken) continue;
        float d = v2_dist_sq(npc->pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void npc_steer_toward(npc_ship_t *npc, vec2 target, float accel, float turn_speed, float dt) {
    vec2 delta = v2_sub(target, npc->pos);
    float desired = atan2f(delta.y, delta.x);
    float diff = wrap_angle(desired - npc->angle);
    float max_turn = turn_speed * dt;
    if (diff > max_turn) diff = max_turn;
    else if (diff < -max_turn) diff = -max_turn;
    npc->angle = wrap_angle(npc->angle + diff);
    vec2 fwd = v2_from_angle(npc->angle);
    npc->vel = v2_add(npc->vel, v2_scale(fwd, accel * dt));
    npc->thrusting = accel > 0.0f;
}

/* (Reactive avoidance steering removed — all NPC/autopilot navigation
 * now uses A* paths via npc_steer_with_path. compute_path_avoidance
 * is retained for potential future use by manual-play collision hints.) */

/* A*-guided NPC steering: compute path on first call or when stale,
 * then steer directly toward the next waypoint. No reactive avoidance —
 * the A* path already routes around station walls and large asteroids. */
static void npc_steer_with_path(const world_t *w, int npc_idx, npc_ship_t *npc,
                                vec2 final_target, float accel, float turn_speed, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    nav_path_t *path = nav_npc_path(npc_idx);
    nav_follow_path(w, path, npc->pos, final_target, hull->ship_radius + 30.0f, dt);
    nav_steer_t st = nav_steer_toward_waypoint(path, npc->pos, final_target, dt);

    float diff = wrap_angle(st.desired_heading - npc->angle);
    float max_turn = turn_speed * dt;
    if (diff > max_turn) diff = max_turn;
    else if (diff < -max_turn) diff = -max_turn;
    npc->angle = wrap_angle(npc->angle + diff);

    float facing = cosf(diff);
    float thrust_gate = (facing > 0.5f) ? facing : 0.0f;
    float speed_cap = accel;
    if (st.wp_dist < 200.0f && st.at_intermediate)
        speed_cap *= fmaxf(0.2f, st.wp_dist / 200.0f);
    /* Brake if asteroid dead ahead */
    float fwd_clear = nav_forward_clearance(w, npc->pos, npc->vel,
                                             hull->ship_radius, npc->angle);
    speed_cap *= fwd_clear;
    vec2 fwd = v2_from_angle(npc->angle);
    npc->vel = v2_add(npc->vel, v2_scale(fwd, speed_cap * thrust_gate * dt));
    npc->thrusting = (speed_cap * thrust_gate) > 0.0f;
}

static void npc_apply_physics(npc_ship_t *npc, float drag, float dt, const world_t *w) {
    npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (drag * dt)));
    npc->pos = v2_add(npc->pos, v2_scale(npc->vel, dt));
    /* Signal-based boundary: NPCs pushed back when confidence is low */
    float sig = signal_strength_at(w, npc->pos);
    float npc_conf = signal_npc_confidence(sig);
    if (npc_conf < 1.0f) {
        float best_d_sq = 1e18f;
        int best_s = 0;
        for (int i = 0; i < MAX_STATIONS; i++) {
            float d_sq = v2_dist_sq(npc->pos, w->stations[i].pos);
            if (d_sq < best_d_sq) { best_d_sq = d_sq; best_s = i; }
        }
        vec2 to_station = v2_sub(w->stations[best_s].pos, npc->pos);
        float d = sqrtf(v2_len_sq(to_station));
        if (d > 0.001f) {
            float edge = w->stations[best_s].signal_range;
            float overshoot = fmaxf(0.0f, d - edge);
            float push_strength = overshoot * 0.08f + (1.0f - npc_conf) * 0.05f;
            vec2 push = v2_scale(to_station, push_strength / d);
            npc->vel = v2_add(npc->vel, push);
        }
    }
}


/* Push NPC out of a circle (no damage, unlike player collision). */
static void resolve_npc_circle(npc_ship_t *npc, vec2 center, float radius) {
    const hull_def_t *hull = npc_hull_def(npc);
    float minimum = radius + hull->ship_radius;
    vec2 delta = v2_sub(npc->pos, center);
    float d_sq = v2_len_sq(delta);
    if (d_sq >= minimum * minimum) return;
    float d = sqrtf(d_sq);
    vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
    npc->pos = v2_add(center, v2_scale(normal, minimum));
    float vel_toward = v2_dot(npc->vel, normal);
    if (vel_toward < 0.0f)
        npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.0f));
}

/* Push NPC out of a corridor annular sector (with angular margin). */
static void resolve_npc_annular_sector(npc_ship_t *npc, vec2 center,
                                        float ring_r, float angle_a, float angle_b) {
    const hull_def_t *hull = npc_hull_def(npc);
    float ship_r = hull->ship_radius;
    vec2 delta = v2_sub(npc->pos, center);
    float dist = sqrtf(v2_len_sq(delta));
    if (dist < 1.0f) return;

    float r_inner = ring_r - STATION_CORRIDOR_HW - ship_r;
    float r_outer = ring_r + STATION_CORRIDOR_HW + ship_r;
    if (dist <= r_inner || dist >= r_outer) return;

    /* Angular test with margin (matches player collision) */
    float npc_angle = atan2f(delta.y, delta.x);
    float angular_margin = (dist > 1.0f) ? asinf(fminf(ship_r / dist, 1.0f)) : 0.0f;
    float da = angle_b - angle_a;
    while (da > PI_F) da -= TWO_PI_F;
    while (da < -PI_F) da += TWO_PI_F;
    float expanded_start = angle_a - (da > 0 ? angular_margin : -angular_margin);
    float expanded_da = da + (da > 0 ? 2.0f : -2.0f) * angular_margin;
    if (angle_in_arc(npc_angle, expanded_start, expanded_da) < 0.0f) return;

    /* Push radially to nearest edge */
    vec2 radial = v2_scale(delta, 1.0f / dist);
    float d_inner = dist - (ring_r - STATION_CORRIDOR_HW);
    float d_outer = (ring_r + STATION_CORRIDOR_HW) - dist;
    if (d_inner < d_outer) {
        npc->pos = v2_add(center, v2_scale(radial, r_inner));
        float vt = v2_dot(npc->vel, radial);
        if (vt > 0.0f) npc->vel = v2_sub(npc->vel, v2_scale(radial, vt * 1.0f));
    } else {
        npc->pos = v2_add(center, v2_scale(radial, r_outer));
        float vt = v2_dot(npc->vel, radial);
        if (vt < 0.0f) npc->vel = v2_sub(npc->vel, v2_scale(radial, vt * 1.0f));
    }
}

static void npc_resolve_station_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    float ship_r = hull->ship_radius;
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        if (!station_collides(st)) continue;

        station_geom_t geom;
        station_build_geom(st, &geom);

        /* Core: empty space, no collision */

        /* Module circles */
        for (int ci = 0; ci < geom.circle_count; ci++)
            resolve_npc_circle(npc, geom.circles[ci].center, geom.circles[ci].radius);

        /* Near-module suppression + corridor annular sectors
         * (matches player collision logic) */
        float npc_dist = sqrtf(v2_dist_sq(npc->pos, st->pos));
        vec2 npc_delta = v2_sub(npc->pos, st->pos);
        float npc_ang = atan2f(npc_delta.y, npc_delta.x);

        for (int ci = 0; ci < geom.corridor_count; ci++) {
            float ring_r = geom.corridors[ci].ring_radius;

            /* Check if NPC is near any module on this corridor's ring */
            bool near_module = false;
            if (fabsf(npc_dist - ring_r) < STATION_CORRIDOR_HW + ship_r + STATION_MODULE_COL_RADIUS) {
                for (int mi = 0; mi < geom.circle_count; mi++) {
                    if (geom.circles[mi].ring != geom.corridors[ci].ring) continue;
                    float ang_diff = wrap_angle(npc_ang - geom.circles[mi].angle);
                    float angular_size = (ring_r > 1.0f) ? (STATION_MODULE_COL_RADIUS + ship_r) / ring_r : 0.0f;
                    if (fabsf(ang_diff) < angular_size) {
                        near_module = true;
                        break;
                    }
                }
            }

            if (!near_module) {
                resolve_npc_annular_sector(npc, geom.center,
                    ring_r, geom.corridors[ci].angle_a, geom.corridors[ci].angle_b);
            }
        }
    }
}

static void npc_resolve_asteroid_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || asteroid_is_collectible(a)) continue;
        float minimum = a->radius + hull->ship_radius;
        vec2 delta = v2_sub(npc->pos, a->pos);
        float d_sq = v2_len_sq(delta);
        if (d_sq >= minimum * minimum) continue;
        float d = sqrtf(d_sq);
        vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
        npc->pos = v2_add(a->pos, v2_scale(normal, minimum));
        float vel_toward = v2_dot(npc->vel, normal);
        if (vel_toward < 0.0f)
            npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.0f));
    }
}

/* Find nearest active station with a dock module. Returns 0 as fallback. */
static int nearest_active_dock_station(const world_t *w, vec2 pos) {
    int best = 0;
    float best_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_is_active(&w->stations[s])) continue;
        if (!station_has_module(&w->stations[s], MODULE_DOCK)) continue;
        float d = v2_dist_sq(pos, w->stations[s].pos);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}

static void npc_validate_stations(world_t *w, npc_ship_t *npc) {
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS ||
        !station_is_active(&w->stations[npc->home_station]))
        npc->home_station = nearest_active_dock_station(w, npc->pos);
    if (npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS ||
        !station_is_active(&w->stations[npc->dest_station]))
        npc->dest_station = npc->home_station;
}

static void step_hauler(world_t *w, npc_ship_t *npc, int n, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            station_t *home = &w->stations[npc->home_station];
            float carried = 0.0f;
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) carried += npc->cargo[c];
            float space = hull->ingot_capacity - carried;
            bool loaded = false;

            /* Contract-driven routing: find highest-value fillable contract */
            int best_contract = -1;
            float best_score = 0.0f;
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) continue;
                if (w->contracts[k].action != CONTRACT_TRACTOR) continue;
                if (w->contracts[k].station_index >= MAX_STATIONS) continue;
                commodity_t c = w->contracts[k].commodity;
                if (c < COMMODITY_RAW_ORE_COUNT) continue; /* haulers carry ingots only */
                if (home->inventory[c] < 0.5f) continue; /* no stock to fill */
                float dist = fmaxf(1.0f, v2_len(v2_sub(w->stations[w->contracts[k].station_index].pos, home->pos)));
                float score = contract_price(&w->contracts[k]) / dist;
                if (score > best_score) {
                    best_score = score;
                    best_contract = k;
                }
            }

            if (best_contract >= 0) {
                /* Load the commodity for this contract (leave reserve for players) */
                commodity_t ingot = w->contracts[best_contract].commodity;
                npc->dest_station = w->contracts[best_contract].station_index;
                float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                float take = fminf(avail, space);
                if (take > 0.5f) {
                    npc->cargo[ingot] += take;
                    home->inventory[ingot] -= take;
                    loaded = true;
                }
            } else {
                /* Fallback: original round-trip behavior (leave reserve for players) */
                station_t *dest = &w->stations[npc->dest_station];
                if (station_has_module(dest, MODULE_FRAME_PRESS)) {
                    commodity_t ingot = COMMODITY_FERRITE_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->cargo[ingot] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
                    }
                }
                if (!loaded && station_has_module(dest, MODULE_LASER_FAB)) {
                    commodity_t ingot = COMMODITY_CUPRITE_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->cargo[ingot] += take;
                        home->inventory[ingot] -= take;
                        space -= take;
                        loaded = true;
                    }
                }
                if (!loaded && station_has_module(dest, MODULE_TRACTOR_FAB)) {
                    commodity_t ingot = COMMODITY_CRYSTAL_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->cargo[ingot] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
                    }
                }
            }
            float total_carried = 0.0f;
            for (int c = 0; c < COMMODITY_COUNT; c++) total_carried += npc->cargo[c];
            if (total_carried < 0.01f) {
                /* Nothing at home — relocate to a station with surplus ingots */
                int best_src = -1;
                float best_stock = 0.0f;
                for (int s = 0; s < MAX_STATIONS; s++) {
                    if (s == npc->home_station) continue;
                    if (!station_is_active(&w->stations[s])) continue;
                    float stock = 0.0f;
                    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                        stock += fmaxf(0.0f, w->stations[s].inventory[c] - HAULER_RESERVE);
                    if (stock > best_stock) { best_stock = stock; best_src = s; }
                }
                if (best_src >= 0 && best_stock > 0.5f) {
                    /* Relocate: fly to the surplus station, dock, and load next cycle */
                    npc->home_station = best_src;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else {
                    npc->state_timer = HAULER_DOCK_TIME;  /* nothing anywhere, wait */
                }
            } else {
                npc->state = NPC_STATE_TRAVEL_TO_DEST;
            }
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        station_t *dest = &w->stations[npc->dest_station];
        vec2 approach = station_approach_target(dest, npc->pos);
        npc_steer_with_path(w, n, npc, approach, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt, w);
        float dock_r = dest->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, dest->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(dest->pos, v2(30.0f * (float)(n % 2 == 0 ? -1 : 1), -(dest->radius + hull->ship_radius + 50.0f)));
            npc->state = NPC_STATE_UNLOADING;
            npc->state_timer = HAULER_LOAD_TIME;
        }
        break;
    }
    case NPC_STATE_UNLOADING: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            station_t *dest = &w->stations[npc->dest_station];
            for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
                dest->inventory[i] += npc->cargo[i];
                if (dest->inventory[i] > MAX_PRODUCT_STOCK)
                    dest->inventory[i] = MAX_PRODUCT_STOCK;
                npc->cargo[i] = 0.0f;
            }
            /* Hauler also delivers ingots to scaffold station and modules */
            if (dest->scaffold || dest->module_count > 0) {
                /* Feed from station inventory into scaffolds */
                ship_t hauler_ship = {0};
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                    hauler_ship.cargo[c] = dest->inventory[c];
                if (dest->scaffold) {
                    float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - dest->scaffold_progress);
                    float deliver = fminf(hauler_ship.cargo[COMMODITY_FRAME], needed);
                    if (deliver > 0.01f) {
                        hauler_ship.cargo[COMMODITY_FRAME] -= deliver;
                        dest->scaffold_progress += deliver / SCAFFOLD_MATERIAL_NEEDED;
                        if (dest->scaffold_progress >= 1.0f)
                            activate_outpost(w, npc->dest_station);
                    }
                }
                step_module_delivery(w, dest, npc->dest_station, &hauler_ship);
                /* Put remaining back */
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                    float consumed = dest->inventory[c] - hauler_ship.cargo[c];
                    if (consumed > 0.01f) dest->inventory[c] -= consumed;
                }
            }
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        vec2 approach_home = station_approach_target(home, npc->pos);
        npc_steer_with_path(w, n, npc, approach_home, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt, w);
        float dock_r = home->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, home->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(home->pos, v2(50.0f * (float)(n % 2 == 0 ? -1 : 1), -(home->radius + hull->ship_radius + 70.0f)));
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

/* Find an open ring slot at any active player outpost (s >= 3) that
 * matches the given module type. Used by tow drones to pick a delivery
 * destination for a loose scaffold. Returns -1 if none. */
static int find_destination_for_scaffold(const world_t *w, module_type_t type) {
    /* Pass 1: outposts (active OR planned) with a placement plan for
     * this type — those are slots the player explicitly reserved. A
     * planned outpost is a valid destination too: when the scaffold
     * arrives the planned ghost can be promoted via the existing
     * snap-to-slot logic, with the relay as its founding module. */
    for (int s = 3; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            if (st->placement_plans[p].type == type) return s;
        }
    }
    /* Pass 2: any active outpost with at least one open ring slot. */
    for (int s = 3; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
            if (ring > 1 && !ring_has_dock(st, ring - 1)) continue;
            if (station_ring_free_slot(st, ring, STATION_RING_SLOTS[ring]) >= 0)
                return s;
        }
    }
    /* Pass 3: SIGNAL_RELAY is special — it founds new outposts. If the
     * player has a planned (ghost) outpost waiting, deliver the relay
     * there even without an explicit placement plan, so the chicken-
     * and-egg of "first relay needs an outpost that needs a relay" is
     * resolved by the drone. */
    if (type == MODULE_SIGNAL_RELAY) {
        for (int s = 3; s < MAX_STATIONS; s++) {
            const station_t *st = &w->stations[s];
            if (st->planned) return s;
        }
    }
    return -1;
}

/* Find a loose scaffold near this NPC's home station that has a known
 * destination. Returns scaffold index or -1. */
static int find_loose_scaffold_for_tow(const world_t *w, const npc_ship_t *npc) {
    const station_t *home = &w->stations[npc->home_station];
    const float pickup_range_sq = 4000.0f * 4000.0f;
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        if (sc->state != SCAFFOLD_LOOSE) continue;
        /* Skip scaffolds being towed by a player or another drone */
        if (sc->towed_by >= 0) continue;
        /* Must be near the home shipyard */
        float d_home = v2_dist_sq(sc->pos, home->pos);
        if (d_home > pickup_range_sq) continue;
        /* Must have a place to deliver */
        if (find_destination_for_scaffold(w, sc->module_type) < 0) continue;
        if (d_home < best_d) { best_d = d_home; best = i; }
    }
    return best;
}

/* Tow drone: spawned at shipyards, picks up loose scaffolds, delivers
 * them to player outposts with placement plans, returns home. Reuses
 * the existing NPC state enum but interprets the states for tow logic.
 *
 *   DOCKED → look for a loose scaffold + matching destination
 *   TRAVEL_TO_ASTEROID → fly to scaffold position (ASTEROID = "thing to grab")
 *   MINING → grab phase: tractor it, set towed_scaffold
 *   TRAVEL_TO_DEST → tow it to destination outpost
 *   UNLOADING → release near open slot, let it snap
 *   RETURN_TO_STATION → fly back to home shipyard
 */
static void step_tow_drone(world_t *w, npc_ship_t *npc, int n, float dt) {
    (void)n;
    const hull_def_t *hull = npc_hull_def(npc);

    /* If we lost our towed scaffold mid-flight (destroyed, snapped early,
     * picked up by a player), drop back to idle. */
    if (npc->towed_scaffold >= 0) {
        scaffold_t *sc = &w->scaffolds[npc->towed_scaffold];
        if (!sc->active || sc->state == SCAFFOLD_PLACED ||
            sc->state == SCAFFOLD_SNAPPING || sc->towed_by != -2 - n) {
            npc->towed_scaffold = -1;
            if (npc->state == NPC_STATE_TRAVEL_TO_DEST ||
                npc->state == NPC_STATE_UNLOADING) {
                npc->state = NPC_STATE_RETURN_TO_STATION;
            }
        }
    }

    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer > 0.0f) break;
        int sc_idx = find_loose_scaffold_for_tow(w, npc);
        if (sc_idx < 0) {
            npc->state_timer = 2.0f; /* nothing to tow, idle and recheck */
            break;
        }
        npc->target_asteroid = sc_idx;  /* repurpose: scaffold idx for tow */
        npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
        break;
    }
    case NPC_STATE_TRAVEL_TO_ASTEROID: {
        if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_SCAFFOLDS) {
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
        }
        scaffold_t *sc = &w->scaffolds[npc->target_asteroid];
        if (!sc->active || sc->state != SCAFFOLD_LOOSE || sc->towed_by >= 0) {
            npc->target_asteroid = -1;
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
        }
        npc_steer_with_path(w, n, npc, sc->pos, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt, w);
        if (v2_dist_sq(npc->pos, sc->pos) < 80.0f * 80.0f) {
            /* Grab — claim the scaffold and switch to tow mode.
             * Use towed_by = -2 - drone_index so positive values keep
             * meaning "player id" and negative values < -1 mean "drone n". */
            sc->towed_by = -2 - n;
            sc->state = SCAFFOLD_TOWING;
            npc->towed_scaffold = npc->target_asteroid;
            int dest = find_destination_for_scaffold(w, sc->module_type);
            if (dest < 0) {
                /* Destination vanished while we were en route; drop and reset */
                sc->towed_by = -1;
                sc->state = SCAFFOLD_LOOSE;
                npc->towed_scaffold = -1;
                npc->target_asteroid = -1;
                npc->state = NPC_STATE_DOCKED;
                npc->state_timer = HAULER_DOCK_TIME;
                break;
            }
            npc->dest_station = dest;
            npc->state = NPC_STATE_TRAVEL_TO_DEST;
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        if (npc->towed_scaffold < 0 ||
            npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS) {
            npc->state = NPC_STATE_RETURN_TO_STATION;
            break;
        }
        scaffold_t *sc = &w->scaffolds[npc->towed_scaffold];
        station_t *dest = &w->stations[npc->dest_station];
        /* Drag the scaffold along behind us with simple spring chase. */
        vec2 to_drone = v2_sub(npc->pos, sc->pos);
        float td = sqrtf(v2_len_sq(to_drone));
        float tow_dist = 60.0f;
        if (td > tow_dist && td > 0.1f) {
            vec2 dir = v2_scale(to_drone, 1.0f / td);
            float over = td - tow_dist;
            sc->vel = v2_add(sc->vel, v2_scale(dir, over * 8.0f * dt));
        }
        sc->vel = v2_scale(sc->vel, 1.0f / (1.0f + 0.6f * dt));
        sc->pos = v2_add(sc->pos, v2_scale(sc->vel, dt));

        vec2 approach = station_approach_target(dest, npc->pos);
        npc_steer_with_path(w, n, npc, approach, hull->accel * 0.6f, hull->turn_speed, dt);
        /* Speed cap while towing — heavy load */
        float spd = v2_len(npc->vel);
        if (spd > 60.0f) npc->vel = v2_scale(npc->vel, 60.0f / spd);
        npc_apply_physics(npc, hull->drag, dt, w);
        if (v2_dist_sq(npc->pos, dest->pos) < 600.0f * 600.0f) {
            /* Release — let the existing snap-to-slot logic in step_scaffolds
             * pick up the loose scaffold near the outpost ring. */
            sc->towed_by = -1;
            sc->state = SCAFFOLD_LOOSE;
            npc->towed_scaffold = -1;
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        vec2 approach = station_approach_target(home, npc->pos);
        npc_steer_with_path(w, n, npc, approach, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt, w);
        if (v2_dist_sq(npc->pos, home->pos) < (home->dock_radius * 0.7f) * (home->dock_radius * 0.7f)) {
            npc->vel = v2(0.0f, 0.0f);
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

void step_npc_ships(world_t *w, float dt) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        npc->thrusting = false;
        npc_validate_stations(w, npc);

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            continue;
        }
        if (npc->role == NPC_ROLE_TOW) {
            step_tow_drone(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            continue;
        }

        const hull_def_t *hull = npc_hull_def(npc);
        switch (npc->state) {
        case NPC_STATE_DOCKED: {
            npc->state_timer -= dt;
            npc->vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) {
                    npc->target_asteroid = target;
                    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                } else {
                    npc->state = NPC_STATE_IDLE;
                    npc->state_timer = 2.0f;
                }
            }
            break;
        }
        case NPC_STATE_TRAVEL_TO_ASTEROID: {
            if (!npc_target_valid(w, npc)) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) npc->target_asteroid = target;
                else { npc->target_asteroid = -1; npc->state = NPC_STATE_RETURN_TO_STATION; break; }
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            npc_steer_with_path(w, n, npc, a->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt, w);
            if (v2_dist_sq(npc->pos, a->pos) < MINING_RANGE * MINING_RANGE)
                npc->state = NPC_STATE_MINING;
            break;
        }
        case NPC_STATE_MINING: {
            if (!npc_target_valid(w, npc)) {
                /* Target gone — look for a fragment to tow, or find new target */
                if (npc->towed_fragment >= 0) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else {
                    int target = npc_find_mineable_asteroid(w, npc);
                    if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                    else npc->state = NPC_STATE_RETURN_TO_STATION;
                }
                break;
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            float dist_sq = v2_dist_sq(npc->pos, a->pos);
            float standoff = a->radius + 60.0f;
            float approach_r = standoff + 20.0f;

            if (dist_sq > approach_r * approach_r) {
                npc_steer_toward(npc, a->pos, hull->accel, hull->turn_speed, dt);
                npc_apply_physics(npc, hull->drag, dt, w);
                break;
            }

            vec2 face_dir = v2_sub(a->pos, npc->pos);
            float desired = atan2f(face_dir.y, face_dir.x);
            float diff = wrap_angle(desired - npc->angle);
            float max_turn = hull->turn_speed * dt;
            if (diff > max_turn) diff = max_turn;
            else if (diff < -max_turn) diff = -max_turn;
            npc->angle = wrap_angle(npc->angle + diff);

            if (dist_sq < standoff * standoff) {
                vec2 away = v2_norm(v2_sub(npc->pos, a->pos));
                npc->vel = v2_add(npc->vel, v2_scale(away, hull->accel * 0.5f * dt));
            }
            npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (4.0f * dt)));
            npc_apply_physics(npc, hull->drag, dt, w);

            float mined = hull->mining_rate * dt;
            mined = fminf(mined, a->hp);
            a->hp -= mined;
            a->net_dirty = true;

            if (a->hp <= 0.01f) {
                vec2 outward = v2_norm(v2_sub(a->pos, npc->pos));
                fracture_asteroid(w, npc->target_asteroid, outward, -1);
                npc->target_asteroid = -1;

                /* Grab the nearest S-tier fragment to tow home */
                float best_frag_d = 200.0f * 200.0f;
                int best_frag = -1;
                for (int fi = 0; fi < MAX_ASTEROIDS; fi++) {
                    asteroid_t *f = &w->asteroids[fi];
                    if (!f->active || f->tier != ASTEROID_TIER_S) continue;
                    float fd = v2_dist_sq(npc->pos, f->pos);
                    if (fd < best_frag_d) { best_frag_d = fd; best_frag = fi; }
                }
                if (best_frag >= 0) {
                    npc->towed_fragment = best_frag;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                }
            }
            break;
        }
        case NPC_STATE_RETURN_TO_STATION: {
            station_t *home = &w->stations[npc->home_station];

            /* Find the nearest furnace on this station to deliver to */
            vec2 delivery_target = home->pos;
            for (int fm = 0; fm < home->module_count; fm++) {
                module_type_t fmt = home->modules[fm].type;
                if (fmt != MODULE_FURNACE && fmt != MODULE_FURNACE_CU && fmt != MODULE_FURNACE_CR) continue;
                if (home->modules[fm].scaffold) continue;
                delivery_target = module_world_pos_ring(home, home->modules[fm].ring, home->modules[fm].slot);
                break;
            }

            /* Slow down when towing so the fragment can keep up */
            float tow_accel = hull->accel;
            if (npc->towed_fragment >= 0) tow_accel *= 0.5f;
            npc_steer_with_path(w, n, npc, delivery_target, tow_accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt, w);

            /* Speed cap when towing */
            if (npc->towed_fragment >= 0) {
                float spd = v2_len(npc->vel);
                float max_tow_speed = 80.0f;
                if (spd > max_tow_speed)
                    npc->vel = v2_scale(npc->vel, max_tow_speed / spd);
            }

            /* Tow the fragment — drag it along with spring physics */
            if (npc->towed_fragment >= 0 && npc->towed_fragment < MAX_ASTEROIDS) {
                asteroid_t *tow = &w->asteroids[npc->towed_fragment];
                if (tow->active) {
                    vec2 to_npc = v2_sub(npc->pos, tow->pos);
                    float td = sqrtf(v2_len_sq(to_npc));
                    float safe = 40.0f + tow->radius;
                    if (td > safe && td > 0.1f) {
                        vec2 pull_dir = v2_scale(to_npc, 1.0f / td);
                        tow->vel = v2_add(tow->vel, v2_scale(pull_dir, 500.0f * dt));
                        tow->vel = v2_scale(tow->vel, 1.0f / (1.0f + 3.0f * dt));
                        float spd = v2_len(tow->vel);
                        if (spd > 150.0f) tow->vel = v2_scale(tow->vel, 150.0f / spd);
                    }
                    /* Release when close to the furnace — let the furnace tractor take over */
                    float furnace_d = v2_dist_sq(tow->pos, delivery_target);
                    if (furnace_d < 150.0f * 150.0f) {
                        npc->towed_fragment = -1;
                    }
                } else {
                    npc->towed_fragment = -1;
                }
            }

            /* Once fragment is delivered (or lost), go find more ore */
            if (npc->towed_fragment < 0) {
                /* Drift away from the furnace, then look for next target */
                npc->state = NPC_STATE_IDLE;
                npc->state_timer = 2.0f;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_IDLE: {
            npc_apply_physics(npc, hull->drag, dt, w);
            npc->state_timer -= dt;
            if (npc->state_timer <= 0.0f) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                else npc->state_timer = 3.0f;
            }
            break;
        }
        default: break;
        }

        /* NPC collision with stations and asteroids */
        if (npc->state != NPC_STATE_DOCKED) {
            npc_resolve_station_collisions(w, npc);
            npc_resolve_asteroid_collisions(w, npc);
        }

        /* Blend tint toward dominant cargo color.
         * Ore colors: ferrite=(0.55, 0.25, 0.18), cuprite=(0.22, 0.30, 0.50), crystal=(0.25, 0.48, 0.30) */
        static const float ore_r[3] = {0.55f, 0.22f, 0.25f};
        static const float ore_g[3] = {0.25f, 0.30f, 0.48f};
        static const float ore_b[3] = {0.18f, 0.50f, 0.30f};
        float total = 0.0f;
        float target_r = 1.0f, target_g = 1.0f, target_b = 1.0f;
        {
            int base = (npc->role == NPC_ROLE_MINER) ? 0 : COMMODITY_RAW_ORE_COUNT;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) total += npc->cargo[base + c];
        }
        if (total > 1.0f) {
            target_r = 0.0f; target_g = 0.0f; target_b = 0.0f;
            int base = (npc->role == NPC_ROLE_MINER) ? 0 : COMMODITY_RAW_ORE_COUNT;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                float w_c = npc->cargo[base + c] / total;
                target_r += ore_r[c] * w_c;
                target_g += ore_g[c] * w_c;
                target_b += ore_b[c] * w_c;
            }
        }
        float blend = 0.3f * dt;  /* slow blend toward cargo color */
        npc->tint_r = lerpf(npc->tint_r, target_r, blend);
        npc->tint_g = lerpf(npc->tint_g, target_g, blend);
        npc->tint_b = lerpf(npc->tint_b, target_b, blend);
    }
}

/* Generate DESTROY contracts for asteroids blocking stuck NPCs. */
void generate_npc_distress_contracts(world_t *w) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* Only haulers in transit can get stuck */
        if (npc->role != NPC_ROLE_HAULER) continue;
        if (npc->state != NPC_STATE_TRAVEL_TO_DEST && npc->state != NPC_STATE_RETURN_TO_STATION) continue;
        /* Check if stuck: low speed for a while (state_timer repurposed — skip if fresh) */
        float speed = v2_len(npc->vel);
        if (speed > 15.0f) continue;
        /* Find nearest blocking asteroid */
        int blocker = -1;
        float best_d = 200.0f * 200.0f; /* within 200u */
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (!w->asteroids[i].active || asteroid_is_collectible(&w->asteroids[i])) continue;
            float d = v2_dist_sq(npc->pos, w->asteroids[i].pos);
            if (d < best_d) { best_d = d; blocker = i; }
        }
        if (blocker < 0) continue;
        /* Check if a DESTROY contract already exists for this asteroid */
        bool exists = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (w->contracts[k].active && w->contracts[k].action == CONTRACT_FRACTURE
                && w->contracts[k].target_index == blocker) {
                exists = true; break;
            }
        }
        if (exists) continue;
        /* Post distress contract */
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active) {
                w->contracts[k] = (contract_t){
                    .active = true, .action = CONTRACT_FRACTURE,
                    .station_index = (uint8_t)npc->home_station,
                    .target_pos = w->asteroids[blocker].pos,
                    .target_index = blocker,
                    .base_price = 20.0f, .age = 0.0f,
                    .claimed_by = -1,
                };
                break;
            }
        }
    }
}
