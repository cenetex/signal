/*
 * net_sync.c -- Multiplayer network state synchronization for the
 * Signal Space Miner client.
 */
#include "net_sync.h"
#include "input.h"   /* set_notice() */

void on_player_join(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    g.world.players[player_id].connected = true;
    g.world.players[player_id].id = player_id;
    if ((int)player_id != g.local_player_slot)
        set_notice("Player %d joined.", (int)player_id);
}

void on_player_leave(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    g.world.players[player_id].connected = false;
    if ((int)player_id != g.local_player_slot)
        set_notice("Player %d left.", (int)player_id);
}

void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {
    /* Shift current -> previous for interpolation */
    memcpy(g.asteroid_interp.prev, g.asteroid_interp.curr, sizeof(g.asteroid_interp.prev));
    g.asteroid_interp.interval = fmaxf(g.asteroid_interp.t * g.asteroid_interp.interval, 0.05f);
    if (g.asteroid_interp.interval > 0.2f) g.asteroid_interp.interval = 0.1f;
    g.asteroid_interp.t = 0.0f;

    bool received[MAX_ASTEROIDS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;
        received[idx] = true;

        asteroid_t* a = &g.asteroid_interp.curr[idx];
        a->active = (asteroids[i].flags & 1) != 0;
        a->fracture_child = (asteroids[i].flags & (1 << 1)) != 0;
        a->tier = (asteroid_tier_t)((asteroids[i].flags >> 2) & 0x7);
        a->commodity = (commodity_t)((asteroids[i].flags >> 5) & 0x7);
        a->pos.x = asteroids[i].x;
        a->pos.y = asteroids[i].y;
        a->vel.x = asteroids[i].vx;
        a->vel.y = asteroids[i].vy;
        a->hp    = asteroids[i].hp;
        a->ore   = asteroids[i].ore;
        a->radius = asteroids[i].radius;
        if (a->max_hp < a->hp) a->max_hp = a->hp;
        if (a->max_ore < a->ore) a->max_ore = a->ore;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!received[i] && g.asteroid_interp.curr[i].active) {
            /* Not in this delta — extrapolate position from velocity.
             * Shift prev to current extrapolated position for smooth interp. */
            g.asteroid_interp.prev[i] = g.asteroid_interp.curr[i];
            g.asteroid_interp.curr[i].pos.x += g.asteroid_interp.curr[i].vel.x * g.asteroid_interp.interval;
            g.asteroid_interp.curr[i].pos.y += g.asteroid_interp.curr[i].vel.y * g.asteroid_interp.interval;
        }
    }

    /* Also copy to world for game logic (targeting, beam hit checks) */
    memcpy(g.world.asteroids, g.asteroid_interp.curr, sizeof(g.world.asteroids));
}

void apply_remote_npcs(const NetNpcState* npcs, int count) {
    memcpy(g.npc_interp.prev, g.npc_interp.curr, sizeof(g.npc_interp.prev));
    g.npc_interp.interval = fmaxf(g.npc_interp.t * g.npc_interp.interval, 0.05f);
    if (g.npc_interp.interval > 0.2f) g.npc_interp.interval = 0.1f;
    g.npc_interp.t = 0.0f;

    bool received[MAX_NPC_SHIPS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;
        received[idx] = true;

        npc_ship_t* n = &g.npc_interp.curr[idx];
        n->active = (npcs[i].flags & 1) != 0;
        n->role = (npc_role_t)((npcs[i].flags >> 1) & 0x3);
        n->state = (npc_state_t)((npcs[i].flags >> 3) & 0x7);
        n->thrusting = (npcs[i].flags & (1 << 6)) != 0;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->vel.x = npcs[i].vx;
        n->vel.y = npcs[i].vy;
        n->angle = npcs[i].angle;
        n->target_asteroid = (int)npcs[i].target_asteroid;
        n->tint_r = (float)npcs[i].tint_r / 255.0f;
        n->tint_g = (float)npcs[i].tint_g / 255.0f;
        n->tint_b = (float)npcs[i].tint_b / 255.0f;
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!received[i]) {
            g.npc_interp.curr[i].active = false;
        }
    }

    memcpy(g.world.npc_ships, g.npc_interp.curr, sizeof(g.world.npc_ships));
}

void apply_remote_stations(uint8_t index, const float* inventory) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    for (int i = 0; i < COMMODITY_COUNT; i++)
        st->inventory[i] = inventory[i];
}

void apply_remote_contracts(const contract_t* contracts, int count) {
    /* Full replacement: clear all, then copy received */
    for (int i = 0; i < MAX_CONTRACTS; i++)
        g.world.contracts[i].active = false;
    for (int i = 0; i < count && i < MAX_CONTRACTS; i++)
        g.world.contracts[i] = contracts[i];
}

