/*
 * net.c — Multiplayer networking implementation for Signal Space Miner.
 *
 * WASM build: Uses emscripten WebSocket API.
 * Native build: Uses mongoose WebSocket client.
 */
#include "net.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ---------- Shared state ------------------------------------------------- */

static struct {
    bool connected;
    uint8_t local_id;
    NetPlayerState players[NET_MAX_PLAYERS];
    NetCallbacks callbacks;
    char server_hash[12];
    uint8_t session_token[8];
    bool session_token_ready;
} net_state;

/* ---------- Protocol helpers (shared between WASM and native) ------------ */

static void write_f32_le(uint8_t* buf, float v) {
    union { float f; uint32_t u; } conv;
    conv.f = v;
    buf[0] = (uint8_t)(conv.u);
    buf[1] = (uint8_t)(conv.u >> 8);
    buf[2] = (uint8_t)(conv.u >> 16);
    buf[3] = (uint8_t)(conv.u >> 24);
}

static uint32_t read_u32_le(const uint8_t* buf) {
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

static float read_f32_le(const uint8_t* buf) {
    union { float f; uint32_t u; } conv;
    conv.u = (uint32_t)buf[0]
           | ((uint32_t)buf[1] << 8)
           | ((uint32_t)buf[2] << 16)
           | ((uint32_t)buf[3] << 24);
    return conv.f;
}

/* Forward declaration — implemented per platform below. */
static void ws_send_binary(const uint8_t* data, int len);

#ifdef __EMSCRIPTEN__
static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0;
}
#endif

static void ensure_session_token(void) {
    if (net_state.session_token_ready) return;
#ifdef __EMSCRIPTEN__
    /* Try to load from localStorage, generate if missing.
     * Returns 16-char hex string or generates + stores a new one. */
    const char *hex = emscripten_run_script_string(
        "(function(){"
        "var k='signal_session_token',s=localStorage.getItem(k);"
        "if(s&&s.length===16)return s;"
        "var a=new Uint8Array(8);crypto.getRandomValues(a);"
        "var h='';for(var i=0;i<8;i++)h+=('0'+a[i].toString(16)).slice(-2);"
        "localStorage.setItem(k,h);return h;"
        "})()"
    );
    if (hex && strlen(hex) == 16) {
        for (int i = 0; i < 8; i++)
            net_state.session_token[i] = (hex_nibble(hex[i*2]) << 4) | hex_nibble(hex[i*2+1]);
    }
#else
    /* Native: generate random token */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(net_state.session_token, 1, 8, f);
        fclose(f);
    } else {
        /* Fallback: time-based seed */
        uint32_t seed = (uint32_t)time(NULL);
        for (int i = 0; i < 8; i++) {
            seed = seed * 1103515245u + 12345u;
            net_state.session_token[i] = (uint8_t)(seed >> 16);
        }
    }
#endif
    net_state.session_token_ready = true;
}

static void send_session_token(void) {
    uint8_t buf[9];
    buf[0] = NET_MSG_SESSION;
    memcpy(&buf[1], net_state.session_token, 8);
    ws_send_binary(buf, 9);
    printf("[net] sent session token\n");
}

