#ifndef SERVER_ACTOR_PRINCIPAL_RESOLVER_H
#define SERVER_ACTOR_PRINCIPAL_RESOLVER_H

#include <stdbool.h>
#include <stdint.h>

#include "actor_principal.h"
#include "game_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACTOR_RESOLUTION_UNKNOWN = 0,
    ACTOR_RESOLUTION_ONLINE,
    ACTOR_RESOLUTION_GRACE,
    ACTOR_RESOLUTION_OFFLINE,
} actor_resolution_state_t;

typedef struct {
    actor_resolution_state_t state;
    int16_t slot;
} actor_resolution_result_t;

/*
 * Promote only a finalized, proof-of-possession-backed player identity into
 * the durable PLAYER namespace. Runtime slots, session tokens, and display
 * metadata are deliberately excluded.
 */
bool actor_principal_from_verified_player(
    const server_player_t *sp,
    actor_principal_t *out);

/*
 * NPC session_token is the persisted economic identity already carried
 * across world saves. Domain-separate and hash it before placing it in the
 * public principal namespace; runtime NPC slots, roles, and home stations
 * are deliberately excluded so reassignment cannot change durable ownership.
 */
bool actor_principal_from_npc(
    const npc_ship_t *npc,
    actor_principal_t *out);

/*
 * Slot-aware fast path: prove the active slot's persisted token is non-zero
 * and unique with bounded raw-token comparisons, then hash it exactly once.
 * Use this when the caller already knows the candidate NPC slot; keep the
 * generic resolver below for lookup from an unknown principal.
 * out_token_conflict is optional and distinguishes a duplicate persisted
 * token from an inactive/zero/invalid slot for legacy quarantine diagnostics.
 */
bool actor_principal_from_unique_npc_slot(
    const world_t *w,
    int npc_slot,
    actor_principal_t *out,
    bool *out_token_conflict);

/*
 * Construct and resolve immutable station actors. station_actor_id is
 * deliberately independent of the station's rotating signing key.
 */
bool actor_principal_from_station(
    const world_t *w,
    int station_slot,
    actor_principal_t *out);

/*
 * Backfill immutable station IDs for a fresh world or a pre-v79 load.
 * Existing non-zero IDs are preserved. Duplicate station IDs/actors fail
 * closed; legacy occupied stations with id==0 receive a fresh monotonic id.
 */
bool world_ensure_station_actor_ids(world_t *w);

/* Pre-v79 migration freezes the saved pre-rekey station public key into an
 * immutable ownership actor. It must run before station signing-key rederive. */
bool world_migrate_legacy_station_actor_ids(world_t *w);

/* Validate current persisted station IDs/actors without filling or rewriting. */
bool world_validate_station_actor_ids(const world_t *w);

/*
 * Project a canonical PLAYER principal onto current runtime state.
 * OFFLINE and UNKNOWN never expose a slot. Duplicate exact proven matches
 * are ambiguous and therefore resolve UNKNOWN rather than selecting one.
 */
actor_resolution_result_t world_resolve_player_principal(
    const world_t *w,
    const actor_principal_t *principal);

actor_resolution_result_t world_resolve_npc_principal(
    const world_t *w,
    const actor_principal_t *principal);

actor_resolution_result_t world_resolve_station_principal(
    const world_t *w,
    const actor_principal_t *principal);

/* Dispatch PLAYER/NPC/STATION through the matching stable resolver. */
actor_resolution_result_t world_resolve_actor_principal(
    const world_t *w,
    const actor_principal_t *principal);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_ACTOR_PRINCIPAL_RESOLVER_H */
