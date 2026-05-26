/*
 * hud.c -- HUD layout, drawing primitives, and the main HUD text
 * renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "net.h"
#include "net_sync.h"
#include "inspect_anim.h"
#include "onboarding.h"
#include "world_draw.h"
#include "avatar.h"
#include "mining_client.h"
#include "mining.h"  /* mining_alphanumeric_callsign — pubkey-derived */
#include "manifest.h"
#include "signal_model.h"
#include "palette.h"
#include "contract_fit.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define HUD_LATENCY_WARN_MS 100.0f
#define HUD_LATENCY_BAD_MS 300.0f
#define HUD_FRAGMENT_NEARBY_RANGE 220.0f

#ifdef __EMSCRIPTEN__
EM_JS(int, signal_relay_debug_region_js, (char *out, int cap), {
    if (cap <= 0) return 0;
    var dbg = window.SIGNAL_RELAY_DEBUG || {};
    var value = dbg.region || "";
    var preferred = dbg.preferredRegion || "";
    if (value && preferred && value !== preferred) value = value + "<-" + preferred;
    stringToUTF8(String(value), out, cap);
    return lengthBytesUTF8(String(value));
})

EM_JS(int, signal_relay_debug_broker_latency_ms_js, (), {
    var dbg = window.SIGNAL_RELAY_DEBUG || {};
    var ms = Number(dbg.brokerLatencyMs || 0);
    return (isFinite(ms) && ms > 0) ? Math.round(ms) : 0;
})

EM_JS(int, signal_relay_debug_health_latency_ms_js, (), {
    var dbg = window.SIGNAL_RELAY_DEBUG || {};
    var ms = Number(dbg.healthLatencyMs || 0);
    return (isFinite(ms) && ms > 0) ? Math.round(ms) : 0;
})
#endif

/* ------------------------------------------------------------------ */
/* Station-local balance helper                                        */
/* ------------------------------------------------------------------ */

/* Returns the player's credit balance at the given station.
 * In singleplayer reads the mirrored ledger; in multiplayer uses
 * the cached value from the last PLAYER_SHIP message. */
static float client_station_balance(int station_idx) {
    if (g.multiplayer_enabled)
        return g.station_balance;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return 0.0f;
    const station_t *st = &g.world.stations[station_idx];
    const uint8_t *token = g.world.players[g.local_player_slot].session_token;
    uint8_t pseudo[32];
    client_session_pseudo_pubkey(token, pseudo);
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_pubkey, pseudo, 32) == 0)
            return st->ledger[i].balance;
    }
    return 0.0f;
}

/* Balance at the player's current docked/nearby station. */
float player_current_balance(void) {
    int st = LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station;
    return client_station_balance(st);
}

/* Forward decl — defined below near the other client_* helpers.
 * Fills `strongest_idx` with the station holding the largest pending
 * balance for the local player, `strongest_balance` with that amount,
 * and `other_count` with how many *other* stations also have a
 * positive balance. Returns true when there's anything to collect.
 * Singleplayer only — MP doesn't push per-station ledgers. */
static bool client_pending_summary(int *strongest_idx,
                                   float *strongest_balance,
                                   int *other_count);

/* Center `text` horizontally around screen-pixel x = `center_x` and
 * place its baseline at row index `row_idx` (already in canvas-cell
 * units). Cell is the sdtx character width (8 px today). Picks up
 * the current sdtx_color. Used by full-screen overlays that center
 * text and were doing the (cx - w*0.5)/cell math by hand at every
 * call. */
static void sdtx_centered_text(float center_x, float row_idx, float cell, const char *text) {
    if (!text) return;
    float w = (float)strlen(text) * cell;
    sdtx_pos((center_x - w * 0.5f) / cell, row_idx);
    sdtx_puts(text);
}

static uint32_t hud_alpha_pipeline_id;

static void hud_draw_alpha_rect(float x, float y, float width, float height,
                                float r, float g0, float b, float a) {
    if (width <= 0.0f || height <= 0.0f || a <= 0.0f) return;
    if (hud_alpha_pipeline_id == 0) {
        sgl_pipeline pip = sgl_make_pipeline(&(sg_pipeline_desc){
            .colors[0] = {
                .write_mask = SG_COLORMASK_RGBA,
                .blend = {
                    .enabled = true,
                    .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                    .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    .src_factor_alpha = SG_BLENDFACTOR_ONE,
                    .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                },
            },
        });
        hud_alpha_pipeline_id = pip.id;
    }

    sgl_push_pipeline();
    sgl_load_pipeline((sgl_pipeline){ hud_alpha_pipeline_id });
    sgl_begin_quads();
    sgl_c4f(r, g0, b, a);
    sgl_v2f(x, y);
    sgl_v2f(x + width, y);
    sgl_v2f(x + width, y + height);
    sgl_v2f(x, y + height);
    sgl_end();
    sgl_pop_pipeline();
}

/* ------------------------------------------------------------------ */
/* Action row classification — single priority chain shared by the     */
/* compact and wide HUDs. hud_classify_action() inspects player +      */
/* world state once and returns a tagged payload; the two renderers    */
/* (hud_render_action_compact / _wide) format it. This eliminates the  */
/* duplicate state machine and fixes the divergence that left towing  */
/* invisible in the wide HUD's action row.                             */
/* ------------------------------------------------------------------ */

typedef enum {
    HUD_ACTION_DOCKED = 0,
    HUD_ACTION_TARGET_ASTEROID,
    HUD_ACTION_SCAN_MODULE,        /* str_a = station name, str_b = module name (or NULL = core hub) */
    HUD_ACTION_SCAN_NPC,           /* str_a = "miner"/"hauler", int_a = total cargo */
    HUD_ACTION_SCAN_PILOT,         /* int_a = pilot id, int_b = hull, str_a = callsign when known */
    HUD_ACTION_MINING,             /* claim window after fracture */
    HUD_ACTION_TOWING,             /* int_a = towed_count, int_b = tractor_active (1/0) */
    HUD_ACTION_TRACTOR_LOCK,       /* int_a = tractor_fragments, int_b = nearby_fragments */
    HUD_ACTION_TRACTOR_REACHING,   /* tractor active, no frag yet — int_b = nearby_fragments */
    HUD_ACTION_FRAGMENTS_NEARBY,   /* tractor inactive, frags in range — int_b = nearby_fragments */
    HUD_ACTION_HOLD_FULL,
    HUD_ACTION_PENDING_COLLECT,    /* int_a = strongest-station balance,
                                    * int_b = other stations with pending,
                                    * str_a = strongest station's currency_name */
    HUD_ACTION_IDLE,
} hud_action_kind_t;

typedef struct {
    hud_action_kind_t kind;
    int int_a, int_b;
    const char *str_a, *str_b;
    /* Asteroid target: separate fields so renderers can format their
     * own short/long flavor of the same data. */
    int tier;            /* asteroid_tier_t */
    int commodity;       /* commodity_t */
    int grade;           /* mining_grade_t, for fragment text color */
} hud_action_t;

static uint8_t hud_best_towed_fragment_grade(void) {
    uint8_t best = (uint8_t)MINING_GRADE_COMMON;
    for (int t = 0; t < LOCAL_PLAYER.ship.towed_count; t++) {
        int idx = LOCAL_PLAYER.ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[idx];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        if (a->grade < (uint8_t)MINING_GRADE_COUNT && a->grade > best)
            best = a->grade;
    }
    return best;
}

static uint8_t hud_best_nearby_fragment_grade(void) {
    uint8_t best = (uint8_t)MINING_GRADE_COMMON;
    float range_sq = HUD_FRAGMENT_NEARBY_RANGE * HUD_FRAGMENT_NEARBY_RANGE;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        if (v2_dist_sq(a->pos, LOCAL_PLAYER.ship.pos) > range_sq) continue;
        if (a->grade < (uint8_t)MINING_GRADE_COUNT && a->grade > best)
            best = a->grade;
    }
    return best;
}

static void hud_set_grade_color(uint8_t grade) {
    uint8_t r, g0, b;
    if (grade >= (uint8_t)MINING_GRADE_COUNT)
        grade = (uint8_t)MINING_GRADE_COMMON;
    mining_grade_rgb((mining_grade_t)grade, &r, &g0, &b);
    sdtx_color3b(r, g0, b);
}

static const NetPlayerState *hud_net_player_state(int idx) {
    if (!g.multiplayer_enabled || idx < 0 || idx >= NET_MAX_PLAYERS)
        return NULL;
    const NetPlayerState *players = net_get_interpolated_players();
    if (players && players[idx].active) return &players[idx];
    players = net_get_players();
    if (players && players[idx].active) return &players[idx];
    return NULL;
}

static void hud_player_scan_label(int idx, char *out, size_t cap) {
    if (!out || cap == 0) return;
    const NetPlayerState *np = hud_net_player_state(idx);
    if (np && np->callsign[0]) {
        snprintf(out, cap, "%s", np->callsign);
    } else {
        snprintf(out, cap, "ID %d", idx);
    }
}

static hud_action_t hud_classify_action(int cargo_units, int cargo_capacity, float sig_quality) {
    hud_action_t out = {0};
    out.kind = HUD_ACTION_IDLE;
    out.grade = (int)MINING_GRADE_COMMON;
    if (LOCAL_PLAYER.docked) { out.kind = HUD_ACTION_DOCKED; return out; }
    /* Target asteroid (laser-pointed). */
    if (LOCAL_PLAYER.hover_asteroid >= 0 &&
        g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        const asteroid_t *a = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
        out.kind = HUD_ACTION_TARGET_ASTEROID;
        out.int_a = (int)lroundf(a->hp);
        out.tier = (int)a->tier;
        out.commodity = (int)a->commodity;
        out.grade = (int)a->grade;
        return out;
    }
    /* Scan results take precedence over towing/fragments — the player
     * actively pointed the beam at a thing they want info about. */
    if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 1) {
        const station_t *st = &g.world.stations[LOCAL_PLAYER.scan_target_index];
        out.kind = HUD_ACTION_SCAN_MODULE;
        out.str_a = st->name;
        if (LOCAL_PLAYER.scan_module_index >= 0)
            out.str_b = module_type_name(st->modules[LOCAL_PLAYER.scan_module_index].type);
        return out;
    }
    if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 2) {
        const npc_ship_t *npc = &g.world.npc_ships[LOCAL_PLAYER.scan_target_index];
        out.kind = HUD_ACTION_SCAN_NPC;
        out.str_a = (npc->role == NPC_ROLE_MINER) ? "miner" : "hauler";
        int total = 0;
        for (int ci = 0; ci < COMMODITY_COUNT; ci++) total += (int)lroundf(npc->cargo[ci]);
        out.int_a = total;
        return out;
    }
    if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 3) {
        const NetPlayerState *np = hud_net_player_state(LOCAL_PLAYER.scan_target_index);
        out.kind = HUD_ACTION_SCAN_PILOT;
        out.int_a = LOCAL_PLAYER.scan_target_index;
        out.str_a = (np && np->callsign[0]) ? np->callsign : NULL;
        if (LOCAL_PLAYER.scan_target_index >= 0 &&
            LOCAL_PLAYER.scan_target_index < MAX_PLAYERS &&
            g.world.players[LOCAL_PLAYER.scan_target_index].ship.hull > 0.0f)
            out.int_b = (int)lroundf(g.world.players[LOCAL_PLAYER.scan_target_index].ship.hull);
        return out;
    }
    if (mining_client_get()->fracture_search_timer > 0.0f) {
        out.kind = HUD_ACTION_MINING;
        return out;
    }
    if (LOCAL_PLAYER.ship.towed_count > 0) {
        out.kind = HUD_ACTION_TOWING;
        out.int_a = LOCAL_PLAYER.ship.towed_count;
        out.int_b = LOCAL_PLAYER.ship.tractor_active ? 1 : 0;
        out.grade = (int)hud_best_towed_fragment_grade();
        return out;
    }
    if (LOCAL_PLAYER.nearby_fragments > 0) {
        out.grade = (int)hud_best_nearby_fragment_grade();
        if (LOCAL_PLAYER.ship.tractor_active && LOCAL_PLAYER.tractor_fragments > 0) {
            out.kind = HUD_ACTION_TRACTOR_LOCK;
            out.int_a = LOCAL_PLAYER.tractor_fragments;
            out.int_b = LOCAL_PLAYER.nearby_fragments;
            return out;
        }
        if (LOCAL_PLAYER.ship.tractor_active) {
            out.kind = HUD_ACTION_TRACTOR_REACHING;
            out.int_b = LOCAL_PLAYER.nearby_fragments;
            return out;
        }
        out.kind = HUD_ACTION_FRAGMENTS_NEARBY;
        out.int_b = LOCAL_PLAYER.nearby_fragments;
        return out;
    }
    if (cargo_units >= cargo_capacity) {
        out.kind = HUD_ACTION_HOLD_FULL;
        return out;
    }
    /* Pending ledger credits — only surface when in usable signal so we
     * don't tease "collect" while H couldn't do anything. Names the
     * single largest-pending station so the per-station ledger model is
     * legible from the action row instead of being flattened into one
     * global number. */
    if (sig_quality >= 0.90f) {
        int best_idx = -1, others = 0;
        float best_bal = 0.0f;
        if (client_pending_summary(&best_idx, &best_bal, &others)) {
            out.kind = HUD_ACTION_PENDING_COLLECT;
            out.int_a = (int)lroundf(best_bal);
            out.int_b = others;
            const char *cn = g.world.stations[best_idx].currency_name;
            out.str_a = (cn && cn[0]) ? cn : "credits";
            return out;
        }
    }
    return out;
}

