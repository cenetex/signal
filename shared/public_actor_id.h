#ifndef PUBLIC_ACTOR_ID_H
#define PUBLIC_ACTOR_ID_H

#include <stdbool.h>
#include <stdint.h>

#include "actor_principal.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /*
     * SHA-256 retains 128 bits of collision resistance. Do not truncate this
     * identifier merely because a shorter display suffix is sufficient in UI.
     */
    PUBLIC_ACTOR_ID_SIZE = 32,
    PUBLIC_ACTOR_ID_WIRE_SIZE = 1 + PUBLIC_ACTOR_ID_SIZE,
};

/*
 * NONE is an absent internal value and must not be emitted as attribution.
 * UNATTRIBUTED is the explicit sentinel for anonymous/current unknown actors.
 * LEGACY_UNATTRIBUTED marks a decoded legacy marker that has no unambiguous
 * principal proof. DERIVED is the only kind that carries identifier bytes.
 */
typedef enum {
    PUBLIC_ACTOR_ID_NONE = 0,
    PUBLIC_ACTOR_ID_UNATTRIBUTED = 1,
    PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED = 2,
    PUBLIC_ACTOR_ID_DERIVED = 3,
    PUBLIC_ACTOR_ID_KIND_COUNT = 4,
} public_actor_id_kind_t;

/*
 * Public, non-secret actor attribution. This is deliberately distinct from
 * actor_principal_t: the durable principal may contain a raw player pubkey,
 * while this value is a domain-separated digest safe for public events,
 * snapshots, logs, moderation exports, and UI models.
 */
typedef struct {
    uint8_t kind; /* public_actor_id_kind_t */
    uint8_t id[PUBLIC_ACTOR_ID_SIZE];
} public_actor_id_t;

#if defined(__cplusplus)
static_assert(sizeof(public_actor_id_t) == PUBLIC_ACTOR_ID_WIRE_SIZE,
              "public actor id layout must stay field-packed");
static_assert(PUBLIC_ACTOR_ID_KIND_COUNT == 4,
              "public actor id wire tags must remain explicitly versioned");
#else
_Static_assert(sizeof(public_actor_id_t) == PUBLIC_ACTOR_ID_WIRE_SIZE,
               "public actor id layout must stay field-packed");
_Static_assert(PUBLIC_ACTOR_ID_KIND_COUNT == 4,
               "public actor id wire tags must remain explicitly versioned");
#endif

public_actor_id_t public_actor_id_none(void);
public_actor_id_t public_actor_id_unattributed(void);
public_actor_id_t public_actor_id_legacy_unattributed(void);

/*
 * Derive:
 *   SHA256("SIGNAL-public-actor-id-v1" || principal_wire)
 *
 * Only concrete PLAYER/STATION/SYSTEM principals are accepted. NPC
 * principals are currently derived from session bearers and are rejected
 * until NPCs have a persisted, non-secret public birth identifier. NONE and
 * UNATTRIBUTED must be mapped to one of the explicit public sentinels by the
 * caller so an absent owner can never alias an attributed actor. This pure
 * transform does not itself prove provenance; authority code must obtain the
 * principal from its proof-gated resolver rather than constructing one from
 * untrusted wire fields.
 */
bool public_actor_id_from_principal(
    const actor_principal_t *principal,
    public_actor_id_t *out);

bool public_actor_id_is_canonical(const public_actor_id_t *actor);

/*
 * Invalid IDs and NONE never compare equal. The two unattributed sentinels
 * remain distinct, so legacy evidence cannot silently become current
 * anonymous attribution.
 */
bool public_actor_id_equal(
    const public_actor_id_t *left,
    const public_actor_id_t *right);

/*
 * Public encoders reject NONE: an emitted attribution must be either a
 * concrete derived actor or one of the explicit unattributed sentinels.
 * Unpack accepts canonical NONE so internal/legacy readers can recognize an
 * absent value and deliberately map it at their trust boundary.
 */
bool public_actor_id_pack(
    const public_actor_id_t *actor,
    uint8_t out[PUBLIC_ACTOR_ID_WIRE_SIZE]);
bool public_actor_id_unpack(
    const uint8_t in[PUBLIC_ACTOR_ID_WIRE_SIZE],
    public_actor_id_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PUBLIC_ACTOR_ID_H */
