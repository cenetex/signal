#ifndef STATION_POLICY_H
#define STATION_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "types.h"

typedef enum {
    STATION_POLICY_DOMAIN_TRADE = 0,
    STATION_POLICY_DOMAIN_CONSTRUCTION,
    STATION_POLICY_DOMAIN_FINANCE,
    STATION_POLICY_DOMAIN_COUNT
} station_policy_domain_t;

typedef enum {
    STATION_POLICY_CARD_OPEN_MARKET = 0,
    STATION_POLICY_CARD_STRATEGIC_IMPORTS,
    STATION_POLICY_CARD_PROVENANCE_SCREENING,
    STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO,
    STATION_POLICY_CARD_REPAIR_STOCK_RESERVE,
    STATION_POLICY_CARD_EXPAND_PRODUCTION,
    STATION_POLICY_CARD_RESERVE_TREASURY,
    STATION_POLICY_CARD_AGGRESSIVE_BOUNTIES,
    STATION_POLICY_CARD_COUNT
} station_policy_card_id_t;

enum { STATION_POLICY_MAX_ACTIVE_CARDS = 8 };

typedef struct {
    uint8_t trade;
    uint8_t construction;
    uint8_t finance;
} station_policy_budget_t;

typedef struct {
    station_policy_card_id_t id;
    station_policy_domain_t domain;
    const char *name;
    uint8_t budget_cost;
} station_policy_card_t;

typedef struct {
    station_policy_card_id_t id;
    station_policy_domain_t domain;
    float score;
    uint8_t budget_cost;
} station_policy_card_rank_t;

typedef struct {
    station_policy_budget_t budget;
    uint8_t count;
    station_policy_card_rank_t cards[STATION_POLICY_MAX_ACTIVE_CARDS];
} station_policy_selection_t;

static inline uint64_t station_policy_forbidden_origin_mask(int station_idx,
                                                            commodity_t c);

static inline const station_policy_card_t *station_policy_card_library(int *count)
{
    static const station_policy_card_t cards[STATION_POLICY_CARD_COUNT] = {
        [STATION_POLICY_CARD_OPEN_MARKET] = {
            STATION_POLICY_CARD_OPEN_MARKET,
            STATION_POLICY_DOMAIN_TRADE,
            "open market",
            20,
        },
        [STATION_POLICY_CARD_STRATEGIC_IMPORTS] = {
            STATION_POLICY_CARD_STRATEGIC_IMPORTS,
            STATION_POLICY_DOMAIN_TRADE,
            "strategic imports",
            35,
        },
        [STATION_POLICY_CARD_PROVENANCE_SCREENING] = {
            STATION_POLICY_CARD_PROVENANCE_SCREENING,
            STATION_POLICY_DOMAIN_TRADE,
            "provenance screening",
            25,
        },
        [STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO] = {
            STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO,
            STATION_POLICY_DOMAIN_TRADE,
            "hostile-origin embargo",
            30,
        },
        [STATION_POLICY_CARD_REPAIR_STOCK_RESERVE] = {
            STATION_POLICY_CARD_REPAIR_STOCK_RESERVE,
            STATION_POLICY_DOMAIN_CONSTRUCTION,
            "repair stock reserve",
            25,
        },
        [STATION_POLICY_CARD_EXPAND_PRODUCTION] = {
            STATION_POLICY_CARD_EXPAND_PRODUCTION,
            STATION_POLICY_DOMAIN_CONSTRUCTION,
            "expand production",
            35,
        },
        [STATION_POLICY_CARD_RESERVE_TREASURY] = {
            STATION_POLICY_CARD_RESERVE_TREASURY,
            STATION_POLICY_DOMAIN_FINANCE,
            "reserve treasury",
            30,
        },
        [STATION_POLICY_CARD_AGGRESSIVE_BOUNTIES] = {
            STATION_POLICY_CARD_AGGRESSIVE_BOUNTIES,
            STATION_POLICY_DOMAIN_FINANCE,
            "aggressive bounties",
            30,
        },
    };
    if (count) *count = STATION_POLICY_CARD_COUNT;
    return cards;
}

