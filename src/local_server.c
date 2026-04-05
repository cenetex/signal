/*
 * local_server.c -- In-process authoritative simulation for singleplayer.
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

void local_server_sync_to_client(const local_server_t *ls) {
    if (!ls->active) return;

    /* Asteroids: copy authoritative state directly into the client world.
     * Interp buffers are skipped — interpolate_world_for_render() returns
     * early in singleplayer mode, so these copies were wasted work. */
    memcpy(g.world.asteroids, ls->world.asteroids, sizeof(g.world.asteroids));

    /* NPCs: same — skip interp copies */
    memcpy(g.world.npc_ships, ls->world.npc_ships, sizeof(g.world.npc_ships));

    /* Stations: direct copy (no interpolation) */
    memcpy(g.world.stations, ls->world.stations, sizeof(g.world.stations));

    /* Contracts: direct copy */
    memcpy(g.world.contracts, ls->world.contracts, sizeof(g.world.contracts));

    /* Player state: copy authoritative ship data unless client is
     * predicting a one-shot action (action_predict_timer). */
    const server_player_t *src = &ls->world.players[g.local_player_slot];
    server_player_t *dst = &g.world.players[g.local_player_slot];

    /* Player position/velocity/angle — authoritative, always sync */
    dst->ship.pos = src->ship.pos;
    dst->ship.vel = src->ship.vel;
    dst->ship.angle = src->ship.angle;

    if (g.action_predict_timer <= 0.0f) {
        dst->ship.hull = src->ship.hull;
        dst->ship.credits = src->ship.credits;
        dst->ship.mining_level = src->ship.mining_level;
        dst->ship.hold_level = src->ship.hold_level;
        dst->ship.tractor_level = src->ship.tractor_level;
        dst->ship.has_scaffold_kit = src->ship.has_scaffold_kit;
        dst->ship.scaffold_kit_type = src->ship.scaffold_kit_type;
        memcpy(dst->ship.cargo, src->ship.cargo, sizeof(dst->ship.cargo));
    }

    /* Dock state */
    dst->docked = src->docked;
    dst->current_station = src->current_station;
    dst->in_dock_range = src->in_dock_range;
    dst->nearby_station = src->nearby_station;
    dst->dock_berth = src->dock_berth;
    dst->docking_approach = src->docking_approach;
    dst->ship.tractor_active = src->ship.tractor_active;

    /* Beam/targeting state (for rendering) */
    dst->beam_active = src->beam_active;
    dst->beam_hit = src->beam_hit;
    dst->beam_ineffective = src->beam_ineffective;
    dst->beam_start = src->beam_start;
    dst->beam_end = src->beam_end;
    dst->hover_asteroid = src->hover_asteroid;
    dst->tractor_fragments = src->tractor_fragments;
    dst->nearby_fragments = src->nearby_fragments;

    /* Tow state (for tether rendering) */
    dst->ship.towed_count = src->ship.towed_count;
    memcpy(dst->ship.towed_fragments, src->ship.towed_fragments, sizeof(dst->ship.towed_fragments));

    /* Sim events: copy so process_sim_events can read from g.world.events
     * in addition to the local_server path — keeps both sources consistent. */
    g.world.events = ls->world.events;

    /* World time */
    g.world.time = ls->world.time;
}
