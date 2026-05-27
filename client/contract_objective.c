/*
 * contract_objective.c -- Shared client resolver for contract next steps.
 */
#include "contract_objective.h"

#include <stdarg.h>
#include "client.h"
#include "contract_fit.h"
#include "manifest.h"

static void objective_reset(contract_objective_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->contract_index = -1;
    out->source_station = -1;
    out->target_station = -1;
    out->commodity = COMMODITY_COUNT;
}

static void objective_set_job(contract_objective_t *out, const char *job) {
    if (!out || !job) return;
    snprintf(out->job, sizeof(out->job), "%s", job);
}

static int objective_fracture_target_index(const contract_t *ct)
{
    if (!ct || ct->action != CONTRACT_FRACTURE) return -1;
    int idx = ct->target_index;
    if (idx >= 0 && idx < MAX_ASTEROIDS &&
        g.world.asteroids[idx].active &&
        contract_asteroid_target_matches(ct, &g.world.asteroids[idx])) {
        return idx;
    }
    if (!contract_target_pub_is_set(ct)) return -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.world.asteroids[i].active) continue;
        if (contract_asteroid_target_matches(ct, &g.world.asteroids[i]))
            return i;
    }
    return -1;
}

static void objective_append(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0 || !src) return;
    size_t len = strlen(dst);
    if (len >= cap - 1) return;
    size_t room = cap - len - 1;
    size_t n = strlen(src);
    if (n > room) n = room;
    memcpy(dst + len, src, n);
    dst[len + n] = '\0';
}

static void objective_set_copy(contract_objective_t *out,
                               const char *label,
                               const char *fmt, ...) {
    if (!out || !label || !fmt) return;
    snprintf(out->label, sizeof(out->label), "%s", label);
    va_list args;
    va_start(args, fmt);
    vsnprintf(out->body, sizeof(out->body), fmt, args);
    va_end(args);
    out->message[0] = '\0';
    objective_append(out->message, sizeof(out->message), out->label);
    objective_append(out->message, sizeof(out->message), " :: ");
    objective_append(out->message, sizeof(out->message), out->body);
}

static void objective_set_station_target(contract_objective_t *out,
                                         int station_index,
                                         contract_objective_target_kind_t kind) {
    if (!out) return;
    if (station_index < 0 || station_index >= MAX_STATIONS) return;
    const station_t *st = &g.world.stations[station_index];
    if (!station_is_active(st)) return;
    out->has_world_target = true;
    out->target_kind = kind;
    out->world_target = st->pos;
    out->world_radius = st->radius + 28.0f;
}

static void objective_set_asteroid_target(contract_objective_t *out,
                                          int asteroid_index,
                                          contract_objective_target_kind_t kind) {
    if (!out) return;
    if (asteroid_index < 0 || asteroid_index >= MAX_ASTEROIDS) return;
    const asteroid_t *a = &g.world.asteroids[asteroid_index];
    if (!a->active) return;
    out->has_world_target = true;
    out->target_kind = kind;
    out->world_target = a->pos;
    out->world_radius = a->radius + 32.0f;
}

static void station_name(int station_index, char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    if (station_index >= 0 && station_index < MAX_STATIONS) {
        const station_t *st = &g.world.stations[station_index];
        if (station_exists(st) && st->name[0] != '\0') {
            snprintf(out, out_size, "%s", st->name);
            return;
        }
    }
    snprintf(out, out_size, "station");
}

static int contract_quantity_goal(const contract_t *ct) {
    int qty = (ct && ct->quantity_needed > 0.5f)
            ? (int)ceilf(ct->quantity_needed)
            : 1;
    return qty > 0 ? qty : 1;
}

static const NetDeliveryLedgerEntry *delivery_ledger_for_contract(
    int contract_index)
{
    for (int i = 0; i < g.delivery_ledger_count; i++) {
        const NetDeliveryLedgerEntry *entry = &g.delivery_ledger[i];
        if (entry->contract_index == (uint8_t)contract_index)
            return entry;
    }
    return NULL;
}

