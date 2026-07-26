/*
 * holographic_nn.c — VSA primitives and holographic associative memory
 * for ship pilot neural networks.
 *
 * Key design decisions:
 *   - HNN_DIM = 1024 for power-of-2 FFT efficiency
 *   - float precision throughout (fast on GPU-less server, good fidelity)
 *   - Deterministic key vectors from seed via frequency-domain construction
 *   - In-place radix-2 Cooley-Tukey FFT
 *   - No heap allocation in bind/unbind/keygen hot paths
 */
#include "holographic_nn.h"
#include "fixpoint.h"

#include <math.h>
#include <string.h>

#define HNN_COMPLEX_COUNT ((size_t)HNN_DIM * 2u)
#define HNN_KEY_CACHE_SIZE 64u
#define HNN_HOLONET_NEW_CELL_SIMILARITY 0.70f

typedef struct {
    uint64_t seed;
    bool valid;
    float vec[HNN_DIM];
} hnn_key_cache_entry_t;

/* A tiny PRNG for deterministic key generation (xorshift32). */
static uint32_t hnn_rand_state;
static hnn_key_cache_entry_t g_hnn_key_cache[HNN_KEY_CACHE_SIZE];
static float g_hnn_feature_value_keys
    [HNN_FEATURE_COUNT][HNN_FEATURE_VALUE_LEVELS][HNN_DIM];
static bool g_hnn_feature_value_keys_initialized = false;

typedef struct {
    const char *name;
    int turn;
    int thrust;
} hnn_action_vocab_entry_t;

static const hnn_action_vocab_entry_t HNN_ACTION_VOCAB[HNN_ACTION_COUNT] = {
    {"NONE", 0, 0},
    {"W", 0, 1},
    {"A", -1, 0},
    {"D", 1, 0},
    {"S", 0, -1},
    {"WA", -1, 1},
    {"WD", 1, 1},
    {"SA", -1, -1},
    {"SD", 1, -1},
};

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