static void hud_format_action_compact(const hud_action_t *a, const char *dock_role,
                                      char *out, size_t out_size) {
    switch (a->kind) {
    case HUD_ACTION_DOCKED:
        snprintf(out, out_size, "%s CONSOLE", dock_role);
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        snprintf(out, out_size, "TGT %s // %s // %d HP",
                 asteroid_tier_name((asteroid_tier_t)a->tier),
                 commodity_code((commodity_t)a->commodity),
                 a->int_a);
        return;
    case HUD_ACTION_SCAN_MODULE:
        if (a->str_b) snprintf(out, out_size, "SCAN %s // %s", a->str_a, a->str_b);
        else          snprintf(out, out_size, "SCAN %s // CORE", a->str_a);
        return;
    case HUD_ACTION_SCAN_NPC:
        snprintf(out, out_size, "SCAN NPC // %s",
                 (a->str_a && a->str_a[0] == 'm') ? "MINER" : "HAULER");
        return;
    case HUD_ACTION_SCAN_PILOT:
        if (a->str_a) snprintf(out, out_size, "SCAN PILOT // %s", a->str_a);
        else          snprintf(out, out_size, "SCAN PILOT // ID %d", a->int_a);
        return;
    case HUD_ACTION_MINING:
        snprintf(out, out_size, "MINING... // CLAIM WINDOW");
        return;
    case HUD_ACTION_TOWING:
        if (a->int_b) snprintf(out, out_size, "TOWING %d // TRACTOR", a->int_a);
        else          snprintf(out, out_size, "TOWING %d // tap [Space] release", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_LOCK:
        snprintf(out, out_size, "TRACTOR // %d FRAG", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_REACHING:
        snprintf(out, out_size, "hold [Space] TRACTOR // %d", a->int_b);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        snprintf(out, out_size, "hold [Space] TRACTOR // %d nearby", a->int_b);
        return;
    case HUD_ACTION_HOLD_FULL:
        snprintf(out, out_size, "Hold full. Dock to sell.");
        return;
    case HUD_ACTION_PENDING_COLLECT:
        if (a->int_b > 0) {
            snprintf(out, out_size, "[H] collect %d %s (+%d)",
                     a->int_a, a->str_a ? a->str_a : "credits", a->int_b);
        } else {
            snprintf(out, out_size, "[H] collect %d %s",
                     a->int_a, a->str_a ? a->str_a : "credits");
        }
        return;
    case HUD_ACTION_IDLE:
    default:
        snprintf(out, out_size, "Nothing in range. Scan for rocks.");
        return;
    }
}

static void hud_format_action_wide(const hud_action_t *a, const station_t *current_station,
                                   char *out, size_t out_size) {
    switch (a->kind) {
    case HUD_ACTION_DOCKED:
        snprintf(out, out_size, "%s console",
                 current_station ? station_role_name(current_station) : "Station");
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        snprintf(out, out_size, "Target %s // %s // %d hp",
                 asteroid_tier_kind((asteroid_tier_t)a->tier),
                 commodity_short_name((commodity_t)a->commodity),
                 a->int_a);
        return;
    case HUD_ACTION_SCAN_MODULE:
        if (a->str_b) snprintf(out, out_size, "Scan %s // %s", a->str_a, a->str_b);
        else          snprintf(out, out_size, "Scan %s // core hub", a->str_a);
        return;
    case HUD_ACTION_SCAN_NPC:
        snprintf(out, out_size, "Scan NPC %s // cargo %d", a->str_a, a->int_a);
        return;
    case HUD_ACTION_SCAN_PILOT:
        if (a->str_a && a->int_b > 0)
            snprintf(out, out_size, "Scan pilot %s // hull %d", a->str_a, a->int_b);
        else if (a->str_a)
            snprintf(out, out_size, "Scan pilot %s", a->str_a);
        else
            snprintf(out, out_size, "Scan pilot %d", a->int_a);
        return;
    case HUD_ACTION_MINING:
        snprintf(out, out_size, "Mining... // claim window");
        return;
    case HUD_ACTION_TOWING:
        if (a->int_b) snprintf(out, out_size, "Towing %d // tractor on", a->int_a);
        else          snprintf(out, out_size, "Towing %d // tap [Space] to release", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_LOCK:
        snprintf(out, out_size, "Tractor lock // %d frag%s",
                 a->int_a, a->int_a == 1 ? "" : "s");
        return;
    case HUD_ACTION_TRACTOR_REACHING:
        snprintf(out, out_size, "Tractor reaching // %d nearby", a->int_b);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        snprintf(out, out_size, "Hold [Space] tractor // %d nearby", a->int_b);
        return;
    case HUD_ACTION_HOLD_FULL:
        snprintf(out, out_size, "Hold full. Dock to sell.");
        return;
    case HUD_ACTION_PENDING_COLLECT:
        if (a->int_b > 0) {
            snprintf(out, out_size, "H to hail // collect %d %s (+%d more)",
                     a->int_a, a->str_a ? a->str_a : "credits", a->int_b);
        } else {
            snprintf(out, out_size, "H to hail // collect %d %s",
                     a->int_a, a->str_a ? a->str_a : "credits");
        }
        return;
    case HUD_ACTION_IDLE:
    default:
        snprintf(out, out_size, "No target // line up a rock");
        return;
    }
}

static void hud_set_action_color(const hud_action_t *a) {
    switch (a->kind) {
    case HUD_ACTION_SCAN_MODULE:
    case HUD_ACTION_SCAN_NPC:
    case HUD_ACTION_SCAN_PILOT:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        hud_set_grade_color((uint8_t)a->grade);
        return;
    case HUD_ACTION_HOLD_FULL:
    case HUD_ACTION_PENDING_COLLECT:
        sdtx_color3b(PAL_ORE_AMBER);
        return;
    case HUD_ACTION_IDLE:
        sdtx_color3b(PAL_TEXT_MUTED);
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        if (a->tier == (int)ASTEROID_TIER_S) {
            hud_set_grade_color((uint8_t)a->grade);
            return;
        }
        sdtx_color3b(PAL_ACTIVE);
        return;
    case HUD_ACTION_TOWING:
    case HUD_ACTION_TRACTOR_LOCK:
    case HUD_ACTION_TRACTOR_REACHING:
        hud_set_grade_color((uint8_t)a->grade);
        return;
    case HUD_ACTION_DOCKED:
    case HUD_ACTION_MINING:
    default:
        sdtx_color3b(PAL_ACTIVE);
        return;
    }
}

/* Compact renderer — short-form ALL CAPS at the bottom of the top panel. */
static void hud_render_action_compact(const hud_action_t *a, const char *dock_role) {
    char text[128];
    hud_format_action_compact(a, dock_role, text, sizeof(text));
    hud_set_action_color(a);
    sdtx_puts(text);
}

/* Wide renderer — full English at the bottom of the top panel. */
static void hud_render_action_wide(const hud_action_t *a, const station_t *current_station) {
    char text[160];
    hud_format_action_wide(a, current_station, text, sizeof(text));
    hud_set_action_color(a);
    sdtx_puts(text);
}

static void hud_format_region_label(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
#ifdef __EMSCRIPTEN__
    (void)signal_relay_debug_region_js(out, (int)cap);
#endif
    if (out[0] == '\0') {
        if (g.multiplayer_enabled)
            snprintf(out, cap, "direct");
        else
            snprintf(out, cap, "solo");
    }
}

static void hud_format_latency_label(char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';

    float ping_ms = g.net_last_ping_rtt * 1000.0f;
    float ack_ms = g.net_last_ack_rtt * 1000.0f;
    if (ping_ms > 0.0f && ack_ms > 0.0f) {
        snprintf(out, cap, "ping %.0fms ack %.0fms", ping_ms, ack_ms);
        return;
    }
    if (ping_ms > 0.0f) {
        snprintf(out, cap, "ping %.0fms", ping_ms);
        return;
    }
    if (ack_ms > 0.0f) {
        snprintf(out, cap, "ack %.0fms", ack_ms);
        return;
    }

#ifdef __EMSCRIPTEN__
    int health_ms = signal_relay_debug_health_latency_ms_js();
    int broker_ms = signal_relay_debug_broker_latency_ms_js();
    if (health_ms > 0 && broker_ms > 0)
        snprintf(out, cap, "boot %dms health %dms", broker_ms, health_ms);
    else if (health_ms > 0)
        snprintf(out, cap, "health %dms", health_ms);
    else if (broker_ms > 0)
        snprintf(out, cap, "boot %dms", broker_ms);
    else
#endif
        snprintf(out, cap, "latency pending");
}

/* ------------------------------------------------------------------ */
/* Shared post-classify panels — render in BOTH compact and wide so a  */
/* small window doesn't silently lose signal-lost warnings, MP status, */
/* hail sigil, etc. Each is a one-shot draw guarded by its own state. */
/* ------------------------------------------------------------------ */

static void hud_draw_alpha_banner_and_mp_indicator(float screen_w, bool compact) {
    /* Version / connection status — top right */
    float info_x = ui_text_pos(fmaxf(8.0f, screen_w - (compact ? 100.0f : 140.0f)));
    float info_y = ui_text_pos(8.0f);
    sdtx_pos(info_x, info_y);
#ifdef GIT_HASH
    const char *client_hash = GIT_HASH;
#else
    const char *client_hash = "dev";
#endif
    if (g.multiplayer_enabled && net_is_connected()) {
        const char *srv = net_server_hash();
        bool match = srv[0] != '\0' && strcmp(client_hash, srv) == 0;
        if (match)              { sdtx_color3b(PAL_SYNC_OK);          sdtx_printf("v%s", client_hash); }
        else if (srv[0] == '\0'){ sdtx_color3b(PAL_SYNC_CONNECTING);  sdtx_puts("connecting..."); }
        else                    { sdtx_color3b(PAL_SYNC_RESYNCING);   sdtx_puts("syncing..."); }
        if (g.net_last_ack_rtt > 0.0f || g.net_last_ping_rtt > 0.0f) {
            float net_x = ui_text_pos(fmaxf(8.0f, screen_w - (compact ? 176.0f : 304.0f)));
            float ack_ms = g.net_last_ack_rtt * 1000.0f;
            float ping_ms = g.net_last_ping_rtt * 1000.0f;
            float gap_ms = (ack_ms > 0.0f && ping_ms > 0.0f && ack_ms > ping_ms)
                ? ack_ms - ping_ms : 0.0f;
            float warning_ms = (ack_ms > 0.0f) ? ack_ms : ping_ms;
            sdtx_pos(net_x, info_y + 1.2f);
            if (warning_ms >= HUD_LATENCY_BAD_MS)
                sdtx_color3b(PAL_WARNING);
            else if (warning_ms >= HUD_LATENCY_WARN_MS)
                sdtx_color3b(PAL_READY_YELLOW);
            else
                sdtx_color3b(PAL_TEXT_GREY);
            if (compact) {
                if (ping_ms > 0.0f && ack_ms > 0.0f)
                    sdtx_printf("ping %.0f ack %.0f", ping_ms, ack_ms);
                else if (ping_ms > 0.0f)
                    sdtx_printf("ping %.0fms", ping_ms);
                else
                    sdtx_printf("ack %.0fms", ack_ms);
                if (ping_ms > 0.0f && ack_ms > 0.0f) {
                    sdtx_pos(net_x, info_y + 2.4f);
                    sdtx_printf("gap %.0f q%u r%u",
                                gap_ms,
                                (unsigned)g.net_action_queue_count,
                                (unsigned)g.net_replay_count);
                }
            } else if (ping_ms > 0.0f && ack_ms > 0.0f) {
                sdtx_printf("ping %.0fms ack %.0fms gap %.0fms q%u r%u",
                            ping_ms,
                            ack_ms,
                            gap_ms,
                            (unsigned)g.net_action_queue_count,
                            (unsigned)g.net_replay_count);
            } else if (ping_ms > 0.0f) {
                sdtx_printf("ping %.0fms q%u r%u",
                            ping_ms,
                            (unsigned)g.net_action_queue_count,
                            (unsigned)g.net_replay_count);
            } else {
                sdtx_printf("ack %.0fms q%u r%u",
                            ack_ms,
                            (unsigned)g.net_action_queue_count,
                            (unsigned)g.net_replay_count);
            }
        }
    } else if (g.multiplayer_enabled) {
        sdtx_color3b(PAL_SYNC_OFFLINE);
        sdtx_puts("offline [P] reconnect");
    } else {
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_printf("v%s", client_hash);
    }
    /* Player callsign is rendered at top_row_0 below (see the "Use
     * the SESSION callsign" block) — derived from the pubkey via
     * mining_alphanumeric_callsign() once #513 lands. Don't double up
     * with a separate identity line here. */

    /* Alpha banner: repeating ticker across the top. */
    float bw = ui_safe_positive(sapp_widthf(), 1280.0f) /
               fmaxf(1.0f, ui_safe_positive(sapp_dpi_scale(), 1.0f));
    int cols = (int)(bw / 8.0f);
    char banner[512];
    char segment[192];
    char region[48];
    char latency[80];
    hud_format_region_label(region, sizeof(region));
    hud_format_latency_label(latency, sizeof(latency));
    snprintf(segment, sizeof(segment),
             "ALPHA // v%s // region %s // %s // frequent server resets // ",
             client_hash, region, latency);
    int seg_len = (int)strlen(segment);
    int pos = 0;
    while (pos < cols && pos < (int)sizeof(banner) - 1) {
        int left = (int)sizeof(banner) - 1 - pos;
        int copy = seg_len < left ? seg_len : left;
        memcpy(&banner[pos], segment, copy);
        pos += copy;
    }
    banner[pos < (int)sizeof(banner) ? pos : (int)sizeof(banner) - 1] = '\0';
    sdtx_pos(0.0f, 0.0f);
    sdtx_color3b(PAL_ALPHA_BANNER);
    sdtx_puts(banner);
}

/* Nearest station name in the bottom-left when undocked, plus the
 * housekeeping that expires a tracked contract once the server retired
 * it. The contract cleanup runs every frame regardless of layout. */
static void hud_draw_nav_label(float screen_w, float screen_h) {
    if (LOCAL_PLAYER.docked) return;
    const station_t *nav_st = navigation_station_ptr();
    if (nav_st && nav_st->name[0] != '\0') {
        sdtx_pos(ui_text_pos(16.0f), ui_text_pos(screen_h - 20.0f));
        sdtx_color3b(PAL_TEXT_FADED);
        int name_cols = (int)((screen_w - 24.0f) / 8.0f);
        sdtx_printf("%.*s", name_cols, nav_st->name);
    }
    if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
        /* Drop tracking when the contract closes OR when it leaves the
         * player's gossip mask (e.g., overwritten by FIFO eviction at a
         * later dock). */
        if (!g.world.contracts[g.tracked_contract].active ||
            !(g.player_known_contract_mask & (1u << g.tracked_contract)))
            g.tracked_contract = -1;
    }
}

/* Station hail sigil — mini profile pic in the lower-left that fades
 * in with hail_timer. "Caller ID" for the hint-bar message above. */
static void hud_draw_hail_sigil(float screen_w, float screen_h) {
    if (g.hail_timer <= 0.0f) return;
    if (g.hail_station_index < 0 || g.hail_station_index >= MAX_STATIONS) return;
    const avatar_cache_t *av = avatar_get(g.hail_station_index);
    if (!av || !av->texture_valid) return;

    float alpha = fminf(g.hail_timer / 0.5f, 1.0f);
    float sig_size = 48.0f;
    float sx0 = 16.0f;
    float sy0 = screen_h - sig_size - 32.0f;
    render_set_screen_space(screen_w, screen_h);
    draw_texture_rect(av->view_id, av->sampler_id,
                      sx0, sy0, sx0 + sig_size, sy0 + sig_size,
                      alpha, alpha, alpha, alpha);
    /* Gold border frame — "transmission active" radio indicator. */
    float border_a = 0.70f * alpha;
    sgl_begin_lines();
    sgl_c4f(0.78f, 0.63f, 0.19f, border_a);
    sgl_v2f(sx0,            sy0);            sgl_v2f(sx0 + sig_size, sy0);
    sgl_v2f(sx0 + sig_size, sy0);            sgl_v2f(sx0 + sig_size, sy0 + sig_size);
    sgl_v2f(sx0 + sig_size, sy0 + sig_size); sgl_v2f(sx0,            sy0 + sig_size);
    sgl_v2f(sx0,            sy0 + sig_size); sgl_v2f(sx0,            sy0);
    sgl_end();
}

static void hud_draw_module_inspect_pane(float screen_w) {
    if (g.inspect_station < 0 || g.inspect_station >= MAX_STATIONS) return;
    if (g.inspect_module < 0) return;
    if (LOCAL_PLAYER.docked) return;
    const station_t *ist = &g.world.stations[g.inspect_station];
    if (!station_exists(ist) || g.inspect_module >= ist->module_count) {
        g.inspect_station = -1;
        return;
    }
    const station_module_t *im = &ist->modules[g.inspect_module];
    station_flow_diag_t flow = station_module_flow_diag_view(
        ist, g.inspect_module, g.multiplayer_enabled && net_is_connected());
    float px = fmaxf(16.0f, screen_w - 260.0f);
    float py = 60.0f;
    float cell = 8.0f;
    float next_y = 60.0f;

    sdtx_pos(px / cell, py / cell);
    sdtx_color3b(PAL_ORE_AMBER);
    sdtx_printf("[ %s ]", module_type_name(im->type));

    sdtx_pos(px / cell, (py + 14.0f) / cell);
    sdtx_color3b(PAL_INSPECT_STATION);
    sdtx_printf("Station: %s", ist->name);

    sdtx_pos(px / cell, (py + 28.0f) / cell);
    sdtx_color3b(PAL_INSPECT_LOCATION);
    sdtx_printf("Ring %d  Slot %d", im->ring, im->slot);

    if (im->scaffold) {
        sdtx_pos(px / cell, (py + 42.0f) / cell);
        if (im->build_progress < 1.0f) {
            int pct = (int)lroundf(im->build_progress * 100.0f);
            sdtx_color3b(PAL_BUILD_SUPPLYING);
            sdtx_printf("SUPPLYING: %d%%", pct);
        } else {
            int pct = (int)lroundf((im->build_progress - 1.0f) * 100.0f);
            sdtx_color3b(PAL_BUILD_BUILDING);
            sdtx_printf("BUILDING: %d%%", pct);
        }
    } else {
        sdtx_pos(px / cell, (py + 42.0f) / cell);
        sdtx_color3b(PAL_ACTIVE);
        sdtx_puts("ONLINE");
    }
    if (flow != STATION_FLOW_DIAG_NONE) {
        sdtx_pos(px / cell, (py + next_y) / cell);
        if (flow == STATION_FLOW_DIAG_RUNNING)
            sdtx_color3b(120, 230, 180);
        else if (flow == STATION_FLOW_DIAG_SLOW_FEED)
            sdtx_color3b(245, 210, 115);
        else
            sdtx_color3b(255, 135, 120);
        sdtx_printf("FLOW: %s", station_flow_diag_label(flow));
        next_y += 14.0f;
    }
    sdtx_pos(px / cell, (py + next_y) / cell);
    sdtx_color3b(PAL_TEXT_GREY);
    sdtx_puts("[E] close");
}

static const char *hud_npc_role_label(uint8_t role) {
    switch ((npc_role_t)role) {
    case NPC_ROLE_MINER:  return "miner";
    case NPC_ROLE_HAULER: return "hauler";
    case NPC_ROLE_TOW:    return "tow";
    default: return "npc";
    }
}

static const char *hud_npc_state_label(uint8_t state) {
    switch ((npc_state_t)state) {
    case NPC_STATE_IDLE:               return "idle";
    case NPC_STATE_TRAVEL_TO_ASTEROID: return "to rock";
    case NPC_STATE_MINING:             return "mining";
    case NPC_STATE_RETURN_TO_STATION:  return "return";
    case NPC_STATE_DOCKED:             return "docked";
    case NPC_STATE_TRAVEL_TO_DEST:     return "to dest";
    case NPC_STATE_UNLOADING:          return "unload";
    default: return "unknown";
    }
}

static const char *hud_hull_class_label(uint8_t hull_class) {
    if (hull_class < (uint8_t)HULL_CLASS_COUNT)
        return HULL_DEFS[hull_class].name;
    return "ship";
}

static const char *hud_grade_short_label(uint8_t grade) {
    if (grade == (uint8_t)MINING_GRADE_COMMISSIONED) return "comm";
    return mining_grade_label((mining_grade_t)grade);
}

static bool hash32_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static void hud_hash_short_label(const uint8_t hash[32], char out[8]) {
    if (hash32_is_zero(hash)) {
        out[0] = '\0';
        return;
    }
    mining_callsign_from_pubkey(hash, out);
}

static void hud_cargo_label(const uint8_t pub[32], char out[12]) {
    if (hash32_is_zero(pub)) {
        out[0] = '\0';
        return;
    }
    mining_render_callsign(pub, out);
}

/* ----- scramble-resolve glyph animation -----
 * Tape-printer feel: when a row's content changes, each character starts
 * as a random glyph and resolves to its true value at t0 + i * stagger.
 * Once settled, the chars stay still — this is not a perpetual jitter.
 *
 * The codebase calls it "hash short label" but mining_callsign_from_pubkey
 * emits base58 — that's the only alphabet we scramble over today, so
 * the truth and the in-flight glyphs share a character class. */
#define HUD_SCRAMBLE_TOTAL_MS    250.0f   /* full settle time per row */
#define HUD_SCRAMBLE_STAGGER_MS   30.0f   /* per-character delay */

static char hud_random_glyph(uint32_t *rng) {
    /* tiny xorshift so the scramble is stable per-frame for the same
     * t but different across (row, char_index, frame). */
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x ? x : 0xDEADBEEFu;
    static const char b58[] =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    return b58[x % (sizeof(b58) - 1)];
}

/* Render `truth` (null-terminated) into `out` (cap bytes), scrambling
 * unsettled chars from the base58 alphabet. `phase_ms` is ms since the
 * row's animation t0. `lock` is a bitmask (positions 0..31) of char
 * indices that should NEVER scramble (e.g. the literal "M-" prefix on
 * a cargo callsign). Char i settles at i*HUD_SCRAMBLE_STAGGER_MS;
 * after HUD_SCRAMBLE_TOTAL_MS the whole string is the truth. */
static void hud_scramble_into(char *out, size_t cap,
                              const char *truth,
                              float phase_ms, uint32_t lock_mask, uint32_t seed) {
    if (cap == 0) return;
    size_t L = strlen(truth);
    if (L >= cap) L = cap - 1;
    for (size_t i = 0; i < L; i++) {
        float settle = (float)i * HUD_SCRAMBLE_STAGGER_MS;
        bool locked = (i < 32) && ((lock_mask >> i) & 1u);
        if (locked || phase_ms >= settle) {
            out[i] = truth[i];
            continue;
        }
        /* Mid-flight: emit a random glyph. The rng seed mixes (row,
         * char_index, phase) so successive frames cycle through
         * different glyphs, but a given (row, char, frame) is
         * deterministic — no per-frame RAND state. */
        uint32_t rng = seed ^ ((uint32_t)i * 0x9E3779B1u)
                            ^ (uint32_t)(phase_ms * 1.7f);
        out[i] = hud_random_glyph(&rng);
    }
    out[L] = '\0';
}

/* Cargo callsigns have a literal class prefix that should never
 * scramble — only the hash body should animate.
 * Returns the lock mask covering the prefix chars. */
static uint32_t hud_cargo_prefix_lock(const char *cargo) {
    if (!cargo) return 0;
    size_t L = strlen(cargo);
    /* Patterns: "RATi-XYZ" (5 prefix chars), "M-ABCDEF" (2 prefix chars),
     * "ABCDEFG" (no prefix). */
    if (L >= 5 && cargo[0]=='R' && cargo[1]=='A' && cargo[2]=='T'
        && cargo[3]=='i' && cargo[4]=='-')
        return 0x1Fu;
    if (L >= 2 && cargo[1] == '-')
        return 0x3u;
    return 0;
}

/* hud_row_signature lives in inspect_anim.c so signal_test can link
 * it without sokol. See inspect_anim.h. */

/* Find a station by pubkey; returns the human name if the pubkey
 * matches a live station, else writes the 7-char hash short label.
 * out cap must be >= 13 to hold the longest station name + null. */
static void hud_station_name_for_pubkey(const uint8_t pub[32],
                                        char *out, size_t cap) {
    if (cap == 0) return;
    if (!hash32_is_zero(pub)) {
        for (int i = 0; i < MAX_STATIONS; i++) {
            const station_t *st = &g.world.stations[i];
            if (!station_exists(st)) continue;
            if (memcmp(st->station_pubkey, pub, 32) == 0) {
                snprintf(out, cap, "%s", st->name);
                return;
            }
        }
    }
    char tmp[8];
    hud_hash_short_label(pub, tmp);
    snprintf(out, cap, "%s", tmp);
}

static const contract_t *hud_tracked_tractor_contract(void) {
    if (g.tracked_contract < 0 || g.tracked_contract >= MAX_CONTRACTS)
        return NULL;
    if (!(g.player_known_contract_mask & (1u << g.tracked_contract)))
        return NULL; /* dropped from gossip memory */
    const contract_t *contract = &g.world.contracts[g.tracked_contract];
    if (!contract->active || contract->action != CONTRACT_TRACTOR)
        return NULL;
    return contract;
}

static void hud_draw_inspect_snapshot_pane(float screen_w, float screen_h) {
    if (g.inspect_snapshot_timer <= 0.0f) return;
    if (LOCAL_PLAYER.docked) return;
    if (g.death_cinematic.active) return;
    /* The timer alone gates visibility — the panel lingers for a few
     * seconds after scan release so the player can read what they
     * just locked onto. */
    const NetInspectSnapshot *snap = &g.inspect_snapshot;
    bool target_is_npc = snap->target_type == INSPECT_TARGET_NPC;
    bool target_is_player = snap->target_type == INSPECT_TARGET_PLAYER;
    if (!target_is_npc && !target_is_player) return;
    int target_idx = (snap->target_index == 0xFFu) ? -1 : (int)snap->target_index;
    if (target_is_npc) {
        if (target_idx < 0 || target_idx >= MAX_NPC_SHIPS) return;
        if (!g.world.npc_ships[target_idx].active) return;
    } else {
        if (target_idx < 0 || target_idx >= MAX_PLAYERS) return;
        const NetPlayerState *np = hud_net_player_state(target_idx);
        if (!np && !g.world.players[target_idx].connected) return;
    }

    float px = fmaxf(16.0f, screen_w - 360.0f);
    float py = (screen_h < 520.0f) ? 76.0f : 92.0f;
    float cell = 8.0f;
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);

    const contract_t *tracked_contract = hud_tracked_tractor_contract();
    int max_rows = (screen_h < 520.0f) ? 5 : 8;
    if (tracked_contract && max_rows > 1) max_rows--;
    int rows = snap->row_count;
    if (rows > max_rows) rows = max_rows;
    unsigned visible_units = 0;
    for (int i = 0; i < rows; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        visible_units += row->quantity > 0 ? row->quantity : 1;
    }
    bool has_more_units = snap->manifest_count > visible_units;
    {
        float bg_x = fmaxf(8.0f, px - 12.0f);
        float bg_y = fmaxf(8.0f, py - 10.0f);
        float bg_w = fmaxf(340.0f, screen_w - bg_x - 16.0f);
        float row_stride = tracked_contract ? 40.0f : 28.0f;
        float bg_h = (snap->manifest_count == 0)
            ? 72.0f
            : 58.0f + (float)rows * row_stride + (has_more_units ? 22.0f : 0.0f);
        bg_h = fminf(bg_h, screen_h - bg_y - 16.0f);
        hud_draw_alpha_rect(bg_x, bg_y, bg_w, bg_h, 0.02f, 0.015f, 0.025f, 0.58f);
        hud_draw_alpha_rect(bg_x, bg_y, 3.0f, bg_h, 0.18f, 0.55f, 0.75f, 0.34f);
    }

    sdtx_pos(px / cell, py / cell);
    sdtx_color3b(PAL_ORE_AMBER);
    if (target_is_npc) {
        sdtx_printf("[ %s CHAIN %02d ]", hud_npc_role_label(snap->role), target_idx);
    } else {
        char pilot[16];
        hud_player_scan_label(target_idx, pilot, sizeof(pilot));
        sdtx_printf("[ PILOT %s ]", pilot);
    }

    sdtx_pos(px / cell, (py + 14.0f) / cell);
    sdtx_color3b(PAL_INSPECT_STATION);
    const char *home = (snap->home_station < MAX_STATIONS)
        ? g.world.stations[snap->home_station].name : "?";
    const char *dest = (snap->dest_station < MAX_STATIONS)
        ? g.world.stations[snap->dest_station].name : "?";
    if (target_is_npc) {
        sdtx_printf("%s  %.12s > %.12s",
                    hud_npc_state_label(snap->state), home, dest);
    } else {
        sdtx_printf("%s  hull %u  near %.12s",
                    hud_hull_class_label(snap->role),
                    (unsigned)snap->state, dest);
    }

    sdtx_pos(px / cell, (py + 28.0f) / cell);
    sdtx_color3b(PAL_TEXT_GREY);
    sdtx_printf("manifest %u unit%s",
                (unsigned)snap->manifest_count,
                snap->manifest_count == 1 ? "" : "s");

    if (snap->manifest_count == 0) {
        sdtx_pos(px / cell, (py + 44.0f) / cell);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_puts(target_is_player ? "no cargo aboard" : "no cargo in custody");
        return;
    }

    float next_y = py + 48.0f;
    float now = g.world.time;
    for (int i = 0; i < rows; i++) {
        const NetInspectSnapshotRow *row = &snap->rows[i];
        char cargo[12];
        hud_cargo_label(row->cargo_pub, cargo);
        unsigned qty = row->quantity > 0 ? row->quantity : 1;
        bool grouped = (row->flags & INSPECT_ROW_GROUPED) != 0;

        /* On grouped rows, chain_len is repurposed as the prefix_class
         * of the bucket (0 = ANONYMOUS bulk, otherwise a named class). */
        const char *prefix_label = NULL;
        if (grouped) {
            switch ((ingot_prefix_t)row->chain_len) {
            case INGOT_PREFIX_M:            prefix_label = "M class"; break;
            case INGOT_PREFIX_H:            prefix_label = "H class"; break;
            case INGOT_PREFIX_T:            prefix_label = "T class"; break;
            case INGOT_PREFIX_S:            prefix_label = "S class"; break;
            case INGOT_PREFIX_F:            prefix_label = "F class"; break;
            case INGOT_PREFIX_K:            prefix_label = "K class"; break;
            case INGOT_PREFIX_RATI:         prefix_label = "RATi class"; break;
            case INGOT_PREFIX_COMMISSIONED: prefix_label = "RATi*"; break;
            case INGOT_PREFIX_ANONYMOUS:
            default:                        prefix_label = "bulk"; break;
            }
        }

        /* Per-row scramble state: re-trigger the animation only when the
         * row's content fingerprint actually changed. Same content across
         * frames = no re-scramble. */
        uint64_t sig = hud_row_signature(row);
        if (g.inspect_row_anim[i].sig != sig) {
            g.inspect_row_anim[i].sig = sig;
            g.inspect_row_anim[i].anim_t0 = now;
            g.inspect_row_anim[i].phase = 0;
            g.inspect_row_anim[i].phase_t0 = now;
        }
        float row_phase_ms = (now - g.inspect_row_anim[i].anim_t0) * 1000.0f;
        float settle_norm = row_phase_ms / HUD_SCRAMBLE_TOTAL_MS;
        if (settle_norm < 0.0f) settle_norm = 0.0f;
        if (settle_norm > 1.0f) settle_norm = 1.0f;
        /* Brightness pulses during settle: dimmer at the start, full at
         * settle, then a touch under so settled rows don't burn too hot. */
        float row_alpha = 0.55f + 0.45f * settle_norm;
        uint8_t a8_label = (uint8_t)(235.0f * row_alpha);
        uint8_t a8_chain = (uint8_t)(200.0f * row_alpha);

        uint8_t rr, gg, bb;
        mining_grade_rgb((mining_grade_t)row->grade, &rr, &gg, &bb);
        float y = next_y;

        /* The label proper: rarity short + commodity code + cargo/prefix
         * + quantity. The cargo callsign body scrambles but the class
         * prefix ("M-", "RATi-") and the ingot-class words ("M class")
         * stay fixed — those aren't hashes, those are stable bucket
         * labels. */
        char cargo_disp[12];
        if (prefix_label) {
            snprintf(cargo_disp, sizeof(cargo_disp), "%s", prefix_label);
        } else {
            uint32_t lock = hud_cargo_prefix_lock(cargo);
            uint32_t seed = (uint32_t)(sig & 0xffffffffu) ^ 0xC0FFEEu;
            hud_scramble_into(cargo_disp, sizeof(cargo_disp), cargo,
                              row_phase_ms, lock, seed);
        }
        sdtx_pos(px / cell, y / cell);
        sdtx_color4b(rr, gg, bb, a8_label);
        sdtx_printf("%-5s %s %-10s x%u",
                    hud_grade_short_label(row->grade),
                    commodity_code((commodity_t)row->commodity),
                    cargo_disp,
                    qty);

        next_y = y + 14.0f;
        bool drew_contract_fit = false;
        if (tracked_contract) {
            bool has_proof = !grouped &&
                (((row->flags & INSPECT_ROW_HAS_RECEIPT) != 0) ||
                 contract_fit_has_bytes(row->cargo_pub));
            contract_fit_reason_t fit = contract_fit_cargo_fields(
                tracked_contract,
                (commodity_t)row->commodity,
                (mining_grade_t)row->grade,
                (uint16_t)qty,
                has_proof);
            bool relevant = contract_fit_is_ok(fit) ||
                            row->commodity == (uint8_t)tracked_contract->commodity;
            if (relevant) {
                sdtx_pos(px / cell, (y + 12.0f) / cell);
                if (contract_fit_is_ok(fit) && has_proof) {
                    sdtx_color4b(PAL_CONTRACT_READY, a8_chain);
                    sdtx_puts("contract: match");
                } else if (contract_fit_is_ok(fit)) {
                    sdtx_color4b(PAL_CONTRACT_HINT, a8_chain);
                    sdtx_puts("contract: match / proof missing");
                } else {
                    sdtx_color4b(PAL_TEXT_GREY, a8_chain);
                    sdtx_printf("contract: %s",
                                contract_fit_reason_label(fit));
                }
                next_y = y + 26.0f;
                drew_contract_fit = true;
            }
        }
        if (!grouped && (row->flags & INSPECT_ROW_HAS_RECEIPT)) {
            char head[8], origin[8], latest[8];
            hud_hash_short_label(row->receipt_head, head);
            hud_hash_short_label(row->origin_station, origin);
            hud_hash_short_label(row->latest_station, latest);
            /* Phase rotation for chained rows. Singletons (chain_len==1)
             * pin to phase 0 — origin and latest are the same so phases
             * B/C add nothing. Each phase fires a fresh per-phase
             * scramble on the bits that actually changed line-to-line. */
            const float PHASE_DUR = 1.2f;
            uint8_t target_phase = 0;
            if (row->chain_len > 1) {
                int slot = (int)((now - g.inspect_row_anim[i].anim_t0) / PHASE_DUR) % 3;
                if (slot < 0) slot = 0;
                target_phase = (uint8_t)slot;
            }
            if (target_phase != g.inspect_row_anim[i].phase) {
                g.inspect_row_anim[i].phase = target_phase;
                g.inspect_row_anim[i].phase_t0 = now;
            }
            float phase_ms = (now - g.inspect_row_anim[i].phase_t0) * 1000.0f;

            float chain_y = drew_contract_fit ? (y + 24.0f) : (y + 12.0f);
            sdtx_pos(px / cell, chain_y / cell);
            sdtx_color4b(PAL_TEXT_GREY, a8_chain);
            uint32_t seed = (uint32_t)(sig & 0xffffffffu);
            if (target_phase == 0) {
                /* Phase A — origin>latest + head + ev. The body of each
                 * 7-char hash scrambles independently. */
                char a_origin[8], a_latest[8], a_head[8];
                hud_scramble_into(a_origin, sizeof(a_origin), origin,
                                  phase_ms,0, seed ^ 1u);
                hud_scramble_into(a_latest, sizeof(a_latest), latest,
                                  phase_ms,0, seed ^ 2u);
                hud_scramble_into(a_head, sizeof(a_head), head,
                                  phase_ms,0, seed ^ 3u);
                sdtx_printf("chain %u  %s>%s  head %s  ev %llu",
                            (unsigned)row->chain_len, a_origin, a_latest,
                            a_head, (unsigned long long)row->event_id);
            } else if (target_phase == 1) {
                /* Phase B — origin: <station name or hash>. */
                char name[24];
                hud_station_name_for_pubkey(row->origin_station, name, sizeof(name));
                char disp[24];
                hud_scramble_into(disp, sizeof(disp), name,
                                  phase_ms,0, seed ^ 4u);
                sdtx_printf("chain %u  origin: %s",
                            (unsigned)row->chain_len, disp);
            } else {
                /* Phase C — latest: <station name or hash>. */
                char name[24];
                hud_station_name_for_pubkey(row->latest_station, name, sizeof(name));
                char disp[24];
                hud_scramble_into(disp, sizeof(disp), name,
                                  phase_ms,0, seed ^ 5u);
                sdtx_printf("chain %u  latest: %s",
                            (unsigned)row->chain_len, disp);
            }
            next_y = chain_y + 14.0f;
        } else if (!grouped) {
            float chain_y = drew_contract_fit ? (y + 24.0f) : (y + 12.0f);
            sdtx_pos(px / cell, chain_y / cell);
            sdtx_color4b(PAL_TEXT_GREY, a8_chain);
            if (hash32_is_zero(row->cargo_pub)) {
                sdtx_puts("identity missing");
            } else {
                sdtx_puts("chain: station origin / receipt pending");
            }
            next_y = chain_y + 14.0f;
        }
    }
    /* Clear stale anim slots beyond the current row count so an
     * eventually-shorter snapshot doesn't carry old signatures forward
     * (would otherwise look like a fresh re-trigger when the same
     * NPC's row count grew back). */
    for (int i = rows; i < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        g.inspect_row_anim[i].sig = 0;
    }

    if (snap->manifest_count > visible_units) {
        sdtx_pos(px / cell, next_y / cell);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_printf("+%u more unit%s",
                    (unsigned)(snap->manifest_count - visible_units),
                    (snap->manifest_count - visible_units) == 1 ? "" : "s");
    }
}

static void hud_draw_signal_lost_warning(float screen_w, float screen_h, float sig_quality) {
    if (LOCAL_PLAYER.docked) return;
    if (sig_quality >= 0.01f) return;
    if (g.death_screen_timer > 0.0f || g.death_cinematic.active) return;
    float blink = sinf(g.world.time * 3.0f);
    if (blink <= 0.0f) return;

    float cell = 8.0f;
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    uint8_t ba = (uint8_t)(blink * 200.0f);
    sdtx_color4b(255, 70, 50, ba);
    sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.40f) / cell, cell,
                       "[ SIGNAL LOST ]");
}

