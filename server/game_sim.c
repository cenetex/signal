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
#include "cargo_legality.h"
#include "faction.h"
#include "manifest.h"
#include "contract_fit.h"
#include "station_policy.h"
#include "gossip.h"
#include "ship.h"
#include "sim_ai.h"
#include "sim_autopilot.h"
#include "signal_brain.h"
#include "sim_nav.h"
#include "sim_asteroid.h"
#include "sim_physics.h"
#include "sim_ship.h"
#include "sim_production.h"
#include "sim_construction.h"
#include "sim_mining.h"
#include "signal_model.h"
#include "cargo_receipt_issue.h"
#include "handoff_flow.h"
#include "mining.h"
#include "pubkey_proof.h"
#include "rng.h"
#include "sha256.h"   /* signal_chain_hash_block */
#include "signal_crypto.h" /* Ed25519 verify for signed actions (#479 A.3) */
#include "station_authority.h"
#include "net_protocol.h"

/* Imported from main.c — aws-swarm avatar keypair */
extern bool g_has_avatar_keypair;
extern uint8_t g_avatar_nacl_secret[64]; /* per-station Ed25519 identity (#479 B) */
#include "chain_log.h"         /* per-station signed event log (#479 C) */
#include "protocol.h"      /* NET_MSG_SIGNED_ACTION + signed_action_type_t */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#else
#include <dirent.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SIGNAL_MAYBE_UNUSED __attribute__((unused))
#else
#define SIGNAL_MAYBE_UNUSED
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

static float ledger_sanitize_float(float value) {
    if (!isfinite(value)) return 0.0f;
    if (value > LEDGER_FLOAT_LIMIT) return LEDGER_FLOAT_LIMIT;
    if (value < -LEDGER_FLOAT_LIMIT) return -LEDGER_FLOAT_LIMIT;
    return value;
}

static void ledger_add_stat_u32(uint32_t *field, float amount) {
    if (!field || !isfinite(amount) || amount <= 0.0f) return;
    float room = (float)(UINT32_MAX - *field);
    if (amount >= room) {
        *field = UINT32_MAX;
    } else {
        *field += (uint32_t)amount;
    }
}

void ledger_sanitize_station(station_t *st) {
    if (!st) return;
    if (st->ledger_count < 0) st->ledger_count = 0;
    if (st->ledger_count > STATION_LEDGER_MAX) st->ledger_count = STATION_LEDGER_MAX;
    for (int i = 0; i < st->ledger_count; i++) {
        st->ledger[i].balance = ledger_sanitize_float(st->ledger[i].balance);
        st->ledger[i].lifetime_supply = ledger_sanitize_float(st->ledger[i].lifetime_supply);
    }
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
    int count = st->ledger_count;
    if (count < 0) count = 0;
    if (count > STATION_LEDGER_MAX) count = STATION_LEDGER_MAX;
    for (int i = 0; i < count; i++) {
        total = ledger_sanitize_float(total + ledger_sanitize_float(st->ledger[i].balance));
    }
    return ledger_sanitize_float(-total);
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
    int count = st->ledger_count;
    if (count < 0) count = 0;
    if (count > STATION_LEDGER_MAX) count = STATION_LEDGER_MAX;
    for (int i = 0; i < count; i++)
        if (memcmp(st->ledger[i].player_pubkey, pubkey, 32) == 0)
            return ledger_sanitize_float(st->ledger[i].balance);
    return 0.0f;
}

void ledger_earn_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount) {
    if (!isfinite(amount) || amount <= 0.0f) return;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return;
    st->ledger[idx].balance = ledger_sanitize_float(st->ledger[idx].balance + amount);
    ledger_add_stat_u32(&st->ledger[idx].lifetime_credits_in, amount);
}

bool ledger_spend_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount, ship_t *ship) {
    if (!isfinite(amount)) return false;
    if (amount <= 0.0f) return true;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return false;
    float balance = ledger_sanitize_float(st->ledger[idx].balance);
    if (balance + 0.01f < amount) return false;
    st->ledger[idx].balance = ledger_sanitize_float(balance - amount);
    if (st->ledger[idx].balance < 0.0f) st->ledger[idx].balance = 0.0f;
    if (ship) ship->stat_credits_spent += amount;
    ledger_add_stat_u32(&st->ledger[idx].lifetime_credits_out, amount);
    return true;
}

void ledger_force_debit_by_pubkey(station_t *st, const uint8_t pubkey[32], float amount, ship_t *ship) {
    if (!isfinite(amount) || amount <= 0.0f) return;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return;
    st->ledger[idx].balance = ledger_sanitize_float(st->ledger[idx].balance - amount);
    if (ship) ship->stat_credits_spent += amount;
    ledger_add_stat_u32(&st->ledger[idx].lifetime_credits_out, amount);
}

/* Smelt-payout credit. Station keeps a 35% cut, supplier gets 65%.
 * Returns the actual amount credited so callers can emit accurate +N
 * UI events. Pre-Layer-A.1 anonymous players (zero pubkey) are not
 * credited; the supplier-cut amount stays on the station's pool. */
float ledger_credit_supply_amount_by_pubkey(station_t *st, const uint8_t pubkey[32], float ore_value) {
    if (!isfinite(ore_value) || ore_value <= 0.0f) return 0.0f;
    int idx = ledger_find_or_create_by_pubkey(st, pubkey);
    if (idx < 0) return 0.0f;
    /* Station keeps 35% cut for smelting — supplier gets 65% */
    float supplier_share = ore_value * 0.65f;
    if (!isfinite(supplier_share) || supplier_share < 0.01f) return 0.0f;
    /* Pool is derived from -Σ(balance); crediting the supplier here
     * automatically pushes the station's net issuance more negative. */
    st->ledger[idx].balance = ledger_sanitize_float(st->ledger[idx].balance + supplier_share);
    st->ledger[idx].lifetime_supply = ledger_sanitize_float(st->ledger[idx].lifetime_supply + ore_value);
    ledger_add_stat_u32(&st->ledger[idx].lifetime_credits_in, supplier_share);
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

static void sim_interactions_clear(world_t *w) {
    if (w) w->interactions.count = 0;
}

static void emit_interaction(world_t *w, sim_interaction_t interaction) {
    if (!w || w->interactions.count >= SIM_MAX_INTERACTIONS) return;
    w->interactions.items[w->interactions.count++] = interaction;
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
        .module_slots  = 3,
        .module_mask   = SHIP_MODULE_TRACTOR | SHIP_MODULE_LASER | SHIP_MODULE_CARGO,
    },
    [HULL_CLASS_HAULER] = {
        .name          = "Frame-2 Cargo Hauler",
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
        .module_slots  = 2,
        .module_mask   = SHIP_MODULE_TRACTOR | SHIP_MODULE_CARGO,
    },
    [HULL_CLASS_NPC_MINER] = {
        .name          = "Frame-2 Mining Workboat",
        .max_hull      = 100.0f,
        .accel         = 300.0f,
        .turn_speed    = 2.75f,
        .drag          = 0.45f,
        .cargo_capacity  = 24.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 28.0f,
        .tractor_range = 150.0f,
        .ship_radius   = 16.0f,
        .render_scale  = 0.7f,
        .module_slots  = 2,
        .module_mask   = SHIP_MODULE_TRACTOR | SHIP_MODULE_LASER,
    },
    [HULL_CLASS_DRONE_TRACTOR] = {
        .name          = "Frame-1 Tractor Drone",
        .max_hull      = 55.0f,
        .accel         = 340.0f,
        .turn_speed    = 3.1f,
        .drag          = 0.50f,
        .cargo_capacity  = 0.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 0.0f,
        .tractor_range = 145.0f,
        .ship_radius   = 11.0f,
        .render_scale  = 0.58f,
        .module_slots  = 1,
        .module_mask   = SHIP_MODULE_TRACTOR,
    },
    [HULL_CLASS_DRONE_LASER] = {
        .name          = "Frame-1 Laser Drone",
        .max_hull      = 55.0f,
        .accel         = 350.0f,
        .turn_speed    = 3.2f,
        .drag          = 0.48f,
        .cargo_capacity  = 0.0f,
        .ingot_capacity= 0.0f,
        .mining_rate   = 18.0f,
        .tractor_range = 0.0f,
        .ship_radius   = 11.0f,
        .render_scale  = 0.55f,
        .module_slots  = 1,
        .module_mask   = SHIP_MODULE_LASER,
    },
    [HULL_CLASS_DRONE_CARGO] = {
        .name          = "Frame-1 Cargo Drone",
        .max_hull      = 65.0f,
        .accel         = 260.0f,
        .turn_speed    = 2.5f,
        .drag          = 0.54f,
        .cargo_capacity  = 0.0f,
        .ingot_capacity= 12.0f,
        .mining_rate   = 0.0f,
        .tractor_range = 0.0f,
        .ship_radius   = 12.0f,
        .render_scale  = 0.62f,
        .module_slots  = 1,
        .module_mask   = SHIP_MODULE_CARGO,
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

static bool spatial_grid_grow(spatial_grid_t *g) {
    if (!g || !g->entries || g->capacity == 0) return false;
    if (g->capacity > UINT32_MAX / 2u) return false;

    uint32_t old_capacity = g->capacity;
    sparse_cell_entry_t *old_entries = g->entries;
    uint32_t new_capacity = old_capacity * 2u;
    sparse_cell_entry_t *new_entries =
        (sparse_cell_entry_t *)calloc(new_capacity, sizeof(sparse_cell_entry_t));
    if (!new_entries) return false;

    for (uint32_t i = 0; i < new_capacity; i++)
        new_entries[i].key_x = INT32_MIN;

    g->entries = new_entries;
    g->capacity = new_capacity;
    g->mask = new_capacity - 1u;
    g->occupied = 0;

    for (uint32_t i = 0; i < old_capacity; i++) {
        const sparse_cell_entry_t *old = &old_entries[i];
        if (old->key_x == INT32_MIN) continue;
        uint32_t h = ((uint32_t)old->key_x * 73856093u) ^
                     ((uint32_t)old->key_y * 19349663u);
        for (uint32_t probes = 0, slot = h & g->mask; probes < g->capacity;
             probes++, slot = (slot + 1u) & g->mask) {
            sparse_cell_entry_t *dst = &g->entries[slot];
            if (dst->key_x != INT32_MIN) continue;
            *dst = *old;
            g->occupied++;
            break;
        }
    }

    free(old_entries);
    return true;
}

static spatial_cell_t *spatial_grid_get_or_create(spatial_grid_t *g, int cx, int cy) {
    spatial_grid_ensure(g);
    if (!g->entries) return NULL; /* OOM — degrade gracefully */
    if ((g->occupied + 1u) * 4u >= g->capacity * 3u) {
        (void)spatial_grid_grow(g);
    }
    /* Mul in unsigned space — signed * 73856093 overflows for |cx| > 29 (UB). */
    uint32_t h = ((uint32_t)cx * 73856093u) ^ ((uint32_t)cy * 19349663u);
    for (uint32_t probes = 0, i = h & g->mask; probes < g->capacity;
         probes++, i = (i + 1) & g->mask) {
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
    return NULL;
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
 * Root stations (indices 0-2, the relay-root ones) are always connected.
 * An outpost is connected if its signal_range overlaps a connected station.
 */
void rebuild_signal_chain(world_t *w) {
    /* Reset all */
    for (int s = 0; s < MAX_STATIONS; s++)
        w->stations[s].signal_connected = false;

    /* Root stations are always connected if active. Freeport is seeded,
     * but has no relay and therefore is not a signal-chain root. */
    for (int s = 0; s < SIGNAL_ROOT_STATION_COUNT && s < MAX_STATIONS; s++) {
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
        float dist = v2_len(v2_sub(pos, w->stations[s].pos));
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
        float dist = v2_len(v2_sub(pos, w->stations[s].pos));
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

static cargo_legality_result_t classify_ship_manifest_unit_at_station(
    const world_t *w, const ship_t *ship, uint16_t index, int station_index);

static bool cargo_unit_pub_nonzero(const cargo_unit_t *unit) {
    static const uint8_t zero[32] = {0};
    return unit && memcmp(unit->pub, zero, sizeof(zero)) != 0;
}

static void emit_station_construction_contribution(world_t *w, station_t *st,
                                                   int station_idx,
                                                   const cargo_unit_t *unit,
                                                   float progress_after) {
    if (!w || !st || !cargo_unit_pub_nonzero(unit)) return;
    chain_payload_construction_t payload = {0};
    memcpy(payload.cargo_pub, unit->pub, sizeof(payload.cargo_pub));
    payload.target_kind = CONSTRUCTION_TARGET_STATION;
    payload.station_index = (station_idx >= 0 && station_idx <= 255)
        ? (uint8_t)station_idx : 0xff;
    payload.module_index = 0xff;
    payload.module_type = 0xff;
    payload.commodity = COMMODITY_FRAME;
    payload.target_id = (station_idx >= 0) ? (uint64_t)station_idx : 0u;
    payload.contributed_units = 1.0f;
    payload.progress_after = progress_after;
    (void)chain_log_emit(w, st, CHAIN_EVT_CONSTRUCTION,
                         &payload, sizeof(payload));
}

/* add_module_at, activate_outpost, begin_module_construction*,
 * step_module_delivery, step_module_activation → sim_construction.c
 * module_build_material, module_build_cost, station_sells_scaffold
 *   → sim_construction.c / sim_construction.h */

static void step_scaffold_delivery(world_t *w, server_player_t *sp) {
    if (!sp->docked) return;
    station_t *st = &w->stations[sp->current_station];
    if (!st->scaffold) return;
    int held = ship_finished_count(&sp->ship, COMMODITY_FRAME) +
               ship_towed_pods_manifest_count(w, &sp->ship, COMMODITY_FRAME);
    if (held <= 0) return;
    float needed_f = SCAFFOLD_MATERIAL_NEEDED * (1.0f - st->scaffold_progress);
    int needed = (int)ceilf(needed_f - 0.0001f);
    if (needed <= 0) return;
    int request = held < needed ? held : needed;
    int accepted = 0;
    while (accepted < request) {
        int idx = -1;
        for (uint16_t i = 0; i < sp->ship.manifest.count; i++) {
            const cargo_unit_t *unit = &sp->ship.manifest.units[i];
            if (!unit || unit->commodity != (uint8_t)COMMODITY_FRAME)
                continue;
            cargo_legality_result_t legality =
                classify_ship_manifest_unit_at_station(
                    w, &sp->ship, i, sp->current_station);
            if (legality.status == CARGO_LEGALITY_CONTRABAND)
                continue;
            idx = (int)i;
            break;
        }
        if (idx < 0) break;
        cargo_unit_t unit = {0};
        if (!ship_manifest_remove_with_chain(&sp->ship, (uint16_t)idx,
                                             &unit, NULL)) {
            break;
        }
        float progress_after = st->scaffold_progress +
            (float)(accepted + 1) / SCAFFOLD_MATERIAL_NEEDED;
        if (progress_after > 1.0f) progress_after = 1.0f;
        emit_station_construction_contribution(w, st, sp->current_station,
                                               &unit, progress_after);
        accepted++;
    }
    while (accepted < request) {
        cargo_unit_t unit = {0};
        if (!ship_towed_pods_take_manifest_unit(w, &sp->ship,
                                                COMMODITY_FRAME, &unit)) {
            break;
        }
        float progress_after = st->scaffold_progress +
            (float)(accepted + 1) / SCAFFOLD_MATERIAL_NEEDED;
        if (progress_after > 1.0f) progress_after = 1.0f;
        emit_station_construction_contribution(w, st, sp->current_station,
                                               &unit, progress_after);
        accepted++;
    }
    if (accepted > 0)
        ship_finished_sync(&sp->ship, COMMODITY_FRAME);
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
static vec2 station_module_cargo_mouth(const station_t *st,
                                       const station_module_t *module,
                                       const cargo_pod_t *pod);
static vec2 station_module_cargo_hold_anchor(const world_t *w,
                                             const station_t *st,
                                             int station_idx,
                                             int module_idx,
                                             const cargo_pod_t *pod,
                                             vec2 base_anchor);

/* Asteroid lifecycle, dynamics, fracture → sim_asteroid.c
 * sim_can_smelt_ore, sim_step_refinery_production, sim_step_station_production,
 * step_furnace_smelting, step_module_flow, step_module_delivery
 *   → sim_production.c */

/* Approach target: aim for the inner dock gap lane nearest `from`, not
 * the dock module body. This is the last station-local waypoint before
 * an NPC is considered docked. Outside ships should route through
 * station_entry_target() first so they cross the outer rings through the
 * station roadway instead of cutting straight at an inner dock. */
vec2 station_approach_target(const station_t *st, vec2 from) {
    float best_d = 1e18f;
    vec2 best_pos = st->pos;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_DOCK) continue;
        if (st->modules[i].scaffold) continue;
        int ring = st->modules[i].ring;
        int slot = st->modules[i].slot;
        float r = STATION_RING_RADIUS[ring] + 80.0f;
        vec2 mp = station_dock_lane_pos(st, ring, slot, r);
        float d = v2_dist_sq(from, mp);
        if (d < best_d) { best_d = d; best_pos = mp; }
    }
    return best_pos;
}

/* Entry target: the outside mouth of the outermost occupied ring's open
 * roadway. This is the first waypoint for ships entering a station whose
 * dock lives on an inner ring, such as Kepler. */
vec2 station_entry_target(const station_t *st, vec2 from) {
    (void)from;
    int outer_ring = station_max_ring(st);
    int road_ring = outer_ring;
    while (road_ring > 1 && ring_module_count(st, road_ring) <= 1)
        road_ring--;
    if (road_ring >= 1 && road_ring <= STATION_NUM_RINGS) {
        float r = STATION_RING_RADIUS[outer_ring] + 160.0f;
        if (station_ring_open_gap_lane(st, road_ring, NULL, NULL))
            return station_ring_open_gap_lane_pos(st, road_ring, r);
    }

    /* No ring roadway: use a stable fallback outside the docking halo. */
    float r = st->dock_radius + 160.0f;
    if (r < st->radius + 160.0f) r = st->radius + 160.0f;
    return v2_add(st->pos, v2(r, 0.0f));
}

/* Exit target: clear the inner dock lane if needed, then route to the
 * same outer roadway mouth used for entry. */
vec2 station_exit_target(const station_t *st, vec2 from) {
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
        return station_entry_target(st, from);
    }
    int ring = st->modules[best_i].ring;
    int slot = st->modules[best_i].slot;
    float inner_r = STATION_RING_RADIUS[ring] - 60.0f;
    if (inner_r < st->radius + 80.0f) inner_r = st->radius + 80.0f;
    vec2 inner_lane = station_dock_lane_pos(st, ring, slot, inner_r);
    float from_r = v2_len(v2_sub(from, st->pos));
    if (from_r < STATION_RING_RADIUS[ring] - 20.0f &&
        v2_dist_sq(from, inner_lane) > 80.0f * 80.0f) {
        return inner_lane;
    }
    return station_entry_target(st, from);
}

/* ================================================================== */
/* Player ship helpers                                                */
/* ================================================================== */

/* ship_forward, ship_muzzle: see ship.h/c */

/* try_spend_credits removed — all spending goes through ledger_spend */

void anchor_ship_in_station(server_player_t *sp, world_t *w) {
    if (!sp) return;
    if (!w || sp->current_station < 0 || sp->current_station >= MAX_STATIONS ||
        !station_exists(&w->stations[sp->current_station])) {
        sp->dock_berth = 0;
        sp->ship.vel = v2(0.0f, 0.0f);
        return;
    }
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

static float player_station_balance_at(const world_t *w,
                                       const server_player_t *sp,
                                       int station_idx) {
    if (!w || !sp || station_idx < 0 || station_idx >= MAX_STATIONS)
        return 0.0f;
    const station_t *st = &w->stations[station_idx];
    if (server_player_can_use_pubkey_persistence(sp))
        return ledger_balance_by_pubkey(st, sp->pubkey);
    return ledger_balance(st, sp->session_token);
}

static void emit_dock_balance_response(world_t *w,
                                       server_player_t *sp,
                                       int station_idx) {
    if (!w || !sp || station_idx < 0 || station_idx >= MAX_STATIONS)
        return;
    if (!station_is_active(&w->stations[station_idx])) return;
    float balance = player_station_balance_at(w, sp, station_idx);
    if (balance <= 0.5f) return;
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_HAIL_RESPONSE,
        .player_id = sp->id,
        .hail_response = {
            .station = station_idx,
            .credits = balance,
            .contract_index = -1,
        },
    });
}

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
    if (sp->current_station >= 0 && server_player_can_use_pubkey_persistence(sp)) {
        ledger_record_dock(&w->stations[sp->current_station], sp->pubkey,
                            (uint64_t)w->time);
    }
    /* Gossip-contract dock handshake: bidirectional set-union with the
     * station's known pool. Player ships are couriers in the gossip
     * protocol exactly like NPC haulers. The contract menu the player
     * sees post-dock is sourced from the player's own known_contracts. */
    if (sp->current_station >= 0) {
        gossip_dock_handshake(w, sp->current_station,
                              sp->ship.known_contracts,
                              &sp->ship.known_contract_count,
                              SHIP_KNOWN_CONTRACT_CAP,
                              &sp->ship.knowledge);
        emit_dock_balance_response(w, sp, sp->current_station);
    }
    emit_event(w, (sim_event_t){.type = SIM_EVENT_DOCK, .player_id = sp->id});
}

/* ================================================================== */
/* Contract-origin ship assets                                        */
/* ================================================================== */

static bool ship_asset_session_nonzero(const uint8_t token[8]) {
    if (!token) return false;
    for (int i = 0; i < 8; i++) if (token[i]) return true;
    return false;
}

static bool ship_asset_pubkey_nonzero(const uint8_t pubkey[32]) {
    if (!pubkey) return false;
    for (int i = 0; i < 32; i++) if (pubkey[i]) return true;
    return false;
}

static int player_slot_for_ptr(const world_t *w, const server_player_t *sp) {
    if (!w || !sp) return -1;
    if (sp < &w->players[0] || sp >= &w->players[MAX_PLAYERS]) return -1;
    return (int)(sp - &w->players[0]);
}

static int shipyard_station_request_owner_code(int station_idx) {
    if (station_idx < 0) station_idx = 0;
    if (station_idx == 0) return INT8_MIN;
    if (station_idx >= 127) return -1; /* int8 pending owner cannot encode 127+ */
    return -1 - station_idx;
}

static bool shipyard_owner_code_is_station_request(int owner_code) {
    return owner_code == INT8_MIN || owner_code <= -2;
}

static int shipyard_owner_code_station(int owner_code) {
    if (owner_code == INT8_MIN) return 0;
    if (owner_code <= -2) return -1 - owner_code;
    return -1;
}

static void ship_asset_init_ship(ship_t *ship, hull_class_t hull_class) {
    if (!ship) return;
    ship_cleanup(ship);
    memset(ship, 0, sizeof(*ship));
    (void)ship_manifest_bootstrap(ship);
    ship->hull_class = ((unsigned)hull_class < HULL_CLASS_COUNT)
        ? hull_class
        : HULL_CLASS_MINER;
    ship->hull = hull_max_for_class(ship->hull_class);
    ship->angle = PI_F * 0.5f;
    ship->comm_range = 1500.0f;
    memset(ship->towed_fragments, -1, sizeof(ship->towed_fragments));
    memset(ship->towed_pods, -1, sizeof(ship->towed_pods));
    ship->towed_scaffold = -1;
}

ship_asset_t *world_ship_asset_by_id(world_t *w, uint32_t asset_id) {
    if (!w || asset_id == SHIP_ASSET_ID_NONE) return NULL;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_asset_t *asset = &w->ship_assets[i];
        if (asset->active && asset->asset_id == asset_id) return asset;
    }
    return NULL;
}

const ship_asset_t *world_ship_asset_by_id_const(const world_t *w, uint32_t asset_id) {
    if (!w || asset_id == SHIP_ASSET_ID_NONE) return NULL;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w->ship_assets[i];
        if (asset->active && asset->asset_id == asset_id) return asset;
    }
    return NULL;
}

static ship_asset_t *world_ship_asset_free_slot(world_t *w) {
    if (!w) return NULL;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (!w->ship_assets[i].active) return &w->ship_assets[i];
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->destroyed ||
            asset->status != SHIP_ASSET_STATUS_DESTROYED ||
            asset->operator_kind != SHIP_ASSET_OPERATOR_NONE) {
            continue;
        }
        bool referenced = false;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (w->players[p].ship_asset_id == asset->asset_id) {
                referenced = true;
                break;
            }
        }
        for (int n = 0; !referenced && n < MAX_NPC_SHIPS; n++) {
            if (w->npc_ships[n].active &&
                w->npc_ships[n].ship_asset_id == asset->asset_id) {
                referenced = true;
                break;
            }
        }
        if (!referenced) return asset;
    }
    return NULL;
}

static uint32_t world_ship_asset_next_id(world_t *w) {
    if (!w) return SHIP_ASSET_ID_NONE;
    if (w->next_ship_asset_id == SHIP_ASSET_ID_NONE)
        w->next_ship_asset_id = 1;
    uint32_t id = w->next_ship_asset_id++;
    if (w->next_ship_asset_id == SHIP_ASSET_ID_NONE)
        w->next_ship_asset_id = 1;
    return id;
}

ship_asset_t *world_ship_asset_mint(world_t *w, hull_class_t hull_class,
                                    ship_asset_owner_kind_t owner_kind,
                                    int owner_station, int custody_station,
                                    ship_asset_provenance_t provenance,
                                    bool loaner, int build_station,
                                    const uint8_t owner_pubkey[32],
                                    const uint8_t owner_session[8]) {
    ship_asset_t *asset = world_ship_asset_free_slot(w);
    if (!asset) return NULL;
    ship_cleanup(&asset->ship);
    memset(asset, 0, sizeof(*asset));
    asset->active = true;
    asset->asset_id = world_ship_asset_next_id(w);
    asset->hull_class = ((unsigned)hull_class < HULL_CLASS_COUNT)
        ? hull_class
        : HULL_CLASS_MINER;
    asset->owner_kind = (uint8_t)owner_kind;
    asset->status = SHIP_ASSET_STATUS_STORED;
    asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
    asset->operator_slot = -1;
    asset->owner_station = (int16_t)owner_station;
    asset->custody_station = (int16_t)custody_station;
    asset->build_station = (int16_t)build_station;
    asset->provenance = (uint8_t)provenance;
    asset->loaner = loaner;
    asset->destroyed = false;
    if (owner_pubkey) memcpy(asset->owner_pubkey, owner_pubkey, 32);
    if (owner_session) memcpy(asset->owner_session, owner_session, 8);
    ship_asset_init_ship(&asset->ship, asset->hull_class);
    world_refresh_station_hull_inventories(w);
    return asset;
}

static bool ship_asset_copy_ship(ship_t *dst, const ship_t *src) {
    if (!dst || !src) return false;
    if (src->manifest.units || src->receipts_opaque)
        return ship_copy(dst, src);
    ship_cleanup(dst);
    *dst = *src;
    dst->manifest = (manifest_t){0};
    dst->receipts_opaque = NULL;
    return ship_manifest_bootstrap(dst);
}

int world_station_stored_hull_count(const world_t *w, int station_idx,
                                    hull_class_t hull_class) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return 0;
    int count = 0;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active || asset->destroyed) continue;
        if (asset->status != SHIP_ASSET_STATUS_STORED) continue;
        if (asset->custody_station != station_idx) continue;
        if ((unsigned)hull_class < HULL_CLASS_COUNT &&
            asset->hull_class != hull_class) continue;
        count++;
    }
    return count;
}

void world_refresh_station_hull_inventories(world_t *w) {
    if (!w) return;
    for (int s = 0; s < MAX_STATIONS; s++)
        memset(w->stations[s].stored_hull_count, 0,
               sizeof(w->stations[s].stored_hull_count));

    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active || asset->destroyed) continue;
        if (asset->status != SHIP_ASSET_STATUS_STORED) continue;
        if ((unsigned)asset->hull_class >= HULL_CLASS_COUNT) continue;
        int station_idx = asset->custody_station;
        if (station_idx < 0 || station_idx >= MAX_STATIONS) continue;
        if (!station_exists(&w->stations[station_idx])) continue;
        uint8_t *count =
            &w->stations[station_idx].stored_hull_count[asset->hull_class];
        if (*count < UINT8_MAX) (*count)++;
    }
}

bool world_player_release_ship_asset(world_t *w, int player_slot) {
    if (!w || player_slot < 0 || player_slot >= MAX_PLAYERS) return false;
    server_player_t *sp = &w->players[player_slot];
    bool released = false;
    ship_asset_t *asset = world_ship_asset_by_id(w, sp->ship_asset_id);
    if (asset && !asset->destroyed &&
        asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
        asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
        asset->operator_slot == player_slot) {
        (void)ship_asset_copy_ship(&asset->ship, &sp->ship);
        asset->status = SHIP_ASSET_STATUS_STORED;
        asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
        asset->operator_slot = -1;
        int custody = sp->docked ? sp->current_station : sp->nearby_station;
        if (custody >= 0 && custody < MAX_STATIONS &&
            station_exists(&w->stations[custody])) {
            asset->custody_station = (int16_t)custody;
        }
        released = true;
    }
    sp->ship_asset_id = SHIP_ASSET_ID_NONE;
    world_refresh_station_hull_inventories(w);
    return released;
}

bool world_player_transfer_ship_state(world_t *w, int dst_slot, int src_slot) {
    if (!w || dst_slot < 0 || dst_slot >= MAX_PLAYERS ||
        src_slot < 0 || src_slot >= MAX_PLAYERS || dst_slot == src_slot) {
        return false;
    }
    server_player_t *dst = &w->players[dst_slot];
    server_player_t *src = &w->players[src_slot];
    if (src->ship_asset_id == SHIP_ASSET_ID_NONE) return false;
    ship_asset_t *asset = world_ship_asset_by_id(w, src->ship_asset_id);
    if (!asset || asset->destroyed ||
        asset->status != SHIP_ASSET_STATUS_ASSIGNED ||
        asset->operator_kind != SHIP_ASSET_OPERATOR_PLAYER ||
        asset->operator_slot != src_slot) {
        return false;
    }

    ship_t copied = {0};
    if (!ship_copy(&copied, &src->ship)) return false;

    uint32_t src_asset_id = src->ship_asset_id;
    if (dst->ship_asset_id != SHIP_ASSET_ID_NONE &&
        dst->ship_asset_id != src_asset_id) {
        (void)world_player_release_ship_asset(w, dst_slot);
    }

    ship_cleanup(&dst->ship);
    dst->ship = copied;
    dst->ship_asset_id = src_asset_id;
    dst->current_station = src->current_station;
    dst->nearby_station = src->nearby_station;
    dst->docked = src->docked;
    dst->in_dock_range = src->in_dock_range;
    dst->docking_approach = src->docking_approach;
    dst->dock_berth = src->dock_berth;
    server_player_clear_transient_input(dst);

    asset->operator_slot = (int16_t)dst_slot;
    (void)world_ship_asset_sync_from_player(w, dst);

    src->ship_asset_id = SHIP_ASSET_ID_NONE;
    ship_cleanup(&src->ship);
    memset(&src->ship, 0, sizeof(src->ship));
    world_refresh_station_hull_inventories(w);
    return true;
}

bool world_ship_asset_sync_from_player(world_t *w, server_player_t *sp) {
    if (!w || !sp || sp->ship_asset_id == SHIP_ASSET_ID_NONE) return false;
    int player_slot = player_slot_for_ptr(w, sp);
    if (player_slot < 0 || player_slot >= MAX_PLAYERS) return false;
    ship_asset_t *asset = world_ship_asset_by_id(w, sp->ship_asset_id);
    if (!asset || asset->destroyed ||
        asset->status != SHIP_ASSET_STATUS_ASSIGNED ||
        asset->operator_kind != SHIP_ASSET_OPERATOR_PLAYER) {
        return false;
    }
    if (asset->operator_slot >= 0 &&
        asset->operator_slot < MAX_PLAYERS &&
        asset->operator_slot != player_slot &&
        w->players[asset->operator_slot].ship_asset_id == asset->asset_id) {
        return false;
    }
    if (!ship_asset_copy_ship(&asset->ship, &sp->ship)) return false;
    asset->hull_class = sp->ship.hull_class;
    asset->operator_slot = (int16_t)player_slot;
    if (sp->docked && sp->current_station >= 0 && sp->current_station < MAX_STATIONS)
        asset->custody_station = (int16_t)sp->current_station;
    return true;
}

bool world_ship_asset_sync_from_npc(world_t *w, int npc_slot) {
    if (!w || npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return false;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    if (!npc->active || npc->ship_asset_id == SHIP_ASSET_ID_NONE) return false;
    ship_asset_t *asset = world_ship_asset_by_id(w, npc->ship_asset_id);
    if (!asset || asset->destroyed ||
        asset->status != SHIP_ASSET_STATUS_ASSIGNED ||
        asset->operator_kind != SHIP_ASSET_OPERATOR_NPC) {
        return false;
    }
    const ship_t *src = world_npc_ship_for(w, npc_slot);
    if (!src) src = &npc->ship;
    if (!ship_asset_copy_ship(&asset->ship, src)) return false;
    asset->hull_class = asset->ship.hull_class;
    asset->operator_slot = (int16_t)npc_slot;
    if (npc->state == NPC_STATE_DOCKED &&
        npc->home_station >= 0 && npc->home_station < MAX_STATIONS) {
        asset->custody_station = (int16_t)npc->home_station;
    }
    return true;
}

static bool ship_asset_player_matches_owner(const ship_asset_t *asset,
                                            const server_player_t *sp) {
    if (!asset || !sp) return false;
    if (asset->owner_kind == SHIP_ASSET_OWNER_PLAYER_PUBKEY &&
        server_player_can_use_pubkey_persistence(sp)) {
        return memcmp(asset->owner_pubkey, sp->pubkey, 32) == 0;
    }
    if (asset->owner_kind == SHIP_ASSET_OWNER_PLAYER_SESSION &&
        ship_asset_session_nonzero(sp->session_token)) {
        return memcmp(asset->owner_session, sp->session_token, 8) == 0;
    }
    return false;
}

static bool ship_asset_player_can_reclaim_bound(const ship_asset_t *asset,
                                                const server_player_t *sp,
                                                int player_slot) {
    if (!asset || !sp || asset->destroyed ||
        asset->status == SHIP_ASSET_STATUS_DESTROYED) {
        return false;
    }
    if (ship_asset_player_matches_owner(asset, sp)) return true;
    return asset->owner_kind == SHIP_ASSET_OWNER_STATION &&
           asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
           asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
           asset->operator_slot == player_slot;
}

static bool ship_asset_assign_to_player(world_t *w, int player_slot,
                                        ship_asset_t *asset, int station_idx) {
    if (!w || !asset || player_slot < 0 || player_slot >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_slot];
    if (asset->destroyed || asset->status == SHIP_ASSET_STATUS_DESTROYED)
        return false;
    if (asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
        !(asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
          asset->operator_slot == player_slot)) {
        return false;
    }
    if (sp->ship_asset_id != SHIP_ASSET_ID_NONE &&
        sp->ship_asset_id != asset->asset_id) {
        ship_asset_t *old_asset = world_ship_asset_by_id(w, sp->ship_asset_id);
        if (old_asset && !old_asset->destroyed &&
            old_asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
            old_asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
            old_asset->operator_slot == player_slot) {
            (void)ship_asset_copy_ship(&old_asset->ship, &sp->ship);
            old_asset->status = SHIP_ASSET_STATUS_STORED;
            old_asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
            old_asset->operator_slot = -1;
            if (sp->current_station >= 0 && sp->current_station < MAX_STATIONS)
                old_asset->custody_station = (int16_t)sp->current_station;
        }
    }
    if (!ship_asset_copy_ship(&sp->ship, &asset->ship)) return false;
    sp->ship_asset_id = asset->asset_id;
    if (sp->ship.hull_class < 0 || sp->ship.hull_class >= HULL_CLASS_COUNT)
        sp->ship.hull_class = asset->hull_class;
    if (!(sp->ship.hull > 0.0f))
        sp->ship.hull = ship_max_hull(&sp->ship);
    if (sp->ship.comm_range <= 0.0f)
        sp->ship.comm_range = 1500.0f;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
    memset(sp->ship.towed_pods, -1, sizeof(sp->ship.towed_pods));
    sp->ship.towed_count = 0;
    sp->ship.towed_pod_count = 0;
    sp->ship.towed_scaffold = -1;
    sp->ship.tractor_active = false;
    sp->current_station = (station_idx >= 0 && station_idx < MAX_STATIONS)
        ? station_idx
        : asset->custody_station;
    if (sp->current_station < 0 || sp->current_station >= MAX_STATIONS ||
        !station_exists(&w->stations[sp->current_station])) {
        sp->current_station = 0;
    }
    sp->nearby_station = sp->current_station;
    sp->docked = true;
    sp->in_dock_range = true;
    sp->docking_approach = false;
    anchor_ship_in_station(sp, w);
    asset->hull_class = sp->ship.hull_class;
    asset->status = SHIP_ASSET_STATUS_ASSIGNED;
    asset->operator_kind = SHIP_ASSET_OPERATOR_PLAYER;
    asset->operator_slot = (int16_t)player_slot;
    asset->custody_station = (int16_t)sp->current_station;
    (void)world_ship_asset_sync_from_player(w, sp);
    gossip_dock_handshake(w, sp->current_station,
                          sp->ship.known_contracts,
                          &sp->ship.known_contract_count,
                          SHIP_KNOWN_CONTRACT_CAP,
                          &sp->ship.knowledge);
    world_refresh_station_hull_inventories(w);
    return true;
}

bool ship_asset_claim_for_player(world_t *w, int player_slot, int station_idx) {
    if (!w || player_slot < 0 || player_slot >= MAX_PLAYERS) return false;
    server_player_t *sp = &w->players[player_slot];
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&w->stations[station_idx])) {
        station_idx = 0;
    }

    ship_asset_t *bound = world_ship_asset_by_id(w, sp->ship_asset_id);
    if (bound && ship_asset_player_can_reclaim_bound(bound, sp, player_slot) &&
        (bound->status == SHIP_ASSET_STATUS_STORED ||
         (bound->status == SHIP_ASSET_STATUS_ASSIGNED &&
          bound->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
          bound->operator_slot == player_slot))) {
        return ship_asset_assign_to_player(w, player_slot, bound,
                                           bound->custody_station);
    }

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
            ship_asset_t *asset = &w->ship_assets[i];
            if (!asset->active || asset->destroyed) continue;
            if (asset->status != SHIP_ASSET_STATUS_STORED &&
                !(asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
                  asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
                  asset->operator_slot == player_slot)) {
                continue;
            }
            if (!ship_asset_player_matches_owner(asset, sp)) continue;
            if (pass == 0 && asset->custody_station != station_idx) continue;
            return ship_asset_assign_to_player(w, player_slot, asset,
                                               asset->custody_station);
        }
    }

    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active || asset->destroyed) continue;
        if (asset->status != SHIP_ASSET_STATUS_STORED) continue;
        if (asset->owner_kind != SHIP_ASSET_OWNER_STATION) continue;
        if (!asset->loaner) continue;
        if (asset->custody_station != station_idx) continue;
        return ship_asset_assign_to_player(w, player_slot, asset, station_idx);
    }

    sp->ship_asset_id = SHIP_ASSET_ID_NONE;
    (void)shipyard_queue_station_hull_request(w, station_idx, HULL_CLASS_MINER);
    return false;
}