static float hnn_clampf(float value, float lo, float hi) {
    if (!isfinite(value)) return 0.0f;
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static uint64_t hnn_fnv1a64_bytes(const void *data, size_t len, uint64_t hash) {
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t hnn_fnv1a64_u32(uint32_t value, uint64_t hash) {
    unsigned char le[4];
    le[0] = (unsigned char)(value & 0xffu);
    le[1] = (unsigned char)((value >> 8) & 0xffu);
    le[2] = (unsigned char)((value >> 16) & 0xffu);
    le[3] = (unsigned char)((value >> 24) & 0xffu);
    return hnn_fnv1a64_bytes(le, sizeof(le), hash);
}

static size_t hnn_key_cache_slot(uint64_t seed) {
    uint64_t x = seed;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return (size_t)(x & (uint64_t)(HNN_KEY_CACHE_SIZE - 1u));
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
        float w_re = fixp_cosf(angle);
        float w_im = fixp_sinf(angle);
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
    if (!isfinite(sum_sq) || sum_sq < 1e-30f) {
        memset(v, 0, HNN_DIM * sizeof(float));
        return 0.0f;
    }
    float norm = fixp_sqrtf(sum_sq);
    if (!isfinite(norm) || norm < 1e-15f) {
        memset(v, 0, HNN_DIM * sizeof(float));
        return 0.0f;
    }
    float inv_norm = 1.0f / norm;
    for (int i = 0; i < HNN_DIM; i++) v[i] *= inv_norm;
    return norm;
}

void hnn_key_vector(uint64_t seed, float out[HNN_DIM]) {
    size_t slot;
    hnn_key_cache_entry_t *entry;
    int half = HNN_DIM / 2;
    if (!out) return;

    slot = hnn_key_cache_slot(seed);
    entry = &g_hnn_key_cache[slot];
    if (entry->valid && entry->seed == seed) {
        memcpy(out, entry->vec, HNN_DIM * sizeof(float));
        return;
    }

    /* Split the 64-bit seed into two 32-bit halves for the PRNG */
    uint32_t lo = (uint32_t)(seed & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)((seed >> 32) & 0xFFFFFFFFu);
    hnn_srand(lo ^ (hi * 2654435761u));

    /* Build in frequency domain: unit magnitude at every bin, random phase. */
    float F[HNN_DIM * 2];

    /* DC: +/-1 */
    F[0] = hnn_randf() < 0.5f ? 1.0f : -1.0f;
    F[1] = 0.0f;

    for (int i = 1; i < half; i++) {
        float phase = hnn_randf() * 2.0f * 3.14159265358979323846f;
        float a_re = fixp_cosf(phase);
        float a_im = fixp_sinf(phase);
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

    /* Normalize onto the hypersphere */
    hnn_normalize(out);
    entry->seed = seed;
    entry->valid = true;
    memcpy(entry->vec, out, HNN_DIM * sizeof(float));
}

void hnn_bind(const float a[HNN_DIM], const float b[HNN_DIM],
              float c[HNN_DIM]) {
    float work[HNN_DIM * 2];
    float Fa[HNN_DIM * 2];
    if (!a || !b || !c) return;

    /* Pack a into complex array and FFT */
    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i]     = a[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_fft(work, HNN_DIM);

    /* Store FFT(a) */
    memcpy(Fa, work, HNN_COMPLEX_COUNT * sizeof(float));

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
    /* IFFT */
    hnn_ifft(work, HNN_DIM);

    /* Copy real parts to output */
    for (int i = 0; i < HNN_DIM; i++) c[i] = work[2 * i];

    hnn_normalize(c);
}

void hnn_unbind(const float a[HNN_DIM], const float b[HNN_DIM],
                float c[HNN_DIM]) {
    float work[HNN_DIM * 2];
    float Fa[HNN_DIM * 2];
    if (!a || !b || !c) return;

    /* FFT a */
    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i]     = a[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_fft(work, HNN_DIM);

    /* Store FFT(a) */
    memcpy(Fa, work, HNN_COMPLEX_COUNT * sizeof(float));

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
    hnn_ifft(work, HNN_DIM);

    for (int i = 0; i < HNN_DIM; i++) c[i] = work[2 * i];

    hnn_normalize(c);
}

void hnn_bundle(float a[HNN_DIM], const float b[HNN_DIM]) {
    for (int i = 0; i < HNN_DIM; i++) a[i] += b[i];
    hnn_normalize(a);
}

float hnn_similarity(const float a[HNN_DIM], const float b[HNN_DIM]) {
    float dot = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) dot += a[i] * b[i];
    return isfinite(dot) ? dot : 0.0f;
}

static void hnn_feature_value_keys_init(void) {
    if (g_hnn_feature_value_keys_initialized) return;
    for (int i = 0; i < HNN_FEATURE_COUNT; i++) {
        for (int level = 0; level < HNN_FEATURE_VALUE_LEVELS; level++) {
            uint64_t seed = HNN_FEATURE_VALUE_KEY_SEED_BASE +
                (uint64_t)(i * HNN_FEATURE_VALUE_LEVELS + level);
            hnn_key_vector(seed, g_hnn_feature_value_keys[i][level]);
        }
    }
    g_hnn_feature_value_keys_initialized = true;
}

/* --- Holographic memory --- */

void hnn_memory_init(hnn_memory_t *mem) {
    memset(mem->store, 0, HNN_DIM * sizeof(float));
    mem->experience_count = 0;
    mem->last_retrieval_similarity = 0.0f;
    mem->last_margin = 0.0f;
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

uint64_t hnn_action_vocabulary_hash(void) {
    uint64_t hash = 1469598103934665603ull;
    hash = hnn_fnv1a64_u32((uint32_t)HNN_ACTION_COUNT, hash);
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        const hnn_action_vocab_entry_t *entry = &HNN_ACTION_VOCAB[i];
        hash = hnn_fnv1a64_u32((uint32_t)i, hash);
        hash = hnn_fnv1a64_bytes(entry->name, strlen(entry->name) + 1u, hash);
        hash = hnn_fnv1a64_u32((uint32_t)(entry->turn + 128), hash);
        hash = hnn_fnv1a64_u32((uint32_t)(entry->thrust + 128), hash);
    }
    return hash;
}

float hnn_memory_capacity_load(const hnn_memory_t *mem) {
    if (!mem || mem->experience_count <= 0) return 0.0f;
    return (float)mem->experience_count / (float)HNN_TRACE_CAPACITY;
}

static float hnn_fidelity_for(int stored_count, float similarity, float margin) {
    if (stored_count <= 0) return 0.0f;
    float load = (float)stored_count / (float)HNN_TRACE_CAPACITY;
    float load_factor = 1.0f / (1.0f + load * load);
    float sim_factor = hnn_clampf((similarity + 1.0f) * 0.5f, 0.0f, 1.0f);
    float margin_factor = hnn_clampf(margin * 8.0f, 0.0f, 1.0f);
    return hnn_clampf(load_factor * (0.65f * sim_factor + 0.35f * margin_factor),
                      0.0f,
                      1.0f);
}

float hnn_memory_fidelity_estimate(const hnn_memory_t *mem) {
    if (!mem) return 0.0f;
    return hnn_fidelity_for(mem->experience_count,
                            mem->last_retrieval_similarity,
                            mem->last_margin);
}

hnn_memory_contract_t hnn_memory_contract(const hnn_memory_t *mem) {
    hnn_memory_contract_t contract;
    memset(&contract, 0, sizeof(contract));
    contract.dim = HNN_DIM;
    contract.seed = HNN_CONTRACT_SEED;
    contract.keygen_version = HNN_KEYGEN_VERSION;
    contract.encoder_version = HNN_PILOT_ENCODER_VERSION;
    contract.action_vocabulary_hash = hnn_action_vocabulary_hash();
    contract.trace_format_version = HNN_TRACE_FORMAT_VERSION;
    if (mem) {
        contract.stored_count = mem->experience_count;
        contract.capacity_load = hnn_memory_capacity_load(mem);
        contract.fidelity_estimate = hnn_memory_fidelity_estimate(mem);
        contract.last_margin = mem->last_margin;
    }
    return contract;
}

void hnn_holonet_init(hnn_holonet_t *net) {
    if (!net) return;
    memset(net, 0, sizeof(*net));
    net->last_route = -1;
    for (int i = 0; i < (int)HNN_HOLONET_TRACE_COUNT; i++)
        hnn_memory_init(&net->cells[i].memory);
}

int hnn_holonet_active_count(const hnn_holonet_t *net) {
    if (!net) return 0;
    int active = 0;
    for (int i = 0; i < (int)HNN_HOLONET_TRACE_COUNT; i++) {
        const hnn_holonet_cell_t *cell = &net->cells[i];
        if (cell->centroid_count > 0 &&
            cell->memory.experience_count > 0) {
            active++;
        }
    }
    return active;
}

static int hnn_holonet_first_inactive(const hnn_holonet_t *net) {
    if (!net) return -1;
    for (int i = 0; i < (int)HNN_HOLONET_TRACE_COUNT; i++) {
        const hnn_holonet_cell_t *cell = &net->cells[i];
        if (cell->centroid_count <= 0 ||
            cell->memory.experience_count <= 0) {
            return i;
        }
    }
    return -1;
}

static int hnn_holonet_nearest_cell(const hnn_holonet_t *net,
                                    const float route_key[HNN_DIM],
                                    float *out_similarity) {
    int best = -1;
    float best_sim = -2.0f;
    if (!net || !route_key) {
        if (out_similarity) *out_similarity = 0.0f;
        return -1;
    }
    for (int i = 0; i < (int)HNN_HOLONET_TRACE_COUNT; i++) {
        const hnn_holonet_cell_t *cell = &net->cells[i];
        if (cell->centroid_count <= 0 ||
            cell->memory.experience_count <= 0) {
            continue;
        }
        float sim = hnn_similarity(route_key, cell->centroid);
        if (!isfinite(sim)) sim = 0.0f;
        if (sim > best_sim) {
            best_sim = sim;
            best = i;
        }
    }
    if (out_similarity) *out_similarity = (best >= 0) ? best_sim : 0.0f;
    return best;
}

void hnn_holonet_store(hnn_holonet_t *net,
                       const float route_key[HNN_DIM],
                       const float assoc_key[HNN_DIM],
                       const float value[HNN_DIM]) {
    if (!net || !route_key || !assoc_key || !value) return;

    int active = hnn_holonet_active_count(net);
    float best_sim = 0.0f;
    int route = hnn_holonet_nearest_cell(net, route_key, &best_sim);
    int open = hnn_holonet_first_inactive(net);
    if ((route < 0 || best_sim < HNN_HOLONET_NEW_CELL_SIMILARITY) &&
        open >= 0) {
        route = open;
        best_sim = 1.0f;
        hnn_holonet_cell_t *cell = &net->cells[route];
        memset(cell, 0, sizeof(*cell));
        hnn_memory_init(&cell->memory);
        memcpy(cell->centroid, route_key, HNN_DIM * sizeof(float));
        hnn_normalize(cell->centroid);
        cell->centroid_count = 0;
        active++;
    }
    if (route < 0) return;

    hnn_holonet_cell_t *cell = &net->cells[route];
    if (cell->centroid_count <= 0) {
        memcpy(cell->centroid, route_key, HNN_DIM * sizeof(float));
        hnn_normalize(cell->centroid);
        cell->centroid_count = 1;
    } else {
        float retain = (float)cell->centroid_count;
        for (int i = 0; i < HNN_DIM; i++)
            cell->centroid[i] = cell->centroid[i] * retain + route_key[i];
        hnn_normalize(cell->centroid);
        cell->centroid_count++;
    }
    hnn_memory_store(&cell->memory, assoc_key, value);

    net->active_count = active;
    net->stored_count++;
    net->last_route = route;
    net->last_route_similarity = best_sim;
}

hnn_memory_contract_t hnn_holonet_contract(const hnn_holonet_t *net) {
    hnn_memory_contract_t contract = hnn_memory_contract(NULL);
    if (!net) return contract;

    int active = 0;
    int stored = 0;
    float fidelity_sum = 0.0f;
    for (int i = 0; i < (int)HNN_HOLONET_TRACE_COUNT; i++) {
        const hnn_holonet_cell_t *cell = &net->cells[i];
        if (cell->centroid_count <= 0 ||
            cell->memory.experience_count <= 0) {
            continue;
        }
        active++;
        stored += cell->memory.experience_count;
        fidelity_sum += hnn_memory_fidelity_estimate(&cell->memory);
    }
    contract.stored_count = stored;
    contract.capacity_load =
        (float)stored / ((float)HNN_TRACE_CAPACITY *
                         (float)HNN_HOLONET_TRACE_COUNT);
    contract.fidelity_estimate =
        active > 0 ? fidelity_sum / (float)active : 0.0f;
    if (net->last_fidelity > 0.0f)
        contract.fidelity_estimate = net->last_fidelity;
    contract.last_margin = net->last_margin;
    return contract;
}

/* --- Pilot feature encoding --- */

static void hnn_encode_feature_value(int feature,
                                     float value,
                                     float out[HNN_DIM]) {
    value = hnn_clampf(value, -1.0f, 1.0f);
    float position = (value + 1.0f) * 0.5f *
                     (float)(HNN_FEATURE_VALUE_LEVELS - 1);
    int lower = (int)position;
    int upper = lower < HNN_FEATURE_VALUE_LEVELS - 1 ? lower + 1 : lower;
    float blend = position - (float)lower;
    const float *a = g_hnn_feature_value_keys[feature][lower];
    const float *b = g_hnn_feature_value_keys[feature][upper];

    for (int i = 0; i < HNN_DIM; i++)
        out[i] = a[i] * (1.0f - blend) + b[i] * blend;
    hnn_normalize(out);
}

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
    hnn_feature_value_keys_init();

    for (int i = 0; i < HNN_FEATURE_COUNT; i++) {
        float feat_val[HNN_DIM];
        hnn_encode_feature_value(i, vals[i], feat_val);

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
        hnn_key_vector(HNN_ACTION_KEY_SEED_BASE + (uint64_t)i,
                       table->vecs[i]);
    }
}

int hnn_score_actions(const hnn_memory_t *mem,
                      const hnn_action_table_t *actions,
                      const hnn_pilot_features_t *state,
                      float scores_out[HNN_ACTION_COUNT],
                      float *out_margin,
                      float *out_fidelity,
                      int cleanup_steps) {
    if (scores_out) {
        for (int i = 0; i < HNN_ACTION_COUNT; i++) scores_out[i] = -INFINITY;
    }
    if (out_margin) *out_margin = 0.0f;
    if (out_fidelity) *out_fidelity = 0.0f;
    if (!mem || !actions || !state) return -1;

    float state_vec[HNN_DIM];
    hnn_encode_state(state, state_vec);

    float retrieved[HNN_DIM];
    if (cleanup_steps > 0)
        hnn_memory_cleanup(mem, state_vec, retrieved, cleanup_steps);
    else
        hnn_memory_retrieve(mem, state_vec, retrieved);

    int best_idx = 0;
    float best_sim = -2.0f;
    float second_sim = -2.0f;

    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        float sim = hnn_similarity(retrieved, actions->vecs[i]);
        if (!isfinite(sim)) sim = 0.0f;
        if (scores_out) scores_out[i] = sim;
        if (sim > best_sim) {
            second_sim = best_sim;
            best_sim = sim;
            best_idx = i;
        } else if (sim > second_sim) {
            second_sim = sim;
        }
    }

    float margin = best_sim - second_sim;
    if (!isfinite(margin)) margin = 0.0f;
    if (out_margin) *out_margin = margin;
    if (out_fidelity)
        *out_fidelity = hnn_fidelity_for(mem->experience_count, best_sim, margin);
    return best_idx;
}

