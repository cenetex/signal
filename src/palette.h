/*
 * palette.h -- Single source of truth for all named color constants.
 * Replaces hardcoded RGB triples across hud.c, station_ui.c, world_draw.c, render.c.
 *
 * Naming convention:
 *   PAL_<SEMANTIC>: bytes (0-255 range)
 *   PAL_F_<SEMANTIC>: floats (0.0-1.0 range)
 *
 * Usage:
 *   sdtx_color3b(PAL_TEXT_PRIMARY);   -- direct in function args
 *   sgl_c4f(PAL_F_WARNING, 1.0f);    -- direct in function args
 *   PAL_UNPACK3(PAL_F_*, r, g, b);   -- assign to three variables/pointers
 */

/* Unpack a 3-component palette macro into three lvalue assignments */
#define PAL_UNPACK3(pal, a, b, c) do { \
    float _pal[] = { pal }; (a) = _pal[0]; (b) = _pal[1]; (c) = _pal[2]; \
} while (0)

/* ================================================================
 * TEXT COLORS
 * ================================================================ */

/* Primary text (heading, status lines) */
#define PAL_TEXT_PRIMARY     232, 241, 255
#define PAL_F_TEXT_PRIMARY   0.91f, 0.95f, 1.00f

/* Secondary text (detail, metadata) */
#define PAL_TEXT_SECONDARY   203, 220, 248
#define PAL_F_TEXT_SECONDARY 0.80f, 0.86f, 0.97f

/* Muted text (dimmed, inactive, hints) */
#define PAL_TEXT_MUTED       169, 179, 204
#define PAL_F_TEXT_MUTED     0.66f, 0.70f, 0.80f

/* Dim/grey text for inactive elements */
#define PAL_TEXT_GREY        100, 110, 100
#define PAL_F_TEXT_GREY      0.39f, 0.43f, 0.39f

/* Faded background text */
#define PAL_TEXT_FADED       100, 130, 120
#define PAL_F_TEXT_FADED     0.39f, 0.51f, 0.47f

/* ================================================================
 * STATUS & SIGNAL COLORS
 * ================================================================ */

/* Active/mint green — docking, active scan, tractor lock */
#define PAL_SIGNAL_MINT      112, 255, 214
#define PAL_F_SIGNAL_MINT    0.44f, 1.00f, 0.84f

/* Light blue — neutral nav info */
#define PAL_NAV_BLUE         199, 222, 255
#define PAL_F_NAV_BLUE       0.78f, 0.87f, 1.00f

/* Bright cyan — interactive status */
#define PAL_ACTIVE           130, 255, 235
#define PAL_F_ACTIVE         0.51f, 1.00f, 0.92f

/* Signal band colors (signal strength) */
#define PAL_SIGNAL_OPERATIONAL 203, 220, 248
#define PAL_F_SIGNAL_OPERATIONAL 0.80f, 0.86f, 0.97f

#define PAL_SIGNAL_FRINGE    255, 221, 119
#define PAL_F_SIGNAL_FRINGE  1.00f, 0.87f, 0.47f

#define PAL_SIGNAL_FRONTIER  255, 80, 80
#define PAL_F_SIGNAL_FRONTIER 1.00f, 0.31f, 0.31f

/* Scan active indicator */
#define PAL_SCAN_ACTIVE      100, 180, 255
#define PAL_F_SCAN_ACTIVE    0.39f, 0.71f, 1.00f

/* Radio green — comms flavor */
#define PAL_RADIO_GREEN      100, 130, 110
#define PAL_F_RADIO_GREEN    0.39f, 0.51f, 0.43f

/* ================================================================
 * COMMODITY & RESOURCE COLORS
 * ================================================================ */

/* Ore amber — cargo, haul values */
#define PAL_ORE_AMBER        255, 221, 119
#define PAL_F_ORE_AMBER      1.00f, 0.87f, 0.47f

/* Tractor + hold colors */
#define PAL_HOLD_CYAN        130, 255, 235
#define PAL_F_HOLD_CYAN      0.51f, 1.00f, 0.92f

/* Tractor off (inactive) */
#define PAL_TRACTOR_OFF      180, 150, 120
#define PAL_F_TRACTOR_OFF    0.71f, 0.59f, 0.47f

/* ================================================================
 * WARNING & ALERT COLORS
 * ================================================================ */

/* Hull critical — blood red */
#define PAL_WARNING          255, 60, 50
#define PAL_F_WARNING        1.00f, 0.24f, 0.20f

/* Generic notice — grey */
#define PAL_NOTICE           140, 140, 140
#define PAL_F_NOTICE         0.55f, 0.55f, 0.55f

/* Plan mode indicator — cyan-ish */
#define PAL_PLAN_MODE        130, 180, 200
#define PAL_F_PLAN_MODE      0.51f, 0.71f, 0.78f

