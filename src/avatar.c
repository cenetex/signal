/*
 * avatar.c — Station portrait fetch, decode, and texture cache.
 * Fetches PNG/JPG from S3 CDN, decodes with stb_image, uploads
 * as sokol_gfx texture for HUD rendering.
 */
#include "avatar.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "sokol_gfx.h"
#include "types.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define ASSET_CDN "https://signal-ratimics-assets.s3.amazonaws.com"

static avatar_cache_t cache[MAX_STATIONS];
static uint32_t shared_sampler;
static bool initialized;

void avatar_init(void) {
    memset(cache, 0, sizeof(cache));
    sg_sampler samp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
    });
    shared_sampler = samp.id;
    initialized = true;
}

void avatar_shutdown(void) {
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (cache[i].texture_valid) {
            sg_destroy_image((sg_image){ cache[i].image_id });
        }
    }
    if (shared_sampler) {
        sg_destroy_sampler((sg_sampler){ shared_sampler });
        shared_sampler = 0;
    }
    initialized = false;
}

static void upload_texture(avatar_cache_t *entry, const unsigned char *rgba, int w, int h) {
    if (entry->texture_valid) {
        sg_destroy_image((sg_image){ entry->image_id });
        entry->texture_valid = false;
    }
    sg_image img = sg_make_image(&(sg_image_desc){
        .width = w,
        .height = h,
        .pixel_format = SG_PIXELFORMAT_RGBA8,
        .usage.stream_update = true,
    });
    sg_update_image(img, &(sg_image_data){
        .mip_levels[0] = { .ptr = rgba, .size = (size_t)(w * h * 4) },
    });
    sg_view view = sg_make_view(&(sg_view_desc){ .texture.image = img });
    entry->image_id = img.id;
    entry->view_id = view.id;
    entry->sampler_id = shared_sampler;
    entry->width = w;
    entry->height = h;
    entry->texture_valid = true;
    entry->state = AVATAR_STATE_READY;
    printf("[avatar] loaded portrait %dx%d for '%s'\n", w, h, entry->slug);
}

static void decode_and_upload(avatar_cache_t *entry, void *data, int size) {
    int w, h, channels;
    unsigned char *pixels = stbi_load_from_memory(
        (const unsigned char *)data, size, &w, &h, &channels, 4);
    if (!pixels) {
        fprintf(stderr, "[avatar] failed to decode image for '%s'\n", entry->slug);
        entry->state = AVATAR_STATE_FAILED;
        return;
    }
    upload_texture(entry, pixels, w, h);
    stbi_image_free(pixels);
}

#ifdef __EMSCRIPTEN__

static void on_fetch_success(void *user, void *data, int size) {
    avatar_cache_t *entry = (avatar_cache_t *)user;
    decode_and_upload(entry, data, size);
}

static void on_fetch_error(void *user) {
    avatar_cache_t *entry = (avatar_cache_t *)user;
    fprintf(stderr, "[avatar] fetch failed for '%s'\n", entry->slug);
    entry->state = AVATAR_STATE_FAILED;
}

#else

static unsigned char *load_file_bytes(const char *path, int *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (buf) *out_size = (int)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

#endif

void avatar_fetch(int station_index, const char *station_slug) {
    if (!initialized) return;
    if (station_index < 0 || station_index >= MAX_STATIONS) return;
    if (!station_slug || !station_slug[0]) return;

    avatar_cache_t *entry = &cache[station_index];

    /* Already cached for this slug? */
    if (entry->state == AVATAR_STATE_READY && strcmp(entry->slug, station_slug) == 0)
        return;
    if (entry->state == AVATAR_STATE_FETCHING)
        return;

    snprintf(entry->slug, sizeof(entry->slug), "%s", station_slug);
    entry->state = AVATAR_STATE_FETCHING;

    char url[256];
    snprintf(url, sizeof(url), "%s/stations/%s/portrait.png", ASSET_CDN, station_slug);

#ifdef __EMSCRIPTEN__
    emscripten_async_wget_data(url, entry, on_fetch_success, on_fetch_error);
#else
    /* Native: try local file first, then URL would need libcurl (not available) */
    char local_path[256];
    snprintf(local_path, sizeof(local_path), "assets/stations/%s/portrait.png", station_slug);
    int file_size = 0;
    unsigned char *data = load_file_bytes(local_path, &file_size);
    if (data && file_size > 0) {
        decode_and_upload(entry, data, file_size);
        free(data);
    } else {
        /* No local file — generate a procedural placeholder */
        unsigned char placeholder[64 * 64 * 4];
        uint32_t hash = 0;
        for (const char *c = station_slug; *c; c++) hash = hash * 31 + (uint32_t)*c;
        uint8_t r = 60 + (hash & 0x3F);
        uint8_t g = 80 + ((hash >> 8) & 0x3F);
        uint8_t b = 100 + ((hash >> 16) & 0x3F);
        for (int i = 0; i < 64 * 64; i++) {
            placeholder[i * 4 + 0] = r;
            placeholder[i * 4 + 1] = g;
            placeholder[i * 4 + 2] = b;
            placeholder[i * 4 + 3] = 200;
        }
        /* Draw a letter in the center (crude 5x7 block) */
        char letter = station_slug[0] & ~0x20; /* uppercase */
        (void)letter; /* placeholder — just colored square */
        upload_texture(entry, placeholder, 64, 64);
    }
#endif
}

const avatar_cache_t *avatar_get(int station_index) {
    if (station_index < 0 || station_index >= MAX_STATIONS) return NULL;
    return &cache[station_index];
}
