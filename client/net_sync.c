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

#define STATION_RING_CORRECTION_SEC 0.35f
#define NET_MOTION_TELEMETRY_WINDOW_SEC 5.0f
#define LOCAL_PLAYER_RENDER_OFFSET_MAX 140.0f
#define LOCAL_PLAYER_RENDER_OFFSET_LATENCY_MAX 260.0f
#define LOCAL_PLAYER_RENDER_SNAP_DIST 200.0f
#define LOCAL_PLAYER_RENDER_SNAP_LATENCY_DIST 360.0f
#define LOCAL_PLAYER_STALE_ACK_DEFER_DIST 200.0f
#define NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC 0.075f
#define NET_REPLAY_LATENCY_BLEND_MAX_SEC 0.45f
#define NET_REPLAY_REBASE_SKEW_TICKS 96
#define ASTEROID_RENDER_CORRECTION_SEC 0.18f
#define ASTEROID_RENDER_EXTRAPOLATE_MAX_SEC 0.75f
#define NPC_RENDER_CORRECTION_SEC 0.18f
#define NPC_RENDER_EXTRAPOLATE_MAX_SEC 0.60f
#define REMOTE_PENDING_RECEIPT_CAP 64
/* Replay is keyed to server-anchored sim ticks. Movement packets carry the
 * client-predicted target tick, and the server only applies them during the
 * matching world_sim_step(), so snapshots and prediction frames share one
 * integer clock instead of racing packet-arrival time. */
#define NET_REPLAY_ENABLED 1

static float station_ring_correction[MAX_STATIONS][MAX_ARMS];
static bool station_ring_have_snapshot[MAX_STATIONS];
static cargo_receipt_chain_t remote_pending_receipts[REMOTE_PENDING_RECEIPT_CAP];
static uint8_t remote_pending_receipt_pub[REMOTE_PENDING_RECEIPT_CAP][32];
static uint8_t remote_pending_receipt_count;

bool net_local_prediction_enabled(void) {
    if (!g.multiplayer_enabled || g.local_server.active) return true;
    return g.net_input_tick_protocol;
}

static bool net_replay_enabled(void) {
    return NET_REPLAY_ENABLED && g.net_input_tick_protocol;
}

static float nearest_angle_delta(float from, float to) {
    float delta = to - from;
    while (delta >  PI_F) delta -= TWO_PI_F;
    while (delta < -PI_F) delta += TWO_PI_F;
    return delta;
}

void reset_station_ring_smoothing(void) {
    memset(station_ring_correction, 0, sizeof(station_ring_correction));
    memset(station_ring_have_snapshot, 0, sizeof(station_ring_have_snapshot));
}

void step_remote_station_rings(float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;
        for (int a = 0; a < MAX_ARMS; a++) {
            float correction = station_ring_correction[s][a];
            float correction_step = 0.0f;
            if (fabsf(correction) > 0.00001f) {
                float k = dt / STATION_RING_CORRECTION_SEC;
                if (k > 1.0f) k = 1.0f;
                correction_step = correction * k;
                station_ring_correction[s][a] -= correction_step;
            } else {
                station_ring_correction[s][a] = 0.0f;
            }
            st->arm_rotation[a] += st->arm_omega[a] * dt + correction_step;
        }
    }
}

static bool replay_tick_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static input_intent_t replay_movement_intent(const input_intent_t *intent) {
    input_intent_t out = {0};
    if (!intent) return out;
    out.turn = intent->turn;
    out.thrust = intent->thrust;
    out.mine = intent->mine;
    out.mining_target_hint = intent->mining_target_hint;
    out.tractor_hold = intent->tractor_hold;
    out.boost = intent->boost;
    out.reverse_thrust = intent->reverse_thrust;
    return out;
}

static input_replay_frame_t *net_replay_frame_at(int offset) {
    int index = ((int)g.net_replay_start + offset) % NET_REPLAY_FRAME_CAP;
    return &g.net_replay[index];
}

static void net_replay_clear_frames(void) {
    g.net_replay_start = 0;
    g.net_replay_count = 0;
}

void net_replay_reset(void) {
    g.net_prediction_tick = 0;
    g.net_prediction_tick_valid = false;
    net_replay_clear_frames();
}

static void net_replay_append(const input_replay_frame_t *frame) {
    if (g.net_replay_count >= NET_REPLAY_FRAME_CAP) {
        g.net_replay_start =
            (uint16_t)((g.net_replay_start + 1u) % NET_REPLAY_FRAME_CAP);
        g.net_replay_count--;
    }
    int index = ((int)g.net_replay_start + (int)g.net_replay_count) %
                NET_REPLAY_FRAME_CAP;
    g.net_replay[index] = *frame;
    g.net_replay_count++;
}

void net_replay_record_prediction(const input_intent_t *intent, float dt) {
    if (!intent || dt <= 0.0f) return;
    if (!net_replay_enabled()) return;
    if (!g.multiplayer_enabled || g.local_server.active || !net_is_connected())
        return;
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    if (!g.net_prediction_tick_valid) return;

    g.net_prediction_tick++;
    input_replay_frame_t frame = {
        .tick = g.net_prediction_tick,
        .input_seq = g.net_input_seq,
        .dt = dt,
        .intent = replay_movement_intent(intent),
    };
    net_replay_append(&frame);
}

static void net_replay_prune_through(uint32_t server_tick) {
    while (g.net_replay_count > 0) {
        input_replay_frame_t *frame = net_replay_frame_at(0);
        if (replay_tick_after(frame->tick, server_tick)) break;
        g.net_replay_start =
            (uint16_t)((g.net_replay_start + 1u) % NET_REPLAY_FRAME_CAP);
        g.net_replay_count--;
    }
    if (g.net_replay_count == 0) g.net_replay_start = 0;
}

static int net_replay_first_after(uint32_t server_tick) {
    for (int i = 0; i < (int)g.net_replay_count; i++) {
        if (replay_tick_after(net_replay_frame_at(i)->tick, server_tick))
            return i;
    }
    return -1;
}

