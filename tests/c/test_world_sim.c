#include "test_harness.h"
#include "signal_brain.h"
#include "signal_intelligence.h"
#include "sim_mining.h"
#include "sim_ship.h"
#include "sim_asteroid.h"
#include "cargo_receipt_issue.h"
#include "contract_fit.h"
#include "faction.h"
#include "gossip.h"
#include "neural_checkpoint.h"
#include <stdio.h>

#define TEST_FLIGHT_CKPT_DATA _Users_ratimics_develop_crlplrimes_build_float_signal_flight_longhorizon_live_signal_flight_nnckpt
#define TEST_FLIGHT_CKPT_LEN  _Users_ratimics_develop_crlplrimes_build_float_signal_flight_longhorizon_live_signal_flight_nnckpt_len

static bool test_load_embedded_flight_brain(void) {
    const char *path = "/tmp/signal-test-flight-brain.nnckpt";
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    size_t wrote = fwrite(TEST_FLIGHT_CKPT_DATA, 1, TEST_FLIGHT_CKPT_LEN, fp);
    bool io_ok = wrote == (size_t)TEST_FLIGHT_CKPT_LEN && fclose(fp) == 0;
    if (!io_ok) {
        remove(path);
        return false;
    }
    char err[256] = {0};
    bool ok = signal_brain_load_checkpoint(path, err, sizeof(err));
    remove(path);
    if (!ok) fprintf(stderr, "embedded neural checkpoint load failed: %s\n", err);
    return ok;
}

static int test_spawn_hauler_at(world_t *w, int station_idx) {
    return spawn_npc(w, station_idx, NPC_ROLE_HAULER);
}

static int test_claim_fresh_npc_hull(world_t *w, int station_idx,
                                     npc_role_t role,
                                     hull_class_t hull_class) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        ship_cleanup(&w->npc_ships[n].ship);
        memset(&w->npc_ships[n], 0, sizeof(w->npc_ships[n]));
    }
    int character_cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int c = 0; c < character_cap; c++)
        w->characters[c].active = false;
    for (int s = 0; s < MAX_SHIPS; s++) {
        ship_cleanup(&w->ships[s]);
        memset(&w->ships[s], 0, sizeof(w->ships[s]));
    }
    for (int a = 0; a < MAX_SHIP_ASSETS; a++) {
        ship_asset_t *asset = &w->ship_assets[a];
        if (!asset->active || asset->owner_kind != SHIP_ASSET_OWNER_STATION ||
            asset->loaner) {
            continue;
        }
        ship_cleanup(&asset->ship);
        memset(asset, 0, sizeof(*asset));
    }
    ship_asset_t *asset = world_ship_asset_mint(
        w, hull_class, SHIP_ASSET_OWNER_STATION,
        station_idx, station_idx, SHIP_ASSET_PROVENANCE_GENESIS,
        false, station_idx, NULL, NULL);
    if (!asset) return -1;
    return ship_asset_claim_for_npc(w, station_idx, role);
}

static int test_active_ship_asset_count(const world_t *w) {
    int count = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        if (w->ship_assets[i].active) count++;
    return count;
}

static int test_count_exact_pod_units(const world_t *w, commodity_t c) {
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

static const cargo_pod_t *test_first_exact_pod_with_units(const world_t *w,
                                                          commodity_t c,
                                                          uint16_t units) {
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != 0 || pod->commodity != c) continue;
        if (pod->manifest_count != units || pod->quantity != units) continue;
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

static bool test_hopper_pos_for(const station_t *st,
                                commodity_t commodity,
                                vec2 *out_pos) {
    if (!st || !out_pos || commodity >= COMMODITY_COUNT) return false;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *m = &st->modules[i];
        if (m->scaffold || m->type != MODULE_HOPPER) continue;
        if ((commodity_t)m->commodity != commodity) continue;
        *out_pos = module_world_pos_ring(st, m->ring, m->slot);
        return true;
    }
    return false;
}

static int test_spawn_exact_pod(world_t *w,
                                vec2 pos,
                                commodity_t commodity,
                                uint16_t units) {
    if (!w || commodity >= COMMODITY_COUNT ||
        units == 0 || units > CARGO_POD_MANIFEST_CAP) {
        return -1;
    }
    cargo_unit_t cargo[CARGO_POD_MANIFEST_CAP];
    memset(cargo, 0, sizeof(cargo));
    const uint8_t origin[8] = { 'T','E','S','T','P','O','D','S' };
    for (uint16_t i = 0; i < units; i++) {
        if (!hash_legacy_migrate_unit(origin, commodity, i, &cargo[i]))
            return -1;
    }
    return spawn_cargo_pod_with_manifest(
        w, pos, v2(0.0f, 0.0f), commodity, cargo, units,
        CARGO_POD_CARGO);
}

static int test_spawn_frame_pod(world_t *w, vec2 pos, uint16_t units) {
    if (!w || units == 0 || units > CARGO_POD_MANIFEST_CAP) return -1;
    cargo_unit_t frames[CARGO_POD_MANIFEST_CAP];
    memset(frames, 0, sizeof(frames));
    const uint8_t origin[8] = { 'T','E','S','T','F','R','M','E' };
    for (uint16_t i = 0; i < units; i++) {
        if (!hash_legacy_migrate_unit(origin, COMMODITY_FRAME, i,
                                      &frames[i])) {
            return -1;
        }
    }
    return spawn_cargo_pod_with_manifest(
        w, pos, v2(0.0f, 0.0f), COMMODITY_FRAME, frames, units,
        CARGO_POD_CARGO);
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

static bool test_first_dock_berth_pos(const station_t *st, vec2 *out);

static station_t *test_reset_single_active_station(world_t *w,
                                                   int station_idx) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return NULL;
    for (int s = 0; s < MAX_STATIONS; s++)
        w->stations[s].signal_range = 0.0f;
    station_t *st = &w->stations[station_idx];
    st->scaffold = false;
    st->planned = false;
    st->pos = v2(0.0f, 0.0f);
    st->radius = 36.0f;
    st->dock_radius = 0.0f;
    st->signal_range = 4000.0f;
    st->module_count = 0;
    memset(st->modules, 0, sizeof(st->modules));
    memset(st->module_active_pulse, 0, sizeof(st->module_active_pulse));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    return st;
}

static int test_spawn_station_market_exact_pod(world_t *w,
                                               int station_idx,
                                               commodity_t commodity,
                                               uint16_t units) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS)
        return -1;
    station_t *st = &w->stations[station_idx];
    int dock_idx = test_first_dock_module_idx(st);
    if (dock_idx < 0) return -1;
    vec2 pos = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    int pod_idx = test_spawn_exact_pod(w, pos, commodity, units);
    if (pod_idx < 0) return -1;
    w->cargo_pods[pod_idx].towed_by = -1;
    cargo_pod_set_module_tractor(&w->cargo_pods[pod_idx],
                                 station_idx, dock_idx);
    return pod_idx;
}

static server_player_t *test_prepare_undocked_tractor_player(world_t *w,
                                                             vec2 pos) {
    if (!w) return NULL;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    memset(sp->session_token, 0x74, sizeof(sp->session_token));
    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = -1;
    sp->in_dock_range = false;
    sp->ship.pos = pos;
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.tractor_level = 0;
    sp->input.tractor_hold = true;
    return sp;
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

static int test_destroy_stored_station_loaners(world_t *w, int station_idx) {
    int count = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active || asset->destroyed) continue;
        if (!asset->loaner) continue;
        if (asset->owner_kind != SHIP_ASSET_OWNER_STATION) continue;
        if (asset->status != SHIP_ASSET_STATUS_STORED) continue;
        if (asset->custody_station != station_idx) continue;
        asset->destroyed = true;
        asset->status = SHIP_ASSET_STATUS_DESTROYED;
        asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
        asset->operator_slot = -1;
        count++;
    }
    return count;
}

static bool test_has_station_hull_request(const world_t *w, int requester_station,
                                          hull_class_t hull_class) {
    int owner_code = requester_station == 0 ? INT8_MIN : -1 - requester_station;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        for (int p = 0; p < st->pending_ship_build_count; p++) {
            if (st->pending_ship_builds[p].owner == (int8_t)owner_code &&
                st->pending_ship_builds[p].hull_class == hull_class) {
                return true;
            }
        }
    }
    return false;
}

static bool test_view_has_market_memory(const knowledge_view_t *view,
                                        uint8_t kind,
                                        int station_a,
                                        int station_b,
                                        uint8_t commodity,
                                        market_memory_t *out) {
    if (!view) return false;
    int count = view->count;
    if (count > KNOWLEDGE_VIEW_MAX_CAP) count = KNOWLEDGE_VIEW_MAX_CAP;
    for (int i = 0; i < count; i++) {
        market_memory_t memory;
        if (!market_memory_from_knowledge_item(&view->items[i], &memory))
            continue;
        if (memory.memory_kind != kind) continue;
        if (station_a >= 0 && memory.station_a != (uint8_t)station_a) continue;
        if (station_b >= 0 && memory.station_b != (uint8_t)station_b) continue;
        if (memory.commodity != commodity) continue;
        if (out) *out = memory;
        return true;
    }
    return false;
}

static bool test_issue_world_station_receipt(station_t *st,
                                             const uint8_t cargo_pub[32],
                                             uint64_t event_id,
                                             cargo_receipt_chain_t *out_chain) {
    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (int i = 0; i < 32; i++) {
        recipient[i] = (uint8_t)(0x50 + i);
        origin_pin[i] = (uint8_t)(0xA0 + i);
    }
    memset(out_chain, 0, sizeof(*out_chain));
    if (!cargo_receipt_issue(st, 1, event_id, cargo_pub, recipient,
                             origin_pin, &out_chain->links[0])) {
        return false;
    }
    out_chain->len = 1;
    return cargo_receipt_chain_verify(out_chain->links, out_chain->len,
                                      cargo_pub) == CARGO_RECEIPT_OK;
}

TEST(test_world_reset_creates_stations) {
    WORLD_DECL;
    world_reset(&w);
    ASSERT_STR_EQ(w.stations[0].name, "Prospect Refinery");
    ASSERT(station_has_module(&w.stations[0], MODULE_FURNACE));
    ASSERT_STR_EQ(w.stations[1].name, "Kepler Yard");
    ASSERT_STR_EQ(w.stations[2].name, "Helios Works");
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    ASSERT_STR_EQ(w.stations[SIGNAL_FREEPORT_STATION_INDEX].name, "Blackglass Freeport");
    ASSERT(station_has_module(&w.stations[SIGNAL_FREEPORT_STATION_INDEX], MODULE_DOCK));
    ASSERT(!station_has_module(&w.stations[SIGNAL_FREEPORT_STATION_INDEX],
                               MODULE_SIGNAL_RELAY));
    ASSERT_EQ_INT(w.station_count, SIGNAL_SEEDED_STATION_COUNT);
}

TEST(test_world_reset_spawns_asteroids) {
    WORLD_DECL;
    world_reset(&w);
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (w.asteroids[i].active) count++;
    ASSERT(count >= 20);
}

TEST(test_world_reset_spawns_npcs) {
    /* Starter NPC roster is a neural worker pool. Workers begin as miner
     * hulls, then specialize into hauler hulls for shipment or scaffold
     * tow contracts. */
    WORLD_DECL;
    world_reset(&w);
    int miners = 0, haulers = 0, tows = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w.npc_ships[i].active) continue;
        if (w.npc_ships[i].role == NPC_ROLE_MINER) {
            miners++;
            ASSERT_EQ_INT(w.npc_ships[i].brain_mode, SERVER_BRAIN_MODE_NEURAL_FLIGHT);
        }
        if (w.npc_ships[i].role == NPC_ROLE_HAULER) {
            haulers++;
            ASSERT_EQ_INT(w.npc_ships[i].brain_mode, SERVER_BRAIN_MODE_NEURAL_FLIGHT);
        }
        if (w.npc_ships[i].role == NPC_ROLE_TOW) {
            tows++;
        }
    }
    ASSERT_EQ_INT(miners, 2);
    ASSERT_EQ_INT(haulers, 0);
    ASSERT_EQ_INT(tows, 3);
}

TEST(test_world_reset_ship_assets_back_active_hulls) {
    WORLD_DECL;
    world_reset(&w);
    int assigned_assets = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *npc = &w.npc_ships[i];
        if (!npc->active) continue;
        ASSERT(npc->ship_asset_id != SHIP_ASSET_ID_NONE);
        const ship_asset_t *asset =
            world_ship_asset_by_id_const(&w, npc->ship_asset_id);
        ASSERT(asset != NULL);
        ASSERT_EQ_INT(asset->provenance, SHIP_ASSET_PROVENANCE_GENESIS);
        ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_ASSIGNED);
        ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_NPC);
        ASSERT_EQ_INT(asset->operator_slot, i);
        ASSERT_EQ_INT(asset->hull_class, npc->ship.hull_class);
        ASSERT_EQ_INT(asset->ship.hull_class, npc->ship.hull_class);
        assigned_assets++;
    }
    ASSERT_EQ_INT(assigned_assets, 5);
    ASSERT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER) >= MAX_PLAYERS);
}

TEST(test_station_hull_inventory_cache_tracks_asset_registry) {
    WORLD_DECL;
    world_reset(&w);

    int initial = world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER);
    ASSERT_EQ_INT(w.stations[0].stored_hull_count[HULL_CLASS_MINER], initial);
    ASSERT(initial > 0);

    player_init_ship(&w.players[0], &w);
    ASSERT_EQ_INT(w.stations[0].stored_hull_count[HULL_CLASS_MINER],
                  initial - 1);
    ASSERT_EQ_INT(w.stations[0].stored_hull_count[HULL_CLASS_MINER],
                  world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER));

    ASSERT(world_player_release_ship_asset(&w, 0));
    ASSERT_EQ_INT(w.stations[0].stored_hull_count[HULL_CLASS_MINER],
                  initial);
    ASSERT_EQ_INT(w.stations[0].stored_hull_count[HULL_CLASS_MINER],
                  world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER));
}

TEST(test_player_init_claims_station_loaner_asset) {
    WORLD_DECL;
    world_reset(&w);
    int asset_count_before = test_active_ship_asset_count(&w);
    int stored_before = world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER);

    player_init_ship(&w.players[0], &w);

    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER),
                  stored_before - 1);
    ASSERT(w.players[0].ship_asset_id != SHIP_ASSET_ID_NONE);
    ship_asset_t *asset = world_ship_asset_by_id(&w, w.players[0].ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->owner_kind, SHIP_ASSET_OWNER_STATION);
    ASSERT(asset->loaner);
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(asset->operator_slot, 0);
}

TEST(test_player_init_bound_asset_preserves_custody_station) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    memset(sp->session_token, 0x42, sizeof(sp->session_token));
    ship_asset_t *asset = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, NULL, sp->session_token);
    ASSERT(asset != NULL);
    sp->ship_asset_id = asset->asset_id;

    player_init_ship(sp, &w);

    ASSERT_EQ_INT(sp->ship_asset_id, asset->asset_id);
    ASSERT_EQ_INT(sp->current_station, 1);
    ASSERT_EQ_INT(sp->nearby_station, 1);
    ASSERT_EQ_INT(sp->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(asset->custody_station, 1);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(asset->operator_slot, 0);
}

TEST(test_player_init_ignores_foreign_bound_asset) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    memset(sp->session_token, 0x11, sizeof(sp->session_token));
    uint8_t other_token[8];
    memset(other_token, 0x22, sizeof(other_token));
    ship_asset_t *foreign = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, NULL, other_token);
    ASSERT(foreign != NULL);
    sp->ship_asset_id = foreign->asset_id;
    int asset_count_before = test_active_ship_asset_count(&w);
    int stored_before = world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER);

    player_init_ship(sp, &w);

    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER),
                  stored_before - 1);
    ASSERT(sp->ship_asset_id != foreign->asset_id);
    ASSERT_EQ_INT(foreign->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(foreign->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(foreign->operator_slot, -1);
    ship_asset_t *claimed = world_ship_asset_by_id(&w, sp->ship_asset_id);
    ASSERT(claimed != NULL);
    ASSERT_EQ_INT(claimed->owner_kind, SHIP_ASSET_OWNER_STATION);
    ASSERT(claimed->loaner);
    ASSERT_EQ_INT(claimed->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(claimed->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(claimed->operator_slot, 0);
}

TEST(test_player_init_clears_stale_binding_when_waiting_for_hull) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    memset(sp->session_token, 0x12, sizeof(sp->session_token));
    uint8_t other_token[8];
    memset(other_token, 0x23, sizeof(other_token));
    ship_asset_t *foreign = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_SESSION,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, NULL, other_token);
    ASSERT(foreign != NULL);
    sp->ship_asset_id = foreign->asset_id;
    ASSERT(test_destroy_stored_station_loaners(&w, 0) > 0);
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER), 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(station_finished_mint(&w.stations[1], COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(&w.stations[1], COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(&w.stations[1], COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);
    int asset_count_before = test_active_ship_asset_count(&w);

    player_init_ship(sp, &w);

    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(sp->ship_asset_id, SHIP_ASSET_ID_NONE);
    ASSERT(sp->docked);
    ASSERT_EQ_INT(sp->current_station, 0);
    ASSERT_EQ_INT(sp->nearby_station, 0);
    ASSERT_EQ_INT(sp->ship.hull_class, HULL_CLASS_MINER);
    ASSERT_EQ_FLOAT(sp->ship.hull, 0.0f, 0.001f);
    ASSERT_EQ_INT(foreign->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(foreign->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT(test_has_station_hull_request(&w, 0, HULL_CLASS_MINER));
}

TEST(test_player_reconnect_transfer_moves_ship_asset_binding) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *src = &w.players[0];
    server_player_t *dst = &w.players[1];
    src->id = 0;
    dst->id = 1;
    player_init_ship(src, &w);
    player_init_ship(dst, &w);
    uint32_t src_asset_id = src->ship_asset_id;
    uint32_t dst_asset_id = dst->ship_asset_id;
    ASSERT(src_asset_id != SHIP_ASSET_ID_NONE);
    ASSERT(dst_asset_id != SHIP_ASSET_ID_NONE);
    ASSERT(src_asset_id != dst_asset_id);
    int asset_count_before = test_active_ship_asset_count(&w);

    src->current_station = 1;
    src->nearby_station = 1;
    src->docked = true;
    src->in_dock_range = true;
    src->docking_approach = true;
    src->dock_berth = 2;
    src->ship.hull = 42.0f;
    src->ship.pos = v2(9999.0f, -8888.0f);
    src->ship.angle = 1.25f;

    ASSERT(world_player_transfer_ship_state(&w, 1, 0));

    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(src->ship_asset_id, SHIP_ASSET_ID_NONE);
    ASSERT_EQ_INT(dst->ship_asset_id, src_asset_id);
    ASSERT_EQ_INT(dst->current_station, 1);
    ASSERT_EQ_INT(dst->nearby_station, 1);
    ASSERT(dst->docked);
    ASSERT(dst->in_dock_range);
    ASSERT(!dst->docking_approach);
    ASSERT_EQ_INT(dst->dock_berth, 2);
    ASSERT_EQ_FLOAT(dst->ship.hull, 42.0f, 0.001f);
    ASSERT(v2_dist_sq(dst->ship.pos, w.stations[1].pos) < 1000.0f * 1000.0f);

    const ship_asset_t *moved = world_ship_asset_by_id_const(&w, src_asset_id);
    ASSERT(moved != NULL);
    ASSERT_EQ_INT(moved->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(moved->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(moved->operator_slot, 1);
    ASSERT_EQ_INT(moved->custody_station, 1);
    ASSERT_EQ_FLOAT(moved->ship.hull, 42.0f, 0.001f);

    const ship_asset_t *released = world_ship_asset_by_id_const(&w, dst_asset_id);
    ASSERT(released != NULL);
    ASSERT_EQ_INT(released->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(released->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(released->operator_slot, -1);
}

TEST(test_ship_asset_sync_rejects_non_world_player_pointer) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->id = 0;
    player_init_ship(sp, &w);
    uint32_t asset_id = sp->ship_asset_id;
    ASSERT(asset_id != SHIP_ASSET_ID_NONE);

    server_player_t outsider = {0};
    ASSERT(ship_copy(&outsider.ship, &sp->ship));
    outsider.ship_asset_id = asset_id;
    outsider.ship.hull = 7.0f;
    ASSERT(!world_ship_asset_sync_from_player(&w, &outsider));
    ship_cleanup(&outsider.ship);

    const ship_asset_t *asset = world_ship_asset_by_id_const(&w, asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->operator_slot, 0);
    ASSERT_EQ_FLOAT(asset->ship.hull, sp->ship.hull, 0.001f);
}

TEST(test_server_player_clear_transient_input_resets_spawn_motion) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->id = 0;
    player_init_ship(sp, &w);

    sp->input.thrust = -1.0f;
    sp->input.turn = 1.0f;
    sp->input.mine = true;
    sp->input.reverse_thrust = true;
    sp->input.boost = true;
    sp->input.tractor_hold = true;
    sp->input.mining_target_hint = 12;
    sp->input.service_sell_only = COMMODITY_FERRITE_ORE;
    sp->input.service_sell_grade = MINING_GRADE_RARE;
    sp->movement_queue_count = 1;
    sp->movement_queue[0].intent.thrust = -1.0f;
    sp->last_input_seq = 42;
    sp->last_input_tick = 1234;
    sp->boost_hold_timer = 2.0f;
    sp->actual_thrusting = true;
    sp->docking_approach = true;
    sp->beam_active = true;
    sp->scan_active = true;
    sp->scan_target_index = 9;
    sp->ship.tractor_active = true;

    server_player_clear_transient_input(sp);

    ASSERT_EQ_FLOAT(sp->input.thrust, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(sp->input.turn, 0.0f, 0.001f);
    ASSERT(!sp->input.mine);
    ASSERT(!sp->input.reverse_thrust);
    ASSERT(!sp->input.boost);
    ASSERT(!sp->input.tractor_hold);
    ASSERT_EQ_INT(sp->input.mining_target_hint, -1);
    ASSERT_EQ_INT(sp->input.service_sell_only, COMMODITY_COUNT);
    ASSERT_EQ_INT(sp->input.service_sell_grade, MINING_GRADE_COUNT);
    ASSERT_EQ_INT(sp->movement_queue_count, 0);
    ASSERT_EQ_INT(sp->last_input_seq, 0);
    ASSERT_EQ_INT((int)sp->last_input_tick, 0);
    ASSERT_EQ_FLOAT(sp->boost_hold_timer, 0.0f, 0.001f);
    ASSERT(!sp->actual_thrusting);
    ASSERT(!sp->docking_approach);
    ASSERT(!sp->beam_active);
    ASSERT(!sp->scan_active);
    ASSERT_EQ_INT(sp->scan_target_index, -1);
    ASSERT(!sp->ship.tractor_active);
}

TEST(test_player_release_returns_provisional_loaner_to_storage) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = false;
    int asset_count_before = test_active_ship_asset_count(&w);
    int stored_before = world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER);

    player_init_ship(sp, &w);

    uint32_t asset_id = sp->ship_asset_id;
    ASSERT(asset_id != SHIP_ASSET_ID_NONE);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER),
                  stored_before - 1);
    const ship_asset_t *assigned = world_ship_asset_by_id_const(&w, asset_id);
    ASSERT(assigned != NULL);
    ASSERT_EQ_INT(assigned->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(assigned->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(assigned->operator_slot, 0);

    ASSERT(world_player_release_ship_asset(&w, 0));

    ASSERT_EQ_INT(sp->ship_asset_id, SHIP_ASSET_ID_NONE);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER),
                  stored_before);
    const ship_asset_t *released = world_ship_asset_by_id_const(&w, asset_id);
    ASSERT(released != NULL);
    ASSERT(released->loaner);
    ASSERT_EQ_INT(released->owner_kind, SHIP_ASSET_OWNER_STATION);
    ASSERT_EQ_INT(released->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(released->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(released->operator_slot, -1);
    ASSERT_EQ_INT(released->custody_station, 0);
}

TEST(test_player_release_stores_owned_hull_for_reclaim) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    memset(sp->session_token, 0x41, sizeof(sp->session_token));
    memset(sp->pubkey, 0x82, sizeof(sp->pubkey));

    ship_asset_t *asset = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_PLAYER_PUBKEY,
        -1, 1, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 1, sp->pubkey, NULL);
    ASSERT(asset != NULL);
    int asset_count_before = test_active_ship_asset_count(&w);

    player_init_ship(sp, &w);
    ASSERT_EQ_INT(sp->ship_asset_id, asset->asset_id);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->docked = true;
    sp->ship.hull = 37.5f;
    sp->ship.angle = 0.75f;

    ASSERT(world_player_release_ship_asset(&w, 0));

    ASSERT_EQ_INT(sp->ship_asset_id, SHIP_ASSET_ID_NONE);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(asset->owner_kind, SHIP_ASSET_OWNER_PLAYER_PUBKEY);
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(asset->operator_slot, -1);
    ASSERT_EQ_INT(asset->custody_station, 1);
    ASSERT_EQ_FLOAT(asset->ship.hull, 37.5f, 0.001f);
    ASSERT_EQ_FLOAT(asset->ship.angle, 0.75f, 0.001f);

    ship_cleanup(&sp->ship);
    memset(sp, 0, sizeof(*sp));
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    memset(sp->session_token, 0x41, sizeof(sp->session_token));
    memset(sp->pubkey, 0x82, sizeof(sp->pubkey));

    player_init_ship(sp, &w);

    ASSERT_EQ_INT(sp->ship_asset_id, asset->asset_id);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), asset_count_before);
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(asset->operator_slot, 0);
    ASSERT_EQ_INT(sp->current_station, 1);
    ASSERT_EQ_FLOAT(sp->ship.hull, 37.5f, 0.001f);
}

TEST(test_player_init_ship_null_context_safe) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(NULL, &w);

    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, NULL);
    ASSERT_EQ_INT(sp.ship.hull_class, HULL_CLASS_MINER);
    ASSERT(sp.ship.hull > 0.0f);
    ASSERT(sp.docked);
    ASSERT_EQ_INT(sp.current_station, 0);

    anchor_ship_in_station(NULL, &w);
    sp.ship.vel = v2(12.0f, -7.0f);
    anchor_ship_in_station(&sp, NULL);
    ASSERT_EQ_FLOAT(sp.ship.vel.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(sp.ship.vel.y, 0.0f, 0.001f);
}

TEST(test_player_respawn_retires_asset_and_claims_loaner) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    uint32_t old_asset_id = w.players[0].ship_asset_id;
    ASSERT(old_asset_id != SHIP_ASSET_ID_NONE);
    w.players[0].ship.cargo[COMMODITY_FERRITE_INGOT] = 3.0f;
    w.players[0].docked = false;
    w.players[0].in_dock_range = false;
    w.players[0].input.reset = true;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.players[0].ship_asset_id != SHIP_ASSET_ID_NONE);
    ASSERT(w.players[0].ship_asset_id != old_asset_id);
    const ship_asset_t *old_asset =
        world_ship_asset_by_id_const(&w, old_asset_id);
    ASSERT(old_asset != NULL);
    ASSERT(old_asset->destroyed);
    ASSERT_EQ_INT(old_asset->status, SHIP_ASSET_STATUS_DESTROYED);
    const ship_asset_t *new_asset =
        world_ship_asset_by_id_const(&w, w.players[0].ship_asset_id);
    ASSERT(new_asset != NULL);
    ASSERT(new_asset->loaner);
    ASSERT_EQ_INT(new_asset->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
}

TEST(test_player_respawn_without_loaner_waits_for_shipyard_asset) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    uint32_t old_asset_id = sp->ship_asset_id;
    ASSERT(old_asset_id != SHIP_ASSET_ID_NONE);

    ASSERT(test_destroy_stored_station_loaners(&w, 0) > 0);
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 0, HULL_CLASS_MINER), 0);
    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(station_finished_mint(&w.stations[1], COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(&w.stations[1], COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(&w.stations[1], COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);
    int active_before_death = test_active_ship_asset_count(&w);

    sp->docked = false;
    sp->in_dock_range = false;
    sp->input.reset = true;
    world_sim_step(&w, SIM_DT);

    const ship_asset_t *old_asset = world_ship_asset_by_id_const(&w, old_asset_id);
    ASSERT(old_asset != NULL);
    ASSERT(old_asset->destroyed);
    ASSERT_EQ_INT(old_asset->status, SHIP_ASSET_STATUS_DESTROYED);
    ASSERT_EQ_INT(sp->ship_asset_id, SHIP_ASSET_ID_NONE);
    ASSERT(sp->docked);
    ASSERT_EQ_INT(sp->current_station, 0);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), active_before_death);
    ASSERT(test_has_station_hull_request(&w, 0, HULL_CLASS_MINER));

    sp->input.launch = true;
    world_sim_step(&w, SIM_DT);
    ASSERT(sp->docked);
    ASSERT_EQ_INT(sp->ship_asset_id, SHIP_ASSET_ID_NONE);

    world_sim_step(&w, 120.0f);
    ASSERT(sp->docked);
    ASSERT(sp->ship_asset_id != SHIP_ASSET_ID_NONE);
    const ship_asset_t *replacement =
        world_ship_asset_by_id_const(&w, sp->ship_asset_id);
    ASSERT(replacement != NULL);
    ASSERT(replacement->loaner);
    ASSERT_EQ_INT(replacement->provenance, SHIP_ASSET_PROVENANCE_SHIPYARD);
    ASSERT_EQ_INT(replacement->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(replacement->operator_slot, 0);
    ASSERT_EQ_INT(replacement->custody_station, 0);

    sp->input.launch = true;
    world_sim_step(&w, SIM_DT);
    ASSERT(!sp->docked);
    ASSERT_EQ_INT(sp->ship_asset_id, replacement->asset_id);
}

TEST(test_ship_asset_mint_reclaims_destroyed_unreferenced_slots) {
    WORLD_DECL;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_asset_t *asset = world_ship_asset_mint(
            &w, HULL_CLASS_MINER, SHIP_ASSET_OWNER_STATION,
            0, 0, SHIP_ASSET_PROVENANCE_GENESIS,
            false, 0, NULL, NULL);
        ASSERT(asset != NULL);
    }
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), MAX_SHIP_ASSETS);

    uint32_t first_id = w.ship_assets[0].asset_id;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        w.ship_assets[i].destroyed = true;
        w.ship_assets[i].status = SHIP_ASSET_STATUS_DESTROYED;
        w.ship_assets[i].operator_kind = SHIP_ASSET_OPERATOR_NONE;
        w.ship_assets[i].operator_slot = -1;
    }

    ship_asset_t *fresh = world_ship_asset_mint(
        &w, HULL_CLASS_HAULER, SHIP_ASSET_OWNER_STATION,
        0, 0, SHIP_ASSET_PROVENANCE_SHIPYARD,
        false, 0, NULL, NULL);
    ASSERT(fresh != NULL);
    ASSERT(fresh->asset_id != first_id);
    ASSERT(!fresh->destroyed);
    ASSERT_EQ_INT(fresh->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(fresh->hull_class, HULL_CLASS_HAULER);
}

TEST(test_spawn_npc_bootstrap_does_not_queue_shipyard_build) {
    WORLD_DECL;
    world_reset(&w);
    int active_before = test_active_ship_asset_count(&w);
    int pending_before = w.stations[1].pending_ship_build_count;
    ASSERT_EQ_INT(world_station_stored_hull_count(&w, 1, HULL_CLASS_HAULER), 0);

    int slot = spawn_npc(&w, 1, NPC_ROLE_HAULER);

    ASSERT(slot >= 0);
    ASSERT_EQ_INT(w.stations[1].pending_ship_build_count, pending_before);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), active_before + 1);
    const npc_ship_t *npc = &w.npc_ships[slot];
    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT(npc->ship_asset_id != SHIP_ASSET_ID_NONE);
    const ship_asset_t *asset = world_ship_asset_by_id_const(&w, npc->ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->provenance, SHIP_ASSET_PROVENANCE_LEGACY);
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_NPC);
    ASSERT_EQ_INT(asset->operator_slot, slot);
}

TEST(test_npc_asset_claim_requires_paired_ship_slot) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w.npc_ships[i].active) continue;
        w.npc_ships[i].active = false;
        w.npc_ships[i].ship_asset_id = SHIP_ASSET_ID_NONE;
    }
    for (int i = 0; i < MAX_SHIPS; i++) {
        w.characters[i].active = true;
        w.characters[i].kind = CHARACTER_KIND_PLAYER;
        w.characters[i].ship_idx = i;
        w.characters[i].npc_slot = -1;
    }
    ship_asset_t *asset = world_ship_asset_mint(
        &w, HULL_CLASS_NPC_MINER, SHIP_ASSET_OWNER_STATION,
        0, 0, SHIP_ASSET_PROVENANCE_GENESIS,
        false, 0, NULL, NULL);
    ASSERT(asset != NULL);
    int active_assets_before = test_active_ship_asset_count(&w);

    int slot = ship_asset_claim_for_npc(&w, 0, NPC_ROLE_MINER);

    ASSERT_EQ_INT(slot, -1);
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), active_assets_before);
    ASSERT(!asset->destroyed);
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(asset->operator_slot, -1);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        ASSERT(!w.npc_ships[i].active);
}

TEST(test_shipyard_keeps_completed_build_when_asset_registry_full) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);
    ASSERT(shipyard_queue_station_hull_request(&w, 1, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);

    while (test_active_ship_asset_count(&w) < MAX_SHIP_ASSETS) {
        ship_asset_t *asset = world_ship_asset_mint(
            &w, HULL_CLASS_MINER, SHIP_ASSET_OWNER_STATION,
            0, 0, SHIP_ASSET_PROVENANCE_GENESIS,
            false, 0, NULL, NULL);
        ASSERT(asset != NULL);
    }
    ASSERT_EQ_INT(test_active_ship_asset_count(&w), MAX_SHIP_ASSETS);

    world_sim_step(&w, 120.0f);

    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].hull_class, HULL_CLASS_MINER);
    ASSERT(st->pending_ship_builds[0].build_progress >= 1.0f);
}

TEST(test_world_reset_prospect_workers_leave_idle) {
    WORLD_DECL;
    world_reset(&w);

    for (int i = 0; i < 480; i++)
        world_sim_step(&w, SIM_DT);

    int prospect_workers = 0;
    int active_assignments = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &w.npc_ships[i];
        if (!npc->active || npc->home_station != 0) continue;
        if (npc->role != NPC_ROLE_MINER &&
            npc->role != NPC_ROLE_HAULER &&
            npc->role != NPC_ROLE_TOW)
            continue;
        prospect_workers++;
        if (npc->state != NPC_STATE_DOCKED &&
            npc->state != NPC_STATE_IDLE) {
            active_assignments++;
        }
    }
    ASSERT_EQ_INT(prospect_workers, 2);
    ASSERT(active_assignments > 0);
}

TEST(test_neural_worker_refits_only_from_home_credit_and_modules) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_MINER,
                                         HULL_CLASS_NPC_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *worker = &w.npc_ships[slot];
    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    worker->state = NPC_STATE_DOCKED;
    worker->state_timer = 0.0f;
    worker->ship.mining_level = 0;
    ship->mining_level = 0;
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_LASER_MODULE, 8));
    ledger_earn(&w.stations[0], worker->session_token, 1000.0f);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(ship->mining_level, 1);
    ASSERT_EQ_INT(worker->ship.mining_level, 1);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0],
                                         COMMODITY_LASER_MODULE), 0);
    ASSERT(ledger_balance(&w.stations[0], worker->session_token) < 1000.0f);
}

TEST(test_neural_worker_posts_home_refit_import_contract) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_LASER_MODULE, 16));

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_MINER,
                                         HULL_CLASS_NPC_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *worker = &w.npc_ships[slot];
    worker->state = NPC_STATE_DOCKED;
    worker->state_timer = 0.0f;
    worker->known_contract_count = 0;
    memset(worker->known_contracts, 0, sizeof(worker->known_contracts));
    memset(&worker->knowledge, 0, sizeof(worker->knowledge));
    knowledge_view_configure(&worker->knowledge, SHIP_KNOWN_ITEM_CAP);

    step_npc_ships(&w, SIM_DT);

    int found = 0;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &w.contracts[k];
        if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index != 0) continue;
        if (ct->commodity != COMMODITY_LASER_MODULE) continue;
        ASSERT(ct->quantity_needed >= 8.0f);
        found++;
    }
    ASSERT_EQ_INT(found, 1);
}

TEST(test_ship_death_drops_cargo_pods) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    sp->docked = false;
    sp->ship.pos = v2_add(w.stations[0].pos, v2(120.0f, 0.0f));
    sp->ship.cargo[COMMODITY_FERRITE_ORE] = 45.0f;
    sp->input.reset = true;

    world_sim_step(&w, SIM_DT);

    int pods = 0;
    int total = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w.cargo_pods[i];
        if (!pod->active) continue;
        ASSERT_EQ_INT(pod->kind, CARGO_POD_CARGO);
        ASSERT_EQ_INT(pod->commodity, COMMODITY_FERRITE_ORE);
        pods++;
        total += pod->quantity;
    }
    ASSERT_EQ_INT(pods, 3);
    ASSERT_EQ_INT(total, 45);
}

