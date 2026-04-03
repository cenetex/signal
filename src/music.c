/* ================================================================== */
/* music.c -- Frontier synth: eerie procedural ambient for deep space */
/*                                                                    */
/* Extracted from tools/music_test.c and retuned for alien/frontier   */
/* sound.  No sokol dependencies -- just writes into a float buffer.  */
/* ================================================================== */

#include "music.h"
#include <math.h>
#include <string.h>

/* ================================================================== */
/* Constants                                                          */
/* ================================================================== */

#ifndef TWO_PI_F
#define TWO_PI_F 6.28318530718f
#endif
#ifndef PI_F
#define PI_F 3.14159265359f
#endif

/* Frontier zone signal thresholds */
#define FS_SIGNAL_FULL    0.10f   /* below this: full volume */
#define FS_SIGNAL_EDGE    0.15f   /* above FULL, below EDGE: 80% */
#define FS_SIGNAL_FADE    0.20f   /* above EDGE, below FADE: fading */

/* Frontier BPM -- very slow, irregular feel */
#define FS_BPM            50.0f

/* Frontier voicings -- dissonant, alien intervals
 * tritones, minor 2nds, perfect 5ths, augmented 4ths */
#define FS_VOICING_NOTES  5

typedef struct { int offsets[FS_VOICING_NOTES]; } fs_voicing_t;

/* Eerie quartal/tritone voicings -- almost recognizable but wrong */
static const fs_voicing_t FS_VOICINGS[] = {
    {{ 0,  6, 13, 18, 25 }},   /* root, tritone, min9+oct, aug11, ... */
    {{ 0,  5, 12, 17, 23 }},   /* root, P4, oct, P4+oct, tritone+oct */
    {{ 0,  7, 11, 18, 24 }},   /* root, P5, maj7, P5+oct, 2oct */
    {{ 0,  1, 13, 14, 25 }},   /* root, min2, min9+oct, min2+2oct, ... clusters */
};
#define FS_NUM_VOICINGS 4

/* Chord progression order -- slow cycling */
static const int FS_PROG[] = { 0, 3, 1, 2 };
#define FS_PROG_LEN 4

/* ================================================================== */
/* Inline helpers                                                     */
/* ================================================================== */

static inline float fs_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float fs_lerpf(float a, float b, float t) {
    return a + t * (b - a);
}

/* ================================================================== */
/* RNG (same LCG as audio.c)                                          */
/* ================================================================== */

static uint32_t fs_rng_next(frontier_synth_t* fs) {
    fs->rng = (fs->rng * 1664525u) + 1013904223u;
    return fs->rng;
}

static float fs_randf(frontier_synth_t* fs) {
    return (float)((fs_rng_next(fs) >> 8) & 0x00FFFFFFu) / 16777215.0f;
}

static float fs_rand_bipolar(frontier_synth_t* fs) {
    return fs_randf(fs) * 2.0f - 1.0f;
}

/* ================================================================== */
/* Oscillators                                                        */
/* ================================================================== */

static float fs_osc_sine(float phase) {
    return sinf(TWO_PI_F * phase);
}

static float fs_osc_triangle(float phase) {
    float w = phase - floorf(phase);
    return 1.0f - 4.0f * fabsf(w - 0.5f);
}

/* ================================================================== */
/* ADSR Envelope                                                      */
/* ================================================================== */

static void fs_adsr_gate_on(fs_adsr_t* e) {
    e->stage = FS_ADSR_ATTACK;
    e->time = 0.0f;
}

static void fs_adsr_gate_off(fs_adsr_t* e) {
    if (e->stage != FS_ADSR_IDLE) {
        e->stage = FS_ADSR_RELEASE;
        e->time = 0.0f;
    }
}

