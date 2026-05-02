/*
 * sim_ai.c -- NPC ship subsystem.
 * Extracted from game_sim.c: target finding, steering, physics,
 * spawn, state machines (MINER / HAULER / TOW), and the per-tick
 * step_npc_ships() dispatcher.
 */
#include "sim_ai.h"
#include "sim_nav.h"
#include "sim_flight.h"
#include "sim_ship.h"
#include "sim_production.h" /* sim_can_smelt_ore for miner asteroid filter */
#include "sim_mining.h"
#include "signal_model.h"
#include "manifest.h"
#include "ship.h"
#include "game_sim.h" /* SHIP_COLLISION_DAMAGE_THRESHOLD/_SCALE */
#include <math.h>
#include <string.h>

/* Remove up to `n` cargo units of `c` from a station's manifest.
 * Returns the number actually removed. Walks backward so removing
 * doesn't disturb earlier indices. Used by NPC haulers so the
 * manifest stays in lockstep with the inventory float; otherwise the
 * trade picker (manifest-only) shows phantom rows for stock the
 * hauler already carried away. */
static int station_manifest_drain_commodity(station_t *st, commodity_t c, int n) {
    if (!st || !st->manifest.units || n <= 0) return 0;
    int removed = 0;
    for (int16_t i = (int16_t)st->manifest.count - 1; i >= 0 && removed < n; i--) {
        if (st->manifest.units[i].commodity == (uint8_t)c) {
            if (manifest_remove(&st->manifest, (uint16_t)i, NULL)) removed++;
        }
    }
    return removed;
}

/* Inverse: push `n` synthetic legacy-migrate units of `c` into a
 * station's manifest. Used at NPC unload until haulers get their own
 * manifest_t. The origin is per-hauler so the units are traceable to
 * "delivered by NPC slot K". */
static int station_manifest_seed_from_npc(station_t *st, commodity_t c, int n,
                                          int npc_slot) {
    if (!st || n <= 0) return 0;
    if (st->manifest.cap == 0 && !station_manifest_bootstrap(st)) return 0;
    uint8_t origin[8] = { 'N','P','C','D','0','0','0','0' };
    origin[7] = (uint8_t)('0' + (npc_slot % 10));
    int pushed = 0;
    for (int i = 0; i < n; i++) {
        if (st->manifest.count >= st->manifest.cap) break;
        cargo_unit_t unit = {0};
        if (!hash_legacy_migrate_unit(origin, c, (uint16_t)i, &unit)) continue;
        if (!manifest_push(&st->manifest, &unit)) break;
        pushed++;
    }
    return pushed;
}

/* ================================================================== */
/* NPC ships                                                          */
/* ================================================================== */

/* #294 Slice 6: paired character_t lifecycle.
 *
 * Each active NPC gets a paired character_t entry; future slices flip
 * the source-of-truth for brain state and damage routing onto it.
 * `ship_idx` carries the NPC slot during the transition — once the
 * unified ships[] pool lands, it'll point there instead.
 *
 * Nothing reads the character pool yet. These writes are intentionally
 * "dead" so the lifecycle is observable in saves/wire without flipping
 * any readers in the same slice. */
static character_kind_t character_kind_from_role(npc_role_t role) {
    switch (role) {
    case NPC_ROLE_MINER:  return CHARACTER_KIND_NPC_MINER;
    case NPC_ROLE_HAULER: return CHARACTER_KIND_NPC_HAULER;
    case NPC_ROLE_TOW:    return CHARACTER_KIND_NPC_TOW;
    default:              return CHARACTER_KIND_NONE;
    }
}

/* Find a free ships[] slot — one not pointed to by any active character.
 * Returns -1 if the pool is full. */
static int ship_pool_alloc_slot(const world_t *w) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int s = 0; s < MAX_SHIPS; s++) {
        bool taken = false;
        for (int i = 0; i < cap; i++) {
            if (w->characters[i].active && w->characters[i].ship_idx == s) {
                taken = true;
                break;
            }
        }
        if (!taken) return s;
    }
    return -1;
}

/* Initialize a ships[] slot from an NPC's snapshot. Frees any prior
 * manifest the slot was holding so this is safe for a slot that was
 * previously occupied (e.g. after rebuild_characters_from_npcs). */
static void ship_pool_init_from_npc(ship_t *ship, const npc_ship_t *npc) {
    ship_cleanup(ship);
    memset(ship, 0, sizeof(*ship));
    (void)ship_manifest_bootstrap(ship);
    ship->pos = npc->pos;
    ship->vel = npc->vel;
    ship->angle = npc->angle;
    ship->hull_class = npc->hull_class;
    ship->hull = npc->hull;
}

static int character_alloc_for_npc(world_t *w, int npc_slot, const npc_ship_t *npc) {
    int ship_slot = ship_pool_alloc_slot(w);
    if (ship_slot < 0) return -1;
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (w->characters[i].active) continue;
        character_t *c = &w->characters[i];
        memset(c, 0, sizeof(*c));
        c->active = true;
        c->kind = character_kind_from_role(npc->role);
        c->ship_idx = ship_slot;
        c->npc_slot = npc_slot;
        c->state = npc->state;
        c->target_asteroid = npc->target_asteroid;
        c->home_station = npc->home_station;
        c->dest_station = npc->dest_station;
        c->state_timer = npc->state_timer;
        c->towed_fragment = npc->towed_fragment;
        c->towed_scaffold = npc->towed_scaffold;
        ship_pool_init_from_npc(&w->ships[ship_slot], npc);
        return i;
    }
    /* No free character slot; release the ship slot we reserved. */
    ship_cleanup(&w->ships[ship_slot]);
    memset(&w->ships[ship_slot], 0, sizeof(w->ships[ship_slot]));
    return -1;
}

/* Find the paired character for an NPC slot, or -1. Matches on the
 * explicit npc_slot field — distinct from ship_idx which addresses a
 * different pool. */
static int character_for_npc_slot(const world_t *w, int npc_slot) {
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return -1;
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        const character_t *c = &w->characters[i];
        if (!c->active) continue;
        if (c->npc_slot != npc_slot) continue;
        if (c->kind != CHARACTER_KIND_NPC_MINER &&
            c->kind != CHARACTER_KIND_NPC_HAULER &&
            c->kind != CHARACTER_KIND_NPC_TOW) continue;
        return i;
    }
    return -1;
}

static void character_free_for_npc(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return;
    int ship_slot = w->characters[idx].ship_idx;
    if (ship_slot >= 0 && ship_slot < MAX_SHIPS) {
        ship_cleanup(&w->ships[ship_slot]);
        memset(&w->ships[ship_slot], 0, sizeof(w->ships[ship_slot]));
    }
    w->characters[idx].active = false;
}

/* Resolve an NPC slot to its paired ship, or NULL if no character is
 * paired or ship_idx is out of range. */
static ship_t *npc_ship_for(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return NULL;
    int s = w->characters[idx].ship_idx;
    if (s < 0 || s >= MAX_SHIPS) return NULL;
    return &w->ships[s];
}

/* Public wrapper: rejects NULL world and out-of-range / inactive
 * slots. Used by tests and external readers that want to inspect or
 * mutate an NPC's paired ship_t. */
ship_t *world_npc_ship_for(world_t *w, int npc_slot) {
    if (!w) return NULL;
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return NULL;
    if (!w->npc_ships[npc_slot].active) return NULL;
    return npc_ship_for(w, npc_slot);
}

/* End-of-tick paired-pool sync — npc -> ship for physics fields plus
 * ship -> npc for hull. Slice 13's pre-mirror (mirror_ship_pos_to_npc,
 * called at the top of each NPC step) is what makes external ship.pos
 * /vel/angle writes between ticks survive; after that pull, this
 * end-of-tick mirror just round-trips them back to the ship.
 *
 *   - hull is ship-authoritative since Slice 9/11 (apply_npc_ship_damage,
 *     hauler dock auto-repair both write ship.hull). Push ship -> npc
 *     so the npc-side despawn check reads a fresh value next tick.
 *   - pos / vel / angle / thrusting still get integrated on npc fields
 *     by the existing dispatch; npc -> ship at end of tick keeps the
 *     ship faithful for external readers and parity tests. Slice 14
 *     will collapse this into a single direction once npc_ship_t loses
 *     its physics fields. */
static void mirror_ship_to_npc(world_t *w, int npc_slot) {
    ship_t *s = npc_ship_for(w, npc_slot);
    if (!s) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    npc->hull = s->hull;
    s->pos = npc->pos;
    s->vel = npc->vel;
    s->angle = npc->angle;
}

/* Slice 13: pre-mirror at the top of each NPC step. Pulls any external
 * ship.pos/vel/angle writes (PvP rock impulse, future autopilot, etc.)
 * into the npc fields BEFORE physics integrates this tick. Without
 * this, the post-mirror at end-of-tick would clobber the external
 * write with the integrated-from-stale-npc value — that was the bug
 * the parity tripwire (Slice 13a) was set up to surface. */
static void mirror_ship_pos_to_npc(world_t *w, int npc_slot) {
    ship_t *s = npc_ship_for(w, npc_slot);
    if (!s) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    npc->pos = s->pos;
    npc->vel = s->vel;
    npc->angle = s->angle;
}

/* Apply damage to an NPC with optional kill attribution. The reverse
 * mirror at end-of-tick pushes the result into npc->hull so the
 * existing despawn check fires when hull <= 0. If the hit drops hull
 * to <= 0 AND killer_token is non-zero, emits SIM_EVENT_NPC_KILL.
 *
 * Public: external code (rock-throw collision, PvP, etc.) reaches NPC
 * damage through this helper so the unified ship_t.hull stays the
 * single source of truth. Validates inputs — out-of-range or inactive
 * slots are no-ops. */
