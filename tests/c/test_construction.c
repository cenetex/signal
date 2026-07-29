#include "test_harness.h"
#include "actor_principal_resolver.h"
#include "chain_log.h"
#include "cargo_receipt_issue.h"
#include "cargo_receipt_trust.h"
#include "mining.h"
#include "sim_physics.h"
#include "signal_intelligence.h"

static int construction_count_active_npcs(const world_t *w) {
    int count = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w->npc_ships[i].active) count++;
    }
    return count;
}

static int construction_count_active_ship_assets(const world_t *w) {
    int count = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (w->ship_assets[i].active && !w->ship_assets[i].destroyed) count++;
    }
    return count;
}

static bool construction_bytes_any(const uint8_t bytes[32]) {
    for (int i = 0; i < 32; i++)
        if (bytes[i] != 0) return true;
    return false;
}

static bool construction_make_verified_player(server_player_t *sp,
                                              uint8_t discriminator) {
    if (!sp) return false;
    sp->connected = true;
    sp->grace_period = false;
    sp->session_ready = true;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_identity_finalized = true;
    for (size_t i = 0; i < sizeof(sp->pubkey); i++)
        sp->pubkey[i] = (uint8_t)(discriminator + i);
    return actor_principal_from_verified_player(
        sp, &(actor_principal_t){0});
}

static void construction_clear_pending_hull_queues(world_t *w) {
    if (!w) return;
    for (int station_idx = 0; station_idx < MAX_STATIONS; station_idx++) {
        station_t *station = &w->stations[station_idx];
        memset(station->pending_ship_builds, 0,
               sizeof(station->pending_ship_builds));
        station->pending_ship_build_count = 0;
        memset(w->ship_birth_assemblies[station_idx], 0,
               sizeof(w->ship_birth_assemblies[station_idx]));
    }
}

/*
 * Build a bounded receipt-backed manifest fixture for narrow construction
 * tests that start after cargo acquisition. The player-facing end-to-end
 * coverage below deliberately does not use this helper: normal market pods
 * cross the production PRESENT/UNPACK bridge instead.
 */
static bool construction_attach_local_ship_receipts(
    world_t *w, int station_idx, server_player_t *sp) {
    if (!w || !sp || !sp->ship ||
        station_idx < 0 || station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS) {
        return false;
    }
    size_t unit_count = sp->ship->manifest.count;
    if (unit_count == 0 ||
        unit_count > CHAIN_LOG_BATCH_MAX_EVENTS) {
        return false;
    }
    cargo_unit_t *units[CHAIN_LOG_BATCH_MAX_EVENTS] = {0};
    for (size_t i = 0; i < unit_count; i++)
        units[i] = &sp->ship->manifest.units[i];
    if (!world_anchor_legacy_cargo_origins(
            w, station_idx, units, unit_count)) {
        return false;
    }

    station_t *station = &w->stations[station_idx];
    if (station->chain_event_count >
        UINT64_MAX - (uint64_t)unit_count) {
        return false;
    }
    cargo_store_t staged = {0};
    if (!cargo_store_clone(
            &staged, &sp->ship->cargo_store)) {
        return false;
    }
    ship_receipts_t *staged_receipts =
        cargo_store_receipts(&staged);
    if (!staged_receipts ||
        staged_receipts->count != staged.manifest.count) {
        cargo_store_cleanup(&staged);
        return false;
    }

    chain_payload_transfer_t
        payloads[CHAIN_LOG_BATCH_MAX_EVENTS];
    chain_log_batch_event_t
        events[CHAIN_LOG_BATCH_MAX_EVENTS];
    memset(payloads, 0, sizeof(payloads));
    memset(events, 0, sizeof(events));
    uint64_t first_event_id = station->chain_event_count + 1u;
    uint64_t epoch = (uint64_t)(w->time * 120.0);
    for (size_t i = 0; i < unit_count; i++) {
        cargo_receipt_chain_t *incoming =
            &staged_receipts->chains[i];
        cargo_receipt_transfer_link_t link =
            cargo_receipt_prepare_transfer_link(
                station, station->station_pubkey,
                staged.manifest.units[i].pub,
                incoming);
        if (link.status !=
                CARGO_RECEIPT_TRANSFER_LINK_READY ||
            incoming->len >=
                CARGO_RECEIPT_CHAIN_MAX_LEN) {
            cargo_store_cleanup(&staged);
            return false;
        }
        cargo_receipt_t receipt = {0};
        if (!cargo_receipt_issue(
                station, epoch,
                first_event_id + (uint64_t)i,
                staged.manifest.units[i].pub,
                sp->pubkey, link.prev_receipt_hash,
                &receipt)) {
            cargo_store_cleanup(&staged);
            return false;
        }
        incoming->links[incoming->len++] = receipt;

        chain_payload_transfer_t *payload =
            &payloads[i];
        memcpy(payload->from_pubkey,
               station->station_pubkey, 32);
        memcpy(payload->to_pubkey, sp->pubkey, 32);
        memcpy(payload->cargo_pub,
               staged.manifest.units[i].pub, 32);
        payload->kind = staged.manifest.units[i].kind;
        events[i] = (chain_log_batch_event_t){
            .type = CHAIN_EVT_TRANSFER,
            .payload = payload,
            .payload_len = (uint16_t)sizeof(*payload),
        };
    }

    chain_log_append_result_t appended =
        chain_log_emit_batch(
            w, station, events, unit_count);
    if (appended.status != CHAIN_LOG_APPEND_OK ||
        appended.event_count != (uint16_t)unit_count ||
        appended.first_event_id != first_event_id) {
        cargo_store_cleanup(&staged);
        return false;
    }
    cargo_store_cleanup(&sp->ship->cargo_store);
    sp->ship->cargo_store = staged;
    memset(&staged, 0, sizeof(staged));
    return true;
}

static bool construction_skip_chain_events(FILE *f, uint64_t count) {
    if (!f) return false;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t header[CHAIN_EVENT_HEADER_SIZE];
        uint8_t len_bytes[2];
        if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
            fread(len_bytes, 1, sizeof(len_bytes), f) !=
                sizeof(len_bytes)) {
            return false;
        }
        uint16_t payload_len = (uint16_t)len_bytes[0] |
            (uint16_t)((uint16_t)len_bytes[1] << 8);
        if (fseek(f, (long)payload_len, SEEK_CUR) != 0)
            return false;
    }
    return true;
}

typedef struct {
    uint16_t calls;
    cargo_receipt_chain_t chains[8];
} construction_receipt_sink_capture_t;

static void construction_capture_receipt_chain(
    void *user, const cargo_receipt_chain_t *chain) {
    construction_receipt_sink_capture_t *capture =
        (construction_receipt_sink_capture_t *)user;
    if (!capture || !chain) return;
    if (capture->calls <
        (uint16_t)(sizeof(capture->chains) /
                   sizeof(capture->chains[0]))) {
        capture->chains[capture->calls] = *chain;
    }
    capture->calls++;
}

static bool construction_read_chain_event_type(
    FILE *f, uint8_t *out_type) {
    if (!f || !out_type) return false;
    uint8_t header[CHAIN_EVENT_HEADER_SIZE];
    uint8_t len_bytes[2];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        fread(len_bytes, 1, sizeof(len_bytes), f) !=
            sizeof(len_bytes)) {
        return false;
    }
    uint16_t payload_len = (uint16_t)len_bytes[0] |
        (uint16_t)((uint16_t)len_bytes[1] << 8);
    if (fseek(f, (long)payload_len, SEEK_CUR) != 0)
        return false;
    *out_type = header[16];
    return true;
}

static bool construction_read_chain_event_payload(
    FILE *f,
    uint8_t *out_type,
    uint64_t *out_event_id,
    void *out_payload,
    size_t payload_cap,
    uint16_t *out_payload_len) {
    if (!f || !out_type || !out_event_id ||
        !out_payload || !out_payload_len) {
        return false;
    }
    uint8_t header[CHAIN_EVENT_HEADER_SIZE];
    uint8_t len_bytes[2];
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        fread(len_bytes, 1, sizeof(len_bytes), f) !=
            sizeof(len_bytes)) {
        return false;
    }
    uint16_t payload_len = (uint16_t)len_bytes[0] |
        (uint16_t)((uint16_t)len_bytes[1] << 8);
    if ((size_t)payload_len > payload_cap ||
        fread(out_payload, 1, payload_len, f) != payload_len) {
        return false;
    }
    uint64_t event_id = 0;
    for (int i = 0; i < 8; i++)
        event_id |= (uint64_t)header[8 + i] << (i * 8);
    *out_type = header[16];
    *out_event_id = event_id;
    *out_payload_len = payload_len;
    return true;
}

static bool construction_cargo_store_matches_clone(
    const cargo_store_t *live, const cargo_store_t *snapshot) {
    if (!live || !snapshot ||
        live->manifest.count != snapshot->manifest.count ||
        live->manifest.cap != snapshot->manifest.cap) {
        return false;
    }
    if (live->manifest.count > 0 &&
        (!live->manifest.units || !snapshot->manifest.units ||
         memcmp(live->manifest.units, snapshot->manifest.units,
                (size_t)live->manifest.count *
                    sizeof(live->manifest.units[0])) != 0)) {
        return false;
    }
    const ship_receipts_t *live_receipts =
        cargo_store_receipts_const(live);
    const ship_receipts_t *snapshot_receipts =
        cargo_store_receipts_const(snapshot);
    if (!live_receipts || !snapshot_receipts ||
        live_receipts->count != snapshot_receipts->count ||
        live_receipts->cap != snapshot_receipts->cap) {
        return false;
    }
    return live_receipts->count == 0 ||
        (live_receipts->chains && snapshot_receipts->chains &&
         memcmp(live_receipts->chains, snapshot_receipts->chains,
                (size_t)live_receipts->count *
                    sizeof(live_receipts->chains[0])) == 0);
}

static bool construction_spawn_towed_material_pod(
    world_t *w, server_player_t *sp, commodity_t c, int count,
    const uint8_t origin[8]);

static int construction_setup_present_pod(
    world_t *w, int station_idx, int unit_count, bool station_custody) {
    if (!w || station_idx < 0 ||
        station_idx >= w->station_count ||
        station_idx >= MAX_STATIONS ||
        unit_count <= 0 ||
        unit_count > CARGO_POD_MANIFEST_CAP) {
        return -1;
    }
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->id = 0;
    if (!construction_make_verified_player(sp, 0xA8))
        return -1;
    sp->docked = true;
    sp->current_station = station_idx;
    sp->ship->pos = w->stations[station_idx].pos;
    if (!construction_spawn_towed_material_pod(
            w, sp, COMMODITY_FRAME, unit_count,
            (const uint8_t *)"PRESENT1")) {
        return -1;
    }
    int pod_idx =
        sp->ship->towed_pods[sp->ship->towed_pod_count - 1];
    if (pod_idx < 0 ||
        !test_anchor_pod_legacy_cargo(w, station_idx, pod_idx)) {
        return -1;
    }
    if (station_custody)
        cargo_pod_set_station_custody(&w->cargo_pods[pod_idx],
                                      station_idx);
    return pod_idx;
}

static void construction_seed_birth_fragment(world_t *w, int idx,
                                             commodity_t commodity,
                                             vec2 pos, uint8_t tag) {
    asteroid_t *a = &w->asteroids[idx];
    memset(a, 0, sizeof(*a));
    memset(&w->fracture_claims[idx], 0, sizeof(w->fracture_claims[idx]));
    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = commodity;
    a->pos = pos;
    a->vel = v2(0.0f, 0.0f);
    a->radius = 10.0f;
    a->hp = 1.0f;
    a->max_hp = 1.0f;
    a->ore = 4.0f;
    a->max_ore = 4.0f;
    a->last_towed_by = -1;
    a->last_fractured_by = -1;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    for (int i = 0; i < 32; i++)
        a->fracture_seed[i] = (uint8_t)(tag + i + 1);
    fracture_claim_state_t *claim = &w->fracture_claims[idx];
    claim->resolved = true;
    claim->best_nonce = (uint32_t)tag + 17u;
    claim->best_grade = a->grade;
    for (int i = 0; i < 32; i++)
        claim->best_player_pub[i] = (uint8_t)(tag ^ (uint8_t)(i + 0x5Au));
    mining_fragment_pub_compute(
        a->fracture_seed, claim->best_player_pub, claim->best_nonce,
        a->fragment_pub);
    a->net_dirty = true;
}

static bool construction_spawn_towed_material_pod(world_t *w,
                                                  server_player_t *sp,
                                                  commodity_t c,
                                                  int count,
                                                  const uint8_t origin[8]) {
    if (!w || !sp || count < 0 || count > CARGO_POD_MANIFEST_CAP)
        return false;
    if (count == 0) return true;
    if (sp->ship->towed_pod_count >= 10) return false;
    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    for (int i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, c, (uint16_t)i, &units[i]))
            return false;
    }
    int pod_idx = spawn_cargo_pod_with_manifest(w, sp->ship->pos,
                                                v2(0.0f, 0.0f), c,
                                                units, (uint16_t)count,
                                                CARGO_POD_CARGO);
    if (pod_idx < 0) return false;
    return world_cargo_pod_set_player_tractor(w, pod_idx, (int)sp->id);
}

static int construction_spawn_loose_material_pod(world_t *w, vec2 pos,
                                                 commodity_t c,
                                                 int count,
                                                 const uint8_t origin[8]) {
    if (!w || count <= 0 || count > CARGO_POD_MANIFEST_CAP)
        return -1;
    cargo_unit_t units[CARGO_POD_MANIFEST_CAP];
    memset(units, 0, sizeof(units));
    for (int i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, c, (uint16_t)i, &units[i]))
            return -1;
    }
    return spawn_cargo_pod_with_manifest(w, pos, v2(0.0f, 0.0f), c,
                                         units, (uint16_t)count,
                                         CARGO_POD_CARGO);
}

static int construction_first_dock_module_idx(const station_t *st) {
    if (!st) return -1;
    for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
        if (st->modules[m].type == MODULE_DOCK &&
            !st->modules[m].scaffold) {
            return m;
        }
    }
    return -1;
}

static int construction_spawn_station_market_pod(world_t *w,
                                                 int station_idx,
                                                 commodity_t c,
                                                 int count,
                                                 const uint8_t origin[8]) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return -1;
    station_t *st = &w->stations[station_idx];
    int dock_idx = construction_first_dock_module_idx(st);
    if (dock_idx < 0) return -1;
    vec2 pos = module_world_pos_ring(st, st->modules[dock_idx].ring,
                                     st->modules[dock_idx].slot);
    int pod_idx = construction_spawn_loose_material_pod(
        w, pos, c, count, origin);
    if (pod_idx < 0) return -1;
    return world_cargo_pod_set_module_tractor(
        w, pod_idx, station_idx, dock_idx) ? pod_idx : -1;
}

static int construction_count_exact_pod_units(const world_t *w, commodity_t c) {
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

static bool construction_hopper_pos_for(const station_t *st,
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

static int construction_hopper_idx_for(const station_t *st,
                                       commodity_t commodity) {
    if (!st || commodity >= COMMODITY_COUNT) return -1;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *m = &st->modules[i];
        if (m->scaffold || m->type != MODULE_HOPPER) continue;
        if ((commodity_t)m->commodity != commodity) continue;
        return i;
    }
    return -1;
}

static void construction_stage_towed_pods_at_hoppers(world_t *w,
                                                     const station_t *st,
                                                     const ship_t *ship) {
    if (!w || !st || !ship) return;
    for (int t = 0; t < ship->towed_pod_count && t < 10; t++) {
        int idx = ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        cargo_pod_t *pod = &w->cargo_pods[idx];
        vec2 hopper_pos = pod->pos;
        if (construction_hopper_pos_for(st, pod->commodity, &hopper_pos))
            pod->pos = hopper_pos;
    }
}

static void construction_reset_station_modules(station_t *st) {
    if (!st) return;
    memset(st->modules, 0, sizeof(st->modules));
    st->module_count = 0;
    st->pending_ship_build_count = 0;
    st->pending_scaffold_count = 0;
    rebuild_station_services(st);
}

static bool construction_far_slot_from_pos(const station_t *st, vec2 pos,
                                           int *out_ring, uint8_t *out_slot) {
    if (!st || !out_ring || !out_slot) return false;
    float min_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        for (int slot = 0; slot < STATION_RING_SLOTS[ring]; slot++) {
            vec2 slot_pos = module_world_pos_ring(st, ring, slot);
            if (v2_dist_sq(pos, slot_pos) <= min_sq) continue;
            *out_ring = ring;
            *out_slot = (uint8_t)slot;
            return true;
        }
    }
    return false;
}

static bool construction_serving_slot_from_pos(const station_t *st, vec2 pos,
                                               int *out_ring,
                                               uint8_t *out_slot) {
    if (!st || !out_ring || !out_slot) return false;
    float pull_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    float staging_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        for (int slot = 0; slot < STATION_RING_SLOTS[ring]; slot++) {
            vec2 slot_pos = module_world_pos_ring(st, ring, slot);
            float d = v2_dist_sq(pos, slot_pos);
            if (d > pull_sq || d <= staging_sq) continue;
            *out_ring = ring;
            *out_slot = (uint8_t)slot;
            return true;
        }
    }
    return false;
}

static void construction_setup_split_shipyard_materials(station_t *st) {
    construction_reset_station_modules(st);
    add_module_at(st, MODULE_SHIPYARD, 1, 0);
    add_hopper_for(st, 2, 0, COMMODITY_FRAME);
    add_module_at(st, MODULE_SHIPYARD, 3, 4);
    add_hopper_for(st, 2, 3, COMMODITY_LASER_MODULE);
    add_hopper_for(st, 3, 4, COMMODITY_TRACTOR_MODULE);
    rebuild_station_services(st);
}

TEST(test_outpost_requires_signal_range) {
    WORLD_DECL;
    world_reset(&w);
    /* Can't place outside signal range */
    bool ok = can_place_outpost(&w, v2(100000.0f, 100000.0f));
    ASSERT(!ok);
    /* Can place within signal range (near refinery at (0,-2400), range 18000) */
    bool ok2 = can_place_outpost(&w, v2_add(w.stations[0].pos, v2(5000.0f, 0.0f)));
    ASSERT(ok2);
}

TEST(test_outpost_extends_signal_range) {
    WORLD_DECL;
    world_reset(&w);
    /* Place point at edge of refinery signal — within range but far */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(8000.0f, 0.0f));
    /* Verify the point is in signal before placing */
    ASSERT(signal_strength_at(&w, outpost_pos) > 0.0f);

    /* Set up a player docked at Kepler Yard (station 1, has BLUEPRINT) */
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    

    int slot = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(slot >= SIGNAL_FIRST_OUTPOST_INDEX);
    /* Scaffold doesn't provide signal — only Prospect's fringe covers this
     * point before activation. */
    ASSERT(signal_strength_at(&w, outpost_pos) > 0.0f);
    ASSERT(signal_strength_at(&w, outpost_pos) < 0.3f);
    /* Complete construction to activate signal */
    w.stations[slot].scaffold = false;
    w.stations[slot].scaffold_progress = 1.0f;
    rebuild_signal_chain(&w);
    /* Now the outpost itself provides strong signal at its own position */
    float s = signal_strength_at(&w, outpost_pos);
    ASSERT(s > 0.9f);
    /* Signal should extend beyond the outpost */
    float s2 = signal_strength_at(&w, v2_add(outpost_pos, v2(3000.0f, 0.0f)));
    ASSERT(s2 > 0.0f);
}

TEST(test_disconnected_station_goes_dark) {
    WORLD_DECL;
    world_reset(&w);
    /* All 3 starter stations should be connected */
    ASSERT(w.stations[0].signal_connected);
    ASSERT(w.stations[1].signal_connected);
    ASSERT(w.stations[2].signal_connected);

    /* Place an outpost within signal range of station 0 */
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* credits are station-local (ledger) — no ship.credits field */
    w.players[0].docked = false;

    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(5000.0f, 0.0f));
    int slot = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(slot >= 0);
    /* Finish construction */
    w.stations[slot].scaffold_progress = 1.0f;
    w.stations[slot].scaffold = false;
    w.stations[slot].signal_range = 6000.0f;
    w.stations[slot].signal_connected = false;
    w.stations[slot].modules[w.stations[slot].module_count++] = (station_module_t){ .type = MODULE_REPAIR_BAY };
    rebuild_signal_chain(&w);
    ASSERT(w.stations[slot].signal_connected);
    ASSERT(station_provides_signal(&w.stations[slot]));

    /* Shrink ALL root stations so the outpost is disconnected */
    float saved[3];
    for (int i = 0; i < 3; i++) {
        saved[i] = w.stations[i].signal_range;
        w.stations[i].signal_range = 1.0f;
    }
    rebuild_signal_chain(&w);
    ASSERT(!w.stations[slot].signal_connected);
    ASSERT(!station_provides_signal(&w.stations[slot]));

    /* Restore — outpost should reconnect */
    for (int i = 0; i < 3; i++)
        w.stations[i].signal_range = saved[i];
    rebuild_signal_chain(&w);
    ASSERT(w.stations[slot].signal_connected);
}

TEST(test_outpost_requires_undocked) {
    /* Must be undocked to place an outpost */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* credits are station-local (ledger) — no ship.credits field */
    

    /* Docked — should fail */
    w.players[0].docked = true;
    int slot = test_place_outpost_via_tow(&w, &w.players[0], v2(6000.0f, -2400.0f));
    ASSERT_EQ_INT(slot, -1);

    /* Undocked — should succeed */
    w.players[0].docked = false;
    slot = test_place_outpost_via_tow(&w, &w.players[0], v2(6000.0f, -2400.0f));
    ASSERT(slot >= SIGNAL_FIRST_OUTPOST_INDEX);
}

TEST(test_outpost_requires_towed_scaffold) {
    /* Without a towed scaffold, place_outpost intent is a no-op. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    /* No spawn_scaffold call — ship has no towed_scaffold */
    w.players[0].input.place_outpost = true;
    world_sim_step(&w, SIM_DT);
    /* No new outpost should exist */
    bool any_new = false;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++)
        if (station_exists(&w.stations[s])) { any_new = true; break; }
    ASSERT(!any_new);
}

TEST(test_outpost_min_distance) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    
    /* Too close to Prospect Refinery at (0,-2400) — within OUTPOST_MIN_DISTANCE (800) */
    int slot = test_place_outpost_via_tow(&w, &w.players[0], v2_add(w.stations[0].pos, v2(500.0f, 0.0f)));
    ASSERT_EQ_INT(slot, -1);
}

TEST(test_module_build_material_types) {
    /* Verify each module requires the correct ingot type. LASER_FAB
     * needs crystal ingots plus frames. Plant both, then queue
     * the laser fab. */
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    add_hopper_for(st, 3, 1, COMMODITY_CRYSTAL_INGOT);
    add_hopper_for(st, 3, 7, COMMODITY_FRAME);
    int build_slot = station_ring_free_slot(
        st, 2, STATION_RING_SLOTS[2]);
    ASSERT(build_slot >= 0);
    begin_module_construction_at(&w, st, 1, MODULE_LASER_FAB, 2, build_slot);
    bool found_crystal = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].commodity == COMMODITY_CRYSTAL_INGOT) {
            found_crystal = true; break;
        }
    }
    ASSERT(found_crystal);
}

