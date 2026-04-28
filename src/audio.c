#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "sokol_audio.h"
#include "sokol_log.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifdef SIGNAL_VOICE
#include "voice.h"
#endif

static uint32_t audio_rng_next(audio_state_t* a) {
    a->rng = (a->rng * 1664525u) + 1013904223u;
    return a->rng;
}

static float audio_randf(audio_state_t* a) {
    return (float)((audio_rng_next(a) >> 8) & 0x00FFFFFFu) / 16777215.0f;
}

static float audio_rand_bipolar(audio_state_t* a) {
    return (audio_randf(a) * 2.0f) - 1.0f;
}

void audio_clear_voices(audio_state_t* a) {
    memset(a->voices, 0, sizeof(a->voices));
    a->mining_tick_cooldown = 0.0f;
}

static void audio_play_voice(audio_state_t* a, audio_wave_t wave, float frequency, float sweep, float gain, float duration, float pan, float noise_mix) {
    if (!a->valid) return;
    for (int i = 0; i < AUDIO_VOICE_COUNT; i++) {
        if (a->voices[i].active) continue;
        {
            float clamped_pan = clampf(pan, -1.0f, 1.0f);
            a->voices[i] = (audio_voice_t){
                .active = true,
                .wave = wave,
                .phase = audio_randf(a),
                .frequency = frequency,
                .sweep = sweep,
                .gain = gain,
                .pan = clamped_pan,
                .pan_l = sqrtf(0.5f * (1.0f - clamped_pan)),
                .pan_r = sqrtf(0.5f * (1.0f + clamped_pan)),
                .duration = fmaxf(duration, 0.02f),
                .age = 0.0f,
                .noise_mix = clampf(noise_mix, 0.0f, 1.0f),
            };
        }
        return;
    }
}

static float audio_sample_wave(audio_state_t* a, audio_wave_t wave, float phase) {
    float wrapped = phase - floorf(phase);
    switch (wave) {
        case AUDIO_WAVE_SINE:
            return sinf(TWO_PI_F * wrapped);
        case AUDIO_WAVE_TRIANGLE:
            return 1.0f - (4.0f * fabsf(wrapped - 0.5f));
        case AUDIO_WAVE_SQUARE:
            return wrapped < 0.5f ? 1.0f : -1.0f;
        case AUDIO_WAVE_NOISE:
            return audio_rand_bipolar(a);
        default:
            return 0.0f;
    }
}

void audio_init(audio_state_t* a) {
    memset(a, 0, sizeof(*a));
    a->rng = 0xA11D0F5Du;
    a->music_duck_current = 1.0f;
    a->music_duck_target = 1.0f;
    a->voice_pcm.sample_rate = 24000; /* kokoro default */
    saudio_setup(&(saudio_desc){
        .sample_rate = 44100,
        .num_channels = 2,
        .buffer_frames = 2048,
        .packet_frames = 256,
        .num_packets = 32,
    });
    a->valid = saudio_isvalid();
    if (a->valid) {
        a->channels = saudio_channels();
        a->sample_rate = saudio_sample_rate();
    }
#ifdef SIGNAL_VOICE
    voice_pcm_init();
#endif
}

void audio_step(audio_state_t* a, float dt) {
    if (a->mining_tick_cooldown > 0.0f) {
        a->mining_tick_cooldown = fmaxf(0.0f, a->mining_tick_cooldown - dt);
    }
}

/* Ring buffer utilities for voice PCM. Always defined — the ring is
   always zero-init, so on OFF builds these return 0 and no I/O happens. */
static int audio_voice_ring_available(audio_state_t* a) {
    audio_voice_pcm_t* v = &a->voice_pcm;
    if (v->write_pos >= v->read_pos) {
        return v->write_pos - v->read_pos;
    } else {
        return AUDIO_VOICE_RING_FRAMES - v->read_pos + v->write_pos;
    }
}