/* Render the post-classify shared panels common to compact + wide.
 * Called once per frame after each layout's top-status section. */
/* Directional hit indicator removed — the rendered triangle didn't
 * read well in practice. Symbol kept (no-op) so the call site doesn't
 * need surgery, and damage_dir_{x,y,timer} keep updating in case a
 * future replacement (audio panning? a 3D-style "incoming" callout?)
 * wants to consume them. The kill-feed + popup + screen shake + audio
 * already cover the "you got hit" telemetry. */
static void hud_draw_hit_indicator(float screen_w, float screen_h) {
    (void)screen_w;
    (void)screen_h;
}

/* PvP kill-feed — single line at top-center, fades on the timer.
 * Server-emitted SIM_EVENT_NPC_KILL drives the text; eventually
 * remote-player kills will share the same surface. */
static void hud_draw_kill_feed(float screen_w, float screen_h) {
    if (g.kill_feed_timer <= 0.0f) return;
    if (g.kill_feed_text[0] == '\0') return;
    float cell = 8.0f;
    /* Fade-in for the first 0.2 s, fade-out for the last 0.5 s. */
    float a = 1.0f;
    if (g.kill_feed_timer > 2.8f) a = (3.0f - g.kill_feed_timer) / 0.2f;
    else if (g.kill_feed_timer < 0.5f) a = g.kill_feed_timer / 0.5f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    uint8_t a8 = (uint8_t)(a * 255.0f);
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color4b(255, 220, 100, a8);  /* warm gold reads as scoreboard */
    sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.08f) / cell, cell,
                       g.kill_feed_text);
    (void)screen_h;
}

