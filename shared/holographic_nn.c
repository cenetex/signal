/*
 * holographic_nn.c — VSA primitives and holographic associative memory
 * for ship pilot neural networks.
 *
 * Key design decisions:
 *   - HNN_DIM = 1024 for power-of-2 FFT efficiency
 *   - float precision throughout (fast on GPU-less server, good fidelity)
 *   - Deterministic key vectors from seed via frequency-domain construction
 *   - In-place radix-2 Cooley-Tukey FFT
 */
#include "holographic_nn.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* A tiny PRNG for deterministic key generation (xorshift32). */
static uint32_t hnn_rand_state;

static void hnn_srand(uint32_t seed) {
    hnn_rand_state = seed ? seed : 1u;
}

static uint32_t hnn_rand(void) {
    uint32_t x = hnn_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    hnn_rand_state = x;
    return x;
}

static float hnn_randf(void) {
    return (float)(hnn_rand() & 0xFFFFFFu) / (float)0x1000000;
}

/* --- FFT (radix-2 Cooley-Tukey, in-place, complex interleaved) --- */

/*
 * Bit-reversal permutation for radix-2 FFT.
 * `data` is [re0, im0, re1, im1, ...].
 */
static void hnn_bit_reverse(float *data, int n) {
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int j = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) j |= 1 << (bits - 1 - b);
        if (j > i) {
            float tr = data[2 * i], ti = data[2 * i + 1];
            data[2 * i]     = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j]     = tr;
            data[2 * j + 1] = ti;
        }
    }
}

static void hnn_fft(float *data, int n) {
    hnn_bit_reverse(data, n);
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * 3.14159265358979323846f / (float)len;
        float w_re = cosf(angle);
        float w_im = sinf(angle);
        for (int i = 0; i < n; i += len) {
            float cur_re = 1.0f, cur_im = 0.0f;
            int half = len >> 1;
            for (int j = 0; j < half; j++) {
                int a_idx = 2 * (i + j);
                int b_idx = 2 * (i + j + half);
                float u_re = data[a_idx];
                float u_im = data[a_idx + 1];
                float v_re = cur_re * data[b_idx] - cur_im * data[b_idx + 1];
                float v_im = cur_re * data[b_idx + 1] + cur_im * data[b_idx];
                data[a_idx]     = u_re + v_re;
                data[a_idx + 1] = u_im + v_im;
                data[b_idx]     = u_re - v_re;
                data[b_idx + 1] = u_im - v_im;
                float next_re = cur_re * w_re - cur_im * w_im;
                float next_im = cur_re * w_im + cur_im * w_re;
                cur_re = next_re;
                cur_im = next_im;
            }
        }
    }
}

static void hnn_ifft(float *data, int n) {
    /* Conjugate, FFT, conjugate, scale */
    for (int i = 0; i < n; i++) data[2 * i + 1] = -data[2 * i + 1];
    hnn_fft(data, n);
    for (int i = 0; i < n; i++) {
        data[2 * i + 1] = -data[2 * i + 1];
        float scale = 1.0f / (float)n;
        data[2 * i]     *= scale;
        data[2 * i + 1] *= scale;
    }
}

/* --- Core VSA operations --- */

float hnn_normalize(float v[HNN_DIM]) {
    float sum_sq = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) sum_sq += v[i] * v[i];
    if (sum_sq < 1e-30f) return 0.0f;
    float inv_norm = 1.0f / sqrtf(sum_sq);
    for (int i = 0; i < HNN_DIM; i++) v[i] *= inv_norm;
    return 1.0f / inv_norm;
}

void hnn_key_vector(uint64_t seed, float out[HNN_DIM]) {
    int half = HNN_DIM / 2;

    /* Split the 64-bit seed into two 32-bit halves for the PRNG */
    uint32_t lo = (uint32_t)(seed & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)((seed >> 32) & 0xFFFFFFFFu);
    hnn_srand(lo ^ (hi * 2654435761u));

    /* Build in frequency domain: unit magnitude at every bin, random phase. */
    float *F = (float *)malloc((size_t)HNN_DIM * 2u * sizeof(float));
    if (!F) {
        memset(out, 0, HNN_DIM * sizeof(float));
        return;
    }

    /* DC: +/-1 */
    F[0] = hnn_randf() < 0.5f ? 1.0f : -1.0f;
    F[1] = 0.0f;

    for (int i = 1; i < half; i++) {
        float phase = hnn_randf() * 2.0f * 3.14159265358979323846f;
        float a_re = cosf(phase);
        float a_im = sinf(phase);
        F[2 * i]             = a_re;
        F[2 * i + 1]         = a_im;
        F[2 * (HNN_DIM - i)]     = a_re;
        F[2 * (HNN_DIM - i) + 1] = -a_im;  /* conjugate symmetry */
    }

    if (HNN_DIM % 2 == 0) {
        /* Nyquist bin: real, +/-1 */
        F[2 * half]     = hnn_randf() < 0.5f ? 1.0f : -1.0f;
        F[2 * half + 1] = 0.0f;
    }

    /* IFFT to time domain */
    hnn_ifft(F, HNN_DIM);

    /* Copy real parts to output */
    for (int i = 0; i < HNN_DIM; i++) out[i] = F[2 * i];

    free(F);

    /* Normalize onto the hypersphere */
    hnn_normalize(out);
}

