/*
 * local_server.c -- In-process authoritative simulation for singleplayer.
 *
 * SP runs the same `world_sim_step` the dedicated server runs, but in
 * the same address space. After each step we mirror the server's
 * world (`ls->world`) into the client's view (`g.world`).
 *
 * The sync rules — read these before adding any new field to world_t
 * or server_player_t:
 *
 *  1. Whole-world arrays (asteroids, npcs, stations, contracts,
 *     scaffolds) are copied with one memcpy each. New world_t arrays
 *     should be added in the WHOLE_WORLD_FIELDS block below — that's
 *     the only edit point.
 *
 *  2. The local player's ship has TWO classes of state:
 *
 *     - "always-sync" — pose, dock state, render hints. These are
 *       overwritten every frame; no risk of flicker because the
 *       client never optimistically modifies them.
 *
 *     - "predict-protected" — hull/credits/cargo/levels. The client
 *       optimistically modifies these from input.c at keypress time
 *       so the UI doesn't lag a frame. The sync skips them while
 *       g.action_predict_timer > 0 so the optimistic value isn't
 *       overwritten by a stale-by-one-frame mirror. Once the timer
 *       drops, the server-authoritative value wins.
 *
 *  3. Sim events flow through `g.world.events` so process_sim_events
 *     can read them on the client side.
 *
 *  Adding a sim field that the client should see: pick category 1, 2a,
 *  or 2b above and edit ONE block. If a category 1 field doesn't show
 *  up on the client, this file is the first place to look.
 */
#include "local_server.h"
#include "client.h"
#include "manifest.h"
#include "mining_client.h"
#include "sim_ai.h"
#include "sim_asteroid.h"

#include <string.h>

static void local_server_process_fracture_updates(local_server_t *ls, int player_slot);

void local_server_init(local_server_t *ls, uint32_t seed) {
    memset(ls, 0, sizeof(*ls));
    ls->world.rng = seed ? seed : 2037u;
    world_reset(&ls->world);
    /* Mirror the dedicated-server load path: turn the seeded float
     * inventory into manifest units so the manifest-only TRADE picker
     * has rows to surface. Without this, a fresh singleplayer start
     * shows empty markets at every station. */
    world_seed_station_manifests(&ls->world);
    /* Singleplayer is always a fresh world at this layer (no save
     * load); seed the chain log genesis events so MOTDs are part of
     * the chain history just like on the dedicated server. */
    world_seed_station_chain_genesis(&ls->world);
    ls->world.players[0].connected = true;
    ls->world.players[0].id = 0;
    ls->world.players[0].session_ready = true;
    /* Deterministic token so the ledger can track singleplayer credits */
    memset(ls->world.players[0].session_token, 0x01, sizeof(ls->world.players[0].session_token));
    player_init_ship(&ls->world.players[0], &ls->world);
    player_seed_credits(&ls->world.players[0], &ls->world);
    ls->active = true;
}

void local_server_step(local_server_t *ls, int player_slot,
                        const input_intent_t *input, float dt) {
    if (!ls->active) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    ls->world.players[player_slot].input = *input;
    world_sim_step(&ls->world, dt);
    local_server_process_fracture_updates(ls, player_slot);
}

static void local_server_process_fracture_updates(local_server_t *ls, int player_slot) {
    if (!ls || player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    /* Per-asteroid legacy path handles the common "asteroid still
     * alive at resolve time" case. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        fracture_claim_state_t *state = &ls->world.fracture_claims[i];
        if (state->challenge_dirty && state->fracture_id) {
            mining_client_claim_t claim = {0};
            if (mining_client_search_fracture(state->fracture_id,
                                              ls->world.asteroids[i].fracture_seed,
                                              state->deadline_ms, state->burst_cap,
                                              &claim)) {
                (void)submit_fracture_claim(&ls->world, player_slot, claim.fracture_id,
                                            claim.burst_nonce,
                                            (uint8_t)claim.claimed_grade);
            }
            state->challenge_dirty = false;
        }
        if (state->resolved_dirty && state->fracture_id) {
            mining_client_resolve_fracture(state->fracture_id,
                                           (mining_grade_t)state->best_grade);
            state->resolved_dirty = false;
        }
    }
    /* Pending resolves queue: fracture_commit_resolution pushes here
     * so "resolve + smelt clear in same tick" still delivers the resolve
     * message to the local client. Drain one pass per step. */
    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        pending_resolve_t *pr = &ls->world.pending_resolves[p];
        if (!pr->active) continue;
        mining_client_resolve_fracture(pr->fracture_id,
                                       (mining_grade_t)pr->grade);
        pr->tx_count++;
        if (pr->tx_count >= FRACTURE_RESOLVE_RETRY_COUNT) pr->active = false;
    }
}