TEST(test_module_construction_and_delivery) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1]; /* Kepler */
    int mc_before = st->module_count;
    /* TRACTOR_FAB needs cuprite ingot plus frame hoppers. */
    add_hopper_for(st, 3, 1, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(st, 3, 2, COMMODITY_FRAME);
    int producer_idx = mc_before + 2;
    int build_slot = station_ring_free_slot(
        st, 2, STATION_RING_SLOTS[2]);
    ASSERT(build_slot >= 0);
    begin_module_construction_at(&w, st, 1, MODULE_TRACTOR_FAB, 2, build_slot);
    ASSERT_EQ_INT(st->module_count, mc_before + 3);
    ASSERT(st->modules[producer_idx].scaffold);
    ASSERT_EQ_INT((int)st->modules[producer_idx].type, (int)MODULE_TRACTOR_FAB);
    /* Deliver the required exact cuprite cargo in a physical towed pod. */
    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_CUPRITE_INGOT,
        (int)ceilf(module_build_cost_lookup(MODULE_TRACTOR_FAB) - 0.0001f),
        (const uint8_t *)"MODBUILD"));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 1);
    ASSERT(test_anchor_pod_legacy_cargo(
        &w, 1, sp->ship->towed_pods[0]));
    ASSERT_EQ_INT(ship_towed_pods_manifest_count(
                      &w, sp->ship, COMMODITY_CUPRITE_INGOT),
                  (int)module_build_cost_lookup(MODULE_TRACTOR_FAB));
    step_module_delivery(&w, st, 1, sp->ship, COMMODITY_COUNT);
    ASSERT_EQ_FLOAT(ship_cargo_amount(sp->ship, COMMODITY_CUPRITE_INGOT),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(st->modules[producer_idx].build_progress, 1.0f, 0.01f); /* fully supplied */
    ASSERT(st->modules[producer_idx].scaffold);  /* still building — not instant */
    /* Run sim for 15 seconds (MODULE_BUILD_TIME = 10s + margin) */
    for (int i = 0; i < (int)(15.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(!st->modules[producer_idx].scaffold);  /* activated after build time */
}

/* Regression: a frame delivered into a scaffold via service_sell must
 * also have its matching cargo_unit_t removed from the ship manifest.
 * Without the consume, the named frame stays in the ship's manifest
 * and could be sold or transferred again. */
TEST(test_construction_consumes_manifest_units) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s_scaffold_lineage", TMP("clog"));
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);
    station_t *st = &w.stations[0];
    st->chain_event_count = 0;
    memset(st->chain_last_hash, 0, sizeof(st->chain_last_hash));
    st->scaffold = true;
    st->scaffold_progress = 0.0f;

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    memset(sp->session_token, 0xCC, sizeof(sp->session_token));
    player_init_ship(sp, &w);
    ASSERT(test_set_ship_finished_units(sp->ship, COMMODITY_FRAME, 5,
                                        MINING_GRADE_COMMON));
    for (uint16_t i = 0; i < sp->ship->manifest.count; i++) {
        ASSERT(test_anchor_legacy_cargo_unit(
            &w, 0, &sp->ship->manifest.units[i]));
    }
    uint64_t origin_events = st->chain_event_count;
    ASSERT_EQ_INT(manifest_count_by_commodity(&sp->ship->manifest, COMMODITY_FRAME), 5);

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_COUNT;
    world_sim_step(&w, SIM_DT);

    int frames_left = manifest_count_by_commodity(&sp->ship->manifest, COMMODITY_FRAME);
    ASSERT_EQ_INT(ship_finished_count(sp->ship, COMMODITY_FRAME), frames_left);
    /* Some frames consumed by the scaffold (it needs them). */
    ASSERT(frames_left < 5);
    int consumed = 5 - frames_left;
    ASSERT_EQ_INT((int)st->chain_event_count,
                  (int)origin_events + consumed);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT((int)walked, (int)origin_events + consumed);

    char path[256];
    ASSERT(chain_log_path_for(st->station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    ASSERT(construction_skip_chain_events(f, origin_events));
    uint8_t header[CHAIN_EVENT_HEADER_SIZE];
    ASSERT(fread(header, 1, sizeof(header), f) == sizeof(header));
    ASSERT_EQ_INT(header[16], CHAIN_EVT_CONSTRUCTION);
    uint8_t len_bytes[2];
    ASSERT(fread(len_bytes, 1, sizeof(len_bytes), f) == sizeof(len_bytes));
    uint16_t payload_len = (uint16_t)len_bytes[0] |
                           (uint16_t)((uint16_t)len_bytes[1] << 8);
    ASSERT_EQ_INT(payload_len, (int)sizeof(chain_payload_construction_t));
    chain_payload_construction_t payload = {0};
    ASSERT(fread(&payload, 1, sizeof(payload), f) == sizeof(payload));
    fclose(f);

    ASSERT_EQ_INT(payload.target_kind, CONSTRUCTION_TARGET_STATION);
    ASSERT_EQ_INT(payload.station_index, 0);
    ASSERT_EQ_INT(payload.module_index, 0xff);
    ASSERT_EQ_INT(payload.module_type, 0xff);
    ASSERT_EQ_INT(payload.commodity, COMMODITY_FRAME);
    ASSERT_EQ_FLOAT(payload.contributed_units, 1.0f, 0.001f);
    chain_log_set_dir(NULL);
}

TEST(test_station_scaffold_manifest_batch_append_failure_is_inert) {
    WORLD_DECL;
    const chain_log_test_fault_point_t faults[] = {
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
    };

    for (int failure_case = 0; failure_case < 3; failure_case++) {
        char dir[256];
        snprintf(
            dir, sizeof(dir), "%s/station_scaffold_batch_failure_%d",
            test_tmp_dir(), failure_case);
        chain_log_set_disk_enabled(true);
        chain_log_set_dir(dir);
        chain_log_test_fault_clear();

        world_reset(&w);
        station_t *st = &w.stations[0];
        st->scaffold = true;
        st->scaffold_progress = 0.0f;

        server_player_t *sp = &w.players[0];
        sp->connected = true;
        sp->session_ready = true;
        sp->id = 0;
        memset(sp->session_token, 0xC7, sizeof(sp->session_token));
        memset(sp->pubkey, 0xA3, sizeof(sp->pubkey));
        sp->pubkey_set = true;
        sp->pubkey_proof_ok = true;
        sp->pubkey_challenge_consumed = true;
        player_init_ship(sp, &w);
        sp->docked = true;
        sp->current_station = 0;
        ASSERT(test_set_ship_finished_units(
            sp->ship, COMMODITY_FRAME, 3, MINING_GRADE_COMMON));
        ASSERT(construction_attach_local_ship_receipts(&w, 0, sp));

        cargo_store_t ship_before = {0};
        ASSERT(cargo_store_clone(
            &ship_before, &sp->ship->cargo_store));
        cargo_unit_t *manifest_ptr_before =
            sp->ship->cargo_store.manifest.units;
        void *receipts_ptr_before =
            sp->ship->cargo_store.receipts_opaque;
        float cargo_before = sp->ship->cargo[COMMODITY_FRAME];
        float progress_before = st->scaffold_progress;
        uint64_t events_before = st->chain_event_count;
        uint8_t hash_before[32];
        memcpy(hash_before, st->chain_last_hash, sizeof(hash_before));
        uint64_t walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT((int)walked, (int)events_before);
        ASSERT(!st->chain_append_blocked);

        if (failure_case < 2) {
            /* Fault the second row so WRITE proves the first serialized
             * contribution is rolled back with the rest of the batch. */
            chain_log_test_fault_inject(
                faults[failure_case],
                CHAIN_EVT_CONSTRUCTION, 2);
        } else {
            chain_log_health_set(
                st, CHAIN_HEALTH_FAILED, true,
                st->chain_event_count, st->chain_last_hash,
                "test pre-blocked station scaffold delivery");
        }

        sp->input.service_sell = true;
        sp->input.service_sell_only = COMMODITY_FRAME;
        world_sim_step(&w, SIM_DT);
        chain_log_test_fault_clear();

        ASSERT(st->scaffold);
        ASSERT_EQ_FLOAT(
            st->scaffold_progress, progress_before, 0.001f);
        ASSERT_EQ_INT(
            (int)st->chain_event_count, (int)events_before);
        ASSERT(memcmp(
            st->chain_last_hash, hash_before,
            sizeof(hash_before)) == 0);
        ASSERT(st->chain_append_blocked);
        walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT((int)walked, (int)events_before);

        ASSERT(
            sp->ship->cargo_store.manifest.units ==
            manifest_ptr_before);
        ASSERT(
            sp->ship->cargo_store.receipts_opaque ==
            receipts_ptr_before);
        ASSERT_EQ_INT(
            sp->ship->cargo_store.manifest.count,
            ship_before.manifest.count);
        ASSERT_EQ_INT(
            sp->ship->cargo_store.manifest.cap,
            ship_before.manifest.cap);
        ASSERT(memcmp(
            sp->ship->cargo_store.manifest.units,
            ship_before.manifest.units,
            (size_t)ship_before.manifest.count *
                sizeof(*ship_before.manifest.units)) == 0);
        const ship_receipts_t *receipts_after =
            cargo_store_receipts_const(
                &sp->ship->cargo_store);
        const ship_receipts_t *receipts_before =
            cargo_store_receipts_const(&ship_before);
        ASSERT(receipts_after != NULL);
        ASSERT(receipts_before != NULL);
        ASSERT_EQ_INT(
            receipts_after->count, receipts_before->count);
        ASSERT_EQ_INT(
            receipts_after->cap, receipts_before->cap);
        ASSERT(memcmp(
            receipts_after->chains,
            receipts_before->chains,
            (size_t)receipts_before->count *
                sizeof(*receipts_before->chains)) == 0);
        ASSERT_EQ_FLOAT(
            sp->ship->cargo[COMMODITY_FRAME],
            cargo_before, 0.001f);
        ASSERT_EQ_INT(
            ship_finished_count(
                sp->ship, COMMODITY_FRAME),
            3);
        cargo_store_cleanup(&ship_before);
    }

    chain_log_test_fault_clear();
    chain_log_set_dir(NULL);
}

TEST(test_module_delivery_emits_construction_chain_event) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s_construction_lineage", TMP("clog"));
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);
    station_t *st = &w.stations[0];
    st->chain_event_count = 0;
    memset(st->chain_last_hash, 0, sizeof(st->chain_last_hash));

    ASSERT(st->module_count < MAX_MODULES_PER_STATION);
    int module_idx = st->module_count++;
    station_module_t *m = &st->modules[module_idx];
    memset(m, 0, sizeof(*m));
    m->type = MODULE_SIGNAL_RELAY;
    m->ring = 1;
    m->slot = 7;
    m->scaffold = true;
    m->build_progress = 0.0f;

    ship_t ship = {0};
    ASSERT(manifest_init(&ship.manifest, 4));
    ship.cargo[COMMODITY_FRAME] = 3.0f;
    cargo_unit_t units[3] = {{0}};
    cargo_unit_t *unit_ptrs[3] = {0};
    for (int i = 0; i < 3; i++) {
        uint8_t origin[8] = {
            'M', 'O', 'D', 'C', 'H', 'N', '0',
            (uint8_t)('1' + i),
        };
        ASSERT(hash_legacy_migrate_unit(
            origin, COMMODITY_FRAME, 0, &units[i]));
        ASSERT(ship_manifest_push_with_chain(
            &ship, &units[i], NULL));
        unit_ptrs[i] = &ship.manifest.units[i];
    }
    ASSERT(world_anchor_legacy_cargo_origins(
        &w, 0, unit_ptrs, 3));
    uint64_t origin_events = st->chain_event_count;
    cargo_receipt_origin_cache_reset();

    float payout = step_module_delivery(&w, st, 0, &ship, COMMODITY_FRAME);
    ASSERT(payout > 0.0f);
    ASSERT_EQ_INT(manifest_count_by_commodity(&ship.manifest, COMMODITY_FRAME), 0);
    ASSERT_EQ_FLOAT(
        m->build_progress,
        3.0f / module_build_cost_lookup(MODULE_SIGNAL_RELAY),
        0.001f);
    ASSERT_EQ_INT((int)st->chain_event_count, (int)origin_events + 3);
    cargo_receipt_origin_cache_stats_t cache_stats =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT((int)cache_stats.full_verifications, 1);
    ASSERT_EQ_INT((int)cache_stats.index_builds, 1);
    ASSERT(cache_stats.hits >= 2);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT((int)walked, (int)origin_events + 3);

    char path[256];
    ASSERT(chain_log_path_for(st->station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    ASSERT(construction_skip_chain_events(f, origin_events));
    uint8_t header[CHAIN_EVENT_HEADER_SIZE];
    ASSERT(fread(header, 1, sizeof(header), f) == sizeof(header));
    ASSERT_EQ_INT(header[16], CHAIN_EVT_CONSTRUCTION);
    uint8_t len_bytes[2];
    ASSERT(fread(len_bytes, 1, sizeof(len_bytes), f) == sizeof(len_bytes));
    uint16_t payload_len = (uint16_t)len_bytes[0] |
                           (uint16_t)((uint16_t)len_bytes[1] << 8);
    ASSERT_EQ_INT(payload_len, (int)sizeof(chain_payload_construction_t));
    chain_payload_construction_t payload = {0};
    ASSERT(fread(&payload, 1, sizeof(payload), f) == sizeof(payload));
    fclose(f);

    ASSERT(memcmp(payload.cargo_pub, units[0].pub,
                  sizeof(units[0].pub)) == 0);
    ASSERT_EQ_INT(payload.target_kind, CONSTRUCTION_TARGET_MODULE);
    ASSERT_EQ_INT(payload.station_index, 0);
    ASSERT_EQ_INT(payload.module_index, module_idx);
    ASSERT_EQ_INT(payload.module_type, MODULE_SIGNAL_RELAY);
    ASSERT_EQ_INT(payload.commodity, COMMODITY_FRAME);
    ASSERT_EQ_FLOAT(payload.contributed_units, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(payload.progress_after,
                    1.0f / module_build_cost_lookup(MODULE_SIGNAL_RELAY),
                    0.001f);
    chain_log_set_dir(NULL);
}

TEST(test_module_manifest_batch_append_failure_is_inert) {
    WORLD_DECL;
    const chain_log_test_fault_point_t faults[] = {
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
    };

    for (int failure_case = 0; failure_case < 3; failure_case++) {
        char dir[256];
        snprintf(dir, sizeof(dir),
                 "%s/module_manifest_batch_failure_%d",
                 test_tmp_dir(), failure_case);
        chain_log_set_disk_enabled(true);
        chain_log_set_dir(dir);
        chain_log_test_fault_clear();

        world_reset(&w);
        station_t *st = &w.stations[0];
        chain_log_reset(st);
        ASSERT(st->module_count < MAX_MODULES_PER_STATION);
        int module_idx = st->module_count++;
        station_module_t *m = &st->modules[module_idx];
        memset(m, 0, sizeof(*m));
        m->type = MODULE_SIGNAL_RELAY;
        m->ring = 1;
        m->slot = 7;
        m->scaffold = true;

        ship_t ship = {0};
        ASSERT(manifest_init(&ship.manifest, 4));
        ship.cargo[COMMODITY_FRAME] = 3.0f;
        cargo_unit_t units[3] = {{0}};
        cargo_unit_t *unit_ptrs[3] = {0};
        for (int i = 0; i < 3; i++) {
            uint8_t origin[8] = {
                'M', 'O', 'D', 'F', 'A', 'I', 'L',
                (uint8_t)('1' + i),
            };
            ASSERT(hash_legacy_migrate_unit(
                origin, COMMODITY_FRAME, 0, &units[i]));
            ASSERT(ship_manifest_push_with_chain(
                &ship, &units[i], NULL));
            unit_ptrs[i] = &ship.manifest.units[i];
        }
        ASSERT(world_anchor_legacy_cargo_origins(
            &w, 0, unit_ptrs, 3));

        cargo_store_t ship_before = {0};
        ASSERT(cargo_store_clone(
            &ship_before, &ship.cargo_store));
        cargo_unit_t *manifest_ptr_before =
            ship.cargo_store.manifest.units;
        void *receipts_ptr_before =
            ship.cargo_store.receipts_opaque;
        uint64_t events_before = st->chain_event_count;
        uint8_t hash_before[32];
        memcpy(hash_before, st->chain_last_hash,
               sizeof(hash_before));

        if (failure_case < 2) {
            chain_log_test_fault_inject(
                faults[failure_case],
                CHAIN_EVT_CONSTRUCTION, 2);
        } else {
            chain_log_health_set(
                st, CHAIN_HEALTH_FAILED, true,
                st->chain_event_count, st->chain_last_hash,
                "test pre-blocked module manifest delivery");
        }
        float payout = step_module_delivery(
            &w, st, 0, &ship, COMMODITY_FRAME);
        chain_log_test_fault_clear();

        ASSERT_EQ_FLOAT(payout, 0.0f, 0.001f);
        ASSERT_EQ_FLOAT(m->build_progress, 0.0f, 0.001f);
        ASSERT_EQ_INT(
            (int)st->chain_event_count, (int)events_before);
        ASSERT(memcmp(st->chain_last_hash, hash_before,
                      sizeof(hash_before)) == 0);
        ASSERT(st->chain_append_blocked);
        uint64_t walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT((int)walked, (int)events_before);

        ASSERT(ship.cargo_store.manifest.units ==
               manifest_ptr_before);
        ASSERT(ship.cargo_store.receipts_opaque ==
               receipts_ptr_before);
        ASSERT_EQ_INT(
            ship.manifest.count,
            ship_before.manifest.count);
        ASSERT(memcmp(
            ship.manifest.units, ship_before.manifest.units,
            (size_t)ship_before.manifest.count *
                sizeof(*ship_before.manifest.units)) == 0);
        const ship_receipts_t *receipts_after =
            cargo_store_receipts_const(&ship.cargo_store);
        const ship_receipts_t *receipts_before =
            cargo_store_receipts_const(&ship_before);
        ASSERT(receipts_after != NULL);
        ASSERT(receipts_before != NULL);
        ASSERT_EQ_INT(
            receipts_after->count, receipts_before->count);
        ASSERT(memcmp(
            receipts_after->chains, receipts_before->chains,
            (size_t)receipts_before->count *
                sizeof(*receipts_before->chains)) == 0);
        ASSERT_EQ_FLOAT(
            ship.cargo[COMMODITY_FRAME], 3.0f, 0.001f);

        cargo_store_cleanup(&ship_before);
        cargo_store_cleanup(&ship.cargo_store);
    }

    chain_log_test_fault_clear();
    chain_log_set_dir(NULL);
}

TEST(test_module_delivery_consumes_towed_manifest_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = &w.stations[0];
    chain_log_reset(st);

    ASSERT(st->module_count < MAX_MODULES_PER_STATION);
    int module_idx = st->module_count++;
    station_module_t *m = &st->modules[module_idx];
    memset(m, 0, sizeof(*m));
    m->type = MODULE_SIGNAL_RELAY;
    m->ring = 1;
    m->slot = 7;
    m->scaffold = true;
    m->build_progress = 0.0f;

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    cargo_unit_t units[3] = {{0}};
    for (int i = 0; i < 3; i++) {
        uint8_t origin[8] = {
            'P', 'O', 'D', 'M', 'O', 'D', 'L',
            (uint8_t)('1' + i),
        };
        ASSERT(hash_legacy_migrate_unit(
            origin, COMMODITY_FRAME, 0, &units[i]));
    }
    int pod_idx = spawn_cargo_pod_with_manifest(&w, st->pos,
                                                v2(0.0f, 0.0f),
                                                COMMODITY_FRAME, units, 3,
                                                CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    ASSERT(world_cargo_pod_set_player_tractor(&w, pod_idx, 0));
    ASSERT(test_anchor_pod_legacy_cargo(&w, 0, pod_idx));
    uint64_t origin_events = st->chain_event_count;
    cargo_receipt_origin_cache_reset();

    float payout = step_module_delivery(&w, st, 0, sp->ship,
                                        COMMODITY_FRAME);

    ASSERT(payout > 0.0f);
    ASSERT_EQ_FLOAT(m->build_progress,
                    3.0f / module_build_cost_lookup(MODULE_SIGNAL_RELAY),
                    0.001f);
    ASSERT(!w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT((int)st->chain_event_count, (int)origin_events + 3);
    cargo_receipt_origin_cache_stats_t cache_stats =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT((int)cache_stats.full_verifications, 1);
    ASSERT_EQ_INT((int)cache_stats.index_builds, 1);
    ASSERT(cache_stats.hits >= 2);
}

TEST(test_module_physical_delivery_append_failure_is_inert) {
    WORLD_DECL;
    const chain_log_test_fault_point_t faults[] = {
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
    };
    for (int failure_case = 0; failure_case < 3; failure_case++) {
        world_reset(&w);
        memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
        station_t *st = &w.stations[0];
        chain_log_reset(st);

        ASSERT(st->module_count < MAX_MODULES_PER_STATION);
        int module_idx = st->module_count++;
        station_module_t *m = &st->modules[module_idx];
        memset(m, 0, sizeof(*m));
        m->type = MODULE_SIGNAL_RELAY;
        m->ring = 1;
        m->slot = 7;
        m->scaffold = true;
        m->build_progress = 0.0f;

        server_player_t *sp = &w.players[0];
        player_init_ship(sp, &w);
        sp->id = 0;
        sp->connected = true;
        cargo_unit_t units[3] = {{0}};
        for (int i = 0; i < 3; i++) {
            uint8_t origin[8] = {
                'P', 'O', 'D', 'F', 'A', 'I', 'L',
                (uint8_t)('1' + i),
            };
            ASSERT(hash_legacy_migrate_unit(
                origin, COMMODITY_FRAME, 0, &units[i]));
        }
        int pod_idx = spawn_cargo_pod_with_manifest(
            &w, st->pos, v2(0.0f, 0.0f), COMMODITY_FRAME,
            units, 3, CARGO_POD_CARGO);
        ASSERT(pod_idx >= 0);
        ASSERT(world_cargo_pod_set_player_tractor(
            &w, pod_idx, 0));
        ASSERT(test_anchor_pod_legacy_cargo(&w, 0, pod_idx));

        cargo_pod_t pod_before = w.cargo_pods[pod_idx];
        int towed_before = sp->ship->towed_pod_count;
        uint64_t events_before = st->chain_event_count;
        uint8_t hash_before[32];
        memcpy(hash_before, st->chain_last_hash,
               sizeof(hash_before));

        if (failure_case < 2) {
            chain_log_test_fault_inject(
                faults[failure_case],
                CHAIN_EVT_CONSTRUCTION, 2);
        } else {
            st->chain_append_blocked = true;
        }
        float payout = step_module_delivery(
            &w, st, 0, sp->ship, COMMODITY_FRAME);
        chain_log_test_fault_clear();

        ASSERT_EQ_FLOAT(payout, 0.0f, 0.001f);
        ASSERT_EQ_FLOAT(m->build_progress, 0.0f, 0.001f);
        ASSERT(memcmp(&w.cargo_pods[pod_idx], &pod_before,
                      sizeof(pod_before)) == 0);
        ASSERT_EQ_INT(sp->ship->towed_pod_count, towed_before);
        ASSERT_EQ_INT(sp->ship->towed_pods[0], pod_idx);
        ASSERT_EQ_INT(
            (int)st->chain_event_count, (int)events_before);
        ASSERT(memcmp(st->chain_last_hash, hash_before,
                      sizeof(hash_before)) == 0);
        uint64_t walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT((int)walked, (int)events_before);
        ASSERT(st->chain_append_blocked);
    }
}

TEST(test_station_scaffold_rejects_towed_manifest_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = &w.stations[0];
    st->scaffold = true;
    st->scaffold_progress = 0.0f;
    chain_log_reset(st);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    sp->docked = true;
    sp->current_station = 0;
    memset(sp->session_token, 0x5c, sizeof(sp->session_token));

    cargo_unit_t unit = {0};
    ASSERT(hash_legacy_migrate_unit((const uint8_t *)"PODSTN01",
                                    COMMODITY_FRAME, 0, &unit));
    int pod_idx = spawn_cargo_pod_with_manifest(&w, sp->ship->pos,
                                                v2(0.0f, 0.0f),
                                                COMMODITY_FRAME, &unit, 1,
                                                CARGO_POD_CARGO);
    ASSERT(pod_idx >= 0);
    ASSERT(world_cargo_pod_set_player_tractor(&w, pod_idx, 0));
    ASSERT(test_anchor_pod_legacy_cargo(&w, 0, pod_idx));
    uint64_t origin_events = st->chain_event_count;

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FRAME;
    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 1);
    ASSERT_EQ_FLOAT(st->scaffold_progress, 0.0f, 0.001f);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT((int)st->chain_event_count, (int)origin_events);
}

TEST(test_present_towed_pod_rejects_unanchored_unknown_identity) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_unknown",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    ASSERT(construction_make_verified_player(sp, 0xA7));
    sp->docked = true;
    sp->current_station = 0;
    sp->ship->pos = w.stations[0].pos;
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_FRAME, 2,
        (const uint8_t *)"UNKNOWN1"));
    int pod_idx =
        sp->ship->towed_pods[sp->ship->towed_pod_count - 1];
    ASSERT(pod_idx >= 0 && pod_idx < MAX_CARGO_PODS);

    /*
     * The unit fields claim this station as their origin, but no matching
     * SMELT/CRAFT/legacy-origin event exists. PRESENT must never turn that
     * assertion into the first portable receipt.
     */
    uint8_t selection_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, selection_token));
    cargo_store_t store_before = {0};
    ASSERT(cargo_store_clone(
        &store_before, &sp->ship->cargo_store));
    cargo_pod_t pod_before = w.cargo_pods[pod_idx];
    tow_link_t tow_before[MAX_TOW_LINKS];
    memcpy(tow_before, w.tow_links, sizeof(tow_before));
    uint64_t events_before =
        w.stations[0].chain_event_count;
    uint8_t hash_before[32];
    memcpy(hash_before, w.stations[0].chain_last_hash,
           sizeof(hash_before));
    construction_receipt_sink_capture_t capture = {0};

    uint16_t moved = 99;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)pod_idx, selection_token,
            construction_capture_receipt_chain, &capture,
            &moved),
        CARGO_POD_PRESENT_REJECT_TRUST);
    ASSERT_EQ_INT(moved, 0);
    ASSERT_EQ_INT(capture.calls, 0);
    ASSERT(construction_cargo_store_matches_clone(
        &sp->ship->cargo_store, &store_before));
    ASSERT(memcmp(&w.cargo_pods[pod_idx], &pod_before,
                  sizeof(pod_before)) == 0);
    ASSERT(memcmp(w.tow_links, tow_before,
                  sizeof(tow_before)) == 0);
    ASSERT_EQ_INT(
        (int)w.stations[0].chain_event_count,
        (int)events_before);
    ASSERT(memcmp(w.stations[0].chain_last_hash,
                  hash_before, sizeof(hash_before)) == 0);

    cargo_store_cleanup(&store_before);
    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_signed_action_feeds_remote_scaffold) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_free_e2e",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);

    int pod_idx = construction_setup_present_pod(&w, 0, 3, false);
    ASSERT(pod_idx >= 0);
    server_player_t *sp = &w.players[0];
    station_t *source = &w.stations[0];
    cargo_unit_t expected[3];
    memcpy(expected, w.cargo_pods[pod_idx].manifest_units,
           sizeof(expected));
    uint64_t source_origin_events = source->chain_event_count;

    ledger_earn_by_pubkey(source, sp->pubkey, 500.0f);
    float balance_before =
        ledger_balance_by_pubkey(source, sp->pubkey);
    sp->ship->stat_credits_spent = 17.0f;
    float spent_before = sp->ship->stat_credits_spent;

    uint8_t selection_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, selection_token));
    uint8_t payload[35] = {0};
    payload[0] = (uint8_t)pod_idx;
    memcpy(&payload[1], selection_token, sizeof(selection_token));
    payload[33] = 0x34;
    payload[34] = 0x12;

    construction_receipt_sink_capture_t capture = {0};
    server_signed_action_dispatch_result_t dispatch = {0};
    ASSERT(server_dispatch_signed_action_payload(
        &w, 0, SIGNED_ACTION_PRESENT_POD,
        payload, sizeof(payload),
        construction_capture_receipt_chain, &capture,
        &dispatch));
    ASSERT(dispatch.pod_present_evaluated);
    ASSERT_EQ_INT(dispatch.pod_present_result,
                  CARGO_POD_PRESENT_OK);
    ASSERT_EQ_INT(dispatch.pod_present_moved, 3);
    ASSERT_EQ_INT(dispatch.pod_present_action_id, 0x1234);
    ASSERT(!sp->pending_action_result_valid);

    ASSERT_EQ_INT(sp->ship->manifest.count, 3);
    const ship_receipts_t *ship_receipts =
        ship_get_receipts_const(sp->ship);
    ASSERT(ship_receipts != NULL);
    ASSERT_EQ_INT(ship_receipts->count, 3);
    ASSERT_EQ_INT(capture.calls, 3);
    for (int i = 0; i < 3; i++) {
        ASSERT(memcmp(&sp->ship->manifest.units[i],
                      &expected[i], sizeof(expected[i])) == 0);
        ASSERT_EQ_INT(ship_receipts->chains[i].len, 1);
        ASSERT_EQ_INT(capture.chains[i].len, 1);
        ASSERT(memcmp(ship_receipts->chains[i].links,
                      capture.chains[i].links,
                      sizeof(cargo_receipt_t)) == 0);
        ASSERT(memcmp(capture.chains[i].links[0].cargo_pub,
                      expected[i].pub, 32) == 0);
        ASSERT(memcmp(
            capture.chains[i].links[0].recipient_pubkey,
            sp->pubkey, 32) == 0);
        ASSERT(memcmp(
            capture.chains[i].links[0].authoring_station,
            source->station_pubkey, 32) == 0);
    }
    ASSERT(!w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(
        (int)source->chain_event_count,
        (int)source_origin_events + 3);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(source, sp->pubkey),
        balance_before, 0.001f);
    ASSERT_EQ_FLOAT(sp->ship->stat_credits_spent,
                    spent_before, 0.001f);

    char source_path[256];
    ASSERT(chain_log_path_for(
        source->station_pubkey,
        source_path, sizeof(source_path)));
    FILE *source_log = fopen(source_path, "rb");
    ASSERT(source_log != NULL);
    ASSERT(construction_skip_chain_events(
        source_log, source_origin_events));
    for (int i = 0; i < 3; i++) {
        uint8_t type = CHAIN_EVT_NONE;
        ASSERT(construction_read_chain_event_type(
            source_log, &type));
        ASSERT_EQ_INT(type, CHAIN_EVT_TRANSFER);
    }
    fclose(source_log);

    /* The bridge's output is the ordinary receipt-backed ship store.
     * Move it to another authority and prove the existing construction
     * gate accepts it without any pod-special-case consumption. */
    station_t *target = &w.stations[1];
    target->scaffold = true;
    target->scaffold_progress = 0.0f;
    uint64_t target_events_before = target->chain_event_count;
    sp->docked = true;
    sp->current_station = 1;
    sp->ship->pos = target->pos;
    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_FRAME;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(sp->ship->manifest.count, 0);
    ASSERT_EQ_INT(ship_finished_count(
                      sp->ship, COMMODITY_FRAME), 0);
    ASSERT_EQ_FLOAT(
        target->scaffold_progress,
        3.0f / SCAFFOLD_MATERIAL_NEEDED, 0.001f);
    ASSERT_EQ_INT(
        (int)target->chain_event_count,
        (int)target_events_before + 3);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(source, &walked, NULL));
    ASSERT_EQ_INT((int)walked,
                  (int)source->chain_event_count);
    walked = 0;
    ASSERT(chain_log_verify(target, &walked, NULL));
    ASSERT_EQ_INT((int)walked,
                  (int)target->chain_event_count);

    chain_log_set_dir(NULL);
}

TEST(test_purchased_frame_pods_found_real_outpost_without_receipt_injection) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_real_outpost",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    server_player_t *sp = &w.players[0];
    uint8_t session_token[8] =
        {0x67, 0x40, 0x52, 0x43, 0x50, 0x4f, 0x44, 0x31};
    memcpy(sp->session_token, session_token,
           sizeof(session_token));
    sp->id = 0;
    ASSERT(construction_make_verified_player(sp, 0xB7));
    ASSERT(registry_register_pubkey(
        &w, sp->pubkey, sp->session_token));
    player_init_ship(sp, &w);

    station_t *source = &w.stations[1];
    world_seed_station_manifests(&w);
    ASSERT_EQ_INT(station_finished_mint(
                      source, COMMODITY_FRAME, 50, NULL), 50);
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->ship->pos = source->pos;
    ledger_earn_by_pubkey(source, sp->pubkey, 10000.0f);
    float balance_before =
        ledger_balance_by_pubkey(source, sp->pubkey);

    int frame_units =
        (int)ceilf(SCAFFOLD_MATERIAL_NEEDED);
    ASSERT_EQ_INT(frame_units, 48);
    cargo_unit_t expected[48];
    memset(expected, 0, sizeof(expected));
    int presented = 0;
    uint16_t action_id = 1;
    for (int purchase = 0;
         presented < frame_units; purchase++) {
        int batch_units = frame_units - presented;
        if (batch_units > 16) batch_units = 16;
        uint8_t origin[8] =
            {'R','E','A','L','P','O','D',0};
        origin[7] = (uint8_t)purchase;
        int pod_idx =
            construction_spawn_station_market_pod(
                &w, 1, COMMODITY_FRAME,
                batch_units, origin);
        ASSERT(pod_idx >= 0);
        ASSERT(test_anchor_pod_legacy_cargo(
            &w, 1, pod_idx));
        memcpy(&expected[presented],
               w.cargo_pods[pod_idx].manifest_units,
               (size_t)batch_units *
                   sizeof(expected[0]));

        /* This is the ordinary station-market purchase: custody and the
         * aggregate price anchor move with the physical pod into tow. */
        sp->input.buy_product = true;
        sp->input.buy_commodity =
            COMMODITY_FRAME;
        sp->input.buy_grade =
            MINING_GRADE_COUNT;
        sp->input.buy_station_pod = true;
        sp->input.buy_station_pod_index =
            (uint16_t)pod_idx;
        world_sim_step(&w, SIM_DT);
        ASSERT_EQ_INT(sp->ship->towed_pod_count, 1);
        ASSERT_EQ_INT(sp->ship->towed_pods[0],
                      pod_idx);
        ASSERT_EQ_INT(cargo_pod_player_tractor(
                          &w.cargo_pods[pod_idx]), 0);
        ASSERT_EQ_INT(cargo_pod_custody_station(
                          &w.cargo_pods[pod_idx]), 1);
        ASSERT(cargo_pod_custody_charge_anchor_valid(
            &w.cargo_pods[pod_idx]));

        /* PRESENT is the normal signed protocol action. It authors the
         * source receipt and atomically installs the exact units plus their
         * sidecars in the ship store; the test never fabricates a receipt. */
        uint8_t selection_token[32];
        ASSERT(server_cargo_pod_selection_token(
            &w, pod_idx, selection_token));
        uint8_t payload[35] = {0};
        payload[0] = (uint8_t)pod_idx;
        memcpy(&payload[1], selection_token,
               sizeof(selection_token));
        write_u16_le(&payload[33],
                     action_id++);
        server_signed_action_dispatch_result_t
            dispatch = {0};
        ASSERT(server_dispatch_signed_action_payload(
            &w, 0, SIGNED_ACTION_PRESENT_POD,
            payload, sizeof(payload), NULL, NULL,
            &dispatch));
        ASSERT(dispatch.pod_present_evaluated);
        ASSERT_EQ_INT(dispatch.pod_present_result,
                      CARGO_POD_PRESENT_OK);
        ASSERT_EQ_INT(dispatch.pod_present_moved,
                      batch_units);
        presented += batch_units;
        ASSERT(!w.cargo_pods[pod_idx].active);
        ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    }

    ASSERT_EQ_INT(presented, frame_units);
    ASSERT_EQ_INT(sp->ship->manifest.count,
                  frame_units);
    ASSERT(ledger_balance_by_pubkey(
               source, sp->pubkey) <
           balance_before);
    ASSERT(sp->ship->stat_credits_spent > 0.0f);
    const ship_receipts_t *receipts =
        ship_get_receipts_const(sp->ship);
    ASSERT(receipts != NULL);
    ASSERT_EQ_INT(receipts->count, frame_units);
    for (int i = 0; i < frame_units; i++) {
        ASSERT(memcmp(&sp->ship->manifest.units[i],
                      &expected[i],
                      sizeof(expected[i])) == 0);
        ASSERT_EQ_INT(receipts->chains[i].len, 1);
        ASSERT(memcmp(
            receipts->chains[i].links[0]
                .recipient_pubkey,
            sp->pubkey, 32) == 0);
        ASSERT(memcmp(
            receipts->chains[i].links[0]
                .authoring_station,
            source->station_pubkey, 32) == 0);
    }

    /* Plant the real relay-tow outpost, carry the ordinary ship manifest to
     * the new authority, and let the unchanged scaffold trust gate consume
     * it. No pod-special-case construction path is involved. */
    sp->docked = false;
    vec2 outpost_pos =
        v2_add(w.stations[0].pos,
               v2(6000.0f, 0.0f));
    int outpost =
        test_place_outpost_via_tow(
            &w, sp, outpost_pos);
    ASSERT(outpost >=
           SIGNAL_FIRST_OUTPOST_INDEX);
    station_t *target =
        &w.stations[outpost];
    ASSERT(target->scaffold);

    for (int i = 0; i < frame_units; i++) {
        cargo_receipt_station_evaluation_t trust =
            cargo_receipt_evaluate_at_station(
                &w, outpost,
                &sp->ship->manifest.units[i],
                &receipts->chains[i]);
        ASSERT(trust.accepted);
    }

    sp->docked = true;
    sp->current_station = outpost;
    sp->nearby_station = outpost;
    sp->in_dock_range = true;
    sp->ship->pos = target->pos;
    uint64_t target_events_before =
        target->chain_event_count;
    for (int i = 0;
         i < 4 && target->scaffold; i++) {
        sp->input.service_sell = true;
        sp->input.service_sell_only =
            COMMODITY_FRAME;
        world_sim_step(&w, SIM_DT);
    }

    ASSERT(!target->scaffold);
    ASSERT_EQ_FLOAT(target->scaffold_progress,
                    1.0f, 0.001f);
    ASSERT(target->signal_connected);
    ASSERT_EQ_INT(sp->ship->manifest.count, 0);
    ASSERT_EQ_INT(ship_finished_count(
                      sp->ship,
                      COMMODITY_FRAME), 0);
    receipts = ship_get_receipts_const(
        sp->ship);
    ASSERT(receipts != NULL);
    ASSERT_EQ_INT(receipts->count, 0);
    ASSERT_EQ_INT(
        (int)target->chain_event_count,
        (int)target_events_before +
            frame_units);

    uint64_t walked = 0;
    ASSERT(chain_log_verify(
        source, &walked, NULL));
    ASSERT_EQ_INT((int)walked,
                  (int)source->chain_event_count);
    walked = 0;
    ASSERT(chain_log_verify(
        target, &walked, NULL));
    ASSERT_EQ_INT((int)walked,
                  (int)target->chain_event_count);

    chain_log_set_dir(NULL);
}

TEST(test_present_station_custody_charges_once_with_paired_trade) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_charged",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);

    int pod_idx = construction_setup_present_pod(&w, 0, 3, true);
    ASSERT(pod_idx >= 0);
    server_player_t *sp = &w.players[0];
    station_t *source = &w.stations[0];
    uint64_t origin_events = source->chain_event_count;
    ledger_earn_by_pubkey(source, sp->pubkey, 1000.0f);
    float balance_before =
        ledger_balance_by_pubkey(source, sp->pubkey);
    float spent_before = sp->ship->stat_credits_spent;

    uint8_t selection_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, selection_token));
    construction_receipt_sink_capture_t capture = {0};
    uint16_t moved = 0;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)pod_idx, selection_token,
            construction_capture_receipt_chain, &capture,
            &moved),
        CARGO_POD_PRESENT_OK);
    ASSERT_EQ_INT(moved, 3);
    ASSERT_EQ_INT(capture.calls, 3);
    ASSERT_EQ_INT(
        (int)source->chain_event_count,
        (int)origin_events + 6);

    float balance_after =
        ledger_balance_by_pubkey(source, sp->pubkey);
    float charged = balance_before - balance_after;
    ASSERT(charged > 0.0f);
    ASSERT_EQ_FLOAT(
        sp->ship->stat_credits_spent - spent_before,
        charged, 0.001f);

    int buy_cost = 0;
    int buy_quantity = 0;
    for (int i = 0; i < w.events.count; i++) {
        if (w.events.events[i].type != SIM_EVENT_BUY)
            continue;
        buy_cost = w.events.events[i].buy.cost;
        buy_quantity = w.events.events[i].buy.quantity;
    }
    ASSERT(buy_cost > 0);
    ASSERT_EQ_INT(buy_quantity, 3);
    ASSERT_EQ_FLOAT(charged, (float)buy_cost, 0.001f);

    char path[256];
    ASSERT(chain_log_path_for(
        source->station_pubkey, path, sizeof(path)));
    FILE *log = fopen(path, "rb");
    ASSERT(log != NULL);
    ASSERT(construction_skip_chain_events(log, origin_events));
    for (int i = 0; i < 3; i++) {
        uint8_t transfer_type = CHAIN_EVT_NONE;
        uint8_t trade_type = CHAIN_EVT_NONE;
        ASSERT(construction_read_chain_event_type(
            log, &transfer_type));
        ASSERT(construction_read_chain_event_type(
            log, &trade_type));
        ASSERT_EQ_INT(transfer_type, CHAIN_EVT_TRANSFER);
        ASSERT_EQ_INT(trade_type, CHAIN_EVT_TRADE);
    }
    fclose(log);

    uint64_t events_after = source->chain_event_count;
    float repeat_balance =
        ledger_balance_by_pubkey(source, sp->pubkey);
    float repeat_spent = sp->ship->stat_credits_spent;
    uint16_t repeat_moved = 99;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)pod_idx, selection_token,
            construction_capture_receipt_chain, &capture,
            &repeat_moved),
        CARGO_POD_PRESENT_REJECT_STALE);
    ASSERT_EQ_INT(repeat_moved, 0);
    ASSERT_EQ_INT(capture.calls, 3);
    ASSERT_EQ_INT((int)source->chain_event_count,
                  (int)events_after);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(source, sp->pubkey),
        repeat_balance, 0.001f);
    ASSERT_EQ_FLOAT(sp->ship->stat_credits_spent,
                    repeat_spent, 0.001f);

    chain_log_set_dir(NULL);
}

