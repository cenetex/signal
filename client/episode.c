/*
 * episode.c — MPEG1 video episode playback for Signal Space Miner.
 * Uses pl_mpeg for decoding, sokol_gfx for texture upload, sokol_gl for rendering.
 *
 * Emscripten fetches episode assets through the same-origin Worker so missing
 * or not-yet-published videos fail as normal asset misses instead of CORS
 * errors. Native uses local file fallback (assets/ directory for development).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "pl_mpeg.h"
#include "episode.h"
#include "episode_media.h"
#include "render.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
static episode_state_t *episode_smoke_state;
#endif

#ifndef SIGNAL_HAS_WEB_EPISODE_ASSETS
#define SIGNAL_HAS_WEB_EPISODE_ASSETS 1
#endif

static bool episode_assets_available(void) {
#ifdef __EMSCRIPTEN__
    return SIGNAL_HAS_WEB_EPISODE_ASSETS != 0;
#else
    return true;
#endif
}

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

static void episode_clear_pending_frame(episode_state_t *ep) {
    ep->pending_rgba = NULL;
    ep->pending_w = 0;
    ep->pending_h = 0;
}

static void episode_destroy_texture(episode_state_t *ep) {
    if (ep->texture_valid) {
        sg_destroy_view((sg_view){ ep->view_id });
        sg_destroy_image((sg_image){ ep->texture_id });
    }
    ep->texture_id = 0;
    ep->view_id = 0;
    ep->video_width = 0;
    ep->video_height = 0;
    ep->texture_valid = false;
    episode_clear_pending_frame(ep);
}

static void episode_stop_media(episode_state_t *ep) {
    if (ep->plm) {
        plm_destroy((plm_t *)ep->plm);
        ep->plm = NULL;
    }
    episode_destroy_texture(ep);
    ep->active = false;
    ep->loading = false;
    ep->deferred_failure = EPISODE_FAILURE_NONE;
    ep->deferred_failure_index = -1;
    ep->audio_write_pos = 0;
    ep->audio_read_pos = 0;
}

static bool episode_fail_attempt(episode_state_t *ep, int index,
                                 episode_attempt_token_t token,
                                 episode_failure_t failure) {
    if (!episode_lifecycle_fail(&ep->lifecycle, index, token, failure))
        return false;
    episode_stop_media(ep);
    return true;
}

/*
 * Video callbacks run inside plm_decode(). Transition a pending attempt to
 * failed immediately, but destroy the decoder only after plm_decode() returns;
 * destroying it reentrantly from its own callback is unsafe.
 *
 * plm_decode() may emit several frames in one call. Clearing pending here is
 * what prevents a later callback in that same call from committing watched
 * state after an earlier allocation or texture failure.
 */
static void episode_defer_failure(episode_state_t *ep,
                                  episode_failure_t failure) {
    if (!ep || failure == EPISODE_FAILURE_NONE ||
        ep->deferred_failure != EPISODE_FAILURE_NONE) {
        return;
    }

    int index = ep->lifecycle.pending;
    episode_attempt_token_t token = ep->lifecycle.pending_token;
    if (index >= 0) {
        if (!episode_lifecycle_fail(&ep->lifecycle, index, token, failure))
            return;
    } else if (ep->lifecycle.current < 0) {
        return;
    }

    ep->deferred_failure = failure;
    ep->deferred_failure_index = index;
}

/* --- pl_mpeg callbacks --- */

