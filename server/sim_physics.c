/*
 * sim_physics.c -- Asteroid physics: N-body gravity, asteroid-asteroid
 * collision, and asteroid-station collision.  Extracted from game_sim.c.
 */
#include "sim_physics.h"
#include <math.h>
#include <string.h>

static uint16_t asteroid_physics_cell_count(const spatial_cell_t *cell) {
    if (!cell) return 0;
    return cell->count < ASTEROID_PHYSICS_CELL_BUDGET
        ? cell->count
        : (uint16_t)ASTEROID_PHYSICS_CELL_BUDGET;
}

static bool token_has_any_bit(const uint8_t token[8]) {
    return (token[0] | token[1] | token[2] | token[3] |
            token[4] | token[5] | token[6] | token[7]) != 0;
}

void sim_body_integrate(sim_body_t body, float dt,
                        float velocity_multiplier) {
    if (!body.pos || !body.vel || !isfinite(dt) || dt <= 0.0f) return;
    if (!isfinite(velocity_multiplier) || velocity_multiplier < 0.0f)
        velocity_multiplier = 0.0f;
    *body.pos = v2_add(*body.pos, v2_scale(*body.vel, dt));
    *body.vel = v2_scale(*body.vel, velocity_multiplier);
}

void sim_body_advance(sim_body_t body, float dt) {
    if (!isfinite(dt) || dt <= 0.0f) return;
    if ((body.flags & SIM_BODY_FLAG_SPIN) && body.rotation && body.spin)
        *body.rotation += *body.spin * dt;
    if ((body.flags & SIM_BODY_FLAG_AGE) && body.age)
        *body.age += dt;
    if ((body.flags & SIM_BODY_FLAG_DYNAMIC) &&
        !(body.flags & SIM_BODY_FLAG_KINEMATIC)) {
        sim_body_integrate(body, dt,
            (body.flags & SIM_BODY_FLAG_DRAG)
                ? body.velocity_multiplier : 1.0f);
    }
}

sim_body_t sim_body_from_asteroid(asteroid_t *asteroid) {
    if (!asteroid) return (sim_body_t){0};
    return (sim_body_t){
        .pos = &asteroid->pos,
        .vel = &asteroid->vel,
        .rotation = &asteroid->rotation,
        .spin = &asteroid->spin,
        .age = &asteroid->age,
        .radius = asteroid->radius,
        .velocity_multiplier = 1.0f,
        .flags = SIM_BODY_FLAG_DYNAMIC | SIM_BODY_FLAG_SPIN |
                 SIM_BODY_FLAG_AGE,
        .collision_mask = SIM_BODY_COLLIDE_SHIP |
                          SIM_BODY_COLLIDE_STATION |
                          SIM_BODY_COLLIDE_ASTEROID |
                          SIM_BODY_COLLIDE_CARGO,
    };
}

sim_body_t sim_body_from_cargo_pod(cargo_pod_t *pod) {
    if (!pod) return (sim_body_t){0};
    return (sim_body_t){
        .pos = &pod->pos,
        .vel = &pod->vel,
        .rotation = &pod->rotation,
        .spin = &pod->spin,
        .age = &pod->age,
        .radius = pod->radius,
        .velocity_multiplier = 1.0f,
        .flags = SIM_BODY_FLAG_DYNAMIC | SIM_BODY_FLAG_SPIN |
                 SIM_BODY_FLAG_AGE,
        .collision_mask = SIM_BODY_COLLIDE_SHIP |
                          SIM_BODY_COLLIDE_STATION |
                          SIM_BODY_COLLIDE_ASTEROID |
                          SIM_BODY_COLLIDE_CARGO,
    };
}

sim_body_t sim_body_from_scaffold(scaffold_t *scaffold) {
    if (!scaffold) return (sim_body_t){0};
    sim_body_t body = {
        .pos = &scaffold->pos,
        .vel = &scaffold->vel,
        .rotation = &scaffold->rotation,
        .spin = &scaffold->spin,
        .age = &scaffold->age,
        .radius = scaffold->radius,
        .velocity_multiplier = 1.0f,
        .flags = SIM_BODY_FLAG_SPIN | SIM_BODY_FLAG_AGE,
        .collision_mask = SIM_BODY_COLLIDE_SHIP |
                          SIM_BODY_COLLIDE_STATION |
                          SIM_BODY_COLLIDE_ASTEROID,
    };
    if (scaffold->state == SCAFFOLD_LOOSE ||
        scaffold->state == SCAFFOLD_SNAPPING) {
        body.flags |= SIM_BODY_FLAG_DYNAMIC;
    } else {
        body.flags |= SIM_BODY_FLAG_KINEMATIC;
    }
    if (scaffold_has_tractor(scaffold)) body.flags |= SIM_BODY_FLAG_TOWED;
    return body;
}

