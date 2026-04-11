/*
 * onboarding.c — Minimal first-run hints for Signal Space Miner.
 *
 * Three steps: FRACTURE, TRACTOR, HAIL.  After that, stations
 * take over progression through contextual hail responses.
 */
#include "client.h"
#include "station_voice.h"
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
        "(function(){"
        "var s=localStorage.getItem('signal_onboarding');"
        "if(!s)return 0;"
        "return parseInt(s,10)||0;"
        "})()"
    );
    /* Backward compat: old onboarding used bits 0-8 for 9 milestones.
     * If any old bits (3+) are set, player finished the old tutorial. */
    if (flags & ~0x7) {
        g.onboarding.fractured = true;
        g.onboarding.tractored = true;
        g.onboarding.hailed    = true;
        g.onboarding.complete  = true;
    } else {
        g.onboarding.fractured = (flags & (1 << 0)) != 0;
        g.onboarding.tractored = (flags & (1 << 1)) != 0;
        g.onboarding.hailed    = (flags & (1 << 2)) != 0;
        g.onboarding.complete  = g.onboarding.hailed;
    }
#endif
}

static void onboarding_save(void) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    if (g.onboarding.fractured) flags |= (1 << 0);
    if (g.onboarding.tractored) flags |= (1 << 1);
    if (g.onboarding.hailed)    flags |= (1 << 2);
    char js[80];
    snprintf(js, sizeof(js), "localStorage.setItem('signal_onboarding','%d')", flags);
    emscripten_run_script(js);
#endif
}

/* ------------------------------------------------------------------ */
/* Step completion                                                     */
/* ------------------------------------------------------------------ */

static void complete_step(bool *step) {
    if (*step) return;
    *step = true;
    onboarding_save();
}

void onboarding_mark_fractured(void) { complete_step(&g.onboarding.fractured); }
void onboarding_mark_tractored(void) { complete_step(&g.onboarding.tractored); }
void onboarding_mark_hailed(void) {
    complete_step(&g.onboarding.hailed);
    g.onboarding.complete = true;
}

/* ------------------------------------------------------------------ */
/* Hint text                                                           */
/* ------------------------------------------------------------------ */

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (g.onboarding.complete) return false;

    if (!g.onboarding.fractured) {
        snprintf(label, label_size, "SIGNAL");
        snprintf(message, message_size, "Hold [Space] on an asteroid to fracture it.");
        return true;
    }
    if (!g.onboarding.tractored) {
        snprintf(label, label_size, "SIGNAL");
        snprintf(message, message_size, "Fly through the debris. Press [R] to fire the tractor.");
        return true;
    }
    if (!g.onboarding.hailed) {
        snprintf(label, label_size, "SIGNAL");
        snprintf(message, message_size, "Press [H] near a station to hail it.");
        return true;
    }
    return false;
}