static void on_video_frame(plm_t *plm, plm_frame_t *frame, void *user) {
    (void)plm;
    episode_state_t *ep = (episode_state_t *)user;
    if (ep->deferred_failure != EPISODE_FAILURE_NONE) return;

    int w = frame->width;
    int h = frame->height;

    if (w <= 0 || h <= 0 ||
        (size_t)w > SIZE_MAX / (size_t)h / 4u) {
        episode_defer_failure(ep, EPISODE_FAILURE_DECODER);
        return;
    }
    size_t rgba_size = (size_t)w * (size_t)h * 4u;
    if (rgba_size > ep->rgba_buffer_size) {
        uint8_t *next = (uint8_t *)realloc(ep->rgba_buffer, rgba_size);
        if (!next) {
            episode_defer_failure(ep, EPISODE_FAILURE_ALLOCATION);
            return;
        }
        ep->rgba_buffer = next;
        ep->rgba_buffer_size = rgba_size;
    }

    plm_frame_to_rgba(frame, ep->rgba_buffer, w * 4);

    if (!ep->texture_valid || ep->video_width != w || ep->video_height != h) {
        sg_sampler new_sampler = { ep->sampler_id };
        bool made_sampler = false;
        if (new_sampler.id == 0) {
            new_sampler = sg_make_sampler(&(sg_sampler_desc){
                .min_filter = SG_FILTER_LINEAR,
                .mag_filter = SG_FILTER_LINEAR,
            });
            made_sampler = true;
        }
        if (new_sampler.id == 0 ||
            sg_query_sampler_state(new_sampler) != SG_RESOURCESTATE_VALID) {
            if (new_sampler.id != 0) sg_destroy_sampler(new_sampler);
            if (!made_sampler) ep->sampler_id = 0;
            episode_defer_failure(ep, EPISODE_FAILURE_TEXTURE);
            return;
        }

        sg_image img = sg_make_image(&(sg_image_desc){
            .width = w,
            .height = h,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .usage.stream_update = true,
        });
        if (img.id == 0 ||
            sg_query_image_state(img) != SG_RESOURCESTATE_VALID) {
            if (img.id != 0) sg_destroy_image(img);
            if (made_sampler) sg_destroy_sampler(new_sampler);
            episode_defer_failure(ep, EPISODE_FAILURE_TEXTURE);
            return;
        }

        sg_view view = sg_make_view(&(sg_view_desc){
            .texture.image = img,
        });
        if (view.id == 0 ||
            sg_query_view_state(view) != SG_RESOURCESTATE_VALID) {
            if (view.id != 0) sg_destroy_view(view);
            sg_destroy_image(img);
            if (made_sampler) sg_destroy_sampler(new_sampler);
            episode_defer_failure(ep, EPISODE_FAILURE_TEXTURE);
            return;
        }

        episode_destroy_texture(ep);
        ep->texture_id = img.id;
        ep->view_id = view.id;
        ep->sampler_id = new_sampler.id;
        ep->video_width = w;
        ep->video_height = h;
        ep->texture_valid = true;
    }

    /* Stash the reusable RGBA buffer as the pending upload. If multiple
     * frames decode before render, the next decode overwrites this buffer
     * and the upload path still sees only the latest frame. */
    ep->pending_rgba = ep->rgba_buffer;
    ep->pending_w = w;
    ep->pending_h = h;

    if (ep->lifecycle.pending >= 0) {
        int index = ep->lifecycle.pending;
        episode_attempt_token_t token = ep->lifecycle.pending_token;
        if (episode_lifecycle_start(&ep->lifecycle, index, token)) {
            ep->loading = false;
            episode_save(ep);
        }
    }
}

static void on_audio_frame(plm_t *plm, plm_samples_t *samples, void *user) {
    (void)plm;
    episode_state_t *ep = (episode_state_t *)user;
    if (ep->deferred_failure != EPISODE_FAILURE_NONE) return;
    int count = samples->count * 2; /* stereo interleaved */
    audio_buf_write(ep, samples->interleaved, count);
}

/* --- Start playback once data is in memory --- */

static void episode_start_playback(episode_state_t *ep, int index,
                                   episode_attempt_token_t token,
                                   uint8_t *data, size_t size) {
    if (!episode_lifecycle_matches(&ep->lifecycle, index, token)) {
        free(data);
        return;
    }

    episode_failure_t failure = EPISODE_FAILURE_DECODER;
    plm_t *plm = (plm_t *)episode_media_create_decoder(
        data, size, &failure);
    if (!plm) {
        fprintf(stderr, "episode: invalid or unsupported video stream\n");
        (void)episode_fail_attempt(ep, index, token, failure);
        return;
    }

    plm_set_video_decode_callback(plm, on_video_frame, ep);
    plm_set_audio_decode_callback(plm, on_audio_frame, ep);
    plm_set_audio_enabled(plm, 1);
    plm_set_loop(plm, 0);
    plm_set_audio_lead_time(plm, 0.1);

    ep->plm = plm;
    ep->active = true;
    ep->loading = true;
    ep->fade_timer = 0.0f;
    ep->audio_write_pos = 0;
    ep->audio_read_pos = 0;
}

