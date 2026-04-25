/*
 * shared/net_protocol.h — Single source of truth for the Signal Space Miner
 * binary wire protocol.  Included by both the client (src/net.h) and the
 * authoritative server (server/net_protocol.h).
 *
 * Packet layouts (little-endian):
 *   JOIN            (0x01): [type:1][player_id:1]
 *   LEAVE           (0x02): [type:1][player_id:1]
 *   STATE           (0x03): [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:20]  = 45 bytes (towed_frags = 10 × uint16_t, 0xFFFF = unused)
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
    NET_MSG_DEATH           = 0x21, /* server -> client: [type:1][player_id:1] */
    NET_MSG_WORLD_TIME      = 0x22, /* server -> client: [type:1][time:f32] */
    NET_MSG_PLAN            = 0x23, /* client -> server: outpost planning intents */
    NET_MSG_WORLD_SCAFFOLDS = 0x24, /* server -> client: active scaffold pool */
    NET_MSG_HAIL_RESPONSE   = 0x25, /* server -> client: hail collected payout */
    NET_MSG_EVENTS          = 0x26, /* server -> client: sim event batch */
    NET_MSG_SIGNAL_CHANNEL  = 0x27, /* server -> client: broadcast-log snapshot / append (#316) */
    NET_MSG_STATION_INGOTS  = 0x28, /* server -> client: station's named-ingot stockpile (RATi v2) */
    NET_MSG_HOLD_INGOTS     = 0x29, /* server -> client: local player's hold ingots (RATi v2) */
    NET_MSG_BUY_INGOT       = 0x2A, /* client -> server: [type:1][pubkey:32] purchase from current docked station */
    NET_MSG_DELIVER_INGOT   = 0x2B, /* client -> server: [type:1][hold_index:1] deposit to current docked station */
    NET_MSG_FRACTURE_CHALLENGE = 0x2C, /* server -> nearby clients: [type:1][fracture_id:u32][seed:32][deadline_ms:u32][burst_cap:u16] */
    NET_MSG_FRACTURE_CLAIM     = 0x2D, /* client -> server: [type:1][fracture_id:u32][burst_nonce:u32][claimed_grade:u8] */
    NET_MSG_FRACTURE_RESOLVED  = 0x2E, /* server -> nearby clients: [type:1][fracture_id:u32][fragment_pub:32][winner_pub:32][grade:u8] */
    NET_MSG_STATION_MANIFEST   = 0x2F, /* server -> client: per-station manifest summary grouped by (commodity, grade) — see STATION_MANIFEST_* below. */
    NET_MSG_HIGHSCORES         = 0x30, /* server -> client: top-N leaderboard. [type:1][count:1] + count × [callsign:8][credits_earned:f32] */
};

/* Top-N global leaderboard persisted server-side, broadcast on join and
 * after every death. */
enum {
    HIGHSCORE_TOP_N      = 10,
    HIGHSCORE_ENTRY_SIZE = 12,   /* 8-byte callsign + f32 credits_earned */
    HIGHSCORE_HEADER     = 2,    /* type + count */
};

/* NET_MSG_STATION_MANIFEST wire layout:
 *   [type:1][station_idx:1][entry_count:u16]
 *     entry_count × [commodity:1][grade:1][count:u16]   (little-endian) */
enum {
    STATION_MANIFEST_HEADER = 4,
    STATION_MANIFEST_ENTRY  = 4,
};

/* Per-class buy price at any station's stockpile. RATi/commissioned
 * are an order of magnitude scarcer so they cost proportionally more.
 * Indexed by ingot_prefix_t. Anonymous = 0 (not purchasable). */
#define INGOT_PRICE_M             1500
#define INGOT_PRICE_H             1500
#define INGOT_PRICE_T             1500
#define INGOT_PRICE_S             1500
#define INGOT_PRICE_F             1500
#define INGOT_PRICE_K             1500
#define INGOT_PRICE_RATI          35000
#define INGOT_PRICE_COMMISSIONED  100000
/* Delivery credit paid to the player when they deposit a named ingot
 * at a station's stockpile — small flat reward for transit. */
#define INGOT_DELIVERY_CREDIT     100

/* Named ingot wire record: [pubkey:32][prefix:1][metal:1][_pad:2][mined_block:8][origin:1][_pad2:7] = 52 bytes
 * Mirrors named_ingot_t exactly so the server can write the struct
 * directly. Class authorization is in the leading char(s) of base58(pubkey). */
#define NAMED_INGOT_RECORD_SIZE 52

/* NET_MSG_STATION_INGOTS layout:
 *   [type:1][station_id:1][count:1] + count × NAMED_INGOT_RECORD_SIZE
 * NET_MSG_HOLD_INGOTS layout (player is implicit — local pilot):
 *   [type:1][count:1] + count × NAMED_INGOT_RECORD_SIZE */
