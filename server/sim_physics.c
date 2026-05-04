/*
 * sim_physics.c -- Asteroid physics: N-body gravity, asteroid-asteroid
 * collision, and asteroid-station collision.  Extracted from game_sim.c.
 */
#include "sim_physics.h"

/* ================================================================== */
/* Asteroid-asteroid gravity                                          */
/* ================================================================== */

void step_asteroid_gravity(world_t *w, float dt) {
    /* Build spatial grid for neighbor lookups */
    spatial_grid_build(w);
    const spatial_grid_t *g = &w->asteroid_grid;

    /* Asteroid-asteroid attraction (non-S tier, within 800 units) via spatial grid */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        int cx, cy;
        spatial_grid_cell(g, a->pos, &cx, &cy);
        for (int gy = cy - 1; gy <= cy + 1; gy++) {
            for (int gx = cx - 1; gx <= cx + 1; gx++) {
                const spatial_cell_t *cell = spatial_grid_lookup(g, gx, gy);
                if (!cell) continue;
                for (int ci = 0; ci < cell->count; ci++) {
                    int j = cell->indices[ci];
                    if (j <= i) continue; /* avoid double-processing */
                    asteroid_t *b = &w->asteroids[j];
                    if (!b->active || b->tier == ASTEROID_TIER_S) continue;
                    vec2 delta = v2_sub(b->pos, a->pos);
                    float dist_sq = v2_len_sq(delta);
                    if (dist_sq > 800.0f * 800.0f || dist_sq < 1.0f) continue;
                    float dist = sqrtf(dist_sq);
                    /* Don't attract asteroids at or inside collision boundary */
                    float min_dist = a->radius + b->radius;
                    if (dist < min_dist * 1.3f) continue; /* dead zone: 30% beyond contact */
                    vec2 normal = v2_scale(delta, 1.0f / dist);
                    float mass_a = a->radius * a->radius;
                    float mass_b = b->radius * b->radius;
                    /* Gravitational force proportional to both masses.
                     * Clamp against the lighter body so swapping slots cannot
                     * change the result while preserving equal/opposite force. */
                    float force_mag = (mass_a * mass_b) / dist_sq * 14.0f;
                    float max_force = 60.0f * fminf(mass_a, mass_b);
                    if (force_mag > max_force) force_mag = max_force;
                    /* F = ma, so acceleration = force / mass */
                    vec2 accel_a = v2_scale(normal, (force_mag / mass_a) * dt);
                    vec2 accel_b = v2_scale(normal, -(force_mag / mass_b) * dt);
                    a->vel = v2_add(a->vel, accel_a);
                    b->vel = v2_add(b->vel, accel_b);
                }
            }
        }
    }

    /* Industrial pull: only stations with active intake/processing modules
     * generate asteroid attraction. Pull scales with industrial activity
     * and inversely with asteroid size (fragments pulled strongly). */
    /* Precompute per-station intake module count */
    int station_intake[MAX_STATIONS];
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_intake[s] = 0;
        const station_t *st = &w->stations[s];
        if (st->scaffold) continue;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_type_t mt = st->modules[m].type;
            if (mt == MODULE_HOPPER || mt == MODULE_FURNACE)
                station_intake[s]++;
        }
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            int intake_modules = station_intake[s];
            if (intake_modules == 0) continue; /* relay-only or scaffold stations: no pull */
            const station_t *st = &w->stations[s];
            vec2 delta = v2_sub(st->pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            float pull_range = 600.0f + (float)intake_modules * 100.0f;
            if (dist_sq > pull_range * pull_range || dist_sq < 1.0f) continue;
            float dist = sqrtf(dist_sq);
            float min_dist = a->radius + st->radius;
            if (dist < min_dist + 10.0f) continue;
            vec2 normal = v2_scale(delta, 1.0f / dist);
            /* Tier-dependent: smaller = more pulled. radius^2 inversely scales force */
            float mass_a = a->radius * a->radius;
            float base_force = (float)intake_modules * 2.5f;
            float force = base_force * st->radius / (dist * 0.8f);
            /* TIER_S fragments get extra pull for hopper feeding */
            if (a->tier == ASTEROID_TIER_S) force *= 3.0f;
            float accel = force / mass_a;
            a->vel = v2_add(a->vel, v2_scale(normal, accel * dt));
        }
    }

    /* Weak-signal current keeps isolated field rocks drifting inward. */

    /* Prebuild connected player positions once — avoids scanning all
     * MAX_PLAYERS (32) for every asteroid in the weak-signal loop. */
    vec2 player_positions[MAX_PLAYERS];
    int player_count = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (w->players[p].connected)
            player_positions[player_count++] = w->players[p].ship.pos;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;

        bool near_player = false;
        for (int p = 0; p < player_count; p++) {
            if (v2_dist_sq(a->pos, player_positions[p]) <= 600.0f * 600.0f) {
                near_player = true;
                break;
            }
        }
        if (near_player) continue;

        bool near_asteroid = false;
        {
            int acx, acy;
            spatial_grid_cell(g, a->pos, &acx, &acy);
            for (int gy = acy - 1; gy <= acy + 1 && !near_asteroid; gy++) {
                for (int gx = acx - 1; gx <= acx + 1 && !near_asteroid; gx++) {
                    const spatial_cell_t *cell = spatial_grid_lookup(g, gx, gy);
                    if (!cell) continue;
                    for (int ci = 0; ci < cell->count; ci++) {
                        int j = cell->indices[ci];
                        if (j == i || !w->asteroids[j].active) continue;
                        if (v2_dist_sq(a->pos, w->asteroids[j].pos) <= 400.0f * 400.0f) {
                            near_asteroid = true;
                            break;
                        }
                    }
                }
            }
        }
        if (near_asteroid) continue;

        float best_signal = 0.0f;
        int best_station = -1;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_provides_signal(&w->stations[s])) continue;
            float dist = sqrtf(v2_dist_sq(a->pos, w->stations[s].pos));
            float strength = fmaxf(0.0f, 1.0f - (dist / w->stations[s].signal_range));
            if (strength > best_signal) {
                best_signal = strength;
                best_station = s;
            }
        }
        if (best_station < 0 || best_signal <= 0.0f || best_signal >= 0.75f) continue;

        vec2 delta = v2_sub(w->stations[best_station].pos, a->pos);
        float dist_sq = v2_len_sq(delta);
        if (dist_sq < 1.0f) continue;
        float dist = sqrtf(dist_sq);
        float min_dist = a->radius + w->stations[best_station].radius;
        if (dist < min_dist + 10.0f) continue;

        vec2 normal = v2_scale(delta, 1.0f / dist);
        float current = (0.75f - best_signal) / 0.75f;
        a->vel = v2_add(a->vel, v2_scale(normal, 3.0f * current * dt));
    }
}

