#include "test_harness.h"

#include "holographic_nn.h"
#include "holographic_nn_backend.h"

#include <float.h>
#include <stdatomic.h>

#ifndef _WIN32
#include <pthread.h>
#endif

static const hnn_backend_kind_t HNN_TEST_BACKENDS[] = {
    HNN_BACKEND_BUILTIN_RADIX2,
    HNN_BACKEND_LECORE_DIRECT,
    HNN_BACKEND_LECORE_RADIX2,
};

static float hnn_test_l2_norm(const float values[HNN_DIM]) {
    float sum = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) sum += values[i] * values[i];
    return sqrtf(sum);
}

static float hnn_test_max_abs_diff(const float a[HNN_DIM],
                                   const float b[HNN_DIM]) {
    float largest = 0.0f;
    for (int i = 0; i < HNN_DIM; i++) {
        float diff = fabsf(a[i] - b[i]);
        if (diff > largest) largest = diff;
    }
    return largest;
}

static bool hnn_test_is_zero(const float values[HNN_DIM]) {
    for (int i = 0; i < HNN_DIM; i++) {
        if (values[i] != 0.0f) return false;
    }
    return true;
}

static void hnn_test_fixture_vectors(float a[HNN_DIM],
                                     float b[HNN_DIM],
                                     float c[HNN_DIM]) {
    hnn_key_vector(0x6c65636f72650011ull, a);
    hnn_key_vector(0x6c65636f72650022ull, b);
    hnn_key_vector(0x6c65636f72650033ull, c);
}

TEST(test_hnn_backend_normalization_and_invalid_input_contract) {
    float source[HNN_DIM];
    float reference[HNN_DIM];
    for (int i = 0; i < HNN_DIM; i++) {
        source[i] = (float)((i % 29) - 14) * 0.03125f;
    }
    memcpy(reference, source, sizeof(reference));
    float reference_norm = hnn_backend_normalize_for(
        HNN_BACKEND_BUILTIN_RADIX2, reference);

    for (size_t backend = 0;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        hnn_backend_kind_t kind = HNN_TEST_BACKENDS[backend];
        ASSERT(hnn_backend_is_available(kind));
        float actual[HNN_DIM];
        memcpy(actual, source, sizeof(actual));
        float norm = hnn_backend_normalize_for(kind, actual);
        ASSERT_EQ_FLOAT(norm, reference_norm, 0.000001f);
        ASSERT_EQ_FLOAT(hnn_test_l2_norm(actual), 1.0f, 0.00005f);
        ASSERT(hnn_test_max_abs_diff(reference, actual) < 0.00005f);

        memset(actual, 0, sizeof(actual));
        ASSERT_EQ_FLOAT(hnn_backend_normalize_for(kind, actual),
                        0.0f,
                        0.0f);
        ASSERT(hnn_test_is_zero(actual));

        memset(actual, 0, sizeof(actual));
        actual[17] = INFINITY;
        ASSERT_EQ_FLOAT(hnn_backend_normalize_for(kind, actual),
                        0.0f,
                        0.0f);
        ASSERT(hnn_test_is_zero(actual));
        ASSERT_EQ_INT((int)hnn_backend_last_status(),
                      (int)HNN_BACKEND_STATUS_NONFINITE);

        memset(actual, 0, sizeof(actual));
        actual[23] = NAN;
        ASSERT_EQ_FLOAT(hnn_backend_normalize_for(kind, actual),
                        0.0f,
                        0.0f);
        ASSERT(hnn_test_is_zero(actual));
    }
}

