/*
 * episode.c — MPEG1 video episode playback for Signal Space Miner.
 * Uses pl_mpeg for decoding, sokol_gfx for texture upload, sokol_gl for rendering.
 *
 * All platforms fetch from S3 CDN. Emscripten uses async XHR, native uses
 * local file fallback (assets/ directory for development).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include "episode.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define ASSET_CDN "https://signal-ratimics-assets.s3.amazonaws.com"

static const episode_info_t episodes[EPISODE_COUNT] = {
    { "anime/ep0-first-light.mpg",     "FIRST LIGHT" },
    { "anime/ep1-keplers-law.mpg",      "KEPLER'S LAW" },
    { "anime/ep2-furnace.mpg",          "FURNACE" },
    { "anime/ep3-scaffold.mpg",         "SCAFFOLD" },
    { "anime/ep4-naming.mpg",           "NAMING" },
    { "anime/ep5-drones.mpg",           "DRONES" },
    { "anime/ep6-hauler.mpg",           "HAULER" },
    { "anime/ep7-dark-sector.mpg",      "DARK SECTOR" },
    { "anime/ep8-every-ai-dreams.mpg",  "EVERY AI DREAMS" },
    { "anime/ep9-death.mpg",            "DEATH" },
};

const episode_info_t *episode_get_info(int index) {
    if (index < 0 || index >= EPISODE_COUNT) return NULL;
    return &episodes[index];
}

/* --- Audio ring buffer helpers --- */

static int audio_buf_available(episode_state_t *ep) {
    int avail = ep->audio_write_pos - ep->audio_read_pos;
    if (avail < 0) avail += ep->audio_buffer_size;
    return avail;
}

static void audio_buf_write(episode_state_t *ep, const float *samples, int count) {
    for (int i = 0; i < count; i++) {
        ep->audio_buffer[ep->audio_write_pos] = samples[i];
        ep->audio_write_pos = (ep->audio_write_pos + 1) % ep->audio_buffer_size;
    }
}

/* --- pl_mpeg callbacks --- */

static void on_video_frame(plm_t *plm, plm_frame_t *frame, void *user) {
    (void)plm;
    episode_state_t *ep = (episode_state_t *)user;

    int w = frame->width;
    int h = frame->height;

    int rgb_size = w * h * 3;
    uint8_t *rgb = (uint8_t *)malloc((size_t)rgb_size);
    if (!rgb) return;

    plm_frame_to_rgb(frame, rgb, w * 3);

    int rgba_size = w * h * 4;
    uint8_t *rgba = (uint8_t *)malloc((size_t)rgba_size);
    if (!rgba) { free(rgb); return; }

    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }

    if (!ep->texture_valid || ep->video_width != w || ep->video_height != h) {
        if (ep->texture_valid) {
            sg_destroy_view((sg_view){ ep->view_id });
            sg_destroy_image((sg_image){ ep->texture_id });
        }
        sg_image img = sg_make_image(&(sg_image_desc){
            .width = w,
            .height = h,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .usage.stream_update = true,
        });
        sg_view view = sg_make_view(&(sg_view_desc){
            .texture.image = img,
        });
        ep->texture_id = img.id;
        ep->view_id = view.id;
        ep->video_width = w;
        ep->video_height = h;
        ep->texture_valid = true;

        if (ep->sampler_id == 0) {
            sg_sampler smp = sg_make_sampler(&(sg_sampler_desc){
                .min_filter = SG_FILTER_LINEAR,
                .mag_filter = SG_FILTER_LINEAR,
            });
            ep->sampler_id = smp.id;
        }
    }

    sg_update_image((sg_image){ ep->texture_id }, &(sg_image_data){
        .mip_levels[0] = { .ptr = rgba, .size = (size_t)rgba_size },
    });

    free(rgba);
    free(rgb);
}

static void on_audio_frame(plm_t *plm, plm_samples_t *samples, void *user) {
    (void)plm;
    episode_state_t *ep = (episode_state_t *)user;
    int count = samples->count * 2; /* stereo interleaved */
    audio_buf_write(ep, samples->interleaved, count);
}

/* --- Start playback once data is in memory --- */

static void episode_start_playback(episode_state_t *ep, uint8_t *data, size_t size) {
    plm_t *plm = plm_create_with_memory(data, size, 1); /* 1 = free data when done */
    if (!plm) {
        free(data);
        fprintf(stderr, "episode: failed to create decoder\n");
        ep->loading = false;
        return;
    }

    plm_set_video_decode_callback(plm, on_video_frame, ep);
    plm_set_audio_decode_callback(plm, on_audio_frame, ep);
    plm_set_audio_enabled(plm, 1);
    plm_set_loop(plm, 0);
    plm_set_audio_lead_time(plm, 0.1);

    ep->plm = plm;
    ep->active = true;
    ep->loading = false;
    ep->fade_timer = 0.0f;
    ep->audio_write_pos = 0;
    ep->audio_read_pos = 0;
}

