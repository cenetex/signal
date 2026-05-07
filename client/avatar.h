/*
 * avatar.h — Station portrait fetch, decode, and texture cache.
 * Portraits are static PNG/JPG files on S3 CDN, fetched async.
 */
#ifndef AVATAR_H
#define AVATAR_H

#include <stdbool.h>
#include <stdint.h>

enum {
    AVATAR_STATE_IDLE,
    AVATAR_STATE_FETCHING,
    AVATAR_STATE_READY,
    AVATAR_STATE_FAILED,
};

typedef struct {
    char text[256];
    float band_min, band_max;  /* signal strength range [0.0, 1.0] */
} motd_tier_t;

typedef struct {
    int state;
    uint32_t image_id;     /* sg_image.id */
    uint32_t view_id;      /* sg_view.id */
    uint32_t sampler_id;   /* sg_sampler.id */
    bool texture_valid;
    int width, height;
    char slug[32];         /* cached slug to detect changes */
    motd_tier_t tiers[4];  /* 0=common, 1=uncommon, 2=rare, 3=ultra_rare */
    bool motd_fetched;
    uint32_t generated_at;
    uint32_t seed;
} avatar_cache_t;

void avatar_init(void);
void avatar_shutdown(void);

/* Begin async fetch of portrait for station. No-op if already cached. */
void avatar_fetch(int station_index, const char *station_slug);

/* Get cache entry for a station (may be IDLE, FETCHING, READY, FAILED). */
const avatar_cache_t *avatar_get(int station_index);

/* Get rarity tier index for signal strength. Returns 0-3, or -1 if invalid.
 * Pure function — defined inline in the header so the test target can use
 * it without pulling in sokol-dependent avatar.c. */
static inline int avatar_motd_tier_for_signal(const avatar_cache_t *av,
                                              float signal_strength) {
    if (!av || signal_strength < 0.0f) return -1;
    for (int i = 0; i < 4; i++) {
        if (signal_strength >= av->tiers[i].band_min &&
            signal_strength <= av->tiers[i].band_max) {
            return i;
        }
    }
    return -1;
}

/* Get string label for tier index ("COMMON", "UNCOMMON", "RARE", "ULTRA_RARE"). */
static inline const char *avatar_motd_tier_label(int tier_index) {
    static const char *labels[] = { "COMMON", "UNCOMMON", "RARE", "ULTRA_RARE" };
    if (tier_index < 0 || tier_index >= 4) return "UNKNOWN";
    return labels[tier_index];
}

#endif /* AVATAR_H */