static bool player_has_assigned_ship_asset(const world_t *w,
                                           const server_player_t *sp) {
    int player_slot = player_slot_for_ptr(w, sp);
    if (player_slot < 0 || !sp) return false;
    const ship_asset_t *asset =
        world_ship_asset_by_id_const(w, sp->ship_asset_id);
    return asset && !asset->destroyed &&
           asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
           asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
           asset->operator_slot == player_slot;
}

static bool player_claim_waiting_ship_asset(world_t *w, server_player_t *sp) {
    if (player_has_assigned_ship_asset(w, sp)) return true;
    int player_slot = player_slot_for_ptr(w, sp);
    if (!w || !sp || player_slot < 0 || !sp->docked) return false;
    int station_idx = sp->current_station;
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&w->stations[station_idx])) {
        station_idx = sp->nearby_station;
    }
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&w->stations[station_idx])) {
        station_idx = 0;
    }
    return ship_asset_claim_for_player(w, player_slot, station_idx);
}

static void ship_asset_retire_player_asset(world_t *w, server_player_t *sp) {
    if (!w || !sp || sp->ship_asset_id == SHIP_ASSET_ID_NONE) return;
    ship_asset_t *asset = world_ship_asset_by_id(w, sp->ship_asset_id);
    if (!asset) return;
    (void)ship_asset_copy_ship(&asset->ship, &sp->ship);
    asset->destroyed = true;
    asset->status = SHIP_ASSET_STATUS_DESTROYED;
    asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
    asset->operator_slot = -1;
    sp->ship_asset_id = SHIP_ASSET_ID_NONE;
    world_refresh_station_hull_inventories(w);
}

bool world_ship_assets_ensure_legacy_bindings(world_t *w) {
    if (!w) return false;
    bool ok = true;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active) continue;
        if (asset->destroyed ||
            asset->status == SHIP_ASSET_STATUS_DESTROYED) {
            asset->destroyed = true;
            asset->status = SHIP_ASSET_STATUS_DESTROYED;
            asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
            asset->operator_slot = -1;
            continue;
        }
        if (asset->status != SHIP_ASSET_STATUS_ASSIGNED) continue;

        bool valid_operator = false;
        int slot = asset->operator_slot;
        if (asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER) {
            valid_operator =
                slot >= 0 && slot < MAX_PLAYERS &&
                w->players[slot].connected &&
                w->players[slot].ship_asset_id == asset->asset_id;
        } else if (asset->operator_kind == SHIP_ASSET_OPERATOR_NPC) {
            valid_operator =
                slot >= 0 && slot < MAX_NPC_SHIPS &&
                w->npc_ships[slot].active &&
                w->npc_ships[slot].ship_asset_id == asset->asset_id;
        }
        if (valid_operator) continue;

        asset->status = SHIP_ASSET_STATUS_STORED;
        asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
        asset->operator_slot = -1;
        if (asset->custody_station < 0 ||
            asset->custody_station >= MAX_STATIONS) {
            if (asset->owner_station >= 0 &&
                asset->owner_station < MAX_STATIONS) {
                asset->custody_station = asset->owner_station;
            } else if (asset->build_station >= 0 &&
                       asset->build_station < MAX_STATIONS) {
                asset->custody_station = asset->build_station;
            } else {
                asset->custody_station = 0;
            }
        }
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active || npc->ship_asset_id == SHIP_ASSET_ID_NONE) continue;
        ship_asset_t *asset = world_ship_asset_by_id(w, npc->ship_asset_id);
        if (asset && !asset->destroyed &&
            asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
            asset->operator_kind == SHIP_ASSET_OPERATOR_NPC &&
            asset->operator_slot == n) {
            continue;
        }
        if (asset && !asset->destroyed &&
            asset->status == SHIP_ASSET_STATUS_STORED &&
            asset->owner_kind == SHIP_ASSET_OWNER_STATION &&
            !asset->loaner) {
            const ship_t *src = world_npc_ship_for(w, n);
            if (!src) src = &npc->ship;
            (void)ship_asset_copy_ship(&asset->ship, src);
            asset->hull_class = asset->ship.hull_class;
            asset->status = SHIP_ASSET_STATUS_ASSIGNED;
            asset->operator_kind = SHIP_ASSET_OPERATOR_NPC;
            asset->operator_slot = (int16_t)n;
            if (npc->home_station >= 0 && npc->home_station < MAX_STATIONS)
                asset->custody_station = (int16_t)npc->home_station;
            continue;
        }
        npc->ship_asset_id = SHIP_ASSET_ID_NONE;
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active || npc->ship_asset_id != SHIP_ASSET_ID_NONE) continue;
        ship_asset_t *asset = world_ship_asset_mint(
            w, npc->ship.hull_class, SHIP_ASSET_OWNER_STATION,
            npc->home_station, npc->home_station,
            SHIP_ASSET_PROVENANCE_LEGACY, false, -1, NULL, NULL);
        if (!asset) { ok = false; continue; }
        const ship_t *src = world_npc_ship_for(w, n);
        if (!src) src = &npc->ship;
        (void)ship_asset_copy_ship(&asset->ship, src);
        asset->status = SHIP_ASSET_STATUS_ASSIGNED;
        asset->operator_kind = SHIP_ASSET_OPERATOR_NPC;
        asset->operator_slot = (int16_t)n;
        npc->ship_asset_id = asset->asset_id;
    }
    world_refresh_station_hull_inventories(w);
    return ok;
}

static bool is_finished_good(commodity_t c);
static void sync_station_finished_inventory(station_t *st, commodity_t c);

static vec2 actor_stack_normal(int a, int b) {
    uint32_t h = (uint32_t)(a + 1) * 1103515245u ^
                 (uint32_t)(b + 1) * 2654435761u ^
                 0x9E3779B9u;
    float angle = ((float)(h & 0xFFFFu) / 65536.0f) * TWO_PI_F;
    return v2_from_angle(angle);
}

static bool launch_candidate_clear(const world_t *w, int player_slot,
                                   vec2 pos, float radius) {
    if (!w) return true;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (p == player_slot) continue;
        const server_player_t *other = &w->players[p];
        if (!other->connected || other->docked) continue;
        const hull_def_t *hull = ship_hull_def(&other->ship);
        float min_d = radius + hull->ship_radius + 32.0f;
        if (v2_dist_sq(pos, other->ship.pos) < min_d * min_d)
            return false;
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        const npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active || npc->state == NPC_STATE_DOCKED) continue;
        const hull_def_t *hull = npc_hull_def(npc);
        float min_d = radius + hull->ship_radius + 32.0f;
        if (v2_dist_sq(pos, npc->ship.pos) < min_d * min_d)
            return false;
    }
    return true;
}

static vec2 launch_clear_position(const world_t *w, int player_slot,
                                  const station_t *st, const ship_t *ship,
                                  vec2 away) {
    const hull_def_t *hull = ship_hull_def(ship);
    float ship_r = hull ? hull->ship_radius : 18.0f;
    float len = v2_len(away);
    if (len <= 1.0f) away = v2(0.0f, -1.0f);
    else away = v2_scale(away, 1.0f / len);

    float launch_r = st->dock_radius + ship_r + STATION_DOCK_APPROACH_OFFSET + 90.0f;
    float min_r = st->radius + ship_r + 180.0f;
    if (launch_r < min_r) launch_r = min_r;
    vec2 base = v2_add(st->pos, v2_scale(away, launch_r));
    if (launch_candidate_clear(w, player_slot, base, ship_r)) return base;

    vec2 tangent = v2(-away.y, away.x);
    float step = ship_r * 2.0f + 44.0f;
    for (int i = 1; i <= 6; i++) {
        float side = (i & 1) ? 1.0f : -1.0f;
        float lane = (float)((i + 1) / 2);
        vec2 candidate = v2_add(base, v2_scale(tangent, side * lane * step));
        candidate = v2_add(candidate, v2_scale(away, lane * 18.0f));
        if (launch_candidate_clear(w, player_slot, candidate, ship_r))
            return candidate;
    }
    return base;
}

static void launch_ship(world_t *w, server_player_t *sp) {
    if (!player_claim_waiting_ship_asset(w, sp)) {
        if (sp) {
            sp->docked = true;
            sp->in_dock_range = true;
            sp->docking_approach = false;
        }
        return;
    }
    sp->docked = false;
    sp->in_dock_range = false;
    sp->docking_approach = false;
    sp->nearby_station = -1;
    /* Kick and face the ship away from station so the player's first
     * forward thrust clears the berth instead of driving back into it. */
    const station_t *st = &w->stations[sp->current_station];
    vec2 away = v2_sub(sp->ship.pos, st->pos);
    float len = v2_len(away);
    if (len > 1.0f) {
        int player_slot = player_slot_for_ptr(w, sp);
        sp->ship.pos = launch_clear_position(w, player_slot, st, &sp->ship, away);
        sp->ship.angle = fixp_atan2f(away.y, away.x);
        sp->ship.vel = v2_scale(away, 95.0f / len);
    } else {
        sp->ship.pos = launch_clear_position(
            w, player_slot_for_ptr(w, sp), st, &sp->ship, v2(0.0f, -1.0f));
        sp->ship.angle = -PI_F * 0.5f;
        sp->ship.vel = v2(0.0f, -95.0f);
    }
    /* First launch: "Hull integrity 94%" */
    if (sp->ship.stat_ore_mined < 0.01f && sp->ship.stat_credits_earned < 0.01f)
        sp->ship.hull = ship_max_hull(&sp->ship) * 0.94f;
    SIM_LOG("[sim] player %d launched\n", sp->id);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_LAUNCH, .player_id = sp->id});
}

static void remove_towed_pod_slot(ship_t *ship, int tow_slot) {
    if (!ship || tow_slot < 0 || tow_slot >= ship->towed_pod_count) return;
    ship->towed_pod_count--;
    ship->towed_pods[tow_slot] = ship->towed_pods[ship->towed_pod_count];
    ship->towed_pods[ship->towed_pod_count] = -1;
}

static int ship_towed_pod_capacity(const ship_t *ship) {
    int cap = 2 + (ship ? ship->tractor_level : 0) * 2;
    if (cap < 0) cap = 0;
    if (cap > 10) cap = 10;
    return cap;
}

static int station_first_dock_module(const station_t *st) {
    if (!st) return -1;
    for (int i = 0; i < st->module_count && i < MAX_MODULES_PER_STATION; i++) {
        const station_module_t *module = &st->modules[i];
        if (!module->scaffold && module->type == MODULE_DOCK)
            return i;
    }
    return -1;
}

static bool station_dock_can_tractor_trade_pod(const station_t *st,
                                               int module_idx,
                                               const cargo_pod_t *pod) {
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->towed_by >= 0 || module_idx < 0 ||
        module_idx >= st->module_count ||
        module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    const station_module_t *module = &st->modules[module_idx];
    return !module->scaffold && module->type == MODULE_DOCK;
}

static bool cargo_pod_is_station_market_pod(const world_t *w,
                                            const cargo_pod_t *pod,
                                            int station_idx) {
    int ps = -1, pm = -1;
    if (!w || !pod || station_idx < 0 || station_idx >= MAX_STATIONS)
        return false;
    if (!cargo_pod_module_tractor_indices(pod, &ps, &pm) ||
        ps != station_idx) {
        return false;
    }
    return station_dock_can_tractor_trade_pod(&w->stations[station_idx],
                                              pm, pod);
}

static bool cargo_pod_set_station_dock_custody(world_t *w,
                                               int pod_idx,
                                               int station_idx) {
    if (!w || pod_idx < 0 || pod_idx >= MAX_CARGO_PODS ||
        station_idx < 0 || station_idx >= MAX_STATIONS) {
        return false;
    }
    station_t *st = &w->stations[station_idx];
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!pod->active || pod->kind != CARGO_POD_CARGO) return false;
    int dock_idx = station_first_dock_module(st);
    if (dock_idx < 0) return false;

    pod->towed_by = -1;
    cargo_pod_set_module_tractor(pod, station_idx, dock_idx);

    vec2 anchor = station_module_cargo_hold_anchor(
        w, st, station_idx, dock_idx, pod,
        station_module_cargo_mouth(st, &st->modules[dock_idx], pod));
    vec2 away = v2_sub(pod->pos, anchor);
    float dist = v2_len(away);
    float max_hold = HOPPER_PULL_RANGE * 0.78f;
    if (dist > max_hold) {
        if (dist > 0.001f) {
            away = v2_scale(away, 1.0f / dist);
        } else {
            away = v2_from_angle(module_angle_ring(st, st->modules[dock_idx].ring,
                                                   st->modules[dock_idx].slot));
        }
        pod->pos = v2_add(anchor, v2_scale(away, max_hold));
        pod->vel = v2_scale(pod->vel, 0.25f);
    }
    st->module_active_pulse[dock_idx] = 1.0f;
    return true;
}

bool cargo_pod_has_exact_manifest(const cargo_pod_t *pod,
                                  commodity_t commodity) {
    if (!pod || !pod->active || pod->kind != CARGO_POD_CARGO) return false;
    if (pod->shipment_id != 0) return false;
    if (pod->commodity != commodity) return false;
    if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
        return false;
    if (pod->manifest_count > CARGO_POD_MANIFEST_CAP) return false;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if ((commodity_t)pod->manifest_units[i].commodity != commodity)
            return false;
    }
    return true;
}

void cargo_pod_set_shell_frame(cargo_pod_t *pod, const cargo_unit_t *frame) {
    if (!pod || !frame || (commodity_t)frame->commodity != COMMODITY_FRAME)
        return;
    pod->shell_frame = *frame;
    pod->shell_frame.quantity = 1;
    pod->has_shell_frame = true;
}

bool cargo_pod_fold_shell_to_frame(cargo_pod_t *pod) {
    if (!pod || !pod->active || !pod->has_shell_frame ||
        (commodity_t)pod->shell_frame.commodity != COMMODITY_FRAME) {
        return false;
    }

    cargo_unit_t shell = pod->shell_frame;
    vec2 pos = pod->pos;
    vec2 vel = pod->vel;
    float radius = pod->radius;
    float rotation = pod->rotation;
    float spin = pod->spin;
    float age = pod->age;
    int8_t towed_by = pod->towed_by;

    memset(pod, 0, sizeof(*pod));
    pod->active = true;
    pod->kind = CARGO_POD_CARGO;
    pod->commodity = COMMODITY_FRAME;
    pod->quantity = 1;
    pod->manifest_count = 1;
    pod->manifest_units[0] = shell;
    pod->pos = pos;
    pod->vel = vel;
    pod->radius = radius > 0.0f ? radius : 18.0f;
    pod->rotation = rotation;
    pod->spin = spin;
    pod->age = age;
    pod->towed_by = towed_by;
    return true;
}

int ship_towed_pods_manifest_count(const world_t *w, const ship_t *ship,
                                   commodity_t commodity) {
    if (!w || !ship || commodity >= COMMODITY_COUNT) return 0;
    int total = 0;
    for (int t = 0; t < ship->towed_pod_count && t < 10; t++) {
        int idx = ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &w->cargo_pods[idx];
        if (!cargo_pod_has_exact_manifest(pod, commodity)) continue;
        total += (int)pod->manifest_count;
    }
    return total;
}

bool ship_towed_pods_take_manifest_unit(world_t *w, ship_t *ship,
                                        commodity_t commodity,
                                        cargo_unit_t *out_unit) {
    if (!w || !ship || !out_unit || commodity >= COMMODITY_COUNT)
        return false;
    for (int t = 0; t < ship->towed_pod_count && t < 10; t++) {
        int idx = ship->towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        cargo_pod_t *pod = &w->cargo_pods[idx];
        if (!cargo_pod_has_exact_manifest(pod, commodity)) continue;
        uint16_t unit_idx = (uint16_t)(pod->manifest_count - 1u);
        *out_unit = pod->manifest_units[unit_idx];
        memset(&pod->manifest_units[unit_idx], 0,
               sizeof(pod->manifest_units[unit_idx]));
        pod->manifest_count--;
        pod->quantity--;
        if (pod->manifest_count == 0) {
            if (!cargo_pod_fold_shell_to_frame(pod)) {
                memset(pod, 0, sizeof(*pod));
                pod->towed_by = -1;
                remove_towed_pod_slot(ship, t);
            }
        }
        return true;
    }
    return false;
}

int spawn_cargo_pod(world_t *w, vec2 pos, vec2 vel, commodity_t commodity,
                    uint16_t quantity, cargo_pod_kind_t kind) {
    if (!w || commodity >= COMMODITY_COUNT || quantity == 0 ||
        kind == CARGO_POD_NONE) {
        return -1;
    }
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (pod->active) continue;
        memset(pod, 0, sizeof(*pod));
        pod->active = true;
        pod->kind = kind;
        pod->commodity = commodity;
        pod->quantity = quantity;
        pod->pos = pos;
        pod->vel = vel;
        pod->radius = (kind == CARGO_POD_GAS) ? 15.0f : 18.0f;
        pod->rotation = rand_range(&w->rng, 0.0f, TWO_PI_F);
        pod->spin = rand_range(&w->rng, -1.4f, 1.4f);
        pod->age = 0.0f;
        pod->towed_by = -1;
        return i;
    }
    return -1;
}

static bool cargo_pod_manifest_units_valid(commodity_t commodity,
                                           const cargo_unit_t *units,
                                           uint16_t unit_count) {
    if (commodity >= COMMODITY_COUNT || !units || unit_count == 0 ||
        unit_count > CARGO_POD_MANIFEST_CAP) {
        return false;
    }
    for (uint16_t i = 0; i < unit_count; i++) {
        if ((commodity_t)units[i].commodity != commodity)
            return false;
    }
    return true;
}

static int spawn_cargo_pod_with_manifest_internal(world_t *w, vec2 pos,
                                                  vec2 vel,
                                                  commodity_t commodity,
                                                  const cargo_unit_t *units,
                                                  uint16_t unit_count,
                                                  cargo_pod_kind_t kind,
                                                  float rotation,
                                                  float spin) {
    if (!w || kind == CARGO_POD_NONE ||
        !cargo_pod_manifest_units_valid(commodity, units, unit_count)) {
        return -1;
    }
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (pod->active) continue;
        memset(pod, 0, sizeof(*pod));
        pod->active = true;
        pod->kind = kind;
        pod->commodity = commodity;
        pod->quantity = unit_count;
        pod->manifest_count = unit_count;
        memcpy(pod->manifest_units, units,
               (size_t)unit_count * sizeof(pod->manifest_units[0]));
        pod->pos = pos;
        pod->vel = vel;
        pod->radius = (kind == CARGO_POD_GAS) ? 15.0f : 18.0f;
        pod->rotation = rotation;
        pod->spin = spin;
        pod->age = 0.0f;
        pod->towed_by = -1;
        return i;
    }
    return -1;
}

int spawn_cargo_pod_with_manifest(world_t *w, vec2 pos, vec2 vel,
                                  commodity_t commodity,
                                  const cargo_unit_t *units,
                                  uint16_t unit_count,
                                  cargo_pod_kind_t kind) {
    if (!w || kind == CARGO_POD_NONE ||
        !cargo_pod_manifest_units_valid(commodity, units, unit_count)) {
        return -1;
    }
    return spawn_cargo_pod_with_manifest_internal(
        w, pos, vel, commodity, units, unit_count, kind,
        rand_range(&w->rng, 0.0f, TWO_PI_F),
        rand_range(&w->rng, -1.4f, 1.4f));
}

int spawn_cargo_pod_with_manifest_deterministic(world_t *w, vec2 pos,
                                                vec2 vel,
                                                commodity_t commodity,
                                                const cargo_unit_t *units,
                                                uint16_t unit_count,
                                                cargo_pod_kind_t kind,
                                                float rotation,
                                                float spin) {
    return spawn_cargo_pod_with_manifest_internal(
        w, pos, vel, commodity, units, unit_count, kind, rotation, spin);
}

static void drop_ship_cargo_pods(world_t *w, server_player_t *sp) {
    if (!w || !sp) return;
    for (int c = 0; c < COMMODITY_COUNT; c++) {
        int units = (int)floorf(sp->ship.cargo[c] + 0.0001f);
        while (units > 0) {
            int q = units > 20 ? 20 : units;
            float angle = ((float)c * 1.618f) + (float)units * 0.37f;
            float dist = ship_hull_def(&sp->ship)->ship_radius + 32.0f +
                         (float)(units % 5) * 7.0f;
            vec2 dir = v2_from_angle(angle);
            vec2 pos = v2_add(sp->ship.pos, v2_scale(dir, dist));
            vec2 vel = v2_add(sp->ship.vel, v2_scale(dir, 35.0f));
            (void)spawn_cargo_pod(w, pos, vel, (commodity_t)c, (uint16_t)q,
                                  CARGO_POD_CARGO);
            units -= q;
        }
    }
}

static void world_seed_frame_pod_near(world_t *w,
                                      int station_idx,
                                      vec2 pos,
                                      uint16_t count,
                                      const uint8_t origin[8],
                                      float rotation) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_exists(&w->stations[station_idx]) ||
        !origin || count == 0 || count > CARGO_POD_MANIFEST_CAP) {
        return;
    }
    cargo_unit_t frames[16] = {{0}};
    for (uint16_t i = 0; i < count; i++) {
        if (!hash_legacy_migrate_unit(origin, COMMODITY_FRAME, i, &frames[i]))
            return;
        frames[i].origin_station = (uint8_t)station_idx;
    }
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (pod->active) continue;
        memset(pod, 0, sizeof(*pod));
        pod->active = true;
        pod->kind = CARGO_POD_CARGO;
        pod->commodity = COMMODITY_FRAME;
        pod->quantity = count;
        pod->manifest_count = count;
        memcpy(pod->manifest_units, frames,
               (size_t)count * sizeof(frames[0]));
        pod->pos = pos;
        pod->vel = v2(0.0f, 0.0f);
        pod->radius = 18.0f;
        pod->rotation = rotation;
        pod->spin = 0.0f;
        pod->towed_by = -1;
        return;
    }
}

static void world_seed_kepler_frame_pod(world_t *w) {
    if (!w || !station_exists(&w->stations[1])) return;
    const uint8_t origin[8] = { 'K','E','P','L','E','R','v','1' };
    vec2 pos = v2_add(w->stations[1].pos, v2(320.0f, -80.0f));
    world_seed_frame_pod_near(w, 1, pos, 16, origin, 0.35f);
}

static void world_seed_prospect_frame_shell_pod(world_t *w) {
    if (!w || !station_exists(&w->stations[0])) return;
    const station_t *prospect = &w->stations[0];
    int furnace_idx = -1;
    for (int i = 0; i < prospect->module_count; i++) {
        if (prospect->modules[i].type == MODULE_FURNACE &&
            module_instance_input_ore(&prospect->modules[i]) ==
                COMMODITY_FERRITE_ORE) {
            furnace_idx = i;
            break;
        }
    }
    if (furnace_idx < 0) return;
    const station_module_t *furnace = &prospect->modules[furnace_idx];
    vec2 shell_pos = module_world_pos_ring(prospect, furnace->ring,
                                           furnace->slot);
    const uint8_t origin[8] = { 'P','R','O','S','H','E','L','L' };
    world_seed_frame_pod_near(w, 0, shell_pos, 16, origin, 0.70f);
}

static bool cargo_pod_fits_contract_exact(const cargo_pod_t *pod,
                                          const contract_t *ct);

static float black_market_pod_quote(const station_t *st,
                                    const cargo_pod_t *pod) {
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->commodity >= COMMODITY_COUNT || pod->quantity == 0 ||
        !station_policy_accepts_contract_bound_cargo(st)) {
        return 0.0f;
    }

    commodity_t c = pod->commodity;
    float value = 0.0f;
    if (pod->manifest_count > 0) {
        if (pod->manifest_count != pod->quantity ||
            pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
            return 0.0f;
        }
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            const cargo_unit_t *unit = &pod->manifest_units[u];
            if ((commodity_t)unit->commodity != c) return 0.0f;
            float unit_value = station_buy_price_unit(st, unit);
            if (unit_value <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON) {
                unit_value = st->base_price[c] *
                    prefix_class_price_multiplier((int)unit->prefix_class);
            }
            unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
            value += unit_value;
        }
    } else {
        float unit_value = station_buy_price(st, c);
        if (unit_value <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
            unit_value = st->base_price[c];
        value = unit_value * (float)pod->quantity;
    }

    return value > FLOAT_EPSILON
        ? value * BLACK_MARKET_CARGO_MARKDOWN
        : 0.0f;
}

static float station_intake_pod_quote(world_t *w,
                                      station_t *st,
                                      int station_idx,
                                      const cargo_pod_t *pod,
                                      bool *out_by_contract) {
    if (out_by_contract) *out_by_contract = false;
    if (!w || !st || !pod || !pod->active ||
        pod->shipment_id != 0 || pod->commodity >= COMMODITY_COUNT ||
        pod->quantity == 0) {
        return 0.0f;
    }
    commodity_t c = pod->commodity;
    int matched_contract = -1;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->station_index != station_idx) continue;
        if (!cargo_pod_fits_contract_exact(pod, ct)) continue;
        matched_contract = k;
        break;
    }

    if (matched_contract < 0 &&
        station_policy_accepts_contract_bound_cargo(st)) {
        float value = black_market_pod_quote(st, pod);
        if (value > FLOAT_EPSILON) {
            if (out_by_contract) *out_by_contract = true;
            return value;
        }
    }

    float price = matched_contract >= 0
        ? contract_price(&w->contracts[matched_contract])
        : station_buy_price(st, c);
    if (price <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
        price = st->base_price[c];
    if (price <= FLOAT_EPSILON) return 0.0f;

    float value = 0.0f;
    if (pod->manifest_count > 0) {
        if (pod->manifest_count != pod->quantity ||
            pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
            return 0.0f;
        }
        for (uint16_t u = 0; u < pod->manifest_count; u++) {
            const cargo_unit_t *unit = &pod->manifest_units[u];
            if ((commodity_t)unit->commodity != c) return 0.0f;
            float unit_value = matched_contract >= 0
                ? price
                : station_buy_price_unit(st, unit);
            if (unit_value <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON) {
                unit_value = st->base_price[c] *
                    prefix_class_price_multiplier((int)unit->prefix_class);
            }
            unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
            value += unit_value;
        }
    } else {
        value = price * (float)pod->quantity;
    }
    if (value <= FLOAT_EPSILON) return 0.0f;
    if (out_by_contract && matched_contract >= 0) *out_by_contract = true;
    return value;
}

static bool station_intake_pay_for_pod(world_t *w,
                                       server_player_t *sp,
                                       station_t *st,
                                       int station_idx,
                                       cargo_pod_t *pod) {
    if (!w || !sp || !st || !pod) return false;
    bool by_contract = false;
    float value = station_intake_pod_quote(w, st, station_idx, pod,
                                           &by_contract);
    if (value <= FLOAT_EPSILON) return false;

    uint16_t units = pod->quantity;
    if (server_player_can_use_pubkey_persistence(sp)) {
        ledger_earn_by_pubkey(st, sp->pubkey, value);
        ledger_record_ore_sold(st, sp->pubkey, units,
                               (uint8_t)pod->commodity);
    } else {
        ledger_earn(st, sp->session_token, value);
    }
    sp->ship.stat_credits_earned += value;

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->station_index != station_idx) continue;
        if (!cargo_pod_fits_contract_exact(pod, ct)) continue;
        ct->quantity_needed -= (float)units;
        if (ct->quantity_needed <= 0.01f) {
            ct->active = false;
            emit_event(w, (sim_event_t){
                .type = SIM_EVENT_CONTRACT_COMPLETE,
                .contract_complete.action = CONTRACT_TRACTOR});
        }
        break;
    }

    SIM_LOG("[intake] player %d sold %s crate (%u units) for %.0f cr at %s\n",
            sp->id, commodity_short_name(pod->commodity),
            (unsigned)units, value, st->name);
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_SELL, .player_id = sp->id,
        .sell = { .station = station_idx,
                  .grade = MINING_GRADE_COMMON,
                  .base_cr = (int)lroundf(value),
                  .bonus_cr = 0,
                  .by_contract = by_contract ? 1u : 0u }});
    return true;
}

static float SIGNAL_MAYBE_UNUSED
try_sell_towed_pods(world_t *w, server_player_t *sp,
                    station_t *st, int station_idx,
                    commodity_t filter,
                    mining_grade_t grade_filter) {
    if (!w || !sp || !st) return 0.0f;
    float payout = 0.0f;
    if (grade_filter < MINING_GRADE_COUNT &&
        grade_filter != MINING_GRADE_COMMON) {
        return 0.0f;
    }

    for (int t = sp->ship.towed_pod_count - 1; t >= 0; t--) {
        int idx = sp->ship.towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS || !w->cargo_pods[idx].active) {
            remove_towed_pod_slot(&sp->ship, t);
            continue;
        }
        cargo_pod_t *pod = &w->cargo_pods[idx];
        if (pod->shipment_id != 0) continue;
        commodity_t c = pod->commodity;
        if (c >= COMMODITY_COUNT) continue;
        if (filter != COMMODITY_COUNT && filter != c) continue;

        int units = (int)pod->quantity;
        if (units <= 0) {
            memset(pod, 0, sizeof(*pod));
            pod->towed_by = -1;
            remove_towed_pod_slot(&sp->ship, t);
            continue;
        }
        int contract_idx = -1;
        contract_t *matched_contract = NULL;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            contract_t *ct = &w->contracts[k];
            if (!ct->active || ct->station_index != station_idx) continue;
            if (!cargo_pod_fits_contract_exact(pod, ct)) continue;
            contract_idx = k;
            matched_contract = ct;
            break;
        }
        float price = station_buy_price(st, c);
        if (matched_contract) price = contract_price(matched_contract);
        if (price <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
            price = st->base_price[c];
        if (price <= FLOAT_EPSILON) continue;

        int sell_units = units;
        if (sell_units <= 0) continue;

        float value = 0.0f;
        if (!matched_contract && station_policy_accepts_contract_bound_cargo(st)) {
            value = black_market_pod_quote(st, pod);
        } else if (pod->manifest_count > 0) {
            if (pod->manifest_count != pod->quantity ||
                pod->manifest_count > CARGO_POD_MANIFEST_CAP ||
                !is_finished_good(c)) {
                continue;
            }
            bool ok = true;
            for (uint16_t u = 0; u < pod->manifest_count; u++) {
                const cargo_unit_t *unit = &pod->manifest_units[u];
                if ((commodity_t)unit->commodity != c) {
                    ok = false;
                    break;
                }
                float unit_value = matched_contract
                    ? price
                    : station_buy_price_unit(st, unit);
                unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
                value += unit_value;
            }
            if (!ok) continue;
        } else {
            value = price * (float)sell_units;
        }
        if (value <= FLOAT_EPSILON) continue;
        if (!cargo_pod_set_station_dock_custody(w, idx, station_idx))
            continue;
        remove_towed_pod_slot(&sp->ship, t);

        if (server_player_can_use_pubkey_persistence(sp)) {
            ledger_earn_by_pubkey(st, sp->pubkey, value);
            ledger_record_ore_sold(st, sp->pubkey, (uint32_t)sell_units, (uint8_t)c);
        } else {
            ledger_earn(st, sp->session_token, value);
        }
        sp->ship.stat_credits_earned += value;
        payout += value;
        if (matched_contract) {
            matched_contract->quantity_needed -= (float)sell_units;
            if (matched_contract->quantity_needed <= 0.01f) {
                matched_contract->active = false;
                emit_event(w, (sim_event_t){
                    .type = SIM_EVENT_CONTRACT_COMPLETE,
                    .contract_complete.action = CONTRACT_TRACTOR});
            }
            (void)contract_idx;
        }

        SIM_LOG("[sim] player %d sold towed %s pod (%d units) for %.0f cr at %s\n",
                sp->id, commodity_short_name(c), sell_units, value, st->name);
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_SELL, .player_id = sp->id,
            .sell = { .station = station_idx,
		                      .grade = MINING_GRADE_COMMON,
		                      .base_cr = (int)lroundf(value),
		                      .bonus_cr = 0,
		                      .by_contract = matched_contract ||
                                      station_policy_accepts_contract_bound_cargo(st)
                                  ? 1u : 0u }});
        break;
    }
    return payout;
}

static bool cargo_pod_matches_buy_grade(const cargo_pod_t *pod,
                                        mining_grade_t grade) {
    if (!pod || grade >= MINING_GRADE_COUNT) return true;
    if (pod->manifest_count == 0)
        return grade == MINING_GRADE_COMMON;
    if (pod->manifest_count != pod->quantity) return false;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if ((mining_grade_t)pod->manifest_units[i].grade != grade)
            return false;
    }
    return true;
}

static float station_market_pod_sell_quote(const station_t *st,
                                           const cargo_pod_t *pod) {
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->commodity >= COMMODITY_COUNT || pod->quantity == 0 ||
        pod->shipment_id != 0) {
        return 0.0f;
    }
    commodity_t c = pod->commodity;
    float fallback = station_sell_price(st, c);
    if (fallback <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
        fallback = st->base_price[c];
    if (fallback <= FLOAT_EPSILON) return 0.0f;

    if (pod->manifest_count > 0) {
        if (pod->manifest_count != pod->quantity ||
            pod->manifest_count > CARGO_POD_MANIFEST_CAP ||
            !is_finished_good(c)) {
            return 0.0f;
        }
        float value = 0.0f;
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            const cargo_unit_t *unit = &pod->manifest_units[i];
            if ((commodity_t)unit->commodity != c) return 0.0f;
            float unit_value = station_sell_price_unit(st, unit);
            unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
            value += unit_value;
        }
        return value;
    }
    return fallback * (float)pod->quantity;
}

