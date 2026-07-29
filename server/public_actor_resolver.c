#include "public_actor_resolver.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

_Static_assert(MAX_PLAYERS <= INT16_MAX,
               "public actor result slot must represent every player");
_Static_assert(MAX_NPC_SHIPS <= INT16_MAX,
               "public actor result slot must represent every NPC");
_Static_assert(MAX_STATIONS <= INT16_MAX,
               "public actor result slot must represent every station");

typedef struct {
    bool matched;
    bool ambiguous;
    bool has_runtime_record;
    actor_resolution_state_t state;
    int16_t slot;
    actor_principal_t principal;
} public_actor_match_t;

static public_actor_resolution_result_t public_actor_unknown(void) {
    public_actor_resolution_result_t result = {
        .state = ACTOR_RESOLUTION_UNKNOWN,
        .slot = -1,
        .principal = {0},
    };
    return result;
}

static bool public_actor_derive(
    const actor_principal_t *principal,
    public_actor_id_t *out) {
    if (out) *out = public_actor_id_none();
    return out &&
        public_actor_id_from_principal(principal, out);
}

bool public_actor_id_from_verified_player(
    const server_player_t *player,
    public_actor_id_t *out) {
    actor_principal_t principal = actor_principal_none();
    if (out) *out = public_actor_id_none();
    return out &&
        actor_principal_from_verified_player(player, &principal) &&
        public_actor_derive(&principal, out);
}

bool public_actor_id_from_verified_player_token(
    const world_t *world,
    const uint8_t token[8],
    public_actor_id_t *out,
    int *out_player_slot) {
    if (out) *out = public_actor_id_none();
    if (out_player_slot) *out_player_slot = -1;
    if (!world || !token || !out) return false;

    uint8_t any = 0;
    for (size_t i = 0; i < 8; i++) any |= token[i];
    if (any == 0) return false;

    int match = -1;
    for (int slot = 0; slot < MAX_PLAYERS; slot++) {
        const server_player_t *player = &world->players[slot];
        if (!player->session_ready ||
            memcmp(player->session_token, token, 8) != 0) {
            continue;
        }
        if (match >= 0) return false;
        match = slot;
    }
    if (match < 0 ||
        !public_actor_id_from_verified_player(
            &world->players[match], out)) {
        return false;
    }
    if (out_player_slot) *out_player_slot = match;
    return true;
}

bool public_actor_id_from_unique_npc_slot(
    const world_t *world,
    int npc_slot,
    public_actor_id_t *out) {
    if (out) *out = public_actor_id_none();
    (void)world;
    (void)npc_slot;
    return false;
}

bool public_actor_id_from_station(
    const world_t *world,
    int station_slot,
    public_actor_id_t *out) {
    actor_principal_t principal = actor_principal_none();
    if (out) *out = public_actor_id_none();
    return out &&
        actor_principal_from_station(
            world, station_slot, &principal) &&
        public_actor_derive(&principal, out);
}

bool public_actor_id_from_ledger_projection(
    const world_t *world,
    const uint8_t ledger_key[ACTOR_PRINCIPAL_ID_SIZE],
    public_actor_id_t *out) {
    if (out) *out = public_actor_id_none();
    if (!world || !ledger_key || !out) return false;

    /*
     * v45 and token-era runtime rows used token[8] || zero[24]. Never hash
     * this low-entropy bearer into a stable public identifier: doing so would
     * preserve a cheap offline token oracle even after hiding the bytes.
     */
    uint8_t legacy_suffix = 0;
    for (size_t i = 8; i < ACTOR_PRINCIPAL_ID_SIZE; i++)
        legacy_suffix |= ledger_key[i];
    if (legacy_suffix == 0) {
        *out = public_actor_id_legacy_unattributed();
        return true;
    }

    actor_principal_t principal = actor_principal_none();
    public_actor_id_t candidate = public_actor_id_none();
    if (!actor_principal_from_stable_id(
            ACTOR_PRINCIPAL_PLAYER, ledger_key, &principal) ||
        !public_actor_derive(&principal, &candidate)) {
        return false;
    }
    public_actor_resolution_result_t resolved =
        world_resolve_public_actor_id(world, &candidate);
    if (resolved.state == ACTOR_RESOLUTION_UNKNOWN ||
        resolved.principal.kind !=
            (uint8_t)ACTOR_PRINCIPAL_PLAYER ||
        memcmp(resolved.principal.id, ledger_key,
               ACTOR_PRINCIPAL_ID_SIZE) != 0) {
        /*
         * Arbitrary or corrupt 32-byte ledger material must not become a
         * public actor merely because it is the right length. The durable
         * registry/proof inventory is the trust boundary.
         */
        *out = public_actor_id_legacy_unattributed();
        return true;
    }
    *out = candidate;
    return true;
}

static bool public_actor_query_matches(
    const public_actor_id_t *query,
    const actor_principal_t *principal) {
    public_actor_id_t candidate = public_actor_id_none();
    return public_actor_id_from_principal(principal, &candidate) &&
        public_actor_id_equal(query, &candidate);
}

