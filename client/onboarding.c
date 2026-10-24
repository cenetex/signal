/*
 * onboarding.c — First-run guide objectives for Signal Space Miner.
 *
 * This is deliberately not a contract/quest generator. It owns the
 * bottom-right guide/SIGNAL text surface: first one honest physical economy
 * loop (launch, scan, fracture, tractor, physical smelt handoff), then the next
 * concrete step
 * for tracked station work, ready ship upgrades, or the highest-priority
 * real station contract available as the economy spine.
 */
#include "client.h"
#include "input.h"
#include "contract_objective.h"
#include "story_runtime.h"
#include "progress_store.h"
#include "signal_model.h"  /* SIGNAL_BAND_OPERATIONAL threshold */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ------------------------------------------------------------------ */
/* Progress follows the selected player and authority.                */
/* ------------------------------------------------------------------ */

void onboarding_load(void) {
    if (g.onboarding.loaded) return;
    g.onboarding.loaded = true;
    unsigned flags = client_progress_current().guide;
    g.onboarding.moved                = (flags & (1 << 0)) != 0;
    g.onboarding.fractured            = (flags & (1 << 1)) != 0;
    g.onboarding.tractored            = (flags & (1 << 2)) != 0;
    g.onboarding.threw                = (flags & (1 << 3)) != 0;
    g.onboarding.hailed               = (flags & (1 << 4)) != 0;
    g.onboarding.earned               = (flags & (1 << 5)) != 0;
    g.onboarding.docked_after_earning = (flags & (1 << 6)) != 0;
    g.onboarding.viewed_trade         = (flags & (1 << 7)) != 0;
    g.onboarding.boosted              = (flags & (1 << 8)) != 0;
    g.onboarding.welcomed             = (flags & (1 << 9)) != 0;
    g.onboarding.story_focus = (flags & CLIENT_PROGRESS_STORY_FOCUS) != 0;
    g.onboarding.complete = g.onboarding.moved &&
                            g.onboarding.hailed &&
                            g.onboarding.fractured &&
                            g.onboarding.tractored &&
                            g.onboarding.earned;
}

static void onboarding_save(void) {
    int flags = 0;
    if (g.onboarding.moved)     flags |= (1 << 0);
    if (g.onboarding.fractured) flags |= (1 << 1);
    if (g.onboarding.tractored) flags |= (1 << 2);
    if (g.onboarding.threw)     flags |= (1 << 3);
    if (g.onboarding.hailed)    flags |= (1 << 4);
    if (g.onboarding.earned)    flags |= (1 << 5);
    if (g.onboarding.docked_after_earning) flags |= (1 << 6);
    if (g.onboarding.viewed_trade) flags |= (1 << 7);
    if (g.onboarding.boosted)   flags |= (1 << 8);
    if (g.onboarding.welcomed)  flags |= (1 << 9);
    if (g.onboarding.story_focus) flags |= CLIENT_PROGRESS_STORY_FOCUS;
    client_progress_save_guide((uint16_t)flags);
}

/* ------------------------------------------------------------------ */
/* Step completion                                                     */
/* ------------------------------------------------------------------ */

static bool touch_controls;

void onboarding_set_touch_controls(bool touch) { touch_controls = touch; }
bool onboarding_uses_touch_controls(void) { return touch_controls; }
bool onboarding_story_focus(void) { return g.onboarding.story_focus; }

void onboarding_choose_story(bool story) {
    if (g.onboarding.story_focus == story) return;
    g.onboarding.story_focus = story;
    onboarding_save();
}

void onboarding_toggle_focus(void) {
    onboarding_choose_story(!g.onboarding.story_focus);
    set_notice(g.onboarding.story_focus
        ? "Guide: Story. [G] switches to Work."
        : "Guide: Work. [G] switches to Story.");
}

/* Translate the shared guide copy at its display boundary. The touch Tow
 * button toggles the held tractor input; keyboard Space remains a hold. */