#ifdef SIGNAL_VOICE
static int audio_voice_ring_space(audio_state_t* a) {
    return AUDIO_VOICE_RING_FRAMES - audio_voice_ring_available(a) - 1;
}
#endif

/* Resample a single voice PCM frame from voice_rate to output_rate.
 * Uses linear interpolation. */
static float audio_resample_voice(audio_state_t* a, float* resample_phase) {
    audio_voice_pcm_t* v = &a->voice_pcm;
    if (audio_voice_ring_available(a) == 0) return 0.0f;

    float ratio = (float)v->sample_rate / (float)a->sample_rate;

    int src_idx = (int)(*resample_phase);
    float frac = *resample_phase - (float)src_idx;
    *resample_phase += ratio;

    int read_idx = (v->read_pos + src_idx * 2) % (AUDIO_VOICE_RING_FRAMES * 2);
    int read_idx_next = (read_idx + 2) % (AUDIO_VOICE_RING_FRAMES * 2);

    float s0 = v->samples[read_idx];
    float s1 = v->samples[read_idx_next];

    return lerpf(s0, s1, frac);
}

/* Read pending PCM frames from voicebox pipe. Non-blocking, drops data if no space. */
static void audio_voice_pcm_pull(audio_state_t* a) {
#ifndef SIGNAL_VOICE
    (void)a;
#else
    audio_voice_pcm_t* v = &a->voice_pcm;
    int space = audio_voice_ring_space(a);
    if (space < 1024) return; /* not enough buffered space */

    /* Try to read PCM header + frames from voicebox fd 3 */
    uint8_t header[4];
    ssize_t nread = read(3, header, sizeof(header));
    if (nread != 4) return;

    uint32_t sr = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                  ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
    if (sr > 0 && sr < 96000) {
        v->sample_rate = (int)sr;
    }

    /* Read interleaved float32 stereo samples */
    float samples[512 * 2]; /* up to 512 frames */
    int frames_to_read = (space / 2 - 4) / 2; /* leave headroom */
    if (frames_to_read > 512) frames_to_read = 512;
    if (frames_to_read <= 0) return;

    nread = read(3, samples, frames_to_read * 2 * sizeof(float));
    if (nread <= 0) return;

    int frames_read = (int)(nread / (2 * sizeof(float)));
    for (int i = 0; i < frames_read * 2; i++) {
        int write_idx = (v->write_pos * 2 + i) % (AUDIO_VOICE_RING_FRAMES * 2);
        v->samples[write_idx] = samples[i];
    }
    v->write_pos = (v->write_pos + frames_read) % AUDIO_VOICE_RING_FRAMES;

    /* Update music duck target based on queue depth */
    int queue = audio_voice_ring_available(a);
    a->music_duck_target = (queue > 1000) ? 0.25f : 1.0f; /* ~6dB = 0.25x power */
#endif
}

