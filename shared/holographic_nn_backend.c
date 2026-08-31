#include "holographic_nn_backend.h"

#include "fixpoint.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef SIGNAL_HNN_LECORE_AVAILABLE
#include "lecore.h"
#endif

#ifndef SIGNAL_SOURCE_REVISION
#define SIGNAL_SOURCE_REVISION "dev"
#endif

#define HNN_COMPLEX_COUNT ((size_t)HNN_DIM * 2u)
#define HNN_BACKEND_LECORE_ARENA_BYTES 32768u
#define LECORE_PIN_VERSION "0.1.0"
#define LECORE_PIN_ABI_VERSION 0u
#define LECORE_PIN_SOURCE_REVISION \
    "sha256:5da2817e8f2addcc15d3a97c17107c22289bb2609bbdd19f2c199d33238a5a55"
#define LECORE_PIN_SOURCE_CHECKSUM \
    "sha256:f3283ebb033e295e5dbdc46d95add91ab154253169d6a2a1ab696464b051ed07"

#if defined(_MSC_VER)
#define SIGNAL_THREAD_LOCAL __declspec(thread)
#else
#define SIGNAL_THREAD_LOCAL _Thread_local
#endif

static SIGNAL_THREAD_LOCAL hnn_backend_status_t g_hnn_backend_last_status =
    HNN_BACKEND_STATUS_OK;

static void hnn_backend_set_status(hnn_backend_status_t status) {
    g_hnn_backend_last_status = status;
}

static void hnn_backend_zero(float out[HNN_DIM]) {
    if (out) memset(out, 0, HNN_DIM * sizeof(float));
}

static bool hnn_backend_vector_is_finite(const float values[HNN_DIM]) {
    if (!values) return false;
    for (int i = 0; i < HNN_DIM; i++) {
        if (!isfinite(values[i])) return false;
    }
    return true;
}

/* --- Signal's existing radix-2 implementation --- */

static void hnn_builtin_bit_reverse(float *data, int n) {
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int j = 0;
        for (int b = 0; b < bits; b++) {
            if (i & (1 << b)) j |= 1 << (bits - 1 - b);
        }
        if (j > i) {
            float tr = data[2 * i];
            float ti = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tr;
            data[2 * j + 1] = ti;
        }
    }
}

static void hnn_builtin_fft(float *data, int n) {
    hnn_builtin_bit_reverse(data, n);
    for (int len = 2; len <= n; len <<= 1) {
        float angle = -2.0f * 3.14159265358979323846f / (float)len;
        float w_re = fixp_cosf(angle);
        float w_im = fixp_sinf(angle);
        for (int i = 0; i < n; i += len) {
            float cur_re = 1.0f;
            float cur_im = 0.0f;
            int half = len >> 1;
            for (int j = 0; j < half; j++) {
                int a_idx = 2 * (i + j);
                int b_idx = 2 * (i + j + half);
                float u_re = data[a_idx];
                float u_im = data[a_idx + 1];
                float v_re = cur_re * data[b_idx] -
                             cur_im * data[b_idx + 1];
                float v_im = cur_re * data[b_idx + 1] +
                             cur_im * data[b_idx];
                data[a_idx] = u_re + v_re;
                data[a_idx + 1] = u_im + v_im;
                data[b_idx] = u_re - v_re;
                data[b_idx + 1] = u_im - v_im;
                float next_re = cur_re * w_re - cur_im * w_im;
                float next_im = cur_re * w_im + cur_im * w_re;
                cur_re = next_re;
                cur_im = next_im;
            }
        }
    }
}

static void hnn_builtin_ifft(float *data, int n) {
    for (int i = 0; i < n; i++) data[2 * i + 1] = -data[2 * i + 1];
    hnn_builtin_fft(data, n);
    for (int i = 0; i < n; i++) {
        data[2 * i + 1] = -data[2 * i + 1];
        float scale = 1.0f / (float)n;
        data[2 * i] *= scale;
        data[2 * i + 1] *= scale;
    }
}

static float hnn_builtin_norm(const float v[HNN_DIM]) {
    float sum_sq = 0.0f;
    if (!v) return 0.0f;
    for (int i = 0; i < HNN_DIM; i++) sum_sq += v[i] * v[i];
    if (!isfinite(sum_sq) || sum_sq < 1e-30f) return 0.0f;
    float norm = fixp_sqrtf(sum_sq);
    return isfinite(norm) && norm >= 1e-15f ? norm : 0.0f;
}

