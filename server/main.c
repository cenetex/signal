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
static const char *allowed_origin = NULL;

/* Shared HTTP response headers for API endpoints */
static char api_headers[256];

/* Dirty flags: only re-broadcast station identity when something changed */
static bool station_identity_dirty[MAX_STATIONS];
static bool station_econ_dirty = true;   /* station inventories changed */
static bool contracts_dirty = true;       /* contract list changed */

#define STATION_IDENTITY_FALLBACK_MS 2000
static uint64_t last_station_identity = 0;

/* Timing intervals in milliseconds */
#define SIM_TICK_MS   33    /* ~30 Hz poll rate; sim uses SIM_DT accumulator */
#define STATE_TICK_MS 50    /* 20 Hz player state broadcast */
#define WORLD_TICK_MS 100   /* 10 Hz world state broadcast */
#define SHIP_TICK_MS  250   /* 4 Hz full ship state (cargo, hull, etc.) */
#define MAX_SIM_STEPS 8     /* cap sub-steps per poll to prevent spiral */
#define SAVE_PATH "world.sav"
#define PLAYER_SAVE_DIR "saves"
#define STATION_CATALOG_DIR "stations"
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
        if (world.players[i].connected && world.players[i].session_ready && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

static void broadcast_except(int exclude, const void *data, size_t len) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == exclude) continue;
        if (world.players[i].connected && world.players[i].session_ready && world.players[i].conn)
            ws_send(world.players[i].conn, data, len);
    }
}

/* ------------------------------------------------------------------ */
/* WS message handler                                                 */
/* ------------------------------------------------------------------ */

/* Per-player WebSocket message rate limiting */
static struct { uint64_t window_start; int msg_count; } ws_rate[MAX_PLAYERS];
#define WS_RATE_WINDOW_MS 1000
#define WS_RATE_LIMIT 60  /* 60 msgs/sec -- generous for 30Hz game input */

