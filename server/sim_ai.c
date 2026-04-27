/*
 * sim_ai.c -- NPC ship subsystem.
 * Extracted from game_sim.c: target finding, steering, physics,
 * spawn, state machines (MINER / HAULER / TOW), and the per-tick
 * step_npc_ships() dispatcher.
 */
#include "sim_ai.h"
#include "sim_nav.h"
#include "sim_flight.h"
#include "signal_model.h"
#include "manifest.h"
#include "ship.h"
#include "game_sim.h" /* SHIP_COLLISION_DAMAGE_THRESHOLD/_SCALE */
#include <math.h>
#include <string.h>

/* Remove up to `n` cargo units of `c` from a station's manifest.
 * Returns the number actually removed. Walks backward so removing
 * doesn't disturb earlier indices. Used by NPC haulers so the
 * manifest stays in lockstep with the inventory float; otherwise the
 * trade picker (manifest-only) shows phantom rows for stock the
 * hauler already carried away. */
static int station_manifest_drain_commodity(station_t *st, commodity_t c, int n) {
    if (!st || !st->manifest.units || n <= 0) return 0;
    int removed = 0;
    for (int16_t i = (int16_t)st->manifest.count - 1; i >= 0 && removed < n; i--) {
        if (st->manifest.units[i].commodity == (uint8_t)c) {
            if (manifest_remove(&st->manifest, (uint16_t)i, NULL)) removed++;
        }
    }
    return removed;
}

/* Inverse: push `n` synthetic legacy-migrate units of `c` into a
 * station's manifest. Used at NPC unload until haulers get their own
 * manifest_t. The origin is per-hauler so the units are traceable to
 * "delivered by NPC slot K". */
static int station_manifest_seed_from_npc(station_t *st, commodity_t c, int n,
                                          int npc_slot) {
    if (!st || n <= 0) return 0;
    if (st->manifest.cap == 0 && !station_manifest_bootstrap(st)) return 0;
    uint8_t origin[8] = { 'N','P','C','D','0','0','0','0' };
    origin[7] = (uint8_t)('0' + (npc_slot % 10));
    int pushed = 0;
    for (int i = 0; i < n; i++) {
        if (st->manifest.count >= st->manifest.cap) break;
        cargo_unit_t unit = {0};
        if (!hash_legacy_migrate_unit(origin, c, (uint16_t)i, &unit)) continue;
        if (!manifest_push(&st->manifest, &unit)) break;
        pushed++;
    }
    return pushed;
}

/* ================================================================== */
/* NPC ships                                                          */
/* ================================================================== */

/* #294 Slice 6: paired character_t lifecycle.
 *
 * Each active NPC gets a paired character_t entry; future slices flip
 * the source-of-truth for brain state and damage routing onto it.
 * `ship_idx` carries the NPC slot during the transition — once the
 * unified ships[] pool lands, it'll point there instead.
 *
 * Nothing reads the character pool yet. These writes are intentionally
 * "dead" so the lifecycle is observable in saves/wire without flipping
 * any readers in the same slice. */
static character_kind_t character_kind_from_role(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return CHARACTER_KIND_NPC_MINER;
    case NPC_ROLE_HAULER: return CHARACTER_KIND_NPC_HAULER;
    case NPC_ROLE_TOW:    return CHARACTER_KIND_NPC_TOW;
    default:              return CHARACTER_KIND_NONE;
    }
}

/* Find a free ships[] slot — one not pointed to by any active character.
 * Returns -1 if the pool is full. */
static int ship_pool_alloc_slot(const world_t *w) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int s = 0; s < MAX_SHIPS; s++) {
        bool taken = false;
        for (int i = 0; i < cap; i++) {
            if (w->characters[i].active && w->characters[i].ship_idx == s) {
                taken = true;
                break;
            }
        }
        if (!taken) return s;
    }
    return -1;
}

/* Initialize a ships[] slot from an NPC's snapshot. Frees any prior
 * manifest the slot was holding so this is safe for a slot that was
 * previously occupied (e.g. after rebuild_characters_from_npcs). */
