/*
 * sim_autopilot.c — Player autopilot state machine for Signal Space Miner.
 * Extracted from game_sim.c (#272 slice).
 *
 * The autopilot drives a player's ship through a mining loop:
 *   find target → fly → mine → collect → return → dock → sell → launch → repeat
 */
#include "sim_autopilot.h"
#include "sim_nav.h"
#include "sim_flight.h"
#include "signal_model.h"
#include "signal_contract_brain.h"
#include "commodity.h"
#include "manifest.h"
#include "ship.h"
#include "station_util.h"

/* ================================================================== */
/* Player autopilot — server-side AI driving the player's own ship    */
/* ================================================================== */

/* What ore commodity is the player carrying right now? Picks the first
 * towed fragment's commodity. Returns COMMODITY_COUNT when nothing is
 * towed. */
static commodity_t autopilot_towed_commodity(const world_t *w, const server_player_t *sp) {
    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &w->asteroids[idx];
        if (!a->active) continue;
        return a->commodity;
    }
    return COMMODITY_COUNT;
}

static const asteroid_t *autopilot_first_towed_fragment(const world_t *w,
                                                        const server_player_t *sp) {
    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &w->asteroids[idx];
        if (!a->active) continue;
        return a;
    }
    return NULL;
}

/* True if the station can smelt `ore` under the tagged furnace rules. When
 * `ore` is COMMODITY_COUNT (nothing towed), accept any station with at
 * least one furnace + a hopper — we just need somewhere to land. */
static bool station_can_smelt_ore_for_autopilot(const station_t *st, commodity_t ore) {
    if (ore == COMMODITY_COUNT) {
        return station_has_module(st, MODULE_HOPPER) &&
               station_has_module(st, MODULE_FURNACE);
    }
    return station_can_smelt(st, ore);
}

static bool autopilot_furnace_allowed_for_fragment(const asteroid_t *fragment,
                                                   int station_idx,
                                                   int module_idx) {
    if (!fragment) return true;
    if (fragment->commodity != COMMODITY_CRYSTAL_ORE ||
        fragment->crystal_stage != CRYSTAL_STAGE_INTERMEDIATE) {
        return true;
    }
    if (fragment->crystal_stage_station == 0xFFu ||
        fragment->crystal_stage_module == 0xFFu) {
        return true;
    }
    return !(fragment->crystal_stage_station == (uint8_t)station_idx &&
             fragment->crystal_stage_module == (uint8_t)module_idx);
}

/* Compute the smelt-beam drop point for `ore` at `st`: the midpoint of a
 * matching furnace and its nearest adjacent-ring ore hopper. Mirrors the
 * pairing logic in step_furnace_smelting so the autopilot parks where
 * fragments will actually be pulled in. Falls back to the station center
 * when no furnace+silo pair exists. */
static vec2 station_smelt_drop_point(const station_t *st, int station_idx,
                                     commodity_t ore,
                                     const asteroid_t *fragment) {
    vec2 best_mid = st->pos;
    float best_silo_d = 1e18f;
    bool found = false;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].scaffold) continue;
        if (st->modules[m].type != MODULE_FURNACE) continue;
        commodity_t furnace_ore = module_instance_input_ore(&st->modules[m]);
        if (ore != COMMODITY_COUNT && furnace_ore != ore) continue;
        if (!autopilot_furnace_allowed_for_fragment(fragment, station_idx, m)) continue;
        int ring = st->modules[m].ring;
        vec2 furnace_pos = module_world_pos_ring(st, ring, st->modules[m].slot);
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int m2 = 0; m2 < st->module_count; m2++) {
                if (st->modules[m2].ring != adj) continue;
                if (st->modules[m2].scaffold) continue;
                if (st->modules[m2].type != MODULE_HOPPER) continue;
                if ((commodity_t)st->modules[m2].commodity != furnace_ore) continue;
                vec2 mp2 = module_world_pos_ring(st, adj, st->modules[m2].slot);
                float d = v2_dist_sq(furnace_pos, mp2);
                if (d < best_silo_d) {
                    best_silo_d = d;
                    best_mid = v2_scale(v2_add(furnace_pos, mp2), 0.5f);
                    found = true;
                }
            }
        }
    }
    return found ? best_mid : st->pos;
}

/* Find the nearest active station with a dock and a furnace that can
 * smelt the player's currently-towed ore. When nothing is towed, falls
 * back to any dock+furnace station so the ship still has somewhere to
 * land for repair / find-target reset. */
static int autopilot_find_refinery(const world_t *w, const server_player_t *sp) {
    commodity_t ore = autopilot_towed_commodity(w, sp);
    int best = -1;
    float best_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        if (!station_has_module(st, MODULE_DOCK)) continue;
        if (!station_can_smelt_ore_for_autopilot(st, ore)) continue;
        float d = v2_dist_sq(sp->ship.pos, st->pos);
        if (d < best_d) { best_d = d; best = s; }
    }
    /* Last-resort fallback: any dock+any-furnace station. Keeps damaged
     * ships with empty tow able to limp home even if no station matches
     * the commodity filter. */
    if (best < 0) {
        for (int s = 0; s < MAX_STATIONS; s++) {
            const station_t *st = &w->stations[s];
            if (!station_is_active(st)) continue;
            if (!station_has_module(st, MODULE_DOCK)) continue;
            if (!station_can_smelt_ore_for_autopilot(st, COMMODITY_COUNT)) continue;
            float d = v2_dist_sq(sp->ship.pos, st->pos);
            if (d < best_d) { best_d = d; best = s; }
        }
    }
    return best;
}