TEST(test_hnn_backend_bind_unbind_numerical_agreement) {
    float a[HNN_DIM];
    float b[HNN_DIM];
    float unused[HNN_DIM];
    float reference_bind[HNN_DIM];
    float reference_unbind[HNN_DIM];
    hnn_test_fixture_vectors(a, b, unused);
    ASSERT(hnn_backend_bind_for(
        HNN_BACKEND_BUILTIN_RADIX2, a, b, reference_bind));
    ASSERT(hnn_backend_unbind_for(
        HNN_BACKEND_BUILTIN_RADIX2, reference_bind, b, reference_unbind));

    for (size_t backend = 1;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        hnn_backend_kind_t kind = HNN_TEST_BACKENDS[backend];
        float actual_bind[HNN_DIM];
        float actual_unbind[HNN_DIM];
        ASSERT(hnn_backend_bind_for(kind, a, b, actual_bind));
        ASSERT(hnn_backend_unbind_for(kind, actual_bind, b, actual_unbind));
        ASSERT(hnn_test_max_abs_diff(reference_bind, actual_bind) < 0.005f);
        ASSERT(hnn_backend_similarity_for(
                   HNN_BACKEND_BUILTIN_RADIX2,
                   reference_bind,
                   actual_bind) > 0.999f);
        ASSERT(hnn_test_max_abs_diff(reference_unbind, actual_unbind) <
               0.005f);
        ASSERT(hnn_backend_similarity_for(
                   HNN_BACKEND_BUILTIN_RADIX2,
                   a,
                   actual_unbind) > 0.999f);
    }

    float invalid[HNN_DIM];
    memcpy(invalid, a, sizeof(invalid));
    invalid[9] = NAN;
    for (size_t backend = 0;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        float out[HNN_DIM];
        ASSERT(!hnn_backend_bind_for(
            HNN_TEST_BACKENDS[backend], invalid, b, out));
        ASSERT(hnn_test_is_zero(out));
    }
}

TEST(test_hnn_backend_bundle_similarity_and_cleanup_agreement) {
    float a[HNN_DIM];
    float b[HNN_DIM];
    float c[HNN_DIM];
    float reference[HNN_DIM];
    hnn_test_fixture_vectors(a, b, c);
    memcpy(reference, a, sizeof(reference));
    ASSERT(hnn_backend_bundle_for(HNN_BACKEND_BUILTIN_RADIX2,
                                  reference,
                                  b));
    ASSERT(hnn_backend_bundle_for(HNN_BACKEND_BUILTIN_RADIX2,
                                  reference,
                                  c));

    for (size_t backend = 1;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        hnn_backend_kind_t kind = HNN_TEST_BACKENDS[backend];
        float actual[HNN_DIM];
        memcpy(actual, a, sizeof(actual));
        ASSERT(hnn_backend_bundle_for(kind, actual, b));
        ASSERT(hnn_backend_bundle_for(kind, actual, c));
        ASSERT(hnn_test_max_abs_diff(reference, actual) < 0.0001f);
        ASSERT_EQ_FLOAT(hnn_backend_similarity_for(kind, a, b),
                        hnn_backend_similarity_for(
                            HNN_BACKEND_BUILTIN_RADIX2, a, b),
                        0.00005f);
    }

    float candidates[3][HNN_DIM];
    memcpy(candidates[0], a, sizeof(candidates[0]));
    memcpy(candidates[1], a, sizeof(candidates[1]));
    memcpy(candidates[2], b, sizeof(candidates[2]));
    for (size_t backend = 0;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        size_t index = SIZE_MAX;
        float score = -FLT_MAX;
        ASSERT(hnn_backend_cleanup_for(HNN_TEST_BACKENDS[backend],
                                       a,
                                       &candidates[0][0],
                                       3u,
                                       &index,
                                       &score));
        ASSERT_EQ_INT((int)index, 0);
        ASSERT_EQ_FLOAT(score, 1.0f, 0.00005f);
    }
}

typedef struct hnn_test_memory {
    float store[HNN_DIM];
    int count;
} hnn_test_memory_t;

static bool hnn_test_memory_store(hnn_backend_kind_t kind,
                                  hnn_test_memory_t *memory,
                                  const float key[HNN_DIM],
                                  const float value[HNN_DIM]) {
    float pair[HNN_DIM];
    if (!hnn_backend_bind_for(kind, key, value, pair)) return false;
    for (int i = 0; i < HNN_DIM; i++) memory->store[i] += pair[i];
    memory->count++;
    (void)hnn_backend_normalize_for(kind, memory->store);
    return hnn_backend_last_status() == HNN_BACKEND_STATUS_OK;
}

static int hnn_test_memory_select(
    hnn_backend_kind_t kind,
    const hnn_test_memory_t *memory,
    const float key[HNN_DIM],
    const hnn_action_table_t *actions) {
    float normalized[HNN_DIM];
    float retrieved[HNN_DIM];
    memcpy(normalized, memory->store, sizeof(normalized));
    (void)hnn_backend_normalize_for(kind, normalized);
    if (!hnn_backend_unbind_for(kind, normalized, key, retrieved)) return -1;
    size_t index = SIZE_MAX;
    float score = 0.0f;
    if (!hnn_backend_cleanup_for(kind,
                                 retrieved,
                                 &actions->vecs[0][0],
                                 HNN_ACTION_COUNT,
                                 &index,
                                 &score)) {
        return -1;
    }
    return (int)index;
}

