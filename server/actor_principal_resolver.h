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

actor_resolution_result_t world_resolve_station_principal(
    const world_t *w,
    const actor_principal_t *principal);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_ACTOR_PRINCIPAL_RESOLVER_H */
