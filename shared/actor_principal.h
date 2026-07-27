#ifndef ACTOR_PRINCIPAL_H
#define ACTOR_PRINCIPAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ACTOR_PRINCIPAL_ID_SIZE = 32,
    ACTOR_PRINCIPAL_WIRE_SIZE = 1 + ACTOR_PRINCIPAL_ID_SIZE,
};

/*
 * Stable actor namespaces. NONE is the zero/default value and means that no
 * principal is present. UNATTRIBUTED is an explicit anonymous owner and must
 * not be conflated with NONE.
 */
typedef enum {
    ACTOR_PRINCIPAL_NONE = 0,
    ACTOR_PRINCIPAL_UNATTRIBUTED = 1,
    ACTOR_PRINCIPAL_PLAYER = 2,
    ACTOR_PRINCIPAL_NPC = 3,
    ACTOR_PRINCIPAL_STATION = 4,
    ACTOR_PRINCIPAL_SYSTEM = 5,
    ACTOR_PRINCIPAL_KIND_COUNT = 6,
} actor_principal_kind_t;

/*
 * Canonical durable actor identity. Runtime pool or transport slots are never
 * stored here. The kind domain-separates equal 32-byte identifiers belonging
 * to different actor namespaces. The fixed layout is an in-memory invariant,
 * not permission to fwrite the struct; durable and wire records must use the
 * explicit pack/unpack functions below.
 */
typedef struct {
    uint8_t kind; /* actor_principal_kind_t */
    uint8_t id[ACTOR_PRINCIPAL_ID_SIZE];
} actor_principal_t;

#if defined(__cplusplus)
static_assert(sizeof(actor_principal_t) == ACTOR_PRINCIPAL_WIRE_SIZE,
              "actor principal layout must stay field-packed");
static_assert(ACTOR_PRINCIPAL_KIND_COUNT == 6,
              "actor principal wire tags must remain explicitly versioned");
#else
_Static_assert(sizeof(actor_principal_t) == ACTOR_PRINCIPAL_WIRE_SIZE,
               "actor principal layout must stay field-packed");
_Static_assert(ACTOR_PRINCIPAL_KIND_COUNT == 6,
               "actor principal wire tags must remain explicitly versioned");
#endif

actor_principal_t actor_principal_none(void);
actor_principal_t actor_principal_unattributed(void);

/*
 * Construct a canonical principal from a stable identifier. Sentinel kinds
 * require an all-zero identifier; PLAYER, NPC, STATION, and SYSTEM require a
 * non-zero identifier. On failure, out is zeroed when it is non-NULL.
 */
bool actor_principal_from_stable_id(
    actor_principal_kind_t kind,
    const uint8_t id[ACTOR_PRINCIPAL_ID_SIZE],
    actor_principal_t *out);

bool actor_principal_is_canonical(const actor_principal_t *principal);

/*
 * Invalid principals never compare equal. Canonical values compare both kind
 * and identifier, so the same identifier in two namespaces is not equal.
 */
bool actor_principal_equal(
    const actor_principal_t *left,
    const actor_principal_t *right);

/*
 * Explicit stable encoding: kind followed by the 32 identifier bytes.
 * Fallible operations zero their output before returning false.
 */
bool actor_principal_pack(
    const actor_principal_t *principal,
    uint8_t out[ACTOR_PRINCIPAL_WIRE_SIZE]);
bool actor_principal_unpack(
    const uint8_t in[ACTOR_PRINCIPAL_WIRE_SIZE],
    actor_principal_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ACTOR_PRINCIPAL_H */