void onboarding_format_controls(char *message, size_t size) {
    if (!touch_controls || !message || size == 0) return;
    const char *use = LOCAL_PLAYER.docked ? "[Launch]"
        : g.plan_mode_active ? (g.plan_target_station < 0 ? "[Lock]" : "[Place]")
        : LOCAL_PLAYER.ship->towed_scaffold >= 0 ? "[Place]" : "[Dock]";
    const char *service = LOCAL_PLAYER.docked && g.station_view == STATION_VIEW_WORK
        ? "[Deliver]" : "[Sell]";
    const char *focus = g.onboarding.story_focus ? "[Work guide]" : "[Story guide]";
    const struct { const char *key; const char *touch; } replacements[] = {
        {"HOLD [SPACE]", "TAP [Tow]"}, {"hold [Space]", "tap [Tow]"},
        {"tap [Space] release", "switch towing off, then double-tap [Release]"},
        {"TAP [SPACE]", "SWITCH TOW OFF, THEN DOUBLE-TAP [Tow]"},
        {"[WASD]", "[Accel] + [<] / [>]"},
        {"dock [E]", "dock with [Dock]"},
        {"then [E] to lock", "then [Lock] to lock"},
        {"Press E to dock.", "Tap [Dock]."},
        {"[SHIFT]", "[Boost]"}, {"[H]", "[Scan]"},
        {"[SPACE]", "[Tow]"}, {"[Space]", "[Tow]"},
        {"[TAB]", "[Panel]"},
        {"[B]", g.plan_mode_active ? "[Exit]" : "[Plan]"},
        {"[R]", LOCAL_PLAYER.docked ? "[Repair]" : "[Type]"},
        {"[G] Story", "[Story guide]"}, {"[G] Work", "[Work guide]"},
        {"AT DOCK [M]", "AT DOCK [Laser+]"},
        {"[E]", use}, {"[M]", "[Mine]"}, {"[S]", service},
        {"[C]", "[Hold+]"}, {"[T]", "[Tow+]"}, {"[G]", focus},
    };
    char source[512];
    snprintf(source, sizeof(source), "%s", message);
    size_t used = 0;
    for (const char *p = source; *p && used + 1 < size;) {
        const char *replacement = NULL;
        size_t consumed = 1;
        for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
            size_t n = strlen(replacements[i].key);
            if (strncmp(p, replacements[i].key, n) == 0) {
                replacement = replacements[i].touch;
                consumed = n;
                break;
            }
        }
        if (replacement) {
            for (const char *r = replacement; *r && used + 1 < size; r++)
                message[used++] = *r;
        } else message[used++] = *p;
        p += consumed;
    }
    message[used] = '\0';
}

static bool onboarding_core_complete(void) {
    return g.onboarding.moved &&
           g.onboarding.hailed &&
           g.onboarding.fractured &&
           g.onboarding.tractored &&
           g.onboarding.earned;
}

static void onboarding_refresh_complete(void) {
    g.onboarding.complete = onboarding_core_complete();
}

static void complete_step(bool *step) {
    if (*step) return;
    *step = true;
    onboarding_refresh_complete();
    onboarding_save();
}

void onboarding_mark_moved(void) {
    complete_step(&g.onboarding.moved);
}
void onboarding_mark_fractured(void) {
    complete_step(&g.onboarding.fractured);
}
void onboarding_mark_tractored(void) {
    complete_step(&g.onboarding.tractored);
}
void onboarding_mark_threw(void) {
    complete_step(&g.onboarding.threw);
}
void onboarding_mark_hailed(void) {
    complete_step(&g.onboarding.hailed);
}
void onboarding_mark_earned(void) {
    complete_step(&g.onboarding.earned);
}
void onboarding_mark_docked_after_earning(void) {
    if (!g.onboarding.earned) return;
    complete_step(&g.onboarding.docked_after_earning);
}
void onboarding_mark_viewed_trade(void) {
    if (!g.onboarding.docked_after_earning) return;
    complete_step(&g.onboarding.viewed_trade);
}
void onboarding_mark_boosted(void) {
    complete_step(&g.onboarding.boosted);
}

bool onboarding_autopilot_unlocked(void) {
    /* Persisted ship history also unlocks assist mode. This avoids making a
     * returning native player replay the browser-local guide state. */
    return g.onboarding.complete ||
           LOCAL_PLAYER.ship->stat_credits_earned > 0.01f;
}

/* ------------------------------------------------------------------ */
/* Guide objective formatters                                          */
/* ------------------------------------------------------------------ */

typedef bool (*guide_formatter_t)(char *message, size_t message_size);

typedef struct {
    guide_formatter_t format;
} guide_objective_t;

static bool guide_launch(char *message, size_t message_size) {
    if (!LOCAL_PLAYER.docked) return false;
    const char *next = !g.onboarding.hailed
        ? "SCAN [H], MINE A ROCK, THEN TOW ORE TO A FURNACE"
        : !g.onboarding.fractured
        ? "FOLLOW THE RED COMPASS PIP TO A ROCK YOUR LASER CAN MINE"
        : "TOW AN ORE FRAGMENT INTO A GLOWING FURNACE TO EARN LOCAL CREDITS";
    snprintf(message, message_size,
             "SIGNAL // GUIDE // LAUNCH FROM DOCK ::::: [E] // %s", next);
    return true;
}