/* (1) Whole-world arrays. Add new world_t arrays here as one line. */
static void mirror_whole_world(const world_t *src) {
    g.world.station_count = src->station_count;
    g.world.next_station_id = src->next_station_id;
    g.world.rng = src->rng;
    g.world.belt_seed = src->belt_seed;
    g.world.world_seq = src->world_seq;
    g.world.field_spawn_timer = src->field_spawn_timer;
    g.world.gravity_accumulator = src->gravity_accumulator;
    g.world.hopper_smelt_events = src->hopper_smelt_events;
    g.world.hopper_smelt_units = src->hopper_smelt_units;
    g.world.npc_respawn_timer = src->npc_respawn_timer;
    g.world.next_npc_token = src->next_npc_token;
    g.world.player_only_mode = false;
    g.world.next_fracture_id = src->next_fracture_id;
    g.world.belt = src->belt;
    g.world.signal_channel = src->signal_channel;
    memcpy(g.world.asteroid_origin, src->asteroid_origin, sizeof(g.world.asteroid_origin));
    memcpy(g.world.fracture_claims, src->fracture_claims, sizeof(g.world.fracture_claims));
    memcpy(g.world.pending_resolves, src->pending_resolves, sizeof(g.world.pending_resolves));
    memcpy(g.world.destroyed_rocks, src->destroyed_rocks, sizeof(g.world.destroyed_rocks));
    g.world.destroyed_rock_count = src->destroyed_rock_count;
    memcpy(g.world.pubkey_registry, src->pubkey_registry, sizeof(g.world.pubkey_registry));

    memcpy(g.world.asteroids, src->asteroids, sizeof(g.world.asteroids));
    memcpy(g.world.npc_ships, src->npc_ships, sizeof(g.world.npc_ships));
    for (int i = 0; i < MAX_STATIONS; i++) {
        /* Diff inventory + credit_pool before the copy clobbers them —
         * heartbeat fires on production cycles, ore intakes, sales,
         * and ledger movement. Same thresholds as apply_remote_stations
         * (MP path). credit_pool is derived from -Σ(ledger.balance);
         * use station_credit_pool() so this matches the wire value. */
        const station_t *src_st = &src->stations[i];
        if (g.station_prev_seen[i] && station_exists(src_st)) {
            bool fired = false;
            for (int c = 0; c < COMMODITY_COUNT; c++) {
                if (fabsf(src_st->_inventory_cache[c] -
                          g.station_prev_inventory[i][c]) >= 0.5f) {
                    fired = true;
                    break;
                }
            }
            if (!fired) {
                float pool = station_credit_pool(src_st);
                if (fabsf(pool - g.station_prev_credit_pool[i]) >= 5.0f)
                    fired = true;
            }
            if (fired) g.station_heartbeat[i] = 1.0f;
        }
        for (int c = 0; c < COMMODITY_COUNT; c++)
            g.station_prev_inventory[i][c] = src_st->_inventory_cache[c];
        g.station_prev_credit_pool[i] = station_credit_pool(src_st);
        g.station_prev_seen[i] = station_exists(src_st);
        (void)station_copy(&g.world.stations[i], src_st);
    }
    memcpy(g.world.contracts, src->contracts, sizeof(g.world.contracts));
    memcpy(g.world.scaffolds, src->scaffolds, sizeof(g.world.scaffolds));
    memcpy(g.world.cargo_pods, src->cargo_pods, sizeof(g.world.cargo_pods));
    g.world.events = src->events;
    g.world.time   = src->time;

    /* Singleplayer mirror of NET_MSG_PLAYER_KNOWN_CONTRACTS. The local
     * mirror keeps g.world.contracts[] in raw world slots, so its mask
     * uses raw slots too; multiplayer uses the compact NET_MSG_CONTRACTS
     * ordinal space because its contract array is compacted on receipt. */
    uint32_t mask = 0;
    if (g.local_player_slot >= 0 && g.local_player_slot < MAX_PLAYERS) {
        const ship_t *ship = &src->players[g.local_player_slot].ship;
        for (int k = 0; k < MAX_CONTRACTS && k < 32; k++) {
            if (!src->contracts[k].active) continue;
            for (int i = 0; i < ship->known_contract_count; i++) {
                const contract_summary_t *cs = &ship->known_contracts[i];
                if (!cs->active) continue;
                if (cs->action == (uint8_t)src->contracts[k].action &&
                    cs->station_index == src->contracts[k].station_index &&
                    cs->commodity == (uint8_t)src->contracts[k].commodity &&
                    cs->required_grade == src->contracts[k].required_grade &&
                    cs->proof_flags == src->contracts[k].proof_flags &&
                    cs->required_prefix_class == src->contracts[k].required_prefix_class &&
                    cs->required_recipe_id == src->contracts[k].required_recipe_id &&
                    memcmp(cs->required_parent, src->contracts[k].required_parent, 32) == 0 &&
                    cs->forbidden_origin_mask == src->contracts[k].forbidden_origin_mask &&
                    memcmp(cs->target_pub, src->contracts[k].target_pub, 32) == 0) {
                    mask |= (1u << k);
                    break;
                }
            }
        }
    }
    g.player_known_contract_mask = mask;
}

