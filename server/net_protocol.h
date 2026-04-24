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
#include "sim_nav.h"
#include "protocol.h"   /* shared/protocol.h — protocol enums & constants */

/* Forward declaration — defined below. */
static inline int serialize_signal_channel(uint8_t *buf, const signal_channel_t *ch);

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

static inline void write_u16_le(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v);
    buf[1] = (uint8_t)(v >> 8);
}

static inline uint16_t read_u16_le(const uint8_t *buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
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
 * STATE message (45 bytes):
 * [type:1][id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1][tractor_lvl:1][towed_count:1][towed_frags:20]
 * towed_frags = 10 × uint16_t (little-endian), 0xFFFF = unused. Widened
 * from uint8_t so slots 255-2047 aren't silently dropped (#285 Phase 3).
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
    if (sp->actual_thrusting) flags |= 1;
    if (sp->beam_active) flags |= 2;
    if (sp->docked) flags |= 4;
    if (sp->scan_active) flags |= 8;
    if (sp->ship.tractor_active) flags |= 16;
    if (sp->beam_ineffective) flags |= 32;
    if (sp->beam_hit) flags |= 64;
    buf[22] = flags;
    buf[23] = (uint8_t)sp->ship.tractor_level;
    buf[24] = sp->ship.towed_count;
    for (int t = 0; t < 10; t++) {
        int16_t fi = (t < sp->ship.towed_count) ? sp->ship.towed_fragments[t] : -1;
        uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        buf[25 + t * 2]     = (uint8_t)(wire & 0xFFu);
        buf[25 + t * 2 + 1] = (uint8_t)(wire >> 8);
    }
    return 45;
}

/*
 * WORLD_PLAYERS message (batched):
 * [type:1][count:1] + count * PLAYER_RECORD_SIZE records
 * Each record: [id:1][x:f32][y:f32][vx:f32][vy:f32][angle:f32][flags:1]
 *              [tractor_lvl:1][towed_count:1][towed_frags:20][callsign:7]
 *              [beam_start_x:f32][beam_start_y:f32][beam_end_x:f32][beam_end_y:f32]
 */
/* PLAYER_RECORD_SIZE defined in shared/net_protocol.h */
static inline int serialize_all_player_states(uint8_t *buf, const server_player_t *players) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!players[i].connected || players[i].grace_period) continue;
        uint8_t *p = &buf[2 + count * PLAYER_RECORD_SIZE];
        p[0] = (uint8_t)i;
        write_f32_le(&p[1],  players[i].ship.pos.x);
        write_f32_le(&p[5],  players[i].ship.pos.y);
        write_f32_le(&p[9],  players[i].ship.vel.x);
        write_f32_le(&p[13], players[i].ship.vel.y);
        write_f32_le(&p[17], players[i].ship.angle);
        uint8_t flags = 0;
        if (players[i].actual_thrusting) flags |= 1;
        /* bit 1 was "beam_active && beam_hit" — now just "beam_active" so
         * the client can render the beam even when it's firing into empty
         * space (no rock target). beam_hit is implied by the beam_end
         * coords matching a target. */
        if (players[i].beam_active) flags |= 2;
        if (players[i].docked) flags |= 4;
        if (players[i].scan_active) flags |= 8;
        if (players[i].ship.tractor_active) flags |= 16;
        if (players[i].beam_ineffective) flags |= 32;
        if (players[i].beam_hit) flags |= 64;
        p[21] = flags;
        p[22] = (uint8_t)players[i].ship.tractor_level;
        p[23] = players[i].ship.towed_count;
        for (int t = 0; t < 10; t++) {
            int16_t fi = (t < players[i].ship.towed_count) ? players[i].ship.towed_fragments[t] : -1;
            uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
            p[24 + t * 2]     = (uint8_t)(wire & 0xFFu);
            p[24 + t * 2 + 1] = (uint8_t)(wire >> 8);
        }
        /* Callsign: 7 bytes (e.g. "KRX-472") */
        memcpy(&p[44], players[i].callsign, 7);
        /* Beam coordinates — server-authoritative. The client mirrors
         * these into LOCAL_PLAYER.beam_start/end so the autopilot's
         * laser visual works (and so combat hits are deterministic
         * once we have weapons). */
        write_f32_le(&p[51], players[i].beam_start.x);
        write_f32_le(&p[55], players[i].beam_start.y);
        write_f32_le(&p[59], players[i].beam_end.x);
        write_f32_le(&p[63], players[i].beam_end.y);
        count++;
    }
    buf[0] = NET_MSG_WORLD_PLAYERS;
    buf[1] = (uint8_t)count;
    return 2 + count * PLAYER_RECORD_SIZE;
}

