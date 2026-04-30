/*
 * chain_log.h -- Per-station signed event chain log (Layer C of #479).
 *
 * Each station owns an append-only on-disk log of state-mutation
 * events that it has authored. Every event is signed by the station's
 * Ed25519 private key (Layer B / station_authority.h) and chained to
 * the previous event by SHA-256 hash. The log is durable, replayable,
 * and verifiable: given just the on-disk log + the station's public
 * key, an auditor can prove that no event was inserted, removed, or
 * altered after the fact.
 *
 * This is deliberately separate from the player-input signed-action
 * machinery in src/identity.c (#479 A.3) — that protects player
 * intent against replay; this protects the world's *recorded history*
 * against tampering by the server operator.
 *
 * Layer scope:
 *   - C (this file): emit + chain + persist + verify-walk.
 *   - D (later): cross-station gossip / merge.
 *   - E (later): standalone `signal_verify` tool wrapping the same
 *     verifier that runs at startup.
 *
 * On-disk layout: `chain/<base58(station_pubkey)>.log`. Each entry is
 * the 184-byte chain_event_header_t followed by uint16 payload_len
 * and payload_len bytes of payload. New entries are appended;
 * existing entries are never rewritten. The CHAIN_DIR can be
 * overridden via chain_log_set_dir() so tests don't trample each
 * other.
 */
#ifndef SERVER_CHAIN_LOG_H
#define SERVER_CHAIN_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "types.h"
#include "game_sim.h"  /* world_t (anonymous struct typedef) */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHAIN_EVT_NONE         = 0,
    CHAIN_EVT_SMELT        = 1,  /* fragment -> ingot at this station */
    CHAIN_EVT_CRAFT        = 2,  /* ingot(s) -> finished product */
    CHAIN_EVT_TRANSFER     = 3,  /* cargo unit moved between holders */
    CHAIN_EVT_TRADE        = 4,  /* transfer + ledger delta, atomic */
    CHAIN_EVT_LEDGER       = 5,  /* station-side credit balance mutation */
    CHAIN_EVT_ROCK_DESTROY = 6,  /* asteroid fractured to terminal state */
    CHAIN_EVT_TYPE_COUNT
} chain_event_type_t;

/* Fixed-size event header — exactly 184 bytes on disk. The serialized
 * form matches this struct's natural C99 layout (verified by static
 * assertion in chain_log.c). */
typedef struct {
    uint64_t epoch;            /* sim tick when authored */
    uint64_t event_id;         /* monotonic per (station, epoch) */
    uint8_t  type;             /* chain_event_type_t */
    uint8_t  _pad[7];          /* MUST be zero */
    uint8_t  authority[32];    /* signing pubkey (the station's) */
    uint8_t  payload_hash[32]; /* SHA-256 of the payload bytes */
    uint8_t  prev_hash[32];    /* hash of the previous event header */
    uint8_t  signature[64];    /* Ed25519 over the unsigned header */
} chain_event_header_t;

#define CHAIN_EVENT_HEADER_SIZE 184

/* Override the on-disk directory used for chain log files. NULL or
 * empty restores the default ("chain/"). The string is copied into a
 * static buffer; the caller may free their copy. */
void chain_log_set_dir(const char *dir);

/* Returns the currently configured chain directory (default "chain/"). */
const char *chain_log_get_dir(void);

/* Append a signed event to station s's chain log.
 *
 * Computes payload_hash (SHA-256 of the payload bytes), reads
 * prev_hash from s->chain_last_hash, signs the unsigned-header bytes
 * with the station's private key, writes (header || payload_len ||
 * payload) atomically (fsync + fclose) to the per-station log file,
 * and updates s->chain_last_hash + s->chain_event_count.
 *
 * payload may be NULL iff payload_len == 0.
 *
 * Returns the new event_id (>= 1), or 0 on failure. Failures are
 * logged via SIM_LOG and leave the station's in-memory state
 * untouched. */
uint64_t chain_log_emit(world_t *w, station_t *s, chain_event_type_t type,
                        const void *payload, uint16_t payload_len);

/* Walk the on-disk chain log for station s. Returns true iff every
 * event verifies: signature against authority pubkey (must equal
 * s->station_pubkey), prev_hash linkage to the previous entry, and
 * payload_hash matches the stored payload bytes.
 *
 * If out_event_count is non-NULL, the number of events successfully
 * walked is written through (regardless of success). If out_last_hash
 * is non-NULL, the SHA-256 of the last successfully-walked header is
 * written through.
 *
 * On a missing log file, returns true with zero events walked — an
 * empty chain is trivially valid. */
bool chain_log_verify(const station_t *s,
                      uint64_t *out_event_count,
                      uint8_t out_last_hash[32]);

/* Compute the SHA-256 of a chain_event_header_t (all 184 bytes,
 * including the signature — this is the full record hash that gets
 * fed into the *next* event's prev_hash). */
void chain_event_header_hash(const chain_event_header_t *h, uint8_t out[32]);

/* Build the path "<dir>/<base58(pubkey)>.log" into out (size at least
 * 256). Returns true on success. */
bool chain_log_path_for(const uint8_t pubkey[32], char *out, size_t cap);

/* Remove the on-disk chain log file for station s, if any. Used by
 * world_reset() so a fresh sim starts with empty per-station logs.
 * Safe to call even if the file does not exist. */
void chain_log_reset(const station_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CHAIN_LOG_H */