void apply_npc_ship_damage_attributed(world_t *w, int npc_slot, float dmg,
                                       const uint8_t killer_token[8], uint8_t cause) {
    if (!w) return;
    if (dmg <= 0.0f) return;
    if (npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    if (!npc->active) return;
    ship_t *s = npc_ship_for(w, npc_slot);
    float prev_hull;
    if (!s) {
        prev_hull = npc->hull;
        npc->hull -= dmg;
        if (npc->hull < 0.0f) npc->hull = 0.0f;
    } else {
        prev_hull = s->hull;
        s->hull -= dmg;
        if (s->hull < 0.0f) s->hull = 0.0f;
    }
    /* Kill-feed: emit only on the lethal blow, only if attributed. */
    if (prev_hull > 0.0f) {
        float new_hull = s ? s->hull : npc->hull;
        if (new_hull <= 0.0f && killer_token) {
            bool nonzero = (killer_token[0] | killer_token[1] | killer_token[2] |
                            killer_token[3] | killer_token[4] | killer_token[5] |
                            killer_token[6] | killer_token[7]) != 0;
            if (nonzero) {
                sim_event_t kill_ev = {
                    .type = SIM_EVENT_NPC_KILL,
                    .npc_kill = {
                        .cause = cause,
                        .npc_role = (uint8_t)npc->role,
                    },
                };
                memcpy(kill_ev.npc_kill.killer_token, killer_token, 8);
                emit_event(w, kill_ev);
            }
        }
    }
}

/* Unattributed damage — environmental hits that don't credit a kill. */
void apply_npc_ship_damage(world_t *w, int npc_slot, float dmg) {
    apply_npc_ship_damage_attributed(w, npc_slot, dmg, NULL, DEATH_CAUSE_ASTEROID);
}

/* Mirror brain state from an NPC into its paired character_t (#294
 * Slice 7) AND physics state into its paired ship_t (#294 Slice 8).
 * Called at the top of each NPC's tick so future readers can trust the
 * controller + ship layer; the npc-side fields remain the source of
 * truth that the dispatch switch writes back to. */
static void mirror_npc_to_character(world_t *w, int npc_slot) {
    int idx = character_for_npc_slot(w, npc_slot);
    if (idx < 0) return;
    const npc_ship_t *npc = &w->npc_ships[npc_slot];
    character_t *c = &w->characters[idx];
    c->state = npc->state;
    c->target_asteroid = npc->target_asteroid;
    c->home_station = npc->home_station;
    c->dest_station = npc->dest_station;
    c->state_timer = npc->state_timer;
    c->towed_fragment = npc->towed_fragment;
    c->towed_scaffold = npc->towed_scaffold;
    if (c->ship_idx >= 0 && c->ship_idx < MAX_SHIPS) {
        ship_t *s = &w->ships[c->ship_idx];
        s->pos = npc->pos;
        s->vel = npc->vel;
        s->angle = npc->angle;
        s->hull_class = npc->hull_class;
        /* Don't mirror hull npc->ship here: ship.hull is authoritative
         * (Slice 9 + 10). External callers — apply_npc_ship_damage and
         * future rock/PvP impact paths — may have mutated ship.hull
         * between this NPC's last tick and now; mirroring npc.hull
         * over it would lose that damage/repair. The end-of-tick
         * reverse mirror (mirror_ship_to_npc) keeps npc.hull in sync. */
    }
}

/* Target NPC roster per starter station — must match the world_reset
 * seed (see game_sim.c: spawn_npc calls). Outposts (>=3) are player-
 * built and don't get auto-replenished here; they can scaffold their
 * own NPCs via gameplay later. */
static void station_target_npc_counts(int station_idx, const station_t *st,
                                      int *miners, int *haulers) {
    *miners = 0;
    *haulers = 0;
    if (!st || !station_is_active(st)) return;
    switch (station_idx) {
    case 0: *miners = 2; *haulers = 2; return;  /* Prospect */
    case 1: *miners = 0; *haulers = 1; return;  /* Kepler   */
    case 2: *miners = 1; *haulers = 1; return;  /* Helios   */
    default: return;                            /* outposts: no auto */
    }
}

/* Walk the active NPC pool and count active members per home station,
 * per role. Used by replenish_npc_roster to pick the most-understaffed
 * (station, role) pair. */
static void count_npc_roster(const world_t *w,
                             int miners[MAX_STATIONS],
                             int haulers[MAX_STATIONS]) {
    for (int s = 0; s < MAX_STATIONS; s++) { miners[s] = 0; haulers[s] = 0; }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        const npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS) continue;
        if (npc->role == NPC_ROLE_MINER)  miners[npc->home_station]++;
        if (npc->role == NPC_ROLE_HAULER) haulers[npc->home_station]++;
    }
}

/* Spawn at most ONE NPC to fill the largest gap between actual and
 * target roster. Drip-feed (caller gates with npc_respawn_timer) so a
 * full wipe recovers gradually. Sovereign station can run negative; pool
 * is informational, so spawning is no longer gated on solvency.
 * Returns true if a spawn fired. */
static bool replenish_npc_roster(world_t *w) {
    int miners[MAX_STATIONS], haulers[MAX_STATIONS];
    count_npc_roster(w, miners, haulers);

    /* Find the largest shortfall across all (station, role) pairs.
     * Tie-broken by station index (lower wins). */
    int best_station = -1;
    npc_role_t best_role = NPC_ROLE_MINER;
    int best_shortfall = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        int target_m = 0, target_h = 0;
        station_target_npc_counts(s, &w->stations[s], &target_m, &target_h);
        /* Sovereign station can run negative; pool is informational. */
        int short_m = target_m - miners[s];
        int short_h = target_h - haulers[s];
        if (short_m > best_shortfall) {
            best_shortfall = short_m; best_station = s; best_role = NPC_ROLE_MINER;
        }
        if (short_h > best_shortfall) {
            best_shortfall = short_h; best_station = s; best_role = NPC_ROLE_HAULER;
        }
    }
    if (best_station < 0) return false;
    int slot = spawn_npc(w, best_station, best_role);
    return slot >= 0;
}

void rebuild_characters_from_npcs(world_t *w) {
    /* Free heap-allocated manifests on all ships[] slots before we
     * deactivate the characters that pinned them. Without this, slots
     * that aren't reclaimed by the next pass (e.g. once MAX_SHIPS >
     * MAX_NPC_SHIPS or when fewer NPCs are active than before) leak
     * their manifest_t.units allocation. ship_pool_init_from_npc on
     * re-alloc would also clean — but only for slots actually picked. */
    for (int s = 0; s < MAX_SHIPS; s++) {
        ship_cleanup(&w->ships[s]);
        memset(&w->ships[s], 0, sizeof(w->ships[s]));
    }
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (w->characters[i].kind == CHARACTER_KIND_NPC_MINER ||
            w->characters[i].kind == CHARACTER_KIND_NPC_HAULER ||
            w->characters[i].kind == CHARACTER_KIND_NPC_TOW) {
            w->characters[i].active = false;
        }
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* v32 -> v33 migration: regenerate session_token for any
         * active NPC loaded without one. Same byte layout as
         * spawn_npc so the token shape is consistent. */
        bool has_token = (npc->session_token[0] | npc->session_token[1] |
                          npc->session_token[2] | npc->session_token[3] |
                          npc->session_token[4] | npc->session_token[5] |
                          npc->session_token[6] | npc->session_token[7]) != 0;
        if (!has_token) {
            if (w->next_npc_token == 0) w->next_npc_token = 1;
            uint16_t tok = w->next_npc_token++;
            npc->session_token[0] = 'N';
            npc->session_token[1] = 'P';
            npc->session_token[2] = 'C';
            npc->session_token[3] = (uint8_t)npc->home_station;
            npc->session_token[4] = (uint8_t)npc->role;
            npc->session_token[5] = (uint8_t)n;
            npc->session_token[6] = (uint8_t)(tok & 0xFF);
            npc->session_token[7] = (uint8_t)((tok >> 8) & 0xFF);
        }
        (void)character_alloc_for_npc(w, n, npc);
    }
}

/* Spawn an NPC at a station. Returns slot index or -1 if full. */
int spawn_npc(world_t *w, int station_idx, npc_role_t role) {
    int slot = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w->npc_ships[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    station_t *st = &w->stations[station_idx];
    hull_class_t hc;
    switch (role) {
    case NPC_ROLE_MINER: hc = HULL_CLASS_NPC_MINER; break;
    case NPC_ROLE_HAULER: hc = HULL_CLASS_HAULER; break;
    case NPC_ROLE_TOW:    hc = HULL_CLASS_HAULER; break; /* tow drone uses hauler hull */
    default: hc = HULL_CLASS_NPC_MINER; break;
    }
    npc_ship_t *npc = &w->npc_ships[slot];
    memset(npc, 0, sizeof(*npc));
    /* Clear stale path from previous occupant of this slot. */
    *nav_npc_path(slot) = (nav_path_t){0};
    npc->active = true;
    npc->role = role;
    npc->hull_class = hc;
    npc->state = NPC_STATE_DOCKED;
    npc->pos = v2_add(st->pos, v2(30.0f * (float)(slot % 3 - 1), -(st->radius + hull_def_for_class(hc)->ship_radius + 50.0f)));
    npc->angle = PI_F * 0.5f;
    npc->target_asteroid = -1;
    npc->towed_fragment = -1;
    npc->towed_scaffold = -1;
    npc->home_station = station_idx;
    npc->dest_station = station_idx;
    npc->state_timer = (role == NPC_ROLE_MINER) ? NPC_DOCK_TIME : HAULER_DOCK_TIME;
    npc->hull = npc_max_hull(npc);
    npc->tint_r = 1.0f; npc->tint_g = 1.0f; npc->tint_b = 1.0f;
    /* Tow drones get a distinct yellow-amber tint */
    if (role == NPC_ROLE_TOW) {
        npc->tint_r = 1.0f; npc->tint_g = 0.85f; npc->tint_b = 0.30f;
    }
    /* Per-NPC economic identity. Bytes:
     *   [0..2] 'NPC' magic (distinguishes from player session_tokens
     *          which are 8 random bytes from the session handshake)
     *   [3]    station_idx
     *   [4]    role
     *   [5]    slot
     *   [6..7] world counter (little-endian) — increments each spawn
     *          so respawns of the same role at the same slot get a
     *          fresh ledger identity. The dead token's ledger entry
     *          stays attributed until the 16-slot LRU evicts it. */
    if (w->next_npc_token == 0) w->next_npc_token = 1;
    uint16_t tok = w->next_npc_token++;
    npc->session_token[0] = 'N';
    npc->session_token[1] = 'P';
    npc->session_token[2] = 'C';
    npc->session_token[3] = (uint8_t)station_idx;
    npc->session_token[4] = (uint8_t)role;
    npc->session_token[5] = (uint8_t)slot;
    npc->session_token[6] = (uint8_t)(tok & 0xFF);
    npc->session_token[7] = (uint8_t)((tok >> 8) & 0xFF);
    /* No starter balance — fresh NPCs run on credit and pay it back
     * as they complete deliveries. ledger_force_debit at the dock
     * lets the balance go negative; the chain self-balances over
     * time as the hauler ferries goods. */
    /* Pair a character_t with the NPC. Lifecycle-only — nothing reads
     * it yet (#294 Slice 6). If the pool is somehow exhausted we still
     * spawn the NPC; this is best-effort during the transition. */
    (void)character_alloc_for_npc(w, slot, npc);
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_NPC_SPAWNED,
        .npc_spawned = { .slot = slot, .role = role, .home_station = station_idx },
    });
    SIM_LOG("[sim] spawned %s at station %d (slot %d)\n",
            role == NPC_ROLE_MINER ? "miner" :
            role == NPC_ROLE_HAULER ? "hauler" : "tow drone",
            station_idx, slot);
    return slot;
}

