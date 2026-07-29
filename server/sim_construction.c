/*
 * sim_construction.c -- Station construction: module placement, activation,
 * and outpost founding.  Extracted from game_sim.c.
 */
#include "sim_construction.h"
#include "cargo_receipt_trust.h"
#include "chain_log.h"
#include "sim_nav.h"
#include "../shared/manifest.h"

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

/* A station sells scaffolds only if it has installed examples of the
 * requested type (it "knows how to build" that). One active shipyard can
 * bootstrap ship construction and more yard capacity; general station-module
 * scaffolds require two active shipyards. */
bool station_sells_scaffold(const station_t *st, module_type_t type) {
    return station_can_order_scaffold(st, type);
}

static bool construction_cargo_pub_nonzero(const cargo_unit_t *unit) {
    static const uint8_t zero[32] = {0};
    return unit && memcmp(unit->pub, zero, sizeof(zero)) != 0;
}

static bool emit_module_supply_contributions(
    world_t *w, station_t *st, int station_idx, int module_idx,
    const station_module_t *module, commodity_t commodity,
    const cargo_unit_t *units, size_t unit_count,
    float progress_before, float cost) {
    if (!w || !st || !module || !units || unit_count == 0 ||
        unit_count > CHAIN_LOG_BATCH_MAX_EVENTS || cost <= 0.0f) {
        return false;
    }

    chain_payload_construction_t
        payloads[CHAIN_LOG_BATCH_MAX_EVENTS];
    chain_log_batch_event_t
        events[CHAIN_LOG_BATCH_MAX_EVENTS];
    for (size_t i = 0; i < unit_count; i++) {
        if (!construction_cargo_pub_nonzero(&units[i]))
            return false;
        payloads[i] = (chain_payload_construction_t){0};
        chain_payload_construction_t *payload = &payloads[i];
        memcpy(payload->cargo_pub, units[i].pub,
               sizeof(payload->cargo_pub));
        payload->target_kind = CONSTRUCTION_TARGET_MODULE;
        payload->station_index =
            (station_idx >= 0 && station_idx <= 255)
                ? (uint8_t)station_idx : 0xff;
        payload->module_index =
            (module_idx >= 0 && module_idx <= 255)
                ? (uint8_t)module_idx : 0xff;
        payload->module_type = (uint8_t)module->type;
        payload->commodity = (uint8_t)commodity;
        payload->target_id =
            (station_idx >= 0) ? (uint64_t)station_idx : 0u;
        payload->contributed_units = 1.0f;
        payload->progress_after =
            progress_before + (float)(i + 1u) / cost;
        if (payload->progress_after > 1.0f - 0.0001f)
            payload->progress_after = 1.0f;
        events[i] = (chain_log_batch_event_t){
            .type = CHAIN_EVT_CONSTRUCTION,
            .payload = payload,
            .payload_len = (uint16_t)sizeof(*payload),
        };
    }
    chain_log_append_result_t appended =
        chain_log_emit_batch(w, st, events, unit_count);
    return appended.status == CHAIN_LOG_APPEND_OK &&
           appended.event_count == (uint16_t)unit_count;
}

static const cargo_receipt_chain_t *construction_station_chain_at(
    const station_t *station, uint16_t index) {
    const ship_receipts_t *receipts =
        station_get_receipts_const(station);
    if (!receipts || !receipts->chains ||
        index >= receipts->count) return NULL;
    return &receipts->chains[index];
}

static bool construction_chainless_unit_trusted(
    const world_t *w, int station_idx, const cargo_unit_t *unit) {
    if (!w || !unit || station_idx < 0 ||
        station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS ||
        unit->origin_station != (uint8_t)station_idx) {
        return false;
    }
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, station_idx, unit, NULL);
    return evaluated.accepted &&
           evaluated.local_origin_without_receipt &&
           evaluated.origin_station == station_idx;
}