static bool net_replay_missing_prefix(uint32_t server_tick, int first_after) {
    if (first_after < 0) return false;
    return net_replay_frame_at(first_after)->tick != server_tick + 1u;
}

float net_prediction_latency_blend(void) {
    if (g.net_last_ack_rtt <= NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC)
        return 0.0f;
    return clampf((g.net_last_ack_rtt - NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC) /
                  (NET_REPLAY_LATENCY_BLEND_MAX_SEC -
                   NET_REPLAY_LATENCY_BLEND_MIN_RTT_SEC),
                  0.0f, 1.0f);
}

static bool net_replay_has_frames_after(uint32_t server_tick) {
    if (!g.net_prediction_tick_valid) return false;
    if (!replay_tick_after(g.net_prediction_tick, server_tick)) return false;
    return net_replay_first_after(server_tick) >= 0;
}

static bool should_defer_stale_unacked_motion(const NetPlayerState *state,
                                              bool has_unacked_input,
                                              float dist_sq) {
    if (!state || !net_local_prediction_enabled() || !has_unacked_input)
        return false;
    float defer_dist = LOCAL_PLAYER_STALE_ACK_DEFER_DIST;
    if (dist_sq > defer_dist * defer_dist) return false;
    if ((state->flags & 4) != 0) return true;
    if (!net_replay_enabled()) return true;
    return !net_replay_has_frames_after(state->server_tick);
}

static void apply_authoritative_local_motion(const NetPlayerState *state,
                                             server_player_t *sp) {
    sp->ship.pos.x = state->x;
    sp->ship.pos.y = state->y;
    sp->ship.vel.x = state->vx;
    sp->ship.vel.y = state->vy;
    sp->ship.angle = state->angle;
    if ((state->flags & 4) == 0)
        sp->docked = false;
}

static bool net_replay_reconcile_local_player(const NetPlayerState *state,
                                              server_player_t *sp,
                                              int *out_replayed) {
    *out_replayed = 0;
    if (!net_replay_enabled()) return false;
    uint32_t server_tick = state->server_tick;
    if (server_tick == 0 && !g.net_prediction_tick_valid) return false;

    if (!g.net_prediction_tick_valid) {
        g.net_prediction_tick = server_tick;
        g.net_prediction_tick_valid = true;
        net_replay_clear_frames();
    }

    if ((state->flags & 4) != 0) {
        apply_authoritative_local_motion(state, sp);
        g.net_prediction_tick = server_tick;
        net_replay_clear_frames();
        return true;
    }

    int first_after = net_replay_first_after(server_tick);
    if (net_replay_missing_prefix(server_tick, first_after)) return false;

    sim_events_t saved_events = g.world.events;
    apply_authoritative_local_motion(state, sp);
    uint32_t last_tick = server_tick;
    int replay_count = (int)g.net_replay_count;
    for (int i = first_after; i >= 0 && i < replay_count; i++) {
        input_replay_frame_t *frame = net_replay_frame_at(i);
        sp->input = frame->intent;
        world_sim_step_player_only(&g.world, g.local_player_slot, frame->dt);
        last_tick = frame->tick;
        (*out_replayed)++;
    }
    g.world.events = saved_events;

    g.net_prediction_tick = last_tick;
    g.net_prediction_tick_valid = true;
    net_replay_prune_through(server_tick);
    return true;
}

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

static asteroid_t asteroid_render_state_at(int slot, float elapsed) {
    const asteroid_t *prev = &g.asteroid_interp.prev[slot];
    const asteroid_t *curr = &g.asteroid_interp.curr[slot];
    asteroid_t out = *curr;
    if (!curr->active) return out;

    out.pos.x += curr->vel.x * elapsed;
    out.pos.y += curr->vel.y * elapsed;
    out.age += elapsed;
    out.rotation = wrap_angle(curr->rotation + curr->spin * elapsed);

    if (prev->active) {
        float blend = clampf(elapsed / ASTEROID_RENDER_CORRECTION_SEC, 0.0f, 1.0f);
        out.pos.x = lerpf(prev->pos.x, out.pos.x, blend);
        out.pos.y = lerpf(prev->pos.y, out.pos.y, blend);
        out.rotation = lerp_angle(prev->rotation, out.rotation, blend);
    }
    return out;
}

static npc_ship_t npc_render_state_at(int slot, float elapsed) {
    const npc_ship_t *prev = &g.npc_interp.prev[slot];
    const npc_ship_t *curr = &g.npc_interp.curr[slot];
    npc_ship_t out = *curr;
    if (!curr->active) return out;

    out.ship.pos.x += curr->ship.vel.x * elapsed;
    out.ship.pos.y += curr->ship.vel.y * elapsed;

    if (prev->active) {
        float blend = clampf(elapsed / NPC_RENDER_CORRECTION_SEC, 0.0f, 1.0f);
        out.ship.pos.x = lerpf(prev->ship.pos.x, out.ship.pos.x, blend);
        out.ship.pos.y = lerpf(prev->ship.pos.y, out.ship.pos.y, blend);
        out.ship.angle = lerp_angle(prev->ship.angle, out.ship.angle, blend);
    }
    return out;
}

void reset_remote_dynamic_sync(void) {
    g.net_input_tick_protocol = false;
    net_replay_reset();

    memset(g.world.asteroids, 0, sizeof(g.world.asteroids));
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    g.asteroid_interp.interval = 0.1f;

    memset(g.world.npc_ships, 0, sizeof(g.world.npc_ships));
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    g.npc_interp.interval = 0.1f;

    memset(g.world.scaffolds, 0, sizeof(g.world.scaffolds));
    LOCAL_PLAYER.hover_asteroid = -1;
}