static inline const char *station_policy_domain_name(station_policy_domain_t domain)
{
    switch (domain) {
        case STATION_POLICY_DOMAIN_TRADE:        return "trade";
        case STATION_POLICY_DOMAIN_CONSTRUCTION: return "construction";
        case STATION_POLICY_DOMAIN_FINANCE:      return "finance";
        default:                                 return "unknown";
    }
}

static inline const char *station_policy_card_name(station_policy_card_id_t id)
{
    int count = 0;
    const station_policy_card_t *cards = station_policy_card_library(&count);
    if ((int)id < 0 || (int)id >= count) return "unknown";
    return cards[id].name;
}

static inline station_policy_budget_t station_policy_default_budget(int station_idx)
{
    switch (station_idx) {
        case 0: return (station_policy_budget_t){ 45, 25, 30 };
        case 1: return (station_policy_budget_t){ 35, 45, 20 };
        case 2: return (station_policy_budget_t){ 45, 35, 20 };
        default: return (station_policy_budget_t){ 35, 35, 30 };
    }
}

static inline uint8_t *station_policy_domain_budget_ptr(
    station_policy_budget_t *budget, station_policy_domain_t domain)
{
    if (!budget) return NULL;
    switch (domain) {
        case STATION_POLICY_DOMAIN_TRADE:        return &budget->trade;
        case STATION_POLICY_DOMAIN_CONSTRUCTION: return &budget->construction;
        case STATION_POLICY_DOMAIN_FINANCE:      return &budget->finance;
        default:                                 return NULL;
    }
}

static inline bool station_policy_has_module(const station_t *st,
                                             module_type_t type)
{
    if (!st) return false;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == type && !st->modules[i].scaffold)
            return true;
    }
    return false;
}

static inline float station_policy_repair_kit_pressure(const station_t *st)
{
    if (!st) return 0.0f;
    float stock = st->_inventory_cache[COMMODITY_REPAIR_KIT];
    float target = 25.0f;
    if (stock >= target) return 0.0f;
    return (target - stock) / target;
}

static inline float station_policy_card_score(const station_t *st,
                                              int station_idx,
                                              station_policy_card_id_t id)
{
    bool has_dock = station_policy_has_module(st, MODULE_DOCK);
    bool has_shipyard = station_policy_has_module(st, MODULE_SHIPYARD);
    bool has_pending = st && st->pending_scaffold_count > 0;
    float kit_pressure = station_policy_repair_kit_pressure(st);

    switch (id) {
        case STATION_POLICY_CARD_OPEN_MARKET:
            return 0.35f;
        case STATION_POLICY_CARD_STRATEGIC_IMPORTS:
            return 0.25f + (kit_pressure * 0.45f) + (has_shipyard ? 0.15f : 0.0f);
        case STATION_POLICY_CARD_PROVENANCE_SCREENING:
            return 0.30f;
        case STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO:
            return station_policy_forbidden_origin_mask(station_idx,
                                                        COMMODITY_REPAIR_KIT) != 0
                ? 0.70f
                : 0.0f;
        case STATION_POLICY_CARD_REPAIR_STOCK_RESERVE:
            return has_dock ? (0.20f + kit_pressure * 0.70f) : 0.0f;
        case STATION_POLICY_CARD_EXPAND_PRODUCTION:
            return (has_shipyard || has_pending) ? 0.65f : 0.25f;
        case STATION_POLICY_CARD_RESERVE_TREASURY:
            return st && st->ledger_count > 12 ? 0.70f : 0.30f;
        case STATION_POLICY_CARD_AGGRESSIVE_BOUNTIES:
            return has_dock && !has_pending ? 0.45f : 0.20f;
        default:
            return 0.0f;
    }
}