static void handle_ws_message(struct mg_connection *c, struct mg_ws_message *wm) {
    int pid = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (world.players[i].conn == c) { pid = i; break; }
    }
    if (pid < 0) return;

    /* Rate limit: silently drop excess messages */
    uint64_t now = mg_millis();
    if (now - ws_rate[pid].window_start > WS_RATE_WINDOW_MS) {
        ws_rate[pid].window_start = now;
        ws_rate[pid].msg_count = 0;
    }
    if (++ws_rate[pid].msg_count > WS_RATE_LIMIT) return;

    const uint8_t *data = (const uint8_t *)wm->data.buf;
    int len = (int)wm->data.len;
    if (len < 1 || pid < 0 || pid >= MAX_PLAYERS) return;

    switch (data[0]) {
    case NET_MSG_INPUT:
        parse_input(data, len, &world.players[pid].input);
        /* If the player just queued a shipyard order, refresh that station's
         * identity on the next world tick so the SHIPYARD tab sees the new
         * pending count immediately instead of waiting for the 2s fallback. */
        if (len >= 3) {
            uint8_t action = data[2];
            if ((action >= NET_ACTION_BUY_SCAFFOLD_TYPED &&
                 action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) ||
                action == NET_ACTION_BUY_SCAFFOLD) {
                int s = world.players[pid].current_station;
                if (s >= 0 && s < MAX_STATIONS) station_identity_dirty[s] = true;
            }
        }
        break;
    case NET_MSG_PLAN:
        parse_plan(data, len, &world.players[pid].input);
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
            /* Extract callsign if present (bytes 9-15) */
            if (len >= 16) {
                memcpy(world.players[pid].callsign, &data[9], 7);
                world.players[pid].callsign[7] = '\0';
                printf("[server] player %d callsign: %s\n", pid, world.players[pid].callsign);
            }
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
                /* Seed starting credits now that session_token is set */
                player_seed_credits(&world.players[pid], &world);
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Station REST API                                                   */
/* ------------------------------------------------------------------ */

static const char *api_token = NULL;

static bool api_auth_ok(struct mg_http_message *hm) {
    if (!api_token || api_token[0] == '\0') return false;
    struct mg_str *auth = mg_http_get_header(hm, "Authorization");
    if (!auth) return false;
    /* Expect "Bearer <token>" */
    if (auth->len < 8) return false;
    const char *prefix = "Bearer ";
    if (strncmp(auth->buf, prefix, 7) != 0) return false;
    return strncmp(auth->buf + 7, api_token, auth->len - 7) == 0
        && strlen(api_token) == auth->len - 7;
}

static int parse_station_id(struct mg_http_message *hm) {
    /* Extract station index from /api/station/<id>/... */
    /* URI looks like /api/station/0/state or /api/station/2/command */
    const char *p = hm->uri.buf + 13; /* skip "/api/station/" */
    const char *end = hm->uri.buf + hm->uri.len;
    if (p >= end) return -1;
    /* Length-safe integer parse: only read digits up to next '/' or URI end */
    int id = 0;
    bool has_digit = false;
    while (p < end && *p >= '0' && *p <= '9') {
        has_digit = true;
        id = id * 10 + (*p - '0');
        if (id >= MAX_STATIONS) return -1;
        p++;
    }
    if (!has_digit) return -1;
    if (id < 0 || id >= MAX_STATIONS) return -1;
    if (!station_exists(&world.stations[id])) return -1;
    return id;
}

/* Append a JSON-escaped string to buf at *pos, respecting bufsz.
 * Escapes quotes, backslashes, and control characters. */
static void json_escape_append(char *buf, int *pos, int bufsz, const char *s) {
    int p = *pos;
    if (p >= bufsz - 1) return;
    for (; *s && p < bufsz - 1; s++) {
        char esc = 0;
        switch (*s) {
            case '"':  esc = '"';  break;
            case '\\': esc = '\\'; break;
            case '\n': esc = 'n';  break;
            case '\r': esc = 'r';  break;
            case '\t': esc = 't';  break;
        }
        if (esc) {
            if (p + 2 > bufsz - 1) break;
            buf[p++] = '\\';
            buf[p++] = esc;
        } else if ((unsigned char)*s < 0x20) {
            /* Other control chars: emit as \u00XX */
            if (p + 6 > bufsz - 1) break;
            p += snprintf(buf + p, (size_t)(bufsz - p), "\\u%04x", (unsigned char)*s);
        } else {
            buf[p++] = *s;
        }
    }
    *pos = p;
}

/* Safe snprintf append: clamp pos so it never exceeds bufsz */
#define BUF_APPEND(pos, buf, bufsz, ...) do { \
    int _n = snprintf((buf) + (pos), (size_t)((bufsz) - (pos)), __VA_ARGS__); \
    if (_n > 0) (pos) += _n; \
    if ((pos) > (bufsz)) (pos) = (bufsz); \
} while (0)

