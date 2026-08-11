#include "contract_ownership.h"

#include <limits.h>

static bool durable_owner_kind_allowed(const actor_principal_t *principal) {
    if (!actor_principal_is_canonical(principal)) return false;
    switch ((actor_principal_kind_t)principal->kind) {
        case ACTOR_PRINCIPAL_NONE:
        case ACTOR_PRINCIPAL_PLAYER:
        case ACTOR_PRINCIPAL_NPC:
            return true;
        case ACTOR_PRINCIPAL_UNATTRIBUTED:
        case ACTOR_PRINCIPAL_STATION:
        case ACTOR_PRINCIPAL_SYSTEM:
        case ACTOR_PRINCIPAL_KIND_COUNT:
        default:
            return false;
    }
}

static bool durable_owner_binding_is_valid(
    const actor_principal_t *principal,
    uint64_t quarantine_record_id) {
    if (!durable_owner_kind_allowed(principal)) return false;
    if (quarantine_record_id != 0)
        return principal->kind == ACTOR_PRINCIPAL_NONE;
    return true;
}

bool contract_ownership_is_valid(const contract_t *contract) {
    return contract &&
        durable_owner_binding_is_valid(
            &contract->claimed_by_principal,
            contract->claimed_by_quarantine_record_id);
}

bool delivery_ownership_is_valid(const delivery_shipment_t *shipment) {
    return shipment &&
        durable_owner_binding_is_valid(
            &shipment->debtor_principal,
            shipment->debtor_quarantine_record_id);
}

bool contract_ownership_is_open(const contract_t *contract) {
    return contract_ownership_is_valid(contract) &&
        contract->claimed_by_quarantine_record_id == 0 &&
        contract->claimed_by_principal.kind == ACTOR_PRINCIPAL_NONE;
}

static bool principal_matches_player(
    const actor_principal_t *principal,
    const world_t *world,
    int player_slot) {
    if (!world || player_slot < 0 ||
        player_slot >= MAX_PLAYERS) {
        return false;
    }
    const server_player_t *player =
        &world->players[player_slot];
    if (player->id != (uint8_t)player_slot)
        return false;
    actor_principal_t candidate = actor_principal_none();
    if (!actor_principal_from_verified_player(
            player, &candidate) ||
        !actor_principal_equal(principal, &candidate)) {
        return false;
    }
    actor_resolution_result_t resolved =
        world_resolve_player_principal(world, &candidate);
    return resolved.slot == player_slot &&
        (resolved.state == ACTOR_RESOLUTION_ONLINE ||
         resolved.state == ACTOR_RESOLUTION_GRACE);
}

static bool principal_matches_npc(
    const actor_principal_t *principal,
    const world_t *world,
    int npc_slot) {
    if (!world || npc_slot < 0 ||
        npc_slot >= MAX_NPC_SHIPS) {
        return false;
    }
    actor_principal_t candidate = actor_principal_none();
    if (!actor_principal_from_unique_npc_slot(
            world, npc_slot, &candidate, NULL) ||
        !actor_principal_equal(principal, &candidate)) {
        return false;
    }
    return true;
}

bool contract_ownership_matches_player(
    const contract_t *contract,
    const world_t *world,
    int player_slot) {
    return contract_ownership_is_valid(contract) &&
        contract->claimed_by_quarantine_record_id == 0 &&
        principal_matches_player(
            &contract->claimed_by_principal,
            world, player_slot);
}

bool contract_ownership_matches_npc(
    const contract_t *contract,
    const world_t *world,
    int npc_slot) {
    return contract_ownership_is_valid(contract) &&
        contract->claimed_by_quarantine_record_id == 0 &&
        principal_matches_npc(
            &contract->claimed_by_principal,
            world, npc_slot);
}

bool contract_ownership_try_claim_player(
    contract_t *contract,
    const world_t *world,
    int player_slot) {
    if (!world || player_slot < 0 ||
        player_slot >= MAX_PLAYERS ||
        !contract_ownership_is_valid(contract) ||
        contract->claimed_by_quarantine_record_id != 0) {
        return false;
    }
    const server_player_t *player =
        &world->players[player_slot];
    if (player->id != (uint8_t)player_slot)
        return false;
    actor_principal_t claimant = actor_principal_none();
    if (!actor_principal_from_verified_player(player, &claimant))
        return false;
    actor_resolution_result_t resolved =
        world_resolve_player_principal(world, &claimant);
    if (resolved.slot != player_slot ||
        (resolved.state != ACTOR_RESOLUTION_ONLINE &&
         resolved.state != ACTOR_RESOLUTION_GRACE)) {
        return false;
    }
    if (contract->claimed_by_principal.kind != ACTOR_PRINCIPAL_NONE &&
        !actor_principal_equal(
            &contract->claimed_by_principal, &claimant)) {
        return false;
    }
    contract->claimed_by_principal = claimant;
    contract->claimed_by =
        player->id <= INT8_MAX ? (int8_t)player->id : -1;
    return true;
}

bool contract_ownership_try_claim_npc(
    contract_t *contract,
    const world_t *world,
    int npc_slot) {
    if (!world || npc_slot < 0 ||
        npc_slot >= MAX_NPC_SHIPS ||
        !contract_ownership_is_valid(contract) ||
        contract->claimed_by_quarantine_record_id != 0) {
        return false;
    }
    actor_principal_t claimant = actor_principal_none();
    if (!actor_principal_from_unique_npc_slot(
            world, npc_slot, &claimant, NULL)) {
        return false;
    }
    if (contract->claimed_by_principal.kind != ACTOR_PRINCIPAL_NONE &&
        !actor_principal_equal(
            &contract->claimed_by_principal, &claimant)) {
        return false;
    }
    contract->claimed_by_principal = claimant;
    int code = MAX_PLAYERS + npc_slot;
    contract->claimed_by =
        code >= 0 && code <= INT8_MAX ? (int8_t)code : -1;
    return true;
}

