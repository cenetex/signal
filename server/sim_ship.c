/*
 * sim_ship.c — Shared ship physics primitives. See sim_ship.h.
 *
 * Slice 2 of #294: extracted from game_sim.c without behavior change.
 * The implementations are byte-for-byte the same as the originals;
 * the `static` qualifier was the only thing keeping them invisible
 * to NPC code.
 */
#include "sim_ship.h"
#include "game_sim.h"  /* SHIP_BRAKE, MAX_STATIONS, station_t */
#include "tractor.h"
#include "types.h"

void step_ship_rotation(ship_t *s, float dt, float turn_input) {
    s->angle = wrap_angle(s->angle + (turn_input * ship_hull_def(s)->turn_speed * dt));
}

float ship_boost_thrust_mult(bool boost, float hold_t) {
    if (!boost) return 1.0f;
    const float steady = 1.6f;
    const float peak   = 2.0f;
    /* exp(-3t) ≈ 1.0 at t=0, 0.05 at t=1.0s — most of the kick in
     * the first ~500ms, tail blends into the steady burn. */
    float kick = fixp_expf(-3.0f * hold_t);
    return steady + (peak - steady) * kick;
}

void ship_apply_body_tow(ship_t *ship, const towable_body_t *body, float dt) {
    if (!ship || !body || !body->pos || !body->vel) return;
    tractor_beam_t tow_beam = tractor_tow_beam(
        0.0f, TRACTOR_TOW_BAND_REST_LENGTH);
    tractor_anchor_t src = {
        .pos      = ship->pos,
        .vel      = &ship->vel,
        .inv_mass = 1.0f / SHIP_TOW_BAND_SHIP_MASS,
    };
    tractor_anchor_t tgt = {
        .pos      = *body->pos,
        .vel      = body->vel,
        .inv_mass = body->inv_mass,
    };
    (void)tractor_apply(&src, &tgt, &tow_beam, dt);
}

void ship_apply_fragment_tow(ship_t *ship, asteroid_t *fragment, float dt) {
    if (!fragment) return;
    towable_body_t body = {
        .pos = &fragment->pos,
        .vel = &fragment->vel,
        .inv_mass = 1.0f,
    };
    ship_apply_body_tow(ship, &body, dt);
}

void step_ship_thrust(ship_t *s, float dt, float thrust_input,
                      vec2 forward, bool boost, float boost_hold,
                      bool reverse_allowed) {
    const hull_def_t *hull = ship_hull_def(s);
    float mult = ship_boost_thrust_mult(boost, boost_hold);
    if (thrust_input > 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, hull->accel * thrust_input * mult * dt));
    } else if (thrust_input < 0.0f) {
        if (reverse_allowed) {
            s->vel = v2_add(s->vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
            return;
        }

        float speed = v2_len(s->vel);
        if (speed <= 0.0001f) {
            s->vel = v2(0.0f, 0.0f);
            return;
        }

        float brake = SHIP_BRAKE * -thrust_input * dt;
        if (brake >= speed) {
            s->vel = v2(0.0f, 0.0f);
        } else {
            s->vel = v2_scale(s->vel, (speed - brake) / speed);
        }
    }
}

float resolve_ship_circle_pushback(ship_t *ship, vec2 center, float radius) {
    float minimum = radius + ship_hull_def(ship)->ship_radius;
    vec2 delta = v2_sub(ship->pos, center);
    float d_sq = v2_len_sq(delta);
    if (d_sq >= minimum * minimum) return 0.0f;
    float d = v2_len(delta);
    vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
    /* Push past the surface by SKIN so we're cleanly outside. */
    ship->pos = v2_add(center, v2_scale(normal, minimum + SHIP_COLLISION_SKIN));
    float vel_toward = v2_dot(ship->vel, normal);
    if (vel_toward >= 0.0f) return 0.0f;
    ship->vel = v2_sub(ship->vel, v2_scale(normal, vel_toward));
    return -vel_toward;
}