static void handle_station_state(struct mg_connection *c, int sid) {
    const station_t *st = &world.stations[sid];
    /* Build JSON response with signal-range-scoped world view */
    enum { BUFSZ = 32768 };
    char buf[BUFSZ];
    int pos = 0;

    /* Station info */
    BUF_APPEND(pos, buf, BUFSZ,
        "{\"station\":{\"index\":%d,\"name\":\"", sid);
    json_escape_append(buf, &pos, BUFSZ, st->name);
    BUF_APPEND(pos, buf, BUFSZ,
        "\",\"signal_range\":%.1f,\"scaffold\":%s,"
        "\"inventory\":{",
        st->signal_range, st->scaffold ? "true" : "false");

    static const char *cnames[] = {
        "ferrite_ore","cuprite_ore","crystal_ore",
        "ferrite_ingot","cuprite_ingot","crystal_ingot",
        "frame","laser_module","tractor_module"
    };
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        if (i > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "\"%s\":%.1f", cnames[i], st->inventory[i]);
    }
    BUF_APPEND(pos, buf, BUFSZ, "},\"modules\":[");
    for (int m = 0; m < st->module_count; m++) {
        if (m > 0) BUF_APPEND(pos, buf, BUFSZ, ",");
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"type\":\"%s\",\"ring\":%d,\"slot\":%d,\"scaffold\":%s,\"progress\":%.2f}",
            module_type_name(st->modules[m].type),
            st->modules[m].ring, st->modules[m].slot,
            st->modules[m].scaffold ? "true" : "false",
            st->modules[m].build_progress);
    }

    BUF_APPEND(pos, buf, BUFSZ, "]},");

    /* Visible asteroids within signal range */
    float sr_sq = st->signal_range * st->signal_range;
    BUF_APPEND(pos, buf, BUFSZ, "\"visible_asteroids\":[");
    bool first = true;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &world.asteroids[i];
        if (!a->active) continue;
        if (v2_dist_sq(a->pos, st->pos) > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"tier\":%d,\"commodity\":%d,\"x\":%.0f,\"y\":%.0f,\"hp\":%.0f}",
            i, a->tier, a->commodity, a->pos.x, a->pos.y, a->hp);
        if (pos > BUFSZ - 256) break;
    }

    /* Visible players within signal range */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_players\":[");
    first = true;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected || world.players[i].grace_period) continue;
        if (v2_dist_sq(world.players[i].ship.pos, st->pos) > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"id\":%d,\"x\":%.0f,\"y\":%.0f,\"docked\":%s}",
            i, world.players[i].ship.pos.x, world.players[i].ship.pos.y,
            world.players[i].docked ? "true" : "false");
    }

    /* Visible stations within signal range */
    BUF_APPEND(pos, buf, BUFSZ, "],\"visible_stations\":[");
    first = true;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (i == sid || !station_exists(&world.stations[i])) continue;
        float d_sq = v2_dist_sq(world.stations[i].pos, st->pos);
        if (d_sq > sr_sq) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        float overlap = st->signal_range + world.stations[i].signal_range - sqrtf(d_sq);
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"name\":\"", i);
        json_escape_append(buf, &pos, BUFSZ, world.stations[i].name);
        BUF_APPEND(pos, buf, BUFSZ,
            "\",\"x\":%.0f,\"y\":%.0f,\"signal_overlap\":%s}",
            world.stations[i].pos.x, world.stations[i].pos.y,
            overlap > 0.0f ? "true" : "false");
    }

    /* Active contracts */
    BUF_APPEND(pos, buf, BUFSZ, "],\"active_contracts\":[");
    first = true;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        const contract_t *ct = &world.contracts[i];
        if (!ct->active || ct->station_index != sid) continue;
        if (!first) BUF_APPEND(pos, buf, BUFSZ, ",");
        first = false;
        BUF_APPEND(pos, buf, BUFSZ,
            "{\"index\":%d,\"action\":%d,\"commodity\":%d,\"quantity\":%.0f,\"base_price\":%.1f,\"age\":%.0f}",
            i, ct->action, ct->commodity, ct->quantity_needed, ct->base_price, ct->age);
    }

    /* Hail message */
    BUF_APPEND(pos, buf, BUFSZ, "],\"hail\":\"");
    json_escape_append(buf, &pos, BUFSZ, st->hail_message);
    BUF_APPEND(pos, buf, BUFSZ, "\"}");

    mg_http_reply(c, 200, api_headers, "%s", buf);
}

