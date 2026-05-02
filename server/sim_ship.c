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
    float kick = expf(-3.0f * hold_t);
    return steady + (peak - steady) * kick;
}

void step_ship_thrust(ship_t *s, float dt, float thrust_input,
                      vec2 forward, bool boost, float boost_hold) {
    const hull_def_t *hull = ship_hull_def(s);
    float mult = ship_boost_thrust_mult(boost, boost_hold);
    if (thrust_input > 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, hull->accel * thrust_input * mult * dt));
    } else if (thrust_input < 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
    }
}

void step_ship_motion(ship_t *s, float dt, const world_t *w, float cached_signal) {
    s->vel = v2_scale(s->vel, 1.0f / (1.0f + (ship_hull_def(s)->drag * dt)));
    s->pos = v2_add(s->pos, v2_scale(s->vel, dt));

    /* Signal-based boundary: push back when in frontier zone. */
    float boundary = signal_boundary_push(cached_signal);
    if (boundary > 0.0f) {
        float best_d_sq = 1e18f;
        int best_s = 0;
        for (int i = 0; i < MAX_STATIONS; i++) {
            float d_sq = v2_dist_sq(s->pos, w->stations[i].pos);
            if (d_sq < best_d_sq) { best_d_sq = d_sq; best_s = i; }
        }
        vec2 to_station = v2_sub(w->stations[best_s].pos, s->pos);
        float d = sqrtf(v2_len_sq(to_station));
        if (d > 0.001f) {
            /* Strong pull — at zero signal this is the only way back.
             * Scales with boundary (0 at frontier, 1 at zero signal). */
            float push_strength = boundary * 2.0f;
            vec2 push = v2_scale(to_station, push_strength / d);
            s->vel = v2_add(s->vel, push);
        }
    }
}
