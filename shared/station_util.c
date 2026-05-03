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

int station_furnace_count(const station_t *st) {
    int n = 0;
    if (!st) return 0;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type != MODULE_FURNACE) continue;
        if (st->modules[m].scaffold) continue;
        n++;
    }
    return n;
}

bool station_can_smelt(const station_t *st, commodity_t ore) {
    if (!st) return false;
    if (!station_has_module(st, MODULE_HOPPER)) return false;
    int n = station_furnace_count(st);
    if (n <= 0) return false;
    switch (ore) {
        case COMMODITY_FERRITE_ORE: return n == 1 || n == 2;
        case COMMODITY_CUPRITE_ORE: return n >= 2;
        case COMMODITY_CRYSTAL_ORE: return n >= 3;
        default: return false;
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
        case COMMODITY_FERRITE_ORE:   return station_can_smelt(st, COMMODITY_FERRITE_ORE);
        case COMMODITY_CUPRITE_ORE:   return station_can_smelt(st, COMMODITY_CUPRITE_ORE);
        case COMMODITY_CRYSTAL_ORE:   return station_can_smelt(st, COMMODITY_CRYSTAL_ORE);
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
        case COMMODITY_FERRITE_INGOT: return station_can_smelt(st, COMMODITY_FERRITE_ORE);
        case COMMODITY_CUPRITE_INGOT: return station_can_smelt(st, COMMODITY_CUPRITE_ORE);
        case COMMODITY_CRYSTAL_INGOT: return station_can_smelt(st, COMMODITY_CRYSTAL_ORE);
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
        MODULE_FURNACE,
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
    /* For dominant=FURNACE we infer the smelt output from the count
     * tier: 3+ means crystal is the headline product, 2 means cuprite,
     * 1 means ferrite. Stations whose dominant module isn't a furnace
     * keep the existing fab-based primary sell. */
    if (dom == MODULE_FURNACE) {
        int n = station_furnace_count(st);
        if (n >= 3) return COMMODITY_CRYSTAL_INGOT;
        if (n == 2) return COMMODITY_CUPRITE_INGOT;
        if (n == 1) return COMMODITY_FERRITE_INGOT;
    }
    switch (dom) {
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

/* ------------------------------------------------------------------ */
/* Slot pairing — cross-ring                                           */
/* ------------------------------------------------------------------ */

/* Closest-slot search on a target ring for a given canonical angle.
 * Ring rotations don't enter here: pairing is defined statically on
 * canonical (zero-rotation) slot angles so the rule is verifiable at
 * construction time, not at runtime ring-spin time. */
static int closest_slot_on_ring(int ring, float angle) {
    int slots = STATION_RING_SLOTS[ring];
    int best = 0;
    float best_d = 1e9f;
    for (int j = 0; j < slots; j++) {
        float aj = TWO_PI_F * (float)j / (float)slots;
        float d = fabsf(aj - angle);
        if (d > PI_F) d = TWO_PI_F - d;
        if (d < best_d) { best_d = d; best = j; }
    }
    return best;
}

int station_pair_neighbors(int ring, int slot, station_slot_pair_t out[2]) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return 0;
    int slots = STATION_RING_SLOTS[ring];
    if (slot < 0 || slot >= slots) return 0;
    float angle = TWO_PI_F * (float)slot / (float)slots;
    int n = 0;
    /* Outer first (ring+1), then inner (ring-1). The smelt path
     * already prefers same-ordering for cross-ring beams, so this
     * keeps validator and renderer in agreement when a producer's
     * intake exists on both flanks. */
    int adj[] = { ring + 1, ring - 1 };
    for (int ri = 0; ri < 2; ri++) {
        int a = adj[ri];
        if (a < 1 || a > STATION_NUM_RINGS) continue;
        out[n].ring = a;
        out[n].slot = closest_slot_on_ring(a, angle);
        n++;
    }
    return n;
}

module_type_t station_module_at(const station_t *st, int ring, int slot) {
    if (!st || ring < 1 || ring > STATION_NUM_RINGS) return MODULE_COUNT;
    if (slot < 0 || slot >= STATION_RING_SLOTS[ring]) return MODULE_COUNT;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].ring != ring) continue;
        if (st->modules[i].slot != slot) continue;
        if (st->modules[i].scaffold) return MODULE_COUNT; /* not yet a real intake */
        return st->modules[i].type;
    }
    return MODULE_COUNT;
}

bool station_pair_satisfied(const station_t *st, int ring, int slot,
                            module_type_t type) {
    (void)ring; (void)slot;
    module_inputs_t req = module_required_inputs(type);
    if (req.count == 0) return true;
    if (req.any_satisfies) {
        for (int i = 0; i < req.count; i++) {
            if (station_find_hopper_for(st, req.commodities[i]) >= 0) return true;
        }
        return false;
    }
    /* All commodities must have a tagged hopper. */
    for (int i = 0; i < req.count; i++) {
        if (station_find_hopper_for(st, req.commodities[i]) < 0) return false;
    }
    return true;
}

int station_find_hopper_for(const station_t *st, commodity_t commodity) {
    if (!st) return -1;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_HOPPER) continue;
        if (st->modules[i].scaffold) continue;
        if ((commodity_t)st->modules[i].commodity == commodity) return i;
    }
    return -1;
}

int station_find_output_hopper_for_module(const station_t *st, const station_module_t *m) {
    if (!st || !m) return -1;
    commodity_t out = module_instance_output(m);
    if (out == COMMODITY_COUNT) return -1; /* services / hoppers / shipyard */
    return station_find_hopper_for(st, out);
}

station_layout_status_t station_module_layout_status(const station_t *st,
                                                     const station_module_t *m) {
    if (!st || !m) return STATION_LAYOUT_OK;
    if (m->scaffold) return STATION_LAYOUT_OK;
    if (!module_is_producer(m->type) && !module_is_shipyard(m->type)) return STATION_LAYOUT_OK;

    /* Inputs: every required input commodity must have a matching hopper.
     * For FURNACE the schema says any_satisfies; under per-instance tagging
     * the actual input is the one ore that matches the furnace's tag. */
    if (m->type == MODULE_FURNACE) {
        commodity_t ore = module_instance_input_ore(m);
        if (ore != COMMODITY_COUNT && station_find_hopper_for(st, ore) < 0) {
            return STATION_LAYOUT_MISSING_INPUT_HOPPER;
        }
    } else {
        module_inputs_t req = module_required_inputs(m->type);
        if (req.any_satisfies) {
            bool ok = false;
            for (int i = 0; i < req.count; i++) {
                if (station_find_hopper_for(st, req.commodities[i]) >= 0) { ok = true; break; }
            }
            if (req.count > 0 && !ok) return STATION_LAYOUT_MISSING_INPUT_HOPPER;
        } else {
            for (int i = 0; i < req.count; i++) {
                if (station_find_hopper_for(st, req.commodities[i]) < 0)
                    return STATION_LAYOUT_MISSING_INPUT_HOPPER;
            }
        }
    }

    /* Output hopper: required for every commodity-emitting producer.
     * SHIPYARD is exempt — its output is a physical scaffold body. */
    commodity_t out = module_instance_output(m);
    if (out != COMMODITY_COUNT && station_find_hopper_for(st, out) < 0) {
        return STATION_LAYOUT_MISSING_OUTPUT_HOPPER;
    }
    return STATION_LAYOUT_OK;
}

