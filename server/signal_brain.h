/*
 * signal_brain.h -- Server-side CRLP neural flight controller.
 *
 * The mining objective loop can remain deterministic while this module
 * replaces per-tick WASD flight choices with a trained signal-flight-live-v2
 * checkpoint scorer.
 */
#ifndef SIGNAL_BRAIN_H
#define SIGNAL_BRAIN_H

#include "game_sim.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool signal_brain_load_checkpoint(const char *path, char *err, size_t err_size);
bool signal_brain_loaded(void);
uint64_t signal_brain_inference_count(void);
void signal_brain_drive(world_t *w, server_player_t *sp, float dt);

#endif /* SIGNAL_BRAIN_H */
