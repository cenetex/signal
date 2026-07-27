#include "actor_principal_resolver.h"

#include <limits.h>
#include <string.h>

_Static_assert(MAX_PLAYERS <= INT16_MAX,
               "actor resolution slot must represent every player");

static actor_resolution_result_t actor_resolution(
    actor_resolution_state_t state,
    int16_t slot) {
    actor_resolution_result_t result = {
        .state = state,
        .slot = slot,
    };
    return result;
}

bool actor_principal_from_verified_player(
    const server_player_t *sp,
    actor_principal_t *out) {
    if (out) *out = actor_principal_none();
    if (!sp || !out ||
        !server_player_can_use_pubkey_persistence(sp) ||
        !sp->pubkey_identity_finalized) {
        return false;
    }
    return actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, sp->pubkey, out);
}

actor_resolution_result_t world_resolve_player_principal(
    const world_t *w,
    const actor_principal_t *principal) {
    actor_resolution_result_t unknown =
        actor_resolution(ACTOR_RESOLUTION_UNKNOWN, -1);
    if (!w || !principal ||
        principal->kind != ACTOR_PRINCIPAL_PLAYER ||
        !actor_principal_is_canonical(principal)) {
        return unknown;
    }

    int matched_slot = -1;
    for (int slot = 0; slot < MAX_PLAYERS; slot++) {
        const server_player_t *sp = &w->players[slot];
        if (!server_player_can_use_pubkey_persistence(sp) ||
            !sp->pubkey_identity_finalized ||
            memcmp(sp->pubkey, principal->id,
                   ACTOR_PRINCIPAL_ID_SIZE) != 0) {
            continue;
        }
        if (matched_slot >= 0)
            return unknown;
        matched_slot = slot;
    }

    if (matched_slot < 0)
        return actor_resolution(ACTOR_RESOLUTION_OFFLINE, -1);

    const server_player_t *matched = &w->players[matched_slot];
    if (matched->connected && matched->grace_period) {
        return actor_resolution(
            ACTOR_RESOLUTION_GRACE, (int16_t)matched_slot);
    }
    if (matched->connected) {
        return actor_resolution(
            ACTOR_RESOLUTION_ONLINE, (int16_t)matched_slot);
    }
    return actor_resolution(ACTOR_RESOLUTION_OFFLINE, -1);
}
