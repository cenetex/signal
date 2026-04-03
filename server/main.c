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
#include <sys/stat.h>

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
#define SAVE_PATH "world.sav"
#define PLAYER_SAVE_DIR "saves"
#define AUTOSAVE_MS 30000   /* autosave every 30 seconds */

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
    if (len < 1 || pid < 0 || pid >= MAX_PLAYERS) return;

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
    case NET_MSG_SESSION:
        if (len >= 9 && !world.players[pid].session_ready) {
            const uint8_t *token = &data[1];
            /* Check for existing grace-period player with same token */
            int reattach = -1;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (i == pid) continue;
                if (world.players[i].connected && world.players[i].grace_period &&
                    world.players[i].session_ready &&
                    memcmp(world.players[i].session_token, token, 8) == 0) {
                    reattach = i;
                    break;
                }
            }
            if (reattach >= 0) {
                /* Reattach: copy state from grace slot to new slot */
                server_player_t *old = &world.players[reattach];
                server_player_t *sp = &world.players[pid];
                sp->ship = old->ship;
                sp->current_station = old->current_station;
                sp->nearby_station = old->nearby_station;
                sp->docked = old->docked;
                sp->in_dock_range = old->in_dock_range;
                memcpy(sp->session_token, token, 8);
                sp->session_ready = true;
                /* Clear the grace slot and broadcast LEAVE so clients drop the ghost */
                old->connected = false;
                old->grace_period = false;
                old->conn = NULL;
                uint8_t leave_old[] = { NET_MSG_LEAVE, (uint8_t)reattach };
                broadcast(leave_old, 2);
                printf("[server] player %d: reconnected (was slot %d)\n", pid, reattach);
            } else {
                memcpy(world.players[pid].session_token, token, 8);
                world.players[pid].session_ready = true;
                /* Try to restore saved state keyed by session token */
                if (player_load_by_token(&world.players[pid], &world,
                                         PLAYER_SAVE_DIR, token)) {
                    printf("[server] player %d: restored save by session\n", pid);
                } else {
                    printf("[server] player %d: no save for session, fresh ship\n", pid);
                }
            }
        }
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
#ifdef GIT_HASH
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"status\":\"ok\",\"players\":%d,\"version\":\"%s\"}", count, GIT_HASH);
#else
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"status\":\"ok\",\"players\":%d,\"version\":\"dev\"}", count);
#endif
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
        sp->session_ready = false;
        /* Start with fresh ship — save is loaded when client sends SESSION */
        player_init_ship(sp, &world);
        printf("[server] player %d: awaiting session token\n", pid);

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

        /* Send station identity for all active stations. */
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&world.stations[s])) continue;
            uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
            int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
            ws_send(c, id_buf, (size_t)id_len);
        }

        /* Send full asteroid sync to new player. */
        {
            uint8_t sync_buf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
            int sync_len = serialize_asteroids_full(sync_buf, world.asteroids);
            ws_send(c, sync_buf, (size_t)sync_len);
        }

        /* Send server version hash. */
        {
#ifdef GIT_HASH
            const char *hash = GIT_HASH;
#else
            const char *hash = "dev";
#endif
            size_t hlen = strlen(hash);
            uint8_t info_msg[12] = { NET_MSG_SERVER_INFO };
            if (hlen > 11) hlen = 11;
            memcpy(&info_msg[1], hash, hlen);
            ws_send(c, info_msg, 1 + hlen);
        }

        printf("[server] player %d joined\n", pid);
    } else if (ev == MG_EV_WS_MSG) {
        handle_ws_message(c, ev_data);
    } else if (ev == MG_EV_CLOSE) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (world.players[i].conn == c) {
                player_save(&world.players[i], PLAYER_SAVE_DIR, i);
                world.players[i].conn = NULL;
                if (world.players[i].session_ready) {
                    /* Keep slot alive for reconnect grace window */
                    world.players[i].grace_period = true;
                    world.players[i].grace_timer = 30.0f;
                    printf("[server] player %d disconnected, grace window 30s\n", i);
                } else {
                    /* No session — immediate full disconnect */
                    world.players[i].connected = false;
                    uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                    broadcast(leave_msg, 2);
                    printf("[server] player %d left (no session)\n", i);
                }
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Broadcast helpers                                                  */
/* ------------------------------------------------------------------ */

static void broadcast_player_states(void) {
    /* Batch all connected player states into one message, send once per client.
     * This is O(N) sends instead of O(N^2). */
    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int len = serialize_all_player_states(buf, world.players);
    broadcast(buf, (size_t)len);
}

static void mark_visible_asteroids_dirty(void) {
    /* Mark asteroids near any connected player as dirty so they get sent.
     * View radius ~1200u covers a generous screen at default zoom. */
    const float VIEW_RADIUS_SQ = 3000.0f * 3000.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!world.asteroids[i].active || world.asteroids[i].net_dirty) continue;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!world.players[p].connected) continue;
            if (v2_dist_sq(world.asteroids[i].pos, world.players[p].ship.pos) < VIEW_RADIUS_SQ) {
                world.asteroids[i].net_dirty = true;
                break;
            }
        }
    }
}