static void handle_station_command(struct mg_connection *c, struct mg_http_message *hm, int sid) {
    station_t *st = &world.stations[sid];
    struct mg_str body = hm->body;
    char *action = mg_json_get_str(body, "$.action");
    long commodity = mg_json_get_long(body, "$.commodity", -1);
    double price_val = 0;
    mg_json_get_num(body, "$.price", &price_val);
    long module_type = mg_json_get_long(body, "$.module_type", -1);
    char *hail = mg_json_get_str(body, "$.hail");

    if (!action) {
        mg_http_reply(c, 400, api_headers,
                      "{\"ok\":false,\"error\":\"missing action\"}");
        free(hail);
        return;
    }

    if (strcmp(action, "set_hail") == 0 && hail && hail[0] != '\0') {
        snprintf(st->hail_message, sizeof(st->hail_message), "%s", hail);
        mg_http_reply(c, 200, api_headers,
                      "{\"ok\":true,\"action\":\"set_hail\"}");
    } else if (strcmp(action, "set_price") == 0 && commodity >= 0 && commodity < COMMODITY_COUNT && price_val > 0) {
        /* Clamp to 0.5x-2.0x of default */
        float default_price = st->base_price[commodity];
        float clamped = (float)price_val;
        if (clamped < default_price * 0.5f) clamped = default_price * 0.5f;
        if (clamped > default_price * 2.0f) clamped = default_price * 2.0f;
        st->base_price[commodity] = clamped;
        mg_http_reply(c, 200, api_headers,
                      "{\"ok\":true,\"action\":\"set_price\",\"commodity\":%ld,\"price\":%.1f}", commodity, clamped);
    } else if (strcmp(action, "build_module") == 0 && module_type >= 0 && module_type < MODULE_COUNT) {
        if (st->module_count >= MAX_MODULES_PER_STATION) {
            mg_http_reply(c, 400, api_headers,
                          "{\"ok\":false,\"error\":\"station full\"}");
        } else {
            begin_module_construction(&world, st, sid, (module_type_t)module_type);
            mg_http_reply(c, 200, api_headers,
                          "{\"ok\":true,\"action\":\"build_module\",\"type\":%ld}", module_type);
        }
    } else {
        mg_http_reply(c, 400, api_headers,
                      "{\"ok\":false,\"error\":\"unknown action\"}");
    }
    free(action);
    free(hail);
}

/* ------------------------------------------------------------------ */
/* Mongoose event handler                                             */
/* ------------------------------------------------------------------ */

/* REST API token-bucket rate limiter: 20 tokens/sec, 40 burst cap */
static uint64_t api_rate_last_refill = 0;
static int api_rate_bucket = 40;
#define API_RATE_REFILL_PER_SEC 20
#define API_RATE_BUCKET_MAX 40

