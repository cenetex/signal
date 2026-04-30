/*
 * economy_const.h — Economy and ship-upgrade tuning constants shared
 * between client and server. Pure constants, no struct dependencies.
 * Extracted from types.h (#273) so changes here don't recompile every
 * translation unit that needs a struct definition.
 */
#ifndef ECONOMY_CONST_H
#define ECONOMY_CONST_H

/* This header is included by shared/types.h *after* the ingot_prefix_t
 * enum is declared, so PREFIX_CLASS_PRICE_MULTIPLIER below can refer
 * to INGOT_PREFIX_* symbols. Don't include this file directly without
 * including types.h first. */

/* Prefix-class price multipliers — the lineage substrate's economic
 * dial. station_buy_price_unit() and station_sell_price_unit()
 * multiply through this table when the cargo unit being priced has a
 * non-anonymous prefix_class. Anonymous (the default for non-traceable
 * goods) stays at 1.0× so existing economy assumptions hold for the
 * bulk of trade.
 *
 * Values chosen so single-letter classes are noticeably premium but
 * not universe-warping; RATi is a once-in-a-thousand-rocks event-
 * pricing tier where finding one is news; commissioned is the RATi
 * Foundation reserve tier (effectively unreachable through normal
 * mining and serves as a top-end ceiling).
 *
 * Indexed by ingot_prefix_t. If you add a new class, add a multiplier
 * here too — the table is sized to INGOT_PREFIX_COUNT and indices are
 * range-checked at the callsite. */
static const float PREFIX_CLASS_PRICE_MULTIPLIER[INGOT_PREFIX_COUNT] = {
    [INGOT_PREFIX_ANONYMOUS]     = 1.0f,
    [INGOT_PREFIX_M]             = 2.0f,
    [INGOT_PREFIX_H]             = 2.0f,
    [INGOT_PREFIX_T]             = 2.0f,
    [INGOT_PREFIX_S]             = 2.0f,
    [INGOT_PREFIX_F]             = 2.0f,
    [INGOT_PREFIX_K]             = 2.5f,   /* slightly rarer letter */
    [INGOT_PREFIX_RATI]          = 50.0f,  /* once-in-a-blue-moon tier */
    [INGOT_PREFIX_COMMISSIONED]  = 100.0f, /* RATi Foundation reserved */
};

/* Helper: bounds-checked multiplier lookup. Returns 1.0× for any
 * class outside the valid range so a malformed cargo_unit_t can't
 * make a negative array index escape. */
static inline float prefix_class_price_multiplier(int cls) {
    if (cls < 0 || cls >= INGOT_PREFIX_COUNT) return 1.0f;
    return PREFIX_CLASS_PRICE_MULTIPLIER[cls];
}

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
/* Reserve a small floor for player purchases without choking the
 * inter-station chain. Was 24 (20% of MAX_PRODUCT_STOCK) — too high:
 * in NPC-only steady-state, stations smelt ~24 ingots over 5 minutes,
 * which never crosses the 24-floor, so no hauler ever picks up cargo.
 * 6 ingots is enough for one player to top off a hold; surplus flows. */
static const float HAULER_RESERVE = 6.0f;

/* --- Shipyard repair-kit fab --- */
/* A station with MODULE_SHIPYARD consumes 1 frame + 1 laser + 1 tractor
 * and produces REPAIR_KIT_PER_BATCH kits every REPAIR_KIT_FAB_PERIOD
 * seconds. Slow cadence is intentional — each batch heavily pulls on
 * three production chains; faster would saturate Kepler/Helios output
 * and starve outpost construction of its inputs. The cap is large
 * because kits get consumed 1-per-HP, not 1-per-ship-visit. */
static const float REPAIR_KIT_FAB_PERIOD  = 30.0f;
static const float REPAIR_KIT_PER_BATCH   = 100.0f;
static const float REPAIR_KIT_STOCK_CAP   = 1000.0f;

/* Kits are dense cargo: 10 kits per cargo unit. A 100-HP repair (= 100
 * kits) costs 10 cargo slots, which fits in any starter ship's hold.
 * commodity_volume() returns this for REPAIR_KIT, 1.0 for everything
 * else. */
static const float REPAIR_KIT_CARGO_DENSITY = 0.1f;

/* Per-HP labor fee charged at non-shipyard docks (where the kits were
 * fabricated elsewhere). Shipyards charge 0 labor — you already paid
 * full station retail when you bought the kits there. */
static const float LABOR_FEE_PER_HP = 1.0f;

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