TEST(test_present_station_custody_large_pod_conserves_aggregate_quote) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_aggregate_price",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);

    enum { UNIT_COUNT = CARGO_POD_MANIFEST_CAP };
    int pod_idx =
        construction_setup_present_pod(&w, 0, UNIT_COUNT, true);
    ASSERT(pod_idx >= 0);
    server_player_t *sp = &w.players[0];
    station_t *source = &w.stations[0];
    cargo_pod_t *pod = &w.cargo_pods[pod_idx];
    uint64_t origin_events = source->chain_event_count;

    /*
     * Make the unit quote deliberately fractional.  Empty finished stock
     * makes the sell curve exactly 2x base, so 1.9834 -> 3.9668 per unit.
     * Rounding the old 64/64/64/8 batch quotes yields one credit more than
     * rounding the aggregate 200-unit quote once.
     */
    int frame_stock =
        station_finished_count(source, COMMODITY_FRAME);
    if (frame_stock > 0) {
        ASSERT_EQ_INT(
            station_finished_drain(
                source, COMMODITY_FRAME, frame_stock),
            frame_stock);
    }
    source->_finished_residue[COMMODITY_FRAME] = 0.0f;
    source->base_price[COMMODITY_FRAME] = 1.9834f;

    float aggregate_quote = 0.0f;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        aggregate_quote +=
            station_sell_price_unit(source, &pod->manifest_units[i]) *
            mining_payout_multiplier(
                (mining_grade_t)pod->manifest_units[i].grade);
    }
    int64_t expected_total = (int64_t)llroundf(aggregate_quote);
    ASSERT(expected_total > 0);

    int64_t independently_rounded = 0;
    for (uint16_t first = 0; first < UNIT_COUNT;) {
        uint16_t count =
            (uint16_t)(UNIT_COUNT - first);
        if (count > CHAIN_LOG_BATCH_MAX_EVENTS / 2u)
            count = CHAIN_LOG_BATCH_MAX_EVENTS / 2u;
        float batch_quote = 0.0f;
        for (uint16_t i = 0; i < count; i++) {
            const cargo_unit_t *unit =
                &pod->manifest_units[first + i];
            batch_quote +=
                station_sell_price_unit(source, unit) *
                mining_payout_multiplier(
                    (mining_grade_t)unit->grade);
        }
        independently_rounded += (int64_t)llroundf(batch_quote);
        first = (uint16_t)(first + count);
    }
    ASSERT(independently_rounded != expected_total);

    ledger_earn_by_pubkey(source, sp->pubkey, 100000.0f);
    float balance_before =
        ledger_balance_by_pubkey(source, sp->pubkey);
    float spent_before = sp->ship->stat_credits_spent;
    int64_t observed_event_cost = 0;
    uint16_t processed = 0;

    while (processed < UNIT_COUNT) {
        uint8_t selection_token[32];
        ASSERT(server_cargo_pod_selection_token(
            &w, pod_idx, selection_token));
        int prior_event_count = w.events.count;
        uint16_t moved = 0;
        ASSERT_EQ_INT(
            server_present_towed_pod(
                &w, 0, (uint8_t)pod_idx, selection_token,
                NULL, NULL, &moved),
            CARGO_POD_PRESENT_OK);
        ASSERT(moved > 0);

        int64_t each =
            expected_total / (int64_t)UNIT_COUNT;
        uint16_t remainder =
            (uint16_t)(expected_total % (int64_t)UNIT_COUNT);
        uint16_t end = (uint16_t)(processed + moved);
        uint16_t bonus_begin =
            processed < remainder ? processed : remainder;
        uint16_t bonus_end =
            end < remainder ? end : remainder;
        int64_t expected_batch =
            each * (int64_t)moved +
            (int64_t)(bonus_end - bonus_begin);

        int buy_events = 0;
        for (int i = prior_event_count; i < w.events.count; i++) {
            if (w.events.events[i].type != SIM_EVENT_BUY)
                continue;
            buy_events++;
            ASSERT_EQ_INT(
                w.events.events[i].buy.quantity, moved);
            ASSERT_EQ_INT(
                w.events.events[i].buy.cost,
                (int)expected_batch);
            observed_event_cost +=
                w.events.events[i].buy.cost;
        }
        ASSERT_EQ_INT(buy_events, 1);
        processed = end;

        if (processed < UNIT_COUNT) {
            pod = &w.cargo_pods[pod_idx];
            ASSERT(pod->active);
            ASSERT_EQ_INT(
                (int)pod->custody_charge_total,
                (int)expected_total);
            ASSERT_EQ_INT(
                pod->custody_charge_unit_count, UNIT_COUNT);
            ASSERT_EQ_INT(
                pod->custody_charge_units_processed, processed);
            ASSERT_EQ_INT(
                pod->manifest_count, UNIT_COUNT - processed);
            ASSERT(cargo_pod_custody_charge_anchor_valid(pod));
            if (processed ==
                CHAIN_LOG_BATCH_MAX_EVENTS / 2u) {
                cargo_pod_t anchored_before = *pod;
                cargo_unit_t escaped = {0};
                ASSERT(!cargo_pod_take_manifest_unit(
                    pod, COMMODITY_FRAME, &escaped));
                ASSERT(memcmp(
                    pod, &anchored_before,
                    sizeof(anchored_before)) == 0);
                ASSERT(!ship_towed_pods_take_manifest_unit(
                    &w, sp->ship, COMMODITY_FRAME, &escaped));
                ASSERT(memcmp(
                    pod, &anchored_before,
                    sizeof(anchored_before)) == 0);
            }
        }
    }

    ASSERT(!w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT((int)observed_event_cost, (int)expected_total);
    ASSERT_EQ_FLOAT(
        balance_before -
            ledger_balance_by_pubkey(source, sp->pubkey),
        (float)expected_total, 0.001f);
    ASSERT_EQ_FLOAT(
        sp->ship->stat_credits_spent - spent_before,
        (float)expected_total, 0.001f);

    char path[256];
    ASSERT(chain_log_path_for(
        source->station_pubkey, path, sizeof(path)));
    FILE *log = fopen(path, "rb");
    ASSERT(log != NULL);
    ASSERT(construction_skip_chain_events(log, origin_events));
    int64_t durable_trade_total = 0;
    for (uint16_t i = 0; i < UNIT_COUNT; i++) {
        uint8_t transfer_type = CHAIN_EVT_NONE;
        uint8_t trade_type = CHAIN_EVT_NONE;
        uint64_t transfer_event_id = 0;
        uint64_t trade_event_id = 0;
        uint16_t transfer_len = 0;
        uint16_t trade_len = 0;
        chain_payload_transfer_t transfer = {0};
        chain_payload_trade_t trade = {0};
        ASSERT(construction_read_chain_event_payload(
            log, &transfer_type, &transfer_event_id,
            &transfer, sizeof(transfer), &transfer_len));
        ASSERT(construction_read_chain_event_payload(
            log, &trade_type, &trade_event_id,
            &trade, sizeof(trade), &trade_len));
        ASSERT_EQ_INT(transfer_type, CHAIN_EVT_TRANSFER);
        ASSERT_EQ_INT(trade_type, CHAIN_EVT_TRADE);
        ASSERT_EQ_INT(
            transfer_len, sizeof(chain_payload_transfer_t));
        ASSERT_EQ_INT(
            trade_len, sizeof(chain_payload_trade_t));
        ASSERT_EQ_INT(
            (int)transfer_event_id,
            (int)(origin_events + 1u + (uint64_t)i * 2u));
        ASSERT_EQ_INT(
            (int)trade_event_id, (int)transfer_event_id + 1);
        ASSERT_EQ_INT(
            (int)trade.transfer_event_id,
            (int)transfer_event_id);
        ASSERT(memcmp(
            transfer.cargo_pub,
            sp->ship->manifest.units[i].pub, 32) == 0);
        ASSERT(memcmp(
            trade.ledger_pubkey, sp->pubkey, 32) == 0);
        ASSERT(trade.ledger_delta_signed <= 0);
        durable_trade_total += trade.ledger_delta_signed;
    }
    ASSERT_EQ_INT(fgetc(log), EOF);
    fclose(log);
    ASSERT_EQ_INT(
        (int)durable_trade_total, (int)-expected_total);

    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_rejects_consumed_identity_replay) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_spent_replay",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);

    int first_pod =
        construction_setup_present_pod(&w, 0, 1, false);
    ASSERT(first_pod >= 0);
    server_player_t *first = &w.players[0];
    cargo_unit_t duplicate =
        w.cargo_pods[first_pod].manifest_units[0];

    server_player_t *second = &w.players[1];
    player_init_ship(second, &w);
    second->id = 1;
    ASSERT(construction_make_verified_player(second, 0xC8));
    second->docked = true;
    second->current_station = 0;
    second->ship->pos = w.stations[0].pos;
    int replay_pod = spawn_cargo_pod_with_manifest(
        &w, second->ship->pos, v2(0.0f, 0.0f),
        COMMODITY_FRAME, &duplicate, 1, CARGO_POD_CARGO);
    ASSERT(replay_pod >= 0);
    ASSERT(world_cargo_pod_set_player_tractor(
        &w, replay_pod, 1));

    uint8_t first_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, first_pod, first_token));
    uint16_t moved = 0;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)first_pod, first_token,
            NULL, NULL, &moved),
        CARGO_POD_PRESENT_OK);
    ASSERT_EQ_INT(moved, 1);
    ASSERT_EQ_INT(first->ship->manifest.count, 1);

    /* Destroy the first received copy before replaying the duplicate. A
     * destination-manifest lookup can no longer mask whether durable source
     * history enforces single-spend. */
    cargo_unit_t consumed = {0};
    cargo_receipt_chain_t consumed_chain = {0};
    ASSERT(ship_manifest_remove_with_chain(
        first->ship, 0, &consumed, &consumed_chain));
    ASSERT(memcmp(consumed.pub, duplicate.pub, 32) == 0);
    ASSERT_EQ_INT(consumed_chain.len, 1);
    ASSERT_EQ_INT(first->ship->manifest.count, 0);

    /*
     * The canonical empty-chain issuance path must see the same durable spent
     * evidence as PRESENT. This rejects an ordinary station/NPC-style first
     * receipt before a second append is even attempted.
     */
    uint64_t ordinary_events_before =
        w.stations[0].chain_event_count;
    cargo_receipt_transfer_link_t ordinary_link =
        cargo_receipt_prepare_transfer_link(
            &w.stations[0], w.stations[0].station_pubkey,
            duplicate.pub, NULL);
    ASSERT_EQ_INT(
        ordinary_link.status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN);
    ASSERT_EQ_INT(
        ordinary_link.origin_status,
        CARGO_RECEIPT_ORIGIN_RESOLVE_ALREADY_TRANSFERRED);
    cargo_receipt_prepared_transfer_t ordinary =
        cargo_receipt_prepare_transfer(
            &w, 0, w.stations[0].station_pubkey,
            second->pubkey, &duplicate, NULL,
            false, 0, NULL);
    ASSERT_EQ_INT(
        ordinary.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_TRUST);
    ASSERT_EQ_INT(
        (int)w.stations[0].chain_event_count,
        (int)ordinary_events_before);

    uint8_t replay_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, replay_pod, replay_token));
    uint64_t events_before =
        w.stations[0].chain_event_count;
    cargo_pod_t pod_before = w.cargo_pods[replay_pod];
    tow_link_t tow_before[MAX_TOW_LINKS];
    memcpy(tow_before, w.tow_links, sizeof(tow_before));
    moved = 99;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 1, (uint8_t)replay_pod, replay_token,
            NULL, NULL, &moved),
        CARGO_POD_PRESENT_REJECT_TRUST);
    ASSERT_EQ_INT(moved, 0);
    ASSERT_EQ_INT(second->ship->manifest.count, 0);
    ASSERT(memcmp(&w.cargo_pods[replay_pod], &pod_before,
                  sizeof(pod_before)) == 0);
    ASSERT(memcmp(w.tow_links, tow_before,
                  sizeof(tow_before)) == 0);
    ASSERT_EQ_INT(
        (int)w.stations[0].chain_event_count,
        (int)events_before);

    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_rejects_identity_spent_by_ordinary_transfer) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_ordinary_spent_replay",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);

    int pod_idx =
        construction_setup_present_pod(&w, 0, 1, false);
    ASSERT(pod_idx >= 0);
    server_player_t *presenter = &w.players[0];
    cargo_unit_t duplicate =
        w.cargo_pods[pod_idx].manifest_units[0];

    server_player_t *ordinary_recipient = &w.players[1];
    player_init_ship(ordinary_recipient, &w);
    ordinary_recipient->id = 1;
    ASSERT(construction_make_verified_player(
        ordinary_recipient, 0xC8));

    cargo_receipt_transfer_commit_result_t ordinary =
        cargo_receipt_commit_transfer(
            &w, 0, w.stations[0].station_pubkey,
            ordinary_recipient->pubkey, &duplicate, NULL,
            false, 0, NULL);
    ASSERT_EQ_INT(
        ordinary.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        ordinary.append.status, CHAIN_LOG_APPEND_OK);
    cargo_receipt_chain_t ordinary_chain = {
        .len = 1,
        .links = {ordinary.receipt},
    };
    ASSERT(ship_manifest_push_with_chain(
        ordinary_recipient->ship, &duplicate,
        &ordinary_chain));

    cargo_unit_t consumed = {0};
    cargo_receipt_chain_t consumed_chain = {0};
    ASSERT(ship_manifest_remove_with_chain(
        ordinary_recipient->ship, 0,
        &consumed, &consumed_chain));
    ASSERT(memcmp(consumed.pub, duplicate.pub, 32) == 0);
    ASSERT_EQ_INT(consumed_chain.len, 1);
    ASSERT_EQ_INT(ordinary_recipient->ship->manifest.count, 0);

    /*
     * Drop the trusted-append cache so PRESENT must rediscover the spend
     * from verified durable history rather than destination inventory or a
     * process-local insertion.
     */
    cargo_receipt_origin_cache_reset();
    uint8_t selection_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, selection_token));
    uint64_t events_before =
        w.stations[0].chain_event_count;
    cargo_pod_t pod_before = w.cargo_pods[pod_idx];
    tow_link_t tow_before[MAX_TOW_LINKS];
    memcpy(tow_before, w.tow_links, sizeof(tow_before));

    uint16_t moved = 99;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)pod_idx, selection_token,
            NULL, NULL, &moved),
        CARGO_POD_PRESENT_REJECT_TRUST);
    ASSERT_EQ_INT(moved, 0);
    ASSERT_EQ_INT(presenter->ship->manifest.count, 0);
    ASSERT(memcmp(&w.cargo_pods[pod_idx], &pod_before,
                  sizeof(pod_before)) == 0);
    ASSERT(memcmp(w.tow_links, tow_before,
                  sizeof(tow_before)) == 0);
    ASSERT_EQ_INT(
        (int)w.stations[0].chain_event_count,
        (int)events_before);

    cargo_receipt_origin_cache_stats_t cache_stats =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT((int)cache_stats.full_verifications, 1);
    ASSERT_EQ_INT((int)cache_stats.index_builds, 1);

    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_rejects_wrong_origin_and_tamper) {
    for (int rejection_case = 0;
         rejection_case < 2; rejection_case++) {
        char dir[256];
        snprintf(dir, sizeof(dir),
                 "%s/present_pod_reject_%d",
                 test_tmp_dir(), rejection_case);
        chain_log_set_disk_enabled(true);
        chain_log_set_dir(dir);
        chain_log_test_fault_clear();

        WORLD_DECL_NAME(case_world);
        world_reset(&case_world);
        for (int s = 0; s < MAX_STATIONS; s++)
            chain_log_reset(&case_world.stations[s]);
        int pod_idx = construction_setup_present_pod(
            &case_world, 0, 3, false);
        ASSERT(pod_idx >= 0);
        cargo_pod_t *pod = &case_world.cargo_pods[pod_idx];
        if (rejection_case == 0) {
            pod->manifest_units[1].origin_station = 1;
        } else {
            pod->manifest_units[1].mined_block ^= 1u;
        }

        uint8_t selection_token[32];
        ASSERT(server_cargo_pod_selection_token(
            &case_world, pod_idx, selection_token));
        cargo_pod_t pod_before = *pod;
        tow_link_t tow_before[MAX_TOW_LINKS];
        memcpy(tow_before, case_world.tow_links,
               sizeof(tow_before));
        uint64_t events_before =
            case_world.stations[0].chain_event_count;
        uint8_t hash_before[32];
        memcpy(hash_before,
               case_world.stations[0].chain_last_hash,
               sizeof(hash_before));
        uint16_t moved = 99;

        cargo_pod_present_result_t result =
            server_present_towed_pod(
                &case_world, 0, (uint8_t)pod_idx,
                selection_token, NULL, NULL, &moved);
        ASSERT_EQ_INT(
            result,
            rejection_case == 0
                ? CARGO_POD_PRESENT_REJECT_WRONG_ORIGIN
                : CARGO_POD_PRESENT_REJECT_TRUST);
        ASSERT_EQ_INT(moved, 0);
        ASSERT(memcmp(pod, &pod_before,
                      sizeof(pod_before)) == 0);
        ASSERT(memcmp(case_world.tow_links, tow_before,
                      sizeof(tow_before)) == 0);
        ASSERT_EQ_INT(
            case_world.players[0].ship->towed_pod_count, 1);
        ASSERT_EQ_INT(
            case_world.players[0].ship->manifest.count, 0);
        ASSERT_EQ_INT(
            (int)case_world.stations[0].chain_event_count,
            (int)events_before);
        ASSERT(memcmp(
            case_world.stations[0].chain_last_hash,
            hash_before, sizeof(hash_before)) == 0);
    }
    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_preflights_tampered_tail) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_tail_tamper",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);
    /* Charged presentation can append at most 64 identities because
     * every TRANSFER is paired with a TRADE. Corrupt row 65: validation
     * must reject the whole selected pod before rows 1..64 commit. */
    int pod_idx = construction_setup_present_pod(
        &w, 0, 65, true);
    ASSERT(pod_idx >= 0);
    server_player_t *sp = &w.players[0];
    station_t *station = &w.stations[0];
    ledger_earn_by_pubkey(station, sp->pubkey, 1000.0f);
    sp->ship->stat_credits_spent = 9.0f;
    w.cargo_pods[pod_idx].manifest_units[64].mined_block ^= 1u;

    uint8_t selection_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, selection_token));
    cargo_store_t store_before = {0};
    ASSERT(cargo_store_clone(
        &store_before, &sp->ship->cargo_store));
    cargo_unit_t *manifest_ptr_before =
        sp->ship->cargo_store.manifest.units;
    void *receipts_ptr_before =
        sp->ship->cargo_store.receipts_opaque;
    cargo_pod_t pod_before = w.cargo_pods[pod_idx];
    tow_link_t tow_before[MAX_TOW_LINKS];
    memcpy(tow_before, w.tow_links, sizeof(tow_before));
    uint8_t ledger_before[sizeof(station->ledger)];
    memcpy(ledger_before, station->ledger,
           sizeof(ledger_before));
    int ledger_count_before = station->ledger_count;
    float spent_before = sp->ship->stat_credits_spent;
    uint64_t events_before = station->chain_event_count;
    uint8_t hash_before[32];
    memcpy(hash_before, station->chain_last_hash,
           sizeof(hash_before));
    construction_receipt_sink_capture_t capture = {0};

    uint16_t moved = 99;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)pod_idx, selection_token,
            construction_capture_receipt_chain, &capture,
            &moved),
        CARGO_POD_PRESENT_REJECT_TRUST);
    ASSERT_EQ_INT(moved, 0);
    ASSERT_EQ_INT(capture.calls, 0);
    ASSERT(sp->ship->cargo_store.manifest.units ==
           manifest_ptr_before);
    ASSERT(sp->ship->cargo_store.receipts_opaque ==
           receipts_ptr_before);
    ASSERT(construction_cargo_store_matches_clone(
        &sp->ship->cargo_store, &store_before));
    ASSERT(memcmp(&w.cargo_pods[pod_idx], &pod_before,
                  sizeof(pod_before)) == 0);
    ASSERT(memcmp(w.tow_links, tow_before,
                  sizeof(tow_before)) == 0);
    ASSERT_EQ_INT(station->ledger_count,
                  ledger_count_before);
    ASSERT(memcmp(station->ledger, ledger_before,
                  sizeof(ledger_before)) == 0);
    ASSERT_EQ_FLOAT(sp->ship->stat_credits_spent,
                    spent_before, 0.001f);
    ASSERT_EQ_INT((int)station->chain_event_count,
                  (int)events_before);
    ASSERT(memcmp(station->chain_last_hash, hash_before,
                  sizeof(hash_before)) == 0);
    cargo_store_cleanup(&store_before);
    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_rejects_recycled_stale_selection) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/present_pod_stale",
             test_tmp_dir());
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(dir);
    chain_log_test_fault_clear();

    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w.stations[s]);
    int pod_idx = construction_setup_present_pod(&w, 0, 2, false);
    ASSERT(pod_idx >= 0);

    uint8_t old_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, old_token));
    cargo_pod_t same_content = w.cargo_pods[pod_idx];
    world_cargo_pod_clear_tractor(&w, pod_idx);
    memset(&w.cargo_pods[pod_idx], 0,
           sizeof(w.cargo_pods[pod_idx]));
    /* Recycle immediately through the production allocator: no world tick
     * and no artificial ref lookup may be needed to retire the generation. */
    int replacement_idx = spawn_cargo_pod_with_manifest(
        &w, same_content.pos, same_content.vel,
        same_content.commodity, same_content.manifest_units,
        same_content.manifest_count, same_content.kind);
    ASSERT_EQ_INT(replacement_idx, pod_idx);
    ASSERT(world_cargo_pod_set_player_tractor(
        &w, pod_idx, 0));

    uint8_t replacement_token[32];
    ASSERT(server_cargo_pod_selection_token(
        &w, pod_idx, replacement_token));
    ASSERT(memcmp(old_token, replacement_token,
                  sizeof(old_token)) != 0);
    cargo_pod_t pod_before = w.cargo_pods[pod_idx];
    tow_link_t tow_before[MAX_TOW_LINKS];
    memcpy(tow_before, w.tow_links, sizeof(tow_before));
    uint64_t events_before =
        w.stations[0].chain_event_count;
    uint8_t hash_before[32];
    memcpy(hash_before, w.stations[0].chain_last_hash,
           sizeof(hash_before));

    uint16_t moved = 99;
    ASSERT_EQ_INT(
        server_present_towed_pod(
            &w, 0, (uint8_t)pod_idx, old_token,
            NULL, NULL, &moved),
        CARGO_POD_PRESENT_REJECT_STALE);
    ASSERT_EQ_INT(moved, 0);
    ASSERT(memcmp(&w.cargo_pods[pod_idx], &pod_before,
                  sizeof(pod_before)) == 0);
    ASSERT(memcmp(w.tow_links, tow_before,
                  sizeof(tow_before)) == 0);
    ASSERT_EQ_INT((int)w.stations[0].chain_event_count,
                  (int)events_before);
    ASSERT(memcmp(w.stations[0].chain_last_hash,
                  hash_before, sizeof(hash_before)) == 0);

    chain_log_set_dir(NULL);
}

TEST(test_present_towed_pod_log_failures_are_byte_inert) {
    const chain_log_test_fault_point_t faults[] = {
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
    };
    const chain_event_type_t fault_events[] = {
        CHAIN_EVT_TRANSFER,
        CHAIN_EVT_TRANSFER,
        CHAIN_EVT_TRADE,
        CHAIN_EVT_TRADE,
    };
    for (int failure_case = 0;
         failure_case < 5; failure_case++) {
        char dir[256];
        snprintf(dir, sizeof(dir),
                 "%s/present_pod_failure_%d",
                 test_tmp_dir(), failure_case);
        chain_log_set_disk_enabled(true);
        chain_log_set_dir(dir);
        chain_log_test_fault_clear();

        WORLD_DECL_NAME(case_world);
        world_reset(&case_world);
        for (int s = 0; s < MAX_STATIONS; s++)
            chain_log_reset(&case_world.stations[s]);
        int pod_idx = construction_setup_present_pod(
            &case_world, 0, 3, true);
        ASSERT(pod_idx >= 0);
        server_player_t *sp = &case_world.players[0];
        station_t *station = &case_world.stations[0];
        ledger_earn_by_pubkey(station, sp->pubkey, 1000.0f);
        sp->ship->stat_credits_spent = 11.0f;

        uint8_t selection_token[32];
        ASSERT(server_cargo_pod_selection_token(
            &case_world, pod_idx, selection_token));
        if (failure_case < 4) {
            chain_log_test_fault_inject(
                faults[failure_case],
                fault_events[failure_case], 2);
        } else {
            chain_log_health_set(
                station, CHAIN_HEALTH_FAILED, true,
                station->chain_event_count,
                station->chain_last_hash,
                "test pre-blocked pod presentation");
        }

        cargo_store_t store_before = {0};
        ASSERT(cargo_store_clone(
            &store_before, &sp->ship->cargo_store));
        cargo_unit_t *manifest_ptr_before =
            sp->ship->cargo_store.manifest.units;
        void *receipts_ptr_before =
            sp->ship->cargo_store.receipts_opaque;
        cargo_pod_t pod_before =
            case_world.cargo_pods[pod_idx];
        tow_link_t tow_before[MAX_TOW_LINKS];
        memcpy(tow_before, case_world.tow_links,
               sizeof(tow_before));
        int towed_count_before = sp->ship->towed_pod_count;
        int16_t towed_before[10];
        memcpy(towed_before, sp->ship->towed_pods,
               sizeof(towed_before));
        uint8_t ledger_before[sizeof(station->ledger)];
        memcpy(ledger_before, station->ledger,
               sizeof(ledger_before));
        int ledger_count_before = station->ledger_count;
        float spent_before = sp->ship->stat_credits_spent;
        uint64_t events_before = station->chain_event_count;
        uint8_t hash_before[32];
        memcpy(hash_before, station->chain_last_hash,
               sizeof(hash_before));
        construction_receipt_sink_capture_t capture = {0};

        uint16_t moved = 99;
        ASSERT_EQ_INT(
            server_present_towed_pod(
                &case_world, 0, (uint8_t)pod_idx,
                selection_token,
                construction_capture_receipt_chain,
                &capture, &moved),
            CARGO_POD_PRESENT_REJECT_LOG);
        chain_log_test_fault_clear();

        ASSERT_EQ_INT(moved, 0);
        ASSERT_EQ_INT(capture.calls, 0);
        ASSERT(
            sp->ship->cargo_store.manifest.units ==
            manifest_ptr_before);
        ASSERT(
            sp->ship->cargo_store.receipts_opaque ==
            receipts_ptr_before);
        ASSERT(construction_cargo_store_matches_clone(
            &sp->ship->cargo_store, &store_before));
        ASSERT(memcmp(
            &case_world.cargo_pods[pod_idx],
            &pod_before, sizeof(pod_before)) == 0);
        ASSERT(memcmp(case_world.tow_links, tow_before,
                      sizeof(tow_before)) == 0);
        ASSERT_EQ_INT(sp->ship->towed_pod_count,
                      towed_count_before);
        ASSERT(memcmp(sp->ship->towed_pods, towed_before,
                      sizeof(towed_before)) == 0);
        ASSERT_EQ_INT(station->ledger_count,
                      ledger_count_before);
        ASSERT(memcmp(station->ledger, ledger_before,
                      sizeof(ledger_before)) == 0);
        ASSERT_EQ_FLOAT(sp->ship->stat_credits_spent,
                        spent_before, 0.001f);
        ASSERT_EQ_INT((int)station->chain_event_count,
                      (int)events_before);
        ASSERT(memcmp(station->chain_last_hash, hash_before,
                      sizeof(hash_before)) == 0);
        ASSERT(station->chain_append_blocked);
        uint64_t walked = 0;
        ASSERT(chain_log_verify(station, &walked, NULL));
        ASSERT_EQ_INT((int)walked, (int)events_before);
        cargo_store_cleanup(&store_before);
    }
    chain_log_test_fault_clear();
    chain_log_set_dir(NULL);
}

/* Regression: a single buy_product intent must purchase exactly one
 * unit, not as-many-as-the-player-can-afford. The TRADE picker
 * advertises rows as "buy 1 frame for $X"; bulk-buy from one keypress
 * was charging the row's grade-multiplied price across the whole drain. */
TEST(test_docked_buy_one_unit_per_intent) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    world_seed_station_manifests(&w);
    station_t *st = &w.stations[1]; /* Kepler — produces frames */
    /* Mint legacy station stock too; BUY availability now comes from the
     * visible dock-held pod staged below, not the hidden manifest stock. */
    station_finished_mint(st, COMMODITY_FRAME, 50, NULL);
    int market_pod = construction_spawn_station_market_pod(
        &w, 1, COMMODITY_FRAME, 1, (const uint8_t *)"BUY1FRME");
    ASSERT(market_pod >= 0);

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    sp->docked = true;
    sp->current_station = 1;
    memset(sp->session_token, 0xAA, sizeof(sp->session_token));
    manifest_free(&sp->ship->manifest);
    ASSERT(manifest_init(&sp->ship->manifest, 16));
    ledger_credit_supply(st, sp->session_token, 5000.0f);
    float bal_before = ledger_balance(st, sp->session_token);
    float cargo_before = sp->ship->cargo[COMMODITY_FRAME];
    int tow_before = sp->ship->towed_pod_count;
    int station_frames_before = station_finished_count(st, COMMODITY_FRAME);

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FRAME;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(&w, SIM_DT);

    float cargo_delta = sp->ship->cargo[COMMODITY_FRAME] - cargo_before;
    float bal_delta = bal_before - ledger_balance(st, sp->session_token);
    ASSERT_EQ_FLOAT(cargo_delta, 0.0f, 0.01f);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, tow_before + 1);
    int pod_idx = sp->ship->towed_pods[tow_before];
    ASSERT(pod_idx >= 0 && pod_idx < MAX_CARGO_PODS);
    ASSERT_EQ_INT(pod_idx, market_pod);
    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].commodity, COMMODITY_FRAME);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].quantity, 1);
    ASSERT_EQ_INT(w.cargo_pods[pod_idx].manifest_count, 1);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME),
                  station_frames_before);
    ASSERT(bal_delta < 100.0f);
}

TEST(test_one_shipyard_builds_ships_two_shipyards_build_station_modules) {
    station_t st = {0};
    snprintf(st.name, sizeof(st.name), "%s", "Test Yard");
    st.signal_range = 1000.0f;
    add_module_at(&st, MODULE_SHIPYARD, 2, 0);
    add_module_at(&st, MODULE_FURNACE, 2, 1);

    ASSERT_EQ_INT(station_active_shipyard_count(&st), 1);
    ASSERT(station_can_order_scaffold(&st, MODULE_SHIPYARD));
    ASSERT(!station_can_order_scaffold(&st, MODULE_FURNACE));

    add_module_at(&st, MODULE_SHIPYARD, 2, 2);
    ASSERT_EQ_INT(station_active_shipyard_count(&st), 2);
    ASSERT(station_can_order_scaffold(&st, MODULE_FURNACE));
    station_cleanup(&st);
}

TEST(test_shipyard_commission_completes_onto_docked_player) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xA1, sizeof(sp->session_token));
    ASSERT(construction_make_verified_player(sp, 0xA1));
    sp->docked = true;
    sp->current_station = 1;
    sp->ship->hull_class = HULL_CLASS_NPC_MINER;
    sp->ship->hull = ship_max_hull(sp->ship);

    actor_principal_t owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(sp, &owner));
    ASSERT(shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[0].owner_principal, &owner));
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), 0);

    for (int i = 0; i < 4000; i++) world_sim_step(&w, 1.0f / 120.0f);
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT_EQ_INT(sp->ship->hull_class, HULL_CLASS_MINER);
    ASSERT_EQ_INT(ship_module_socket_count(sp->ship), 3);
    ASSERT(sp->ship_asset_id != SHIP_ASSET_ID_NONE);
    const ship_asset_t *asset = world_ship_asset_by_id_const(&w, sp->ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->provenance, SHIP_ASSET_PROVENANCE_SHIPYARD);
    ASSERT(actor_principal_equal(&asset->owner_principal, &owner));
    ASSERT_EQ_INT(asset->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(asset->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT(world_station_stored_hull_count(&w, 1, HULL_CLASS_MINER) >= 1);
}

TEST(test_shipyard_birth_assembly_consumes_three_fragments) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.fracture_claims, 0, sizeof(w.fracture_claims));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    ASSERT(station_has_module(st, MODULE_SHIPYARD));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));

    construction_seed_birth_fragment(&w, 0, COMMODITY_FERRITE_ORE,
                                     v2(st->pos.x - 640.0f, st->pos.y - 180.0f),
                                     0x10);
    construction_seed_birth_fragment(&w, 1, COMMODITY_CUPRITE_ORE,
                                     v2(st->pos.x + 620.0f, st->pos.y - 160.0f),
                                     0x40);
    construction_seed_birth_fragment(&w, 2, COMMODITY_CRYSTAL_ORE,
                                     v2(st->pos.x + 20.0f, st->pos.y + 720.0f),
                                     0x70);
    uint8_t expected_pubs[3][32];
    memcpy(expected_pubs[0], w.asteroids[0].fragment_pub, 32);
    memcpy(expected_pubs[1], w.asteroids[1].fragment_pub, 32);
    memcpy(expected_pubs[2], w.asteroids[2].fragment_pub, 32);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0xB1, sizeof(sp->session_token));
    ASSERT(construction_make_verified_player(sp, 0xB1));
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;

    ASSERT(shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY);
    ASSERT(w.ship_birth_assemblies[1][0].active);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), 0);

    for (int i = 0; i < 4000 && st->pending_ship_build_count > 0; i++)
        world_sim_step(&w, 1.0f / 120.0f);

    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT(!w.asteroids[0].active);
    ASSERT(!w.asteroids[1].active);
    ASSERT(!w.asteroids[2].active);
    ASSERT(sp->ship_asset_id != SHIP_ASSET_ID_NONE);
    const ship_asset_t *asset = world_ship_asset_by_id_const(&w, sp->ship_asset_id);
    ASSERT(asset != NULL);
    ASSERT_EQ_INT(asset->provenance, SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY);
    ASSERT_EQ_INT(
        asset->birth_proof_version,
        SHIP_BIRTH_PROOF_VERSION_V1);
    actor_principal_t owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(sp, &owner));
    ASSERT(actor_principal_equal(&asset->owner_principal, &owner));
    ASSERT(construction_bytes_any(asset->birth_soul_pub));
    ASSERT(construction_bytes_any(asset->birth_material_root));
    ASSERT(memcmp(asset->birth_fragment_pubs[0], expected_pubs[0], 32) == 0);
    ASSERT(memcmp(asset->birth_fragment_pubs[1], expected_pubs[1], 32) == 0);
    ASSERT(memcmp(asset->birth_fragment_pubs[2], expected_pubs[2], 32) == 0);
}

