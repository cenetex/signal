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
    memcpy(g.world.asteroids, src->asteroids, sizeof(g.world.asteroids));
    memcpy(g.world.npc_ships, src->npc_ships, sizeof(g.world.npc_ships));
    for (int i = 0; i < MAX_STATIONS; i++)
        (void)station_copy(&g.world.stations[i], &src->stations[i]);
    memcpy(g.world.contracts, src->contracts, sizeof(g.world.contracts));
    memcpy(g.world.scaffolds, src->scaffolds, sizeof(g.world.scaffolds));
    g.world.events = src->events;
    g.world.time   = src->time;
}

/* (2a) Local player ship — always-sync fields (no client optimism). */
static void mirror_player_always(server_player_t *dst, const server_player_t *src) {
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

static bool local_inspect_unit_is_groupable_bulk(const cargo_unit_t *unit) {
    if (!unit) return false;
    if ((cargo_kind_t)unit->kind != CARGO_KIND_INGOT) return false;
    if ((ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS) return false;
    if (unit->commodity >= COMMODITY_COUNT) return false;
    if (unit->grade >= MINING_GRADE_COUNT) return false;
    return true;
}

static void local_server_copy_inspect_group(NetInspectSnapshotRow *row,
                                            uint8_t commodity,
                                            uint8_t grade,
                                            uint16_t quantity) {
    memset(row, 0, sizeof(*row));
    row->commodity = commodity;
    row->grade = grade;
    row->quantity = quantity > 0 ? quantity : 1;
    row->flags |= INSPECT_ROW_GROUPED;
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
        g.inspect_snapshot = snap;
        g.inspect_snapshot_timer = 0.0f;
        return;
    }

    snap.target_type = (uint8_t)src->scan_target_type;
    snap.target_index = (src->scan_target_index >= 0)
        ? (uint8_t)src->scan_target_index : 0xFFu;
    snap.module_index = (src->scan_module_index >= 0)
        ? (uint8_t)src->scan_module_index : 0xFFu;

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
            snap.manifest_count = ship->manifest.units ? ship->manifest.count : 0;
            const ship_receipts_t *rcpts = ship_get_receipts_const(ship);
            uint16_t bulk[COMMODITY_COUNT][MINING_GRADE_COUNT];
            memset(bulk, 0, sizeof(bulk));
            for (uint16_t i = 0; i < snap.manifest_count; i++) {
                const cargo_unit_t *unit = &ship->manifest.units[i];
                if (!local_inspect_unit_is_groupable_bulk(unit)) continue;
                if (bulk[unit->commodity][unit->grade] < 0xFFFF)
                    bulk[unit->commodity][unit->grade]++;
            }
            snap.row_count = 0;
            for (int gr = 0; gr < MINING_GRADE_COUNT && snap.row_count < INSPECT_SNAPSHOT_MAX_ROWS; gr++) {
                for (int c = 0; c < COMMODITY_COUNT && snap.row_count < INSPECT_SNAPSHOT_MAX_ROWS; c++) {
                    if (bulk[c][gr] > 0) {
                        local_server_copy_inspect_group(&snap.rows[snap.row_count],
                                                        (uint8_t)c, (uint8_t)gr,
                                                        bulk[c][gr]);
                        snap.row_count++;
                    }
                    for (uint16_t i = 0; i < snap.manifest_count &&
                         snap.row_count < INSPECT_SNAPSHOT_MAX_ROWS; i++) {
                        const cargo_unit_t *unit = &ship->manifest.units[i];
                        if (unit->commodity != c || unit->grade != gr) continue;
                        if (local_inspect_unit_is_groupable_bulk(unit)) continue;
                        const cargo_receipt_chain_t *chain =
                            (rcpts && i < rcpts->count) ? &rcpts->chains[i] : NULL;
                        local_server_copy_inspect_row(&snap.rows[snap.row_count],
                                                      unit, chain);
                        snap.row_count++;
                    }
                }
            }
        }
    }

    g.inspect_snapshot = snap;
    g.inspect_snapshot_timer = 0.60f;
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
