/*
 * station_util.c — implementations for station_util.h queries.
 *
 * Split out from the header so editing these helpers (frequent during
 * gameplay tuning — modules, primary-trade derivations, ring math)
 * doesn't recompile every TU that pulls station_t through types.h.
 */
#include <math.h>
#include <stdio.h>   /* snprintf — station_short_name */
#include <string.h>
#include "types.h"
#include "commodity.h"
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

int station_nascent_scaffold_index(const scaffold_t *scaffolds,
                                   int scaffold_count,
                                   int station_idx) {
    if (!scaffolds || scaffold_count <= 0 || station_idx < 0) return -1;
    for (int i = 0; i < scaffold_count; i++) {
        if (!scaffolds[i].active) continue;
        if (scaffolds[i].state != SCAFFOLD_NASCENT) continue;
        if (scaffolds[i].built_at_station != station_idx) continue;
        return i;
    }
    return -1;
}

int station_construction_blocker_index(const station_t *st,
                                       const scaffold_t *scaffolds,
                                       int scaffold_count) {
    if (!st || !scaffolds || scaffold_count <= 0) return -1;
    float clear_r = STATION_RING_RADIUS[1] * 0.6f;
    float clear_r_sq = clear_r * clear_r;
    for (int i = 0; i < scaffold_count; i++) {
        if (!scaffolds[i].active) continue;
        if (scaffolds[i].state != SCAFFOLD_LOOSE) continue;
        if (v2_dist_sq(scaffolds[i].pos, st->pos) < clear_r_sq)
            return i;
    }
    return -1;
}

bool station_construction_area_blocked(const station_t *st,
                                       const scaffold_t *scaffolds,
                                       int scaffold_count) {
    return station_construction_blocker_index(st, scaffolds, scaffold_count) >= 0;
}

bool station_planned_site_abandoned(const station_t *st) {
    return st && st->planned && st->placement_plan_count <= 0;
}

bool station_construction_material_need(const station_t *st,
                                        station_construction_need_t *out) {
    if (!st || !out) return false;
    memset(out, 0, sizeof(*out));
    out->module_index = -1;

    if (st->scaffold && st->scaffold_progress < 0.999f) {
        float required = SCAFFOLD_MATERIAL_NEEDED;
        float supplied = required * st->scaffold_progress;
        if (supplied < 0.0f) supplied = 0.0f;
        if (supplied > required) supplied = required;
        *out = (station_construction_need_t){
            .station_shell = true,
            .module_index = -1,
            .module_type = MODULE_SIGNAL_RELAY,
            .material = COMMODITY_FRAME,
            .required = required,
            .supplied = supplied,
            .remaining = required - supplied,
        };
        return out->remaining > 0.001f;
    }

    for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
        const station_module_t *mod = &st->modules[m];
        if (module_build_state(mod) != MODULE_BUILD_AWAITING_SUPPLY) continue;
        float required = module_build_cost_lookup(mod->type);
        float supplied = required * module_supply_fraction(mod);
        if (supplied < 0.0f) supplied = 0.0f;
        if (supplied > required) supplied = required;
        *out = (station_construction_need_t){
            .station_shell = false,
            .module_index = m,
            .module_type = mod->type,
            .material = module_build_material_lookup(mod->type),
            .required = required,
            .supplied = supplied,
            .remaining = required - supplied,
        };
        return out->remaining > 0.001f;
    }

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
    if (!st) return (int)REPAIR_KITS_PER_RESPAWN;
    float kit_price = station_sell_price(st, COMMODITY_REPAIR_KIT);
    if (kit_price <= FLOAT_EPSILON)
        kit_price = st->base_price[COMMODITY_REPAIR_KIT] > FLOAT_EPSILON
                  ? st->base_price[COMMODITY_REPAIR_KIT] : 1.0f;
    int fee = (int)ceilf(kit_price * REPAIR_KITS_PER_RESPAWN);
    return fee > 0 ? fee : (int)REPAIR_KITS_PER_RESPAWN;
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

static bool station_has_adjacent_hopper_for(const station_t *st,
                                            const station_module_t *m,
                                            commodity_t commodity);
static bool station_has_adjacent_hopper_on_ring(const station_t *st,
                                                int ring,
                                                commodity_t commodity);

bool station_can_smelt(const station_t *st, commodity_t ore) {
    if (!st) return false;
    if (ore >= COMMODITY_RAW_ORE_COUNT) return false;

    int matching_pairs = 0;
    for (int m = 0; m < st->module_count; m++) {
        const station_module_t *f = &st->modules[m];
        if (f->type != MODULE_FURNACE || f->scaffold) continue;
        if (module_instance_input_ore(f) != ore) continue;
        if (!station_has_adjacent_hopper_for(st, f, ore)) continue;
        matching_pairs++;
    }
    if (ore == COMMODITY_CRYSTAL_ORE)
        return matching_pairs >= 2;
    return matching_pairs >= 1;
}

bool station_consumes(const station_t *st, commodity_t c) {
    /* Finished-good consumers feed demand, selling, and contracts. Frames
     * now sit in the middle of the whole ladder: fabs consume them to make
     * modules, and shipyards consume them again for repair-kit batches. */
    bool is_shipyard = station_has_module(st, MODULE_SHIPYARD);
    switch (c) {
        case COMMODITY_FERRITE_ORE:   return station_can_smelt(st, COMMODITY_FERRITE_ORE);
        case COMMODITY_CUPRITE_ORE:   return station_can_smelt(st, COMMODITY_CUPRITE_ORE);
        case COMMODITY_CRYSTAL_ORE:   return station_can_smelt(st, COMMODITY_CRYSTAL_ORE);
        case COMMODITY_FERRITE_INGOT: return station_has_module(st, MODULE_FRAME_PRESS);
        case COMMODITY_CUPRITE_INGOT:
            return station_has_module(st, MODULE_LASER_FAB);
        case COMMODITY_CRYSTAL_INGOT:
            return station_has_module(st, MODULE_TRACTOR_FAB);
        case COMMODITY_FRAME:
            return is_shipyard ||
                   station_has_module(st, MODULE_LASER_FAB) ||
                   station_has_module(st, MODULE_TRACTOR_FAB);
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
        case MODULE_TRACTOR_FAB: return COMMODITY_CRYSTAL_INGOT;
        default: break;
    }
    return (commodity_t)-1;
}

