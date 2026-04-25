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
/* Production: ~1 unit/sec → conversions are visible inside 5-10s of docked
 * dwell time. Slower felt like nothing was happening. */
static const float STATION_PRODUCTION_RATE = 1.0f;
/* Tuned 2026-04-24 (#299): bumped 2.0 → 5.0 cr/HP. Miner runs average
 * 50-100 cr/rock; the old rate let pilots crash through belts and pay
 * back damage in two rocks. 5.0 makes a full miner repair (~500 cr)
 * cost a real handful of work, and pairs with the spawn-fee debt model
 * so reckless flying actually keeps the ledger negative. */
static const float STATION_REPAIR_COST_PER_HULL = 5.0f;
/* Refined-product stockpile cap: needs to be wide enough that a full hold
 * of ingots can be converted without stalling at the cap. */
static const float MAX_PRODUCT_STOCK = 120.0f;
static const float HAULER_RESERVE = 24.0f;  /* keep 20% stock for player purchases */

/* --- Dock repair-kit fab --- */
/* A station with MODULE_DOCK consumes 1 frame + 1 laser + 1 tractor and
 * produces REPAIR_KIT_PER_BATCH kits every REPAIR_KIT_FAB_PERIOD seconds.
 * Slow cadence is intentional — each batch heavily pulls on three
 * production chains; faster would saturate Kepler/Helios output and
 * starve outpost construction of its inputs. The cap is large because
 * kits get consumed 1-per-HP, not 1-per-ship-visit. */
static const float REPAIR_KIT_FAB_PERIOD  = 30.0f;
static const float REPAIR_KIT_PER_BATCH   = 100.0f;
static const float REPAIR_KIT_STOCK_CAP   = 1000.0f;

/* --- Outpost construction --- */
static const float SCAFFOLD_MATERIAL_NEEDED = 60.0f;   /* total frames needed */

/* --- Ship upgrades --- */
static const float SHIP_HOLD_UPGRADE_STEP = 8.0f;
static const float SHIP_MINING_UPGRADE_STEP = 7.0f;
static const float SHIP_TRACTOR_UPGRADE_STEP = 24.0f;
static const float SHIP_BASE_COLLECT_RADIUS = 30.0f;
static const float SHIP_COLLECT_UPGRADE_STEP = 5.0f;
static const float UPGRADE_BASE_PRODUCT = 8.0f;
static const int   SHIP_UPGRADE_MAX_LEVEL = 4;

#endif