void audio_generate_stream(audio_state_t* a) {
    if (!a->valid || !saudio_isvalid()) return;

    int channels = saudio_channels();
    int sample_rate = saudio_sample_rate();
    if ((channels < 1) || (channels > 2) || (sample_rate <= 0)) return;

    a->channels = channels;
    a->sample_rate = sample_rate;
    const float sample_dt = 1.0f / (float)sample_rate;

    audio_voice_pcm_pull(a);

    int frames_requested = saudio_expect();
    float voice_resample_phase = 0.0f;
    while (frames_requested > 0) {
        int frames_to_mix = frames_requested > AUDIO_MIX_FRAMES ? AUDIO_MIX_FRAMES : frames_requested;
        memset(a->mix_buffer, 0, sizeof(float) * (size_t)(frames_to_mix * channels));

        /* Update music duck envelope (smooth ramp over ~50ms) */
        float duck_speed = 0.1f; /* exponential smoothing coefficient */
        a->music_duck_current = lerpf(a->music_duck_current, a->music_duck_target, duck_speed);

        for (int fi = 0; fi < frames_to_mix; fi++) {
            float left = 0.0f;
            float right = 0.0f;

            for (int vi = 0; vi < AUDIO_VOICE_COUNT; vi++) {
                audio_voice_t* v = &a->voices[vi];
                if (!v->active) continue;
                if (v->age >= v->duration) { v->active = false; continue; }

                float attack = clampf(v->age / 0.01f, 0.0f, 1.0f);
                float release = 1.0f - clampf(v->age / v->duration, 0.0f, 1.0f);
                float envelope = v->gain * attack * release * release;
                float sample = audio_sample_wave(a, v->wave, v->phase);
                if ((v->noise_mix > 0.0f) && (v->wave != AUDIO_WAVE_NOISE)) {
                    sample = lerpf(sample, audio_rand_bipolar(a), v->noise_mix);
                }
                sample *= envelope;

                if (channels == 1) {
                    left += sample;
                } else {
                    left += sample * v->pan_l;
                    right += sample * v->pan_r;
                }

                v->frequency = fmaxf(45.0f, v->frequency + (v->sweep * sample_dt));
                v->phase += v->frequency * sample_dt;
                v->phase -= floorf(v->phase);
                v->age += sample_dt;
            }

            if (channels == 1) {
                a->mix_buffer[fi] = clampf(left * 0.75f, -1.0f, 1.0f);
            } else {
                int base = fi * channels;
                a->mix_buffer[base + 0] = clampf(left * 0.75f, -1.0f, 1.0f);
                a->mix_buffer[base + 1] = clampf(right * 0.75f, -1.0f, 1.0f);
            }
        }

        /* Mix in external audio sources (music, video episodes) with ducking */
        if (a->mix_callback) {
            a->mix_callback(a->mix_buffer, frames_to_mix, channels, a->mix_callback_user);

            /* Apply music ducking: scale music channel by duck_current */
            for (int fi = 0; fi < frames_to_mix * channels; fi++) {
                a->mix_buffer[fi] *= a->music_duck_current;
            }
        }

        /* Mix in voice PCM (with resampling from 24kHz to output rate) */
        if (audio_voice_ring_available(a) > 0) {
            for (int fi = 0; fi < frames_to_mix; fi++) {
                float voice_sample = audio_resample_voice(a, &voice_resample_phase);
                if (channels == 1) {
                    a->mix_buffer[fi] += voice_sample * 0.5f; /* scale down to avoid clipping */
                } else {
                    int base = fi * channels;
                    a->mix_buffer[base + 0] += voice_sample * 0.5f;
                    a->mix_buffer[base + 1] += voice_sample * 0.5f;
                }
            }
        }

        /* Final clamp */
        for (int fi2 = 0; fi2 < frames_to_mix * channels; fi2++) {
            if (a->mix_buffer[fi2] > 1.0f) a->mix_buffer[fi2] = 1.0f;
            else if (a->mix_buffer[fi2] < -1.0f) a->mix_buffer[fi2] = -1.0f;
        }

        saudio_push(a->mix_buffer, frames_to_mix);
        frames_requested = saudio_expect();
    }
}

void audio_play_mining_tick(audio_state_t* a) {
    if ((a->mining_tick_cooldown > 0.0f) || !a->valid) return;
    a->mining_tick_cooldown = 0.06f;
    audio_play_voice(a, AUDIO_WAVE_SQUARE, 1080.0f, -5200.0f, 0.035f, 0.035f, audio_rand_bipolar(a) * 0.12f, 0.08f);
    audio_play_voice(a, AUDIO_WAVE_TRIANGLE, 720.0f, -1800.0f, 0.022f, 0.05f, 0.0f, 0.0f);
}

void audio_play_fracture(audio_state_t* a, asteroid_tier_t parent_tier) {
    static const float base_freqs[ASTEROID_TIER_COUNT] = { 180.0f, 250.0f, 340.0f, 420.0f };
    float base = base_freqs[parent_tier < ASTEROID_TIER_COUNT ? parent_tier : ASTEROID_TIER_L];
    audio_play_voice(a, AUDIO_WAVE_TRIANGLE, base, -base * 0.65f, 0.10f, 0.18f, audio_rand_bipolar(a) * 0.24f, 0.15f);
    audio_play_voice(a, AUDIO_WAVE_NOISE, base * 0.7f, -base * 0.45f, 0.05f, 0.12f, 0.0f, 1.0f);
}

