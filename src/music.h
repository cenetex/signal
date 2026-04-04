#ifndef MUSIC_H
#define MUSIC_H

#include <stdbool.h>

#define MUSIC_TRACK_COUNT 12

typedef struct {
    const char *filename;
    const char *title;
} music_track_info_t;

typedef struct {
    bool playing;
    bool paused;
    bool loading;       /* async download in progress (Emscripten) */
    int current_track;
    float track_display_timer; /* seconds since track changed, for fade */
    float volume;       /* 0.0 - 1.0 */
    float fade_volume;  /* for fade in/out transitions */
    float fade_target;
    float fade_speed;

    /* minimp3 decoder state (opaque) */
    void *decoder;
    unsigned char *file_data;
    int file_size;
    int file_offset;

    /* Audio ring buffer for decoded music */
    float audio_buffer[44100 * 2 * 4]; /* ~4 seconds stereo at 44100 Hz */
    int audio_write_pos;
    int audio_read_pos;
    int audio_buffer_size;
    int sample_rate;
    int channels;
} music_state_t;

void music_init(music_state_t *m);
void music_play(music_state_t *m, int track);
void music_stop(music_state_t *m);
void music_pause(music_state_t *m);
void music_resume(music_state_t *m);
void music_set_volume(music_state_t *m, float vol);
void music_fade_to(music_state_t *m, float vol, float seconds);
void music_update(music_state_t *m, float dt);
void music_next_track(music_state_t *m);
void music_shutdown(music_state_t *m);

/* Called by audio_generate_stream to mix music into output */
int music_read_audio(music_state_t *m, float *buffer, int frames, int channels);

const music_track_info_t *music_get_info(int index);

#endif
