#ifndef OWNERSHIP_QUARANTINE_H
#define OWNERSHIP_QUARANTINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OWNERSHIP_QUARANTINE_CAP = 16384,
    OWNERSHIP_QUARANTINE_HEADER_WIRE_SIZE = 10,
    OWNERSHIP_QUARANTINE_ENTRY_WIRE_SIZE = 16,
};

#define OWNERSHIP_QUARANTINE_NA UINT16_MAX

typedef enum {
    OWNERSHIP_QUARANTINE_SOURCE_NONE = 0,
    OWNERSHIP_QUARANTINE_SOURCE_CONTRACT = 1,
    OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT = 2,
    OWNERSHIP_QUARANTINE_SOURCE_STATION_PLANNED_OWNER = 3,
    OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD = 4,
    OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD = 5,
    OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN = 6,
    OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET = 7,
    OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR = 8,
    OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_TOWED = 9,
    OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_FRACTURED = 10,
    OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_THROWN_BY = 11,
    OWNERSHIP_QUARANTINE_SOURCE_SCAFFOLD_OWNER = 12,
    OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER = 13,
    OWNERSHIP_QUARANTINE_SOURCE_COUNT = 14,
} ownership_quarantine_source_kind_t;

typedef enum {
    OWNERSHIP_QUARANTINE_REASON_NONE = 0,
    OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN = 1,
    OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN = 2,
    OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL = 3,
    OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL = 4,
    OWNERSHIP_QUARANTINE_REASON_COUNT = 5,
} ownership_quarantine_reason_t;

/*
 * Stable public diagnostic row. It deliberately contains no pubkeys,
 * session/reconnect tokens, signatures, or other bearer material.
 */
typedef struct {
    uint64_t record_id;
    uint8_t source_kind;
    uint8_t reason;
    uint16_t station_index;
    uint16_t row_index;
    uint16_t legacy_actor_code;
} ownership_quarantine_entry_t;

typedef struct {
    uint64_t record_id_high_water;
    uint16_t count;
    ownership_quarantine_entry_t entries[OWNERSHIP_QUARANTINE_CAP];
} ownership_quarantine_t;

#if defined(__cplusplus)
static_assert(
    OWNERSHIP_QUARANTINE_HEADER_WIRE_SIZE ==
        sizeof(uint64_t) + sizeof(uint16_t),
    "ownership quarantine header wire-size constant is stale");
static_assert(
    OWNERSHIP_QUARANTINE_ENTRY_WIRE_SIZE ==
        sizeof(uint64_t) + 2 * sizeof(uint8_t) + 3 * sizeof(uint16_t),
    "ownership quarantine row wire-size constant is stale");
static_assert(OWNERSHIP_QUARANTINE_CAP <= UINT16_MAX,
              "ownership quarantine count must represent the table capacity");
static_assert(OWNERSHIP_QUARANTINE_SOURCE_COUNT == 14,
              "ownership quarantine source tags are persistent");
static_assert(OWNERSHIP_QUARANTINE_REASON_COUNT == 5,
              "ownership quarantine reason tags are persistent");
#else
_Static_assert(
    OWNERSHIP_QUARANTINE_HEADER_WIRE_SIZE ==
        sizeof(uint64_t) + sizeof(uint16_t),
    "ownership quarantine header wire-size constant is stale");
_Static_assert(
    OWNERSHIP_QUARANTINE_ENTRY_WIRE_SIZE ==
        sizeof(uint64_t) + 2 * sizeof(uint8_t) + 3 * sizeof(uint16_t),
    "ownership quarantine row wire-size constant is stale");
_Static_assert(OWNERSHIP_QUARANTINE_CAP <= UINT16_MAX,
               "ownership quarantine count must represent the table capacity");
_Static_assert(OWNERSHIP_QUARANTINE_SOURCE_COUNT == 14,
               "ownership quarantine source tags are persistent");
_Static_assert(OWNERSHIP_QUARANTINE_REASON_COUNT == 5,
               "ownership quarantine reason tags are persistent");
#endif

/*
 * Reset a fresh-world or failed/legacy-decode table, including allocator
 * state. Operator reconciliation must remove rows without lowering the
 * persisted high-water mark.
 */
void ownership_quarantine_clear(ownership_quarantine_t *quarantine);

bool ownership_quarantine_entry_is_canonical(
    const ownership_quarantine_entry_t *entry);

/*
 * Validate canonical rows and strict ascending record IDs. A record ID is a
 * stable diagnostic identity allocated when the row is quarantined; locators
 * are snapshots and may legitimately recur after a source pool slot is reused.
 */
bool ownership_quarantine_validate(
    const ownership_quarantine_t *quarantine);

/*
 * Return the next never-before-issued record ID. The persisted high-water mark
 * survives future row reconciliation/removal so a diagnostic ID is not reused.
 */
bool ownership_quarantine_next_record_id(
    const ownership_quarantine_t *quarantine,
    uint64_t *record_id_out);

/*
 * Insert a canonical row in record-ID order. Invalid rows, invalid tables,
 * previously issued record IDs, and capacity exhaustion fail without
 * mutation. Call ownership_quarantine_next_record_id() for normal appends.
 */
bool ownership_quarantine_add(
    ownership_quarantine_t *quarantine,
    const ownership_quarantine_entry_t *entry);

const char *ownership_quarantine_source_name(uint8_t source_kind);
const char *ownership_quarantine_reason_name(uint8_t reason);
const char *ownership_quarantine_reason_description(uint8_t reason);

/*
 * Emit public operator diagnostics only. Returns false for an invalid table,
 * NULL stream, or stream error.
 */
bool ownership_quarantine_report(
    FILE *out,
    const ownership_quarantine_t *quarantine);

/*
 * Emit at most max_rows entries followed by an omitted-row count. This is the
 * safe automatic-startup path; the unbounded report above is reserved for an
 * explicit operator request.
 */
bool ownership_quarantine_report_bounded(
    FILE *out,
    const ownership_quarantine_t *quarantine,
    uint16_t max_rows);

#ifdef __cplusplus
}
#endif

#endif /* OWNERSHIP_QUARANTINE_H */