/*
 * Registry rows are durable evidence for one PLAYER principal, but they are
 * not runtime records. Repeated rows for the same pubkey therefore collapse
 * into the same candidate. Any second runtime record remains ambiguous:
 * choosing a slot in that state could transfer authority to the wrong actor.
 */
static void public_actor_merge_match(
    public_actor_match_t *match,
    const actor_principal_t *principal,
    actor_resolution_state_t state,
    int16_t slot,
    bool runtime_record) {
    if (!match || !principal || match->ambiguous) return;
    if (!match->matched) {
        match->matched = true;
        match->has_runtime_record = runtime_record;
        match->state = state;
        match->slot = slot;
        match->principal = *principal;
        return;
    }
    if (!actor_principal_equal(&match->principal, principal)) {
        match->ambiguous = true;
        return;
    }
    if (!runtime_record) {
        return;
    }
    if (match->has_runtime_record) {
        match->ambiguous = true;
        return;
    }
    match->has_runtime_record = true;
    match->state = state;
    match->slot = slot;
}

static actor_resolution_state_t public_actor_player_state(
    const server_player_t *player) {
    if (!player || !player->connected)
        return ACTOR_RESOLUTION_OFFLINE;
    if (player->grace_period)
        return ACTOR_RESOLUTION_GRACE;
    return ACTOR_RESOLUTION_ONLINE;
}

static void public_actor_scan_players(
    const world_t *world,
    const public_actor_id_t *query,
    public_actor_match_t *match) {
    for (int slot = 0; slot < MAX_PLAYERS; slot++) {
        const server_player_t *player = &world->players[slot];
        actor_principal_t principal = actor_principal_none();
        if (!actor_principal_from_verified_player(
                player, &principal) ||
            !public_actor_query_matches(query, &principal)) {
            continue;
        }
        actor_resolution_state_t state =
            public_actor_player_state(player);
        public_actor_merge_match(
            match, &principal, state,
            state == ACTOR_RESOLUTION_OFFLINE
                ? -1
                : (int16_t)slot,
            true);
    }
}

static bool public_actor_registry_pubkey_seen(
    const world_t *world,
    int row) {
    for (int prior = 0; prior < row; prior++) {
        if (world->pubkey_registry[prior].in_use &&
            memcmp(world->pubkey_registry[prior].pubkey,
                   world->pubkey_registry[row].pubkey,
                   ACTOR_PRINCIPAL_ID_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static void public_actor_scan_registry(
    const world_t *world,
    const public_actor_id_t *query,
    public_actor_match_t *match) {
    for (int row = 0; row < MAX_PLAYERS; row++) {
        if (!world->pubkey_registry[row].in_use ||
            public_actor_registry_pubkey_seen(world, row)) {
            continue;
        }
        actor_principal_t principal = actor_principal_none();
        if (!actor_principal_from_stable_id(
                ACTOR_PRINCIPAL_PLAYER,
                world->pubkey_registry[row].pubkey,
                &principal) ||
            !public_actor_query_matches(query, &principal)) {
            continue;
        }
        public_actor_merge_match(
            match, &principal, ACTOR_RESOLUTION_OFFLINE, -1, false);
    }
}

static void public_actor_scan_stations(
    const world_t *world,
    const public_actor_id_t *query,
    public_actor_match_t *match) {
    for (int slot = 0; slot < MAX_STATIONS; slot++) {
        actor_principal_t principal = actor_principal_none();
        if (!actor_principal_from_station(
                world, slot, &principal) ||
            !public_actor_query_matches(query, &principal)) {
            continue;
        }
        public_actor_merge_match(
            match, &principal, ACTOR_RESOLUTION_ONLINE,
            (int16_t)slot, true);
    }
}

public_actor_resolution_result_t world_resolve_public_actor_id(
    const world_t *world,
    const public_actor_id_t *actor) {
    public_actor_resolution_result_t unknown = public_actor_unknown();
    if (!world || !public_actor_id_is_canonical(actor) ||
        actor->kind != (uint8_t)PUBLIC_ACTOR_ID_DERIVED) {
        return unknown;
    }

    public_actor_match_t match = {
        .state = ACTOR_RESOLUTION_UNKNOWN,
        .slot = -1,
        .principal = {0},
    };
    public_actor_scan_players(world, actor, &match);
    public_actor_scan_registry(world, actor, &match);
    public_actor_scan_stations(world, actor, &match);

    if (!match.matched || match.ambiguous ||
        !actor_principal_is_canonical(&match.principal) ||
        match.principal.kind == (uint8_t)ACTOR_PRINCIPAL_NONE ||
        match.principal.kind ==
            (uint8_t)ACTOR_PRINCIPAL_UNATTRIBUTED) {
        return unknown;
    }

    public_actor_resolution_result_t result = {
        .state = match.state,
        .slot = match.state == ACTOR_RESOLUTION_OFFLINE
            ? -1
            : match.slot,
        .principal = match.principal,
    };
    return result;
}