#define STATION_INGOTS_HEADER 3
#define HOLD_INGOTS_HEADER    2

/* Client-hashed fracture window */
#define FRACTURE_CHALLENGE_BURST_CAP 50
#define FRACTURE_CHALLENGE_SIZE      (1 + 4 + 32 + 4 + 2)
#define FRACTURE_CLAIM_SIZE          (1 + 4 + 4 + 1)
#define FRACTURE_RESOLVED_SIZE       (1 + 4 + 32 + 32 + 1)

/* Signal channel wire record:
 *   [id:u64][ts_ms:u32][sender:i8][text_len:u8][text:200][entry_hash:32] = 246 bytes
 * audio_url is server-side only for V1; agents read it via REST.
 * entry_hash is the SHA-256 chain link — clients can recompute and
 * verify against this value to detect tampering / desync. */
#define SIGNAL_CHANNEL_RECORD_SIZE (8 + 4 + 1 + 1 + 200 + 32)

/* ------------------------------------------------------------------ */
/* Plan operations (NET_MSG_PLAN payload byte 1)                      */
/* Layout: [type:1][op:1][station:1][ring:1][slot:1][module_type:1]   */
/*         [px:f32][py:f32]  = 14 bytes                               */
/* ------------------------------------------------------------------ */

enum {
    NET_PLAN_OP_NONE              = 0,
    NET_PLAN_OP_CREATE_OUTPOST    = 1, /* uses px,py */
    NET_PLAN_OP_ADD_SLOT          = 2, /* uses station,ring,slot,module_type */
    NET_PLAN_OP_CANCEL_OUTPOST    = 3, /* uses station */
    /* Atomic create + first plan: outpost is born at (px,py) AND the
     * first slot plan is added in the same input pulse. The client uses
     * this when leaving "ghost preview" mode so the lock-in position is
     * the player's current ship pos at the moment they pressed E. */
    NET_PLAN_OP_CREATE_AND_ADD    = 4, /* uses px,py + ring,slot,module_type */
    /* Cancel a single plan slot (red/clear state). */
    NET_PLAN_OP_CANCEL_PLAN_SLOT  = 5, /* uses station,ring,slot */
};

#define NET_PLAN_MSG_SIZE 14

/* ------------------------------------------------------------------ */
/* Input flags (client -> server), packed into one byte                */
/* ------------------------------------------------------------------ */

enum {
    NET_INPUT_THRUST = 1 << 0,
    NET_INPUT_LEFT   = 1 << 1,
    NET_INPUT_RIGHT  = 1 << 2,
    NET_INPUT_FIRE   = 1 << 3,
    NET_INPUT_BRAKE   = 1 << 4,
    NET_INPUT_TRACTOR = 1 << 5,
    NET_INPUT_BOOST   = 1 << 6,
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
    NET_ACTION_BUILD_MODULE   = 9,  /* DEPRECATED #259 — legacy build menu, no-op on server */
    NET_ACTION_BUY_SCAFFOLD   = 25,
    NET_ACTION_HAIL           = 26,  /* collect pending credits via signal hail */
    NET_ACTION_RELEASE_TOW    = 27,  /* tap R: release towed fragments (no longer toggles) */
    NET_ACTION_RESET          = 28,  /* self-destruct — respawn at nearest station */
    NET_ACTION_BUY_PRODUCT    = 30, /* +commodity offset, range [30..30+COMMODITY_COUNT) */
    NET_ACTION_PLACE_MODULE   = 49, /* DEPRECATED #259 — legacy placement, no-op on server (range sentinel) */
    NET_ACTION_BUY_SCAFFOLD_TYPED = 50, /* +module_type offset, range [50..50+MODULE_COUNT) */
    NET_ACTION_DELIVER_COMMODITY  = 70, /* +commodity offset, range [70..70+COMMODITY_COUNT) */
    NET_ACTION_AUTOPILOT_TOGGLE   = 90, /* toggle player mining autopilot on/off */
};

/* ------------------------------------------------------------------ */
/* Event broadcast (NET_MSG_EVENTS)                                   */
/* ------------------------------------------------------------------ */

/* Fixed-size event record: [type:1][player_id:1][payload:16] = 18 bytes.
 * Payload meaning depends on type — unused bytes are zero. */
enum {
    NET_EVENT_RECORD_SIZE   = 18,
};

/* Compile-time check: action ranges must not overlap.
 * BUILD_MODULE is deprecated and no-op, so its range collapses to a
 * single byte; new module types can grow MODULE_COUNT freely. */
