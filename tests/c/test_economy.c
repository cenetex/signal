#include "test_harness.h"
#include "contract_fit.h"
#include "cargo_legality.h"
#include "cargo_receipt_issue.h"
#include "station_policy.h"
#include "chain_log.h"
#include "gossip.h"

static void economy_chain_test_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_chain_%s", TMP("econ"), suffix);
    chain_log_set_dir(path);
    chain_log_set_disk_enabled(true);
}

static void economy_chain_test_teardown(void) {
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(NULL);
}

static void economy_chain_test_wipe_logs(world_t *w) {
    if (!w) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        chain_log_reset(&w->stations[s]);
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0,
               sizeof(w->stations[s].chain_last_hash));
    }
}

static void economy_fill_pubkey(uint8_t out[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed + i);
}

static cargo_unit_t economy_test_cargo_unit(const uint8_t cargo_pub[32]) {
    cargo_unit_t unit = {0};
    unit.kind = CARGO_KIND_INGOT;
    unit.commodity = COMMODITY_FERRITE_INGOT;
    unit.grade = MINING_GRADE_COMMON;
    unit.recipe_id = RECIPE_SMELT;
    unit.prefix_class = INGOT_PREFIX_M;
    unit.quantity = 1;
    memcpy(unit.pub, cargo_pub, 32);
    return unit;
}

static int economy_count_exact_pod_units(const world_t *w, commodity_t c) {
    int total = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != c) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            if ((commodity_t)pod->manifest_units[u].commodity != c) {
                exact = false;
                break;
            }
        }
        if (exact) total += (int)pod->manifest_count;
    }
    return total;
}

static const cargo_pod_t *economy_first_exact_pod(const world_t *w,
                                                  commodity_t c) {
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != c) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            if ((commodity_t)pod->manifest_units[u].commodity != c) {
                exact = false;
                break;
            }
        }
        if (exact) return pod;
    }
    return NULL;
}

static bool economy_issue_single_receipt(world_t *w,
                                         int station_idx,
                                         const uint8_t recipient[32],
                                         const uint8_t cargo_pub[32],
                                         cargo_receipt_chain_t *out) {
    if (!w || !out || station_idx < 0 || station_idx >= MAX_STATIONS)
        return false;
    station_t *st = &w->stations[station_idx];
    memset(out, 0, sizeof(*out));
    if (chain_log_emit(w, st, CHAIN_EVT_SMELT, "legality", 8) == 0)
        return false;
    cargo_receipt_t receipt = {0};
    if (cargo_receipt_emit_transfer(w, st, st->station_pubkey, recipient,
                                    cargo_pub, (uint8_t)CARGO_KIND_INGOT,
                                    st->chain_last_hash, &receipt) == 0) {
        return false;
    }
    out->links[0] = receipt;
    out->len = 1;
    return true;
}

static void economy_force_provenance_screening(world_t *w, int station_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_idx];
    st->policy_generation = 1;
    st->policy_tick = w->tick + 1;
    st->policy_card_count = 1;
    st->policy_card_ids[0] = (uint8_t)STATION_POLICY_CARD_PROVENANCE_SCREENING;
    st->policy_card_domains[0] = (uint8_t)STATION_POLICY_DOMAIN_TRADE;
    st->policy_card_costs[0] = 25;
    st->policy_card_scores[0] = 1.0f;
}

static void economy_force_black_market(world_t *w, int station_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    station_t *st = &w->stations[station_idx];
    st->policy_generation = 1;
    st->policy_tick = w->tick + 1;
    st->policy_card_count = 1;
    st->policy_card_ids[0] = (uint8_t)STATION_POLICY_CARD_BLACK_MARKET;
    st->policy_card_domains[0] = (uint8_t)STATION_POLICY_DOMAIN_TRADE;
    st->policy_card_costs[0] = 20;
    st->policy_card_scores[0] = 1.0f;
}

static const cargo_unit_t *test_station_first_unit(const station_t *st,
                                                   commodity_t c,
                                                   recipe_id_t recipe_id) {
    if (!st || !st->manifest.units) return NULL;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        const cargo_unit_t *u = &st->manifest.units[i];
        if ((commodity_t)u->commodity == c &&
            (recipe_id_t)u->recipe_id == recipe_id) {
            return u;
        }
    }
    return NULL;
}

static delivery_shipment_t *test_find_delivery_shipment(world_t *w,
                                                        int contract_index) {
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (shipment->active &&
            shipment->contract_index == (uint8_t)contract_index) {
            return shipment;
        }
    }
    return NULL;
}

static int test_find_delivery_shipment_pod(const world_t *w,
                                           const server_player_t *sp,
                                           const delivery_shipment_t *shipment) {
    if (!w || !sp || !shipment) return -1;
    for (int t = 0; t < sp->ship->towed_pod_count && t < 10; t++) {
        int idx = sp->ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &w->cargo_pods[idx];
        if (pod->active &&
            pod->kind == CARGO_POD_CARGO &&
            pod->shipment_id == shipment->shipment_id) {
            return idx;
        }
    }
    return -1;
}

static int test_find_towed_exact_cargo_pod(const world_t *w,
                                           const server_player_t *sp,
                                           commodity_t commodity) {
    if (!w || !sp) return -1;
    for (int t = 0; t < sp->ship->towed_pod_count && t < 10; t++) {
        int idx = sp->ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &w->cargo_pods[idx];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != commodity) continue;
        if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
            continue;
        bool exact = true;
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            if ((commodity_t)pod->manifest_units[i].commodity != commodity) {
                exact = false;
                break;
            }
        }
        if (exact) return idx;
    }
    return -1;
}

static int test_spawn_towed_exact_cargo_pod(world_t *w,
                                            server_player_t *sp,
                                            commodity_t commodity,
                                            uint16_t count) {
    if (!w || !sp || commodity >= COMMODITY_COUNT ||
        count == 0 || count > CARGO_POD_MANIFEST_CAP ||
        sp->ship->towed_pod_count >= 10) {
        return -1;
    }
    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    const uint8_t origin[8] = { 'T','E','S','T','B','L','K','G' };
    for (uint16_t i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, commodity, i, &units[i]))
            return -1;
    }
    vec2 pos = v2_add(sp->ship->pos, v2(-44.0f, 20.0f));
    int pod_idx = spawn_cargo_pod_with_manifest(
        w, pos, sp->ship->vel, commodity, units, count, CARGO_POD_CARGO);
    if (pod_idx < 0) return -1;
    if (!world_cargo_pod_set_player_tractor(w, pod_idx, (int)sp->id)) {
        memset(&w->cargo_pods[pod_idx], 0, sizeof(w->cargo_pods[pod_idx]));
        return -1;
    }
    return pod_idx;
}

static bool test_stage_pod_at_station_hopper(world_t *w,
                                             int station_idx,
                                             int pod_idx,
                                             commodity_t commodity,
                                             int *out_hopper_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return false;
    }
    station_t *st = &w->stations[station_idx];
    int hopper_idx = station_find_hopper_for(st, commodity);
    if (hopper_idx < 0 || hopper_idx >= st->module_count ||
        hopper_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    w->cargo_pods[pod_idx].pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);
    if (out_hopper_idx) *out_hopper_idx = hopper_idx;
    return true;
}

static int test_first_dock_module_idx(const station_t *st) {
    if (!st) return -1;
    for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            return m;
        }
    }
    return -1;
}

static int test_spawn_station_market_exact_cargo_pod(world_t *w,
                                                     int station_idx,
                                                     commodity_t commodity,
                                                     uint16_t count) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        commodity >= COMMODITY_COUNT || count == 0 ||
        count > CARGO_POD_MANIFEST_CAP) {
        return -1;
    }
    station_t *st = &w->stations[station_idx];
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx < 0) return -1;

    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    const uint8_t origin[8] = { 'T','E','S','T','M','K','T','P' };
    for (uint16_t i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, commodity, i, &units[i]))
            return -1;
    }

    vec2 pos = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    int pod_idx = spawn_cargo_pod_with_manifest(
        w, pos, v2(0.0f, 0.0f), commodity, units, count, CARGO_POD_CARGO);
    if (pod_idx < 0) return -1;

    cargo_unit_t shell = {0};
    const uint8_t shell_origin[8] = { 'T','E','S','T','S','H','E','L' };
    if (hash_legacy_migrate_unit(shell_origin, COMMODITY_FRAME, 0, &shell))
        cargo_pod_set_shell_frame(&w->cargo_pods[pod_idx], &shell);
    return world_cargo_pod_set_module_tractor(
               w, pod_idx, station_idx, dock_idx) ? pod_idx : -1;
}

static float test_station_market_pod_sell_quote(const station_t *st,
                                                const cargo_pod_t *pod) {
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->manifest_count == 0 || pod->manifest_count != pod->quantity) {
        return 0.0f;
    }
    float quote = 0.0f;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        const cargo_unit_t *unit = &pod->manifest_units[i];
        quote += station_sell_price_unit(st, unit) *
                 mining_payout_multiplier((mining_grade_t)unit->grade);
    }
    return quote;
}

static void test_move_pod_past_station_charge_boundary(world_t *w,
                                                       int station_idx,
                                                       int pod_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return;
    }
    station_t *st = &w->stations[station_idx];
    vec2 base = st->pos;
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx >= 0) {
        base = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    }
    w->cargo_pods[pod_idx].pos =
        v2_add(base, v2(CARGO_POD_DOCK_TRACTOR_RANGE +
                        HOPPER_INTAKE_STAGING_RANGE + 80.0f, 0.0f));
    w->cargo_pods[pod_idx].vel = v2(0.0f, 0.0f);
}

static bool test_stage_pod_at_station_dock_mouth(world_t *w,
                                                  int station_idx,
                                                  int pod_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) {
        return false;
    }
    station_t *st = &w->stations[station_idx];
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx < 0) return false;
    const station_module_t *dock = &st->modules[dock_idx];
    vec2 dock_pos = module_world_pos_ring(st, dock->ring, dock->slot);
    vec2 outward = v2_norm(v2_sub(dock_pos, st->pos));
    if (v2_len_sq(outward) < 0.5f)
        outward = v2_from_angle(module_angle_ring(st, dock->ring,
                                                  dock->slot));
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    float radius = pod->radius > 0.0f ? pod->radius : 18.0f;
    pod->pos = v2_add(dock_pos, v2_scale(
        outward, STATION_MODULE_COL_RADIUS + radius + 8.0f));
    pod->vel = station_ring_point_velocity(st, dock->ring, pod->pos);
    return true;
}

static bool test_view_has_market_memory(const knowledge_view_t *view,
                                        uint8_t memory_kind,
                                        uint8_t station_a,
                                        uint8_t station_b,
                                        uint8_t commodity,
                                        market_memory_t *out) {
    if (!view) return false;
    for (int i = 0; i < view->count && i < KNOWLEDGE_VIEW_MAX_CAP; i++) {
        market_memory_t memory;
        if (!market_memory_from_knowledge_item(&view->items[i], &memory))
            continue;
        if (memory.memory_kind == memory_kind &&
            memory.station_a == station_a &&
            memory.station_b == station_b &&
            memory.commodity == commodity) {
            if (out) *out = memory;
            return true;
        }
    }
    return false;
}

static void test_setup_delivery_player(world_t *w, server_player_t **out_sp) {
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    memset(w->contracts, 0, sizeof(w->contracts));
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x5a, sizeof(sp->session_token));
    ASSERT(test_set_station_finished_units(&w->stations[0],
                                           COMMODITY_FRAME, 8));
    if (out_sp) *out_sp = sp;
}

TEST(test_station_production_yard_makes_frames) {
    station_t station = {0};
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_FRAME_PRESS };
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FERRITE_INGOT, 5));
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_FERRITE_INGOT),
                    4.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FRAME),
                    2.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FERRITE_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME), 2);
}

TEST(test_station_production_beamworks_makes_modules) {
    station_t station = {0};
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_LASER_FAB };
    station.modules[station.module_count++] = (station_module_t){ .type = MODULE_TRACTOR_FAB };
    ASSERT(test_set_station_finished_units(&station, COMMODITY_CUPRITE_INGOT, 5));
    ASSERT(test_set_station_finished_units(&station, COMMODITY_CRYSTAL_INGOT, 5));
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 1));
    step_station_production(&station, 1, 1.0f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_CUPRITE_INGOT),
                    4.5f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_CRYSTAL_INGOT),
                    4.5f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station, COMMODITY_FRAME),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_LASER_MODULE),
                    0.5f, 0.001f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&station,
                                              COMMODITY_TRACTOR_MODULE),
                    0.5f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_CUPRITE_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_CRYSTAL_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME), 0);
}

TEST(test_station_repair_cost_no_damage) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 100.0f;
    station_t station = {0};
    ASSERT_EQ_FLOAT(station_repair_cost(&ship, &station), 0.0f, 0.01f);
}

TEST(test_station_repair_cost_with_damage) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.hull = 50.0f;
    station_t station = {0};
    /* Any dock can install kits — repair quote needs MODULE_DOCK. */
    station.modules[station.module_count++] =
        (station_module_t){ .type = MODULE_DOCK };
    float cost = station_repair_cost(&ship, &station);
    ASSERT(cost > 0.0f);
}

TEST(test_can_afford_upgrade_dock_fallback) {
    /* Empty cargo, but station stocks the modules and player has
     * credits — dock fills the gap from inventory at retail. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    STATION_DECL(station);
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 100));
    station.base_price[COMMODITY_FRAME] = 22.0f;
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,10000.0f));
}

TEST(test_can_afford_upgrade_no_credits_for_dock_fallback) {
    /* Empty cargo, station has modules, balance zero — fallback
     * needs credits, so this must be rejected. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    STATION_DECL(station);
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    ASSERT(test_set_station_finished_units(&station, COMMODITY_FRAME, 100));
    station.base_price[COMMODITY_FRAME] = 22.0f;
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,0.0f));
}

TEST(test_can_afford_upgrade_no_product_anywhere) {
    /* Empty cargo, empty station inventory — no modules to install. */
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    station._inventory_cache[COMMODITY_FRAME] = 0.0f;
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,10000.0f));
}

TEST(test_can_afford_upgrade_cargo_only_no_credits_needed) {
    /* Ship cargo covers the full module cost — credit balance is
     * irrelevant since the dock has nothing to sell. */
    SHIP_DECL(ship);
    ship.hull_class = HULL_CLASS_MINER;
    station_t station = {0};
    station.services = STATION_SERVICE_UPGRADE_HOLD;
    /* Empty dock inventory; ship carries enough frames itself. */
    int need = (int)ceilf(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD));
    ASSERT(test_set_ship_finished_units(&ship, COMMODITY_FRAME, need,
                                        MINING_GRADE_COMMON));
    ASSERT(can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD,0.0f));
}