static void broadcast_world(void) {
    /* Mark asteroids in player views as dirty before serializing */
    mark_visible_asteroids_dirty();

    /* Asteroids (delta: only dirty) */
    uint8_t abuf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    int alen = serialize_asteroids(abuf, world.asteroids);
    broadcast(abuf, (size_t)alen);

    /* NPCs */
    uint8_t nbuf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    int nlen = serialize_npcs(nbuf, world.npc_ships);
    broadcast(nbuf, (size_t)nlen);
}

static void broadcast_ship_states(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected || !world.players[i].conn) continue;
        uint8_t buf[PLAYER_SHIP_SIZE + 4]; /* +4 headroom */
        int len = serialize_player_ship(buf, (uint8_t)i, &world.players[i]);
        /* Full ship state sent only to the owning player. */
        ws_send(world.players[i].conn, buf, (size_t)len);
    }

    /* Station state */
    uint8_t sbuf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int slen = serialize_stations(sbuf, world.stations);
    broadcast(sbuf, (size_t)slen);

    /* Contracts */
    uint8_t cbuf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int clen = serialize_contracts(cbuf, world.contracts);
    broadcast(cbuf, (size_t)clen);
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

    /* Ensure saves directory exists. */
#ifdef _WIN32
    _mkdir(PLAYER_SAVE_DIR);
#else
    mkdir(PLAYER_SAVE_DIR, 0755);
#endif

    /* Initialise world. */
    world_reset(&world);
    if (world_load(&world, SAVE_PATH))
        printf("[server] loaded world from %s\n", SAVE_PATH);
    else
        printf("[server] fresh world\n");

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, listen_url, ev_handler, NULL);
#ifdef GIT_HASH
    printf("[server] SIGNAL alpha %s on %s\n", GIT_HASH, listen_url);
#else
    printf("[server] SIGNAL alpha on %s\n", listen_url);
#endif
    printf("[server] ALPHA BUILD — world may reset without notice\n");

    uint64_t last_sim = 0, last_state = 0, last_world = 0, last_ship = 0, last_save = 0;
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
                /* Immediate broadcasts triggered by sim events. */
                for (int e = 0; e < world.events.count; e++) {
                    sim_event_t *ev = &world.events.events[e];
                    if (ev->type == SIM_EVENT_OUTPOST_PLACED) {
                        int slot = ev->outpost_placed.slot;
                        uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
                        int id_len = serialize_station_identity(id_buf, slot, &world.stations[slot]);
                        broadcast(id_buf, (size_t)id_len);
                    }
                    /* Send immediate ship + station state after actions that
                     * change cargo, credits, hull, or dock status — eliminates
                     * the 250ms stale window from SHIP_TICK_MS cadence. */
                    if (ev->type == SIM_EVENT_SELL || ev->type == SIM_EVENT_REPAIR ||
                        ev->type == SIM_EVENT_UPGRADE || ev->type == SIM_EVENT_DOCK ||
                        ev->type == SIM_EVENT_LAUNCH) {
                        int pid = ev->player_id;
                        if (pid >= 0 && pid < MAX_PLAYERS && world.players[pid].connected && world.players[pid].conn) {
                            uint8_t buf[PLAYER_SHIP_SIZE + 4];
                            int len = serialize_player_ship(buf, (uint8_t)pid, &world.players[pid]);
                            ws_send(world.players[pid].conn, buf, (size_t)len);
                            /* Also send updated station inventory so market UI refreshes */
                            uint8_t sbuf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
                            int slen = serialize_stations(sbuf, world.stations);
                            ws_send(world.players[pid].conn, sbuf, (size_t)slen);
                        }
                    }
                }
                sim_accum -= SIM_DT;
                steps++;
            }
            if (sim_accum > SIM_DT) sim_accum = 0.0f; /* prevent spiral */
        }
        /* Tick down reconnect grace timers */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            server_player_t *sp = &world.players[i];
            if (sp->connected && sp->grace_period) {
                sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
                if (sp->grace_timer <= 0.0f) {
                    sp->connected = false;
                    sp->grace_period = false;
                    uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                    broadcast(leave_msg, 2);
                    printf("[server] player %d grace expired, fully disconnected\n", i);
                }
            }
        }
        if (now - last_state >= STATE_TICK_MS) {
            broadcast_player_states();
            last_state = now;
        }
        if (now - last_world >= WORLD_TICK_MS) {
            broadcast_world();
            /* Re-broadcast station identities so scaffold status stays synced */
            for (int s = 0; s < MAX_STATIONS; s++) {
                if (!station_exists(&world.stations[s])) continue;
                uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
                int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
                broadcast(id_buf, (size_t)id_len);
            }
            last_world = now;
        }
        if (now - last_ship >= SHIP_TICK_MS) {
            broadcast_ship_states();
            last_ship = now;
        }
        if (now - last_save >= AUTOSAVE_MS) {
            world_save(&world, SAVE_PATH);
            last_save = now;
        }
    }

    mg_mgr_free(&mgr);
    world_save(&world, SAVE_PATH);
    printf("[server] world saved\n");
    printf("[server] shutdown\n");
    return 0;
}