static float fs_adsr_process(fs_adsr_t* e, float dt) {
    e->time += dt;
    switch (e->stage) {
        case FS_ADSR_ATTACK:
            if (e->attack <= 0.0f) {
                e->level = 1.0f; e->stage = FS_ADSR_DECAY; e->time = 0.0f;
            } else {
                e->level = e->time / e->attack;
                if (e->level >= 1.0f) {
                    e->level = 1.0f; e->stage = FS_ADSR_DECAY; e->time = 0.0f;
                }
            }
            break;
        case FS_ADSR_DECAY: {
            float t = (e->decay > 0.0f) ? e->time / e->decay : 1.0f;
            if (t > 1.0f) t = 1.0f;
            e->level = 1.0f + (e->sustain - 1.0f) * t;
            if (t >= 1.0f) { e->level = e->sustain; e->stage = FS_ADSR_SUSTAIN; }
            break;
        }
        case FS_ADSR_SUSTAIN:
            e->level = e->sustain;
            break;
        case FS_ADSR_RELEASE: {
            float t = (e->release > 0.0f) ? e->time / e->release : 1.0f;
            if (t > 1.0f) t = 1.0f;
            e->level = e->sustain * (1.0f - t);
            if (t >= 1.0f) { e->level = 0.0f; e->stage = FS_ADSR_IDLE; }
            break;
        }
        default:
            e->level = 0.0f;
            break;
    }
    return e->level;
}

/* ================================================================== */
/* Low-Pass Filter (two-pole cascaded one-pole)                       */
/* ================================================================== */

static void fs_lpf_set(fs_lpf_t* f, float freq_hz, float sr) {
    float fc = freq_hz / sr;
    if (fc > 0.49f) fc = 0.49f;
    f->cutoff = 1.0f - expf(-TWO_PI_F * fc);
}

static float fs_lpf_process(fs_lpf_t* f, float in) {
    f->y1 += f->cutoff * (in - f->y1);
    f->y2 += f->cutoff * (f->y1 - f->y2);
    return f->y2;
}

/* ================================================================== */
/* High-Pass Filter (one-pole)                                        */
/* ================================================================== */

static void fs_hpf_set(fs_hpf_t* f, float freq_hz, float sr) {
    float rc = 1.0f / (TWO_PI_F * freq_hz);
    f->coeff = rc / (rc + 1.0f / sr);
}

static float fs_hpf_process(fs_hpf_t* f, float in) {
    f->prev_out = f->coeff * (f->prev_out + in - f->prev_in);
    f->prev_in = in;
    return f->prev_out;
}

/* ================================================================== */
/* Comb Filter (Schroeder reverb)                                     */
/* ================================================================== */

static float fs_comb_process(fs_comb_t* c, float in) {
    float delayed = c->buf[c->pos];
    float filtered = fs_lpf_process(&c->tone, delayed);
    c->buf[c->pos] = in + filtered * c->feedback;
    c->pos++;
    if (c->pos >= c->size) c->pos = 0;
    return delayed;
}

/* ================================================================== */
/* Allpass Filter (Schroeder reverb)                                  */
/* ================================================================== */

static float fs_allpass_process(fs_allpass_t* a, float in) {
    float delayed = a->buf[a->pos];
    float out = -in * a->gain + delayed;
    a->buf[a->pos] = in + delayed * a->gain;
    a->pos++;
    if (a->pos >= a->size) a->pos = 0;
    return out;
}

/* ================================================================== */
/* Reverb (4 combs + 2 allpasses, heavy wet for frontier)             */
/* ================================================================== */

static void fs_reverb_init(fs_reverb_t* r, float wet, float sr) {
    memset(r, 0, sizeof(*r));
    r->wet = wet;

    /* Long feedback for cavernous space reverb */
    float fb = 0.85f;

    r->combs[0].size = FS_COMB1_SIZE; r->combs[0].feedback = fb;
    r->combs[1].size = FS_COMB2_SIZE; r->combs[1].feedback = fb;
    r->combs[2].size = FS_COMB3_SIZE; r->combs[2].feedback = fb;
    r->combs[3].size = FS_COMB4_SIZE; r->combs[3].feedback = fb;

    /* Dark reverb -- low cutoff in feedback path */
    for (int i = 0; i < 4; i++)
        fs_lpf_set(&r->combs[i].tone, 1800.0f, sr);

    r->allpasses[0].size = FS_AP1_SIZE; r->allpasses[0].gain = 0.7f;
    r->allpasses[1].size = FS_AP2_SIZE; r->allpasses[1].gain = 0.7f;

    fs_hpf_set(&r->lo_cut, 180.0f, sr);
}

