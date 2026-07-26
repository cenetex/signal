/*
 * sim_production.c -- Material flow, smelting, and station production.
 * Extracted from game_sim.c.
 */
#include "sim_production.h"
#include "tractor.h"
#include "laser.h"
#include "sim_asteroid.h"      /* fracture_claim_state_reset */
#include "sim_construction.h"  /* module_build_material, module_build_cost */
#include "manifest.h"
#include "cargo_receipt_trust.h"
#include "mining.h"            /* grade roll at smelt time */
#include "fixpoint.h"
#include "sha256.h"
#include "chain_log.h"         /* signed event emission (#479 C) */
#include <stdlib.h>            /* abs */
#include <math.h>
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
        if (!w->players[i].connected || !w->players[i].session_ready) continue;
        if (memcmp(w->players[i].session_token, token, 8) == 0)
            return i;
    }
    return -1;
}

typedef enum {
    FURNACE_SHELL_NONE = 0,
    FURNACE_SHELL_LOOSE_POD,
    FURNACE_SHELL_STATION,
} furnace_shell_source_kind_t;

typedef struct {
    furnace_shell_source_kind_t kind;
    int pod_idx;
    vec2 pod_pos;
    vec2 pod_vel;
    float pod_rotation;
    float pod_spin;
    tractor_binding_t pod_tractor;
    cargo_unit_t unit;
    cargo_receipt_chain_t chain;
} furnace_shell_source_t;

static bool furnace_take_loose_shell_frame(world_t *w,
                                           const station_t *st,
                                           int station_idx,
                                           vec2 target,
                                           float range,
                                           furnace_shell_source_t *source) {
    if (!w || !st || !source || range <= 0.0f ||
        station_idx < 0 || station_idx >= MAX_STATIONS) {
        return false;
    }
    float range_sq = range * range;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME)) continue;
        if (cargo_pod_has_player_tractor(pod)) continue;
        int tractor_station = -1;
        int tractor_module = -1;
        if (!cargo_pod_module_tractor_indices(pod, &tractor_station,
                                              &tractor_module))
            continue;
        if (tractor_station != station_idx ||
            tractor_module < 0 || tractor_module >= st->module_count)
            continue;
        if (!cargo_pod_module_tractor_arrived(w, pod, station_idx,
                                              tractor_module))
            continue;
        const station_module_t *owner = &st->modules[tractor_module];
        if (owner->scaffold) continue;
        bool owner_can_unfold_shell = false;
        if (owner->type == MODULE_FURNACE) {
            owner_can_unfold_shell = true;
        } else if (owner->type == MODULE_HOPPER &&
                   (commodity_t)owner->commodity == COMMODITY_FRAME) {
            owner_can_unfold_shell = true;
        } else {
            const module_schema_t *schema = module_schema(owner->type);
            owner_can_unfold_shell =
                schema && schema->kind == MODULE_KIND_PRODUCER;
        }
        if (!owner_can_unfold_shell) continue;
        vec2 owner_pos = module_world_pos_ring(st, owner->ring, owner->slot);
        if (v2_dist_sq(owner_pos, target) > range_sq) continue;

        furnace_shell_source_t next = {0};
        next.kind = FURNACE_SHELL_LOOSE_POD;
        next.pod_idx = i;
        next.pod_pos = pod->pos;
        next.pod_vel = pod->vel;
        next.pod_rotation = pod->rotation;
        next.pod_spin = pod->spin;
        next.pod_tractor = pod->tractor;
        if (!cargo_pod_take_manifest_unit(pod, COMMODITY_FRAME,
                                          &next.unit)) {
            return false;
        }
        if (!pod->active) world_cargo_pod_clear_tractor(w, i);
        *source = next;
        return true;
    }
    return false;
}

static bool furnace_take_station_shell_frame(station_t *st,
                                             furnace_shell_source_t *source) {
    if (!st || !source) return false;
    if (!station_manifest_bootstrap(st)) return false;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        if ((commodity_t)st->manifest.units[i].commodity != COMMODITY_FRAME)
            continue;

        furnace_shell_source_t next = {0};
        next.kind = FURNACE_SHELL_STATION;
        next.pod_idx = -1;
        if (!station_manifest_remove_with_chain(st, i, &next.unit,
                                                &next.chain)) {
            return false;
        }
        station_finished_sync(st, COMMODITY_FRAME);
        *source = next;
        return true;
    }
    return false;
}

static bool furnace_take_shell_frame(world_t *w,
                                     station_t *st,
                                     int station_idx,
                                     vec2 target,
                                     float range,
                                     furnace_shell_source_t *source) {
    if (!source) return false;
    memset(source, 0, sizeof(*source));
    source->pod_idx = -1;
    if (furnace_take_loose_shell_frame(w, st, station_idx, target, range,
                                       source))
        return true;
    return furnace_take_station_shell_frame(st, source);
}