TEST(test_shipyard_birth_assembly_roundtrips_and_completes_offline) {
    const char *catalog_dir =
        TMP("test_birth_assembly_roundtrip_catalog");
    const char *save_path =
        TMP("test_birth_assembly_roundtrip.sav");
    WORLD_HEAP world = calloc(1, sizeof(world_t));
    ASSERT(world != NULL);
    world_reset(world);
    memset(world->asteroids, 0, sizeof(world->asteroids));
    memset(world->fracture_claims, 0,
           sizeof(world->fracture_claims));
    memset(world->cargo_pods, 0, sizeof(world->cargo_pods));

    station_t *station = &world->stations[1];
    ASSERT(test_set_station_finished_units(
        station, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(
        station, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(
        station, COMMODITY_TRACTOR_MODULE, 0));
    construction_seed_birth_fragment(
        world, 0, COMMODITY_FERRITE_ORE,
        v2_add(station->pos, v2(-640.0f, -180.0f)), 0x16);
    construction_seed_birth_fragment(
        world, 1, COMMODITY_CUPRITE_ORE,
        v2_add(station->pos, v2(620.0f, -160.0f)), 0x46);
    construction_seed_birth_fragment(
        world, 2, COMMODITY_CRYSTAL_ORE,
        v2_add(station->pos, v2(20.0f, 720.0f)), 0x76);

    server_player_t *player = &world->players[0];
    player->id = 0;
    player->connected = true;
    player->session_ready = true;
    memset(player->session_token, 0xB6,
           sizeof(player->session_token));
    ASSERT(construction_make_verified_player(player, 0xB6));
    player_init_ship(player, world);
    player->docked = true;
    player->current_station = 1;
    player->nearby_station = 1;
    player->in_dock_range = true;

    ASSERT(shipyard_queue_ship_commission(
        world, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(
        station->pending_ship_builds[0].mode,
        PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY);
    ASSERT(world->ship_birth_assemblies[1][0].active);
    actor_principal_t owner =
        station->pending_ship_builds[0].owner_principal;
    uint8_t fragment_pubs[
        SHIP_BIRTH_PROOF_FRAGMENT_COUNT][32];
    memcpy(fragment_pubs,
           world->ship_birth_assemblies[1][0].fragment_pubs,
           sizeof(fragment_pubs));

    ASSERT(station_catalog_save_all(
        world->stations, MAX_STATIONS, catalog_dir));
    ASSERT(world_save(world, save_path));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT_EQ_INT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS, catalog_dir),
        SIGNAL_SEEDED_STATION_COUNT);
    ASSERT(world_load(loaded, save_path));
    station_t *loaded_station = &loaded->stations[1];
    ASSERT_EQ_INT(loaded_station->pending_ship_build_count, 1);
    ASSERT(actor_principal_equal(
        &loaded_station->pending_ship_builds[0].owner_principal,
        &owner));
    ASSERT(loaded->ship_birth_assemblies[1][0].active);
    ASSERT(memcmp(
        loaded->ship_birth_assemblies[1][0].fragment_pubs,
        fragment_pubs, sizeof(fragment_pubs)) == 0);
    for (int i = 0; i < SHIP_BIRTH_PROOF_FRAGMENT_COUNT; i++) {
        int slot =
            loaded->ship_birth_assemblies[1][0].fragment_slots[i];
        ASSERT(slot >= 0 && slot < MAX_ASTEROIDS);
        ASSERT(memcmp(
            loaded->asteroids[slot].fragment_pub,
            fragment_pubs[i], 32) == 0);
    }

    for (int i = 0;
         i < 4000 &&
         loaded_station->pending_ship_build_count > 0;
         i++) {
        world_sim_step(loaded, 1.0f / 120.0f);
    }
    ASSERT_EQ_INT(loaded_station->pending_ship_build_count, 0);

    const ship_asset_t *completed = NULL;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *candidate =
            &loaded->ship_assets[i];
        if (candidate->active &&
            candidate->provenance ==
                SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY &&
            actor_principal_equal(
                &candidate->owner_principal, &owner)) {
            ASSERT(completed == NULL);
            completed = candidate;
        }
    }
    ASSERT(completed != NULL);
    ASSERT_EQ_INT(
        completed->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(
        completed->birth_proof_version,
        SHIP_BIRTH_PROOF_VERSION_V1);
    ASSERT(memcmp(
        completed->birth_fragment_pubs,
        fragment_pubs, sizeof(fragment_pubs)) == 0);
    uint8_t expected_soul[32];
    uint8_t expected_material[32];
    ASSERT(ship_birth_proof_compute_v1(
        completed->birth_fragment_pubs,
        completed->birth_fragment_grades,
        expected_soul, expected_material));
    ASSERT(memcmp(
        completed->birth_soul_pub,
        expected_soul, sizeof(expected_soul)) == 0);
    ASSERT(memcmp(
        completed->birth_material_root,
        expected_material, sizeof(expected_material)) == 0);

    remove(save_path);
}

TEST(test_shipyard_birth_fragments_cannot_back_two_commissions) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.fracture_claims, 0, sizeof(w.fracture_claims));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    ASSERT(station_has_module(st, MODULE_SHIPYARD));
    for (int p = 0; p < 2; p++) {
        server_player_t *sp = &w.players[p];
        sp->id = (uint8_t)p;
        memset(sp->session_token, 0xC2 + p, sizeof(sp->session_token));
        ASSERT(construction_make_verified_player(sp, (uint8_t)(0xC2 + p)));
        player_init_ship(sp, &w);
        sp->docked = true;
        sp->current_station = 1;
        sp->nearby_station = 1;
        sp->in_dock_range = true;
    }
    /* A second initial player may request a station loaner while spawning.
     * Keep this fixture scoped to the two explicit player commissions. */
    construction_clear_pending_hull_queues(&w);

    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    construction_seed_birth_fragment(
        &w, 0, COMMODITY_FERRITE_ORE, v2_add(st->pos, v2(-600.0f, 0.0f)),
        0x11);
    construction_seed_birth_fragment(
        &w, 1, COMMODITY_CUPRITE_ORE, v2_add(st->pos, v2(600.0f, 0.0f)),
        0x41);
    construction_seed_birth_fragment(
        &w, 2, COMMODITY_CRYSTAL_ORE, v2_add(st->pos, v2(0.0f, 700.0f)),
        0x71);

    actor_principal_t first_owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(
        &w.players[0], &first_owner));
    ASSERT(shipyard_queue_ship_commission(
        &w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY);
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[0].owner_principal, &first_owner));
    ASSERT(w.ship_birth_assemblies[1][0].active);

    int16_t reserved[3];
    memcpy(reserved, w.ship_birth_assemblies[1][0].fragment_slots,
           sizeof(reserved));
    ASSERT(!shipyard_queue_ship_commission(
        &w, 1, 1, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT(w.ship_birth_assemblies[1][0].active);
    ASSERT(memcmp(reserved,
                  w.ship_birth_assemblies[1][0].fragment_slots,
                  sizeof(reserved)) == 0);
    ASSERT(!w.ship_birth_assemblies[1][1].active);
}

TEST(test_shipyard_queue_compaction_moves_birth_sidecar_with_owner) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.fracture_claims, 0, sizeof(w.fracture_claims));
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    ASSERT(station_has_module(st, MODULE_SHIPYARD));
    for (int p = 0; p < 2; p++) {
        server_player_t *sp = &w.players[p];
        sp->id = (uint8_t)p;
        memset(sp->session_token, 0x92 + p, sizeof(sp->session_token));
        ASSERT(construction_make_verified_player(sp, (uint8_t)(0x92 + p)));
        player_init_ship(sp, &w);
        sp->docked = true;
        sp->current_station = 1;
        sp->nearby_station = 1;
        sp->in_dock_range = true;
    }
    construction_clear_pending_hull_queues(&w);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(
        HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(
        st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(
        st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(
        st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    ASSERT(shipyard_queue_ship_commission(
        &w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);
    ASSERT(!w.ship_birth_assemblies[1][0].active);

    construction_seed_birth_fragment(
        &w, 0, COMMODITY_FERRITE_ORE, v2_add(st->pos, v2(-640.0f, -80.0f)),
        0x12);
    construction_seed_birth_fragment(
        &w, 1, COMMODITY_CUPRITE_ORE, v2_add(st->pos, v2(620.0f, -60.0f)),
        0x42);
    construction_seed_birth_fragment(
        &w, 2, COMMODITY_CRYSTAL_ORE, v2_add(st->pos, v2(20.0f, 720.0f)),
        0x72);

    ASSERT(shipyard_queue_ship_commission(
        &w, 1, 1, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 2);
    ASSERT_EQ_INT(st->pending_ship_builds[1].mode,
                  PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY);
    ASSERT(w.ship_birth_assemblies[1][1].active);
    st->pending_ship_builds[1].owner_quarantine_record_id =
        UINT64_C(0x101);
    st->pending_ship_builds[1].mode_quarantine_record_id =
        UINT64_C(0x102);

    actor_principal_t birth_owner =
        st->pending_ship_builds[1].owner_principal;
    ship_birth_assembly_t birth_before =
        w.ship_birth_assemblies[1][1];
    for (int i = 0;
         i < 4000 && st->pending_ship_build_count == 2; i++) {
        world_sim_step(&w, 1.0f / 120.0f);
    }

    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY);
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[0].owner_principal, &birth_owner));
    ASSERT(st->pending_ship_builds[0].owner_quarantine_record_id ==
           UINT64_C(0x101));
    ASSERT(st->pending_ship_builds[0].mode_quarantine_record_id ==
           UINT64_C(0x102));
    ASSERT(w.ship_birth_assemblies[1][0].active);
    ASSERT(memcmp(
        w.ship_birth_assemblies[1][0].fragment_slots,
        birth_before.fragment_slots,
        sizeof(birth_before.fragment_slots)) == 0);
    ASSERT(memcmp(
        w.ship_birth_assemblies[1][0].fragment_pubs,
        birth_before.fragment_pubs,
        sizeof(birth_before.fragment_pubs)) == 0);
    ASSERT(!w.ship_birth_assemblies[1][1].active);
    ASSERT_EQ_INT(st->pending_ship_builds[1].mode,
                  PENDING_SHIP_BUILD_MODE_UNKNOWN);
}

TEST(test_shipyard_commission_owner_survives_player_slot_reuse) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    uint8_t original_token[8];
    memset(original_token, 0xD4, sizeof(original_token));
    server_player_t *original = &w.players[0];
    original->id = 0;
    original->connected = true;
    original->session_ready = true;
    memcpy(original->session_token, original_token, sizeof(original_token));
    ASSERT(construction_make_verified_player(original, 0xD4));
    player_init_ship(original, &w);
    original->docked = true;
    original->current_station = 1;
    original->nearby_station = 1;
    original->in_dock_range = true;

    actor_principal_t original_owner = actor_principal_none();
    ASSERT(actor_principal_from_verified_player(
        original, &original_owner));
    ASSERT(shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[0].owner_principal,
        &original_owner));

    (void)world_player_release_ship_asset(&w, 0);
    ship_cleanup(original->ship);
    memset(original, 0, sizeof(*original));

    server_player_t *replacement = &w.players[0];
    replacement->id = 0;
    replacement->connected = true;
    replacement->session_ready = true;
    /* Even reusing the old transport token and runtime slot cannot inherit
     * work owned by a different stable player principal. */
    memcpy(replacement->session_token, original_token,
           sizeof(replacement->session_token));
    ASSERT(construction_make_verified_player(replacement, 0xE5));
    player_init_ship(replacement, &w);
    replacement->docked = true;
    replacement->current_station = 1;
    replacement->nearby_station = 1;
    replacement->in_dock_range = true;

    for (int i = 0; i < 4000; i++) world_sim_step(&w, 1.0f / 120.0f);
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);

    uint32_t commissioned_id = SHIP_ASSET_ID_NONE;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w.ship_assets[i];
        if (!asset->active) continue;
        if (asset->provenance != SHIP_ASSET_PROVENANCE_SHIPYARD) continue;
        if (!actor_principal_equal(
                &asset->owner_principal, &original_owner)) continue;
        commissioned_id = asset->asset_id;
        break;
    }
    ASSERT(commissioned_id != SHIP_ASSET_ID_NONE);
    ASSERT(replacement->ship_asset_id != commissioned_id);

    const ship_asset_t *stored =
        world_ship_asset_by_id_const(&w, commissioned_id);
    ASSERT(stored != NULL);
    ASSERT_EQ_INT(stored->status, SHIP_ASSET_STATUS_STORED);
    ASSERT_EQ_INT(stored->operator_kind, SHIP_ASSET_OPERATOR_NONE);
    ASSERT_EQ_INT(stored->custody_station, 1);

    server_player_t *rejoin = &w.players[1];
    rejoin->id = 1;
    rejoin->connected = true;
    rejoin->session_ready = true;
    memset(rejoin->session_token, 0xF6, sizeof(rejoin->session_token));
    ASSERT(construction_make_verified_player(rejoin, 0xD4));
    player_init_ship(rejoin, &w);

    ASSERT_EQ_INT(rejoin->ship_asset_id, commissioned_id);
    const ship_asset_t *claimed =
        world_ship_asset_by_id_const(&w, commissioned_id);
    ASSERT(claimed != NULL);
    ASSERT(actor_principal_equal(
        &claimed->owner_principal, &original_owner));
    ASSERT_EQ_INT(claimed->status, SHIP_ASSET_STATUS_ASSIGNED);
    ASSERT_EQ_INT(claimed->operator_kind, SHIP_ASSET_OPERATOR_PLAYER);
    ASSERT_EQ_INT(claimed->operator_slot, 1);
}

TEST(test_shipyard_commission_debits_player_ledger) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));
    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->docked = true;
    sp->current_station = 1;
    memset(sp->session_token, 0xC1, sizeof(sp->session_token));
    ASSERT(construction_make_verified_player(sp, 0xC1));

    float expected =
        (float)frames * station_sell_price(st, COMMODITY_FRAME) +
        (float)lasers * station_sell_price(st, COMMODITY_LASER_MODULE) +
        (float)tractors * station_sell_price(st, COMMODITY_TRACTOR_MODULE);
    float before = ledger_balance_by_pubkey(st, sp->pubkey);

    ASSERT(shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);

    float after = ledger_balance_by_pubkey(st, sp->pubkey);
    ASSERT_EQ_FLOAT(before - after, expected, 0.01f);
}

TEST(test_shipyard_commission_consumes_towed_material_pods) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    int expected_pods = (frames > 0 ? 1 : 0) + (lasers > 0 ? 1 : 0) +
                        (tractors > 0 ? 1 : 0);
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->ship->hull_class = HULL_CLASS_HAULER;
    sp->ship->tractor_level = 1;
    memset(sp->session_token, 0xB7, sizeof(sp->session_token));
    ASSERT(construction_make_verified_player(sp, 0xB7));

    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_FRAME, frames, (const uint8_t *)"PODHULF1"));
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_LASER_MODULE, lasers, (const uint8_t *)"PODHULL1"));
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_TRACTOR_MODULE, tractors, (const uint8_t *)"PODHULT1"));
    ASSERT(sp->ship->towed_pod_count > 0);
    sp->ship->pos = st->pos;

    ASSERT(!shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, expected_pods);

    construction_stage_towed_pods_at_hoppers(&w, st, sp->ship);

    float before = ledger_balance_by_pubkey(st, sp->pubkey);
    ASSERT(shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));

    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), 0);
    ASSERT(ledger_balance_by_pubkey(st, sp->pubkey) < before - 0.01f);
}

TEST(test_shipyard_station_request_consumes_staged_material_pods) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(frames > 0 && frames <= CARGO_POD_MANIFEST_CAP);
    ASSERT(lasers > 0 && lasers <= CARGO_POD_MANIFEST_CAP);
    ASSERT(tractors > 0 && tractors <= CARGO_POD_MANIFEST_CAP);
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w.stations[s])) continue;
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_FRAME, 0));
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_LASER_MODULE, 0));
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_TRACTOR_MODULE, 0));
    }

    vec2 frame_hopper = st->pos;
    vec2 laser_hopper = st->pos;
    vec2 tractor_hopper = st->pos;
    ASSERT(construction_hopper_pos_for(st, COMMODITY_FRAME, &frame_hopper));
    ASSERT(construction_hopper_pos_for(st, COMMODITY_LASER_MODULE,
                                       &laser_hopper));
    ASSERT(construction_hopper_pos_for(st, COMMODITY_TRACTOR_MODULE,
                                       &tractor_hopper));
    int frame_hopper_idx = construction_hopper_idx_for(st, COMMODITY_FRAME);
    int laser_hopper_idx = construction_hopper_idx_for(st, COMMODITY_LASER_MODULE);
    int tractor_hopper_idx = construction_hopper_idx_for(st, COMMODITY_TRACTOR_MODULE);
    ASSERT(frame_hopper_idx >= 0);
    ASSERT(laser_hopper_idx >= 0);
    ASSERT(tractor_hopper_idx >= 0);

    int frame_pod = construction_spawn_loose_material_pod(
        &w, st->pos, COMMODITY_FRAME, frames,
        (const uint8_t *)"LOOSHULF");
    int laser_pod = construction_spawn_loose_material_pod(
        &w, st->pos, COMMODITY_LASER_MODULE, lasers,
        (const uint8_t *)"LOOSHULL");
    int tractor_pod = construction_spawn_loose_material_pod(
        &w, st->pos, COMMODITY_TRACTOR_MODULE, tractors,
        (const uint8_t *)"LOOSHULT");
    ASSERT(frame_pod >= 0);
    ASSERT(laser_pod >= 0);
    ASSERT(tractor_pod >= 0);

    ASSERT(!shipyard_queue_station_hull_request(&w, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT(w.cargo_pods[frame_pod].active);
    ASSERT(w.cargo_pods[laser_pod].active);
    ASSERT(w.cargo_pods[tractor_pod].active);

    w.cargo_pods[frame_pod].pos = frame_hopper;
    w.cargo_pods[laser_pod].pos = laser_hopper;
    w.cargo_pods[tractor_pod].pos = tractor_hopper;
    ASSERT(world_cargo_pod_set_module_tractor(
        &w, frame_pod, 1, frame_hopper_idx));
    ASSERT(world_cargo_pod_set_module_tractor(
        &w, laser_pod, 1, laser_hopper_idx));
    ASSERT(world_cargo_pod_set_module_tractor(
        &w, tractor_pod, 1, tractor_hopper_idx));

    ASSERT(shipyard_queue_station_hull_request(&w, 0, HULL_CLASS_MINER));

    ASSERT_EQ_INT(st->pending_ship_build_count, 1);
    actor_principal_t station_owner = actor_principal_none();
    ASSERT(actor_principal_from_station(&w, 0, &station_owner));
    ASSERT(actor_principal_equal(
        &st->pending_ship_builds[0].owner_principal, &station_owner));
    ASSERT_EQ_INT(st->pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);
    ASSERT(!w.cargo_pods[frame_pod].active);
    ASSERT(!w.cargo_pods[laser_pod].active);
    ASSERT(!w.cargo_pods[tractor_pod].active);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), 0);
}

TEST(test_shipyard_station_request_rejects_far_staged_hoppers) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    construction_reset_station_modules(st);
    add_module_at(st, MODULE_SHIPYARD, 1, 0);
    add_hopper_for(st, 3, 0, COMMODITY_FRAME);
    add_hopper_for(st, 3, 3, COMMODITY_LASER_MODULE);
    add_hopper_for(st, 3, 6, COMMODITY_TRACTOR_MODULE);
    rebuild_station_services(st);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w.stations[s])) continue;
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_FRAME, 0));
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_LASER_MODULE, 0));
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_TRACTOR_MODULE, 0));
    }

    vec2 yard_pos = module_world_pos_ring(st, 1, 0);
    vec2 frame_hopper = st->pos;
    vec2 laser_hopper = st->pos;
    vec2 tractor_hopper = st->pos;
    ASSERT(construction_hopper_pos_for(st, COMMODITY_FRAME, &frame_hopper));
    ASSERT(construction_hopper_pos_for(st, COMMODITY_LASER_MODULE,
                                       &laser_hopper));
    ASSERT(construction_hopper_pos_for(st, COMMODITY_TRACTOR_MODULE,
                                       &tractor_hopper));
    ASSERT(v2_dist_sq(yard_pos, frame_hopper) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(yard_pos, laser_hopper) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(yard_pos, tractor_hopper) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);

    int frame_pod = construction_spawn_loose_material_pod(
        &w, frame_hopper, COMMODITY_FRAME, frames,
        (const uint8_t *)"FARYRDF1");
    int laser_pod = construction_spawn_loose_material_pod(
        &w, laser_hopper, COMMODITY_LASER_MODULE, lasers,
        (const uint8_t *)"FARYRDL1");
    int tractor_pod = construction_spawn_loose_material_pod(
        &w, tractor_hopper, COMMODITY_TRACTOR_MODULE, tractors,
        (const uint8_t *)"FARYRDT1");
    ASSERT(frame_pod >= 0);
    ASSERT(laser_pod >= 0);
    ASSERT(tractor_pod >= 0);

    ASSERT(!shipyard_queue_station_hull_request(&w, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT(w.cargo_pods[frame_pod].active);
    ASSERT(w.cargo_pods[laser_pod].active);
    ASSERT(w.cargo_pods[tractor_pod].active);
}

TEST(test_shipyard_station_request_rejects_split_yard_materials) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    construction_setup_split_shipyard_materials(st);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w.stations[s])) continue;
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_FRAME, 0));
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_LASER_MODULE, 0));
        ASSERT(test_set_station_finished_units(&w.stations[s],
                                               COMMODITY_TRACTOR_MODULE, 0));
    }

    vec2 yard_a = module_world_pos_ring(st, 1, 0);
    vec2 yard_b = module_world_pos_ring(st, 3, 4);
    vec2 frame_hopper = st->pos;
    vec2 laser_hopper = st->pos;
    vec2 tractor_hopper = st->pos;
    ASSERT(construction_hopper_pos_for(st, COMMODITY_FRAME, &frame_hopper));
    ASSERT(construction_hopper_pos_for(st, COMMODITY_LASER_MODULE,
                                       &laser_hopper));
    ASSERT(construction_hopper_pos_for(st, COMMODITY_TRACTOR_MODULE,
                                       &tractor_hopper));
    ASSERT(v2_dist_sq(frame_hopper, yard_a) <=
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(frame_hopper, yard_b) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(laser_hopper, yard_b) <=
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(laser_hopper, yard_a) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(tractor_hopper, yard_b) <=
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(tractor_hopper, yard_a) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);

    int frame_pod = construction_spawn_loose_material_pod(
        &w, frame_hopper, COMMODITY_FRAME, frames,
        (const uint8_t *)"SPLTYRDF");
    int laser_pod = construction_spawn_loose_material_pod(
        &w, laser_hopper, COMMODITY_LASER_MODULE, lasers,
        (const uint8_t *)"SPLTYRDL");
    int tractor_pod = construction_spawn_loose_material_pod(
        &w, tractor_hopper, COMMODITY_TRACTOR_MODULE, tractors,
        (const uint8_t *)"SPLTYRDT");
    ASSERT(frame_pod >= 0);
    ASSERT(laser_pod >= 0);
    ASSERT(tractor_pod >= 0);

    ASSERT(!shipyard_queue_station_hull_request(&w, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT(w.cargo_pods[frame_pod].active);
    ASSERT(w.cargo_pods[laser_pod].active);
    ASSERT(w.cargo_pods[tractor_pod].active);
}

TEST(test_shipyard_player_commission_rejects_split_yard_towed_materials) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    construction_setup_split_shipyard_materials(st);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    sp->ship->hull_class = HULL_CLASS_HAULER;
    sp->ship->tractor_level = 1;
    memset(sp->session_token, 0xD5, sizeof(sp->session_token));
    ASSERT(construction_make_verified_player(sp, 0xD5));

    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_FRAME, frames, (const uint8_t *)"SPLTPYDF"));
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_LASER_MODULE, lasers,
        (const uint8_t *)"SPLTPYDL"));
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_TRACTOR_MODULE, tractors,
        (const uint8_t *)"SPLTPYDT"));
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 3);
    construction_stage_towed_pods_at_hoppers(&w, st, sp->ship);

    ASSERT(!shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    /* A rejected authorization/yard plan is transactionally inert: the
     * caller retains every tow binding and material pod. */
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 3);
    ASSERT_EQ_INT(construction_count_exact_pod_units(&w, COMMODITY_FRAME),
                  frames);
    ASSERT_EQ_INT(construction_count_exact_pod_units(&w, COMMODITY_LASER_MODULE),
                  lasers);
    ASSERT_EQ_INT(construction_count_exact_pod_units(&w, COMMODITY_TRACTOR_MODULE),
                  tractors);
}

TEST(test_shipyard_station_request_rejects_inventory_without_yard_hoppers) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    construction_reset_station_modules(st);
    add_module_at(st, MODULE_SHIPYARD, 1, 0);
    rebuild_station_services(st);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    ASSERT(!shipyard_queue_station_hull_request(&w, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), frames);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), lasers);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), tractors);
}

TEST(test_shipyard_player_commission_rejects_inventory_without_yard_hoppers) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    construction_reset_station_modules(st);
    add_module_at(st, MODULE_SHIPYARD, 1, 0);
    rebuild_station_services(st);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    server_player_t *sp = &w.players[0];
    player_init_ship(sp, &w);
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;
    memset(sp->session_token, 0xE6, sizeof(sp->session_token));
    ASSERT(construction_make_verified_player(sp, 0xE6));

    ASSERT(!shipyard_queue_ship_commission(&w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), frames);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), lasers);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), tractors);
}

TEST(test_shipyard_manufacture_consumes_staged_material_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    module_type_t type = MODULE_HOPPER;
    commodity_t mat = module_build_material_lookup(type);
    int units = (int)ceilf(module_build_cost_lookup(type) - 0.0001f);
    ASSERT_EQ_INT(mat, COMMODITY_FRAME);
    ASSERT(units > 0 && units <= CARGO_POD_MANIFEST_CAP);
    ASSERT(test_set_station_finished_units(st, mat, 0));
    st->_inventory_cache[mat] = 0.0f;

    vec2 pod_pos = st->pos;
    ASSERT(construction_hopper_pos_for(st, mat, &pod_pos));
    int pod_idx = construction_spawn_loose_material_pod(
        &w, pod_pos, mat, units, (const uint8_t *)"PODSCF01");
    ASSERT(pod_idx >= 0);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[pod_idx]), -1);

    st->pending_scaffolds[0].type = type;
    st->pending_scaffolds[0].owner = -1;
    st->pending_scaffold_count = 1;

    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(st->pending_scaffold_count, 0);
    ASSERT(!w.cargo_pods[pod_idx].active);
    ASSERT_EQ_INT(station_finished_count(st, mat), 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[mat], 0.0f, 0.001f);

    bool found = false;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w.scaffolds[i];
        if (!sc->active) continue;
        if (sc->module_type == type && sc->state == SCAFFOLD_LOOSE) {
            found = true;
            break;
        }
    }
    ASSERT(found);
}

TEST(test_shipyard_commission_rejects_invalid_owner_without_draining_materials) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(station_finished_mint(st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    int pending_before = st->pending_ship_build_count;
    int frames_before = station_finished_count(st, COMMODITY_FRAME);
    int lasers_before = station_finished_count(st, COMMODITY_LASER_MODULE);
    int tractors_before = station_finished_count(st, COMMODITY_TRACTOR_MODULE);

    ASSERT(!shipyard_queue_ship_commission(&w, 1, MAX_PLAYERS, HULL_CLASS_MINER));
    ASSERT(!shipyard_queue_ship_commission(&w, 1, INT8_MAX + 1, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, pending_before);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), frames_before);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), lasers_before);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE), tractors_before);
}

TEST(test_shipyard_commission_rejects_session_only_owner_without_draining) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    if (!station_has_module(st, MODULE_SHIPYARD))
        add_module_at(st, MODULE_SHIPYARD, 2, 0);

    int frames = 0, lasers = 0, tractors = 0;
    ASSERT(shipyard_hull_cost(
        HULL_CLASS_MINER, &frames, &lasers, &tractors));
    ASSERT(test_set_station_finished_units(st, COMMODITY_FRAME, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_LASER_MODULE, 0));
    ASSERT(test_set_station_finished_units(st, COMMODITY_TRACTOR_MODULE, 0));
    ASSERT(station_finished_mint(
        st, COMMODITY_FRAME, frames, NULL) == frames);
    ASSERT(station_finished_mint(
        st, COMMODITY_LASER_MODULE, lasers, NULL) == lasers);
    ASSERT(station_finished_mint(
        st, COMMODITY_TRACTOR_MODULE, tractors, NULL) == tractors);

    server_player_t *sp = &w.players[0];
    sp->id = 0;
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x6A, sizeof(sp->session_token));
    player_init_ship(sp, &w);
    sp->docked = true;
    sp->current_station = 1;
    sp->nearby_station = 1;
    sp->in_dock_range = true;

    ASSERT(!sp->pubkey_identity_finalized);
    ASSERT(!shipyard_queue_ship_commission(
        &w, 1, 0, HULL_CLASS_MINER));
    ASSERT_EQ_INT(st->pending_ship_build_count, 0);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_FRAME), frames);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_LASER_MODULE), lasers);
    ASSERT_EQ_INT(station_finished_count(st, COMMODITY_TRACTOR_MODULE),
                  tractors);
}

/* Regression: world_seed_station_manifests populates each active
 * station's manifest from its float inventory so the manifest-only
 * TRADE picker has rows to surface. The singleplayer init path must
 * call this for parity with the dedicated server. */
TEST(test_world_seed_station_manifests_matches_float) {
    WORLD_DECL;
    world_reset(&w);
    int expected[MAX_STATIONS][COMMODITY_COUNT] = {{0}};
    for (int i = 0; i < 3; i++) {
        int expected_count = i == 1 ? 8 : 0;
        ASSERT_EQ_INT(w.stations[i].manifest.count, expected_count);
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
            expected[i][c] = station_finished_count(&w.stations[i],
                                                    (commodity_t)c);
    }
    world_seed_station_manifests(&w);
    for (int s = 0; s < 3; s++) {
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
            int got = manifest_count_by_commodity(&w.stations[s].manifest,
                                                  (commodity_t)c);
            ASSERT_EQ_INT(got, expected[s][c]);
            ASSERT_EQ_FLOAT(w.stations[s]._inventory_cache[c], 0.0f, 0.0f);
        }
    }
}

TEST(test_kepler_starts_with_frame_pod_not_frame_inventory) {
    WORLD_DECL;
    world_reset(&w);

    station_t *kepler = &w.stations[1];
    ASSERT_EQ_INT(station_finished_count(kepler, COMMODITY_FRAME), 0);
    ASSERT_EQ_INT(station_finished_count(kepler,
                                         COMMODITY_LASER_MODULE), 8);
    ASSERT_EQ_FLOAT(kepler->_inventory_cache[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(kepler->_inventory_cache[COMMODITY_LASER_MODULE],
                    0.0f, 0.001f);

    int frame_pod = -1;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w.cargo_pods[i];
        if (!pod->active) continue;
        if (pod->kind != CARGO_POD_CARGO) continue;
        if (pod->commodity != COMMODITY_FRAME) continue;
        if (pod->quantity != 16) continue;
        if (pod->shipment_id != 0) continue;
        if (v2_dist_sq(pod->pos, kepler->pos) > 800.0f * 800.0f) continue;
        frame_pod = i;
        break;
    }

    ASSERT(frame_pod >= 0);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&w.cargo_pods[frame_pod]), -1);
}

TEST(test_module_activation_does_not_spawn_free_worker_hull) {
    WORLD_DECL;
    world_reset(&w);
    int npc_before = construction_count_active_npcs(&w);
    int asset_before = construction_count_active_ship_assets(&w);
    /* Build a furnace on Kepler. FURNACE accepts any ore — plant a
     * ferrite ore hopper to satisfy its input. */
    station_t *st = &w.stations[1];
    add_hopper_for(st, 3, 1, COMMODITY_FERRITE_ORE);
    int build_slot = station_ring_free_slot(
        st, 2, STATION_RING_SLOTS[2]);
    ASSERT(build_slot >= 0);
    begin_module_construction_at(&w, st, 1, MODULE_FURNACE, 2, build_slot);
    station_module_t *furnace = &st->modules[st->module_count - 1];
    ASSERT_EQ_INT(furnace->type, MODULE_FURNACE);
    ASSERT(furnace->scaffold);
    int pending_hulls_before = st->pending_ship_build_count;

    /* Route station inventory into the scaffold, then finish the build timer
     * by calling activation directly. This must activate the module but leave
     * worker provisioning to the roster/shipyard pass. */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    ASSERT(test_set_station_finished_amount(
        st, mat, module_build_cost_lookup(MODULE_FURNACE)));
    ASSERT(test_anchor_station_legacy_cargo(&w, 1));
    step_module_activation(&w, SIM_DT);
    ASSERT_EQ_INT(module_build_state(furnace), MODULE_BUILD_BUILDING);

    step_module_activation(&w, MODULE_BUILD_TIME_SEC);

    ASSERT(!furnace->scaffold);
    ASSERT_EQ_FLOAT(furnace->build_progress, 1.0f, 0.001f);
    ASSERT_EQ_INT(construction_count_active_npcs(&w), npc_before);
    ASSERT_EQ_INT(construction_count_active_ship_assets(&w), asset_before);
    ASSERT_EQ_INT(st->pending_ship_build_count, pending_hulls_before);
}

TEST(test_238_station_core_blocks_player) {
    /* Issue 1: player should not fly through station center.
     * Place player on a collision course with station 0 core. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;
    float st_r = w->stations[0].radius; /* 40 */
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius; /* 16 */

    /* Start 200 units away, heading straight at center */
    w->players[0].ship->pos = v2(st_pos.x + 200.0f, st_pos.y);
    w->players[0].ship->vel = v2(-500.0f, 0.0f);

    /* Run 120 ticks (~1 second) */
    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    float dist = v2_len(v2_sub(w->players[0].ship->pos, st_pos));
    float min_allowed = st_r + 4.0f + ship_r;
    /* Player must be outside the core collision boundary */
    ASSERT(dist >= min_allowed - 1.0f);
}

TEST(test_238_module_circle_blocks_player) {
    /* Module collision circles should block the player.
     * Fly directly at the signal relay on ring 1, slot 1 of station 0. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 mod_pos = module_world_pos_ring(&w->stations[0], 1, 1);
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius;

    /* Approach from outside, heading at module */
    vec2 approach_dir = v2_norm(v2_sub(mod_pos, w->stations[0].pos));
    w->players[0].ship->pos = v2_add(mod_pos, v2_scale(approach_dir, 100.0f));
    w->players[0].ship->vel = v2_scale(approach_dir, -400.0f);

    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    float dist = v2_len(v2_sub(w->players[0].ship->pos, mod_pos));
    float min_allowed = 34.0f /* MODULE_COLLISION_RADIUS */ + ship_r;
    ASSERT(dist >= min_allowed - 2.0f);
}

TEST(test_238_corridor_blocks_radial_approach) {
    /* Corridor between relay@1 and furnace@2 on ring 1 of station 0.
     * Dock@0 is skipped, so the relay-furnace corridor should block.
     * Approach radially — should be pushed out. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;

    /* Midpoint angle between slot 1 and slot 2 on ring 1 (accounts for ring_offset) */
    float ang1 = module_angle_ring(&w->stations[0], 1, 1);
    float ang2 = module_angle_ring(&w->stations[0], 1, 2);
    float mid_ang = (ang1 + ang2) * 0.5f;
    float ring_r = 180.0f; /* STATION_RING_RADIUS[1] */

    /* Place player at the ring radius at the corridor midpoint, approaching inward */
    w->players[0].ship->pos = v2_add(st_pos, v2(cosf(mid_ang) * (ring_r + 60.0f), sinf(mid_ang) * (ring_r + 60.0f)));
    vec2 inward = v2_norm(v2_sub(st_pos, w->players[0].ship->pos));
    w->players[0].ship->vel = v2_scale(inward, 300.0f);

    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    /* Player should have been pushed to outer edge of corridor band.
     * Corridor outer edge = ring_r + CORRIDOR_HW + ship_r */
    float dist_from_center = v2_len(v2_sub(w->players[0].ship->pos, st_pos));
    float corridor_hw = 10.0f; /* CORRIDOR_HW */
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius;
    float outer_edge = ring_r + corridor_hw + ship_r;
    /* Player should be at or beyond the outer edge (pushed out) */
    ASSERT(dist_from_center >= outer_edge - 2.0f);
}

TEST(test_238_fragment_collides_with_corridor_wall) {
    WORLD_HEAP w = setup_collision_world_heap();

    vec2 st_pos = w->stations[0].pos;
    float ang1 = module_angle_ring(&w->stations[0], 1, 1);
    float ang2 = module_angle_ring(&w->stations[0], 1, 2);
    float mid_ang = (ang1 + ang2) * 0.5f;
    float ring_r = STATION_RING_RADIUS[1];
    float radius = 8.0f;

    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->radius = radius;
    a->hp = 10.0f;
    a->max_hp = 10.0f;
    a->pos = v2_add(st_pos, v2(cosf(mid_ang) * (ring_r + STATION_CORRIDOR_HW + radius - 3.0f),
                               sinf(mid_ang) * (ring_r + STATION_CORRIDOR_HW + radius - 3.0f)));
    vec2 radial = v2_norm(v2_sub(a->pos, st_pos));
    a->vel = v2_scale(radial, -100.0f);

    resolve_asteroid_station_collisions(w);

    float dist_from_center = v2_len(v2_sub(a->pos, st_pos));
    float outer_edge = ring_r + STATION_CORRIDOR_HW + radius;
    ASSERT(dist_from_center >= outer_edge);
    ASSERT(v2_dot(a->vel, radial) >= -0.01f);
}

TEST(test_238_dock_gap_allows_entry) {
    /* Rings are intentionally always open — the wrap-around corridor
     * is never emitted, so the largest empty arc is the entry gap.
     * Prospect ring 1: dock@0, relay@1, furnace@2. Corridors are
     * dock→relay and relay→furnace. The open gap is from furnace@2
     * (240°) wrapping back to dock@0 (0°/360°), midpoint ~300°. */
    WORLD_HEAP w = setup_collision_world_heap();

    vec2 st_pos = w->stations[0].pos;
    float ring_r = 180.0f; /* STATION_RING_RADIUS[1] */

    float furnace_ang = module_angle_ring(&w->stations[0], 1, 2);
    /* Forward arc from slot 2 around to slot 0 spans (3-2+0)/3 of
     * the circle = 1/3 = 120°. Midpoint sits 60° past furnace. */
    float gap_mid = furnace_ang + (TWO_PI_F / 3.0f) * 0.5f;
    vec2 outside = v2_add(st_pos, v2(cosf(gap_mid) * (ring_r + 80.0f), sinf(gap_mid) * (ring_r + 80.0f)));
    vec2 inside_target = v2_add(st_pos, v2(cosf(gap_mid) * (ring_r - 80.0f), sinf(gap_mid) * (ring_r - 80.0f)));

    w->players[0].ship->pos = outside;
    vec2 dir = v2_norm(v2_sub(inside_target, outside));
    w->players[0].ship->vel = v2_scale(dir, 200.0f);

    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    float dist_from_center = v2_len(v2_sub(w->players[0].ship->pos, st_pos));
    ASSERT(dist_from_center < ring_r);
}

