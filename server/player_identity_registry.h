#ifndef PLAYER_IDENTITY_REGISTRY_H
#define PLAYER_IDENTITY_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Standalone storage substrate for the pubkey/actor compatibility registry.
 *
 * This module deliberately does not decide whether historical evidence is
 * canonical. Callers must classify imported rows before inserting them here.
 * Canonical actor IDs are sensitive compatibility bearers; this layer does
 * not authenticate them and its ordinary metrics/visitor surfaces never
 * expose them. Both pubkeys and actor IDs are globally unique across the
 * active and archive tiers. Ambiguous legacy evidence belongs in quarantine.
 */
enum {
    PLAYER_IDENTITY_PUBKEY_SIZE = 32,
    PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE = 8,
    PLAYER_IDENTITY_REGISTRY_EXPORT_VERSION = 1,
    PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE = 96,
    PLAYER_IDENTITY_CANONICAL_WIRE_SIZE = 40,
    PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE = 64,
    PLAYER_IDENTITY_REGISTRY_EXPORT_FLAG_SENSITIVE_BEARERS = 1u << 0,
};

typedef enum {
    PLAYER_IDENTITY_TIER_NONE = 0,
    PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE = 1,
    PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE = 2,
    PLAYER_IDENTITY_TIER_QUARANTINE = 3,
} player_identity_registry_tier_t;

/*
 * Persistent tags. New reasons must be appended; existing numeric values must
 * never be repurposed.
 */
typedef enum {
    PLAYER_IDENTITY_QUARANTINE_REASON_NONE = 0,
    PLAYER_IDENTITY_QUARANTINE_REASON_LEGACY_NONCANONICAL = 1,
    PLAYER_IDENTITY_QUARANTINE_REASON_DUPLICATE_PUBKEY = 2,
    PLAYER_IDENTITY_QUARANTINE_REASON_CONFLICTING_MAPPING = 3,
    PLAYER_IDENTITY_QUARANTINE_REASON_INVALID_PUBKEY = 4,
    PLAYER_IDENTITY_QUARANTINE_REASON_INVALID_ACTOR = 5,
    PLAYER_IDENTITY_QUARANTINE_REASON_UNPROVEN_MAPPING = 6,
    PLAYER_IDENTITY_QUARANTINE_REASON_OPERATOR_HOLD = 7,
    PLAYER_IDENTITY_QUARANTINE_REASON_COUNT = 8,
} player_identity_quarantine_reason_t;

typedef enum {
    PLAYER_IDENTITY_REGISTRY_OK = 0,
    PLAYER_IDENTITY_REGISTRY_OK_EXISTING,
    PLAYER_IDENTITY_REGISTRY_OK_INSERTED,
    PLAYER_IDENTITY_REGISTRY_OK_UPDATED,
    PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED,
    PLAYER_IDENTITY_REGISTRY_OK_RESTORED,
    PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED,
    PLAYER_IDENTITY_REGISTRY_OK_CLONED,
    PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT,
    PLAYER_IDENTITY_REGISTRY_INVALID_STATE,
    PLAYER_IDENTITY_REGISTRY_NOT_FOUND,
    PLAYER_IDENTITY_REGISTRY_CONFLICT,
    PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED,
    PLAYER_IDENTITY_REGISTRY_ARCHIVE_BUDGET_EXHAUSTED,
    PLAYER_IDENTITY_REGISTRY_QUARANTINE_BUDGET_EXHAUSTED,
    PLAYER_IDENTITY_REGISTRY_QUARANTINE_ID_EXHAUSTED,
    PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW,
    PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY,
    PLAYER_IDENTITY_REGISTRY_BUFFER_TOO_SMALL,
    PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED,
    PLAYER_IDENTITY_REGISTRY_STATUS_COUNT,
} player_identity_registry_status_t;

typedef struct {
    uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE];
    /*
     * Sensitive legacy compatibility actor. In the current format this is a
     * reconnect bearer/session token, not a public stable identity. It must
     * never enter health output, ordinary operator diagnostics, logs, or the
     * public/redacted visitor.
     */
    uint8_t sensitive_actor_id[
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE];
} player_identity_canonical_entry_t;