/* Killer-side confirm banner — sits one line below the kill feed so
 * both can be visible if a kill confirm and a separate feed event
 * happen close together. Brighter / hotter color to read as an
 * achievement notification rather than a scoreboard entry. */
static void hud_draw_kill_confirm(float screen_w, float screen_h) {
    if (g.kill_confirm_timer <= 0.0f) return;
    if (g.kill_confirm_text[0] == '\0') return;
    float cell = 8.0f;
    float a = 1.0f;
    if (g.kill_confirm_timer > 2.8f) a = (3.0f - g.kill_confirm_timer) / 0.2f;
    else if (g.kill_confirm_timer < 0.5f) a = g.kill_confirm_timer / 0.5f;
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    uint8_t a8 = (uint8_t)(a * 255.0f);
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    sdtx_color4b(255, 90, 60, a8);
    sdtx_centered_text(screen_w * 0.5f, (screen_h * 0.08f + 18.0f) / cell, cell,
                       g.kill_confirm_text);
}

/* Session kill counter, top-right under the version line. Compact
 * "K 3" so it fits beside the v<hash>/connection status without
 * crowding the alpha banner above. */
static void hud_draw_kill_counter(float screen_w) {
    if (g.kill_count_session <= 0) return;
    float x = ui_text_pos(fmaxf(8.0f, screen_w - 60.0f));
    float y = ui_text_pos(22.0f);
    sdtx_pos(x, y);
    sdtx_color3b(255, 200, 80);
    sdtx_printf("K %d", g.kill_count_session);
}

/* Session scoreboard, toggled with [Tab] while undocked. Kills/deaths
 * are aggregated client-side from observed SIM_EVENT_DEATH /
 * SIM_EVENT_NPC_KILL events. Sorted by kills desc; ties broken by
 * fewer deaths. Single-pass insertion sort -- the row pool is 16. */
static void hud_draw_scoreboard(float screen_w, float screen_h) {
    if (!g.scoreboard.show) return;
    float cell = 8.0f;
    int order[16];
    int n = g.scoreboard.row_count;
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i];
        int j = i - 1;
        while (j >= 0) {
            int a = order[j];
            bool worse =
                (g.scoreboard.rows[a].kills < g.scoreboard.rows[k].kills) ||
                (g.scoreboard.rows[a].kills == g.scoreboard.rows[k].kills &&
                 g.scoreboard.rows[a].deaths > g.scoreboard.rows[k].deaths);
            if (!worse) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = k;
    }

    float panel_w = 280.0f;
    float panel_x = (screen_w - panel_w) * 0.5f;
    float panel_y = screen_h * 0.18f;
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);

    sdtx_color3b(255, 220, 100);
    sdtx_pos(panel_x / cell, panel_y / cell);
    sdtx_puts("== SCOREBOARD ==  [Tab to close]");

    sdtx_color3b(180, 180, 180);
    sdtx_pos(panel_x / cell, (panel_y + 16.0f) / cell);
    sdtx_puts("PILOT             K   D   K/D");

    if (n == 0) {
        sdtx_color3b(140, 140, 140);
        sdtx_pos(panel_x / cell, (panel_y + 32.0f) / cell);
        sdtx_puts("(no kills or deaths yet)");
        return;
    }
    for (int rank = 0; rank < n; rank++) {
        int idx = order[rank];
        const char *label = g.scoreboard.rows[idx].label[0]
                          ? g.scoreboard.rows[idx].label : "????";
        bool is_me = memcmp(g.scoreboard.rows[idx].token,
                            g.world.players[g.local_player_slot].session_token, 8) == 0;
        if (is_me) sdtx_color3b(255, 220, 100);
        else       sdtx_color3b(200, 200, 200);
        char kdr[8];
        if (g.scoreboard.rows[idx].deaths == 0) {
            snprintf(kdr, sizeof(kdr), "%d.0", g.scoreboard.rows[idx].kills);
        } else {
            float r = (float)g.scoreboard.rows[idx].kills /
                      (float)g.scoreboard.rows[idx].deaths;
            snprintf(kdr, sizeof(kdr), "%.2f", r);
        }
        sdtx_pos(panel_x / cell, (panel_y + 32.0f + (float)rank * 12.0f) / cell);
        sdtx_printf("%-16s %3d %3d %5s",
                    label,
                    g.scoreboard.rows[idx].kills,
                    g.scoreboard.rows[idx].deaths,
                    kdr);
    }
}

static void hud_draw_shared_panels(float screen_w, float screen_h, float sig_quality, bool compact) {
    hud_draw_alpha_banner_and_mp_indicator(screen_w, compact);
    hud_draw_nav_label(screen_w, screen_h);
    hud_draw_hail_sigil(screen_w, screen_h);
    hud_draw_module_inspect_pane(screen_w);
    hud_draw_inspect_snapshot_pane(screen_w, screen_h);
    hud_draw_signal_lost_warning(screen_w, screen_h, sig_quality);
    hud_draw_hit_indicator(screen_w, screen_h);
    hud_draw_kill_feed(screen_w, screen_h);
    hud_draw_kill_confirm(screen_w, screen_h);
    hud_draw_kill_counter(screen_w);
    hud_draw_scoreboard(screen_w, screen_h);
    /* Hit feedback vignette — drawn last so it sits above the HUD
     * readouts, but the inset rectangle in the middle leaves the
     * action row + flight readouts unobscured. */
    draw_damage_flash(screen_w, screen_h);
}

/* Pending credits, broken out per station. Reports the station holding
 * the largest player-owned balance plus how many *other* stations also
 * have a positive balance, so the HUD can name the strongest station
 * and surface multi-ledger spread without flattening it into one
 * generic number. Singleplayer only: in MP the server doesn't push a
 * per-station ledger and this returns false. Cheap O(stations × ledger)
 * walk; reused per-frame by both HUD variants. */
static bool client_pending_summary(int *strongest_idx,
                                   float *strongest_balance,
                                   int *other_count) {
    if (strongest_idx) *strongest_idx = -1;
    if (strongest_balance) *strongest_balance = 0.0f;
    if (other_count) *other_count = 0;
    if (!g.local_server.active) return false;
    uint8_t pseudo[32];
    client_session_pseudo_pubkey(LOCAL_PLAYER.session_token, pseudo);
    int best_idx = -1;
    float best_bal = 0.0f;
    int positives = 0;
    for (int si = 0; si < MAX_STATIONS; si++) {
        const station_t *st = &g.world.stations[si];
        float bal = 0.0f;
        for (int li = 0; li < st->ledger_count; li++) {
            if (memcmp(st->ledger[li].player_pubkey,
                       pseudo, 32) == 0) {
                bal += st->ledger[li].balance;
            }
        }
        if (bal > 0.5f) {
            positives++;
            if (bal > best_bal) {
                best_bal = bal;
                best_idx = si;
            }
        }
    }
    if (best_idx < 0) return false;
    if (strongest_idx) *strongest_idx = best_idx;
    if (strongest_balance) *strongest_balance = best_bal;
    if (other_count) *other_count = positives - 1;
    return true;
}

/* Currency label at the player's current (docked or nearby) station.
 * Falls back to "cr" when the station hasn't set a currency_name. */
static const char *player_current_currency(void) {
    int st = LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station;
    if (st < 0 || st >= MAX_STATIONS) return "cr";
    const char *cn = g.world.stations[st].currency_name;
    return (cn && cn[0]) ? cn : "cr";
}

/* ------------------------------------------------------------------ */
/* UI scaling / layout helpers                                         */
/* ------------------------------------------------------------------ */

static const float UI_SCALE_TIGHT   = 1.45f;
static const float UI_SCALE_COMPACT = 1.60f;
static const float UI_SCALE_DEFAULT = 1.42f;
static const float UI_SCALE_WIDE    = 1.28f;

/* ui_safe_positive is shared via client.h so render_world (and any
 * other direct sapp_* consumer) can guard against the same NaN frame. */

float ui_window_width(void) {
    float px  = ui_safe_positive(sapp_widthf(), 1280.0f);
    float dpi = ui_safe_positive(sapp_dpi_scale(), 1.0f);
    return px / fmaxf(1.0f, dpi);
}

float ui_window_height(void) {
    float px  = ui_safe_positive(sapp_heightf(), 720.0f);
    float dpi = ui_safe_positive(sapp_dpi_scale(), 1.0f);
    return px / fmaxf(1.0f, dpi);
}

float ui_scale(void) {
    float width = ui_window_width();
    float height = ui_window_height();
    if ((width < 900.0f) || (height < 620.0f)) {
        return UI_SCALE_TIGHT;
    }
    if ((width < 1280.0f) || (height < 780.0f)) {
        return UI_SCALE_COMPACT;
    }
    if ((width > 1800.0f) && (height > 980.0f)) {
        return UI_SCALE_WIDE;
    }
    return UI_SCALE_DEFAULT;
}

float ui_screen_width(void) {
    return ui_window_width() / ui_scale();
}

float ui_screen_height(void) {
    return ui_window_height() / ui_scale();
}

bool ui_is_compact(void) {
    return (ui_window_width() < 1200.0f) || (ui_window_height() < 760.0f);
}

float ui_text_zoom(void) {
    return 1.0f;
}

float ui_text_pos(float pixel_value) {
    /* Snap to the debugtext cell grid so scaled layouts don't self-overlap. */
    return roundf(pixel_value / (HUD_CELL * ui_text_zoom()));
}

/* ------------------------------------------------------------------ */
/* UI drawing primitives                                               */
/* ------------------------------------------------------------------ */

void draw_ui_scanlines(float x, float y, float width, float height, float spacing, float alpha) {
    begin_line_batch();
    for (float scan_y = y + 10.0f; scan_y < (y + height - 10.0f); scan_y += spacing) {
        draw_segment_batched(v2(x + 10.0f, scan_y), v2(x + width - 10.0f, scan_y), 0.08f, 0.14f, 0.20f, alpha);
    }
    end_line_batch();
}

void draw_ui_corner_brackets(float x, float y, float width, float height, float r, float g0, float b, float alpha) {
    float arm = fminf(26.0f, fminf(width, height) * 0.16f);
    float inset = 3.0f;
    begin_line_batch();
    draw_segment_batched(v2(x + inset, y + arm), v2(x + inset, y + inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + inset, y + inset), v2(x + arm, y + inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - arm, y + inset), v2(x + width - inset, y + inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - inset, y + inset), v2(x + width - inset, y + arm), r, g0, b, alpha);
    draw_segment_batched(v2(x + inset, y + height - arm), v2(x + inset, y + height - inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + inset, y + height - inset), v2(x + arm, y + height - inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - arm, y + height - inset), v2(x + width - inset, y + height - inset), r, g0, b, alpha);
    draw_segment_batched(v2(x + width - inset, y + height - inset), v2(x + width - inset, y + height - arm), r, g0, b, alpha);
    end_line_batch();
}

void draw_ui_rule(float x0, float x1, float y, float r, float g0, float b, float alpha) {
    draw_segment(v2(x0, y), v2(x1, y), r, g0, b, alpha);
}

void draw_ui_panel(float x, float y, float width, float height, float accent) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    float accent_r = 0.26f + (accent * 0.28f);
    float accent_g = 0.72f + (accent * 0.20f);
    float accent_b = 0.98f;

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.015f, 0.028f, 0.05f, 0.94f);
    draw_rect_centered(center, (width * 0.5f) - 2.0f, (height * 0.5f) - 2.0f, 0.018f, 0.044f, 0.072f, 0.86f);
    draw_ui_scanlines(x, y, width, height, 8.0f, 0.06f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.09f, 0.18f, 0.28f, 0.34f);
    draw_ui_corner_brackets(x, y, width, height, accent_r, accent_g, accent_b, 0.82f);
    draw_ui_rule(x + 14.0f, x + fminf(116.0f, width * 0.28f), y + 14.0f, accent_r, accent_g, accent_b, 0.82f);
    draw_ui_rule(x + width - fminf(56.0f, width * 0.18f), x + width - 14.0f, y + 14.0f, 0.18f, 0.28f, 0.38f, 0.55f);
}

void draw_ui_panel_colored(float x, float y, float width, float height, float ar, float ag, float ab) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.015f, 0.028f, 0.05f, 0.94f);
    draw_rect_centered(center, (width * 0.5f) - 2.0f, (height * 0.5f) - 2.0f, 0.018f, 0.044f, 0.072f, 0.86f);
    draw_ui_scanlines(x, y, width, height, 8.0f, 0.06f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.09f, 0.18f, 0.28f, 0.34f);
    draw_ui_corner_brackets(x, y, width, height, ar, ag, ab, 0.82f);
    draw_ui_rule(x + 14.0f, x + fminf(116.0f, width * 0.28f), y + 14.0f, ar, ag, ab, 0.82f);
    draw_ui_rule(x + width - fminf(56.0f, width * 0.18f), x + width - 14.0f, y + 14.0f, 0.18f, 0.28f, 0.38f, 0.55f);
}

void get_station_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    bool cramped = compact && (screen_w < 280.0f || screen_h < 300.0f);
    float hud_margin = compact ? (cramped ? 10.0f : 14.0f) : HUD_MARGIN;
    float top_reserved = compact ? (cramped ? 18.0f : 38.0f)
                                 : (HUD_TOP_PANEL_HEIGHT + 16.0f);
    float bottom_reserved = compact ? (cramped ? 8.0f : 14.0f)
                                    : (HUD_BOTTOM_PANEL_HEIGHT + 14.0f);
    float panel_width = fminf(compact ? STATION_PANEL_COMPACT_WIDTH : STATION_PANEL_WIDTH,
                              screen_w - (hud_margin * 2.0f));
    float available_h = screen_h - (hud_margin * 2.0f) -
                        top_reserved - bottom_reserved;
    if (available_h < 96.0f)
        available_h = fmaxf(64.0f, screen_h - (hud_margin * 2.0f));
    float panel_height = fminf(compact ? STATION_PANEL_COMPACT_HEIGHT : STATION_PANEL_HEIGHT,
                               available_h);
    float panel_x = (screen_w - panel_width) * 0.5f;
    float min_y = hud_margin + top_reserved;
    float max_y = screen_h - hud_margin - bottom_reserved - panel_height;
    float panel_y = clampf((screen_h - panel_height) * 0.5f, min_y, fmaxf(min_y, max_y));
    if (panel_y + panel_height > screen_h - hud_margin)
        panel_y = fmaxf(hud_margin, screen_h - hud_margin - panel_height);

    *x = panel_x;
    *y = panel_y;
    *width = panel_width;
    *height = panel_height;
}