void hnn_bind(const float a[HNN_DIM], const float b[HNN_DIM],
              float c[HNN_DIM]) {
    float *work = (float *)malloc((size_t)HNN_DIM * 2u * sizeof(float));
    if (!work) { memset(c, 0, HNN_DIM * sizeof(float)); return; }

    /* Pack a into complex array and FFT */
    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i]     = a[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_fft(work, HNN_DIM);

    /* Store FFT(a) */
    float *Fa = (float *)malloc((size_t)HNN_DIM * 2u * sizeof(float));
    if (!Fa) { free(work); memset(c, 0, HNN_DIM * sizeof(float)); return; }
    memcpy(Fa, work, (size_t)HNN_DIM * 2u * sizeof(float));

    /* Pack b into work and FFT */
    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i]     = b[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_fft(work, HNN_DIM);

    /* Multiply: Fa * work (element-wise complex multiply) */
    for (int i = 0; i < HNN_DIM; i++) {
        float a_re = Fa[2 * i], a_im = Fa[2 * i + 1];
        float b_re = work[2 * i], b_im = work[2 * i + 1];
        work[2 * i]     = a_re * b_re - a_im * b_im;
        work[2 * i + 1] = a_re * b_im + a_im * b_re;
    }
    free(Fa);

    /* IFFT */
    hnn_ifft(work, HNN_DIM);

    /* Copy real parts to output */
    for (int i = 0; i < HNN_DIM; i++) c[i] = work[2 * i];
    free(work);

    hnn_normalize(c);
}

void hnn_unbind(const float a[HNN_DIM], const float b[HNN_DIM],
                float c[HNN_DIM]) {
    float *work = (float *)malloc((size_t)HNN_DIM * 2u * sizeof(float));
    if (!work) { memset(c, 0, HNN_DIM * sizeof(float)); return; }

    /* FFT a */
    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i]     = a[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_fft(work, HNN_DIM);

    /* Store FFT(a) */
    float *Fa = (float *)malloc((size_t)HNN_DIM * 2u * sizeof(float));
    if (!Fa) { free(work); memset(c, 0, HNN_DIM * sizeof(float)); return; }
    memcpy(Fa, work, (size_t)HNN_DIM * 2u * sizeof(float));

    /* FFT b into work */
    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i]     = b[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_fft(work, HNN_DIM);

    /* Multiply Fa * conj(work) — correlation via conjugate in freq domain */
    for (int i = 0; i < HNN_DIM; i++) {
        float a_re = Fa[2 * i], a_im = Fa[2 * i + 1];
        float b_re = work[2 * i], b_im = -work[2 * i + 1];
        work[2 * i]     = a_re * b_re - a_im * b_im;
        work[2 * i + 1] = a_re * b_im + a_im * b_re;
    }
    free(Fa);

    hnn_ifft(work, HNN_DIM);

    for (int i = 0; i < HNN_DIM; i++) c[i] = work[2 * i];
    free(work);

    hnn_normalize(c);
}

void hnn_bundle(float a[HNN_DIM], const float b[HNN_DIM]) {
    for (int i = 0; i < HNN_DIM; i++) a[i] += b[i];
    hnn_normalize(a);
}

float hnn_similarity(const float a[HNN_DIM], const float b[HNN_DIM]) {
    float dot = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) dot += a[i] * b[i];
    return dot;
}

/* --- Holographic memory --- */

void hnn_memory_init(hnn_memory_t *mem) {
    memset(mem->store, 0, HNN_DIM * sizeof(float));
    mem->experience_count = 0;
    mem->last_retrieval_similarity = 0.0f;
}

void hnn_memory_store(hnn_memory_t *mem,
                      const float key[HNN_DIM],
                      const float value[HNN_DIM]) {
    float pair[HNN_DIM];
    hnn_bind(key, value, pair);
    for (int i = 0; i < HNN_DIM; i++) mem->store[i] += pair[i];
    mem->experience_count++;
    hnn_normalize(mem->store);
}

