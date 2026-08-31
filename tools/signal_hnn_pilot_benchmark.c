#include "holographic_nn_backend.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PILOT_BACKEND_COUNT 3
#define PILOT_DEFAULT_SAMPLES 256
#define PILOT_WARMUP_SAMPLES 16
#define PILOT_BATCH_SIZE 8
#define PILOT_TOLERANCE 1.0e-5f

typedef enum pilot_operation {
    PILOT_BIND = 0,
    PILOT_UNBIND,
    PILOT_ACTION_SCORING,
} pilot_operation_t;

typedef struct pilot_result {
    const char *name;
    size_t scratch_bytes;
    size_t context_bytes;
    size_t context_allocations;
    float max_bind_delta;
    float max_unbind_delta;
    float max_action_score_delta;
    int top_action;
    int top_action_agrees;
    double bind_median_us;
    double bind_p95_us;
    double unbind_median_us;
    double unbind_p95_us;
    double action_median_us;
    double action_p95_us;
    int passed;
} pilot_result_t;

static volatile float g_pilot_sink = 0.0f;

static uint64_t pilot_now_ns(void) {
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static uint32_t pilot_rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int pilot_make_unit_vector(uint32_t seed, float out[HNN_DIM]) {
    uint32_t state = seed ? seed : 1u;
    for (int i = 0; i < HNN_DIM; i++) {
        int32_t centered = (int32_t)(pilot_rng_next(&state) & 0xffffu) -
                           32768;
        out[i] = (float)centered / 32768.0f;
    }
    (void)hnn_backend_normalize_for(HNN_BACKEND_BUILTIN_RADIX2, out);
    return hnn_backend_last_status() == HNN_BACKEND_STATUS_OK;
}

static float pilot_max_delta(const float a[HNN_DIM],
                             const float b[HNN_DIM]) {
    float maximum = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) {
        float delta = fabsf(a[i] - b[i]);
        if (delta > maximum) maximum = delta;
    }
    return maximum;
}

static int pilot_double_compare(const void *left, const void *right) {
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double pilot_percentile(double *values, int count, double percentile) {
    qsort(values, (size_t)count, sizeof(*values), pilot_double_compare);
    double rank = ceil(percentile * (double)count);
    int index = (int)rank - 1;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    return values[index];
}

static int pilot_run_operation(hnn_backend_kind_t kind,
                               pilot_operation_t operation,
                               const float a[HNN_DIM],
                               const float b[HNN_DIM],
                               const float *candidates,
                               float out[HNN_DIM]) {
    if (operation == PILOT_BIND) {
        if (!hnn_backend_bind_for(kind, a, b, out)) return 0;
        g_pilot_sink += out[0];
        return 1;
    }
    if (operation == PILOT_UNBIND) {
        if (!hnn_backend_unbind_for(kind, a, b, out)) return 0;
        g_pilot_sink += out[1];
        return 1;
    }
    size_t best = 0u;
    float score = 0.0f;
    if (!hnn_backend_cleanup_for(kind,
                                 a,
                                 candidates,
                                 HNN_ACTION_COUNT,
                                 &best,
                                 &score)) {
        return 0;
    }
    g_pilot_sink += score + (float)best * 1.0e-9f;
    return 1;
}

static int pilot_measure(hnn_backend_kind_t kind,
                         pilot_operation_t operation,
                         const float a[HNN_DIM],
                         const float b[HNN_DIM],
                         const float *candidates,
                         int samples,
                         double *out_median_us,
                         double *out_p95_us) {
    float out[HNN_DIM];
    double *durations = calloc((size_t)samples, sizeof(*durations));
    if (!durations) return 0;
    for (int i = 0; i < PILOT_WARMUP_SAMPLES; i++) {
        if (!pilot_run_operation(kind, operation, a, b, candidates, out)) {
            free(durations);
            return 0;
        }
    }
    for (int i = 0; i < samples; i++) {
        uint64_t start = pilot_now_ns();
        for (int batch = 0; batch < PILOT_BATCH_SIZE; batch++) {
            if (!pilot_run_operation(kind, operation, a, b, candidates, out)) {
                free(durations);
                return 0;
            }
        }
        uint64_t end = pilot_now_ns();
        if (end < start) {
            free(durations);
            return 0;
        }
        durations[i] = (double)(end - start) /
                       (1000.0 * (double)PILOT_BATCH_SIZE);
    }
    *out_median_us = pilot_percentile(durations, samples, 0.50);
    *out_p95_us = pilot_percentile(durations, samples, 0.95);
    free(durations);
    return 1;
}

static int pilot_parse_samples(int argc, char **argv, int *out_samples) {
    int samples = PILOT_DEFAULT_SAMPLES;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--samples") != 0 || i + 1 >= argc) return 0;
        char *end = NULL;
        long parsed = strtol(argv[++i], &end, 10);
        if (!end || *end != '\0' || parsed < 32 || parsed > 100000) return 0;
        samples = (int)parsed;
    }
    *out_samples = samples;
    return 1;
}