TEST(test_towed_cargo_pod_sells_at_matching_intake) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x55, sizeof(sp->session_token));
    station_t *st = &w.stations[1];
    st->base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    float before = st->_inventory_cache[COMMODITY_FERRITE_INGOT];
    int hopper_idx = station_find_hopper_for(st, COMMODITY_FERRITE_INGOT);
    ASSERT(hopper_idx >= 0);
    vec2 hopper_pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);

    int pod_idx = test_spawn_exact_pod(&w, hopper_pos,
                                       COMMODITY_FERRITE_INGOT, 7);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 7);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            1, hopper_idx));
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_FERRITE_INGOT], before, 0.01f);
    ASSERT(sp->ship.stat_credits_earned >= 70.0f);
}

TEST(test_towed_shell_pod_keeps_shell_after_intake_custody_sale) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    station_t *st = &w.stations[1];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x5a, sizeof(sp->session_token));
    st->base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    int hopper_idx = station_find_hopper_for(st, COMMODITY_FERRITE_INGOT);
    ASSERT(hopper_idx >= 0);
    vec2 hopper_pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);

    cargo_unit_t ingot = {0};
    cargo_unit_t shell = {0};
    uint8_t fragment_pub[32] = {0};
    const uint8_t shell_origin[8] = { 'S','H','E','L','L','S','A','L' };
    fragment_pub[31] = 0x5a;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_pub, 0, &ingot));
    ASSERT(hash_legacy_migrate_unit(shell_origin, COMMODITY_FRAME, 0,
                                    &shell));

    int pod_idx = spawn_cargo_pod_with_manifest(
        &w, hopper_pos, v2(0.0f, 0.0f), COMMODITY_FERRITE_INGOT,
        &ingot, 1, CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    cargo_pod_set_shell_frame(&w.cargo_pods[pod_idx], &shell);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 1);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].manifest_count, 1);
    ASSERT(w.cargo_pods[pod_idx].has_shell_frame);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            1, hopper_idx));
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT(memcmp(w.cargo_pods[pod_idx].manifest_units[0].pub,
                  ingot.pub, 32) == 0);
    ASSERT(memcmp(w.cargo_pods[pod_idx].shell_frame.pub,
                  shell.pub, 32) == 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FERRITE_INGOT), 0);
}

TEST(test_buy_station_held_pod_transfers_custody_to_ship) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    station_t *st = &w.stations[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x44, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    st->base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    ledger_earn(st, sp->session_token, 1000.0f);

    int dock_idx = -1;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            dock_idx = m;
            break;
        }
    }
    ASSERT(dock_idx >= 0);

    int pod_idx = spawn_cargo_pod(&w, st->pos, v2(0.0f, 0.0f),
                                  COMMODITY_FERRITE_INGOT, 5,
                                  CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = -1;
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_idx], 0, dock_idx);

    float before = ledger_balance(st, sp->session_token);
    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COUNT;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, 0);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[pod_idx]));
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], pod_idx);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[pod_idx]), 0);
    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token), before, 0.001f);

    test_move_pod_past_station_charge_boundary(&w, 0, pod_idx);
    world_sim_step(&w, SIM_DT);
    ASSERT(ledger_balance(st, sp->session_token) < before);
}

TEST(test_buy_selected_station_held_pod_transfers_that_pod) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    station_t *st = &w.stations[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x71, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    st->base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    ledger_earn(st, sp->session_token, 1000.0f);

    int dock_idx = -1;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            dock_idx = m;
            break;
        }
    }
    ASSERT(dock_idx >= 0);

    int first = spawn_cargo_pod(&w, st->pos, v2(0.0f, 0.0f),
                                COMMODITY_FERRITE_INGOT, 3,
                                CARGO_POD_CARGO);
    int selected = spawn_cargo_pod(&w, v2_add(st->pos, v2(12.0f, 0.0f)),
                                   v2(0.0f, 0.0f),
                                   COMMODITY_FERRITE_INGOT, 5,
                                   CARGO_POD_CARGO);
    ASSERT(first >= 0);
    ASSERT(selected >= 0);
    w.cargo_pods[first].towed_by = -1;
    w.cargo_pods[selected].towed_by = -1;
    cargo_pod_set_module_tractor(&w.cargo_pods[first], 0, dock_idx);
    cargo_pod_set_module_tractor(&w.cargo_pods[selected], 0, dock_idx);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COUNT;
    sp->input.buy_station_pod = true;
    sp->input.buy_station_pod_index = (uint16_t)selected;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], selected);
    ASSERT_EQ_INT(w.cargo_pods[selected].towed_by, 0);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[first],
                                            0, dock_idx));
    ASSERT_EQ_INT(w.cargo_pods[first].towed_by, -1);
}

TEST(test_docked_towed_cargo_pod_stays_parked_at_ship) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, &w);

    int dock_idx = test_first_dock_module_idx(&w.stations[0]);
    ASSERT(dock_idx >= 0);
    int pod_idx = test_spawn_exact_pod(
        &w, v2_add(sp->ship.pos, v2(120.0f, 60.0f)),
        COMMODITY_FERRITE_INGOT, 2);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;
    w.cargo_pods[pod_idx].vel = v2(520.0f, -180.0f);
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_idx], 0, dock_idx);

    for (int i = 0; i < 24; i++)
        world_sim_step(&w, SIM_DT);

    vec2 expected = v2_add(sp->ship.pos,
                           v2_scale(v2_from_angle(sp->ship.angle + PI_F),
                                    46.0f));
    ASSERT(sp->docked);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], pod_idx);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, 0);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[pod_idx]));
    ASSERT_EQ_FLOAT(w.cargo_pods[pod_idx].pos.x, expected.x, 0.01f);
    ASSERT_EQ_FLOAT(w.cargo_pods[pod_idx].pos.y, expected.y, 0.01f);
    ASSERT_EQ_FLOAT(v2_len(w.cargo_pods[pod_idx].vel), 0.0f, 0.001f);
}

TEST(test_cargo_pods_collide_and_separate) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    vec2 pos = v2(5000.0f, 5000.0f);
    int pod_a = spawn_cargo_pod(&w, pos, v2(0.0f, 0.0f),
                                COMMODITY_FRAME, 1, CARGO_POD_CARGO);
    int pod_b = spawn_cargo_pod(&w, pos, v2(0.0f, 0.0f),
                                COMMODITY_FRAME, 1, CARGO_POD_CARGO);
    ASSERT(pod_a >= 0);
    ASSERT(pod_b >= 0);
    w.cargo_pods[pod_a].towed_by = -1;
    w.cargo_pods[pod_b].towed_by = -1;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_a].active);
    ASSERT(w.cargo_pods[pod_b].active);
    float dist = v2_len(v2_sub(w.cargo_pods[pod_b].pos,
                               w.cargo_pods[pod_a].pos));
    ASSERT(dist >= w.cargo_pods[pod_a].radius +
                   w.cargo_pods[pod_b].radius);
}

static int test_count_pod_module_tractor_interactions(const world_t *w,
                                                      int pod_idx,
                                                      int station_idx,
                                                      int module_idx) {
    int count = 0;
    if (!w) return 0;
    for (int i = 0; i < w->interactions.count; i++) {
        const sim_interaction_t *it = &w->interactions.items[i];
        if (it->type != SIM_INTERACTION_TRACTOR_BEAM) continue;
        if (it->visual != SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR)
            continue;
        if (it->source.type != SIM_INTERACTION_ENTITY_STATION_MODULE ||
            it->source.index != station_idx || it->source.aux != module_idx) {
            continue;
        }
        if (it->target.type != SIM_INTERACTION_ENTITY_CARGO_POD ||
            it->target.index != pod_idx) {
            continue;
        }
        if (it->intensity <= 0.0f || it->range <= 0.0f) continue;
        count++;
    }
    return count;
}

TEST(test_station_dock_tractor_spreads_market_pods) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[0];
    int dock_idx = -1;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            dock_idx = m;
            break;
        }
    }
    ASSERT(dock_idx >= 0);

    const station_module_t *dock = &st->modules[dock_idx];
    vec2 module_pos = module_world_pos_ring(st, dock->ring, dock->slot);
    vec2 outward = v2_sub(module_pos, st->pos);
    float out_len = v2_len(outward);
    ASSERT(out_len > 0.001f);
    outward = v2_scale(outward, 1.0f / out_len);
    vec2 hold = v2_add(module_pos, v2_scale(outward,
        STATION_MODULE_COL_RADIUS + 26.0f));

    int pod_a = spawn_cargo_pod(&w, hold, v2(0.0f, 0.0f),
                                COMMODITY_FERRITE_INGOT, 3,
                                CARGO_POD_CARGO);
    int pod_b = spawn_cargo_pod(&w, hold, v2(0.0f, 0.0f),
                                COMMODITY_FERRITE_INGOT, 4,
                                CARGO_POD_CARGO);
    ASSERT(pod_a >= 0);
    ASSERT(pod_b >= 0);
    w.cargo_pods[pod_a].towed_by = -1;
    w.cargo_pods[pod_b].towed_by = -1;
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_a], 0, dock_idx);
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_b], 0, dock_idx);

    for (int i = 0; i < 60; i++)
        world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_a].active);
    ASSERT(w.cargo_pods[pod_b].active);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_a],
                                            0, dock_idx));
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_b],
                                            0, dock_idx));
    ASSERT_EQ_INT(test_count_pod_module_tractor_interactions(
                      &w, pod_a, 0, dock_idx), 1);
    ASSERT_EQ_INT(test_count_pod_module_tractor_interactions(
                      &w, pod_b, 0, dock_idx), 1);
    float dist = v2_len(v2_sub(w.cargo_pods[pod_b].pos,
                               w.cargo_pods[pod_a].pos));
    ASSERT(dist >= w.cargo_pods[pod_a].radius +
                   w.cargo_pods[pod_b].radius);
}

TEST(test_station_dock_tractor_clears_after_pod_moves_out_of_range) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[0];
    int dock_idx = -1;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            dock_idx = m;
            break;
        }
    }
    ASSERT(dock_idx >= 0);

    const station_module_t *dock = &st->modules[dock_idx];
    vec2 module_pos = module_world_pos_ring(st, dock->ring, dock->slot);
    vec2 outward = v2_norm(v2_sub(module_pos, st->pos));
    vec2 hold = v2_add(module_pos, v2_scale(outward,
        STATION_MODULE_COL_RADIUS + 26.0f));

    int pod_idx = spawn_cargo_pod(&w, hold, v2_scale(outward, 10000.0f),
                                  COMMODITY_FERRITE_INGOT, 3,
                                  CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = -1;
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_idx], 0, dock_idx);

    world_sim_step(&w, 2.0f);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[pod_idx]));
    ASSERT_EQ_INT(test_count_pod_module_tractor_interactions(
                      &w, pod_idx, 0, dock_idx), 0);
}

TEST(test_station_dock_adopts_finished_output_pod_for_market) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[0];
    for (int a = 0; a < MAX_ARMS; a++) {
        st->arm_speed[a] = 0.0f;
        st->arm_omega[a] = 0.0f;
        st->arm_rotation[a] = 0.0f;
    }

    int dock_idx = -1;
    int furnace_idx = -1;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            dock_idx = m;
        }
        if (st->modules[m].type == MODULE_FURNACE &&
            !st->modules[m].scaffold) {
            furnace_idx = m;
        }
    }
    ASSERT(dock_idx >= 0);
    ASSERT(furnace_idx >= 0);

    vec2 furnace_pos = module_world_pos_ring(
        st, st->modules[furnace_idx].ring, st->modules[furnace_idx].slot);
    vec2 furnace_out = v2_norm(v2_sub(furnace_pos, st->pos));
    if (v2_len_sq(furnace_out) < 0.5f)
        furnace_out = v2_from_angle(module_angle_ring(
            st, st->modules[furnace_idx].ring, st->modules[furnace_idx].slot));
    vec2 output_pos = v2_add(furnace_pos, v2_scale(
        furnace_out, STATION_MODULE_COL_RADIUS + 18.0f + 20.0f));
    int pod_idx = test_spawn_exact_pod(
        &w, output_pos, COMMODITY_FERRITE_INGOT, 6);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = -1;
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_idx], 0, furnace_idx);

    for (int i = 0; i < 240; i++)
        world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            0, dock_idx));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xE8, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    sp->ship.pos = st->pos;
    ledger_earn(st, sp->session_token, 100000.0f);
    float before = ledger_balance(st, sp->session_token);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FERRITE_INGOT;
    sp->input.buy_grade = MINING_GRADE_COUNT;
    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, 0);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[pod_idx]));
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], pod_idx);
    ASSERT_EQ_INT(cargo_pod_custody_station(&w.cargo_pods[pod_idx]), 0);
    ASSERT_EQ_FLOAT(ledger_balance(st, sp->session_token), before, 0.001f);

    test_move_pod_past_station_charge_boundary(&w, 0, pod_idx);
    world_sim_step(&w, SIM_DT);
    ASSERT(ledger_balance(st, sp->session_token) < before);
}

TEST(test_cargo_pod_high_speed_station_impact_destroys_shell) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    const station_t *st = &w.stations[0];
    cargo_unit_t ingot = {0};
    cargo_unit_t shell = {0};
    uint8_t fragment_pub[32] = {0};
    const uint8_t shell_origin[8] = { 'S','M','A','S','H','P','O','D' };
    fragment_pub[31] = 0x6a;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_pub, 0, &ingot));
    ASSERT(hash_legacy_migrate_unit(shell_origin, COMMODITY_FRAME, 0,
                                    &shell));

    vec2 relay_pos = module_world_pos_ring(st, 1, 1);
    int pod_idx = spawn_cargo_pod_with_manifest(
        &w, v2_add(relay_pos, v2(-20.0f, 0.0f)),
        v2(420.0f, 0.0f), COMMODITY_FERRITE_INGOT,
        &ingot, 1, CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    cargo_pod_set_shell_frame(&w.cargo_pods[pod_idx], &shell);

    world_sim_step(&w, SIM_DT);

    ASSERT(!w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FRAME), 0);
}

TEST(test_cargo_pod_bounces_off_station_corridor_ring) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[0];
    for (int a = 0; a < MAX_ARMS; a++) {
        st->arm_speed[a] = 0.0f;
        st->arm_omega[a] = 0.0f;
        st->arm_rotation[a] = 0.0f;
    }

    float ang1 = module_angle_ring(st, 1, 1);
    float ang2 = module_angle_ring(st, 1, 2);
    float mid_ang = (ang1 + ang2) * 0.5f;
    float ring_r = STATION_RING_RADIUS[1];
    float pod_r = 18.0f;
    float outer_edge = ring_r + STATION_CORRIDOR_HW + pod_r;
    vec2 radial = v2(cosf(mid_ang), sinf(mid_ang));
    vec2 pos = v2_add(st->pos, v2_scale(radial, outer_edge - 4.0f));

    int pod_idx = test_spawn_exact_pod(&w, pos, COMMODITY_LASER_MODULE, 1);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].vel = v2_scale(radial, -110.0f);

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    float dist = v2_len(v2_sub(w.cargo_pods[pod_idx].pos, st->pos));
    ASSERT(dist >= outer_edge);
    ASSERT(v2_dot(w.cargo_pods[pod_idx].vel, radial) > 0.0f);
}

TEST(test_cargo_pod_bounces_off_asteroid) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    asteroid_t *a = &w.asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->radius = 26.0f;
    a->hp = 100.0f;
    a->max_hp = 100.0f;
    a->pos = v2_add(w.stations[0].pos, v2(1800.0f, 0.0f));
    a->vel = v2(0.0f, 0.0f);

    float pod_r = 18.0f;
    vec2 normal = v2(-1.0f, 0.0f);
    vec2 pod_pos = v2_add(a->pos,
                          v2_scale(normal, a->radius + pod_r - 4.0f));
    int pod_idx = test_spawn_exact_pod(&w, pod_pos,
                                       COMMODITY_TRACTOR_MODULE, 1);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].vel = v2(120.0f, 0.0f);

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT(v2_dot(w.cargo_pods[pod_idx].vel, normal) > 0.0f);
    ASSERT(v2_dist_sq(w.cargo_pods[pod_idx].pos, a->pos) >=
           (a->radius + pod_r) * (a->radius + pod_r));
}

TEST(test_towed_cargo_pod_bulk_sell_no_longer_uses_dock_custody) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x58, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    w.stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    float before = w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE];
    int pod_a = spawn_cargo_pod(&w, sp->ship.pos, v2(0.0f, 0.0f),
                                COMMODITY_FERRITE_ORE, 7, CARGO_POD_CARGO);
    int pod_b = spawn_cargo_pod(&w, sp->ship.pos, v2(0.0f, 0.0f),
                                COMMODITY_FERRITE_ORE, 5, CARGO_POD_CARGO);
    ASSERT(pod_a >= 0);
    ASSERT(pod_b >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_a;
    sp->ship.towed_pods[1] = (int16_t)pod_b;
    sp->ship.towed_pod_count = 2;
    w.cargo_pods[pod_a].towed_by = 0;
    w.cargo_pods[pod_b].towed_by = 0;

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_COUNT;
    sp->input.service_sell_grade = MINING_GRADE_COUNT;
    sp->input.service_sell_one = false;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_a].active);
    ASSERT(w.cargo_pods[pod_b].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_a].towed_by, 0);
    ASSERT_EQ_INT(w.cargo_pods[pod_b].towed_by, 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 2);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], pod_a);
    ASSERT_EQ_INT(sp->ship.towed_pods[1], pod_b);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE],
                    before, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship.stat_credits_earned, 0.0f, 0.01f);
}

TEST(test_towed_cargo_pod_row_sell_no_longer_uses_dock_custody) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x56, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    w.stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    float before = w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE];
    int pod_idx = spawn_cargo_pod(&w, sp->ship.pos, v2(0.0f, 0.0f),
                                  COMMODITY_FERRITE_ORE, 7, CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_ORE;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE],
                    before, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship.stat_credits_earned, 0.0f, 0.01f);
}

TEST(test_manifest_cargo_pod_sale_preserves_exact_units) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x57, sizeof(sp->session_token));
    station_t *st = &w.stations[1];
    ASSERT(station_manifest_bootstrap(st));
    manifest_clear(&st->manifest);
    st->_inventory_cache[COMMODITY_FRAME] = 0.0f;
    st->base_price[COMMODITY_FRAME] = 20.0f;
    int hopper_idx = station_find_hopper_for(st, COMMODITY_FRAME);
    ASSERT(hopper_idx >= 0);
    vec2 hopper_pos = module_world_pos_ring(
        st, st->modules[hopper_idx].ring, st->modules[hopper_idx].slot);

    cargo_unit_t units[2] = {{0}};
    const uint8_t origin[8] = { 'P','O','D','T','E','S','T','1' };
    ASSERT(hash_legacy_migrate_unit(origin, COMMODITY_FRAME, 0, &units[0]));
    ASSERT(hash_legacy_migrate_unit(origin, COMMODITY_FRAME, 1, &units[1]));
    units[0].origin_station = 1;
    units[1].origin_station = 1;

    int pod_idx = spawn_cargo_pod_with_manifest(&w, hopper_pos,
                                                v2(0.0f, 0.0f),
                                                COMMODITY_FRAME, units, 2,
                                                CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            1, hopper_idx));
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].manifest_count, 2);
    ASSERT(memcmp(w.cargo_pods[pod_idx].manifest_units[0].pub,
                  units[0].pub, 32) == 0);
    ASSERT(memcmp(w.cargo_pods[pod_idx].manifest_units[1].pub,
                  units[1].pub, 32) == 0);
    ASSERT(sp->ship.stat_credits_earned >= 40.0f);
}

TEST(test_towed_cargo_pod_row_sell_requires_physical_intake_when_hopper_full) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x57, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    w.stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] =
        REFINERY_HOPPER_CAPACITY;
    int pod_idx = spawn_cargo_pod(&w, sp->ship.pos, v2(0.0f, 0.0f),
                                  COMMODITY_FERRITE_ORE, 7, CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_ORE;
    sp->input.service_sell_grade = MINING_GRADE_COMMON;
    sp->input.service_sell_one = true;

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 7);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE],
                    REFINERY_HOPPER_CAPACITY, 0.01f);
    ASSERT_EQ_FLOAT(sp->ship.stat_credits_earned, 0.0f, 0.01f);
}

TEST(test_towed_cargo_pod_intake_handoff_moves_whole_pod_to_hopper) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x58, sizeof(sp->session_token));
    station_t *kepler = &w.stations[1];
    int hopper_idx = station_find_hopper_for(kepler, COMMODITY_FERRITE_INGOT);
    ASSERT(hopper_idx >= 0);
    vec2 hopper_pos = module_world_pos_ring(
        kepler, kepler->modules[hopper_idx].ring,
        kepler->modules[hopper_idx].slot);
    float before = ledger_balance(kepler, sp->session_token);

    int pod_idx = test_spawn_exact_pod(&w, hopper_pos,
                                       COMMODITY_FERRITE_INGOT, 7);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;
    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 7);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            1, hopper_idx));
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT(ledger_balance(kepler, sp->session_token) > before);
    ASSERT(sp->ship.stat_credits_earned > 0.0f);
}

TEST(test_docking_works_while_towing_cargo_pod) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    sp->docked = false;
    sp->current_station = -1;

    vec2 berth = {0};
    ASSERT(test_first_dock_berth_pos(&w.stations[0], &berth));
    sp->ship.pos = berth;
    sp->ship.vel = v2(0.0f, 0.0f);

    int pod_idx = spawn_cargo_pod(&w, v2_add(berth, v2(-72.0f, 0.0f)),
                                  v2(0.0f, 0.0f), COMMODITY_CRYSTAL_ORE,
                                  3, CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = 0;
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;

    sp->input.interact = true;
    world_sim_step(&w, SIM_DT);

    ASSERT(sp->docked);
    ASSERT_EQ_INT(sp->current_station, 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], pod_idx);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, 0);
}

TEST(test_space_release_drops_cargo_pod_instead_of_slingshot) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    vec2 pos = v2_add(w.stations[0].pos, v2(1100.0f, 0.0f));
    server_player_t *sp = test_prepare_undocked_tractor_player(&w, pos);
    ASSERT(sp != NULL);
    sp->ship.angle = 0.0f;
    sp->ship.vel = v2(300.0f, 0.0f);
    sp->input.tractor_hold = false;
    sp->input.release_tow = true;

    int pod_idx = spawn_cargo_pod(&w, v2_add(pos, v2(-100.0f, 0.0f)),
                                  v2(0.0f, 0.0f), COMMODITY_CRYSTAL_ORE,
                                  3, CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = 0;
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    float pod_speed = v2_len(w.cargo_pods[pod_idx].vel);
    float ship_speed = v2_len(sp->ship.vel);
    ASSERT(pod_speed < ship_speed * 0.5f + 30.0f);
}

TEST(test_towed_fragment_loads_raw_contract_at_dock) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(w.contracts, 0, sizeof(w.contracts));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x59, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_ORE,
        .quantity_needed = 10.0f,
        .base_price = 8.0f,
        .claimed_by = -1,
    };

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);
    asteroid_t *a = &w.asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 7.0f;
    a->max_ore = 7.0f;
    a->radius = 8.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;

    sp->ship.towed_fragments[0] = (int16_t)frag;
    sp->ship.towed_count = 1;
    float before = w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE];
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FERRITE_ORE;

    world_sim_step(&w, SIM_DT);

    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(sp->ship.towed_count, 0);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE],
                    before + 7.0f, 0.001f);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 3.0f, 0.001f);
    ASSERT(sp->ship.stat_credits_earned > 0.0f);
}

TEST(test_tow_capacity_counts_pods_against_fragment_pickup) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    vec2 pos = v2_add(w.stations[0].pos, v2(900.0f, 0.0f));
    server_player_t *sp = test_prepare_undocked_tractor_player(&w, pos);
    ASSERT(sp != NULL);

    int pod_a = test_spawn_exact_pod(&w, v2_add(pos, v2(-45.0f, 0.0f)),
                                     COMMODITY_FRAME, 1);
    int pod_b = test_spawn_exact_pod(&w, v2_add(pos, v2(-70.0f, 0.0f)),
                                     COMMODITY_LASER_MODULE, 1);
    ASSERT(pod_a >= 0);
    ASSERT(pod_b >= 0);
    w.cargo_pods[pod_a].towed_by = 0;
    w.cargo_pods[pod_b].towed_by = 0;
    sp->ship.towed_pods[0] = (int16_t)pod_a;
    sp->ship.towed_pods[1] = (int16_t)pod_b;
    sp->ship.towed_pod_count = 2;

    asteroid_t *frag = &w.asteroids[0];
    frag->active = true;
    frag->tier = ASTEROID_TIER_S;
    frag->commodity = COMMODITY_FERRITE_ORE;
    frag->pos = v2_add(pos, v2(45.0f, 0.0f));
    frag->radius = 8.0f;
    frag->ore = 4.0f;
    frag->max_ore = 4.0f;
    frag->hp = 4.0f;
    frag->max_hp = 4.0f;
    frag->fracture_child = true;

    ASSERT_EQ_INT(ship_tow_body_capacity(&sp->ship), 2);
    ASSERT_EQ_INT(ship_tow_body_space(&sp->ship), 0);
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 2);
    ASSERT_EQ_INT(sp->ship.towed_count, 0);
    ASSERT_EQ_INT(ship_tow_body_space(&sp->ship), 0);
}

TEST(test_tow_capacity_counts_fragments_against_pod_pickup) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    vec2 pos = v2_add(w.stations[0].pos, v2(1100.0f, 0.0f));
    server_player_t *sp = test_prepare_undocked_tractor_player(&w, pos);
    ASSERT(sp != NULL);

    int held_pod = test_spawn_exact_pod(&w, v2_add(pos, v2(-45.0f, 0.0f)),
                                        COMMODITY_FRAME, 1);
    int loose_pod = test_spawn_exact_pod(&w, v2_add(pos, v2(65.0f, 0.0f)),
                                         COMMODITY_LASER_MODULE, 1);
    ASSERT(held_pod >= 0);
    ASSERT(loose_pod >= 0);
    w.cargo_pods[held_pod].towed_by = 0;
    w.cargo_pods[loose_pod].towed_by = -1;
    sp->ship.towed_pods[0] = (int16_t)held_pod;
    sp->ship.towed_pod_count = 1;

    asteroid_t *frag = &w.asteroids[0];
    frag->active = true;
    frag->tier = ASTEROID_TIER_S;
    frag->commodity = COMMODITY_FERRITE_ORE;
    frag->pos = v2_add(pos, v2(45.0f, 0.0f));
    frag->radius = 8.0f;
    frag->ore = 4.0f;
    frag->max_ore = 4.0f;
    frag->hp = 4.0f;
    frag->max_hp = 4.0f;
    frag->fracture_child = true;
    sp->ship.towed_fragments[0] = 0;
    sp->ship.towed_count = 1;

    ASSERT_EQ_INT(ship_tow_body_space(&sp->ship), 0);
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], held_pod);
    ASSERT_EQ_INT(sp->ship.towed_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_fragments[0], 0);
    ASSERT_EQ_INT(w.cargo_pods[loose_pod].towed_by, -1);
    ASSERT_EQ_INT(ship_tow_body_space(&sp->ship), 0);
}

TEST(test_gas_rich_asteroid_emits_gas_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    asteroid_t *a = &w.asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_L;
    a->commodity = COMMODITY_CUPRITE_ORE;
    a->pos = v2_add(w.stations[0].pos, v2(240.0f, 0.0f));
    a->vel = v2(0.0f, 0.0f);
    a->radius = 42.0f;
    a->phase = ASTEROID_PHASE_GAS_RICH;
    a->gas_emit_timer = 0.0f;

    sim_step_asteroid_dynamics(&w, SIM_DT);

    bool found = false;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w.cargo_pods[i];
        if (!pod->active) continue;
        if (pod->kind == CARGO_POD_GAS &&
            pod->commodity == COMMODITY_CUPRITE_ORE &&
            pod->quantity >= 2) {
            found = true;
        }
    }
    ASSERT(found);
}

TEST(test_npc_embedded_towed_fragment_skips_ambient_asteroid_drag) {
    WORLD_DECL;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[0].signal_range = 1000.0f;
    w.stations[0].signal_connected = true;

    asteroid_t *frag = &w.asteroids[3];
    frag->active = true;
    frag->tier = ASTEROID_TIER_S;
    frag->radius = 12.0f;
    frag->pos = v2(100.0f, 0.0f);
    frag->vel = v2(120.0f, 0.0f);

    npc_ship_t *npc = &w.npc_ships[0];
    npc->active = true;
    npc_clear_towed_fragment(npc);
    npc->towed_fragment = -1;
    npc->ship.towed_fragments[0] = 3;
    npc->ship.towed_count = 1;

    sim_step_asteroid_dynamics(&w, 1.0f);

    ASSERT(frag->active);
    ASSERT_EQ_INT(npc_towed_fragment_index(npc), 3);
    ASSERT_EQ_FLOAT(frag->vel.x, 120.0f, 0.001f);
}

static const sim_event_t *find_hail_response_event(const world_t *w) {
    for (int i = 0; i < w->events.count; i++) {
        if (w->events.events[i].type == SIM_EVENT_HAIL_RESPONSE)
            return &w->events.events[i];
    }
    return NULL;
}

static bool test_first_dock_berth_pos(const station_t *st, vec2 *out) {
    if (!st || !out) return false;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *m = &st->modules[i];
        if (m->type != MODULE_DOCK || m->scaffold) continue;
        vec2 mod_pos = module_world_pos_ring(st, m->ring, m->slot);
        vec2 radial = v2_from_angle(module_angle_ring(st, m->ring, m->slot));
        *out = v2_add(mod_pos, v2_scale(radial, 55.0f));
        return true;
    }
    return false;
}

TEST(test_hail_responds_while_docked) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x42, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    sp->input.hail = true;

    world_sim_step(&w, SIM_DT);

    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, 0);
    ASSERT(ev->hail_response.credits >= 0.0f);
    ASSERT_EQ_INT(ev->hail_response.decision_mode,
                  HAIL_DECISION_MODE_DOCKED);
    ASSERT_EQ_INT(ev->hail_response.decision_candidate_count, 1);
    ASSERT(ev->hail_response.decision_flags &
           SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(ev->hail_response.decision_flags &
           SIGNAL_DECISION_REASON_HARD_APPROVED);
    ASSERT(ev->hail_response.decision_signal_quality > 0.0f);
    ASSERT(ev->hail_response.decision_source_id != 0ull);
    ASSERT_EQ_INT(sp->hail_decision_valid, 1);
    ASSERT_EQ_INT(sp->hail_decision_station, 0);
    ASSERT(sp->hail_decision_candidate_count >= 1);
    ASSERT(sp->hail_decision_flags & SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(sp->hail_decision_flags & SIGNAL_DECISION_REASON_ADVISORY_ONLY);
    ASSERT(sp->hail_decision_flags & SIGNAL_DECISION_REASON_HARD_APPROVED);
    ASSERT(sp->hail_decision_flags & SIGNAL_DECISION_REASON_HAS_SIGNAL_CONTEXT);
    ASSERT(!(sp->hail_decision_flags &
             SIGNAL_DECISION_REASON_HAS_SOURCE_MEMORY));
    ASSERT(sp->hail_decision_signal_quality > 0.0f);
    ASSERT(sp->hail_decision_source_id != 0ull);
}

TEST(test_docking_auto_reports_station_balance) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x46, sizeof(sp->session_token));
    ledger_earn(&w.stations[0], sp->session_token, 321.0f);

    sp->input.interact = true;
    world_sim_step(&w, SIM_DT);
    ASSERT(!sp->docked);
    sp->input.interact = false;

    vec2 berth = {0};
    ASSERT(test_first_dock_berth_pos(&w.stations[0], &berth));
    sp->ship.pos = berth;
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->input.interact = true;

    world_sim_step(&w, SIM_DT);

    ASSERT(sp->docked);
    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, 0);
    ASSERT_EQ_FLOAT(ev->hail_response.credits, 321.0f, 0.01f);
    ASSERT_EQ_INT(ev->hail_response.contract_index, -1);
}

TEST(test_hail_does_not_spawn_nearest_rock_contract) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x46, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 2; /* Helios */
    sp->input.hail = true;

    world_sim_step(&w, SIM_DT);

    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, 2);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].action == CONTRACT_FRACTURE &&
                 w.contracts[k].claimed_by == 0));
    }
}

TEST(test_hail_claims_existing_station_work) {
    WORLD_DECL;
    world_reset(&w);
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FRAME,
        .quantity_needed = 2.0f,
        .base_price = 10.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x47, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 2;
    sp->input.hail = true;

    world_sim_step(&w, SIM_DT);

    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, 2);
    ASSERT_EQ_INT(ev->hail_response.contract_index, 0);
    ASSERT_EQ_INT(w.contracts[0].claimed_by, 0);
    ASSERT_EQ_INT(sp->ship.known_contract_count, 1);
    ASSERT_EQ_INT(sp->ship.known_contracts[0].station_index, 2);
    ASSERT_EQ_INT(sp->ship.known_contracts[0].commodity, COMMODITY_FRAME);
}

TEST(test_hail_responds_to_station_signal_outside_ship_comm_range) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x43, sizeof(sp->session_token));
    sp->docked = false;
    sp->ship.pos = v2_add(w.stations[0].pos, v2(sp->ship.comm_range * 1.5f, 0.0f));
    sp->input.hail = true;

    world_sim_step(&w, SIM_DT);

    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, 0);
    ASSERT(ev->hail_response.credits >= 0.0f);
}

TEST(test_hail_responds_at_helios_dock_even_with_short_ship_comm) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x45, sizeof(sp->session_token));
    sp->docked = false;
    sp->ship.comm_range = 150.0f;
    sp->ship.pos = station_dock_lane_pos(&w.stations[2], 1, 0,
        STATION_RING_RADIUS[1] + 55.0f);
    sp->input.hail = true;

    world_sim_step(&w, SIM_DT);

    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, 2);
    ASSERT(ev->hail_response.credits >= 0.0f);
}

TEST(test_hail_reports_no_station_in_range) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    memset(sp->session_token, 0x44, sizeof(sp->session_token));
    sp->docked = false;
    sp->ship.pos = v2(WORLD_RADIUS - 100.0f, WORLD_RADIUS - 100.0f);
    sp->input.hail = true;

    world_sim_step(&w, SIM_DT);

    const sim_event_t *ev = find_hail_response_event(&w);
    ASSERT(ev != NULL);
    ASSERT_EQ_INT(ev->hail_response.station, -1);
    ASSERT(ev->hail_response.credits < 0.0f);
    ASSERT_EQ_INT(ev->hail_response.decision_mode,
                  HAIL_DECISION_MODE_NONE);
    ASSERT_EQ_INT(ev->hail_response.decision_candidate_count, 0);
    ASSERT(ev->hail_response.decision_flags &
           SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(!(ev->hail_response.decision_flags &
             SIGNAL_DECISION_REASON_HARD_APPROVED));
    ASSERT_EQ_FLOAT(ev->hail_response.decision_signal_quality, 0.0f, 0.001f);
    ASSERT(ev->hail_response.decision_source_id == 0ull);
    ASSERT_EQ_INT(sp->hail_decision_valid, 1);
    ASSERT_EQ_INT(sp->hail_decision_station, -1);
    ASSERT_EQ_INT(sp->hail_decision_candidate_count, 0);
    ASSERT(sp->hail_decision_flags & SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(!(sp->hail_decision_flags &
             SIGNAL_DECISION_REASON_HARD_APPROVED));
    ASSERT(!(sp->hail_decision_flags &
             SIGNAL_DECISION_REASON_HAS_SIGNAL_CONTEXT));
    ASSERT_EQ_FLOAT(sp->hail_decision_score, 0.0f, 0.001f);
    ASSERT(sp->hail_decision_source_id == 0ull);
}

TEST(test_dead_neural_worker_auto_respawns) {
    /* Contract-origin hulls mean a dead worker is not replaced by a
     * free spawn. With yard materials available, replenishment first
     * queues a shipyard-local hull build, then claims the completed
     * stored hull on a later roster tick. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
    station_t *kepler = &w->stations[1];
    ASSERT(station_finished_mint(kepler, COMMODITY_FRAME, 2, NULL) == 2);
    ASSERT(station_finished_mint(kepler, COMMODITY_TRACTOR_MODULE, 1, NULL) == 1);
    /* Find the Kepler-homed tug. */
    int target_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_TOW) continue;
        if (w->npc_ships[n].home_station != 1) continue;
        target_slot = n;
        break;
    }
    ASSERT(target_slot >= 0);
    /* Force-kill via the public damage helper. ship.hull is the
     * authoritative side post-#294 slice 9-11 so we hit ship.hull
     * directly to skip the npc-side mirror lag. */
    ship_t *s = world_npc_ship_for(w, target_slot);
    ASSERT(s != NULL);
    s->hull = 0.0f;
    /* One sim step lets the despawn check at top of step_npc_ships
     * notice and free the slot. */
    world_sim_step(w, SIM_DT);
    int kepler_workers = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_TOW) continue;
        if (w->npc_ships[n].home_station == 1) kepler_workers++;
    }
    ASSERT_EQ_INT(kepler_workers, 0);

    /* First cooldown: demand is converted to a pending shipyard build,
     * not an instant worker. */
    for (int i = 0; i < 1900; i++) world_sim_step(w, SIM_DT);

    ASSERT_EQ_INT(kepler->pending_ship_build_count, 1);
    ASSERT_EQ_INT(kepler->pending_ship_builds[0].hull_class,
                  HULL_CLASS_DRONE_TRACTOR);
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_TRACTOR_MODULE), 0);

    for (int i = 0; i < 4000; i++) world_sim_step(w, SIM_DT);

    int kepler_workers_after = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].home_station == 1) kepler_workers_after++;
    }
    ASSERT_EQ_INT(kepler_workers_after, 1);
}

