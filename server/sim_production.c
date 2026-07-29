/*
 * sim_production.c -- Material flow, smelting, and station production.
 * Extracted from game_sim.c.
 */
#include "sim_production.h"
#include "tractor.h"
#include "laser.h"
#include "sim_asteroid.h"      /* fracture_claim_state_reset */
#include "sim_construction.h"  /* module_build_material, module_build_cost */
#include "actor_principal_resolver.h"
#include "manifest.h"
#include "cargo_receipt_trust.h"
#include "mining.h"            /* grade roll at smelt time */
#include "fixpoint.h"
#include "chain_log.h"         /* signed event emission (#479 C) */
#include "ship_birth_reservation.h"
#include <stdlib.h>            /* abs */
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Smelting helpers                                                    */
/* ------------------------------------------------------------------ */

static bool fragment_pub_is_zero(const asteroid_t *a) {
    static const uint8_t zero[32] = {0};
    return !a || memcmp(a->fragment_pub, zero, sizeof(zero)) == 0;
}

static void smelt_fragment_pub_compat(asteroid_t *a) {
    uint8_t zero_pub[32] = {0};
    if (!a || !fragment_pub_is_zero(a)) return;
    mining_fragment_pub_compute(a->fracture_seed, zero_pub, 0, a->fragment_pub);
}

/* fracture_claim_state_clear was a local duplicate of
 * fracture_claim_state_reset — now sourced from sim_asteroid.h so
 * the single source of truth covers both the birth and smelt-done paths. */

static int connected_player_by_token(const world_t *w, const uint8_t token[8]) {
    static const uint8_t zero_token[8] = {0};
    if (!w || !token || memcmp(token, zero_token, sizeof(zero_token)) == 0)
        return -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!server_player_is_gameplay_ready(&w->players[i])) continue;
        if (memcmp(w->players[i].session_token, token, 8) == 0)
            return i;
    }
    return -1;
}

static void smelt_public_player_label(const server_player_t *player,
                                      char out[12]) {
    actor_principal_t principal = actor_principal_none();
    if (out &&
        actor_principal_from_verified_player(player, &principal) &&
        principal.kind == (uint8_t)ACTOR_PRINCIPAL_PLAYER) {
        mining_callsign_from_pubkey(principal.id, out);
        return;
    }
    if (out) snprintf(out, 12, "%s", "anonymous");
}

static const cargo_receipt_chain_t *production_station_chain_at(
    const station_t *station, uint16_t index) {
    const ship_receipts_t *receipts =
        station_get_receipts_const(station);
    if (!receipts || !receipts->chains ||
        index >= receipts->count) return NULL;
    return &receipts->chains[index];
}

static bool production_station_unit_trusted(
    const world_t *w, int station_idx, uint16_t index) {
    if (!w || station_idx < 0 ||
        station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS) return false;
    const station_t *station = &w->stations[station_idx];
    if (!station->manifest.units ||
        index >= station->manifest.count) return false;
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, station_idx, &station->manifest.units[index],
            production_station_chain_at(station, index));
    return evaluated.accepted;
}

/*
 * Physical pods intentionally do not carry receipt sidecars.  They can only
 * participate in a provenance-sensitive local production transaction when
 * the exact unit resolves against the receiving station's own durable
 * SMELT/CRAFT history.  Cross-station physical cargo must first acquire a
 * receipt-backed manifest row; origin_station alone is never proof.
 */
static bool production_chainless_unit_trusted(
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

static bool production_chainless_pod_trusted(
    const world_t *w, int station_idx, const cargo_pod_t *pod) {
    if (!pod || !cargo_pod_has_exact_manifest(pod, pod->commodity))
        return false;
    if (!cargo_pod_custody_charge_anchor_valid(pod) ||
        pod->custody_charge_total > 0) {
        return false;
    }
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if (!production_chainless_unit_trusted(
                w, station_idx, &pod->manifest_units[i])) {
            return false;
        }
    }
    if (pod->has_shell_frame &&
        !production_chainless_unit_trusted(
            w, station_idx, &pod->shell_frame)) {
        return false;
    }
    return true;
}

enum { PRODUCTION_STAGED_POD_CAP = RECIPE_INPUT_MAX + 3 };

typedef struct {
    int pod_idx;
    cargo_pod_t pod;
} production_staged_pod_t;

typedef struct {
    production_staged_pod_t pods[PRODUCTION_STAGED_POD_CAP];
    size_t count;
} production_pod_plan_t;

static cargo_pod_t *production_stage_pod(
    production_pod_plan_t *plan, const world_t *w, int pod_idx) {
    if (!plan || !w || pod_idx < 0 || pod_idx >= MAX_CARGO_PODS)
        return NULL;
    for (size_t i = 0; i < plan->count; i++) {
        if (plan->pods[i].pod_idx == pod_idx)
            return &plan->pods[i].pod;
    }
    if (plan->count >= PRODUCTION_STAGED_POD_CAP) return NULL;
    production_staged_pod_t *staged = &plan->pods[plan->count++];
    staged->pod_idx = pod_idx;
    staged->pod = w->cargo_pods[pod_idx];
    return &staged->pod;
}

static void production_commit_pod_plan(
    world_t *w, const production_pod_plan_t *plan) {
    if (!w || !plan) return;
    for (size_t i = 0; i < plan->count; i++) {
        int pod_idx = plan->pods[i].pod_idx;
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) continue;
        w->cargo_pods[pod_idx] = plan->pods[i].pod;
    }
}

static chain_log_append_result_t station_emit_craft_events(
    world_t *w, station_t *station, recipe_id_t recipe_id,
    const cargo_unit_t *inputs, size_t input_count,
    const cargo_unit_t *products, size_t product_count) {
    chain_log_append_result_t rejected = {
        .status = CHAIN_LOG_APPEND_BAD_ARGUMENTS,
    };
    if (!w || !station || !inputs || !products ||
        input_count == 0 || input_count > RECIPE_INPUT_MAX ||
        product_count == 0 ||
        product_count > CHAIN_LOG_BATCH_MAX_EVENTS ||
        product_count > CARGO_POD_MANIFEST_CAP) {
        return rejected;
    }
    chain_payload_craft_t payloads[CARGO_POD_MANIFEST_CAP];
    chain_log_batch_event_t events[CARGO_POD_MANIFEST_CAP];
    memset(payloads, 0, sizeof(payloads));
    memset(events, 0, sizeof(events));
    for (size_t p = 0; p < product_count; p++) {
        if (products[p].recipe_id != (uint16_t)recipe_id ||
            !chain_payload_craft_bind_output(
                &payloads[p], inputs, input_count, &products[p])) {
            return rejected;
        }
        events[p] = (chain_log_batch_event_t){
            .type = CHAIN_EVT_CRAFT,
            .payload = &payloads[p],
            .payload_len = (uint16_t)sizeof(payloads[p]),
        };
    }
    return chain_log_emit_batch(
        w, station, events, product_count);
}

static bool manifest_unit_matches_recipe_input(const cargo_unit_t *unit,
                                               commodity_t commodity) {
    cargo_kind_t kind;
    if (!cargo_kind_for_commodity(commodity, &kind)) return false;
    return unit != NULL &&
           (cargo_kind_t)unit->kind == kind &&
           (commodity_t)unit->commodity == commodity;
}

static bool station_manifest_select_recipe_inputs(
                                                  const world_t *w,
                                                  int station_idx,
                                                  const station_t *st,
                                                  const recipe_def_t *recipe,
                                                  uint16_t out_indices[RECIPE_INPUT_MAX],
                                                  cargo_unit_t out_inputs[RECIPE_INPUT_MAX]) {
    if (!st || !recipe || !out_indices || !out_inputs ||
        !st->manifest.units || recipe->input_count == 0 ||
        recipe->input_count > RECIPE_INPUT_MAX) {
        return false;
    }

    for (size_t want = 0; want < recipe->input_count; want++) {
        bool found = false;
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            bool already_selected = false;
            for (size_t prev = 0; prev < want; prev++) {
                if (out_indices[prev] == i) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) continue;
            if (!manifest_unit_matches_recipe_input(&st->manifest.units[i],
                                                    recipe->input_commodities[want])) {
                continue;
            }
            if (!production_station_unit_trusted(
                    w, station_idx, i)) continue;
            out_indices[want] = i;
            out_inputs[want] = st->manifest.units[i];
            found = true;
            break;
        }
        if (!found) return false;
    }

    return true;
}

typedef struct {
    int pod_idx;
    commodity_t commodity;
    cargo_unit_t unit;
} loose_pod_recipe_input_t;

static int selected_count_for_pod(
    const loose_pod_recipe_input_t *selected,
    size_t selected_count, int pod_idx) {
    int count = 0;
    if (!selected || pod_idx < 0) return 0;
    for (size_t i = 0; i < selected_count; i++) {
        if (selected[i].pod_idx == pod_idx) count++;
    }
    return count;
}

static bool production_pod_staged_at_matching_hopper(
    const world_t *w, const station_t *st,
    int station_idx, int module_idx,
    const cargo_pod_t *pod, commodity_t commodity) {
    if (!w || !st || !pod || commodity >= COMMODITY_COUNT ||
        module_idx < 0 || module_idx >= st->module_count) {
        return false;
    }
    const station_module_t *module = &st->modules[module_idx];
    if (module->scaffold) return false;
    vec2 module_pos = module_world_pos_ring(
        st, module->ring, module->slot);
    const float consumer_range_sq =
        HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;

    if (cargo_pod_is_tractored_by_module(
            pod, station_idx, module_idx)) {
        int hopper_idx = station_find_hopper_for(st, commodity);
        if (hopper_idx >= 0 && hopper_idx < st->module_count) {
            const station_module_t *hopper =
                &st->modules[hopper_idx];
            if (!hopper->scaffold &&
                hopper->type == MODULE_HOPPER &&
                (commodity_t)hopper->commodity == commodity) {
                vec2 hopper_pos = module_world_pos_ring(
                    st, hopper->ring, hopper->slot);
                if (v2_dist_sq(hopper_pos, module_pos) <=
                        consumer_range_sq &&
                    cargo_pod_module_tractor_arrived(
                        w, pod, station_idx, module_idx)) {
                    return true;
                }
            }
        }
    }

    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold ||
            hopper->type != MODULE_HOPPER ||
            (commodity_t)hopper->commodity != commodity ||
            !cargo_pod_is_tractored_by_module(
                pod, station_idx, i)) {
            continue;
        }
        vec2 hopper_pos = module_world_pos_ring(
            st, hopper->ring, hopper->slot);
        if (v2_dist_sq(hopper_pos, module_pos) >
            consumer_range_sq) {
            continue;
        }
        if (cargo_pod_module_tractor_arrived(
                w, pod, station_idx, i)) {
            return true;
        }
    }
    return false;
}