/* --- Async fetch (Emscripten) --- */

#ifdef __EMSCRIPTEN__
typedef struct {
    episode_state_t *ep;
    int index;
    episode_attempt_token_t token;
} episode_fetch_context_t;

static void on_fetch_success(void *user, void *data, int size) {
    episode_fetch_context_t *ctx = (episode_fetch_context_t *)user;
    episode_state_t *ep = ctx->ep;
    if (!episode_lifecycle_matches(&ep->lifecycle, ctx->index, ctx->token)) {
        free(ctx);
        return;
    }
    if (size <= 0) {
        (void)episode_fail_attempt(ep, ctx->index, ctx->token,
                                   EPISODE_FAILURE_FETCH);
        free(ctx);
        return;
    }
    uint8_t *copy = (uint8_t *)malloc((size_t)size);
    if (!copy) {
        fprintf(stderr, "episode: out of memory for %d bytes\n", size);
        (void)episode_fail_attempt(ep, ctx->index, ctx->token,
                                   EPISODE_FAILURE_ALLOCATION);
        free(ctx);
        return;
    }
    memcpy(copy, data, (size_t)size);
    episode_start_playback(ep, ctx->index, ctx->token, copy, (size_t)size);
    free(ctx);
}

static void on_fetch_error(void *user) {
    episode_fetch_context_t *ctx = (episode_fetch_context_t *)user;
    episode_state_t *ep = ctx->ep;
    if (episode_lifecycle_matches(&ep->lifecycle,
                                  ctx->index, ctx->token)) {
        fprintf(stderr, "episode: fetch failed for ep %d (no internet?)\n",
                ctx->index);
        (void)episode_fail_attempt(ep, ctx->index, ctx->token,
                                   EPISODE_FAILURE_FETCH);
    }
    free(ctx);
}
#endif

/* --- Public API --- */

void episode_init(episode_state_t *ep) {
    memset(ep, 0, sizeof(*ep));
    episode_lifecycle_init(&ep->lifecycle);
    ep->deferred_failure_index = -1;
    ep->fade_duration = 0.5f;
    ep->audio_buffer_size = (int)(sizeof(ep->audio_buffer) / sizeof(ep->audio_buffer[0]));
#ifdef __EMSCRIPTEN__
    episode_smoke_state = ep;
#endif
}

void episode_load(episode_state_t *ep) {
    if (ep->loaded) return;
    ep->loaded = true;
#ifdef __EMSCRIPTEN__
    int flags = emscripten_run_script_int(
        "(function(){var s=localStorage.getItem('signal_episodes');"
        "if(!s)return 0;return parseInt(s,10)||0;})()");
    for (int i = 0; i < EPISODE_COUNT; i++)
        ep->lifecycle.watched[i] = (flags & (1 << i)) != 0;
#endif
}

void episode_save(episode_state_t *ep) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    for (int i = 0; i < EPISODE_COUNT; i++)
        if (ep->lifecycle.watched[i]) flags |= (1 << i);
    char js[80];
    snprintf(js, sizeof(js),
        "localStorage.setItem('signal_episodes','%d')", flags);
    emscripten_run_script(js);
#else
    (void)ep;
#endif
}

static void episode_trigger_internal(episode_state_t *ep, int index,
                                     bool allow_unpublished_asset) {
    if (index < 0 || index >= EPISODE_COUNT) return;
    if (!allow_unpublished_asset && !episode_assets_available()) return;
    episode_attempt_token_t token = 0;
    if (!episode_lifecycle_begin(&ep->lifecycle, index, &token)) return;
    ep->loading = true;
    ep->deferred_failure = EPISODE_FAILURE_NONE;
    ep->deferred_failure_index = -1;

    const episode_info_t *info = &episodes[index];

#ifdef __EMSCRIPTEN__
    /* Async fetch from the same-origin asset Worker */
    char url[256];
    snprintf(url, sizeof(url), "/%s", info->filename);
    episode_fetch_context_t *ctx =
        (episode_fetch_context_t *)malloc(sizeof(*ctx));
    if (!ctx) {
        (void)episode_fail_attempt(ep, index, token,
                                   EPISODE_FAILURE_ALLOCATION);
        return;
    }
    ctx->ep = ep;
    ctx->index = index;
    ctx->token = token;
    emscripten_async_wget_data(url, ctx, on_fetch_success, on_fetch_error);
#else
    /* Native: try local file */
    char path[256];
    snprintf(path, sizeof(path), "assets/%s", info->filename);
    size_t file_size = 0;
    episode_failure_t failure = EPISODE_FAILURE_FILE_READ;
    uint8_t *file_data =
        episode_media_read_file(path, &file_size, &failure);
    if (!file_data) {
        if (failure == EPISODE_FAILURE_ALLOCATION) {
            fprintf(stderr, "episode: out of memory loading %s\n", path);
        } else {
            fprintf(stderr, "episode: %s not found locally, skipping\n",
                    path);
        }
        (void)episode_fail_attempt(ep, index, token, failure);
        return;
    }
    episode_start_playback(ep, index, token, file_data, file_size);
#endif
}