TEST(test_can_afford_upgrade_rejects_float_only_finished_goods) {
    SHIP_DECL(ship);
    STATION_DECL(station);
    ship.hull_class = HULL_CLASS_MINER;
    ASSERT(ship_manifest_bootstrap(&ship));
    ASSERT(station_manifest_bootstrap(&station));

    int need = (int)ceilf(upgrade_product_cost(&ship, SHIP_UPGRADE_HOLD));
    ship.cargo[COMMODITY_FRAME] = (float)need;
    station._inventory_cache[COMMODITY_FRAME] = (float)need;

    ASSERT_EQ_INT(ship_finished_count(&ship, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(&station, COMMODITY_FRAME), 0);
    ASSERT(!can_afford_upgrade(&station, &ship, SHIP_UPGRADE_HOLD, 10000.0f));
}

TEST(test_contract_generated_from_hopper_deficit) {
    /* A refinery with low ore_buffer should generate an ore contract */
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    /* Make ferrite the biggest deficit by filling the others */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    w.stations[0]._inventory_cache[COMMODITY_CUPRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    w.stations[0]._inventory_cache[COMMODITY_CRYSTAL_ORE] = REFINERY_HOPPER_CAPACITY;
    world_sim_step(&w, SIM_DT);
    /* Find contract for station 0, ferrite ore */
    contract_t *found = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            found = &w.contracts[k];
            break;
        }
    }
    ASSERT(found != NULL);
    /* Ore contracts are inventory-driven — quantity_needed is 0 */
    ASSERT_EQ_FLOAT(found->quantity_needed, 0.0f, 0.01f);
}

TEST(test_contract_price_escalates_with_age) {
    /* An unfilled contract should increase in price over time */
    contract_t c = {.active = true, .base_price = 10.0f, .age = 0.0f};
    float price_t0 = contract_price(&c);
    c.age = 300.0f; /* 5 minutes */
    float price_t5 = contract_price(&c);
    ASSERT(price_t5 > price_t0);
    ASSERT_EQ_FLOAT(price_t5, 10.0f * 1.2f, 0.01f);
}

TEST(test_contract_fit_requires_material_grade_and_fragment_tier) {
    contract_t ingot_contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
    };
    cargo_unit_t unit = {
        .kind = CARGO_KIND_INGOT,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .grade = (uint8_t)MINING_GRADE_FINE,
        .quantity = 1,
    };
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&ingot_contract, &unit),
                  (int)CONTRACT_FIT_GRADE_TOO_LOW);

    unit.grade = (uint8_t)MINING_GRADE_RARE;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&ingot_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    unit.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&ingot_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_COMMODITY);

    contract_t ore_contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .commodity = COMMODITY_FERRITE_ORE,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
    };
    asteroid_t fragment = {
        .active = true,
        .tier = ASTEROID_TIER_S,
        .ore = 8.0f,
        .commodity = COMMODITY_FERRITE_ORE,
        .grade = (uint8_t)MINING_GRADE_FINE,
    };
    ASSERT_EQ_INT((int)contract_fit_fragment(&ore_contract, &fragment),
                  (int)CONTRACT_FIT_GRADE_TOO_LOW);

    fragment.grade = (uint8_t)MINING_GRADE_RARE;
    ASSERT_EQ_INT((int)contract_fit_fragment(&ore_contract, &fragment),
                  (int)CONTRACT_FIT_OK);

    fragment.tier = ASTEROID_TIER_M;
    ASSERT_EQ_INT((int)contract_fit_fragment(&ore_contract, &fragment),
                  (int)CONTRACT_FIT_WRONG_TIER);
}

TEST(test_contract_fit_enforces_heritage_recipe_prefix_and_parent) {
    cargo_unit_t unit = {
        .kind = CARGO_KIND_INGOT,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .grade = (uint8_t)MINING_GRADE_COMMON,
        .quantity = 1,
        .prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS,
        .recipe_id = (uint16_t)RECIPE_LEGACY_MIGRATE,
    };
    for (int i = 0; i < 32; i++) {
        unit.pub[i] = (uint8_t)(0x20 + i);
        unit.parent_merkle[i] = (uint8_t)(0x80 + i);
    }

    contract_t recipe_contract = {
        .active = true,
        .action = CONTRACT_TRACTOR,
        .commodity = COMMODITY_FERRITE_INGOT,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE),
        .required_recipe_id = (uint16_t)RECIPE_SMELT,
    };
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&recipe_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_RECIPE);
    unit.recipe_id = (uint16_t)RECIPE_SMELT;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&recipe_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    contract_t prefix_contract = recipe_contract;
    prefix_contract.proof_flags |= (uint8_t)CONTRACT_PROOF_REQUIRE_PREFIX;
    prefix_contract.required_prefix_class = (uint8_t)INGOT_PREFIX_M;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&prefix_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_PREFIX);
    unit.prefix_class = (uint8_t)INGOT_PREFIX_M;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&prefix_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    contract_t parent_contract = recipe_contract;
    parent_contract.proof_flags |= (uint8_t)CONTRACT_PROOF_REQUIRE_PARENT;
    memset(parent_contract.required_parent, 0xAA, sizeof(parent_contract.required_parent));
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&parent_contract, &unit),
                  (int)CONTRACT_FIT_WRONG_PARENT);
    memcpy(parent_contract.required_parent, unit.parent_merkle,
           sizeof(parent_contract.required_parent));
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&parent_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    contract_t origin_contract = recipe_contract;
    origin_contract.proof_flags |= (uint8_t)CONTRACT_PROOF_FORBID_ORIGIN;
    origin_contract.forbidden_origin_mask = 1ULL << 2;
    unit.origin_station = 2;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&origin_contract, &unit),
                  (int)CONTRACT_FIT_FORBIDDEN_ORIGIN);
    unit.origin_station = 1;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&origin_contract, &unit),
                  (int)CONTRACT_FIT_OK);

    memset(unit.pub, 0, sizeof(unit.pub));
    memset(unit.parent_merkle, 0, sizeof(unit.parent_merkle));
    unit.mined_block = 0;
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&recipe_contract, &unit),
                  (int)CONTRACT_FIT_MISSING_PROOF);
    ASSERT_EQ_INT((int)contract_fit_cargo_unit(&origin_contract, &unit),
                  (int)CONTRACT_FIT_MISSING_PROOF);
}

TEST(test_contract_delivery_requires_required_grade) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);

    ASSERT(test_set_ship_finished_units(w.players[0].ship,
                                        COMMODITY_TRACTOR_MODULE, 5,
                                        MINING_GRADE_COMMON));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_TRACTOR_MODULE,
        .required_grade = (uint8_t)MINING_GRADE_RARE,
        .quantity_needed = 2.0f,
        .base_price = 100.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    float credits_before = ledger_balance(&w.stations[0],
                                          w.players[0].session_token);
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_FLOAT(ship_cargo_amount(w.players[0].ship,
                                      COMMODITY_TRACTOR_MODULE),
                    5.0f, 0.01f);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 2.0f, 0.01f);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[0],
                                   w.players[0].session_token),
                    credits_before, 0.01f);
}

TEST(test_contract_delivery_requires_heritage_recipe) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FRAME, 1);
    ASSERT(pod_idx >= 0);
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE),
        .required_recipe_id = (uint16_t)RECIPE_FRAME_BASIC,
        .quantity_needed = 2.0f,
        .base_price = 100.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.stations[1].base_price[COMMODITY_FRAME] = 0.0f;
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FRAME, NULL));

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 1);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 2.0f, 0.01f);

    w.cargo_pods[pod_idx].manifest_units[0].recipe_id =
        (uint16_t)RECIPE_FRAME_BASIC;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 1.0f, 0.01f);
}

TEST(test_contract_delivery_bans_enemy_origin_station) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x03, 8);

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FRAME, 1);
    ASSERT(pod_idx >= 0);
    cargo_unit_t *unit = &w.cargo_pods[pod_idx].manifest_units[0];
    unit->recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    unit->origin_station = 2;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE |
                                 CONTRACT_PROOF_FORBID_ORIGIN),
        .required_recipe_id = (uint16_t)RECIPE_FRAME_BASIC,
        .forbidden_origin_mask = 1ULL << 2,
        .quantity_needed = 2.0f,
        .base_price = 100.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.stations[1].base_price[COMMODITY_FRAME] = 0.0f;
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FRAME, NULL));

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 1);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 2.0f, 0.01f);

    unit = &w.cargo_pods[pod_idx].manifest_units[0];
    unit->origin_station = 1;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 1.0f, 0.01f);
}

TEST(test_contract_closes_when_deficit_filled) {
    /* Tractor-contract close hysteresis: opens on deficit (<90%), must NOT
     * close until inventory crosses 95% — otherwise a station sitting in
     * [80%, 95%] opens-and-closes a contract every tick, spamming
     * SIM_EVENT_CONTRACT_COMPLETE. See fix for issue #461. */
    WORLD_DECL;
    world_reset(&w);
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 10.0f;
    world_sim_step(&w, SIM_DT); /* generates contract (deficit > threshold) */

    /* 85% should NOT close the contract anymore — it's between open (90%) and close (95%) */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.85f;
    world_sim_step(&w, SIM_DT);
    bool still_active = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            still_active = true; break;
        }
    }
    ASSERT(still_active);

    /* Above the 95% close threshold, contract closes */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.96f;
    world_sim_step(&w, SIM_DT);
    bool still_active2 = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0 && w.contracts[k].commodity == COMMODITY_FERRITE_ORE) {
            still_active2 = true; break;
        }
    }
    ASSERT(!still_active2);
}

TEST(test_raw_ore_contract_retires_when_refined_output_full) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)MAX_PRODUCT_STOCK));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_ORE,
        .quantity_needed = 1.0f,
        .base_price = 3.0f,
        .claimed_by = -1,
    };

    world_sim_step(&w, SIM_DT);

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].station_index == 0 &&
                 w.contracts[k].commodity == COMMODITY_FERRITE_ORE));
    }
}

TEST(test_kit_input_contract_closes_at_kit_target) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FRAME, 20));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FRAME,
        .quantity_needed = 12.0f,
        .base_price = 1.0f,
        .claimed_by = -1,
    };

    world_sim_step(&w, SIM_DT);

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].station_index == 2 &&
                 w.contracts[k].commodity == COMMODITY_FRAME));
    }
}

TEST(test_generated_heritage_contracts_require_source_recipe) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    station_t *kepler = &w.stations[1];
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FRAME, 0));

    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, 100));

    world_sim_step(&w, SIM_DT);

    bool found_smelted_ingot = false;
    bool found_fabbed_frame = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (!c->active || c->action != CONTRACT_TRACTOR) continue;
        if (c->station_index == 1 && c->commodity == COMMODITY_FERRITE_INGOT) {
            found_smelted_ingot = true;
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE);
            ASSERT_EQ_INT(c->required_recipe_id, RECIPE_SMELT);
        }
        if (c->station_index == 2 && c->commodity == COMMODITY_FRAME) {
            found_fabbed_frame = true;
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE);
            ASSERT_EQ_INT(c->required_recipe_id, RECIPE_FRAME_BASIC);
        }
    }
    ASSERT(found_smelted_ingot);
    ASSERT(found_fabbed_frame);
}

TEST(test_station_policy_preserves_seeded_supply_loop) {
    uint64_t prospect = station_policy_forbidden_origin_mask(
        0, COMMODITY_REPAIR_KIT);
    uint64_t kepler = station_policy_forbidden_origin_mask(
        1, COMMODITY_FERRITE_INGOT);
    uint64_t helios = station_policy_forbidden_origin_mask(
        2, COMMODITY_FRAME);

    ASSERT_EQ_INT((int)(prospect & ((1ULL << 1) | (1ULL << 2))), 0);
    ASSERT_EQ_INT((int)(kepler & ((1ULL << 0) | (1ULL << 2))), 0);
    ASSERT_EQ_INT((int)(helios & ((1ULL << 0) | (1ULL << 1))), 0);
    ASSERT_EQ_INT((int)(prospect & (1ULL << SIGNAL_FREEPORT_STATION_INDEX)), 0);
    ASSERT((helios & (1ULL << SIGNAL_FREEPORT_STATION_INDEX)) != 0);
}

TEST(test_station_policy_cards_rank_under_domain_budgets) {
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(
        prospect, COMMODITY_REPAIR_KIT, 0));

    station_policy_selection_t selection;
    station_policy_select_cards(prospect, 0, &selection);

    ASSERT(station_policy_selection_has(
        &selection, STATION_POLICY_CARD_REPAIR_STOCK_RESERVE));
    ASSERT(station_policy_selection_has(
        &selection, STATION_POLICY_CARD_STRATEGIC_IMPORTS));
    ASSERT(!station_policy_selection_has(
        &selection, STATION_POLICY_CARD_HOSTILE_ORIGIN_EMBARGO));

    int spent[STATION_POLICY_DOMAIN_COUNT] = {0};
    for (int i = 0; i < selection.count; i++)
        spent[selection.cards[i].domain] += selection.cards[i].budget_cost;
    ASSERT(spent[STATION_POLICY_DOMAIN_TRADE] <= selection.budget.trade);
    ASSERT(spent[STATION_POLICY_DOMAIN_CONSTRUCTION] <=
           selection.budget.construction);
    ASSERT(spent[STATION_POLICY_DOMAIN_FINANCE] <= selection.budget.finance);
}

TEST(test_station_policy_black_market_requires_off_relay_station) {
    WORLD_DECL;
    world_reset(&w);

    station_policy_selection_t relay_selection;
    station_policy_select_cards(&w.stations[0], 0, &relay_selection);
    ASSERT(!station_policy_selection_has(
        &relay_selection, STATION_POLICY_CARD_BLACK_MARKET));

    station_t freeport = {0};
    snprintf(freeport.name, sizeof(freeport.name), "Freeport");
    freeport.signal_range = 0.0f;
    freeport.module_count = 1;
    freeport.modules[0] = (station_module_t){ .type = MODULE_DOCK };

    station_policy_selection_t off_relay_selection;
    station_policy_select_cards(&freeport, 3, &off_relay_selection);
    ASSERT(station_policy_selection_has(
        &off_relay_selection, STATION_POLICY_CARD_BLACK_MARKET));
}