commodity_t station_primary_sell(const station_t *st) {
    module_type_t dom = station_dominant_module(st);
    /* For dominant=FURNACE, infer the headline product from furnace
     * instance tags rather than station-wide count tiers. Prefer the
     * highest-tier tagged ingot present. */
    if (dom == MODULE_FURNACE) {
        bool has_fe = false, has_cu = false, has_cr = false;
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].type != MODULE_FURNACE ||
                st->modules[i].scaffold) {
                continue;
            }
            commodity_t out = module_instance_output(&st->modules[i]);
            if (out == COMMODITY_FERRITE_INGOT) has_fe = true;
            if (out == COMMODITY_CUPRITE_INGOT) has_cu = true;
            if (out == COMMODITY_CRYSTAL_INGOT) has_cr = true;
        }
        if (has_cr) return COMMODITY_CRYSTAL_INGOT;
        if (has_cu) return COMMODITY_CUPRITE_INGOT;
        if (has_fe) return COMMODITY_FERRITE_INGOT;
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

float station_dock_lane_angle(const station_t *st, int ring, int slot) {
    if (ring < 1 || ring > STATION_NUM_RINGS) return 0.0f;
    int slots = STATION_RING_SLOTS[ring];
    if (slots <= 0) return module_angle_ring(st, ring, slot);
    float slot_arc = TWO_PI_F / (float)slots;
    float dir = (slot == 0) ? -1.0f : 1.0f;
    return module_angle_ring(st, ring, slot) + dir * slot_arc * 0.5f;
}

vec2 station_dock_lane_pos(const station_t *st, int ring, int slot,
                           float radius) {
    float angle = station_dock_lane_angle(st, ring, slot);
    return v2_add(st->pos, v2(cosf(angle) * radius, sinf(angle) * radius));
}

bool station_ring_open_gap_lane(const station_t *st, int ring,
                                int *out_slot, float *out_offset) {
    if (!st || ring < 1 || ring > STATION_NUM_RINGS) return false;
    int slots_n = STATION_RING_SLOTS[ring];
    if (slots_n <= 0) return false;

    int slots[MAX_MODULES_PER_STATION];
    int count = 0;
    for (int i = 0; i < st->module_count && count < MAX_MODULES_PER_STATION; i++) {
        if (st->modules[i].ring != ring) continue;
        slots[count++] = st->modules[i].slot;
    }
    if (count <= 0) return false;

    for (int i = 1; i < count; i++) {
        int tmp = slots[i];
        int j = i - 1;
        while (j >= 0 && slots[j] > tmp) {
            slots[j + 1] = slots[j];
            j--;
        }
        slots[j + 1] = tmp;
    }

    int first = slots[0];
    int last = slots[count - 1];
    int gap_slots = first + slots_n - last;
    if (gap_slots <= 0) gap_slots += slots_n;

    if (out_slot) *out_slot = last;
    if (out_offset) {
        float slot_arc = TWO_PI_F / (float)slots_n;
        *out_offset = slot_arc * (float)gap_slots * 0.5f;
    }
    return true;
}

float station_ring_open_gap_angle(const station_t *st, int ring) {
    int slot = 0;
    float offset = 0.0f;
    if (!station_ring_open_gap_lane(st, ring, &slot, &offset)) return 0.0f;
    return module_angle_ring(st, ring, slot) + offset;
}

