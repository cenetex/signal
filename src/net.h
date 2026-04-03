/*
 * net.h — Multiplayer networking layer for Signal Space Miner.
 *
 * Provides WebSocket-based connectivity to the relay server.
 * Uses emscripten WebSocket API for WASM builds; native builds
 * are stubbed with a TODO for future POSIX implementation.
 *
 * Binary protocol (little-endian):
 *   JOIN  (0x01): 1 type + 1 player_id
 *   LEAVE (0x02): 1 type + 1 player_id
 *   STATE (0x03): 1 type + 1 player_id + 5 float32 (x, y, vx, vy, angle)
 *   INPUT (0x04): 1 type + 1 flags + 1 action + 1 mining_target
 *   ASTEROID_UPDATE (0x05): relay-only
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>
#include "types.h"      /* COMMODITY_COUNT */
#include "protocol.h"   /* shared protocol enums, message types, record sizes */

enum {
    NET_MAX_PLAYERS = 32,
};

typedef struct {
    uint8_t player_id;
    float x, y;
    float vx, vy;
    float angle;
    uint8_t flags;  /* bit0=thrust, bit1=mining, bit2=docked */
    bool active;
} NetPlayerState;

/* Packed asteroid state for world sync (30 bytes per asteroid). */
typedef struct {
    uint8_t index;      /* asteroid slot 0-254 */
    uint8_t flags;      /* bit0=active, bit1=fracture_child, bits2-4=tier(3), bits5-7=commodity(3) */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float hp;           /* current HP */
    float ore;          /* ore amount (for TIER_S) */
    float radius;       /* radius */
} NetAsteroidState;

/* Packed NPC state for world sync (23 bytes per NPC). */
typedef struct {
    uint8_t index;      /* NPC slot 0-15 */
    uint8_t flags;      /* bit0=active, bits1-2=role, bits3-5=state, bit6=thrusting */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float angle;        /* facing */
    int8_t target_asteroid; /* mining target (-1 for none) */
    uint8_t tint_r, tint_g, tint_b; /* accumulated ore color */
} NetNpcState;

/* Callbacks — set these before calling net_init(). */
typedef void (*net_on_player_join_fn)(uint8_t player_id);
typedef void (*net_on_player_leave_fn)(uint8_t player_id);
typedef void (*net_on_player_state_fn)(const NetPlayerState* state);
typedef void (*net_on_asteroids_fn)(const NetAsteroidState* asteroids, int count);
typedef void (*net_on_npcs_fn)(const NetNpcState* npcs, int count);
/* Packed player ship state (from PLAYER_SHIP 0x15). */
typedef struct {
    uint8_t player_id;
    float hull;
    float credits;
    bool docked;
    uint8_t current_station;
    uint8_t mining_level;
    uint8_t hold_level;
    uint8_t tractor_level;
    bool has_scaffold_kit;
    float cargo[COMMODITY_COUNT];
} NetPlayerShipState;

typedef void (*net_on_player_ship_fn)(const NetPlayerShipState* state);

/* Station update callback: index + full inventory[COMMODITY_COUNT]. */
typedef void (*net_on_stations_fn)(uint8_t index, const float* inventory);

/* Contracts callback: full replacement of contract array. */
typedef void (*net_on_contracts_fn)(const contract_t* contracts, int count);

/* Packed station identity for network sync. */
typedef struct {
    uint8_t index;
    uint8_t flags;
    uint32_t services;
    float pos_x, pos_y;
    float radius, dock_radius, signal_range;
    char name[32];
    float base_price[COMMODITY_COUNT];
    float scaffold_progress;
    int module_count;
    station_module_t modules[MAX_MODULES_PER_STATION];
} NetStationIdentity;

/* Station identity callback: full static fields for a station slot. */
typedef void (*net_on_station_identity_fn)(const NetStationIdentity* station);

typedef void (*net_on_players_begin_fn)(void);

typedef struct {
    net_on_player_join_fn on_join;
    net_on_player_leave_fn on_leave;
    net_on_player_state_fn on_state;
    net_on_players_begin_fn on_players_begin;
    net_on_asteroids_fn on_asteroids;
    net_on_npcs_fn on_npcs;
    net_on_stations_fn on_stations;
    net_on_station_identity_fn on_station_identity;
    net_on_player_ship_fn on_player_ship;
    net_on_contracts_fn on_contracts;
} NetCallbacks;

/* Initialize networking and connect to the relay server.
 * url: WebSocket URL, e.g. "ws://localhost:8080/ws"
 * Returns true if connection was initiated. */
bool net_init(const char* url, const NetCallbacks* callbacks);

/* Shut down the connection and free resources. */
void net_shutdown(void);

/* Send an 8-byte session token to the server for save identification.
 * Called automatically on JOIN. */
void net_send_session(const uint8_t token[8]);

/* Send the local player's input state to the server.
 * flags: bitmask of NET_INPUT_* values.
 * action: station interaction (0=none, 1=dock, 2=launch, etc.)
 * mining_target: client's hover_asteroid index (255=none) */
void net_send_input(uint8_t flags, uint8_t action, uint8_t mining_target);

/* Send the local player's full state to the server for relay. */
void net_send_state(float x, float y, float vx, float vy, float angle);

/* Process incoming messages. Call once per frame. */
void net_poll(void);

/* Returns true if connected to the relay server. */
bool net_is_connected(void);

/* Returns the local player's assigned ID, or 0xFF if not assigned. */
uint8_t net_local_id(void);

/* Access remote player state array (NET_MAX_PLAYERS entries). */
const NetPlayerState* net_get_players(void);

/* Returns the number of currently active remote players. */
int net_remote_player_count(void);

/* Returns the server's git hash (empty string if not received). */
const char* net_server_hash(void);

#endif /* NET_H */
