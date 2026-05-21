/*
 * sim_flight.c — Shared flight controller implementation.
 * See sim_flight.h for the public API.
 */
#include "sim_flight.h"
#include "signal_model.h"
#include <math.h>

/* ------------------------------------------------------------------ */
/* flight_face_heading                                                 */
/* ------------------------------------------------------------------ */

float flight_face_heading(const ship_t *ship, float desired_angle) {
    float diff = wrap_angle(desired_angle - ship->angle);
    float strength = fminf(fabsf(diff) * 3.0f, 1.0f);
    if (diff > 0.02f) return strength;
    if (diff < -0.02f) return -strength;
    return 0.0f;
}

static bool flight_heading_blocked(const world_t *w, const ship_t *ship,
                                   float clearance, float heading) {
    vec2 fwd = v2(cosf(heading), sinf(heading));
    float speed = v2_len(ship->vel);
    float lookahead = fmaxf(100.0f, fminf(speed * 1.5f, 500.0f));
    vec2 probe_end = v2_add(ship->pos, v2_scale(fwd, lookahead));
    return !nav_segment_clear(w, ship->pos, probe_end, clearance);
}

void flight_avoid_station_wall(const world_t *w, const ship_t *ship,
                               flight_cmd_t *cmd) {
    if (!w || !ship || !cmd) return;

    float clearance = ship_hull_def(ship)->ship_radius + 30.0f;
    if (!flight_heading_blocked(w, ship, clearance, ship->angle)) return;

    bool left_blocked = flight_heading_blocked(w, ship, clearance,
                                               ship->angle + 0.7f);
    bool right_blocked = flight_heading_blocked(w, ship, clearance,
                                                ship->angle - 0.7f);
    bool back_blocked = flight_heading_blocked(w, ship, clearance,
                                               wrap_angle(ship->angle + PI_F));

    const station_t *nearest = NULL;
    float best_d = 1e30f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        float d = v2_dist_sq(ship->pos, st->pos);
        if (d < best_d) {
            best_d = d;
            nearest = st;
        }
    }

    if (nearest) {
        vec2 away = v2_sub(ship->pos, nearest->pos);
        if (v2_len_sq(away) > 1.0f)
            cmd->turn = flight_face_heading(ship, atan2f(away.y, away.x));
    } else if (left_blocked != right_blocked) {
        cmd->turn = left_blocked ? -1.0f : 1.0f;
    } else {
        cmd->turn = cmd->turn >= 0.0f ? 1.0f : -1.0f;
    }

    float speed = v2_len(ship->vel);
    cmd->reverse_thrust = false;
    if (speed < 35.0f && !back_blocked) {
        cmd->thrust = -0.6f;
        cmd->reverse_thrust = true;
    } else if (speed > 20.0f) {
        cmd->thrust = -1.0f;
    } else if (cmd->thrust > 0.0f) {
        cmd->thrust = 0.0f;
    }
}

/* ------------------------------------------------------------------ */
/* flight_steer_to                                                     */
/* ------------------------------------------------------------------ */

flight_cmd_t flight_steer_to(const world_t *w, const ship_t *ship,
                              nav_path_t *path, vec2 target,
                              float standoff, float max_speed, float dt) {
    flight_cmd_t cmd = {0.0f, 0.0f, false};
    const hull_def_t *hull = ship_hull_def(ship);
    float clearance = hull->ship_radius + 30.0f;

    /* Keep the A* path fresh and advance waypoints. */
    nav_follow_path(w, path, ship->pos, target, clearance, dt);
    vec2 control_target = target;
    if (path->count > 0 && path->current < path->count)
        control_target = path->waypoints[path->current];
    nav_steer_t st = nav_steer_toward_waypoint(path, ship->pos, target, dt);

    /* Proportional turn toward the current waypoint heading. */
    float diff = wrap_angle(st.desired_heading - ship->angle);
    float turn_strength = fminf(fabsf(diff) * 3.0f, 1.0f);
    cmd.turn = (diff > 0.02f) ? turn_strength
             : (diff < -0.02f ? -turn_strength : 0.0f);
    float facing = cosf(diff);

    /* Velocity-controlled approach. Intermediate path legs must brake
     * against their active waypoint, not the final destination, or ships
     * carry too much speed into station lanes and orbit the dock. */
    float dist_to_control = sqrtf(v2_dist_sq(ship->pos, control_target));
    float control_standoff = st.at_intermediate ? 0.0f : standoff;
    float effective_dist = fmaxf(0.0f, dist_to_control - control_standoff);
    float target_speed = nav_approach_speed(effective_dist, max_speed);

    /* Slow down near intermediate waypoints to make clean turns. */
    if (st.wp_dist < 200.0f && st.at_intermediate) {
        float wp_speed = nav_approach_speed(st.wp_dist, 80.0f);
        if (wp_speed < target_speed) target_speed = wp_speed;
    }

    /* Project velocity onto the same leg used for speed control. */
    vec2 to_target_dir = (dist_to_control > 0.5f)
        ? v2_scale(v2_sub(control_target, ship->pos), 1.0f / dist_to_control)
        : v2(cosf(ship->angle), sinf(ship->angle));
    float approach_v = v2_dot(ship->vel, to_target_dir);
    float thrust_cmd = nav_speed_control(approach_v, target_speed);

    /* Don't thrust while facing away from the waypoint. */
    if (facing < 0.5f && thrust_cmd > 0.0f) thrust_cmd = 0.0f;

    /* Obstacle avoidance: check forward clearance in heading AND velocity
     * direction. When an obstacle is detected, both brake AND steer
     * laterally to deflect around it. */
    float fwd_clear = nav_forward_clearance(w, ship->pos, ship->vel,
                                             hull->ship_radius, ship->angle);
    float speed = v2_len(ship->vel);
    float vel_clear = fwd_clear;
    if (speed > 0.5f) {
        float vel_angle = atan2f(ship->vel.y, ship->vel.x);
        vel_clear = nav_forward_clearance(w, ship->pos, ship->vel,
                                          hull->ship_radius, vel_angle);
    }
    float worst_clear = fminf(fwd_clear, vel_clear);
    if (worst_clear < 1.0f) {
        if (worst_clear < 0.3f) {
            if (speed < 35.0f) {
                thrust_cmd = -0.6f;
                cmd.reverse_thrust = true;
            } else {
                thrust_cmd = -1.0f;
            }
        } else if (thrust_cmd > 0.0f) {
            thrust_cmd *= worst_clear;
        }

        /* Lateral deflection: check clearance 45 degrees to each side,
         * steer toward the clearer side. This prevents the ship from
         * braking into a standstill against a rock face. */
        float left_angle  = ship->angle + 0.7f;
        float right_angle = ship->angle - 0.7f;
        float left_clear  = nav_forward_clearance(w, ship->pos, ship->vel,
                                                   hull->ship_radius, left_angle);
        float right_clear = nav_forward_clearance(w, ship->pos, ship->vel,
                                                   hull->ship_radius, right_angle);
        float deflect = (1.0f - worst_clear) * 0.8f;
        if (left_clear > right_clear)
            cmd.turn = fminf(cmd.turn + deflect, 1.0f);
        else
            cmd.turn = fmaxf(cmd.turn - deflect, -1.0f);
    }

    cmd.thrust = thrust_cmd;
    return cmd;
}