/* (2a) Local player ship — always-sync fields (no client optimism). */
static void mirror_player_always(server_player_t *dst, const server_player_t *src) {
    memcpy(dst->session_token, src->session_token, sizeof(dst->session_token));
    dst->session_ready = src->session_ready;
    memcpy(dst->pubkey, src->pubkey, sizeof(dst->pubkey));
    dst->pubkey_set = src->pubkey_set;
    snprintf(dst->callsign, sizeof(dst->callsign), "%s", src->callsign);
    (void)manifest_clone(&dst->ship.manifest, &src->ship.manifest);
    dst->ship.pos    = src->ship.pos;
    dst->ship.vel    = src->ship.vel;
    dst->ship.angle  = src->ship.angle;
    dst->ship.tractor_active = src->ship.tractor_active;
    /* Dock state */
    dst->docked          = src->docked;
    dst->current_station = src->current_station;
    dst->in_dock_range   = src->in_dock_range;
    dst->nearby_station  = src->nearby_station;
    dst->dock_berth      = src->dock_berth;
    dst->docking_approach= src->docking_approach;
    /* Autopilot state — read by HUD for indicator */
    dst->autopilot_mode  = src->autopilot_mode;
    dst->autopilot_state = src->autopilot_state;
    dst->autopilot_target= src->autopilot_target;
    /* Beam / targeting (render hints) */
    dst->beam_active      = src->beam_active;
    dst->beam_hit         = src->beam_hit;
    dst->beam_ineffective = src->beam_ineffective;
    dst->beam_start       = src->beam_start;
    dst->beam_end         = src->beam_end;
    dst->scan_active      = src->scan_active;
    dst->scan_target_type = src->scan_target_type;
    dst->scan_target_index= src->scan_target_index;
    dst->scan_module_index= src->scan_module_index;
    dst->hover_asteroid   = src->hover_asteroid;
    dst->tractor_fragments= src->tractor_fragments;
    dst->nearby_fragments = src->nearby_fragments;
    /* Tow state (for tether rendering) */
    dst->ship.towed_count    = src->ship.towed_count;
    memcpy(dst->ship.towed_fragments, src->ship.towed_fragments, sizeof(dst->ship.towed_fragments));
    dst->ship.towed_pod_count = src->ship.towed_pod_count;
    memcpy(dst->ship.towed_pods, src->ship.towed_pods, sizeof(dst->ship.towed_pods));
    dst->ship.towed_scaffold = src->ship.towed_scaffold;
}

/* (2b) Local player ship — predict-protected fields. */
static void mirror_player_predicted(server_player_t *dst, const server_player_t *src) {
    dst->ship.hull          = src->ship.hull;
    /* credits removed — balance lives in station ledger (mirrored via stations memcpy) */
    dst->ship.mining_level  = src->ship.mining_level;
    dst->ship.hold_level    = src->ship.hold_level;
    dst->ship.tractor_level = src->ship.tractor_level;
    memcpy(dst->ship.cargo, src->ship.cargo, sizeof(dst->ship.cargo));
}