void episode_trigger(episode_state_t *ep, int index) {
    episode_trigger_internal(ep, index, false);
}

static bool episode_stop_started(episode_state_t *ep) {
    if (!episode_lifecycle_stop_started(&ep->lifecycle)) return false;
    episode_stop_media(ep);
    return true;
}

void episode_skip(episode_state_t *ep) {
    (void)episode_stop_started(ep);
}

void episode_reset(episode_state_t *ep) {
    episode_lifecycle_reset(&ep->lifecycle);
    episode_stop_media(ep);
}

void episode_update(episode_state_t *ep, float dt) {
    if (!ep->active || !ep->plm) {
        return;
    }

    ep->fade_timer += dt;

    plm_decode((plm_t *)ep->plm, (double)dt);
    if (ep->deferred_failure != EPISODE_FAILURE_NONE) {
        int index = ep->deferred_failure_index;
        if (index >= 0) {
            /*
             * episode_defer_failure already made this attempt terminal so a
             * later callback in the same decode call could not start it.
             * Teardown is the only operation that had to wait until now.
             */
            episode_stop_media(ep);
        } else {
            fprintf(stderr, "episode: playback stopped after media failure\n");
            (void)episode_stop_started(ep);
        }
    }
}

void episode_upload_frame(episode_state_t *ep) {
    if (!ep->active || !ep->plm) return;

    if (ep->pending_rgba && ep->texture_valid &&
        ep->pending_w == ep->video_width && ep->pending_h == ep->video_height) {
        size_t rgba_size =
            (size_t)ep->pending_w * (size_t)ep->pending_h * 4u;
        sg_update_image((sg_image){ ep->texture_id }, &(sg_image_data){
            .mip_levels[0] = { .ptr = ep->pending_rgba, .size = rgba_size },
        });
    }
    episode_clear_pending_frame(ep);

    if (plm_has_ended((plm_t *)ep->plm)) {
        if (ep->lifecycle.current >= 0) {
            (void)episode_stop_started(ep);
        } else {
            int index = ep->lifecycle.pending;
            episode_attempt_token_t token = ep->lifecycle.pending_token;
            (void)episode_fail_attempt(ep, index, token,
                                       EPISODE_FAILURE_DECODER);
        }
    }
}

void episode_render(episode_state_t *ep, float screen_w, float screen_h) {
    if (!ep->active) return;
    int display_index = ep->lifecycle.current >= 0
        ? ep->lifecycle.current : ep->lifecycle.pending;

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
            const episode_info_t *info = episode_get_info(display_index);
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

            /* Tint: suppress red/green, boost blue — gives cold transmission feel */
            draw_texture_rect(ep->view_id, ep->sampler_id,
                              x0, y0, x1, y1,
                              0.55f * vid_fade, 0.65f * vid_fade,
                              1.0f * vid_fade, alpha);

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
        const episode_info_t *info = episode_get_info(display_index);
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
    episode_reset(ep);
    if (ep->sampler_id != 0) {
        sg_destroy_sampler((sg_sampler){ ep->sampler_id });
        ep->sampler_id = 0;
    }
    free(ep->rgba_buffer);
    ep->rgba_buffer = NULL;
    ep->rgba_buffer_size = 0;
    ep->pending_rgba = NULL;
}

bool episode_is_active(episode_state_t *ep) {
    return ep->active;
}

