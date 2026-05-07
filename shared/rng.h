#ifndef RNG_H
#define RNG_H

#include <stdint.h>

uint32_t rng_next(uint32_t* state);
float    randf(uint32_t* state);
float    rand_range(uint32_t* state, float min_value, float max_value);
int      rand_int(uint32_t* state, int min_value, int max_value);

#endif /* RNG_H */