static bool construction_pod_arrived_for_module(
    const world_t *w, const station_t *station,
    int station_idx, int module_idx,
    const cargo_pod_t *pod, commodity_t material) {
    if (!w || !station || !pod ||
        station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= station->module_count ||
        material >= COMMODITY_COUNT ||
        cargo_pod_has_player_tractor(pod)) {
        return false;
    }
    int owner_station = -1;
    int owner_module = -1;
    if (!cargo_pod_module_tractor_indices(
            pod, &owner_station, &owner_module) ||
        owner_station != station_idx ||
        owner_module < 0 ||
        owner_module >= station->module_count ||
        !cargo_pod_module_tractor_arrived(
            w, pod, station_idx, owner_module)) {
        return false;
    }
    const station_module_t *hopper =
        &station->modules[owner_module];
    if (hopper->scaffold ||
        hopper->type != MODULE_HOPPER ||
        (commodity_t)hopper->commodity != material) {
        return false;
    }
    const station_module_t *module =
        &station->modules[module_idx];
    vec2 hopper_pos = module_world_pos_ring(
        station, hopper->ring, hopper->slot);
    vec2 module_pos = module_world_pos_ring(
        station, module->ring, module->slot);
    return v2_dist_sq(hopper_pos, module_pos) <=
        HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
}

/*
 * Consume the trusted suffix of one arrived physical pod as one durable
 * construction transaction. A negative result means a matching pod was
 * staged but its append failed; zero means no eligible physical cargo.
 */
static int consume_trusted_module_supply_pod(
    world_t *w, station_t *station, int station_idx,
    int module_idx, station_module_t *module,
    commodity_t material, float cost, int max_units) {
    if (!w || !station || !module ||
        material >= COMMODITY_COUNT || cost <= 0.0f ||
        max_units <= 0) {
        return 0;
    }
    if (max_units > CHAIN_LOG_BATCH_MAX_EVENTS)
        max_units = CHAIN_LOG_BATCH_MAX_EVENTS;

    for (int pod_idx = 0; pod_idx < MAX_CARGO_PODS; pod_idx++) {
        const cargo_pod_t *pod = &w->cargo_pods[pod_idx];
        if (!cargo_pod_has_exact_manifest(pod, material) ||
            !construction_pod_arrived_for_module(
                w, station, station_idx, module_idx,
                pod, material)) {
            continue;
        }

        cargo_unit_t units[CHAIN_LOG_BATCH_MAX_EVENTS];
        size_t unit_count = 0;
        size_t available = pod->manifest_count;
        if (available > (size_t)max_units)
            available = (size_t)max_units;
        while (unit_count < available) {
            const cargo_unit_t *candidate =
                &pod->manifest_units[
                    pod->manifest_count - 1u - unit_count];
            if (!construction_chainless_unit_trusted(
                    w, station_idx, candidate)) {
                break;
            }
            units[unit_count++] = *candidate;
        }
        if (unit_count == 0) continue;

        cargo_pod_t staged = *pod;
        for (size_t i = 0; i < unit_count; i++) {
            cargo_unit_t removed = {0};
            if (!cargo_pod_take_manifest_unit(
                    &staged, material, &removed) ||
                memcmp(removed.pub, units[i].pub,
                       sizeof(removed.pub)) != 0) {
                return -1;
            }
        }
        float progress_after =
            module->build_progress + (float)unit_count / cost;
        if (progress_after > 1.0f - 0.0001f)
            progress_after = 1.0f;
        if (!emit_module_supply_contributions(
                w, station, station_idx, module_idx,
                module, material, units, unit_count,
                module->build_progress, cost)) {
            return -1;
        }

        w->cargo_pods[pod_idx] = staged;
        if (!staged.active)
            world_cargo_pod_clear_tractor(w, pod_idx);
        module->build_progress = progress_after;
        return (int)unit_count;
    }
    return 0;
}

