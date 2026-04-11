/*
 * sim_construction.c -- Station construction: module placement, activation,
 * and outpost founding.  Extracted from game_sim.c.
 */
#include "sim_construction.h"
#include "sim_ai.h"
#include "sim_nav.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* What material each module requires for construction */
commodity_t module_build_material(module_type_t type) {
    return module_schema(type)->build_commodity;
}

/* Module construction cost (material quantity to manufacture scaffold) */
float module_build_cost(module_type_t type) {
    return module_schema(type)->build_material;
}

/* A station sells scaffolds only if it has a SHIPYARD module AND an
 * installed example of the requested type (it "knows how to build" that). */
bool station_sells_scaffold(const station_t *st, module_type_t type) {
    if (!station_has_module(st, MODULE_SHIPYARD)) return false;
    return station_has_module(st, type);
}

/* ------------------------------------------------------------------ */
/* Module placement                                                    */
/* ------------------------------------------------------------------ */

void add_module_at(station_t *st, module_type_t type, uint8_t arm, uint8_t chain_pos) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    station_module_t *m = &st->modules[st->module_count++];
    m->type = type;
    m->ring = arm;
    m->slot = chain_pos;
    m->scaffold = false;
    m->build_progress = 1.0f;
}

void activate_outpost(world_t *w, int station_idx) {
    station_t *st = &w->stations[station_idx];
    st->scaffold = false;
    st->scaffold_progress = 1.0f;
    st->signal_range = OUTPOST_SIGNAL_RANGE;
    /* First module: signal relay on ring 1, orbiting the placement point */
    add_module_at(st, MODULE_SIGNAL_RELAY, 1, 0);
    st->arm_count = 1;
    st->arm_speed[0] = STATION_RING_SPEED;
    rebuild_station_services(st);
    rebuild_signal_chain(w);
    /* Count connected stations for milestone tracking */
    int connected = 0;
    for (int s = 0; s < MAX_STATIONS; s++)
        if (station_is_active(&w->stations[s]) && w->stations[s].signal_connected)
            connected++;

    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_OUTPOST_ACTIVATED,
        .outpost_activated = { .slot = station_idx },
    });
    if (connected >= 5) {
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_STATION_CONNECTED,
            .station_connected = { .connected_count = connected },
        });
    }
    SIM_LOG("[sim] outpost %d activated (signal_range=%.0f)\n", station_idx, OUTPOST_SIGNAL_RANGE);
}

/* Add a scaffold module to a station and generate a supply contract */
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int arm, int chain_pos) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;

    station_module_t *m = &st->modules[st->module_count++];
    m->type = type;
    m->ring = (uint8_t)arm;
    m->slot = (uint8_t)chain_pos;
    m->scaffold = true;
    m->build_progress = 0.0f;

    /* Generate a supply contract for the required material */
    float cost = module_build_cost(type);
    commodity_t material = module_build_material(type);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active) {
            w->contracts[k] = (contract_t){
                .active = true, .action = CONTRACT_TRACTOR,
                .station_index = (uint8_t)station_idx,
                .commodity = material,
                .quantity_needed = cost,
                .base_price = st->base_price[material] * 1.15f, .age = 0.0f,
                .target_index = -1, .claimed_by = -1,
            };
            break;
        }
    }
    SIM_LOG("[sim] began construction of module %d at station %d ring %d slot %d\n",
            type, station_idx, arm, chain_pos);
}

void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    int target_ring = 1;
    for (int r = STATION_NUM_RINGS; r >= 1; r--) {
        if (station_has_ring(st, r)) { target_ring = r; break; }
    }
    int target_slot = station_ring_free_slot(st, target_ring, STATION_RING_SLOTS[target_ring]);
    if (target_slot < 0) target_slot = 0xFF; /* ring module or full */
    begin_module_construction_at(w, st, station_idx, type, target_ring, target_slot);
}

/* ------------------------------------------------------------------ */
/* Module activation timer                                             */
/* ------------------------------------------------------------------ */

