#ifndef EPISODE_H
#define EPISODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "episode_lifecycle.h"

typedef struct {
    const char *filename;
    const char *title;
} episode_info_t;

typedef struct {
    bool active;        /* decoder is running, including pre-commit startup */
    bool loading;       /* fetch/decode is waiting for its first usable frame */
    episode_lifecycle_t lifecycle;
    float fade_timer;
    float fade_duration;
    bool loaded;
    episode_failure_t deferred_failure;
    int deferred_failure_index;

    /* pl_mpeg handle (void* to avoid exposing pl_mpeg.h in header) */
    void *plm;

    /* Decoded video frame texture (stored as uint32_t IDs to avoid sokol header dep) */
    uint32_t texture_id;   /* sg_image.id */
    uint32_t view_id;      /* sg_view.id for texture sampling */
    uint32_t sampler_id;   /* sg_sampler.id */
    int video_width;
    int video_height;
    bool texture_valid;

    /* Latest decoded RGBA frame waiting to be uploaded. plm_decode() can emit
     * multiple video frames in a single tick (catch-up after a stall), and
     * sim_step can run several times per render frame, but sokol forbids more
     * than one sg_update_image per image per frame. Decode stashes the most
     * recent frame here; episode_upload_frame uploads it once per render
     * frame, between the sim loop and render_frame. */
    uint8_t *pending_rgba;
    int pending_w;
    int pending_h;
    uint8_t *rgba_buffer;
    size_t rgba_buffer_size;

    /* Trigger tracking */
    uint8_t stations_visited;  /* bitmask of original stations docked at (0-2) */

    /* Audio ring buffer for decoded video audio */
    float audio_buffer[44100 * 2 * 2]; /* ~2 seconds stereo at 44100 Hz */
    int audio_write_pos;
    int audio_read_pos;
    int audio_buffer_size;
} episode_state_t;

void episode_init(episode_state_t *ep);
void episode_load(episode_state_t *ep);
void episode_save(episode_state_t *ep);
void episode_trigger(episode_state_t *ep, int index);
/* Explicit skip is accepted only after the first usable video frame. */
void episode_skip(episode_state_t *ep);
/* Cancels active/pending work and invalidates callbacks; preserves watched. */
void episode_reset(episode_state_t *ep);
void episode_update(episode_state_t *ep, float dt);
void episode_upload_frame(episode_state_t *ep);
void episode_render(episode_state_t *ep, float screen_w, float screen_h);
void episode_shutdown(episode_state_t *ep);
bool episode_is_active(episode_state_t *ep);
bool episode_was_watched(const episode_state_t *ep, int index);
/* In-memory mutation; call episode_save separately when persistence is wanted. */
void episode_set_watched(episode_state_t *ep, int index, bool watched);
void episode_clear_watched(episode_state_t *ep);

/* Called by audio_generate_stream to mix episode audio into output */
int episode_read_audio(episode_state_t *ep, float *buffer, int frames, int channels);

/* Episode info table */
const episode_info_t *episode_get_info(int index);

#endif