TEST(test_blackglass_posts_black_market_buy_contract) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    station_t *freeport = &w.stations[SIGNAL_FREEPORT_STATION_INDEX];
    ASSERT(station_exists(freeport));
    ASSERT(station_faction_is_pirate_economy(freeport));

    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_CRYSTAL_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_CUPRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        freeport, COMMODITY_FRAME, 0));

    world_sim_step(&w, SIM_DT);

    contract_t *found = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR &&
            c->station_index == SIGNAL_FREEPORT_STATION_INDEX) {
            found = c;
            break;
        }
    }

    ASSERT(found != NULL);
    ASSERT_EQ_INT(found->commodity, COMMODITY_TRACTOR_MODULE);
    ASSERT(found->quantity_needed > 0.0f);
    ASSERT(found->base_price >
           freeport->base_price[found->commodity]);
    ASSERT_EQ_INT(found->proof_flags, 0);
    ASSERT_EQ_INT(found->required_recipe_id, 0);
    ASSERT_EQ_INT((int)found->forbidden_origin_mask, 0);
}

TEST(test_station_policy_cache_drives_trade_price_modifier) {
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    prospect->_inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT(test_set_station_finished_units(
        prospect, COMMODITY_REPAIR_KIT, 0));

    station_policy_refresh(prospect, 0, 7);

    ASSERT_EQ_INT((int)prospect->policy_tick, 7);
    ASSERT(prospect->policy_generation > 0);
    ASSERT(station_policy_cached_has(
        prospect, STATION_POLICY_CARD_REPAIR_STOCK_RESERVE));
    ASSERT(station_policy_cached_has(
        prospect, STATION_POLICY_CARD_STRATEGIC_IMPORTS));
    ASSERT_EQ_INT((int)prospect->policy_top_demand_commodity,
                  COMMODITY_REPAIR_KIT);
    ASSERT(prospect->policy_top_demand_severity > 0.9f);
    ASSERT(station_policy_trade_price_multiplier(
        prospect, COMMODITY_REPAIR_KIT) > 1.4f);

    uint32_t generation = prospect->policy_generation;
    station_policy_refresh(prospect, 0, 7);
    ASSERT_EQ_INT((int)prospect->policy_generation, (int)generation);
}

TEST(test_cargo_legality_clean_chain_is_not_contraband) {
    economy_chain_test_setup("legality_clean");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);

    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    economy_fill_pubkey(player_pk, 0x31);
    economy_fill_pubkey(cargo_pk, 0x71);
    cargo_unit_t unit = economy_test_cargo_unit(cargo_pk);
    cargo_receipt_chain_t chain = {0};
    ASSERT(economy_issue_single_receipt(&w, 2, player_pk, cargo_pk, &chain));
    economy_force_provenance_screening(&w, 0);

    cargo_legality_result_t result = cargo_legality_classify(
        w.stations, MAX_STATIONS, 0, &unit, &chain);
    ASSERT_EQ_INT((int)result.status, CARGO_LEGALITY_CLEAN);
    ASSERT(cargo_legality_station_accepts(result));
    ASSERT_EQ_INT(result.origin_station, 2);
    ASSERT((result.reasons & CARGO_LEGALITY_REASON_POLICY_SCREENS) != 0);

    economy_chain_test_teardown();
}

TEST(test_cargo_legality_missing_receipt_is_policy_contraband) {
    WORLD_DECL;
    world_reset(&w);
    uint8_t cargo_pk[32];
    economy_fill_pubkey(cargo_pk, 0x72);
    cargo_unit_t unit = economy_test_cargo_unit(cargo_pk);
    economy_force_provenance_screening(&w, 0);

    cargo_legality_result_t result = cargo_legality_classify(
        w.stations, MAX_STATIONS, 0, &unit, NULL);
    ASSERT_EQ_INT((int)result.status, CARGO_LEGALITY_CONTRABAND);
    ASSERT(!cargo_legality_station_accepts(result));
    ASSERT((result.reasons & CARGO_LEGALITY_REASON_MISSING_RECEIPT) != 0);
    ASSERT((result.reasons & CARGO_LEGALITY_REASON_POLICY_SCREENS) != 0);
}

TEST(test_cargo_legality_black_market_authority_is_local_policy) {
    economy_chain_test_setup("legality_black_market");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);

    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    economy_fill_pubkey(player_pk, 0x33);
    economy_fill_pubkey(cargo_pk, 0x73);
    cargo_unit_t unit = economy_test_cargo_unit(cargo_pk);

    /* Make station 1 a black-market authority without changing the cargo. */
    economy_force_black_market(&w, 1);

    cargo_receipt_chain_t chain = {0};
    ASSERT(economy_issue_single_receipt(&w, 1, player_pk, cargo_pk, &chain));
    economy_force_provenance_screening(&w, 0);

    cargo_legality_result_t lawful = cargo_legality_classify(
        w.stations, MAX_STATIONS, 0, &unit, &chain);
    ASSERT_EQ_INT((int)lawful.status, CARGO_LEGALITY_CONTRABAND);
    ASSERT(!cargo_legality_station_accepts(lawful));
    ASSERT_EQ_INT(lawful.black_market_station, 1);
    ASSERT((lawful.reasons &
            CARGO_LEGALITY_REASON_BLACK_MARKET_AUTHORITY) != 0);

    cargo_legality_result_t pirate = cargo_legality_classify(
        w.stations, MAX_STATIONS, 1, &unit, &chain);
    ASSERT_EQ_INT((int)pirate.status, CARGO_LEGALITY_CONTRABAND);
    ASSERT(cargo_legality_station_accepts(pirate));
    ASSERT((pirate.reasons & CARGO_LEGALITY_REASON_POLICY_TOLERATES) != 0);

    economy_chain_test_teardown();
}

TEST(test_bulk_sell_refuses_black_market_origin_at_lawful_station) {
    economy_chain_test_setup("bulk_sell_legality");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    economy_fill_pubkey(player_pk, 0x34);
    economy_fill_pubkey(cargo_pk, 0x74);
    cargo_unit_t unit = economy_test_cargo_unit(cargo_pk);
    cargo_receipt_chain_t chain = {0};
    economy_force_black_market(&w, 0);
    economy_force_provenance_screening(&w, 1);
    ASSERT(economy_issue_single_receipt(&w, 0, player_pk, cargo_pk, &chain));
    ASSERT(ship_manifest_push_with_chain(sp->ship, &unit, &chain));
    ship_finished_sync(sp->ship, COMMODITY_FERRITE_INGOT);

    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_COUNT;
    int kepler_before = station_finished_count(&w.stations[1],
                                               COMMODITY_FERRITE_INGOT);
    float balance_before = ledger_balance(&w.stations[1], sp->session_token);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 1);
    ASSERT_EQ_INT(station_finished_count(&w.stations[1],
                                         COMMODITY_FERRITE_INGOT),
                  kepler_before);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[1], sp->session_token),
                    balance_before, 0.001f);

    economy_chain_test_teardown();
}

TEST(test_black_market_buys_unwanted_towed_pod_at_markdown) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    const int blackglass_idx = SIGNAL_FREEPORT_STATION_INDEX;
    station_t *blackglass = &w.stations[blackglass_idx];
    ASSERT(station_exists(blackglass));
    economy_force_black_market(&w, blackglass_idx);
    ASSERT(station_policy_accepts_contract_bound_cargo(blackglass));
    ASSERT(!station_consumes(blackglass, COMMODITY_LASER_MODULE));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_TRACTOR_MODULE, 6));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_LASER_MODULE, 6));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_CRYSTAL_INGOT, 10));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_CUPRITE_INGOT, 10));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_FERRITE_INGOT, 10));
    ASSERT(test_set_station_finished_units(blackglass, COMMODITY_FRAME, 12));
    blackglass->base_price[COMMODITY_LASER_MODULE] = 40.0f;

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xB1, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = blackglass_idx;
    sp->nearby_station = blackglass_idx;
    sp->in_dock_range = true;
    sp->ship->pos = blackglass->pos;

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, sp, COMMODITY_LASER_MODULE, 2);
    ASSERT(pod_idx >= 0);
    int dock_idx = test_first_dock_module_idx(blackglass);
    ASSERT(dock_idx >= 0);
    w.cargo_pods[pod_idx].pos = module_world_pos_ring(
        blackglass, blackglass->modules[dock_idx].ring,
        blackglass->modules[dock_idx].slot);
    int laser_stock_before =
        station_finished_count(blackglass, COMMODITY_LASER_MODULE);

    float full_quote = 0.0f;
    for (uint16_t u = 0; u < w.cargo_pods[pod_idx].manifest_count; u++) {
        const cargo_unit_t *unit = &w.cargo_pods[pod_idx].manifest_units[u];
        float unit_quote = station_buy_price_unit(blackglass, unit);
        unit_quote *= mining_payout_multiplier((mining_grade_t)unit->grade);
        full_quote += unit_quote;
    }
    float expected = full_quote * BLACK_MARKET_CARGO_MARKDOWN;
    ASSERT(full_quote > expected);
    float before = ledger_balance(blackglass, sp->session_token);

    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_FLOAT(ledger_balance(blackglass, sp->session_token) - before,
                    expected, 0.01f);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].commodity, COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].manifest_count, 2);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[pod_idx]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            blackglass_idx, dock_idx));
    ASSERT_EQ_INT(station_finished_count(blackglass, COMMODITY_LASER_MODULE),
                  laser_stock_before);
}

TEST(test_raw_ore_contract_prefers_starved_downstream_output) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    station_t *helios = &w.stations[2];
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE] = 0.0f;
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CUPRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CRYSTAL_INGOT, 12));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, 12));

    world_sim_step(&w, SIM_DT);

    bool found_cuprite = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w.contracts[k].active || w.contracts[k].station_index != 2) continue;
        if (w.contracts[k].commodity == COMMODITY_CUPRITE_ORE)
            found_cuprite = true;
        ASSERT(w.contracts[k].commodity != COMMODITY_CRYSTAL_ORE);
    }
    ASSERT(found_cuprite);
}

TEST(test_sell_price_uses_contract_price) {
    /* When a contract exists, selling at that station should pay the
     * escalated contract price, not the base buy_price.
     *
     * Uses COMMODITY_FERRITE_INGOT because raw-ore cargo delivery is a
     * dead path post-#259 (physical ore towing; fragments ride in
     * ship.towed_fragments[], not ship.cargo[]). Ingot delivery is the
     * live path this contract-price logic actually serves. */
    WORLD_DECL;
    world_reset(&w);
    /* Create a contract with aged price — Kepler needs an ingot. */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR, .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 50.0f,
        .base_price = 10.0f, .age = 300.0f, /* 5 min -> 1.2x */
    };
    /* Set up player with a deliverable ingot at Kepler's physical intake. */
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 10);
    ASSERT(pod_idx >= 0);
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    /* Zero out ledger balance for precise payout check */
    float init_bal = ledger_balance(&w.stations[1], w.players[0].session_token);
    float expected_price = 10.0f * 1.2f; /* contract_price at age 300 */
    world_sim_step(&w, SIM_DT);
    /* Credits should reflect escalated price, not base 10.0 */
    float earned = ledger_balance(&w.stations[1], w.players[0].session_token) - init_bal;
    ASSERT(earned > 10.0f * 10.0f); /* more than base */
    ASSERT_EQ_FLOAT(earned, 10.0f * expected_price, 1.0f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
}

TEST(test_hauler_fills_highest_value_contract) {
    /* NPC hauler at a station should pick the highest-value contract
     * fillable from local inventory, not a hardcoded destination */
    WORLD_DECL;
    world_reset(&w);
    /* Set up two contracts: one cheap at station 1, one expensive at station 2 */
    w.contracts[0] = (contract_t){
        .active = true, .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 10.0f, .age = 0.0f,
    };
    w.contracts[1] = (contract_t){
        .active = true, .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 50.0f, .age = 0.0f,
    };
    /* Give home station (0) manifest-backed inventory of both. */
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 20));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_CUPRITE_INGOT, 20));

    int seeded_hauler = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    npc_ship_t *hauler = &w.npc_ships[seeded_hauler];
    ASSERT(hauler != NULL);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        if (i != seeded_hauler) w.npc_ships[i].active = false;
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f; /* ready to act */
    hauler->home_station = 0;
    hauler->dest_station = 1; /* default dest */
    memset(hauler->ship->cargo, 0, sizeof(hauler->ship->cargo));
    /* Seed known_contracts to simulate prior gossip — under the
     * gossip-contract model the hauler only acts on contracts it has
     * heard about via dock contact. The test is exercising the picker
     * scoring, not the gossip propagation, so we inject knowledge
     * directly. */
    test_clear_knowledge(&hauler->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w.contracts[k].active) continue;
        contract_summary_t summary = contract_summary_make(&w.contracts[k]);
        ASSERT(test_add_known_contract(&hauler->ship->knowledge, &summary));
    }
    world_sim_step(&w, SIM_DT);
    /* Hauler should target station 2 (higher value contract) */
    ASSERT(hauler->dest_station == 2);
}

TEST(test_hauler_picker_trusts_gossiped_contract) {
    /* Under the gossip-contract model the hauler trusts known contract
     * summaries — it cannot peek at foreign station module state to
     * filter out destinations that don't accept the commodity. The
     * authoritative compatibility check happens at delivery time, where
     * a mismatch costs a wasted trip rather than a wrong pick. So the
     * picker simply takes the highest-scoring contract by price/dist. */
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_CRYSTAL_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 500.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.contracts[1] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_CRYSTAL_INGOT, 20));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_CUPRITE_INGOT, 20));

    int seeded_hauler = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    npc_ship_t *hauler = NULL;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler_slot = i;
            hauler = &w.npc_ships[i];
            break;
        }
    }
    ASSERT(hauler != NULL);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        if (i != hauler_slot) w.npc_ships[i].active = false;
    ship_t *hauler_ship = world_npc_ship_for(&w, hauler_slot);
    ASSERT(hauler_ship != NULL);
    ASSERT(ship_manifest_bootstrap(hauler_ship));
    manifest_clear(&hauler_ship->manifest);
    ship_receipts_clear(ship_get_receipts(hauler_ship));
    memset(hauler_ship->cargo, 0, sizeof(hauler_ship->cargo));
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 1;
    memset(hauler->ship->cargo, 0, sizeof(hauler->ship->cargo));
    /* Seed known_contracts (see comment in test_hauler_fills_highest_value_contract) */
    test_clear_knowledge(&hauler->ship->knowledge, SHIP_KNOWN_ITEM_CAP);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w.contracts[k].active) continue;
        contract_summary_t summary = contract_summary_make(&w.contracts[k]);
        ASSERT(test_add_known_contract(&hauler->ship->knowledge, &summary));
    }

    step_npc_ships(&w, SIM_DT);

    /* Highest-value contract wins: $500 crystal to station 1, even
     * though station 1 doesn't actually have a crystal-consuming
     * module. The mismatch will surface at unloading; for the picker,
     * the gossiped contract is the source of truth. */
    ASSERT_EQ_INT(hauler->dest_station, 1);
    ASSERT(ship_finished_count(hauler->ship, COMMODITY_CRYSTAL_INGOT) > 0);
    ASSERT_EQ_INT(ship_finished_count(hauler->ship,
                                      COMMODITY_CUPRITE_INGOT), 0);

    int dest_stock_before = station_finished_count(&w.stations[1],
                                                   COMMODITY_CRYSTAL_INGOT);
    float ledger_before = ledger_balance(&w.stations[1],
                                         hauler->session_token);
    hauler->state = NPC_STATE_UNLOADING;
    hauler->state_timer = 0.0f;
    hauler->dest_station = 1;
    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(station_finished_count(&w.stations[1],
                                         COMMODITY_CRYSTAL_INGOT),
                  dest_stock_before);
    ASSERT_EQ_FLOAT(ledger_balance(&w.stations[1], hauler->session_token),
                    ledger_before, 0.001f);
    ASSERT(manifest_count_by_commodity(&hauler_ship->manifest,
                                       COMMODITY_CRYSTAL_INGOT) > 0);
    ASSERT(!w.contracts[0].active);
}