static void ship_pool_init_from_npc(ship_t *ship, const npc_ship_t *npc) {
    ship_cleanup(ship);
    memset(ship, 0, sizeof(*ship));
    (void)ship_manifest_bootstrap(ship);
    ship->pos = npc->pos;
    ship->vel = npc->vel;
    ship->angle = npc->angle;
    ship->hull_class = npc->hull_class;
    ship->hull = npc->hull;
}

static int character_alloc_for_npc(world_t *w, int npc_slot, const npc_ship_t *npc) {
    (void)npc_slot; /* ship_idx now points to the unified ships[] pool, not npc_slot. */
    int ship_slot = ship_pool_alloc_slot(w);
    if (ship_slot < 0) return -1;
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (w->characters[i].active) continue;
        character_t *c = &w->characters[i];
        memset(c, 0, sizeof(*c));
        c->active = true;
        c->kind = character_kind_from_role(npc->role);
        c->ship_idx = ship_slot;
        c->state = npc->state;
        c->target_asteroid = npc->target_asteroid;
        c->home_station = npc->home_station;
        c->dest_station = npc->dest_station;
        c->state_timer = npc->state_timer;
        c->towed_fragment = npc->towed_fragment;
        c->towed_scaffold = npc->towed_scaffold;
        ship_pool_init_from_npc(&w->ships[ship_slot], npc);
        return i;
    }
    /* No free character slot; release the ship slot we reserved. */
    ship_cleanup(&w->ships[ship_slot]);
    return -1;
}

/* Find the paired character for an NPC slot, or -1. */
static int character_for_npc_slot(const world_t *w, int npc_slot) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        const character_t *c = &w->characters[i];
        if (!c->active) continue;
        if (c->ship_idx != npc_slot) continue;
        if (c->kind != CHARACTER_KIND_NPC_MINER &&
            c->kind != CHARACTER_KIND_NPC_HAULER &&
            c->kind != CHARACTER_KIND_NPC_TOW) continue;
        return i;
    }
    return -1;
}

static void character_free_for_npc(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return;
    int ship_slot = w->characters[idx].ship_idx;
    if (ship_slot >= 0 && ship_slot < MAX_SHIPS) {
        ship_cleanup(&w->ships[ship_slot]);
        memset(&w->ships[ship_slot], 0, sizeof(w->ships[ship_slot]));
    }
    w->characters[idx].active = false;
}

/* Resolve an NPC slot to its paired ship, or NULL if no character is
 * paired or ship_idx is out of range. */
static ship_t *npc_ship_for(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return NULL;
    int s = w->characters[idx].ship_idx;
    if (s < 0 || s >= MAX_SHIPS) return NULL;
    return &w->ships[s];
}

/* Reverse mirror: push ship.hull back into npc.hull at the end of an
 * NPC's tick. Lets damage written through the ship layer (the player
 * path's substrate) flow into the npc's despawn check next tick.
 * Physics fields stay npc-authoritative for now — dispatch writes
 * pos/vel/angle on the npc side and the top-of-tick mirror keeps the
 * ship in sync; flipping dispatch onto ship_t is a later slice. */
static void mirror_ship_to_npc(world_t *w, int npc_slot) {
    const ship_t *s = npc_ship_for(w, npc_slot);
    if (!s) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    npc->hull = s->hull;
}

/* Apply damage to an NPC by mutating its ship_t.hull. The reverse
 * mirror at end-of-tick pushes the result into npc->hull so the
 * existing despawn check fires when hull <= 0.
 *
 * Public: external code (rock-throw collision, PvP, etc.) reaches NPC
 * damage through this helper so the unified ship_t.hull stays the
 * single source of truth. */
void apply_npc_ship_damage(world_t *w, int npc_slot, float dmg) {
    if (dmg <= 0.0f) return;
    ship_t *s = npc_ship_for(w, npc_slot);
    if (!s) {
        /* Fallback: paired ship missing; mutate the npc directly so
         * we don't silently swallow damage. */
        w->npc_ships[npc_slot].hull -= dmg;
        if (w->npc_ships[npc_slot].hull < 0.0f) w->npc_ships[npc_slot].hull = 0.0f;
        return;
    }
    s->hull -= dmg;
    if (s->hull < 0.0f) s->hull = 0.0f;
}