static float hnn_builtin_normalize(float v[HNN_DIM]) {
    if (!v) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return 0.0f;
    }
    float norm = hnn_builtin_norm(v);
    if (norm == 0.0f) {
        bool finite = hnn_backend_vector_is_finite(v);
        hnn_backend_zero(v);
        hnn_backend_set_status(finite ? HNN_BACKEND_STATUS_OK
                                      : HNN_BACKEND_STATUS_NONFINITE);
        return 0.0f;
    }
    float inv_norm = 1.0f / norm;
    for (int i = 0; i < HNN_DIM; i++) v[i] *= inv_norm;
    hnn_backend_set_status(HNN_BACKEND_STATUS_OK);
    return norm;
}

static bool hnn_builtin_bind_raw(const float a[HNN_DIM],
                                 const float b[HNN_DIM],
                                 float out[HNN_DIM],
                                 bool unbind) {
    float work[HNN_DIM * 2];
    float left_fft[HNN_DIM * 2];
    if (!a || !b || !out) {
        hnn_backend_zero(out);
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return false;
    }

    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i] = a[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_builtin_fft(work, HNN_DIM);
    memcpy(left_fft, work, HNN_COMPLEX_COUNT * sizeof(float));

    for (int i = 0; i < HNN_DIM; i++) {
        work[2 * i] = b[i];
        work[2 * i + 1] = 0.0f;
    }
    hnn_builtin_fft(work, HNN_DIM);

    for (int i = 0; i < HNN_DIM; i++) {
        float a_re = left_fft[2 * i];
        float a_im = left_fft[2 * i + 1];
        float b_re = work[2 * i];
        float b_im = unbind ? -work[2 * i + 1] : work[2 * i + 1];
        work[2 * i] = a_re * b_re - a_im * b_im;
        work[2 * i + 1] = a_re * b_im + a_im * b_re;
    }
    hnn_builtin_ifft(work, HNN_DIM);
    for (int i = 0; i < HNN_DIM; i++) out[i] = work[2 * i];
    hnn_builtin_normalize(out);
    return g_hnn_backend_last_status == HNN_BACKEND_STATUS_OK;
}

static bool hnn_builtin_bundle(float a[HNN_DIM],
                               const float b[HNN_DIM]) {
    if (!a || !b) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return false;
    }
    for (int i = 0; i < HNN_DIM; i++) a[i] += b[i];
    hnn_builtin_normalize(a);
    return g_hnn_backend_last_status == HNN_BACKEND_STATUS_OK;
}

static float hnn_builtin_similarity(const float a[HNN_DIM],
                                    const float b[HNN_DIM]) {
    if (!a || !b) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return 0.0f;
    }
    float dot = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) dot += a[i] * b[i];
    if (!isfinite(dot)) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_NONFINITE);
        return 0.0f;
    }
    hnn_backend_set_status(HNN_BACKEND_STATUS_OK);
    return dot;
}

static bool hnn_builtin_cleanup(
    const float query[HNN_DIM],
    const float candidates[][HNN_DIM],
    size_t candidate_count,
    size_t *out_index,
    float *out_score) {
    if (!query || !candidates || candidate_count == 0 ||
        !out_index || !out_score) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return false;
    }
    size_t best = 0;
    float best_score = hnn_builtin_similarity(query, candidates[0]);
    for (size_t i = 1; i < candidate_count; i++) {
        float score = hnn_builtin_similarity(query, candidates[i]);
        if (score > best_score) {
            best = i;
            best_score = score;
        }
    }
    *out_index = best;
    *out_score = best_score;
    hnn_backend_set_status(HNN_BACKEND_STATUS_OK);
    return true;
}

/* --- Pinned liblecore contexts and operations --- */

#ifdef SIGNAL_HNN_LECORE_AVAILABLE
typedef union hnn_backend_aligned_arena {
    max_align_t alignment;
    unsigned char bytes[HNN_BACKEND_LECORE_ARENA_BYTES];
} hnn_backend_aligned_arena_t;

typedef struct hnn_backend_lecore_runtime {
    hnn_backend_aligned_arena_t arena;
    size_t arena_offset;
    size_t allocation_count;
    size_t allocation_bytes;
    lecore_context *context;
    float bundle_rows[2][HNN_DIM];
    float bundle_out[HNN_DIM];
} hnn_backend_lecore_runtime_t;