void draw_ui_scrim(float alpha) {
    draw_rect_centered(v2(ui_screen_width() * 0.5f, ui_screen_height() * 0.5f), ui_screen_width() * 0.5f, ui_screen_height() * 0.5f, 0.01f, 0.03f, 0.06f, alpha);
}

void draw_ui_meter(float x, float y, float width, float height, float fill, float r, float g0, float b) {
    float clamped_fill = clampf(fill, 0.0f, 1.0f);
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.03f, 0.06f, 0.10f, 0.98f);
    draw_ui_scanlines(x, y, width, height, 4.0f, 0.05f);
    if (clamped_fill > 0.0f) {
        float fill_width = width * clamped_fill;
        draw_rect_centered(v2(x + (fill_width * 0.5f), y + (height * 0.5f)), fill_width * 0.5f, height * 0.5f, r, g0, b, 0.92f);
        draw_ui_rule(x + 2.0f, x + fill_width - 2.0f, y + 2.0f, fminf(1.0f, r + 0.18f), fminf(1.0f, g0 + 0.18f), fminf(1.0f, b + 0.18f), 0.72f);
    }
    draw_ui_corner_brackets(x, y, width, height, 0.24f, 0.48f, 0.62f, 0.70f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.12f, 0.22f, 0.32f, 0.48f);
}

/* ------------------------------------------------------------------ */
/* HUD layout rects                                                    */
/* ------------------------------------------------------------------ */

void get_flight_hud_rects(float* top_x, float* top_y, float* top_w, float* top_h,
    float* bottom_x, float* bottom_y, float* bottom_w, float* bottom_h) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float top_width = fminf(compact ? HUD_TOP_PANEL_COMPACT_WIDTH : HUD_TOP_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float top_height = compact ? HUD_TOP_PANEL_COMPACT_HEIGHT : HUD_TOP_PANEL_HEIGHT;
    float bottom_width = fminf(compact ? HUD_BOTTOM_PANEL_COMPACT_WIDTH : HUD_BOTTOM_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float bottom_height = compact ? 28.0f : HUD_BOTTOM_PANEL_HEIGHT;

    *top_x = hud_margin;
    *top_y = hud_margin;
    *top_w = top_width;
    *top_h = top_height;
    *bottom_x = hud_margin;
    *bottom_y = screen_h - hud_margin - bottom_height;
    *bottom_w = bottom_width;
    *bottom_h = bottom_height;
}

bool hud_should_draw_message_panel(void) {
    if (episode_is_active(&g.episode)) return false;
    return !LOCAL_PLAYER.docked || !g.onboarding.complete || !g.onboarding.welcomed ||
           (g.notice_timer > 0.0f) || (g.collection_feedback_timer > 0.0f);
}

void get_hud_message_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = ui_screen_width();
    bool compact = ui_is_compact();
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float panel_w = compact ? HUD_MESSAGE_PANEL_COMPACT_WIDTH : HUD_MESSAGE_PANEL_WIDTH;
    float panel_h = compact ? HUD_MESSAGE_PANEL_COMPACT_HEIGHT : HUD_MESSAGE_PANEL_HEIGHT;
    float gap = compact ? 8.0f : 12.0f;

    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);
    panel_w = fminf(panel_w, screen_w - (hud_margin * 2.0f));

    *x = screen_w - hud_margin - panel_w;
    *y = bottom_y - gap - panel_h;
    *width = panel_w;
    *height = panel_h;
}

/* ------------------------------------------------------------------ */
/* Message line splitting / building                                   */
/* ------------------------------------------------------------------ */

/* Word-wrap `text` into up to `max_lines` output slots of size
 * `line_cap` each. Each line is at most `max_cols` characters; line
 * breaks are preferred at spaces. If the text doesn't fit in the
 * available slots, the last slot is truncated with "...". Slots past
 * what the text needs are left as empty strings. */
static void wrap_hud_message_lines(const char *text, int max_cols,
                                   char (*lines)[96], int line_cap,
                                   int max_lines) {
    for (int i = 0; i < max_lines; i++) lines[i][0] = '\0';
    if (!text || !text[0] || max_lines <= 0) return;
    if (max_cols < 8) max_cols = 8;

    int li = 0;
    const char *cursor = text;
    while (*cursor && li < max_lines) {
        size_t remaining = strlen(cursor);
        if ((int)remaining <= max_cols) {
            snprintf(lines[li], line_cap, "%.*s",
                     (int)(line_cap - 1), cursor);
            li++;
            break;
        }
        int split = max_cols;
        while (split > (max_cols / 2) && cursor[split] != ' ') split--;
        if (split <= max_cols / 2) split = max_cols; /* no space → hard cut */
        /* Last slot and still more text after the split — append "..." */
        if (li == max_lines - 1) {
            int tail = split - 3 < 0 ? 0 : split - 3;
            snprintf(lines[li], line_cap, "%.*s...", tail, cursor);
            li++;
            break;
        }
        snprintf(lines[li], line_cap, "%.*s", split, cursor);
        li++;
        cursor += split;
        while (*cursor == ' ') cursor++;
    }
}

typedef struct {
    int station_idx;
    station_construction_need_t need;
    int held;
} hud_construction_need_t;

typedef struct {
    int station_idx;
    int ring;
    int slot;
    module_type_t type;
} hud_snapping_scaffold_t;

typedef struct {
    int station_idx;
    int blocker_idx;
    module_type_t type;
} hud_shipyard_blocked_t;

typedef struct {
    int station_idx;
} hud_abandoned_plan_t;

static const char *hud_station_short_name(int station_idx) {
    const char *name = station_short_name(station_idx);
    return (name && name[0] != '?') ? name : "station";
}

static int hud_ship_material_count(commodity_t material) {
    if ((unsigned)material >= COMMODITY_COUNT) return 0;
    if (material >= COMMODITY_RAW_ORE_COUNT)
        return manifest_count_by_commodity(&LOCAL_PLAYER.ship.manifest, material);
    return (int)floorf(LOCAL_PLAYER.ship.cargo[material] + 0.001f);
}

static const char *hud_material_source_hint(commodity_t material) {
    switch (material) {
    case COMMODITY_FRAME:
        return "Make frames at a Frame Press, then dock and press [S].";
    case COMMODITY_FERRITE_INGOT:
    case COMMODITY_CUPRITE_INGOT:
    case COMMODITY_CRYSTAL_INGOT:
        return "Smelt matching ore at a Furnace, then dock and press [S].";
    case COMMODITY_LASER_MODULE:
    case COMMODITY_TRACTOR_MODULE:
        return "Fabricate modules at a fab, then dock and press [S].";
    default:
        return "Haul the material here, then dock and press [S].";
    }
}

static bool hud_station_construction_need(const station_t *st, int station_idx,
                                          hud_construction_need_t *out) {
    if (!st || !out) return false;
    station_construction_need_t need;
    if (!station_construction_material_need(st, &need)) return false;
    *out = (hud_construction_need_t){
        .station_idx = station_idx,
        .need = need,
        .held = hud_ship_material_count(need.material),
    };
    return true;
}

static bool hud_find_construction_need(hud_construction_need_t *out) {
    if (!out) return false;
    int focus = -1;
    if (LOCAL_PLAYER.docked) focus = LOCAL_PLAYER.current_station;
    else if (LOCAL_PLAYER.in_dock_range) focus = LOCAL_PLAYER.nearby_station;
    if (focus >= 0 && focus < MAX_STATIONS &&
        hud_station_construction_need(&g.world.stations[focus], focus, out)) {
        return true;
    }

    const float max_range = 1400.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_construction_need_t best = {0};
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        float d2 = v2_dist_sq(st->pos, LOCAL_PLAYER.ship.pos);
        if (d2 >= best_d2) continue;
        hud_construction_need_t candidate;
        if (!hud_station_construction_need(st, s, &candidate)) continue;
        best = candidate;
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}

static bool hud_station_shipyard_blocked(const station_t *st, int station_idx,
                                         hud_shipyard_blocked_t *out) {
    if (!st || !out) return false;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    if (st->pending_scaffold_count <= 0) return false;
    if (!station_has_module(st, MODULE_SHIPYARD)) return false;
    if (station_nascent_scaffold_index(g.world.scaffolds, MAX_SCAFFOLDS,
                                       station_idx) >= 0) {
        return false;
    }
    int blocker = station_construction_blocker_index(st, g.world.scaffolds,
                                                     MAX_SCAFFOLDS);
    if (blocker < 0) return false;

    *out = (hud_shipyard_blocked_t){
        .station_idx = station_idx,
        .blocker_idx = blocker,
        .type = st->pending_scaffolds[0].type,
    };
    return true;
}

static bool hud_find_shipyard_blocked(hud_shipyard_blocked_t *out) {
    if (!out) return false;
    int focus = -1;
    if (LOCAL_PLAYER.docked) focus = LOCAL_PLAYER.current_station;
    else if (LOCAL_PLAYER.in_dock_range) focus = LOCAL_PLAYER.nearby_station;
    if (focus >= 0 && focus < MAX_STATIONS &&
        hud_station_shipyard_blocked(&g.world.stations[focus], focus, out)) {
        return true;
    }

    const float max_range = 1400.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_shipyard_blocked_t best = {0};
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        float d2 = v2_dist_sq(st->pos, LOCAL_PLAYER.ship.pos);
        if (d2 >= best_d2) continue;
        hud_shipyard_blocked_t candidate;
        if (!hud_station_shipyard_blocked(st, s, &candidate)) continue;
        best = candidate;
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}

static bool hud_find_abandoned_plan(hud_abandoned_plan_t *out) {
    if (!out) return false;
    const float max_range = 1800.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_abandoned_plan_t best = {0};
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_planned_site_abandoned(st)) continue;
        float d2 = v2_dist_sq(st->pos, LOCAL_PLAYER.ship.pos);
        if (d2 >= best_d2) continue;
        best.station_idx = s;
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}

static bool hud_find_snapping_scaffold(hud_snapping_scaffold_t *out) {
    if (!out) return false;
    const float max_range = 900.0f;
    float best_d2 = max_range * max_range;
    bool found = false;
    hud_snapping_scaffold_t best = {0};
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &g.world.scaffolds[i];
        if (!sc->active || sc->state != SCAFFOLD_SNAPPING) continue;
        if (sc->placed_station < 0 || sc->placed_station >= MAX_STATIONS) continue;
        float d2 = v2_dist_sq(sc->pos, LOCAL_PLAYER.ship.pos);
        if (d2 >= best_d2) continue;
        best = (hud_snapping_scaffold_t){
            .station_idx = sc->placed_station,
            .ring = sc->placed_ring,
            .slot = sc->placed_slot,
            .type = sc->module_type,
        };
        best_d2 = d2;
        found = true;
    }
    if (found) *out = best;
    return found;
}


static bool build_hud_message(char* label, size_t label_size, char* message, size_t message_size, uint8_t* r, uint8_t* g0, uint8_t* b) {
    /* ================================================================
     * SYSTEM HINT PANEL — bottom-right, one message at a time.
     * Tutorial copy uses the local guide voice; stations never speak
     * here because they use the hail overlay.
     * ================================================================ */

    /* Subtitle-style messages: one clean line, no label brackets.
     * Priority order — only the most important message shows. */

    /* Hull critical */
    if (!LOCAL_PLAYER.docked && g.death_screen_timer <= 0.0f) {
        float max_hull = ship_max_hull(&LOCAL_PLAYER.ship);
        if (max_hull > 0.0f && LOCAL_PLAYER.ship.hull / max_hull < 0.20f) {
            label[0] = '\0';
            snprintf(message, message_size, "Hull failing. Dock for repairs.");
            *r = 255; *g0 = 60; *b = 50;
            return true;
        }
    }

    /* Transient notice */
    if (g.notice_timer > 0.0f) {
        label[0] = '\0';
        snprintf(message, message_size, "%s", g.notice);
        *r = 140; *g0 = 140; *b = 140;
        return true;
    }

    /* Plan mode */
    if (g.plan_mode_active) {
        bool ghost = (g.plan_target_station == -1);
        snprintf(label, label_size, "%s", ghost ? "PLAN GHOST" : "PLAN SLOT");
        if (g.placement_target_ring > 0 && g.placement_target_slot >= 0) {
            if (ghost) {
                snprintf(message, message_size,
                         "%s preview ring %d slot %d. [E] locks an outpost; [R] changes type; [B] exits.",
                         module_type_name((module_type_t)g.plan_type),
                         g.placement_target_ring,
                         g.placement_target_slot);
            } else {
                snprintf(message, message_size,
                         "%s at %s ring %d slot %d. [E] toggles this slot; [R] changes type; [B] exits.",
                         module_type_name((module_type_t)g.plan_type),
                         hud_station_short_name(g.placement_target_station),
                         g.placement_target_ring,
                         g.placement_target_slot);
                if (g.placement_target_station >= 0 &&
                    g.placement_target_station < MAX_STATIONS) {
                    const station_t *st = &g.world.stations[g.placement_target_station];
                    station_plan_flow_hint_t hint;
                    char flow_hint[96];
                    if (station_plan_flow_hint(st, (module_type_t)g.plan_type,
                                               g.placement_target_ring,
                                               g.placement_target_slot,
                                               &hint) &&
                        station_plan_flow_hint_format(&hint, flow_hint,
                                                      sizeof(flow_hint))) {
                        size_t used = strlen(message);
                        if (used + 1 < message_size)
                            snprintf(message + used, message_size - used,
                                     " %s.", flow_hint);
                    }
                }
            }
        } else {
            snprintf(message, message_size,
                     "%s planning. [R] changes type; [E] places; [B] exits.",
                     module_type_name((module_type_t)g.plan_type));
        }
        if (ghost) { *r = 180; *g0 = 165; *b = 115; }
        else       { *r = 130; *g0 = 190; *b = 210; }
        return true;
    }

    /* Autopilot */
    if (LOCAL_PLAYER.autopilot_mode) {
        label[0] = '\0';
        snprintf(message, message_size, "Autopilot active. Move to cancel.");
        *r = 180; *g0 = 160; *b = 90;
        return true;
    }

    /* Scaffold tow */
    if (LOCAL_PLAYER.ship.towed_scaffold >= 0) {
        snprintf(label, label_size, "SCAFFOLD TOW");
        snprintf(message, message_size, "Towing scaffold. [E] place, tap [Space] release.");
        *r = 160; *g0 = 150; *b = 100;
        return true;
    }

    hud_snapping_scaffold_t snap;
    if (hud_find_snapping_scaffold(&snap)) {
        snprintf(label, label_size, "SCAFFOLD SNAP");
        snprintf(message, message_size,
                 "%s snapping to %s ring %d slot %d. Supply starts when it locks.",
                 module_type_name(snap.type),
                 hud_station_short_name(snap.station_idx),
                 snap.ring,
                 snap.slot);
        *r = 190; *g0 = 170; *b = 100;
        return true;
    }

    hud_shipyard_blocked_t blocked;
    if (hud_find_shipyard_blocked(&blocked)) {
        snprintf(label, label_size, "YARD BLOCKED");
        snprintf(message, message_size,
                 "%s yard blocked by loose scaffold. Tow it clear to start %s.",
                 hud_station_short_name(blocked.station_idx),
                 module_type_name(blocked.type));
        *r = 255; *g0 = 140; *b = 60;
        return true;
    }

    hud_abandoned_plan_t abandoned;
    if (hud_find_abandoned_plan(&abandoned)) {
        snprintf(label, label_size, "ABANDONED PLAN");
        snprintf(message, message_size,
                 "%s has no reserved modules. Enter plan mode to add one, or clear the blueprint.",
                 hud_station_short_name(abandoned.station_idx));
        *r = 210; *g0 = 115; *b = 75;
        return true;
    }

    hud_construction_need_t need;
    if (hud_find_construction_need(&need)) {
        snprintf(label, label_size, "SUPPLY NEED");
        const char *station = hud_station_short_name(need.station_idx);
        const char *mat = commodity_short_label(need.need.material);
        int needed = (int)ceilf(need.need.remaining - 0.001f);
        if (needed < 1) needed = 1;
        if (need.held > 0) {
            snprintf(message, message_size,
                     "%s needs %d %s at %s. You carry %d; dock and press [S].",
                     need.need.station_shell ? "Outpost scaffold" : module_type_name(need.need.module_type),
                     needed, mat, station, need.held);
        } else {
            snprintf(message, message_size,
                     "%s needs %d %s at %s. %s",
                     need.need.station_shell ? "Outpost scaffold" : module_type_name(need.need.module_type),
                     needed, mat, station,
                     hud_material_source_hint(need.need.material));
        }
        *r = 205; *g0 = 165; *b = 90;
        return true;
    }

    /* Onboarding */
    if (onboarding_hint(label, label_size, message, message_size)) {
        *r = 255; *g0 = 221; *b = 119;
        return true;
    }

    /* Hold full */
    {
        int cargo = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
        int cap = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
        if (cargo >= cap) {
            label[0] = '\0';
            snprintf(message, message_size, "Hold full. Dock to sell.");
            *r = 180; *g0 = 150; *b = 80;
            return true;
        }
    }

    /* Docking */
    if (LOCAL_PLAYER.docking_approach) {
        label[0] = '\0';
        snprintf(message, message_size, "Docking. W or S to cancel.");
        *r = 120; *g0 = 160; *b = 150;
        return true;
    }
    if (LOCAL_PLAYER.in_dock_range) {
        label[0] = '\0';
        snprintf(message, message_size, "Press E to dock.");
        *r = 120; *g0 = 160; *b = 150;
        return true;
    }

    /* Tractor pickup */
    if (g.collection_feedback_timer > 0.0f) {
        label[0] = '\0';
        snprintf(message, message_size, "+%d fragment%s collected.",
            g.collection_feedback_fragments,
            g.collection_feedback_fragments == 1 ? "" : "s");
        mining_grade_rgb((mining_grade_t)hud_best_towed_fragment_grade(),
                         r, g0, b);
        return true;
    }

    /* Nothing to say. Panel is empty. */
    return false;
}