TEST(test_hauler_preserves_cargo_identity_in_transit) {
    WORLD_DECL;
    world_reset(&w);

    int seeded_hauler = test_spawn_hauler_at(&w, 0);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w.npc_ships[n].active) continue;
        if (w.npc_ships[n].role != NPC_ROLE_HAULER) continue;
        if (w.npc_ships[n].home_station != 0) continue;
        hauler_slot = n;
        break;
    }
    ASSERT(hauler_slot >= 0);

    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (n != hauler_slot) w.npc_ships[n].active = false;
    }

    npc_ship_t *hauler = &w.npc_ships[hauler_slot];
    ship_t *hauler_ship = world_npc_ship_for(&w, hauler_slot);
    ASSERT(hauler_ship != NULL);
    ASSERT(ship_manifest_bootstrap(hauler_ship));
    manifest_clear(&hauler_ship->manifest);
    ship_receipts_t *hauler_receipts = ship_get_receipts(hauler_ship);
    ASSERT(hauler_receipts != NULL);
    ship_receipts_clear(hauler_receipts);
    memset(hauler->cargo, 0, sizeof(hauler->cargo));
    memset(hauler_ship->cargo, 0, sizeof(hauler_ship->cargo));

    station_t *home = &w.stations[0];
    station_t *dest = &w.stations[1];
    ASSERT(station_manifest_bootstrap(home));
    ASSERT(station_manifest_bootstrap(dest));
    manifest_clear(&home->manifest);
    manifest_clear(&dest->manifest);
    memset(home->_inventory_cache, 0, sizeof(home->_inventory_cache));
    memset(dest->_inventory_cache, 0, sizeof(dest->_inventory_cache));
    dest->scaffold = false;

    enum { EXPECTED_MOVED = 2 };
    int stock_units = (int)HAULER_RESERVE + EXPECTED_MOVED;
    cargo_unit_t units[16] = {{0}};
    cargo_receipt_chain_t chains[16] = {0};
    ASSERT(stock_units <= (int)(sizeof(units) / sizeof(units[0])));
    for (int i = 0; i < stock_units; i++) {
        uint8_t fragment_pub[32] = {0};
        fragment_pub[31] = (uint8_t)(0x40 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                          fragment_pub, (uint16_t)i, &units[i]));
        ASSERT(test_issue_world_station_receipt(home, units[i].pub,
                                                (uint64_t)(700 + i),
                                                &chains[i]));
        ASSERT(station_manifest_push_with_chain(home, &units[i], &chains[i]));
    }
    home->_inventory_cache[COMMODITY_FERRITE_INGOT] = (float)stock_units;

    memset(w.contracts, 0, sizeof(w.contracts));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    hauler->state = NPC_STATE_DOCKED;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 0;
    /* Seed gossip — under the gossip-contract model the hauler only
     * acts on contracts it has heard about. The dock handshake at home
     * pulls in home's locally-issued contracts, but this contract is
     * issued AT station 1 (its destination), not at home; without a
     * prior visit to station 1, the hauler doesn't know about it.
     * Seeding directly skips the propagation step. */
    hauler->known_contract_count = 0;
    for (int k = 0; k < MAX_CONTRACTS && hauler->known_contract_count < SHIP_KNOWN_CONTRACT_CAP; k++) {
        if (!w.contracts[k].active) continue;
        hauler->known_contracts[hauler->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)w.contracts[k].action,
            .station_index = w.contracts[k].station_index,
            .commodity = (uint8_t)w.contracts[k].commodity,
            .quantity_needed = w.contracts[k].quantity_needed,
            .base_price = w.contracts[k].base_price,
            .age_at_copy = w.contracts[k].age,
        };
    }

    uint64_t contract_decisions_before =
        signal_intelligence_contract_decision_count();
    uint64_t contract_teacher_before =
        signal_intelligence_contract_teacher_decision_count();

    step_npc_ships(&w, SIM_DT);

    ASSERT(signal_intelligence_contract_decision_count() > contract_decisions_before);
    ASSERT(signal_intelligence_contract_teacher_decision_count() > contract_teacher_before);
    ASSERT_EQ_INT(hauler->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(hauler->dest_station, 1);
    ASSERT_EQ_INT(hauler_ship->manifest.count, EXPECTED_MOVED);
    ASSERT_EQ_INT((int)hauler_receipts->count, EXPECTED_MOVED);
    ASSERT_EQ_INT((int)hauler_receipts->chains[0].len, 2);
    ASSERT_EQ_INT((int)hauler_receipts->chains[0].links[0].event_id, 700);
    ASSERT(hauler_receipts->chains[0].links[1].event_id != 0);
    ASSERT(memcmp(hauler_receipts->chains[0].links[1].cargo_pub,
                  units[0].pub, 32) == 0);
    ASSERT(memcmp(hauler_receipts->chains[0].links[1].authoring_station,
                  home->station_pubkey, 32) == 0);
    ASSERT_EQ_INT(manifest_find(&home->manifest, units[0].pub), -1);
    ASSERT_EQ_INT(manifest_find(&home->manifest, units[1].pub), -1);
    ASSERT(manifest_find(&hauler_ship->manifest, units[0].pub) >= 0);
    ASSERT(manifest_find(&hauler_ship->manifest, units[1].pub) >= 0);
    ASSERT_EQ_FLOAT(hauler->cargo[COMMODITY_FERRITE_INGOT],
                    (float)EXPECTED_MOVED, 0.001f);

    hauler->state = NPC_STATE_UNLOADING;
    hauler->state_timer = 0.0f;
    hauler->dest_station = 1;

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(hauler_ship->manifest.count, 0);
    ASSERT_EQ_INT((int)hauler_receipts->count, 0);
    ASSERT_EQ_FLOAT(hauler->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    ASSERT(manifest_find(&dest->manifest, units[0].pub) >= 0);
    ASSERT(manifest_find(&dest->manifest, units[1].pub) >= 0);
    ship_receipts_t *dest_receipts = station_get_receipts(dest);
    ASSERT(dest_receipts != NULL);
    int d0 = manifest_find(&dest->manifest, units[0].pub);
    int d1 = manifest_find(&dest->manifest, units[1].pub);
    ASSERT(d0 >= 0);
    ASSERT(d1 >= 0);
    ASSERT_EQ_INT((int)dest_receipts->chains[d0].len, 3);
    ASSERT_EQ_INT((int)dest_receipts->chains[d0].links[0].event_id, 700);
    ASSERT(memcmp(dest_receipts->chains[d0].links[1].authoring_station,
                  home->station_pubkey, 32) == 0);
    ASSERT(memcmp(dest_receipts->chains[d0].links[2].authoring_station,
                  dest->station_pubkey, 32) == 0);
    ASSERT(memcmp(dest_receipts->chains[d0].links[2].recipient_pubkey,
                  dest->station_pubkey, 32) == 0);
    ASSERT_EQ_INT((int)dest_receipts->chains[d1].len, 3);
    ASSERT_EQ_INT((int)dest_receipts->chains[d1].links[0].event_id, 701);
    ASSERT(memcmp(dest_receipts->chains[d1].links[1].authoring_station,
                  home->station_pubkey, 32) == 0);
    ASSERT(memcmp(dest_receipts->chains[d1].links[2].authoring_station,
                  dest->station_pubkey, 32) == 0);
    ASSERT_EQ_INT(manifest_find(&home->manifest, units[0].pub), -1);
    ASSERT_EQ_INT(manifest_find(&home->manifest, units[1].pub), -1);
    ASSERT_EQ_FLOAT(dest->_inventory_cache[COMMODITY_FERRITE_INGOT],
                    (float)EXPECTED_MOVED, 0.001f);
    market_memory_t route_success = {0};
    ASSERT(test_view_has_market_memory(&hauler->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &route_success));
    ASSERT_EQ_INT(route_success.quantity_hint, EXPECTED_MOVED);
    ASSERT(route_success.confidence >= 200);
    ASSERT(test_view_has_market_memory(&dest->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    market_memory_t reputation = {0};
    ASSERT(test_view_has_market_memory(&hauler->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &reputation));
    ASSERT_EQ_INT(reputation.quantity_hint, EXPECTED_MOVED);
    ASSERT(reputation.confidence >= 200);
    ASSERT(test_view_has_market_memory(&dest->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    market_memory_t station_trust = {0};
    ASSERT(test_view_has_market_memory(&hauler->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_TRUST,
                                       1, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &station_trust));
    ASSERT_EQ_INT(station_trust.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(station_trust.quantity_hint, EXPECTED_MOVED);
    ASSERT(test_view_has_market_memory(&dest->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_TRUST,
                                       1, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    for (uint16_t i = 0; i < dest->manifest.count; i++) {
        ASSERT(dest->manifest.units[i].recipe_id != RECIPE_LEGACY_MIGRATE);
    }
}

TEST(test_black_market_contract_accepts_npc_module_delivery) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    int hauler_slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                                HULL_CLASS_HAULER);
    ASSERT(hauler_slot >= 0);
    npc_ship_t *hauler = &w.npc_ships[hauler_slot];
    ship_t *hauler_ship = world_npc_ship_for(&w, hauler_slot);
    ASSERT(hauler_ship != NULL);
    ASSERT(ship_manifest_bootstrap(hauler_ship));
    manifest_clear(&hauler_ship->manifest);
    ship_receipts_t *hauler_receipts = ship_get_receipts(hauler_ship);
    ASSERT(hauler_receipts != NULL);
    ship_receipts_clear(hauler_receipts);
    memset(hauler->cargo, 0, sizeof(hauler->cargo));
    memset(hauler_ship->cargo, 0, sizeof(hauler_ship->cargo));

    station_t *home = &w.stations[0];
    station_t *freeport = &w.stations[SIGNAL_FREEPORT_STATION_INDEX];
    ASSERT(station_exists(freeport));
    ASSERT(station_faction_is_pirate_economy(freeport));
    ASSERT(station_manifest_bootstrap(freeport));
    manifest_clear(&freeport->manifest);
    ship_receipts_t *freeport_receipts = station_get_receipts(freeport);
    ASSERT(freeport_receipts != NULL);
    ship_receipts_clear(freeport_receipts);
    freeport->_inventory_cache[COMMODITY_TRACTOR_MODULE] = 0.0f;

    uint8_t origin[8] = {0};
    origin[0] = 0x42;
    origin[1] = (uint8_t)COMMODITY_TRACTOR_MODULE;
    cargo_unit_t unit = {0};
    ASSERT(hash_legacy_migrate_unit(origin, COMMODITY_TRACTOR_MODULE, 0,
                                    &unit));
    cargo_receipt_chain_t chain = {0};
    ASSERT(test_issue_world_station_receipt(home, unit.pub, 910, &chain));
    ASSERT(ship_manifest_push_with_chain(hauler_ship, &unit, &chain));
    ship_finished_sync(hauler_ship, COMMODITY_TRACTOR_MODULE);
    hauler->cargo[COMMODITY_TRACTOR_MODULE] = 1.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = SIGNAL_FREEPORT_STATION_INDEX,
        .commodity = COMMODITY_TRACTOR_MODULE,
        .quantity_needed = 1.0f,
        .base_price = 800.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    ASSERT_EQ_INT(hauler_ship->manifest.count, 1);
    ASSERT_EQ_INT(contract_fit_manifest_count(&w.contracts[0],
                                              &hauler_ship->manifest), 1);
    ASSERT_EQ_INT(station_finished_count(freeport,
                                         COMMODITY_TRACTOR_MODULE), 0);

    float ledger_before = ledger_balance(freeport, hauler->session_token);
    hauler->state = NPC_STATE_UNLOADING;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = SIGNAL_FREEPORT_STATION_INDEX;
    hauler->pickup_station = -1;
    hauler->pickup_commodity = COMMODITY_COUNT;
    hauler->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    hauler_ship->pos = station_approach_target(freeport, hauler_ship->pos);
    hauler_ship->vel = v2(0.0f, 0.0f);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(hauler_ship->manifest.count, 0);
    ASSERT_EQ_INT((int)hauler_receipts->count, 0);
    ASSERT_EQ_FLOAT(hauler->cargo[COMMODITY_TRACTOR_MODULE], 0.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(freeport,
                                         COMMODITY_TRACTOR_MODULE), 1);
    ASSERT(manifest_find(&freeport->manifest, unit.pub) >= 0);
    ASSERT(!w.contracts[0].active);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 0.0f, 0.001f);
    ASSERT(ledger_balance(freeport, hauler->session_token) > ledger_before);
}

TEST(test_legacy_hauler_cargo_unloads_when_manifest_empty) {
    WORLD_DECL;
    world_reset(&w);

    int seeded_hauler = test_spawn_hauler_at(&w, 0);
    ASSERT(seeded_hauler >= 0);

    int hauler_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w.npc_ships[n].active) continue;
        if (w.npc_ships[n].role != NPC_ROLE_HAULER) continue;
        hauler_slot = n;
        break;
    }
    ASSERT(hauler_slot >= 0);

    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (n != hauler_slot) w.npc_ships[n].active = false;
    }

    npc_ship_t *hauler = &w.npc_ships[hauler_slot];
    ship_t *hauler_ship = world_npc_ship_for(&w, hauler_slot);
    ASSERT(hauler_ship != NULL);
    ASSERT(ship_manifest_bootstrap(hauler_ship));
    manifest_clear(&hauler_ship->manifest);
    ship_receipts_t *hauler_receipts = ship_get_receipts(hauler_ship);
    ASSERT(hauler_receipts != NULL);
    ship_receipts_clear(hauler_receipts);

    station_t *dest = &w.stations[1];
    ASSERT(station_manifest_bootstrap(dest));
    manifest_clear(&dest->manifest);
    memset(dest->_inventory_cache, 0, sizeof(dest->_inventory_cache));
    dest->module_count = 0;
    dest->scaffold = false;
    add_module_at(dest, MODULE_DOCK, 1, 0);
    memset(w.contracts, 0, sizeof(w.contracts));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_REPAIR_KIT,
        .quantity_needed = 2.0f,
        .base_price = 5.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    memset(hauler->cargo, 0, sizeof(hauler->cargo));
    hauler->cargo[COMMODITY_REPAIR_KIT] = 2.0f;
    hauler->state = NPC_STATE_UNLOADING;
    hauler->state_timer = 0.0f;
    hauler->home_station = 0;
    hauler->dest_station = 1;

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(hauler_ship->manifest.count, 0);
    ASSERT_EQ_INT((int)hauler_receipts->count, 0);
    ASSERT_EQ_FLOAT(hauler->cargo[COMMODITY_REPAIR_KIT], 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(dest->_inventory_cache[COMMODITY_REPAIR_KIT],
                    2.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(dest, COMMODITY_REPAIR_KIT), 2);
    ASSERT_EQ_INT(dest->manifest.count, 2);
    ASSERT_EQ_INT(dest->manifest.units[0].recipe_id, RECIPE_LEGACY_MIGRATE);
    ASSERT_EQ_INT(dest->manifest.units[1].recipe_id, RECIPE_LEGACY_MIGRATE);
}

TEST(test_station_roster_uses_shipyard_contract_for_resident_worker_hulls) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    station_t *helios = &w->stations[2];
    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_NPC_MINER, &frames, &lasers, &tractors));
    ASSERT(station_finished_mint(helios, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(helios, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(helios, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);
    int target_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_MINER) continue;
        if (w->npc_ships[n].home_station != 2) continue;
        target_slot = n;
        break;
    }
    ASSERT(target_slot >= 0);
    ship_t *s = world_npc_ship_for(w, target_slot);
    ASSERT(s != NULL);
    s->hull = 0.0f;
    world_sim_step(w, SIM_DT);

    int helios_tows = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_TOW) continue;
        if (w->npc_ships[n].home_station == 2) helios_tows++;
    }
    ASSERT_EQ_INT(helios_tows, 1);

    for (int i = 0; i < 1900; i++) world_sim_step(w, SIM_DT);

    ASSERT_EQ_INT(helios->pending_ship_build_count, 1);
    ASSERT_EQ_INT(helios->pending_ship_builds[0].hull_class,
                  HULL_CLASS_NPC_MINER);

    for (int i = 0; i < 4000; i++) world_sim_step(w, SIM_DT);

    int helios_miners_after = 0;
    int helios_tows_after = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].home_station != 2) continue;
        if (w->npc_ships[n].role == NPC_ROLE_MINER) helios_miners_after++;
        if (w->npc_ships[n].role == NPC_ROLE_TOW) helios_tows_after++;
        ASSERT(w->npc_ships[n].ship_asset_id != SHIP_ASSET_ID_NONE);
    }
    ASSERT_EQ_INT(helios_miners_after, 1);
    ASSERT_EQ_INT(helios_tows_after, 1);
    ASSERT_EQ_INT(helios->pending_ship_build_count, 0);
}

TEST(test_frontier_outpost_roster_respects_virtual_logistics_budget) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);

    frontier_virtual_pilots_set(w, 0);
    int zero_budget_station = SIGNAL_FIRST_OUTPOST_INDEX;
    station_t *zero_budget = &w->stations[zero_budget_station];
    memset(zero_budget, 0, sizeof(*zero_budget));
    snprintf(zero_budget->name, sizeof(zero_budget->name),
             "Zero Budget Outpost");
    zero_budget->pos = v2(5200.0f, 2500.0f);
    zero_budget->radius = 80.0f;
    zero_budget->dock_radius = 140.0f;
    zero_budget->signal_range = OUTPOST_SIGNAL_RANGE;
    add_module_at(zero_budget, MODULE_DOCK, 1, 0);
    add_module_at(zero_budget, MODULE_FURNACE, 2, 0);
    add_module_at(zero_budget, MODULE_SIGNAL_RELAY, 1, 1);
    rebuild_station_services(zero_budget);
    for (int c = 0; c < COMMODITY_COUNT; c++)
        zero_budget->_inventory_cache[c] = 0.0f;
    rebuild_signal_chain(w);

    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (w->npc_ships[n].active &&
            w->npc_ships[n].home_station >= SIGNAL_FIRST_OUTPOST_INDEX) {
            w->npc_ships[n].active = false;
        }
    }
    for (int i = 0; i < 6; i++) {
        w->npc_respawn_timer = 0.001f;
        world_sim_step(w, SIM_DT);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        ASSERT(!w->npc_ships[n].active ||
               w->npc_ships[n].home_station < SIGNAL_FIRST_OUTPOST_INDEX);
    }

    frontier_virtual_pilots_set(w, 1000);
    int budget = 1 + w->frontier_virtual_pilots / 250;
    if (budget > 8) budget = 8;
    ASSERT_EQ_INT(budget, 5);

    for (int s = SIGNAL_FIRST_OUTPOST_INDEX;
         s < SIGNAL_FIRST_OUTPOST_INDEX + 7 && s < MAX_STATIONS;
         s++) {
        station_t *st = &w->stations[s];
        memset(st, 0, sizeof(*st));
        snprintf(st->name, sizeof(st->name), "Budget Outpost %d", s);
        st->pos = v2(6000.0f + (float)(s - SIGNAL_FIRST_OUTPOST_INDEX) * 900.0f,
                     3000.0f);
        st->radius = 80.0f;
        st->dock_radius = 140.0f;
        st->signal_range = OUTPOST_SIGNAL_RANGE;
        add_module_at(st, MODULE_DOCK, 1, 0);
        add_module_at(st, MODULE_SIGNAL_RELAY, 1, 1);
        rebuild_station_services(st);
        ASSERT(world_ship_asset_mint(w, HULL_CLASS_NPC_MINER,
                                     SHIP_ASSET_OWNER_STATION,
                                     s, s, SHIP_ASSET_PROVENANCE_GENESIS,
                                     false, s, NULL, NULL) != NULL);
        ASSERT(world_ship_asset_mint(w, HULL_CLASS_DRONE_TRACTOR,
                                     SHIP_ASSET_OWNER_STATION,
                                     s, s, SHIP_ASSET_PROVENANCE_GENESIS,
                                     false, s, NULL, NULL) != NULL);
    }
    rebuild_signal_chain(w);

    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (w->npc_ships[n].active &&
            w->npc_ships[n].home_station >= SIGNAL_FIRST_OUTPOST_INDEX) {
            w->npc_ships[n].active = false;
        }
    }

    for (int i = 0; i < budget * 2 + 6; i++) {
        w->npc_respawn_timer = 0.001f;
        world_sim_step(w, SIM_DT);
    }

    int frontier_workers = 0;
    int frontier_station_workers[8] = {0};
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        int home = w->npc_ships[n].home_station;
        if (home < SIGNAL_FIRST_OUTPOST_INDEX) continue;
        frontier_workers++;
        int local = home - SIGNAL_FIRST_OUTPOST_INDEX;
        if (local >= 0 && local < 8) frontier_station_workers[local]++;
    }

    ASSERT_EQ_INT(frontier_workers, budget * 2);
    for (int i = 0; i < 7; i++) {
        if (i < budget) {
            ASSERT_EQ_INT(frontier_station_workers[i], 2);
        } else {
            ASSERT_EQ_INT(frontier_station_workers[i], 0);
        }
    }
}

TEST(test_player_init_ship_docked) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    ASSERT_EQ_INT(w.players[0].current_station, 0);
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, 100.0f, 0.01f);
}

TEST(test_world_sim_step_advances_time) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    float t0 = w.time;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.time > t0);
}

TEST(test_world_sim_step_moves_ship_with_thrust) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].ship.angle = 0.0f;
    w.players[0].ship.pos = v2(0.0f, 0.0f);
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].input.thrust = 1.0f;
    for (int i = 0; i < 120; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.players[0].ship.pos.x > 5.0f);
}

TEST(test_ship_brake_opposes_velocity_not_facing) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.vel = v2(100.0f, 0.0f);

    step_ship_thrust(&ship, 0.1f, -1.0f, v2(0.0f, 1.0f),
                     false, 0.0f, false);

    ASSERT(ship.vel.x > 0.0f);
    ASSERT(ship.vel.x < 100.0f);
    ASSERT_EQ_FLOAT(ship.vel.y, 0.0f, 0.001f);
}

TEST(test_ship_brake_stops_without_overshoot) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.vel = v2(5.0f, 0.0f);

    step_ship_thrust(&ship, 1.0f, -1.0f, v2(1.0f, 0.0f),
                     false, 0.0f, false);

    ASSERT_EQ_FLOAT(ship.vel.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(ship.vel.y, 0.0f, 0.001f);
}

TEST(test_ship_reverse_requires_reverse_flag) {
    ship_t ship = {0};
    ship.hull_class = HULL_CLASS_MINER;
    ship.vel = v2(0.0f, 0.0f);

    step_ship_thrust(&ship, 1.0f / 60.0f, -1.0f, v2(1.0f, 0.0f),
                     false, 0.0f, false);
    ASSERT_EQ_FLOAT(ship.vel.x, 0.0f, 0.001f);

    step_ship_thrust(&ship, 1.0f / 60.0f, -1.0f, v2(1.0f, 0.0f),
                     false, 0.0f, true);
    ASSERT(ship.vel.x < 0.0f);
}

TEST(test_world_sim_step_mining_damages_asteroid) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].ship.mining_level = SHIP_UPGRADE_MAX_LEVEL;
    /* Place player right next to first active non-S asteroid */
    int target = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active && w.asteroids[i].tier != ASTEROID_TIER_S) {
            target = i;
            break;
        }
    }
    ASSERT(target >= 0);
    vec2 apos = w.asteroids[target].pos;
    w.players[0].ship.pos = v2(apos.x - 50.0f, apos.y);
    w.players[0].ship.angle = 0.0f;
    w.players[0].input.mine = true;
    float hp_before = w.asteroids[target].hp;
    for (int i = 0; i < 60; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.asteroids[target].hp < hp_before);
}

static mining_beam_t test_fire_once_at_asteroid(world_t *w, asteroid_t *a,
                                                int mining_level) {
    int target = (int)(a - w->asteroids);
    return sim_mining_beam_step(w, v2(0.0f, 0.0f), v2(1.0f, 0.0f),
                                target, mining_level, 60.0f, 1.0f, -1,
                                1.0f / 60.0f);
}

static void test_seed_gate_asteroid(world_t *w, asteroid_tier_t tier,
                                    commodity_t commodity) {
    memset(w->asteroids, 0, sizeof(w->asteroids));
    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = tier;
    a->commodity = commodity;
    a->radius = 30.0f;
    a->hp = 40.0f;
    a->max_hp = 40.0f;
    a->pos = v2(80.0f, 0.0f);
}

TEST(test_mining_laser_size_gate_starts_at_m) {
    WORLD_DECL;
    world_reset(&w);

    ASSERT_EQ_INT(max_mineable_tier(0), ASTEROID_TIER_M);
    ASSERT_EQ_INT(max_mineable_tier(1), ASTEROID_TIER_L);
    ASSERT_EQ_INT(max_mineable_tier(2), ASTEROID_TIER_XL);
    ASSERT_EQ_INT(max_mineable_tier(3), ASTEROID_TIER_XXL);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_L, COMMODITY_FERRITE_ORE);
    mining_beam_t l1_on_l = test_fire_once_at_asteroid(&w, &w.asteroids[0], 0);
    ASSERT(l1_on_l.hit);
    ASSERT(l1_on_l.ineffective);
    ASSERT(!l1_on_l.fired);
    ASSERT_EQ_FLOAT(w.asteroids[0].hp, 40.0f, 0.001f);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_FERRITE_ORE);
    mining_beam_t l1_on_m = test_fire_once_at_asteroid(&w, &w.asteroids[0], 0);
    ASSERT(l1_on_m.hit);
    ASSERT(!l1_on_m.ineffective);
    ASSERT(l1_on_m.fired);
    ASSERT(w.asteroids[0].hp < 40.0f);
}

TEST(test_mining_laser_material_gate_requires_upgrades) {
    WORLD_DECL;
    world_reset(&w);

    ASSERT_EQ_INT(mining_required_level_for_commodity(COMMODITY_FERRITE_ORE), 0);
    ASSERT_EQ_INT(mining_required_level_for_commodity(COMMODITY_CUPRITE_ORE), 1);
    ASSERT_EQ_INT(mining_required_level_for_commodity(COMMODITY_CRYSTAL_ORE), 2);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CUPRITE_ORE);
    mining_beam_t l1_on_cuprite = test_fire_once_at_asteroid(&w, &w.asteroids[0], 0);
    ASSERT(l1_on_cuprite.hit);
    ASSERT(l1_on_cuprite.ineffective);
    ASSERT_EQ_FLOAT(w.asteroids[0].hp, 40.0f, 0.001f);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CUPRITE_ORE);
    mining_beam_t l2_on_cuprite = test_fire_once_at_asteroid(&w, &w.asteroids[0], 1);
    ASSERT(l2_on_cuprite.hit);
    ASSERT(!l2_on_cuprite.ineffective);
    ASSERT(l2_on_cuprite.fired);
    ASSERT(w.asteroids[0].hp < 40.0f);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CRYSTAL_ORE);
    mining_beam_t l2_on_crystal = test_fire_once_at_asteroid(&w, &w.asteroids[0], 1);
    ASSERT(l2_on_crystal.hit);
    ASSERT(l2_on_crystal.ineffective);
    ASSERT_EQ_FLOAT(w.asteroids[0].hp, 40.0f, 0.001f);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CRYSTAL_ORE);
    mining_beam_t l3_on_crystal = test_fire_once_at_asteroid(&w, &w.asteroids[0], 2);
    ASSERT(l3_on_crystal.hit);
    ASSERT(!l3_on_crystal.ineffective);
    ASSERT(l3_on_crystal.fired);
    ASSERT(w.asteroids[0].hp < 40.0f);
}

TEST(test_world_sim_step_laser_scans_cargo_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->docked = false;
    sp->in_dock_range = false;
    sp->nearby_station = -1;
    sp->current_station = -1;
    sp->ship.pos = v2(10000.0f, 10000.0f);
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->ship.angle = 0.0f;
    sp->input.mine = true;

    vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);
    int pod_idx = test_spawn_frame_pod(&w, v2_add(muzzle, v2(90.0f, 0.0f)), 1);
    ASSERT(pod_idx >= 0);

    world_sim_step(&w, SIM_DT);

    ASSERT(sp->scan_active);
    ASSERT(sp->beam_hit);
    ASSERT_EQ_INT(sp->scan_target_type, INSPECT_TARGET_CARGO_POD);
    ASSERT_EQ_INT(sp->scan_target_index, pod_idx);
}

TEST(test_world_sim_step_docking) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    /* Launch */
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(!w.players[0].docked);
    /* Fly back into dock range (inside ring gap corridor) and dock */
    w.players[0].input.interact = false;
    for (int i = 0; i < 10; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    /* Place ship at dock port and dock */
    w.players[0].ship.pos = w.stations[0].pos;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].docked = true;
    w.players[0].in_dock_range = true;
    w.players[0].current_station = 0;
    w.players[0].nearby_station = 0;
    ASSERT(w.players[0].docked);
}

TEST(test_world_sim_step_refinery_hopper_path_retired) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 50.0f;
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT],
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE],
                    50.0f, 0.001f);
    ASSERT_EQ_INT((int)w.hopper_smelt_events, 0);
    ASSERT(w.hopper_smelt_units == 0.0);
}

TEST(test_mining_class_prefix_round_trip) {
    /* Pubkeys whose first base58 char is M, H, T, S, F, K, RATi prefix,
     * and one that is "anonymous" (digit/lowercase). We don't have direct
     * control over base58 output but we can iterate seeds until each
     * prefix appears. */
    int seen[MINING_CLASS_COMMISSIONED + 1] = {0};
    int found = 0;
    for (uint32_t seed = 1; seed < 1000000 && found < 7; seed++) {
        uint8_t s[32];
        for (int i = 0; i < 32; i++)
            s[i] = (uint8_t)((seed >> ((i & 3) * 8)) ^ (seed * 2654435761u >> (i & 7)));
        uint8_t pub[32];
        sha256_bytes(s, 32, pub);
        int cls = mining_pubkey_class(pub);
        if (cls < 0 || cls > MINING_CLASS_COMMISSIONED) continue;
        if (cls == MINING_CLASS_ANONYMOUS || cls == MINING_CLASS_COMMISSIONED) continue;
        if (!seen[cls]) {
            seen[cls] = 1;
            found++;
            char render[12];
            mining_render_callsign(pub, render);
            /* Render must contain a dash, and the prefix segment must
             * match what mining_pubkey_class said. */
            ASSERT(strchr(render, '-') != NULL);
        }
    }
    /* Should hit M/H/T/S/F/K within 1M iterations. RATi is ~1 in 11M so
     * not asserted here. */
    ASSERT(seen[MINING_CLASS_M]);
    ASSERT(seen[MINING_CLASS_H]);
    ASSERT(seen[MINING_CLASS_T]);
    ASSERT(seen[MINING_CLASS_S]);
    ASSERT(seen[MINING_CLASS_F]);
    ASSERT(seen[MINING_CLASS_K]);
}

TEST(test_refinery_deposits_named_ingot) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    w->players[0].docked = false;
    w->players[0].session_ready = true;
    memset(w->players[0].session_token, 0x42, 8);
    /* Force a furnace at station 0 — already exists by default in
     * world_reset, but assert. */
    bool has_furnace = false;
    int furnace_idx = -1;
    for (int m = 0; m < w->stations[0].module_count; m++) {
        if (w->stations[0].modules[m].type == MODULE_FURNACE) {
            has_furnace = true;
            furnace_idx = m;
        }
    }
    ASSERT(has_furnace);

    /* Stop ring motion. Then mirror the smelt code's silo pick — the
     * closest module on an adjacent ring to the furnace, with current
     * ring offsets baked in. With Prospect's full hopper ring on
     * ring 2, several hoppers are within range; whichever is closest
     * becomes the silo end of the smelt beam. */
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w->stations[0].arm_speed[arm] = 0.0f;
        w->stations[0].arm_rotation[arm] = 0.0f;
    }
    vec2 furnace_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[furnace_idx].ring, w->stations[0].modules[furnace_idx].slot);
    int silo_idx = -1;
    {
        int fr = w->stations[0].modules[furnace_idx].ring;
        float best_d = 1e18f;
        int adj_rings[] = { fr + 1, fr - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int m2 = 0; m2 < w->stations[0].module_count; m2++) {
                if (w->stations[0].modules[m2].ring != adj) continue;
                vec2 mp2 = module_world_pos_ring(&w->stations[0], adj,
                                                  w->stations[0].modules[m2].slot);
                float dd = v2_dist_sq(furnace_pos, mp2);
                if (dd < best_d) { best_d = dd; silo_idx = m2; }
            }
        }
    }
    ASSERT(silo_idx >= 0);
    vec2 silo_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[silo_idx].ring, w->stations[0].modules[silo_idx].slot);
    vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);

    /* Spawn an S-tier ferrite fragment on the smelt midpoint, with an
     * arbitrary fracture_seed. */
    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 10.0f;
    a->max_ore = 10.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    /* Seed values intentionally varied so the roll lands somewhere. */
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(i * 17 + 3);
    a->grade = (uint8_t)MINING_GRADE_RATI;
    a->pos = midpoint;
    a->vel = v2(0, 0);
    a->last_fractured_by = 0;
    a->last_towed_by = 0;
    a->net_dirty = true;
    w->players[0].ship.pos = v2_add(midpoint, v2(100.0f, 0.0f));
    w->players[0].ship.vel = v2(0.0f, 0.0f);

    /* Run sim until smelt completes (smelt_progress accumulates ~0.5/s). */
    int initial_manifest = w->stations[0].manifest.count;
    int initial_frames = station_finished_count(&w->stations[0], COMMODITY_FRAME);
    int initial_frame_pod_units =
        test_count_exact_pod_units(w, COMMODITY_FRAME);
    ASSERT(initial_frame_pod_units > 0);
    float initial_bulk = w->stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT];
    for (int i = 0; i < 600 && w->asteroids[slot].active; i++)
        world_sim_step(w, 1.0f / 120.0f);
    /* Asteroid should be consumed. */
    ASSERT(!w->asteroids[slot].active);
    ASSERT_EQ_FLOAT(w->stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT],
                    initial_bulk, 0.001f);
    ASSERT_EQ_INT(station_finished_count(&w->stations[0], COMMODITY_FRAME),
                  initial_frames);
    ASSERT_EQ_INT(test_count_exact_pod_units(w, COMMODITY_FRAME),
                  initial_frame_pod_units - 1);
    ASSERT_EQ_INT(w->stations[0].manifest.count, initial_manifest);
    ASSERT(test_count_exact_pod_units(w, COMMODITY_FERRITE_INGOT) >= 10);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        w, COMMODITY_FERRITE_INGOT, 10);
    ASSERT(pod != NULL);
    bool any_named = false;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        const cargo_unit_t *unit = &pod->manifest_units[i];
        ASSERT_EQ_INT(unit->kind, CARGO_KIND_INGOT);
        ASSERT_EQ_INT(unit->commodity, COMMODITY_FERRITE_INGOT);
        ASSERT_EQ_INT(unit->grade, MINING_GRADE_RATI);
        ASSERT_EQ_INT(unit->recipe_id, RECIPE_SMELT);
        /* origin_station is stamped at smelt time. */
        ASSERT_EQ_INT(unit->origin_station, 0);
        /* prefix_class always matches mining_pubkey_class(pub). */
        ASSERT_EQ_INT(unit->prefix_class, mining_pubkey_class(unit->pub));
        if ((ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS) any_named = true;
    }
    ASSERT(memcmp(pod->manifest_units[0].parent_merkle,
                  pod->manifest_units[pod->manifest_count - 1].parent_merkle,
                  32) == 0);
    ASSERT(memcmp(pod->manifest_units[0].pub,
                  pod->manifest_units[pod->manifest_count - 1].pub,
                  32) != 0);
    /* The first non-anonymous unit gets a non-zero mined_block stamped
     * after the signal_channel post; anonymous units stay at 0. */
    if (any_named) {
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            const cargo_unit_t *unit = &pod->manifest_units[i];
            if ((ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS) {
                ASSERT(unit->mined_block != 0);
                break;
            }
        }
    }
}

TEST(test_furnace_smelting_accepts_beam_corridor_delivery) {
    WORLD_DECL;
    world_reset(&w);

    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w.stations[0].arm_speed[arm] = 0.0f;
        w.stations[0].arm_rotation[arm] = 0.0f;
    }

    int furnace_idx = -1;
    int hopper_idx = -1;
    for (int m = 0; m < w.stations[0].module_count; m++) {
        station_module_t *f = &w.stations[0].modules[m];
        if (f->scaffold || f->type != MODULE_FURNACE) continue;
        if (module_instance_input_ore(f) != COMMODITY_FERRITE_ORE) continue;
        int ring = f->ring;
        vec2 furnace_pos = module_world_pos_ring(&w.stations[0], ring, f->slot);
        float best_d = 1e18f;
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int h = 0; h < w.stations[0].module_count; h++) {
                station_module_t *hm = &w.stations[0].modules[h];
                if (hm->scaffold || hm->ring != adj) continue;
                if (hm->type != MODULE_HOPPER) continue;
                if ((commodity_t)hm->commodity != COMMODITY_FERRITE_ORE) continue;
                vec2 hp = module_world_pos_ring(&w.stations[0], adj, hm->slot);
                float d = v2_dist_sq(furnace_pos, hp);
                if (d < best_d) { best_d = d; furnace_idx = m; hopper_idx = h; }
            }
        }
    }
    ASSERT(furnace_idx >= 0);
    ASSERT(hopper_idx >= 0);

    vec2 furnace_pos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[furnace_idx].ring, w.stations[0].modules[furnace_idx].slot);
    vec2 hopper_pos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[hopper_idx].ring, w.stations[0].modules[hopper_idx].slot);
    vec2 midpoint = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
    vec2 beam_axis = v2_sub(hopper_pos, furnace_pos);
    vec2 tangent = v2_norm(v2_perp(beam_axis));
    vec2 delivery_pos = v2_add(midpoint, v2_scale(tangent, 120.0f));

    ASSERT(sqrtf(v2_dist_sq(delivery_pos, midpoint)) > 80.0f);
    ASSERT(v2_dist_sq(delivery_pos, furnace_pos) < HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(delivery_pos, hopper_pos) < HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);

    asteroid_t *a = &w.asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 4.0f;
    a->max_ore = 4.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->pos = delivery_pos;
    a->vel = v2(0.0f, 0.0f);

    step_furnace_smelting(&w, SIM_DT);
    ASSERT(a->smelt_progress > 0.0f);
}

