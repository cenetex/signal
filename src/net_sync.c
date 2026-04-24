/*
 * net_sync.c -- Multiplayer network state synchronization for the
 * Signal Space Miner client.
 */
#include <stdlib.h>  /* rand, RAND_MAX */
#include "net_sync.h"
#include "input.h"   /* set_notice() */
#include "manifest.h"
#include "onboarding.h"
#include "episode.h"

static void server_player_cleanup_local(server_player_t *sp) {
    if (!sp) return;
    ship_cleanup(&sp->ship);
}

static bool server_player_copy_local(server_player_t *dst, const server_player_t *src) {
    ship_t cloned_ship = {0};

    if (!dst || !src) return false;
    if (dst == src) return true;
    if (!ship_copy(&cloned_ship, &src->ship)) return false;
    server_player_cleanup_local(dst);
    *dst = *src;
    dst->ship = cloned_ship;
    return true;
}

void on_player_join(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    g.world.players[player_id].connected = true;
    g.world.players[player_id].id = player_id;
    /* Don't show join notice here — callsign hasn't arrived yet.
     * We detect new players in apply_remote_player_state instead. */
    (void)0;
}

void on_player_leave(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    const NetPlayerState *ps = &net_get_players()[player_id];
    if ((int)player_id != g.local_player_slot) {
        if (ps->callsign[0])
            set_notice("%s left.", ps->callsign);
        else
            set_notice("Pilot left.");
    }
    g.world.players[player_id].connected = false;
}

void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {
    /* Shift current -> previous for interpolation.
     * Compute interval as blend of measured elapsed and previous interval
     * to smooth out network jitter. */
    memcpy(g.asteroid_interp.prev, g.asteroid_interp.curr, sizeof(g.asteroid_interp.prev));
    float elapsed = g.asteroid_interp.t * g.asteroid_interp.interval;
    elapsed = clampf(elapsed, 0.05f, 0.2f);
    g.asteroid_interp.interval = lerpf(g.asteroid_interp.interval, elapsed, 0.3f);
    g.asteroid_interp.t = 0.0f;

    bool received[MAX_ASTEROIDS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint16_t idx = asteroids[i].index;
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
        a->smelt_progress = asteroids[i].smelt_progress;
        a->grade = asteroids[i].grade;
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

    /* World asteroids are updated by interpolate_world_for_render() at
     * render time, ensuring game logic and rendering see the same positions. */
}

void apply_remote_npcs(const NetNpcState* npcs, int count) {
    memcpy(g.npc_interp.prev, g.npc_interp.curr, sizeof(g.npc_interp.prev));
    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, npc_elapsed, 0.3f);
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

    /* World NPCs updated by interpolate_world_for_render(). */
}

void apply_remote_stations(uint8_t index, const float* inventory, float credit_pool) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    for (int i = 0; i < COMMODITY_COUNT; i++)
        st->inventory[i] = inventory[i];
    st->credit_pool = credit_pool;
}

void apply_remote_station_ingots(uint8_t station_id,
                                 const named_ingot_t *ingots, int count) {
    if (station_id >= MAX_STATIONS) return;
    if (count < 0) count = 0;
    if (count > STATION_NAMED_INGOTS_MAX) count = STATION_NAMED_INGOTS_MAX;
    station_t *st = &g.world.stations[station_id];
    /* Full replacement — server is the source of truth. */
    memset(st->named_ingots, 0, sizeof(st->named_ingots));
    for (int i = 0; i < count; i++) st->named_ingots[i] = ingots[i];
    st->named_ingots_count = count;
}