/*
 * WORLD_ASTEROIDS message (v2 — uint16 indices):
 * [type:1][count:2] + count * ASTEROID_RECORD_SIZE-byte records
 * Record: [index:2][flags:1][pos:2xf32][vel:2xf32][hp:f32][ore:f32][radius:f32] = 31 bytes
 */
#define ASTEROID_MSG_HEADER 3  /* type + uint16 count */

/* Per-player asteroid serialization with relevance filtering. */
#define ASTEROID_VIEW_RADIUS_SQ (3000.0f * 3000.0f)
static inline int serialize_asteroids_for_player(
    uint8_t *buf, const asteroid_t *asteroids, vec2 player_pos, bool *sent) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &asteroids[i];
        bool in_view = a->active &&
            v2_dist_sq(a->pos, player_pos) < ASTEROID_VIEW_RADIUS_SQ;

        if (in_view) {
            uint8_t *p = &buf[ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
            write_u16_le(&p[0], (uint16_t)i);
            p[2] = 1;
            if (a->fracture_child) p[2] |= (1 << 1);
            p[2] |= (((uint8_t)a->tier & 0x7) << 2);
            p[2] |= (((uint8_t)a->commodity & 0x7) << 5);
            write_f32_le(&p[3],  a->pos.x);
            write_f32_le(&p[7],  a->pos.y);
            write_f32_le(&p[11], a->vel.x);
            write_f32_le(&p[15], a->vel.y);
            write_f32_le(&p[19], a->hp);
            write_f32_le(&p[23], a->ore);
            write_f32_le(&p[27], a->radius);
            /* smelt_progress: 0.0-1.0 quantized to uint8 so the client
             * furnace-glow + laser-beam visuals fire in multiplayer. */
            {
                float sp_f = a->smelt_progress;
                if (sp_f < 0.0f) sp_f = 0.0f;
                if (sp_f > 1.0f) sp_f = 1.0f;
                p[31] = (uint8_t)(sp_f * 255.0f);
            }
            p[32] = a->grade;
            sent[i] = true;
            count++;
        } else if (sent[i] && !in_view) {
            uint8_t *p = &buf[ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
            memset(p, 0, ASTEROID_RECORD_SIZE);
            write_u16_le(&p[0], (uint16_t)i);
            p[2] = 0; /* active = false */
            sent[i] = false;
            count++;
        }
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    write_u16_le(&buf[1], (uint16_t)count);
    return ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE;
}

/* Serialize every asteroid slot (full snapshot for new player join).
 * Inactive slots must be included so clients can clear any locally
 * predicted or stale asteroid state they seeded before connecting.
 * Does not clear dirty flags. */
static inline int serialize_asteroids_full(uint8_t *buf, const asteroid_t *asteroids) {
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &asteroids[i];
        if (!a->active) continue; /* skip inactive for full snapshot — too many slots at 2048 */
        uint8_t *p = &buf[ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE];
        memset(p, 0, ASTEROID_RECORD_SIZE);
        write_u16_le(&p[0], (uint16_t)i);
        p[2] = 1;
        if (a->fracture_child) p[2] |= (1 << 1);
        p[2] |= (((uint8_t)a->tier & 0x7) << 2);
        p[2] |= (((uint8_t)a->commodity & 0x7) << 5);
        write_f32_le(&p[3],  a->pos.x);
        write_f32_le(&p[7],  a->pos.y);
        write_f32_le(&p[11], a->vel.x);
        write_f32_le(&p[15], a->vel.y);
        write_f32_le(&p[19], a->hp);
        write_f32_le(&p[23], a->ore);
        write_f32_le(&p[27], a->radius);
        {
            float sp_f = a->smelt_progress;
            if (sp_f < 0.0f) sp_f = 0.0f;
            if (sp_f > 1.0f) sp_f = 1.0f;
            p[31] = (uint8_t)(sp_f * 255.0f);
        }
        p[32] = a->grade;
        count++;
    }
    buf[0] = NET_MSG_WORLD_ASTEROIDS;
    write_u16_le(&buf[1], (uint16_t)count);
    return ASTEROID_MSG_HEADER + count * ASTEROID_RECORD_SIZE;
}

/* RATi v2 — write a single named ingot record into the buffer.
 * Layout matches the on-wire NAMED_INGOT_RECORD_SIZE definition. */
