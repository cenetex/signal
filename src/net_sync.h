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
void apply_remote_hail_response(uint8_t station, float credits);
void apply_remote_events(const sim_event_t *events, int count);
void begin_player_state_batch(void);
void apply_remote_player_state(const NetPlayerState* state);
void apply_remote_player_ship(const NetPlayerShipState* state);

/* Death event from server — drives the death cinematic. */
void on_remote_death(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured);

/* World time sync from server. */
void on_remote_world_time(float server_time);

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
