#ifndef CONTRACT_FIT_H
#define CONTRACT_FIT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "types.h"

typedef enum {
    CONTRACT_FIT_OK = 0,
    CONTRACT_FIT_INACTIVE,
    CONTRACT_FIT_WRONG_ACTION,
    CONTRACT_FIT_WRONG_COMMODITY,
    CONTRACT_FIT_GRADE_TOO_LOW,
    CONTRACT_FIT_WRONG_TIER,
    CONTRACT_FIT_NO_CARGO,
    CONTRACT_FIT_NOT_DELIVERABLE,
    CONTRACT_FIT_MISSING_PROOF,
    CONTRACT_FIT_WRONG_RECIPE,
    CONTRACT_FIT_WRONG_PREFIX,
    CONTRACT_FIT_WRONG_PARENT,
    CONTRACT_FIT_FORBIDDEN_ORIGIN,
} contract_fit_reason_t;

static inline const char *contract_fit_reason_label(contract_fit_reason_t reason) {
    switch (reason) {
    case CONTRACT_FIT_OK:              return "match";
    case CONTRACT_FIT_INACTIVE:        return "inactive";
    case CONTRACT_FIT_WRONG_ACTION:    return "wrong contract";
    case CONTRACT_FIT_WRONG_COMMODITY: return "wrong material";
    case CONTRACT_FIT_GRADE_TOO_LOW:   return "grade too low";
    case CONTRACT_FIT_WRONG_TIER:      return "needs fragment";
    case CONTRACT_FIT_NO_CARGO:        return "no cargo";
    case CONTRACT_FIT_NOT_DELIVERABLE: return "not deliverable";
    case CONTRACT_FIT_MISSING_PROOF:   return "proof missing";
    case CONTRACT_FIT_WRONG_RECIPE:    return "wrong recipe";
    case CONTRACT_FIT_WRONG_PREFIX:    return "wrong class";
    case CONTRACT_FIT_WRONG_PARENT:    return "wrong lineage";
    case CONTRACT_FIT_FORBIDDEN_ORIGIN: return "enemy origin";
    default:                           return "unknown";
    }
}

static inline bool contract_fit_is_ok(contract_fit_reason_t reason) {
    return reason == CONTRACT_FIT_OK;
}

static inline bool contract_fit_has_bytes(const uint8_t bytes[32]) {
    static const uint8_t zero[32] = {0};
    return bytes && memcmp(bytes, zero, sizeof(zero)) != 0;
}

static inline bool contract_target_pub_is_set(const contract_t *contract)
{
    return contract && contract_fit_has_bytes(contract->target_pub);
}

static inline bool contract_asteroid_target_matches(const contract_t *contract,
                                                    const asteroid_t *asteroid)
{
    if (!contract || !asteroid) return false;
    if (!contract_target_pub_is_set(contract)) return true;
    if (contract_fit_has_bytes(asteroid->rock_pub) &&
        memcmp(contract->target_pub, asteroid->rock_pub, 32) == 0) {
        return true;
    }
    return contract_fit_has_bytes(asteroid->fragment_pub) &&
           memcmp(contract->target_pub, asteroid->fragment_pub, 32) == 0;
}

static inline void contract_set_target_pub_from_asteroid(contract_t *contract,
                                                        const asteroid_t *asteroid)
{
    if (!contract) return;
    memset(contract->target_pub, 0, sizeof(contract->target_pub));
    if (!asteroid) return;
    if (contract_fit_has_bytes(asteroid->rock_pub)) {
        memcpy(contract->target_pub, asteroid->rock_pub, sizeof(contract->target_pub));
    } else if (contract_fit_has_bytes(asteroid->fragment_pub)) {
        memcpy(contract->target_pub, asteroid->fragment_pub, sizeof(contract->target_pub));
    }
}

static inline bool contract_fit_is_finished_cargo_action(
    const contract_t *contract)
{
    return contract &&
           (contract->action == CONTRACT_TRACTOR ||
            contract->action == CONTRACT_DELIVERY);
}

static inline contract_fit_reason_t contract_fit_cargo_fields(
    const contract_t *contract,
    commodity_t commodity,
    mining_grade_t grade,
    uint16_t quantity,
    bool has_proof)
{
    if (!contract || !contract->active) return CONTRACT_FIT_INACTIVE;
    if (!contract_fit_is_finished_cargo_action(contract))
        return CONTRACT_FIT_WRONG_ACTION;
    if (quantity == 0) return CONTRACT_FIT_NO_CARGO;
    if (commodity >= COMMODITY_COUNT ||
        contract->commodity != commodity) {
        return CONTRACT_FIT_WRONG_COMMODITY;
    }
    if (grade >= MINING_GRADE_COUNT ||
        grade < (mining_grade_t)contract->required_grade) {
        return CONTRACT_FIT_GRADE_TOO_LOW;
    }
    if ((contract->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF) && !has_proof)
        return CONTRACT_FIT_MISSING_PROOF;
    if ((contract->proof_flags & CONTRACT_PROOF_FORBID_ORIGIN) && !has_proof)
        return CONTRACT_FIT_MISSING_PROOF;
    return CONTRACT_FIT_OK;
}