static SIGNAL_THREAD_LOCAL hnn_backend_lecore_runtime_t
    g_hnn_lecore_direct;
static SIGNAL_THREAD_LOCAL hnn_backend_lecore_runtime_t
    g_hnn_lecore_radix2;

static void *hnn_backend_lecore_allocate(void *user,
                                         size_t bytes,
                                         size_t alignment) {
    hnn_backend_lecore_runtime_t *runtime = user;
    uintptr_t base = (uintptr_t)runtime->arena.bytes;
    uintptr_t cursor = base + runtime->arena_offset;
    uintptr_t aligned = (cursor + alignment - 1u) & ~(alignment - 1u);
    size_t offset = (size_t)(aligned - base);
    if (offset > HNN_BACKEND_LECORE_ARENA_BYTES ||
        bytes > HNN_BACKEND_LECORE_ARENA_BYTES - offset) {
        return NULL;
    }
    runtime->arena_offset = offset + bytes;
    runtime->allocation_count++;
    runtime->allocation_bytes += bytes;
    return (void *)aligned;
}

static void hnn_backend_lecore_deallocate(void *user,
                                           void *pointer,
                                           size_t bytes,
                                           size_t alignment) {
    (void)user;
    (void)pointer;
    (void)bytes;
    (void)alignment;
}

static hnn_backend_lecore_runtime_t *hnn_backend_lecore_runtime(
    hnn_backend_kind_t kind) {
    if (kind == HNN_BACKEND_LECORE_DIRECT) return &g_hnn_lecore_direct;
    if (kind == HNN_BACKEND_LECORE_RADIX2) return &g_hnn_lecore_radix2;
    return NULL;
}

static lecore_backend hnn_backend_lecore_kind(hnn_backend_kind_t kind) {
    return kind == HNN_BACKEND_LECORE_RADIX2
        ? LECORE_BACKEND_RADIX2
        : LECORE_BACKEND_DIRECT;
}

static lecore_context *hnn_backend_lecore_context(hnn_backend_kind_t kind) {
    hnn_backend_lecore_runtime_t *runtime =
        hnn_backend_lecore_runtime(kind);
    if (!runtime) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
        return NULL;
    }
    if (runtime->context) return runtime->context;

    lecore_config_v0 config;
    lecore_config_init_v0(&config);
    config.profile = LECORE_PROFILE_HRR_F32_V1;
    config.backend = hnn_backend_lecore_kind(kind);
    config.validation = LECORE_VALIDATION_FINITE;
    config.dimension = HNN_DIM;
    config.allocator.user = runtime;
    config.allocator.allocate = hnn_backend_lecore_allocate;
    config.allocator.deallocate = hnn_backend_lecore_deallocate;

    if (lecore_context_create(&config, &runtime->context) != LECORE_OK) {
        runtime->context = NULL;
        runtime->arena_offset = 0u;
        runtime->allocation_count = 0u;
        runtime->allocation_bytes = 0u;
        hnn_backend_set_status(HNN_BACKEND_STATUS_INIT_FAILED);
        return NULL;
    }
    hnn_backend_set_status(HNN_BACKEND_STATUS_OK);
    return runtime->context;
}

static hnn_backend_status_t hnn_backend_status_from_lecore(
    lecore_status status) {
    if (status == LECORE_OK) return HNN_BACKEND_STATUS_OK;
    if (status == LECORE_EINVAL || status == LECORE_EDIM) {
        return HNN_BACKEND_STATUS_INVALID_ARGUMENT;
    }
    if (status == LECORE_ENONFINITE) return HNN_BACKEND_STATUS_NONFINITE;
    return HNN_BACKEND_STATUS_OPERATION_FAILED;
}

static float hnn_lecore_normalize(hnn_backend_kind_t kind,
                                  float v[HNN_DIM]) {
    if (!v) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return 0.0f;
    }
    float norm = hnn_builtin_norm(v);
    if (norm == 0.0f) {
        bool finite = hnn_backend_vector_is_finite(v);
        hnn_backend_zero(v);
        hnn_backend_set_status(finite ? HNN_BACKEND_STATUS_OK
                                      : HNN_BACKEND_STATUS_NONFINITE);
        return 0.0f;
    }
    lecore_context *context = hnn_backend_lecore_context(kind);
    if (!context) {
        hnn_backend_zero(v);
        return 0.0f;
    }
    lecore_status status = lecore_normalize_f32(context, v, v);
    hnn_backend_set_status(hnn_backend_status_from_lecore(status));
    if (status != LECORE_OK) {
        hnn_backend_zero(v);
        return 0.0f;
    }
    return norm;
}