int main(int argc, char **argv) {
    static const hnn_backend_kind_t backends[PILOT_BACKEND_COUNT] = {
        HNN_BACKEND_BUILTIN_RADIX2,
        HNN_BACKEND_LECORE_DIRECT,
        HNN_BACKEND_LECORE_RADIX2,
    };
    int samples = PILOT_DEFAULT_SAMPLES;
    float left[HNN_DIM];
    float right[HNN_DIM];
    float reference_bind[HNN_DIM];
    float reference_unbind[HNN_DIM];
    float actual[HNN_DIM];
    float candidates[HNN_ACTION_COUNT * HNN_DIM];
    float reference_scores[HNN_ACTION_COUNT];
    pilot_result_t results[PILOT_BACKEND_COUNT];
    size_t reference_top = 0u;
    float reference_top_score = 0.0f;
    int all_passed = 1;

    if (!pilot_parse_samples(argc, argv, &samples)) {
        fprintf(stderr, "usage: signal_hnn_pilot_benchmark [--samples N]\n");
        return 2;
    }
    if (!pilot_make_unit_vector(0x51a7c0deu, left) ||
        !pilot_make_unit_vector(0x6c8e9cf5u, right)) {
        fprintf(stderr, "failed to create deterministic benchmark vectors\n");
        return 1;
    }
    for (int action = 0; action < HNN_ACTION_COUNT; action++) {
        if (!pilot_make_unit_vector(0x10001u + (uint32_t)action * 0x9e37u,
                                    candidates + action * HNN_DIM)) {
            fprintf(stderr, "failed to create action vector %d\n", action);
            return 1;
        }
    }
    if (!hnn_backend_bind_for(HNN_BACKEND_BUILTIN_RADIX2,
                              left,
                              right,
                              reference_bind) ||
        !hnn_backend_unbind_for(HNN_BACKEND_BUILTIN_RADIX2,
                                reference_bind,
                                right,
                                reference_unbind)) {
        fprintf(stderr, "failed to create reference results\n");
        return 1;
    }
    for (int action = 0; action < HNN_ACTION_COUNT; action++) {
        reference_scores[action] = hnn_backend_similarity_for(
            HNN_BACKEND_BUILTIN_RADIX2,
            left,
            candidates + action * HNN_DIM);
    }
    if (!hnn_backend_cleanup_for(HNN_BACKEND_BUILTIN_RADIX2,
                                 left,
                                 candidates,
                                 HNN_ACTION_COUNT,
                                 &reference_top,
                                 &reference_top_score)) {
        fprintf(stderr, "failed to create reference action result\n");
        return 1;
    }

    for (int i = 0; i < PILOT_BACKEND_COUNT; i++) {
        hnn_backend_kind_t kind = backends[i];
        pilot_result_t *result = &results[i];
        memset(result, 0, sizeof(*result));
        result->name = hnn_backend_kind_name(kind);
        if (!hnn_backend_thread_init(kind)) {
            fprintf(stderr, "failed to initialize %s: %s\n",
                    result->name,
                    hnn_backend_status_string(hnn_backend_last_status()));
            return 1;
        }
        result->scratch_bytes = hnn_backend_scratch_bytes(kind);
        result->context_bytes = hnn_backend_thread_memory_bytes(kind);
        result->context_allocations =
            hnn_backend_thread_allocation_count(kind);

        if (!hnn_backend_bind_for(kind, left, right, actual)) return 1;
        result->max_bind_delta = pilot_max_delta(reference_bind, actual);
        if (!hnn_backend_unbind_for(kind, reference_bind, right, actual)) {
            return 1;
        }
        result->max_unbind_delta = pilot_max_delta(reference_unbind, actual);
        for (int action = 0; action < HNN_ACTION_COUNT; action++) {
            float score = hnn_backend_similarity_for(
                kind, left, candidates + action * HNN_DIM);
            float delta = fabsf(score - reference_scores[action]);
            if (delta > result->max_action_score_delta) {
                result->max_action_score_delta = delta;
            }
        }
        size_t top = 0u;
        float top_score = 0.0f;
        if (!hnn_backend_cleanup_for(kind,
                                     left,
                                     candidates,
                                     HNN_ACTION_COUNT,
                                     &top,
                                     &top_score)) {
            return 1;
        }
        result->top_action = (int)top;
        result->top_action_agrees = top == reference_top;
        if (!pilot_measure(kind, PILOT_BIND, left, right, candidates, samples,
                           &result->bind_median_us, &result->bind_p95_us) ||
            !pilot_measure(kind, PILOT_UNBIND, reference_bind, right,
                           candidates, samples,
                           &result->unbind_median_us,
                           &result->unbind_p95_us) ||
            !pilot_measure(kind, PILOT_ACTION_SCORING, left, right,
                           candidates, samples,
                           &result->action_median_us,
                           &result->action_p95_us)) {
            fprintf(stderr, "failed to benchmark %s\n", result->name);
            return 1;
        }
        result->passed = result->max_bind_delta <= PILOT_TOLERANCE &&
                         result->max_unbind_delta <= PILOT_TOLERANCE &&
                         result->max_action_score_delta <= PILOT_TOLERANCE &&
                         result->top_action_agrees;
        if (!result->passed) all_passed = 0;
    }

    printf("{\"schema\":\"signal.hnn_backend_benchmark.v1\",");
    printf("\"dimension\":%d,\"samples\":%d,\"batch_size\":%d,",
           HNN_DIM, samples, PILOT_BATCH_SIZE);
    printf("\"tolerance\":%.9g,\"reference_top_action\":%zu,",
           (double)PILOT_TOLERANCE, reference_top);
    printf("\"backends\":[");
    for (int i = 0; i < PILOT_BACKEND_COUNT; i++) {
        const pilot_result_t *r = &results[i];
        if (i > 0) printf(",");
        printf("{\"name\":\"%s\",", r->name);
        printf("\"scratch_bytes\":%zu,\"context_bytes\":%zu,",
               r->scratch_bytes, r->context_bytes);
        printf("\"context_allocations\":%zu,", r->context_allocations);
        printf("\"correctness\":{\"max_bind_delta\":%.9g,",
               (double)r->max_bind_delta);
        printf("\"max_unbind_delta\":%.9g,",
               (double)r->max_unbind_delta);
        printf("\"max_action_score_delta\":%.9g,",
               (double)r->max_action_score_delta);
        printf("\"top_action\":%d,\"top_action_agrees\":%s},",
               r->top_action, r->top_action_agrees ? "true" : "false");
        printf("\"timings_us\":{");
        printf("\"bind\":{\"median\":%.6f,\"p95\":%.6f},",
               r->bind_median_us, r->bind_p95_us);
        printf("\"unbind\":{\"median\":%.6f,\"p95\":%.6f},",
               r->unbind_median_us, r->unbind_p95_us);
        printf("\"action_scoring\":{\"median\":%.6f,\"p95\":%.6f}},",
               r->action_median_us, r->action_p95_us);
        printf("\"passed\":%s}", r->passed ? "true" : "false");
    }
    printf("],\"passed\":%s}\n", all_passed ? "true" : "false");
    return all_passed ? 0 : 1;
}