static const float MODULE_BUILD_TIME = 10.0f;  /* seconds after full delivery */

void step_module_activation(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        /* Route station inventory to scaffold modules (NPC deliveries) */
        for (int i = 0; i < st->module_count; i++) {
            if (!st->modules[i].scaffold) continue;
            if (st->modules[i].build_progress >= 1.0f) continue;
            commodity_t mat = module_build_material(st->modules[i].type);
            if (st->inventory[mat] < 0.01f) continue;
            float cost = module_build_cost(st->modules[i].type);
            float needed = cost - st->modules[i].build_progress * cost;
            if (needed < 0.01f) continue;
            float deliver = fminf(st->inventory[mat], needed);
            st->inventory[mat] -= deliver;
            st->modules[i].build_progress += deliver / cost;
            if (st->modules[i].build_progress > 1.0f)
                st->modules[i].build_progress = 1.0f;
        }
        /* Activate fully-supplied scaffold modules after build timer.
         * Modules do NOT tick while their station is itself still under
         * construction — the station has to be born first. */
        if (st->scaffold) continue;
        for (int i = 0; i < st->module_count; i++) {
            if (!st->modules[i].scaffold) continue;
            if (st->modules[i].build_progress < 1.0f) continue; /* not fully supplied */
            /* Count down build time using build_progress > 1.0 as timer.
             * 1.0 = just finished delivery, 2.0 = construction complete. */
            st->modules[i].build_progress += dt / MODULE_BUILD_TIME;
            if (st->modules[i].build_progress >= 2.0f) {
                st->modules[i].scaffold = false;
                st->modules[i].build_progress = 1.0f;
                rebuild_station_services(st);
                rebuild_signal_chain(w);
                if (st->modules[i].type == MODULE_FURNACE || st->modules[i].type == MODULE_FURNACE_CU || st->modules[i].type == MODULE_FURNACE_CR)
                    spawn_npc(w, s, NPC_ROLE_MINER);
                if (st->modules[i].type == MODULE_FRAME_PRESS || st->modules[i].type == MODULE_LASER_FAB || st->modules[i].type == MODULE_TRACTOR_FAB)
                    spawn_npc(w, s, NPC_ROLE_HAULER);
                if (st->modules[i].type == MODULE_SHIPYARD)
                    spawn_npc(w, s, NPC_ROLE_TOW);
                /* Close any construction supply contracts that targeted
                 * this module's build material, unless another scaffold
                 * at this station still needs it. */
                commodity_t mat = module_build_material(st->modules[i].type);
                for (int k = 0; k < MAX_CONTRACTS; k++) {
                    if (!w->contracts[k].active) continue;
                    if (w->contracts[k].action != CONTRACT_TRACTOR) continue;
                    if (w->contracts[k].station_index != s) continue;
                    if (w->contracts[k].commodity != mat) continue;
                    bool still_needed = false;
                    for (int j = 0; j < st->module_count; j++) {
                        if (j == i) continue;
                        if (!st->modules[j].scaffold) continue;
                        if (st->modules[j].build_progress >= 1.0f) continue;
                        if (module_build_material(st->modules[j].type) == mat) {
                            still_needed = true; break;
                        }
                    }
                    if (!still_needed) {
                        w->contracts[k].active = false;
                        emit_event(w, (sim_event_t){
                            .type = SIM_EVENT_CONTRACT_COMPLETE,
                            .contract_complete.action = CONTRACT_TRACTOR,
                        });
                    }
                }
                emit_event(w, (sim_event_t){
                    .type = SIM_EVENT_MODULE_ACTIVATED,
                    .module_activated = { .station = s, .module_idx = i, .module_type = (int)st->modules[i].type },
                });
                SIM_LOG("[sim] module %d activated at station %d\n", st->modules[i].type, s);
                /* Rebuild nav mesh — station geometry changed. */
                station_build_nav(w, s);
            }
        }
    }
}
