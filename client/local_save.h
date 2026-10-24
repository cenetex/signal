#ifndef SIGNAL_LOCAL_SAVE_H
#define SIGNAL_LOCAL_SAVE_H

#include "game_sim.h"

typedef struct local_save local_save_t;

typedef enum {
    LOCAL_SAVE_PREPARING = 0,
    LOCAL_SAVE_SAVING,
    LOCAL_SAVE_SAVED,
    LOCAL_SAVE_RETRY_NEEDED,
} local_save_state_t;

typedef struct {
    local_save_state_t state;
    uint64_t completed_generation;
    double completed_at_ms; /* Zero for a restored save with unknown timing. */
    double completed_age_seconds;
} local_save_status_t;

local_save_status_t local_save_status(const local_save_t *save);
void local_save_status_text(const local_save_t *save, char *out, size_t size);


/* Own a local world's storage for one verified player. world_reset must run
 * before open; a saved catalog and world are restored as one generation. */
local_save_t *local_save_open(const char *root, world_t *world,
                              const uint8_t pubkey[32], bool *fresh);
bool local_save_restore_player(local_save_t *save, world_t *world, int slot);
void local_save_update(local_save_t *save, world_t *world, float dt);
bool local_save_request(local_save_t *save, world_t *world, bool wait);
void local_save_close(local_save_t *save, world_t *world);
uint64_t local_save_generation(const local_save_t *save);
bool local_save_failed(const local_save_t *save);

#endif
