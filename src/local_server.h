/*
 * local_server.h -- In-process authoritative simulation for singleplayer.
 *
 * Runs world_sim_step() locally so singleplayer uses the same
 * client-prediction + server-authoritative architecture as multiplayer.
 */
#ifndef LOCAL_SERVER_H
#define LOCAL_SERVER_H

#include "game_sim.h"

typedef struct {
    world_t world;
    bool active;
} local_server_t;

/* Initialize the local server world and spawn the player. */
void local_server_init(local_server_t *ls, uint32_t seed);

/* Run one authoritative sim tick. */
void local_server_step(local_server_t *ls, int player_slot,
                        const input_intent_t *input, float dt);

/* Copy authoritative state into the client's interpolation buffers. */
void local_server_sync_to_client(const local_server_t *ls);

#endif /* LOCAL_SERVER_H */