static bool hnn_lecore_bind(hnn_backend_kind_t kind,
                            const float a[HNN_DIM],
                            const float b[HNN_DIM],
                            float out[HNN_DIM],
                            bool unbind) {
    if (!a || !b || !out) {
        hnn_backend_zero(out);
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return false;
    }
    lecore_context *context = hnn_backend_lecore_context(kind);
    if (!context) {
        hnn_backend_zero(out);
        return false;
    }
    lecore_status status = unbind
        ? lecore_hrr_unbind_f32(context, a, b, out)
        : lecore_hrr_bind_f32(context, a, b, out);
    hnn_backend_set_status(hnn_backend_status_from_lecore(status));
    if (status != LECORE_OK) {
        hnn_backend_zero(out);
        return false;
    }
    (void)hnn_lecore_normalize(kind, out);
    return g_hnn_backend_last_status == HNN_BACKEND_STATUS_OK;
}

static bool hnn_lecore_bundle(hnn_backend_kind_t kind,
                              float a[HNN_DIM],
                              const float b[HNN_DIM]) {
    if (!a || !b) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return false;
    }
    hnn_backend_lecore_runtime_t *runtime =
        hnn_backend_lecore_runtime(kind);
    lecore_context *context = hnn_backend_lecore_context(kind);
    if (!runtime || !context) {
        hnn_backend_zero(a);
        return false;
    }
    memcpy(runtime->bundle_rows[0], a, HNN_DIM * sizeof(float));
    memcpy(runtime->bundle_rows[1], b, HNN_DIM * sizeof(float));
    lecore_status status = lecore_bundle_f32(
        context,
        &runtime->bundle_rows[0][0],
        2u,
        HNN_DIM,
        runtime->bundle_out);
    hnn_backend_set_status(hnn_backend_status_from_lecore(status));
    if (status != LECORE_OK) {
        hnn_backend_zero(a);
        return false;
    }
    memcpy(a, runtime->bundle_out, HNN_DIM * sizeof(float));
    return true;
}

static float hnn_lecore_similarity(hnn_backend_kind_t kind,
                                   const float a[HNN_DIM],
                                   const float b[HNN_DIM]) {
    if (!a || !b) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return 0.0f;
    }
    lecore_context *context = hnn_backend_lecore_context(kind);
    if (!context) return 0.0f;
    float score = 0.0f;
    lecore_status status = lecore_cosine_f32(context, a, b, &score);
    hnn_backend_set_status(hnn_backend_status_from_lecore(status));
    return status == LECORE_OK && isfinite(score) ? score : 0.0f;
}

static bool hnn_lecore_cleanup(
    hnn_backend_kind_t kind,
    const float query[HNN_DIM],
    const float candidates[][HNN_DIM],
    size_t candidate_count,
    size_t *out_index,
    float *out_score) {
    if (!query || !candidates || candidate_count == 0 ||
        !out_index || !out_score) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_INVALID_ARGUMENT);
        return false;
    }
    lecore_context *context = hnn_backend_lecore_context(kind);
    if (!context) return false;
    lecore_status status = lecore_cleanup_f32(
        context,
        query,
        &candidates[0][0],
        candidate_count,
        HNN_DIM,
        out_index,
        out_score);
    hnn_backend_set_status(hnn_backend_status_from_lecore(status));
    if (status != LECORE_OK || !isfinite(*out_score)) {
        *out_index = 0;
        *out_score = 0.0f;
        return false;
    }
    return true;
}
#endif

hnn_backend_kind_t hnn_backend_active_kind(void) {
#if defined(SIGNAL_HNN_BACKEND_LECORE_DIRECT)
    return HNN_BACKEND_LECORE_DIRECT;
#elif defined(SIGNAL_HNN_BACKEND_LECORE_RADIX2)
    return HNN_BACKEND_LECORE_RADIX2;
#else
    return HNN_BACKEND_BUILTIN_RADIX2;
#endif
}

bool hnn_backend_is_available(hnn_backend_kind_t kind) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) return true;
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    return kind == HNN_BACKEND_LECORE_DIRECT ||
           kind == HNN_BACKEND_LECORE_RADIX2;
#else
    (void)kind;
    return false;
#endif
}