static inline void write_named_ingot(uint8_t *p, const named_ingot_t *m) {
    memset(p, 0, NAMED_INGOT_RECORD_SIZE);
    memcpy(&p[0], m->pubkey, 32);
    p[32] = m->prefix_class;
    p[33] = m->metal;
    /* p[34..35] pad */
    for (int k = 0; k < 8; k++) p[36 + k] = (uint8_t)(m->mined_block >> (8 * k));
    p[44] = m->origin_station;
    /* p[45..51] pad */
}

/* Per-station named-ingot stockpile snapshot. Sent on dock + on
 * stockpile change. */
static inline int serialize_station_ingots(uint8_t *buf, int station_idx,
                                           const station_t *st) {
    int n = st->named_ingots_count;
    if (n < 0) n = 0;
    if (n > STATION_NAMED_INGOTS_MAX) n = STATION_NAMED_INGOTS_MAX;
    if (n > 255) n = 255; /* count fits in u8 */
    buf[0] = NET_MSG_STATION_INGOTS;
    buf[1] = (uint8_t)station_idx;
    buf[2] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *p = &buf[STATION_INGOTS_HEADER + i * NAMED_INGOT_RECORD_SIZE];
        write_named_ingot(p, &st->named_ingots[i]);
    }
    return STATION_INGOTS_HEADER + n * NAMED_INGOT_RECORD_SIZE;
}

/* Per-station manifest summary (Phase 2). Runs through every unit in
 * station.manifest, builds a per-(commodity, grade) count table, and
 * serializes only the non-zero slots.
 * Wire layout is defined in shared/protocol.h (STATION_MANIFEST_HEADER /
 * STATION_MANIFEST_ENTRY) so the client can decode. Sent per-station on
 * manifest change (cheapest) or on dock. */
static inline int serialize_station_manifest(uint8_t *buf, int station_idx,
                                             const station_t *st) {
    /* Build the (commodity × grade) count table from the manifest. */
    uint16_t table[COMMODITY_COUNT][MINING_GRADE_COUNT];
    memset(table, 0, sizeof(table));
    if (st->manifest.units) {
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            const cargo_unit_t *u = &st->manifest.units[i];
            if (u->commodity >= COMMODITY_COUNT) continue;
            if (u->grade >= MINING_GRADE_COUNT) continue;
            if (table[u->commodity][u->grade] < 0xFFFF)
                table[u->commodity][u->grade]++;
        }
    }
    buf[0] = NET_MSG_STATION_MANIFEST;
    buf[1] = (uint8_t)station_idx;
    int n = 0;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        for (int g = 0; g < MINING_GRADE_COUNT; g++) {
            uint16_t count = table[c][g];
            if (count == 0) continue;
            uint8_t *p = &buf[STATION_MANIFEST_HEADER + n * STATION_MANIFEST_ENTRY];
            p[0] = (uint8_t)c;
            p[1] = (uint8_t)g;
            write_u16_le(&p[2], count);
            n++;
        }
    }
    write_u16_le(&buf[2], (uint16_t)n);
    return STATION_MANIFEST_HEADER + n * STATION_MANIFEST_ENTRY;
}

/* Local player's hold-ingot snapshot. Sent on contents change. */
static inline int serialize_hold_ingots(uint8_t *buf, const ship_t *ship) {
    int n = ship->hold_ingots_count;
    if (n < 0) n = 0;
    if (n > SHIP_HOLD_INGOTS_MAX) n = SHIP_HOLD_INGOTS_MAX;
    buf[0] = NET_MSG_HOLD_INGOTS;
    buf[1] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        uint8_t *p = &buf[HOLD_INGOTS_HEADER + i * NAMED_INGOT_RECORD_SIZE];
        write_named_ingot(p, &ship->hold_ingots[i]);
    }
    return HOLD_INGOTS_HEADER + n * NAMED_INGOT_RECORD_SIZE;
}

/* Signal channel (#316) snapshot — the client dedupes by id so this
 * works as both the connect-time full sync and the per-post update. */