static bool guide_flight(char *message, size_t message_size) {
    if (g.onboarding.moved || LOCAL_PLAYER.docked) return false;
    snprintf(message, message_size,
             "SIGNAL // GUIDE // FLIGHT CHECK ::::: [WASD] FLY TOWARD ROCKS");
    return true;
}

static bool guide_fracture(char *message, size_t message_size) {
    if (!g.onboarding.moved || !g.onboarding.hailed ||
        g.onboarding.fractured || LOCAL_PLAYER.docked)
        return false;
    int hovered = LOCAL_PLAYER.hover_asteroid;
    if (hovered >= 0 && hovered < MAX_ASTEROIDS &&
        mining_level_can_fracture_asteroid(LOCAL_PLAYER.ship->mining_level,
                                          &g.world.asteroids[hovered])) {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // ROCK TARGETED ::::: HOLD [M] TO FRACTURE");
    } else {
        /* Match the red compass pip's nearest compatible rock. */
        const asteroid_t *nearest = NULL;
        float best_distance = 1e30f;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            const asteroid_t *rock = &g.world.asteroids[i];
            if (!mining_level_can_fracture_asteroid(LOCAL_PLAYER.ship->mining_level, rock))
                continue;
            float distance = v2_dist_sq(LOCAL_PLAYER.ship->pos, rock->pos);
            if (distance < best_distance) {
                nearest = rock;
                best_distance = distance;
            }
        }
        if (nearest) {
            snprintf(message, message_size,
                     "SIGNAL // GUIDE // RED COMPASS PIP: %s %s (%s) // HOLD [M] TO MINE",
                     commodity_short_name(nearest->commodity),
                     asteroid_tier_name(nearest->tier), commodity_code(nearest->commodity));
        } else {
            snprintf(message, message_size,
                     "SIGNAL // GUIDE // EXPLORE FOR ROCKS ::::: [H] SCAN // RED PIP MARKS A MINEABLE ROCK");
        }
    }
    return true;
}

static bool guide_tractor(char *message, size_t message_size) {
    if (!g.onboarding.fractured || g.onboarding.tractored || LOCAL_PLAYER.docked)
        return false;
    if (LOCAL_PLAYER.nearby_fragments > 0) {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // FRAGMENTS NEARBY ::::: HOLD [SPACE] TRACTOR");
    } else {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // BREAK ROCKS INTO FRAGMENTS ::::: THEN HOLD [SPACE]");
    }
    return true;
}

static int guide_nearest_smelt_station(commodity_t ore) {
    int best = -1;
    float best_dist_sq = 1e30f;
    int station_count = g.world.station_count;
    if (station_count < 0) station_count = 0;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    for (int s = 0; s < station_count; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st) || !station_can_smelt(st, ore)) continue;
        float dist_sq = v2_dist_sq(LOCAL_PLAYER.ship->pos, st->pos);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            best = s;
        }
    }
    return best;
}

static int guide_first_towed_fragment(void) {
    int count = ship_towed_fragment_count(LOCAL_PLAYER.ship);
    for (int t = 0; t < count; t++) {
        int idx = LOCAL_PLAYER.ship->towed_fragments[t];
        if (idx >= 0 && idx < MAX_ASTEROIDS && g.world.asteroids[idx].active)
            return idx;
    }
    return -1;
}

static bool guide_scan(char *message, size_t message_size) {
    if (!g.onboarding.moved || g.onboarding.hailed || LOCAL_PLAYER.docked)
        return false;
    float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
    if (sig > 0.0f) {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // SCAN FOR USEFUL ROCKS ::::: [H]");
    } else {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // RETURN TO SIGNAL ::::: [H] SCANS WHEN LINKED");
    }
    return true;
}

static bool guide_deliver(char *message, size_t message_size) {
    if (!g.onboarding.tractored || g.onboarding.earned || LOCAL_PLAYER.docked)
        return false;
    if (LOCAL_PLAYER.ship->towed_count > 0) {
        int fragment_idx = guide_first_towed_fragment();
        commodity_t ore = fragment_idx >= 0
            ? g.world.asteroids[fragment_idx].commodity
            : COMMODITY_FERRITE_ORE;
        int station_idx = guide_nearest_smelt_station(ore);
        if (station_idx >= 0) {
            snprintf(message, message_size,
                     "SIGNAL // GUIDE // DELIVER ORE ::::: TOW IT TO THE GLOWING FURNACE AT %s",
                     g.world.stations[station_idx].name);
        } else {
            snprintf(message, message_size,
                     "SIGNAL // GUIDE // DELIVER ORE ::::: FIND A MATCHING GLOWING FURNACE");
        }
    } else {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // FIND AN ORE FRAGMENT ::::: HOLD [SPACE] TO TOW");
    }
    return true;
}