static void local_server_copy_inspect_row(NetInspectSnapshotRow *row,
                                          const cargo_unit_t *unit,
                                          const cargo_receipt_chain_t *chain) {
    memset(row, 0, sizeof(*row));
    if (!unit) return;
    row->commodity = unit->commodity;
    row->grade = unit->grade;
    row->quantity = 1;
    memcpy(row->cargo_pub, unit->pub, sizeof(row->cargo_pub));
    if (chain && chain->len > 0) {
        const cargo_receipt_t *origin = &chain->links[0];
        const cargo_receipt_t *latest = &chain->links[chain->len - 1];
        row->chain_len = chain->len;
        row->flags |= INSPECT_ROW_HAS_RECEIPT;
        row->event_id = latest->event_id;
        cargo_receipt_hash(latest, row->receipt_head);
        memcpy(row->origin_station, origin->authoring_station, sizeof(row->origin_station));
        memcpy(row->latest_station, latest->authoring_station, sizeof(row->latest_station));
    }
}

static void local_server_copy_inspect_group(NetInspectSnapshotRow *row,
                                            uint8_t commodity,
                                            uint8_t grade,
                                            uint16_t quantity,
                                            uint8_t prefix_class) {
    memset(row, 0, sizeof(*row));
    row->commodity = commodity;
    row->grade = grade;
    row->quantity = quantity > 0 ? quantity : 1;
    row->chain_len = prefix_class;  /* repurposed when GROUPED is set */
    row->flags |= INSPECT_ROW_GROUPED;
}

static uint8_t local_inspect_diag_kind_from_market(uint8_t memory_kind) {
    switch ((market_memory_kind_t)memory_kind) {
    case MARKET_MEMORY_DEMAND:           return (uint8_t)INSPECT_DIAG_MARKET_DEMAND;
    case MARKET_MEMORY_SUPPLY:           return (uint8_t)INSPECT_DIAG_MARKET_SUPPLY;
    case MARKET_MEMORY_ROUTE_DANGER:     return (uint8_t)INSPECT_DIAG_ROUTE_DANGER;
    case MARKET_MEMORY_ROUTE_SUCCESS:    return (uint8_t)INSPECT_DIAG_ROUTE_SUCCESS;
    case MARKET_MEMORY_DELIVERY_RECEIPT: return (uint8_t)INSPECT_DIAG_DELIVERY_RECEIPT;
    case MARKET_MEMORY_ROUTE_REPUTATION: return (uint8_t)INSPECT_DIAG_ROUTE_REPUTATION;
    case MARKET_MEMORY_ROUTE_RISK:       return (uint8_t)INSPECT_DIAG_ROUTE_RISK;
    case MARKET_MEMORY_STATION_TRUST:    return (uint8_t)INSPECT_DIAG_STATION_TRUST;
    case MARKET_MEMORY_STATION_RISK:     return (uint8_t)INSPECT_DIAG_STATION_RISK;
    case MARKET_MEMORY_NONE:
    default:                             return (uint8_t)INSPECT_DIAG_NONE;
    }
}

static bool local_inspect_market_memory_from_item(const knowledge_item_t *item,
                                                  market_memory_t *out) {
    if (!item || !out) return false;
    if (item->kind != (uint8_t)KNOW_MARKET) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY) return false;
    market_memory_t memory;
    memset(&memory, 0, sizeof(memory));
    memcpy(&memory, item->payload, sizeof(memory));
    if (!memory.active) return false;
    if (memory.memory_kind == (uint8_t)MARKET_MEMORY_NONE) return false;
    memory.confidence = item->confidence;
    memory.salience = item->salience;
    *out = memory;
    return true;
}

static void local_server_copy_inspect_market_diag(NetInspectSnapshotRow *row,
                                                  const market_memory_t *memory,
                                                  const knowledge_item_t *item) {
    memset(row, 0, sizeof(*row));
    if (!memory) return;
    row->commodity = local_inspect_diag_kind_from_market(memory->memory_kind);
    row->grade = memory->confidence;
    row->chain_len = memory->salience;
    row->flags = INSPECT_ROW_DIAGNOSTIC;
    row->event_id = (uint64_t)memory->station_a
                  | ((uint64_t)memory->station_b << 8)
                  | ((uint64_t)memory->action << 16)
                  | ((uint64_t)memory->commodity << 24);
    row->quantity = memory->value_hint ? memory->value_hint : memory->quantity_hint;
    if (item) {
        memcpy(row->cargo_pub, item->subject_hash, sizeof(row->cargo_pub));
        memcpy(row->receipt_head, item->chain_anchor, sizeof(row->receipt_head));
        memcpy(row->origin_station, item->source_hash, sizeof(row->origin_station));
        memcpy(row->latest_station, item->witness_hash, sizeof(row->latest_station));
    }
}

