/*
 * sim_production.c -- Material flow, smelting, and station production.
 * Extracted from game_sim.c.
 */
#include "sim_production.h"
#include "sim_asteroid.h"      /* fracture_claim_state_reset */
#include "sim_construction.h"  /* module_build_material, module_build_cost */
#include "manifest.h"
#include "mining.h"            /* grade roll at smelt time */
#include "sha256.h"
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
        if (!manifest_remove(&st->manifest, 0, NULL)) return false;
    }
    /* Phase 2: flag dirty on every successful push so the manifest-
     * summary broadcast runs after smelts too (not just rotations). */
    if (manifest_push(&st->manifest, unit)) {
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
    if (st->manifest.count >= st->manifest.cap) return false;
    return manifest_push(&st->manifest, unit);
}

static bool manifest_unit_matches_recipe_input(const cargo_unit_t *unit,
                                               commodity_t commodity) {
    return unit != NULL &&
           (cargo_kind_t)unit->kind == CARGO_KIND_INGOT &&
           (commodity_t)unit->commodity == commodity;
}

static bool station_manifest_select_recipe_inputs(const station_t *st,
                                                  const recipe_def_t *recipe,
                                                  uint16_t out_indices[2],
                                                  cargo_unit_t out_inputs[2]) {
    if (!st || !recipe || !out_indices || !out_inputs ||
        !st->manifest.units || recipe->input_count == 0 || recipe->input_count > 2) {
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
    if (!st || !indices || count == 0 || count > 2) return false;
    if (!st->manifest.units) return false;

    if (count == 2) {
        uint16_t hi = indices[0] > indices[1] ? indices[0] : indices[1];
        uint16_t lo = indices[0] > indices[1] ? indices[1] : indices[0];
        return manifest_remove(&st->manifest, hi, NULL) &&
               manifest_remove(&st->manifest, lo, NULL);
    }

    return manifest_remove(&st->manifest, indices[0], NULL);
}

static bool station_manifest_craft_product(station_t *st, recipe_id_t recipe_id) {
    const recipe_def_t *recipe = recipe_get(recipe_id);
    uint16_t indices[2] = {0, 0};
    cargo_unit_t inputs[2] = {{0}};
    cargo_unit_t product = {0};

    if (!st || !recipe || recipe->input_count == 0 || recipe->input_count > 2) return false;
    if (st->manifest.cap == 0 || st->manifest.units == NULL) {
        if (!station_manifest_bootstrap(st)) return false;
    }
    if (!station_manifest_select_recipe_inputs(st, recipe, indices, inputs)) return false;
    if (!hash_product(recipe_id, inputs, recipe->input_count, 0, &product)) return false;
    if (!station_manifest_consume_selected_inputs(st, indices, recipe->input_count)) return false;
    return station_manifest_push_finished(st, &product);
}

typedef struct {
    recipe_id_t recipe_id;
    commodity_t primary_input;
    float primary_units_per_output;
    commodity_t secondary_input;
    float secondary_units_per_output;
    commodity_t output;
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

    for (size_t i = 0; i < recipe->input_count; i++) {
        commodity_t input = recipe->input_commodities[i];
        if (input == primary) {
            out_recipe->primary_units_per_output += 1.0f;
            continue;
        }
        if (out_recipe->secondary_input == COMMODITY_COUNT ||
            out_recipe->secondary_input == input) {
            out_recipe->secondary_input = input;
            out_recipe->secondary_units_per_output += 1.0f;
            continue;
        }
        return false;
    }

    return out_recipe->primary_units_per_output > 0.0f &&
           out_recipe->output == module_schema_output(mt);
}

/* The count-tier rules live in shared/station_util.c
 * (`station_can_smelt`, `station_furnace_count`) so the client-side
 * dock UI can use the same predicate. The sim wrapper here just adds
 * the existing public symbol so the rest of game_sim.c keeps working. */
bool sim_can_smelt_ore(const station_t *st, commodity_t ore) {
    return station_can_smelt(st, ore);
}


/* ------------------------------------------------------------------ */
/* Refinery production                                                 */
/* ------------------------------------------------------------------ */

/* Per-furnace smelting: any furnace smelts ore from station inventory into ingots.
 * Rate split across active furnaces to avoid instant consumption. */
void sim_step_refinery_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];

        /* Count-tier smelt: each station declares which ores its furnace
         * stack can process via sim_can_smelt_ore. Build the per-tick
         * task list once, then divide the throughput across active
         * (furnace × ore) pairs so a 3-furnace stack doesn't outrun a
         * 1-furnace one on the same ore stockpile. */
        commodity_t smeltable[3] = { COMMODITY_FERRITE_ORE,
                                     COMMODITY_CUPRITE_ORE,
                                     COMMODITY_CRYSTAL_ORE };
        int n_furnaces = station_furnace_count(st);
        if (n_furnaces == 0) continue;
        if (n_furnaces > REFINERY_MAX_FURNACES) n_furnaces = REFINERY_MAX_FURNACES;
        int active_ores = 0;
        for (int oi = 0; oi < 3; oi++) {
            commodity_t ore = smeltable[oi];
            if (!sim_can_smelt_ore(st, ore)) continue;
            if (st->_inventory_cache[ore] <= 0.01f) continue;
            active_ores++;
        }
        if (active_ores == 0) continue;
        /* Total throughput = base × furnace_count, split evenly across
         * the ore types currently smelting. */
        float total_rate = REFINERY_BASE_SMELT_RATE * (float)n_furnaces;
        float rate = total_rate / (float)active_ores;

        /* Round-robin furnace slot indices so each ore stream lands on a
         * distinct furnace's output buffer when more than one is
         * present — the flow graph downstream still sees per-module
         * production deltas. */
        int furnace_slots[REFINERY_MAX_FURNACES];
        int n_slots = 0;
        for (int m = 0; m < st->module_count && n_slots < REFINERY_MAX_FURNACES; m++) {
            if (st->modules[m].type != MODULE_FURNACE) continue;
            if (st->modules[m].scaffold) continue;
            furnace_slots[n_slots++] = m;
        }
        int next_furnace = 0;
        for (int oi = 0; oi < 3; oi++) {
            commodity_t ore = smeltable[oi];
            if (!sim_can_smelt_ore(st, ore)) continue;
            if (st->_inventory_cache[ore] <= 0.01f) continue;
            commodity_t ingot = commodity_refined_form(ore);
            float room = MAX_PRODUCT_STOCK - st->_inventory_cache[ingot];
            if (room <= 0.01f) continue;
            float consume = fminf(fminf(st->_inventory_cache[ore], rate * dt), room);
            st->_inventory_cache[ore] -= consume;
            /* Manifest-as-truth: mint ingot manifest entries at integer
             * crossings (legacy-migrate origin so future inspectors can
             * tell this came from the refinery hopper path, not the
             * fragment-tow path). The float cache is bumped in lockstep
             * inside the helper so the supply strip and BUY check stay
             * aligned with manifest count. */
            uint8_t origin[8] = { 'R','E','F','N','0','0','0','0' };
            origin[7] = (uint8_t)('0' + (s % 10));
            station_finished_accumulate(st, ingot, consume, origin);
            /* Mirror to module output buffer for the flow graph (#280).
             * Round-robin across the furnace slots so each smelt ore
             * has a distinct anchor. */
            int slot_idx = furnace_slots[next_furnace % (n_slots > 0 ? n_slots : 1)];
            next_furnace++;
            float cap = module_buffer_capacity(MODULE_FURNACE);
            if (cap > 0.0f && n_slots > 0) {
                float overflow = (st->module_output[slot_idx] + consume) - cap;
                float to_buffer = (overflow > 0.0f) ? consume - overflow : consume;
                if (to_buffer > 0.0f) st->module_output[slot_idx] += to_buffer;
            }
        }
    }
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
            float stock_before;
            if (input_com >= COMMODITY_COUNT || output_com >= COMMODITY_COUNT) continue;
            if (st->_inventory_cache[output_com] >= MAX_PRODUCT_STOCK) continue;

            stock_before = st->_inventory_cache[output_com];
            float room = MAX_PRODUCT_STOCK - st->_inventory_cache[output_com];
            float rate = schema->rate > 0.0f ? schema->rate : STATION_PRODUCTION_RATE;

            /* Primary: consume from module input buffer (filled by flow graph).
             * This is the fast path — placement-dependent throughput. */
            float produced = 0.0f;
            if (st->module_input[m] > 0.01f) {
                float from_buffer = fminf(rate * dt, room);
                from_buffer = fminf(from_buffer,
                                    st->module_input[m] / recipe.primary_units_per_output);
                if (recipe.secondary_input < COMMODITY_COUNT) {
                    from_buffer = fminf(from_buffer,
                                        st->_inventory_cache[recipe.secondary_input] /
                                        recipe.secondary_units_per_output);
                }
                if (from_buffer > 0.0f) {
                    st->module_input[m] -= from_buffer * recipe.primary_units_per_output;
                    if (recipe.secondary_input < COMMODITY_COUNT)
                        st->_inventory_cache[recipe.secondary_input] -=
                            from_buffer * recipe.secondary_units_per_output;
                    st->_inventory_cache[output_com] += from_buffer;
                    room -= from_buffer;
                    produced += from_buffer;
                }
            }

            /* Fallback: slow trickle from station inventory (0.2x rate).
             * Only kicks in when the flow graph delivered nothing this tick.
             * Prevents total stall for poorly-placed fabs. */
            if (produced < 0.001f && room > 0.01f && st->_inventory_cache[input_com] > 0.01f) {
                float fallback_rate = rate * 0.2f;
                float from_inv = fminf(fallback_rate * dt, room);
                from_inv = fminf(from_inv,
                                 st->_inventory_cache[input_com] / recipe.primary_units_per_output);
                if (recipe.secondary_input < COMMODITY_COUNT) {
                    from_inv = fminf(from_inv,
                                     st->_inventory_cache[recipe.secondary_input] /
                                     recipe.secondary_units_per_output);
                }
                if (from_inv > 0.0f) {
                    st->_inventory_cache[input_com] -= from_inv * recipe.primary_units_per_output;
                    if (recipe.secondary_input < COMMODITY_COUNT)
                        st->_inventory_cache[recipe.secondary_input] -=
                            from_inv * recipe.secondary_units_per_output;
                    st->_inventory_cache[output_com] += from_inv;
                    produced += from_inv;
                }
            }

            /* Mirror produced amount to output buffer for downstream flow */
            if (produced > 0.0f) {
                float cap = module_buffer_capacity(mt);
                if (cap > 0.0f) {
                    float buf_room = cap - st->module_output[m];
                    if (buf_room > 0.0f) {
                        float add = fminf(produced, buf_room);
                        st->module_output[m] += add;
                    }
                }

                /* Dual-write whole finished goods only when the legacy
                 * float path actually crosses an integer boundary and
                 * we can prove provenance from manifest-tracked ingots.
                 *
                 * If craft fails (no input manifest entry to consume), the
                 * float increment must be reverted by the shortfall
                 * amount. Otherwise we leave orphan integer-unit float
                 * with no manifest backing — exactly the drift class
                 * the manifest-as-truth refactor is closing out. */
                {
                    int units_before = (int)floorf(stock_before + 0.0001f);
                    int units_after = (int)floorf(st->_inventory_cache[output_com] + 0.0001f);
                    int manifest_units = units_after - units_before;
                    int crafted = 0;
                    for (int idx = 0; idx < manifest_units; idx++) {
                        if (!station_manifest_craft_product(st, recipe.recipe_id))
                            break;
                        crafted++;
                    }
                    int shortfall = manifest_units - crafted;
                    if (shortfall > 0) {
                        /* Revert the orphan integer crossings. The
                         * fractional residue under stock_before stays
                         * because crafted == 0 case still has a
                         * sub-1.0 accumulator, which is fine. */
                        st->_inventory_cache[output_com] -= (float)shortfall;
                        if (st->_inventory_cache[output_com] < (float)units_before)
                            st->_inventory_cache[output_com] = (float)units_before;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Furnace smelting (fragment hopper pull + smelt)                     */
/* ------------------------------------------------------------------ */

void step_furnace_smelting(world_t *w, float dt) {
    float pull_range = HOPPER_PULL_RANGE;
    float pull_sq = pull_range * pull_range;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;

        int smelt_station = -1;
        bool smelted = false;

        for (int s = 0; s < MAX_STATIONS && !smelted; s++) {
            station_t *st = &w->stations[s];
            if (st->scaffold) continue;

            /* Station-level capability gate: a furnace only fires if
             * the station's count tier covers this commodity. Stops the
             * "Helios with no ferrite capability still pulling ferrite
             * fragments" failure mode under the new rules. */
            if (!sim_can_smelt_ore(st, a->commodity)) continue;

            /* Find furnace+target pairs: any furnace on the station can
             * anchor the beam, paired with the nearest module on an
             * adjacent ring. */
            for (int m = 0; m < st->module_count && !smelted; m++) {
                if (st->modules[m].scaffold) continue;
                if (st->modules[m].type != MODULE_FURNACE) continue;

                /* Output cap: refuse to engage the beam if the station's
                 * ingot stockpile is already full. Without this, smelts
                 * silently overflow (the inventory float clamps at
                 * MAX_PRODUCT_STOCK and the manifest entry vanishes —
                 * including the rare-grade identity the player worked
                 * for). Skipping the beam keeps the fragment free; the
                 * player gets visible feedback that this station can't
                 * accept it (no tractor pull, no smelt) and can route
                 * to a station with room. */
                commodity_t furnace_ingot = commodity_refined_form(a->commodity);
                if (st->_inventory_cache[furnace_ingot] + a->ore > MAX_PRODUCT_STOCK)
                    continue;  /* hopper full — skip this furnace */

                int ring = st->modules[m].ring;
                vec2 furnace_pos = module_world_pos_ring(st, ring, st->modules[m].slot);

                /* Find nearest module on an adjacent ring (inner or outer) */
                vec2 silo_pos = furnace_pos;
                bool has_silo = false;
                float best_d = 1e18f;
                int adj_rings[] = { ring + 1, ring - 1 };
                for (int ri = 0; ri < 2; ri++) {
                    int adj = adj_rings[ri];
                    if (adj < 1 || adj > STATION_NUM_RINGS) continue;
                    for (int m2 = 0; m2 < st->module_count; m2++) {
                        if (st->modules[m2].ring != adj) continue;
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

                /* Pull toward midpoint between furnace and silo — strong pull */
                vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);
                vec2 to_mid = v2_sub(midpoint, a->pos);
                float d_mid = sqrtf(v2_len_sq(to_mid));
                if (d_mid > 0.5f) {
                    float strength = HOPPER_PULL_ACCEL * 1.5f * (1.0f - d_mid / pull_range);
                    vec2 dir = v2_scale(to_mid, 1.0f / d_mid);
                    a->vel = v2_add(a->vel, v2_scale(dir, strength * dt));
                    a->vel = v2_scale(a->vel, 1.0f / (1.0f + 8.0f * dt));
                    float spd = v2_len(a->vel);
                    if (spd > 100.0f) a->vel = v2_scale(a->vel, 100.0f / spd);
                }

                /* Smelt when fragment is close to the midpoint */
                if (d_mid < 80.0f) {
                    smelt_station = s;
                    smelted = true;
                }
            }
        }

        /* If not in any smelt beam this tick, decay progress */
        if (!smelted) {
            if (a->smelt_progress > 0.0f) {
                a->smelt_progress -= dt * 0.5f;
                if (a->smelt_progress < 0.0f) a->smelt_progress = 0.0f;
            }
            continue;
        }

        /* Accumulate smelt progress (~2 seconds to fully smelt) */
        a->smelt_progress += dt * 0.5f;

        /* Hold fragment in place while smelting — dampen velocity */
        a->vel = v2_scale(a->vel, 1.0f / (1.0f + 10.0f * dt));

        /* L8: clamp accumulated progress at 1.0 so a fragment waiting on
         * a fracture-claim resolution doesn't grow smelt_progress unbounded. */
        if (a->smelt_progress > 1.0f) a->smelt_progress = 1.0f;

        if (a->smelt_progress >= 1.0f && smelt_station >= 0) {
            station_t *st = &w->stations[smelt_station];
            fracture_claim_state_t *claim_state = &w->fracture_claims[i];
            /* L11: fill legacy fragment_pub up-front so the compat shim is
             * visible at the top of the block regardless of code flow. */
            if (fragment_pub_is_zero(a))
                smelt_fragment_pub_compat(a);
            /* Wait out active, unresolved fracture claims before paying out. */
            if (claim_state->active && !claim_state->resolved) continue;

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
                if (tower >= 0) {
                    float credited = 0.0f;
                    if (w->players[tower].session_ready)
                        credited = ledger_credit_supply_amount(st, w->players[tower].session_token, graded_value);
                    w->players[tower].ship.stat_credits_earned += credited;
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_SELL, .player_id = tower,
                        .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                  .base_cr = base_cr, .bonus_cr = bonus_cr,
                                  .by_contract = bc }});
                    if (fracturer >= 0 && fracturer != tower) {
                        float finders = graded_value * 0.25f;
                        float fcredited = 0.0f;
                        if (w->players[fracturer].session_ready)
                            fcredited = ledger_credit_supply_amount(st, w->players[fracturer].session_token, finders);
                        w->players[fracturer].ship.stat_credits_earned += fcredited;
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
                    if (w->players[fracturer].session_ready)
                        credited = ledger_credit_supply_amount(st, w->players[fracturer].session_token, half);
                    w->players[fracturer].ship.stat_credits_earned += credited;
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
             * stockpile cap — overshoot is lost (and the manifest dual-
             * write below sees a smaller delta). The player still gets
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
                /* The furnace beam-engagement check (above) guarantees
                 * stock_before + a->ore <= MAX_PRODUCT_STOCK, so the
                 * post-clamp delta = pre-clamp delta. No more silent
                 * overflow → no more "unknown origin" rows. */
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
    /* Producers consume their declared input */
    if (cs->kind == MODULE_KIND_PRODUCER && cs->input == commodity) return true;
    /* Storage modules accept anything they're typed for (input == primary).
     * For now: ore silos and hoppers only accept ore types. */
    if (cs->kind == MODULE_KIND_STORAGE) {
        /* Hopper and ore silo accept any raw ore */
        if (commodity == COMMODITY_FERRITE_ORE ||
            commodity == COMMODITY_CUPRITE_ORE ||
            commodity == COMMODITY_CRYSTAL_ORE) return true;
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
             * flow graph. They feed both furnaces (with ore) and fabs (with
             * ingots), acting as buffers in the production chain. */
            if (output == COMMODITY_COUNT) {
                if (producer_kind != MODULE_KIND_STORAGE) continue;

                float cap = module_buffer_capacity(st->modules[p].type);
                if (cap <= 0.0f) continue;

                /* Scan all commodities that downstream modules want.
                 * Pull the first available one from inventory. */
                if (st->module_output[p] < cap * 0.5f) {
                    /* Check what consumers on this station need */
                    commodity_t feedable[] = {
                        COMMODITY_FERRITE_ORE, COMMODITY_CUPRITE_ORE, COMMODITY_CRYSTAL_ORE,
                        COMMODITY_FERRITE_INGOT, COMMODITY_CUPRITE_INGOT, COMMODITY_CRYSTAL_INGOT
                    };
                    for (int fi = 0; fi < 6; fi++) {
                        commodity_t com = feedable[fi];
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
                        break;
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
void step_module_delivery(world_t *w, station_t *st, int station_idx,
                          ship_t *ship, commodity_t filter) {
    (void)w; (void)station_idx;
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
         * named identity can't be sold or transferred again). */
        if (ship->cargo[mat] > 0.01f) {
            float deliver = fminf(ship->cargo[mat], needed);
            ship->cargo[mat] -= deliver;
            m->build_progress += deliver / cost;
            int whole = (int)floorf(deliver + 0.0001f);
            if (whole > 0)
                manifest_consume_by_commodity(&ship->manifest, mat, whole);
            needed -= deliver;
        }

        /* Also pull from station inventory (NPC deliveries land here).
         * Match the consume on the station manifest too — same drift
         * class as the ship side. Mark the manifest dirty so the
         * multiplayer broadcaster forwards the new station summary;
         * otherwise clients keep showing materials that construction
         * already consumed until some unrelated event triggers a sync. */
        if (needed > 0.01f && st->_inventory_cache[mat] > 0.01f) {
            float deliver = fminf(st->_inventory_cache[mat], needed);
            st->_inventory_cache[mat] -= deliver;
            m->build_progress += deliver / cost;
            int whole = (int)floorf(deliver + 0.0001f);
            if (whole > 0) {
                int drained = manifest_consume_by_commodity(&st->manifest, mat, whole);
                if (drained > 0) st->manifest_dirty = true;
            }
        }

        if (m->build_progress > 1.0f) m->build_progress = 1.0f;
    }
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
        if (st->_inventory_cache[COMMODITY_REPAIR_KIT] >= REPAIR_KIT_STOCK_CAP) continue;

        st->repair_kit_fab_timer += dt;
        if (st->repair_kit_fab_timer < REPAIR_KIT_FAB_PERIOD) continue;

        /* All three inputs required. If any are missing, hold the timer
         * at the period (don't keep accumulating) so the next batch
         * fires the moment supply arrives. */
        if (st->_inventory_cache[COMMODITY_FRAME] < 1.0f ||
            st->_inventory_cache[COMMODITY_LASER_MODULE] < 1.0f ||
            st->_inventory_cache[COMMODITY_TRACTOR_MODULE] < 1.0f) {
            st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
            continue;
        }

        /* Consume one of each from the float + manifest, in lockstep
         * with the manifest-truth invariant. */
        st->_inventory_cache[COMMODITY_FRAME]          -= 1.0f;
        st->_inventory_cache[COMMODITY_LASER_MODULE]   -= 1.0f;
        st->_inventory_cache[COMMODITY_TRACTOR_MODULE] -= 1.0f;
        manifest_consume_by_commodity(&st->manifest, COMMODITY_FRAME, 1);
        manifest_consume_by_commodity(&st->manifest, COMMODITY_LASER_MODULE, 1);
        manifest_consume_by_commodity(&st->manifest, COMMODITY_TRACTOR_MODULE, 1);

        /* Mint kits — clamp at the cap. Each minted unit gets a
         * legacy-migrate manifest entry so the TRADE picker can see
         * them and the per-station origin stays traceable. */
        float room = REPAIR_KIT_STOCK_CAP - st->_inventory_cache[COMMODITY_REPAIR_KIT];
        float minted = fminf(REPAIR_KIT_PER_BATCH, room);
        st->_inventory_cache[COMMODITY_REPAIR_KIT] += minted;
        int int_minted = (int)floorf(minted + 0.0001f);
        if (int_minted > 0) {
            uint8_t origin[8] = { 'D','O','C','K','F','A','B','0' };
            origin[7] = (uint8_t)('0' + (s % 10));
            for (int k = 0; k < int_minted; k++) {
                if (st->manifest.cap == 0 && !station_manifest_bootstrap(st)) break;
                if (st->manifest.count >= st->manifest.cap) break;
                cargo_unit_t unit = {0};
                if (!hash_legacy_migrate_unit(origin, COMMODITY_REPAIR_KIT,
                                              (uint16_t)k, &unit))
                    continue;
                /* Stamp the recipe so future inspectors can see this is a
                 * dock-fab kit rather than a save-migration leftover. */
                unit.recipe_id = (uint16_t)RECIPE_REPAIR_KIT_FAB;
                if (!manifest_push(&st->manifest, &unit)) break;
            }
            st->manifest_dirty = true;
        }
        st->repair_kit_fab_timer = 0.0f;
        SIM_LOG("[shipyard-fab] station %d minted %d kits (1 frame + 1 laser + 1 tractor consumed)\n",
                s, int_minted);
    }
}
