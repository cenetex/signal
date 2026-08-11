#include "ownership_quarantine.h"

#include "game_sim.h"

#include <string.h>

enum {
    OWNERSHIP_PENDING_SCAFFOLD_CAP =
        sizeof(((station_t *)0)->pending_scaffolds) /
        sizeof(((station_t *)0)->pending_scaffolds[0]),
    OWNERSHIP_PENDING_SHIP_BUILD_CAP =
        sizeof(((station_t *)0)->pending_ship_builds) /
        sizeof(((station_t *)0)->pending_ship_builds[0]),
    OWNERSHIP_PLACEMENT_PLAN_CAP =
        sizeof(((station_t *)0)->placement_plans) /
        sizeof(((station_t *)0)->placement_plans[0]),
};

static bool ownership_quarantine_global_row(
    const ownership_quarantine_entry_t *row,
    uint16_t row_cap) {
    return row->station_index == OWNERSHIP_QUARANTINE_NA &&
           row->row_index < row_cap;
}

static bool ownership_quarantine_station_row(
    const ownership_quarantine_entry_t *row,
    uint16_t row_cap) {
    return row->station_index < MAX_STATIONS &&
           row->row_index < row_cap;
}

static bool ownership_quarantine_station_only(
    const ownership_quarantine_entry_t *row,
    uint16_t first_station) {
    return row->station_index >= first_station &&
           row->station_index < MAX_STATIONS &&
           row->row_index == OWNERSHIP_QUARANTINE_NA;
}

static bool ownership_quarantine_locator_is_canonical(
    const ownership_quarantine_entry_t *row) {
    switch ((ownership_quarantine_source_kind_t)row->source_kind) {
        case OWNERSHIP_QUARANTINE_SOURCE_CONTRACT:
            return ownership_quarantine_global_row(row, MAX_CONTRACTS);
        case OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT:
            return ownership_quarantine_global_row(
                row, MAX_DELIVERY_SHIPMENTS);
        case OWNERSHIP_QUARANTINE_SOURCE_STATION_PLANNED_OWNER:
            return ownership_quarantine_station_only(
                row, SIGNAL_FIRST_OUTPOST_INDEX);
        case OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD:
            return ownership_quarantine_station_row(
                row, OWNERSHIP_PENDING_SCAFFOLD_CAP);
        case OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD:
            return ownership_quarantine_station_row(
                row, OWNERSHIP_PENDING_SHIP_BUILD_CAP);
        case OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN:
            return ownership_quarantine_station_row(
                row, OWNERSHIP_PLACEMENT_PLAN_CAP);
        case OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET:
            return ownership_quarantine_global_row(row, MAX_SHIP_ASSETS);
        case OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR:
            return ownership_quarantine_global_row(row, MAX_CARGO_PODS);
        case OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_TOWED:
        case OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_FRACTURED:
        case OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_THROWN_BY:
            return ownership_quarantine_global_row(row, MAX_ASTEROIDS);
        case OWNERSHIP_QUARANTINE_SOURCE_SCAFFOLD_OWNER:
            return ownership_quarantine_global_row(row, MAX_SCAFFOLDS);
        case OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER:
            return ownership_quarantine_station_only(
                row, SIGNAL_FIRST_OUTPOST_INDEX);
        case OWNERSHIP_QUARANTINE_SOURCE_NONE:
        case OWNERSHIP_QUARANTINE_SOURCE_COUNT:
        default:
            return false;
    }
}

static int ownership_quarantine_record_compare(
    const ownership_quarantine_entry_t *left,
    const ownership_quarantine_entry_t *right) {
    if (left->record_id != right->record_id)
        return left->record_id < right->record_id ? -1 : 1;
    return 0;
}

void ownership_quarantine_clear(ownership_quarantine_t *quarantine) {
    if (quarantine) memset(quarantine, 0, sizeof(*quarantine));
}

