/*
 * chunk.h — Deterministic asteroid terrain generation from belt noise.
 *
 * The world is tiled into 400×400u chunks. Each chunk deterministically
 * generates 0-6 asteroids based on belt density and a chunk-local RNG
 * seeded from the world seed + chunk coordinates.
 *
 * The same chunk always produces the same asteroids at the same positions.
 * This is the foundation for "asteroids as terrain" — discoverable,
 * persistent geology that replaces the random respawn pool.
 */
#ifndef CHUNK_H
#define CHUNK_H

#include <math.h>
#include <string.h>
#include "types.h"
#include "belt.h"
#include "asteroid.h"
#include "rng.h"

#define CHUNK_SIZE 400.0f
#define CHUNK_MAX_ASTEROIDS 6

/* Lightweight descriptor — everything needed to materialize an asteroid. */
typedef struct {
    vec2 pos;
    asteroid_tier_t tier;
    commodity_t commodity;
    float radius, hp, ore;
    float rotation, spin, seed;
} chunk_asteroid_t;

/* Convert world position to chunk coordinates. */
static inline void chunk_coord(float wx, float wy, int32_t *cx, int32_t *cy) {
    *cx = (int32_t)floorf(wx / CHUNK_SIZE);
    *cy = (int32_t)floorf(wy / CHUNK_SIZE);
}

/* Deterministic seed for a chunk. Uses hash combine (FNV-1a style). */
static inline uint32_t chunk_seed(uint32_t world_seed, int32_t cx, int32_t cy) {
    uint32_t h = world_seed ^ 0x811C9DC5u;
    uint32_t ux, uy;
    memcpy(&ux, &cx, sizeof(ux));
    memcpy(&uy, &cy, sizeof(uy));
    h ^= ux; h *= 0x01000193u;
    h ^= uy; h *= 0x01000193u;
    /* Extra mix to avoid correlation between adjacent chunks */
    h ^= h >> 16;
    h *= 0x45D9F3Bu;
    h ^= h >> 16;
    return h ? h : 1u;
}

/* Generate deterministic asteroids for a chunk. Returns count (0 to max_out).
 * Caller provides belt field for density/ore queries. */
static inline int chunk_generate(const belt_field_t *belt, uint32_t world_seed,
                                  int32_t cx, int32_t cy,
                                  chunk_asteroid_t *out, int max_out) {
    if (max_out <= 0) return 0;
    if (max_out > CHUNK_MAX_ASTEROIDS) max_out = CHUNK_MAX_ASTEROIDS;

    /* Chunk center in world coords */
    float center_x = ((float)cx + 0.5f) * CHUNK_SIZE;
    float center_y = ((float)cy + 0.5f) * CHUNK_SIZE;

    /* Density at chunk center determines how many candidates */
    float density = belt_density_at(belt, center_x, center_y);
    if (density < 0.01f) return 0;

    /* 0-6 asteroids based on density */
    int target = (int)(density * 6.0f);
    if (target < 1) target = 1;
    if (target > max_out) target = max_out;

    uint32_t rng = chunk_seed(world_seed, cx, cy);
    int count = 0;

    for (int i = 0; i < target; i++) {
        /* Position: jittered within chunk */
        float jx = rand_range(&rng, 0.0f, CHUNK_SIZE);
        float jy = rand_range(&rng, 0.0f, CHUNK_SIZE);
        float wx = (float)cx * CHUNK_SIZE + jx;
        float wy = (float)cy * CHUNK_SIZE + jy;

        /* Fine-grained density check at actual position */
        float local_d = belt_density_at(belt, wx, wy);
        if (local_d < 0.05f) continue;

        /* Tier from density: high density → bigger rocks */
        asteroid_tier_t tier;
        float tier_roll = rand_range(&rng, 0.0f, 1.0f);
        if (local_d > 0.7f && tier_roll < 0.08f)
            tier = ASTEROID_TIER_XXL;
        else if (local_d > 0.4f && tier_roll < 0.25f)
            tier = ASTEROID_TIER_XL;
        else if (tier_roll < 0.55f)
            tier = ASTEROID_TIER_L;
        else
            tier = ASTEROID_TIER_M;

        /* Ore from belt */
        commodity_t commodity = belt_ore_at(belt, wx, wy);

        /* Properties from tier — uses the same configure functions */
        float radius = rand_range(&rng, asteroid_radius_min(tier), asteroid_radius_max(tier));
        float hp = rand_range(&rng, asteroid_hp_min(tier), asteroid_hp_max(tier));
        float ore = 0.0f;
        if (tier == ASTEROID_TIER_S) ore = rand_range(&rng, 8.0f, 14.0f);
        float spin_limit = asteroid_spin_limit(tier);
        float rotation = rand_range(&rng, 0.0f, TWO_PI_F);
        float spin = rand_range(&rng, -spin_limit, spin_limit);
        float seed = rand_range(&rng, 0.0f, 100.0f);

        out[count++] = (chunk_asteroid_t){
            .pos = v2(wx, wy),
            .tier = tier,
            .commodity = commodity,
            .radius = radius,
            .hp = hp,
            .ore = ore,
            .rotation = rotation,
            .spin = spin,
            .seed = seed,
        };
    }

    return count;
}

#endif /* CHUNK_H */
