/*
 * belt.h — Procedural asteroid belt density field.
 * Evaluates noise-based density and ore bias at any world position.
 * Deterministic from seed. No pre-generated data.
 */
#ifndef BELT_H
#define BELT_H

#include <math.h>
#include <stdint.h>
#include "types.h"

/* ------------------------------------------------------------------ */
/* Permutation-based Perlin noise (2D)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t perm[512];
} noise2d_t;

static inline void noise2d_init(noise2d_t *n, uint32_t seed) {
    /* Fisher-Yates shuffle with xorshift RNG */
    for (int i = 0; i < 256; i++) n->perm[i] = (uint8_t)i;
    uint32_t s = seed ? seed : 1;
    for (int i = 255; i > 0; i--) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        int j = (int)(s % (uint32_t)(i + 1));
        uint8_t tmp = n->perm[i];
        n->perm[i] = n->perm[j];
        n->perm[j] = tmp;
    }
    for (int i = 0; i < 256; i++) n->perm[256 + i] = n->perm[i];
}

static inline float noise2d_fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static inline float noise2d_grad(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

static inline float noise2d_eval(const noise2d_t *n, float x, float y) {
    int xi = (int)floorf(x) & 255;
    int yi = (int)floorf(y) & 255;
    float xf = x - floorf(x);
    float yf = y - floorf(y);
    float u = noise2d_fade(xf);
    float v = noise2d_fade(yf);
    int aa = n->perm[n->perm[xi] + yi];
    int ab = n->perm[n->perm[xi] + yi + 1];
    int ba = n->perm[n->perm[xi + 1] + yi];
    int bb = n->perm[n->perm[xi + 1] + yi + 1];
    float x1 = aa + (ba - aa) * 0.0f; /* suppress warning */
    (void)x1;
    float g_aa = noise2d_grad(aa, xf, yf);
    float g_ba = noise2d_grad(ba, xf - 1.0f, yf);
    float g_ab = noise2d_grad(ab, xf, yf - 1.0f);
    float g_bb = noise2d_grad(bb, xf - 1.0f, yf - 1.0f);
    float l1 = g_aa + u * (g_ba - g_aa);
    float l2 = g_ab + u * (g_bb - g_ab);
    return l1 + v * (l2 - l1);
}

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

static inline void belt_field_init(belt_field_t *bf, uint32_t seed, float world_scale) {
    noise2d_init(&bf->n1, seed);
    noise2d_init(&bf->n2, seed + 7919u);
    noise2d_init(&bf->n3, seed + 31337u);
    noise2d_init(&bf->n4, seed + 65537u);
    noise2d_init(&bf->n5, seed + 99991u);
    bf->world_scale = world_scale;
}

/*
 * Returns asteroid density at world position (x, y).
 * 0.0 = empty space, 1.0 = dense ocean of rock.
 *
 * Layers:
 *   - Ridged noise for sharp belt centers (rivers)
 *   - Pool noise for filled areas (lakes)
 *   - Regional bias for large-scale ocean/desert (continents)
 *   - Fine grain for cluster variation
 *   - Hard floor threshold for truly empty lanes
 */
static inline float belt_density_at(const belt_field_t *bf, float x, float y) {
    float nx = x / bf->world_scale;
    float ny = y / bf->world_scale;

    /* Ridged noise: belt structure */
    float r = 1.0f - fabsf(noise2d_eval(&bf->n1, nx * 3.0f, ny * 3.0f));
    r = r * r;  /* sharpen ridges */

    /* Pool noise: medium variation */
    float p = noise2d_eval(&bf->n2, nx * 7.0f, ny * 7.0f);
    p = fminf(fmaxf((p + 0.3f) * 0.8f, 0.0f), 1.0f);

    /* Fine grain */
    float g = noise2d_eval(&bf->n1, nx * 18.0f, ny * 18.0f) * 0.15f;

    /* Regional bias */
    float rb = noise2d_eval(&bf->n2, nx * 1.2f, ny * 1.2f);
    rb = fminf(fmaxf((rb + 0.2f) * 0.7f, 0.0f), 1.0f);

    /* Combine */
    float d = r * 0.6f + p * 0.3f + g;
    d = d * (0.4f + rb * 0.6f);

    /* Hard floor: empty lanes */
    d = fminf(fmaxf(d, 0.0f), 1.0f);
    if (d < 0.15f) return 0.0f;
    d = (d - 0.15f) / 0.85f;

    /* Slight boost to mid-range */
    d = powf(d, 0.7f);
    return fminf(fmaxf(d, 0.0f), 1.0f);
}

/*
 * Returns the dominant ore commodity at world position (x, y).
 * Uses separate noise layers so ore zones are geographically distinct.
 */
static inline commodity_t belt_ore_at(const belt_field_t *bf, float x, float y) {
    float nx = x / bf->world_scale;
    float ny = y / bf->world_scale;

    /* Rarity: ferrite 80%, crystal 16%, cuprite 4%.
     * Bias noise weights so ferrite dominates most of the map,
     * crystal appears in moderate pockets, cuprite is rare. */
    float fe = fmaxf(noise2d_eval(&bf->n3, nx * 4.0f, ny * 4.0f) + 0.5f, 0.1f) * 4.0f;
    float cu = fmaxf(noise2d_eval(&bf->n4, nx * 4.0f, ny * 4.0f) + 0.5f, 0.1f) * 0.2f;
    float cr = fmaxf(noise2d_eval(&bf->n5, nx * 4.0f, ny * 4.0f) + 0.5f, 0.1f) * 0.8f;

    if (fe >= cu && fe >= cr) return COMMODITY_FERRITE_ORE;
    if (cr >= cu) return COMMODITY_CRYSTAL_ORE;
    return COMMODITY_CUPRITE_ORE;
}

#endif /* BELT_H */