static int try_buy_station_market_pod(world_t *w,
                                      server_player_t *sp,
                                      station_t *st,
                                      int station_idx,
                                      commodity_t commodity,
                                      mining_grade_t grade,
                                      bool prefer_pod,
                                      uint16_t preferred_pod_idx) {
    if (!w || !sp || !st || station_idx < 0 ||
        station_idx >= MAX_STATIONS || commodity >= COMMODITY_COUNT) {
        return 0;
    }

    int best_idx = -1;
    float best_quote = 0.0f;
    int start = 0;
    int end = MAX_CARGO_PODS;
    if (prefer_pod) {
        if (preferred_pod_idx >= MAX_CARGO_PODS) return 0;
        start = (int)preferred_pod_idx;
        end = start + 1;
    }
    for (int i = start; i < end; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_is_station_market_pod(w, pod, station_idx))
            continue;
        if (pod->commodity != commodity || pod->shipment_id != 0)
            continue;
        if (!cargo_pod_matches_buy_grade(pod, grade))
            continue;
        float quote = station_market_pod_sell_quote(st, pod);
        if (quote <= FLOAT_EPSILON) continue;
        best_idx = i;
        best_quote = quote;
        break;
    }
    if (best_idx < 0) return 0;

    int tow_space = ship_towed_pod_capacity(&sp->ship) -
        sp->ship.towed_pod_count;
    if (tow_space <= 0) {
        SIM_LOG("[buy-pod] REJECT: tow slots full for c=%d\n", (int)commodity);
        return -1;
    }

    bool pubkey_ledger = server_player_can_use_pubkey_persistence(sp);
    float balance = pubkey_ledger
        ? ledger_balance_by_pubkey(st, sp->pubkey)
        : ledger_balance(st, sp->session_token);
    bool neural_bot_credit =
        (sp->server_brain_mode == SERVER_BRAIN_MODE_NEURAL_FLIGHT ||
         sp->server_brain_mode == SERVER_BRAIN_MODE_HEURISTIC_LOGISTICS) &&
        sp->autopilot_mode != 0 &&
        sp->autopilot_state == AUTOPILOT_STEP_LOGISTICS_BUY &&
        sp->autopilot_cargo == commodity &&
        sp->autopilot_station_target >= 0 &&
        sp->autopilot_station_target < MAX_STATIONS;
    bool spent = false;
    if (neural_bot_credit) {
        if (pubkey_ledger) {
            ledger_force_debit_by_pubkey(st, sp->pubkey, best_quote,
                                         &sp->ship);
        } else {
            ledger_force_debit(st, sp->session_token, best_quote,
                               &sp->ship);
        }
        spent = true;
    } else if (balance + FLOAT_EPSILON >= best_quote) {
        spent = pubkey_ledger
            ? ledger_spend_by_pubkey(st, sp->pubkey, best_quote, &sp->ship)
            : ledger_spend(st, sp->session_token, best_quote, &sp->ship);
    }
    if (!spent) {
        SIM_LOG("[buy-pod] REJECT: c=%d cost=%.2f bal=%.2f\n",
                (int)commodity, best_quote, balance);
        return -1;
    }

    cargo_pod_t *pod = &w->cargo_pods[best_idx];
    cargo_pod_clear_module_tractor(pod);
    pod->towed_by = (int8_t)sp->id;
    vec2 pod_dir = v2_from_angle(sp->ship.angle + PI_F);
    float spacing = 46.0f + 8.0f * (float)sp->ship.towed_pod_count;
    pod->pos = v2_add(sp->ship.pos, v2_scale(pod_dir, spacing));
    pod->vel = sp->ship.vel;
    sp->ship.towed_pods[sp->ship.towed_pod_count++] = (int16_t)best_idx;

    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_BUY, .player_id = sp->id,
        .buy = { .station = station_idx,
                 .commodity = (uint8_t)commodity,
                 .grade = (uint8_t)grade,
                 .cost = (int)lroundf(best_quote),
                 .quantity = (uint16_t)pod->quantity }});
    SIM_LOG("[buy-pod] OK player %d bought station-held %s pod (%u units) for %.0f cr at %s\n",
            sp->id, commodity_short_name(commodity),
            (unsigned)pod->quantity, best_quote, st->name);
    return 1;
}

static bool cargo_pod_fits_contract_exact(const cargo_pod_t *pod,
                                          const contract_t *ct) {
    if (!pod || !ct || !ct->active) return false;
    if (ct->action != CONTRACT_TRACTOR) return false;
    if (ct->commodity < COMMODITY_RAW_ORE_COUNT) return false;
    if (pod->shipment_id != 0 || pod->commodity != ct->commodity) return false;
    if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity)
        return false;
    if (pod->quantity <= 0) return false;
    int needed = (int)floorf(ct->quantity_needed + 0.0001f);
    if (needed < (int)pod->quantity) return false;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if (!contract_fit_is_ok(contract_fit_cargo_unit(
                ct, &pod->manifest_units[i]))) {
            return false;
        }
    }
    return true;
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
     * verified pubkey players debit their pubkey entry (the same one that
     * carries their earnings); legacy/pending players use session-token. */
    int fee = station_spawn_fee(&w->stations[best]);
    if (server_player_can_use_pubkey_persistence(sp)) {
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
    ship_asset_retire_player_asset(w, sp);
    drop_ship_cargo_pods(w, sp);
    /* Reset attribution for next life. */
    memset(sp->last_damage_killer_token, 0, 8);
    sp->last_damage_cause = DEATH_CAUSE_UNKNOWN;
    clear_ship_cargo(&sp->ship);
    /* Release towed fragments */
    sp->ship.towed_count = 0;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
    for (int t = 0; t < sp->ship.towed_pod_count; t++) {
        int idx = sp->ship.towed_pods[t];
        if (idx >= 0 && idx < MAX_CARGO_PODS && w->cargo_pods[idx].active)
            w->cargo_pods[idx].towed_by = -1;
    }
    sp->ship.towed_pod_count = 0;
    memset(sp->ship.towed_pods, -1, sizeof(sp->ship.towed_pods));
    sp->current_station = best;
    sp->nearby_station = best;
    sp->dock_berth = 0;
    sp->docked = true;
    sp->in_dock_range = true;
    sp->docking_approach = false;
    if (!ship_asset_claim_for_player(w, sp->id, best)) {
        ship_cleanup(&sp->ship);
        memset(&sp->ship, 0, sizeof(sp->ship));
        (void)ship_manifest_bootstrap(&sp->ship);
        sp->ship.hull_class = HULL_CLASS_MINER;
        sp->ship.hull = 0.0f;
        sp->ship.angle = PI_F * 0.5f;
        sp->ship.comm_range = 1500.0f;
        memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
        memset(sp->ship.towed_pods, -1, sizeof(sp->ship.towed_pods));
        sp->ship.towed_scaffold = -1;
        sp->ship.pos = dock_berth_pos(&w->stations[best], 0);
        sp->ship.vel = v2(0.0f, 0.0f);
    }
    SIM_LOG("[sim] player %d emergency recovered at station %d (fee %d, asset %u)\n",
            sp->id, best, fee, sp->ship_asset_id);
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
    if (impact <= 0.0f || sp->docked || w->player_only_mode) return;
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
 *   2. thrown_by_token attributes the kill during the ballistic window;
 *      self-damage is suppressed so your own thrown rocks don't kill you
 *      on the rebound.
 *   3. Damage scales with rock radius. An XL rock hits ~2.5× harder
 *      than an S-tier fragment. Free signal that bigger rocks matter. */
static void resolve_ship_asteroid_collision(world_t *w, server_player_t *sp, asteroid_t *a) {
    /* Geometric push-out + mass-equal bounce live in sim_ship now;
     * player-only attribution / self-damage suppression sits on top. */
    vec2 asteroid_vel_before = a->vel;
    bool asteroid_dirty_before = a->net_dirty;
    float impact = resolve_ship_asteroid_pushback(&sp->ship, a);
    if (w->player_only_mode) {
        a->vel = asteroid_vel_before;
        a->net_dirty = asteroid_dirty_before;
    }
    if (impact <= 0.0f) return;
    ship_collision_count++;
    if (w->player_only_mode) return;

    /* Self-damage skip: your own ballistic rock can't hurt you. The
     * pushback already resolved geometrically; the first impact consumes
     * combat ownership whether it damages the target or not. */
    bool attributed = asteroid_is_ballistic(a);
    uint8_t thrown_token[8] = {0};
    if (attributed) {
        memcpy(thrown_token, a->thrown_by_token, sizeof(thrown_token));
        asteroid_clear_thrown(a);
    }
    bool self = attributed && memcmp(thrown_token, sp->session_token, 8) == 0;
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
            attributed ? thrown_token : NULL, cause, a->pos);
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
    if (sp->docked || w->player_only_mode) return;
    float dmg = collision_damage_for(impact, 1.0f);
    if (dmg > 0.0f) apply_ship_damage(w, sp, dmg);
}

/* ================================================================== */
/* Mining target                                                      */
/* ================================================================== */

/* Max asteroid tier mineable at each laser level. UI calls starter gear an
 * L1 laser; internally that is mining_level 0.
 *   L1/level 0: M
 *   L2/level 1: L
 *   L3/level 2: XL
 *   L4+/level 3: XXL */
asteroid_tier_t max_mineable_tier(int mining_level) {
    /* Tier enum is inverted: TIER_XXL=0 (toughest) -> TIER_S=4 (softest). */
    switch (mining_level) {
        case 0: return ASTEROID_TIER_M;
        case 1: return ASTEROID_TIER_L;
        case 2: return ASTEROID_TIER_XL;
        default: return ASTEROID_TIER_XXL;
    }
}

int mining_required_level_for_commodity(commodity_t commodity) {
    switch (commodity) {
    case COMMODITY_CUPRITE_ORE: return 1; /* L2 laser */
    case COMMODITY_CRYSTAL_ORE: return 2; /* L3 laser */
    default: return 0;
    }
}

bool mining_level_can_fracture_asteroid(int mining_level, const asteroid_t *a) {
    if (!a || !a->active || asteroid_is_collectible(a)) return false;
    if (mining_level < mining_required_level_for_commodity(a->commodity))
        return false;
    return (int)a->tier >= (int)max_mineable_tier(mining_level);
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
        float surface_dist = proj - fixp_sqrtf(fmaxf(0.0f, a->radius * a->radius - perp * perp));
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

static int SIGNAL_MAYBE_UNUSED
manifest_count_market_buy_units(const manifest_t *manifest,
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

static int SIGNAL_MAYBE_UNUSED
manifest_find_market_buy_unit(const manifest_t *manifest,
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

static bool transfer_station_unit_to_ship(station_t *src, ship_t *dst, uint16_t idx) {
    cargo_unit_t unit = {0};
    cargo_receipt_chain_t chain = {0};

    if (!src || !dst) return false;
    if (!station_manifest_remove_with_chain(src, idx, &unit, &chain)) return false;
    if (!ship_manifest_push_with_chain(dst, &unit, &chain)) {
        (void)station_manifest_push_with_chain(src, &unit, &chain);
        return false;
    }
    return true;
}

static int SIGNAL_MAYBE_UNUSED
transfer_station_to_ship_by_commodity_ex(station_t *src, ship_t *dst,
                                         commodity_t commodity,
                                         mining_grade_t preferred_grade,
                                         int n) {
    if (!src || !dst || n <= 0) return 0;
    if (!station_manifest_bootstrap(src) || !ship_manifest_bootstrap(dst)) return 0;
    int moved = 0;
    bool allow_any_grade = (preferred_grade >= MINING_GRADE_COUNT);
    while (moved < n) {
        int idx = -1;
        if (!allow_any_grade)
            idx = manifest_find_first_cg(&src->manifest, commodity, preferred_grade);
        if (idx < 0) {
            for (uint16_t i = 0; i < src->manifest.count; i++) {
                if (src->manifest.units[i].commodity == (uint8_t)commodity) {
                    idx = (int)i;
                    break;
                }
            }
        }
        if (idx < 0) break;
        if (!transfer_station_unit_to_ship(src, dst, (uint16_t)idx)) break;
        moved++;
    }
    return moved;
}

static bool station_take_pod_shell_frame(station_t *st,
                                         cargo_unit_t *unit,
                                         cargo_receipt_chain_t *chain) {
    if (!st || !unit || !chain) return false;
    if (!station_manifest_bootstrap(st)) return false;
    for (uint16_t i = 0; i < st->manifest.count; i++) {
        if (st->manifest.units[i].commodity != (uint8_t)COMMODITY_FRAME)
            continue;
        if (!station_manifest_remove_with_chain(st, i, unit, chain))
            return false;
        sync_station_finished_inventory(st, COMMODITY_FRAME);
        return true;
    }
    return false;
}

static void station_restore_pod_shell_frame(station_t *st,
                                            const cargo_unit_t *unit,
                                            const cargo_receipt_chain_t *chain) {
    if (!st || !unit || !chain) return;
    if ((commodity_t)unit->commodity != COMMODITY_FRAME) return;
    (void)station_manifest_push_with_chain(st, unit, chain);
    sync_station_finished_inventory(st, COMMODITY_FRAME);
}

static const cargo_receipt_chain_t *ship_receipt_chain_at(
    const ship_t *ship, uint16_t index) {
    const ship_receipts_t *receipts = ship_get_receipts_const(ship);
    if (!receipts || !receipts->chains || index >= receipts->count)
        return NULL;
    return &receipts->chains[index];
}

static cargo_legality_result_t classify_ship_manifest_unit_at_station(
    const world_t *w, const ship_t *ship, uint16_t index, int station_index) {
    cargo_legality_result_t result = {
        .status = CARGO_LEGALITY_SUSPICIOUS,
        .origin_station = -1,
        .black_market_station = -1,
    };
    if (!w || !ship || !ship->manifest.units ||
        index >= ship->manifest.count ||
        station_index < 0 || station_index >= MAX_STATIONS) {
        result.reasons = CARGO_LEGALITY_REASON_UNKNOWN_AUTHORITY;
        return result;
    }
    return cargo_legality_classify(
        w->stations, MAX_STATIONS, station_index, &ship->manifest.units[index],
        ship_receipt_chain_at(ship, index));
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

static void sync_station_finished_inventory(station_t *st, commodity_t c) {
    if (!st || !is_finished_good(c)) return;
    st->_inventory_cache[c] =
        (float)manifest_count_by_commodity(&st->manifest, c) +
        station_finished_fraction(st, c);
    st->manifest_dirty = true;
}

/* ================================================================== */
/* Delivery credit shipments                                          */
/* ================================================================== */

static const float DELIVERY_ORIGIN_CREDIT_RATE = 0.10f;
static const uint32_t DELIVERY_DUE_TICKS = 120u * 60u * 8u; /* 8 minutes */

static void player_ledger_earn_at(server_player_t *sp, station_t *st,
                                  float amount) {
    if (!sp || !st || amount <= 0.0f) return;
    if (server_player_can_use_pubkey_persistence(sp))
        ledger_earn_by_pubkey(st, sp->pubkey, amount);
    else
        ledger_earn(st, sp->session_token, amount);
    sp->ship.stat_credits_earned += amount;
}

static void player_ledger_force_debit_at(server_player_t *sp, station_t *st,
                                         float amount) {
    if (!sp || !st || amount <= 0.0f) return;
    if (server_player_can_use_pubkey_persistence(sp))
        ledger_force_debit_by_pubkey(st, sp->pubkey, amount, &sp->ship);
    else
        ledger_force_debit(st, sp->session_token, amount, &sp->ship);
}

static bool delivery_shipment_pod_active(const world_t *w,
                                         const delivery_shipment_t *shipment) {
    if (!w || !shipment || shipment->shipment_id == 0) return false;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id == shipment->shipment_id) return true;
    }
    return false;
}

static int delivery_towed_pod_slot_for_shipment(const world_t *w,
                                                const server_player_t *sp,
                                                const delivery_shipment_t *shipment) {
    if (!w || !sp || !shipment || shipment->shipment_id == 0) return -1;
    for (int t = 0; t < sp->ship.towed_pod_count && t < 10; t++) {
        int idx = sp->ship.towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        const cargo_pod_t *pod = &w->cargo_pods[idx];
        if (!pod->active || pod->kind != CARGO_POD_CARGO) continue;
        if (pod->shipment_id != shipment->shipment_id) continue;
        if (pod->commodity != (commodity_t)shipment->commodity) continue;
        if (pod->towed_by >= 0 && pod->towed_by != sp->id) continue;
        return t;
    }
    return -1;
}

static bool delivery_materialize_pod_manifest(cargo_pod_t *pod,
                                              const delivery_shipment_t *shipment,
                                              uint16_t cargo_offset,
                                              uint16_t expected_quantity) {
    if (!pod || !shipment || expected_quantity == 0 ||
        expected_quantity > CARGO_POD_MANIFEST_CAP ||
        cargo_offset > shipment->quantity_bound ||
        cargo_offset + expected_quantity > shipment->quantity_bound ||
        cargo_offset + expected_quantity > MAX_DELIVERY_BOUND_CARGO) {
        return false;
    }
    if (!pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->commodity != (commodity_t)shipment->commodity ||
        pod->quantity != expected_quantity ||
        pod->shipment_id != shipment->shipment_id) {
        return false;
    }
    if (pod->manifest_count > 0) {
        if (pod->manifest_count != expected_quantity) return false;
        for (uint16_t i = 0; i < expected_quantity; i++) {
            const cargo_unit_t *want =
                &shipment->cargo_units[cargo_offset + i];
            const cargo_unit_t *have = &pod->manifest_units[i];
            if ((commodity_t)have->commodity != (commodity_t)shipment->commodity ||
                memcmp(have->pub, want->pub, sizeof(have->pub)) != 0) {
                return false;
            }
        }
        return true;
    }

    memset(pod->manifest_units, 0, sizeof(pod->manifest_units));
    for (uint16_t i = 0; i < expected_quantity; i++) {
        const cargo_unit_t *unit = &shipment->cargo_units[cargo_offset + i];
        if ((commodity_t)unit->commodity != (commodity_t)shipment->commodity)
            return false;
        pod->manifest_units[i] = *unit;
    }
    pod->manifest_count = expected_quantity;
    pod->quantity = expected_quantity;
    return true;
}

static bool delivery_transfer_towed_pod_to_station_custody(
                                       world_t *w,
                                       server_player_t *sp,
                                       delivery_shipment_t *shipment,
                                       uint16_t cargo_offset,
                                       uint16_t expected_quantity,
                                       int station_idx) {
    int tow_slot = delivery_towed_pod_slot_for_shipment(w, sp, shipment);
    if (!w || !sp || !shipment || tow_slot < 0 ||
        station_idx < 0 || station_idx >= MAX_STATIONS ||
        station_first_dock_module(&w->stations[station_idx]) < 0) {
        return false;
    }
    int pod_idx = sp->ship.towed_pods[tow_slot];
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!delivery_materialize_pod_manifest(pod, shipment, cargo_offset,
                                           expected_quantity)) {
        return false;
    }
    pod->shipment_id = 0;
    remove_towed_pod_slot(&sp->ship, tow_slot);
    return cargo_pod_set_station_dock_custody(w, pod_idx, station_idx);
}

static uint64_t delivery_receipt_nonce(const delivery_shipment_t *shipment) {
    if (!shipment) return 0;
    return (uint64_t)shipment->shipment_id
         | ((uint64_t)shipment->origin_station << 16)
         | ((uint64_t)shipment->destination_station << 24)
         | ((uint64_t)shipment->commodity << 32);
}

static void delivery_receipt_anchor(const delivery_shipment_t *shipment,
                                    uint8_t out[32]) {
    if (!out) return;
    uint8_t buf[16 + MAX_DELIVERY_BOUND_CARGO * 32] = {0};
    if (!shipment) {
        sha256_bytes(buf, sizeof(buf), out);
        return;
    }
    buf[0] = (uint8_t)(shipment->shipment_id & 0xffu);
    buf[1] = (uint8_t)((shipment->shipment_id >> 8) & 0xffu);
    buf[2] = shipment->origin_station;
    buf[3] = shipment->destination_station;
    buf[4] = shipment->contract_index;
    buf[5] = shipment->commodity;
    buf[6] = (uint8_t)(shipment->quantity_total & 0xffu);
    buf[7] = (uint8_t)((shipment->quantity_total >> 8) & 0xffu);
    buf[8] = (uint8_t)(shipment->quantity_delivered & 0xffu);
    buf[9] = (uint8_t)((shipment->quantity_delivered >> 8) & 0xffu);
    uint16_t n = shipment->quantity_bound < MAX_DELIVERY_BOUND_CARGO
        ? shipment->quantity_bound
        : MAX_DELIVERY_BOUND_CARGO;
    for (uint16_t i = 0; i < n; i++)
        memcpy(&buf[16 + i * 32], shipment->cargo_pub[i], 32);
    sha256_bytes(buf, sizeof(buf), out);
}

static void delivery_emit_receipt_memory(world_t *w,
                                         server_player_t *sp,
                                         station_t *dest,
                                         const delivery_shipment_t *shipment,
                                         float payout) {
    if (!w || !sp || !dest || !shipment) return;
    if (shipment->quantity_delivered == 0) return;
    if (shipment->origin_station < MAX_STATIONS) {
        station_t *origin = &w->stations[shipment->origin_station];
        if (origin != dest && station_exists(origin)) {
            station_faction_adjust_relation_to(
                dest, origin->faction_id, 1);
            station_faction_adjust_relation_to(
                origin, dest->faction_id, 1);
        }
    }
    market_memory_t memory = {0};
    if (!market_memory_from_delivery_receipt(
            shipment->origin_station,
            shipment->destination_station,
            (commodity_t)shipment->commodity,
            shipment->quantity_delivered,
            payout,
            w->tick,
            delivery_receipt_nonce(shipment),
            &memory)) {
        return;
    }

    knowledge_item_t item;
    if (!knowledge_item_from_market_memory(&memory, &item)) return;
    delivery_receipt_anchor(shipment, item.chain_anchor);

    knowledge_view_configure(&dest->knowledge, STATION_KNOWN_ITEM_CAP);
    knowledge_view_insert(&dest->knowledge, &item);
    market_memory_t reputation = {0};
    if (market_memory_from_route_reputation(
            shipment->origin_station,
            shipment->destination_station,
            (commodity_t)shipment->commodity,
            shipment->quantity_delivered,
            payout,
            w->tick,
            false,
            &reputation)) {
        knowledge_view_reinforce_route_reputation(&dest->knowledge, &reputation);
    }
    market_memory_t trust = {0};
    if (market_memory_from_station_trust(
            shipment->destination_station,
            (uint8_t)CONTRACT_DELIVERY,
            (commodity_t)shipment->commodity,
            shipment->quantity_delivered,
            payout,
            w->tick,
            &trust)) {
        knowledge_view_reinforce_station_trust(&dest->knowledge, &trust);
    }
    knowledge_view_forget_contract(&dest->knowledge,
                                   (uint8_t)CONTRACT_DELIVERY,
                                   shipment->destination_station,
                                   (commodity_t)shipment->commodity);

    knowledge_view_configure(&sp->ship.knowledge, SHIP_KNOWN_ITEM_CAP);
    knowledge_view_insert(&sp->ship.knowledge, &item);
    if (reputation.active)
        knowledge_view_reinforce_route_reputation(&sp->ship.knowledge, &reputation);
    if (trust.active)
        knowledge_view_reinforce_station_trust(&sp->ship.knowledge, &trust);
    knowledge_view_forget_contract(&sp->ship.knowledge,
                                   (uint8_t)CONTRACT_DELIVERY,
                                   shipment->destination_station,
                                   (commodity_t)shipment->commodity);
}

static void delivery_emit_default_memory(world_t *w,
                                         server_player_t *sp,
                                         station_t *observer,
                                         const delivery_shipment_t *shipment,
                                         float value_hint) {
    if (!w || !shipment) return;
    if (shipment->destination_station >= MAX_STATIONS) return;
    commodity_t commodity = (commodity_t)shipment->commodity;
    if (commodity >= COMMODITY_COUNT) return;

    market_memory_t risk = {0};
    if (!market_memory_from_station_risk(
            shipment->destination_station,
            (uint8_t)CONTRACT_DELIVERY,
            commodity,
            1,
            value_hint,
            w->tick,
            &risk)) {
        return;
    }

    station_t *dest = &w->stations[shipment->destination_station];
    if (station_exists(dest)) {
        knowledge_view_configure(&dest->knowledge, STATION_KNOWN_ITEM_CAP);
        knowledge_view_reinforce_station_trust(&dest->knowledge, &risk);
        knowledge_view_forget_contract(&dest->knowledge,
                                       (uint8_t)CONTRACT_DELIVERY,
                                       shipment->destination_station,
                                       commodity);
    }
    if (observer && observer != dest && station_exists(observer) &&
        station_exists(dest)) {
        if (station_faction_is_pirate_economy(observer)) {
            station_faction_adjust_relation_to(
                dest, observer->faction_id, -5);
            station_faction_adjust_relation_to(
                observer, dest->faction_id, -2);
        }
        knowledge_view_configure(&observer->knowledge, STATION_KNOWN_ITEM_CAP);
        knowledge_view_reinforce_station_trust(&observer->knowledge, &risk);
        knowledge_view_forget_contract(&observer->knowledge,
                                       (uint8_t)CONTRACT_DELIVERY,
                                       shipment->destination_station,
                                       commodity);
    }
    if (sp) {
        knowledge_view_configure(&sp->ship.knowledge, SHIP_KNOWN_ITEM_CAP);
        knowledge_view_reinforce_station_trust(&sp->ship.knowledge, &risk);
        knowledge_view_forget_contract(&sp->ship.knowledge,
                                       (uint8_t)CONTRACT_DELIVERY,
                                       shipment->destination_station,
                                       commodity);
    }
}

static delivery_shipment_t *delivery_active_for_contract(world_t *w,
                                                         int player_id,
                                                         int contract_index) {
    if (!w || player_id < 0 || contract_index < 0) return NULL;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (!shipment->active) continue;
        if (shipment->debtor_player != (uint8_t)player_id) continue;
        if (shipment->contract_index != (uint8_t)contract_index) continue;
        if (shipment->status == DELIVERY_SHIPMENT_CLEARED ||
            shipment->status == DELIVERY_SHIPMENT_BLACK_MARKET_SOLD) {
            continue;
        }
        return shipment;
    }
    return NULL;
}

static delivery_shipment_t *delivery_alloc_shipment(world_t *w) {
    if (!w) return NULL;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (shipment->active &&
            shipment->status != DELIVERY_SHIPMENT_CLEARED &&
            shipment->status != DELIVERY_SHIPMENT_DEFAULTED &&
            shipment->status != DELIVERY_SHIPMENT_BLACK_MARKET_SOLD) {
            continue;
        }
        memset(shipment, 0, sizeof(*shipment));
        shipment->active = true;
        if (w->next_delivery_shipment_id == 0)
            w->next_delivery_shipment_id = 1;
        shipment->shipment_id = w->next_delivery_shipment_id++;
        if (w->next_delivery_shipment_id == 0)
            w->next_delivery_shipment_id = 1;
        return shipment;
    }
    return NULL;
}

static bool delivery_contract_has_source(const contract_t *ct) {
    return ct && ct->action == CONTRACT_DELIVERY &&
           ct->target_index >= 0 &&
           ct->target_index < MAX_STATIONS;
}

static int delivery_find_source_stock_unit(const station_t *origin,
                                           const contract_t *ct) {
    if (!origin || !ct || !origin->manifest.units) return -1;
    for (uint16_t i = 0; i < origin->manifest.count; i++) {
        const cargo_unit_t *unit = &origin->manifest.units[i];
        if (!contract_fit_is_ok(contract_fit_cargo_unit(ct, unit))) continue;
        if (ct->proof_flags == 0 &&
            (cargo_kind_t)unit->kind == CARGO_KIND_INGOT &&
            (ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS) {
            continue;
        }
        return (int)i;
    }
    return -1;
}

static int delivery_source_stock_count(const station_t *origin,
                                       const contract_t *ct) {
    if (!origin || !ct || !origin->manifest.units) return 0;
    int count = 0;
    for (uint16_t i = 0; i < origin->manifest.count; i++) {
        const cargo_unit_t *unit = &origin->manifest.units[i];
        if (!contract_fit_is_ok(contract_fit_cargo_unit(ct, unit))) continue;
        if (ct->proof_flags == 0 &&
            (cargo_kind_t)unit->kind == CARGO_KIND_INGOT &&
            (ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS) {
            continue;
        }
        count++;
    }
    return count;
}

static void delivery_restore_shipment_to_origin(station_t *origin,
                                                delivery_shipment_t *shipment,
                                                int count) {
    if (!origin || !shipment || count <= 0) return;
    if (count > MAX_DELIVERY_BOUND_CARGO) count = MAX_DELIVERY_BOUND_CARGO;
    for (int i = 0; i < count; i++) {
        if (shipment->cargo_units[i].commodity >= COMMODITY_COUNT) continue;
        (void)station_manifest_push_with_chain(origin,
                                               &shipment->cargo_units[i],
                                               &shipment->cargo_chains[i]);
    }
}

static int delivery_pickup_from_origin(world_t *w, server_player_t *sp,
                                       contract_t *ct,
                                       int contract_index) {
    if (!w || !sp || !ct || !delivery_contract_has_source(ct)) return 0;
    int origin_idx = ct->target_index;
    if (sp->current_station != origin_idx) return 0;
    if (delivery_active_for_contract(w, sp->id, contract_index)) return 0;
    station_t *origin = &w->stations[origin_idx];
    if (!station_exists(origin)) return 0;
    if (!station_manifest_bootstrap(origin)) {
        return 0;
    }
    int max_tow = 2 + sp->ship.tractor_level * 2;
    if (sp->ship.towed_pod_count >= max_tow) return 0;
    int stock = delivery_source_stock_count(origin, ct);
    if (stock <= 0) return 0;
    int needed = (int)ceilf(ct->quantity_needed);
    if (needed <= 0) needed = 1;
    int take = needed;
    if (take > stock) take = stock;
    if (take > MAX_DELIVERY_BOUND_CARGO) take = MAX_DELIVERY_BOUND_CARGO;
    if (take <= 0) return 0;
    cargo_unit_t shell_frame = {0};
    cargo_receipt_chain_t shell_chain = {0};
    if (!station_take_pod_shell_frame(origin, &shell_frame, &shell_chain))
        return 0;

    delivery_shipment_t scratch = {0};
    scratch.active = true;
    scratch.origin_station = (uint8_t)origin_idx;
    scratch.destination_station = ct->station_index;
    scratch.contract_index = (uint8_t)contract_index;
    scratch.debtor_player = (uint8_t)sp->id;
    scratch.commodity = (uint8_t)ct->commodity;
    scratch.status = DELIVERY_SHIPMENT_PICKED_UP;
    scratch.due_tick = w->tick + DELIVERY_DUE_TICKS;

    int moved = 0;
    float debt = 0.0f;
    while (moved < take) {
        int idx = delivery_find_source_stock_unit(origin, ct);
        if (idx < 0) break;
        cargo_unit_t unit = {0};
        cargo_receipt_chain_t chain = {0};
        if (!station_manifest_remove_with_chain(origin, (uint16_t)idx,
                                                &unit, &chain)) {
            break;
        }
        memcpy(scratch.cargo_pub[moved], unit.pub, 32);
        scratch.cargo_units[moved] = unit;
        scratch.cargo_chains[moved] = chain;
        moved++;
        float unit_debt = station_sell_price(origin, ct->commodity);
        if (unit_debt <= 0.0f)
            unit_debt = station_buy_price(origin, ct->commodity);
        debt += unit_debt;
    }
    if (moved <= 0) {
        station_restore_pod_shell_frame(origin, &shell_frame, &shell_chain);
        return 0;
    }

    delivery_shipment_t *shipment = delivery_alloc_shipment(w);
    if (!shipment) {
        station_restore_pod_shell_frame(origin, &shell_frame, &shell_chain);
        delivery_restore_shipment_to_origin(origin, &scratch, moved);
        sync_station_finished_inventory(origin, ct->commodity);
        return 0;
    }
    uint16_t id = shipment->shipment_id;
    *shipment = scratch;
    shipment->active = true;
    shipment->shipment_id = id;
    shipment->quantity_total = (uint16_t)moved;
    shipment->quantity_bound = (uint16_t)moved;
    shipment->debt_principal = debt;
    shipment->destination_payout = contract_price(ct) * (float)moved;
    shipment->origin_completion_credit = debt * DELIVERY_ORIGIN_CREDIT_RATE;
    vec2 pod_dir = v2_from_angle(sp->ship.angle + PI_F);
    vec2 pod_pos = v2_add(sp->ship.pos, v2_scale(pod_dir, 46.0f));
    int pod_idx = spawn_cargo_pod_with_manifest(
        w, pod_pos, sp->ship.vel, ct->commodity,
        scratch.cargo_units, (uint16_t)moved, CARGO_POD_CARGO);
    if (pod_idx < 0 || sp->ship.towed_pod_count >= max_tow) {
        station_restore_pod_shell_frame(origin, &shell_frame, &shell_chain);
        delivery_restore_shipment_to_origin(origin, shipment, moved);
        sync_station_finished_inventory(origin, ct->commodity);
        memset(shipment, 0, sizeof(*shipment));
        return 0;
    }
    w->cargo_pods[pod_idx].shipment_id = shipment->shipment_id;
    cargo_pod_set_shell_frame(&w->cargo_pods[pod_idx], &shell_frame);
    w->cargo_pods[pod_idx].towed_by = (int8_t)sp->id;
    sp->ship.towed_pods[sp->ship.towed_pod_count++] = (int16_t)pod_idx;
    player_ledger_force_debit_at(sp, origin, debt);
    ct->claimed_by = (int8_t)sp->id;
    sync_station_finished_inventory(origin, ct->commodity);
    sync_station_finished_inventory(origin, COMMODITY_FRAME);
    SIM_LOG("[delivery] player %d took shipment %u: %d %s %s -> %s debt %.0f\n",
            sp->id, shipment->shipment_id, moved,
            commodity_short_name(ct->commodity),
            origin->name, w->stations[ct->station_index].name, debt);
    return moved;
}

static float delivery_try_deliver_bound_cargo(world_t *w,
                                              server_player_t *sp,
                                              station_t *st,
                                              commodity_t filter) {
    if (!w || !sp || !st) return 0.0f;
    float payout = 0.0f;
    int station_idx = sp->current_station;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (!shipment->active) continue;
        if (shipment->debtor_player != (uint8_t)sp->id) continue;
        if (shipment->destination_station != (uint8_t)station_idx) continue;
        if (shipment->status != DELIVERY_SHIPMENT_PICKED_UP) continue;
        commodity_t c = (commodity_t)shipment->commodity;
        if (filter < COMMODITY_COUNT && filter != c) continue;
        uint16_t remaining = shipment->quantity_total > shipment->quantity_delivered
            ? (uint16_t)(shipment->quantity_total - shipment->quantity_delivered)
            : 0;
        if (remaining == 0) continue;
        int tow_slot = delivery_towed_pod_slot_for_shipment(w, sp, shipment);
        if (tow_slot < 0) continue;
        int pod_idx = sp->ship.towed_pods[tow_slot];
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) continue;
        cargo_pod_t *pod = &w->cargo_pods[pod_idx];
        if (pod->quantity != remaining) continue;

        float shipment_payout = 0.0f;
        bool ok = true;
        for (uint16_t i = 0; i < remaining; i++) {
            uint16_t cargo_idx =
                (uint16_t)(shipment->quantity_delivered + i);
            if (cargo_idx >= shipment->quantity_bound ||
                cargo_idx >= MAX_DELIVERY_BOUND_CARGO) {
                ok = false;
                break;
            }
            cargo_unit_t unit = shipment->cargo_units[cargo_idx];
            if (unit.commodity != (uint8_t)c) {
                ok = false;
                break;
            }
            float unit_pay = shipment->quantity_total > 0
                ? shipment->destination_payout / (float)shipment->quantity_total
                : 0.0f;
            shipment_payout += unit_pay;
        }
        if (!ok ||
            !delivery_transfer_towed_pod_to_station_custody(
                w, sp, shipment, shipment->quantity_delivered,
                remaining, station_idx)) {
            continue;
        }
        shipment->quantity_delivered =
            (uint16_t)(shipment->quantity_delivered + remaining);
        payout += shipment_payout;
        if (shipment->quantity_delivered >= shipment->quantity_total) {
            shipment->status = DELIVERY_SHIPMENT_DELIVERED;
            int ci = shipment->contract_index;
            if (ci >= 0 && ci < MAX_CONTRACTS &&
                w->contracts[ci].active &&
                w->contracts[ci].action == CONTRACT_DELIVERY) {
                w->contracts[ci].quantity_needed = 0.0f;
            }
            delivery_emit_receipt_memory(w, sp, st, shipment, shipment_payout);
            emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE,
                .player_id = sp->id,
                .contract_complete.action = CONTRACT_DELIVERY});
        }
    }
    if (payout > 0.01f) {
        player_ledger_earn_at(sp, st, payout);
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_SELL, .player_id = sp->id,
            .sell = { .station = sp->current_station,
                      .grade = (uint8_t)MINING_GRADE_COMMON,
                      .base_cr = (int)lroundf(payout),
                      .bonus_cr = 0,
                      .by_contract = 1u }});
    }
    return payout;
}

