/*
 * game_sim.c -- Game simulation for Signal Space Miner.
 * Used by both the authoritative server and the client (local sim).
 * All rendering, audio, and sokol references are excluded.
 * Global state replaced with world_t *w and server_player_t *sp parameters.
 *
 * ⚠️  DO NOT MECHANICALLY SPLIT THIS FILE.  ⚠️
 *
 * Yes, this file is large (~5k LOC). The split is tracked as #272 slices
 * 2-6. Those slices are intentionally BLOCKED on the engine refactor in
 * #285 (streaming entity pool + persistent station catalog). Splitting
 * along the current data shape would lock in `MAX_STATIONS=8`-style
 * assumptions across six new files; every one of them would need to be
 * re-touched when #285 lands. The only universally-correct slice was
 * slice 1 (save/load → server/sim_save.c, commit 8611749), which doesn't
 * depend on the data shape. Until #285 Phase 3 lands, keep edits in this
 * file behind banner comments and resist the urge to extract.
 *
 * If you're reading this because the file is unwieldy: feature work that
 * touches `MAX_*` constants, `WORLD_RADIUS`, or the spatial grid IS a
 * slice of #285 in disguise — file it against #285, not as a refactor.
 */
#include "game_sim.h"
#include "tractor.h"
#include "laser.h"
#include "manifest.h"
#include "ship.h"
#include "sim_ai.h"
#include "sim_autopilot.h"
#include "sim_nav.h"
#include "sim_asteroid.h"
#include "sim_physics.h"
#include "sim_ship.h"
#include "sim_production.h"
#include "sim_construction.h"
#include "sim_mining.h"
#include "signal_model.h"
#include "rng.h"
#include "sha256.h"   /* signal_chain_hash_block */
#include "signal_crypto.h" /* Ed25519 verify for signed actions (#479 A.3) */
#include "station_authority.h" /* per-station Ed25519 identity (#479 B) */
#include "chain_log.h"         /* per-station signed event log (#479 C) */
#include "protocol.h"      /* NET_MSG_SIGNED_ACTION + signed_action_type_t */
#include <math.h>      /* isfinite for contract base_price sanity clamp */
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#else
#include <dirent.h>
#endif

/* SIM_LOG moved to game_sim.h so all sim_*.c files share the same macro. */

/* --- Station-local ledger economy ---
 * All credits are per-station. There is no global wallet.
 * Players earn by smelting/delivering at a station and spend at that station.
 * Cross-station wealth transfer requires physically hauling goods. */

/* Forward decl — definition below. The declaration in game_sim.h
 * makes this externally visible; the forward decl here is just so
 * the early helper code in this file can call it. */

/* Token-based ledger compatibility shim.
 *
 * #257 / #479-A.1 keys ledger entries by Ed25519 pubkey (32B). Pre-A.1
 * callers and pre-A.4 saves used 8B session tokens; some legacy paths
 * (NPC players, dev-mode anonymous play) still don't have a registered
 * pubkey at the call site. Rather than duplicate every helper, the
 * token-based functions construct a "pseudo-pubkey" by placing the
 * 8B token in the first 8 bytes of a 32B buffer and zero-filling the
 * rest. Real Layer-A.1 pubkeys are full Ed25519 public keys with
 * statistically zero chance of having 24 trailing zero bytes, so
 * pseudo-pubkeys and real pubkeys never collide. */
static void token_to_pseudo_pubkey(const uint8_t *token, uint8_t pseudo[32]) {
    memset(pseudo, 0, 32);
    if (token) memcpy(pseudo, token, 8);
}

float ledger_balance(const station_t *st, const uint8_t *token) {
    uint8_t pseudo[32];
    token_to_pseudo_pubkey(token, pseudo);
    return ledger_balance_by_pubkey(st, pseudo);
}

/* Net currency this station has issued, derived from the ledger
 * (single source of truth). Equal to -Σ(balance) over all entries.
 * A positive value means the station has more in player accounts
 * than has been redeemed — i.e. it's in net debt to its players.
 *
 * Was previously a stored `station_t::credit_pool` field with its
 * own +=/-= mutations paired with ledger writes. The field is gone;
 * conservation is structural now. */
float station_credit_pool(const station_t *st) {
    float total = 0.0f;
    for (int i = 0; i < st->ledger_count; i++) total += st->ledger[i].balance;
    return -total;
}

void ledger_earn(station_t *st, const uint8_t *token, float amount) {
    uint8_t pseudo[32];
    token_to_pseudo_pubkey(token, pseudo);
    ledger_earn_by_pubkey(st, pseudo, amount);
}

bool ledger_spend(station_t *st, const uint8_t *token, float amount, ship_t *ship) {
    uint8_t pseudo[32];
    token_to_pseudo_pubkey(token, pseudo);
    return ledger_spend_by_pubkey(st, pseudo, amount, ship);
}

/* Force a debit through even when the balance can't cover it. The
 * shortfall pushes the ledger into negative — the player owes the
 * station. Use for unrefusable services (spawn fee, mandatory repair)
 * where rejecting the spend would leave the ship in a worse state. */
void ledger_force_debit(station_t *st, const uint8_t *token, float amount, ship_t *ship) {
    uint8_t pseudo[32];
    token_to_pseudo_pubkey(token, pseudo);
    ledger_force_debit_by_pubkey(st, pseudo, amount, ship);
}

/* Full-price transfer from station to player ledger — used for
 * inter-station contract deliveries (no smelt cut, unlike
 * ledger_credit_supply). The credit appears on the player's ledger;
 * the station's derived pool decreases by the same amount. Caller is
 * responsible for contract bookkeeping. */
void ledger_earn_from_pool(station_t *st, const uint8_t *token, float amount) {
    /* Same shape as ledger_earn — full credit, no station cut. */
    ledger_earn(st, token, amount);
}

/* ---- PubKey-based ledger API (#257 #479) ---- */
/* New ledger functions that use player pubkey (32B) instead of session
 * token (8B). Relationships survive token rotation. */

float ledger_balance_by_pubkey(const station_t *st, const uint8_t pubkey[32]) {
    if (!pubkey) return 0.0f;
    for (int i = 0; i < st->ledger_count; i++)
        if (memcmp(st->ledger[i].player_pubkey, pubkey, 32) == 0)
            return st->ledger[i].balance;
    return 0.0f;
}

void ledger_earn_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount) {
    if (amount <= 0.0f) return;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return;
    st->ledger[idx].balance += amount;
    st->ledger[idx].lifetime_credits_in += (uint32_t)amount;
}

bool ledger_spend_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount, ship_t *ship) {
    if (amount <= 0.0f) return true;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return false;
    if (st->ledger[idx].balance + 0.01f < amount) return false;
    st->ledger[idx].balance -= amount;
    if (st->ledger[idx].balance < 0.0f) st->ledger[idx].balance = 0.0f;
    if (ship) ship->stat_credits_spent += amount;
    st->ledger[idx].lifetime_credits_out += (uint32_t)amount;
    return true;
}

void ledger_force_debit_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount, ship_t *ship) {
    if (amount <= 0.0f) return;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return;
    st->ledger[idx].balance -= amount;
    if (ship) ship->stat_credits_spent += amount;
    st->ledger[idx].lifetime_credits_out += (uint32_t)amount;
}

/* Smelt-payout credit. Station keeps a 35% cut, supplier gets 65%.
 * Returns the actual amount credited so callers can emit accurate +N
 * UI events. Pre-Layer-A.1 anonymous players (zero pubkey) are not
 * credited; the supplier-cut amount stays on the station's pool. */
float ledger_credit_supply_amount_by_pubkey(station_t *st, const uint8_t pubkey[32], float ore_value) {
    if (ore_value <= 0.0f) return 0.0f;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return 0.0f;
    /* Station keeps 35% cut for smelting — supplier gets 65% */
    float supplier_share = ore_value * 0.65f;
    if (supplier_share < 0.01f) return 0.0f;
    /* Pool is derived from -Σ(balance); crediting the supplier here
     * automatically pushes the station's net issuance more negative. */
    st->ledger[idx].balance += supplier_share;
    st->ledger[idx].lifetime_supply += ore_value;
    st->ledger[idx].lifetime_credits_in += (uint32_t)supplier_share;
    return supplier_share;
}

void ledger_credit_supply_by_pubkey(station_t *st, const uint8_t pubkey[32], float ore_value) {
    (void)ledger_credit_supply_amount_by_pubkey(st, pubkey, ore_value);
}

void ledger_record_ore_sold(station_t *st, const uint8_t pubkey[32], uint32_t ore_units, uint8_t commodity) {
    if (!pubkey) return;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return;
    st->ledger[idx].lifetime_ore_units += ore_units;
    /* Track the top commodity sold to this station */
    st->ledger[idx].top_commodity = commodity;
}

void ledger_record_dock(station_t *st, const uint8_t pubkey[32], uint64_t tick) {
    if (!pubkey) return;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return;
    /* Use total_docks==0 as the first-dock sentinel rather than
     * first_dock_tick==0: tick 0 is a valid initial-world-state
     * dock time, not a "no dock yet" marker. */
    if (st->ledger[idx].total_docks == 0) {
        st->ledger[idx].first_dock_tick = tick;
    }
    st->ledger[idx].last_dock_tick = tick;
    st->ledger[idx].total_docks++;
}

void emit_event(world_t *w, sim_event_t ev) {
    if (w->events.count < SIM_MAX_EVENTS) {
        w->events.events[w->events.count++] = ev;
    }
}

/* ================================================================== */
/* Hull definitions                                                   */
/* ================================================================== */

const hull_def_t HULL_DEFS[HULL_CLASS_COUNT] = {
    [HULL_CLASS_MINER] = {
        .name          = "Mining Cutter",
        .max_hull      = 100.0f,
        .accel         = 300.0f,
        .turn_speed    = 2.75f,
        .drag          = 0.45f,
        .cargo_capacity  = 24.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 28.0f,
        .tractor_range = 150.0f,
        .ship_radius   = 16.0f,
        .render_scale  = 1.0f,
    },
    [HULL_CLASS_HAULER] = {
        .name          = "Cargo Hauler",
        .max_hull      = 150.0f,
        .accel         = 140.0f,
        .turn_speed    = 1.6f,
        .drag          = 0.55f,
        .cargo_capacity  = 0.0f,
        .ingot_capacity= 40.0f,
        .mining_rate   = 0.0f,
        .tractor_range = 0.0f,
        .ship_radius   = 22.0f,
        .render_scale  = 1.15f,
    },
    [HULL_CLASS_NPC_MINER] = {
        .name          = "Mining Drone",
        .max_hull      = 80.0f,
        .accel         = 140.0f,
        .turn_speed    = 1.8f,
        .drag          = 0.5f,
        .cargo_capacity  = 16.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 8.0f,
        .tractor_range = 0.0f,
        .ship_radius   = 12.0f,
        .render_scale  = 0.7f,
    },
};

/* ================================================================== */
/* Math / utility                                                     */
/* ================================================================== */

/* ================================================================== */
/* RNG -- thin wrappers over shared rng.h (pass &w->rng)             */
/* ================================================================== */


/* w_randf, w_rand_range, w_rand_int — moved to sim_asteroid.c (local copies) */

/* ================================================================== */
/* Spatial grid helpers                                                */
/* ================================================================== */

/* Sparse spatial hash — no world bounds, heap-allocated */

static void spatial_grid_ensure(spatial_grid_t *g) {
    if (g->entries) return;
    g->capacity = SPATIAL_HASH_INITIAL_CAP;
    g->mask = g->capacity - 1;
    g->entries = (sparse_cell_entry_t *)calloc(g->capacity, sizeof(sparse_cell_entry_t));
    if (!g->entries) {
        /* OOM — leave the grid empty; callers (get_or_create, lookup,
         * insert) check for NULL entries. The asteroid grid will
         * silently degrade to "no spatial accel" rather than crash. */
        g->capacity = 0;
        g->mask = 0;
        g->occupied = 0;
        return;
    }
    for (uint32_t i = 0; i < g->capacity; i++)
        g->entries[i].key_x = INT32_MIN; /* empty sentinel */
    g->occupied = 0;
}

static void spatial_grid_clear(spatial_grid_t *g) {
    if (!g->entries) return;
    for (uint32_t i = 0; i < g->capacity; i++) {
        g->entries[i].key_x = INT32_MIN;
        g->entries[i].cell.count = 0;
    }
    g->occupied = 0;
}

static spatial_cell_t *spatial_grid_get_or_create(spatial_grid_t *g, int cx, int cy) {
    spatial_grid_ensure(g);
    if (!g->entries) return NULL; /* OOM — degrade gracefully */
    /* Mul in unsigned space — signed * 73856093 overflows for |cx| > 29 (UB). */
    uint32_t h = ((uint32_t)cx * 73856093u) ^ ((uint32_t)cy * 19349663u);
    for (uint32_t i = h & g->mask; ; i = (i + 1) & g->mask) {
        sparse_cell_entry_t *e = &g->entries[i];
        if (e->key_x == INT32_MIN) {
            e->key_x = cx;
            e->key_y = cy;
            e->cell.count = 0;
            g->occupied++;
            return &e->cell;
        }
        if (e->key_x == cx && e->key_y == cy) return &e->cell;
    }
}

static void spatial_grid_insert(spatial_grid_t *g, int idx, vec2 pos) {
    int cx, cy;
    spatial_grid_cell(g, pos, &cx, &cy);
    spatial_cell_t *cell = spatial_grid_get_or_create(g, cx, cy);
    if (!cell) return; /* OOM — see spatial_grid_ensure */
    if (cell->count < SPATIAL_MAX_PER_CELL) {
        cell->indices[cell->count++] = (int16_t)idx;
    }
}

void spatial_grid_build(world_t *w) {
    spatial_grid_t *g = &w->asteroid_grid;
    spatial_grid_ensure(g);
    spatial_grid_clear(g);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        spatial_grid_insert(g, i, w->asteroids[i].pos);
    }
}

/* ================================================================== */
/* Signal strength                                                    */
/* ================================================================== */

static void signal_grid_build(world_t *w); /* forward decl */

/*
 * Recompute signal_connected for all stations via flood-fill.
 * Root stations (indices 0-2, the built-in ones) are always connected.
 * An outpost is connected if its signal_range overlaps a connected station.
 */
void rebuild_signal_chain(world_t *w) {
    /* Reset all */
    for (int s = 0; s < MAX_STATIONS; s++)
        w->stations[s].signal_connected = false;

    /* Root stations (first 3) are always connected if active */
    for (int s = 0; s < 3 && s < MAX_STATIONS; s++) {
        if (station_is_active(&w->stations[s]))
            w->stations[s].signal_connected = true;
    }

    /* Flood-fill: keep scanning until no new connections found */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (w->stations[s].signal_connected) continue;
            if (!station_is_active(&w->stations[s])) continue;
            /* Check if this station is within the signal range of any connected station */
            for (int o = 0; o < MAX_STATIONS; o++) {
                if (!w->stations[o].signal_connected) continue;
                float dist_sq = v2_dist_sq(w->stations[s].pos, w->stations[o].pos);
                float range = w->stations[o].signal_range;
                if (dist_sq < range * range) {
                    w->stations[s].signal_connected = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    /* Rebuild the signal strength cache grid now that connectivity is settled. */
    signal_grid_build(w);
}

/* Unboosted signal — pure best-of per-station strength. Used for game
 * rules (outpost placement, planning) that don't want the overlap boost
 * to shrink the "fringe" where new stations can go. */
static float signal_strength_unboosted(const world_t *w, vec2 pos) {
    float best = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float dist = sqrtf(v2_dist_sq(pos, w->stations[s].pos));
        float strength = fmaxf(0.0f, 1.0f - (dist / w->stations[s].signal_range));
        if (strength > best) best = strength;
    }
    return best;
}

/* Raw signal computation — scans all stations. Used to build the cache
 * and as fallback for positions outside the cached grid.
 *
 * Overlap boost: when multiple connected stations cover the same position
 * their signal reinforces each other. The effective strength is the best
 * per-station strength multiplied by min(overlap_count, 3) and clamped to
 * 1.0. So two overlapping stations give a 2× boost (extending the reliable
 * band further out of each circle), three-or-more overlapping stations
 * cap at 3×, and additional stations past that don't stack further. A
 * station alone (count = 1) is unchanged. */
static float signal_strength_raw(const world_t *w, vec2 pos) {
    float best = 0.0f;
    int overlap_count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float dist = sqrtf(v2_dist_sq(pos, w->stations[s].pos));
        float strength = fmaxf(0.0f, 1.0f - (dist / w->stations[s].signal_range));
        if (strength > 0.0f) overlap_count++;
        if (strength > best) best = strength;
    }
    if (overlap_count <= 1) return best;
    int boost = overlap_count < 3 ? overlap_count : 3;
    return fminf(1.0f, best * (float)boost);
}

/* Build/rebuild the signal cache grid. Called after topology changes
 * (station activation, signal chain rebuild). O(GRID² × N_stations)
 * but runs infrequently — only on structural world changes. */
static void signal_grid_build(world_t *w) {
    signal_grid_t *sg = &w->signal_cache;
    if (!sg->strength) {
        sg->strength = (float *)calloc((size_t)SIGNAL_GRID_DIM * SIGNAL_GRID_DIM, sizeof(float));
        if (!sg->strength) return;
    }
    /* Center grid on station centroid so it covers the active network */
    float cx = 0.0f, cy = 0.0f;
    int n = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        cx += w->stations[s].pos.x;
        cy += w->stations[s].pos.y;
        n++;
    }
    if (n > 0) { cx /= (float)n; cy /= (float)n; }
    sg->offset_x = (SIGNAL_GRID_DIM * SIGNAL_CELL_SIZE) * 0.5f - cx;
    sg->offset_y = (SIGNAL_GRID_DIM * SIGNAL_CELL_SIZE) * 0.5f - cy;
    for (int y = 0; y < SIGNAL_GRID_DIM; y++) {
        for (int x = 0; x < SIGNAL_GRID_DIM; x++) {
            float wx = ((float)x + 0.5f) * SIGNAL_CELL_SIZE - sg->offset_x;
            float wy = ((float)y + 0.5f) * SIGNAL_CELL_SIZE - sg->offset_y;
            sg->strength[y * SIGNAL_GRID_DIM + x] = signal_strength_raw(w, v2(wx, wy));
        }
    }
    sg->valid = true;
}

/* O(1) signal lookup via cached grid with bilinear interpolation.
 * Falls back to raw computation for out-of-bounds positions or
 * when the cache hasn't been built yet. */
float signal_strength_at(const world_t *w, vec2 pos) {
    const signal_grid_t *sg = &w->signal_cache;
    if (!sg->valid || !sg->strength) return signal_strength_raw(w, pos);

    /* Map world position to continuous grid coordinate. */
    float gx = (pos.x + sg->offset_x) / SIGNAL_CELL_SIZE - 0.5f;
    float gy = (pos.y + sg->offset_y) / SIGNAL_CELL_SIZE - 0.5f;

    /* Bounds check — fall back to raw for positions outside the grid. */
    if (gx < 0.0f || gy < 0.0f ||
        gx >= (float)(SIGNAL_GRID_DIM - 1) || gy >= (float)(SIGNAL_GRID_DIM - 1))
        return signal_strength_raw(w, pos);

    /* Bilinear interpolation from the 4 nearest cell centers. */
    int x0 = (int)gx, y0 = (int)gy;
    float fx = gx - (float)x0, fy = gy - (float)y0;
    float s00 = sg->strength[y0 * SIGNAL_GRID_DIM + x0];
    float s10 = sg->strength[y0 * SIGNAL_GRID_DIM + x0 + 1];
    float s01 = sg->strength[(y0 + 1) * SIGNAL_GRID_DIM + x0];
    float s11 = sg->strength[(y0 + 1) * SIGNAL_GRID_DIM + x0 + 1];
    float top = s00 + (s10 - s00) * fx;
    float bot = s01 + (s11 - s01) * fx;
    return top + (bot - top) * fy;
}

/* ================================================================== */
/* Station construction                                               */
/* ================================================================== */

bool can_place_outpost(const world_t *w, vec2 pos) {
    /* Unboosted signal is the planning reference. The overlap boost
     * applies to player-facing signal quality; for placement it would
     * otherwise inflate the "settled ring" and shrink the fringe below
     * what this rule is trying to preserve. */
    float sig = signal_strength_unboosted(w, pos);
    if (sig <= 0.0f) return false;
    /* Must NOT be deep inside an existing station's coverage — forces
     * new outposts to the fringe so the network extends instead of
     * stacking on the starter ring. */
    if (sig >= OUTPOST_MAX_SIGNAL) return false;
    /* Must not overlap existing stations */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) continue;
        if (v2_dist_sq(pos, w->stations[s].pos) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) return false;
    }
    /* Must have a free station slot */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) return true;
    }
    return false;
}

/* add_module_at, activate_outpost, begin_module_construction*,
 * step_module_delivery, step_module_activation → sim_construction.c
 * module_build_material, module_build_cost, station_sells_scaffold
 *   → sim_construction.c / sim_construction.h */

static void step_scaffold_delivery(world_t *w, server_player_t *sp) {
    if (!sp->docked) return;
    station_t *st = &w->stations[sp->current_station];
    if (!st->scaffold) return;
    int held = ship_finished_count(&sp->ship, COMMODITY_FRAME);
    if (held <= 0) return;
    float needed_f = SCAFFOLD_MATERIAL_NEEDED * (1.0f - st->scaffold_progress);
    int needed = (int)ceilf(needed_f - 0.0001f);
    if (needed <= 0) return;
    int request = held < needed ? held : needed;
    int accepted = ship_finished_drain(&sp->ship, COMMODITY_FRAME, request);
    if (accepted <= 0) return;
    st->scaffold_progress += (float)accepted / SCAFFOLD_MATERIAL_NEEDED;
    SIM_LOG("[sim] player %d delivered %d frames to scaffold %d (progress %.0f%%)\n",
            sp->id, accepted, sp->current_station, st->scaffold_progress * 100.0f);
    if (st->scaffold_progress >= 1.0f) {
        activate_outpost(w, sp->current_station);
    }
}

/* Generate a frontier-flavored name from a world position hash.
 * Used by tow-founded outposts. */
static void generate_outpost_name(char *out, size_t out_size, vec2 pos, int slot) {
    static const char *prefixes[] = {
        "Far", "Deep", "Outer", "Edge", "Inner", "High", "Low", "Near",
        "Mid", "Upper", "Lower", "North", "South",
        "Void", "Drift", "Pale", "Dim", "Faint", "Thin", "Hollow",
        "Blank", "Null", "Silent", "Still", "Quiet", "Hush",
        "Iron", "Rust", "Ash", "Slag", "Ore", "Copper", "Tin",
        "Lead", "Salt", "Flint", "Basalt", "Granite", "Cobalt",
        "Carbon", "Nickel", "Sulfur", "Zinc",
        "Cold", "Dark", "Red", "Black", "Grey", "White", "Burnt",
        "Ember", "Cinder", "Frost", "Char",
        "Grim", "Last", "Lost", "Worn", "Lone", "Stark", "Bleak",
        "Gaunt", "Bare", "Stern", "Hard", "Grit", "Dusk", "Dawn",
        "Wane", "Rift", "Brink", "Fringe", "Verge", "Scarp",
        "Sol", "Arc", "Zenith", "Nadir", "Apex", "Nova", "Vega",
        "Polar", "Umbra", "Halo", "Corona", "Nebula",
        "Bolt", "Rivet", "Weld", "Truss", "Strut", "Keel",
        "Anvil", "Hammer", "Crucible",
    };
    static const char *suffixes[] = {
        "Reach", "Point", "Gate", "Rock", "Ridge", "Ledge",
        "Spur", "Pike", "Notch", "Gap", "Pass", "Shelf",
        "Rim", "Crest", "Bluff", "Mesa", "Knoll", "Butte",
        "Anchor", "Post", "Haven", "Hold", "Watch", "Keep",
        "Fort", "Camp", "Rest", "Berth", "Dock", "Pier",
        "Mooring", "Station", "Depot", "Outpost",
        "Forge", "Yard", "Works", "Mill", "Foundry", "Smelter",
        "Refinery", "Quarry", "Pit", "Mine", "Shaft", "Kiln",
        "Furnace", "Press", "Crucible",
        "Light", "Mark", "Beacon", "Signal", "Relay", "Spark",
        "Flare", "Pulse", "Lantern", "Lamp",
        "Well", "Spring", "Basin", "Cistern", "Trough",
        "Cairn", "Marker", "Waypoint", "Crossing", "Threshold",
        "Border", "Margin", "Line", "Terminus",
        "Hollow", "Shelter", "Cove", "Nook", "Pocket", "Nest",
    };
    enum { NUM_PREFIXES = sizeof(prefixes) / sizeof(prefixes[0]) };
    enum { NUM_SUFFIXES = sizeof(suffixes) / sizeof(suffixes[0]) };
    /* Use memcpy to bit-cast floats to uint32 — avoids UB from
     * negative float → unsigned int conversion. */
    float fx = pos.x * 7.13f, fy = pos.y * 13.37f;
    uint32_t hx, hy;
    memcpy(&hx, &fx, sizeof(hx));
    memcpy(&hy, &fy, sizeof(hy));
    uint32_t h = hx ^ hy ^ (uint32_t)slot;
    h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
    int pi = (int)(h % NUM_PREFIXES);
    int si = (int)((h >> 8) % NUM_SUFFIXES);
    snprintf(out, out_size, "%s %s", prefixes[pi], suffixes[si]);
}

/* point_within_signal_margin → sim_asteroid.c (local helper) */

/* ================================================================== */
/* Commodity / ship helpers                                           */
/* ================================================================== */

static void clear_ship_cargo(ship_t *s) {
    /* Wipe both the float side and the manifest. Without the manifest
     * reset, an emergency-recover would leave phantom cargo_unit_t
     * entries on the ship — the TRADE picker reads the manifest and
     * would still surface "SELL Iron ingot" rows even though cargo[]
     * was zero, so [3]/[4] would resolve to a row that produced
     * nothing on the server. */
    memset(s->cargo, 0, sizeof(s->cargo));
    s->manifest.count = 0;
    if (s->manifest.units && s->manifest.cap > 0)
        memset(s->manifest.units, 0, s->manifest.cap * sizeof(cargo_unit_t));
}

/* ================================================================== */
/* Station helpers                                                    */
/* ================================================================== */

/* Forward declarations for module-based docking */
static int station_dock_count(const station_t *st);
static int station_berth_count(const station_t *st);
static vec2 dock_berth_pos(const station_t *st, int berth);
static float dock_berth_angle(const station_t *st, int berth);
static int find_best_berth(const world_t *w, const station_t *st, int station_idx, vec2 ship_pos);

/* Repair pricing lives inline in try_repair_ship now. The standalone
 * helper used to be kept around speculatively but it's dead in every
 * TU and MSVC chokes on __attribute__((unused)) — drop it. Bring it
 * back as a real function the moment a caller materializes. */

/* Asteroid lifecycle, dynamics, fracture → sim_asteroid.c
 * sim_can_smelt_ore, sim_step_refinery_production, sim_step_station_production,
 * step_furnace_smelting, step_module_flow, step_module_delivery
 *   → sim_production.c */

/* Approach target: aim for the dock module on the side nearest `from`.
 * This puts the ship on a heading that naturally threads a dock opening
 * instead of flying into a corridor wall. Falls back to station center
 * if no dock module exists. */
vec2 station_approach_target(const station_t *st, vec2 from) {
    float best_d = 1e18f;
    vec2 best_pos = st->pos;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_DOCK) continue;
        if (st->modules[i].scaffold) continue;
        vec2 mp = module_world_pos_ring(st, st->modules[i].ring, st->modules[i].slot);
        vec2 outward = v2_sub(mp, st->pos);
        float len = v2_len(outward);
        if (len > 1.0f)
            mp = v2_add(mp, v2_scale(outward, 60.0f / len));
        float d = v2_dist_sq(from, mp);
        if (d < best_d) { best_d = d; best_pos = mp; }
    }
    return best_pos;
}

/* Exit target: pick the dock module nearest `from`, project a waypoint
 * past the outermost ring along that dock's current radial. NPCs (and
 * eventually player autopilot) steer through this waypoint as their
 * UNDOCKING phase, ensuring they clear ring-corridor obstacles before
 * heading to their real destination. */
vec2 station_exit_target(const station_t *st, vec2 from) {
    /* Push 160u past the outermost ring radius — comfortably clear of
     * the ring corridor band's lookahead cone (which fires at ring_r
     * +/- ship_radius + 40u). */
    const float exit_pad = STATION_RING_RADIUS[STATION_NUM_RINGS] + 160.0f;
    int best_i = -1;
    float best_d = 1e18f;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_DOCK) continue;
        if (st->modules[i].scaffold) continue;
        vec2 mp = module_world_pos_ring(st, st->modules[i].ring, st->modules[i].slot);
        float d = v2_dist_sq(from, mp);
        if (d < best_d) { best_d = d; best_i = i; }
    }
    if (best_i < 0) {
        /* No dock — emit a stable fallback past the outer ring. */
        return v2_add(st->pos, v2(exit_pad, 0.0f));
    }
    vec2 mp = module_world_pos_ring(st, st->modules[best_i].ring,
                                    st->modules[best_i].slot);
    vec2 outward = v2_sub(mp, st->pos);
    float len = v2_len(outward);
    if (len < 1.0f) return v2_add(st->pos, v2(exit_pad, 0.0f));
    return v2_add(st->pos, v2_scale(outward, exit_pad / len));
}

/* ================================================================== */
/* Player ship helpers                                                */
/* ================================================================== */

/* ship_forward, ship_muzzle: see ship.h/c */

/* try_spend_credits removed — all spending goes through ledger_spend */

void anchor_ship_in_station(server_player_t *sp, world_t *w) {
    const station_t *st = &w->stations[sp->current_station];
    /* Assign a dock berth and position ship there */
    int nberths = station_berth_count(st);
    if (nberths > 0) {
        sp->dock_berth = sp->id % nberths;
        sp->ship.pos = dock_berth_pos(st, sp->dock_berth);
        sp->ship.angle = dock_berth_angle(st, sp->dock_berth);
    } else {
        /* Fallback: no dock modules, park near center */
        const hull_def_t *hull = ship_hull_def(&sp->ship);
        sp->ship.pos = v2_add(st->pos, v2(0.0f, -(st->radius + hull->ship_radius + STATION_DOCK_APPROACH_OFFSET)));
    }
    sp->ship.vel = v2(0.0f, 0.0f);
}

static void apply_ship_damage(world_t *w, server_player_t *sp, float damage);
static void release_towed_scaffold(world_t *w, server_player_t *sp);
static bool find_nearest_open_slot(const station_t *st, vec2 pos, int *out_ring, int *out_slot);

static void dock_ship(world_t *w, server_player_t *sp) {
    if (sp->nearby_station >= 0) sp->current_station = sp->nearby_station;
    sp->docked = true;
    sp->in_dock_range = true;
    /* Release towed scaffold on dock — can't tow while docked */
    if (sp->ship.towed_scaffold >= 0) release_towed_scaffold(w, sp);
    /* Keep ship at its current position (already in dock range) — just stop it */
    sp->ship.vel = v2(0.0f, 0.0f);
    SIM_LOG("[sim] player %d docked at station %d\n", sp->id, sp->current_station);
    /* Track dock event for relationship data (#257). w->time is a
     * float — explicitly cast to the uint64_t tick parameter. */
    if (sp->current_station >= 0 && sp->pubkey_set) {
        ledger_record_dock(&w->stations[sp->current_station], sp->pubkey,
                            (uint64_t)w->time);
    }
    emit_event(w, (sim_event_t){.type = SIM_EVENT_DOCK, .player_id = sp->id});
}