bool ownership_quarantine_entry_is_canonical(
    const ownership_quarantine_entry_t *row) {
    if (!row ||
        row->record_id == 0 ||
        row->source_kind <= OWNERSHIP_QUARANTINE_SOURCE_NONE ||
        row->source_kind >= OWNERSHIP_QUARANTINE_SOURCE_COUNT ||
        row->reason <= OWNERSHIP_QUARANTINE_REASON_NONE ||
        row->reason >= OWNERSHIP_QUARANTINE_REASON_COUNT) {
        return false;
    }

    if (row->reason ==
        OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN) {
        if (row->legacy_actor_code >= MAX_PLAYERS)
            return false;
    } else if (row->legacy_actor_code != OWNERSHIP_QUARANTINE_NA &&
               row->legacy_actor_code >= MAX_PLAYERS) {
        return false;
    }

    return ownership_quarantine_locator_is_canonical(row);
}

bool ownership_quarantine_validate(
    const ownership_quarantine_t *quarantine) {
    if (!quarantine || quarantine->count > OWNERSHIP_QUARANTINE_CAP)
        return false;
    for (uint16_t i = 0; i < quarantine->count; i++) {
        if (!ownership_quarantine_entry_is_canonical(
                &quarantine->entries[i])) {
            return false;
        }
        if (i > 0 &&
            ownership_quarantine_record_compare(
                &quarantine->entries[i - 1],
                &quarantine->entries[i]) >= 0) {
            return false;
        }
    }
    if (quarantine->count > 0 &&
        quarantine->entries[quarantine->count - 1].record_id >
            quarantine->record_id_high_water) {
        return false;
    }
    return true;
}

bool ownership_quarantine_next_record_id(
    const ownership_quarantine_t *quarantine,
    uint64_t *record_id_out) {
    if (!record_id_out ||
        !ownership_quarantine_validate(quarantine) ||
        quarantine->record_id_high_water == UINT64_MAX) {
        return false;
    }
    *record_id_out = quarantine->record_id_high_water + 1;
    return true;
}

bool ownership_quarantine_add(
    ownership_quarantine_t *quarantine,
    const ownership_quarantine_entry_t *entry) {
    return ownership_quarantine_add_batch(quarantine, entry, 1);
}

bool ownership_quarantine_add_batch(
    ownership_quarantine_t *quarantine,
    const ownership_quarantine_entry_t *entries,
    size_t entry_count) {
    if (!quarantine ||
        !ownership_quarantine_validate(quarantine)) {
        return false;
    }
    if (entry_count == 0) return true;
    if (!entries ||
        entry_count > SIZE_MAX / sizeof(*entries) ||
        entry_count >
            (size_t)OWNERSHIP_QUARANTINE_CAP - quarantine->count) {
        return false;
    }

    uint64_t previous_record_id =
        quarantine->record_id_high_water;
    for (size_t i = 0; i < entry_count; i++) {
        if (!ownership_quarantine_entry_is_canonical(&entries[i]) ||
            entries[i].record_id <= previous_record_id) {
            return false;
        }
        previous_record_id = entries[i].record_id;
    }

    /*
     * All fallible work is complete. memmove keeps the commit alias-safe when
     * callers stage candidates in the table's unused entry storage.
     */
    memmove(&quarantine->entries[quarantine->count],
            entries, entry_count * sizeof(*entries));
    quarantine->count =
        (uint16_t)(quarantine->count + entry_count);
    quarantine->record_id_high_water = previous_record_id;
    return true;
}

const char *ownership_quarantine_source_name(uint8_t source_kind) {
    switch ((ownership_quarantine_source_kind_t)source_kind) {
        case OWNERSHIP_QUARANTINE_SOURCE_NONE:
            return "none";
        case OWNERSHIP_QUARANTINE_SOURCE_CONTRACT:
            return "contract";
        case OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT:
            return "delivery_shipment";
        case OWNERSHIP_QUARANTINE_SOURCE_STATION_PLANNED_OWNER:
            return "station_planned_owner";
        case OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD:
            return "pending_scaffold";
        case OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD:
            return "pending_ship_build";
        case OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN:
            return "placement_plan";
        case OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET:
            return "ship_asset";
        case OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR:
            return "cargo_pod_tractor";
        case OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_TOWED:
            return "fracture_last_towed";
        case OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_FRACTURED:
            return "fracture_last_fractured";
        case OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_THROWN_BY:
            return "fracture_thrown_by";
        case OWNERSHIP_QUARANTINE_SOURCE_SCAFFOLD_OWNER:
            return "scaffold_owner";
        case OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER:
            return "outpost_founder";
        case OWNERSHIP_QUARANTINE_SOURCE_COUNT:
        default:
            return "unknown";
    }
}