_Static_assert(NET_ACTION_BUY_SCAFFOLD < NET_ACTION_BUY_PRODUCT,
               "BUY_SCAFFOLD overlaps BUY_PRODUCT range");
_Static_assert(NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT <= NET_ACTION_PLACE_MODULE,
               "BUY_PRODUCT range overlaps PLACE_MODULE");
_Static_assert(NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT <= NET_ACTION_DELIVER_COMMODITY,
               "BUY_SCAFFOLD_TYPED overlaps DELIVER_COMMODITY range");
_Static_assert(NET_ACTION_DELIVER_COMMODITY + COMMODITY_COUNT <= 256,
               "DELIVER_COMMODITY range overflows uint8_t");

/* ------------------------------------------------------------------ */
/* Record sizes                                                       */
/* ------------------------------------------------------------------ */

/* Station economic snapshot: [index:1][inventory:COMMODITY_COUNT×f32] */
#define STATION_RECORD_SIZE (1 + COMMODITY_COUNT * 4 + 4)  /* 41 bytes: index + inventory + credit_pool */

/* Player state record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:20][callsign:7]
 * [beam_start_x:f32][beam_start_y:f32][beam_end_x:f32][beam_end_y:f32]
 * towed_frags: 10 × uint16_t asteroid index, 0xFFFF = unused. Widened
 * from uint8_t in #285 Phase 3 so slots 255-2047 survive the wire.
 * flags bits: 1=thrust 2=beam_active+hit 4=docked 8=scan 16=tractor 32=beam_ineffective
 * Beam coords are server-authoritative — fixes autopilot mining visuals
 * and (eventually) combat hit prediction. */
#define PLAYER_RECORD_SIZE 67  /* 51 + 16 bytes beam coords */

/* Asteroid record: [index:2][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32][radius:f32][smelt:u8][grade:u8] */
#define ASTEROID_RECORD_SIZE 33  /* uint16 index + flags + 7 floats + smelt:u8 + grade:u8 */

/* NPC record: [id+flags:1][flags:1][pos:2xf32][vel:2xf32][angle:f32][target:1][tint:3] */
#define NPC_RECORD_SIZE 26

/* Station identity: [index:1][flags:1][services:4][pos:2xf32][radius:f32][dock_radius:f32][signal_range:f32][name:32]
 * [base_price:COMMODITY_COUNT×f32][scaffold_progress:f32][module_count:1][modules:MAX_MODULES×8]
 * [arm_count:1][arm_speed:MAX_ARMS×f32][ring_offset:MAX_ARMS×f32]
 * [plan_count:1][plans:8 × (type:1, ring:1, slot:1, owner:1)]
 * [pending_count:1][pending:4 × (type:1, owner:1)]
 * flags: bit0=scaffold, bit1=planned */
#define STATION_MODULE_RECORD_SIZE 8  /* type:1 + scaffold:1 + ring:1 + slot:1 + build_progress:f32 */
#define STATION_PLAN_RECORD_SIZE 4    /* type:1 + ring:1 + slot:1 + owner:1 */
#define STATION_PLAN_RECORD_COUNT 8
#define STATION_PENDING_SCAFFOLD_RECORD_SIZE 2  /* type:1 + owner:1 */
#define STATION_PENDING_SCAFFOLD_RECORD_COUNT 4
#define STATION_IDENTITY_CURRENCY_NAME_LEN 32  /* trailer: per-station scrip label */
#define STATION_IDENTITY_SIZE (59 + COMMODITY_COUNT * 4 + 4 \
    + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE \
    + 1 + MAX_ARMS * 4 + MAX_ARMS * 4 \
    + 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE \
    + 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT * STATION_PENDING_SCAFFOLD_RECORD_SIZE \
    + STATION_IDENTITY_CURRENCY_NAME_LEN)

/* Scaffold record: [id:1][state+owner_sign:1][module_type:1][owner:1]
 *                  [pos:2xf32][vel:2xf32][radius:f32][build_amount:f32] = 28 bytes */
#define SCAFFOLD_RECORD_SIZE 28

/* Player ship state: [type:1][id:1][hull:f32][credits:f32][docked:1][station:1]
 * [mining:1][hold:1][tractor:1][scaffold_kit:1][cargo:COMMODITY_COUNT×f32]
 * [nearby_frags:1][tractor_frags:1][towed_count:1][towed_frags:20]
 * [autopilot_target:1][path_count:1][path_current:1][waypoints: count×(x:f32,y:f32)]
 * towed_frags: 10 × uint16_t asteroid index, 0xFFFF = unused. */
#define PLAYER_SHIP_SIZE (42 + COMMODITY_COUNT * 4 + 12 * 8)  /* 174 bytes max */

#endif /* SHARED_PROTOCOL_H */