static float delivery_try_black_market_sell(world_t *w, server_player_t *sp,
                                            station_t *st,
                                            commodity_t filter,
                                            mining_grade_t grade,
                                            bool single_unit) {
    if (!w || !sp || !st) return 0.0f;
    (void)grade;
    station_policy_refresh(st, sp->current_station, w->tick);
    if (!station_policy_accepts_contract_bound_cargo(st)) return 0.0f;

    float payout = 0.0f;
    bool sold_any = false;
    bool sold_bound_any = false;
    for (int si = 0; si < MAX_DELIVERY_SHIPMENTS; si++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[si];
        if (!shipment->active) continue;
        if (shipment->debtor_player != (uint8_t)sp->id) continue;
        if (shipment->status != DELIVERY_SHIPMENT_PICKED_UP) continue;
        commodity_t c = (commodity_t)shipment->commodity;
        if (filter < COMMODITY_COUNT && filter != c) continue;
        uint16_t remaining =
            shipment->quantity_total >
                shipment->quantity_delivered + shipment->quantity_black_market_sold
                ? (uint16_t)(shipment->quantity_total -
                             shipment->quantity_delivered -
                             shipment->quantity_black_market_sold)
                : 0;
        if (remaining == 0) continue;
        int tow_slot = delivery_towed_pod_slot_for_shipment(w, sp, shipment);
        if (tow_slot < 0) continue;
        int pod_idx = sp->ship.towed_pods[tow_slot];
        if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) continue;
        cargo_pod_t *pod = &w->cargo_pods[pod_idx];
        if (pod->quantity != remaining) continue;

        float shipment_payout = 0.0f;
        bool ok = true;
        uint16_t cargo_offset =
            (uint16_t)(shipment->quantity_delivered +
                       shipment->quantity_black_market_sold);
        for (uint16_t i = 0; i < remaining; i++) {
            uint16_t cargo_idx = (uint16_t)(shipment->quantity_delivered +
                                            shipment->quantity_black_market_sold +
                                            i);
            if (cargo_idx >= shipment->quantity_bound ||
                cargo_idx >= MAX_DELIVERY_BOUND_CARGO) {
                ok = false;
                break;
            }
            cargo_unit_t unit = shipment->cargo_units[cargo_idx];
            float unit_price = station_buy_price_unit(st, &unit);
            if (unit_price <= 0.0f)
                unit_price = station_buy_price(st, c);
            shipment_payout += unit_price * BLACK_MARKET_CARGO_MARKDOWN;
        }
        if (!ok ||
            !delivery_transfer_towed_pod_to_station_custody(
                w, sp, shipment, cargo_offset, remaining,
                sp->current_station)) {
            continue;
        }
        payout += shipment_payout;
        shipment->quantity_black_market_sold =
            (uint16_t)(shipment->quantity_black_market_sold + remaining);
        sold_any = true;
        sold_bound_any = true;
        if (shipment->quantity_black_market_sold +
                shipment->quantity_delivered >=
            shipment->quantity_total) {
            delivery_emit_default_memory(w, sp, st, shipment,
                                         shipment->destination_payout);
            shipment->status = DELIVERY_SHIPMENT_BLACK_MARKET_SOLD;
            int ci = shipment->contract_index;
            if (ci >= 0 && ci < MAX_CONTRACTS &&
                w->contracts[ci].active &&
                w->contracts[ci].action == CONTRACT_DELIVERY) {
                w->contracts[ci].active = false;
            }
        }
        if (single_unit && sold_bound_any) break;
    }
    if (payout > 0.01f) {
        player_ledger_earn_at(sp, st, payout);
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_SELL, .player_id = sp->id,
            .sell = { .station = sp->current_station,
                      .grade = (uint8_t)MINING_GRADE_COMMON,
                      .base_cr = (int)lroundf(payout),
                      .bonus_cr = 0,
                      .by_contract = sold_bound_any ? 1u : 0u }});
    }
    if (sold_any) {
        SIM_LOG("[delivery] player %d black-market sold cargo for %.0f at %s\n",
                sp->id, payout, st->name);
    }
    return payout;
}

static void delivery_clear_origin_proofs(world_t *w, server_player_t *sp,
                                         int station_idx) {
    if (!w || !sp || station_idx < 0 || station_idx >= MAX_STATIONS) return;
    station_t *origin = &w->stations[station_idx];
    if (!station_exists(origin)) return;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (!shipment->active) continue;
        if (shipment->debtor_player != (uint8_t)sp->id) continue;
        if (shipment->origin_station != (uint8_t)station_idx) continue;
        if (shipment->status != DELIVERY_SHIPMENT_DELIVERED) continue;
        float credit = shipment->debt_principal +
                       shipment->origin_completion_credit;
        player_ledger_earn_at(sp, origin, credit);
        shipment->status = DELIVERY_SHIPMENT_CLEARED;
        int ci = shipment->contract_index;
        if (ci >= 0 && ci < MAX_CONTRACTS &&
            w->contracts[ci].active &&
            w->contracts[ci].action == CONTRACT_DELIVERY) {
            w->contracts[ci].active = false;
        }
        SIM_LOG("[delivery] player %d cleared shipment %u at %s for %.0f\n",
                sp->id, shipment->shipment_id, origin->name, credit);
    }
}

static void step_delivery_shipments(world_t *w) {
    if (!w) return;
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        delivery_shipment_t *shipment = &w->delivery_shipments[i];
        if (!shipment->active) continue;
        if (shipment->status != DELIVERY_SHIPMENT_PICKED_UP) continue;
        if (shipment->due_tick != 0 && w->tick > shipment->due_tick) {
            int pid = shipment->debtor_player;
            server_player_t *sp =
                (pid >= 0 && pid < MAX_PLAYERS && w->players[pid].connected)
                    ? &w->players[pid] : NULL;
            delivery_emit_default_memory(w, sp, NULL, shipment,
                                         shipment->destination_payout);
            shipment->status = DELIVERY_SHIPMENT_DEFAULTED;
            int ci = shipment->contract_index;
            if (ci >= 0 && ci < MAX_CONTRACTS &&
                w->contracts[ci].active &&
                w->contracts[ci].action == CONTRACT_DELIVERY) {
                w->contracts[ci].active = false;
            }
            continue;
        }
        int pid = shipment->debtor_player;
        if (pid < 0 || pid >= MAX_PLAYERS || !w->players[pid].connected)
            continue;
        if (!delivery_shipment_pod_active(w, shipment) &&
            shipment->quantity_delivered == 0) {
            delivery_emit_default_memory(w, &w->players[pid], NULL, shipment,
                                         shipment->destination_payout);
            shipment->status = DELIVERY_SHIPMENT_DEFAULTED;
            int ci = shipment->contract_index;
            if (ci >= 0 && ci < MAX_CONTRACTS &&
                w->contracts[ci].active &&
                w->contracts[ci].action == CONTRACT_DELIVERY) {
                w->contracts[ci].active = false;
            }
        }
    }
}

static float try_deliver_towed_fragments_to_contracts(world_t *w,
                                                      server_player_t *sp,
                                                      station_t *st,
                                                      commodity_t filter) {
    if (!w || !sp || !st) return 0.0f;
    if (sp->current_station < 0 || sp->current_station >= MAX_STATIONS)
        return 0.0f;

    float payout = 0.0f;
    for (int t = sp->ship.towed_count - 1; t >= 0; t--) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) {
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] =
                sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
            continue;
        }

        asteroid_t *a = &w->asteroids[idx];
        if (a->commodity >= COMMODITY_RAW_ORE_COUNT) continue;
        if (filter != COMMODITY_COUNT && filter != a->commodity) continue;

        int best_contract = -1;
        float best_price = 0.0f;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            contract_t *ct = &w->contracts[k];
            if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
            if (ct->station_index != sp->current_station) continue;
            if (!contract_fit_is_ok(contract_fit_fragment(ct, a))) continue;
            float price = contract_price(ct);
            if (best_contract < 0 || price > best_price) {
                best_contract = k;
                best_price = price;
            }
        }
        if (best_contract < 0) continue;

        contract_t *ct = &w->contracts[best_contract];
        mining_grade_t grade = (a->grade < (uint8_t)MINING_GRADE_COUNT)
            ? (mining_grade_t)a->grade
            : MINING_GRADE_COMMON;
        float ore_units = a->ore;
        if (ore_units <= 0.0f) continue;

        st->_inventory_cache[a->commodity] += ore_units;
        if (st->_inventory_cache[a->commodity] > REFINERY_HOPPER_CAPACITY)
            st->_inventory_cache[a->commodity] = REFINERY_HOPPER_CAPACITY;

        float ore_value = ore_units * best_price;
        float graded_value = ore_value * mining_payout_multiplier(grade);
        float credited = 0.0f;
        if (server_player_can_use_pubkey_persistence(sp)) {
            credited = ledger_credit_supply_amount_by_pubkey(st, sp->pubkey,
                                                             graded_value);
            ledger_record_ore_sold(st, sp->pubkey, (uint32_t)lroundf(ore_units),
                                   (uint8_t)a->commodity);
        } else {
            credited = ledger_credit_supply_amount(st, sp->session_token,
                                                   graded_value);
        }
        sp->ship.stat_credits_earned += credited;
        payout += credited;

        ct->quantity_needed -= ore_units;
        if (ct->quantity_needed <= 0.01f) {
            ct->active = false;
            emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE,
                .contract_complete.action = CONTRACT_TRACTOR});
        }

        sp->ship.towed_count--;
        sp->ship.towed_fragments[t] =
            sp->ship.towed_fragments[sp->ship.towed_count];
        sp->ship.towed_fragments[sp->ship.towed_count] = -1;

        SIM_LOG("[sim] player %d loaded towed %s fragment (%.0f ore) for %.0f cr at %s\n",
                sp->id, commodity_short_name(a->commodity), ore_units,
                credited, st->name);
        emit_event(w, (sim_event_t){
            .type = SIM_EVENT_SELL, .player_id = sp->id,
            .sell = { .station = sp->current_station,
                      .grade = (uint8_t)grade,
                      .base_cr = (int)lroundf(ore_value),
                      .bonus_cr = (int)lroundf(graded_value - ore_value),
                      .by_contract = 1 }});

        memset(a, 0, sizeof(*a));
        if (idx >= 0 && idx < MAX_ASTEROIDS)
            memset(&w->asteroid_origin[idx], 0, sizeof(w->asteroid_origin[idx]));
    }
    return payout;
}

static void try_sell_station_cargo(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];

    /* Pod economy: SELL is a physical handoff. This fallback only handles
     * physical things that are not ordinary market pods: destination-bound
     * freight pods, black-market freight pods, and towed ore fragments.
     * Legacy ship.manifest / ship.cargo units are intentionally not
     * transferred here anymore. */
    if (sp->input.service_sell_one) {
        commodity_t commodity = sp->input.service_sell_only;
        mining_grade_t grade  = sp->input.service_sell_grade;
        float delivery_payout =
            delivery_try_deliver_bound_cargo(w, sp, st, commodity);
        if (delivery_payout <= 0.01f)
            (void)delivery_try_black_market_sell(
                w, sp, st, commodity, grade, true);
        sp->input.service_sell_only = COMMODITY_COUNT;
        sp->input.service_sell_grade = MINING_GRADE_COUNT;
        sp->input.service_sell_one = false;
        return;
    }

    /* Optional one-shot filter: if the client requested selective
     * delivery via NET_ACTION_DELIVER_COMMODITY, only commodities
     * matching `filter` are delivered. COMMODITY_COUNT lets the dock
     * choose the next accepted target. Ordinary pod cargo is handled
     * before this path and sells one whole pod per press. */
    commodity_t filter = sp->input.service_sell_only;

    float fragment_payout = try_deliver_towed_fragments_to_contracts(
        w, sp, st, filter);
    (void)fragment_payout;

    float delivery_payout = delivery_try_deliver_bound_cargo(w, sp, st, filter);
    (void)delivery_payout;

    float black_market_payout = delivery_try_black_market_sell(
        w, sp, st, filter, MINING_GRADE_COUNT, false);
    (void)black_market_payout;

    /* Clear the one-shot filter so the next plain SELL_CARGO press
     * resumes the default dock-selected behavior. */
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
        if (server_player_can_use_pubkey_persistence(sp)) {
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
        bool can_afford = server_player_can_use_pubkey_persistence(sp) ?
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
    float ship_dist = v2_len(v2_sub(sp->ship.pos, st->pos));
    vec2 ship_delta = v2_sub(sp->ship.pos, st->pos);
    float ship_ang = fixp_atan2f(ship_delta.y, ship_delta.x);

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
    if (!w->player_only_mode && !sp->docked && ship_collision_count >= 3) {
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
        float dist = v2_len(v2_sub(target, sp->ship.pos));

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

static commodity_t autopilot_tow_collection_filter(const world_t *w,
                                                   const server_player_t *sp) {
    if (!w || !sp || sp->autopilot_mode == 0)
        return COMMODITY_COUNT;

    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &w->asteroids[idx];
        if (!a->active || a->commodity >= COMMODITY_RAW_ORE_COUNT) continue;
        return a->commodity;
    }

    if (sp->autopilot_state == AUTOPILOT_STEP_FLY_TO_TARGET ||
        sp->autopilot_state == AUTOPILOT_STEP_MINE ||
        sp->autopilot_state == AUTOPILOT_STEP_COLLECT) {
        int idx = sp->autopilot_target;
        if (idx >= 0 && idx < MAX_ASTEROIDS) {
            const asteroid_t *a = &w->asteroids[idx];
            if (a->active && a->commodity < COMMODITY_RAW_ORE_COUNT)
                return a->commodity;
        }
    }

    return COMMODITY_COUNT;
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
 * Constants live in sim_ship.h so player ships and NPC miner ships use
 * the same tow primitive. Tuned so:
 *   - 100 u stretch ≈ a noticeable tug (~3 HP/s of ship drag at full load)
 *   - 200 u stretch ≈ near-elastic-limit, hauling feels heavy
 *   - tractor_range * 1.5 ≈ snap-out (band breaks). */
static void apply_band_force(server_player_t *sp, asteroid_t *a, float dt) {
    ship_apply_fragment_tow(&sp->ship, a, dt);
}

static void resolve_towed_body_ship_overlap(const ship_t *ship, vec2 *pos,
                                            vec2 *vel, float body_radius,
                                            float clearance) {
    if (!ship || !pos || !vel) return;
    float ship_r = ship_hull_def(ship)->ship_radius;
    float min_d = body_radius + ship_r + clearance;
    vec2 ship_to_body = v2_sub(*pos, ship->pos);
    float ds = v2_len_sq(ship_to_body);
    if (ds >= min_d * min_d) return;

    float dd = 0.0f;
    vec2 n = ship_forward(ship->angle);
    if (ds > 0.1f) {
        dd = fixp_sqrtf(ds);
        n = v2_scale(ship_to_body, 1.0f / dd);
    }
    *pos = v2_add(*pos, v2_scale(n, min_d - dd));

    float closing = v2_dot(v2_sub(*vel, ship->vel), n);
    if (closing < 0.0f)
        *vel = v2_sub(*vel, v2_scale(n, closing));
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

        resolve_towed_body_ship_overlap(&sp->ship, &a->pos, &a->vel,
                                        a->radius, 4.0f);

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
                float abd = fixp_sqrtf(ab_sq);
                float overlap = (sep - abd) * 0.5f;
                vec2 n = v2_scale(ab, overlap / abd);
                a->pos = v2_sub(a->pos, n);
                b->pos = v2_add(b->pos, n);
                float closing = v2_dot(v2_sub(b->vel, a->vel), n);
                if (closing < 0.0f) {
                    vec2 impulse = v2_scale(n, -closing * 0.5f);
                    a->vel = v2_sub(a->vel, impulse);
                    b->vel = v2_add(b->vel, impulse);
                }
            }
        }
    }

    int max_tow = 2 + sp->ship.tractor_level * 2; /* 2/4/6/8/10 */
    commodity_t tow_filter = autopilot_tow_collection_filter(w, sp);
    /* Use nearby range (the larger of the two) for the broad check */
    float broad_sq = (nearby_sq > tr_sq) ? nearby_sq : tr_sq;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier != ASTEROID_TIER_S) continue;
        if (tow_filter != COMMODITY_COUNT && a->commodity != tow_filter) continue;
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
        resolve_towed_body_ship_overlap(&sp->ship, &a->pos, &a->vel,
                                        a->radius, 4.0f);
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
 * last_towed_token stays set for smelt provenance. thrown_by_token is the
 * short-lived combat owner used for damage and kill credit. */
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
            vec2 fwd = v2_from_angle(sp->ship.angle);
            a->vel = v2_add(sp->ship.vel, v2_scale(fwd, ROCK_THROW_BASE_SPEED));
            asteroid_mark_thrown(a, sp->session_token, ROCK_THROW_BALLISTIC_SECONDS);
            a->net_dirty = true;
            continue;
        }
        vec2 dir = v2_scale(to_ship, 1.0f / dist);
        /* Stretch beyond rest length — only the elastic portion counts
         * as stored energy. A rock at slack-distance gets just BASE. */
        float stretch = dist - SHIP_TOW_BAND_REST_LEN;
        if (stretch < 0.0f) stretch = 0.0f;
        /* v = sqrt(K) * stretch  is the elastic-energy fling. With
         * SHIP_TOW_BAND_SPRING_K = 4 and stretch = 200 (deep stretch),
         * this is ~400 m/s. Half-stretch (100) is ~200 m/s. */
        float elastic = fixp_sqrtf(SHIP_TOW_BAND_SPRING_K) * stretch;
        float fling = ROCK_THROW_BASE_SPEED + elastic;
        a->vel = v2_add(sp->ship.vel, v2_scale(dir, fling));
        asteroid_mark_thrown(a, sp->session_token, ROCK_THROW_BALLISTIC_SECONDS);
        a->net_dirty = true;
        /* last_towed_by / last_towed_token already set when the
         * tractor pulled the fragment in — leave them for smelt credit. */
    }
    sp->ship.towed_count = 0;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
}

static void apply_pod_band_force(server_player_t *sp, cargo_pod_t *pod, float dt) {
    towable_body_t body = {
        .pos = &pod->pos,
        .vel = &pod->vel,
        .inv_mass = (pod->kind == CARGO_POD_GAS) ? 1.2f : 0.8f,
    };
    ship_apply_body_tow(&sp->ship, &body, dt);
}

static bool ship_is_towing_pod(const ship_t *ship, int pod_idx) {
    if (!ship) return false;
    for (int i = 0; i < ship->towed_pod_count; i++)
        if (ship->towed_pods[i] == pod_idx) return true;
    return false;
}

static void step_towed_pod_forces(world_t *w, server_player_t *sp, float dt) {
    for (int t = 0; t < sp->ship.towed_pod_count; t++) {
        int idx = sp->ship.towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS || !w->cargo_pods[idx].active) {
            remove_towed_pod_slot(&sp->ship, t);
            t--;
            continue;
        }
        cargo_pod_t *pod = &w->cargo_pods[idx];
        pod->towed_by = (int8_t)sp->id;
        cargo_pod_clear_module_tractor(pod);
        apply_pod_band_force(sp, pod, dt);

        resolve_towed_body_ship_overlap(&sp->ship, &pod->pos, &pod->vel,
                                        pod->radius, 5.0f);
    }
}

static void step_cargo_pod_collection(world_t *w, server_player_t *sp, float dt) {
    step_towed_pod_forces(w, sp, dt);

    int max_tow = 2 + sp->ship.tractor_level * 2;
    if (!sp->ship.tractor_active || sp->ship.towed_pod_count >= max_tow) return;

    float tr = ship_tractor_range(&sp->ship);
    float tr_sq = tr * tr;
    commodity_t tow_filter = autopilot_tow_collection_filter(w, sp);
    for (int i = 0; i < MAX_CARGO_PODS && sp->ship.towed_pod_count < max_tow; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active) continue;
        if (pod->towed_by >= 0 && pod->towed_by != sp->id) continue;
        if (cargo_pod_has_module_tractor(pod)) continue;
        if (tow_filter != COMMODITY_COUNT && pod->commodity != tow_filter) continue;
        if (ship_is_towing_pod(&sp->ship, i)) continue;
        if (v2_dist_sq(sp->ship.pos, pod->pos) > tr_sq) continue;
        sp->ship.towed_pods[sp->ship.towed_pod_count++] = (int16_t)i;
        pod->towed_by = (int8_t)sp->id;
        cargo_pod_clear_module_tractor(pod);
        emit_event(w, (sim_event_t){.type = SIM_EVENT_PICKUP, .player_id = sp->id,
                                    .pickup = {.ore = (float)pod->quantity, .fragments = 1}});
    }
}

static void step_leashed_cargo_pods(world_t *w, server_player_t *sp, float dt) {
    float tractor_r = ship_tractor_range(&sp->ship);
    for (int t = sp->ship.towed_pod_count - 1; t >= 0; t--) {
        int idx = sp->ship.towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS || !w->cargo_pods[idx].active) {
            remove_towed_pod_slot(&sp->ship, t);
            continue;
        }
        cargo_pod_t *pod = &w->cargo_pods[idx];
        float dist = v2_len(v2_sub(sp->ship.pos, pod->pos));
        if (dist > tractor_r * 1.5f) {
            pod->towed_by = -1;
            remove_towed_pod_slot(&sp->ship, t);
            continue;
        }
        apply_pod_band_force(sp, pod, dt);
        resolve_towed_body_ship_overlap(&sp->ship, &pod->pos, &pod->vel,
                                        pod->radius, 5.0f);
    }
}

static void step_predicted_towed_body_forces(world_t *w, server_player_t *sp,
                                             float dt) {
    if (!w || !sp) return;

    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        asteroid_t *a = &w->asteroids[idx];
        if (!a->active) continue;
        apply_band_force(sp, a, dt);
        resolve_towed_body_ship_overlap(&sp->ship, &a->pos, &a->vel,
                                        a->radius, 4.0f);
    }

    for (int t = 0; t < sp->ship.towed_pod_count; t++) {
        int idx = sp->ship.towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS) continue;
        cargo_pod_t *pod = &w->cargo_pods[idx];
        if (!pod->active) continue;
        apply_pod_band_force(sp, pod, dt);
        resolve_towed_body_ship_overlap(&sp->ship, &pod->pos, &pod->vel,
                                        pod->radius, 5.0f);
    }
}

static void release_towed_pods(world_t *w, server_player_t *sp) {
    for (int t = 0; t < sp->ship.towed_pod_count; t++) {
        int idx = sp->ship.towed_pods[t];
        if (idx < 0 || idx >= MAX_CARGO_PODS || !w->cargo_pods[idx].active) continue;
        cargo_pod_t *pod = &w->cargo_pods[idx];
        vec2 to_ship = v2_sub(sp->ship.pos, pod->pos);
        float dist = v2_len(to_ship);
        if (dist > 0.01f) {
            vec2 dir = v2_scale(to_ship, 1.0f / dist);
            float stretch = fmaxf(0.0f, dist - SHIP_TOW_BAND_REST_LEN);
            float fling = ROCK_THROW_BASE_SPEED + fixp_sqrtf(SHIP_TOW_BAND_SPRING_K) * stretch;
            pod->vel = v2_add(sp->ship.vel, v2_scale(dir, fling));
        }
        pod->towed_by = -1;
    }
    sp->ship.towed_pod_count = 0;
    memset(sp->ship.towed_pods, -1, sizeof(sp->ship.towed_pods));
}

static bool station_hopper_matches_pod(const station_t *st,
                                       int module_idx,
                                       const cargo_pod_t *pod) {
    if (!st || !pod || module_idx < 0 ||
        module_idx >= st->module_count ||
        module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    if (!pod->active || pod->shipment_id != 0 ||
        pod->commodity >= COMMODITY_COUNT) {
        return false;
    }
    if (!cargo_pod_has_exact_manifest(pod, pod->commodity)) return false;
    const station_module_t *hopper = &st->modules[module_idx];
    if (hopper->scaffold || hopper->type != MODULE_HOPPER) return false;
    return (commodity_t)hopper->commodity == pod->commodity;
}

static bool station_hopper_can_tractor_pod(const station_t *st,
                                           int module_idx,
                                           const cargo_pod_t *pod) {
    return pod && pod->towed_by < 0 &&
           station_hopper_matches_pod(st, module_idx, pod);
}

#define CARGO_POD_BREAK_SPEED 360.0f
#define CARGO_POD_BOUNCE_SCALE 1.65f

static vec2 station_module_cargo_mouth(const station_t *st,
                                       const station_module_t *module,
                                       const cargo_pod_t *pod) {
    if (!st || !module) return v2(0.0f, 0.0f);
    vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
    vec2 outward = v2_sub(module_pos, st->pos);
    float len = v2_len(outward);
    if (len > 0.001f) {
        outward = v2_scale(outward, 1.0f / len);
    } else {
        outward = v2_from_angle(module_angle_ring(st, module->ring,
                                                  module->slot));
    }
    float pod_radius = (pod && pod->radius > 0.0f) ? pod->radius : 18.0f;
    float holdout = STATION_MODULE_COL_RADIUS + pod_radius + 8.0f;
    return v2_add(module_pos, v2_scale(outward, holdout));
}

static int cargo_pod_module_hold_slot(const world_t *w,
                                      const cargo_pod_t *pod,
                                      int station_idx,
                                      int module_idx,
                                      int *out_total) {
    if (out_total) *out_total = 1;
    if (!w || !pod || station_idx < 0 || module_idx < 0)
        return 0;

    int self = -1;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (&w->cargo_pods[i] == pod) {
            self = i;
            break;
        }
    }

    int slot = 0;
    int total = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *other = &w->cargo_pods[i];
        int ps = -1;
        int pm = -1;
        if (!other->active ||
            !cargo_pod_module_tractor_indices(other, &ps, &pm) ||
            ps != station_idx || pm != module_idx) {
            continue;
        }
        if (i == self) slot = total;
        total++;
    }

    if (out_total) *out_total = total > 0 ? total : 1;
    return slot;
}

static vec2 station_module_cargo_hold_anchor(const world_t *w,
                                             const station_t *st,
                                             int station_idx,
                                             int module_idx,
                                             const cargo_pod_t *pod,
                                             vec2 base_anchor) {
    if (!st || module_idx < 0 || module_idx >= st->module_count ||
        module_idx >= MAX_MODULES_PER_STATION) {
        return base_anchor;
    }

    const station_module_t *module = &st->modules[module_idx];
    vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
    vec2 outward = v2_sub(module_pos, st->pos);
    float len = v2_len(outward);
    if (len > 0.001f) {
        outward = v2_scale(outward, 1.0f / len);
    } else {
        outward = v2_from_angle(module_angle_ring(st, module->ring,
                                                  module->slot));
    }
    vec2 tangent = v2(-outward.y, outward.x);

    int total = 1;
    int slot = cargo_pod_module_hold_slot(w, pod, station_idx, module_idx,
                                          &total);
    enum { POD_HOLD_LANES = 4 };
    int row = slot / POD_HOLD_LANES;
    int lane = slot % POD_HOLD_LANES;
    int remaining = total - row * POD_HOLD_LANES;
    int lanes_this_row = remaining < POD_HOLD_LANES ? remaining : POD_HOLD_LANES;
    if (lanes_this_row < 1) lanes_this_row = 1;

    float pod_radius = (pod && pod->radius > 0.0f) ? pod->radius : 18.0f;
    float lane_spacing = pod_radius * 2.90f + 16.0f;
    float row_spacing = pod_radius * 2.20f + 20.0f;
    float centered_lane = (float)lane - ((float)lanes_this_row - 1.0f) * 0.5f;
    vec2 spread = v2_add(v2_scale(tangent, centered_lane * lane_spacing),
                         v2_scale(outward, (float)row * row_spacing));
    return v2_add(base_anchor, spread);
}

static bool station_producer_can_tractor_output_pod(const station_t *st,
                                                    int module_idx,
                                                    const cargo_pod_t *pod) {
    if (!st || !pod || module_idx < 0 ||
        module_idx >= st->module_count ||
        module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    if (pod->commodity >= COMMODITY_COUNT)
        return false;
    if (!cargo_pod_has_exact_manifest(pod, pod->commodity))
        return false;
    const station_module_t *module = &st->modules[module_idx];
    if (module->scaffold)
        return false;
    if (module->type == MODULE_FURNACE)
        return false;
    if (pod->manifest_count >= CARGO_POD_UNIT_CAPACITY)
        return false;
    const module_schema_t *schema = module_schema(module->type);
    if (!schema || schema->kind != MODULE_KIND_PRODUCER)
        return false;
    return module_instance_output(module) == pod->commodity;
}

static bool station_module_can_tractor_shell_frame_pod(const station_t *st,
                                                       int module_idx,
                                                       const cargo_pod_t *pod) {
    if (!st || !pod || module_idx < 0 ||
        module_idx >= st->module_count ||
        module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    if (!cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME)) {
        return false;
    }
    const station_module_t *module = &st->modules[module_idx];
    if (module->scaffold) return false;
    if (module->type == MODULE_FURNACE) return true;
    const module_schema_t *schema = module_schema(module->type);
    return schema && schema->kind == MODULE_KIND_PRODUCER;
}

static float point_segment_dist_sq(vec2 p, vec2 a, vec2 b);

static bool station_producer_input_hopper_for_pod(const station_t *st,
                                                  int module_idx,
                                                  const cargo_pod_t *pod,
                                                  int *out_hopper,
                                                  vec2 *out_anchor) {
    if (!st || !pod || pod->commodity >= COMMODITY_COUNT ||
        module_idx < 0 || module_idx >= st->module_count ||
        module_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    if (!cargo_pod_has_exact_manifest(pod, pod->commodity)) return false;

    const station_module_t *module = &st->modules[module_idx];
    if (module->scaffold || module->type == MODULE_FURNACE) return false;
    const module_schema_t *schema = module_schema(module->type);
    if (!schema || schema->kind != MODULE_KIND_PRODUCER) return false;

    module_inputs_t req = module_instance_required_inputs(module);
    bool wants = false;
    for (int i = 0; i < req.count; i++) {
        if (req.commodities[i] == pod->commodity) {
            wants = true;
            break;
        }
    }
    if (!wants) return false;

    int hopper_idx = station_find_hopper_for(st, pod->commodity);
    if (hopper_idx < 0 || hopper_idx >= st->module_count ||
        hopper_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    const station_module_t *hopper = &st->modules[hopper_idx];
    if (hopper->scaffold || hopper->type != MODULE_HOPPER ||
        (commodity_t)hopper->commodity != pod->commodity) {
        return false;
    }

    vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
    vec2 hopper_pos = module_world_pos_ring(st, hopper->ring, hopper->slot);
    if (v2_dist_sq(module_pos, hopper_pos) >
        HOPPER_PULL_RANGE * HOPPER_PULL_RANGE) {
        return false;
    }

    if (out_hopper) *out_hopper = hopper_idx;
    if (out_anchor) *out_anchor = v2_scale(v2_add(module_pos, hopper_pos), 0.5f);
    return true;
}

static bool station_hopper_input_anchor_for_pod(const station_t *st,
                                                int hopper_idx,
                                                const cargo_pod_t *pod,
                                                int *out_producer,
                                                vec2 *out_anchor) {
    if (!st || !pod || pod->commodity >= COMMODITY_COUNT ||
        hopper_idx < 0 || hopper_idx >= st->module_count ||
        hopper_idx >= MAX_MODULES_PER_STATION) {
        return false;
    }
    const station_module_t *hopper = &st->modules[hopper_idx];
    if (hopper->scaffold || hopper->type != MODULE_HOPPER ||
        (commodity_t)hopper->commodity != pod->commodity) {
        return false;
    }

    vec2 hopper_pos = module_world_pos_ring(st, hopper->ring, hopper->slot);
    int best_producer = -1;
    float best_d = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    vec2 best_anchor = hopper_pos;
    for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
        int matched_hopper = -1;
        vec2 anchor = hopper_pos;
        if (!station_producer_input_hopper_for_pod(st, m, pod,
                                                   &matched_hopper,
                                                   &anchor) ||
            matched_hopper != hopper_idx) {
            continue;
        }
        const station_module_t *module = &st->modules[m];
        vec2 module_pos = module_world_pos_ring(st, module->ring, module->slot);
        float d = point_segment_dist_sq(pod->pos, module_pos, hopper_pos);
        if (d <= best_d) {
            best_d = d;
            best_producer = m;
            best_anchor = anchor;
        }
    }

    if (best_producer < 0) return false;
    if (out_producer) *out_producer = best_producer;
    if (out_anchor) *out_anchor = best_anchor;
    return true;
}

static float point_segment_dist_sq(vec2 p, vec2 a, vec2 b) {
    vec2 ab = v2_sub(b, a);
    float ab_sq = v2_len_sq(ab);
    if (ab_sq <= 0.0001f) return v2_dist_sq(p, a);
    float t = v2_dot(v2_sub(p, a), ab) / ab_sq;
    t = clampf(t, 0.0f, 1.0f);
    vec2 closest = v2_add(a, v2_scale(ab, t));
    return v2_dist_sq(p, closest);
}

static bool cargo_pod_find_furnace_shell_hopper(const world_t *w,
                                                const cargo_pod_t *pod,
                                                int *out_station,
                                                int *out_module) {
    if (!w || !cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME))
        return false;

    const float beam_intake_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    const float shell_reach_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = beam_intake_sq;

    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;

        for (int f = 0; f < st->module_count && f < MAX_MODULES_PER_STATION; f++) {
            const station_module_t *furnace = &st->modules[f];
            if (furnace->scaffold || furnace->type != MODULE_FURNACE) continue;
            commodity_t ore = module_instance_input_ore(furnace);
            if (ore >= COMMODITY_RAW_ORE_COUNT) continue;
            vec2 furnace_pos = module_world_pos_ring(st, furnace->ring,
                                                     furnace->slot);

            int adj_rings[] = { furnace->ring + 1, furnace->ring - 1 };
            for (int ri = 0; ri < 2; ri++) {
                int adj = adj_rings[ri];
                if (adj < 1 || adj > STATION_NUM_RINGS) continue;
                for (int h = 0; h < st->module_count && h < MAX_MODULES_PER_STATION; h++) {
                    const station_module_t *ore_hopper = &st->modules[h];
                    if (ore_hopper->scaffold ||
                        ore_hopper->type != MODULE_HOPPER ||
                        ore_hopper->ring != adj ||
                        (commodity_t)ore_hopper->commodity != ore) {
                        continue;
                    }
                    vec2 ore_hopper_pos = module_world_pos_ring(
                        st, ore_hopper->ring, ore_hopper->slot);
                    float pod_d = point_segment_dist_sq(pod->pos,
                                                        furnace_pos,
                                                        ore_hopper_pos);
                    if (pod_d > best_d) continue;
                    vec2 smelt_target = v2_scale(
                        v2_add(furnace_pos, ore_hopper_pos), 0.5f);

                    for (int fh = 0; fh < st->module_count &&
                                     fh < MAX_MODULES_PER_STATION; fh++) {
                        const station_module_t *frame_hopper = &st->modules[fh];
                        if (frame_hopper->scaffold ||
                            frame_hopper->type != MODULE_HOPPER ||
                            (commodity_t)frame_hopper->commodity != COMMODITY_FRAME) {
                            continue;
                        }
                        vec2 frame_hopper_pos = module_world_pos_ring(
                            st, frame_hopper->ring, frame_hopper->slot);
                        if (v2_dist_sq(frame_hopper_pos, smelt_target) >
                            shell_reach_sq) {
                            continue;
                        }
                        best_d = pod_d;
                        best_station = s;
                        best_module = fh;
                    }
                }
            }
        }
    }

    if (best_station < 0 || best_module < 0) return false;
    if (out_station) *out_station = best_station;
    if (out_module) *out_module = best_module;
    return true;
}

static bool cargo_pod_find_producer_output_module(const world_t *w,
                                                  const cargo_pod_t *pod,
                                                  int *out_station,
                                                  int *out_module) {
    if (!w || !pod || !cargo_pod_has_exact_manifest(pod, pod->commodity))
        return false;
    if (pod->manifest_count >= CARGO_POD_UNIT_CAPACITY)
        return false;

    const float acquire_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            if (!station_producer_can_tractor_output_pod(st, m, pod))
                continue;
            vec2 anchor = module_world_pos_ring(st, st->modules[m].ring,
                                                st->modules[m].slot);
            float d = v2_dist_sq(pod->pos, anchor);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }

    if (best_station < 0 || best_module < 0) return false;
    if (out_station) *out_station = best_station;
    if (out_module) *out_module = best_module;
    return true;
}

static bool cargo_pod_find_producer_input_module(const world_t *w,
                                                 const cargo_pod_t *pod,
                                                 int *out_station,
                                                 int *out_module) {
    if (!w || !pod || !cargo_pod_has_exact_manifest(pod, pod->commodity))
        return false;

    const float acquire_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            int hopper_idx = -1;
            if (!station_producer_input_hopper_for_pod(st, m, pod,
                                                       &hopper_idx, NULL))
                continue;
            const station_module_t *module = &st->modules[m];
            const station_module_t *hopper = &st->modules[hopper_idx];
            vec2 module_pos = module_world_pos_ring(st, module->ring,
                                                    module->slot);
            vec2 hopper_pos = module_world_pos_ring(st, hopper->ring,
                                                    hopper->slot);
            float d = point_segment_dist_sq(pod->pos, module_pos, hopper_pos);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }

    if (best_station < 0 || best_module < 0) return false;
    if (out_station) *out_station = best_station;
    if (out_module) *out_module = best_module;
    return true;
}

static bool cargo_pod_find_shell_frame_module(const world_t *w,
                                              const cargo_pod_t *pod,
                                              int *out_station,
                                              int *out_module) {
    if (!w || !cargo_pod_has_exact_manifest(pod, COMMODITY_FRAME))
        return false;

    const float acquire_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            if (!station_module_can_tractor_shell_frame_pod(st, m, pod))
                continue;
            vec2 anchor = module_world_pos_ring(st, st->modules[m].ring,
                                                st->modules[m].slot);
            float d = v2_dist_sq(pod->pos, anchor);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }

    if (best_station < 0 || best_module < 0) return false;
    if (out_station) *out_station = best_station;
    if (out_module) *out_module = best_module;
    return true;
}

