/*
 * onboarding.c — First-run checklist for Signal Space Miner.
 *
 * Four steps in any order: MOVE, FRACTURE, TRACTOR, HAIL.
 * Shown as a persistent checklist until all are complete.
 * After that, stations take over via contextual hail responses.
 */
#include "client.h"
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
    /* Backward compat: old onboarding used bits 0-2 for 3 milestones.
     * If old bit 2 (hailed) is set, player finished the old tutorial. */
    if (flags & ~0xF) {
        g.onboarding.moved     = true;
        g.onboarding.fractured = true;
        g.onboarding.tractored = true;
        g.onboarding.hailed    = true;
        g.onboarding.complete  = true;
    } else {
        g.onboarding.moved     = (flags & (1 << 0)) != 0;
        g.onboarding.fractured = (flags & (1 << 1)) != 0;
        g.onboarding.tractored = (flags & (1 << 2)) != 0;
        g.onboarding.hailed    = (flags & (1 << 3)) != 0;
        g.onboarding.complete  = g.onboarding.moved &&
                                  g.onboarding.fractured &&
                                  g.onboarding.tractored &&
                                  g.onboarding.hailed;
    }
#endif
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

static void complete_step(bool *step) {
    if (*step) return;
    *step = true;
    g.onboarding.complete = g.onboarding.moved &&
                             g.onboarding.fractured &&
                             g.onboarding.tractored &&
                             g.onboarding.hailed;
    onboarding_save();
}

void onboarding_mark_moved(void)     { complete_step(&g.onboarding.moved); }
void onboarding_mark_fractured(void) { complete_step(&g.onboarding.fractured); }
void onboarding_mark_tractored(void) { complete_step(&g.onboarding.tractored); }
void onboarding_mark_hailed(void)    { complete_step(&g.onboarding.hailed); }

/* ------------------------------------------------------------------ */
/* Checklist hint                                                      */
/* ------------------------------------------------------------------ */

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (g.onboarding.complete) return false;

    snprintf(label, label_size, "SIGNAL");
    snprintf(message, message_size, "%s [W/A/S/D] move  %s [Space] fracture  %s [R] tractor  %s [H] hail",
        g.onboarding.moved     ? "+" : "-",
        g.onboarding.fractured ? "+" : "-",
        g.onboarding.tractored ? "+" : "-",
        g.onboarding.hailed    ? "+" : "-");
    return true;
}