TEST(test_238_corridor_angular_edge_no_clip) {
    /* Corridor between relay@1 and furnace@2 on ring 1.
     * Approach at the angular edge near the furnace end — should not clip through. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;
    float ring_r = 180.0f; /* STATION_RING_RADIUS[1] */

    /* Furnace at slot 2 on ring 1 — approach from just before its angle */
    float slot2_ang = module_angle_ring(&w->stations[0], 1, 2);
    float test_ang = slot2_ang - 0.02f; /* just inside corridor end */
    w->players[0].ship->pos = v2_add(st_pos, v2(cosf(test_ang) * (ring_r + 50.0f), sinf(test_ang) * (ring_r + 50.0f)));
    vec2 inward = v2_norm(v2_sub(st_pos, w->players[0].ship->pos));
    w->players[0].ship->vel = v2_scale(inward, 300.0f);

    for (int i = 0; i < 60; i++)
        world_sim_step(w, SIM_DT);

    float dist = v2_len(v2_sub(w->players[0].ship->pos, st_pos));
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius;
    float outer_edge = ring_r + 10.0f + ship_r;
    ASSERT(dist >= outer_edge - 2.0f);
}

TEST(test_238_module_corridor_junction_no_jitter) {
    /* Place player at the junction between a module circle and a corridor arc.
     * Run 240 ticks. Ship should settle — not oscillate between collision handlers. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;
    float ring_r = 340.0f;

    /* Stop ring rotation so module positions are stable during test */
    w->stations[0].arm_speed[0] = 0.0f;
    w->stations[0].arm_speed[1] = 0.0f;
    w->stations[0].arm_rotation[0] = 0.0f;
    w->stations[0].arm_rotation[1] = 0.0f;

    /* Furnace at slot 2 on ring 2 — get actual module angle */
    float mod_ang = module_angle_ring(&w->stations[0], 2, 2);
    /* Place ship just corridor-side of the module at the ring radius */
    float junction_ang = mod_ang - 0.05f;
    w->players[0].ship->pos = v2_add(st_pos, v2(cosf(junction_ang) * ring_r, sinf(junction_ang) * ring_r));
    w->players[0].ship->vel = v2(0.0f, 0.0f);

    /* Record position every 30 ticks, check for oscillation */
    vec2 positions[8];
    for (int snap = 0; snap < 8; snap++) {
        positions[snap] = w->players[0].ship->pos;
        for (int i = 0; i < 30; i++)
            world_sim_step(w, SIM_DT);
    }

    /* Check that ship settled — last 4 snapshots should be within 5 units of each other */
    float max_drift = 0.0f;
    for (int i = 5; i < 8; i++) {
        float d = v2_len(v2_sub(positions[i], positions[4]));
        if (d > max_drift) max_drift = d;
    }
    /* FAILS if collision handlers are fighting (ship jitters > 5 units) */
    ASSERT(max_drift < 5.0f);
}

TEST(test_238_invisible_wall_repro) {
    /* The original bug: player flying parallel to a corridor at the inflated
     * collision distance bounces off "nothing visible".
     * Test: fly tangentially just outside the visual corridor width (ring_r + hw)
     * but inside the collision band (ring_r + hw + ship_r). Should collide. */
    WORLD_HEAP w = setup_collision_world_heap();
    /* Suppress chunk materialization so terrain doesn't interfere with collision test */
    w->field_spawn_timer = -9999.0f;

    vec2 st_pos = w->stations[0].pos;
    float ring_r = 340.0f;
    float corridor_hw = 10.0f;
    (void)HULL_DEFS; /* ship_r available if needed */

    /* Midpoint of corridor between slot 1 and slot 2 */
    float mid_ang = TWO_PI_F * 1.5f / 6.0f;
    /* Place at ring_r + corridor_hw + 5 (inside collision band but outside visual) */
    float test_r = ring_r + corridor_hw + 5.0f; /* between visual edge and collision edge */
    w->players[0].ship->pos = v2_add(st_pos, v2(cosf(mid_ang) * test_r, sinf(mid_ang) * test_r));
    /* Fly tangentially (no radial component) */
    vec2 radial = v2_norm(v2_sub(w->players[0].ship->pos, st_pos));
    vec2 tangent = v2(-radial.y, radial.x);
    w->players[0].ship->vel = v2_scale(tangent, 200.0f);

    vec2 start_pos = w->players[0].ship->pos;
    for (int i = 0; i < 60; i++)
        world_sim_step(w, SIM_DT);

    /* The ship is inside the collision band (ring_r+hw+ship_r) but outside
     * the visual corridor (ring_r+hw). This IS the "invisible wall" —
     * the collision is correct (ship has physical radius) but the visual
     * doesn't show it. Verify the collision fires: ship should be pushed outward. */
    float start_r = v2_len(v2_sub(start_pos, st_pos));
    float end_r = v2_len(v2_sub(w->players[0].ship->pos, st_pos));
    /* Ship should have been pushed outward (end_r >= start_r) because it was
     * inside the collision band. If this FAILS, the collision isn't detecting
     * the ship at this distance. */
    ASSERT(end_r >= start_r - 1.0f);
}

TEST(test_station_geom_emitter_prospect) {
    /* Verify the geometry emitter produces correct shapes for Prospect.
	 * Cross-ring pair layout:
	 *   Ring 1: DOCK(0) + SIGNAL_RELAY(1) + FURNACE(2)
	 *   Ring 2: HOPPER(4) — paired with the ring-1 furnace.
	 * Folded frame pods are tractored by the furnace directly.
	 */
    WORLD_HEAP w = setup_collision_world_heap();
    w->rng = 2037u;
    world_reset(w);

    station_geom_t geom;
    station_build_geom(&w->stations[0], &geom);

    /* Core: Prospect has radius 40 */
    ASSERT(geom.has_core == true);

	/* Circles: dock (half-size) + relay + furnace (ring 1) + ferrite
	 * intake hopper (ring 2) = 4. */
	ASSERT_EQ_INT(geom.circle_count, 4);
    /* Corridors: ring 1 = 3 modules → 2 corridors. Ring 2 has only one
     * module so no within-ring corridor. */
    ASSERT_EQ_INT(geom.corridor_count, 2);

    /* Docks: 1 dock on ring 1 */
    ASSERT_EQ_INT(geom.dock_count, 1);
    ASSERT_EQ_INT(geom.spoke_count, 1);
    ASSERT_EQ_INT(geom.spokes[0].commodity, COMMODITY_FERRITE_ORE);

    /* The same catalog module records now emit a reinforced axial graph.
     * This is the physical construction view used by render/inspect. */
    ASSERT_EQ_INT(geom.cell_count, w->stations[0].module_count + 1);
    ASSERT_EQ_INT(geom.cells[0].node.shape, CELL_SHAPE_REINFORCED_HEX);
    ASSERT_EQ_INT(geom.cells[0].node.role, CELL_ROLE_HUB);
    ASSERT_EQ_FLOAT(geom.cells[0].center.x, w->stations[0].pos.x, 0.001f);
    ASSERT_EQ_FLOAT(geom.cells[0].center.y, w->stations[0].pos.y, 0.001f);
    ASSERT(cell_nodes_join(&geom.cells[0].node, &geom.cells[1].node));
    ASSERT_EQ_INT(geom.cells[1].node.shape, CELL_SHAPE_TRIANGLE);
    vec2 first_module_pos = module_world_pos_ring(
        &w->stations[0], w->stations[0].modules[0].ring,
        w->stations[0].modules[0].slot);
    ASSERT_EQ_FLOAT(geom.cells[1].center.x, first_module_pos.x, 0.001f);
    ASSERT_EQ_FLOAT(geom.cells[1].center.y, first_module_pos.y, 0.001f);
}

TEST(test_station_cell_topology_is_canonical_from_persisted_module_records) {
    station_t source = {0};
    source.id = 77;
    source.signal_range = 1.0f;
    add_furnace_for(&source, 1, 2, COMMODITY_FERRITE_INGOT);
    add_hopper_for(&source, 2, 4, COMMODITY_FERRITE_ORE);
    source.modules[1].scaffold = true;
    source.modules[1].build_progress = 0.375f;
    source.modules[1].input_buffer = 40.0f;
    source.modules[1].output_buffer = 12.0f;

    /* Catalog/save and station-identity wire records preserve this ordered
     * module slice. Reconstructing it must reproduce identical graph bytes. */
    station_t mirror = {0};
    mirror.id = source.id;
    mirror.signal_range = source.signal_range;
    mirror.module_count = source.module_count;
    memcpy(mirror.modules, source.modules,
           sizeof(source.modules[0]) * (size_t)source.module_count);

    cell_graph_t a, b;
    uint8_t ea[512], eb[512];
    size_t na = 0, nb = 0;
    ASSERT(station_cell_graph(&source, &a));
    ASSERT(station_cell_graph(&mirror, &b));
    ASSERT(cell_graph_encode(&a, ea, sizeof(ea), &na));
    ASSERT(cell_graph_encode(&b, eb, sizeof(eb), &nb));
    ASSERT_EQ_INT(na, nb);
    ASSERT(memcmp(ea, eb, na) == 0);
    ASSERT((b.nodes[2].flags & CELL_NODE_FLAG_SCAFFOLD) != 0);
    ASSERT_EQ_INT(b.nodes[2].payload_units, CELL_HEX_PAYLOAD_CAPACITY);
}

TEST(test_furnace_geom_spokes_use_instance_ore_tag) {
    station_t st = {0};
    station_geom_t geom;

    st.signal_range = 1.0f;
    add_furnace_for(&st, 1, 1, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&st, 2, 0, COMMODITY_FERRITE_ORE);
    add_hopper_for(&st, 2, 2, COMMODITY_CUPRITE_ORE);
    st.modules[0].active_pulse = 1.0f;

    station_build_geom(&st, &geom);

    ASSERT_EQ_INT(geom.spoke_count, 1);
    ASSERT_EQ_INT(geom.spokes[0].commodity, COMMODITY_CUPRITE_ORE);
}

TEST(test_station_geom_spoke_uses_module_diag_fallback) {
    station_t st = {0};
    station_geom_t geom;

    st.signal_range = 1.0f;
    add_furnace_for(&st, 1, 1, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&st, 2, 2, COMMODITY_CUPRITE_ORE);
    st.modules[0].active_pulse = 0.0f;
    st.modules[0].flow_diag = (uint8_t)STATION_FLOW_DIAG_RUNNING;

    station_build_geom(&st, &geom);

    ASSERT_EQ_INT(geom.spoke_count, 1);
    ASSERT(geom.spokes[0].pulse > 0.1f);

    st.modules[0].flow_diag = (uint8_t)STATION_FLOW_DIAG_CONSUMER_FULL;
    station_build_geom(&st, &geom);

    ASSERT_EQ_INT(geom.spoke_count, 1);
    ASSERT(geom.spokes[0].pulse <= 0.01f);
}

TEST(test_scaffold_spawn) {
    WORLD_DECL;
    world_reset(&w);

    /* Spawn a furnace scaffold near station 0 */
    vec2 spawn_pos = v2_add(w.stations[0].pos, v2(100.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, spawn_pos, 0);
    ASSERT(idx >= 0);
    ASSERT(idx < MAX_SCAFFOLDS);
    ASSERT(w.scaffolds[idx].active);
    ASSERT_EQ_INT(w.scaffolds[idx].module_type, MODULE_FURNACE);
    ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_LOOSE);
    ASSERT_EQ_INT(w.scaffolds[idx].owner, 0);
    ASSERT_EQ_INT(w.scaffolds[idx].placed_station, -1);
    ASSERT_EQ_INT(scaffold_tractor_player(&w.scaffolds[idx]), -1);
    ASSERT(w.scaffolds[idx].radius > 0.0f);

    /* Spawn fills slots until full */
    for (int i = 1; i < MAX_SCAFFOLDS; i++) {
        int s = spawn_scaffold(&w, MODULE_DOCK, spawn_pos, 0);
        ASSERT(s >= 0);
    }
    /* No free slots left */
    int overflow = spawn_scaffold(&w, MODULE_DOCK, spawn_pos, 0);
    ASSERT_EQ_INT(overflow, -1);
}

TEST(test_scaffold_physics_loose) {
    WORLD_DECL;
    world_reset(&w);

    /* Spawn scaffold with initial velocity */
    vec2 spawn_pos = v2_add(w.stations[0].pos, v2(200.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FRAME_PRESS, spawn_pos, 0);
    ASSERT(idx >= 0);
    w.scaffolds[idx].vel = v2(50.0f, 0.0f);

    vec2 start_pos = w.scaffolds[idx].pos;

    /* Run a few sim steps */
    for (int i = 0; i < 120; i++) {
        world_sim_step(&w, SIM_DT);
    }

    /* Scaffold should have moved from its starting position */
    float dist = v2_dist_sq(w.scaffolds[idx].pos, start_pos);
    ASSERT(dist > 1.0f);

    /* Age should have advanced */
    ASSERT(w.scaffolds[idx].age > 0.5f);

    /* Rotation should have advanced */
    ASSERT(w.scaffolds[idx].rotation != 0.0f);
}

TEST(test_scaffold_towed_scaffold_init) {
    WORLD_DECL;
    world_reset(&w);

    /* Player ship should start with no towed scaffold */
    SERVER_PLAYER_DECL(sp);
    sp.connected = true;
    player_init_ship(&sp, &w);
    ASSERT_EQ_INT(sp.ship->towed_scaffold, -1);
}

TEST(test_scaffold_tow_pickup) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;  /* hold R to grab */

    /* Spawn scaffold very close to the player */
    vec2 player_pos = w.players[0].ship->pos;
    vec2 scaffold_pos = v2_add(player_pos, v2(50.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, scaffold_pos, 0);
    ASSERT(idx >= 0);

    /* Run sim — player should pick up the scaffold */
    for (int i = 0; i < 10; i++) world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_scaffold, idx);
    ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_TOWING);
    ASSERT_EQ_INT(scaffold_tractor_player(&w.scaffolds[idx]), 0);
}

TEST(test_scaffold_tow_release_on_r) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;  /* hold R to grab */

    /* Spawn and attach scaffold */
    vec2 player_pos = w.players[0].ship->pos;
    int idx = spawn_scaffold(&w, MODULE_FURNACE, v2_add(player_pos, v2(50.0f, 0.0f)), 0);
    ASSERT(idx >= 0);
    for (int i = 0; i < 10; i++) world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(w.players[0].ship->towed_scaffold, idx);

    /* Tap R = release scaffold */
    w.players[0].input.tractor_hold = false;
    w.players[0].input.release_tow = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship->towed_scaffold, -1);
    ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_LOOSE);
    ASSERT_EQ_INT(scaffold_tractor_player(&w.scaffolds[idx]), -1);
}

TEST(test_scaffold_tow_release_on_dock) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].in_dock_range = false;
    w.players[0].input.tractor_hold = true;

    /* Spawn and manually attach scaffold */
    vec2 near_station = v2_add(w.stations[0].pos, v2(100.0f, 0.0f));
    w.players[0].ship->pos = near_station;
    w.players[0].ship->vel = v2(0.0f, 0.0f);
    int idx = spawn_scaffold(&w, MODULE_DOCK, v2_add(near_station, v2(50.0f, 0.0f)), 0);
    ASSERT(idx >= 0);
    /* Manually attach to avoid needing sim steps in dock approach range */
    w.scaffolds[idx].state = SCAFFOLD_TOWING;
    ASSERT(world_scaffold_set_player_tractor(&w, idx, 0));

    /* Now dock — scaffold should be released */
    w.players[0].nearby_station = 0;
    w.players[0].in_dock_range = true;
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);

    /* After docking, scaffold should be loose */
    if (w.players[0].docked) {
        ASSERT_EQ_INT(w.players[0].ship->towed_scaffold, -1);
        ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_LOOSE);
    }
}

TEST(test_scaffold_tow_speed_cap) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;

    /* Place player far from stations to avoid docking interference */
    w.players[0].ship->pos = v2(5000.0f, 5000.0f);
    w.players[0].ship->vel = v2(200.0f, 0.0f); /* moving fast */

    /* Spawn and manually attach scaffold */
    int idx = spawn_scaffold(&w, MODULE_FURNACE, v2(5050.0f, 5000.0f), 0);
    ASSERT(idx >= 0);
    w.scaffolds[idx].state = SCAFFOLD_TOWING;
    ASSERT(world_scaffold_set_player_tractor(&w, idx, 0));

    /* Run sim for a while */
    for (int i = 0; i < 240; i++) world_sim_step(&w, SIM_DT);

    /* Scaffold speed should be capped */
    float spd = v2_len(w.scaffolds[idx].vel);
    ASSERT(spd <= 60.0f); /* slightly above cap due to spring forces in single frame */
}

TEST(test_scaffold_snap_to_slot) {
    WORLD_DECL;
    world_reset(&w);

    /* We need a player outpost (index >= 3). Place one. */
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    
    /* credits are station-local (ledger) — no ship.credits field */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    /* Activate the outpost so it can accept scaffolds */
    w.stations[outpost].scaffold = false;
    w.stations[outpost].scaffold_progress = 1.0f;
    w.stations[outpost].signal_range = 6000.0f;
    w.stations[outpost].arm_count = 1;
    w.stations[outpost].arm_speed[0] = 0.04f;
    rebuild_signal_chain(&w);

    /* Count existing modules */
    int before_count = w.stations[outpost].module_count;

    /* Spawn a scaffold near ring 1 of the outpost */
    vec2 ring1_near = v2_add(outpost_pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, ring1_near, 0);
    ASSERT(idx >= 0);

    /* Run sim — station should grab it and pull it into a slot */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);

    /* Scaffold should have been consumed (deactivated) */
    ASSERT(!w.scaffolds[idx].active);

    /* Station should have a new module */
    ASSERT(w.stations[outpost].module_count == before_count + 1);

    /* The new module should be a furnace scaffold (under construction) */
    station_module_t *m = &w.stations[outpost].modules[before_count];
    ASSERT_EQ_INT(m->type, MODULE_FURNACE);
    ASSERT(m->scaffold); /* still under construction */
    ASSERT(m->ring >= 1);
}

TEST(test_scaffold_snap_ignores_starter_stations) {
    WORLD_DECL;
    world_reset(&w);

    /* Spawn scaffold near station 0 (starter station, index < 3) */
    vec2 near_prospect = v2_add(w.stations[0].pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, near_prospect, 0);
    ASSERT(idx >= 0);

    /* Run sim */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);

    /* Scaffold should still be active (not grabbed by starter station) */
    ASSERT(w.scaffolds[idx].active);
    ASSERT(w.scaffolds[idx].state != SCAFFOLD_SNAPPING);
}

TEST(test_scaffold_full_pipeline) {
    /* End-to-end: spawn → snap → supply → build timer → activate */
    WORLD_DECL;
    world_reset(&w);

    /* Create and activate a player outpost */
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;

    /* credits are station-local (ledger) — no ship.credits field */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    w.stations[outpost].scaffold = false;
    w.stations[outpost].scaffold_progress = 1.0f;
    w.stations[outpost].signal_range = 6000.0f;
    w.stations[outpost].arm_count = 1;
    w.stations[outpost].arm_speed[0] = 0.04f;
    /* Pre-supply founding module scaffolds so they don't compete */
    for (int mi = 0; mi < w.stations[outpost].module_count; mi++) {
        if (w.stations[outpost].modules[mi].scaffold)
            w.stations[outpost].modules[mi].build_progress = 1.0f;
    }
    rebuild_signal_chain(&w);

    int before_count = w.stations[outpost].module_count;

    /* Step 1: Spawn scaffold near ring 1 → station grabs it */
    vec2 ring1_near = v2_add(outpost_pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, ring1_near, 0);
    ASSERT(idx >= 0);

    /* Run until scaffold is consumed (snapped + placed as module) */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!w.scaffolds[idx].active);
    ASSERT(w.stations[outpost].module_count == before_count + 1);

    /* Post-placement: module enters supply phase (build_progress = 0) */
    station_module_t *m = &w.stations[outpost].modules[before_count];
    ASSERT_EQ_INT(m->type, MODULE_FURNACE);
    ASSERT(m->scaffold);
    ASSERT(!module_is_fully_supplied(m)); /* in supply phase, not pre-paid */

    /* Step 2: Deliver build material to advance supply phase.
     * Furnaces need frames — deposit into station inventory,
     * step_module_activation will route it to the scaffold. */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat, cost));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied, timer may have started */

    /* Step 3: Run construction timer (10s = 1200 ticks at 120Hz) */
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);

    /* Module should be fully activated */
    ASSERT(!m->scaffold);
    ASSERT_EQ_FLOAT(m->build_progress, 1.0f, 0.01f);
}

/* End-to-end: a player plants an outpost via tow, supplies its founding
 * frame quota by docking and dumping cargo, sims through scaffold +
 * seed-module activation, then plants and supplies a second module
 * (furnace). Asserts no credits leak, the outpost becomes dockable, the
 * furnace activates, and activation itself does not mint a worker hull. */
TEST(test_build_outpost_full_economy) {
    WORLD_DECL;
    world_reset(&w);

    server_player_t *sp = &w.players[0];
    uint8_t token[8] = {0xB1, 0xD9, 0x07, 0x12, 0x33, 0x44, 0x55, 0x66};
    memcpy(sp->session_token, token, 8);
    sp->id = 0;
    ASSERT(construction_make_verified_player(sp, 0xA0));
    ASSERT(registry_register_pubkey(&w, sp->pubkey, sp->session_token));

    player_init_ship(sp, &w);

    double credits_start = econ_total_credits(&w);

    /* Step 1 — buy exact frame pods from Kepler's physical dock
     * inventory, then PRESENT each while still docked at its source.
     * Purchase only releases a station-held pod into the player's tow;
     * the dedicated signed action performs the source-local TRADE +
     * TRANSFER batch and installs the first portable receipt on every
     * exact frame. No receipt sidecar is synthesized by this test. */
    float frame_budget =
        SCAFFOLD_MATERIAL_NEEDED                          /* outpost scaffold */
        + module_build_cost_lookup(MODULE_SIGNAL_RELAY);  /* seed module */
    int frame_units = (int)ceilf(frame_budget);
    station_t *source = &w.stations[1]; /* Kepler frame works */
    world_seed_station_manifests(&w);
    ASSERT(station_finished_mint(
        source, COMMODITY_FRAME, 50, NULL) == 50);
    sp->docked = true;
    sp->current_station = 1;
    sp->ship->pos = source->pos;
    ledger_earn_by_pubkey(source, sp->pubkey, 10000.0f);

    int presented = 0;
    uint16_t action_id = 1;
    int market_purchase = 0;
    while (presented < frame_units) {
        int batch_units = frame_units - presented;
        if (batch_units > 4) batch_units = 4;
        uint8_t origin[8] = {
            'F', 'O', 'U', 'N', 'D', 0, 0, 0,
        };
        origin[5] = (uint8_t)market_purchase;
        origin[6] = (uint8_t)(market_purchase >> 8);
        origin[7] = (uint8_t)(market_purchase >> 16);
        int founding_pod =
            construction_spawn_station_market_pod(
                &w, 1, COMMODITY_FRAME, batch_units,
                origin);
        ASSERT(founding_pod >= 0);
        ASSERT(test_anchor_pod_legacy_cargo(
            &w, 1, founding_pod));
        ASSERT(cargo_pod_has_module_tractor(
            &w.cargo_pods[founding_pod]));
        ASSERT_EQ_INT(cargo_pod_player_tractor(
                          &w.cargo_pods[founding_pod]), -1);

        sp->input.buy_product = true;
        sp->input.buy_commodity = COMMODITY_FRAME;
        sp->input.buy_grade = MINING_GRADE_COUNT;
        sp->input.buy_station_pod = true;
        sp->input.buy_station_pod_index =
            (uint16_t)founding_pod;
        world_sim_step(&w, SIM_DT);
        ASSERT_EQ_INT(sp->ship->towed_pod_count, 1);
        ASSERT_EQ_INT(
            sp->ship->towed_pods[0], founding_pod);
        ASSERT_EQ_INT(cargo_pod_player_tractor(
                          &w.cargo_pods[founding_pod]), 0);
        ASSERT(!cargo_pod_has_module_tractor(
            &w.cargo_pods[founding_pod]));
        ASSERT_EQ_INT(cargo_pod_custody_station(
                          &w.cargo_pods[founding_pod]), 1);

        uint8_t selection_token[32];
        ASSERT(server_cargo_pod_selection_token(
            &w, founding_pod, selection_token));
        uint8_t payload[35] = {0};
        payload[0] = (uint8_t)founding_pod;
        memcpy(&payload[1], selection_token,
               sizeof(selection_token));
        write_u16_le(&payload[33], action_id++);
        server_signed_action_dispatch_result_t dispatch = {0};
        ASSERT(server_dispatch_signed_action_payload(
            &w, 0, SIGNED_ACTION_PRESENT_POD,
            payload, sizeof(payload), NULL, NULL,
            &dispatch));
        ASSERT(dispatch.pod_present_evaluated);
        ASSERT_EQ_INT(dispatch.pod_present_result,
                      CARGO_POD_PRESENT_OK);
        ASSERT_EQ_INT(dispatch.pod_present_moved,
                      batch_units);
        presented += dispatch.pod_present_moved;
        ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
        ASSERT(!w.cargo_pods[founding_pod].active);
        ASSERT_EQ_INT(cargo_pod_player_tractor(
                          &w.cargo_pods[founding_pod]), -1);
        market_purchase++;
    }
    ASSERT_EQ_INT(presented, frame_units);
    ASSERT_EQ_INT(sp->ship->manifest.count, frame_units);
    const ship_receipts_t *founding_receipts =
        ship_get_receipts_const(sp->ship);
    ASSERT(founding_receipts != NULL);
    ASSERT_EQ_INT(founding_receipts->count, frame_units);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 0);
    for (int i = 0; i < frame_units; i++) {
        ASSERT_EQ_INT(
            sp->ship->manifest.units[i].origin_station, 1);
        ASSERT_EQ_INT(founding_receipts->chains[i].len, 1);
        ASSERT(memcmp(
            founding_receipts->chains[i]
                .links[0].recipient_pubkey,
            sp->pubkey, 32) == 0);
    }

    /* Step 2 — plant an outpost ~6kU east of Prospect via the tow flow.
     * The harness spawns a SIGNAL_RELAY scaffold, attaches it to the
     * player, and trips place_outpost — i.e. exactly what the client
     * does when the player presses E with a relay in tow. */
    sp->docked = false;
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(&w, sp, outpost_pos);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_t *st_out = &w.stations[outpost];
    ASSERT(st_out->scaffold);              /* under construction */
    ASSERT(st_out->signal_range > 0.0f);   /* relay seeded its range */
    int seed_mod_count = st_out->module_count; /* DOCK + seed SIGNAL_RELAY */
    ASSERT(seed_mod_count >= 2);

    /* The founding flow opens a CONTRACT_TRACTOR for FRAMES at the
     * outpost; the same flow we'd use to deliver to it. */
    bool found_frame_contract = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active
            && w.contracts[k].station_index == outpost
            && w.contracts[k].commodity == COMMODITY_FRAME) {
            found_frame_contract = true; break;
        }
    }
    ASSERT(found_frame_contract);

    /* Step 3 — dock at the outpost and pour in the Kepler-authored
     * receipt-backed frames.
     * The outpost has an OUTPOST_DOCK module stamped on by
     * place_towed_scaffold so docked-mode is valid here. */
    sp->docked = true;
    sp->current_station = outpost;
    sp->ship->pos = st_out->pos;

    /* service_sell triggers step_scaffold_delivery (advances the
     * station scaffold) and step_module_delivery (advances any module
     * scaffold). Run a few sells back-to-back — each sim step the
     * server clears the intent flag, so we re-arm it. */
    for (int i = 0; i < 10 && st_out->scaffold; i++) {
        sp->input.service_sell = true;
        sp->input.service_sell_only = COMMODITY_FRAME;
        world_sim_step(&w, SIM_DT);
    }

    /* Step 4 — the final accepted contribution activates the outpost
     * atomically; signal-chain rebuild is part of that transition. */
    ASSERT(!st_out->scaffold);
    ASSERT(st_out->signal_connected); /* tied back into the chain */

    /* Step 5 — advance the deterministic seed-module build timer directly.
     * Timer cadence has dedicated tick-level coverage; this scenario keeps
     * its focus on the cross-system economy/construction transitions. */
    step_module_activation(&w, MODULE_BUILD_TIME_SEC);
    for (int m = 0; m < st_out->module_count; m++) {
        ASSERT(!st_out->modules[m].scaffold);
    }

    /* Step 6 — plant a second module: a furnace. Spawn a furnace
     * scaffold near ring 1 and let the snap-to-slot path consume it,
     * the same flow test_scaffold_full_pipeline exercises. */
    int npc_before = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) if (w.npc_ships[i].active) npc_before++;

    sp->docked = false; /* leave so the snap step has clean state */
    int furnace_ring = -1;
    int furnace_slot = -1;
    for (int ring = 1;
         ring <= STATION_NUM_RINGS && furnace_ring < 0; ring++) {
        for (int slot = 0; slot < STATION_RING_SLOTS[ring]; slot++) {
            if (station_placement_validate(
                    st_out, MODULE_FURNACE, ring, slot,
                    STATION_PLACEMENT_SCAFFOLD) !=
                STATION_PLACEMENT_OK) {
                continue;
            }
            furnace_ring = ring;
            furnace_slot = slot;
            break;
        }
    }
    ASSERT(furnace_ring >= 0 && furnace_slot >= 0);
    vec2 furnace_target = module_world_pos_ring(
        st_out, furnace_ring, furnace_slot);
    int sc_idx = spawn_scaffold(
        &w, MODULE_FURNACE, furnace_target, sp->id);
    ASSERT(sc_idx >= 0);
    w.scaffolds[sc_idx].state = SCAFFOLD_SNAPPING;
    w.scaffolds[sc_idx].placed_station = outpost;
    w.scaffolds[sc_idx].placed_ring = furnace_ring;
    w.scaffolds[sc_idx].placed_slot = furnace_slot;
    int mod_count_pre = st_out->module_count;
    world_sim_step(&w, SIM_DT);
    ASSERT(!w.scaffolds[sc_idx].active);
    ASSERT_EQ_INT(st_out->module_count, mod_count_pre + 1);
    station_module_t *furn = &st_out->modules[mod_count_pre];
    ASSERT_EQ_INT(furn->type, MODULE_FURNACE);
    ASSERT(furn->scaffold);

    /* Step 7 — re-dock and supply the furnace's build material with a
     * second exact frame crate. */
    const uint8_t furnace_frame_origin[8] = { 'F','U','R','P','O','D','0','1' };
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_FRAME,
        (int)ceilf(module_build_cost_lookup(MODULE_FURNACE)),
        furnace_frame_origin));
    ASSERT(sp->ship->towed_pod_count > 0);
    ASSERT(test_anchor_pod_legacy_cargo(
        &w, outpost,
        sp->ship->towed_pods[sp->ship->towed_pod_count - 1]));
    sp->docked = true;
    sp->current_station = outpost;
    for (int i = 0; i < 10 && furn->scaffold; i++) {
        sp->input.service_sell = true;
        world_sim_step(&w, SIM_DT);
    }
    step_module_activation(&w, MODULE_BUILD_TIME_SEC);
    ASSERT(!furn->scaffold);
    ASSERT_EQ_FLOAT(furn->build_progress, 1.0f, 0.01f);

    /* Step 8 — activation creates worker demand only. It must not mint
     * a free hull; a later roster pass can satisfy demand from stored
     * hulls or a shipyard-local build contract. */
    int npc_after = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) if (w.npc_ships[i].active) npc_after++;
    ASSERT_EQ_INT(npc_after, npc_before);

    /* Step 9 — plant a HOPPER on the furnace's adjacent-ring pair slot.
     * The live fragment beam requires a real furnace/hopper pair, not
     * just a station-level "has any hopper" gate. The furnace's tag
     * defaults to ferrite; we tag the hopper for FERRITE_ORE explicitly. */
    sp->docked = false;
    /* Tow one more exact frame crate for the hopper scaffold. */
    const uint8_t hopper_frame_origin[8] = { 'H','O','P','P','O','D','0','1' };
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_FRAME,
        (int)ceilf(module_build_cost_lookup(MODULE_HOPPER)),
        hopper_frame_origin));
    ASSERT(sp->ship->towed_pod_count > 0);
    ASSERT(test_anchor_pod_legacy_cargo(
        &w, outpost,
        sp->ship->towed_pods[sp->ship->towed_pod_count - 1]));

    station_slot_pair_t pair_slots[2];
    int pair_count = station_pair_neighbors((int)furn->ring, (int)furn->slot,
                                            pair_slots);
    ASSERT(pair_count > 0);
    int hop_ring = -1, hop_slot = -1;
    for (int i = 0; i < pair_count; i++) {
        if (station_module_at(st_out, pair_slots[i].ring, pair_slots[i].slot)
            == MODULE_COUNT) {
            hop_ring = pair_slots[i].ring;
            hop_slot = pair_slots[i].slot;
            break;
        }
    }
    ASSERT(hop_ring >= 0 && hop_slot >= 0);

    int mod_count_pre_hop = st_out->module_count;
    begin_module_construction_at(&w, st_out, outpost, MODULE_HOPPER,
                                 hop_ring, hop_slot);
    ASSERT_EQ_INT(st_out->module_count, mod_count_pre_hop + 1);
    station_module_t *hop = &st_out->modules[mod_count_pre_hop];
    ASSERT_EQ_INT(hop->type, MODULE_HOPPER);
    ASSERT(hop->scaffold);
    /* Tag the hopper for ferrite ore now (the snap path doesn't call
     * add_module_at, so auto_pick_hopper_commodity never runs). */
    hop->commodity = (uint8_t)COMMODITY_FERRITE_ORE;

    sp->docked = true;
    sp->current_station = outpost;
    for (int i = 0; i < 10 && hop->scaffold; i++) {
        sp->input.service_sell = true;
        world_sim_step(&w, SIM_DT);
    }
    step_module_activation(&w, MODULE_BUILD_TIME_SEC);
    ASSERT(!hop->scaffold);
    ASSERT(station_can_smelt(st_out, COMMODITY_FERRITE_ORE));

    /* Step 10 — build a frame hopper and stage one physical frame shell
     * pod so the furnace can unfold it into the output ingot crate. */
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        st_out->arm_speed[arm] = 0.0f;
        st_out->arm_rotation[arm] = 0.0f;
    }
    vec2 furnace_pos = module_world_pos_ring(st_out, furn->ring, furn->slot);
    vec2 hopper_pos = module_world_pos_ring(st_out, hop->ring, hop->slot);
    vec2 smelt_midpoint = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
    int frame_hop_ring = -1;
    int frame_hop_slot = -1;
    float shell_reach_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    for (int ring = 1; ring <= STATION_NUM_RINGS && frame_hop_ring < 0; ring++) {
        for (int slot = 0; slot < STATION_RING_SLOTS[ring]; slot++) {
            if (station_module_at(st_out, ring, slot) != MODULE_COUNT)
                continue;
            vec2 slot_pos = module_world_pos_ring(st_out, ring, slot);
            if (v2_dist_sq(slot_pos, furnace_pos) > shell_reach_sq)
                continue;
            frame_hop_ring = ring;
            frame_hop_slot = slot;
            break;
        }
    }
    ASSERT(frame_hop_ring >= 0 && frame_hop_slot >= 0);
    int frame_hop_idx = st_out->module_count;
    begin_module_construction_at(&w, st_out, outpost, MODULE_HOPPER,
                                 frame_hop_ring, frame_hop_slot);
    ASSERT(frame_hop_idx < st_out->module_count);
    station_module_t *frame_hop = &st_out->modules[frame_hop_idx];
    frame_hop->commodity = (uint8_t)COMMODITY_FRAME;
    const uint8_t frame_hopper_origin[8] = { 'F','R','H','O','P','0','0','1' };
    ASSERT(construction_spawn_towed_material_pod(
        &w, sp, COMMODITY_FRAME,
        (int)ceilf(module_build_cost_lookup(MODULE_HOPPER)),
        frame_hopper_origin));
    ASSERT(sp->ship->towed_pod_count > 0);
    ASSERT(test_anchor_pod_legacy_cargo(
        &w, outpost,
        sp->ship->towed_pods[sp->ship->towed_pod_count - 1]));
    sp->docked = true;
    sp->current_station = outpost;
    for (int i = 0; i < 10 && frame_hop->scaffold; i++) {
        sp->input.service_sell = true;
        world_sim_step(&w, SIM_DT);
    }
    step_module_activation(&w, MODULE_BUILD_TIME_SEC);
    ASSERT(!frame_hop->scaffold);

    const uint8_t shell_origin[8] = { 'S','H','E','L','L','0','0','1' };
    vec2 frame_hop_pos = module_world_pos_ring(
        st_out, frame_hop->ring, frame_hop->slot);
    vec2 frame_hop_out = v2_norm(
        v2_sub(frame_hop_pos, st_out->pos));
    vec2 shell_pos = v2_add(
        frame_hop_pos, v2_scale(
            frame_hop_out,
            STATION_MODULE_COL_RADIUS + 18.0f + 8.0f));
    int shell_pod = construction_spawn_loose_material_pod(
        &w, shell_pos, COMMODITY_FRAME, 1, shell_origin);
    ASSERT(shell_pod >= 0);
    ASSERT(test_anchor_pod_legacy_cargo(&w, outpost, shell_pod));
    w.cargo_pods[shell_pod].vel =
        station_ring_point_velocity(
            st_out, frame_hop->ring, shell_pos);
    ASSERT(world_cargo_pod_set_module_tractor(
        &w, shell_pod, outpost, frame_hop_idx));
    vec2 shell_anchor = v2(0.0f, 0.0f);
    ASSERT(cargo_pod_module_tractor_anchor(
        &w, &w.cargo_pods[shell_pod],
        outpost, frame_hop_idx, &shell_anchor));
    w.cargo_pods[shell_pod].pos = shell_anchor;
    w.cargo_pods[shell_pod].vel =
        station_ring_point_velocity(
            st_out, frame_hop->ring, shell_anchor);
    ASSERT(cargo_pod_module_tractor_arrived(
        &w, &w.cargo_pods[shell_pod],
        outpost, frame_hop_idx));
    ASSERT(v2_dist_sq(frame_hop_pos, furnace_pos) <=
           shell_reach_sq);

    /* Step 11 — process ore. The retired hopper-float path no longer
     * accepts raw `_inventory_cache[ORE]`; smelting now means a physical
     * fragment enters the furnace/hopper beam and mints an attributed
     * FERRITE_INGOT cargo pod in space. */
    sp->docked = false;
    sp->input.service_sell = false;
    furnace_pos = module_world_pos_ring(
        st_out, furn->ring, furn->slot);
    hopper_pos = module_world_pos_ring(
        st_out, hop->ring, hop->slot);
    smelt_midpoint =
        v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);

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
    a->ore = 6.0f;
    a->max_ore = 6.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = (uint8_t)MINING_GRADE_COMMON;
    for (int b = 0; b < 32; b++) a->fracture_seed[b] = (uint8_t)(0x50 + b);
    a->pos = smelt_midpoint;
    a->vel = v2(0.0f, 0.0f);

    int ingots_before =
        construction_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    step_furnace_smelting(&w, 0.0f);
    w.asteroids[frag].smelt_progress = 1.0f;
    step_furnace_smelting(&w, 0.0f);
    ASSERT(!w.asteroids[frag].active);
    int ingots_after =
        construction_count_exact_pod_units(&w, COMMODITY_FERRITE_INGOT);
    ASSERT(ingots_after >= ingots_before + 6); /* smelter produced a pod */
    ASSERT_EQ_INT((int)w.hopper_smelt_events, 0);
    ASSERT(w.hopper_smelt_units == 0.0);

    /* Step 12 — credit conservation. The whole pipeline runs through
     * ledger paths; nothing should leak. econ_total_credits sums every
     * station pool plus every player ledger row, so the diff captures
     * any silent mint or burn. */
    double credits_end = econ_total_credits(&w);
    if (fabs(credits_end - credits_start) > 5.0) {
        printf("    credit drift: start=%.2f end=%.2f delta=%.2f\n",
               credits_start, credits_end, credits_end - credits_start);
    }
    ASSERT(fabs(credits_end - credits_start) <= 5.0);
}