static bool autopilot_can_mine_asteroid(const server_player_t *sp, const asteroid_t *a) {
    if (!a->active || asteroid_is_collectible(a)) return false;
    return (int)a->tier >= (int)max_mineable_tier(sp->ship.mining_level);
}

static bool autopilot_clear_mining_approach(const world_t *w, const server_player_t *sp,
                                            const asteroid_t *a) {
    const hull_def_t *hull = ship_hull_def(&sp->ship);
    vec2 from_rock = v2_sub(sp->ship.pos, a->pos);
    float from_len = v2_len(from_rock);
    if (from_len < 1.0f) return true;
    vec2 outward = v2_scale(from_rock, 1.0f / from_len);
    vec2 approach = v2_add(a->pos, v2_scale(outward, a->radius + 120.0f));
    return nav_segment_clear(w, sp->ship.pos, approach, hull->ship_radius + 30.0f);
}

static bool autopilot_station_prefers_asteroid(const world_t *w,
                                               const server_player_t *sp,
                                               const asteroid_t *a) {
    if (!w || !sp || !a) return false;
    int s = sp->current_station;
    if (s < 0 || s >= MAX_STATIONS) return false;
    const station_t *st = &w->stations[s];
    if (!station_is_active(st)) return false;
    if (!station_has_module(st, MODULE_DOCK)) return false;
    return station_can_smelt_ore_for_autopilot(st, a->commodity);
}

/* Pick the most autopilot-friendly mining target.
 *
 * Priority order:
 *   0. Nearest mineable rock that the player's current/home station can smelt
 *   1. Nearest mineable rock with a clear direct approach
 *   2. Nearest mineable rock even if the final approach is cluttered
 * Fragments are NOT targeted — the tractor auto-collects nearby ones
 * during flight. Explicitly targeting fragments caused orbit loops.
 */
static int autopilot_find_mining_target(const world_t *w, const server_player_t *sp) {
    int best = -1;
    float best_d = 1e18f;

    /* Pass 0 removed: don't explicitly target fragments. The tractor
     * auto-collects nearby fragments during flight. Targeting fragments
     * caused the ship to orbit them endlessly near stations when the
     * tractor couldn't grab them (chase → timeout → re-target loop). */

    /* Pass 0: when launched from a station with matching smelters, prefer
     * ore that can come back to that station. This keeps bot traffic from
     * collapsing into the ferrite-only refinery when other stations can
     * process higher-tier ores. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (!autopilot_station_prefers_asteroid(w, sp, a)) continue;
        if (signal_strength_at(w, a->pos) < 0.5f) continue;
        if (!autopilot_clear_mining_approach(w, sp, a)) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) return best;

    /* Pass 1: nearest mineable rock with a clear final approach. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (signal_strength_at(w, a->pos) < 0.5f) continue;
        if (!autopilot_clear_mining_approach(w, sp, a)) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best >= 0) return best;

    /* Pass 2 removed: same fragment-orbiting issue as pass 0. */

    /* Pass 3: any mineable rock — A* can still get there, but this is
     * lower priority than rocks we can work cleanly or fragments already
     * nearby. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!autopilot_can_mine_asteroid(sp, a)) continue;
        if (signal_strength_at(w, a->pos) < 0.5f) continue;
        float d = v2_dist_sq(sp->ship.pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* Tractor capacity = 2 + 2 × tractor_level (2/4/6/8/10).
 * The autopilot's mining loop is bounded by THIS, not by cargo capacity:
 * mined fragments live in the tow chain, not ship.cargo, and only
 * become credits when smelted at a station's furnace. */
/* True if the ship is damaged enough that the autopilot should bail
 * out of mining and return for repair. Also returns true any time
 * we've ALREADY started returning (so the threshold doesn't oscillate
 * if the hull regenerates back to 80%+ momentarily). */
static bool autopilot_needs_repair(const ship_t *s) {
    float max = ship_max_hull(s);
    if (max <= 0.0f) return false;
    return (s->hull / max) < 0.80f;
}

static bool autopilot_hull_full(const ship_t *s) {
    float max = ship_max_hull(s);
    if (max <= 0.0f) return true;
    return s->hull >= max - 0.5f;
}

static bool autopilot_logistics_enabled(const server_player_t *sp) {
    return sp && sp->server_brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT;
}

static void autopilot_clear_logistics(server_player_t *sp) {
    sp->autopilot_station_target = -1;
    sp->autopilot_cargo = COMMODITY_COUNT;
}

static bool autopilot_valid_dock_station(const world_t *w, int station_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    const station_t *st = &w->stations[station_idx];
    return station_is_active(st) && station_has_module(st, MODULE_DOCK);
}

static bool autopilot_finished_good(commodity_t c) {
    return c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT;
}

static bool autopilot_ship_has_finished(const server_player_t *sp, commodity_t c) {
    return sp && autopilot_finished_good(c) &&
           ship_finished_count(&sp->ship, c) > 0;
}

static bool autopilot_first_ship_finished(const server_player_t *sp,
                                          commodity_t *out) {
    if (!sp) return false;
    for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
        commodity_t c = (commodity_t)i;
        if (ship_finished_count(&sp->ship, c) > 0) {
            if (out) *out = c;
            return true;
        }
    }
    return false;
}

