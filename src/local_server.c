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

void local_server_init(local_server_t *ls, uint32_t seed) {
    memset(ls, 0, sizeof(*ls));
    ls->world.rng = seed ? seed : 2037u;
    world_reset(&ls->world);
    ls->world.players[0].connected = true;
    ls->world.players[0].id = 0;
    player_init_ship(&ls->world.players[0], &ls->world);
    ls->active = true;
}

void local_server_step(local_server_t *ls, int player_slot,
                        const input_intent_t *input, float dt) {
    if (!ls->active) return;
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return;
    ls->world.players[player_slot].input = *input;
    world_sim_step(&ls->world, dt);
}

/* (1) Whole-world arrays. Add new world_t arrays here as one line. */
static void mirror_whole_world(const world_t *src) {
    memcpy(g.world.asteroids, src->asteroids, sizeof(g.world.asteroids));
    memcpy(g.world.npc_ships, src->npc_ships, sizeof(g.world.npc_ships));
    memcpy(g.world.stations,  src->stations,  sizeof(g.world.stations));
    memcpy(g.world.contracts, src->contracts, sizeof(g.world.contracts));
    memcpy(g.world.scaffolds, src->scaffolds, sizeof(g.world.scaffolds));
    g.world.events = src->events;
    g.world.time   = src->time;
}

/* (2a) Local player ship — always-sync fields (no client optimism). */
static void mirror_player_always(server_player_t *dst, const server_player_t *src) {
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
    dst->ship.credits       = src->ship.credits;
    dst->ship.mining_level  = src->ship.mining_level;
    dst->ship.hold_level    = src->ship.hold_level;
    dst->ship.tractor_level = src->ship.tractor_level;
    memcpy(dst->ship.cargo, src->ship.cargo, sizeof(dst->ship.cargo));
}

void local_server_sync_to_client(const local_server_t *ls) {
    if (!ls->active) return;
    mirror_whole_world(&ls->world);

    server_player_t *dst = &g.world.players[g.local_player_slot];
    const server_player_t *src = &ls->world.players[g.local_player_slot];
    mirror_player_always(dst, src);
    if (g.action_predict_timer <= 0.0f)
        mirror_player_predicted(dst, src);
}