static bool station_select_loose_pod_recipe_inputs(
    const world_t *w, const station_t *st,
    int station_idx, int module_idx,
    const recipe_def_t *recipe,
    loose_pod_recipe_input_t selected[RECIPE_INPUT_MAX],
    cargo_unit_t inputs[RECIPE_INPUT_MAX]) {
    if (!w || !st || !recipe || !selected || !inputs ||
        module_idx < 0 || module_idx >= st->module_count ||
        recipe->input_count == 0 ||
        recipe->input_count > RECIPE_INPUT_MAX) {
        return false;
    }
    memset(selected, 0,
           sizeof(loose_pod_recipe_input_t) * RECIPE_INPUT_MAX);
    memset(inputs, 0,
           sizeof(cargo_unit_t) * RECIPE_INPUT_MAX);

    for (size_t want = 0; want < recipe->input_count; want++) {
        commodity_t commodity =
            recipe->input_commodities[want];
        bool found = false;
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            const cargo_pod_t *pod = &w->cargo_pods[i];
            if (!cargo_pod_has_exact_manifest(
                    pod, commodity) ||
                cargo_pod_has_player_tractor(pod) ||
                !production_pod_staged_at_matching_hopper(
                    w, st, station_idx, module_idx,
                    pod, commodity)) {
                continue;
            }
            int already = selected_count_for_pod(
                selected, want, i);
            if (already < 0 ||
                already >= (int)pod->manifest_count) {
                continue;
            }
            uint16_t unit_idx =
                (uint16_t)(pod->manifest_count - 1u -
                           (uint16_t)already);
            cargo_unit_t unit =
                pod->manifest_units[unit_idx];
            if (!manifest_unit_matches_recipe_input(
                    &unit, commodity) ||
                !production_chainless_unit_trusted(
                    w, station_idx, &unit)) {
                continue;
            }
            selected[want] = (loose_pod_recipe_input_t){
                .pod_idx = i,
                .commodity = commodity,
                .unit = unit,
            };
            inputs[want] = unit;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

static bool station_loose_pod_recipe_inputs_available(
    const world_t *w, const station_t *st,
    int station_idx, int module_idx,
    const recipe_def_t *recipe) {
    loose_pod_recipe_input_t selected[RECIPE_INPUT_MAX];
    cargo_unit_t inputs[RECIPE_INPUT_MAX];
    return station_select_loose_pod_recipe_inputs(
        w, st, station_idx, module_idx,
        recipe, selected, inputs);
}

static bool production_pod_can_accept_output(const cargo_pod_t *pod,
                                             commodity_t commodity,
                                             int product_count) {
    if (!pod || product_count <= 0 || commodity >= COMMODITY_COUNT)
        return false;
    if (!cargo_pod_custody_charge_anchor_valid(pod) ||
        pod->custody_charge_total > 0) {
        return false;
    }
    if (!cargo_pod_has_exact_manifest(pod, commodity))
        return false;
    if (pod->manifest_count > CARGO_POD_UNIT_CAPACITY)
        return false;
    return (int)pod->manifest_count + product_count <= CARGO_POD_UNIT_CAPACITY;
}

static int station_find_output_pod_for_module(world_t *w,
                                              const station_t *st,
                                              int station_idx,
                                              int module_idx,
                                              commodity_t commodity,
                                              int product_count) {
    if (!w || !st || station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= st->module_count ||
        commodity >= COMMODITY_COUNT || product_count <= 0) {
        return -1;
    }

    const station_module_t *module = &st->modules[module_idx];
    int output_hopper = station_find_output_hopper_for_module(st, module);
    vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
    const float staged_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    int best_idx = -1;
    float best_d = staged_sq;

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!production_pod_can_accept_output(pod, commodity, product_count))
            continue;
        if (!production_chainless_pod_trusted(
                w, station_idx, pod))
            continue;
        if (cargo_pod_has_player_tractor(pod)) continue;
        if (cargo_pod_is_tractored_by_module(pod, station_idx, module_idx) ||
            (output_hopper >= 0 &&
             cargo_pod_is_tractored_by_module(pod, station_idx,
                                               output_hopper)))
            return i;
        if (cargo_pod_has_module_tractor(pod)) continue;

        float d = v2_dist_sq(pod->pos, module_pos);
        if (d <= best_d) {
            best_d = d;
            best_idx = i;
        }
    }
    return best_idx;
}

typedef struct {
    int pod_idx;
    cargo_unit_t unit;
} production_loose_shell_t;

static bool production_select_loose_shell(
    const world_t *w, int station_idx, int module_idx,
    production_loose_shell_t *out) {
    if (!w || !out || station_idx < 0 ||
        station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS ||
        module_idx < 0 ||
        module_idx >= w->stations[station_idx].module_count) {
        return false;
    }
    const station_t *station = &w->stations[station_idx];
    const station_module_t *target =
        &station->modules[module_idx];
    vec2 target_pos = module_world_pos_ring(
        station, target->ring, target->slot);
    const float shell_lane_sq =
        HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(
                pod, COMMODITY_FRAME) ||
            cargo_pod_has_player_tractor(pod)) {
            continue;
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
            continue;
        }
        const station_module_t *owner =
            &station->modules[owner_module];
        bool owner_can_supply =
            owner_module == module_idx ||
            (owner->type == MODULE_HOPPER &&
             (commodity_t)owner->commodity ==
                 COMMODITY_FRAME);
        if (!owner_can_supply) {
            const module_schema_t *schema =
                module_schema(owner->type);
            owner_can_supply =
                schema &&
                schema->kind == MODULE_KIND_PRODUCER;
        }
        if (!owner_can_supply) continue;
        vec2 owner_pos = module_world_pos_ring(
            station, owner->ring, owner->slot);
        if (v2_dist_sq(owner_pos, target_pos) >
            shell_lane_sq) {
            continue;
        }
        const cargo_unit_t *unit =
            &pod->manifest_units[pod->manifest_count - 1u];
        if (!production_chainless_unit_trusted(
                w, station_idx, unit)) {
            continue;
        }
        *out = (production_loose_shell_t){
            .pod_idx = i,
            .unit = *unit,
        };
        return true;
    }
    return false;
}

static bool production_stage_take_selected_unit(
    production_pod_plan_t *plan, const world_t *w,
    int pod_idx, commodity_t commodity,
    const cargo_unit_t *expected,
    cargo_unit_t *out_unit) {
    if (!plan || !w || !expected || !out_unit)
        return false;
    cargo_pod_t *pod =
        production_stage_pod(plan, w, pod_idx);
    if (!pod) return false;
    cargo_unit_t removed = {0};
    if (!cargo_pod_take_manifest_unit(
            pod, commodity, &removed) ||
        memcmp(removed.pub, expected->pub,
               sizeof(removed.pub)) != 0) {
        return false;
    }
    *out_unit = removed;
    return true;
}

static int production_find_free_pod_slot(const world_t *w) {
    if (!w) return -1;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!w->cargo_pods[i].active) return i;
    }
    return -1;
}

static int station_manifest_craft_product_pod_batch(world_t *w,
                                                    int station_idx,
                                                    int module_idx,
                                                    recipe_id_t recipe_id) {
    station_t *st;
    const recipe_def_t *recipe = recipe_get(recipe_id);
    uint16_t indices[RECIPE_INPUT_MAX] = {0};
    cargo_unit_t inputs[RECIPE_INPUT_MAX] = {{0}};

    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= MAX_MODULES_PER_STATION) {
        return 0;
    }
    st = &w->stations[station_idx];
    if (module_idx >= st->module_count) return 0;
    if (!recipe || recipe->input_count == 0 ||
        recipe->input_count > RECIPE_INPUT_MAX)
        return 0;
    if (!st->manifest.units ||
        !cargo_store_receipts_const(&st->cargo_store))
        return 0;
    if (!station_manifest_select_recipe_inputs(
            w, station_idx, st, recipe, indices, inputs)) {
        return 0;
    }
    int output_count = recipe->output_count > 0
        ? (int)recipe->output_count : 1;
    if (output_count <= 0 ||
        output_count > CARGO_POD_MANIFEST_CAP ||
        output_count > CHAIN_LOG_BATCH_MAX_EVENTS) {
        return 0;
    }
    cargo_unit_t products[CARGO_POD_MANIFEST_CAP] = {{0}};
    for (int i = 0; i < output_count; i++) {
        if (!hash_product(
                recipe_id, inputs, recipe->input_count,
                (uint16_t)i, &products[i])) {
            return 0;
        }
        products[i].origin_station = (uint8_t)station_idx;
    }

    cargo_store_t staged_station = {0};
    if (!cargo_store_clone(
            &staged_station, &st->cargo_store)) return 0;
    uint16_t sorted[RECIPE_INPUT_MAX] = {0};
    for (size_t i = 0; i < recipe->input_count; i++)
        sorted[i] = indices[i];
    for (size_t i = 0; i < recipe->input_count; i++) {
        for (size_t j = i + 1;
             j < recipe->input_count; j++) {
            if (sorted[j] > sorted[i]) {
                uint16_t swap = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = swap;
            }
        }
    }
    for (size_t i = 0; i < recipe->input_count; i++) {
        cargo_unit_t removed = {0};
        cargo_receipt_chain_t removed_chain = {0};
        if (!cargo_store_remove_with_chain(
                &staged_station, sorted[i],
                &removed, &removed_chain)) {
            cargo_store_cleanup(&staged_station);
            return 0;
        }
    }

    int pod_idx = station_find_output_pod_for_module(
        w, st, station_idx, module_idx,
        recipe->output_commodity, output_count);
    bool new_pod = pod_idx < 0;
    cargo_pod_t staged_pod = {0};
    production_pod_plan_t shell_plan = {0};
    production_loose_shell_t loose_shell = {0};
    bool loose_shell_used = false;
    if (!new_pod) {
        staged_pod = w->cargo_pods[pod_idx];
        if (!production_pod_can_accept_output(
                &staged_pod, recipe->output_commodity,
                output_count)) {
            cargo_store_cleanup(&staged_station);
            return 0;
        }
        for (int i = 0; i < output_count; i++) {
            staged_pod.manifest_units[
                staged_pod.manifest_count++] = products[i];
        }
        staged_pod.quantity = staged_pod.manifest_count;
        staged_pod.age = 0.0f;
        cargo_pod_set_station_custody(
            &staged_pod, station_idx);
    } else {
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            if (!w->cargo_pods[i].active) {
                pod_idx = i;
                break;
            }
        }
        if (pod_idx < 0) {
            cargo_store_cleanup(&staged_station);
            return 0;
        }
        cargo_unit_t shell = {0};
        if (production_select_loose_shell(
                w, station_idx, module_idx,
                &loose_shell)) {
            if (!production_stage_take_selected_unit(
                    &shell_plan, w, loose_shell.pod_idx,
                    COMMODITY_FRAME, &loose_shell.unit,
                    &shell)) {
                cargo_store_cleanup(&staged_station);
                return 0;
            }
            loose_shell_used = true;
        } else {
            int shell_actual = -1;
            for (uint16_t i = 0;
                 i < st->manifest.count; i++) {
                bool selected_input = false;
                for (size_t input = 0;
                     input < recipe->input_count;
                     input++) {
                    if (indices[input] == i) {
                        selected_input = true;
                        break;
                    }
                }
                if (selected_input ||
                    st->manifest.units[i].commodity !=
                        (uint8_t)COMMODITY_FRAME ||
                    !production_station_unit_trusted(
                        w, station_idx, i)) {
                    continue;
                }
                shell_actual = (int)i;
                break;
            }
            if (shell_actual < 0) {
                cargo_store_cleanup(&staged_station);
                return 0;
            }
            int staged_shell = manifest_find(
                &staged_station.manifest,
                st->manifest.units[shell_actual].pub);
            if (staged_shell < 0 ||
                !cargo_store_remove_with_chain(
                    &staged_station,
                    (uint16_t)staged_shell,
                    &shell, NULL)) {
                cargo_store_cleanup(&staged_station);
                return 0;
            }
        }
        const station_module_t *module =
            &st->modules[module_idx];
        vec2 module_pos = module_world_pos_ring(
            st, module->ring, module->slot);
        float angle = module_angle_ring(
            st, module->ring, module->slot);
        vec2 dir = v2_from_angle(angle);
        const float pod_radius = 18.0f;
        const float mouth_offset =
            STATION_MODULE_COL_RADIUS + pod_radius + 8.0f;
        staged_pod.active = true;
        staged_pod.kind = CARGO_POD_CARGO;
        staged_pod.commodity = recipe->output_commodity;
        staged_pod.quantity = (uint16_t)output_count;
        staged_pod.manifest_count = (uint16_t)output_count;
        memcpy(staged_pod.manifest_units, products,
               (size_t)output_count * sizeof(products[0]));
        staged_pod.pos = v2_add(
            module_pos, v2_scale(dir, mouth_offset));
        staged_pod.vel = station_ring_point_velocity(
            st, module->ring, staged_pod.pos);
        staged_pod.radius = pod_radius;
        staged_pod.rotation = angle;
        staged_pod.spin = 0.22f;
        cargo_pod_set_shell_frame(&staged_pod, &shell);
        cargo_pod_set_station_custody(
            &staged_pod, station_idx);
    }

    chain_log_append_result_t appended =
        station_emit_craft_events(
            w, st, recipe_id, inputs,
            recipe->input_count, products,
            (size_t)output_count);
    if (appended.status != CHAIN_LOG_APPEND_OK) {
        cargo_store_cleanup(&staged_station);
        return 0;
    }

    cargo_store_cleanup(&st->cargo_store);
    st->cargo_store = staged_station;
    memset(&staged_station, 0, sizeof(staged_station));
    st->manifest_dirty = true;
    if (loose_shell_used) {
        production_commit_pod_plan(w, &shell_plan);
        if (loose_shell.pod_idx >= 0 &&
            loose_shell.pod_idx < MAX_CARGO_PODS &&
            !w->cargo_pods[loose_shell.pod_idx].active) {
            world_cargo_pod_clear_tractor(
                w, loose_shell.pod_idx);
        }
    }
    w->cargo_pods[pod_idx] = staged_pod;
    if (new_pod) world_cargo_pod_clear_tractor(w, pod_idx);
    const station_module_t *module = &st->modules[module_idx];
    int output_hopper =
        station_find_output_hopper_for_module(st, module);
    (void)world_cargo_pod_set_module_tractor(
        w, pod_idx, station_idx,
        output_hopper >= 0 ? output_hopper : module_idx);
    for (size_t i = 0; i < recipe->input_count; i++) {
        station_finished_sync(
            st, recipe->input_commodities[i]);
    }
    if (new_pod)
        station_finished_sync(st, COMMODITY_FRAME);
    return output_count;
}

