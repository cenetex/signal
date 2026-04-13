/*
 * station_util.h — Inline helpers for querying and computing station
 * state. Extracted from types.h (#273) so changes to a helper don't
 * recompile every translation unit that just needs a struct definition.
 *
 * Includes: lifecycle predicates, module queries, dominant-module /
 * primary-trade derivations, ring rotation, module world position, and
 * ring-slot accounting.
 */
#ifndef STATION_UTIL_H
#define STATION_UTIL_H

#include <stdbool.h>
/* types.h includes this file at the bottom — don't re-include types.h.
 * All types (station_t, module_type_t, commodity_t, vec2, etc.) and the
 * STATION_NUM_RINGS / STATION_RING_RADIUS / STATION_RING_SLOTS constants
 * are available from the includer. Same pattern as station_geom.h. */

/* ------------------------------------------------------------------ */
/* Lifecycle predicates                                                */
/* ------------------------------------------------------------------ */

/* A station slot is in use if it has signal range, is under construction,
 * is planned, or has a dock radius. Empty/zeroed slots return false. */
static inline bool station_exists(const station_t *st) {
    return st->signal_range > 0.0f || st->scaffold || st->planned || st->dock_radius > 0.0f;
}

/* A station is active (fully built and operational). */
static inline bool station_is_active(const station_t *st) {
    return st->signal_range > 0.0f && !st->scaffold && !st->planned;
}

/* Should this station provide a dock ring? */
static inline bool station_provides_docking(const station_t *st) {
    return st->dock_radius > 0.0f && !st->planned;
}

/* Should this station contribute to signal coverage? */
static inline bool station_provides_signal(const station_t *st) {
    return st->signal_range > 0.0f && st->signal_connected && !st->planned;
}

/* Should this station participate in collision? */
static inline bool station_collides(const station_t *st) {
    return st->radius > 0.0f && !st->planned;
}

/* ------------------------------------------------------------------ */
/* Module queries                                                      */
/* ------------------------------------------------------------------ */

static inline bool station_has_module(const station_t *st, module_type_t type) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].type == type && !st->modules[i].scaffold) return true;
    return false;
}

/* Returns true if the station consumes this commodity as production input. */
static inline bool station_consumes(const station_t *st, commodity_t c) {
    switch (c) {
        case COMMODITY_FERRITE_ORE:   return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE:   return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE:   return station_has_module(st, MODULE_FURNACE_CR);
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_CUPRITE_INGOT: return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_CRYSTAL_INGOT: return station_has_module(st, MODULE_TRACTOR_FAB);
        default: return false;
    }
}

/* Returns true if the station produces this commodity (has the right module). */
static inline bool station_produces(const station_t *st, commodity_t c) {
    switch (c) {
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_INGOT: return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_INGOT: return station_has_module(st, MODULE_FURNACE_CR);
        case COMMODITY_FRAME:         return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_LASER_MODULE:  return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_TRACTOR_MODULE:return station_has_module(st, MODULE_TRACTOR_FAB);
        default: return false;
    }
}

static inline void rebuild_station_services(station_t *st) {
    st->services = 0;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].scaffold) continue;
        switch (st->modules[i].type) {
            case MODULE_REPAIR_BAY:     st->services |= STATION_SERVICE_REPAIR; break;
            case MODULE_LASER_FAB:      st->services |= STATION_SERVICE_UPGRADE_LASER; break;
            case MODULE_TRACTOR_FAB:    st->services |= STATION_SERVICE_UPGRADE_TRACTOR; break;
            case MODULE_FRAME_PRESS:    st->services |= STATION_SERVICE_UPGRADE_HOLD; break;
            default: break;
        }
    }
}

/* Return the dominant module type for display purposes (name, color, visual).
 * Priority: FURNACE > FRAME_PRESS > LASER_FAB > TRACTOR_FAB > SIGNAL_RELAY > others.
 * Returns MODULE_DOCK as fallback. */