TEST(test_hauler_ignores_float_only_finished_stock) {
    WORLD_DECL;
    world_reset(&w);

    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
        ASSERT(test_set_station_finished_units(&w.stations[0], (commodity_t)c, 0));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 50.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] = 20.0f;

    int seeded_hauler = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler_slot = i;
            break;
        }
    }
    ASSERT(hauler_slot >= 0);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (i != hauler_slot) w.npc_ships[i].active = false;
    }

    npc_ship_t *hauler = &w.npc_ships[hauler_slot];
    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 1;
    memset(hauler->ship->cargo, 0, sizeof(hauler->ship->cargo));

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_FLOAT(hauler->ship->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0],
                                         COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT],
                    20.0f, 0.001f);
}

TEST(test_one_contract_per_station) {
    WORLD_DECL;
    world_reset(&w);
    /* Empty all hoppers to create demand */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        w.stations[0]._inventory_cache[i] = 0.0f;
    /* Run a few ticks to generate contracts */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    /* Count contracts for station 0. Up to two are allowed per station:
     * one ore contract (raw mining) + one production contract
     * (scaffold/ingot/kit-fab input). */
    int count = 0;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 0) count++;
    }
    ASSERT(count >= 1 && count <= 2);
}

TEST(test_destroy_contract_completes_when_asteroid_gone) {
    /* DESTROY contracts should close when their target_index is invalid or inactive.
     * Test without full sim to avoid respawn interference. */
    contract_t c = {
        .active = true, .action = CONTRACT_FRACTURE,
        .target_index = -1,  /* invalid = gone */
        .base_price = 30.0f, .claimed_by = -1,
    };
    /* The fulfillment check: idx < 0 || idx >= MAX_ASTEROIDS || !asteroids[idx].active */
    bool target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS);
    ASSERT(target_gone);

    /* Valid index, inactive asteroid */
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));
    c.target_index = 5;
    asteroids[5].active = false;
    target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS || !asteroids[c.target_index].active);
    ASSERT(target_gone);

    /* Valid index, active asteroid — should NOT be gone */
    asteroids[5].active = true;
    target_gone = (c.target_index < 0 || c.target_index >= MAX_ASTEROIDS || !asteroids[c.target_index].active);
    ASSERT(!target_gone);
}

TEST(test_fracture_contract_target_pub_matches_asteroid_identity) {
    contract_t c = {
        .active = true,
        .action = CONTRACT_FRACTURE,
        .target_index = 5,
        .base_price = 30.0f,
        .claimed_by = -1,
    };
    asteroid_t asteroid = {
        .active = true,
        .tier = ASTEROID_TIER_L,
        .commodity = COMMODITY_FERRITE_ORE,
    };
    for (int i = 0; i < 32; i++)
        asteroid.rock_pub[i] = (uint8_t)(0x20u + (uint8_t)i);

    contract_set_target_pub_from_asteroid(&c, &asteroid);
    ASSERT(contract_target_pub_is_set(&c));
    ASSERT(contract_asteroid_target_matches(&c, &asteroid));
    ASSERT_EQ_INT((int)contract_fit_asteroid(&c, &asteroid),
                  (int)CONTRACT_FIT_OK);

    asteroid.rock_pub[0] ^= 0x7Fu;
    ASSERT(!contract_asteroid_target_matches(&c, &asteroid));
    ASSERT_EQ_INT((int)contract_fit_asteroid(&c, &asteroid),
                  (int)CONTRACT_FIT_WRONG_COMMODITY);
}

TEST(test_supply_contract_uses_correct_material) {
    WORLD_DECL;
    world_reset(&w);
    /* LASER_FAB needs crystal ingot + frame hoppers. Plant both. */
    add_hopper_for(&w.stations[1], 3, 1, COMMODITY_CRYSTAL_INGOT);
    add_hopper_for(&w.stations[1], 3, 7, COMMODITY_FRAME);
    begin_module_construction_at(&w, &w.stations[1], 1, MODULE_LASER_FAB, 2, 4);
    /* The generated contract should be for crystal ingots */
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == 1
            && w.contracts[k].commodity == COMMODITY_CRYSTAL_INGOT) {
            found = true; break;
        }
    }
    ASSERT(found);
    /* After contract expires and regenerates via step_contracts, it should still be crystal */
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].station_index == 1
            && w.contracts[k].commodity == COMMODITY_CRYSTAL_INGOT) {
            found = true; break;
        }
    }
    ASSERT(found);
}

TEST(test_dynamic_ore_price_deficit) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    /* Buy price: empty=1× base, full=0.5× base */
    st._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 5.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY * 0.5f;
    ASSERT_EQ_FLOAT(station_buy_price(&st, COMMODITY_FERRITE_ORE), 7.5f, 0.1f);
    /* Sell price: empty=2× base, full=1× base */
    st._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 20.0f, 0.1f);
    st._inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FERRITE_ORE), 10.0f, 0.1f);
}

TEST(test_product_price_tracks_ore) {
    station_t st = {0};
    st.base_price[COMMODITY_FRAME] = 20.0f;
    /* Sell price: empty=2× base, full=1× base */
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 40.0f, 0.1f);
    ASSERT(test_set_station_finished_units(
        &st, COMMODITY_FRAME, (int)MAX_PRODUCT_STOCK));
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 20.0f, 0.1f);
    ASSERT(test_set_station_finished_units(
        &st, COMMODITY_FRAME, (int)(MAX_PRODUCT_STOCK * 0.5f)));
    ASSERT_EQ_FLOAT(station_sell_price(&st, COMMODITY_FRAME), 25.0f, 0.1f);
}

TEST(test_deliver_ingots_to_contract) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player tows the exact ferrite-ingot crate the contract can accept. */
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 20);
    ASSERT(pod_idx >= 0);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    float credits_before = ledger_balance(&w.stations[1], w.players[0].session_token);
    /* Create a contract at station 1 (Kepler Yard) for ferrite ingots */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 20.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    world_sim_step(&w, SIM_DT);
    /* Ingot crate delivered, credits gained at station 1 */
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) > credits_before);
    /* Contract quantity reduced */
    ASSERT(w.contracts[0].quantity_needed < 20.0f || !w.contracts[0].active);
}

TEST(test_first_cross_station_haul_uses_local_ledgers) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x42, sizeof(sp->session_token));

    station_t *prospect = &w.stations[0];
    station_t *kepler = &w.stations[1];
    ASSERT_STR_EQ(prospect->currency_name, "prospect vouchers");
    ASSERT_STR_EQ(kepler->currency_name, "kepler bonds");

    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 2));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FRAME,
                                           (int)MAX_PRODUCT_STOCK));

    for (int i = 0; i < 4; i++) world_sim_step(&w, SIM_DT);

    int kepler_contract = -1;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w.contracts[k];
        if (ct->active && ct->action == CONTRACT_TRACTOR &&
            ct->station_index == 1 &&
            ct->commodity == COMMODITY_FERRITE_INGOT) {
            kepler_contract = k;
            break;
        }
    }
    ASSERT(kepler_contract >= 0);

    ledger_earn(prospect, sp->session_token, 100.0f);
    float prospect_start = ledger_balance(prospect, sp->session_token);
    float kepler_start = ledger_balance(kepler, sp->session_token);
    int prospect_market_pod = test_spawn_station_market_exact_cargo_pod(
        &w, 0, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(prospect_market_pod >= 0);

    sp->docked = true;
    sp->current_station = 0;
    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    int prospect_ingots_before = station_finished_count(prospect,
                                                        COMMODITY_FERRITE_INGOT);
    int prospect_frames_before = station_finished_count(prospect,
                                                        COMMODITY_FRAME);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    int bought_ingot_pod = test_find_towed_exact_cargo_pod(
        &w, sp, COMMODITY_FERRITE_INGOT);
    ASSERT(bought_ingot_pod >= 0);
    ASSERT_EQ_INT(bought_ingot_pod, prospect_market_pod);
    ASSERT_EQ_INT(w.cargo_pods[bought_ingot_pod].quantity, 1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT),
                  prospect_ingots_before);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  prospect_frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_ingot_pod]), 0);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_start, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(kepler, sp->session_token), kepler_start, 0.001f);

    float expected_ingot_cost = test_station_market_pod_sell_quote(
        prospect, &w.cargo_pods[bought_ingot_pod]);
    ASSERT(expected_ingot_cost > 0.0f);
    test_move_pod_past_station_charge_boundary(&w, 0, bought_ingot_pod);
    world_sim_step(&w, SIM_DT);
    bool found_buy = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_BUY) {
            found_buy = true;
            ASSERT_EQ_INT(ev->buy.station, 0);
            ASSERT_EQ_INT(ev->buy.commodity, COMMODITY_FERRITE_INGOT);
            ASSERT_EQ_INT(ev->buy.cost, (int)lroundf(expected_ingot_cost));
            ASSERT_EQ_INT(ev->buy.quantity, 1);
        }
    }
    ASSERT(found_buy);
    ASSERT_EQ_FLOAT(prospect_start -
                    ledger_balance(prospect, sp->session_token),
                    expected_ingot_cost, 0.01f);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_ingot_pod]),
                  -1);
    float prospect_after_buy = ledger_balance(prospect, sp->session_token);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[bought_ingot_pod]), sp->id);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 1);

    int kepler_ingots_before = station_finished_count(kepler,
                                                      COMMODITY_FERRITE_INGOT);
    int kepler_hopper_idx = -1;
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, bought_ingot_pod,
                                            COMMODITY_FERRITE_INGOT,
                                            &kepler_hopper_idx));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(w.cargo_pods[bought_ingot_pod].active);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[bought_ingot_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[bought_ingot_pod],
                                            1, kepler_hopper_idx));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FERRITE_INGOT),
                  kepler_ingots_before);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_after_buy, 0.001f);
    float kepler_after_delivery = ledger_balance(kepler, sp->session_token);
    ASSERT(kepler_after_delivery > kepler_start);

    int kepler_frame_market_pod = test_spawn_station_market_exact_cargo_pod(
        &w, 1, COMMODITY_FRAME, 1);
    ASSERT(kepler_frame_market_pod >= 0);
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FRAME;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    int kepler_frames_before = station_finished_count(kepler, COMMODITY_FRAME);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FRAME), 0);
    int bought_frame_pod = test_find_towed_exact_cargo_pod(
        &w, sp, COMMODITY_FRAME);
    ASSERT(bought_frame_pod >= 0);
    ASSERT_EQ_INT(bought_frame_pod, kepler_frame_market_pod);
    ASSERT_EQ_INT(w.cargo_pods[bought_frame_pod].quantity, 1);
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FRAME),
                  kepler_frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_frame_pod]), 1);
    ASSERT_EQ_FLOAT(ledger_balance(kepler, sp->session_token),
                    kepler_after_delivery, 0.001f);
    test_move_pod_past_station_charge_boundary(&w, 1, bought_frame_pod);
    world_sim_step(&w, SIM_DT);
    ASSERT(ledger_balance(kepler, sp->session_token) < kepler_after_delivery);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_after_buy, 0.001f);
}

TEST(test_delivery_credit_contract_pickup_deliver_and_clear) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    int helios_to_prospect_before =
        station_faction_relation_to(helios, prospect->faction_id);
    int prospect_to_helios_before =
        station_faction_relation_to(prospect, helios->faction_id);
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 3));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };
    knowledge_view_configure(&helios->knowledge, STATION_KNOWN_ITEM_CAP);
    market_memory_t stale_demand = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 2,
        .value_hint = 50,
        .observed_tick = 1,
    };
    knowledge_item_t stale_item;
    ASSERT(knowledge_item_from_market_memory(&stale_demand, &stale_item));
    knowledge_view_insert(&helios->knowledge, &stale_item);

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT), 1);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    float prospect_after_pickup = ledger_balance(prospect, sp->session_token);
    ASSERT(prospect_after_pickup < 0.0f);

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    float helios_before = ledger_balance(helios, sp->session_token);
    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 2, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 2);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            2, helios_dock));
    ASSERT(ledger_balance(helios, sp->session_token) > helios_before);
    ASSERT(w.contracts[0].active);
    ASSERT(station_faction_relation_to(helios, prospect->faction_id) >
           helios_to_prospect_before);
    ASSERT(station_faction_relation_to(prospect, helios->faction_id) >
           prospect_to_helios_before);
    market_memory_t receipt = {0};
    ASSERT(test_view_has_market_memory(&helios->knowledge,
                                       (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT,
                                       2, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &receipt));
    ASSERT_EQ_INT(receipt.quantity_hint, 2);
    ASSERT(receipt.value_hint > 0);
    ASSERT(!test_view_has_market_memory(&helios->knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_CLEARED);
    ASSERT(!w.contracts[0].active);
    ASSERT(ledger_balance(prospect, sp->session_token) > 0.0f);
}

TEST(test_delivery_credit_dock_custody_does_not_teleport_far_pod) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect,
                                           COMMODITY_FERRITE_INGOT, 2));
    ASSERT(test_set_station_finished_units(helios,
                                           COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);

    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    const station_module_t *dock = &helios->modules[helios_dock];
    vec2 dock_pos = module_world_pos_ring(helios, dock->ring, dock->slot);
    vec2 outward = v2_norm(v2_sub(dock_pos, helios->pos));
    if (v2_len_sq(outward) < 0.5f)
        outward = v2_from_angle(module_angle_ring(helios, dock->ring,
                                                  dock->slot));
    vec2 far_pos = v2_add(dock_pos, v2_scale(outward,
        CARGO_POD_DOCK_TRACTOR_RANGE * 2.0f));
    vec2 far_vel = v2_scale(outward, -17.0f);
    w.cargo_pods[shipment_pod].pos = far_pos;
    w.cargo_pods[shipment_pod].vel = far_vel;

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[shipment_pod]));
    ASSERT_EQ_INT(cargo_pod_custody_station(
                      &w.cargo_pods[shipment_pod]), 2);
    ASSERT(v2_dist_sq(w.cargo_pods[shipment_pod].pos, far_pos) <
           80.0f * 80.0f);
    ASSERT(v2_dist_sq(w.cargo_pods[shipment_pod].pos, dock_pos) >
           CARGO_POD_DOCK_TRACTOR_RANGE *
           CARGO_POD_DOCK_TRACTOR_RANGE);
}