static bool local_hash32_nonzero(const uint8_t hash[32]) {
    if (!hash) return false;
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return true;
    }
    return false;
}

static bool local_hash32_equal(const uint8_t a[32], const uint8_t b[32]) {
    if (!local_hash32_nonzero(a) || !local_hash32_nonzero(b)) return false;
    return memcmp(a, b, 32) == 0;
}

static bool local_inspect_item_matches_job_proof(const knowledge_item_t *item,
                                                 const npc_ship_t *npc,
                                                 int job_idx) {
    if (!item || !npc || job_idx < 0 || job_idx >= npc->job_diag_count)
        return false;
    const uint8_t *proof = npc->job_diag_proof_hash[job_idx];
    if (!local_hash32_nonzero(proof)) return false;
    return local_hash32_equal(proof, item->chain_anchor) ||
           local_hash32_equal(proof, item->witness_hash) ||
           local_hash32_equal(proof, item->subject_hash);
}

static void local_server_fill_job_source_diagnostics(
    NetInspectSnapshot *snap,
    const knowledge_view_t *knowledge,
    const npc_ship_t *npc,
    uint8_t emitted[KNOWLEDGE_VIEW_MAX_CAP]) {
    if (!snap || !knowledge || !npc || !emitted) return;
    int item_cap = knowledge->count;
    if (item_cap > KNOWLEDGE_VIEW_MAX_CAP) item_cap = KNOWLEDGE_VIEW_MAX_CAP;
    int job_count = npc->job_diag_count;
    int job_cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
    if (job_count > job_cap) job_count = job_cap;
    for (int j = 0; j < job_count &&
                    snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; j++) {
        if (npc->job_diag_kind[j] == (uint8_t)INSPECT_DIAG_NONE) continue;
        for (int i = 0; i < item_cap &&
                        snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
            if (emitted[i]) continue;
            market_memory_t memory;
            if (!local_inspect_market_memory_from_item(&knowledge->items[i],
                                                       &memory)) {
                continue;
            }
            if (!local_inspect_item_matches_job_proof(&knowledge->items[i],
                                                      npc, j)) {
                continue;
            }
            local_server_copy_inspect_market_diag(
                &snap->rows[snap->row_count], &memory, &knowledge->items[i]);
            emitted[i] = 1;
            snap->row_count++;
            break;
        }
    }
}

static bool local_inspect_chain_matches_job_proof(const cargo_receipt_chain_t *chain,
                                                  const npc_ship_t *npc) {
    if (!chain || chain->len == 0 || chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return false;
    if (!npc) return false;
    uint8_t head[32];
    cargo_receipt_hash(&chain->links[chain->len - 1], head);
    int job_count = npc->job_diag_count;
    int job_cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
    if (job_count > job_cap) job_count = job_cap;
    for (int i = 0; i < job_count; i++) {
        if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_NONE) continue;
        if (local_hash32_equal(head, npc->job_diag_proof_hash[i]))
            return true;
    }
    return false;
}

static void local_server_copy_inspect_receipt_link(NetInspectSnapshotRow *row,
                                                   const cargo_receipt_t *receipt,
                                                   uint8_t link_idx,
                                                   uint8_t link_count) {
    memset(row, 0, sizeof(*row));
    if (!receipt) return;
    row->commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    row->grade = link_idx;
    row->chain_len = link_count;
    row->flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
    row->event_id = receipt->event_id;
    row->quantity = link_idx;
    memcpy(row->cargo_pub, receipt->cargo_pub, sizeof(row->cargo_pub));
    cargo_receipt_hash(receipt, row->receipt_head);
    memcpy(row->origin_station, receipt->authoring_station,
           sizeof(row->origin_station));
    memcpy(row->latest_station, receipt->recipient_pubkey,
           sizeof(row->latest_station));
}