static bool npc_target_valid(const world_t *w, const npc_ship_t *npc) {
    if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_ASTEROIDS) return false;
    const asteroid_t *a = &w->asteroids[npc->target_asteroid];
    return a->active && a->tier != ASTEROID_TIER_S;
}

/* Asteroid-already-taken check, reading from the controller layer
 * (#294 Slice 7+8): scan characters[] for any other MINER targeting
 * `target_idx`. The mirror at top of tick keeps character.target_asteroid
 * in sync with npc.target_asteroid. `self_char_idx` is excluded so the
 * caller doesn't see itself as a competitor. */
static bool miner_target_taken(const world_t *w, int target_idx, int self_char_idx) {
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        if (i == self_char_idx) continue;
        const character_t *c = &w->characters[i];
        if (!c->active || c->kind != CHARACTER_KIND_NPC_MINER) continue;
        if (c->target_asteroid == target_idx) return true;
    }
    return false;
}

/* Look for a free-floating S-tier fragment within `range_sq` of the
 * NPC. "Free" means not currently on any player's tractor (their
 * ship.towed_fragments[] list) and not already claimed by another
 * miner NPC (their npc_ship_t.towed_fragment).
 *
 * Used so miner NPCs prefer cleaning up loose fragments over fracturing
 * fresh rock. Returns asteroid index, or -1. */
static int npc_find_loose_fragment(const world_t *w, const npc_ship_t *self, float range_sq) {
    int best = -1;
    float best_d = range_sq;
    int self_slot = (int)(self - w->npc_ships);
    for (int fi = 0; fi < MAX_ASTEROIDS; fi++) {
        const asteroid_t *f = &w->asteroids[fi];
        if (!f->active || f->tier != ASTEROID_TIER_S) continue;
        bool taken = false;
        /* Player tow check. */
        for (int p = 0; p < MAX_PLAYERS && !taken; p++) {
            if (!w->players[p].connected) continue;
            const ship_t *ship = &w->players[p].ship;
            for (int t = 0; t < (int)(sizeof(ship->towed_fragments) / sizeof(ship->towed_fragments[0])); t++) {
                if (ship->towed_fragments[t] == (int16_t)fi) { taken = true; break; }
            }
        }
        if (taken) continue;
        /* Other-NPC tow check. */
        for (int j = 0; j < MAX_NPC_SHIPS; j++) {
            if (j == self_slot) continue;
            if (w->npc_ships[j].active && w->npc_ships[j].towed_fragment == fi) {
                taken = true; break;
            }
        }
        if (taken) continue;
        float d2 = v2_dist_sq(self->pos, f->pos);
        if (d2 < best_d) { best_d = d2; best = fi; }
    }
    return best;
}

/* Wrapper: claim a loose fragment for this NPC and stamp the
 * smelt-payout token. Called at every place the miner is about to
 * decide on a fracture target — picking up an existing fragment is
 * always preferable to fracturing more rock. Returns true iff a
 * fragment was claimed and the caller should transition to
 * NPC_STATE_RETURN_TO_STATION. */
static bool npc_try_claim_loose_fragment(world_t *w, npc_ship_t *npc, float range_sq) {
    int frag = npc_find_loose_fragment(w, npc, range_sq);
    if (frag < 0) return false;
    npc->towed_fragment = frag;
    asteroid_t *f = &w->asteroids[frag];
    memcpy(f->last_towed_token, npc->session_token, sizeof(f->last_towed_token));
    return true;
}

/* True if the miner's home station has any smeltable raw-ore stock
 * above `frac` of REFINERY_HOPPER_CAPACITY. Used to gate fracturing:
 * when the hopper is at-or-above 50%, miners stop creating new mass
 * and only tractor pre-existing fragments — keeps the belt clean
 * without backing the smelter further. */
static bool npc_home_ore_above_frac(const world_t *w, const npc_ship_t *npc, float frac) {
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS) return false;
    const station_t *home = &w->stations[npc->home_station];
    float threshold = REFINERY_HOPPER_CAPACITY * frac;
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
        if (!sim_can_smelt_ore(home, (commodity_t)c)) continue;
        if (home->_inventory_cache[c] >= threshold) return true;
    }
    return false;
}

static int npc_find_mineable_asteroid(const world_t *w, const npc_ship_t *npc) {
    int self_npc_slot = (int)(npc - w->npc_ships);
    int self_char = character_for_npc_slot(w, self_npc_slot);

    /* Priority: DESTROY contract targets first — but only if reasonably
     * nearby. Without the distance cap, a Helios miner would pick up a
     * FRACTURE distress posted near Prospect and drift halfway across
     * the map "lasering" a rock it can't reach. 2500u is roughly the
     * radius at which a miner would notice trouble in its own sector. */
    const float MAX_DISTRESS_DIST_SQ = 2500.0f * 2500.0f;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active || w->contracts[k].action != CONTRACT_FRACTURE) continue;
        int idx = w->contracts[k].target_index;
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
        if (v2_dist_sq(npc->pos, w->asteroids[idx].pos) > MAX_DISTRESS_DIST_SQ) continue;
        if (!miner_target_taken(w, idx, self_char)) return idx;
    }

    /* Two-pass nearest search:
     *   pass 1 — only ores the miner's HOME station can actually smelt.
     *            Prevents a Prospect miner from filling its hold with
     *            cuprite (Prospect has no FURNACE_CU); same for a Helios
     *            miner grabbing ferrite ore.
     *   pass 2 — any ore. Fallback so a miner is never idle when there's
     *            ANY rock in range (e.g. early game with sparse spawns).
     * Two passes preserve the "nearest available" feel while preferring
     * useful loads. */
    const station_t *home = (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
                          ? &w->stations[npc->home_station]
                          : NULL;
    for (int pass = 0; pass < 2; pass++) {
        int best = -1;
        float best_d = 1e18f;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            const asteroid_t *a = &w->asteroids[i];
            if (!a->active || a->tier == ASTEROID_TIER_S) continue;
            if (signal_npc_confidence(signal_strength_at(w, a->pos)) < 0.1f) continue;
            if (miner_target_taken(w, i, self_char)) continue;
            if (pass == 0 && home && !sim_can_smelt_ore(home, a->commodity)) continue;
            float d = v2_dist_sq(npc->pos, a->pos);
            if (d < best_d) { best_d = d; best = i; }
        }
        if (best >= 0) return best;
    }
    return -1;
}

/* Forward decl — definition below; npc_steer_toward routes through it. */
static void npc_apply_flight_cmd(npc_ship_t *npc, flight_cmd_t cmd, float dt);

/* Direct face-and-thrust steering used by the MINING-approach state.
 * Produces a normalized flight_cmd_t and routes through
 * npc_apply_flight_cmd so all NPC physics goes through the same
 * sim_ship primitives as the path-following steer. */
static void npc_steer_toward(npc_ship_t *npc, vec2 target, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    vec2 delta = v2_sub(target, npc->pos);
    float desired = atan2f(delta.y, delta.x);
    float diff = wrap_angle(desired - npc->angle);
    float max_turn = hull->turn_speed * dt;
    flight_cmd_t cmd = {0.0f, 1.0f};
    if (max_turn > 0.0f) {
        float t = diff / max_turn;
        if (t > 1.0f) t = 1.0f;
        else if (t < -1.0f) t = -1.0f;
        cmd.turn = t;
    }
    npc_apply_flight_cmd(npc, cmd, dt);
}

/* (Reactive avoidance steering removed — all NPC/autopilot navigation
 * now uses A* paths via npc_steer_with_path. compute_path_avoidance
 * is retained for potential future use by manual-play collision hints.) */

/* (a) of #294 — load the npc duplicate physics fields into the
 * embedded ship_t before any sim_ship call, and write them back after.
 * The transient `ship_view_from_npc` is gone: helpers now mutate
 * `&npc->ship` directly. The duplicates stay live as a facade for the
 * AI dispatch and the save serializer until slice 5+ migrates every
 * reader to `npc->ship.*`. Read-only ship_t* views (e.g. for
 * flight_steer_to / flight_hover_near) also point at npc->ship after
 * the seed.
 *
 * Seed is idempotent and cheap (a 5-field copy), so callers can
 * sprinkle them defensively without worrying about stale state from a
 * previous helper that updated only the duplicates. */
static void npc_ship_seed(npc_ship_t *npc) {
    npc->ship.pos = npc->pos;
    npc->ship.vel = npc->vel;
    npc->ship.angle = npc->angle;
    npc->ship.hull_class = npc->hull_class;
}

static void npc_ship_sync(npc_ship_t *npc) {
    npc->pos = npc->ship.pos;
    npc->vel = npc->ship.vel;
    npc->angle = npc->ship.angle;
}

