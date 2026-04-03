/* ================================================================== */
/* music_test.c -- Signal-reactive procedural space-ambient generator  */
/*                                                                    */
/* Compile:  cc -O2 tools/music_test.c -o music_test -lm              */
/* Usage:    ./music_test -o out.wav -s 0.8                           */
/*           ./music_test -o frontier.wav -s 0.1 -d 30                */
/*           ./music_test | ffplay -f s16le -ar 44100 -ac 2 -nodisp - */
/* ================================================================== */

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* Constants                                                          */
/* ================================================================== */

#define SAMPLE_RATE       44100
#define NUM_CHANNELS      2
#define TWO_PI            6.283185307179586f
#define PI_F              3.14159265358979f
#define TICKS_PER_BEAT    4
#define MAX_SYNTH_VOICES  32
#define RENDER_CHUNK      512

/* Signal bands (matching shared/signal_model.h) */
#define SIGNAL_FRONTIER    0.15f
#define SIGNAL_FRINGE      0.50f
#define SIGNAL_OPERATIONAL 0.80f

/* Reverb buffer sizes (mutually prime for diffusion) */
#define COMB1_SIZE  1557
#define COMB2_SIZE  1617
#define COMB3_SIZE  1491
#define COMB4_SIZE  1422
#define AP1_SIZE    225
#define AP2_SIZE    556

/* Delay / chorus buffer */
#define DELAY_BUF_SIZE    (SAMPLE_RATE * 2)
#define CHORUS_BUF_SIZE   2048

/* ================================================================== */
/* RNG                                                                */
/* ================================================================== */

static uint32_t rng_state = 0xA11D0F5Du;

static uint32_t rng_next(void) {
    rng_state = (rng_state * 1664525u) + 1013904223u;
    return rng_state;
}

static float randf(void) {
    return (float)((rng_next() >> 8) & 0x00FFFFFFu) / 16777215.0f;
}

