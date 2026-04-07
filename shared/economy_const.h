/*
 * economy_const.h — Economy and ship-upgrade tuning constants shared
 * between client and server. Pure constants, no struct dependencies.
 * Extracted from types.h (#273) so changes here don't recompile every
 * translation unit that needs a struct definition.
 */
#ifndef ECONOMY_CONST_H
#define ECONOMY_CONST_H

/* --- Refinery / station production --- */
static const float REFINERY_HOPPER_CAPACITY = 500.0f;
static const float REFINERY_BASE_SMELT_RATE = 2.0f;
static const int   REFINERY_MAX_FURNACES = 3;
static const float STATION_PRODUCTION_RATE = 0.3f;
static const float STATION_REPAIR_COST_PER_HULL = 2.0f;
static const float MAX_PRODUCT_STOCK = 40.0f;
static const float HAULER_RESERVE = 8.0f;  /* keep 20% stock for player purchases */

/* --- Outpost construction --- */
static const float SCAFFOLD_MATERIAL_NEEDED = 60.0f;   /* total frames needed */

/* --- Ship upgrades --- */
static const float SHIP_HOLD_UPGRADE_STEP = 24.0f;
static const float SHIP_MINING_UPGRADE_STEP = 7.0f;
static const float SHIP_TRACTOR_UPGRADE_STEP = 24.0f;
static const float SHIP_BASE_COLLECT_RADIUS = 30.0f;
static const float SHIP_COLLECT_UPGRADE_STEP = 5.0f;
static const float UPGRADE_BASE_PRODUCT = 8.0f;
static const int   SHIP_UPGRADE_MAX_LEVEL = 4;

#endif