static void launch_ship(world_t *w, server_player_t *sp) {
    sp->docked = false;
    sp->in_dock_range = false;
    sp->docking_approach = false;
    sp->nearby_station = -1;
    /* Kick ship away from station so it clears dock range */
    const station_t *st = &w->stations[sp->current_station];
    vec2 away = v2_sub(sp->ship.pos, st->pos);
    float len = sqrtf(v2_len_sq(away));
    if (len > 1.0f) {
        sp->ship.vel = v2_scale(away, 40.0f / len);
    } else {
        sp->ship.vel = v2(0.0f, -40.0f);
    }
    /* First launch: "Hull integrity 94%" */
    if (sp->ship.stat_ore_mined < 0.01f && sp->ship.stat_credits_earned < 0.01f)
        sp->ship.hull = ship_max_hull(&sp->ship) * 0.94f;
    SIM_LOG("[sim] player %d launched\n", sp->id);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_LAUNCH, .player_id = sp->id});
}

static void emergency_recover_ship(world_t *w, server_player_t *sp) {
    /* Pick respawn station first so the death event can name it for the
     * client overlay ("respawn -300 Helios credits"). */
    int best = 0;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_exists(&w->stations[i])) continue;
        float d = v2_dist_sq(sp->ship.pos, w->stations[i].pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    /* Charge the spawn fee against THAT station's ledger. Force-debit so
     * a bankrupt player still gets a ship — the negative balance becomes
     * the next-run mining target, which is the whole point of the debt
     * loop. Unlike player_seed_credits, this fires on EVERY respawn so
     * the cost of dying is visible and recurring. Identity-aware:
     * registered players debit their pubkey entry (the same one that
     * carries their earnings); legacy players use session-token. */
    int fee = station_spawn_fee(&w->stations[best]);
    if (sp->pubkey_set) {
        ledger_force_debit_by_pubkey(&w->stations[best], sp->pubkey,
                                     (float)fee, &sp->ship);
    } else {
        ledger_force_debit(&w->stations[best], sp->session_token,
                           (float)fee, &sp->ship);
    }

    sim_event_t death_ev = {
        .type = SIM_EVENT_DEATH, .player_id = sp->id,
        .death = {
            .ore_mined = sp->ship.stat_ore_mined,
            .credits_earned = sp->ship.stat_credits_earned,
            .credits_spent = sp->ship.stat_credits_spent,
            .asteroids_fractured = sp->ship.stat_asteroids_fractured,
            .pos_x = sp->ship.pos.x,
            .pos_y = sp->ship.pos.y,
            .vel_x = sp->ship.vel.x,
            .vel_y = sp->ship.vel.y,
            .angle = sp->ship.angle,
            .cause = sp->last_damage_cause,
            .respawn_station = (uint8_t)best,
            .respawn_fee = (float)fee,
        }
    };
    memcpy(death_ev.death.killer_token, sp->last_damage_killer_token, 8);
    emit_event(w, death_ev);
    /* Reset attribution for next life. */
    memset(sp->last_damage_killer_token, 0, 8);
    sp->last_damage_cause = DEATH_CAUSE_UNKNOWN;
    clear_ship_cargo(&sp->ship);
    /* Release towed fragments */
    sp->ship.towed_count = 0;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.angle = PI_F * 0.5f;
    sp->ship.stat_ore_mined = 0.0f;
    sp->ship.stat_credits_earned = 0.0f;
    sp->ship.stat_credits_spent = 0.0f;
    sp->ship.stat_asteroids_fractured = 0;
    /* Reset upgrades on death -- ship comes back stock. The modules
     * (laser/hold/tractor) the player accumulated are part of the
     * progression loop they need to re-earn through trade. */
    sp->ship.mining_level  = 0;
    sp->ship.hold_level    = 0;
    sp->ship.tractor_level = 0;
    sp->current_station = best;
    sp->nearby_station = best;
    sp->dock_berth = 0;
    sp->ship.pos = dock_berth_pos(&w->stations[best], 0);
    dock_ship(w, sp);
    SIM_LOG("[sim] player %d emergency recovered at station %d (fee %d)\n",
            sp->id, best, fee);
}

/* Apply hull damage with optional kill attribution. killer_token=NULL or
 * a zero-byte token means unattributed (environmental). cause is one of
 * death_cause_t — defaults to DEATH_CAUSE_UNKNOWN if zeroes. The
 * attribution is stored on the player so the eventual SIM_EVENT_DEATH
 * fires with the correct killer/cause even if the lethal blow lands
 * several ticks after the ramp-down begins. */
static void apply_ship_damage_attributed(world_t *w, server_player_t *sp, float damage,
                                          const uint8_t killer_token[8], uint8_t cause,
                                          vec2 source) {
    if (damage <= 0.0f) return;
    sp->ship.hull = fmaxf(0.0f, sp->ship.hull - damage);
    /* Record attribution if this hit is non-environmental, OR if no
     * prior attribution exists (so the FIRST cause sticks). Don't
     * overwrite an already-attributed killer. */
    bool has_attribution = (killer_token != NULL) &&
        (killer_token[0] | killer_token[1] | killer_token[2] | killer_token[3] |
         killer_token[4] | killer_token[5] | killer_token[6] | killer_token[7]) != 0;
    if (has_attribution) {
        memcpy(sp->last_damage_killer_token, killer_token, 8);
        sp->last_damage_cause = cause;
    } else if (sp->last_damage_cause == DEATH_CAUSE_UNKNOWN) {
        sp->last_damage_cause = cause;
    }
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_DAMAGE, .player_id = sp->id,
        .damage = { .amount = damage, .source_x = source.x, .source_y = source.y },
    });
    if (sp->ship.hull <= 0.01f) emergency_recover_ship(w, sp);
}

static void apply_ship_damage(world_t *w, server_player_t *sp, float damage) {
    /* Environmental, unsourced — caller doesn't know where the hit
     * came from. Client treats source = (0,0) as "unknown" and skips
     * the directional indicator. */
    apply_ship_damage_attributed(w, sp, damage, NULL, DEATH_CAUSE_ASTEROID, v2(0.0f, 0.0f));
}

/* ================================================================== */
/* Ship collision                                                     */
/* ================================================================== */

static int ship_collision_count; /* per-frame overlap counter for crush detection */

static void resolve_ship_circle(world_t *w, server_player_t *sp, vec2 center, float radius) {
    float impact = resolve_ship_circle_pushback(&sp->ship, center, radius);
    if (impact > 0.0f) ship_collision_count++;
    if (impact <= 0.0f || sp->docked) return;
    float dmg = collision_damage_for(impact, 1.0f);
    if (dmg > 0.0f) {
        /* Source = the offending station-module circle. Player's
         * directional indicator points at the wall they hit. */
        apply_ship_damage_attributed(w, sp, dmg, NULL, DEATH_CAUSE_STATION, center);
    }
}

/* Asteroid-vs-ship collision with relative velocity, kill attribution,
 * and size-scaled damage.
 *
 *   1. Damage uses |rel_vel . normal| (not just ship.vel) so a stationary
 *      ship hit by a fast rock takes the right impact.
 *   2. last_towed_token attributes the kill to the player who threw the
 *      rock; self-damage is suppressed so your own thrown rocks don't
 *      kill you on the rebound.
 *   3. Damage scales with rock radius. An XL rock hits ~2.5× harder
 *      than an S-tier fragment. Free signal that bigger rocks matter. */
static void resolve_ship_asteroid_collision(world_t *w, server_player_t *sp, asteroid_t *a) {
    /* Geometric push-out + mass-equal bounce live in sim_ship now;
     * player-only attribution / self-damage suppression sits on top. */
    float impact = resolve_ship_asteroid_pushback(&sp->ship, a);
    if (impact <= 0.0f) return;
    ship_collision_count++;

    /* Self-damage skip: your own thrown rock can't hurt you. The
     * pushback already resolved geometrically — we just gate the
     * damage / kill credit. */
    bool attributed =
        (a->last_towed_token[0] | a->last_towed_token[1] | a->last_towed_token[2] |
         a->last_towed_token[3] | a->last_towed_token[4] | a->last_towed_token[5] |
         a->last_towed_token[6] | a->last_towed_token[7]) != 0;
    bool self = attributed && memcmp(a->last_towed_token, sp->session_token, 8) == 0;
    if (self) return;

    /* Size scaling: S-tier (~10) → 0.5×, M (~30) → 1.0×, XL (~60) →
     * ~2.0×, XXL (~80) → 2.5× cap. Free signal that bigger rocks
     * matter. */
    float size_mult = a->radius / 30.0f;
    if (size_mult < 0.5f) size_mult = 0.5f;
    if (size_mult > 2.5f) size_mult = 2.5f;
    float dmg = sp->docked ? 0.0f : collision_damage_for(impact, size_mult);
    if (dmg > 0.0f) {
        uint8_t cause = attributed ? DEATH_CAUSE_THROWN_ROCK : DEATH_CAUSE_ASTEROID;
        /* Source = rock position so the indicator points at the actual
         * incoming projectile, not the thrower. */
        apply_ship_damage_attributed(w, sp, dmg,
            attributed ? a->last_towed_token : NULL, cause, a->pos);
    }
}

/* Player corridor collision: shared annular pushback in sim_ship,
 * then apply player-only damage on top of the impact magnitude. */
static void resolve_ship_annular_sector(world_t *w, server_player_t *sp,
                                         vec2 center, float ring_r,
                                         float angle_a, float arc_delta) {
    float impact = resolve_ship_annular_pushback(&sp->ship, center, ring_r,
                                                  angle_a, arc_delta);
    if (impact <= 0.0f) return;
    if (sp->docked) return;
    float dmg = collision_damage_for(impact, 1.0f);
    if (dmg > 0.0f) apply_ship_damage(w, sp, dmg);
}

/* ================================================================== */
/* Mining target                                                      */
/* ================================================================== */

/* Max asteroid tier mineable at each laser level:
 * Level 0: M, Level 1: L, Level 2: XL, Level 3: XXL, Level 4: all */
asteroid_tier_t max_mineable_tier(int mining_level) {
    /* Tier enum is inverted: TIER_XXL=0 (toughest) → TIER_S=4 (softest).
     * Post-#285 belt is denser near spawn, so level-0 miners kept hitting
     * L rocks that showed the beam but did no damage ("laser broken").
     * Starter laser now mines L-and-softer so the common belt rock is
     * always a valid target; upgrades unlock XL, XXL. */
    switch (mining_level) {
        case 0: return ASTEROID_TIER_L;
        case 1: return ASTEROID_TIER_XL;
        default: return ASTEROID_TIER_XXL;
    }
}

static bool hinted_target_in_mining_cone(vec2 muzzle, vec2 forward, const asteroid_t *a) {
    /* Multiplayer clients render asteroid positions slightly behind the
     * authoritative server. Give explicit target hints a small amount of
     * aim slack so fast-moving fracture shards still mine when the intent is
     * clear, without relaxing general fallback targeting. */
    const float aim_slack = 12.0f;
    vec2 to_a = v2_sub(a->pos, muzzle);
    float proj = v2_dot(to_a, forward);
    float perp = fabsf(v2_cross(to_a, forward));
    float effective_radius = a->radius + aim_slack;
    return perp <= effective_radius
        && proj >= -effective_radius
        && proj <= MINING_RANGE + effective_radius;
}

static int sim_find_mining_target(const world_t *w, vec2 origin, vec2 forward, int mining_level) {
    (void)mining_level; /* tier check moved to damage step */
    int best = -1;
    float best_dist = MINING_RANGE + 1.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || asteroid_is_collectible(a)) continue;
        vec2 to_a = v2_sub(a->pos, origin);
        float proj = v2_dot(to_a, forward);
        float perp = fabsf(v2_cross(to_a, forward));
        /* Ray-circle intersection: ray hits if perpendicular distance < radius */
        if (perp > a->radius) continue;
        /* Distance to surface along the ray (not center) */
        float surface_dist = proj - sqrtf(fmaxf(0.0f, a->radius * a->radius - perp * perp));
        if (surface_dist < -a->radius) continue; /* behind us */
        if (surface_dist > MINING_RANGE) continue; /* too far */
        /* Pick closest surface hit */
        float hit_dist = fmaxf(0.0f, surface_dist);
        if (hit_dist < best_dist) { best_dist = hit_dist; best = i; }
    }
    return best;
}

/* ================================================================== */
/* Station interactions                                               */
/* ================================================================== */

/* ledger_credit_supply declared in game_sim.h */

/* Transfer `n` manifest units of `commodity` from `src` to `dst`.
 *
 * `preferred_grade`: if MINING_GRADE_COUNT (sentinel) → FIFO across any
 * grade. Otherwise the helper first picks units of exactly that grade;
 * once those are exhausted (or if there were none to begin with) it
 * falls through to any-grade FIFO.
 *
 * Returns the number actually transferred. The cargo_unit_t (pub +
 * grade + parent_merkle) moves to the receiving side; callers sync any
 * derived float caches from the manifest after a successful transfer.
 *
 * Conservation invariant (see tests): at any point,
 *   count_in_any_manifest(pub) == 1
 * manifest_remove + manifest_push with the same `unit` value satisfies
 * this because we push a copy of the removed value, not allocate. */
static int manifest_transfer_by_commodity_ex(manifest_t *src, manifest_t *dst,
                                             commodity_t commodity,
                                             mining_grade_t preferred_grade,
                                             int n) {
    if (!src || !dst || n <= 0) return 0;
    int moved = 0;
    bool allow_any_grade = (preferred_grade >= MINING_GRADE_COUNT);
    while (moved < n) {
        int idx = -1;
        if (!allow_any_grade) {
            idx = manifest_find_first_cg(src, commodity, preferred_grade);
        }
        if (idx < 0) {
            /* Exhausted preferred grade (or none requested) — fall back
             * to the first matching commodity, any grade. */
            for (uint16_t i = 0; i < src->count; i++) {
                if (src->units[i].commodity == (uint8_t)commodity) { idx = (int)i; break; }
            }
        }
        if (idx < 0) break;
        cargo_unit_t unit;
        if (!manifest_remove(src, (uint16_t)idx, &unit)) break;
        if (!manifest_push(dst, &unit)) {
            /* dst full — put it back so the invariant holds. */
            (void)manifest_push(src, &unit);
            break;
        }
        moved++;
    }
    return moved;
}

static bool manifest_unit_is_named_ingot(const cargo_unit_t *u) {
    return u && (cargo_kind_t)u->kind == CARGO_KIND_INGOT &&
           (ingot_prefix_t)u->prefix_class != INGOT_PREFIX_ANONYMOUS;
}

static bool manifest_unit_matches_market_buy(const cargo_unit_t *u,
                                             commodity_t commodity,
                                             mining_grade_t preferred_grade) {
    if (!u || u->commodity != (uint8_t)commodity) return false;
    if (preferred_grade < MINING_GRADE_COUNT &&
        u->grade != (uint8_t)preferred_grade) return false;
    return !manifest_unit_is_named_ingot(u);
}

static int manifest_count_market_buy_units(const manifest_t *manifest,
                                           commodity_t commodity,
                                           mining_grade_t preferred_grade) {
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        if (manifest_unit_matches_market_buy(&manifest->units[i],
                                             commodity, preferred_grade)) n++;
    }
    return n;
}

static int manifest_find_market_buy_unit(const manifest_t *manifest,
                                         commodity_t commodity,
                                         mining_grade_t preferred_grade) {
    if (!manifest || !manifest->units) return -1;
    for (uint16_t i = 0; i < manifest->count; i++) {
        if (manifest_unit_matches_market_buy(&manifest->units[i],
                                             commodity, preferred_grade))
            return (int)i;
    }
    return -1;
}

static int manifest_transfer_market_buy(manifest_t *src, manifest_t *dst,
                                        commodity_t commodity,
                                        mining_grade_t preferred_grade,
                                        int n) {
    if (!src || !dst || n <= 0) return 0;
    int moved = 0;
    while (moved < n) {
        int idx = manifest_find_market_buy_unit(src, commodity, preferred_grade);
        if (idx < 0) break;
        cargo_unit_t unit;
        if (!manifest_remove(src, (uint16_t)idx, &unit)) break;
        if (!manifest_push(dst, &unit)) {
            (void)manifest_push(src, &unit);
            break;
        }
        moved++;
    }
    return moved;
}

static int manifest_find_top_sell_unit(const manifest_t *manifest,
                                       commodity_t commodity,
                                       mining_grade_t grade) {
    if (!manifest || !manifest->units) return -1;
    int top_idx = -1;
    float top_mult = 1.0f;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (u->commodity != (uint8_t)commodity) continue;
        if (grade < MINING_GRADE_COUNT && u->grade != (uint8_t)grade) continue;
        float mult = prefix_class_price_multiplier((int)u->prefix_class);
        if (top_idx < 0 || mult > top_mult) {
            top_idx = (int)i;
            top_mult = mult;
        }
    }
    return top_idx;
}

/* Backwards-compatible wrapper — any-grade transfer (what Phase 1 used). */
static int manifest_transfer_by_commodity(manifest_t *src, manifest_t *dst,
                                          commodity_t commodity, int n) {
    return manifest_transfer_by_commodity_ex(src, dst, commodity,
                                              MINING_GRADE_COUNT, n);
}

static bool is_finished_good(commodity_t c) {
    return c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT;
}

static float station_finished_fraction(const station_t *st, commodity_t c) {
    if (!st || !is_finished_good(c)) return 0.0f;
    float v = st->_inventory_cache[c];
    float floor_v = floorf(v + 0.0001f);
    float frac = v - floor_v;
    if (frac < 0.0f || frac >= 1.0f) frac = 0.0f;
    return frac;
}

static float station_finished_space(const station_t *st, commodity_t c) {
    if (!st || !is_finished_good(c)) return 0.0f;
    float stock = (float)manifest_count_by_commodity(&st->manifest, c) +
                  station_finished_fraction(st, c);
    return fmaxf(0.0f, MAX_PRODUCT_STOCK - stock);
}

static void sync_ship_finished_cargo(ship_t *ship, commodity_t c) {
    if (!ship || !is_finished_good(c)) return;
    ship->cargo[c] = (float)manifest_count_by_commodity(&ship->manifest, c);
}

static void sync_station_finished_inventory(station_t *st, commodity_t c) {
    if (!st || !is_finished_good(c)) return;
    st->_inventory_cache[c] =
        (float)manifest_count_by_commodity(&st->manifest, c) +
        station_finished_fraction(st, c);
    st->manifest_dirty = true;
}

static float manifest_grade_bonus_from_range(const manifest_t *manifest,
                                             uint16_t start,
                                             commodity_t commodity,
                                             float price_per) {
    if (!manifest || start >= manifest->count || price_per <= 0.0f) return 0.0f;
    float bonus = 0.0f;
    for (uint16_t u = start; u < manifest->count; u++) {
        const cargo_unit_t *cu = &manifest->units[u];
        if (cu->commodity != commodity) continue;
        float mult = mining_payout_multiplier((mining_grade_t)cu->grade) *
                     prefix_class_price_multiplier((int)cu->prefix_class);
        bonus += (mult - 1.0f) * price_per;
    }
    return bonus;
}

/* Sell exactly one (commodity, grade) unit — the trade-tab per-row
 * counterpart to `trade_apply_buy_row`. Pulls one cargo_unit_t from the
 * ship manifest matching `grade` (or the FIFO-first matching commodity
 * when `grade == MINING_GRADE_COUNT`), credits the player at this
 * station's buy price + the unit's grade bonus, transfers the manifest
 * entry to the station, then syncs the derived float cargo/cache.
 *
 * Returns true when a unit actually sold, so the caller can skip the
 * bulk path and not also drain the rest of the cargo. */
static bool try_sell_one_unit(world_t *w, server_player_t *sp,
                              commodity_t commodity, mining_grade_t grade) {
    if (commodity >= COMMODITY_COUNT) return false;
    /* Raw ore lives in towed fragments now (#259), not in ship.cargo,
     * so it can't be sold by the per-row path. */
    if (commodity < COMMODITY_RAW_ORE_COUNT) return false;
    station_t *st = &w->stations[sp->current_station];
    if (!station_consumes(st, commodity)) return false;
    float space = station_finished_space(st, commodity);
    if (space < 0.999f) {
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_ORDER_REJECTED,
            .player_id = sp->id,
            .order_rejected = { .reason = ORDER_REJECT_SELL_INVENTORY_FULL },
        });
        return false;
    }

    /* Match the TRADE row quote: sell the highest prefix multiplier in
     * this commodity/grade bucket, not whichever unit happens to be FIFO. */
    int unit_idx = manifest_find_top_sell_unit(&sp->ship.manifest, commodity, grade);
    /* Without a manifest unit we'd be selling provenance-less float
     * cargo, which the player UI shouldn't have offered. Fall through
     * to refusal (and keep the cargo) so we don't silently mint a
     * common-grade payout that breaks the chain. */
    if (unit_idx < 0) return false;

    const cargo_unit_t *quoted = &sp->ship.manifest.units[unit_idx];
    mining_grade_t actual_grade = (mining_grade_t)quoted->grade;
    float price = station_buy_price(st, commodity);
    float mult = mining_payout_multiplier(actual_grade);
    float graded_price = station_buy_price_unit(st, quoted) * mult;
    int base_cr = (int)lroundf(price);
    int bonus_cr = (int)lroundf(graded_price - price);

    cargo_unit_t sold_unit;
    if (!manifest_remove(&sp->ship.manifest, (uint16_t)unit_idx, &sold_unit))
        return false;
    if (!manifest_push(&st->manifest, &sold_unit)) {
        (void)manifest_push(&sp->ship.manifest, &sold_unit);
        return false;
    }
    sync_ship_finished_cargo(&sp->ship, commodity);
    sync_station_finished_inventory(st, commodity);

    /* Pool decrement is implicit via ledger_earn; pool is now derived
     * from -Σ(balance), so the credit on the player's ledger naturally
     * shows up as a deeper net-issuance for the station. */
    if (sp->pubkey_set) {
        ledger_earn_by_pubkey(st, sp->pubkey, graded_price);
        ledger_record_ore_sold(st, sp->pubkey, 1, commodity);
    } else {
        ledger_earn(st, sp->session_token, graded_price);
    }
    sp->ship.stat_credits_earned += graded_price;
    SIM_LOG("[sim] player %d sold 1× %s (grade %d) for %.0f cr at %s\n",
            sp->id, commodity_short_name(commodity), (int)actual_grade,
            graded_price, st->name);
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_SELL, .player_id = sp->id,
        .sell = { .station = sp->current_station,
                  .grade = (uint8_t)actual_grade,
                  .base_cr = base_cr,
                  .bonus_cr = bonus_cr,
                  .by_contract = 0 }});
    return true;
}

static void try_sell_station_cargo(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];

    /* Per-row sell: trade-tab click on a single grade row drops exactly
     * one unit — same cadence as the buy hotkeys. Bypasses the bulk
     * path below so the rest of the hold stays put. */
    if (sp->input.service_sell_one) {
        commodity_t commodity = sp->input.service_sell_only;
        mining_grade_t grade  = sp->input.service_sell_grade;
        try_sell_one_unit(w, sp, commodity, grade);
        sp->input.service_sell_only = COMMODITY_COUNT;
        sp->input.service_sell_grade = MINING_GRADE_COUNT;
        sp->input.service_sell_one = false;
        return;
    }

    float payout = 0.0f;
    /* M7: track whether any of the accepted deliveries landed against an
     * active contract so the client's sell FX can tint yellow. */
    bool sold_against_contract = false;
    /* Optional one-shot filter: if the client requested selective
     * delivery via NET_ACTION_DELIVER_COMMODITY, only commodities
     * matching `filter` are delivered. COMMODITY_COUNT (the default)
     * means "deliver everything that fits", which is the legacy
     * "deliver all" behavior triggered by NET_ACTION_SELL_CARGO. */
    commodity_t filter = sp->input.service_sell_only;
    bool selective = (filter < COMMODITY_COUNT);
    /* Snapshot the player's cargo of the filtered commodity (if any) so
     * we can tell at the bottom whether the station refused everything
     * the player tried to sell — vs the player just having empty hold.
     * Only the selective-filter path needs this notice; the bulk
     * SELL_CARGO already drops what fits and ignores the rest. */
    float pre_cargo = (selective ? sp->ship.cargo[filter] : 0.0f);
    bool tried_but_full = false;
    bool had_sellable_cargo = false;

    /* Deliver any cargo matching active supply contracts at this station.
     *
     * Raw ore contracts (c < COMMODITY_RAW_ORE_COUNT) are intentionally
     * skipped: since physical ore towing replaced cargo vacuum (#259),
     * players no longer carry raw ore in ship.cargo[] — fragments ride
     * in ship.towed_fragments[] and are consumed by furnaces at dock.
     * Ore-contract fulfillment is driven by smelter-throughput bumping
     * station._inventory_cache[ORE], not by this delivery path. Leaving the
     * ore branch in would be a silent no-op. */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index != sp->current_station) continue;
        commodity_t c = ct->commodity;
        if (c < COMMODITY_RAW_ORE_COUNT) continue; /* see comment above */
        if (selective && filter != c) continue;
        int held = manifest_count_by_commodity(&sp->ship.manifest, c);
        if (held <= 0) continue;
        had_sellable_cargo = true;
        float space = station_finished_space(st, c);
        int space_units = (int)floorf(space + 0.0001f);
        if (space_units <= 0) {
            tried_but_full = true;
            continue;
        }
        int needed = (int)floorf(ct->quantity_needed + 0.0001f);
        int deliver_units = held;
        if (deliver_units > needed) deliver_units = needed;
        if (deliver_units > space_units) deliver_units = space_units;
        if (deliver_units <= 0) continue;
        float price_per = contract_price(ct);
        uint16_t station_count_before = st->manifest.count;
        int moved = manifest_transfer_by_commodity(&sp->ship.manifest,
                                                   &st->manifest, c,
                                                   deliver_units);
        if (moved <= 0) continue;
        float bonus = manifest_grade_bonus_from_range(&st->manifest,
                                                      station_count_before,
                                                      c, price_per);
        payout += ((float)moved * price_per) + bonus;
        sold_against_contract = true;
        sync_ship_finished_cargo(&sp->ship, c);
        sync_station_finished_inventory(st, c);
        float deliver = (float)moved;
        ct->quantity_needed -= deliver;
        if (ct->quantity_needed <= 0.01f) {
            /* Don't close if scaffold modules still need this material */
            bool scaffold_still_needs = false;
            for (int m2 = 0; m2 < st->module_count; m2++) {
                if (module_build_state(&st->modules[m2]) == MODULE_BUILD_AWAITING_SUPPLY
                    && module_build_material(st->modules[m2].type) == c) {
                    scaffold_still_needs = true; break;
                }
            }
            if (!scaffold_still_needs) {
                ct->active = false;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE,
                    .contract_complete.action = CONTRACT_TRACTOR});
            }
        }
    }

    /* Fallback: fab stations accept any ingot they consume, even without
     * an active contract. This lets multi-input recipes source their
     * secondary ingredient through the normal station sell path. */
    for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
        commodity_t buy = (commodity_t)i;
        if (!station_consumes(st, buy)) continue;
        if (selective && filter != buy) continue;
        int held = manifest_count_by_commodity(&sp->ship.manifest, buy);
        if (held <= 0) continue;
        had_sellable_cargo = true;
        {
            float space = station_finished_space(st, buy);
            int space_units = (int)floorf(space + 0.0001f);
            if (space_units <= 0) tried_but_full = true;
            if (space_units > 0) {
                int accepted_units = held < space_units ? held : space_units;
                if (accepted_units <= 0) continue;
                float price = station_buy_price(st, buy);
                uint16_t station_count_before = st->manifest.count;
                int moved = manifest_transfer_by_commodity(&sp->ship.manifest,
                                                           &st->manifest, buy,
                                                           accepted_units);
                if (moved <= 0) continue;
                float bonus = manifest_grade_bonus_from_range(&st->manifest,
                                                              station_count_before,
                                                              buy, price);
                payout += ((float)moved * price) + bonus;
                sync_ship_finished_cargo(&sp->ship, buy);
                sync_station_finished_inventory(st, buy);
            }
        }
    }

    if (payout > 0.01f) {
        /* Stations carry the debt — pool is derived from -Σ(balance),
         * so crediting the player's ledger naturally pushes the station's
         * net issuance more negative. Conservation is structural.
         *
         * Use ledger_earn_by_pubkey (full credit). DON'T route through
         * ledger_credit_supply_by_pubkey — that one applies the 35%
         * smelt-station cut and is meant for raw ore → ingot deliveries
         * where the destination station does the smelting work. Contract
         * delivery and the consume-fallback below are pure trade
         * settlements; the player gets the full quoted price.
         *
         * Bug history: until now the pubkey branch silently dropped 35%
         * of every inter-station ingot delivery on the floor — the
         * popup showed the full payout (`payout`) but the ledger only
         * received `payout * 0.65`. Reported as "press S, popup says
         * +152, wallet only sees +99." */
        {
            if (sp->pubkey_set) {
                ledger_earn_by_pubkey(st, sp->pubkey, payout);
            } else {
                ledger_earn(st, sp->session_token, payout);
            }
            sp->ship.stat_credits_earned += payout;
            SIM_LOG("[sim] player %d sold cargo for %.0f cr at %s\n", sp->id, payout, st->name);
            /* M7: populate the sell event with station + amount so the
             * client's +$N popup and hint-bar batch actually animate dock
             * deliveries (previously the event carried zero payload). Grade
             * stays MINING_GRADE_COMMON — ingot deliveries don't have a
             * per-unit grade at this call site. */
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_SELL, .player_id = sp->id,
                .sell = { .station = sp->current_station,
                          .grade = (uint8_t)MINING_GRADE_COMMON,
                          .base_cr = (int)lroundf(payout),
                          .bonus_cr = 0,
                          .by_contract = sold_against_contract ? 1u : 0u }});
        }
    }
    /* Surface a notice when the press did nothing — distinguish "no
     * consumer here" from "consumer present but glutted". Fires for
     * both selective and bulk sell so the player isn't left wondering
     * why the dock ate their input silently. */
    if (payout < 0.01f) {
        if (tried_but_full) {
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_ORDER_REJECTED,
                .player_id = sp->id,
                .order_rejected = { .reason = ORDER_REJECT_SELL_INVENTORY_FULL },
            });
        } else if (selective && pre_cargo > 0.01f
                   && sp->ship.cargo[filter] > pre_cargo - 0.01f) {
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_ORDER_REJECTED,
                .player_id = sp->id,
                .order_rejected = { .reason = ORDER_REJECT_SELL_NOT_ACCEPTED },
            });
        }
        (void)had_sellable_cargo;
    }
    /* Clear the one-shot filter so the next plain SELL_CARGO press
     * resumes the default "deliver all" behavior. */
    sp->input.service_sell_only = COMMODITY_COUNT;
    sp->input.service_sell_grade = MINING_GRADE_COUNT;
    sp->input.service_sell_one = false;
}