static void handle_message(const uint8_t* data, int len) {
    if (len < 1) return;

    switch (data[0]) {
    case NET_MSG_JOIN:
        if (len < 2) break;
        {
            uint8_t id = data[1];
            if (net_state.local_id == 0xFF) {
                net_state.local_id = id;
                printf("[net] assigned player id %d\n", id);
            } else if (id != net_state.local_id) {
                if (id < NET_MAX_PLAYERS) {
                    net_state.players[id].player_id = id;
                    net_state.players[id].active = true;
                }
                if (net_state.callbacks.on_join) {
                    net_state.callbacks.on_join(id);
                }
                printf("[net] player %d joined\n", id);
            }
        }
        break;

    case NET_MSG_LEAVE:
        if (len < 2) break;
        {
            uint8_t id = data[1];
            if (id < NET_MAX_PLAYERS) {
                net_state.players[id].active = false;
            }
            if (net_state.callbacks.on_leave) {
                net_state.callbacks.on_leave(id);
            }
            printf("[net] player %d left\n", id);
        }
        break;

    case NET_MSG_STATE:
        if (len < 22) break;
        {
            uint8_t id = data[1];
            if (id >= NET_MAX_PLAYERS) break;

            NetPlayerState* ps = &net_state.players[id];
            ps->player_id = id;
            ps->x     = read_f32_le(&data[2]);
            ps->y     = read_f32_le(&data[6]);
            ps->vx    = read_f32_le(&data[10]);
            ps->vy    = read_f32_le(&data[14]);
            ps->angle = read_f32_le(&data[18]);
            ps->flags = (len >= 23) ? data[22] : 0;
            ps->tractor_level = (len >= 24) ? data[23] : 0;
            ps->towed_count = (len >= 25) ? data[24] : 0;
            if (len >= 35) {
                for (int t = 0; t < 10; t++) ps->towed_fragments[t] = data[25 + t];
            } else {
                memset(ps->towed_fragments, 0xFF, 10);
            }
            ps->active = true;

            if (net_state.callbacks.on_state) {
                net_state.callbacks.on_state(ps);
            }
        }
        break;

    case NET_MSG_WORLD_PLAYERS:
        if (len < 2) break;
        {
            int count = (int)data[1];
            int expected = 2 + count * PLAYER_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_players_begin)
                net_state.callbacks.on_players_begin();
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[2 + i * PLAYER_RECORD_SIZE];
                uint8_t id = p[0];
                if (id >= NET_MAX_PLAYERS) continue;
                NetPlayerState* ps = &net_state.players[id];
                ps->player_id = id;
                ps->x     = read_f32_le(&p[1]);
                ps->y     = read_f32_le(&p[5]);
                ps->vx    = read_f32_le(&p[9]);
                ps->vy    = read_f32_le(&p[13]);
                ps->angle = read_f32_le(&p[17]);
                ps->flags = p[21];
                ps->tractor_level = p[22];
                ps->towed_count = p[23];
                for (int t = 0; t < 10; t++) ps->towed_fragments[t] = p[24 + t];
                ps->active = true;
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROIDS:
        if (len < 2) break;
        {
            int count = (int)data[1];
            int expected = 2 + count * ASTEROID_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroids) {
                NetAsteroidState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[2 + i * ASTEROID_RECORD_SIZE];
                    arr[i].index  = p[0];
                    arr[i].flags  = p[1];
                    arr[i].x      = read_f32_le(&p[2]);
                    arr[i].y      = read_f32_le(&p[6]);
                    arr[i].vx     = read_f32_le(&p[10]);
                    arr[i].vy     = read_f32_le(&p[14]);
                    arr[i].hp     = read_f32_le(&p[18]);
                    arr[i].ore    = read_f32_le(&p[22]);
                    arr[i].radius = read_f32_le(&p[26]);
                }
                net_state.callbacks.on_asteroids(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_NPCS:
        if (len < 2) break;
        {
            int count = (int)data[1];
            int expected = 2 + count * NPC_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_npcs) {
                NetNpcState arr[MAX_NPC_SHIPS];
                int decoded = (count > MAX_NPC_SHIPS) ? MAX_NPC_SHIPS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[2 + i * NPC_RECORD_SIZE];
                    arr[i].index            = p[0];
                    arr[i].flags            = p[1];
                    arr[i].x                = read_f32_le(&p[2]);
                    arr[i].y                = read_f32_le(&p[6]);
                    arr[i].vx               = read_f32_le(&p[10]);
                    arr[i].vy               = read_f32_le(&p[14]);
                    arr[i].angle            = read_f32_le(&p[18]);
                    arr[i].target_asteroid  = (int8_t)p[22];
                    arr[i].tint_r           = p[23];
                    arr[i].tint_g           = p[24];
                    arr[i].tint_b           = p[25];
                }
                net_state.callbacks.on_npcs(arr, decoded);
            }
        }
        break;

    case NET_MSG_WORLD_STATIONS:
        if (len < 2) break;
        {
            uint8_t count = data[1];
            if (len < 2 + count * STATION_RECORD_SIZE) break;
            if (net_state.callbacks.on_stations) {
                for (int i = 0; i < count; i++) {
                    const uint8_t *p = &data[2 + i * STATION_RECORD_SIZE];
                    uint8_t idx = p[0];
                    float inv[COMMODITY_COUNT];
                    for (int j = 0; j < COMMODITY_COUNT; j++)
                        inv[j] = read_f32_le(&p[1 + j * 4]);
                    net_state.callbacks.on_stations(idx, inv);
                }
            }
        }
        break;

    case NET_MSG_PLAYER_SHIP:
        if (len < 16 + COMMODITY_COUNT * 4) break;
        {
            uint8_t id = data[1];
            if (id != net_state.local_id) break;
            if (net_state.callbacks.on_player_ship) {
                NetPlayerShipState pss = {0};
                pss.player_id       = id;
                pss.hull            = read_f32_le(&data[2]);
                pss.credits         = read_f32_le(&data[6]);
                pss.docked          = data[10] != 0;
                pss.current_station = data[11];
                pss.mining_level    = data[12];
                pss.hold_level      = data[13];
                pss.tractor_level   = data[14];
                pss.has_scaffold_kit = data[15] != 0;
                for (int c = 0; c < COMMODITY_COUNT; c++)
                    pss.cargo[c] = read_f32_le(&data[16 + c * 4]);
                int off = 16 + COMMODITY_COUNT * 4;
                if (len >= off + 13) {
                    pss.nearby_fragments = data[off];
                    pss.tractor_fragments = data[off + 1];
                    pss.towed_count = data[off + 2];
                    for (int t = 0; t < 10; t++)
                        pss.towed_fragments[t] = data[off + 3 + t];
                } else {
                    memset(pss.towed_fragments, 0xFF, 10);
                }
                net_state.callbacks.on_player_ship(&pss);
            }
        }
        break;

    case NET_MSG_STATION_IDENTITY:
        if (len >= STATION_IDENTITY_SIZE && net_state.callbacks.on_station_identity) {
            NetStationIdentity si = {0};
            si.index = data[1];
            si.flags = data[2];
            si.services = read_u32_le(&data[3]);
            si.pos_x = read_f32_le(&data[7]);
            si.pos_y = read_f32_le(&data[11]);
            si.radius = read_f32_le(&data[15]);
            si.dock_radius = read_f32_le(&data[19]);
            si.signal_range = read_f32_le(&data[23]);
            memcpy(si.name, &data[27], 31);
            si.name[31] = '\0';
            for (int c = 0; c < COMMODITY_COUNT; c++)
                si.base_price[c] = read_f32_le(&data[59 + c * 4]);
            si.scaffold_progress = read_f32_le(&data[59 + COMMODITY_COUNT * 4]);
            int moff = 59 + COMMODITY_COUNT * 4 + 4;
            si.module_count = data[moff];
            if (si.module_count > MAX_MODULES_PER_STATION)
                si.module_count = MAX_MODULES_PER_STATION;
            moff++;
            for (int m = 0; m < si.module_count; m++) {
                si.modules[m].type = (module_type_t)data[moff];
                si.modules[m].scaffold = data[moff + 1] != 0;
                si.modules[m].ring = data[moff + 2];
                si.modules[m].slot = data[moff + 3];
                si.modules[m].build_progress = read_f32_le(&data[moff + 4]);
                moff += STATION_MODULE_RECORD_SIZE;
            }
            /* Skip over unused module record slots to reach arm data */
            moff = 59 + COMMODITY_COUNT * 4 + 4 + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE;
            si.arm_count = data[moff];
            if (si.arm_count > MAX_ARMS) si.arm_count = MAX_ARMS;
            moff++;
            for (int a = 0; a < MAX_ARMS; a++)
                si.arm_speed[a] = read_f32_le(&data[moff + a * 4]);
            moff += MAX_ARMS * 4;
            for (int a = 0; a < MAX_ARMS; a++)
                si.ring_offset[a] = read_f32_le(&data[moff + a * 4]);
            net_state.callbacks.on_station_identity(&si);
        }
        break;

    case NET_MSG_SERVER_INFO:
        if (len >= 2) {
            int hash_len = len - 1;
            if (hash_len > 11) hash_len = 11;
            memcpy(net_state.server_hash, &data[1], (size_t)hash_len);
            net_state.server_hash[hash_len] = '\0';
            printf("[net] server version: %s\n", net_state.server_hash);
        }
        break;

    case NET_MSG_DEATH:
        if (len >= 2 && net_state.callbacks.on_death) {
            net_state.callbacks.on_death(data[1]);
        }
        break;

    case NET_MSG_WORLD_TIME:
        if (len >= 5 && net_state.callbacks.on_world_time) {
            float server_time = read_f32_le(&data[1]);
            net_state.callbacks.on_world_time(server_time);
        }
        break;

    case NET_MSG_CONTRACTS:
        if (len >= 2 && net_state.callbacks.on_contracts) {
            uint8_t count = data[1];
            if (len >= 2 + count * 25) {
                contract_t contracts[MAX_CONTRACTS];
                memset(contracts, 0, sizeof(contracts));
                int n = count < MAX_CONTRACTS ? count : MAX_CONTRACTS;
                for (int i = 0; i < n; i++) {
                    const uint8_t *p = &data[2 + i * 25];
                    contracts[i].active = true;
                    contracts[i].action = (p[0] <= CONTRACT_SCAN) ? (contract_action_t)p[0] : CONTRACT_SUPPLY;
                    contracts[i].station_index = (p[1] < MAX_STATIONS) ? p[1] : 0;
                    contracts[i].commodity = (p[2] < COMMODITY_COUNT) ? (commodity_t)p[2] : COMMODITY_FERRITE_ORE;
                    contracts[i].quantity_needed = read_f32_le(&p[3]);
                    contracts[i].base_price = read_f32_le(&p[7]);
                    contracts[i].age = read_f32_le(&p[11]);
                    contracts[i].target_pos.x = read_f32_le(&p[15]);
                    contracts[i].target_pos.y = read_f32_le(&p[19]);
                    contracts[i].target_index = (int)(int32_t)read_u32_le(&p[23]);
                    contracts[i].claimed_by = -1;
                }
                net_state.callbacks.on_contracts(contracts, n);
            }
        }
        break;

    default:
        break;
    }
}

