/*
 * belt.h — Procedural asteroid belt density field.
 * Evaluates noise-based density and ore bias at any world position.
 * Deterministic from seed. No pre-generated data.
 *
 * Implementations live in belt.c so editing the noise/density/ore math
 * doesn't trigger a recompile of every TU that touches the belt struct
 * (which used to fan out to ~40 .o files).
 */
#ifndef BELT_H
#define BELT_H

#include <stdint.h>
#include "types.h"

/* ------------------------------------------------------------------ */
/* Permutation-based Perlin noise (2D)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t perm[512];
} noise2d_t;

void  noise2d_init(noise2d_t *n, uint32_t seed);
float noise2d_eval(const noise2d_t *n, float x, float y);

/* ------------------------------------------------------------------ */
/* Belt density field                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    noise2d_t n1;  /* belt structure */
    noise2d_t n2;  /* pools + regional bias */
    noise2d_t n3;  /* ore: ferrite */
    noise2d_t n4;  /* ore: cuprite */
    noise2d_t n5;  /* ore: crystal */
    float world_scale;
} belt_field_t;

void        belt_field_init(belt_field_t *bf, uint32_t seed, float world_scale);

/* Returns asteroid density at world position (x, y).
 * 0.0 = empty space, 1.0 = dense ocean of rock. */
float       belt_density_at(const belt_field_t *bf, float x, float y);

/* Returns the dominant ore commodity at world position (x, y). */
commodity_t belt_ore_at(const belt_field_t *bf, float x, float y);

#endif /* BELT_H */