void sim_world_integrate_bodies(world_t *w, sim_body_phase_t phase,
                                float dt) {
    if (!w || !isfinite(dt) || dt <= 0.0f) return;
    if (phase == SIM_BODY_PHASE_ASTEROIDS) {
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            asteroid_t *asteroid = &w->asteroids[i];
            if (!asteroid->active) continue;
            sim_body_t body = sim_body_from_asteroid(asteroid);
            entity_ref_t target = world_entity_ref_for_slot(
                w, ENTITY_KIND_ASTEROID, i, -1);
            if (world_tow_link_for_target_const(w, target))
                body.flags |= SIM_BODY_FLAG_TOWED;
            else {
                body.flags |= SIM_BODY_FLAG_DRAG;
                body.velocity_multiplier = 1.0f / (1.0f + 0.42f * dt);
            }
            sim_body_advance(body, dt);
        }
        return;
    }
    if (phase == SIM_BODY_PHASE_CARGO_PODS) {
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            cargo_pod_t *pod = &w->cargo_pods[i];
            if (!pod->active) continue;
            sim_body_t body = sim_body_from_cargo_pod(pod);
            body.flags |= SIM_BODY_FLAG_DRAG;
            body.velocity_multiplier = 1.0f / (1.0f + 0.35f * dt);
            if (cargo_pod_has_player_tractor(pod) ||
                cargo_pod_has_module_tractor(pod))
                body.flags |= SIM_BODY_FLAG_TOWED;
            sim_body_advance(body, dt);
        }
        return;
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        scaffold_t *scaffold = &w->scaffolds[i];
        if (!scaffold->active) continue;
        sim_body_t body = sim_body_from_scaffold(scaffold);
        if (phase == SIM_BODY_PHASE_SCAFFOLD_AMBIENT) {
            /* Spin and age every live scaffold; only loose bodies drift. */
            if (scaffold->state != SCAFFOLD_LOOSE)
                body.flags &= (uint16_t)~SIM_BODY_FLAG_DYNAMIC;
            else {
                body.flags |= SIM_BODY_FLAG_DRAG;
                body.velocity_multiplier = 0.98f;
            }
        } else if (phase == SIM_BODY_PHASE_SCAFFOLD_SNAPPING) {
            if (scaffold->state != SCAFFOLD_SNAPPING) continue;
            body.flags &= (uint16_t)~(SIM_BODY_FLAG_SPIN | SIM_BODY_FLAG_AGE);
        } else {
            continue;
        }
        sim_body_advance(body, dt);
    }
}

sim_body_contact_t sim_body_resolve_static_circle(
    sim_body_t body, vec2 center, float obstacle_radius,
    vec2 obstacle_vel, float bounce_scale, float skin) {
    sim_body_contact_t contact = {0};
    if (!body.pos || !body.vel || body.radius < 0.0f ||
        obstacle_radius < 0.0f) {
        return contact;
    }
    float min_dist = body.radius + obstacle_radius;
    vec2 delta = v2_sub(*body.pos, center);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq >= min_dist * min_dist) return contact;

    float dist = v2_len(delta);
    if (dist < 0.001f) {
        dist = 0.001f;
        delta = v2(1.0f, 0.0f);
    }
    contact.collided = true;
    contact.normal = v2_scale(delta, 1.0f / dist);
    vec2 relative_vel = v2_sub(*body.vel, obstacle_vel);
    contact.closing_speed = -v2_dot(relative_vel, contact.normal);

    float overlap = min_dist - dist;
    *body.pos = v2_add(*body.pos,
                       v2_scale(contact.normal, overlap + skin));
    float vel_along = v2_dot(relative_vel, contact.normal);
    if (vel_along < 0.0f) {
        *body.vel = v2_sub(*body.vel,
                           v2_scale(contact.normal,
                                    vel_along * bounce_scale));
    }
    return contact;
}

