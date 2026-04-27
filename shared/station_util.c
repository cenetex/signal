/*
 * station_util.c — implementations for station_util.h queries.
 *
 * Split out from the header so editing these helpers (frequent during
 * gameplay tuning — modules, primary-trade derivations, ring math)
 * doesn't recompile every TU that pulls station_t through types.h.
 */
#include <math.h>
#include "types.h"
#include "station_util.h"

bool station_exists(const station_t *st) {
    return st->signal_range > 0.0f || st->scaffold || st->planned || st->dock_radius > 0.0f;
}

bool station_is_active(const station_t *st) {
    return st->signal_range > 0.0f && !st->scaffold && !st->planned;
}

bool station_provides_docking(const station_t *st) {
    return st->dock_radius > 0.0f && !st->planned;
}

bool station_provides_signal(const station_t *st) {
    return st->signal_range > 0.0f && st->signal_connected && !st->planned;
}

bool station_collides(const station_t *st) {
    return st->radius > 0.0f && !st->planned;
}

bool station_has_module(const station_t *st, module_type_t type) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].type == type && !st->modules[i].scaffold) return true;
    return false;
}

int station_max_ring(const station_t *st) {
    int max = 1;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].scaffold) continue;
        int r = (int)st->modules[i].ring;
        if (r > max && r <= STATION_NUM_RINGS) max = r;
    }
    return max;
}

int station_spawn_fee(const station_t *st) {
    switch (station_max_ring(st)) {
        case 1:  return 50;
        case 2:  return 100;
        case 3:  return 300;
        default: return 50;
    }
}

bool station_consumes(const station_t *st, commodity_t c) {
    /* Shipyards consume frame + laser + tractor as kit-fab inputs.
     * Without this branch, a player who fills a frame contract at
     * Helios and has leftover frames can't sell them — Helios's frame
     * press doesn't exist locally, so the fallback skipped, even
     * though the kit fab will happily eat any extras. */
    bool is_shipyard = station_has_module(st, MODULE_SHIPYARD);
    switch (c) {
        case COMMODITY_FERRITE_ORE:   return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE:   return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE:   return station_has_module(st, MODULE_FURNACE_CR);
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_CUPRITE_INGOT:
            return station_has_module(st, MODULE_LASER_FAB) ||
                   station_has_module(st, MODULE_TRACTOR_FAB);
        case COMMODITY_CRYSTAL_INGOT:
            return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_FRAME:         return is_shipyard;
        case COMMODITY_LASER_MODULE:  return is_shipyard;
        case COMMODITY_TRACTOR_MODULE:return is_shipyard;
        case COMMODITY_REPAIR_KIT:
            return station_has_module(st, MODULE_DOCK) && !is_shipyard;
        default: return false;
    }
}

bool station_produces(const station_t *st, commodity_t c) {
    switch (c) {
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_INGOT: return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_INGOT: return station_has_module(st, MODULE_FURNACE_CR);
        case COMMODITY_FRAME:         return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_LASER_MODULE:  return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_TRACTOR_MODULE:return station_has_module(st, MODULE_TRACTOR_FAB);
        case COMMODITY_REPAIR_KIT:    return station_has_module(st, MODULE_SHIPYARD);
        default: return false;
    }
}

void rebuild_station_services(station_t *st) {
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

module_type_t station_dominant_module(const station_t *st) {
    static const module_type_t priority[] = {
        MODULE_FURNACE_CU, MODULE_FURNACE_CR, MODULE_FURNACE,
        MODULE_FRAME_PRESS, MODULE_LASER_FAB,
        MODULE_TRACTOR_FAB, MODULE_SIGNAL_RELAY, MODULE_HOPPER,
    };
    for (int p = 0; p < (int)(sizeof(priority) / sizeof(priority[0])); p++) {
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].type == priority[p]) return priority[p];
        }
    }
    return MODULE_DOCK;
}

commodity_t station_primary_buy(const station_t *st) {
    module_type_t dom = station_dominant_module(st);
    switch (dom) {
        case MODULE_FRAME_PRESS: return COMMODITY_FERRITE_INGOT;
        case MODULE_LASER_FAB:   return COMMODITY_CUPRITE_INGOT;
        case MODULE_TRACTOR_FAB: return COMMODITY_CUPRITE_INGOT;
        default: break;
    }
    return (commodity_t)-1;
}

commodity_t station_primary_sell(const station_t *st) {
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

float station_ring_rotation(const station_t *st, int ring) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return 0.0f;
    int idx = ring - 1;
    if (idx < MAX_ARMS) return st->arm_rotation[idx] + st->ring_offset[idx];
    return 0.0f;
}

vec2 module_world_pos_ring(const station_t *st, int ring, int slot) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return st->pos;
    int slots = STATION_RING_SLOTS[ring];
    float angle = TWO_PI_F * (float)slot / (float)slots + station_ring_rotation(st, ring);
    float r = STATION_RING_RADIUS[ring];
    return v2_add(st->pos, v2(cosf(angle) * r, sinf(angle) * r));
}

float module_angle_ring(const station_t *st, int ring, int slot) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return 0.0f;
    int slots = STATION_RING_SLOTS[ring];
    return TWO_PI_F * (float)slot / (float)slots + station_ring_rotation(st, ring);
}

int ring_module_count(const station_t *st, int ring) {
    int count = 0;
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].ring == ring) count++;
    return count;
}

bool station_has_ring(const station_t *st, int ring) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].ring == ring) return true;
    return false;
}

bool ring_has_dock(const station_t *st, int ring) {
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].ring == ring && st->modules[i].type == MODULE_DOCK && !st->modules[i].scaffold)
            return true;
    return false;
}

int station_ring_free_slot(const station_t *st, int ring, int port_count) {
    for (int slot = 0; slot < port_count; slot++) {
        bool taken = false;
        for (int i = 0; i < st->module_count; i++)
            if (st->modules[i].ring == ring && st->modules[i].slot == slot) { taken = true; break; }
        if (!taken) return slot;
    }
    return -1;
}
