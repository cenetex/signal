/*
 * station_voice.h -- Authored voice lines for the three starter stations.
 * Each station has a personality expressed through short, in-character text.
 *
 * Prospect Refinery (0): Pragmatic, tired, notices everything, says little.
 * Kepler Yard (1): Engineer, talks to machines, perks up for construction.
 * Helios Works (2): Ambitious, enthusiastic, uses "we" meaning "I."
 *
 * All lines must fit in ~50 chars (two HUD message rows at 26 chars each).
 */
#ifndef STATION_VOICE_H
#define STATION_VOICE_H

/* ------------------------------------------------------------------ */
/* Onboarding voice lines — indexed [station][milestone]              */
/* ------------------------------------------------------------------ */

/* Milestone indices (must match onboarding step order) */
enum {
    VOICE_ONBOARD_LAUNCH,
    VOICE_ONBOARD_MINE,
    VOICE_ONBOARD_COLLECT,
    VOICE_ONBOARD_HAUL,
    VOICE_ONBOARD_SELL,
    VOICE_ONBOARD_BUY,
    VOICE_ONBOARD_UPGRADE,
    VOICE_ONBOARD_SCAFFOLD,
    VOICE_ONBOARD_PLACE_TOW,
    VOICE_ONBOARD_PLACE_ANCHOR,
    VOICE_ONBOARD_COMPLETE,
    VOICE_ONBOARD_COUNT,
};

/* Station 0 = Prospect, 1 = Kepler, 2 = Helios.
 * Only the "speaker" station's line is used per milestone. */
static const char *STATION_ONBOARD[3][VOICE_ONBOARD_COUNT] = {
    /* Prospect Refinery — terse, practical */
    {
        /* LAUNCH  */ "Signal tag registered. Press [E] when you're ready.",
        /* MINE    */ "Belt's hot. Hold [Space] on a rock. Start small.",
        /* COLLECT */ "Debris in your wake. Fly through it — tractor does the rest.",
        /* HAUL    */ "Full hold's no good out here. Bring it home.",
        /* SELL    */ "Hold's not empty. Press [1] — I'll take what you've got.",
        /* BUY     */ "Credits in your account. Press [F] for the market.",
        /* UPGRADE */ "Your rig's stock. Kepler and Helios handle upgrades.",
        /* SCAFFOLD*/ "Dock at Kepler. Tab for SHIPYARD, [1-9] to order a kit.",
        /* PLACE_T */ "Find your scaffold near the yard. Grab it with [R].",
        /* PLACE_A */ "Tow it out. Press [B] to anchor your outpost.",
        /* COMPLETE*/ "New signal online. Welcome to the network.",
    },
    /* Kepler Yard — technical, trails off */
    {
        /* LAUNCH  */ "Bay clear. Press [E] to undock.",
        /* MINE    */ "Asteroids are fragile. Hold [Space] to crack one.",
        /* COLLECT */ "Fragments drift. Fly close — tractor pulls them in.",
        /* HAUL    */ "Ore's no good floating. Get it to a hopper.",
        /* SELL    */ "Raw ore on board? Press [1] at the refinery.",
        /* BUY     */ "Press [F] to check stock. Frames, ingots...",
        /* UPGRADE */ "New hull class. Let's see what we can do with it.",
        /* SCAFFOLD*/ "Shipyard's tab four. Press [1-9] to queue a kit.",
        /* PLACE_T */ "Scaffold's ready. Grab it with [R] outside.",
        /* PLACE_A */ "Good spot? Press [B] to lock it down.",
        /* COMPLETE*/ "Signal chain looks clean from here.",
    },
    /* Helios Works — enthusiastic, ambitious */
    {
        /* LAUNCH  */ "Welcome to Helios. Press [E] — always more to find.",
        /* MINE    */ "Rich veins out there. Hold [Space] on a big one.",
        /* COLLECT */ "Don't leave ore behind. Fly through the debris.",
        /* HAUL    */ "Bring it home. We'll make something of it.",
        /* SELL    */ "Beautiful ore. Press [1] — we pay well.",
        /* BUY     */ "Press [F]. We've got materials if you've got credits.",
        /* UPGRADE */ "New face. We should talk about upgrades. [3]/[4]/[5].",
        /* SCAFFOLD*/ "Talk to Kepler for scaffold kits. Tab, then [1-9].",
        /* PLACE_T */ "Your scaffold's waiting. Grab it with [R].",
        /* PLACE_A */ "Building out there? Press [B] to place it. Good.",
        /* COMPLETE*/ "New signal! We should talk about modules.",
    },
};

/* ------------------------------------------------------------------ */
/* Docked context tips — indexed [station][tip_cycle]                 */
/* ------------------------------------------------------------------ */

enum {
    DOCK_TIP_SELL,
    DOCK_TIP_MARKET,
    DOCK_TIP_SHIPYARD,
    DOCK_TIP_LAUNCH,
    DOCK_TIP_DEFAULT,
    DOCK_TIP_COUNT,
};

static const char *STATION_DOCK_TIPS[3][DOCK_TIP_COUNT] = {
    /* Prospect */
    {
        /* SELL    */ "We buy ferrite. Press [1] to dump it in the hopper.",
        /* MARKET  */ "Press [F] if you want ingots. Not cheap.",
        /* SHIPYARD*/ "No shipyard here. Try Kepler.",
        /* LAUNCH  */ "Belt's waiting. Press [E].",
        /* DEFAULT */ "Press [Tab] to switch panels.",
    },
    /* Kepler */
    {
        /* SELL    */ "We take ore. Press [1].",
        /* MARKET  */ "Frames and ingots. Press [F] to browse.",
        /* SHIPYARD*/ "Shipyard's open. [Tab] then [1-9] for a kit.",
        /* LAUNCH  */ "Mind the scaffold arm on the way out. [E].",
        /* DEFAULT */ "Press [Tab] to switch panels.",
    },
    /* Helios */
    {
        /* SELL    */ "Cuprite, crystal — we take it all. Press [1].",
        /* MARKET  */ "Press [F]. We've got specialty alloys.",
        /* SHIPYARD*/ "No shipyard. Kepler handles kits.",
        /* LAUNCH  */ "More out there than rocks. Press [E].",
        /* DEFAULT */ "Press [Tab] to switch panels.",
    },
};

/* ------------------------------------------------------------------ */
/* NPC radio chatter — short one-liners near sprites                  */
/* ------------------------------------------------------------------ */

#define NPC_CHATTER_MINER_COUNT  8
#define NPC_CHATTER_HAULER_COUNT 8

static const char *NPC_CHATTER_MINER[NPC_CHATTER_MINER_COUNT] = {
    "big one",
    "vein looks rich",
    "drilling",
    "hull steady",
    "cracking",
    "good rock",
    "hold's packed",
    "heading back",
};

static const char *NPC_CHATTER_HAULER[NPC_CHATTER_HAULER_COUNT] = {
    "load secured",
    "ore inbound",
    "heading home",
    "hopper's full",
    "nothing to load",
    "frames loaded",
    "delivery run",
    "on schedule",
};

/* Tow drones: silent. The silence is characterization. */

#endif /* STATION_VOICE_H */
