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
    m->last_smelt_commodity = LAST_SMELT_NONE;
}

void activate_outpost(world_t *w, int station_idx) {
    station_t *st = &w->stations[station_idx];
    st->scaffold = false;
    st->scaffold_progress = 1.0f;
    st->signal_range = OUTPOST_SIGNAL_RANGE;
    /* The signal relay module was placed when the player towed the
     * relay-core seed here in place_towed_scaffold. Activation just
     * promotes it (and the dock + any other founding scaffolds) from
     * pending → built. Fallback: if no relay is present (legacy save
     * or NPC-built outpost), add one so the station still works. */
    bool have_relay = false;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == MODULE_SIGNAL_RELAY) {
            st->modules[m].scaffold = false;
            st->modules[m].build_progress = 1.0f;
            have_relay = true;
        } else if (st->modules[m].type == MODULE_DOCK) {
            st->modules[m].scaffold = false;
            st->modules[m].build_progress = 1.0f;
        }
    }
    if (!have_relay) add_module_at(st, MODULE_SIGNAL_RELAY, 1, 0);
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

/* Per-ring furnace cap: at most one furnace per ring. The count-tier
 * smelt rules want furnaces spread across rings (1/ring → ferrite,
 * 2 rings → cuprite, 3 rings → crystal), so stacking two on the same
 * ring is both useless for tier progression and visually crowded.
 * Counts BUILDING + AWAITING_SUPPLY scaffolds too — placing a second
 * furnace in the same ring while the first is still under construction
 * would slip past a count-only check. */
static bool ring_already_has_furnace(const station_t *st, int ring) {
    if (!st) return false;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].ring != ring) continue;
        if (st->modules[i].type != MODULE_FURNACE) continue;
        return true;
    }
    return false;
}

/* Add a scaffold module to a station and generate a supply contract */
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int arm, int chain_pos) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    if (type == MODULE_FURNACE && ring_already_has_furnace(st, arm)) {
        SIM_LOG("[sim] refused FURNACE on station %d ring %d — already has one\n",
                station_idx, arm);
        return;
    }

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

/* MODULE_BUILD_TIME_SEC lives in shared/module_schema.h so UI / tests
 * see the same constant without having to import a server header. */

void step_module_activation(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        /* Route station inventory to scaffold modules (NPC deliveries) */
        for (int i = 0; i < st->module_count; i++) {
            station_module_t *m = &st->modules[i];
            if (module_build_state(m) != MODULE_BUILD_AWAITING_SUPPLY) continue;
            commodity_t mat = module_build_material(m->type);
            if (st->_inventory_cache[mat] < 0.01f) continue;
            float cost = module_build_cost(m->type);
            float needed = cost * (1.0f - module_supply_fraction(m));
            if (needed < 0.01f) continue;
            float deliver = fminf(st->_inventory_cache[mat], needed);
            st->_inventory_cache[mat] -= deliver;
            m->build_progress += deliver / cost;
            if (m->build_progress > 1.0f) m->build_progress = 1.0f;
        }
        /* Activate fully-supplied scaffold modules after build timer.
         * Modules do NOT tick while their station is itself still under
         * construction — the station has to be born first. */
        if (st->scaffold) continue;
        for (int i = 0; i < st->module_count; i++) {
            station_module_t *m = &st->modules[i];
            if (module_build_state(m) != MODULE_BUILD_BUILDING) continue;
            /* Internal: build_progress in [1.0, 2.0] is the timer phase. */
            m->build_progress += dt / MODULE_BUILD_TIME_SEC;
            if (m->build_progress >= 2.0f) {
                m->scaffold = false;
                m->build_progress = 1.0f;
                rebuild_station_services(st);
                rebuild_signal_chain(w);
                if (st->modules[i].type == MODULE_FURNACE)
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
                        if (module_build_state(&st->modules[j])
                            != MODULE_BUILD_AWAITING_SUPPLY) continue;
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
