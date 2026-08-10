#include "actor_principal_resolver.h"

#include <limits.h>
#include <string.h>

#include "sha256.h"

_Static_assert(MAX_PLAYERS <= INT16_MAX,
               "actor resolution slot must represent every player");
_Static_assert(MAX_STATIONS <= INT16_MAX,
               "actor resolution slot must represent every station");

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

bool actor_principal_from_npc(
    const npc_ship_t *npc,
    actor_principal_t *out) {
    static const char domain[] = "SIGNAL-npc-actor-v1";
    if (out) *out = actor_principal_none();
    if (!npc || !out || !npc->active) return false;

    uint8_t any = 0;
    for (size_t i = 0; i < sizeof(npc->session_token); i++)
        any |= npc->session_token[i];
    if (any == 0) return false;

    uint8_t id[ACTOR_PRINCIPAL_ID_SIZE];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain) - 1);
    sha256_update(&ctx, npc->session_token,
                  sizeof(npc->session_token));
    sha256_final(&ctx, id);
    return actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NPC, id, out);
}

bool actor_principal_from_unique_npc_slot(
    const world_t *w,
    int npc_slot,
    actor_principal_t *out,
    bool *out_token_conflict) {
    if (out) *out = actor_principal_none();
    if (out_token_conflict) *out_token_conflict = false;
    if (!w || !out || npc_slot < 0 ||
        npc_slot >= MAX_NPC_SHIPS) {
        return false;
    }
    const npc_ship_t *npc = &w->npc_ships[npc_slot];
    if (!npc->active) return false;

    uint8_t any = 0;
    for (size_t i = 0; i < sizeof(npc->session_token); i++)
        any |= npc->session_token[i];
    if (any == 0) return false;

    for (int other_slot = 0;
         other_slot < MAX_NPC_SHIPS;
         other_slot++) {
        if (other_slot == npc_slot) continue;
        const npc_ship_t *other =
            &w->npc_ships[other_slot];
        if (!other->active) continue;
        if (memcmp(
                other->session_token,
                npc->session_token,
                sizeof(npc->session_token)) == 0) {
            if (out_token_conflict)
                *out_token_conflict = true;
            return false;
        }
    }
    return actor_principal_from_npc(npc, out);
}

static bool station_actor_bytes_nonzero(const uint8_t *bytes, size_t len) {
    if (!bytes) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

static bool station_actor_slot_occupied(const station_t *station) {
    return station &&
        (station_exists(station) || station->planned ||
         station_actor_bytes_nonzero(
             station->station_pubkey, sizeof(station->station_pubkey)));
}

static void station_actor_hash_u32(sha256_ctx_t *ctx, uint32_t value) {
    uint8_t encoded[4] = {
        (uint8_t)(value & 0xffu),
        (uint8_t)((value >> 8) & 0xffu),
        (uint8_t)((value >> 16) & 0xffu),
        (uint8_t)((value >> 24) & 0xffu),
    };
    sha256_update(ctx, encoded, sizeof(encoded));
}

static void station_actor_id_derive(
    const station_t *station,
    uint32_t station_id,
    uint8_t out[ACTOR_PRINCIPAL_ID_SIZE]) {
    static const char domain[] = "SIGNAL-station-actor-v1";
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain) - 1);
    /*
     * Freeze the first public signing identity into a distinct ownership
     * namespace. The resulting actor ID is persisted and never recomputed
     * after a signing-key rotation; station_id separates intentionally
     * duplicated/imported genesis keys.
     */
    sha256_update(&ctx, station->station_pubkey,
                  sizeof(station->station_pubkey));
    station_actor_hash_u32(&ctx, station_id);
    sha256_final(&ctx, out);
}

static void station_actor_id_derive_legacy(
    const station_t *station,
    uint8_t out[ACTOR_PRINCIPAL_ID_SIZE]) {
    static const char domain[] = "SIGNAL-station-actor-legacy-v1";
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain) - 1);
    sha256_update(&ctx, station->station_pubkey,
                  sizeof(station->station_pubkey));
    sha256_final(&ctx, out);
}

