/*
 * onboarding.c — First-run guide objectives for Signal Space Miner.
 *
 * This is deliberately not a contract/quest generator. It owns the
 * bottom-right guide/SIGNAL text surface: first the local teaching loop
 * (launch, fly, fracture, tractor, scan), then the concrete next step
 * for whatever station contract or ready ship upgrade the player is
 * tracking.
 */
#include "client.h"
#include "contract_objective.h"
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
    /* Always start fresh — controls change between versions,
     * so the guide can re-teach bindings every session. */
}

static void onboarding_save(void) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    if (g.onboarding.moved)     flags |= (1 << 0);
    if (g.onboarding.fractured) flags |= (1 << 1);
    if (g.onboarding.tractored) flags |= (1 << 2);
    if (g.onboarding.hailed)    flags |= (1 << 3);
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
           g.onboarding.fractured &&
           g.onboarding.tractored &&
           g.onboarding.hailed;
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
void onboarding_mark_hailed(void) {
    complete_step(&g.onboarding.hailed);
}
void onboarding_mark_boosted(void) {
    complete_step(&g.onboarding.boosted);
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
             "GUIDE // LAUNCH FROM DOCK ::::: [E]");
    return true;
}

static bool guide_flight(char *message, size_t message_size) {
    if (g.onboarding.moved || LOCAL_PLAYER.docked) return false;
    snprintf(message, message_size,
             "GUIDE // FLIGHT CHECK ::::: [WASD] FLY TOWARD ROCKS");
    return true;
}

static bool guide_fracture(char *message, size_t message_size) {
    if (!g.onboarding.moved || g.onboarding.fractured || LOCAL_PLAYER.docked)
        return false;
    if (LOCAL_PLAYER.hover_asteroid >= 0 &&
        g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active) {
        snprintf(message, message_size,
                 "GUIDE // ROCK TARGETED ::::: [M] FRACTURE");
    } else {
        snprintf(message, message_size,
                 "GUIDE // AIM AT A LARGE ROCK ::::: [M] FRACTURE");
    }
    return true;
}

static bool guide_tractor(char *message, size_t message_size) {
    if (!g.onboarding.fractured || g.onboarding.tractored || LOCAL_PLAYER.docked)
        return false;
    if (LOCAL_PLAYER.nearby_fragments > 0) {
        snprintf(message, message_size,
                 "GUIDE // FRAGMENTS NEARBY ::::: HOLD [SPACE] TRACTOR");
    } else {
        snprintf(message, message_size,
                 "GUIDE // BREAK ROCKS INTO FRAGMENTS ::::: THEN [SPACE]");
    }
    return true;
}

static bool guide_scan(char *message, size_t message_size) {
    if (!g.onboarding.tractored || g.onboarding.hailed || LOCAL_PLAYER.docked)
        return false;
    float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
    if (sig > 0.0f) {
        snprintf(message, message_size,
                 "GUIDE // LOCAL SCAN READY ::::: [H] REVEAL IDS + WORK");
    } else {
        snprintf(message, message_size,
                 "GUIDE // RETURN TO SIGNAL ::::: [H] SCANS WHEN LINKED");
    }
    return true;
}

static const guide_objective_t GUIDE_OBJECTIVES[] = {
    { guide_launch },
    { guide_flight },
    { guide_fracture },
    { guide_tractor },
    { guide_scan },
};

bool contract_step_hint(char *message, size_t message_size) {
    if (message_size == 0) return false;
    message[0] = '\0';
    contract_objective_t objective;
    if (!contract_objective_for_tracked(&objective)) return false;
    snprintf(message, message_size, "%s", objective.message);
    return true;
}

static bool ship_upgrade_step_hint(char *message, size_t message_size) {
    if (message_size == 0) return false;
    contract_objective_t objective;
    if (!contract_objective_ready_upgrade(&objective)) return false;
    snprintf(message, message_size, "%s", objective.message);
    return true;
}

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (label_size > 0) label[0] = '\0';
    onboarding_refresh_complete();
    if (g.onboarding.complete) {
        /* One final system line, then station hails own station voice. */
        if (!g.onboarding.welcomed) {
            g.onboarding.welcomed = true;
            snprintf(message, message_size,
                     "GUIDE // LOOP COMPLETE ::::: [H] SCAN // LASER INSPECTS");
            return true;
        }
        if (contract_step_hint(message, message_size))
            return true;
        return ship_upgrade_step_hint(message, message_size);
    }

    /* Contextual, optional: boost is useful, but not part of completing the
     * first economy loop. Show it only when the weak-signal context exists. */
    if (!LOCAL_PLAYER.docked && g.onboarding.moved && !g.onboarding.boosted) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig > 0.0f && sig < SIGNAL_BAND_OPERATIONAL) {
            snprintf(message, message_size,
                     "GUIDE // LOW SIGNAL ::::: [SHIFT] BOOST TOWARD LINK");
            return true;
        }
    }

    if (contract_step_hint(message, message_size))
        return true;
    if (ship_upgrade_step_hint(message, message_size))
        return true;

    for (int i = 0; i < (int)(sizeof(GUIDE_OBJECTIVES) / sizeof(GUIDE_OBJECTIVES[0])); i++) {
        if (GUIDE_OBJECTIVES[i].format(message, message_size))
            return true;
    }

    if (message_size > 0) message[0] = '\0';
    return false;
}
