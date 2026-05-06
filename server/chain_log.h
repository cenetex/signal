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
 *   - D: cross-station cargo receipts live in shared/cargo_receipt.h.
 *   - E: standalone `signal_verify` wraps the same verifier that runs
 *     at startup.
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

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "types.h"
#include "game_sim.h"  /* world_t (anonymous struct typedef) */

#ifdef __cplusplus
extern "C" {
#endif

/* Portable struct-packing primitives. MSVC rejects the GCC/Clang
 * `__attribute__((packed))` spelling; both compilers do accept the
 * `#pragma pack(push, 1)` / `pop` form. Use SIGNAL_PACK_PUSH /
 * SIGNAL_PACK_POP around the typedef, and tag the struct with
 * SIGNAL_PACKED for the GCC/Clang side. */
#if defined(_MSC_VER)
   /* MSVC C4200: zero-sized / flexible array members are C99 standard
    * but MSVC's strict mode flags them. We use them deliberately for
    * variable-length payloads (operator-post text). Suppress around
    * the pack region. */
#  define SIGNAL_PACK_PUSH __pragma(pack(push, 1)) __pragma(warning(push)) __pragma(warning(disable: 4200))
#  define SIGNAL_PACK_POP  __pragma(warning(pop)) __pragma(pack(pop))
#  define SIGNAL_PACKED
#else
#  define SIGNAL_PACK_PUSH
#  define SIGNAL_PACK_POP
#  define SIGNAL_PACKED __attribute__((packed))
#endif

typedef enum {
    CHAIN_EVT_NONE         = 0,
    CHAIN_EVT_SMELT        = 1,  /* fragment -> ingot at this station */
    CHAIN_EVT_CRAFT        = 2,  /* ingot(s) -> finished product */
    CHAIN_EVT_TRANSFER     = 3,  /* cargo unit moved between holders */
    CHAIN_EVT_TRADE        = 4,  /* transfer + ledger delta, atomic */
    CHAIN_EVT_LEDGER       = 5,  /* station-side credit balance mutation */
    CHAIN_EVT_ROCK_DESTROY = 6,  /* asteroid fractured to terminal state */
    CHAIN_EVT_OPERATOR_POST = 7, /* persona-authored text signed by station */
    /* Fragment-lifecycle events: in-flight ore movement that previously
     * left the chain log silent. Pairs with EVT_SMELT (which captures
     * the productive end of a fragment's life): TOW records who took
     * possession, RELEASE records when possession ended without a
     * smelt. Heritage queries that filter on tower identity ("frames
     * smelted from ferrite that 0F3H-CH towed") need both. */
    CHAIN_EVT_FRAGMENT_TOW     = 8,  /* player tractor grabs a fragment */
    CHAIN_EVT_FRAGMENT_RELEASE = 9,  /* tow ended without smelt */
    /* Player death: highscores are now a view of these events replayed
     * out of the chain log at server boot. Carries the run summary
     * (credits/ore/asteroids) plus victim+killer tokens for attribution. */
    CHAIN_EVT_DEATH            = 10,
    CHAIN_EVT_TYPE_COUNT
} chain_event_type_t;

/* On-disk payload schemas — one per event type. Field order, sizes, and
 * padding are wire-stable and verified by static_assert below. The
 * existing inline anonymous structs at the emit sites used the same
 * layout; these typedefs are the single source of truth so the byte
 * format can't drift across the seven historical callsites. */

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  fragment_pub[32];
    uint8_t  ingot_pub[32];
    uint8_t  prefix_class;
    uint8_t  _pad[7];
    uint64_t mined_block;
} SIGNAL_PACKED chain_payload_smelt_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint16_t recipe_id;
    uint8_t  input_count;
    uint8_t  _pad[5];
    uint8_t  output_pub[32];
    uint8_t  input_pubs[2][32];
} SIGNAL_PACKED chain_payload_craft_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t from_pubkey[32];
    uint8_t to_pubkey[32];
    uint8_t cargo_pub[32];
    uint8_t kind;
    uint8_t _pad[7];
} SIGNAL_PACKED chain_payload_transfer_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint64_t transfer_event_id;
    int64_t  ledger_delta_signed;
    uint8_t  ledger_pubkey[32];
} SIGNAL_PACKED chain_payload_trade_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t rock_pub[32];
    uint8_t fracturing_player_pub[32];
    uint8_t station_pubkey[32];
} SIGNAL_PACKED chain_payload_rock_destroy_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  kind;            /* 0=HAIL_MOTD, 1=CONTRACT_FLAVOR, 2=RARITY_TIER,
                               * 3=BUILD_INFO (text=8-hex-char build SHA, ref_id unused),
                               * 4=WORLD_INFO (text = belt_seed:u32 LE || world_seq:u32 LE
                               *               || build SHA hex; pre-v52 emits omit world_seq
                               *               and the parser defaults it to 0),
                               * reserved 5-255 */
    uint8_t  tier;            /* for kind=RARITY_TIER: 0=common,1=uncommon,2=rare,3=ultra */
    uint16_t ref_id;          /* contract id, motd seed, etc. — kind-specific */
    uint8_t  text_sha256[32]; /* SHA-256 of UTF-8 text bytes */
    uint16_t text_len;        /* 0..256 */
    uint8_t  text[];          /* UTF-8, exact length text_len, no NUL terminator */
} SIGNAL_PACKED chain_payload_operator_post_t;
SIGNAL_PACK_POP

