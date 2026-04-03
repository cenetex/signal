/*
 * hud.c -- HUD layout, drawing primitives, and the main HUD text
 * renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "net.h"
#include "onboarding.h"

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
    for (float scan_y = y + 10.0f; scan_y < (y + height - 10.0f); scan_y += spacing) {
        draw_segment(v2(x + 10.0f, scan_y), v2(x + width - 10.0f, scan_y), 0.08f, 0.14f, 0.20f, alpha);
    }
}

void draw_ui_corner_brackets(float x, float y, float width, float height, float r, float g0, float b, float alpha) {
    float arm = fminf(26.0f, fminf(width, height) * 0.16f);
    float inset = 3.0f;

    draw_segment(v2(x + inset, y + arm), v2(x + inset, y + inset), r, g0, b, alpha);
    draw_segment(v2(x + inset, y + inset), v2(x + arm, y + inset), r, g0, b, alpha);

    draw_segment(v2(x + width - arm, y + inset), v2(x + width - inset, y + inset), r, g0, b, alpha);
    draw_segment(v2(x + width - inset, y + inset), v2(x + width - inset, y + arm), r, g0, b, alpha);

    draw_segment(v2(x + inset, y + height - arm), v2(x + inset, y + height - inset), r, g0, b, alpha);
    draw_segment(v2(x + inset, y + height - inset), v2(x + arm, y + height - inset), r, g0, b, alpha);

    draw_segment(v2(x + width - arm, y + height - inset), v2(x + width - inset, y + height - inset), r, g0, b, alpha);
    draw_segment(v2(x + width - inset, y + height - inset), v2(x + width - inset, y + height - arm), r, g0, b, alpha);
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
    if ((text == NULL) || (text[0] == '\0')) {
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

    if (g.notice_timer > 0.0f) {
        snprintf(label, label_size, "NOTICE");
        snprintf(message, message_size, "%s", g.notice);
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if (onboarding_hint(label, label_size, message, message_size)) {
        *r = 114; *g0 = 230; *b = 255;
        return true;
    }

    if (g.placing_outpost && !LOCAL_PLAYER.docked) {
        vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
        vec2 target = v2_add(LOCAL_PLAYER.ship.pos, v2_scale(forward, 150.0f));
        bool valid = can_place_outpost(&g.world, target);
        int cost = (int)lroundf(OUTPOST_CREDIT_COST);
        int sig = (int)lroundf(signal_strength_at(&g.world, target) * 100.0f);
        snprintf(label, label_size, "OUTPOST");
        if (!valid) {
            snprintf(message, message_size, "Invalid position. Cost %d cr. Signal %d%%", cost, sig);
            *r = 255; *g0 = 100; *b = 100;
        } else if (LOCAL_PLAYER.ship.credits < OUTPOST_CREDIT_COST) {
            snprintf(message, message_size, "Need %d cr (have %d). Signal %d%%", cost, (int)lroundf(LOCAL_PLAYER.ship.credits), sig);
            *r = 255; *g0 = 221; *b = 119;
        } else {
            snprintf(message, message_size, "Place outpost for %d cr. Signal %d%%", cost, sig);
            *r = 130; *g0 = 255; *b = 235;
        }
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
            int tip_cycle = (int)(g.world.time / 5.0f) % 3;
            bool has_market = false;
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                if (station->inventory[c] > 0.5f) { has_market = true; break; }

            if (tip_cycle == 1 && has_market) {
                snprintf(label, label_size, "MARKET");
                snprintf(message, message_size, "Press F to buy ingots. Haul them to your outpost blueprints.");
            } else if (tip_cycle == 2 && station_has_module(station, MODULE_ORE_BUYER)) {
                snprintf(label, label_size, "TIP");
                snprintf(message, message_size, "Press 1 to sell ore. Press B to place an outpost blueprint.");
            } else {
                snprintf(label, label_size, "%s", station_role_name(station));
                if (station_has_module(station, MODULE_FURNACE)) {
                    snprintf(message, message_size, "Sell raw ore here, repair up, then head back into the belt.");
                } else if (station_has_module(station, MODULE_FRAME_PRESS)) {
                    snprintf(message, message_size, "Press F to buy ingots. Haul to blueprints.");
                } else {
                    snprintf(message, message_size, "Tune the laser or tractor, then get back on the run.");
                }
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
            snprintf(message, message_size, "Sweep through the debris cloud and let the tractor finish the pull.");
        } else {
            snprintf(message, message_size, "Close in on the fragments and let the tractor catch them.");
        }
        *r = 114;
        *g0 = 255;
        *b = 192;
        return true;
    }

    if ((LOCAL_PLAYER.hover_asteroid >= 0) && g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        snprintf(label, label_size, "TIP");
        snprintf(message, message_size, "Hold the beam steady, crack the rock down, then sweep the fragments.");
        *r = 164;
        *g0 = 177;
        *b = 205;
        return true;
    }

    snprintf(label, label_size, "TIP");
    snprintf(message, message_size, "Crack rocks, sweep fragments, and run raw ore back to the refinery.");
    *r = 164;
    *g0 = 177;
    *b = 205;
    return true;
}

/* ------------------------------------------------------------------ */
/* draw_hud_panels -- background panel geometry for the flight HUD     */
/* ------------------------------------------------------------------ */