static float autopilot_ledger_balance(const station_t *st,
                                      const server_player_t *sp) {
    if (!st || !sp) return 0.0f;
    return server_player_can_use_pubkey_persistence(sp)
        ? ledger_balance_by_pubkey(st, sp->pubkey)
        : ledger_balance(st, sp->session_token);
}

static float autopilot_hull_ratio(const ship_t *s) {
    float max = ship_max_hull(s);
    if (max <= 0.0f) return 1.0f;
    float ratio = s->hull / max;
    if (ratio < 0.0f) return 0.0f;
    if (ratio > 1.0f) return 1.0f;
    return ratio;
}

static bool autopilot_has_delivery_demand(const world_t *w,
                                          int station_idx,
                                          commodity_t c) {
    if (!autopilot_valid_dock_station(w, station_idx) ||
        !autopilot_finished_good(c)) {
        return false;
    }
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index != station_idx) continue;
        if (ct->commodity != c) continue;
        if (ct->quantity_needed <= 0.01f) continue;
        return true;
    }
    return station_consumes(&w->stations[station_idx], c);
}

static float autopilot_contract_score(const world_t *w,
                                      const server_player_t *sp,
                                      const contract_t *ct) {
    if (!w || !sp || !ct) return -1.0f;
    if (!ct->active || ct->action != CONTRACT_TRACTOR) return -1.0f;
    if (!autopilot_finished_good(ct->commodity)) return -1.0f;
    if (!autopilot_valid_dock_station(w, ct->station_index)) return -1.0f;
    if (ct->quantity_needed <= 0.01f) return -1.0f;
    float dist = sqrtf(v2_dist_sq(sp->ship.pos,
                                  w->stations[ct->station_index].pos));
    return contract_price(ct) / fmaxf(1.0f, dist / 1000.0f);
}

static bool autopilot_source_can_sell_to_bot(const world_t *w,
                                             const server_player_t *sp,
                                             int source_station,
                                             commodity_t c) {
    if (!autopilot_valid_dock_station(w, source_station) ||
        !autopilot_finished_good(c)) {
        return false;
    }
    const station_t *src = &w->stations[source_station];
    if (!station_produces(src, c)) return false;
    if (station_finished_count(src, c) <= 0) return false;
    float free_volume = ship_cargo_capacity(&sp->ship) - ship_total_cargo(&sp->ship);
    if (free_volume + 0.0001f < commodity_volume(c)) return false;
    float price = station_sell_price(src, c);
    if (price <= 0.01f) return false;
    return autopilot_ledger_balance(src, sp) + 0.01f >= price;
}

static void autopilot_make_contract_candidate(
    const world_t *w,
    const server_player_t *sp,
    signal_contract_action_t action,
    int source_station,
    int dest_station,
    commodity_t cargo,
    const contract_t *ct,
    float teacher_score,
    signal_contract_candidate_t *out) {
    memset(out, 0, sizeof(*out));
    const station_t *src = (source_station >= 0 && source_station < MAX_STATIONS)
        ? &w->stations[source_station]
        : NULL;
    const station_t *dst = (dest_station >= 0 && dest_station < MAX_STATIONS)
        ? &w->stations[dest_station]
        : NULL;
    out->action = action;
    out->source_station = source_station;
    out->dest_station = dest_station;
    out->commodity = cargo;
    out->quantity_needed = ct ? ct->quantity_needed : 0.0f;
    out->contract_price = ct ? contract_price(ct) : (dst ? station_buy_price(dst, cargo) : 0.0f);
    out->source_price = src ? station_sell_price(src, cargo) : 0.0f;
    out->source_stock = src ? (float)station_finished_count(src, cargo) : 0.0f;
    out->dest_stock = dst ? (float)station_finished_count(dst, cargo) : 0.0f;
    out->ledger_balance = src ? autopilot_ledger_balance(src, sp) : 0.0f;
    out->free_cargo = ship_cargo_capacity(&sp->ship) - ship_total_cargo(&sp->ship);
    out->distance = (dst && source_station != dest_station)
        ? sqrtf(v2_dist_sq(sp->ship.pos, dst->pos))
        : 0.0f;
    out->age = ct ? ct->age : 0.0f;
    out->hull_ratio = autopilot_hull_ratio(&sp->ship);
    out->teacher_score = teacher_score;
}

