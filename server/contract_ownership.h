#ifndef SERVER_CONTRACT_OWNERSHIP_H
#define SERVER_CONTRACT_OWNERSHIP_H

#include <stdbool.h>

#include "actor_principal_resolver.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Durable contract/delivery ownership boundary.
 *
 * The int8/uint8 slot fields retained in the structs are projections for old
 * client/test surfaces only. Every authorization decision must enter through
 * these helpers and compare canonical principals.
 */
bool contract_ownership_is_valid(const contract_t *contract);
bool delivery_ownership_is_valid(const delivery_shipment_t *shipment);

bool contract_ownership_is_open(const contract_t *contract);
bool contract_ownership_matches_player(
    const contract_t *contract,
    const world_t *world,
    int player_slot);
bool contract_ownership_matches_npc(
    const contract_t *contract,
    const world_t *world,
    int npc_slot);

/*
 * Claim an open contract, or accept an idempotent claim by the same actor.
 * Unverified players, invalid NPC identities, foreign claimants, and
 * quarantined legacy rows fail without mutating the contract.
 */
bool contract_ownership_try_claim_player(
    contract_t *contract,
    const world_t *world,
    int player_slot);
bool contract_ownership_try_claim_npc(
    contract_t *contract,
    const world_t *world,
    int npc_slot);

/*
 * Commit only the ownership fields from a copy that was accepted by one of
 * the try-claim helpers. This is deliberately allocation-free and infallible
 * for non-NULL inputs so receipt/log preflights can finish before the durable
 * claim and the associated gameplay state swap are committed together.
 */
void contract_ownership_commit_staged_claim(
    contract_t *contract,
    const contract_t *staged_contract);

void contract_ownership_clear(contract_t *contract);

bool delivery_ownership_matches_player(
    const delivery_shipment_t *shipment,
    const world_t *world,
    int player_slot);
bool delivery_ownership_matches_npc(
    const delivery_shipment_t *shipment,
    const world_t *world,
    int npc_slot);

/*
 * Assign a freshly staged shipment debtor. These functions require an
 * unowned, non-quarantined row and fail without mutation otherwise.
 */
bool delivery_ownership_assign_player(
    delivery_shipment_t *shipment,
    const world_t *world,
    int player_slot);
bool delivery_ownership_assign_npc(
    delivery_shipment_t *shipment,
    const world_t *world,
    int npc_slot);

/* Recompute compatibility projections from live resolver state. */
void contract_ownership_refresh_projection(
    const world_t *world,
    contract_t *contract);
void delivery_ownership_refresh_projection(
    const world_t *world,
    delivery_shipment_t *shipment);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CONTRACT_OWNERSHIP_H */
