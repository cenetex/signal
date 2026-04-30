/*
 * hud.c -- HUD layout, drawing primitives, and the main HUD text
 * renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "net.h"
#include "net_sync.h"
#include "onboarding.h"
#include "world_draw.h"
#include "avatar.h"
#include "mining_client.h"
#include "mining.h"  /* mining_alphanumeric_callsign — pubkey-derived */
#include "signal_model.h"
#include "palette.h"

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
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_token, token, 8) == 0)
            return st->ledger[i].balance;
    }
    return 0.0f;
}

/* Balance at the player's current docked/nearby station. */
float player_current_balance(void) {
    int st = LOCAL_PLAYER.docked ? LOCAL_PLAYER.current_station : LOCAL_PLAYER.nearby_station;
    return client_station_balance(st);
}

/* Forward decl — defined below near the other client_* helpers. */
static float client_pending_balance(void);

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
    HUD_ACTION_SCAN_PILOT,         /* int_a = pilot id, int_b = hull */
    HUD_ACTION_MINING,             /* claim window after fracture */
    HUD_ACTION_TOWING,             /* int_a = towed_count, int_b = tractor_active (1/0) */
    HUD_ACTION_TRACTOR_LOCK,       /* int_a = tractor_fragments, int_b = nearby_fragments */
    HUD_ACTION_TRACTOR_REACHING,   /* tractor active, no frag yet — int_b = nearby_fragments */
    HUD_ACTION_FRAGMENTS_NEARBY,   /* tractor inactive, frags in range — int_b = nearby_fragments */
    HUD_ACTION_HOLD_FULL,
    HUD_ACTION_PENDING_COLLECT,    /* int_a = total pending credits */
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
} hud_action_t;