static const guide_objective_t GUIDE_OBJECTIVES[] = {
    { guide_launch },
    { guide_flight },
    { guide_scan },
    { guide_fracture },
    { guide_tractor },
    { guide_deliver },
};

bool contract_step_hint(char *message, size_t message_size) {
    if (message_size == 0) return false;
    message[0] = '\0';
    contract_objective_t objective;
    if (!contract_objective_for_tracked(&objective)) return false;
    snprintf(message, message_size, "%s", objective.message);
    return true;
}

static bool copy_objective_directive(const contract_objective_t *objective,
                                     char *label, size_t label_size,
                                     char *message, size_t message_size) {
    if (!objective || !objective->active) return false;
    if (label_size > 0)
        snprintf(label, label_size, "%s", objective->label);
    if (message_size > 0)
        snprintf(message, message_size, "%s", objective->body[0]
                 ? objective->body : objective->message);
    return true;
}

static bool tracked_contract_directive(char *label, size_t label_size,
                                       char *message, size_t message_size) {
    contract_objective_t objective;
    if (!contract_objective_for_tracked(&objective)) return false;
    return copy_objective_directive(&objective, label, label_size,
                                    message, message_size);
}

static bool recommended_contract_directive(char *label, size_t label_size,
                                           char *message, size_t message_size) {
    contract_objective_t objective;
    if (!contract_objective_for_recommended(&objective)) return false;
    return copy_objective_directive(&objective, label, label_size,
                                    message, message_size);
}

static bool ship_upgrade_directive(char *label, size_t label_size,
                                   char *message, size_t message_size) {
    contract_objective_t objective;
    if (!contract_objective_ready_upgrade(&objective)) return false;
    return copy_objective_directive(&objective, label, label_size,
                                    message, message_size);
}

static bool onboarding_hint_raw(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (label_size > 0) label[0] = '\0';
    onboarding_refresh_complete();
    if (g.onboarding.complete) {
        /* One final system line, then station hails own station voice. */
        if (!g.onboarding.welcomed) {
            g.onboarding.welcomed = true;
            onboarding_save();
            snprintf(message, message_size,
                     "SIGNAL // GUIDE // ECONOMY LOOP COMPLETE ::::: MONEY STAYS LOCAL // GOODS TRAVEL");
            return true;
        }
        if (g.onboarding.story_focus &&
            story_runtime_hint(label, label_size, message, message_size))
            return true;
        if (tracked_contract_directive(label, label_size,
                                       message, message_size))
            return true;
        if (ship_upgrade_directive(label, label_size,
                                   message, message_size))
            return true;
        if (recommended_contract_directive(label, label_size,
                                            message, message_size))
            return true;
        if (story_runtime_hint(label, label_size, message, message_size))
            return true;
        if (!g.onboarding.threw && !LOCAL_PLAYER.docked &&
            LOCAL_PLAYER.ship->towed_count > 0) {
            snprintf(message, message_size,
                     "SIGNAL // OPTIONAL // ROCK THROW ::::: STRETCH TETHER, TAP [SPACE]");
            return true;
        }
        return false;
    }

    for (int i = 0; i < (int)(sizeof(GUIDE_OBJECTIVES) / sizeof(GUIDE_OBJECTIVES[0])); i++) {
        if (GUIDE_OBJECTIVES[i].format(message, message_size))
            return true;
    }

    /* Contextual, optional: boost is useful, but not part of completing the
     * first economy loop. Show it only when the weak-signal context exists. */
    if (!LOCAL_PLAYER.docked && g.onboarding.moved && !g.onboarding.boosted) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship->pos);
        if (sig > 0.0f && sig < SIGNAL_BAND_OPERATIONAL) {
            snprintf(message, message_size,
                     "SIGNAL // GUIDE // LOW SIGNAL ::::: [SHIFT] BOOST TOWARD LINK");
            return true;
        }
    }

    if (tracked_contract_directive(label, label_size,
                                   message, message_size))
        return true;
    if (ship_upgrade_directive(label, label_size,
                               message, message_size))
        return true;
    if (recommended_contract_directive(label, label_size,
                                       message, message_size))
        return true;

    if (message_size > 0) message[0] = '\0';
    return false;
}

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    bool shown = onboarding_hint_raw(label, label_size, message, message_size);
    if (shown && g.onboarding.complete && message && message_size > 0) {
        size_t used = strlen(message);
        if (used < message_size)
            snprintf(message + used, message_size - used, " // [G] %s",
                     g.onboarding.story_focus ? "Work" : "Story");
    }
    return shown;
}