static bool cargo_unit_is_named_ingot(const cargo_unit_t *unit) {
    return unit && (cargo_kind_t)unit->kind == CARGO_KIND_INGOT &&
           (ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS;
}

static int station_market_stock_for_contract(const station_t *st,
                                             const contract_t *ct) {
    if (!st || !ct) return 0;
    if (!station_produces(st, ct->commodity)) return 0;
    if (station_sell_price(st, ct->commodity) <= FLOAT_EPSILON) return 0;
    if (!st->manifest.units) return 0;
    int count = 0;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        const cargo_unit_t *unit = &st->manifest.units[i];
        if (!contract_fit_is_ok(contract_fit_cargo_unit(ct, unit))) continue;
        if (ct->proof_flags == 0 && cargo_unit_is_named_ingot(unit)) continue;
        count++;
    }
    return count;
}

static int nearest_station_with_buyable_stock(const contract_t *ct,
                                              vec2 from,
                                              int exclude_station) {
    int best = -1;
    float best_d = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        if (station_market_stock_for_contract(st, ct) <= 0) continue;
        float d = v2_dist_sq(from, st->pos);
        if (best < 0 || d < best_d) {
            best = s;
            best_d = d;
        }
    }
    return best;
}

static int nearest_station_producing(commodity_t commodity, vec2 from) {
    int best = -1;
    float best_d = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        if (!station_produces(st, commodity)) continue;
        float d = v2_dist_sq(from, st->pos);
        if (best < 0 || d < best_d) {
            best = s;
            best_d = d;
        }
    }
    return best;
}

static int nearest_matching_fragment(const contract_t *ct, vec2 from,
                                     float *out_ore) {
    int best = -1;
    float best_d = 0.0f;
    float ore = 0.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!contract_fit_is_ok(contract_fit_fragment(ct, a))) continue;
        float d = v2_dist_sq(from, a->pos);
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
        ore += a->ore;
    }
    if (out_ore) *out_ore = ore;
    return best;
}

static int nearest_compatible_furnace(const station_t *st,
                                      commodity_t commodity,
                                      vec2 *out_pos) {
    if (!st || !station_can_smelt(st, commodity)) return -1;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].scaffold) continue;
        if (st->modules[m].type != MODULE_FURNACE) continue;
        if (out_pos)
            *out_pos = module_world_pos_ring(st, st->modules[m].ring,
                                             st->modules[m].slot);
        return m;
    }
    return -1;
}

static float towed_matching_ore(const contract_t *ct) {
    float held = 0.0f;
    const ship_t *ship = &LOCAL_PLAYER.ship;
    for (int t = 0; t < ship->towed_count; t++) {
        int fi = ship->towed_fragments[t];
        if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[fi];
        if (contract_fit_is_ok(contract_fit_fragment(ct, a)))
            held += a->ore;
    }
    return held;
}

static void format_upstream_step(commodity_t commodity,
                                 char *out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    switch (commodity) {
    case COMMODITY_FERRITE_INGOT:
        snprintf(out, out_size, "MINE FERRITE; SMELT TO FE INGOT");
        break;
    case COMMODITY_CUPRITE_INGOT:
        snprintf(out, out_size, "MINE CUPRITE; SMELT TO CU INGOT");
        break;
    case COMMODITY_CRYSTAL_INGOT:
        snprintf(out, out_size, "MINE CRYSTAL; SMELT TO CR INGOT");
        break;
    case COMMODITY_FRAME:
        snprintf(out, out_size, "MAKE FRAME: FE INGOT TO FRAME PRESS");
        break;
    case COMMODITY_LASER_MODULE:
        snprintf(out, out_size, "MAKE LASER MOD: CU INGOT + FRAME");
        break;
    case COMMODITY_TRACTOR_MODULE:
        snprintf(out, out_size, "MAKE TRACTOR MOD: CR INGOT + FRAME");
        break;
    case COMMODITY_REPAIR_KIT:
        snprintf(out, out_size, "MAKE REPAIR KITS: FRAME + LASER + TRACTOR");
        break;
    default:
        snprintf(out, out_size, "SOURCE %s", commodity_short_name(commodity));
        break;
    }
}

