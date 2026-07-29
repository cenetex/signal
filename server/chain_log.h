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
 * machinery in client/identity.c (#479 A.3) — that protects player
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
#include "cargo_craft_provenance.h"
#include "cargo_smelt_provenance.h"

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
    CHAIN_EVT_CRAFT        = 2,  /* cargo input(s) -> finished product */
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
     * out of the chain log at server boot. Carries the run summary plus a
     * verified victim pubkey and presentation labels. Token-shaped fields in
     * the legacy fixed layout are zero in all newly emitted records. */
    CHAIN_EVT_DEATH            = 10,
    /* Construction contribution: a named manifest unit was consumed into
     * infrastructure. This is the provenance bridge from "cargo existed"
     * to "cargo became signal/station/gate capacity." */
    CHAIN_EVT_CONSTRUCTION     = 11,
    /* Route-history summary: a station observed enough distinct
     * receipt-backed gossip for a route to summarize it as local durable
     * history. This records reputation only; payouts and cargo movement still
     * resolve through exact contracts, ledgers, manifests, and receipts. */
    CHAIN_EVT_ROUTE_HISTORY    = 12,
    /* Fracture-claim observation: persists enough claim-local inputs to
     * recompute fragment_pub and grade math. It does not bind those inputs
     * to canonical asteroid/material evidence and is not mining proof. */
    CHAIN_EVT_CLAIM_FRAGMENT   = 13,
    CHAIN_EVT_TYPE_COUNT
} chain_event_type_t;

/* On-disk payload schemas — one per event type. Field order and sizes are
 * wire-stable and verified by static_assert below. Cargo transform payloads
 * deliberately version the bytes that used to be anonymous padding: version
 * zero is a legacy, semantically-unbound event and must never establish cargo
 * trust; version one binds every non-derivable output trait without changing
 * either historical payload size.
 *
 * The
 * existing inline anonymous structs at the emit sites used the same
 * layout; these typedefs are the single source of truth so the byte
 * format can't drift across the seven historical callsites. */

typedef enum {
    CHAIN_CARGO_SEMANTICS_UNBOUND =
        CARGO_RECEIPT_ORIGIN_SEMANTICS_UNBOUND,
    CHAIN_CARGO_SEMANTICS_V1 =
        CARGO_RECEIPT_ORIGIN_SEMANTICS_V1,
} chain_cargo_semantics_version_t;

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  fragment_pub[32];
    uint8_t  ingot_pub[32];
    uint8_t  prefix_class;
    uint8_t  semantics_version;
    uint8_t  commodity;
    uint8_t  grade;
    uint16_t output_index;
    uint8_t  _reserved[2];
    uint64_t mined_block;
} SIGNAL_PACKED chain_payload_smelt_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint16_t recipe_id;
    uint8_t  input_count;
    uint8_t  semantics_version;
    uint8_t  output_kind;
    uint8_t  output_commodity;
    uint8_t  output_grade;
    uint8_t  output_quantity;
    uint8_t  output_pub[32];
    uint8_t  input_pubs[RECIPE_INPUT_MAX][32];
} SIGNAL_PACKED chain_payload_craft_t;
SIGNAL_PACK_POP

/*
 * Populate the versioned semantic bytes together with the historical
 * identity fields. These helpers reject internally inconsistent cargo so an
 * emitter cannot accidentally sign a partial or relabelled transform.
 */
bool chain_payload_smelt_bind_output(
    chain_payload_smelt_t *payload,
    const uint8_t fragment_pub[32],
    uint16_t output_index,
    const cargo_unit_t *output);
bool chain_payload_craft_bind_output(
    chain_payload_craft_t *payload,
    const cargo_unit_t *inputs,
    size_t input_count,
    const cargo_unit_t *output);

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
                               * 5=MINER_CHATTER, 6=HAULER_CHATTER, 7=RATI_DELIVERY,
                               * reserved 8-255 */
    uint8_t  tier;            /* for kind=RARITY_TIER: 0=common,1=uncommon,2=rare,3=ultra */
    uint16_t ref_id;          /* contract id, motd seed, etc. — kind-specific */
    uint8_t  text_sha256[32]; /* SHA-256 of UTF-8 text bytes */
    uint16_t text_len;        /* 0..256 */
    uint8_t  text[];          /* UTF-8, exact length text_len, no NUL terminator */
} SIGNAL_PACKED chain_payload_operator_post_t;
SIGNAL_PACK_POP