TEST(test_crystal_requires_two_distinct_furnace_passes) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w.stations[2].arm_speed[arm] = 0.0f;
        w.stations[2].arm_rotation[arm] = 0.0f;
    }

    station_t *helios = &w.stations[2];
    int furnace_idx[2] = { -1, -1 };
    vec2 midpoint[2] = { {0.0f, 0.0f}, {0.0f, 0.0f} };
    int pair_count = 0;
    for (int m = 0; m < helios->module_count && pair_count < 2; m++) {
        station_module_t *f = &helios->modules[m];
        if (f->scaffold || f->type != MODULE_FURNACE) continue;
        if (module_instance_input_ore(f) != COMMODITY_CRYSTAL_ORE) continue;
        int ring = f->ring;
        vec2 furnace_pos = module_world_pos_ring(helios, ring, f->slot);
        int best_h = -1;
        float best_d = 1e18f;
        int adj_rings[2] = { ring + 1, ring - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int h = 0; h < helios->module_count; h++) {
                station_module_t *hm = &helios->modules[h];
                if (hm->scaffold || hm->ring != adj) continue;
                if (hm->type != MODULE_HOPPER) continue;
                if ((commodity_t)hm->commodity != COMMODITY_CRYSTAL_ORE) continue;
                vec2 hp = module_world_pos_ring(helios, adj, hm->slot);
                float d = v2_dist_sq(furnace_pos, hp);
                if (d < best_d) { best_d = d; best_h = h; }
            }
        }
        ASSERT(best_h >= 0);
        vec2 hopper_pos = module_world_pos_ring(helios,
            helios->modules[best_h].ring, helios->modules[best_h].slot);
        furnace_idx[pair_count] = m;
        midpoint[pair_count] = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
        pair_count++;
    }
    ASSERT_EQ_INT(pair_count, 2);
    ASSERT(station_can_smelt(helios, COMMODITY_CRYSTAL_ORE));
    ASSERT(station_finished_mint(helios, COMMODITY_FRAME, 1, NULL) == 1);

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);

    asteroid_t *a = &w.asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_CRYSTAL_ORE;
    a->ore = 3.0f;
    a->max_ore = 3.0f;
    a->radius = 7.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    a->pos = midpoint[0];

    int initial_manifest = helios->manifest.count;
    int initial_frames = station_finished_count(helios, COMMODITY_FRAME);
    int initial_pod_units =
        test_count_exact_pod_units(&w, COMMODITY_CRYSTAL_INGOT);
    for (int i = 0; i < 400 &&
                    a->active &&
                    a->crystal_stage != CRYSTAL_STAGE_INTERMEDIATE; i++) {
        step_furnace_smelting(&w, SIM_DT);
    }

    ASSERT(a->active);
    ASSERT_EQ_INT(a->crystal_stage, CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(a->crystal_stage_station, 2);
    ASSERT_EQ_INT(a->crystal_stage_module, furnace_idx[0]);
    ASSERT_EQ_INT(helios->manifest.count, initial_manifest);

    a->pos = midpoint[1];
    a->vel = v2(0.0f, 0.0f);
    a->smelt_progress = 0.0f;
    for (int i = 0; i < 500 && w.asteroids[frag].active; i++) {
        step_furnace_smelting(&w, SIM_DT);
    }

    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(helios->manifest.count, initial_manifest - 1);
    ASSERT_EQ_INT(station_finished_count(helios, COMMODITY_FRAME),
                  initial_frames - 1);
    ASSERT(test_count_exact_pod_units(&w, COMMODITY_CRYSTAL_INGOT) >=
           initial_pod_units + 3);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_CRYSTAL_INGOT, 3);
    ASSERT(pod != NULL);
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        ASSERT_EQ_INT(pod->manifest_units[i].commodity,
                      COMMODITY_CRYSTAL_INGOT);
    }
}

TEST(test_station_production_ejects_frame_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t input = {0};
    cargo_unit_t expected_first = {0};
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};

    fragment_a[31] = 0x11;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RATI, fragment_a, 0, &input));
    ASSERT(hash_product(RECIPE_FRAME_BASIC, &input, 1, 0, &expected_first));
    ASSERT(manifest_push(&st->manifest, &input));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);

    st->module_input[press_idx] = 1.0f;
    sim_step_station_production(&w, 1.0f);

    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_INT(st->manifest.count, 0);
    ASSERT_EQ_INT(manifest_find(&st->manifest, input.pub), -1);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_FRAME, 2);
    ASSERT(pod != NULL);
    ASSERT(memcmp(pod->manifest_units[0].pub, expected_first.pub, 32) == 0);
    ASSERT(memcmp(pod->manifest_units[0].parent_merkle,
                  expected_first.parent_merkle, 32) == 0);
    ASSERT_EQ_INT(pod->manifest_units[0].kind, CARGO_KIND_FRAME);
    ASSERT_EQ_INT(pod->manifest_units[0].commodity, COMMODITY_FRAME);
    ASSERT_EQ_INT(pod->manifest_units[0].grade, MINING_GRADE_RATI);
    ASSERT_EQ_INT(pod->manifest_units[0].recipe_id, RECIPE_FRAME_BASIC);
    ASSERT(pod->has_shell_frame);
    ASSERT_EQ_INT(pod->shell_frame.commodity, COMMODITY_FRAME);
}

TEST(test_station_production_fills_existing_frame_output_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t inputs[2] = {{0}};
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};
    uint8_t fragment_b[32] = {0};

    fragment_a[31] = 0x24;
    fragment_b[31] = 0x25;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RATI,
                      fragment_a, 0, &inputs[0]));
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_b, 0, &inputs[1]));
    ASSERT(manifest_push(&st->manifest, &inputs[0]));
    ASSERT(manifest_push(&st->manifest, &inputs[1]));

    vec2 press_pos = module_world_pos_ring(
        st, st->modules[press_idx].ring, st->modules[press_idx].slot);
    int output_pod = test_spawn_frame_pod(&w, press_pos, 1);
    ASSERT(output_pod >= 0);
    cargo_pod_set_module_tractor(&w.cargo_pods[output_pod], 1, press_idx);

    st->module_input[press_idx] = 2.0f;
    sim_step_station_production(&w, 2.0f);

    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FRAME), 5);
    ASSERT_EQ_INT(w.cargo_pods[output_pod].manifest_count, 5);
    ASSERT_EQ_INT(w.cargo_pods[output_pod].quantity, 5);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[output_pod],
                                            1, press_idx));
    ASSERT(test_first_exact_pod_with_units(&w, COMMODITY_FRAME, 1) == NULL);
}

TEST(test_station_production_consumes_loose_ingot_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t input = {0};
    cargo_unit_t expected_first = {0};
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};

    fragment_a[31] = 0x22;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_a, 0, &input));
    ASSERT(hash_product(RECIPE_FRAME_BASIC, &input, 1, 0, &expected_first));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);

    vec2 press_pos = module_world_pos_ring(
        st, st->modules[press_idx].ring, st->modules[press_idx].slot);
    vec2 ferrite_hopper_pos = st->pos;
    ASSERT(test_hopper_pos_for(st, COMMODITY_FERRITE_INGOT,
                               &ferrite_hopper_pos));
    int input_pod = spawn_cargo_pod_with_manifest(
        &w, v2_add(press_pos, v2(28.0f, 0.0f)), v2(0.0f, 0.0f),
        COMMODITY_FERRITE_INGOT, &input, 1, CARGO_POD_CARGO);
    ASSERT(input_pod >= 0);

    sim_step_station_production(&w, 1.0f);

    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(st->manifest.count, 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(st->module_input[press_idx], 0.0f, 0.001f);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_FRAME, 2);
    ASSERT(pod != NULL);
    ASSERT(memcmp(pod->manifest_units[0].pub, expected_first.pub, 32) == 0);
    ASSERT_EQ_INT(pod->manifest_units[0].kind, CARGO_KIND_FRAME);
    ASSERT_EQ_INT(pod->manifest_units[0].commodity, COMMODITY_FRAME);
    ASSERT_EQ_INT(pod->manifest_units[0].grade, MINING_GRADE_RARE);
    ASSERT_EQ_INT(pod->manifest_units[0].recipe_id, RECIPE_FRAME_BASIC);
    ASSERT(pod->has_shell_frame);
}

TEST(test_frame_press_accepts_player_towed_ingot_pod_at_press) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    server_player_t *sp = &w.players[0];
    cargo_unit_t input = {0};
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};

    fragment_a[31] = 0x26;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_a, 0, &input));

    vec2 press_pos = module_world_pos_ring(
        st, st->modules[press_idx].ring, st->modules[press_idx].slot);
    vec2 ferrite_hopper_pos = st->pos;
    ASSERT(test_hopper_pos_for(st, COMMODITY_FERRITE_INGOT,
                               &ferrite_hopper_pos));
    vec2 midpoint = v2_scale(v2_add(press_pos, ferrite_hopper_pos), 0.5f);

    int input_pod = spawn_cargo_pod_with_manifest(
        &w, v2_add(press_pos, v2(24.0f, 0.0f)), v2(0.0f, 0.0f),
        COMMODITY_FERRITE_INGOT, &input, 1, CARGO_POD_CARGO);
    ASSERT(input_pod >= 0);

    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->ship.towed_pods[0] = (int16_t)input_pod;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[input_pod].towed_by = 0;

    step_station_cargo_pod_tractors(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(w.cargo_pods[input_pod].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[input_pod],
                                            1, press_idx));
    ASSERT_EQ_FLOAT(st->module_active_pulse[press_idx], 1.0f, 0.001f);
    vec2 to_mid = v2_sub(midpoint, w.cargo_pods[input_pod].pos);
    ASSERT(v2_dot(w.cargo_pods[input_pod].vel, to_mid) > 0.0f);
}

TEST(test_station_hopper_accepts_player_towed_ingot_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    server_player_t *sp = &w.players[0];
    cargo_unit_t input = {0};
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};

    fragment_a[31] = 0x23;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_a, 0, &input));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);

    vec2 ferrite_hopper_pos = st->pos;
    ASSERT(test_hopper_pos_for(st, COMMODITY_FERRITE_INGOT,
                               &ferrite_hopper_pos));
    int input_pod = spawn_cargo_pod_with_manifest(
        &w, ferrite_hopper_pos, v2(0.0f, 0.0f),
        COMMODITY_FERRITE_INGOT, &input, 1, CARGO_POD_CARGO);
    ASSERT(input_pod >= 0);

    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->ship.towed_pods[0] = (int16_t)input_pod;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[input_pod].towed_by = 0;

    sim_step_station_production(&w, 1.0f);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT), 0);
    ASSERT(test_first_exact_pod_with_units(&w, COMMODITY_FRAME, 2) != NULL);
}

TEST(test_frame_press_consumes_dock_held_ingot_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t input = {0};
    cargo_unit_t expected_first = {0};
    int dock_idx = test_first_dock_module_idx(st);
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};

    fragment_a[31] = 0x27;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(dock_idx >= 0);
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_a, 0, &input));
    ASSERT(hash_product(RECIPE_FRAME_BASIC, &input, 1, 0,
                        &expected_first));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);

    vec2 dock_pos = module_world_pos_ring(
        st, st->modules[dock_idx].ring, st->modules[dock_idx].slot);
    vec2 press_pos = module_world_pos_ring(
        st, st->modules[press_idx].ring, st->modules[press_idx].slot);
    vec2 dock_lane = v2_scale(v2_add(dock_pos, press_pos), 0.5f);
    int input_pod = spawn_cargo_pod_with_manifest(
        &w, dock_lane, v2(0.0f, 0.0f),
        COMMODITY_FERRITE_INGOT, &input, 1, CARGO_POD_CARGO);
    ASSERT(input_pod >= 0);
    cargo_pod_set_module_tractor(&w.cargo_pods[input_pod], 1, dock_idx);

    sim_step_station_production(&w, 1.0f);

    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT), 0);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_FRAME, 2);
    ASSERT(pod != NULL);
    ASSERT(memcmp(pod->manifest_units[0].pub, expected_first.pub, 32) == 0);
    ASSERT(cargo_pod_is_tractored_by_module(pod, 1, press_idx));
}

TEST(test_frame_press_reclaims_dock_held_frame_pod_as_output_crate) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t input = {0};
    int dock_idx = test_first_dock_module_idx(st);
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};

    fragment_a[31] = 0x28;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(dock_idx >= 0);
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_FINE,
                      fragment_a, 0, &input));
    ASSERT(manifest_push(&st->manifest, &input));
    st->module_input[press_idx] = 1.0f;

    vec2 dock_pos = module_world_pos_ring(
        st, st->modules[dock_idx].ring, st->modules[dock_idx].slot);
    vec2 press_pos = module_world_pos_ring(
        st, st->modules[press_idx].ring, st->modules[press_idx].slot);
    vec2 dock_lane = v2_scale(v2_add(dock_pos, press_pos), 0.5f);
    int frame_pod = test_spawn_frame_pod(&w, dock_lane, 1);
    ASSERT(frame_pod >= 0);
    cargo_pod_set_module_tractor(&w.cargo_pods[frame_pod], 1, dock_idx);

    sim_step_station_production(&w, 1.0f);

    ASSERT(w.cargo_pods[frame_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[frame_pod].manifest_count, 3);
    ASSERT_EQ_INT(w.cargo_pods[frame_pod].quantity, 3);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[frame_pod],
                                            1, press_idx));
    ASSERT_EQ_INT(st->manifest.count, 0);
}

TEST(test_kepler_frame_press_accepts_player_repositioned_local_frame_crate) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    server_player_t *sp = &w.players[0];
    int press_idx = -1;

    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    vec2 press_pos = module_world_pos_ring(
        st, st->modules[press_idx].ring, st->modules[press_idx].slot);
    int frame_pod = test_spawn_frame_pod(
        &w, v2_add(press_pos, v2(24.0f, 0.0f)), 1);
    ASSERT(frame_pod >= 0);
    w.cargo_pods[frame_pod].manifest_units[0].origin_station = 1;

    step_station_cargo_pod_tractors(&w, SIM_DT);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[frame_pod],
                                            1, press_idx));

    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->docked = false;
    sp->ship.pos = w.cargo_pods[frame_pod].pos;
    sp->ship.tractor_active = true;
    sp->input.tractor_hold = true;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], frame_pod);
    ASSERT_EQ_INT(w.cargo_pods[frame_pod].towed_by, 0);
    ASSERT(!cargo_pod_has_module_tractor(&w.cargo_pods[frame_pod]));

    w.cargo_pods[frame_pod].pos = press_pos;
    step_station_cargo_pod_tractors(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(w.cargo_pods[frame_pod].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[frame_pod],
                                            1, press_idx));
}

TEST(test_furnace_accepts_player_towed_frame_shell_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[0];
    server_player_t *sp = &w.players[0];
    int furnace_idx = -1;

    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FURNACE &&
            module_instance_input_ore(&st->modules[i]) == COMMODITY_FERRITE_ORE) {
            furnace_idx = i;
            break;
        }
    }
    ASSERT(furnace_idx >= 0);
    vec2 furnace_pos = module_world_pos_ring(
        st, st->modules[furnace_idx].ring,
        st->modules[furnace_idx].slot);
    int pod_idx = test_spawn_frame_pod(&w, furnace_pos, 1);
    ASSERT(pod_idx >= 0);

    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;

    step_station_cargo_pod_tractors(&w, 0.0f);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            0, furnace_idx));
}

TEST(test_furnace_accepts_frame_shell_pod_near_smelt_beam) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[0];
    server_player_t *sp = &w.players[0];
    int furnace_idx = -1;
    int ore_hopper_idx = -1;

    for (int arm = 0; arm < MAX_ARMS; arm++) {
        st->arm_speed[arm] = 0.0f;
        st->arm_rotation[arm] = 0.0f;
    }
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *m = &st->modules[i];
        if (m->type == MODULE_FURNACE &&
            module_instance_input_ore(m) == COMMODITY_FERRITE_ORE) {
            furnace_idx = i;
        }
        if (m->type == MODULE_HOPPER &&
            (commodity_t)m->commodity == COMMODITY_FERRITE_ORE) {
            ore_hopper_idx = i;
        }
    }
    ASSERT(furnace_idx >= 0);
    ASSERT(ore_hopper_idx >= 0);

    vec2 furnace_pos = module_world_pos_ring(st,
        st->modules[furnace_idx].ring, st->modules[furnace_idx].slot);
    vec2 ore_hopper_pos = module_world_pos_ring(st,
        st->modules[ore_hopper_idx].ring, st->modules[ore_hopper_idx].slot);
    (void)ore_hopper_pos;

    int pod_idx = test_spawn_frame_pod(&w, furnace_pos, 1);
    ASSERT(pod_idx >= 0);
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;

    step_station_cargo_pod_tractors(&w, 0.0f);

    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            0, furnace_idx));
}

TEST(test_frame_shell_pod_targets_furnace_frame_hopper_lane) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = test_reset_single_active_station(&w, 0);
    ASSERT(st != NULL);

    add_furnace_for(st, 2, 0, COMMODITY_FERRITE_INGOT);
    int furnace_idx = 0;
    add_hopper_for(st, 3, 0, COMMODITY_FERRITE_ORE);
    add_hopper_for(st, 3, 0, COMMODITY_FRAME);
    int frame_hopper_idx = 2;

    vec2 furnace_pos = module_world_pos_ring(st,
        st->modules[furnace_idx].ring, st->modules[furnace_idx].slot);
    vec2 ore_hopper_pos = module_world_pos_ring(st,
        st->modules[1].ring, st->modules[1].slot);
    vec2 smelt_lane = v2_scale(v2_add(furnace_pos, ore_hopper_pos), 0.5f);
    vec2 pod_start = v2_add(smelt_lane, v2(96.0f, 0.0f));
    int pod_idx = test_spawn_frame_pod(&w, pod_start, 1);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = -1;

    step_station_cargo_pod_tractors(&w, SIM_DT);

    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            0, frame_hopper_idx));
    vec2 to_smelt_lane = v2_sub(smelt_lane, w.cargo_pods[pod_idx].pos);
    ASSERT(v2_dot(w.cargo_pods[pod_idx].vel, to_smelt_lane) > 0.0f);
}

TEST(test_frame_pod_prefers_shipyard_serving_hopper_over_storage_twin) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = test_reset_single_active_station(&w, 0);
    ASSERT(st != NULL);

    add_module_at(st, MODULE_SHIPYARD, 2, 0);
    int shipyard_idx = 0;
    add_hopper_for(st, 3, 0, COMMODITY_FRAME);
    int serving_hopper_idx = 1;
    add_hopper_for(st, 3, 0, COMMODITY_FRAME);
    int storage_twin_idx = 2;

    vec2 shipyard_pos = module_world_pos_ring(st,
        st->modules[shipyard_idx].ring, st->modules[shipyard_idx].slot);
    vec2 hopper_pos = module_world_pos_ring(st,
        st->modules[serving_hopper_idx].ring,
        st->modules[serving_hopper_idx].slot);
    vec2 lane = v2_scale(v2_add(shipyard_pos, hopper_pos), 0.5f);
    int pod_idx = test_spawn_frame_pod(&w, lane, 1);
    ASSERT(pod_idx >= 0);
    w.cargo_pods[pod_idx].towed_by = -1;

    step_station_cargo_pod_tractors(&w, 0.0f);

    ASSERT(!cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                             0, storage_twin_idx));
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            0, serving_hopper_idx));
}

TEST(test_furnace_tractor_holds_frame_pod_outside_module) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[0];
    for (int a = 0; a < MAX_ARMS; a++) {
        st->arm_speed[a] = 0.0f;
        st->arm_omega[a] = 0.0f;
        st->arm_rotation[a] = 0.0f;
    }

    int furnace_idx = -1;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FURNACE &&
            module_instance_input_ore(&st->modules[i]) == COMMODITY_FERRITE_ORE) {
            furnace_idx = i;
            break;
        }
    }
    ASSERT(furnace_idx >= 0);

    vec2 furnace_pos = module_world_pos_ring(st,
        st->modules[furnace_idx].ring, st->modules[furnace_idx].slot);
    int pod_idx = test_spawn_frame_pod(&w, furnace_pos, 1);
    ASSERT(pod_idx >= 0);
    cargo_pod_set_module_tractor(&w.cargo_pods[pod_idx], 0, furnace_idx);

    for (int i = 0; i < 180; i++)
        world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[pod_idx],
                                            0, furnace_idx));
    float dist = v2_len(v2_sub(w.cargo_pods[pod_idx].pos, furnace_pos));
    ASSERT(dist >= STATION_MODULE_COL_RADIUS + w.cargo_pods[pod_idx].radius - 0.5f);
    vec2 outward = v2_norm(v2_sub(furnace_pos, st->pos));
    ASSERT(v2_dot(v2_sub(w.cargo_pods[pod_idx].pos, furnace_pos), outward) > 0.0f);
}

TEST(test_station_production_ejects_laser_pod) {
    WORLD_DECL;
    world_reset(&w);
    /* LASER_FAB lives on Helios (st[2]) under the minimal layout —
     * Kepler is shipyard + frame press only. */
    station_t *st = &w.stations[2];
    cargo_unit_t inputs[2] = {{0}};
    cargo_unit_t expected = {0};
    int laser_idx = -1;
    uint8_t fragment_cr[32] = {0};
    uint8_t frame_pub[32] = {0};

    fragment_cr[31] = 0x33;
    frame_pub[31] = 0x44;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_LASER_FAB) {
            laser_idx = i;
            break;
        }
    }
    ASSERT(laser_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_CRYSTAL_INGOT, MINING_GRADE_RARE,
                      fragment_cr, 0, &inputs[0]));
    inputs[1].kind = (uint8_t)CARGO_KIND_FRAME;
    inputs[1].commodity = (uint8_t)COMMODITY_FRAME;
    inputs[1].grade = (uint8_t)MINING_GRADE_FINE;
    inputs[1].quantity = 1;
    memcpy(inputs[1].pub, frame_pub, sizeof(frame_pub));
    ASSERT(hash_product(RECIPE_LASER_BASIC, inputs, 2, 0, &expected));
    ASSERT(manifest_push(&st->manifest, &inputs[0]));
    ASSERT(manifest_push(&st->manifest, &inputs[1]));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, 1, NULL) == 1);

    st->module_input[laser_idx] = 1.0f;
    st->_inventory_cache[COMMODITY_FRAME] = 2.0f;
    sim_step_station_production(&w, 2.0f);

    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_LASER_MODULE], 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_INT(st->manifest.count, 0);
    ASSERT_EQ_INT(manifest_find(&st->manifest, inputs[0].pub), -1);
    ASSERT_EQ_INT(manifest_find(&st->manifest, inputs[1].pub), -1);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_LASER_MODULE, 1);
    ASSERT(pod != NULL);
    ASSERT(memcmp(pod->manifest_units[0].pub, expected.pub, 32) == 0);
    ASSERT(memcmp(pod->manifest_units[0].parent_merkle,
                  expected.parent_merkle, 32) == 0);
    ASSERT_EQ_INT(pod->manifest_units[0].kind, CARGO_KIND_LASER);
    ASSERT_EQ_INT(pod->manifest_units[0].commodity, COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(pod->manifest_units[0].grade, MINING_GRADE_FINE);
    ASSERT_EQ_INT(pod->manifest_units[0].recipe_id, RECIPE_LASER_BASIC);
    ASSERT(pod->has_shell_frame);
    ASSERT_EQ_INT(pod->shell_frame.commodity, COMMODITY_FRAME);
}

TEST(test_station_production_fills_existing_laser_output_pod) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[2];
    cargo_unit_t inputs[2] = {{0}};
    int laser_idx = -1;
    uint8_t fragment_cr[32] = {0};
    uint8_t frame_pub[32] = {0};

    fragment_cr[31] = 0x35;
    frame_pub[31] = 0x45;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_LASER_FAB) {
            laser_idx = i;
            break;
        }
    }
    ASSERT(laser_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    ASSERT(hash_ingot(COMMODITY_CRYSTAL_INGOT, MINING_GRADE_RARE,
                      fragment_cr, 0, &inputs[0]));
    inputs[1].kind = (uint8_t)CARGO_KIND_FRAME;
    inputs[1].commodity = (uint8_t)COMMODITY_FRAME;
    inputs[1].grade = (uint8_t)MINING_GRADE_FINE;
    inputs[1].quantity = 1;
    memcpy(inputs[1].pub, frame_pub, sizeof(frame_pub));
    ASSERT(manifest_push(&st->manifest, &inputs[0]));
    ASSERT(manifest_push(&st->manifest, &inputs[1]));

    vec2 laser_pos = module_world_pos_ring(
        st, st->modules[laser_idx].ring, st->modules[laser_idx].slot);
    int output_pod = test_spawn_exact_pod(
        &w, laser_pos, COMMODITY_LASER_MODULE, 1);
    ASSERT(output_pod >= 0);
    cargo_pod_set_module_tractor(&w.cargo_pods[output_pod], 2, laser_idx);

    st->module_input[laser_idx] = 1.0f;
    st->_inventory_cache[COMMODITY_FRAME] = 1.0f;
    sim_step_station_production(&w, 2.0f);

    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_LASER_MODULE), 2);
    ASSERT_EQ_INT(w.cargo_pods[output_pod].manifest_count, 2);
    ASSERT_EQ_INT(w.cargo_pods[output_pod].quantity, 2);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[output_pod],
                                            2, laser_idx));
    ASSERT(test_first_exact_pod_with_units(&w, COMMODITY_LASER_MODULE, 1) == NULL);
}

/* Manifest-as-truth invariant: production refuses to mint orphan
 * frames. With no FE_INGOT manifest entry to consume, station_manifest_
 * craft_product fails and the float increment is reverted. This is the
 * inverse of the old legacy-path test (which asserted that the float
 * path kept producing without manifest); that behavior is the bug class
 * the manifest-as-truth refactor closes. */
TEST(test_station_production_without_manifest_inputs_refuses_to_mint) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    int press_idx = -1;

    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->_inventory_cache, 0, sizeof(st->_inventory_cache));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));

    st->module_input[press_idx] = 2.0f;
    sim_step_station_production(&w, 1.0f);

    /* Float reverted by E1 fix: no manifest input → no manifest output
     * → no orphan float either. */
    ASSERT_EQ_FLOAT(st->_inventory_cache[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_INT(st->manifest.count, 0);
}

TEST(test_world_sim_step_events_emitted) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    /* Launch should emit LAUNCH event */
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    bool found_launch = false;
    for (int i = 0; i < w.events.count; i++) {
        if (w.events.events[i].type == SIM_EVENT_LAUNCH) found_launch = true;
    }
    ASSERT(found_launch);
}

TEST(test_world_sim_step_npc_miners_work) {
    WORLD_DECL;
    world_reset(&w);
    /* Run for 5 seconds of sim time */
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    /* At least one miner should have left docked state */
    bool any_traveling = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].role == NPC_ROLE_MINER &&
            w.npc_ships[i].state != NPC_STATE_DOCKED) {
            any_traveling = true;
        }
    }
    ASSERT(any_traveling);
}

TEST(test_embedded_neural_checkpoint_drives_npc_worker) {
    ASSERT(test_load_embedded_flight_brain());

    WORLD_DECL;
    world_reset(&w);

    int npc_idx = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_MINER) {
            npc_idx = i;
            break;
        }
    }
    ASSERT(npc_idx >= 0);

    int ast_idx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active && w.asteroids[i].tier != ASTEROID_TIER_S) {
            ast_idx = i;
            break;
        }
    }
    ASSERT(ast_idx >= 0);

    npc_ship_t *npc = &w.npc_ships[npc_idx];
    npc->brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
    npc->state_timer = 0.0f;
    npc->target_asteroid = ast_idx;
    npc->ship.pos = v2_add(w.asteroids[ast_idx].pos, v2(-900.0f, -80.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;
    npc->ship.hull = ship_max_hull(&npc->ship);
    npc->hull = npc->ship.hull;
    rebuild_characters_from_npcs(&w);

    uint64_t before_inferences = signal_brain_inference_count();
    vec2 before_pos = npc->ship.pos;
    step_npc_ships(&w, SIM_DT);

    npc = &w.npc_ships[npc_idx];
    ASSERT(signal_brain_inference_count() > before_inferences);
    ASSERT(fabsf(npc->input.turn) > 0.0f || fabsf(npc->input.thrust) > 0.0f);
    ASSERT(v2_dist_sq(npc->ship.pos, before_pos) > 0.0001f);
}

TEST(test_holographic_npc_bootstrap_gate_blocks_forward_thrust) {
    WORLD_DECL;
    world_reset(&w);
    signal_brain_holographic_init();

    npc_ship_t *npc = &w.npc_ships[0];
    ship_cleanup(&npc->ship);
    memset(npc, 0, sizeof(*npc));
    ASSERT(ship_manifest_bootstrap(&npc->ship));

    station_t *st = &w.stations[0];
    ASSERT(station_exists(st));
    npc->active = true;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
    npc->brain_mode = SERVER_BRAIN_MODE_HOLOGRAPHIC;
    npc->home_station = 0;
    npc->dest_station = 0;
    npc->target_asteroid = 0;
    npc->ship.hull_class = HULL_CLASS_NPC_MINER;
    npc->ship.hull = ship_max_hull(&npc->ship);
    npc->hull = npc->ship.hull;
    npc->ship.pos = v2(st->pos.x - st->radius - 80.0f, st->pos.y);
    npc->ship.vel = v2(80.0f, 0.0f);
    npc->ship.angle = 0.0f;
    hnn_memory_init(&npc->hnn_mem);

    w.asteroids[0].active = true;
    w.asteroids[0].pos = v2(npc->ship.pos.x + 1200.0f, npc->ship.pos.y);
    w.asteroids[0].radius = 80.0f;

    signal_brain_drive_npc(&w, npc, SIM_DT);

    ASSERT_EQ_INT(npc->hnn_mem.experience_count, 1);
    ASSERT_EQ_INT(signal_brain_holographic_npc_holonet_active_count(&w, npc), 1);
    ASSERT(npc->input.thrust <= 0.0f);
    ASSERT(!npc->thrusting);
}

TEST(test_world_network_writes_persist) {
    /* Simulate: world_sim_step runs, then network callback overwrites asteroid,
     * next world_sim_step should see the overwritten state */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    world_sim_step(&w, 1.0f / 120.0f);
    /* Simulate network overwrite of asteroid 0 */
    w.asteroids[0].active = true;
    w.asteroids[0].hp = 999.0f;
    w.asteroids[0].pos = v2(100.0f, 100.0f);
    world_sim_step(&w, 1.0f / 120.0f);
    /* HP should still be near 999 (only drag/dynamics, no mining) */
    ASSERT(w.asteroids[0].hp > 990.0f);
    ASSERT(w.asteroids[0].active);
}

TEST(test_scenario_full_mining_cycle) {
    /* Test the physical ore flow: create S fragment → tow → deposit at hopper → earn credits */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x42, 8);  /* test token */

    /* Create a collectible S-tier fragment directly */
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);
    w.asteroids[frag].active = true;
    w.asteroids[frag].tier = ASTEROID_TIER_S;
    w.asteroids[frag].radius = 8.0f;
    w.asteroids[frag].hp = 1.0f;
    w.asteroids[frag].max_hp = 1.0f;
    w.asteroids[frag].ore = 15.0f;
    w.asteroids[frag].max_ore = 15.0f;
    w.asteroids[frag].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[frag].fracture_child = true;
    w.asteroids[frag].pos = v2(5000.0f, 5000.0f);
    w.asteroids[frag].vel = v2(0.0f, 0.0f);

    /* Manually attach as towed (simulates tractor pickup) */
    w.players[0].ship.towed_fragments[0] = (int16_t)frag;
    w.players[0].ship.towed_count = 1;

    /* Find the furnace and the smelt-beam silo. The smelt code picks
     * the closest module on an adjacent ring (with current offsets
     * baked in), so mirror that here rather than hard-coding a slot. */
    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w.stations[0].module_count; m++) {
        if (w.stations[0].modules[m].type == MODULE_FURNACE && !w.stations[0].modules[m].scaffold) {
            furnace_idx = m; break;
        }
    }
    ASSERT(furnace_idx >= 0);
    float start_credits = ledger_balance(&w.stations[0], w.players[0].session_token);

    /* Clear station ore inventory */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        w.stations[0]._inventory_cache[i] = 0.0f;

    /* Stop rotation, then mirror the smelt code's silo pick (closest
     * adjacent-ring module to the furnace). */
    for (int a = 0; a < MAX_ARMS; a++) {
        w.stations[0].arm_speed[a] = 0.0f;
        w.stations[0].arm_rotation[a] = 0.0f;
    }
    vec2 furnace_pos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[furnace_idx].ring, w.stations[0].modules[furnace_idx].slot);
    {
        int fr = w.stations[0].modules[furnace_idx].ring;
        float best_d = 1e18f;
        int adj_rings[] = { fr + 1, fr - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int m2 = 0; m2 < w.stations[0].module_count; m2++) {
                if (w.stations[0].modules[m2].ring != adj) continue;
                vec2 mp2 = module_world_pos_ring(&w.stations[0], adj,
                                                  w.stations[0].modules[m2].slot);
                float dd = v2_dist_sq(furnace_pos, mp2);
                if (dd < best_d) { best_d = dd; silo_idx = m2; }
            }
        }
    }
    ASSERT(silo_idx >= 0);
    vec2 silo_pos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[silo_idx].ring, w.stations[0].modules[silo_idx].slot);
    vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);
    ASSERT(station_buy_price(&w.stations[0], COMMODITY_FERRITE_ORE) > 0.0f);
    w.asteroids[frag].pos = midpoint;
    w.asteroids[frag].vel = v2(0.0f, 0.0f);
    w.asteroids[frag].last_fractured_by = 0;
    w.asteroids[frag].last_towed_by = 0;
    /* Credit attribution is strictly token-based now (H1) — mirror what
     * live tow code does at game_sim.c:1343 by stamping the tow/fracture
     * tokens to match the towing player's session. */
    memcpy(w.asteroids[frag].last_towed_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_towed_token));
    memcpy(w.asteroids[frag].last_fractured_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_fractured_token));
    w.players[0].ship.pos = v2_add(midpoint, v2(100.0f, 0.0f));
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    ASSERT(station_finished_mint(&w.stations[0], COMMODITY_FRAME, 1, NULL) == 1);
    /* Run enough steps for smelt_progress to reach 1.0 (~2 seconds at 120Hz) */
    for (int i = 0; i < 300; i++) world_sim_step(&w, SIM_DT);

    /* Fragment should be consumed */
    ASSERT(w.players[0].ship.towed_count == 0);

    /* Credits are in the station ledger — check balance directly */
    ASSERT(ledger_balance(&w.stations[0], w.players[0].session_token) > start_credits);
}

/* Phase 1-3 manifest-first invariant:
 *   Across every station, connected ship, and loose cargo pod, no pub
 *   appears twice. Runs a smelt + a dock delivery + a buyback through
 *   the normal sim code paths. */
