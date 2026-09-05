#ifndef SIGNAL_LOCAL_SAVE_H
#define SIGNAL_LOCAL_SAVE_H

#include "game_sim.h"

typedef struct local_save local_save_t;

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
