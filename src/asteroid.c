#include <math.h>
#include <string.h>
#include "asteroid.h"
#include "commodity.h"

static const float WORLD_RADIUS = 5000.0f;  /* safety net; gameplay bounded by station signal_range */
static const float FRACTURE_CHILD_CLEANUP_AGE = 22.0f;
static const float FRACTURE_CHILD_CLEANUP_DISTANCE = 940.0f;

asteroid_tier_t asteroid_next_tier(asteroid_tier_t tier) {
    if (tier >= ASTEROID_TIER_S) return ASTEROID_TIER_S;
    return (asteroid_tier_t)(tier + 1);
}

bool asteroid_is_collectible(const asteroid_t* asteroid) {
    return asteroid->active && (asteroid->tier == ASTEROID_TIER_S);
}

float asteroid_progress_ratio(const asteroid_t* asteroid) {
    if (asteroid_is_collectible(asteroid) && (asteroid->max_ore > 0.0f)) {
        return clampf(asteroid->ore / asteroid->max_ore, 0.0f, 1.0f);
    }
    if (asteroid->max_hp > 0.0f) {
        return clampf(asteroid->hp / asteroid->max_hp, 0.0f, 1.0f);
    }
    return 0.0f;
}

const char* asteroid_tier_name(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return "Titan";
        case ASTEROID_TIER_XL: return "XL";
        case ASTEROID_TIER_L: return "L";
        case ASTEROID_TIER_M: return "M";
        case ASTEROID_TIER_S: return "S";
        case ASTEROID_TIER_COUNT: default: return "?";
    }
}

const char* asteroid_tier_kind(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return "titan";
        case ASTEROID_TIER_XL: return "body";
        case ASTEROID_TIER_L: return "rock";
        case ASTEROID_TIER_M: return "shard";
        case ASTEROID_TIER_S: return "fragment";
        case ASTEROID_TIER_COUNT: default: return "debris";
    }
}

float asteroid_spin_limit(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return 0.06f;
        case ASTEROID_TIER_XL: return 0.16f;
        case ASTEROID_TIER_L: return 0.24f;
        case ASTEROID_TIER_M: return 0.38f;
        case ASTEROID_TIER_S: return 0.62f;
        case ASTEROID_TIER_COUNT: default: return 0.2f;
    }
}

float asteroid_radius_min(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return 180.0f;
        case ASTEROID_TIER_XL: return 54.0f;
        case ASTEROID_TIER_L: return 34.0f;
        case ASTEROID_TIER_M: return 20.0f;
        case ASTEROID_TIER_S: return 11.0f;
        case ASTEROID_TIER_COUNT: default: return 16.0f;
    }
}

float asteroid_radius_max(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return 350.0f;
        case ASTEROID_TIER_XL: return 78.0f;
        case ASTEROID_TIER_L: return 48.0f;
        case ASTEROID_TIER_M: return 30.0f;
        case ASTEROID_TIER_S: return 16.0f;
        case ASTEROID_TIER_COUNT: default: return 18.0f;
    }
}

float asteroid_hp_min(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return 800.0f;
        case ASTEROID_TIER_XL: return 120.0f;
        case ASTEROID_TIER_L: return 68.0f;
        case ASTEROID_TIER_M: return 32.0f;
        case ASTEROID_TIER_S: return 10.0f;
        case ASTEROID_TIER_COUNT: default: return 8.0f;
    }
}

float asteroid_hp_max(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL: return 1400.0f;
        case ASTEROID_TIER_XL: return 170.0f;
        case ASTEROID_TIER_L: return 96.0f;
        case ASTEROID_TIER_M: return 46.0f;
        case ASTEROID_TIER_S: return 18.0f;
        case ASTEROID_TIER_COUNT: default: return 12.0f;
    }
}

void clear_asteroid(asteroid_t* asteroid) {
    bool was_active = asteroid->active;
    memset(asteroid, 0, sizeof(*asteroid));
    if (was_active) asteroid->net_dirty = true; /* signal deactivation to network */
}