/* Fragment-tow event: a player has taken possession of a fragment via
 * tractor. tower_player_pub is the tower's identity pubkey; for
 * unregistered (legacy) clients that's all-zero and the
 * tower_session_token holds the legacy 8-byte session ID instead. */
SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  fragment_pub[32];        /* the rock that's now under tow */
    uint8_t  tower_player_pub[32];    /* identity pubkey, or 0 for anonymous */
    uint8_t  tower_session_token[8];  /* legacy session ID (lower 8 bytes) */
    uint64_t epoch_tick;              /* sim tick when tow began */
} SIGNAL_PACKED chain_payload_fragment_tow_t;
SIGNAL_PACK_POP

/* Fragment-release event: tow ended without a smelt completing. The
 * fragment may or may not still exist in the world; this records the
 * tow's terminus from the chain log's perspective. Reasons capture the
 * three player-visible end states.
 *
 * DESTROYED is reserved but not currently emitted: when an asteroid
 * dies mid-tow, EVT_ROCK_DESTROY already fires from sim_asteroid.c,
 * and a verifier can cross-reference TOW + ROCK_DESTROY events to
 * detect "was this rock under tow at death" without a separate event.
 * Wiring DESTROYED in directly would require sim_asteroid.c to scan
 * player tow lists at destruction time. */
typedef enum {
    FRAGMENT_RELEASE_DESTROYED = 0,  /* reserved — see comment above */
    FRAGMENT_RELEASE_BAND_SNAP = 1,  /* fragment escaped past 1.5x tractor range */
    FRAGMENT_RELEASE_MANUAL    = 2,  /* player tapped R or threw the rock (PvP fling) */
} fragment_release_reason_t;

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  fragment_pub[32];        /* the rock whose tow just ended */
    uint8_t  tower_player_pub[32];    /* who was towing — same as TOW event */
    uint8_t  tower_session_token[8];  /* legacy session ID */
    uint64_t epoch_tick;              /* sim tick when release happened */
    uint8_t  reason;                   /* fragment_release_reason_t */
    uint8_t  _pad[7];                  /* MUST be zero */
} SIGNAL_PACKED chain_payload_fragment_release_t;
SIGNAL_PACK_POP

/* Death event: a single run ended. Replayed out of the chain log at
 * server boot to rebuild the in-memory highscore table — there is no
 * separate highscores.dat anymore. victim_pubkey is zeroed for legacy
 * (un-registered) clients; victim_session_token is always populated.
 * killer_token is the killer's session_token (zero if unattributed /
 * NPC / self). killed_by_callsign is resolved against the connected
 * players list at emit time — leaves the field zero for NPC kills,
 * disconnected players, or self-destructs. The replay walker reads
 * this field directly; the legacy victim-callsign-map fallback only
 * kicks in for events emitted before this field existed. */
SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  victim_pubkey[32];        /* 0 for legacy clients */
    uint8_t  victim_session_token[8];
    uint8_t  victim_callsign[8];       /* not NUL-terminated if 8 chars */
    uint8_t  killer_token[8];
    uint8_t  cause;                    /* death_cause_t */
    uint8_t  _pad[7];                  /* MUST be zero */
    uint64_t epoch_tick;
    float    credits_earned;
    float    credits_spent;
    float    ore_mined;
    uint32_t asteroids_fractured;
    uint8_t  killed_by_callsign[8];    /* resolved at emit; 0 if unattributed */
} SIGNAL_PACKED chain_payload_death_t;
SIGNAL_PACK_POP

/* Wire-format guards: any field-list change that shifts these sizes
 * forks the chain log byte format and must be paired with a
 * versioning story (or accepted as a hard break). */
_Static_assert(sizeof(chain_payload_smelt_t)            == 80,  "smelt payload size");
_Static_assert(sizeof(chain_payload_craft_t)            == 104, "craft payload size");
_Static_assert(sizeof(chain_payload_transfer_t)         == 104, "transfer payload size");
_Static_assert(sizeof(chain_payload_trade_t)            == 48,  "trade payload size");
_Static_assert(sizeof(chain_payload_rock_destroy_t)     == 96,  "rock_destroy payload size");
_Static_assert(sizeof(chain_payload_fragment_tow_t)     == 80,  "fragment_tow payload size");
_Static_assert(sizeof(chain_payload_fragment_release_t) == 88,  "fragment_release payload size");
_Static_assert(sizeof(chain_payload_death_t)            == 96,  "death payload size");
/* The fixed-prefix size (before the text[] variable-length array):
 * kind(1) + tier(1) + ref_id(2) + text_sha256(32) + text_len(2) = 38 bytes */
_Static_assert(offsetof(chain_payload_operator_post_t, text) == 38, "operator_post fixed-prefix size");

/* Fixed-size event header — exactly 184 bytes on disk. The serialized
 * form matches this struct's natural C11 layout (verified by static
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

/* Lifted post-mortem verifier (#479 Layer E).
 *
 * Reasons to add this alongside the live-sim chain_log_verify():
 *  - The standalone signal_verify CLI has no world_t. It needs to
 *    walk a log given just a path on disk + the station pubkey.
 *  - Operators / federated peers / on-chain anchor verifiers want a
 *    structured *report* (counts, per-type, first-failure) — not a
 *    bare bool.
 *
 * Behaviorally identical to chain_log_verify for the signature +
 * linkage + payload-hash + monotonic-event_id + authority-pubkey
 * checks. Operates on an open FILE* so tests can verify in-memory or
 * partial logs without going through the chain dir. The caller owns
 * the FILE* and is responsible for fclose. The file pointer is read
 * from its current offset to EOF.
 *
 * Returns true iff the log is fully valid AND all events parsed
 * cleanly. On failure, out_report->first_fail_reason describes the
 * first violating event (and first_fail_event_id is 1-based, or 0 if
 * the failure preceded the first valid event). */
typedef struct {
    uint64_t total_events;
    uint64_t valid_events;
    uint64_t bad_signatures;
    uint64_t bad_linkage;        /* prev_hash mismatch */
    uint64_t bad_payload_hash;   /* payload bytes don't match header */
    uint64_t bad_authority;      /* authority field != expected pubkey */
    uint64_t monotonic_violations;
    uint64_t event_type_counts[CHAIN_EVT_TYPE_COUNT];
    uint64_t first_fail_event_id;
    char     first_fail_reason[128];
} chain_log_verify_report_t;

bool chain_log_verify_with_pubkey(FILE *log,
                                  const uint8_t station_pubkey[32],
                                  chain_log_verify_report_t *out_report);

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