static inline bool station_policy_selection_has(
    const station_policy_selection_t *selection,
    station_policy_card_id_t id)
{
    if (!selection) return false;
    for (int i = 0; i < selection->count; i++)
        if (selection->cards[i].id == id) return true;
    return false;
}

static inline bool station_policy_cached_has(const station_t *st,
                                             station_policy_card_id_t id)
{
    if (!st) return false;
    for (int i = 0; i < st->policy_card_count && i < STATION_POLICY_MAX_ACTIVE_CARDS; i++) {
        if (st->policy_card_ids[i] == (uint8_t)id) return true;
    }
    return false;
}

static inline void station_policy_select_cards(const station_t *st,
                                               int station_idx,
                                               station_policy_selection_t *out)
{
    if (!out) return;
    *out = (station_policy_selection_t){
        .budget = station_policy_default_budget(station_idx),
    };

    int count = 0;
    const station_policy_card_t *cards = station_policy_card_library(&count);
    bool used[STATION_POLICY_CARD_COUNT] = {0};
    station_policy_budget_t remaining = out->budget;

    while (out->count < STATION_POLICY_MAX_ACTIVE_CARDS) {
        int best = -1;
        float best_score = 0.0f;
        for (int i = 0; i < count; i++) {
            if (used[i]) continue;
            uint8_t *budget = station_policy_domain_budget_ptr(
                &remaining, cards[i].domain);
            if (!budget || *budget < cards[i].budget_cost) continue;
            float score = station_policy_card_score(st, station_idx,
                                                    cards[i].id);
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best < 0 || best_score <= 0.0f) break;
        used[best] = true;
        uint8_t *budget = station_policy_domain_budget_ptr(
            &remaining, cards[best].domain);
        if (!budget || *budget < cards[best].budget_cost) break;
        *budget = (uint8_t)(*budget - cards[best].budget_cost);
        out->cards[out->count++] = (station_policy_card_rank_t){
            .id = cards[best].id,
            .domain = cards[best].domain,
            .score = best_score,
            .budget_cost = cards[best].budget_cost,
        };
    }
}

static inline bool station_policy_cache_differs(
    const station_t *st,
    const station_policy_selection_t *selection,
    station_demand_t top_demand)
{
    if (!st || !selection) return true;
    if (st->policy_generation == 0) return true;
    if (st->policy_budget_trade != selection->budget.trade ||
        st->policy_budget_construction != selection->budget.construction ||
        st->policy_budget_finance != selection->budget.finance ||
        st->policy_card_count != selection->count) {
        return true;
    }
    for (int i = 0; i < selection->count && i < STATION_POLICY_MAX_ACTIVE_CARDS; i++) {
        const station_policy_card_rank_t *card = &selection->cards[i];
        if (st->policy_card_ids[i] != (uint8_t)card->id ||
            st->policy_card_domains[i] != (uint8_t)card->domain ||
            st->policy_card_costs[i] != card->budget_cost ||
            fabsf(st->policy_card_scores[i] - card->score) > 0.0001f) {
            return true;
        }
    }
    if (st->policy_top_demand_commodity != (uint8_t)top_demand.commodity)
        return true;
    if (fabsf(st->policy_top_demand_severity - top_demand.severity) > 0.0001f)
        return true;
    if (fabsf(st->policy_top_demand_price_mult - top_demand.price_mult) > 0.0001f)
        return true;
    return false;
}

static inline void station_policy_store_selection(
    station_t *st,
    const station_policy_selection_t *selection,
    station_demand_t top_demand)
{
    if (!st || !selection) return;
    st->policy_budget_trade = selection->budget.trade;
    st->policy_budget_construction = selection->budget.construction;
    st->policy_budget_finance = selection->budget.finance;
    st->policy_card_count = selection->count;
    for (int i = 0; i < STATION_POLICY_MAX_ACTIVE_CARDS; i++) {
        if (i < selection->count) {
            const station_policy_card_rank_t *card = &selection->cards[i];
            st->policy_card_ids[i] = (uint8_t)card->id;
            st->policy_card_domains[i] = (uint8_t)card->domain;
            st->policy_card_costs[i] = card->budget_cost;
            st->policy_card_scores[i] = card->score;
        } else {
            st->policy_card_ids[i] = 0;
            st->policy_card_domains[i] = 0;
            st->policy_card_costs[i] = 0;
            st->policy_card_scores[i] = 0.0f;
        }
    }
    st->policy_top_demand_commodity = (uint8_t)top_demand.commodity;
    st->policy_top_demand_severity = top_demand.severity;
    st->policy_top_demand_price_mult = top_demand.price_mult;
}