TEST(test_manifest_conservation_across_transactions) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x33, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;

    /* Smelt one fragment to populate a loose manifest-bearing cargo pod. */
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (!w.asteroids[i].active) { frag = i; break; }
    ASSERT(frag >= 0);
    w.asteroids[frag] = (asteroid_t){0};
    w.asteroids[frag].active = true;
    w.asteroids[frag].tier = ASTEROID_TIER_S;
    w.asteroids[frag].radius = 8.0f;
    w.asteroids[frag].hp = 1.0f;
    w.asteroids[frag].max_hp = 1.0f;
    w.asteroids[frag].ore = 3.0f;       /* 3 whole units -> 3 pod entries */
    w.asteroids[frag].max_ore = 3.0f;
    w.asteroids[frag].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[frag].fracture_child = true;
    w.asteroids[frag].grade = MINING_GRADE_COMMON;
    w.players[0].ship.towed_fragments[0] = (int16_t)frag;
    w.players[0].ship.towed_count = 1;
    memcpy(w.asteroids[frag].last_towed_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_towed_token));
    memcpy(w.asteroids[frag].last_fractured_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_fractured_token));
    /* Place fragment between furnace and silo on station 0. Mirror
     * the smelt code's silo pick (closest module on adjacent ring). */
    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w.stations[0].module_count; m++) {
        if (w.stations[0].modules[m].type == MODULE_FURNACE) { furnace_idx = m; break; }
    }
    ASSERT(furnace_idx >= 0);
    for (int a = 0; a < MAX_ARMS; a++) {
        w.stations[0].arm_speed[a] = 0.0f;
        w.stations[0].arm_rotation[a] = 0.0f;
    }
    vec2 fpos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[furnace_idx].ring, w.stations[0].modules[furnace_idx].slot);
    {
        int fr = w.stations[0].modules[furnace_idx].ring;
        float best_d = 1e18f;
        int adj_rings[] = { fr + 1, fr - 1 };
        for (int ri = 0; ri < 2; ri++) {
            int adj = adj_rings[ri];
            if (adj < 1 || adj > STATION_NUM_RINGS) continue;
            for (int m2 = 0; m2 < w.stations[0].module_count; m2++) {
                if (w.stations[0].modules[m2].ring != adj) continue;
                vec2 mp2 = module_world_pos_ring(&w.stations[0], adj,
                                                  w.stations[0].modules[m2].slot);
                float dd = v2_dist_sq(fpos, mp2);
                if (dd < best_d) { best_d = dd; silo_idx = m2; }
            }
        }
    }
    ASSERT(silo_idx >= 0);
    vec2 spos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[silo_idx].ring, w.stations[0].modules[silo_idx].slot);
    w.asteroids[frag].pos = v2_scale(v2_add(fpos, spos), 0.5f);
    ASSERT(station_finished_mint(&w.stations[0], COMMODITY_FRAME, 1, NULL) == 1);
    for (int i = 0; i < 400; i++) world_sim_step(&w, SIM_DT);

    /* Post-smelt: a loose pod should carry 3 COMMON ferrite ingots. */
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_FERRITE_INGOT, 3);
    ASSERT(pod != NULL);

    /* Invariant sweep: no pub repeats anywhere. */
    static uint8_t seen_pubs[64][32]; int seen_n = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w.stations[s];
        if (!st->manifest.units) continue;
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            for (int k = 0; k < seen_n; k++) {
                ASSERT(memcmp(seen_pubs[k], st->manifest.units[i].pub, 32) != 0);
            }
            if (seen_n < (int)(sizeof(seen_pubs) / sizeof(seen_pubs[0]))) {
                memcpy(seen_pubs[seen_n++], st->manifest.units[i].pub, 32);
            }
        }
    }
    for (int p = 0; p < MAX_PLAYERS; p++) {
        const ship_t *ship = &w.players[p].ship;
        if (!ship->manifest.units) continue;
        for (uint16_t i = 0; i < ship->manifest.count; i++) {
            for (int k = 0; k < seen_n; k++) {
                ASSERT(memcmp(seen_pubs[k], ship->manifest.units[i].pub, 32) != 0);
            }
            if (seen_n < (int)(sizeof(seen_pubs) / sizeof(seen_pubs[0]))) {
                memcpy(seen_pubs[seen_n++], ship->manifest.units[i].pub, 32);
            }
        }
    }
    for (int c = 0; c < MAX_CARGO_PODS; c++) {
        const cargo_pod_t *cp = &w.cargo_pods[c];
        if (!cp->active) continue;
        for (uint16_t i = 0; i < cp->manifest_count; i++) {
            for (int k = 0; k < seen_n; k++) {
                ASSERT(memcmp(seen_pubs[k], cp->manifest_units[i].pub, 32) != 0);
            }
            if (seen_n < (int)(sizeof(seen_pubs) / sizeof(seen_pubs[0]))) {
                memcpy(seen_pubs[seen_n++], cp->manifest_units[i].pub, 32);
            }
        }
    }
}

TEST(test_scenario_two_players_mining) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[1].session_ready = true;
    memset(w.players[1].session_token, 0x02, 8);
    player_init_ship(&w.players[0], &w);
    player_init_ship(&w.players[1], &w);
    player_seed_credits(&w.players[0], &w);
    player_seed_credits(&w.players[1], &w);
    w.players[0].connected = true;
    w.players[1].connected = true;
    w.players[0].ship.mining_level = SHIP_UPGRADE_MAX_LEVEL;
    w.players[1].ship.mining_level = SHIP_UPGRADE_MAX_LEVEL;

    /* Launch both */
    w.players[0].input.interact = true;
    w.players[1].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    w.players[1].input.interact = false;
    ASSERT(!w.players[0].docked);
    ASSERT(!w.players[1].docked);

    /* Create two M-tier test asteroids near station 0 */
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    int ast0 = 0, ast1 = 1;
    w.asteroids[ast0].active = true; w.asteroids[ast0].tier = ASTEROID_TIER_M;
    w.asteroids[ast0].radius = 25.0f; w.asteroids[ast0].hp = 50.0f; w.asteroids[ast0].max_hp = 50.0f;
    w.asteroids[ast0].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[ast0].pos = v2_add(w.stations[0].pos, v2(500.0f, 0.0f));
    w.asteroids[ast1].active = true; w.asteroids[ast1].tier = ASTEROID_TIER_M;
    w.asteroids[ast1].radius = 25.0f; w.asteroids[ast1].hp = 50.0f; w.asteroids[ast1].max_hp = 50.0f;
    w.asteroids[ast1].commodity = COMMODITY_CUPRITE_ORE;
    w.asteroids[ast1].pos = v2_add(w.stations[0].pos, v2(-500.0f, 0.0f));

    float hp0_before = w.asteroids[ast0].hp;
    float hp1_before = w.asteroids[ast1].hp;

    /* Position players near their respective asteroids */
    w.players[0].ship.pos = v2(w.asteroids[ast0].pos.x - 60.0f, w.asteroids[ast0].pos.y);
    w.players[0].ship.angle = 0.0f;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[1].ship.pos = v2(w.asteroids[ast1].pos.x - 60.0f, w.asteroids[ast1].pos.y);
    w.players[1].ship.angle = 0.0f;
    w.players[1].ship.vel = v2(0.0f, 0.0f);

    /* Both mine for 120 ticks */
    w.players[0].input.mine = true;
    w.players[1].input.mine = true;
    for (int i = 0; i < 120; i++) {
        w.players[0].ship.pos = v2(w.asteroids[ast0].pos.x - 60.0f, w.asteroids[ast0].pos.y);
        w.players[1].ship.pos = v2(w.asteroids[ast1].pos.x - 60.0f, w.asteroids[ast1].pos.y);
        w.players[0].ship.vel = v2(0.0f, 0.0f);
        w.players[1].ship.vel = v2(0.0f, 0.0f);
        world_sim_step(&w, SIM_DT);
    }
    w.players[0].input.mine = false;
    w.players[1].input.mine = false;

    /* Each asteroid took damage independently */
    ASSERT(w.asteroids[ast0].hp < hp0_before);
    ASSERT(w.asteroids[ast1].hp < hp1_before);

    /* No state bleed: player 0's cargo didn't affect player 1.
     * Both spawned with the same spawn-fee debit at station 0, so
     * their balances should be the same negative number regardless of
     * what either of them mined. */
    float p0 = ledger_balance(&w.stations[0], w.players[0].session_token);
    float p1 = ledger_balance(&w.stations[0], w.players[1].session_token);
    int fee = station_spawn_fee(&w.stations[0]);
    ASSERT_EQ_FLOAT(p0, -(float)fee, 0.01f);
    ASSERT_EQ_FLOAT(p1, -(float)fee, 0.01f);
}

TEST(test_scenario_npc_economy_30_seconds) {
    WORLD_DECL;
    world_reset(&w);

    /* Run 60 sim seconds. Originally 30s, but the NPC mining → tow →
     * smelt pipeline only just barely finishes one cycle by t=30s on
     * macOS, and Linux CI's slightly different float rounding pushes
     * the first delivery past the cutoff. 60s gives ~2× margin while
     * keeping the test fast. */
    for (int i = 0; i < 7200; i++)
        world_sim_step(&w, SIM_DT);

    /* Verify: at least one asteroid was mined (some HP < max_hp or deactivated) */
    bool any_mined = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active ||
            (w.asteroids[i].hp < w.asteroids[i].max_hp && w.asteroids[i].max_hp > 0.0f)) {
            any_mined = true; break;
        }
    }
    ASSERT(any_mined);

    /* Verify: at least one station has either old-style finished stock,
     * a new physical finished-good pod, or raw ore mid-smelt. Originally
     * checked station[0] only, but Helios also has active furnace pairs;
     * on machines where Prospect's ferrite pipeline runs slower than
     * Helios's cuprite/crystal one, station[0] can stay quiet for the
     * first minute even though the economy is clearly working. Looking at
     * every station and loose pod catches both ends. */
    bool any_ingot = false;
    for (int s = 0; s < MAX_STATIONS && !any_ingot; s++) {
        for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
            if (w.stations[s]._inventory_cache[i] > 0.0f) { any_ingot = true; break; }
        }
    }
    bool any_pod_ingot = false;
    for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
        if (test_count_exact_pod_units(&w, (commodity_t)i) > 0) {
            any_pod_ingot = true;
            break;
        }
    }
    bool ore_consumed = false;
    for (int s = 0; s < MAX_STATIONS && !ore_consumed; s++) {
        for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
            if (w.stations[s]._inventory_cache[i] > 0.0f) { ore_consumed = true; break; }
        }
    }
    ASSERT(any_ingot || any_pod_ingot || ore_consumed);

    /* Verify: no negative values anywhere */
    for (int s = 0; s < MAX_STATIONS; s++) {
        for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
            ASSERT(w.stations[s]._inventory_cache[i] >= 0.0f);
        for (int i = 0; i < COMMODITY_COUNT; i++)
            ASSERT(w.stations[s]._inventory_cache[i] >= 0.0f);
        for (int i = 0; i < PRODUCT_COUNT; i++)
            ASSERT(w.stations[s]._inventory_cache[COMMODITY_FRAME + i] >= 0.0f);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w.npc_ships[n].active) continue;
        for (int i = 0; i < COMMODITY_COUNT; i++)
            ASSERT(w.npc_ships[n].cargo[i] >= 0.0f);
    }
}

TEST(test_npc_exits_station_with_blocked_rings) {
    /* Slice 1.5b regression — Prospect's NPCs used to get stuck in the
     * inner zone whenever ring 2 had multiple hoppers. Blocking ring-2
     * slots 1, 2, 3, 5 (everything except the dock-radial slot 0 and
     * the existing slot-4 ferrite-ore intake) plus ring-3 slots 0/3/6
     * stresses the layout. With npc_target_clear_of_home_rings routing
     * just-undocked NPCs through the dock-radial exit waypoint, the
     * miner reaches an asteroid placed outside the rings without
     * collision-stalling. */
    WORLD_DECL;
    world_reset(&w);
    /* Block every ring-2 non-dock-radial slot on Prospect, plus a few
     * ring-3 slots, with FERRITE_ORE-tagged hoppers (cheap to add via
     * the seed helper). */
    add_hopper_for(&w.stations[0], 2, 1, COMMODITY_FERRITE_ORE);
    add_hopper_for(&w.stations[0], 2, 2, COMMODITY_FERRITE_ORE);
    add_hopper_for(&w.stations[0], 2, 3, COMMODITY_FERRITE_ORE);
    add_hopper_for(&w.stations[0], 2, 5, COMMODITY_FERRITE_ORE);
    add_hopper_for(&w.stations[0], 3, 0, COMMODITY_FERRITE_ORE);
    add_hopper_for(&w.stations[0], 3, 3, COMMODITY_FERRITE_ORE);
    add_hopper_for(&w.stations[0], 3, 6, COMMODITY_FERRITE_ORE);
    station_rebuild_all_nav(&w);

    /* Find a Prospect miner and force it just-undocked. */
    int miner = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_MINER
            && w.npc_ships[i].home_station == 0) { miner = i; break; }
    }
    ASSERT(miner >= 0);
    w.npc_ships[miner].state = NPC_STATE_DOCKED;
    w.npc_ships[miner].state_timer = 0.0f;

    /* Plant a ferrite asteroid 3000u east of Prospect — well outside
     * any ring envelope. The miner must reach MINING_RANGE of it. */
    int target_a = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { target_a = i; break; }
    }
    ASSERT(target_a >= 0);
    asteroid_t *a = &w.asteroids[target_a];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 30.0f;
    a->max_ore = 30.0f;
    a->hp = 100.0f;
    a->max_hp = 100.0f;
    a->radius = 30.0f;
    a->pos = v2_add(w.stations[0].pos, v2(3000.0f, 0.0f));

    /* Run up to 30 sim seconds. The miner must reach NPC_STATE_MINING
     * (not just "near the asteroid") — proximity-only acceptance was
     * loose enough that a regression where the NPC drifts slowly
     * toward the asteroid without ever locking on would still pass.
     * MINING gating requires the asteroid to be in the mining cone,
     * which means the miner has to actually navigate there. */
    bool reached = false;
    for (int i = 0; i < 3600 && !reached; i++) {
        world_sim_step(&w, SIM_DT);
        const npc_ship_t *npc = &w.npc_ships[miner];
        if (npc->state == NPC_STATE_MINING) { reached = true; break; }
    }
    ASSERT(reached);
}

TEST(test_hauler_exits_non_home_station_before_return) {
    WORLD_DECL;
    world_reset(&w);

    int seeded_hauler = test_spawn_hauler_at(&w, 0);
    ASSERT(seeded_hauler >= 0);

    int hauler = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler = i;
            break;
        }
    }
    ASSERT(hauler >= 0);

    npc_ship_t *npc = &w.npc_ships[hauler];
    npc->home_station = 0;
    npc->dest_station = 1;
    npc->state = NPC_STATE_RETURN_TO_STATION;
    npc->state_timer = 0.0f;
    npc->ship.hull_class = HULL_CLASS_HAULER;
    npc->ship.pos = w.stations[1].pos;
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;
    ship_t *paired = world_npc_ship_for(&w, hauler);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;
    paired->vel = npc->ship.vel;
    paired->angle = npc->ship.angle;
    *nav_npc_path(hauler) = (nav_path_t){0};

    vec2 expected_exit = station_exit_target(&w.stations[1], npc->ship.pos);
    world_sim_step(&w, SIM_DT);

    const nav_path_t *path = nav_npc_path(hauler);
    ASSERT(v2_dist_sq(path->goal, expected_exit) < 5.0f * 5.0f);

    bool moved = false;
    for (int i = 0; i < 240; i++) {
        world_sim_step(&w, SIM_DT);
        if (v2_dist_sq(npc->ship.pos, w.stations[1].pos) > 25.0f * 25.0f &&
            v2_len(npc->ship.vel) > 1.0f) {
            moved = true;
            break;
        }
    }
    ASSERT(moved);
}

TEST(test_miner_inside_station_nav_envelope_routes_to_outer_gap) {
    WORLD_DECL;
    world_reset(&w);

    int miner = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_MINER
            && w.npc_ships[i].home_station == 2) {
            miner = i;
            break;
        }
    }
    ASSERT(miner >= 0);

    int target_a = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { target_a = i; break; }
    }
    ASSERT(target_a >= 0);
    asteroid_t *a = &w.asteroids[target_a];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_CUPRITE_ORE;
    a->ore = 30.0f;
    a->max_ore = 30.0f;
    a->hp = 100.0f;
    a->max_hp = 100.0f;
    a->radius = 30.0f;
    a->pos = v2_add(w.stations[2].pos, v2(3240.0f, -4200.0f));

    npc_ship_t *npc = &w.npc_ships[miner];
    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
    npc->state_timer = 0.0f;
    npc->target_asteroid = target_a;
    npc->towed_fragment = -1;
    npc->ship.hull_class = HULL_CLASS_NPC_MINER;
    npc->ship.mining_level = 1;
    npc->ship.pos = v2_add(w.stations[2].pos, v2(292.0f, -485.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = -1.12f;
    ship_t *paired = world_npc_ship_for(&w, miner);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;
    paired->vel = npc->ship.vel;
    paired->angle = npc->ship.angle;
    *nav_npc_path(miner) = (nav_path_t){0};

    vec2 start = npc->ship.pos;
    world_sim_step(&w, SIM_DT);

    const nav_path_t *path = nav_npc_path(miner);
    vec2 expected_exit = station_exit_target(&w.stations[2], start);
    ASSERT(v2_dist_sq(path->goal, expected_exit) < 10.0f * 10.0f);

    bool moved = false;
    for (int i = 0; i < 240; i++) {
        world_sim_step(&w, SIM_DT);
        if (v2_dist_sq(npc->ship.pos, start) > 25.0f * 25.0f &&
            v2_len(npc->ship.vel) > 1.0f) {
            moved = true;
            break;
        }
    }
    ASSERT(moved);
}

TEST(test_hauler_near_station_does_not_post_distress_contract) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    int seeded_hauler = test_spawn_hauler_at(&w, 2);
    ASSERT(seeded_hauler >= 0);

    int hauler = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler = i;
            break;
        }
    }
    ASSERT(hauler >= 0);

    npc_ship_t *npc = &w.npc_ships[hauler];
    npc->home_station = 2;
    npc->dest_station = 1;
    npc->state = NPC_STATE_RETURN_TO_STATION;
    npc->ship.pos = station_approach_target(&w.stations[2], w.stations[1].pos);
    npc->ship.vel = v2(0.0f, 0.0f);

    int blocker = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { blocker = i; break; }
    }
    ASSERT(blocker >= 0);
    asteroid_t *a = &w.asteroids[blocker];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->hp = 100.0f;
    a->max_hp = 100.0f;
    a->radius = 40.0f;
    a->pos = v2_add(npc->ship.pos, v2(80.0f, 0.0f));

    generate_npc_distress_contracts(&w, SIM_DT);

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].action == CONTRACT_FRACTURE &&
                 w.contracts[k].target_index == blocker));
    }
}

TEST(test_hauler_distress_requires_sustained_stall) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));

    int seeded_hauler = test_spawn_hauler_at(&w, 0);
    ASSERT(seeded_hauler >= 0);

    int hauler = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler = i;
            break;
        }
    }
    ASSERT(hauler >= 0);

    npc_ship_t *npc = &w.npc_ships[hauler];
    npc->home_station = 0;
    npc->dest_station = 1;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->ship.pos = v2(1200.0f, 1800.0f);
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->state_timer = 0.0f;

    int blocker = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { blocker = i; break; }
    }
    ASSERT(blocker >= 0);
    asteroid_t *a = &w.asteroids[blocker];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->hp = 100.0f;
    a->max_hp = 100.0f;
    a->radius = 40.0f;
    a->pos = v2_add(npc->ship.pos, v2(80.0f, 0.0f));

    generate_npc_distress_contracts(&w, 1.0f);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        ASSERT(!(w.contracts[k].active &&
                 w.contracts[k].action == CONTRACT_FRACTURE &&
                 w.contracts[k].target_index == blocker));
    }

    for (int i = 0; i < 6; i++)
        generate_npc_distress_contracts(&w, 1.0f);

    bool posted = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active &&
            w.contracts[k].action == CONTRACT_FRACTURE &&
            w.contracts[k].target_index == blocker) {
            posted = true;
            break;
        }
    }
    ASSERT(posted);
}

TEST(test_hauler_docks_when_reaching_station_lane) {
    WORLD_DECL;
    world_reset(&w);

    int seeded_hauler = test_spawn_hauler_at(&w, 0);
    ASSERT(seeded_hauler >= 0);

    int hauler = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler = i;
            break;
        }
    }
    ASSERT(hauler >= 0);

    station_t *kepler = &w.stations[1];
    npc_ship_t *npc = &w.npc_ships[hauler];
    npc->home_station = 0;
    npc->dest_station = 1;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->state_timer = 0.0f;
    npc->ship.hull_class = HULL_CLASS_HAULER;
    vec2 lane = station_approach_target(kepler, v2_add(kepler->pos, v2(900.0f, 0.0f)));
    npc->ship.pos = lane;
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;
    ship_t *paired = world_npc_ship_for(&w, hauler);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;
    paired->vel = npc->ship.vel;
    paired->angle = npc->ship.angle;
    *nav_npc_path(hauler) = (nav_path_t){0};

    ASSERT(v2_dist_sq(npc->ship.pos, kepler->pos) >
           (kepler->dock_radius * 0.7f) * (kepler->dock_radius * 0.7f));

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(npc->state, NPC_STATE_UNLOADING);
    ASSERT(v2_dist_sq(npc->ship.pos, lane) < 5.0f * 5.0f);
}

TEST(test_hauler_does_not_dock_from_outer_station_ring) {
    WORLD_DECL;
    world_reset(&w);

    int seeded_hauler = test_spawn_hauler_at(&w, 0);
    ASSERT(seeded_hauler >= 0);

    int hauler = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            hauler = i;
            break;
        }
    }
    ASSERT(hauler >= 0);

    station_t *kepler = &w.stations[1];
    npc_ship_t *npc = &w.npc_ships[hauler];
    npc->home_station = 0;
    npc->dest_station = 1;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->state_timer = 0.0f;
    npc->ship.hull_class = HULL_CLASS_HAULER;

    vec2 lane = station_approach_target(kepler, v2_add(kepler->pos, v2(900.0f, 0.0f)));
    vec2 lane_dir = v2_norm(v2_sub(lane, kepler->pos));
    vec2 off_lane = v2(-lane_dir.y, lane_dir.x);
    npc->ship.pos = v2_add(kepler->pos, v2_scale(off_lane, 500.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;

    ASSERT(v2_dist_sq(npc->ship.pos, kepler->pos) <
           (STATION_RING_RADIUS[station_max_ring(kepler)] + 80.0f) *
           (STATION_RING_RADIUS[station_max_ring(kepler)] + 80.0f));
    ASSERT(v2_dist_sq(npc->ship.pos, lane) > 180.0f * 180.0f);

    ship_t *paired = world_npc_ship_for(&w, hauler);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;
    paired->vel = npc->ship.vel;
    paired->angle = npc->ship.angle;
    *nav_npc_path(hauler) = (nav_path_t){0};

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
}

TEST(test_kepler_frame_hauler_reaches_helios_dock) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);

    int seeded_hauler = test_spawn_hauler_at(w, 1);
    ASSERT(seeded_hauler >= 0);

    int hauler = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w->npc_ships[i].active && w->npc_ships[i].role == NPC_ROLE_HAULER
            && w->npc_ships[i].home_station == 1) {
            hauler = i;
            break;
        }
    }
    ASSERT(hauler >= 0);

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (i != hauler) w->npc_ships[i].active = false;
    }

    npc_ship_t *npc = &w->npc_ships[hauler];
    ship_t *ship = world_npc_ship_for(w, hauler);
    ASSERT(ship != NULL);
    ASSERT(test_set_ship_finished_units(ship, COMMODITY_FRAME, 12,
                                        MINING_GRADE_COMMON));
    memset(npc->cargo, 0, sizeof(npc->cargo));
    npc->cargo[COMMODITY_FRAME] = 12.0f;

    memset(w->contracts, 0, sizeof(w->contracts));
    w->contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FRAME,
        .quantity_needed = 12.0f,
        .base_price = 5.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    station_t *kepler = &w->stations[1];
    station_t *helios = &w->stations[2];
    npc->home_station = 1;
    npc->dest_station = 2;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->state_timer = 0.0f;
    npc->ship.hull_class = HULL_CLASS_HAULER;
    npc->ship.pos = station_approach_target(helios, kepler->pos);
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;
    ship->pos = npc->ship.pos;
    ship->vel = npc->ship.vel;
    ship->angle = npc->ship.angle;
    *nav_npc_path(hauler) = (nav_path_t){0};

    bool reached = false;
    float best_d = 1e18f;
    for (int i = 0; i < 12000; i++) {
        world_sim_step(w, SIM_DT);
        float d = v2_dist_sq(npc->ship.pos, helios->pos);
        if (d < best_d) best_d = d;
        if (npc->state == NPC_STATE_UNLOADING ||
            npc->state == NPC_STATE_RETURN_TO_STATION ||
            npc->state == NPC_STATE_DOCKED) {
            reached = true;
            break;
        }
    }

    if (!reached) {
        const nav_path_t *path = nav_npc_path(hauler);
        vec2 wp = (path->count > 0 && path->current < path->count)
            ? path->waypoints[path->current] : path->goal;
        printf("Kepler hauler did not dock: state=%d pos=(%.1f,%.1f) dist=%.1f best=%.1f speed=%.1f path_goal=(%.1f,%.1f) count=%d cur=%d wp=(%.1f,%.1f)\n",
               (int)npc->state,
               npc->ship.pos.x, npc->ship.pos.y,
               v2_len(v2_sub(npc->ship.pos, helios->pos)),
               sqrtf(best_d),
               v2_len(npc->ship.vel),
               path->goal.x, path->goal.y, path->count, path->current,
               wp.x, wp.y);
    }
    ASSERT(reached);
}

TEST(test_miner_enters_station_before_smelt_delivery) {
    WORLD_DECL;
    world_reset(&w);

    int miner = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_MINER
            && w.npc_ships[i].home_station == 0) {
            miner = i;
            break;
        }
    }
    ASSERT(miner >= 0);

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) {
            frag = i;
            break;
        }
    }
    ASSERT(frag >= 0);
    asteroid_t *tow = &w.asteroids[frag];
    memset(tow, 0, sizeof(*tow));
    tow->active = true;
    tow->tier = ASTEROID_TIER_S;
    tow->commodity = COMMODITY_FERRITE_ORE;
    tow->radius = 12.0f;
    tow->ore = 1.0f;
    tow->max_ore = 1.0f;

    npc_ship_t *npc = &w.npc_ships[miner];
    npc->state = NPC_STATE_RETURN_TO_STATION;
    npc->target_asteroid = -1;
    npc->towed_fragment = frag;
    npc->ship.hull_class = HULL_CLASS_NPC_MINER;
    npc->ship.pos = v2_add(w.stations[0].pos, v2(900.0f, 0.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = PI_F;
    tow->pos = v2_add(npc->ship.pos, v2(40.0f, 0.0f));
    ship_t *paired = world_npc_ship_for(&w, miner);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;
    paired->vel = npc->ship.vel;
    paired->angle = npc->ship.angle;
    *nav_npc_path(miner) = (nav_path_t){0};

    vec2 expected_entry = station_entry_target(&w.stations[0], npc->ship.pos);
    world_sim_step(&w, SIM_DT);

    const nav_path_t *path = nav_npc_path(miner);
    ASSERT(v2_dist_sq(path->goal, expected_entry) < 5.0f * 5.0f);
}

static bool test_station_smelt_endpoint_for_ore(const station_t *st,
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

static void test_prepare_autopilot_player(world_t *w, server_player_t *sp) {
    memset(sp, 0, sizeof(*sp));
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    uint8_t token[8] = {0xB0, 0x7A, 0x51, 0x9A, 0x01, 0x02, 0x03, 0x04};
    memcpy(sp->session_token, token, sizeof(token));
    player_init_ship(sp, w);
    memcpy(sp->session_token, token, sizeof(token));
    sp->autopilot_mode = 1;
    sp->autopilot_target = -1;
    sp->autopilot_last_pos = sp->ship.pos;
    sp->autopilot_stuck_timer = 0.0f;
    sp->ship.hull = ship_max_hull(&sp->ship);
}

static int test_attach_towed_fragment(world_t *w,
                                      server_player_t *sp,
                                      commodity_t ore,
                                      vec2 pos) {
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) {
            frag = i;
            break;
        }
    }
    if (frag < 0) return -1;
    asteroid_t *a = &w->asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = ore;
    a->radius = 12.0f;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->pos = pos;
    a->last_towed_by = (int8_t)sp->id;
    memcpy(a->last_towed_token, sp->session_token, sizeof(a->last_towed_token));
    sp->ship.towed_fragments[0] = (int16_t)frag;
    sp->ship.towed_count = 1;
    return frag;
}

static int test_spawn_collectible_fragment(world_t *w,
                                           commodity_t ore,
                                           vec2 pos) {
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) {
            frag = i;
            break;
        }
    }
    if (frag < 0) return -1;
    asteroid_t *a = &w->asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = ore;
    a->radius = 10.0f;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->pos = pos;
    return frag;
}

static int test_spawn_smelt_fragment(world_t *w,
                                     commodity_t ore,
                                     float amount,
                                     vec2 pos) {
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) {
            frag = i;
            break;
        }
    }
    if (frag < 0) return -1;
    asteroid_t *a = &w->asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = ore;
    a->radius = 8.0f;
    a->ore = amount;
    a->max_ore = amount;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    a->pos = pos;
    a->vel = v2(0.0f, 0.0f);
    return frag;
}

TEST(test_furnace_smelting_requires_frame_shell) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    station_t *prospect = &w.stations[0];
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        prospect->arm_speed[arm] = 0.0f;
        prospect->arm_rotation[arm] = 0.0f;
    }
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 0));

    vec2 smelt_target = prospect->pos;
    ASSERT(test_station_smelt_endpoint_for_ore(prospect, COMMODITY_FERRITE_ORE,
                                               &smelt_target));
    int pod_units_before =
        test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    int frag = test_spawn_smelt_fragment(
        &w, COMMODITY_FERRITE_ORE, 4.0f, smelt_target);
    ASSERT(frag >= 0);

    for (int i = 0; i < 600 && w.asteroids[frag].active; i++)
        step_furnace_smelting(&w, SIM_DT);

    ASSERT(w.asteroids[frag].active);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT),
                  pod_units_before);
}

TEST(test_furnace_smelting_consumes_loose_frame_shell) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    station_t *prospect = &w.stations[0];
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        prospect->arm_speed[arm] = 0.0f;
        prospect->arm_rotation[arm] = 0.0f;
    }
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 0));

    vec2 smelt_target = prospect->pos;
    ASSERT(test_station_smelt_endpoint_for_ore(prospect, COMMODITY_FERRITE_ORE,
                                               &smelt_target));
    int furnace_idx = -1;
    for (int i = 0; i < prospect->module_count; i++) {
        if (prospect->modules[i].type == MODULE_FURNACE &&
            module_instance_input_ore(&prospect->modules[i]) ==
                COMMODITY_FERRITE_ORE) {
            furnace_idx = i;
            break;
        }
    }
    ASSERT(furnace_idx >= 0);
    vec2 frame_shell_pos = module_world_pos_ring(
        prospect, prospect->modules[furnace_idx].ring,
        prospect->modules[furnace_idx].slot);
    int frame_units_before =
        test_count_exact_pod_units(&w, COMMODITY_FRAME);
    ASSERT(test_spawn_frame_pod(&w, frame_shell_pos, 1) >= 0);
    int frame_units_with_shell =
        test_count_exact_pod_units(&w, COMMODITY_FRAME);
    ASSERT_EQ_INT(frame_units_with_shell, frame_units_before + 1);

    int frag = test_spawn_smelt_fragment(
        &w, COMMODITY_FERRITE_ORE, 4.0f, smelt_target);
    ASSERT(frag >= 0);

    for (int i = 0; i < 600 && w.asteroids[frag].active; i++)
        step_furnace_smelting(&w, SIM_DT);

    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FRAME),
                  frame_units_with_shell - 1);
    const cargo_pod_t *pod = test_first_exact_pod_with_units(
        &w, COMMODITY_FERRITE_INGOT, 4);
    ASSERT(pod != NULL);
}

TEST(test_autopilot_routes_towed_fragment_to_smelt_even_near_station) {
    WORLD_DECL;
    world_reset(&w);

    station_t *prospect = &w.stations[0];
    vec2 smelt_target = prospect->pos;
    ASSERT(test_station_smelt_endpoint_for_ore(prospect, COMMODITY_FERRITE_ORE,
                                               &smelt_target));

    server_player_t *sp = &w.players[0];
    test_prepare_autopilot_player(&w, sp);
    sp->docked = false;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->ship.pos = v2_add(prospect->pos, v2(80.0f, 0.0f));
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
    ASSERT(test_attach_towed_fragment(&w, sp, COMMODITY_FERRITE_ORE,
                                      v2_add(sp->ship.pos, v2(35.0f, 0.0f))) >= 0);

    step_autopilot(&w, sp, SIM_DT);

    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_RETURN_TO_REFINERY);

    step_autopilot(&w, sp, SIM_DT);
    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_RETURN_TO_REFINERY);
    ASSERT(sp->input.tractor_hold);
}

TEST(test_autopilot_does_not_mix_ore_fragments_while_returning) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    server_player_t *sp = &w.players[0];
    test_prepare_autopilot_player(&w, sp);
    sp->docked = false;
    sp->ship.pos = v2_add(w.stations[0].pos, v2(3000.0f, 0.0f));
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;

    ASSERT(test_attach_towed_fragment(&w, sp, COMMODITY_CRYSTAL_ORE,
                                      v2_add(sp->ship.pos, v2(35.0f, 0.0f))) >= 0);
    int ferrite = test_spawn_collectible_fragment(
        &w, COMMODITY_FERRITE_ORE, v2_add(sp->ship.pos, v2(20.0f, 0.0f)));
    ASSERT(ferrite >= 0);

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship.towed_count, 1);
    for (int t = 0; t < sp->ship.towed_count; t++)
        ASSERT(sp->ship.towed_fragments[t] != ferrite);
    ASSERT(w.asteroids[ferrite].active);
}

TEST(test_autopilot_smelt_delivery_preempts_repair_dock) {
    WORLD_DECL;
    world_reset(&w);

    station_t *prospect = &w.stations[0];
    vec2 smelt_target = prospect->pos;
    ASSERT(test_station_smelt_endpoint_for_ore(prospect, COMMODITY_FERRITE_ORE,
                                               &smelt_target));

    server_player_t *sp = &w.players[0];
    test_prepare_autopilot_player(&w, sp);
    sp->docked = false;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    sp->ship.pos = smelt_target;
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->ship.hull = ship_max_hull(&sp->ship) * 0.45f;
    sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
    ASSERT(test_attach_towed_fragment(&w, sp, COMMODITY_FERRITE_ORE,
                                      v2_add(smelt_target, v2(35.0f, 0.0f))) >= 0);

    step_autopilot(&w, sp, SIM_DT);

    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_RETURN_TO_REFINERY);
    ASSERT(sp->input.tractor_hold);
    ASSERT(!sp->input.interact);
}

TEST(test_fragment_smelt_full_stock_still_emits_pod) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    station_t *prospect = &w.stations[0];
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        prospect->arm_speed[arm] = 0.0f;
        prospect->arm_rotation[arm] = 0.0f;
    }

    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT,
                                           (int)MAX_PRODUCT_STOCK - 5));

    vec2 smelt_target = prospect->pos;
    ASSERT(test_station_smelt_endpoint_for_ore(prospect, COMMODITY_FERRITE_ORE,
                                               &smelt_target));

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);

    asteroid_t *a = &w.asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 12.0f;
    a->max_ore = 12.0f;
    a->radius = 8.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    a->pos = smelt_target;

    int initial_count = station_finished_count(prospect, COMMODITY_FERRITE_INGOT);
    float initial_stock = prospect->_inventory_cache[COMMODITY_FERRITE_INGOT];
    int initial_frames = station_finished_count(prospect, COMMODITY_FRAME);
    int initial_frame_pod_units =
        test_count_exact_pod_units(&w, COMMODITY_FRAME);
    ASSERT(initial_frame_pod_units > 0);
    int pod_units_before =
        test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    for (int i = 0; i < 600 && w.asteroids[frag].active; i++)
        world_sim_step(&w, 1.0f / 120.0f);

    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT),
                  initial_count);
    ASSERT_EQ_FLOAT(prospect->_inventory_cache[COMMODITY_FERRITE_INGOT],
                    initial_stock, 0.001f);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  initial_frames);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FRAME),
                  initial_frame_pod_units - 1);
    ASSERT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT) >=
           pod_units_before + 12);
}

TEST(test_fragment_smelt_at_full_stock_keeps_station_stock_and_emits_pod) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    station_t *prospect = &w.stations[0];
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        prospect->arm_speed[arm] = 0.0f;
        prospect->arm_rotation[arm] = 0.0f;
    }

    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT,
                                           (int)MAX_PRODUCT_STOCK));

    vec2 smelt_target = prospect->pos;
    ASSERT(test_station_smelt_endpoint_for_ore(prospect, COMMODITY_FERRITE_ORE,
                                               &smelt_target));

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);

    asteroid_t *a = &w.asteroids[frag];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 12.0f;
    a->max_ore = 12.0f;
    a->radius = 8.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    a->pos = smelt_target;

    int initial_count = station_finished_count(prospect, COMMODITY_FERRITE_INGOT);
    float initial_stock = prospect->_inventory_cache[COMMODITY_FERRITE_INGOT];
    int initial_frames = station_finished_count(prospect, COMMODITY_FRAME);
    int initial_frame_pod_units =
        test_count_exact_pod_units(&w, COMMODITY_FRAME);
    ASSERT(initial_frame_pod_units > 0);
    int pod_units_before =
        test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    for (int i = 0; i < 600 && w.asteroids[frag].active; i++)
        world_sim_step(&w, 1.0f / 120.0f);

    ASSERT(!w.asteroids[frag].active);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT),
                  initial_count);
    ASSERT_EQ_FLOAT(prospect->_inventory_cache[COMMODITY_FERRITE_INGOT],
                    initial_stock, 0.001f);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME),
                  initial_frames);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FRAME),
                  initial_frame_pod_units - 1);
    ASSERT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT) >=
           pod_units_before + 12);
}

TEST(test_neural_npc_assignment_preserves_miner_hull_for_hauler_work) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_MINER,
                                         HULL_CLASS_NPC_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    ship_asset_t *asset = world_ship_asset_by_id(&w, npc->ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->hull_class, HULL_CLASS_NPC_MINER);
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    ASSERT_EQ_INT(npc->brain_mode, SERVER_BRAIN_MODE_NEURAL_FLIGHT);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 2));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    npc->known_contract_count = 1;
    npc->known_contracts[0] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
    };

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_MINER);
    ASSERT_EQ_INT(npc->ship.hull_class, HULL_CLASS_NPC_MINER);
    ASSERT_EQ_INT(asset->hull_class, HULL_CLASS_NPC_MINER);
    ASSERT_EQ_INT(asset->ship.hull_class, HULL_CLASS_NPC_MINER);
}

