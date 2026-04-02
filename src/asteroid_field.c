#include "asteroid_field.h"
#include "rng.h"
#include "asteroid.h"
#include "game_sim.h"  /* for WORLD_RADIUS */

commodity_t random_raw_ore(uint32_t* rng) {
    return (commodity_t)rand_int(rng, (int)COMMODITY_FERRITE_ORE, (int)COMMODITY_CRYSTAL_ORE);
}

asteroid_tier_t random_field_asteroid_tier(uint32_t* rng) {
    float roll = randf(rng);
    if (roll < 0.03f) {
        return ASTEROID_TIER_XXL;
    }
    if (roll < 0.26f) {
        return ASTEROID_TIER_XL;
    }
    if (roll < 0.70f) {
        return ASTEROID_TIER_L;
    }
    return ASTEROID_TIER_M;
}

float client_max_signal_range(const station_t* stations, int count) {
    float best = 0.0f;
    for (int i = 0; i < count; i++) {
        if (stations[i].signal_range > best) best = stations[i].signal_range;
    }
    return best > 0.0f ? best : WORLD_RADIUS;
}

void spawn_field_asteroid_of_tier(asteroid_t* asteroid, asteroid_tier_t tier,
                                  const station_t* stations, int station_count,
                                  uint32_t* rng) {
    float angle = rand_range(rng, 0.0f, TWO_PI_F);
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, random_raw_ore(rng), rng);
    asteroid->fracture_child = false;
    /* Pick a random station and spawn within its signal range */
    int stn = rand_int(rng, 0, station_count - 1);
    float sr = stations[stn].signal_range;
    if (sr <= 0.0f) sr = client_max_signal_range(stations, station_count);
    if (tier == ASTEROID_TIER_XXL) {
        vec2 center = stations[stn].pos;
        asteroid->pos = v2_add(center, v2(cosf(angle) * sr, sinf(angle) * sr));
        float inward_speed = rand_range(rng, 15.0f, 30.0f);
        asteroid->vel = v2(-cosf(angle) * inward_speed, -sinf(angle) * inward_speed);
    } else {
        vec2 center = stations[stn].pos;
        float distance = rand_range(rng, 420.0f, sr - 180.0f);
        asteroid->pos = v2_add(center, v2(cosf(angle) * distance, sinf(angle) * distance));
        asteroid->vel = v2(rand_range(rng, -4.0f, 4.0f), rand_range(rng, -4.0f, 4.0f));
    }
}

void spawn_field_asteroid(asteroid_t* asteroid,
                          const station_t* stations, int station_count,
                          uint32_t* rng) {
    spawn_field_asteroid_of_tier(asteroid, random_field_asteroid_tier(rng),
                                 stations, station_count, rng);
}

void spawn_child_asteroid(asteroid_t* asteroid, asteroid_tier_t tier,
                          commodity_t commodity, vec2 pos, vec2 vel,
                          uint32_t* rng) {
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, commodity, rng);
    asteroid->fracture_child = true;
    asteroid->pos = pos;
    asteroid->vel = vel;
}

int desired_child_count(asteroid_tier_t tier, uint32_t* rng) {
    switch (tier) {
        case ASTEROID_TIER_XXL:
            return rand_int(rng, 8, 14);
        case ASTEROID_TIER_XL:
            return rand_int(rng, 2, 3);
        case ASTEROID_TIER_L:
            return rand_int(rng, 2, 3);
        case ASTEROID_TIER_M:
            return rand_int(rng, 2, 4);
        case ASTEROID_TIER_S:
        default:
            return 0;
    }
}

void inspect_asteroid_field(const asteroid_t* asteroids, int count,
                            int* seeded_count, int* first_inactive_slot) {
    *seeded_count = 0;
    *first_inactive_slot = -1;

    for (int i = 0; i < count; i++) {
        if (!asteroids[i].active) {
            if (*first_inactive_slot < 0) {
                *first_inactive_slot = i;
            }
            continue;
        }

        if (!asteroids[i].fracture_child) {
            (*seeded_count)++;
        }
    }
}