static void try_repair_ship(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    /* Any dock can install kits — the kits themselves are the gate.
     * No kits in cargo or station inventory = hp_apply==0 below and
     * we early-return without charging anything. */
    float max_hull = ship_max_hull(&sp->ship);
    float missing = fmaxf(0.0f, max_hull - sp->ship.hull);
    if (missing <= 0.0f) return;

    /* 1 kit = 1 HP. Source priority: ship cargo first (kits the player
     * brought along — already paid for), then station inventory at
     * station retail. Shipyards charge no labor (you paid station
     * retail already if buying here); any other dock charges
     * LABOR_FEE_PER_HP for the install. Partial repair is allowed if
     * neither source has enough kits. */
    int kits_in_cargo  = ship_finished_count(&sp->ship, COMMODITY_REPAIR_KIT);
    int kits_at_station = station_finished_count(st, COMMODITY_REPAIR_KIT);
    int hp_needed       = (int)ceilf(missing);
    int hp_apply        = hp_needed;
    if (hp_apply > kits_in_cargo + kits_at_station)
        hp_apply = kits_in_cargo + kits_at_station;
    if (hp_apply <= 0) return;

    int from_cargo   = (hp_apply < kits_in_cargo) ? hp_apply : kits_in_cargo;
    int from_station = hp_apply - from_cargo;

    int drained_cargo = ship_finished_drain(&sp->ship, COMMODITY_REPAIR_KIT, from_cargo);
    int drained_station = station_finished_drain(st, COMMODITY_REPAIR_KIT, from_station);
    int actual_apply = drained_cargo + drained_station;
    if (actual_apply <= 0) return;

    /* Cost = station retail on station-sourced kits + labor at non-shipyard. */
    float station_kit_cost = (float)drained_station
                           * station_sell_price(st, COMMODITY_REPAIR_KIT);
    bool is_shipyard = station_has_module(st, MODULE_SHIPYARD);
    float labor_cost = is_shipyard ? 0.0f : (float)actual_apply * LABOR_FEE_PER_HP;
    float cost = ceilf(station_kit_cost + labor_cost);
    if (cost > 0.0f) {
        if (sp->pubkey_set) {
            ledger_force_debit_by_pubkey(st, sp->pubkey, cost, &sp->ship);
        } else {
            ledger_force_debit(st, sp->session_token, cost, &sp->ship);
        }
    }

    sp->ship.hull = fminf(max_hull, sp->ship.hull + (float)actual_apply);
    SIM_LOG("[sim] player %d repaired %d HP (%d cargo + %d station kits, %.0f cr)\n",
            sp->id, actual_apply, drained_cargo, drained_station, cost);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_REPAIR, .player_id = sp->id});
}

static void try_apply_ship_upgrade(world_t *w, server_player_t *sp, ship_upgrade_t upgrade) {
    station_t *st = &w->stations[sp->current_station];
    /* Any dock installs the upgrade — the modules themselves are the
     * gate. No more FAB-module service requirement; the recipe input
     * (frame/laser/tractor) has to come from cargo or the dock's
     * inventory and that's what limits the action. Mirrors the
     * repair-kit "any dock" model from #373. */
    if (ship_upgrade_maxed(&sp->ship, upgrade)) return;

    /* Real cost = the modules themselves (frames / lasers / tractors).
     * Cargo first; if short, dock fills the gap from station inventory
     * at retail price. No flat credit upgrade fee anymore — the credit
     * cost is purely the per-unit retail on dock-sourced units. Mirrors
     * try_repair_ship's cargo-first / dock-fallback pattern. */
    product_t required = upgrade_required_product(upgrade);
    commodity_t comm = (commodity_t)(COMMODITY_FRAME + required);
    int units_needed = (int)ceilf(upgrade_product_cost(&sp->ship, upgrade));
    int in_cargo  = ship_finished_count(&sp->ship, comm);
    int at_station = station_finished_count(st, comm);
    if (in_cargo + at_station < units_needed) return;

    int from_cargo   = (units_needed < in_cargo) ? units_needed : in_cargo;
    int from_station = units_needed - from_cargo;

    float credit_cost = (float)from_station * station_sell_price(st, comm);
    if (credit_cost > 0.0f) {
        bool can_afford = sp->pubkey_set ?
            ledger_spend_by_pubkey(st, sp->pubkey, credit_cost, &sp->ship) :
            ledger_spend(st, sp->session_token, credit_cost, &sp->ship);
        if (!can_afford) return;
    }

    int drained_cargo = ship_finished_drain(&sp->ship, comm, from_cargo);
    int drained_station = station_finished_drain(st, comm, from_station);
    if (drained_cargo + drained_station < units_needed) return;

    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  sp->ship.mining_level++;  break;
    case SHIP_UPGRADE_HOLD:    sp->ship.hold_level++;    break;
    case SHIP_UPGRADE_TRACTOR: sp->ship.tractor_level++; break;
    default: break;
    }
    SIM_LOG("[sim] player %d upgraded %d to level %d (%d cargo + %d dock kits, %.0f cr)\n",
            sp->id, (int)upgrade, ship_upgrade_level(&sp->ship, upgrade),
            drained_cargo, drained_station, credit_cost);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_UPGRADE, .player_id = sp->id, .upgrade.upgrade = upgrade});
}

/* ================================================================== */
/* Per-player per-step functions                                      */
/* ================================================================== */

/* step_ship_rotation, step_ship_thrust, ship_boost_thrust_mult, and
 * step_ship_motion now live in server/sim_ship.c (shared between
 * player + future NPC controllers per #294 Slice 2). */

/* Boost hull drain: 0.02 HP/s baseline (near-free cruise), +1.4 HP/s per
 * unit of |turn_input|. Straight-line boost is barely noticeable (~1 HP
 * per 50s) so haulers with cargo aren't silently bled out. The turn
 * coefficient is preserved — combat maneuvering (yank + boost) still
 * costs ~1.5 HP/s. Silent drain (no DAMAGE event) — emitting one per
 * tick would spam screen-shake and damage audio. If the drain empties
 * the hull, route through emergency_recover_ship so the usual
 * death/respawn UX fires. */
static void step_ship_boost_drain(world_t *w, server_player_t *sp, float dt, bool boost, float turn_input) {
    if (!boost || sp->ship.hull <= 0.0f) return;
    float turn_abs = turn_input < 0.0f ? -turn_input : turn_input;
    float drain = (0.02f + 1.4f * turn_abs) * dt;
    sp->ship.hull = fmaxf(0.0f, sp->ship.hull - drain);
    if (sp->ship.hull <= 0.01f) emergency_recover_ship(w, sp);
}

/* step_ship_motion moved to server/sim_ship.c (#294 Slice 2). */

/* Resolve ship vs station using shared geometry emitter. */
static void resolve_module_collisions(world_t *w, server_player_t *sp, const station_t *st) {
    station_geom_t geom;
    station_build_geom(st, &geom);
    float ship_r = ship_hull_def(&sp->ship)->ship_radius;

    /* Core: station center is empty space (construction yard).
     * Modules and corridors form the structure; the center is fly-through. */

    /* Module circles */
    for (int i = 0; i < geom.circle_count; i++)
        resolve_ship_circle(w, sp, geom.circles[i].center, geom.circles[i].radius);

    /* Near-module suppression: if ship is angularly close to any module
     * on a corridor's ring, skip corridor tests (module circle takes priority,
     * prevents junction jitter). */
    float ship_dist = sqrtf(v2_dist_sq(sp->ship.pos, st->pos));
    vec2 ship_delta = v2_sub(sp->ship.pos, st->pos);
    float ship_ang = atan2f(ship_delta.y, ship_delta.x);

    for (int ci = 0; ci < geom.corridor_count; ci++) {
        float ring_r = geom.corridors[ci].ring_radius;

        /* Check if ship is near any module on this corridor's ring */
        bool near_module = false;
        if (fabsf(ship_dist - ring_r) < STATION_CORRIDOR_HW + ship_r + STATION_MODULE_COL_RADIUS) {
            for (int mi = 0; mi < geom.circle_count; mi++) {
                if (geom.circles[mi].ring != geom.corridors[ci].ring) continue;
                float ang_diff = wrap_angle(ship_ang - geom.circles[mi].angle);
                float angular_size = (ring_r > 1.0f) ? (STATION_MODULE_COL_RADIUS + ship_r) / ring_r : 0.0f;
                if (fabsf(ang_diff) < angular_size) {
                    near_module = true;
                    break;
                }
            }
        }

        if (!near_module) {
            resolve_ship_annular_sector(w, sp, geom.center,
                ring_r, geom.corridors[ci].angle_a, geom.corridors[ci].arc_delta);
        }
    }
}

static bool is_already_towed(const ship_t *ship, int asteroid_idx);

static void resolve_world_collisions(world_t *w, server_player_t *sp) {
    ship_collision_count = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_collides(&w->stations[i])) continue;
        /* Skip collision with docking target during approach lerp */
        if (sp->docking_approach && i == sp->nearby_station) continue;
        resolve_module_collisions(w, sp, &w->stations[i]);
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        /* Run the full resolver for every active asteroid — fragments
         * (whether free-drifting, tractored by another player, or
         * whiplashed loose) all collide and damage. The resolver gates
         * damage on closing relative velocity + threshold, so a parked
         * fragment drifting alongside a ship still resolves to zero
         * damage; a tractored fragment dragged INTO a third ship hits
         * normally. The owner's own tow doesn't self-damage thanks to
         * the session-token check in resolve_ship_asteroid_collision. */
        resolve_ship_asteroid_collision(w, sp, &w->asteroids[i]);
    }
    /* Crush: pinched between 3+ bodies simultaneously (2 adjacent modules
     * on the same ring is normal, only crush when truly trapped) */
    if (!sp->docked && ship_collision_count >= 3) {
        float crush = (float)(ship_collision_count - 2) * 2.0f;
        apply_ship_damage(w, sp, crush);
    }
}


/* Module-based docking: each MODULE_DOCK provides 3 berth slots spread
 * around the dock module (center, left, right of the outward offset).
 * dock_berth = dock_module_index * BERTHS_PER_DOCK + sub_slot. */
#define DOCK_BERTH_OFFSET 55.0f    /* radial offset from dock module center */
#define DOCK_BERTH_SPREAD 28.0f    /* tangential spread between sub-berths */
#define BERTHS_PER_DOCK 3          /* berths per MODULE_DOCK */
#define DOCK_SNAP_DISTANCE 30.0f   /* snap-to-docked threshold */

/* Count dock modules on a station */
static int station_dock_count(const station_t *st) {
    int count = 0;
    for (int i = 0; i < st->module_count; i++)
        if (st->modules[i].type == MODULE_DOCK && !st->modules[i].scaffold) count++;
    return count;
}

/* Total berth slots across all dock modules */
static int station_berth_count(const station_t *st) {
    return station_dock_count(st) * BERTHS_PER_DOCK;
}

/* Get the i-th dock module index */
static int station_dock_module(const station_t *st, int dock_index) {
    int count = 0;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_DOCK && !st->modules[i].scaffold) {
            if (count == dock_index) return i;
            count++;
        }
    }
    return -1;
}

/* Dock berth position: 0=outward end, 1=left side, 2=right side.
 * End berth is past the dock, side berths flank the module. */
static vec2 dock_berth_pos(const station_t *st, int berth) {
    int dock_idx = berth / BERTHS_PER_DOCK;
    int sub = berth % BERTHS_PER_DOCK;
    int mi = station_dock_module(st, dock_idx);
    if (mi < 0) return st->pos;
    int ring = st->modules[mi].ring;
    int slot = st->modules[mi].slot;
    vec2 mod_pos = module_world_pos_ring(st, ring, slot);
    float angle = module_angle_ring(st, ring, slot);
    vec2 radial = v2_from_angle(angle);  /* center → module (outward) */
    /* U-shape: berths on 3 sides of the dock, open on the corridor side.
     * Corridor connects toward higher slots, so U opens toward lower slots. */
    int slots = STATION_RING_SLOTS[ring];
    float slot_arc = TWO_PI_F / (float)slots;
    /* Gap direction: negative tangent (toward lower slot / gap) */
    float gap_angle = angle - slot_arc * 0.5f;
    vec2 gap_dir = v2_from_angle(gap_angle);
    vec2 gap_tangent = v2(-gap_dir.y, gap_dir.x); /* not used but clarifies intent */
    (void)gap_tangent;
    if (sub == 0) {
        /* Outward berth: radially away from center */
        return v2_add(mod_pos, v2_scale(radial, DOCK_BERTH_OFFSET));
    } else if (sub == 1) {
        /* Inward berth: radially toward center */
        return v2_add(mod_pos, v2_scale(radial, -DOCK_BERTH_OFFSET));
    } else {
        /* Gap-side berth: tangentially toward the ring gap */
        vec2 gap_tangent_dir = v2(-radial.y, radial.x);
        /* Dock at slot 0: gap is at negative tangent; higher slots: positive */
        float dir = (slot == 0) ? -1.0f : 1.0f;
        return v2_add(mod_pos, v2_scale(gap_tangent_dir, dir * DOCK_BERTH_OFFSET));
    }
}

/* Dock berth angle: face toward the dock module */
static float dock_berth_angle(const station_t *st, int berth) {
    int dock_idx = berth / BERTHS_PER_DOCK;
    int sub = berth % BERTHS_PER_DOCK;
    int mi = station_dock_module(st, dock_idx);
    if (mi < 0) return 0.0f;
    float angle = module_angle_ring(st, st->modules[mi].ring, st->modules[mi].slot);
    if (sub == 0) return angle + PI_F;       /* outward: face inward */
    if (sub == 1) return angle;              /* inward: face outward */
    /* Gap-side: face toward dock along tangent */
    int slot = st->modules[mi].slot;
    float dir = (slot == 0) ? 1.0f : -1.0f;
    float tang_angle = angle + PI_F * 0.5f * dir;
    return tang_angle;
}

/* Find the best (closest, unoccupied) berth slot */
static int find_best_berth(const world_t *w, const station_t *st, int station_idx, vec2 ship_pos) {
    int total = station_berth_count(st);
    if (total == 0) return 0;
    int best = 0;
    float best_d = 1e18f;
    for (int s = 0; s < total; s++) {
        vec2 bp = dock_berth_pos(st, s);
        float d = v2_dist_sq(ship_pos, bp);
        bool occupied = false;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!w->players[p].connected || !w->players[p].docked) continue;
            if (w->players[p].current_station != station_idx) continue;
            if (w->players[p].dock_berth == s) { occupied = true; break; }
        }
        if (!occupied && d < best_d) { best_d = d; best = s; }
    }
    return best;
}

static void update_docking_state(world_t *w, server_player_t *sp, float dt) {
    if (sp->docked) {
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
        /* Hold ship at dock module berth — rotates with the ring */
        sp->ship.pos = dock_berth_pos(&w->stations[sp->current_station], sp->dock_berth);
        sp->ship.angle = dock_berth_angle(&w->stations[sp->current_station], sp->dock_berth);
        sp->ship.vel = v2(0.0f, 0.0f);
        /* No passive heal: all repair goes through kits via try_repair_ship.
         * Press R to spend kits + credits, or carry kits in cargo and let
         * the autopilot trigger the repair on dock. */
        return;
    }

    /* Find nearest station with a dock module within approach range.
     * Distance measured to station CENTER (core), not to rotating module. */
    float approach_sq = DOCK_APPROACH_RANGE * DOCK_APPROACH_RANGE;
    float best_d = 1e18f;
    sp->nearby_station = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &w->stations[i];
        if (!station_exists(st)) continue;
        if (!station_has_module(st, MODULE_DOCK)) continue;
        float d_sq = v2_dist_sq(sp->ship.pos, st->pos);
        if (d_sq > approach_sq) continue;
        if (d_sq < best_d) {
            best_d = d_sq;
            sp->nearby_station = i;
        }
    }
    sp->in_dock_range = sp->nearby_station >= 0;

    /* Cancel approach if out of range */
    if (!sp->in_dock_range) sp->docking_approach = false;

    /* Docking approach: decelerate and glide toward locked berth */
    if (sp->docking_approach && sp->in_dock_range) {
        const station_t *dock_st = &w->stations[sp->nearby_station];
        vec2 target = dock_berth_pos(dock_st, sp->dock_berth);
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, target));

        /* Decelerate: approach speed scales with distance for smooth arrival */
        float approach_speed = fminf(160.0f, 40.0f + dist * 0.8f);
        float damping = 1.0f / (1.0f + 8.0f * dt);
        sp->ship.vel = v2_scale(sp->ship.vel, damping);
        float step = fminf(approach_speed * dt, dist);
        if (dist > 0.5f) {
            vec2 dir = v2_scale(v2_sub(target, sp->ship.pos), step / dist);
            sp->ship.pos = v2_add(sp->ship.pos, dir);
        }

        /* Rotate toward berth angle */
        float desired = dock_berth_angle(dock_st, sp->dock_berth);
        float rot_speed = fminf(8.0f, 3.0f + (1.0f - fminf(dist, 100.0f) / 100.0f) * 5.0f);
        sp->ship.angle = wrap_angle(sp->ship.angle + wrap_angle(desired - sp->ship.angle) * rot_speed * dt);

        /* Snap when close — berth was locked at approach start */
        if (dist < 20.0f) {
            dock_ship(w, sp);
            sp->docking_approach = false;
        }
    }
}

static void update_targeting_state(world_t *w, server_player_t *sp, vec2 forward) {
    vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);
    /* Prefer client's mining target hint if valid, in range, and in front.
     * Server re-validates: must be active, minable, within mining range,
     * and inside the forward cone. Prevents desynced hints from steering. */
    int hint = sp->input.mining_target_hint;
    if (hint >= 0 && hint < MAX_ASTEROIDS && w->asteroids[hint].active
        && !asteroid_is_collectible(&w->asteroids[hint])) {
        const asteroid_t *a = &w->asteroids[hint];
        float d_sq = v2_dist_sq(muzzle, a->pos);
        float max_r = MINING_RANGE + a->radius + 12.0f;
        if (d_sq <= max_r * max_r && hinted_target_in_mining_cone(muzzle, forward, a)) {
            sp->hover_asteroid = hint;
            return;
        }
    }
    sp->hover_asteroid = sim_find_mining_target(w, muzzle, forward, sp->ship.mining_level);
}

/* Check if a fragment is already towed by this player */
static bool is_already_towed(const ship_t *ship, int asteroid_idx) {
    for (int i = 0; i < ship->towed_count; i++)
        if (ship->towed_fragments[i] == asteroid_idx) return true;
    return false;
}

/* ---- Unified rubber-band physics ----
 *
 * One model for all towed rocks regardless of whether the player is
 * holding the tractor. Hooke's law:
 *
 *   stretch = max(0, |ship - rock| - REST_LEN)
 *   F_spring = K * stretch * dir_rock_to_ship    (pulls rock toward ship)
 *   F_damp   = D * dot(rel_vel, dir) * dir       (kills oscillation)
 *
 * The active-tractor flag (sp->ship.tractor_active) only enables the
 * GRAB step (auto-attach a new fragment). Once attached, the band
 * physics runs every tick. No artificial "two-zone" force, no hard
 * brake — rocks naturally trail at REST_LEN while the ship cruises,
 * and stretch when the ship accelerates away.
 *
 * Constants tuned so:
 *   - 100 u stretch ≈ a noticeable tug (~3 HP/s of ship drag at full load)
 *   - 200 u stretch ≈ near-elastic-limit, hauling feels heavy
 *   - tractor_range * 1.5 ≈ snap-out (band breaks). */
#define BAND_REST_LEN     80.0f
#define BAND_SPRING_K      4.0f   /* per unit of stretch (looser = more elastic lag) */
#define BAND_DAMPING       0.6f   /* light along-band damping — let it bounce */
#define BAND_TANGENT_DRAG  0.4f   /* just enough to bleed orbit, not lock parallel motion */
#define BAND_SHIP_MASS     8.0f   /* ship is heavier than a rock; reaction force scaled by 1/MASS */
/* Player tow band: symmetric spring (push == pull, signed by stretch),
 * 1D axial damping, separate tangent drag, full reaction on the ship.
 * Behavior identical to the pre-tractor-primitive hand-rolled version. */
static const tractor_beam_t PLAYER_TOW_BAND = {
    .rest_length     = BAND_REST_LEN,
    .pull_strength   = BAND_SPRING_K,
    .push_strength   = BAND_SPRING_K,
    .range           = 0.0f,                       /* 0 = no range gate */
    .axial_damping   = BAND_DAMPING,
    .tangent_damping = BAND_TANGENT_DRAG,
    .speed_cap       = 0.0f,
    .falloff         = TRACTOR_FALLOFF_CONSTANT,
};
static void apply_band_force(server_player_t *sp, asteroid_t *a, float dt) {
    tractor_anchor_t src = {
        .pos      = sp->ship.pos,
        .vel      = &sp->ship.vel,
        .inv_mass = 1.0f / BAND_SHIP_MASS,
    };
    tractor_anchor_t tgt = {
        .pos      = a->pos,
        .vel      = &a->vel,
        .inv_mass = 1.0f,
    };
    (void)tractor_apply(&src, &tgt, &PLAYER_TOW_BAND, dt);
}

/* Pick the closest signal-providing station to a position, or -1 if no
 * station's signal range covers that point. Used to attribute
 * fragment-lifecycle chain events to a witnessing station — the chain
 * log is per-station, so events without a witness can't be recorded.
 * Same shape as the rock_destroy witness picker in sim_asteroid.c. */
static int chain_pick_witness(const world_t *w, vec2 pos) {
    int witness = -1;
    float best_d2 = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_provides_signal(st)) continue;
        float sr = st->signal_range;
        float d2 = v2_dist_sq(pos, st->pos);
        if (d2 <= sr * sr && (witness < 0 || d2 < best_d2)) {
            witness = s;
            best_d2 = d2;
        }
    }
    return witness;
}

/* Sim tick at sub-tick precision rolled up to a 64-bit value the chain
 * log can record. Mirrors the convention used in chain_log_emit's
 * `epoch` field (sim ticks at 120 Hz). */
static uint64_t chain_epoch_tick(const world_t *w) {
    return (uint64_t)(w->time * 120.0);
}

/* Emit EVT_FRAGMENT_TOW for the witnessing station closest to the
 * fragment. Silently skips if no station has the position in signal
 * range — in-the-void tow events fall outside any chain. Same
 * fail-quiet semantics as EVT_ROCK_DESTROY. */
static void emit_fragment_tow_event(world_t *w, const asteroid_t *a,
                                    const server_player_t *sp) {
    int witness = chain_pick_witness(w, a->pos);
    if (witness < 0) return;
    chain_payload_fragment_tow_t payload = {0};
    memcpy(payload.fragment_pub, a->fragment_pub, 32);
    if (sp && sp->session_ready) {
        memcpy(payload.tower_player_pub, sp->pubkey, 32);
        memcpy(payload.tower_session_token, sp->session_token,
               sizeof(payload.tower_session_token));
    }
    payload.epoch_tick = chain_epoch_tick(w);
    (void)chain_log_emit(w, &w->stations[witness], CHAIN_EVT_FRAGMENT_TOW,
                         &payload, (uint16_t)sizeof(payload));
}

/* Emit EVT_FRAGMENT_RELEASE — tow ended without a smelt. Reason
 * captures whether the asteroid was destroyed mid-tow, the band
 * snapped, or the player manually released (which includes the PvP
 * fling at high stretch — same code path either way). */
static void emit_fragment_release_event(world_t *w, const asteroid_t *a,
                                        const server_player_t *sp,
                                        fragment_release_reason_t reason) {
    int witness = chain_pick_witness(w, a->pos);
    if (witness < 0) return;
    chain_payload_fragment_release_t payload = {0};
    memcpy(payload.fragment_pub, a->fragment_pub, 32);
    if (sp && sp->session_ready) {
        memcpy(payload.tower_player_pub, sp->pubkey, 32);
        memcpy(payload.tower_session_token, sp->session_token,
               sizeof(payload.tower_session_token));
    }
    payload.epoch_tick = chain_epoch_tick(w);
    payload.reason = (uint8_t)reason;
    (void)chain_log_emit(w, &w->stations[witness], CHAIN_EVT_FRAGMENT_RELEASE,
                         &payload, (uint16_t)sizeof(payload));
}

static void step_fragment_collection(world_t *w, server_player_t *sp, float dt) {
    float nearby_sq = FRAGMENT_NEARBY_RANGE * FRAGMENT_NEARBY_RANGE;
    float tr = ship_tractor_range(&sp->ship);
    float tr_sq = tr * tr;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    /* Update towed fragments via the unified band physics. Same code
     * runs whether the tractor is held or released — release just
     * stops auto-grabbing new rocks. */
    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) {
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
            t--;
            continue;
        }
        asteroid_t *a = &w->asteroids[idx];
        apply_band_force(sp, a, dt);
        sp->tractor_fragments++;

        /* Hard separation: fragment never overlaps ship (band can't
         * push hard enough to overshoot at high stretch otherwise). */
        float ship_r = ship_hull_def(&sp->ship)->ship_radius;
        float min_d = a->radius + ship_r + 4.0f;
        vec2 frag_to_ship = v2_sub(sp->ship.pos, a->pos);
        float ds = v2_len_sq(frag_to_ship);
        if (ds < min_d * min_d && ds > 0.1f) {
            float dd = sqrtf(ds);
            vec2 push = v2_scale(frag_to_ship, -((min_d - dd) / dd));
            a->pos = v2_add(a->pos, push);
        }

        /* Fragment-fragment separation: towed rocks push apart so they
         * settle into a constellation around the ship instead of
         * stacking. */
        for (int u = t + 1; u < sp->ship.towed_count; u++) {
            int uidx = sp->ship.towed_fragments[u];
            if (uidx < 0 || uidx >= MAX_ASTEROIDS || !w->asteroids[uidx].active) continue;
            asteroid_t *b = &w->asteroids[uidx];
            float sep = a->radius + b->radius + 2.0f;
            vec2 ab = v2_sub(b->pos, a->pos);
            float ab_sq = v2_len_sq(ab);
            if (ab_sq < sep * sep && ab_sq > 0.1f) {
                float abd = sqrtf(ab_sq);
                float overlap = (sep - abd) * 0.5f;
                vec2 n = v2_scale(ab, overlap / abd);
                a->pos = v2_sub(a->pos, n);
                b->pos = v2_add(b->pos, n);
            }
        }
    }

    int max_tow = 2 + sp->ship.tractor_level * 2; /* 2/4/6/8/10 */
    /* Use nearby range (the larger of the two) for the broad check */
    float broad_sq = (nearby_sq > tr_sq) ? nearby_sq : tr_sq;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        /* Cheap axis-aligned pre-check before expensive distance calc */
        float dx = sp->ship.pos.x - a->pos.x;
        float dy = sp->ship.pos.y - a->pos.y;
        if (dx * dx > broad_sq || dy * dy > broad_sq) continue;
        if (is_already_towed(&sp->ship, i)) continue;
        float d_sq = dx * dx + dy * dy;
        if (d_sq <= nearby_sq) sp->nearby_fragments++;
        if (d_sq <= tr_sq) {
            sp->tractor_fragments++;
            /* Instant grab: tractor pulse snaps fragments to tow chain.
             * No drift phase — if it's in range and there's room, grab it.
             * The fracture claim window owns rarity; tow ownership only
             * matters for the later smelt-time payout split. */
            if (sp->ship.towed_count < max_tow) {
                sp->ship.towed_fragments[sp->ship.towed_count] = (int16_t)i;
                sp->ship.towed_count++;
                a->last_towed_by = (int8_t)sp->id;
                if (sp->session_ready)
                    memcpy(a->last_towed_token, sp->session_token,
                           sizeof(a->last_towed_token));
                sp->ship.stat_ore_mined += a->ore;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_PICKUP, .player_id = sp->id,
                                            .pickup = {.ore = a->ore, .fragments = 1}});
                /* Layer C of #479: chain-log the start of this tow so
                 * heritage queries can reconstruct who held a fragment
                 * before it became an ingot. Witnessed by the closest
                 * signal-providing station; out-of-signal grabs are
                 * silently invisible (same fail-quiet semantics as
                 * EVT_ROCK_DESTROY). */
                emit_fragment_tow_event(w, a, sp);
            }
        }
    }
}

/* Leashed fragments: tractor not held, rocks still tethered. The band
 * physics is identical to active-tow — Hooke spring + damping, run by
 * apply_band_force in step_fragment_collection. The only thing this
 * pass does separately is the snap-out check (rocks beyond 1.5×
 * tractor_range fall off) and clearing the dead-fragment slots,
 * because the active path lives inside step_fragment_collection
 * which we don't run when tractor is off. */
static void step_leashed_fragments(world_t *w, server_player_t *sp, float dt) {
    float tractor_r = ship_tractor_range(&sp->ship);
    /* Walk backward so removal-by-swap doesn't skip elements. */
    for (int t = sp->ship.towed_count - 1; t >= 0; t--) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) {
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
            continue;
        }
        asteroid_t *a = &w->asteroids[idx];
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float dist = v2_len(to_ship);

        /* Elastic limit: band snaps past 1.5 × tractor_range. */
        if (dist > tractor_r * 1.5f) {
            emit_fragment_release_event(w, a, sp, FRAGMENT_RELEASE_BAND_SNAP);
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
            continue;
        }
        apply_band_force(sp, a, dt);
    }
}

/* Deposit towed fragments: when the SHIP is near an ore buyer module,
 * all towed fragments get consumed (ore → station, credits → player).
 * Fragments don't need to individually reach the hopper — the ship does. */
/* HOPPER_PULL_RANGE, HOPPER_PULL_ACCEL → game_sim.h */
#define FURNACE_SMELT_RANGE 250.0f  /* fragment counts as "held" by furnace within this range */

static void release_towed_fragments(world_t *w, server_player_t *sp);

/* Clean up dead refs AND auto-detach ALL towed fragments when ship is near a hopper. */
static void step_towed_cleanup(world_t *w, server_player_t *sp) {
    /* Clean dead refs */
    for (int t = sp->ship.towed_count - 1; t >= 0; t--) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) {
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
        }
    }
    /* Auto-release removed — player must manually release with R key.
     * Furnace smelting (step_furnace_smelting) consumes S-tier fragments held by 2+ tractors
     * directly, crediting the towing player. */
}

/* Slingshot release. Fire the towed rocks along the BAND AXIS using
 * the stored elastic energy of the stretched spring.
 *
 * Slingshot mental model:
 *   - The band points from rock -> ship. Releasing it accelerates the
 *     rock in that direction (toward the ship and past it).
 *   - Stored elastic PE = 0.5 * k * stretch² converts to KE
 *     (assuming unit rock mass): v_added = stretch * sqrt(K).
 *   - Plus a small floor (BASE_SPEED) so a tap with no stretch still
 *     throws something — tap-to-yeet, not tap-to-drop.
 *   - Plus the ship's velocity (you're moving with the rock when you
 *     release).
 *
 * Direction comes from band geometry, NOT ship.angle. Aim by
 * positioning yourself so the rock you want to throw is OPPOSITE the
 * target direction. That's the slingshot. The ship-facing angle was
 * cheesy because it broke the spatial intuition: you'd stretch east
 * and the rock would yet shoot wherever your nose was pointing.
 *
 * last_towed_token stays set so kill credit resolves on impact. */
