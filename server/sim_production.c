/*
 * sim_production.c -- Material flow, smelting, and station production.
 * Extracted from game_sim.c.
 */
#include "sim_production.h"
#include "sim_construction.h"  /* module_build_material, module_build_cost */
#include "mining.h"            /* grade roll at smelt time */
#include <stdlib.h>            /* abs */
#include <math.h>              /* lroundf */

/* ------------------------------------------------------------------ */
/* Smelting helpers                                                    */
/* ------------------------------------------------------------------ */

bool sim_can_smelt_ore(const station_t *st, commodity_t ore) {
    switch (ore) {
        case COMMODITY_FERRITE_ORE: return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE: return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE: return station_has_module(st, MODULE_FURNACE_CR);
        default: return false;
    }
}

/* What ore type a furnace module smelts. Returns -1 for non-furnaces. */
static commodity_t furnace_ore_type(module_type_t mt) {
    switch (mt) {
        case MODULE_FURNACE:    return COMMODITY_FERRITE_ORE;
        case MODULE_FURNACE_CU: return COMMODITY_CUPRITE_ORE;
        case MODULE_FURNACE_CR: return COMMODITY_CRYSTAL_ORE;
        default: return (commodity_t)-1;
    }
}


/* ------------------------------------------------------------------ */
/* Refinery production                                                 */
/* ------------------------------------------------------------------ */

/* Per-furnace smelting: any furnace smelts ore from station inventory into ingots.
 * Rate split across active furnaces to avoid instant consumption. */