/* Apply a normalized flight_cmd_t (turn/thrust each in -1..1) to an NPC,
 * routed through the shared sim_ship primitives. Caller still owns
 * physics integration (npc_apply_physics) and any thrust<0 handling
 * (e.g. hover-specific brake-away-from-target).
 *
 * Forward-thrust-only gate: NPCs don't have a brake controller, so
 * flight_steer_to's negative-thrust "panic stop" clamps to 0. To
 * throttle the engine (hauler-tow paths used to pass hull->accel *
 * 0.6f), scale cmd.thrust before calling — thrust ∈ [-1,1] so this is
 * equivalent to the old accel multiplier. */
static void npc_apply_flight_cmd(npc_ship_t *npc, flight_cmd_t cmd, float dt) {
    npc_ship_seed(npc);
    step_ship_rotation(&npc->ship, dt, cmd.turn);

    float thrust_in = (cmd.thrust > 0.0f) ? cmd.thrust : 0.0f;
    vec2 fwd = ship_forward(npc->ship.angle);
    step_ship_thrust(&npc->ship, dt, thrust_in, fwd, /*boost=*/false, 0.0f);

    npc_ship_sync(npc);
    npc->thrusting = thrust_in > 0.0f;
}

/* A*-guided NPC steering via the shared flight controller. Creates a
 * temporary ship_t so flight_steer_to can read pos/vel/angle/hull_class
 * — slice 4 will fold ship_t into npc_ship_t and drop the view.
 *
 * `thrust_scale` ∈ (0,1] throttles forward thrust without changing the
 * controller's turn output; pass 1.0 for full engine, smaller values
 * for tow paths (was hull->accel * scale before). */
static void npc_steer_with_path(const world_t *w, int npc_idx, npc_ship_t *npc,
                                vec2 final_target, float thrust_scale, float dt) {
    npc_ship_seed(npc);
    nav_path_t *path = nav_npc_path(npc_idx);
    flight_cmd_t cmd = flight_steer_to(w, &npc->ship, path, final_target,
                                        0.0f, 200.0f, dt);
    cmd.thrust *= thrust_scale;
    npc_apply_flight_cmd(npc, cmd, dt);
}

/* Drag + position integration + signal-frontier pushback, routed
 * through step_ship_motion via the ship_view+writeback adapter. The
 * NPC confidence-graduated push that used to live here was retired in
 * favor of the player frontier yank — NPCs and players now share the
 * same boundary behavior. NPC willingness to *operate* at low signal
 * is still gated separately by signal_npc_confidence() in the AI brain
 * (mining target selection, willingness to leave the dock); only the
 * physics-side pushback was duplicated. */
static void npc_apply_physics(npc_ship_t *npc, float dt, const world_t *w) {
    npc_ship_seed(npc);
    float sig = signal_strength_at(w, npc->ship.pos);
    step_ship_motion(&npc->ship, dt, w, sig);
    npc_ship_sync(npc);
}


/* NPC circle pushback: routed through the shared sim_ship primitive
 * on the embedded ship_t. NPCs take no damage from station geometry
 * today; if that changes, the impact return is available. */
static void resolve_npc_circle(npc_ship_t *npc, vec2 center, float radius) {
    npc_ship_seed(npc);
    float impact = resolve_ship_circle_pushback(&npc->ship, center, radius);
    if (impact <= 0.0f) return;
    npc_ship_sync(npc);
}

/* NPC corridor collision: routed through the shared sim_ship
 * primitive on the embedded ship_t. Returns true on push so the
 * caller can force a nav replan. */
static bool resolve_npc_annular_sector(npc_ship_t *npc, vec2 center,
                                        float ring_r, float angle_a, float arc_delta) {
    npc_ship_seed(npc);
    float impact = resolve_ship_annular_pushback(&npc->ship, center, ring_r,
                                                  angle_a, arc_delta);
    if (impact <= 0.0f) return false;
    npc_ship_sync(npc);
    return true;
}

static void npc_resolve_station_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    float ship_r = hull->ship_radius;
    bool any_push = false;
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        if (!station_collides(st)) continue;

        station_geom_t geom;
        station_build_geom(st, &geom);

        /* Core: empty space, no collision */

        /* Module circles */
        for (int ci = 0; ci < geom.circle_count; ci++)
            resolve_npc_circle(npc, geom.circles[ci].center, geom.circles[ci].radius);

        /* Near-module suppression + corridor annular sectors
         * (matches player collision logic) */
        float npc_dist = sqrtf(v2_dist_sq(npc->pos, st->pos));
        vec2 npc_delta = v2_sub(npc->pos, st->pos);
        float npc_ang = atan2f(npc_delta.y, npc_delta.x);

        for (int ci = 0; ci < geom.corridor_count; ci++) {
            float ring_r = geom.corridors[ci].ring_radius;

            /* Check if NPC is near any module on this corridor's ring */
            bool near_module = false;
            if (fabsf(npc_dist - ring_r) < STATION_CORRIDOR_HW + ship_r + STATION_MODULE_COL_RADIUS) {
                for (int mi = 0; mi < geom.circle_count; mi++) {
                    if (geom.circles[mi].ring != geom.corridors[ci].ring) continue;
                    float ang_diff = wrap_angle(npc_ang - geom.circles[mi].angle);
                    float angular_size = (ring_r > 1.0f) ? (STATION_MODULE_COL_RADIUS + ship_r) / ring_r : 0.0f;
                    if (fabsf(ang_diff) < angular_size) {
                        near_module = true;
                        break;
                    }
                }
            }

            if (!near_module) {
                if (resolve_npc_annular_sector(npc, geom.center,
                        ring_r, geom.corridors[ci].angle_a, geom.corridors[ci].arc_delta)) {
                    any_push = true;
                }
            }
        }
    }
    /* Any corridor push means our cached A* path is leading us into
     * walls — force a replan so the next tick's flight_steer_to picks
     * a fresh route around the obstacle. Without this the NPC parks
     * against the wall, since path-following keeps pointing at the
     * same target and the forward-clearance brake bottoms out. */
    if (any_push) {
        int slot = (int)(npc - w->npc_ships);
        if (slot >= 0 && slot < MAX_NPC_SHIPS) {
            nav_force_replan(nav_npc_path(slot));
        }
    }
}

static void npc_resolve_asteroid_collisions(world_t *w, npc_ship_t *npc) {
    int towed = npc->towed_fragment;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (i == towed) continue;  /* tow physics owns this fragment */
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        /* Fragments (collectible-tier) collide too — a thrown or
         * tractored ore chunk should hit an NPC the same way it hits a
         * player. Geometry + mass-equal bounce live in sim_ship; only
         * NPC-specific damage routing layers on top. Unconditional
         * writeback so the geometric push-out lands even when the
         * contact is separating (impact=0, no damage but ship was
         * still moved out of overlap). */
        npc_ship_seed(npc);
        float impact = resolve_ship_asteroid_pushback(&npc->ship, a);
        npc_ship_sync(npc);
        if (impact <= 0.0f) continue;

        float size_mult = a->radius / 30.0f;
        if (size_mult < 0.5f) size_mult = 0.5f;
        if (size_mult > 2.5f) size_mult = 2.5f;
        float dmg = collision_damage_for(impact, size_mult);
        if (dmg <= 0.0f) continue;
        int npc_slot = (int)(npc - w->npc_ships);
        bool attributed =
            (a->last_towed_token[0] | a->last_towed_token[1] | a->last_towed_token[2] |
             a->last_towed_token[3] | a->last_towed_token[4] | a->last_towed_token[5] |
             a->last_towed_token[6] | a->last_towed_token[7]) != 0;
        if (attributed) {
            apply_npc_ship_damage_attributed(w, npc_slot, dmg,
                a->last_towed_token, DEATH_CAUSE_THROWN_ROCK);
        } else {
            apply_npc_ship_damage(w, npc_slot, dmg);
        }
    }
}

/* Find nearest active station with a dock module. Returns 0 as fallback. */
static int nearest_active_dock_station(const world_t *w, vec2 pos) {
    int best = 0;
    float best_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_is_active(&w->stations[s])) continue;
        if (!station_has_module(&w->stations[s], MODULE_DOCK)) continue;
        float d = v2_dist_sq(pos, w->stations[s].pos);
        if (d < best_d) { best_d = d; best = s; }
    }
    return best;
}

static void npc_validate_stations(world_t *w, npc_ship_t *npc) {
    if (npc->home_station < 0 || npc->home_station >= MAX_STATIONS ||
        !station_is_active(&w->stations[npc->home_station]))
        npc->home_station = nearest_active_dock_station(w, npc->pos);
    if (npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS)
        npc->dest_station = npc->home_station;
    /* Tow drones can deliver to planned stations (blueprints) which are
     * not active yet. Only reset dest for non-tow roles. */
    else if (npc->role != NPC_ROLE_TOW &&
             !station_is_active(&w->stations[npc->dest_station]))
        npc->dest_station = npc->home_station;
}