static inline int serialize_signal_channel(uint8_t *buf, const signal_channel_t *ch) {
    int n = ch->count;
    if (n > SIGNAL_CHANNEL_CAPACITY) n = SIGNAL_CHANNEL_CAPACITY;
    buf[0] = NET_MSG_SIGNAL_CHANNEL;
    write_u16_le(&buf[1], (uint16_t)n);
    int start = (ch->head - n + SIGNAL_CHANNEL_CAPACITY) % SIGNAL_CHANNEL_CAPACITY;
    for (int i = 0; i < n; i++) {
        int slot = (start + i) % SIGNAL_CHANNEL_CAPACITY;
        const signal_channel_msg_t *m = &ch->msgs[slot];
        uint8_t *p = &buf[3 + i * SIGNAL_CHANNEL_RECORD_SIZE];
        memset(p, 0, SIGNAL_CHANNEL_RECORD_SIZE);
        for (int k = 0; k < 8; k++) p[k] = (uint8_t)(m->id >> (8 * k));
        for (int k = 0; k < 4; k++) p[8 + k] = (uint8_t)(m->timestamp_ms >> (8 * k));
        p[12] = (uint8_t)(m->sender_station & 0xFF);
        p[13] = m->text_len;
        memcpy(&p[14], m->text, m->text_len);
        memcpy(&p[14 + 200], m->entry_hash, 32);
    }
    return 3 + n * SIGNAL_CHANNEL_RECORD_SIZE;
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
    1 + 5 * 4 + 1 + 1 + 1 + 20 + 7 + 4 * 4 == PLAYER_RECORD_SIZE,
    "PLAYER_RECORD_SIZE must match serialized player state layout"
);
_Static_assert(
    2 + 1 + 7 * 4 + 1 + 1 == ASTEROID_RECORD_SIZE,  /* uint16 index + flags + 7 floats + smelt:u8 + grade:u8 */
    "ASTEROID_RECORD_SIZE must match serialized asteroid layout"
);
_Static_assert(
    2 + 5 * 4 + 1 + 3 == NPC_RECORD_SIZE,
    "NPC_RECORD_SIZE must match serialized NPC layout"
);
_Static_assert(
    1 + COMMODITY_COUNT * 4 + 4 == STATION_RECORD_SIZE,
    "STATION_RECORD_SIZE must match serialized station econ layout"
);
/* PLAYER_SHIP_SIZE is now a maximum (variable length due to path waypoints).
 * The fixed header is 16 + COMMODITY_COUNT*4 + 14 = 66 bytes, plus up to
 * 2 + 12*8 = 98 bytes of path data. */

static inline int serialize_stations(uint8_t *buf, const station_t *stations) {
    int count = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &stations[i];
        if (!station_exists(st)) continue;
        uint8_t *p = &buf[2 + count * STATION_RECORD_SIZE];
        p[0] = (uint8_t)i;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            write_f32_le(&p[1 + c * 4], st->inventory[c]);
        write_f32_le(&p[1 + COMMODITY_COUNT * 4], st->credit_pool);
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
    if (st->planned)  buf[2] |= 2;  /* bit 1: planned */
    write_u32_le(&buf[3], st->services);
    write_f32_le(&buf[7], st->pos.x);
    write_f32_le(&buf[11], st->pos.y);
    write_f32_le(&buf[15], st->radius);
    write_f32_le(&buf[19], st->dock_radius);
    write_f32_le(&buf[23], st->signal_range);
    memset(&buf[27], 0, 32);
    { size_t n = strlen(st->name); if (n > 31) n = 31; memcpy(&buf[27], st->name, n); }
    for (int c = 0; c < COMMODITY_COUNT; c++)
        write_f32_le(&buf[59 + c * 4], st->base_price[c]);
    write_f32_le(&buf[59 + COMMODITY_COUNT * 4], st->scaffold_progress);
    int moff = 59 + COMMODITY_COUNT * 4 + 4;  /* after scaffold_progress */
    buf[moff] = (uint8_t)st->module_count;
    moff++;
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        buf[moff]     = (m < st->module_count) ? (uint8_t)st->modules[m].type : 0;
        buf[moff + 1] = (m < st->module_count && st->modules[m].scaffold) ? 1 : 0;
        buf[moff + 2] = (m < st->module_count) ? st->modules[m].ring : 0;
        buf[moff + 3] = (m < st->module_count) ? st->modules[m].slot : 0;
        write_f32_le(&buf[moff + 4], (m < st->module_count) ? st->modules[m].build_progress : 0.0f);
        moff += STATION_MODULE_RECORD_SIZE;
    }
    /* Ring rotation speeds + offsets */
    buf[moff] = (uint8_t)st->arm_count;
    moff++;
    for (int a = 0; a < MAX_ARMS; a++) {
        write_f32_le(&buf[moff], st->arm_speed[a]);
        moff += 4;
    }
    for (int a = 0; a < MAX_ARMS; a++) {
        write_f32_le(&buf[moff], st->ring_offset[a]);
        moff += 4;
    }
    /* Placement plans (faction-shared blueprint slots) */
    int plan_n = st->placement_plan_count;
    if (plan_n > STATION_PLAN_RECORD_COUNT) plan_n = STATION_PLAN_RECORD_COUNT;
    buf[moff] = (uint8_t)plan_n;
    moff++;
    for (int p = 0; p < STATION_PLAN_RECORD_COUNT; p++) {
        if (p < plan_n) {
            buf[moff + 0] = (uint8_t)st->placement_plans[p].type;
            buf[moff + 1] = st->placement_plans[p].ring;
            buf[moff + 2] = st->placement_plans[p].slot;
            buf[moff + 3] = (uint8_t)st->placement_plans[p].owner;
        } else {
            buf[moff + 0] = 0;
            buf[moff + 1] = 0;
            buf[moff + 2] = 0;
            buf[moff + 3] = 0xFF;
        }
        moff += STATION_PLAN_RECORD_SIZE;
    }
    /* Pending shipyard orders (head-of-queue first). Count + fixed-size
     * trailer so the message size stays a compile-time constant. */
    int pend_n = st->pending_scaffold_count;
    if (pend_n > STATION_PENDING_SCAFFOLD_RECORD_COUNT) pend_n = STATION_PENDING_SCAFFOLD_RECORD_COUNT;
    buf[moff] = (uint8_t)pend_n;
    moff++;
    for (int p = 0; p < STATION_PENDING_SCAFFOLD_RECORD_COUNT; p++) {
        if (p < pend_n) {
            buf[moff + 0] = (uint8_t)st->pending_scaffolds[p].type;
            buf[moff + 1] = (uint8_t)st->pending_scaffolds[p].owner;
        } else {
            buf[moff + 0] = 0;
            buf[moff + 1] = 0xFF;
        }
        moff += STATION_PENDING_SCAFFOLD_RECORD_SIZE;
    }
    /* Currency name trailer — 32 bytes, zero-padded. Empty → client
     * shows "credits". AI-editable via /api/station/<id>/command. */
    memset(&buf[moff], 0, STATION_IDENTITY_CURRENCY_NAME_LEN);
    {
        size_t n = strlen(st->currency_name);
        if (n > STATION_IDENTITY_CURRENCY_NAME_LEN - 1) n = STATION_IDENTITY_CURRENCY_NAME_LEN - 1;
        memcpy(&buf[moff], st->currency_name, n);
    }
    moff += STATION_IDENTITY_CURRENCY_NAME_LEN;
    return STATION_IDENTITY_SIZE;
}

