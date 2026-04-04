/*
 * music.c — MP3 background music playback for Signal Space Miner.
 * Uses minimp3 for decoding, mixes into sokol_audio via the audio system.
 *
 * All platforms fetch from S3 CDN. Emscripten uses async XHR, native uses
 * local file fallback.
 */

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "music.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define ASSET_CDN "https://signal-ratimics-assets.s3.amazonaws.com"

/* S3 uses underscores — these must match the bucket keys */
static const music_track_info_t tracks[MUSIC_TRACK_COUNT] = {
    { "music/belt_drifters.mp3",          "Belt Drifters" },
    { "music/belt_drifters_2.mp3",        "Belt Drifters II" },
    { "music/belt_drifters_3.mp3",        "Belt Drifters III" },
    { "music/belt_drifters_4.mp3",        "Belt Drifters IV" },
    { "music/echoes.mp3",                 "Echoes in the Belt" },
    { "music/echoes_2.mp3",               "Echoes in the Belt II" },
    { "music/echoes_3.mp3",               "Echoes in the Belt III" },
    { "music/lofi_drift_jam.mp3",         "Lo-Fi Drift Jam" },
    { "music/vector_miners_epoch.mp3",    "Vector Miners Epoch" },
    { "music/vector_miners_epoch_2.mp3",  "Vector Miners Epoch II" },
    { "music/vector_economy.mp3",         "Vector Space Economy" },
    { "music/vector_economy_2.mp3",       "Vector Space Economy II" },
};

const music_track_info_t *music_get_info(int index) {
    if (index < 0 || index >= MUSIC_TRACK_COUNT) return NULL;
    return &tracks[index];
}

/* --- Audio ring buffer helpers --- */

static int audio_buf_available(music_state_t *m) {
    int avail = m->audio_write_pos - m->audio_read_pos;
    if (avail < 0) avail += m->audio_buffer_size;
    return avail;
}

static int audio_buf_free(music_state_t *m) {
    return m->audio_buffer_size - 1 - audio_buf_available(m);
}

static void audio_buf_write(music_state_t *m, const float *samples, int count) {
    for (int i = 0; i < count; i++) {
        m->audio_buffer[m->audio_write_pos] = samples[i];
        m->audio_write_pos = (m->audio_write_pos + 1) % m->audio_buffer_size;
    }
}

/* --- MP3 decode chunk --- */

static void decode_chunk(music_state_t *m) {
    if (!m->decoder || !m->file_data) return;

    mp3dec_t *dec = (mp3dec_t *)m->decoder;
    mp3dec_frame_info_t info;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    int safety = 32;
    while (safety-- > 0 && m->file_offset < m->file_size) {
        int remaining = m->file_size - m->file_offset;
        int samples = mp3dec_decode_frame(dec,
            m->file_data + m->file_offset, remaining,
            pcm, &info);

        if (info.frame_bytes <= 0) break;
        m->file_offset += info.frame_bytes;

        if (samples <= 0) continue;

        if (m->sample_rate == 0) {
            m->sample_rate = info.hz;
            m->channels = info.channels;
        }

        int total_samples = samples * info.channels;
        if (audio_buf_free(m) < total_samples + 1024) break;

        float fbuf[MINIMP3_MAX_SAMPLES_PER_FRAME];
        for (int i = 0; i < total_samples; i++) {
            fbuf[i] = (float)pcm[i] / 32768.0f;
        }
        audio_buf_write(m, fbuf, total_samples);
    }

    /* Loop: restart at end of file */
    if (m->file_offset >= m->file_size && m->playing) {
        m->file_offset = 0;
        mp3dec_init(dec);
    }
}

/* --- Start playback once data is in memory --- */

static void music_start_playback(music_state_t *m, unsigned char *data, int size) {
    mp3dec_t *dec = (mp3dec_t *)malloc(sizeof(mp3dec_t));
    if (!dec) { free(data); return; }
    mp3dec_init(dec);

    m->decoder = dec;
    m->file_data = data;
    m->file_size = size;
    m->file_offset = 0;
    m->sample_rate = 0;
    m->channels = 0;
    m->playing = true;
    m->paused = false;
    m->loading = false;
    m->track_display_timer = 0.0f;
    m->fade_volume = 0.0f;       /* start silent */
    m->fade_target = 1.0f;       /* fade in */
    m->fade_speed = 2.0f;        /* 0.5s fade */
    m->audio_write_pos = 0;
    m->audio_read_pos = 0;

    /* Pre-fill buffer */
    decode_chunk(m);
    decode_chunk(m);
}

/* --- Async fetch (Emscripten) --- */

#ifdef __EMSCRIPTEN__
static void on_music_fetch_success(void *user, void *data, int size) {
    music_state_t *m = (music_state_t *)user;
    unsigned char *copy = (unsigned char *)malloc((size_t)size);
    if (!copy) {
        fprintf(stderr, "music: out of memory for %d bytes\n", size);
        m->loading = false;
        return;
    }
    memcpy(copy, data, (size_t)size);
    music_start_playback(m, copy, size);
}

static void on_music_fetch_error(void *user) {
    music_state_t *m = (music_state_t *)user;
    fprintf(stderr, "music: fetch failed for track %d (no internet?)\n", m->current_track);
    m->loading = false;
    /* Try next track after a failure */
    m->current_track = (m->current_track + 1) % MUSIC_TRACK_COUNT;
}
#endif

