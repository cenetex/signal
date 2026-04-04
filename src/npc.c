#include <math.h>
#include "npc.h"

float npc_total_cargo(const npc_ship_t* npc) {
    float total = 0.0f;
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        total += npc->cargo[i];
    }
    return total;
}

bool npc_target_valid(const npc_ship_t* npc, const asteroid_t* asteroids, int count) {
    if (npc->target_asteroid < 0 || npc->target_asteroid >= count) return false;
    const asteroid_t* a = &asteroids[npc->target_asteroid];
    return a->active && a->tier != ASTEROID_TIER_S;
}

int npc_find_mineable_asteroid(const npc_ship_t* npc, const asteroid_t* asteroids, int count) {
    int best = -1;
    float best_dist_sq = 1e18f;
    for (int i = 0; i < count; i++) {
        const asteroid_t* a = &asteroids[i];
        if (!a->active) continue;
        if (a->tier == ASTEROID_TIER_S) continue;
        float d = v2_dist_sq(npc->pos, a->pos);
        if (d < best_dist_sq) {
            best_dist_sq = d;
            best = i;
        }
    }
    return best;
}

void npc_steer_toward(npc_ship_t* npc, vec2 target, float accel, float turn_speed, float dt) {
    vec2 delta = v2_sub(target, npc->pos);
    float desired_angle = atan2f(delta.y, delta.x);
    float diff = wrap_angle(desired_angle - npc->angle);
    float max_turn = turn_speed * dt;
    if (diff > max_turn) diff = max_turn;
    else if (diff < -max_turn) diff = -max_turn;
    npc->angle = wrap_angle(npc->angle + diff);

    vec2 forward = v2_from_angle(npc->angle);
    npc->vel = v2_add(npc->vel, v2_scale(forward, accel * dt));
    npc->thrusting = true;
}

void npc_apply_physics(npc_ship_t* npc, float drag, float dt) {
    npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (drag * dt)));
    npc->pos = v2_add(npc->pos, v2_scale(npc->vel, dt));
}