/*
 * WORLD_SCAFFOLDS message: active scaffold pool (NASCENT/LOOSE/TOWING/SNAPPING/PLACED).
 * [type:1][count:1] + count * SCAFFOLD_RECORD_SIZE
 * Per record: [id:1][state:1][module_type:1][owner:1][pos:2xf32][vel:2xf32][radius:f32][build_amount:f32]
 * = 28 bytes
 */
static inline void serialize_one_scaffold(uint8_t *p, int index, const scaffold_t *sc) {
    p[0] = (uint8_t)index;
    p[1] = (uint8_t)sc->state;
    p[2] = (uint8_t)sc->module_type;
    p[3] = (sc->owner < 0) ? 0xFFu : (uint8_t)sc->owner;
    write_f32_le(&p[4],  sc->pos.x);
    write_f32_le(&p[8],  sc->pos.y);
    write_f32_le(&p[12], sc->vel.x);
    write_f32_le(&p[16], sc->vel.y);
    write_f32_le(&p[20], sc->radius);
    write_f32_le(&p[24], sc->build_amount);
}

static inline int serialize_scaffolds(uint8_t *buf, const scaffold_t *scaffolds) {
    int count = 0;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!scaffolds[i].active) continue;
        serialize_one_scaffold(&buf[2 + count * SCAFFOLD_RECORD_SIZE], i, &scaffolds[i]);
        count++;
    }
    buf[0] = NET_MSG_WORLD_SCAFFOLDS;
    buf[1] = (uint8_t)count;
    return 2 + count * SCAFFOLD_RECORD_SIZE;
}

/*
 * PLAYER_SHIP message:
 * [type:1][id:1][hull:f32][station_balance:f32][docked:1][station:1]
 * [mining_lvl:1][hold_lvl:1][tractor_lvl:1][flags:1]
 * [cargo: COMMODITY_COUNT × f32]
 * [nearby_frags:1][tractor_frags:1][towed_count:1][towed_frags:20]
 * towed_frags = 10 × uint16_t (little-endian), 0xFFFF = unused.
 */