typedef enum {
    OPERATOR_POST_HAIL_MOTD      = 0,
    OPERATOR_POST_CONTRACT_FLAVOR= 1,
    OPERATOR_POST_RARITY_TIER    = 2,
    OPERATOR_POST_BUILD_INFO     = 3,
    OPERATOR_POST_WORLD_INFO     = 4,
    OPERATOR_POST_MINER_CHATTER  = 5,
    OPERATOR_POST_HAULER_CHATTER = 6,
    OPERATOR_POST_RATI_DELIVERY  = 7,
} operator_post_kind_t;

/* Fragment-tow event: a player has taken possession of a fragment via
 * tractor. tower_player_pub is the tower's verified identity pubkey.
 * tower_session_token is a retired legacy field: new writers MUST leave it
 * zero because session IDs are reconnect bearer credentials. Historical
 * non-zero bytes decode only as legacy/unattributed evidence. */
SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  fragment_pub[32];        /* the rock that's now under tow */
    uint8_t  tower_player_pub[32];    /* identity pubkey, or 0 for anonymous */
    uint8_t  tower_session_token[8];  /* RETIRED: new writers emit zero */
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
    uint8_t  tower_session_token[8];  /* RETIRED: new writers emit zero */
    uint64_t epoch_tick;              /* sim tick when release happened */
    uint8_t  reason;                   /* fragment_release_reason_t */
    uint8_t  _pad[7];                  /* MUST be zero */
} SIGNAL_PACKED chain_payload_fragment_release_t;
SIGNAL_PACK_POP

/* Death event: a single run ended. Replayed out of the chain log at server
 * boot to rebuild the in-memory highscore table. victim_pubkey is zero for
 * unverified/legacy clients. victim_session_token and killer_token are
 * retired legacy fields: new writers MUST leave both zero because session
 * IDs are reconnect bearer credentials. killed_by_callsign is resolved
 * transiently at emit time for compatibility; it is presentation, not
 * authority. Historical token fields remain readable only by the legacy
 * replay fallback and must never be re-emitted as actor identity. */
SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  victim_pubkey[32];        /* 0 for legacy clients */
    uint8_t  victim_session_token[8]; /* RETIRED: new writers emit zero */
    uint8_t  victim_callsign[8];       /* not NUL-terminated if 8 chars */
    uint8_t  killer_token[8];         /* RETIRED: new writers emit zero */
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

typedef enum {
    CONSTRUCTION_TARGET_STATION = 1,
    CONSTRUCTION_TARGET_MODULE  = 2,
    CONSTRUCTION_TARGET_GATE    = 3,
} construction_target_kind_t;

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  cargo_pub[32];       /* manifest unit consumed */
    uint8_t  target_kind;         /* construction_target_kind_t */
    uint8_t  station_index;       /* local station/outpost slot */
    uint8_t  module_index;        /* station_module_t index, 0xff if N/A */
    uint8_t  module_type;         /* module_type_t, 0xff if N/A */
    uint8_t  commodity;           /* commodity_t */
    uint8_t  _pad[3];             /* MUST be zero */
    uint64_t target_id;           /* reserved for gate/project ids */
    float    contributed_units;   /* normally 1.0 for manifest units */
    float    progress_after;      /* module/station supply fraction after consume */
} SIGNAL_PACKED chain_payload_construction_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  memory_kind;         /* market_memory_kind_t; reputation/risk */
    uint8_t  origin_station;
    uint8_t  destination_station;
    uint8_t  commodity;           /* commodity_t */
    uint8_t  action;              /* contract_action_t */
    uint8_t  confidence;
    uint8_t  salience;
    uint8_t  _pad;                /* MUST be zero */
    uint16_t evidence_count;      /* distinct receipt-backed units heard */
    uint16_t value_hint;          /* compact value carried by reputation */
    uint32_t observed_tick;
    uint64_t subject_nonce;       /* route/memory subject key */
} SIGNAL_PACKED chain_payload_route_history_t;
SIGNAL_PACK_POP