const char *ownership_quarantine_reason_name(uint8_t reason) {
    switch ((ownership_quarantine_reason_t)reason) {
        case OWNERSHIP_QUARANTINE_REASON_NONE:
            return "none";
        case OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN:
            return "legacy_slot_unproven";
        case OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN:
            return "legacy_session_unproven";
        case OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL:
            return "invalid_principal";
        case OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL:
            return "conflicting_principal";
        case OWNERSHIP_QUARANTINE_REASON_LEGACY_BUILD_MODE_UNPROVEN:
            return "legacy_build_mode_unproven";
        case OWNERSHIP_QUARANTINE_REASON_COUNT:
        default:
            return "unknown";
    }
}

const char *ownership_quarantine_reason_description(uint8_t reason) {
    switch ((ownership_quarantine_reason_t)reason) {
        case OWNERSHIP_QUARANTINE_REASON_NONE:
            return "no quarantine reason";
        case OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN:
            return "legacy runtime slot had no unambiguous stable actor proof";
        case OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN:
            return "legacy session token is bearer material, not durable "
                   "ownership proof";
        case OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL:
            return "stored principal was malformed or noncanonical";
        case OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL:
            return "multiple records claimed conflicting stable principals";
        case OWNERSHIP_QUARANTINE_REASON_LEGACY_BUILD_MODE_UNPROVEN:
            return "legacy ship build may have consumed materials or reserved "
                   "birth fragments; input mode cannot be proven";
        case OWNERSHIP_QUARANTINE_REASON_COUNT:
        default:
            return "unknown quarantine reason";
    }
}

static bool ownership_quarantine_report_index(
    FILE *stream,
    const char *label,
    uint16_t value) {
    if (value == OWNERSHIP_QUARANTINE_NA)
        return fprintf(stream, " %s=n/a", label) >= 0;
    return fprintf(stream, " %s=%u", label, (unsigned)value) >= 0;
}

bool ownership_quarantine_report_bounded(
    FILE *out,
    const ownership_quarantine_t *quarantine,
    uint16_t max_rows) {
    if (!out || !ownership_quarantine_validate(quarantine))
        return false;
    if (fprintf(out,
                "ownership_quarantine count=%u record_id_high_water=%llu\n",
                (unsigned)quarantine->count,
                (unsigned long long)
                    quarantine->record_id_high_water) < 0) {
        return false;
    }
    uint16_t row_count = quarantine->count < max_rows
        ? quarantine->count : max_rows;
    for (uint16_t i = 0; i < row_count; i++) {
        const ownership_quarantine_entry_t *row =
            &quarantine->entries[i];
        if (fprintf(out,
                    "[%u] id=%llu source=%s reason=%s detail=\"%s\"",
                    (unsigned)i,
                    (unsigned long long)row->record_id,
                    ownership_quarantine_source_name(row->source_kind),
                    ownership_quarantine_reason_name(row->reason),
                    ownership_quarantine_reason_description(
                        row->reason)) < 0 ||
            !ownership_quarantine_report_index(
                out, "station", row->station_index) ||
            !ownership_quarantine_report_index(
                out, "row", row->row_index) ||
            !ownership_quarantine_report_index(
                out, "legacy_actor", row->legacy_actor_code) ||
            fputc('\n', out) == EOF) {
            return false;
        }
    }
    if (row_count < quarantine->count &&
        fprintf(out, "... omitted=%u\n",
                (unsigned)(quarantine->count - row_count)) < 0) {
        return false;
    }
    return ferror(out) == 0;
}

bool ownership_quarantine_report(
    FILE *out,
    const ownership_quarantine_t *quarantine) {
    if (!quarantine) return false;
    return ownership_quarantine_report_bounded(
        out, quarantine, quarantine->count);
}