static int autopilot_append_contract_candidates(
    const world_t *w,
    const server_player_t *sp,
    int source_station,
    signal_contract_candidate_t *candidates,
    int cap) {
    if (!autopilot_logistics_enabled(sp) ||
        !autopilot_valid_dock_station(w, source_station) ||
        sp->ship.towed_count > 0 ||
        cap <= 0) {
        return 0;
    }

    int count = 0;
    commodity_t held = COMMODITY_COUNT;
    if (autopilot_first_ship_finished(sp, &held) &&
        autopilot_has_delivery_demand(w, source_station, held)) {
        const contract_t *best_ct = NULL;
        float best_price = 0.0f;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            const contract_t *ct = &w->contracts[k];
            if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
            if (ct->station_index != source_station || ct->commodity != held) continue;
            if (ct->quantity_needed <= 0.01f) continue;
            float price = contract_price(ct);
            if (!best_ct || price > best_price) {
                best_ct = ct;
                best_price = price;
            }
        }
        autopilot_make_contract_candidate(
            w, sp, SIGNAL_CONTRACT_ACTION_DELIVER_LOCAL,
            source_station, source_station, held, best_ct,
            10000.0f + best_price,
            &candidates[count++]);
    }

    const station_t *src = &w->stations[source_station];
    for (int k = 0; k < MAX_CONTRACTS && count < cap; k++) {
        const contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index == source_station) continue;
        if (!autopilot_finished_good(ct->commodity)) continue;
        if (!station_produces(src, ct->commodity)) continue;

        float score = autopilot_contract_score(w, sp, ct);
        if (score < 0.0f) continue;
        if (station_finished_count(src, ct->commodity) > 0) {
            if (!autopilot_source_can_sell_to_bot(w, sp, source_station, ct->commodity)) {
                continue;
            }
            autopilot_make_contract_candidate(
                w, sp, SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER,
                source_station, ct->station_index, ct->commodity, ct,
                score,
                &candidates[count++]);
        } else {
            autopilot_make_contract_candidate(
                w, sp, SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK,
                source_station, ct->station_index, ct->commodity, ct,
                score * 0.55f,
                &candidates[count++]);
        }
    }

    return count;
}

static bool autopilot_plan_docked_logistics(world_t *w, server_player_t *sp) {
    if (!autopilot_logistics_enabled(sp) ||
        !sp->docked ||
        !autopilot_valid_dock_station(w, sp->current_station)) {
        return false;
    }

    signal_contract_candidate_t candidates[MAX_CONTRACTS + 1];
    int count = autopilot_append_contract_candidates(
        w, sp, sp->current_station, candidates, MAX_CONTRACTS + 1);
    int choice = signal_contract_brain_choose(w, sp, candidates, count);
    if (choice < 0 || choice >= count) {
        autopilot_clear_logistics(sp);
        return false;
    }

    const signal_contract_candidate_t *picked = &candidates[choice];
    commodity_t cargo = picked->commodity;
    switch (picked->action) {
    case SIGNAL_CONTRACT_ACTION_DELIVER_LOCAL:
        sp->autopilot_station_target = sp->current_station;
        sp->autopilot_cargo = cargo;
        sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_DELIVER;
        sp->autopilot_timer = 0.0f;
        return true;
    case SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER:
        sp->autopilot_station_target = picked->dest_station;
        sp->autopilot_cargo = cargo;
        sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_BUY;
        sp->autopilot_timer = 0.0f;
        return true;
    case SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK:
        sp->autopilot_station_target = picked->dest_station;
        sp->autopilot_cargo = cargo;
        sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_WAIT;
        sp->autopilot_timer = 0.0f;
        return true;
    case SIGNAL_CONTRACT_ACTION_NONE:
    default:
        break;
    }

    autopilot_clear_logistics(sp);
    return false;
}

/* Drive the player's ship via simulated input. The autopilot writes
 * sp->input each tick, and the existing physics/mining/dock systems
 * consume those intents like they would for a human player. */
