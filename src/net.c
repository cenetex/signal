/*
 * net.c — Multiplayer networking implementation for Signal Space Miner.
 *
 * WASM build: Uses emscripten WebSocket API.
 * Native build: Stubbed — prints a notice and operates as disconnected.
 */
#include "net.h"

#include <string.h>
#include <stdio.h>

/* ---------- Shared state ------------------------------------------------- */

static struct {
    bool connected;
    uint8_t local_id;
    NetPlayerState players[NET_MAX_PLAYERS];
    NetCallbacks callbacks;
} net_state;

/* ---------- Protocol helpers --------------------------------------------- */

#ifdef __EMSCRIPTEN__

static void write_f32_le(uint8_t* buf, float v) {
    union { float f; uint32_t u; } conv;
    conv.f = v;
    buf[0] = (uint8_t)(conv.u);
    buf[1] = (uint8_t)(conv.u >> 8);
    buf[2] = (uint8_t)(conv.u >> 16);
    buf[3] = (uint8_t)(conv.u >> 24);
}

static float read_f32_le(const uint8_t* buf) {
    union { float f; uint32_t u; } conv;
    conv.u = (uint32_t)buf[0]
           | ((uint32_t)buf[1] << 8)
           | ((uint32_t)buf[2] << 16)
           | ((uint32_t)buf[3] << 24);
    return conv.f;
}

static void handle_message(const uint8_t* data, int len) {
    if (len < 1) return;

    switch (data[0]) {
    case NET_MSG_JOIN:
        if (len < 2) break;
        {
            uint8_t id = data[1];
            /* First JOIN we receive is our own ID assignment. */
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
            /* Skip our own state echoed back. */
            if (id == net_state.local_id) break;
            if (id >= NET_MAX_PLAYERS) break;

            NetPlayerState* ps = &net_state.players[id];
            ps->player_id = id;
            ps->x     = read_f32_le(&data[2]);
            ps->y     = read_f32_le(&data[6]);
            ps->vx    = read_f32_le(&data[10]);
            ps->vy    = read_f32_le(&data[14]);
            ps->angle = read_f32_le(&data[18]);
            ps->active = true;

            if (net_state.callbacks.on_state) {
                net_state.callbacks.on_state(ps);
            }
        }
        break;

    default:
        break;
    }
}

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
    return EM_TRUE;
}

static EM_BOOL on_ws_message(int eventType, const EmscriptenWebSocketMessageEvent* event, void* userData) {
    (void)eventType; (void)userData;
    if (event->isText) return EM_TRUE; /* Ignore text messages. */
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

    if (callbacks) {
        net_state.callbacks = *callbacks;
    }

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

void net_send_input(uint8_t flags, float angle) {
    uint8_t buf[6];
    buf[0] = NET_MSG_INPUT;
    buf[1] = flags;
    write_f32_le(&buf[2], angle);
    ws_send_binary(buf, 6);
}

void net_send_state(float x, float y, float vx, float vy, float angle) {
    uint8_t buf[22];
    buf[0] = NET_MSG_STATE;
    buf[1] = net_state.local_id;
    write_f32_le(&buf[2], x);
    write_f32_le(&buf[6], y);
    write_f32_le(&buf[10], vx);
    write_f32_le(&buf[14], vy);
    write_f32_le(&buf[18], angle);
    ws_send_binary(buf, 22);
}

void net_poll(void) {
    /* Emscripten WebSocket callbacks fire on the main thread automatically.
     * Nothing to do here — messages are handled in on_ws_message(). */
}

/* ========================================================================= */
/* Native stub — TODO: POSIX WebSocket client implementation                 */
/* ========================================================================= */
#else

bool net_init(const char* url, const NetCallbacks* callbacks) {
    memset(&net_state, 0, sizeof(net_state));
    net_state.local_id = 0xFF;

    if (callbacks) {
        net_state.callbacks = *callbacks;
    }

    if (url && url[0] != '\0') {
        printf("[net] native multiplayer not yet supported (URL: %s)\n", url);
        printf("[net] TODO: implement POSIX WebSocket client\n");
    }
    return false;
}

void net_shutdown(void) {
    net_state.connected = false;
}

void net_send_input(uint8_t flags, float angle) {
    (void)flags; (void)angle;
}

void net_send_state(float x, float y, float vx, float vy, float angle) {
    (void)x; (void)y; (void)vx; (void)vy; (void)angle;
}

void net_poll(void) {
    /* No-op on native builds. */
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
