/*
 * onboarding.c — First-run checklist for Signal Space Miner.
 *
 * Five milestones in loose order: LAUNCH/MOVE, FRACTURE, TRACTOR,
 * HAIL, BOOST.
 * Shown as a persistent checklist until all are complete.
 * After that, stations take over via contextual hail responses.
 */
#include "client.h"
#include "station_voice.h"
#include "world_draw.h"
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
                             g.onboarding.hailed &&
                             g.onboarding.boosted;
    onboarding_save();
}

static void emit_onboarding_voice(int milestone) {
    /* No-op since the voice subsystem was removed. Kept as a stub so the
     * onboarding flow's call sites still compile and reads as a marker
     * for "this is where a station hail used to fire" — handy if the
     * line content is ever reused for HUD subtitles. */
    (void)milestone;
}

void onboarding_mark_moved(void) {
    if (!g.onboarding.moved) emit_onboarding_voice(VOICE_ONBOARD_LAUNCH);
    complete_step(&g.onboarding.moved);
}
void onboarding_mark_fractured(void) {
    if (!g.onboarding.fractured) emit_onboarding_voice(VOICE_ONBOARD_MINE);
    complete_step(&g.onboarding.fractured);
}
void onboarding_mark_tractored(void) {
    if (!g.onboarding.tractored) emit_onboarding_voice(VOICE_ONBOARD_COLLECT);
    complete_step(&g.onboarding.tractored);
}
void onboarding_mark_hailed(void) {
    if (!g.onboarding.hailed) emit_onboarding_voice(VOICE_ONBOARD_SELL);
    complete_step(&g.onboarding.hailed);
}
void onboarding_mark_boosted(void) {
    if (!g.onboarding.boosted) emit_onboarding_voice(VOICE_ONBOARD_UPGRADE);
    complete_step(&g.onboarding.boosted);
}

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

    /* Subtitle-style: show the next useful action, not a checklist. */
    label[0] = '\0';
    if (LOCAL_PLAYER.docked) {
        if (!g.onboarding.moved) {
            snprintf(message, message_size, "Press E to launch.");
            return true;
        }
        /* The station terminal has its own verb rows. Avoid showing
         * stale flight hints while the player is docked. */
        if (message_size > 0) message[0] = '\0';
        return false;
    }

    /* Contextual: if the player has left core signal and hasn't
     * discovered SHIFT yet, that teaching beats the normal queue. */
    if (g.onboarding.moved && !g.onboarding.boosted) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig > 0.0f && sig < SIGNAL_BAND_OPERATIONAL) {
            snprintf(message, message_size, "Signal degraded -- hold SHIFT to boost through");
            return true;
        }
    }
    if (!g.onboarding.moved)
        snprintf(message, message_size, "Fly with W A S D.");
    else if (!g.onboarding.fractured) {
        if (LOCAL_PLAYER.hover_asteroid >= 0 &&
            g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active)
            snprintf(message, message_size, "Hold M to fracture the targeted rock.");
        else
            snprintf(message, message_size, "Line up a rock, then hold M to fracture.");
    } else if (!g.onboarding.tractored) {
        if (LOCAL_PLAYER.nearby_fragments > 0)
            snprintf(message, message_size, "Hold SPACE to tractor loose fragments.");
        else
            snprintf(message, message_size, "Fracture rocks into fragments, then hold SPACE.");
    } else if (!g.onboarding.hailed) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig >= SIGNAL_BAND_OPERATIONAL)
            snprintf(message, message_size, "Press H near a station to hail and collect credits.");
        else
            snprintf(message, message_size, "Return to signal range, then press H to hail.");
    } else {
        /* Only boost remains. Wait for weak signal so the hint is timely
         * instead of pinning an empty subtitle over other system state. */
        if (message_size > 0) message[0] = '\0';
        return false;
    }
    return true;
}
