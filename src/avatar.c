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

/* Parse multi-tier MOTD JSON from S3:
 *   {"messages":{"common":"...","uncommon":"...","rare":"...","ultra_rare":"..."},
 *    "bands":{"common":[0.8,1.0],...},"generated_at":123456,"seed":42}
 * Returns 1 on success, 0 on parse failure.
 *
 * Defined outside the __EMSCRIPTEN__ branch so both web and native callers
 * resolve it (the native local-MOTD-load path uses it too). */
static int parse_motd_json(avatar_cache_t *entry, const char *json, int json_size) {
    (void)json_size;
    memset(entry->tiers, 0, sizeof(entry->tiers));

    const char *tier_names[] = { "common", "uncommon", "rare", "ultra_rare" };
    const char *messages_start = strstr(json, "\"messages\":");
    if (!messages_start) return 0;

    for (int i = 0; i < 4; i++) {
        char search_buf[64];
        snprintf(search_buf, sizeof(search_buf), "\"%s\":\"", tier_names[i]);
        const char *msg_start = strstr(messages_start, search_buf);
        if (!msg_start) return 0;
        msg_start += strlen(search_buf);

        const char *msg_end = strchr(msg_start, '"');
        if (!msg_end) return 0;

        int msg_len = (int)(msg_end - msg_start);
        if (msg_len > 255) msg_len = 255;
        memcpy(entry->tiers[i].text, msg_start, (size_t)msg_len);
        entry->tiers[i].text[msg_len] = '\0';
    }

    /* Parse bands: [min, max] for each tier */
    const char *bands_start = strstr(json, "\"bands\":");
    if (bands_start) {
        for (int i = 0; i < 4; i++) {
            char search_buf[64];
            snprintf(search_buf, sizeof(search_buf), "\"%s\":[", tier_names[i]);
            const char *band_start = strstr(bands_start, search_buf);
            if (band_start) {
                band_start += strlen(search_buf);
                if (sscanf(band_start, "%f,%f]", &entry->tiers[i].band_min,
                          &entry->tiers[i].band_max) != 2) {
                    const float defaults[][2] = {{0.8f,1.0f}, {0.5f,0.8f}, {0.2f,0.5f}, {0.0f,0.2f}};
                    entry->tiers[i].band_min = defaults[i][0];
                    entry->tiers[i].band_max = defaults[i][1];
                }
            } else {
                const float defaults[][2] = {{0.8f,1.0f}, {0.5f,0.8f}, {0.2f,0.5f}, {0.0f,0.2f}};
                entry->tiers[i].band_min = defaults[i][0];
                entry->tiers[i].band_max = defaults[i][1];
            }
        }
    } else {
        const float defaults[][2] = {{0.8f,1.0f}, {0.5f,0.8f}, {0.2f,0.5f}, {0.0f,0.2f}};
        for (int i = 0; i < 4; i++) {
            entry->tiers[i].band_min = defaults[i][0];
            entry->tiers[i].band_max = defaults[i][1];
        }
    }

    /* Parse metadata */
    const char *ts_start = strstr(json, "\"generated_at\":");
    if (ts_start) sscanf(ts_start + strlen("\"generated_at\":"), "%u", &entry->generated_at);
    const char *seed_start = strstr(json, "\"seed\":");
    if (seed_start) sscanf(seed_start + strlen("\"seed\":"), "%u", &entry->seed);

    return 1;
}

#ifdef __EMSCRIPTEN__

static void on_fetch_success(void *user, void *data, int size) {
    avatar_cache_t *entry = (avatar_cache_t *)user;
    decode_and_upload(entry, data, size);
}

static void on_fetch_error(void *user) {
    avatar_cache_t *entry = (avatar_cache_t *)user;
    fprintf(stderr, "[avatar] portrait fetch failed for '%s'\n", entry->slug);
    entry->state = AVATAR_STATE_FAILED;
}


static void on_motd_success(void *user, void *data, int size) {
    avatar_cache_t *entry = (avatar_cache_t *)user;
    char *json = (char *)malloc((size_t)(size + 1));
    if (!json) {
        fprintf(stderr, "[avatar] malloc failed for MOTD JSON\n");
        return;
    }
    memcpy(json, data, (size_t)size);
    json[size] = '\0';

    if (parse_motd_json(entry, json, size)) {
        entry->motd_fetched = true;
        printf("[avatar] MOTD tiers loaded for '%s': common=%.30s...\n",
               entry->slug, entry->tiers[0].text);
    } else {
        fprintf(stderr, "[avatar] MOTD JSON parse failed for '%s'\n", entry->slug);
    }
    free(json);
}

static void on_motd_error(void *user) {
    avatar_cache_t *entry = (avatar_cache_t *)user;
    fprintf(stderr, "[avatar] MOTD fetch failed for '%s'\n", entry->slug);
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
    /* Also fetch MOTD JSON */
    if (!entry->motd_fetched) {
        char motd_url[256];
        snprintf(motd_url, sizeof(motd_url), "%s/stations/%s/motd.json", ASSET_CDN, station_slug);
        emscripten_async_wget_data(motd_url, entry, on_motd_success, on_motd_error);
    }
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

    /* Also try to load MOTD JSON locally */
    if (!entry->motd_fetched) {
        char motd_path[256];
        snprintf(motd_path, sizeof(motd_path), "assets/stations/%s/motd.json", station_slug);
        int motd_size = 0;
        unsigned char *motd_data = load_file_bytes(motd_path, &motd_size);
        if (motd_data && motd_size > 0) {
            char *json = (char *)malloc((size_t)(motd_size + 1));
            if (json) {
                memcpy(json, motd_data, (size_t)motd_size);
                json[motd_size] = '\0';
                if (parse_motd_json(entry, json, motd_size)) {
                    entry->motd_fetched = true;
                    printf("[avatar] MOTD tiers loaded for '%s' from file\n", entry->slug);
                }
                free(json);
            }
            free(motd_data);
        }
    }
#endif
}

const avatar_cache_t *avatar_get(int station_index) {
    if (station_index < 0 || station_index >= MAX_STATIONS) return NULL;
    return &cache[station_index];
}

/* avatar_motd_tier_for_signal and avatar_motd_tier_label are defined
 * inline in avatar.h so the test target can use them without linking
 * sokol-dependent avatar.c. */
