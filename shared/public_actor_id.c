#include "public_actor_id.h"

#include <stddef.h>
#include <string.h>

#include "sha256.h"

static bool public_actor_bytes_are_zero(
    const uint8_t bytes[PUBLIC_ACTOR_ID_SIZE]) {
    if (!bytes) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < PUBLIC_ACTOR_ID_SIZE; i++)
        any |= bytes[i];
    return any == 0;
}

static public_actor_id_t public_actor_sentinel(
    public_actor_id_kind_t kind) {
    public_actor_id_t actor = {0};
    actor.kind = (uint8_t)kind;
    return actor;
}

public_actor_id_t public_actor_id_none(void) {
    return public_actor_sentinel(PUBLIC_ACTOR_ID_NONE);
}

public_actor_id_t public_actor_id_unattributed(void) {
    return public_actor_sentinel(PUBLIC_ACTOR_ID_UNATTRIBUTED);
}

public_actor_id_t public_actor_id_legacy_unattributed(void) {
    return public_actor_sentinel(PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED);
}

bool public_actor_id_is_canonical(const public_actor_id_t *actor) {
    if (!actor ||
        actor->kind >= (uint8_t)PUBLIC_ACTOR_ID_KIND_COUNT) {
        return false;
    }
    bool zero = public_actor_bytes_are_zero(actor->id);
    switch ((public_actor_id_kind_t)actor->kind) {
    case PUBLIC_ACTOR_ID_NONE:
    case PUBLIC_ACTOR_ID_UNATTRIBUTED:
    case PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED:
        return zero;
    case PUBLIC_ACTOR_ID_DERIVED:
        return !zero;
    case PUBLIC_ACTOR_ID_KIND_COUNT:
    default:
        return false;
    }
}

bool public_actor_id_from_principal(
    const actor_principal_t *principal,
    public_actor_id_t *out) {
    static const char domain[] = "SIGNAL-public-actor-id-v1";
    actor_principal_t principal_copy = actor_principal_none();
    uint8_t principal_wire[ACTOR_PRINCIPAL_WIRE_SIZE];
    public_actor_id_t actor = {0};

    if (!out) return false;
    /* Preserve the principal when callers reuse one 33-byte storage slot. */
    if (principal) principal_copy = *principal;
    *out = public_actor_id_none();
    if (!actor_principal_is_canonical(&principal_copy) ||
        principal_copy.kind == (uint8_t)ACTOR_PRINCIPAL_NONE ||
        principal_copy.kind == (uint8_t)ACTOR_PRINCIPAL_UNATTRIBUTED ||
        principal_copy.kind == (uint8_t)ACTOR_PRINCIPAL_NPC ||
        !actor_principal_pack(&principal_copy, principal_wire)) {
        return false;
    }

    sha256_ctx_t hash;
    sha256_init(&hash);
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    sha256_update(&hash, principal_wire, sizeof(principal_wire));
    sha256_final(&hash, actor.id);
    memset(principal_wire, 0, sizeof(principal_wire));

    actor.kind = (uint8_t)PUBLIC_ACTOR_ID_DERIVED;
    if (!public_actor_id_is_canonical(&actor)) {
        memset(&actor, 0, sizeof(actor));
        return false;
    }
    *out = actor;
    return true;
}

bool public_actor_id_equal(
    const public_actor_id_t *left,
    const public_actor_id_t *right) {
    if (!public_actor_id_is_canonical(left) ||
        !public_actor_id_is_canonical(right) ||
        left->kind == (uint8_t)PUBLIC_ACTOR_ID_NONE ||
        right->kind == (uint8_t)PUBLIC_ACTOR_ID_NONE) {
        return false;
    }
    return left->kind == right->kind &&
        memcmp(left->id, right->id, sizeof(left->id)) == 0;
}

bool public_actor_id_pack(
    const public_actor_id_t *actor,
    uint8_t out[PUBLIC_ACTOR_ID_WIRE_SIZE]) {
    if (!out) return false;
    if (!public_actor_id_is_canonical(actor) ||
        actor->kind == (uint8_t)PUBLIC_ACTOR_ID_NONE) {
        memset(out, 0, PUBLIC_ACTOR_ID_WIRE_SIZE);
        return false;
    }
    public_actor_id_t copy = *actor;
    out[0] = copy.kind;
    memcpy(&out[1], copy.id, PUBLIC_ACTOR_ID_SIZE);
    return true;
}

bool public_actor_id_unpack(
    const uint8_t in[PUBLIC_ACTOR_ID_WIRE_SIZE],
    public_actor_id_t *out) {
    if (!out) return false;
    if (!in) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    public_actor_id_t actor = {0};
    actor.kind = in[0];
    memcpy(actor.id, &in[1], sizeof(actor.id));
    if (!public_actor_id_is_canonical(&actor)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = actor;
    return true;
}