TEST(test_hnn_backend_all_actions_and_capacity_decisions_agree) {
    hnn_action_table_t actions;
    hnn_action_table_init(&actions);

    for (int action = 0; action < HNN_ACTION_COUNT; action++) {
        float state[HNN_DIM];
        hnn_key_vector(9000u + (uint64_t)action, state);
        for (size_t backend = 0;
             backend < sizeof(HNN_TEST_BACKENDS) /
                           sizeof(HNN_TEST_BACKENDS[0]);
             backend++) {
            hnn_test_memory_t memory = {0};
            ASSERT(hnn_test_memory_store(HNN_TEST_BACKENDS[backend],
                                         &memory,
                                         state,
                                         actions.vecs[action]));
            ASSERT_EQ_INT(hnn_test_memory_select(HNN_TEST_BACKENDS[backend],
                                                 &memory,
                                                 state,
                                                 &actions),
                          action);
        }
    }

    enum { HNN_TEST_OVERLOAD_COUNT = HNN_TRACE_CAPACITY + 32 };
    hnn_test_memory_t memories[3] = {0};
    float first_state[HNN_DIM];
    int full_decisions[3] = {-1, -1, -1};
    for (int item = 0; item < HNN_TEST_OVERLOAD_COUNT; item++) {
        float state[HNN_DIM];
        hnn_key_vector(12000u + (uint64_t)item, state);
        if (item == 0) memcpy(first_state, state, sizeof(first_state));
        for (size_t backend = 0;
             backend < sizeof(HNN_TEST_BACKENDS) /
                           sizeof(HNN_TEST_BACKENDS[0]);
             backend++) {
            ASSERT(hnn_test_memory_store(HNN_TEST_BACKENDS[backend],
                                         &memories[backend],
                                         state,
                                         actions.vecs[6]));
            if (item + 1 == (int)HNN_TRACE_CAPACITY) {
                full_decisions[backend] = hnn_test_memory_select(
                    HNN_TEST_BACKENDS[backend],
                    &memories[backend],
                    first_state,
                    &actions);
            }
        }
    }
    ASSERT(full_decisions[0] >= 0);
    ASSERT_EQ_INT(full_decisions[1], full_decisions[0]);
    ASSERT_EQ_INT(full_decisions[2], full_decisions[0]);

    int overload_decisions[3] = {-1, -1, -1};
    for (size_t backend = 0;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        ASSERT_EQ_INT(memories[backend].count, HNN_TEST_OVERLOAD_COUNT);
        overload_decisions[backend] = hnn_test_memory_select(
            HNN_TEST_BACKENDS[backend],
            &memories[backend],
            first_state,
            &actions);
    }
    ASSERT(overload_decisions[0] >= 0);
    ASSERT_EQ_INT(overload_decisions[1], overload_decisions[0]);
    ASSERT_EQ_INT(overload_decisions[2], overload_decisions[0]);
}

TEST(test_hnn_backend_context_reuse_has_no_hot_path_allocations) {
    hnn_backend_thread_reset_for_tests();
    float a[HNN_DIM];
    float b[HNN_DIM];
    float c[HNN_DIM];
    hnn_test_fixture_vectors(a, b, c);

    for (size_t backend = 1;
         backend < sizeof(HNN_TEST_BACKENDS) / sizeof(HNN_TEST_BACKENDS[0]);
         backend++) {
        hnn_backend_kind_t kind = HNN_TEST_BACKENDS[backend];
        ASSERT(hnn_backend_thread_init(kind));
        size_t allocations = hnn_backend_thread_allocation_count(kind);
        size_t memory_bytes = hnn_backend_thread_memory_bytes(kind);
        ASSERT(allocations > 0u);
        ASSERT(memory_bytes > 0u);

        for (int pass = 0; pass < 4; pass++) {
            float bound[HNN_DIM];
            float unbound[HNN_DIM];
            float bundled[HNN_DIM];
            memcpy(bundled, a, sizeof(bundled));
            ASSERT(hnn_backend_bind_for(kind, a, b, bound));
            ASSERT(hnn_backend_unbind_for(kind, bound, b, unbound));
            ASSERT(hnn_backend_bundle_for(kind, bundled, c));
            ASSERT(isfinite(hnn_backend_similarity_for(kind, unbound, a)));
        }
        ASSERT(hnn_backend_thread_allocation_count(kind) == allocations);
        ASSERT(hnn_backend_thread_memory_bytes(kind) == memory_bytes);
    }
}