void step_autopilot(world_t *w, server_player_t *sp, float dt) {
    if (sp->autopilot_mode == 0) return;

    sp->autopilot_timer += dt;

    /* Stuck detection: if the ship hasn't moved >50u in 8 seconds
     * while in a transit state, pick a new target. This breaks
     * deadlocks where avoidance oscillates against station walls. */
    if (sp->autopilot_state == AUTOPILOT_STEP_FLY_TO_TARGET ||
        sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY ||
        sp->autopilot_state == AUTOPILOT_STEP_LOGISTICS_TRAVEL) {
        float moved = v2_dist_sq(sp->ship.pos, sp->autopilot_last_pos);
        if (moved > 50.0f * 50.0f) {
            sp->autopilot_last_pos = sp->ship.pos;
            sp->autopilot_stuck_timer = 0.0f;
        } else {
            sp->autopilot_stuck_timer += dt;
            if (sp->autopilot_stuck_timer > 8.0f) {
                SIM_LOG("[autopilot] player %d stuck for 8s, re-planning\n", sp->id);
                bool keep_target = false;
                /* If carrying fragments, stay in RETURN_TO_REFINERY but
                 * force a path recompute (clear path age). Don't abandon
                 * the delivery — that causes the ship to tow rocks away
                 * from the station toward a new mining target. */
                if (sp->ship.towed_count > 0 &&
                    sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY) {
                    nav_force_replan(nav_player_path(sp->id));
                    keep_target = true;
                } else if (sp->autopilot_state == AUTOPILOT_STEP_LOGISTICS_TRAVEL &&
                           sp->autopilot_station_target >= 0 &&
                           sp->autopilot_cargo < COMMODITY_COUNT) {
                    nav_force_replan(nav_player_path(sp->id));
                    keep_target = true;
                } else {
                    sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
                }
                if (!keep_target) {
                    sp->autopilot_target = -1;
                    autopilot_clear_logistics(sp);
                }
                sp->autopilot_timer = 0.0f;
                sp->autopilot_stuck_timer = 0.0f;
                sp->autopilot_last_pos = sp->ship.pos;
            }
        }
    } else {
        sp->autopilot_last_pos = sp->ship.pos;
        sp->autopilot_stuck_timer = 0.0f;
    }

    /* Damage check: if hull dropped below 80%, bail out of mining and
     * return to a refinery for repair. The ship will hold in dock
     * until hull is at 100% before relaunching (handled in SELL state).
     * Skip the bail if we're already heading home or docked. */
    if (autopilot_needs_repair(&sp->ship) &&
        sp->autopilot_state != AUTOPILOT_STEP_RETURN_TO_REFINERY &&
        sp->autopilot_state != AUTOPILOT_STEP_DOCK &&
        sp->autopilot_state != AUTOPILOT_STEP_SELL &&
        sp->autopilot_state != AUTOPILOT_STEP_LAUNCH &&
        sp->autopilot_state != AUTOPILOT_STEP_LOGISTICS_BUY &&
        sp->autopilot_state != AUTOPILOT_STEP_LOGISTICS_TRAVEL &&
        sp->autopilot_state != AUTOPILOT_STEP_LOGISTICS_DOCK &&
        sp->autopilot_state != AUTOPILOT_STEP_LOGISTICS_DELIVER &&
        sp->autopilot_state != AUTOPILOT_STEP_LOGISTICS_WAIT) {
        sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
        sp->autopilot_target = -1;
        autopilot_clear_logistics(sp);
        sp->autopilot_timer = 0.0f;
    }

    /* Tractor management: ON when mining, collecting, or hauling home.
     * OFF during FIND_TARGET and FLY_TO_TARGET (transit without cargo).
     * RETURN_TO_REFINERY keeps tractor ON so spring physics pull towed
     * fragments along — this matters when the user toggles autopilot ON
     * while already carrying fragments. */
    /* Autopilot holds tractor during mining/collecting/returning with cargo */
    sp->input.tractor_hold =
        (sp->autopilot_state == AUTOPILOT_STEP_MINE ||
         sp->autopilot_state == AUTOPILOT_STEP_COLLECT ||
         (sp->autopilot_state == AUTOPILOT_STEP_RETURN_TO_REFINERY &&
          sp->ship.towed_count > 0));

    /* Mode 1: mining loop. */
    switch (sp->autopilot_state) {
    case AUTOPILOT_STEP_FIND_TARGET: {
        if (sp->docked) {
            if (autopilot_plan_docked_logistics(w, sp)) {
                break;
            }
            sp->input.interact = true; /* launch */
            sp->autopilot_state = AUTOPILOT_STEP_LAUNCH;
            break;
        }
        /* Carrying fragments means the next objective is a furnace/hopper
         * beam corridor, not another mining target or the station dock. */
        if (sp->ship.towed_count > 0) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        int t = autopilot_find_mining_target(w, sp);
        if (t < 0) {
            /* Nothing minable — if near a station, dock and wait.
             * Otherwise head to the refinery. This prevents the
             * FIND→RETURN→FIND oscillation loop. */
            if (sp->in_dock_range && sp->nearby_station >= 0) {
                sp->input.interact = true; /* dock */
                sp->autopilot_state = AUTOPILOT_STEP_SELL;
                sp->autopilot_timer = 0.0f;
            } else {
                sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            }
            break;
        }
        sp->autopilot_target = t;
        sp->autopilot_state = AUTOPILOT_STEP_FLY_TO_TARGET;
        sp->autopilot_timer = 0.0f;
        /* Compute A* path to the mining target */
        nav_find_path(w, sp->ship.pos, w->asteroids[t].pos,
                      ship_hull_def(&sp->ship)->ship_radius + 30.0f,
                      nav_player_path(sp->id));
        break;
    }
    case AUTOPILOT_STEP_FLY_TO_TARGET: {
        if (sp->autopilot_target < 0 || sp->autopilot_target >= MAX_ASTEROIDS) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        const asteroid_t *a = &w->asteroids[sp->autopilot_target];
        if (!a->active || asteroid_is_collectible(a)) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        /* Bail to delivery if carrying fragments. */
        if (sp->ship.towed_count > 0) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        /* Don't fly into weak signal — the target may have drifted. */
        if (signal_strength_at(w, sp->ship.pos) < 0.5f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            break;
        }
        /* Also check the target's signal — don't fly to a dead zone. */
        if (signal_strength_at(w, a->pos) < 0.3f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            break;
        }
        /* Standoff distance: where the autopilot wants the ship to "park"
         * relative to the asteroid surface. Laser MINING_RANGE is 170u
         * so anywhere within radius+170 reaches; we sit at radius+120
         * which gives ~100u of clearance from the surface (after the
         * 16u ship_radius) — enough to absorb fracture spawn velocity,
         * tracking jitter, and gravity perturbations without grinding
         * the hull on the rock. Fragments (S-tier) need to be at the
         * ship itself for tractor pickup. */
        float standoff = (a->tier == ASTEROID_TIER_S)
            ? 0.0f
            : (a->radius + 120.0f);
        float dist_to_a = sqrtf(v2_dist_sq(sp->ship.pos, a->pos));
        float effective_dist = fmaxf(0.0f, dist_to_a - standoff);

        /* Transition to MINE/COLLECT once close enough AND slow enough
         * for the hover controller to manage. 30 u/s prevents the
         * overshoot-through-asteroid cycle. */
        float current_speed = sqrtf(v2_len_sq(sp->ship.vel));
        if (effective_dist < 30.0f && current_speed < 30.0f) {
            sp->input.thrust = 0.0f;
            if (a->tier == ASTEROID_TIER_S) {
                sp->autopilot_state = AUTOPILOT_STEP_COLLECT;
            } else {
                sp->autopilot_state = AUTOPILOT_STEP_MINE;
            }
            sp->autopilot_timer = 0.0f;
            break;
        }

        /* Follow A* path via flight controller. */
        nav_path_t *path = nav_player_path(sp->id);
        flight_cmd_t cmd = flight_steer_to(w, &sp->ship, path, a->pos,
                                            standoff, 150.0f, dt);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
        sp->input.mine = false;

        /* Stuck-fly safety: if we've been flying >60s and haven't arrived,
         * pick a new target. */
        if (sp->autopilot_timer > 60.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
        }
        break;
    }
    case AUTOPILOT_STEP_MINE: {
        if (sp->autopilot_target < 0 || sp->autopilot_target >= MAX_ASTEROIDS) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        /* Don't mine while carrying fragments. */
        if (sp->ship.towed_count > 0) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const asteroid_t *a = &w->asteroids[sp->autopilot_target];
        if (!a->active || asteroid_is_collectible(a)) {
            /* Asteroid fractured, vanished, or became a fragment
             * (slot recycled). Re-evaluate. */
            sp->autopilot_state = AUTOPILOT_STEP_COLLECT;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* Hover near the rock at a safe standoff via flight controller. */
        float standoff = a->radius + 120.0f;
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, a->pos));

        /* If we drifted way out, return to FLY_TO_TARGET. */
        if (dist > standoff + 30.0f + 200.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FLY_TO_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* If gravity dragged us (and the asteroid) out of signal,
         * abandon this rock and find a new target closer to home. */
        if (signal_strength_at(w, sp->ship.pos) < 0.5f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }

        flight_cmd_t cmd = flight_hover_near(w, &sp->ship, a->pos, standoff);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
        /* Mine when roughly facing the rock within mining range.
         * MINING_RANGE is 170u, so anything within standoff+50 works.
         * Angle threshold widened to 0.35 rad (~20°) to prevent the
         * proportional turn from oscillating past the fire window. */
        vec2 to_a = v2_sub(a->pos, sp->ship.pos);
        float face = atan2f(to_a.y, to_a.x);
        float diff = wrap_angle(face - sp->ship.angle);
        if (dist < standoff + 50.0f && fabsf(diff) < 0.35f) {
            sp->input.mine = true;
            sp->input.mining_target_hint = sp->autopilot_target;
        } else {
            sp->input.mine = false;
        }
        break;
    }
    case AUTOPILOT_STEP_COLLECT: {
        /* Sweep nearby fragments only — DON'T chase fragments across
         * the world. The COLLECT state is for the cluster spawned by
         * the rock we just fractured. Bail out the instant the tractor
         * is full OR nothing's nearby OR we've been loitering too long. */
        sp->input.tractor_hold = true;
        sp->input.mine = false;
        /* Signal check — don't collect in weak signal. */
        if (signal_strength_at(w, sp->ship.pos) < 0.5f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        /* Carrying fragments = go deliver. */
        if (sp->ship.towed_count > 0) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const float collect_range_sq = 600.0f * 600.0f;
        int best = -1;
        float best_d = 1e18f;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            const asteroid_t *a = &w->asteroids[i];
            if (!a->active || !asteroid_is_collectible(a)) continue;
            float d = v2_dist_sq(sp->ship.pos, a->pos);
            if (d > collect_range_sq) continue;
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best < 0) {
            /* No more fragments in range. If we're carrying anything,
             * dump it at the nearest refinery; otherwise look for a
             * new mining target. */
            sp->autopilot_state = (sp->ship.towed_count > 0)
                ? AUTOPILOT_STEP_RETURN_TO_REFINERY
                : AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }
        const asteroid_t *frag = &w->asteroids[best];
        vec2 to = v2_sub(frag->pos, sp->ship.pos);
        float desired = atan2f(to.y, to.x);
        sp->input.turn = flight_face_heading(&sp->ship, desired);
        float diff = wrap_angle(desired - sp->ship.angle);
        sp->input.thrust = (cosf(diff) > 0.5f) ? 0.6f : 0.0f;
        if (sp->autopilot_timer > 8.0f) {
            sp->autopilot_state = (sp->ship.towed_count > 0)
                ? AUTOPILOT_STEP_RETURN_TO_REFINERY
                : AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_RETURN_TO_REFINERY: {
        if (sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_SELL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        int s = autopilot_find_refinery(w, sp);
        if (s < 0) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            break;
        }
        const station_t *st = &w->stations[s];
        sp->autopilot_target = s;
        /* Damage routing: if hull is below the repair threshold, this
         * is a "dock for repair" run; we approach the dock berth and
         * trigger interact when close. Otherwise it's a "drop fragments
         * at the hopper" run; we park at the smelt drop point — the
         * midpoint of the matching furnace + adjacent-ring silo — so
         * fragments actually fall inside both pull radii. Targeting
         * st->pos is wrong: with a station whose furnace+silo are off
         * to one side, the spring tow keeps fragments at center and
         * the silo never reaches them. */
        commodity_t towed_ore = autopilot_towed_commodity(w, sp);
        const asteroid_t *towed_fragment = autopilot_first_towed_fragment(w, sp);
        bool hauling_fragment = sp->ship.towed_count > 0 &&
                                towed_ore < COMMODITY_COUNT &&
                                towed_fragment != NULL;
        bool need_repair = autopilot_needs_repair(&sp->ship) && !hauling_fragment;
        vec2 smelt_pt = station_smelt_drop_point(st, s, towed_ore, towed_fragment);

        vec2 fly_target = need_repair
            ? station_approach_target(st, sp->ship.pos)
            : smelt_pt;
        nav_path_t *path = nav_player_path(sp->id);
        flight_cmd_t cmd = flight_steer_to(w, &sp->ship, path, fly_target,
                                            need_repair ? 0.0f : 80.0f, 120.0f, dt);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
        sp->input.mine = false;
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, smelt_pt));

        /* Drop-and-leave path (no damage): once the smelter has consumed
         * everything we towed in, head back out for another load. The
         * furnace pulls fragments in while we hold position at the
         * smelt point above. */
        if (!need_repair && sp->ship.towed_count == 0 && dist < 500.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
            break;
        }

        /* Damage path: dock for passive heal once close enough. */
        if (need_repair && dist < DOCK_APPROACH_RANGE && sp->in_dock_range) {
            sp->input.interact = true;
            sp->autopilot_state = AUTOPILOT_STEP_DOCK;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_DOCK: {
        if (sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_SELL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->autopilot_timer > 6.0f) {
            /* Approach didn't snap. Re-issue interact and re-aim. */
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_SELL: {
        if (!sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
            break;
        }
        /* Repeatedly trigger sell + repair while docked. service_repair
         * is harmless when hull is already full; with kit-based repair
         * it consumes one kit per HP every tick the action fires. */
        sp->input.service_sell = true;
        sp->input.service_sell_only = COMMODITY_COUNT;
        sp->input.service_repair = true;
        if (sp->autopilot_timer < 0.6f) break;

        /* Wait until either hull is full OR no kits are available
         * anywhere (cargo + this station's inventory). The latter
         * prevents an infinite wait when the supply chain hasn't
         * delivered kits to this dock and the player isn't carrying
         * any — better to launch with damage than to idle forever. */
        const station_t *st = &w->stations[sp->current_station];
        int ship_kits = ship_finished_count(&sp->ship, COMMODITY_REPAIR_KIT);
        int station_kits = station_finished_count(st, COMMODITY_REPAIR_KIT);
        bool any_kits = (ship_kits + station_kits) > 0;
        if (!autopilot_hull_full(&sp->ship) && any_kits) {
            /* Stay docked; repair will keep ticking from cargo first
             * then station inventory. */
            break;
        }
        if (autopilot_plan_docked_logistics(w, sp)) {
            break;
        }
        /* Hull repaired (or unrepairable) AND cargo sold — launch. */
        sp->input.interact = true;
        sp->autopilot_state = AUTOPILOT_STEP_LAUNCH;
        sp->autopilot_timer = 0.0f;
        break;
    }
    case AUTOPILOT_STEP_LOGISTICS_BUY: {
        int source = sp->current_station;
        commodity_t cargo = sp->autopilot_cargo;
        if (!sp->docked ||
            !autopilot_valid_dock_station(w, source) ||
            !autopilot_valid_dock_station(w, sp->autopilot_station_target) ||
            !autopilot_finished_good(cargo)) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        sp->input.service_repair = true;
        if (autopilot_ship_has_finished(sp, cargo)) {
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_TRAVEL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (!autopilot_source_can_sell_to_bot(w, sp, source, cargo)) {
            signal_contract_candidate_t candidates[MAX_CONTRACTS + 1];
            int count = autopilot_append_contract_candidates(
                w, sp, source, candidates, MAX_CONTRACTS + 1);
            int choice = signal_contract_brain_choose(w, sp, candidates, count);
            if (choice >= 0 && choice < count &&
                candidates[choice].action == SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK) {
                sp->autopilot_station_target = candidates[choice].dest_station;
                sp->autopilot_cargo = candidates[choice].commodity;
                sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_WAIT;
            } else {
                autopilot_clear_logistics(sp);
                sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            }
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->autopilot_timer < 0.08f) {
            sp->input.buy_product = true;
            sp->input.buy_commodity = cargo;
            sp->input.buy_grade = MINING_GRADE_COUNT;
        } else if (sp->autopilot_timer > 0.8f) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_LOGISTICS_TRAVEL: {
        int dest = sp->autopilot_station_target;
        commodity_t cargo = sp->autopilot_cargo;
        if (!autopilot_valid_dock_station(w, dest) ||
            !autopilot_finished_good(cargo) ||
            !autopilot_ship_has_finished(sp, cargo)) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->docked) {
            if (sp->current_station == dest) {
                sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_DELIVER;
                sp->autopilot_timer = 0.0f;
            } else {
                sp->input.interact = true; /* launch */
            }
            break;
        }
        const station_t *st = &w->stations[dest];
        vec2 dock_target = station_approach_target(st, sp->ship.pos);
        if (sp->autopilot_timer < 0.08f) {
            nav_find_path(w, sp->ship.pos, dock_target,
                          ship_hull_def(&sp->ship)->ship_radius + 30.0f,
                          nav_player_path(sp->id));
        }
        nav_path_t *path = nav_player_path(sp->id);
        flight_cmd_t cmd = flight_steer_to(w, &sp->ship, path, dock_target,
                                            0.0f, 145.0f, dt);
        sp->input.turn = cmd.turn;
        sp->input.thrust = cmd.thrust;
        sp->input.mine = false;
        if (sp->in_dock_range && sp->nearby_station == dest) {
            sp->input.interact = true;
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_DOCK;
            sp->autopilot_timer = 0.0f;
        } else if (sp->autopilot_timer > 75.0f) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_LOGISTICS_DOCK: {
        int dest = sp->autopilot_station_target;
        commodity_t cargo = sp->autopilot_cargo;
        if (!autopilot_valid_dock_station(w, dest) ||
            !autopilot_finished_good(cargo) ||
            !autopilot_ship_has_finished(sp, cargo)) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->docked) {
            if (sp->current_station == dest) {
                sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_DELIVER;
            } else {
                sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_TRAVEL;
            }
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->autopilot_timer > 6.0f) {
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_TRAVEL;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_LOGISTICS_DELIVER: {
        int dest = sp->autopilot_station_target;
        commodity_t cargo = sp->autopilot_cargo;
        if (!autopilot_finished_good(cargo) ||
            !autopilot_ship_has_finished(sp, cargo)) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (!sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_TRAVEL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (dest >= 0 && sp->current_station != dest &&
            !autopilot_has_delivery_demand(w, sp->current_station, cargo)) {
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_TRAVEL;
            sp->autopilot_timer = 0.0f;
            break;
        }
        sp->input.service_sell = true;
        sp->input.service_sell_only = cargo;
        sp->input.service_sell_grade = MINING_GRADE_COUNT;
        sp->input.service_repair = true;
        if (sp->autopilot_timer > 1.25f) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_LOGISTICS_WAIT: {
        int source = sp->current_station;
        commodity_t cargo = sp->autopilot_cargo;
        if (!sp->docked ||
            !autopilot_valid_dock_station(w, source) ||
            !autopilot_finished_good(cargo)) {
            autopilot_clear_logistics(sp);
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_timer = 0.0f;
            break;
        }
        sp->input.service_repair = true;
        if (autopilot_ship_has_finished(sp, cargo) &&
            autopilot_has_delivery_demand(w, source, cargo)) {
            sp->autopilot_station_target = source;
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_DELIVER;
            sp->autopilot_timer = 0.0f;
            break;
        }
        signal_contract_candidate_t candidates[MAX_CONTRACTS + 1];
        int count = autopilot_append_contract_candidates(
            w, sp, source, candidates, MAX_CONTRACTS + 1);
        int choice = signal_contract_brain_choose(w, sp, candidates, count);
        if (choice >= 0 && choice < count &&
            candidates[choice].action == SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER) {
            sp->autopilot_station_target = candidates[choice].dest_station;
            sp->autopilot_cargo = candidates[choice].commodity;
            sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_BUY;
            sp->autopilot_timer = 0.0f;
            break;
        }
        if (sp->autopilot_timer > 18.0f ||
            choice < 0 || choice >= count ||
            candidates[choice].action != SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK) {
            autopilot_clear_logistics(sp);
            sp->input.interact = true; /* leave idle dock instead of spinning forever */
            sp->autopilot_state = AUTOPILOT_STEP_LAUNCH;
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    case AUTOPILOT_STEP_LAUNCH: {
        if (!sp->docked) {
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            sp->autopilot_timer = 0.0f;
        } else if (sp->autopilot_timer > 2.0f) {
            sp->input.interact = true; /* re-issue */
            sp->autopilot_timer = 0.0f;
        }
        break;
    }
    default:
        sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
        break;
    }
}