static inline void station_policy_refresh(station_t *st,
                                          int station_idx,
                                          uint64_t tick)
{
    if (!st) return;
    if (st->policy_tick == tick && st->policy_generation > 0) return;

    station_policy_selection_t selection;
    station_policy_select_cards(st, station_idx, &selection);
    station_demand_t top_demand = station_top_demand(st);
    bool changed = station_policy_cache_differs(st, &selection, top_demand);

    station_policy_store_selection(st, &selection, top_demand);
    st->policy_tick = tick;
    if (changed || st->policy_generation == 0)
        st->policy_generation++;
}

static inline float station_policy_trade_price_multiplier(const station_t *st,
                                                          commodity_t c)
{
    if (!st || c >= COMMODITY_COUNT) return 1.0f;
    float mult = 1.0f;
    station_demand_t demand = station_demand_for(st, c);

    if (station_policy_cached_has(st, STATION_POLICY_CARD_OPEN_MARKET) &&
        station_consumes(st, c)) {
        mult *= 1.05f;
    }
    if (station_policy_cached_has(st, STATION_POLICY_CARD_STRATEGIC_IMPORTS) &&
        demand.severity > 0.0f) {
        mult *= 1.0f + 0.25f * demand.severity;
    }
    if (station_policy_cached_has(st, STATION_POLICY_CARD_REPAIR_STOCK_RESERVE) &&
        c == COMMODITY_REPAIR_KIT) {
        mult *= 1.0f + 0.20f * station_policy_repair_kit_pressure(st);
    }
    if (station_policy_cached_has(st, STATION_POLICY_CARD_RESERVE_TREASURY))
        mult *= 0.90f;

    if (mult < 0.50f) mult = 0.50f;
    if (mult > 1.75f) mult = 1.75f;
    return mult;
}

/* Baseline deterministic station policy.
 *
 * Keep this as the single table for station-level political/trade rules.
 * The seeded economy is cooperative today:
 *   Prospect -> Kepler ferrite ingots
 *   Kepler   -> Helios frames
 *   Helios   -> Prospect repair kits
 *
 * That means the founding stations do not have blanket hostile-origin bans.
 * Future station lore/operator policy can populate this table without
 * changing the contract/provenance machinery. */
static inline uint64_t station_policy_forbidden_origin_mask(int station_idx,
                                                            commodity_t c)
{
    (void)station_idx;
    (void)c;
    return 0;
}

static inline void station_policy_apply_contract_origin_rules(const station_t *st,
                                                              contract_t *c)
{
    if (!c || !c->active || c->action != CONTRACT_TRACTOR) return;
    if (c->commodity < COMMODITY_RAW_ORE_COUNT ||
        c->commodity >= COMMODITY_COUNT) return;
    bool embargo = station_policy_cached_has(
        st, STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO);
    if (!embargo) {
        station_policy_selection_t selection;
        station_policy_select_cards(st, (int)c->station_index, &selection);
        embargo = station_policy_selection_has(
            &selection, STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO);
    }
    if (!embargo) return;

    uint64_t mask = station_policy_forbidden_origin_mask(
        (int)c->station_index, c->commodity);
    if (c->station_index < 64)
        mask &= ~(1ULL << c->station_index);
    if (mask == 0) return;

    c->proof_flags |= (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                CONTRACT_PROOF_FORBID_ORIGIN);
    c->forbidden_origin_mask |= mask;
}

#endif /* STATION_POLICY_H */