void hnn_memory_retrieve(const hnn_memory_t *mem,
                         const float key[HNN_DIM],
                         float value_out[HNN_DIM]) {
    if (mem->experience_count == 0) {
        memset(value_out, 0, HNN_DIM * sizeof(float));
        return;
    }
    float M_norm[HNN_DIM];
    memcpy(M_norm, mem->store, HNN_DIM * sizeof(float));
    hnn_normalize(M_norm);
    hnn_unbind(M_norm, key, value_out);
}

void hnn_memory_cleanup(const hnn_memory_t *mem,
                        const float key[HNN_DIM],
                        float value_out[HNN_DIM],
                        int steps) {
    if (mem->experience_count == 0) {
        memset(value_out, 0, HNN_DIM * sizeof(float));
        return;
    }

    hnn_memory_retrieve(mem, key, value_out);

    float M_norm[HNN_DIM];
    memcpy(M_norm, mem->store, HNN_DIM * sizeof(float));
    hnn_normalize(M_norm);

    for (int s = 0; s < steps; s++) {
        float contrib[HNN_DIM];
        hnn_bind(key, value_out, contrib);

        float residual[HNN_DIM];
        for (int i = 0; i < HNN_DIM; i++)
            residual[i] = M_norm[i] - contrib[i];
        hnn_normalize(residual);

        float noise[HNN_DIM];
        hnn_unbind(residual, key, noise);

        for (int i = 0; i < HNN_DIM; i++)
            value_out[i] += 0.5f * noise[i];
        hnn_normalize(value_out);
    }
}

/* --- Pilot feature encoding --- */

void hnn_encode_state(const hnn_pilot_features_t *f,
                      float state_out[HNN_DIM]) {
    float vals[HNN_FEATURE_COUNT];
    vals[0]  = f->target_dist;
    vals[1]  = f->heading_error;
    vals[2]  = f->heading_cos;
    vals[3]  = f->heading_sin;
    vals[4]  = f->speed;
    vals[5]  = f->forward_speed;
    vals[6]  = f->lateral_speed;
    vals[7]  = f->brake_distance;
    vals[8]  = f->fwd_clear;
    vals[9]  = f->left_clear;
    vals[10] = f->right_clear;
    vals[11] = f->signal_quality;
    vals[12] = f->hull_ratio;
    vals[13] = f->path_count;
    vals[14] = f->path_current;
    vals[15] = f->fwd_blocked;
    vals[16] = f->left_blocked;
    vals[17] = f->right_blocked;
    vals[18] = f->goal_close;
    vals[19] = f->action_delta_turn;
    vals[20] = f->action_delta_thrust;
    vals[21] = f->action_is_none;
    vals[22] = f->action_is_reverse;
    vals[23] = f->composite_dot;

    memset(state_out, 0, HNN_DIM * sizeof(float));

    for (int i = 0; i < HNN_FEATURE_COUNT; i++) {
        float feat_key[HNN_DIM];
        hnn_key_vector((uint64_t)(i + 1000), feat_key);

        /* Scale the key vector by the feature value */
        float feat_val[HNN_DIM];
        for (int j = 0; j < HNN_DIM; j++)
            feat_val[j] = feat_key[j] * vals[i];
        hnn_normalize(feat_val);

        for (int j = 0; j < HNN_DIM; j++)
            state_out[j] += feat_val[j];
    }
    hnn_normalize(state_out);
}

/* --- Action table --- */

void hnn_action_table_init(hnn_action_table_t *table) {
    /*
     * Action 0 = NONE (0,0), 1 = W (0,1), 2 = A (-1,0), 3 = D (1,0),
     * 4 = S (0,-1), 5 = WA (-1,1), 6 = WD (1,1), 7 = SA (-1,-1), 8 = SD (1,-1)
     */
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        hnn_key_vector((uint64_t)(i + 2000), table->vecs[i]);
    }
}

int hnn_select_action(const hnn_memory_t *mem,
                      const hnn_action_table_t *actions,
                      const hnn_pilot_features_t *state,
                      float *out_confidence) {
    float state_vec[HNN_DIM];
    hnn_encode_state(state, state_vec);

    float retrieved[HNN_DIM];
    hnn_memory_cleanup(mem, state_vec, retrieved, 3);

    int best_idx = 0;
    float best_sim = -2.0f;

    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        float sim = hnn_similarity(retrieved, actions->vecs[i]);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }

    if (out_confidence) *out_confidence = best_sim;
    return best_idx;
}