static void furnace_restore_shell_frame(world_t *w,
                                        station_t *st,
                                        const furnace_shell_source_t *source) {
    if (!source || source->kind == FURNACE_SHELL_NONE) return;
    if (source->kind == FURNACE_SHELL_STATION) {
        if (st && (commodity_t)source->unit.commodity == COMMODITY_FRAME) {
            (void)station_manifest_push_with_chain(st, &source->unit,
                                                   &source->chain);
            station_finished_sync(st, COMMODITY_FRAME);
        }
        return;
    }
    if (source->kind != FURNACE_SHELL_LOOSE_POD || !w) return;

    if (source->pod_idx >= 0 && source->pod_idx < MAX_CARGO_PODS) {
        cargo_pod_t *pod = &w->cargo_pods[source->pod_idx];
        if (pod->active &&
            cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME) &&
            pod->manifest_count < CARGO_POD_MANIFEST_CAP) {
            pod->manifest_units[pod->manifest_count++] = source->unit;
            pod->quantity = pod->manifest_count;
            return;
        }
        if (!pod->active) {
            memset(pod, 0, sizeof(*pod));
            pod->active = true;
            pod->kind = CARGO_POD_CARGO;
            pod->commodity = COMMODITY_FRAME;
            pod->quantity = 1;
            pod->manifest_count = 1;
            pod->manifest_units[0] = source->unit;
            pod->pos = source->pod_pos;
            pod->vel = source->pod_vel;
            pod->radius = 18.0f;
            pod->rotation = source->pod_rotation;
            pod->spin = source->pod_spin;
            pod->tractor = source->pod_tractor;
            return;
        }
    }

    (void)spawn_cargo_pod_with_manifest_deterministic(
        w, source->pod_pos, source->pod_vel, COMMODITY_FRAME,
        &source->unit, 1, CARGO_POD_CARGO,
        source->pod_rotation, source->pod_spin);
}

static bool station_manifest_push_finished(station_t *st, const cargo_unit_t *unit) {
    if (!st || !unit) return false;
    if (st->manifest.cap == 0 || st->manifest.units == NULL) {
        if (!station_manifest_bootstrap(st)) return false;
    }
    if (station_manifest_push_with_chain(st, unit, NULL)) {
        st->manifest_dirty = true;
        return true;
    }
    return false;
}

static void station_emit_craft_event(world_t *w, station_t *st,
                                     recipe_id_t recipe_id,
                                     const cargo_unit_t *inputs,
                                     size_t input_count,
                                     const cargo_unit_t *product) {
    if (!w || !st || !inputs || !product ||
        input_count == 0 || input_count > RECIPE_INPUT_MAX) {
        return;
    }

    chain_payload_craft_t payload = {0};
    payload.recipe_id = (uint16_t)recipe_id;
    payload.input_count = (uint8_t)input_count;
    memcpy(payload.output_pub, product->pub, sizeof(payload.output_pub));
    for (size_t i = 0; i < input_count; i++)
        memcpy(payload.input_pubs[i], inputs[i].pub, sizeof(payload.input_pubs[i]));

    (void)chain_log_emit(w, st, CHAIN_EVT_CRAFT,
                         &payload, (uint16_t)sizeof(payload));
}

static bool manifest_unit_matches_recipe_input(const cargo_unit_t *unit,
                                               commodity_t commodity) {
    cargo_kind_t kind;
    if (!cargo_kind_for_commodity(commodity, &kind)) return false;
    return unit != NULL &&
           (cargo_kind_t)unit->kind == kind &&
           (commodity_t)unit->commodity == commodity;
}

static bool station_manifest_select_recipe_inputs(const station_t *st,
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
            out_indices[want] = i;
            out_inputs[want] = st->manifest.units[i];
            found = true;
            break;
        }
        if (!found) return false;
    }

    return true;
}

static bool station_manifest_consume_selected_inputs(station_t *st,
                                                     const uint16_t *indices,
                                                     size_t count) {
    uint16_t sorted[RECIPE_INPUT_MAX] = {0};
    if (!st || !indices || count == 0 || count > RECIPE_INPUT_MAX) return false;
    if (!st->manifest.units) return false;

    for (size_t i = 0; i < count; i++) sorted[i] = indices[i];
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (sorted[j] > sorted[i]) {
                uint16_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (!station_manifest_remove_with_chain(st, sorted[i], NULL, NULL))
            return false;
    }
    return true;
}

typedef struct {
    int pod_idx;
    commodity_t commodity;
    cargo_unit_t unit;
} loose_pod_recipe_input_t;

