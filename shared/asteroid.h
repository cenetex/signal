#ifndef ASTEROID_H
#define ASTEROID_H

#include "types.h"

asteroid_tier_t asteroid_next_tier(asteroid_tier_t tier);
bool asteroid_is_collectible(const asteroid_t* asteroid);
float asteroid_progress_ratio(const asteroid_t* asteroid);

const char* asteroid_tier_name(asteroid_tier_t tier);
const char* asteroid_tier_kind(asteroid_tier_t tier);
float asteroid_spin_limit(asteroid_tier_t tier);
float asteroid_radius_min(asteroid_tier_t tier);
float asteroid_radius_max(asteroid_tier_t tier);
float asteroid_hp_min(asteroid_tier_t tier);
float asteroid_hp_max(asteroid_tier_t tier);

void clear_asteroid(asteroid_t* asteroid);
void configure_asteroid_tier(asteroid_t* asteroid, asteroid_tier_t tier, commodity_t commodity, uint32_t* rng);

void step_asteroid_dynamics(asteroid_t* asteroids, int count, vec2 ship_pos, float dt);

int find_mining_target(const asteroid_t* asteroids, int count, vec2 origin, vec2 forward, float range, int mining_level);

#endif