static void local_server_fill_matching_receipt_diagnostics(
    NetInspectSnapshot *snap,
    const ship_t *ship,
    const ship_receipts_t *rcpts,
    const npc_ship_t *npc) {
    if (!snap || !ship || !ship->manifest.units || !rcpts || !npc) return;
    for (uint16_t i = 0; i < ship->manifest.count &&
                         snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
        const cargo_unit_t *unit = &ship->manifest.units[i];
        if (inspect_snapshot_unit_is_groupable(unit)) continue;
        const cargo_receipt_chain_t *chain =
            (i < rcpts->count) ? &rcpts->chains[i] : NULL;
        if (!local_inspect_chain_matches_job_proof(chain, npc)) continue;
        local_server_copy_inspect_row(&snap->rows[snap->row_count],
                                      unit, chain);
        snap->row_count++;
        for (uint8_t link = 0; link < chain->len &&
                               snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS;
             link++) {
            local_server_copy_inspect_receipt_link(
                &snap->rows[snap->row_count], &chain->links[link],
                (uint8_t)(link + 1), chain->len);
            snap->row_count++;
        }
    }
}

static void local_server_fill_matching_station_receipt_diagnostics(
    NetInspectSnapshot *snap,
    const station_t *stations,
    int station_count,
    const npc_ship_t *npc) {
    if (!snap || !stations || station_count <= 0 || !npc) return;
    for (int st = 0; st < station_count &&
                    snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; st++) {
        const station_t *station = &stations[st];
        if (!station->manifest.units) continue;
        const ship_receipts_t *rcpts = station_get_receipts_const(station);
        if (!rcpts) continue;
        for (uint16_t i = 0; i < station->manifest.count &&
                             snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
            const cargo_unit_t *unit = &station->manifest.units[i];
            if (inspect_snapshot_unit_is_groupable(unit)) continue;
            const cargo_receipt_chain_t *chain =
                (i < rcpts->count) ? &rcpts->chains[i] : NULL;
            if (!local_inspect_chain_matches_job_proof(chain, npc)) continue;
            local_server_copy_inspect_row(&snap->rows[snap->row_count],
                                          unit, chain);
            snap->rows[snap->row_count].flags |= INSPECT_ROW_STATION_RECEIPT;
            snap->row_count++;
            for (uint8_t link = 0; link < chain->len &&
                                   snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS;
                 link++) {
                local_server_copy_inspect_receipt_link(
                    &snap->rows[snap->row_count], &chain->links[link],
                    (uint8_t)(link + 1), chain->len);
                snap->row_count++;
            }
            return;
        }
    }
}

static void local_server_fill_inspect_diagnostics(NetInspectSnapshot *snap,
                                                  const knowledge_view_t *knowledge,
                                                  const npc_ship_t *npc) {
    if (!snap) return;
    if (npc) {
        int count = npc->job_diag_count;
        int cap = (int)(sizeof(npc->job_diag_kind) / sizeof(npc->job_diag_kind[0]));
        if (count > cap) count = cap;
        for (int i = 0; i < count && snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS &&
             snap->row_count < 4; i++) {
            if (npc->job_diag_kind[i] == (uint8_t)INSPECT_DIAG_NONE) continue;
            NetInspectSnapshotRow *row = &snap->rows[snap->row_count];
            memset(row, 0, sizeof(*row));
            row->commodity = npc->job_diag_kind[i];
            row->grade = npc->job_diag_score[i];
            row->chain_len = npc->job_diag_selected[i];
            row->flags = INSPECT_ROW_DIAGNOSTIC;
            row->event_id = (uint64_t)npc->job_diag_source[i]
                          | ((uint64_t)npc->job_diag_dest[i] << 8)
                          | ((uint64_t)npc->job_diag_kind[i] << 16)
                          | ((uint64_t)npc->job_diag_commodity[i] << 24);
            row->quantity = npc->job_diag_hint[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_VALUE] = npc->job_diag_factor_value[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_DEMAND] = npc->job_diag_factor_demand[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_SUPPLY] = npc->job_diag_factor_supply[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_ROUTE] = npc->job_diag_factor_route[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_FRESHNESS] = npc->job_diag_factor_freshness[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_CAPABILITY] = npc->job_diag_factor_capability[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_PROOF] = npc->job_diag_factor_proof[i];
            row->cargo_pub[INSPECT_JOB_FACTOR_HOLOGRAM] = npc->job_diag_factor_hologram[i];
            row->cargo_pub[INSPECT_JOB_META_REASON] = npc->job_diag_reason[i];
            row->cargo_pub[INSPECT_JOB_META_MEMORY_KIND] = npc->job_diag_memory_kind[i];
            row->cargo_pub[INSPECT_JOB_META_HOPS] = npc->job_diag_memory_hops[i];
            row->cargo_pub[INSPECT_JOB_META_AGE] = npc->job_diag_memory_age[i];
            row->cargo_pub[INSPECT_JOB_META_SOURCE_STATION] = npc->job_diag_memory_station[i];
            row->cargo_pub[INSPECT_JOB_META_PROOF_KIND] = npc->job_diag_proof_kind[i];
            row->cargo_pub[INSPECT_JOB_META_PROOF0] = npc->job_diag_proof_prefix[i][0];
            row->cargo_pub[INSPECT_JOB_META_PROOF1] = npc->job_diag_proof_prefix[i][1];
            row->cargo_pub[INSPECT_JOB_META_PROOF2] = npc->job_diag_proof_prefix[i][2];
            row->cargo_pub[INSPECT_JOB_META_PROOF3] = npc->job_diag_proof_prefix[i][3];
            memcpy(row->receipt_head, npc->job_diag_proof_hash[i],
                   sizeof(row->receipt_head));
            snap->row_count++;
        }
    }
    uint8_t emitted_market_rows[KNOWLEDGE_VIEW_MAX_CAP] = {0};
    local_server_fill_job_source_diagnostics(snap, knowledge, npc,
                                             emitted_market_rows);
    if (knowledge) {
        int cap = knowledge->count;
        if (cap > KNOWLEDGE_VIEW_MAX_CAP) cap = KNOWLEDGE_VIEW_MAX_CAP;
        for (int i = 0; i < cap && snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS &&
             snap->row_count < 4; i++) {
            if (emitted_market_rows[i]) continue;
            market_memory_t memory;
            if (!local_inspect_market_memory_from_item(&knowledge->items[i], &memory))
                continue;
            local_server_copy_inspect_market_diag(&snap->rows[snap->row_count],
                                                  &memory, &knowledge->items[i]);
            snap->row_count++;
        }
    }
}