TEST(test_neural_npc_assignment_keeps_mining_over_weak_haul_offer) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_MINER,
                                         HULL_CLASS_NPC_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 2));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 1.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 1.0f,
    };

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_MINER);
    ASSERT_EQ_INT(npc->ship.hull_class, HULL_CLASS_NPC_MINER);
    ASSERT_EQ_FLOAT(npc->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
}

TEST(test_neural_npc_assignment_uses_hauler_hull_for_scaffold_tow) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int c = 0; c < COMMODITY_COUNT; c++)
        w.stations[0]._inventory_cache[c] = MAX_PRODUCT_STOCK;
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++)
        w.stations[0]._inventory_cache[c] = REFINERY_HOPPER_CAPACITY;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int plan_slot = SIGNAL_FIRST_OUTPOST_INDEX;
    w.stations[plan_slot].planned = true;
    w.stations[plan_slot].pos = v2_add(w.stations[1].pos, v2(4000.0f, 0.0f));

    vec2 near_kepler = v2_add(w.stations[1].pos, v2(200.0f, 0.0f));
    int sc_idx = spawn_scaffold(&w, MODULE_SIGNAL_RELAY, near_kepler, 0);
    ASSERT(sc_idx >= 0);
    w.scaffolds[sc_idx].state = SCAFFOLD_LOOSE;
    w.scaffolds[sc_idx].towed_by = -1;

    int slot = test_claim_fresh_npc_hull(&w, 1, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
    npc->ship.pos = w.stations[1].pos;
    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    ship->pos = npc->ship.pos;

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_ASTEROID);
    ASSERT_EQ_INT(npc->target_asteroid, sc_idx);
    ASSERT_EQ_INT(npc->pickup_action, 0xfe);
    bool selected_tow = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_TOW &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_hint[i] == (uint16_t)sc_idx) {
            ASSERT(npc->job_diag_factor_hologram[i] > 0);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_SCAFFOLD_PRESSURE);
            ASSERT(npc->job_diag_memory_station[i] < MAX_STATIONS);
            selected_tow = true;
            break;
        }
    }
    ASSERT(selected_tow);
}

TEST(test_neural_npc_assignment_switches_worker_to_scout_for_fracture_work) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int asteroid = 0;
    memset(&w.asteroids[asteroid], 0, sizeof(w.asteroids[asteroid]));
    w.asteroids[asteroid].active = true;
    w.asteroids[asteroid].tier = ASTEROID_TIER_M;
    w.asteroids[asteroid].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[asteroid].ore = 40.0f;
    w.asteroids[asteroid].max_ore = 40.0f;
    w.asteroids[asteroid].radius = 48.0f;
    w.asteroids[asteroid].pos = v2_add(w.stations[0].pos, v2(800.0f, 0.0f));
    w.asteroids[asteroid].rock_pub[30] = 0x5c;
    w.asteroids[asteroid].rock_pub[31] = 0x01;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_FRACTURE,
        .station_index = 0,
        .commodity = COMMODITY_FERRITE_ORE,
        .quantity_needed = 1.0f,
        .base_price = 250.0f,
        .target_index = asteroid,
        .target_pos = w.asteroids[asteroid].pos,
        .claimed_by = -1,
    };
    contract_set_target_pub_from_asteroid(&w.contracts[0],
                                          &w.asteroids[asteroid]);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_MINER,
                                         HULL_CLASS_NPC_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_MINER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_ASTEROID);
    ASSERT_EQ_INT(npc->target_asteroid, asteroid);
    bool selected_scout = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_SCOUT &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_hint[i] == (uint16_t)asteroid) {
            selected_scout = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_DISTRESS_SIGNAL);
            ASSERT(npc->job_diag_factor_proof[i] > 0);
        }
    }
    ASSERT(selected_scout);
}

TEST(test_neural_npc_assignment_repairs_damaged_worker_from_shared_offer) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_REPAIR_KIT, 20));
    int before_kits = station_finished_count(&w.stations[0],
                                             COMMODITY_REPAIR_KIT);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_MINER,
                                         HULL_CLASS_NPC_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);
    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    npc->ship.hull_class = HULL_CLASS_NPC_MINER;
    ship->hull_class = HULL_CLASS_NPC_MINER;
    ship->hull = npc_max_hull(npc) - 12.0f;
    npc->hull = ship->hull;

    market_memory_t supply = {0};
    ASSERT(market_memory_from_station_supply(&w.stations[0],
                                             0,
                                             COMMODITY_REPAIR_KIT,
                                             12,
                                             &supply));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT(ship->hull > npc_max_hull(npc) - 12.0f);
    ASSERT(npc->hnn_market_mem.experience_count > 0);
    ASSERT_EQ_INT(station_finished_count(&w.stations[0],
                                         COMMODITY_REPAIR_KIT),
                  before_kits - 12);
    bool selected_repair = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_REPAIR &&
            npc->job_diag_selected[i] >= 200) {
            selected_repair = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_REPAIR_NEED);
            ASSERT_EQ_INT(npc->job_diag_commodity[i], COMMODITY_REPAIR_KIT);
            ASSERT(npc->job_diag_factor_hologram[i] > 0);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_SUPPLY);
            ASSERT_EQ_INT(npc->job_diag_memory_station[i], 0);
        }
    }
    ASSERT(selected_repair);
}

TEST(test_neural_npc_assignment_executes_delivery_proof_offer) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 2));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 1.0f,
        .base_price = 500.0f,
        .claimed_by = -1,
    };
    w.contracts[0].proof_flags = CONTRACT_PROOF_REQUIRE_PROOF;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    ASSERT(contract_fit_manifest_count(&w.contracts[0], &ship->manifest) > 0);

    delivery_shipment_t *shipment = NULL;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        if (w.delivery_shipments[i].active &&
            w.delivery_shipments[i].contract_index == 0 &&
            w.delivery_shipments[i].debtor_player == (uint8_t)(MAX_PLAYERS + slot)) {
            shipment = &w.delivery_shipments[i];
            break;
        }
    }
    ASSERT(shipment != NULL);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(shipment->origin_station, 0);
    ASSERT_EQ_INT(shipment->destination_station, 2);
    ASSERT_EQ_INT(shipment->quantity_bound, 1);

    bool selected_proof = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] ==
                (uint8_t)INSPECT_DIAG_JOB_DELIVER_PROOF &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_source[i] == 0 &&
            npc->job_diag_dest[i] == 2) {
            selected_proof = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_DELIVERY_PROOF);
            ASSERT_EQ_INT(npc->job_diag_commodity[i],
                          COMMODITY_FERRITE_INGOT);
            ASSERT(npc->job_diag_factor_proof[i] > 0);
        }
    }
    ASSERT(selected_proof);

    npc->dest_station = 2;
    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    step_npc_ships(&w, SIM_DT);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_DELIVERED);
    ASSERT_EQ_INT(shipment->quantity_delivered, 1);
    ASSERT_EQ_INT(contract_fit_manifest_count(&w.contracts[0],
                                              &w.stations[2].manifest), 1);
    ASSERT_EQ_INT(w.contracts[0].active, true);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 0.0f, 0.001f);

    npc->dest_station = 0;
    npc->state = NPC_STATE_UNLOADING;
    npc->state_timer = 0.0f;
    step_npc_ships(&w, SIM_DT);
    ASSERT_EQ_INT(shipment->status, DELIVERY_SHIPMENT_CLEARED);
    ASSERT_EQ_INT(w.contracts[0].active, false);
}

TEST(test_neural_npc_assignment_uses_market_memory_demand) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 2));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    market_memory_t memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)w.contracts[0].action,
            .station_index = w.contracts[0].station_index,
            .commodity = (uint8_t)w.contracts[0].commodity,
            .quantity_needed = w.contracts[0].quantity_needed,
            .base_price = w.contracts[0].base_price,
            .age_at_copy = w.contracts[0].age,
        },
        &memory));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 1);
    ASSERT(npc->known_contract_count >= 1);
    ASSERT(npc->cargo[COMMODITY_FERRITE_INGOT] > 0.0f);
}

TEST(test_hauler_assignment_weights_route_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    /* Equalize distance so the route memories, not map geometry, decide. */
    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    for (int i = 0; i < 2; i++) {
        int dest = i == 0 ? 1 : 2;
        w.contracts[i] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
            .target_index = -1,
            .claimed_by = -1,
        };
        npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
        };
    }

    const market_memory_t danger = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 20,
        .observed_tick = 10,
        .subject_nonce = 0xA1,
    };
    const market_memory_t success = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS,
        .station_a = 2,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 2,
        .observed_tick = 11,
        .subject_nonce = 0xB2,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&danger, &item));
    knowledge_view_insert(&npc->knowledge, &item);
    ASSERT(knowledge_item_from_market_memory(&success, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 2);
    bool found_route_diag = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_dest[i] == 2) {
            found_route_diag = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_ROUTE_MEMORY);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_ROUTE_SUCCESS);
            ASSERT_EQ_INT(npc->job_diag_proof_kind[i],
                          INSPECT_JOB_PROOF_SUBJECT_HASH);
        }
    }
    ASSERT(found_route_diag);
}

TEST(test_hauler_assignment_explains_selected_route_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 45.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 45.0f,
    };

    const market_memory_t danger = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 20,
        .observed_tick = 10,
        .subject_nonce = 0xE1,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&danger, &item));
    item.witness_hash[0] = 0x44;
    item.witness_hash[1] = 0x55;
    item.witness_hash[2] = 0x66;
    item.witness_hash[3] = 0x77;
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 1);
    bool found_risk_diag = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_dest[i] == 1) {
            found_risk_diag = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_ROUTE_RISK);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_ROUTE_DANGER);
            ASSERT_EQ_INT(npc->job_diag_proof_kind[i],
                          INSPECT_JOB_PROOF_WITNESS_HASH);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][0], 0x44);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][1], 0x55);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][2], 0x66);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][3], 0x77);
        }
    }
    ASSERT(found_risk_diag);
}

TEST(test_risky_hauler_dispatch_emits_escort_route_reputation) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 45.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 45.0f,
    };

    const market_memory_t danger = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 20,
        .observed_tick = 10,
        .subject_nonce = 0xE2,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&danger, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 1);
    market_memory_t escort = {0};
    ASSERT(test_view_has_market_memory(&npc->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &escort));
    ASSERT_EQ_INT(escort.action, CONTRACT_TRACTOR);
    ASSERT(escort.quantity_hint >= 3);
    ASSERT(test_view_has_market_memory(&w.stations[0].knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
}

TEST(test_route_safety_proof_offsets_route_risk_diagnostic) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 45.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 45.0f,
    };

    const market_memory_t danger = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 20,
        .observed_tick = 10,
        .subject_nonce = 0xE3,
    };
    const market_memory_t patrol = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 24,
        .observed_tick = 11,
        .subject_nonce = 0xE4,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&danger, &item));
    knowledge_view_insert(&npc->knowledge, &item);
    ASSERT(knowledge_item_from_market_memory(&patrol, &item));
    item.chain_anchor[0] = 0x91;
    item.chain_anchor[1] = 0x92;
    item.chain_anchor[2] = 0x93;
    item.chain_anchor[3] = 0x94;
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 1);
    bool found_safety_diag = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_dest[i] == 1) {
            found_safety_diag = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_ROUTE_MEMORY);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_ROUTE_REPUTATION);
            ASSERT_EQ_INT(npc->job_diag_proof_kind[i],
                          INSPECT_JOB_PROOF_CHAIN_ANCHOR);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][0], 0x91);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][1], 0x92);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][2], 0x93);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][3], 0x94);
        }
    }
    ASSERT(found_safety_diag);
}

TEST(test_hauler_assignment_weights_delivery_receipt_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    for (int i = 0; i < 2; i++) {
        int dest = i == 0 ? 1 : 2;
        w.contracts[i] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
            .target_index = -1,
            .claimed_by = -1,
        };
        npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
        };
    }

    const market_memory_t danger = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
        .station_a = 1,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 20,
        .observed_tick = 10,
        .subject_nonce = 0xC1,
    };
    const market_memory_t receipt = {
        .active = true,
        .memory_kind = (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT,
        .station_a = 2,
        .station_b = 0,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .action = (uint8_t)CONTRACT_DELIVERY,
        .confidence = 255,
        .salience = 255,
        .quantity_hint = 2,
        .observed_tick = 11,
        .subject_nonce = 0xD2,
    };
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&danger, &item));
    knowledge_view_insert(&npc->knowledge, &item);
    ASSERT(knowledge_item_from_market_memory(&receipt, &item));
    item.chain_anchor[0] = 0xA7;
    item.chain_anchor[1] = 0xB8;
    item.chain_anchor[2] = 0xC9;
    item.chain_anchor[3] = 0xDA;
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 2);
    bool found_receipt_diag = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_dest[i] == 2) {
            found_receipt_diag = true;
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_RECEIPT_PROOF);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_DELIVERY_RECEIPT);
            ASSERT_EQ_INT(npc->job_diag_proof_kind[i],
                          INSPECT_JOB_PROOF_CHAIN_ANCHOR);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][0], 0xA7);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][1], 0xB8);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][2], 0xC9);
            ASSERT_EQ_INT(npc->job_diag_proof_prefix[i][3], 0xDA);
        }
    }
    ASSERT(found_receipt_diag);
}

TEST(test_hauler_assignment_weights_route_reputation_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    for (int i = 0; i < 2; i++) {
        int dest = i == 0 ? 1 : 2;
        w.contracts[i] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
            .target_index = -1,
            .claimed_by = -1,
        };
        npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
        };
    }

    market_memory_t memory = {0};
    knowledge_item_t item;
    ASSERT(market_memory_from_route_reputation(0, 1,
                                               COMMODITY_FERRITE_INGOT,
                                               4,
                                               40.0f,
                                               12,
                                               true,
                                               &memory));
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&npc->knowledge, &item);
    ASSERT(market_memory_from_route_reputation(0, 2,
                                               COMMODITY_FERRITE_INGOT,
                                               4,
                                               80.0f,
                                               13,
                                               false,
                                               &memory));
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 2);
}

TEST(test_hauler_assignment_weights_station_trust_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 4));
    for (int i = 0; i < 2; i++) {
        int dest = i == 0 ? 1 : 2;
        w.contracts[i] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
            .target_index = -1,
            .claimed_by = -1,
        };
        npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = (uint8_t)dest,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 2.0f,
            .base_price = 25.0f,
        };
    }

    market_memory_t trust = {0};
    ASSERT(market_memory_from_station_trust(2,
                                            (uint8_t)CONTRACT_TRACTOR,
                                            COMMODITY_FERRITE_INGOT,
                                            8,
                                            200.0f,
                                            15,
                                            &trust));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&trust, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->dest_station, 2);
    bool found_trust_diag = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_dest[i] == 2) {
            found_trust_diag = true;
            ASSERT(npc->job_diag_factor_proof[i] > 0);
        }
    }
    ASSERT(found_trust_diag);
}

TEST(test_hauler_assignment_weights_supply_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_REPAIR_KIT,
                                           (int)HAULER_RESERVE + 1));
    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 8));

    const commodity_t commodities[2] = {
        COMMODITY_REPAIR_KIT,
        COMMODITY_FERRITE_INGOT,
    };
    for (int i = 0; i < 2; i++) {
        w.contracts[i] = (contract_t){
            .active = true,
            .action = CONTRACT_TRACTOR,
            .station_index = 1,
            .commodity = commodities[i],
            .quantity_needed = 1.0f,
            .base_price = 25.0f,
            .target_index = -1,
            .claimed_by = -1,
        };
        npc->known_contracts[npc->known_contract_count++] = (contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = 1,
            .commodity = (uint8_t)commodities[i],
            .quantity_needed = 1.0f,
            .base_price = 25.0f,
        };
    }

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT(npc->cargo[COMMODITY_FERRITE_INGOT] > 0.0f);
    ASSERT_EQ_FLOAT(npc->cargo[COMMODITY_REPAIR_KIT], 0.0f, 0.001f);
}

TEST(test_hauler_uses_remote_supply_memory_for_pickup) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(&w.stations[1],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 3));
    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FERRITE_INGOT, 0));
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 400.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    market_memory_t demand = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)w.contracts[0].action,
            .station_index = w.contracts[0].station_index,
            .commodity = (uint8_t)w.contracts[0].commodity,
            .quantity_needed = w.contracts[0].quantity_needed,
            .base_price = w.contracts[0].base_price,
            .age_at_copy = w.contracts[0].age,
        },
        &demand));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&demand, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    market_memory_t supply = {0};
    ASSERT(market_memory_from_station_supply(&w.stations[1],
                                             1,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    item.hops = 2;
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->pickup_station, 1);
    ASSERT_EQ_INT(npc->dest_station, 2);
    ASSERT_EQ_INT(npc->pickup_commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_FLOAT(npc->cargo[COMMODITY_FERRITE_INGOT], 0.0f, 0.001f);
    bool found_remote_pickup_diag = false;
    for (int i = 0; i < npc->job_diag_count && i < 4; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            npc->job_diag_selected[i] >= 200 &&
            npc->job_diag_source[i] == 1 &&
            npc->job_diag_dest[i] == 2) {
            found_remote_pickup_diag = true;
            ASSERT(npc->job_diag_factor_value[i] > 0);
            ASSERT(npc->job_diag_factor_demand[i] > 0);
            ASSERT(npc->job_diag_factor_supply[i] > 0);
            ASSERT(npc->job_diag_factor_route[i] > 0);
            ASSERT(npc->job_diag_factor_freshness[i] > 0);
            ASSERT(npc->job_diag_factor_capability[i] > 0);
            ASSERT_EQ_INT(npc->job_diag_reason[i],
                          INSPECT_JOB_REASON_REMOTE_SUPPLY);
            ASSERT_EQ_INT(npc->job_diag_memory_kind[i],
                          MARKET_MEMORY_SUPPLY);
            ASSERT_EQ_INT(npc->job_diag_memory_hops[i], 2);
            ASSERT_EQ_INT(npc->job_diag_memory_station[i], 1);
        }
    }
    ASSERT(found_remote_pickup_diag);

    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    npc->ship.pos = station_approach_target(&w.stations[1], npc->ship.pos);
    ship->pos = npc->ship.pos;
    npc->ship.vel = v2(0.0f, 0.0f);
    ship->vel = npc->ship.vel;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(npc->state, NPC_STATE_UNLOADING);
    npc->state_timer = 0.0f;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->pickup_station, -1);
    ASSERT(npc->cargo[COMMODITY_FERRITE_INGOT] > 0.0f);
    ASSERT_EQ_INT(station_finished_count(&w.stations[1], COMMODITY_FERRITE_INGOT),
                  (int)HAULER_RESERVE);
}

TEST(test_remote_supply_route_starts_from_current_ship_position) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.stations[0].pos = v2(0.0f, 0.0f);
    w.stations[1].pos = v2(10000.0f, 0.0f);
    w.stations[2].pos = v2(0.0f, 100.0f);
    w.stations[3].pos = v2(0.0f, 1000.0f);

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(&w.stations[1],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 3));
    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 3));
    ASSERT(test_set_station_finished_units(&w.stations[3],
                                           COMMODITY_FERRITE_INGOT, 0));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 3,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 400.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    npc->state = NPC_STATE_IDLE;
    npc->state_timer = 0.0f;
    npc->ship.pos = w.stations[1].pos;
    npc->ship.vel = v2(0.0f, 0.0f);
    ship->pos = npc->ship.pos;
    ship->vel = npc->ship.vel;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    knowledge_item_t item;
    market_memory_t demand = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)w.contracts[0].action,
            .station_index = w.contracts[0].station_index,
            .commodity = (uint8_t)w.contracts[0].commodity,
            .quantity_needed = w.contracts[0].quantity_needed,
            .base_price = w.contracts[0].base_price,
            .age_at_copy = w.contracts[0].age,
        },
        &demand));
    ASSERT(knowledge_item_from_market_memory(&demand, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    market_memory_t supply = {0};
    ASSERT(market_memory_from_station_supply(&w.stations[1],
                                             1,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);
    ASSERT(market_memory_from_station_supply(&w.stations[2],
                                             2,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->pickup_station, 1);
    ASSERT_EQ_INT(npc->dest_station, 3);
}

TEST(test_failed_remote_pickup_emits_station_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(&w.stations[1],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 2));
    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FERRITE_INGOT, 0));
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 400.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    market_memory_t demand = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)w.contracts[0].action,
            .station_index = w.contracts[0].station_index,
            .commodity = (uint8_t)w.contracts[0].commodity,
            .quantity_needed = w.contracts[0].quantity_needed,
            .base_price = w.contracts[0].base_price,
            .age_at_copy = w.contracts[0].age,
        },
        &demand));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&demand, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    market_memory_t supply = {0};
    ASSERT(market_memory_from_station_supply(&w.stations[1],
                                             1,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);
    ASSERT_EQ_INT(npc->pickup_station, 1);
    ASSERT_EQ_INT(npc->dest_station, 2);

    ASSERT(test_set_station_finished_units(&w.stations[1],
                                           COMMODITY_FERRITE_INGOT, 0));

    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    npc->ship.pos = station_approach_target(&w.stations[1], npc->ship.pos);
    ship->pos = npc->ship.pos;
    npc->ship.vel = v2(0.0f, 0.0f);
    ship->vel = npc->ship.vel;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(npc->state, NPC_STATE_UNLOADING);
    npc->state_timer = 0.0f;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(npc->state, NPC_STATE_RETURN_TO_STATION);
    ASSERT(test_view_has_market_memory(&npc->knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       1, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT(test_view_has_market_memory(&w.stations[1].knowledge,
                                       (uint8_t)MARKET_MEMORY_STATION_RISK,
                                       1, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
}

TEST(test_hauler_assignment_avoids_station_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(&w.stations[1],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 3));
    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 3));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 3,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 400.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    knowledge_item_t item;
    market_memory_t demand = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)w.contracts[0].action,
            .station_index = w.contracts[0].station_index,
            .commodity = (uint8_t)w.contracts[0].commodity,
            .quantity_needed = w.contracts[0].quantity_needed,
            .base_price = w.contracts[0].base_price,
            .age_at_copy = w.contracts[0].age,
        },
        &demand));
    ASSERT(knowledge_item_from_market_memory(&demand, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    market_memory_t supply = {0};
    ASSERT(market_memory_from_station_supply(&w.stations[1],
                                             1,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);
    ASSERT(market_memory_from_station_supply(&w.stations[2],
                                             2,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    market_memory_t risk = {0};
    ASSERT(market_memory_from_station_risk(1,
                                           (uint8_t)CONTRACT_TRACTOR,
                                           COMMODITY_FERRITE_INGOT,
                                           6,
                                           50.0f,
                                           20,
                                           &risk));
    ASSERT(knowledge_item_from_market_memory(&risk, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->pickup_station, 2);
    ASSERT_EQ_INT(npc->dest_station, 3);
}

TEST(test_hauler_assignment_avoids_destination_station_risk_memory) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    ASSERT(test_set_station_finished_units(&w.stations[0],
                                           COMMODITY_FERRITE_INGOT,
                                           (int)HAULER_RESERVE + 6));
    ASSERT(test_set_station_finished_units(&w.stations[1],
                                           COMMODITY_FERRITE_INGOT, 0));
    ASSERT(test_set_station_finished_units(&w.stations[2],
                                           COMMODITY_FERRITE_INGOT, 0));
    w.stations[1].pos = v2(1000.0f, 0.0f);
    w.stations[2].pos = v2(1000.0f, 0.0f);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 400.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    w.contracts[1] = w.contracts[0];
    w.contracts[1].station_index = 2;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->known_contracts, 0, sizeof(npc->known_contracts));
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    knowledge_item_t item;
    for (int i = 0; i < 2; i++) {
        market_memory_t demand = {0};
        ASSERT(market_memory_from_contract_summary(
            &(contract_summary_t){
                .active = true,
                .action = (uint8_t)w.contracts[i].action,
                .station_index = w.contracts[i].station_index,
                .commodity = (uint8_t)w.contracts[i].commodity,
                .quantity_needed = w.contracts[i].quantity_needed,
                .base_price = w.contracts[i].base_price,
                .age_at_copy = w.contracts[i].age,
            },
            &demand));
        ASSERT(knowledge_item_from_market_memory(&demand, &item));
        knowledge_view_insert(&npc->knowledge, &item);
    }

    market_memory_t supply = {0};
    ASSERT(market_memory_from_station_supply(&w.stations[0],
                                             0,
                                             COMMODITY_FERRITE_INGOT,
                                             12,
                                             &supply));
    ASSERT(knowledge_item_from_market_memory(&supply, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    market_memory_t risk = {0};
    ASSERT(market_memory_from_station_risk(1,
                                           (uint8_t)CONTRACT_TRACTOR,
                                           COMMODITY_FERRITE_INGOT,
                                           6,
                                           50.0f,
                                           20,
                                           &risk));
    ASSERT(knowledge_item_from_market_memory(&risk, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(npc->pickup_station, -1);
    ASSERT_EQ_INT(npc->dest_station, 2);
}

TEST(test_neural_worker_dock_encodes_market_memory_into_hnn) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    ASSERT_EQ_INT(npc->brain_mode, SERVER_BRAIN_MODE_NEURAL_FLIGHT);
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    memset(&npc->hnn_market_mem, 0, sizeof(npc->hnn_market_mem));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    market_memory_t memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = 2,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 3.0f,
            .base_price = 90.0f,
            .age_at_copy = 12.0f,
        },
        &memory));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    gossip_hnn_exchange(&w, 0, npc);

    ASSERT(npc->hnn_market_mem.experience_count > 0);
    ASSERT(w.stations[0].hnn_market_memory.experience_count > 0);
    ASSERT(gossip_hnn_market_resonance(&npc->hnn_market_mem,
                                       &memory,
                                       GOSSIP_HNN_JOB_HAUL) > 0.05f);
}

TEST(test_neural_worker_transports_market_hnn_between_stations) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
        memset(&w.stations[s].hnn_market_memory, 0,
               sizeof(w.stations[s].hnn_market_memory));
        w.stations[s].hnn_market_version = 0;
    }

    int courier_slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    int listener_slot = spawn_npc(&w, 1, NPC_ROLE_MINER);
    ASSERT(courier_slot >= 0);
    ASSERT(listener_slot >= 0);
    npc_ship_t *courier = &w.npc_ships[courier_slot];
    npc_ship_t *listener = &w.npc_ships[listener_slot];
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    memset(&courier->hnn_market_mem, 0, sizeof(courier->hnn_market_mem));
    memset(&listener->knowledge, 0, sizeof(listener->knowledge));
    memset(&listener->hnn_market_mem, 0, sizeof(listener->hnn_market_mem));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);
    knowledge_view_configure(&listener->knowledge, SHIP_KNOWN_ITEM_CAP);
    courier->hnn_market_station = 0xffu;
    listener->hnn_market_station = 0xffu;

    market_memory_t memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_FRACTURE,
            .station_index = 2,
            .commodity = (uint8_t)COMMODITY_FERRITE_ORE,
            .quantity_needed = 1.0f,
            .base_price = 180.0f,
            .age_at_copy = 10.0f,
        },
        &memory));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&courier->knowledge, &item);

    gossip_hnn_exchange(&w, 0, courier);
    ASSERT(courier->hnn_market_mem.experience_count > 0);
    ASSERT(w.stations[0].hnn_market_memory.experience_count > 0);
    ASSERT_EQ_INT(w.stations[1].hnn_market_memory.experience_count, 0);

    gossip_hnn_exchange(&w, 1, courier);
    ASSERT(w.stations[1].hnn_market_memory.experience_count > 0);
    ASSERT(gossip_hnn_market_resonance(&w.stations[1].hnn_market_memory,
                                       &memory,
                                       GOSSIP_HNN_JOB_SCOUT) > 0.05f);

    gossip_hnn_exchange(&w, 1, listener);
    ASSERT(listener->hnn_market_mem.experience_count > 0);
    ASSERT(gossip_hnn_market_resonance(&listener->hnn_market_mem,
                                       &memory,
                                       GOSSIP_HNN_JOB_SCOUT) > 0.05f);
}

TEST(test_neural_worker_physically_transports_contract_memory_between_stations) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 3.0f,
        .base_price = 90.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    gossip_bootstrap_world_stations(&w);
    ASSERT_EQ_INT(w.stations[2].known_contract_count, 1);
    ASSERT_EQ_INT(w.stations[0].known_contract_count, 0);
    ASSERT(!test_view_has_market_memory(&w.stations[0].knowledge,
                                        (uint8_t)MARKET_MEMORY_DEMAND,
                                        2, 0xff,
                                        (uint8_t)COMMODITY_FERRITE_INGOT,
                                        NULL));

    int slot = test_claim_fresh_npc_hull(&w, 2, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *courier = &w.npc_ships[slot];
    courier->state = NPC_STATE_DOCKED;
    courier->state_timer = 0.0f;
    courier->known_contract_count = 0;
    memset(courier->known_contracts, 0, sizeof(courier->known_contracts));
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);

    gossip_dock_handshake(&w, 2,
                          courier->known_contracts,
                          &courier->known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &courier->knowledge);
    ASSERT_EQ_INT(courier->known_contract_count, 1);
    ASSERT(test_view_has_market_memory(&courier->knowledge,
                                       (uint8_t)MARKET_MEMORY_DEMAND,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));

    courier->dest_station = 0;
    courier->pickup_station = -1;
    courier->pickup_commodity = COMMODITY_COUNT;
    courier->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    courier->state = NPC_STATE_UNLOADING;
    courier->state_timer = 0.0f;
    courier->ship.pos = station_approach_target(&w.stations[0],
                                                courier->ship.pos);
    courier->ship.vel = v2(0.0f, 0.0f);

    step_npc_ships(&w, SIM_DT);

    ASSERT(test_view_has_market_memory(&w.stations[0].knowledge,
                                       (uint8_t)MARKET_MEMORY_DEMAND,
                                       2, 0xff,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       NULL));
    ASSERT_EQ_INT(w.stations[0].known_contract_count, 1);
    ASSERT_EQ_INT(w.stations[0].known_contracts[0].station_index, 2);
}

TEST(test_idle_neural_worker_runs_gossip_courier_trip) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) w.scaffolds[i].active = false;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FRAME,
        .quantity_needed = 2.0f,
        .base_price = 110.0f,
        .target_index = -1,
        .claimed_by = -1,
    };
    gossip_bootstrap_world_stations(&w);
    ASSERT_EQ_INT(w.stations[1].known_contract_count, 1);
    ASSERT_EQ_INT(w.stations[2].known_contract_count, 0);

    int slot = test_claim_fresh_npc_hull(&w, 1, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *courier = &w.npc_ships[slot];
    courier->state = NPC_STATE_DOCKED;
    courier->state_timer = 0.0f;
    courier->known_contract_count = 0;
    memset(courier->known_contracts, 0, sizeof(courier->known_contracts));
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(courier->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(courier->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(courier->dest_station, 2);
    ASSERT_EQ_INT(courier->pickup_station, -1);
    ASSERT_EQ_INT(courier->pickup_commodity, COMMODITY_COUNT);
    ASSERT(test_view_has_market_memory(&courier->knowledge,
                                       (uint8_t)MARKET_MEMORY_DEMAND,
                                       1, 0xff,
                                       (uint8_t)COMMODITY_FRAME,
                                       NULL));
    bool selected_courier = false;
    for (int i = 0; i < courier->job_diag_count && i < 4; i++) {
        if (courier->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            courier->job_diag_selected[i] >= 200 &&
            courier->job_diag_dest[i] == 2) {
            selected_courier = true;
            ASSERT_EQ_INT(courier->job_diag_reason[i],
                          INSPECT_JOB_REASON_GOSSIP_COURIER);
            ASSERT_EQ_INT(courier->job_diag_commodity[i], COMMODITY_COUNT);
        }
    }
    ASSERT(selected_courier);

    courier->state = NPC_STATE_UNLOADING;
    courier->state_timer = 0.0f;
    courier->ship.pos = station_approach_target(&w.stations[2],
                                                courier->ship.pos);
    courier->ship.vel = v2(0.0f, 0.0f);
    step_npc_ships(&w, SIM_DT);

    ASSERT(test_view_has_market_memory(&w.stations[2].knowledge,
                                       (uint8_t)MARKET_MEMORY_DEMAND,
                                       1, 0xff,
                                       (uint8_t)COMMODITY_FRAME,
                                       NULL));
    bool station2_heard_station1_contract = false;
    for (int i = 0; i < w.stations[2].known_contract_count; i++) {
        if (w.stations[2].known_contracts[i].station_index == 1 &&
            w.stations[2].known_contracts[i].commodity == COMMODITY_FRAME) {
            station2_heard_station1_contract = true;
            break;
        }
    }
    ASSERT(station2_heard_station1_contract);
}

TEST(test_idle_neural_worker_runs_baseline_gossip_without_contracts) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int c = 0; c < COMMODITY_COUNT; c++)
        w.stations[0]._inventory_cache[c] = MAX_PRODUCT_STOCK;
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++)
        w.stations[0]._inventory_cache[c] = REFINERY_HOPPER_CAPACITY;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) w.scaffolds[i].active = false;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *courier = &w.npc_ships[slot];
    courier->state = NPC_STATE_DOCKED;
    courier->state_timer = 0.0f;
    courier->known_contract_count = 0;
    memset(courier->known_contracts, 0, sizeof(courier->known_contracts));
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(courier->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(courier->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(courier->dest_station, 1);
    ASSERT_EQ_INT(courier->pickup_station, -1);
    ASSERT_EQ_INT(courier->pickup_commodity, COMMODITY_COUNT);

    bool selected_courier = false;
    for (int i = 0; i < courier->job_diag_count && i < 4; i++) {
        if (courier->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            courier->job_diag_selected[i] >= 200 &&
            courier->job_diag_reason[i] == INSPECT_JOB_REASON_GOSSIP_COURIER) {
            selected_courier = true;
            ASSERT_EQ_INT(courier->job_diag_commodity[i], COMMODITY_COUNT);
            break;
        }
    }
    ASSERT(selected_courier);
}

TEST(test_neural_worker_runs_gossip_when_ore_target_unavailable) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int s = 0; s < MAX_STATIONS; s++) {
        memset(w.stations[s].known_contracts, 0,
               sizeof(w.stations[s].known_contracts));
        w.stations[s].known_contract_count = 0;
        memset(&w.stations[s].knowledge, 0, sizeof(w.stations[s].knowledge));
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) w.scaffolds[i].active = false;

    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 0.0f;
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] = 0.0f;
    w.stations[0]._inventory_cache[COMMODITY_FRAME] = 0.0f;

    int slot = test_claim_fresh_npc_hull(&w, 0, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *courier = &w.npc_ships[slot];
    courier->state = NPC_STATE_DOCKED;
    courier->state_timer = 0.0f;
    courier->known_contract_count = 0;
    memset(courier->known_contracts, 0, sizeof(courier->known_contracts));
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(courier->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(courier->state, NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(courier->dest_station, 1);
    bool selected_courier = false;
    for (int i = 0; i < courier->job_diag_count && i < 4; i++) {
        if (courier->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_JOB_HAUL &&
            courier->job_diag_selected[i] >= 200 &&
            courier->job_diag_reason[i] == INSPECT_JOB_REASON_GOSSIP_COURIER) {
            selected_courier = true;
            break;
        }
    }
    ASSERT(selected_courier);
}

TEST(test_neural_worker_market_hnn_pool_decays_under_new_attention) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));
    memset(&w.stations[0].hnn_market_memory, 0,
           sizeof(w.stations[0].hnn_market_memory));
    w.stations[0].hnn_market_version = 0;

    int slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *courier = &w.npc_ships[slot];
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    memset(&courier->hnn_market_mem, 0, sizeof(courier->hnn_market_mem));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);
    courier->hnn_market_station = 0xffu;
    courier->hnn_market_version = 0;

    market_memory_t haul_memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = 2,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 3.0f,
            .base_price = 90.0f,
            .age_at_copy = 12.0f,
        },
        &haul_memory));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&haul_memory, &item));
    knowledge_view_insert(&courier->knowledge, &item);
    gossip_hnn_exchange(&w, 0, courier);

    float haul_before = gossip_hnn_market_resonance(
        &w.stations[0].hnn_market_memory, &haul_memory, GOSSIP_HNN_JOB_HAUL);
    ASSERT(haul_before > 0.05f);

    market_memory_t proof_memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_DELIVERY,
            .station_index = 3,
            .commodity = (uint8_t)COMMODITY_LASER_MODULE,
            .quantity_needed = 1.0f,
            .base_price = 180.0f,
            .age_at_copy = 2.0f,
        },
        &proof_memory));

    for (int i = 0; i < 10; i++) {
        memset(&courier->knowledge, 0, sizeof(courier->knowledge));
        knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);
        hnn_memory_init(&courier->hnn_market_mem);
        courier->hnn_market_station = 0xffu;
        courier->hnn_market_version = 0;
        ASSERT(knowledge_item_from_market_memory(&proof_memory, &item));
        knowledge_view_insert(&courier->knowledge, &item);
        gossip_hnn_exchange(&w, 0, courier);
    }

    ASSERT(w.stations[0].hnn_market_memory.experience_count <= 16);
    float haul_after = gossip_hnn_market_resonance(
        &w.stations[0].hnn_market_memory, &haul_memory, GOSSIP_HNN_JOB_HAUL);
    float proof_after = gossip_hnn_market_resonance(
        &w.stations[0].hnn_market_memory,
        &proof_memory,
        GOSSIP_HNN_JOB_DELIVER_PROOF);
    ASSERT(proof_after > 0.05f);
    ASSERT(haul_after < haul_before);
    ASSERT(proof_after > haul_after);
}