static void step_hauler(world_t *w, npc_ship_t *npc, int n, float dt) {
    const hull_def_t *hull = npc_hull_def(npc);
    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            station_t *home = &w->stations[npc->home_station];
            float carried = 0.0f;
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) carried += npc->cargo[c];
            float space = hull->ingot_capacity - carried;

            /* Contract-driven routing: find highest-value fillable
             * external contract. SKIP contracts whose station_index
             * is our own home — a self-delivery is zero-distance,
             * scores dist=1 = max possible price/dist, and would
             * always win the race. The bug it caused: Prospect
             * issues a P6 kit-import contract for itself; Prospect
             * haulers loaded local stock and "delivered" it back to
             * Prospect, never carrying ferrite ingots out to Kepler. */
            int best_contract = -1;
            float best_score = 0.0f;
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) continue;
                if (w->contracts[k].action != CONTRACT_TRACTOR) continue;
                if (w->contracts[k].station_index >= MAX_STATIONS) continue;
                if (w->contracts[k].station_index == npc->home_station) continue;
                commodity_t c = w->contracts[k].commodity;
                if (c < COMMODITY_RAW_ORE_COUNT) continue; /* haulers carry ingots only */
                if (home->_inventory_cache[c] < 0.5f) continue; /* no stock to fill */
                float dist = fmaxf(1.0f, v2_len(v2_sub(w->stations[w->contracts[k].station_index].pos, home->pos)));
                float score = contract_price(&w->contracts[k]) / dist;
                if (score > best_score) {
                    best_score = score;
                    best_contract = k;
                }
            }

            if (best_contract >= 0) {
                /* Load the commodity for this contract (leave reserve for players) */
                commodity_t ingot = w->contracts[best_contract].commodity;
                npc->dest_station = w->contracts[best_contract].station_index;
                float avail = fmaxf(0.0f, home->_inventory_cache[ingot] - HAULER_RESERVE);
                float take = fminf(avail, space);
                if (take > 0.5f) {
                    npc->cargo[ingot] += take;
                    home->_inventory_cache[ingot] -= take;
                    int whole = (int)floorf(take + 0.0001f);
                    if (whole > 0) {
                        if (station_manifest_drain_commodity(home, ingot, whole) > 0)
                            home->manifest_dirty = true;
                    }
                }
            } else {
                /* Fallback: original round-trip behavior (leave reserve for players) */
                station_t *dest = &w->stations[npc->dest_station];
                /* Sized for: FRAME_PRESS (FE) + LASER_FAB (CU+CR) +
                 * TRACTOR_FAB (CU). Original code had wants[3] which
                 * stack-overflowed when a station had FRAME_PRESS +
                 * LASER_FAB + TRACTOR_FAB (Kepler post-#367). */
                commodity_t wants[4];
                int want_count = 0;
                commodity_t best_ingot = COMMODITY_COUNT;
                float best_need = -1.0f;

                if (station_has_module(dest, MODULE_FRAME_PRESS))
                    wants[want_count++] = COMMODITY_FERRITE_INGOT;
                if (station_has_module(dest, MODULE_LASER_FAB)) {
                    wants[want_count++] = COMMODITY_CUPRITE_INGOT;
                    wants[want_count++] = COMMODITY_CRYSTAL_INGOT;
                }
                if (station_has_module(dest, MODULE_TRACTOR_FAB))
                    wants[want_count++] = COMMODITY_CUPRITE_INGOT;

                for (int wi = 0; wi < want_count; wi++) {
                    commodity_t ingot = wants[wi];
                    float avail = fmaxf(0.0f, home->_inventory_cache[ingot] - HAULER_RESERVE);
                    float need;
                    bool seen = false;

                    for (int wj = 0; wj < wi; wj++) {
                        if (wants[wj] == ingot) { seen = true; break; }
                    }
                    if (seen || avail <= 0.5f) continue;

                    need = fmaxf(0.0f, MAX_PRODUCT_STOCK * 0.5f - dest->_inventory_cache[ingot]);
                    if (need > best_need) {
                        best_need = need;
                        best_ingot = ingot;
                    }
                }

                /* If the previously-bound dest_station doesn't want any
                 * of home's stock, the original fallback would just sit
                 * the hauler in the dock. Bug surfaced as "Prospect is
                 * full of ferrite, haulers are idle" — Prospect's home
                 * had ingots but the stale dest_station (e.g. Prospect
                 * itself, or a station with no FRAME_PRESS) had no
                 * matching `wants[]`, so best_ingot stayed unset and
                 * total_carried fell to 0. Re-scan every other station
                 * and pick the best (surplus × need / dist) match. */
                if (best_ingot >= COMMODITY_COUNT) {
                    int best_alt_dest = -1;
                    commodity_t best_alt_ingot = COMMODITY_COUNT;
                    float best_alt_score = 0.0f;
                    for (int s = 0; s < MAX_STATIONS; s++) {
                        if (s == npc->home_station) continue;
                        if (!station_is_active(&w->stations[s])) continue;
                        const station_t *alt = &w->stations[s];
                        commodity_t alt_wants[4];
                        int alt_want_count = 0;
                        if (station_has_module(alt, MODULE_FRAME_PRESS))
                            alt_wants[alt_want_count++] = COMMODITY_FERRITE_INGOT;
                        if (station_has_module(alt, MODULE_LASER_FAB)) {
                            alt_wants[alt_want_count++] = COMMODITY_CUPRITE_INGOT;
                            alt_wants[alt_want_count++] = COMMODITY_CRYSTAL_INGOT;
                        }
                        if (station_has_module(alt, MODULE_TRACTOR_FAB))
                            alt_wants[alt_want_count++] = COMMODITY_CUPRITE_INGOT;
                        for (int wi = 0; wi < alt_want_count; wi++) {
                            commodity_t ingot = alt_wants[wi];
                            bool seen = false;
                            for (int wj = 0; wj < wi; wj++) {
                                if (alt_wants[wj] == ingot) { seen = true; break; }
                            }
                            if (seen) continue;
                            float avail = fmaxf(0.0f, home->_inventory_cache[ingot] - HAULER_RESERVE);
                            if (avail <= 0.5f) continue;
                            float need = fmaxf(0.0f, MAX_PRODUCT_STOCK * 0.5f - alt->_inventory_cache[ingot]);
                            if (need <= 0.0f) continue;
                            float dist = fmaxf(1.0f, v2_len(v2_sub(alt->pos, home->pos)));
                            float score = (avail * need) / dist;
                            if (score > best_alt_score) {
                                best_alt_score = score;
                                best_alt_dest = s;
                                best_alt_ingot = ingot;
                            }
                        }
                    }
                    if (best_alt_dest >= 0) {
                        npc->dest_station = best_alt_dest;
                        best_ingot = best_alt_ingot;
                    }
                }

                if (best_ingot < COMMODITY_COUNT) {
                    float avail = fmaxf(0.0f, home->_inventory_cache[best_ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->cargo[best_ingot] += take;
                        home->_inventory_cache[best_ingot] -= take;
                        int whole = (int)floorf(take + 0.0001f);
                        if (whole > 0) {
                            if (station_manifest_drain_commodity(home, best_ingot, whole) > 0)
                                home->manifest_dirty = true;
                        }
                    }
                }
            }
            float total_carried = 0.0f;
            for (int c = 0; c < COMMODITY_COUNT; c++) total_carried += npc->cargo[c];
            if (total_carried < 0.01f) {
                /* Nothing at home — relocate to a station with surplus ingots */
                int best_src = -1;
                float best_stock = 0.0f;
                for (int s = 0; s < MAX_STATIONS; s++) {
                    if (s == npc->home_station) continue;
                    if (!station_is_active(&w->stations[s])) continue;
                    float stock = 0.0f;
                    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                        stock += fmaxf(0.0f, w->stations[s]._inventory_cache[c] - HAULER_RESERVE);
                    if (stock > best_stock) { best_stock = stock; best_src = s; }
                }
                /* Stay docked at home and wait for stock or a contract.
                 * Prior to this, the fallback permanently mutated
                 * home_station to wherever had surplus, which caused
                 * every hauler in the world to converge on the
                 * highest-stock station (Helios) and the inter-station
                 * chain to permanently stall. Haulers belong to their
                 * spawn station; the auto-respawn loop replaces dead
                 * slots if a station's roster ever drops to zero. */
                (void)best_src; (void)best_stock;
                npc->state_timer = HAULER_DOCK_TIME;
            } else {
                npc->state = NPC_STATE_TRAVEL_TO_DEST;
            }
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        station_t *dest = &w->stations[npc->dest_station];
        vec2 approach = station_approach_target(dest, npc->pos);
        npc_steer_with_path(w, n, npc, approach, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        float dock_r = dest->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, dest->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(dest->pos, v2(30.0f * (float)(n % 2 == 0 ? -1 : 1), -(dest->radius + hull->ship_radius + 50.0f)));
            npc->state = NPC_STATE_UNLOADING;
            npc->state_timer = HAULER_LOAD_TIME;
        }
        break;
    }
    case NPC_STATE_UNLOADING: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer <= 0.0f) {
            station_t *dest = &w->stations[npc->dest_station];
            for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
                if (npc->cargo[i] <= 0.0f) continue;
                float delivered = npc->cargo[i];
                float before = dest->_inventory_cache[i];
                dest->_inventory_cache[i] += delivered;
                if (dest->_inventory_cache[i] > MAX_PRODUCT_STOCK)
                    dest->_inventory_cache[i] = MAX_PRODUCT_STOCK;
                /* Mirror the float bump into the manifest so the trade
                 * picker (manifest-only) sees the new stock. Use the
                 * post-clamp delta so overflow doesn't create phantom
                 * manifest entries. */
                int int_delta = (int)floorf(dest->_inventory_cache[i] + 0.0001f)
                              - (int)floorf(before + 0.0001f);
                if (int_delta > 0) {
                    if (station_manifest_seed_from_npc(dest, (commodity_t)i,
                                                       int_delta, n) > 0)
                        dest->manifest_dirty = true;
                }
                /* Pay the NPC for fulfilling a contract. Walk active
                 * TRACTOR contracts at this destination for the same
                 * commodity, prefer the highest contract_price. The
                 * full base_price (already pool-factor adjusted at
                 * issue time) goes into the NPC's ledger entry at the
                 * destination station — the hauler is now a real
                 * economic actor whose accumulated credits pay for
                 * its own dock-side repair-kit consumption. */
                float best_price = 0.0f;
                for (int k = 0; k < MAX_CONTRACTS; k++) {
                    const contract_t *ct = &w->contracts[k];
                    if (!ct->active) continue;
                    if (ct->action != CONTRACT_TRACTOR) continue;
                    if (ct->station_index != npc->dest_station) continue;
                    if (ct->commodity != (commodity_t)i) continue;
                    if (ct->base_price > best_price) best_price = ct->base_price;
                }
                if (best_price > 0.0f && delivered > 0.01f) {
                    ledger_earn_from_pool(dest, npc->session_token,
                                           best_price * delivered);
                }
                npc->cargo[i] = 0.0f;
            }
            /* Hauler also delivers ingots to scaffold station and modules */
            if (dest->scaffold || dest->module_count > 0) {
                /* Feed from station inventory into scaffolds */
                ship_t hauler_ship = {0};
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                    hauler_ship.cargo[c] = dest->_inventory_cache[c];
                if (dest->scaffold) {
                    float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - dest->scaffold_progress);
                    float deliver = fminf(hauler_ship.cargo[COMMODITY_FRAME], needed);
                    if (deliver > 0.01f) {
                        hauler_ship.cargo[COMMODITY_FRAME] -= deliver;
                        dest->scaffold_progress += deliver / SCAFFOLD_MATERIAL_NEEDED;
                        if (dest->scaffold_progress >= 1.0f)
                            activate_outpost(w, npc->dest_station);
                    }
                }
                step_module_delivery(w, dest, npc->dest_station, &hauler_ship, COMMODITY_COUNT);
                /* Put remaining back. The float was drained into
                 * module_input by step_module_delivery; we have to drain
                 * the matching manifest entries too or the BUY picker
                 * (manifest-only) will keep advertising stock that the
                 * server-side float check (game_sim.c try_buy_product)
                 * sees as 0 and silently rejects. */
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                    float consumed = dest->_inventory_cache[c] - hauler_ship.cargo[c];
                    if (consumed > 0.01f) {
                        dest->_inventory_cache[c] -= consumed;
                        int whole = (int)floorf(consumed + 0.0001f);
                        if (whole > 0) {
                            manifest_consume_by_commodity(&dest->manifest,
                                                          (commodity_t)c, whole);
                            dest->manifest_dirty = true;
                        }
                    }
                }
            }
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        vec2 approach_home = station_approach_target(home, npc->pos);
        npc_steer_with_path(w, n, npc, approach_home, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        float dock_r = home->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, home->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(home->pos, v2(50.0f * (float)(n % 2 == 0 ? -1 : 1), -(home->radius + hull->ship_radius + 70.0f)));
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            /* Dock auto-repair: NPC owes the home station for the
             * kits it consumes. Closed loop:
             *   1. NPC delivers a contract -> dest station credits its
             *      ledger from credit_pool (ledger_earn_from_pool).
             *   2. NPC docks at home with hull damage -> home applies
             *      kits up to (kits in stock, hull missing) and
             *      force-debits the NPC's ledger for the cost.
             *
             * Force-debit so a damaged hauler ALWAYS gets repaired
             * even if its balance is empty — the debt persists and
             * gets paid back as the hauler completes future contracts.
             * Otherwise a single bad scrape could permanently strand
             * a fresh drone with no income path. The home dock still
             * needs kits in stock; if not, no repair this cycle. */
            float max_h = npc_max_hull(npc);
            ship_t *ship = npc_ship_for(w, n);
            float cur_hull = ship ? ship->hull : npc->hull;
            if (cur_hull < max_h - 0.5f
                && station_has_module(home, MODULE_DOCK)) {
                int kits = (int)floorf(home->_inventory_cache[COMMODITY_REPAIR_KIT] + 0.0001f);
                int missing = (int)ceilf(max_h - cur_hull);
                int apply = kits < missing ? kits : missing;
                if (apply > 0) {
                    float kit_price = home->base_price[COMMODITY_REPAIR_KIT];
                    if (kit_price < 0.01f) kit_price = 6.0f;
                    float cost = (float)apply * kit_price;
                    /* Force-debit -> balance can go negative, station
                     * still gets credited. Hauler pays it back over
                     * subsequent deliveries. */
                    ledger_force_debit(home, npc->session_token, cost, ship);
                    home->_inventory_cache[COMMODITY_REPAIR_KIT] -= (float)apply;
                    if (home->_inventory_cache[COMMODITY_REPAIR_KIT] < 0.0f)
                        home->_inventory_cache[COMMODITY_REPAIR_KIT] = 0.0f;
                    if (manifest_consume_by_commodity(&home->manifest,
                                                     COMMODITY_REPAIR_KIT, apply) > 0)
                        home->manifest_dirty = true;
                    /* Write through ship layer; reverse-mirror at
                     * end of the NPC tick pushes the value back to
                     * npc->hull. */
                    if (ship) {
                        ship->hull += (float)apply;
                        if (ship->hull > max_h) ship->hull = max_h;
                    } else {
                        npc->hull += (float)apply;
                        if (npc->hull > max_h) npc->hull = max_h;
                    }
                }
            }
            /* Wear-and-tear maintenance: each home-dock visit consumes
             * one repair kit regardless of damage. This is the baseline
             * demand that keeps kits flowing through the economy --
             * without it, Prospect's kit shelf never drains (haulers
             * rarely take real damage), kit-import contracts never
             * trip, and the inter-station chain stalls. Force-debit so
             * the hauler is on the hook for upkeep just like a repair. */
            if (station_has_module(home, MODULE_DOCK)
                && home->_inventory_cache[COMMODITY_REPAIR_KIT] >= 1.0f) {
                float kit_price = home->base_price[COMMODITY_REPAIR_KIT];
                if (kit_price < 0.01f) kit_price = 6.0f;
                ledger_force_debit(home, npc->session_token, kit_price, ship);
                home->_inventory_cache[COMMODITY_REPAIR_KIT] -= 1.0f;
                if (manifest_consume_by_commodity(&home->manifest,
                                                  COMMODITY_REPAIR_KIT, 1) > 0)
                    home->manifest_dirty = true;
            }
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

/* Find an open ring slot at any active player outpost (s >= 3) that
 * matches the given module type. Used by tow drones to pick a delivery
 * destination for a loose scaffold. Returns -1 if none. */
static int find_destination_for_scaffold(const world_t *w, module_type_t type,
                                        int exclude_station) {
    /* Pass 1: outposts (active OR planned) with a placement plan for
     * this type — those are slots the player explicitly reserved. A
     * planned outpost is a valid destination too: when the scaffold
     * arrives the planned ghost can be promoted via the existing
     * snap-to-slot logic, with the relay as its founding module. */
    for (int s = 3; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            if (st->placement_plans[p].type == type) return s;
        }
    }
    /* Pass 2: any active outpost with at least one open ring slot. */
    for (int s = 3; s < MAX_STATIONS; s++) {
        if (s == exclude_station) continue;
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
            if (ring > 1 && !ring_has_dock(st, ring - 1)) continue;
            if (station_ring_free_slot(st, ring, STATION_RING_SLOTS[ring]) >= 0)
                return s;
        }
    }
    /* Pass 3: SIGNAL_RELAY is special — it founds new outposts. If the
     * player has a planned (ghost) outpost waiting, deliver the relay
     * there even without an explicit placement plan, so the chicken-
     * and-egg of "first relay needs an outpost that needs a relay" is
     * resolved by the drone. */
    if (type == MODULE_SIGNAL_RELAY) {
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (s == exclude_station) continue;
            const station_t *st = &w->stations[s];
            if (st->planned) return s;
        }
    }
    return -1;
}