void apply_remote_hold_ingots(const named_ingot_t *ingots, int count) {
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    if (count < 0) count = 0;
    if (count > SHIP_HOLD_INGOTS_MAX) count = SHIP_HOLD_INGOTS_MAX;
    ship_t *ship = &g.world.players[g.local_player_slot].ship;
    memset(ship->hold_ingots, 0, sizeof(ship->hold_ingots));
    for (int i = 0; i < count; i++) ship->hold_ingots[i] = ingots[i];
    ship->hold_ingots_count = count;
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
    st->planned  = (si->flags & 2) != 0;
    st->scaffold_progress = si->scaffold_progress;
    st->services = si->services;
    st->pos = v2(si->pos_x, si->pos_y);
    st->radius = si->radius;
    st->dock_radius = si->dock_radius;
    st->signal_range = si->signal_range;
    snprintf(st->name, sizeof(st->name), "%s", si->name);
    for (int c = 0; c < COMMODITY_COUNT; c++)
        st->base_price[c] = si->base_price[c];
    st->module_count = si->module_count;
    for (int m = 0; m < si->module_count && m < MAX_MODULES_PER_STATION; m++)
        st->modules[m] = si->modules[m];
    st->arm_count = si->arm_count;
    for (int a = 0; a < MAX_ARMS; a++) {
        st->arm_speed[a] = si->arm_speed[a];
        st->ring_offset[a] = si->ring_offset[a];
    }
    /* Placement plans (faction-shared blueprint slots) */
    st->placement_plan_count = si->plan_count;
    for (int p = 0; p < si->plan_count && p < 8; p++) {
        st->placement_plans[p].type  = si->plans[p].type;
        st->placement_plans[p].ring  = si->plans[p].ring;
        st->placement_plans[p].slot  = si->plans[p].slot;
        st->placement_plans[p].owner = si->plans[p].owner;
    }
    /* Pending shipyard orders — head-of-queue first */
    st->pending_scaffold_count = si->pending_scaffold_count;
    if (st->pending_scaffold_count > 4) st->pending_scaffold_count = 4;
    for (int p = 0; p < st->pending_scaffold_count; p++) {
        st->pending_scaffolds[p].type  = si->pending_scaffolds[p].type;
        st->pending_scaffolds[p].owner = si->pending_scaffolds[p].owner;
    }
    snprintf(st->currency_name, sizeof(st->currency_name), "%s", si->currency_name);
}

void apply_remote_scaffolds(const NetScaffoldState* received, int count) {
    /* Server sends a snapshot of every active scaffold each tick. Anything
     * not in the snapshot is gone — clear locally so the SHIPYARD UI and
     * tow targeting reflect server truth. */
    bool seen[MAX_SCAFFOLDS] = { false };
    for (int i = 0; i < count; i++) {
        uint8_t idx = received[i].index;
        if (idx >= MAX_SCAFFOLDS) continue;
        scaffold_t *sc = &g.world.scaffolds[idx];
        sc->active = true;
        sc->state = (scaffold_state_t)received[i].state;
        sc->module_type = (module_type_t)received[i].module_type;
        sc->owner = received[i].owner;
        sc->pos = v2(received[i].pos_x, received[i].pos_y);
        sc->vel = v2(received[i].vel_x, received[i].vel_y);
        sc->radius = received[i].radius;
        sc->build_amount = received[i].build_amount;
        if (sc->state == SCAFFOLD_NASCENT) {
            /* Nascent scaffolds need built_at_station so the SHIPYARD UI
             * can match them. We don't network it explicitly; instead,
             * derive from nearest station while NASCENT. */
            float best_d = 1e18f;
            int best_s = -1;
            for (int s = 0; s < MAX_STATIONS; s++) {
                const station_t *st = &g.world.stations[s];
                if (!station_exists(st)) continue;
                float d = v2_dist_sq(sc->pos, st->pos);
                if (d < best_d) { best_d = d; best_s = s; }
            }
            sc->built_at_station = best_s;
        } else {
            sc->built_at_station = -1;
        }
        seen[idx] = true;
    }
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!seen[i]) g.world.scaffolds[i].active = false;
    }
}

/* Defined in main.c — process events for audio + UI */
extern void process_sim_events(const sim_events_t *events);

void apply_remote_events(const sim_event_t *events, int count) {
    /* Process immediately — Emscripten WebSocket callbacks fire async,
     * so we can't rely on g.world.events surviving until sim_step. */
    if (count > SIM_MAX_EVENTS) count = SIM_MAX_EVENTS;
    sim_events_t temp;
    memcpy(temp.events, events, (size_t)count * sizeof(sim_event_t));
    temp.count = count;
    process_sim_events(&temp);
}