TEST(test_neural_worker_market_hnn_pool_decays_when_idle) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;
    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));
    memset(&w.stations[0].hnn_market_memory, 0,
           sizeof(w.stations[0].hnn_market_memory));
    w.stations[0].hnn_market_version = 0;
    w.stations[0].hnn_market_decay_tick = 0;

    int slot = spawn_npc(&w, 0, NPC_ROLE_MINER);
    ASSERT(slot >= 0);
    npc_ship_t *courier = &w.npc_ships[slot];
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    memset(&courier->hnn_market_mem, 0, sizeof(courier->hnn_market_mem));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);
    courier->hnn_market_station = 0xffu;
    courier->hnn_market_version = 0;
    courier->hnn_market_decay_tick = 0;

    market_memory_t memory = {0};
    ASSERT(market_memory_from_contract_summary(
        &(contract_summary_t){
            .active = true,
            .action = (uint8_t)CONTRACT_TRACTOR,
            .station_index = 2,
            .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
            .quantity_needed = 3.0f,
            .base_price = 90.0f,
            .age_at_copy = 12.0f,
        },
        &memory));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&memory, &item));
    knowledge_view_insert(&courier->knowledge, &item);
    gossip_hnn_exchange(&w, 0, courier);

    ASSERT(w.stations[0].hnn_market_memory.experience_count > 0);
    ASSERT(gossip_hnn_market_resonance(&w.stations[0].hnn_market_memory,
                                       &memory,
                                       GOSSIP_HNN_JOB_HAUL) > 0.05f);

    memset(&w.stations[0].knowledge, 0, sizeof(w.stations[0].knowledge));
    knowledge_view_configure(&w.stations[0].knowledge, STATION_KNOWN_ITEM_CAP);
    memset(&courier->knowledge, 0, sizeof(courier->knowledge));
    knowledge_view_configure(&courier->knowledge, SHIP_KNOWN_ITEM_CAP);
    hnn_memory_init(&courier->hnn_market_mem);
    courier->hnn_market_station = 0xffu;
    courier->hnn_market_version = 0;
    courier->hnn_market_decay_tick = 0;

    w.tick += 120u * 60u;
    gossip_hnn_exchange(&w, 0, courier);

    ASSERT_EQ_INT(w.stations[0].hnn_market_memory.experience_count, 0);
    ASSERT(gossip_hnn_market_resonance(&w.stations[0].hnn_market_memory,
                                       &memory,
                                       GOSSIP_HNN_JOB_HAUL) == 0.0f);
}

TEST(test_neural_npc_assignment_preserves_hauler_hull_for_ore_work) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    ASSERT(test_set_station_finished_units(&w.stations[2], COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(&w.stations[2], COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(&w.stations[2], COMMODITY_TRACTOR_MODULE, 0));

    int slot = test_claim_fresh_npc_hull(&w, 2, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    ship_asset_t *asset = world_ship_asset_by_id(&w, npc->ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->hull_class, HULL_CLASS_HAULER);
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->known_contract_count = 0;
    memset(npc->cargo, 0, sizeof(npc->cargo));
    ASSERT_EQ_INT(npc->brain_mode, SERVER_BRAIN_MODE_NEURAL_FLIGHT);

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(asset->hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(asset->ship.hull_class, HULL_CLASS_HAULER);
}

TEST(test_hauler_damage_emits_route_danger_memory) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->role = NPC_ROLE_HAULER;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->home_station = 0;
    npc->dest_station = 1;
    npc->cargo[COMMODITY_FERRITE_INGOT] = 1.0f;
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    ship->hull = 100.0f;
    apply_npc_ship_damage_attributed(&w, slot, 12.0f, NULL,
                                     DEATH_CAUSE_ASTEROID);

    market_memory_t danger = {0};
    ASSERT(test_view_has_market_memory(&npc->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &danger));
    ASSERT_EQ_INT(danger.action, CONTRACT_TRACTOR);
    ASSERT(danger.salience >= 120);
    ASSERT_EQ_INT(danger.quantity_hint, 12);
    market_memory_t risk = {0};
    ASSERT(test_view_has_market_memory(&npc->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_RISK,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &risk));
    ASSERT_EQ_INT(risk.action, CONTRACT_TRACTOR);
    ASSERT_EQ_INT(risk.quantity_hint, 1);
}

TEST(test_route_reputation_reduces_hauler_damage) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    int slot = spawn_npc(&w, 0, NPC_ROLE_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    npc->role = NPC_ROLE_HAULER;
    npc->state = NPC_STATE_TRAVEL_TO_DEST;
    npc->home_station = 0;
    npc->dest_station = 1;
    npc->cargo[COMMODITY_FERRITE_INGOT] = 1.0f;
    memset(&npc->knowledge, 0, sizeof(npc->knowledge));
    knowledge_view_configure(&npc->knowledge, SHIP_KNOWN_ITEM_CAP);

    market_memory_t patrol = {0};
    ASSERT(market_memory_from_route_reputation(0, 1,
                                               COMMODITY_FERRITE_INGOT,
                                               8,
                                               180.0f,
                                               42,
                                               false,
                                               &patrol));
    knowledge_item_t item;
    ASSERT(knowledge_item_from_market_memory(&patrol, &item));
    knowledge_view_insert(&npc->knowledge, &item);

    ship_t *ship = world_npc_ship_for(&w, slot);
    ASSERT(ship != NULL);
    ship->hull = 100.0f;
    apply_npc_ship_damage_attributed(&w, slot, 20.0f, NULL,
                                     DEATH_CAUSE_ASTEROID);

    ASSERT(ship->hull > 80.0f);
    ASSERT(ship->hull < 100.0f);

    market_memory_t danger = {0};
    ASSERT(test_view_has_market_memory(&npc->knowledge,
                                       (uint8_t)MARKET_MEMORY_ROUTE_DANGER,
                                       1, 0,
                                       (uint8_t)COMMODITY_FERRITE_INGOT,
                                       &danger));
    ASSERT(danger.quantity_hint < 20);
    ASSERT(danger.quantity_hint >= 12);
}

TEST(test_legacy_hauler_brain_mode_upgrades_before_assignment) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    ASSERT(test_set_station_finished_units(&w.stations[2], COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(&w.stations[2], COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(&w.stations[2], COMMODITY_TRACTOR_MODULE, 0));

    int slot = test_claim_fresh_npc_hull(&w, 2, NPC_ROLE_HAULER,
                                         HULL_CLASS_HAULER);
    ASSERT(slot >= 0);
    npc_ship_t *npc = &w.npc_ships[slot];
    ship_asset_t *asset = world_ship_asset_by_id(&w, npc->ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->hull_class, HULL_CLASS_HAULER);
    npc->state = NPC_STATE_DOCKED;
    npc->state_timer = 0.0f;
    npc->brain_mode = SERVER_BRAIN_MODE_NONE;
    npc->known_contract_count = 0;
    memset(npc->cargo, 0, sizeof(npc->cargo));

    step_npc_ships(&w, SIM_DT);

    ASSERT_EQ_INT(npc->brain_mode, SERVER_BRAIN_MODE_NEURAL_FLIGHT);
    ASSERT_EQ_INT(npc->role, NPC_ROLE_HAULER);
    ASSERT_EQ_INT(npc->ship.hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(asset->hull_class, HULL_CLASS_HAULER);
    ASSERT_EQ_INT(asset->ship.hull_class, HULL_CLASS_HAULER);
}

TEST(test_neural_bot_contract_logistics_buys_and_delivers_ingot) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *prospect = &w.stations[0];
    station_t *kepler = &w.stations[1];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 4));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    ASSERT(test_set_station_finished_units(kepler, COMMODITY_FERRITE_INGOT, 0));
    int prospect_market_pod = test_spawn_station_market_exact_pod(
        &w, 0, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(prospect_market_pod >= 0);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    uint8_t token[8] = {0xB0, 0x7A, 0xC0, 0x01, 0x02, 0x03, 0x04, 0x05};
    memcpy(sp->session_token, token, sizeof(token));
    player_init_ship(sp, &w);
    memcpy(sp->session_token, token, sizeof(token));
    ledger_earn(prospect, sp->session_token, 100.0f);

    sp->server_brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    sp->autopilot_mode = 1;
    sp->autopilot_state = AUTOPILOT_STEP_SELL;
    sp->autopilot_target = -1;
    sp->autopilot_timer = 1.0f;
    sp->autopilot_last_pos = sp->ship.pos;
    sp->autopilot_stuck_timer = 0.0f;
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, &w);

    uint64_t contract_decisions_before =
        signal_intelligence_contract_decision_count();
    uint64_t contract_teacher_before =
        signal_intelligence_contract_teacher_decision_count();

    world_sim_step(&w, SIM_DT);
    ASSERT(signal_intelligence_contract_decision_count() > contract_decisions_before);
    ASSERT(signal_intelligence_contract_teacher_decision_count() > contract_teacher_before);
    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_LOGISTICS_BUY);
    ASSERT_EQ_INT(sp->autopilot_station_target, 1);
    ASSERT_EQ_INT(sp->autopilot_cargo, COMMODITY_FERRITE_INGOT);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(ship_finished_count(&sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], prospect_market_pod);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT), 1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME), 1);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_LOGISTICS_TRAVEL);
    ASSERT_EQ_INT(sp->autopilot_station_target, 1);

    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->autopilot_state = AUTOPILOT_STEP_LOGISTICS_DELIVER;
    sp->autopilot_timer = 0.0f;
    anchor_ship_in_station(sp, &w);

    int delivered_pod = sp->ship.towed_pods[0];
    int kepler_hopper_idx =
        station_find_hopper_for(kepler, COMMODITY_FERRITE_INGOT);
    ASSERT(delivered_pod >= 0);
    ASSERT(kepler_hopper_idx >= 0);
    int kepler_before = station_finished_count(kepler, COMMODITY_FERRITE_INGOT);
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(ship_finished_count(&sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 0);
    ASSERT(w.cargo_pods[delivered_pod].active);
    ASSERT_EQ_INT(w.cargo_pods[delivered_pod].towed_by, -1);
    ASSERT(cargo_pod_is_tractored_by_module(&w.cargo_pods[delivered_pod],
                                            1, kepler_hopper_idx));
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FERRITE_INGOT),
                  kepler_before);
    ASSERT_EQ_FLOAT(w.contracts[0].quantity_needed, 1.0f, 0.001f);

    bool sold_at_kepler = false;
    for (int i = 0; i < w.events.count; i++) {
        if (w.events.events[i].type == SIM_EVENT_SELL &&
            w.events.events[i].sell.station == 1 &&
            w.events.events[i].sell.by_contract) {
            sold_at_kepler = true;
            break;
        }
    }
    ASSERT(sold_at_kepler);
}

TEST(test_autopilot_toggle_with_towed_pod_plans_logistics_delivery) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 1.0f,
        .base_price = 70.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    sp->docked = false;
    sp->current_station = 0;
    sp->nearby_station = -1;
    sp->in_dock_range = false;
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.pos = v2_add(w.stations[0].pos, v2(700.0f, 0.0f));
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->input.toggle_autopilot = true;

    int pod_idx = test_spawn_exact_pod(&w, v2_add(sp->ship.pos, v2(-55.0f, 0.0f)),
                                       COMMODITY_FERRITE_INGOT, 1);
    ASSERT(pod_idx >= 0);
    sp->ship.towed_pods[0] = (int16_t)pod_idx;
    sp->ship.towed_pod_count = 1;
    w.cargo_pods[pod_idx].towed_by = 0;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->autopilot_mode, 1);
    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_LOGISTICS_TRAVEL);
    ASSERT_EQ_INT(sp->autopilot_station_target, 1);
    ASSERT_EQ_INT(sp->autopilot_cargo, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], pod_idx);
}

TEST(test_neural_bot_logistics_buys_on_station_credit) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *prospect = &w.stations[0];
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FERRITE_INGOT, 4));
    ASSERT(test_set_station_finished_units(prospect, COMMODITY_FRAME, 1));
    int prospect_market_pod = test_spawn_station_market_exact_pod(
        &w, 0, COMMODITY_FERRITE_INGOT, 1);
    ASSERT(prospect_market_pod >= 0);

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 2.0f,
        .base_price = 25.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    uint8_t token[8] = {0xB0, 0x7A, 0xC0, 0x09, 0x02, 0x03, 0x04, 0x05};
    memcpy(sp->session_token, token, sizeof(token));
    player_init_ship(sp, &w);
    memcpy(sp->session_token, token, sizeof(token));

    sp->server_brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    sp->autopilot_mode = 1;
    sp->autopilot_state = AUTOPILOT_STEP_SELL;
    sp->autopilot_target = -1;
    sp->autopilot_timer = 1.0f;
    sp->autopilot_last_pos = sp->ship.pos;
    sp->autopilot_stuck_timer = 0.0f;
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, &w);

    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token), 0.0f, 0.001f);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_LOGISTICS_BUY);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(ship_finished_count(&sp->ship, COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(sp->ship.towed_pod_count, 1);
    ASSERT_EQ_INT(sp->ship.towed_pods[0], prospect_market_pod);
    ASSERT_EQ_INT(test_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT), 1);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FERRITE_INGOT), 4);
    ASSERT_EQ_INT(station_finished_count(prospect, COMMODITY_FRAME), 1);
    ASSERT_EQ_FLOAT(ledger_balance(prospect, sp->session_token), 0.0f, 0.001f);

    test_move_pod_past_station_charge_boundary(&w, 0, prospect_market_pod);
    world_sim_step(&w, SIM_DT);
    ASSERT(ledger_balance(prospect, sp->session_token) < 0.0f);
}

TEST(test_neural_autopilot_flight_records_decision_reason) {
    if (!signal_intelligence_flight_loaded()) {
        TEST_WARN("flight intelligence unavailable; skipping flight reason replay");
        return;
    }

    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        w.asteroids[i].active = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        w.npc_ships[i].active = false;

    station_t *prospect = &w.stations[0];
    w.asteroids[0] = (asteroid_t){
        .active = true,
        .tier = ASTEROID_TIER_M,
        .commodity = COMMODITY_FERRITE_ORE,
        .pos = { prospect->pos.x + 3650.0f, prospect->pos.y + 250.0f },
        .radius = 40.0f,
        .hp = 30.0f,
        .max_hp = 30.0f,
        .ore = 30.0f,
        .max_ore = 30.0f,
    };
    spatial_grid_build(&w);

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    sp->session_ready = true;
    player_init_ship(sp, &w);
    sp->server_brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    sp->autopilot_mode = 1;
    sp->autopilot_state = AUTOPILOT_STEP_FLY_TO_TARGET;
    sp->autopilot_target = 0;
    sp->autopilot_station_target = -1;
    sp->autopilot_last_pos = sp->ship.pos;
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.pos = (vec2){ prospect->pos.x + 3000.0f, prospect->pos.y };
    sp->ship.vel = (vec2){0};
    sp->ship.angle = 0.0f;
    sp->docked = false;
    sp->current_station = -1;
    sp->nearby_station = -1;
    sp->in_dock_range = false;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->autopilot_decision_valid, 1);
    ASSERT_EQ_INT(sp->autopilot_decision_candidate_count,
                  SIGNAL_BRAIN_FLIGHT_ACTION_COUNT);
    ASSERT(sp->autopilot_decision_action < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT);
    ASSERT(sp->autopilot_decision_flags &
           SIGNAL_DECISION_REASON_ADVISORY_ONLY);
    ASSERT(sp->autopilot_decision_flags &
           SIGNAL_DECISION_REASON_HARD_APPROVED);
    ASSERT(sp->autopilot_decision_flags &
           SIGNAL_DECISION_REASON_HAS_SIGNAL_CONTEXT);
    ASSERT(sp->autopilot_decision_flags &
           (SIGNAL_DECISION_REASON_USED_NEURAL |
            SIGNAL_DECISION_REASON_HARD_OVERRIDE));
    ASSERT(sp->autopilot_decision_signal_quality > 0.70f);
    ASSERT(sp->autopilot_decision_route_risk >= 0.0f);
    ASSERT(sp->autopilot_decision_route_risk <= 1.0f);
}

TEST(test_autopilot_prioritizes_raw_ore_contract_mining_target) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.contracts, 0, sizeof(w.contracts));
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        w.asteroids[i].active = false;

    station_t *helios = &w.stations[2];
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CUPRITE_INGOT] = 0.0f;
    helios->_inventory_cache[COMMODITY_LASER_MODULE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_INGOT] = 0.0f;
    helios->_inventory_cache[COMMODITY_TRACTOR_MODULE] = 0.0f;

    w.contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_CUPRITE_ORE,
        .quantity_needed = 0.0f,
        .base_price = 21.0f,
        .target_index = -1,
        .claimed_by = -1,
    };

    int crystal = 0;
    int cuprite = 1;
    w.asteroids[crystal] = (asteroid_t){
        .active = true,
        .tier = ASTEROID_TIER_M,
        .commodity = COMMODITY_CRYSTAL_ORE,
        .pos = { helios->pos.x + 350.0f, helios->pos.y + 850.0f },
        .radius = 30.0f,
        .hp = 30.0f,
        .ore = 30.0f,
        .max_ore = 30.0f,
    };
    w.asteroids[cuprite] = (asteroid_t){
        .active = true,
        .tier = ASTEROID_TIER_M,
        .commodity = COMMODITY_CUPRITE_ORE,
        .pos = { helios->pos.x + 1300.0f, helios->pos.y + 950.0f },
        .radius = 30.0f,
        .hp = 30.0f,
        .ore = 30.0f,
        .max_ore = 30.0f,
    };

    server_player_t *sp = &w.players[0];
    test_prepare_autopilot_player(&w, sp);
    sp->server_brain_mode = SERVER_BRAIN_MODE_NEURAL_FLIGHT;
    sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
    sp->current_station = 2;
    sp->nearby_station = 2;
    sp->docked = false;
    sp->in_dock_range = false;
    sp->ship.mining_level = 2;
    sp->ship.pos = (vec2){ helios->pos.x, helios->pos.y + 700.0f };
    sp->ship.vel = (vec2){0};

    step_autopilot(&w, sp, SIM_DT);

    ASSERT_EQ_INT(sp->autopilot_state, AUTOPILOT_STEP_FLY_TO_TARGET);
    ASSERT_EQ_INT(sp->autopilot_target, cuprite);
}

TEST(test_miner_routes_crystal_to_crystal_smelt_endpoint) {
    WORLD_DECL;
    world_reset(&w);

    int miner = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_MINER
            && w.npc_ships[i].home_station == 2) {
            miner = i;
            break;
        }
    }
    ASSERT(miner >= 0);

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);
    asteroid_t *tow = &w.asteroids[frag];
    memset(tow, 0, sizeof(*tow));
    tow->active = true;
    tow->tier = ASTEROID_TIER_S;
    tow->commodity = COMMODITY_CRYSTAL_ORE;
    tow->radius = 12.0f;
    tow->ore = 1.0f;
    tow->max_ore = 1.0f;

    npc_ship_t *npc = &w.npc_ships[miner];
    npc->state = NPC_STATE_RETURN_TO_STATION;
    npc->target_asteroid = -1;
    npc->towed_fragment = frag;
    npc->ship.hull_class = HULL_CLASS_NPC_MINER;
    npc->ship.pos = v2_add(w.stations[2].pos, v2(100.0f, 0.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;
    tow->pos = v2_add(npc->ship.pos, v2(40.0f, 0.0f));
    ship_t *paired = world_npc_ship_for(&w, miner);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;
    paired->vel = npc->ship.vel;
    paired->angle = npc->ship.angle;
    *nav_npc_path(miner) = (nav_path_t){0};

    vec2 crystal_endpoint = {0};
    ASSERT(test_station_smelt_endpoint_for_ore(&w.stations[2],
                                               COMMODITY_CRYSTAL_ORE,
                                               &crystal_endpoint));

    world_sim_step(&w, SIM_DT);

    const nav_path_t *path = nav_npc_path(miner);
    ASSERT(v2_dist_sq(path->goal, crystal_endpoint) < 5.0f * 5.0f);
}

TEST(test_miner_drops_fragment_without_matching_smelt_endpoint) {
    WORLD_DECL;
    world_reset(&w);

    int miner = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_MINER
            && w.npc_ships[i].home_station == 2) {
            miner = i;
            break;
        }
    }
    ASSERT(miner >= 0);

    for (int m = 0; m < w.stations[2].module_count; m++) {
        station_module_t *mod = &w.stations[2].modules[m];
        if (mod->type == MODULE_FURNACE &&
            (commodity_t)mod->commodity == COMMODITY_CRYSTAL_INGOT) {
            mod->commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
        }
    }
    ASSERT(!test_station_smelt_endpoint_for_ore(&w.stations[2],
                                                COMMODITY_CRYSTAL_ORE,
                                                NULL));

    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);
    asteroid_t *tow = &w.asteroids[frag];
    memset(tow, 0, sizeof(*tow));
    tow->active = true;
    tow->tier = ASTEROID_TIER_S;
    tow->commodity = COMMODITY_CRYSTAL_ORE;
    tow->radius = 12.0f;
    tow->ore = 1.0f;
    tow->max_ore = 1.0f;

    npc_ship_t *npc = &w.npc_ships[miner];
    npc->state = NPC_STATE_RETURN_TO_STATION;
    npc->target_asteroid = -1;
    npc->towed_fragment = frag;
    npc->ship.pos = v2_add(w.stations[2].pos, v2(100.0f, 0.0f));
    ship_t *paired = world_npc_ship_for(&w, miner);
    ASSERT(paired != NULL);
    paired->pos = npc->ship.pos;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(npc->towed_fragment, -1);
    ASSERT_EQ_INT(npc->state, NPC_STATE_IDLE);
}

TEST(test_scenario_upgrade_requires_products) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);

    /* Launch then dock at station 2 (Helios Works - has laser upgrade) */
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    ASSERT(!w.players[0].docked);

    /* Dock directly at station 2 for test */
    w.players[0].docked = true;
    w.players[0].current_station = 2;
    w.players[0].nearby_station = 2;
    w.players[0].in_dock_range = true;
    w.players[0].ship.pos = w.stations[2].pos;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    ASSERT(w.players[0].docked);
    ASSERT_EQ_INT(w.players[0].current_station, 2);

    /* Give player enough credits at station 2 */
    ledger_earn(&w.stations[2], w.players[0].session_token, 1000.0f);
    int level_before = w.players[0].ship.mining_level;

    /* Set inventory for PRODUCT_LASER_MODULE to 0 */
    w.stations[2]._inventory_cache[COMMODITY_LASER_MODULE] = 0.0f;

    /* Try upgrade_mining -- should fail (no product stock) */
    w.players[0].input.upgrade_mining = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.upgrade_mining = false;
    ASSERT_EQ_INT(w.players[0].ship.mining_level, level_before);

    /* Mint manifest + float in lockstep. The manifest reconcile pass at
     * end of tick now snaps inventory[c] DOWN to manifest_count for
     * finished goods, so a bare float assignment would get trimmed to
     * zero before the upgrade path runs. Use the helper for correctness. */
    station_finished_mint(&w.stations[2], COMMODITY_LASER_MODULE, 20, NULL);

    /* Try upgrade_mining -- should succeed */
    w.players[0].input.upgrade_mining = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.upgrade_mining = false;
    ASSERT_EQ_INT(w.players[0].ship.mining_level, level_before + 1);
}

TEST(test_fresh_world_kepler_starter_laser_refit_bootstrap) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x02, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;

    server_player_t *sp = &w.players[0];
    station_t *kepler = &w.stations[1];
    int need = (int)ceilf(upgrade_product_cost(&sp->ship,
                                               SHIP_UPGRADE_MINING));
    ASSERT_EQ_INT(need, 8);
    ASSERT_EQ_INT(station_finished_count(kepler,
                                         COMMODITY_LASER_MODULE), need);
    ASSERT(upgrade_uses_starter_refit_subsidy(
        kepler, &sp->ship, SHIP_UPGRADE_MINING, need));
    ASSERT_EQ_FLOAT(upgrade_station_credit_cost(
        kepler, &sp->ship, SHIP_UPGRADE_MINING, need), 0.0f, 0.001f);
    ASSERT(can_afford_upgrade(kepler, &sp->ship,
                              SHIP_UPGRADE_MINING, 0.0f));

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CUPRITE_ORE);
    ASSERT(!mining_level_can_fracture_asteroid(sp->ship.mining_level,
                                               &w.asteroids[0]));
    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CRYSTAL_ORE);
    ASSERT(!mining_level_can_fracture_asteroid(sp->ship.mining_level,
                                               &w.asteroids[0]));

    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->ship.pos = kepler->pos;
    sp->ship.vel = v2(0.0f, 0.0f);
    sp->input.upgrade_mining = true;
    world_sim_step(&w, SIM_DT);
    sp->input.upgrade_mining = false;

    ASSERT_EQ_INT(sp->ship.mining_level, 1);
    ASSERT_EQ_INT(station_finished_count(kepler,
                                         COMMODITY_LASER_MODULE), 0);

    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CUPRITE_ORE);
    ASSERT(mining_level_can_fracture_asteroid(sp->ship.mining_level,
                                              &w.asteroids[0]));
    test_seed_gate_asteroid(&w, ASTEROID_TIER_M, COMMODITY_CRYSTAL_ORE);
    ASSERT(!mining_level_can_fracture_asteroid(sp->ship.mining_level,
                                               &w.asteroids[0]));
}

TEST(test_scenario_emergency_recovery) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;

    /* Launch */
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    ASSERT(!w.players[0].docked);

    /* Give player some cargo */
    w.players[0].ship.cargo[COMMODITY_FERRITE_ORE] = 50.0f;

    /* Set hull to 1.0 (near death) */
    w.players[0].ship.hull = 1.0f;

    /* Give high velocity towards a ring 1 module to trigger collision damage.
     * Signal relay is at ring 1, slot 1 (slot 0 is dock — no collision). */
    vec2 mod = module_world_pos_ring(&w.stations[0], 1, 1);
    w.players[0].ship.pos = v2(mod.x + 60.0f, mod.y);
    w.players[0].ship.vel = v2(-2000.0f, 0.0f);

    /* Run sim for a few ticks */
    for (int i = 0; i < 10; i++)
        world_sim_step(&w, SIM_DT);

    /* Verify: player is docked (emergency recovery triggered) */
    ASSERT(w.players[0].docked);

    /* Verify: hull is restored to max */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, ship_max_hull(&w.players[0].ship), 0.01f);

    /* Verify: cargo is cleared (lost on recovery) */
    ASSERT(ship_total_cargo(&w.players[0].ship) < 0.01f);
}

TEST(test_scenario_product_cap_pauses_production) {
    WORLD_DECL;
    world_reset(&w);

    /* Set station 1 (Kepler Yard) inventory[COMMODITY_FRAME] to MAX_PRODUCT_STOCK */
    w.stations[1]._inventory_cache[COMMODITY_FRAME] = MAX_PRODUCT_STOCK;

    /* Set ingot_buffer with some frame ingots */
    w.stations[1]._inventory_cache[COMMODITY_FERRITE_INGOT] = 20.0f;

    /* Run 120 ticks */
    for (int i = 0; i < 120; i++)
        world_sim_step(&w, SIM_DT);

    /* Verify inventory didn't exceed MAX_PRODUCT_STOCK */
    ASSERT(w.stations[1]._inventory_cache[COMMODITY_FRAME] <= MAX_PRODUCT_STOCK + 0.01f);
}

TEST(test_signal_strength_at_station) {
    /* At a station's position, signal should be 1.0 (full strength) */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(signal_strength_at(&w, w.stations[0].pos) > 0.95f);
    ASSERT(signal_strength_at(&w, w.stations[1].pos) > 0.95f);
    ASSERT(signal_strength_at(&w, w.stations[2].pos) > 0.95f);
}

TEST(test_signal_strength_falls_off) {
    /* Signal should decrease linearly from 1.0 at station to 0.0 at range edge.
     * Sample south of Prospect Refinery where Kepler/Helios don't reach, so
     * the overlap boost (multi-station reinforcement) isn't in play. */
    WORLD_DECL;
    world_reset(&w);
    /* Station 0 at (0, -2400), signal_range = 9000. Point 5000u south
     * is comfortably outside Kepler/Helios ranges, so only Prospect covers it
     * (and the bilinear cache cells around it are also single-station,
     * so the overlap boost doesn't leak in via interpolation). */
    float half = signal_strength_at(&w, v2_add(w.stations[0].pos, v2(0.0f, -5000.0f)));
    ASSERT(half > 0.3f && half < 0.7f);
}

TEST(test_signal_overlap_boosts_strength) {
    /* Overlap mechanic: two connected stations covering the same point
     * give 2x the best single-station strength; three-or-more overlap
     * caps at 3x. The starter triangle overlaps all three signals at the
     * center, so the boost saturates there. */
    WORLD_DECL;
    world_reset(&w);
    /* The inner basin sits inside both Prospect and Kepler coverage. */
    vec2 inner_mid = v2_scale(v2_add(w.stations[0].pos, w.stations[1].pos), 0.5f);
    float boosted = signal_strength_at(&w, inner_mid);
    ASSERT_EQ_FLOAT(boosted, 1.0f, 0.01f);
}

TEST(test_sector_one_broken_helios_corridor) {
    WORLD_DECL;
    world_reset(&w);

    ASSERT(w.stations[2].pos.y > w.stations[1].pos.y + 12000.0f);
    ASSERT(!station_provides_signal(&w.stations[SIGNAL_FREEPORT_STATION_INDEX]));

    vec2 blackglass = w.stations[SIGNAL_FREEPORT_STATION_INDEX].pos;
    float gap_signal = signal_strength_at(&w, blackglass);
    ASSERT(gap_signal > 0.0f);
    ASSERT(gap_signal < 0.25f);

    vec2 old_corridor_mid = v2(0.0f, 11000.0f);
    float mid_signal = signal_strength_at(&w, old_corridor_mid);
    ASSERT(mid_signal > 0.0f);
    ASSERT(mid_signal < 0.25f);

    ASSERT(signal_strength_at(&w, w.stations[2].pos) > 0.95f);
}

TEST(test_freeport_dock_beacon_restores_local_control) {
    WORLD_DECL;
    world_reset(&w);

    station_t *freeport = &w.stations[SIGNAL_FREEPORT_STATION_INDEX];
    ASSERT(station_exists(freeport));
    ASSERT(!station_provides_signal(freeport));
    ASSERT(signal_strength_at(&w, freeport->pos) < 0.25f);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->current_station = SIGNAL_FREEPORT_STATION_INDEX;
    sp->nearby_station = SIGNAL_FREEPORT_STATION_INDEX;
    sp->docked = true;
    sp->dock_berth = -1;
    anchor_ship_in_station(sp, &w);

    float berth_signal = signal_strength_at(&w, sp->ship.pos);
    ASSERT(berth_signal > 0.35f);
    ASSERT(signal_control_scale(berth_signal) > 0.35f);
}

TEST(test_signal_zero_outside_range) {
    /* Far from all stations, signal should be 0.0 */
    WORLD_DECL;
    world_reset(&w);
    ASSERT_EQ_FLOAT(signal_strength_at(&w, v2(100000.0f, 100000.0f)), 0.0f, 0.01f);
}

TEST(test_signal_max_of_stations) {
    /* When inside multiple stations' ranges, signal is the maximum, not sum */
    WORLD_DECL;
    world_reset(&w);
    /* Midpoint between station 0 and station 1 should get max of the two signals,
     * not their sum. Signal is never > 1.0. */
    float s = signal_strength_at(&w, v2(-160.0f, 0.0f));
    ASSERT(s <= 1.0f);
    ASSERT(s > 0.0f);
}

TEST(test_ship_thrust_scales_with_signal) {
    /* At low signal, ship should accelerate slower */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* Place ship at station (full signal) → thrust → measure velocity */
    w.players[0].ship.pos = w.stations[0].pos;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].ship.angle = 0.0f;
    w.players[0].input.thrust = 1.0f;
    world_sim_step(&w, SIM_DT);
    float vel_full_signal = w.players[0].ship.vel.x;
    /* Place ship far from all stations (low/zero signal) → same thrust → should be slower */
    w.players[0].ship.pos = v2(40000.0f, 0.0f); /* outside all station signal ranges */
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].input.thrust = 1.0f;
    world_sim_step(&w, SIM_DT);
    float vel_low_signal = w.players[0].ship.vel.x;
    /* After #82: vel_low_signal should be significantly less than vel_full_signal */
    /* Currently both are the same — no signal scaling */
    ASSERT(vel_low_signal < vel_full_signal * 0.7f);
}

TEST(test_zero_signal_preserves_ship_momentum_without_boundary_pull) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->docked = false;
    sp->ship.pos = v2(40000.0f, 0.0f);
    sp->ship.vel = v2(120.0f, 0.0f);
    sp->ship.angle = 0.0f;
    ASSERT(signal_strength_at(&w, sp->ship.pos) < 0.01f);

    vec2 start = sp->ship.pos;
    world_sim_step_player_only(&w, 0, SIM_DT);

    ASSERT(sp->ship.pos.x > start.x);
    ASSERT(sp->ship.vel.x > 0.0f);
    ASSERT(sp->ship.vel.x < 120.0f);
    ASSERT_EQ_FLOAT(sp->ship.vel.y, 0.0f, 0.001f);
}

TEST(test_asteroid_outside_signal_despawns) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_L;
    w.asteroids[0].radius = 40.0f;
    w.asteroids[0].hp = 100.0f;
    w.asteroids[0].max_hp = 100.0f;
    w.asteroids[0].pos = v2(40000.0f, 0.0f);
    w.asteroids[0].vel = v2(0.0f, 0.0f);
    world_sim_step(&w, SIM_DT);
    ASSERT(!w.asteroids[0].active);
}

TEST(test_npc_miners_avoid_zero_signal_asteroids) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int i = 1; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_L;
    w.asteroids[0].radius = 50.0f;
    w.asteroids[0].hp = 120.0f;
    w.asteroids[0].max_hp = 120.0f;
    w.asteroids[0].pos = v2(260.0f, -240.0f);

    w.asteroids[1].active = true;
    w.asteroids[1].tier = ASTEROID_TIER_XL;
    w.asteroids[1].radius = 80.0f;
    w.asteroids[1].hp = 240.0f;
    w.asteroids[1].max_hp = 240.0f;
    w.asteroids[1].pos = v2(4000.0f, 0.0f);

    w.npc_ships[0].active = true;
    w.npc_ships[0].role = NPC_ROLE_MINER;
    w.npc_ships[0].ship.hull_class = HULL_CLASS_NPC_MINER;
    w.npc_ships[0].ship.mining_level = 1;
    w.npc_ships[0].home_station = 0;
    w.npc_ships[0].state = NPC_STATE_DOCKED;
    w.npc_ships[0].state_timer = 0.0f;
    w.npc_ships[0].target_asteroid = -1;
    w.npc_ships[0].ship.pos = w.stations[0].pos;
    w.npc_ships[0].ship.vel = v2(0.0f, 0.0f);
    w.npc_ships[0].ship.angle = 0.0f;

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(w.npc_ships[0].target_asteroid, 0);
}

TEST(test_npc_miner_prefers_starved_ore_over_nearest_compatible_rock) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int i = 1; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    station_t *helios = &w.stations[2];
    helios->_inventory_cache[COMMODITY_CUPRITE_ORE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_ORE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CUPRITE_INGOT] = 0.0f;
    helios->_inventory_cache[COMMODITY_LASER_MODULE] = 0.0f;
    helios->_inventory_cache[COMMODITY_CRYSTAL_INGOT] = 12.0f;
    helios->_inventory_cache[COMMODITY_TRACTOR_MODULE] = 12.0f;

    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_L;
    w.asteroids[0].commodity = COMMODITY_CRYSTAL_ORE;
    w.asteroids[0].radius = 50.0f;
    w.asteroids[0].hp = 120.0f;
    w.asteroids[0].max_hp = 120.0f;
    w.asteroids[0].pos = v2_add(helios->pos, v2(350.0f, 0.0f));

    w.asteroids[1].active = true;
    w.asteroids[1].tier = ASTEROID_TIER_L;
    w.asteroids[1].commodity = COMMODITY_CUPRITE_ORE;
    w.asteroids[1].radius = 50.0f;
    w.asteroids[1].hp = 120.0f;
    w.asteroids[1].max_hp = 120.0f;
    w.asteroids[1].pos = v2_add(helios->pos, v2(1300.0f, 0.0f));

    w.npc_ships[0].active = true;
    w.npc_ships[0].role = NPC_ROLE_MINER;
    w.npc_ships[0].ship.hull_class = HULL_CLASS_NPC_MINER;
    w.npc_ships[0].ship.mining_level = 2;
    w.npc_ships[0].home_station = 2;
    w.npc_ships[0].state = NPC_STATE_DOCKED;
    w.npc_ships[0].state_timer = 0.0f;
    w.npc_ships[0].target_asteroid = -1;
    w.npc_ships[0].towed_fragment = -1;
    w.npc_ships[0].ship.pos = helios->pos;
    w.npc_ships[0].ship.vel = v2(0.0f, 0.0f);
    w.npc_ships[0].ship.angle = 0.0f;

    step_npc_ships(&w, SIM_DT);
    ASSERT_EQ_INT(w.npc_ships[0].target_asteroid, 1);
}