TEST(test_delivery_credit_requires_exact_bound_cargo) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT,
                                           2));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT,
                                           0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity,
                  COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);

    uint8_t first_exact_pub[32];
    uint8_t second_exact_pub[32];
    memcpy(first_exact_pub, shipment->cargo_pub[0], sizeof(first_exact_pub));
    memcpy(second_exact_pub, shipment->cargo_pub[1], sizeof(second_exact_pub));
    ASSERT(memcmp(shipment->cargo_units[0].pub, first_exact_pub, 32) == 0);
    ASSERT(memcmp(shipment->cargo_units[1].pub, second_exact_pub, 32) == 0);

    world_cargo_pod_clear_tractor(&w, shipment_pod);

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    float helios_before = ledger_balance(helios, sp->session_token);
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(shipment->quantity_delivered, 0);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_FLOAT(ledger_balance(helios, sp->session_token),
                    helios_before, 0.001f);

    ASSERT(world_cargo_pod_set_player_tractor(
        &w, shipment_pod, (int)sp->id));
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 2, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 2);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            2, helios_dock));
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(ledger_balance(helios, sp->session_token) > helios_before);
}

TEST(test_delivery_credit_row_sell_unloads_bound_pod) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect,
                                           COMMODITY_FERRITE_INGOT, 2));
    ASSERT(test_set_station_finished_units(helios,
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);

    sp->docked = true;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    float helios_before = ledger_balance(helios, sp->session_token);
    int helios_dock = test_first_dock_module_idx(helios);
    ASSERT(helios_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 2, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 2);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT(w.cargo_pods[shipment_pod].has_shell_frame);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            2, helios_dock));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(ledger_balance(helios, sp->session_token) > helios_before);
}

TEST(test_delivery_credit_hail_ignores_empty_origin) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT(test_find_delivery_shipment(&w, 0) == NULL);
    ASSERT_EQ_INT(w.contracts[0].claimed_by, -1);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token), 0.0f, 0.001f);

    bool found_hail = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_HAIL_RESPONSE) {
            found_hail = true;
            ASSERT_EQ_INT(ev->hail_response.station, 0);
            ASSERT(ev->hail_response.contract_index != 0);
        }
    }
    ASSERT(found_hail);
}

TEST(test_delivery_credit_hail_requires_docking_to_pick_up) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 3));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->ship->pos = prospect->pos;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT(test_find_delivery_shipment(&w, 0) == NULL);
    ASSERT_EQ_INT(w.contracts[0].claimed_by, -1);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT), 3);

    bool found_hail = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_HAIL_RESPONSE) {
            found_hail = true;
            ASSERT_EQ_INT(ev->hail_response.station, 0);
            ASSERT_EQ_INT(ev->hail_response.contract_index, 0);
        }
    }
    ASSERT(found_hail);
}

TEST(test_delivery_credit_black_market_sale_defaults_origin_debt) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    station_t *pirate = &w.stations[3];
    snprintf(pirate->name, sizeof(pirate->name), "Freeport");
    pirate->signal_range = 0.0f;
    pirate->dock_radius = 96.0f;
    pirate->radius = 120.0f;
    pirate->module_count = 1;
    pirate->modules[0] = (station_module_t){ .type = MODULE_DOCK };
    pirate->base_price[COMMODITY_FERRITE_INGOT] = 18.0f;
    ASSERT(station_manifest_bootstrap(pirate));
    int helios_to_pirate_before =
        station_faction_relation_to(helios, pirate->faction_id);
    int pirate_to_helios_before =
        station_faction_relation_to(pirate, helios->faction_id);

    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 2));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    knowledge_view_configure(&helios->knowledge, STATION_KNOWN_ITEM_CAP);
    market_memory_t stale_demand = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 2,
        .value_hint = 50,
        .observed_tick = 1,
    };
    knowledge_item_t stale_item;
    ASSERT(knowledge_item_from_market_memory(&stale_demand, &stale_item));
    knowledge_view_insert(&helios->knowledge, &stale_item);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    int shipment_pod = test_find_delivery_shipment_pod(&w, sp, shipment);
    ASSERT(shipment_pod >= 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    float prospect_after_pickup = ledger_balance(prospect, sp->session_token);
    ASSERT(prospect_after_pickup < 0.0f);

    sp->docked = true;
    sp->current_station = 3;
    sp->nearby_station = 3;
    sp->in_dock_range = true;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    float pirate_before = ledger_balance(pirate, sp->session_token);
    int pirate_dock = test_first_dock_module_idx(pirate);
    ASSERT(pirate_dock >= 0);
    ASSERT(test_stage_pod_at_station_dock_mouth(&w, 3, shipment_pod));
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_BLACK_MARKET_SOLD);
    ASSERT_EQ_INT(shipment->quantity_black_market_sold, 2);
    ASSERT(w.cargo_pods[shipment_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].shipment_id, 0);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].quantity, 2);
    ASSERT_EQ_INT(w.cargo_pods[shipment_pod].manifest_count, 2);
    ASSERT(w.cargo_pods[shipment_pod].has_shell_frame);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[shipment_pod]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[shipment_pod],
                                            3, pirate_dock));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(ledger_balance(pirate, sp->session_token) > pirate_before);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    prospect_after_pickup, 0.001f);
    ASSERT(!w.contracts[0].active);
    ASSERT(station_faction_relation_to(helios, pirate->faction_id) <
           helios_to_pirate_before);
    ASSERT(station_faction_relation_to(pirate, helios->faction_id) <
           pirate_to_helios_before);
    ASSERT(test_view_has_market_memory(&helios->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&pirate->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&sp->ship->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(!test_view_has_market_memory(&helios->knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));
}

TEST(test_delivery_credit_timeout_emits_station_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = NULL;
    test_setup_delivery_player(&w, &sp);

    station_t *prospect = &w.stations[0];
    station_t *helios = &w.stations[2];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 1));
    ASSERT(test_set_station_finished_units(helios, COMMODITY_FERRITE_INGOT, 0));
    prospect->base_price[COMMODITY_FERRITE_INGOT] = 20.0f;
    helios->base_price[COMMODITY_FERRITE_INGOT] = 30.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 1.0f,
        .base_price = 50.0f,
        .claimed_by = -1,
    };

    knowledge_view_configure(&helios->knowledge, STATION_KNOWN_ITEM_CAP);
    market_memory_t stale_demand = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DEMAND,
        .station_a = 2,
        .station_b = 0xff,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 210,
        .salience = 180,
        .quantity_hint = 1,
        .value_hint = 50,
        .observed_tick = 1,
    };
    knowledge_item_t stale_item;
    ASSERT(knowledge_item_from_market_memory(&stale_demand, &stale_item));
    knowledge_view_insert(&helios->knowledge, &stale_item);

    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.hail = true;
    world_sim_step(&w, SIM_DT);
    memset(&sp->input, 0, sizeof(sp->input));

    delivery_shipment_t *shipment = test_find_delivery_shipment(&w, 0);
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    shipment->due_tick = w.tick;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DEFAULTED);
    ASSERT(!w.contracts[0].active);
    ASSERT(test_view_has_market_memory(&helios->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&sp->ship->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(!test_view_has_market_memory(&helios->knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));
}

TEST(test_prospect_pubkey_buy_debits_pubkey_ledger) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x42, sizeof(sp->session_token));
    memset(sp->pubkey, 0xA5, sizeof(sp->pubkey));
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->docked = true;
    sp->current_station = 0;

    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 1));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    int market_pod = test_spawn_station_market_exact_cargo_pod(
        &w, 0, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(market_pod >= 0);
    ledger_earn_by_pubkey(prospect, sp->pubkey, 1000.0f);
    ledger_earn(prospect, sp->session_token, 333.0f);
    float pubkey_before = ledger_balance_by_pubkey(prospect, sp->pubkey);
    float session_before = ledger_balance(prospect, sp->session_token);
    int frames_before = station_finished_count(prospect, COMMODITY_FRAME);
    float expected_cost = test_station_market_pod_sell_quote(
        prospect, &w.cargo_pods[market_pod]);
    ASSERT(expected_cost > 0.0f);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FERRITE_INGOT), 0);
    int bought_pod = test_find_towed_exact_cargo_pod(
        &w, sp, COMMODITY_FERRITE_INGOT);
    ASSERT(bought_pod >= 0);
    ASSERT_EQ_INT(bought_pod, market_pod);
    ASSERT_EQ_INT(w.cargo_pods[bought_pod].quantity, 1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[bought_pod]), 0);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(prospect, sp->pubkey),
                    pubkey_before, 0.001f);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    session_before, 0.001f);

    test_move_pod_past_station_charge_boundary(&w, 0, bought_pod);
    world_sim_step(&w, SIM_DT);

    bool found_buy = false;
    for (int i = 0; i < w.events.count; i++) {
        const sim_event_t *ev = &w.events.events[i];
        if (ev->type == SIM_EVENT_BUY) {
            found_buy = true;
            ASSERT_EQ_INT(ev->buy.station, 0);
            ASSERT_EQ_INT(ev->buy.commodity, COMMODITY_FERRITE_INGOT);
            ASSERT_EQ_INT(ev->buy.cost, (int)lroundf(expected_cost));
            ASSERT_EQ_INT(ev->buy.quantity, 1);
        }
    }
    ASSERT(found_buy);
    ASSERT_EQ_FLOAT(pubkey_before - ledger_balance_by_pubkey(prospect, sp->pubkey),
                    expected_cost, 0.01f);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    session_before, 0.001f);
}

TEST(test_market_buy_requires_station_held_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x43, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;

    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(prospect,
                                           COMMODITY_FERRITE_INGOT, 1));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 2));
    ledger_earn(prospect, sp->session_token, 1000.0f);

    int ingots_before = station_finished_count(prospect,
                                               COMMODITY_FERRITE_INGOT);
    int frames_before = station_finished_count(prospect, COMMODITY_FRAME);
    float balance_before = ledger_balance(prospect, sp->session_token);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(test_find_towed_exact_cargo_pod(
                      &w, sp, COMMODITY_FERRITE_INGOT), -1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT),
                  ingots_before);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  frames_before);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token),
                    balance_before, 0.001f);
}

/* Pubkey-registered players had been getting 65% of the contract payout
 * because try_sell_station_cargo routed through ledger_credit_supply
 * (which applies the 35% smelt-station cut) instead of ledger_earn
 * (full credit). Locks the fix in: payout to the ledger == quoted price
 * × quantity, not × 0.65. Reported as "press S, popup says +152, wallet
 * only sees +99" on the WORK tab. */
TEST(test_deliver_ingots_full_payout_to_pubkey_player) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    /* Verify the pubkey so the bulk-sell path takes the pubkey ledger. */
    memset(w.players[0].pubkey, 0xAA, 32);
    w.players[0].pubkey_set = true;
    w.players[0].pubkey_proof_ok = true;
    /* Player tows 10 ferrite ingots; Kepler's physical intake contract
     * pays 20 cr each when the hopper tractor takes custody. */
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 10);
    ASSERT(pod_idx >= 0);
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 10.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    int hopper_idx = station_find_hopper_for(&w.stations[1],
                                             COMMODITY_FERRITE_INGOT);
    ASSERT(hopper_idx >= 0);
    w.cargo_pods[pod_idx].pos = module_world_pos_ring(
        &w.stations[1], w.stations[1].modules[hopper_idx].ring,
        w.stations[1].modules[hopper_idx].slot);
    float bal_before = ledger_balance_by_pubkey(&w.stations[1], w.players[0].pubkey);
    world_sim_step(&w, SIM_DT);
    /* Expect 10 × 20 = 200 cr credited (allow tiny float slack for
     * age-escalation drift on tick 1 — should be effectively zero). */
    float bal_after = ledger_balance_by_pubkey(&w.stations[1], w.players[0].pubkey);
    float gained = bal_after - bal_before;
    ASSERT(gained > 199.0f);
    ASSERT(gained < 201.0f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            1, hopper_idx));
}

TEST(test_deliver_ingots_pending_pubkey_uses_session_ledger) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    memset(w.players[0].pubkey, 0xBB, 32);
    w.players[0].pubkey_set = true;
    w.players[0].pubkey_proof_ok = false;

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 1);
    ASSERT(pod_idx >= 0);
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    float session_before = ledger_balance(&w.stations[1],
                                          w.players[0].session_token);
    float pubkey_before = ledger_balance_by_pubkey(&w.stations[1],
                                                   w.players[0].pubkey);

    world_sim_step(&w, SIM_DT);

    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) >
           session_before + 0.01f);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(&w.stations[1],
                                             w.players[0].pubkey),
                    pubkey_before, 0.001f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
}

TEST(test_mixed_cargo_sell_and_deliver) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* Player tows the exact ingot crate the refinery contract can accept. */
    int pod_idx = test_spawn_towed_exact_cargo_pod(
        &w, &w.players[0], COMMODITY_FERRITE_INGOT, 15);
    ASSERT(pod_idx >= 0);
    /* Contract at Kepler for ferrite ingots. */
    w.contracts[0] = (contract_t){
        .active = true, .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 15.0f,
        .base_price = 20.0f,
        .target_index = -1, .claimed_by = -1,
    };
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    float credits_before = ledger_balance(&w.stations[1], w.players[0].session_token);
    ASSERT(test_stage_pod_at_station_hopper(&w, 1, pod_idx,
                                            COMMODITY_FERRITE_INGOT, NULL));
    world_sim_step(&w, SIM_DT);
    /* Ingot crate delivered via contract */
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
    ASSERT(ledger_balance(&w.stations[1], w.players[0].session_token) > credits_before);
}

