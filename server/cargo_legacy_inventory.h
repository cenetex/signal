#ifndef SERVER_CARGO_LEGACY_INVENTORY_H
#define SERVER_CARGO_LEGACY_INVENTORY_H

#include "game_sim.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    CARGO_LEGACY_INVENTORY_SCHEMA_V1 = 1,
    /*
     * The startup audit is diagnostic, not an excuse for unbounded work on
     * operator-controlled save bytes. Once this many cargo rows have been
     * examined the report is marked truncated and normal trust remains
     * fail-closed for every unexamined row.
     */
    CARGO_LEGACY_INVENTORY_SCAN_LIMIT = 65536,
};

/*
 * Stable serialized/logical holder tags. Assigned ship-asset payloads alias
 * the authoritative live player/NPC ship and are scanned once under that live
 * holder. Dormant/destroyed asset snapshots use SHIP_ASSET_STORED.
 */
typedef enum {
    CARGO_LEGACY_HOLDER_STATION_MANIFEST = 0,
    CARGO_LEGACY_HOLDER_PLAYER_SHIP = 1,
    CARGO_LEGACY_HOLDER_NPC_SHIP = 2,
    CARGO_LEGACY_HOLDER_SHIP_ASSET_STORED = 3,
    CARGO_LEGACY_HOLDER_CARGO_POD_MANIFEST = 4,
    CARGO_LEGACY_HOLDER_CARGO_POD_SHELL = 5,
    CARGO_LEGACY_HOLDER_DELIVERY_SHIPMENT = 6,
    CARGO_LEGACY_HOLDER_COUNT = 7,
} cargo_legacy_holder_t;

/*
 * Stable, additive reason tags. Every discovered legacy/migrate row receives
 * EXPLICIT_REIDENTIFICATION_REQUIRED. Other reasons describe independent
 * fail-closed defects and therefore their counts may overlap.
 */
typedef enum {
    CARGO_LEGACY_REASON_EXPLICIT_REIDENTIFICATION_REQUIRED = 0,
    CARGO_LEGACY_REASON_MALFORMED_UNIT = 1,
    CARGO_LEGACY_REASON_DUPLICATE_IDENTITY = 2,
    CARGO_LEGACY_REASON_RECEIPT_SIDECAR_MISMATCH = 3,
    CARGO_LEGACY_REASON_RECEIPT_SIDECAR_ABSENT = 4,
    CARGO_LEGACY_REASON_UNRESOLVED_CUSTODIAN = 5,
    CARGO_LEGACY_REASON_HOLDER_BOUNDS_INVALID = 6,
    CARGO_LEGACY_REASON_SCAN_LIMIT_REACHED = 7,
    CARGO_LEGACY_REASON_COUNT = 8,
} cargo_legacy_inventory_reason_t;

typedef struct {
    uint32_t schema_version;
    uint32_t scan_limit;
    uint32_t units_examined;
    uint32_t legacy_candidates;
    uint32_t holder_candidate_count[CARGO_LEGACY_HOLDER_COUNT];
    uint32_t reason_count[CARGO_LEGACY_REASON_COUNT];
    uint32_t assigned_asset_aliases;
    uint32_t delivery_pod_aliases;
    uint32_t delivery_npc_ship_aliases;
    bool truncated;
    /*
     * Player save files are keyed/authenticated outside world_t and cannot be
     * safely directory-enumerated at this startup boundary. Loaded live
     * player ships are covered; unopened external player files are not.
     */
    bool external_player_saves_not_enumerated;
} cargo_legacy_inventory_report_t;

const char *cargo_legacy_holder_name(cargo_legacy_holder_t holder);
const char *cargo_legacy_inventory_reason_name(
    cargo_legacy_inventory_reason_t reason);

/*
 * Deterministic, read-only world inventory. It never signs, appends, mutates,
 * quarantines, guesses an owner, or rewrites a cargo byte. A true result may
 * still be truncated; callers must inspect report->truncated.
 */
bool cargo_legacy_inventory_scan_world(
    const world_t *world,
    cargo_legacy_inventory_report_t *report);

/*
 * Aggregate-only operator output in stable holder/reason order. Deliberately
 * emits no cargo identities, actor IDs, receipt bytes, session tokens,
 * station secrets, paths, or other bearer/private material.
 */
bool cargo_legacy_inventory_report_write(
    FILE *out,
    const cargo_legacy_inventory_report_t *report);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_CARGO_LEGACY_INVENTORY_H */