static float rand_bipolar(void) {
    return randf() * 2.0f - 1.0f;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float lerpf(float a, float b, float t) {
    return a + t * (b - a);
}

/* ================================================================== */
/* Oscillators                                                        */
/* ================================================================== */

static float osc_sine(float phase) {
    return sinf(TWO_PI * phase);
}

static float osc_triangle(float phase) {
    float w = phase - floorf(phase);
    return 1.0f - 4.0f * fabsf(w - 0.5f);
}

static float osc_saw(float phase) {
    float w = phase - floorf(phase);
    return 2.0f * w - 1.0f;
}

static float osc_noise(void) {
    return rand_bipolar();
}

/* ================================================================== */
/* ADSR Envelope                                                      */
/* ================================================================== */

typedef enum {
    ADSR_IDLE = 0, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE,
} adsr_stage_t;

typedef struct {
    float attack, decay, sustain, release;
    float level, time;
    adsr_stage_t stage;
} adsr_t;

static void adsr_gate_on(adsr_t* e) { e->stage = ADSR_ATTACK; e->time = 0.0f; }
static void adsr_gate_off(adsr_t* e) {
    if (e->stage != ADSR_IDLE) { e->stage = ADSR_RELEASE; e->time = 0.0f; }
}

static float adsr_process(adsr_t* e, float dt) {
    e->time += dt;
    switch (e->stage) {
        case ADSR_ATTACK:
            if (e->attack <= 0.0f) { e->level = 1.0f; e->stage = ADSR_DECAY; e->time = 0.0f; }
            else { e->level = e->time / e->attack; if (e->level >= 1.0f) { e->level = 1.0f; e->stage = ADSR_DECAY; e->time = 0.0f; } }
            break;
        case ADSR_DECAY: {
            float t = (e->decay > 0.0f) ? e->time / e->decay : 1.0f;
            e->level = 1.0f + (e->sustain - 1.0f) * (t < 1.0f ? t : 1.0f);
            if (t >= 1.0f) { e->level = e->sustain; e->stage = ADSR_SUSTAIN; }
            break;
        }
        case ADSR_SUSTAIN: e->level = e->sustain; break;
        case ADSR_RELEASE: {
            float t = (e->release > 0.0f) ? e->time / e->release : 1.0f;
            e->level = e->sustain * (1.0f - (t < 1.0f ? t : 1.0f));
            if (t >= 1.0f) { e->level = 0.0f; e->stage = ADSR_IDLE; }
            break;
        }
        default: e->level = 0.0f; break;
    }
    return e->level;
}

/* ================================================================== */
/* Low-Pass Filter (two-pole cascaded one-pole)                       */
/* ================================================================== */

typedef struct { float cutoff, y1, y2; } lpf_t;

static void lpf_set_freq(lpf_t* f, float freq_hz, float sr) {
    float fc = freq_hz / sr;
    if (fc > 0.49f) fc = 0.49f;
    f->cutoff = 1.0f - expf(-TWO_PI * fc);
}

static float lpf_process(lpf_t* f, float in) {
    f->y1 += f->cutoff * (in - f->y1);
    f->y2 += f->cutoff * (f->y1 - f->y2);
    return f->y2;
}

/* ================================================================== */
/* High-Pass Filter (one-pole)                                        */
/* ================================================================== */

typedef struct { float coeff, prev_in, prev_out; } hpf_t;

static void hpf_set_freq(hpf_t* f, float freq_hz, float sr) {
    float rc = 1.0f / (TWO_PI * freq_hz);
    f->coeff = rc / (rc + 1.0f / sr);
}

static float hpf_process(hpf_t* f, float in) {
    f->prev_out = f->coeff * (f->prev_out + in - f->prev_in);
    f->prev_in = in;
    return f->prev_out;
}

/* ================================================================== */
/* Bit Crusher                                                        */
/* ================================================================== */

typedef struct { float bits, hold; int rate_div, counter; } bitcrush_t;

static float bitcrush_process(bitcrush_t* bc, float in) {
    bc->counter++;
    if (bc->counter >= bc->rate_div) {
        bc->counter = 0;
        float levels = powf(2.0f, bc->bits);
        bc->hold = roundf(in * levels) / levels;
    }
    return bc->hold;
}

/* ================================================================== */
/* Comb Filter (for Schroeder reverb)                                 */
/* ================================================================== */

typedef struct {
    float* buf;
    int size, pos;
    float feedback;
    lpf_t tone; /* damping filter in feedback loop */
} comb_t;

static float comb_process(comb_t* c, float in) {
    float delayed = c->buf[c->pos];
    float filtered = lpf_process(&c->tone, delayed);
    c->buf[c->pos] = in + filtered * c->feedback;
    c->pos = (c->pos + 1) % c->size;
    return delayed;
}

/* ================================================================== */
/* Allpass Filter (for Schroeder reverb)                              */
/* ================================================================== */

typedef struct { float* buf; int size, pos; float gain; } allpass_t;

static float allpass_process(allpass_t* a, float in) {
    float delayed = a->buf[a->pos];
    float out = -in * a->gain + delayed;
    a->buf[a->pos] = in + delayed * a->gain;
    a->pos = (a->pos + 1) % a->size;
    return out;
}

/* ================================================================== */
/* Schroeder Reverb (4 combs + 2 allpasses)                          */
/* ================================================================== */

typedef struct {
    float comb_buf1[COMB1_SIZE], comb_buf2[COMB2_SIZE];
    float comb_buf3[COMB3_SIZE], comb_buf4[COMB4_SIZE];
    float ap_buf1[AP1_SIZE], ap_buf2[AP2_SIZE];
    comb_t combs[4];
    allpass_t allpasses[2];
    hpf_t lo_cut;   /* cut mud from reverb return */
    float wet;
} reverb_t;

static void reverb_init(reverb_t* r, float wet) {
    memset(r, 0, sizeof(*r));
    r->wet = wet;

    float fb = 0.78f;
    r->combs[0] = (comb_t){ r->comb_buf1, COMB1_SIZE, 0, fb };
    r->combs[1] = (comb_t){ r->comb_buf2, COMB2_SIZE, 0, fb };
    r->combs[2] = (comb_t){ r->comb_buf3, COMB3_SIZE, 0, fb };
    r->combs[3] = (comb_t){ r->comb_buf4, COMB4_SIZE, 0, fb };
    for (int i = 0; i < 4; i++) lpf_set_freq(&r->combs[i].tone, 3000.0f, SAMPLE_RATE);

    r->allpasses[0] = (allpass_t){ r->ap_buf1, AP1_SIZE, 0, 0.7f };
    r->allpasses[1] = (allpass_t){ r->ap_buf2, AP2_SIZE, 0, 0.7f };

    hpf_set_freq(&r->lo_cut, 250.0f, SAMPLE_RATE);
}

static float reverb_process(reverb_t* r, float in) {
    /* 4 parallel combs summed */
    float sum = 0.0f;
    for (int i = 0; i < 4; i++) sum += comb_process(&r->combs[i], in);
    sum *= 0.25f;
    /* 2 series allpasses */
    sum = allpass_process(&r->allpasses[0], sum);
    sum = allpass_process(&r->allpasses[1], sum);
    /* Cut low-end mud from reverb */
    sum = hpf_process(&r->lo_cut, sum);
    return in + sum * r->wet;
}

/* ================================================================== */
/* Delay Line (tempo-synced, filtered feedback)                       */
/* ================================================================== */

typedef struct {
    float buffer[DELAY_BUF_SIZE];
    int write_pos, delay_samples;
    float feedback, wet;
    lpf_t tone;
} delay_t;

static void delay_init(delay_t* d, float bpm) {
    memset(d, 0, sizeof(*d));
    float beat_sec = 60.0f / bpm;
    d->delay_samples = (int)(beat_sec * 0.75f * SAMPLE_RATE); /* dotted eighth */
    if (d->delay_samples >= DELAY_BUF_SIZE) d->delay_samples = DELAY_BUF_SIZE - 1;
    d->feedback = 0.40f;
    d->wet = 0.25f;
    lpf_set_freq(&d->tone, 1800.0f, SAMPLE_RATE);
}

static float delay_process(delay_t* d, float in) {
    int rp = d->write_pos - d->delay_samples;
    if (rp < 0) rp += DELAY_BUF_SIZE;
    float delayed = d->buffer[rp];
    float filtered = lpf_process(&d->tone, delayed);
    d->buffer[d->write_pos] = in + filtered * d->feedback;
    d->write_pos = (d->write_pos + 1) % DELAY_BUF_SIZE;
    return in + delayed * d->wet;
}

/* ================================================================== */
/* Wow / Flutter (tape-like pitch drift)                              */
/* ================================================================== */

typedef struct {
    float wow_phase, flutter_phase;
    float wow_rate, flutter_rate;
    float wow_depth, flutter_depth;
} tape_t;

static float tape_pitch_mod(tape_t* t, float dt) {
    t->wow_phase += t->wow_rate * dt;
    t->wow_phase -= floorf(t->wow_phase);
    t->flutter_phase += t->flutter_rate * dt;
    t->flutter_phase -= floorf(t->flutter_phase);
    return 1.0f + osc_sine(t->wow_phase) * t->wow_depth
                + osc_sine(t->flutter_phase) * t->flutter_depth;
}

/* ================================================================== */
/* Vinyl Crackle                                                      */
/* ================================================================== */

static float vinyl_crackle(float density) {
    float r = randf();
    if (r < density * 0.002f) {
        return rand_bipolar() * (0.05f + randf() * 0.1f);
    }
    return 0.0f;
}

/* ================================================================== */
/* Euclidean Rhythm Generator                                         */
/* ================================================================== */

static uint32_t euclidean_rhythm(int pulses, int steps) {
    uint32_t pattern = 0;
    int bucket = 0;
    for (int i = 0; i < steps; i++) {
        bucket += pulses;
        if (bucket >= steps) {
            bucket -= steps;
            pattern |= (1u << i);
        }
    }
    return pattern;
}

static bool eucl_hit(uint32_t pattern, int step, int total) {
    return (pattern >> (step % total)) & 1;
}

/* ================================================================== */
/* Music Theory                                                       */
/* ================================================================== */

static float midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* Minor pentatonic — no dissonant intervals, always musical */
static const int SCALE_PENTA[] = { 0, 3, 5, 7, 10 };
#define SCALE_PENTA_LEN 5

/* Dorian — richer, more notes for higher signal */
static const int SCALE_DORIAN[] = { 0, 2, 3, 5, 7, 9, 10 };
#define SCALE_DORIAN_LEN 7

/* Natural minor — for tension */
static const int SCALE_MINOR[] = { 0, 2, 3, 5, 7, 8, 10 };
#define SCALE_MINOR_LEN 7

/* Markov transition matrix for pentatonic melody (favors stepwise motion) */
static const float MELODY_MARKOV[5][5] = {
    { 0.05f, 0.35f, 0.20f, 0.25f, 0.15f },
    { 0.30f, 0.05f, 0.35f, 0.15f, 0.15f },
    { 0.15f, 0.30f, 0.05f, 0.35f, 0.15f },
    { 0.15f, 0.15f, 0.30f, 0.05f, 0.35f },
    { 0.35f, 0.15f, 0.15f, 0.25f, 0.10f },
};

static int markov_next(int current) {
    float r = randf();
    float cum = 0.0f;
    for (int j = 0; j < SCALE_PENTA_LEN; j++) {
        cum += MELODY_MARKOV[current][j];
        if (r <= cum) return j;
    }
    return SCALE_PENTA_LEN - 1;
}

/* Modern chord voicings — wide intervals, 7ths, 9ths, suspensions.
 * Each voicing is 5 semitone offsets from root. Spread across octaves
 * for that open, cinematic feel rather than stacked triads. */
typedef struct { int offsets[5]; } voicing_t;

/* Am9 shape, Fmaj7#11, Cmaj9, Em7sus4 — modern ambient/neo-soul */
static const voicing_t VOICINGS_A[] = {
    {{ 0, 7, 14, 16, 24 }},   /* root, 5th, 9th(+oct), 10th, octave+12 */
    {{ 5, 12, 16, 21, 29 }},  /* F, C, E, A, F — open Fmaj7 */
    {{ 3, 7, 14, 19, 26 }},   /* Eb, G, D, G, D — Cm add9 spread */
    {{ 7, 12, 19, 24, 31 }},  /* G, C, G, C, G — power 5ths */
};

/* Darker voicings for low signal */
static const voicing_t VOICINGS_B[] = {
    {{ 0, 7, 15, 19, 24 }},   /* root, 5th, maj7+oct, 3rd+oct, 2oct */
    {{ 3, 10, 15, 22, 27 }},  /* minor 3rd spread */
    {{ 5, 12, 17, 24, 29 }},  /* 4th based, sus feel */
    {{ 0, 5, 12, 17, 24 }},   /* quartal stack — very modern */
};

#define NUM_VOICINGS 4
#define VOICING_NOTES 5

/* Progression: which voicing index to use per bar */
static const int PROG_A[] = { 0, 2, 1, 3 };
static const int PROG_B[] = { 0, 3, 2, 1 };
#define PROG_LEN 4

static void build_chord(int root_midi, const voicing_t* voicings,
                        int prog_idx, int* notes, int* count) {
    *count = VOICING_NOTES;
    const voicing_t* v = &voicings[prog_idx % NUM_VOICINGS];
    for (int i = 0; i < VOICING_NOTES; i++)
        notes[i] = root_midi + v->offsets[i];
}

/* Arp patterns — modern: skip beats, syncopated, not just up/down */
/* Pattern values are chord-note indices; -1 = rest (skip this step) */
static const int ARP_SPARSE[] = { 0, -1, 2, -1, 1, -1, 3, -1 };
#define ARP_SPARSE_LEN 8

static const int ARP_SYNCO[] = { 0, 2, -1, 4, 1, -1, 3, 0 };
#define ARP_SYNCO_LEN 8

static const int ARP_ROLLING[] = { 0, 1, 2, 1, 3, 2, 4, 3 };
#define ARP_ROLLING_LEN 8

/* ================================================================== */
/* Synth Voice                                                        */
/* ================================================================== */

typedef enum {
    VTYPE_BASS, VTYPE_ARP, VTYPE_PAD, VTYPE_HIHAT, VTYPE_KICK,
    VTYPE_DRONE, VTYPE_GHOST, VTYPE_RIM,
} voice_type_t;

typedef struct {
    bool active;
    voice_type_t type;
    float phase, phase2;
    float frequency, detune;
    float velocity, pan;
    adsr_t amp_env, filter_env;
    lpf_t filter;
} synth_voice_t;

/* ================================================================== */
/* Signal-Derived Parameters                                          */
/* ================================================================== */

typedef struct {
    float signal;           /* 0.0 - 1.0 */
    float noise_floor;      /* static/hiss level */
    float lp_cutoff;        /* master LP cutoff Hz */
    float bit_depth;        /* bit crusher depth */
    int   sample_div;       /* sample rate divisor */
    float note_prob;        /* probability a note actually plays */
    float wow_depth;        /* tape wow depth */
    float flutter_depth;    /* tape flutter depth */
    float crackle_density;  /* vinyl crackle density */
    float reverb_wet;       /* reverb send */
    float delay_wet;        /* delay send */
    float saturation_drive; /* tanh drive */
    int   arp_pulses;       /* Euclidean pulse count for arps */
    int   kick_pulses;      /* Euclidean pulse count for kicks */
    float stereo_width;     /* 0=mono, 1=full stereo */
    float pad_vol;
    float arp_vol;
    float bass_vol;
    float melody_prob;      /* probability of melody notes */
} signal_params_t;

static signal_params_t compute_signal_params(float sig) {
    signal_params_t p;
    p.signal = sig;

    /* Noise: subliminal warmth only — never audible as hiss */
    p.noise_floor = (1.0f - sig) * 0.001f;

    /* Filter: quadratic curve — opens slowly, then wide */
    p.lp_cutoff = 200.0f + sig * sig * 14000.0f;

    /* Bit depth: higher floor so it never sounds crunchy/static */
    p.bit_depth = 8.0f + sig * 7.0f;
    p.sample_div = (sig < 0.3f) ? 2 : 1;

    /* Note probability: quadratic so notes thin out fast at low signal */
    p.note_prob = sig * sig;
    if (sig < SIGNAL_FRONTIER) p.note_prob = 0.0f;

    /* Tape wow/flutter: strong drift at low signal */
    p.wow_depth = 0.0005f + (1.0f - sig) * 0.005f;
    p.flutter_depth = 0.0001f + (1.0f - sig) * 0.0015f;

    /* Vinyl crackle — barely there, only at very low signal */
    p.crackle_density = (sig < 0.3f) ? (0.3f - sig) * 0.05f : 0.0f;

    /* Reverb: more wash when distant */
    p.reverb_wet = 0.55f - sig * 0.40f;

    /* Delay: spacious echoing plucks, generous wet */
    p.delay_wet = 0.30f + (1.0f - sig) * 0.15f;

    /* Saturation: warm, not gritty */
    p.saturation_drive = 1.1f + (1.0f - sig) * 1.5f;

    /* Euclidean density */
    p.arp_pulses = 1 + (int)(sig * 6.0f);   /* 1-7 out of 8 */
    p.kick_pulses = 2 + (int)(sig * 2.5f);  /* 2-4 out of 8, always driving */

    /* Stereo width: mono at zero, full at one */
    p.stereo_width = sig;

    /* Layer volumes */
    p.bass_vol = (sig > SIGNAL_FRONTIER) ? 0.5f + sig * 0.4f : sig / SIGNAL_FRONTIER * 0.2f;
    p.pad_vol = (sig > 0.25f) ? (sig - 0.25f) / 0.75f * 0.18f : 0.0f;
    p.arp_vol = (sig > SIGNAL_FRINGE * 0.5f) ? sig * 0.22f : 0.0f;
    p.melody_prob = (sig > SIGNAL_FRINGE) ? (sig - SIGNAL_FRINGE) * 1.5f : 0.0f;

    return p;
}

/* ================================================================== */
/* Music State                                                        */
/* ================================================================== */

typedef struct {
    /* Timing */
    float  bpm;
    double samples_per_tick;
    double tick_accum;
    int    current_tick;
    float  swing;            /* 0.0=straight, 0.3=hard swing */

    /* Signal */
    signal_params_t sp;

    /* Chord / melody state */
    int  prog_index;
    int  chord_notes[VOICING_NOTES];
    int  chord_count;
    int  root_midi;
    int  arp_pos;
    int  melody_note_idx;   /* index into pentatonic scale */

    /* Euclidean patterns */
    uint32_t arp_pattern;
    uint32_t kick_pattern;
    uint32_t hat_pattern;
    uint32_t rim_pattern;

    /* Bass LFO */
    float bass_lfo_phase, bass_lfo_rate;

    /* Sidechain pump (triggered by kick) */
    float pump_env;
    float pump_release;

    /* Energy wave — slow sine that modulates density/filter over 8-16 bars */
    float energy_phase;
    float energy_rate;       /* Hz — very slow */

    /* Sweep LFO — for scanner/granular texture */
    float sweep_phase;
    float sweep_rate;

    /* Tape */
    tape_t tape;

    /* Voices */
    synth_voice_t voices[MAX_SYNTH_VOICES];

    /* Effects */
    delay_t    delay_l, delay_r;
    reverb_t   reverb_l, reverb_r;
    bitcrush_t crush_l, crush_r;
    lpf_t      master_lpf_l, master_lpf_r;
    lpf_t      crackle_lpf;  /* soften crackle edges */
    lpf_t      noise_lpf;   /* soften background hiss */
} music_state_t;

/* ================================================================== */
/* Voice Allocation                                                   */
/* ================================================================== */

static synth_voice_t* voice_alloc(music_state_t* m, voice_type_t type) {
    for (int i = 0; i < MAX_SYNTH_VOICES; i++)
        if (!m->voices[i].active) {
            memset(&m->voices[i], 0, sizeof(synth_voice_t));
            m->voices[i].type = type;
            return &m->voices[i];
        }
    for (int i = 0; i < MAX_SYNTH_VOICES; i++)
        if (m->voices[i].type == type && m->voices[i].amp_env.stage == ADSR_RELEASE) {
            memset(&m->voices[i], 0, sizeof(synth_voice_t));
            m->voices[i].type = type;
            return &m->voices[i];
        }
    return NULL;
}

static void release_type(music_state_t* m, voice_type_t type) {
    for (int i = 0; i < MAX_SYNTH_VOICES; i++)
        if (m->voices[i].active && m->voices[i].type == type)
            adsr_gate_off(&m->voices[i].amp_env);
}

/* ================================================================== */
/* Note Triggers                                                      */
/* ================================================================== */

static void trigger_bass(music_state_t* m, int midi) {
    synth_voice_t* v = voice_alloc(m, VTYPE_BASS);
    if (!v) return;
    v->active = true;
    v->frequency = midi_to_freq(midi);
    v->velocity = m->sp.bass_vol;
    v->amp_env = (adsr_t){ .attack=0.04f, .decay=0.5f, .sustain=0.45f, .release=0.6f };
    v->filter_env = (adsr_t){ .attack=0.03f, .decay=0.7f, .sustain=0.1f, .release=0.4f };
    lpf_set_freq(&v->filter, 90.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
    adsr_gate_on(&v->filter_env);
}

static void trigger_arp(music_state_t* m, int midi, float vel) {
    synth_voice_t* v = voice_alloc(m, VTYPE_ARP);
    if (!v) return;
    v->active = true;
    v->frequency = midi_to_freq(midi);
    v->velocity = vel * m->sp.arp_vol;
    v->pan = rand_bipolar() * 0.5f * m->sp.stereo_width;
    /* Crisp pluck — bright attack that echoes through delay */
    v->amp_env = (adsr_t){ .attack=0.001f, .decay=0.08f, .sustain=0.04f, .release=0.10f };
    v->filter_env = (adsr_t){ .attack=0.001f, .decay=0.12f, .sustain=0.08f, .release=0.08f };
    lpf_set_freq(&v->filter, 600.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
    adsr_gate_on(&v->filter_env);
}

static void trigger_pad(music_state_t* m, int midi) {
    synth_voice_t* v = voice_alloc(m, VTYPE_PAD);
    if (!v) return;
    v->active = true;
    v->frequency = midi_to_freq(midi);
    v->detune = v->frequency * 0.008f;  /* wide chorus for choir-like shimmer */
    v->velocity = m->sp.pad_vol;
    v->pan = rand_bipolar() * 0.6f * m->sp.stereo_width; /* wide stereo field */
    /* Glacial evolving pad */
    v->amp_env = (adsr_t){ .attack=3.0f, .decay=2.0f, .sustain=0.4f, .release=4.0f };
    v->filter_env = (adsr_t){ .attack=4.0f, .decay=3.0f, .sustain=0.15f, .release=3.0f };
    lpf_set_freq(&v->filter, 160.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
    adsr_gate_on(&v->filter_env);
}

static void trigger_hihat(music_state_t* m) {
    synth_voice_t* v = voice_alloc(m, VTYPE_HIHAT);
    if (!v) return;
    v->active = true;
    v->frequency = 5000.0f;
    v->velocity = 0.015f + randf() * 0.01f;
    v->pan = rand_bipolar() * 0.35f * m->sp.stereo_width;
    v->amp_env = (adsr_t){ .attack=0.001f, .decay=0.02f, .sustain=0.0f, .release=0.015f };
    lpf_set_freq(&v->filter, 1600.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
}

static void trigger_kick(music_state_t* m) {
    synth_voice_t* v = voice_alloc(m, VTYPE_KICK);
    if (!v) return;
    v->active = true;
    v->frequency = 50.0f;
    v->velocity = 0.55f;  /* punchy */
    v->amp_env = (adsr_t){ .attack=0.001f, .decay=0.18f, .sustain=0.0f, .release=0.1f };
    lpf_set_freq(&v->filter, 300.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
    /* Trigger sidechain pump */
    m->pump_env = 0.0f;
}

/* Rim: tonal percussive click — pitched, short, cuts through */
static void trigger_rim(music_state_t* m, int midi) {
    synth_voice_t* v = voice_alloc(m, VTYPE_RIM);
    if (!v) return;
    v->active = true;
    v->frequency = midi_to_freq(midi);
    v->velocity = 0.10f + randf() * 0.04f;
    v->pan = rand_bipolar() * 0.3f * m->sp.stereo_width;
    /* Very short, snappy — like a clave or rimshot */
    v->amp_env = (adsr_t){ .attack=0.0005f, .decay=0.03f, .sustain=0.0f, .release=0.02f };
    v->filter_env = (adsr_t){ .attack=0.0005f, .decay=0.04f, .sustain=0.0f, .release=0.02f };
    lpf_set_freq(&v->filter, 800.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
    adsr_gate_on(&v->filter_env);
}

static void trigger_drone(music_state_t* m, int midi) {
    synth_voice_t* v = voice_alloc(m, VTYPE_DRONE);
    if (!v) return;
    v->active = true;
    v->frequency = midi_to_freq(midi);
    v->velocity = 0.04f * m->sp.signal;
    v->amp_env = (adsr_t){ .attack=3.0f, .decay=2.0f, .sustain=0.5f, .release=3.0f };
    lpf_set_freq(&v->filter, 150.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
}

/* Ghost tone: random radio blip at frontier signal */
static void trigger_ghost(music_state_t* m) {
    synth_voice_t* v = voice_alloc(m, VTYPE_GHOST);
    if (!v) return;
    v->active = true;
    v->frequency = 200.0f + randf() * 2000.0f;
    v->velocity = 0.02f + randf() * 0.03f;
    v->pan = rand_bipolar() * 0.8f;
    v->amp_env = (adsr_t){ .attack=0.01f, .decay=0.05f, .sustain=0.0f, .release=0.04f };
    lpf_set_freq(&v->filter, 1200.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
}

/* Melody note: longer, singing tone */
static void trigger_melody(music_state_t* m, int midi) {
    synth_voice_t* v = voice_alloc(m, VTYPE_ARP); /* reuse arp type */
    if (!v) return;
    v->active = true;
    v->frequency = midi_to_freq(midi);
    v->velocity = 0.12f * m->sp.signal;
    v->pan = rand_bipolar() * 0.25f * m->sp.stereo_width;
    v->amp_env = (adsr_t){ .attack=0.02f, .decay=0.3f, .sustain=0.2f, .release=0.25f };
    v->filter_env = (adsr_t){ .attack=0.01f, .decay=0.4f, .sustain=0.15f, .release=0.2f };
    lpf_set_freq(&v->filter, 600.0f, SAMPLE_RATE);
    adsr_gate_on(&v->amp_env);
    adsr_gate_on(&v->filter_env);
}

/* ================================================================== */
/* Sequencer Tick                                                     */
/* ================================================================== */

static void on_tick(music_state_t* m) {
    int tick = m->current_tick;
    int beat = tick / TICKS_PER_BEAT;
    int sub = tick % TICKS_PER_BEAT;
    int bar = beat / 4;
    int beat_in_bar = beat % 4;
    int step16 = tick % 16;

    signal_params_t* sp = &m->sp;

    /* Select voicing set based on signal */
    const voicing_t* voicings = (sp->signal > SIGNAL_FRINGE) ? VOICINGS_A : VOICINGS_B;
    const int* prog = (sp->signal > SIGNAL_FRINGE) ? PROG_B : PROG_A;

    /* New bar: advance chord, rebuild patterns */
    if (step16 == 0) {
        m->prog_index = bar % PROG_LEN;
        build_chord(m->root_midi, voicings, prog[m->prog_index],
                    m->chord_notes, &m->chord_count);

        /* Rebuild Euclidean patterns from signal */
        m->arp_pattern = euclidean_rhythm(sp->arp_pulses, 8);
        m->kick_pattern = euclidean_rhythm(sp->kick_pulses, 8);
        m->hat_pattern = euclidean_rhythm(2 + (int)(sp->signal * 5), 8);
        m->rim_pattern = euclidean_rhythm(2 + (int)(sp->signal * 3), 8);

        /* Bass on root */
        if (sp->note_prob > 0.0f) {
            release_type(m, VTYPE_BASS);
            trigger_bass(m, m->chord_notes[0] - 12);
        }

        /* Pad — use wide voicing notes, not stacked */
        if (sp->pad_vol > 0.0f) {
            release_type(m, VTYPE_PAD);
            trigger_pad(m, m->chord_notes[1]);  /* 5th/wider interval */
            trigger_pad(m, m->chord_notes[2]);  /* 9th or high extension */
            if (sp->signal > SIGNAL_OPERATIONAL)
                trigger_pad(m, m->chord_notes[4]); /* top voicing note */
        }

        /* Drone */
        if ((bar % 4) == 0) {
            release_type(m, VTYPE_DRONE);
            trigger_drone(m, m->chord_notes[0]);
        }

        m->arp_pos = 0;
    }

    /* Kick — syncopated, always has weight */
    if (sub == 0 && eucl_hit(m->kick_pattern, beat_in_bar, 4)) {
        if (randf() < fmaxf(sp->note_prob, 0.3f))  /* kicks always have min probability */
            trigger_kick(m);
    }
    /* Extra offbeat kick for drive at higher signal */
    if (sp->signal > SIGNAL_FRINGE && sub == 2 && beat_in_bar == 2) {
        if (randf() < sp->note_prob * 0.5f)
            trigger_kick(m);
    }

    /* Hi-hat — Euclidean on 8th notes */
    if ((sub % 2) == 0) {
        int hat_step = beat_in_bar * 2 + sub / 2;
        if (eucl_hit(m->hat_pattern, hat_step, 8)) {
            if (randf() < sp->note_prob)
                trigger_hihat(m);
        }
    }

    /* Rim — syncopated percussive clicks, pitched from voicing */
    if ((sub % 2) == 0) {
        int rim_step = beat_in_bar * 2 + sub / 2;
        if (eucl_hit(m->rim_pattern, rim_step, 8)) {
            if (randf() < sp->note_prob) {
                int rim_idx = ((int)(randf() * (VOICING_NOTES - 0.01f))) % m->chord_count;
                trigger_rim(m, m->chord_notes[rim_idx] + 12);
            }
        }
    }

    /* Arpeggio — modern patterns with rests (-1 = skip) */
    {
        /* Pick pattern based on signal band */
        const int* arp_pat;
        int arp_len;
        if (sp->signal > SIGNAL_OPERATIONAL) {
            arp_pat = ARP_ROLLING; arp_len = ARP_ROLLING_LEN;
        } else if (sp->signal > SIGNAL_FRINGE) {
            arp_pat = ARP_SYNCO; arp_len = ARP_SYNCO_LEN;
        } else {
            arp_pat = ARP_SPARSE; arp_len = ARP_SPARSE_LEN;
        }

        if (eucl_hit(m->arp_pattern, step16 % 8, 8) && sp->arp_vol > 0.0f) {
            if (randf() < sp->note_prob) {
                int pat_val = arp_pat[m->arp_pos % arp_len];
                if (pat_val >= 0) {  /* -1 = rest, skip */
                    int note = m->chord_notes[pat_val % m->chord_count] + 12;
                    float vel = 0.12f + randf() * 0.08f;
                    if (sub == 0) vel += 0.04f;
                    /* Ghost notes on weak beats for groove */
                    if (sub == 2 || sub == 3) vel *= 0.6f;
                    trigger_arp(m, note, vel);
                }
                m->arp_pos++;
            }
        }
    }

    /* Melody — Markov, sparse, only at higher signal */
    if (sub == 0 && beat_in_bar == 0 && sp->melody_prob > 0.0f) {
        if (randf() < sp->melody_prob * 0.35f) {
            m->melody_note_idx = markov_next(m->melody_note_idx);
            int note = m->root_midi + SCALE_PENTA[m->melody_note_idx] + 24;
            trigger_melody(m, note);
        }
    }

    /* Ghost tones at very low signal — less frequent */
    if (sp->signal < SIGNAL_FRINGE && randf() < 0.02f * (1.0f - sp->signal)) {
        trigger_ghost(m);
    }
}

/* ================================================================== */
/* Per-Sample Voice Rendering                                         */
/* ================================================================== */

static float render_voice(synth_voice_t* v, float dt, float tape_mod) {
    if (!v->active) return 0.0f;

    float amp = adsr_process(&v->amp_env, dt);
    float filt_env = adsr_process(&v->filter_env, dt);

    if (v->amp_env.stage == ADSR_IDLE) { v->active = false; return 0.0f; }

    float sample = 0.0f;
    float freq_mod = v->frequency * tape_mod;

    switch (v->type) {
        case VTYPE_BASS: {
            sample = osc_sine(v->phase) * 0.93f + osc_triangle(v->phase * 2.0f) * 0.07f;
            lpf_set_freq(&v->filter, 75.0f + filt_env * 160.0f, SAMPLE_RATE);
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_ARP: {
            sample = osc_triangle(v->phase);
            lpf_set_freq(&v->filter, 250.0f + filt_env * 1000.0f, SAMPLE_RATE);
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_PAD: {
            /* Choir-like: two detuned sines + triangle for warmth, wide and distant */
            float s1 = osc_sine(v->phase) * 0.5f + osc_triangle(v->phase) * 0.5f;
            float s2 = osc_sine(v->phase2) * 0.5f + osc_triangle(v->phase2) * 0.5f;
            sample = (s1 + s2) * 0.5f;
            /* Slow filter breathing driven by filter envelope */
            lpf_set_freq(&v->filter, 140.0f + filt_env * 600.0f, SAMPLE_RATE);
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_HIHAT: {
            /* Metallic hi-hat: cluster of high sine partials, no noise */
            sample = osc_sine(v->phase) * 0.4f
                   + osc_sine(v->phase * 1.47f) * 0.3f
                   + osc_sine(v->phase * 2.09f) * 0.2f
                   + osc_sine(v->phase * 2.81f) * 0.1f;
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_KICK: {
            float pitch_env = expf(-v->amp_env.time * 35.0f);
            float kick_freq = 48.0f + 180.0f * pitch_env;
            sample = osc_sine(v->phase);
            /* Click from a fast triangle transient, not noise */
            if (v->amp_env.time < 0.003f) sample += osc_triangle(v->phase * 6.0f) * 0.15f;
            freq_mod = kick_freq * tape_mod;
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_DRONE: {
            sample = osc_sine(v->phase) * 0.7f + osc_triangle(v->phase * 0.5f) * 0.3f;
            lpf_set_freq(&v->filter, 120.0f, SAMPLE_RATE);
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_GHOST: {
            sample = osc_sine(v->phase);
            sample = lpf_process(&v->filter, sample);
            break;
        }
        case VTYPE_RIM: {
            /* Pitched triangle click with filter sweep — like a clave */
            sample = osc_triangle(v->phase) * 0.7f + osc_sine(v->phase * 3.0f) * 0.3f;
            lpf_set_freq(&v->filter, 500.0f + filt_env * 2000.0f, SAMPLE_RATE);
            sample = lpf_process(&v->filter, sample);
            break;
        }
    }

    sample *= amp * v->velocity;

    v->phase += freq_mod * dt;
    v->phase -= floorf(v->phase);
    if (v->type == VTYPE_PAD) {
        v->phase2 += (freq_mod + v->detune) * dt;
        v->phase2 -= floorf(v->phase2);
    }

    return sample;
}

/* ================================================================== */
/* Initialization                                                     */
/* ================================================================== */

static void music_init(music_state_t* m, float bpm, float signal) {
    memset(m, 0, sizeof(*m));
    m->bpm = bpm;
    m->root_midi = 45; /* A2 */
    m->samples_per_tick = (double)SAMPLE_RATE * 60.0 / ((double)bpm * TICKS_PER_BEAT);
    m->tick_accum = m->samples_per_tick; /* first tick immediately */
    m->swing = 0.12f;

    m->sp = compute_signal_params(signal);

    m->bass_lfo_rate = bpm / 60.0f * 0.25f;  /* whole-note sub swell */
    m->pump_env = 1.0f;
    m->pump_release = 6.0f;

    m->energy_rate = bpm / 60.0f / 32.0f;  /* one cycle per 32 beats (~26s at 72bpm) */
    m->sweep_rate = 0.03f;                  /* very slow sweep */

    m->tape.wow_rate = 0.8f;
    m->tape.flutter_rate = 7.5f;
    m->tape.wow_depth = m->sp.wow_depth;
    m->tape.flutter_depth = m->sp.flutter_depth;

    delay_init(&m->delay_l, bpm);
    delay_init(&m->delay_r, bpm);
    m->delay_r.delay_samples = (int)(m->delay_r.delay_samples * 1.13f);
    if (m->delay_r.delay_samples >= DELAY_BUF_SIZE) m->delay_r.delay_samples = DELAY_BUF_SIZE - 1;
    m->delay_l.wet = m->sp.delay_wet;
    m->delay_r.wet = m->sp.delay_wet;

    reverb_init(&m->reverb_l, m->sp.reverb_wet);
    reverb_init(&m->reverb_r, m->sp.reverb_wet);

    m->crush_l = (bitcrush_t){ .bits = m->sp.bit_depth, .rate_div = m->sp.sample_div };
    m->crush_r = (bitcrush_t){ .bits = m->sp.bit_depth, .rate_div = m->sp.sample_div };

    lpf_set_freq(&m->master_lpf_l, m->sp.lp_cutoff, SAMPLE_RATE);
    lpf_set_freq(&m->master_lpf_r, m->sp.lp_cutoff, SAMPLE_RATE);
    lpf_set_freq(&m->crackle_lpf, 2500.0f, SAMPLE_RATE);
    lpf_set_freq(&m->noise_lpf, 1200.0f, SAMPLE_RATE);  /* very soft hiss */

    /* Build initial chord and patterns */
    const int* scale = SCALE_PENTA;
    int scale_len = SCALE_PENTA_LEN;
    build_chord(m->root_midi, VOICINGS_B, PROG_A[0], m->chord_notes, &m->chord_count);
    m->arp_pattern = euclidean_rhythm(m->sp.arp_pulses, 8);
    m->kick_pattern = euclidean_rhythm(m->sp.kick_pulses, 8);
    m->hat_pattern = euclidean_rhythm(2 + (int)(signal * 5), 8);
    m->rim_pattern = euclidean_rhythm(2 + (int)(signal * 3), 8);
}

/* ================================================================== */
/* Main Render Loop                                                   */
/* ================================================================== */

static void music_render_samples(music_state_t* m, int16_t* out, int frames) {
    const float dt = 1.0f / (float)SAMPLE_RATE;

    for (int fi = 0; fi < frames; fi++) {
        /* Sequencer */
        m->tick_accum += 1.0;
        /* Swing: delay odd 16th notes slightly */
        double threshold = m->samples_per_tick;
        if ((m->current_tick % 2) == 1)
            threshold *= (1.0 + (double)m->swing);
        if (m->tick_accum >= threshold) {
            m->tick_accum -= threshold;
            on_tick(m);
            m->current_tick = (m->current_tick + 1) % 64;
        }

        /* Tape pitch modulation */
        float tape_mod = tape_pitch_mod(&m->tape, dt);

        /* Bass LFO — slow sub swell */
        m->bass_lfo_phase += m->bass_lfo_rate * dt;
        m->bass_lfo_phase -= floorf(m->bass_lfo_phase);
        float bass_lfo = 0.5f + 0.5f * osc_sine(m->bass_lfo_phase);

        /* Sidechain pump */
        m->pump_env += (1.0f - m->pump_env) * m->pump_release * dt;
        float pump = 0.3f + 0.7f * m->pump_env;

        /* Energy wave — slow modulation that makes the track breathe */
        m->energy_phase += m->energy_rate * dt;
        m->energy_phase -= floorf(m->energy_phase);
        float energy = 0.6f + 0.4f * osc_sine(m->energy_phase);

        /* Sweep — slow scanner texture */
        m->sweep_phase += m->sweep_rate * dt;
        m->sweep_phase -= floorf(m->sweep_phase);

        /* Render voices */
        float left = 0.0f, right = 0.0f;

        for (int vi = 0; vi < MAX_SYNTH_VOICES; vi++) {
            synth_voice_t* v = &m->voices[vi];
            if (!v->active) continue;

            float sample = render_voice(v, dt, tape_mod);

            /* Sub bass swell */
            if (v->type == VTYPE_BASS) sample *= bass_lfo;

            /* Pads breathe with energy wave */
            if (v->type == VTYPE_PAD) sample *= energy;

            /* Sidechain pump on everything except kick and rim */
            if (v->type != VTYPE_KICK && v->type != VTYPE_RIM)
                sample *= pump;

            float pan = v->pan * m->sp.stereo_width;
            float pl = sqrtf(0.5f * (1.0f - pan));
            float pr = sqrtf(0.5f * (1.0f + pan));
            left += sample * pl;
            right += sample * pr;
        }

        /* Add noise floor — soft, filtered, barely there */
        if (m->sp.noise_floor > 0.0f) {
            float n = osc_noise() * m->sp.noise_floor;
            n = lpf_process(&m->noise_lpf, n);
            left += n;
            right += n;
        }

        /* Add vinyl crackle */
        if (m->sp.crackle_density > 0.0f) {
            float crack = vinyl_crackle(m->sp.crackle_density);
            crack = lpf_process(&m->crackle_lpf, crack);
            left += crack;
            right += crack * 0.8f;
        }

        /* Bit crush */
        left = bitcrush_process(&m->crush_l, left);
        right = bitcrush_process(&m->crush_r, right);

        /* Delay */
        left = delay_process(&m->delay_l, left);
        right = delay_process(&m->delay_r, right);

        /* Reverb */
        left = reverb_process(&m->reverb_l, left);
        right = reverb_process(&m->reverb_r, right);

        /* Master LPF — modulated by sweep for cinematic movement */
        float sweep_mod = 1.0f + 0.15f * osc_sine(m->sweep_phase);
        lpf_set_freq(&m->master_lpf_l, m->sp.lp_cutoff * sweep_mod, SAMPLE_RATE);
        lpf_set_freq(&m->master_lpf_r, m->sp.lp_cutoff * sweep_mod, SAMPLE_RATE);
        left = lpf_process(&m->master_lpf_l, left);
        right = lpf_process(&m->master_lpf_r, right);

        /* Saturation */
        float drive = m->sp.saturation_drive;
        left = tanhf(left * drive);
        right = tanhf(right * drive);

        /* Master volume */
        left *= 0.60f;
        right *= 0.60f;

        /* 16-bit output */
        out[fi * 2 + 0] = (int16_t)(clampf(left, -1.0f, 1.0f) * 32767.0f);
        out[fi * 2 + 1] = (int16_t)(clampf(right, -1.0f, 1.0f) * 32767.0f);
    }
}

/* ================================================================== */
/* WAV Header                                                         */
/* ================================================================== */

static void write_wav_header(FILE* f, int sr, int ch, int frames) {
    int bps = 2;
    int data_size = frames * ch * bps;
    #define W32(val) do { uint32_t v=(uint32_t)(val); uint8_t b[4]={v&0xFF,(v>>8)&0xFF,(v>>16)&0xFF,(v>>24)&0xFF}; fwrite(b,1,4,f); } while(0)
    #define W16(val) do { uint16_t v=(uint16_t)(val); uint8_t b[2]={v&0xFF,(v>>8)&0xFF}; fwrite(b,1,2,f); } while(0)
    fwrite("RIFF",1,4,f); W32(36+data_size); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); W32(16); W16(1); W16(ch);
    W32(sr); W32(sr*ch*bps); W16(ch*bps); W16(bps*8);
    fwrite("data",1,4,f); W32(data_size);
    #undef W32
    #undef W16
}

/* ================================================================== */
/* CLI                                                                */
/* ================================================================== */

static void print_usage(const char* a) {
    fprintf(stderr,
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  -s <0.0-1.0>   Signal strength (default: 0.8)\n"
        "  -d <seconds>   Duration (default: 60)\n"
        "  -b <bpm>       Tempo (default: 72)\n"
        "  -o <file.wav>  Output WAV (default: raw PCM to stdout)\n"
        "  -h             Help\n\n"
        "Signal bands:\n"
        "  0.00-0.15  FRONTIER     static + ghost tones\n"
        "  0.15-0.50  FRINGE       bass drone + sparse arps\n"
        "  0.50-0.80  OPERATIONAL  full rhythm + melody\n"
        "  0.80-1.00  CORE         crystal clear, dense\n\n"
        "Examples:\n"
        "  %s -o core.wav -s 1.0 -d 30\n"
        "  %s -o frontier.wav -s 0.05 -d 30\n"
        "  %s -o fringe.wav -s 0.3 -d 30\n", a, a, a, a);
}

static const char* band_name(float s) {
    if (s < SIGNAL_FRONTIER) return "FRONTIER";
    if (s < SIGNAL_FRINGE) return "FRINGE";
    if (s < SIGNAL_OPERATIONAL) return "OPERATIONAL";
    return "CORE";
}

int main(int argc, char* argv[]) {
    float bpm = 72.0f, signal = 0.8f, duration = 60.0f;
    const char* output_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) { print_usage(argv[0]); return 0; }
        else if (!strcmp(argv[i],"-b") && i+1<argc) bpm = (float)atof(argv[++i]);
        else if (!strcmp(argv[i],"-s") && i+1<argc) signal = clampf((float)atof(argv[++i]),0.0f,1.0f);
        else if (!strcmp(argv[i],"-d") && i+1<argc) duration = (float)atof(argv[++i]);
        else if (!strcmp(argv[i],"-o") && i+1<argc) output_file = argv[++i];
    }
    bpm = clampf(bpm, 30.0f, 200.0f);

    static music_state_t music;
    music_init(&music, bpm, signal);

    fprintf(stderr, "Signal: %.2f [%s]  BPM: %.0f  Duration: %.0fs\n",
            signal, band_name(signal), bpm, duration);
    signal_params_t sp = music.sp;
    fprintf(stderr, "  note_prob=%.2f  bit_depth=%.1f  lp_cutoff=%.0fHz  reverb=%.2f\n",
            sp.note_prob, sp.bit_depth, sp.lp_cutoff, sp.reverb_wet);
    fprintf(stderr, "  arp_pulses=%d/8  crackle=%.2f  noise=%.3f  wow=%.4f\n",
            sp.arp_pulses, sp.crackle_density, sp.noise_floor, sp.wow_depth);

    FILE* out = stdout;
    bool is_wav = false;
    if (output_file) {
        out = fopen(output_file, "wb");
        if (!out) { fprintf(stderr, "Cannot open %s\n", output_file); return 1; }
        is_wav = true;
    }

    int64_t total_frames = (duration > 0.0f) ? (int64_t)(duration * SAMPLE_RATE) : INT64_MAX;

    if (is_wav && duration > 0.0f)
        write_wav_header(out, SAMPLE_RATE, NUM_CHANNELS, (int)total_frames);

    int64_t written = 0;
    int16_t buf[RENDER_CHUNK * NUM_CHANNELS];

    while (written < total_frames) {
        int chunk = RENDER_CHUNK;
        if (written + chunk > total_frames) chunk = (int)(total_frames - written);
        music_render_samples(&music, buf, chunk);
        size_t bytes = (size_t)(chunk * NUM_CHANNELS) * sizeof(int16_t);
        if (fwrite(buf, 1, bytes, out) != bytes) break;
        written += chunk;
    }

    if (output_file) fclose(out);
    fprintf(stderr, "Done: %.1f seconds\n", (double)written / SAMPLE_RATE);
    return 0;
}
