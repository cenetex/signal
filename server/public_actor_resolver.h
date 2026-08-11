#ifndef SERVER_PUBLIC_ACTOR_RESOLVER_H
#define SERVER_PUBLIC_ACTOR_RESOLVER_H

#include <stdbool.h>
#include <stdint.h>

#include "actor_principal_resolver.h"
#include "public_actor_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A successful reverse lookup returns both the durable principal and its
 * current runtime disposition. UNKNOWN always carries principal NONE and
 * slot -1. OFFLINE carries a proven PLAYER principal but never a stale slot.
 */
typedef struct {
    actor_resolution_state_t state;
    int16_t slot;
    actor_principal_t principal;
} public_actor_resolution_result_t;

/*
 * Trust-boundary constructors. These deliberately accept actor records, not
 * raw identifiers: a player must have completed proof finalization and a
 * station must carry its immutable actor identifier.
 *
 * NPC session tokens are bearer credentials, not public birth identities.
 * public_actor_id_from_unique_npc_slot is retained as an explicit fail-closed
 * compatibility boundary: it returns false/NONE until NPCs have a persisted,
 * non-secret birth identifier. Changing an NPC token therefore cannot change
 * or create public identity bytes.
 *
 * Every failure replaces out with PUBLIC_ACTOR_ID_NONE.
 */
bool public_actor_id_from_verified_player(
    const server_player_t *player,
    public_actor_id_t *out);

/*
 * Resolve a server-internal bearer to exactly one proof-finalized player,
 * then project that player's stable pubkey-backed identity. The token is only
 * a bounded runtime locator and is never hashed or copied into the output.
 * Duplicate, zero, stale-unproven, or unknown tokens fail closed to NONE.
 * out_player_slot is optional and becomes -1 on failure.
 */
bool public_actor_id_from_verified_player_token(
    const world_t *world,
    const uint8_t token[8],
    public_actor_id_t *out,
    int *out_player_slot);

bool public_actor_id_from_unique_npc_slot(
    const world_t *world,
    int npc_slot,
    public_actor_id_t *out);

bool public_actor_id_from_station(
    const world_t *world,
    int station_slot,
    public_actor_id_t *out);

/*
 * Project one persisted station-ledger key into a value safe for public
 * diagnostics. Current rows contain a 32-byte player pubkey. Legacy rows
 * instead contain an 8-byte session bearer followed by 24 zero bytes; those
 * rows deliberately collapse to LEGACY_UNATTRIBUTED rather than hashing the
 * bearer into an offline verifier.
 *
 * A full key is projected only when the world's proof/registry inventory can
 * resolve it unambiguously; arbitrary 32-byte ledger material also collapses
 * to LEGACY_UNATTRIBUTED. This is presentation-only migration logic: a
 * DERIVED result does not confer authority and must never replace the
 * proof-gated constructors above. Every failure replaces out with
 * PUBLIC_ACTOR_ID_NONE.
 */
bool public_actor_id_from_ledger_projection(
    const world_t *world,
    const uint8_t ledger_key[ACTOR_PRINCIPAL_ID_SIZE],
    public_actor_id_t *out);

/*
 * Reverse a canonical derived public ID through bounded stable-identity
 * inventories. Player pubkeys (including persisted registry rows) and
 * immutable station actor IDs are the only candidates. NPC token-derived
 * principals are deliberately excluded from public resolution.
 * Session tokens, callsigns, and runtime slots are never candidate identity.
 *
 * Identical duplicate registry pubkeys are one offline actor even if their
 * stale token projections differ. Multiple runtime records for one actor, or
 * two distinct principals whose public digests collide, fail closed UNKNOWN.
 * NONE and the unattributed sentinels never resolve to authority.
 */
public_actor_resolution_result_t world_resolve_public_actor_id(
    const world_t *world,
    const public_actor_id_t *actor);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_PUBLIC_ACTOR_RESOLVER_H */