#define ROCK_THROW_BASE_SPEED  40.0f
static void release_towed_fragments(world_t *w, server_player_t *sp) {
    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        if (!w->asteroids[idx].active) continue;
        asteroid_t *a = &w->asteroids[idx];
        /* Chain-log the manual release. Same call covers innocent drops
         * (R-key tap) and the PvP fling (release at high stretch sends
         * the rock flying) — both are tow terminations from the chain
         * log's perspective. */
        emit_fragment_release_event(w, a, sp, FRAGMENT_RELEASE_MANUAL);
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float dist = v2_len(to_ship);
        if (dist < 0.01f) {
            /* Degenerate: rock is on top of the ship. Fire forward as
             * a fallback — no band axis to read. */
            vec2 fwd = v2(cosf(sp->ship.angle), sinf(sp->ship.angle));
            a->vel = v2_add(sp->ship.vel, v2_scale(fwd, ROCK_THROW_BASE_SPEED));
            a->net_dirty = true;
            continue;
        }
        vec2 dir = v2_scale(to_ship, 1.0f / dist);
        /* Stretch beyond rest length — only the elastic portion counts
         * as stored energy. A rock at slack-distance gets just BASE. */
        float stretch = dist - BAND_REST_LEN;
        if (stretch < 0.0f) stretch = 0.0f;
        /* v = sqrt(K) * stretch  is the elastic-energy fling. With
         * BAND_SPRING_K = 6 and stretch = 200 (deep stretch), this is
         * ~490 m/s — punchy. Half-stretch (100) is ~245 m/s. */
        float elastic = sqrtf(BAND_SPRING_K) * stretch;
        float fling = ROCK_THROW_BASE_SPEED + elastic;
        a->vel = v2_add(sp->ship.vel, v2_scale(dir, fling));
        a->net_dirty = true;
        /* last_towed_by / last_towed_token already set when the
         * tractor pulled the fragment in — leave them so kill credit
         * resolves on impact. */
    }
    sp->ship.towed_count = 0;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
}

/* ---- Scaffold tow physics ---- */

/* Base tow cap at hull accel = 200 (the rough average of the hull
 * classes). The actual cap scales with the towing ship's accel so
 * a powerful engine can pull harder. */
static const float SCAFFOLD_TOW_SPEED_BASE = 55.0f;
static const float SCAFFOLD_PICKUP_RANGE = 80.0f;    /* how close to grab one */

/* Compute the effective tow speed cap for a ship hull. Engine
 * horsepower (accel) is the only input — bigger accel pulls the
 * scaffold faster. Floored at 30 so weak hulls can still move it. */
static float scaffold_tow_speed_cap(const hull_def_t *hull) {
    float scale = hull->accel / 200.0f;
    float cap = SCAFFOLD_TOW_SPEED_BASE * scale;
    if (cap < 30.0f) cap = 30.0f;
    if (cap > 180.0f) cap = 180.0f;
    return cap;
}

/* Simple release — scaffold floats loose. */
static void release_towed_scaffold(world_t *w, server_player_t *sp) {
    int idx = sp->ship.towed_scaffold;
    if (idx >= 0 && idx < MAX_SCAFFOLDS && w->scaffolds[idx].active) {
        w->scaffolds[idx].state = SCAFFOLD_LOOSE;
        w->scaffolds[idx].towed_by = -1;
    }
    sp->ship.towed_scaffold = -1;
}

/* Intentional placement — snap to outpost or found new station.
 * If the player chose an explicit target via the placement reticle
 * (place_target_station >= 0), use that. Otherwise auto-snap. */
static void place_towed_scaffold(world_t *w, server_player_t *sp) {
    int idx = sp->ship.towed_scaffold;
    if (idx < 0 || idx >= MAX_SCAFFOLDS || !w->scaffolds[idx].active) return;
    scaffold_t *sc = &w->scaffolds[idx];

    /* Explicit target from client reticle */
    if (sp->input.place_target_station >= 0) {
        int s = sp->input.place_target_station;
        int ring = sp->input.place_target_ring;
        int slot = sp->input.place_target_slot;
        if (s >= 0 && s < MAX_STATIONS && station_is_active(&w->stations[s])) {
            station_t *st = &w->stations[s];
            /* Verify the slot is still open */
            bool taken = false;
            for (int m = 0; m < st->module_count; m++)
                if (st->modules[m].ring == ring && st->modules[m].slot == slot) {
                    taken = true; break;
                }
            if (!taken) {
                sc->state = SCAFFOLD_SNAPPING;
                sc->placed_station = s;
                sc->placed_ring = ring;
                sc->placed_slot = slot;
                sc->vel = v2(0.0f, 0.0f);
                sc->towed_by = -1;
                sp->ship.towed_scaffold = -1;
                return;
            }
        }
    }

    /* Materialize a nearby planned station if scaffold is close to it.
     * Only a SIGNAL RELAY scaffold can found (or materialize) a
     * station — the relay IS the station's core, not just another
     * module. Other module types must be towed to an already-active
     * outpost and snapped into a ring slot. */
    if (sc->module_type == MODULE_SIGNAL_RELAY) {
        const float MATERIALIZE_RANGE = 600.0f;
        const float MATERIALIZE_RANGE_SQ = MATERIALIZE_RANGE * MATERIALIZE_RANGE;
        for (int s = 3; s < MAX_STATIONS; s++) {
            station_t *st = &w->stations[s];
            if (!st->planned) continue;
            if (v2_dist_sq(st->pos, sc->pos) > MATERIALIZE_RANGE_SQ) continue;
            /* Materialize: planned → scaffold-state */
            st->planned = false;
            st->scaffold = true;
            st->scaffold_progress = 0.0f;
            st->radius = OUTPOST_RADIUS;
            st->dock_radius = OUTPOST_DOCK_RADIUS;
            st->signal_range = OUTPOST_SIGNAL_RANGE;
            add_module_at(st, MODULE_DOCK, 0, 0xFF);
            /* The towed relay becomes the station's core relay below;
             * don't auto-add an extra one here. */
            rebuild_station_services(st);
            /* Generate supply contract for activation frames */
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) {
                    w->contracts[k] = (contract_t){
                        .active = true, .action = CONTRACT_TRACTOR,
                        .station_index = (uint8_t)s,
                        .commodity = COMMODITY_FRAME,
                        .quantity_needed = SCAFFOLD_MATERIAL_NEEDED,
                        .base_price = 23.0f,
                        .target_index = -1, .claimed_by = -1,
                    };
                    break;
                }
            }
            /* Try to find a planned slot matching the scaffold's type. */
            int chosen_ring = -1, chosen_slot = -1;
            for (int p = 0; p < st->placement_plan_count; p++) {
                if (st->placement_plans[p].type == sc->module_type) {
                    chosen_ring = st->placement_plans[p].ring;
                    chosen_slot = st->placement_plans[p].slot;
                    /* Remove the plan — it's being fulfilled */
                    for (int q = p; q < st->placement_plan_count - 1; q++)
                        st->placement_plans[q] = st->placement_plans[q + 1];
                    st->placement_plan_count--;
                    break;
                }
            }
            if (chosen_ring < 0) {
                chosen_ring = 1;
                chosen_slot = 0;
            }
            if (st->module_count < MAX_MODULES_PER_STATION) {
                station_module_t *m = &st->modules[st->module_count++];
                m->type = sc->module_type;
                m->ring = (uint8_t)chosen_ring;
                m->slot = (uint8_t)chosen_slot;
                m->scaffold = true;
                m->build_progress = 0.0f; /* needs supply after outpost activates */
            }
            sc->active = false;
            sp->ship.towed_scaffold = -1;
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_OUTPOST_PLACED,
                .player_id = sp->id,
                .outpost_placed = { .slot = s },
            });
            return;
        }
    }

    /* Auto-snap fallback: try to snap to a nearby outpost ring slot */
    for (int s = 3; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        int ring, slot;
        if (find_nearest_open_slot(st, sc->pos, &ring, &slot)) {
            sc->state = SCAFFOLD_SNAPPING;
            sc->placed_station = s;
            sc->placed_ring = ring;
            sc->placed_slot = slot;
            sc->vel = v2(0.0f, 0.0f);
            sc->towed_by = -1;
            sp->ship.towed_scaffold = -1;
            return;
        }
    }

    /* Not near an outpost — found a new station if the towed kit is a
     * SIGNAL RELAY (only relays can found stations, per the founding
     * ritual: tow the seed, not every brick) and we're in signal
     * range. Other module types fall through and keep towing — they
     * need an existing outpost to snap to. */
    if (sc->module_type == MODULE_SIGNAL_RELAY
        && signal_strength_at(w, sc->pos) > 0.0f
        && can_place_outpost(w, sc->pos)) {
        int slot = -1;
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_exists(&w->stations[s])) { slot = s; break; }
        }
        if (slot >= 0) {
            station_t *st = &w->stations[slot];
            station_cleanup(st);
            memset(st, 0, sizeof(*st));
            (void)station_manifest_bootstrap(st);
            generate_outpost_name(st->name, sizeof(st->name), sc->pos, slot);
            st->pos = sc->pos;
            st->radius = OUTPOST_RADIUS;
            st->dock_radius = OUTPOST_DOCK_RADIUS;
            st->signal_range = OUTPOST_SIGNAL_RANGE;
            /* Layer B of #479: derive the outpost's Ed25519 identity
             * from the founder's pubkey + station name + planted tick.
             * Must run after the name is set (the name is part of the
             * derivation) and stays stable for the station's lifetime. */
            station_authority_init_outpost(st, sp->pubkey,
                                           (uint64_t)(w->time * 128.0f));
            /* Outpost is born under construction — needs frames delivered
             * to activate. The towed relay seed becomes the station's
             * core relay (added below); the dock comes pre-stamped. */
            st->scaffold = true;
            st->scaffold_progress = 0.0f;
            add_module_at(st, MODULE_DOCK, 0, 0xFF);
            /* Relay added by the founding-tow path below. */
            rebuild_station_services(st);
            /* Generate supply contract for the outpost activation frames */
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) {
                    w->contracts[k] = (contract_t){
                        .active = true, .action = CONTRACT_TRACTOR,
                        .station_index = (uint8_t)slot,
                        .commodity = COMMODITY_FRAME,
                        .quantity_needed = SCAFFOLD_MATERIAL_NEEDED,
                        .base_price = 23.0f,
                        .target_index = -1, .claimed_by = -1,
                    };
                    break;
                }
            }
            /* Queue the player's module scaffold — needs material delivery
             * after the outpost activates before the build timer starts. */
            if (st->module_count < MAX_MODULES_PER_STATION) {
                station_module_t *m = &st->modules[st->module_count++];
                m->type = sc->module_type;
                m->ring = 1;
                m->slot = 0;
                m->scaffold = true;
                m->build_progress = 0.0f; /* needs supply after outpost activates */
            }
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_OUTPOST_PLACED,
                .outpost_placed = { .slot = slot },
            });
            sc->active = false;
            sp->ship.towed_scaffold = -1;
            return;
        }
    }
    /* Can't place here — do nothing, keep towing. Stamp a reason code
     * so the client can surface a useful notice ("out of signal range",
     * "needs a relay", etc.) instead of a silent fizzle. */
    uint8_t reject_reason;
    if (sc->module_type != MODULE_SIGNAL_RELAY) {
        reject_reason = ORDER_REJECT_SCAFFOLD_PLACEMENT_NEEDS_RELAY;
    } else if (signal_strength_unboosted(w, sc->pos) <= 0.0f) {
        reject_reason = ORDER_REJECT_SCAFFOLD_PLACEMENT_NO_SIGNAL;
    } else {
        /* In signal but can_place_outpost said no — most likely too
         * close to / overlapping an existing station, or no free slot. */
        bool free_slot = false;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&w->stations[s])) { free_slot = true; break; }
        }
        reject_reason = free_slot
            ? ORDER_REJECT_SCAFFOLD_PLACEMENT_TOO_CLOSE
            : ORDER_REJECT_SCAFFOLD_PLACEMENT_NO_SLOT;
    }
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_ORDER_REJECTED,
        .player_id = sp->id,
        .order_rejected = { .reason = reject_reason },
    });
}

static void step_scaffold_tow(world_t *w, server_player_t *sp, float dt) {
    int idx = sp->ship.towed_scaffold;

    /* Validate existing tow */
    if (idx >= 0) {
        if (idx >= MAX_SCAFFOLDS || !w->scaffolds[idx].active ||
            w->scaffolds[idx].state != SCAFFOLD_TOWING) {
            sp->ship.towed_scaffold = -1;
            idx = -1;
        }
    }

    /* If towing a scaffold, apply spring physics */
    if (idx >= 0) {
        scaffold_t *sc = &w->scaffolds[idx];
        float ship_r = ship_hull_def(&sp->ship)->ship_radius;
        float safe_dist = sc->radius + ship_r + 20.0f;
        vec2 to_ship = v2_sub(sp->ship.pos, sc->pos);
        float dist = v2_len(to_ship);

        /* Pull toward ship if too far */
        float tractor_r = ship_tractor_range(&sp->ship);
        if (dist > tractor_r * 0.8f) {
            /* Strong pull to catch up */
            vec2 pull = v2_scale(to_ship, 3.0f);
            sc->vel = v2_add(sc->vel, v2_scale(pull, dt));
        } else if (dist > safe_dist) {
            /* Gentle pull */
            vec2 pull = v2_scale(to_ship, 1.2f);
            sc->vel = v2_add(sc->vel, v2_scale(pull, dt));
        }

        /* Push away if too close */
        if (dist < safe_dist && dist > 0.1f) {
            vec2 push = v2_scale(to_ship, -(safe_dist - dist) * 6.0f);
            sc->vel = v2_add(sc->vel, v2_scale(push, dt));
        }

        /* Heavy drag — scaffolds feel massive */
        sc->vel = v2_scale(sc->vel, 1.0f / (1.0f + 3.0f * dt));

        /* Speed cap scaled by engine power. A miner (accel 300) tows
         * faster than a hauler (accel 140). Multiple ships pulling
         * the same scaffold can each contribute, but in practice the
         * primary tower's cap dominates. */
        float tow_cap = scaffold_tow_speed_cap(ship_hull_def(&sp->ship));
        float spd = v2_len(sc->vel);
        if (spd > tow_cap)
            sc->vel = v2_scale(sc->vel, tow_cap / spd);

        /* Move scaffold */
        sc->pos = v2_add(sc->pos, v2_scale(sc->vel, dt));

        /* If scaffold drifts too far (tractor broke), release */
        if (dist > tractor_r * 1.5f) {
            release_towed_scaffold(w, sp);
        }
        return;
    }

    /* Not towing — check if we can pick one up */
    if (!sp->ship.tractor_active) return;

    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active || sc->state != SCAFFOLD_LOOSE) continue;
        float d_sq = v2_dist_sq(sp->ship.pos, sc->pos);
        if (d_sq > SCAFFOLD_PICKUP_RANGE * SCAFFOLD_PICKUP_RANGE) continue;

        /* Attach */
        sp->ship.towed_scaffold = (int16_t)i;
        sc->state = SCAFFOLD_TOWING;
        sc->towed_by = sp->id;
        return; /* one scaffold at a time */
    }
}

/* Find scan target (station module, NPC, or player) along beam ray.
 * Returns true if a scan target was found, populating sp->scan_* fields. */
static bool find_scan_target(world_t *w, server_player_t *sp, vec2 muzzle, vec2 forward) {
    float best_dist = MINING_RANGE;
    sp->scan_target_type = 0;
    sp->scan_target_index = -1;
    sp->scan_module_index = -1;

    /* Each circle-target test reuses the same laser_ray. We compare the
     * `along` distance returned by laser_target_in_beam against best_dist
     * manually rather than tightening the ray's range each time, so a
     * larger target whose center sits past best_dist but whose radius
     * extends within still has a chance to register — preserving the
     * legacy behavior where best_dist gated on projected-center distance. */
    laser_ray_t ray = {
        .source_pos = muzzle, .source_dir = forward,
        .range = MINING_RANGE, .cone_half_angle = 0.0f,
    };

    /* Check station modules */
    for (int si = 0; si < MAX_STATIONS; si++) {
        const station_t *st = &w->stations[si];
        if (st->signal_range <= 0.0f) continue;
        /* Check core */
        vec2 hit; float along;
        if (laser_target_in_beam(&ray, st->pos, st->radius, &hit, &along)
            && along < best_dist) {
            best_dist = along;
            sp->scan_target_type = 1;
            sp->scan_target_index = si;
            sp->scan_module_index = -1; /* core */
            sp->beam_end = hit;
        }
        /* Check structural rings — ray vs annulus. Each station ring
         * is a thin band of girders at STATION_RING_RADIUS[r]. Cast the
         * beam against each ring circle and pick the nearest entry point. */
        for (int r = 1; r <= STATION_NUM_RINGS; r++) {
            float rr = STATION_RING_RADIUS[r];
            if (rr <= 0.0f) continue;
            const float ring_thickness = 12.0f;
            /* Ray-circle intersection: |muzzle + t*forward - st->pos|^2 = rr^2 */
            vec2 oc = v2_sub(muzzle, st->pos);
            float b_coef = v2_dot(oc, forward);
            float c_coef = v2_dot(oc, oc) - rr * rr;
            float disc = b_coef * b_coef - c_coef;
            if (disc < 0.0f) continue;
            float sq = sqrtf(disc);
            float t_near = -b_coef - sq;
            float t_far  = -b_coef + sq;
            /* Choose the first positive intersection (entry point) */
            float t_hit = (t_near > 0.0f) ? t_near : ((t_far > 0.0f) ? t_far : -1.0f);
            if (t_hit < 0.0f || t_hit >= best_dist) continue;
            /* Verify the hit is on the ring band, not just crossing the
             * inner empty space (annulus check via distance from station). */
            vec2 ring_hit = v2_add(muzzle, v2_scale(forward, t_hit));
            float hit_dist = v2_len(v2_sub(ring_hit, st->pos));
            if (fabsf(hit_dist - rr) > ring_thickness) continue;
            best_dist = t_hit;
            sp->scan_target_type = 1;
            sp->scan_target_index = si;
            sp->scan_module_index = -1;
            sp->beam_end = ring_hit;
        }
        /* Check individual modules */
        for (int mi = 0; mi < st->module_count; mi++) {
            if (st->modules[mi].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[mi].ring, st->modules[mi].slot);
            vec2 mod_hit; float mod_along;
            if (laser_target_in_beam(&ray, mp, STATION_MODULE_COL_RADIUS, &mod_hit, &mod_along)
                && mod_along < best_dist) {
                best_dist = mod_along;
                sp->scan_target_type = 1;
                sp->scan_target_index = si;
                sp->scan_module_index = mi;
                sp->beam_end = mod_hit;
            }
        }
    }

    /* Check NPC ships */
    for (int ni = 0; ni < MAX_NPC_SHIPS; ni++) {
        const npc_ship_t *npc = &w->npc_ships[ni];
        if (!npc->active) continue;
        float npc_r = npc_hull_def(npc)->render_scale * 16.0f;
        vec2 hit; float along;
        if (laser_target_in_beam(&ray, npc->ship.pos, npc_r, &hit, &along)
            && along < best_dist) {
            best_dist = along;
            sp->scan_target_type = 2;
            sp->scan_target_index = ni;
            sp->scan_module_index = -1;
            sp->beam_end = hit;
        }
    }

    /* Check other players */
    for (int pi = 0; pi < MAX_PLAYERS; pi++) {
        const server_player_t *other = &w->players[pi];
        if (!other->connected || other->id == sp->id) continue;
        float pr = ship_hull_def(&other->ship)->ship_radius;
        vec2 hit; float along;
        if (laser_target_in_beam(&ray, other->ship.pos, pr, &hit, &along)
            && along < best_dist) {
            best_dist = along;
            sp->scan_target_type = 3;
            sp->scan_target_index = pi;
            sp->scan_module_index = -1;
            sp->beam_end = hit;
        }
    }

    return sp->scan_target_type != 0;
}

static void step_mining_system(world_t *w, server_player_t *sp, float dt, bool mining, vec2 forward, float cached_signal) {
    /* Beam state is server-authoritative — client prediction must NOT touch it.
     * Server PLAYER_STATE messages set beam_active/start/end/hit fields directly.
     * This matters for autopilot (server drives intent.mine, client's intent is false)
     * and for future combat prediction. */
    if (w->player_only_mode) return;

    sp->beam_active = false;
    sp->beam_hit = false;
    sp->beam_ineffective = false;
    sp->scan_active = false;
    if (!mining) return;

    vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);
    sp->beam_active = true;
    sp->beam_start = muzzle;

    if (sp->hover_asteroid >= 0) {
        asteroid_t *a = &w->asteroids[sp->hover_asteroid];
        if (!a->active || asteroid_is_collectible(a)) {
            sp->hover_asteroid = -1;
            a = NULL;
        }
        if (a == NULL) {
            if (find_scan_target(w, sp, muzzle, forward)) {
                sp->scan_active = true;
                sp->beam_hit = true;
            } else {
                sp->beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
            }
            return;
        }
        /* Shared mining-beam kernel: range/cone/tier/signal/damage all
         * applied identically here as in NPC fire. Player owns
         * hover_asteroid acquisition (cone search + manual hint), the
         * helper owns "given that target, what does one tick do?" */
        mining_beam_t mb = sim_mining_beam_step(w, muzzle, forward,
            sp->hover_asteroid, sp->ship.mining_level,
            ship_mining_rate(&sp->ship), signal_mining_efficiency(cached_signal),
            (int8_t)sp->id, dt);
        sp->beam_end = mb.beam_end;
        sp->beam_hit = mb.hit;
        sp->beam_ineffective = mb.ineffective;
        if (mb.fired)
            emit_event(w, (sim_event_t){.type = SIM_EVENT_MINING_TICK, .player_id = sp->id});
        if (mb.fractured)
            sp->ship.stat_asteroids_fractured++;
    } else {
        /* No asteroid target — check for scan targets */
        if (find_scan_target(w, sp, muzzle, forward)) {
            sp->scan_active = true;
            sp->beam_hit = true;
        } else {
            sp->beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
        }
    }
}

/* --- Economy ledger helpers --- */

/* Find or create a ledger entry for a player at a station.
 * When the 16-slot table is full, evict the entry with the smallest
 * lifetime_supply (the least-active contributor). Their balance is
 * dropped on eviction; since pool is derived from -Σ(balance),
 * removing an entry naturally absorbs its balance back into the
 * station's net issuance. */
/* Find or create a ledger entry keyed by player pubkey (#257 #479).
 * Ledger entries are now keyed by Ed25519 pubkey (32B) instead of
 * session token (8B), so relationships survive token rotation. */
int ledger_find_or_create_by_pubkey(station_t *st, const uint8_t pubkey[32]) {
    if (!pubkey) return -1;
    /* Check if all zeros — anonymous player without registered identity */
    bool is_zero = true;
    for (int j = 0; j < 32; j++) {
        if (pubkey[j] != 0) {
            is_zero = false;
            break;
        }
    }
    if (is_zero) return -1;

    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_pubkey, pubkey, 32) == 0) return i;
    }
    int idx;
    if (st->ledger_count < 16) {
        idx = st->ledger_count++;
    } else {
        /* Evict least-supplied. Ties resolved by lowest index. */
        int evict = 0;
        float worst = st->ledger[0].lifetime_supply;
        for (int i = 1; i < 16; i++) {
            if (st->ledger[i].lifetime_supply < worst) {
                worst = st->ledger[i].lifetime_supply;
                evict = i;
            }
        }
        idx = evict;
    }
    memcpy(st->ledger[idx].player_pubkey, pubkey, 32);
    st->ledger[idx].balance = 0.0f;
    st->ledger[idx].lifetime_supply = 0.0f;
    st->ledger[idx].first_dock_tick = 0;
    st->ledger[idx].last_dock_tick = 0;
    st->ledger[idx].total_docks = 0;
    st->ledger[idx].lifetime_ore_units = 0;
    st->ledger[idx].lifetime_credits_in = 0;
    st->ledger[idx].lifetime_credits_out = 0;
    st->ledger[idx].top_commodity = 0;
    memset(st->ledger[idx]._pad, 0, 3);
    return idx;
}

/* Credit a player's ledger when they supply ore to a station.
 * Pays from the station's credit pool — pool may go negative (the
 * station carries the debt). Total system credits are still conserved.
 * Returns the actual amount credited so callers can emit accurate +N
 * events. Token form runs through the pseudo-pubkey shim so legacy
 * callers stay working. */
float ledger_credit_supply_amount(station_t *st, const uint8_t *token, float ore_value) {
    uint8_t pseudo[32];
    token_to_pseudo_pubkey(token, pseudo);
    return ledger_credit_supply_amount_by_pubkey(st, pseudo, ore_value);
}

void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value) {
    (void)ledger_credit_supply_amount(st, token, ore_value);
}

/* Hail: report station-local balance (informational — no withdrawal). */
/* Hail-as-quest: if the player has no open claimed contract, pick an
 * appropriate one — FRACTURE if they're empty-holded, TRACTOR if they
 * already have fragments/ore on board. Returns slot index, or -1 if
 * the contract pool is full. Reuses an existing claimed contract when
 * present so H-mashing doesn't spam the board. */
static int hail_find_or_issue_contract(world_t *w, server_player_t *sp, int issuer_station) {
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        contract_t *c = &w->contracts[i];
        if (c->active && c->claimed_by == (int8_t)sp->id) return i;
    }
    int slot = -1;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!w->contracts[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    contract_t *c = &w->contracts[slot];
    memset(c, 0, sizeof(*c));

    /* FRACTURE: nearest mineable rock in signal coverage. Raw-ore TRACTOR
     * contracts were removed because players can't carry ore — fragments
     * ride in ship.towed_fragments[] and smelt directly on the beam. */
    int best_a = -1;
    float best_d = 1e18f;
    for (int a = 0; a < MAX_ASTEROIDS; a++) {
        asteroid_t *ast = &w->asteroids[a];
        if (!ast->active) continue;
        if (ast->tier == ASTEROID_TIER_S) continue;
        if (signal_strength_at(w, ast->pos) <= 0.0f) continue;
        float d = v2_dist_sq(sp->ship.pos, ast->pos);
        if (d < best_d) { best_d = d; best_a = a; }
    }
    if (best_a < 0) return -1;
    c->active = true;
    c->action = CONTRACT_FRACTURE;
    c->station_index = (uint8_t)((issuer_station >= 0) ? issuer_station : 0);
    c->target_pos = w->asteroids[best_a].pos;
    c->target_index = best_a;
    c->base_price = 25.0f;
    c->claimed_by = (int8_t)sp->id;
    return slot;
}

static void handle_hail(world_t *w, server_player_t *sp) {
    /* Docked hail: the station the player is sitting in should answer
     * immediately. This keeps H from feeling dead on the station screen
     * and uses the same response/contract path as an undocked ping. */
    if (sp->docked) {
        int docked_station = sp->current_station;
        if (docked_station >= 0 && docked_station < MAX_STATIONS &&
            station_is_active(&w->stations[docked_station])) {
            float balance = sp->pubkey_set
                ? ledger_balance_by_pubkey(&w->stations[docked_station], sp->pubkey)
                : ledger_balance(&w->stations[docked_station], sp->session_token);
            int contract_idx = hail_find_or_issue_contract(w, sp, docked_station);
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_HAIL_RESPONSE,
                .player_id = sp->id,
                .hail_response = { .station = docked_station, .credits = balance, .contract_index = contract_idx },
            });
        } else {
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_HAIL_RESPONSE,
                .player_id = sp->id,
                .hail_response = { .station = -1, .credits = -1.0f, .contract_index = -1 },
            });
        }
        return;
    }

    /* Ship-based ping: the player is the transmitter. Stations inside
     * comm_range respond normally; stations within 2× comm_range chirp
     * back "out of range" with a bearing so the player knows which way
     * to fly. If nothing is even in chirp range, the client gets an
     * explicit miss notice instead of a dead keypress.
     *
     * credits == -1.0f is the out-of-range sentinel on the wire; the
     * client renders a short "too far -- nearest: <name>" notice
     * instead of the full hail UI. */
    float comm = (sp->ship.comm_range > 0.0f) ? sp->ship.comm_range : 1500.0f;
    float comm_sq = comm * comm;
    float chirp_sq = (2.0f * comm) * (2.0f * comm);

    int nearest_in = -1;    float best_in = 1e18f;
    int nearest_chirp = -1; float best_chirp = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        float d_sq = v2_dist_sq(sp->ship.pos, st->pos);
        if (d_sq <= comm_sq) {
            if (d_sq < best_in) { best_in = d_sq; nearest_in = s; }
        } else if (d_sq <= chirp_sq) {
            if (d_sq < best_chirp) { best_chirp = d_sq; nearest_chirp = s; }
        }
    }

    if (nearest_in >= 0) {
        /* Hail-credits display must read the same identity the player's
         * earnings landed on — pubkey when registered, else session token. */
        float balance = sp->pubkey_set
            ? ledger_balance_by_pubkey(&w->stations[nearest_in], sp->pubkey)
            : ledger_balance(&w->stations[nearest_in], sp->session_token);
        int contract_idx = hail_find_or_issue_contract(w, sp, nearest_in);
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_HAIL_RESPONSE,
            .player_id = sp->id,
            .hail_response = { .station = nearest_in, .credits = balance, .contract_index = contract_idx },
        });
    } else if (nearest_chirp >= 0) {
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_HAIL_RESPONSE,
            .player_id = sp->id,
            .hail_response = { .station = nearest_chirp, .credits = -1.0f, .contract_index = -1 },
        });
    } else {
        /* No station even in chirp range: send an explicit miss so H never
         * feels broken. Remote clients receive station=255; local clients see
         * the negative station index directly. */
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_HAIL_RESPONSE,
            .player_id = sp->id,
            .hail_response = { .station = -1, .credits = -1.0f, .contract_index = -1 },
        });
    }
}

