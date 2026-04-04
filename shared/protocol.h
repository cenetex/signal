/*
 * shared/net_protocol.h — Single source of truth for the Signal Space Miner
 * binary wire protocol.  Included by both the client (src/net.h) and the
 * authoritative server (server/net_protocol.h).
 *
 * Packet layouts (little-endian):
 *   JOIN            (0x01): [type:1][player_id:1]
 *   LEAVE           (0x02): [type:1][player_id:1]
 *   STATE           (0x03): [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:10]  = 35 bytes
 *   INPUT           (0x04): [type:1][flags:1][action:1][mining_target:1]  = 4 bytes
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

#include "types.h"  /* MODULE_COUNT, COMMODITY_COUNT, MAX_ASTEROIDS, etc. */

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
    NET_MSG_CONTRACTS       = 0x19,
    NET_MSG_SESSION         = 0x20, /* client -> server: [type:1][token:8] */
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
    NET_ACTION_BUILD_MODULE   = 9,  /* +module_type offset, range [9..9+MODULE_COUNT) */
    NET_ACTION_BUY_SCAFFOLD   = 25,
    NET_ACTION_HAIL           = 26,  /* collect pending credits via signal hail */
    NET_ACTION_BUY_PRODUCT    = 30, /* +commodity offset, range [30..30+COMMODITY_COUNT) */
};

/* Compile-time check: module and scaffold action ranges must not overlap */
_Static_assert(NET_ACTION_BUILD_MODULE + MODULE_COUNT <= NET_ACTION_BUY_SCAFFOLD,
               "BUILD_MODULE range overlaps BUY_SCAFFOLD");
_Static_assert(NET_ACTION_BUY_SCAFFOLD < NET_ACTION_BUY_PRODUCT,
               "BUY_SCAFFOLD overlaps BUY_PRODUCT range");
_Static_assert(NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT <= 256,
               "BUY_PRODUCT range overflows uint8_t");

/* ------------------------------------------------------------------ */
/* Record sizes                                                       */
/* ------------------------------------------------------------------ */

/* Station economic snapshot: [index:1][inventory:COMMODITY_COUNT×f32] */
#define STATION_RECORD_SIZE (1 + COMMODITY_COUNT * 4)  /* 37 bytes when COMMODITY_COUNT == 9 */

/* Player state record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:10] */
#define PLAYER_RECORD_SIZE 34

/* Asteroid record: [id+flags:1][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32][radius:f32] */
#define ASTEROID_RECORD_SIZE 30

/* NPC record: [id+flags:1][flags:1][pos:2xf32][vel:2xf32][angle:f32][target:1][tint:3] */
#define NPC_RECORD_SIZE 26

/* Station identity: [index:1][flags:1][services:4][pos:2xf32][radius:f32][dock_radius:f32][signal_range:f32][name:32]
 * [base_price:COMMODITY_COUNT×f32][scaffold_progress:f32][module_count:1][modules:MAX_MODULES×8]
 * [arm_count:1][arm_speed:MAX_ARMS×f32] */
#define STATION_MODULE_RECORD_SIZE 8  /* type:1 + scaffold:1 + ring:1 + slot:1 + build_progress:f32 */
#define STATION_IDENTITY_SIZE (59 + COMMODITY_COUNT * 4 + 4 + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE + 1 + MAX_ARMS * 4)

/* Player ship state: [type:1][id:1][hull:f32][credits:f32][docked:1][station:1]
 * [mining:1][hold:1][tractor:1][scaffold_kit:1][cargo:COMMODITY_COUNT×f32]
 * [nearby_frags:1][tractor_frags:1][towed_count:1][towed_frags:10] */
#define PLAYER_SHIP_SIZE (29 + COMMODITY_COUNT * 4)  /* 65 bytes when COMMODITY_COUNT == 9 */

#endif /* SHARED_PROTOCOL_H */