void apply_remote_signal_channel(const NetSignalChannelMsg *msgs, int count) {
    /* Rebuild the client-side ring buffer from the snapshot. Server is
     * authoritative; on every post we get the current tail. */
    signal_channel_t *ch = &g.world.signal_channel;
    memset(ch, 0, sizeof(*ch));
    int n = count;
    if (n > SIGNAL_CHANNEL_CAPACITY) n = SIGNAL_CHANNEL_CAPACITY;
    for (int i = 0; i < n; i++) {
        signal_channel_msg_t *dst = &ch->msgs[i];
        memset(dst, 0, sizeof(*dst));
        dst->id = msgs[i].id;
        dst->timestamp_ms = msgs[i].timestamp_ms;
        dst->sender_station = msgs[i].sender_station;
        size_t tn = strlen(msgs[i].text);
        if (tn > SIGNAL_CHANNEL_TEXT_MAX - 1) tn = SIGNAL_CHANNEL_TEXT_MAX - 1;
        memcpy(dst->text, msgs[i].text, tn);
        dst->text_len = (uint8_t)tn;
        if (msgs[i].id > ch->next_id) ch->next_id = msgs[i].id;
    }
    ch->count = n;
    ch->head = n % SIGNAL_CHANNEL_CAPACITY;
}

void apply_remote_hail_response(uint8_t station, float credits, int contract_index) {
    if (station >= MAX_STATIONS) {
        set_notice("No station in range to hail.");
        return;
    }
    /* credits == -1 sentinel: ping reached this station's chirp range
     * (2× comm_range) but not its comms range. Show just a bearing. */
    if (credits < 0.0f) {
        set_notice("Too far — nearest: %s", g.world.stations[station].name);
        return;
    }
    /* Use the same hail overlay as singleplayer — station name + contextual
     * voice line + credits. The notice system is for transient alerts;
     * hails get their own 6-second radio-style overlay in the HUD. */
    snprintf(g.hail_station, sizeof(g.hail_station), "%s", g.world.stations[station].name);
    const char *ctx = contextual_hail_message(station);
    if (ctx)
        snprintf(g.hail_message, sizeof(g.hail_message), "%s", ctx);
    else
        snprintf(g.hail_message, sizeof(g.hail_message), "%s", g.world.stations[station].hail_message);
    g.hail_credits = credits;
    g.hail_station_index = station;
    g.hail_timer = 6.0f;
    /* Also route the hail through the bottom-right hint bar so the message
     * is visible in peripheral vision alongside the center-screen overlay. */
    set_notice("%s: %s", g.hail_station, g.hail_message);
    /* Auto-track the contract the station just issued, so the yellow
     * ring + compass pip appear without any tab navigation. */
    if (contract_index >= 0 && contract_index < MAX_CONTRACTS)
        g.tracked_contract = contract_index;
    onboarding_mark_hailed();
}

void begin_player_state_batch(void) {
    memcpy(g.player_interp.prev, g.player_interp.curr,
           sizeof(g.player_interp.prev));
    float elapsed = g.player_interp.t * g.player_interp.interval;
    elapsed = clampf(elapsed, 0.03f, 0.15f);
    g.player_interp.interval = lerpf(g.player_interp.interval, elapsed, 0.3f);
    g.player_interp.t = 0.0f;
}