/*
 * Quarantine preserves exact legacy evidence for later repair. pubkey and the
 * sensitive actor bytes may be zero or malformed. record_id is the stable
 * operator identity. The source version and ordinal identify the exact legacy
 * row that produced it. The ordinary redacted visitor exposes neither the raw
 * actor nor a hash that could become an offline bearer-guessing oracle.
 */
typedef struct {
    uint64_t record_id;
    uint32_t source_version;
    uint64_t source_ordinal;
    uint16_t reason;
    uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE];
    uint8_t sensitive_actor_id[
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE];
} player_identity_quarantine_entry_t;

typedef struct {
    size_t canonical_active_limit;
    size_t canonical_archive_limit;
    size_t quarantine_limit;
} player_identity_registry_config_t;

typedef struct {
    size_t count;
    size_t capacity;
    size_t hard_limit;
    size_t hard_bytes;
    size_t high_water;
    uint64_t failure_count;
    uint64_t budget_failure_count;
    uint64_t allocation_failure_count;
} player_identity_registry_tier_metrics_t;

typedef struct {
    player_identity_registry_tier_metrics_t canonical_active;
    player_identity_registry_tier_metrics_t canonical_archive;
    player_identity_registry_tier_metrics_t quarantine;
    uint64_t quarantine_record_id_high_water;
    uint64_t failure_status_counts[
        PLAYER_IDENTITY_REGISTRY_STATUS_COUNT];
} player_identity_registry_metrics_t;

/*
 * The stores are exposed so the registry can later be embedded without an
 * additional allocation. Treat every member as private and use the API below.
 */
typedef struct {
    player_identity_canonical_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t hard_limit;
    size_t high_water;
    uint64_t failure_count;
    uint64_t budget_failure_count;
    uint64_t allocation_failure_count;
} player_identity_canonical_store_t;

typedef struct {
    player_identity_quarantine_entry_t *entries;
    size_t count;
    size_t capacity;
    size_t hard_limit;
    size_t high_water;
    uint64_t record_id_high_water;
    uint64_t failure_count;
    uint64_t budget_failure_count;
    uint64_t allocation_failure_count;
} player_identity_quarantine_store_t;

typedef struct {
    uint32_t initialized_tag;
    player_identity_canonical_store_t canonical_active;
    player_identity_canonical_store_t canonical_archive;
    player_identity_quarantine_store_t quarantine;
    uint64_t failure_status_counts[
        PLAYER_IDENTITY_REGISTRY_STATUS_COUNT];
} player_identity_registry_t;

/*
 * reg must be zero-initialized before its first init. A zero hard limit is
 * valid and provides a deterministic exhausted tier without allocating.
 */
player_identity_registry_status_t player_identity_registry_init(
    player_identity_registry_t *reg,
    const player_identity_registry_config_t *config);

/* Safe when repeated after a successful init or cleanup. */
void player_identity_registry_cleanup(player_identity_registry_t *reg);

/*
 * Deep-copy source into a zero-initialized or valid destination. Allocation
 * failure leaves an existing destination's semantic contents unchanged.
 */
player_identity_registry_status_t player_identity_registry_clone(
    player_identity_registry_t *destination,
    const player_identity_registry_t *source);

/*
 * Full structural and global-uniqueness validation. This fails closed if its
 * temporary actor-sort scratch cannot be allocated; status-returning APIs
 * distinguish that case as OUT_OF_MEMORY.
 */
bool player_identity_registry_validate(
    const player_identity_registry_t *reg);
bool player_identity_canonical_entry_is_valid(
    const player_identity_canonical_entry_t *entry);
bool player_identity_quarantine_entry_is_valid(
    const player_identity_quarantine_entry_t *entry);

/*
 * Full validation followed by binary lookup across the active tier and then
 * the archive. A valid lookup miss clears out_tier to NONE. Invalid arguments
 * or registry state leave caller output unchanged.
 */
player_identity_registry_status_t player_identity_registry_find(
    const player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    player_identity_registry_tier_t *out_tier);

/*
 * Explicit sensitive lookup for trusted internal authentication/persistence
 * code. out_entry carries the raw compatibility bearer. Returned entries are
 * copies, so later registry growth cannot invalidate caller storage. A valid
 * lookup miss clears out_entry and out_tier; invalid arguments, invalid state,
 * or an output alias into an occupied registry row leave outputs unchanged.
 */