SIGNAL_PACK_PUSH
typedef struct {
    uint8_t  fracture_seed[32];
    uint8_t  fragment_pub[32];
    uint8_t  claimant_pubkey[32]; /* zero for unclaimed fallback resolution */
    uint32_t fracture_id;
    uint32_t burst_nonce;
    uint16_t burst_cap;
    uint8_t  grade;               /* mining_grade_t */
    uint8_t  asteroid_slot;        /* diagnostic slot at resolution time */
} SIGNAL_PACKED chain_payload_claim_fragment_t;
SIGNAL_PACK_POP

/* Wire-format guards: any field-list change that shifts these sizes
 * forks the chain log byte format and must be paired with a
 * versioning story (or accepted as a hard break). */
_Static_assert(sizeof(chain_payload_smelt_t)            == 80,  "smelt payload size");
_Static_assert(sizeof(chain_payload_craft_t)            == 136, "craft payload size");
_Static_assert(offsetof(chain_payload_smelt_t, semantics_version) == 65,
               "smelt semantics_version must occupy legacy padding");
_Static_assert(offsetof(chain_payload_smelt_t, output_index) == 68,
               "smelt output_index must occupy legacy padding");
_Static_assert(offsetof(chain_payload_smelt_t, mined_block) == 72,
               "smelt mined_block compatibility offset");
_Static_assert(offsetof(chain_payload_craft_t, semantics_version) == 3,
               "craft semantics_version must occupy legacy padding");
_Static_assert(offsetof(chain_payload_craft_t, output_pub) == 8,
               "craft output_pub compatibility offset");
_Static_assert(offsetof(chain_payload_craft_t, input_pubs) == 40,
               "craft input_pubs compatibility offset");
_Static_assert(sizeof(chain_payload_transfer_t)         == 104, "transfer payload size");
_Static_assert(sizeof(chain_payload_trade_t)            == 48,  "trade payload size");
_Static_assert(sizeof(chain_payload_rock_destroy_t)     == 96,  "rock_destroy payload size");
_Static_assert(sizeof(chain_payload_fragment_tow_t)     == 80,  "fragment_tow payload size");
_Static_assert(sizeof(chain_payload_fragment_release_t) == 88,  "fragment_release payload size");
_Static_assert(sizeof(chain_payload_death_t)            == 96,  "death payload size");
_Static_assert(sizeof(chain_payload_construction_t)     == 56,  "construction payload size");
_Static_assert(sizeof(chain_payload_route_history_t)    == 24,  "route_history payload size");
_Static_assert(sizeof(chain_payload_claim_fragment_t)   == 108, "claim_fragment payload size");
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

/*
 * One gameplay transaction may require several same-station events (for
 * example TRANSFER+TRADE, or one SMELT/CRAFT event per output unit). Keep the
 * batch bounded so staging has a predictable memory ceiling while still
 * covering the largest current production batch.
 */
#define CHAIN_LOG_BATCH_MAX_EVENTS 128

typedef struct {
    chain_event_type_t type;
    const void *payload;
    uint16_t payload_len;
} chain_log_batch_event_t;

typedef enum {
    CHAIN_LOG_APPEND_OK = 0,
    CHAIN_LOG_APPEND_BAD_ARGUMENTS,
    CHAIN_LOG_APPEND_BATCH_TOO_LARGE,
    CHAIN_LOG_APPEND_EVENT_ID_OVERFLOW,
    CHAIN_LOG_APPEND_UNKEYED,
    CHAIN_LOG_APPEND_BLOCKED,
    CHAIN_LOG_APPEND_SIGNING_FAILED,
    CHAIN_LOG_APPEND_NO_MEMORY,
    CHAIN_LOG_APPEND_PATH_FAILED,
    CHAIN_LOG_APPEND_OPEN_FAILED,
    CHAIN_LOG_APPEND_SEEK_FAILED,
    CHAIN_LOG_APPEND_TELL_FAILED,
    CHAIN_LOG_APPEND_WRITE_FAILED,
    CHAIN_LOG_APPEND_FLUSH_FAILED,
    CHAIN_LOG_APPEND_CLOSE_FAILED,
    CHAIN_LOG_APPEND_DIR_SYNC_FAILED,
    CHAIN_LOG_APPEND_ROLLBACK_FAILED,
} chain_log_append_status_t;

