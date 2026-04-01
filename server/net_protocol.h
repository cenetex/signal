/*
 * net_protocol.h -- Server-side serialization helpers for the Signal
 * Space Miner authoritative server.
 *
 * Protocol enums, message types, and record sizes live in
 * shared/net_protocol.h (single source of truth).
 */
#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "game_sim.h"
#include "protocol.h"   /* shared/protocol.h — protocol enums & constants */

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

static inline void write_u32_le(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
    buf[2] = (uint8_t)(v >> 16);
    buf[3] = (uint8_t)(v >> 24);
}

static inline uint32_t read_u32_le(const uint8_t *buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
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
static inline int serialize_player_state(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    buf[0] = NET_MSG_STATE;
    buf[1] = id;
    write_f32_le(&buf[2],  sp->ship.pos.x);
    write_f32_le(&buf[6],  sp->ship.pos.y);
    write_f32_le(&buf[10], sp->ship.vel.x);
    write_f32_le(&buf[14], sp->ship.vel.y);
    write_f32_le(&buf[18], sp->ship.angle);
    uint8_t flags = 0;
    if (sp->input.thrust > 0.0f) flags |= 1;
    if (sp->beam_active && sp->beam_hit) flags |= 2;
    if (sp->docked) flags |= 4;
    buf[22] = flags;
    return 23;
}

/*
 * WORLD_PLAYERS message (batched):
 * [type:1][count:1] + count * 22-byte records
 * Each record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1]
 */
/* PLAYER_RECORD_SIZE defined in shared/net_protocol.h */
static inline int serialize_all_player_states(uint8_t *buf, const server_player_t *players) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].connected) continue;
        uint8_t *p = &buf[2 + count * PLAYER_RECORD_SIZE];
        p[0] = (uint8_t)i;
        write_f32_le(&p[1],  players[i].ship.pos.x);
        write_f32_le(&p[5],  players[i].ship.pos.y);
        write_f32_le(&p[9],  players[i].ship.vel.x);
        write_f32_le(&p[13], players[i].ship.vel.y);
        write_f32_le(&p[17], players[i].ship.angle);
        uint8_t flags = 0;
        if (players[i].input.thrust > 0.0f) flags |= 1;
        if (players[i].beam_active && players[i].beam_hit) flags |= 2;
        if (players[i].docked) flags |= 4;
        p[21] = flags;
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYERS;
    buf[1] = (uint8_t)count;
    return 2 + count * PLAYER_RECORD_SIZE;
}

/*
 * WORLD_ASTEROIDS message:
 * [type:1][count:1] + count * ASTEROID_RECORD_SIZE-byte records
 */
/* Serialize only dirty asteroids (delta update). Clears dirty flags after serialization. */
static inline int serialize_asteroids(uint8_t *buf, asteroid_t *asteroids) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!asteroids[i].net_dirty) continue;
        asteroid_t *a = &asteroids[i];
        uint8_t *p = &buf[2 + count * ASTEROID_RECORD_SIZE];
        p[0] = (uint8_t)i;
        p[1] = a->active ? 1 : 0;  /* inactive = deactivation signal */
        if (a->fracture_child) p[1] |= (1 << 1);
        p[1] |= (((uint8_t)a->tier & 0x7) << 2);
        p[1] |= (((uint8_t)a->commodity & 0x7) << 5);
        write_f32_le(&p[2],  a->pos.x);
        write_f32_le(&p[6],  a->pos.y);
        write_f32_le(&p[10], a->vel.x);
        write_f32_le(&p[14], a->vel.y);
        write_f32_le(&p[18], a->hp);
        write_f32_le(&p[22], a->ore);
        write_f32_le(&p[26], a->radius);
        a->net_dirty = false;
        count++;
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    buf[1] = (uint8_t)count;
    return 2 + count * ASTEROID_RECORD_SIZE;
}

/* Serialize ALL asteroids (full sync for new player join). Does not clear dirty flags. */
static inline int serialize_asteroids_full(uint8_t *buf, const asteroid_t *asteroids) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!asteroids[i].active) continue;
        const asteroid_t *a = &asteroids[i];
        uint8_t *p = &buf[2 + count * ASTEROID_RECORD_SIZE];
        p[0] = (uint8_t)i;
        p[1] = 1;
        if (a->fracture_child) p[1] |= (1 << 1);
        p[1] |= (((uint8_t)a->tier & 0x7) << 2);
        p[1] |= (((uint8_t)a->commodity & 0x7) << 5);
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
    return 2 + count * ASTEROID_RECORD_SIZE;
}

/*
 * WORLD_NPCS message:
 * [type:1][count:1] + count * NPC_RECORD_SIZE-byte records
 * (23 original + 3 tint bytes)
 */
static inline int serialize_npcs(uint8_t *buf, const npc_ship_t *npcs) {
    int count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!npcs[i].active) continue;
        const npc_ship_t *n = &npcs[i];
        uint8_t *p = &buf[2 + count * NPC_RECORD_SIZE];
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
        p[23] = (uint8_t)(n->tint_r * 255.0f);
        p[24] = (uint8_t)(n->tint_g * 255.0f);
        p[25] = (uint8_t)(n->tint_b * 255.0f);
        count++;
    }
    buf[0] = NET_MSG_WORLD_NPCS;
    buf[1] = (uint8_t)count;
    return 2 + count * NPC_RECORD_SIZE;
}

/*
 * WORLD_STATIONS message:
 * [type:1][count:1] + count * [index:1][inventory: COMMODITY_COUNT×f32]
 * = 2 + count * STATION_RECORD_SIZE bytes
 */
/* STATION_RECORD_SIZE defined in shared/net_protocol.h */

/* Compile-time guard: if the record layout changes, update STATION_RECORD_SIZE
 * (in shared/net_protocol.h) and all buffers that depend on it. */