player_identity_registry_status_t
player_identity_registry_find_sensitive(
    const player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    player_identity_canonical_entry_t *out_entry,
    player_identity_registry_tier_t *out_tier);

/*
 * Insert rejects a different mapping for an existing pubkey or an actor ID
 * already bound to any other canonical pubkey. Updating an existing
 * proof-backed mapping is intentionally a separate explicit action and
 * preserves the same global actor-ID uniqueness invariant.
 */
player_identity_registry_status_t player_identity_registry_insert_active(
    player_identity_registry_t *reg,
    const player_identity_canonical_entry_t *entry);
player_identity_registry_status_t player_identity_registry_update(
    player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    const uint8_t
        sensitive_actor_id[
            PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE]);
player_identity_registry_status_t player_identity_registry_archive(
    player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE]);
player_identity_registry_status_t player_identity_registry_restore(
    player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE]);

player_identity_registry_status_t
player_identity_registry_next_quarantine_id(
    const player_identity_registry_t *reg,
    uint64_t *out_record_id);
player_identity_registry_status_t
player_identity_registry_quarantine_append(
    player_identity_registry_t *reg,
    const player_identity_quarantine_entry_t *entry);

void player_identity_registry_get_metrics(
    const player_identity_registry_t *reg,
    player_identity_registry_metrics_t *out);

typedef struct {
    uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE];
} player_identity_canonical_redacted_t;

typedef struct {
    uint64_t record_id;
    uint32_t source_version;
    uint64_t source_ordinal;
    uint16_t reason;
    uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE];
} player_identity_quarantine_redacted_t;

typedef bool (*player_identity_canonical_visit_fn)(
    player_identity_registry_tier_t tier,
    size_t ordinal,
    const player_identity_canonical_redacted_t *entry,
    void *user);
typedef bool (*player_identity_quarantine_visit_fn)(
    size_t ordinal,
    const player_identity_quarantine_redacted_t *entry,
    void *user);

/*
 * Deterministic traversal order is active pubkey order, archived pubkey order,
 * then quarantine record-ID order. Either callback may be NULL.
 */
player_identity_registry_status_t player_identity_registry_visit(
    const player_identity_registry_t *reg,
    player_identity_canonical_visit_fn canonical_visit,
    player_identity_quarantine_visit_fn quarantine_visit,
    void *user);

typedef bool (*player_identity_canonical_sensitive_visit_fn)(
    player_identity_registry_tier_t tier,
    size_t ordinal,
    const player_identity_canonical_entry_t *entry,
    void *user);
typedef bool (*player_identity_quarantine_sensitive_visit_fn)(
    size_t ordinal,
    const player_identity_quarantine_entry_t *entry,
    void *user);

/*
 * Explicit sensitive traversal for protected internal persistence and repair
 * code only. Callbacks receive raw bearer compatibility bytes.
 */
player_identity_registry_status_t
player_identity_registry_visit_sensitive(
    const player_identity_registry_t *reg,
    player_identity_canonical_sensitive_visit_fn
        canonical_visit,
    player_identity_quarantine_sensitive_visit_fn
        quarantine_visit,
    void *user);

/*
 * Deterministic, explicitly little-endian sensitive export for protected
 * internal persistence only. It contains raw bearer compatibility bytes and
 * must never be returned from health/operator diagnostics, logged, or written
 * to an unprotected path. Callers own and must securely erase the output
 * buffer. Runtime allocation capacities and failure counters are excluded;
 * hard limits, semantic high-water values, canonical rows, and quarantine
 * evidence are included. This is not wired into the world-save format.
 * The header always sets
 * PLAYER_IDENTITY_REGISTRY_EXPORT_FLAG_SENSITIVE_BEARERS.
 *
 * On BUFFER_TOO_SMALL, out_written receives the required size and out is not
 * modified.
 */
player_identity_registry_status_t
player_identity_registry_export_sensitive_size(
    const player_identity_registry_t *reg,
    size_t *out_size);
player_identity_registry_status_t
player_identity_registry_export_sensitive(
    const player_identity_registry_t *reg,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_written);

bool player_identity_registry_status_is_success(
    player_identity_registry_status_t status);
const char *player_identity_registry_status_name(
    player_identity_registry_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* PLAYER_IDENTITY_REGISTRY_H */