int hnn_holonet_score_actions(hnn_holonet_t *net,
                              const hnn_action_table_t *actions,
                              const hnn_pilot_features_t *state,
                              float scores_out[HNN_ACTION_COUNT],
                              float *out_margin,
                              float *out_fidelity,
                              int cleanup_steps) {
    float accum[HNN_ACTION_COUNT] = {0.0f};
    float state_vec[HNN_DIM];
    float best_score = -2.0f;
    float second_score = -2.0f;
    int best_action = -1;
    int scored = 0;
    float weight_sum = 0.0f;
    float fidelity_sum = 0.0f;
    float nearest_sim = 0.0f;
    int nearest = -1;

    if (scores_out) {
        for (int i = 0; i < HNN_ACTION_COUNT; i++) scores_out[i] = -INFINITY;
    }
    if (out_margin) *out_margin = 0.0f;
    if (out_fidelity) *out_fidelity = 0.0f;
    if (!net || !actions || !state) return -1;

    hnn_encode_state(state, state_vec);
    nearest = hnn_holonet_nearest_cell(net, state_vec, &nearest_sim);
    if (nearest < 0) {
        net->last_route = -1;
        net->last_scored_count = 0;
        net->last_route_similarity = 0.0f;
        net->last_margin = 0.0f;
        net->last_fidelity = 0.0f;
        return -1;
    }

    for (int c = 0; c < (int)HNN_HOLONET_TRACE_COUNT; c++) {
        hnn_holonet_cell_t *cell = &net->cells[c];
        if (cell->centroid_count <= 0 ||
            cell->memory.experience_count <= 0) {
            cell->last_route_similarity = 0.0f;
            cell->last_weight = 0.0f;
            continue;
        }

        float sim = hnn_similarity(state_vec, cell->centroid);
        if (!isfinite(sim)) sim = 0.0f;
        float route = hnn_clampf((sim + 1.0f) * 0.5f, 0.0f, 1.0f);
        float weight = route * route + 0.01f;
        if (c == nearest) weight += 0.25f;

        float cell_scores[HNN_ACTION_COUNT];
        float cell_margin = 0.0f;
        float cell_fidelity = 0.0f;
        int cell_top = hnn_score_actions(&cell->memory,
                                         actions,
                                         state,
                                         cell_scores,
                                         &cell_margin,
                                         &cell_fidelity,
                                         cleanup_steps);
        (void)cell_top;
        (void)cell_margin;
        for (int i = 0; i < HNN_ACTION_COUNT; i++) {
            float score = isfinite(cell_scores[i]) ? cell_scores[i] : 0.0f;
            accum[i] += score * weight;
        }
        fidelity_sum += cell_fidelity * weight;
        weight_sum += weight;
        scored++;
        cell->last_route_similarity = sim;
        cell->last_weight = weight;
    }

    if (weight_sum <= 0.0f || scored <= 0) return -1;

    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        float score = accum[i] / weight_sum;
        if (!isfinite(score)) score = 0.0f;
        if (scores_out) scores_out[i] = score;
        if (score > best_score) {
            second_score = best_score;
            best_score = score;
            best_action = i;
        } else if (score > second_score) {
            second_score = score;
        }
    }

    float margin = best_score - second_score;
    if (!isfinite(margin)) margin = 0.0f;
    float fidelity = fidelity_sum / weight_sum;
    if (!isfinite(fidelity)) fidelity = 0.0f;
    if (out_margin) *out_margin = margin;
    if (out_fidelity) *out_fidelity = fidelity;
    net->active_count = hnn_holonet_active_count(net);
    net->last_route = nearest;
    net->last_scored_count = scored;
    net->last_route_similarity = nearest_sim;
    net->last_margin = margin;
    net->last_fidelity = fidelity;
    return best_action;
}

int hnn_select_action(const hnn_memory_t *mem,
                      const hnn_action_table_t *actions,
                      const hnn_pilot_features_t *state,
                      float *out_confidence) {
    float scores[HNN_ACTION_COUNT];
    int best_idx = hnn_score_actions(mem, actions, state, scores, NULL, NULL, 3);
    if (best_idx < 0) {
        if (out_confidence) *out_confidence = 0.0f;
        return 0;
    }
    if (out_confidence) *out_confidence = scores[best_idx];
    return best_idx;
}