void sim_step_refinery_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];

        /* Count active furnaces with ore to smelt */
        int active = 0;
        for (int m = 0; m < st->module_count; m++) {
            module_type_t mt = st->modules[m].type;
            if (mt != MODULE_FURNACE && mt != MODULE_FURNACE_CU && mt != MODULE_FURNACE_CR) continue;
            if (st->modules[m].scaffold) continue;
            commodity_t ore = furnace_ore_type(mt);
            if (ore < 0 || st->inventory[ore] <= 0.01f) continue;
            active++;
        }
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;
        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        /* Smelt per furnace */
        for (int m = 0; m < st->module_count; m++) {
            module_type_t mt = st->modules[m].type;
            if (mt != MODULE_FURNACE && mt != MODULE_FURNACE_CU && mt != MODULE_FURNACE_CR) continue;
            if (st->modules[m].scaffold) continue;
            commodity_t ore = furnace_ore_type(mt);
            if (ore < 0 || st->inventory[ore] <= 0.01f) continue;
            commodity_t ingot = commodity_refined_form(ore);
            float room = MAX_PRODUCT_STOCK - st->inventory[ingot];
            if (room <= 0.01f) continue;
            float consume = fminf(fminf(st->inventory[ore], rate * dt), room);
            st->inventory[ore] -= consume;
            st->inventory[ingot] += consume;
            /* Mirror to module output buffer for the flow graph (#280).
             * Capped at the schema's per-module buffer capacity. */
            float cap = module_buffer_capacity(mt);
            if (cap > 0.0f) {
                float overflow = (st->module_output[m] + consume) - cap;
                float to_buffer = (overflow > 0.0f) ? consume - overflow : consume;
                if (to_buffer > 0.0f) st->module_output[m] += to_buffer;
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
            if (st->modules[m].scaffold) continue;

            const module_schema_t *schema = module_schema(mt);
            if (schema->kind != MODULE_KIND_PRODUCER) continue;
            /* Furnaces handled separately in sim_step_refinery_production */
            if (mt == MODULE_FURNACE || mt == MODULE_FURNACE_CU || mt == MODULE_FURNACE_CR) continue;

            commodity_t input_com = schema->input;
            commodity_t output_com = schema->output;
            if (input_com >= COMMODITY_COUNT || output_com >= COMMODITY_COUNT) continue;
            if (st->inventory[output_com] >= MAX_PRODUCT_STOCK) continue;

            float room = MAX_PRODUCT_STOCK - st->inventory[output_com];
            float rate = schema->rate > 0.0f ? schema->rate : STATION_PRODUCTION_RATE;

            /* Primary: consume from module input buffer (filled by flow graph).
             * This is the fast path — placement-dependent throughput. */
            float produced = 0.0f;
            if (st->module_input[m] > 0.01f) {
                float from_buffer = fminf(st->module_input[m], fminf(rate * dt, room));
                st->module_input[m] -= from_buffer;
                st->inventory[output_com] += from_buffer;
                room -= from_buffer;
                produced += from_buffer;
            }

            /* Fallback: slow trickle from station inventory (0.2x rate).
             * Only kicks in when the flow graph delivered nothing this tick.
             * Prevents total stall for poorly-placed fabs. */
            if (produced < 0.001f && room > 0.01f && st->inventory[input_com] > 0.01f) {
                float fallback_rate = rate * 0.2f;
                float from_inv = fminf(st->inventory[input_com], fminf(fallback_rate * dt, room));
                st->inventory[input_com] -= from_inv;
                st->inventory[output_com] += from_inv;
                produced += from_inv;
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
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Grade roll at fracture (universe-determined, public)                */
/* ------------------------------------------------------------------ */

/* Roll the capped burst against the fragment's fracture_seed using a
 * fixed "universe key" (no per-player input). Grade is a property of
 * the rock itself: every observer sees the same value, the rock's dot
 * color reveals it instantly, theft is for a known prize. Budget is
 * MINING_CANDIDATES_PER_TON × ore_tons. */
static const uint8_t MINING_UNIVERSE_KEY[32] = {
    'R','A','T','i',  '/','o','r','e',  '/','u','n','i', 'v','e','r','s',
    'e','/','v','1',  0,0,0,0,           0,0,0,0,         0,0,0,0
};

mining_grade_t sim_roll_fragment_grade(const asteroid_t *a) {
    (void)a; /* ore tonnage no longer scales burst — flat per-fragment */
    mining_grade_t best = MINING_GRADE_COMMON;
    for (int i = 0; i < MINING_BURST_PER_FRAGMENT; i++) {
        mining_keypair_t kp;
        mining_keypair_derive(a->fracture_seed, MINING_UNIVERSE_KEY, (uint32_t)i, &kp);
        char callsign[8];
        mining_callsign_from_pubkey(kp.pub, callsign);
        mining_grade_t g = mining_classify_base58(callsign);
        if (g > best) best = g;
    }
    return best;
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

            /* Find furnace+target pairs: furnace on one ring, nearest module on next ring */
            for (int m = 0; m < st->module_count && !smelted; m++) {
                if (st->modules[m].scaffold) continue;
                bool is_furnace = (st->modules[m].type == MODULE_FURNACE)
                               || (st->modules[m].type == MODULE_FURNACE_CU)
                               || (st->modules[m].type == MODULE_FURNACE_CR);
                if (!is_furnace) continue;

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

        if (a->smelt_progress >= 1.0f && smelt_station >= 0) {
            station_t *st = &w->stations[smelt_station];
            /* Check for active ore contract — apply premium if one exists */
            float price = station_buy_price(st, a->commodity);
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (w->contracts[k].active
                    && w->contracts[k].action == CONTRACT_TRACTOR
                    && w->contracts[k].station_index == smelt_station
                    && w->contracts[k].commodity == a->commodity) {
                    float cp = contract_price(&w->contracts[k]);
                    if (cp > price) price = cp;
                    break;
                }
            }
            float ore_value = a->ore * price;

            /* Credit fracturer and tower.
             * When neither maps to a connected player (e.g. NPC miners), no
             * credits are issued — NPCs are station infrastructure, not profit
             * centers. The value they produce is ingots in station inventory. */
            int tower = (a->last_towed_by >= 0 && a->last_towed_by < MAX_PLAYERS
                         && w->players[a->last_towed_by].connected)
                        ? a->last_towed_by : -1;
            int fracturer = (a->last_fractured_by >= 0 && a->last_fractured_by < MAX_PLAYERS
                             && w->players[a->last_fractured_by].connected)
                            ? a->last_fractured_by : -1;

            /* Grade was stamped on the asteroid the moment the current
             * tower grabbed it (sim_roll_fragment_grade). Smelt just
             * publishes that grade — no fresh dice. */
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
                if (tower >= 0) {
                    if (w->players[tower].session_ready)
                        ledger_credit_supply(st, w->players[tower].session_token, graded_value);
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_SELL, .player_id = tower,
                        .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                  .base_cr = base_cr, .bonus_cr = bonus_cr }});
                    if (fracturer >= 0 && fracturer != tower) {
                        float finders = graded_value * 0.25f;
                        if (w->players[fracturer].session_ready)
                            ledger_credit_supply(st, w->players[fracturer].session_token, finders);
                        emit_event(w, (sim_event_t){
                            .type = SIM_EVENT_SELL, .player_id = fracturer,
                            .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                      .base_cr = (int)lroundf(finders / bonus_mult),
                                      .bonus_cr = (int)lroundf(finders - finders / bonus_mult) }});
                    }
                } else if (fracturer >= 0) {
                    float half = graded_value * 0.5f;
                    if (w->players[fracturer].session_ready)
                        ledger_credit_supply(st, w->players[fracturer].session_token, half);
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_SELL, .player_id = fracturer,
                        .sell = { .station = smelt_station, .grade = (uint8_t)grade,
                                  .base_cr = (int)lroundf(half / bonus_mult),
                                  .bonus_cr = (int)lroundf(half - half / bonus_mult) }});
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

            /* Smelt: ore -> ingot in station inventory */
            commodity_t ingot = commodity_refined_form(a->commodity);
            if (ingot != a->commodity)
                st->inventory[ingot] += a->ore;
            else
                st->inventory[a->commodity] += a->ore;
            clear_asteroid(a);
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
            if (st->module_output[p] <= 0.0f) continue;
            commodity_t output = module_schema_output(st->modules[p].type);
            /* Storage modules pull from station inventory and push into the
             * flow graph. They feed both furnaces (with ore) and fabs (with
             * ingots), acting as buffers in the production chain. */
            if (output == COMMODITY_COUNT) {
                module_kind_t k = module_kind(st->modules[p].type);
                if (k != MODULE_KIND_STORAGE) continue;

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
                        if (st->inventory[com] <= 0.1f) continue;
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
                        float pull = fminf(st->inventory[com], (cap - st->module_output[p]) * 0.5f);
                        if (pull > 0.01f) {
                            st->inventory[com] -= pull;
                            st->module_output[p] += pull;
                            output = com; /* remember what we're carrying */
                        }
                        break;
                    }
                }
                if (st->module_output[p] <= 0.0f) continue;
                /* If we didn't just pull, we're draining residual buffer.
                 * Use ferrite ore as a fallback output type — consumers
                 * re-check via module_accepts_input anyway. */
                if (output == COMMODITY_COUNT) output = COMMODITY_FERRITE_ORE;
            }

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
            if (pull > room) pull = room;
            if (pull > 0.0f) {
                st->module_output[p] -= pull;
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
void step_module_delivery(world_t *w, station_t *st, int station_idx, ship_t *ship) {
    (void)w; (void)station_idx;
    for (int i = 0; i < st->module_count; i++) {
        if (!st->modules[i].scaffold) continue;
        commodity_t mat = module_build_material(st->modules[i].type);
        float cost = module_build_cost(st->modules[i].type);
        float needed = cost - st->modules[i].build_progress * cost;
        if (needed < 0.01f) continue;

        /* Pull from docked ship cargo */
        if (ship->cargo[mat] > 0.01f) {
            float deliver = fminf(ship->cargo[mat], needed);
            ship->cargo[mat] -= deliver;
            st->modules[i].build_progress += deliver / cost;
            needed -= deliver;
        }

        /* Also pull from station inventory (NPC deliveries land here) */
        if (needed > 0.01f && st->inventory[mat] > 0.01f) {
            float deliver = fminf(st->inventory[mat], needed);
            st->inventory[mat] -= deliver;
            st->modules[i].build_progress += deliver / cost;
        }

        if (st->modules[i].build_progress > 1.0f)
            st->modules[i].build_progress = 1.0f;
    }
}