/* Find a loose scaffold near this NPC's home station that has a known
 * destination. Returns scaffold index or -1. */
static int find_loose_scaffold_for_tow(const world_t *w, const npc_ship_t *npc) {
    const station_t *home = &w->stations[npc->home_station];
    const float pickup_range_sq = 4000.0f * 4000.0f;
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        if (sc->state != SCAFFOLD_LOOSE) continue;
        /* Skip scaffolds being towed by a player or another drone */
        if (sc->towed_by >= 0) continue;
        /* Must be near the home shipyard */
        float d_home = v2_dist_sq(sc->pos, home->pos);
        if (d_home > pickup_range_sq) continue;
        /* Must have a place to deliver (not back to home station) */
        if (find_destination_for_scaffold(w, sc->module_type, npc->home_station) < 0) continue;
        if (d_home < best_d) { best_d = d_home; best = i; }
    }
    return best;
}

/* Tow drone: spawned at shipyards, picks up loose scaffolds, delivers
 * them to player outposts with placement plans, returns home. Reuses
 * the existing NPC state enum but interprets the states for tow logic.
 *
 *   DOCKED → look for a loose scaffold + matching destination
 *   TRAVEL_TO_ASTEROID → fly to scaffold position (ASTEROID = "thing to grab")
 *   MINING → grab phase: tractor it, set towed_scaffold
 *   TRAVEL_TO_DEST → tow it to destination outpost
 *   UNLOADING → release near open slot, let it snap
 *   RETURN_TO_STATION → fly back to home shipyard
 */