static inline module_type_t station_dominant_module(const station_t *st) {
    static const module_type_t priority[] = {
        MODULE_FURNACE_CU, MODULE_FURNACE_CR, MODULE_FURNACE,
        MODULE_FRAME_PRESS, MODULE_LASER_FAB,
        MODULE_TRACTOR_FAB, MODULE_SIGNAL_RELAY, MODULE_ORE_BUYER,
    };
    for (int p = 0; p < (int)(sizeof(priority) / sizeof(priority[0])); p++) {
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].type == priority[p]) return priority[p];
        }
    }
    return MODULE_DOCK;
}

/* Primary trade slot: the one commodity this station buys from players.
 * Derived from the dominant production module. Returns -1 if none. */
static inline commodity_t station_primary_buy(const station_t *st) {
    module_type_t dom = station_dominant_module(st);
    switch (dom) {
        case MODULE_FURNACE:     return COMMODITY_FERRITE_ORE;
        case MODULE_FURNACE_CU:  return COMMODITY_CUPRITE_ORE;
        case MODULE_FURNACE_CR:  return COMMODITY_CRYSTAL_ORE;
        case MODULE_FRAME_PRESS: return COMMODITY_FERRITE_INGOT;
        case MODULE_LASER_FAB:   return COMMODITY_CUPRITE_INGOT;
        case MODULE_TRACTOR_FAB: return COMMODITY_CRYSTAL_INGOT;
        default: break;
    }
    return (commodity_t)-1;
}

/* Primary trade slot: the one commodity this station sells to players.
 * Derived from the dominant production module. Returns -1 if none. */
static inline commodity_t station_primary_sell(const station_t *st) {
    module_type_t dom = station_dominant_module(st);
    switch (dom) {
        case MODULE_FURNACE:     return COMMODITY_FERRITE_INGOT;
        case MODULE_FURNACE_CU:  return COMMODITY_CUPRITE_INGOT;
        case MODULE_FURNACE_CR:  return COMMODITY_CRYSTAL_INGOT;
        case MODULE_FRAME_PRESS: return COMMODITY_FRAME;
        case MODULE_LASER_FAB:   return COMMODITY_LASER_MODULE;
        case MODULE_TRACTOR_FAB: return COMMODITY_TRACTOR_MODULE;
        default: break;
    }
    return (commodity_t)-1;
}

/* ------------------------------------------------------------------ */
/* Ring rotation / module world geometry                               */
/* ------------------------------------------------------------------ */

/* Per-ring rotation: ring 1 fastest, outer rings slower. */
static inline float station_ring_rotation(const station_t *st, int ring) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return 0.0f;
    int idx = ring - 1;
    if (idx < MAX_ARMS) return st->arm_rotation[idx] + st->ring_offset[idx];
    return 0.0f;
}

/* World-space position of a module: ring determines radius,
 * slot determines angle. Each ring rotates independently. */
static inline vec2 module_world_pos_ring(const station_t *st, int ring, int slot) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return st->pos;
    int slots = STATION_RING_SLOTS[ring];
    float angle = TWO_PI_F * (float)slot / (float)slots + station_ring_rotation(st, ring);
    float r = STATION_RING_RADIUS[ring];
    return v2_add(st->pos, v2(cosf(angle) * r, sinf(angle) * r));
}

static inline float module_angle_ring(const station_t *st, int ring, int slot) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return 0.0f;
    int slots = STATION_RING_SLOTS[ring];
    return TWO_PI_F * (float)slot / (float)slots + station_ring_rotation(st, ring);
}

/* Count modules on a given ring. */
static inline int ring_module_count(const station_t *st, int ring) {
    int count = 0;
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].ring == ring) count++;
    return count;
}

static inline bool station_has_ring(const station_t *st, int ring) {
    /* A ring "exists" if any module is placed on it */
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].ring == ring) return true;
    return false;
}

/* A ring has a completed dock module — gates construction of the next ring. */
static inline bool ring_has_dock(const station_t *st, int ring) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].ring == ring && st->modules[i].type == MODULE_DOCK && !st->modules[i].scaffold)
            return true;
    return false;
}

static inline int station_ring_free_slot(const station_t *st, int ring, int port_count) {
    for (int slot = 0; slot < port_count; slot++) {
        bool taken = false;
        for (int i = 0; i < st->module_count; i++)
            if (st->modules[i].ring == ring && st->modules[i].slot == slot) { taken = true; break; }
        if (!taken) return slot;
    }
    return -1;
}

#endif
