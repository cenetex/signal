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
#include "mining.h"            /* grade roll at smelt time */
#include "sha256.h"
#include "chain_log.h"         /* signed event emission (#479 C) */
#include <stdlib.h>            /* abs */
#include <math.h>              /* lroundf */
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

static bool station_manifest_push_ingot(station_t *st, const cargo_unit_t *unit) {
    if (!st || !unit) return false;
    if (st->manifest.cap == 0 || st->manifest.units == NULL) {
        if (!station_manifest_bootstrap(st)) return false;
    }
    /* FIFO-evict the oldest unit when the manifest is full. M4: flag the
     * station as manifest-dirty so the world broadcaster picks up the
     * rotation and clients can surface a "stockpile rotated" notice
     * instead of the ingot silently vanishing. */
    if (st->manifest.count >= st->manifest.cap) {
        if (!station_manifest_remove_with_chain(st, 0, NULL, NULL)) return false;
    }
    /* Phase 2: flag dirty on every successful push so the manifest-
     * summary broadcast runs after smelts too (not just rotations). */
    if (station_manifest_push_with_chain(st, unit, NULL)) {
        st->manifest_dirty = true;
        return true;
    }
    return false;
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

static int station_manifest_craft_product_batch(world_t *w, station_t *st,
                                                recipe_id_t recipe_id) {
    const recipe_def_t *recipe = recipe_get(recipe_id);
    uint16_t indices[RECIPE_INPUT_MAX] = {0};
    cargo_unit_t inputs[RECIPE_INPUT_MAX] = {{0}};
    int output_count;
    int crafted = 0;

    if (!st || !recipe || recipe->input_count == 0 ||
        recipe->input_count > 2) {
        return 0;
    }
    output_count = recipe->output_count > 0 ? (int)recipe->output_count : 1;
    if (output_count <= 0) return 0;
    if (st->manifest.cap == 0 || st->manifest.units == NULL) {
        if (!station_manifest_bootstrap(st)) return 0;
    }
    if (!station_manifest_select_recipe_inputs(st, recipe, indices, inputs)) return 0;
    if (!station_manifest_consume_selected_inputs(st, indices, recipe->input_count)) return 0;

    for (int out_idx = 0; out_idx < output_count; out_idx++) {
        cargo_unit_t product = {0};
        if (!hash_product(recipe_id, inputs, recipe->input_count,
                          (uint16_t)out_idx, &product)) {
            break;
        }
        if (w && st >= w->stations && st < w->stations + MAX_STATIONS) {
            product.origin_station = (uint8_t)(st - w->stations);
            product.mined_block = (uint64_t)(w->time * 120.0);
        }
        if (!station_manifest_push_finished(st, &product)) break;
        crafted++;

        chain_payload_craft_t payload = {0};
        payload.recipe_id = (uint16_t)recipe_id;
        payload.input_count = (uint8_t)recipe->input_count;
        memcpy(payload.output_pub, product.pub, 32);
        for (size_t i = 0; i < recipe->input_count && i < 2; i++)
            memcpy(payload.input_pubs[i], inputs[i].pub, 32);
        (void)chain_log_emit(w, st, CHAIN_EVT_CRAFT,
                             &payload, (uint16_t)sizeof(payload));
    }
    return crafted;
}

typedef struct {
    recipe_id_t recipe_id;
    commodity_t primary_input;
    float primary_units_per_batch;
    commodity_t secondary_input;
    float secondary_units_per_batch;
    commodity_t output;
    float output_units_per_batch;
} producer_recipe_t;

static bool producer_recipe_for_module(module_type_t mt, producer_recipe_t *out_recipe) {
    recipe_id_t recipe_id;
    const recipe_def_t *recipe;
    commodity_t primary;

    if (!out_recipe) return false;
    memset(out_recipe, 0, sizeof(*out_recipe));
    out_recipe->secondary_input = COMMODITY_COUNT;

    switch (mt) {
    case MODULE_FRAME_PRESS: recipe_id = RECIPE_FRAME_BASIC; break;
    case MODULE_LASER_FAB:   recipe_id = RECIPE_LASER_BASIC; break;
    case MODULE_TRACTOR_FAB: recipe_id = RECIPE_TRACTOR_COIL; break;
    default: return false;
    }

    recipe = recipe_get(recipe_id);
    if (!recipe) return false;
    out_recipe->recipe_id = recipe_id;
    primary = module_schema_input(mt);
    out_recipe->primary_input = primary;
    out_recipe->output = recipe->output_commodity;
    out_recipe->output_units_per_batch =
        recipe->output_count > 0 ? (float)recipe->output_count : 1.0f;

    for (size_t i = 0; i < recipe->input_count; i++) {
        commodity_t input = recipe->input_commodities[i];
        if (input == primary) {
            out_recipe->primary_units_per_batch += 1.0f;
            continue;
        }
        if (out_recipe->secondary_input == COMMODITY_COUNT ||
            out_recipe->secondary_input == input) {
            out_recipe->secondary_input = input;
            out_recipe->secondary_units_per_batch += 1.0f;
            continue;
        }
        return false;
    }

    return out_recipe->primary_units_per_batch > 0.0f &&
           out_recipe->output == module_schema_output(mt);
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
/* Fabs also pull directly from inventory as a slow fallback so        */
/* production never fully stalls, but flow-fed fabs run much faster.   */
/* ------------------------------------------------------------------ */

void sim_step_station_production(world_t *w, float dt) {
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

            commodity_t input_com = recipe.primary_input;
            commodity_t output_com = recipe.output;
            if (input_com >= COMMODITY_COUNT || output_com >= COMMODITY_COUNT) continue;
            float room_units = MAX_PRODUCT_STOCK - st->_inventory_cache[output_com];
            if (room_units + FLOAT_EPSILON < recipe.output_units_per_batch) continue;
            float rate = schema->rate > 0.0f ? schema->rate : STATION_PRODUCTION_RATE;
            bool secondary_ready = true;
            if (recipe.secondary_input < COMMODITY_COUNT) {
                secondary_ready =
                    st->_inventory_cache[recipe.secondary_input] + FLOAT_EPSILON >=
                    recipe.secondary_units_per_batch;
            }
            bool buffer_ready =
                st->module_input[m] + FLOAT_EPSILON >= recipe.primary_units_per_batch &&
                secondary_ready;
            bool inventory_ready =
                st->_inventory_cache[input_com] + FLOAT_EPSILON >= recipe.primary_units_per_batch &&
                secondary_ready;
            if (!buffer_ready && !inventory_ready) continue;

            st->module_craft_progress[m] += rate * dt;
            if (st->module_craft_progress[m] > 4.0f)
                st->module_craft_progress[m] = 4.0f;

            float produced = 0.0f;
            while (st->module_craft_progress[m] + FLOAT_EPSILON >= 1.0f &&
                   room_units + FLOAT_EPSILON >= recipe.output_units_per_batch) {
                bool from_buffer =
                    st->module_input[m] + FLOAT_EPSILON >= recipe.primary_units_per_batch;
                if (!from_buffer &&
                    st->_inventory_cache[input_com] + FLOAT_EPSILON <
                    recipe.primary_units_per_batch) {
                    break;
                }
                if (recipe.secondary_input < COMMODITY_COUNT &&
                    st->_inventory_cache[recipe.secondary_input] + FLOAT_EPSILON <
                    recipe.secondary_units_per_batch) {
                    break;
                }

                int crafted = station_manifest_craft_product_batch(w, st, recipe.recipe_id);
                if (crafted <= 0) {
                    st->module_craft_progress[m] = 1.0f;
                    break;
                }

                if (from_buffer) {
                    st->module_input[m] -= recipe.primary_units_per_batch;
                    if (st->module_input[m] < 0.0f) st->module_input[m] = 0.0f;
                } else {
                    st->_inventory_cache[input_com] -= recipe.primary_units_per_batch;
                    if (st->_inventory_cache[input_com] < 0.0f)
                        st->_inventory_cache[input_com] = 0.0f;
                }
                if (recipe.secondary_input < COMMODITY_COUNT) {
                    st->_inventory_cache[recipe.secondary_input] -=
                        recipe.secondary_units_per_batch;
                    if (st->_inventory_cache[recipe.secondary_input] < 0.0f)
                        st->_inventory_cache[recipe.secondary_input] = 0.0f;
                }
                st->_inventory_cache[output_com] += (float)crafted;
                produced += (float)crafted;
                room_units -= (float)crafted;
                st->module_craft_progress[m] -= 1.0f;
            }

            if (produced > 0.0f) {
                st->module_active_pulse[m] = 1.0f;
                float cap = module_buffer_capacity(mt);
                if (cap > 0.0f) {
                    float buf_room = cap - st->module_output[m];
                    if (buf_room > 0.0f) {
                        float add = fminf(produced, buf_room);
                        st->module_output[m] += add;
                    }
                }
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
                        if (dd < best_d) { best_d = dd; silo_pos = mp2; has_silo = true; }
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

                /* Pull fragment toward the midpoint between furnace and
                 * silo. World-pinned source (midpoint isn't a single
                 * body — it's the geometric center of the two-module
                 * smelt path). Linear-falloff constant-pull beam:
                 * peak at d=0, decays to zero at pull_range. */
                vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);
                static const tractor_beam_t SMELT_BEAM = {
                    .rest_length     = 0.0f,
                    .pull_strength   = 0.0f,
                    .push_strength   = 0.0f,
                    .pull_constant   = HOPPER_PULL_ACCEL * 1.5f,
                    .push_constant   = 0.0f,
                    .range           = HOPPER_PULL_RANGE,
                    .axial_damping   = 8.0f,
                    .tangent_damping = 2.0f,    /* ~25% of axial */
                    .speed_cap       = 100.0f,
                    .falloff         = TRACTOR_FALLOFF_LINEAR,
                };
                tractor_anchor_t src = { .pos = midpoint, .vel = NULL,    .inv_mass = 0.0f };
                tractor_anchor_t tgt = { .pos = a->pos,   .vel = &a->vel, .inv_mass = 1.0f };
                (void)tractor_apply(&src, &tgt, &SMELT_BEAM, dt);

                /* Pulse the furnace module — the existing ring-spoke
                 * physics in step_station_ring_dynamics looks at
                 * module_active_pulse[] to scale spoke torque. Without
                 * this, an active smelt beam wouldn't drive any ring
                 * rotation. Also tag the ore for the dynamic furnace
                 * glow; the retired hopper-float path used to be the
                 * only writer for this field. */
                st->modules[m].last_smelt_commodity = (uint8_t)a->commodity;
                st->module_active_pulse[m] = 1.0f;

                float d_mid = sqrtf(v2_dist_sq(a->pos, midpoint));
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
             * clear the player's tractor — let the smelt run and clamp
             * the inventory below; overshoot is lost to atmospheric
             * exhaust but the player keeps the payout. */

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
                commodity_t proj_ingot = commodity_refined_form(a->commodity);
                if (proj_ingot != a->commodity) {
                    int proj_units = (int)floorf(a->ore + 0.0001f);
                    if (proj_units > 0) {
                        float sum_mult = 0.0f;
                        int counted = 0;
                        for (int idx = 0; idx < proj_units; idx++) {
                            cargo_unit_t proj_u = {0};
                            if (!hash_ingot(proj_ingot, (mining_grade_t)a->grade,
                                            a->fragment_pub,
                                            (uint16_t)idx, &proj_u))
                                continue;
                            sum_mult += prefix_class_price_multiplier(
                                            (int)proj_u.prefix_class);
                            counted++;
                        }
                        if (counted > 0)
                            ore_value *= (sum_mult / (float)counted);
                    }
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
             * now smelt with no credit recipient. The ore still lands in
             * the station inventory as infrastructure value; the ingot
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
            mining_grade_t grade = (mining_grade_t)a->grade;
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
                    pt->ship.stat_credits_earned += credited;
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
                        pf->ship.stat_credits_earned += fcredited;
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
                    pf->ship.stat_credits_earned += credited;
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_SELL, .player_id = fracturer,
                        .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                  .base_cr = (int)lroundf(half / bonus_mult),
                                  .bonus_cr = (int)lroundf(half - half / bonus_mult),
                                  .by_contract = bc }});
                }
            }

            /* Clean from tower's tow list */
            if (tower >= 0) {
                server_player_t *sp = &w->players[tower];
                for (int t = 0; t < sp->ship.towed_count; t++) {
                    if (sp->ship.towed_fragments[t] == i) {
                        sp->ship.towed_count--;
                        sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
                        sp->ship.towed_fragments[sp->ship.towed_count] = -1;
                        break;
                    }
                }
            }

            /* Smelt: ore -> ingot in station inventory. Clamp at the
             * stockpile cap — overshoot is vented (and the manifest write
             * below sees only the accepted delta). The player still gets
             * paid for the full ore value via the ledger above. */
            commodity_t ingot = commodity_refined_form(a->commodity);
            commodity_t output = (ingot != a->commodity) ? ingot : a->commodity;
            float stock_before = st->_inventory_cache[output];
            st->_inventory_cache[output] += a->ore;
            if (st->_inventory_cache[output] > MAX_PRODUCT_STOCK)
                st->_inventory_cache[output] = MAX_PRODUCT_STOCK;

            /* Push one manifest unit per integer of finished ingot
             * smelted. Each unit carries its own prefix_class derived
             * from base58(pub); this is the single identity store now —
             * the legacy named_ingots[] dual store was removed. */
            {
                /* Large fragments may overfill the remaining room. Mint
                 * identity only for the units that actually landed in the
                 * station bin; the rest is explicit vented overflow. */
                int units_before = (int)floorf(stock_before + 0.0001f);
                int units_after = (int)floorf(st->_inventory_cache[output] + 0.0001f);
                int manifest_units = units_after - units_before;
                int pushed = 0;
                int first_named_idx = -1;
                for (int idx = 0; idx < manifest_units; idx++) {
                    cargo_unit_t unit = {0};
                    if (!hash_ingot(output, grade, a->fragment_pub, (uint16_t)idx, &unit))
                        continue;
                    /* Stamp origin so the unit can be traced back to this
                     * refinery without a side table. mined_block is filled
                     * after-the-fact for the first non-anonymous unit (so
                     * the signal_channel post is the chain anchor). */
                    unit.origin_station = (uint8_t)smelt_station;
                    if (!station_manifest_push_ingot(st, &unit))
                        break;
                    pushed++;
                    if (first_named_idx < 0 &&
                        (ingot_prefix_t)unit.prefix_class != INGOT_PREFIX_ANONYMOUS) {
                        first_named_idx = (int)st->manifest.count - 1;
                    }
                }
                if (pushed > 0) st->manifest_dirty = true;
                SIM_LOG("[smelt] station %d %s grade=%d ore=%.2f units=%d pushed=%d\n",
                        smelt_station, commodity_short_name(output),
                        (int)grade, a->ore, manifest_units, pushed);

                /* Announce the first named ingot on the station signal
                 * channel and stamp the resulting block id back onto the
                 * manifest unit. This replaces the old named_ingot_t
                 * stockpile mirror — the unit IS the named-ingot record. */
                if (first_named_idx >= 0 &&
                    first_named_idx < (int)st->manifest.count) {
                    cargo_unit_t *u = &st->manifest.units[first_named_idx];
                    char cs[12];
                    mining_render_callsign(u->pub, cs);
                    char text[96];
                    snprintf(text, sizeof(text), "smelted %s", cs);
                    u->mined_block = signal_channel_post(w, smelt_station, text, "");
                }
                (void)ingot; /* unused now — kept above for the inventory write */

                /* Layer C of #479: emit EVT_SMELT for each newly-minted
                 * ingot. fragment_pub is populated from the consumed
                 * asteroid record so the chain log captures real
                 * provenance — every downstream verifier can walk back
                 * to the source rock. The retired hopper-float smelt
                 * path no longer emits zero-fragment EVT_SMELT events. */
                if (pushed > 0) {
                    uint16_t first_new = (uint16_t)((int)st->manifest.count - pushed);
                    for (uint16_t u = first_new; u < st->manifest.count; u++) {
                        const cargo_unit_t *unit = &st->manifest.units[u];
                        chain_payload_smelt_t payload = {0};
                        memcpy(payload.fragment_pub, a->fragment_pub, 32);
                        memcpy(payload.ingot_pub, unit->pub, 32);
                        payload.prefix_class = unit->prefix_class;
                        payload.mined_block = unit->mined_block;
                        (void)chain_log_emit(w, st, CHAIN_EVT_SMELT,
                                             &payload, (uint16_t)sizeof(payload));
                    }
                }
            }

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
     * the single module_input[] buffer must not accept those secondaries. */
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
                if (st->module_output[p] < cap * 0.5f) {
                    /* Check what consumers on this station need */
                    commodity_t tag = (commodity_t)st->modules[p].commodity;
                    if (tag >= COMMODITY_COUNT) continue;
                    if (tag < COMMODITY_RAW_ORE_COUNT) {
                        st->module_output[p] = 0.0f;
                        continue;
                    }
                    {
                        commodity_t com = tag;
                        if (st->_inventory_cache[com] <= 0.1f) continue;
                        /* Check if any module on this station actually wants this */
                        bool wanted = false;
                        for (int c = 0; c < st->module_count; c++) {
                            if (c == p || st->modules[c].scaffold) continue;
                            if (module_accepts_input(&st->modules[c], com)) {
                                float c_cap = module_buffer_capacity(st->modules[c].type);
                                if (c_cap > 0.0f && st->module_input[c] < c_cap) {
                                    wanted = true; break;
                                }
                            }
                        }
                        if (!wanted) continue;
                        float pull = fminf(st->_inventory_cache[com], (cap - st->module_output[p]) * 0.5f);
                        if (pull > 0.01f) {
                            st->module_output[p] += pull;
                            output = com; /* remember what we're carrying */
                        }
                    }
                }
                /* Storage output is only a mirror of inventory. If we
                 * can't refresh it with a concrete commodity this tick,
                 * drop any stale residue rather than guessing wrong. */
                if (output == COMMODITY_COUNT) {
                    st->module_output[p] = 0.0f;
                    continue;
                }
            }
            if (st->module_output[p] <= 0.0f) continue;

            /* Find the best consumer (closest, has space) */
            int best_consumer = -1;
            float best_rate = 0.0f;
            for (int c = 0; c < st->module_count; c++) {
                if (c == p) continue;
                if (st->modules[c].scaffold) continue;
                if (!module_accepts_input(&st->modules[c], output)) continue;
                float cap = module_buffer_capacity(st->modules[c].type);
                if (cap <= 0.0f) continue;
                if (st->module_input[c] >= cap) continue;
                float rate = module_flow_rate(st, p, c);
                if (rate > best_rate) {
                    best_rate = rate;
                    best_consumer = c;
                }
            }
            if (best_consumer < 0) continue;

            float room = module_buffer_capacity(st->modules[best_consumer].type)
                       - st->module_input[best_consumer];
            float pull = best_rate * dt;
            if (pull > st->module_output[p]) pull = st->module_output[p];
            if (producer_kind == MODULE_KIND_STORAGE && pull > st->_inventory_cache[output])
                pull = st->_inventory_cache[output];
            if (pull > room) pull = room;
            if (pull > 0.0f) {
                st->module_output[p] -= pull;
                if (producer_kind == MODULE_KIND_STORAGE)
                    st->_inventory_cache[output] -= pull;
                st->module_input[best_consumer] += pull;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Module delivery (docked ship -> scaffold)                           */
/* ------------------------------------------------------------------ */

/* Deliver materials directly to scaffold modules. Materials are consumed
 * immediately from cargo but build progress advances at a fixed rate --
 * delivery fills the module's internal hopper (tracked via build_progress
 * vs the total cost), construction ticks over time in step_module_activation. */
float step_module_delivery(world_t *w, station_t *st, int station_idx,
                           ship_t *ship, commodity_t filter) {
    (void)w; (void)station_idx;
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

        /* Pull from docked ship cargo (consume the manifest unit so the
         * named identity can't be sold or transferred again). The ship
         * is paid the station's buy price for whatever it delivers. */
        if (ship->cargo[mat] > 0.01f) {
            float deliver = fminf(ship->cargo[mat], needed);
            ship->cargo[mat] -= deliver;
            m->build_progress += deliver / cost;
            int whole = (int)floorf(deliver + 0.0001f);
            if (whole > 0) {
                /* Sum prefix-class multipliers across the units we're
                 * about to consume so high-grade ingots pay the player
                 * proportionally. Same per-unit accounting as
                 * try_sell_station_cargo's grade-bonus loop. */
                float price = station_buy_price(st, mat);
                int counted = 0;
                for (uint16_t u = 0; u < ship->manifest.count && counted < whole; u++) {
                    const cargo_unit_t *cu = &ship->manifest.units[u];
                    if (cu->commodity != mat) continue;
                    float mult = mining_payout_multiplier((mining_grade_t)cu->grade);
                    payout += mult * price;
                    counted++;
                }
                if (counted < whole)
                    payout += (float)(whole - counted) * price;
                /* Fractional remainder (sub-unit float) priced at base. */
                float frac = deliver - (float)whole;
                if (frac > 0.0f) payout += frac * price;
                if (manifest_count_by_commodity(&ship->manifest, mat) > 0)
                    (void)ship_manifest_consume_by_commodity(ship, mat, whole);
            } else {
                payout += deliver * station_buy_price(st, mat);
            }
            needed -= deliver;
        }

        /* Also pull from station inventory (NPC deliveries land here).
         * NOT credited as a player payout — the materials were already
         * paid for when they entered the station. Mark the manifest
         * dirty so the multiplayer broadcaster forwards the new station
         * summary; otherwise clients keep showing materials that
         * construction already consumed. */
        if (needed > 0.01f && st->_inventory_cache[mat] > 0.01f) {
            float deliver = fminf(st->_inventory_cache[mat], needed);
            st->_inventory_cache[mat] -= deliver;
            m->build_progress += deliver / cost;
            int whole = (int)floorf(deliver + 0.0001f);
            if (whole > 0) {
                (void)station_manifest_consume_by_commodity(st, mat, whole);
            }
        }

        if (m->build_progress > 1.0f) m->build_progress = 1.0f;
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
                st->module_active_pulse[m] = 1.0f;
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
            actual_minted++;
        }
        if (actual_minted > 0)
            station_finished_sync(st, COMMODITY_REPAIR_KIT);
        st->repair_kit_fab_timer = 0.0f;
        SIM_LOG("[shipyard-fab] station %d minted %d kits (1 frame + 1 laser + 1 tractor consumed)\n",
                s, actual_minted);
    }
}