static float fs_reverb_process(fs_reverb_t* r, float in) {
    float sum = 0.0f;
    for (int i = 0; i < 4; i++)
        sum += fs_comb_process(&r->combs[i], in);
    sum *= 0.25f;
    sum = fs_allpass_process(&r->allpasses[0], sum);
    sum = fs_allpass_process(&r->allpasses[1], sum);
    sum = fs_hpf_process(&r->lo_cut, sum);
    return in + sum * r->wet;
}

/* ================================================================== */
/* Delay Line (long, dark, for spacious echoes)                       */
/* ================================================================== */

static void fs_delay_init(fs_delay_t* d, float bpm, float sr) {
    memset(d, 0, sizeof(*d));
    /* Dotted quarter at frontier BPM for very slow echoes */
    float beat_sec = 60.0f / bpm;
    d->delay_samples = (int)(beat_sec * 1.5f * sr);
    if (d->delay_samples >= FS_DELAY_SIZE)
        d->delay_samples = FS_DELAY_SIZE - 1;
    d->feedback = 0.50f;   /* long tail */
    d->wet = 0.35f;
    fs_lpf_set(&d->tone, 1200.0f, sr);
}

static float fs_delay_process(fs_delay_t* d, float in) {
    int rp = d->write_pos - d->delay_samples;
    if (rp < 0) rp += FS_DELAY_SIZE;
    float delayed = d->buffer[rp];
    float filtered = fs_lpf_process(&d->tone, delayed);
    d->buffer[d->write_pos] = in + filtered * d->feedback;
    d->write_pos++;
    if (d->write_pos >= FS_DELAY_SIZE) d->write_pos = 0;
    return in + delayed * d->wet;
}

/* ================================================================== */
/* Tape Wow / Flutter                                                 */
/* ================================================================== */

static float fs_tape_pitch_mod(fs_tape_t* t, float dt) {
    t->wow_phase += t->wow_rate * dt;
    t->wow_phase -= floorf(t->wow_phase);
    t->flutter_phase += t->flutter_rate * dt;
    t->flutter_phase -= floorf(t->flutter_phase);
    return 1.0f + fs_osc_sine(t->wow_phase) * t->wow_depth
                + fs_osc_sine(t->flutter_phase) * t->flutter_depth;
}

/* ================================================================== */
/* Music Theory                                                       */
/* ================================================================== */