/* --- Async fetch (Emscripten) --- */

#ifdef __EMSCRIPTEN__
static void on_fetch_success(void *user, void *data, int size) {
    episode_state_t *ep = (episode_state_t *)user;
    /* emscripten gives us a buffer we must copy — it frees it after callback */
    uint8_t *copy = (uint8_t *)malloc((size_t)size);
    if (!copy) {
        fprintf(stderr, "episode: out of memory for %d bytes\n", size);
        ep->loading = false;
        return;
    }
    memcpy(copy, data, (size_t)size);
    episode_start_playback(ep, copy, (size_t)size);
}

static void on_fetch_error(void *user) {
    episode_state_t *ep = (episode_state_t *)user;
    fprintf(stderr, "episode: fetch failed for ep %d (no internet?)\n", ep->pending);
    ep->loading = false;
    ep->pending = -1;
}
#endif

/* --- Native file loading (dev fallback) --- */

#ifndef __EMSCRIPTEN__
static uint8_t *load_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return NULL; }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(data); return NULL; }
    *out_size = (size_t)sz;
    return data;
}
#endif

/* --- Public API --- */

void episode_init(episode_state_t *ep) {
    memset(ep, 0, sizeof(*ep));
    ep->current = -1;
    ep->pending = -1;
    ep->fade_duration = 0.5f;
    ep->audio_buffer_size = (int)(sizeof(ep->audio_buffer) / sizeof(ep->audio_buffer[0]));
}

void episode_load(episode_state_t *ep) {
    if (ep->loaded) return;
    ep->loaded = true;
#ifdef __EMSCRIPTEN__
    int flags = emscripten_run_script_int(
        "(function(){var s=localStorage.getItem('signal_episodes');"
        "if(!s)return 0;return parseInt(s,10)||0;})()");
    for (int i = 0; i < EPISODE_COUNT; i++)
        ep->watched[i] = (flags & (1 << i)) != 0;
#endif
}

void episode_save(episode_state_t *ep) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    for (int i = 0; i < EPISODE_COUNT; i++)
        if (ep->watched[i]) flags |= (1 << i);
    char js[80];
    snprintf(js, sizeof(js),
        "localStorage.setItem('signal_episodes','%d')", flags);
    emscripten_run_script(js);
#else
    (void)ep;
#endif
}

void episode_trigger(episode_state_t *ep, int index) {
    if (index < 0 || index >= EPISODE_COUNT) return;
    if (ep->watched[index]) return;
    if (ep->active || ep->loading) return;

    ep->watched[index] = true;
    ep->current = index;
    ep->pending = index;
    episode_save(ep);

    const episode_info_t *info = &episodes[index];

#ifdef __EMSCRIPTEN__
    /* Async fetch from S3 */
    char url[256];
    snprintf(url, sizeof(url), "%s/%s", ASSET_CDN, info->filename);
    ep->loading = true;
    emscripten_async_wget_data(url, ep, on_fetch_success, on_fetch_error);
#else
    /* Native: try local file */
    char path[256];
    snprintf(path, sizeof(path), "assets/%s", info->filename);
    size_t file_size = 0;
    uint8_t *file_data = load_file(path, &file_size);
    if (!file_data) {
        fprintf(stderr, "episode: %s not found locally, skipping\n", path);
        return;
    }
    episode_start_playback(ep, file_data, file_size);
#endif
}

void episode_skip(episode_state_t *ep) {
    if (!ep->active) return;

    if (ep->plm) {
        plm_destroy((plm_t *)ep->plm);
        ep->plm = NULL;
    }
    if (ep->texture_valid) {
        sg_destroy_view((sg_view){ ep->view_id });
        sg_destroy_image((sg_image){ ep->texture_id });
        ep->texture_valid = false;
    }
    ep->active = false;
    ep->current = -1;
    ep->pending = -1;
    ep->audio_write_pos = 0;
    ep->audio_read_pos = 0;
}

void episode_update(episode_state_t *ep, float dt) {
    if (!ep->active || !ep->plm) {
        return;
    }

    ep->fade_timer += dt;

    plm_t *plm = (plm_t *)ep->plm;
    plm_decode(plm, (double)dt);

    if (plm_has_ended(plm)) {
        episode_skip(ep);
    }
}