static bool objective_fracture(int contract_index, const contract_t *ct,
                               contract_objective_t *out) {
    out->active = true;
    out->kind = CONTRACT_OBJECTIVE_FRACTURE;
    out->contract_index = contract_index;
    out->target_station = ct->station_index;
    out->commodity = ct->commodity;
    out->quantity = 1;
    objective_set_job(out, "fracture");

    int idx = objective_fracture_target_index(ct);
    if (idx >= 0 && idx < MAX_ASTEROIDS) {
        const asteroid_t *a = &g.world.asteroids[idx];
        if (a->active && a->tier != ASTEROID_TIER_S) {
            objective_set_asteroid_target(out, idx,
                                          CONTRACT_OBJECTIVE_TARGET_EXACT);
            objective_set_copy(out, "SIGNAL // CONTRACT",
                               "FRACTURE %s TARGET ASTEROID [M]",
                               commodity_short_name((commodity_t)a->commodity));
            return true;
        }
    }

    if (ct->commodity < COMMODITY_COUNT) {
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "FRACTURE %s ASTEROID [M]",
                           commodity_short_name(ct->commodity));
    } else {
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "FRACTURE TARGET ASTEROID [M]");
    }
    return true;
}

static bool objective_raw_tractor(int contract_index, const contract_t *ct,
                                  contract_objective_t *out) {
    char dest[32];
    station_name(ct->station_index, dest, sizeof(dest));

    out->active = true;
    out->contract_index = contract_index;
    out->target_station = ct->station_index;
    out->commodity = ct->commodity;

    if (towed_matching_ore(ct) > 0.0f) {
        out->kind = CONTRACT_OBJECTIVE_TOW;
        objective_set_job(out, "tow");
        const station_t *st = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;
        vec2 furnace;
        if (nearest_compatible_furnace(st, ct->commodity, &furnace) >= 0) {
            out->has_world_target = true;
            out->target_kind = CONTRACT_OBJECTIVE_TARGET_FURNACE;
            out->world_target = furnace;
            out->world_radius = 22.0f;
            objective_set_copy(out, "SIGNAL // CONTRACT",
                               "TOW %s FRAGMENTS TO %s FURNACE BEAM",
                               commodity_short_name(ct->commodity), dest);
        } else {
            objective_set_copy(out, "SIGNAL // CONTRACT",
                               "TOW %s FRAGMENTS TO COMPATIBLE FURNACE",
                               commodity_short_name(ct->commodity));
        }
        return true;
    }

    float available_ore = 0.0f;
    int frag = nearest_matching_fragment(ct, LOCAL_PLAYER.ship.pos,
                                         &available_ore);
    if (frag >= 0) {
        out->kind = CONTRACT_OBJECTIVE_TRACTOR;
        out->quantity = (int)lroundf(available_ore);
        objective_set_job(out, "tractor");
        objective_set_asteroid_target(out, frag,
                                      CONTRACT_OBJECTIVE_TARGET_SUGGESTED_SOURCE);
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "TRACTOR %s FRAGMENTS, THEN FURNACE BEAM",
                           commodity_short_name(ct->commodity));
    } else {
        out->kind = CONTRACT_OBJECTIVE_FRACTURE;
        out->quantity = 1;
        objective_set_job(out, "fracture");
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "FRACTURE %s ASTEROID FOR FURNACE FEED",
                           commodity_short_name(ct->commodity));
    }
    return true;
}