static int station_loose_pod_craft_product_pod_batch(
    world_t *w, int station_idx, int module_idx,
    recipe_id_t recipe_id) {
    if (!w || station_idx < 0 ||
        station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS) {
        return 0;
    }
    station_t *st = &w->stations[station_idx];
    const recipe_def_t *recipe = recipe_get(recipe_id);
    if (!recipe || recipe->input_count == 0 ||
        recipe->input_count > RECIPE_INPUT_MAX ||
        recipe->output_count == 0 ||
        recipe->output_count > CARGO_POD_MANIFEST_CAP ||
        recipe->output_count > CHAIN_LOG_BATCH_MAX_EVENTS ||
        module_idx < 0 || module_idx >= st->module_count) {
        return 0;
    }

    loose_pod_recipe_input_t
        selected[RECIPE_INPUT_MAX] = {{0}};
    cargo_unit_t inputs[RECIPE_INPUT_MAX] = {{0}};
    if (!station_select_loose_pod_recipe_inputs(
            w, st, station_idx, module_idx,
            recipe, selected, inputs)) {
        return 0;
    }

    int product_count = (int)recipe->output_count;
    cargo_unit_t products[CARGO_POD_MANIFEST_CAP] = {{0}};
    for (int i = 0; i < product_count; i++) {
        if (!hash_product(
                recipe_id, inputs, recipe->input_count,
                (uint16_t)i, &products[i])) {
            return 0;
        }
        products[i].origin_station = (uint8_t)station_idx;
    }

    production_pod_plan_t pod_plan = {0};
    for (size_t i = 0; i < recipe->input_count; i++) {
        cargo_unit_t removed = {0};
        if (!production_stage_take_selected_unit(
                &pod_plan, w, selected[i].pod_idx,
                selected[i].commodity, &selected[i].unit,
                &removed)) {
            return 0;
        }
    }

    int output_pod_idx = station_find_output_pod_for_module(
        w, st, station_idx, module_idx,
        recipe->output_commodity, product_count);
    bool new_output_pod = output_pod_idx < 0;
    cargo_store_t staged_station = {0};
    bool staged_station_live = false;

    if (!new_output_pod) {
        cargo_pod_t *output = production_stage_pod(
            &pod_plan, w, output_pod_idx);
        if (!output ||
            !production_pod_can_accept_output(
                output, recipe->output_commodity,
                product_count)) {
            return 0;
        }
        for (int i = 0; i < product_count; i++) {
            output->manifest_units[
                output->manifest_count++] = products[i];
        }
        output->quantity = output->manifest_count;
        output->age = 0.0f;
        cargo_pod_set_station_custody(
            output, station_idx);
    } else {
        output_pod_idx = production_find_free_pod_slot(w);
        if (output_pod_idx < 0) return 0;

        cargo_unit_t shell = {0};
        production_loose_shell_t loose_shell = {0};
        if (production_select_loose_shell(
                w, station_idx, module_idx,
                &loose_shell)) {
            if (!production_stage_take_selected_unit(
                    &pod_plan, w, loose_shell.pod_idx,
                    COMMODITY_FRAME, &loose_shell.unit,
                    &shell)) {
                return 0;
            }
        } else {
            int shell_idx = -1;
            for (uint16_t i = 0; i < st->manifest.count; i++) {
                if (st->manifest.units[i].commodity ==
                        (uint8_t)COMMODITY_FRAME &&
                    production_station_unit_trusted(
                        w, station_idx, i)) {
                    shell_idx = (int)i;
                    break;
                }
            }
            if (shell_idx < 0 ||
                !cargo_store_clone(
                    &staged_station, &st->cargo_store)) {
                return 0;
            }
            staged_station_live = true;
            if (!cargo_store_remove_with_chain(
                    &staged_station, (uint16_t)shell_idx,
                    &shell, NULL)) {
                cargo_store_cleanup(&staged_station);
                return 0;
            }
        }

        cargo_pod_t *output = production_stage_pod(
            &pod_plan, w, output_pod_idx);
        if (!output) {
            if (staged_station_live)
                cargo_store_cleanup(&staged_station);
            return 0;
        }
        memset(output, 0, sizeof(*output));
        output->active = true;
        output->kind = CARGO_POD_CARGO;
        output->commodity = recipe->output_commodity;
        output->quantity = (uint16_t)product_count;
        output->manifest_count = (uint16_t)product_count;
        memcpy(output->manifest_units, products,
               (size_t)product_count * sizeof(products[0]));
        const station_module_t *module =
            &st->modules[module_idx];
        vec2 module_pos = module_world_pos_ring(
            st, module->ring, module->slot);
        float angle = module_angle_ring(
            st, module->ring, module->slot);
        vec2 dir = v2_from_angle(angle);
        const float pod_radius = 18.0f;
        output->pos = v2_add(
            module_pos,
            v2_scale(dir, STATION_MODULE_COL_RADIUS +
                          pod_radius + 8.0f));
        output->vel = station_ring_point_velocity(
            st, module->ring, output->pos);
        output->radius = pod_radius;
        output->rotation = angle;
        output->spin = 0.22f;
        cargo_pod_set_shell_frame(output, &shell);
        cargo_pod_set_station_custody(
            output, station_idx);
    }

    chain_log_append_result_t appended =
        station_emit_craft_events(
            w, st, recipe_id, inputs,
            recipe->input_count, products,
            (size_t)product_count);
    if (appended.status != CHAIN_LOG_APPEND_OK) {
        if (staged_station_live)
            cargo_store_cleanup(&staged_station);
        return 0;
    }

    if (staged_station_live) {
        cargo_store_cleanup(&st->cargo_store);
        st->cargo_store = staged_station;
        memset(&staged_station, 0,
               sizeof(staged_station));
        st->manifest_dirty = true;
        station_finished_sync(
            st, COMMODITY_FRAME);
    }
    production_commit_pod_plan(w, &pod_plan);
    for (size_t i = 0; i < recipe->input_count; i++) {
        int pod_idx = selected[i].pod_idx;
        if (pod_idx >= 0 && pod_idx < MAX_CARGO_PODS &&
            !w->cargo_pods[pod_idx].active) {
            world_cargo_pod_clear_tractor(w, pod_idx);
        }
    }
    const station_module_t *module =
        &st->modules[module_idx];
    int output_hopper =
        station_find_output_hopper_for_module(st, module);
    (void)world_cargo_pod_set_module_tractor(
        w, output_pod_idx, station_idx,
        output_hopper >= 0
            ? output_hopper : module_idx);
    return product_count;
}

/* Tagged furnace/pair capability lives in shared/station_util.c
 * (`station_can_smelt`) so the client-side dock UI can use the same
 * predicate. The sim wrapper here just keeps the existing public symbol
 * for game_sim.c callers. */
bool sim_can_smelt_ore(const station_t *st, commodity_t ore) {
    return station_can_smelt(st, ore);
}


/* ------------------------------------------------------------------ */
/* Refinery production                                                 */
/* ------------------------------------------------------------------ */

/* Retired refinery-hopper smelting.
 *
 * Raw ore no longer enters station inventory in normal play. Players and
 * NPC miners tow physical fragments, and `step_furnace_smelting` is the
 * only path that turns ore into ingot manifests and EVT_SMELT chain
 * entries. Keeping this symbol wired into the tick preserves call-site
 * shape and save compatibility for `_inventory_cache[ORE]`, but it must
 * not mint zero-lineage ingots from anonymous bulk floats.
 */
