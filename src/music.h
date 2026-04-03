/* ================================================================== */
/* music.h -- Frontier synth: eerie ambient when outside signal range */
/* ================================================================== */
#ifndef MUSIC_H
#define MUSIC_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Buffer sizes (kept under ~320KB total)                             */
/* ------------------------------------------------------------------ */

enum {
    FS_SAMPLE_RATE    = 44100,
    FS_MAX_VOICES     = 24,
    FS_TICKS_PER_BEAT = 4,

    /* Delay line: ~0.74s per channel */
    FS_DELAY_SIZE     = 32768,

    /* Schroeder reverb comb/allpass (mutually prime for diffusion) */
    FS_COMB1_SIZE     = 1557,
    FS_COMB2_SIZE     = 1617,
    FS_COMB3_SIZE     = 1491,
    FS_COMB4_SIZE     = 1422,
    FS_AP1_SIZE       = 225,
    FS_AP2_SIZE       = 556,
};

/* ------------------------------------------------------------------ */
/* DSP primitives (all stored inline, no heap)                        */
/* ------------------------------------------------------------------ */

typedef enum {
    FS_ADSR_IDLE = 0, FS_ADSR_ATTACK, FS_ADSR_DECAY,
    FS_ADSR_SUSTAIN, FS_ADSR_RELEASE,
} fs_adsr_stage_t;

typedef struct {
    float attack, decay, sustain, release;
    float level, time;
    fs_adsr_stage_t stage;
} fs_adsr_t;

typedef struct { float cutoff, y1, y2; } fs_lpf_t;
typedef struct { float coeff, prev_in, prev_out; } fs_hpf_t;

typedef struct {
    float buf[FS_COMB2_SIZE]; /* sized to largest comb (1617) */
    int   size, pos;
    float feedback;
    fs_lpf_t tone;
} fs_comb_t;

typedef struct {
    float buf[FS_AP2_SIZE]; /* sized to largest allpass */
    int   size, pos;
    float gain;
} fs_allpass_t;

typedef struct {
    fs_comb_t    combs[4];
    fs_allpass_t allpasses[2];
    fs_hpf_t     lo_cut;
    float        wet;
} fs_reverb_t;

typedef struct {
    float buffer[FS_DELAY_SIZE];
    int   write_pos, delay_samples;
    float feedback, wet;
    fs_lpf_t tone;
} fs_delay_t;

typedef struct {
    float wow_phase, flutter_phase;
    float wow_rate, flutter_rate;
    float wow_depth, flutter_depth;
} fs_tape_t;

/* ------------------------------------------------------------------ */
/* Synth voice types                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    FS_VTYPE_DRONE,
    FS_VTYPE_GHOST,
    FS_VTYPE_PAD,
    FS_VTYPE_RIM,
    FS_VTYPE_BASS,
} fs_voice_type_t;

typedef struct {
    bool            active;
    fs_voice_type_t type;
    float           phase, phase2;
    float           frequency, detune;
    float           velocity, pan;
    fs_adsr_t       amp_env, filter_env;
    fs_lpf_t        filter;
} fs_synth_voice_t;

/* ------------------------------------------------------------------ */
/* Main synth struct (~320KB, embeddable without heap)                 */
/* ------------------------------------------------------------------ */

typedef struct frontier_synth {
    /* Config / state */
    int   sample_rate;
    float signal;           /* current signal strength 0.0-1.0 */
    float master_gain;      /* computed from signal mapping */

    /* RNG */
    uint32_t rng;

    /* Timing */
    float  bpm;
    double samples_per_tick;
    double tick_accum;
    int    current_tick;

    /* Chord / melody state */
    int  chord_notes[5];
    int  chord_count;
    int  root_midi;
    int  prog_index;

    /* Euclidean patterns */
    uint32_t rim_pattern;

    /* LFOs and modulators */
    float drone_lfo_phase;
    float energy_phase, energy_rate;
    float sweep_phase, sweep_rate;

    /* Tape wow/flutter */
    fs_tape_t tape;

    /* Ghost tone timer */
    float ghost_timer;
    float ghost_interval;

    /* Voices */
    fs_synth_voice_t voices[FS_MAX_VOICES];

    /* Effects (stereo pair) */
    fs_delay_t  delay_l, delay_r;
    fs_reverb_t reverb_l, reverb_r;
    fs_lpf_t    master_lpf_l, master_lpf_r;
    fs_lpf_t    noise_lpf;

    bool initialized;
} frontier_synth_t;

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void frontier_synth_init(frontier_synth_t* fs, int sample_rate);
void frontier_synth_set_signal(frontier_synth_t* fs, float signal);
void frontier_synth_render(frontier_synth_t* fs, float* mix_buffer, int frames, int channels);

#endif