void apply_remote_station_identity(const NetStationIdentity* si) {
    if (si->index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[si->index];
    st->scaffold = (si->flags & 1) != 0;
    st->scaffold_progress = si->scaffold_progress;
    st->services = si->services;
    st->pos = v2(si->pos_x, si->pos_y);
    st->radius = si->radius;
    st->dock_radius = si->dock_radius;
    st->signal_range = si->signal_range;
    snprintf(st->name, sizeof(st->name), "%s", si->name);
    for (int c = 0; c < COMMODITY_COUNT; c++)
        st->buy_price[c] = si->buy_price[c];
    st->module_count = si->module_count;
    for (int m = 0; m < si->module_count && m < MAX_MODULES_PER_STATION; m++)
        st->modules[m] = si->modules[m];
}

void apply_remote_player_state(const NetPlayerState* state) {
    /* Reconcile local prediction with server-authoritative position. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    /* Position reconciliation: blend gently for small divergence,
     * snap for large divergence (e.g., server teleported us). */
    float dx = state->x - sp->ship.pos.x;
    float dy = state->y - sp->ship.pos.y;
    float dist_sq = dx * dx + dy * dy;
    if (dist_sq > 500.0f * 500.0f) {
        /* Large divergence: snap to server (server probably docked/teleported us) */
        sp->ship.pos.x = state->x;
        sp->ship.pos.y = state->y;
        sp->ship.vel.x = state->vx;
        sp->ship.vel.y = state->vy;
    } else {
        /* Small divergence: gentle blend */
        float t = dist_sq > 100.0f * 100.0f ? 0.15f : 0.08f;
        sp->ship.pos.x = lerpf(sp->ship.pos.x, state->x, t);
        sp->ship.pos.y = lerpf(sp->ship.pos.y, state->y, t);
        sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, t);
        sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, t);
    }
    sp->ship.angle = lerp_angle(sp->ship.angle, state->angle, 0.1f);
}

void apply_remote_player_ship(const NetPlayerShipState* state) {
    /* Apply server-authoritative ship state for the local player. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    /* While the action predict timer is active, the client has made an
     * optimistic change (buy/sell/upgrade/launch) that the server hasn't
     * confirmed yet.  Skip overwriting mutable ship state to prevent
     * flicker from stale PLAYER_SHIP messages. */
    if (g.action_predict_timer <= 0.0f) {
        sp->ship.hull = state->hull;
        sp->ship.credits = state->credits;
        sp->ship.mining_level = (int)state->mining_level;
        sp->ship.hold_level = (int)state->hold_level;
        sp->ship.tractor_level = (int)state->tractor_level;
        sp->ship.has_scaffold_kit = state->has_scaffold_kit;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            sp->ship.cargo[c] = state->cargo[c];
    }
    /* Dock-state reconciliation:
     * - Server says undocked -> always accept.
     * - Server says docked  -> only accept if we locally agree
     *   or the predict window has expired. */
    if (!state->docked) {
        sp->docked = false;
    } else if (sp->docked || g.action_predict_timer <= 0.0f) {
        sp->docked = true;
        sp->current_station = (int)state->current_station;
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
    }
}

void sync_local_player_slot_from_network(void) {
    uint8_t net_id = net_local_id();
    if (net_id == 0xFF || net_id >= MAX_PLAYERS) return;
    if (g.local_player_slot == (int)net_id) {
        LOCAL_PLAYER.connected = true;
        return;
    }

    server_player_t previous = g.world.players[g.local_player_slot];
    server_player_t* assigned = &g.world.players[net_id];
    memset(&g.world.players[g.local_player_slot], 0, sizeof(g.world.players[g.local_player_slot]));
    g.local_player_slot = (int)net_id;
    if (!assigned->connected && assigned->ship.hull <= 0.0f) {
        *assigned = previous;
    }
    LOCAL_PLAYER.id = net_id;
    LOCAL_PLAYER.connected = true;
    LOCAL_PLAYER.conn = NULL;
}

void interpolate_world_for_render(void) {
    float t = clampf(g.asteroid_interp.t, 0.0f, 1.2f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *dst = &g.world.asteroids[i];
        const asteroid_t *prev = &g.asteroid_interp.prev[i];
        const asteroid_t *curr = &g.asteroid_interp.curr[i];
        /* Use current state for everything except position */
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, t);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, t);
            dst->rotation = lerp_angle(prev->rotation, curr->rotation, t);
        }
    }

    float nt = clampf(g.npc_interp.t, 0.0f, 1.2f);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *dst = &g.world.npc_ships[i];
        const npc_ship_t *prev = &g.npc_interp.prev[i];
        const npc_ship_t *curr = &g.npc_interp.curr[i];
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, nt);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, nt);
            dst->angle = lerp_angle(prev->angle, curr->angle, nt);
        }
    }
}
