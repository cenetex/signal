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
    int state;
    uint32_t image_id;     /* sg_image.id */
    uint32_t view_id;      /* sg_view.id */
    uint32_t sampler_id;   /* sg_sampler.id */
    bool texture_valid;
    int width, height;
    char slug[32];         /* cached slug to detect changes */
} avatar_cache_t;

void avatar_init(void);
void avatar_shutdown(void);

/* Begin async fetch of portrait for station. No-op if already cached. */
void avatar_fetch(int station_index, const char *station_slug);

/* Get cache entry for a station (may be IDLE, FETCHING, READY, FAILED). */
const avatar_cache_t *avatar_get(int station_index);

#endif /* AVATAR_H */