/* Autopilot mode — warm amber */
#define PAL_AUTOPILOT        180, 160, 90
#define PAL_F_AUTOPILOT      0.71f, 0.63f, 0.35f

/* Docking message */
#define PAL_DOCKING          120, 160, 150
#define PAL_F_DOCKING        0.47f, 0.63f, 0.59f

/* Collection feedback — pale green */
#define PAL_SCOOP            120, 150, 120
#define PAL_F_SCOOP          0.47f, 0.59f, 0.47f

/* Tow mode */
#define PAL_TOW              160, 150, 100
#define PAL_F_TOW            0.63f, 0.59f, 0.39f

/* ================================================================
 * STATION IDENTITY COLORS (role-based accents)
 * ================================================================ */

/* Prospect Refinery — green */
#define PAL_STATION_PROSPECT 0.30f, 0.80f, 0.35f

/* Kepler Yard — gold */
#define PAL_STATION_KEPLER   0.90f, 0.75f, 0.20f

/* Helios Works (CU module) — bright blue */
#define PAL_STATION_HELIOS_CU 0.25f, 0.50f, 0.90f

/* Helios Works (CR module) — indigo */
#define PAL_STATION_HELIOS_CR 0.40f, 0.35f, 0.85f

/* Generic station — neutral teal-grey */
#define PAL_STATION_NEUTRAL  0.35f, 0.55f, 0.50f

/* ================================================================
 * MODULE COLORS (world rendering)
 * ================================================================ */

/* Prospect modules */
#define PAL_MODULE_FURNACE   0.30f, 0.80f, 0.35f
#define PAL_MODULE_HOPPER 0.40f, 0.72f, 0.30f

/* Kepler modules */
#define PAL_MODULE_FRAME_PRESS 0.90f, 0.75f, 0.20f
#define PAL_MODULE_SHIPYARD  0.85f, 0.70f, 0.20f

/* Shared neutral modules */
#define PAL_MODULE_ORE_SILO  0.45f, 0.48f, 0.50f
#define PAL_MODULE_LASER_FAB 0.55f, 0.45f, 0.50f
#define PAL_MODULE_TRACTOR_FAB 0.45f, 0.50f, 0.48f
#define PAL_MODULE_SIGNAL_RELAY 0.35f, 0.55f, 0.50f
#define PAL_MODULE_REPAIR_BAY 0.40f, 0.60f, 0.50f
#define PAL_MODULE_GENERIC   0.35f, 0.40f, 0.45f

/* ================================================================
 * SYSTEM UI COLORS
 * ================================================================ */

/* Death screen title */
#define PAL_DEATH_TITLE      255, 80, 60
#define PAL_F_DEATH_TITLE    1.00f, 0.31f, 0.24f

/* Death screen stats — earned (green) */
#define PAL_DEATH_EARNED     120, 200, 120
#define PAL_F_DEATH_EARNED   0.47f, 0.78f, 0.47f

/* Death screen stats — spent (red) */
#define PAL_DEATH_SPENT      200, 120, 120
#define PAL_F_DEATH_SPENT    0.78f, 0.47f, 0.47f

/* Death screen prompt (flashing red) */
#define PAL_DEATH_PROMPT     255, 30, 20
#define PAL_F_DEATH_PROMPT   1.00f, 0.12f, 0.08f

/* Multiplayer sync OK */
#define PAL_SYNC_OK          80, 180, 120
#define PAL_F_SYNC_OK        0.31f, 0.71f, 0.47f

/* Multiplayer disconnected */
#define PAL_SYNC_OFFLINE     180, 120, 60
#define PAL_F_SYNC_OFFLINE   0.71f, 0.47f, 0.24f

/* Multiplayer connecting */
#define PAL_SYNC_CONNECTING  220, 200, 60
#define PAL_F_SYNC_CONNECTING 0.86f, 0.78f, 0.24f

/* Multiplayer resyncing */
#define PAL_SYNC_RESYNCING   255, 160, 60
#define PAL_F_SYNC_RESYNCING 1.00f, 0.63f, 0.24f

/* Alpha version banner */
#define PAL_ALPHA_BANNER     180, 160, 60
#define PAL_F_ALPHA_BANNER   0.71f, 0.63f, 0.24f

/* ================================================================
 * HUD PANEL COLORS (procedural rendering)
 * ================================================================ */

/* UI accent baseline (varies by context) */
#define PAL_UI_ACCENT_BASE   0.26f, 0.72f, 0.98f

/* UI accent with variation */
#define PAL_UI_ACCENT_LIGHT  0.14f, 0.92f, 1.00f

/* UI background (dark) */
#define PAL_UI_BG_DARK       0.01f, 0.03f, 0.06f
#define PAL_UI_BG_SLIGHTLY_LIGHTER 0.018f, 0.044f, 0.072f