void audio_play_pickup(audio_state_t* a, float ore, int fragments) {
    float gain = clampf(0.04f + (ore * 0.0032f), 0.04f, 0.11f);
    float pitch = 540.0f + clampf(ore * 14.0f, 0.0f, 220.0f) + (float)(fragments * 24);
    audio_play_voice(a, AUDIO_WAVE_SINE, pitch, 920.0f, gain, 0.09f, audio_rand_bipolar(a) * 0.35f, 0.0f);
}

void audio_play_dock(audio_state_t* a) {
    audio_play_voice(a, AUDIO_WAVE_SINE, 310.0f, 580.0f, 0.08f, 0.16f, -0.12f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE, 470.0f, 380.0f, 0.06f, 0.18f, 0.12f, 0.0f);
}

void audio_play_launch(audio_state_t* a) {
    audio_play_voice(a, AUDIO_WAVE_TRIANGLE, 620.0f, -980.0f, 0.06f, 0.14f, -0.10f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE, 420.0f, -420.0f, 0.04f, 0.18f, 0.10f, 0.0f);
}

void audio_play_sale(audio_state_t* a) {
    audio_play_voice(a, AUDIO_WAVE_SINE, 420.0f, 440.0f, 0.07f, 0.15f, -0.18f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE, 630.0f, 520.0f, 0.06f, 0.17f, 0.18f, 0.0f);
}

void audio_play_repair(audio_state_t* a) {
    audio_play_voice(a, AUDIO_WAVE_TRIANGLE, 240.0f, 260.0f, 0.06f, 0.18f, 0.0f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE, 510.0f, -120.0f, 0.03f, 0.20f, 0.0f, 0.0f);
}

void audio_play_upgrade(audio_state_t* a, ship_upgrade_t upgrade) {
    float root = 420.0f;
    switch (upgrade) {
        case SHIP_UPGRADE_MINING: root = 520.0f; break;
        case SHIP_UPGRADE_HOLD: root = 430.0f; break;
        case SHIP_UPGRADE_TRACTOR: root = 610.0f; break;
        case SHIP_UPGRADE_COUNT: default: break;
    }
    audio_play_voice(a, AUDIO_WAVE_SINE, root, 320.0f, 0.06f, 0.12f, -0.12f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE, root * 1.25f, 380.0f, 0.05f, 0.15f, 0.12f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE, root * 1.5f, 480.0f, 0.04f, 0.18f, 0.0f, 0.0f);
}

void audio_play_commission(audio_state_t* a) {
    /* Rising chord: module coming online. Three voices stepping up. */
    audio_play_voice(a, AUDIO_WAVE_SINE,     330.0f, 200.0f, 0.06f, 0.25f, -0.15f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE,     440.0f, 260.0f, 0.05f, 0.30f,  0.15f, 0.0f);
    audio_play_voice(a, AUDIO_WAVE_SINE,     550.0f, 330.0f, 0.04f, 0.35f,  0.0f,  0.0f);
    audio_play_voice(a, AUDIO_WAVE_TRIANGLE, 220.0f, 110.0f, 0.03f, 0.40f,  0.0f,  0.0f);
}

void audio_play_damage(audio_state_t* a, float damage) {
    float gain = clampf(0.035f + (damage * 0.0018f), 0.04f, 0.10f);
    audio_play_voice(a, AUDIO_WAVE_NOISE, 180.0f, -80.0f, gain, 0.12f, audio_rand_bipolar(a) * 0.22f, 1.0f);
    audio_play_voice(a, AUDIO_WAVE_SQUARE, 130.0f, -160.0f, gain * 0.5f, 0.10f, 0.0f, 0.15f);
}