static bool objective_finished_delivery(int contract_index, const contract_t *ct,
                                        contract_objective_t *out) {
    char dest[32];
    station_name(ct->station_index, dest, sizeof(dest));

    out->active = true;
    out->contract_index = contract_index;
    out->target_station = ct->station_index;
    out->commodity = ct->commodity;

    int qty = contract_quantity_goal(ct);
    int held = contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest);
    if (held > 0) {
        int deliver = held < qty ? held : qty;
        out->kind = CONTRACT_OBJECTIVE_DELIVER;
        out->quantity = deliver;
        objective_set_job(out, "deliver");
        objective_set_station_target(out, ct->station_index,
                                     CONTRACT_OBJECTIVE_TARGET_DESTINATION);
        if (LOCAL_PLAYER.docked &&
            LOCAL_PLAYER.current_station == (int)ct->station_index) {
            if (g.station_view == STATION_VIEW_WORK) {
                objective_set_copy(out, "SIGNAL // CONTRACT",
                                   "DELIVER %s x%d TO %s [S]",
                                   commodity_short_name(ct->commodity),
                                   deliver, dest);
            } else {
                objective_set_copy(out, "SIGNAL // CONTRACT",
                                   "DELIVER %s x%d TO %s: OPEN JOBS [TAB]",
                                   commodity_short_name(ct->commodity),
                                   deliver, dest);
            }
        } else {
            objective_set_copy(out, "SIGNAL // CONTRACT",
                               "DELIVER %s x%d TO %s",
                               commodity_short_name(ct->commodity),
                               deliver, dest);
        }
        return true;
    }

    int stock_station = nearest_station_with_buyable_stock(ct,
        LOCAL_PLAYER.ship.pos, (int)ct->station_index);
    if (stock_station >= 0) {
        char stock[32];
        station_name(stock_station, stock, sizeof(stock));
        out->kind = CONTRACT_OBJECTIVE_PICKUP;
        out->source_station = stock_station;
        out->quantity = qty;
        objective_set_job(out, "pickup");
        objective_set_station_target(out, stock_station,
                                     CONTRACT_OBJECTIVE_TARGET_SUGGESTED_SOURCE);
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "BUY %s AT %s, DELIVER TO %s",
                           commodity_short_name(ct->commodity), stock, dest);
        return true;
    }

    int producer = nearest_station_producing(ct->commodity,
                                             LOCAL_PLAYER.ship.pos);
    char upstream[80];
    format_upstream_step(ct->commodity, upstream, sizeof(upstream));
    out->kind = CONTRACT_OBJECTIVE_MAKE;
    out->quantity = qty;
    objective_set_job(out, "make");
    if (producer >= 0) {
        char where[32];
        station_name(producer, where, sizeof(where));
        out->source_station = producer;
        objective_set_station_target(out, producer,
                                     CONTRACT_OBJECTIVE_TARGET_PRODUCER);
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "%s AT %s", upstream, where);
    } else {
        objective_set_copy(out, "SIGNAL // CONTRACT", "%s", upstream);
    }
    return true;
}

static bool objective_credit_delivery(int contract_index, const contract_t *ct,
                                      contract_objective_t *out) {
    if (!ct || ct->station_index >= MAX_STATIONS ||
        ct->target_index < 0 || ct->target_index >= MAX_STATIONS) {
        return false;
    }

    char origin[32], dest[32];
    station_name(ct->target_index, origin, sizeof(origin));
    station_name(ct->station_index, dest, sizeof(dest));

    out->active = true;
    out->contract_index = contract_index;
    out->source_station = ct->target_index;
    out->target_station = ct->station_index;
    out->commodity = ct->commodity;

    const NetDeliveryLedgerEntry *ledger =
        delivery_ledger_for_contract(contract_index);
    int qty = contract_quantity_goal(ct);
    if (ledger && ledger->quantity_total > 0)
        qty = (int)ledger->quantity_total;
    out->quantity = qty;

    if (ledger && ledger->status == DELIVERY_SHIPMENT_DELIVERED) {
        out->kind = CONTRACT_OBJECTIVE_PICKUP;
        objective_set_job(out, "clear");
        objective_set_station_target(out, ct->target_index,
                                     CONTRACT_OBJECTIVE_TARGET_DESTINATION);
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "RETURN %s PROOF TO %s",
                           dest, origin);
        return true;
    }

    if (ledger && ledger->status == DELIVERY_SHIPMENT_PICKED_UP) {
        int held = contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest);
        if (held > 0) {
            int deliver = held < qty ? held : qty;
            out->kind = CONTRACT_OBJECTIVE_DELIVER;
            out->quantity = deliver;
            objective_set_job(out, "deliver");
            objective_set_station_target(out, ct->station_index,
                                         CONTRACT_OBJECTIVE_TARGET_DESTINATION);
            objective_set_copy(out, "SIGNAL // CONTRACT",
                               "DELIVER CREDIT %s x%d TO %s",
                               commodity_short_name(ct->commodity),
                               deliver, dest);
            return true;
        }
        out->kind = CONTRACT_OBJECTIVE_PICKUP;
        objective_set_job(out, "recover");
        objective_set_station_target(out, ct->target_index,
                                     CONTRACT_OBJECTIVE_TARGET_SUGGESTED_SOURCE);
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "RECOVER %s SHIPMENT OR RETURN TO %s",
                           commodity_short_name(ct->commodity), origin);
        return true;
    }

    int source_stock = station_contract_source_stock_count(
        &g.world.stations[ct->target_index], ct);
    if (source_stock <= 0) {
        out->kind = CONTRACT_OBJECTIVE_PICKUP;
        objective_set_job(out, "wait");
        objective_set_station_target(out, ct->target_index,
                                     CONTRACT_OBJECTIVE_TARGET_SUGGESTED_SOURCE);
        objective_set_copy(out, "SIGNAL // CONTRACT",
                           "WAIT FOR %s AT %s",
                           commodity_short_name(ct->commodity), origin);
        return true;
    }

    out->kind = CONTRACT_OBJECTIVE_PICKUP;
    objective_set_job(out, "credit");
    objective_set_station_target(out, ct->target_index,
                                 CONTRACT_OBJECTIVE_TARGET_SUGGESTED_SOURCE);
    objective_set_copy(out, "SIGNAL // CONTRACT",
                       "TAKE %s x%d ON CREDIT AT %s, DELIVER TO %s",
                       commodity_short_name(ct->commodity), qty, origin, dest);
    return true;
}

