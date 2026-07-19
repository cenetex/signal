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
    bool station_snapshot_dirty;
    bool private_snapshot_dirty;
    bool global_snapshot_dirty;
    /* true (default): mirror the dedicated server's bounded broadcast
     * cadences (20 Hz player / 10 Hz world / 4 Hz private). This keeps
     * loopback packet work stable and exercises the same prediction path
     * as multiplayer. Set ?netcadence=0 (web) or
     * SIGNAL_LOCAL_NET_CADENCE=0 (native) for per-tick diagnostic mode. */
    bool throttled_snapshots;
} local_server_t;

/* Initialize the local server world and spawn the player. */
void local_server_init(local_server_t *ls, uint32_t seed);

/* Attach the in-process server to net.c's loopback transport and drive it
 * through the same client networking path used by multiplayer. */
void local_server_attach_loopback(local_server_t *ls);
void local_server_send_initial_snapshot(local_server_t *ls, int player_slot);
void local_server_step_loopback(local_server_t *ls, int player_slot, float dt);

#endif /* LOCAL_SERVER_H */
