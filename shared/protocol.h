/*
 * shared/net_protocol.h — Single source of truth for the Signal Space Miner
 * binary wire protocol.  Included by both the client (src/net.h) and the
 * authoritative server (server/net_protocol.h).
 *
 * Packet layouts (little-endian):
 *   JOIN            (0x01): [type:1][player_id:1]
 *   LEAVE           (0x02): [type:1][player_id:1]
 *   STATE           (0x03): [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1]  = 23 bytes
 *   INPUT           (0x04): [type:1][flags:1][action:1]  = 3 bytes
 *   WORLD_ASTEROIDS (0x10): [type:1][count:1] + count * 30-byte records
 *   WORLD_NPCS      (0x11): [type:1][count:1] + count * 26-byte records
 *   WORLD_STATIONS  (0x12): [type:1][count:1] + count * STATION_RECORD_SIZE records
 *   PLAYER_SHIP     (0x15): [type:1][id:1] + ship cargo/hull/credits/levels
 *   SERVER_INFO     (0x16): [type:1][hash:up to 11]
 *   STATION_IDENTITY(0x17): [type:1][index:1][reserved:1][services:4][pos:2xf32][radius:f32][dock_radius:f32][signal_range:f32][name:32]
 *   WORLD_PLAYERS   (0x18): [type:1][count:1] + count * PLAYER_RECORD_SIZE records
 */
#ifndef SHARED_PROTOCOL_H
#define SHARED_PROTOCOL_H

/* ------------------------------------------------------------------ */
/* Message types                                                      */
/* ------------------------------------------------------------------ */

enum {
    NET_MSG_JOIN            = 0x01,
    NET_MSG_LEAVE           = 0x02,
    NET_MSG_STATE           = 0x03,
    NET_MSG_INPUT           = 0x04,
    NET_MSG_WORLD_ASTEROIDS = 0x10,
    NET_MSG_WORLD_NPCS      = 0x11,
    NET_MSG_WORLD_STATIONS  = 0x12,
    NET_MSG_MINING_ACTION   = 0x13,
    NET_MSG_HOST_ASSIGN     = 0x14,
    NET_MSG_PLAYER_SHIP     = 0x15,
    NET_MSG_SERVER_INFO     = 0x16,
    NET_MSG_STATION_IDENTITY= 0x17,
    NET_MSG_WORLD_PLAYERS   = 0x18,
};

/* ------------------------------------------------------------------ */
/* Input flags (client -> server), packed into one byte                */
/* ------------------------------------------------------------------ */

enum {
    NET_INPUT_THRUST = 1 << 0,
    NET_INPUT_LEFT   = 1 << 1,
    NET_INPUT_RIGHT  = 1 << 2,
    NET_INPUT_FIRE   = 1 << 3,
    NET_INPUT_BRAKE  = 1 << 4,
};

/* ------------------------------------------------------------------ */
/* Station action byte values (sent inside INPUT packets)             */
/* ------------------------------------------------------------------ */

enum {
    NET_ACTION_NONE           = 0,
    NET_ACTION_DOCK           = 1,
    NET_ACTION_LAUNCH         = 2,
    NET_ACTION_SELL_CARGO     = 3,
    NET_ACTION_REPAIR         = 4,
    NET_ACTION_UPGRADE_MINING = 5,
    NET_ACTION_UPGRADE_HOLD   = 6,
    NET_ACTION_UPGRADE_TRACTOR= 7,
    NET_ACTION_PLACE_OUTPOST  = 8,
    NET_ACTION_BUILD_MODULE   = 9,  /* +module_type offset */
    NET_ACTION_BUY_SCAFFOLD   = 25,
    NET_ACTION_BUY_PRODUCT    = 30, /* +commodity offset */
};

/* ------------------------------------------------------------------ */
/* Record sizes                                                       */
/* ------------------------------------------------------------------ */

/* Station economic snapshot: [index:1][ore_buf:3xf32][inv:6xf32][prod:3xf32] */
#define STATION_RECORD_SIZE 53

/* Player state record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1] */
#define PLAYER_RECORD_SIZE 22

/* Asteroid record: [id+flags:1][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32][radius:f32] */
#define ASTEROID_RECORD_SIZE 30

/* NPC record: [id+flags:1][flags:1][pos:2xf32][vel:2xf32][angle:f32][target:1][tint:3] */
#define NPC_RECORD_SIZE 26

/* Station identity message payload (after type byte): [index:1][reserved:1][services:4][pos:2xf32][radius:f32][dock_radius:f32][signal_range:f32][name:32] */
#define STATION_IDENTITY_SIZE 59

#endif /* SHARED_PROTOCOL_H */