static void step_station_interaction_system(world_t *w, server_player_t *sp, const input_intent_t *intent) {
    /* Order scaffold from shipyard: queues build + generates material contract */
    if (intent->buy_scaffold_kit && sp->docked && !w->player_only_mode) {
        module_type_t kit_type = intent->scaffold_kit_module;
        station_t *st = &w->stations[sp->current_station];
        if (!station_sells_scaffold(st, kit_type)) {
            emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id,
                .order_rejected = { .reason = ORDER_REJECT_SHIPYARD_NOT_SOLD }});
        } else if (st->pending_scaffold_count >= 4) {
            emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id,
                .order_rejected = { .reason = ORDER_REJECT_SHIPYARD_QUEUE_FULL }});
        } else if (!module_unlocked_for_player(sp->ship.unlocked_modules, kit_type)) {
            /* Tech tree gate: prereq not yet unlocked */
            emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id,
                .order_rejected = { .reason = ORDER_REJECT_SHIPYARD_LOCKED }});
        } else {
            float fee = (float)scaffold_order_fee(kit_type);
            bool can_afford = sp->pubkey_set ?
                ledger_spend_by_pubkey(st, sp->pubkey, fee, &sp->ship) :
                ledger_spend(st, sp->session_token, fee, &sp->ship);
            if (!can_afford) {
                emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id,
                    .order_rejected = { .reason = ORDER_REJECT_SHIPYARD_NO_FUNDS }});
            } else {
                /* Tech tree: ordering this type unlocks any module that
                 * lists it as prerequisite. */
                sp->ship.unlocked_modules |= (1u << (uint32_t)kit_type);
                /* Queue pending scaffold */
                int idx = st->pending_scaffold_count++;
                st->pending_scaffolds[idx].type = kit_type;
                st->pending_scaffolds[idx].owner = (int8_t)sp->id;
                /* Generate supply contract for the material */
                commodity_t mat = module_build_material(kit_type);
                float needed = module_build_cost(kit_type);
                for (int k = 0; k < MAX_CONTRACTS; k++) {
                    if (!w->contracts[k].active) {
                        w->contracts[k] = (contract_t){
                            .active = true, .action = CONTRACT_TRACTOR,
                            .station_index = (uint8_t)sp->current_station,
                            .commodity = mat,
                            .quantity_needed = needed,
                            .base_price = st->base_price[mat] * 1.15f,
                            .target_index = -1, .claimed_by = -1,
                        };
                        break;
                    }
                }
                SIM_LOG("[sim] player %d ordered %s scaffold at station %d\n",
                        sp->id, module_type_name(kit_type), sp->current_station);
            }
        }
    }
    /* Outpost / module placement via towed scaffold + reticle. */
    if (intent->place_outpost && !sp->docked && sp->ship.towed_scaffold >= 0) {
        place_towed_scaffold(w, sp);
        return;
    }
    if (intent->interact) {
        if (sp->docked) { launch_ship(w, sp); return; }
        if (sp->in_dock_range) {
            const station_t *dock_st = &w->stations[sp->nearby_station];
            int berth = find_best_berth(w, dock_st, sp->nearby_station, sp->ship.pos);
            sp->dock_berth = berth;
            vec2 bp = dock_berth_pos(dock_st, berth);
            float d = sqrtf(v2_dist_sq(sp->ship.pos, bp));
            if (d <= DOCK_SNAP_DISTANCE) {
                dock_ship(w, sp);
            } else {
                sp->docking_approach = true;
            }
            return;
        }
    }
    /* Cancel docking approach if player thrusts away */
    if (sp->docking_approach && (intent->thrust > 0.1f || intent->thrust < -0.1f)) {
        sp->docking_approach = false;
    }
    if (!sp->docked) return;
    station_t *docked_st = &w->stations[sp->current_station];
    if (intent->service_sell) {
        /* Deliver to scaffolds/modules first, then sell remaining.
         * Honor service_sell_only as a filter: when the player picks a
         * specific commodity row, scaffold/module delivery should not
         * eat unrelated build materials (e.g. selecting "deliver
         * ingots" must not silently pour frames into a scaffold). */
        commodity_t filter = intent->service_sell_only;
        bool deliver_frames = (filter == COMMODITY_COUNT) || (filter == COMMODITY_FRAME);
        if (deliver_frames) step_scaffold_delivery(w, sp);
        float build_payout = step_module_delivery(w, docked_st,
                                                  sp->current_station,
                                                  &sp->ship, filter);
        if (build_payout > 0.01f) {
            if (sp->pubkey_set) {
                ledger_earn_by_pubkey(docked_st, sp->pubkey, build_payout);
            } else {
                ledger_earn(docked_st, sp->session_token, build_payout);
            }
            sp->ship.stat_credits_earned += build_payout;
            int base_cr = (int)lroundf(build_payout);
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_SELL, .player_id = sp->id,
                .sell = { .station = sp->current_station,
                          .grade = (uint8_t)MINING_GRADE_COMMON,
                          .base_cr = base_cr,
                          .bonus_cr = 0,
                          .by_contract = 0 }});
            SIM_LOG("[sim] player %d delivered build materials for %.0f cr at %s\n",
                    sp->id, build_payout, docked_st->name);
        }
        try_sell_station_cargo(w, sp);
    }
    else if (intent->service_repair) try_repair_ship(w, sp);
    else if (intent->upgrade_mining) try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_MINING);
    else if (intent->upgrade_hold)   try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_HOLD);
    else if (intent->upgrade_tractor)try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_TRACTOR);
    /* Buy ingots from station inventory */
    if (intent->buy_product && !w->player_only_mode) {
        commodity_t c = intent->buy_commodity;
        SIM_LOG("[buy] player %d req c=%d grade=%d at station %d (produces=%d)\n",
                sp->id, (int)c, (int)intent->buy_grade,
                sp->current_station,
                (c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT) ?
                    station_produces(docked_st, c) : -1);
        if (c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT
            && station_produces(docked_st, c)) {
            /* Manifest is authoritative for finished goods. Reading the
             * float here previously rejected buys that the picker had
             * advertised. Named ingots are deliberately excluded from
             * generic BUY_PRODUCT so the market cannot transfer a prefix-
             * premium collectible at bulk price; those use BUY_INGOT. */
            float available = (float)manifest_count_market_buy_units(
                &docked_st->manifest, c, intent->buy_grade);
            float free_volume = ship_cargo_capacity(&sp->ship) - ship_total_cargo(&sp->ship);
            float vol = commodity_volume(c);
            float space = (vol > FLOAT_EPSILON) ? (free_volume / vol) : free_volume;
            float price_base = station_sell_price(docked_st, c);
            /* Grade-aware pricing: if the client picked a specific-grade row
             * in TRADE the row's displayed price was base * grade_multiplier,
             * so charge the same here. Sentinel MINING_GRADE_COUNT keeps the
             * legacy any-grade path at 1.0x. */
            float grade_mult = (intent->buy_grade < MINING_GRADE_COUNT)
                ? mining_payout_multiplier((mining_grade_t)intent->buy_grade)
                : 1.0f;
            float price_per = price_base * grade_mult;
            /* One unit per intent. The TRADE picker advertises a row
             * as "buy 1 frame for $X" — the old "buy as much as you
             * can afford" behavior could drain a station's stock and
             * the player's wallet from a single keypress, charging the
             * row's grade-multiplied price across the whole quantity
             * even though manifest_transfer falls back to other grades
             * once the named ones run out. Bulk-buy can come back as
             * an explicit `buy_quantity` intent if/when needed. */
            /* Balance and spend MUST use the same identity the SELL path
             * credits — pubkey when registered (the real Ed25519 entry),
             * else the session-token-hashed pseudokey. Mismatch here was
             * the "client predicts buy, server snaps it back" bug:
             * earnings landed on the pubkey ledger entry but the buy
             * path was reading the (empty) session-token entry. */
            float bal = sp->pubkey_set
                ? ledger_balance_by_pubkey(docked_st, sp->pubkey)
                : ledger_balance(docked_st, sp->session_token);
            SIM_LOG("[buy-bal] player %d at station %d: pubkey_set=%d pk_prefix=%02x%02x%02x%02x bal=%.2f ledger_count=%d\n",
                    sp->id, sp->current_station, sp->pubkey_set ? 1 : 0,
                    sp->pubkey[0], sp->pubkey[1], sp->pubkey[2], sp->pubkey[3],
                    bal, docked_st->ledger_count);
            for (int li = 0; li < docked_st->ledger_count; li++) {
                const uint8_t *lpk = docked_st->ledger[li].player_pubkey;
                (void)lpk;
                SIM_LOG("[buy-bal]   ledger[%d] pk=%02x%02x%02x%02x bal=%.2f\n",
                        li, lpk[0], lpk[1], lpk[2], lpk[3],
                        docked_st->ledger[li].balance);
            }
            float afford = (price_per > 0.01f) ? floorf(bal / price_per) : 0.0f;
            /* Per-press unit cap scales by commodity density: standard
             * goods (vol = 1.0) buy 1 per press; dense goods like
             * repair kits (vol = 0.1) buy 10 per press so a single
             * keystroke fills one cargo unit's worth. */
            int per_press_cap = (vol > FLOAT_EPSILON)
                ? (int)lroundf(1.0f / vol) : 1;
            if (per_press_cap < 1) per_press_cap = 1;
            float amount = fminf(fminf(fminf(available, space), afford), (float)per_press_cap);
            float total_cost = amount * price_per;
            SIM_LOG("[buy] avail=%.2f space=%.2f price/u=%.2f bal=%.2f afford=%.0f amount=%.2f\n",
                    available, space, price_per, bal, afford, amount);
            /* Finished goods: manifest is authoritative. Move the unit
             * first, then charge for what actually moved. Prevents the
             * silent-drift bug where float said "got 1 frame" but the
             * manifest had nothing to give and the player paid anyway. */
            bool finished = (c >= COMMODITY_RAW_ORE_COUNT);
            int whole = (int)floorf(amount + 0.0001f);
            int moved = 0;
            float charge_amount = amount;
            float charge_cost = total_cost;
            if (finished) {
                if (whole <= 0) {
                    SIM_LOG("[buy] REJECT: finished good but whole=%d (amount=%.2f)\n",
                            whole, amount);
                    return;
                }
                moved = manifest_transfer_market_buy(
                    &docked_st->manifest, &sp->ship.manifest,
                    c, intent->buy_grade, whole);
                if (moved <= 0) {
                    SIM_LOG("[buy] REJECT: manifest had no transferable unit for c=%d\n", (int)c);
                    return;
                }
                charge_amount = (float)moved;
                charge_cost = charge_amount * price_per;
            }
            bool spent = false;
            if (charge_amount > 0.01f) {
                spent = sp->pubkey_set
                    ? ledger_spend_by_pubkey(docked_st, sp->pubkey, charge_cost, &sp->ship)
                    : ledger_spend(docked_st, sp->session_token, charge_cost, &sp->ship);
            }
            if (spent) {
                if (finished) {
                    sync_ship_finished_cargo(&sp->ship, c);
                    sync_station_finished_inventory(docked_st, c);
                } else {
                    sp->ship.cargo[c] += charge_amount;
                    docked_st->_inventory_cache[c] -= charge_amount;
                    if (docked_st->_inventory_cache[c] < 0.0f) docked_st->_inventory_cache[c] = 0.0f;
                }
                if (!finished && whole > 0) {
                    moved = manifest_transfer_by_commodity_ex(
                        &docked_st->manifest, &sp->ship.manifest,
                        c, intent->buy_grade, whole);
                }
                SIM_LOG("[buy] OK player %d bought %.0f of c=%d for %.0f cr (manifest moved=%d)\n",
                        sp->id, charge_amount, c, charge_cost, moved);
            } else if (charge_amount > 0.01f) {
                if (finished && moved > 0) {
                    /* Roll back the manifest move — payment failed. */
                    (void)manifest_transfer_by_commodity_ex(
                        &sp->ship.manifest, &docked_st->manifest,
                        c, intent->buy_grade, moved);
                }
                SIM_LOG("[buy] REJECT: ledger_spend failed (bal=%.2f cost=%.2f)\n",
                        bal, total_cost);
            } else {
                SIM_LOG("[buy] REJECT: amount=%.2f too small (avail=%.2f space=%.2f afford=%.0f)\n",
                        amount, available, space, afford);
            }
        } else {
            SIM_LOG("[buy] REJECT: c=%d out of range or station doesn't produce\n", (int)c);
        }
    }
}

/* ================================================================== */
/* step_player -- one player per tick                                 */
/* ================================================================== */

/* Calculate signal interference from nearby objects.  Returns 0..1
 * where 0 = clean signal, 1 = maximum interference. */
static float calc_signal_interference(const world_t *w, const server_player_t *sp) {
    float interference = 0.0f;
    vec2 pos = sp->ship.pos;

    /* Other players — strong interference at close range */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!w->players[i].connected || w->players[i].docked) continue;
        if (&w->players[i] == sp) continue;
        float dist_sq = v2_dist_sq(pos, w->players[i].ship.pos);
        if (dist_sq < 200.0f * 200.0f) {
            float d = sqrtf(dist_sq);
            float strength = (200.0f - d) / 200.0f;
            interference += strength * 0.5f;
        }
    }

    /* Large asteroids — mass creates interference */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        float range = a->radius * 3.0f;
        float dist_sq = v2_dist_sq(pos, a->pos);
        if (dist_sq < range * range) {
            float d = sqrtf(dist_sq);
            float strength = (range - d) / range;
            float mass_factor = a->radius / 80.0f;  /* bigger = more interference */
            interference += strength * mass_factor * 0.15f;
        }
    }

    return clampf(interference, 0.0f, 0.7f);  /* cap at 70% interference */
}

/* Player autopilot extracted to sim_autopilot.c (#272 slice). */




static void step_player(world_t *w, server_player_t *sp, float dt) {
    /* One-shot: toggle autopilot from network action. */
    if (sp->input.toggle_autopilot) {
        if (sp->autopilot_mode) {
            /* Turning OFF — always allowed. */
            sp->autopilot_mode = 0;
        } else {
            /* Turning ON — requires 80%+ signal. */
            float sig = signal_strength_at(w, sp->ship.pos);
            if (sig >= 0.80f) {
                sp->autopilot_mode = 1;
                if (sp->ship.towed_count > 0) {
                    sp->autopilot_state = AUTOPILOT_STEP_RETURN_TO_REFINERY;
                } else {
                    sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
                }
                sp->autopilot_target = -1;
                sp->autopilot_timer = 0.0f;
            }
        }
        sp->input.toggle_autopilot = false;
    }

    /* Snapshot the network-provided continuous inputs BEFORE the autopilot
     * gets a chance to overwrite them. The MP server sub-steps at 120Hz
     * but parse_input only runs when a NET_MSG_INPUT arrives (~30Hz), so
     * sp->input is the last network state. The autopilot writes turn /
     * thrust / mine each tick to drive physics — without this snapshot,
     * the NEXT sub-step's manual-override check would see the autopilot's
     * own writes and cancel itself after one frame. We restore at the end
     * so sp->input continues to reflect "what the player actually pressed."
     */
    float net_turn   = sp->input.turn;
    float net_thrust = sp->input.thrust;
    bool  net_mine   = sp->input.mine;
    int   net_target = sp->input.mining_target_hint;

    /* Autopilot requires 80%+ signal strength. If signal drops below
     * that threshold, disengage — the ship is too far from a relay. */
    if (sp->autopilot_mode && !w->player_only_mode) {
        float ap_sig = signal_strength_at(w, sp->ship.pos);
        if (ap_sig < 0.80f) {
            sp->autopilot_mode = 0;
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
            emit_event(w, (sim_event_t){.type = SIM_EVENT_SIGNAL_LOST, .player_id = sp->id});
        }
    }

    /* Manual override: any directional / mining input cancels autopilot.
     * Checks the snapshot, NOT sp->input — autopilot writes don't count. */
    if (sp->autopilot_mode && !w->player_only_mode) {
        bool manual_input =
            fabsf(net_turn) > 0.01f ||
            fabsf(net_thrust) > 0.01f ||
            net_mine ||
            sp->input.release_tow ||
            sp->input.reset;
        if (manual_input) {
            sp->autopilot_mode = 0;
            sp->autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
            sp->autopilot_target = -1;
        } else {
            step_autopilot(w, sp, dt);
        }
    }

    /* Self-destruct: X key */
    if (sp->input.reset && !sp->docked) {
        sp->ship.hull = 0.0f;
        emergency_recover_ship(w, sp);
        return;
    }
    /* Mark that we still need to restore inputs at end of step_player */
    bool restore_net_input = sp->autopilot_mode != 0;

    sp->hover_asteroid = -1;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    /* In client prediction mode (player_only_mode) with autopilot,
     * zero local inputs so we don't fight the server's steering.
     * Motion physics (drag + position) still runs for smooth camera. */
    if (sp->autopilot_mode && w->player_only_mode) {
        sp->input.turn = 0.0f;
        sp->input.thrust = 0.0f;
        sp->input.mine = false;
    }

    if (!sp->docked) {
        /* Signal attenuation: scale controls by station signal strength */
        float sig = signal_strength_at(w, sp->ship.pos);
        bool in_signal = sig > 0.01f;
        if (sp->was_in_signal && !in_signal) {
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_SIGNAL_LOST, .player_id = sp->id,
            });
        }
        sp->was_in_signal = in_signal;
        float signal_scale = signal_control_scale(sig);
        float turn_input = sp->input.turn * signal_scale;
        float thrust_input = sp->input.thrust * signal_scale;

        /* Signal interference: nearby objects add noise to controls */
        float interference = calc_signal_interference(w, sp);
        if (interference > 0.01f) {
            /* Add jitter to controls proportional to interference.
             * Use a local RNG seeded from player position to avoid
             * mutating world RNG state (bug 47). */
            /* Bit-cast floats to uint32 to avoid UB from negative float→uint. */
            float rx = sp->ship.pos.x * 1000.0f, ry = sp->ship.pos.y * 1000.0f;
            uint32_t ux, uy;
            memcpy(&ux, &rx, sizeof(ux));
            memcpy(&uy, &ry, sizeof(uy));
            uint32_t local_rng = ux ^ uy ^ ((uint32_t)sp->id * 0x9E3779B9u);
            if (local_rng == 0) local_rng = 0xA341316Cu;
            local_rng ^= local_rng << 13; local_rng ^= local_rng >> 17; local_rng ^= local_rng << 5;
            float r1 = (float)(local_rng & 0x00FFFFFFu) / 16777215.0f;
            local_rng ^= local_rng << 13; local_rng ^= local_rng >> 17; local_rng ^= local_rng << 5;
            float r2 = (float)(local_rng & 0x00FFFFFFu) / 16777215.0f;
            float noise_turn = (r1 - 0.5f) * 2.0f * interference;
            float noise_thrust = (r2 - 0.5f) * 0.6f * interference;
            turn_input += noise_turn;
            thrust_input = clampf(thrust_input + noise_thrust, -1.0f, 1.0f);
        }

        vec2 forward = ship_forward(sp->ship.angle);
        step_ship_rotation(&sp->ship, dt, turn_input);
        forward = ship_forward(sp->ship.angle);           /* refresh after rotation */
        bool boost = sp->input.boost && !sp->docked;
        if (boost) sp->boost_hold_timer += dt;
        else       sp->boost_hold_timer  = 0.0f;
        step_ship_thrust(&sp->ship, dt, thrust_input, forward, boost, sp->boost_hold_timer,
                         sp->input.reverse_thrust);
        step_ship_boost_drain(w, sp, dt, boost, turn_input);
        step_ship_motion(&sp->ship, dt, w, sig);
        /* Tow drag: each fragment adds drag, slowing the ship */
        if (sp->ship.towed_count > 0) {
            float tow_drag = 0.15f * (float)sp->ship.towed_count;
            sp->ship.vel = v2_scale(sp->ship.vel, 1.0f / (1.0f + tow_drag * dt));
        }
        /* Scaffold tow drag: heavy — ship feels the mass. Speed cap
         * scales with engine accel (so the ship and the scaffold are
         * limited by the same engine-coupled cap). */
        if (sp->ship.towed_scaffold >= 0) {
            sp->ship.vel = v2_scale(sp->ship.vel, 1.0f / (1.0f + 0.8f * dt));
            float tow_cap = scaffold_tow_speed_cap(ship_hull_def(&sp->ship));
            float spd = v2_len(sp->ship.vel);
            if (spd > tow_cap)
                sp->ship.vel = v2_scale(sp->ship.vel, tow_cap / spd);
        }
        /* Skip collision in client prediction — authoritative server handles it.
         * Running collision on both client and server worlds with slightly
         * different ring rotations causes jitter and invisible walls. */
        if (!w->player_only_mode)
            resolve_world_collisions(w, sp);
        update_docking_state(w, sp, dt);
        /* In client prediction mode (player_only_mode), skip station
         * interactions — the server is authoritative for dock/launch,
         * sell, repair, and upgrades.  This prevents snap-back flicker
         * when the client predicts an action before the server confirms. */
        if (!w->player_only_mode)
            step_station_interaction_system(w, sp, &sp->input);
        /* Undocked module interactions (laser-to-activate) */
        if (!sp->docked && sp->in_dock_range && sp->nearby_station >= 0 && !w->player_only_mode) {
            station_t *nearby_st = &w->stations[sp->nearby_station];
            if (sp->input.buy_product) {
                commodity_t c = sp->input.buy_commodity;
                if (c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT) {
                    float available = (float)manifest_count_market_buy_units(
                        &nearby_st->manifest, c, sp->input.buy_grade);
                    float base = station_sell_price(nearby_st, c);
                    float gmult = (sp->input.buy_grade < MINING_GRADE_COUNT)
                        ? mining_payout_multiplier((mining_grade_t)sp->input.buy_grade)
                        : 1.0f;
                    float price_per = base * gmult;
                    /* Same pubkey-vs-session-token identity rule as the
                     * docked-buy path. */
                    float nbal = sp->pubkey_set
                        ? ledger_balance_by_pubkey(nearby_st, sp->pubkey)
                        : ledger_balance(nearby_st, sp->session_token);
                    float afford = (price_per > FLOAT_EPSILON) ? floorf(nbal / price_per) : 0.0f;
                    float amount = fminf(fminf(available, 1.0f), afford); /* buy 1 at a time */
                    if (amount > FLOAT_EPSILON) {
                        int whole = (int)floorf(amount + 0.0001f);
                        int moved = manifest_transfer_market_buy(
                            &nearby_st->manifest, &sp->ship.manifest,
                            c, sp->input.buy_grade, whole);
                        if (moved <= 0) {
                            sp->input.buy_product = false;
                            return;
                        }
                        float cost = (float)moved * price_per;
                        bool spent = sp->pubkey_set
                            ? ledger_spend_by_pubkey(nearby_st, sp->pubkey, cost, &sp->ship)
                            : ledger_spend(nearby_st, sp->session_token, cost, &sp->ship);
                        if (spent) {
                            sync_ship_finished_cargo(&sp->ship, c);
                            sync_station_finished_inventory(nearby_st, c);
                            emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = sp->id});
                        } else {
                            (void)manifest_transfer_by_commodity_ex(
                                &sp->ship.manifest, &nearby_st->manifest,
                                c, sp->input.buy_grade, moved);
                        }
                    }
                }
            }
            /* Repair is now passive while docked — no laser interaction needed */
        }
        if (!sp->docked) {
            update_targeting_state(w, sp, forward);
            step_mining_system(w, sp, dt, sp->input.mine, forward, sig);
            if (!w->player_only_mode) {
                /* Hold R = tractor active; tap R = release fragments + scaffold */
                sp->ship.tractor_active = sp->input.tractor_hold;
                if (sp->input.release_tow) {
                    release_towed_fragments(w, sp);
                    release_towed_scaffold(w, sp);
                }
                step_towed_cleanup(w, sp);
                if (sp->ship.tractor_active)
                    step_fragment_collection(w, sp, dt);
                else if (sp->ship.towed_count > 0)
                    step_leashed_fragments(w, sp, dt);
                step_scaffold_tow(w, sp, dt);

                /* B while towing scaffold = place it (snap to outpost or found station) */
                if (sp->input.place_outpost && sp->ship.towed_scaffold >= 0) {
                    place_towed_scaffold(w, sp);
                    sp->input.place_outpost = false; /* consume the intent */
                }

                /* Laser-to-snap: firing at a scaffold triggers snap if near open slot */
                if (sp->input.mine && sp->beam_active) {
                    for (int si = 0; si < MAX_SCAFFOLDS; si++) {
                        scaffold_t *sc = &w->scaffolds[si];
                        if (!sc->active || sc->state != SCAFFOLD_LOOSE) continue;
                        float d_sq = v2_dist_sq(sp->beam_end, sc->pos);
                        if (d_sq > (sc->radius + 30.0f) * (sc->radius + 30.0f)) continue;
                        /* Hit — check if near a player outpost open slot */
                        for (int s = 3; s < MAX_STATIONS; s++) {
                            station_t *st = &w->stations[s];
                            if (!station_is_active(st)) continue;
                            int ring, slot;
                            if (find_nearest_open_slot(st, sc->pos, &ring, &slot)) {
                                sc->state = SCAFFOLD_SNAPPING;
                                sc->placed_station = s;
                                sc->placed_ring = ring;
                                sc->placed_slot = slot;
                                sc->vel = v2(0.0f, 0.0f);
                                /* Release from tow if we were towing it */
                                if (sp->ship.towed_scaffold == si)
                                    sp->ship.towed_scaffold = -1;
                                break;
                            }
                        }
                        break; /* only one scaffold per laser frame */
                    }
                }
            }
        }
    } else {
        update_docking_state(w, sp, dt);
        if (!w->player_only_mode)
            step_station_interaction_system(w, sp, &sp->input);
    }

    /* Hail: collect pending credits from nearby station(s) */
    if (sp->input.hail) {
        handle_hail(w, sp);
    }

    /* --- Outpost planning: create → add → cancel (order matters) --- */

    /* 1. Create a planned outpost (server-side ghost).
     * Runs FIRST so a combined CREATE_AND_ADD op can resolve the
     * plan_station=-2 sentinel for the add_plan below. */
    int just_created_planned_station = -1;
    if (sp->input.create_planned_outpost && !w->player_only_mode) {
        vec2 pos = sp->input.planned_outpost_pos;
        /* Faction-shared: only one planned outpost in the world at a time.
         * Any player creating a new blueprint cancels every existing one. */
        for (int s = 3; s < MAX_STATIONS; s++) {
            station_t *old = &w->stations[s];
            if (old->planned) {
                SIM_LOG("[sim] player %d cancelled blueprint at slot %d (was owner %d)\n",
                    sp->id, s, old->planned_owner);
                station_cleanup(old);
                memset(old, 0, sizeof(*old));
                (void)station_manifest_bootstrap(old);
            }
        }
        /* Validate position */
        bool too_close = false;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&w->stations[s])) continue;
            if (v2_dist_sq(w->stations[s].pos, pos) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) {
                too_close = true; break;
            }
        }
        /* Planning uses the unboosted signal — same reason as can_place_outpost. */
        float plan_sig = signal_strength_unboosted(w, pos);
        /* Reject: too close, no signal, or deep in an existing station's
         * core coverage (>= OPERATIONAL band). */
        if (!too_close && plan_sig > 0.0f && plan_sig < OUTPOST_MAX_SIGNAL) {
            int slot = -1;
            for (int s = 3; s < MAX_STATIONS; s++) {
                if (!station_exists(&w->stations[s])) { slot = s; break; }
            }
            if (slot >= 0) {
                if (slot >= w->station_count) w->station_count = slot + 1;
                station_t *st = &w->stations[slot];
                station_cleanup(st);
                memset(st, 0, sizeof(*st));
                (void)station_manifest_bootstrap(st);
                st->id = w->next_station_id++;
                generate_outpost_name(st->name, sizeof(st->name), pos, slot);
                st->pos = pos;
                st->planned = true;
                st->planned_owner = (int8_t)sp->id;
                /* Layer B of #479: stamp the outpost's keypair at
                 * planning time. The founder's identity is locked in
                 * here — even if a different player later supplies the
                 * frames, the station's pubkey traces to the planner. */
                station_authority_init_outpost(st, sp->pubkey,
                                               (uint64_t)(w->time * 128.0f));
                st->radius = 0.0f;
                st->dock_radius = 0.0f;
                st->signal_range = 0.0f;
                st->arm_count = 0;
                for (int r = 0; r < MAX_ARMS; r++) {
                    st->arm_rotation[r] = 0.0f;
                    st->ring_offset[r] = 0.0f;
                    st->arm_speed[r] = 0.0f;
                }
                emit_event(w, (sim_event_t){
                    .type = SIM_EVENT_OUTPOST_PLACED,
                    .player_id = sp->id,
                    .outpost_placed = { .slot = slot },
                });
                just_created_planned_station = slot;
                SIM_LOG("[sim] player %d created planned outpost at slot %d\n", sp->id, slot);
            }
        }
    }

    /* 2. Add placement plan to a player outpost (active or planned).
     * plan_station=-2 is a sentinel: use the station just created above. */
    if (sp->input.add_plan && !w->player_only_mode) {
        int s = (sp->input.plan_station == -2 && just_created_planned_station >= 0)
                ? just_created_planned_station
                : (int)sp->input.plan_station;
        int ring = sp->input.plan_ring;
        int slot = sp->input.plan_slot;
        module_type_t type = sp->input.plan_type;
        if (s >= 3 && s < MAX_STATIONS && station_exists(&w->stations[s])
            && !w->stations[s].scaffold
            && ring >= 1 && ring <= STATION_NUM_RINGS
            && slot >= 0 && slot < STATION_RING_SLOTS[ring]
            && (int)type < MODULE_COUNT) {
            station_t *st = &w->stations[s];
            bool taken = false;
            for (int m = 0; m < st->module_count; m++)
                if (st->modules[m].ring == ring && st->modules[m].slot == slot) {
                    taken = true; break;
                }
            int existing = -1;
            for (int p = 0; p < st->placement_plan_count; p++) {
                if (st->placement_plans[p].ring == ring &&
                    st->placement_plans[p].slot == slot) {
                    existing = p; break;
                }
            }
            module_type_t distinct[PLAYER_PLAN_TYPE_LIMIT];
            int distinct_n = 0;
            for (int ss = 0; ss < MAX_STATIONS && distinct_n < PLAYER_PLAN_TYPE_LIMIT; ss++) {
                const station_t *sct = &w->stations[ss];
                for (int p = 0; p < sct->placement_plan_count; p++) {
                    if (sct == st && p == existing) continue;
                    module_type_t pt = sct->placement_plans[p].type;
                    bool dup = false;
                    for (int k = 0; k < distinct_n; k++)
                        if (distinct[k] == pt) { dup = true; break; }
                    if (!dup && distinct_n < PLAYER_PLAN_TYPE_LIMIT)
                        distinct[distinct_n++] = pt;
                }
            }
            bool already = false;
            for (int k = 0; k < distinct_n; k++)
                if (distinct[k] == type) { already = true; break; }
            bool over_cap = !already && distinct_n >= PLAYER_PLAN_TYPE_LIMIT;
            if (!taken && !over_cap) {
                if (existing >= 0) {
                    st->placement_plans[existing].type = type;
                    st->placement_plans[existing].owner = (int8_t)sp->id;
                } else if (st->placement_plan_count < 8) {
                    int idx = st->placement_plan_count++;
                    st->placement_plans[idx].type = type;
                    st->placement_plans[idx].ring = (uint8_t)ring;
                    st->placement_plans[idx].slot = (uint8_t)slot;
                    st->placement_plans[idx].owner = (int8_t)sp->id;
                }
            }
        }
    }

    /* 3. Cancel a single plan slot (red/clear state). */
    if (sp->input.cancel_plan_slot && !w->player_only_mode) {
        int s = sp->input.cancel_plan_st;
        int ring = sp->input.cancel_plan_ring;
        int slot = sp->input.cancel_plan_sl;
        if (s >= 3 && s < MAX_STATIONS && station_exists(&w->stations[s])) {
            station_t *st = &w->stations[s];
            for (int p = 0; p < st->placement_plan_count; p++) {
                if (st->placement_plans[p].ring == ring &&
                    st->placement_plans[p].slot == slot) {
                    for (int q = p; q < st->placement_plan_count - 1; q++)
                        st->placement_plans[q] = st->placement_plans[q + 1];
                    st->placement_plan_count--;
                    break;
                }
            }
        }
    }

    /* 4. Cancel a planned outpost (faction-shared — anyone can cancel). */
    if (sp->input.cancel_planned_outpost && !w->player_only_mode) {
        int s = sp->input.cancel_planned_station;
        if (s >= 3 && s < MAX_STATIONS) {
            station_t *st = &w->stations[s];
            if (st->planned) {
                station_cleanup(st);
                memset(st, 0, sizeof(*st));
                (void)station_manifest_bootstrap(st);
                SIM_LOG("[sim] player %d cancelled planned outpost at slot %d\n", sp->id, s);
            }
        }
    }

    /* Clear one-shot action flags after the sim has consumed them. */
    sp->input.interact = false;
    sp->input.service_sell = false;
    sp->input.service_repair = false;
    sp->input.upgrade_mining = false;
    sp->input.upgrade_hold = false;
    sp->input.upgrade_tractor = false;
    sp->input.place_outpost = false;
    sp->input.buy_scaffold_kit = false;
    sp->input.buy_product = false;
    sp->input.hail = false;
    sp->input.release_tow = false;
    sp->input.add_plan = false;
    sp->input.create_planned_outpost = false;
    sp->input.cancel_planned_outpost = false;
    sp->input.cancel_plan_slot = false;

    /* Snapshot actual thrust state BEFORE restoring manual inputs.
     * This survives the restore so serialization and mirroring see it. */
    sp->actual_thrusting = (sp->input.thrust > 0.01f) && !sp->docked;

    /* Restore the network-provided continuous inputs so the autopilot's
     * per-tick writes don't leak into the next sub-step's manual-override
     * check. parse_input on the next NET_MSG_INPUT will overwrite these
     * with whatever the player is actually pressing. */
    if (restore_net_input) {
        sp->input.turn = net_turn;
        sp->input.thrust = net_thrust;
        sp->input.mine = net_mine;
        sp->input.mining_target_hint = net_target;
    }
}