bool episode_was_watched(const episode_state_t *ep, int index) {
    return ep && index >= 0 && index < EPISODE_COUNT &&
           ep->lifecycle.watched[index];
}

void episode_set_watched(episode_state_t *ep, int index, bool watched) {
    if (!ep || index < 0 || index >= EPISODE_COUNT) return;
    ep->lifecycle.watched[index] = watched;
}

void episode_clear_watched(episode_state_t *ep) {
    if (!ep) return;
    memset(ep->lifecycle.watched, 0, sizeof(ep->lifecycle.watched));
}

#ifdef __EMSCRIPTEN__
enum {
    EPISODE_SMOKE_WATCHED = 1 << 0,
    EPISODE_SMOKE_PENDING = 1 << 1,
    EPISODE_SMOKE_STARTED = 1 << 2,
    EPISODE_SMOKE_ACTIVE = 1 << 3,
    EPISODE_SMOKE_LOADING = 1 << 4,
    EPISODE_SMOKE_FAILURE_SHIFT = 8,
};

static bool episode_smoke_hooks_enabled(void) {
    return emscripten_run_script_int(
        "(new URLSearchParams(location.search).get('smoke')==='1')") != 0;
}

/*
 * Isolate one episode for the browser retry test. Marking every other story
 * watched prevents normal gameplay milestones from racing the injected fetch.
 */
EMSCRIPTEN_KEEPALIVE
int signal_smoke_episode_prepare(int index) {
    if (!episode_smoke_hooks_enabled() || !episode_smoke_state ||
        index < 0 || index >= EPISODE_COUNT) {
        return 0;
    }
    episode_reset(episode_smoke_state);
    for (int i = 0; i < EPISODE_COUNT; i++)
        episode_smoke_state->lifecycle.watched[i] = i != index;
    episode_save(episode_smoke_state);
    return 1;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_episode_trigger(int index) {
    if (!episode_smoke_hooks_enabled() || !episode_smoke_state ||
        index < 0 || index >= EPISODE_COUNT) {
        return 0;
    }
    episode_trigger_internal(episode_smoke_state, index, true);
    return episode_smoke_state->lifecycle.watched[index] ||
           episode_smoke_state->lifecycle.pending == index ||
           episode_smoke_state->lifecycle.current == index;
}

EMSCRIPTEN_KEEPALIVE
int signal_smoke_episode_state(int index) {
    if (!episode_smoke_hooks_enabled() || !episode_smoke_state ||
        index < 0 || index >= EPISODE_COUNT) {
        return 0;
    }
    int state = 0;
    if (episode_smoke_state->lifecycle.watched[index])
        state |= EPISODE_SMOKE_WATCHED;
    if (episode_smoke_state->lifecycle.pending == index)
        state |= EPISODE_SMOKE_PENDING;
    if (episode_smoke_state->lifecycle.current == index)
        state |= EPISODE_SMOKE_STARTED;
    if (episode_smoke_state->active)
        state |= EPISODE_SMOKE_ACTIVE;
    if (episode_smoke_state->loading)
        state |= EPISODE_SMOKE_LOADING;
    state |= (int)episode_smoke_state->lifecycle.last_failure
             << EPISODE_SMOKE_FAILURE_SHIFT;
    return state;
}
#endif

int episode_read_audio(episode_state_t *ep, float *buffer, int frames, int channels) {
    if (!ep->active) return 0;

    /* Audio fade: 0.5s in, 0.5s out at end */
    float audio_vol = 1.0f;
    if (ep->fade_timer < 0.5f) audio_vol = ep->fade_timer / 0.5f;
    if (ep->plm) {
        double remaining = plm_get_duration((plm_t *)ep->plm) - plm_get_time((plm_t *)ep->plm);
        if (remaining < 0.5) audio_vol *= (float)(remaining / 0.5);
    }
    if (audio_vol < 0.0f) audio_vol = 0.0f;

    int samples_needed = frames * channels;
    int available = audio_buf_available(ep);
    int to_read = (available < samples_needed) ? available : samples_needed;

    if (channels == 2) {
        int pairs = to_read / 2;
        for (int i = 0; i < pairs * 2; i++) {
            buffer[i] += ep->audio_buffer[ep->audio_read_pos] * audio_vol;
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
            buffer[i] += (l + r) * 0.5f * audio_vol;
        }
        return pairs;
    }
}
