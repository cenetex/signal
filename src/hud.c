/*
 * hud.c -- HUD layout, drawing primitives, and the main HUD text
 * renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "net.h"
#include "net_sync.h"
#include "onboarding.h"
#include "avatar.h"
#include "signal_model.h"

/* ------------------------------------------------------------------ */
/* UI scaling / layout helpers                                         */
/* ------------------------------------------------------------------ */

static const float UI_SCALE_TIGHT   = 1.85f;
static const float UI_SCALE_COMPACT = 1.60f;
static const float UI_SCALE_DEFAULT = 1.42f;
static const float UI_SCALE_WIDE    = 1.28f;

float ui_window_width(void) {
    return sapp_widthf() / fmaxf(1.0f, sapp_dpi_scale());
}

float ui_window_height(void) {
    return sapp_heightf() / fmaxf(1.0f, sapp_dpi_scale());
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

static void split_hud_message_lines(const char* text, int max_cols, char* line0, size_t line0_size, char* line1, size_t line1_size) {
    if (!text || (text[0] == '\0')) {
        line0[0] = '\0';
        line1[0] = '\0';
        return;
    }

    if (max_cols < 8) {
        max_cols = 8;
    }

    size_t len = strlen(text);
    if ((int)len <= max_cols) {
        snprintf(line0, line0_size, "%s", text);
        line1[0] = '\0';
        return;
    }

    int split = max_cols;
    while ((split > (max_cols / 2)) && (text[split] != ' ')) {
        split--;
    }
    if (split <= (max_cols / 2)) {
        split = max_cols;
    }

    snprintf(line0, line0_size, "%.*s", split, text);

    const char* rest = text + split;
    while (*rest == ' ') {
        rest++;
    }

    if ((int)strlen(rest) <= max_cols) {
        snprintf(line1, line1_size, "%s", rest);
    } else if (max_cols > 3) {
        snprintf(line1, line1_size, "%.*s...", max_cols - 3, rest);
    } else {
        snprintf(line1, line1_size, "%s", rest);
    }
}

static bool build_hud_message(char* label, size_t label_size, char* message, size_t message_size, uint8_t* r, uint8_t* g0, uint8_t* b) {
    int cargo_units = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    const station_t* station = current_station_ptr();

    /* Hull integrity warning — highest priority, replaces red vignette */
    if (!LOCAL_PLAYER.docked && g.death_screen_timer <= 0.0f) {
        float max_hull = ship_max_hull(&LOCAL_PLAYER.ship);
        if (max_hull > 0.0f) {
            float hp_ratio = LOCAL_PLAYER.ship.hull / max_hull;
            if (hp_ratio < 0.20f) {
                snprintf(label, label_size, "WARNING");
                int hp_pct = (int)lroundf(hp_ratio * 100.0f);
                snprintf(message, message_size, "[ HULL INTEGRITY FAILING ] %d%%", hp_pct);
                *r = 255; *g0 = 60; *b = 50;
                return true;
            }
        }
    }

    if (g.notice_timer > 0.0f) {
        snprintf(label, label_size, "NOTICE");
        snprintf(message, message_size, "%s", g.notice);
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    /* Persistent plan mode hint (existing or planned outpost) */
    if (g.plan_mode_active) {
        snprintf(label, label_size, "PLAN");
        snprintf(message, message_size,
            "[R] %s  [E] place slot  [B/Esc] exit",
            module_type_name((module_type_t)g.plan_type));
        *r = 130; *g0 = 220; *b = 255;
        return true;
    }

    /* Persistent autopilot indicator. Manual movement / mining cancels. */
    if (LOCAL_PLAYER.autopilot_mode) {
        snprintf(label, label_size, "AUTOPILOT");
        snprintf(message, message_size,
            "Mining loop active. Any movement key cancels. [O] toggle");
        *r = 255; *g0 = 200; *b = 90;
        return true;
    }

    if (onboarding_hint(label, label_size, message, message_size)) {
        *r = 114; *g0 = 230; *b = 255;
        return true;
    }


    if (g.collection_feedback_timer > 0.0f) {
        int recovered_ore = (int)lroundf(g.collection_feedback_ore);
        snprintf(label, label_size, "RECOVERY");
        if (g.collection_feedback_fragments > 0) {
            snprintf(message, message_size, "Recovered %d ore from %d fragment%s.", recovered_ore, g.collection_feedback_fragments, g.collection_feedback_fragments == 1 ? "" : "s");
        } else {
            snprintf(message, message_size, "Recovered %d ore.", recovered_ore);
        }
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if (LOCAL_PLAYER.docked) {
        if (station != NULL) {
            /* Rotate tips based on time */
            /* Cycle through context-specific tips. Each tip is one
             * concrete game action with the keybind in [brackets]. */
            int tip_cycle = (int)(g.world.time / 5.0f) % 4;
            bool has_shipyard = station_has_module(station, MODULE_SHIPYARD);
            bool has_market = false;
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                if (station->inventory[c] > 0.5f) { has_market = true; break; }

            if (tip_cycle == 0 && station_has_module(station, MODULE_ORE_BUYER)) {
                snprintf(label, label_size, "SELL");
                snprintf(message, message_size, "Press [1] to sell raw ore at this hopper.");
            } else if (tip_cycle == 1 && has_market) {
                snprintf(label, label_size, "MARKET");
                snprintf(message, message_size, "Press [F] to buy frames or ingots from this station.");
            } else if (tip_cycle == 2 && has_shipyard) {
                snprintf(label, label_size, "SHIPYARD");
                snprintf(message, message_size, "Open the SHIPYARD tab [Tab] then [1-9] to order a scaffold.");
            } else if (tip_cycle == 3) {
                snprintf(label, label_size, "LAUNCH");
                snprintf(message, message_size, "Press [E] to undock and head back to the belt.");
            } else {
                snprintf(label, label_size, "%s", station_role_name(station));
                snprintf(message, message_size, "Press [Tab] to switch panels.");
            }
            *r = 164;
            *g0 = 177;
            *b = 205;
            return true;
        }
        return false;
    }

    if ((cargo_units >= cargo_capacity) && (LOCAL_PLAYER.nearby_fragments > 0)) {
        snprintf(label, label_size, "WARN");
        snprintf(message, message_size, "Hold full. Fragments are still drifting outside the scoop.");
        *r = 255;
        *g0 = 221;
        *b = 119;
        return true;
    }

    if (cargo_units >= cargo_capacity) {
        snprintf(label, label_size, "WARN");
        snprintf(message, message_size, "Hold full. Run the ore home to the refinery.");
        *r = 255;
        *g0 = 221;
        *b = 119;
        return true;
    }

    if (LOCAL_PLAYER.docking_approach) {
        snprintf(label, label_size, "DOCKING");
        snprintf(message, message_size, "Tractor lock. Thrust W/S to cancel.");
        *r = 112; *g0 = 255; *b = 214;
        return true;
    }

    if (LOCAL_PLAYER.in_dock_range) {
        snprintf(label, label_size, "DOCK");
        snprintf(message, message_size, "Dock module in range. Press E.");
        *r = 112; *g0 = 255; *b = 214;
        return true;
    }

    if (LOCAL_PLAYER.nearby_fragments > 0) {
        snprintf(label, label_size, "TRACTOR");
        if (LOCAL_PLAYER.tractor_fragments > 0) {
            snprintf(message, message_size, "Tractor [R] is locked on. Drift through to scoop the fragments.");
        } else {
            snprintf(message, message_size, "Press [R] to fire the tractor and pull fragments in.");
        }
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        snprintf(label, label_size, "MINE");
        snprintf(message, message_size, "Hold [Space] to fire the mining laser. Crack the rock down to fragments.");
        *r = 164;
        *g0 = 177;
        *b = 205;
        return true;
    }

    /* Default tip cycle: rotate through actionable hints based on context */
    int idle_tip = (int)(g.world.time / 6.0f) % 4;
    if (LOCAL_PLAYER.ship.towed_scaffold >= 0) {
        snprintf(label, label_size, "PLACE");
        snprintf(message, message_size, "You're towing a scaffold. Press [B] to lock it into a ring slot.");
        *r = 255; *g0 = 221; *b = 119;
        return true;
    }
    switch (idle_tip) {
        case 0:
            snprintf(label, label_size, "MINE");
            snprintf(message, message_size, "Find an asteroid and hold [Space] to mine it.");
            break;
        case 1:
            snprintf(label, label_size, "TRACTOR");
            snprintf(message, message_size, "Press [R] to toggle your tractor and pull fragments in.");
            break;
        case 2:
            snprintf(label, label_size, "DOCK");
            snprintf(message, message_size, "Approach a station and press [E] to dock.");
            break;
        default:
            snprintf(label, label_size, "BUILD");
            snprintf(message, message_size, "Order a scaffold at any [Tab] SHIPYARD, then tow it to your outpost.");
            break;
    }
    *r = 164;
    *g0 = 177;
    *b = 205;
    return true;
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
    uint32_t blend_pip_id; /* sgl pipeline with alpha blending enabled */
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

/* Hull-integrity overlay: dark blood-red fog vignette that closes in as
 * hull drops. Crossfades between four pre-baked radial textures so each
 * threshold gets its own clear-hole radius. */
static void draw_hull_warning_overlay(void) {
    /* Drawn whenever the smoothed fog intensity is non-zero. While docked
     * the tint shifts to blue and the intensity recedes naturally as the
     * dock repairs the hull. */
    if (!hull_fog.initialized) return;

    /* Use the smoothed fog accumulator (advanced in advance_simulation_frame). */
    float damage = g.fog_intensity;
    if (damage < 0.0f) damage = 0.0f;
    if (damage > 1.0f) damage = 1.0f;
    if (damage <= 0.01f) return; /* spotless hull, no fog at all */

    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    float t = g.world.time;

    /* Continuous tier value in [0, HULL_FOG_LEVELS - 1]. Lerp the texture
     * pair around it so transitions are smooth across the whole range. */
    float tier = damage * (float)(HULL_FOG_LEVELS - 1);
    int t0 = (int)floorf(tier);
    if (t0 < 0) t0 = 0;
    if (t0 > HULL_FOG_LEVELS - 1) t0 = HULL_FOG_LEVELS - 1;
    int t1 = t0 + 1;
    if (t1 > HULL_FOG_LEVELS - 1) t1 = HULL_FOG_LEVELS - 1;
    float blend = tier - (float)t0;
    if (blend < 0.0f) blend = 0.0f;
    if (blend > 1.0f) blend = 1.0f;

    /* Lava-lamp wobble — slow, gentle, multi-frequency drift. No spikes,
     * no strobing. Bites slightly harder as damage rises. */
    float wob = 0.55f * sinf(t * 0.43f)
              + 0.30f * sinf(t * 0.71f + 1.3f)
              + 0.15f * sinf(t * 1.07f + 2.7f);
    float pulse = 0.92f + 0.08f * wob + 0.04f * damage;

    /* Dark blood red tint. Brightens slightly with damage. */
    float r = 0.20f + 0.25f * damage;
    float g = 0.005f + 0.01f * damage;
    float b = 0.01f + 0.02f * damage;

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
        sgl_c4f(r, g, b, a);
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
        sgl_c4f(r, g, b, a);
        sgl_v2f_t2f(0.0f,     0.0f,     0.0f, 0.0f);
        sgl_v2f_t2f(screen_w, 0.0f,     1.0f, 0.0f);
        sgl_v2f_t2f(screen_w, screen_h, 1.0f, 1.0f);
        sgl_v2f_t2f(0.0f,     screen_h, 0.0f, 1.0f);
        sgl_end();
    }

    sgl_disable_texture();
    sgl_pop_pipeline();
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

    draw_ui_panel(top_x, top_y, top_w, top_h, 0.03f);

    if (LOCAL_PLAYER.docked) {
        float panel_x = 0.0f;
        float panel_y = 0.0f;
        float panel_w = 0.0f;
        float panel_h = 0.0f;
        bool compact = ui_is_compact();
        station_ui_state_t ui = { 0 };
        build_station_ui_state(&ui);
        get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
        draw_ui_scrim(0.34f);
        draw_ui_panel(panel_x, panel_y, panel_w, panel_h, 0.08f);

        float inner_x = panel_x + 18.0f;
        float inner_y = panel_y + 18.0f;
        float inner_w = panel_w - 36.0f;

        /* Header rule below station name */
        draw_ui_rule(inner_x, panel_x + panel_w - 18.0f, inner_y + 26.0f, 0.14f, 0.26f, 0.38f, 0.70f);

        /* Tab bar */
        float tab_y = inner_y + 32.0f;
        float tab_h = compact ? 16.0f : 20.0f;
        station_tab_t visible_tabs[STATION_TAB_COUNT];
        int tab_count = 0;
        visible_tabs[tab_count++] = STATION_TAB_STATUS;
        visible_tabs[tab_count++] = STATION_TAB_MARKET;
        visible_tabs[tab_count++] = STATION_TAB_CONTRACTS;
        const station_t *cur_st = current_station_ptr();
        if (cur_st && station_has_module(cur_st, MODULE_SHIPYARD)) {
            visible_tabs[tab_count++] = STATION_TAB_SHIPYARD;
        }
        float tab_w = fminf(inner_w / (float)tab_count, 120.0f);

        for (int t = 0; t < tab_count; t++) {
            float tx = inner_x + (float)t * tab_w;
            bool active = (g.station_tab == visible_tabs[t]);
            float accent_a = active ? 0.92f : 0.20f;
            if (active) {
                draw_rect_centered(v2(tx + tab_w * 0.5f, tab_y + tab_h * 0.5f),
                    tab_w * 0.5f, tab_h * 0.5f, 0.06f, 0.12f, 0.18f, 0.95f);
            }
            draw_ui_rule(tx + 4.0f, tx + tab_w - 4.0f, tab_y + tab_h - 2.0f,
                0.30f, 0.85f, 1.0f, accent_a);
        }

        /* Tab content area */
        float content_y = tab_y + tab_h + 8.0f;
        float strip_h = compact ? 32.0f : 38.0f;
        float content_h = panel_y + panel_h - 18.0f - content_y - strip_h;
        draw_ui_panel(inner_x, content_y, inner_w, content_h, 0.03f);

        /* Ship status strip -- always visible below the content area */
        {
            float strip_y = panel_y + panel_h - (compact ? 32.0f : 38.0f);
            float meter_x = inner_x + 4.0f;
            float meter_w = compact ? 80.0f : 100.0f;
            float pip_x = meter_x + meter_w * 2.0f + 36.0f;
            /* Hull + cargo meters side by side */
            draw_ui_meter(meter_x, strip_y + 4.0f, meter_w, 10.0f,
                LOCAL_PLAYER.ship.hull / ship_max_hull(&LOCAL_PLAYER.ship), 0.96f, 0.54f, 0.28f);
            draw_ui_meter(meter_x + meter_w + 8.0f, strip_y + 4.0f, meter_w, 10.0f,
                ship_total_cargo(&LOCAL_PLAYER.ship) / fmaxf(1.0f, ship_cargo_capacity(&LOCAL_PLAYER.ship)), 0.26f, 0.90f, 0.72f);
            /* Upgrade pips inline */
            if (!compact) {
                draw_upgrade_pips(pip_x, strip_y + 2.0f, LOCAL_PLAYER.ship.mining_level, 0.34f, 0.88f, 1.0f);
                draw_upgrade_pips(pip_x, strip_y + 14.0f, LOCAL_PLAYER.ship.tractor_level, 0.42f, 1.0f, 0.86f);
                draw_upgrade_pips(pip_x, strip_y + 26.0f, LOCAL_PLAYER.ship.hold_level, 0.50f, 0.82f, 1.0f);
            }
        }
    }

    /* Message panel background removed — text only now */
}

/* ------------------------------------------------------------------ */
/* draw_hud -- the main HUD text layer                                 */
/* ------------------------------------------------------------------ */

void draw_hud(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();

    /* --- Death screen overlay (driven by the death cinematic) --- */
    if (g.death_cinematic.active || g.death_cinematic.menu_alpha > 0.001f) {
        float menu_alpha = g.death_cinematic.menu_alpha;
        if (menu_alpha < 0.0f) menu_alpha = 0.0f;
        if (menu_alpha > 1.0f) menu_alpha = 1.0f;
        float scrim = 0.55f * menu_alpha;

        /* Dark scrim under the menu */
        sgl_begin_quads();
        sgl_c4f(0.0f, 0.0f, 0.0f, scrim);
        sgl_v2f(0.0f, 0.0f);
        sgl_v2f(screen_w, 0.0f);
        sgl_v2f(screen_w, screen_h);
        sgl_v2f(0.0f, screen_h);
        sgl_end();
        float alpha = menu_alpha;

        /* Use 1:1 canvas so text fills the screen */
        sdtx_canvas(screen_w, screen_h);
        sdtx_origin(0.0f, 0.0f);
        float cx = screen_w * 0.5f;
        float cy = screen_h * 0.5f;
        float cell = 8.0f;
        uint8_t a8 = (uint8_t)(alpha * 255.0f);

        /* Title */
        const char *title = "SHIP DESTROYED";
        float title_w = (float)strlen(title) * cell;
        sdtx_pos((cx - title_w * 0.5f) / cell, (cy - 60.0f) / cell);
        sdtx_color4b(255, 80, 60, a8);
        sdtx_puts(title);

        /* Stats */
        float row = (cy - 16.0f) / cell;
        float left = (cx - 110.0f) / cell;
        sdtx_color4b(180, 180, 180, a8);

        sdtx_pos(left, row);
        sdtx_printf("Ore mined:     %8.0f", g.death_ore_mined);
        row += 2.5f;

        sdtx_pos(left, row);
        sdtx_printf("Rocks broken:  %8d", g.death_asteroids_fractured);
        row += 2.5f;

        sdtx_pos(left, row);
        sdtx_color4b(120, 200, 120, a8);
        sdtx_printf("Credits earned:%8.0f", g.death_credits_earned);
        row += 2.5f;

        sdtx_pos(left, row);
        sdtx_color4b(200, 120, 120, a8);
        sdtx_printf("Credits spent: %8.0f", g.death_credits_spent);
        row += 4.0f;

        /* Prompt — RED, hard FLASH on/off */
        float flash = (sinf(g.world.time * 7.0f) > 0.0f) ? 1.0f : 0.25f;
        uint8_t pa = (uint8_t)(flash * (float)a8);
        sdtx_color4b(255, 30, 20, pa);
        const char *prompt = "[ E ] launch";
        float prompt_w = (float)strlen(prompt) * cell;
        sdtx_pos((cx - prompt_w * 0.5f) / cell, row);
        sdtx_puts(prompt);

        return; /* skip normal HUD */
    }
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
    char message_label[16] = { 0 };
    char message_text[160] = { 0 };
    char message_line0[96] = { 0 };
    char message_line1[96] = { 0 };
    uint8_t message_r = 164;
    uint8_t message_g = 177;
    uint8_t message_b = 205;
    int hull_units = (int)lroundf(LOCAL_PLAYER.ship.hull);
    int hull_capacity = (int)lroundf(ship_max_hull(&LOCAL_PLAYER.ship));

    /* --- Low HP warning: pulsing red text in message area instead of vignette --- */
    /* (hull warning state is used by build_hud_message to show HULL INTEGRITY FAILING) */
    int cargo_units = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
    int credits = (int)lroundf(LOCAL_PLAYER.ship.credits);
    int cargo_capacity = (int)lroundf(ship_cargo_capacity(&LOCAL_PLAYER.ship));
    int payout_preview = (int)lroundf(station_cargo_sale_value(&LOCAL_PLAYER.ship, current_station_ptr()));
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
        split_hud_message_lines(message_text, message_cols, message_line0, sizeof(message_line0), message_line1, sizeof(message_line1));
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
        sdtx_color3b(232, 241, 255);
        {
            const char *cs = net_local_callsign();
            const char *tag = (cs && cs[0] != '\0') ? cs : (LOCAL_PLAYER.docked ? "RUN" : "SHIP");
            sdtx_printf("%s // CR %d", tag, credits);
        }

        sdtx_pos(top_text_x, top_row_1);
        sdtx_color3b(203, 220, 248);
        sdtx_printf("H %d/%d  C %d/%d  ", hull_units, hull_capacity, cargo_units, cargo_capacity);
        sdtx_color3b(sig_r, sig_g, sig_b);
        sdtx_printf("%s %d%%", sig_band, sig_pct);
        if (sig_quality < SIGNAL_BAND_OPERATIONAL) {
            int mine_pct = (int)lroundf(signal_mining_efficiency(sig_quality) * 100.0f);
            sdtx_printf(" M%d%%", mine_pct);
        }

        sdtx_pos(top_text_x, top_row_2);
        if (LOCAL_PLAYER.docked) {
            sdtx_color3b(112, 255, 214);
            sdtx_printf("%s // E launch", dock_role);
        } else if (LOCAL_PLAYER.in_dock_range) {
            sdtx_color3b(112, 255, 214);
            sdtx_puts("DOCK RING // E dock");
        } else {
            sdtx_color3b(199, 222, 255);
            sdtx_printf("%s %d u // %d %s", nav_role, station_distance, bearing_degrees, bearing_mark);
        }

        sdtx_pos(top_text_x, top_row_3);
        if (LOCAL_PLAYER.docked) {
            sdtx_color3b(130, 255, 235);
            if (station_has_service(STATION_SERVICE_ORE_BUYER)) {
                if (cargo_units > 0) {
                    sdtx_printf("ORE BOARD // HAUL %d", payout_preview);
                } else {
                    sdtx_puts("ORE BOARD // HOLD EMPTY");
                }
            } else {
                sdtx_printf("%s CONSOLE", dock_role);
            }
        } else if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
            const asteroid_t* asteroid = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
            int integrity_left = (int)lroundf(asteroid->hp);
            sdtx_color3b(130, 255, 235);
            sdtx_printf("TGT %s // %s // %d HP", asteroid_tier_name(asteroid->tier), commodity_code(asteroid->commodity), integrity_left);
        } else if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 1) {
            const station_t *st = &g.world.stations[LOCAL_PLAYER.scan_target_index];
            sdtx_color3b(100, 180, 255);
            if (LOCAL_PLAYER.scan_module_index >= 0) {
                const station_module_t *m = &st->modules[LOCAL_PLAYER.scan_module_index];
                sdtx_printf("SCAN %s // %s", st->name, module_type_name(m->type));
            } else {
                sdtx_printf("SCAN %s // CORE", st->name);
            }
        } else if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 2) {
            const npc_ship_t *npc = &g.world.npc_ships[LOCAL_PLAYER.scan_target_index];
            sdtx_color3b(100, 180, 255);
            sdtx_printf("SCAN NPC // %s", npc->role == NPC_ROLE_MINER ? "MINER" : "HAULER");
        } else if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 3) {
            sdtx_color3b(100, 180, 255);
            sdtx_printf("SCAN PILOT // ID %d", LOCAL_PLAYER.scan_target_index);
        } else if (LOCAL_PLAYER.ship.towed_count > 0) {
            sdtx_color3b(130, 255, 235);
            sdtx_printf("TOWING %d // [R] release", LOCAL_PLAYER.ship.towed_count);
        } else if (LOCAL_PLAYER.nearby_fragments > 0) {
            if (LOCAL_PLAYER.ship.tractor_active) {
                sdtx_color3b(130, 255, 235);
                if (LOCAL_PLAYER.ship.towed_count > 0) {
                    sdtx_printf("TOWING %d // (R) OFF", LOCAL_PLAYER.ship.towed_count);
                } else if (LOCAL_PLAYER.tractor_fragments > 0) {
                    sdtx_printf("TRACTOR ON // %d FRAG", LOCAL_PLAYER.tractor_fragments);
                } else {
                    sdtx_printf("(R) TRACTOR ON // %d", LOCAL_PLAYER.nearby_fragments);
                }
            } else {
                sdtx_color3b(180, 150, 120);
                sdtx_puts("(R) TRACTOR OFF");
            }
        } else if (cargo_units >= cargo_capacity) {
            sdtx_color3b(255, 221, 119);
            sdtx_puts("HOLD FULL // RETURN");
        } else {
            /* Check for pending credits from ledger (singleplayer) */
            float pending = 0.0f;
            if (g.local_server.active && sig_quality >= 0.90f) {
                for (int si = 0; si < MAX_STATIONS; si++) {
                    const station_t *st = &g.world.stations[si];
                    for (int li = 0; li < st->ledger_count; li++) {
                        if (memcmp(st->ledger[li].player_token,
                                   LOCAL_PLAYER.session_token, 8) == 0) {
                            pending += st->ledger[li].pending_credits;
                        }
                    }
                }
            }
            if (pending > 0.5f) {
                sdtx_color3b(255, 221, 119);
                sdtx_printf("H HAIL // %d CR", (int)lroundf(pending));
            } else {
                sdtx_color3b(169, 179, 204);
                sdtx_puts("FIELD CLEAR // SCAN");
            }
        }

        if (hud_should_draw_message_panel()) {
            /* Simple centered message line at bottom of screen */
            float cell = HUD_CELL * ui_text_zoom();
            char full_msg[256];
            if (message_line1[0] != '\0')
                snprintf(full_msg, sizeof(full_msg), "[ %s ]  %s %s", message_label, message_line0, message_line1);
            else
                snprintf(full_msg, sizeof(full_msg), "[ %s ]  %s", message_label, message_line0);
            float msg_w = (float)strlen(full_msg) * cell;
            float msg_x = (screen_w * 0.5f - msg_w * 0.5f) / cell;
            float msg_y = (screen_h - 32.0f) / cell;
            sdtx_pos(msg_x, msg_y);
            sdtx_color3b(message_r, message_g, message_b);
            sdtx_puts(full_msg);
        }

        draw_station_services(&ui);
        return;
    }

    sdtx_pos(top_text_x, top_row_0);
    sdtx_color3b(232, 241, 255);
    {
        const char *cs = net_local_callsign();
        const char *fallback = LOCAL_PLAYER.docked ? "RUN STATUS" : "SHIP STATUS";
        if (cs && cs[0] != '\0')
            sdtx_printf("%s // %s", cs, LOCAL_PLAYER.docked ? "DOCKED" : "FLIGHT");
        else
            sdtx_puts(fallback);
    }

    sdtx_pos(top_text_x, top_row_1);
    sdtx_color3b(203, 220, 248);
    sdtx_printf("CR %d  H %d/%d  C %d/%d  ", credits, hull_units, hull_capacity, cargo_units, cargo_capacity);
    sdtx_color3b(sig_r, sig_g, sig_b);
    sdtx_printf("%s %d%%", sig_band, sig_pct);
    if (sig_quality < SIGNAL_BAND_OPERATIONAL) {
        int mine_eff = (int)lroundf(signal_mining_efficiency(sig_quality) * 100.0f);
        int ctrl_eff = (int)lroundf(signal_control_scale(sig_quality) * 100.0f);
        sdtx_printf("  MINE %d%% CTRL %d%%", mine_eff, ctrl_eff);
    }

    sdtx_pos(top_text_x, top_row_2);
    if (LOCAL_PLAYER.docked && current_station) {
        sdtx_color3b(112, 255, 214);
        sdtx_printf("%s // docked // E launch", current_station->name);
    } else if (LOCAL_PLAYER.in_dock_range) {
        sdtx_color3b(112, 255, 214);
        sdtx_puts("Dock ring hot // E to dock");
    } else {
        sdtx_color3b(199, 222, 255);
        sdtx_printf("%s %d u // %d deg %s",
            navigation_station != NULL ? navigation_station->name : "Station",
            station_distance,
            bearing_degrees,
            bearing_side);
    }

    sdtx_pos(top_text_x, top_row_3);
    if (LOCAL_PLAYER.docked) {
        sdtx_color3b(130, 255, 235);
        if (station_has_service(STATION_SERVICE_ORE_BUYER)) {
            if (cargo_units > 0) {
                sdtx_printf("Ore board // haul %d", payout_preview);
            } else {
                sdtx_puts("Ore board // hold empty");
            }
        } else {
            sdtx_printf("%s console", station_role_name(current_station));
        }
    } else if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        const asteroid_t* asteroid = &g.world.asteroids[LOCAL_PLAYER.hover_asteroid];
        int integrity_left = (int)lroundf(asteroid->hp);
        sdtx_color3b(130, 255, 235);
        sdtx_printf("Target %s // %s // %d hp", asteroid_tier_kind(asteroid->tier), commodity_short_name(asteroid->commodity), integrity_left);
    } else if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 1) {
        const station_t *st = &g.world.stations[LOCAL_PLAYER.scan_target_index];
        sdtx_color3b(100, 180, 255);
        if (LOCAL_PLAYER.scan_module_index >= 0) {
            const station_module_t *m = &st->modules[LOCAL_PLAYER.scan_module_index];
            sdtx_printf("Scan %s // %s", st->name, module_type_name(m->type));
        } else {
            sdtx_printf("Scan %s // core hub", st->name);
        }
    } else if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 2) {
        const npc_ship_t *npc = &g.world.npc_ships[LOCAL_PLAYER.scan_target_index];
        int npc_cargo = 0;
        for (int ci = 0; ci < COMMODITY_COUNT; ci++)
            npc_cargo += (int)lroundf(npc->cargo[ci]);
        sdtx_color3b(100, 180, 255);
        sdtx_printf("Scan NPC %s // cargo %d", npc->role == NPC_ROLE_MINER ? "miner" : "hauler", npc_cargo);
    } else if (LOCAL_PLAYER.scan_active && LOCAL_PLAYER.scan_target_type == 3) {
        const server_player_t *other = &g.world.players[LOCAL_PLAYER.scan_target_index];
        int other_hull = (int)lroundf(other->ship.hull);
        sdtx_color3b(100, 180, 255);
        sdtx_printf("Scan pilot %d // hull %d", LOCAL_PLAYER.scan_target_index, other_hull);
    } else if (LOCAL_PLAYER.nearby_fragments > 0) {
        sdtx_color3b(130, 255, 235);
        if (LOCAL_PLAYER.tractor_fragments > 0) {
            sdtx_printf("Tractor lock // %d frag%s", LOCAL_PLAYER.tractor_fragments, LOCAL_PLAYER.tractor_fragments == 1 ? "" : "s");
        } else {
            sdtx_printf("Nearby fragments // %d", LOCAL_PLAYER.nearby_fragments);
        }
    } else if (cargo_units >= cargo_capacity) {
        sdtx_color3b(255, 221, 119);
        sdtx_puts("Hold full // return run");
    } else {
        /* Check for pending credits from ledger (singleplayer) */
        float pending_n = 0.0f;
        if (g.local_server.active && sig_quality >= 0.90f) {
            for (int si = 0; si < MAX_STATIONS; si++) {
                const station_t *st = &g.world.stations[si];
                for (int li = 0; li < st->ledger_count; li++) {
                    if (memcmp(st->ledger[li].player_token,
                               LOCAL_PLAYER.session_token, 8) == 0) {
                        pending_n += st->ledger[li].pending_credits;
                    }
                }
            }
        }
        if (pending_n > 0.5f) {
            sdtx_color3b(255, 221, 119);
            sdtx_printf("H to hail // collect %d cr", (int)lroundf(pending_n));
        } else {
            sdtx_color3b(169, 179, 204);
            sdtx_puts("No target // line up a rock");
        }
    }

    if (hud_should_draw_message_panel()) {
        float message_text_x = ui_text_pos(message_x + 16.0f);
        float message_row_0 = ui_text_pos(message_y + 16.0f);
        float message_row_1 = ui_text_pos(message_y + 30.0f);
        float message_row_2 = ui_text_pos(message_y + 42.0f);

        /* Pulse the label for hull warning */
        bool is_hull_warn_n = (message_r == 255 && message_g == 60 && message_b == 50);
        if (is_hull_warn_n) {
            float pulse = 0.5f + 0.5f * sinf((float)sapp_frame_count() * 0.06f);
            sdtx_pos(message_text_x, message_row_0);
            sdtx_color3b((uint8_t)(message_r * pulse), (uint8_t)(40 + 20 * pulse), (uint8_t)(40 + 10 * pulse));
            sdtx_puts(message_label);

            sdtx_pos(message_text_x, message_row_1);
            sdtx_color3b((uint8_t)(200 * pulse + 55), (uint8_t)(40 + 20 * pulse), (uint8_t)(40 + 10 * pulse));
            sdtx_puts(message_line0);
        } else {
            sdtx_pos(message_text_x, message_row_0);
            sdtx_color3b(message_r, message_g, message_b);
            sdtx_puts(message_label);

            sdtx_pos(message_text_x, message_row_1);
            sdtx_color3b(232, 241, 255);
            sdtx_puts(message_line0);
        }

        if (message_line1[0] != '\0') {
            sdtx_pos(message_text_x, message_row_2);
            sdtx_color3b(169, 179, 204);
            sdtx_puts(message_line1);
        }
    }

    /* --- Multiplayer HUD indicator + version --- */
    /* Version / connection status — top right */
    {
        float info_x = ui_text_pos(screen_w - (compact ? 100.0f : 140.0f));
        float info_y = ui_text_pos(8.0f);
        sdtx_pos(info_x, info_y);
#ifdef GIT_HASH
        const char* client_hash = GIT_HASH;
#else
        const char* client_hash = "dev";
#endif
        if (g.multiplayer_enabled && net_is_connected()) {
            const char* srv = net_server_hash();
            bool match = srv[0] != '\0' && strcmp(client_hash, srv) == 0;
            if (match) {
                /* Synced: show version */
                sdtx_color3b(80, 180, 120);
                sdtx_printf("v%s", client_hash);
            } else if (srv[0] == '\0') {
                /* Connecting */
                sdtx_color3b(220, 200, 60);
                sdtx_puts("connecting...");
            } else {
                /* Version mismatch — reloading (shown briefly before redirect) */
                sdtx_color3b(255, 160, 60);
                sdtx_puts("syncing...");
            }
        } else if (g.multiplayer_enabled) {
            sdtx_color3b(220, 200, 60);
            sdtx_puts("connecting...");
        } else {
            sdtx_color3b(80, 100, 80);
            sdtx_printf("v%s", client_hash);
        }
        /* Alpha banner: repeating ticker across the top */
        {
            float bw = sapp_widthf() / fmaxf(1.0f, sapp_dpi_scale());
            int cols = (int)(bw / 8.0f); /* sdtx char width ~8px */
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
            sdtx_color3b(180, 160, 60);
            sdtx_puts(banner);
        }
    }

    /* Nearest station name — bottom left */
    if (!LOCAL_PLAYER.docked) {
        const station_t* nav_st = navigation_station_ptr();
        if (nav_st && nav_st->name[0] != '\0') {
            sdtx_pos(ui_text_pos(16.0f), ui_text_pos(screen_h - 20.0f));
            sdtx_color3b(100, 130, 120);
            sdtx_puts(nav_st->name);
        }
        /* Expire tracked contract */
        if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
            if (!g.world.contracts[g.tracked_contract].active)
                g.tracked_contract = -1;
        }
    }

    /* Station hail overlay — portrait panel + radio-style message */
    if (g.hail_timer > 0.0f && g.hail_station[0]) {
        float alpha = fminf(g.hail_timer / 0.5f, 1.0f);
        float cell = 8.0f;
        float portrait_size = 64.0f;
        float pad = 12.0f;
        float hx = screen_w * 0.5f - 120.0f;
        float hy = screen_h * 0.30f;

        /* Portrait (if loaded) */
        const avatar_cache_t *av = avatar_get(g.hail_station_index);
        if (av && av->texture_valid) {
            float ax = hx - portrait_size - pad;
            float ay = hy - 4.0f;
            /* Set screen-space projection for textured quad */
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
            sgl_v2f_t2f(ax, ay, 0.0f, 0.0f);
            sgl_v2f_t2f(ax + portrait_size, ay, 1.0f, 0.0f);
            sgl_v2f_t2f(ax + portrait_size, ay + portrait_size, 1.0f, 1.0f);
            sgl_v2f_t2f(ax, ay + portrait_size, 0.0f, 1.0f);
            sgl_end();
            sgl_disable_texture();
            /* Scanline overlay */
            sgl_begin_quads();
            for (float sy = ay; sy < ay + portrait_size; sy += 3.0f) {
                sgl_c4f(0.0f, 0.0f, 0.0f, 0.15f * alpha);
                sgl_v2f(ax, sy);
                sgl_v2f(ax + portrait_size, sy);
                sgl_v2f(ax + portrait_size, sy + 1.0f);
                sgl_v2f(ax, sy + 1.0f);
            }
            sgl_end();
            /* Border */
            sgl_begin_lines();
            sgl_c4f(0.5f, 0.6f, 0.8f, 0.4f * alpha);
            sgl_v2f(ax, ay); sgl_v2f(ax + portrait_size, ay);
            sgl_v2f(ax + portrait_size, ay); sgl_v2f(ax + portrait_size, ay + portrait_size);
            sgl_v2f(ax + portrait_size, ay + portrait_size); sgl_v2f(ax, ay + portrait_size);
            sgl_v2f(ax, ay + portrait_size); sgl_v2f(ax, ay);
            sgl_end();
        }

        /* Text */
        sdtx_pos(hx / cell, hy / cell);
        sdtx_color3b((uint8_t)(130*alpha), (uint8_t)(200*alpha), (uint8_t)(255*alpha));
        sdtx_printf("// %s //", g.hail_station);

        if (g.hail_message[0]) {
            sdtx_pos(hx / cell, (hy + 14.0f) / cell);
            sdtx_color3b((uint8_t)(180*alpha), (uint8_t)(190*alpha), (uint8_t)(210*alpha));
            sdtx_puts(g.hail_message);
        }

        if (g.hail_credits > 0.5f) {
            sdtx_pos(hx / cell, (hy + 32.0f) / cell);
            sdtx_color3b((uint8_t)(130*alpha), (uint8_t)(255*alpha), (uint8_t)(235*alpha));
            sdtx_printf("+%d cr collected", (int)lroundf(g.hail_credits));
        }
    }

    /* Module inspect pane */
    if (g.inspect_station >= 0 && g.inspect_station < MAX_STATIONS &&
        g.inspect_module >= 0 && !LOCAL_PLAYER.docked) {
        const station_t *ist = &g.world.stations[g.inspect_station];
        if (station_exists(ist) && g.inspect_module < ist->module_count) {
            const station_module_t *im = &ist->modules[g.inspect_module];
            float px = screen_w - 260.0f;
            float py = 60.0f;
            float cell = 8.0f;

            sdtx_pos(px / cell, py / cell);
            sdtx_color3b(255, 221, 119);
            sdtx_printf("[ %s ]", module_type_name(im->type));

            sdtx_pos(px / cell, (py + 14.0f) / cell);
            sdtx_color3b(145, 160, 188);
            sdtx_printf("Station: %s", ist->name);

            sdtx_pos(px / cell, (py + 28.0f) / cell);
            sdtx_color3b(130, 200, 255);
            sdtx_printf("Ring %d  Slot %d", im->ring, im->slot);

            if (im->scaffold) {
                sdtx_pos(px / cell, (py + 42.0f) / cell);
                if (im->build_progress < 1.0f) {
                    int pct = (int)lroundf(im->build_progress * 100.0f);
                    sdtx_color3b(255, 140, 40); /* orange — awaiting material */
                    sdtx_printf("SUPPLYING: %d%%", pct);
                } else {
                    int pct = (int)lroundf((im->build_progress - 1.0f) * 100.0f);
                    sdtx_color3b(255, 180, 60); /* amber — build timer */
                    sdtx_printf("BUILDING: %d%%", pct);
                }
            } else {
                sdtx_pos(px / cell, (py + 42.0f) / cell);
                sdtx_color3b(130, 255, 235);
                sdtx_puts("ONLINE");
            }

            /* Close hint */
            sdtx_pos(px / cell, (py + 60.0f) / cell);
            sdtx_color3b(100, 110, 120);
            sdtx_puts("[E] close");
        } else {
            g.inspect_station = -1;
        }
    }

    draw_station_services(&ui);
}