TEST(test_scaffold_ship_drag) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;
    w.players[0].ship->pos = v2(5000.0f, 5000.0f);
    w.players[0].ship->vel = v2(0.0f, 0.0f);

    /* Spawn and attach scaffold */
    int idx = spawn_scaffold(&w, MODULE_FURNACE, v2(5050.0f, 5000.0f), 0);
    w.scaffolds[idx].state = SCAFFOLD_TOWING;
    ASSERT(world_scaffold_set_player_tractor(&w, idx, 0));

    /* Thrust for a while */
    for (int i = 0; i < 600; i++) {
        w.players[0].input.thrust = 1.0f;
        world_sim_step(&w, SIM_DT);
    }

    /* Ship speed should be capped (much slower than free flight). Cap
     * is now engine-coupled — miner accel 300 → tow cap ~82 u/s. */
    float spd = v2_len(w.players[0].ship->vel);
    ASSERT_EQ_INT(w.players[0].ship->towed_scaffold, idx);
    ASSERT(spd <= 100.0f); /* engine-coupled cap + thrust/drag balance */

    /* Compare to free-flight speed: reset and thrust without scaffold */
    w.scaffolds[idx].state = SCAFFOLD_LOOSE;
    world_scaffold_clear_tractor(&w, idx);
    w.players[0].ship->vel = v2(0.0f, 0.0f);
    for (int i = 0; i < 600; i++) {
        w.players[0].input.thrust = 1.0f;
        world_sim_step(&w, SIM_DT);
    }
    float free_spd = v2_len(w.players[0].ship->vel);

    /* Free flight should be significantly faster */
    ASSERT(free_spd > spd * 1.5f);
}

static int test_count_planned_frontier_outposts(const world_t *w) {
    int count = 0;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (w->stations[s].planned) count++;
    }
    return count;
}

static int test_count_pending_relay_orders(const world_t *w) {
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int i = 0; i < st->pending_scaffold_count; i++) {
            if (st->pending_scaffolds[i].type == MODULE_SIGNAL_RELAY)
                count++;
        }
    }
    return count;
}

static int test_count_pending_scaffold_orders(const world_t *w,
                                              module_type_t type) {
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int i = 0; i < st->pending_scaffold_count; i++) {
            if (st->pending_scaffolds[i].type == type)
                count++;
        }
    }
    return count;
}

static int test_count_frontier_scaffold_work(const world_t *w,
                                             module_type_t type) {
    int count = test_count_pending_scaffold_orders(w, type);
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active || sc->module_type != type) continue;
        if (sc->state == SCAFFOLD_NASCENT ||
            sc->state == SCAFFOLD_LOOSE ||
            sc->state == SCAFFOLD_TOWING ||
            sc->state == SCAFFOLD_SNAPPING)
            count++;
    }
    return count;
}

static int test_count_frontier_placement_plans(const world_t *w,
                                               module_type_t type) {
    int count = 0;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            if (st->placement_plans[p].type == type)
                count++;
        }
    }
    return count;
}

static int test_count_active_frontier_outposts(const world_t *w) {
    int count = 0;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (station_is_active(&w->stations[s])) count++;
    }
    return count;
}

static bool test_station_has_duplicate_slots(const station_t *st) {
    if (!st) return false;
    for (int a = 0; a < st->module_count; a++) {
        if (st->modules[a].ring == 0 || st->modules[a].slot == 0xFF)
            continue;
        for (int b = a + 1; b < st->module_count; b++) {
            if (st->modules[b].ring == 0 || st->modules[b].slot == 0xFF)
                continue;
            if (st->modules[a].ring == st->modules[b].ring &&
                st->modules[a].slot == st->modules[b].slot) {
                return true;
            }
        }
    }
    return false;
}

TEST(test_frontier_virtual_pilots_plan_and_order_relay) {
    WORLD_DECL;
    world_reset(&w);

    frontier_virtual_pilots_set(&w, 1000);
    signal_intelligence_decision_reason_t reason;
    ASSERT(signal_intelligence_step_frontier_director_with_reason(
        &w, 1.0f, &reason));

    ASSERT_EQ_INT(w.frontier_virtual_pilots, 1000);
    ASSERT_EQ_INT((int)w.frontier_plans_created, 1);
    ASSERT_EQ_INT((int)w.frontier_scaffold_orders, 1);
    ASSERT_EQ_INT(test_count_planned_frontier_outposts(&w), 1);
    ASSERT_EQ_INT(test_count_pending_relay_orders(&w), 1);
    ASSERT_EQ_INT(test_count_frontier_placement_plans(&w, MODULE_HOPPER), 1);
    ASSERT_EQ_INT(test_count_frontier_placement_plans(&w, MODULE_FURNACE), 1);
    ASSERT_EQ_INT(test_count_pending_scaffold_orders(&w, MODULE_HOPPER), 1);
    ASSERT_EQ_INT(test_count_pending_scaffold_orders(&w, MODULE_FURNACE), 1);
    ASSERT_EQ_INT((int)w.frontier_module_plans_created, 2);
    ASSERT_EQ_INT((int)w.frontier_module_scaffold_orders, 2);
    ASSERT_EQ_INT(reason.task, SIGNAL_INTEL_TASK_FRONTIER_PLAN);
    ASSERT_EQ_INT(reason.selected_index, FRONTIER_DIRECTOR_DECISION_PLAN_OUTPOST);
    ASSERT_EQ_INT(reason.candidate_count, 5);
    ASSERT_EQ_FLOAT(reason.frontier_pressure, 1.0f, 0.001f);
    ASSERT(reason.source_memory_id != 0ull);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_USED_TEACHER);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_ADVISORY_ONLY);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HARD_APPROVED);
    ASSERT(reason.flags & SIGNAL_DECISION_REASON_HAS_FRONTIER_PRESSURE);
    ASSERT(!(reason.flags & SIGNAL_DECISION_REASON_HAS_SOURCE_MEMORY));
    ASSERT_EQ_INT(w.frontier_decision_valid, 1);
    ASSERT_EQ_INT(w.frontier_decision_action,
                  FRONTIER_DIRECTOR_DECISION_PLAN_OUTPOST);
    ASSERT_EQ_INT(w.frontier_decision_plan_limit, 5);
    ASSERT(w.frontier_decision_flags &
           SIGNAL_DECISION_REASON_HAS_FRONTIER_PRESSURE);

    int plan_slot = -1;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (w.stations[s].planned) { plan_slot = s; break; }
    }
    ASSERT(plan_slot >= SIGNAL_FIRST_OUTPOST_INDEX);

    int hopper_ring = -1, furnace_ring = -1;
    for (int p = 0; p < w.stations[plan_slot].placement_plan_count; p++) {
        if (w.stations[plan_slot].placement_plans[p].type == MODULE_HOPPER)
            hopper_ring = w.stations[plan_slot].placement_plans[p].ring;
        if (w.stations[plan_slot].placement_plans[p].type == MODULE_FURNACE)
            furnace_ring = w.stations[plan_slot].placement_plans[p].ring;
    }
    ASSERT(hopper_ring >= 1);
    ASSERT(furnace_ring >= 1);
    int ring_delta = furnace_ring - hopper_ring;
    if (ring_delta < 0) ring_delta = -ring_delta;
    ASSERT_EQ_INT(ring_delta, 1);

    ASSERT(can_place_outpost(&w, v2_add(w.stations[plan_slot].pos,
                                        v2(OUTPOST_MIN_DISTANCE * 0.5f, 0.0f))) == false);
}

TEST(test_invalid_outpost_plan_preserves_existing_blueprint) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    vec2 valid_pos = v2_add(w.stations[1].pos, v2(4000.0f, 0.0f));
    sp->input.create_planned_outpost = true;
    sp->input.planned_outpost_pos = valid_pos;
    world_sim_step(&w, SIM_DT);

    int plan_slot = -1;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (w.stations[s].planned) { plan_slot = s; break; }
    }
    ASSERT(plan_slot >= SIGNAL_FIRST_OUTPOST_INDEX);
    uint32_t plan_id = w.stations[plan_slot].id;
    vec2 plan_pos = w.stations[plan_slot].pos;

    sp->input.create_planned_outpost = true;
    sp->input.planned_outpost_pos = w.stations[1].pos; /* too close */
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(test_count_planned_frontier_outposts(&w), 1);
    ASSERT(w.stations[plan_slot].planned);
    ASSERT_EQ_INT((int)w.stations[plan_slot].id, (int)plan_id);
    ASSERT_EQ_FLOAT(w.stations[plan_slot].pos.x, plan_pos.x, 0.001f);
    ASSERT_EQ_FLOAT(w.stations[plan_slot].pos.y, plan_pos.y, 0.001f);
}

TEST(test_frontier_virtual_pilots_scale_planned_queue) {
    WORLD_DECL;
    world_reset(&w);

    frontier_virtual_pilots_set(&w, 1000);
    for (int i = 0; i < 5; i++) {
        step_frontier_director(&w, 2.0f);
    }

    ASSERT(test_count_planned_frontier_outposts(&w) >= 2);
    ASSERT(test_count_planned_frontier_outposts(&w) <= 5);
    ASSERT(test_count_frontier_scaffold_work(&w, MODULE_SIGNAL_RELAY) >= 2);
    ASSERT(w.frontier_scaffold_orders >= 2);
    ASSERT(w.frontier_virtual_scaffold_deliveries <=
           w.frontier_virtual_scaffolds_manufactured);
}

TEST(test_frontier_virtual_pilots_execute_growth_loop) {
    WORLD_DECL;
    world_reset(&w);
    w.field_spawn_timer = -9999.0f;

    frontier_virtual_pilots_set(&w, 1000);
    for (int i = 0; i < 120 * 25; i++) {
        world_sim_step(&w, SIM_DT);
    }

    ASSERT(w.frontier_virtual_scaffolds_manufactured >= 3);
    ASSERT(w.frontier_virtual_scaffold_deliveries >= 3);
    ASSERT(w.frontier_virtual_scaffold_deliveries <=
           w.frontier_virtual_scaffolds_manufactured);
    ASSERT(w.frontier_virtual_supply_deliveries >= 3);
    ASSERT(test_count_active_frontier_outposts(&w) >= 1);

    bool found_bootstrapped_outpost = false;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        station_t *st = &w.stations[s];
        if (!station_is_active(st)) continue;
        ASSERT(!test_station_has_duplicate_slots(st));
        if (station_has_module(st, MODULE_HOPPER) &&
            station_has_module(st, MODULE_FURNACE)) {
            ASSERT(station_can_smelt(st, COMMODITY_FERRITE_ORE));
            found_bootstrapped_outpost = true;
            break;
        }
    }
    ASSERT(found_bootstrapped_outpost);
}

TEST(test_hauler_delivers_to_planned_outpost) {
    WORLD_DECL;
    world_reset(&w);
    w.field_spawn_timer = -9999.0f; /* suppress chunk re-materialization */
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */

    /* Create a planned outpost within Kepler's signal range. This test
     * isolates scaffold materialization, not long-route corridor travel. */
    vec2 plan_pos = v2_add(w.stations[1].pos, v2(4000.0f, 0.0f));
    w.players[0].input.create_planned_outpost = true;
    w.players[0].input.planned_outpost_pos = plan_pos;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.create_planned_outpost = false;

    int plan_slot = -1;
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
        if (w.stations[s].planned) { plan_slot = s; break; }
    }
    ASSERT(plan_slot >= 0);

    /* Spawn a loose signal relay near Kepler (station 1) */
    vec2 near_kepler = v2_add(w.stations[1].pos, v2(200.0f, 0.0f));
    int sc_idx = spawn_scaffold(&w, MODULE_SIGNAL_RELAY, near_kepler, 0);
    ASSERT(sc_idx >= 0);
    w.scaffolds[sc_idx].state = SCAFFOLD_LOOSE;
    world_scaffold_clear_tractor(&w, sc_idx);

    /* Find or seed Kepler's hauler-class worker to take the scaffold tow
     * contract. Scaffold delivery work is executed by hauler hulls; tow
     * drones are legacy tractor workers and must not morph classes. */
    int worker_idx = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER
            && w.npc_ships[i].home_station == 1) {
            worker_idx = i; break;
        }
    }
    if (worker_idx < 0)
        worker_idx = spawn_npc(&w, 1, NPC_ROLE_HAULER);
    ASSERT(worker_idx >= 0);
    w.npc_ships[worker_idx].state = NPC_STATE_DOCKED;
    w.npc_ships[worker_idx].state_timer = 0.0f;
    w.npc_ships[worker_idx].ship->pos = w.stations[1].pos;
    /* Resolve through the public actor lookup and seed its ship position. */
    {
        ship_t *worker_ship = world_npc_ship_for(&w, worker_idx);
        ASSERT(worker_ship != NULL);
        worker_ship->pos = w.stations[1].pos;
    }

    /* Run up to 30s — wait for the worker to grab the scaffold */
    npc_ship_t *worker = &w.npc_ships[worker_idx];
    for (int i = 0; i < 120 * 30 && worker->ship->towed_scaffold < 0; i++)
        world_sim_step(&w, SIM_DT);

    /* Worker must have accepted the tow contract and grabbed the scaffold. */
    ASSERT_EQ_INT(worker->role, NPC_ROLE_HAULER);
    ASSERT(worker->ship->towed_scaffold >= 0);
    /* Destination must be the planned outpost, not a starter station */
    ASSERT(worker->dest_station >= SIGNAL_FIRST_OUTPOST_INDEX);
    ASSERT_EQ_INT(worker->dest_station, plan_slot);

    /* Run long enough for the worker to cross the larger Sector One
     * starter basin. */
    for (int i = 0; i < 120 * 360; i++) world_sim_step(&w, SIM_DT);

    station_t *outpost = &w.stations[plan_slot];
    bool materialized = (!outpost->planned && outpost->scaffold) ||
                        station_is_active(outpost);
    ASSERT(materialized);
}

TEST(test_save_preserves_pending_scaffolds) {
    /* Save/load round-trip should preserve shipyard pending orders,
     * per-module buffers, and active scaffolds. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);

    /* Add a pending order at Kepler (station 1, has shipyard) */
    w->stations[1].pending_scaffolds[0].type = MODULE_FURNACE;
    w->stations[1].pending_scaffolds[0].owner = 0;
    w->stations[1].pending_scaffold_count = 1;
    w->stations[1].pending_ship_builds[0].hull_class = HULL_CLASS_HAULER;
    uint8_t pending_owner_id[ACTOR_PRINCIPAL_ID_SIZE];
    for (size_t i = 0; i < sizeof(pending_owner_id); i++)
        pending_owner_id[i] = (uint8_t)(0x71u + i);
    actor_principal_t pending_owner = actor_principal_none();
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, pending_owner_id, &pending_owner));
    w->stations[1].pending_ship_builds[0].owner_principal = pending_owner;
    w->stations[1].pending_ship_builds[0].build_progress = 0.5f;
    w->stations[1].pending_ship_builds[0].mode =
        PENDING_SHIP_BUILD_MODE_MATERIAL;
    w->stations[1].pending_ship_build_count = 1;
    /* Some module buffer state */
    w->stations[1].modules[3].input_buffer = 42.5f;
    w->stations[1].modules[5].output_buffer = 17.0f;
    /* Spawn a nascent scaffold */
    int sidx = spawn_scaffold(w, MODULE_FRAME_PRESS, w->stations[1].pos, 0);
    ASSERT(sidx >= 0);
    w->scaffolds[sidx].state = SCAFFOLD_NASCENT;
    w->scaffolds[sidx].built_at_station = 1;
    w->scaffolds[sidx].build_amount = 17.0f;

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_pendcat")));
    ASSERT(world_save(w, TMP("test_pending.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_pendcat"));
    ASSERT(world_load(loaded, TMP("test_pending.sav")));

    /* Verify pending order survived (session-tier data) */
    ASSERT_EQ_INT(loaded->stations[1].pending_scaffold_count, 1);
    ASSERT_EQ_INT(loaded->stations[1].pending_scaffolds[0].type, MODULE_FURNACE);
    ASSERT_EQ_INT(loaded->stations[1].pending_scaffolds[0].owner, 0);
    ASSERT_EQ_INT(loaded->stations[1].pending_ship_build_count, 1);
    ASSERT_EQ_INT(loaded->stations[1].pending_ship_builds[0].hull_class,
                  HULL_CLASS_HAULER);
    ASSERT(actor_principal_equal(
        &loaded->stations[1].pending_ship_builds[0].owner_principal,
        &pending_owner));
    ASSERT_EQ_FLOAT(loaded->stations[1].pending_ship_builds[0].build_progress,
                    0.5f, 0.01f);
    ASSERT_EQ_INT(loaded->stations[1].pending_ship_builds[0].mode,
                  PENDING_SHIP_BUILD_MODE_MATERIAL);
    ASSERT_EQ_FLOAT(loaded->stations[1].modules[3].input_buffer, 42.5f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->stations[1].modules[5].output_buffer, 17.0f, 0.01f);

    /* Scaffolds are transient in v24 — not persisted in world save.
     * Nascent scaffolds are regenerated from pending orders on restart. */
    (void)sidx;

    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_pending.sav"));
}

TEST(test_shipyard_queue_waits_for_loose_scaffold_to_clear) {
    WORLD_DECL;
    world_reset(&w);

    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        w.npc_ships[i].active = false;

    station_t *st = &w.stations[1]; /* Kepler has a shipyard. */
    st->pending_scaffolds[0].type = MODULE_FURNACE;
    st->pending_scaffolds[0].owner = 0;
    st->pending_scaffold_count = 1;

    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    st->_inventory_cache[mat] = module_build_cost_lookup(MODULE_FURNACE);

    int blocker = spawn_scaffold(&w, MODULE_DOCK, st->pos, 0);
    ASSERT(blocker >= 0);
    w.scaffolds[blocker].state = SCAFFOLD_LOOSE;
    w.scaffolds[blocker].vel = v2(0.0f, 0.0f);

    ASSERT(station_construction_area_blocked(st, w.scaffolds, MAX_SCAFFOLDS));
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(st->pending_scaffold_count, 1);
    ASSERT_EQ_INT(station_nascent_scaffold_index(w.scaffolds,
                                                 MAX_SCAFFOLDS, 1), -1);

    w.scaffolds[blocker].pos = v2_add(st->pos,
                                      v2(STATION_RING_RADIUS[1] * 2.0f, 0.0f));
    ASSERT(!station_construction_area_blocked(st, w.scaffolds, MAX_SCAFFOLDS));
    world_sim_step(&w, SIM_DT);

    int nascent = station_nascent_scaffold_index(w.scaffolds, MAX_SCAFFOLDS, 1);
    ASSERT(nascent >= 0);
    ASSERT_EQ_INT(w.scaffolds[nascent].state, SCAFFOLD_NASCENT);
    ASSERT_EQ_INT(w.scaffolds[nascent].module_type, MODULE_FURNACE);
    ASSERT_EQ_INT(w.scaffolds[nascent].built_at_station, 1);
}

TEST(test_placed_scaffold_supply_phase) {
    /* After snap, module starts at build_progress=0. Delivering material
     * advances it to 1.0, then the 10s build timer runs 1.0 → 2.0. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    ASSERT(m->scaffold);
    ASSERT(m->build_progress < 0.01f); /* supply phase start */

    /* Deliver half the material */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat,
                                            cost * 0.5f));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(m->build_progress > 0.4f && m->build_progress < 0.6f);
    ASSERT(m->scaffold); /* still building */

    /* Deliver the rest */
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat,
                                            cost * 0.5f));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied, timer may have started */

    /* Build timer: 10s = 1200 ticks */
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!m->scaffold);
    ASSERT_EQ_FLOAT(m->build_progress, 1.0f, 0.01f);
}

TEST(test_placed_scaffold_manifest_supply_batches_multiple_units) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    int module_idx = -1;
    int outpost = test_setup_placed_scaffold(
        &w, &module_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_t *st = &w.stations[outpost];
    station_module_t *module =
        &st->modules[module_idx];
    ASSERT(module_build_state(module) ==
           MODULE_BUILD_AWAITING_SUPPLY);

    commodity_t material =
        module_build_material_lookup(module->type);
    float cost =
        module_build_cost_lookup(module->type);
    const int units = 3;
    ASSERT(material == COMMODITY_FRAME);
    ASSERT(cost > (float)units);
    ASSERT(test_set_station_finished_units(
        st, material, units));
    ASSERT(test_anchor_station_legacy_cargo(
        &w, outpost));

    float progress_before =
        module->build_progress;
    uint64_t events_before =
        st->chain_event_count;
    cargo_receipt_origin_cache_reset();
    step_module_activation(&w, 0.0f);

    ASSERT_EQ_INT(
        station_finished_count(st, material), 0);
    ASSERT_EQ_FLOAT(
        module->build_progress,
        progress_before + (float)units / cost,
        0.001f);
    ASSERT_EQ_INT(
        (int)st->chain_event_count,
        (int)events_before + units);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT(
        (int)walked, (int)st->chain_event_count);
    cargo_receipt_origin_cache_stats_t cache_stats =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT(
        (int)cache_stats.full_verifications, 1);
    ASSERT_EQ_INT(
        (int)cache_stats.index_builds, 1);
    ASSERT(cache_stats.hits >=
           (uint64_t)(units - 1));
}

TEST(test_placed_scaffold_manifest_supply_batch_failure_is_inert) {
    WORLD_DECL;
    const chain_log_test_fault_point_t faults[] = {
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
    };
    const int units = 3;
    int previous_outpost = -1;

    for (int failure_case = 0;
         failure_case < 3;
         failure_case++) {
        if (previous_outpost >= 0)
            chain_log_reset(
                &w.stations[previous_outpost]);
        world_reset(&w);
        memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

        int module_idx = -1;
        int outpost = test_setup_placed_scaffold(
            &w, &module_idx);
        ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
        previous_outpost = outpost;
        station_t *st = &w.stations[outpost];
        station_module_t *module =
            &st->modules[module_idx];
        ASSERT(module_build_state(module) ==
               MODULE_BUILD_AWAITING_SUPPLY);

        commodity_t material =
            module_build_material_lookup(module->type);
        float cost =
            module_build_cost_lookup(module->type);
        ASSERT(material == COMMODITY_FRAME);
        ASSERT(cost > (float)units);
        ASSERT(test_set_station_finished_units(
            st, material, units));
        ASSERT(test_anchor_station_legacy_cargo(
            &w, outpost));

        cargo_store_t store_before = {0};
        ASSERT(cargo_store_clone(
            &store_before, &st->cargo_store));
        cargo_unit_t *manifest_ptr_before =
            st->cargo_store.manifest.units;
        void *receipts_ptr_before =
            st->cargo_store.receipts_opaque;
        bool dirty_before = st->manifest_dirty;
        float progress_before =
            module->build_progress;
        uint64_t events_before =
            st->chain_event_count;
        uint8_t hash_before[32];
        memcpy(
            hash_before, st->chain_last_hash,
            sizeof(hash_before));
        uint64_t walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT(
            (int)walked, (int)events_before);

        cargo_receipt_origin_cache_reset();
        if (failure_case < 2) {
            chain_log_test_fault_inject(
                faults[failure_case],
                CHAIN_EVT_CONSTRUCTION, 2);
        } else {
            chain_log_health_set(
                st, CHAIN_HEALTH_FAILED, true,
                st->chain_event_count,
                st->chain_last_hash,
                "test pre-blocked station module supply");
        }
        step_module_activation(&w, 0.0f);
        chain_log_test_fault_clear();

        ASSERT_EQ_FLOAT(
            module->build_progress,
            progress_before, 0.001f);
        ASSERT_EQ_INT(
            station_finished_count(
                st, material), units);
        ASSERT_EQ_INT(
            (int)st->chain_event_count,
            (int)events_before);
        ASSERT(memcmp(
            st->chain_last_hash, hash_before,
            sizeof(hash_before)) == 0);
        walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT(
            (int)walked, (int)events_before);
        ASSERT(st->chain_append_blocked);
        ASSERT_EQ_INT(
            st->manifest_dirty, dirty_before);
        ASSERT(
            st->cargo_store.manifest.units ==
            manifest_ptr_before);
        ASSERT(
            st->cargo_store.receipts_opaque ==
            receipts_ptr_before);
        ASSERT_EQ_INT(
            st->cargo_store.manifest.count,
            store_before.manifest.count);
        ASSERT_EQ_INT(
            st->cargo_store.manifest.cap,
            store_before.manifest.cap);
        ASSERT(memcmp(
            st->cargo_store.manifest.units,
            store_before.manifest.units,
            (size_t)store_before.manifest.count *
                sizeof(*store_before.manifest.units)) == 0);
        const ship_receipts_t *receipts_after =
            cargo_store_receipts_const(
                &st->cargo_store);
        const ship_receipts_t *receipts_before =
            cargo_store_receipts_const(
                &store_before);
        ASSERT(receipts_after != NULL);
        ASSERT(receipts_before != NULL);
        ASSERT_EQ_INT(
            receipts_after->count,
            receipts_before->count);
        ASSERT_EQ_INT(
            receipts_after->cap,
            receipts_before->cap);
        ASSERT(memcmp(
            receipts_after->chains,
            receipts_before->chains,
            (size_t)receipts_before->count *
                sizeof(*receipts_before->chains)) == 0);
        cargo_receipt_origin_cache_stats_t cache_stats =
            cargo_receipt_origin_cache_stats();
        ASSERT_EQ_INT(
            (int)cache_stats.full_verifications, 1);
        ASSERT_EQ_INT(
            (int)cache_stats.index_builds, 1);
        ASSERT(cache_stats.hits >=
               (uint64_t)(units - 1));
        cargo_store_cleanup(&store_before);
    }
}

TEST(test_placed_scaffold_supply_consumes_staged_material_pod) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_t *st = &w.stations[outpost];
    station_module_t *m = &st->modules[mod_idx];
    ASSERT(m->scaffold);
    ASSERT(m->build_progress < 0.01f);

    commodity_t mat = module_build_material_lookup(m->type);
    int units = (int)ceilf(module_build_cost_lookup(m->type) - 0.0001f);
    ASSERT_EQ_INT(mat, COMMODITY_FRAME);
    ASSERT(units > 1 && units <= CHAIN_LOG_BATCH_MAX_EVENTS);
    ASSERT(test_set_station_finished_units(st, mat, 0));
    st->_inventory_cache[mat] = 0.0f;

    vec2 module_pos = module_world_pos_ring(st, m->ring, m->slot);
    int hopper_ring = 0;
    uint8_t hopper_slot = 0;
    ASSERT(construction_serving_slot_from_pos(st, module_pos,
                                              &hopper_ring, &hopper_slot));
    int hopper_idx = st->module_count;
    add_hopper_for(st, (uint8_t)hopper_ring, hopper_slot, mat);
    ASSERT(hopper_idx < st->module_count);
    vec2 hopper_pos = module_world_pos_ring(st, hopper_ring, hopper_slot);
    ASSERT(v2_dist_sq(module_pos, hopper_pos) <=
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);
    ASSERT(v2_dist_sq(module_pos, hopper_pos) >
           HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE);

    int pod_idx = construction_spawn_loose_material_pod(
        &w, module_pos, mat, units,
        (const uint8_t *)"PODPLC01");
    ASSERT(pod_idx >= 0);
    ASSERT(test_anchor_pod_legacy_cargo(&w, outpost, pod_idx));

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT(!module_is_fully_supplied(m));

    vec2 hopper_out = v2_norm(v2_sub(hopper_pos, st->pos));
    w.cargo_pods[pod_idx].pos = v2_add(hopper_pos, v2_scale(
        hopper_out, STATION_MODULE_COL_RADIUS +
                    w.cargo_pods[pod_idx].radius + 8.0f));
    w.cargo_pods[pod_idx].vel = station_ring_point_velocity(
        st, hopper_ring, w.cargo_pods[pod_idx].pos);
    ASSERT(world_cargo_pod_set_module_tractor(
        &w, pod_idx, outpost, hopper_idx));
    cargo_receipt_origin_cache_reset();
    uint64_t events_before = st->chain_event_count;
    step_module_activation(&w, 0.0f);

    ASSERT(!w.cargo_pods[pod_idx].active);
    ASSERT(module_is_fully_supplied(m));
    ASSERT_EQ_INT(
        (int)st->chain_event_count,
        (int)events_before + units);
    uint64_t walked = 0;
    ASSERT(chain_log_verify(st, &walked, NULL));
    ASSERT_EQ_INT((int)walked, (int)st->chain_event_count);
    cargo_receipt_origin_cache_stats_t cache_stats =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT((int)cache_stats.full_verifications, 1);
    ASSERT_EQ_INT((int)cache_stats.index_builds, 1);
    ASSERT(cache_stats.hits >= (uint64_t)(units - 1));
    ASSERT_EQ_INT(station_finished_count(st, mat), 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[mat], 0.0f, 0.001f);
}

