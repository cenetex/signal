/*
 * net.c — Multiplayer networking implementation for Signal Space Miner.
 *
 * WASM build: Uses emscripten WebSocket API.
 * Native build: Uses mongoose WebSocket client.
 */
#include "net.h"
#include "mining_client.h"
#include "mining.h"  /* mining_alphanumeric_callsign — pubkey-derived */
#include "signal_crypto.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef _WIN32
/* GetSystemTimePreciseAsFileTime — sub-microsecond wall clock on Win8+. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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
    char callsign[8];
    bool callsign_ready;
    char server_url[256];
    /* Layer A.2 of #479 — Ed25519 pubkey advertised in REGISTER_PUBKEY
     * on every connect/reconnect. Owned by the client at large
     * (game_t::identity); set via net_set_identity_pubkey before
     * net_init runs the WebSocket handshake. */
    uint8_t identity_pubkey[32];
    bool identity_pubkey_ready;
    /* Layer A.3 of #479 — Ed25519 secret for signing state-changing
     * actions. Owned by the client (game_t::identity); installed via
     * net_set_identity_secret. Never sent on the wire. */
    uint8_t identity_secret[64];
    bool identity_secret_ready;
    /* Monotonic per-process nonce high-water mark for signed actions.
     * Seeded on first use to current wall-clock microseconds so a
     * client-side restart still produces nonces that strictly exceed
     * the server's persisted last_signed_nonce in practice (server
     * also rejects strict-replay, so monotonicity is what matters). */
    uint64_t signed_action_nonce;
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

static uint16_t read_u16_le(const uint8_t* buf) {
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
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

static void send_fracture_claim(uint32_t fracture_id, uint32_t burst_nonce,
                                mining_grade_t claimed_grade) {
    uint8_t buf[FRACTURE_CLAIM_SIZE];
    buf[0] = NET_MSG_FRACTURE_CLAIM;
    buf[1] = (uint8_t)(fracture_id);
    buf[2] = (uint8_t)(fracture_id >> 8);
    buf[3] = (uint8_t)(fracture_id >> 16);
    buf[4] = (uint8_t)(fracture_id >> 24);
    buf[5] = (uint8_t)(burst_nonce);
    buf[6] = (uint8_t)(burst_nonce >> 8);
    buf[7] = (uint8_t)(burst_nonce >> 16);
    buf[8] = (uint8_t)(burst_nonce >> 24);
    buf[9] = (uint8_t)claimed_grade;
    ws_send_binary(buf, sizeof(buf));
}

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
        size_t got = fread(net_state.session_token, 1, 8, f);
        fclose(f);
        if (got != 8) {
            /* Short read from /dev/urandom; fall back to time-based seed */
            uint32_t seed = (uint32_t)time(NULL);
            for (int i = 0; i < 8; i++) {
                seed = seed * 1103515245u + 12345u;
                net_state.session_token[i] = (uint8_t)(seed >> 16);
            }
        }
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

const uint8_t* net_local_session_token(void) {
    return net_state.session_token_ready ? net_state.session_token : NULL;
}

static void ensure_callsign(void) {
    if (net_state.callsign_ready) return;
    /* Callsign is now derived from the player's Ed25519 pubkey via
     * mining_alphanumeric_callsign(). Same pubkey → same callsign on
     * every machine forever, no localStorage cache needed. The legacy
     * random/localStorage path is gone. */
    if (!net_state.identity_pubkey_ready) {
        /* Pubkey not yet provided — defer; main.c installs it before
         * net_init via net_set_identity_pubkey(). Leave callsign as-is
         * (zeroed); ensure_callsign() will be called again. */
        return;
    }
    mining_alphanumeric_callsign(net_state.identity_pubkey, net_state.callsign);
    net_state.callsign_ready = true;
    printf("[net] callsign: %s\n", net_state.callsign);
}

/* Layer A.2 of #479 — send the persistent Ed25519 pubkey to the server
 * immediately on connect, BEFORE the SESSION handshake, so the server
 * can bind (pubkey ↔ session_token) for this connection. Identity at
 * the wire level is still the 8-byte session_token; A.3 will require
 * signed inputs. */
static void send_register_pubkey(void) {
    if (!net_state.identity_pubkey_ready) return;
    uint8_t buf[REGISTER_PUBKEY_MSG_SIZE];
    buf[0] = NET_MSG_REGISTER_PUBKEY;
    memcpy(&buf[1], net_state.identity_pubkey, 32);
    ws_send_binary(buf, REGISTER_PUBKEY_MSG_SIZE);
    printf("[net] sent pubkey registration (%02x%02x%02x%02x...)\n",
           net_state.identity_pubkey[0], net_state.identity_pubkey[1],
           net_state.identity_pubkey[2], net_state.identity_pubkey[3]);
}

void net_set_identity_pubkey(const uint8_t pubkey[32]) {
    if (!pubkey) {
        net_state.identity_pubkey_ready = false;
        return;
    }
    memcpy(net_state.identity_pubkey, pubkey, 32);
    net_state.identity_pubkey_ready = true;
    /* Now that we have a pubkey, the callsign can be derived. If
     * ensure_callsign() ran earlier and bailed because the pubkey
     * wasn't set yet, the callsign[] is still zeroed — clear the
     * ready flag so the next ensure_callsign() call does the work. */
    net_state.callsign_ready = false;
}

void net_set_identity_secret(const uint8_t secret[64]) {
    if (!secret) {
        memset(net_state.identity_secret, 0, sizeof(net_state.identity_secret));
        net_state.identity_secret_ready = false;
        return;
    }
    memcpy(net_state.identity_secret, secret, 64);
    net_state.identity_secret_ready = true;
}

bool net_has_identity_secret(void) {
    return net_state.identity_secret_ready;
}

/* Allocate a strictly-increasing nonce. Seeded on first call to the
 * current wall-clock in microseconds so a process restart still beats
 * any nonce we used last run (the server's persisted last_signed_nonce
 * also gates this, but the client cooperating means fewer rejects). */
static uint64_t next_signed_action_nonce(void) {
    uint64_t now_us;
#ifdef __EMSCRIPTEN__
    /* Date.now() has ms resolution; multiply to keep us in the same
     * units as native. */
    now_us = (uint64_t)emscripten_get_now() * 1000ULL;
#elif defined(_WIN32)
    /* MSVC has no clock_gettime/CLOCK_REALTIME. GetSystemTimePreciseAsFileTime
     * returns 100-ns ticks since 1601-01-01 UTC; subtract the 1601→1970
     * delta and divide to microseconds since the Unix epoch. */
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t ticks_100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    now_us = (ticks_100ns - 116444736000000000ULL) / 10ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    now_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000);
#endif
    if (now_us <= net_state.signed_action_nonce)
        net_state.signed_action_nonce += 1;
    else
        net_state.signed_action_nonce = now_us;
    return net_state.signed_action_nonce;
}