#ifndef _WIN32
enum { HNN_BACKEND_THREAD_TEST_COUNT = 4 };

typedef struct hnn_backend_thread_case {
    atomic_int *ready;
    atomic_bool *go;
    bool ok;
    float direct[HNN_DIM];
    float radix2[HNN_DIM];
    size_t direct_allocations;
    size_t radix2_allocations;
} hnn_backend_thread_case_t;

static void *hnn_backend_thread_main(void *opaque) {
    hnn_backend_thread_case_t *test_case = opaque;
    float a[HNN_DIM];
    float b[HNN_DIM];
    float unused[HNN_DIM];
    hnn_test_fixture_vectors(a, b, unused);
    atomic_fetch_add_explicit(test_case->ready, 1, memory_order_release);
    while (!atomic_load_explicit(test_case->go, memory_order_acquire)) {
    }
    test_case->ok =
        hnn_backend_thread_init(HNN_BACKEND_LECORE_DIRECT) &&
        hnn_backend_thread_init(HNN_BACKEND_LECORE_RADIX2) &&
        hnn_backend_bind_for(
            HNN_BACKEND_LECORE_DIRECT, a, b, test_case->direct) &&
        hnn_backend_bind_for(
            HNN_BACKEND_LECORE_RADIX2, a, b, test_case->radix2);
    test_case->direct_allocations = hnn_backend_thread_allocation_count(
        HNN_BACKEND_LECORE_DIRECT);
    test_case->radix2_allocations = hnn_backend_thread_allocation_count(
        HNN_BACKEND_LECORE_RADIX2);
    return NULL;
}
#endif

TEST(test_hnn_backend_contexts_are_thread_local_and_deterministic) {
#ifdef _WIN32
    ASSERT(true);
#else
    atomic_int ready;
    atomic_bool go;
    atomic_init(&ready, 0);
    atomic_init(&go, false);
    pthread_t threads[HNN_BACKEND_THREAD_TEST_COUNT];
    hnn_backend_thread_case_t cases[HNN_BACKEND_THREAD_TEST_COUNT] = {0};

    for (int i = 0; i < HNN_BACKEND_THREAD_TEST_COUNT; i++) {
        cases[i].ready = &ready;
        cases[i].go = &go;
        ASSERT_EQ_INT(pthread_create(
            &threads[i], NULL, hnn_backend_thread_main, &cases[i]), 0);
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) !=
           HNN_BACKEND_THREAD_TEST_COUNT) {
    }
    atomic_store_explicit(&go, true, memory_order_release);
    for (int i = 0; i < HNN_BACKEND_THREAD_TEST_COUNT; i++) {
        ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
        ASSERT(cases[i].ok);
        ASSERT(cases[i].direct_allocations > 0u);
        ASSERT(cases[i].radix2_allocations > 0u);
        ASSERT(hnn_test_max_abs_diff(cases[i].direct,
                                     cases[i].radix2) < 0.005f);
        if (i > 0) {
            ASSERT(memcmp(cases[0].direct,
                          cases[i].direct,
                          sizeof(cases[0].direct)) == 0);
            ASSERT(memcmp(cases[0].radix2,
                          cases[i].radix2,
                          sizeof(cases[0].radix2)) == 0);
        }
    }
#endif
}

void register_hnn_backend_tests(void);
void register_hnn_backend_tests(void) {
    TEST_SECTION("\nHNN backend adapter:\n");
    RUN(test_hnn_backend_normalization_and_invalid_input_contract);
    RUN(test_hnn_backend_bind_unbind_numerical_agreement);
    RUN(test_hnn_backend_bundle_similarity_and_cleanup_agreement);
    RUN(test_hnn_backend_all_actions_and_capacity_decisions_agree);
    RUN(test_hnn_backend_context_reuse_has_no_hot_path_allocations);
    RUN(test_hnn_backend_contexts_are_thread_local_and_deterministic);
}