static void step_tow_drone(world_t *w, npc_ship_t *npc, int n, float dt) {
    /* If we lost our towed scaffold mid-flight (destroyed, snapped early,
     * picked up by a player), drop back to idle. */
    if (npc->towed_scaffold >= 0) {
        scaffold_t *sc = &w->scaffolds[npc->towed_scaffold];
        if (!sc->active || sc->state == SCAFFOLD_PLACED ||
            sc->state == SCAFFOLD_SNAPPING || sc->towed_by != -2 - n) {
            npc->towed_scaffold = -1;
            if (npc->state == NPC_STATE_TRAVEL_TO_DEST ||
                npc->state == NPC_STATE_UNLOADING) {
                npc->state = NPC_STATE_RETURN_TO_STATION;
            }
        }
    }

    switch (npc->state) {
    case NPC_STATE_DOCKED: {
        npc->state_timer -= dt;
        npc->vel = v2(0.0f, 0.0f);
        if (npc->state_timer > 0.0f) break;
        int sc_idx = find_loose_scaffold_for_tow(w, npc);
        if (sc_idx < 0) {
            npc->state_timer = 2.0f; /* nothing to tow, idle and recheck */
            break;
        }
        npc->target_asteroid = sc_idx;  /* repurpose: scaffold idx for tow */
        npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
        break;
    }
    case NPC_STATE_TRAVEL_TO_ASTEROID: {
        if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_SCAFFOLDS) {
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
        }
        scaffold_t *sc = &w->scaffolds[npc->target_asteroid];
        if (!sc->active || sc->state != SCAFFOLD_LOOSE || sc->towed_by >= 0) {
            npc->target_asteroid = -1;
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
            break;
        }
        npc_steer_with_path(w, n, npc, sc->pos, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        if (v2_dist_sq(npc->pos, sc->pos) < 80.0f * 80.0f) {
            /* Grab — claim the scaffold and switch to tow mode.
             * Use towed_by = -2 - drone_index so positive values keep
             * meaning "player id" and negative values < -1 mean "drone n". */
            sc->towed_by = -2 - n;
            sc->state = SCAFFOLD_TOWING;
            npc->towed_scaffold = npc->target_asteroid;
            int dest = find_destination_for_scaffold(w, sc->module_type, npc->home_station);
            if (dest < 0) {
                /* Destination vanished while we were en route; drop and reset */
                sc->towed_by = -1;
                sc->state = SCAFFOLD_LOOSE;
                npc->towed_scaffold = -1;
                npc->target_asteroid = -1;
                npc->state = NPC_STATE_DOCKED;
                npc->state_timer = HAULER_DOCK_TIME;
                break;
            }
            npc->dest_station = dest;
            npc->state = NPC_STATE_TRAVEL_TO_DEST;
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        if (npc->towed_scaffold < 0 ||
            npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS) {
            npc->state = NPC_STATE_RETURN_TO_STATION;
            break;
        }
        scaffold_t *sc = &w->scaffolds[npc->towed_scaffold];
        station_t *dest = &w->stations[npc->dest_station];
        /* Drag the scaffold along behind us with simple spring chase. */
        vec2 to_drone = v2_sub(npc->pos, sc->pos);
        float td = sqrtf(v2_len_sq(to_drone));
        float tow_dist = 60.0f;
        if (td > tow_dist && td > 0.1f) {
            vec2 dir = v2_scale(to_drone, 1.0f / td);
            float over = td - tow_dist;
            sc->vel = v2_add(sc->vel, v2_scale(dir, over * 8.0f * dt));
        }
        sc->vel = v2_scale(sc->vel, 1.0f / (1.0f + 0.6f * dt));
        sc->pos = v2_add(sc->pos, v2_scale(sc->vel, dt));

        vec2 approach = station_approach_target(dest, npc->pos);
        npc_steer_with_path(w, n, npc, approach, /*thrust_scale=*/0.6f, dt);
        /* Speed cap while towing — heavy load */
        float spd = v2_len(npc->vel);
        if (spd > 60.0f) npc->vel = v2_scale(npc->vel, 60.0f / spd);
        npc_apply_physics(npc, dt, w);
        if (v2_dist_sq(npc->pos, dest->pos) < 600.0f * 600.0f) {
            /* Release — let the existing snap-to-slot logic in step_scaffolds
             * pick up the loose scaffold near the outpost ring. */
            sc->towed_by = -1;
            sc->state = SCAFFOLD_LOOSE;
            npc->towed_scaffold = -1;
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        vec2 approach = station_approach_target(home, npc->pos);
        npc_steer_with_path(w, n, npc, approach, /*thrust_scale=*/1.0f, dt);
        npc_apply_physics(npc, dt, w);
        if (v2_dist_sq(npc->pos, home->pos) < (home->dock_radius * 0.7f) * (home->dock_radius * 0.7f)) {
            npc->vel = v2(0.0f, 0.0f);
            npc->state = NPC_STATE_DOCKED;
            npc->state_timer = HAULER_DOCK_TIME;
        }
        break;
    }
    default:
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = HAULER_DOCK_TIME;
        break;
    }
}

/* Cooldown between auto-respawn attempts. 15 s feels recoverable
 * (full chain-wipe of 7 NPCs comes back over ~100 s) without making
 * the rocks-vs-NPC PvP feature feel toothless. */
#define NPC_RESPAWN_INTERVAL 15.0f

void step_npc_ships(world_t *w, float dt) {
    /* Replenish dead haulers/miners on a slow drip. The first call
     * after world_reset waits the full interval so the seeded roster
     * isn't immediately doubled. */
    if (w->npc_respawn_timer <= 0.0f) w->npc_respawn_timer = NPC_RESPAWN_INTERVAL;
    w->npc_respawn_timer -= dt;
    if (w->npc_respawn_timer <= 0.0f) {
        w->npc_respawn_timer = NPC_RESPAWN_INTERVAL;
        (void)replenish_npc_roster(w);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* Despawn-on-destroy: the spawn loop replaces dead slots on
         * the next tick. Cargo is lost (the chain takes a hit when a
         * loaded hauler dies — that's the cost of letting them get
         * smashed by asteroids). Read ship.hull (authoritative since
         * Slice 9-11) so external damage delivered between ticks via
         * apply_npc_ship_damage despawns immediately rather than
         * limping one extra tick. */
        const ship_t *paired_ship = npc_ship_for(w, n);
        float live_hull = paired_ship ? paired_ship->hull : npc->hull;
        if (live_hull <= 0.0f) {
            SIM_LOG("[npc] %d (role=%d) destroyed — hull 0\n", n, (int)npc->role);
            npc->active = false;
            character_free_for_npc(w, n);
            continue;
        }
        npc->thrusting = false;
        /* Slice 13: pull external ship.pos/vel/angle writes into the
         * npc fields before physics integration this tick. */
        mirror_ship_pos_to_npc(w, n);
        mirror_npc_to_character(w, n);
        npc_validate_stations(w, npc);

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            mirror_ship_to_npc(w, n);
            continue;
        }
        if (npc->role == NPC_ROLE_TOW) {
            step_tow_drone(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            mirror_ship_to_npc(w, n);
            continue;
        }

        const hull_def_t *hull = npc_hull_def(npc);
        switch (npc->state) {
        case NPC_STATE_DOCKED: {
            npc->state_timer -= dt;
            npc->vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
                /* Prefer towing a loose fragment over fracturing fresh
                 * rock — keeps the belt clean and is faster than mining.
                 * Generous range: a fresh-undock miner is willing to
                 * traverse the local sector for low-hanging fruit. */
                if (npc_try_claim_loose_fragment(w, npc, 4000.0f * 4000.0f)) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                /* Home hopper above 50%? Don't add more mass — IDLE and
                 * wait for fragments to drift through, or for the smelter
                 * to drain stock back below the threshold. */
                if (npc_home_ore_above_frac(w, npc, 0.5f)) {
                    npc->state = NPC_STATE_IDLE;
                    npc->state_timer = 5.0f;
                    break;
                }
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) {
                    npc->target_asteroid = target;
                    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                } else {
                    npc->state = NPC_STATE_IDLE;
                    npc->state_timer = 2.0f;
                }
            }
            break;
        }
        case NPC_STATE_TRAVEL_TO_ASTEROID: {
            if (!npc_target_valid(w, npc)) {
                /* Same fragment-first rule when the current target dies
                 * (someone else fractured it, etc.). */
                if (npc_try_claim_loose_fragment(w, npc, 4000.0f * 4000.0f)) {
                    npc->target_asteroid = -1;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                if (npc_home_ore_above_frac(w, npc, 0.5f)) {
                    npc->target_asteroid = -1;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) npc->target_asteroid = target;
                else { npc->target_asteroid = -1; npc->state = NPC_STATE_RETURN_TO_STATION; break; }
            }
            /* Pre-empt for FRACTURE distress: a stuck hauler may have
             * posted a distress contract since we left dock. Switch
             * only if the distress target is SIGNIFICANTLY closer than
             * our current target (≥50% nearer) — keeps the response
             * fast for genuine shortcut detours but prevents thrashing
             * mid-approach when the current target is close enough that
             * any FRACTURE rock looks "closer" each tick. */
            if (npc->towed_fragment < 0 && npc_target_valid(w, npc)) {
                vec2 cur_pos = w->asteroids[npc->target_asteroid].pos;
                float cur_d2 = v2_dist_sq(npc->pos, cur_pos);
                const float MAX_DISTRESS_PREEMPT_SQ = 2500.0f * 2500.0f;
                for (int k = 0; k < MAX_CONTRACTS; k++) {
                    if (!w->contracts[k].active) continue;
                    if (w->contracts[k].action != CONTRACT_FRACTURE) continue;
                    int idx = w->contracts[k].target_index;
                    if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
                    if (idx == npc->target_asteroid) break;
                    float new_d2 = v2_dist_sq(npc->pos, w->asteroids[idx].pos);
                    if (new_d2 > MAX_DISTRESS_PREEMPT_SQ) continue;
                    if (new_d2 < cur_d2 * 0.25f) { npc->target_asteroid = idx; break; }
                }
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            npc_steer_with_path(w, n, npc, a->pos, /*thrust_scale=*/1.0f, dt);
            npc_apply_physics(npc, dt, w);
            if (v2_dist_sq(npc->pos, a->pos) < MINING_RANGE * MINING_RANGE)
                npc->state = NPC_STATE_MINING;
            break;
        }
        case NPC_STATE_MINING: {
            if (!npc_target_valid(w, npc)) {
                /* Target gone — look for a fragment to tow, or find new target */
                if (npc->towed_fragment >= 0) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else if (npc_try_claim_loose_fragment(w, npc, 4000.0f * 4000.0f)) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else if (npc_home_ore_above_frac(w, npc, 0.5f)) {
                    /* Hopper full and no fragment in range — head home
                     * and IDLE there until the smelter drains. */
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else {
                    int target = npc_find_mineable_asteroid(w, npc);
                    if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                    else npc->state = NPC_STATE_RETURN_TO_STATION;
                }
                break;
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            float dist_sq = v2_dist_sq(npc->pos, a->pos);
            float standoff = a->radius + 60.0f;
            float approach_r = standoff + 20.0f;

            /* If we got shoved past mining range entirely (NPC↔NPC
             * collision, fracture knockback, gravity), drop back to
             * TRAVEL so the renderer doesn't keep drawing a beam to a
             * target we can't actually fire at. The MINING state is
             * for ships in firing position, not for "approaching from
             * across the map". */
            if (dist_sq > MINING_RANGE * MINING_RANGE) {
                npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                break;
            }

            if (dist_sq > approach_r * approach_r) {
                npc_steer_toward(npc, a->pos, dt);
                npc_apply_physics(npc, dt, w);
                break;
            }

            /* Hover near the rock via flight controller. The away-push
             * and extra damping below are role-specific overlays that
             * sit on top of shared sim_ship physics — same pattern as
             * player tow drag in game_sim.c. They're intentional, not a
             * leftover from the unification: hover wants different
             * brake semantics than flight_steer_to (push radially away
             * from the target, not reverse along velocity), and the 4.0
             * extra damping is what holds the standoff distance. */
            {
                npc_ship_seed(npc);
                flight_cmd_t cmd = flight_hover_near(w, &npc->ship, a->pos, standoff);
                if (cmd.thrust < 0.0f) {
                    vec2 away = v2_norm(v2_sub(npc->pos, a->pos));
                    npc->vel = v2_add(npc->vel, v2_scale(away, hull->accel * 0.5f * dt));
                    cmd.thrust = 0.0f;
                }
                npc_apply_flight_cmd(npc, cmd, dt);
                /* Hover never lights the engine flame — keep prior visual. */
                npc->thrusting = false;
            }
            npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (4.0f * dt)));
            npc_apply_physics(npc, dt, w);

            /* Strict range+cone gate before firing — same metric the player
             * uses. Without this, NPCs that get shoved (NPC↔NPC collision,
             * gravity, fracture knockback) used to keep the MINING state
             * and beam-render across the map. If we lost the firing line,
             * fall back to TRAVEL so steering pulls us back into range. */
            npc_ship_seed(npc);
            vec2 forward = v2_from_angle(npc->angle);
            vec2 muzzle = ship_muzzle(npc->pos, npc->angle, &npc->ship);
            int mining_level = (int)hull->mining_rate >= 1 ? 99 : 0; /* NPCs ignore tier */
            float sig_eff = signal_mining_efficiency(signal_strength_at(w, npc->pos));
            mining_beam_t mb = sim_mining_beam_step(w, muzzle, forward,
                npc->target_asteroid, mining_level,
                hull->mining_rate, sig_eff, /*fracturer*/ -1, dt);

            if (!mb.fired && !mb.fractured) {
                /* Out of range / cone — let TRAVEL re-acquire instead of
                 * sitting here lit up. Player path naturally re-acquires
                 * via cone search; NPC path uses target_asteroid + state. */
                if (dist_sq > MINING_RANGE * MINING_RANGE) {
                    npc->state = NPC_STATE_TRAVEL_TO_ASTEROID;
                }
                break;
            }

            if (mb.fractured) {
                npc->target_asteroid = -1;

                /* Grab the nearest S-tier fragment to tow home */
                float best_frag_d = 200.0f * 200.0f;
                int best_frag = -1;
                for (int fi = 0; fi < MAX_ASTEROIDS; fi++) {
                    asteroid_t *f = &w->asteroids[fi];
                    if (!f->active || f->tier != ASTEROID_TIER_S) continue;
                    float fd = v2_dist_sq(npc->pos, f->pos);
                    if (fd < best_frag_d) { best_frag_d = fd; best_frag = fi; }
                }
                if (best_frag >= 0) {
                    npc->towed_fragment = best_frag;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    /* Stamp the NPC's token onto the towed fragment so
                     * the eventual smelt-payout (ledger_credit_supply
                     * keyed off last_towed_token) credits the NPC's
                     * ledger at the home station. Same hook the player
                     * pickup uses — symmetrical economic identity. */
                    asteroid_t *frag = &w->asteroids[best_frag];
                    memcpy(frag->last_towed_token, npc->session_token,
                           sizeof(frag->last_towed_token));
                }
            }
            break;
        }
        case NPC_STATE_RETURN_TO_STATION: {
            station_t *home = &w->stations[npc->home_station];

            /* Find the nearest furnace on this station to deliver to */
            vec2 delivery_target = home->pos;
            for (int fm = 0; fm < home->module_count; fm++) {
                module_type_t fmt = home->modules[fm].type;
                if (fmt != MODULE_FURNACE) continue;
                if (home->modules[fm].scaffold) continue;
                delivery_target = module_world_pos_ring(home, home->modules[fm].ring, home->modules[fm].slot);
                break;
            }

            /* Slow down when towing so the fragment can keep up */
            float tow_thrust_scale = (npc->towed_fragment >= 0) ? 0.5f : 1.0f;
            npc_steer_with_path(w, n, npc, delivery_target, tow_thrust_scale, dt);
            npc_apply_physics(npc, dt, w);

            /* Speed cap when towing */
            if (npc->towed_fragment >= 0) {
                float spd = v2_len(npc->vel);
                float max_tow_speed = 80.0f;
                if (spd > max_tow_speed)
                    npc->vel = v2_scale(npc->vel, max_tow_speed / spd);
            }

            /* Tow the fragment — drag it along with spring physics */
            if (npc->towed_fragment >= 0 && npc->towed_fragment < MAX_ASTEROIDS) {
                asteroid_t *tow = &w->asteroids[npc->towed_fragment];
                if (tow->active) {
                    vec2 to_npc = v2_sub(npc->pos, tow->pos);
                    float td = sqrtf(v2_len_sq(to_npc));
                    float safe = 40.0f + tow->radius;
                    if (td > safe && td > 0.1f) {
                        vec2 pull_dir = v2_scale(to_npc, 1.0f / td);
                        tow->vel = v2_add(tow->vel, v2_scale(pull_dir, 500.0f * dt));
                        tow->vel = v2_scale(tow->vel, 1.0f / (1.0f + 3.0f * dt));
                        float spd = v2_len(tow->vel);
                        if (spd > 150.0f) tow->vel = v2_scale(tow->vel, 150.0f / spd);
                    }
                    /* Release when close to the furnace — let the furnace tractor take over */
                    float furnace_d = v2_dist_sq(tow->pos, delivery_target);
                    if (furnace_d < 150.0f * 150.0f) {
                        npc->towed_fragment = -1;
                    }
                } else {
                    npc->towed_fragment = -1;
                }
            }

            /* Once fragment is delivered (or lost), go find more ore */
            if (npc->towed_fragment < 0) {
                /* Drift away from the furnace, then look for next target */
                npc->state = NPC_STATE_IDLE;
                npc->state_timer = 2.0f;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_IDLE: {
            npc_apply_physics(npc, dt, w);
            npc->state_timer -= dt;
            if (npc->state_timer <= 0.0f) {
                /* IDLE → fragment first, fracture second. */
                if (npc_try_claim_loose_fragment(w, npc, 4000.0f * 4000.0f)) {
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                    break;
                }
                /* Hopper full → stay idle until either a fragment drifts
                 * into range or the smelter drains stock back below 50%. */
                if (npc_home_ore_above_frac(w, npc, 0.5f)) {
                    npc->state_timer = 5.0f;
                    break;
                }
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                else npc->state_timer = 3.0f;
            }
            break;
        }
        default: break;
        }

        /* Re-mirror after the dispatch wrote npc->target_asteroid /
         * state / etc., so the next miner processed in the same tick
         * sees fresh target contention via characters[]. */
        mirror_npc_to_character(w, n);

        /* NPC collision with stations and asteroids */
        if (npc->state != NPC_STATE_DOCKED) {
            npc_resolve_station_collisions(w, npc);
            npc_resolve_asteroid_collisions(w, npc);
        }
        /* Reverse-mirror ship -> npc after damage was applied through
         * the ship layer (#294 Slice 9). Keeps npc->hull authoritative
         * for the despawn check at the top of the next tick. */
        mirror_ship_to_npc(w, n);

        /* Blend tint toward dominant cargo color.
         * Ore colors: ferrite=(0.55, 0.25, 0.18), cuprite=(0.22, 0.30, 0.50), crystal=(0.25, 0.48, 0.30) */
        static const float ore_r[3] = {0.55f, 0.22f, 0.25f};
        static const float ore_g[3] = {0.25f, 0.30f, 0.48f};
        static const float ore_b[3] = {0.18f, 0.50f, 0.30f};
        float total = 0.0f;
        float target_r = 1.0f, target_g = 1.0f, target_b = 1.0f;
        {
            int base = (npc->role == NPC_ROLE_MINER) ? 0 : COMMODITY_RAW_ORE_COUNT;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) total += npc->cargo[base + c];
        }
        if (total > 1.0f) {
            target_r = 0.0f; target_g = 0.0f; target_b = 0.0f;
            int base = (npc->role == NPC_ROLE_MINER) ? 0 : COMMODITY_RAW_ORE_COUNT;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                float w_c = npc->cargo[base + c] / total;
                target_r += ore_r[c] * w_c;
                target_g += ore_g[c] * w_c;
                target_b += ore_b[c] * w_c;
            }
        }
        float blend = 0.3f * dt;  /* slow blend toward cargo color */
        npc->tint_r = lerpf(npc->tint_r, target_r, blend);
        npc->tint_g = lerpf(npc->tint_g, target_g, blend);
        npc->tint_b = lerpf(npc->tint_b, target_b, blend);
    }
}

/* Generate DESTROY contracts for asteroids blocking stuck NPCs. */
void generate_npc_distress_contracts(world_t *w) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        /* Only haulers in transit can get stuck */
        if (npc->role != NPC_ROLE_HAULER) continue;
        if (npc->state != NPC_STATE_TRAVEL_TO_DEST && npc->state != NPC_STATE_RETURN_TO_STATION) continue;
        /* Check if stuck: low speed for a while (state_timer repurposed — skip if fresh) */
        float speed = v2_len(npc->vel);
        if (speed > 15.0f) continue;
        /* Find nearest blocking asteroid */
        int blocker = -1;
        float best_d = 200.0f * 200.0f; /* within 200u */
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (!w->asteroids[i].active || asteroid_is_collectible(&w->asteroids[i])) continue;
            float d = v2_dist_sq(npc->pos, w->asteroids[i].pos);
            if (d < best_d) { best_d = d; blocker = i; }
        }
        if (blocker < 0) continue;
        /* Check if a DESTROY contract already exists for this asteroid */
        bool exists = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (w->contracts[k].active && w->contracts[k].action == CONTRACT_FRACTURE
                && w->contracts[k].target_index == blocker) {
                exists = true; break;
            }
        }
        if (exists) continue;
        /* Post distress contract */
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active) {
                w->contracts[k] = (contract_t){
                    .active = true, .action = CONTRACT_FRACTURE,
                    .station_index = (uint8_t)npc->home_station,
                    .target_pos = w->asteroids[blocker].pos,
                    .target_index = blocker,
                    .base_price = 20.0f, .age = 0.0f,
                    .claimed_by = -1,
                };
                break;
            }
        }
    }
}