static int consume_trusted_module_supply_units(
    world_t *w, station_t *station, int station_idx,
    int module_idx, station_module_t *module,
    commodity_t material, float cost, int max_units) {
    if (!w || !station || !module || cost <= 0.0f ||
        max_units <= 0) {
        return 0;
    }
    if (max_units > CHAIN_LOG_BATCH_MAX_EVENTS)
        max_units = CHAIN_LOG_BATCH_MAX_EVENTS;

    cargo_unit_t units[CHAIN_LOG_BATCH_MAX_EVENTS];
    size_t unit_count = 0;
    for (uint16_t i = 0;
         i < station->manifest.count &&
         unit_count < (size_t)max_units;
         i++) {
        const cargo_unit_t *candidate =
            &station->manifest.units[i];
        if (candidate->commodity != (uint8_t)material) continue;
        cargo_receipt_station_evaluation_t evaluated =
            cargo_receipt_evaluate_at_station(
                w, station_idx, candidate,
                construction_station_chain_at(station, i));
        if (!evaluated.accepted) continue;
        units[unit_count++] = *candidate;
    }
    if (unit_count == 0) return 0;

    cargo_store_t staged = {0};
    if (!cargo_store_clone(
            &staged, &station->cargo_store)) return -1;
    for (size_t i = 0; i < unit_count; i++) {
        int selected =
            manifest_find(&staged.manifest, units[i].pub);
        cargo_unit_t removed = {0};
        cargo_receipt_chain_t chain = {0};
        if (selected < 0 ||
            !cargo_store_remove_with_chain(
                &staged, (uint16_t)selected,
                &removed, &chain) ||
            memcmp(removed.pub, units[i].pub,
                   sizeof(removed.pub)) != 0) {
            cargo_store_cleanup(&staged);
            return -1;
        }
    }
    float progress_after =
        module->build_progress + (float)unit_count / cost;
    if (progress_after > 1.0f - 0.0001f)
        progress_after = 1.0f;
    if (!emit_module_supply_contributions(
            w, station, station_idx, module_idx, module,
            material, units, unit_count,
            module->build_progress,
            cost)) {
        cargo_store_cleanup(&staged);
        return -1;
    }
    cargo_store_cleanup(&station->cargo_store);
    station->cargo_store = staged;
    memset(&staged, 0, sizeof(staged));
    station->manifest_dirty = true;
    module->build_progress = progress_after;
    station_finished_sync(station, material);
    return (int)unit_count;
}

/* ------------------------------------------------------------------ */
/* Module placement                                                    */
/* ------------------------------------------------------------------ */

void add_module_at(station_t *st, module_type_t type, uint8_t arm, uint8_t chain_pos) {
    commodity_t commodity = station_default_module_commodity(st, type);
    (void)station_module_append(st, type, arm, chain_pos, false, 1.0f,
                                commodity);
}

/* Override-aware variant for callers that want a specific commodity
 * (tests, or future explicit-commodity build orders). */
void add_hopper_for(station_t *st, uint8_t arm, uint8_t chain_pos, commodity_t c) {
    add_module_at(st, MODULE_HOPPER, arm, chain_pos);
    if (st->module_count > 0) {
        st->modules[st->module_count - 1].commodity = (uint8_t)c;
    }
}