static inline int serialize_player_ship_bal(uint8_t *buf, uint8_t id, const server_player_t *sp, float station_balance) {
    buf[0] = NET_MSG_PLAYER_SHIP;
    buf[1] = id;
    write_f32_le(&buf[2], sp->ship.hull);
    write_f32_le(&buf[6], station_balance);
    buf[10] = sp->docked ? 1 : 0;
    buf[11] = (uint8_t)sp->current_station;
    buf[12] = (uint8_t)sp->ship.mining_level;
    buf[13] = (uint8_t)sp->ship.hold_level;
    buf[14] = (uint8_t)sp->ship.tractor_level;
    buf[15] = sp->autopilot_mode; /* repurposed reserved byte */
    for (int c = 0; c < COMMODITY_COUNT; c++)
        write_f32_le(&buf[16 + c * 4], sp->ship.cargo[c]);
    int off = 16 + COMMODITY_COUNT * 4;
    buf[off++] = (uint8_t)(sp->nearby_fragments < 255 ? sp->nearby_fragments : 255);
    buf[off++] = (uint8_t)(sp->tractor_fragments < 255 ? sp->tractor_fragments : 255);
    buf[off++] = sp->ship.towed_count;
    for (int t = 0; t < 10; t++) {
        int16_t fi = (t < sp->ship.towed_count) ? sp->ship.towed_fragments[t] : -1;
        uint16_t wire = (fi >= 0 && fi < MAX_ASTEROIDS) ? (uint16_t)fi : 0xFFFFu;
        buf[off++] = (uint8_t)(wire & 0xFFu);
        buf[off++] = (uint8_t)(wire >> 8);
    }
    /* Autopilot target asteroid index (0xFF = none). */
    buf[off++] = (sp->autopilot_target >= 0 && sp->autopilot_target < 255)
        ? (uint8_t)sp->autopilot_target : 0xFF;
    /* Autopilot A* path waypoints — the actual path the server is following.
     * [count:1][wp0_x:f32][wp0_y:f32][wp1_x:f32]... */
    {
        nav_path_t *path = nav_player_path(sp->id);
        int pc = (sp->autopilot_mode && path->count > 0) ? path->count : 0;
        if (pc > 12) pc = 12;
        buf[off++] = (uint8_t)pc;
        buf[off++] = (pc > 0) ? (uint8_t)path->current : 0;
        for (int i = 0; i < pc; i++) {
            write_f32_le(&buf[off], path->waypoints[i].x); off += 4;
            write_f32_le(&buf[off], path->waypoints[i].y); off += 4;
        }
    }
    return off;
}

/*
 * CONTRACTS message:
 * [type:1][count:1] + count * [action:1][station:1][commodity:1][quantity:f32][base_price:f32][age:f32][target_x:f32][target_y:f32][target_index:i32]
 * = 2 + count * 25 bytes
 */
/* Wire layout per contract record (28 bytes total):
 *   action(1) + station(1) + commodity(1) + required_grade(1)
 *   + quantity_needed(f32) + base_price(f32) + age(f32)
 *   + target.x(f32) + target.y(f32) + target_index(u32)
 * Bumped from 27 when required_grade was added. */
#define CONTRACT_RECORD_SIZE 28

static inline int serialize_contracts(uint8_t *buf, const contract_t *contracts) {
    int count = 0;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!contracts[i].active) continue;
        uint8_t *p = &buf[2 + count * CONTRACT_RECORD_SIZE];
        p[0] = (uint8_t)contracts[i].action;
        p[1] = contracts[i].station_index;
        p[2] = (uint8_t)contracts[i].commodity;
        p[3] = contracts[i].required_grade;
        write_f32_le(&p[4],  contracts[i].quantity_needed);
        write_f32_le(&p[8],  contracts[i].base_price);
        write_f32_le(&p[12], contracts[i].age);
        write_f32_le(&p[16], contracts[i].target_pos.x);
        write_f32_le(&p[20], contracts[i].target_pos.y);
        write_u32_le(&p[24], (uint32_t)contracts[i].target_index);
        /* Note: claimed_by not sent — server-only field */
        count++;
    }
    buf[0] = NET_MSG_CONTRACTS;
    buf[1] = (uint8_t)count;
    return 2 + count * CONTRACT_RECORD_SIZE;
}

/* ------------------------------------------------------------------ */
/* Deserialisation (client -> server)                                 */
/* ------------------------------------------------------------------ */

/*
 * INPUT message (4 or 5 bytes):
 * [type:1][flags:1][action:1][mining_target:1][buy_grade:1 (optional)]
 * Older clients send 4 bytes — buy_grade is treated as MINING_GRADE_COUNT
 * ("any grade, FIFO"). Only meaningful when action is in the
 * NET_ACTION_BUY_PRODUCT range.
 */