void episode_render(episode_state_t *ep, float screen_w, float screen_h) {
    if (!ep->active) return;

    float t = ep->fade_timer;
    const float INTRO_DURATION = 2.0f; /* flicker intro before video */
    bool in_intro = (t < INTRO_DURATION) || !ep->texture_valid;

    /* Size: ~35% of screen width, bottom-right with margin */
    float margin = 12.0f;
    float vid_w = (ep->video_width > 0) ? (float)ep->video_width : 640.0f;
    float vid_h = (ep->video_height > 0) ? (float)ep->video_height : 360.0f;
    float vid_aspect = vid_w / vid_h;
    float quad_w = screen_w * 0.35f;
    float quad_h = quad_w / vid_aspect;
    if (quad_h > screen_h * 0.4f) {
        quad_h = screen_h * 0.4f;
        quad_w = quad_h * vid_aspect;
    }

    float x0 = screen_w - quad_w - margin;
    float y0 = screen_h - quad_h - margin;
    float x1 = x0 + quad_w;
    float y1 = y0 + quad_h;
    float pad = 4.0f;

    /* Panel alpha — quick fade in over 0.3s */
    float alpha = (t < 0.3f) ? t / 0.3f : 1.0f;

    /* Dark panel background — darker during intro so text is readable */
    float bg_opacity = in_intro ? 0.92f : 0.85f;
    sgl_begin_quads();
    sgl_c4f(0.02f, 0.02f, 0.04f, bg_opacity * alpha);
    sgl_v2f(x0 - pad, y0 - pad - 14.0f);
    sgl_v2f(x1 + pad, y0 - pad - 14.0f);
    sgl_v2f(x1 + pad, y1 + pad);
    sgl_v2f(x0 - pad, y1 + pad);
    sgl_end();

    /* Gold border with flicker */
    float border_alpha = 0.6f * alpha;
    if (in_intro) {
        /* Flicker effect: rapid on/off with static bursts */
        float flicker = sinf(t * 31.0f) * sinf(t * 47.0f) * sinf(t * 13.0f);
        border_alpha *= (flicker > 0.0f) ? 1.0f : 0.15f;
    }
    float bw = 1.0f;
    sgl_begin_quads();
    sgl_c4f(0.78f, 0.63f, 0.19f, border_alpha);
    sgl_v2f(x0 - pad, y0 - pad - 14.0f);
    sgl_v2f(x1 + pad, y0 - pad - 14.0f);
    sgl_v2f(x1 + pad, y0 - pad - 14.0f + bw);
    sgl_v2f(x0 - pad, y0 - pad - 14.0f + bw);
    sgl_v2f(x0 - pad, y1 + pad - bw);
    sgl_v2f(x1 + pad, y1 + pad - bw);
    sgl_v2f(x1 + pad, y1 + pad);
    sgl_v2f(x0 - pad, y1 + pad);
    sgl_v2f(x0 - pad, y0 - pad - 14.0f);
    sgl_v2f(x0 - pad + bw, y0 - pad - 14.0f);
    sgl_v2f(x0 - pad + bw, y1 + pad);
    sgl_v2f(x0 - pad, y1 + pad);
    sgl_v2f(x1 + pad - bw, y0 - pad - 14.0f);
    sgl_v2f(x1 + pad, y0 - pad - 14.0f);
    sgl_v2f(x1 + pad, y1 + pad);
    sgl_v2f(x1 + pad - bw, y1 + pad);
    sgl_end();

    if (in_intro) {
        /* Flicker intro: "SIGNAL RECEIVED" then episode title */
        sdtx_canvas(screen_w, screen_h);
        sdtx_origin(0.0f, 0.0f);
        float cell = 8.0f;
        float cx = (x0 + x1) * 0.5f;
        float cy = (y0 + y1) * 0.5f;

        /* Static noise scanlines in the panel */
        float noise_alpha = 0.12f * alpha;
        if (sinf(t * 47.0f) > 0.3f) noise_alpha *= 2.5f;
        sgl_begin_quads();
        for (float sy = y0; sy < y1; sy += 4.0f) {
            float line_noise = sinf(sy * 0.7f + t * 120.0f) * 0.5f + 0.5f;
            if (line_noise > 0.6f) {
                sgl_c4f(0.78f, 0.63f, 0.19f, noise_alpha * line_noise);
                sgl_v2f(x0, sy);
                sgl_v2f(x1, sy);
                sgl_v2f(x1, sy + 1.0f);
                sgl_v2f(x0, sy + 1.0f);
            }
        }
        sgl_end();

        if (t < 1.2f) {
            /* Phase 1: "SIGNAL RECEIVED" flickering */
            const char *msg = "SIGNAL RECEIVED";
            float tw = (float)strlen(msg) * cell;
            float flicker = sinf(t * 23.0f) * sinf(t * 37.0f);
            uint8_t bright = (flicker > -0.2f) ? 200 : 40;
            sdtx_color3b(bright, (uint8_t)(bright * 0.8f), (uint8_t)(bright * 0.24f));
            sdtx_pos((cx - tw * 0.5f) / cell, (cy - 8.0f) / cell);
            sdtx_puts(msg);
        } else {
            /* Phase 2: episode title + "MILESTONE ACHIEVED" */
            const episode_info_t *info = episode_get_info(ep->current);
            if (info) {
                float tw = (float)strlen(info->title) * cell;
                sdtx_color3b(200, 160, 48);
                sdtx_pos((cx - tw * 0.5f) / cell, (cy - 16.0f) / cell);
                sdtx_puts(info->title);
            }
            const char *sub = "MILESTONE ACHIEVED";
            float sw = (float)strlen(sub) * cell;
            uint8_t sub_bright = (uint8_t)(100.0f + 40.0f * sinf(t * 5.0f));
            sdtx_color3b(sub_bright, sub_bright, sub_bright);
            sdtx_pos((cx - sw * 0.5f) / cell, (cy + 8.0f) / cell);
            sdtx_puts(sub);
        }
    } else {
        /* Video playback — blue-shifted desaturated look (signal ghost aesthetic) */
        if (ep->texture_valid) {
            /* Fade video in from intro, fade out near end */
            float vid_fade = 1.0f;
            float since_intro = t - INTRO_DURATION;
            if (since_intro < 0.8f) vid_fade = since_intro / 0.8f; /* fade in */
            plm_t *plm_check = (plm_t *)ep->plm;
            if (plm_check) {
                double remaining = plm_get_duration(plm_check) - plm_get_time(plm_check);
                if (remaining < 1.0) vid_fade *= (float)remaining; /* fade out */
            }
            if (vid_fade < 0.0f) vid_fade = 0.0f;

            sgl_enable_texture();
            sgl_texture((sg_view){ ep->view_id }, (sg_sampler){ ep->sampler_id });
            sgl_begin_quads();
            /* Tint: suppress red/green, boost blue — gives cold transmission feel */
            sgl_c4f(0.55f * vid_fade, 0.65f * vid_fade, 1.0f * vid_fade, alpha);
            sgl_v2f_t2f(x0, y0, 0.0f, 0.0f);
            sgl_v2f_t2f(x1, y0, 1.0f, 0.0f);
            sgl_v2f_t2f(x1, y1, 1.0f, 1.0f);
            sgl_v2f_t2f(x0, y1, 0.0f, 1.0f);
            sgl_end();
            sgl_disable_texture();

            /* Scanline overlay for retro transmission look */
            sgl_begin_quads();
            for (float sy = y0; sy < y1; sy += 3.0f) {
                sgl_c4f(0.0f, 0.0f, 0.0f, 0.12f);
                sgl_v2f(x0, sy);
                sgl_v2f(x1, sy);
                sgl_v2f(x1, sy + 1.0f);
                sgl_v2f(x0, sy + 1.0f);
            }
            sgl_end();
        }

        /* Title text above video */
        const episode_info_t *info = episode_get_info(ep->current);
        if (info) {
            sdtx_canvas(screen_w, screen_h);
            sdtx_origin(0.0f, 0.0f);
            sdtx_color3b(200, 160, 48);
            sdtx_pos(x0 / 8.0f, (y0 - pad - 12.0f) / 8.0f);
            sdtx_puts(info->title);
        }
    }
}

