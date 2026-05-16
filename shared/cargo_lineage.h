/*
 * cargo_lineage.h -- Small player-facing labels for cargo provenance.
 *
 * These helpers deliberately format the same cargo_unit_t fields that
 * verifiers consume, but with UI language: serial, parent, recipe, origin.
 */
#ifndef SHARED_CARGO_LINEAGE_H
#define SHARED_CARGO_LINEAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "manifest.h"
#include "mining.h"
#include "station_util.h"

static inline bool cargo_lineage_hash_is_zero(const uint8_t hash[32]) {
    if (!hash) return true;
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static inline void cargo_lineage_serial_label(const cargo_unit_t *unit,
                                              char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!unit || cargo_lineage_hash_is_zero(unit->pub)) {
        snprintf(out, cap, "-");
        return;
    }
    char label[12];
    mining_render_callsign(unit->pub, label);
    snprintf(out, cap, "%s", label);
}

static inline void cargo_lineage_parent_label(const cargo_unit_t *unit,
                                              char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!unit || cargo_lineage_hash_is_zero(unit->parent_merkle)) {
        snprintf(out, cap, "none");
        return;
    }
    char label[8];
    mining_callsign_from_pubkey(unit->parent_merkle, label);
    snprintf(out, cap, "%s", label);
}

static inline const char *cargo_lineage_recipe_label(const cargo_unit_t *unit) {
    if (!unit) return "unknown";
    switch ((recipe_id_t)unit->recipe_id) {
        case RECIPE_SMELT:          return "smelt";
        case RECIPE_FRAME_BASIC:    return "frame";
        case RECIPE_LASER_BASIC:    return "laser";
        case RECIPE_TRACTOR_COIL:   return "tractor";
        case RECIPE_REPAIR_KIT_FAB: return "repair kit";
        case RECIPE_LEGACY_MIGRATE: return "legacy";
        default:                    return "unknown";
    }
}

static inline void cargo_lineage_origin_label(const cargo_unit_t *unit,
                                              char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!unit) {
        snprintf(out, cap, "?");
        return;
    }
    snprintf(out, cap, "%s", station_short_name((int)unit->origin_station));
}

#endif /* SHARED_CARGO_LINEAGE_H */