static bool cargo_pod_find_station_dock_module(const world_t *w,
                                               const cargo_pod_t *pod,
                                               int *out_station,
                                               int *out_module) {
    if (!w || !pod || pod->shipment_id != 0 ||
        !cargo_pod_has_exact_manifest(pod, pod->commodity)) {
        return false;
    }
    const float catch_speed = CARGO_POD_BREAK_SPEED * 0.85f;
    if (v2_len_sq(pod->vel) > catch_speed * catch_speed)
        return false;

    const float acquire_sq =
        CARGO_POD_DOCK_TRACTOR_RANGE * CARGO_POD_DOCK_TRACTOR_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        bool station_origin = pod->manifest_count > 0;
        for (uint16_t u = 0; u < pod->manifest_count && station_origin; u++) {
            if (pod->manifest_units[u].origin_station != (uint8_t)s)
                station_origin = false;
        }
        if (!station_origin) continue;
        bool hopper_claim_nearby = false;
        for (int h = 0; h < st->module_count && h < MAX_MODULES_PER_STATION; h++) {
            if (!station_hopper_matches_pod(st, h, pod)) continue;
            vec2 hopper_pos = module_world_pos_ring(st, st->modules[h].ring,
                                                    st->modules[h].slot);
            if (v2_dist_sq(pod->pos, hopper_pos) <=
                HOPPER_PULL_RANGE * HOPPER_PULL_RANGE) {
                hopper_claim_nearby = true;
                break;
            }
        }
        if (hopper_claim_nearby) continue;
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            if (!station_dock_can_tractor_trade_pod(st, m, pod)) continue;
            vec2 anchor = station_module_cargo_mouth(st, &st->modules[m], pod);
            float d = v2_dist_sq(pod->pos, anchor);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }

    if (best_station < 0 || best_module < 0) return false;
    if (out_station) *out_station = best_station;
    if (out_module) *out_module = best_module;
    return true;
}

static bool cargo_pod_find_black_market_dock_module(world_t *w,
                                                    const cargo_pod_t *pod,
                                                    int *out_station,
                                                    int *out_module) {
    if (!w || !pod || pod->shipment_id != 0 ||
        !cargo_pod_has_exact_manifest(pod, pod->commodity)) {
        return false;
    }
    const float acquire_sq =
        CARGO_POD_DOCK_TRACTOR_RANGE * CARGO_POD_DOCK_TRACTOR_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if ((!station_exists(st) || st->scaffold || st->planned) ||
            !station_policy_accepts_contract_bound_cargo(st) ||
            black_market_pod_quote(st, pod) <= FLOAT_EPSILON) {
            continue;
        }
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            const station_module_t *module = &st->modules[m];
            if (module->scaffold || module->type != MODULE_DOCK) continue;
            vec2 anchor = station_module_cargo_mouth(st, module, pod);
            float d = v2_dist_sq(pod->pos, anchor);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }

    if (best_station < 0 || best_module < 0) return false;
    if (out_station) *out_station = best_station;
    if (out_module) *out_module = best_module;
    return true;
}

static bool cargo_pod_current_module_tractor_valid(const world_t *w,
                                                   cargo_pod_t *pod,
                                                   int *out_station,
                                                   int *out_module,
                                                   vec2 *out_anchor,
                                                   int *out_pulse_module) {
    int station_idx = -1;
    int module_idx = -1;
    if (!w || !pod ||
        !cargo_pod_module_tractor_indices(pod, &station_idx, &module_idx)) {
        return false;
    }
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    const station_t *st = &w->stations[station_idx];
    bool dock_owner = station_dock_can_tractor_trade_pod(st, module_idx, pod);
    bool valid_owner = station_hopper_can_tractor_pod(st, module_idx, pod) ||
                       dock_owner ||
                       station_producer_input_hopper_for_pod(st, module_idx, pod,
                                                             NULL, NULL) ||
                       station_producer_can_tractor_output_pod(st, module_idx, pod) ||
                       station_module_can_tractor_shell_frame_pod(st, module_idx, pod);
    bool station_can_hold = station_is_active(st) ||
        (dock_owner && station_provides_docking(st) &&
         station_policy_accepts_contract_bound_cargo(st));
    if (!station_can_hold || !valid_owner) {
        cargo_pod_clear_module_tractor(pod);
        return false;
    }
    vec2 anchor = station_module_cargo_mouth(st, &st->modules[module_idx],
                                             pod);
    int pulse_module = module_idx;
    if (station_producer_input_hopper_for_pod(st, module_idx, pod,
                                              NULL, &anchor)) {
        pulse_module = module_idx;
    } else {
        int producer_idx = -1;
        vec2 input_anchor = anchor;
        if (station_hopper_input_anchor_for_pod(st, module_idx, pod,
                                                &producer_idx,
                                                &input_anchor)) {
            anchor = input_anchor;
            pulse_module = producer_idx;
        }
    }
    anchor = station_module_cargo_hold_anchor(w, st, station_idx, module_idx,
                                              pod, anchor);
    const station_module_t *held_module = &st->modules[module_idx];
    float valid_range = cargo_pod_module_tractor_range(held_module->type);
    tractor_beam_t pod_tractor =
        CARGO_POD_MODULE_TRACTOR_BEAM_INIT(valid_range);
    if (!tractor_beam_points_in_range(anchor, pod->pos, &pod_tractor)) {
        cargo_pod_clear_module_tractor(pod);
        return false;
    }
    if (out_station) *out_station = station_idx;
    if (out_module) *out_module = module_idx;
    if (out_anchor) *out_anchor = anchor;
    if (out_pulse_module) *out_pulse_module = pulse_module;
    return true;
}

static bool cargo_pod_try_acquire_module_tractor(world_t *w,
                                                 cargo_pod_t *pod) {
    if (!w || !pod || !pod->active || pod->towed_by >= 0 ||
        cargo_pod_has_module_tractor(pod)) {
        return false;
    }
    if (!cargo_pod_has_exact_manifest(pod, pod->commodity)) return false;

    const float acquire_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            if (!station_hopper_can_tractor_pod(st, m, pod)) continue;
            vec2 anchor = module_world_pos_ring(st, st->modules[m].ring,
                                                st->modules[m].slot);
            float d = v2_dist_sq(pod->pos, anchor);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }
    if (best_station < 0 || best_module < 0) {
        if (!cargo_pod_find_producer_input_module(w, pod,
                                                  &best_station,
                                                  &best_module)) {
                if (!cargo_pod_find_furnace_shell_hopper(w, pod,
                                                         &best_station,
                                                         &best_module)) {
                    if (!cargo_pod_find_producer_output_module(w, pod,
                                                               &best_station,
                                                               &best_module)) {
                        if (!cargo_pod_find_shell_frame_module(w, pod,
                                                               &best_station,
                                                               &best_module)) {
                            if (!cargo_pod_find_station_dock_module(w, pod,
                                                                    &best_station,
                                                                    &best_module)) {
                                return false;
                            }
                        }
                    }
                }
        }
    }
    cargo_pod_set_module_tractor(pod, best_station, best_module);
    return true;
}

static bool cargo_pod_try_handoff_to_matching_hopper(world_t *w,
                                                     int pod_idx,
                                                     cargo_pod_t *pod) {
    if (!w || !pod || !pod->active || pod_idx < 0 ||
        pod_idx >= MAX_CARGO_PODS || pod->towed_by < 0 ||
        pod->towed_by >= MAX_PLAYERS) {
        return false;
    }

    const float acquire_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    int best_station = -1;
    int best_module = -1;
    float best_d = acquire_sq;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            if (!station_hopper_matches_pod(st, m, pod)) continue;
            vec2 anchor = module_world_pos_ring(st, st->modules[m].ring,
                                                st->modules[m].slot);
            float d = v2_dist_sq(pod->pos, anchor);
            if (d <= best_d) {
                best_d = d;
                best_station = s;
                best_module = m;
            }
        }
    }
    if (best_station < 0 || best_module < 0) {
        if (!cargo_pod_find_producer_input_module(w, pod,
                                                  &best_station,
                                                  &best_module)) {
            if (!cargo_pod_find_furnace_shell_hopper(w, pod,
                                                     &best_station,
                                                     &best_module)) {
                if (!cargo_pod_find_producer_output_module(w, pod,
                                                           &best_station,
                                                           &best_module)) {
                    if (!cargo_pod_find_shell_frame_module(w, pod,
                                                           &best_station,
                                                           &best_module)) {
                        if (!cargo_pod_find_black_market_dock_module(
                                w, pod, &best_station, &best_module)) {
                            return false;
                        }
                    }
                }
            }
        }
    }

    server_player_t *sp = &w->players[pod->towed_by];
    station_t *st = &w->stations[best_station];
    if (!station_intake_pay_for_pod(w, sp, st, best_station, pod))
        return false;

    bool removed = false;
    for (int t = 0; t < sp->ship.towed_pod_count; t++) {
        if (sp->ship.towed_pods[t] == pod_idx) {
            remove_towed_pod_slot(&sp->ship, t);
            removed = true;
            break;
        }
    }
    if (!removed) return false;

    pod->towed_by = -1;
    cargo_pod_set_module_tractor(pod, best_station, best_module);
    if (best_station >= 0 && best_station < MAX_STATIONS &&
        best_module >= 0 && best_module < MAX_MODULES_PER_STATION) {
        w->stations[best_station].module_active_pulse[best_module] = 1.0f;
    }
    return true;
}

void step_station_cargo_pod_tractors(world_t *w, float dt) {
    if (!w) return;

    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active) continue;
        if (pod->towed_by >= 0) {
            if (!cargo_pod_try_handoff_to_matching_hopper(w, i, pod)) {
                cargo_pod_clear_module_tractor(pod);
                continue;
            }
        }
        if (!cargo_pod_has_module_tractor(pod))
            (void)cargo_pod_try_acquire_module_tractor(w, pod);

        int station_idx = -1;
        int module_idx = -1;
        int pulse_module = -1;
        vec2 anchor = pod->pos;
        if (!cargo_pod_current_module_tractor_valid(
                w, pod, &station_idx, &module_idx, &anchor, &pulse_module)) {
            continue;
        }
        if (dt > 0.0f) {
            tractor_anchor_t src = {
                .pos = anchor, .vel = NULL, .inv_mass = 0.0f
            };
            tractor_anchor_t tgt = {
                .pos = pod->pos, .vel = &pod->vel, .inv_mass = 1.0f
            };
            const station_module_t *module =
                &w->stations[station_idx].modules[module_idx];
            float tractor_range =
                cargo_pod_module_tractor_range(module->type);
            tractor_beam_t pod_tractor =
                CARGO_POD_MODULE_TRACTOR_BEAM_INIT(tractor_range);
            (void)tractor_apply(&src, &tgt, &pod_tractor, dt);
            if (station_idx >= 0 && station_idx < MAX_STATIONS &&
                pulse_module >= 0 && pulse_module < MAX_MODULES_PER_STATION) {
                w->stations[station_idx].module_active_pulse[pulse_module] = 1.0f;
            }
        }
    }
}

static void cargo_pod_revalidate_module_tractor(world_t *w, cargo_pod_t *pod) {
    if (!cargo_pod_has_module_tractor(pod)) return;
    (void)cargo_pod_current_module_tractor_valid(w, pod, NULL, NULL, NULL, NULL);
}

static void publish_cargo_pod_module_tractor_interactions(world_t *w) {
    if (!w) return;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active || !cargo_pod_has_module_tractor(pod)) continue;

        int station_idx = -1;
        int module_idx = -1;
        vec2 anchor = pod->pos;
        if (!cargo_pod_current_module_tractor_valid(
                w, pod, &station_idx, &module_idx, &anchor, NULL)) {
            continue;
        }
        if (station_idx < 0 || station_idx >= MAX_STATIONS ||
            module_idx < 0 || module_idx >= MAX_MODULES_PER_STATION) {
            continue;
        }

        const station_t *st = &w->stations[station_idx];
        if (module_idx >= st->module_count) continue;
        const station_module_t *module = &st->modules[module_idx];
        float tractor_range = cargo_pod_module_tractor_range(module->type);
        tractor_beam_t pod_tractor =
            CARGO_POD_MODULE_TRACTOR_BEAM_INIT(tractor_range);
        float intensity = tractor_beam_range_fraction(
            anchor, pod->pos, &pod_tractor);
        if (intensity <= 0.0f) continue;

        emit_interaction(w, (sim_interaction_t){
            .type = SIM_INTERACTION_TRACTOR_BEAM,
            .visual = SIM_INTERACTION_VISUAL_CARGO_POD_MODULE_TRACTOR,
            .commodity = pod->commodity < COMMODITY_COUNT
                ? (uint8_t)pod->commodity
                : (uint8_t)COMMODITY_COUNT,
            .source = {
                .type = SIM_INTERACTION_ENTITY_STATION_MODULE,
                .index = (int16_t)station_idx,
                .aux = (int16_t)module_idx,
            },
            .target = {
                .type = SIM_INTERACTION_ENTITY_CARGO_POD,
                .index = (int16_t)i,
                .aux = -1,
            },
            .source_pos = anchor,
            .target_pos = pod->pos,
            .range = tractor_range,
            .intensity = intensity,
        });
    }
}

static void clear_cargo_pod_and_tow_refs(world_t *w, int pod_idx) {
    if (!w || pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) return;
    for (int p = 0; p < MAX_PLAYERS; p++) {
        ship_t *ship = &w->players[p].ship;
        for (int t = ship->towed_pod_count - 1; t >= 0; t--) {
            if (ship->towed_pods[t] == pod_idx)
                remove_towed_pod_slot(ship, t);
        }
    }
    memset(&w->cargo_pods[pod_idx], 0, sizeof(w->cargo_pods[pod_idx]));
    w->cargo_pods[pod_idx].towed_by = -1;
}

static bool resolve_cargo_pod_circle_collision(world_t *w,
                                               int pod_idx,
                                               vec2 center,
                                               float radius,
                                               vec2 obstacle_vel) {
    if (!w || pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) return false;
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!pod->active) return false;

    float min_dist = pod->radius + radius;
    vec2 delta = v2_sub(pod->pos, center);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq >= min_dist * min_dist) return false;

    float dist = v2_len(delta);
    if (dist < 0.001f) {
        dist = 0.001f;
        delta = v2(1.0f, 0.0f);
    }
    vec2 normal = v2_scale(delta, 1.0f / dist);
    float closing = -v2_dot(v2_sub(pod->vel, obstacle_vel), normal);
    if (closing > CARGO_POD_BREAK_SPEED) {
        clear_cargo_pod_and_tow_refs(w, pod_idx);
        return true;
    }

    float overlap = min_dist - dist;
    pod->pos = v2_add(pod->pos, v2_scale(normal, overlap + 1.0f));
    float vel_along = v2_dot(v2_sub(pod->vel, obstacle_vel), normal);
    if (vel_along < 0.0f)
        pod->vel = v2_sub(pod->vel, v2_scale(normal,
                                             vel_along * CARGO_POD_BOUNCE_SCALE));
    return false;
}

static bool cargo_pod_near_corridor_module(const cargo_pod_t *pod,
                                           const station_geom_t *geom,
                                           const geom_corridor_t *cor) {
    if (!pod || !geom || !cor) return false;
    vec2 delta = v2_sub(pod->pos, geom->center);
    float dist = v2_len(delta);
    if (fabsf(dist - cor->ring_radius) >=
        STATION_CORRIDOR_HW + pod->radius + STATION_MODULE_COL_RADIUS) {
        return false;
    }

    float pod_ang = fixp_atan2f(delta.y, delta.x);
    for (int mi = 0; mi < geom->circle_count; mi++) {
        const geom_circle_t *circle = &geom->circles[mi];
        if (circle->ring != cor->ring) continue;
        float angular_size = (cor->ring_radius > 1.0f)
            ? (STATION_MODULE_COL_RADIUS + pod->radius) / cor->ring_radius
            : 0.0f;
        if (fabsf(wrap_angle(pod_ang - circle->angle)) < angular_size)
            return true;
    }
    return false;
}

static bool resolve_cargo_pod_corridor_collision(world_t *w,
                                                 int pod_idx,
                                                 vec2 center,
                                                 float ring_r,
                                                 float angle_a,
                                                 float arc_delta,
                                                 vec2 obstacle_vel) {
    if (!w || pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) return false;
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!pod->active) return false;

    vec2 delta = v2_sub(pod->pos, center);
    float dist = v2_len(delta);
    if (dist < 1.0f) return false;

    float r_inner = ring_r - STATION_CORRIDOR_HW - pod->radius;
    float r_outer = ring_r + STATION_CORRIDOR_HW + pod->radius;
    if (dist <= r_inner || dist >= r_outer) return false;

    float pod_angle = fixp_atan2f(delta.y, delta.x);
    float angular_margin = fixp_asinf(fminf(pod->radius / dist, 1.0f));
    float expanded_start = angle_a - angular_margin;
    float expanded_delta = arc_delta + 2.0f * angular_margin;
    if (angle_in_arc(pod_angle, expanded_start, expanded_delta) < 0.0f)
        return false;

    vec2 radial = v2_scale(delta, 1.0f / dist);
    vec2 push_normal;
    float d_inner = dist - (ring_r - STATION_CORRIDOR_HW);
    float d_outer = (ring_r + STATION_CORRIDOR_HW) - dist;
    if (d_inner < d_outer) {
        pod->pos = v2_add(center, v2_scale(radial,
            ring_r - STATION_CORRIDOR_HW - pod->radius - 1.0f));
        push_normal = v2_scale(radial, -1.0f);
    } else {
        pod->pos = v2_add(center, v2_scale(radial,
            ring_r + STATION_CORRIDOR_HW + pod->radius + 1.0f));
        push_normal = radial;
    }

    float closing = -v2_dot(v2_sub(pod->vel, obstacle_vel), push_normal);
    if (closing > CARGO_POD_BREAK_SPEED) {
        clear_cargo_pod_and_tow_refs(w, pod_idx);
        return true;
    }
    float vel_along = v2_dot(v2_sub(pod->vel, obstacle_vel), push_normal);
    if (vel_along < 0.0f)
        pod->vel = v2_sub(pod->vel, v2_scale(push_normal,
                                             vel_along * CARGO_POD_BOUNCE_SCALE));
    return false;
}

static bool resolve_cargo_pod_station_collisions(world_t *w, int pod_idx) {
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!pod->active)
        return false;

    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_collides(st)) continue;
        station_geom_t geom;
        station_build_geom(st, &geom);
        if (geom.has_core) {
            if (resolve_cargo_pod_circle_collision(
                    w, pod_idx, geom.core.center, geom.core.radius,
                    st->jostle_vel)) {
                return true;
            }
            if (!w->cargo_pods[pod_idx].active) return true;
        }
        for (int i = 0; i < geom.circle_count; i++) {
            if (resolve_cargo_pod_circle_collision(
                    w, pod_idx, geom.circles[i].center,
                    geom.circles[i].radius, st->jostle_vel)) {
                return true;
            }
            if (!w->cargo_pods[pod_idx].active) return true;
        }
        for (int i = 0; i < geom.corridor_count; i++) {
            const geom_corridor_t *cor = &geom.corridors[i];
            if (cargo_pod_near_corridor_module(pod, &geom, cor)) continue;
            if (resolve_cargo_pod_corridor_collision(
                    w, pod_idx, geom.center, cor->ring_radius,
                    cor->angle_a, cor->arc_delta, st->jostle_vel)) {
                return true;
            }
            if (!w->cargo_pods[pod_idx].active) return true;
        }
    }
    return false;
}

static bool resolve_cargo_pod_asteroid_collisions(world_t *w, int pod_idx) {
    cargo_pod_t *pod = &w->cargo_pods[pod_idx];
    if (!pod->active) return false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        if (resolve_cargo_pod_circle_collision(w, pod_idx, a->pos,
                                               a->radius, a->vel)) {
            return true;
        }
        if (!w->cargo_pods[pod_idx].active) return true;
    }
    return false;
}

static void resolve_cargo_pod_pair_collisions(world_t *w) {
    if (!w) return;
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            cargo_pod_t *a = &w->cargo_pods[i];
            if (!a->active) continue;
            for (int j = i + 1; j < MAX_CARGO_PODS; j++) {
                cargo_pod_t *b = &w->cargo_pods[j];
                if (!b->active) continue;

                float min_dist = a->radius + b->radius + 8.0f;
                vec2 ab = v2_sub(b->pos, a->pos);
                float d_sq = v2_len_sq(ab);
                if (d_sq >= min_dist * min_dist) continue;

                float d = 0.0f;
                vec2 n = v2(1.0f, 0.0f);
                if (d_sq > 0.1f) {
                    d = fixp_sqrtf(d_sq);
                    n = v2_scale(ab, 1.0f / d);
                } else {
                    float angle = (float)((i * 73 + j * 41 + pass * 29) % 360) *
                                  (PI_F / 180.0f);
                    n = v2_from_angle(angle);
                }

                float overlap = min_dist - d;
                vec2 shift = v2_scale(n, overlap * 0.5f + 0.75f);
                a->pos = v2_sub(a->pos, shift);
                b->pos = v2_add(b->pos, shift);

                float closing = v2_dot(v2_sub(b->vel, a->vel), n);
                if (closing < 0.0f) {
                    vec2 impulse = v2_scale(n, -closing * 0.62f);
                    a->vel = v2_sub(a->vel, impulse);
                    b->vel = v2_add(b->vel, impulse);
                }
            }
        }
    }
}

static void step_cargo_pods(world_t *w, float dt) {
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active) continue;
        pod->pos = v2_add(pod->pos, v2_scale(pod->vel, dt));
        pod->vel = v2_scale(pod->vel, 1.0f / (1.0f + 0.35f * dt));
        pod->rotation += pod->spin * dt;
        pod->age += dt;
        if (pod->quantity == 0) {
            if (!cargo_pod_fold_shell_to_frame(pod))
                clear_cargo_pod_and_tow_refs(w, i);
            continue;
        }
        if (resolve_cargo_pod_station_collisions(w, i) ||
            resolve_cargo_pod_asteroid_collisions(w, i)) {
            continue;
        }
        if (pod->age > 30.0f && signal_strength_at(w, pod->pos) <= 0.001f) {
            clear_cargo_pod_and_tow_refs(w, i);
        }
    }
    resolve_cargo_pod_pair_collisions(w);
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!pod->active) continue;
        cargo_pod_revalidate_module_tractor(w, pod);
    }
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
        for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
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
                commodity_t commodity = station_default_module_commodity(
                    st, sc->module_type);
                station_module_t *m = &st->modules[st->module_count++];
                m->type = sc->module_type;
                m->ring = (uint8_t)chosen_ring;
                m->slot = (uint8_t)chosen_slot;
                m->scaffold = true;
                m->build_progress = 0.0f; /* needs supply after outpost activates */
                m->last_smelt_commodity = LAST_SMELT_NONE;
                m->commodity = (uint8_t)commodity;
                m->_pad[0] = 0; m->_pad[1] = 0;
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
    for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
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
        for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
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
            chain_log_health_set(st, CHAIN_HEALTH_FRESH, false, 0, NULL,
                                 "new outpost chain; no log events yet");
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
                commodity_t commodity = station_default_module_commodity(
                    st, sc->module_type);
                station_module_t *m = &st->modules[st->module_count++];
                m->type = sc->module_type;
                m->ring = 1;
                m->slot = 0;
                m->scaffold = true;
                m->build_progress = 0.0f; /* needs supply after outpost activates */
                m->last_smelt_commodity = LAST_SMELT_NONE;
                m->commodity = (uint8_t)commodity;
                m->_pad[0] = 0; m->_pad[1] = 0;
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

/* Find scan target (station module, cargo pod, NPC, or player) along beam ray.
 * Returns true if a scan target was found, populating sp->scan_* fields. */
static bool find_scan_target(world_t *w, server_player_t *sp, vec2 muzzle, vec2 forward) {
    float best_dist = MINING_RANGE;
    sp->scan_target_type = INSPECT_TARGET_NONE;
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
            float sq = fixp_sqrtf(disc);
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

    /* Check towable cargo pods. Crates are deliberately small on-screen, so
     * give the scan ray a slight practical margin while still ranking by
     * actual hit distance. */
    for (int ci = 0; ci < MAX_CARGO_PODS; ci++) {
        const cargo_pod_t *pod = &w->cargo_pods[ci];
        if (!pod->active) continue;
        float scan_r = fmaxf(10.0f, pod->radius * 0.95f + 4.0f);
        vec2 hit; float along;
        if (laser_target_in_beam(&ray, pod->pos, scan_r, &hit, &along)
            && along < best_dist) {
            best_dist = along;
            sp->scan_target_type = INSPECT_TARGET_CARGO_POD;
            sp->scan_target_index = ci;
            sp->scan_module_index = -1;
            sp->beam_end = hit;
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

    return sp->scan_target_type != INSPECT_TARGET_NONE;
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
        float aim_slack = 0.0f;
        if (sp->input.mining_target_hint == sp->hover_asteroid &&
            hinted_target_in_mining_cone(muzzle, forward, a)) {
            aim_slack = 12.0f;
        }
        mining_beam_t mb = sim_mining_beam_step_with_aim_slack(w, muzzle, forward,
            sp->hover_asteroid, sp->ship.mining_level,
            ship_mining_rate(&sp->ship), signal_mining_efficiency(cached_signal),
            (int8_t)sp->id, dt, aim_slack);
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
    if (st->ledger_count < STATION_LEDGER_MAX) {
        idx = st->ledger_count++;
    } else {
        /* Prefer reclaiming an empty/inert entry; never discard a funded
         * ledger because that destroys spendable station-local credits. */
        int evict = -1;
        float worst = 0.0f;
        for (int i = 0; i < STATION_LEDGER_MAX; i++) {
            if (st->ledger[i].balance > 0.01f) continue;
            if (evict < 0 || st->ledger[i].lifetime_supply < worst) {
                worst = st->ledger[i].lifetime_supply;
                evict = i;
            }
        }
        if (evict < 0) return -1;
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

/* Hail: report station-local balance (informational -- no withdrawal).
 *
 * H used to manufacture a nearest-rock FRACTURE contract when the player had
 * nothing claimed. That made scan feel like a fake quest generator: a station
 * could "ask" for a random ferrite rock near the player, then the client would
 * paint it yellow as if it were intentional work. Hail now only returns real
 * station-authored work that already exists on the board. */
static int hail_find_station_work_contract(world_t *w, server_player_t *sp,
                                           int issuer_station,
                                           bool allow_delivery_pickup) {
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        contract_t *c = &w->contracts[i];
        if (c->active && c->claimed_by == (int8_t)sp->id) {
            if (c->action == CONTRACT_DELIVERY &&
                issuer_station == c->target_index &&
                allow_delivery_pickup) {
                (void)delivery_pickup_from_origin(w, sp, c, i);
            }
            return i;
        }
    }

    if (issuer_station < 0 || issuer_station >= MAX_STATIONS) return -1;

    int best_contract = -1;
    float best_score = 0.0f;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        contract_t *c = &w->contracts[i];
        if (!c->active) continue;
        bool station_matches = c->station_index == (uint8_t)issuer_station;
        if (c->action == CONTRACT_DELIVERY)
            station_matches = station_matches || c->target_index == issuer_station;
        if (!station_matches) continue;
        if (c->claimed_by >= 0 && c->claimed_by != (int8_t)sp->id) continue;

        float price_hint = isfinite(c->base_price) && c->base_price > 0.0f
            ? c->base_price : 1.0f;
        float score = 1.0f + price_hint * 0.01f;
        if (c->action == CONTRACT_TRACTOR) {
            commodity_t commodity = c->commodity;
            if (commodity < COMMODITY_RAW_ORE_COUNT) {
                for (int t = 0; t < sp->ship.towed_count; t++) {
                    int fi = sp->ship.towed_fragments[t];
                    if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
                    asteroid_t *a = &w->asteroids[fi];
                    if (contract_fit_is_ok(contract_fit_fragment(c, a)))
                        score += 500.0f;
                }
            } else {
                int held = contract_fit_manifest_count(c, &sp->ship.manifest);
                if (held > 0) score += 700.0f + (float)held * 10.0f;
                else          score += 100.0f;
            }
        } else if (c->action == CONTRACT_FRACTURE) {
            int idx = c->target_index;
            if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active)
                continue;
            float d = v2_len(v2_sub(w->asteroids[idx].pos, sp->ship.pos));
            score += 250.0f / fmaxf(1.0f, d / 1000.0f);
        } else if (c->action == CONTRACT_DELIVERY) {
            if (!delivery_contract_has_source(c)) continue;
            delivery_shipment_t *shipment =
                delivery_active_for_contract(w, sp->id, i);
            int origin = c->target_index;
            int destination = c->station_index;
            if (issuer_station == origin) {
                if (shipment && shipment->status == DELIVERY_SHIPMENT_DELIVERED) {
                    score += 1000.0f;
                } else if (!shipment) {
                    int stock = delivery_source_stock_count(&w->stations[origin], c);
                    if (stock <= 0) continue;
                    score += 800.0f + (float)stock * 10.0f;
                } else {
                    score += 100.0f;
                }
            } else if (issuer_station == destination) {
                if (!shipment || shipment->status != DELIVERY_SHIPMENT_PICKED_UP)
                    continue;
                score += 900.0f;
            } else {
                continue;
            }
        }

        if (score > best_score) {
            best_score = score;
            best_contract = i;
        }
    }

    if (best_contract >= 0) {
        contract_t *ct = &w->contracts[best_contract];
        if (ct->action == CONTRACT_DELIVERY &&
            issuer_station == ct->target_index) {
            if (allow_delivery_pickup)
                (void)delivery_pickup_from_origin(w, sp, ct, best_contract);
        } else if (ct->claimed_by < 0) {
            ct->claimed_by = (int8_t)sp->id;
        }
        contract_summary_t summary = contract_summary_make(ct);
        contract_pool_insert(sp->ship.known_contracts,
                             &sp->ship.known_contract_count,
                             SHIP_KNOWN_CONTRACT_CAP,
                             &summary);
        knowledge_view_configure(&sp->ship.knowledge, SHIP_KNOWN_ITEM_CAP);
        knowledge_item_t item;
        if (knowledge_item_from_contract_summary(&summary, &item))
            knowledge_view_insert(&sp->ship.knowledge, &item);
    }
    return best_contract;
}

static void emit_hail_miss(world_t *w, server_player_t *sp) {
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_HAIL_RESPONSE,
        .player_id = sp->id,
        .hail_response = { .station = -1, .credits = -1.0f, .contract_index = -1 },
    });
}

static void emit_station_hail_response(world_t *w, server_player_t *sp, int station_idx) {
    if (station_idx < 0 || station_idx >= MAX_STATIONS ||
        !station_is_active(&w->stations[station_idx])) {
        emit_hail_miss(w, sp);
        return;
    }

    delivery_clear_origin_proofs(w, sp, station_idx);

    float balance = server_player_can_use_pubkey_persistence(sp)
        ? ledger_balance_by_pubkey(&w->stations[station_idx], sp->pubkey)
        : ledger_balance(&w->stations[station_idx], sp->session_token);
    bool allow_delivery_pickup = sp->docked &&
        sp->current_station == station_idx;
    int contract_idx = hail_find_station_work_contract(
        w, sp, station_idx, allow_delivery_pickup);
    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_HAIL_RESPONSE,
        .player_id = sp->id,
        .hail_response = { .station = station_idx, .credits = balance, .contract_index = contract_idx },
    });
}

static int find_nearest_hail_station(const world_t *w, const server_player_t *sp) {
    if (sp->in_dock_range && sp->nearby_station >= 0 &&
        sp->nearby_station < MAX_STATIONS &&
        station_is_active(&w->stations[sp->nearby_station])) {
        return sp->nearby_station;
    }

    float comm = (sp->ship.comm_range > 0.0f) ? sp->ship.comm_range : 1500.0f;
    int best_station = -1;
    float best_d = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;

        float scan = st->signal_range;
        float comm_fallback = comm * 2.0f;
        if (scan < comm_fallback) scan = comm_fallback;
        float d_sq = v2_dist_sq(sp->ship.pos, st->pos);
        if (d_sq > scan * scan) continue;
        if (d_sq < best_d) {
            best_d = d_sq;
            best_station = s;
        }
    }
    return best_station;
}

static void handle_hail(world_t *w, server_player_t *sp) {
    /* Docked hail: the station the player is sitting in should answer
     * immediately. This keeps H from feeling dead on the station screen
     * and uses the same response/contract path as an undocked ping. */
    if (sp->docked) {
        emit_station_hail_response(w, sp, sp->current_station);
        return;
    }

    /* Hail is now a scan/contact action, not a binary comms-distance
     * check. Near-dock players should always get the station they are
     * interacting with; otherwise the closest active station in signal
     * coverage answers with the same full response. */
    int station_idx = find_nearest_hail_station(w, sp);
    if (station_idx >= 0)
        emit_station_hail_response(w, sp, station_idx);
    else
        emit_hail_miss(w, sp);
}

static bool try_dock_from_range(world_t *w, server_player_t *sp) {
    if (!sp->in_dock_range || sp->nearby_station < 0 ||
        sp->nearby_station >= MAX_STATIONS) {
        return false;
    }

    const station_t *dock_st = &w->stations[sp->nearby_station];
    int berth = find_best_berth(w, dock_st, sp->nearby_station, sp->ship.pos);
    sp->dock_berth = berth;
    vec2 bp = dock_berth_pos(dock_st, berth);
    float d = v2_len(v2_sub(sp->ship.pos, bp));
    if (d <= DOCK_SNAP_DISTANCE) {
        dock_ship(w, sp);
    } else {
        sp->docking_approach = true;
    }
    return true;
}