/* step_asteroid_gravity → sim_physics.c
 * step_furnace_smelting → sim_production.c
 * resolve_asteroid_collisions → sim_physics.c
 * resolve_asteroid_station_collisions → sim_physics.c */

/* ================================================================== */
/* Contract system                                                    */
/* ================================================================== */

float contract_price(const contract_t *c) {
    /* Price escalates with age: +20% per 5 minutes, capped at +20% */
    float escalation = 1.0f + fminf(c->age / 300.0f, 1.0f) * 0.2f;
    return c->base_price * escalation;
}

static void step_contracts(world_t *w, float dt) {
    /* Age existing contracts and check fulfillment */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!w->contracts[i].active) continue;
        /* Defensive sanity sweep. Clamps any contract whose base_price
         * went non-finite or absurd (seen in WORK rows as "+??? cr" /
         * INT_MAX payouts). Also guards quantity_needed so a bad spawn
         * can't produce an x2147483648 cargo display. */
        float bp = w->contracts[i].base_price;
        if (!isfinite(bp) || bp <= 0.0f || bp > 10000.0f) {
            SIM_LOG("[sim] contract %d had bad base_price %.1f -> clamped to 1\n", i, bp);
            w->contracts[i].base_price = 1.0f;
        }
        float qn = w->contracts[i].quantity_needed;
        if (!isfinite(qn) || qn <= 0.0f || qn > 10000.0f) {
            w->contracts[i].quantity_needed = 1.0f;
        }
        w->contracts[i].age += dt;

        switch (w->contracts[i].action) {
        case CONTRACT_TRACTOR: {
            if (w->contracts[i].station_index >= MAX_STATIONS) break;
            station_t *st = &w->stations[w->contracts[i].station_index];
            commodity_t c = w->contracts[i].commodity;

            /* Check if any scaffold module at this station still needs
             * this commodity — if so, close on scaffold progress, not
             * on the generic inventory threshold. */
            bool scaffold_needs = false;
            for (int m = 0; m < st->module_count; m++) {
                if (module_build_state(&st->modules[m]) != MODULE_BUILD_AWAITING_SUPPLY) continue;
                if (module_build_material(st->modules[m].type) != c) continue;
                scaffold_needs = true;
                break;
            }
            if (st->scaffold && c == COMMODITY_FRAME && st->scaffold_progress < 1.0f)
                scaffold_needs = true;

            if (scaffold_needs) {
                /* Close when ALL scaffolds needing this commodity are supplied */
                bool all_supplied = true;
                for (int m = 0; m < st->module_count; m++) {
                    if (!st->modules[m].scaffold) continue;
                    if (module_build_material(st->modules[m].type) != c) continue;
                    if (!module_is_fully_supplied(&st->modules[m])) { all_supplied = false; break; }
                }
                if (st->scaffold && c == COMMODITY_FRAME && st->scaffold_progress < 1.0f)
                    all_supplied = false;
                if (all_supplied) {
                    bool was_claimed = (w->contracts[i].claimed_by >= 0);
                    w->contracts[i].active = false;
                    if (was_claimed)
                        emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_TRACTOR});
                }
            } else {
                /* Non-construction: close once inventory is above the OPEN
                 * threshold. Hysteresis (open <90%, close >=95%) keeps a
                 * single station from oscillating, but production-chain
                 * stations consume ingots fast enough to re-cross 90%
                 * within seconds — so we only fire CONTRACT_COMPLETE when
                 * a player/NPC actually claimed and delivered. Otherwise
                 * the contract just retires silently. */
                float current = st->_inventory_cache[c];
                float threshold = (c < COMMODITY_RAW_ORE_COUNT) ? REFINERY_HOPPER_CAPACITY * 0.95f : MAX_PRODUCT_STOCK * 0.95f;
                if (current >= threshold) {
                    bool was_claimed = (w->contracts[i].claimed_by >= 0);
                    w->contracts[i].active = false;
                    if (was_claimed)
                        emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_TRACTOR});
                }
            }
            break;
        }
        case CONTRACT_FRACTURE: {
            /* Close when target asteroid is gone or index invalid */
            int idx = w->contracts[i].target_index;
            bool target_gone = (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active);
            if (target_gone) {
                w->contracts[i].active = false;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_FRACTURE});
            }
            /* Expire after 60 seconds if unfulfilled */
            if (w->contracts[i].active && w->contracts[i].age > 60.0f) w->contracts[i].active = false;
            break;
        }
        }
    }

    /* Generate up to TWO contracts per station: one ore, one non-ore.
     * Priority: scaffold modules > empty hoppers > empty ingot buffers.
     * Ore and production contracts can coexist at the same station. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;

        /* Check which contract types this station already has. The
         * kit-input slot tracks shipyard imports of frame / laser /
         * tractor commodities separately from the ingot / scaffold
         * production slot, so a shipyard that's already importing
         * ingots for its own fabs can ALSO be importing frames for
         * its kit fab. Without this split, Helios (which always has
         * some CU/CR ingot deficit) would never get a chance to ask
         * for frames — kit fab silently starved. */
        bool has_ore_contract = false;
        bool has_production_contract = false;
        bool has_kit_input_contract = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active || w->contracts[k].station_index != s) continue;
            commodity_t cc = w->contracts[k].commodity;
            if (cc < COMMODITY_RAW_ORE_COUNT) {
                has_ore_contract = true;
            } else if (cc == COMMODITY_FRAME ||
                       cc == COMMODITY_LASER_MODULE ||
                       cc == COMMODITY_TRACTOR_MODULE) {
                has_kit_input_contract = true;
            } else {
                has_production_contract = true;
            }
        }
        if (has_ore_contract && has_production_contract && has_kit_input_contract) continue;

        /* Sovereign station can run negative; pool is informational, so
         * contract generation is no longer gated on solvency. The
         * pool_factor below still scales offer pricing — that's a
         * deliberate (and untouched) pricing dynamic, not a refusal. */

        /* Pool factor: rich stations offer better prices.
         * 0.2x at 1000 cr, 1.0x at 5000 cr, 1.5x at 10000+ cr */
        float pool_factor = station_credit_pool(st) / 5000.0f;
        if (pool_factor < 0.2f) pool_factor = 0.2f;
        if (pool_factor > 1.5f) pool_factor = 1.5f;

        /* Evaluate station's top need */
        contract_t need = {0};
        need.target_index = -1;
        need.claimed_by = -1;

        /* Priority 1: scaffold modules need ingots (production slot) */
        if (!has_production_contract) {
            for (int m = 0; m < st->module_count; m++) {
                if (!st->modules[m].scaffold) continue;
                float cost = module_build_cost(st->modules[m].type);
                float remaining = cost * (1.0f - module_supply_fraction(&st->modules[m]));
                if (remaining > 0.5f) {
                    need = (contract_t){
                        .active = true, .action = CONTRACT_TRACTOR,
                        .station_index = (uint8_t)s,
                        .commodity = module_build_material(st->modules[m].type),
                        .quantity_needed = remaining,
                        .base_price = st->base_price[module_build_material(st->modules[m].type)] * 1.15f * pool_factor,
                        .target_index = -1, .claimed_by = -1,
                    };
                    break;
                }
            }
        }

        /* Priority 2: station scaffold needs frames (production slot) */
        if (!need.active && !has_production_contract && st->scaffold) {
            float remaining = SCAFFOLD_MATERIAL_NEEDED * (1.0f - st->scaffold_progress);
            if (remaining > 0.5f) {
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = COMMODITY_FRAME,
                    .quantity_needed = remaining,
                    .base_price = 23.0f * pool_factor,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 3: ore hopper with biggest deficit (ore slot).
         * Ore contracts are inventory-driven — fulfilled by fragment smelting, not cargo
         * delivery. quantity_needed is unused; contract closes when inventory > 80%. */
        if (!need.active && !has_ore_contract && station_has_module(st, MODULE_FURNACE)) {
            float worst_deficit = 0.0f;
            int worst_ore = -1;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                if (!sim_can_smelt_ore(st, (commodity_t)c)) continue;
                float deficit = REFINERY_HOPPER_CAPACITY * 0.5f - st->_inventory_cache[c];
                if (deficit > worst_deficit) { worst_deficit = deficit; worst_ore = c; }
            }
            if (worst_ore >= 0) {
                /* Demand multiplier: 1.0× when at target, up to 1.5× at
                 * total starvation. Layered on top of pool_factor so a
                 * starved-but-broke station still posts a sensible
                 * price. station_demand_for shares its severity
                 * definition with the priority-3 deficit calc above —
                 * they cannot disagree about who is starving. */
                float dmult = station_demand_for(st, (commodity_t)worst_ore).price_mult;
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = (commodity_t)worst_ore,
                    .quantity_needed = 0.0f, /* inventory-driven, not delivery-driven */
                    .base_price = st->base_price[worst_ore] * pool_factor * dmult,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 4: ingot buffer deficit (production slot). Runs
         * BEFORE the dock kit-fab fallback because a station's own
         * production chain (e.g. Kepler smelting ferrite ingots into
         * frames) is upstream of, and feeds, the kit-fab demand. If
         * Kepler stops asking for ferrite ingots its frame press dries
         * up, and then Helios's kit fab dries up too. */
        if (!need.active && !has_production_contract) {
            struct { commodity_t ingot; bool needed; } checks[] = {
                { COMMODITY_FERRITE_INGOT, station_has_module(st, MODULE_FRAME_PRESS) },
                { COMMODITY_CUPRITE_INGOT,
                  station_has_module(st, MODULE_LASER_FAB) ||
                  station_has_module(st, MODULE_TRACTOR_FAB) },
                { COMMODITY_CRYSTAL_INGOT, station_has_module(st, MODULE_LASER_FAB) },
            };
            float worst_deficit = 0.0f;
            int worst_idx = -1;
            for (int j = 0; j < 3; j++) {
                if (!checks[j].needed) continue;
                /* Don't import what we make ourselves. Helios has both
                 * FURNACE_CU and LASER_FAB, so the local furnace feeds
                 * the local fab — posting an import contract for the
                 * same ingot duplicates supply and shows up to players
                 * as "asking for what's already on the shelf". */
                if (station_produces(st, checks[j].ingot)) continue;
                /* Lifted threshold from 50% to 90%: at 50%, the chain
                 * stalled in steady-state because Prospect's FE shelf
                 * filled past cap before Kepler dropped low enough to
                 * trigger a contract. 90% keeps haulers moving while
                 * still gating contracts on actual demand. */
                float deficit = MAX_PRODUCT_STOCK * 0.9f - st->_inventory_cache[checks[j].ingot];
                if (deficit > worst_deficit) { worst_deficit = deficit; worst_idx = j; }
            }
            if (worst_idx >= 0) {
                float dmult = station_demand_for(st, checks[worst_idx].ingot).price_mult;
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = checks[worst_idx].ingot,
                    .quantity_needed = worst_deficit,
                    .base_price = st->base_price[checks[worst_idx].ingot] * 1.15f * pool_factor * dmult,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 5: shipyard kit-fab inputs. Lives in its own slot
         * so an ongoing ingot/scaffold contract doesn't starve it.
         * Helios always has some CU/CR ingot deficit; without this
         * split, the frame contract for kit-fab never gets posted. */
        contract_t kit_need = {0};
        kit_need.target_index = -1;
        kit_need.claimed_by = -1;
        if (!has_kit_input_contract && station_has_module(st, MODULE_SHIPYARD)) {
            const struct { commodity_t c; module_type_t producer; } kit_inputs[] = {
                { COMMODITY_FRAME,          MODULE_FRAME_PRESS  },
                { COMMODITY_LASER_MODULE,   MODULE_LASER_FAB    },
                { COMMODITY_TRACTOR_MODULE, MODULE_TRACTOR_FAB  },
            };
            float worst_deficit = 0.0f;
            int   worst_idx = -1;
            const float kit_input_target = 12.0f; /* keep ~3 batches' worth on hand */
            for (int j = 0; j < 3; j++) {
                if (station_has_module(st, kit_inputs[j].producer)) continue;
                float deficit = kit_input_target - st->_inventory_cache[kit_inputs[j].c];
                if (deficit > worst_deficit) { worst_deficit = deficit; worst_idx = j; }
            }
            if (worst_idx >= 0) {
                commodity_t mat = kit_inputs[worst_idx].c;
                float dmult = station_demand_for(st, mat).price_mult;
                kit_need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = mat,
                    .quantity_needed = worst_deficit,
                    .base_price = (st->base_price[mat] > 0.0f
                                  ? st->base_price[mat] * 1.25f * pool_factor
                                  : 28.0f * pool_factor) * dmult,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 6: kit imports at consumer-only stations. A station
         * with a dock but no shipyard (Prospect, future outposts) can't
         * mint kits — it needs them hauled in. Issue a TRACTOR contract
         * for REPAIR_KIT when the station's kit inventory falls below
         * 25% of cap. Players and NPC haulers can fulfill via the same
         * delivery loop that handles ingot/frame deliveries. */
        if (!need.active && !has_production_contract
            && station_has_module(st, MODULE_DOCK)
            && !station_has_module(st, MODULE_SHIPYARD)) {
            const float kit_import_threshold = REPAIR_KIT_STOCK_CAP * 0.25f;
            float kits_on_hand = (float)station_finished_count(st, COMMODITY_REPAIR_KIT);
            if (kits_on_hand < kit_import_threshold) {
                float deficit = REPAIR_KIT_STOCK_CAP - kits_on_hand;
                float seed = st->base_price[COMMODITY_REPAIR_KIT] > 0.0f
                             ? st->base_price[COMMODITY_REPAIR_KIT]
                             : 6.0f;
                float dmult = station_demand_for(st, COMMODITY_REPAIR_KIT).price_mult;
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = COMMODITY_REPAIR_KIT,
                    .quantity_needed = deficit,
                    .base_price = seed * 1.5f * pool_factor * dmult,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Post any contract we found a need for. `need` and `kit_need`
         * occupy separate slots, so a shipyard can simultaneously be
         * importing ingots for its fabs AND frames for its kit fab. */
        contract_t *to_post[2] = { NULL, NULL };
        int post_count = 0;
        if (need.active)     to_post[post_count++] = &need;
        if (kit_need.active) to_post[post_count++] = &kit_need;
        for (int p = 0; p < post_count; p++) {
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) {
                    w->contracts[k] = *to_post[p];
                    break;
                }
            }
        }
    }
}

/* ================================================================== */
/* Scaffolds: spawn + physics                                         */
/* ================================================================== */

static const float SCAFFOLD_RADIUS = 32.0f;
static const float SCAFFOLD_DRAG = 0.98f;  /* gentle drag when loose */

/* What commodity does a producer module output? Exposed (rather than
 * static) so tests can pin the mapping directly — driving it through
 * shipyard_intake_rate would need a full sim build-up for a 5-case
 * lookup. */
module_type_t producer_module_for_commodity(commodity_t c) {
    switch (c) {
        case COMMODITY_FRAME:         return MODULE_FRAME_PRESS;
        /* All ingot tiers come from MODULE_FURNACE; the count tier on
         * the station gates which ones can mint. */
        case COMMODITY_FERRITE_INGOT:
        case COMMODITY_CUPRITE_INGOT:
        case COMMODITY_CRYSTAL_INGOT: return MODULE_FURNACE;
        default:                      return MODULE_COUNT;
    }
}

/* Compute intake rate for a shipyard pulling a given commodity, based on
 * the station layout. Same-ring producers feed faster than cross-ring. */
static float shipyard_intake_rate(const station_t *st, int shipyard_idx, commodity_t mat) {
    module_type_t prod_type = producer_module_for_commodity(mat);
    if (prod_type == MODULE_COUNT) return 0.5f; /* unknown commodity, slow trickle */

    int yard_ring = st->modules[shipyard_idx].ring;
    int yard_slot = (int)st->modules[shipyard_idx].slot;
    float best_rate = 0.0f;
    for (int i = 0; i < st->module_count; i++) {
        if (i == shipyard_idx) continue;
        if (st->modules[i].scaffold) continue;
        if (st->modules[i].type != prod_type) continue;
        float rate;
        if (st->modules[i].ring == yard_ring) {
            /* Same ring: wrap-aware slot distance */
            int slots = STATION_RING_SLOTS[yard_ring];
            int d = abs((int)st->modules[i].slot - yard_slot);
            if (slots > 0 && d > slots / 2) d = slots - d;
            if (d < 1) d = 1;
            rate = 5.0f / (float)d;
        } else {
            /* Cross-ring: angular distance via base slot angles (rotation-independent) */
            float y_angle = TWO_PI_F * (float)yard_slot / (float)STATION_RING_SLOTS[yard_ring];
            float p_angle = TWO_PI_F * (float)st->modules[i].slot / (float)STATION_RING_SLOTS[st->modules[i].ring];
            float da = fabsf(y_angle - p_angle);
            if (da > PI_F) da = TWO_PI_F - da;
            float t = da / PI_F;
            rate = 3.0f - t * 2.5f;
        }
        if (rate > best_rate) best_rate = rate;
    }
    return best_rate > 0.0f ? best_rate : 0.5f;
}

/* Find an existing nascent scaffold being built at this station, if any. */
static int find_nascent_scaffold(const world_t *w, int station_idx) {
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!w->scaffolds[i].active) continue;
        if (w->scaffolds[i].state != SCAFFOLD_NASCENT) continue;
        if (w->scaffolds[i].built_at_station != station_idx) continue;
        return i;
    }
    return -1;
}

/* Is there a LOOSE scaffold still occupying the construction area near
 * this station's center? Used to gate spawning the next nascent. */
static bool construction_area_blocked(const world_t *w, int station_idx) {
    const station_t *st = &w->stations[station_idx];
    float clear_r = STATION_RING_RADIUS[1] * 0.6f; /* roughly inside ring 1 */
    float clear_r_sq = clear_r * clear_r;
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (!w->scaffolds[i].active) continue;
        if (w->scaffolds[i].state != SCAFFOLD_LOOSE) continue;
        if (v2_dist_sq(w->scaffolds[i].pos, st->pos) < clear_r_sq) return true;
    }
    return false;
}

/* Production layer v1: a nascent scaffold appears at the station center
 * when there's a pending order. Producer modules beam material to it.
 * The intake rate is layout-aware (same-ring fast, cross-ring slow).
 * When complete, the scaffold becomes LOOSE and can be towed away. */
/* module_flow_rate, module_accepts_input, step_module_flow → sim_production.c */

static void step_shipyard_manufacture(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        if (st->pending_scaffold_count == 0) continue;

        /* Find a SHIPYARD module on this station */
        int yard_idx = -1;
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].type == MODULE_SHIPYARD && !st->modules[i].scaffold) {
                yard_idx = i; break;
            }
        }
        if (yard_idx < 0) continue;

        /* Process the head of the queue */
        module_type_t type = st->pending_scaffolds[0].type;
        int8_t owner = st->pending_scaffolds[0].owner;
        commodity_t mat = module_build_material(type);
        float needed = module_build_cost(type);

        /* Make sure a nascent scaffold exists at the station center.
         * If a previously-completed scaffold is still in the construction
         * area, wait for it to drift clear before starting the next one. */
        int nidx = find_nascent_scaffold(w, s);
        if (nidx < 0) {
            if (construction_area_blocked(w, s)) continue;
            nidx = spawn_scaffold(w, type, st->pos, (int)owner);
            if (nidx < 0) continue; /* no slots */
            w->scaffolds[nidx].state = SCAFFOLD_NASCENT;
            w->scaffolds[nidx].built_at_station = s;
            w->scaffolds[nidx].build_amount = 0.0f;
            w->scaffolds[nidx].vel = v2(0.0f, 0.0f);
            w->scaffolds[nidx].pos = st->pos;
        }
        scaffold_t *nascent = &w->scaffolds[nidx];

        /* Pull material from station inventory into the nascent scaffold's
         * build pool at a layout-aware rate. */
        if (nascent->build_amount < needed) {
            float rate = shipyard_intake_rate(st, yard_idx, mat);
            float pull = rate * dt;
            if (pull > st->_inventory_cache[mat]) pull = st->_inventory_cache[mat];
            float room = needed - nascent->build_amount;
            if (pull > room) pull = room;
            if (pull > 0.0f) {
                st->_inventory_cache[mat] -= pull;
                nascent->build_amount += pull;
            }
        }

        /* Manufacture complete: nascent → loose, eject from the center */
        if (nascent->build_amount >= needed) {
            nascent->state = SCAFFOLD_LOOSE;
            nascent->built_at_station = -1;
            nascent->build_amount = 0.0f;
            /* Eject in a deterministic direction based on time so successive
             * builds spread around the station instead of stacking. Push
             * hard enough to clear the inner ring quickly. */
            float angle = w->time * 0.7f; /* slow rotation through directions */
            nascent->pos = v2_add(st->pos, v2(cosf(angle) * 12.0f, sinf(angle) * 12.0f));
            nascent->vel = v2(cosf(angle) * 90.0f, sinf(angle) * 90.0f);
            /* Shift queue */
            for (int i = 0; i < st->pending_scaffold_count - 1; i++) {
                st->pending_scaffolds[i] = st->pending_scaffolds[i + 1];
            }
            st->pending_scaffold_count--;
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_SCAFFOLD_READY,
                .scaffold_ready = { .station = s, .module_type = (int)type },
            });
            SIM_LOG("[sim] station %d manufactured %s scaffold\n", s, module_type_name(type));
        }
    }
}

int spawn_scaffold(world_t *w, module_type_t type, vec2 pos, int owner) {
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        if (w->scaffolds[i].active) continue;
        scaffold_t *sc = &w->scaffolds[i];
        memset(sc, 0, sizeof(*sc));
        sc->active = true;
        sc->module_type = type;
        sc->state = SCAFFOLD_LOOSE;
        sc->owner = owner;
        sc->pos = pos;
        sc->vel = v2(0.0f, 0.0f);
        sc->radius = SCAFFOLD_RADIUS;
        sc->rotation = 0.0f;
        sc->spin = 0.3f + (float)(w->rng % 100) * 0.004f;
        w->rng = w->rng * 1103515245u + 12345u;
        sc->age = 0.0f;
        sc->placed_station = -1;
        sc->placed_ring = -1;
        sc->placed_slot = -1;
        sc->towed_by = -1;
        sc->built_at_station = -1;
        sc->build_amount = 0.0f;
        return i;
    }
    return -1; /* no free slot */
}

/* Snap range: how close a LOOSE scaffold must be to a ring slot for the
 * station to reach out and grab it. */
#define SCAFFOLD_SNAP_RANGE 200.0f
/* How fast the station's tendrils pull a scaffold into position.
 * #define instead of static const float so MSVC accepts SCAFFOLD_SNAP_PULL
 * * 3.0f as a constant initializer for the static SCAFFOLD_SNAP tractor
 * beam below — clang/gcc treat const float as a constant expression but
 * MSVC does not. */
#define SCAFFOLD_SNAP_PULL  4.0f
/* Distance threshold to finalize placement. */
#define SCAFFOLD_SNAP_ARRIVE 8.0f

/* Find the open ring slot on a station that best matches a scaffold's
 * approach. The RING is chosen by the scaffold's distance from the station
 * center (closest ring radius wins). The SLOT is chosen by the scaffold's
 * angle around the station. This lets the player aim by flying to the
 * inner area for ring 1, outer area for ring 3, and aiming the angle. */
static bool find_nearest_open_slot(const station_t *st, vec2 pos, int *out_ring, int *out_slot) {
    vec2 delta = v2_sub(pos, st->pos);
    float dist = sqrtf(v2_len_sq(delta));
    if (dist > SCAFFOLD_SNAP_RANGE + STATION_RING_RADIUS[STATION_NUM_RINGS]) return false;

    /* Pick ring by distance match — closest STATION_RING_RADIUS wins */
    int best_ring = -1;
    float best_ring_diff = 1e18f;
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        if (ring > 1 && !ring_has_dock(st, ring - 1)) continue; /* dock gates next ring */
        /* Check if any slot on this ring is open */
        int slots = STATION_RING_SLOTS[ring];
        bool any_open = false;
        for (int slot = 0; slot < slots; slot++) {
            bool taken = false;
            for (int m = 0; m < st->module_count; m++)
                if (st->modules[m].ring == ring && st->modules[m].slot == slot) { taken = true; break; }
            if (!taken) { any_open = true; break; }
        }
        if (!any_open) continue;
        float ring_r = STATION_RING_RADIUS[ring];
        float diff = fabsf(dist - ring_r);
        if (diff < best_ring_diff) {
            best_ring_diff = diff;
            best_ring = ring;
        }
    }
    if (best_ring < 0) return false;

    /* Pick the open slot on that ring whose angle best matches the
     * scaffold's angle (slot angle includes ring rotation). */
    float scaffold_angle = atan2f(delta.y, delta.x);
    int best_slot = -1;
    float best_slot_diff = 1e18f;
    int slots = STATION_RING_SLOTS[best_ring];
    for (int slot = 0; slot < slots; slot++) {
        bool taken = false;
        for (int m = 0; m < st->module_count; m++)
            if (st->modules[m].ring == best_ring && st->modules[m].slot == slot) { taken = true; break; }
        if (taken) continue;
        float slot_angle = module_angle_ring(st, best_ring, slot);
        float diff = fabsf(wrap_angle(slot_angle - scaffold_angle));
        if (diff < best_slot_diff) {
            best_slot_diff = diff;
            best_slot = slot;
        }
    }
    if (best_slot < 0) return false;

    *out_ring = best_ring;
    *out_slot = best_slot;
    return true;
}

/* Convert a snapped scaffold into a station module.
 * The placed module enters a supply phase (build_progress 0→1) where
 * material must be delivered before the 10s construction timer starts. */
static void finalize_scaffold_placement(world_t *w, scaffold_t *sc) {
    station_t *st = &w->stations[sc->placed_station];
    if (st->module_count >= MAX_MODULES_PER_STATION) {
        sc->active = false;
        return;
    }
    station_module_t *m = &st->modules[st->module_count++];
    m->type = sc->module_type;
    m->ring = (uint8_t)sc->placed_ring;
    m->slot = (uint8_t)sc->placed_slot;
    m->scaffold = true;
    m->build_progress = 0.0f; /* enter post-placement supply phase */
    /* If this slot was planned, fulfill the plan (remove it). */
    for (int p = 0; p < st->placement_plan_count; p++) {
        if (st->placement_plans[p].ring == sc->placed_ring &&
            st->placement_plans[p].slot == sc->placed_slot) {
            for (int q = p; q < st->placement_plan_count - 1; q++)
                st->placement_plans[q] = st->placement_plans[q + 1];
            st->placement_plan_count--;
            break;
        }
    }
    /* Post a supply contract so NPCs can deliver the build material.
     * step_contracts() Priority 1 will also regenerate if this closes. */
    float cost = module_build_cost(sc->module_type);
    commodity_t material = module_build_material(sc->module_type);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active) {
            w->contracts[k] = (contract_t){
                .active = true, .action = CONTRACT_TRACTOR,
                .station_index = (uint8_t)sc->placed_station,
                .commodity = material,
                .quantity_needed = cost,
                .base_price = st->base_price[material] * 1.15f,
                .target_index = -1, .claimed_by = -1,
            };
            break;
        }
    }
    SIM_LOG("[sim] placed %s at station %d ring %d slot %d (needs %.0f %s)\n",
            module_type_name(sc->module_type), sc->placed_station,
            sc->placed_ring, sc->placed_slot, cost, commodity_name(material));
    sc->active = false;
}