static bool api_rate_check(void) {
    uint64_t now = mg_millis();
    uint64_t elapsed = now - api_rate_last_refill;
    if (elapsed >= 50) {  /* refill every 50ms to smooth out bursts */
        int refill = (int)(elapsed * API_RATE_REFILL_PER_SEC / 1000);
        if (refill > 0) {
            api_rate_bucket += refill;
            if (api_rate_bucket > API_RATE_BUCKET_MAX)
                api_rate_bucket = API_RATE_BUCKET_MAX;
            api_rate_last_refill = now;
        }
    }
    if (api_rate_bucket <= 0) return false;
    api_rate_bucket--;
    return true;
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = ev_data;
        if (mg_match(hm->uri, mg_str("/ws"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/station/*/state"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else
            if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else {
                    handle_station_state(c, sid);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/api/station/*/command"), NULL)) {
            if (!api_rate_check()) {
                mg_http_reply(c, 429, api_headers, "{\"error\":\"rate limit exceeded\"}");
            } else if (!api_auth_ok(hm)) {
                mg_http_reply(c, 401, api_headers, "{\"error\":\"unauthorized\"}");
            } else {
                int sid = parse_station_id(hm);
                if (sid < 0) {
                    mg_http_reply(c, 404, api_headers, "{\"error\":\"station not found\"}");
                } else {
                    handle_station_command(c, hm, sid);
                }
            }
        } else if (mg_match(hm->uri, mg_str("/health"), NULL)) {
            int count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (world.players[i].connected) count++;
#ifdef GIT_HASH
            mg_http_reply(c, 200, api_headers,
                          "{\"status\":\"ok\",\"players\":%d,\"version\":\"%s\"}", count, GIT_HASH);
#else
            mg_http_reply(c, 200, api_headers,
                          "{\"status\":\"ok\",\"players\":%d,\"version\":\"dev\"}", count);
#endif
        } else {
            mg_http_reply(c, 404, "", "Not found");
        }
    } else if (ev == MG_EV_WS_OPEN) {
        /* Per-IP connection limit to mitigate slot exhaustion */
        #define MAX_CONNS_PER_IP 4
        {
            int ip_count = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (world.players[i].connected && world.players[i].conn) {
                    struct mg_connection *pc = (struct mg_connection *)world.players[i].conn;
                    if (memcmp(&pc->rem.addr, &c->rem.addr, sizeof(c->rem.addr)) == 0)
                        ip_count++;
                }
            }
            if (ip_count >= MAX_CONNS_PER_IP) {
                printf("[server] per-IP limit reached for connection, rejecting\n");
                mg_ws_send(c, NULL, 0, WEBSOCKET_OP_CLOSE);
                return;
            }
        }
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
        sp->grace_timer = 5.0f;  /* Must send SESSION within 5 seconds */
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
            if (world.players[i].grace_period) continue; /* skip ghosts */
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

        /* Send full asteroid sync to new player and mark all as sent. */
        {
            uint8_t sync_buf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
            int sync_len = serialize_asteroids_full(sync_buf, world.asteroids);
            ws_send(c, sync_buf, (size_t)sync_len);
            /* Initialize per-player sent tracking */
            server_player_t *sp = &world.players[pid];
            for (int ai = 0; ai < MAX_ASTEROIDS; ai++)
                sp->asteroid_sent[ai] = world.asteroids[ai].active;
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

/* mark_visible_asteroids_dirty removed — per-player relevance filtering
 * in serialize_asteroids_for_player handles viewport culling. */

static void broadcast_world(void) {
    /* Asteroids: per-player relevance filtering.
     * Each player gets only asteroids in their view radius.
     * Deactivation records sent when asteroids leave a player's view. */
    {
        uint8_t abuf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->conn) continue;
            int alen = serialize_asteroids_for_player(
                abuf, world.asteroids, sp->ship.pos, sp->asteroid_sent);
            if (alen > 2) /* skip empty messages */
                ws_send(sp->conn, abuf, (size_t)alen);
        }
        /* Bulk clear dirty flags after all players served */
        for (int i = 0; i < MAX_ASTEROIDS; i++)
            world.asteroids[i].net_dirty = false;
    }

    /* NPCs: per-player view filtering (same radius as asteroids) */
    {
        uint8_t nbuf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->conn) continue;
            int count = 0;
            for (int i = 0; i < MAX_NPC_SHIPS; i++) {
                if (!world.npc_ships[i].active) continue;
                if (v2_dist_sq(world.npc_ships[i].pos, sp->ship.pos) > ASTEROID_VIEW_RADIUS_SQ)
                    continue;
                const npc_ship_t *n = &world.npc_ships[i];
                uint8_t *q = &nbuf[2 + count * NPC_RECORD_SIZE];
                q[0] = (uint8_t)i;
                q[1] = 1;
                q[1] |= (((uint8_t)n->role & 0x3) << 1);
                q[1] |= (((uint8_t)n->state & 0x7) << 3);
                if (n->thrusting) q[1] |= (1 << 6);
                write_f32_le(&q[2],  n->pos.x);
                write_f32_le(&q[6],  n->pos.y);
                write_f32_le(&q[10], n->vel.x);
                write_f32_le(&q[14], n->vel.y);
                write_f32_le(&q[18], n->angle);
                q[22] = (uint8_t)(int8_t)n->target_asteroid;
                q[23] = (uint8_t)(n->tint_r * 255.0f);
                q[24] = (uint8_t)(n->tint_g * 255.0f);
                q[25] = (uint8_t)(n->tint_b * 255.0f);
                count++;
            }
            nbuf[0] = NET_MSG_WORLD_NPCS;
            nbuf[1] = (uint8_t)count;
            if (count > 0)
                ws_send(sp->conn, nbuf, (size_t)(2 + count * NPC_RECORD_SIZE));
        }
    }

    /* Scaffolds: per-player view filtering */
    {
        uint8_t scbuf[2 + MAX_SCAFFOLDS * SCAFFOLD_RECORD_SIZE];
        for (int p = 0; p < MAX_PLAYERS; p++) {
            server_player_t *sp = &world.players[p];
            if (!sp->connected || !sp->conn) continue;
            int count = 0;
            for (int i = 0; i < MAX_SCAFFOLDS; i++) {
                if (!world.scaffolds[i].active) continue;
                if (v2_dist_sq(world.scaffolds[i].pos, sp->ship.pos) > ASTEROID_VIEW_RADIUS_SQ)
                    continue;
                serialize_one_scaffold(&scbuf[2 + count * SCAFFOLD_RECORD_SIZE], i, &world.scaffolds[i]);
                count++;
            }
            scbuf[0] = NET_MSG_WORLD_SCAFFOLDS;
            scbuf[1] = (uint8_t)count;
            if (count > 0)
                ws_send(sp->conn, scbuf, (size_t)(2 + count * SCAFFOLD_RECORD_SIZE));
        }
    }

    /* World time sync (5 bytes: type + float) */
    uint8_t tbuf[5];
    tbuf[0] = NET_MSG_WORLD_TIME;
    write_f32_le(&tbuf[1], world.time);
    broadcast(tbuf, 5);
}

