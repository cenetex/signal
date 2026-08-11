/*
 * Authenticated one-time legacy-save recovery.
 *
 * This module owns only the connection-bound offer/challenge and redacted
 * audit vocabulary. Filesystem discovery, staged decoding, and persistence
 * generation publication live in sim_save.c / persistence_generation.c.
 */
#ifndef SIGNAL_LEGACY_SAVE_RECOVERY_H
#define SIGNAL_LEGACY_SAVE_RECOVERY_H

#include "game_sim.h"
#include "persistence_generation.h"
#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    LEGACY_RECOVERY_DEFAULT_TTL_MS = 30000,
    LEGACY_RECOVERY_MAX_TTL_MS = 300000,
};

typedef enum {
    LEGACY_RECOVERY_OFFER_NONE = 0,
    LEGACY_RECOVERY_OFFER_AVAILABLE,
    LEGACY_RECOVERY_OFFER_IN_FLIGHT,
} legacy_recovery_offer_phase_t;

/*
 * Runtime-only, one instance per transport. It must be cleared when the
 * transport is released; carrying it across reconnect would defeat the
 * connection binding.
 */
typedef struct {
    uint8_t phase;
    uint8_t offer_id[LEGACY_RECOVERY_OFFER_ID_SIZE];
    uint8_t binding_digest[32];
    uint64_t connection_generation;
    uint64_t expires_at_ms;
} legacy_recovery_offer_t;

typedef enum {
    LEGACY_RECOVERY_SOURCE_NO_MATCH = 0,
    LEGACY_RECOVERY_SOURCE_CANDIDATE,
    LEGACY_RECOVERY_SOURCE_INVALID,
    LEGACY_RECOVERY_SOURCE_DESTINATION_CONFLICT,
} legacy_recovery_source_status_t;

void legacy_recovery_offer_clear(legacy_recovery_offer_t *offer);
bool legacy_recovery_offer_expired(
    const legacy_recovery_offer_t *offer,
    uint64_t now_ms);
bool legacy_recovery_offer_blocks_persistence(
    const legacy_recovery_offer_t *offer,
    uint64_t now_ms);

/*
 * Issue a fresh opaque challenge after pubkey proof but before durable
 * identity finalization. Deferring finalization keeps registry, ledger, ship,
 * and ownership state out of the live world until recovery commits.
 * `connection_generation` is a non-zero server-local transport generation.
 * Entropy failure clears `offer` and fails closed.
 */
bool legacy_recovery_offer_issue(
    legacy_recovery_offer_t *offer,
    const world_t *world,
    int player_idx,
    uint64_t connection_generation,
    uint64_t now_ms,
    uint64_t ttl_ms);

/*
 * Validate and consume an offer confirmation. Success changes AVAILABLE to
 * IN_FLIGHT. All failures leave authoritative game state untouched; expired
 * or identity-mismatched offers are cleared.
 */
legacy_recovery_result_status_t legacy_recovery_offer_begin(
    legacy_recovery_offer_t *offer,
    const world_t *world,
    int player_idx,
    uint64_t connection_generation,
    uint64_t now_ms,
    const uint8_t *payload,
    uint16_t payload_len);

/* End an attempt. A consumed offer is never reusable, regardless of result. */
void legacy_recovery_offer_finish(legacy_recovery_offer_t *offer);

int legacy_recovery_serialize_offer(
    uint8_t out[NET_LEGACY_RECOVERY_OFFER_SIZE],
    const legacy_recovery_offer_t *offer,
    uint64_t now_ms);

int legacy_recovery_serialize_result(
    uint8_t out[NET_LEGACY_RECOVERY_RESULT_SIZE],
    legacy_recovery_result_status_t status);

const char *legacy_recovery_result_name(
    legacy_recovery_result_status_t status);

/*
 * Append one bearer-free audit result under `root_dir`. The record contains
 * only a format tag and stable status name: never a token, basename, path,
 * pubkey, reconnect secret, offer id, or signature.
 */
bool legacy_recovery_audit_append(
    const char *root_dir,
    legacy_recovery_result_status_t status);

/*
 * Probe only legacy/player_<lowerhex(authenticated token)>.sav in the
 * currently selected persistence generation (or the pre-generation player
 * directory). No directory enumeration result is exposed to the caller.
 */
legacy_recovery_source_status_t legacy_recovery_source_probe(
    const char *generation_root,
    const char *legacy_player_dir,
    const uint8_t session_token[8],
    const uint8_t pubkey[32]);

/*
 * Probe only pubkey/<base58(authenticated pubkey)>.sav in the selected
 * namespace. This lets bootstrap give an existing canonical pubkey save
 * priority without first mutating the live registry or legacy ledger.
 */
legacy_recovery_source_status_t legacy_recovery_destination_probe(
    const char *generation_root,
    const char *legacy_player_dir,
    const uint8_t pubkey[32]);

/*
 * Decode the canonical source through a no-follow, single-link, bounded,
 * CRC-checked snapshot into a detached world clone; preserve the maximum
 * signed-action nonce; rerun token-to-pubkey migration; publish a complete
 * recovery generation; then and only then swap the clone into `world`.
 */
legacy_recovery_result_status_t legacy_recovery_execute(
    world_t *world,
    int player_idx,
    uint64_t confirmed_nonce,
    const char *generation_root,
    const char *legacy_player_dir,
    const bool save_player_slot[MAX_PLAYERS],
    persistence_generation_fault_t fault,
    persistence_generation_paths_t *published);

#endif /* SIGNAL_LEGACY_SAVE_RECOVERY_H */