static bool world_has_free_cargo_pod_slot(const world_t *w) {
    if (!w) return false;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!w->cargo_pods[i].active) return true;
    }
    return false;
}

static int selected_count_for_pod(const loose_pod_recipe_input_t *selected,
                                  size_t selected_count,
                                  int pod_idx) {
    int count = 0;
    if (!selected || pod_idx < 0) return 0;
    for (size_t i = 0; i < selected_count; i++) {
        if (selected[i].pod_idx == pod_idx) count++;
    }
    return count;
}

static bool production_pod_staged_at_matching_hopper(const world_t *w,
                                                     const station_t *st,
                                                     int station_idx,
                                                     int module_idx,
                                                     const cargo_pod_t *pod,
                                                     commodity_t commodity) {
    if (!w || !st || !pod || commodity >= COMMODITY_COUNT ||
        module_idx < 0 || module_idx >= st->module_count) {
        return false;
    }
    const station_module_t *module = &st->modules[module_idx];
    if (module->scaffold) return false;
    vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
    const float consumer_range_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    if (cargo_pod_is_tractored_by_module(pod, station_idx, module_idx)) {
        int hopper_idx = station_find_hopper_for(st, commodity);
        if (hopper_idx >= 0 && hopper_idx < st->module_count) {
            const station_module_t *hopper = &st->modules[hopper_idx];
            if (!hopper->scaffold && hopper->type == MODULE_HOPPER &&
                (commodity_t)hopper->commodity == commodity) {
                vec2 hopper_pos = module_world_pos_ring(st, hopper->ring,
                                                        hopper->slot);
                if (v2_dist_sq(hopper_pos, module_pos) <= consumer_range_sq &&
                    cargo_pod_module_tractor_arrived(
                        w, pod, station_idx, module_idx))
                    return true;
            }
        }
    }
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold || hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity != commodity) continue;
        if (!cargo_pod_is_tractored_by_module(pod, station_idx, i)) continue;
        vec2 hopper_pos = module_world_pos_ring(st, hopper->ring,
                                                hopper->slot);
        if (v2_dist_sq(hopper_pos, module_pos) > consumer_range_sq)
            continue;
        if (cargo_pod_module_tractor_arrived(w, pod, station_idx, i))
            return true;
    }
    return false;
}

