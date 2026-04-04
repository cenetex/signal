#ifndef EPISODE_H
#define EPISODE_H

#include <stdbool.h>
#include <stdint.h>

#define EPISODE_COUNT 10

typedef struct {
    const char *filename;
    const char *title;
} episode_info_t;

typedef struct {
    bool active;
    bool loading;       /* async download in progress (Emscripten) */
    bool watched[EPISODE_COUNT];
    int current;
    int pending;        /* episode index queued for download, -1 = none */
    float fade_timer;
    float fade_duration;
    bool loaded;

    /* pl_mpeg handle (void* to avoid exposing pl_mpeg.h in header) */
    void *plm;

    /* Decoded video frame texture (stored as uint32_t IDs to avoid sokol header dep) */
    uint32_t texture_id;   /* sg_image.id */
    uint32_t view_id;      /* sg_view.id for texture sampling */
    uint32_t sampler_id;   /* sg_sampler.id */
    int video_width;
    int video_height;
    bool texture_valid;

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
void episode_skip(episode_state_t *ep);
void episode_update(episode_state_t *ep, float dt);
void episode_render(episode_state_t *ep, float screen_w, float screen_h);
void episode_shutdown(episode_state_t *ep);
bool episode_is_active(episode_state_t *ep);

/* Called by audio_generate_stream to mix episode audio into output */
int episode_read_audio(episode_state_t *ep, float *buffer, int frames, int channels);

/* Episode info table */
const episode_info_t *episode_get_info(int index);

#endif