/* ========================================================================= */
/* Platform-specific implementations                                        */
/* ========================================================================= */

#ifdef __EMSCRIPTEN__

/* ========================================================================= */
/* WASM implementation using emscripten WebSocket API                        */
/* ========================================================================= */

#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

static EMSCRIPTEN_WEBSOCKET_T ws_socket = 0;

static EM_BOOL on_ws_open(int eventType, const EmscriptenWebSocketOpenEvent* event, void* userData) {
    (void)eventType; (void)event; (void)userData;
    net_state.connected = true;
    printf("[net] connected to relay server\n");
    /* Send session token immediately so server can match grace slots */
    ensure_session_token();
    send_session_token();
    return EM_TRUE;
}

static EM_BOOL on_ws_message(int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData) {
    (void)eventType; (void)userData;
    if (event->isText) return EM_TRUE;
    handle_message((const uint8_t*)event->data, (int)event->numBytes);
    return EM_TRUE;
}

static EM_BOOL on_ws_error(int eventType, const EmscriptenWebSocketErrorEvent* event, void* userData) {
    (void)eventType; (void)event; (void)userData;
    printf("[net] websocket error\n");
    net_state.connected = false;
    return EM_TRUE;
}

static EM_BOOL on_ws_close(int eventType, const EmscriptenWebSocketCloseEvent* event, void* userData) {
    (void)eventType; (void)event; (void)userData;
    printf("[net] disconnected from relay server\n");
    net_state.connected = false;
    ws_socket = 0;
    return EM_TRUE;
}