vec2 station_ring_open_gap_lane_pos(const station_t *st, int ring,
                                    float radius) {
    float angle = station_ring_open_gap_angle(st, ring);
    return v2_add(st->pos, v2(cosf(angle) * radius, sinf(angle) * radius));
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
    (void)slot;
    module_inputs_t req = module_required_inputs(type);
    if (req.count == 0) return true;
    if (req.any_satisfies) {
        for (int i = 0; i < req.count; i++) {
            if (type == MODULE_FURNACE) {
                if (station_has_adjacent_hopper_on_ring(st, ring, req.commodities[i]))
                    return true;
            } else if (station_find_hopper_for(st, req.commodities[i]) >= 0) {
                return true;
            }
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

/* ------------------------------------------------------------------ */
/* Demand: top shortage, severity, recommended pay multiplier.         */
/* ------------------------------------------------------------------ */

/* Per-commodity supply target. Mirrors the targets the contract priority
 * ladder uses in game_sim.c (priority 3 for ore, priority 4 for ingots,
 * priority 5 for kit inputs) so the demand primitive and the contract
 * generator agree on what "starving" means. */
static float station_demand_target_for(const station_t *st, commodity_t c) {
    if (c < COMMODITY_RAW_ORE_COUNT) {
        /* Raw ore — matches priority 3 (REFINERY_HOPPER_CAPACITY * 0.5). */
        return REFINERY_HOPPER_CAPACITY * 0.5f;
    }
    switch (c) {
        case COMMODITY_FERRITE_INGOT:
        case COMMODITY_CUPRITE_INGOT:
        case COMMODITY_CRYSTAL_INGOT:
            /* Matches priority 4 (MAX_PRODUCT_STOCK * 0.9). */
            return MAX_PRODUCT_STOCK * 0.9f;
        case COMMODITY_FRAME:
        case COMMODITY_LASER_MODULE:
        case COMMODITY_TRACTOR_MODULE:
            /* Matches priority 5 kit_input_target. */
            return 12.0f;
        case COMMODITY_REPAIR_KIT:
            /* Non-shipyard dock buffer. Half the shipyard's per-batch
             * output keeps small docks topped up without making them
             * post a contract on every minor repair. */
            return 50.0f;
        default: break;
    }
    /* Unknown commodity — caller will see severity 0 and skip it. */
    if (st) (void)st;
    return 1.0f;
}

station_demand_t station_demand_for(const station_t *st, commodity_t c) {
    station_demand_t out = {
        .commodity  = COMMODITY_COUNT,
        .severity   = 0.0f,
        .price_mult = 1.0f,
    };
    if (!st || !station_is_active(st)) return out;
    if (c >= COMMODITY_COUNT) return out;
    if (!station_consumes(st, c)) return out;
    /* A station that produces what it consumes (e.g. Helios with its
     * own cuprite furnace feeding its laser fab) is not starving for
     * that commodity — the producer keeps the local shelf supplied.
     * Mirrors priority 4's "don't import what we make" check. */
    if (station_produces(st, c)) return out;

    float supply = st->_inventory_cache[c];
    float target = station_demand_target_for(st, c);
    if (target <= 0.0f) return out;

    float deficit = target - supply;
    if (deficit <= 0.0f) return out;
    float severity = deficit / target;
    if (severity > 1.0f) severity = 1.0f;

    out.commodity  = c;
    out.severity   = severity;
    /* 1.0× at no shortage, up to 1.5× at total starvation. The 0.5
     * slope is conservative — generous enough that haulers will
     * notice, gentle enough that players can't game the system by
     * deliberately starving a station they own. */
    out.price_mult = 1.0f + 0.5f * severity;
    return out;
}

static float shortage01(float stock, float target) {
    if (target <= 0.0f) return 0.0f;
    float deficit = target - stock;
    if (deficit <= 0.0f) return 0.0f;
    float score = deficit / target;
    return score > 1.0f ? 1.0f : score;
}

float station_raw_ore_chain_need_score(const station_t *st, commodity_t ore) {
    if (!st || !station_is_active(st)) return 0.0f;
    if (ore >= COMMODITY_RAW_ORE_COUNT) return 0.0f;
    if (!station_can_smelt(st, ore)) return 0.0f;

    commodity_t ingot = commodity_refined_form(ore);
    if (ingot == ore || ingot >= COMMODITY_COUNT) return 0.0f;

    float ingot_room = MAX_PRODUCT_STOCK - st->_inventory_cache[ingot];
    if (ingot_room <= FLOAT_EPSILON) return 0.0f;

    float score = shortage01(st->_inventory_cache[ingot], MAX_PRODUCT_STOCK);
    switch (ore) {
    case COMMODITY_FERRITE_ORE:
        if (station_has_module(st, MODULE_FRAME_PRESS)) {
            score = fmaxf(shortage01(st->_inventory_cache[COMMODITY_FRAME],
                                      MAX_PRODUCT_STOCK),
                          shortage01(st->_inventory_cache[ingot], 12.0f) * 0.5f);
        }
        break;
    case COMMODITY_CUPRITE_ORE:
        if (station_has_module(st, MODULE_LASER_FAB)) {
            score = fmaxf(shortage01(st->_inventory_cache[COMMODITY_LASER_MODULE],
                                      12.0f),
                          shortage01(st->_inventory_cache[ingot], 12.0f) * 0.5f);
        }
        break;
    case COMMODITY_CRYSTAL_ORE:
        if (station_has_module(st, MODULE_TRACTOR_FAB)) {
            score = fmaxf(shortage01(st->_inventory_cache[COMMODITY_TRACTOR_MODULE],
                                      12.0f),
                          shortage01(st->_inventory_cache[ingot], 12.0f) * 0.5f);
        }
        break;
    default:
        break;
    }

    return score;
}

float station_raw_ore_need_score(const station_t *st, commodity_t ore) {
    float chain_need = station_raw_ore_chain_need_score(st, ore);
    if (chain_need <= 0.0f) return 0.0f;
    float raw_gate = shortage01(st->_inventory_cache[ore],
                                REFINERY_HOPPER_CAPACITY * 0.5f);
    return chain_need * raw_gate;
}

station_demand_t station_top_demand(const station_t *st) {
    station_demand_t out = {
        .commodity  = COMMODITY_COUNT,
        .severity   = 0.0f,
        .price_mult = 1.0f,
    };
    if (!st || !station_is_active(st)) return out;

    for (int c = 0; c < COMMODITY_COUNT; c++) {
        station_demand_t d = station_demand_for(st, (commodity_t)c);
        if (d.severity > out.severity) out = d;
    }
    return out;
}

static bool station_has_adjacent_hopper_for(const station_t *st,
                                            const station_module_t *m,
                                            commodity_t commodity) {
    if (!m) return false;
    return station_has_adjacent_hopper_on_ring(st, (int)m->ring, commodity);
}

static bool station_has_adjacent_hopper_on_ring(const station_t *st,
                                                int ring,
                                                commodity_t commodity) {
    if (!st || commodity == COMMODITY_COUNT) return false;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *h = &st->modules[i];
        if (h->scaffold) continue;
        if (h->type != MODULE_HOPPER) continue;
        if ((commodity_t)h->commodity != commodity) continue;
        int dr = (int)h->ring - ring;
        if (dr == 1 || dr == -1) return true;
    }
    return false;
}

commodity_t station_default_module_commodity(const station_t *st,
                                             module_type_t type) {
    if (type == MODULE_FURNACE)
        return module_furnace_default_output();
    if (type != MODULE_HOPPER)
        return COMMODITY_COUNT;

    if (st) {
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_inputs_t req = module_instance_required_inputs(&st->modules[m]);
            for (int i = 0; i < req.count; i++) {
                commodity_t c = req.commodities[i];
                bool covered = false;
                for (int n = 0; n < st->module_count; n++) {
                    if (st->modules[n].scaffold) continue;
                    if (st->modules[n].type != MODULE_HOPPER) continue;
                    if ((commodity_t)st->modules[n].commodity == c) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) return c;
            }
        }
    }
    return COMMODITY_FERRITE_ORE;
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
        if (ore != COMMODITY_COUNT && !station_has_adjacent_hopper_for(st, m, ore)) {
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

    /* Output hopper: required only when a different on-station producer
     * declares the same commodity as one of its inputs. If nothing
     * locally consumes this output, the smelt/fab pipeline writes
     * straight to station inventory and a hauler picks it up — no
     * staging hopper needed. SHIPYARD's output is a physical scaffold
     * body and falls through the COMMODITY_COUNT branch naturally. */
    commodity_t out = module_instance_output(m);
    if (out != COMMODITY_COUNT) {
        bool has_local_consumer = false;
        for (int j = 0; j < st->module_count && !has_local_consumer; j++) {
            if (j == (int)(m - st->modules)) continue;
            if (st->modules[j].scaffold) continue;
            module_inputs_t cons = module_instance_required_inputs(&st->modules[j]);
            for (int k = 0; k < cons.count; k++) {
                if (cons.commodities[k] == out) { has_local_consumer = true; break; }
            }
        }
        if (has_local_consumer && station_find_hopper_for(st, out) < 0) {
            return STATION_LAYOUT_MISSING_OUTPUT_HOPPER;
        }
    }
    return STATION_LAYOUT_OK;
}

/* ------------------------------------------------------------------ */
/* Module-flow diagnostics                                             */
/* ------------------------------------------------------------------ */

#define STATION_FLOW_DIAG_EPS 0.01f
#define STATION_FLOW_DIAG_SLOW_RATE 1.0f

static int station_flow_ring_slot_distance(int slot_a, int slot_b,
                                           int total_slots) {
    int d = slot_a - slot_b;
    if (d < 0) d = -d;
    if (total_slots > 0 && d > total_slots / 2)
        d = total_slots - d;
    return d > 0 ? d : 1;
}

/* Keep this in sync with server/sim_production.c::module_flow_rate:
 * diagnostics should describe the same flow graph the sim actually runs. */
static float station_flow_rate_between_modules(const station_module_t *p,
                                               const station_module_t *c) {
    if (p->ring == c->ring && p->ring >= 1 && p->ring <= STATION_NUM_RINGS) {
        int slots = STATION_RING_SLOTS[p->ring];
        int d = station_flow_ring_slot_distance((int)p->slot, (int)c->slot, slots);
        return 5.0f / (float)d;
    }
    if (p->ring >= 1 && p->ring <= STATION_NUM_RINGS &&
        c->ring >= 1 && c->ring <= STATION_NUM_RINGS) {
        float p_angle = TWO_PI_F * (float)p->slot / (float)STATION_RING_SLOTS[p->ring];
        float c_angle = TWO_PI_F * (float)c->slot / (float)STATION_RING_SLOTS[c->ring];
        float da = fabsf(p_angle - c_angle);
        if (da > PI_F) da = TWO_PI_F - da;
        float t = da / PI_F;
        return 3.0f - t * 2.5f;
    }
    return 0.5f;
}

static float station_flow_rate_between(const station_t *st,
                                       int producer_idx,
                                       int consumer_idx) {
    return station_flow_rate_between_modules(&st->modules[producer_idx],
                                             &st->modules[consumer_idx]);
}

static bool station_flow_accepts_input(const station_module_t *consumer,
                                       commodity_t commodity) {
    const module_schema_t *cs = module_schema(consumer->type);
    if (cs->kind == MODULE_KIND_PRODUCER) {
        commodity_t input = consumer->type == MODULE_FURNACE
                          ? module_instance_input_ore(consumer)
                          : cs->input;
        if (input == commodity) return true;
    }
    if (cs->kind == MODULE_KIND_STORAGE) {
        if ((commodity_t)consumer->commodity == commodity) return true;
    }
    return false;
}

typedef struct {
    bool any;
    bool any_space;
    float best_rate;
} station_flow_consumer_status_t;

static station_flow_consumer_status_t
station_flow_consumer_status(const station_t *st, int producer_idx,
                             commodity_t commodity) {
    station_flow_consumer_status_t status = {0};
    for (int c = 0; c < st->module_count; c++) {
        if (c == producer_idx) continue;
        if (st->modules[c].scaffold) continue;
        if (!station_flow_accepts_input(&st->modules[c], commodity)) continue;
        float cap = module_buffer_capacity(st->modules[c].type);
        if (cap <= 0.0f) continue;
        status.any = true;
        if (st->module_input[c] + STATION_FLOW_DIAG_EPS >= cap) continue;
        status.any_space = true;
        float rate = station_flow_rate_between(st, producer_idx, c);
        if (rate > status.best_rate) status.best_rate = rate;
    }
    return status;
}

static commodity_t station_flow_storage_tag(const station_module_t *m) {
    commodity_t tag = (commodity_t)m->commodity;
    if (tag >= COMMODITY_COUNT) return COMMODITY_COUNT;
    /* Raw-ore hoppers are physical fragment-smelt anchors. Finished
     * goods hoppers participate in module flow. */
    if (tag < COMMODITY_RAW_ORE_COUNT) return COMMODITY_COUNT;
    return tag;
}

station_flow_diag_t station_module_flow_diag(const station_t *st,
                                             int module_index) {
    if (!st) return STATION_FLOW_DIAG_NONE;
    if (!station_is_active(st)) return STATION_FLOW_DIAG_NONE;
    if (module_index < 0 || module_index >= st->module_count ||
        module_index >= MAX_MODULES_PER_STATION)
        return STATION_FLOW_DIAG_NONE;

    const station_module_t *m = &st->modules[module_index];
    if (m->scaffold) {
        return m->build_progress < 1.0f
             ? STATION_FLOW_DIAG_AWAITING_SUPPLY
             : STATION_FLOW_DIAG_NONE;
    }

    module_kind_t kind = module_kind(m->type);
    if (kind == MODULE_KIND_SERVICE || kind == MODULE_KIND_NONE)
        return STATION_FLOW_DIAG_NONE;

    if (kind == MODULE_KIND_STORAGE) {
        commodity_t tag = station_flow_storage_tag(m);
        if (tag == COMMODITY_COUNT) return STATION_FLOW_DIAG_NONE;
        float cap = module_buffer_capacity(m->type);
        if (cap > 0.0f && st->module_output[module_index] + STATION_FLOW_DIAG_EPS >= cap)
            return STATION_FLOW_DIAG_OUTPUT_FULL;
        station_flow_consumer_status_t downstream =
            station_flow_consumer_status(st, module_index, tag);
        if (st->module_output[module_index] > STATION_FLOW_DIAG_EPS) {
            if (!downstream.any) return STATION_FLOW_DIAG_NO_CONSUMER;
            if (!downstream.any_space) return STATION_FLOW_DIAG_CONSUMER_FULL;
            if (downstream.best_rate <= STATION_FLOW_DIAG_SLOW_RATE)
                return STATION_FLOW_DIAG_SLOW_FEED;
            return STATION_FLOW_DIAG_RUNNING;
        }
        if (downstream.any && !downstream.any_space)
            return STATION_FLOW_DIAG_CONSUMER_FULL;
        if (downstream.any && st->_inventory_cache[tag] <= STATION_FLOW_DIAG_EPS)
            return STATION_FLOW_DIAG_NO_INPUT;
        return downstream.any_space ? STATION_FLOW_DIAG_RUNNING
                                    : STATION_FLOW_DIAG_NONE;
    }

    if (kind == MODULE_KIND_SHIPYARD) {
        if (st->pending_scaffold_count <= 0) return STATION_FLOW_DIAG_NONE;
        return st->module_input[module_index] > STATION_FLOW_DIAG_EPS
             ? STATION_FLOW_DIAG_RUNNING
             : STATION_FLOW_DIAG_NO_INPUT;
    }

    commodity_t output = module_instance_output(m);
    float cap = module_buffer_capacity(m->type);
    if (cap > 0.0f && st->module_output[module_index] + STATION_FLOW_DIAG_EPS >= cap)
        return STATION_FLOW_DIAG_OUTPUT_FULL;

    if (output != COMMODITY_COUNT &&
        st->module_output[module_index] > STATION_FLOW_DIAG_EPS) {
        station_flow_consumer_status_t downstream =
            station_flow_consumer_status(st, module_index, output);
        if (!downstream.any) return STATION_FLOW_DIAG_NO_CONSUMER;
        if (!downstream.any_space) return STATION_FLOW_DIAG_CONSUMER_FULL;
        if (downstream.best_rate <= STATION_FLOW_DIAG_SLOW_RATE)
            return STATION_FLOW_DIAG_SLOW_FEED;
    }

    commodity_t input = m->type == MODULE_FURNACE
                      ? module_instance_input_ore(m)
                      : module_schema_input(m->type);
    if (input != COMMODITY_COUNT &&
        st->module_input[module_index] <= STATION_FLOW_DIAG_EPS)
        return STATION_FLOW_DIAG_NO_INPUT;

    if (st->module_input[module_index] > STATION_FLOW_DIAG_EPS ||
        st->module_output[module_index] > STATION_FLOW_DIAG_EPS ||
        st->module_craft_progress[module_index] > STATION_FLOW_DIAG_EPS)
        return STATION_FLOW_DIAG_RUNNING;

    return STATION_FLOW_DIAG_NONE;
}

const char *station_flow_diag_label(station_flow_diag_t diag) {
    switch (diag) {
    case STATION_FLOW_DIAG_RUNNING:         return "running";
    case STATION_FLOW_DIAG_NO_INPUT:        return "missing input";
    case STATION_FLOW_DIAG_OUTPUT_FULL:     return "output full";
    case STATION_FLOW_DIAG_NO_CONSUMER:     return "no valid consumer";
    case STATION_FLOW_DIAG_CONSUMER_FULL:   return "consumer full";
    case STATION_FLOW_DIAG_SLOW_FEED:       return "slow route";
    case STATION_FLOW_DIAG_AWAITING_SUPPLY: return "scaffold needs supply";
    case STATION_FLOW_DIAG_NONE:
    default:                                return "idle";
    }
}

static int station_clamped_module_count(int module_count)
{
    if (module_count < 0) return 0;
    if (module_count > MAX_MODULES_PER_STATION) return MAX_MODULES_PER_STATION;
    return module_count;
}

static bool station_module_identity_equal(const station_module_t *a,
                                          const station_module_t *b)
{
    if (!a || !b) return false;
    return a->type == b->type &&
           a->ring == b->ring &&
           a->slot == b->slot &&
           a->scaffold == b->scaffold &&
           a->commodity == b->commodity;
}

void station_reconcile_module_diag_for_identity(station_t *st,
                                                const station_module_t *modules,
                                                int module_count)
{
    if (!st) return;
    int old_count = station_clamped_module_count(st->module_count);
    int new_count = station_clamped_module_count(module_count);
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        bool same_live_slot = modules &&
            m < old_count &&
            m < new_count &&
            station_module_identity_equal(&st->modules[m], &modules[m]);
        if (!same_live_slot)
            st->module_diag[m] = STATION_FLOW_DIAG_NONE;
    }
}

station_flow_diag_t station_module_flow_diag_view(const station_t *st,
                                                  int module_index,
                                                  bool mirrored_authoritative)
{
    if (!st || module_index < 0 || module_index >= st->module_count ||
        module_index >= MAX_MODULES_PER_STATION) {
        return STATION_FLOW_DIAG_NONE;
    }
    if (mirrored_authoritative)
        return (station_flow_diag_t)st->module_diag[module_index];
    if (st->module_diag[module_index] != STATION_FLOW_DIAG_NONE)
        return (station_flow_diag_t)st->module_diag[module_index];
    return station_module_flow_diag(st, module_index);
}

static int station_flow_diag_rank(station_flow_diag_t diag)
{
    switch (diag) {
    case STATION_FLOW_DIAG_AWAITING_SUPPLY: return 60;
    case STATION_FLOW_DIAG_OUTPUT_FULL:     return 50;
    case STATION_FLOW_DIAG_CONSUMER_FULL:   return 49;
    case STATION_FLOW_DIAG_NO_CONSUMER:     return 48;
    case STATION_FLOW_DIAG_NO_INPUT:        return 47;
    case STATION_FLOW_DIAG_SLOW_FEED:       return 30;
    case STATION_FLOW_DIAG_RUNNING:         return 10;
    case STATION_FLOW_DIAG_NONE:
    default:                                return 0;
    }
}

bool station_flow_summary(const station_t *st, bool mirrored_authoritative,
                          station_flow_summary_t *out)
{
    if (!out) return false;
    *out = (station_flow_summary_t){
        .diag = STATION_FLOW_DIAG_NONE,
        .module_index = -1,
        .module_type = MODULE_COUNT,
        .active_count = 0,
    };
    if (!st) return false;

    int best_rank = 0;
    for (int i = 0; i < st->module_count && i < MAX_MODULES_PER_STATION; i++) {
        station_flow_diag_t diag =
            station_module_flow_diag_view(st, i, mirrored_authoritative);
        if (diag == STATION_FLOW_DIAG_NONE) continue;
        if (diag == STATION_FLOW_DIAG_RUNNING) out->active_count++;
        int rank = station_flow_diag_rank(diag);
        if (rank > best_rank) {
            best_rank = rank;
            out->diag = diag;
            out->module_index = i;
            out->module_type = st->modules[i].type;
        }
    }

    if (out->module_index >= 0 && out->diag != STATION_FLOW_DIAG_RUNNING)
        return true;
    if (out->active_count > 0) {
        out->diag = STATION_FLOW_DIAG_RUNNING;
        out->module_index = -1;
        out->module_type = MODULE_COUNT;
        return true;
    }
    *out = (station_flow_summary_t){
        .diag = STATION_FLOW_DIAG_NONE,
        .module_index = -1,
        .module_type = MODULE_COUNT,
        .active_count = 0,
    };
    return false;
}

bool station_flow_summary_format(const station_flow_summary_t *summary,
                                 char *out, size_t cap)
{
    if (!summary || !out || cap == 0) return false;
    out[0] = '\0';
    if (summary->diag == STATION_FLOW_DIAG_NONE) return false;

    if (summary->diag == STATION_FLOW_DIAG_RUNNING) {
        int active = summary->active_count > 0 ? summary->active_count : 1;
        snprintf(out, cap, "FLOW %d module%s active", active,
                 active == 1 ? "" : "s");
        return true;
    }

    const char *module = "module";
    if ((int)summary->module_type >= 0 && summary->module_type < MODULE_COUNT)
        module = module_type_name(summary->module_type);
    snprintf(out, cap, "FLOW %s: %s", module,
             station_flow_diag_label(summary->diag));
    return true;
}

#define STATION_PLAN_CANDIDATE_CAP (MAX_MODULES_PER_STATION + 8)

static station_module_t station_plan_make_module(const station_t *st,
                                                 module_type_t type,
                                                 int ring,
                                                 int slot)
{
    station_module_t m = {0};
    m.type = type;
    m.ring = (uint8_t)ring;
    m.slot = (uint8_t)slot;
    m.build_progress = 1.0f;
    m.last_smelt_commodity = LAST_SMELT_NONE;
    m.commodity = (uint8_t)station_default_module_commodity(st, type);
    return m;
}

static bool station_plan_same_slot(const station_module_t *m, int ring, int slot)
{
    return m && (int)m->ring == ring && (int)m->slot == slot;
}

static int station_plan_collect_candidates(const station_t *st,
                                           int skip_ring,
                                           int skip_slot,
                                           station_module_t *out,
                                           int cap)
{
    int count = 0;
    if (!st || !out || cap <= 0) return 0;
    for (int i = 0; i < st->module_count && count < cap; i++) {
        const station_module_t *m = &st->modules[i];
        if (m->scaffold) continue;
        if (station_plan_same_slot(m, skip_ring, skip_slot)) continue;
        out[count++] = *m;
    }
    for (int p = 0; p < st->placement_plan_count && count < cap; p++) {
        int ring = (int)st->placement_plans[p].ring;
        int slot = (int)st->placement_plans[p].slot;
        if (ring == skip_ring && slot == skip_slot) continue;
        out[count++] = station_plan_make_module(
            st, st->placement_plans[p].type, ring, slot);
    }
    return count;
}

static commodity_t station_plan_module_output(const station_module_t *m)
{
    if (!m) return COMMODITY_COUNT;
    if (module_kind(m->type) == MODULE_KIND_STORAGE) {
        commodity_t tag = (commodity_t)m->commodity;
        return tag < COMMODITY_COUNT ? tag : COMMODITY_COUNT;
    }
    return module_instance_output(m);
}

static bool station_plan_best_output_peer(const station_module_t *producer,
                                          commodity_t commodity,
                                          const station_module_t *mods,
                                          int mod_count,
                                          const station_module_t **out_peer,
                                          float *out_rate)
{
    const station_module_t *best = NULL;
    float best_rate = 0.0f;
    if (!producer || commodity == COMMODITY_COUNT) return false;
    for (int i = 0; i < mod_count; i++) {
        const station_module_t *peer = &mods[i];
        if (!station_flow_accepts_input(peer, commodity)) continue;
        if (module_buffer_capacity(peer->type) <= 0.0f) continue;
        float rate = station_flow_rate_between_modules(producer, peer);
        if (rate > best_rate) {
            best_rate = rate;
            best = peer;
        }
    }
    if (!best) return false;
    if (out_peer) *out_peer = best;
    if (out_rate) *out_rate = best_rate;
    return true;
}

static bool station_plan_best_input_peer(const station_module_t *consumer,
                                         commodity_t commodity,
                                         const station_module_t *mods,
                                         int mod_count,
                                         const station_module_t **out_peer,
                                         float *out_rate)
{
    const station_module_t *best = NULL;
    float best_rate = 0.0f;
    if (!consumer || commodity == COMMODITY_COUNT) return false;
    for (int i = 0; i < mod_count; i++) {
        const station_module_t *peer = &mods[i];
        if (station_plan_module_output(peer) != commodity) continue;
        if (module_buffer_capacity(peer->type) <= 0.0f) continue;
        float rate = station_flow_rate_between_modules(peer, consumer);
        if (rate > best_rate) {
            best_rate = rate;
            best = peer;
        }
    }
    if (!best) return false;
    if (out_peer) *out_peer = best;
    if (out_rate) *out_rate = best_rate;
    return true;
}

static void station_plan_consider_hint(station_plan_flow_hint_t *out,
                                       station_flow_diag_t diag,
                                       station_plan_flow_role_t role,
                                       const station_module_t *peer,
                                       commodity_t commodity,
                                       float rate)
{
    int rank = station_flow_diag_rank(diag);
    int old_rank = station_flow_diag_rank(out->diag);
    if (rank < old_rank) return;
    if (rank == old_rank &&
        !(out->role == STATION_PLAN_FLOW_ROLE_INPUT &&
          role == STATION_PLAN_FLOW_ROLE_OUTPUT))
        return;
    out->diag = diag;
    out->role = role;
    out->peer_type = peer ? peer->type : MODULE_COUNT;
    out->commodity = commodity;
    out->rate = rate;
}

bool station_plan_flow_hint(const station_t *st, module_type_t type,
                            int ring, int slot,
                            station_plan_flow_hint_t *out)
{
    station_module_t planned;
    station_module_t candidates[STATION_PLAN_CANDIDATE_CAP];
    int candidate_count;
    module_kind_t kind;

    if (!out) return false;
    *out = (station_plan_flow_hint_t){
        .diag = STATION_FLOW_DIAG_NONE,
        .role = STATION_PLAN_FLOW_ROLE_NONE,
        .peer_type = MODULE_COUNT,
        .commodity = COMMODITY_COUNT,
        .rate = 0.0f,
    };
    if (!st || !station_exists(st) || ring < 1 || ring > STATION_NUM_RINGS)
        return false;

    planned = station_plan_make_module(st, type, ring, slot);
    kind = module_kind(type);
    if (kind == MODULE_KIND_NONE || kind == MODULE_KIND_SERVICE)
        return false;

    candidate_count = station_plan_collect_candidates(
        st, ring, slot, candidates, STATION_PLAN_CANDIDATE_CAP);

    if (kind == MODULE_KIND_STORAGE) {
        const station_module_t *peer = NULL;
        float rate = 0.0f;
        commodity_t tag = station_plan_module_output(&planned);
        if (tag == COMMODITY_COUNT) return false;
        if (!station_plan_best_output_peer(&planned, tag, candidates,
                                           candidate_count, &peer, &rate)) {
            station_plan_consider_hint(out, STATION_FLOW_DIAG_NO_CONSUMER,
                                       STATION_PLAN_FLOW_ROLE_OUTPUT, NULL,
                                       tag, 0.0f);
        } else if (rate <= STATION_FLOW_DIAG_SLOW_RATE) {
            station_plan_consider_hint(out, STATION_FLOW_DIAG_SLOW_FEED,
                                       STATION_PLAN_FLOW_ROLE_OUTPUT, peer,
                                       tag, rate);
        } else {
            station_plan_consider_hint(out, STATION_FLOW_DIAG_RUNNING,
                                       STATION_PLAN_FLOW_ROLE_OUTPUT, peer,
                                       tag, rate);
        }
        return out->diag != STATION_FLOW_DIAG_NONE;
    }

    if (kind == MODULE_KIND_PRODUCER || kind == MODULE_KIND_SHIPYARD) {
        module_inputs_t req = module_instance_required_inputs(&planned);
        bool any_input_source = false;
        for (int i = 0; i < req.count; i++) {
            const station_module_t *peer = NULL;
            float rate = 0.0f;
            commodity_t input = req.commodities[i];
            if (station_plan_best_input_peer(&planned, input, candidates,
                                             candidate_count, &peer, &rate)) {
                any_input_source = true;
                station_plan_consider_hint(
                    out,
                    rate <= STATION_FLOW_DIAG_SLOW_RATE
                        ? STATION_FLOW_DIAG_SLOW_FEED
                        : STATION_FLOW_DIAG_RUNNING,
                    STATION_PLAN_FLOW_ROLE_INPUT, peer, input, rate);
            } else if (!req.any_satisfies) {
                station_plan_consider_hint(out, STATION_FLOW_DIAG_NO_INPUT,
                                           STATION_PLAN_FLOW_ROLE_INPUT, NULL,
                                           input, 0.0f);
                break;
            }
        }
        if (req.any_satisfies && req.count > 0 && !any_input_source) {
            station_plan_consider_hint(out, STATION_FLOW_DIAG_NO_INPUT,
                                       STATION_PLAN_FLOW_ROLE_INPUT, NULL,
                                       req.commodities[0], 0.0f);
        }

        commodity_t output = module_instance_output(&planned);
        if (output != COMMODITY_COUNT) {
            const station_module_t *peer = NULL;
            float rate = 0.0f;
            if (!station_plan_best_output_peer(&planned, output, candidates,
                                               candidate_count, &peer, &rate)) {
                station_plan_consider_hint(out, STATION_FLOW_DIAG_NO_CONSUMER,
                                           STATION_PLAN_FLOW_ROLE_OUTPUT,
                                           NULL, output, 0.0f);
            } else if (rate <= STATION_FLOW_DIAG_SLOW_RATE) {
                station_plan_consider_hint(out, STATION_FLOW_DIAG_SLOW_FEED,
                                           STATION_PLAN_FLOW_ROLE_OUTPUT,
                                           peer, output, rate);
            } else {
                station_plan_consider_hint(out, STATION_FLOW_DIAG_RUNNING,
                                           STATION_PLAN_FLOW_ROLE_OUTPUT,
                                           peer, output, rate);
            }
        }
    }

    return out->diag != STATION_FLOW_DIAG_NONE;
}

bool station_plan_flow_hint_format(const station_plan_flow_hint_t *hint,
                                   char *out, size_t cap)
{
    const char *peer = "module";
    const char *commodity = NULL;
    if (!hint || !out || cap == 0) return false;
    out[0] = '\0';
    if (hint->diag == STATION_FLOW_DIAG_NONE) return false;
    if ((int)hint->peer_type >= 0 && hint->peer_type < MODULE_COUNT)
        peer = module_type_name(hint->peer_type);
    if (hint->commodity >= 0 && hint->commodity < COMMODITY_COUNT)
        commodity = commodity_name(hint->commodity);

    switch (hint->diag) {
    case STATION_FLOW_DIAG_RUNNING:
        if (hint->role == STATION_PLAN_FLOW_ROLE_INPUT)
            snprintf(out, cap, "flow: well-connected input from %s", peer);
        else if (hint->role == STATION_PLAN_FLOW_ROLE_OUTPUT)
            snprintf(out, cap, "flow: well-connected output to %s", peer);
        else
            snprintf(out, cap, "flow: well-connected");
        return true;
    case STATION_FLOW_DIAG_SLOW_FEED:
        if (hint->role == STATION_PLAN_FLOW_ROLE_INPUT)
            snprintf(out, cap, "flow: slow input from %s", peer);
        else
            snprintf(out, cap, "flow: slow route to %s", peer);
        return true;
    case STATION_FLOW_DIAG_NO_INPUT:
        if (commodity)
            snprintf(out, cap, "flow: missing %s input", commodity);
        else
            snprintf(out, cap, "flow: missing input");
        return true;
    case STATION_FLOW_DIAG_NO_CONSUMER:
        if (commodity)
            snprintf(out, cap, "flow: no valid consumer for %s", commodity);
        else
            snprintf(out, cap, "flow: no valid local consumer");
        return true;
    default:
        snprintf(out, cap, "flow: %s", station_flow_diag_label(hint->diag));
        return true;
    }
}

float station_clamp_operator_price(float requested, float baseline)
{
    if (!(requested > 0.0f)) return 0.0f;
    if (!(baseline > 0.0f)) return requested;
    float lo = baseline * 0.5f;
    float hi = baseline * 2.0f;
    if (requested < lo) return lo;
    if (requested > hi) return hi;
    return requested;
}

const char *station_short_name(int station_idx) {
    /* Founding stations have stable, well-known short names that match
     * the in-fiction station identities. Anything beyond the three
     * founders is a player-built outpost — return a generic short tag
     * with the index so distinct outposts read distinctly in the UI.
     *
     * Helper is shared/station_util so both the docked trade UI and any
     * test or server-side code that wants to render lineage can use the
     * same names. The 16-byte static buffer for outposts caps at
     * MAX_STATIONS = 128, so "Outpost 127" still fits. */
    static char outpost_buf[16];
    switch (station_idx) {
    case 0: return "Prospect";
    case 1: return "Kepler";
    case 2: return "Helios";
    case SIGNAL_FREEPORT_STATION_INDEX: return "Freeport";
    default:
        if (station_idx < 0 || station_idx >= MAX_STATIONS) return "?";
        snprintf(outpost_buf, sizeof(outpost_buf), "Outpost %d", station_idx);
        return outpost_buf;
    }
}