static void step_station_interaction_system(world_t *w, server_player_t *sp, const input_intent_t *intent) {
    if (sp->docked && !player_claim_waiting_ship_asset(w, sp)) {
        return;
    }

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
            bool can_afford = server_player_can_use_pubkey_persistence(sp) ?
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
    if (intent->commission_ship && sp->docked && !w->player_only_mode) {
        (void)shipyard_queue_ship_commission(w, sp->current_station, sp->id,
                                             intent->commission_hull_class);
    }
    /* Outpost / module placement via towed scaffold + reticle. */
    if (intent->place_outpost && !sp->docked && sp->ship.towed_scaffold >= 0) {
        place_towed_scaffold(w, sp);
        return;
    }
    if (intent->launch) {
        if (sp->docked) { launch_ship(w, sp); return; }
    } else if (intent->dock) {
        if (!sp->docked && try_dock_from_range(w, sp)) return;
    } else if (intent->interact) {
        if (sp->docked) { launch_ship(w, sp); return; }
        if (try_dock_from_range(w, sp)) return;
    }
    /* Cancel docking approach if player thrusts away */
    if (sp->docking_approach && (intent->thrust > 0.1f || intent->thrust < -0.1f)) {
        sp->docking_approach = false;
    }
    if (!sp->docked) return;
    delivery_clear_origin_proofs(w, sp, sp->current_station);
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
            if (server_player_can_use_pubkey_persistence(sp)) {
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
    /* Buy visible station-owned crates. Trade is custody transfer: if no
     * dock-held pod exists, there is nothing physical for the dock to hand
     * to the ship. */
    if (intent->buy_product && !w->player_only_mode) {
        commodity_t c = intent->buy_commodity;
        SIM_LOG("[buy] player %d req c=%d grade=%d at station %d (produces=%d)\n",
                sp->id, (int)c, (int)intent->buy_grade,
                sp->current_station,
                (c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT) ?
                    station_produces(docked_st, c) : -1);
        int physical_pod_buy = try_buy_station_market_pod(
            w, sp, docked_st, sp->current_station, c, intent->buy_grade,
            intent->buy_station_pod, intent->buy_station_pod_index);
        if (physical_pod_buy == 0)
            SIM_LOG("[buy] REJECT: no station-owned %s pod at %s dock\n",
                    commodity_short_name(c), docked_st->name);
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
            float d = fixp_sqrtf(dist_sq);
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
            float d = fixp_sqrtf(dist_sq);
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
     * but parse_input only runs when a NET_MSG_INPUT arrives (~60Hz), so
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
            signal_brain_drive(w, sp, dt);
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
        if (sp->ship.towed_pod_count > 0) {
            float tow_drag = 0.22f * (float)sp->ship.towed_pod_count;
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
        /* Prediction and server both run the same collision geometry so
         * obstacles feel solid immediately. The collision handlers suppress
         * damage/events/server-only dirtying while w->player_only_mode is set. */
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
                int physical_pod_buy = try_buy_station_market_pod(
                    w, sp, nearby_st, sp->nearby_station, c,
                    sp->input.buy_grade, sp->input.buy_station_pod,
                    sp->input.buy_station_pod_index);
                if (physical_pod_buy == 0)
                    SIM_LOG("[buy] REJECT: no station-owned %s pod near %s\n",
                            commodity_short_name(c), nearby_st->name);
            }
            /* Repair is now passive while docked — no laser interaction needed */
        }
        if (!sp->docked) {
            update_targeting_state(w, sp, forward);
            step_mining_system(w, sp, dt, sp->input.mine, forward, sig);
            if (w->player_only_mode) {
                step_predicted_towed_body_forces(w, sp, dt);
            } else {
                /* Hold R = tractor active; tap R = release fragments + scaffold */
                sp->ship.tractor_active = sp->input.tractor_hold;
                if (sp->input.release_tow) {
                    release_towed_fragments(w, sp);
                    release_towed_pods(w, sp);
                    release_towed_scaffold(w, sp);
                }
                step_towed_cleanup(w, sp);
                if (sp->ship.tractor_active) {
                    step_fragment_collection(w, sp, dt);
                    step_cargo_pod_collection(w, sp, dt);
                } else {
                    if (sp->ship.towed_pod_count > 0)
                        step_leashed_cargo_pods(w, sp, dt);
                }
                if (!sp->ship.tractor_active && sp->ship.towed_count > 0)
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
                        for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
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

    /* Hail: contact nearby station(s) and report station-local balance. */
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
        /* Validate position */
        bool too_close = false;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&w->stations[s])) continue;
            if (w->stations[s].planned) continue;
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
            for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
                if (!station_exists(&w->stations[s]) || w->stations[s].planned) {
                    slot = s; break;
                }
            }
            if (slot >= 0) {
                /* Faction-shared: only one planned outpost in the world at
                 * a time. Clear the old blueprint only after the replacement
                 * location has passed authoritative validation. */
                for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
                    station_t *old = &w->stations[s];
                    if (old->planned) {
                        SIM_LOG("[sim] player %d cancelled blueprint at slot %d (was owner %d)\n",
                            sp->id, s, old->planned_owner);
                        station_cleanup(old);
                        memset(old, 0, sizeof(*old));
                        (void)station_manifest_bootstrap(old);
                    }
                }
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
                chain_log_health_set(st, CHAIN_HEALTH_FRESH, false, 0, NULL,
                                     "planned outpost chain; no log events yet");
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
        if (s >= SIGNAL_FIRST_OUTPOST_INDEX && s < MAX_STATIONS && station_exists(&w->stations[s])
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
        if (s >= SIGNAL_FIRST_OUTPOST_INDEX && s < MAX_STATIONS && station_exists(&w->stations[s])) {
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
        if (s >= SIGNAL_FIRST_OUTPOST_INDEX && s < MAX_STATIONS) {
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
    sp->input.dock = false;
    sp->input.launch = false;
    sp->input.interact = false;
    sp->input.service_sell = false;
    sp->input.service_repair = false;
    sp->input.upgrade_mining = false;
    sp->input.upgrade_hold = false;
    sp->input.upgrade_tractor = false;
    sp->input.place_outpost = false;
    sp->input.buy_scaffold_kit = false;
    sp->input.commission_ship = false;
    sp->input.buy_product = false;
    sp->input.buy_station_pod = false;
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

static int contract_fracture_target_index(const world_t *w,
                                          const contract_t *ct)
{
    if (!w || !ct || ct->action != CONTRACT_FRACTURE) return -1;
    int idx = ct->target_index;
    if (idx >= 0 && idx < MAX_ASTEROIDS &&
        w->asteroids[idx].active &&
        contract_asteroid_target_matches(ct, &w->asteroids[idx])) {
        return idx;
    }
    if (!contract_target_pub_is_set(ct)) return -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        if (contract_asteroid_target_matches(ct, &w->asteroids[i]))
            return i;
    }
    return -1;
}

static bool heritage_recipe_for_commodity(commodity_t c, recipe_id_t *out) {
    if (!out) return false;
    switch (c) {
        case COMMODITY_FERRITE_INGOT:
        case COMMODITY_CUPRITE_INGOT:
        case COMMODITY_CRYSTAL_INGOT:
            *out = RECIPE_SMELT;
            return true;
        case COMMODITY_FRAME:
            *out = RECIPE_FRAME_BASIC;
            return true;
        case COMMODITY_LASER_MODULE:
            *out = RECIPE_LASER_BASIC;
            return true;
        case COMMODITY_TRACTOR_MODULE:
            *out = RECIPE_TRACTOR_COIL;
            return true;
        case COMMODITY_REPAIR_KIT:
            *out = RECIPE_REPAIR_KIT_FAB;
            return true;
        default:
            return false;
    }
}

static void contract_require_recipe_provenance(contract_t *c, recipe_id_t recipe) {
    if (!c) return;
    c->proof_flags |= (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                CONTRACT_PROOF_REQUIRE_RECIPE);
    c->required_recipe_id = (uint16_t)recipe;
}

static bool delivery_contract_duplicate(const world_t *w, int origin, int dest,
                                        commodity_t commodity) {
    if (!w) return true;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        const contract_t *ct = &w->contracts[i];
        if (!ct->active || ct->action != CONTRACT_DELIVERY) continue;
        if (ct->target_index == origin &&
            ct->station_index == (uint8_t)dest &&
            ct->commodity == commodity) {
            return true;
        }
    }
    return false;
}

static int delivery_best_source_for_contract(const world_t *w,
                                             const contract_t *demand) {
    if (!w || !demand || demand->station_index >= MAX_STATIONS) return -1;
    int dest = demand->station_index;
    int best = -1;
    float best_score = 0.0f;
    contract_t fit = *demand;
    fit.action = CONTRACT_DELIVERY;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == dest) continue;
        const station_t *source = &w->stations[s];
        if (!station_exists(source)) continue;
        int stock = delivery_source_stock_count(source, &fit);
        if (stock <= 0) continue;
        float d = v2_len(v2_sub(source->pos, w->stations[dest].pos));
        float score = (float)stock * 1000.0f - d;
        if (best < 0 || score > best_score) {
            best = s;
            best_score = score;
        }
    }
    return best;
}

static void delivery_maybe_post_credit_contracts(world_t *w) {
    if (!w) return;
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        const contract_t *demand = &w->contracts[i];
        if (!demand->active || demand->action != CONTRACT_TRACTOR) continue;
        if (demand->commodity < COMMODITY_RAW_ORE_COUNT) continue;
        if (demand->station_index >= MAX_STATIONS) continue;

        int free_slot = -1;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active) {
                free_slot = k;
                break;
            }
        }
        if (free_slot < 0) return;

        int dest = demand->station_index;
        int origin = delivery_best_source_for_contract(w, demand);
        if (origin < 0) continue;
        if (delivery_contract_duplicate(w, origin, dest, demand->commodity))
            continue;

        contract_t fit = *demand;
        fit.action = CONTRACT_DELIVERY;
        fit.target_index = origin;
        int stock = delivery_source_stock_count(&w->stations[origin], &fit);
        if (stock <= 0) continue;
        int qty = (int)ceilf(demand->quantity_needed);
        if (qty <= 0) qty = 1;
        if (qty > stock) qty = stock;
        if (qty > 4) qty = 4;
        if (qty > MAX_DELIVERY_BOUND_CARGO) qty = MAX_DELIVERY_BOUND_CARGO;
        if (qty <= 0) continue;

        contract_t delivery = *demand;
        delivery.action = CONTRACT_DELIVERY;
        delivery.station_index = (uint8_t)dest;
        delivery.target_index = origin;
        delivery.quantity_needed = (float)qty;
        delivery.base_price = demand->base_price * 1.05f;
        delivery.age = 0.0f;
        delivery.claimed_by = -1;
        delivery.target_pos = w->stations[origin].pos;
        w->contracts[free_slot] = delivery;
    }
}

static bool station_black_market_contract_commodity(const station_t *st,
                                                    commodity_t c) {
    if (!st || !station_policy_accepts_contract_bound_cargo(st))
        return false;
    switch (c) {
        case COMMODITY_TRACTOR_MODULE:
        case COMMODITY_LASER_MODULE:
        case COMMODITY_CRYSTAL_INGOT:
        case COMMODITY_CUPRITE_INGOT:
        case COMMODITY_FERRITE_INGOT:
        case COMMODITY_FRAME:
            return true;
        default:
            return false;
    }
}

static bool station_black_market_contract_need(const station_t *st,
                                               int station_idx,
                                               float pool_factor,
                                               contract_t *out) {
    if (!st || !out || station_idx < 0 || station_idx >= MAX_STATIONS)
        return false;
    if (!station_policy_accepts_contract_bound_cargo(st))
        return false;

    static const struct {
        commodity_t commodity;
        float target;
    } wants[] = {
        { COMMODITY_TRACTOR_MODULE, 6.0f },
        { COMMODITY_LASER_MODULE,   6.0f },
        { COMMODITY_CRYSTAL_INGOT, 10.0f },
        { COMMODITY_CUPRITE_INGOT, 10.0f },
        { COMMODITY_FERRITE_INGOT, 10.0f },
        { COMMODITY_FRAME,         12.0f },
    };

    commodity_t best = COMMODITY_COUNT;
    float best_score = 0.0f;
    float best_deficit = 0.0f;
    float best_base = 0.0f;
    for (size_t i = 0; i < sizeof(wants) / sizeof(wants[0]); i++) {
        commodity_t c = wants[i].commodity;
        float target = wants[i].target;
        float stock = (float)station_finished_count(st, c);
        float deficit = target - stock;
        if (deficit <= 0.5f) continue;
        float base = st->base_price[c];
        if (!isfinite(base) || base <= 0.0f)
            base = station_buy_price(st, c);
        if (!isfinite(base) || base <= 0.0f)
            base = 1.0f;
        float shortage = deficit / fmaxf(1.0f, target);
        float score = base * (1.0f + shortage);
        if (score > best_score) {
            best_score = score;
            best = c;
            best_deficit = deficit;
            best_base = base;
        }
    }

    if (best == COMMODITY_COUNT)
        return false;

    int qty = (int)ceilf(best_deficit);
    if (qty < 1) qty = 1;
    if (qty > 12) qty = 12;

    float price = best_base * (1.20f + 0.40f * pool_factor);
    if (!isfinite(price) || price <= 0.0f)
        price = best_base;

    *out = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = (uint8_t)station_idx,
        .commodity = best,
        .quantity_needed = (float)qty,
        .base_price = price,
        .target_index = -1,
        .claimed_by = -1,
    };
    return true;
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
                /* Non-construction: close once inventory reaches the
                 * commodity's actual buffer target. Bulk products use
                 * MAX_PRODUCT_STOCK hysteresis, kit inputs use the
                 * 12-unit shipyard target, and repair kits use their
                 * larger dock buffer. */
                float current = st->_inventory_cache[c];
                float threshold = MAX_PRODUCT_STOCK * 0.95f;
                if (c < COMMODITY_RAW_ORE_COUNT) {
                    threshold = REFINERY_HOPPER_CAPACITY * 0.95f;
                } else if ((c == COMMODITY_FRAME ||
                            c == COMMODITY_LASER_MODULE ||
                            c == COMMODITY_TRACTOR_MODULE) &&
                           station_has_module(st, MODULE_SHIPYARD)) {
                    threshold = 12.0f;
                } else if (c == COMMODITY_REPAIR_KIT &&
                           station_has_module(st, MODULE_DOCK) &&
                           !station_has_module(st, MODULE_SHIPYARD)) {
                    threshold = REPAIR_KIT_STOCK_CAP * 0.95f;
                }
                bool obsolete_raw_ore =
                    c < COMMODITY_RAW_ORE_COUNT &&
                    station_raw_ore_chain_need_score(st, c) <= 0.0f;
                if (obsolete_raw_ore || current >= threshold) {
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
            int idx = contract_fracture_target_index(w, &w->contracts[i]);
            bool target_gone = idx < 0;
            if (target_gone) {
                w->contracts[i].active = false;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_FRACTURE});
            } else if (idx != w->contracts[i].target_index) {
                w->contracts[i].target_index = idx;
                w->contracts[i].target_pos = w->asteroids[idx].pos;
            }
            /* Expire after 60 seconds if unfulfilled */
            if (w->contracts[i].active && w->contracts[i].age > 60.0f) w->contracts[i].active = false;
            break;
        }
        case CONTRACT_DELIVERY:
            if (w->contracts[i].claimed_by < 0 && w->contracts[i].age > 600.0f)
                w->contracts[i].active = false;
            break;
        }
    }

    /* Generate up to TWO contracts per station: one ore, one non-ore.
     * Priority: scaffold modules > empty hoppers > empty ingot buffers.
     * Ore and production contracts can coexist at the same station. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        station_policy_refresh(st, s, w->tick);

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
        bool has_black_market_contract = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active ||
                w->contracts[k].action != CONTRACT_TRACTOR ||
                w->contracts[k].station_index != s) {
                continue;
            }
            commodity_t cc = w->contracts[k].commodity;
            if (cc < COMMODITY_RAW_ORE_COUNT) {
                has_ore_contract = true;
            } else if (station_black_market_contract_commodity(st, cc)) {
                has_black_market_contract = true;
                has_production_contract = true;
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
                    commodity_t mat = module_build_material(st->modules[m].type);
                    float policy_mult = station_policy_trade_price_multiplier(st, mat);
                    need = (contract_t){
                        .active = true, .action = CONTRACT_TRACTOR,
                        .station_index = (uint8_t)s,
                        .commodity = mat,
                        .quantity_needed = remaining,
                        .base_price = st->base_price[mat] * 1.15f * pool_factor * policy_mult,
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
                float policy_mult = station_policy_trade_price_multiplier(st, COMMODITY_FRAME);
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = COMMODITY_FRAME,
                    .quantity_needed = remaining,
                    .base_price = 23.0f * pool_factor * policy_mult,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 3: ore chain with biggest useful deficit (ore slot).
         * Ore contracts are inventory-driven — fulfilled by fragment
         * smelting, not cargo delivery. quantity_needed is unused;
         * demand is gated by raw hopper room, refined output room, and
         * downstream product shortage. */
        if (!need.active && !has_ore_contract && station_has_module(st, MODULE_FURNACE)) {
            float worst_deficit = 0.0f;
            int worst_ore = -1;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                float deficit = station_raw_ore_need_score(st, (commodity_t)c);
                if (deficit > worst_deficit) { worst_deficit = deficit; worst_ore = c; }
            }
            if (worst_ore >= 0) {
                /* Demand multiplier: 1.0× when at target, up to 1.5× at
                 * total starvation. Layered on top of pool_factor so a
                 * starved-but-broke station still posts a sensible
                 * price. */
                float dmult = (1.0f + 0.5f * worst_deficit) *
                              station_policy_trade_price_multiplier(
                                  st, (commodity_t)worst_ore);
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
                { COMMODITY_CUPRITE_INGOT, station_has_module(st, MODULE_LASER_FAB) },
                { COMMODITY_CRYSTAL_INGOT, station_has_module(st, MODULE_TRACTOR_FAB) },
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
                commodity_t ingot = checks[worst_idx].ingot;
                float dmult = station_demand_for(st, ingot).price_mult *
                              station_policy_trade_price_multiplier(st, ingot);
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = ingot,
                    .quantity_needed = worst_deficit,
                    .base_price = st->base_price[ingot] * 1.15f * pool_factor * dmult,
                    .target_index = -1, .claimed_by = -1,
                };
                contract_require_recipe_provenance(&need, RECIPE_SMELT);
            }
        }

        /* Priority 5: black-market demand. Pirate/off-relay stations
         * publish institutional "no questions" buy pressure for scarce
         * high-value finished goods. This is still an ordinary station
         * contract, so gossip, NPC haulers, player delivery, and station
         * authority all use the same path. */
        if (!need.active && !has_production_contract &&
            !has_black_market_contract) {
            (void)station_black_market_contract_need(
                st, s, pool_factor, &need);
        }

        /* Priority 6: shipyard kit-fab inputs. Lives in its own slot
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
                float dmult = station_demand_for(st, mat).price_mult *
                              station_policy_trade_price_multiplier(st, mat);
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
                recipe_id_t recipe;
                if (heritage_recipe_for_commodity(mat, &recipe))
                    contract_require_recipe_provenance(&kit_need, recipe);
            }
        }

        /* Priority 7: kit imports at consumer-only stations. A station
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
                float dmult = station_demand_for(st, COMMODITY_REPAIR_KIT).price_mult *
                              station_policy_trade_price_multiplier(
                                  st, COMMODITY_REPAIR_KIT);
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = COMMODITY_REPAIR_KIT,
                    .quantity_needed = deficit,
                    .base_price = seed * 1.5f * pool_factor * dmult,
                    .target_index = -1, .claimed_by = -1,
                };
                contract_require_recipe_provenance(&need, RECIPE_REPAIR_KIT_FAB);
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
            station_policy_apply_contract_origin_rules(st, to_post[p]);
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
        /* All ingot tiers come from commodity-tagged furnace instances. */
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
    return station_nascent_scaffold_index(w->scaffolds, MAX_SCAFFOLDS,
                                          station_idx);
}

static void clear_loose_cargo_pod(cargo_pod_t *pod) {
    if (!pod) return;
    memset(pod, 0, sizeof(*pod));
    pod->towed_by = -1;
}

bool cargo_pod_take_manifest_unit(cargo_pod_t *pod,
                                  commodity_t commodity,
                                  cargo_unit_t *out_unit) {
    if (!out_unit || !cargo_pod_has_exact_manifest(pod, commodity))
        return false;
    uint16_t unit_idx = (uint16_t)(pod->manifest_count - 1u);
    *out_unit = pod->manifest_units[unit_idx];
    memset(&pod->manifest_units[unit_idx], 0,
           sizeof(pod->manifest_units[unit_idx]));
    pod->manifest_count--;
    pod->quantity--;
    if (pod->manifest_count == 0 &&
        !cargo_pod_fold_shell_to_frame(pod))
        clear_loose_cargo_pod(pod);
    return true;
}

static bool shipyard_hopper_serves_yard(const station_t *st,
                                        int hopper_idx,
                                        commodity_t material,
                                        int specific_yard_idx) {
    if (!st || hopper_idx < 0 || hopper_idx >= st->module_count ||
        material >= COMMODITY_COUNT) {
        return false;
    }
    const station_module_t *hopper = &st->modules[hopper_idx];
    if (hopper->scaffold || hopper->type != MODULE_HOPPER) return false;
    if ((commodity_t)hopper->commodity != material) return false;
    vec2 hopper_pos = module_world_pos_ring(st, hopper->ring, hopper->slot);
    float reach_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    for (int i = 0; i < st->module_count; i++) {
        if (specific_yard_idx >= 0 && i != specific_yard_idx) continue;
        const station_module_t *yard = &st->modules[i];
        if (yard->scaffold || yard->type != MODULE_SHIPYARD) continue;
        module_inputs_t req = module_required_inputs(MODULE_SHIPYARD);
        bool accepts = false;
        for (int r = 0; r < req.count; r++) {
            if (req.commodities[r] == material) {
                accepts = true;
                break;
            }
        }
        if (!accepts) continue;
        vec2 yard_pos = module_world_pos_ring(st, yard->ring, yard->slot);
        if (v2_dist_sq(hopper_pos, yard_pos) <= reach_sq)
            return true;
    }
    return false;
}

static int shipyard_feed_nascent_from_loose_pods(world_t *w,
                                                 const station_t *st,
                                                 int station_idx,
                                                 int yard_idx,
                                                 const scaffold_t *nascent,
                                                 commodity_t material,
                                                 int max_units) {
    if (!w || !st || !nascent || material >= COMMODITY_COUNT ||
        max_units <= 0) {
        return 0;
    }
    (void)nascent;
    int accepted = 0;
    for (int i = 0; i < MAX_CARGO_PODS && accepted < max_units; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(pod, material)) continue;
        if (pod->towed_by >= 0) continue;
        bool staged = false;
        for (int h = 0; h < st->module_count; h++) {
            const station_module_t *hopper = &st->modules[h];
            if (hopper->scaffold || hopper->type != MODULE_HOPPER) continue;
            if ((commodity_t)hopper->commodity != material) continue;
            if (!shipyard_hopper_serves_yard(st, h, material, yard_idx))
                continue;
            if (cargo_pod_is_tractored_by_module(pod, station_idx, h)) {
                staged = true;
                break;
            }
        }
        if (!staged) continue;
        while (accepted < max_units &&
               cargo_pod_has_exact_manifest(pod, material)) {
            cargo_unit_t unit = {0};
            if (!cargo_pod_take_manifest_unit(pod, material, &unit))
                break;
            accepted++;
        }
    }
    return accepted;
}

static bool shipyard_material_pod_staged_at_hopper(const station_t *st,
                                                   int station_idx,
                                                   const cargo_pod_t *pod,
                                                   commodity_t material,
                                                   int yard_idx,
                                                   bool allow_player_tow) {
    if (!st || !pod || material >= COMMODITY_COUNT) return false;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold || hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity != material) continue;
        if (!shipyard_hopper_serves_yard(st, i, material, yard_idx)) continue;
        if (cargo_pod_is_tractored_by_module(pod, station_idx, i))
            return true;
        if (allow_player_tow && pod->towed_by >= 0) {
            vec2 hopper_pos = module_world_pos_ring(st, hopper->ring,
                                                    hopper->slot);
            if (v2_dist_sq(pod->pos, hopper_pos) <=
                HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE)
                return true;
        }
    }
    return false;
}

static int shipyard_ship_tow_slot_for_pod(const ship_t *ship, int pod_idx) {
    if (!ship || pod_idx < 0) return -1;
    for (int t = 0; t < ship->towed_pod_count && t < 10; t++) {
        if (ship->towed_pods[t] == pod_idx) return t;
    }
    return -1;
}

static int shipyard_staged_material_count(const world_t *w,
                                          const station_t *st,
                                          int station_idx,
                                          const ship_t *ship,
                                          commodity_t c,
                                          bool include_towed_pods,
                                          int yard_idx) {
    if (!w || !st || c >= COMMODITY_COUNT) return 0;
    int total = 0;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(pod, c)) continue;
        int tow_slot = shipyard_ship_tow_slot_for_pod(ship, i);
        bool allow_player_tow = pod->towed_by >= 0 && include_towed_pods &&
                                tow_slot >= 0;
        if (pod->towed_by >= 0 &&
            (!include_towed_pods ||
             tow_slot < 0)) {
            continue;
        }
        if (!shipyard_material_pod_staged_at_hopper(st, station_idx, pod, c,
                                                    yard_idx,
                                                    allow_player_tow))
            continue;
        total += (int)pod->manifest_count;
    }
    return total;
}

static int shipyard_take_staged_material_units(world_t *w,
                                               const station_t *st,
                                               int station_idx,
                                               ship_t *ship,
                                               commodity_t c,
                                               int needed,
                                               bool include_towed_pods,
                                               int yard_idx) {
    if (!w || !st || c >= COMMODITY_COUNT || needed <= 0) return 0;
    int taken = 0;
    for (int i = 0; i < MAX_CARGO_PODS && taken < needed; i++) {
        cargo_pod_t *pod = &w->cargo_pods[i];
        if (!cargo_pod_has_exact_manifest(pod, c)) continue;
        int tow_slot = shipyard_ship_tow_slot_for_pod(ship, i);
        bool allow_player_tow = pod->towed_by >= 0 && include_towed_pods &&
                                tow_slot >= 0;
        if (pod->towed_by >= 0 &&
            (!include_towed_pods || tow_slot < 0)) {
            continue;
        }
        if (!shipyard_material_pod_staged_at_hopper(st, station_idx, pod, c,
                                                    yard_idx,
                                                    allow_player_tow))
            continue;
        while (taken < needed && cargo_pod_has_exact_manifest(pod, c)) {
            cargo_unit_t unit = {0};
            if (!cargo_pod_take_manifest_unit(pod, c, &unit))
                break;
            taken++;
            if (!pod->active && tow_slot >= 0) {
                remove_towed_pod_slot(ship, tow_slot);
                break;
            }
        }
    }
    return taken;
}

/* Is there a LOOSE scaffold still occupying the construction area near
 * this station's center? Used to gate spawning the next nascent. */
static bool construction_area_blocked(const world_t *w, int station_idx) {
    return station_construction_area_blocked(&w->stations[station_idx],
                                             w->scaffolds,
                                             MAX_SCAFFOLDS);
}

bool shipyard_hull_cost(hull_class_t hull_class, int *out_frames,
                        int *out_lasers, int *out_tractors) {
    if ((unsigned)hull_class >= HULL_CLASS_COUNT) return false;
    const hull_def_t *def = hull_def_for_class(hull_class);
    int frames = 2 * (int)def->module_slots;
    if (def->module_mask & SHIP_MODULE_CARGO) frames += 2;
    int lasers = (def->module_mask & SHIP_MODULE_LASER) ? 1 : 0;
    int tractors = (def->module_mask & SHIP_MODULE_TRACTOR) ? 1 : 0;
    if (out_frames) *out_frames = frames;
    if (out_lasers) *out_lasers = lasers;
    if (out_tractors) *out_tractors = tractors;
    return true;
}

bool shipyard_can_commission_hull(const station_t *st, hull_class_t hull_class) {
    if (!st || station_active_shipyard_count(st) < 1) return false;
    int frames = 0, lasers = 0, tractors = 0;
    if (!shipyard_hull_cost(hull_class, &frames, &lasers, &tractors)) return false;
    return station_finished_count(st, COMMODITY_FRAME) >= frames &&
           station_finished_count(st, COMMODITY_LASER_MODULE) >= lasers &&
           station_finished_count(st, COMMODITY_TRACTOR_MODULE) >= tractors;
}

static int shipyard_station_material_available(const station_t *st,
                                               commodity_t c,
                                               int yard_idx) {
    if (!st || c >= COMMODITY_COUNT) return 0;
    (void)yard_idx;
    int stored = station_finished_count(st, c);
    if (stored <= 0) return 0;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold || hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity != c) continue;
        return stored;
    }
    return 0;
}

static int shipyard_material_available(const station_t *st, const world_t *w,
                                       int station_idx, const ship_t *ship,
                                       commodity_t c,
                                       bool include_towed_pods,
                                       int yard_idx) {
    int total = shipyard_station_material_available(st, c, yard_idx);
    total += shipyard_staged_material_count(w, st, station_idx, ship, c,
                                            include_towed_pods, yard_idx);
    return total;
}

static bool shipyard_can_commission_hull_at_yard(const world_t *w,
                                                 const station_t *st,
                                                 int station_idx,
                                                 const ship_t *ship,
                                                 hull_class_t hull_class,
                                                 bool include_towed_pods,
                                                 int yard_idx) {
    if (!w || !st || yard_idx < 0 || yard_idx >= st->module_count)
        return false;
    const station_module_t *yard = &st->modules[yard_idx];
    if (yard->scaffold || yard->type != MODULE_SHIPYARD) return false;
    int frames = 0, lasers = 0, tractors = 0;
    if (!shipyard_hull_cost(hull_class, &frames, &lasers, &tractors)) return false;
    return shipyard_material_available(st, w, station_idx, ship, COMMODITY_FRAME,
                                       include_towed_pods, yard_idx) >= frames &&
           shipyard_material_available(st, w, station_idx, ship, COMMODITY_LASER_MODULE,
                                       include_towed_pods, yard_idx) >= lasers &&
           shipyard_material_available(st, w, station_idx, ship, COMMODITY_TRACTOR_MODULE,
                                       include_towed_pods, yard_idx) >= tractors;
}

static bool shipyard_find_ready_yard_for_hull(const world_t *w,
                                              const station_t *st,
                                              int station_idx,
                                              const ship_t *ship,
                                              hull_class_t hull_class,
                                              bool include_towed_pods,
                                              int *out_yard_idx) {
    if (out_yard_idx) *out_yard_idx = -1;
    if (!w || !st || station_active_shipyard_count(st) < 1) return false;
    for (int i = 0; i < st->module_count; i++) {
        if (shipyard_can_commission_hull_at_yard(
                w, st, station_idx, ship, hull_class, include_towed_pods, i)) {
            if (out_yard_idx) *out_yard_idx = i;
            return true;
        }
    }
    return false;
}

static bool shipyard_can_commission_hull_from_ship(const world_t *w,
                                                   const station_t *st,
                                                   int station_idx,
                                                   const ship_t *ship,
                                                   hull_class_t hull_class,
                                                   bool include_towed_pods) {
    return shipyard_find_ready_yard_for_hull(w, st, station_idx, ship, hull_class,
                                             include_towed_pods, NULL);
}

static bool shipyard_build_owner_valid(int owner, bool debit_player) {
    if (owner < INT8_MIN || owner > INT8_MAX) return false;
    if (debit_player) return owner >= 0 && owner < MAX_PLAYERS;
    return owner == -1 || shipyard_owner_code_is_station_request(owner);
}

static bool shipyard_prepare_pending_ship_build(world_t *w,
                                                pending_ship_build_t *build,
                                                int owner,
                                                hull_class_t hull_class,
                                                bool debit_player) {
    if (!w || !build) return false;
    memset(build, 0, sizeof(*build));
    build->hull_class = hull_class;
    build->owner = (int8_t)owner;
    build->build_progress = 0.0f;

    if (!debit_player) {
        build->owner_kind = (uint8_t)SHIP_ASSET_OWNER_STATION;
        return true;
    }

    if (owner < 0 || owner >= MAX_PLAYERS) return false;
    server_player_t *sp = &w->players[owner];
    if (server_player_can_use_pubkey_persistence(sp)) {
        build->owner_kind = (uint8_t)SHIP_ASSET_OWNER_PLAYER_PUBKEY;
        memcpy(build->owner_pubkey, sp->pubkey, sizeof(build->owner_pubkey));
        return true;
    }
    if (ship_asset_session_nonzero(sp->session_token)) {
        build->owner_kind = (uint8_t)SHIP_ASSET_OWNER_PLAYER_SESSION;
        memcpy(build->owner_session, sp->session_token,
               sizeof(build->owner_session));
        return true;
    }
    return false;
}

static int shipyard_consume_material_for_build(world_t *w, station_t *st,
                                               int station_idx,
                                               ship_t *ship, commodity_t c,
                                               int needed,
                                               bool include_towed_pods,
                                               int yard_idx) {
    if (!w || !st || needed <= 0) return 0;
    int from_staged = shipyard_staged_material_count(w, st, station_idx, ship, c,
                                                     include_towed_pods,
                                                     yard_idx);
    if (from_staged > needed) from_staged = needed;
    int from_station = needed - from_staged;
    if (shipyard_station_material_available(st, c, yard_idx) < from_station)
        return -1;

    if (from_staged > 0 &&
        shipyard_take_staged_material_units(w, st, station_idx, ship, c,
                                            from_staged,
                                            include_towed_pods,
                                            yard_idx) != from_staged) {
        return -1;
    }
    if (from_station > 0 &&
        station_finished_drain(st, c, from_station) != from_station) {
        return -1;
    }
    return from_station;
}

static bool shipyard_queue_hull_build(world_t *w, int station_idx, int owner,
                                      hull_class_t hull_class,
                                      bool debit_player) {
    if (!w || station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    if (!shipyard_build_owner_valid(owner, debit_player)) return false;
    station_t *st = &w->stations[station_idx];
    step_station_cargo_pod_tractors(w, 0.0f);
    if (st->pending_ship_build_count >= 4) return false;
    server_player_t *sp = NULL;
    bool include_towed_pods = false;
    if (debit_player && owner >= 0 && owner < MAX_PLAYERS) {
        sp = &w->players[owner];
        include_towed_pods = sp->connected && sp->docked &&
                             sp->current_station == station_idx;
    }
    int yard_idx = -1;
    if (!shipyard_find_ready_yard_for_hull(
            w, st, station_idx, include_towed_pods ? &sp->ship : NULL,
            hull_class, include_towed_pods, &yard_idx)) {
        return false;
    }
    pending_ship_build_t build;
    if (!shipyard_prepare_pending_ship_build(w, &build, owner, hull_class,
                                             debit_player)) {
        return false;
    }

    int frames = 0, lasers = 0, tractors = 0;
    if (!shipyard_hull_cost(hull_class, &frames, &lasers, &tractors)) return false;
    float frame_price = station_sell_price(st, COMMODITY_FRAME);
    float laser_price = station_sell_price(st, COMMODITY_LASER_MODULE);
    float tractor_price = station_sell_price(st, COMMODITY_TRACTOR_MODULE);
    int station_frames = shipyard_consume_material_for_build(
        w, st, station_idx, include_towed_pods ? &sp->ship : NULL,
        COMMODITY_FRAME,
        frames, include_towed_pods, yard_idx);
    if (station_frames < 0) return false;
    int station_lasers = shipyard_consume_material_for_build(
        w, st, station_idx, include_towed_pods ? &sp->ship : NULL,
        COMMODITY_LASER_MODULE,
        lasers, include_towed_pods, yard_idx);
    if (station_lasers < 0) {
        (void)station_finished_mint(st, COMMODITY_FRAME, station_frames, NULL);
        return false;
    }
    int station_tractors = shipyard_consume_material_for_build(
        w, st, station_idx, include_towed_pods ? &sp->ship : NULL,
        COMMODITY_TRACTOR_MODULE, tractors, include_towed_pods, yard_idx);
    if (station_tractors < 0) {
        (void)station_finished_mint(st, COMMODITY_FRAME, station_frames, NULL);
        (void)station_finished_mint(st, COMMODITY_LASER_MODULE,
                                    station_lasers, NULL);
        return false;
    }

    float commission_cost = 0.0f;
    (void)station_frames;
    (void)station_lasers;
    (void)station_tractors;
    commission_cost += (float)frames * frame_price;
    commission_cost += (float)lasers * laser_price;
    commission_cost += (float)tractors * tractor_price;

    if (debit_player && sp) {
        if (sp->connected && sp->docked && sp->current_station == station_idx)
            player_ledger_force_debit_at(sp, st, commission_cost);
    }

    int idx = st->pending_ship_build_count++;
    st->pending_ship_builds[idx] = build;
    return true;
}

bool shipyard_queue_ship_commission(world_t *w, int station_idx, int owner,
                                    hull_class_t hull_class) {
    return shipyard_queue_hull_build(w, station_idx, owner, hull_class, true);
}

bool shipyard_queue_station_hull_request(world_t *w, int requester_station,
                                         hull_class_t hull_class) {
    if (!w || requester_station < 0 || requester_station >= MAX_STATIONS)
        return false;
    if (!station_exists(&w->stations[requester_station])) return false;
    int owner_code = shipyard_station_request_owner_code(requester_station);
    if (!shipyard_owner_code_is_station_request(owner_code)) return false;
    step_station_cargo_pod_tractors(w, 0.0f);

    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        for (int p = 0; p < st->pending_ship_build_count; p++) {
            if (st->pending_ship_builds[p].owner == (int8_t)owner_code &&
                st->pending_ship_builds[p].hull_class == hull_class) {
                return true;
            }
        }
    }

    int best_station = -1;
    float best_d = 1e18f;
    vec2 requester_pos = w->stations[requester_station].pos;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;
        if (st->pending_ship_build_count >= 4) continue;
        if (!shipyard_can_commission_hull_from_ship(
                w, st, s, NULL, hull_class, false)) continue;
        float d = (s == requester_station)
            ? -1.0f
            : v2_dist_sq(requester_pos, st->pos);
        if (d < best_d) {
            best_d = d;
            best_station = s;
        }
    }
    if (best_station < 0) return false;
    return shipyard_queue_hull_build(w, best_station, owner_code,
                                     hull_class, false);
}

static float shipyard_hull_build_time(hull_class_t hull_class) {
    const hull_def_t *def = hull_def_for_class(hull_class);
    return 10.0f + 5.0f * (float)def->module_slots;
}

static void step_shipyard_shipbuilding(world_t *w, float dt) {
    if (!w) return;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;
        if (station_active_shipyard_count(st) < 1) continue;
        if (st->pending_ship_build_count <= 0) continue;

        st->pending_ship_builds[0].build_progress +=
            dt / shipyard_hull_build_time(st->pending_ship_builds[0].hull_class);
        if (st->pending_ship_builds[0].build_progress < 1.0f) continue;

        pending_ship_build_t build = st->pending_ship_builds[0];
        hull_class_t hull_class = build.hull_class;
        int owner = build.owner;
        ship_asset_t *completed_asset = NULL;
        if (owner >= 0 && owner < MAX_PLAYERS &&
            ((build.owner_kind == SHIP_ASSET_OWNER_PLAYER_PUBKEY &&
              ship_asset_pubkey_nonzero(build.owner_pubkey)) ||
             (build.owner_kind == SHIP_ASSET_OWNER_PLAYER_SESSION &&
              ship_asset_session_nonzero(build.owner_session)))) {
            server_player_t *sp = &w->players[owner];
            ship_asset_owner_kind_t owner_kind =
                (ship_asset_owner_kind_t)build.owner_kind;
            const uint8_t *owner_pubkey =
                owner_kind == SHIP_ASSET_OWNER_PLAYER_PUBKEY
                    ? build.owner_pubkey
                    : NULL;
            const uint8_t *owner_session =
                owner_kind == SHIP_ASSET_OWNER_PLAYER_SESSION
                    ? build.owner_session
                    : NULL;
            completed_asset = world_ship_asset_mint(
                w, hull_class, owner_kind, -1, s,
                SHIP_ASSET_PROVENANCE_SHIPYARD, false, s,
                owner_pubkey, owner_session);
            if (completed_asset && sp->connected && sp->docked &&
                sp->current_station == s &&
                ship_asset_player_matches_owner(completed_asset, sp)) {
                (void)ship_asset_assign_to_player(w, owner, completed_asset, s);
            }
        } else {
            int target_station = shipyard_owner_code_station(owner);
            if (target_station < 0 || target_station >= MAX_STATIONS ||
                !station_exists(&w->stations[target_station])) {
                target_station = s;
            }
            completed_asset = world_ship_asset_mint(
                w, hull_class, SHIP_ASSET_OWNER_STATION,
                target_station, target_station,
                SHIP_ASSET_PROVENANCE_SHIPYARD,
                hull_class == HULL_CLASS_MINER, s, NULL, NULL);
        }
        if (!completed_asset) {
            st->pending_ship_builds[0].build_progress = 1.0f;
            continue;
        }
        for (int i = 0; i < st->pending_ship_build_count - 1; i++)
            st->pending_ship_builds[i] = st->pending_ship_builds[i + 1];
        st->pending_ship_build_count--;
        SIM_LOG("[sim] station %d completed %s commission\n",
                s, ship_loadout_name(hull_class));
    }
}