void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {
    /* Keep asteroid motion at client frame speed. Before applying the
     * new server packet, carry the current visual state forward by
     * velocity. The authoritative snapshot then blends from that visible
     * state instead of snapping rocks back to packet-age positions. */
    float elapsed = g.asteroid_interp.t * g.asteroid_interp.interval;
    elapsed = clampf(elapsed, 0.0f, ASTEROID_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        g.asteroid_interp.prev[i] = asteroid_render_state_at(i, elapsed);

    float packet_interval = clampf(elapsed, 0.05f, 0.2f);
    g.asteroid_interp.interval = lerpf(g.asteroid_interp.interval, packet_interval, 0.3f);
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
        a->crystal_stage = asteroids[i].crystal_stage;
        if (a->max_hp < a->hp) a->max_hp = a->hp;
        if (a->max_ore < a->ore) a->max_ore = a->ore;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!received[i] && g.asteroid_interp.curr[i].active) {
            /* Not in this delta: continue from the dead-reckoned visual
             * position instead of freezing until a later packet. */
            g.asteroid_interp.curr[i] = g.asteroid_interp.prev[i];
        }
    }

    /* World asteroids are updated by interpolate_world_for_render() at
     * render time, ensuring game logic and rendering see the same positions. */
}

void apply_remote_npcs(const NetNpcState* npcs, int count) {
    float npc_elapsed = g.npc_interp.t * g.npc_interp.interval;
    npc_elapsed = clampf(npc_elapsed, 0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++)
        g.npc_interp.prev[i] = npc_render_state_at(i, npc_elapsed);

    float packet_interval = clampf(npc_elapsed, 0.05f, 0.2f);
    g.npc_interp.interval = lerpf(g.npc_interp.interval, packet_interval, 0.3f);
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
        n->ship.hull_class = (n->role == NPC_ROLE_HAULER)
            ? HULL_CLASS_HAULER : HULL_CLASS_MINER;
        n->ship.pos.x = npcs[i].x;
        n->ship.pos.y = npcs[i].y;
        n->ship.vel.x = npcs[i].vx;
        n->ship.vel.y = npcs[i].vy;
        n->ship.angle = npcs[i].angle;
        n->target_asteroid = npcs[i].target_asteroid;
        n->towed_fragment = npcs[i].towed_fragment;
        n->towed_scaffold = -1;
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
    /* Diff against last seen inventory + credit_pool to fire a chain-
     * event heartbeat pulse on the world. Inventory tracks production
     * and consumption; credit_pool tracks commerce (ledger movement
     * from sales, supplier credits, contract payouts). Together they
     * cover the chain events visible to a player at-a-glance.
     * Thresholds are loose so float drift in the smelter (~0.016/tick)
     * doesn't fire every frame: 0.5 units of any commodity, or 5
     * credits of pool delta. Mirror the singleplayer existence gate
     * so an uninhabited slot with stale prev_seen=true doesn't fire. */
    if (g.station_prev_seen[index] && station_exists(st)) {
        bool fired = false;
        for (int i = 0; i < COMMODITY_COUNT; i++) {
            if (fabsf(inventory[i] - g.station_prev_inventory[index][i]) >= 0.5f) {
                fired = true;
                break;
            }
        }
        if (!fired
            && fabsf(credit_pool - g.station_prev_credit_pool[index]) >= 5.0f) {
            fired = true;
        }
        if (fired) g.station_heartbeat[index] = 1.0f;
    }
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        st->_inventory_cache[i] = inventory[i];
        g.station_prev_inventory[index][i] = inventory[i];
    }
    g.station_prev_credit_pool[index] = credit_pool;
    g.station_prev_seen[index] = station_exists(st);
}

/* Phase 2 wire: server → client station manifest summary. Fully
 * replaces the (commodity, grade) count matrix for this station so a
 * missing entry reads as zero. */
void apply_remote_station_manifest(uint8_t station_id,
                                   const NetStationManifestEntry *entries,
                                   int count) {
    if (station_id >= MAX_STATIONS) return;
    if (count < 0) count = 0;
    memset(&g.station_manifest_summary[station_id][0][0], 0,
           sizeof(g.station_manifest_summary[station_id]));
    for (int i = 0; i < count; i++) {
        uint8_t c = entries[i].commodity;
        uint8_t gr = entries[i].grade;
        if (c >= COMMODITY_COUNT) continue;
        if (gr >= MINING_GRADE_COUNT) continue;
        g.station_manifest_summary[station_id][c][gr] = entries[i].count;
    }
}

static bool cargo_unit_from_named_ingot_entry(const NetNamedIngotEntry *entry,
                                             cargo_unit_t *out) {
    if (!entry || !out) return false;
    if (entry->commodity >= COMMODITY_COUNT) return false;
    if (entry->grade >= MINING_GRADE_COUNT) return false;
    memset(out, 0, sizeof(*out));
    out->kind = (uint8_t)CARGO_KIND_INGOT;
    out->commodity = entry->commodity;
    out->grade = entry->grade;
    out->prefix_class = entry->prefix_class;
    if (out->prefix_class >= INGOT_PREFIX_COUNT)
        out->prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
    out->recipe_id = (uint16_t)RECIPE_SMELT;
    out->origin_station = entry->origin_station;
    out->quantity = 1;
    out->mined_block = entry->mined_block;
    memcpy(out->pub, entry->pub, sizeof(out->pub));
    return true;
}

/* Detailed station named-ingot snapshot. The station manifest remains a
 * partial provenance mirror in multiplayer: counts come from
 * g.station_manifest_summary, while this manifest holds only the named
 * ingot units needed for representative lineage strings. */
void apply_remote_station_ingots(uint8_t station_id,
                                 const NetNamedIngotEntry *entries,
                                 int count) {
    if (station_id >= MAX_STATIONS) return;
    if (count < 0) count = 0;
    if (count > NET_NAMED_INGOT_MAX) count = NET_NAMED_INGOT_MAX;
    station_t *st = &g.world.stations[station_id];
    if (!st->manifest.units && !station_manifest_bootstrap(st)) return;
    manifest_clear(&st->manifest);
    ship_receipts_t *station_receipts = station_get_receipts(st);
    if (station_receipts) ship_receipts_clear(station_receipts);
    for (int i = 0; i < count; i++) {
        cargo_unit_t unit = {0};
        if (!cargo_unit_from_named_ingot_entry(&entries[i], &unit)) continue;
        if (!station_manifest_push_with_chain(st, &unit, NULL)) break;
    }
}