bool net_init(const char* url, const NetCallbacks* callbacks) {
    memset(&net_state, 0, sizeof(net_state));
    net_state.local_id = 0xFF;
    if (callbacks) net_state.callbacks = *callbacks;

    if (!url || url[0] == '\0') {
        printf("[net] no server URL provided, multiplayer disabled\n");
        return false;
    }
    if (!emscripten_websocket_is_supported()) {
        printf("[net] WebSocket not supported in this browser\n");
        return false;
    }

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url;
    attr.protocols = NULL;
    attr.createOnMainThread = EM_TRUE;

    ws_socket = emscripten_websocket_new(&attr);
    if (ws_socket <= 0) {
        printf("[net] failed to create WebSocket\n");
        return false;
    }

    emscripten_websocket_set_onopen_callback(ws_socket, NULL, on_ws_open);
    emscripten_websocket_set_onmessage_callback(ws_socket, NULL, on_ws_message);
    emscripten_websocket_set_onerror_callback(ws_socket, NULL, on_ws_error);
    emscripten_websocket_set_onclose_callback(ws_socket, NULL, on_ws_close);

    printf("[net] connecting to %s\n", url);
    return true;
}

void net_shutdown(void) {
    if (ws_socket > 0) {
        emscripten_websocket_close(ws_socket, 1000, "shutdown");
        emscripten_websocket_delete(ws_socket);
        ws_socket = 0;
    }
    net_state.connected = false;
}

static void ws_send_binary(const uint8_t* data, int len) {
    if (!net_state.connected || ws_socket <= 0) return;
    emscripten_websocket_send_binary(ws_socket, (void*)data, (unsigned int)len);
}

void net_send_session(const uint8_t token[8]) {
    uint8_t buf[9];
    buf[0] = NET_MSG_SESSION;
    memcpy(&buf[1], token, 8);
    ws_send_binary(buf, 9);
}

void net_send_input(uint8_t flags, uint8_t action, uint8_t mining_target) {
    uint8_t buf[4];
    buf[0] = NET_MSG_INPUT;
    buf[1] = flags;
    buf[2] = action;
    buf[3] = mining_target;
    ws_send_binary(buf, 4);
}

