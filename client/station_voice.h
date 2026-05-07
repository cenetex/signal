/*
 * station_voice.h -- fallback NPC chatter for hail-scan labels.
 *
 * Station hails and station-authored NPC chatter come from server state.
 * These short lines are only fallbacks for NPCs whose home station has not
 * provided operator content yet.
 */
#ifndef STATION_VOICE_H
#define STATION_VOICE_H

/* Not every TU that includes this header uses every table. gcc's
 * -Werror=unused-variable fires per-TU on the ones it doesn't touch.
 * STATION_VOICE_UNUSED silences that without hiding a real unused var. */
#if defined(__GNUC__) || defined(__clang__)
#  define STATION_VOICE_UNUSED __attribute__((unused))
#else
#  define STATION_VOICE_UNUSED
#endif

/* ------------------------------------------------------------------ */
/* NPC radio chatter - short one-liners near sprites                  */
/* ------------------------------------------------------------------ */

#define NPC_CHATTER_MINER_COUNT  8
#define NPC_CHATTER_HAULER_COUNT 8

static STATION_VOICE_UNUSED const char *NPC_CHATTER_MINER[NPC_CHATTER_MINER_COUNT] = {
    "big one",
    "vein looks rich",
    "drilling",
    "hull steady",
    "cracking",
    "good rock",
    "hold's packed",
    "heading back",
};

static STATION_VOICE_UNUSED const char *NPC_CHATTER_HAULER[NPC_CHATTER_HAULER_COUNT] = {
    "load secured",
    "ingots inbound",
    "heading home",
    "hopper's full",
    "nothing to load",
    "frames loaded",
    "delivery run",
    "on schedule",
};

/* Tow drones: silent. The silence is characterization. */

#endif /* STATION_VOICE_H */