const char *hnn_backend_kind_name(hnn_backend_kind_t kind) {
    switch (kind) {
    case HNN_BACKEND_BUILTIN_RADIX2:
        return "builtin-radix2";
    case HNN_BACKEND_LECORE_DIRECT:
        return "lecore-direct";
    case HNN_BACKEND_LECORE_RADIX2:
        return "lecore-radix2";
    default:
        return "unknown";
    }
}

bool hnn_backend_thread_init(hnn_backend_kind_t kind) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) {
        hnn_backend_set_status(HNN_BACKEND_STATUS_OK);
        return true;
    }
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    return hnn_backend_lecore_context(kind) != NULL;
#else
    (void)kind;
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return false;
#endif
}

float hnn_backend_normalize_for(hnn_backend_kind_t kind,
                                float v[HNN_DIM]) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) return hnn_builtin_normalize(v);
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (kind == HNN_BACKEND_LECORE_DIRECT ||
        kind == HNN_BACKEND_LECORE_RADIX2) {
        return hnn_lecore_normalize(kind, v);
    }
#endif
    hnn_backend_zero(v);
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return 0.0f;
}

bool hnn_backend_bind_for(hnn_backend_kind_t kind,
                          const float a[HNN_DIM],
                          const float b[HNN_DIM],
                          float out[HNN_DIM]) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) {
        return hnn_builtin_bind_raw(a, b, out, false);
    }
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (kind == HNN_BACKEND_LECORE_DIRECT ||
        kind == HNN_BACKEND_LECORE_RADIX2) {
        return hnn_lecore_bind(kind, a, b, out, false);
    }
#endif
    hnn_backend_zero(out);
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return false;
}

bool hnn_backend_unbind_for(hnn_backend_kind_t kind,
                            const float composite[HNN_DIM],
                            const float key[HNN_DIM],
                            float out[HNN_DIM]) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) {
        return hnn_builtin_bind_raw(composite, key, out, true);
    }
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (kind == HNN_BACKEND_LECORE_DIRECT ||
        kind == HNN_BACKEND_LECORE_RADIX2) {
        return hnn_lecore_bind(kind, composite, key, out, true);
    }
#endif
    hnn_backend_zero(out);
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return false;
}

bool hnn_backend_bundle_for(hnn_backend_kind_t kind,
                            float a[HNN_DIM],
                            const float b[HNN_DIM]) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) return hnn_builtin_bundle(a, b);
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (kind == HNN_BACKEND_LECORE_DIRECT ||
        kind == HNN_BACKEND_LECORE_RADIX2) {
        return hnn_lecore_bundle(kind, a, b);
    }
#endif
    hnn_backend_zero(a);
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return false;
}

float hnn_backend_similarity_for(hnn_backend_kind_t kind,
                                 const float a[HNN_DIM],
                                 const float b[HNN_DIM]) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) {
        return hnn_builtin_similarity(a, b);
    }
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (kind == HNN_BACKEND_LECORE_DIRECT ||
        kind == HNN_BACKEND_LECORE_RADIX2) {
        return hnn_lecore_similarity(kind, a, b);
    }
#endif
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return 0.0f;
}

bool hnn_backend_cleanup_for(hnn_backend_kind_t kind,
                             const float query[HNN_DIM],
                             const float candidates[][HNN_DIM],
                             size_t candidate_count,
                             size_t *out_index,
                             float *out_score) {
    if (kind == HNN_BACKEND_BUILTIN_RADIX2) {
        return hnn_builtin_cleanup(
            query, candidates, candidate_count, out_index, out_score);
    }
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (kind == HNN_BACKEND_LECORE_DIRECT ||
        kind == HNN_BACKEND_LECORE_RADIX2) {
        return hnn_lecore_cleanup(
            kind, query, candidates, candidate_count, out_index, out_score);
    }
#endif
    hnn_backend_set_status(HNN_BACKEND_STATUS_UNAVAILABLE);
    return false;
}

float hnn_backend_normalize(float v[HNN_DIM]) {
    return hnn_backend_normalize_for(hnn_backend_active_kind(), v);
}

bool hnn_backend_bind(const float a[HNN_DIM],
                      const float b[HNN_DIM],
                      float out[HNN_DIM]) {
    return hnn_backend_bind_for(hnn_backend_active_kind(), a, b, out);
}

bool hnn_backend_unbind(const float composite[HNN_DIM],
                        const float key[HNN_DIM],
                        float out[HNN_DIM]) {
    return hnn_backend_unbind_for(
        hnn_backend_active_kind(), composite, key, out);
}