typedef struct {
    chain_log_append_status_t status;
    uint16_t event_count;
    uint64_t first_event_id;
    uint64_t last_event_id;
    uint8_t last_hash[32];
} chain_log_append_result_t;

/*
 * Append a bounded same-station event batch as one durability transaction.
 *
 * Every header, event id, signature, linkage hash, and serialized byte is
 * derived into private staging memory first. The station and world remain
 * untouched during staging. With disk logging enabled, the staged bytes are
 * written in one append sequence and receive exactly one durability flush.
 * The station counter/hash are committed only after that flush and close
 * succeed. The first append durably syncs the log entry in the chain
 * directory; if that directory was created for this append, its entry is
 * also synced in the directory's parent. Existing-directory appends do not
 * pay that parent-sync cost. A partial write or failed flush/close/directory
 * sync truncates back to the original file offset (or removes a newly-created
 * log and directory) and durably syncs that rollback before returning.
 *
 * On success, first_event_id..last_event_id are the contiguous ids assigned to
 * this batch and last_hash is the committed continuation hash. On failure,
 * those fields remain zero and the station continuation pointer is unchanged.
 */
chain_log_append_result_t chain_log_emit_batch(
    world_t *w,
    station_t *s,
    const chain_log_batch_event_t *events,
    size_t event_count);

const char *chain_log_append_status_name(chain_log_append_status_t status);

#if defined(SIGNAL_CHAIN_LOG_TESTING)
/* Deterministic test-only I/O fault injection. `event_type` selects matching
 * staged events (CHAIN_EVT_NONE matches any type); `occurrence` is one-based.
 * WRITE fails immediately before that matching event, so occurrences after
 * the first exercise rollback of a deterministic partial append. FLUSH,
 * SEEK, TELL, and CLOSE fail the batch containing the selected occurrence at
 * their named file step. DIR_SYNC targets a newly-created log entry;
 * PARENT_DIR_SYNC targets the first-boot chain-directory entry. A configured
 * fault fires once and then clears itself. */
typedef enum {
    CHAIN_LOG_TEST_FAULT_NONE = 0,
    CHAIN_LOG_TEST_FAULT_WRITE,
    CHAIN_LOG_TEST_FAULT_FLUSH,
    CHAIN_LOG_TEST_FAULT_CLOSE,
    CHAIN_LOG_TEST_FAULT_SEEK,
    CHAIN_LOG_TEST_FAULT_TELL,
    CHAIN_LOG_TEST_FAULT_DIR_SYNC,
    CHAIN_LOG_TEST_FAULT_PARENT_DIR_SYNC,
} chain_log_test_fault_point_t;

void chain_log_test_fault_inject(chain_log_test_fault_point_t point,
                                 chain_event_type_t event_type,
                                 uint32_t occurrence);
void chain_log_test_fault_clear(void);
#endif

/* Override the on-disk directory used for chain log files. NULL or
 * empty restores the default ("chain/"). The string is copied into a
 * static buffer; the caller may free their copy. */
void chain_log_set_dir(const char *dir);

/* Returns the currently configured chain directory (default "chain/"). */
const char *chain_log_get_dir(void);

/* Monotonic process-local epoch for read caches. It changes whenever the
 * configured chain directory or disk mode changes, or a log is reset. */
uint64_t chain_log_configuration_generation(void);

/* Controls whether chain_log_emit writes append records to disk. When
 * disabled, emits still sign and advance the in-memory station chain so
 * same-session receipt flows continue to work, but no local files are
 * created. */
void chain_log_set_disk_enabled(bool enabled);

/* True only while local durable history is enabled. Origin-proof resolvers
 * must fail closed while this is false even if an older log file exists. */
bool chain_log_disk_enabled(void);

/* Append one signed event to station s's chain log.
 *
 * This is the compatibility wrapper over chain_log_emit_batch(). It inherits
 * the same staging, durable rollback, and in-memory commit guarantees.
 *
 * payload may be NULL iff payload_len == 0.
 *
 * Returns the new event_id (>= 1), or 0 on failure. Failures are
 * logged via SIM_LOG and leave the station's in-memory state
 * untouched. Returns 0 if startup verification has marked the station's
 * chain unsafe to append. */
uint64_t chain_log_emit(world_t *w, station_t *s, chain_event_type_t type,
                        const void *payload, uint16_t payload_len);