static bool world_fill_station_actor_ids(
    world_t *w,
    bool legacy_saved_identity) {
    if (!w) return false;
    uint32_t staged_ids[MAX_STATIONS] = {0};
    uint8_t staged_actors[MAX_STATIONS][ACTOR_PRINCIPAL_ID_SIZE] = {{0}};
    uint32_t staged_next_id = w->next_station_id;
    if (staged_next_id == 0) staged_next_id = 1;

    uint32_t max_existing_id = 0;
    for (int slot = 0; slot < MAX_STATIONS; slot++) {
        const station_t *station = &w->stations[slot];
        if (!station_actor_slot_occupied(station)) continue;
        if (station->id > max_existing_id) max_existing_id = station->id;
    }
    if (max_existing_id == UINT32_MAX) return false;
    if (staged_next_id <= max_existing_id)
        staged_next_id = max_existing_id + 1;

    /* Resolve all permitted zero IDs/actors without mutating the world. */
    for (int slot = 0; slot < MAX_STATIONS; slot++) {
        const station_t *station = &w->stations[slot];
        if (!station_actor_slot_occupied(station)) continue;
        if (!station_actor_bytes_nonzero(
                station->station_pubkey,
                sizeof(station->station_pubkey))) {
            return false;
        }
        staged_ids[slot] = station->id;
        if (staged_ids[slot] == 0) {
            for (;;) {
                if (staged_next_id == 0) return false;
                bool used = false;
                for (int other = 0; other < MAX_STATIONS; other++) {
                    uint32_t existing = staged_ids[other]
                        ? staged_ids[other]
                        : (station_actor_slot_occupied(&w->stations[other])
                               ? w->stations[other].id
                               : 0);
                    if (existing == staged_next_id) {
                        used = true;
                        break;
                    }
                }
                if (!used) break;
                staged_next_id++;
            }
            staged_ids[slot] = staged_next_id++;
            if (staged_next_id == 0) return false;
        }
        for (int prior = 0; prior < slot; prior++) {
            if (station_actor_slot_occupied(&w->stations[prior]) &&
                staged_ids[prior] == staged_ids[slot]) {
                return false;
            }
        }
        if (legacy_saved_identity) {
            if (!station_actor_bytes_nonzero(
                    station->station_pubkey,
                    sizeof(station->station_pubkey))) {
                return false;
            }
            station_actor_id_derive_legacy(
                station, staged_actors[slot]);
            if (station->station_actor_catalog_attested &&
                (!station_actor_bytes_nonzero(
                     station->station_actor_id,
                     sizeof(station->station_actor_id)) ||
                 memcmp(station->station_actor_id,
                        staged_actors[slot],
                        ACTOR_PRINCIPAL_ID_SIZE) != 0)) {
                return false;
            }
        } else if (!station_actor_bytes_nonzero(
                       station->station_actor_id,
                       sizeof(station->station_actor_id))) {
            if (!station_actor_bytes_nonzero(
                    station->station_pubkey,
                    sizeof(station->station_pubkey))) {
                return false;
            }
            station_actor_id_derive(
                station, staged_ids[slot], staged_actors[slot]);
        } else {
            memcpy(staged_actors[slot], station->station_actor_id,
                   sizeof(staged_actors[slot]));
        }
        actor_principal_t principal = actor_principal_none();
        if (!actor_principal_from_stable_id(
                ACTOR_PRINCIPAL_STATION,
                staged_actors[slot], &principal)) {
            return false;
        }
        for (int prior = 0; prior < slot; prior++) {
            const station_t *other = &w->stations[prior];
            if (!station_actor_slot_occupied(other)) continue;
            if (memcmp(staged_actors[prior],
                       staged_actors[slot],
                       ACTOR_PRINCIPAL_ID_SIZE) == 0) {
                return false;
            }
        }
    }

    /* Commit only after every ID and actor has passed uniqueness checks. */
    for (int slot = 0; slot < MAX_STATIONS; slot++) {
        station_t *station = &w->stations[slot];
        if (!station_actor_slot_occupied(station)) {
            station->id = 0;
            memset(station->station_actor_id, 0,
                   sizeof(station->station_actor_id));
            continue;
        }
        station->id = staged_ids[slot];
        memcpy(station->station_actor_id, staged_actors[slot],
               sizeof(station->station_actor_id));
    }
    w->next_station_id = staged_next_id;
    return true;
}

bool world_ensure_station_actor_ids(world_t *w) {
    return world_fill_station_actor_ids(w, false);
}

bool world_migrate_legacy_station_actor_ids(world_t *w) {
    return world_fill_station_actor_ids(w, true);
}