void net_send_state(float x, float y, float vx, float vy, float angle) {
    uint8_t buf[23];
    buf[0] = NET_MSG_STATE;
    buf[1] = net_state.local_id;
    write_f32_le(&buf[2], x);
    write_f32_le(&buf[6], y);
    write_f32_le(&buf[10], vx);
    write_f32_le(&buf[14], vy);
    write_f32_le(&buf[18], angle);
    buf[22] = 0;
    ws_send_binary(buf, 23);
}

void net_poll(void) {
    /* Emscripten WebSocket callbacks fire on the main thread automatically. */
}

#else

/* ========================================================================= */
/* Native implementation using mongoose WebSocket client                     */
/* ========================================================================= */

#include "mongoose.h"

static struct mg_mgr net_mgr;
static struct mg_connection *ws_conn = NULL;
static bool mgr_initialized = false;

static void ws_send_binary(const uint8_t* data, int len) {
    if (!net_state.connected || !ws_conn) return;
    mg_ws_send(ws_conn, data, (size_t)len, WEBSOCKET_OP_BINARY);
}

static void net_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_WS_OPEN) {
        net_state.connected = true;
        ws_conn = c;
        printf("[net] connected to server\n");
        ensure_session_token();
        send_session_token();
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        handle_message((const uint8_t *)wm->data.buf, (int)wm->data.len);
    } else if (ev == MG_EV_ERROR) {
        printf("[net] connection error: %s\n", (char *)ev_data);
        net_state.connected = false;
        ws_conn = NULL;
    } else if (ev == MG_EV_CLOSE) {
        printf("[net] disconnected from server\n");
        net_state.connected = false;
        ws_conn = NULL;
    }
}

bool net_init(const char* url, const NetCallbacks* callbacks) {
    memset(&net_state, 0, sizeof(net_state));
    net_state.local_id = 0xFF;
    if (callbacks) net_state.callbacks = *callbacks;

    if (!url || url[0] == '\0') {
        printf("[net] no server URL provided, multiplayer disabled\n");
        return false;
    }

    mg_mgr_init(&net_mgr);
    mgr_initialized = true;

    struct mg_connection *c = mg_ws_connect(&net_mgr, url, net_ev_handler, NULL, NULL);
    if (!c) {
        printf("[net] failed to connect to %s\n", url);
        mg_mgr_free(&net_mgr);
        mgr_initialized = false;
        return false;
    }

    printf("[net] connecting to %s\n", url);
    return true;
}

void net_shutdown(void) {
    if (ws_conn) {
        mg_ws_send(ws_conn, "", 0, WEBSOCKET_OP_CLOSE);
        ws_conn = NULL;
    }
    if (mgr_initialized) {
        mg_mgr_free(&net_mgr);
        mgr_initialized = false;
    }
    net_state.connected = false;
}

void net_send_session(const uint8_t token[8]) {
    uint8_t buf[9];
    buf[0] = NET_MSG_SESSION;
    memcpy(&buf[1], token, 8);
    ws_send_binary(buf, 9);
}

void net_send_input(uint8_t flags, uint8_t action, uint8_t mining_target) {
    uint8_t buf[4];
    buf[0] = NET_MSG_INPUT;
    buf[1] = flags;
    buf[2] = action;
    buf[3] = mining_target;
    ws_send_binary(buf, 4);
}

void net_send_state(float x, float y, float vx, float vy, float angle) {
    uint8_t buf[23];
    buf[0] = NET_MSG_STATE;
    buf[1] = net_state.local_id;
    write_f32_le(&buf[2], x);
    write_f32_le(&buf[6], y);
    write_f32_le(&buf[10], vx);
    write_f32_le(&buf[14], vy);
    write_f32_le(&buf[18], angle);
    buf[22] = 0;
    ws_send_binary(buf, 23);
}

void net_poll(void) {
    if (mgr_initialized) {
        mg_mgr_poll(&net_mgr, 0);  /* non-blocking */
    }
}

#endif /* __EMSCRIPTEN__ */

/* ---------- Common accessors --------------------------------------------- */

bool net_is_connected(void) {
    return net_state.connected;
}

uint8_t net_local_id(void) {
    return net_state.local_id;
}

const NetPlayerState* net_get_players(void) {
    return net_state.players;
}

int net_remote_player_count(void) {
    int count = 0;
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (net_state.players[i].active && i != (int)net_state.local_id) {
            count++;
        }
    }
    return count;
}

const char* net_server_hash(void) {
    return net_state.server_hash;
}