void apply_remote_player_state(const NetPlayerState* state) {
    if (state->player_id >= NET_MAX_PLAYERS) return;

    if (state->player_id == net_local_id()) {
        /* Reconcile local prediction with server-authoritative position. */
        server_player_t* sp = &g.world.players[state->player_id];
        float dx = state->x - sp->ship.pos.x;
        float dy = state->y - sp->ship.pos.y;
        float dist_sq = dx * dx + dy * dy;

        if (dist_sq > 200.0f * 200.0f) {
            sp->ship.pos.x = state->x;
            sp->ship.pos.y = state->y;
            sp->ship.vel.x = state->vx;
            sp->ship.vel.y = state->vy;
        } else if (dist_sq > 20.0f * 20.0f) {
            sp->ship.pos.x = lerpf(sp->ship.pos.x, state->x, 0.5f);
            sp->ship.pos.y = lerpf(sp->ship.pos.y, state->y, 0.5f);
            sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, 0.5f);
            sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, 0.5f);
        } else {
            sp->ship.pos.x = lerpf(sp->ship.pos.x, state->x, 0.2f);
            sp->ship.pos.y = lerpf(sp->ship.pos.y, state->y, 0.2f);
            sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, 0.2f);
            sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, 0.2f);
        }
        sp->ship.angle = lerp_angle(sp->ship.angle, state->angle, 0.3f);
        /* Beam state is server-authoritative for the local player too —
         * the autopilot fires server-side and the client never predicts
         * its laser. Combat / hit prediction will eventually rely on
         * this same path. */
        sp->beam_active      = (state->flags & 2) != 0;
        sp->beam_ineffective = (state->flags & 32) != 0;
        sp->beam_hit         = (state->flags & 64) != 0;
        sp->scan_active      = (state->flags & 8) != 0;
        sp->beam_start = v2(state->beam_start_x, state->beam_start_y);
        sp->beam_end   = v2(state->beam_end_x,   state->beam_end_y);
        /* Tractor active is server-authoritative — autopilot owns it
         * server-side and the client never predicts toggles in MP mode.
         * Without this, the HUD shows stale "TRACTOR OFF" while the
         * server is actively pulling fragments. */
        sp->ship.tractor_active = (state->flags & 16) != 0;
        /* Thrust flag — drives flame visual when autopilot is active. */
        g.server_thrusting = (state->flags & 1) != 0;
    } else {
        /* Remote player: update curr for interpolation.
         * begin_player_state_batch() already shifted prev←curr. */
        bool was_active = g.player_interp.curr[state->player_id].active;
        g.player_interp.curr[state->player_id] = *state;
        /* First time we see this player with a callsign — show join notice */
        if (!was_active && state->active && state->callsign[0])
            set_notice("%s joined.", state->callsign);
    }
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
        /* Death detection moved to on_remote_death (NET_MSG_DEATH).
         * The packet now carries position + stats so the cinematic can
         * anchor at the wreckage. */
        sp->ship.hull = state->hull;
        g.station_balance = state->station_balance;
        sp->ship.mining_level = (int)state->mining_level;
        sp->ship.hold_level = (int)state->hold_level;
        sp->ship.tractor_level = (int)state->tractor_level;
        for (int c = 0; c < COMMODITY_COUNT; c++)
            sp->ship.cargo[c] = state->cargo[c];
        sp->nearby_fragments = (int)state->nearby_fragments;
        sp->tractor_fragments = (int)state->tractor_fragments;
        sp->ship.towed_count = state->towed_count;
        for (int t = 0; t < 10; t++)
            sp->ship.towed_fragments[t] = (state->towed_fragments[t] == 0xFFFFu)
                ? -1 : (int16_t)state->towed_fragments[t];
        /* Autopilot is also predict-protected: the [O] press triggers an
         * optimistic local toggle, and stale PLAYER_SHIP messages can
         * arrive carrying the pre-toggle value before the server has
         * processed the action. Without this guard the HUD label flickered
         * on/off during the round-trip window. */
        sp->autopilot_mode = state->autopilot_mode;
        sp->autopilot_target = (state->autopilot_target == 0xFF) ? -1 : (int)state->autopilot_target;
        /* Apply server's actual A* path for preview rendering. */
        g.autopilot_path_count = (int)state->path_count;
        g.autopilot_path_current = (int)state->path_current;
        for (int i = 0; i < state->path_count && i < 12; i++)
            g.autopilot_path[i] = v2(state->path_x[i], state->path_y[i]);
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
    server_player_t previous = {0};
    bool have_previous = false;
    if (net_id == 0xFF || net_id >= MAX_PLAYERS) return;
    if (g.local_player_slot == (int)net_id) {
        LOCAL_PLAYER.connected = true;
        return;
    }

    have_previous = server_player_copy_local(&previous, &g.world.players[g.local_player_slot]);
    server_player_t* assigned = &g.world.players[net_id];
    server_player_cleanup_local(&g.world.players[g.local_player_slot]);
    memset(&g.world.players[g.local_player_slot], 0, sizeof(g.world.players[g.local_player_slot]));
    g.local_player_slot = (int)net_id;
    if (have_previous && !assigned->connected && assigned->ship.hull <= 0.0f) {
        server_player_cleanup_local(assigned);
        *assigned = previous;
        previous.ship.manifest.units = NULL;
        previous.ship.manifest.count = 0;
        previous.ship.manifest.cap = 0;
    }
    server_player_cleanup_local(&previous);
    LOCAL_PLAYER.id = net_id;
    LOCAL_PLAYER.connected = true;
    LOCAL_PLAYER.conn = NULL;
}