static float fs_midi_to_freq(int note) {
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

/* Euclidean rhythm generator */
static uint32_t fs_euclidean(int pulses, int steps) {
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

static bool fs_eucl_hit(uint32_t pattern, int step, int total) {
    return (pattern >> (step % total)) & 1;
}

static void fs_build_chord(int root_midi, int prog_idx, int* notes, int* count) {
    *count = FS_VOICING_NOTES;
    const fs_voicing_t* v = &FS_VOICINGS[prog_idx % FS_NUM_VOICINGS];
    for (int i = 0; i < FS_VOICING_NOTES; i++)
        notes[i] = root_midi + v->offsets[i];
}

/* ================================================================== */
/* Voice Allocation                                                   */
/* ================================================================== */

static fs_synth_voice_t* fs_voice_alloc(frontier_synth_t* fs, fs_voice_type_t type) {
    /* Find free slot */
    for (int i = 0; i < FS_MAX_VOICES; i++) {
        if (!fs->voices[i].active) {
            memset(&fs->voices[i], 0, sizeof(fs_synth_voice_t));
            fs->voices[i].type = type;
            return &fs->voices[i];
        }
    }
    /* Steal a releasing voice of same type */
    for (int i = 0; i < FS_MAX_VOICES; i++) {
        if (fs->voices[i].type == type &&
            fs->voices[i].amp_env.stage == FS_ADSR_RELEASE) {
            memset(&fs->voices[i], 0, sizeof(fs_synth_voice_t));
            fs->voices[i].type = type;
            return &fs->voices[i];
        }
    }
    return NULL;
}

static void fs_release_type(frontier_synth_t* fs, fs_voice_type_t type) {
    for (int i = 0; i < FS_MAX_VOICES; i++) {
        if (fs->voices[i].active && fs->voices[i].type == type)
            fs_adsr_gate_off(&fs->voices[i].amp_env);
    }
}

/* ================================================================== */
/* Note Triggers -- tuned for eerie, alien frontier sound             */
/* ================================================================== */

/* Drone: very slow evolving sine, heavy pitch drift */
static void fs_trigger_drone(frontier_synth_t* fs, int midi) {
    fs_synth_voice_t* v = fs_voice_alloc(fs, FS_VTYPE_DRONE);
    if (!v) return;
    v->active = true;
    v->frequency = fs_midi_to_freq(midi);
    v->detune = v->frequency * 0.015f; /* wide detune for beating */
    v->velocity = 0.12f;
    v->pan = fs_rand_bipolar(fs) * 0.3f;
    /* Glacial attack and release */
    v->amp_env = (fs_adsr_t){
        .attack = 6.0f, .decay = 4.0f, .sustain = 0.5f, .release = 8.0f
    };
    v->filter_env = (fs_adsr_t){
        .attack = 5.0f, .decay = 6.0f, .sustain = 0.2f, .release = 5.0f
    };
    fs_lpf_set(&v->filter, 100.0f, (float)fs->sample_rate);
    fs_adsr_gate_on(&v->amp_env);
    fs_adsr_gate_on(&v->filter_env);
}

/* Ghost: random radio blip, unsettling pitch */
static void fs_trigger_ghost(frontier_synth_t* fs) {
    fs_synth_voice_t* v = fs_voice_alloc(fs, FS_VTYPE_GHOST);
    if (!v) return;
    v->active = true;
    /* Random pitch in eerie range -- deliberately non-musical */
    v->frequency = 120.0f + fs_randf(fs) * 3000.0f;
    v->velocity = 0.02f + fs_randf(fs) * 0.04f;
    v->pan = fs_rand_bipolar(fs) * 0.9f;
    /* Short blip that fades in eerily */
    v->amp_env = (fs_adsr_t){
        .attack = 0.3f + fs_randf(fs) * 1.5f,
        .decay = 0.2f,
        .sustain = 0.0f,
        .release = 0.5f + fs_randf(fs) * 2.0f
    };
    fs_lpf_set(&v->filter, 800.0f + fs_randf(fs) * 1500.0f, (float)fs->sample_rate);
    fs_adsr_gate_on(&v->amp_env);
}

/* Pad: wide detuned sine drones, dissonant intervals */
static void fs_trigger_pad(frontier_synth_t* fs, int midi) {
    fs_synth_voice_t* v = fs_voice_alloc(fs, FS_VTYPE_PAD);
    if (!v) return;
    v->active = true;
    v->frequency = fs_midi_to_freq(midi);
    v->detune = v->frequency * 0.012f; /* wide chorus beating */
    v->velocity = 0.06f;
    v->pan = fs_rand_bipolar(fs) * 0.7f;
    /* Very slow evolving pad */
    v->amp_env = (fs_adsr_t){
        .attack = 5.0f, .decay = 3.0f, .sustain = 0.35f, .release = 6.0f
    };
    v->filter_env = (fs_adsr_t){
        .attack = 6.0f, .decay = 4.0f, .sustain = 0.1f, .release = 4.0f
    };
    fs_lpf_set(&v->filter, 120.0f, (float)fs->sample_rate);
    fs_adsr_gate_on(&v->amp_env);
    fs_adsr_gate_on(&v->filter_env);
}

/* Rim: faint metallic ping -- like distant station debris */
static void fs_trigger_rim(frontier_synth_t* fs, int midi) {
    fs_synth_voice_t* v = fs_voice_alloc(fs, FS_VTYPE_RIM);
    if (!v) return;
    v->active = true;
    v->frequency = fs_midi_to_freq(midi);
    v->velocity = 0.04f + fs_randf(fs) * 0.03f;
    v->pan = fs_rand_bipolar(fs) * 0.6f;
    /* Very short metallic click */
    v->amp_env = (fs_adsr_t){
        .attack = 0.0005f, .decay = 0.06f, .sustain = 0.0f, .release = 0.04f
    };
    v->filter_env = (fs_adsr_t){
        .attack = 0.0005f, .decay = 0.08f, .sustain = 0.0f, .release = 0.03f
    };
    fs_lpf_set(&v->filter, 600.0f, (float)fs->sample_rate);
    fs_adsr_gate_on(&v->amp_env);
    fs_adsr_gate_on(&v->filter_env);
}

/* Sub bass: deep rumbling drone at the bottom */
static void fs_trigger_bass(frontier_synth_t* fs, int midi) {
    fs_synth_voice_t* v = fs_voice_alloc(fs, FS_VTYPE_BASS);
    if (!v) return;
    v->active = true;
    v->frequency = fs_midi_to_freq(midi);
    v->velocity = 0.08f;
    /* Very slow swell */
    v->amp_env = (fs_adsr_t){
        .attack = 4.0f, .decay = 3.0f, .sustain = 0.4f, .release = 5.0f
    };
    v->filter_env = (fs_adsr_t){
        .attack = 3.0f, .decay = 4.0f, .sustain = 0.1f, .release = 3.0f
    };
    fs_lpf_set(&v->filter, 60.0f, (float)fs->sample_rate);
    fs_adsr_gate_on(&v->amp_env);
    fs_adsr_gate_on(&v->filter_env);
}

/* ================================================================== */
/* Sequencer Tick -- sparse, irregular, alien                         */
/* ================================================================== */

static void fs_on_tick(frontier_synth_t* fs) {
    int tick = fs->current_tick;
    int beat = tick / FS_TICKS_PER_BEAT;
    int sub = tick % FS_TICKS_PER_BEAT;
    int bar = beat / 4;
    int step16 = tick % 16;

    /* New bar: advance chord */
    if (step16 == 0) {
        fs->prog_index = bar % FS_PROG_LEN;
        fs_build_chord(fs->root_midi, FS_PROG[fs->prog_index],
                       fs->chord_notes, &fs->chord_count);

        /* Rebuild rim pattern -- sparse: 1-2 hits per 8 steps */
        fs->rim_pattern = fs_euclidean(1 + (int)(fs_randf(fs) * 1.5f), 8);

        /* Drone on root every 2 bars */
        if ((bar % 2) == 0) {
            fs_release_type(fs, FS_VTYPE_DRONE);
            fs_trigger_drone(fs, fs->chord_notes[0]);
        }

        /* Sub bass every 4 bars */
        if ((bar % 4) == 0) {
            fs_release_type(fs, FS_VTYPE_BASS);
            fs_trigger_bass(fs, fs->chord_notes[0] - 12);
        }

        /* Pad voices -- use the dissonant voicing notes */
        if ((bar % 2) == 1) {
            fs_release_type(fs, FS_VTYPE_PAD);
            /* Pick two dissonant notes from the voicing */
            int idx1 = 1 + (int)(fs_randf(fs) * 3.99f);
            int idx2 = 1 + (int)(fs_randf(fs) * 3.99f);
            if (idx2 == idx1) idx2 = (idx1 % 4) + 1;
            fs_trigger_pad(fs, fs->chord_notes[idx1 % fs->chord_count]);
            fs_trigger_pad(fs, fs->chord_notes[idx2 % fs->chord_count]);
        }
    }

    /* Rim clicks -- very sparse, metallic pings through the void */
    if ((sub == 0) && fs_eucl_hit(fs->rim_pattern, beat % 8, 8)) {
        if (fs_randf(fs) < 0.25f) {
            int rim_idx = (int)(fs_randf(fs) * (float)(fs->chord_count - 0.01f));
            fs_trigger_rim(fs, fs->chord_notes[rim_idx] + 24);
        }
    }
}

/* ================================================================== */
/* Per-Sample Voice Rendering                                         */
/* ================================================================== */

static float fs_render_voice(fs_synth_voice_t* v, float dt, float tape_mod, float sr) {
    if (!v->active) return 0.0f;

    float amp = fs_adsr_process(&v->amp_env, dt);
    float filt_env = fs_adsr_process(&v->filter_env, dt);

    if (v->amp_env.stage == FS_ADSR_IDLE) {
        v->active = false;
        return 0.0f;
    }

    float sample = 0.0f;
    float freq_mod = v->frequency * tape_mod;

    switch (v->type) {
        case FS_VTYPE_DRONE: {
            /* Two detuned sines + slow triangle undertone */
            float s1 = fs_osc_sine(v->phase) * 0.6f;
            float s2 = fs_osc_sine(v->phase2) * 0.4f;
            sample = s1 + s2;
            fs_lpf_set(&v->filter, 80.0f + filt_env * 200.0f, sr);
            sample = fs_lpf_process(&v->filter, sample);
            /* Update detuned oscillator */
            v->phase2 += (freq_mod + v->detune) * dt;
            v->phase2 -= floorf(v->phase2);
            break;
        }
        case FS_VTYPE_GHOST: {
            /* Pure sine -- eerie radio blip */
            sample = fs_osc_sine(v->phase);
            sample = fs_lpf_process(&v->filter, sample);
            break;
        }
        case FS_VTYPE_PAD: {
            /* Two detuned sines for wide beating chorus */
            float s1 = fs_osc_sine(v->phase) * 0.5f
                      + fs_osc_triangle(v->phase) * 0.5f;
            float s2 = fs_osc_sine(v->phase2) * 0.5f
                      + fs_osc_triangle(v->phase2) * 0.5f;
            sample = (s1 + s2) * 0.5f;
            fs_lpf_set(&v->filter, 100.0f + filt_env * 400.0f, sr);
            sample = fs_lpf_process(&v->filter, sample);
            /* Update detuned oscillator */
            v->phase2 += (freq_mod + v->detune) * dt;
            v->phase2 -= floorf(v->phase2);
            break;
        }
        case FS_VTYPE_RIM: {
            /* Metallic ping: triangle + inharmonic sine partial */
            sample = fs_osc_triangle(v->phase) * 0.6f
                   + fs_osc_sine(v->phase * 3.07f) * 0.3f
                   + fs_osc_sine(v->phase * 5.43f) * 0.1f;
            fs_lpf_set(&v->filter, 400.0f + filt_env * 3000.0f, sr);
            sample = fs_lpf_process(&v->filter, sample);
            break;
        }
        case FS_VTYPE_BASS: {
            /* Deep sine sub with slight harmonic */
            sample = fs_osc_sine(v->phase) * 0.95f
                   + fs_osc_triangle(v->phase * 2.0f) * 0.05f;
            fs_lpf_set(&v->filter, 50.0f + filt_env * 80.0f, sr);
            sample = fs_lpf_process(&v->filter, sample);
            break;
        }
    }

    sample *= amp * v->velocity;

    v->phase += freq_mod * dt;
    v->phase -= floorf(v->phase);

    return sample;
}

/* ================================================================== */
/* Signal-to-gain mapping                                             */
/* ================================================================== */

static float fs_compute_gain(float signal) {
    if (signal >= FS_SIGNAL_FADE) return 0.0f;
    if (signal >= FS_SIGNAL_EDGE) {
        /* 0.15 -> 0.20: fade from 0.80 to 0.0 */
        float t = (signal - FS_SIGNAL_EDGE) / (FS_SIGNAL_FADE - FS_SIGNAL_EDGE);
        return 0.80f * (1.0f - t);
    }
    if (signal >= FS_SIGNAL_FULL) {
        /* 0.10 -> 0.15: constant 0.80 */
        return 0.80f;
    }
    /* 0.00 -> 0.10: full volume */
    return 1.0f;
}

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */

void frontier_synth_init(frontier_synth_t* fs, int sample_rate) {
    memset(fs, 0, sizeof(*fs));
    fs->sample_rate = sample_rate > 0 ? sample_rate : FS_SAMPLE_RATE;
    fs->rng = 0xDEAD5167u;
    fs->signal = 1.0f;   /* start silent (in signal coverage) */
    fs->master_gain = 0.0f;

    fs->bpm = FS_BPM;
    float sr = (float)fs->sample_rate;
    fs->samples_per_tick = (double)fs->sample_rate * 60.0
                         / ((double)fs->bpm * FS_TICKS_PER_BEAT);
    fs->tick_accum = fs->samples_per_tick; /* trigger first tick immediately */

    /* A1 root -- low, ominous */
    fs->root_midi = 33;

    /* Very slow energy wave */
    fs->energy_rate = fs->bpm / 60.0f / 64.0f; /* one cycle per 64 beats */
    fs->sweep_rate = 0.015f; /* very slow sweep */

    /* Heavy wow/flutter for pitch drift */
    fs->tape.wow_rate = 0.4f;
    fs->tape.flutter_rate = 3.5f;
    fs->tape.wow_depth = 0.008f;   /* heavy drift */
    fs->tape.flutter_depth = 0.003f;

    /* Ghost tone timer -- irregular */
    fs->ghost_timer = 0.0f;
    fs->ghost_interval = 2.0f + fs_randf(fs) * 6.0f;

    /* Delay: long dark echoes */
    fs_delay_init(&fs->delay_l, fs->bpm, sr);
    fs_delay_init(&fs->delay_r, fs->bpm, sr);
    /* Offset right delay for stereo width */
    fs->delay_r.delay_samples = (int)((float)fs->delay_r.delay_samples * 1.17f);
    if (fs->delay_r.delay_samples >= FS_DELAY_SIZE)
        fs->delay_r.delay_samples = FS_DELAY_SIZE - 1;

    /* Heavy reverb */
    fs_reverb_init(&fs->reverb_l, 0.65f, sr);
    fs_reverb_init(&fs->reverb_r, 0.65f, sr);

    /* Dark master filter */
    fs_lpf_set(&fs->master_lpf_l, 2500.0f, sr);
    fs_lpf_set(&fs->master_lpf_r, 2500.0f, sr);
    fs_lpf_set(&fs->noise_lpf, 600.0f, sr);

    /* Build initial chord */
    fs_build_chord(fs->root_midi, FS_PROG[0], fs->chord_notes, &fs->chord_count);
    fs->rim_pattern = fs_euclidean(1, 8);

    fs->initialized = true;
}

void frontier_synth_set_signal(frontier_synth_t* fs, float signal) {
    fs->signal = fs_clampf(signal, 0.0f, 1.0f);
    fs->master_gain = fs_compute_gain(fs->signal);
}

void frontier_synth_render(frontier_synth_t* fs, float* mix_buffer,
                           int frames, int channels) {
    if (!fs->initialized || fs->master_gain <= 0.0001f) return;
    if (channels < 1 || channels > 2) return;

    float sr = (float)fs->sample_rate;
    float dt = 1.0f / sr;
    float gain = fs->master_gain;

    for (int fi = 0; fi < frames; fi++) {
        /* ---- Sequencer ---- */
        fs->tick_accum += 1.0;
        if (fs->tick_accum >= fs->samples_per_tick) {
            fs->tick_accum -= fs->samples_per_tick;
            fs_on_tick(fs);
            fs->current_tick = (fs->current_tick + 1) % 64;
        }

        /* ---- Ghost tone timer (irregular, outside sequencer) ---- */
        fs->ghost_timer += dt;
        if (fs->ghost_timer >= fs->ghost_interval) {
            fs->ghost_timer = 0.0f;
            fs->ghost_interval = 1.5f + fs_randf(fs) * 8.0f;
            /* More ghosts in deep frontier */
            float ghost_prob = (fs->signal < 0.05f) ? 0.7f : 0.3f;
            if (fs_randf(fs) < ghost_prob)
                fs_trigger_ghost(fs);
        }

        /* ---- Tape pitch modulation ---- */
        float tape_mod = fs_tape_pitch_mod(&fs->tape, dt);

        /* ---- Energy wave (slow breathing) ---- */
        fs->energy_phase += fs->energy_rate * dt;
        fs->energy_phase -= floorf(fs->energy_phase);
        float energy = 0.5f + 0.5f * fs_osc_sine(fs->energy_phase);

        /* ---- Sweep LFO for filter modulation ---- */
        fs->sweep_phase += fs->sweep_rate * dt;
        fs->sweep_phase -= floorf(fs->sweep_phase);

        /* ---- Drone LFO (sub swell) ---- */
        fs->drone_lfo_phase += (fs->bpm / 60.0f * 0.125f) * dt;
        fs->drone_lfo_phase -= floorf(fs->drone_lfo_phase);
        float drone_lfo = 0.4f + 0.6f * fs_osc_sine(fs->drone_lfo_phase);

        /* ---- Render voices ---- */
        float left = 0.0f, right = 0.0f;

        for (int vi = 0; vi < FS_MAX_VOICES; vi++) {
            fs_synth_voice_t* v = &fs->voices[vi];
            if (!v->active) continue;

            float sample = fs_render_voice(v, dt, tape_mod, sr);

            /* Drones swell with LFO */
            if (v->type == FS_VTYPE_DRONE || v->type == FS_VTYPE_BASS)
                sample *= drone_lfo;

            /* Pads breathe with energy wave */
            if (v->type == FS_VTYPE_PAD)
                sample *= energy;

            float pan = v->pan;
            float pl = sqrtf(0.5f * (1.0f - pan));
            float pr = sqrtf(0.5f * (1.0f + pan));
            left += sample * pl;
            right += sample * pr;
        }

        /* ---- Subtle noise floor ---- */
        {
            float n = fs_rand_bipolar(fs) * 0.0008f;
            n = fs_lpf_process(&fs->noise_lpf, n);
            left += n;
            right += n;
        }

        /* ---- Delay ---- */
        left = fs_delay_process(&fs->delay_l, left);
        right = fs_delay_process(&fs->delay_r, right);

        /* ---- Reverb ---- */
        left = fs_reverb_process(&fs->reverb_l, left);
        right = fs_reverb_process(&fs->reverb_r, right);

        /* ---- Master LPF with slow sweep ---- */
        float sweep_mod = 1.0f + 0.2f * fs_osc_sine(fs->sweep_phase);
        fs_lpf_set(&fs->master_lpf_l, 2500.0f * sweep_mod, sr);
        fs_lpf_set(&fs->master_lpf_r, 2500.0f * sweep_mod, sr);
        left = fs_lpf_process(&fs->master_lpf_l, left);
        right = fs_lpf_process(&fs->master_lpf_r, right);

        /* ---- Soft saturation ---- */
        left = tanhf(left * 1.8f);
        right = tanhf(right * 1.8f);

        /* ---- Apply master gain and ADD to mix buffer ---- */
        float final_gain = gain * 0.45f;
        if (channels == 1) {
            mix_buffer[fi] += (left + right) * 0.5f * final_gain;
        } else {
            int base = fi * channels;
            mix_buffer[base + 0] += left * final_gain;
            mix_buffer[base + 1] += right * final_gain;
        }
    }
}