/* Mirror brain state from an NPC into its paired character_t (#294
 * Slice 7) AND physics state into its paired ship_t (#294 Slice 8).
 * Called at the top of each NPC's tick so future readers can trust the
 * controller + ship layer; the npc-side fields remain the source of
 * truth that the dispatch switch writes back to. */
static void mirror_npc_to_character(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return;
    const npc_ship_t *npc = &w->npc_ships[npc_slot];
    character_t *c = &w->characters[idx];
    c->state = npc->state;
    c->target_asteroid = npc->target_asteroid;
    c->home_station = npc->home_station;
    c->dest_station = npc->dest_station;
    c->state_timer = npc->state_timer;
    c->towed_fragment = npc->towed_fragment;
    c->towed_scaffold = npc->towed_scaffold;
    if (c->ship_idx >= 0 && c->ship_idx < MAX_SHIPS) {
        ship_t *s = &w->ships[c->ship_idx];
        s->pos = npc->pos;
        s->vel = npc->vel;
        s->angle = npc->angle;
        s->hull = npc->hull;
        s->hull_class = npc->hull_class;
    }
}

void rebuild_characters_from_npcs(world_t *w) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (w->characters[i].kind == CHARACTER_KIND_NPC_MINER ||
            w->characters[i].kind == CHARACTER_KIND_NPC_HAULER ||
            w->characters[i].kind == CHARACTER_KIND_NPC_TOW) {
            w->characters[i].active = false;
        }
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        const npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        (void)character_alloc_for_npc(w, n, npc);
    }
}

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
    /* Clear stale path from previous occupant of this slot. */
    *nav_npc_path(slot) = (nav_path_t){0};
    npc->active = true;
    npc->role = role;
    npc->hull_class = hc;
    npc->state = NPC_STATE_DOCKED;
    npc->pos = v2_add(st->pos, v2(30.0f * (float)(slot % 3 - 1), -(st->radius + hull_def_for_class(hc)->ship_radius + 50.0f)));
    npc->angle = PI_F * 0.5f;
    npc->target_asteroid = -1;
    npc->towed_fragment = -1;
    npc->towed_scaffold = -1;
    npc->home_station = station_idx;
    npc->dest_station = station_idx;
    npc->state_timer = (role == NPC_ROLE_MINER) ? NPC_DOCK_TIME : HAULER_DOCK_TIME;
    npc->hull = npc_max_hull(npc);
    npc->tint_r = 1.0f; npc->tint_g = 1.0f; npc->tint_b = 1.0f;
    /* Tow drones get a distinct yellow-amber tint */
    if (role == NPC_ROLE_TOW) {
        npc->tint_r = 1.0f; npc->tint_g = 0.85f; npc->tint_b = 0.30f;
    }
    /* Pair a character_t with the NPC. Lifecycle-only — nothing reads
     * it yet (#294 Slice 6). If the pool is somehow exhausted we still
     * spawn the NPC; this is best-effort during the transition. */
    (void)character_alloc_for_npc(w, slot, npc);
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

/* Asteroid-already-taken check, reading from the controller layer
 * (#294 Slice 7+8): scan characters[] for any other MINER targeting
 * `target_idx`. The mirror at top of tick keeps character.target_asteroid
 * in sync with npc.target_asteroid. `self_char_idx` is excluded so the
 * caller doesn't see itself as a competitor. */
static bool miner_target_taken(const world_t *w, int target_idx, int self_char_idx) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (i == self_char_idx) continue;
        const character_t *c = &w->characters[i];
        if (!c->active || c->kind != CHARACTER_KIND_NPC_MINER) continue;
        if (c->target_asteroid == target_idx) return true;
    }
    return false;
}

static int npc_find_mineable_asteroid(const world_t *w, const npc_ship_t *npc) {
    int self_npc_slot = (int)(npc - w->npc_ships);
    int self_char = character_for_npc_slot(w, self_npc_slot);

    /* Priority: DESTROY contract targets first */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active || w->contracts[k].action != CONTRACT_FRACTURE) continue;
        int idx = w->contracts[k].target_index;
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
        if (!miner_target_taken(w, idx, self_char)) return idx;
    }

    /* Normal: find nearest mineable asteroid */
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        if (signal_npc_confidence(signal_strength_at(w, a->pos)) < 0.1f) continue;
        if (miner_target_taken(w, i, self_char)) continue;
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