static bool station_select_loose_pod_recipe_inputs(
    const world_t *w,
    const station_t *st,
    int station_idx,
    int module_idx,
    const recipe_def_t *recipe,
    loose_pod_recipe_input_t selected[RECIPE_INPUT_MAX],
    cargo_unit_t inputs[RECIPE_INPUT_MAX]) {
    if (!w || !st || !recipe || !selected || !inputs ||
        module_idx < 0 || module_idx >= st->module_count ||
        recipe->input_count == 0 || recipe->input_count > RECIPE_INPUT_MAX) {
        return false;
    }

    memset(selected, 0, sizeof(loose_pod_recipe_input_t) * RECIPE_INPUT_MAX);
    memset(inputs, 0, sizeof(cargo_unit_t) * RECIPE_INPUT_MAX);

    for (size_t want = 0; want < recipe->input_count; want++) {
        commodity_t commodity = recipe->input_commodities[want];
        bool found = false;
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            const cargo_pod_t *pod = &w->cargo_pods[i];
            if (!cargo_pod_has_exact_manifest(pod, commodity)) continue;
            if (cargo_pod_has_player_tractor(pod)) continue;
            if (!production_pod_staged_at_matching_hopper(w, st, station_idx,
                                                          module_idx, pod,
                                                          commodity))
                continue;

            int already = selected_count_for_pod(selected, want, i);
            if (already < 0 || already >= (int)pod->manifest_count) continue;
            uint16_t unit_idx = (uint16_t)(pod->manifest_count - 1u - (uint16_t)already);
            cargo_unit_t unit = pod->manifest_units[unit_idx];
            if (!manifest_unit_matches_recipe_input(&unit, commodity))
                continue;

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

static bool station_loose_pod_recipe_inputs_available(const world_t *w,
                                                      const station_t *st,
                                                      int station_idx,
                                                      int module_idx,
                                                      const recipe_def_t *recipe) {
    loose_pod_recipe_input_t selected[RECIPE_INPUT_MAX];
    cargo_unit_t inputs[RECIPE_INPUT_MAX];
    return station_select_loose_pod_recipe_inputs(
        w, st, station_idx, module_idx, recipe, selected, inputs);
}

static bool station_consume_loose_pod_recipe_inputs(
    world_t *w,
    const loose_pod_recipe_input_t selected[RECIPE_INPUT_MAX],
    size_t input_count) {
    if (!w || !selected || input_count == 0 ||
        input_count > RECIPE_INPUT_MAX) {
        return false;
    }
    for (size_t i = 0; i < input_count; i++) {
        int pod_idx = selected[i].pod_idx;
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) return false;
        cargo_unit_t consumed = {0};
        if (!cargo_pod_take_manifest_unit(&w->cargo_pods[pod_idx],
                                          selected[i].commodity,
                                          &consumed)) {
            return false;
        }
        if (!w->cargo_pods[pod_idx].active)
            world_cargo_pod_clear_tractor(w, pod_idx);
        if (memcmp(consumed.pub, selected[i].unit.pub,
                   sizeof(consumed.pub)) != 0) {
            return false;
        }
    }
    return true;
}

static bool production_pod_can_accept_output(const cargo_pod_t *pod,
                                             commodity_t commodity,
                                             int product_count) {
    if (!pod || product_count <= 0 || commodity >= COMMODITY_COUNT)
        return false;
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

static int station_append_products_to_output_pod(world_t *w,
                                                 station_t *st,
                                                 int station_idx,
                                                 int module_idx,
                                                 commodity_t commodity,
                                                 const cargo_unit_t *products,
                                                 int product_count) {
    int pod_idx = station_find_output_pod_for_module(
        w, st, station_idx, module_idx, commodity, product_count);
    if (pod_idx < 0) return 0;

    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!production_pod_can_accept_output(pod, commodity, product_count))
        return 0;
    for (int i = 0; i < product_count; i++) {
        pod->manifest_units[pod->manifest_count++] = products[i];
    }
    pod->quantity = pod->manifest_count;
    pod->age = 0.0f;
    cargo_pod_set_station_custody(pod, station_idx);
    const station_module_t *module = &st->modules[module_idx];
    int output_hopper = station_find_output_hopper_for_module(st, module);
    (void)world_cargo_pod_set_module_tractor(
        w, pod_idx, station_idx,
        output_hopper >= 0 ? output_hopper : module_idx);
    return product_count;
}

static bool production_take_loose_shell_frame(world_t *w,
                                              int station_idx,
                                              int module_idx,
                                              furnace_shell_source_t *source) {
    if (!w || !source || station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME)) continue;
        if (cargo_pod_has_player_tractor(pod)) continue;
        if (!cargo_pod_is_tractored_by_module(pod, station_idx, module_idx))
            continue;
        if (!cargo_pod_module_tractor_arrived(w, pod, station_idx,
                                              module_idx))
            continue;

        furnace_shell_source_t next = {0};
        next.kind = FURNACE_SHELL_LOOSE_POD;
        next.pod_idx = i;
        next.pod_pos = pod->pos;
        next.pod_vel = pod->vel;
        next.pod_rotation = pod->rotation;
        next.pod_spin = pod->spin;
        next.pod_tractor = pod->tractor;
        if (!cargo_pod_take_manifest_unit(pod, COMMODITY_FRAME,
                                          &next.unit)) {
            return false;
        }
        if (!pod->active) world_cargo_pod_clear_tractor(w, i);
        *source = next;
        return true;
    }
    return false;
}

static bool production_loose_shell_frame_available(const world_t *w,
                                                   int station_idx,
                                                   int module_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME)) continue;
        if (cargo_pod_has_player_tractor(pod)) continue;
        if (cargo_pod_is_tractored_by_module(pod, station_idx, module_idx) &&
            cargo_pod_module_tractor_arrived(w, pod, station_idx,
                                             module_idx))
            return true;
    }
    return false;
}

static bool production_station_shell_frame_available(station_t *st) {
    if (!st) return false;
    if (!station_manifest_bootstrap(st)) return false;
    return manifest_count_by_commodity(&st->manifest, COMMODITY_FRAME) > 0;
}

static bool station_can_place_product_output(world_t *w,
                                             station_t *st,
                                             int station_idx,
                                             int module_idx,
                                             const recipe_def_t *recipe) {
    if (!w || !st || !recipe || recipe->output_commodity >= COMMODITY_COUNT)
        return false;
    int output_count = recipe->output_count > 0 ? (int)recipe->output_count : 1;
    if (output_count <= 0 || output_count > CARGO_POD_MANIFEST_CAP)
        return false;
    if (station_find_output_pod_for_module(w, st, station_idx, module_idx,
                                           recipe->output_commodity,
                                           output_count) >= 0) {
        return true;
    }
    return world_has_free_cargo_pod_slot(w) &&
           (production_loose_shell_frame_available(w, station_idx, module_idx) ||
            production_station_shell_frame_available(st));
}

static bool production_take_shell_frame(world_t *w,
                                        station_t *st,
                                        int station_idx,
                                        int module_idx,
                                        furnace_shell_source_t *source) {
    if (!source) return false;
    memset(source, 0, sizeof(*source));
    source->pod_idx = -1;
    if (production_take_loose_shell_frame(w, station_idx, module_idx, source))
        return true;
    return furnace_take_station_shell_frame(st, source);
}