#ifdef __EMSCRIPTEN__
static int smoke_loop_state_override = 0;
static int smoke_apply_loop_state(int state);
enum { SMOKE_OUTPOST_INDEX = SIGNAL_FIRST_OUTPOST_INDEX };

static bool smoke_hooks_enabled(void) {
    return emscripten_run_script_int(
        "(new URLSearchParams(location.search).get('smoke')==='1')") != 0;
}

EMSCRIPTEN_KEEPALIVE
const char *get_hud_hint_text(void) {
    static char out[384];
    char label[64];
    char message[256];
    uint8_t r = 0, g0 = 0, b = 0;

    if (smoke_loop_state_override != 0)
        (void)smoke_apply_loop_state(smoke_loop_state_override);

    out[0] = '\0';
    label[0] = '\0';
    message[0] = '\0';
    if (!build_hud_message(label, sizeof(label), message, sizeof(message), &r, &g0, &b))
        return out;

    if (label[0] != '\0')
        snprintf(out, sizeof(out), "%s :: %s", label, message);
    else
        snprintf(out, sizeof(out), "%s", message);
    return out;
}

EMSCRIPTEN_KEEPALIVE
const char *get_hud_action_text(void) {
    static char out[192];
    int cargo_units = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    float sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
    hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);

    if (smoke_loop_state_override != 0) {
        (void)smoke_apply_loop_state(smoke_loop_state_override);
        cargo_units = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
        cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
        sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
    }

    out[0] = '\0';
    hud_format_action_wide(&act, current_station_ptr(), out, sizeof(out));
    return out;
}

enum {
    SMOKE_LOOP_STATE_CLEAR = 0,
    SMOKE_LOOP_STATE_FRAGMENTS_NEARBY = 1,
    SMOKE_LOOP_STATE_TRACTOR_REACHING = 2,
    SMOKE_LOOP_STATE_TRACTOR_LOCK = 3,
    SMOKE_LOOP_STATE_TOWING = 4,
    SMOKE_LOOP_STATE_HAIL_READY = 5,
    SMOKE_LOOP_STATE_HAIL_NOTICE = 6,
    SMOKE_LOOP_STATE_PLAN_GHOST = 7,
    SMOKE_LOOP_STATE_PLAN_SLOT = 8,
    SMOKE_LOOP_STATE_SCAFFOLD_SNAP = 9,
    SMOKE_LOOP_STATE_SUPPLY_NEED = 10,
    SMOKE_LOOP_STATE_YARD_BLOCKED = 11,
    SMOKE_LOOP_STATE_ABANDONED_PLAN = 12,
};

static void smoke_clear_loop_state(void) {
    server_player_t *sp = &LOCAL_PLAYER;
    float max_hull = ship_max_hull(&sp->ship);

    sp->docked = false;
    sp->in_dock_range = false;
    sp->docking_approach = false;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->hover_asteroid = -1;
    sp->scan_active = false;
    sp->scan_target_type = 0;
    sp->scan_target_index = -1;
    sp->scan_module_index = -1;
    sp->ship.towed_count = 0;
    sp->ship.towed_scaffold = -1;
    sp->ship.tractor_active = false;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;
    sp->autopilot_mode = 0;
    if (max_hull > 0.0f)
        sp->ship.hull = max_hull;

    g.plan_mode_active = false;
    g.notice_timer = 0.0f;
    g.notice[0] = '\0';
    g.collection_feedback_timer = 0.0f;
    g.collection_feedback_fragments = 0;
    g.collection_feedback_ore = 0.0f;
    g.hail_timer = 0.0f;
    g.hail_station[0] = '\0';
    g.hail_message[0] = '\0';
    g.hail_credits = 0.0f;
    g.hail_station_index = -1;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        g.world.scaffolds[i].active = false;
    }
    if (MAX_STATIONS > SMOKE_OUTPOST_INDEX) {
        station_t *ghost = &g.world.stations[SMOKE_OUTPOST_INDEX];
        ghost->scaffold = false;
        ghost->planned = false;
        ghost->scaffold_progress = 1.0f;
        ghost->name[0] = '\0';
        ghost->module_count = 0;
        ghost->pending_scaffold_count = 0;
    }

    if (g.world.station_count > 0 && station_exists(&g.world.stations[0]))
        sp->ship.pos = g.world.stations[0].pos;
}

static int smoke_apply_loop_state(int state) {
    server_player_t *sp = &LOCAL_PLAYER;

    smoke_clear_loop_state();
    switch (state) {
    case SMOKE_LOOP_STATE_CLEAR:
        return 1;
    case SMOKE_LOOP_STATE_FRAGMENTS_NEARBY:
        sp->nearby_fragments = 3;
        return 1;
    case SMOKE_LOOP_STATE_TRACTOR_REACHING:
        sp->nearby_fragments = 4;
        sp->ship.tractor_active = true;
        return 1;
    case SMOKE_LOOP_STATE_TRACTOR_LOCK:
        sp->nearby_fragments = 5;
        sp->tractor_fragments = 2;
        sp->ship.tractor_active = true;
        return 1;
    case SMOKE_LOOP_STATE_TOWING:
        sp->ship.towed_count = 1;
        sp->ship.towed_fragments[0] = 0;
        return 1;
    case SMOKE_LOOP_STATE_HAIL_READY:
        if (g.world.station_count <= 0 || !station_exists(&g.world.stations[0]))
            return 0;
        g.local_server.active = true;
        g.world.stations[0].ledger_count = 0;
        ledger_earn(&g.world.stations[0], sp->session_token, 123.0f);
        return 1;
    case SMOKE_LOOP_STATE_HAIL_NOTICE:
        snprintf(g.notice, sizeof(g.notice),
                 "Prospect: channel open. Balance 123 cr.");
        g.notice_timer = 6.0f;
        snprintf(g.hail_station, sizeof(g.hail_station), "Prospect");
        snprintf(g.hail_message, sizeof(g.hail_message), "channel open");
        g.hail_credits = 123.0f;
        g.hail_timer = 6.0f;
        g.hail_station_index = 0;
        return 1;
    case SMOKE_LOOP_STATE_PLAN_GHOST:
        g.plan_mode_active = true;
        g.plan_target_station = -1;
        g.placement_target_station = -1;
        g.placement_target_ring = 1;
        g.placement_target_slot = 2;
        g.plan_type = MODULE_SIGNAL_RELAY;
        return 1;
    case SMOKE_LOOP_STATE_PLAN_SLOT:
        g.plan_mode_active = true;
        g.plan_target_station = 0;
        g.placement_target_station = 0;
        g.placement_target_ring = 1;
        g.placement_target_slot = 0;
        g.plan_type = MODULE_HOPPER;
        return 1;
    case SMOKE_LOOP_STATE_SCAFFOLD_SNAP:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.scaffolds[0].active = true;
        g.world.scaffolds[0].state = SCAFFOLD_SNAPPING;
        g.world.scaffolds[0].module_type = MODULE_FURNACE;
        g.world.scaffolds[0].placed_station = SMOKE_OUTPOST_INDEX;
        g.world.scaffolds[0].placed_ring = 2;
        g.world.scaffolds[0].placed_slot = 3;
        g.world.scaffolds[0].pos = sp->ship.pos;
        return 1;
    case SMOKE_LOOP_STATE_SUPPLY_NEED:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].pos = sp->ship.pos;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold = true;
        g.world.stations[SMOKE_OUTPOST_INDEX].planned = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold_progress = 0.5f;
        g.world.stations[SMOKE_OUTPOST_INDEX].dock_radius = OUTPOST_DOCK_RADIUS;
        return 1;
    case SMOKE_LOOP_STATE_YARD_BLOCKED:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].pos = sp->ship.pos;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].planned = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold_progress = 1.0f;
        g.world.stations[SMOKE_OUTPOST_INDEX].signal_range = 6000.0f;
        g.world.stations[SMOKE_OUTPOST_INDEX].dock_radius = OUTPOST_DOCK_RADIUS;
        g.world.stations[SMOKE_OUTPOST_INDEX].module_count = 1;
        g.world.stations[SMOKE_OUTPOST_INDEX].modules[0] = (station_module_t){
            .type = MODULE_SHIPYARD,
            .ring = 2,
            .slot = 0,
            .scaffold = false,
            .build_progress = 1.0f,
        };
        g.world.stations[SMOKE_OUTPOST_INDEX].pending_scaffold_count = 1;
        g.world.stations[SMOKE_OUTPOST_INDEX].pending_scaffolds[0].type = MODULE_FURNACE;
        g.world.stations[SMOKE_OUTPOST_INDEX].pending_scaffolds[0].owner = 0;
        g.world.scaffolds[0].active = true;
        g.world.scaffolds[0].state = SCAFFOLD_LOOSE;
        g.world.scaffolds[0].module_type = MODULE_DOCK;
        g.world.scaffolds[0].pos = sp->ship.pos;
        g.world.scaffolds[0].built_at_station = -1;
        g.world.scaffolds[0].towed_by = -1;
        return 1;
    case SMOKE_LOOP_STATE_ABANDONED_PLAN:
        if (MAX_STATIONS <= SMOKE_OUTPOST_INDEX) return 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].pos = sp->ship.pos;
        g.world.stations[SMOKE_OUTPOST_INDEX].planned = true;
        g.world.stations[SMOKE_OUTPOST_INDEX].scaffold = false;
        g.world.stations[SMOKE_OUTPOST_INDEX].placement_plan_count = 0;
        g.world.stations[SMOKE_OUTPOST_INDEX].dock_radius = 0.0f;
        return 1;
    default:
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE
int set_smoke_loop_state(int state) {
    if (!smoke_hooks_enabled()) {
        smoke_loop_state_override = 0;
        return 0;
    }
    int ok = smoke_apply_loop_state(state);
    smoke_loop_state_override = ok ? state : 0;
    return ok;
}
#endif

/* ------------------------------------------------------------------ */
/* draw_hud_panels -- background panel geometry for the flight HUD     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Hull-fog textures — generated radial vignettes, one per damage tier */
/* ------------------------------------------------------------------ */

#define HULL_FOG_LEVELS 4
#define HULL_FOG_TEX_SIZE 256

static struct {
    bool initialized;
    uint32_t image_id[HULL_FOG_LEVELS];
    uint32_t view_id[HULL_FOG_LEVELS];
    uint32_t sampler_id;
    uint32_t blend_pip_id;    /* sgl pipeline with alpha blending enabled */
} hull_fog;