static inline void parse_input(const uint8_t *data, int len, input_intent_t *intent) {
    if (len < 4) return;
    intent->mining_target_hint = -1;
    /* Default buy_grade each message; the BUY branch below overrides if
     * the 5th byte is a valid grade index. */
    intent->buy_grade = MINING_GRADE_COUNT;
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
    intent->tractor_hold = (flags & NET_INPUT_TRACTOR) != 0;
    intent->boost = (flags & NET_INPUT_BOOST) != 0;

    /* One-shot actions — accumulate until the sim consumes them. */
    {
        uint8_t action = data[2];
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
        case NET_ACTION_PLACE_MODULE:
            /* Legacy: no-op (module placement now via towed scaffold + reticle) */
            break;
        case NET_ACTION_HAIL:
            intent->hail = true;
            break;
        case NET_ACTION_RELEASE_TOW:
            intent->release_tow = true;
            break;
        case NET_ACTION_RESET:
            intent->reset = true;
            break;
        case NET_ACTION_AUTOPILOT_TOGGLE:
            intent->toggle_autopilot = true;
            break;
        default:
            /* NET_ACTION_BUILD_MODULE legacy: no-op (range collapsed) */
            /* NET_ACTION_BUY_PRODUCT + commodity (30..30+COMMODITY_COUNT) */
            if (action >= NET_ACTION_BUY_PRODUCT && action < NET_ACTION_BUY_PRODUCT + COMMODITY_COUNT) {
                intent->buy_product = true;
                intent->buy_commodity = (commodity_t)(action - NET_ACTION_BUY_PRODUCT);
                if (len >= 5) {
                    uint8_t g = data[4];
                    /* Valid grades are 0..MINING_GRADE_COUNT-1; pass the
                     * sentinel MINING_GRADE_COUNT through so "any" reads
                     * cleanly in manifest_transfer_by_commodity_ex. */
                    if (g <= MINING_GRADE_COUNT) intent->buy_grade = (mining_grade_t)g;
                }
            }
            /* NET_ACTION_BUY_SCAFFOLD_TYPED + module_type (50..50+MODULE_COUNT) */
            else if (action >= NET_ACTION_BUY_SCAFFOLD_TYPED && action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) {
                intent->buy_scaffold_kit = true;
                intent->scaffold_kit_module = (module_type_t)(action - NET_ACTION_BUY_SCAFFOLD_TYPED);
            }
            /* NET_ACTION_DELIVER_COMMODITY + commodity (70..70+COMMODITY_COUNT)
             * — selective fulfillment from the contracts tab. Sets
             * service_sell with a one-shot filter so try_sell_station_cargo
             * only consumes the chosen commodity. */
            else if (action >= NET_ACTION_DELIVER_COMMODITY && action < NET_ACTION_DELIVER_COMMODITY + COMMODITY_COUNT) {
                intent->service_sell = true;
                intent->service_sell_only = (commodity_t)(action - NET_ACTION_DELIVER_COMMODITY);
            }
            break;
        }
    }

    /* Mining target hint (uint8 — always valid with MAX_ASTEROIDS=2048) */
    {
        uint8_t target = data[3];
        intent->mining_target_hint = (target == 0xFF) ? -1 : (int)target;
    }
}

/*
 * PLAN message (NET_PLAN_MSG_SIZE bytes):
 * [type:1][op:1][station:1][ring:1][slot:1][module_type:1][px:f32][py:f32]
 *
 * Accumulates onto the player's pending intent so the next sim step can
 * apply it. Plan ops can stack across messages without colliding with the
 * one-shot action byte in NET_MSG_INPUT.
 */