float resolve_ship_annular_pushback(ship_t *ship, vec2 center,
                                    float ring_r, float angle_a, float arc_delta) {
    float ship_r = ship_hull_def(ship)->ship_radius;
    vec2 delta = v2_sub(ship->pos, center);
    float dist = v2_len(delta);
    if (dist < 1.0f) return 0.0f;

    /* Radial test: is the ship within the corridor's inflated band? */
    float r_inner = ring_r - STATION_CORRIDOR_HW - ship_r;
    float r_outer = ring_r + STATION_CORRIDOR_HW + ship_r;
    if (dist <= r_inner || dist >= r_outer) return 0.0f;

    /* Angular test with margin so ships near the arc edge don't
     * clip through. */
    float ship_angle = fixp_atan2f(delta.y, delta.x);
    float angular_margin = (dist > 1.0f) ? fixp_asinf(fminf(ship_r / dist, 1.0f)) : 0.0f;
    float expanded_start = angle_a - angular_margin;
    float expanded_da = arc_delta + 2.0f * angular_margin;
    if (angle_in_arc(ship_angle, expanded_start, expanded_da) < 0.0f) return 0.0f;

    /* Push radially to nearest edge plus SHIP_COLLISION_SKIN so the
     * next frame's tangential slide doesn't immediately re-trigger
     * this same corridor. */
    vec2 radial = v2_scale(delta, 1.0f / dist);
    vec2 push_normal;
    float d_inner = dist - (ring_r - STATION_CORRIDOR_HW);
    float d_outer = (ring_r + STATION_CORRIDOR_HW) - dist;
    if (d_inner < d_outer) {
        ship->pos = v2_add(center, v2_scale(radial,
            ring_r - STATION_CORRIDOR_HW - ship_r - SHIP_COLLISION_SKIN));
        push_normal = v2_scale(radial, -1.0f);
    } else {
        ship->pos = v2_add(center, v2_scale(radial,
            ring_r + STATION_CORRIDOR_HW + ship_r + SHIP_COLLISION_SKIN));
        push_normal = radial;
    }

    /* Zero the inward velocity component. Tangential slip preserved
     * so the ship can still scrape along the wall. */
    float vel_toward = v2_dot(ship->vel, push_normal);
    if (vel_toward >= 0.0f) return 0.0f;
    ship->vel = v2_sub(ship->vel, v2_scale(push_normal, vel_toward * 1.0f));
    return -vel_toward;
}

float resolve_ship_asteroid_pushback(ship_t *ship, asteroid_t *a) {
    float minimum = a->radius + ship_hull_def(ship)->ship_radius;
    vec2 delta = v2_sub(ship->pos, a->pos);
    float d_sq = v2_len_sq(delta);
    if (d_sq >= minimum * minimum) return 0.0f;
    float d = v2_len(delta);
    vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
    ship->pos = v2_add(a->pos, v2_scale(normal, minimum + SHIP_COLLISION_SKIN));

    /* Closing velocity along the contact normal. Negative = rock + ship
     * coming together; positive = already separating. */
    vec2 rel_vel = v2_sub(ship->vel, a->vel);
    float vel_toward = v2_dot(rel_vel, normal);
    if (vel_toward >= 0.0f) return 0.0f;

    /* Mass-equal split: cheapest stable bounce model. The ship loses
     * half the inward component of rel_vel; the rock gains the other
     * half. Net momentum conserved along the normal. */
    vec2 impulse = v2_scale(normal, vel_toward * 0.5f);
    ship->vel = v2_sub(ship->vel, impulse);
    a->vel    = v2_add(a->vel, impulse);
    a->net_dirty = true;
    return -vel_toward;
}

void step_ship_motion(ship_t *s, float dt, const world_t *w, float cached_signal) {
    (void)w;
    (void)cached_signal;
    s->vel = v2_scale(s->vel, 1.0f / (1.0f + (ship_hull_def(s)->drag * dt)));
    s->pos = v2_add(s->pos, v2_scale(s->vel, dt));
}
