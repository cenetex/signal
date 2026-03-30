/*
 * main.c -- Headless authoritative game server for Signal Space Miner.
 *
 * Uses cesanta/mongoose for WebSocket handling.  Runs the full game
 * simulation and broadcasts state to browser clients.
 */
#include "mongoose.h"
#include "game_sim.h"
#include "net_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/* Global state                                                       */
/* ------------------------------------------------------------------ */

static world_t world;
static bool running = true;

/* Timing intervals in milliseconds */
#define SIM_TICK_MS   33    /* ~30 Hz poll rate; sim uses SIM_DT accumulator */
#define STATE_TICK_MS 50    /* 20 Hz player state broadcast */
#define WORLD_TICK_MS 100   /* 10 Hz world state broadcast */
#define SHIP_TICK_MS  250   /* 4 Hz full ship state (cargo, hull, etc.) */
#define MAX_SIM_STEPS 8     /* cap sub-steps per poll to prevent spiral */

/* ------------------------------------------------------------------ */
/* Signal handler                                                     */
/* ------------------------------------------------------------------ */

static void signal_handler(int sig) {
    (void)sig;
    running = false;
}

/* ------------------------------------------------------------------ */
/* Player management                                                  */
/* ------------------------------------------------------------------ */

static int alloc_player(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected) return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* WebSocket send helpers                                             */
/* ------------------------------------------------------------------ */

static void ws_send(struct mg_connection *c, const void *data, size_t len) {
    mg_ws_send(c, data, len, WEBSOCKET_OP_BINARY);
}

static void broadcast(const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].connected && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static void broadcast_except(int exclude, const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude) continue;
        if (world.players[i].connected && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

/* ------------------------------------------------------------------ */
/* WS message handler                                                 */
/* ------------------------------------------------------------------ */

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    int pid = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].conn == c) { pid = i; break; }
    }
    if (pid < 0) return;

    const uint8_t *data = (const uint8_t *)wm->data.buf;
    int len = (int)wm->data.len;
    if (len < 1) return;

    switch (data[0]) {
    case NET_MSG_INPUT:
        parse_input(data, len, &world.players[pid].input);
        break;
    case NET_MSG_STATE:
        /* Ignored -- server is authoritative. */
        break;
    case NET_MSG_MINING_ACTION:
        /* Legacy -- mining handled via INPUT flags now. */
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Mongoose event handler                                             */
/* ------------------------------------------------------------------ */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/health"), NULL)) {
            int count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (world.players[i].connected) count++;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"status\":\"ok\",\"players\":%d}", count);
        } else {
            mg_http_reply(c, 404, "", "Not found");
        }
    } else if (ev == MG_EV_WS_OPEN) {
        int pid = alloc_player();
        if (pid < 0) {
            mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
            return;
        }
        server_player_t *sp = &world.players[pid];
        memset(sp, 0, sizeof(*sp));
        sp->connected = true;
        sp->id = (uint8_t)pid;
        sp->conn = c;
        player_init_ship(sp, &world);

        /* Send JOIN to new player (their own ID). */
        uint8_t join_msg[] = { NET_MSG_JOIN, (uint8_t)pid };
        ws_send(c, join_msg, 2);

        /* Notify others and tell new player about existing players. */
        broadcast_except(pid, join_msg, 2);
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pid || !world.players[i].connected) continue;
            uint8_t exist_msg[] = { NET_MSG_JOIN, (uint8_t)i };
            ws_send(c, exist_msg, 2);
        }

        printf("[server] player %d joined\n", pid);
    } else if (ev == MG_EV_WS_MSG) {
        handle_ws_message(c, ev_data);
    } else if (ev == MG_EV_CLOSE) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (world.players[i].conn == c) {
                world.players[i].connected = false;
                world.players[i].conn = NULL;
                uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                broadcast(leave_msg, 2);
                printf("[server] player %d left\n", i);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Broadcast helpers                                                  */
/* ------------------------------------------------------------------ */

static void broadcast_player_states(void) {
    uint8_t buf[22];
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected) continue;
        int len = serialize_player_state(buf, (uint8_t)i, &world.players[i].ship);
        broadcast(buf, (size_t)len);
    }
}

static void broadcast_world(void) {
    /* Asteroids */
    uint8_t abuf[2 + MAX_ASTEROIDS * 30];
    int alen = serialize_asteroids(abuf, world.asteroids);
    broadcast(abuf, (size_t)alen);

    /* NPCs */
    uint8_t nbuf[2 + MAX_NPC_SHIPS * 23];
    int nlen = serialize_npcs(nbuf, world.npc_ships);
    broadcast(nbuf, (size_t)nlen);
}

static void broadcast_ship_states(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected) continue;
        uint8_t buf[32];
        int len = serialize_player_ship(buf, (uint8_t)i, &world.players[i]);
        /* Full ship state sent only to the owning player. */
        ws_send(world.players[i].conn, buf, (size_t)len);
    }

    /* Station state (inventory, ore buffers, products) at same cadence. */
    uint8_t sbuf[2 + MAX_STATIONS * 49];
    int slen = serialize_stations(sbuf, world.stations);
    broadcast(sbuf, (size_t)slen);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char *port = getenv("PORT");
    if (!port) port = "8080";
    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%s", port);

    /* Initialise world. */
    world_reset(&world);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, listen_url, ev_handler, NULL);
    printf("[server] signal game server on %s\n", listen_url);

    uint64_t last_sim = 0, last_state = 0, last_world = 0, last_ship = 0;
    float sim_accum = 0.0f;

    while (running) {
        mg_mgr_poll(&mgr, 1);
        uint64_t now = mg_millis();

        if (now - last_sim >= SIM_TICK_MS) {
            float elapsed = (float)(now - last_sim) / 1000.0f;
            last_sim = now;
            sim_accum += elapsed;
            int steps = 0;
            while (sim_accum >= SIM_DT && steps < MAX_SIM_STEPS) {
                world_sim_step(&world, SIM_DT);
                sim_accum -= SIM_DT;
                steps++;
            }
            if (sim_accum > SIM_DT) sim_accum = 0.0f; /* prevent spiral */
        }
        if (now - last_state >= STATE_TICK_MS) {
            broadcast_player_states();
            last_state = now;
        }
        if (now - last_world >= WORLD_TICK_MS) {
            broadcast_world();
            last_world = now;
        }
        if (now - last_ship >= SHIP_TICK_MS) {
            broadcast_ship_states();
            last_ship = now;
        }
    }

    mg_mgr_free(&mgr);
    printf("[server] shutdown\n");
    return 0;
}