bool net_send_signed_action(uint8_t action_type,
                            const uint8_t *payload, uint16_t payload_len) {
    if (!net_state.identity_secret_ready) return false;
    if (payload_len > SIGNED_ACTION_MAX_PAYLOAD) return false;

    /* On-stack scratch is fine: max-sized message is 12 + 256 + 64 = 332. */
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + SIGNED_ACTION_MAX_PAYLOAD +
                SIGNED_ACTION_SIG_SIZE];
    uint64_t nonce = next_signed_action_nonce();
    buf[0] = NET_MSG_SIGNED_ACTION;
    for (int i = 0; i < 8; i++) buf[1 + i] = (uint8_t)(nonce >> (i * 8));
    buf[9]  = action_type;
    buf[10] = (uint8_t)(payload_len & 0xFF);
    buf[11] = (uint8_t)(payload_len >> 8);
    if (payload && payload_len) memcpy(&buf[12], payload, payload_len);
    /* Sign (nonce || action_type || payload_len || payload) =
     * exactly bytes [1..12+payload_len). The leading message-type byte
     * and trailing signature are NOT part of the signed envelope. */
    signal_crypto_sign(&buf[12 + payload_len],
                       &buf[1], (size_t)(11 + (int)payload_len),
                       net_state.identity_secret);
    int total = SIGNED_ACTION_HEADER_SIZE + (int)payload_len +
                (int)SIGNED_ACTION_SIG_SIZE;
    ws_send_binary(buf, total);
    return true;
}

bool net_send_claim_legacy_save(const char *token_basename) {
    if (!token_basename || !net_state.identity_secret_ready) return false;
    size_t hex_len = strlen(token_basename);
    if (hex_len == 0 || hex_len > 64) return false;
    /* Sign domain || token_hex with the persistent identity. */
    const char *domain = CLAIM_LEGACY_SAVE_DOMAIN;
    size_t dlen = strlen(domain);
    uint8_t msg[64 + 64];
    if (dlen + hex_len > sizeof(msg)) return false;
    memcpy(msg, domain, dlen);
    memcpy(msg + dlen, token_basename, hex_len);
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, dlen + hex_len, net_state.identity_secret);

    uint8_t buf[2 + 64 + SIGNAL_CRYPTO_SIG_BYTES];
    buf[0] = NET_MSG_CLAIM_LEGACY_SAVE;
    buf[1] = (uint8_t)hex_len;
    memcpy(&buf[2], token_basename, hex_len);
    memcpy(&buf[2 + hex_len], sig, SIGNAL_CRYPTO_SIG_BYTES);
    ws_send_binary(buf, (int)(2 + hex_len + SIGNAL_CRYPTO_SIG_BYTES));
    printf("[net] sent legacy-save claim for %s\n", token_basename);
    return true;
}

