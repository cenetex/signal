/*
 * onboarding.c — First-run checklist for Signal Space Miner.
 *
 * Four steps in any order: MOVE, FRACTURE, TRACTOR, HAIL.
 * Shown as a persistent checklist until all are complete.
 * After that, stations take over via contextual hail responses.
 */
#include "client.h"
#include "station_voice.h"
#include "world_draw.h"
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
     * so the checklist re-teaches bindings every session. */
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
    if (g.onboarding.complete) {
        /* Show welcome message once, from nearest station */
        if (!g.onboarding.welcomed) {
            g.onboarding.welcomed = true;
            int ns = nearest_signal_station(LOCAL_PLAYER.ship.pos);
            if (ns >= 0 && ns < 3) {
                snprintf(label, label_size, "%s", g.world.stations[ns].name);
                snprintf(message, message_size, "%s",
                    STATION_ONBOARD[ns][VOICE_ONBOARD_COMPLETE]);
            } else {
                snprintf(label, label_size, "SIGNAL");
                snprintf(message, message_size, "Signal chain active. Welcome to the network.");
            }
            return true;
        }
        return false;
    }

    /* Subtitle-style: show the NEXT thing to learn, not a checklist */
    label[0] = '\0';
    if (!g.onboarding.moved)
        snprintf(message, message_size, "Use W A S D to fly your ship.");
    else if (!g.onboarding.fractured)
        snprintf(message, message_size, "Aim at a rock and press M to fracture it.");
    else if (!g.onboarding.tractored)
        snprintf(message, message_size, "Hold SPACE near fragments to tractor them.");
    else if (!g.onboarding.hailed)
        snprintf(message, message_size, "Press H in signal range to hail a station.");
    else
        snprintf(message, message_size, "");
    return true;
}