/* UI border (dimmed) */
#define PAL_UI_BORDER        0.09f, 0.18f, 0.28f

/* UI scanline overlay */
#define PAL_UI_SCANLINE      0.08f, 0.14f, 0.20f

/* UI bracket accents */
#define PAL_UI_BRACKET       0.24f, 0.48f, 0.62f

/* UI rule (header dividers) */
#define PAL_UI_RULE_DIM      0.14f, 0.26f, 0.38f
#define PAL_UI_RULE_DIMMER   0.18f, 0.28f, 0.38f

/* UI outline */
#define PAL_UI_OUTLINE       0.12f, 0.22f, 0.32f

/* UI focus/highlight */
#define PAL_UI_FOCUS         0.30f, 0.85f, 1.00f

/* UI building status — material awaiting (orange) */
#define PAL_BUILD_SUPPLYING  255, 140, 40
#define PAL_F_BUILD_SUPPLYING 1.00f, 0.55f, 0.16f

/* UI building status — build timer (amber) */
#define PAL_BUILD_BUILDING   255, 180, 60
#define PAL_F_BUILD_BUILDING 1.00f, 0.71f, 0.24f

/* UI online status (cyan) */
#define PAL_UI_ONLINE        130, 255, 235
#define PAL_F_UI_ONLINE      0.51f, 1.00f, 0.92f

/* UI hull good */
#define PAL_HULL_GOOD        0.96f, 0.54f, 0.28f

/* UI cargo filled */
#define PAL_CARGO_FILLED     0.26f, 0.90f, 0.72f

/* UI upgrade pip colors */
#define PAL_UPGRADE_MINING   0.34f, 0.88f, 1.00f
#define PAL_UPGRADE_TRACTOR  0.42f, 1.00f, 0.86f
#define PAL_UPGRADE_HOLD     0.50f, 0.82f, 1.00f

/* ================================================================
 * STATION UI SPECIFIC COLORS
 * ================================================================ */

/* Module inspect — module name (amber) */
#define PAL_INSPECT_MODULE   255, 221, 119
#define PAL_F_INSPECT_MODULE 1.00f, 0.87f, 0.47f

/* Module inspect — station name (muted blue) */
#define PAL_INSPECT_STATION  145, 160, 188
#define PAL_F_INSPECT_STATION 0.57f, 0.63f, 0.74f

/* Module inspect — location (bright cyan) */
#define PAL_INSPECT_LOCATION 130, 200, 255
#define PAL_F_INSPECT_LOCATION 0.51f, 0.78f, 1.00f

/* Station console header */
#define PAL_STATION_HEADER   232, 241, 255
#define PAL_F_STATION_HEADER 0.91f, 0.95f, 1.00f

/* Station console hint/muted */
#define PAL_STATION_HINT     145, 160, 188
#define PAL_F_STATION_HINT   0.57f, 0.63f, 0.74f

/* Station tab active underline */
#define PAL_STATION_TAB_ACTIVE 0.30f, 0.85f, 1.00f

/* Hail portrait border */
#define PAL_HAIL_PORTRAIT_BORDER 0.5f, 0.6f, 0.8f

/* Hail station name text */
#define PAL_HAIL_STATION_BYTES 130, 200, 255
#define PAL_F_HAIL_STATION   0.51f, 0.78f, 1.00f

/* Hail message text */
#define PAL_HAIL_MESSAGE_BYTES 180, 190, 210
#define PAL_F_HAIL_MESSAGE   0.71f, 0.75f, 0.82f

/* Hail credits collected */
#define PAL_HAIL_CREDITS_BYTES 130, 255, 235
#define PAL_F_HAIL_CREDITS   0.51f, 1.00f, 0.92f

/* ================================================================
 * READY STATES & AFFORDABILITY
 * ================================================================ */

/* Delivery ready (green) */
#define PAL_READY_GREEN      130, 255, 180
#define PAL_F_READY_GREEN    0.51f, 1.00f, 0.71f

/* Delivery pending (yellow) */
#define PAL_READY_YELLOW     255, 240, 130
#define PAL_F_READY_YELLOW   1.00f, 0.94f, 0.51f

/* Delivery status */
#define PAL_DELIVERY_STATUS  220, 230, 160
#define PAL_F_DELIVERY_STATUS 0.86f, 0.90f, 0.63f

/* Delivery hint */
#define PAL_DELIVERY_HINT    160, 175, 200
#define PAL_F_DELIVERY_HINT  0.63f, 0.69f, 0.78f

/* Can afford (bright) */
#define PAL_AFFORD_YES       130, 255, 235
#define PAL_F_AFFORD_YES     0.51f, 1.00f, 0.92f

/* Cannot afford (dimmed) */
#define PAL_AFFORD_NO        90, 105, 130
#define PAL_F_AFFORD_NO      0.35f, 0.41f, 0.51f

