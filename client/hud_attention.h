/*
 * hud_attention.h -- Pure HUD attention policy.
 *
 * Keep the precedence and semantic-zoom budgets independent from rendering so
 * they can be tested without a graphics context.
 */
#ifndef SIGNAL_HUD_ATTENTION_H
#define SIGNAL_HUD_ATTENTION_H

#include <stdbool.h>

typedef enum {
    HUD_ATTENTION_FLIGHT = 0,
    HUD_ATTENTION_MESSAGE,
    HUD_ATTENTION_SCOREBOARD,
    HUD_ATTENTION_INSPECT,
    HUD_ATTENTION_STATION,
    HUD_ATTENTION_DEATH,
} hud_attention_surface_t;

typedef struct {
    bool death;
    bool docked;
    bool inspect;
    bool scoreboard;
    bool message;
} hud_attention_flags_t;

static inline hud_attention_surface_t
hud_attention_select(hud_attention_flags_t flags) {
    if (flags.death) return HUD_ATTENTION_DEATH;
    if (flags.docked) return HUD_ATTENTION_STATION;
    if (flags.inspect) return HUD_ATTENTION_INSPECT;
    if (flags.scoreboard) return HUD_ATTENTION_SCOREBOARD;
    if (flags.message) return HUD_ATTENTION_MESSAGE;
    return HUD_ATTENTION_FLIGHT;
}

static inline const char *
hud_attention_surface_name(hud_attention_surface_t surface) {
    switch (surface) {
    case HUD_ATTENTION_DEATH:      return "death";
    case HUD_ATTENTION_STATION:    return "station";
    case HUD_ATTENTION_INSPECT:    return "inspect";
    case HUD_ATTENTION_SCOREBOARD: return "scoreboard";
    case HUD_ATTENTION_MESSAGE:    return "message";
    case HUD_ATTENTION_FLIGHT:
    default:                       return "flight";
    }
}

static inline int hud_scan_asteroid_budget(float viewport_width) {
    /* viewport_width is the scaled HUD canvas, not raw window pixels. */
    if (viewport_width < 360.0f) return 4;
    if (viewport_width < 720.0f) return 6;
    return 8;
}

static inline int hud_scan_npc_budget(float viewport_width) {
    if (viewport_width < 360.0f) return 2;
    if (viewport_width < 720.0f) return 3;
    return 4;
}

#endif
