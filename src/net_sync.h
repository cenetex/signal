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
void apply_remote_stations(uint8_t index, const float* inventory);
void apply_remote_contracts(const contract_t* contracts, int count);
void apply_remote_station_identity(const NetStationIdentity* si);
void apply_remote_player_state(const NetPlayerState* state);
void apply_remote_player_ship(const NetPlayerShipState* state);

/* Sync local player slot to the network-assigned ID. */
void sync_local_player_slot_from_network(void);

/* Interpolate asteroid and NPC positions for smooth multiplayer rendering. */
void interpolate_world_for_render(void);

#endif /* NET_SYNC_H */