void asteroid_clear_thrown(asteroid_t *a) {
    if (!a) return;
    bool changed = a->thrown_timer_q > 0 || token_has_any_bit(a->thrown_by_token);
    memset(a->thrown_by_token, 0, sizeof(a->thrown_by_token));
    a->thrown_timer_q = 0;
    if (changed) a->net_dirty = true;
}

void asteroid_mark_thrown(asteroid_t *a, const uint8_t token[8], float seconds) {
    if (!a) return;
    if (!token || !token_has_any_bit(token) || seconds <= 0.0f || !isfinite(seconds)) {
        asteroid_clear_thrown(a);
        return;
    }
    unsigned q = (unsigned)ceilf(seconds * 10.0f);
    if (q == 0) q = 1;
    if (q > 255u) q = 255u;
    memcpy(a->thrown_by_token, token, sizeof(a->thrown_by_token));
    a->thrown_timer_q = (uint8_t)q;
    a->net_dirty = true;
}

bool asteroid_is_ballistic(const asteroid_t *a) {
    return a && a->thrown_timer_q > 0 && token_has_any_bit(a->thrown_by_token);
}

void asteroid_step_thrown_state(world_t *w) {
    if (!w || w->tick == 0 || (w->tick % ASTEROID_THROW_TIMER_TICKS) != 0) return;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->thrown_timer_q == 0) continue;
        a->thrown_timer_q--;
        if (a->thrown_timer_q == 0)
            memset(a->thrown_by_token, 0, sizeof(a->thrown_by_token));
        a->net_dirty = true;
    }
}

typedef struct {
    vec2 pos;
    float radius;
    int intake_modules;
} asteroid_pull_station_t;