/* Dim/inactive affordability */
#define PAL_AFFORD_INACTIVE  80, 90, 110
#define PAL_F_AFFORD_INACTIVE 0.31f, 0.35f, 0.43f

/* ================================================================
 * HALO EFFECTS & BACKGROUND
 * ================================================================ */

/* Star background (base neutral) */
#define PAL_STAR_BASE        0.65f, 0.75f, 1.00f

/* Background neutral */
#define PAL_BG_NEUTRAL       100, 130, 110
#define PAL_F_BG_NEUTRAL     0.39f, 0.51f, 0.43f

/* World draw station text (light blue) */
#define PAL_WORLD_STATION    180, 220, 240
#define PAL_F_WORLD_STATION  0.71f, 0.86f, 0.94f

/* Episode bright text */
#define PAL_EPISODE_BRIGHT_BASE 200, 160, 48
#define PAL_F_EPISODE_BRIGHT_BASE 0.78f, 0.63f, 0.19f

/* ================================================================
 * UI TEXT — SERVICE LINES & TABLE VARIANTS
 * ================================================================ */

/* Service line headers — light blue */
#define PAL_SERVICE_HEADER      180, 195, 215
#define PAL_F_SERVICE_HEADER    0.71f, 0.76f, 0.84f

/* Service line content — slightly dimmer */
#define PAL_SERVICE_CONTENT     120, 140, 165
#define PAL_F_SERVICE_CONTENT   0.47f, 0.55f, 0.65f

/* Contract affordability — can afford (bright cyan) */
#define PAL_CONTRACT_AFFORD     130, 255, 235
#define PAL_F_CONTRACT_AFFORD   0.51f, 1.00f, 0.92f

/* Contract item ready (green) */
#define PAL_CONTRACT_READY      130, 255, 180
#define PAL_F_CONTRACT_READY    0.51f, 1.00f, 0.71f

/* Contract item pending (yellow) */
#define PAL_CONTRACT_PENDING    255, 240, 130
#define PAL_F_CONTRACT_PENDING  1.00f, 0.94f, 0.51f

/* Contract item status (grey) */
#define PAL_CONTRACT_STATUS     220, 230, 160
#define PAL_F_CONTRACT_STATUS   0.86f, 0.90f, 0.63f

/* Contract item hint (dimmed) */
#define PAL_CONTRACT_HINT       160, 175, 200
#define PAL_F_CONTRACT_HINT     0.63f, 0.69f, 0.78f

/* Cannot afford variant */
#define PAL_CANNOT_AFFORD       120, 130, 150
#define PAL_F_CANNOT_AFFORD     0.47f, 0.51f, 0.59f

/* Cargo affordability display (alternate) */
#define PAL_CARGO_ITEM          90, 220, 170
#define PAL_F_CARGO_ITEM        0.35f, 0.86f, 0.67f

/* Hold status with condition */
#define PAL_HOLD_STATUS         100, 120, 145
#define PAL_F_HOLD_STATUS       0.39f, 0.47f, 0.57f

/* Shipyard section header */
#define PAL_SHIPYARD_HEADER     85, 100, 120
#define PAL_F_SHIPYARD_HEADER   0.33f, 0.39f, 0.47f

/* Shipyard item hint */
#define PAL_SHIPYARD_HINT       150, 165, 185
#define PAL_F_SHIPYARD_HINT     0.59f, 0.65f, 0.73f

/* Shipyard affordability check */
#define PAL_SHIPYARD_AFFORD     130, 145, 168
#define PAL_F_SHIPYARD_AFFORD   0.51f, 0.57f, 0.66f

/* Status check — cannot perform (very dim) */
#define PAL_STATUS_DISABLED     100, 115, 138
#define PAL_F_STATUS_DISABLED   0.39f, 0.45f, 0.54f

/* Active/secondary option */
#define PAL_OPTION_SECONDARY    100, 120, 145
#define PAL_F_OPTION_SECONDARY  0.39f, 0.47f, 0.57f

/* Conditional disable state (afford) */
#define PAL_COND_DISABLE_AFFORD 90, 105, 130
#define PAL_F_COND_DISABLE_AFFORD 0.35f, 0.41f, 0.51f

/* Delivery status blue */
#define PAL_DELIVERY_BLUE       180, 220, 255
#define PAL_F_DELIVERY_BLUE     0.71f, 0.86f, 1.00f

/* Supply status dim */
#define PAL_SUPPLY_DIM          120, 135, 160
#define PAL_F_SUPPLY_DIM        0.47f, 0.53f, 0.63f

/* World draw station name (light cyan) */
#define PAL_WORLD_STATION_CYAN  180, 220, 240
#define PAL_F_WORLD_STATION_CYAN 0.71f, 0.86f, 0.94f