static inline contract_fit_reason_t contract_fit_cargo_unit(
    const contract_t *contract,
    const cargo_unit_t *unit)
{
    if (!unit) return CONTRACT_FIT_NO_CARGO;
    uint16_t quantity = unit->quantity > 0 ? unit->quantity : 1;
    bool has_proof = contract_fit_has_bytes(unit->pub) ||
                     contract_fit_has_bytes(unit->parent_merkle) ||
                     unit->mined_block != 0;
    contract_fit_reason_t base = contract_fit_cargo_fields(
        contract,
        (commodity_t)unit->commodity,
        (mining_grade_t)unit->grade,
        quantity,
        has_proof);
    if (!contract_fit_is_ok(base)) return base;

    if ((contract->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE) &&
        unit->recipe_id != contract->required_recipe_id) {
        return CONTRACT_FIT_WRONG_RECIPE;
    }
    if ((contract->proof_flags & CONTRACT_PROOF_REQUIRE_PREFIX) &&
        unit->prefix_class != contract->required_prefix_class) {
        return CONTRACT_FIT_WRONG_PREFIX;
    }
    if ((contract->proof_flags & CONTRACT_PROOF_REQUIRE_PARENT) &&
        memcmp(unit->parent_merkle, contract->required_parent, 32) != 0) {
        return CONTRACT_FIT_WRONG_PARENT;
    }
    if ((contract->proof_flags & CONTRACT_PROOF_FORBID_ORIGIN) &&
        unit->origin_station < 64 &&
        (contract->forbidden_origin_mask & (1ULL << unit->origin_station)) != 0) {
        return CONTRACT_FIT_FORBIDDEN_ORIGIN;
    }
    return CONTRACT_FIT_OK;
}

static inline contract_fit_reason_t contract_fit_fragment(
    const contract_t *contract,
    const asteroid_t *fragment)
{
    if (!contract || !contract->active) return CONTRACT_FIT_INACTIVE;
    if (contract->action != CONTRACT_TRACTOR) return CONTRACT_FIT_WRONG_ACTION;
    if (!fragment || !fragment->active || fragment->ore <= 0.0f)
        return CONTRACT_FIT_NO_CARGO;
    if (contract->commodity >= COMMODITY_RAW_ORE_COUNT)
        return CONTRACT_FIT_NOT_DELIVERABLE;
    if (fragment->tier != ASTEROID_TIER_S)
        return CONTRACT_FIT_WRONG_TIER;
    if (fragment->commodity != contract->commodity)
        return CONTRACT_FIT_WRONG_COMMODITY;
    if ((mining_grade_t)fragment->grade < (mining_grade_t)contract->required_grade)
        return CONTRACT_FIT_GRADE_TOO_LOW;
    return CONTRACT_FIT_OK;
}

static inline contract_fit_reason_t contract_fit_asteroid(
    const contract_t *contract,
    const asteroid_t *asteroid)
{
    if (!contract || !contract->active) return CONTRACT_FIT_INACTIVE;
    if (!asteroid || !asteroid->active) return CONTRACT_FIT_NO_CARGO;
    if (contract->action == CONTRACT_TRACTOR)
        return contract_fit_fragment(contract, asteroid);
    if (contract->action != CONTRACT_FRACTURE)
        return CONTRACT_FIT_WRONG_ACTION;
    if (asteroid->tier == ASTEROID_TIER_S)
        return CONTRACT_FIT_WRONG_TIER;
    if (!contract_asteroid_target_matches(contract, asteroid))
        return CONTRACT_FIT_WRONG_COMMODITY;
    if (contract->target_index >= 0)
        return CONTRACT_FIT_OK;
    if (contract->commodity < COMMODITY_COUNT &&
        asteroid->commodity != contract->commodity) {
        return CONTRACT_FIT_WRONG_COMMODITY;
    }
    return CONTRACT_FIT_OK;
}

static inline int contract_fit_manifest_count(const contract_t *contract,
                                              const manifest_t *manifest)
{
    if (!manifest || !manifest->units) return 0;
    int count = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        if (contract_fit_is_ok(contract_fit_cargo_unit(contract,
                                                       &manifest->units[i]))) {
            count++;
        }
    }
    return count;
}

#endif /* CONTRACT_FIT_H */