static int station_craft_product_pod_from_inputs(world_t *w,
                                                 int station_idx,
                                                 int module_idx,
                                                 recipe_id_t recipe_id,
                                                 const cargo_unit_t *inputs,
                                                 size_t input_count) {
    station_t *st;
    const recipe_def_t *recipe = recipe_get(recipe_id);
    cargo_unit_t products[CARGO_POD_MANIFEST_CAP] = {{0}};
    furnace_shell_source_t pod_shell = {0};
    int output_count;
    int crafted = 0;
    int payload_start = 0;
    int payload_count = 0;

    if (!w || !inputs || station_idx < 0 || station_idx >= MAX_STATIONS ||
        module_idx < 0 || module_idx >= MAX_MODULES_PER_STATION) {
        return 0;
    }
    st = &w->stations[station_idx];
    if (module_idx >= st->module_count) return 0;
    if (!recipe || recipe->input_count == 0 ||
        recipe->input_count > RECIPE_INPUT_MAX ||
        input_count != recipe->input_count) {
        return 0;
    }
    output_count = recipe->output_count > 0 ? (int)recipe->output_count : 1;
    if (output_count <= 0 || output_count > CARGO_POD_MANIFEST_CAP)
        return 0;
    if (recipe->output_commodity >= COMMODITY_COUNT) return 0;

    for (int out_idx = 0; out_idx < output_count; out_idx++) {
        cargo_unit_t product = {0};
        if (!hash_product(recipe_id, inputs, input_count,
                          (uint16_t)out_idx, &product)) {
            break;
        }
        product.origin_station = (uint8_t)station_idx;
        product.mined_block = (uint64_t)(w->time * 120.0);
        products[crafted++] = product;
    }
    if (crafted <= 0) return 0;
    pod_shell.pod_idx = -1;

    int filled = station_append_products_to_output_pod(
        w, st, station_idx, module_idx, recipe->output_commodity,
        products, crafted);
    if (filled == crafted) {
        for (int i = 0; i < crafted; i++)
            station_emit_craft_event(w, st, recipe_id, inputs,
                                     recipe->input_count, &products[i]);
        return filled;
    }

    payload_count = crafted;
    if (!production_take_shell_frame(w, st, station_idx, module_idx,
                                     &pod_shell)) {
        return 0;
    }

    const station_module_t *module = &st->modules[module_idx];
    vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
    float angle = module_angle_ring(st, module->ring, module->slot);
    vec2 dir = v2_from_angle(angle);
    const float pod_radius = 18.0f;
    const float mouth_offset = STATION_MODULE_COL_RADIUS + pod_radius + 8.0f;
    vec2 pod_pos = v2_add(module_pos, v2_scale(dir, mouth_offset));
    vec2 pod_vel = station_ring_point_velocity(st, module->ring, pod_pos);
    int pod_idx = spawn_cargo_pod_with_manifest_deterministic(
        w, pod_pos, pod_vel, recipe->output_commodity,
        &products[payload_start], (uint16_t)payload_count,
        CARGO_POD_CARGO, angle, 0.22f);
    if (pod_idx < 0) {
        furnace_restore_shell_frame(w, st, &pod_shell);
        return 0;
    }
    cargo_pod_set_shell_frame(&w->cargo_pods[pod_idx], &pod_shell.unit);
    cargo_pod_set_station_custody(&w->cargo_pods[pod_idx], station_idx);
    int output_hopper = station_find_output_hopper_for_module(st, module);
    (void)world_cargo_pod_set_module_tractor(
        w, pod_idx, station_idx,
        output_hopper >= 0 ? output_hopper : module_idx);

    for (int i = payload_start; i < crafted; i++)
        station_emit_craft_event(w, st, recipe_id, inputs,
                                 recipe->input_count, &products[i]);
    return payload_count;
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
    if (!recipe || recipe->input_count == 0 || recipe->input_count > 2)
        return 0;
    if (!station_can_place_product_output(w, st, station_idx, module_idx,
                                          recipe)) {
        return 0;
    }
    if (st->manifest.cap == 0 || st->manifest.units == NULL) {
        if (!station_manifest_bootstrap(st)) return 0;
    }
    if (!station_manifest_select_recipe_inputs(st, recipe, indices, inputs)) return 0;

    if (!station_manifest_consume_selected_inputs(st, indices, recipe->input_count))
        return 0;

    int crafted = station_craft_product_pod_from_inputs(
        w, station_idx, module_idx, recipe_id, inputs, recipe->input_count);
    if (crafted <= 0) {
        for (size_t i = 0; i < recipe->input_count; i++)
            (void)station_manifest_push_with_chain(st, &inputs[i], NULL);
        return 0;
    }

    return crafted;
}