bool hnn_backend_bundle(float a[HNN_DIM], const float b[HNN_DIM]) {
    return hnn_backend_bundle_for(hnn_backend_active_kind(), a, b);
}

float hnn_backend_similarity(const float a[HNN_DIM],
                             const float b[HNN_DIM]) {
    return hnn_backend_similarity_for(hnn_backend_active_kind(), a, b);
}

bool hnn_backend_cleanup(const float query[HNN_DIM],
                         const float candidates[][HNN_DIM],
                         size_t candidate_count,
                         size_t *out_index,
                         float *out_score) {
    return hnn_backend_cleanup_for(
        hnn_backend_active_kind(),
        query,
        candidates,
        candidate_count,
        out_index,
        out_score);
}

size_t hnn_backend_thread_allocation_count(hnn_backend_kind_t kind) {
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    hnn_backend_lecore_runtime_t *runtime =
        hnn_backend_lecore_runtime(kind);
    return runtime ? runtime->allocation_count : 0u;
#else
    (void)kind;
    return 0u;
#endif
}

size_t hnn_backend_thread_memory_bytes(hnn_backend_kind_t kind) {
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    hnn_backend_lecore_runtime_t *runtime =
        hnn_backend_lecore_runtime(kind);
    return runtime ? runtime->allocation_bytes : 0u;
#else
    (void)kind;
    return 0u;
#endif
}

void hnn_backend_thread_reset_for_tests(void) {
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
    if (g_hnn_lecore_direct.context) {
        lecore_context_destroy(g_hnn_lecore_direct.context);
    }
    if (g_hnn_lecore_radix2.context) {
        lecore_context_destroy(g_hnn_lecore_radix2.context);
    }
    memset(&g_hnn_lecore_direct, 0, sizeof(g_hnn_lecore_direct));
    memset(&g_hnn_lecore_radix2, 0, sizeof(g_hnn_lecore_radix2));
#endif
    hnn_backend_set_status(HNN_BACKEND_STATUS_OK);
}

hnn_backend_status_t hnn_backend_last_status(void) {
    return g_hnn_backend_last_status;
}

const char *hnn_backend_status_string(hnn_backend_status_t status) {
    switch (status) {
    case HNN_BACKEND_STATUS_OK:
        return "ok";
    case HNN_BACKEND_STATUS_INVALID_ARGUMENT:
        return "invalid-argument";
    case HNN_BACKEND_STATUS_NONFINITE:
        return "nonfinite";
    case HNN_BACKEND_STATUS_UNAVAILABLE:
        return "unavailable";
    case HNN_BACKEND_STATUS_INIT_FAILED:
        return "init-failed";
    case HNN_BACKEND_STATUS_OPERATION_FAILED:
        return "operation-failed";
    default:
        return "unknown";
    }
}

hnn_backend_metadata_t hnn_backend_metadata(void) {
    hnn_backend_kind_t kind = hnn_backend_active_kind();
    bool is_liblecore = kind != HNN_BACKEND_BUILTIN_RADIX2;
    hnn_backend_metadata_t metadata = {
        .active_library = is_liblecore ? "liblecore" : "signal",
        .active_library_version = is_liblecore
            ? LECORE_PIN_VERSION
            : "hnn-contract-1",
        .active_abi_version = is_liblecore
            ? LECORE_PIN_ABI_VERSION
            : HNN_CONTRACT_VERSION,
        .active_backend = hnn_backend_kind_name(kind),
        .dimension = HNN_DIM,
        .active_source_revision = is_liblecore
            ? LECORE_PIN_SOURCE_REVISION
            : SIGNAL_SOURCE_REVISION,
        .scratch_bytes = kind == HNN_BACKEND_LECORE_DIRECT
            ? (size_t)HNN_DIM * sizeof(float)
            : (size_t)HNN_DIM * 4u * sizeof(float),
#ifdef SIGNAL_HNN_LECORE_AVAILABLE
        .liblecore_compiled = true,
#else
        .liblecore_compiled = false,
#endif
        .liblecore_version = LECORE_PIN_VERSION,
        .liblecore_abi_version = LECORE_PIN_ABI_VERSION,
        .liblecore_source_revision = LECORE_PIN_SOURCE_REVISION,
        .liblecore_source_checksum = LECORE_PIN_SOURCE_CHECKSUM,
    };
    return metadata;
}