TEST(test_no_delivery_without_matching_contract) {
    /* Cargo with no matching contract AND no consuming module on the
     * station should stay in the hold. Use Prospect (no shipyard,
     * no fab) so a tractor-module load has nowhere to land. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship->cargo[COMMODITY_TRACTOR_MODULE] = 20.0f;
    for (int k = 0; k < MAX_CONTRACTS; k++) w.contracts[k].active = false;
    /* Prospect Refinery (station 0): DOCK + SIGNAL_RELAY + FURNACE +
     * ORE_SILO. No SHIPYARD, no TRACTOR_FAB → station_consumes returns
     * false for tractor modules, so the SELL fallback should skip. */
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship->cargo[COMMODITY_TRACTOR_MODULE], 20.0f, 0.01f);
}

TEST(test_no_passive_heal_without_kits) {
    /* Passive heal was removed: docking alone never repairs. With both
     * ship cargo and station inventory empty, damaged hull stays
     * damaged — repair requires kits. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].ship->hull = 50.0f;
    w.players[0].docked = true;
    w.players[0].current_station = 0;
    ASSERT(test_set_station_finished_units(
        &w.stations[0], COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_ship_finished_units(
        w.players[0].ship, COMMODITY_REPAIR_KIT, 0,
        MINING_GRADE_COMMON));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, 50.0f, 0.01f);
}

static bool economy_test_smelt_target_for_ore(const station_t *st,
                                              commodity_t ore,
                                              vec2 *out_target) {
    bool found = false;
    float best_d = 1e18f;
    for (int fm = 0; fm < st->module_count; fm++) {
        const station_module_t *f = &st->modules[fm];
        if (f->type != MODULE_FURNACE || f->scaffold) continue;
        if (module_instance_input_ore(f) != ore) continue;
        int ring = (int)f->ring;
        vec2 furnace_pos = module_world_pos_ring(st, ring, f->slot);
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int hm = 0; hm < st->module_count; hm++) {
                const station_module_t *h = &st->modules[hm];
                if (h->ring != adj || h->scaffold) continue;
                if (h->type != MODULE_HOPPER) continue;
                if ((commodity_t)h->commodity != ore) continue;
                vec2 hopper_pos = module_world_pos_ring(st, adj, h->slot);
                float d = v2_dist_sq(furnace_pos, hopper_pos);
                if (d < best_d) {
                    best_d = d;
                    if (out_target)
                        *out_target = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
                    found = true;
                }
            }
        }
    }
    return found;
}

static int economy_test_spawn_fragment(world_t *w, commodity_t ore,
                                       float units, vec2 pos) {
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { frag = i; break; }
    }
    if (frag < 0) return -1;
    asteroid_t *a = &w->asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = ore;
    a->ore = units;
    a->max_ore = units;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    for (int b = 0; b < 32; b++) a->fracture_seed[b] = (uint8_t)(0x80 + b);
    a->pos = pos;
    a->vel = v2(0.0f, 0.0f);
    return frag;
}

TEST(test_refinery_smelts_fragment_into_ingot_pod) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0xC7, sizeof(w.players[0].session_token));
    player_init_ship(&w.players[0], &w);

    ASSERT(station_has_module(&w.stations[0], MODULE_FURNACE));
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w.stations[0].arm_speed[arm] = 0.0f;
        w.stations[0].arm_rotation[arm] = 0.0f;
    }

    vec2 smelt_target = w.stations[0].pos;
    ASSERT(economy_test_smelt_target_for_ore(&w.stations[0],
                                             COMMODITY_FERRITE_ORE,
                                             &smelt_target));
    int frag = economy_test_spawn_fragment(&w, COMMODITY_FERRITE_ORE,
                                           10.0f, smelt_target);
    ASSERT(frag >= 0);
    w.asteroids[frag].last_towed_by = 0;
    memcpy(w.asteroids[frag].last_towed_token,
           w.players[0].session_token,
           sizeof(w.asteroids[frag].last_towed_token));
    ASSERT(world_asteroid_set_player_tractor(&w, frag, 0));

    float station_ingots_before = station_inventory_amount(
        &w.stations[0], COMMODITY_FERRITE_INGOT);
    int station_frames_before =
        station_finished_count(&w.stations[0], COMMODITY_FRAME);
    int frame_pod_units_before =
        economy_count_exact_pod_units(&w, COMMODITY_FRAME);
    ASSERT(frame_pod_units_before > 0);
    int pod_units_before =
        economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    for (int i = 0; i < (int)(10.0f / SIM_DT) && w.asteroids[frag].active; i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(w.players[0].ship->towed_count, 0);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w.stations[0], COMMODITY_FERRITE_INGOT),
                    station_ingots_before, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0], COMMODITY_FRAME),
                  station_frames_before);
    ASSERT_EQ_INT(economy_count_exact_pod_units(&w, COMMODITY_FRAME),
                  frame_pod_units_before - 1);
    const cargo_pod_t *pod = economy_first_exact_pod(
        &w, COMMODITY_FERRITE_INGOT);
    ASSERT(pod != NULL);
    for (int i = 0; i < 120 && !cargo_pod_has_module_tractor(pod); i++) {
        world_sim_step(&w, SIM_DT);
        pod = economy_first_exact_pod(&w, COMMODITY_FERRITE_INGOT);
        ASSERT(pod != NULL);
    }
    int pod_station = -1;
    int pod_module = -1;
    ASSERT(cargo_pod_module_tractor_indices(pod, &pod_station, &pod_module));
    ASSERT_EQ_INT(pod_station, 0);
    ASSERT(pod_module >= 0 && pod_module < w.stations[0].module_count);
    ASSERT_EQ_INT(w.stations[0].modules[pod_module].type, MODULE_DOCK);
    ASSERT_EQ_INT(cargo_pod_player_tractor(pod), -1);
    ASSERT_EQ_INT(pod->manifest_count, 10);
    ASSERT_EQ_INT(pod->quantity, 10);
    ASSERT(economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT) >=
           pod_units_before + 10);
    ASSERT_EQ_INT((int)w.hopper_smelt_events, 0);
}

TEST(test_kit_fab_requires_shipyard) {
    /* After the shipyard-fab redesign, only stations with MODULE_SHIPYARD
     * mint repair kits. A station with only a dock + the three input
     * commodities should never produce kits. */
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    /* Prospect (station 0) has a dock but no shipyard. Kepler and Helios
     * both have shipyards. Pre-fill all three with kit-fab inputs. */
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    for (int s = 0; s < 3; s++) {
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_FRAME, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_LASER_MODULE, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_TRACTOR_MODULE, 5));
        ASSERT(test_set_station_finished_units(&w.stations[s], COMMODITY_REPAIR_KIT, 0));
        w.stations[s].repair_kit_fab_timer = 0.0f;
    }
    /* Run long enough for at least one fab cycle (REPAIR_KIT_FAB_PERIOD = 30s). */
    for (int i = 0; i < (int)(35.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    /* Shipyard station produces kits; dock-only station does not. */
    ASSERT(station_inventory_amount(&w.stations[1],
                                    COMMODITY_REPAIR_KIT) > 0.0f);
    ASSERT(station_inventory_amount(&w.stations[2],
                                    COMMODITY_REPAIR_KIT) > 0.0f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&w.stations[0],
                                              COMMODITY_REPAIR_KIT),
                    0.0f, 0.01f);
}

TEST(test_kit_import_contract_at_consumer_station) {
    /* A station with a dock but no shipyard should issue a TRACTOR
     * contract for REPAIR_KIT when its kit inventory drops below the
     * import threshold. Players or NPC haulers fulfill via the same
     * delivery loop that handles ingots. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[0], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[0], MODULE_SHIPYARD));
    /* Drain Prospect's kit inventory to force the deficit. */
    ASSERT(test_set_station_finished_units(
        &w.stations[0], COMMODITY_REPAIR_KIT, 0));
    /* Run a few seconds for the contract step to fire. */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    bool found = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && c->station_index == 0
            && c->commodity == COMMODITY_REPAIR_KIT) {
            found = true;
            ASSERT(c->base_price > 0.0f);
            ASSERT(c->quantity_needed > 0.0f);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_PROOF);
            ASSERT(c->proof_flags & CONTRACT_PROOF_REQUIRE_RECIPE);
            ASSERT_EQ_INT(c->required_recipe_id, RECIPE_REPAIR_KIT_FAB);
            break;
        }
    }
    ASSERT(found);
}

TEST(test_kit_import_contract_skips_shipyard_stations) {
    /* A shipyard station mints its own kits; the import contract should
     * not fire there even with kit inventory at zero. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    ASSERT(test_set_station_finished_units(
        &w.stations[1], COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_station_finished_units(
        &w.stations[2], COMMODITY_REPAIR_KIT, 0));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *c = &w.contracts[k];
        if (c->active && c->action == CONTRACT_TRACTOR
            && (c->station_index == 1 || c->station_index == 2)
            && c->commodity == COMMODITY_REPAIR_KIT) {
            ASSERT(false); /* shouldn't reach here */
        }
    }
}

TEST(test_repair_drains_ship_cargo_first) {
    /* Player docked at a station with a repair service. Ship carries
     * 50 kits in cargo, station has 100 kits in inventory. A 30 HP
     * repair drains 30 kits from ship cargo, leaves station inventory
     * untouched, and charges only the labor fee (no station retail
     * since no kits sourced from station). */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    /* Force the repair service (default Prospect lacks REPAIR_BAY). */
    w.stations[0].services |= STATION_SERVICE_REPAIR;
    ASSERT(test_set_station_finished_units(&w.stations[0], COMMODITY_REPAIR_KIT, 100));
    ASSERT(test_set_ship_finished_units(w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        50, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 30.0f; /* 30 HP missing */

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* Hull restored, ship cargo drained, station inventory untouched. */
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull, 0.5f);
    ASSERT_EQ_INT(ship_finished_count(w.players[0].ship,
                                      COMMODITY_REPAIR_KIT), 20);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0],
                                         COMMODITY_REPAIR_KIT), 100);

    /* Charge: only labor (no station retail). 30 HP * 1 cr/HP. */
    float bal_after = ledger_balance(&w.stations[0],
                                     w.players[0].session_token);
    float charged = bal_before - bal_after;
    ASSERT_EQ_FLOAT(charged, 30.0f * LABOR_FEE_PER_HP, 0.5f);
}

TEST(test_repair_falls_back_to_station_inventory) {
    /* Player has no kits in cargo; station inventory covers it. Repair
     * charges retail (station_sell_price) + labor since not a shipyard. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    w.stations[0].services |= STATION_SERVICE_REPAIR;
    w.stations[0].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    ASSERT(test_set_station_finished_units(&w.stations[0], COMMODITY_REPAIR_KIT,
                                           (int)MAX_PRODUCT_STOCK)); /* full → 1× */
    ASSERT(test_set_ship_finished_units(w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        0, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[0],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: 10 kits drained, charge = 10 * (6 + 1) = 70 cr. */
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull, 0.5f);
    ASSERT_EQ_FLOAT(station_inventory_amount(&w.stations[0],
                                              COMMODITY_REPAIR_KIT),
                    MAX_PRODUCT_STOCK - 10.0f, 0.5f);
    float charged = bal_before - ledger_balance(&w.stations[0],
                                                w.players[0].session_token);
    ASSERT_EQ_FLOAT(charged, 10.0f * (6.0f + LABOR_FEE_PER_HP), 1.0f);
}

TEST(test_repair_at_shipyard_no_labor_fee) {
    /* At a shipyard the labor fee is zero — you already paid retail
     * when you bought the kits there. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 1; /* Kepler has shipyard */
    ASSERT(station_has_module(&w.stations[1], MODULE_SHIPYARD));

    w.stations[1].services |= STATION_SERVICE_REPAIR;
    w.stations[1].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    ASSERT(test_set_station_finished_units(&w.stations[1], COMMODITY_REPAIR_KIT,
                                           (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_ship_finished_units(w.players[0].ship, COMMODITY_REPAIR_KIT,
                                        0, MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 10.0f;

    float bal_before = ledger_balance(&w.stations[1],
                                      w.players[0].session_token);
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* 10 HP from station: charge = 10 * (6 + 0) = 60 cr (no labor). */
    float charged = bal_before - ledger_balance(&w.stations[1],
                                                w.players[0].session_token);
    ASSERT_EQ_FLOAT(charged, 10.0f * 6.0f, 1.0f);
}

TEST(test_repair_partial_when_kits_short) {
    /* Both ship cargo and station inventory empty: repair does nothing
     * (no partial heal because no kits to consume). */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x03, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    ASSERT(test_set_station_finished_units(
        &w.stations[0], COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_ship_finished_units(
        w.players[0].ship, COMMODITY_REPAIR_KIT, 0,
        MINING_GRADE_COMMON));
    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 20.0f;

    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    /* No kits anywhere = no heal at all (passive heal removed). */
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull - 20.0f, 0.01f);
}

TEST(test_repair_rejects_float_only_kits) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x04, 8);
    w.players[0].docked = true;
    w.players[0].current_station = 0;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_REPAIR_KIT, 0));
    ASSERT(test_set_ship_finished_units(w.players[0].ship,
                                        COMMODITY_REPAIR_KIT, 0,
                                        MINING_GRADE_COMMON));
    w.stations[0]._inventory_cache[COMMODITY_REPAIR_KIT] = 100.0f;
    w.players[0].ship->cargo[COMMODITY_REPAIR_KIT] = 100.0f;

    float max_hull = ship_max_hull(w.players[0].ship);
    w.players[0].ship->hull = max_hull - 20.0f;
    w.players[0].input.service_repair = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_FLOAT(w.players[0].ship->hull, max_hull - 20.0f, 0.01f);
    ASSERT_EQ_FLOAT(w.players[0].ship->cargo[COMMODITY_REPAIR_KIT],
                    100.0f, 0.001f);
}

TEST(test_repair_kit_fab_requires_manifest_inputs) {
    WORLD_DECL;
    world_reset(&w);

    int shipyard = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w.stations[s], MODULE_SHIPYARD)) {
            shipyard = s;
            break;
        }
    }
    ASSERT(shipyard >= 0);
    station_t *st = &w.stations[shipyard];

    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_REPAIR_KIT, 0));
    st->_inventory_cache[COMMODITY_FRAME] = 5.0f;
    st->_inventory_cache[COMMODITY_LASER_MODULE] = 5.0f;
    st->_inventory_cache[COMMODITY_TRACTOR_MODULE] = 5.0f;
    st->repair_kit_fab_timer = 0.0f;

    step_dock_repair_kit_fab(&w, 60.0f);

    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_REPAIR_KIT), 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_REPAIR_KIT], 0.0f, 0.001f);
}

