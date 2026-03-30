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
 *   INPUT (0x04): 1 type + 1 flags + 1 float32 angle
 *   ASTEROID_UPDATE (0x05): relay-only
 */
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stdint.h>

enum {
    NET_MAX_PLAYERS = 32,
};

enum {
    NET_MSG_JOIN            = 0x01,
    NET_MSG_LEAVE           = 0x02,
    NET_MSG_STATE           = 0x03,
    NET_MSG_INPUT           = 0x04,
    NET_MSG_WORLD_ASTEROIDS = 0x10,
    NET_MSG_WORLD_NPCS      = 0x11,
    NET_MSG_WORLD_STATIONS  = 0x12,
    NET_MSG_MINING_ACTION   = 0x13,
    NET_MSG_PLAYER_SHIP     = 0x15,
};

/* Input flags packed into a single byte. */
enum {
    NET_INPUT_THRUST = 1 << 0,
    NET_INPUT_LEFT   = 1 << 1,
    NET_INPUT_RIGHT  = 1 << 2,
    NET_INPUT_FIRE   = 1 << 3,
};

typedef struct {
    uint8_t player_id;
    float x, y;
    float vx, vy;
    float angle;
    bool active;
} NetPlayerState;

/* Packed asteroid state for world sync (30 bytes per asteroid). */
typedef struct {
    uint8_t index;      /* asteroid slot 0-47 */
    uint8_t flags;      /* bit0=active, bit1=fracture_child, bits2-3=tier, bits4-6=commodity */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float hp;           /* current HP */
    float ore;          /* ore amount (for TIER_S) */
    float radius;       /* radius */
} NetAsteroidState;

/* Packed NPC state for world sync (23 bytes per NPC). */
typedef struct {
    uint8_t index;      /* NPC slot 0-5 */
    uint8_t flags;      /* bit0=active, bits1-2=role, bits3-5=state, bit6=thrusting */
    float x, y;         /* position */
    float vx, vy;       /* velocity */
    float angle;        /* facing */
    int8_t target_asteroid; /* mining target (-1 for none) */
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
    float cargo_ferrite;
    float cargo_cuprite;
    float cargo_crystal;
} NetPlayerShipState;

typedef void (*net_on_player_ship_fn)(const NetPlayerShipState* state);

/* Station update callback: index, ore_buffer[3], inventory[6], product_stock[3]. */
typedef void (*net_on_stations_fn)(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock);

typedef struct {
    net_on_player_join_fn on_join;
    net_on_player_leave_fn on_leave;
    net_on_player_state_fn on_state;
    net_on_asteroids_fn on_asteroids;
    net_on_npcs_fn on_npcs;
    net_on_stations_fn on_stations;
    net_on_player_ship_fn on_player_ship;
} NetCallbacks;

/* Initialize networking and connect to the relay server.
 * url: WebSocket URL, e.g. "ws://localhost:8080/ws"
 * Returns true if connection was initiated. */
bool net_init(const char* url, const NetCallbacks* callbacks);

/* Shut down the connection and free resources. */
void net_shutdown(void);

/* Send the local player's input state to the server.
 * flags: bitmask of NET_INPUT_* values.
 * angle: current ship angle in radians.
 * action: station interaction (0=none, 1=dock, 2=launch, etc.) */
void net_send_input(uint8_t flags, float angle, uint8_t action);

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

#endif /* NET_H */
