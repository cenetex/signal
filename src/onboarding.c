/*
 * onboarding.c — First-run progression hints for Signal Space Miner.
 * Tracks which game actions the player has completed and provides
 * contextual hints for the next step in the loop.
 *
 * #249: Hints now come from stations, not a disembodied "GUIDE".
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

/* Pick the station that should "speak" for the current hint. */
static int onboard_speaker(int fallback) {
    if (LOCAL_PLAYER.docked && LOCAL_PLAYER.current_station >= 0)
        return LOCAL_PLAYER.current_station;
    int sig = nearest_signal_station(LOCAL_PLAYER.ship.pos);
    return (sig >= 0) ? sig : fallback;
}

/* Clamp to 0-2 for voice table lookup (outposts fall back to Prospect). */
static int voice_idx(int station) {
    return (station >= 0 && station < 3) ? station : 0;
}

static void say(int station, int line, char *label, size_t label_size,
                char *message, size_t message_size) {
    int vi = voice_idx(station);
    if (station >= 0 && station < MAX_STATIONS && g.world.stations[station].name[0])
        snprintf(label, label_size, "%s", g.world.stations[station].name);
    else
        snprintf(label, label_size, "SIGNAL");
    snprintf(message, message_size, "%s", STATION_ONBOARD[vi][line]);
}

bool onboarding_hint(char *label, size_t label_size,
                     char *message, size_t message_size) {
    if (g.onboarding.complete) return false;

    if (!g.onboarding.launched) {
        if (!LOCAL_PLAYER.docked) return false;
        say(0, VOICE_ONBOARD_LAUNCH, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.mined) {
        say(onboard_speaker(0), VOICE_ONBOARD_MINE, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.collected) {
        say(onboard_speaker(0), VOICE_ONBOARD_COLLECT, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.towed) {
        say(onboard_speaker(0), VOICE_ONBOARD_HAUL, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.sold) {
        if ((int)lroundf(ship_total_cargo(&LOCAL_PLAYER.ship)) <= 0) return false;
        say(onboard_speaker(0), VOICE_ONBOARD_SELL, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.bought) {
        if (!LOCAL_PLAYER.docked) return false;
        say(onboard_speaker(0), VOICE_ONBOARD_BUY, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.upgraded) {
        /* Milestone 6: handoff moment — the speaker depends on where you are */
        say(onboard_speaker(0), VOICE_ONBOARD_UPGRADE, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.got_scaffold) {
        say(onboard_speaker(1), VOICE_ONBOARD_SCAFFOLD, label, label_size, message, message_size);
        return true;
    }
    if (!g.onboarding.placed_outpost) {
        if (LOCAL_PLAYER.ship.towed_scaffold >= 0) {
            say(onboard_speaker(1), VOICE_ONBOARD_PLACE_ANCHOR, label, label_size, message, message_size);
        } else {
            say(onboard_speaker(1), VOICE_ONBOARD_PLACE_TOW, label, label_size, message, message_size);
        }
        return true;
    }
    return false;
}