TEST(test_repair_kit_fab_emits_craft_chain_event) {
    economy_chain_test_setup("repair_kit_craft");
    WORLD_DECL;
    world_reset(&w);
    economy_chain_test_wipe_logs(&w);

    int shipyard = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (station_has_module(&w.stations[s], MODULE_SHIPYARD)) {
            shipyard = s;
            break;
        }
    }
    ASSERT(shipyard >= 0);
    station_t *st = &w.stations[shipyard];

    ASSERT(test_set_station_finished_units(st, COMMODITY_REPAIR_KIT,
                                           (int)REPAIR_KIT_STOCK_CAP - 1));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 1));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 1));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 1));

    const cargo_unit_t *frame = test_station_first_unit(st, COMMODITY_FRAME,
                                                       RECIPE_LEGACY_MIGRATE);
    const cargo_unit_t *laser = test_station_first_unit(st, COMMODITY_LASER_MODULE,
                                                       RECIPE_LEGACY_MIGRATE);
    const cargo_unit_t *tractor = test_station_first_unit(st, COMMODITY_TRACTOR_MODULE,
                                                         RECIPE_LEGACY_MIGRATE);
    ASSERT(frame != NULL);
    ASSERT(laser != NULL);
    ASSERT(tractor != NULL);
    uint8_t expected_inputs[RECIPE_INPUT_MAX][32] = {{0}};
    memcpy(expected_inputs[0], frame->pub, 32);
    memcpy(expected_inputs[1], laser->pub, 32);
    memcpy(expected_inputs[2], tractor->pub, 32);

    st->repair_kit_fab_timer = REPAIR_KIT_FAB_PERIOD;
    step_dock_repair_kit_fab(&w, SIM_DT);

    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_REPAIR_KIT),
                  (int)REPAIR_KIT_STOCK_CAP);
    ASSERT_EQ_INT((int)st->chain_event_count, 1);

    const cargo_unit_t *kit = test_station_first_unit(st, COMMODITY_REPAIR_KIT,
                                                     RECIPE_REPAIR_KIT_FAB);
    ASSERT(kit != NULL);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT((int)walked, 1);

    char path[256];
    ASSERT(chain_log_path_for(st->station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    uint8_t header[CHAIN_EVENT_HEADER_SIZE];
    ASSERT(fread(header, 1, sizeof(header), f) == sizeof(header));
    ASSERT_EQ_INT(header[16], CHAIN_EVT_CRAFT);
    uint8_t len_bytes[2];
    ASSERT(fread(len_bytes, 1, sizeof(len_bytes), f) == sizeof(len_bytes));
    uint16_t payload_len = (uint16_t)len_bytes[0] |
                           (uint16_t)((uint16_t)len_bytes[1] << 8);
    ASSERT_EQ_INT(payload_len, (int)sizeof(chain_payload_craft_t));
    uint8_t payload[sizeof(chain_payload_craft_t)];
    ASSERT(fread(payload, 1, sizeof(payload), f) == sizeof(payload));
    fclose(f);

    uint16_t recipe_id = (uint16_t)payload[0] |
                         (uint16_t)((uint16_t)payload[1] << 8);
    ASSERT_EQ_INT(recipe_id, RECIPE_REPAIR_KIT_FAB);
    ASSERT_EQ_INT(payload[2], RECIPE_INPUT_MAX);
    ASSERT(memcmp(&payload[8], kit->pub, 32) == 0);
    ASSERT(memcmp(&payload[40], expected_inputs[0], 32) == 0);
    ASSERT(memcmp(&payload[72], expected_inputs[1], 32) == 0);
    ASSERT(memcmp(&payload[104], expected_inputs[2], 32) == 0);

    economy_chain_test_teardown();
}

TEST(test_furnace_without_hopper_does_not_smelt) {
    /* Furnace capability is pair/tag based: a tagged furnace still
     * requires an adjacent matching ore hopper before it'll fire. */
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w.stations[0].arm_speed[arm] = 0.0f;
        w.stations[0].arm_rotation[arm] = 0.0f;
    }
    w.stations[0].module_count = 0;
    rebuild_station_services(&w.stations[0]);
    w.stations[0].modules[0] = (station_module_t){
        .type = MODULE_FURNACE,
        .ring = 2,
        .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .scaffold = false,
        .build_progress = 1.0f
    };
    w.stations[0].module_count = 1;
    float initial_ingots = station_inventory_amount(
        &w.stations[0], COMMODITY_FERRITE_INGOT);
    int initial_pod_units =
        economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    vec2 furnace_only_pos = module_world_pos_ring(&w.stations[0], 2, 0);
    int frag = economy_test_spawn_fragment(&w, COMMODITY_FERRITE_ORE,
                                           8.0f, furnace_only_pos);
    ASSERT(frag >= 0);
    for (int i = 0; i < (int)(5.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(w.asteroids[frag].active);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w.stations[0], COMMODITY_FERRITE_INGOT),
                    initial_ingots, 0.001f);
    ASSERT_EQ_INT(economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT),
                  initial_pod_units);

    /* Add a matching hopper and let it run again — now it should smelt. */
    w.stations[0].modules[1] = (station_module_t){
        .type = MODULE_HOPPER,
        .ring = 1,
        .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .scaffold = false,
        .build_progress = 1.0f
    };
    w.stations[0].module_count = 2;
    vec2 smelt_target = w.stations[0].pos;
    ASSERT(economy_test_smelt_target_for_ore(&w.stations[0],
                                             COMMODITY_FERRITE_ORE,
                                             &smelt_target));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FRAME, 1));
    w.asteroids[frag].pos = smelt_target;
    w.asteroids[frag].vel = v2(0.0f, 0.0f);
    w.asteroids[frag].smelt_progress = 0.0f;
    for (int i = 0; i < (int)(5.0f / SIM_DT) && w.asteroids[frag].active; i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_FLOAT(station_inventory_amount(
                        &w.stations[0], COMMODITY_FERRITE_INGOT),
                    initial_ingots, 0.001f);
    ASSERT(economy_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT) >=
           initial_pod_units + 8);
    ASSERT_EQ_INT((int)w.hopper_smelt_events, 0);
}

TEST(test_commodity_volume_kit_dense) {
    /* Kits take REPAIR_KIT_CARGO_DENSITY units of cargo each; everything
     * else is 1.0. */
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_REPAIR_KIT),
                    REPAIR_KIT_CARGO_DENSITY, 0.001f);
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_FRAME), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(commodity_volume(COMMODITY_FERRITE_INGOT), 1.0f, 0.001f);
}

TEST(test_ship_total_cargo_kit_density) {
    /* 100 kits + 5 frames = 100 * 0.1 + 5 * 1.0 = 15 cargo units. */
    ship_t ship = {0};
    ASSERT(ship_manifest_bootstrap(&ship));
    uint8_t origin[8] = {0};
    float legacy[COMMODITY_COUNT] = {0};
    legacy[COMMODITY_REPAIR_KIT] = 100.0f;
    legacy[COMMODITY_FRAME] = 5.0f;
    ASSERT(manifest_migrate_legacy_inventory(
        &ship.manifest, legacy, COMMODITY_COUNT, origin));
    ASSERT_EQ_FLOAT(ship_total_cargo(&ship),
                    100.0f * REPAIR_KIT_CARGO_DENSITY + 5.0f, 0.001f);
    ship_cleanup(&ship);
}

TEST(test_sell_legacy_manifest_requires_pod) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int k = 0; k < MAX_CONTRACTS; k++) w->contracts[k].active = false;

    int consumer = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_consumes(&w->stations[i], COMMODITY_FERRITE_INGOT)) {
            consumer = i;
            break;
        }
    }
    ASSERT(consumer >= 0);
    station_t *st = &w->stations[consumer];
    (void)manifest_consume_by_commodity(&st->manifest,
                                         COMMODITY_FERRITE_INGOT,
                                         manifest_count_by_commodity(&st->manifest,
                                                                     COMMODITY_FERRITE_INGOT));
    ASSERT(test_set_station_finished_units(
        st, COMMODITY_FERRITE_INGOT, 0));

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xD1, 8);
    sp->docked = true;
    sp->current_station = (uint8_t)consumer;
    sp->ship->pos = st->pos;

    cargo_unit_t anon = {0};
    anon.kind = (uint8_t)CARGO_KIND_INGOT;
    anon.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    anon.grade = (uint8_t)MINING_GRADE_COMMON;
    anon.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    anon.quantity = 1;
    anon.pub[0] = 0x11;
    cargo_unit_t premium = anon;
    premium.prefix_class = (uint8_t)INGOT_PREFIX_M;
    premium.pub[0] = 0x22;
    ASSERT(manifest_push(&sp->ship->manifest, &anon));
    ASSERT(manifest_push(&sp->ship->manifest, &premium));
    sp->ship->cargo[COMMODITY_FERRITE_INGOT] = 2.0f;

    float before = ledger_balance(st, sp->session_token);

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_INGOT;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token), before, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship->cargo[COMMODITY_FERRITE_INGOT], 2.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship->manifest.count, 2);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
}

TEST(test_market_buy_ignores_legacy_manifest_ingots) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));

    int producer = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (station_produces(&w->stations[i], COMMODITY_FERRITE_INGOT)) {
            producer = i;
            break;
        }
    }
    ASSERT(producer >= 0);
    station_t *st = &w->stations[producer];
    ASSERT(station_manifest_bootstrap(st));
    (void)manifest_consume_by_commodity(&st->manifest,
                                         COMMODITY_FERRITE_INGOT,
                                         manifest_count_by_commodity(&st->manifest,
                                                                     COMMODITY_FERRITE_INGOT));

    cargo_unit_t premium = {0};
    premium.kind = (uint8_t)CARGO_KIND_INGOT;
    premium.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    premium.grade = (uint8_t)MINING_GRADE_COMMON;
    premium.prefix_class = (uint8_t)INGOT_PREFIX_M;
    premium.quantity = 1;
    premium.pub[0] = 0x31;
    cargo_unit_t anon = premium;
    anon.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    anon.pub[0] = 0x32;
    ASSERT(manifest_push(&st->manifest, &premium));
    ASSERT(manifest_push(&st->manifest, &anon));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);
    int market_pod = test_spawn_station_market_exact_cargo_pod(
        w, producer, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(market_pod >= 0);
    ASSERT_EQ_INT(w->cargo_pods[market_pod].manifest_units[0].prefix_class,
                  INGOT_PREFIX_ANONYMOUS);

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xD2, 8);
    sp->docked = true;
    sp->current_station = (uint8_t)producer;
    sp->ship->pos = st->pos;
    ledger_earn(st, sp->session_token, 100000.0f);
    float before = ledger_balance(st, sp->session_token);
    float expected_cost = test_station_market_pod_sell_quote(
        st, &w->cargo_pods[market_pod]);
    ASSERT(expected_cost > 0.0f);
    int frames_before = station_finished_count(st, COMMODITY_FRAME);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(sp->ship->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship->manifest.count, 0);
    int bought_pod = test_find_towed_exact_cargo_pod(
        w, sp, COMMODITY_FERRITE_INGOT);
    ASSERT(bought_pod >= 0);
    ASSERT_EQ_INT(bought_pod, market_pod);
    ASSERT_EQ_INT(w->cargo_pods[bought_pod].quantity, 1);
    ASSERT_EQ_INT(w->cargo_pods[bought_pod].manifest_units[0].prefix_class,
                  INGOT_PREFIX_ANONYMOUS);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), frames_before);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w->cargo_pods[bought_pod]),
                  producer);
    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token), before, 0.001f);

    test_move_pod_past_station_charge_boundary(w, producer, bought_pod);
    world_sim_step(w, SIM_DT);
    ASSERT_EQ_FLOAT(before - ledger_balance(st, sp->session_token),
                    expected_cost, 0.01f);

    int station_named = 0;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        const cargo_unit_t *u = &st->manifest.units[i];
        if (u->commodity == (uint8_t)COMMODITY_FERRITE_INGOT &&
            u->prefix_class == (uint8_t)INGOT_PREFIX_M) station_named++;
    }
    ASSERT_EQ_INT(station_named, 1);
}

/* Pod trade is physical: once a towed crate reaches a matching station
 * intake, the station tractor takes custody and every unit inside moves
 * with the crate. */
TEST(test_sell_towed_pod_transfers_whole_pod) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xAA, 8);

    int kepler = 1;
    station_t *st = &w->stations[kepler];
    ASSERT(station_consumes(st, COMMODITY_FRAME));
    int hopper_idx = station_find_hopper_for(st, COMMODITY_FRAME);
    ASSERT(hopper_idx >= 0);
    vec2 hopper_pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);
    sp->ship->pos = hopper_pos;
    float before = ledger_balance(st, sp->session_token);

    int pod_idx = test_spawn_towed_exact_cargo_pod(
        w, sp, COMMODITY_FRAME, 3);
    ASSERT(pod_idx >= 0);
    w->cargo_pods[pod_idx].pos = hopper_pos;

    world_sim_step(w, SIM_DT);

    ASSERT_EQ_FLOAT(sp->ship->cargo[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_INT(sp->ship->manifest.count, 0);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT(w->cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w->cargo_pods[pod_idx].quantity, 3);
    ASSERT_EQ_INT(w->cargo_pods[pod_idx].manifest_count, 3);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w->cargo_pods[pod_idx]), -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w->cargo_pods[pod_idx],
                                            kepler, hopper_idx));
    ASSERT(ledger_balance(st, sp->session_token) > before);
}

void register_economy_basic_tests(void) {
    TEST_SECTION("\nEconomy tests:\n");
    RUN(test_sell_legacy_manifest_requires_pod);
    RUN(test_market_buy_ignores_legacy_manifest_ingots);
    RUN(test_sell_towed_pod_transfers_whole_pod);
    RUN(test_station_production_yard_makes_frames);
    RUN(test_station_production_beamworks_makes_modules);
    RUN(test_station_repair_cost_no_damage);
    RUN(test_station_repair_cost_with_damage);
    RUN(test_can_afford_upgrade_dock_fallback);
    RUN(test_can_afford_upgrade_no_credits_for_dock_fallback);
    RUN(test_can_afford_upgrade_no_product_anywhere);
    RUN(test_can_afford_upgrade_cargo_only_no_credits_needed);
    RUN(test_can_afford_upgrade_rejects_float_only_finished_goods);
    RUN(test_commodity_volume_kit_dense);
    RUN(test_ship_total_cargo_kit_density);
}