void add_furnace_for(station_t *st, uint8_t arm, uint8_t chain_pos, commodity_t ingot) {
    /* The tag must be one of the smeltable ingot commodities. Anything
     * else (a raw ore, a finished good, COMMODITY_COUNT) would silently
     * fall back to FERRITE_INGOT via module_instance_output() — easy to
     * miss in seed code. Guard at the construction site. */
    if (!(ingot == COMMODITY_FERRITE_INGOT ||
          ingot == COMMODITY_CUPRITE_INGOT ||
          ingot == COMMODITY_CRYSTAL_INGOT)) {
        SIM_LOG("[sim] add_furnace_for: invalid ingot tag %d (defaulting to ferrite)\n", (int)ingot);
        ingot = COMMODITY_FERRITE_INGOT;
    }
    add_module_at(st, MODULE_FURNACE, arm, chain_pos);
    if (st->module_count > 0) {
        st->modules[st->module_count - 1].commodity = (uint8_t)ingot;
    }
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
    /* Per-ring drift bias under the all-passive Slice 1.5a dynamics —
     * a perfectly balanced station still rotates at this rate. Set
     * both inner rings so an outpost growing through ring tiers keeps
     * a sensible rotation without a separate retrigger event. */
    st->arm_speed[0] = STATION_RING_SPEED;
    st->arm_speed[1] = STATION_RING_SPEED;
    /* Bootstrap omega to match the bias so the freshly-activated
     * outpost doesn't show a 1.7s spin-up transient. Mirrors the
     * post-seed bootstrap loop in world_reset. */
    st->arm_omega[0] = STATION_RING_SPEED;
    st->arm_omega[1] = STATION_RING_SPEED;
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

/* Pair-based placement validator.
 *
 * Furnaces are rejected unless a matching ore hopper is already installed
 * on an adjacent ring. Other producers use the station-wide tagged hopper
 * coverage check because their finished-good flow is not a physical smelt
 * beam. Returns true and emits no log if the placement is valid. */
static bool construction_check_placement(const station_t *st,
                                         module_type_t type,
                                         int ring, int slot,
                                         int station_idx) {
    (void)station_idx; /* SIM_LOG compiles out in non-debug builds. */
    station_placement_status_t status = station_placement_validate(
        st, type, ring, slot, STATION_PLACEMENT_PHYSICAL);
    if (status == STATION_PLACEMENT_OK) return true;
    SIM_LOG("[sim] refused %s on station %d ring %d slot %d — %s\n",
            module_type_name(type), station_idx, ring, slot,
            station_placement_status_label(status));
    return false;
}

/* Add a scaffold module to a station and generate a supply contract */
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int arm, int chain_pos) {
    if (!construction_check_placement(st, type, arm, chain_pos, station_idx)) return;

    commodity_t commodity = station_default_module_commodity(st, type);
    station_module_t *m = station_module_append(
        st, type, (uint8_t)arm, (uint8_t)chain_pos, true, 0.0f, commodity);
    if (!m) return;

    /* Generate a supply contract for the required material */
    float cost = module_build_cost(type);
    commodity_t material = module_build_material(type);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (contract_slot_available_for_post(
                &w->contracts[k])) {
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

/* Auto-picker: walk rings high-to-low and find the first free slot whose
 * canonical pair holds the type's required intake (if any). For
 * non-pairing modules the search collapses to "first free slot on the
 * topmost active ring," matching the pre-pair behavior. */
static bool find_paired_free_slot(const station_t *st, module_type_t type,
                                  int *out_ring, int *out_slot) {
    for (int r = STATION_NUM_RINGS; r >= 1; r--) {
        if (!station_has_ring(st, r)) continue;
        int slots = STATION_RING_SLOTS[r];
        for (int s = 0; s < slots; s++) {
            if (station_placement_validate(
                    st, type, r, s, STATION_PLACEMENT_PHYSICAL) !=
                STATION_PLACEMENT_OK) continue;
            *out_ring = r;
            *out_slot = s;
            return true;
        }
    }
    return false;
}

void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    int target_ring = 1, target_slot = -1;
    if (!find_paired_free_slot(st, type, &target_ring, &target_slot)) {
        /* No valid free slot — fall through to begin_module_construction_at
         * with the topmost ring and a free slot, which will log + refuse
         * if pairing is unsatisfied. Keeps the failure path observable
         * rather than silently no-op'ing. */
        for (int r = STATION_NUM_RINGS; r >= 1; r--) {
            if (station_has_ring(st, r)) { target_ring = r; break; }
        }
        target_slot = station_ring_free_slot(st, target_ring, STATION_RING_SLOTS[target_ring]);
        if (target_slot < 0) target_slot = 0xFF;
    }
    begin_module_construction_at(w, st, station_idx, type, target_ring, target_slot);
}

/* ------------------------------------------------------------------ */
/* Module activation timer                                             */
/* ------------------------------------------------------------------ */

/* MODULE_BUILD_TIME_SEC lives in shared/module_schema.h so UI / tests
 * see the same constant without having to import a server header. */

void step_module_activation(world_t *w, float dt) {
    step_station_cargo_pod_tractors(w, 0.0f);
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        /*
         * Construction accepts receipt-backed station rows and physical
         * hopper pods whose exact units resolve against this station's
         * durable local origin. Cross-station or merely stamped loose cargo
         * has no receipt sidecar and remains fail-closed.
         */
        for (int i = 0; i < st->module_count; i++) {
            station_module_t *m = &st->modules[i];
            if (module_build_state(m) != MODULE_BUILD_AWAITING_SUPPLY) continue;
            commodity_t mat = module_build_material(m->type);
            float cost = module_build_cost(m->type);
            float needed = cost * (1.0f - module_supply_fraction(m));
            if (needed < 0.01f) continue;
            int whole = (int)ceilf(needed - 0.0001f);
            while (whole > 0 &&
                   module_build_state(m) ==
                       MODULE_BUILD_AWAITING_SUPPLY) {
                int pod_units =
                    consume_trusted_module_supply_pod(
                        w, st, s, i, m, mat, cost,
                        whole);
                if (pod_units < 0) break;
                if (pod_units > 0) {
                    whole -= pod_units;
                    continue;
                }
                int station_units =
                    consume_trusted_module_supply_units(
                        w, st, s, i, m, mat, cost,
                        whole);
                if (station_units <= 0) {
                    break;
                }
                whole -= station_units;
            }
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