void contract_ownership_commit_staged_claim(
    contract_t *contract,
    const contract_t *staged_contract) {
    if (!contract || !staged_contract) return;
    contract->claimed_by_principal =
        staged_contract->claimed_by_principal;
    contract->claimed_by_quarantine_record_id =
        staged_contract->claimed_by_quarantine_record_id;
    contract->claimed_by = staged_contract->claimed_by;
}

void contract_ownership_clear(contract_t *contract) {
    if (!contract) return;
    contract->claimed_by_principal = actor_principal_none();
    contract->claimed_by_quarantine_record_id = 0;
    contract->claimed_by = -1;
}

bool delivery_ownership_matches_player(
    const delivery_shipment_t *shipment,
    const world_t *world,
    int player_slot) {
    return delivery_ownership_is_valid(shipment) &&
        shipment->debtor_quarantine_record_id == 0 &&
        principal_matches_player(
            &shipment->debtor_principal,
            world, player_slot);
}

bool delivery_ownership_matches_npc(
    const delivery_shipment_t *shipment,
    const world_t *world,
    int npc_slot) {
    return delivery_ownership_is_valid(shipment) &&
        shipment->debtor_quarantine_record_id == 0 &&
        principal_matches_npc(
            &shipment->debtor_principal,
            world, npc_slot);
}

bool delivery_ownership_assign_player(
    delivery_shipment_t *shipment,
    const world_t *world,
    int player_slot) {
    if (!world || player_slot < 0 ||
        player_slot >= MAX_PLAYERS ||
        !delivery_ownership_is_valid(shipment) ||
        shipment->debtor_quarantine_record_id != 0 ||
        shipment->debtor_principal.kind != ACTOR_PRINCIPAL_NONE) {
        return false;
    }
    const server_player_t *player =
        &world->players[player_slot];
    if (player->id != (uint8_t)player_slot)
        return false;
    actor_principal_t debtor = actor_principal_none();
    if (!actor_principal_from_verified_player(player, &debtor))
        return false;
    actor_resolution_result_t resolved =
        world_resolve_player_principal(world, &debtor);
    if (resolved.slot != player_slot ||
        (resolved.state != ACTOR_RESOLUTION_ONLINE &&
         resolved.state != ACTOR_RESOLUTION_GRACE)) {
        return false;
    }
    shipment->debtor_principal = debtor;
    shipment->debtor_player = player->id;
    return true;
}

bool delivery_ownership_assign_npc(
    delivery_shipment_t *shipment,
    const world_t *world,
    int npc_slot) {
    if (!world || npc_slot < 0 ||
        npc_slot >= MAX_NPC_SHIPS ||
        !delivery_ownership_is_valid(shipment) ||
        shipment->debtor_quarantine_record_id != 0 ||
        shipment->debtor_principal.kind != ACTOR_PRINCIPAL_NONE) {
        return false;
    }
    actor_principal_t debtor = actor_principal_none();
    if (!actor_principal_from_unique_npc_slot(
            world, npc_slot, &debtor, NULL)) {
        return false;
    }
    int code = MAX_PLAYERS + npc_slot;
    if (code < 0 || code > UINT8_MAX) return false;
    shipment->debtor_principal = debtor;
    shipment->debtor_player = (uint8_t)code;
    return true;
}

void contract_ownership_refresh_projection(
    const world_t *world,
    contract_t *contract) {
    if (!contract) return;
    contract->claimed_by = -1;
    if (!contract_ownership_is_valid(contract) ||
        contract->claimed_by_quarantine_record_id != 0) {
        return;
    }
    if (contract->claimed_by_principal.kind !=
            ACTOR_PRINCIPAL_PLAYER &&
        contract->claimed_by_principal.kind !=
            ACTOR_PRINCIPAL_NPC) {
        return;
    }
    actor_resolution_result_t result =
        world_resolve_actor_principal(
            world, &contract->claimed_by_principal);
    if (result.slot < 0) return;
    int code = result.slot;
    if (contract->claimed_by_principal.kind == ACTOR_PRINCIPAL_NPC)
        code += MAX_PLAYERS;
    if (code <= INT8_MAX)
        contract->claimed_by = (int8_t)code;
}

void delivery_ownership_refresh_projection(
    const world_t *world,
    delivery_shipment_t *shipment) {
    if (!shipment) return;
    shipment->debtor_player = UINT8_MAX;
    if (!delivery_ownership_is_valid(shipment) ||
        shipment->debtor_quarantine_record_id != 0) {
        return;
    }
    if (shipment->debtor_principal.kind !=
            ACTOR_PRINCIPAL_PLAYER &&
        shipment->debtor_principal.kind !=
            ACTOR_PRINCIPAL_NPC) {
        return;
    }
    actor_resolution_result_t result =
        world_resolve_actor_principal(
            world, &shipment->debtor_principal);
    if (result.slot < 0) return;
    int code = result.slot;
    if (shipment->debtor_principal.kind == ACTOR_PRINCIPAL_NPC)
        code += MAX_PLAYERS;
    if (code <= UINT8_MAX)
        shipment->debtor_player = (uint8_t)code;
}