static int station_loose_pod_craft_product_pod_batch(world_t *w,
                                                     int station_idx,
                                                     int module_idx,
                                                     recipe_id_t recipe_id) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return 0;
    station_t *st = &w->stations[station_idx];
    const recipe_def_t *recipe = recipe_get(recipe_id);
    loose_pod_recipe_input_t selected[RECIPE_INPUT_MAX];
    cargo_unit_t inputs[RECIPE_INPUT_MAX];
    if (!recipe || recipe->input_count == 0 ||
        recipe->input_count > RECIPE_INPUT_MAX) {
        return 0;
    }
    if (!station_can_place_product_output(w, st, station_idx, module_idx,
                                          recipe)) {
        return 0;
    }
    if (!station_select_loose_pod_recipe_inputs(
            w, st, station_idx, module_idx, recipe, selected, inputs)) {
        return 0;
    }
    if (!station_consume_loose_pod_recipe_inputs(
            w, selected, recipe->input_count)) {
        return 0;
    }
    return station_craft_product_pod_from_inputs(
        w, station_idx, module_idx, recipe_id, inputs, recipe->input_count);
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
                station_loose_pod_recipe_inputs_available(w, st, s, m,
                                                          recipe_def);
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
            if (!pod_ready && !buffer_ready && !inventory_ready) continue;

            st->modules[m].craft_progress += rate * dt;
            if (st->modules[m].craft_progress > 4.0f)
                st->modules[m].craft_progress = 4.0f;

            float produced = 0.0f;
            while (st->modules[m].craft_progress + FLOAT_EPSILON >= 1.0f) {
                if (station_loose_pod_recipe_inputs_available(w, st, s, m,
                                                              recipe_def)) {
                    int crafted = station_loose_pod_craft_product_pod_batch(
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
                units[pushed] = unit;
                if (first_named_idx < 0 &&
                    (ingot_prefix_t)unit.prefix_class != INGOT_PREFIX_ANONYMOUS) {
                    first_named_idx = pushed;
                }
                pushed++;
            }

            int pod_idx = -1;
            furnace_shell_source_t pod_shell = {0};
            pod_shell.pod_idx = -1;
            if (pushed > 0) {
                if (!furnace_take_shell_frame(w, st, smelt_station,
                                              smelt_midpoint,
                                              HOPPER_PULL_RANGE,
                                              &pod_shell)) {
                    SIM_LOG("[smelt] station %d waiting for frame shell for %s pod\n",
                            smelt_station, commodity_short_name(output));
                    continue;
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
                pod_idx = spawn_cargo_pod_with_manifest_deterministic(
                    w, pod_pos, pod_vel, output, units, (uint16_t)pushed,
                    CARGO_POD_CARGO, rotation, 0.18f);
                if (pod_idx < 0) {
                    furnace_restore_shell_frame(w, st, &pod_shell);
                    SIM_LOG("[smelt] station %d could not spawn %s pod; restored frame shell\n",
                            smelt_station, commodity_short_name(output));
                    continue;
                }
                cargo_pod_set_shell_frame(&w->cargo_pods[pod_idx],
                                          &pod_shell.unit);
                cargo_pod_set_station_custody(&w->cargo_pods[pod_idx],
                                              smelt_station);
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
            SIM_LOG("[smelt-attr] tower=%d fracturer=%d tow_tok=%02x%02x%02x%02x frac_tok=%02x%02x%02x%02x\n",
                    tower, fracturer,
                    a->last_towed_token[0], a->last_towed_token[1],
                    a->last_towed_token[2], a->last_towed_token[3],
                    a->last_fractured_token[0], a->last_fractured_token[1],
                    a->last_fractured_token[2], a->last_fractured_token[3]);

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
                uint8_t pk[32];
                sha256_bytes(w->players[roller].session_token, 8, pk);
                char cs[8];
                mining_callsign_from_pubkey(pk, cs);
                if (grade == MINING_GRADE_COMMISSIONED)
                    snprintf(msg, sizeof(msg), "%s published commissioned ore  +%d",
                             cs, bonus_cr);
                else
                    snprintf(msg, sizeof(msg), "%s published RATi ore  +%d",
                             cs, bonus_cr);
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
                        u->mined_block = signal_channel_post(w, smelt_station,
                                                             text, "");
                    }

                    for (uint16_t u = 0; u < pod->manifest_count; u++) {
                        const cargo_unit_t *unit = &pod->manifest_units[u];
                        chain_payload_smelt_t payload = {0};
                        memcpy(payload.fragment_pub, a->fragment_pub, 32);
                        memcpy(payload.ingot_pub, unit->pub, 32);
                        payload.prefix_class = unit->prefix_class;
                        payload.mined_block = unit->mined_block;
                        (void)chain_log_emit(w, st, CHAIN_EVT_SMELT,
                                             &payload,
                                             (uint16_t)sizeof(payload));
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

static const cargo_receipt_chain_t *production_station_receipt_chain_at(
    const station_t *station, uint16_t index) {
    const ship_receipts_t *receipts =
        station_get_receipts_const(station);
    if (!receipts || !receipts->chains || index >= receipts->count)
        return NULL;
    return &receipts->chains[index];
}

static int station_manifest_find_first_trusted_commodity(
    const world_t *w, const station_t *station, int station_idx,
    commodity_t commodity) {
    if (!w || !station || !station->manifest.units) return -1;
    for (uint16_t i = 0; i < station->manifest.count; i++) {
        const cargo_unit_t *unit = &station->manifest.units[i];
        if (unit->commodity != (uint8_t)commodity) continue;
        cargo_receipt_station_evaluation_t evaluated =
            cargo_receipt_evaluate_at_station(
                w, station_idx, unit,
                production_station_receipt_chain_at(station, i));
        if (evaluated.accepted) return (int)i;
    }
    return -1;
}

static bool ship_manifest_unit_trusted_for_station(
    const world_t *w, const ship_t *ship, uint16_t index, int station_idx) {
    if (!w || !ship || !ship->manifest.units ||
        index >= ship->manifest.count ||
        station_idx < 0 || station_idx >= MAX_STATIONS) {
        return false;
    }
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, station_idx, &ship->manifest.units[index],
            production_ship_receipt_chain_at(ship, index));
    return evaluated.accepted;
}

static int ship_manifest_count_legal_commodity(const world_t *w,
                                               const ship_t *ship,
                                               int station_idx,
                                               commodity_t c) {
    if (!ship || !ship->manifest.units) return 0;
    int count = 0;
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        if (ship->manifest.units[i].commodity != (uint8_t)c) continue;
        if (!ship_manifest_unit_trusted_for_station(w, ship, i, station_idx))
            continue;
        count++;
    }
    return count;
}

static int ship_manifest_find_first_legal_commodity(const world_t *w,
                                                    const ship_t *ship,
                                                    int station_idx,
                                                    commodity_t c) {
    if (!ship || !ship->manifest.units) return -1;
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        if (ship->manifest.units[i].commodity != (uint8_t)c) continue;
        if (!ship_manifest_unit_trusted_for_station(w, ship, i, station_idx))
            continue;
        return (int)i;
    }
    return -1;
}

static bool cargo_pub_nonzero(const cargo_unit_t *unit) {
    static const uint8_t zero[32] = {0};
    return unit && memcmp(unit->pub, zero, sizeof(zero)) != 0;
}

static void emit_construction_contribution(world_t *w, station_t *st,
                                           int station_idx, int module_idx,
                                           const station_module_t *module,
                                           commodity_t commodity,
                                           const cargo_unit_t *unit,
                                           float progress_after) {
    if (!w || !st || !module || !cargo_pub_nonzero(unit)) return;
    chain_payload_construction_t payload = {0};
    memcpy(payload.cargo_pub, unit->pub, sizeof(payload.cargo_pub));
    payload.target_kind = CONSTRUCTION_TARGET_MODULE;
    payload.station_index = (station_idx >= 0 && station_idx <= 255)
        ? (uint8_t)station_idx : 0xff;
    payload.module_index = (module_idx >= 0 && module_idx <= 255)
        ? (uint8_t)module_idx : 0xff;
    payload.module_type = (uint8_t)module->type;
    payload.commodity = (uint8_t)commodity;
    payload.target_id = (station_idx >= 0) ? (uint64_t)station_idx : 0u;
    payload.contributed_units = 1.0f;
    payload.progress_after = progress_after;
    (void)chain_log_emit(w, st, CHAIN_EVT_CONSTRUCTION,
                         &payload, sizeof(payload));
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

        int pod_units = ship
            ? ship_towed_pods_trusted_manifest_count(
                w, ship, mat, station_idx) : 0;
        if (pod_units > 0) {
            int whole = (int)ceilf(needed - 0.0001f);
            if (whole > pod_units) whole = pod_units;
            if (whole < 0) whole = 0;
            if (whole > 0) {
                float price = station_buy_price(st, mat);
                int removed = 0;
                while (removed < whole) {
                    cargo_unit_t unit = {0};
                    if (!ship_towed_pods_take_trusted_manifest_unit(
                            w, ship, mat, station_idx, &unit)) {
                        break;
                    }
                    m->build_progress += 1.0f / cost;
                    float progress_after = module_supply_fraction(m);
                    float mult = mining_payout_multiplier((mining_grade_t)unit.grade);
                    payout += mult * price;
                    emit_construction_contribution(w, st, station_idx, i, m,
                                                   mat, &unit, progress_after);
                    removed++;
                }
                if (removed > 0)
                    needed -= (float)removed;
            }
        }

        /* Finished cargo moves only as manifest+receipt rows. Float cargo
         * is not consulted or mutated; compatibility summaries are derived
         * when a legacy protocol payload is serialized. */
        if (ship && needed > 0.01f) {
            int legal_units = ship_manifest_count_legal_commodity(
                w, ship, station_idx, mat);
            int whole = (int)ceilf(needed - 0.0001f);
            if (whole > legal_units) whole = legal_units;
            if (whole < 0) whole = 0;
            float price = station_buy_price(st, mat);
            int removed = 0;
            while (removed < whole) {
                int cargo_idx = ship_manifest_find_first_legal_commodity(
                    w, ship, station_idx, mat);
                if (cargo_idx < 0) break;
                cargo_unit_t unit = {0};
                if (!ship_manifest_remove_with_chain(
                        ship, (uint16_t)cargo_idx, &unit, NULL)) break;
                m->build_progress += 1.0f / cost;
                float progress_after = module_supply_fraction(m);
                payout += mining_payout_multiplier(
                    (mining_grade_t)unit.grade) * price;
                emit_construction_contribution(
                    w, st, station_idx, i, m, mat, &unit, progress_after);
                removed++;
            }
            needed -= (float)removed;
        }

        /* Station-held finished stock is the same cargo-store component.
         * NPC deliveries no longer bounce through a temporary fake ship. */
        if (needed > 0.01f) {
            int available = station_finished_count(st, mat);
            int whole = (int)ceilf(needed - 0.0001f);
            if (whole > available) whole = available;
            if (whole < 0) whole = 0;
            int removed = 0;
            while (removed < whole) {
                int cargo_idx =
                    station_manifest_find_first_trusted_commodity(
                        w, st, station_idx, mat);
                if (cargo_idx < 0) break;
                cargo_unit_t unit = {0};
                if (!station_manifest_remove_with_chain(
                        st, (uint16_t)cargo_idx, &unit, NULL)) break;
                m->build_progress += 1.0f / cost;
                float progress_after = module_supply_fraction(m);
                emit_construction_contribution(
                    w, st, station_idx, i, m, mat, &unit, progress_after);
                removed++;
            }
            if (removed > 0) station_finished_sync(st, mat);
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
        if (!recipe || !station_manifest_select_recipe_inputs(st, recipe, indices, inputs)) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }

        int room = (int)floorf(REPAIR_KIT_STOCK_CAP + 0.0001f) -
                   station_finished_count(st, COMMODITY_REPAIR_KIT);
        int batch = recipe && recipe->output_count > 0
                  ? (int)recipe->output_count
                  : (int)floorf(REPAIR_KIT_PER_BATCH + 0.0001f);
        int int_minted = room < batch ? room : batch;
        if (int_minted <= 0) continue;

        if (!station_manifest_consume_selected_inputs(st, indices, recipe->input_count)) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }
        station_finished_sync(st, COMMODITY_FRAME);
        station_finished_sync(st, COMMODITY_LASER_MODULE);
        station_finished_sync(st, COMMODITY_TRACTOR_MODULE);

        /* Light the SHIPYARD's tractor beam. The kit-fab path doesn't
         * go through the producer-recipe pipeline that already pulses
         * other producers, so set it here. */
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].type == MODULE_SHIPYARD &&
                !st->modules[m].scaffold) {
                st->modules[m].active_pulse = 1.0f;
            }
        }
        /* Mint kits as real recipe products. The output multiplier lives
         * in this production loop; each kit's pub key binds back to the
         * same frame/laser/tractor input set through hash_product. */
        int actual_minted = 0;
        for (int k = 0; k < int_minted; k++) {
            cargo_unit_t unit = {0};
            if (!hash_product(RECIPE_REPAIR_KIT_FAB, inputs, recipe->input_count,
                              (uint16_t)k, &unit))
                break;
            unit.origin_station = (uint8_t)s;
            unit.mined_block = (uint64_t)(w->time * 120.0);
            if (!station_manifest_push_finished(st, &unit)) break;
            station_emit_craft_event(w, st, RECIPE_REPAIR_KIT_FAB, inputs,
                                     recipe->input_count, &unit);
            actual_minted++;
        }
        if (actual_minted > 0)
            station_finished_sync(st, COMMODITY_REPAIR_KIT);
        st->repair_kit_fab_timer = 0.0f;
        SIM_LOG("[shipyard-fab] station %d minted %d kits (1 frame + 1 laser + 1 tractor consumed)\n",
                s, actual_minted);
    }
}