/* Production layer v1: a nascent scaffold appears at the station center
 * when there's a pending order. Producer modules beam material to it.
 * The intake rate is layout-aware (same-ring fast, cross-ring slow).
 * When complete, the scaffold becomes LOOSE and can be towed away. */
/* module_flow_rate, module_accepts_input, step_module_flow → sim_production.c */

static void step_shipyard_manufacture(world_t *w, float dt) {
    step_station_cargo_pod_tractors(w, 0.0f);
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

        /* Pull physical loose pod units first; station inventory remains as
         * a transitional fallback for legacy saves and NPC flows. */
        if (nascent->build_amount < needed) {
            float room = needed - nascent->build_amount;
            int room_units = (int)ceilf(room - 0.0001f);
            int pod_units = shipyard_feed_nascent_from_loose_pods(
                w, st, s, yard_idx, nascent, mat, room_units);
            if (pod_units > 0) {
                nascent->build_amount += (float)pod_units;
                if (nascent->build_amount > needed)
                    nascent->build_amount = needed;
            }

            room = needed - nascent->build_amount;
            if (room > 0.01f) {
                float rate = shipyard_intake_rate(st, yard_idx, mat);
                float pull = rate * dt;
                if (pull > st->_inventory_cache[mat]) pull = st->_inventory_cache[mat];
                if (pull > room) pull = room;
                if (pull > 0.0f) {
                    st->_inventory_cache[mat] -= pull;
                    nascent->build_amount += pull;
                }
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
            vec2 dir = v2_from_angle(angle);
            nascent->pos = v2_add(st->pos, v2_scale(dir, 12.0f));
            nascent->vel = v2_scale(dir, 90.0f);
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
    float dist = v2_len(delta);
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
    float scaffold_angle = fixp_atan2f(delta.y, delta.x);
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
    commodity_t commodity = station_default_module_commodity(st, sc->module_type);
    station_module_t *m = &st->modules[st->module_count++];
    m->type = sc->module_type;
    m->ring = (uint8_t)sc->placed_ring;
    m->slot = (uint8_t)sc->placed_slot;
    m->scaffold = true;
    m->build_progress = 0.0f; /* enter post-placement supply phase */
    m->last_smelt_commodity = LAST_SMELT_NONE;
    m->commodity = (uint8_t)commodity;
    m->_pad[0] = 0; m->_pad[1] = 0;
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
                float dist = v2_len(delta);
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

            /* Planned station tractor: blueprints pull founding relays
             * straight toward center. No orbit — ghosts aren't rotating.
             * On arrival, materialize the ghost into a real station.
             * Non-relay module scaffolds wait until the outpost is active. */
            for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
                station_t *st = &w->stations[s];
                if (!st->planned) continue;
                if (sc->module_type != MODULE_SIGNAL_RELAY) continue;
                vec2 delta = v2_sub(st->pos, sc->pos);
                float dist_sq = v2_len_sq(delta);
                const float PLAN_PULL_RANGE = 800.0f;
                if (dist_sq > PLAN_PULL_RANGE * PLAN_PULL_RANGE) continue;
                float dist = fixp_sqrtf(dist_sq);
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
                        commodity_t commodity = station_default_module_commodity(
                            st, sc->module_type);
                        station_module_t *m = &st->modules[st->module_count++];
                        m->type = sc->module_type;
                        m->ring = (uint8_t)chosen_ring;
                        m->slot = (uint8_t)chosen_slot;
                        m->scaffold = true;
                        m->build_progress = 0.0f;
                        m->last_smelt_commodity = LAST_SMELT_NONE;
                        m->commodity = (uint8_t)commodity;
                        m->_pad[0] = 0; m->_pad[1] = 0;
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
            for (int s = SIGNAL_FIRST_OUTPOST_INDEX; s < MAX_STATIONS; s++) {
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
            float dist = v2_len(delta);

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

static bool signal_chain_hash_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

/* Append a sealed block to the per-station chain log on disk. The log
 * is the durable source of truth — the in-memory ring is just a cache.
 * Format: each record is a fixed-size signal_channel_msg_t blob (no
 * prev_hash field needed since prev_hash = previous record's entry_hash;
 * genesis is the all-zero hash). */
static bool signal_chain_disk_enabled = true;

void signal_chain_set_disk_enabled(bool enabled) {
    signal_chain_disk_enabled = enabled;
}

static void signal_chain_persist(int station, const signal_channel_msg_t *m) {
    if (!signal_chain_disk_enabled) return;
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

    /* Prev hash = the durable replay tail, or genesis (zeroes). Fall back
     * to the ring tail for older in-memory test fixtures that predate
     * signal_channel_t::last_hash. */
    uint8_t prev_hash[32] = {0};
    memcpy(prev_hash, ch->last_hash, sizeof(prev_hash));
    if (signal_chain_hash_is_zero(prev_hash) && ch->count > 0) {
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
    memcpy(ch->last_hash, m->entry_hash, sizeof(ch->last_hash));
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
     * a sortable buffer; second sorts by id, de-duplicates replayed ids,
     * and inserts the latest SIGNAL_CHANNEL_CAPACITY into the ring. The
     * chain spans the whole world (single feed across stations), so a
     * single ordering and one durable tail hash matter. */
    signal_channel_msg_t *scratch = NULL;
    size_t collected = 0;
#ifndef _WIN32
    size_t scratch_cap = 0;
    /* POSIX directory walk. Windows server is build-only (no production
     * deploy), so we no-op there to keep the cross-compile clean. */
    DIR *dir = opendir("chain");
    if (!dir) return;
    bool oom = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && !oom) {
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
        for (;;) {
            signal_channel_msg_t msg;
            if (fread(&msg, sizeof(msg), 1, f) != 1) break;
            if (msg.id == 0) continue;
            if (collected >= scratch_cap) {
                size_t next_cap = scratch_cap ? scratch_cap * 2u : 256u;
                signal_channel_msg_t *next =
                    (signal_channel_msg_t *)realloc(scratch,
                                                    next_cap * sizeof(*scratch));
                if (!next) {
                    oom = true;
                    break;
                }
                scratch = next;
                scratch_cap = next_cap;
            }
            scratch[collected++] = msg;
        }
        fclose(f);
    }
    closedir(dir);
    if (oom) {
        free(scratch);
        return;
    }
#endif

    /* Sort by id (insertion sort — collected is small in practice). */
    for (size_t i = 1; i < collected; i++) {
        signal_channel_msg_t key = scratch[i];
        size_t j = i;
        while (j > 0 && scratch[j - 1u].id > key.id) {
            scratch[j] = scratch[j - 1u];
            j--;
        }
        scratch[j] = key;
    }

    /* Collapse duplicate ids caused by old fresh-world restarts reusing
     * ids 1..N. Stable sort order means the later disk occurrence wins. */
    size_t unique = 0;
    for (size_t i = 0; i < collected; i++) {
        if (unique > 0 && scratch[unique - 1u].id == scratch[i].id) {
            scratch[unique - 1u] = scratch[i];
        } else {
            scratch[unique++] = scratch[i];
        }
    }

    /* Take the most recent SIGNAL_CHANNEL_CAPACITY unique ids into the ring,
     * but keep next_id and last_hash from the full unique replay tail. */
    size_t start = (unique > SIGNAL_CHANNEL_CAPACITY)
        ? unique - SIGNAL_CHANNEL_CAPACITY : 0;
    ch->head = 0;
    ch->count = 0;
    ch->next_id = 0;
    memset(ch->last_hash, 0, sizeof(ch->last_hash));
    if (unique > 0) {
        ch->next_id = scratch[unique - 1u].id;
        memcpy(ch->last_hash, scratch[unique - 1u].entry_hash,
               sizeof(ch->last_hash));
    }
    for (size_t i = start; i < unique; i++) {
        ch->msgs[ch->head] = scratch[i];
        ch->head = (ch->head + 1) % SIGNAL_CHANNEL_CAPACITY;
        if (ch->count < SIGNAL_CHANNEL_CAPACITY) ch->count++;
    }
    free(scratch);
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
            float dist = fixp_sqrtf(dist_sq);
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
            float speed = fixp_sqrtf(speed_sq);
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
    float T = pulse * RING_SPOKE_K * fixp_sinf(dr);
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
            module_inputs_t req = module_instance_required_inputs(prod);
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

static void sync_npc_paired_ship_physics(world_t *w, int npc_slot) {
    if (!w || npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return;
    npc_ship_t *npc = &w->npc_ships[npc_slot];
    if (!npc->active) return;
    ship_t *ship = world_npc_ship_for(w, npc_slot);
    if (!ship) return;
    ship->pos = npc->ship.pos;
    ship->vel = npc->ship.vel;
    ship->angle = npc->ship.angle;
    ship->hull_class = npc->ship.hull_class;
}

static bool sim_tick_after(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

static bool input_seq_after(uint16_t a, uint16_t b) {
    return (int16_t)(a - b) > 0;
}

static input_intent_t movement_intent_from_input(const input_intent_t *intent) {
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

static void apply_movement_intent(server_player_t *sp,
                                  const input_intent_t *intent) {
    if (!sp || !intent) return;
    sp->input.turn = intent->turn;
    sp->input.thrust = intent->thrust;
    sp->input.mine = intent->mine;
    sp->input.mining_target_hint = intent->mining_target_hint;
    sp->input.tractor_hold = intent->tractor_hold;
    sp->input.boost = intent->boost;
    sp->input.reverse_thrust = intent->reverse_thrust;
}

void server_player_queue_movement_input(server_player_t *sp,
                                        const input_intent_t *intent,
                                        uint16_t input_seq,
                                        uint32_t apply_tick) {
    if (!sp || !intent || apply_tick == 0) return;

    input_intent_t movement = movement_intent_from_input(intent);
    if (input_seq != 0) {
        if (sp->last_input_seq != 0 &&
            !input_seq_after(input_seq, sp->last_input_seq)) {
            return;
        }
        for (int i = 0; i < (int)sp->movement_queue_count; i++) {
            movement_input_cmd_t *cmd = &sp->movement_queue[i];
            if (cmd->input_seq == input_seq) {
                cmd->intent = movement;
                return;
            }
        }
    }

    uint8_t count = sp->movement_queue_count;
    if (count >= PLAYER_MOVEMENT_QUEUE_CAP) {
        memmove(&sp->movement_queue[0], &sp->movement_queue[1],
                (PLAYER_MOVEMENT_QUEUE_CAP - 1) *
                    sizeof(sp->movement_queue[0]));
        count = PLAYER_MOVEMENT_QUEUE_CAP - 1;
        sp->movement_queue_count = count;
    }

    int insert = (int)count;
    while (insert > 0 &&
           sim_tick_after(sp->movement_queue[insert - 1].apply_tick,
                          apply_tick)) {
        sp->movement_queue[insert] = sp->movement_queue[insert - 1];
        insert--;
    }
    sp->movement_queue[insert] = (movement_input_cmd_t){
        .apply_tick = apply_tick,
        .input_seq = input_seq,
        .intent = movement,
    };
    sp->movement_queue_count = (uint8_t)(count + 1u);
}

uint32_t server_input_apply_tick_for_world(const world_t *w,
                                           uint32_t client_tick) {
    if (!w) return 1u;
    const uint32_t max_future_ticks = NET_INPUT_APPLY_FUTURE_MAX_TICKS;
    uint32_t next_tick = w->tick + 1u;
    if (client_tick == 0 || !sim_tick_after(client_tick, w->tick))
        return next_tick;
    if (sim_tick_after(client_tick, w->tick + max_future_ticks))
        return w->tick + max_future_ticks;
    return client_tick;
}

void server_merge_one_shot_input(input_intent_t *dst,
                                 const input_intent_t *src) {
    if (!dst || !src) return;
    if (src->dock) {
        dst->dock = true;
        dst->interact = true;
    }
    if (src->launch) {
        dst->launch = true;
        dst->interact = true;
    }
    if (src->interact) dst->interact = true;
    if (src->service_sell) {
        dst->service_sell = true;
        dst->service_sell_only = src->service_sell_only;
        dst->service_sell_grade = src->service_sell_grade;
        dst->service_sell_one = src->service_sell_one;
    }
    if (src->service_repair) dst->service_repair = true;
    if (src->upgrade_mining) dst->upgrade_mining = true;
    if (src->upgrade_hold) dst->upgrade_hold = true;
    if (src->upgrade_tractor) dst->upgrade_tractor = true;
    if (src->place_outpost) {
        dst->place_outpost = true;
        dst->place_target_station = src->place_target_station;
        dst->place_target_ring = src->place_target_ring;
        dst->place_target_slot = src->place_target_slot;
    }
    if (src->buy_scaffold_kit) {
        dst->buy_scaffold_kit = true;
        dst->scaffold_kit_module = src->scaffold_kit_module;
    }
    if (src->commission_ship) {
        dst->commission_ship = true;
        dst->commission_hull_class = src->commission_hull_class;
    }
    if (src->buy_product) {
        dst->buy_product = true;
        dst->buy_commodity = src->buy_commodity;
        dst->buy_grade = src->buy_grade;
        dst->buy_station_pod = src->buy_station_pod;
        dst->buy_station_pod_index = src->buy_station_pod_index;
    }
    if (src->hail) dst->hail = true;
    if (src->release_tow) dst->release_tow = true;
    if (src->reset) dst->reset = true;
    if (src->toggle_autopilot) dst->toggle_autopilot = true;
}

static void server_input_intent_wire_defaults(input_intent_t *intent) {
    if (!intent) return;
    memset(intent, 0, sizeof(*intent));
    intent->mining_target_hint = -1;
    intent->buy_grade = MINING_GRADE_COUNT;
    intent->service_sell_only = COMMODITY_COUNT;
    intent->service_sell_grade = MINING_GRADE_COUNT;
}

bool server_dispatch_input_message(world_t *w, int player_idx,
                                   const uint8_t *data, int len,
                                   server_input_dispatch_result_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->station_identity_dirty = -1;
    }
    if (!w || player_idx < 0 || player_idx >= MAX_PLAYERS ||
        !data || len < 4) {
        return false;
    }

    server_player_t *sp = &w->players[player_idx];
    const uint8_t *input_data = data;
    uint8_t input_copy[NET_INPUT_MSG_SIZE];
    int input_len = len;
    uint8_t original_action = (len >= 3) ? data[2] : NET_ACTION_NONE;
    uint8_t effective_action = original_action;
    uint16_t input_seq = (len >= 10)
        ? (uint16_t)data[8] | ((uint16_t)data[9] << 8)
        : 0;
    uint16_t action_id = 0;
    uint8_t ack_status = 0;
    uint32_t client_tick = input_client_tick(data, len);
    bool rejected_unsigned_action = false;

    if (effective_action != NET_ACTION_NONE && sp->pubkey_set) {
        rejected_unsigned_action = true;
        ack_status = NET_ACTION_ACK_REJECTED;
        size_t copy_len = (size_t)len;
        if (copy_len > sizeof(input_copy)) copy_len = sizeof(input_copy);
        memcpy(input_copy, data, copy_len);
        input_copy[2] = NET_ACTION_NONE;
        input_data = input_copy;
        input_len = (int)copy_len;
        effective_action = NET_ACTION_NONE;
        if (len >= 14) action_id = input_action_id(data, len);
    } else if (len >= 14 && effective_action != NET_ACTION_NONE) {
        action_id = input_action_id(data, len);
        if (action_id != 0 && sp->last_input_action_id_valid &&
            sp->last_input_action_id == action_id) {
            ack_status = NET_ACTION_ACK_DUPLICATE;
            size_t copy_len = (size_t)len;
            if (copy_len > sizeof(input_copy)) copy_len = sizeof(input_copy);
            memcpy(input_copy, data, copy_len);
            input_copy[2] = NET_ACTION_NONE;
            input_data = input_copy;
            input_len = (int)copy_len;
            effective_action = NET_ACTION_NONE;
        } else if (action_id != 0) {
            sp->last_input_action_id = action_id;
            sp->last_input_action_id_valid = true;
            ack_status = NET_ACTION_ACK_RECEIVED;
        }
    }

    input_intent_t parsed;
    server_input_intent_wire_defaults(&parsed);
    parse_input(input_data, input_len, &parsed);
    uint32_t apply_tick = server_input_apply_tick_for_world(w, client_tick);
    server_player_queue_movement_input(sp, &parsed, input_seq, apply_tick);

    int station_dirty = -1;
    if ((effective_action >= NET_ACTION_BUY_SCAFFOLD_TYPED &&
         effective_action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) ||
        effective_action == NET_ACTION_BUY_SCAFFOLD) {
        int s = sp->current_station;
        if (s >= 0 && s < MAX_STATIONS) station_dirty = s;
    }

    if (out) {
        out->intent = parsed;
        out->action = original_action;
        out->ack_status = ack_status;
        out->action_id = action_id;
        out->input_seq = input_seq;
        out->client_tick = client_tick;
        out->apply_tick = apply_tick;
        out->rejected_unsigned_action = rejected_unsigned_action;
        out->force_authoritative_resync =
            ack_status == NET_ACTION_ACK_REJECTED &&
            original_action != NET_ACTION_NONE &&
            (action_id != 0 || sp->pubkey_set);
        out->station_identity_dirty = station_dirty;
    }
    return true;
}

uint16_t server_signed_action_payload_id(const uint8_t *payload,
                                         uint16_t payload_len,
                                         uint16_t fixed_len) {
    if (!payload || payload_len < (uint16_t)(fixed_len + 2u)) return 0;
    return read_u16_le(&payload[fixed_len]);
}

bool server_parse_signed_input_action_payload(const uint8_t *payload,
                                              uint16_t payload_len,
                                              input_intent_t *out_intent,
                                              uint16_t *out_action_id,
                                              uint8_t *out_action) {
    if (out_action_id) *out_action_id = 0;
    if (out_action) *out_action = NET_ACTION_NONE;
    if (out_intent) server_input_intent_wire_defaults(out_intent);
    if (!payload || payload_len < 5) return false;

    uint8_t action = payload[0];
    uint16_t action_id = (payload_len >= 7) ? read_u16_le(&payload[5]) : 0;
    uint8_t buf[8] = {
        NET_MSG_INPUT,
        0,
        action,
        0xFF,
        payload[1],
        payload[2],
        payload[3],
        payload[4],
    };
    input_intent_t parsed;
    server_input_intent_wire_defaults(&parsed);
    parse_input(buf, (int)sizeof(buf), &parsed);

    if (out_intent) *out_intent = parsed;
    if (out_action_id) *out_action_id = action_id;
    if (out_action) *out_action = action;
    return true;
}

bool server_apply_signed_plan_payload(server_player_t *sp,
                                      const uint8_t *payload,
                                      uint16_t payload_len) {
    if (!sp || !payload) return false;
    if (payload_len != NET_PLAN_MSG_SIZE - 1) return false;
    uint8_t buf[NET_PLAN_MSG_SIZE];
    buf[0] = NET_MSG_PLAN;
    memcpy(&buf[1], payload, NET_PLAN_MSG_SIZE - 1);
    parse_plan(buf, NET_PLAN_MSG_SIZE, &sp->input);
    return true;
}

static void server_dispatch_receipt_chain(
    server_receipt_chain_sink_fn sink,
    void *user,
    const cargo_receipt_chain_t *chain) {
    if (sink && chain && chain->len > 0 &&
        chain->len <= CARGO_RECEIPT_CHAIN_MAX_LEN) {
        sink(user, chain);
    }
}

static void server_dispatch_buy_named_ingot(
    world_t *w,
    server_player_t *sp,
    int pid,
    const uint8_t pubkey[32],
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user) {
    if (!w || !sp || !pubkey || !sp->docked) return;
    int sidx = sp->current_station;
    if (sidx < 0 || sidx >= MAX_STATIONS) return;
    station_t *st = &w->stations[sidx];
    ship_t *ship = &sp->ship;
    int slot = manifest_find(&st->manifest, pubkey);
    if (slot < 0) return;
    cargo_unit_t *src = &st->manifest.units[slot];
    if ((cargo_kind_t)src->kind != CARGO_KIND_INGOT) return;
    if ((ingot_prefix_t)src->prefix_class == INGOT_PREFIX_ANONYMOUS)
        return;

    int price = (int)lroundf(station_sell_price_unit(st, src));
    if (price <= 0) return;
    if (!station_manifest_bootstrap(st) || !ship_manifest_bootstrap(ship))
        return;

    ship_receipts_t *station_receipts = station_get_receipts(st);
    cargo_receipt_chain_t station_chain = {0};
    if (station_receipts && slot < (int)station_receipts->count)
        station_chain = station_receipts->chains[slot];
    if (station_chain.len >= CARGO_RECEIPT_CHAIN_MAX_LEN) return;

    bool spent = server_player_can_use_pubkey_persistence(sp)
        ? ledger_spend_by_pubkey(st, sp->pubkey, (float)price, ship)
        : ledger_spend(st, sp->session_token, (float)price, ship);
    if (!spent) return;

    cargo_unit_t copy = {0};
    if (!station_manifest_remove_with_chain(st, (uint16_t)slot, &copy,
                                            &station_chain)) {
        return;
    }

    cargo_receipt_t receipt = {0};
    uint8_t prev_hash[32] = {0};
    cargo_receipt_chain_t outgoing_chain = station_chain;
    if (station_chain.len > 0)
        cargo_receipt_hash(&station_chain.links[station_chain.len - 1],
                           prev_hash);
    uint64_t xfer_id = cargo_receipt_emit_transfer(
        w, st,
        st->station_pubkey,
        sp->pubkey,
        copy.pub,
        (uint8_t)CARGO_KIND_INGOT,
        station_chain.len > 0 ? prev_hash : st->chain_last_hash,
        &receipt);
    if (xfer_id != 0 && outgoing_chain.len < CARGO_RECEIPT_CHAIN_MAX_LEN)
        outgoing_chain.links[outgoing_chain.len++] = receipt;

    if (!ship_manifest_push_with_chain(ship, &copy, &outgoing_chain)) {
        (void)station_manifest_push_with_chain(st, &copy, &station_chain);
        return;
    }

    if (xfer_id != 0) {
        server_dispatch_receipt_chain(receipt_sink, receipt_user,
                                      &outgoing_chain);
        chain_payload_trade_t trade = {0};
        trade.transfer_event_id = xfer_id;
        trade.ledger_delta_signed = -(int64_t)price;
        memcpy(trade.ledger_pubkey, sp->pubkey, 32);
        (void)chain_log_emit(w, st, CHAIN_EVT_TRADE,
                             &trade, (uint16_t)sizeof(trade));
    }

    char cs[12];
    mining_render_callsign(copy.pub, cs);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s purchased %s for %d",
             sp->callsign, cs, price);
    signal_channel_post(w, sidx, msg, "");
    (void)pid;
}

static void server_dispatch_deliver_named_ingot(
    world_t *w,
    server_player_t *sp,
    int pid,
    uint8_t target,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user) {
    if (!w || !sp || !sp->docked) return;
    int sidx = sp->current_station;
    if (sidx < 0 || sidx >= MAX_STATIONS) return;
    station_t *st = &w->stations[sidx];
    ship_t *ship = &sp->ship;
    int hidx = -1;
    int seen = 0;
    for (uint16_t u = 0; u < ship->manifest.count; u++) {
        const cargo_unit_t *cu = &ship->manifest.units[u];
        if ((cargo_kind_t)cu->kind != CARGO_KIND_INGOT) continue;
        if ((ingot_prefix_t)cu->prefix_class == INGOT_PREFIX_ANONYMOUS)
            continue;
        if (seen == target) { hidx = (int)u; break; }
        seen++;
    }
    if (hidx < 0) return;

    cargo_unit_t copy = ship->manifest.units[hidx];
    cargo_receipt_chain_t attached_chain = {0};
    ship_receipts_t *rcpts = ship_get_receipts(ship);
    if (rcpts && hidx < (int)rcpts->count) {
        const cargo_receipt_chain_t *attached = &rcpts->chains[hidx];
        attached_chain = *attached;
        if (attached->len > 0) {
            cargo_receipt_result_t vr = cargo_receipt_chain_verify(
                attached->links, attached->len, copy.pub);
            if (vr != CARGO_RECEIPT_OK) {
                printf("[server] receipt_chain_invalid: deliver from player %d, reason=%d\n",
                       pid, (int)vr);
                return;
            }
            if (attached->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
                printf("[server] receipt_chain_cap_exceeded: deliver from player %d\n",
                       pid);
                return;
            }
        }
    }

    if (st->manifest.count >= st->manifest.cap) {
        cargo_unit_t evicted = {0};
        if (station_manifest_remove_with_chain(st, 0, &evicted, NULL) &&
            (ingot_prefix_t)evicted.prefix_class != INGOT_PREFIX_ANONYMOUS) {
            char ev_cs[12];
            mining_render_callsign(evicted.pub, ev_cs);
            char ev_msg[96];
            snprintf(ev_msg, sizeof(ev_msg), "stockpile full - voided %s",
                     ev_cs);
            signal_channel_post(w, sidx, ev_msg, "");
        }
    }

    uint8_t prev_hash[32] = {0};
    bool have_prev = false;
    if (attached_chain.len > 0) {
        cargo_receipt_hash(&attached_chain.links[attached_chain.len - 1],
                           prev_hash);
        have_prev = true;
    }

    cargo_receipt_chain_t removed_chain = {0};
    if (!ship_manifest_remove_with_chain(ship, (uint16_t)hidx,
                                         &copy, &removed_chain)) {
        return;
    }

    cargo_receipt_t receipt = {0};
    cargo_receipt_chain_t station_chain = removed_chain;
    uint64_t xfer_id = cargo_receipt_emit_transfer(
        w, st,
        sp->pubkey,
        st->station_pubkey,
        copy.pub,
        (uint8_t)CARGO_KIND_INGOT,
        have_prev ? prev_hash : st->chain_last_hash,
        &receipt);
    if (xfer_id != 0 && station_chain.len < CARGO_RECEIPT_CHAIN_MAX_LEN)
        station_chain.links[station_chain.len++] = receipt;

    if (!station_manifest_push_with_chain(st, &copy, &station_chain)) {
        (void)ship_manifest_push_with_chain(ship, &copy, &removed_chain);
        return;
    }

    float delivery_f = station_buy_price_unit(st, &copy);
    float floor_f = (float)INGOT_DELIVERY_CREDIT;
    if (delivery_f < floor_f) delivery_f = floor_f;
    int delivery_int = (int)lroundf(delivery_f);
    if (server_player_can_use_pubkey_persistence(sp)) {
        ledger_credit_supply_by_pubkey(st, sp->pubkey, (float)delivery_int);
    } else {
        ledger_credit_supply(st, sp->session_token, (float)delivery_int);
    }

    if (xfer_id != 0) {
        server_dispatch_receipt_chain(receipt_sink, receipt_user,
                                      &station_chain);
        chain_payload_trade_t trade = {0};
        trade.transfer_event_id = xfer_id;
        trade.ledger_delta_signed = (int64_t)delivery_int;
        memcpy(trade.ledger_pubkey, sp->pubkey, 32);
        (void)chain_log_emit(w, st, CHAIN_EVT_TRADE,
                             &trade, (uint16_t)sizeof(trade));
    }

    char cs[12];
    mining_render_callsign(copy.pub, cs);
    char msg[96];
    snprintf(msg, sizeof(msg), "%s delivered %s", sp->callsign, cs);
    signal_channel_post(w, sidx, msg, "");
}

bool server_dispatch_legacy_plan_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_unsigned_dispatch_result_t *out) {
    if (out) out->rejected_unsigned_action = false;
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_idx];
    if (sp->pubkey_set) {
        if (out) out->rejected_unsigned_action = true;
        return true;
    }
    parse_plan(data, len, &sp->input);
    return true;
}

bool server_dispatch_legacy_buy_ingot_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user,
    server_legacy_cargo_dispatch_result_t *out) {
    if (out) out->rejected_unsigned_action = false;
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_idx];
    if (sp->pubkey_set) {
        if (out) out->rejected_unsigned_action = true;
        return true;
    }
    if (len >= 33) {
        server_dispatch_buy_named_ingot(w, sp, player_idx, &data[1],
                                        receipt_sink, receipt_user);
    }
    return true;
}

bool server_dispatch_legacy_deliver_ingot_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user,
    server_legacy_cargo_dispatch_result_t *out) {
    if (out) out->rejected_unsigned_action = false;
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_idx];
    if (sp->pubkey_set) {
        if (out) out->rejected_unsigned_action = true;
        return true;
    }
    if (len >= 2) {
        server_dispatch_deliver_named_ingot(w, sp, player_idx, data[1],
                                            receipt_sink, receipt_user);
    }
    return true;
}

bool server_dispatch_receipt_presentation_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_receipt_presentation_dispatch_result_t *out) {
    if (out) {
        out->evaluated = false;
        out->result = CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS;
    }
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    if (len < 35) return true;

    const uint8_t *cargo_pub = &data[1];
    uint16_t chain_len = read_u16_le(&data[33]);
    size_t expected = 35u + (size_t)chain_len * CARGO_RECEIPT_SIZE;
    if (chain_len == 0 || chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN)
        return true;
    if ((size_t)len < expected) return true;

    cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN];
    for (uint16_t i = 0; i < chain_len; i++) {
        const uint8_t *p = &data[35u + (size_t)i * CARGO_RECEIPT_SIZE];
        (void)cargo_receipt_unpack(p, &chain[i]);
    }

    cargo_receipt_present_result_t result = cargo_receipt_present_to_ship(
        &w->players[player_idx], cargo_pub, chain, (uint8_t)chain_len);
    if (out) {
        out->evaluated = true;
        out->result = result;
    }
    return true;
}

bool server_dispatch_fracture_claim_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_unsigned_dispatch_result_t *out) {
    if (out) out->rejected_unsigned_action = false;
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_idx];
    if (sp->pubkey_set) {
        if (out) out->rejected_unsigned_action = true;
        return true;
    }
    if (len >= FRACTURE_CLAIM_SIZE) {
        uint32_t fracture_id = read_u32_le(&data[1]);
        uint32_t burst_nonce = read_u32_le(&data[5]);
        (void)submit_fracture_claim(w, player_idx, fracture_id,
                                    burst_nonce, data[9]);
    }
    return true;
}

bool server_dispatch_signed_action_payload(
    world_t *w,
    int player_idx,
    uint8_t action_type,
    const uint8_t *payload,
    uint16_t payload_len,
    server_receipt_chain_sink_fn receipt_sink,
    void *receipt_user,
    server_signed_action_dispatch_result_t *out) {
    if (out) out->station_identity_dirty = -1;
    if (!w || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_idx];

    switch ((signed_action_type_t)action_type) {
    case SIGNED_ACTION_BUY_PRODUCT:
        if (payload_len >= 2) {
            uint8_t commodity = payload[0];
            uint8_t grade = payload[1];
            uint16_t action_id = server_signed_action_payload_id(
                payload, payload_len, 2);
            uint8_t action = (commodity < COMMODITY_COUNT)
                ? (uint8_t)(NET_ACTION_BUY_PRODUCT + commodity)
                : NET_ACTION_BUY_PRODUCT;
            server_begin_pending_action_result(w, sp, action_id, 0, action);
            if (commodity < COMMODITY_COUNT) {
                sp->input.buy_product = true;
                sp->input.buy_commodity = (commodity_t)commodity;
                sp->input.buy_grade = (grade <= MINING_GRADE_COUNT)
                    ? (mining_grade_t)grade
                    : MINING_GRADE_COUNT;
            }
        }
        return true;
    case SIGNED_ACTION_BUY_INGOT:
        if (payload_len >= 32) {
            uint16_t action_id = server_signed_action_payload_id(
                payload, payload_len, 32);
            server_begin_pending_action_result(
                w, sp, action_id, 0, NET_ACTION_BUY_INGOT);
            server_dispatch_buy_named_ingot(
                w, sp, player_idx, payload, receipt_sink, receipt_user);
        }
        return true;
    case SIGNED_ACTION_SELL_CARGO:
        if (payload_len >= 2) {
            uint8_t commodity = payload[0];
            uint8_t grade = payload[1];
            uint16_t action_id = server_signed_action_payload_id(
                payload, payload_len, 2);
            uint8_t action = (commodity < COMMODITY_COUNT)
                ? (uint8_t)(NET_ACTION_DELIVER_COMMODITY + commodity)
                : NET_ACTION_SELL_CARGO;
            server_begin_pending_action_result(w, sp, action_id, 0, action);
            sp->input.service_sell = true;
            sp->input.service_sell_only = (commodity < COMMODITY_COUNT)
                ? (commodity_t)commodity
                : COMMODITY_COUNT;
            if (grade < MINING_GRADE_COUNT) {
                sp->input.service_sell_grade = (mining_grade_t)grade;
                sp->input.service_sell_one = true;
            } else {
                sp->input.service_sell_grade = MINING_GRADE_COUNT;
                sp->input.service_sell_one = false;
            }
        }
        return true;
    case SIGNED_ACTION_DELIVER:
        if (payload_len >= 1) {
            uint16_t action_id = server_signed_action_payload_id(
                payload, payload_len, 1);
            server_begin_pending_action_result(
                w, sp, action_id, 0, NET_ACTION_SELL_CARGO);
            server_dispatch_deliver_named_ingot(
                w, sp, player_idx, payload[0], receipt_sink, receipt_user);
        }
        return true;
    case SIGNED_ACTION_PLACE_OUTPOST:
        if (payload_len >= 3) {
            uint16_t action_id = server_signed_action_payload_id(
                payload, payload_len, 3);
            server_begin_pending_action_result(
                w, sp, action_id, 0, NET_ACTION_PLACE_OUTPOST);
            sp->input.place_outpost = true;
            sp->input.place_target_station = (int8_t)payload[0];
            sp->input.place_target_ring = (int8_t)payload[1];
            sp->input.place_target_slot = (int8_t)payload[2];
        }
        return true;
    case SIGNED_ACTION_FRACTURE_CLAIM:
        if (payload_len >= 9) {
            uint32_t fracture_id = (uint32_t)payload[0]
                                 | ((uint32_t)payload[1] << 8)
                                 | ((uint32_t)payload[2] << 16)
                                 | ((uint32_t)payload[3] << 24);
            uint32_t burst_nonce = (uint32_t)payload[4]
                                 | ((uint32_t)payload[5] << 8)
                                 | ((uint32_t)payload[6] << 16)
                                 | ((uint32_t)payload[7] << 24);
            (void)submit_fracture_claim(w, player_idx, fracture_id,
                                        burst_nonce, payload[8]);
        }
        return true;
    case SIGNED_ACTION_INPUT_ACTION:
    {
        input_intent_t intent;
        uint16_t action_id = 0;
        uint8_t action = NET_ACTION_NONE;
        if (!server_parse_signed_input_action_payload(payload, payload_len,
                                                      &intent, &action_id,
                                                      &action)) {
            return true;
        }
        if (action_id != 0)
            server_begin_pending_action_result(w, sp, action_id, 0, action);
        server_merge_one_shot_input(&sp->input, &intent);
        if ((action >= NET_ACTION_BUY_SCAFFOLD_TYPED &&
             action < NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_COUNT) ||
            action == NET_ACTION_BUY_SCAFFOLD) {
            int s = sp->current_station;
            if (out && s >= 0 && s < MAX_STATIONS)
                out->station_identity_dirty = s;
        }
        return true;
    }
    case SIGNED_ACTION_PLAN:
        (void)server_apply_signed_plan_payload(sp, payload, payload_len);
        return true;
    case SIGNED_ACTION_CLAIM_CONTRACT:
    case SIGNED_ACTION_CANCEL_CONTRACT:
        return true;
    case SIGNED_ACTION_COUNT:
    default:
        return false;
    }
}

bool server_dispatch_handoff_request(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_handoff_ticket_sink_fn ticket_sink,
    void *ticket_user) {
    if (!w || !data || len < NET_HANDOFF_REQUEST_SIZE ||
        player_idx < 0 || player_idx >= MAX_PLAYERS) {
        return false;
    }

    uint8_t source_wire = data[1];
    uint8_t dest_wire = data[2];
    int source_station = (source_wire == 0xFFu)
        ? w->players[player_idx].current_station
        : (int)source_wire;
    int dest_station = (int)dest_wire;
    uint32_t ttl_ticks = read_u32_le(&data[3]);
    handoff_ticket_t ticket;
    memset(&ticket, 0, sizeof(ticket));
    bool ok = handoff_issue_ticket_to_station(
        w, player_idx, source_station, dest_station, ttl_ticks, &ticket);

    if (ticket_sink) {
        ticket_sink(ticket_user,
                    ok ? NET_HANDOFF_STATUS_OK : NET_HANDOFF_STATUS_REJECTED,
                    (uint8_t)(source_station >= 0 && source_station < 256
                              ? source_station : 0xFF),
                    dest_wire,
                    ok ? &ticket : NULL);
    }
    return true;
}

