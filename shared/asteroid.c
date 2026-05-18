#include <string.h>
#include "asteroid.h"
#include "commodity.h"
#include "rng.h"

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
    asteroid->last_towed_by = -1;
    asteroid->last_fractured_by = -1;
    asteroid->crystal_stage_station = 0xFFu;
    asteroid->crystal_stage_module = 0xFFu;
    if (was_active) asteroid->net_dirty = true; /* signal deactivation to network */
}

void configure_asteroid_tier(asteroid_t* asteroid, asteroid_tier_t tier, commodity_t commodity, uint32_t* rng) {
    float spin_limit = asteroid_spin_limit(tier);
    asteroid->active = true;
    asteroid->tier = tier;
    asteroid->commodity = commodity;
    asteroid->radius = rand_range(rng, asteroid_radius_min(tier), asteroid_radius_max(tier));
    asteroid->max_hp = rand_range(rng, asteroid_hp_min(tier), asteroid_hp_max(tier));
    asteroid->hp = asteroid->max_hp;
    asteroid->max_ore = 0.0f;
    asteroid->ore = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        (void)rng;
        asteroid->max_ore = REFINERY_INGOTS_PER_FRAGMENT;
        asteroid->ore = asteroid->max_ore;
    }
    asteroid->rotation = rand_range(rng, 0.0f, TWO_PI_F);
    asteroid->spin = rand_range(rng, -spin_limit, spin_limit);
    asteroid->seed = rand_range(rng, 0.0f, 100.0f);
    asteroid->age = 0.0f;
    asteroid->crystal_stage = CRYSTAL_STAGE_RAW;
    asteroid->crystal_stage_station = 0xFFu;
    asteroid->crystal_stage_module = 0xFFu;
}
