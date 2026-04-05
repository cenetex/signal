/*
 * onboarding.c — First-run progression hints for Signal Space Miner.
 * Tracks which game actions the player has completed and provides
 * contextual hints for the next step in the loop.
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
    g.onboarding.launched       = (flags & (1 << 0)) != 0;
    g.onboarding.mined          = (flags & (1 << 1)) != 0;
    g.onboarding.collected      = (flags & (1 << 2)) != 0;
    g.onboarding.towed          = (flags & (1 << 3)) != 0;
    g.onboarding.sold           = (flags & (1 << 4)) != 0;
    g.onboarding.bought         = (flags & (1 << 5)) != 0;
    g.onboarding.upgraded       = (flags & (1 << 6)) != 0;
    g.onboarding.got_scaffold   = (flags & (1 << 7)) != 0;
    g.onboarding.placed_outpost = (flags & (1 << 8)) != 0;
    g.onboarding.complete = g.onboarding.placed_outpost;
#endif
}

static void onboarding_save(void) {
#ifdef __EMSCRIPTEN__
    int flags = 0;
    if (g.onboarding.launched)       flags |= (1 << 0);
    if (g.onboarding.mined)          flags |= (1 << 1);
    if (g.onboarding.collected)      flags |= (1 << 2);
    if (g.onboarding.towed)          flags |= (1 << 3);
    if (g.onboarding.sold)           flags |= (1 << 4);
    if (g.onboarding.bought)         flags |= (1 << 5);
    if (g.onboarding.upgraded)       flags |= (1 << 6);
    if (g.onboarding.got_scaffold)   flags |= (1 << 7);
    if (g.onboarding.placed_outpost) flags |= (1 << 8);
    char js[80];
    snprintf(js, sizeof(js), "localStorage.setItem('signal_onboarding','%d')", flags);
    emscripten_run_script(js);
#endif
}

/* ------------------------------------------------------------------ */
/* Step completion (call from game logic when actions happen)          */
/* ------------------------------------------------------------------ */

static void complete_step(bool *step) {
    if (*step) return;
    *step = true;
    onboarding_save();
}

void onboarding_mark_launched(void)       { complete_step(&g.onboarding.launched); }
void onboarding_mark_mined(void)          { complete_step(&g.onboarding.mined); }
void onboarding_mark_collected(void)      { complete_step(&g.onboarding.collected); }
void onboarding_mark_towed(void)          { complete_step(&g.onboarding.towed); }
void onboarding_mark_sold(void)           { complete_step(&g.onboarding.sold); }
void onboarding_mark_bought(void)         { complete_step(&g.onboarding.bought); }
void onboarding_mark_upgraded(void)       { complete_step(&g.onboarding.upgraded); }
void onboarding_mark_got_scaffold(void)   { complete_step(&g.onboarding.got_scaffold); }
void onboarding_mark_placed_outpost(void) {
    complete_step(&g.onboarding.placed_outpost);
    g.onboarding.complete = true;
}

/* ------------------------------------------------------------------ */
/* Hint text (returns false if onboarding is complete)                 */
/* ------------------------------------------------------------------ */

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (g.onboarding.complete) return false;

    if (!g.onboarding.launched) {
        if (LOCAL_PLAYER.docked) {
            snprintf(label, label_size, "GUIDE");
            snprintf(message, message_size, "Welcome, pilot. Press E to launch and start mining.");
            return true;
        }
        return false;
    }
    if (!g.onboarding.mined) {
        snprintf(label, label_size, "GUIDE");
        snprintf(message, message_size, "Fly toward an asteroid and hold Space to fire the mining beam.");
        return true;
    }
    if (!g.onboarding.collected) {
        snprintf(label, label_size, "GUIDE");
        snprintf(message, message_size, "Fly through ore fragments to collect them.");
        return true;
    }
    if (!g.onboarding.towed) {
        snprintf(label, label_size, "GUIDE");
        snprintf(message, message_size, "Tow fragments to a furnace and press R to release.");
        return true;
    }
    if (!g.onboarding.sold) {
        int cargo = (int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship));
        if (cargo > 0) {
            snprintf(label, label_size, "GUIDE");
            snprintf(message, message_size, "Head back to a station, dock with E, and press 1 to sell ore.");
            return true;
        }
        return false;
    }
    if (!g.onboarding.bought) {
        if (LOCAL_PLAYER.docked) {
            snprintf(label, label_size, "GUIDE");
            snprintf(message, message_size, "Press F to buy frames from the station.");
            return true;
        }
        return false;
    }
    if (!g.onboarding.upgraded) {
        snprintf(label, label_size, "GUIDE");
        snprintf(message, message_size, "Dock at Kepler Yard or Helios Works and upgrade your ship.");
        return true;
    }
    if (!g.onboarding.got_scaffold) {
        snprintf(label, label_size, "GUIDE");
        snprintf(message, message_size, "Buy a scaffold kit at a Blueprint Desk station (Kepler or Helios).");
        return true;
    }
    if (!g.onboarding.placed_outpost) {
        if (LOCAL_PLAYER.ship.has_scaffold_kit) {
            snprintf(label, label_size, "GUIDE");
            snprintf(message, message_size, "Press B in open space to place your outpost, then supply it.");
            return true;
        }
        return false;
    }
    return false;
}
