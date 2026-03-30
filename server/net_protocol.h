/*
 * net_protocol.h -- Binary protocol for the Signal Space Miner
 * authoritative server.
 *
 * Message types match the client-side net.h definitions.
 */
#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "game_sim.h"

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
};

/* Input flags (client -> server) */
enum {
    NET_INPUT_THRUST = 1 << 0,
    NET_INPUT_LEFT   = 1 << 1,
    NET_INPUT_RIGHT  = 1 << 2,
    NET_INPUT_FIRE   = 1 << 3,
};

/* Station action byte values */
enum {
    NET_ACTION_NONE           = 0,
    NET_ACTION_DOCK           = 1,
    NET_ACTION_LAUNCH         = 2,
    NET_ACTION_SELL_CARGO     = 3,
    NET_ACTION_REPAIR         = 4,
    NET_ACTION_UPGRADE_MINING = 5,
    NET_ACTION_UPGRADE_HOLD   = 6,
    NET_ACTION_UPGRADE_TRACTOR= 7,
};

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static inline void write_f32_le(uint8_t *buf, float v) {
    union { float f; uint32_t u; } conv;
    conv.f = v;
    buf[0] = (uint8_t)(conv.u);
    buf[1] = (uint8_t)(conv.u >> 8);
    buf[2] = (uint8_t)(conv.u >> 16);
    buf[3] = (uint8_t)(conv.u >> 24);
}

static inline float read_f32_le(const uint8_t *buf) {
    union { float f; uint32_t u; } conv;
    conv.u = (uint32_t)buf[0]
           | ((uint32_t)buf[1] << 8)
           | ((uint32_t)buf[2] << 16)
           | ((uint32_t)buf[3] << 24);
    return conv.f;
}

/* ------------------------------------------------------------------ */
/* Serialisation (server -> client)                                   */
/* ------------------------------------------------------------------ */

/*
 * STATE message (22 bytes):
 * [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32]
 */
static inline int serialize_player_state(uint8_t *buf, uint8_t id, const ship_t *s) {
    buf[0] = NET_MSG_STATE;
    buf[1] = id;
    write_f32_le(&buf[2],  s->pos.x);
    write_f32_le(&buf[6],  s->pos.y);
    write_f32_le(&buf[10], s->vel.x);
    write_f32_le(&buf[14], s->vel.y);
    write_f32_le(&buf[18], s->angle);
    return 22;
}

/*
 * WORLD_ASTEROIDS message:
 * [type:1][count:1] + count * 30-byte records
 */
static inline int serialize_asteroids(uint8_t *buf, const asteroid_t *asteroids) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!asteroids[i].active) continue;
        const asteroid_t *a = &asteroids[i];
        uint8_t *p = &buf[2 + count * 30];
        p[0] = (uint8_t)i;
        p[1] = 1; /* active */
        if (a->fracture_child) p[1] |= (1 << 1);
        p[1] |= (((uint8_t)a->tier & 0x3) << 2);
        p[1] |= (((uint8_t)a->commodity & 0x7) << 4);
        write_f32_le(&p[2],  a->pos.x);
        write_f32_le(&p[6],  a->pos.y);
        write_f32_le(&p[10], a->vel.x);
        write_f32_le(&p[14], a->vel.y);
        write_f32_le(&p[18], a->hp);
        write_f32_le(&p[22], a->ore);
        write_f32_le(&p[26], a->radius);
        count++;
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    buf[1] = (uint8_t)count;
    return 2 + count * 30;
}

/*
 * WORLD_NPCS message:
 * [type:1][count:1] + count * 23-byte records
 */
static inline int serialize_npcs(uint8_t *buf, const npc_ship_t *npcs) {
    int count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        const npc_ship_t *n = &npcs[i];
        uint8_t *p = &buf[2 + count * 23];
        p[0] = (uint8_t)i;
        p[1] = 1; /* active */
        p[1] |= (((uint8_t)n->role & 0x3) << 1);
        p[1] |= (((uint8_t)n->state & 0x7) << 3);
        if (n->thrusting) p[1] |= (1 << 6);
        write_f32_le(&p[2],  n->pos.x);
        write_f32_le(&p[6],  n->pos.y);
        write_f32_le(&p[10], n->vel.x);
        write_f32_le(&p[14], n->vel.y);
        write_f32_le(&p[18], n->angle);
        p[22] = (uint8_t)(int8_t)n->target_asteroid;
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPCS;
    buf[1] = (uint8_t)count;
    return 2 + count * 23;
}

/*
 * PLAYER_SHIP message (24 bytes):
 * [type:1][id:1][hull:f32][credits:f32][docked:1][station:1]
 * [mining_lvl:1][hold_lvl:1][tractor_lvl:1]
 * [cargo_fe:f32][cargo_cu:f32][cargo_cr:f32]
 */
static inline int serialize_player_ship(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    buf[0] = NET_MSG_PLAYER_SHIP;
    buf[1] = id;
    write_f32_le(&buf[2], sp->ship.hull);
    write_f32_le(&buf[6], sp->ship.credits);
    buf[10] = sp->docked ? 1 : 0;
    buf[11] = (uint8_t)sp->current_station;
    buf[12] = (uint8_t)sp->ship.mining_level;
    buf[13] = (uint8_t)sp->ship.hold_level;
    buf[14] = (uint8_t)sp->ship.tractor_level;
    write_f32_le(&buf[15], sp->ship.cargo[COMMODITY_FERRITE_ORE]);
    write_f32_le(&buf[19], sp->ship.cargo[COMMODITY_CUPRITE_ORE]);
    write_f32_le(&buf[23], sp->ship.cargo[COMMODITY_CRYSTAL_ORE]);
    return 27;
}

/* ------------------------------------------------------------------ */
/* Deserialisation (client -> server)                                 */
/* ------------------------------------------------------------------ */

/*
 * INPUT message (7 bytes):
 * [type:1][flags:1][angle:f32][action:1]
 *
 * Falls back to 6 bytes for legacy clients (no action byte).
 */
static inline void parse_input(const uint8_t *data, int len, input_intent_t *intent) {
    if (len < 6) return;
    uint8_t flags = data[1];
    (void)read_f32_le(&data[2]);

    /* Overwrite continuous inputs every message. */
    intent->thrust = (flags & NET_INPUT_THRUST) ? 1.0f : 0.0f;
    intent->turn = 0.0f;
    if ((flags & NET_INPUT_LEFT) && !(flags & NET_INPUT_RIGHT))
        intent->turn = 1.0f;
    else if ((flags & NET_INPUT_RIGHT) && !(flags & NET_INPUT_LEFT))
        intent->turn = -1.0f;
    intent->mine = (flags & NET_INPUT_FIRE) != 0;

    /* OR-in one-shot actions — they accumulate until the sim consumes them. */
    if (len >= 7) {
        uint8_t action = data[6];
        switch (action) {
        case NET_ACTION_DOCK:
        case NET_ACTION_LAUNCH:
            intent->interact = true;
            break;
        case NET_ACTION_SELL_CARGO:
            intent->service_sell = true;
            break;
        case NET_ACTION_REPAIR:
            intent->service_repair = true;
            break;
        case NET_ACTION_UPGRADE_MINING:
            intent->upgrade_mining = true;
            break;
        case NET_ACTION_UPGRADE_HOLD:
            intent->upgrade_hold = true;
            break;
        case NET_ACTION_UPGRADE_TRACTOR:
            intent->upgrade_tractor = true;
            break;
        default:
            break;
        }
    }
}

#endif /* NET_PROTOCOL_H */