static float rand_range_rng(float min_val, float max_val, uint32_t* rng) {
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x;
    float t = (float)(x & 0x00FFFFFFu) / 16777215.0f;
    return min_val + (max_val - min_val) * t;
}

void configure_asteroid_tier(asteroid_t* asteroid, asteroid_tier_t tier, commodity_t commodity, uint32_t* rng) {
    float spin_limit = asteroid_spin_limit(tier);
    asteroid->active = true;
    asteroid->tier = tier;
    asteroid->commodity = commodity;
    asteroid->radius = rand_range_rng(asteroid_radius_min(tier), asteroid_radius_max(tier), rng);
    asteroid->max_hp = rand_range_rng(asteroid_hp_min(tier), asteroid_hp_max(tier), rng);
    asteroid->hp = asteroid->max_hp;
    asteroid->max_ore = 0.0f;
    asteroid->ore = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        asteroid->max_ore = rand_range_rng(8.0f, 14.0f, rng);
        asteroid->ore = asteroid->max_ore;
    }
    asteroid->rotation = rand_range_rng(0.0f, TWO_PI_F, rng);
    asteroid->spin = rand_range_rng(-spin_limit, spin_limit, rng);
    asteroid->seed = rand_range_rng(0.0f, 100.0f, rng);
    asteroid->age = 0.0f;
}

void step_asteroid_dynamics(asteroid_t* asteroids, int count, vec2 ship_pos, float dt) {
    float cleanup_distance_sq = FRACTURE_CHILD_CLEANUP_DISTANCE * FRACTURE_CHILD_CLEANUP_DISTANCE;

    for (int i = 0; i < count; i++) {
        asteroid_t* asteroid = &asteroids[i];
        if (!asteroid->active) continue;

        asteroid->rotation += asteroid->spin * dt;
        asteroid->pos = v2_add(asteroid->pos, v2_scale(asteroid->vel, dt));
        asteroid->vel = v2_scale(asteroid->vel, 1.0f / (1.0f + (0.42f * dt)));
        asteroid->age += dt;

        /* Clean up asteroids that drift too far from any station signal.
         * Check distance from origin as a coarse safety net. */
        float max_distance = WORLD_RADIUS * 1.5f;
        if (v2_len_sq(asteroid->pos) > (max_distance * max_distance)) {
            clear_asteroid(asteroid);
            continue;
        }

        if (asteroid->fracture_child && (asteroid->age >= FRACTURE_CHILD_CLEANUP_AGE)) {
            if (v2_dist_sq(asteroid->pos, ship_pos) > cleanup_distance_sq) {
                clear_asteroid(asteroid);
            }
        }
    }
}

int find_mining_target(const asteroid_t* asteroids, int count, vec2 origin, vec2 forward, float range, int mining_level) {
    /* Max tier: Level 0=M, 1=L, 2=XL, 3+=XXL */
    int max_tier = (mining_level >= 3) ? ASTEROID_TIER_XXL :
                   (mining_level == 2) ? ASTEROID_TIER_XL :
                   (mining_level == 1) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
    int best_index = -1;
    float best_projection = range + 1.0f;

    for (int i = 0; i < count; i++) {
        const asteroid_t* asteroid = &asteroids[i];
        if (!asteroid->active || asteroid_is_collectible(asteroid)) continue;
        if (asteroid->tier < max_tier) continue; /* tier enum: XXL=0 is biggest */

        vec2 to_asteroid = v2_sub(asteroid->pos, origin);
        float projection = v2_dot(to_asteroid, forward);
        if ((projection < 0.0f) || (projection > range + asteroid->radius)) continue;

        float distance_from_ray = fabsf(v2_cross(to_asteroid, forward));
        if (distance_from_ray > (asteroid->radius + 9.0f)) continue;

        if (projection < best_projection) {
            best_projection = projection;
            best_index = i;
        }
    }
    return best_index;
}
