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
#include "contract_objective.h"
#include "story_runtime.h"
#include "signal_model.h"  /* SIGNAL_BAND_OPERATIONAL threshold */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ------------------------------------------------------------------ */
/* Persistence (localStorage for browser, no-op for native)           */
/* ------------------------------------------------------------------ */

void onboarding_load(void) {
    if (g.onboarding.loaded) return;
    g.onboarding.loaded = true;
#ifdef __EMSCRIPTEN__
    int flags = emscripten_run_script_int(
        "(function(){var s=localStorage.getItem('signal_onboarding');"
        "if(!s)return 0;return parseInt(s,10)||0;})()");
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
#endif
    g.onboarding.complete = g.onboarding.moved &&
                            g.onboarding.hailed &&
                            g.onboarding.fractured &&
                            g.onboarding.tractored &&
                            g.onboarding.earned;
}

static void onboarding_save(void) {
#ifdef __EMSCRIPTEN__
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
    char js[80];
    snprintf(js, sizeof(js), "localStorage.setItem('signal_onboarding','%d')", flags);
    emscripten_run_script(js);
#endif
}

/* ------------------------------------------------------------------ */
/* Step completion                                                     */
/* ------------------------------------------------------------------ */

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
    if (g.onboarding.moved || !LOCAL_PLAYER.docked) return false;
    snprintf(message, message_size,
             "SIGNAL // GUIDE // LAUNCH FROM DOCK ::::: [E]");
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
    if (LOCAL_PLAYER.hover_asteroid >= 0 &&
        g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // ROCK TARGETED ::::: [M] FRACTURE");
    } else {
        snprintf(message, message_size,
                 "SIGNAL // GUIDE // AIM AT A LARGE ROCK ::::: [M] FRACTURE");
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

bool onboarding_hint(char *label, size_t label_size,
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
        if (story_runtime_hint(label, label_size,
                               message, message_size))
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