static float fog_smoothstep(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

void hull_fog_init(void) {
    if (hull_fog.initialized) return;

    sg_sampler samp = sg_make_sampler(&(sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v = SG_WRAP_CLAMP_TO_EDGE,
    });
    hull_fog.sampler_id = samp.id;

    /* Custom sokol_gl pipeline with alpha blending enabled. The default
     * sgl pipeline has blend.enabled=false and write_mask=RGB, so any
     * alpha in vertex colors or textures is completely ignored. */
    sgl_pipeline blend_pip = sgl_make_pipeline(&(sg_pipeline_desc){
        .colors[0] = {
            .write_mask = SG_COLORMASK_RGBA,
            .blend = {
                .enabled = true,
                .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA,
                .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                .src_factor_alpha = SG_BLENDFACTOR_ONE,
                .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
            },
        },
    });
    hull_fog.blend_pip_id = blend_pip.id;

    /* Generate one radial vignette per damage tier. The "clear hole" in
     * the middle gets smaller and the surrounding fog gets darker as
     * the tier rises. RGB is white so the vertex color can tint it. */
    static const float clear_radius[HULL_FOG_LEVELS] = {
        0.70f, /* tier 0: caution — barely any darkening */
        0.50f, /* tier 1: warn */
        0.32f, /* tier 2: danger */
        0.18f, /* tier 3: critical — tight aperture */
    };
    static const float peak_alpha[HULL_FOG_LEVELS] = {
        0.45f, 0.65f, 0.82f, 0.95f,
    };

    /* Separate buffer per tier so the immutable image uploads each
     * see distinct data, no aliasing.
     *
     * The fog edge isn't a clean circle — it's deformed by a stack of
     * angular cosines so fingers reach inward at irregular intervals.
     * Multiple frequencies + per-octave phase offsets keep them from
     * lining up into a clean star. The deformation is enveloped so it
     * vanishes at the center (clear hole stays clean) and at the
     * corners (full coverage holds). Result reads as "rivers of blood
     * pulling in from the edges" rather than a circular vignette,
     * while the smoothstep falloff keeps everything blurry. */
    static uint8_t pixels[HULL_FOG_LEVELS][HULL_FOG_TEX_SIZE * HULL_FOG_TEX_SIZE * 4];
    for (int level = 0; level < HULL_FOG_LEVELS; level++) {
        uint8_t *pix = pixels[level];
        float r0 = clear_radius[level];
        float r1 = 1.10f; /* fog reaches full alpha just past the corners */
        float pa = peak_alpha[level];
        /* Tendril depth scales with tier — barely visible at caution,
         * pronounced fingers at critical. */
        float tendril_amp = 0.06f + 0.10f * ((float)level / (float)(HULL_FOG_LEVELS - 1));
        for (int y = 0; y < HULL_FOG_TEX_SIZE; y++) {
            for (int x = 0; x < HULL_FOG_TEX_SIZE; x++) {
                float fx = ((float)x / (float)(HULL_FOG_TEX_SIZE - 1)) * 2.0f - 1.0f;
                float fy = ((float)y / (float)(HULL_FOG_TEX_SIZE - 1)) * 2.0f - 1.0f;
                float d_radial = sqrtf(fx * fx + fy * fy);
                float theta = atan2f(fy, fx);
                /* Three octaves of cosine + irrational-ish phase offsets
                 * so the period is long and the pattern looks organic. */
                float tendril =
                    0.55f * cosf(theta *  5.0f + 0.0f)
                  + 0.32f * cosf(theta *  9.0f + 1.7f)
                  + 0.18f * cosf(theta * 17.0f + 0.4f);
                /* Envelope: 4 * d * (1-d) peaks at d=0.5, 0 at d=0 and d=1.
                 * Keeps the clear center round and the corners saturated. */
                float env = 4.0f * d_radial * (1.0f - d_radial);
                if (env < 0.0f) env = 0.0f;
                float d = d_radial - tendril_amp * tendril * env;
                float a = fog_smoothstep(r0, r1, d) * pa;
                int p = (y * HULL_FOG_TEX_SIZE + x) * 4;
                pix[p + 0] = 255;
                pix[p + 1] = 255;
                pix[p + 2] = 255;
                pix[p + 3] = (uint8_t)(a * 255.0f);
            }
        }
        /* IMMUTABLE image with .data inline. sg_make_image uploads
         * synchronously and bypasses sg_update_image's frame-index
         * check that silently drops updates at init time
         * (upd_frame_index == frame_index == 0 → validation fails). */
        sg_image img = sg_make_image(&(sg_image_desc){
            .width = HULL_FOG_TEX_SIZE,
            .height = HULL_FOG_TEX_SIZE,
            .pixel_format = SG_PIXELFORMAT_RGBA8,
            .data.mip_levels[0] = {
                .ptr = pix,
                .size = (size_t)(HULL_FOG_TEX_SIZE * HULL_FOG_TEX_SIZE * 4),
            },
        });
        sg_view view = sg_make_view(&(sg_view_desc){ .texture.image = img });
        hull_fog.image_id[level] = img.id;
        hull_fog.view_id[level] = view.id;
    }
    hull_fog.initialized = true;
}

void hull_fog_shutdown(void) {
    if (hud_alpha_pipeline_id) {
        sgl_destroy_pipeline((sgl_pipeline){ hud_alpha_pipeline_id });
        hud_alpha_pipeline_id = 0;
    }

    if (!hull_fog.initialized) return;

    if (hull_fog.blend_pip_id) {
        sgl_destroy_pipeline((sgl_pipeline){ hull_fog.blend_pip_id });
        hull_fog.blend_pip_id = 0;
    }
    for (int level = 0; level < HULL_FOG_LEVELS; level++) {
        if (hull_fog.view_id[level]) {
            sg_destroy_view((sg_view){ hull_fog.view_id[level] });
            hull_fog.view_id[level] = 0;
        }
        if (hull_fog.image_id[level]) {
            sg_destroy_image((sg_image){ hull_fog.image_id[level] });
            hull_fog.image_id[level] = 0;
        }
    }
    if (hull_fog.sampler_id) {
        sg_destroy_sampler((sg_sampler){ hull_fog.sampler_id });
        hull_fog.sampler_id = 0;
    }
    hull_fog.initialized = false;
}

/* Shared lava-lamp pulse — slow multi-frequency drift, no spikes. */
static float fog_pulse(void) {
    float t = g.world.time;
    float wob = 0.55f * sinf(t * 0.43f)
              + 0.30f * sinf(t * 0.71f + 1.3f)
              + 0.15f * sinf(t * 1.07f + 2.7f);
    return 0.92f + 0.08f * wob;
}

/* Emit the textured fog quads for one wave at the given intensity. Picks
 * the tier-pair around `intensity` and crossfades them so the aperture
 * tightens smoothly. Caller is responsible for setting up screen-space
 * ortho before calling. */
static void draw_fog_quads(float intensity, float pulse) {
    if (!hull_fog.initialized) return;
    if (intensity <= 0.01f) return;
    if (intensity > 1.0f) intensity = 1.0f;

    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();

    /* Continuous tier value in [0, HULL_FOG_LEVELS - 1]. Lerp the texture
     * pair around it so transitions are smooth across the whole range. */
    float tier = intensity * (float)(HULL_FOG_LEVELS - 1);
    int t0 = (int)floorf(tier);
    if (t0 < 0) t0 = 0;
    if (t0 > HULL_FOG_LEVELS - 1) t0 = HULL_FOG_LEVELS - 1;
    int t1 = t0 + 1;
    if (t1 > HULL_FOG_LEVELS - 1) t1 = HULL_FOG_LEVELS - 1;
    float blend = tier - (float)t0;
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;

    /* Dark blood red tint. Brightens slightly with intensity.
     * Locals prefixed with tint_ to avoid shadowing the global `g` game_t. */
    float tint_r = 0.20f + 0.25f * intensity;
    float tint_g = 0.005f + 0.01f * intensity;
    float tint_b = 0.01f + 0.02f * intensity;

    /* Push our alpha-blending pipeline so the texture's alpha actually
     * affects the framebuffer. The default sokol_gl pipeline disables
     * blending and writes RGB only — alpha is silently discarded. */
    sgl_push_pipeline();
    sgl_load_pipeline((sgl_pipeline){ hull_fog.blend_pip_id });
    sgl_enable_texture();

    /* Lower tier (full strength) */
    {
        float a = (1.0f - blend) * pulse;
        sgl_texture(
            (sg_view){ hull_fog.view_id[t0] },
            (sg_sampler){ hull_fog.sampler_id });
        sgl_begin_quads();
        sgl_c4f(tint_r, tint_g, tint_b, a);
        sgl_v2f_t2f(0.0f,     0.0f,     0.0f, 0.0f);
        sgl_v2f_t2f(screen_w, 0.0f,     1.0f, 0.0f);
        sgl_v2f_t2f(screen_w, screen_h, 1.0f, 1.0f);
        sgl_v2f_t2f(0.0f,     screen_h, 0.0f, 1.0f);
        sgl_end();
    }

    /* Upper tier (blended weight) */
    if (t1 != t0) {
        float a = blend * pulse;
        sgl_texture(
            (sg_view){ hull_fog.view_id[t1] },
            (sg_sampler){ hull_fog.sampler_id });
        sgl_begin_quads();
        sgl_c4f(tint_r, tint_g, tint_b, a);
        sgl_v2f_t2f(0.0f,     0.0f,     0.0f, 0.0f);
        sgl_v2f_t2f(screen_w, 0.0f,     1.0f, 0.0f);
        sgl_v2f_t2f(screen_w, screen_h, 1.0f, 1.0f);
        sgl_v2f_t2f(0.0f,     screen_h, 0.0f, 1.0f);
        sgl_end();
    }

    sgl_disable_texture();
    sgl_pop_pipeline();
}

/* Back wave — drawn before the world. Ramps 0..1 over damage 0..0.5
 * (HP 100→50), then holds at 1. Caller (render_frame) sets up screen
 * ortho. Renders the dark void deepening behind ships/stations without
 * eating their contrast. */
void draw_hull_fog_back(void) {
    if (!hull_fog.initialized) return;
    float damage = g.fog_intensity;
    if (damage <= 0.01f) return;
    float intensity = damage * 2.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    float pulse = fog_pulse() + 0.04f * intensity;
    draw_fog_quads(intensity, pulse);
}

/* Front wave — drawn over the world (HUD pass). Closing-in aperture that
 * only kicks in below 50% HP, ramping 0..1 over damage 0.5..1.0. */
static void draw_hull_warning_overlay(void) {
    if (!hull_fog.initialized) return;
    float damage = g.fog_intensity;
    float intensity = (damage - 0.5f) * 2.0f;
    if (intensity <= 0.01f) return;
    if (intensity > 1.0f) intensity = 1.0f;
    float pulse = fog_pulse() + 0.04f * intensity;
    draw_fog_quads(intensity, pulse);
}

void draw_hud_panels(void) {
    if (g.death_screen_timer > 0.0f) return;
    /* Suppress HUD chrome during the death cinematic — we want a clean
     * shot of the wreckage and stats menu. */
    if (g.death_cinematic.active || g.death_cinematic.menu_alpha > 0.001f) {
        draw_hull_warning_overlay();
        return;
    }
    draw_hull_warning_overlay();
    float top_x = 0.0f, top_y = 0.0f, top_w = 0.0f, top_h = 0.0f;
    float bottom_x = 0.0f, bottom_y = 0.0f, bottom_w = 0.0f, bottom_h = 0.0f;
    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);

    /* Top flight HUD panel — suppressed while docked so the station terminal
     * is the only status surface in view. */
    if (!(LOCAL_PLAYER.docked && g.dock_settle_timer <= 0.0f)) {
        draw_ui_panel(top_x, top_y, top_w, top_h, 0.03f);
    }

    if (LOCAL_PLAYER.docked && g.dock_settle_timer <= 0.0f) {
        float panel_x = 0.0f;
        float panel_y = 0.0f;
        float panel_w = 0.0f;
        float panel_h = 0.0f;
        bool compact = ui_is_compact();
        station_ui_state_t ui = { 0 };
        build_station_ui_state(&ui);
        get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
        draw_ui_scrim(0.34f);
        /* Tint panel accent by station role color */
        {
            float sr, sg, sb;
            station_role_color(ui.station, &sr, &sg, &sb);
            draw_ui_panel_colored(panel_x, panel_y, panel_w, panel_h, sr, sg, sb);
        }

        float inner_x = panel_x + 18.0f;

        /* Divider rule below the persistent header band (drawn by
         * station_ui.c at panel_y + 26/42/58). The rule visually separates
         * the always-visible header (name/role/ledger/hull/cargo/ticker)
         * from the per-view content body below. */
        draw_ui_rule(inner_x, panel_x + panel_w - 18.0f, panel_y + 72.0f,
            0.14f, 0.26f, 0.38f, 0.70f);

        /* Content body draws directly on the outer panel — no nested frame. */

        /* Ship status strip -- persistent labeled ship state.
         * Layout: "HULL" label, hull meter, "HULL N/N", gap, "CARGO" label,
         * cargo meter, "CARGO N/N", then "LSR N HLD N TRC N" on the right.
         * Pip rows replaced by inline text chips to match the terminal voice. */
        {
            float strip_y = panel_y + panel_h - (compact ? 32.0f : 38.0f);
            float label_y = strip_y + 6.0f;     /* text baseline inside strip */
            float meter_y = strip_y + 10.0f;    /* meter top */
            float meter_h = 8.0f;
            float meter_w = compact ? 80.0f : 100.0f;
            const float cell_w = 8.0f;

            const ship_t *ship = &LOCAL_PLAYER.ship;
            int hull_n = (int)lroundf(ship->hull);
            int hull_m = (int)lroundf(ship_max_hull(ship));
            int carg_n = (int)lroundf(ship_total_cargo(ship));
            int carg_m = (int)lroundf(ship_cargo_capacity(ship));

            float x = inner_x + 6.0f;

            /* HULL label */
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts("HULL");
            x += 5.0f * cell_w;
            /* Hull meter */
            draw_ui_meter(x, meter_y, meter_w, meter_h,
                ship->hull / ship_max_hull(ship), 0.96f, 0.54f, 0.28f);
            x += meter_w + 6.0f;
            /* Hull numeric */
            char hull_buf[20];
            snprintf(hull_buf, sizeof(hull_buf), "%d/%d", hull_n, hull_m);
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts(hull_buf);
            x += ((float)strlen(hull_buf) + 2.0f) * cell_w;

            /* CARGO label */
            sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts("CARGO");
            x += 6.0f * cell_w;
            draw_ui_meter(x, meter_y, meter_w, meter_h,
                (float)carg_n / fmaxf(1.0f, (float)carg_m), 0.26f, 0.90f, 0.72f);
            x += meter_w + 6.0f;
            char carg_buf[20];
            snprintf(carg_buf, sizeof(carg_buf), "%d/%d", carg_n, carg_m);
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_pos(ui_text_pos(x), ui_text_pos(label_y));
            sdtx_puts(carg_buf);

            /* Upgrade chips — right-aligned inside the panel. */
            if (!compact) {
                char chips[32];
                snprintf(chips, sizeof(chips), "LSR %d  HLD %d  TRC %d",
                         ship->mining_level, ship->hold_level, ship->tractor_level);
                float chips_w = (float)strlen(chips) * cell_w;
                sdtx_color3b(PAL_NAV_BLUE);
                sdtx_pos(ui_text_pos(panel_x + panel_w - 22.0f - chips_w),
                         ui_text_pos(label_y));
                sdtx_puts(chips);
            }
        }
    }

    /* Message panel background removed — text only now */
}

/* ------------------------------------------------------------------ */
/* draw_hud -- the main HUD text layer                                 */
/* ------------------------------------------------------------------ */

/* Death overlay. Phase 0 keeps the world visible: wreckage tumbles,
 * critical warning flashes, and the screen fades slowly toward black.
 * Phase 1 fades in stats + "[E] launch". Returns true when it owns the
 * HUD layer so regular flight chrome stays hidden. */
static bool draw_death_overlay(float screen_w, float screen_h) {
    bool active = g.death_cinematic.active;
    if (!active && g.death_cinematic.menu_alpha <= 0.001f)
        return false;

    float menu_alpha = g.death_cinematic.menu_alpha;
    if (menu_alpha < 0.0f) menu_alpha = 0.0f;
    if (menu_alpha > 1.0f) menu_alpha = 1.0f;
    float age = active ? g.death_cinematic.age : DEATH_CINEMATIC_FADE_TO_BLACK_SEC;
    float fade_t = active ? clampf(age / DEATH_CINEMATIC_FADE_TO_BLACK_SEC, 0.0f, 1.0f)
                          : menu_alpha;
    fade_t = fade_t * fade_t * (3.0f - 2.0f * fade_t);
    float scrim = fmaxf(0.74f * fade_t, 0.55f * menu_alpha);
    if (scrim > 0.001f)
        hud_draw_alpha_rect(0.0f, 0.0f, screen_w, screen_h, 0.0f, 0.0f, 0.0f, scrim);

    /* 1:1 canvas so text fills the screen. */
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    float cx = screen_w * 0.5f;
    float cy = screen_h * 0.5f;
    float cell = (screen_w < 380.0f) ? 7.0f : 8.0f;

    if (active && age < DEATH_CINEMATIC_WARNING_SEC) {
        float warning_fade = clampf((DEATH_CINEMATIC_WARNING_SEC - age) / 0.8f, 0.0f, 1.0f);
        float blink = (sinf(g.world.time * 18.0f) > 0.0f) ? 1.0f : 0.20f;
        uint8_t wa = (uint8_t)(255.0f * warning_fade * blink);
        sdtx_color4b(PAL_DEATH_PROMPT, wa);
        sdtx_centered_text(cx, fmaxf(2.0f, (screen_h * 0.18f) / cell),
                           cell, "[ SYSTEM CRITICAL :::: SIGNAL LOST ]");
    }

    if (menu_alpha <= 0.001f) return true;

    float alpha = menu_alpha;
    uint8_t a8 = (uint8_t)(alpha * 255.0f);

    /* Title. */
    sdtx_color4b(PAL_DEATH_TITLE, a8);
    sdtx_centered_text(cx, (cy - 60.0f) / cell, cell, "SHIP DESTROYED");

    /* Stats. */
    float row = (cy - 16.0f) / cell;
    float left = fmaxf(1.0f, (cx - 110.0f) / cell);
    sdtx_color4b(PAL_NOTICE, a8);

    sdtx_pos(left, row);
    sdtx_printf("Ore smelted:   %8.0f", g.death_ore_mined);
    row += 2.5f;
    sdtx_pos(left, row);
    sdtx_printf("Rocks broken:  %8d", g.death_asteroids_fractured);
    row += 2.5f;
    sdtx_pos(left, row);
    sdtx_color4b(PAL_DEATH_EARNED, a8);
    sdtx_printf("Credits earned:%8.0f", g.death_credits_earned);
    row += 2.5f;
    sdtx_pos(left, row);
    sdtx_color4b(PAL_DEATH_SPENT, a8);
    sdtx_printf("Credits spent: %8.0f", g.death_credits_spent);
    row += 3.0f;

    /* Global highscores — top runs broadcast by the server. The
     * player's just-submitted run is highlighted in the death-earned
     * color so they can spot where they landed. Always render the
     * header so the player knows the leaderboard exists even when
     * empty (first run on a fresh server) or before the server's
     * post-death broadcast has arrived. */
    {
        sdtx_color4b(PAL_NOTICE, a8);
        sdtx_centered_text(cx, row, cell, "-- TOP RUNS --");
        row += 1.6f;
            if (g.highscore_count > 0) {
                int show = (g.highscore_count > 8) ? 8 : g.highscore_count;
                for (int i = 0; i < show; i++) {
                    char cs[9];
                    memcpy(cs, g.highscores[i].callsign, 8);
                    cs[8] = '\0';
                    for (int k = 7; k >= 0 && (cs[k] == ' ' || cs[k] == '\0'); k--) cs[k] = '\0';
                    char kb[9];
                    memcpy(kb, g.highscores[i].killed_by, 8);
                    kb[8] = '\0';
                    for (int k = 7; k >= 0 && (kb[k] == ' ' || kb[k] == '\0'); k--) kb[k] = '\0';
                    bool is_me = (g.death_credits_earned > 0.5f
                                  && fabsf(g.highscores[i].credits_earned - g.death_credits_earned) < 0.5f);
                    if (is_me) sdtx_color4b(PAL_DEATH_EARNED, a8);
                    else       sdtx_color4b(PAL_TEXT_FADED,  a8);
                    sdtx_pos(left, row);
                    /* callsign / credits / killed-by / build. world_id is
                     * kept in the data but dropped from the visible row
                     * to keep the line readable on narrow viewports. */
                    sdtx_printf("%2d. %-8s %7.0f  %-8s b#%08x",
                                i + 1, cs, g.highscores[i].credits_earned,
                                kb[0] ? kb : "-",
                                g.highscores[i].build_id);
                    row += 1.4f;
                }
            } else {
                sdtx_color4b(PAL_TEXT_FADED, a8);
                sdtx_pos(left, row);
                sdtx_puts("  (no records yet)");
                row += 1.4f;
            }
            row += 1.0f;
        }

    /* Prompt — RED, hard FLASH on/off. Includes the spawn fee that was
     * just debited at the respawn station so the player sees the cost
     * of dying ("respawn -300 Helios credits"). */
    float flash = (sinf(g.world.time * 7.0f) > 0.0f) ? 1.0f : 0.25f;
    uint8_t pa = (uint8_t)(flash * (float)a8);
    sdtx_color4b(PAL_DEATH_PROMPT, pa);
    char prompt[80];
    if (g.death_respawn_fee > 0.5f &&
        g.death_respawn_station < MAX_STATIONS &&
        g.world.stations[g.death_respawn_station].name[0]) {
        const char *cur =
            g.world.stations[g.death_respawn_station].currency_name[0]
            ? g.world.stations[g.death_respawn_station].currency_name
            : "credits";
        snprintf(prompt, sizeof(prompt),
                 "[ E ] launch  -%.0f %s",
                 g.death_respawn_fee, cur);
    } else {
        snprintf(prompt, sizeof(prompt), "[ E ] launch");
    }
    sdtx_centered_text(cx, row, cell, prompt);

    return true;
}