static hud_action_t hud_classify_action(int cargo_units, int cargo_capacity, float sig_quality) {
    hud_action_t out = { HUD_ACTION_IDLE, 0, 0, NULL, NULL, 0, 0 };
    if (LOCAL_PLAYER.docked) { out.kind = HUD_ACTION_DOCKED; return out; }
    /* Target asteroid (laser-pointed). */
    if (LOCAL_PLAYER.hover_asteroid >= 0 &&
        g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        const asteroid_t *a = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
        out.kind = HUD_ACTION_TARGET_ASTEROID;
        out.int_a = (int)lroundf(a->hp);
        out.tier = (int)a->tier;
        out.commodity = (int)a->commodity;
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
        out.kind = HUD_ACTION_SCAN_PILOT;
        out.int_a = LOCAL_PLAYER.scan_target_index;
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
        return out;
    }
    if (LOCAL_PLAYER.nearby_fragments > 0) {
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
     * don't tease "collect" while H couldn't do anything. */
    float pending = (sig_quality >= 0.90f) ? client_pending_balance() : 0.0f;
    if (pending > 0.5f) {
        out.kind = HUD_ACTION_PENDING_COLLECT;
        out.int_a = (int)lroundf(pending);
        return out;
    }
    return out;
}

/* Compact renderer — short-form ALL CAPS at the bottom of the top panel. */
static void hud_render_action_compact(const hud_action_t *a, const char *dock_role) {
    switch (a->kind) {
    case HUD_ACTION_DOCKED:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("%s CONSOLE", dock_role);
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("TGT %s // %s // %d HP",
                    asteroid_tier_name((asteroid_tier_t)a->tier),
                    commodity_code((commodity_t)a->commodity),
                    a->int_a);
        return;
    case HUD_ACTION_SCAN_MODULE:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        if (a->str_b) sdtx_printf("SCAN %s // %s", a->str_a, a->str_b);
        else          sdtx_printf("SCAN %s // CORE", a->str_a);
        return;
    case HUD_ACTION_SCAN_NPC:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        sdtx_printf("SCAN NPC // %s",
                    (a->str_a && a->str_a[0] == 'm') ? "MINER" : "HAULER");
        return;
    case HUD_ACTION_SCAN_PILOT:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        sdtx_printf("SCAN PILOT // ID %d", a->int_a);
        return;
    case HUD_ACTION_MINING:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_puts("MINING... // CLAIM WINDOW");
        return;
    case HUD_ACTION_TOWING:
        sdtx_color3b(PAL_ACTIVE);
        if (a->int_b) sdtx_printf("TOWING %d // TRACTOR", a->int_a);
        else          sdtx_printf("TOWING %d // tap [Space] release", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_LOCK:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("TRACTOR // %d FRAG", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_REACHING:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("hold [Space] TRACTOR // %d", a->int_b);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        sdtx_color3b(PAL_TRACTOR_OFF);
        sdtx_printf("hold [Space] TRACTOR // %d nearby", a->int_b);
        return;
    case HUD_ACTION_HOLD_FULL:
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_puts("Hold full. Dock to sell.");
        return;
    case HUD_ACTION_PENDING_COLLECT:
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_printf("[H] collect %d", a->int_a);
        return;
    case HUD_ACTION_IDLE:
    default:
        sdtx_color3b(PAL_TEXT_MUTED);
        sdtx_puts("Nothing in range. Scan for rocks.");
        return;
    }
}

/* Wide renderer — full English at the bottom of the top panel. */
static void hud_render_action_wide(const hud_action_t *a, const station_t *current_station) {
    switch (a->kind) {
    case HUD_ACTION_DOCKED:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("%s console", station_role_name(current_station));
        return;
    case HUD_ACTION_TARGET_ASTEROID:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("Target %s // %s // %d hp",
                    asteroid_tier_kind((asteroid_tier_t)a->tier),
                    commodity_short_name((commodity_t)a->commodity),
                    a->int_a);
        return;
    case HUD_ACTION_SCAN_MODULE:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        if (a->str_b) sdtx_printf("Scan %s // %s", a->str_a, a->str_b);
        else          sdtx_printf("Scan %s // core hub", a->str_a);
        return;
    case HUD_ACTION_SCAN_NPC:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        sdtx_printf("Scan NPC %s // cargo %d", a->str_a, a->int_a);
        return;
    case HUD_ACTION_SCAN_PILOT:
        sdtx_color3b(PAL_SCAN_ACTIVE);
        sdtx_printf("Scan pilot %d // hull %d", a->int_a, a->int_b);
        return;
    case HUD_ACTION_MINING:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_puts("Mining... // claim window");
        return;
    case HUD_ACTION_TOWING:
        sdtx_color3b(PAL_ACTIVE);
        if (a->int_b) sdtx_printf("Towing %d // tractor on", a->int_a);
        else          sdtx_printf("Towing %d // tap [Space] to release", a->int_a);
        return;
    case HUD_ACTION_TRACTOR_LOCK:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("Tractor lock // %d frag%s", a->int_a, a->int_a == 1 ? "" : "s");
        return;
    case HUD_ACTION_TRACTOR_REACHING:
        sdtx_color3b(PAL_ACTIVE);
        sdtx_printf("Tractor reaching // %d nearby", a->int_b);
        return;
    case HUD_ACTION_FRAGMENTS_NEARBY:
        sdtx_color3b(PAL_TRACTOR_OFF);
        sdtx_printf("Hold [Space] tractor // %d nearby", a->int_b);
        return;
    case HUD_ACTION_HOLD_FULL:
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_puts("Hold full. Dock to sell.");
        return;
    case HUD_ACTION_PENDING_COLLECT:
        sdtx_color3b(PAL_ORE_AMBER);
        sdtx_printf("H to hail // collect %d cr", a->int_a);
        return;
    case HUD_ACTION_IDLE:
    default:
        sdtx_color3b(PAL_TEXT_MUTED);
        sdtx_puts("No target // line up a rock");
        return;
    }
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
    char segment[64];
    snprintf(segment, sizeof(segment), "ALPHA // v%s // frequent server resets // ", client_hash);
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
        if (!g.world.contracts[g.tracked_contract].active)
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
    /* Reset projection — texture pipeline can have stale state from
     * the world pass; same as the center hail overlay does. */
    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0, screen_w, screen_h, 0, -1, 1);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    sgl_enable_texture();
    sgl_texture((sg_view){ av->view_id }, (sg_sampler){ av->sampler_id });
    sgl_begin_quads();
    sgl_c4f(alpha, alpha, alpha, alpha);
    sgl_v2f_t2f(sx0,            sy0,            0.0f, 0.0f);
    sgl_v2f_t2f(sx0 + sig_size, sy0,            1.0f, 0.0f);
    sgl_v2f_t2f(sx0 + sig_size, sy0 + sig_size, 1.0f, 1.0f);
    sgl_v2f_t2f(sx0,            sy0 + sig_size, 0.0f, 1.0f);
    sgl_end();
    sgl_disable_texture();
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
    float px = fmaxf(16.0f, screen_w - 260.0f);
    float py = 60.0f;
    float cell = 8.0f;

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
    sdtx_pos(px / cell, (py + 60.0f) / cell);
    sdtx_color3b(PAL_TEXT_GREY);
    sdtx_puts("[E] close");
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

static void hud_draw_shared_panels(float screen_w, float screen_h, float sig_quality, bool compact) {
    hud_draw_alpha_banner_and_mp_indicator(screen_w, compact);
    hud_draw_nav_label(screen_w, screen_h);
    hud_draw_hail_sigil(screen_w, screen_h);
    hud_draw_module_inspect_pane(screen_w);
    hud_draw_signal_lost_warning(screen_w, screen_h, sig_quality);
    hud_draw_hit_indicator(screen_w, screen_h);
    hud_draw_kill_feed(screen_w, screen_h);
    /* Hit feedback vignette — drawn last so it sits above the HUD
     * readouts, but the inset rectangle in the middle leaves the
     * action row + flight readouts unobscured. */
    draw_damage_flash(screen_w, screen_h);
}

/* Pending credits across all stations — sum of every ledger entry the
 * local player owns. Singleplayer only: in MP, the server doesn't push
 * a per-station ledger and the wide/compact action rows treat this as
 * 0. Cheap O(stations × ledger) walk; reused per-frame by both HUD
 * variants instead of duplicating the loop. */
static float client_pending_balance(void) {
    if (!g.local_server.active) return 0.0f;
    float pending = 0.0f;
    for (int si = 0; si < MAX_STATIONS; si++) {
        const station_t *st = &g.world.stations[si];
        for (int li = 0; li < st->ledger_count; li++) {
            if (memcmp(st->ledger[li].player_token,
                       LOCAL_PLAYER.session_token, 8) == 0) {
                pending += st->ledger[li].balance;
            }
        }
    }
    return pending;
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

static const float UI_SCALE_TIGHT   = 1.85f;
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
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_height = compact ? 28.0f : HUD_BOTTOM_PANEL_HEIGHT;
    float top_height = compact ? HUD_TOP_PANEL_COMPACT_HEIGHT : HUD_TOP_PANEL_HEIGHT;
    float panel_width = fminf(compact ? STATION_PANEL_COMPACT_WIDTH : STATION_PANEL_WIDTH, screen_w - (hud_margin * 2.0f));
    float panel_height = fminf(compact ? STATION_PANEL_COMPACT_HEIGHT : STATION_PANEL_HEIGHT, screen_h - top_height - bottom_height - (hud_margin * 2.0f) - 20.0f);
    float panel_x = (screen_w - panel_width) * 0.5f;
    float min_y = hud_margin + top_height + 16.0f;
    float max_y = screen_h - hud_margin - bottom_height - panel_height - 14.0f;
    float panel_y = clampf((screen_h - panel_height) * 0.5f, min_y, fmaxf(min_y, max_y));

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

void draw_upgrade_pips(float x, float y, int level, float r, float g0, float b) {
    const float pip_w = 16.0f;
    const float pip_h = 7.0f;
    const float gap = 6.0f;

    for (int i = 0; i < SHIP_UPGRADE_MAX_LEVEL; i++) {
        float px = x + ((pip_w + gap) * (float)i);
        float alpha = i < level ? 0.95f : 0.35f;
        draw_rect_centered(v2(px + (pip_w * 0.5f), y + (pip_h * 0.5f)), pip_w * 0.5f, pip_h * 0.5f,
            i < level ? r : 0.12f,
            i < level ? g0 : 0.18f,
            i < level ? b : 0.24f,
            alpha);
        draw_rect_outline(v2(px + (pip_w * 0.5f), y + (pip_h * 0.5f)), pip_w * 0.5f, pip_h * 0.5f, 0.18f, 0.30f, 0.42f, 0.88f);
    }
}

void draw_service_card(float x, float y, float width, float height, float accent_r, float accent_g, float accent_b, bool hot) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    float border_a = hot ? 0.84f : 0.30f;
    float accent_a = hot ? 0.92f : 0.48f;
    float body_tint = hot ? 0.10f : 0.06f;
    float status_w = fminf(92.0f, width * 0.24f);

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.025f, body_tint, 0.09f, 0.94f);
    draw_rect_centered(v2(x + width - (status_w * 0.5f) - 8.0f, y + (height * 0.5f)), status_w * 0.5f, (height * 0.5f) - 5.0f, 0.02f, 0.05f, 0.08f, 0.92f);
    draw_rect_centered(v2(x + 4.0f, y + (height * 0.5f)), 3.0f, height * 0.5f, accent_r, accent_g, accent_b, accent_a);
    draw_ui_rule(x + 14.0f, x + fminf(90.0f, width * 0.20f), y + 8.0f, accent_r, accent_g, accent_b, hot ? 0.78f : 0.38f);
    draw_ui_rule(x + 10.0f, x + width - 10.0f, y + height - 2.0f, accent_r * 0.5f, accent_g * 0.5f, accent_b * 0.5f, hot ? 0.26f : 0.14f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, accent_r * 0.30f, accent_g * 0.30f, accent_b * 0.30f, border_a);
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
    return !LOCAL_PLAYER.docked || (g.notice_timer > 0.0f) || (g.collection_feedback_timer > 0.0f);
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


static bool build_hud_message(char* label, size_t label_size, char* message, size_t message_size, uint8_t* r, uint8_t* g0, uint8_t* b) {
    /* ================================================================
     * SYSTEM HINT PANEL — bottom-right, muted colors, no personality.
     * Shows the single most relevant system message or nothing.
     * Stations never speak here — they use the hail overlay.
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
        label[0] = '\0';
        snprintf(message, message_size, "Planning %s. R cycle, E place, B exit.",
            module_type_name((module_type_t)g.plan_type));
        *r = 130; *g0 = 180; *b = 200;
        return true;
    }

    /* Autopilot */
    if (LOCAL_PLAYER.autopilot_mode) {
        label[0] = '\0';
        snprintf(message, message_size, "Autopilot active. Move to cancel.");
        *r = 180; *g0 = 160; *b = 90;
        return true;
    }

    /* Onboarding */
    if (onboarding_hint(label, label_size, message, message_size)) {
        *r = 130; *g0 = 180; *b = 200;
        return true;
    }

    /* Scaffold tow */
    if (LOCAL_PLAYER.ship.towed_scaffold >= 0) {
        label[0] = '\0';
        snprintf(message, message_size, "Towing scaffold. E place, SPACE release.");
        *r = 160; *g0 = 150; *b = 100;
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
        *r = 120; *g0 = 150; *b = 120;
        return true;
    }

    /* Nothing to say. Panel is empty. */
    return false;
}

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
     * see distinct data, no aliasing. */
    static uint8_t pixels[HULL_FOG_LEVELS][HULL_FOG_TEX_SIZE * HULL_FOG_TEX_SIZE * 4];
    for (int level = 0; level < HULL_FOG_LEVELS; level++) {
        uint8_t *pix = pixels[level];
        float r0 = clear_radius[level];
        float r1 = 1.10f; /* fog reaches full alpha just past the corners */
        float pa = peak_alpha[level];
        for (int y = 0; y < HULL_FOG_TEX_SIZE; y++) {
            for (int x = 0; x < HULL_FOG_TEX_SIZE; x++) {
                float fx = ((float)x / (float)(HULL_FOG_TEX_SIZE - 1)) * 2.0f - 1.0f;
                float fy = ((float)y / (float)(HULL_FOG_TEX_SIZE - 1)) * 2.0f - 1.0f;
                float d = sqrtf(fx * fx + fy * fy);
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

/* Death-screen overlay — full-screen scrim + stats + leaderboard +
 * "[E] launch" prompt. Returns true if the overlay drew (caller skips
 * the regular flight HUD), false otherwise. */
static bool draw_death_overlay(float screen_w, float screen_h) {
    if (!g.death_cinematic.active && g.death_cinematic.menu_alpha <= 0.001f)
        return false;

    float menu_alpha = g.death_cinematic.menu_alpha;
    if (menu_alpha < 0.0f) menu_alpha = 0.0f;
    if (menu_alpha > 1.0f) menu_alpha = 1.0f;
    float scrim = 0.55f * menu_alpha;

    /* Dark scrim under the menu. */
    sgl_begin_quads();
    sgl_c4f(0.0f, 0.0f, 0.0f, scrim);
    sgl_v2f(0.0f, 0.0f);
    sgl_v2f(screen_w, 0.0f);
    sgl_v2f(screen_w, screen_h);
    sgl_v2f(0.0f, screen_h);
    sgl_end();
    float alpha = menu_alpha;

    /* 1:1 canvas so text fills the screen. */
    sdtx_canvas(screen_w, screen_h);
    sdtx_origin(0.0f, 0.0f);
    float cx = screen_w * 0.5f;
    float cy = screen_h * 0.5f;
    float cell = 8.0f;
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
                    bool is_me = (g.death_credits_earned > 0.5f
                                  && fabsf(g.highscores[i].credits_earned - g.death_credits_earned) < 0.5f);
                    if (is_me) sdtx_color4b(PAL_DEATH_EARNED, a8);
                    else       sdtx_color4b(PAL_TEXT_FADED,  a8);
                    sdtx_pos(left, row);
                    sdtx_printf("%2d. %-8s %8.0f", i + 1, cs, g.highscores[i].credits_earned);
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

    /* Prompt — RED, hard FLASH on/off */
    float flash = (sinf(g.world.time * 7.0f) > 0.0f) ? 1.0f : 0.25f;
    uint8_t pa = (uint8_t)(flash * (float)a8);
    sdtx_color4b(PAL_DEATH_PROMPT, pa);
    sdtx_centered_text(cx, row, cell, "[ E ] launch");

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

        sdtx_pos(top_text_x, top_row_0);
        sdtx_color3b(PAL_TEXT_PRIMARY);
        {
            /* Use the SESSION callsign (sent over the wire and echoed
             * back in highscores + remote ship labels) — NOT the local
             * mining-keypair callsign, which is cryptographic-only and
             * doesn't match what other players or the death-screen
             * leaderboard see. */
            const char *cs = net_local_callsign();
            const char *tag = (cs && cs[0] != '\0') ? cs : (LOCAL_PLAYER.docked ? "RUN" : "SHIP");
            if (LOCAL_PLAYER.docked)
                sdtx_printf("%s // %d %s", tag, credits, player_current_currency());
            else
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
        if (LOCAL_PLAYER.docked) {
            sdtx_color3b(PAL_SIGNAL_MINT);
            sdtx_printf("%s // E launch", dock_role);
        } else if (LOCAL_PLAYER.in_dock_range) {
            sdtx_color3b(PAL_SIGNAL_MINT);
            sdtx_puts("DOCK RING // E dock");
        } else {
            sdtx_color3b(PAL_NAV_BLUE);
            sdtx_printf("%s %d u // %d %s", nav_role, station_distance, bearing_degrees, bearing_mark);
        }

        sdtx_pos(top_text_x, top_row_3);
        {
            hud_action_t act = hud_classify_action(cargo_units, cargo_capacity, sig_quality);
            hud_render_action_compact(&act, dock_role);
        }

        if (hud_should_draw_message_panel()) {
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
        /* Pre-measure the composed line so we can center it. Same layout as
         * the text form flush_sell_batch used to emit:
         *   "[ +$N  [contract]  common xA  fine xB  ... ]" */
        char head[32], tail[160];
        int head_len = snprintf(head, sizeof(head), "[ +$%d", g.sell_batch.total_cr);
        int tail_off = 0;
        if (g.sell_batch.any_by_contract)
            tail_off += snprintf(tail + tail_off, sizeof(tail) - tail_off, "  contract");
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

        /* "[ +$N" */
        sdtx_pos(cur_x / cell, msg_y / cell);
        sdtx_color4b(total_r, total_g, total_b, (uint8_t)(alpha * 255.0f));
        sdtx_puts(head);
        cur_x += (float)head_len * cell;

        /* "  contract" marker (fixed yellow if present) */
        if (g.sell_batch.any_by_contract) {
            const char *tag = "  contract";
            sdtx_pos(cur_x / cell, msg_y / cell);
            sdtx_color4b(255, 210, 60, (uint8_t)(alpha * 255.0f));
            sdtx_puts(tag);
            cur_x += (float)strlen(tag) * cell;
        }

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
                snprintf(line_buf, sizeof(line_buf), "%s: %s",
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