bool server_dispatch_handoff_present(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_handoff_result_sink_fn result_sink,
    void *result_user) {
    if (!w || !data ||
        (size_t)len < 1u + HANDOFF_TICKET_SIZE + 4u ||
        player_idx < 0 || player_idx >= MAX_PLAYERS) {
        return false;
    }

    handoff_ticket_t ticket;
    uint8_t ticket_hash[32];
    ship_t presented;
    size_t consumed = 0;
    int dest_station = -1;
    handoff_flow_result_t hr = HANDOFF_FLOW_REJECT_BAD_ARGS;
    uint32_t snapshot_len = read_u32_le(&data[1 + HANDOFF_TICKET_SIZE]);
    size_t snapshot_off = 1u + HANDOFF_TICKET_SIZE + 4u;
    memset(&ticket, 0, sizeof(ticket));
    memset(ticket_hash, 0, sizeof(ticket_hash));
    memset(&presented, 0, sizeof(presented));

    if (handoff_ticket_unpack(&data[1], &ticket))
        handoff_ticket_hash(&ticket, ticket_hash);

    if (snapshot_len <= HANDOFF_SHIP_SNAPSHOT_MAX_SIZE &&
        (size_t)len >= snapshot_off + (size_t)snapshot_len &&
        handoff_ship_snapshot_unpack(&data[snapshot_off],
                                     (size_t)snapshot_len,
                                     &presented, &consumed) &&
        consumed == (size_t)snapshot_len) {
        hr = handoff_accept_presented_ship(w, player_idx, &ticket,
                                           &presented, &dest_station);
    }

    if (result_sink) {
        result_sink(result_user,
                    hr == HANDOFF_FLOW_OK ? NET_HANDOFF_STATUS_OK
                                          : NET_HANDOFF_STATUS_REJECTED,
                    (uint8_t)hr,
                    (uint8_t)(dest_station >= 0 && dest_station < 256
                              ? dest_station : 0xFF),
                    ticket_hash);
    }
    ship_cleanup(&presented);
    return true;
}

static void server_player_apply_queued_movement(server_player_t *sp,
                                                uint32_t tick) {
    if (!sp) return;
    while (sp->movement_queue_count > 0) {
        const movement_input_cmd_t *cmd = &sp->movement_queue[0];
        if (sim_tick_after(cmd->apply_tick, tick)) break;
        apply_movement_intent(sp, &cmd->intent);
        sp->last_input_seq = cmd->input_seq;
        sp->last_input_tick = tick;
        sp->movement_queue_count--;
        if (sp->movement_queue_count > 0) {
            memmove(&sp->movement_queue[0], &sp->movement_queue[1],
                    sp->movement_queue_count * sizeof(sp->movement_queue[0]));
        }
    }
}

void world_sim_step(world_t *w, float dt) {
    w->events.count = 0;
    sim_interactions_clear(w);
    w->tick++;
    w->time += dt;
    step_station_ring_dynamics(w, dt);
    step_station_jostle(w, dt);
    sim_step_asteroid_dynamics(w, dt);
    step_station_cargo_pod_tractors(w, dt);
    step_cargo_pods(w, dt);
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
    step_shipyard_shipbuilding(w, dt);
    step_module_flow(w, dt);

    /* Manifest-as-truth reconciliation: snap floor(inventory[c]) ==
     * manifest_count(c) for every finished commodity at every station.
     * Now bidirectional — production paths mint manifest in lockstep
     * with float increments, NPC unload + delivery + trade sales drain
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
    step_frontier_director(w, dt);
    step_scaffolds(w, dt);
    step_contracts(w, dt);
    step_delivery_shipments(w);
    step_npc_ships(w, dt);
    generate_npc_distress_contracts(w, dt);
    delivery_maybe_post_credit_contracts(w);
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!w->players[p].connected) continue;
        server_player_apply_queued_movement(&w->players[p], w->tick);
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
            float d = fixp_sqrtf(d_sq);
            vec2 normal = d > 0.00001f
                ? v2_scale(delta, 1.0f / d)
                : actor_stack_normal(i, j);
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
            float d = fixp_sqrtf(d_sq);
            vec2 normal = d > 0.00001f
                ? v2_scale(delta, 1.0f / d)
                : actor_stack_normal(i, j);
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
            sync_npc_paired_ship_physics(w, i);
            sync_npc_paired_ship_physics(w, j);
        }
    }

    /* Player-NPC collision: same shape as player-player. Players push
     * NPCs around at full force (mass-symmetric), and ramming a hauler
     * costs both sides hull. Collision writes land on npc_ship_t first,
     * then sync_npc_paired_ship_physics keeps the paired ship_t from
     * lagging one tick behind. */
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
            float d = fixp_sqrtf(d_sq);
            vec2 normal = d > 0.00001f
                ? v2_scale(delta, 1.0f / d)
                : actor_stack_normal(i, n + MAX_PLAYERS);
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
            sync_npc_paired_ship_physics(w, n);
        }
    }

    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (w->players[p].connected)
            (void)world_ship_asset_sync_from_player(w, &w->players[p]);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (w->npc_ships[n].active)
            (void)world_ship_asset_sync_from_npc(w, n);
    }
    publish_cargo_pod_module_tractor_interactions(w);
    world_refresh_station_hull_inventories(w);
}

/* ================================================================== */
/* Public: world_sim_step_player_only                                 */
/* ================================================================== */

void world_sim_step_player_only(world_t *w, int player_idx, float dt) {
    w->events.count = 0;
    sim_interactions_clear(w);
    /* Do NOT advance w->time or w->tick — both are server-authoritative. */
    if (player_idx < 0 || player_idx >= MAX_PLAYERS) return;
    server_player_t *sp = &w->players[player_idx];
    if (!sp->connected) return;
    w->player_only_mode = true;  /* suppress mining HP and world RNG mutation */
    step_player(w, sp, dt);
    w->player_only_mode = false;
    (void)world_ship_asset_sync_from_player(w, sp);
}

/* ================================================================== */
/* Public: world_reset                                                */
/* ================================================================== */

void world_cleanup(world_t *w) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        ship_cleanup(&w->players[i].ship);
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        ship_cleanup(&w->ship_assets[i].ship);
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

void world_ensure_seeded_freeport(world_t *w) {
    if (!w) return;
    station_t *st = &w->stations[SIGNAL_FREEPORT_STATION_INDEX];
    if (station_exists(st)) return;

    station_cleanup(st);
    memset(st, 0, sizeof(*st));
    (void)station_manifest_bootstrap(st);
    station_authority_init_seeded(st, w->belt_seed,
                                  (uint32_t)SIGNAL_FREEPORT_STATION_INDEX);
    chain_log_health_set(st, CHAIN_HEALTH_FRESH, false,
                         0, NULL, "fresh chain; not verified yet");

    if (w->next_station_id == 0) w->next_station_id = 1;
    st->id = w->next_station_id++;
    snprintf(st->name, sizeof(st->name), "%s", "Blackglass Freeport");
    st->pos = v2(1200.0f, 11000.0f);
    st->radius = 34.0f;
    st->dock_radius = 220.0f;
    st->signal_range = 0.0f;
    st->base_price[COMMODITY_FERRITE_INGOT] = 18.0f;
    st->base_price[COMMODITY_CUPRITE_INGOT] = 20.0f;
    st->base_price[COMMODITY_CRYSTAL_INGOT] = 24.0f;
    st->base_price[COMMODITY_FRAME] = 8.0f;
    st->base_price[COMMODITY_LASER_MODULE] = 32.0f;
    st->base_price[COMMODITY_TRACTOR_MODULE] = 34.0f;
    st->base_price[COMMODITY_REPAIR_KIT] = 1.0f;
    add_module_at(st, MODULE_DOCK, 1, 0);
    st->arm_count = 1;
    rebuild_station_services(st);
    snprintf(st->station_slug, sizeof(st->station_slug), "blackglass");
    snprintf(st->currency_name, sizeof(st->currency_name), "blackglass chits");
    station_faction_seed_station(st, SIGNAL_FREEPORT_STATION_INDEX);
    snprintf(st->hail_message, sizeof(st->hail_message),
             "Blackglass Freeport. No relay, no questions. Cargo bought as-is.");
    snprintf(st->rati_hail_message, sizeof(st->rati_hail_message),
             "Blackglass recognizes the mark. Keep it off the open relay.");
    if (w->station_count <= SIGNAL_FREEPORT_STATION_INDEX)
        w->station_count = SIGNAL_FREEPORT_STATION_INDEX + 1;
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
 * Voice direction lives in client/station_voice.h; tone here matches the
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
        "Kepler Yard, machinist on duty. Frames pressing on schedule. Corridor work takes priority.",
        "Foreman Kepler used to tell apprentices: a relay is just a promise with mass.",
        "The Helios corridor failed in sections. The logs say damaged. The crews said deliberate.",
        "You shouldn't be hearing this. Yard signal is bleeding into the old booster line.",
    },
    /* Helios Works (2) — ambitious, enthusiastic, "we" meaning "I". */
    {
        "Helios Works. Prestige fabrication. Corridor degraded; docking lane remains live.",
        "The Director walked the smelting floor at dawn. She left a coin on the cold furnace.",
        "The corridor was safe when the boosters stacked. Now Blackglass hears the gaps first.",
        "We received a transmission from the broken relay. The signature was authentic. The author is dead.",
    },
};

/* Seed a station's chain log with the initial hail-message OPERATOR_POST
 * plus four rarity-tier events. Each tier event carries real flavor text
 * from DEFAULT_STATION_TIER_TEXT — the SHA-256 in the chain header binds
 * to actual content, so a verifier walking the log can prove a station
 * authored a specific tier message at a specific tick.
 *
 * Non-root stations emit the hail message but skip the tier events:
 * there's no authored content for off-relay ports or outposts yet. */
static void seed_station_motd_chain_events(world_t *w, station_t *st,
                                           int station_idx) {
    emit_operator_post(w, st, 0 /* HAIL_MOTD */, 0,
                       st->hail_message, (int)strlen(st->hail_message));
    if (station_idx < 0 || station_idx >= SIGNAL_ROOT_STATION_COUNT) return;
    for (int tier_idx = 0; tier_idx < 4; tier_idx++) {
        const char *text = DEFAULT_STATION_TIER_TEXT[station_idx][tier_idx];
        emit_operator_post(w, st, 2 /* RARITY_TIER */,
                           (uint8_t)tier_idx,
                           text,
                           (int)strlen(text));
    }
}

/* Genesis MOTD + tier events for the seeded stations. Caller must
 * invoke this only on a fresh world (no save loaded), AFTER world_reset
 * has set up station_authority. Calling it on a resumed world would
 * append duplicate genesis events to an already-extended chain. */
void world_seed_station_chain_genesis(world_t *w) {
    int n = w->station_count < SIGNAL_ROOT_STATION_COUNT
        ? w->station_count
        : SIGNAL_ROOT_STATION_COUNT;
    for (int s = 0; s < n; s++) {
        station_t *st = &w->stations[s];
        seed_station_motd_chain_events(w, st, s);
        chain_log_health_set(st,
                             st->chain_event_count > 0
                                 ? CHAIN_HEALTH_OK
                                 : CHAIN_HEALTH_EMPTY,
                             false, st->chain_event_count,
                             st->chain_last_hash,
                             st->chain_event_count > 0
                                 ? "fresh chain genesis authored"
                                 : "fresh chain has no events");
    }
}

static void world_seed_genesis_ship_assets(world_t *w) {
    if (!w) return;
    if (w->next_ship_asset_id == SHIP_ASSET_ID_NONE)
        w->next_ship_asset_id = 1;

    (void)world_ship_asset_mint(
        w, HULL_CLASS_NPC_MINER, SHIP_ASSET_OWNER_STATION,
        0, 0, SHIP_ASSET_PROVENANCE_GENESIS, false, 0, NULL, NULL);
    (void)world_ship_asset_mint(
        w, HULL_CLASS_DRONE_TRACTOR, SHIP_ASSET_OWNER_STATION,
        0, 0, SHIP_ASSET_PROVENANCE_GENESIS, false, 0, NULL, NULL);
    (void)world_ship_asset_mint(
        w, HULL_CLASS_DRONE_TRACTOR, SHIP_ASSET_OWNER_STATION,
        1, 1, SHIP_ASSET_PROVENANCE_GENESIS, false, 1, NULL, NULL);
    (void)world_ship_asset_mint(
        w, HULL_CLASS_NPC_MINER, SHIP_ASSET_OWNER_STATION,
        2, 2, SHIP_ASSET_PROVENANCE_GENESIS, false, 2, NULL, NULL);
    (void)world_ship_asset_mint(
        w, HULL_CLASS_DRONE_TRACTOR, SHIP_ASSET_OWNER_STATION,
        2, 2, SHIP_ASSET_PROVENANCE_GENESIS, false, 2, NULL, NULL);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        (void)world_ship_asset_mint(
            w, HULL_CLASS_MINER, SHIP_ASSET_OWNER_STATION,
            0, 0, SHIP_ASSET_PROVENANCE_GENESIS, true, 0, NULL, NULL);
    }
    for (int s = 1; s < SIGNAL_ROOT_STATION_COUNT; s++) {
        for (int i = 0; i < 2; i++) {
            (void)world_ship_asset_mint(
                w, HULL_CLASS_MINER, SHIP_ASSET_OWNER_STATION,
                s, s, SHIP_ASSET_PROVENANCE_GENESIS, true, s, NULL, NULL);
        }
    }
}

void world_reset(world_t *w) {
    uint32_t seed = w->rng;  /* caller may pre-set seed; 0 = default */
    float *sig_buf = w->signal_cache.strength; /* preserve heap allocation */
    sparse_cell_entry_t *grid_entries = w->asteroid_grid.entries;
    for (int i = 0; i < MAX_PLAYERS; i++)
        ship_cleanup(&w->players[i].ship);
    for (int i = 0; i < MAX_SHIP_ASSETS; i++)
        ship_cleanup(&w->ship_assets[i].ship);
    for (int i = 0; i < MAX_SHIPS; i++)
        ship_cleanup(&w->ships[i]);
    for (int i = 0; i < MAX_STATIONS; i++)
        station_cleanup(&w->stations[i]);
    free(grid_entries);
    memset(w, 0, sizeof(*w));
    w->signal_cache.strength = sig_buf; /* restore — signal_grid_build reuses it */
    /* Caller-supplied non-zero seed → reproducible (test fixtures, save
     * load with persisted belt_seed). The server's load_world_state()
     * pre-stamps a fresh wall-clock seed before calling world_reset on
     * a clean boot, so production sees a new belt_seed every restart;
     * tests get the deterministic 2037 fallback. The chain log replay
     * (highscore_replay_from_chain) treats prior worlds' on-disk logs
     * as orphans and rebuilds the leaderboard view from them, so
     * rotation never loses history. */
    w->rng = seed ? seed : 2037u;
    w->belt_seed = w->rng;  /* anchor for rock_pub derivation (#285) */
    w->next_delivery_shipment_id = 1;
    /* Wipe process-level nav scratch so a freshly-reset world doesn't
     * inherit stale path/nav-mesh state from a previously-run world.
     * Matters for test isolation when many world_t instances are reset
     * back-to-back in the same process. */
    nav_caches_reset();
    belt_field_init(&w->belt, w->rng, BELT_SCALE);
    for (int i = 0; i < MAX_STATIONS; i++)
        (void)station_manifest_bootstrap(&w->stations[i]);

    /* --- Seeded-station identity (Layer B of #479) ---
     * Derive deterministic Ed25519 keypairs for seeded stations from
     * the world seed *before* any other identity logic runs, so
     * subsequent code (catalog save, signal_chain bootstrap, etc.) sees
     * stations with stable pubkeys. New seed → new pubkeys
     * → new chain log filenames; previous worlds' logs survive on
     * disk under their old pubkeys and feed the highscore replay. */
    for (int s = 0; s < SIGNAL_SEEDED_STATION_COUNT; s++) {
        station_authority_init_seeded(&w->stations[s], w->belt_seed,
                                       (uint32_t)s);
        chain_log_health_set(&w->stations[s], CHAIN_HEALTH_FRESH, false,
                             0, NULL, "fresh chain; not verified yet");
    }

    /* In-memory chain state is implicitly zero from the memset above.
     * Do NOT delete chain log files here — world_reset is called as
     * part of every load path before the saved belt_seed is restored,
     * so deleting the current-pubkey log file would clobber the saved
     * game's history. Fresh-world setup (true first boot, test
     * fixtures resetting state) calls world_chain_logs_reset()
     * explicitly after the seed is settled. */

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
    w->stations[0].base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_INGOT] = 12.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_INGOT] = 14.0f;
    w->stations[0].base_price[COMMODITY_REPAIR_KIT] = 1.0f;
    /* Finished-good price baselines if Prospect receives stock; its dock
     * imports repair kits rather than shipyard kit-fab inputs. */
    w->stations[0].base_price[COMMODITY_FRAME]          = 2.0f;
    w->stations[0].base_price[COMMODITY_LASER_MODULE]   = 16.0f;
    w->stations[0].base_price[COMMODITY_TRACTOR_MODULE] = 18.0f;
    w->stations[0].signal_range = 9000.0f;
    /* Ring 1: dock + relay + ferrite furnace (tagged FERRITE_INGOT). */
    add_module_at(&w->stations[0], MODULE_DOCK,         1, 0);
    add_module_at(&w->stations[0], MODULE_SIGNAL_RELAY, 1, 1);
    add_furnace_for(&w->stations[0], 1, 2, COMMODITY_FERRITE_INGOT);
    /* Ring 2: ferrite-ore intake at slot 4 (240°, cross-ring opposite
     * the furnace at ring 1 slot 2). Folded frame pods are packaging stock:
     * the furnace tractors them directly as shell supply, so Prospect does
     * not need a separate visible frame hopper. */
    add_hopper_for(&w->stations[0], 2, 4, COMMODITY_FERRITE_ORE);
    w->stations[0].arm_count = 3;
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
    station_faction_seed_station(&w->stations[0], 0);
    snprintf(w->stations[0].hail_message, sizeof(w->stations[0].hail_message),
             "Prospect Refinery. Inner basin smelting. Tow ferrite to the furnace.");
    snprintf(w->stations[0].miner_chatter[0], sizeof(w->stations[0].miner_chatter[0]), "Prospect says keep it small.");
    snprintf(w->stations[0].miner_chatter[1], sizeof(w->stations[0].miner_chatter[1]), "Furnace wants ferrite.");
    snprintf(w->stations[0].hauler_chatter[0], sizeof(w->stations[0].hauler_chatter[0]), "Prospect load secured.");
    snprintf(w->stations[0].hauler_chatter[1], sizeof(w->stations[0].hauler_chatter[1]), "Tow line clean.");
    snprintf(w->stations[0].rati_hail_message, sizeof(w->stations[0].rati_hail_message),
             "RATi-grade ore confirmed. Prospect has your signature on the log.");

    w->stations[1].id = w->next_station_id++;
    snprintf(w->stations[1].name, sizeof(w->stations[1].name), "%s", "Kepler Yard");
    w->stations[1].pos         = v2(0.0f, 2400.0f);
    w->stations[1].radius      = 36.0f;
    w->stations[1].dock_radius = 240.0f;
    w->stations[1].signal_range = 8500.0f;
    /* Smelt-payout floor (see Prospect comment above). */
    w->stations[1].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[1].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[1].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[1].base_price[COMMODITY_FERRITE_INGOT] = 10.0f;
    w->stations[1].base_price[COMMODITY_FRAME] = 2.0f;
    w->stations[1].base_price[COMMODITY_REPAIR_KIT] = 1.0f;
    /* Kepler imports laser/tractor modules for its shipyard kit fab. */
    w->stations[1].base_price[COMMODITY_LASER_MODULE]   = 16.0f;
    w->stations[1].base_price[COMMODITY_TRACTOR_MODULE] = 18.0f;
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
    /* Ring 3: ferrite-ingot hopper feeding the frame press, plus the
     * second shipyard gantry that unlocks station-module scaffold
     * fabrication. A single yard is enough for hull commissions; two
     * active yards mean Kepler can build external station modules. */
    add_hopper_for(&w->stations[1], 3, 0, COMMODITY_FERRITE_INGOT);
    add_module_at(&w->stations[1], MODULE_SHIPYARD, 3, 4);
    w->stations[1].arm_count = 3;
    w->stations[1].arm_speed[1] = STATION_RING_SPEED; /* ring 2 drift bias */
    rebuild_station_services(&w->stations[1]);
    snprintf(w->stations[1].station_slug, sizeof(w->stations[1].station_slug), "kepler");
    snprintf(w->stations[1].currency_name, sizeof(w->stations[1].currency_name), "kepler bonds");
    station_faction_seed_station(&w->stations[1], 1);
    snprintf(w->stations[1].hail_message, sizeof(w->stations[1].hail_message),
             "Kepler Yard. Fabrication and scaffold kits. Rebuild the Helios corridor.");
    snprintf(w->stations[1].miner_chatter[0], sizeof(w->stations[1].miner_chatter[0]), "Kepler wants clean ingots.");
    snprintf(w->stations[1].miner_chatter[1], sizeof(w->stations[1].miner_chatter[1]), "Frame stock running low.");
    snprintf(w->stations[1].hauler_chatter[0], sizeof(w->stations[1].hauler_chatter[0]), "Frames inbound.");
    snprintf(w->stations[1].hauler_chatter[1], sizeof(w->stations[1].hauler_chatter[1]), "Yard manifest updated.");
    snprintf(w->stations[1].rati_hail_message, sizeof(w->stations[1].rati_hail_message),
             "That RATi ore just made the yard stop talking. Logged and paid.");

    w->stations[2].id = w->next_station_id++;
    snprintf(w->stations[2].name, sizeof(w->stations[2].name), "%s", "Helios Works");
    w->stations[2].pos         = v2(0.0f, 15000.0f);
    w->stations[2].radius      = 36.0f;
    w->stations[2].dock_radius = 240.0f;
    /* Helios used to sit at the far end of a boosted relay corridor.
     * The local station still works, but the intervening boosters are
     * gone, leaving a low-signal run through Blackglass territory. */
    w->stations[2].signal_range = 4500.0f;
    /* Smelt-payout floor (see Prospect comment above). */
    w->stations[2].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_INGOT] = 12.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_INGOT] = 14.0f;
    w->stations[2].base_price[COMMODITY_LASER_MODULE] = 16.0f;
    w->stations[2].base_price[COMMODITY_TRACTOR_MODULE] = 18.0f;
    w->stations[2].base_price[COMMODITY_REPAIR_KIT] = 1.0f;
    /* Helios imports frames for its shipyard kit fab. */
    w->stations[2].base_price[COMMODITY_FRAME]          = 2.0f;
    /* No ferrite ingots produced or imported here. Helios specializes in
     * cuprite plus the two-pass crystal process; the ferrite-ingot
     * pipeline stays Prospect's. */
    w->stations[2].base_price[COMMODITY_FERRITE_INGOT]  = 0.0f;
    /* Producers spread across all three rings; commodity-tagged
     * hoppers feed them all. */
    /* Ring 1: dock + relay + first crystal furnace. Crystal now takes
     * two crystal furnace passes: raw crystal becomes an intermediate
     * fragment here, then the other crystal furnace finishes it. */
    add_module_at(&w->stations[2], MODULE_DOCK,         1, 0);
    add_module_at(&w->stations[2], MODULE_SIGNAL_RELAY, 1, 1);
    add_furnace_for(&w->stations[2], 1, 2, COMMODITY_CRYSTAL_INGOT);
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
     * and shipyard. The second shipyard gantry gives Helios enough
     * industrial capacity to fabricate station-module scaffolds under
     * the two-yard rule. The two crystal furnaces share the ring-2 crystal
     * ore intake; the ring-3 cuprite furnace uses the cuprite intake. */
    add_module_at(&w->stations[2], MODULE_SHIPYARD, 3, 1);
    add_hopper_for(&w->stations[2], 3, 2, COMMODITY_LASER_MODULE);   /* LASER_FAB output + shipyard input */
    add_hopper_for(&w->stations[2], 3, 3, COMMODITY_FRAME);          /* feeds SHIPYARD */
    add_furnace_for(&w->stations[2],   3, 4, COMMODITY_CRYSTAL_INGOT);
    add_hopper_for(&w->stations[2], 3, 5, COMMODITY_CRYSTAL_INGOT);
    add_furnace_for(&w->stations[2],   3, 6, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(&w->stations[2], 3, 7, COMMODITY_TRACTOR_MODULE); /* TRACTOR_FAB output + shipyard input */
    w->stations[2].arm_count = 3;
    w->stations[2].arm_speed[0] = STATION_RING_SPEED;
    w->stations[2].arm_speed[1] = STATION_RING_SPEED;
    w->stations[2].arm_speed[2] = STATION_RING_SPEED;
    rebuild_station_services(&w->stations[2]);
    snprintf(w->stations[2].station_slug, sizeof(w->stations[2].station_slug), "helios");
    snprintf(w->stations[2].currency_name, sizeof(w->stations[2].currency_name), "helios credits");
    station_faction_seed_station(&w->stations[2], 2);
    snprintf(w->stations[2].hail_message, sizeof(w->stations[2].hail_message),
             "Helios Works. Advanced smelting beyond the broken corridor.");
    snprintf(w->stations[2].miner_chatter[0], sizeof(w->stations[2].miner_chatter[0]), "Helios wants premium ore.");
    snprintf(w->stations[2].miner_chatter[1], sizeof(w->stations[2].miner_chatter[1]), "Corridor signal is still failing.");
    snprintf(w->stations[2].hauler_chatter[0], sizeof(w->stations[2].hauler_chatter[0]), "Helios shipment bright.");
    snprintf(w->stations[2].hauler_chatter[1], sizeof(w->stations[2].hauler_chatter[1]), "Crossing Blackglass gap.");
    snprintf(w->stations[2].rati_hail_message, sizeof(w->stations[2].rati_hail_message),
             "RATi-grade delivery received. Helios will remember the callsign.");

    world_ensure_seeded_freeport(w);

    /* Genesis MOTD + tier events used to be emitted here, but doing so
     * on every world_reset corrupted the chain log: load_world_state
     * calls world_reset BEFORE world_load restores the saved seed, so
     * MOTDs landed at the default-2037 station pubkey's file with
     * prev_hash=0 every restart, breaking that chain forever. Seeding
     * is now triggered explicitly on the fresh-world boot path
     * (load_world_state and local_server_init) via
     * world_seed_station_chain_genesis. */

    rebuild_signal_chain(w);

    /* --- Initial asteroid field: materialize terrain chunks near stations --- */
    {
        int slot = 0;
        int budget = FIELD_ASTEROID_TARGET; /* leave headroom for fracture children */
        for (int s = 0; s < SIGNAL_ROOT_STATION_COUNT && slot < budget; s++) {
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

    /* --- NPC ships: seed resident station drones.
     * Prospect and Helios get one LM miner plus one TM tug because they
     * have raw-ore work. Kepler is a yard, so it starts with only the tug;
     * seeding a miner there leaves a visible idle hull with no ore loop. */
    /* Seed the starter industrial roster immediately. The respawn loop
     * drip-fills losses later, but reset must start with the documented
     * inter-station chain online so tests and fresh worlds do not wait
     * through staggered replacement timers. */
    {
        world_seed_genesis_ship_assets(w);
        (void)ship_asset_claim_for_npc(w, 0, NPC_ROLE_MINER);
        (void)ship_asset_claim_for_npc(w, 0, NPC_ROLE_TOW);
        (void)ship_asset_claim_for_npc(w, 1, NPC_ROLE_TOW);
        (void)ship_asset_claim_for_npc(w, 2, NPC_ROLE_MINER);
        (void)ship_asset_claim_for_npc(w, 2, NPC_ROLE_TOW);
    }

    world_seed_kepler_frame_pod(w);
    world_seed_prospect_frame_shell_pod(w);

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

    /* Cold-start local gossip bootstrap — see gossip.h for the
     * long-form rationale. This refreshes station-local pressure
     * without broadcasting peer-station contracts. */
    gossip_bootstrap_world_stations(w);

    SIM_LOG("[sim] world reset complete (%d asteroids, starter NPC roster seeded)\n",
            FIELD_ASTEROID_TARGET);
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

bool server_player_can_use_pubkey_persistence(const server_player_t *sp) {
    if (!sp) return false;
    return sp->session_ready &&
           sp->pubkey_set &&
           sp->pubkey_proof_ok &&
           !pubkey_is_zero(sp->pubkey);
}

bool server_finalize_pubkey_identity(world_t *w, int player_idx) {
    if (!w || player_idx < 0 || player_idx >= MAX_PLAYERS) return false;
    server_player_t *sp = &w->players[player_idx];
    if (!server_player_can_use_pubkey_persistence(sp)) return false;
    (void)registry_register_pubkey(w, sp->pubkey, sp->session_token);
    sp->pubkey_identity_finalized = true;
    return true;
}

bool server_parse_session_message(const uint8_t *data, int len,
                                  server_session_message_t *out) {
    if (!data || !out || len < 9) return false;
    if (data[0] != NET_MSG_SESSION) return false;
    memset(out, 0, sizeof(*out));
    memcpy(out->token, &data[1], 8);
    if (len >= 16) {
        memcpy(out->callsign, &data[9], 7);
        out->callsign[7] = '\0';
        out->has_callsign = true;
    }
    return true;
}

bool server_apply_session_message(world_t *w, int player_idx,
                                  const server_session_message_t *msg) {
    if (!w || !msg || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    server_player_t *sp = &w->players[player_idx];
    if (sp->session_ready) return false;
    memcpy(sp->session_token, msg->token, 8);
    sp->session_ready = true;
    if (msg->has_callsign) {
        memcpy(sp->callsign, msg->callsign, sizeof(sp->callsign));
        sp->callsign[sizeof(sp->callsign) - 1] = '\0';
    }
    sp->last_input_action_id = 0;
    sp->last_input_action_id_valid = false;
    sp->pending_action_result_valid = false;
    server_player_clear_transient_input(sp);
    return true;
}

bool server_dispatch_register_pubkey_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_pubkey_register_result_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    if (len < REGISTER_PUBKEY_MSG_SIZE || data[0] != NET_MSG_REGISTER_PUBKEY)
        return false;

    const uint8_t *pk = &data[1];
    server_player_t *sp = &w->players[player_idx];
    bool same_pubkey = sp->pubkey_set && memcmp(sp->pubkey, pk, 32) == 0;
    if (out) {
        out->accepted = true;
        out->same_pubkey = same_pubkey;
        memcpy(out->pubkey, pk, 32);
    }
    if (same_pubkey) return true;

    memcpy(sp->pubkey, pk, 32);
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = false;
    sp->pubkey_identity_finalized = false;
    return true;
}

bool server_dispatch_pubkey_proof_message(
    world_t *w,
    int player_idx,
    const uint8_t *data,
    int len,
    server_pubkey_proof_result_t *out) {
    if (out) {
        out->status = SERVER_PUBKEY_PROOF_MALFORMED;
        out->verified = false;
    }
    if (!w || !data || player_idx < 0 || player_idx >= MAX_PLAYERS)
        return false;
    if (len < PROVE_PUBKEY_MSG_SIZE || data[0] != NET_MSG_PROVE_PUBKEY)
        return false;

    server_player_t *sp = &w->players[player_idx];
    const uint8_t *pk = &data[PROVE_PUBKEY_PUBKEY_OFFSET];
    const uint8_t *token = &data[PROVE_PUBKEY_TOKEN_OFFSET];
    const uint8_t *sig = &data[PROVE_PUBKEY_SIG_OFFSET];
    server_pubkey_proof_status_t status = SERVER_PUBKEY_PROOF_OK;

    if (!sp->pubkey_set || !sp->session_ready) {
        status = SERVER_PUBKEY_PROOF_NO_REGISTRATION;
    } else if (memcmp(pk, sp->pubkey, 32) != 0) {
        status = SERVER_PUBKEY_PROOF_PUBKEY_MISMATCH;
    } else if (memcmp(token, sp->session_token, 8) != 0) {
        status = SERVER_PUBKEY_PROOF_SESSION_MISMATCH;
    } else if (!pubkey_proof_verify(pk, token, sig)) {
        status = SERVER_PUBKEY_PROOF_BAD_SIGNATURE;
    }

    if (status == SERVER_PUBKEY_PROOF_OK) {
        sp->pubkey_proof_ok = true;
    }
    if (out) {
        out->status = status;
        out->verified = status == SERVER_PUBKEY_PROOF_OK;
    }
    return true;
}

const char *server_pubkey_proof_status_name(
    server_pubkey_proof_status_t status) {
    switch (status) {
    case SERVER_PUBKEY_PROOF_OK: return "ok";
    case SERVER_PUBKEY_PROOF_MALFORMED: return "malformed";
    case SERVER_PUBKEY_PROOF_NO_REGISTRATION: return "no-registration";
    case SERVER_PUBKEY_PROOF_PUBKEY_MISMATCH: return "pubkey-mismatch";
    case SERVER_PUBKEY_PROOF_SESSION_MISMATCH: return "session-mismatch";
    case SERVER_PUBKEY_PROOF_BAD_SIGNATURE: return "bad-signature";
    default: return "unknown";
    }
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
    if (!server_player_can_use_pubkey_persistence(sp))
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

void server_player_clear_transient_input(server_player_t *sp) {
    if (!sp) return;
    sp->input = (input_intent_t){
        .service_sell_only = COMMODITY_COUNT,
        .service_sell_grade = MINING_GRADE_COUNT,
        .place_target_station = -1,
        .place_target_ring = -1,
        .place_target_slot = -1,
        .plan_station = -1,
        .plan_ring = -1,
        .plan_slot = -1,
        .cancel_planned_station = -1,
        .cancel_plan_st = -1,
        .cancel_plan_ring = -1,
        .cancel_plan_sl = -1,
        .buy_grade = MINING_GRADE_COUNT,
        .mining_target_hint = -1,
    };
    memset(sp->movement_queue, 0, sizeof(sp->movement_queue));
    sp->movement_queue_count = 0;
    sp->last_input_seq = 0;
    sp->last_input_tick = 0;
    sp->boost_hold_timer = 0.0f;
    sp->actual_thrusting = false;
    sp->docking_approach = false;
    sp->beam_active = false;
    sp->beam_hit = false;
    sp->beam_ineffective = false;
    sp->scan_active = false;
    sp->scan_target_type = 0;
    sp->scan_target_index = -1;
    sp->scan_module_index = -1;
    sp->ship.tractor_active = false;
}

void player_init_ship(server_player_t *sp, world_t *w) {
    if (!sp) return;
    int player_slot = player_slot_for_ptr(w, sp);
    uint32_t prior_asset_id = sp->ship_asset_id;
    ship_cleanup(&sp->ship);
    memset(&sp->ship, 0, sizeof(sp->ship));
    (void)ship_manifest_bootstrap(&sp->ship);
    server_player_clear_transient_input(sp);
    sp->ship_asset_id = prior_asset_id;
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
    sp->autopilot_station_target = -1;
    sp->autopilot_cargo = COMMODITY_COUNT;
    sp->autopilot_timer = 0.0f;

    if (player_slot >= 0 && ship_asset_claim_for_player(w, player_slot, 0)) {
        return;
    }

    if (player_slot >= 0) {
        sp->ship.hull_class = HULL_CLASS_MINER;
        sp->ship.hull = 0.0f;
        sp->ship.angle = PI_F * 0.5f;
        sp->ship.comm_range = 1500.0f;
        memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
        memset(sp->ship.towed_pods, -1, sizeof(sp->ship.towed_pods));
        sp->ship.towed_scaffold = -1;
        anchor_ship_in_station(sp, w);
        return;
    }

    sp->ship.hull_class = HULL_CLASS_MINER;
    sp->ship.hull       = hull_max_for_class(HULL_CLASS_MINER);
    sp->ship.angle      = PI_F * 0.5f;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
    memset(sp->ship.towed_pods, -1, sizeof(sp->ship.towed_pods));
    sp->ship.towed_scaffold = -1;
    sp->ship.tractor_active = false;  /* driven by tractor_hold each frame */
    sp->ship.comm_range     = 1500.0f; /* H-ping reach — roughly one screen */
    anchor_ship_in_station(sp, w);
    /* Stack-only harness players are not backed by world player slots,
     * so they keep the old direct bootstrap. Real players must bind a
     * durable ship_asset_t above. */
    if (w && sp->docked && sp->current_station >= 0 &&
        sp->current_station < MAX_STATIONS) {
        gossip_dock_handshake(w, sp->current_station,
                              sp->ship.known_contracts,
                              &sp->ship.known_contract_count,
                              SHIP_KNOWN_CONTRACT_CAP,
                              &sp->ship.knowledge);
    }
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
     * Identity-aware lookup: verified-pubkey players match the
     * full 32-byte pubkey entry (the same one their earnings credit
     * to). Legacy session-token players match the SHA256-of-token
     * pseudokey via the existing ledger_balance shim. The OLD code
     * here did `memcmp(ledger.player_pubkey, session_token, 8)` —
     * comparing the first 8 bytes of a 32-byte sha256 against the
     * raw session token, which never matches even for legacy
     * players, so the fee was charged on every reconnect. */
    if (server_player_can_use_pubkey_persistence(sp)) {
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