bool world_validate_station_actor_ids(const world_t *w) {
    if (!w || w->next_station_id == 0) return false;
    uint32_t max_id = 0;
    for (int slot = 0; slot < MAX_STATIONS; slot++) {
        const station_t *station = &w->stations[slot];
        bool occupied = station_actor_slot_occupied(station);
        bool actor_nonzero = station_actor_bytes_nonzero(
            station->station_actor_id,
            sizeof(station->station_actor_id));
        if (!occupied) {
            if (station->id != 0 || actor_nonzero) return false;
            continue;
        }
        if (!station_actor_bytes_nonzero(
                station->station_pubkey,
                sizeof(station->station_pubkey))) {
            return false;
        }
        actor_principal_t principal = actor_principal_none();
        if (station->id == 0 || !actor_nonzero ||
            !actor_principal_from_stable_id(
                ACTOR_PRINCIPAL_STATION,
                station->station_actor_id, &principal)) {
            return false;
        }
        if (station->id > max_id) max_id = station->id;
        for (int prior = 0; prior < slot; prior++) {
            const station_t *other = &w->stations[prior];
            if (!station_actor_slot_occupied(other)) continue;
            if (other->id == station->id ||
                memcmp(other->station_actor_id,
                       station->station_actor_id,
                       ACTOR_PRINCIPAL_ID_SIZE) == 0) {
                return false;
            }
        }
    }
    return max_id < w->next_station_id;
}

bool actor_principal_from_station(
    const world_t *w,
    int station_slot,
    actor_principal_t *out) {
    if (out) *out = actor_principal_none();
    if (!w || !out ||
        station_slot < 0 || station_slot >= MAX_STATIONS ||
        !station_actor_slot_occupied(&w->stations[station_slot])) {
        return false;
    }
    return actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_STATION,
        w->stations[station_slot].station_actor_id, out);
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

actor_resolution_result_t world_resolve_npc_principal(
    const world_t *w,
    const actor_principal_t *principal) {
    actor_resolution_result_t unknown =
        actor_resolution(ACTOR_RESOLUTION_UNKNOWN, -1);
    if (!w || !principal ||
        principal->kind != ACTOR_PRINCIPAL_NPC ||
        !actor_principal_is_canonical(principal)) {
        return unknown;
    }

    int matched_slot = -1;
    for (int slot = 0; slot < MAX_NPC_SHIPS; slot++) {
        const npc_ship_t *npc = &w->npc_ships[slot];
        actor_principal_t candidate = actor_principal_none();
        if (!actor_principal_from_npc(npc, &candidate) ||
            !actor_principal_equal(&candidate, principal)) {
            continue;
        }
        if (matched_slot >= 0) return unknown;
        matched_slot = slot;
    }
    if (matched_slot < 0)
        return actor_resolution(ACTOR_RESOLUTION_OFFLINE, -1);
    return actor_resolution(
        ACTOR_RESOLUTION_ONLINE, (int16_t)matched_slot);
}

actor_resolution_result_t world_resolve_station_principal(
    const world_t *w,
    const actor_principal_t *principal) {
    actor_resolution_result_t unknown =
        actor_resolution(ACTOR_RESOLUTION_UNKNOWN, -1);
    if (!w || !principal ||
        principal->kind != ACTOR_PRINCIPAL_STATION ||
        !actor_principal_is_canonical(principal)) {
        return unknown;
    }

    int matched_slot = -1;
    for (int slot = 0; slot < MAX_STATIONS; slot++) {
        const station_t *station = &w->stations[slot];
        if (!station_actor_slot_occupied(station) ||
            memcmp(station->station_actor_id, principal->id,
                   ACTOR_PRINCIPAL_ID_SIZE) != 0) {
            continue;
        }
        if (matched_slot >= 0) return unknown;
        matched_slot = slot;
    }
    if (matched_slot < 0)
        return actor_resolution(ACTOR_RESOLUTION_OFFLINE, -1);
    return actor_resolution(
        ACTOR_RESOLUTION_ONLINE, (int16_t)matched_slot);
}

actor_resolution_result_t world_resolve_actor_principal(
    const world_t *w,
    const actor_principal_t *principal) {
    if (!principal || !actor_principal_is_canonical(principal)) {
        return actor_resolution(ACTOR_RESOLUTION_UNKNOWN, -1);
    }
    switch ((actor_principal_kind_t)principal->kind) {
        case ACTOR_PRINCIPAL_PLAYER:
            return world_resolve_player_principal(w, principal);
        case ACTOR_PRINCIPAL_NPC:
            return world_resolve_npc_principal(w, principal);
        case ACTOR_PRINCIPAL_STATION:
            return world_resolve_station_principal(w, principal);
        case ACTOR_PRINCIPAL_NONE:
        case ACTOR_PRINCIPAL_UNATTRIBUTED:
        case ACTOR_PRINCIPAL_SYSTEM:
        case ACTOR_PRINCIPAL_KIND_COUNT:
        default:
            return actor_resolution(ACTOR_RESOLUTION_UNKNOWN, -1);
    }
}