static int commodity_spine_priority(commodity_t commodity) {
    switch (commodity) {
    case COMMODITY_FERRITE_ORE:      return 0;
    case COMMODITY_FERRITE_INGOT:    return 1;
    case COMMODITY_FRAME:            return 2;
    case COMMODITY_CUPRITE_ORE:      return 3;
    case COMMODITY_CUPRITE_INGOT:    return 4;
    case COMMODITY_LASER_MODULE:     return 5;
    case COMMODITY_CRYSTAL_ORE:      return 6;
    case COMMODITY_CRYSTAL_INGOT:    return 7;
    case COMMODITY_TRACTOR_MODULE:   return 8;
    case COMMODITY_REPAIR_KIT:       return 9;
    default:                         return 20;
    }
}

static bool contract_is_claimed_by_other_player(const contract_t *ct) {
    if (!ct) return true;
    return ct->claimed_by >= 0 && ct->claimed_by != (int8_t)LOCAL_PLAYER.id;
}

static bool player_can_fulfill_contract_now(const contract_t *ct) {
    if (!ct || !ct->active) return false;
    if (ct->action == CONTRACT_DELIVERY) {
        const NetDeliveryLedgerEntry *ledger =
            delivery_ledger_for_contract((int)(ct - g.world.contracts));
        if (ledger && ledger->status == DELIVERY_SHIPMENT_DELIVERED &&
            LOCAL_PLAYER.docked &&
            LOCAL_PLAYER.current_station == ct->target_index) {
            return true;
        }
        return ledger && ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
               contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest) > 0;
    }
    if (ct->action != CONTRACT_TRACTOR) return false;
    if (ct->commodity < COMMODITY_RAW_ORE_COUNT)
        return towed_matching_ore(ct) > 0.0f;
    return contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship.manifest) > 0;
}

static bool contract_has_valid_fracture_target(const contract_t *ct) {
    if (!ct || ct->action != CONTRACT_FRACTURE) return true;
    return objective_fracture_target_index(ct) >= 0;
}

bool contract_objective_for_contract(int contract_index,
                                     contract_objective_t *out) {
    objective_reset(out);
    if (!out) return false;
    if (contract_index < 0 || contract_index >= MAX_CONTRACTS) return false;
    /* Gossip filter: only show objective markers for contracts in the
     * player's known set. */
    if (!(g.player_known_contract_mask & (1u << contract_index))) return false;
    const contract_t *ct = &g.world.contracts[contract_index];
    if (!ct->active) return false;

    if (ct->action == CONTRACT_FRACTURE)
        return objective_fracture(contract_index, ct, out);

    if (ct->action == CONTRACT_DELIVERY)
        return objective_credit_delivery(contract_index, ct, out);

    if (ct->action == CONTRACT_TRACTOR) {
        if (ct->commodity < COMMODITY_RAW_ORE_COUNT)
            return objective_raw_tractor(contract_index, ct, out);
        return objective_finished_delivery(contract_index, ct, out);
    }

    return false;
}

bool contract_objective_for_tracked(contract_objective_t *out) {
    objective_reset(out);
    if (g.tracked_contract < 0 || g.tracked_contract >= MAX_CONTRACTS)
        return false;
    if (!g.world.contracts[g.tracked_contract].active) {
        g.tracked_contract = -1;
        return false;
    }
    return contract_objective_for_contract(g.tracked_contract, out);
}

