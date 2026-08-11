#include "legacy_recovery_ui.h"

#include "client.h"
#include "palette.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    RECOVERY_VIEW_LINE_CAP = 128,
    RECOVERY_VIEW_MAX_LINES = 5,
};

static void recovery_fit_text(
    const char *src,
    int max_chars,
    char *out,
    size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!src || max_chars <= 0) return;
    size_t len = strlen(src);
    size_t cap = (size_t)max_chars;
    if (cap >= out_size) cap = out_size - 1;
    if (len <= cap) {
        memcpy(out, src, len + 1);
        return;
    }
    if (cap <= 3) {
        memcpy(out, src, cap);
        out[cap] = '\0';
        return;
    }
    memcpy(out, src, cap - 3);
    memcpy(out + cap - 3, "...", 3);
    out[cap] = '\0';
}

static int recovery_wrap_text(
    const char *src,
    int max_chars,
    char lines[RECOVERY_VIEW_MAX_LINES][RECOVERY_VIEW_LINE_CAP])
{
    if (!src || !src[0] || max_chars <= 0)
        return 0;
    int count = 0;
    const char *cursor = src;
    while (*cursor && count < RECOVERY_VIEW_MAX_LINES) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) break;
        size_t remaining = strlen(cursor);
        size_t take = remaining < (size_t)max_chars
            ? remaining : (size_t)max_chars;
        if (remaining > take) {
            size_t split = take;
            while (split > 0 && cursor[split] != ' ')
                split--;
            if (split > 0) take = split;
        }
        if (take >= RECOVERY_VIEW_LINE_CAP)
            take = RECOVERY_VIEW_LINE_CAP - 1;
        memcpy(lines[count], cursor, take);
        lines[count][take] = '\0';
        count++;
        cursor += take;
        while (*cursor == ' ') cursor++;
    }
    if (*cursor && count > 0) {
        char *last = lines[count - 1];
        size_t len = strlen(last);
        if (len + 3 < RECOVERY_VIEW_LINE_CAP &&
            len + 3 <= (size_t)max_chars) {
            memcpy(last + len, "...", 4);
        } else if (len >= 3) {
            memcpy(last + len - 3, "...", 4);
        }
    }
    return count;
}

static void recovery_status_color(
    legacy_recovery_ui_semantic_t semantic)
{
    switch (semantic) {
    case LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS:
        sdtx_color3b(PAL_SIGNAL_MINT);
        break;
    case LEGACY_RECOVERY_UI_SEMANTIC_OFFER:
    case LEGACY_RECOVERY_UI_SEMANTIC_CONFIRMING:
        sdtx_color3b(PAL_NAV_BLUE);
        break;
    case LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED:
    case LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND:
        sdtx_color3b(PAL_SIGNAL_OPERATIONAL);
        break;
    default:
        sdtx_color3b(PAL_WARNING);
        break;
    }
}

static void recovery_draw_text(
    float x,
    float y,
    const char *text)
{
    sdtx_pos(ui_text_pos(x), ui_text_pos(y));
    sdtx_puts(text ? text : "");
}

