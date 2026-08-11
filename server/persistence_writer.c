#include "persistence_writer.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

struct persistence_writer {
#ifdef _WIN32
    HANDLE thread;
    CRITICAL_SECTION lock;
#else
    pthread_t thread;
    pthread_mutex_t lock;
#endif
    bool thread_joinable;
    persistence_writer_state_t state;
    world_t *snapshot;
    char root_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy_player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    bool save_player_slot[MAX_PLAYERS];
    persistence_generation_paths_t published;
};

static void writer_lock(persistence_writer_t *writer) {
#ifdef _WIN32
    EnterCriticalSection(&writer->lock);
#else
    (void)pthread_mutex_lock(&writer->lock);
#endif
}

static void writer_unlock(persistence_writer_t *writer) {
#ifdef _WIN32
    LeaveCriticalSection(&writer->lock);
#else
    (void)pthread_mutex_unlock(&writer->lock);
#endif
}

static void persistence_writer_run(persistence_writer_t *writer) {
    persistence_generation_paths_t published = {0};
    bool ok = persistence_generation_commit(
        writer->root_dir,
        writer->legacy_player_dir,
        writer->snapshot,
        writer->save_player_slot,
        PERSISTENCE_GENERATION_FAULT_NONE,
        &published);
    world_snapshot_clone_destroy(writer->snapshot);
    writer->snapshot = NULL;

    writer_lock(writer);
    if (ok) writer->published = published;
    writer->state = ok
        ? PERSISTENCE_WRITER_SUCCEEDED
        : PERSISTENCE_WRITER_FAILED;
    writer_unlock(writer);
}

#ifdef _WIN32
static DWORD WINAPI persistence_writer_thread(void *context) {
    persistence_writer_run((persistence_writer_t *)context);
    return 0;
}
#else
static void *persistence_writer_thread(void *context) {
    persistence_writer_run((persistence_writer_t *)context);
    return NULL;
}
#endif

persistence_writer_t *persistence_writer_create(void) {
    persistence_writer_t *writer = calloc(1, sizeof(*writer));
    if (!writer) return NULL;
#ifdef _WIN32
    InitializeCriticalSection(&writer->lock);
#else
    if (pthread_mutex_init(&writer->lock, NULL) != 0) {
        free(writer);
        return NULL;
    }
#endif
    return writer;
}

static void persistence_writer_join(persistence_writer_t *writer) {
    if (!writer || !writer->thread_joinable) return;
#ifdef _WIN32
    (void)WaitForSingleObject(writer->thread, INFINITE);
    (void)CloseHandle(writer->thread);
    writer->thread = NULL;
#else
    (void)pthread_join(writer->thread, NULL);
#endif
    writer->thread_joinable = false;
}

void persistence_writer_destroy(persistence_writer_t *writer) {
    if (!writer) return;
    persistence_writer_join(writer);
    world_snapshot_clone_destroy(writer->snapshot);
#ifdef _WIN32
    DeleteCriticalSection(&writer->lock);
#else
    (void)pthread_mutex_destroy(&writer->lock);
#endif
    free(writer);
}

bool persistence_writer_start(
    persistence_writer_t *writer,
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS]
) {
    if (!writer || !root_dir || !legacy_player_dir || !world ||
        !save_player_slot || writer->thread_joinable) {
        return false;
    }
    int root_len = snprintf(writer->root_dir, sizeof(writer->root_dir),
                            "%s", root_dir);
    int legacy_len = snprintf(writer->legacy_player_dir,
                              sizeof(writer->legacy_player_dir),
                              "%s", legacy_player_dir);
    if (root_len <= 0 || (size_t)root_len >= sizeof(writer->root_dir) ||
        legacy_len <= 0 ||
        (size_t)legacy_len >= sizeof(writer->legacy_player_dir)) {
        return false;
    }
    writer->snapshot = world_snapshot_clone_create(world);
    if (!writer->snapshot) return false;
    memcpy(writer->save_player_slot, save_player_slot,
           sizeof(writer->save_player_slot));
    memset(&writer->published, 0, sizeof(writer->published));
    writer->state = PERSISTENCE_WRITER_RUNNING;

#ifdef _WIN32
    writer->thread = CreateThread(
        NULL, 0, persistence_writer_thread, writer, 0, NULL);
    if (!writer->thread) {
#else
    if (pthread_create(
            &writer->thread, NULL,
            persistence_writer_thread, writer) != 0) {
#endif
        world_snapshot_clone_destroy(writer->snapshot);
        writer->snapshot = NULL;
        writer->state = PERSISTENCE_WRITER_IDLE;
        return false;
    }
    writer->thread_joinable = true;
    return true;
}

bool persistence_writer_active(persistence_writer_t *writer) {
    /* A completed OS thread is still busy until poll/wait joins it and
     * consumes its result; start() enforces the same single-writer rule. */
    return writer && writer->thread_joinable;
}

static persistence_writer_state_t persistence_writer_collect(
    persistence_writer_t *writer,
    persistence_generation_paths_t *published,
    bool wait
) {
    if (!writer) return PERSISTENCE_WRITER_IDLE;
    writer_lock(writer);
    persistence_writer_state_t state = writer->state;
    writer_unlock(writer);
    if (!wait && state == PERSISTENCE_WRITER_RUNNING)
        return state;
    if (writer->thread_joinable) {
        persistence_writer_join(writer);
        writer_lock(writer);
        state = writer->state;
        writer_unlock(writer);
    }
    if (state != PERSISTENCE_WRITER_SUCCEEDED &&
        state != PERSISTENCE_WRITER_FAILED) {
        return state;
    }
    writer_lock(writer);
    if (state == PERSISTENCE_WRITER_SUCCEEDED && published)
        *published = writer->published;
    writer->state = PERSISTENCE_WRITER_IDLE;
    memset(&writer->published, 0, sizeof(writer->published));
    writer_unlock(writer);
    return state;
}

persistence_writer_state_t persistence_writer_poll(
    persistence_writer_t *writer,
    persistence_generation_paths_t *published
) {
    return persistence_writer_collect(writer, published, false);
}

persistence_writer_state_t persistence_writer_wait(
    persistence_writer_t *writer,
    persistence_generation_paths_t *published
) {
    return persistence_writer_collect(writer, published, true);
}