void interpolate_world_for_render(void) {
    /* Singleplayer: local server syncs every tick, no interpolation needed.
     * g.world already has authoritative state from local_server_sync_to_client. */
    if (g.local_server.active) return;

    float t = clampf(g.asteroid_interp.t, 0.0f, 1.0f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *curr = &g.asteroid_interp.curr[i];
        const asteroid_t *prev = &g.asteroid_interp.prev[i];
        /* Skip inactive slots — avoid ~100-byte struct copy for empty entries. */
        if (!curr->active && !prev->active) {
            g.world.asteroids[i].active = false;
            continue;
        }
        asteroid_t *dst = &g.world.asteroids[i];
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, t);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, t);
            dst->rotation = lerp_angle(prev->rotation, curr->rotation, t);
        }
    }

    float nt = clampf(g.npc_interp.t, 0.0f, 1.0f);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *curr = &g.npc_interp.curr[i];
        const npc_ship_t *prev = &g.npc_interp.prev[i];
        if (!curr->active && !prev->active) {
            g.world.npc_ships[i].active = false;
            continue;
        }
        npc_ship_t *dst = &g.world.npc_ships[i];
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, nt);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, nt);
            dst->angle = lerp_angle(prev->angle, curr->angle, nt);
        }
    }
}

const NetPlayerState* net_get_interpolated_players(void) {
    static NetPlayerState result[NET_MAX_PLAYERS];
    if (g.local_server.active) return net_get_players();

    float pt = clampf(g.player_interp.t, 0.0f, 1.0f);
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        const NetPlayerState *prev = &g.player_interp.prev[i];
        const NetPlayerState *curr = &g.player_interp.curr[i];
        result[i] = *curr;
        if (prev->active && curr->active) {
            result[i].x = lerpf(prev->x, curr->x, pt);
            result[i].y = lerpf(prev->y, curr->y, pt);
            result[i].angle = lerp_angle(prev->angle, curr->angle, pt);
        }
    }
    return result;
}

void on_remote_death(uint8_t player_id, float pos_x, float pos_y,
                     float vel_x, float vel_y, float angle,
                     float ore_mined, float credits_earned, float credits_spent,
                     int asteroids_fractured) {
    if ((int)player_id != g.local_player_slot) return;
    g.death_ore_mined = ore_mined;
    g.death_credits_earned = credits_earned;
    g.death_credits_spent = credits_spent;
    g.death_asteroids_fractured = asteroids_fractured;
    /* Fire the cinematic at the death position. */
    g.death_cinematic.active = true;
    g.death_cinematic.phase = 0;
    g.death_cinematic.pos = v2(pos_x, pos_y);
    g.death_cinematic.vel = v2(vel_x, vel_y);
    g.death_cinematic.angle = angle;
    g.death_cinematic.spin = (((float)rand() / (float)RAND_MAX) - 0.5f) * 3.0f;
    g.death_cinematic.age = 0.0f;
    g.death_cinematic.menu_alpha = 0.0f;
    for (int i = 0; i < 8; i++) {
        float ang = ((float)i / 8.0f) * 2.0f * PI_F + (float)(i * 13 % 7) * 0.15f;
        float speed = 30.0f + (float)((i * 7 + 3) % 5) * 12.0f;
        g.death_cinematic.fragments[i][0] = 0.0f;
        g.death_cinematic.fragments[i][1] = 0.0f;
        g.death_cinematic.fragments[i][2] = cosf(ang) * speed + vel_x * 0.6f;
        g.death_cinematic.fragments[i][3] = sinf(ang) * speed + vel_y * 0.6f;
        g.death_cinematic.fragments[i][4] = ang;
        g.death_cinematic.fragments[i][5] = ((float)((i * 19 + 7) % 11) - 5.0f) * 0.6f;
    }
    /* Suppress the legacy detector path */
    g.death_screen_timer = 0.0f;
    g.death_screen_max = 0.0f;
    episode_trigger(&g.episode, 9);
    memset(g.episode.watched, 0, sizeof(g.episode.watched));
    g.episode.stations_visited = 0;
    episode_save(&g.episode);
}

void on_remote_world_time(float server_time) {
    g.world.time = server_time;
}
