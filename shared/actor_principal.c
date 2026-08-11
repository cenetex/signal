#include "actor_principal.h"

#include <string.h>

static bool actor_principal_id_is_zero(
    const uint8_t id[ACTOR_PRINCIPAL_ID_SIZE]) {
    if (!id) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < ACTOR_PRINCIPAL_ID_SIZE; i++)
        any |= id[i];
    return any == 0;
}

actor_principal_t actor_principal_none(void) {
    return (actor_principal_t){0};
}

actor_principal_t actor_principal_unattributed(void) {
    actor_principal_t principal = {0};
    principal.kind = (uint8_t)ACTOR_PRINCIPAL_UNATTRIBUTED;
    return principal;
}

bool actor_principal_is_canonical(
    const actor_principal_t *principal) {
    if (!principal ||
        principal->kind >= (uint8_t)ACTOR_PRINCIPAL_KIND_COUNT) {
        return false;
    }

    bool id_is_zero = actor_principal_id_is_zero(principal->id);
    switch ((actor_principal_kind_t)principal->kind) {
        case ACTOR_PRINCIPAL_NONE:
        case ACTOR_PRINCIPAL_UNATTRIBUTED:
            return id_is_zero;
        case ACTOR_PRINCIPAL_PLAYER:
        case ACTOR_PRINCIPAL_NPC:
        case ACTOR_PRINCIPAL_STATION:
        case ACTOR_PRINCIPAL_SYSTEM:
            return !id_is_zero;
        case ACTOR_PRINCIPAL_KIND_COUNT:
        default:
            return false;
    }
}

bool actor_principal_from_stable_id(
    actor_principal_kind_t kind,
    const uint8_t id[ACTOR_PRINCIPAL_ID_SIZE],
    actor_principal_t *out) {
    if (!out) return false;
    if (!id ||
        kind < ACTOR_PRINCIPAL_NONE ||
        kind >= ACTOR_PRINCIPAL_KIND_COUNT) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    /* Copy before touching out so retagging from out->id is alias-safe. */
    actor_principal_t principal = {0};
    principal.kind = (uint8_t)kind;
    memcpy(principal.id, id, sizeof(principal.id));
    if (!actor_principal_is_canonical(&principal)) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    *out = principal;
    return true;
}

bool actor_principal_equal(
    const actor_principal_t *left,
    const actor_principal_t *right) {
    if (!actor_principal_is_canonical(left) ||
        !actor_principal_is_canonical(right)) {
        return false;
    }
    return left->kind == right->kind &&
        memcmp(left->id, right->id, sizeof(left->id)) == 0;
}

bool actor_principal_pack(
    const actor_principal_t *principal,
    uint8_t out[ACTOR_PRINCIPAL_WIRE_SIZE]) {
    if (!out) return false;
    if (!actor_principal_is_canonical(principal)) {
        memset(out, 0, ACTOR_PRINCIPAL_WIRE_SIZE);
        return false;
    }

    /* Preserve the value when principal aliases the 33-byte output. */
    actor_principal_t copy = *principal;
    out[0] = copy.kind;
    memcpy(&out[1], copy.id, ACTOR_PRINCIPAL_ID_SIZE);
    return true;
}

bool actor_principal_unpack(
    const uint8_t in[ACTOR_PRINCIPAL_WIRE_SIZE],
    actor_principal_t *out) {
    if (!out) return false;
    if (!in) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    /* Read the complete wire value before clearing a possibly aliased out. */
    actor_principal_t principal = {0};
    principal.kind = in[0];
    memcpy(principal.id, &in[1], sizeof(principal.id));
    if (!actor_principal_is_canonical(&principal)) {
        memset(out, 0, sizeof(*out));
        return false;
    }

    *out = principal;
    return true;
}