static inline void parse_plan(const uint8_t *data, int len, input_intent_t *intent) {
    if (len < NET_PLAN_MSG_SIZE) return;
    uint8_t op = data[1];
    int8_t station = (int8_t)data[2];
    int8_t ring = (int8_t)data[3];
    int8_t slot = (int8_t)data[4];
    uint8_t mtype = data[5];
    float px, py;
    memcpy(&px, &data[6], 4);
    memcpy(&py, &data[10], 4);

    switch (op) {
    case NET_PLAN_OP_CREATE_OUTPOST:
        intent->create_planned_outpost = true;
        intent->planned_outpost_pos.x = px;
        intent->planned_outpost_pos.y = py;
        break;
    case NET_PLAN_OP_ADD_SLOT:
        if ((int)mtype < MODULE_COUNT) {
            intent->add_plan = true;
            intent->plan_station = station;
            intent->plan_ring = ring;
            intent->plan_slot = slot;
            intent->plan_type = (module_type_t)mtype;
        }
        break;
    case NET_PLAN_OP_CANCEL_OUTPOST:
        intent->cancel_planned_outpost = true;
        intent->cancel_planned_station = station;
        break;
    case NET_PLAN_OP_CREATE_AND_ADD:
        /* Atomic create + first plan: born at (px,py), plan at ring/slot/type.
         * plan_station=-2 is a sentinel resolved at processing time. */
        intent->create_planned_outpost = true;
        intent->planned_outpost_pos.x = px;
        intent->planned_outpost_pos.y = py;
        if ((int)mtype < MODULE_COUNT) {
            intent->add_plan = true;
            intent->plan_station = -2; /* sentinel: just-created station */
            intent->plan_ring = ring;
            intent->plan_slot = slot;
            intent->plan_type = (module_type_t)mtype;
        }
        break;
    case NET_PLAN_OP_CANCEL_PLAN_SLOT:
        intent->cancel_plan_slot = true;
        intent->cancel_plan_st = station;
        intent->cancel_plan_ring = ring;
        intent->cancel_plan_sl = slot;
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Event broadcast serialization                                       */
/* ------------------------------------------------------------------ */

/* Serialize all events from the current sim step into a NET_MSG_EVENTS
 * packet. Skips DEATH (has its own message) and HAIL_RESPONSE (ditto).
 * Returns total packet length. */
static inline int serialize_events(uint8_t *buf, const sim_events_t *events) {
    int count = 0;
    for (int i = 0; i < events->count; i++) {
        const sim_event_t *ev = &events->events[i];
        /* Skip types that already have dedicated messages */
        if (ev->type == SIM_EVENT_DEATH) continue;
        if (ev->type == SIM_EVENT_HAIL_RESPONSE) continue;

        uint8_t *p = &buf[2 + count * NET_EVENT_RECORD_SIZE];
        memset(p, 0, NET_EVENT_RECORD_SIZE);
        p[0] = (uint8_t)ev->type;
        p[1] = (uint8_t)ev->player_id;

        switch (ev->type) {
        case SIM_EVENT_FRACTURE:
            p[2] = (uint8_t)ev->fracture.tier;
            break;
        case SIM_EVENT_PICKUP:
            write_f32_le(&p[2], ev->pickup.ore);
            p[6] = (uint8_t)ev->pickup.fragments;
            break;
        case SIM_EVENT_UPGRADE:
            p[2] = (uint8_t)ev->upgrade.upgrade;
            break;
        case SIM_EVENT_DAMAGE:
            write_f32_le(&p[2], ev->damage.amount);
            break;
        case SIM_EVENT_OUTPOST_PLACED:
            p[2] = (uint8_t)ev->outpost_placed.slot;
            break;
        case SIM_EVENT_OUTPOST_ACTIVATED:
            p[2] = (uint8_t)ev->outpost_activated.slot;
            break;
        case SIM_EVENT_MODULE_ACTIVATED:
            p[2] = (uint8_t)ev->module_activated.station;
            p[3] = (uint8_t)ev->module_activated.module_idx;
            p[4] = (uint8_t)ev->module_activated.module_type;
            break;
        case SIM_EVENT_NPC_SPAWNED:
            p[2] = (uint8_t)ev->npc_spawned.slot;
            p[3] = (uint8_t)ev->npc_spawned.role;
            p[4] = (uint8_t)ev->npc_spawned.home_station;
            break;
        case SIM_EVENT_STATION_CONNECTED:
            p[2] = (uint8_t)ev->station_connected.connected_count;
            break;
        case SIM_EVENT_CONTRACT_COMPLETE:
            p[2] = (uint8_t)ev->contract_complete.action;
            break;
        case SIM_EVENT_SCAFFOLD_READY:
            p[2] = (uint8_t)ev->scaffold_ready.station;
            p[3] = (uint8_t)ev->scaffold_ready.module_type;
            break;
        case SIM_EVENT_SELL:
            p[2] = (uint8_t)ev->sell.station;
            p[3] = (uint8_t)ev->sell.grade;
            write_u32_le(&p[4], (uint32_t)ev->sell.base_cr);
            write_u32_le(&p[8], (uint32_t)ev->sell.bonus_cr);
            p[12] = ev->sell.by_contract;
            break;
        default:
            /* MINING_TICK, DOCK, LAUNCH, REPAIR, SIGNAL_LOST,
             * ORDER_REJECTED: type + player_id is sufficient */
            break;
        }
        count++;
    }
    buf[0] = NET_MSG_EVENTS;
    buf[1] = (uint8_t)count;
    return 2 + count * NET_EVENT_RECORD_SIZE;
}

#endif /* NET_PROTOCOL_H */