static void local_server_fill_inspect_manifest(NetInspectSnapshot *snap,
                                               const ship_t *ship,
                                               const npc_ship_t *npc,
                                               const station_t *stations,
                                               int station_count) {
    if (!snap || !ship) return;

    snap->manifest_count = ship->manifest.units ? ship->manifest.count : 0;
    const ship_receipts_t *rcpts = ship_get_receipts_const(ship);
    uint8_t receipt_row_start = snap->row_count;
    local_server_fill_matching_receipt_diagnostics(snap, ship, rcpts, npc);
    if (snap->row_count == receipt_row_start) {
        local_server_fill_matching_station_receipt_diagnostics(
            snap, stations, station_count, npc);
    }
    uint16_t bulk[COMMODITY_COUNT][MINING_GRADE_COUNT];
    memset(bulk, 0, sizeof(bulk));
    for (uint16_t i = 0; i < snap->manifest_count; i++) {
        const cargo_unit_t *unit = &ship->manifest.units[i];
        if (inspect_snapshot_unit_is_groupable(unit)) {
            if (bulk[unit->commodity][unit->grade] < 0xFFFF)
                bulk[unit->commodity][unit->grade]++;
        }
    }
    for (int gr = 0; gr < MINING_GRADE_COUNT && snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; gr++) {
        for (int c = 0; c < COMMODITY_COUNT && snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; c++) {
            if (bulk[c][gr] > 0) {
                local_server_copy_inspect_group(&snap->rows[snap->row_count],
                                                (uint8_t)c, (uint8_t)gr,
                                                bulk[c][gr],
                                                (uint8_t)INGOT_PREFIX_ANONYMOUS);
                snap->row_count++;
            }
            for (uint16_t i = 0; i < snap->manifest_count &&
                 snap->row_count < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
                const cargo_unit_t *unit = &ship->manifest.units[i];
                if (unit->commodity != c || unit->grade != gr) continue;
                if (inspect_snapshot_unit_is_groupable(unit)) continue;
                const cargo_receipt_chain_t *chain =
                    (rcpts && i < rcpts->count) ? &rcpts->chains[i] : NULL;
                if (local_inspect_chain_matches_job_proof(chain, npc))
                    continue;
                local_server_copy_inspect_row(&snap->rows[snap->row_count],
                                              unit, chain);
                snap->row_count++;
            }
        }
    }
}