static int collect_asteroid_pull_stations(const world_t *w,
                                          asteroid_pull_station_t out[MAX_STATIONS]) {
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (st->scaffold) continue;
        int intake_modules = 0;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_type_t mt = st->modules[m].type;
            if (mt == MODULE_HOPPER || mt == MODULE_FURNACE)
                intake_modules++;
        }
        if (intake_modules == 0) continue;
        out[count++] = (asteroid_pull_station_t) {
            .pos = st->pos,
            .radius = st->radius,
            .intake_modules = intake_modules,
        };
    }
    return count;
}

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
                uint16_t candidate_count =
                    asteroid_physics_cell_count(cell);
                for (uint16_t ci = 0; ci < candidate_count; ci++) {
                    int j = cell->indices[ci];
                    if (j <= i) continue; /* avoid double-processing */
                    asteroid_t *b = &w->asteroids[j];
                    if (!b->active || b->tier == ASTEROID_TIER_S) continue;
                    vec2 delta = v2_sub(b->pos, a->pos);
                    float dist_sq = v2_len_sq(delta);
                    if (dist_sq > 800.0f * 800.0f || dist_sq < 1.0f) continue;
                    float dist = v2_len(delta);
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
    asteroid_pull_station_t pull_stations[MAX_STATIONS];
    int pull_station_count = collect_asteroid_pull_stations(w, pull_stations);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < pull_station_count; s++) {
            const asteroid_pull_station_t *st = &pull_stations[s];
            int intake_modules = st->intake_modules;
            vec2 delta = v2_sub(st->pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            float pull_range = 600.0f + (float)intake_modules * 100.0f;
            if (dist_sq > pull_range * pull_range || dist_sq < 1.0f) continue;
            float dist = v2_len(delta);
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

    /* Signal pressure: strong relay coverage pushes isolated field rocks
     * down the signal gradient. That makes mined-out/high-signal cores
     * shed terrain toward the frontier/fringe instead of pulling the
     * frontier empty. */

    /* Prebuild connected player positions once — avoids scanning all
     * MAX_PLAYERS (32) for every asteroid in the weak-signal loop. */
    vec2 player_positions[MAX_PLAYERS];
    int player_count = 0;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (w->players[p].connected)
            player_positions[player_count++] = w->players[p].ship->pos;
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
                    uint16_t candidate_count =
                        asteroid_physics_cell_count(cell);
                    for (uint16_t ci = 0; ci < candidate_count; ci++) {
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

        float sig_here = signal_strength_at(w, a->pos);
        if (sig_here <= SIGNAL_BAND_FRONTIER) continue;

        const float step = 300.0f;
        float sxp = signal_strength_at(w, v2_add(a->pos, v2(step, 0.0f)));
        float sxm = signal_strength_at(w, v2_add(a->pos, v2(-step, 0.0f)));
        float syp = signal_strength_at(w, v2_add(a->pos, v2(0.0f, step)));
        float sym = signal_strength_at(w, v2_add(a->pos, v2(0.0f, -step)));
        vec2 uphill = v2(sxp - sxm, syp - sym);
        float glen_sq = v2_len_sq(uphill);
        if (glen_sq < 0.000001f) continue;

        vec2 downhill = v2_scale(uphill, -1.0f / v2_len(uphill));
        float pressure = (sig_here - SIGNAL_BAND_FRONTIER) /
                         (SIGNAL_BAND_OPERATIONAL - SIGNAL_BAND_FRONTIER);
        if (pressure > 1.0f) pressure = 1.0f;
        a->vel = v2_add(a->vel, v2_scale(downhill, 3.0f * pressure * dt));
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
                uint16_t candidate_count =
                    asteroid_physics_cell_count(cell);
                for (uint16_t ci = 0; ci < candidate_count; ci++) {
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
                    float dist = v2_len(delta);
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
    sim_body_contact_t contact = sim_body_resolve_static_circle(
        (sim_body_t){ .pos = &a->pos, .vel = &a->vel, .radius = a->radius },
        mod_pos, mod_radius, v2(0.0f, 0.0f), 1.0f, 1.0f);
    if (contact.collided) a->net_dirty = true;
}

static void resolve_asteroid_corridor_collision(asteroid_t *a, vec2 center,
                                                float ring_r, float angle_a,
                                                float arc_delta) {
    vec2 delta = v2_sub(a->pos, center);
    float dist = v2_len(delta);
    if (dist < 1.0f) return;

    float r_inner = ring_r - STATION_CORRIDOR_HW - a->radius;
    float r_outer = ring_r + STATION_CORRIDOR_HW + a->radius;
    if (dist <= r_inner || dist >= r_outer) return;

    float ast_angle = fixp_atan2f(delta.y, delta.x);
    float angular_margin = fixp_asinf(fminf(a->radius / dist, 1.0f));
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
    float dist = v2_len(delta);
    if (fabsf(dist - cor->ring_radius) >=
        STATION_CORRIDOR_HW + a->radius + STATION_MODULE_COL_RADIUS)
        return false;

    float ast_ang = fixp_atan2f(delta.y, delta.x);
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

static bool asteroid_near_station_collision_envelope(const asteroid_t *a,
                                                     const station_t *st) {
    float reach = station_collision_envelope_radius(st) + a->radius;
    return v2_dist_sq(a->pos, st->pos) <= reach * reach;
}

void resolve_asteroid_station_collisions(world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;

        bool any_near = false;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            asteroid_t *a = &w->asteroids[i];
            if (!a->active) continue;
            if (asteroid_near_station_collision_envelope(a, st)) {
                any_near = true;
                break;
            }
        }
        if (!any_near) continue;

        /* A full MAX_STATIONS geometry cache is ~320 KiB with the expanded
         * station cap, which overflows the browser WASM stack. Keep only one
         * station's geometry live while walking all asteroids. */
        station_geom_t geom;
        station_build_geom(st, &geom);

        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            asteroid_t *a = &w->asteroids[i];
            if (!a->active) continue;
            if (!asteroid_near_station_collision_envelope(a, st)) continue;
            /* Core collision */
            if (geom.has_core)
                resolve_asteroid_module_collision(a, geom.core.center, geom.core.radius);
            /* Module and dock collision circles. */
            for (int ci = 0; ci < geom.circle_count; ci++)
                resolve_asteroid_module_collision(a, geom.circles[ci].center, geom.circles[ci].radius);
            /* Corridor arcs form the visible station wall bands between modules. */
            for (int ci = 0; ci < geom.corridor_count; ci++) {
                const geom_corridor_t *cor = &geom.corridors[ci];
                if (asteroid_near_corridor_module(a, &geom, cor)) continue;
                resolve_asteroid_corridor_collision(a, geom.center,
                    cor->ring_radius, cor->angle_a, cor->arc_delta);
            }
        }
    }
}
