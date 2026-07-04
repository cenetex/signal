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
    /* false (default): emit authoritative snapshots every tick. The
     * loopback transport is an in-process memcpy — there is no bandwidth
     * to save, and anything less than per-tick pose data renders as
     * visible stepping in singleplayer.
     * true: mirror the dedicated server's throttled broadcast cadences
     * (20 Hz player / 10 Hz world / 4 Hz private) so local mode exercises
     * the same prediction + dead-reckoning path as multiplayer. Opt-in
     * via ?netcadence=1 (web) or SIGNAL_LOCAL_NET_CADENCE=1 (native). */
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
