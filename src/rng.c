#include "rng.h"
#include "math_util.h"

uint32_t rng_next(uint32_t* state) {
    if (*state == 0) {
        *state = 0xA341316Cu;
    }
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

float randf(uint32_t* state) {
    return (float)(rng_next(state) & 0x00FFFFFFu) / 16777215.0f;
}

float rand_range(uint32_t* state, float min_value, float max_value) {
    return lerpf(min_value, max_value, randf(state));
}

int rand_int(uint32_t* state, int min_value, int max_value) {
    return min_value + (int)(rng_next(state) % (uint32_t)(max_value - min_value + 1));
}