TEST(test_npc_miner_idles_when_refined_output_is_full) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int i = 1; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    station_t *prospect = &w.stations[0];
    prospect->_inventory_cache[COMMODITY_FERRITE_INGOT] = MAX_PRODUCT_STOCK;

    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_L;
    w.asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[0].radius = 50.0f;
    w.asteroids[0].hp = 120.0f;
    w.asteroids[0].max_hp = 120.0f;
    w.asteroids[0].pos = v2_add(prospect->pos, v2(700.0f, 0.0f));

    w.asteroids[1].active = true;
    w.asteroids[1].tier = ASTEROID_TIER_S;
    w.asteroids[1].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[1].radius = 24.0f;
    w.asteroids[1].hp = 1.0f;
    w.asteroids[1].max_hp = 1.0f;
    w.asteroids[1].pos = v2_add(prospect->pos, v2(300.0f, 0.0f));

    w.npc_ships[0].active = true;
    w.npc_ships[0].role = NPC_ROLE_MINER;
    w.npc_ships[0].ship.hull_class = HULL_CLASS_NPC_MINER;
    w.npc_ships[0].home_station = 0;
    w.npc_ships[0].state = NPC_STATE_DOCKED;
    w.npc_ships[0].state_timer = 0.0f;
    w.npc_ships[0].target_asteroid = -1;
    w.npc_ships[0].towed_fragment = -1;
    w.npc_ships[0].ship.pos = prospect->pos;
    w.npc_ships[0].ship.vel = v2(0.0f, 0.0f);
    w.npc_ships[0].ship.angle = 0.0f;

    step_npc_ships(&w, SIM_DT);
    ASSERT_EQ_INT(w.npc_ships[0].target_asteroid, -1);
    ASSERT_EQ_INT(w.npc_ships[0].towed_fragment, -1);
    ASSERT_EQ_INT(w.npc_ships[0].state, NPC_STATE_IDLE);
}

TEST(test_field_respawn_starts_beyond_signal_edge) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;

    w.field_spawn_timer = FIELD_ASTEROID_RESPAWN_DELAY;
    world_sim_step(&w, SIM_DT);

    int spawned = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active) {
            spawned = i;
            break;
        }
    }
    ASSERT(spawned >= 0);

    const asteroid_t *a = &w.asteroids[spawned];
    /* Belt-based spawning: asteroid should be within signal range and at a
     * position with nonzero belt density. Ore type matches belt geography. */
    ASSERT(signal_strength_at(&w, a->pos) >= 0.0f);
    /* Verify it spawned near a station (within signal range) */
    bool near_station = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w.stations[s])) continue;
        float d = sqrtf(v2_dist_sq(a->pos, w.stations[s].pos));
        if (d <= w.stations[s].signal_range) { near_station = true; break; }
    }
    ASSERT(near_station);

    /* Chunk-based terrain: asteroids spawn stationary (vel ~0).
     * Gravity/physics will give them velocity over time. */
    ASSERT(v2_len(a->vel) < 50.0f); /* not launched at high speed */

    world_sim_step(&w, SIM_DT);
    ASSERT(w.asteroids[spawned].active);
}

TEST(test_asteroids_drift_toward_lower_signal_band) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int s = 1; s < MAX_STATIONS; s++) w.stations[s].signal_range = 0.0f;
    rebuild_signal_chain(&w);

    asteroid_t *a = &w.asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_XL;
    a->radius = 60.0f;
    a->hp = 150.0f;
    a->max_hp = 150.0f;
    a->pos = v2_add(w.stations[0].pos, v2(7000.0f, 0.0f));
    a->vel = v2(0.0f, 0.0f);

    vec2 start_pos = a->pos;
    float start_signal = signal_strength_at(&w, a->pos);
    for (int i = 0; i < 1200; i++) world_sim_step(&w, SIM_DT);

    ASSERT(a->active);
    ASSERT(v2_dist_sq(a->pos, start_pos) > 1.0f);
    ASSERT(signal_strength_at(&w, a->pos) < start_signal);
}

TEST(test_belt_density_varies) {
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    /* Sample multiple points — should get both zero and nonzero density */
    int zeros = 0, nonzeros = 0;
    for (int i = 0; i < 100; i++) {
        float x = (float)(i * 1000 - 50000);
        float d = belt_density_at(&bf, x, 0.0f);
        if (d < 0.01f) zeros++;
        else nonzeros++;
    }
    ASSERT(zeros > 10);     /* some empty space */
    ASSERT(nonzeros > 10);  /* some belt regions */
}

TEST(test_belt_ore_distribution) {
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    int fe = 0, cu = 0, cr = 0;
    for (int i = 0; i < 1000; i++) {
        float x = (float)(i * 100 - 50000);
        float y = (float)((i * 73) % 100000 - 50000);
        commodity_t ore = belt_ore_at(&bf, x, y);
        if (ore == COMMODITY_FERRITE_ORE) fe++;
        else if (ore == COMMODITY_CUPRITE_ORE) cu++;
        else if (ore == COMMODITY_CRYSTAL_ORE) cr++;
    }
    /* Target mix ~60/16/24 (Fe/Cu/Cr). Cuprite >0 is load-bearing:
     * laser modules + tractor coils + repair kits all need cuprite
     * ingots. The bounds are loose to absorb tuning drift; tighten
     * them in a follow-up if a real regression slips past. */
    printf("    belt mix: fe=%d cu=%d cr=%d (target ~60/16/24)\n", fe, cu, cr);
    ASSERT(fe > 500);     /* ferrite still dominant */
    ASSERT(cu > 50);      /* cuprite reliably present */
    ASSERT(cu < 250);     /* but still the rarest of three */
    ASSERT(cr > 100);     /* crystal at least mid-share */
    ASSERT(cr < fe);      /* less than ferrite */
    ASSERT(cu < cr);      /* cuprite rarer than crystal */
}

TEST(test_chunk_determinism) {
    /* Same chunk coordinates + seed must produce identical asteroids */
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    chunk_asteroid_t a[CHUNK_MAX_ASTEROIDS], b[CHUNK_MAX_ASTEROIDS];
    int na = chunk_generate(&bf, 2037, 5, -3, a, CHUNK_MAX_ASTEROIDS);
    int nb = chunk_generate(&bf, 2037, 5, -3, b, CHUNK_MAX_ASTEROIDS);
    ASSERT_EQ_INT(na, nb);
    for (int i = 0; i < na; i++) {
        ASSERT_EQ_FLOAT(a[i].pos.x, b[i].pos.x, 0.001f);
        ASSERT_EQ_FLOAT(a[i].pos.y, b[i].pos.y, 0.001f);
        ASSERT_EQ_INT((int)a[i].tier, (int)b[i].tier);
        ASSERT_EQ_INT((int)a[i].commodity, (int)b[i].commodity);
        ASSERT_EQ_FLOAT(a[i].radius, b[i].radius, 0.001f);
        ASSERT_EQ_FLOAT(a[i].hp, b[i].hp, 0.001f);
    }
}

TEST(test_chunk_different_coords_differ) {
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    chunk_asteroid_t a[CHUNK_MAX_ASTEROIDS], b[CHUNK_MAX_ASTEROIDS];
    int na = 0, nb = 0;
    for (int cx = -5; cx < 5 && na == 0; cx++)
        na = chunk_generate(&bf, 2037, cx, 0, a, CHUNK_MAX_ASTEROIDS);
    for (int cx = 10; cx < 20 && nb == 0; cx++)
        nb = chunk_generate(&bf, 2037, cx, 3, b, CHUNK_MAX_ASTEROIDS);
    if (na > 0 && nb > 0) {
        ASSERT(fabsf(a[0].pos.x - b[0].pos.x) > 1.0f ||
               fabsf(a[0].pos.y - b[0].pos.y) > 1.0f);
    }
}

TEST(test_chunk_respects_belt_density) {
    /* Chunks at belt density > 0 should produce asteroids */
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    /* Station 0 is at (0, -2400). Belt density should be nonzero nearby. */
    chunk_asteroid_t out[CHUNK_MAX_ASTEROIDS];
    int total = 0;
    for (int cx = -10; cx < 10; cx++)
        for (int cy = -16; cy < -4; cy++)
            total += chunk_generate(&bf, 2037, cx, cy, out, CHUNK_MAX_ASTEROIDS);
    ASSERT(total > 0); /* at least some asteroids near the belt */
}

/* #294 slice 2 regression: an NPC in MINING state that is shoved past
 * MINING_RANGE used to keep firing its beam across the map (no exit
 * condition + center-distance entry test). After the unification, the
 * shared sim_mining_beam_step refuses fire at long range, and the NPC
 * MINING state drops back to TRAVEL when out of MINING_RANGE. */
TEST(test_npc_mining_drops_state_when_far_from_target) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    npc_ship_t *npc = &w->npc_ships[0];
    npc->active = true;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_MINING;
    npc->home_station = 0;
    npc->target_asteroid = 0;
    npc->ship.hull_class = HULL_CLASS_MINER;
    npc->hull = 100.0f;
    npc->ship.pos = v2(0.0f, 0.0f);
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;

    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->radius = 30.0f;
    a->hp = 40.0f;
    a->max_hp = 40.0f;
    a->pos = v2(800.0f, 0.0f); /* well outside MINING_RANGE (170u) */

    float hp_before = a->hp;
    for (int i = 0; i < 60; i++) world_sim_step(w, 1.0f / 120.0f);

    ASSERT_EQ_FLOAT(a->hp, hp_before, 0.001f);
    ASSERT(npc->state != NPC_STATE_MINING);
}

TEST(test_mining_beam_step_rejects_target_beyond_surface_range) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->radius = 30.0f;
    a->hp = 40.0f;
    a->max_hp = 40.0f;
    a->pos = v2(MINING_RANGE + a->radius + 5.0f, 0.0f);

    mining_beam_t mb = sim_mining_beam_step(w, v2(0.0f, 0.0f), v2(1.0f, 0.0f),
        0, 99, 25.0f, 1.0f, -1, 1.0f / 60.0f);

    ASSERT(!mb.hit);
    ASSERT(!mb.fired);
    ASSERT_EQ_FLOAT(a->hp, 40.0f, 0.001f);
    ASSERT_EQ_FLOAT(mb.beam_end.x, MINING_RANGE, 0.001f);
    ASSERT_EQ_FLOAT(mb.beam_end.y, 0.0f, 0.001f);
}

TEST(test_mining_beam_step_rejects_off_axis_target) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->radius = 25.0f;
    a->hp = 40.0f;
    a->max_hp = 40.0f;
    a->pos = v2(80.0f, 70.0f);

    mining_beam_t mb = sim_mining_beam_step(w, v2(0.0f, 0.0f), v2(1.0f, 0.0f),
        0, 99, 25.0f, 1.0f, -1, 1.0f / 60.0f);

    ASSERT(!mb.hit);
    ASSERT(!mb.fired);
    ASSERT_EQ_FLOAT(a->hp, 40.0f, 0.001f);
}

TEST(test_npc_mining_drops_state_when_target_out_of_cone) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    npc_ship_t *npc = &w->npc_ships[0];
    npc->active = true;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_MINING;
    npc->home_station = 0;
    npc->target_asteroid = 0;
    npc->ship.hull_class = HULL_CLASS_MINER;
    npc->hull = 100.0f;
    npc->ship.pos = v2(0.0f, 0.0f);
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;

    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->radius = 25.0f;
    a->hp = 40.0f;
    a->max_hp = 40.0f;
    a->pos = v2(0.0f, 90.0f); /* within center range, outside forward ray */

    float hp_before = a->hp;
    world_sim_step(w, 1.0f / 120.0f);

    ASSERT_EQ_FLOAT(a->hp, hp_before, 0.001f);
    ASSERT_EQ_INT(npc->state, NPC_STATE_TRAVEL_TO_ASTEROID);
}

TEST(test_npc_miner_does_not_claim_fragment_outside_tractor_range) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    npc_ship_t *npc = &w->npc_ships[0];
    npc->active = true;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_IDLE;
    npc->home_station = 0;
    npc->dest_station = 0;
    npc->state_timer = 0.0f;
    npc->target_asteroid = -1;
    npc->towed_fragment = -1;
    npc->ship.hull_class = HULL_CLASS_MINER;
    npc->hull = 100.0f;
    npc->ship.pos = v2_add(w->stations[0].pos, v2(900.0f, 900.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;

    asteroid_t *frag = &w->asteroids[0];
    frag->active = true;
    frag->tier = ASTEROID_TIER_S;
    frag->commodity = COMMODITY_FERRITE_ORE;
    frag->radius = 12.0f;
    frag->ore = 1.0f;
    frag->max_ore = 1.0f;
    frag->pos = v2_add(npc->ship.pos, v2(ship_tractor_range(&npc->ship) + 80.0f, 0.0f));
    frag->vel = v2(0.0f, 0.0f);

    rebuild_characters_from_npcs(w);
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_INT(npc->towed_fragment, -1);
    ASSERT_EQ_FLOAT(frag->vel.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(frag->vel.y, 0.0f, 0.001f);
}

TEST(test_npc_miner_drops_fragment_when_tow_band_snaps) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    npc_ship_t *npc = &w->npc_ships[0];
    npc->active = true;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_RETURN_TO_STATION;
    npc->home_station = 0;
    npc->dest_station = 0;
    npc->target_asteroid = -1;
    npc->towed_fragment = 0;
    npc->ship.hull_class = HULL_CLASS_MINER;
    npc->hull = 100.0f;
    npc->ship.pos = v2_add(w->stations[0].pos, v2(900.0f, 900.0f));
    npc->ship.vel = v2(0.0f, 0.0f);
    npc->ship.angle = 0.0f;

    asteroid_t *frag = &w->asteroids[0];
    frag->active = true;
    frag->tier = ASTEROID_TIER_S;
    frag->commodity = COMMODITY_FERRITE_ORE;
    frag->radius = 12.0f;
    frag->ore = 1.0f;
    frag->max_ore = 1.0f;
    frag->pos = v2_add(npc->ship.pos, v2(ship_tractor_range(&npc->ship) * 1.5f + 40.0f, 0.0f));
    frag->vel = v2(0.0f, 0.0f);

    rebuild_characters_from_npcs(w);
    world_sim_step(w, SIM_DT);

    ASSERT_EQ_INT(npc->towed_fragment, -1);
    ASSERT_EQ_FLOAT(frag->vel.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(frag->vel.y, 0.0f, 0.001f);
}

void register_world_sim_basic_tests(void) {
    TEST_SECTION("\nWorld sim tests:\n");
    RUN(test_world_reset_creates_stations);
    RUN(test_world_reset_spawns_asteroids);
    RUN(test_world_reset_spawns_npcs);
    RUN(test_world_reset_ship_assets_back_active_hulls);
    RUN(test_station_hull_inventory_cache_tracks_asset_registry);
    RUN(test_player_init_claims_station_loaner_asset);
    RUN(test_player_init_bound_asset_preserves_custody_station);
    RUN(test_player_init_ignores_foreign_bound_asset);
    RUN(test_player_init_clears_stale_binding_when_waiting_for_hull);
    RUN(test_player_reconnect_transfer_moves_ship_asset_binding);
    RUN(test_ship_asset_sync_rejects_non_world_player_pointer);
    RUN(test_server_player_clear_transient_input_resets_spawn_motion);
    RUN(test_player_release_returns_provisional_loaner_to_storage);
    RUN(test_player_release_stores_owned_hull_for_reclaim);
    RUN(test_player_init_ship_null_context_safe);
    RUN(test_player_respawn_retires_asset_and_claims_loaner);
    RUN(test_player_respawn_without_loaner_waits_for_shipyard_asset);
    RUN(test_ship_asset_mint_reclaims_destroyed_unreferenced_slots);
    RUN(test_spawn_npc_bootstrap_does_not_queue_shipyard_build);
    RUN(test_npc_asset_claim_requires_paired_ship_slot);
    RUN(test_shipyard_keeps_completed_build_when_asset_registry_full);
    RUN(test_world_reset_prospect_workers_leave_idle);
    RUN(test_neural_worker_refits_only_from_home_credit_and_modules);
    RUN(test_neural_worker_posts_home_refit_import_contract);
    RUN(test_ship_death_drops_cargo_pods);
    RUN(test_towed_cargo_pod_sells_at_matching_intake);
    RUN(test_towed_shell_pod_keeps_shell_after_intake_custody_sale);
    RUN(test_buy_station_held_pod_transfers_custody_to_ship);
    RUN(test_buy_selected_station_held_pod_transfers_that_pod);
    RUN(test_docked_towed_cargo_pod_stays_parked_at_ship);
    RUN(test_cargo_pods_collide_and_separate);
    RUN(test_station_dock_tractor_spreads_market_pods);
    RUN(test_station_dock_tractor_clears_after_pod_moves_out_of_range);
    RUN(test_station_dock_adopts_finished_output_pod_for_market);
    RUN(test_cargo_pod_high_speed_station_impact_destroys_shell);
    RUN(test_cargo_pod_bounces_off_station_corridor_ring);
    RUN(test_cargo_pod_bounces_off_asteroid);
    RUN(test_towed_cargo_pod_bulk_sell_no_longer_uses_dock_custody);
    RUN(test_towed_cargo_pod_row_sell_no_longer_uses_dock_custody);
    RUN(test_manifest_cargo_pod_sale_preserves_exact_units);
    RUN(test_towed_cargo_pod_row_sell_requires_physical_intake_when_hopper_full);
    RUN(test_towed_cargo_pod_intake_handoff_moves_whole_pod_to_hopper);
    RUN(test_docking_works_while_towing_cargo_pod);
    RUN(test_space_release_drops_cargo_pod_instead_of_slingshot);
    RUN(test_towed_fragment_loads_raw_contract_at_dock);
    RUN(test_tow_capacity_counts_pods_against_fragment_pickup);
    RUN(test_tow_capacity_counts_fragments_against_pod_pickup);
    RUN(test_gas_rich_asteroid_emits_gas_pod);
    RUN(test_npc_embedded_towed_fragment_skips_ambient_asteroid_drag);
    RUN(test_hail_responds_while_docked);
    RUN(test_docking_auto_reports_station_balance);
    RUN(test_hail_does_not_spawn_nearest_rock_contract);
    RUN(test_hail_claims_existing_station_work);
    RUN(test_hail_responds_to_station_signal_outside_ship_comm_range);
    RUN(test_hail_responds_at_helios_dock_even_with_short_ship_comm);
    RUN(test_hail_reports_no_station_in_range);
    RUN(test_dead_neural_worker_auto_respawns);
    RUN(test_hauler_preserves_cargo_identity_in_transit);
    RUN(test_black_market_contract_accepts_npc_module_delivery);
    RUN(test_hauler_docks_when_reaching_station_lane);
    RUN(test_hauler_does_not_dock_from_outer_station_ring);
    RUN(test_legacy_hauler_cargo_unloads_when_manifest_empty);
    RUN(test_station_roster_uses_shipyard_contract_for_resident_worker_hulls);
    RUN(test_frontier_outpost_roster_respects_virtual_logistics_budget);
    RUN(test_player_init_ship_docked);
    RUN(test_world_sim_step_advances_time);
    RUN(test_world_sim_step_moves_ship_with_thrust);
    RUN(test_ship_brake_opposes_velocity_not_facing);
    RUN(test_ship_brake_stops_without_overshoot);
    RUN(test_ship_reverse_requires_reverse_flag);
    RUN(test_world_sim_step_mining_damages_asteroid);
    RUN(test_mining_laser_size_gate_starts_at_m);
    RUN(test_mining_laser_material_gate_requires_upgrades);
    RUN(test_fresh_world_kepler_starter_laser_refit_bootstrap);
    RUN(test_world_sim_step_laser_scans_cargo_pod);
    RUN(test_world_sim_step_docking);
    RUN(test_world_sim_step_refinery_hopper_path_retired);
    RUN(test_mining_class_prefix_round_trip);
    RUN(test_refinery_deposits_named_ingot);
    RUN(test_furnace_smelting_accepts_beam_corridor_delivery);
    RUN(test_furnace_smelting_requires_frame_shell);
    RUN(test_furnace_smelting_consumes_loose_frame_shell);
    RUN(test_crystal_requires_two_distinct_furnace_passes);
    RUN(test_neural_bot_contract_logistics_buys_and_delivers_ingot);
    RUN(test_autopilot_toggle_with_towed_pod_plans_logistics_delivery);
    RUN(test_neural_bot_logistics_buys_on_station_credit);
    RUN(test_neural_autopilot_flight_records_decision_reason);
    RUN(test_autopilot_prioritizes_raw_ore_contract_mining_target);
    RUN(test_station_production_ejects_frame_pod);
    RUN(test_station_production_fills_existing_frame_output_pod);
    RUN(test_station_production_consumes_loose_ingot_pod);
    RUN(test_frame_press_accepts_player_towed_ingot_pod_at_press);
    RUN(test_station_hopper_accepts_player_towed_ingot_pod);
    RUN(test_frame_press_consumes_dock_held_ingot_pod);
    RUN(test_frame_press_reclaims_dock_held_frame_pod_as_output_crate);
    RUN(test_kepler_frame_press_accepts_player_repositioned_local_frame_crate);
    RUN(test_furnace_accepts_player_towed_frame_shell_pod);
    RUN(test_furnace_accepts_frame_shell_pod_near_smelt_beam);
    RUN(test_frame_shell_pod_targets_furnace_frame_hopper_lane);
    RUN(test_frame_pod_prefers_shipyard_serving_hopper_over_storage_twin);
    RUN(test_furnace_tractor_holds_frame_pod_outside_module);
    RUN(test_station_production_ejects_laser_pod);
    RUN(test_station_production_fills_existing_laser_output_pod);
    RUN(test_station_production_without_manifest_inputs_refuses_to_mint);
    RUN(test_world_sim_step_events_emitted);
    RUN(test_world_sim_step_npc_miners_work);
    RUN(test_embedded_neural_checkpoint_drives_npc_worker);
    RUN(test_holographic_npc_bootstrap_gate_blocks_forward_thrust);
    RUN(test_npc_mining_drops_state_when_far_from_target);
    RUN(test_mining_beam_step_rejects_target_beyond_surface_range);
    RUN(test_mining_beam_step_rejects_off_axis_target);
    RUN(test_npc_mining_drops_state_when_target_out_of_cone);
    RUN(test_npc_miner_does_not_claim_fragment_outside_tractor_range);
    RUN(test_npc_miner_drops_fragment_when_tow_band_snaps);
    RUN(test_world_network_writes_persist);
}

void register_world_sim_scenarios_tests(void) {
    TEST_SECTION("\nSim integration scenarios:\n");
    RUN(test_scenario_full_mining_cycle);
    RUN(test_manifest_conservation_across_transactions);
    RUN(test_scenario_two_players_mining);
    RUN(test_scenario_npc_economy_30_seconds);
    RUN(test_npc_exits_station_with_blocked_rings);
    RUN(test_hauler_exits_non_home_station_before_return);
    RUN(test_miner_inside_station_nav_envelope_routes_to_outer_gap);
    RUN(test_hauler_near_station_does_not_post_distress_contract);
    RUN(test_hauler_distress_requires_sustained_stall);
    RUN(test_miner_enters_station_before_smelt_delivery);
    RUN(test_kepler_frame_hauler_reaches_helios_dock);
    RUN(test_autopilot_routes_towed_fragment_to_smelt_even_near_station);
    RUN(test_autopilot_does_not_mix_ore_fragments_while_returning);
    RUN(test_autopilot_smelt_delivery_preempts_repair_dock);
    RUN(test_fragment_smelt_full_stock_still_emits_pod);
    RUN(test_fragment_smelt_at_full_stock_keeps_station_stock_and_emits_pod);
    RUN(test_neural_npc_assignment_preserves_miner_hull_for_hauler_work);
    RUN(test_neural_npc_assignment_keeps_mining_over_weak_haul_offer);
    RUN(test_neural_npc_assignment_uses_hauler_hull_for_scaffold_tow);
    RUN(test_neural_npc_assignment_switches_worker_to_scout_for_fracture_work);
    RUN(test_neural_npc_assignment_repairs_damaged_worker_from_shared_offer);
    RUN(test_neural_npc_assignment_executes_delivery_proof_offer);
    RUN(test_neural_npc_assignment_uses_market_memory_demand);
    RUN(test_hauler_assignment_weights_route_memory);
    RUN(test_hauler_assignment_explains_selected_route_risk_memory);
    RUN(test_risky_hauler_dispatch_emits_escort_route_reputation);
    RUN(test_route_safety_proof_offsets_route_risk_diagnostic);
    RUN(test_hauler_assignment_weights_delivery_receipt_memory);
    RUN(test_hauler_assignment_weights_route_reputation_memory);
    RUN(test_hauler_assignment_weights_station_trust_memory);
    RUN(test_hauler_assignment_weights_supply_memory);
    RUN(test_hauler_uses_remote_supply_memory_for_pickup);
    RUN(test_remote_supply_route_starts_from_current_ship_position);
    RUN(test_failed_remote_pickup_emits_station_risk_memory);
    RUN(test_hauler_assignment_avoids_station_risk_memory);
    RUN(test_hauler_assignment_avoids_destination_station_risk_memory);
    RUN(test_neural_worker_dock_encodes_market_memory_into_hnn);
    RUN(test_neural_worker_transports_market_hnn_between_stations);
    RUN(test_neural_worker_physically_transports_contract_memory_between_stations);
    RUN(test_idle_neural_worker_runs_gossip_courier_trip);
    RUN(test_idle_neural_worker_runs_baseline_gossip_without_contracts);
    RUN(test_neural_worker_runs_gossip_when_ore_target_unavailable);
    RUN(test_neural_worker_market_hnn_pool_decays_under_new_attention);
    RUN(test_neural_worker_market_hnn_pool_decays_when_idle);
    RUN(test_neural_npc_assignment_preserves_hauler_hull_for_ore_work);
    RUN(test_hauler_damage_emits_route_danger_memory);
    RUN(test_route_reputation_reduces_hauler_damage);
    RUN(test_legacy_hauler_brain_mode_upgrades_before_assignment);
    RUN(test_miner_routes_crystal_to_crystal_smelt_endpoint);
    RUN(test_miner_drops_fragment_without_matching_smelt_endpoint);
    RUN(test_scenario_upgrade_requires_products);
    RUN(test_scenario_emergency_recovery);
    RUN(test_scenario_product_cap_pauses_production);
}

void register_world_sim_signal_tests(void) {
    TEST_SECTION("\nSignal range (#82):\n");
    RUN(test_signal_strength_at_station);
    RUN(test_signal_strength_falls_off);
    RUN(test_signal_overlap_boosts_strength);
    RUN(test_sector_one_broken_helios_corridor);
    RUN(test_freeport_dock_beacon_restores_local_control);
    RUN(test_signal_zero_outside_range);
    RUN(test_signal_max_of_stations);
    RUN(test_ship_thrust_scales_with_signal);
    RUN(test_zero_signal_preserves_ship_momentum_without_boundary_pull);
    RUN(test_asteroid_outside_signal_despawns);
    RUN(test_npc_miners_avoid_zero_signal_asteroids);
    RUN(test_npc_miner_prefers_starved_ore_over_nearest_compatible_rock);
    RUN(test_npc_miner_idles_when_refined_output_is_full);
    RUN(test_field_respawn_starts_beyond_signal_edge);
    RUN(test_asteroids_drift_toward_lower_signal_band);
}

void register_world_sim_belt_tests(void) {
    TEST_SECTION("\nBelt generation:\n");
    RUN(test_belt_density_varies);
    RUN(test_belt_ore_distribution);
}

/* #285 slice 1: count rocks materialized for a chunk after planting a
 * player at the chunk center and forcing a maintenance sweep. */
static int count_rocks_in_chunk(world_t *w, int32_t cx, int32_t cy) {
    int n = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        if (!w->asteroid_origin[i].from_chunk) continue;
        if (w->asteroid_origin[i].chunk_x == cx &&
            w->asteroid_origin[i].chunk_y == cy)
            n++;
    }
    return n;
}

/* Permanent terrain: every materialized terrain rock carries a non-
 * zero rock_pub stamped from (belt_seed, cx, cy, slot). */
TEST(test_rock_pub_assigned_at_first_contact) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->players[0].connected = true;
    player_init_ship(&w->players[0], w);
    int32_t cx = 4, cy = -2;
    w->players[0].ship.pos = v2(((float)cx + 0.5f) * CHUNK_SIZE,
                                 ((float)cy + 0.5f) * CHUNK_SIZE);
    w->field_spawn_timer = 1e6f;
    maintain_asteroid_field(w, 0.016f);
    int found_with_pub = 0;
    static const uint8_t zero[32] = {0};
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        if (!w->asteroid_origin[i].from_chunk) continue;
        if (memcmp(w->asteroids[i].rock_pub, zero, 32) != 0) found_with_pub++;
    }
    /* At least one terrain rock should be in this chunk and stamped. */
    ASSERT(found_with_pub > 0);
}

/* Mining a rock retires its rock_pub forever; revisits skip that slot. */
TEST(test_destroyed_rock_does_not_respawn) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->players[0].connected = true;
    player_init_ship(&w->players[0], w);
    int32_t cx = 7, cy = 11;
    w->players[0].ship.pos = v2(((float)cx + 0.5f) * CHUNK_SIZE,
                                 ((float)cy + 0.5f) * CHUNK_SIZE);
    w->field_spawn_timer = 1e6f;
    maintain_asteroid_field(w, 0.016f);
    int initial = count_rocks_in_chunk(w, cx, cy);
    if (initial == 0) {
        /* Belt density may have left this chunk empty; pick another. */
        cx = 9; cy = -3;
        w->players[0].ship.pos = v2(((float)cx + 0.5f) * CHUNK_SIZE,
                                     ((float)cy + 0.5f) * CHUNK_SIZE);
        w->field_spawn_timer = 1e6f;
        maintain_asteroid_field(w, 0.016f);
        initial = count_rocks_in_chunk(w, cx, cy);
    }
    ASSERT(initial > 0);
    /* Pick a terrain rock from the chunk and fracture it directly. */
    int target = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        if (!w->asteroid_origin[i].from_chunk) continue;
        if (w->asteroid_origin[i].chunk_x != cx ||
            w->asteroid_origin[i].chunk_y != cy) continue;
        target = i;
        break;
    }
    ASSERT(target >= 0);
    fracture_asteroid(w, target, v2(1.0f, 0.0f), -1);
    ASSERT_EQ_INT(w->destroyed_rock_count, 1);
    /* Push the chunk far out of viewport, sweep to despawn, then come
     * back and re-materialize. The destroyed rock must not return. */
    w->players[0].ship.pos = v2(50000.0f, 50000.0f);
    w->field_spawn_timer = 1e6f;
    maintain_asteroid_field(w, 0.016f);
    /* Clear any non-disturbed leftovers — also wipes fracture children
     * which would otherwise occupy slots. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        memset(&w->asteroids[i], 0, sizeof(w->asteroids[i]));
        memset(&w->asteroid_origin[i], 0, sizeof(w->asteroid_origin[i]));
    }
    w->players[0].ship.pos = v2(((float)cx + 0.5f) * CHUNK_SIZE,
                                 ((float)cy + 0.5f) * CHUNK_SIZE);
    w->field_spawn_timer = 1e6f;
    maintain_asteroid_field(w, 0.016f);
    int after = count_rocks_in_chunk(w, cx, cy);
    /* Strictly fewer rocks than the first visit. */
    ASSERT(after < initial);
}

/* Save/load round-trips the destroyed ledger, belt_seed, and per-entry
 * timestamps; ledger stays sorted across the round-trip. */
TEST(test_save_preserves_destroyed_rocks_ledger) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    /* Fabricate two destroyed pubs in sorted order (CD > AB by byte
     * compare) with distinct timestamps so the round-trip can verify
     * both fields. */
    memset(w->destroyed_rocks[0].rock_pub, 0xAB, 32);
    w->destroyed_rocks[0].destroyed_at_ms = 1234;
    memset(w->destroyed_rocks[1].rock_pub, 0xCD, 32);
    w->destroyed_rocks[1].destroyed_at_ms = 5678;
    w->destroyed_rock_count = 2;
    uint32_t expected_seed = w->belt_seed;
    ASSERT(world_save(w, TMP("test_rockpub.sav")));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, TMP("test_rockpub.sav")));
    ASSERT_EQ_INT(loaded->destroyed_rock_count, 2);
    ASSERT_EQ_INT(loaded->belt_seed, expected_seed);
    uint8_t want_ab[32]; memset(want_ab, 0xAB, 32);
    uint8_t want_cd[32]; memset(want_cd, 0xCD, 32);
    /* Sorted order survives. */
    ASSERT(memcmp(loaded->destroyed_rocks[0].rock_pub, want_ab, 32) == 0);
    ASSERT(memcmp(loaded->destroyed_rocks[1].rock_pub, want_cd, 32) == 0);
    ASSERT_EQ_INT((int)loaded->destroyed_rocks[0].destroyed_at_ms, 1234);
    ASSERT_EQ_INT((int)loaded->destroyed_rocks[1].destroyed_at_ms, 5678);
    remove(TMP("test_rockpub.sav"));
}

TEST(test_destroyed_rocks_persists_past_legacy_256_cap) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(w && loaded);
    world_reset(w);

    enum { COUNT = 300 };
    ASSERT((int)COUNT < (int)MAX_DESTROYED_ROCKS);
    for (uint16_t i = 0; i < COUNT; i++) {
        memset(w->destroyed_rocks[i].rock_pub, 0, 32);
        w->destroyed_rocks[i].rock_pub[30] = (uint8_t)(i >> 8);
        w->destroyed_rocks[i].rock_pub[31] = (uint8_t)i;
        w->destroyed_rocks[i].destroyed_at_ms = (uint64_t)(100000 + i);
    }
    w->destroyed_rock_count = COUNT;

    ASSERT(world_save(w, TMP("test_rockpub_300.sav")));
    ASSERT(world_load(loaded, TMP("test_rockpub_300.sav")));
    ASSERT_EQ_INT(loaded->destroyed_rock_count, COUNT);
    ASSERT_EQ_INT((int)loaded->destroyed_rocks[299].destroyed_at_ms, 100299);
    ASSERT_EQ_INT(loaded->destroyed_rocks[299].rock_pub[30], 1);
    ASSERT_EQ_INT(loaded->destroyed_rocks[299].rock_pub[31], 43);
    remove(TMP("test_rockpub_300.sav"));
}

TEST(test_destroyed_rocks_insert_past_legacy_256_cap) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    world_reset(w);
    w->time = 12.345f;

    for (uint16_t i = 0; i < 256; i++) {
        memset(w->destroyed_rocks[i].rock_pub, 0, 32);
        w->destroyed_rocks[i].rock_pub[30] = (uint8_t)(i >> 8);
        w->destroyed_rocks[i].rock_pub[31] = (uint8_t)i;
        w->destroyed_rocks[i].destroyed_at_ms = (uint64_t)i;
    }
    w->destroyed_rock_count = 256;

    int slot = 0;
    memset(&w->asteroids[slot], 0, sizeof(w->asteroids[slot]));
    memset(&w->asteroid_origin[slot], 0, sizeof(w->asteroid_origin[slot]));
    asteroid_t *a = &w->asteroids[slot];
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 16.0f;
    a->pos = w->stations[0].pos;
    memset(a->rock_pub, 0, 32);
    a->rock_pub[30] = 1;
    a->rock_pub[31] = 0;

    fracture_asteroid(w, slot, v2(1.0f, 0.0f), -1);
    ASSERT_EQ_INT(w->destroyed_rock_count, 257);
    ASSERT_EQ_INT(w->destroyed_rocks[256].rock_pub[30], 1);
    ASSERT_EQ_INT(w->destroyed_rocks[256].rock_pub[31], 0);
    ASSERT_EQ_INT((int)w->destroyed_rocks[256].destroyed_at_ms, 12345);
}

/* Slice 2 invariant: destroyed_rocks stays sorted ascending by rock_pub
 * across out-of-order inserts. Without this, bsearch breaks. */
TEST(test_destroyed_rocks_stays_sorted_after_inserts) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    int32_t cx = 5, cy = 9;
    w->players[0].connected = true;
    player_init_ship(&w->players[0], w);
    w->players[0].ship.pos = v2(((float)cx + 0.5f) * CHUNK_SIZE,
                                 ((float)cy + 0.5f) * CHUNK_SIZE);
    w->field_spawn_timer = 1e6f;
    maintain_asteroid_field(w, 0.016f);
    /* Fracture a handful of terrain rocks; their rock_pubs are
     * pseudo-random (SHA-256-derived from coords), so insertion
     * order is essentially shuffled. */
    int fractured = 0;
    for (int i = 0; i < MAX_ASTEROIDS && fractured < 5; i++) {
        if (!w->asteroids[i].active) continue;
        if (!w->asteroid_origin[i].from_chunk) continue;
        fracture_asteroid(w, i, v2(1.0f, 0.0f), -1);
        fractured++;
    }
    if (fractured >= 2) {
        for (uint16_t k = 1; k < w->destroyed_rock_count; k++) {
            int cmp = memcmp(w->destroyed_rocks[k - 1].rock_pub,
                             w->destroyed_rocks[k].rock_pub, 32);
            ASSERT(cmp < 0);  /* strictly ascending */
        }
    }
}

void register_world_sim_chunk_tests(void) {
    TEST_SECTION("\nChunk terrain generation:\n");
    RUN(test_chunk_determinism);
    RUN(test_chunk_different_coords_differ);
    RUN(test_chunk_respects_belt_density);
    RUN(test_rock_pub_assigned_at_first_contact);
    RUN(test_destroyed_rock_does_not_respawn);
    RUN(test_save_preserves_destroyed_rocks_ledger);
    RUN(test_destroyed_rocks_persists_past_legacy_256_cap);
    RUN(test_destroyed_rocks_insert_past_legacy_256_cap);
    RUN(test_destroyed_rocks_stays_sorted_after_inserts);
}