/* --- Native file loading --- */

#ifndef __EMSCRIPTEN__
static unsigned char *load_file(const char *path, int *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *data = (unsigned char *)malloc((size_t)sz);
    if (!data) { fclose(f); return NULL; }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if ((long)rd != sz) { free(data); return NULL; }
    *out_size = (int)sz;
    return data;
}
#endif

/* --- Public API --- */

void music_init(music_state_t *m) {
    memset(m, 0, sizeof(*m));
    m->current_track = -1;
    m->pending_track = -1;
    m->volume = 0.35f;
    m->fade_volume = 1.0f;
    m->fade_target = 1.0f;
    m->audio_buffer_size = (int)(sizeof(m->audio_buffer) / sizeof(m->audio_buffer[0]));
}

static void music_play_immediate(music_state_t *m, int track);

void music_play(music_state_t *m, int track) {
    if (track < 0 || track >= MUSIC_TRACK_COUNT) return;

    if (m->playing && !m->paused && m->fade_volume > 0.1f) {
        /* Crossfade: fade out current, queue next */
        m->pending_track = track;
        m->fade_target = 0.0f;
        m->fade_speed = 2.0f; /* 0.5s fade out */
        return;
    }

    music_stop(m);
    music_play_immediate(m, track);
}

static void music_play_immediate(music_state_t *m, int track) {
    m->current_track = track;

    const music_track_info_t *info = &tracks[track];

#ifdef __EMSCRIPTEN__
    char url[256];
    snprintf(url, sizeof(url), "%s/%s", ASSET_CDN, info->filename);
    m->loading = true;
    emscripten_async_wget_data(url, m, on_music_fetch_success, on_music_fetch_error);
#else
    /* Native: try local file with underscore naming (matching S3) */
    char path[256];
    snprintf(path, sizeof(path), "assets/%s", info->filename);
    int file_size = 0;
    unsigned char *data = load_file(path, &file_size);
    if (!data) {
        fprintf(stderr, "music: %s not found locally, skipping\n", path);
        return;
    }
    music_start_playback(m, data, file_size);
#endif
}

void music_stop(music_state_t *m) {
    m->playing = false;
    m->paused = false;
    m->loading = false;
    if (m->decoder) {
        free(m->decoder);
        m->decoder = NULL;
    }
    if (m->file_data) {
        free(m->file_data);
        m->file_data = NULL;
    }
    m->audio_write_pos = 0;
    m->audio_read_pos = 0;
}

void music_pause(music_state_t *m) {
    m->paused = true;
}

void music_resume(music_state_t *m) {
    m->paused = false;
}

void music_set_volume(music_state_t *m, float vol) {
    m->volume = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
}

void music_fade_to(music_state_t *m, float vol, float seconds) {
    m->fade_target = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
    m->fade_speed = seconds > 0.0f ? 1.0f / seconds : 100.0f;
}

void music_update(music_state_t *m, float dt) {
    if (!m->playing || m->paused) return;

    m->track_display_timer += dt;

    /* Fade volume */
    if (m->fade_volume < m->fade_target) {
        m->fade_volume += m->fade_speed * dt;
        if (m->fade_volume > m->fade_target) m->fade_volume = m->fade_target;
    } else if (m->fade_volume > m->fade_target) {
        m->fade_volume -= m->fade_speed * dt;
        if (m->fade_volume < m->fade_target) m->fade_volume = m->fade_target;
    }

    /* Crossfade: when fade-out completes, start pending track */
    if (m->pending_track >= 0 && m->fade_volume <= 0.01f) {
        int next = m->pending_track;
        m->pending_track = -1;
        music_stop(m);
        music_play_immediate(m, next);
        return;
    }

    /* Keep ring buffer fed */
    if (audio_buf_available(m) < m->audio_buffer_size / 2) {
        decode_chunk(m);
    }
}

void music_next_track(music_state_t *m) {
    int next = (m->current_track + 1) % MUSIC_TRACK_COUNT;
    music_play(m, next);
}

void music_shutdown(music_state_t *m) {
    music_stop(m);
}

int music_read_audio(music_state_t *m, float *buffer, int frames, int channels) {
    if (!m->playing || m->paused) return 0;

    float vol = m->volume * m->fade_volume;
    int available = audio_buf_available(m);
    int src_channels = m->channels > 0 ? m->channels : 2;
    int src_samples_per_frame = src_channels;
    int frames_available = available / src_samples_per_frame;
    int to_read = (frames_available < frames) ? frames_available : frames;

    for (int i = 0; i < to_read; i++) {
        float l = m->audio_buffer[m->audio_read_pos] * vol;
        m->audio_read_pos = (m->audio_read_pos + 1) % m->audio_buffer_size;

        float r = l;
        if (src_channels >= 2) {
            r = m->audio_buffer[m->audio_read_pos] * vol;
            m->audio_read_pos = (m->audio_read_pos + 1) % m->audio_buffer_size;
        }

        if (channels == 1) {
            buffer[i] += (l + r) * 0.5f;
        } else {
            buffer[i * 2 + 0] += l;
            buffer[i * 2 + 1] += r;
        }
    }

    /* Auto-advance to next track when buffer runs dry and file is consumed */
    if (to_read == 0 && m->file_data && m->file_offset >= m->file_size) {
        music_next_track(m);
    }

    return to_read;
}