/* Build a transient ship_t view of an NPC so the shared flight_*
 * controllers (which read pos/vel/angle/hull_class off ship_t) can be
 * called against an npc_ship_t. Strictly read-only on the caller side:
 * mutations land on the npc via npc_apply_flight_cmd, not on the view.
 * Goes away in the slice that embeds ship_t inside npc_ship_t. */
static ship_t ship_view_from_npc(const npc_ship_t *npc) {
    ship_t v = {0};
    v.pos = npc->pos;
    v.vel = npc->vel;
    v.angle = npc->angle;
    v.hull_class = npc->hull_class;
    return v;
}

/* Apply a normalized flight_cmd_t (turn/thrust each in -1..1) to an NPC.
 * Rate-limits turn by turn_speed; gates thrust to forward acceleration only.
 * Caller owns physics integration (npc_apply_physics) and any thrust<0
 * handling (e.g. hover-specific brake-away-from-target). */
static void npc_apply_flight_cmd(npc_ship_t *npc, flight_cmd_t cmd,
                                  float accel, float turn_speed, float dt) {
    float max_turn = turn_speed * dt;
    float turn_angle = cmd.turn * turn_speed * dt;
    if (turn_angle > max_turn) turn_angle = max_turn;
    if (turn_angle < -max_turn) turn_angle = -max_turn;
    npc->angle = wrap_angle(npc->angle + turn_angle);

    float thrust_gate = (cmd.thrust > 0.0f) ? cmd.thrust : 0.0f;
    vec2 fwd = v2_from_angle(npc->angle);
    npc->vel = v2_add(npc->vel, v2_scale(fwd, accel * thrust_gate * dt));
    npc->thrusting = thrust_gate > 0.0f;
}

/* A*-guided NPC steering via the shared flight controller.
 * Creates a temporary ship_t so flight_steer_to can read pos/vel/angle/hull_class.
 * Phase 2 will give NPCs a real ship_t; this is intentionally transitional. */
