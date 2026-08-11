/*
 * local_server.h -- In-process authoritative simulation for singleplayer.
 *
 * Runs world_sim_step() locally so singleplayer uses the same
 * client-prediction + server-authoritative architecture as multiplayer.
 */
#ifndef LOCAL_SERVER_H
#define LOCAL_SERVER_H

#include "game_sim.h"
#include "local_authority.h"

typedef struct {
    local_authority_t authority;
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
    /* Counts successful world initializations.  Preserved across shutdown so
     * restart tests and diagnostics can distinguish a fresh authority. */
    uint32_t generation;
} local_server_t;

/* Allocate/initialize the local server world and spawn the player.  The
 * object must be zero-initialized before its first call.  Reinitialization is
 * safe and releases the previous world first. */
bool local_server_init(local_server_t *ls, uint32_t seed);
/* Stop and release the owned world.  Safe after failed init and when repeated. */
void local_server_shutdown(local_server_t *ls);
world_t *local_server_world(local_server_t *ls);
const world_t *local_server_world_const(const local_server_t *ls);
bool local_server_has_world(const local_server_t *ls);

/* Attach the in-process server to net.c's loopback transport and drive it
 * through the same client networking path used by multiplayer. */
void local_server_attach_loopback(local_server_t *ls);
/* Issue and complete the one-time pubkey challenge before sending world
 * state. Returns false and disables the loopback authority if authentication
 * cannot be established. */
bool local_server_send_initial_snapshot(local_server_t *ls, int player_slot);
void local_server_step_loopback(local_server_t *ls, int player_slot, float dt);

#endif /* LOCAL_SERVER_H */