static void send_session_token(void) {
    uint8_t buf[16]; /* type(1) + token(8) + callsign(7) */
    buf[0] = NET_MSG_SESSION;
    memcpy(&buf[1], net_state.session_token, 8);
    memcpy(&buf[9], net_state.callsign, 7);
    ws_send_binary(buf, 16);
    printf("[net] sent session token + callsign %s\n", net_state.callsign);
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
            if (len >= 45) {
                for (int t = 0; t < 10; t++)
                    ps->towed_fragments[t] = (uint16_t)data[25 + t * 2]
                                           | ((uint16_t)data[25 + t * 2 + 1] << 8);
            } else {
                for (int t = 0; t < 10; t++) ps->towed_fragments[t] = 0xFFFFu;
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
                for (int t = 0; t < 10; t++)
                    ps->towed_fragments[t] = (uint16_t)p[24 + t * 2]
                                           | ((uint16_t)p[24 + t * 2 + 1] << 8);
                memcpy(ps->callsign, &p[44], 7);
                ps->callsign[7] = '\0';
                ps->beam_start_x = read_f32_le(&p[51]);
                ps->beam_start_y = read_f32_le(&p[55]);
                ps->beam_end_x   = read_f32_le(&p[59]);
                ps->beam_end_y   = read_f32_le(&p[63]);
                ps->active = true;
                if (net_state.callbacks.on_state) {
                    net_state.callbacks.on_state(ps);
                }
            }
        }
        break;

    case NET_MSG_WORLD_ASTEROIDS:
        if (len < 3) break;
        {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * ASTEROID_RECORD_SIZE;
            if (len < expected) break;
            if (net_state.callbacks.on_asteroids) {
                /* File-scope buffer sized to MAX_ASTEROIDS so a dense belt
                 * view (which can now exceed 512 rocks post-#285) isn't
                 * truncated. Stack allocation would blow the WASM main
                 * thread's 64KB stack. */
                static NetAsteroidState arr[MAX_ASTEROIDS];
                int decoded = (count > MAX_ASTEROIDS) ? MAX_ASTEROIDS : count;
                for (int i = 0; i < decoded; i++) {
                    const uint8_t* p = &data[3 + i * ASTEROID_RECORD_SIZE];
                    arr[i].index  = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                    arr[i].flags  = p[2];
                    arr[i].x      = read_f32_le(&p[3]);
                    arr[i].y      = read_f32_le(&p[7]);
                    arr[i].vx     = read_f32_le(&p[11]);
                    arr[i].vy     = read_f32_le(&p[15]);
                    arr[i].hp     = read_f32_le(&p[19]);
                    arr[i].ore    = read_f32_le(&p[23]);
                    arr[i].radius = read_f32_le(&p[27]);
                    arr[i].smelt_progress = (float)p[31] / 255.0f;
                    arr[i].grade = p[32];
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
                    float pool = read_f32_le(&p[1 + COMMODITY_COUNT * 4]);
                    net_state.callbacks.on_stations(idx, inv, pool);
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
                pss.station_balance  = read_f32_le(&data[6]);
                pss.docked          = data[10] != 0;
                pss.current_station = data[11];
                pss.mining_level    = data[12];
                pss.hold_level      = data[13];
                pss.tractor_level   = data[14];
                pss.autopilot_mode  = data[15]; /* repurposed reserved byte */
                for (int c = 0; c < COMMODITY_COUNT; c++)
                    pss.cargo[c] = read_f32_le(&data[16 + c * 4]);
                int off = 16 + COMMODITY_COUNT * 4;
                if (len >= off + 23) {
                    pss.nearby_fragments = data[off];
                    pss.tractor_fragments = data[off + 1];
                    pss.towed_count = data[off + 2];
                    for (int t = 0; t < 10; t++)
                        pss.towed_fragments[t] = (uint16_t)data[off + 3 + t * 2]
                                               | ((uint16_t)data[off + 3 + t * 2 + 1] << 8);
                    pss.autopilot_target = (len >= off + 24) ? data[off + 23] : 0xFF;
                    /* A* path waypoints from server. */
                    int path_off = off + 24;
                    if (len >= path_off + 2) {
                        pss.path_count = data[path_off];
                        pss.path_current = data[path_off + 1];
                        if (pss.path_count > 12) pss.path_count = 12;
                        for (int i = 0; i < pss.path_count && path_off + 2 + i * 8 + 8 <= len; i++) {
                            pss.path_x[i] = read_f32_le(&data[path_off + 2 + i * 8]);
                            pss.path_y[i] = read_f32_le(&data[path_off + 2 + i * 8 + 4]);
                        }
                    }
                } else {
                    for (int t = 0; t < 10; t++) pss.towed_fragments[t] = 0xFFFFu;
                    pss.autopilot_target = 0xFF;
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
            moff += MAX_ARMS * 4;
            /* Placement plans */
            si.plan_count = data[moff];
            if (si.plan_count > STATION_PLAN_RECORD_COUNT) si.plan_count = STATION_PLAN_RECORD_COUNT;
            moff++;
            for (int p = 0; p < STATION_PLAN_RECORD_COUNT; p++) {
                si.plans[p].type  = (module_type_t)data[moff + 0];
                si.plans[p].ring  = data[moff + 1];
                si.plans[p].slot  = data[moff + 2];
                si.plans[p].owner = (int8_t)data[moff + 3];
                moff += STATION_PLAN_RECORD_SIZE;
            }
            /* Pending shipyard orders */
            si.pending_scaffold_count = data[moff];
            if (si.pending_scaffold_count > STATION_PENDING_SCAFFOLD_RECORD_COUNT)
                si.pending_scaffold_count = STATION_PENDING_SCAFFOLD_RECORD_COUNT;
            moff++;
            for (int p = 0; p < STATION_PENDING_SCAFFOLD_RECORD_COUNT; p++) {
                si.pending_scaffolds[p].type  = (module_type_t)data[moff + 0];
                int8_t owner = (int8_t)data[moff + 1];
                si.pending_scaffolds[p].owner = (data[moff + 1] == 0xFF) ? -1 : owner;
                moff += STATION_PENDING_SCAFFOLD_RECORD_SIZE;
            }
            /* Currency name trailer — 32 bytes, null-padded. */
            memcpy(si.currency_name, &data[moff], STATION_IDENTITY_CURRENCY_NAME_LEN - 1);
            si.currency_name[STATION_IDENTITY_CURRENCY_NAME_LEN - 1] = '\0';
            moff += STATION_IDENTITY_CURRENCY_NAME_LEN;
            /* Station Ed25519 pubkey (#479 B). The server only sends the
             * pubkey; private material stays operator-side. */
            memcpy(si.station_pubkey, &data[moff], STATION_IDENTITY_PUBKEY_LEN);
            net_state.callbacks.on_station_identity(&si);
        }
        break;

    case NET_MSG_WORLD_SCAFFOLDS:
        if (len >= 2 && net_state.callbacks.on_scaffolds) {
            int count = data[1];
            if (count < 0) count = 0;
            if (count * SCAFFOLD_RECORD_SIZE + 2 > len)
                count = (len - 2) / SCAFFOLD_RECORD_SIZE;
            NetScaffoldState scaffolds[16];
            int max = (count > 16) ? 16 : count;
            for (int i = 0; i < max; i++) {
                const uint8_t *p = &data[2 + i * SCAFFOLD_RECORD_SIZE];
                scaffolds[i].index       = p[0];
                scaffolds[i].state       = p[1];
                scaffolds[i].module_type = p[2];
                scaffolds[i].owner       = (p[3] == 0xFF) ? -1 : (int8_t)p[3];
                scaffolds[i].pos_x       = read_f32_le(&p[4]);
                scaffolds[i].pos_y       = read_f32_le(&p[8]);
                scaffolds[i].vel_x       = read_f32_le(&p[12]);
                scaffolds[i].vel_y       = read_f32_le(&p[16]);
                scaffolds[i].radius      = read_f32_le(&p[20]);
                scaffolds[i].build_amount= read_f32_le(&p[24]);
            }
            net_state.callbacks.on_scaffolds(scaffolds, max);
        }
        break;

    case NET_MSG_HAIL_RESPONSE:
        if (len >= 6 && net_state.callbacks.on_hail_response) {
            uint8_t station = data[1];
            float credits = read_f32_le(&data[2]);
            /* Contract idx added in the hail-as-quest change; old servers
             * that don't send it decode as "no contract" (0xFF). */
            int contract_index = (len >= 7 && data[6] != 0xFF) ? (int)data[6] : -1;
            net_state.callbacks.on_hail_response(station, credits, contract_index);
        }
        break;

    case NET_MSG_EVENTS:
        if (len >= 2 && net_state.callbacks.on_events) {
            int ecount = data[1];
            if (ecount > SIM_MAX_EVENTS) ecount = SIM_MAX_EVENTS;
            if ((int)len < 2 + ecount * NET_EVENT_RECORD_SIZE) break;
            sim_event_t evbuf[SIM_MAX_EVENTS];
            for (int i = 0; i < ecount; i++) {
                const uint8_t *p = &data[2 + i * NET_EVENT_RECORD_SIZE];
                sim_event_t *ev = &evbuf[i];
                memset(ev, 0, sizeof(*ev));
                ev->type = (sim_event_type_t)p[0];
                ev->player_id = (int)p[1];
                switch (ev->type) {
                case SIM_EVENT_FRACTURE:
                    ev->fracture.tier = (asteroid_tier_t)p[2]; break;
                case SIM_EVENT_PICKUP:
                    ev->pickup.ore = read_f32_le(&p[2]);
                    ev->pickup.fragments = (int)p[6]; break;
                case SIM_EVENT_UPGRADE:
                    ev->upgrade.upgrade = (ship_upgrade_t)p[2]; break;
                case SIM_EVENT_DAMAGE:
                    ev->damage.amount   = read_f32_le(&p[2]);
                    ev->damage.source_x = read_f32_le(&p[6]);
                    ev->damage.source_y = read_f32_le(&p[10]); break;
                case SIM_EVENT_NPC_KILL:
                    ev->npc_kill.cause    = p[2];
                    ev->npc_kill.npc_role = p[3];
                    memcpy(ev->npc_kill.killer_token, &p[4], 8); break;
                case SIM_EVENT_OUTPOST_PLACED:
                    ev->outpost_placed.slot = (int)p[2]; break;
                case SIM_EVENT_OUTPOST_ACTIVATED:
                    ev->outpost_activated.slot = (int)p[2]; break;
                case SIM_EVENT_MODULE_ACTIVATED:
                    ev->module_activated.station = (int)p[2];
                    ev->module_activated.module_idx = (int)p[3];
                    ev->module_activated.module_type = (int)p[4]; break;
                case SIM_EVENT_NPC_SPAWNED:
                    ev->npc_spawned.slot = (int)p[2];
                    ev->npc_spawned.role = (npc_role_t)p[3];
                    ev->npc_spawned.home_station = (int)p[4]; break;
                case SIM_EVENT_STATION_CONNECTED:
                    ev->station_connected.connected_count = (int)p[2]; break;
                case SIM_EVENT_CONTRACT_COMPLETE:
                    ev->contract_complete.action = (contract_action_t)p[2]; break;
                case SIM_EVENT_SCAFFOLD_READY:
                    ev->scaffold_ready.station = (int)p[2];
                    ev->scaffold_ready.module_type = (int)p[3]; break;
                case SIM_EVENT_SELL:
                    ev->sell.station     = (int)p[2];
                    ev->sell.grade       = p[3];
                    ev->sell.base_cr     = (int)read_u32_le(&p[4]);
                    ev->sell.bonus_cr    = (int)read_u32_le(&p[8]);
                    ev->sell.by_contract = p[12];
                    break;
                case SIM_EVENT_ORDER_REJECTED:
                    ev->order_rejected.reason = p[2];
                    break;
                default: break;
                }
            }
            net_state.callbacks.on_events(evbuf, ecount);
        }
        break;

    case NET_MSG_STATION_INGOTS:
        /* Wire payload kept for backward compatibility — a server still
         * sends per-station named-ingot snapshots derived from the
         * unified manifest. The client no longer maintains a separate
         * named-ingot store (single-source-of-truth refactor); the
         * STATION_MANIFEST summary feeds the trade UI counts and the
         * full provenance lives in the singleplayer mirror's manifest.
         * We just length-validate and drop the bytes here. */
        if (len >= STATION_INGOTS_HEADER) {
            int count = data[2];
            int expected = STATION_INGOTS_HEADER + count * NAMED_INGOT_RECORD_SIZE;
            (void)expected;
        }
        break;

    case NET_MSG_HIGHSCORES:
        if (len >= HIGHSCORE_HEADER && net_state.callbacks.on_highscores) {
            int count = data[1];
            int expected = HIGHSCORE_HEADER + count * HIGHSCORE_ENTRY_SIZE;
            if (len < expected) break;
            if (count > HIGHSCORE_TOP_N) count = HIGHSCORE_TOP_N;
            static NetHighscoreEntry scratch[HIGHSCORE_TOP_N];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[HIGHSCORE_HEADER + i * HIGHSCORE_ENTRY_SIZE];
                memcpy(scratch[i].callsign, p, 8);
                scratch[i].credits_earned = read_f32_le(&p[8]);
            }
            net_state.callbacks.on_highscores(scratch, count);
        }
        break;

    case NET_MSG_PLAYER_MANIFEST:
        if (len >= PLAYER_MANIFEST_HEADER && net_state.callbacks.on_player_manifest) {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = PLAYER_MANIFEST_HEADER + count * PLAYER_MANIFEST_ENTRY;
            if (len < expected) break;
            if (count > COMMODITY_COUNT * MINING_GRADE_COUNT)
                count = COMMODITY_COUNT * MINING_GRADE_COUNT;
            static NetStationManifestEntry pmscratch[COMMODITY_COUNT * MINING_GRADE_COUNT];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[PLAYER_MANIFEST_HEADER + i * PLAYER_MANIFEST_ENTRY];
                pmscratch[i].commodity = p[0];
                pmscratch[i].grade     = p[1];
                pmscratch[i].count     = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
            }
            net_state.callbacks.on_player_manifest(pmscratch, count);
        }
        break;

    case NET_MSG_STATION_MANIFEST:
        if (len >= STATION_MANIFEST_HEADER && net_state.callbacks.on_station_manifest) {
            uint8_t station_id = data[1];
            int count = (int)(data[2] | ((uint16_t)data[3] << 8));
            int expected = STATION_MANIFEST_HEADER + count * STATION_MANIFEST_ENTRY;
            if (len < expected) break;
            if (count > COMMODITY_COUNT * MINING_GRADE_COUNT)
                count = COMMODITY_COUNT * MINING_GRADE_COUNT;
            static NetStationManifestEntry scratch[COMMODITY_COUNT * MINING_GRADE_COUNT];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[STATION_MANIFEST_HEADER + i * STATION_MANIFEST_ENTRY];
                scratch[i].commodity = p[0];
                scratch[i].grade     = p[1];
                scratch[i].count     = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
            }
            net_state.callbacks.on_station_manifest(station_id, scratch, count);
        }
        break;

    case NET_MSG_HOLD_INGOTS:
        /* Wire payload kept for backward compatibility (see
         * NET_MSG_STATION_INGOTS above for the rationale). The named-
         * ingot identity now lives in the ship manifest, populated by
         * PLAYER_MANIFEST + the singleplayer mirror; the dedicated
         * hold-ingot wire snapshot is informational only. */
        if (len >= HOLD_INGOTS_HEADER) {
            int count = data[1];
            int expected = HOLD_INGOTS_HEADER + count * NAMED_INGOT_RECORD_SIZE;
            (void)expected;
        }
        break;

    case NET_MSG_SIGNAL_CHANNEL:
        if (len >= 3 && net_state.callbacks.on_signal_channel) {
            int count = (int)(data[1] | ((uint16_t)data[2] << 8));
            int expected = 3 + count * SIGNAL_CHANNEL_RECORD_SIZE;
            if (len < expected) break;
            /* Cap at CAPACITY so we don't blow the static buffer if a
             * server version sends more records than we expect. */
            if (count > 100) count = 100;
            static NetSignalChannelMsg msgs[100];
            for (int i = 0; i < count; i++) {
                const uint8_t *p = &data[3 + i * SIGNAL_CHANNEL_RECORD_SIZE];
                uint64_t id = 0;
                for (int k = 0; k < 8; k++) id |= ((uint64_t)p[k]) << (8 * k);
                uint32_t ts = 0;
                for (int k = 0; k < 4; k++) ts |= ((uint32_t)p[8 + k]) << (8 * k);
                msgs[i].id = id;
                msgs[i].timestamp_ms = ts;
                msgs[i].sender_station = (int8_t)p[12];
                int tlen = p[13];
                if (tlen >= SIGNAL_CHANNEL_TEXT_MAX) tlen = SIGNAL_CHANNEL_TEXT_MAX - 1;
                memcpy(msgs[i].text, &p[14], tlen);
                msgs[i].text[tlen] = '\0';
                memcpy(msgs[i].entry_hash, &p[14 + 200], 32);
            }
            net_state.callbacks.on_signal_channel(msgs, count);
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
        if (len >= 43 && net_state.callbacks.on_death) {
            uint8_t pid = data[1];
            float px = read_f32_le(&data[2]);
            float py = read_f32_le(&data[6]);
            float vx = read_f32_le(&data[10]);
            float vy = read_f32_le(&data[14]);
            float ang = read_f32_le(&data[18]);
            float ore = read_f32_le(&data[22]);
            float earned = read_f32_le(&data[26]);
            float spent = read_f32_le(&data[30]);
            int asteroids = (int)read_f32_le(&data[34]);
            uint8_t rs = data[38];
            float fee = read_f32_le(&data[39]);
            net_state.callbacks.on_death(pid, px, py, vx, vy, ang,
                                         ore, earned, spent, asteroids, rs, fee);
        } else if (len >= 38 && net_state.callbacks.on_death) {
            /* Legacy 38-byte packet — no respawn-station/fee yet. */
            uint8_t pid = data[1];
            float px = read_f32_le(&data[2]);
            float py = read_f32_le(&data[6]);
            float vx = read_f32_le(&data[10]);
            float vy = read_f32_le(&data[14]);
            float ang = read_f32_le(&data[18]);
            float ore = read_f32_le(&data[22]);
            float earned = read_f32_le(&data[26]);
            float spent = read_f32_le(&data[30]);
            int asteroids = (int)read_f32_le(&data[34]);
            net_state.callbacks.on_death(pid, px, py, vx, vy, ang,
                                         ore, earned, spent, asteroids, 0, 0.0f);
        } else if (len >= 2 && net_state.callbacks.on_death) {
            /* Very-legacy short packet — position-less. */
            net_state.callbacks.on_death(data[1], 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0f);
        }
        break;

    case NET_MSG_WORLD_TIME:
        if (len >= 5 && net_state.callbacks.on_world_time) {
            float server_time = read_f32_le(&data[1]);
            net_state.callbacks.on_world_time(server_time);
        }
        break;

    case NET_MSG_FRACTURE_CHALLENGE:
        if (len >= FRACTURE_CHALLENGE_SIZE) {
            mining_client_claim_t claim = {0};
            uint32_t fracture_id = read_u32_le(&data[1]);
            uint32_t deadline_ms = read_u32_le(&data[37]);
            uint16_t burst_cap = read_u16_le(&data[41]);
            /* Server rebroadcasts challenges every 100ms while the
             * window is open so late joiners can race. Skip the work
             * if we already searched this fracture_id — the sha256
             * burst would be redundant and would re-send our claim. */
            const mining_client_t *mc = mining_client_get();
            if (mc->fracture_search_id == fracture_id) break;
            if (mining_client_search_fracture(fracture_id, &data[5], deadline_ms,
                                              burst_cap, &claim)) {
                send_fracture_claim(claim.fracture_id, claim.burst_nonce,
                                    claim.claimed_grade);
            }
        }
        break;

    case NET_MSG_FRACTURE_RESOLVED:
        if (len >= FRACTURE_RESOLVED_SIZE) {
            mining_client_resolve_fracture(read_u32_le(&data[1]),
                                           (mining_grade_t)data[69]);
        }
        break;

    case NET_MSG_CONTRACTS:
        if (len >= 2 && net_state.callbacks.on_contracts) {
            uint8_t count = data[1];
            if (len >= 2 + count * 28) {
                contract_t contracts[MAX_CONTRACTS];
                memset(contracts, 0, sizeof(contracts));
                int n = count < MAX_CONTRACTS ? count : MAX_CONTRACTS;
                for (int i = 0; i < n; i++) {
                    const uint8_t *p = &data[2 + i * 28];
                    contracts[i].active = true;
                    contracts[i].action = (p[0] <= CONTRACT_FRACTURE) ? (contract_action_t)p[0] : CONTRACT_TRACTOR;
                    contracts[i].station_index = (p[1] < MAX_STATIONS) ? p[1] : 0;
                    contracts[i].commodity = (p[2] < COMMODITY_COUNT) ? (commodity_t)p[2] : COMMODITY_FERRITE_ORE;
                    contracts[i].required_grade = (p[3] < MINING_GRADE_COUNT) ? p[3] : (uint8_t)MINING_GRADE_COMMON;
                    contracts[i].quantity_needed = read_f32_le(&p[4]);
                    contracts[i].base_price = read_f32_le(&p[8]);
                    contracts[i].age = read_f32_le(&p[12]);
                    contracts[i].target_pos.x = read_f32_le(&p[16]);
                    contracts[i].target_pos.y = read_f32_le(&p[20]);
                    contracts[i].target_index = (int)(int32_t)read_u32_le(&p[24]);
                    contracts[i].claimed_by = -1;
                }
                net_state.callbacks.on_contracts(contracts, n);
            }
        }
        break;

    case NET_MSG_LEGACY_SAVES_AVAILABLE:
        /* Layer A.4 of #479 — server reports legacy saves the player
         * could claim. For now we just log; a docked-UI integration is
         * a follow-up issue. Operators can trigger
         * net_send_claim_legacy_save() manually for a stranded player. */
        if (len >= LEGACY_SAVES_HEADER) {
            int count = data[1];
            int max = (len - LEGACY_SAVES_HEADER) / LEGACY_SAVES_PREFIX_LEN;
            if (count > max) count = max;
            if (count > LEGACY_SAVES_MAX_LIST) count = LEGACY_SAVES_MAX_LIST;
            printf("[net] %d legacy save(s) available — import via "
                   "net_send_claim_legacy_save():\n", count);
            for (int i = 0; i < count; i++) {
                char prefix[LEGACY_SAVES_PREFIX_LEN + 1];
                memcpy(prefix,
                       &data[LEGACY_SAVES_HEADER + i * LEGACY_SAVES_PREFIX_LEN],
                       LEGACY_SAVES_PREFIX_LEN);
                prefix[LEGACY_SAVES_PREFIX_LEN] = '\0';
                printf("[net]   [%d] %s...\n", i, prefix);
            }
            /* TODO(#479-A.5): surface this in the docked HUD as a one-tap
             * import prompt. Today the operator drives the claim. */
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
    /* Layer A.2 of #479 — pubkey registration MUST precede the session
     * handshake so the server can fold the pubkey into reconnect
     * resolution. */
    send_register_pubkey();
    /* Send session token immediately so server can match grace slots */
    ensure_session_token();
    ensure_callsign();
    send_session_token();
    /* Rebind the mining identity to the real (server-known) token. */
    mining_client_set_session_token(net_state.session_token);
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
    /* Preserve identity fields across the reset — main.c installs the
     * pubkey + secret BEFORE net_init so the first wire SESSION packet
     * carries the pubkey-derived alphanumeric callsign. A blanket memset
     * would zero them and the on-connect handshake would send an empty
     * callsign (server stores ""; HUD shows "SHIP"; highscores blank). */
    uint8_t saved_pubkey[32];
    uint8_t saved_secret[64];
    bool    saved_pub_ready    = net_state.identity_pubkey_ready;
    bool    saved_secret_ready = net_state.identity_secret_ready;
    memcpy(saved_pubkey, net_state.identity_pubkey, sizeof(saved_pubkey));
    memcpy(saved_secret, net_state.identity_secret, sizeof(saved_secret));

    memset(&net_state, 0, sizeof(net_state));
    net_state.local_id = 0xFF;
    if (callbacks) net_state.callbacks = *callbacks;

    memcpy(net_state.identity_pubkey, saved_pubkey, sizeof(saved_pubkey));
    memcpy(net_state.identity_secret, saved_secret, sizeof(saved_secret));
    net_state.identity_pubkey_ready = saved_pub_ready;
    net_state.identity_secret_ready = saved_secret_ready;

    if (!url || url[0] == '\0') {
        printf("[net] no server URL provided, multiplayer disabled\n");
        return false;
    }
    snprintf(net_state.server_url, sizeof(net_state.server_url), "%s", url);
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

bool net_reconnect(void) {
    if (net_state.server_url[0] == '\0') return false;
    if (ws_socket > 0) {
        emscripten_websocket_delete(ws_socket);
        ws_socket = 0;
    }
    /* Preserve callbacks and session token, reset connection state */
    net_state.connected = false;
    net_state.local_id = 0xFF;
    net_state.server_hash[0] = '\0';
    memset(net_state.players, 0, sizeof(net_state.players));

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = net_state.server_url;
    attr.protocols = NULL;
    attr.createOnMainThread = EM_TRUE;

    ws_socket = emscripten_websocket_new(&attr);
    if (ws_socket <= 0) {
        printf("[net] reconnect failed\n");
        return false;
    }
    emscripten_websocket_set_onopen_callback(ws_socket, NULL, on_ws_open);
    emscripten_websocket_set_onmessage_callback(ws_socket, NULL, on_ws_message);
    emscripten_websocket_set_onerror_callback(ws_socket, NULL, on_ws_error);
    emscripten_websocket_set_onclose_callback(ws_socket, NULL, on_ws_close);
    printf("[net] reconnecting to %s\n", net_state.server_url);
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

void net_send_input(uint8_t flags, uint8_t action, uint8_t mining_target,
                    uint8_t buy_grade, int8_t place_station,
                    int8_t place_ring, int8_t place_slot) {
    uint8_t buf[8];
    buf[0] = NET_MSG_INPUT;
    buf[1] = flags;
    buf[2] = action;
    buf[3] = mining_target;
    buf[4] = buy_grade;
    /* int8 -> uint8 round-trip preserves sentinel -1 (=> 0xFF). */
    buf[5] = (uint8_t)place_station;
    buf[6] = (uint8_t)place_ring;
    buf[7] = (uint8_t)place_slot;
    ws_send_binary(buf, 8);
}

void net_send_buy_ingot(const uint8_t ingot_pubkey[32]) {
    uint8_t buf[33];
    buf[0] = NET_MSG_BUY_INGOT;
    memcpy(&buf[1], ingot_pubkey, 32);
    ws_send_binary(buf, 33);
}

void net_send_deliver_ingot(uint8_t hold_index) {
    uint8_t buf[2];
    buf[0] = NET_MSG_DELIVER_INGOT;
    buf[1] = hold_index;
    ws_send_binary(buf, 2);
}

void net_send_plan(uint8_t op, int8_t station, int8_t ring, int8_t slot,
                   uint8_t module_type, float px, float py) {
    uint8_t buf[NET_PLAN_MSG_SIZE];
    buf[0] = NET_MSG_PLAN;
    buf[1] = op;
    buf[2] = (uint8_t)station;
    buf[3] = (uint8_t)ring;
    buf[4] = (uint8_t)slot;
    buf[5] = module_type;
    write_f32_le(&buf[6], px);
    write_f32_le(&buf[10], py);
    ws_send_binary(buf, NET_PLAN_MSG_SIZE);
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
        /* Layer A.2 of #479 — pubkey registration before SESSION. */
        send_register_pubkey();
        ensure_session_token();
        ensure_callsign();
        send_session_token();
        mining_client_set_session_token(net_state.session_token);
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
    /* Preserve identity fields across the reset — main.c installs the
     * pubkey + secret BEFORE net_init so the first wire SESSION packet
     * carries the pubkey-derived alphanumeric callsign. A blanket memset
     * would zero them and the on-connect handshake would send an empty
     * callsign (server stores ""; HUD shows "SHIP"; highscores blank). */
    uint8_t saved_pubkey[32];
    uint8_t saved_secret[64];
    bool    saved_pub_ready    = net_state.identity_pubkey_ready;
    bool    saved_secret_ready = net_state.identity_secret_ready;
    memcpy(saved_pubkey, net_state.identity_pubkey, sizeof(saved_pubkey));
    memcpy(saved_secret, net_state.identity_secret, sizeof(saved_secret));

    memset(&net_state, 0, sizeof(net_state));
    net_state.local_id = 0xFF;
    if (callbacks) net_state.callbacks = *callbacks;

    memcpy(net_state.identity_pubkey, saved_pubkey, sizeof(saved_pubkey));
    memcpy(net_state.identity_secret, saved_secret, sizeof(saved_secret));
    net_state.identity_pubkey_ready = saved_pub_ready;
    net_state.identity_secret_ready = saved_secret_ready;

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

void net_send_input(uint8_t flags, uint8_t action, uint8_t mining_target,
                    uint8_t buy_grade, int8_t place_station,
                    int8_t place_ring, int8_t place_slot) {
    uint8_t buf[8];
    buf[0] = NET_MSG_INPUT;
    buf[1] = flags;
    buf[2] = action;
    buf[3] = mining_target;
    buf[4] = buy_grade;
    /* int8 -> uint8 round-trip preserves sentinel -1 (=> 0xFF). */
    buf[5] = (uint8_t)place_station;
    buf[6] = (uint8_t)place_ring;
    buf[7] = (uint8_t)place_slot;
    ws_send_binary(buf, 8);
}

void net_send_buy_ingot(const uint8_t ingot_pubkey[32]) {
    uint8_t buf[33];
    buf[0] = NET_MSG_BUY_INGOT;
    memcpy(&buf[1], ingot_pubkey, 32);
    ws_send_binary(buf, 33);
}

void net_send_deliver_ingot(uint8_t hold_index) {
    uint8_t buf[2];
    buf[0] = NET_MSG_DELIVER_INGOT;
    buf[1] = hold_index;
    ws_send_binary(buf, 2);
}

void net_send_plan(uint8_t op, int8_t station, int8_t ring, int8_t slot,
                   uint8_t module_type, float px, float py) {
    uint8_t buf[NET_PLAN_MSG_SIZE];
    buf[0] = NET_MSG_PLAN;
    buf[1] = op;
    buf[2] = (uint8_t)station;
    buf[3] = (uint8_t)ring;
    buf[4] = (uint8_t)slot;
    buf[5] = module_type;
    write_f32_le(&buf[6], px);
    write_f32_le(&buf[10], py);
    ws_send_binary(buf, NET_PLAN_MSG_SIZE);
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

bool net_reconnect(void) {
    /* Native: reconnect via mongoose */
    if (net_state.server_url[0] == '\0') return false;
    if (ws_conn) { ws_conn->is_closing = 1; ws_conn = NULL; }
    net_state.connected = false;
    net_state.local_id = 0xFF;
    net_state.server_hash[0] = '\0';
    memset(net_state.players, 0, sizeof(net_state.players));
    ws_conn = mg_ws_connect(&net_mgr, net_state.server_url, net_ev_handler, NULL, NULL);
    printf("[net] reconnecting to %s\n", net_state.server_url);
    return ws_conn != NULL;
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

const char* net_local_callsign(void) {
    ensure_callsign();
    return net_state.callsign;
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
