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

    /* Asteroids: shift curr -> prev, copy server state -> curr */
    memcpy(g.asteroid_interp.prev, g.asteroid_interp.curr,
           sizeof(g.asteroid_interp.prev));
    memcpy(g.asteroid_interp.curr, ls->world.asteroids,
           sizeof(g.asteroid_interp.curr));
    g.asteroid_interp.interval = SIM_DT;
    g.asteroid_interp.t = 0.0f;

    /* NPCs: same pattern */
    memcpy(g.npc_interp.prev, g.npc_interp.curr,
           sizeof(g.npc_interp.prev));
    memcpy(g.npc_interp.curr, ls->world.npc_ships,
           sizeof(g.npc_interp.curr));
    g.npc_interp.interval = SIM_DT;
    g.npc_interp.t = 0.0f;

    /* Stations: direct copy (no interpolation) */
    memcpy(g.world.stations, ls->world.stations, sizeof(g.world.stations));

    /* Contracts: direct copy */
    memcpy(g.world.contracts, ls->world.contracts, sizeof(g.world.contracts));

    /* Player state: copy authoritative ship data unless client is
     * predicting a one-shot action (action_predict_timer). */
    const server_player_t *src = &ls->world.players[g.local_player_slot];
    server_player_t *dst = &g.world.players[g.local_player_slot];

    if (g.action_predict_timer <= 0.0f) {
        dst->ship.hull = src->ship.hull;
        dst->ship.credits = src->ship.credits;
        dst->ship.mining_level = src->ship.mining_level;
        dst->ship.hold_level = src->ship.hold_level;
        dst->ship.tractor_level = src->ship.tractor_level;
        dst->ship.has_scaffold_kit = src->ship.has_scaffold_kit;
        memcpy(dst->ship.cargo, src->ship.cargo, sizeof(dst->ship.cargo));
    }

    /* Dock state */
    dst->docked = src->docked;
    dst->current_station = src->current_station;
    dst->in_dock_range = src->in_dock_range;
    dst->nearby_station = src->nearby_station;

    /* Beam/targeting state (for rendering) */
    dst->beam_active = src->beam_active;
    dst->beam_hit = src->beam_hit;
    dst->beam_ineffective = src->beam_ineffective;
    dst->beam_start = src->beam_start;
    dst->beam_end = src->beam_end;
    dst->hover_asteroid = src->hover_asteroid;
    dst->tractor_fragments = src->tractor_fragments;
    dst->nearby_fragments = src->nearby_fragments;

    /* World time */
    g.world.time = ls->world.time;
}