void sim_step_refinery_production(world_t *w, float dt) {
    (void)w;
    (void)dt;
}

/* ------------------------------------------------------------------ */
/* Station production (frame press, laser fab, tractor fab)            */
/* Uses module input buffers from the flow graph — placement matters.  */
/* Fabs still accept legacy inventory-backed inputs during the          */
/* transition, but finished batches eject as physical cargo pods.       */
/* ------------------------------------------------------------------ */

void sim_step_station_production(world_t *w, float dt) {
    step_station_cargo_pod_tractors(w, 0.0f);
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;

        for (int m = 0; m < st->module_count; m++) {
            module_type_t mt = st->modules[m].type;
            producer_recipe_t recipe;
            if (st->modules[m].scaffold) continue;

            const module_schema_t *schema = module_schema(mt);
            if (schema->kind != MODULE_KIND_PRODUCER) continue;
            /* Furnaces handled separately in sim_step_refinery_production */
            if (mt == MODULE_FURNACE) continue;
            if (!producer_recipe_for_module(mt, &recipe)) continue;
            const recipe_def_t *recipe_def = recipe_get(recipe.recipe_id);
            if (!recipe_def) continue;

            commodity_t input_com = recipe.primary_input;
            commodity_t output_com = recipe.output;
            if (input_com >= COMMODITY_COUNT || output_com >= COMMODITY_COUNT) continue;
            float rate = schema->rate > 0.0f ? schema->rate : STATION_PRODUCTION_RATE;
            bool pod_ready =
                station_loose_pod_recipe_inputs_available(
                    w, st, s, m, recipe_def);
            bool secondary_ready = true;
            if (recipe.secondary_input < COMMODITY_COUNT) {
                secondary_ready =
                    station_inventory_amount(st, recipe.secondary_input) + FLOAT_EPSILON >=
                    recipe.secondary_units_per_batch;
            }
            bool buffer_ready =
                st->modules[m].input_buffer + FLOAT_EPSILON >= recipe.primary_units_per_batch &&
                secondary_ready;
            bool inventory_ready =
                station_inventory_amount(st, input_com) + FLOAT_EPSILON >=
                    recipe.primary_units_per_batch &&
                secondary_ready;
            if (!pod_ready && !buffer_ready &&
                !inventory_ready) {
                continue;
            }

            st->modules[m].craft_progress += rate * dt;
            if (st->modules[m].craft_progress > 4.0f)
                st->modules[m].craft_progress = 4.0f;

            float produced = 0.0f;
            while (st->modules[m].craft_progress + FLOAT_EPSILON >= 1.0f) {
                if (station_loose_pod_recipe_inputs_available(
                        w, st, s, m, recipe_def)) {
                    int crafted =
                        station_loose_pod_craft_product_pod_batch(
                            w, s, m, recipe.recipe_id);
                    if (crafted <= 0) {
                        st->modules[m].craft_progress = 1.0f;
                        break;
                    }
                    produced += (float)crafted;
                    st->modules[m].craft_progress -= 1.0f;
                    continue;
                }
                bool from_buffer =
                    st->modules[m].input_buffer + FLOAT_EPSILON >= recipe.primary_units_per_batch;
                if (!from_buffer &&
                    station_inventory_amount(st, input_com) + FLOAT_EPSILON <
                    recipe.primary_units_per_batch) {
                    break;
                }
                if (recipe.secondary_input < COMMODITY_COUNT &&
                    station_inventory_amount(st, recipe.secondary_input) + FLOAT_EPSILON <
                    recipe.secondary_units_per_batch) {
                    break;
                }

                int crafted = station_manifest_craft_product_pod_batch(
                    w, s, m, recipe.recipe_id);
                if (crafted <= 0) {
                    st->modules[m].craft_progress = 1.0f;
                    break;
                }

                if (from_buffer) {
                    st->modules[m].input_buffer -= recipe.primary_units_per_batch;
                    if (st->modules[m].input_buffer < 0.0f) st->modules[m].input_buffer = 0.0f;
                }
                /* The manifest craft transaction consumed the selected cargo
                 * units. Module buffers are routing reservations, not another
                 * inventory authority. */
                produced += (float)crafted;
                st->modules[m].craft_progress -= 1.0f;
            }

            if (produced > 0.0f) {
                st->modules[m].active_pulse = 1.0f;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Furnace smelting (fragment hopper pull + smelt)                     */
/* ------------------------------------------------------------------ */

static bool crystal_stage_is_intermediate(const asteroid_t *a) {
    return a && a->commodity == COMMODITY_CRYSTAL_ORE &&
           a->crystal_stage == CRYSTAL_STAGE_INTERMEDIATE;
}

static bool crystal_stage_source_matches(const asteroid_t *a,
                                         int station_idx,
                                         int module_idx) {
    if (!crystal_stage_is_intermediate(a)) return false;
    if (a->crystal_stage_station == 0xFFu ||
        a->crystal_stage_module == 0xFFu) {
        return false;
    }
    return a->crystal_stage_station == (uint8_t)station_idx &&
           a->crystal_stage_module == (uint8_t)module_idx;
}

static void crystal_fragment_make_intermediate(asteroid_t *a,
                                               int station_idx,
                                               int module_idx,
                                               vec2 midpoint,
                                               const station_t *st) {
    if (!a) return;
    a->crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    a->crystal_stage_station = (uint8_t)station_idx;
    a->crystal_stage_module = (uint8_t)module_idx;
    a->smelt_progress = 0.0f;
    a->net_dirty = true;

    vec2 away = v2_sub(a->pos, midpoint);
    float len = v2_len(away);
    if (len < 1.0f && st)
        away = v2_sub(midpoint, st->pos);
    len = v2_len(away);
    if (len < 1.0f)
        away = v2(1.0f, 0.0f);
    else
        away = v2_scale(away, 1.0f / len);
    a->pos = v2_add(a->pos, v2_scale(away, 36.0f));
    a->vel = v2_add(a->vel, v2_scale(away, 90.0f));
}

void step_furnace_smelting(world_t *w, float dt) {
    step_station_cargo_pod_tractors(w, 0.0f);
    float pull_range = HOPPER_PULL_RANGE;
    float pull_sq = pull_range * pull_range;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        if (world_ship_birth_fragment_reserved(w, i)) continue;

        int smelt_station = -1;
        int smelt_module = -1;
        vec2 smelt_midpoint = a->pos;
        bool smelted = false;

        for (int s = 0; s < MAX_STATIONS && !smelted; s++) {
            station_t *st = &w->stations[s];
            if (st->scaffold) continue;

            /* Station-level capability gate: a station only advertises
             * full smelt capability when it has matching tagged
             * furnace+hoppers. Crystal requires two such pairs because
             * stage one leaves a physical intermediate fragment. */
            if (!sim_can_smelt_ore(st, a->commodity)) continue;

            /* Find furnace+target pairs: only a furnace tagged for this ore
             * can anchor the beam, paired with the nearest matching hopper
             * on an adjacent ring. */
            for (int m = 0; m < st->module_count && !smelted; m++) {
                if (st->modules[m].scaffold) continue;
                if (st->modules[m].type != MODULE_FURNACE) continue;
                if (module_instance_input_ore(&st->modules[m]) != a->commodity) continue;
                if (crystal_stage_source_matches(a, s, m)) continue;

                int ring = st->modules[m].ring;
                vec2 furnace_pos = module_world_pos_ring(st, ring, st->modules[m].slot);

                /* Find nearest matching ORE-tagged hopper on an adjacent
                 * ring. Filtering by commodity matters now that stations
                 * carry both input AND output hoppers (Slice 1) — without
                 * the filter, a ferrite-furnace's silo could anchor on a
                 * neighboring FERRITE_INGOT output hopper at certain
                 * angles, shifting the smelt midpoint and the visible
                 * beam endpoint to the wrong hopper. */
                vec2 silo_pos = furnace_pos;
                int silo_ring = ring;
                int silo_module = -1;
                bool has_silo = false;
                float best_d = 1e18f;
                int adj_rings[] = { ring + 1, ring - 1 };
                for (int ri = 0; ri < 2; ri++) {
                    int adj = adj_rings[ri];
                    if (adj < 1 || adj > STATION_NUM_RINGS) continue;
                    for (int m2 = 0; m2 < st->module_count; m2++) {
                        if (st->modules[m2].ring != adj) continue;
                        if (st->modules[m2].scaffold) continue;
                        if (st->modules[m2].type != MODULE_HOPPER) continue;
                        if ((commodity_t)st->modules[m2].commodity != a->commodity) continue;
                        vec2 mp2 = module_world_pos_ring(st, adj, st->modules[m2].slot);
                        float dd = v2_dist_sq(furnace_pos, mp2);
                        if (dd < best_d) {
                            best_d = dd;
                            silo_pos = mp2;
                            silo_ring = adj;
                            silo_module = m2;
                            has_silo = true;
                        }
                    }
                }
                if (!has_silo) continue;

                /* Check if BOTH furnace and silo can reach this fragment */
                float d_furnace_sq = v2_dist_sq(a->pos, furnace_pos);
                float d_silo_sq = v2_dist_sq(a->pos, silo_pos);
                if (d_furnace_sq > pull_sq && d_silo_sq > pull_sq) continue;
                bool furnace_reach = (d_furnace_sq <= pull_sq);
                bool silo_reach = (d_silo_sq <= pull_sq);
                if (!furnace_reach || !silo_reach) continue;  /* both must reach */

                /* The furnace and its ore hopper are two real tractor
                 * sources. Apply exactly the two links that are published to
                 * clients so the visible field and authoritative momentum
                 * cannot disagree. Their equal springs naturally converge
                 * on the midpoint used by the smelting corridor below. */
                vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);
                vec2 beam_velocities[2] = {
                    module_world_velocity_ring(st, ring,
                                               st->modules[m].slot),
                    station_ring_point_velocity(st, silo_ring, silo_pos),
                };
                const int beam_modules[2] = {m, silo_module};
                const vec2 beam_sources[2] = {furnace_pos, silo_pos};
                for (int beam_idx = 0; beam_idx < 2; beam_idx++) {
                    tractor_link_t link = {
                        .source = {
                            .pos = beam_sources[beam_idx],
                            .vel = &beam_velocities[beam_idx],
                            .inv_mass = 0.0f,
                        },
                        .target = {
                            .pos = a->pos,
                            .vel = &a->vel,
                            .inv_mass = 1.0f,
                        },
                        .beam = tractor_tow_beam(HOPPER_PULL_RANGE, 0.0f),
                    };
                    (void)tractor_link_apply(&link, dt);
                    /* Station tractors continuously bend this fragment away
                     * from the ordinary drag-only dead-reckoning model.
                     * Keep the motion stream hot while the force is active
                     * so clients reconcile the acceleration at tow cadence. */
                    a->net_dirty = true;
                    float intensity = tractor_beam_range_fraction(
                        beam_sources[beam_idx], a->pos, &link.beam);
                    if (intensity <= 0.0f || beam_modules[beam_idx] < 0)
                        continue;
                    vec2 emitter = beam_sources[beam_idx];
                    (void)station_module_tractor_emitter(
                        w, s, beam_modules[beam_idx], a->pos, &emitter);
                    sim_emit_interaction(w, (sim_interaction_t){
                        .type = SIM_INTERACTION_TRACTOR_BEAM,
                        .visual =
                            SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR,
                        .commodity = (uint8_t)a->commodity,
                        .source = {
                            .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
                            .index = (int16_t)s,
                            .aux = (int16_t)beam_modules[beam_idx],
                        },
                        .target = {
                            .type = SIM_INTERACTION_ENTITY_ASTEROID,
                            .index = (int16_t)i,
                            .aux = -1,
                        },
                        .source_pos = emitter,
                        .target_pos = a->pos,
                        .range = HOPPER_PULL_RANGE,
                        .intensity = intensity,
                    });
                }

                /* Pulse the furnace module — the existing ring-spoke
                 * physics in step_station_ring_dynamics looks at
                 * station_module_t.active_pulse to scale spoke torque. Without
                 * this, an active smelt beam wouldn't drive any ring
                 * rotation. Also tag the ore for the dynamic furnace
                 * glow; the retired hopper-float path used to be the
                 * only writer for this field. */
                st->modules[m].last_smelt_commodity = (uint8_t)a->commodity;
                st->modules[m].active_pulse = 1.0f;

                float d_mid = v2_len(v2_sub(a->pos, midpoint));
                /* Smelt once the fragment is in the central beam corridor.
                 * The both-module reach gate above keeps this local to the
                 * furnace/hopper pair; the wider midpoint radius lets player
                 * and NPC tow bands finish deliveries instead of holding ore
                 * just outside the old 80u threshold forever. */
                if (d_mid < HOPPER_PULL_RANGE * 0.5f) {
                    smelt_station = s;
                    smelt_module = m;
                    smelt_midpoint = midpoint;
                    smelted = true;
                }
            }
        }

        /* Smelt-progress laser-effect: in beam → accumulate at 0.5/sec
         * (~2s to fully smelt); not in beam → decay at 0.5/sec back to 0.
         * Clamped to [0, 1] so a fragment waiting on a fracture-claim
         * resolution doesn't grow progress unbounded. */
        const float SMELT_RATE = 0.5f;
        if (!smelted) {
            laser_apply_effect(&a->smelt_progress, -SMELT_RATE, 1.0f, dt);
            continue;
        }
        laser_apply_effect(&a->smelt_progress, +SMELT_RATE, 1.0f, dt);

        /* Hold fragment in place while smelting — dampen velocity */
        a->vel = v2_scale(a->vel, 1.0f / (1.0f + 10.0f * dt));

        if (a->smelt_progress >= 1.0f && smelt_station >= 0) {
            station_t *st = &w->stations[smelt_station];
            fracture_claim_state_t *claim_state = &w->fracture_claims[i];
            /* L11: fill legacy fragment_pub up-front so the compat shim is
             * visible at the top of the block regardless of code flow. */
            if (fragment_pub_is_zero(a))
                smelt_fragment_pub_compat(a);
            /* Wait out active, unresolved fracture claims before paying out. */
            if (claim_state->active && !claim_state->resolved) continue;

            if (a->commodity == COMMODITY_CRYSTAL_ORE &&
                a->crystal_stage != CRYSTAL_STAGE_INTERMEDIATE) {
                crystal_fragment_make_intermediate(a, smelt_station,
                                                   smelt_module,
                                                   smelt_midpoint, st);
                SIM_LOG("[smelt] station %d staged crystal fragment via module %d\n",
                        smelt_station, smelt_module);
                continue;
            }

            /* M5 backpressure removed: it stuck fragments on the beam
             * forever when the station's output bin filled up (e.g.
             * Prospect with no local ferrite consumer). UX bias is to
             * clear the player's tractor. Output no longer lands in a
             * bin; a real frame shell is consumed before the pod exists. */

            /* Smelt: ore -> a physical ingot pod in space. Station
             * storage is not the sink anymore; the pod's embedded manifest
             * is the exact identity store for downstream sale,
             * construction, and delivery. */
            mining_grade_t grade = (mining_grade_t)a->grade;
            commodity_t output = commodity_refined_form(a->commodity);
            if (output == a->commodity) output = a->commodity;
            int manifest_units = (int)floorf(a->ore + 0.0001f);
            if (manifest_units > CARGO_POD_MANIFEST_CAP)
                manifest_units = CARGO_POD_MANIFEST_CAP;

            cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
            memset(units, 0, sizeof(units));
            int pushed = 0;
            int first_named_idx = -1;
            for (int idx = 0; idx < manifest_units; idx++) {
                cargo_unit_t unit = {0};
                if (!hash_ingot(output, grade, a->fragment_pub,
                                (uint16_t)idx, &unit)) {
                    continue;
                }
                unit.origin_station = (uint8_t)smelt_station;
                unit.mined_block = (uint64_t)(w->time * 120.0);
                units[pushed] = unit;
                if (first_named_idx < 0 &&
                    (ingot_prefix_t)unit.prefix_class != INGOT_PREFIX_ANONYMOUS) {
                    first_named_idx = pushed;
                }
                pushed++;
            }

            int pod_idx = -1;
            cargo_unit_t pod_shell = {0};
            if (pushed <= 0) continue;
            if (pushed > 0) {
                pod_idx = production_find_free_pod_slot(w);
                if (pod_idx < 0) {
                    SIM_LOG("[smelt] station %d waiting for frame shell for %s pod\n",
                            smelt_station, commodity_short_name(output));
                    continue;
                }

                cargo_store_t staged_station = {0};
                bool staged_station_live = false;
                production_pod_plan_t shell_plan = {0};
                production_loose_shell_t loose_shell = {0};
                if (production_select_loose_shell(
                        w, smelt_station, smelt_module,
                        &loose_shell)) {
                    if (!production_stage_take_selected_unit(
                            &shell_plan, w,
                            loose_shell.pod_idx,
                            COMMODITY_FRAME,
                            &loose_shell.unit,
                            &pod_shell)) {
                        continue;
                    }
                } else {
                    int shell_idx = -1;
                    for (uint16_t shell = 0;
                         shell < st->manifest.count; shell++) {
                        if (st->manifest.units[shell].commodity !=
                                (uint8_t)COMMODITY_FRAME ||
                            !production_station_unit_trusted(
                                w, smelt_station, shell)) {
                            continue;
                        }
                        shell_idx = (int)shell;
                        break;
                    }
                    if (shell_idx < 0 ||
                        !cargo_store_clone(
                            &staged_station,
                            &st->cargo_store)) {
                        SIM_LOG("[smelt] station %d waiting for trusted frame shell for %s pod\n",
                                smelt_station,
                                commodity_short_name(output));
                        continue;
                    }
                    staged_station_live = true;
                    if (!cargo_store_remove_with_chain(
                            &staged_station,
                            (uint16_t)shell_idx,
                            &pod_shell, NULL)) {
                        cargo_store_cleanup(
                            &staged_station);
                        continue;
                    }
                }

                const station_module_t *furnace =
                    &st->modules[smelt_module];
                vec2 dir = v2_sub(smelt_midpoint, st->pos);
                float dir_len = v2_len(dir);
                if (dir_len > 0.001f)
                    dir = v2_scale(dir, 1.0f / dir_len);
                else
                    dir = v2_from_angle((float)smelt_module * 0.731f);
                vec2 pod_pos = smelt_midpoint;
                vec2 pod_vel = station_ring_point_velocity(
                    st, furnace->ring, pod_pos);
                float rotation = fixp_atan2f(dir.y, dir.x);
                cargo_pod_t staged_pod = {0};
                staged_pod.active = true;
                staged_pod.kind = CARGO_POD_CARGO;
                staged_pod.commodity = output;
                staged_pod.quantity = (uint16_t)pushed;
                staged_pod.manifest_count = (uint16_t)pushed;
                memcpy(staged_pod.manifest_units, units,
                       (size_t)pushed * sizeof(units[0]));
                staged_pod.pos = pod_pos;
                staged_pod.vel = pod_vel;
                staged_pod.radius = 18.0f;
                staged_pod.rotation = rotation;
                staged_pod.spin = 0.18f;
                cargo_pod_set_shell_frame(
                    &staged_pod, &pod_shell);
                cargo_pod_set_station_custody(
                    &staged_pod, smelt_station);

                chain_payload_smelt_t
                    payloads[CARGO_POD_MANIFEST_CAP];
                chain_log_batch_event_t
                    events[CARGO_POD_MANIFEST_CAP];
                memset(payloads, 0, sizeof(payloads));
                memset(events, 0, sizeof(events));
                bool payloads_ready = true;
                for (int unit_idx = 0;
                     unit_idx < pushed; unit_idx++) {
                    if (!chain_payload_smelt_bind_output(
                            &payloads[unit_idx],
                            a->fragment_pub,
                            (uint16_t)unit_idx,
                            &units[unit_idx])) {
                        payloads_ready = false;
                        break;
                    }
                    events[unit_idx] =
                        (chain_log_batch_event_t){
                            .type = CHAIN_EVT_SMELT,
                            .payload = &payloads[unit_idx],
                            .payload_len =
                                (uint16_t)sizeof(payloads[unit_idx]),
                        };
                }
                if (!payloads_ready) {
                    if (staged_station_live)
                        cargo_store_cleanup(&staged_station);
                    continue;
                }
                chain_log_append_result_t appended =
                    chain_log_emit_batch(
                        w, st, events, (size_t)pushed);
                if (appended.status != CHAIN_LOG_APPEND_OK) {
                    if (staged_station_live)
                        cargo_store_cleanup(
                            &staged_station);
                    continue;
                }

                if (staged_station_live) {
                    cargo_store_cleanup(&st->cargo_store);
                    st->cargo_store = staged_station;
                    memset(&staged_station, 0,
                           sizeof(staged_station));
                    st->manifest_dirty = true;
                    station_finished_sync(
                        st, COMMODITY_FRAME);
                } else {
                    production_commit_pod_plan(
                        w, &shell_plan);
                    if (loose_shell.pod_idx >= 0 &&
                        loose_shell.pod_idx <
                            MAX_CARGO_PODS &&
                        !w->cargo_pods[
                            loose_shell.pod_idx].active) {
                        world_cargo_pod_clear_tractor(
                            w, loose_shell.pod_idx);
                    }
                }
                w->cargo_pods[pod_idx] = staged_pod;
                world_cargo_pod_clear_tractor(w, pod_idx);
                int output_hopper = station_find_hopper_for(st, output);
                (void)world_cargo_pod_set_module_tractor(
                    w, pod_idx, smelt_station,
                    output_hopper >= 0 ? output_hopper : smelt_module);
                st->modules[smelt_module].active_pulse = 1.0f;
            }

            /* M3: scan all matching contracts (was break-on-first). Pick the
             * contract whose price is highest above the station buy price. */
            float price = station_buy_price(st, a->commodity);
            bool by_contract = false;
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) continue;
                if (w->contracts[k].action != CONTRACT_TRACTOR) continue;
                if (w->contracts[k].station_index != smelt_station) continue;
                if (w->contracts[k].commodity != a->commodity) continue;
                float cp = contract_price(&w->contracts[k]);
                if (cp > price) { price = cp; by_contract = true; }
            }
            float ore_value = a->ore * price;

            /* Prefix-class price multipliers (#prefix-pricing): project
             * the to-be-minted ingot units and lift ore_value by the
             * mean prefix multiplier across them. Anonymous-class units
             * (the bulk of mining output) have multiplier 1.0×, so the
             * historical balance is unchanged in expectation; the rare
             * M/RATi-class fragment lifts the whole payout.
             *
             * The minted-unit pubkeys are deterministic in
             * (output, grade, fragment_pub, idx) — see hash_ingot — so
             * projecting here picks the same prefixes the manifest push
             * below will mint. We project against the OUTPUT ingot
             * commodity, not the raw ore, because the lineage substrate
             * tags ingots, not ore.
             *
             * Skipped on contract payouts — by_contract paths pay the
             * flat per-unit contract_price by design (see #496 / spec
             * out-of-scope: filtered contracts come in a follow-up PR). */
            if (!by_contract) {
                if (output != a->commodity && pushed > 0) {
                    float sum_mult = 0.0f;
                    int counted = 0;
                    for (int idx = 0; idx < pushed; idx++) {
                        sum_mult += prefix_class_price_multiplier(
                                        (int)units[idx].prefix_class);
                        counted++;
                    }
                    if (counted > 0)
                        ore_value *= (sum_mult / (float)counted);
                }
            }

            /* Credit fracturer and tower.
             *
             * Credit attribution is strictly token-based: we look up each
             * role's session_token against currently-connected sessions.
             * The legacy `last_towed_by` / `last_fractured_by` slot
             * fallback was REMOVED — it could pay the wrong player when a
             * slot gets reused on disconnect/rejoin. Do not restore it
             * without a token-match guard.
             *
             * Known gap: fragments in saves from before the token fields
             * existed, or fragments towed/fractured before the player's
             * session was `session_ready` (zero token at tow time), will
             * now smelt with no credit recipient. The ore still becomes
             * a loose ingot pod as infrastructure value; the embedded pod
             * manifest still records the hash. If that's not acceptable
             * for in-the-wild saves, add a one-time migration that
             * stamps a synthetic-but-valid owner token on pre-token
             * fragments at save-load time — do NOT re-enable the slot
             * fallback. */
            int tower = connected_player_by_token(w, a->last_towed_token);
            int fracturer = connected_player_by_token(w, a->last_fractured_token);
            SIM_LOG("[smelt-attr] tower_match=%s fracturer_match=%s\n",
                    tower >= 0 ? "connected" : "unresolved",
                    fracturer >= 0 ? "connected" : "unresolved");

            /* Grade is committed when the fracture claim resolves.
             * Smelt only publishes that cached value — no fresh dice. */
            float bonus_mult = mining_payout_multiplier(grade);
            float graded_value = ore_value * bonus_mult;
            int base_cr  = (int)lroundf(ore_value);
            int bonus_cr = (int)lroundf(graded_value - ore_value);
            int roller = (tower >= 0) ? tower : fracturer;

            /* Announce rare strikes on the station signal channel so
             * other players see them flicker across the Network tab. */
            if (grade >= MINING_GRADE_RATI && roller >= 0) {
                char msg[96];
                char actor_label[12];
                smelt_public_player_label(
                    &w->players[roller], actor_label);
                if (grade == MINING_GRADE_COMMISSIONED)
                    snprintf(msg, sizeof(msg), "%s published commissioned ore  +%d",
                             actor_label, bonus_cr);
                else
                    snprintf(msg, sizeof(msg), "%s published RATi ore  +%d",
                             actor_label, bonus_cr);
                signal_channel_post(w, smelt_station, msg, "");
            }

            if (ore_value > 0.0f) {
                uint8_t bc = by_contract ? 1 : 0;
                /* Credit to the same identity the buy/balance paths read:
                 * pubkey when registered, session_token-pseudokey otherwise.
                 * Mismatched identity here was the visible-bug:
                 * smelt-payouts landed on the session-token ledger entry
                 * but the buy path read the pubkey entry → balance shown
                 * as 0, "REJECT: finished good but whole=0 (afford=0)". */
                if (tower >= 0) {
                    float credited = 0.0f;
                    server_player_t *pt = &w->players[tower];
                    if (pt->session_ready) {
                        credited = server_player_can_use_pubkey_persistence(pt)
                            ? ledger_credit_supply_amount_by_pubkey(st, pt->pubkey, graded_value)
                            : ledger_credit_supply_amount(st, pt->session_token, graded_value);
                    }
                    SIM_LOG("[smelt-pay] player %d tower credit: graded=%.2f credited=%.2f pubkey_ledger=%d session_ready=%d\n",
                            tower, graded_value, credited,
                            server_player_can_use_pubkey_persistence(pt) ? 1 : 0,
                            pt->session_ready ? 1 : 0);
                    pt->ship->stat_credits_earned += credited;
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_SELL, .player_id = tower,
                        .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                  .base_cr = base_cr, .bonus_cr = bonus_cr,
                                  .by_contract = bc }});
                    if (fracturer >= 0 && fracturer != tower) {
                        float finders = graded_value * 0.25f;
                        float fcredited = 0.0f;
                        server_player_t *pf = &w->players[fracturer];
                        if (pf->session_ready) {
                            fcredited = server_player_can_use_pubkey_persistence(pf)
                                ? ledger_credit_supply_amount_by_pubkey(st, pf->pubkey, finders)
                                : ledger_credit_supply_amount(st, pf->session_token, finders);
                        }
                        pf->ship->stat_credits_earned += fcredited;
                        emit_event(w, (sim_event_t){
                            .type = SIM_EVENT_SELL, .player_id = fracturer,
                            .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                      .base_cr = (int)lroundf(finders / bonus_mult),
                                      .bonus_cr = (int)lroundf(finders - finders / bonus_mult),
                                      .by_contract = bc }});
                    }
                } else if (fracturer >= 0) {
                    float half = graded_value * 0.5f;
                    float credited = 0.0f;
                    server_player_t *pf = &w->players[fracturer];
                    if (pf->session_ready) {
                        credited = server_player_can_use_pubkey_persistence(pf)
                            ? ledger_credit_supply_amount_by_pubkey(st, pf->pubkey, half)
                            : ledger_credit_supply_amount(st, pf->session_token, half);
                    }
                    pf->ship->stat_credits_earned += credited;
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_SELL, .player_id = fracturer,
                        .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                  .base_cr = (int)lroundf(half / bonus_mult),
                                  .bonus_cr = (int)lroundf(half - half / bonus_mult),
                                  .by_contract = bc }});
                }
            }

            {
                SIM_LOG("[smelt] station %d %s grade=%d ore=%.2f units=%d pod=%d\n",
                        smelt_station, commodity_short_name(output),
                        (int)grade, a->ore, pushed, pod_idx);

                if (pod_idx >= 0) {
                    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
                    if (first_named_idx >= 0 &&
                        first_named_idx < (int)pod->manifest_count) {
                        cargo_unit_t *u = &pod->manifest_units[first_named_idx];
                        char cs[12];
                        mining_render_callsign(u->pub, cs);
                        char text[96];
                        snprintf(text, sizeof(text), "smelted %s", cs);
                        (void)signal_channel_post(
                            w, smelt_station, text, "");
                    }

                }
            }

            world_asteroid_clear_tractor(w, i);
            clear_asteroid(a);
            fracture_claim_state_reset(claim_state);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Material flow graph (#280)                                          */
/* ------------------------------------------------------------------ */

/* Shortest slot distance on a ring, accounting for wrap-around.
 * e.g. on a 6-slot ring, slot 0 and slot 5 are distance 1, not 5. */
static int ring_slot_distance(int slot_a, int slot_b, int total_slots) {
    int d = abs(slot_a - slot_b);
    if (total_slots > 0 && d > total_slots / 2)
        d = total_slots - d;
    return d > 0 ? d : 1;
}

/* Adjacency-aware transfer rate between two modules on the same station.
 * Same ring: fast when adjacent, drops with slot distance (wrap-aware).
 * Cross-ring: based on angular distance — closer angles = faster beam.
 * Layout shapes throughput: placement matters. */
static float module_flow_rate(const station_t *st, int producer_idx, int consumer_idx) {
    const station_module_t *p = &st->modules[producer_idx];
    const station_module_t *c = &st->modules[consumer_idx];
    if (p->ring == c->ring && p->ring >= 1) {
        int slots = STATION_RING_SLOTS[p->ring];
        int d = ring_slot_distance((int)p->slot, (int)c->slot, slots);
        /* Same ring: 5/sec adjacent, drops with distance along ring */
        return 5.0f / (float)d;
    }
    /* Cross-ring: rate depends on angular proximity.
     * Uses base slot angles (ignoring ring rotation) so the rate is
     * stable — placement matters, not the current rotation phase. */
    if (p->ring >= 1 && c->ring >= 1) {
        float p_angle = TWO_PI_F * (float)p->slot / (float)STATION_RING_SLOTS[p->ring];
        float c_angle = TWO_PI_F * (float)c->slot / (float)STATION_RING_SLOTS[c->ring];
        float da = fabsf(p_angle - c_angle);
        if (da > PI_F) da = TWO_PI_F - da;
        /* da=0 (same angle) → 3.0/sec. da=PI (opposite) → 0.5/sec. */
        float t = da / PI_F;
        return 3.0f - t * 2.5f;
    }
    return 0.5f;
}

/* Match a producer's output commodity against any module's input commodity.
 * Returns true if the consumer should accept this material. */
static bool module_accepts_input(const station_module_t *consumer, commodity_t commodity) {
    const module_schema_t *cs = module_schema(consumer->type);
    /* Producers consume their primary flow input. Multi-input recipes
     * consume secondary ingredients from station inventory/manifest, so
     * the single per-module input buffer must not accept those secondaries. */
    if (cs->kind == MODULE_KIND_PRODUCER) {
        commodity_t input = consumer->type == MODULE_FURNACE
                          ? module_instance_input_ore(consumer)
                          : cs->input;
        if (input == commodity) return true;
    }
    /* Storage modules are commodity-tagged. A hopper tagged for frames
     * must not pull crystal ore just because some other producer wants
     * crystal; that was the source of "irrelevant" module beams. */
    if (cs->kind == MODULE_KIND_STORAGE) {
        if ((commodity_t)consumer->commodity == commodity) return true;
    }
    /* Shipyards accept whatever their pending order needs (handled separately) */
    return false;
}

/* Move material from producers' output buffers into matching consumers'
 * input buffers, prioritizing closer modules. Runs each tick. */
void step_module_flow(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;

        /* For each module with material in its output buffer, find the
         * best consumer for that commodity and transfer. */
        for (int p = 0; p < st->module_count; p++) {
            if (st->modules[p].scaffold) continue;
            commodity_t output = module_schema_output(st->modules[p].type);
            module_kind_t producer_kind = module_kind(st->modules[p].type);
            /* Storage modules pull from station inventory and push into the
             * flow graph for finished goods. Raw-ore hoppers are physical
             * fragment-smelt anchors only; they no longer drain anonymous
             * `_inventory_cache[ORE]` into furnace buffers. */
            if (output == COMMODITY_COUNT) {
                if (producer_kind != MODULE_KIND_STORAGE) continue;

                float cap = module_buffer_capacity(st->modules[p].type);
                if (cap <= 0.0f) continue;

                /* Scan all commodities that downstream modules want.
                 * Pull the first available one from inventory. */
                if (st->modules[p].output_buffer < cap * 0.5f) {
                    /* Check what consumers on this station need */
                    commodity_t tag = (commodity_t)st->modules[p].commodity;
                    if (tag >= COMMODITY_COUNT) continue;
                    if (tag < COMMODITY_RAW_ORE_COUNT) {
                        st->modules[p].output_buffer = 0.0f;
                        continue;
                    }
                    {
                        commodity_t com = tag;
                        float stored = station_inventory_amount(st, com);
                        if (stored <= 0.1f) continue;
                        /* Check if any module on this station actually wants this */
                        bool wanted = false;
                        for (int c = 0; c < st->module_count; c++) {
                            if (c == p || st->modules[c].scaffold) continue;
                            if (module_accepts_input(&st->modules[c], com)) {
                                float c_cap = module_buffer_capacity(st->modules[c].type);
                                if (c_cap > 0.0f && st->modules[c].input_buffer < c_cap) {
                                    wanted = true; break;
                                }
                            }
                        }
                        if (!wanted) continue;
                        float pull = fminf(stored,
                            (cap - st->modules[p].output_buffer) * 0.5f);
                        if (pull > 0.01f) {
                            st->modules[p].output_buffer += pull;
                            output = com; /* remember what we're carrying */
                        }
                    }
                }
                /* Storage output is only a mirror of inventory. If we
                 * can't refresh it with a concrete commodity this tick,
                 * drop any stale residue rather than guessing wrong. */
                if (output == COMMODITY_COUNT) {
                    st->modules[p].output_buffer = 0.0f;
                    continue;
                }
            }
            if (st->modules[p].output_buffer <= 0.0f) continue;

            /* Find the best consumer (closest, has space) */
            int best_consumer = -1;
            float best_rate = 0.0f;
            for (int c = 0; c < st->module_count; c++) {
                if (c == p) continue;
                if (st->modules[c].scaffold) continue;
                if (!module_accepts_input(&st->modules[c], output)) continue;
                float cap = module_buffer_capacity(st->modules[c].type);
                if (cap <= 0.0f) continue;
                if (st->modules[c].input_buffer >= cap) continue;
                float rate = module_flow_rate(st, p, c);
                if (rate > best_rate) {
                    best_rate = rate;
                    best_consumer = c;
                }
            }
            if (best_consumer < 0) continue;

            float room = module_buffer_capacity(st->modules[best_consumer].type)
                       - st->modules[best_consumer].input_buffer;
            float pull = best_rate * dt;
            if (pull > st->modules[p].output_buffer) pull = st->modules[p].output_buffer;
            if (producer_kind == MODULE_KIND_STORAGE) {
                float stored = station_inventory_amount(st, output);
                if (pull > stored) pull = stored;
            }
            if (pull > room) pull = room;
            if (pull > 0.0f) {
                st->modules[p].output_buffer -= pull;
                st->modules[best_consumer].input_buffer += pull;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Module delivery (docked ship -> scaffold)                           */
/* ------------------------------------------------------------------ */

static const cargo_receipt_chain_t *production_ship_receipt_chain_at(
    const ship_t *ship, uint16_t index) {
    const ship_receipts_t *receipts = ship_get_receipts_const(ship);
    if (!receipts || !receipts->chains || index >= receipts->count)
        return NULL;
    return &receipts->chains[index];
}

static bool ship_manifest_unit_trusted_at_station(
    const world_t *w, const ship_t *ship, uint16_t index, int station_idx) {
    if (!w || !ship || !ship->manifest.units ||
        index >= ship->manifest.count ||
        station_idx < 0 || station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS) {
        return false;
    }
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, station_idx, &ship->manifest.units[index],
            production_ship_receipt_chain_at(ship, index));
    return evaluated.accepted;
}

static bool cargo_pub_nonzero(const cargo_unit_t *unit) {
    static const uint8_t zero[32] = {0};
    return unit && memcmp(unit->pub, zero, sizeof(zero)) != 0;
}

static bool emit_construction_contribution_batch(
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
    memset(payloads, 0, sizeof(payloads));
    memset(events, 0, sizeof(events));
    for (size_t i = 0; i < unit_count; i++) {
        if (!cargo_pub_nonzero(&units[i])) return false;
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
        if (payload->progress_after > 1.0f)
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

static int ship_contribute_trusted_module_supply(
    world_t *w, station_t *st, int station_idx, int module_idx,
    station_module_t *module, ship_t *ship, commodity_t material,
    float cost, int max_units,
    cargo_unit_t out_units[CHAIN_LOG_BATCH_MAX_EVENTS]) {
    if (!w || !st || !module || !ship || !out_units ||
        cost <= 0.0f || max_units <= 0) {
        return 0;
    }
    if (max_units > CHAIN_LOG_BATCH_MAX_EVENTS)
        max_units = CHAIN_LOG_BATCH_MAX_EVENTS;
    cargo_unit_t selected[CHAIN_LOG_BATCH_MAX_EVENTS] = {{0}};
    int selected_count = 0;
    for (uint16_t i = 0;
         i < ship->manifest.count &&
         selected_count < max_units; i++) {
        if (ship->manifest.units[i].commodity !=
                (uint8_t)material ||
            !ship_manifest_unit_trusted_at_station(
                w, ship, i, station_idx)) {
            continue;
        }
        selected[selected_count++] = ship->manifest.units[i];
    }
    if (selected_count <= 0) return 0;

    cargo_store_t staged = {0};
    if (!cargo_store_clone(&staged, &ship->cargo_store)) return 0;
    for (int i = 0; i < selected_count; i++) {
        int selected_idx =
            manifest_find(&staged.manifest, selected[i].pub);
        cargo_receipt_chain_t chain = {0};
        if (selected_idx < 0 ||
            !cargo_store_remove_with_chain(
                &staged, (uint16_t)selected_idx,
                &out_units[i], &chain) ||
            memcmp(out_units[i].pub, selected[i].pub,
                   sizeof(out_units[i].pub)) != 0) {
            cargo_store_cleanup(&staged);
            return 0;
        }
    }
    float progress_before = module->build_progress;
    float progress_after = progress_before +
        (float)selected_count / cost;
    if (progress_after > 1.0f - 0.0001f)
        progress_after = 1.0f;
    if (!emit_construction_contribution_batch(
            w, st, station_idx, module_idx, module,
            material, out_units, (size_t)selected_count,
            progress_before, cost)) {
        cargo_store_cleanup(&staged);
        return 0;
    }

    cargo_store_cleanup(&ship->cargo_store);
    ship->cargo_store = staged;
    memset(&staged, 0, sizeof(staged));
    module->build_progress = progress_after;
    ship_finished_sync(ship, material);
    return selected_count;
}

static int station_contribute_trusted_module_supply(
    world_t *w, station_t *st, int station_idx, int module_idx,
    station_module_t *module, commodity_t material, float cost,
    int max_units) {
    if (!w || !st || !module || cost <= 0.0f ||
        max_units <= 0) {
        return 0;
    }
    if (max_units > CHAIN_LOG_BATCH_MAX_EVENTS)
        max_units = CHAIN_LOG_BATCH_MAX_EVENTS;
    cargo_unit_t selected[CHAIN_LOG_BATCH_MAX_EVENTS] = {{0}};
    int selected_count = 0;
    for (uint16_t i = 0;
         i < st->manifest.count &&
         selected_count < max_units; i++) {
        if (st->manifest.units[i].commodity !=
                (uint8_t)material ||
            !production_station_unit_trusted(
                w, station_idx, i)) {
            continue;
        }
        selected[selected_count++] = st->manifest.units[i];
    }
    if (selected_count <= 0) return 0;

    cargo_store_t staged = {0};
    if (!cargo_store_clone(&staged, &st->cargo_store)) return 0;
    cargo_unit_t removed[CHAIN_LOG_BATCH_MAX_EVENTS] = {{0}};
    for (int i = 0; i < selected_count; i++) {
        int selected_idx =
            manifest_find(&staged.manifest, selected[i].pub);
        cargo_receipt_chain_t chain = {0};
        if (selected_idx < 0 ||
            !cargo_store_remove_with_chain(
                &staged, (uint16_t)selected_idx,
                &removed[i], &chain) ||
            memcmp(removed[i].pub, selected[i].pub,
                   sizeof(removed[i].pub)) != 0) {
            cargo_store_cleanup(&staged);
            return 0;
        }
    }
    float progress_before = module->build_progress;
    float progress_after = progress_before +
        (float)selected_count / cost;
    if (progress_after > 1.0f - 0.0001f)
        progress_after = 1.0f;
    if (!emit_construction_contribution_batch(
            w, st, station_idx, module_idx, module,
            material, removed, (size_t)selected_count,
            progress_before, cost)) {
        cargo_store_cleanup(&staged);
        return 0;
    }

    cargo_store_cleanup(&st->cargo_store);
    st->cargo_store = staged;
    memset(&staged, 0, sizeof(staged));
    st->manifest_dirty = true;
    module->build_progress = progress_after;
    station_finished_sync(st, material);
    return selected_count;
}

static int pod_contribute_trusted_module_supply(
    world_t *w, station_t *st, int station_idx, int module_idx,
    station_module_t *module, ship_t *ship,
    commodity_t material, float cost, int max_units,
    cargo_unit_t out_units[CHAIN_LOG_BATCH_MAX_EVENTS]) {
    if (!w || !st || !module || !ship || !out_units ||
        material >= COMMODITY_COUNT || cost <= 0.0f ||
        max_units <= 0) {
        return 0;
    }
    if (max_units > CHAIN_LOG_BATCH_MAX_EVENTS)
        max_units = CHAIN_LOG_BATCH_MAX_EVENTS;
    int pod_idx = -1;
    int selected_count = 0;
    cargo_unit_t selected[CHAIN_LOG_BATCH_MAX_EVENTS] = {{0}};
    for (int t = 0;
         t < ship->towed_pod_count && t < 10; t++) {
        int candidate = ship->towed_pods[t];
        if (candidate < 0 ||
            candidate >= MAX_CARGO_PODS) {
            continue;
        }
        const cargo_pod_t *pod =
            &w->cargo_pods[candidate];
        if (!cargo_pod_has_exact_manifest(
                pod, material) ||
            cargo_pod_player_tractor(pod) < 0) {
            continue;
        }
        int available = (int)pod->manifest_count;
        if (available > max_units) available = max_units;
        bool trusted = available > 0;
        for (int i = 0; i < available; i++) {
            const cargo_unit_t *unit =
                &pod->manifest_units[
                    pod->manifest_count - 1u - (uint16_t)i];
            if (!production_chainless_unit_trusted(
                    w, station_idx, unit)) {
                trusted = false;
                break;
            }
            selected[i] = *unit;
        }
        if (!trusted) {
            continue;
        }
        pod_idx = candidate;
        selected_count = available;
        break;
    }
    if (pod_idx < 0 || selected_count <= 0) return 0;

    production_pod_plan_t pod_plan = {0};
    for (int i = 0; i < selected_count; i++) {
        if (!production_stage_take_selected_unit(
                &pod_plan, w, pod_idx, material,
                &selected[i], &out_units[i])) {
            return 0;
        }
    }
    float progress_before = module->build_progress;
    float progress_after = progress_before +
        (float)selected_count / cost;
    if (progress_after > 1.0f - 0.0001f)
        progress_after = 1.0f;
    if (!emit_construction_contribution_batch(
            w, st, station_idx, module_idx,
            module, material, out_units,
            (size_t)selected_count,
            progress_before, cost)) {
        return 0;
    }

    production_commit_pod_plan(w, &pod_plan);
    if (!w->cargo_pods[pod_idx].active)
        world_cargo_pod_clear_tractor(w, pod_idx);
    module->build_progress = progress_after;
    return selected_count;
}

/* Deliver materials directly to scaffold modules. Materials are consumed
 * immediately from cargo but build progress advances at a fixed rate --
 * delivery fills the module's internal hopper (tracked via build_progress
 * vs the total cost), construction ticks over time in step_module_activation. */
float step_module_delivery(world_t *w, station_t *st, int station_idx,
                           ship_t *ship, commodity_t filter) {
    /* Total credit owed for materials this ship donated to construction.
     * The caller (player path: pay via ledger_earn + SELL event; NPC
     * path: credit via the hauler's economic identity) decides what to
     * do with it. Returning 0 means nothing was delivered or the cargo
     * was already drained by a contract path upstream.
     *
     * Bug history: until this returned a payout the player's cargo
     * silently vanished into a docked station's scaffolded modules
     * without payment — symptom was "selling ingots drains inventory
     * but doesn't credit." The contract path at try_sell_station_cargo
     * runs AFTER this function and would have paid, but module
     * construction had already consumed the cargo. */
    float payout = 0.0f;
    for (int i = 0; i < st->module_count; i++) {
        station_module_t *m = &st->modules[i];
        if (module_build_state(m) != MODULE_BUILD_AWAITING_SUPPLY) continue;
        commodity_t mat = module_build_material(m->type);
        /* Selective-delivery filter: when the player picks "deliver
         * <commodity>" the construction path must not eat anything
         * else. COMMODITY_COUNT means "no filter, deliver anything". */
        if (filter != COMMODITY_COUNT && filter != mat) continue;
        float cost = module_build_cost(m->type);
        float needed = cost * (1.0f - module_supply_fraction(m));
        if (needed < 0.01f) continue;

        /*
         * A towed physical pod has no receipt sidecar, but a unit produced
         * by this station can still be proven directly against its local
         * durable transform.  Trust and the contribution append are both
         * preflighted against a staged pod copy; failure leaves the pod,
         * tow binding, progress, and credits untouched.
         */
        if (ship && needed > 0.01f) {
            int whole = (int)ceilf(
                needed - 0.0001f);
            if (whole < 0) whole = 0;
            float price = station_buy_price(st, mat);
            int removed = 0;
            while (removed < whole) {
                cargo_unit_t
                    units[CHAIN_LOG_BATCH_MAX_EVENTS] = {{0}};
                int count =
                    pod_contribute_trusted_module_supply(
                        w, st, station_idx, i, m,
                        ship, mat, cost,
                        whole - removed, units);
                if (count <= 0) {
                    break;
                }
                for (int unit_idx = 0;
                     unit_idx < count; unit_idx++) {
                    payout += mining_payout_multiplier(
                        (mining_grade_t)units[unit_idx].grade) *
                        price;
                }
                removed += count;
            }
            needed -= (float)removed;
        }

        /* Finished cargo moves only as manifest+receipt rows. Float cargo
         * is not consulted or mutated; compatibility summaries are derived
         * when a legacy protocol payload is serialized. */
        if (ship && needed > 0.01f) {
            int whole = (int)ceilf(needed - 0.0001f);
            if (whole < 0) whole = 0;
            float price = station_buy_price(st, mat);
            int removed = 0;
            while (removed < whole) {
                cargo_unit_t
                    units[CHAIN_LOG_BATCH_MAX_EVENTS] = {{0}};
                int count =
                    ship_contribute_trusted_module_supply(
                        w, st, station_idx, i, m, ship,
                        mat, cost, whole - removed, units);
                if (count <= 0) {
                    break;
                }
                for (int unit_idx = 0;
                     unit_idx < count; unit_idx++) {
                    payout += mining_payout_multiplier(
                        (mining_grade_t)units[unit_idx].grade) *
                        price;
                }
                removed += count;
            }
            needed -= (float)removed;
        }

        /* Station-held finished stock is the same cargo-store component.
         * NPC deliveries no longer bounce through a temporary fake ship. */
        if (needed > 0.01f) {
            int whole = (int)ceilf(needed - 0.0001f);
            if (whole < 0) whole = 0;
            int removed = 0;
            while (removed < whole) {
                int count =
                    station_contribute_trusted_module_supply(
                        w, st, station_idx, i, m, mat, cost,
                        whole - removed);
                if (count <= 0) {
                    break;
                }
                removed += count;
            }
        }
        if (m->build_progress > 1.0f - 0.0001f) m->build_progress = 1.0f;
    }
    return payout;
}

/* ------------------------------------------------------------------ */
/* Shipyard repair-kit fabrication                                     */
/* ------------------------------------------------------------------ */
/* Every station with a MODULE_SHIPYARD runs an assembly bench that
 * turns 1 frame + 1 laser + 1 tractor module into REPAIR_KIT_PER_BATCH
 * (100) repair kits, on a slow cadence (REPAIR_KIT_FAB_PERIOD seconds
 * per batch). Kits are commodities — bought as cargo at the shipyard,
 * carried, and consumed at any dock during repair.
 *
 * Stations with a dock but no shipyard (e.g. Prospect) don't make
 * kits; they import them via the kit-deficit TRACTOR contract issued
 * by step_contracts. The triple-input recipe remains the load-bearing
 * demand sink that closes the production loop. */
void step_dock_repair_kit_fab(world_t *w, float dt) {
    if (!w) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        if (!station_has_module(st, MODULE_SHIPYARD)) continue;
        if ((float)station_finished_count(st, COMMODITY_REPAIR_KIT) >= REPAIR_KIT_STOCK_CAP) continue;

        st->repair_kit_fab_timer += dt;
        if (st->repair_kit_fab_timer < REPAIR_KIT_FAB_PERIOD) continue;

        /* All three inputs required. If any are missing, hold the timer
         * at the period (don't keep accumulating) so the next batch
         * fires the moment supply arrives. */
        const recipe_def_t *recipe = recipe_get(RECIPE_REPAIR_KIT_FAB);
        uint16_t indices[RECIPE_INPUT_MAX] = {0};
        cargo_unit_t inputs[RECIPE_INPUT_MAX] = {{0}};
        if (st->manifest.cap == 0 && !station_manifest_bootstrap(st)) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }
        if (!recipe || !station_manifest_select_recipe_inputs(
                w, s, st, recipe, indices, inputs)) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }

        int room = (int)floorf(REPAIR_KIT_STOCK_CAP + 0.0001f) -
                   station_finished_count(st, COMMODITY_REPAIR_KIT);
        int batch = recipe && recipe->output_count > 0
                  ? (int)recipe->output_count
                  : (int)floorf(REPAIR_KIT_PER_BATCH + 0.0001f);
        int int_minted = room < batch ? room : batch;
        if (int_minted <= 0 ||
            int_minted > CARGO_POD_MANIFEST_CAP ||
            int_minted > CHAIN_LOG_BATCH_MAX_EVENTS) {
            continue;
        }

        cargo_unit_t products[CARGO_POD_MANIFEST_CAP] = {{0}};
        bool products_ready = true;
        for (int k = 0; k < int_minted; k++) {
            if (!hash_product(
                    RECIPE_REPAIR_KIT_FAB, inputs,
                    recipe->input_count, (uint16_t)k,
                    &products[k])) {
                products_ready = false;
                break;
            }
            products[k].origin_station = (uint8_t)s;
        }
        if (!products_ready) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }

        cargo_store_t staged = {0};
        if (!cargo_store_clone(&staged, &st->cargo_store)) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }
        uint16_t sorted[RECIPE_INPUT_MAX] = {0};
        for (size_t i = 0; i < recipe->input_count; i++)
            sorted[i] = indices[i];
        for (size_t i = 0; i < recipe->input_count; i++) {
            for (size_t j = i + 1;
                 j < recipe->input_count; j++) {
                if (sorted[j] > sorted[i]) {
                    uint16_t swap = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = swap;
                }
            }
        }
        bool staged_ok = true;
        for (size_t i = 0; i < recipe->input_count; i++) {
            cargo_unit_t removed = {0};
            cargo_receipt_chain_t chain = {0};
            if (!cargo_store_remove_with_chain(
                    &staged, sorted[i], &removed, &chain)) {
                staged_ok = false;
                break;
            }
        }
        for (int k = 0; staged_ok && k < int_minted; k++) {
            if (!cargo_store_push_with_chain(
                    &staged, &products[k], NULL)) {
                staged_ok = false;
            }
        }
        if (!staged_ok) {
            cargo_store_cleanup(&staged);
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }
        chain_log_append_result_t appended =
            station_emit_craft_events(
                w, st, RECIPE_REPAIR_KIT_FAB, inputs,
                recipe->input_count, products,
                (size_t)int_minted);
        if (appended.status != CHAIN_LOG_APPEND_OK) {
            cargo_store_cleanup(&staged);
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }

        cargo_store_cleanup(&st->cargo_store);
        st->cargo_store = staged;
        memset(&staged, 0, sizeof(staged));
        st->manifest_dirty = true;
        for (size_t i = 0; i < recipe->input_count; i++) {
            station_finished_sync(
                st, recipe->input_commodities[i]);
        }
        station_finished_sync(st, COMMODITY_REPAIR_KIT);

        /* Light the SHIPYARD's tractor beam only after the durable craft
         * transaction and staged manifest commit both succeed. */
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].type == MODULE_SHIPYARD &&
                !st->modules[m].scaffold) {
                st->modules[m].active_pulse = 1.0f;
            }
        }
        st->repair_kit_fab_timer = 0.0f;
        SIM_LOG("[shipyard-fab] station %d minted %d kits (1 frame + 1 laser + 1 tractor consumed)\n",
                s, int_minted);
    }
}
