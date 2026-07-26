#include "test_harness.h"

#include "holographic_nn.h"

#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>

enum {
    NATIVE_SAFETY_THREAD_COUNT = 4,
    NATIVE_SAFETY_STEPS = 32,
};

typedef struct {
    atomic_int ready;
    atomic_bool go;
} native_safety_start_t;

typedef struct {
    native_safety_start_t *start;
    world_t *world;
    float key[HNN_DIM];
    float state[HNN_DIM];
    vec2 final_pos;
} native_safety_thread_t;

static void *native_safety_thread_main(void *opaque) {
    native_safety_thread_t *ctx = opaque;
    hnn_pilot_features_t features = {
        .target_dist = 0.75f,
        .heading_error = -0.25f,
        .heading_cos = 0.96875f,
        .heading_sin = -0.25f,
        .speed = 0.40f,
        .forward_speed = 0.35f,
        .lateral_speed = 0.05f,
        .brake_distance = 0.10f,
        .fwd_clear = 0.80f,
        .left_clear = 0.60f,
        .right_clear = 0.70f,
        .signal_quality = 0.90f,
        .hull_ratio = 1.0f,
        .path_count = 0.25f,
        .path_current = 0.125f,
    };

    atomic_fetch_add_explicit(&ctx->start->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&ctx->start->go, memory_order_acquire)) {
    }

    /*
     * Each worker enters key/cache and feature-key initialization with a
     * fresh thread-local cache. TSan therefore covers the old mutable-global
     * race while the value comparison below preserves deterministic output.
     */
    hnn_key_vector(0x6245afe123456789ull, ctx->key);
    hnn_encode_state(&features, ctx->state);

    for (int i = 0; i < NATIVE_SAFETY_STEPS; i++)
        world_sim_step_player_only(ctx->world, 0, SIM_DT);
    ctx->final_pos = ctx->world->players[0].ship->pos;
    return NULL;
}
#endif

TEST(test_native_safety_thread_reentrant_hnn_and_simulation) {
#ifdef _WIN32
    /* The scheduled TSan lane is Linux-only. Keep Windows test builds valid. */
    ASSERT(true);
#else
    native_safety_start_t start;
    atomic_init(&start.ready, 0);
    atomic_init(&start.go, false);

    pthread_t threads[NATIVE_SAFETY_THREAD_COUNT];
    native_safety_thread_t workers[NATIVE_SAFETY_THREAD_COUNT] = {0};
    int created = 0;
    int thread_error = 0;
    bool results_match = true;

    for (int i = 0; i < NATIVE_SAFETY_THREAD_COUNT; i++) {
        workers[i].start = &start;
        workers[i].world = calloc(1, sizeof(*workers[i].world));
        if (!workers[i].world) {
            for (int j = 0; j < i; j++) {
                world_cleanup(workers[j].world);
                free(workers[j].world);
            }
            ASSERT(workers[i].world != NULL);
        }
        world_reset(workers[i].world);

        server_player_t *sp = &workers[i].world->players[0];
        player_init_ship(sp, workers[i].world);
        sp->connected = true;
        sp->session_ready = true;
        sp->docked = false;
        sp->current_station = -1;
        sp->ship->pos = v2(120.0f, -80.0f);
        sp->ship->vel = v2(12.0f, -3.0f);
        sp->input.thrust = 0.5f;
        sp->input.turn = -0.25f;

    }

    for (int i = 0; i < NATIVE_SAFETY_THREAD_COUNT; i++) {
        thread_error = pthread_create(&threads[i], NULL,
                                      native_safety_thread_main, &workers[i]);
        if (thread_error != 0) break;
        created++;
    }

    while (thread_error == 0 &&
           atomic_load_explicit(&start.ready, memory_order_acquire) !=
               NATIVE_SAFETY_THREAD_COUNT) {
    }
    atomic_store_explicit(&start.go, true, memory_order_release);

    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0 && thread_error == 0)
            thread_error = -1;
    }

    if (thread_error == 0) {
        for (int i = 1; i < NATIVE_SAFETY_THREAD_COUNT; i++) {
            results_match =
                results_match &&
                memcmp(workers[0].key, workers[i].key,
                       sizeof(workers[0].key)) == 0 &&
                memcmp(workers[0].state, workers[i].state,
                       sizeof(workers[0].state)) == 0 &&
                fabsf(workers[0].final_pos.x - workers[i].final_pos.x) <=
                    0.000001f &&
                fabsf(workers[0].final_pos.y - workers[i].final_pos.y) <=
                    0.000001f;
        }
    }

    for (int i = 0; i < NATIVE_SAFETY_THREAD_COUNT; i++) {
        world_cleanup(workers[i].world);
        free(workers[i].world);
    }
    ASSERT_EQ_INT(thread_error, 0);
    ASSERT(results_match);
#endif
}

void register_native_safety_tests(void);
void register_native_safety_tests(void) {
    TEST_SECTION("\nNative safety:\n");
    RUN(test_native_safety_thread_reentrant_hnn_and_simulation);
}