static void step_scaffolds(world_t *w, float dt) {
    step_shipyard_manufacture(w, dt);
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        scaffold_t *sc = &w->scaffolds[i];
        if (!sc->active) continue;
        sc->age += dt;
        sc->rotation += sc->spin * dt;

        /* Nascent scaffolds: anchored at station center, no movement */
        if (sc->state == SCAFFOLD_NASCENT) {
            if (sc->built_at_station >= 0 && sc->built_at_station < MAX_STATIONS) {
                sc->pos = w->stations[sc->built_at_station].pos;
            }
            continue;
        }

        if (sc->state == SCAFFOLD_LOOSE) {
            /* Apply drag so loose scaffolds settle near where they spawned */
            sc->pos = v2_add(sc->pos, v2_scale(sc->vel, dt));
            sc->vel = v2_scale(sc->vel, SCAFFOLD_DRAG);

            /* Station vortex: loose scaffolds near active stations orbit */
            for (int s = 0; s < MAX_STATIONS; s++) {
                station_t *st = &w->stations[s];
                if (!station_is_active(st)) continue;
                vec2 delta = v2_sub(st->pos, sc->pos);
                float dist = sqrtf(v2_len_sq(delta));
                float vortex_range = st->dock_radius * 2.0f;
                if (dist < 10.0f || dist > vortex_range) continue;
                vec2 norm = v2_scale(delta, 1.0f / dist);
                /* Tangential orbit + gentle inward pull */
                vec2 tangent = v2(-norm.y, norm.x);
                float orbit_speed = 15.0f;
                float pull = 5.0f;
                sc->vel = v2_add(sc->vel, v2_scale(tangent, orbit_speed * dt));
                sc->vel = v2_add(sc->vel, v2_scale(norm, pull * dt));
            }

            /* Planned station tractor: blueprints pull matching scaffolds
             * straight toward center. No orbit — ghosts aren't rotating.
             * On arrival, materialize the ghost into a real station. */
            for (int s = 3; s < MAX_STATIONS; s++) {
                station_t *st = &w->stations[s];
                if (!st->planned) continue;
                bool type_matches = (sc->module_type == MODULE_SIGNAL_RELAY);
                if (!type_matches) {
                    for (int p = 0; p < st->placement_plan_count; p++)
                        if (st->placement_plans[p].type == sc->module_type) { type_matches = true; break; }
                }
                if (!type_matches) continue;
                vec2 delta = v2_sub(st->pos, sc->pos);
                float dist_sq = v2_len_sq(delta);
                const float PLAN_PULL_RANGE = 800.0f;
                if (dist_sq > PLAN_PULL_RANGE * PLAN_PULL_RANGE) continue;
                float dist = sqrtf(dist_sq);
                /* Constant-pull beam from blueprint center to scaffold.
                 * Legacy 25*(1 + 2*(1-d/range)) ranged from 25 (at
                 * d=range) to 75 (at d=0) with average ~50. Modeling
                 * as a fixed 50 with no falloff matches the average
                 * impulse the legacy delivered over a typical
                 * approach trajectory, and means scaffolds at the
                 * edge of pull range still get engaged at half the
                 * legacy peak (vs zero with linear falloff). */
                static const tractor_beam_t PLAN_BLUEPRINT = {
                    .rest_length     = 0.0f,
                    .pull_strength   = 0.0f,
                    .push_strength   = 0.0f,
                    .pull_constant   = 50.0f,
                    .push_constant   = 0.0f,
                    .range           = 800.0f,   /* PLAN_PULL_RANGE */
                    .axial_damping   = 3.0f,
                    /* Tangent damping matches axial (= legacy isotropic
                     * drag 3.0). Lowering breaks placement timing on
                     * test_outpost_*. Worth revisiting once playtest
                     * confirms the pull feels right. */
                    .tangent_damping = 3.0f,
                    .speed_cap       = 0.0f,
                    .falloff         = TRACTOR_FALLOFF_CONSTANT,
                };
                tractor_anchor_t plan_src = { .pos = st->pos, .vel = NULL,     .inv_mass = 0.0f };
                tractor_anchor_t plan_tgt = { .pos = sc->pos, .vel = &sc->vel, .inv_mass = 1.0f };
                (void)tractor_apply(&plan_src, &plan_tgt, &PLAN_BLUEPRINT, dt);
                /* Materialize on arrival */
                if (dist < 40.0f) {
                    st->planned = false;
                    st->scaffold = true;
                    st->scaffold_progress = 0.0f;
                    st->radius = OUTPOST_RADIUS;
                    st->dock_radius = OUTPOST_DOCK_RADIUS;
                    st->signal_range = OUTPOST_SIGNAL_RANGE;
                    add_module_at(st, MODULE_DOCK, 0, 0xFF);
                    add_module_at(st, MODULE_SIGNAL_RELAY, 0, 0xFF);
                    rebuild_station_services(st);
                    int chosen_ring = 1, chosen_slot = 0;
                    for (int p = 0; p < st->placement_plan_count; p++) {
                        if (st->placement_plans[p].type == sc->module_type) {
                            chosen_ring = st->placement_plans[p].ring;
                            chosen_slot = st->placement_plans[p].slot;
                            for (int q = p; q < st->placement_plan_count - 1; q++)
                                st->placement_plans[q] = st->placement_plans[q + 1];
                            st->placement_plan_count--;
                            break;
                        }
                    }
                    if (st->module_count < MAX_MODULES_PER_STATION) {
                        station_module_t *m = &st->modules[st->module_count++];
                        m->type = sc->module_type;
                        m->ring = (uint8_t)chosen_ring;
                        m->slot = (uint8_t)chosen_slot;
                        m->scaffold = true;
                        m->build_progress = 0.0f;
                    }
                    sc->active = false;
                    emit_event(w, (sim_event_t){
                        .type = SIM_EVENT_OUTPOST_PLACED,
                        .outpost_placed = { .slot = s },
                    });
                    break;
                }
            }
            if (!sc->active) continue; /* consumed by planned station above */

            /* Check if near an open ring slot on active outpost */
            for (int s = 3; s < MAX_STATIONS; s++) {
                station_t *st = &w->stations[s];
                if (!station_is_active(st)) continue;
                int ring, slot;
                if (find_nearest_open_slot(st, sc->pos, &ring, &slot)) {
                    sc->state = SCAFFOLD_SNAPPING;
                    sc->placed_station = s;
                    sc->placed_ring = ring;
                    sc->placed_slot = slot;
                    sc->vel = v2(0.0f, 0.0f);
                    break;
                }
            }
        }

        if (sc->state == SCAFFOLD_SNAPPING) {
            /* Station tendrils pull the scaffold toward its target slot.
             * The target rotates with the ring, so we chase it each frame. */
            station_t *st = &w->stations[sc->placed_station];
            vec2 target = module_world_pos_ring(st, sc->placed_ring, sc->placed_slot);
            vec2 delta = v2_sub(target, sc->pos);
            float dist = sqrtf(v2_len_sq(delta));

            if (dist < SCAFFOLD_SNAP_ARRIVE) {
                /* Close enough — lock into place and become a module */
                sc->pos = target;
                finalize_scaffold_placement(w, sc);
                continue; /* scaffold is now deactivated */
            }

            /* Spring pull toward the rotating target slot. Legacy used
             * K*d*(1+2*(1-d/range)) which integrates to ~7K total
             * impulse over the snap range. A constant-K spring with
             * pull_strength=12 (= 3*SCAFFOLD_SNAP_PULL) and no falloff
             * delivers slightly stronger total impulse than the legacy,
             * which keeps the snap fast enough to satisfy the existing
             * 5-sim-second test windows. */
            (void)dist;
            static const tractor_beam_t SCAFFOLD_SNAP = {
                .rest_length     = 0.0f,
                .pull_strength   = SCAFFOLD_SNAP_PULL * 3.0f,   /* K = 12 */
                .push_strength   = 0.0f,
                .pull_constant   = 0.0f,
                .push_constant   = 0.0f,
                /* No range gate — the SNAPPING state itself guarantees
                 * the scaffold is supposed to be converging. A range
                 * gate would disable damping past the limit and let an
                 * overshooting scaffold fly off into the void. */
                .range           = 0.0f,
                .axial_damping   = 5.0f,
                /* Tangent matches axial (= legacy isotropic drag 5.0).
                 * The target slot rotates with the ring; without strong
                 * tangent damping the scaffold orbits the rotating slot
                 * instead of converging on it. */
                .tangent_damping = 5.0f,
                .speed_cap       = 0.0f,
                .falloff         = TRACTOR_FALLOFF_CONSTANT,
            };
            tractor_anchor_t snap_src = { .pos = target,  .vel = NULL,     .inv_mass = 0.0f };
            tractor_anchor_t snap_tgt = { .pos = sc->pos, .vel = &sc->vel, .inv_mass = 1.0f };
            (void)tractor_apply(&snap_src, &snap_tgt, &SCAFFOLD_SNAP, dt);
            sc->pos = v2_add(sc->pos, v2_scale(sc->vel, dt));

            /* Safety: if station was destroyed or slot got taken, release back to LOOSE */
            if (!station_is_active(st)) {
                sc->state = SCAFFOLD_LOOSE;
                sc->placed_station = -1;
            }
        }

        /* SCAFFOLD_TOWING: position controlled by tow physics in step_player */
        /* SCAFFOLD_PLACED: static, owned by station module system */
    }
}

/* ================================================================== */
/* Public: world_sim_step                                             */
/* ================================================================== */

/* Signal-channel append. Returns the new message's id (0 on reject).
 * sender_station == -1 is allowed for system-origin posts (e.g. map
 * events). Text is trimmed to SIGNAL_CHANNEL_TEXT_MAX-1 chars;
 * audio_url must be empty or start with https: (enforced by the
 * REST handler, not this helper). */
/* Compute a block's entry_hash given its content and the prev block's hash.
 * Layout hashed: prev_hash(32) || id(8 LE) || ts(4 LE) || sender(2 LE) ||
 * text_len(1) || text(text_len). Stable across server restarts. */
static void signal_chain_hash_block(const uint8_t prev_hash[32],
                                    const signal_channel_msg_t *m,
                                    uint8_t out[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, prev_hash, 32);
    uint8_t header[15];
    for (int k = 0; k < 8; k++) header[k]      = (uint8_t)(m->id >> (8 * k));
    for (int k = 0; k < 4; k++) header[8 + k]  = (uint8_t)(m->timestamp_ms >> (8 * k));
    header[12] = (uint8_t)(m->sender_station & 0xFF);
    header[13] = (uint8_t)((uint16_t)m->sender_station >> 8);
    header[14] = m->text_len;
    sha256_update(&ctx, header, sizeof(header));
    sha256_update(&ctx, m->text, m->text_len);
    sha256_final(&ctx, out);
}

/* Append a sealed block to the per-station chain log on disk. The log
 * is the durable source of truth — the in-memory ring is just a cache.
 * Format: each record is a fixed-size signal_channel_msg_t blob (no
 * prev_hash field needed since prev_hash = previous record's entry_hash;
 * genesis is the all-zero hash). */
static void signal_chain_persist(int station, const signal_channel_msg_t *m) {
    char dir[]  = "chain";
    char path[64];
    snprintf(path, sizeof(path), "%s/%d.chain", dir, station);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    FILE *f = fopen(path, "ab");
    if (!f) return;
    fwrite(m, sizeof(*m), 1, f);
    fclose(f);
}

uint64_t signal_channel_post(world_t *w, int sender_station, const char *text, const char *audio_url) {
    if (!w || !text || text[0] == '\0') return 0;
    signal_channel_t *ch = &w->signal_channel;

    /* Prev hash = the most recent block's entry_hash, or genesis (zeroes). */
    uint8_t prev_hash[32] = {0};
    if (ch->count > 0) {
        int prev_slot = (ch->head - 1 + SIGNAL_CHANNEL_CAPACITY) % SIGNAL_CHANNEL_CAPACITY;
        memcpy(prev_hash, ch->msgs[prev_slot].entry_hash, 32);
    }

    int slot = ch->head;
    signal_channel_msg_t *m = &ch->msgs[slot];
    memset(m, 0, sizeof(*m));
    ch->next_id++;
    m->id = ch->next_id;
    m->timestamp_ms = (uint32_t)(w->time * 1000.0f);
    m->sender_station = (int16_t)sender_station;
    size_t tn = strlen(text);
    if (tn > SIGNAL_CHANNEL_TEXT_MAX - 1) tn = SIGNAL_CHANNEL_TEXT_MAX - 1;
    memcpy(m->text, text, tn);
    m->text[tn] = '\0';
    m->text_len = (uint8_t)tn;
    if (audio_url && audio_url[0]) {
        size_t an = strlen(audio_url);
        if (an > SIGNAL_CHANNEL_AUDIO_MAX - 1) an = SIGNAL_CHANNEL_AUDIO_MAX - 1;
        memcpy(m->audio_url, audio_url, an);
        m->audio_url[an] = '\0';
        m->audio_len = (uint8_t)(an > 255 ? 255 : an);
    }

    /* Seal the block: hash content + prev → entry_hash, then persist. */
    signal_chain_hash_block(prev_hash, m, m->entry_hash);
    signal_chain_persist(sender_station, m);

    ch->head = (ch->head + 1) % SIGNAL_CHANNEL_CAPACITY;
    if (ch->count < SIGNAL_CHANNEL_CAPACITY) ch->count++;
    return m->id;
}

/* Replay the on-disk chain on server boot. Reads the tail of each
 * station's chain file (last SIGNAL_CHANNEL_CAPACITY blocks) into the
 * world's ring buffer so the Network tab survives restarts. Bumps
 * ch->next_id past the highest block id seen. */
void signal_chain_load(world_t *w) {
    if (!w) return;
    signal_channel_t *ch = &w->signal_channel;
    /* Two-pass: first pass collects all blocks across all stations into
     * a sortable buffer; second sorts by id and inserts the latest
     * SIGNAL_CHANNEL_CAPACITY into the ring. The chain spans the whole
     * world (single feed across stations), so a single ordering matters. */
    enum { SCRATCH_CAP = 4096 };
    static signal_channel_msg_t scratch[SCRATCH_CAP];
    int collected = 0;
#ifndef _WIN32
    /* POSIX directory walk. Windows server is build-only (no production
     * deploy), so we no-op there to keep the cross-compile clean. */
    DIR *dir = opendir("chain");
    if (!dir) return;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && collected < SCRATCH_CAP) {
        const char *name = de->d_name;
        size_t n = strlen(name);
        if (n < 7 || strcmp(name + n - 6, ".chain") != 0) continue;
        /* dirent_t::d_name can be up to 255 bytes; precision-cap so
         * gcc -Werror=format-truncation is happy. "chain/" is 6 chars,
         * +null = 7, leaving 73 for the filename. */
        char path[80];
        snprintf(path, sizeof(path), "chain/%.73s", name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        while (collected < SCRATCH_CAP &&
               fread(&scratch[collected], sizeof(signal_channel_msg_t), 1, f) == 1) {
            collected++;
        }
        fclose(f);
    }
    closedir(dir);
#endif

    /* Sort by id (insertion sort — collected is small in practice). */
    for (int i = 1; i < collected; i++) {
        signal_channel_msg_t key = scratch[i];
        int j = i - 1;
        while (j >= 0 && scratch[j].id > key.id) {
            scratch[j + 1] = scratch[j]; j--;
        }
        scratch[j + 1] = key;
    }

    /* Take the most recent SIGNAL_CHANNEL_CAPACITY into the ring. */
    int start = (collected > SIGNAL_CHANNEL_CAPACITY)
        ? collected - SIGNAL_CHANNEL_CAPACITY : 0;
    ch->head = 0;
    ch->count = 0;
    for (int i = start; i < collected; i++) {
        ch->msgs[ch->head] = scratch[i];
        ch->head = (ch->head + 1) % SIGNAL_CHANNEL_CAPACITY;
        if (ch->count < SIGNAL_CHANNEL_CAPACITY) ch->count++;
        if (scratch[i].id > ch->next_id) ch->next_id = scratch[i].id;
    }
}

/* Iterate messages in post order (oldest first) via callback-free index
 * walk. Caller passes index 0..count-1 and receives a pointer. */
const signal_channel_msg_t *signal_channel_at(const world_t *w, int i) {
    const signal_channel_t *ch = &w->signal_channel;
    if (i < 0 || i >= ch->count) return NULL;
    int start = (ch->head - ch->count + SIGNAL_CHANNEL_CAPACITY) % SIGNAL_CHANNEL_CAPACITY;
    int slot = (start + i) % SIGNAL_CHANNEL_CAPACITY;
    return &ch->msgs[slot];
}

/* Spoke spring + drag dynamics constants. Tuned so the steady-state
 * phase lag at the drift-bias velocity is a visible 15-25° per spoke
 * group — enough to read as "the ring is being dragged" but well
 * shy of 90° where the spring would flip. Per-spoke stiffness sums
 * linearly: a station with many spokes between two rings tracks
 * tightly, a station with one is loose. */
#define RING_SPOKE_K        2.5f   /* spring constant per spoke (torque/rad) */
#define RING_DRAG_MU        0.6f   /* angular drag coefficient (torque per rad/s) */
#define RING_INERTIA_I      1.0f   /* moment of inertia per ring */
/* Drift bias: ambient torque applied per ring. arm_speed[r] * this
 * keeps a perfectly balanced station drifting (so an idle station
 * still rotates) and matches the legacy kinematic driver — at zero
 * spoke load, omega settles at arm_speed[r] * BIAS / DRAG_MU. With
 * BIAS = DRAG_MU, the steady-state omega equals arm_speed exactly,
 * so the legacy seed code (`arm_speed[1] = STATION_RING_SPEED`)
 * keeps Prospect's ring-2 spinning at the same rate as before. */
#define RING_DRIVE_BIAS_K   0.6f
/* Hard clamp on per-ring angular velocity. Prevents pathologically
 * asymmetric station layouts from driving a ring into a runaway
 * positive-feedback loop while still letting normal spoke balance
 * settle freely. ~4× the legacy STATION_RING_SPEED (0.04 rad/s). */
#define RING_OMEGA_MAX      0.16f
/* How long after a producer's last activity its tractor beam keeps
 * pulling (and rendering) at full strength. Pulse decays linearly
 * to 0 over this many seconds. */
#define RING_PULSE_LINGER_SEC 1.5f

/* Station jostle constants. Personal space = (a.dock_radius +
 * b.dock_radius) × FACTOR; below that, a spring force scaled by
 * overlap depth pushes them apart. Drag is high so transients die
 * out within ~1-2 seconds. K stays small so motion reads as "very
 * slowly settling" — well below STATION_RING_SPEED. */
#define STATION_PERSONAL_SPACE_FACTOR 1.5f
#define STATION_JOSTLE_K              4.0f   /* spring stiffness per unit overlap */
#define STATION_JOSTLE_DRAG           1.5f   /* per-second velocity decay */
#define STATION_JOSTLE_MAX_SPEED      8.0f   /* cap so things don't go ballistic */

void step_station_jostle(world_t *w, float dt) {
    /* Two passes:
     *   1. Sum pairwise repulsion impulses into each station's
     *      jostle_vel.
     *   2. Integrate jostle_vel onto pos with drag. */
    for (int a = 0; a < MAX_STATIONS; a++) {
        station_t *sa = &w->stations[a];
        if (!station_is_active(sa)) continue;
        if (sa->dock_radius <= 0.0f) continue;
        for (int b = a + 1; b < MAX_STATIONS; b++) {
            station_t *sb = &w->stations[b];
            if (!station_is_active(sb)) continue;
            if (sb->dock_radius <= 0.0f) continue;
            vec2 delta = v2_sub(sa->pos, sb->pos);
            float dist_sq = v2_len_sq(delta);
            float personal = (sa->dock_radius + sb->dock_radius) * STATION_PERSONAL_SPACE_FACTOR;
            if (dist_sq >= personal * personal) continue;
            float dist = sqrtf(dist_sq);
            float overlap = personal - dist;
            if (dist < 0.001f) {
                /* Coincident — pick an arbitrary direction so the
                 * pair doesn't sit at distance 0 forever. */
                delta = v2(1.0f, 0.0f);
                dist = 1.0f;
            }
            vec2 dir = v2_scale(delta, 1.0f / dist);
            float impulse = STATION_JOSTLE_K * overlap * dt;
            sa->jostle_vel = v2_add(sa->jostle_vel, v2_scale(dir, +impulse));
            sb->jostle_vel = v2_add(sb->jostle_vel, v2_scale(dir, -impulse));
        }
    }
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        /* Drag */
        float decay = 1.0f - STATION_JOSTLE_DRAG * dt;
        if (decay < 0.0f) decay = 0.0f;
        st->jostle_vel = v2_scale(st->jostle_vel, decay);
        /* Cap absolute speed */
        float speed_sq = v2_len_sq(st->jostle_vel);
        if (speed_sq > STATION_JOSTLE_MAX_SPEED * STATION_JOSTLE_MAX_SPEED) {
            float speed = sqrtf(speed_sq);
            st->jostle_vel = v2_scale(st->jostle_vel, STATION_JOSTLE_MAX_SPEED / speed);
        }
        /* Integrate onto pos */
        st->pos = v2_add(st->pos, v2_scale(st->jostle_vel, dt));
    }
}

/* Apply one spoke's spring torque to its two endpoint rings. Equal-and-
 * opposite (Newton's third) so the spoke conserves angular momentum
 * within the station. Same-ring spokes (rb == ra) net to zero and are
 * skipped — their torque contribution would cancel anyway, but the
 * skip also keeps the renderer/physics agreement clean. Out-of-bounds
 * rings or scaffolded hoppers are no-ops. */
static void apply_spoke_torque(const station_t *st,
                               const station_module_t *prod, float wa, int ra,
                               int hop, float pulse, float net_torque[]) {
    if (hop < 0) return;
    const station_module_t *hm = &st->modules[hop];
    if (hm->scaffold) return;
    int rb = (int)hm->ring;
    if (rb < 1 || rb > STATION_NUM_RINGS) return;
    if (rb == ra) return;
    int slots_b = STATION_RING_SLOTS[rb];
    if (slots_b <= 0) return;
    float alpha_b = TWO_PI_F * (float)hm->slot / (float)slots_b;
    float wb = st->arm_rotation[rb-1] + alpha_b;
    float dr = wb - wa;
    while (dr >  PI_F) dr -= TWO_PI_F;
    while (dr < -PI_F) dr += TWO_PI_F;
    float T = pulse * RING_SPOKE_K * sinf(dr);
    net_torque[ra-1] += T;
    net_torque[rb-1] -= T;
    (void)prod; /* reserved for future per-spoke scaling */
}

void step_station_ring_dynamics(world_t *w, float dt) {
    /* Decay all module activity pulses linearly. Production code
     * sets the pulse to 1.0 each tick a producer actually consumes
     * input; here we age every module's pulse, so when production
     * stalls (hopper empty, output full) the spoke goes slack. */
    float decay = dt / RING_PULSE_LINGER_SEC;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        for (int m = 0; m < st->module_count; m++) {
            float p = st->module_active_pulse[m] - decay;
            st->module_active_pulse[m] = (p < 0.0f) ? 0.0f : p;
        }
    }

    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;

        /* All-passive ring dynamics (Slice 1.5a). Every ring is a
         * passive ring receiving torque from its spokes; per-ring
         * arm_speed[r] becomes a "drift bias" so a perfectly balanced
         * station still rotates instead of locking up. There is no
         * kinematic driver — passive rings balance against each other
         * naturally.
         *
         * Spoke set: every active producer contributes one spoke per
         * declared input commodity AND one spoke for its output
         * commodity (when one exists — SHIPYARD is exempt; output is a
         * physical scaffold body). Each spoke applies equal-and-opposite
         * spring torque to its two endpoints; spokes whose endpoints
         * sit on the same ring net to zero, which is correct. */
        float net_torque[STATION_NUM_RINGS] = {0};

        for (int m = 0; m < st->module_count; m++) {
            const station_module_t *prod = &st->modules[m];
            if (prod->scaffold) continue;
            float pulse = st->module_active_pulse[m];
            if (pulse <= 0.0f) continue;

            int ra = (int)prod->ring;
            if (ra < 1 || ra > STATION_NUM_RINGS) continue;
            int slots_a = STATION_RING_SLOTS[ra];
            if (slots_a <= 0) continue;
            float alpha_a = TWO_PI_F * (float)prod->slot / (float)slots_a;
            float wa = st->arm_rotation[ra-1] + alpha_a;

            /* Input spokes — each declared input commodity. */
            module_inputs_t req = module_required_inputs(prod->type);
            for (int i = 0; i < req.count; i++) {
                int hop = station_find_hopper_for(st, req.commodities[i]);
                apply_spoke_torque(st, prod, wa, ra, hop, pulse, net_torque);
            }
            /* Output spoke (Slice 1 — cargo-in-space schema). SHIPYARD
             * has no commodity output and is naturally skipped: its
             * module_instance_output() returns COMMODITY_COUNT, and
             * station_find_output_hopper_for_module returns -1. */
            apply_spoke_torque(st, prod, wa, ra,
                               station_find_output_hopper_for_module(st, prod),
                               pulse, net_torque);
        }

        /* Per-ring integrate: drift bias + drag, semi-implicit Euler. */
        for (int idx = 0; idx < STATION_NUM_RINGS && idx < MAX_ARMS; idx++) {
            float bias = st->arm_speed[idx] * RING_DRIVE_BIAS_K;
            float damp = RING_DRAG_MU * st->arm_omega[idx];
            float tau  = net_torque[idx] + bias - damp;

            st->arm_omega[idx] += (tau / RING_INERTIA_I) * dt;
            if (st->arm_omega[idx] >  RING_OMEGA_MAX) st->arm_omega[idx] =  RING_OMEGA_MAX;
            if (st->arm_omega[idx] < -RING_OMEGA_MAX) st->arm_omega[idx] = -RING_OMEGA_MAX;
            st->arm_rotation[idx] += st->arm_omega[idx] * dt;
            /* No 2π wrap — sin/cos are periodic in the renderer, and
             * wrapping server-side caused visible "snap-back" artifacts
             * for clients interpolating across snapshots that landed on
             * opposite sides of the wrap boundary. arm_rotation grows
             * unbounded; f32 precision holds for years of session time
             * at typical drift rates (~0.04 rad/s). */
        }
    }
}

void world_sim_step(world_t *w, float dt) {
    w->events.count = 0;
    w->time += dt;
    step_station_ring_dynamics(w, dt);
    step_station_jostle(w, dt);
    sim_step_asteroid_dynamics(w, dt);
    maintain_asteroid_field(w, dt);
    /* Gravity + asteroid collisions at 30Hz (not 120Hz) — O(N²) is expensive */
    w->gravity_accumulator += dt;
    if (w->gravity_accumulator >= 1.0f / 30.0f) {
        float gdt = w->gravity_accumulator;
        w->gravity_accumulator = 0.0f;
        step_asteroid_gravity(w, gdt);
        resolve_asteroid_collisions(w);
        resolve_asteroid_station_collisions(w);
    }
    step_fracture_claims(w);
    step_furnace_smelting(w, dt);
    sim_step_refinery_production(w, dt);
    sim_step_station_production(w, dt);
    step_dock_repair_kit_fab(w, dt);
    step_module_flow(w, dt);

    /* Manifest-as-truth reconciliation: snap floor(inventory[c]) ==
     * manifest_count(c) for every finished commodity at every station.
     * Now bidirectional — production paths mint manifest in lockstep
     * with float increments, NPC unload + delivery + buy/sell all drain
     * both, so the only remaining sources of drift are legacy float-only
     * test fixtures (which the SELL/upgrade path can still consume). For
     * the LIVE simulation, manifest is the source of truth.
     *
     * The fractional residue under inventory[c] is preserved (production
     * accumulator state mid-cycle). Any drift over the integer-unit
     * boundary surfaces as a [drift] log line so future regressions are
     * caught immediately. */
    /* One-directional: only snap UP when manifest exceeds float (the
     * orphan-manifest case that was making BUY rows reject silently).
     * Don't snap DOWN — production/construction/upgrade tests still
     * depend on legacy float-only fixtures that have no manifest, and
     * snapping them to 0 breaks every chain that consumes from float
     * without first minting matching manifest. The two-directional
     * path is gated on cleaning those up site-by-site (#339 slice C). */
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
            int mc = manifest_count_by_commodity(&st->manifest, (commodity_t)c);
            if (mc <= 0) continue;
            int fc = (int)floorf(st->_inventory_cache[c] + 0.0001f);
            if (mc <= fc) continue;
            float frac = st->_inventory_cache[c] - (float)fc;
            if (frac < 0.0f) frac = 0.0f;
            st->_inventory_cache[c] = (float)mc + frac;
        }
    }
    step_module_activation(w, dt);
    step_scaffolds(w, dt);
    step_contracts(w, dt);
    step_npc_ships(w, dt);
    generate_npc_distress_contracts(w);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].connected) continue;
        step_player(w, &w->players[p], dt);
    }

    /* Player-player collision: ramming damage + signal interference */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!w->players[i].connected || w->players[i].docked) continue;
        for (int j = i + 1; j < MAX_PLAYERS; j++) {
            if (!w->players[j].connected || w->players[j].docked) continue;
            float ri = ship_hull_def(&w->players[i].ship)->ship_radius;
            float rj = ship_hull_def(&w->players[j].ship)->ship_radius;
            float minimum = ri + rj;
            vec2 delta = v2_sub(w->players[i].ship.pos, w->players[j].ship.pos);
            float d_sq = v2_len_sq(delta);
            if (d_sq >= minimum * minimum) continue;
            float d = sqrtf(d_sq);
            vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
            float overlap = minimum - d;
            w->players[i].ship.pos = v2_add(w->players[i].ship.pos, v2_scale(normal, overlap * 0.5f));
            w->players[j].ship.pos = v2_sub(w->players[j].ship.pos, v2_scale(normal, overlap * 0.5f));
            float rel_vel = v2_dot(v2_sub(w->players[i].ship.vel, w->players[j].ship.vel), normal);
            if (rel_vel < 0.0f) {
                float impact = -rel_vel;
                vec2 impulse = v2_scale(normal, rel_vel * 0.6f);
                w->players[i].ship.vel = v2_sub(w->players[i].ship.vel, impulse);
                w->players[j].ship.vel = v2_add(w->players[j].ship.vel, impulse);
                /* Ramming damage — both ships take damage based on impact speed.
                 * Threshold mult 0.7× makes deliberate rams sting at speeds
                 * that wouldn't bruise a static collision. */
                float dmg = collision_damage_for(impact, 0.7f);
                if (dmg > 0.0f) {
                    /* Each player's directional indicator points at
                     * the OTHER ship — the rammer they collided with. */
                    apply_ship_damage_attributed(w, &w->players[i], dmg,
                        w->players[j].session_token, DEATH_CAUSE_RAM,
                        w->players[j].ship.pos);
                    apply_ship_damage_attributed(w, &w->players[j], dmg,
                        w->players[i].session_token, DEATH_CAUSE_RAM,
                        w->players[i].ship.pos);
                }
            }
        }
    }

    /* NPC-NPC collision: same mass-symmetric resolution as player-player.
     * Without this, AI ships happily phase through each other — most
     * visibly when haulers stack on the same berth approach lane. Damage
     * is attributed both ways so a careless rammer eats hull too. */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *a = &w->npc_ships[i];
        if (!a->active || a->state == NPC_STATE_DOCKED) continue;
        const hull_def_t *adef = npc_hull_def(a);
        for (int j = i + 1; j < MAX_NPC_SHIPS; j++) {
            npc_ship_t *b = &w->npc_ships[j];
            if (!b->active || b->state == NPC_STATE_DOCKED) continue;
            const hull_def_t *bdef = npc_hull_def(b);
            float minimum = adef->ship_radius + bdef->ship_radius;
            vec2 delta = v2_sub(a->ship.pos, b->ship.pos);
            float d_sq = v2_len_sq(delta);
            if (d_sq >= minimum * minimum) continue;
            float d = sqrtf(d_sq);
            vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
            float overlap = minimum - d;
            a->ship.pos = v2_add(a->ship.pos, v2_scale(normal, overlap * 0.5f));
            b->ship.pos = v2_sub(b->ship.pos, v2_scale(normal, overlap * 0.5f));
            float rel_vel = v2_dot(v2_sub(a->ship.vel, b->ship.vel), normal);
            if (rel_vel < 0.0f) {
                float impact = -rel_vel;
                vec2 impulse = v2_scale(normal, rel_vel * 0.6f);
                a->ship.vel = v2_sub(a->ship.vel, impulse);
                b->ship.vel = v2_add(b->ship.vel, impulse);
                float dmg = collision_damage_for(impact, 0.7f);
                if (dmg > 0.0f) {
                    apply_npc_ship_damage_attributed(w, i, dmg,
                        b->session_token, DEATH_CAUSE_RAM);
                    apply_npc_ship_damage_attributed(w, j, dmg,
                        a->session_token, DEATH_CAUSE_RAM);
                }
            }
        }
    }

    /* Player-NPC collision: same shape as player-player. Players push
     * NPCs around at full force (mass-symmetric), and ramming a hauler
     * costs both sides hull. NPC physics writes land on npc_ship_t for
     * now; the end-of-tick mirror in step_npc_ships pushes them onto
     * the paired ship_t. */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        server_player_t *sp = &w->players[i];
        if (!sp->connected || sp->docked) continue;
        float pr = ship_hull_def(&sp->ship)->ship_radius;
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            npc_ship_t *npc = &w->npc_ships[n];
            if (!npc->active) continue;
            if (npc->state == NPC_STATE_DOCKED) continue;
            const hull_def_t *npcdef = npc_hull_def(npc);
            float nr = npcdef->ship_radius;
            float minimum = pr + nr;
            vec2 delta = v2_sub(sp->ship.pos, npc->ship.pos);
            float d_sq = v2_len_sq(delta);
            if (d_sq >= minimum * minimum) continue;
            float d = sqrtf(d_sq);
            vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
            float overlap = minimum - d;
            sp->ship.pos = v2_add(sp->ship.pos, v2_scale(normal, overlap * 0.5f));
            npc->ship.pos     = v2_sub(npc->ship.pos,    v2_scale(normal, overlap * 0.5f));
            float rel_vel = v2_dot(v2_sub(sp->ship.vel, npc->ship.vel), normal);
            if (rel_vel < 0.0f) {
                float impact = -rel_vel;
                vec2 impulse = v2_scale(normal, rel_vel * 0.6f);
                sp->ship.vel = v2_sub(sp->ship.vel, impulse);
                npc->ship.vel     = v2_add(npc->ship.vel,    impulse);
                float dmg = collision_damage_for(impact, 0.7f);
                if (dmg > 0.0f) {
                    apply_ship_damage_attributed(w, sp, dmg,
                        npc->session_token, DEATH_CAUSE_RAM,
                        npc->ship.pos);
                    apply_npc_ship_damage_attributed(w, n, dmg,
                        sp->session_token, DEATH_CAUSE_RAM);
                }
            }
        }
    }
}

/* ================================================================== */
/* Public: world_sim_step_player_only                                 */
/* ================================================================== */

void world_sim_step_player_only(world_t *w, int player_idx, float dt) {
    w->events.count = 0;
    /* Do NOT advance w->time — world time is server-authoritative (bug 46) */
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return;
    server_player_t *sp = &w->players[player_idx];
    if (!sp->connected) return;
    w->player_only_mode = true;  /* suppress mining HP and world RNG mutation */
    step_player(w, sp, dt);
    w->player_only_mode = false;
}