/* ================================================================== */
/* Asteroid-asteroid collision                                        */
/* ================================================================== */

void resolve_asteroid_collisions(world_t *w) {
    const spatial_grid_t *g = &w->asteroid_grid;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        int cx, cy;
        spatial_grid_cell(g, a->pos, &cx, &cy);
        for (int gy = cy - 1; gy <= cy + 1; gy++) {
            for (int gx = cx - 1; gx <= cx + 1; gx++) {
                const spatial_cell_t *cell = spatial_grid_lookup(g, gx, gy);
                if (!cell) continue;
                for (int ci = 0; ci < cell->count; ci++) {
                    int j = cell->indices[ci];
                    if (j <= i) continue; /* avoid double-processing */
                    asteroid_t *b = &w->asteroids[j];
                    if (!b->active) continue;
                    /* Skip if both are S tier */
                    if (a->tier == ASTEROID_TIER_S && b->tier == ASTEROID_TIER_S) continue;
                    float min_dist = a->radius + b->radius;
                    vec2 delta = v2_sub(a->pos, b->pos);
                    float dist_sq = v2_len_sq(delta);
                    if (dist_sq >= min_dist * min_dist) continue;
                    float dist = sqrtf(dist_sq);
                    if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
                    vec2 normal = v2_scale(delta, 1.0f / dist);
                    float overlap = min_dist - dist;
                    /* Push apart: heavier (larger radius) moves less */
                    float mass_a = a->radius * a->radius;
                    float mass_b = b->radius * b->radius;
                    float total_mass = mass_a + mass_b;
                    float ratio_a = mass_b / total_mass; /* a moves proportional to b's mass */
                    float ratio_b = mass_a / total_mass;
                    a->pos = v2_add(a->pos, v2_scale(normal, overlap * ratio_a));
                    b->pos = v2_sub(b->pos, v2_scale(normal, overlap * ratio_b));
                    /* Transfer velocity along collision normal */
                    float rel_vel = v2_dot(v2_sub(a->vel, b->vel), normal);
                    if (rel_vel < 0.0f) {
                        vec2 impulse_a = v2_scale(normal, rel_vel * ratio_a);
                        vec2 impulse_b = v2_scale(normal, rel_vel * ratio_b);
                        a->vel = v2_sub(a->vel, impulse_a);
                        b->vel = v2_add(b->vel, impulse_b);
                    }
                }
            }
        }
    }
}

/* ================================================================== */
/* Asteroid-station collision                                         */
/* ================================================================== */