TEST(test_placed_scaffold_physical_supply_append_failure_is_inert) {
    WORLD_DECL;
    const chain_log_test_fault_point_t faults[] = {
        CHAIN_LOG_TEST_FAULT_WRITE,
        CHAIN_LOG_TEST_FAULT_FLUSH,
    };
    int previous_outpost = -1;

    for (int failure_case = 0; failure_case < 3; failure_case++) {
        if (previous_outpost >= 0)
            chain_log_reset(&w.stations[previous_outpost]);
        world_reset(&w);
        memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

        int module_idx = -1;
        int outpost = test_setup_placed_scaffold(
            &w, &module_idx);
        ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
        previous_outpost = outpost;
        station_t *st = &w.stations[outpost];
        station_module_t *module =
            &st->modules[module_idx];
        ASSERT(module_build_state(module) ==
               MODULE_BUILD_AWAITING_SUPPLY);

        commodity_t material =
            module_build_material_lookup(module->type);
        int units = (int)ceilf(
            module_build_cost_lookup(module->type) -
            0.0001f);
        ASSERT(material == COMMODITY_FRAME);
        ASSERT(units > 1 &&
               units <= CHAIN_LOG_BATCH_MAX_EVENTS);
        ASSERT(test_set_station_finished_units(
            st, material, 0));

        vec2 module_pos = module_world_pos_ring(
            st, module->ring, module->slot);
        int hopper_ring = 0;
        uint8_t hopper_slot = 0;
        ASSERT(construction_serving_slot_from_pos(
            st, module_pos, &hopper_ring,
            &hopper_slot));
        int hopper_idx = st->module_count;
        add_hopper_for(
            st, (uint8_t)hopper_ring,
            hopper_slot, material);
        ASSERT(hopper_idx < st->module_count);
        vec2 hopper_pos = module_world_pos_ring(
            st, hopper_ring, hopper_slot);
        vec2 outward = v2_norm(
            v2_sub(hopper_pos, st->pos));
        vec2 pod_pos = v2_add(
            hopper_pos, v2_scale(
                outward,
                STATION_MODULE_COL_RADIUS +
                18.0f + 8.0f));
        int pod_idx =
            construction_spawn_loose_material_pod(
                &w, pod_pos, material, units,
                (const uint8_t *)"PODFL001");
        ASSERT(pod_idx >= 0);
        ASSERT(test_anchor_pod_legacy_cargo(
            &w, outpost, pod_idx));
        w.cargo_pods[pod_idx].vel =
            station_ring_point_velocity(
                st, hopper_ring, pod_pos);
        ASSERT(world_cargo_pod_set_module_tractor(
            &w, pod_idx, outpost, hopper_idx));
        step_station_cargo_pod_tractors(
            &w, 0.0f);

        cargo_pod_t pod_before =
            w.cargo_pods[pod_idx];
        float progress_before =
            module->build_progress;
        uint64_t events_before =
            st->chain_event_count;
        uint8_t hash_before[32];
        memcpy(
            hash_before, st->chain_last_hash,
            sizeof(hash_before));
        uint64_t walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT(
            (int)walked, (int)events_before);
        bool contract_before[MAX_CONTRACTS];
        for (int k = 0; k < MAX_CONTRACTS; k++)
            contract_before[k] =
                w.contracts[k].active;

        if (failure_case < 2) {
            /* The second matching row distinguishes one atomic batch from
             * repeated one-row appends: WRITE leaves a serialized prefix
             * that must be truncated, and FLUSH must reject every row. */
            chain_log_test_fault_inject(
                faults[failure_case],
                CHAIN_EVT_CONSTRUCTION, 2);
        } else {
            chain_log_health_set(
                st, CHAIN_HEALTH_FAILED, true,
                st->chain_event_count,
                st->chain_last_hash,
                "test pre-blocked physical module supply");
        }
        step_module_activation(&w, 0.0f);
        chain_log_test_fault_clear();

        ASSERT(memcmp(
            &w.cargo_pods[pod_idx], &pod_before,
            sizeof(pod_before)) == 0);
        ASSERT_EQ_FLOAT(
            module->build_progress,
            progress_before, 0.001f);
        ASSERT_EQ_INT(
            (int)st->chain_event_count,
            (int)events_before);
        ASSERT(memcmp(
            st->chain_last_hash, hash_before,
            sizeof(hash_before)) == 0);
        walked = 0;
        ASSERT(chain_log_verify(st, &walked, NULL));
        ASSERT_EQ_INT(
            (int)walked, (int)events_before);
        ASSERT_EQ_INT(
            station_finished_count(st, material), 0);
        for (int k = 0; k < MAX_CONTRACTS; k++)
            ASSERT_EQ_INT(
                w.contracts[k].active,
                contract_before[k]);
        ASSERT(st->chain_append_blocked);
    }
}

TEST(test_placed_scaffold_supply_rejects_far_staged_hopper) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_t *st = &w.stations[outpost];
    station_module_t *m = &st->modules[mod_idx];
    ASSERT(m->scaffold);
    ASSERT(m->build_progress < 0.01f);

    commodity_t mat = module_build_material_lookup(m->type);
    int units = (int)ceilf(module_build_cost_lookup(m->type) - 0.0001f);
    ASSERT_EQ_INT(mat, COMMODITY_FRAME);
    ASSERT(units > 0 && units <= CARGO_POD_MANIFEST_CAP);
    ASSERT(test_set_station_finished_units(st, mat, 0));
    st->_inventory_cache[mat] = 0.0f;

    vec2 module_pos = module_world_pos_ring(st, m->ring, m->slot);
    int hopper_ring = 0;
    uint8_t hopper_slot = 0;
    ASSERT(construction_far_slot_from_pos(st, module_pos,
                                          &hopper_ring, &hopper_slot));
    add_hopper_for(st, (uint8_t)hopper_ring, hopper_slot, mat);
    vec2 hopper_pos = module_world_pos_ring(st, hopper_ring, hopper_slot);
    ASSERT(v2_dist_sq(module_pos, hopper_pos) >
           HOPPER_PULL_RANGE * HOPPER_PULL_RANGE);

    int pod_idx = construction_spawn_loose_material_pod(
        &w, hopper_pos, mat, units,
        (const uint8_t *)"FARPLC01");
    ASSERT(pod_idx >= 0);
    ASSERT(test_anchor_pod_legacy_cargo(&w, outpost, pod_idx));

    world_sim_step(&w, SIM_DT);

    ASSERT(w.cargo_pods[pod_idx].active);
    ASSERT(!module_is_fully_supplied(m));
    ASSERT_EQ_INT(station_finished_count(st, mat), 0);
    ASSERT_EQ_FLOAT(st->_inventory_cache[mat], 0.0f, 0.001f);
}

TEST(test_placed_scaffold_player_delivery) {
    /* A docked player delivering the build material should advance the
     * scaffold's build_progress via step_module_delivery. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    ASSERT(m->scaffold);

    /* Dock the player at the outpost with the required cargo */
    w.players[0].docked = true;
    w.players[0].current_station = outpost;
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    ASSERT(construction_spawn_towed_material_pod(
        &w, &w.players[0], mat, (int)ceilf(cost - 0.0001f),
        (const uint8_t *)"PLYBUILD"));
    ASSERT(w.players[0].ship->towed_pod_count > 0);
    ASSERT(test_anchor_pod_legacy_cargo(
        &w, outpost,
        w.players[0].ship->towed_pods[
            w.players[0].ship->towed_pod_count - 1]));

    /* Trigger sell action — step_module_delivery pulls from cargo */
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(m->build_progress, 1.0f, 0.01f);
    ASSERT_EQ_INT(w.players[0].ship->towed_pod_count, 0);
}

TEST(test_construction_contract_closes_on_activation) {
    /* When the scaffold module activates, any supply contract at
     * this station for the build material should close. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);

    /* There should be a supply contract for this station+material */
    bool found_contract = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            found_contract = true; break;
        }
    }
    ASSERT(found_contract);

    /* Supply and activate */
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat, cost));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied */
    /* Run build timer */
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!m->scaffold); /* activated */

    /* Contract should now be closed */
    bool contract_alive = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            contract_alive = true; break;
        }
    }
    ASSERT(!contract_alive);
}

TEST(test_stale_contract_does_not_block_next_need) {
    /* After a construction contract completes, the station should be
     * able to generate its next need contract (e.g. ore hopper). */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];

    /* Supply, build, activate */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat, cost));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!m->scaffold);

    /* Run a few more ticks for step_contracts to generate the next need */
    for (int i = 0; i < 240; i++) world_sim_step(&w, SIM_DT);

    /* The station should be able to post a new contract (not blocked).
     * A furnace station needs ore — check if any contract exists or
     * at least that no stale construction contract is blocking. */
    bool stale_construction = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            /* A supply contract for the build material should not linger */
            stale_construction = true; break;
        }
    }
    ASSERT(!stale_construction);
}

TEST(test_construction_contract_checks_scaffold_not_threshold) {
    /* A construction supply contract should NOT close based on the
     * 80% station inventory threshold — it should stay open while
     * the scaffold still needs material, regardless of inventory level. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= SIGNAL_FIRST_OUTPOST_INDEX);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);

    /* Deliver a partial amount (not enough to fully supply).
     * But make the station inventory exceed the 80% generic threshold
     * by adding a different commodity that fills the buffer. */
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat,
                                            cost * 0.3f));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    world_sim_step(&w, SIM_DT);

    /* After one tick, step_module_activation routed the partial amount
     * into the scaffold. Scaffold is partially supplied, not full. */
    ASSERT(m->build_progress > 0.2f && m->build_progress < 0.4f);
    ASSERT(m->scaffold);

    /* Contract must still be open — scaffold isn't fully supplied */
    bool contract_alive = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            contract_alive = true; break;
        }
    }
    ASSERT(contract_alive);

    /* Now deliver the rest */
    ASSERT(test_set_station_finished_amount(&w.stations[outpost], mat, cost));
    ASSERT(test_anchor_station_legacy_cargo(&w, outpost));
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied */
}

/* #307: module build state helpers — verify the lifecycle predicates
 * agree with the underlying float convention without leaking it. */
TEST(test_module_build_state_lifecycle) {
    station_module_t m = {0};
    /* Active scaffold, no supply yet. */
    m.scaffold = true;
    m.build_progress = 0.0f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_AWAITING_SUPPLY);
    ASSERT(!module_is_complete(&m));
    ASSERT(!module_is_fully_supplied(&m));
    ASSERT_EQ_FLOAT(module_supply_fraction(&m), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 0.0f, 0.001f);

    /* Half supplied. */
    m.build_progress = 0.5f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_AWAITING_SUPPLY);
    ASSERT_EQ_FLOAT(module_supply_fraction(&m), 0.5f, 0.001f);

    /* Just hit full supply — moves to BUILDING. */
    m.build_progress = 1.0f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_BUILDING);
    ASSERT(module_is_fully_supplied(&m));
    ASSERT(!module_is_complete(&m));
    ASSERT_EQ_FLOAT(module_supply_fraction(&m), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 0.0f, 0.001f);

    /* Mid-build. */
    m.build_progress = 1.5f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_BUILDING);
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 0.5f, 0.001f);

    /* Activated — scaffold cleared, build_progress reset to 1.0. */
    m.scaffold = false;
    m.build_progress = 1.0f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_COMPLETE);
    ASSERT(module_is_complete(&m));
    ASSERT(module_is_fully_supplied(&m));
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 1.0f, 0.001f);
}

TEST(test_module_schema_basic_kinds) {
    /* Each kind classification should be consistent */
    ASSERT_EQ_INT(module_kind(MODULE_DOCK), MODULE_KIND_SERVICE);
    ASSERT_EQ_INT(module_kind(MODULE_REPAIR_BAY), MODULE_KIND_SERVICE);
    ASSERT_EQ_INT(module_kind(MODULE_SIGNAL_RELAY), MODULE_KIND_SERVICE);
    ASSERT_EQ_INT(module_kind(MODULE_FURNACE), MODULE_KIND_PRODUCER);
    ASSERT_EQ_INT(module_kind(MODULE_FRAME_PRESS), MODULE_KIND_PRODUCER);
    ASSERT_EQ_INT(module_kind(MODULE_LASER_FAB), MODULE_KIND_PRODUCER);
    ASSERT_EQ_INT(module_kind(MODULE_HOPPER), MODULE_KIND_STORAGE);
    ASSERT_EQ_INT(module_kind(MODULE_SHIPYARD), MODULE_KIND_SHIPYARD);
}

TEST(test_module_schema_producer_io) {
    /* Producers expose their primary input and output commodity. */
    /* Furnace exposes its primary (ferrite) recipe in the schema; the
     * cuprite/crystal tiers live in the runtime sim_can_smelt rules,
     * not the static schema. */
    ASSERT_EQ_INT(module_schema_input(MODULE_FURNACE), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(module_schema_output(MODULE_FURNACE), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_schema_input(MODULE_FRAME_PRESS), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_schema_output(MODULE_FRAME_PRESS), COMMODITY_FRAME);
    ASSERT_EQ_INT(module_schema_input(MODULE_LASER_FAB), COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT(module_schema_output(MODULE_LASER_FAB), COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(module_schema_input(MODULE_TRACTOR_FAB), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(module_schema_output(MODULE_TRACTOR_FAB), COMMODITY_TRACTOR_MODULE);
    /* Services have no input/output */
    ASSERT_EQ_INT(module_schema_input(MODULE_DOCK), COMMODITY_COUNT);
    ASSERT_EQ_INT(module_schema_output(MODULE_DOCK), COMMODITY_COUNT);
}

TEST(test_module_schema_required_output) {
    /* Slice 1 — every non-shipyard producer declares a single output
     * commodity at the schema level. SHIPYARD is exempt (output is a
     * physical scaffold, not a commodity). */
    ASSERT_EQ_INT(module_required_output(MODULE_FURNACE),     COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_required_output(MODULE_FRAME_PRESS), COMMODITY_FRAME);
    ASSERT_EQ_INT(module_required_output(MODULE_LASER_FAB),   COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(module_required_output(MODULE_TRACTOR_FAB), COMMODITY_TRACTOR_MODULE);
    ASSERT_EQ_INT(module_required_output(MODULE_SHIPYARD),    COMMODITY_COUNT);
    ASSERT_EQ_INT(module_required_output(MODULE_DOCK),        COMMODITY_COUNT);
    ASSERT_EQ_INT(module_required_output(MODULE_HOPPER),      COMMODITY_COUNT);
}

TEST(test_module_furnace_instance_tag) {
    /* Furnace output follows the per-instance commodity tag. Untagged
     * (legacy COMMODITY_COUNT) falls back to FERRITE_INGOT. Each ingot
     * tag implies a matching input ore. Non-furnace producers ignore
     * the tag and read schema. */
    station_module_t m = { .type = MODULE_FURNACE, .commodity = (uint8_t)COMMODITY_COUNT };
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_FERRITE_ORE);

    m.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_CUPRITE_ORE);

    m.commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT;
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_CRYSTAL_ORE);

    /* Garbage tag → fallback to default. */
    m.commodity = (uint8_t)COMMODITY_FRAME;
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_FERRITE_ORE);

    /* Frame press: instance output ignores tag, uses schema. */
    station_module_t fp = { .type = MODULE_FRAME_PRESS, .commodity = (uint8_t)COMMODITY_LASER_MODULE };
    ASSERT_EQ_INT(module_instance_output(&fp),    COMMODITY_FRAME);
    ASSERT_EQ_INT(module_instance_input_ore(&fp), COMMODITY_COUNT); /* not a furnace */
}

TEST(test_commodity_ore_ingot_pairing) {
    /* Round-trip: ingot ↔ ore. Non-pairs return COMMODITY_COUNT. */
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_FERRITE_ORE), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_CUPRITE_ORE), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_CRYSTAL_ORE), COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_FERRITE_INGOT), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_CUPRITE_INGOT), COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_CRYSTAL_INGOT), COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_FRAME),         COMMODITY_COUNT);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_LASER_MODULE),  COMMODITY_COUNT);
}

TEST(test_station_module_layout_status_missing_output) {
    /* Synthetic station: TRACTOR_FAB with input hopper, AND a SHIPYARD
     * on the station that consumes TRACTOR_MODULE — so the TRACTOR_FAB
     * has a local downstream consumer and an output hopper IS required.
     * Without the hopper → MISSING_OUTPUT_HOPPER. Adding it restores OK. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 2, 0, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&st, 2, 4, COMMODITY_FRAME);
    add_module_at(&st, MODULE_TRACTOR_FAB, 2, 1);
    add_module_at(&st, MODULE_SHIPYARD,    2, 5);   /* downstream consumer */
    /* Shipyard also needs FRAME and LASER_MODULE input hoppers to be OK
     * for itself, but we're testing the TRACTOR_FAB module specifically. */
    add_hopper_for(&st, 3, 0, COMMODITY_FRAME);
    add_hopper_for(&st, 3, 1, COMMODITY_LASER_MODULE);
    const station_module_t *fab = &st.modules[2];   /* TRACTOR_FAB */
    ASSERT_EQ_INT(station_module_layout_status(&st, fab),
                  STATION_LAYOUT_MISSING_OUTPUT_HOPPER);
    add_hopper_for(&st, 2, 2, COMMODITY_TRACTOR_MODULE);
    ASSERT_EQ_INT(station_module_layout_status(&st, fab), STATION_LAYOUT_OK);
}

TEST(test_station_module_layout_status_no_local_consumer_is_ok) {
    /* The mirror case: a producer with NO local downstream consumer
     * doesn't need an output hopper. Smelted ingots ride out via
     * haulers from station inventory. Models Prospect's furnace —
     * 1-furnace ferrite station with no on-station frame press. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_furnace_for(&st, 1, 1, COMMODITY_FERRITE_INGOT);
    add_hopper_for(&st, 2, 0, COMMODITY_FERRITE_ORE);   /* adjacent input only */
    const station_module_t *furnace = &st.modules[0];
    /* No FRAME_PRESS / LASER_FAB / TRACTOR_FAB on the station, so
     * nothing locally consumes ferrite ingots. Layout is OK without
     * an output hopper. */
    ASSERT_EQ_INT(station_module_layout_status(&st, furnace),
                  STATION_LAYOUT_OK);
}

TEST(test_station_module_layout_status_furnace_uses_tag) {
    /* A furnace tagged for CUPRITE_INGOT needs CUPRITE_ORE in (not any
     * ore) and CUPRITE_INGOT out (because we add a TRACTOR_FAB to give
     * the cuprite ingot a local downstream consumer). FERRITE_ORE alone
     * is missing-input. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 2, 0, COMMODITY_FERRITE_ORE); /* wrong ore for a CU furnace */
    add_furnace_for(&st, 1, 1, COMMODITY_CUPRITE_INGOT);
    add_module_at(&st, MODULE_TRACTOR_FAB, 2, 5);       /* consumes CUPRITE_INGOT */
    add_hopper_for(&st, 3, 0, COMMODITY_FRAME);
    const station_module_t *fc = &st.modules[1];     /* the furnace */
    ASSERT_EQ_INT(station_module_layout_status(&st, fc),
                  STATION_LAYOUT_MISSING_INPUT_HOPPER);
    add_hopper_for(&st, 2, 2, COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(station_module_layout_status(&st, fc),
                  STATION_LAYOUT_MISSING_OUTPUT_HOPPER);
    add_hopper_for(&st, 2, 3, COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(station_module_layout_status(&st, fc), STATION_LAYOUT_OK);
}

TEST(test_station_module_layout_status_furnace_requires_adjacent_ore_hopper) {
    station_t st = {0};
    st.signal_range = 1.0f;
    add_furnace_for(&st, 2, 1, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&st, 2, 2, COMMODITY_CUPRITE_ORE);
    const station_module_t *fc = &st.modules[0];

    ASSERT_EQ_INT(station_module_layout_status(&st, fc),
                  STATION_LAYOUT_MISSING_INPUT_HOPPER);

    add_hopper_for(&st, 3, 3, COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(station_module_layout_status(&st, fc), STATION_LAYOUT_OK);
}

TEST(test_seeded_stations_layout_ok) {
    /* Slice 1 — every producer module on every seeded station reports
     * STATION_LAYOUT_OK (i.e., its inputs and output have matching
     * tagged hoppers, except SHIPYARD which is exempt from the output
     * rule). This is the end-state validator on a fresh world. */
    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < 3; s++) {
        const station_t *st = &w.stations[s];
        for (int i = 0; i < st->module_count; i++) {
            const station_module_t *m = &st->modules[i];
            if (m->scaffold) continue;
            if (!module_is_producer(m->type) && !module_is_shipyard(m->type)) continue;
            station_layout_status_t status = station_module_layout_status(st, m);
            if (status != STATION_LAYOUT_OK) {
                printf("station %d (%s) module %d (type=%d, commodity=%u) layout status %d\n",
                       s, st->name, i, m->type, m->commodity, status);
            }
            ASSERT_EQ_INT(status, STATION_LAYOUT_OK);
        }
    }
}

TEST(test_seeded_furnaces_tagged) {
    /* Slice 1 — seeded stations tag every furnace with its output ingot.
     * Prospect runs ferrite. Helios tags one cuprite furnace and two
     * crystal furnaces for the two-stage crystal process. */
    WORLD_DECL;
    world_reset(&w);
    int prospect_furnaces = 0;
    for (int i = 0; i < w.stations[0].module_count; i++) {
        if (w.stations[0].modules[i].type != MODULE_FURNACE) continue;
        if (w.stations[0].modules[i].scaffold) continue;
        prospect_furnaces++;
        ASSERT_EQ_INT((int)w.stations[0].modules[i].commodity,
                      (int)COMMODITY_FERRITE_INGOT);
    }
    ASSERT_EQ_INT(prospect_furnaces, 1);

    int helios_cu = 0, helios_cr = 0;
    for (int i = 0; i < w.stations[2].module_count; i++) {
        if (w.stations[2].modules[i].type != MODULE_FURNACE) continue;
        if (w.stations[2].modules[i].scaffold) continue;
        commodity_t tag = (commodity_t)w.stations[2].modules[i].commodity;
        if (tag == COMMODITY_CUPRITE_INGOT) helios_cu++;
        else if (tag == COMMODITY_CRYSTAL_INGOT) helios_cr++;
        else ASSERT(false /* unexpected Helios furnace tag */);
    }
    ASSERT_EQ_INT(helios_cu, 1);
    ASSERT_EQ_INT(helios_cr, 2);
}

static bool test_furnace_has_adjacent_ore_hopper(const station_t *st,
                                                 const station_module_t *furnace) {
    commodity_t ore = module_instance_input_ore(furnace);
    if (ore == COMMODITY_COUNT) return false;
    vec2 furnace_pos = module_world_pos_ring(st, furnace->ring, furnace->slot);
    float max_pair_dist = HOPPER_PULL_RANGE * 2.0f;
    float max_pair_sq = max_pair_dist * max_pair_dist;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold) continue;
        if (hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity != ore) continue;
        int dr = (int)hopper->ring - (int)furnace->ring;
        if (dr != 1 && dr != -1) continue;
        vec2 hopper_pos = module_world_pos_ring(st, hopper->ring, hopper->slot);
        if (v2_dist_sq(furnace_pos, hopper_pos) <= max_pair_sq) return true;
    }
    return false;
}

TEST(test_seeded_helios_output_hoppers) {
    /* Helios's LASER_FAB and TRACTOR_FAB each have a dedicated
     * commodity-tagged output hopper on ring 3, and its shipyard has
     * the frame / laser / tractor hoppers needed for repair-kit fab.
     * Its furnaces also each have a matching ore hopper on an adjacent
     * ring, which the smelt-beam code requires before firing. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_has_module(&w.stations[2], MODULE_SHIPYARD));
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_FRAME) >= 0);
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_LASER_MODULE)   >= 0);
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_TRACTOR_MODULE) >= 0);
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_CUPRITE_ORE) >= 0);
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_CRYSTAL_ORE) >= 0);
    int checked_furnaces = 0;
    for (int i = 0; i < w.stations[2].module_count; i++) {
        const station_module_t *m = &w.stations[2].modules[i];
        if (m->scaffold) continue;
        if (m->type != MODULE_FURNACE) continue;
        ASSERT(test_furnace_has_adjacent_ore_hopper(&w.stations[2], m));
        checked_furnaces++;
    }
    ASSERT_EQ_INT(checked_furnaces, 3);
    /* All Helios producers report OK under the new layout rule. */
    for (int i = 0; i < w.stations[2].module_count; i++) {
        const station_module_t *m = &w.stations[2].modules[i];
        if (m->scaffold) continue;
        if (!module_is_producer(m->type) && !module_is_shipyard(m->type)) continue;
        station_layout_status_t s = station_module_layout_status(&w.stations[2], m);
        ASSERT_EQ_INT(s, STATION_LAYOUT_OK);
    }
}

TEST(test_station_module_layout_status_shipyard_exempt) {
    /* SHIPYARD output is a physical scaffold body, not a commodity —
     * so it doesn't need an output hopper. With its 3 input hoppers
     * present (frame, laser, tractor module), layout is OK. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 3, 0, COMMODITY_FRAME);
    add_hopper_for(&st, 3, 1, COMMODITY_LASER_MODULE);
    add_hopper_for(&st, 3, 2, COMMODITY_TRACTOR_MODULE);
    add_module_at(&st, MODULE_SHIPYARD, 3, 3);
    const station_module_t *sy = &st.modules[st.module_count - 1];
    ASSERT_EQ_INT(station_module_layout_status(&st, sy), STATION_LAYOUT_OK);
}

TEST(test_module_schema_valid_rings) {
    /* Service modules can go anywhere */
    ASSERT(module_valid_on_ring(MODULE_DOCK, 0));
    ASSERT(module_valid_on_ring(MODULE_DOCK, 1));
    ASSERT(module_valid_on_ring(MODULE_DOCK, 3));
    ASSERT(module_valid_on_ring(MODULE_SIGNAL_RELAY, 0));
    ASSERT(module_valid_on_ring(MODULE_SIGNAL_RELAY, 2));
    /* Outer-only modules reject ring 0 */
    ASSERT(!module_valid_on_ring(MODULE_FURNACE, 0));
    ASSERT(module_valid_on_ring(MODULE_FURNACE, 1));
    ASSERT(module_valid_on_ring(MODULE_FURNACE, 3));
    /* Industrial modules need ring 2+ */
    ASSERT(!module_valid_on_ring(MODULE_FRAME_PRESS, 1));
    ASSERT(module_valid_on_ring(MODULE_FRAME_PRESS, 2));
    ASSERT(module_valid_on_ring(MODULE_FRAME_PRESS, 3));
    ASSERT(!module_valid_on_ring(MODULE_SHIPYARD, 1));
    ASSERT(module_valid_on_ring(MODULE_SHIPYARD, 2));
}

TEST(test_module_schema_helpers) {
    /* Boolean kind helpers */
    ASSERT(module_is_producer(MODULE_FURNACE));
    ASSERT(module_is_producer(MODULE_FRAME_PRESS));
    ASSERT(!module_is_producer(MODULE_DOCK));
    ASSERT(module_is_service(MODULE_DOCK));
    ASSERT(module_is_service(MODULE_REPAIR_BAY));
    ASSERT(!module_is_service(MODULE_FURNACE));
    ASSERT(module_is_storage(MODULE_HOPPER));
    ASSERT(module_is_shipyard(MODULE_SHIPYARD));
    ASSERT(!module_is_dead(MODULE_FURNACE));
}

TEST(test_module_schema_build_costs_match) {
    /* Schema build costs match the existing lookup helpers for ALL
     * non-dead modules. Once production code starts reading from the
     * schema in commit 2, drift would change behavior — this test
     * catches it. */
    for (int t = 0; t < MODULE_COUNT; t++) {
        if (module_is_dead((module_type_t)t)) continue;
        const module_schema_t *s = module_schema((module_type_t)t);
        ASSERT_EQ_FLOAT(s->build_material,
                        module_build_cost_lookup((module_type_t)t), 0.01f);
        ASSERT_EQ_INT(s->build_commodity,
                      module_build_material_lookup((module_type_t)t));
        ASSERT_EQ_INT(s->order_fee,
                      scaffold_order_fee((module_type_t)t));
    }
}

TEST(test_module_schema_uses_accepted_rock_cell_balance) {
    static const float expected_cost[MODULE_COUNT] = {
        [MODULE_DOCK] = 16.0f,
        [MODULE_HOPPER] = 32.0f,
        [MODULE_FURNACE] = 48.0f,
        [MODULE_REPAIR_BAY] = 24.0f,
        [MODULE_SIGNAL_RELAY] = 32.0f,
        [MODULE_FRAME_PRESS] = 64.0f,
        [MODULE_LASER_FAB] = 32.0f,
        [MODULE_TRACTOR_FAB] = 32.0f,
        [MODULE_SHIPYARD] = 96.0f,
    };
    for (int t = 0; t < MODULE_COUNT; t++) {
        if (module_is_dead((module_type_t)t)) continue;
        ASSERT_EQ_FLOAT(module_build_cost_lookup((module_type_t)t),
                        expected_cost[t], 0.001f);
    }
    ASSERT_EQ_FLOAT(REFINERY_INGOTS_PER_FRAGMENT,
                    (float)CELL_INGOTS_PER_FRAGMENT, 0.001f);
    ASSERT_EQ_FLOAT(SCAFFOLD_MATERIAL_NEEDED,
                    3.0f * (float)CELL_STRUTS_PER_FRAGMENT, 0.001f);
    ASSERT_EQ_INT(module_build_material_lookup(MODULE_LASER_FAB),
                  COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT(module_build_material_lookup(MODULE_TRACTOR_FAB),
                  COMMODITY_CUPRITE_INGOT);
}

TEST(test_module_flow_same_ring_transfer) {
    /* Furnace produces ferrite ingots into output_buffer.
     * Frame Press accepts ferrite ingots as input.
     * On the same ring, material should flow at ~5/sec adjacent. */
    WORLD_DECL;
    world_reset(&w);
    /* Use Kepler (station 1) which has both furnace logic and ring layout.
     * Find a furnace and a frame press by index. */
    int furnace_idx = -1, press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS && press_idx < 0)
            press_idx = i;
    }
    /* Manually create a furnace adjacent to the press if not present */
    if (press_idx >= 0 && furnace_idx < 0) {
        /* Add a furnace at the same ring as the press */
        if (w.stations[1].module_count < MAX_MODULES_PER_STATION) {
            int idx = w.stations[1].module_count++;
            w.stations[1].modules[idx].type = MODULE_FURNACE;
            w.stations[1].modules[idx].ring = w.stations[1].modules[press_idx].ring;
            w.stations[1].modules[idx].slot = (uint8_t)
                ((w.stations[1].modules[press_idx].slot + 1)
                 % STATION_RING_SLOTS[w.stations[1].modules[press_idx].ring]);
            w.stations[1].modules[idx].scaffold = false;
            w.stations[1].modules[idx].build_progress = 1.0f;
            furnace_idx = idx;
        }
    }
    if (furnace_idx < 0 || press_idx < 0) return; /* setup failed, skip */

    /* Seed the furnace's output with ferrite ingots */
    w.stations[1].modules[furnace_idx].output_buffer = 10.0f;
    w.stations[1].modules[press_idx].input_buffer = 0.0f;

    /* Run one full second of sim */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);

    /* Material should have moved from furnace output to press input.
     * The press now actively consumes from its input buffer to produce
     * frames at a 2-ingot recipe, so we just check that flow happened
     * and some downstream work is visible. */
    ASSERT(w.stations[1].modules[furnace_idx].output_buffer < 10.0f);
    ASSERT(w.stations[1].modules[press_idx].input_buffer > 0.0f ||
           w.stations[1].modules[press_idx].output_buffer > 0.0f ||
           station_inventory_amount(
               &w.stations[1], COMMODITY_FRAME) > 0.0f);
}

TEST(test_module_flow_production_fills_buffers) {
    /* Finished production now ejects exact physical cargo pods. The
     * station-manifest path therefore needs both provenance-backed ingot
     * input and one folded frame for the output pod shell; module buffers
     * remain routing reservations rather than a second inventory authority. */
    WORLD_DECL;
    world_reset(&w);
    memset(w.cargo_pods, 0, sizeof(w.cargo_pods));

    /* Seed Kepler with the complete frame-press transaction inputs. */
    ASSERT(test_set_station_finished_units(
        &w.stations[1], COMMODITY_FERRITE_INGOT, 50));
    ASSERT(test_set_station_finished_units(
        &w.stations[1], COMMODITY_FRAME, 1));
    ASSERT(test_anchor_station_legacy_cargo(&w, 1));
    int pod_frames_before =
        construction_count_exact_pod_units(&w, COMMODITY_FRAME);
    /* Find frame press */
    int press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i; break;
        }
    }
    if (press_idx < 0) return;

    /* Run a few seconds of sim — production should route the ingot and
     * materialize its output in a physical pod. */
    for (int i = 0; i < 240; i++) world_sim_step(&w, SIM_DT);

    /* The durable CRAFT transaction consumes one exact ingot and its shell,
     * then exposes the output as exact physical cargo. */
    ASSERT(station_inventory_amount(&w.stations[1],
                                    COMMODITY_FERRITE_INGOT) < 50.0f);
    ASSERT(construction_count_exact_pod_units(
               &w, COMMODITY_FRAME) > pod_frames_before);
}

TEST(test_module_flow_does_not_overflow_capacity) {
    /* Material should never exceed buffer capacity at the consumer. */
    WORLD_DECL;
    world_reset(&w);
    int furnace_idx = -1, press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS) press_idx = i;
    }
    if (press_idx < 0) return;
    if (w.stations[1].module_count < MAX_MODULES_PER_STATION) {
        int idx = w.stations[1].module_count++;
        w.stations[1].modules[idx].type = MODULE_FURNACE;
        w.stations[1].modules[idx].ring = w.stations[1].modules[press_idx].ring;
        w.stations[1].modules[idx].slot = (uint8_t)
            ((w.stations[1].modules[press_idx].slot + 1)
             % STATION_RING_SLOTS[w.stations[1].modules[press_idx].ring]);
        furnace_idx = idx;
    }
    if (furnace_idx < 0) return;

    /* Seed a huge amount of output, run for many ticks */
    w.stations[1].modules[furnace_idx].output_buffer = 1000.0f;
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);

    /* Press input must not exceed its capacity */
    float cap = module_buffer_capacity(MODULE_FRAME_PRESS);
    ASSERT(w.stations[1].modules[press_idx].input_buffer <= cap + 0.01f);
}

/* #280: storage modules must participate in flow as buffers, not be
 * pure sinks. Raw ore hoppers are fragment-smelt anchors now, so this
 * covers the remaining storage-flow job: finished goods feeding fabs. */
TEST(test_module_flow_storage_feeds_consumer) {
    WORLD_DECL;
    world_reset(&w);

    /* Use Kepler (station 1): its ferrite-ingot hopper feeds the frame
     * press through the module-flow graph. */
    int hopper_idx = -1, press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_HOPPER &&
            w.stations[1].modules[i].commodity == (uint8_t)COMMODITY_FERRITE_INGOT)
            hopper_idx = i;
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS)
            press_idx = i;
    }
    if (hopper_idx < 0 || press_idx < 0) return; /* layout drift, skip */

    ASSERT(test_set_station_finished_units(
        &w.stations[1], COMMODITY_FERRITE_INGOT, 50));
    w.stations[1].modules[hopper_idx].output_buffer = 0.0f;
    w.stations[1].modules[press_idx].input_buffer = 0.0f;
    float ingots_before = station_inventory_amount(
        &w.stations[1], COMMODITY_FERRITE_INGOT);

    /* One second of sim — hopper should refill its output from inventory
     * and the flow stepper should push it onward. */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);

    bool flowed = w.stations[1].modules[hopper_idx].output_buffer > 0.0f
               || w.stations[1].modules[press_idx].input_buffer > 0.0f
               || station_inventory_amount(&w.stations[1],
                                            COMMODITY_FERRITE_INGOT) <
                  ingots_before - 0.5f;
    ASSERT(flowed);
}