/* ================================================================== */
/* Public: world_reset                                                */
/* ================================================================== */

void world_cleanup(world_t *w) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        ship_cleanup(&w->players[i].ship);
    for (int i = 0; i < MAX_SHIPS; i++)
        ship_cleanup(&w->ships[i]);
    for (int i = 0; i < MAX_STATIONS; i++)
        station_cleanup(&w->stations[i]);
    free(w->signal_cache.strength);
    w->signal_cache.strength = NULL;
    w->signal_cache.valid = false;
    free(w->asteroid_grid.entries);
    w->asteroid_grid.entries = NULL;
}

void world_seed_station_manifests(world_t *w) {
    if (!w) return;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_exists(&w->stations[i])) continue;
        uint8_t origin[8] = { 'S','E','E','D','0','0','0','0' };
        origin[7] = (uint8_t)('0' + (i % 10));
        manifest_migrate_legacy_inventory(&w->stations[i].manifest,
                                          w->stations[i]._inventory_cache,
                                          COMMODITY_COUNT, origin);
        w->stations[i].manifest_dirty = true;
    }
}

/* Build and emit one CHAIN_EVT_OPERATOR_POST event of the given kind +
 * tier, with `text` as the payload body. The chain payload is a
 * fixed-prefix 38-byte header followed by the UTF-8 text bytes (no
 * NUL terminator). Caller passes `text_len` separately so empty texts
 * (or text already bounded) work. */
static void emit_operator_post(world_t *w, station_t *st,
                               uint8_t kind, uint8_t tier,
                               const char *text, int text_len) {
    if (text_len < 0) text_len = 0;
    if (text_len > 256) text_len = 256;
    chain_payload_operator_post_t hdr = {
        .kind = kind,
        .tier = tier,
        .ref_id = 0,
        .text_len = (uint16_t)text_len,
    };
    sha256_bytes((const uint8_t *)text, (size_t)text_len, hdr.text_sha256);
    uint8_t payload[38 + 256];
    memcpy(payload, &hdr, 38);
    /* memcpy(NULL, ..., 0) is UB per the C standard even with size 0.
     * The chain_log_emit doesn't read the body bytes when text_len == 0,
     * but we still memcpy for a populated payload prefix. Guard so a
     * paranoid caller passing NULL+0 doesn't trip pedantic sanitizers. */
    if (text_len > 0 && text != NULL) {
        memcpy(payload + 38, text, (size_t)text_len);
    }
    (void)chain_log_emit(w, st, CHAIN_EVT_OPERATOR_POST,
                         payload, (uint16_t)(38 + text_len));
}

/* Default per-station rarity-tier flavor text. Indexed by [station_idx][tier].
 * These are the *seed* values written into the chain log on world_reset; an
 * operator-push flow can later append `EVT_OPERATOR_POST(kind=RARITY_TIER, tier=N)`
 * events whose text supersedes these. Clients walking the chain log use the
 * latest event per (kind, tier) tuple as the canonical content.
 *
 * Tier 0 (common, 80–100% signal): generic hospitality.
 * Tier 1 (uncommon, 50–80%): mild personality / station chatter.
 * Tier 2 (rare, 20–50%): real station lore.
 * Tier 3 (ultra_rare, 0–20%): cryptic, in genre. Far from signal.
 *
 * Voice direction lives in src/station_voice.h; tone here matches the
 * three starter stations' established personalities. */
static const char *const DEFAULT_STATION_TIER_TEXT[3][4] = {
    /* Prospect Refinery (0) — pragmatic, tired, notices everything. */
    {
        "Prospect Refinery, foreman speaking. Furnaces hot, ore moving. Standard rates today.",
        "Night shift's been running 18% over throughput. Won't say why. We're not asking.",
        "Prospect was the first furnace in this arc. The original ferrite is still load-bearing.",
        "There's a hopper in the back we never open. The tag predates the station charter.",
    },
    /* Kepler Yard (1) — engineer, talks to machines, perks up for construction. */
    {
        "Kepler Yard, machinist on duty. Frames pressing on schedule. Drop your specs.",
        "Foreman Kepler used to tell apprentices: a frame is just slow ferrite.",
        "Our archive holds a frame stamped 'do not press.' The stamp predates the seal.",
        "You shouldn't be hearing this. Yard signal is gated to dock range. Step closer.",
    },
    /* Helios Works (2) — ambitious, enthusiastic, "we" meaning "I". */
    {
        "Helios Works. Prestige fabrication. Bring quality, take quality.",
        "The Director walked the smelting floor at dawn. She left a coin on the cold furnace.",
        "Helios was built atop another Helios. Three layers down, the foundation talks.",
        "We received a transmission. The signature was authentic. The author is dead.",
    },
};

/* Seed a station's chain log with the initial hail-message OPERATOR_POST
 * plus four rarity-tier events. Each tier event carries real flavor text
 * from DEFAULT_STATION_TIER_TEXT — the SHA-256 in the chain header binds
 * to actual content, so a verifier walking the log can prove a station
 * authored a specific tier message at a specific tick.
 *
 * Stations with index >= 3 (player outposts) emit the hail message but
 * skip the tier events: there's no authored content for outposts yet. */
static void seed_station_motd_chain_events(world_t *w, station_t *st,
                                           int station_idx) {
    emit_operator_post(w, st, 0 /* HAIL_MOTD */, 0,
                       st->hail_message, (int)strlen(st->hail_message));
    if (station_idx < 0 || station_idx >= 3) return;
    for (int tier_idx = 0; tier_idx < 4; tier_idx++) {
        const char *text = DEFAULT_STATION_TIER_TEXT[station_idx][tier_idx];
        emit_operator_post(w, st, 2 /* RARITY_TIER */,
                           (uint8_t)tier_idx,
                           text,
                           (int)strlen(text));
    }
}

void world_reset(world_t *w) {
    uint32_t seed = w->rng;  /* caller may pre-set seed; 0 = default */
    float *sig_buf = w->signal_cache.strength; /* preserve heap allocation */
    sparse_cell_entry_t *grid_entries = w->asteroid_grid.entries;
    for (int i = 0; i < MAX_PLAYERS; i++)
        ship_cleanup(&w->players[i].ship);
    for (int i = 0; i < MAX_SHIPS; i++)
        ship_cleanup(&w->ships[i]);
    for (int i = 0; i < MAX_STATIONS; i++)
        station_cleanup(&w->stations[i]);
    free(grid_entries);
    memset(w, 0, sizeof(*w));
    w->signal_cache.strength = sig_buf; /* restore — signal_grid_build reuses it */
    w->rng = seed ? seed : 2037u;
    w->belt_seed = w->rng;  /* anchor for rock_pub derivation (#285) */
    /* Wipe process-level nav scratch so a freshly-reset world doesn't
     * inherit stale path/nav-mesh state from a previously-run world.
     * Matters for test isolation when many world_t instances are reset
     * back-to-back in the same process. */
    nav_caches_reset();
    belt_field_init(&w->belt, w->rng, BELT_SCALE);
    for (int i = 0; i < MAX_STATIONS; i++)
        (void)station_manifest_bootstrap(&w->stations[i]);

    /* --- Seeded-station identity (Layer B of #479) ---
     * Derive deterministic Ed25519 keypairs for the three seeded
     * stations from the world seed *before* any other identity logic
     * runs, so subsequent code (catalog save, signal_chain bootstrap,
     * etc.) sees stations with stable pubkeys. */
    for (int s = 0; s < 3; s++)
        station_authority_init_seeded(&w->stations[s], w->belt_seed,
                                       (uint32_t)s);

    /* --- Chain log reset (Layer C of #479) ---
     * world_reset() blows away in-memory state. Match it on disk: the
     * seeded stations' chain log files (if any from a previous run)
     * must go too, otherwise the next emit's prev_hash (which we just
     * zeroed above via memset(w, 0, ...)) won't chain to the on-disk
     * tail and the verifier will reject. */
    for (int s = 0; s < 3; s++) {
        memset(w->stations[s].chain_last_hash, 0,
               sizeof(w->stations[s].chain_last_hash));
        w->stations[s].chain_event_count = 0;
        chain_log_reset(&w->stations[s]);
    }

    /* --- Stations ---
     *
     * Layout follows the cross-ring pair construction rule (see
     * shared/station_util.h::station_pair_neighbors): producers need
     * a HOPPER on an adjacent ring at the closest-canonical-angle
     * slot. A producer on ring N beams across the ring gap to its
     * paired hopper — the visual signature of every station.
     *
     * Layout principle: one HOPPER per producer, placed at the
     * cross-ring slot whose canonical angle is closest to the
     * producer's. Hoppers are NOT decorative — every hopper exists
     * because some producer paired with it. Starter stations bias
     * producers inward and use adjacent rings as readable staging belts.
     *   - Slot angles (zero rotation):
     *       ring 1 (3): 0°, 120°, 240°
     *       ring 2 (6): 0°, 60°, 120°, 180°, 240°, 300°
     *       ring 3 (9): 0°, 40°, 80°, 120°, 160°, 200°, 240°, 280°, 320°
     */
    w->next_station_id = 1; /* IDs start at 1; 0 = unassigned */
    w->stations[0].id = w->next_station_id++;
    snprintf(w->stations[0].name, sizeof(w->stations[0].name), "%s", "Prospect Refinery");
    w->stations[0].pos         = v2(0.0f, -2400.0f);
    w->stations[0].radius      = 40.0f;
    w->stations[0].dock_radius = 240.0f;
    /* Ore base prices are the smelt-payout floor when no TRACTOR contract
     * is active (sim_production.c smelt-payout reads station_buy_price for
     * the commodity). Never sold/bought as cargo — players don't carry
     * raw ore. */
    w->stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[0].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    /* Finished-good price baselines if Prospect receives stock; its dock
     * imports repair kits rather than shipyard kit-fab inputs. */
    w->stations[0].base_price[COMMODITY_FRAME]          = 22.0f;
    w->stations[0].base_price[COMMODITY_LASER_MODULE]   = 30.0f;
    w->stations[0].base_price[COMMODITY_TRACTOR_MODULE] = 38.0f;
    w->stations[0].signal_range = 18000.0f;
    /* Ring 1: dock + relay + ferrite furnace (tagged FERRITE_INGOT). */
    add_module_at(&w->stations[0], MODULE_DOCK,         1, 0);
    add_module_at(&w->stations[0], MODULE_SIGNAL_RELAY, 1, 1);
    add_furnace_for(&w->stations[0], 1, 2, COMMODITY_FERRITE_INGOT);
    /* Ring 2: ferrite-ore intake at slot 4 (240°, cross-ring opposite
     * the furnace at ring 1 slot 2). No output hopper — Prospect has
     * no downstream local consumer of ferrite ingots (no frame press
     * here), so smelt-completed ingots go straight to station
     * inventory and ride out via haulers to Kepler. */
    add_hopper_for(&w->stations[0], 2, 4, COMMODITY_FERRITE_ORE);
    w->stations[0].arm_count = 2;
    /* Drift bias on ring 2 — under the all-passive Slice 1.5a dynamics
     * this is the per-ring ambient torque. Ring 1 co-rotates via the
     * cross-ring spoke spring. Prospect has light spoke load (one
     * input + one output spoke) so ring 1 lags noticeably behind. */
    w->stations[0].arm_speed[1] = STATION_RING_SPEED;
    rebuild_station_services(&w->stations[0]);
    /* Stations are sovereign currency issuers. Net issuance is derived
     * from -Σ(ledger.balance) via station_credit_pool(); conservation
     * is structural. No initial pool seed — issuance starts at 0 and
     * floats freely as miners get paid and players spend back. */
    snprintf(w->stations[0].station_slug, sizeof(w->stations[0].station_slug), "prospect");
    snprintf(w->stations[0].currency_name, sizeof(w->stations[0].currency_name), "prospect vouchers");
    snprintf(w->stations[0].hail_message, sizeof(w->stations[0].hail_message),
             "Prospect Refinery. Ferrite smelting. Tow fragments to the furnace.");

    w->stations[1].id = w->next_station_id++;
    snprintf(w->stations[1].name, sizeof(w->stations[1].name), "%s", "Kepler Yard");
    w->stations[1].pos         = v2(-3200.0f, 2300.0f);
    w->stations[1].radius      = 36.0f;
    w->stations[1].dock_radius = 240.0f;
    w->stations[1].signal_range = 15000.0f;
    /* Smelt-payout floor (see Prospect comment above). */
    w->stations[1].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[1].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[1].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[1].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[1].base_price[COMMODITY_FRAME] = 20.0f;
    w->stations[1].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    /* Kepler imports laser/tractor modules for its shipyard kit fab. */
    w->stations[1].base_price[COMMODITY_LASER_MODULE]   = 30.0f;
    w->stations[1].base_price[COMMODITY_TRACTOR_MODULE] = 38.0f;
    /* Ring 1: dock + relay + shipyard. The shipyard sits on the inner
     * ring so its three input hoppers can read as a compact ring-2
     * staging belt instead of being buried on the outer hull. */
    add_module_at(&w->stations[1], MODULE_DOCK,         1, 0);
    add_module_at(&w->stations[1], MODULE_SIGNAL_RELAY, 1, 1);
    add_module_at(&w->stations[1], MODULE_SHIPYARD,     1, 2); /* needs FRAME, LASER, TRACTOR */
    /* Ring 2: frame press + shipyard input hoppers. Frame sits on the
     * shipyard's centerline; laser/tractor flank it. */
    add_module_at(&w->stations[1], MODULE_FRAME_PRESS,  2, 0); /* needs FERRITE_INGOT */
    add_hopper_for(&w->stations[1], 2, 3, COMMODITY_LASER_MODULE);   /* feeds SHIPYARD    */
    add_hopper_for(&w->stations[1], 2, 4, COMMODITY_FRAME);          /* frame output + shipyard input */
    add_hopper_for(&w->stations[1], 2, 5, COMMODITY_TRACTOR_MODULE); /* feeds SHIPYARD    */
    /* Ring 3: just the ferrite-ingot hopper feeding the frame press. */
    add_hopper_for(&w->stations[1], 3, 0, COMMODITY_FERRITE_INGOT);
    w->stations[1].arm_count = 3;
    w->stations[1].arm_speed[1] = STATION_RING_SPEED; /* ring 2 drift bias */
    rebuild_station_services(&w->stations[1]);
    snprintf(w->stations[1].station_slug, sizeof(w->stations[1].station_slug), "kepler");
    snprintf(w->stations[1].currency_name, sizeof(w->stations[1].currency_name), "kepler bonds");
    snprintf(w->stations[1].hail_message, sizeof(w->stations[1].hail_message),
             "Kepler Yard. Fabrication and scaffold kits. Build the frontier.");

    w->stations[2].id = w->next_station_id++;
    snprintf(w->stations[2].name, sizeof(w->stations[2].name), "%s", "Helios Works");
    w->stations[2].pos         = v2(3200.0f, 2300.0f);
    w->stations[2].radius      = 36.0f;
    w->stations[2].dock_radius = 240.0f;
    w->stations[2].signal_range = 15000.0f;
    /* Smelt-payout floor (see Prospect comment above). */
    w->stations[2].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[2].base_price[COMMODITY_LASER_MODULE] = 28.0f;
    w->stations[2].base_price[COMMODITY_TRACTOR_MODULE] = 36.0f;
    w->stations[2].base_price[COMMODITY_REPAIR_KIT] = 6.0f;
    /* Helios imports frames for its shipyard kit fab. */
    w->stations[2].base_price[COMMODITY_FRAME]          = 22.0f;
    /* No ferrite ingots produced or imported here — Helios runs at the
     * 3-furnace tier, which the new count rules deliberately gate
     * against ferrite. The ferrite-ingot pipeline stays Prospect's. */
    w->stations[2].base_price[COMMODITY_FERRITE_INGOT]  = 0.0f;
    /* Producers spread across all three rings; commodity-tagged
     * hoppers feed them all. */
    /* Ring 1: dock + relay + cuprite furnace. */
    add_module_at(&w->stations[2], MODULE_DOCK,         1, 0);
    add_module_at(&w->stations[2], MODULE_SIGNAL_RELAY, 1, 1);
    add_furnace_for(&w->stations[2], 1, 2, COMMODITY_CUPRITE_INGOT);
    /* Ring 2: fabs + paired ingot / ore hoppers + shipyard. Smelter beams
     * require the ore hopper on an adjacent ring, so cuprite/crystal ore
     * intakes live between the ring-1/ring-3 furnaces they feed. */
    add_module_at(&w->stations[2], MODULE_LASER_FAB,    2, 0);
    add_hopper_for(&w->stations[2], 2, 1, COMMODITY_CUPRITE_INGOT);
    add_module_at(&w->stations[2], MODULE_SHIPYARD,     2, 2); /* needs FRAME, LASER, TRACTOR */
    add_hopper_for(&w->stations[2], 2, 3, COMMODITY_CRYSTAL_ORE);
    add_hopper_for(&w->stations[2], 2, 4, COMMODITY_CUPRITE_ORE);
    add_module_at(&w->stations[2], MODULE_TRACTOR_FAB,  2, 5);
    /* Ring 3: 2 more furnaces (crystal + cuprite output) plus frame /
     * crystal-ingot / laser / tractor module hoppers for the ring-2 fabs
     * and shipyard. The ring-3 cuprite furnace shares the ring-2 cuprite
     * ore intake with the inner cuprite furnace. */
    add_hopper_for(&w->stations[2], 3, 2, COMMODITY_LASER_MODULE);   /* LASER_FAB output + shipyard input */
    add_hopper_for(&w->stations[2], 3, 3, COMMODITY_FRAME);          /* feeds SHIPYARD */
    add_furnace_for(&w->stations[2],   3, 4, COMMODITY_CRYSTAL_INGOT);
    add_hopper_for(&w->stations[2], 3, 5, COMMODITY_CRYSTAL_INGOT);
    add_furnace_for(&w->stations[2],   3, 6, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&w->stations[2], 3, 7, COMMODITY_TRACTOR_MODULE); /* TRACTOR_FAB output + shipyard input */
    w->stations[2].arm_count = 3;
    w->stations[2].arm_speed[1] = STATION_RING_SPEED; /* ring 2 drift bias */
    rebuild_station_services(&w->stations[2]);
    snprintf(w->stations[2].station_slug, sizeof(w->stations[2].station_slug), "helios");
    snprintf(w->stations[2].currency_name, sizeof(w->stations[2].currency_name), "helios credits");
    snprintf(w->stations[2].hail_message, sizeof(w->stations[2].hail_message),
             "Helios Works. Advanced smelting. Copper and crystal refined here.");
    w->station_count = 3; /* 3 starter stations */

    /* Seed each starter station's chain log with its initial hail
     * message + four authored rarity-tier events. See
     * seed_station_motd_chain_events for the per-event shape and
     * DEFAULT_STATION_TIER_TEXT for the tier flavor copy. */
    for (int s = 0; s < 3; s++)
        seed_station_motd_chain_events(w, &w->stations[s], s);

    rebuild_signal_chain(w);

    /* --- Initial asteroid field: materialize terrain chunks near stations --- */
    {
        int slot = 0;
        int budget = FIELD_ASTEROID_TARGET; /* leave headroom for fracture children */
        for (int s = 0; s < 3 && slot < budget; s++) {
            vec2 sp = w->stations[s].pos;
            int32_t scx, scy;
            chunk_coord(sp.x, sp.y, &scx, &scy);
            int r = 8; /* ~3200u radius in chunks */
            for (int dy = -r; dy <= r && slot < budget; dy++) {
                for (int dx = -r; dx <= r && slot < budget; dx++) {
                    int32_t cx = scx + dx;
                    int32_t cy = scy + dy;
                    chunk_asteroid_t rocks[CHUNK_MAX_ASTEROIDS];
                    int count = chunk_generate(&w->belt, w->rng, cx, cy,
                                                rocks, CHUNK_MAX_ASTEROIDS);
                    for (int ri = 0; ri < count && slot < budget; ri++) {
                        materialize_asteroid(w, slot, &rocks[ri], cx, cy, (uint16_t)ri);
                        slot++;
                    }
                }
            }
        }
    }

    /* --- NPC ships: seed haulers at every station so the inter-station
     * trade chain has a carrier on every hop. Without a Kepler-homed
     * hauler, frames pile up at Kepler with nobody to deliver them to
     * Helios; without a Helios-homed hauler, repair kits can't reach
     * Prospect's dock. The contract dispatcher in step_hauler picks
     * the best fillable contract from the home station's inventory,
     * so spawning the right home is the only seeding step needed.
     *
     * Miners also spread: Helios's CU/CR furnaces need their own
     * feed (Prospect-homed miners only deliver to Prospect's hopper).
     *
     * Tow drones stay at the two shipyards (Kepler, Helios). --- */
    spawn_npc(w, 0, NPC_ROLE_MINER);    /* Prospect: ferrite hopper feed */
    spawn_npc(w, 0, NPC_ROLE_MINER);
    spawn_npc(w, 2, NPC_ROLE_MINER);    /* Helios: CU/CR hopper feed */
    spawn_npc(w, 0, NPC_ROLE_HAULER);   /* Prospect -> Kepler ferrite ingots */
    spawn_npc(w, 0, NPC_ROLE_HAULER);
    spawn_npc(w, 1, NPC_ROLE_HAULER);   /* Kepler -> Helios frames */
    spawn_npc(w, 2, NPC_ROLE_HAULER);   /* Helios -> Prospect repair kits */
    spawn_npc(w, 1, NPC_ROLE_TOW);      /* Kepler shipyard */
    spawn_npc(w, 2, NPC_ROLE_TOW);      /* Helios shipyard */

    /* Bootstrap each station's per-ring angular velocity to its drift
     * bias. Under the all-passive Slice 1.5a dynamics, omega ramps to
     * (arm_speed * RING_DRIVE_BIAS_K / RING_DRAG_MU) over a ~1.7s time
     * constant. Pre-loading omega = arm_speed avoids a visible "spin
     * up" transient on world_reset and keeps the legacy steady-state
     * (omega == arm_speed when BIAS_K == DRAG_MU). */
    for (int s = 0; s < MAX_STATIONS; s++) {
        for (int r = 0; r < MAX_ARMS; r++) {
            w->stations[s].arm_omega[r] = w->stations[s].arm_speed[r];
        }
    }

    /* Precompute station nav meshes now that geometry is finalized. */
    station_rebuild_all_nav(w);

    SIM_LOG("[sim] world reset complete (%d asteroids, 7 NPCs)\n", FIELD_ASTEROID_TARGET);
}

/* ================================================================== */
/* Layer A.2 of #479 — pubkey registry                                */
/* ================================================================== */

static bool pubkey_is_zero(const uint8_t pk[32]) {
    for (int i = 0; i < 32; i++) if (pk[i]) return false;
    return true;
}

int registry_lookup_by_pubkey(const world_t *w, const uint8_t pubkey[32]) {
    if (!w || !pubkey || pubkey_is_zero(pubkey)) return -1;
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (!w->pubkey_registry[r].in_use) continue;
        if (memcmp(w->pubkey_registry[r].pubkey, pubkey, 32) != 0) continue;
        /* Find the player slot owning this session_token. */
        const uint8_t *tok = w->pubkey_registry[r].session_token;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!w->players[p].session_ready) continue;
            if (memcmp(w->players[p].session_token, tok, 8) == 0) return p;
        }
        /* Registry entry exists but no live player slot — return -1
         * (the binding will be reattached on the next REGISTER_PUBKEY). */
        return -1;
    }
    return -1;
}

bool registry_register_pubkey(world_t *w, const uint8_t pubkey[32],
                              const uint8_t session_token[8]) {
    if (!w || !pubkey || !session_token) return false;
    if (pubkey_is_zero(pubkey)) return false;
    /* Already registered? Update token (handles reconnect token rotation). */
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (!w->pubkey_registry[r].in_use) continue;
        if (memcmp(w->pubkey_registry[r].pubkey, pubkey, 32) != 0) continue;
        memcpy(w->pubkey_registry[r].session_token, session_token, 8);
        return true;
    }
    /* Fresh: take the first free slot. */
    for (int r = 0; r < MAX_PLAYERS; r++) {
        if (w->pubkey_registry[r].in_use) continue;
        memcpy(w->pubkey_registry[r].pubkey, pubkey, 32);
        memcpy(w->pubkey_registry[r].session_token, session_token, 8);
        w->pubkey_registry[r].in_use = true;
        return true;
    }
    return false; /* registry full */
}

/* ================================================================== */
/* Layer A.3 of #479 — signed-action verification                     */
/* ================================================================== */

static uint64_t read_u64_le_buf(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static uint16_t read_u16_le_buf(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

signed_action_result_t signed_action_verify(const world_t *w, int player_idx,
                                            const uint8_t *data, int len,
                                            uint8_t *out_action_type,
                                            uint64_t *out_nonce,
                                            const uint8_t **out_payload,
                                            uint16_t *out_payload_len) {
    if (!w || !data) return SIGNED_ACTION_REJECT_MALFORMED;
    if (player_idx < 0 || player_idx >= MAX_PLAYERS)
        return SIGNED_ACTION_REJECT_MALFORMED;
    /* Must include the type byte plus the 11-byte fixed header tail
     * (nonce + action_type + payload_len) plus a signature. */
    if (len < 1 + 11 + (int)SIGNED_ACTION_SIG_SIZE)
        return SIGNED_ACTION_REJECT_MALFORMED;
    if (data[0] != NET_MSG_SIGNED_ACTION)
        return SIGNED_ACTION_REJECT_MALFORMED;

    const server_player_t *sp = &w->players[player_idx];
    if (!sp->pubkey_set || pubkey_is_zero(sp->pubkey))
        return SIGNED_ACTION_REJECT_NO_PUBKEY;

    /* Layout: [type:1][nonce:8][action_type:1][payload_len:2][payload][sig:64] */
    uint64_t nonce       = read_u64_le_buf(&data[1]);
    uint8_t  action_type = data[9];
    uint16_t payload_len = read_u16_le_buf(&data[10]);
    if (payload_len > SIGNED_ACTION_MAX_PAYLOAD)
        return SIGNED_ACTION_REJECT_MALFORMED;
    int expected = 1 + 11 + (int)payload_len + (int)SIGNED_ACTION_SIG_SIZE;
    if (len != expected)
        return SIGNED_ACTION_REJECT_MALFORMED;
    if (action_type == 0 || action_type >= SIGNED_ACTION_COUNT)
        return SIGNED_ACTION_REJECT_UNKNOWN_TYPE;

    /* Reconstruct the signed message: nonce(8) || action_type(1) ||
     * payload_len(2) || payload. The signature covers exactly these
     * bytes; the leading message-type byte and trailing signature are
     * NOT signed. */
    const uint8_t *payload = &data[12];
    const uint8_t *sig     = &data[12 + payload_len];

    /* The signed prefix is contiguous in `data` (bytes [1..12+payload_len)),
     * so we don't need to memcpy into a scratch buffer. */
    if (!signal_crypto_verify(sig, &data[1], (size_t)(11 + payload_len),
                              sp->pubkey)) {
        return SIGNED_ACTION_REJECT_BAD_SIG;
    }

    /* Replay protection: nonce must be strictly greater than the
     * persisted high-water mark. last_signed_nonce==0 means "no
     * action accepted yet" — any non-zero nonce is fine. */
    if (nonce == 0 || nonce <= sp->last_signed_nonce)
        return SIGNED_ACTION_REJECT_REPLAY;

    if (out_action_type) *out_action_type = action_type;
    if (out_nonce)       *out_nonce       = nonce;
    if (out_payload)     *out_payload     = payload;
    if (out_payload_len) *out_payload_len = payload_len;
    return SIGNED_ACTION_OK;
}

/* ================================================================== */
/* Public: player_init_ship                                           */
/* ================================================================== */

void player_init_ship(server_player_t *sp, world_t *w) {
    ship_cleanup(&sp->ship);
    memset(&sp->ship, 0, sizeof(sp->ship));
    (void)ship_manifest_bootstrap(&sp->ship);
    sp->ship.hull_class = HULL_CLASS_MINER;
    sp->ship.hull       = hull_max_for_class(HULL_CLASS_MINER);
    sp->ship.angle      = PI_F * 0.5f;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
    sp->ship.towed_scaffold = -1;
    sp->ship.tractor_active = false;  /* driven by tractor_hold each frame */
    sp->ship.comm_range     = 1500.0f; /* H-ping reach — roughly one screen */
    sp->docked          = true;
    sp->current_station = 0;
    /* Seed credits are granted by player_seed_credits() AFTER session_token is set.
     * Calling ledger_earn here would use the wrong (zero) token on the server. */
    sp->nearby_station  = 0;
    sp->in_dock_range   = true;
    sp->hover_asteroid  = -1;
    /* Default to "deliver everything matching" — selective delivery
     * is opt-in via NET_ACTION_DELIVER_COMMODITY. */
    sp->input.service_sell_only = COMMODITY_COUNT;
    sp->input.service_sell_grade = MINING_GRADE_COUNT;
    sp->input.service_sell_one = false;
    sp->autopilot_mode = 0;
    sp->autopilot_state = 0;
    sp->autopilot_target = -1;
    sp->autopilot_timer = 0.0f;
    anchor_ship_in_station(sp, w);
}

/* Charge the spawn / docking fee at the player's current station.
 * Replaces the legacy "+50 starter grant" — ships now begin in the
 * red and have to earn their way out. Fee scales with station ring
 * count: 50 cr at a 1-ring outpost, 100 at a 2-ring station, 300 at
 * a full 3-ring hub. Skips if a ledger entry already exists for this
 * token (e.g. save reload, reconnect, post-death respawn) so a
 * player isn't charged twice for the same berth. */
void player_seed_credits(server_player_t *sp, world_t *w) {
    int st = sp->current_station;
    if (st < 0 || st >= MAX_STATIONS) st = 0;
    /* Already established a ledger here? Skip — debt and earnings
     * carry across reconnects and respawns.
     *
     * Identity-aware lookup: pubkey-registered players match the
     * full 32-byte pubkey entry (the same one their earnings credit
     * to). Legacy session-token players match the SHA256-of-token
     * pseudokey via the existing ledger_balance shim. The OLD code
     * here did `memcmp(ledger.player_pubkey, session_token, 8)` —
     * comparing the first 8 bytes of a 32-byte sha256 against the
     * raw session token, which never matches even for legacy
     * players, so the fee was charged on every reconnect. */
    if (sp->pubkey_set) {
        for (int i = 0; i < w->stations[st].ledger_count; i++) {
            if (memcmp(w->stations[st].ledger[i].player_pubkey,
                       sp->pubkey, 32) == 0) {
                return;
            }
        }
        int fee = station_spawn_fee(&w->stations[st]);
        ledger_force_debit_by_pubkey(&w->stations[st], sp->pubkey,
                                     (float)fee, &sp->ship);
        return;
    }
    /* Legacy: derive the pseudokey via the same helper ledger_balance
     * uses, then compare full 32 bytes. token_to_pseudo_pubkey copies
     * 8 bytes of token + 24 zero bytes — NOT a sha256. */
    uint8_t pseudo[32];
    token_to_pseudo_pubkey(sp->session_token, pseudo);
    for (int i = 0; i < w->stations[st].ledger_count; i++) {
        if (memcmp(w->stations[st].ledger[i].player_pubkey,
                   pseudo, 32) == 0) {
            return;
        }
    }
    int fee = station_spawn_fee(&w->stations[st]);
    ledger_force_debit(&w->stations[st], sp->session_token, (float)fee, &sp->ship);
}