/* Compile-time guards: record sizes must match serialization layouts. */
_Static_assert(
    1 + 5 * 4 + 1 == PLAYER_RECORD_SIZE,
    "PLAYER_RECORD_SIZE must match serialized player state layout"
);
_Static_assert(
    1 + 1 + 7 * 4 == ASTEROID_RECORD_SIZE,
    "ASTEROID_RECORD_SIZE must match serialized asteroid layout"
);
_Static_assert(
    2 + 5 * 4 + 1 + 3 == NPC_RECORD_SIZE,
    "NPC_RECORD_SIZE must match serialized NPC layout"
);
_Static_assert(
    1 + COMMODITY_COUNT * 4 == STATION_RECORD_SIZE,
    "STATION_RECORD_SIZE must match serialized station econ layout"
);
_Static_assert(
    16 + COMMODITY_COUNT * 4 == PLAYER_SHIP_SIZE,
    "PLAYER_SHIP_SIZE must match serialized player ship layout"
);

static inline int serialize_stations(uint8_t *buf, const station_t *stations) {
    int count = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &stations[i];
        if (!station_exists(st)) continue;
        uint8_t *p = &buf[2 + count * STATION_RECORD_SIZE];
        p[0] = (uint8_t)i;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            write_f32_le(&p[1 + c * 4], st->inventory[c]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_STATIONS;
    buf[1] = (uint8_t)count;
    return 2 + count * STATION_RECORD_SIZE;
}

/*
 * STATION_IDENTITY message — full static fields for one station.
 * Sent on player join (for all active stations) and when a new outpost is placed.
 * [type:1][index:1][reserved:1][services:4][pos_x:f32][pos_y:f32]
 * [radius:f32][dock_radius:f32][signal_range:f32][name:32]
 * = 59 bytes per message
 */
static inline int serialize_station_identity(uint8_t *buf, int index, const station_t *st) {
    buf[0] = NET_MSG_STATION_IDENTITY;
    buf[1] = (uint8_t)index;
    buf[2] = 0;
    if (st->scaffold) buf[2] |= 1;  /* bit 0: scaffold */
    write_u32_le(&buf[3], st->services);
    write_f32_le(&buf[7], st->pos.x);
    write_f32_le(&buf[11], st->pos.y);
    write_f32_le(&buf[15], st->radius);
    write_f32_le(&buf[19], st->dock_radius);
    write_f32_le(&buf[23], st->signal_range);
    memset(&buf[27], 0, 32);
    memcpy(&buf[27], st->name, strnlen(st->name, 31));
    return STATION_IDENTITY_SIZE;
}

/*
 * PLAYER_SHIP message (40 bytes):
 * [type:1][id:1][hull:f32][credits:f32][docked:1][station:1]
 * [mining_lvl:1][hold_lvl:1][tractor_lvl:1][flags:1]
 * [cargo: COMMODITY_COUNT × f32]
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
    buf[15] = sp->ship.has_scaffold_kit ? 1 : 0;
    for (int c = 0; c < COMMODITY_COUNT; c++)
        write_f32_le(&buf[16 + c * 4], sp->ship.cargo[c]);
    return 16 + COMMODITY_COUNT * 4;
}

/* ------------------------------------------------------------------ */
/* Deserialisation (client -> server)                                 */
/* ------------------------------------------------------------------ */

/*
 * INPUT message (3 bytes, current):
 * [type:1][flags:1][action:1]
 *
 * Legacy (7 bytes): [type:1][flags:1][angle:f32][action:1]
 */
static inline void parse_input(const uint8_t *data, int len, input_intent_t *intent) {
    if (len < 3) return;
    uint8_t flags = data[1];

    /* Overwrite continuous inputs every message. */
    if (flags & NET_INPUT_THRUST)
        intent->thrust = 1.0f;
    else if (flags & NET_INPUT_BRAKE)
        intent->thrust = -1.0f;
    else
        intent->thrust = 0.0f;
    intent->turn = 0.0f;
    if ((flags & NET_INPUT_LEFT) && !(flags & NET_INPUT_RIGHT))
        intent->turn = 1.0f;
    else if ((flags & NET_INPUT_RIGHT) && !(flags & NET_INPUT_LEFT))
        intent->turn = -1.0f;
    intent->mine = (flags & NET_INPUT_FIRE) != 0;

    /* OR-in one-shot actions — they accumulate until the sim consumes them.
     * 3-byte format (current): action at byte 2.
     * 7-byte format (legacy): action at byte 6 (angle at bytes 2-5, ignored).
     * 6-byte format (legacy, no action): no action byte present. */
    {
        uint8_t action = 0;
        if (len == 3) action = data[2];
        else if (len >= 7) action = data[6];
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
        case NET_ACTION_PLACE_OUTPOST:
            intent->place_outpost = true;
            break;
        case NET_ACTION_BUY_SCAFFOLD:
            intent->buy_scaffold_kit = true;
            break;
        default:
            /* NET_ACTION_BUILD_MODULE + module_type (9..9+MODULE_COUNT) */
            if (action >= NET_ACTION_BUILD_MODULE && action < NET_ACTION_BUILD_MODULE + MODULE_COUNT) {
                intent->build_module = true;
                intent->build_module_type = (module_type_t)(action - NET_ACTION_BUILD_MODULE);
            }
            /* NET_ACTION_BUY_PRODUCT + commodity (30..30+COMMODITY_COUNT) */
            else if (action >= NET_ACTION_BUY_PRODUCT && action < NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT) {
                intent->buy_product = true;
                intent->buy_commodity = (commodity_t)(action - NET_ACTION_BUY_PRODUCT);
            }
            break;
        }
    }
}

#endif /* NET_PROTOCOL_H */