void draw_hud(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();

    /* Death-screen overlay short-circuits the rest of the HUD. */
    if (draw_death_overlay(screen_w, screen_h)) return;

    bool compact = ui_is_compact();
    float top_x = 0.0f;
    float top_y = 0.0f;
    float top_w = 0.0f;
    float top_h = 0.0f;
    float bottom_x = 0.0f;
    float bottom_y = 0.0f;
    float bottom_w = 0.0f;
    float bottom_h = 0.0f;
    float message_x = 0.0f;
    float message_y = 0.0f;
    float message_w = 0.0f;
    float message_h = 0.0f;
    get_flight_hud_rects(&top_x, &top_y, &top_w, &top_h, &bottom_x, &bottom_y, &bottom_w, &bottom_h);
    float top_text_x = ui_text_pos(top_x + 16.0f);
    float top_row_0 = ui_text_pos(top_y + 16.0f);
    float top_row_1 = ui_text_pos(top_y + (compact ? 24.0f : 30.0f));
    float top_row_2 = ui_text_pos(top_y + (compact ? 32.0f : 44.0f));
    float top_row_3 = ui_text_pos(top_y + (compact ? 40.0f : 58.0f));
    char message_label[32] = { 0 };
    char message_text[320] = { 0 };
    /* Up to 4 wrapped lines for the subtitle. Long station hails land
     * here; empty slots are skipped on render. */
    enum { HUD_MSG_LINES = 4, HUD_MSG_LINE_CAP = 96 };
    char message_lines[HUD_MSG_LINES][HUD_MSG_LINE_CAP] = {{0}};
    uint8_t message_r = 164;
    uint8_t message_g = 177;
    uint8_t message_b = 205;
    int hull_units = (int)lroundf(LOCAL_PLAYER.ship.hull);
    int hull_capacity = (int)lroundf(ship_max_hull(&LOCAL_PLAYER.ship));

    /* --- Low HP warning: pulsing red text in message area instead of vignette --- */
    /* (hull warning state is used by build_hud_message to show HULL INTEGRITY FAILING) */
    int cargo_units = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
    int credits = (int)lroundf(player_current_balance());
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    const station_t* current_station = current_station_ptr();
    const station_t* navigation_station = navigation_station_ptr();
    station_ui_state_t ui = { 0 };
    if (LOCAL_PLAYER.docked) {
        build_station_ui_state(&ui);
    }
    int station_distance = 0;

    vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
    vec2 home = v2(0.0f, -1.0f);
    if (navigation_station != NULL) {
        station_distance = (int)lroundf(v2_len(v2_sub(navigation_station->pos, LOCAL_PLAYER.ship.pos)));
        home = v2_norm(v2_sub(navigation_station->pos, LOCAL_PLAYER.ship.pos));
    }
    float bearing = atan2f(v2_cross(forward, home), v2_dot(forward, home));
    int bearing_degrees = (int)lroundf(fabsf(bearing) * (180.0f / PI_F));
    const char* bearing_side = "ahead";
    if (bearing > 0.12f) {
        bearing_side = "left";
    } else if (bearing < -0.12f) {
        bearing_side = "right";
    } else {
        bearing_degrees = 0;
    }

    sdtx_canvas(screen_w / ui_text_zoom(), screen_h / ui_text_zoom());
    sdtx_font(0);
    sdtx_origin(0.0f, 0.0f);
    sdtx_home();
    if (hud_should_draw_message_panel()) {
        int message_cols = 0;
        get_hud_message_panel_rect(&message_x, &message_y, &message_w, &message_h);
        build_hud_message(message_label, sizeof(message_label), message_text, sizeof(message_text), &message_r, &message_g, &message_b);
        message_cols = (int)((message_w - 28.0f) / (HUD_CELL * ui_text_zoom()));
        /* Wrap into up to HUD_MSG_LINES word-wrapped lines. Long station
         * hails ("Station: MOTD  (balance N cur)") fit fully now; the
         * old 2-line splitter dropped everything past line 1. */
        wrap_hud_message_lines(message_text, message_cols, message_lines,
                               HUD_MSG_LINE_CAP, HUD_MSG_LINES);
    }

    float sig_quality = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
    int sig_pct = (int)lroundf(sig_quality * 100.0f);
    const char* sig_band = signal_band_name(sig_quality);
    uint8_t sig_r, sig_g, sig_b;
    if (sig_quality < SIGNAL_BAND_FRONTIER)         { sig_r = 255; sig_g = 80;  sig_b = 80;  }
    else if (sig_quality < SIGNAL_BAND_FRINGE)      { sig_r = 255; sig_g = 180; sig_b = 80;  }
    else if (sig_quality < SIGNAL_BAND_OPERATIONAL) { sig_r = 255; sig_g = 221; sig_b = 119; }
    else                                            { sig_r = 203; sig_g = 220; sig_b = 248; }

    if (compact) {
        const char* nav_role = navigation_station != NULL ? station_role_short_name(navigation_station) : "STN";
        const char* dock_role = current_station != NULL ? station_role_short_name(current_station) : "STN";
        const char* bearing_mark = "A";
        if (bearing > 0.12f) {
            bearing_mark = "L";
        } else if (bearing < -0.12f) {
            bearing_mark = "R";
        }

        if (!LOCAL_PLAYER.docked) {
            sdtx_pos(top_text_x, top_row_0);
            sdtx_color3b(PAL_TEXT_PRIMARY);
            {
                /* Use the SESSION callsign (sent over the wire and echoed
                 * back in highscores + remote ship labels) — NOT the local
                 * mining-keypair callsign, which is cryptographic-only and
                 * doesn't match what other players or the death-screen
                 * leaderboard see. */
                const char *cs = net_local_callsign();
                const char *tag = (cs && cs[0] != '\0') ? cs : "SHIP";
                sdtx_puts(tag);
            }

            sdtx_pos(top_text_x, top_row_1);
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_printf("H %d/%d  C %d/%d  ", hull_units, hull_capacity, cargo_units, cargo_capacity);
            sdtx_color3b(sig_r, sig_g, sig_b);
            sdtx_printf("%s %d%%", sig_band, sig_pct);
            if (sig_quality < SIGNAL_BAND_OPERATIONAL) {
                int mine_pct = (int)lroundf(signal_mining_efficiency(sig_quality) * 100.0f);
                sdtx_printf(" M%d%%", mine_pct);
            }

            sdtx_pos(top_text_x, top_row_2);
            if (LOCAL_PLAYER.in_dock_range) {
                sdtx_color3b(PAL_SIGNAL_MINT);
                sdtx_puts("DOCK RANGE // E dock");
            } else {
                sdtx_color3b(PAL_NAV_BLUE);
                sdtx_printf("%s %d u // %d %s", nav_role, station_distance, bearing_degrees, bearing_mark);
            }

            sdtx_pos(top_text_x, top_row_3);
            {
                hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
                hud_render_action_compact(&act, dock_role);
            }
        }

        if (!LOCAL_PLAYER.docked && hud_should_draw_message_panel()) {
            /* Subtitle: up to HUD_MSG_LINES wrapped lines, stacked bottom-up. */
            float cell = HUD_CELL * ui_text_zoom();
            int first_line_with_content = -1;
            int last_line_with_content = -1;
            for (int li = 0; li < HUD_MSG_LINES; li++) {
                if (message_lines[li][0] != '\0') {
                    if (first_line_with_content < 0) first_line_with_content = li;
                    last_line_with_content = li;
                }
            }
            int count = (last_line_with_content >= first_line_with_content)
                        ? (last_line_with_content - first_line_with_content + 1) : 0;
            for (int vi = 0; vi < count; vi++) {
                int li = first_line_with_content + vi;
                char full_msg[256];
                if (vi == 0 && message_label[0] != '\0')
                    snprintf(full_msg, sizeof(full_msg), "%s: %s",
                             message_label, message_lines[li]);
                else
                    snprintf(full_msg, sizeof(full_msg), "%s", message_lines[li]);
                float msg_w = (float)strlen(full_msg) * cell;
                float msg_x = (screen_w * 0.5f - msg_w * 0.5f) / cell;
                /* Stack upward so the newest line sits at the panel bottom. */
                float msg_y = (screen_h - 32.0f - (float)(count - 1 - vi) * cell * 1.25f) / cell;
                sdtx_pos(msg_x, msg_y);
                sdtx_color3b(message_r, message_g, message_b);
                sdtx_puts(full_msg);
            }
        }

        draw_station_services(&ui);
        /* Shared post-classify panels also fire in compact mode so a
         * narrow window doesn't lose signal-lost warnings, MP status,
         * the hail sigil, etc. (Previously the compact early-return
         * hid them entirely.) */
        hud_draw_shared_panels(screen_w, screen_h, sig_quality, true);
        return;
    }

    sdtx_pos(top_text_x, top_row_0);
    sdtx_color3b(PAL_TEXT_PRIMARY);
    {
        /* SESSION callsign — see compact_top above for why we ignore
         * mining_client_get()->player_callsign here. */
        const char *cs = net_local_callsign();
        const char *fallback = LOCAL_PLAYER.docked ? "RUN STATUS" : "SHIP STATUS";
        if (cs && cs[0] != '\0')
            sdtx_printf("%s // %s", cs, LOCAL_PLAYER.docked ? "DOCKED" : "FLIGHT");
        else
            sdtx_puts(fallback);
    }

    sdtx_pos(top_text_x, top_row_1);
    sdtx_color3b(PAL_TEXT_SECONDARY);
    if (LOCAL_PLAYER.docked) {
        sdtx_printf("%d %s  H %d/%d  C %d/%d  ",
                    credits, player_current_currency(),
                    hull_units, hull_capacity, cargo_units, cargo_capacity);
    } else {
        sdtx_printf("H %d/%d  C %d/%d  ",
                    hull_units, hull_capacity, cargo_units, cargo_capacity);
    }
    sdtx_color3b(sig_r, sig_g, sig_b);
    sdtx_printf("%s %d%%", sig_band, sig_pct);
    if (sig_quality < SIGNAL_BAND_OPERATIONAL) {
        int mine_eff = (int)lroundf(signal_mining_efficiency(sig_quality) * 100.0f);
        int ctrl_eff = (int)lroundf(signal_control_scale(sig_quality) * 100.0f);
        sdtx_printf("  MINE %d%% CTRL %d%%", mine_eff, ctrl_eff);
    }

    sdtx_pos(top_text_x, top_row_2);
    if (LOCAL_PLAYER.docked && current_station) {
        sdtx_color3b(PAL_SIGNAL_MINT);
        sdtx_printf("%s // docked // E launch", current_station->name);
    } else if (LOCAL_PLAYER.in_dock_range) {
        sdtx_color3b(PAL_SIGNAL_MINT);
        sdtx_puts("In dock range. [E] to dock.");
    } else {
        sdtx_color3b(PAL_NAV_BLUE);
        sdtx_printf("%s %d u // %d deg %s",
            navigation_station != NULL ? navigation_station->name : "Station",
            station_distance,
            bearing_degrees,
            bearing_side);
    }

    sdtx_pos(top_text_x, top_row_3);
    {
        hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
        hud_render_action_wide(&act, current_station);
    }

    /* Subtitle: one clean line, centered at bottom-center.
     *
     * Sell-batch summary takes priority when it has content — the line is
     * rendered as colored segments (total in white/gold, each grade label
     * in its canonical color) so the player can see at a glance which
     * grades just paid out. See shared/mining.h:mining_grade_rgb for the
     * palette. Falls through to the regular message otherwise. */
    bool sell_batch_has_content = false;
    if (g.sell_batch.active || g.sell_batch.display_timer > 0.0f) {
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
            if (g.sell_batch.grade_counts[gi] > 0) { sell_batch_has_content = true; break; }
        }
    }

    if (sell_batch_has_content) {
        const float cell = 8.0f;
        /* Pre-measure the composed line so we can center it. Contract
         * settlements name the paying station and its local currency:
         *   "[ Kepler Yard paid +120 kepler bonds  common x1 ]" */
        char head[128], tail[160];
        const station_t *paid_station = NULL;
        if (!g.sell_batch.mixed_stations &&
            g.sell_batch.station >= 0 &&
            g.sell_batch.station < MAX_STATIONS &&
            station_exists(&g.world.stations[g.sell_batch.station])) {
            paid_station = &g.world.stations[g.sell_batch.station];
        }
        const char *pay_cur = (paid_station && paid_station->currency_name[0])
            ? paid_station->currency_name : "credits";
        int head_len = 0;
        if (g.sell_batch.any_by_contract && paid_station) {
            head_len = snprintf(head, sizeof(head), "[ %s paid +%d %s",
                                paid_station->name, g.sell_batch.total_cr,
                                pay_cur);
        } else if (g.sell_batch.any_by_contract) {
            head_len = snprintf(head, sizeof(head), "[ stations paid +%d credits",
                                g.sell_batch.total_cr);
        } else {
            head_len = snprintf(head, sizeof(head), "[ +%d %s",
                                g.sell_batch.total_cr, pay_cur);
        }
        head_len = (int)strlen(head);
        int tail_off = 0;
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
            int n = g.sell_batch.grade_counts[gi];
            if (n <= 0) continue;
            tail_off += snprintf(tail + tail_off, sizeof(tail) - tail_off,
                                 "  %s x%d", mining_grade_label((mining_grade_t)gi), n);
            if (tail_off >= (int)sizeof(tail) - 8) break;
        }
        int total_cols = head_len + tail_off + 2; /* " ]" */
        float msg_w = (float)total_cols * cell;
        float msg_x0 = screen_w * 0.5f - msg_w * 0.5f;
        float msg_y = screen_h * 0.82f;

        /* Fade during the display tail (last 1s of display_timer). */
        float alpha = 1.0f;
        if (!g.sell_batch.active && g.sell_batch.display_timer < 1.0f)
            alpha = g.sell_batch.display_timer;
        if (alpha < 0.0f) alpha = 0.0f;

        uint8_t total_r = g.sell_batch.any_by_contract ? 255 : 200;
        uint8_t total_g = g.sell_batch.any_by_contract ? 210 : 220;
        uint8_t total_b = g.sell_batch.any_by_contract ?  60 : 230;
        float cur_x = msg_x0;

        /* Payout head: generic station sale or station-named contract pay. */
        sdtx_pos(cur_x / cell, msg_y / cell);
        sdtx_color4b(total_r, total_g, total_b, (uint8_t)(alpha * 255.0f));
        sdtx_puts(head);
        cur_x += (float)head_len * cell;

        /* Per-grade segments, each colored by mining_grade_rgb. */
        for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
            int n = g.sell_batch.grade_counts[gi];
            if (n <= 0) continue;
            char seg[32];
            int seg_len = snprintf(seg, sizeof(seg), "  %s x%d",
                                   mining_grade_label((mining_grade_t)gi), n);
            uint8_t gr, gg_, gb;
            mining_grade_rgb((mining_grade_t)gi, &gr, &gg_, &gb);
            sdtx_pos(cur_x / cell, msg_y / cell);
            sdtx_color4b(gr, gg_, gb, (uint8_t)(alpha * 255.0f));
            sdtx_puts(seg);
            cur_x += (float)seg_len * cell;
        }

        /* " ]" closer */
        sdtx_pos(cur_x / cell, msg_y / cell);
        sdtx_color4b(total_r, total_g, total_b, (uint8_t)(alpha * 255.0f));
        sdtx_puts(" ]");
    } else if (hud_should_draw_message_panel()) {
        float cell = 8.0f;
        bool is_hull_warn = (message_r == 255 && message_g == 60);
        uint8_t r8 = message_r, g8 = message_g, b8 = message_b;
        if (is_hull_warn) {
            float pulse = 0.5f + 0.5f * sinf((float)sapp_frame_count() * 0.06f);
            r8 = (uint8_t)(message_r * pulse);
            g8 = (uint8_t)(40 + 20 * pulse);
            b8 = (uint8_t)(40 + 10 * pulse);
        }
        /* Count non-empty wrapped lines. Subtitle anchors line 0 at 0.82
         * of screen height; extra lines go BELOW (to avoid collision with
         * docked UI above). Long station hails arrive as 2-4 lines. */
        int count = 0;
        for (int li = 0; li < HUD_MSG_LINES; li++) {
            if (message_lines[li][0] != '\0') count = li + 1;
        }
        float msg_y = screen_h * 0.82f;
        for (int li = 0; li < count; li++) {
            /* 512 (was 256): GCC -Werror=format-truncation traces the
             * message_label (32) + ": " + a wrapped line (HUD_MSG_LINE_CAP)
             * and decides 256 might overflow. Bumping silences the warning
             * without forcing an explicit length check. */
            char line_buf[512];
            if (li == 0 && message_label[0] != '\0')
                snprintf(line_buf, sizeof(line_buf), "%s :: %s",
                         message_label, message_lines[li]);
            else
                snprintf(line_buf, sizeof(line_buf), "%s", message_lines[li]);
            float w = (float)strlen(line_buf) * cell;
            float x = screen_w * 0.5f - w * 0.5f;
            float y = msg_y + (float)li * cell * 1.25f;
            sdtx_pos(x / cell, y / cell);
            sdtx_color3b(r8, g8, b8);
            sdtx_puts(line_buf);
        }
    }

    /* --- [E] context prompt — only when docked. [E] is always LAUNCH. --- */
    if (!g.death_cinematic.active && LOCAL_PLAYER.docked) {
        float ex = ui_text_pos(message_x + 16.0f);
        float ey = ui_text_pos(message_y + message_h + 6.0f);
        sdtx_pos(ex, ey);
        sdtx_color3b(PAL_TEXT_GREY);
        sdtx_puts("[E] launch");
    }

    /* Shared post-classify panels: MP indicator + alpha banner, nav
     * label, hail sigil, module inspect pane, signal-lost warning.
     * Same call also fires from the compact path so a narrow window
     * doesn't silently lose any of these. */
    hud_draw_shared_panels(screen_w, screen_h, sig_quality, compact);

    draw_station_services(&ui);
}
