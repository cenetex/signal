/*
 * signal_brain.h -- Server-side neural and holographic flight controllers.
 *
 * The mining objective loop can remain deterministic while this module
 * replaces per-tick WASD flight choices with a trained signal-flight-live-v2
 * checkpoint scorer or a holographic VSA-based associative memory.
 */
#ifndef SIGNAL_BRAIN_H
#define SIGNAL_BRAIN_H

#include "game_sim.h"
#include "holographic_nn.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool signal_brain_load_checkpoint(const char *path, char *err, size_t err_size);
bool signal_brain_loaded(void);
uint64_t signal_brain_inference_count(void);
void signal_brain_drive(world_t *w, server_player_t *sp, float dt);

/* Score NPC flight toward an explicit target. Returns true when a loaded
 * neural checkpoint produced turn/thrust intent for this tick. */
bool signal_brain_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target);

/* NPC holographic brain: uses VSA associative memory to select flight
 * actions. Initializes per-NPC memory on first call and stores
 * state->action associations into a bundled holographic trace.
 * Requires brain_mode == SERVER_BRAIN_MODE_HOLOGRAPHIC on the NPC. */
void signal_brain_drive_npc(world_t *w, npc_ship_t *npc, float dt);

/* Initialize the global holographic action table (called once at
 * server start). */
void signal_brain_holographic_init(void);

extern bool g_neural_singleplayer;

#endif /* SIGNAL_BRAIN_H */
