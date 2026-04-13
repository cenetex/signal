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
/* Onboarding voice lines -indexed [station][milestone]              */
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
    /* Prospect Refinery -terse, practical */
    {
        /* LAUNCH  */ "Signal tag registered. Launch when you're ready.",
        /* MINE    */ "Belt's hot. Point your laser at a rock. Start small.",
        /* COLLECT */ "Debris in your wake. Fly through it -tractor does the rest.",
        /* HAUL    */ "Full hold's no good out here. Bring it home.",
        /* SELL    */ "Hold's not empty. Sell at the market - I'll take what you've got.",
        /* BUY     */ "Credits in your account. Check the market.",
        /* UPGRADE */ "Your rig's stock. Kepler and Helios handle upgrades.",
        /* SCAFFOLD*/ "Dock at Kepler. Check the shipyard tab, order a kit.",
        /* PLACE_T */ "Find your scaffold near the yard. Tractor it.",
        /* PLACE_A */ "Tow it out. Anchor your outpost when you find a spot.",
        /* COMPLETE*/ "You know the basics. Next: build a furnace at your own outpost.",
    },
    /* Kepler Yard -technical, trails off */
    {
        /* LAUNCH  */ "Bay clear. Undock when ready.",
        /* MINE    */ "Asteroids are fragile. Fire your laser to crack one.",
        /* COLLECT */ "Fragments drift. Fly close -tractor pulls them in.",
        /* HAUL    */ "Ore's no good floating. Get it to a hopper.",
        /* SELL    */ "Raw ore on board? Sell it at the refinery.",
        /* BUY     */ "Check the market. Frames, ingots...",
        /* UPGRADE */ "New hull class. Let's see what we can do with it.",
        /* SCAFFOLD*/ "Shipyard's the last tab. Queue a kit from there.",
        /* PLACE_T */ "Scaffold's ready. Tractor it outside the yard.",
        /* PLACE_A */ "Good spot? Anchor it down.",
        /* COMPLETE*/ "Good setup. Next step: get a furnace running out there.",
    },
    /* Helios Works -enthusiastic, ambitious */
    {
        /* LAUNCH  */ "Welcome to Helios. Launch -always more to find.",
        /* MINE    */ "Rich veins out there. Laser a big one.",
        /* COLLECT */ "Don't leave ore behind. Fly through the debris.",
        /* HAUL    */ "Bring it home. We'll make something of it.",
        /* SELL    */ "Beautiful ore. Sell it here -we pay well.",
        /* BUY     */ "Check the market. We've got materials if you've got credits.",
        /* UPGRADE */ "New face. We should talk about upgrades.",
        /* SCAFFOLD*/ "Talk to Kepler for scaffold kits. Check their shipyard.",
        /* PLACE_T */ "Your scaffold's waiting outside. Tractor it.",
        /* PLACE_A */ "Building out there? Anchor it. Good.",
        /* COMPLETE*/ "You're ready. Build a furnace - we'll show you what copper can do.",
    },
};

/* ------------------------------------------------------------------ */
/* Docked context tips -indexed [station][tip_cycle]                 */
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
        /* SELL    */ "We buy ferrite. Dump it in the hopper.",
        /* MARKET  */ "Want ingots? Check the market. Not cheap.",
        /* SHIPYARD*/ "No shipyard here. Try Kepler.",
        /* LAUNCH  */ "Belt's waiting. Launch when ready.",
        /* DEFAULT */ "Switch tabs to see what's available.",
    },
    /* Kepler */
    {
        /* SELL    */ "We take ore. Sell from the market tab.",
        /* MARKET  */ "Frames and ingots. Browse the market.",
        /* SHIPYARD*/ "Shipyard's open. Order a kit from there.",
        /* LAUNCH  */ "Mind the scaffold arm on the way out.",
        /* DEFAULT */ "Switch tabs to see what's available.",
    },
    /* Helios */
    {
        /* SELL    */ "Cuprite, crystal -we take it all.",
        /* MARKET  */ "We've got specialty alloys. Check the market.",
        /* SHIPYARD*/ "No shipyard. Kepler handles kits.",
        /* LAUNCH  */ "More out there than rocks.",
        /* DEFAULT */ "Switch tabs to see what's available.",
    },
};

/* ------------------------------------------------------------------ */
/* NPC radio chatter -short one-liners near sprites                  */
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

/* ------------------------------------------------------------------ */
/* Contextual hail responses -condition → message per station        */
/* Checked in priority order; first match wins.                       */
/* ------------------------------------------------------------------ */