void apply_remote_hold_ingots(const NetNamedIngotEntry *entries, int count) {
    if (count < 0) count = 0;
    if (count > NET_NAMED_INGOT_MAX) count = NET_NAMED_INGOT_MAX;
    g.remote_hold_named_ingot_count = 0;
    if (!entries || count == 0) return;
    for (int i = 0; i < count; i++)
        g.remote_hold_named_ingots[g.remote_hold_named_ingot_count++] = entries[i];
}

static int remote_pending_receipt_find(const uint8_t cargo_pub[32]) {
    if (!cargo_pub) return -1;
    for (int i = 0; i < remote_pending_receipt_count; i++) {
        if (memcmp(remote_pending_receipt_pub[i], cargo_pub, 32) == 0)
            return i;
    }
    return -1;
}

static bool receipt_chain_cargo_pub(const cargo_receipt_chain_t *chain,
                                    uint8_t out[32]) {
    static const uint8_t zero32[32] = {0};
    if (!chain || chain->len == 0 || chain->len > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return false;
    if (memcmp(chain->links[0].cargo_pub, zero32, 32) == 0) return false;
    memcpy(out, chain->links[0].cargo_pub, 32);
    return true;
}

static bool remote_attach_receipt_chain(ship_t *ship,
                                        const cargo_receipt_chain_t *chain) {
    uint8_t cargo_pub[32];
    if (!ship || !chain || !receipt_chain_cargo_pub(chain, cargo_pub))
        return false;
    if (!ship->manifest.units || !ship->receipts_opaque) return false;
    int idx = manifest_find(&ship->manifest, cargo_pub);
    if (idx < 0) return false;
    ship_receipts_t *receipts = ship_get_receipts(ship);
    if (!receipts || idx >= (int)receipts->count) return false;
    receipts->chains[idx] = *chain;
    return true;
}

static void remote_store_receipt_chain(const cargo_receipt_chain_t *chain) {
    uint8_t cargo_pub[32];
    if (!receipt_chain_cargo_pub(chain, cargo_pub)) return;
    int idx = remote_pending_receipt_find(cargo_pub);
    if (idx < 0) {
        if (remote_pending_receipt_count >= REMOTE_PENDING_RECEIPT_CAP) {
            memmove(remote_pending_receipt_pub,
                    &remote_pending_receipt_pub[1],
                    (REMOTE_PENDING_RECEIPT_CAP - 1) * sizeof(remote_pending_receipt_pub[0]));
            memmove(remote_pending_receipts,
                    &remote_pending_receipts[1],
                    (REMOTE_PENDING_RECEIPT_CAP - 1) * sizeof(remote_pending_receipts[0]));
            idx = REMOTE_PENDING_RECEIPT_CAP - 1;
        } else {
            idx = remote_pending_receipt_count++;
        }
    }
    memcpy(remote_pending_receipt_pub[idx], cargo_pub, 32);
    remote_pending_receipts[idx] = *chain;

    if (g.local_player_slot >= 0 && g.local_player_slot < MAX_PLAYERS)
        (void)remote_attach_receipt_chain(&g.world.players[g.local_player_slot].ship,
                                          chain);
}

static bool receipt_bundle_is_one_chain(const cargo_receipt_t *receipts,
                                        int count) {
    if (!receipts || count <= 0 || count > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return false;
    const uint8_t *cargo_pub = receipts[0].cargo_pub;
    for (int i = 1; i < count; i++) {
        if (memcmp(receipts[i].cargo_pub, cargo_pub, 32) != 0)
            return false;
    }
    return cargo_receipt_chain_verify(receipts, (size_t)count, cargo_pub)
        == CARGO_RECEIPT_OK;
}

void apply_remote_cargo_receipt_bundle(const cargo_receipt_t *receipts,
                                       int count) {
    if (!receipts || count <= 0) return;
    if (count > CARGO_RECEIPT_CHAIN_MAX_LEN)
        count = CARGO_RECEIPT_CHAIN_MAX_LEN;

    if (receipt_bundle_is_one_chain(receipts, count)) {
        cargo_receipt_chain_t chain = {0};
        memcpy(chain.links, receipts, (size_t)count * sizeof(receipts[0]));
        chain.len = (uint8_t)count;
        remote_store_receipt_chain(&chain);
        return;
    }

    for (int i = 0; i < count; i++) {
        if (cargo_receipt_chain_verify(&receipts[i], 1, receipts[i].cargo_pub)
            != CARGO_RECEIPT_OK) {
            continue;
        }
        cargo_receipt_chain_t chain = {0};
        chain.links[0] = receipts[i];
        chain.len = 1;
        remote_store_receipt_chain(&chain);
    }
}

void apply_remote_inspect_snapshot(const NetInspectSnapshot *snapshot) {
    if (!snapshot) return;

    /* Linger: keep the snapshot on screen for ~3.5s after release.
     * The was_active flag marks the active→idle edge. Once we've
     * bumped the timer into the linger window, was_active is false
     * and this branch is a no-op until the next active scan — the
     * timer counts down naturally. Earlier `timer ≤ 0.60` trick
     * silently re-fired ~2.9s into the linger when the timer
     * crossed back below 0.60 on its way to zero, restarting the
     * countdown indefinitely. */
    if (snapshot->target_type == INSPECT_TARGET_NONE) {
        if (g.inspect_was_active) {
            g.inspect_snapshot_timer = 3.5f;
            g.inspect_was_active = false;
        }
    } else {
        g.inspect_snapshot = *snapshot;
        g.inspect_snapshot_timer = 0.60f;
        g.inspect_was_active = true;
    }

    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    server_player_t *sp = &g.world.players[g.local_player_slot];
    if (snapshot->target_type == INSPECT_TARGET_NONE) {
        sp->scan_active = false;
        sp->scan_target_type = 0;
        sp->scan_target_index = -1;
        sp->scan_module_index = -1;
        return;
    }

    sp->scan_active = true;
    sp->scan_target_type = (int)snapshot->target_type;
    sp->scan_target_index = (snapshot->target_index == 0xFFu)
        ? -1 : (int)snapshot->target_index;
    sp->scan_module_index = (snapshot->module_index == 0xFFu)
        ? -1 : (int)snapshot->module_index;
}

void apply_remote_highscores(const NetHighscoreEntry *entries, int count) {
    if (count < 0) count = 0;
    int cap = (int)(sizeof(g.highscores) / sizeof(g.highscores[0]));
    if (count > cap) count = cap;
    memset(g.highscores, 0, sizeof(g.highscores));
    for (int i = 0; i < count; i++) {
        memcpy(g.highscores[i].callsign, entries[i].callsign, 8);
        g.highscores[i].credits_earned = entries[i].credits_earned;
        g.highscores[i].world_id   = entries[i].world_id;
        g.highscores[i].world_seq  = entries[i].world_seq;
        g.highscores[i].build_id   = entries[i].build_id;
        g.highscores[i].epoch_tick = entries[i].epoch_tick;
        memcpy(g.highscores[i].killed_by, entries[i].killed_by, 8);
    }
    g.highscore_count = count;
}

/* Replace the local player's ship.manifest with units that match the
 * server-authoritative count summary. HOLD_INGOTS supplies detailed
 * named-ingot provenance for units the protocol can describe; the rest
 * are synthesized legacy-migrate units so counts remain complete. */
void apply_remote_player_manifest(const NetStationManifestEntry *entries,
                                  int count) {
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS) return;
    ship_t *ship = &g.world.players[g.local_player_slot].ship;
    /* Always apply -- WORLD_STATE overwrites cargo[] every tick, so
     * gating manifest on action_predict_timer leaves cargo and
     * manifest in inconsistent states (cargo refreshed, manifest
     * frozen at pre-action). The trade UI then shows phantom rows
     * (manifest > cargo). The brief predict/snapshot flicker is the
     * lesser evil compared to ghost SELL rows the player can't act on. */
    if (!ship->manifest.units && !ship_manifest_bootstrap(ship)) return;
    manifest_clear(&ship->manifest);
    ship_receipts_t *receipts = ship_get_receipts(ship);
    if (receipts) ship_receipts_clear(receipts);
    if (count <= 0) return;
    uint8_t origin[8] = { 'S','R','V','M','I','R','R','0' };
    uint16_t out_idx = 0;
    bool named_used[NET_NAMED_INGOT_MAX] = { false };
    for (int i = 0; i < count; i++) {
        uint8_t c = entries[i].commodity;
        uint8_t gr = entries[i].grade;
        uint16_t n = entries[i].count;
        if (c >= COMMODITY_COUNT) continue;
        if (gr >= MINING_GRADE_COUNT) continue;
        cargo_kind_t kind;
        if (!cargo_kind_for_commodity((commodity_t)c, &kind)) continue;
        uint16_t remaining = n;
        for (int j = 0; j < g.remote_hold_named_ingot_count && remaining > 0; j++) {
            if (named_used[j]) continue;
            const NetNamedIngotEntry *entry = &g.remote_hold_named_ingots[j];
            if (entry->commodity != c || entry->grade != gr) continue;
            if (ship->manifest.count >= ship->manifest.cap) return;
            cargo_unit_t unit = {0};
            if (!cargo_unit_from_named_ingot_entry(entry, &unit)) continue;
            if (!ship_manifest_push_with_chain(ship, &unit, NULL)) return;
            int pending_idx = remote_pending_receipt_find(unit.pub);
            if (pending_idx >= 0)
                (void)remote_attach_receipt_chain(ship,
                                                  &remote_pending_receipts[pending_idx]);
            named_used[j] = true;
            remaining--;
        }
        for (uint16_t k = 0; k < remaining; k++) {
            if (ship->manifest.count >= ship->manifest.cap) return;
            cargo_unit_t unit = {0};
            if (!hash_legacy_migrate_unit(origin, (commodity_t)c, out_idx++, &unit))
                continue;
            unit.grade = gr;
            if (!ship_manifest_push_with_chain(ship, &unit, NULL)) return;
        }
    }
}

void apply_remote_contracts(const contract_t* contracts, int count) {
    /* Full replacement: clear all, then copy received */
    for (int i = 0; i < MAX_CONTRACTS; i++)
        g.world.contracts[i].active = false;
    for (int i = 0; i < count && i < MAX_CONTRACTS; i++)
        g.world.contracts[i] = contracts[i];
}

void apply_remote_player_known_contracts(uint32_t mask) {
    g.player_known_contract_mask = mask;
}

void apply_remote_station_identity(const NetStationIdentity* si) {
    if (si->index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[si->index];
    float local_rotation[MAX_ARMS];
    for (int a = 0; a < MAX_ARMS; a++)
        local_rotation[a] = st->arm_rotation[a];
    bool smooth_rotation = station_ring_have_snapshot[si->index];

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
    for (int m = si->module_count; m < MAX_MODULES_PER_STATION; m++)
        st->module_diag[m] = STATION_FLOW_DIAG_NONE;
    st->arm_count = si->arm_count;
    for (int a = 0; a < MAX_ARMS; a++) {
        st->arm_speed[a] = si->arm_speed[a];
        st->ring_offset[a] = si->ring_offset[a];
        if (smooth_rotation) {
            station_ring_correction[si->index][a] =
                nearest_angle_delta(local_rotation[a], si->arm_rotation[a]);
            st->arm_rotation[a] = local_rotation[a];
        } else {
            st->arm_rotation[a] = si->arm_rotation[a];
            station_ring_correction[si->index][a] = 0.0f;
        }
        st->arm_omega[a] = si->arm_omega[a];
    }
    station_ring_have_snapshot[si->index] = true;
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
    snprintf(st->hail_message, sizeof(st->hail_message), "%s", si->hail_message);
    for (int i = 0; i < STATION_IDENTITY_CHATTER_LINES; i++) {
        snprintf(st->miner_chatter[i], sizeof(st->miner_chatter[i]), "%s",
                 si->miner_chatter[i]);
        snprintf(st->hauler_chatter[i], sizeof(st->hauler_chatter[i]), "%s",
                 si->hauler_chatter[i]);
    }
    snprintf(st->rati_hail_message, sizeof(st->rati_hail_message), "%s",
             si->rati_hail_message);
    snprintf(st->currency_name, sizeof(st->currency_name), "%s", si->currency_name);
    /* Mirror the station's Ed25519 pubkey for client-side verification of
     * future signed events (#479 B). The secret stays server-side. */
    memcpy(st->station_pubkey, si->station_pubkey, sizeof(st->station_pubkey));
}

void apply_remote_station_diag(uint8_t station_id, const uint8_t *diag,
                               int module_count) {
    if (station_id >= MAX_STATIONS || !diag) return;
    station_t *st = &g.world.stations[station_id];
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        st->module_diag[m] = (m < module_count) ? diag[m] : STATION_FLOW_DIAG_NONE;
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

static bool net_hash32_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static void net_station_hail_label(uint8_t station, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (station >= MAX_STATIONS) {
        snprintf(out, cap, "Unknown");
        return;
    }
    const station_t *st = &g.world.stations[station];
    if (net_hash32_is_zero(st->station_pubkey)) {
        snprintf(out, cap, "%s", st->name);
        return;
    }
    char id[8];
    mining_callsign_from_pubkey(st->station_pubkey, id);
    snprintf(out, cap, "%s [%s]", st->name, id);
}

void apply_remote_hail_response(uint8_t station, float credits, int contract_index) {
    if (station >= MAX_STATIONS) {
        set_notice("Local scan sweep.");
        return;
    }
    /* Use the same hail overlay as singleplayer — station name + the
     * operator-authored station hail + credits. Tutorial/system guidance
     * is intentionally kept out of station hails. */
    net_station_hail_label(station, g.hail_station, sizeof(g.hail_station));
    const char *msg = g.world.stations[station].hail_message;
    snprintf(g.hail_message, sizeof(g.hail_message), "%s",
             msg[0] ? msg : "Signal acknowledged.");
    float shown_credits = credits >= 0.0f ? credits : 0.0f;
    g.hail_credits = shown_credits;
    g.hail_station_index = station;
    g.hail_timer = 6.0f;
    /* Route the hail through the bottom-right hint bar. Includes the
     * station balance so all the info the old center-screen overlay
     * carried lands there. `credits` is authoritative from the server. */
    {
        const char *unit = g.world.stations[station].currency_name;
        if (!unit[0]) unit = "credits";
        if (credits >= 0.0f) {
            set_notice("%s: %s  (balance %d %s)",
                g.hail_station, g.hail_message, (int)lroundf(shown_credits), unit);
        } else {
            set_notice("%s: %s", g.hail_station, g.hail_message);
        }
    }
    /* Track station work when the hail response names a real board contract.
     * Hail itself is scan/contact; the server no longer mints ad hoc
     * nearest-rock fracture jobs just to have something to point at. */
    if (contract_index >= 0 && contract_index < MAX_CONTRACTS)
        g.tracked_contract = contract_index;
    onboarding_mark_hailed();
}

void begin_player_state_batch(void) {
    memcpy(g.player_interp.prev, g.player_interp.curr,
           sizeof(g.player_interp.prev));
    float prev_interval = g.net_motion.packet_interval;
    float elapsed = g.player_interp.t * g.player_interp.interval;
    elapsed = clampf(elapsed, 0.03f, 0.15f);
    g.net_motion.packet_interval = elapsed;
    if (elapsed > g.net_motion.max_packet_interval_run)
        g.net_motion.max_packet_interval_run = elapsed;
    if (prev_interval > 0.0f) {
        float jitter = fabsf(elapsed - prev_interval);
        if (jitter > g.net_motion.max_packet_jitter_run)
            g.net_motion.max_packet_jitter_run = jitter;
    }
    g.net_motion.total_player_batches++;
    g.player_interp.interval = lerpf(g.player_interp.interval, elapsed, 0.3f);
    g.player_interp.t = 0.0f;
}

void net_record_input_ack(uint16_t input_seq_ack) {
    if (input_seq_ack == 0) return;
    int index = (int)(input_seq_ack % NET_INPUT_TIMING_CAP);
    net_input_timing_t *timing = &g.net_input_timing[index];
    if (timing->seq != input_seq_ack || timing->sent_at <= 0.0f) return;

    float rtt = g.net_time - timing->sent_at;
    if (rtt < 0.0f || rtt > 30.0f) return;
    g.net_last_ack_rtt = rtt;
    if (rtt > g.net_max_ack_rtt_5s) g.net_max_ack_rtt_5s = rtt;
    if (rtt > g.net_motion.max_ack_rtt_run)
        g.net_motion.max_ack_rtt_run = rtt;
    g.net_motion.total_input_acks++;
    timing->seq = 0;
    timing->sent_at = 0.0f;
}

static void record_local_player_motion_telemetry(float correction_dist,
                                                 float velocity_error,
                                                 float applied_correction_dist,
                                                 bool deferred,
                                                 int replayed_frames) {
    g.net_motion.correction_dist = correction_dist;
    g.net_motion.applied_correction_dist = applied_correction_dist;
    g.net_motion.velocity_error = velocity_error;
    if (correction_dist > g.net_motion.max_correction_5s)
        g.net_motion.max_correction_5s = correction_dist;
    if (applied_correction_dist > g.net_motion.max_applied_correction_5s)
        g.net_motion.max_applied_correction_5s = applied_correction_dist;
    if (correction_dist > g.net_motion.max_correction_run)
        g.net_motion.max_correction_run = correction_dist;
    if (applied_correction_dist > g.net_motion.max_applied_correction_run)
        g.net_motion.max_applied_correction_run = applied_correction_dist;
    if (velocity_error > g.net_motion.max_velocity_error_run)
        g.net_motion.max_velocity_error_run = velocity_error;
    g.net_motion.window_elapsed += g.net_motion.packet_interval;
    g.net_motion.samples++;
    g.net_motion.total_samples++;
    if (deferred) g.net_motion.deferred_samples++;
    if (deferred) g.net_motion.total_deferred_samples++;
    if (replayed_frames > 0) {
        g.net_motion.replayed_samples++;
        g.net_motion.replayed_frames += (uint32_t)replayed_frames;
        g.net_motion.total_replayed_samples++;
        g.net_motion.total_replayed_frames += (uint32_t)replayed_frames;
    }
    if (g.net_motion.window_elapsed < NET_MOTION_TELEMETRY_WINDOW_SEC) return;

    printf("[net-motion] pkt=%.3fs corr=%.1f max5=%.1f applied=%.1f maxapp5=%.1f velerr=%.1f deferred=%u/%u replayed=%u/%u frames=%u\n",
           g.net_motion.packet_interval,
           g.net_motion.correction_dist,
           g.net_motion.max_correction_5s,
           g.net_motion.applied_correction_dist,
           g.net_motion.max_applied_correction_5s,
           g.net_motion.velocity_error,
           (unsigned)g.net_motion.deferred_samples,
           (unsigned)g.net_motion.samples,
           (unsigned)g.net_motion.replayed_samples,
           (unsigned)g.net_motion.samples,
           (unsigned)g.net_motion.replayed_frames);
    g.net_motion.max_correction_5s = 0.0f;
    g.net_motion.max_applied_correction_5s = 0.0f;
    g.net_motion.window_elapsed = 0.0f;
    g.net_motion.samples = 0;
    g.net_motion.deferred_samples = 0;
    g.net_motion.replayed_samples = 0;
    g.net_motion.replayed_frames = 0;
}

static void add_local_player_render_correction(vec2 applied_delta,
                                               float correction_dist,
                                               bool docked) {
    float latency_blend = net_prediction_latency_blend();
    float snap_dist = lerpf(LOCAL_PLAYER_RENDER_SNAP_DIST,
                            LOCAL_PLAYER_RENDER_SNAP_LATENCY_DIST,
                            latency_blend);
    if (docked || correction_dist > snap_dist) {
        g.local_player_render_offset = v2(0.0f, 0.0f);
        return;
    }

    g.local_player_render_offset =
        v2_add(g.local_player_render_offset, applied_delta);
    float len = v2_len(g.local_player_render_offset);
    float max_offset = lerpf(LOCAL_PLAYER_RENDER_OFFSET_MAX,
                             LOCAL_PLAYER_RENDER_OFFSET_LATENCY_MAX,
                             latency_blend);
    if (len > max_offset) {
        g.local_player_render_offset =
            v2_scale(g.local_player_render_offset,
                     max_offset / len);
        len = max_offset;
    }
    if (len > g.net_motion.max_render_offset_run)
        g.net_motion.max_render_offset_run = len;
}

void apply_remote_player_state(const NetPlayerState* state) {
    if (state->player_id >= NET_MAX_PLAYERS) return;

    if (state->player_id == net_local_id()) {
        /* Reconcile local prediction with server-authoritative position. */
        server_player_t* sp = &g.world.players[state->player_id];
        vec2 before_pos = sp->ship.pos;
        if (state->has_input_tick_ack) g.net_input_tick_protocol = true;
        bool force_rebase = false;
        if (state->server_tick != 0 && g.net_prediction_tick_valid) {
            int32_t skew =
                (int32_t)(g.net_prediction_tick - state->server_tick);
            int32_t abs_skew = skew < 0 ? -skew : skew;
            g.net_motion.tick_skew = skew;
            if (abs_skew > g.net_motion.max_tick_skew_abs)
                g.net_motion.max_tick_skew_abs = abs_skew;
            force_rebase = skew > NET_REPLAY_REBASE_SKEW_TICKS;
        }
        bool has_input_ack = state->input_seq_ack != 0;
        bool has_unacked_input =
            has_input_ack && g.net_input_seq != 0 &&
            state->input_seq_ack != g.net_input_seq;
        if (has_input_ack) {
            g.net_last_server_ack = state->input_seq_ack;
            net_record_input_ack(state->input_seq_ack);
        }
        if (state->server_tick != 0) g.net_last_server_tick = state->server_tick;

        float target_x = state->x;
        float target_y = state->y;

        float dx = target_x - sp->ship.pos.x;
        float dy = target_y - sp->ship.pos.y;
        float dist_sq = dx * dx + dy * dy;
        float correction_dist = sqrtf(dist_sq);
        float dvx = state->vx - sp->ship.vel.x;
        float dvy = state->vy - sp->ship.vel.y;
        float velocity_error = sqrtf(dvx * dvx + dvy * dvy);

        bool state_docked = (state->flags & 4) != 0;
        int replayed_frames = 0;
        bool used_replay = false;
        bool used_snap = false;
        bool used_lerp = false;
        bool defer_motion_correction = false;
        if (force_rebase) {
            apply_authoritative_local_motion(state, sp);
            net_replay_clear_frames();
            g.net_prediction_tick = state->server_tick;
            g.net_prediction_tick_valid = state->server_tick != 0;
            used_snap = true;
        } else {
            defer_motion_correction =
                should_defer_stale_unacked_motion(state, has_unacked_input, dist_sq);
        }
        if (!force_rebase && !defer_motion_correction)
            used_replay =
                net_replay_reconcile_local_player(state, sp, &replayed_frames);
        if (!force_rebase && !defer_motion_correction && !used_replay) {
            defer_motion_correction =
                should_defer_stale_unacked_motion(state, has_unacked_input, dist_sq);
        }
        if (!force_rebase && !used_replay && !defer_motion_correction) {
            if (dist_sq > 200.0f * 200.0f) {
                used_snap = true;
                sp->ship.pos.x = target_x;
                sp->ship.pos.y = target_y;
                sp->ship.vel.x = state->vx;
                sp->ship.vel.y = state->vy;
            } else if (dist_sq > 20.0f * 20.0f) {
                used_lerp = true;
                sp->ship.pos.x = lerpf(sp->ship.pos.x, target_x, 0.5f);
                sp->ship.pos.y = lerpf(sp->ship.pos.y, target_y, 0.5f);
                sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, 0.5f);
                sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, 0.5f);
            } else {
                used_lerp = dist_sq > 0.01f;
                sp->ship.pos.x = lerpf(sp->ship.pos.x, target_x, 0.2f);
                sp->ship.pos.y = lerpf(sp->ship.pos.y, target_y, 0.2f);
                sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, 0.2f);
                sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, 0.2f);
            }
            net_replay_reset();
            if (state->server_tick != 0) {
                g.net_prediction_tick = state->server_tick;
                g.net_prediction_tick_valid = true;
            }
        }
        if (used_snap) g.net_motion.total_snap_samples++;
        if (used_lerp) g.net_motion.total_lerp_samples++;
        vec2 applied_delta = v2_sub(before_pos, sp->ship.pos);
        add_local_player_render_correction(
            applied_delta, v2_len(applied_delta), state_docked);
        record_local_player_motion_telemetry(
            correction_dist, velocity_error, v2_len(applied_delta),
            defer_motion_correction, replayed_frames);
        if (!used_replay && !defer_motion_correction)
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

    net_replay_reset();
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

    float asteroid_elapsed = clampf(g.asteroid_interp.t * g.asteroid_interp.interval,
                                    0.0f, ASTEROID_RENDER_EXTRAPOLATE_MAX_SEC);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *curr = &g.asteroid_interp.curr[i];
        const asteroid_t *prev = &g.asteroid_interp.prev[i];
        /* Skip inactive slots — avoid ~100-byte struct copy for empty entries. */
        if (!curr->active && !prev->active) {
            g.world.asteroids[i].active = false;
            continue;
        }
        asteroid_t *dst = &g.world.asteroids[i];
        *dst = asteroid_render_state_at(i, asteroid_elapsed);
    }

    float npc_elapsed = clampf(g.npc_interp.t * g.npc_interp.interval,
                               0.0f, NPC_RENDER_EXTRAPOLATE_MAX_SEC);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *curr = &g.npc_interp.curr[i];
        const npc_ship_t *prev = &g.npc_interp.prev[i];
        if (!curr->active && !prev->active) {
            g.world.npc_ships[i].active = false;
            continue;
        }
        npc_ship_t *dst = &g.world.npc_ships[i];
        *dst = npc_render_state_at(i, npc_elapsed);
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
                     int asteroids_fractured,
                     uint8_t respawn_station, float respawn_fee) {
    if ((int)player_id != g.local_player_slot) return;
    net_replay_reset();
    float impact_speed = sqrtf(vel_x * vel_x + vel_y * vel_y);
    float severity = clampf(impact_speed / 260.0f, 0.8f, 2.4f);
    float spin_dir = ((rand() & 1) != 0) ? 1.0f : -1.0f;
    g.death_ore_mined = ore_mined;
    g.death_credits_earned = credits_earned;
    g.death_credits_spent = credits_spent;
    g.death_asteroids_fractured = asteroids_fractured;
    g.death_respawn_station = respawn_station;
    g.death_respawn_fee = respawn_fee;
    /* Fire the cinematic at the death position. */
    g.death_cinematic.active = true;
    g.death_cinematic.phase = 0;
    g.death_cinematic.pos = v2(pos_x, pos_y);
    g.death_cinematic.vel = v2(vel_x, vel_y);
    g.death_cinematic.angle = angle;
    g.death_cinematic.spin = spin_dir * clampf(3.0f + impact_speed / 45.0f, 5.0f, 16.0f);
    g.death_cinematic.age = 0.0f;
    g.death_cinematic.menu_alpha = 0.0f;
    g.thrusting = false;
    LOCAL_PLAYER.beam_active = false;
    LOCAL_PLAYER.beam_hit = false;
    LOCAL_PLAYER.ship.tractor_active = false;
    g.screen_shake = fmaxf(g.screen_shake, clampf(26.0f + impact_speed * 0.12f, 38.0f, 82.0f));
    for (int i = 0; i < 8; i++) {
        float ang = ((float)i / 8.0f) * 2.0f * PI_F + (float)(i * 13 % 7) * 0.15f;
        float speed = 82.0f + severity * 34.0f + (float)((i * 7 + 3) % 5) * 22.0f;
        g.death_cinematic.fragments[i][0] = 0.0f;
        g.death_cinematic.fragments[i][1] = 0.0f;
        g.death_cinematic.fragments[i][2] = cosf(ang) * speed + vel_x * 0.45f;
        g.death_cinematic.fragments[i][3] = sinf(ang) * speed + vel_y * 0.45f;
        g.death_cinematic.fragments[i][4] = ang;
        g.death_cinematic.fragments[i][5] = ((float)((i * 19 + 7) % 11) - 5.0f) *
                                            (1.0f + severity * 0.45f);
    }
    /* Suppress the legacy detector path */
    g.death_screen_timer = 0.0f;
    g.death_screen_max = 0.0f;
    memset(g.episode.watched, 0, sizeof(g.episode.watched));
    g.episode.stations_visited = 0;
    episode_trigger(&g.episode, 9);
    episode_save(&g.episode);
    music_enter_death(&g.music);
}

void on_remote_world_time(float server_time) {
    float delta = server_time - g.world.time;
    if (fabsf(delta) > 2.0f) {
        g.world.time = server_time;
    } else {
        g.world.time += delta * 0.10f;
    }
}