TEST(test_station_default_module_commodity_picks_uncovered_input) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };

    ASSERT_EQ_INT(station_default_module_commodity(&st, MODULE_HOPPER),
                  COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(station_default_module_commodity(&st, MODULE_FURNACE),
                  COMMODITY_FERRITE_INGOT);
}

TEST(test_station_plan_flow_hint_connected_output) {
    station_t st = {0};
    station_plan_flow_hint_t hint;
    char line[96];

    st.signal_range = 1.0f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 1,
        .build_progress = 1.0f,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 2,
        .build_progress = 1.0f,
        .commodity = (uint8_t)COMMODITY_FRAME,
    };

    ASSERT(station_plan_flow_hint(&st, MODULE_FRAME_PRESS, 2, 0, &hint));
    ASSERT_EQ_INT(hint.diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(hint.role, STATION_PLAN_FLOW_ROLE_OUTPUT);
    ASSERT_EQ_INT(hint.peer_type, MODULE_HOPPER);
    ASSERT(station_plan_flow_hint_format(&hint, line, sizeof(line)));
    ASSERT(strstr(line, "well-connected output to Hopper") != NULL);
}

TEST(test_station_plan_flow_hint_slow_hopper_feed) {
    station_t st = {0};
    station_plan_flow_hint_t hint;
    char line[96];

    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 3,
        .slot = (uint8_t)(STATION_RING_SLOTS[3] / 2),
        .build_progress = 1.0f,
    };

    ASSERT(station_plan_flow_hint(&st, MODULE_HOPPER, 2, 0, &hint));
    ASSERT_EQ_INT(hint.diag, STATION_FLOW_DIAG_SLOW_FEED);
    ASSERT_EQ_INT(hint.role, STATION_PLAN_FLOW_ROLE_OUTPUT);
    ASSERT_EQ_INT(hint.peer_type, MODULE_FRAME_PRESS);
    ASSERT(station_plan_flow_hint_format(&hint, line, sizeof(line)));
    ASSERT(strstr(line, "slow route") != NULL);
}

TEST(test_station_plan_flow_hint_no_consumer) {
    station_t st = {0};
    station_plan_flow_hint_t hint;
    char line[96];

    st.signal_range = 1.0f;

    ASSERT(station_plan_flow_hint(&st, MODULE_FRAME_PRESS, 2, 0, &hint));
    ASSERT_EQ_INT(hint.diag, STATION_FLOW_DIAG_NO_CONSUMER);
    ASSERT(station_plan_flow_hint_format(&hint, line, sizeof(line)));
    ASSERT(strstr(line, "no valid consumer") != NULL);
}

TEST(test_module_flow_diag_no_input) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 1,
        .build_progress = 1.0f, .commodity = (uint8_t)COMMODITY_FRAME,
    };

    ASSERT_EQ_INT(station_module_flow_diag(&st, 0),
                  STATION_FLOW_DIAG_NO_INPUT);
}

TEST(test_module_flow_diag_output_full) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };
    st.modules[0].output_buffer = module_buffer_capacity(MODULE_FRAME_PRESS);

    ASSERT_EQ_INT(station_module_flow_diag(&st, 0),
                  STATION_FLOW_DIAG_OUTPUT_FULL);
}

TEST(test_module_flow_diag_no_consumer) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };
    st.modules[0].output_buffer = 2.0f;

    ASSERT_EQ_INT(station_module_flow_diag(&st, 0),
                  STATION_FLOW_DIAG_NO_CONSUMER);
}

TEST(test_module_flow_diag_slow_feed) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 3,
        .slot = (uint8_t)(STATION_RING_SLOTS[3] / 2),
        .build_progress = 1.0f, .commodity = (uint8_t)COMMODITY_FRAME,
    };
    st.modules[0].output_buffer = 2.0f;

    ASSERT_EQ_INT(station_module_flow_diag(&st, 0),
                  STATION_FLOW_DIAG_SLOW_FEED);
}

TEST(test_module_flow_diag_storage_consumer_full) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER, .ring = 2, .slot = 0,
        .build_progress = 1.0f, .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 1,
        .build_progress = 1.0f,
    };
    ASSERT(test_set_station_finished_units(
        &st, COMMODITY_FERRITE_INGOT, 10));
    st.modules[1].input_buffer = module_buffer_capacity(MODULE_FRAME_PRESS);

    ASSERT_EQ_INT(station_module_flow_diag(&st, 0),
                  STATION_FLOW_DIAG_CONSUMER_FULL);
}

TEST(test_module_flow_diag_awaiting_supply) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .scaffold = true, .build_progress = 0.25f,
    };

    ASSERT_EQ_INT(station_module_flow_diag(&st, 0),
                  STATION_FLOW_DIAG_AWAITING_SUPPLY);
}

TEST(test_station_diag_serializes_module_flow_diag) {
    station_t st = {0};
    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };

    uint8_t buf[STATION_DIAG_SIZE];
    int len = serialize_station_diag(buf, 7, &st);
    ASSERT_EQ_INT(len, STATION_DIAG_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATION_DIAG);
    ASSERT_EQ_INT(buf[1], 7);
    ASSERT_EQ_INT(buf[2], 1);
    ASSERT_EQ_INT(buf[3], STATION_FLOW_DIAG_NO_INPUT);
}

TEST(test_station_flow_summary_formats_active_modules) {
    station_t st = {0};
    station_flow_summary_t summary;
    char line[64];

    st.signal_range = 1.0f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){ .type = MODULE_FRAME_PRESS };
    st.modules[1] = (station_module_t){ .type = MODULE_LASER_FAB };
    st.modules[0].flow_diag = (uint8_t)STATION_FLOW_DIAG_RUNNING;
    st.modules[1].flow_diag = (uint8_t)STATION_FLOW_DIAG_RUNNING;

    ASSERT(station_flow_summary(&st, true, &summary));
    ASSERT_EQ_INT(summary.diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(summary.active_count, 2);
    ASSERT(station_flow_summary_format(&summary, line, sizeof(line)));
    ASSERT_STR_EQ(line, "FLOW 2 modules active");
}

TEST(test_station_flow_summary_prioritizes_blocked_module) {
    station_t st = {0};
    station_flow_summary_t summary;
    char line[96];

    st.signal_range = 1.0f;
    st.module_count = 2;
    st.modules[0] = (station_module_t){ .type = MODULE_FRAME_PRESS };
    st.modules[1] = (station_module_t){ .type = MODULE_LASER_FAB };
    st.modules[0].flow_diag = (uint8_t)STATION_FLOW_DIAG_RUNNING;
    st.modules[1].flow_diag = (uint8_t)STATION_FLOW_DIAG_OUTPUT_FULL;

    ASSERT(station_flow_summary(&st, true, &summary));
    ASSERT_EQ_INT(summary.diag, STATION_FLOW_DIAG_OUTPUT_FULL);
    ASSERT_EQ_INT(summary.module_index, 1);
    ASSERT(station_flow_summary_format(&summary, line, sizeof(line)));
    ASSERT(strstr(line, "Laser") != NULL);
    ASSERT(strstr(line, "output full") != NULL);
}

TEST(test_station_identity_reconcile_clears_stale_module_diag) {
    station_t st = {0};
    station_module_t incoming[MAX_MODULES_PER_STATION] = {0};

    st.module_count = 2;
    st.modules[0] = (station_module_t){ .type = MODULE_FRAME_PRESS, .ring = 1,
                                         .slot = 0, .build_progress = 1.0f,
                                         .commodity = COMMODITY_COUNT };
    st.modules[1] = (station_module_t){ .type = MODULE_LASER_FAB, .ring = 2,
                                         .slot = 1, .build_progress = 1.0f,
                                         .commodity = COMMODITY_COUNT };
    st.modules[0].flow_diag = (uint8_t)STATION_FLOW_DIAG_RUNNING;
    st.modules[1].flow_diag = (uint8_t)STATION_FLOW_DIAG_OUTPUT_FULL;
    incoming[0] = st.modules[0];
    incoming[1] = st.modules[1];

    station_reconcile_module_diag_for_identity(&st, incoming, 2);
    ASSERT_EQ_INT(st.modules[0].flow_diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(st.modules[1].flow_diag, STATION_FLOW_DIAG_OUTPUT_FULL);

    incoming[1].build_progress = 0.5f;
    station_reconcile_module_diag_for_identity(&st, incoming, 2);
    ASSERT_EQ_INT(st.modules[0].flow_diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(st.modules[1].flow_diag, STATION_FLOW_DIAG_OUTPUT_FULL);

    incoming[1].type = MODULE_TRACTOR_FAB;
    station_reconcile_module_diag_for_identity(&st, incoming, 2);
    ASSERT_EQ_INT(st.modules[0].flow_diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(st.modules[1].flow_diag, STATION_FLOW_DIAG_NONE);

    st.modules[1].flow_diag = (uint8_t)STATION_FLOW_DIAG_RUNNING;
    station_reconcile_module_diag_for_identity(&st, incoming, 1);
    ASSERT_EQ_INT(st.modules[0].flow_diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(st.modules[1].flow_diag, STATION_FLOW_DIAG_NONE);
}

TEST(test_station_module_lifecycle_moves_and_clears_runtime) {
    station_t st = {0};
    station_module_t *dock = station_module_append(
        &st, MODULE_DOCK, 0, 0, false, 1.0f, COMMODITY_COUNT);
    station_module_t *press = station_module_append(
        &st, MODULE_FRAME_PRESS, 2, 1, false, 1.0f, COMMODITY_FRAME);
    station_module_t *laser = station_module_append(
        &st, MODULE_LASER_FAB, 3, 2, false, 1.0f,
        COMMODITY_LASER_MODULE);
    ASSERT(dock != NULL);
    ASSERT(press != NULL);
    ASSERT(laser != NULL);

    press->input_buffer = 3.0f;
    press->output_buffer = 4.0f;
    press->active_pulse = 0.5f;
    press->craft_progress = 0.25f;
    press->flow_diag = STATION_FLOW_DIAG_RUNNING;
    laser->input_buffer = 7.0f;

    ASSERT(station_module_remove(&st, 0));
    ASSERT_EQ_INT(st.module_count, 2);
    ASSERT_EQ_INT(st.modules[0].type, MODULE_FRAME_PRESS);
    ASSERT_EQ_FLOAT(st.modules[0].input_buffer, 3.0f, 0.001f);
    ASSERT_EQ_FLOAT(st.modules[0].output_buffer, 4.0f, 0.001f);
    ASSERT_EQ_FLOAT(st.modules[0].active_pulse, 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(st.modules[0].craft_progress, 0.25f, 0.001f);
    ASSERT_EQ_INT(st.modules[0].flow_diag, STATION_FLOW_DIAG_RUNNING);
    ASSERT_EQ_INT(st.modules[1].type, MODULE_LASER_FAB);
    ASSERT_EQ_FLOAT(st.modules[1].input_buffer, 7.0f, 0.001f);

    station_module_t *replacement = station_module_append(
        &st, MODULE_HOPPER, 1, 3, false, 1.0f, COMMODITY_FERRITE_INGOT);
    ASSERT(replacement != NULL);
    ASSERT_EQ_FLOAT(replacement->input_buffer, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(replacement->output_buffer, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(replacement->active_pulse, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(replacement->craft_progress, 0.0f, 0.001f);
    ASSERT_EQ_INT(replacement->flow_diag, STATION_FLOW_DIAG_NONE);
}

TEST(test_station_module_identity_copy_preserves_only_matching_runtime) {
    station_module_t local = {
        .type = MODULE_FRAME_PRESS,
        .ring = 2,
        .slot = 1,
        .commodity = COMMODITY_FRAME,
        .input_buffer = 3.0f,
        .flow_diag = STATION_FLOW_DIAG_RUNNING,
    };
    station_module_t incoming = local;
    incoming.build_progress = 0.5f;
    incoming.input_buffer = 0.0f;
    incoming.flow_diag = STATION_FLOW_DIAG_NONE;

    station_module_copy_identity(&local, &incoming);
    ASSERT_EQ_FLOAT(local.build_progress, 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(local.input_buffer, 3.0f, 0.001f);
    ASSERT_EQ_INT(local.flow_diag, STATION_FLOW_DIAG_RUNNING);

    incoming.type = MODULE_LASER_FAB;
    station_module_copy_identity(&local, &incoming);
    ASSERT_EQ_INT(local.type, MODULE_LASER_FAB);
    ASSERT_EQ_FLOAT(local.input_buffer, 0.0f, 0.001f);
    ASSERT_EQ_INT(local.flow_diag, STATION_FLOW_DIAG_NONE);
}

TEST(test_station_flow_summary_mirrored_authoritative) {
    station_t st = {0};
    station_flow_summary_t summary;

    st.signal_range = 1.0f;
    st.module_count = 1;
    st.modules[0] = (station_module_t){
        .type = MODULE_FRAME_PRESS, .ring = 2, .slot = 0,
        .build_progress = 1.0f,
    };

    ASSERT(!station_flow_summary(&st, true, &summary));
    ASSERT(station_flow_summary(&st, false, &summary));
    ASSERT_EQ_INT(summary.diag, STATION_FLOW_DIAG_NO_INPUT);
}

void register_construction_outposts_tests(void) {
    TEST_SECTION("\nStation construction (#83):\n");
    RUN(test_outpost_requires_signal_range);
    RUN(test_outpost_extends_signal_range);
    RUN(test_disconnected_station_goes_dark);
    RUN(test_outpost_requires_undocked);
    RUN(test_outpost_requires_towed_scaffold);
    RUN(test_outpost_min_distance);
}

void register_construction_modules_tests(void) {
    TEST_SECTION("\nModule construction:\n");
    RUN(test_module_build_material_types);
    RUN(test_module_construction_and_delivery);
    RUN(test_construction_consumes_manifest_units);
    RUN(test_station_scaffold_manifest_batch_append_failure_is_inert);
    RUN(test_module_delivery_emits_construction_chain_event);
    RUN(test_module_manifest_batch_append_failure_is_inert);
    RUN(test_module_delivery_consumes_towed_manifest_pod);
    RUN(test_module_physical_delivery_append_failure_is_inert);
    RUN(test_station_scaffold_rejects_towed_manifest_pod);
    RUN(test_present_towed_pod_rejects_unanchored_unknown_identity);
    RUN(test_present_towed_pod_signed_action_feeds_remote_scaffold);
    RUN(test_purchased_frame_pods_found_real_outpost_without_receipt_injection);
    RUN(test_present_station_custody_charges_once_with_paired_trade);
    RUN(test_present_station_custody_large_pod_conserves_aggregate_quote);
    RUN(test_present_towed_pod_rejects_consumed_identity_replay);
    RUN(test_present_towed_pod_rejects_identity_spent_by_ordinary_transfer);
    RUN(test_present_towed_pod_rejects_wrong_origin_and_tamper);
    RUN(test_present_towed_pod_preflights_tampered_tail);
    RUN(test_present_towed_pod_rejects_recycled_stale_selection);
    RUN(test_present_towed_pod_log_failures_are_byte_inert);
    RUN(test_docked_buy_one_unit_per_intent);
    RUN(test_one_shipyard_builds_ships_two_shipyards_build_station_modules);
    RUN(test_shipyard_commission_completes_onto_docked_player);
    RUN(test_shipyard_birth_assembly_consumes_three_fragments);
    RUN(test_shipyard_birth_assembly_roundtrips_and_completes_offline);
    RUN(test_shipyard_birth_fragments_cannot_back_two_commissions);
    RUN(test_shipyard_queue_compaction_moves_birth_sidecar_with_owner);
    RUN(test_shipyard_commission_owner_survives_player_slot_reuse);
    RUN(test_shipyard_commission_debits_player_ledger);
    RUN(test_shipyard_commission_consumes_towed_material_pods);
    RUN(test_shipyard_station_request_consumes_staged_material_pods);
    RUN(test_shipyard_station_request_rejects_far_staged_hoppers);
    RUN(test_shipyard_station_request_rejects_split_yard_materials);
    RUN(test_shipyard_player_commission_rejects_split_yard_towed_materials);
    RUN(test_shipyard_station_request_rejects_inventory_without_yard_hoppers);
    RUN(test_shipyard_player_commission_rejects_inventory_without_yard_hoppers);
    RUN(test_shipyard_manufacture_consumes_staged_material_pod);
    RUN(test_shipyard_commission_rejects_invalid_owner_without_draining_materials);
    RUN(test_shipyard_commission_rejects_session_only_owner_without_draining);
    RUN(test_world_seed_station_manifests_matches_float);
    RUN(test_kepler_starts_with_frame_pod_not_frame_inventory);
    RUN(test_module_activation_does_not_spawn_free_worker_hull);
}

void register_construction_collision238_tests(void) {
    TEST_SECTION("\nCollision accuracy (#238):\n");
    RUN(test_238_station_core_blocks_player);
    RUN(test_238_module_circle_blocks_player);
    RUN(test_238_corridor_blocks_radial_approach);
    RUN(test_238_fragment_collides_with_corridor_wall);
    RUN(test_238_dock_gap_allows_entry);
    RUN(test_238_corridor_angular_edge_no_clip);
    RUN(test_238_module_corridor_junction_no_jitter);
    RUN(test_238_invisible_wall_repro);
}

void register_construction_station_geom_tests(void) {
    TEST_SECTION("\nStation geometry emitter:\n");
    RUN(test_station_geom_emitter_prospect);
    RUN(test_station_cell_topology_is_canonical_from_persisted_module_records);
    RUN(test_furnace_geom_spokes_use_instance_ore_tag);
    RUN(test_station_geom_spoke_uses_module_diag_fallback);
}

void register_construction_scaffold_tests(void) {
    TEST_SECTION("\nScaffold entity (#277):\n");
    RUN(test_scaffold_spawn);
    RUN(test_scaffold_physics_loose);
    RUN(test_scaffold_towed_scaffold_init);
    RUN(test_scaffold_tow_pickup);
    RUN(test_scaffold_tow_release_on_r);
    RUN(test_scaffold_tow_release_on_dock);
    RUN(test_scaffold_tow_speed_cap);
    RUN(test_scaffold_snap_to_slot);
    RUN(test_scaffold_snap_ignores_starter_stations);
    RUN(test_scaffold_full_pipeline);
    RUN(test_build_outpost_full_economy);
    RUN(test_scaffold_ship_drag);
    RUN(test_frontier_virtual_pilots_plan_and_order_relay);
    RUN(test_invalid_outpost_plan_preserves_existing_blueprint);
    RUN(test_frontier_virtual_pilots_scale_planned_queue);
    RUN(test_frontier_virtual_pilots_execute_growth_loop);
    RUN(test_hauler_delivers_to_planned_outpost);
    RUN(test_save_preserves_pending_scaffolds);
    RUN(test_shipyard_queue_waits_for_loose_scaffold_to_clear);
}

void register_construction_placed_scaffold_tests(void) {
    TEST_SECTION("\nPlaced-scaffold supply (#277):\n");
    RUN(test_placed_scaffold_supply_phase);
    RUN(test_placed_scaffold_manifest_supply_batches_multiple_units);
    RUN(test_placed_scaffold_manifest_supply_batch_failure_is_inert);
    RUN(test_placed_scaffold_supply_consumes_staged_material_pod);
    RUN(test_placed_scaffold_physical_supply_append_failure_is_inert);
    RUN(test_placed_scaffold_supply_rejects_far_staged_hopper);
    RUN(test_placed_scaffold_player_delivery);
    RUN(test_construction_contract_closes_on_activation);
    RUN(test_stale_contract_does_not_block_next_need);
    RUN(test_construction_contract_checks_scaffold_not_threshold);
}

TEST(test_pair_neighbors_geometry) {
    /* Cross-ring pair geometry — slot angles map across adjacent rings.
     * Producer on ring N at slot S → closest-angle slot on ring N±1.
     * Tie-break: lower slot index wins. */
    station_slot_pair_t out[2];

    /* Ring 1 has only ring 2 as a neighbor. */
    int n = station_pair_neighbors(1, 0, out);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0].ring, 2);
    ASSERT_EQ_INT(out[0].slot, 0);  /* 0° → ring-2 slot 0 (0°) */

    n = station_pair_neighbors(1, 2, out);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0].ring, 2);
    ASSERT_EQ_INT(out[0].slot, 4);  /* 240° → ring-2 slot 4 (240°) */

    /* Ring 3 has only ring 2 as a neighbor. */
    n = station_pair_neighbors(3, 6, out);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0].ring, 2);
    ASSERT_EQ_INT(out[0].slot, 4);  /* 240° → ring-2 slot 4 (240°) */

    /* Ring 2 has both ring 1 and ring 3 as neighbors. Outer first. */
    n = station_pair_neighbors(2, 0, out);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(out[0].ring, 3);
    ASSERT_EQ_INT(out[0].slot, 0);
    ASSERT_EQ_INT(out[1].ring, 1);
    ASSERT_EQ_INT(out[1].slot, 0);

    /* Tie-break: ring-2 slot 1 (60°) is equidistant from ring-1 slot 0
     * (0°, 60° off) and slot 1 (120°, 60° off) — strict-less-than picks
     * the lower index, slot 0. Ring-3 slot 1 (40°, 20° off) and slot 2
     * (80°, 20° off) tie — picks slot 1. */
    n = station_pair_neighbors(2, 1, out);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(out[0].ring, 3);
    ASSERT_EQ_INT(out[0].slot, 1);
    ASSERT_EQ_INT(out[1].ring, 1);
    ASSERT_EQ_INT(out[1].slot, 0);
}

TEST(test_pair_satisfied_cross_ring) {
    /* Producer pair-validation under the commodity-tagged hopper
     * model: a producer is satisfied when ALL its required input
     * commodities have a tagged hopper somewhere on the station.
     * For LASER_FAB that means BOTH crystal ingot AND frame
     * hoppers must exist. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    station_t *st = &w->stations[5]; /* unused slot, completely empty */
    station_cleanup(st);
    memset(st, 0, sizeof(*st));
    st->signal_range = 1.0f;

    /* Empty station — no hoppers, LASER_FAB cannot be paired. */
    ASSERT(!station_pair_satisfied(st, 2, 3, MODULE_LASER_FAB));

    /* Add only one of the two — still not satisfied. */
    add_hopper_for(st, 3, 4, COMMODITY_CRYSTAL_INGOT);
    ASSERT(!station_pair_satisfied(st, 2, 3, MODULE_LASER_FAB));

    /* Add the second commodity — now satisfied. */
    add_hopper_for(st, 3, 5, COMMODITY_FRAME);
    ASSERT(station_pair_satisfied(st, 2, 3, MODULE_LASER_FAB));

    /* FURNACE accepts ANY ore, but the hopper has to sit on an adjacent ring. */
    station_t *st2 = &w->stations[6];
    station_cleanup(st2);
    memset(st2, 0, sizeof(*st2));
    st2->signal_range = 1.0f;
    ASSERT(!station_pair_satisfied(st2, 2, 0, MODULE_FURNACE));
    add_hopper_for(st2, 2, 1, COMMODITY_FERRITE_ORE);
    ASSERT(!station_pair_satisfied(st2, 2, 0, MODULE_FURNACE));
    st2->module_count = 0;
    add_hopper_for(st2, 3, 0, COMMODITY_FERRITE_ORE);
    ASSERT(station_pair_satisfied(st2, 2, 0, MODULE_FURNACE));

    /* Non-producer modules are always satisfied. */
    ASSERT(station_pair_satisfied(st, 2, 3, MODULE_DOCK));
    ASSERT(station_pair_satisfied(st, 1, 0, MODULE_SIGNAL_RELAY));
}

TEST(test_helios_rings_rotate_under_dynamics) {
    /* All three Helios rings carry seeded drift bias so the station reads
     * as continuously alive even before production spokes are hot. After
     * 2 sim seconds, every populated ring should visibly advance. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[2];
    for (int ring = 0; ring < 3; ring++)
        ASSERT(st->arm_speed[ring] > 0.0f);
    float r0[3] = { st->arm_rotation[0], st->arm_rotation[1], st->arm_rotation[2] };
    for (int i = 0; i < 240; i++) world_sim_step(w, 1.0f / 120.0f);
    for (int ring = 0; ring < 3; ring++) {
        float delta = st->arm_rotation[ring] - r0[ring];
        ASSERT(delta > 0.03f);
    }
}

TEST(test_helios_ring2_keeps_legacy_rotation_rate) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[2];
    ASSERT(st->arm_speed[1] > 0.0f);
    float r0 = st->arm_rotation[1];
    for (int i = 0; i < 240; i++) world_sim_step(w, 1.0f / 120.0f);
    float r1 = st->arm_rotation[1];
    /* Expect ~speed * 2.0s = 0.08 rad of rotation. */
    ASSERT(r1 - r0 > 0.05f);
}

TEST(test_targeted_spokes_drive_only_loaded_rings) {
    /* With per-instance furnace tags, Helios no longer gets fake spoke
     * torque from unrelated ore hoppers. Both furnace rings now carry
     * real load: ring 1 is the first crystal pass, and ring 3 carries
     * the second crystal pass plus cuprite/fab load. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[2];
    /* Force every Helios producer's pulse high so spokes are taut. */
    for (int m = 0; m < st->module_count; m++) {
        if (module_is_producer(st->modules[m].type)) st->modules[m].active_pulse = 1.0f;
    }
    float r1_0 = st->arm_rotation[0];  /* first crystal pass */
    float r3_0 = st->arm_rotation[2];  /* ring 3 */
    for (int i = 0; i < 1200; i++) {  /* 10 sim seconds */
        for (int m = 0; m < st->module_count; m++) {
            if (module_is_producer(st->modules[m].type)) st->modules[m].active_pulse = 1.0f;
        }
        world_sim_step(w, 1.0f / 120.0f);
    }
    float r1_1 = st->arm_rotation[0];
    float r3_1 = st->arm_rotation[2];
    ASSERT(fabsf(r1_1 - r1_0) > 0.01f);
    ASSERT(fabsf(r3_1 - r3_0) > 0.01f);
}

TEST(test_output_hopper_spoke_contributes_torque) {
    /* Slice 1.5a — output hoppers participate in spoke physics. A
     * synthetic 2-ring station with only an output spoke (no input
     * spoke) must still apply torque to its passive ring when the
     * producer's pulse is hot. Asserts both magnitude AND direction:
     * a producer that's behind its hopper in phase pulls the hopper
     * ring backward (torque toward closing the phase gap), and the
     * producer ring forward — Newton's third. A sign-flip in the
     * spoke math would fail the direction assertion. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    station_t *st = &w->stations[0];
    st->signal_range = 1.0f;
    /* Frame press on ring 2 slot 0; frame output hopper on ring 3
     * slot 4 (160° ahead — hopper leads the producer in phase). */
    add_module_at(st, MODULE_FRAME_PRESS, 2, 0);
    add_hopper_for(st, 3, 4, COMMODITY_FRAME);
    /* No drift bias — isolate the spoke contribution. arm_omega all 0. */
    st->modules[0].active_pulse = 1.0f;

    /* Single tick: omega should become non-zero on both endpoint rings,
     * with opposite signs (Newton's third). */
    float r2_omega_pre = st->arm_omega[1];
    float r3_omega_pre = st->arm_omega[2];
    world_sim_step(w, 1.0f / 120.0f);
    float r2_omega_post = st->arm_omega[1];
    float r3_omega_post = st->arm_omega[2];
    float dr2 = r2_omega_post - r2_omega_pre;
    float dr3 = r3_omega_post - r3_omega_pre;

    /* Magnitude: spoke applied torque to both rings. */
    ASSERT(fabsf(dr2) > 1e-5f);
    ASSERT(fabsf(dr3) > 1e-5f);
    /* Direction: signs are opposite (Newton's third — a sign flip
     * would push both rings the same way, failing this). */
    ASSERT(dr2 * dr3 < 0.0f);
    /* Phase pursuit: hopper leads (slot 4 of 9 = ~160° vs 0°), so dr =
     * +160°, sin(dr) > 0, T = K*sin(dr) > 0. apply_spoke_torque adds
     * +T to producer ring (ra=2) and -T to hopper ring (rb=3). So ring 2
     * accelerates positive (toward the hopper) and ring 3 decelerates
     * negative (away from the producer). */
    ASSERT(dr2 > 0.0f);
    ASSERT(dr3 < 0.0f);
}

TEST(test_seeded_kepler_shipyard_inner_ring_layout) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    const station_t *st = &w->stations[1];
    ASSERT_EQ_INT(station_module_at(st, 1, 2), MODULE_SHIPYARD);
    ASSERT_EQ_INT(station_module_at(st, 3, 4), MODULE_SHIPYARD);
    ASSERT_EQ_INT(station_module_at(st, 2, 0), MODULE_FRAME_PRESS);
    ASSERT_EQ_INT(ring_module_count(st, 3), 2);

    int frame_hopper = station_find_hopper_for(st, COMMODITY_FRAME);
    int laser_hopper = station_find_hopper_for(st, COMMODITY_LASER_MODULE);
    int tractor_hopper = station_find_hopper_for(st, COMMODITY_TRACTOR_MODULE);
    int ferrite_hopper = station_find_hopper_for(st, COMMODITY_FERRITE_INGOT);
    ASSERT(frame_hopper >= 0);
    ASSERT(laser_hopper >= 0);
    ASSERT(tractor_hopper >= 0);
    ASSERT(ferrite_hopper >= 0);

    ASSERT_EQ_INT(st->modules[frame_hopper].ring, 2);
    ASSERT_EQ_INT(st->modules[frame_hopper].slot, 4);
    ASSERT_EQ_INT(st->modules[laser_hopper].ring, 2);
    ASSERT_EQ_INT(st->modules[tractor_hopper].ring, 2);
    ASSERT_EQ_INT(st->modules[ferrite_hopper].ring, 3);
    ASSERT(station_pair_satisfied(st, 1, 2, MODULE_SHIPYARD));
    ASSERT(station_pair_satisfied(st, 3, 4, MODULE_SHIPYARD));
    ASSERT(station_pair_satisfied(st, 2, 0, MODULE_FRAME_PRESS));
}

TEST(test_seed_stations_pair_complete) {
    /* Every producer on every starter station must have its cross-ring
     * pair-intake already satisfied at boot. This is the construction
     * regression catch — drift in either game_sim seeding or the pair
     * helper trips this. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    for (int s = 0; s < 3; s++) {
        const station_t *st = &w->stations[s];
        for (int m = 0; m < st->module_count; m++) {
            const station_module_t *mod = &st->modules[m];
            if (mod->scaffold) continue;
            if (!module_requires_pair(mod->type)) continue;
            ASSERT(station_pair_satisfied(st, mod->ring, mod->slot, mod->type));
        }
    }
}

TEST(test_station_placement_contract_distinguishes_plan_and_physical) {
    station_t st = {0};
    st.signal_range = 100.0f;

    /* A future plan may declare a producer before its intake is built; a
     * physical scaffold may not materialize into that invalid live layout. */
    ASSERT_EQ_INT(station_placement_validate(
                      &st, MODULE_FRAME_PRESS, 2, 0,
                      STATION_PLACEMENT_PLAN),
                  STATION_PLACEMENT_OK);
    ASSERT_EQ_INT(station_placement_validate(
                      &st, MODULE_FRAME_PRESS, 2, 0,
                      STATION_PLACEMENT_PHYSICAL),
                  STATION_PLACEMENT_MISSING_INPUT_PAIR);

    st.modules[0] = (station_module_t){
        .type = MODULE_SIGNAL_RELAY,
        .ring = 2,
        .slot = 0,
        .build_progress = 1.0f,
    };
    st.module_count = 1;
    ASSERT_EQ_INT(station_placement_validate(
                      &st, MODULE_FRAME_PRESS, 2, 0,
                      STATION_PLACEMENT_PLAN),
                  STATION_PLACEMENT_SLOT_OCCUPIED);
}

void register_construction_module_schema_tests(void) {
    TEST_SECTION("\nModule schema (#280):\n");
    RUN(test_module_build_state_lifecycle);
    RUN(test_module_schema_basic_kinds);
    RUN(test_module_schema_producer_io);
    RUN(test_module_schema_required_output);
    RUN(test_module_furnace_instance_tag);
    RUN(test_commodity_ore_ingot_pairing);
    RUN(test_station_module_layout_status_missing_output);
    RUN(test_station_module_layout_status_no_local_consumer_is_ok);
    RUN(test_station_module_layout_status_furnace_uses_tag);
    RUN(test_station_module_layout_status_furnace_requires_adjacent_ore_hopper);
    RUN(test_station_module_layout_status_shipyard_exempt);
    RUN(test_seeded_furnaces_tagged);
    RUN(test_seeded_helios_output_hoppers);
    RUN(test_seeded_stations_layout_ok);
    RUN(test_module_schema_valid_rings);
    RUN(test_module_schema_helpers);
    RUN(test_module_schema_build_costs_match);
    RUN(test_module_schema_uses_accepted_rock_cell_balance);
    RUN(test_module_flow_same_ring_transfer);
    RUN(test_module_flow_production_fills_buffers);
    RUN(test_module_flow_does_not_overflow_capacity);
    RUN(test_module_flow_storage_feeds_consumer);
    RUN(test_station_default_module_commodity_picks_uncovered_input);
    RUN(test_station_plan_flow_hint_connected_output);
    RUN(test_station_plan_flow_hint_slow_hopper_feed);
    RUN(test_station_plan_flow_hint_no_consumer);
    RUN(test_module_flow_diag_no_input);
    RUN(test_module_flow_diag_output_full);
    RUN(test_module_flow_diag_no_consumer);
    RUN(test_module_flow_diag_slow_feed);
    RUN(test_module_flow_diag_storage_consumer_full);
    RUN(test_module_flow_diag_awaiting_supply);
    RUN(test_station_diag_serializes_module_flow_diag);
    RUN(test_station_flow_summary_formats_active_modules);
    RUN(test_station_flow_summary_prioritizes_blocked_module);
    RUN(test_station_identity_reconcile_clears_stale_module_diag);
    RUN(test_station_module_lifecycle_moves_and_clears_runtime);
    RUN(test_station_module_identity_copy_preserves_only_matching_runtime);
    RUN(test_station_flow_summary_mirrored_authoritative);
    RUN(test_pair_neighbors_geometry);
    RUN(test_pair_satisfied_cross_ring);
    RUN(test_seeded_kepler_shipyard_inner_ring_layout);
    RUN(test_seed_stations_pair_complete);
    RUN(test_station_placement_contract_distinguishes_plan_and_physical);
    RUN(test_helios_rings_rotate_under_dynamics);
    RUN(test_helios_ring2_keeps_legacy_rotation_rate);
    RUN(test_targeted_spokes_drive_only_loaded_rings);
    RUN(test_output_hopper_spoke_contributes_torque);
}
