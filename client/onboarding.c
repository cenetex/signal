/*
 * onboarding.c — First-run checklist for Signal Space Miner.
 *
 * Five milestones in loose order: LAUNCH/MOVE, FRACTURE, TRACTOR,
 * HAIL, BOOST.
 * Shown as a persistent checklist until all are complete.
 * After that, stations take over via operator-authored hails.
 */
#include "client.h"
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
/* Checklist hint                                                      */
/* ------------------------------------------------------------------ */

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (label_size > 0) label[0] = '\0';
    if (g.onboarding.complete) {
        /* One final system line, then station hails own station voice. */
        if (!g.onboarding.welcomed) {
            g.onboarding.welcomed = true;
            snprintf(message, message_size,
                     "SIGNAL // CALIBRATION COMPLETE ::::: STATION NETWORK ONLINE");
            return true;
        }
        return false;
    }

    /* Subtitle-style: show the next useful action in the tutorial's own
     * system voice. Stations should not teach controls through hails. */
    if (LOCAL_PLAYER.docked) {
        if (!g.onboarding.moved) {
            snprintf(message, message_size,
                     "SIGNAL // GREETINGS PILOT ::::: SYSTEM CALIBRATING // [E] LAUNCH");
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
            snprintf(message, message_size,
                     "SIGNAL // LINK DEGRADED ::::: BOOST AVAILABLE // [SHIFT] BOOST");
            return true;
        }
    }
    if (!g.onboarding.moved)
        snprintf(message, message_size,
                 "SIGNAL // FLIGHT CONTROL ::::: [WASD] MOVE");
    else if (!g.onboarding.fractured) {
        if (LOCAL_PLAYER.hover_asteroid >= 0 &&
            g.world.asteroids[LOCAL_PLAYER.hover_asteroid].active)
            snprintf(message, message_size,
                     "SIGNAL // TARGET LOCK ::::: [M] FRACTURE ROCK");
        else
            snprintf(message, message_size,
                     "SIGNAL // TARGET ACQUISITION ::::: LINE UP ROCK // [M] FRACTURE");
    } else if (!g.onboarding.tractored) {
        if (LOCAL_PLAYER.nearby_fragments > 0)
            snprintf(message, message_size,
                     "SIGNAL // FRAGMENTS LOOSE ::::: [SPACE] TRACTOR");
        else
            snprintf(message, message_size,
                     "SIGNAL // MINING LOOP ::::: FRACTURE ROCKS INTO FRAGMENTS");
    } else if (!g.onboarding.hailed) {
        float sig = signal_strength_at(&g.world, LOCAL_PLAYER.ship.pos);
        if (sig >= SIGNAL_BAND_OPERATIONAL)
            snprintf(message, message_size,
                     "SIGNAL // LEDGER READY ::::: [H] HAIL STATION");
        else
            snprintf(message, message_size,
                     "SIGNAL // LINK REQUIRED ::::: RETURN TO SIGNAL // [H] HAIL");
    } else {
        /* Only boost remains. Wait for weak signal so the hint is timely
         * instead of pinning an empty subtitle over other system state. */
        if (message_size > 0) message[0] = '\0';
        return false;
    }
    return true;
}