const char *chain_log_health_status_name(chain_health_status_t status);
const char *chain_log_health_repair_hint(chain_health_status_t status,
                                         bool append_blocked);
void chain_log_health_set(station_t *s, chain_health_status_t status,
                          bool append_blocked,
                          uint64_t verified_event_count,
                          const uint8_t verified_last_hash[32],
                          const char *message);

/* Walk the on-disk chain log for station s. Returns true iff every
 * event verifies: signature against authority pubkey (must equal
 * s->station_pubkey), prev_hash linkage to the previous entry, and
 * payload_hash matches the stored payload bytes.
 *
 * If out_event_count is non-NULL, the last valid event_id in the final
 * segment is written through (regardless of success). On single-segment
 * logs this is the number of events successfully walked. If
 * out_last_hash is non-NULL, the SHA-256 of the last successfully-
 * walked header is written through.
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
 * checks. A later event with event_id==1 and prev_hash==0 is treated
 * as a fresh segment boundary: each segment is verified strictly on
 * its own chain, while the report keeps total and tail counters
 * separate. Operates on an open FILE* so tests can verify in-memory or
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
    uint64_t segment_count;
    uint64_t segment_resets;      /* accepted clean event_id=1/prev_hash=0 restarts */
    uint64_t tail_event_id;       /* last valid event_id in the final segment */
    uint64_t tail_valid_events;   /* valid events in the final segment */
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

bool chain_log_verify_station(const station_t *s,
                              uint64_t *out_event_count,
                              uint8_t out_last_hash[32],
                              chain_log_verify_report_t *out_report);

/*
 * Verify one chain-log identity directly by public key. This keeps preserved
 * historical identities discoverable after a station rekey. Missing files
 * retain the same trivially-empty semantics as chain_log_verify_station;
 * callers that need to distinguish absence must check the path first.
 */
bool chain_log_verify_identity(
    const uint8_t station_pubkey[32],
    uint64_t *out_event_count,
    uint8_t out_last_hash[32],
    chain_log_verify_report_t *out_report);

typedef struct {
    uint64_t event_id;
    uint64_t epoch;
    chain_payload_route_history_t payload;
} chain_route_history_tail_t;

/* Read model for one cargo-producing event. A player-facing lineage view can
 * follow a cargo pubkey back through CRAFT outputs and SMELT outputs without
 * treating the chain log as inventory authority. header_hash is the exact
 * signed record hash used for receipt origin pins; authority is copied from
 * the event header. The payload matching `type` is populated and the other
 * payload stays zeroed. */
typedef struct {
    uint8_t type; /* CHAIN_EVT_SMELT or CHAIN_EVT_CRAFT */
    uint64_t event_id;
    uint64_t epoch;
    uint8_t header_hash[32];
    uint8_t authority[32];
    /*
     * Canonical semantic view reconstructed from the signed payload.
     * origin_station stays zero here because authority -> station-index
     * resolution is local policy, not an on-chain numeric identity.
     */
    uint8_t output_semantics_version;
    cargo_unit_t output_cargo;
    /*
     * Populated for the matching transform after the containing chain event
     * has been verified. V1 can therefore be station-attested while its
     * stronger lineage/proof flags remain false.
     */
    cargo_smelt_provenance_result_t smelt_provenance;
    cargo_craft_provenance_result_t craft_provenance;
    chain_payload_smelt_t smelt;
    chain_payload_craft_t craft;
} chain_cargo_transform_t;

typedef enum {
    CHAIN_CARGO_TRANSFORM_NOT_FOUND = 0,
    CHAIN_CARGO_TRANSFORM_FOUND,
    CHAIN_CARGO_TRANSFORM_AMBIGUOUS,
    CHAIN_CARGO_TRANSFORM_READ_INVALID,
} chain_cargo_transform_find_status_t;

typedef bool (*chain_cargo_transform_visitor_t)(
    const chain_cargo_transform_t *transform,
    void *user);

typedef bool (*chain_cargo_transfer_visitor_t)(
    const chain_payload_transfer_t *transfer,
    void *user);

enum {
    CHAIN_LOG_EVIDENCE_SNAPSHOT_MAX_BYTES =
        64 * 1024 * 1024,
};

