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
    /* Flags bit layout:
     *   bit0 = thrust
     *   bit1 = beam_active (the beam is firing — may or may not have hit)
     *   bit2 = docked
     *   bit3 = scan_active
     *   bit4 = tractor_active
     *   bit5 = beam_ineffective (laser too weak for the target tier)
     *   bit6 = beam_hit (beam terminates on a target instead of empty space)
     */
    uint8_t flags;
    uint8_t tractor_level;
    uint8_t towed_count;
    uint8_t towed_fragments[10]; /* asteroid indices, 0xFF = unused */
    char callsign[8];            /* e.g. "KRX-472" */
    /* Beam endpoints — server-authoritative. Used for both local and
     * remote player beam visuals. */
    float beam_start_x, beam_start_y;
    float beam_end_x, beam_end_y;
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
    uint8_t autopilot_mode; /* 0 = off, 1 = mining loop */
    float cargo[COMMODITY_COUNT];
    uint8_t nearby_fragments;
    uint8_t tractor_fragments;
    uint8_t towed_count;
    uint8_t towed_fragments[10]; /* asteroid indices, 0xFF = unused */
    uint8_t autopilot_target;    /* asteroid index, 0xFF = none */
} NetPlayerShipState;

typedef void (*net_on_player_ship_fn)(const NetPlayerShipState* state);

/* Station update callback: index + full inventory[COMMODITY_COUNT]. */
typedef void (*net_on_stations_fn)(uint8_t index, const float* inventory);

/* Contracts callback: full replacement of contract array. */
typedef void (*net_on_contracts_fn)(const contract_t* contracts, int count);

/* Packed station identity for network sync.
 * flags: bit0=scaffold, bit1=planned. */
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
    int arm_count;
    float arm_speed[MAX_ARMS];
    float ring_offset[MAX_ARMS];
    int plan_count;
    struct {
        module_type_t type;
        uint8_t ring;
        uint8_t slot;
        int8_t owner;
    } plans[STATION_PLAN_RECORD_COUNT];
    int pending_scaffold_count;
    struct {
        module_type_t type;
        int8_t owner;
    } pending_scaffolds[STATION_PENDING_SCAFFOLD_RECORD_COUNT];
} NetStationIdentity;

/* Packed scaffold state — server pushes the active scaffold pool.
 * Mirrors enough of scaffold_t for client rendering + tow logic. */
typedef struct {
    uint8_t index;
    uint8_t state;        /* scaffold_state_t enum */
    uint8_t module_type;
    int8_t  owner;        /* -1 for NPC-produced */
    float   pos_x, pos_y;
    float   vel_x, vel_y;
    float   radius;
    float   build_amount;
} NetScaffoldState;

/* Station identity callback: full static fields for a station slot. */
typedef void (*net_on_station_identity_fn)(const NetStationIdentity* station);
/* Scaffold pool snapshot callback. */
typedef void (*net_on_scaffolds_fn)(const NetScaffoldState* scaffolds, int count);
/* Hail response callback: server confirmed payout from a hail attempt. */
typedef void (*net_on_hail_response_fn)(uint8_t station, float credits);

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
    net_on_scaffolds_fn on_scaffolds;
    net_on_hail_response_fn on_hail_response;
    net_on_player_ship_fn on_player_ship;
    net_on_contracts_fn on_contracts;
    void (*on_death)(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured);
    void (*on_world_time)(float server_time);
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

/* Send a planning intent (outpost create / module slot / cancel). */
void net_send_plan(uint8_t op, int8_t station, int8_t ring, int8_t slot,
                   uint8_t module_type, float px, float py);

/* Send the local player's full state to the server for relay. */
void net_send_state(float x, float y, float vx, float vy, float angle);

/* Process incoming messages. Call once per frame. */
void net_poll(void);

/* Returns true if connected to the relay server. */
bool net_is_connected(void);

/* Returns the local player's assigned ID, or 0xFF if not assigned. */
uint8_t net_local_id(void);
const char* net_local_callsign(void);

/* Access remote player state array (NET_MAX_PLAYERS entries). */
const NetPlayerState* net_get_players(void);

/* Returns the number of currently active remote players. */
int net_remote_player_count(void);

/* Returns the server's git hash (empty string if not received). */
const char* net_server_hash(void);

#endif /* NET_H */
