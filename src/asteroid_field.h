#ifndef ASTEROID_FIELD_H
#define ASTEROID_FIELD_H

#include "types.h"

commodity_t random_raw_ore(uint32_t* rng);
asteroid_tier_t random_field_asteroid_tier(uint32_t* rng);
float client_max_signal_range(const station_t* stations, int count);

void spawn_field_asteroid_of_tier(asteroid_t* asteroid, asteroid_tier_t tier,
                                  const station_t* stations, int station_count,
                                  uint32_t* rng);
void spawn_field_asteroid(asteroid_t* asteroid,
                          const station_t* stations, int station_count,
                          uint32_t* rng);
void spawn_child_asteroid(asteroid_t* asteroid, asteroid_tier_t tier,
                          commodity_t commodity, vec2 pos, vec2 vel,
                          uint32_t* rng);
int desired_child_count(asteroid_tier_t tier, uint32_t* rng);
void inspect_asteroid_field(const asteroid_t* asteroids, int count,
                            int* seeded_count, int* first_inactive_slot);

#endif /* ASTEROID_FIELD_H */