/*
 * Copy one already-open path-backed log into a bounded anonymous file.
 * Verification and interpretation must both use the returned snapshot to
 * exclude pathname replacement and in-place mutation between passes.
 * `source` remains caller-owned; `*out_snapshot` is NULL on failure and
 * caller-owned on success.
 */
bool chain_log_snapshot_evidence_file(
    FILE *source,
    FILE **out_snapshot);

/*
 * Visit exactly `verified_event_count` events from an already-open log.
 * The caller must first verify the same FILE* with
 * chain_log_verify_with_pubkey(). Keeping both passes on one open file
 * prevents an atomic path replacement from making the indexer consume a
 * different file than the verifier. The stream is rewound before reading
 * and remains owned by the caller.
 */
bool chain_log_visit_cargo_transforms_from_verified_file(
    FILE *log,
    uint64_t verified_event_count,
    chain_cargo_transform_visitor_t visitor,
    void *user,
    size_t *out_transform_count,
    uint8_t out_last_hash[32]);

/*
 * Build origin and prior-transfer evidence from the same already-verified
 * descriptor. Either visitor may be NULL, but at least one is required.
 * `verified_event_count` is the exact bound returned by
 * chain_log_verify_with_pubkey(); trailing or truncated records fail closed.
 */
bool chain_log_visit_cargo_evidence_from_verified_file(
    FILE *log,
    uint64_t verified_event_count,
    chain_cargo_transform_visitor_t transform_visitor,
    void *transform_user,
    chain_cargo_transfer_visitor_t transfer_visitor,
    void *transfer_user,
    size_t *out_transform_count,
    size_t *out_transfer_count,
    uint8_t out_last_hash[32]);

/* Sequentially visit every SMELT/CRAFT output in one identity log. Call only
 * after verification when the records are used as trust evidence. */
bool chain_log_visit_cargo_transforms_for_identity(
    const uint8_t station_pubkey[32],
    chain_cargo_transform_visitor_t visitor,
    void *user,
    size_t *out_transform_count);

/* Read the most recent route-history summaries from a station chain. This is
 * a read model only: it verifies neither payouts nor inventory, and callers
 * must still use chain_log_verify/chain_log_verify_with_pubkey when they need
 * proof that the whole log is valid. Returns the number of rows copied. */
int chain_log_read_route_history_tail(const station_t *s,
                                      chain_route_history_tail_t *out,
                                      int cap);

/* Find the most recent local SMELT/CRAFT event whose output is `cargo_pub`.
 * This intentionally searches one station log at a time: callers decide
 * which station histories are locally available and never infer a global
 * omniscient chain from a missing row. */
bool chain_log_find_cargo_transform(const station_t *s,
                                    const uint8_t cargo_pub[32],
                                    chain_cargo_transform_t *out);

/* Public-key form used to inspect a preserved historical chain identity. */
bool chain_log_find_cargo_transform_for_identity(
    const uint8_t station_pubkey[32],
    const uint8_t cargo_pub[32],
    chain_cargo_transform_t *out);

/*
 * Exact transform lookup with optional origin pin. Without a pin, two or more
 * valid SMELT/CRAFT events naming the same output are ambiguous and rejected.
 * With a non-zero event_hash_pin, only the event whose signed header hash
 * matches the pin is eligible; this is how a first receipt selects its exact
 * origin when duplicate output identities exist.
 */
chain_cargo_transform_find_status_t
chain_log_find_cargo_transform_for_identity_pinned(
    const uint8_t station_pubkey[32],
    const uint8_t cargo_pub[32],
    const uint8_t event_hash_pin[32],
    chain_cargo_transform_t *out);

/* Compute the SHA-256 of a chain_event_header_t (all 184 bytes,
 * including the signature — this is the full record hash that gets
 * fed into the *next* event's prev_hash). */
void chain_event_header_hash(const chain_event_header_t *h, uint8_t out[32]);

/* Build the path "<dir>/<base58(pubkey)>.log" into out (size at least
 * 256). Returns true on success. */
bool chain_log_path_for(const uint8_t pubkey[32], char *out, size_t cap);

/* Remove the on-disk chain log file for station s, if any. Use only for
 * explicit test/fresh-world tooling after the intended seed is known.
 * Safe to call even if the file does not exist. */
void chain_log_reset(const station_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CHAIN_LOG_H */
