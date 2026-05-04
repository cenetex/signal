/*
 * net_sync.h -- Multiplayer network state synchronization for the
 * Signal Space Miner client.  Handles applying server-authoritative
 * state to the local world and interpolating for smooth rendering.
 */
#ifndef NET_SYNC_H
#define NET_SYNC_H

#include "client.h"
#include "net.h"

/* Player join/leave callbacks. */
void on_player_join(uint8_t player_id);
void on_player_leave(uint8_t player_id);

/* Apply server-authoritative world state. */
void apply_remote_asteroids(const NetAsteroidState* asteroids, int count);
void apply_remote_npcs(const NetNpcState* npcs, int count);
void apply_remote_stations(uint8_t index, const float* inventory, float credit_pool);
void apply_remote_contracts(const contract_t* contracts, int count);
void apply_remote_station_identity(const NetStationIdentity* si);
void apply_remote_scaffolds(const NetScaffoldState* scaffolds, int count);
void apply_remote_hail_response(uint8_t station, float credits, int contract_index);
void apply_remote_signal_channel(const NetSignalChannelMsg *msgs, int count);
/* Phase 2 — station manifest summary (per-{commodity, grade} counts). */
void apply_remote_station_manifest(uint8_t station_id,
                                   const NetStationManifestEntry *entries,
                                   int count);
/* Local player ship manifest summary (server-mirrored). */
void apply_remote_player_manifest(const NetStationManifestEntry *entries,
                                  int count);
/* Detailed named-ingot snapshots that supplement manifest summaries with
 * per-unit provenance for trade-row display. */
void apply_remote_station_ingots(uint8_t station_id,
                                 const NetNamedIngotEntry *entries,
                                 int count);
void apply_remote_hold_ingots(const NetNamedIngotEntry *entries, int count);
/* Global leaderboard snapshot. */
void apply_remote_highscores(const NetHighscoreEntry *entries, int count);
void apply_remote_events(const sim_event_t *events, int count);
void begin_player_state_batch(void);
void apply_remote_player_state(const NetPlayerState* state);
void apply_remote_player_ship(const NetPlayerShipState* state);

/* Death event from server — drives the death cinematic. respawn_station
 * + respawn_fee carry the per-station spawn fee that was just debited
 * (rendered on the death overlay). */
void on_remote_death(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured,
                     uint8_t respawn_station, float respawn_fee);

/* World time sync from server. */
void on_remote_world_time(float server_time);

/* Multiplayer station ring prediction/correction. */
void reset_station_ring_smoothing(void);
void step_remote_station_rings(float dt);

/* Sync local player slot to the network-assigned ID. */
void sync_local_player_slot_from_network(void);

/* Interpolate asteroid, NPC, and player positions for smooth multiplayer rendering. */
void interpolate_world_for_render(void);

/* Get interpolated remote player states for rendering. */
const NetPlayerState* net_get_interpolated_players(void);

/* Contextual hail message for a starter station (0-2). Returns NULL for
 * outposts or if no condition matches. Defined in main.c. */
const char *contextual_hail_message(int station_index);

#endif /* NET_SYNC_H */