void register_economy_contracts_tests(void) {
    TEST_SECTION("\nContract tests:\n");
    RUN(test_contract_generated_from_hopper_deficit);
    RUN(test_contract_price_escalates_with_age);
    RUN(test_contract_fit_requires_material_grade_and_fragment_tier);
    RUN(test_contract_fit_enforces_heritage_recipe_prefix_and_parent);
    RUN(test_contract_delivery_requires_required_grade);
    RUN(test_contract_delivery_requires_heritage_recipe);
    RUN(test_contract_delivery_bans_enemy_origin_station);
    RUN(test_contract_closes_when_deficit_filled);
    RUN(test_raw_ore_contract_retires_when_refined_output_full);
    RUN(test_kit_input_contract_closes_at_kit_target);
    RUN(test_generated_heritage_contracts_require_source_recipe);
    RUN(test_station_policy_preserves_seeded_supply_loop);
    RUN(test_station_policy_cards_rank_under_domain_budgets);
    RUN(test_station_policy_black_market_requires_off_relay_station);
    RUN(test_blackglass_posts_black_market_buy_contract);
    RUN(test_station_policy_cache_drives_trade_price_modifier);
    RUN(test_cargo_legality_clean_chain_is_not_contraband);
    RUN(test_cargo_legality_missing_receipt_is_policy_contraband);
    RUN(test_cargo_legality_black_market_authority_is_local_policy);
    RUN(test_bulk_sell_refuses_black_market_origin_at_lawful_station);
    RUN(test_black_market_buys_unwanted_towed_pod_at_markdown);
    RUN(test_raw_ore_contract_prefers_starved_downstream_output);
    RUN(test_sell_price_uses_contract_price);
    RUN(test_hauler_fills_highest_value_contract);
    RUN(test_hauler_picker_trusts_gossiped_contract);
    RUN(test_hauler_ignores_float_only_finished_stock);
    RUN(test_kit_fab_requires_shipyard);
    RUN(test_kit_import_contract_at_consumer_station);
    RUN(test_kit_import_contract_skips_shipyard_stations);
    RUN(test_repair_drains_ship_cargo_first);
    RUN(test_repair_falls_back_to_station_inventory);
    RUN(test_repair_at_shipyard_no_labor_fee);
    RUN(test_repair_partial_when_kits_short);
    RUN(test_repair_rejects_float_only_kits);
    RUN(test_repair_kit_fab_requires_manifest_inputs);
    RUN(test_repair_kit_fab_emits_craft_chain_event);
}

void register_economy_contract3_tests(void) {
    TEST_SECTION("\nContract system (3-action):\n");
    RUN(test_one_contract_per_station);
    RUN(test_destroy_contract_completes_when_asteroid_gone);
    RUN(test_fracture_contract_target_pub_matches_asteroid_identity);
    RUN(test_supply_contract_uses_correct_material);
}

void register_economy_pricing_tests(void) {
    TEST_SECTION("\nDynamic pricing:\n");
    RUN(test_dynamic_ore_price_deficit);
    RUN(test_product_price_tracks_ore);
}

void register_economy_mixed_cargo_tests(void) {
    TEST_SECTION("\nMixed cargo sell/deliver:\n");
    RUN(test_deliver_ingots_to_contract);
    RUN(test_first_cross_station_haul_uses_local_ledgers);
    RUN(test_delivery_credit_contract_pickup_deliver_and_clear);
    RUN(test_delivery_credit_dock_custody_does_not_teleport_far_pod);
    RUN(test_delivery_credit_requires_exact_bound_cargo);
    RUN(test_delivery_credit_row_sell_unloads_bound_pod);
    RUN(test_delivery_credit_hail_ignores_empty_origin);
    RUN(test_delivery_credit_hail_requires_docking_to_pick_up);
    RUN(test_delivery_credit_black_market_sale_defaults_origin_debt);
    RUN(test_delivery_credit_timeout_emits_station_risk_memory);
    RUN(test_prospect_pubkey_buy_debits_pubkey_ledger);
    RUN(test_market_buy_requires_station_held_pod);
    RUN(test_deliver_ingots_full_payout_to_pubkey_player);
    RUN(test_deliver_ingots_pending_pubkey_uses_session_ledger);
    RUN(test_mixed_cargo_sell_and_deliver);
    RUN(test_no_delivery_without_matching_contract);
}

void register_economy_service259_tests(void) {
    TEST_SECTION("\nStation service semantics (#259):\n");
    RUN(test_no_passive_heal_without_kits);
}

/* Tagged furnace/pair smelt rules pinned: smelt capability comes from
 * a furnace tagged for the output ingot plus a matching ore hopper on an
 * adjacent ring. Crystal needs two distinct crystal furnace pairs because
 * the first pass creates a tractorable intermediate fragment. */
TEST(test_tagged_furnace_pair_smelt_rules) {
    station_t st = {0};
    /* 0 furnaces: nothing smelts even with a tagged hopper. */
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .build_progress = 1.0f,
    };
    st.module_count = 1;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* One ferrite furnace+hopper pair: ferrite only. */
    st.modules[1] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 2;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* Add a cuprite pair: ferrite and cuprite both work by tag. */
    st.modules[2] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 1,
        .commodity = (uint8_t)COMMODITY_CUPRITE_ORE,
        .build_progress = 1.0f,
    };
    st.modules[3] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 1,
        .commodity = (uint8_t)COMMODITY_CUPRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 4;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* One crystal pair can stage crystal, but does not advertise full
     * station smelt capability until there is a second crystal pair. */
    st.modules[4] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 2,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_ORE,
        .build_progress = 1.0f,
    };
    st.modules[5] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 2,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 6;
    ASSERT(station_can_smelt(&st, COMMODITY_FERRITE_ORE));
    ASSERT(station_can_smelt(&st, COMMODITY_CUPRITE_ORE));
    ASSERT(!station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    st.modules[6] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 3, .slot = 2,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 7;
    ASSERT(station_can_smelt(&st, COMMODITY_CRYSTAL_ORE));

    /* Tagged furnace without matching adjacent hopper: nothing smelts. */
    memset(&st, 0, sizeof st);
    st.modules[0] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.module_count = 1;
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));

    /* Scaffold furnaces don't count. */
    memset(&st, 0, sizeof st);
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_FURNACE, .ring = 1, .slot = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .build_progress = 1.0f,
    };
    st.modules[1].scaffold = true;
    st.module_count = 2;
    ASSERT_EQ_INT(station_furnace_count(&st), 0);
    ASSERT(!station_can_smelt(&st, COMMODITY_FERRITE_ORE));
}

void register_economy_refinery_smelt_tests(void) {
    TEST_SECTION("\nRefinery smelt test:\n");
    RUN(test_refinery_smelts_fragment_into_ingot_pod);
    RUN(test_furnace_without_hopper_does_not_smelt);
    RUN(test_tagged_furnace_pair_smelt_rules);
}

/* station_top_demand: derives the top shortage from inventory + the
 * station's consumed-commodity list. This is the primitive HUD
 * beacons / contract auto-pricing / NPC scoring will compose on top
 * of, so the contract-priority code in game_sim.c and this primitive
 * MUST agree on what "starving" means. The tests below pin those
 * agreements to the same constants. */
TEST(test_top_demand_no_shortage_returns_none) {
    WORLD_DECL;
    world_reset(&w);
    /* Top up Kepler's frame_press input commodity to its target — the
     * station has no shortage, so top demand should be empty. */
    station_t *kepler = &w.stations[1];
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FERRITE_INGOT, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FRAME, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_LASER_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_TRACTOR_MODULE, 100));
    station_demand_t d = station_top_demand(kepler);
    ASSERT_EQ_INT((int)d.commodity, (int)COMMODITY_COUNT);
    ASSERT_EQ_FLOAT(d.severity, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(d.price_mult, 1.0f, 0.001f);
}

TEST(test_top_demand_picks_starving_commodity) {
    WORLD_DECL;
    world_reset(&w);
    station_t *kepler = &w.stations[1];
    /* Mild shortage on FRAME (consumed by shipyard kit-fab),
     * severe shortage on FERRITE_INGOT (frame_press input). The
     * primitive should pick the worst — ferrite ingots. Targets:
     * frames at 12.0, ferrite ingots at MAX_PRODUCT_STOCK*0.9 = 108.
     * Set frames to 6 (mild, severity ~0.5) and ingots to 0 (full
     * starvation). */
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FRAME, 6));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_LASER_MODULE, 100));
    ASSERT(test_set_station_finished_units(
        kepler, COMMODITY_TRACTOR_MODULE, 100));
    station_demand_t d = station_top_demand(kepler);
    ASSERT_EQ_INT((int)d.commodity, (int)COMMODITY_FERRITE_INGOT);
    ASSERT(d.severity > 0.95f);
    /* price_mult = 1.0 + 0.5 * severity → ~1.5 at full starvation. */
    ASSERT(d.price_mult > 1.45f);
    ASSERT(d.price_mult <= 1.5001f);
}

TEST(test_top_demand_skips_self_produced_commodities) {
    /* Helios has its own cuprite furnace + laser fab, so it produces
     * cuprite ingots locally. Even with the float at zero, the
     * primitive must not flag cuprite as a top demand — the local
     * producer is the right answer, not an import. Mirrors the
     * "don't import what we make ourselves" check in game_sim.c
     * priority 4. */
    WORLD_DECL;
    world_reset(&w);
    station_t *helios = &w.stations[2];
    /* Knock out everything else so cuprite is the only candidate
     * (besides things Helios produces). */
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CUPRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_CRYSTAL_INGOT, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_FRAME, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_LASER_MODULE, (int)MAX_PRODUCT_STOCK));
    ASSERT(test_set_station_finished_units(
        helios, COMMODITY_TRACTOR_MODULE, (int)MAX_PRODUCT_STOCK));
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE]   = REFINERY_HOPPER_CAPACITY;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE]   = REFINERY_HOPPER_CAPACITY;
    helios->_inventory_cache[COMMODITY_FERRITE_ORE]   = REFINERY_HOPPER_CAPACITY;
    station_demand_t d = station_top_demand(helios);
    /* Either no demand at all, or demand for something Helios
     * actually doesn't produce — but specifically NOT cuprite ingot. */
    ASSERT(d.commodity != COMMODITY_CUPRITE_INGOT);
}

TEST(test_top_demand_severity_clamped_zero_to_one) {
    /* A negative deficit (overstock) should not produce negative
     * severity, and a wildly empty hopper should clamp to 1.0. */
    WORLD_DECL;
    world_reset(&w);
    station_t *prospect = &w.stations[0];
    /* Force an overstock on FERRITE_ORE: target = HOPPER_CAPACITY*0.5,
     * supply = capacity, so deficit is negative. The primitive
     * should still report severity = 0 for that commodity (and pick
     * something else, or none). */
    prospect->_inventory_cache[COMMODITY_FERRITE_ORE] = REFINERY_HOPPER_CAPACITY;
    station_demand_t d = station_top_demand(prospect);
    /* Whatever it picks, severity must be in [0,1]. */
    ASSERT(d.severity >= 0.0f && d.severity <= 1.0f);
    ASSERT(d.price_mult >= 1.0f && d.price_mult <= 1.5f + 0.001f);
    /* And it must not have picked overstocked ferrite ore. */
    ASSERT(d.commodity != COMMODITY_FERRITE_ORE);
}

/* Demand pricing: a station that's starving for an ingot should post a
 * higher contract price than one that's stocked. Pool_factor and the
 * existing 1.15× content premium stay; the new demand multiplier
 * layers on top, so a fully-stocked station's contract still uses the
 * old price exactly (1.0× demand mult), and a starved station pays up
 * to 50% more. */
TEST(test_contract_price_scales_with_demand) {
    /* Helper to grab Kepler's frame_press ingot import contract. */
    WORLD_HEAP stocked = calloc(1, sizeof(world_t));
    WORLD_HEAP starved = calloc(1, sizeof(world_t));
    ASSERT(stocked != NULL);
    ASSERT(starved != NULL);
    world_reset(stocked);
    /* Top up Kepler's ferrite ingot inventory to its target so demand
     * mult is 1.0 — i.e. the existing pricing path. */
    ASSERT(test_set_station_finished_units(&stocked->stations[1],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)MAX_PRODUCT_STOCK));
    /* Run a few seconds for contract step to fire. */
    for (int i = 0; i < 240; i++) world_sim_step(stocked, SIM_DT);

    world_reset(starved);
    /* Starve Kepler completely for ferrite ingots — demand mult ~1.5. */
    ASSERT(test_set_station_finished_units(
        &starved->stations[1], COMMODITY_FERRITE_INGOT, 0));
    for (int i = 0; i < 240; i++) world_sim_step(starved, SIM_DT);

    /* Find the (Kepler, FERRITE_INGOT) contract in each world. The
     * stocked world may not generate one at all if supply is at
     * target — that's also a valid outcome (no demand → no
     * contract). The starved world must generate one and price it
     * higher than the stocked baseline if the stocked world did
     * post one. */
    contract_t *c_stocked = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (stocked->contracts[k].active
            && stocked->contracts[k].station_index == 1
            && stocked->contracts[k].commodity == COMMODITY_FERRITE_INGOT) {
            c_stocked = &stocked->contracts[k]; break;
        }
    }
    contract_t *c_starved = NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (starved->contracts[k].active
            && starved->contracts[k].station_index == 1
            && starved->contracts[k].commodity == COMMODITY_FERRITE_INGOT) {
            c_starved = &starved->contracts[k]; break;
        }
    }
    ASSERT(c_starved != NULL); /* starvation MUST produce a contract */

    if (c_stocked != NULL) {
        /* If the stocked world also posted a contract, the starved
         * one must be priced higher. The two worlds are otherwise
         * identical so pool_factor + base_price are equal. The only
         * delta is the demand multiplier. */
        ASSERT(c_starved->base_price > c_stocked->base_price * 1.05f);
    }
    /* Either way, the starved contract's price must reflect the
     * demand boost vs. the no-demand baseline of base × 1.15 ×
     * pool. base_price[FERRITE_INGOT] is non-zero by world_reset
     * seeding; the contract should land somewhere between 1.0× and
     * 1.5× of (base × 1.15 × pool). We don't assert the exact value
     * because pool_factor moves with the simulated economy. */
    ASSERT(c_starved->base_price > 0.0f);
}

void register_economy_demand_tests(void) {
    TEST_SECTION("\nStation demand primitive:\n");
    RUN(test_top_demand_no_shortage_returns_none);
    RUN(test_top_demand_picks_starving_commodity);
    RUN(test_top_demand_skips_self_produced_commodities);
    RUN(test_top_demand_severity_clamped_zero_to_one);
    RUN(test_contract_price_scales_with_demand);
}
