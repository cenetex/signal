/*
 * sim_production.c -- Material flow, smelting, and station production.
 * Extracted from game_sim.c.
 */
#include "sim_production.h"
#include "sim_construction.h"  /* module_build_material, module_build_cost */
#include <stdlib.h>            /* abs */

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

/* Mirror produced material into a producer module's output buffer
 * for the flow graph. Capped at schema buffer capacity. */
static void mirror_to_module_output(station_t *st, module_type_t mt, float amount) {
    float cap = module_buffer_capacity(mt);
    if (cap <= 0.0f) return;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type != mt) continue;
        if (st->modules[m].scaffold) continue;
        float room = cap - st->module_output[m];
        if (room <= 0.0f) return;
        float add = (amount > room) ? room : amount;
        st->module_output[m] += add;
        return; /* one module per type per call */
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
/* ------------------------------------------------------------------ */

void sim_step_station_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (station_has_module(st, MODULE_FRAME_PRESS)) {
            if (st->inventory[COMMODITY_FRAME] < MAX_PRODUCT_STOCK) {
                float buf = st->inventory[COMMODITY_FERRITE_INGOT];
                if (buf > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->inventory[COMMODITY_FRAME];
                    float consume = fminf(buf, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->inventory[COMMODITY_FERRITE_INGOT] -= consume;
                    st->inventory[COMMODITY_FRAME] += consume;
                    mirror_to_module_output(st, MODULE_FRAME_PRESS, consume);
                }
            }
        }
        if (station_has_module(st, MODULE_LASER_FAB)) {
            if (st->inventory[COMMODITY_LASER_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_co = st->inventory[COMMODITY_CUPRITE_INGOT];
                if (buf_co > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->inventory[COMMODITY_LASER_MODULE];
                    float consume = fminf(buf_co, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->inventory[COMMODITY_CUPRITE_INGOT] -= consume;
                    st->inventory[COMMODITY_LASER_MODULE] += consume;
                    mirror_to_module_output(st, MODULE_LASER_FAB, consume);
                }
            }
        }
        if (station_has_module(st, MODULE_TRACTOR_FAB)) {
            if (st->inventory[COMMODITY_TRACTOR_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_ln = st->inventory[COMMODITY_CRYSTAL_INGOT];
                if (buf_ln > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->inventory[COMMODITY_TRACTOR_MODULE];
                    float consume = fminf(buf_ln, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->inventory[COMMODITY_CRYSTAL_INGOT] -= consume;
                    st->inventory[COMMODITY_TRACTOR_MODULE] += consume;
                    mirror_to_module_output(st, MODULE_TRACTOR_FAB, consume);
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
            float ore_value = a->ore * station_buy_price(st, a->commodity);

            /* Credit fracturer and tower */
            int tower = (a->last_towed_by >= 0 && a->last_towed_by < MAX_PLAYERS
                         && w->players[a->last_towed_by].connected)
                        ? a->last_towed_by : -1;
            int fracturer = (a->last_fractured_by >= 0 && a->last_fractured_by < MAX_PLAYERS
                             && w->players[a->last_fractured_by].connected)
                            ? a->last_fractured_by : -1;

            if (ore_value > 0.0f) {
                if (tower >= 0 && fracturer >= 0 && tower != fracturer) {
                    float share = ore_value * 0.75f;
                    earn_credits(&w->players[tower].ship, share);
                    emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = tower});
                    earn_credits(&w->players[fracturer].ship, share);
                    emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = fracturer});
                } else {
                    int best_p = (tower >= 0) ? tower : fracturer;
                    if (best_p >= 0) {
                        earn_credits(&w->players[best_p].ship, ore_value);
                        emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = best_p});
                    }
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

/* Adjacency-aware transfer rate between two modules on the same station.
 * Same ring + adjacent slots = fast. Same ring + far slots = medium.
 * Cross-ring = slow trickle (drones will speed this up later, #281). */
static float module_flow_rate(const station_t *st, int producer_idx, int consumer_idx) {
    const station_module_t *p = &st->modules[producer_idx];
    const station_module_t *c = &st->modules[consumer_idx];
    if (p->ring == c->ring && p->ring >= 1) {
        int slot_dist = abs((int)p->slot - (int)c->slot);
        if (slot_dist == 0) slot_dist = 1;
        /* Same ring: 5/sec for adjacent, drops with distance */
        return 5.0f / (float)slot_dist;
    }
    /* Cross-ring or core: trickle (drones will improve this) */
    return 1.0f;
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
            /* Storage modules also push their stored material (treat as output) */
            if (output == COMMODITY_COUNT) {
                module_kind_t k = module_kind(st->modules[p].type);
                if (k == MODULE_KIND_STORAGE) {
                    /* Storage's output commodity is whatever it's holding;
                     * for now we don't track per-storage commodity, so skip
                     * unless the schema declares one. Leave as future work. */
                    continue;
                }
                continue;
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