typedef enum {
    HAIL_COND_NO_TOWED,             /* not towing any fragments */
    HAIL_COND_HAS_TOWED,            /* towing S-tier fragments */
    HAIL_COND_LOW_CREDITS,          /* credits < 50, no outpost -early game */
    HAIL_COND_HAS_CREDITS_NO_OUTPOST, /* credits > 200 but no outpost */
    HAIL_COND_HAS_OUTPOST_NO_FURNACE, /* outpost exists but no furnace */
    HAIL_COND_HAS_FURNACE,          /* outpost has a furnace */
    HAIL_COND_HAS_NO_FRAMES,        /* never bought frames */
    HAIL_COND_HAS_NO_SCAFFOLD,      /* hasn't ordered a scaffold yet */
    HAIL_COND_HAS_OUTPOST_NO_PRESS,  /* outpost but no frame press */
    HAIL_COND_HAS_PRESS,            /* outpost has a frame press */
    /* HAIL_COND_HAS_SPECIALTY_ORE removed in #259 — ore-as-cargo is dead */
    HAIL_COND_NEVER_UPGRADED,       /* no upgrades applied */
    HAIL_COND_NO_SPECIALTY_FURNACE,  /* outpost but no cu/cr furnace */
    HAIL_COND_ONE_OUTPOST,          /* exactly 1 outpost, could build more */
    HAIL_COND_NEAR_EDGE,            /* player in fringe/frontier signal */
    HAIL_COND_DEFAULT,              /* always matches -ambient chatter */
    HAIL_COND_COUNT,
} hail_cond_t;

typedef struct {
    hail_cond_t condition;
    const char *message;
} hail_response_t;

/* Prospect Refinery -teaches the mining loop */
static const hail_response_t PROSPECT_HAILS[] = {
    { HAIL_COND_NO_TOWED,
      "Nothing on the line. Crack a rock. Point your laser at an asteroid." },
    { HAIL_COND_HAS_TOWED,
      "Fragments on the line. Tow them to a furnace." },
    { HAIL_COND_LOW_CREDITS,
      "Keep mining. Ore pays. Every run counts." },
    { HAIL_COND_HAS_CREDITS_NO_OUTPOST,
      "You've got credits. Think about building out there." },
    { HAIL_COND_HAS_OUTPOST_NO_FURNACE,
      "Your outpost needs a smelter. Talk to Kepler about a kit." },
    { HAIL_COND_HAS_FURNACE,
      "Your furnace is running. Keep it fed." },
    { HAIL_COND_NEAR_EDGE,
      "Signal's thin where you are. Belt drops off past the markers." },
    { HAIL_COND_DEFAULT,
      "Belt's quiet today." },
};
#define PROSPECT_HAIL_COUNT (int)(sizeof(PROSPECT_HAILS) / sizeof(PROSPECT_HAILS[0]))

/* Kepler Yard -teaches construction */
static const hail_response_t KEPLER_HAILS[] = {
    { HAIL_COND_NO_TOWED,
      "Nothing to haul here. Go mine something first." },
    { HAIL_COND_HAS_NO_FRAMES,
      "Frames are the bones of everything. Check the market." },
    { HAIL_COND_HAS_NO_SCAFFOLD,
      "Shipyard's open. Order a kit from there." },
    { HAIL_COND_HAS_OUTPOST_NO_PRESS,
      "Your outpost could use a frame press." },
    { HAIL_COND_HAS_PRESS,
      "Good setup. You're making your own frames now." },
    { HAIL_COND_NEAR_EDGE,
      "Something pinged the relay last night. Probably noise." },
    { HAIL_COND_DEFAULT,
      "Bay clear. Mind the scaffold arm." },
};
#define KEPLER_HAIL_COUNT (int)(sizeof(KEPLER_HAILS) / sizeof(KEPLER_HAILS[0]))

/* Helios Works -teaches expansion */
static const hail_response_t HELIOS_HAILS[] = {
    { HAIL_COND_NO_TOWED,
      "Nothing on the line? There's ore everywhere. Go crack some." },
    { HAIL_COND_NEVER_UPGRADED,
      "New face. We should talk about upgrades." },
    { HAIL_COND_NO_SPECIALTY_FURNACE,
      "Copper changes everything. Build a furnace for it." },
    { HAIL_COND_ONE_OUTPOST,
      "One outpost is a start. The network should be bigger." },
    { HAIL_COND_NEAR_EDGE,
      "We should build further out. There's more out there than rocks." },
    { HAIL_COND_DEFAULT,
      "Welcome to Helios. Always expanding." },
};
#define HELIOS_HAIL_COUNT (int)(sizeof(HELIOS_HAILS) / sizeof(HELIOS_HAILS[0]))

/* Lookup table for station index → response array */
static const hail_response_t *STATION_HAIL_TABLES[] = {
    PROSPECT_HAILS, KEPLER_HAILS, HELIOS_HAILS,
};
static const int STATION_HAIL_COUNTS[] = {
    PROSPECT_HAIL_COUNT, KEPLER_HAIL_COUNT, HELIOS_HAIL_COUNT,
};

#endif /* STATION_VOICE_H */