static void local_server_sync_inspect_snapshot(const local_server_t *ls,
                                               const server_player_t *src) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_index = 0xFFu;
    snap.module_index = 0xFFu;
    snap.home_station = 0xFFu;
    snap.dest_station = 0xFFu;

    if (!src->scan_active || src->scan_target_type == INSPECT_TARGET_NONE) {
        /* Linger: bump the timer once on the active→idle edge and
         * leave it alone on subsequent idle frames so it can decay.
         * The earlier `timer ≤ 0.60` trick silently re-fired ~2.9s
         * in when the timer crossed back below 0.60 on its way down,
         * trapping the panel/ring on screen forever. */
        if (g.inspect_was_active) {
            g.inspect_snapshot_timer = 3.5f;
            g.inspect_was_active = false;
        }
        return;
    }

    snap.target_type = (uint8_t)src->scan_target_type;
    snap.target_index = (src->scan_target_index >= 0)
        ? (uint8_t)src->scan_target_index : 0xFFu;
    snap.module_index = (src->scan_module_index >= 0)
        ? (uint8_t)src->scan_module_index : 0xFFu;

    const ship_t *inspect_ship = NULL;
    const npc_ship_t *inspect_npc = NULL;
    if (src->scan_target_type == INSPECT_TARGET_NPC &&
        src->scan_target_index >= 0 &&
        src->scan_target_index < MAX_NPC_SHIPS) {
        const npc_ship_t *npc = &ls->world.npc_ships[src->scan_target_index];
        ship_t *ship = world_npc_ship_for((world_t *)&ls->world, src->scan_target_index);
        if (npc->active && ship) {
            snap.role = (uint8_t)npc->role;
            snap.state = (uint8_t)npc->state;
            snap.home_station = (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
                ? (uint8_t)npc->home_station : 0xFFu;
            snap.dest_station = (npc->dest_station >= 0 && npc->dest_station < MAX_STATIONS)
                ? (uint8_t)npc->dest_station : 0xFFu;
            local_server_fill_inspect_diagnostics(&snap, &npc->knowledge, npc);
            inspect_ship = ship;
            inspect_npc = npc;
        }
    } else if (src->scan_target_type == INSPECT_TARGET_PLAYER &&
               src->scan_target_index >= 0 &&
               src->scan_target_index < MAX_PLAYERS) {
        const server_player_t *target = &ls->world.players[src->scan_target_index];
        if (target->connected) {
            snap.role = (uint8_t)target->ship.hull_class;
            float rounded_hull = target->ship.hull + 0.5f;
            if (rounded_hull < 0.0f) rounded_hull = 0.0f;
            if (rounded_hull > 255.0f) rounded_hull = 255.0f;
            snap.state = (uint8_t)rounded_hull;
            snap.home_station =
                (target->current_station >= 0 && target->current_station < MAX_STATIONS)
                ? (uint8_t)target->current_station : 0xFFu;
            snap.dest_station =
                (target->nearby_station >= 0 && target->nearby_station < MAX_STATIONS)
                ? (uint8_t)target->nearby_station : snap.home_station;
            inspect_ship = &target->ship;
        }
    }
    if (inspect_ship)
        local_server_fill_inspect_manifest(&snap, inspect_ship, inspect_npc,
                                           ls->world.stations, MAX_STATIONS);

    if (g.inspect_snapshot.target_type != snap.target_type ||
        g.inspect_snapshot.target_index != snap.target_index ||
        g.inspect_snapshot.module_index != snap.module_index) {
        g.inspect_receipt_page = 0;
        g.inspect_receipt_browser = false;
    }
    g.inspect_snapshot = snap;
    g.inspect_snapshot_timer = 0.60f;
    g.inspect_was_active = true;
}

void local_server_sync_to_client(const local_server_t *ls) {
    if (!ls->active) return;
    mirror_whole_world(&ls->world);

    server_player_t *dst = &g.world.players[g.local_player_slot];
    const server_player_t *src = &ls->world.players[g.local_player_slot];
    mirror_player_always(dst, src);
    /* Server-authoritative thrust flag — drives flames in autopilot. */
    g.server_thrusting = src->actual_thrusting;
    /* Mirror autopilot path for dotted-line preview. */
    g.autopilot_path_count = nav_get_player_path(
        g.local_player_slot, g.autopilot_path, 12, &g.autopilot_path_current);
    local_server_sync_inspect_snapshot(ls, src);
    if (g.action_predict_timer <= 0.0f)
        mirror_player_predicted(dst, src);
}