void draw_hud_panels(void) {
    if (g.death_screen_timer > 0.0f) return;
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

    if (hud_should_draw_message_panel()) {
        get_hud_message_panel_rect(&message_x, &message_y, &message_w, &message_h);
        draw_ui_panel(message_x, message_y, message_w, message_h, 0.05f);
    }
}

/* ------------------------------------------------------------------ */
/* draw_hud -- the main HUD text layer                                 */
/* ------------------------------------------------------------------ */

void draw_hud(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();

    /* --- Death screen overlay --- */
    if (g.death_screen_timer > 0.0f) {
        /* Dark overlay */
        float alpha = fminf(g.death_screen_timer, 1.0f);
        sgl_begin_quads();
        sgl_c4f(0.0f, 0.0f, 0.0f, 0.7f * alpha);
        sgl_v2f(0.0f, 0.0f);
        sgl_v2f(screen_w, 0.0f);
        sgl_v2f(screen_w, screen_h);
        sgl_v2f(0.0f, screen_h);
        sgl_end();

        float cx = screen_w * 0.5f;
        float cy = screen_h * 0.5f;

        /* Title */
        sdtx_canvas(screen_w * 2.0f, screen_h * 2.0f);
        sdtx_origin(0.0f, 0.0f);

        const char *title = "SHIP DESTROYED";
        float title_w = (float)strlen(title) * 8.0f;
        sdtx_pos((cx - title_w * 0.5f) / 8.0f, (cy - 50.0f) / 8.0f);
        sdtx_color3b(255, 80, 60);
        sdtx_puts(title);

        /* Stats */
        float row = (cy - 20.0f) / 8.0f;
        float left = (cx - 100.0f) / 8.0f;
        sdtx_color3b(180, 180, 180);

        sdtx_pos(left, row);
        sdtx_printf("Ore mined:    %8.0f", g.death_ore_mined);
        row += 2.0f;

        sdtx_pos(left, row);
        sdtx_printf("Rocks broken: %8d", g.death_asteroids_fractured);
        row += 2.0f;

        sdtx_pos(left, row);
        sdtx_color3b(120, 200, 120);
        sdtx_printf("Credits earned:%7.0f", g.death_credits_earned);
        row += 2.0f;

        sdtx_pos(left, row);
        sdtx_color3b(200, 120, 120);
        sdtx_printf("Credits spent: %7.0f", g.death_credits_spent);
        row += 3.0f;

        /* Prompt */
        sdtx_color3b(100, 100, 100);
        const char *prompt = "respawning...";
        float prompt_w = (float)strlen(prompt) * 8.0f;
        sdtx_pos((cx - prompt_w * 0.5f) / 8.0f, row);
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

    int sig_pct = (int)lroundf(signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos) * 100.0f);

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
        sdtx_printf("%s // CR %d", LOCAL_PLAYER.docked ? "RUN" : "SHIP", credits);

        sdtx_pos(top_text_x, top_row_1);
        if (sig_pct < 30) sdtx_color3b(255, 100, 100);
        else if (sig_pct < 60) sdtx_color3b(255, 221, 119);
        else sdtx_color3b(203, 220, 248);
        sdtx_printf("H %d/%d  C %d/%d  SIG %d%%", hull_units, hull_capacity, cargo_units, cargo_capacity, sig_pct);

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
        } else if (LOCAL_PLAYER.nearby_fragments > 0) {
            sdtx_color3b(130, 255, 235);
            if (LOCAL_PLAYER.tractor_fragments > 0) {
                sdtx_printf("TRACTOR // %d FRAG", LOCAL_PLAYER.tractor_fragments);
            } else {
                sdtx_printf("FRAGMENTS // %d", LOCAL_PLAYER.nearby_fragments);
            }
        } else if (cargo_units >= cargo_capacity) {
            sdtx_color3b(255, 221, 119);
            sdtx_puts("HOLD FULL // RETURN");
        } else {
            sdtx_color3b(169, 179, 204);
            sdtx_puts("FIELD CLEAR // SCAN");
        }

        if (hud_should_draw_message_panel()) {
            float message_text_x = ui_text_pos(message_x + 16.0f);
            float message_row_0 = ui_text_pos(message_y + 14.0f);
            float message_row_1 = ui_text_pos(message_y + 24.0f);
            float message_row_2 = ui_text_pos(message_y + 34.0f);

            sdtx_pos(message_text_x, message_row_0);
            sdtx_color3b(message_r, message_g, message_b);
            sdtx_puts(message_label);

            sdtx_pos(message_text_x, message_row_1);
            sdtx_color3b(232, 241, 255);
            sdtx_puts(message_line0);

            if (message_line1[0] != '\0') {
                sdtx_pos(message_text_x, message_row_2);
                sdtx_color3b(169, 179, 204);
                sdtx_puts(message_line1);
            }
        }

        draw_station_services(&ui);
        return;
    }

    sdtx_pos(top_text_x, top_row_0);
    sdtx_color3b(232, 241, 255);
    sdtx_puts(LOCAL_PLAYER.docked ? "RUN STATUS" : "SHIP STATUS");

    sdtx_pos(top_text_x, top_row_1);
    if (sig_pct < 30) sdtx_color3b(255, 100, 100);
    else if (sig_pct < 60) sdtx_color3b(255, 221, 119);
    else sdtx_color3b(203, 220, 248);
    sdtx_printf("CR %d  H %d/%d  C %d/%d  SIG %d%%", credits, hull_units, hull_capacity, cargo_units, cargo_capacity, sig_pct);

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
        sdtx_color3b(169, 179, 204);
        sdtx_puts("No target // line up a rock");
    }

    if (hud_should_draw_message_panel()) {
        float message_text_x = ui_text_pos(message_x + 16.0f);
        float message_row_0 = ui_text_pos(message_y + 16.0f);
        float message_row_1 = ui_text_pos(message_y + 30.0f);
        float message_row_2 = ui_text_pos(message_y + 42.0f);

        sdtx_pos(message_text_x, message_row_0);
        sdtx_color3b(message_r, message_g, message_b);
        sdtx_puts(message_label);

        sdtx_pos(message_text_x, message_row_1);
        sdtx_color3b(232, 241, 255);
        sdtx_puts(message_line0);

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
        sdtx_pos(info_x, ui_text_pos(18.0f));
        sdtx_color3b(70, 60, 50);
        sdtx_puts("ALPHA // world resets daily");
    }

    /* --- Edge pips --- */
    if (!LOCAL_PLAYER.docked) {
        float half_w = sapp_widthf() * 0.5f / fmaxf(1.0f, sapp_dpi_scale());
        float half_h = sapp_heightf() * 0.5f / fmaxf(1.0f, sapp_dpi_scale());

        /* Helper: draw a chevron pip at screen edge toward a world position */
        #define DRAW_PIP(target_pos, pr, pg, pb) do { \
            vec2 _to = v2_sub(target_pos, LOCAL_PLAYER.ship.pos); \
            float _dist = sqrtf(v2_len_sq(_to)); \
            bool _on = (fabsf(_to.x) < half_w * 0.85f) && (fabsf(_to.y) < half_h * 0.85f); \
            if (_dist > 50.0f && !_on) { \
                float _a = atan2f(-_to.y, _to.x); \
                float _m = 40.0f, _cx = screen_w*0.5f, _cy = screen_h*0.5f; \
                float _ex = _cx + cosf(_a)*(_cx-_m), _ey = _cy + sinf(_a)*(_cy-_m); \
                if (_ex < _m) _ex = _m; if (_ex > screen_w-_m) _ex = screen_w-_m; \
                if (_ey < _m) _ey = _m; if (_ey > screen_h-_m) _ey = screen_h-_m; \
                float _ar = 8.0f, _ca = cosf(_a), _sa = sinf(_a); \
                float _pulse = 0.6f + 0.3f * sinf(g.world.time * 3.0f); \
                sgl_defaults(); sgl_matrix_mode_projection(); sgl_load_identity(); \
                sgl_ortho(0,screen_w,screen_h,0,-1,1); sgl_matrix_mode_modelview(); sgl_load_identity(); \
                sgl_begin_lines(); sgl_c4f(pr, pg, pb, _pulse); \
                sgl_v2f(_ex+(-_ca*_ar-_sa*_ar*0.6f), _ey+(-_sa*_ar+_ca*_ar*0.6f)); sgl_v2f(_ex, _ey); \
                sgl_v2f(_ex, _ey); sgl_v2f(_ex+(-_ca*_ar+_sa*_ar*0.6f), _ey+(-_sa*_ar-_ca*_ar*0.6f)); \
                sgl_end(); \
            } \
        } while(0)

        /* Nearest station name — bottom left */
        {
            const station_t* nav_st = navigation_station_ptr();
            if (nav_st && nav_st->name[0] != '\0') {
                sdtx_pos(ui_text_pos(16.0f), ui_text_pos(screen_h - 20.0f));
                sdtx_color3b(100, 130, 120);
                sdtx_puts(nav_st->name);
            }
        }

        /* Nav pips: blueprint (yellow) and/or station (green) */
        if (g.nav_pip_active && g.nav_pip_is_blueprint)
            DRAW_PIP(g.nav_pip_pos, 1.0f, 0.87f, 0.20f);
        {
            /* Always show green pip to nearest station */
            const station_t* nav_st = navigation_station_ptr();
            if (nav_st)
                DRAW_PIP(nav_st->pos, 0.34f, 0.96f, 0.76f);
        }

        /* Target pip: nearest off-screen asteroid (red) */
        {
            float best_d = 1e18f;
            vec2 best_pos = LOCAL_PLAYER.ship.pos;
            bool found = false;
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                if (!g.world.asteroids[i].active) continue;
                if (asteroid_is_collectible(&g.world.asteroids[i])) continue;
                float d = v2_dist_sq(g.world.asteroids[i].pos, LOCAL_PLAYER.ship.pos);
                if (d < best_d) { best_d = d; best_pos = g.world.asteroids[i].pos; found = true; }
            }
            if (found)
                DRAW_PIP(best_pos, 0.9f, 0.25f, 0.2f);
        }

        /* Tracked contract pip (yellow) */
        if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
            contract_t *ct = &g.world.contracts[g.tracked_contract];
            if (ct->active) {
                vec2 target = (ct->action == CONTRACT_SUPPLY)
                    ? g.world.stations[ct->station_index].pos
                    : ct->target_pos;
                DRAW_PIP(target, 1.0f, 0.87f, 0.20f);
            } else {
                g.tracked_contract = -1; /* contract completed/expired */
            }
        }

        #undef DRAW_PIP
    }

    draw_station_services(&ui);
}