static void npc_steer_with_path(const world_t *w, int npc_idx, npc_ship_t *npc,
                                vec2 final_target, float accel, float turn_speed, float dt) {
    ship_t view = ship_view_from_npc(npc);
    nav_path_t *path = nav_npc_path(npc_idx);
    flight_cmd_t cmd = flight_steer_to(w, &view, path, final_target,
                                        0.0f, 200.0f, dt);
    npc_apply_flight_cmd(npc, cmd, accel, turn_speed, dt);
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
        if (vel_toward < 0.0f) {
            float impact = -vel_toward;
            npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.0f));
            /* Same formula as players (collision_damage_for in game_sim.h).
             * NPCs feeding the kit-demand sink is the load-bearing reason
             * the kit economy exists at all. */
            float dmg = collision_damage_for(impact, 1.0f);
            int npc_slot = (int)(npc - w->npc_ships);
            apply_npc_ship_damage(w, npc_slot, dmg);
        }
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
    if (npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS)
        npc->dest_station = npc->home_station;
    /* Tow drones can deliver to planned stations (blueprints) which are
     * not active yet. Only reset dest for non-tow roles. */
    else if (npc->role != NPC_ROLE_TOW &&
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
                    int whole = (int)floorf(take + 0.0001f);
                    if (whole > 0) {
                        if (station_manifest_drain_commodity(home, ingot, whole) > 0)
                            home->named_ingots_dirty = true;
                    }
                }
            } else {
                /* Fallback: original round-trip behavior (leave reserve for players) */
                station_t *dest = &w->stations[npc->dest_station];
                /* Sized for: FRAME_PRESS (FE) + LASER_FAB (CU+CR) +
                 * TRACTOR_FAB (CU). Original code had wants[3] which
                 * stack-overflowed when a station had FRAME_PRESS +
                 * LASER_FAB + TRACTOR_FAB (Kepler post-#367). */
                commodity_t wants[4];
                int want_count = 0;
                commodity_t best_ingot = COMMODITY_COUNT;
                float best_need = -1.0f;

                if (station_has_module(dest, MODULE_FRAME_PRESS))
                    wants[want_count++] = COMMODITY_FERRITE_INGOT;
                if (station_has_module(dest, MODULE_LASER_FAB)) {
                    wants[want_count++] = COMMODITY_CUPRITE_INGOT;
                    wants[want_count++] = COMMODITY_CRYSTAL_INGOT;
                }
                if (station_has_module(dest, MODULE_TRACTOR_FAB))
                    wants[want_count++] = COMMODITY_CUPRITE_INGOT;

                for (int wi = 0; wi < want_count; wi++) {
                    commodity_t ingot = wants[wi];
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float need;
                    bool seen = false;

                    for (int wj = 0; wj < wi; wj++) {
                        if (wants[wj] == ingot) { seen = true; break; }
                    }
                    if (seen || avail <= 0.5f) continue;

                    need = fmaxf(0.0f, MAX_PRODUCT_STOCK * 0.5f - dest->inventory[ingot]);
                    if (need > best_need) {
                        best_need = need;
                        best_ingot = ingot;
                    }
                }

                if (best_ingot < COMMODITY_COUNT) {
                    float avail = fmaxf(0.0f, home->inventory[best_ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->cargo[best_ingot] += take;
                        home->inventory[best_ingot] -= take;
                        int whole = (int)floorf(take + 0.0001f);
                        if (whole > 0) {
                            if (station_manifest_drain_commodity(home, best_ingot, whole) > 0)
                                home->named_ingots_dirty = true;
                        }
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
                if (npc->cargo[i] <= 0.0f) continue;
                float before = dest->inventory[i];
                dest->inventory[i] += npc->cargo[i];
                if (dest->inventory[i] > MAX_PRODUCT_STOCK)
                    dest->inventory[i] = MAX_PRODUCT_STOCK;
                /* Mirror the float bump into the manifest so the trade
                 * picker (manifest-only) sees the new stock. Use the
                 * post-clamp delta so overflow doesn't create phantom
                 * manifest entries. */
                int int_delta = (int)floorf(dest->inventory[i] + 0.0001f)
                              - (int)floorf(before + 0.0001f);
                if (int_delta > 0) {
                    if (station_manifest_seed_from_npc(dest, (commodity_t)i,
                                                       int_delta, n) > 0)
                        dest->named_ingots_dirty = true;
                }
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
                step_module_delivery(w, dest, npc->dest_station, &hauler_ship, COMMODITY_COUNT);
                /* Put remaining back. The float was drained into
                 * module_input by step_module_delivery; we have to drain
                 * the matching manifest entries too or the BUY picker
                 * (manifest-only) will keep advertising stock that the
                 * server-side float check (game_sim.c try_buy_product)
                 * sees as 0 and silently rejects. */
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                    float consumed = dest->inventory[c] - hauler_ship.cargo[c];
                    if (consumed > 0.01f) {
                        dest->inventory[c] -= consumed;
                        int whole = (int)floorf(consumed + 0.0001f);
                        if (whole > 0) {
                            manifest_consume_by_commodity(&dest->manifest,
                                                          (commodity_t)c, whole);
                            dest->named_ingots_dirty = true;
                        }
                    }
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
            /* Dock auto-repair: consume kits to restore the hauler's
             * hull. Closes the kit demand loop in single-player —
             * without this, only the human pilot's repairs ever
             * consumed kits and production hit cap immediately. If the
             * home dock is dry on kits we leave the hauler damaged;
             * next dock cycle tries again. */
            float max_h = npc_max_hull(npc);
            ship_t *ship = npc_ship_for(w, n);
            float cur_hull = ship ? ship->hull : npc->hull;
            if (cur_hull < max_h - 0.5f
                && station_has_module(home, MODULE_DOCK)) {
                int kits = (int)floorf(home->inventory[COMMODITY_REPAIR_KIT] + 0.0001f);
                int missing = (int)ceilf(max_h - cur_hull);
                int apply = kits < missing ? kits : missing;
                if (apply > 0) {
                    home->inventory[COMMODITY_REPAIR_KIT] -= (float)apply;
                    if (home->inventory[COMMODITY_REPAIR_KIT] < 0.0f)
                        home->inventory[COMMODITY_REPAIR_KIT] = 0.0f;
                    if (manifest_consume_by_commodity(&home->manifest,
                                                     COMMODITY_REPAIR_KIT, apply) > 0)
                        home->named_ingots_dirty = true;
                    /* Write through ship layer; reverse-mirror at end of
                     * the NPC tick pushes the value back to npc->hull. */
                    if (ship) {
                        ship->hull += (float)apply;
                        if (ship->hull > max_h) ship->hull = max_h;
                    } else {
                        npc->hull += (float)apply;
                        if (npc->hull > max_h) npc->hull = max_h;
                    }
                }
            }
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
static int find_destination_for_scaffold(const world_t *w, module_type_t type,
                                        int exclude_station) {
    /* Pass 1: outposts (active OR planned) with a placement plan for
     * this type — those are slots the player explicitly reserved. A
     * planned outpost is a valid destination too: when the scaffold
     * arrives the planned ghost can be promoted via the existing
     * snap-to-slot logic, with the relay as its founding module. */
    for (int s = 3; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            if (st->placement_plans[p].type == type) return s;
        }
    }
    /* Pass 2: any active outpost with at least one open ring slot. */
    for (int s = 3; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
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
            if (s == exclude_station) continue;
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
        /* Must have a place to deliver (not back to home station) */
        if (find_destination_for_scaffold(w, sc->module_type, npc->home_station) < 0) continue;
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
            int dest = find_destination_for_scaffold(w, sc->module_type, npc->home_station);
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
        /* Despawn-on-destroy: the spawn loop replaces dead slots on
         * the next tick. Cargo is lost (the chain takes a hit when a
         * loaded hauler dies — that's the cost of letting them get
         * smashed by asteroids). */
        if (npc->hull <= 0.0f) {
            SIM_LOG("[npc] %d (role=%d) destroyed — hull 0\n", n, (int)npc->role);
            npc->active = false;
            character_free_for_npc(w, n);
            continue;
        }
        npc->thrusting = false;
        mirror_npc_to_character(w, n);
        npc_validate_stations(w, npc);

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            mirror_ship_to_npc(w, n);
            continue;
        }
        if (npc->role == NPC_ROLE_TOW) {
            step_tow_drone(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            mirror_ship_to_npc(w, n);
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

            /* Hover near the rock via flight controller. */
            {
                ship_t view = ship_view_from_npc(npc);
                flight_cmd_t cmd = flight_hover_near(w, &view, a->pos, standoff);
                if (cmd.thrust < 0.0f) {
                    /* Hover-specific brake: push away from the asteroid we're
                     * hugging instead of reversing along velocity. Strip the
                     * negative thrust before the shared apply so the helper
                     * skips the forward-thrust branch. */
                    vec2 away = v2_norm(v2_sub(npc->pos, a->pos));
                    npc->vel = v2_add(npc->vel, v2_scale(away, hull->accel * 0.5f * dt));
                    cmd.thrust = 0.0f;
                }
                npc_apply_flight_cmd(npc, cmd, hull->accel, hull->turn_speed, dt);
                /* Hover never lights the engine flame — keep prior visual. */
                npc->thrusting = false;
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

        /* Re-mirror after the dispatch wrote npc->target_asteroid /
         * state / etc., so the next miner processed in the same tick
         * sees fresh target contention via characters[]. */
        mirror_npc_to_character(w, n);

        /* NPC collision with stations and asteroids */
        if (npc->state != NPC_STATE_DOCKED) {
            npc_resolve_station_collisions(w, npc);
            npc_resolve_asteroid_collisions(w, npc);
        }
        /* Reverse-mirror ship -> npc after damage was applied through
         * the ship layer (#294 Slice 9). Keeps npc->hull authoritative
         * for the despawn check at the top of the next tick. */
        mirror_ship_to_npc(w, n);

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