void draw_legacy_recovery_ui(
    const legacy_recovery_ui_t *ui,
    uint32_t now_ms)
{
    if (!legacy_recovery_ui_visible(ui)) return;

    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    bool compact = ui_is_compact();
    float station_x = 0.0f;
    float station_y = 0.0f;
    float station_w = 0.0f;
    float station_h = 0.0f;
    get_station_panel_rect(
        &station_x, &station_y, &station_w, &station_h);

    float panel_w = fminf(station_w, compact ? 440.0f : 520.0f);
    float panel_h = fminf(station_h, compact ? 280.0f : 300.0f);
    float panel_x = (screen_w - panel_w) * 0.5f;
    float panel_y = (screen_h - panel_h) * 0.5f;
    if (panel_x < 6.0f) panel_x = 6.0f;
    if (panel_y < 6.0f) panel_y = 6.0f;

    draw_ui_scrim(0.82f);
    draw_ui_panel(panel_x, panel_y, panel_w, panel_h, 0.72f);
    draw_ui_rule(
        panel_x + 18.0f, panel_x + panel_w - 18.0f,
        panel_y + 56.0f, PAL_F_SIGNAL_MINT, 0.62f);

    sdtx_canvas(
        screen_w / ui_text_zoom(),
        screen_h / ui_text_zoom());
    sdtx_font(0);
    sdtx_origin(0.0f, 0.0f);
    sdtx_home();

    int max_chars = (int)floorf((panel_w - 36.0f) / 8.0f);
    if (max_chars < 8) max_chars = 8;
    char fit[RECOVERY_VIEW_LINE_CAP];
    recovery_fit_text(
        legacy_recovery_ui_title(ui), max_chars, fit, sizeof(fit));
    sdtx_color3b(PAL_TEXT_PRIMARY);
    recovery_draw_text(panel_x + 18.0f, panel_y + 22.0f, fit);

    recovery_fit_text(
        legacy_recovery_ui_status(ui), max_chars, fit, sizeof(fit));
    recovery_status_color(ui->semantic);
    recovery_draw_text(panel_x + 18.0f, panel_y + 40.0f, fit);

    float cursor_y = panel_y + 76.0f;
    if (ui->phase == LEGACY_RECOVERY_UI_OFFERED) {
        sdtx_color3b(PAL_SIGNAL_MINT);
        recovery_draw_text(
            panel_x + 18.0f, cursor_y,
            "AUTHORIZED CANDIDATE // OPAQUE");
        cursor_y += 22.0f;
    }

    char body_lines[RECOVERY_VIEW_MAX_LINES]
                   [RECOVERY_VIEW_LINE_CAP] = {{0}};
    int body_count = recovery_wrap_text(
        legacy_recovery_ui_body(ui), max_chars, body_lines);
    sdtx_color3b(PAL_TEXT_SECONDARY);
    for (int i = 0; i < body_count; i++) {
        recovery_draw_text(
            panel_x + 18.0f, cursor_y, body_lines[i]);
        cursor_y += 16.0f;
    }

    bool cramped = panel_h < 220.0f;
    if (!cramped) {
        cursor_y += 8.0f;
        char detail_lines[RECOVERY_VIEW_MAX_LINES]
                         [RECOVERY_VIEW_LINE_CAP] = {{0}};
        int detail_count = recovery_wrap_text(
            legacy_recovery_ui_detail(ui),
            max_chars, detail_lines);
        sdtx_color3b(PAL_TEXT_MUTED);
        for (int i = 0; i < detail_count; i++) {
            if (cursor_y > panel_y + panel_h - 58.0f)
                break;
            recovery_draw_text(
                panel_x + 18.0f, cursor_y, detail_lines[i]);
            cursor_y += 16.0f;
        }
    }

    char footer[RECOVERY_VIEW_LINE_CAP];
    if (ui->phase == LEGACY_RECOVERY_UI_OFFERED) {
        uint32_t seconds =
            legacy_recovery_ui_seconds_remaining(ui, now_ms);
        snprintf(
            footer, sizeof(footer),
            compact
                ? "[ENTER/A] RECOVER  [ESC/B] LEAVE  %us"
                : "[ENTER / A] RECOVER    [ESC / B] LEAVE UNTOUCHED    %us",
            (unsigned)seconds);
    } else if (ui->phase == LEGACY_RECOVERY_UI_CONFIRMING) {
        snprintf(
            footer, sizeof(footer),
            "CONFIRMATION SENT ONCE // WAITING FOR SERVER");
    } else {
        snprintf(
            footer, sizeof(footer),
            "AUTHORITATIVE RESULT // CONSOLE CLOSING");
    }
    recovery_fit_text(footer, max_chars, fit, sizeof(fit));
    sdtx_color3b(PAL_TEXT_FADED);
    recovery_draw_text(
        panel_x + 18.0f,
        panel_y + panel_h - 30.0f,
        fit);
}