bool contract_objective_for_recommended(contract_objective_t *out) {
    objective_reset(out);
    int best = -1;
    float best_score = -1.0e20f;
    vec2 here = LOCAL_PLAYER.docked && LOCAL_PLAYER.current_station >= 0 &&
                LOCAL_PLAYER.current_station < MAX_STATIONS
              ? g.world.stations[LOCAL_PLAYER.current_station].pos
              : LOCAL_PLAYER.ship.pos;

    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!(g.player_known_contract_mask & (1u << i))) continue; /* gossip filter */
        const contract_t *ct = &g.world.contracts[i];
        if (!ct->active) continue;
        if (contract_is_claimed_by_other_player(ct)) continue;
        if (!contract_has_valid_fracture_target(ct)) continue;

        float score = 10000.0f - (float)commodity_spine_priority(ct->commodity) * 300.0f;
        score += fminf(ct->age, 300.0f) * 0.25f;
        if (ct->claimed_by == (int8_t)LOCAL_PLAYER.id) score += 5000.0f;
        if (player_can_fulfill_contract_now(ct)) score += 6000.0f;
        if (LOCAL_PLAYER.docked &&
            (LOCAL_PLAYER.current_station == (int)ct->station_index ||
             (ct->action == CONTRACT_DELIVERY &&
              LOCAL_PLAYER.current_station == ct->target_index))) {
            score += 900.0f;
        }

        vec2 target = here;
        if (ct->action == CONTRACT_FRACTURE) {
            int target_idx = objective_fracture_target_index(ct);
            if (target_idx >= 0)
                target = g.world.asteroids[target_idx].pos;
        } else if (ct->action == CONTRACT_DELIVERY) {
            const NetDeliveryLedgerEntry *ledger = delivery_ledger_for_contract(i);
            int station_idx = ct->target_index;
            if (ledger && ledger->status == DELIVERY_SHIPMENT_PICKED_UP)
                station_idx = ct->station_index;
            if (ledger && ledger->status == DELIVERY_SHIPMENT_DELIVERED)
                station_idx = ct->target_index;
            if (station_idx >= 0 && station_idx < MAX_STATIONS)
                target = g.world.stations[station_idx].pos;
        } else if (ct->station_index < MAX_STATIONS) {
            target = g.world.stations[ct->station_index].pos;
        }
        score -= sqrtf(v2_dist_sq(here, target)) * 0.01f;

        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best < 0) return false;
    return contract_objective_for_contract(best, out);
}

bool contract_objective_track_contract(int contract_index,
                                       char *message,
                                       size_t message_size) {
    if (message && message_size > 0) message[0] = '\0';
    if (contract_index < 0 || contract_index >= MAX_CONTRACTS) return false;

    /* A hail response is authoritative station contact. Let the newly named
     * contract resolve immediately, before the next known-contract mask lands. */
    if (contract_index < 32)
        g.player_known_contract_mask |= (1u << contract_index);

    contract_objective_t objective;
    if (!contract_objective_for_contract(contract_index, &objective))
        return false;

    g.tracked_contract = contract_index;
    if (message && message_size > 0) {
        const char *text = objective.body[0] ? objective.body : objective.message;
        if (text && text[0])
            snprintf(message, message_size, "%s", text);
    }
    return true;
}

bool contract_objective_ready_upgrade(contract_objective_t *out) {
    objective_reset(out);
    if (!out || !LOCAL_PLAYER.docked) return false;
    const station_t *st = current_station_ptr();
    if (!st || !station_has_module(st, MODULE_DOCK)) return false;

    const struct {
        ship_upgrade_t upgrade;
        const char *label;
        const char *key;
    } checks[] = {
        { SHIP_UPGRADE_MINING,  "UPGRADE LASER",        "[M]" },
        { SHIP_UPGRADE_HOLD,    "EXPAND CARGO HOLD",    "[C]" },
        { SHIP_UPGRADE_TRACTOR, "UPGRADE TRACTOR BEAM", "[T]" },
    };

    float balance = player_current_balance();
    for (int i = 0; i < (int)(sizeof(checks) / sizeof(checks[0])); i++) {
        if (!can_afford_upgrade(st, &LOCAL_PLAYER.ship,
                                checks[i].upgrade, balance))
            continue;
        out->active = true;
        out->kind = CONTRACT_OBJECTIVE_UPGRADE;
        objective_set_job(out, "upgrade");
        objective_set_copy(out, "SIGNAL // UPGRADE",
                           "%s AT DOCK %s",
                           checks[i].label, checks[i].key);
        return true;
    }

    return false;
}
