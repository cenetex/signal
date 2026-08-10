/* Single-worker background generation persistence. */
#ifndef SIGNAL_PERSISTENCE_WRITER_H
#define SIGNAL_PERSISTENCE_WRITER_H

#include "persistence_generation.h"

typedef struct persistence_writer persistence_writer_t;

typedef enum {
    PERSISTENCE_WRITER_IDLE = 0,
    PERSISTENCE_WRITER_RUNNING,
    PERSISTENCE_WRITER_SUCCEEDED,
    PERSISTENCE_WRITER_FAILED,
} persistence_writer_state_t;

persistence_writer_t *persistence_writer_create(void);
void persistence_writer_destroy(persistence_writer_t *writer);

/* Takes an immutable deep snapshot before returning to the simulation loop. */
bool persistence_writer_start(
    persistence_writer_t *writer,
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS]);

bool persistence_writer_active(persistence_writer_t *writer);

/* Completed results are consumed by poll/wait and the writer returns idle. */
persistence_writer_state_t persistence_writer_poll(
    persistence_writer_t *writer,
    persistence_generation_paths_t *published);
persistence_writer_state_t persistence_writer_wait(
    persistence_writer_t *writer,
    persistence_generation_paths_t *published);

#endif