/* Compute station-local balance for a player at their current/nearby station */
static float player_station_balance(const server_player_t *sp) {
    int st = sp->docked ? sp->current_station : sp->nearby_station;
    if (st < 0 || st >= MAX_STATIONS) return 0.0f;
    return ledger_balance(&world.stations[st], sp->session_token);
}

static int send_player_ship(uint8_t *buf, uint8_t id, const server_player_t *sp) {
    return serialize_player_ship_bal(buf, id, sp, player_station_balance(sp));
}

static void broadcast_ship_states(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!world.players[i].connected || !world.players[i].conn) continue;
        uint8_t buf[PLAYER_SHIP_SIZE + 4]; /* +4 headroom */
        int len = send_player_ship(buf, (uint8_t)i, &world.players[i]);
        /* Full ship state sent only to the owning player. */
        ws_send(world.players[i].conn, buf, (size_t)len);
    }

    if (station_econ_dirty) {
        uint8_t sbuf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
        int slen = serialize_stations(sbuf, world.stations);
        broadcast(sbuf, (size_t)slen);
        station_econ_dirty = false;
    }

    if (contracts_dirty) {
        uint8_t cbuf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
        int clen = serialize_contracts(cbuf, world.contracts);
        broadcast(cbuf, (size_t)clen);
        contracts_dirty = false;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char *port = getenv("PORT");
    if (!port) port = "8080";
    api_token = getenv("SIGNAL_API_TOKEN");
    if (api_token && api_token[0] != '\0')
        printf("[server] Station API enabled (token set)\n");
    else {
        fprintf(stderr, "[WARN] SIGNAL_API_TOKEN is unset -- REST API will reject all requests\n");
        if (getenv("SIGNAL_REQUIRE_API_TOKEN")) {
            fprintf(stderr, "[FATAL] SIGNAL_REQUIRE_API_TOKEN set but no token provided\n");
            return 1;
        }
    }
    allowed_origin = getenv("SIGNAL_ALLOWED_ORIGIN");
    if (!allowed_origin) allowed_origin = "*";
    snprintf(api_headers, sizeof(api_headers),
        "Content-Type: application/json\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Cache-Control: no-store\r\n", allowed_origin);
    printf("[server] CORS origin: %s\n", allowed_origin);
    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%s", port);

    /* Ensure persistence directories exist. */
#ifdef _WIN32
    _mkdir(PLAYER_SAVE_DIR);
    _mkdir(STATION_CATALOG_DIR);
#else
    mkdir(PLAYER_SAVE_DIR, 0755);
    mkdir(STATION_CATALOG_DIR, 0755);
#endif

    /* Initialise world — layered persistence (#314):
     * 1. world_reset() seeds starter stations + belt field
     * 2. Catalog overwrites identity for any persisted stations
     * 3. Session snapshot overlays economy state (inventories, NPCs, contracts)
     * 4. Rebuild derived structures */
    world_reset(&world);

    int catalog_count = station_catalog_load_all(world.stations, MAX_STATIONS,
                                                  STATION_CATALOG_DIR);
    if (catalog_count > 0)
        printf("[server] loaded %d station(s) from catalog\n", catalog_count);

    if (world_load(&world, SAVE_PATH)) {
        printf("[server] loaded session from %s\n", SAVE_PATH);
    } else {
        printf("[server] no session save — fresh economy\n");
        /* If catalog exists but session doesn't, seed economy for active stations */
        if (catalog_count > 0) {
            for (int i = 0; i < MAX_STATIONS; i++) {
                if (station_exists(&world.stations[i]) && world.stations[i].credit_pool == 0.0f)
                    world.stations[i].credit_pool = 10000.0f;
            }
        }
    }

    /* Assign stable IDs to any stations loaded from v1 catalogs (id == 0) */
    if (world.next_station_id == 0) world.next_station_id = 1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_exists(&world.stations[i]) && world.stations[i].id == 0)
            world.stations[i].id = world.next_station_id++;
    }

    rebuild_signal_chain(&world);
    station_rebuild_all_nav(&world);
    for (int i = 0; i < MAX_STATIONS; i++) station_identity_dirty[i] = true;

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
    uint64_t last_econ_dirty = 0;
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
                        station_identity_dirty[slot] = true;
                        station_econ_dirty = true;
                        contracts_dirty = true;
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
                            int len = send_player_ship(buf, (uint8_t)pid, &world.players[pid]);
                            ws_send(world.players[pid].conn, buf, (size_t)len);
                            /* Send only the station the player is at, not all stations */
                            int st_idx = world.players[pid].current_station;
                            if (st_idx >= 0 && st_idx < MAX_STATIONS) {
                                uint8_t sbuf[2 + STATION_RECORD_SIZE];
                                sbuf[0] = NET_MSG_WORLD_STATIONS;
                                sbuf[1] = 1;
                                uint8_t *p = &sbuf[2];
                                p[0] = (uint8_t)st_idx;
                                for (int c = 0; c < COMMODITY_COUNT; c++)
                                    write_f32_le(&p[1 + c * 4], world.stations[st_idx].inventory[c]);
                                ws_send(world.players[pid].conn, sbuf, (size_t)(2 + STATION_RECORD_SIZE));
                            }
                        }
                        station_econ_dirty = true;
                        contracts_dirty = true;
                    }
                    if (ev->type == SIM_EVENT_DEATH) {
                        int pid = ev->player_id;
                        if (pid >= 0 && pid < MAX_PLAYERS && world.players[pid].connected && world.players[pid].conn) {
                            /* Death packet now carries position + stats so
                             * the client cinematic anchors at the wreckage
                             * before the server-side respawn moves the
                             * ship. Layout:
                             * [type:1][pid:1][px:f32][py:f32][vx:f32][vy:f32]
                             * [ang:f32][ore:f32][earned:f32][spent:f32]
                             * [asteroids:f32] = 38 bytes */
                            uint8_t msg[38];
                            msg[0] = NET_MSG_DEATH;
                            msg[1] = (uint8_t)pid;
                            write_f32_le(&msg[2],  ev->death.pos_x);
                            write_f32_le(&msg[6],  ev->death.pos_y);
                            write_f32_le(&msg[10], ev->death.vel_x);
                            write_f32_le(&msg[14], ev->death.vel_y);
                            write_f32_le(&msg[18], ev->death.angle);
                            write_f32_le(&msg[22], ev->death.ore_mined);
                            write_f32_le(&msg[26], ev->death.credits_earned);
                            write_f32_le(&msg[30], ev->death.credits_spent);
                            write_f32_le(&msg[34], (float)ev->death.asteroids_fractured);
                            ws_send(world.players[pid].conn, msg, sizeof(msg));
                            /* Also send updated ship state (hull restored, docked) */
                            uint8_t buf[PLAYER_SHIP_SIZE + 4];
                            int len = send_player_ship(buf, (uint8_t)pid, &world.players[pid]);
                            ws_send(world.players[pid].conn, buf, (size_t)len);
                        }
                    }
                    if (ev->type == SIM_EVENT_CONTRACT_COMPLETE) {
                        station_econ_dirty = true;
                        contracts_dirty = true;
                    }
                    if (ev->type == SIM_EVENT_HAIL_RESPONSE) {
                        int pid = ev->player_id;
                        if (pid >= 0 && pid < MAX_PLAYERS && world.players[pid].connected && world.players[pid].conn) {
                            uint8_t msg[6];
                            msg[0] = NET_MSG_HAIL_RESPONSE;
                            msg[1] = (uint8_t)ev->hail_response.station;
                            write_f32_le(&msg[2], ev->hail_response.credits);
                            ws_send(world.players[pid].conn, msg, sizeof(msg));
                            /* Push fresh ship state so the credit bump is visible immediately */
                            uint8_t buf[PLAYER_SHIP_SIZE + 4];
                            int len = send_player_ship(buf, (uint8_t)pid, &world.players[pid]);
                            ws_send(world.players[pid].conn, buf, (size_t)len);
                        }
                    }
                    if (ev->type == SIM_EVENT_OUTPOST_PLACED ||
                        ev->type == SIM_EVENT_OUTPOST_ACTIVATED ||
                        ev->type == SIM_EVENT_MODULE_ACTIVATED ||
                        ev->type == SIM_EVENT_SCAFFOLD_READY) {
                        /* Any structure event needs the station identity refreshed
                         * so the client sees the updated module/pending list. */
                        for (int s = 0; s < MAX_STATIONS; s++)
                            station_identity_dirty[s] = true;
                    }
                }
                /* Broadcast all events to clients for audio + UI */
                if (world.events.count > 0) {
                    uint8_t ebuf[2 + SIM_MAX_EVENTS * NET_EVENT_RECORD_SIZE];
                    int elen = serialize_events(ebuf, &world.events);
                    if (elen > 2) broadcast(ebuf, (size_t)elen);
                }
                sim_accum -= SIM_DT;
                steps++;
            }
            if (sim_accum > SIM_DT) sim_accum = 0.0f; /* prevent spiral */
            /* Mark econ dirty every ~1s as fallback for production changes */
            if (now - last_econ_dirty >= 1000) {
                station_econ_dirty = true;
                contracts_dirty = true;
                last_econ_dirty = now;
            }
        }
        /* Tick down reconnect grace timers and session auth timeouts */
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
            /* Kick clients that never sent SESSION within the auth window */
            if (sp->connected && !sp->session_ready && !sp->grace_period) {
                sp->grace_timer -= (float)SIM_TICK_MS / 1000.0f;
                if (sp->grace_timer <= 0.0f) {
                    printf("[server] player %d: session timeout, disconnecting\n", i);
                    mg_ws_send(sp->conn, NULL, 0, WEBSOCKET_OP_CLOSE);
                    sp->connected = false;
                    sp->conn = NULL;
                    uint8_t leave_msg[] = { NET_MSG_LEAVE, (uint8_t)i };
                    broadcast(leave_msg, 2);
                }
            }
        }
        if (now - last_state >= STATE_TICK_MS) {
            broadcast_player_states();
            last_state = now;
        }
        if (now - last_world >= WORLD_TICK_MS) {
            broadcast_world();
            /* Periodic fallback re-sync for scaffold progress */
            if (now - last_station_identity >= STATION_IDENTITY_FALLBACK_MS) {
                for (int s = 0; s < MAX_STATIONS; s++) station_identity_dirty[s] = true;
                last_station_identity = now;
            }
            /* Re-broadcast dirty station identities only to players in signal range */
            for (int s = 0; s < MAX_STATIONS; s++) {
                if (!station_identity_dirty[s]) continue;
                if (!station_exists(&world.stations[s])) continue;
                uint8_t id_buf[STATION_IDENTITY_SIZE + 4];
                int id_len = serialize_station_identity(id_buf, s, &world.stations[s]);
                float sr_sq = world.stations[s].signal_range * world.stations[s].signal_range;
                for (int p = 0; p < MAX_PLAYERS; p++) {
                    if (!world.players[p].connected || !world.players[p].conn) continue;
                    if (v2_dist_sq(world.players[p].ship.pos, world.stations[s].pos) <= sr_sq)
                        ws_send(world.players[p].conn, id_buf, (size_t)id_len);
                }
                station_identity_dirty[s] = false;
            }
            last_world = now;
        }
        if (now - last_ship >= SHIP_TICK_MS) {
            broadcast_ship_states();
            last_ship = now;
        }
        if (now - last_save >= AUTOSAVE_MS) {
            station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
            world_save(&world, SAVE_PATH);
            last_save = now;
        }
    }

    mg_mgr_free(&mgr);
    station_catalog_save_all(world.stations, MAX_STATIONS, STATION_CATALOG_DIR);
    world_save(&world, SAVE_PATH);
    printf("[server] world saved\n");
    printf("[server] shutdown\n");
    return 0;
}