void episode_shutdown(episode_state_t *ep) {
    if (ep->plm) {
        plm_destroy((plm_t *)ep->plm);
        ep->plm = NULL;
    }
    if (ep->texture_valid) {
        sg_destroy_view((sg_view){ ep->view_id });
        sg_destroy_image((sg_image){ ep->texture_id });
        ep->texture_valid = false;
    }
    if (ep->sampler_id != 0) {
        sg_destroy_sampler((sg_sampler){ ep->sampler_id });
        ep->sampler_id = 0;
    }
}

bool episode_is_active(episode_state_t *ep) {
    return ep->active;
}

int episode_read_audio(episode_state_t *ep, float *buffer, int frames, int channels) {
    if (!ep->active) return 0;

    int samples_needed = frames * channels;
    int available = audio_buf_available(ep);
    int to_read = (available < samples_needed) ? available : samples_needed;

    if (channels == 2) {
        int pairs = to_read / 2;
        for (int i = 0; i < pairs * 2; i++) {
            buffer[i] += ep->audio_buffer[ep->audio_read_pos];
            ep->audio_read_pos = (ep->audio_read_pos + 1) % ep->audio_buffer_size;
        }
        return pairs;
    } else {
        int pairs = to_read / 2;
        for (int i = 0; i < pairs; i++) {
            float l = ep->audio_buffer[ep->audio_read_pos];
            ep->audio_read_pos = (ep->audio_read_pos + 1) % ep->audio_buffer_size;
            float r = ep->audio_buffer[ep->audio_read_pos];
            ep->audio_read_pos = (ep->audio_read_pos + 1) % ep->audio_buffer_size;
            buffer[i] += (l + r) * 0.5f;
        }
        return pairs;
    }
}