static void resolve_asteroid_module_collision(asteroid_t *a, vec2 mod_pos, float mod_radius) {
    float min_dist = a->radius + mod_radius;
    vec2 delta = v2_sub(a->pos, mod_pos);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq >= min_dist * min_dist) return;
    float dist = sqrtf(dist_sq);
    if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
    vec2 normal = v2_scale(delta, 1.0f / dist);
    float overlap = min_dist - dist;
    a->pos = v2_add(a->pos, v2_scale(normal, overlap + 1.0f));
    float vel_along = v2_dot(a->vel, normal);
    if (vel_along < 0.0f)
        a->vel = v2_sub(a->vel, v2_scale(normal, vel_along * 1.0f));
    a->net_dirty = true;
}

static void resolve_asteroid_corridor_collision(asteroid_t *a, vec2 center,
                                                float ring_r, float angle_a,
                                                float arc_delta) {
    vec2 delta = v2_sub(a->pos, center);
    float dist = sqrtf(v2_len_sq(delta));
    if (dist < 1.0f) return;

    float r_inner = ring_r - STATION_CORRIDOR_HW - a->radius;
    float r_outer = ring_r + STATION_CORRIDOR_HW + a->radius;
    if (dist <= r_inner || dist >= r_outer) return;

    float ast_angle = atan2f(delta.y, delta.x);
    float angular_margin = asinf(fminf(a->radius / dist, 1.0f));
    float expanded_start = angle_a - angular_margin;
    float expanded_delta = arc_delta + 2.0f * angular_margin;
    if (angle_in_arc(ast_angle, expanded_start, expanded_delta) < 0.0f) return;

    vec2 radial = v2_scale(delta, 1.0f / dist);
    vec2 push_normal;
    float d_inner = dist - (ring_r - STATION_CORRIDOR_HW);
    float d_outer = (ring_r + STATION_CORRIDOR_HW) - dist;
    if (d_inner < d_outer) {
        a->pos = v2_add(center, v2_scale(radial,
            ring_r - STATION_CORRIDOR_HW - a->radius - 1.0f));
        push_normal = v2_scale(radial, -1.0f);
    } else {
        a->pos = v2_add(center, v2_scale(radial,
            ring_r + STATION_CORRIDOR_HW + a->radius + 1.0f));
        push_normal = radial;
    }

    float vel_along = v2_dot(a->vel, push_normal);
    if (vel_along < 0.0f)
        a->vel = v2_sub(a->vel, v2_scale(push_normal, vel_along));
    a->net_dirty = true;
}

static bool asteroid_near_corridor_module(const asteroid_t *a,
                                          const station_geom_t *geom,
                                          const geom_corridor_t *cor) {
    vec2 delta = v2_sub(a->pos, geom->center);
    float dist = sqrtf(v2_len_sq(delta));
    if (fabsf(dist - cor->ring_radius) >=
        STATION_CORRIDOR_HW + a->radius + STATION_MODULE_COL_RADIUS)
        return false;

    float ast_ang = atan2f(delta.y, delta.x);
    for (int mi = 0; mi < geom->circle_count; mi++) {
        const geom_circle_t *circle = &geom->circles[mi];
        if (circle->ring != cor->ring) continue;
        float angular_size = (cor->ring_radius > 1.0f)
            ? (STATION_MODULE_COL_RADIUS + a->radius) / cor->ring_radius
            : 0.0f;
        if (fabsf(wrap_angle(ast_ang - circle->angle)) < angular_size)
            return true;
    }
    return false;
}

void resolve_asteroid_station_collisions(world_t *w) {
    /* Build geometry for all active stations once */
    station_geom_t geom_cache[MAX_STATIONS];
    bool geom_valid[MAX_STATIONS];
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_collides(&w->stations[s])) {
            station_build_geom(&w->stations[s], &geom_cache[s]);
            geom_valid[s] = true;
        } else {
            geom_valid[s] = false;
        }
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!geom_valid[s]) continue;
            const station_geom_t *geom = &geom_cache[s];
            /* Core collision */
            if (geom->has_core)
                resolve_asteroid_module_collision(a, geom->core.center, geom->core.radius);
            /* Module and dock collision circles. */
            for (int ci = 0; ci < geom->circle_count; ci++)
                resolve_asteroid_module_collision(a, geom->circles[ci].center, geom->circles[ci].radius);
            /* Corridor arcs form the visible station wall bands between modules. */
            for (int ci = 0; ci < geom->corridor_count; ci++) {
                const geom_corridor_t *cor = &geom->corridors[ci];
                if (asteroid_near_corridor_module(a, geom, cor)) continue;
                resolve_asteroid_corridor_collision(a, geom->center,
                    cor->ring_radius, cor->angle_a, cor->arc_delta);
            }
        }
    }
}