/* ------------------------------------------------------------------ */
/* flight_hover_near                                                   */
/* ------------------------------------------------------------------ */

flight_cmd_t flight_hover_near(const world_t *w, const ship_t *ship,
                                vec2 target, float standoff) {
    (void)w;

    flight_cmd_t cmd = {0.0f, 0.0f, false};
    float dist = sqrtf(v2_dist_sq(ship->pos, target));
    float speed = sqrtf(v2_len_sq(ship->vel));
    float sweet_min = standoff - 15.0f;
    float sweet_max = standoff + 30.0f;

    /* Priority 1: if going too fast, brake regardless of zone.
     * This prevents the overshoot-rocket cycle where the ship
     * passes through the asteroid and accelerates away. */
    if (speed > 40.0f) {
        cmd = flight_brake(ship);
        return cmd;
    }

    /* Direction to target (for facing). */
    vec2 to_target = v2_sub(target, ship->pos);

    if (dist < sweet_min) {
        /* Too close — drift away gently. Don't thrust hard or we
         * overshoot and enter the rocket cycle. */
        vec2 away = v2_sub(ship->pos, target);
        float push_angle = atan2f(away.y, away.x);
        cmd.turn = flight_face_heading(ship, push_angle);
        float facing = cosf(wrap_angle(push_angle - ship->angle));
        cmd.thrust = (facing > 0.6f) ? 0.3f : 0.0f;
    } else if (dist > sweet_max) {
        /* Drifted out — close in slowly at max 40 u/s. */
        float face = atan2f(to_target.y, to_target.x);
        cmd.turn = flight_face_heading(ship, face);
        float facing = cosf(wrap_angle(face - ship->angle));
        float approach_v = v2_dot(ship->vel, v2_scale(to_target, 1.0f / dist));
        cmd.thrust = nav_speed_control(approach_v, 40.0f);
        if (facing < 0.5f) cmd.thrust = 0.0f;
    } else {
        /* Sweet spot — face target, hold position. */
        float face = atan2f(to_target.y, to_target.x);
        cmd.turn = flight_face_heading(ship, face);
        /* Gently oppose any residual drift. */
        if (speed > 10.0f) {
            float vel_angle = atan2f(ship->vel.y, ship->vel.x);
            float brake_heading = wrap_angle(vel_angle + PI_F);
            float brake_facing = cosf(wrap_angle(brake_heading - ship->angle));
            if (brake_facing > 0.3f)
                cmd.thrust = fminf(speed / 30.0f, 0.5f);
            /* else: turning to face, let it happen */
        }
    }
    return cmd;
}

/* ------------------------------------------------------------------ */
/* flight_brake                                                        */
/* ------------------------------------------------------------------ */

flight_cmd_t flight_brake(const ship_t *ship) {
    flight_cmd_t cmd = {0.0f, 0.0f, false};
    float speed = sqrtf(v2_len_sq(ship->vel));
    if (speed < 5.0f) return cmd;

    /* Face opposite to velocity and thrust forward. */
    float vel_angle = atan2f(ship->vel.y, ship->vel.x);
    float brake_angle = wrap_angle(vel_angle + PI_F);
    cmd.turn = flight_face_heading(ship, brake_angle);
    float diff = wrap_angle(brake_angle - ship->angle);
    if (cosf(diff) > 0.3f) cmd.thrust = 1.0f;
    return cmd;
}
