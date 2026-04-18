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
#include "sim_ai.h"
#include "sim_autopilot.h"
#include "sim_nav.h"
#include "sim_asteroid.h"
#include "sim_physics.h"
#include "sim_production.h"
#include "sim_construction.h"
#include "signal_model.h"
#include "rng.h"
#include <stdlib.h>

/* SIM_LOG moved to game_sim.h so all sim_*.c files share the same macro. */

/* --- Station-local ledger economy ---
 * All credits are per-station. There is no global wallet.
 * Players earn by smelting/delivering at a station and spend at that station.
 * Cross-station wealth transfer requires physically hauling goods. */

/* Find or create a ledger entry — forward decl (defined with other ledger code below) */
static int ledger_find_or_create(station_t *st, const uint8_t *token);

float ledger_balance(const station_t *st, const uint8_t *token) {
    for (int i = 0; i < st->ledger_count; i++)
        if (memcmp(st->ledger[i].player_token, token, 8) == 0)
            return st->ledger[i].balance;
    return 0.0f;
}

void ledger_earn(station_t *st, const uint8_t *token, float amount) {
    int idx = ledger_find_or_create(st, token);
    if (idx < 0) return;
    st->ledger[idx].balance += amount;
}

static bool ledger_spend(station_t *st, const uint8_t *token, float amount, ship_t *ship) {
    if (amount <= 0.0f) return true;
    int idx = ledger_find_or_create(st, token);
    if (idx < 0) return false;
    if (st->ledger[idx].balance + 0.01f < amount) return false;
    st->ledger[idx].balance -= amount;
    if (st->ledger[idx].balance < 0.0f) st->ledger[idx].balance = 0.0f;
    ship->stat_credits_spent += amount;
    st->credit_pool += amount;
    return true;
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
    uint32_t h = (uint32_t)((cx * 73856093) ^ (cy * 19349663));
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

/* Raw signal computation — scans all stations. Used to build the cache
 * and as fallback for positions outside the cached grid. */
static float signal_strength_raw(const world_t *w, vec2 pos) {
    float best = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float dist = sqrtf(v2_dist_sq(pos, w->stations[s].pos));
        float strength = fmaxf(0.0f, 1.0f - (dist / w->stations[s].signal_range));
        if (strength > best) best = strength;
    }
    return best;
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
    /* Must be within signal range of an existing station */
    float sig = signal_strength_at(w, pos);
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
    if (sp->ship.cargo[COMMODITY_FRAME] < 0.01f) return;
    float deliver = sp->ship.cargo[COMMODITY_FRAME];
    float needed = SCAFFOLD_MATERIAL_NEEDED * (1.0f - st->scaffold_progress);
    float accepted = fminf(deliver, needed);
    sp->ship.cargo[COMMODITY_FRAME] -= accepted;
    st->scaffold_progress += accepted / SCAFFOLD_MATERIAL_NEEDED;
    SIM_LOG("[sim] player %d delivered %.1f frames to scaffold %d (progress %.0f%%)\n",
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
    memset(s->cargo, 0, sizeof(s->cargo));
}

static uint32_t station_upgrade_service(ship_upgrade_t upgrade) {
    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  return STATION_SERVICE_UPGRADE_LASER;
    case SHIP_UPGRADE_HOLD:    return STATION_SERVICE_UPGRADE_HOLD;
    case SHIP_UPGRADE_TRACTOR: return STATION_SERVICE_UPGRADE_TRACTOR;
    default: return 0;
    }
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

static bool station_has_service(const station_t *station, uint32_t service) {
    return station && ((station->services & service) != 0);
}

static float sim_station_repair_cost(const ship_t *s) {
    float missing = fmaxf(0.0f, ship_max_hull(s) - s->hull);
    return ceilf(missing * STATION_REPAIR_COST_PER_HULL);
}

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
    emit_event(w, (sim_event_t){
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
        }
    });
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
    /* Respawn at nearest station — teleport to its dock */
    int best = 0;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_exists(&w->stations[i])) continue;
        float d = v2_dist_sq(sp->ship.pos, w->stations[i].pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    sp->current_station = best;
    sp->nearby_station = best;
    sp->dock_berth = 0;
    sp->ship.pos = dock_berth_pos(&w->stations[best], 0);
    dock_ship(w, sp);
    SIM_LOG("[sim] player %d emergency recovered at station 0\n", sp->id);
}

static void apply_ship_damage(world_t *w, server_player_t *sp, float damage) {
    if (damage <= 0.0f) return;
    sp->ship.hull = fmaxf(0.0f, sp->ship.hull - damage);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_DAMAGE, .player_id = sp->id, .damage.amount = damage});
    if (sp->ship.hull <= 0.01f) emergency_recover_ship(w, sp);
}

/* ================================================================== */
/* Ship collision                                                     */
/* ================================================================== */

static int ship_collision_count; /* per-frame overlap counter for crush detection */

/* Skin width: tiny gap between ship surface and obstacle surface after a
 * push-out. Eliminates per-tick boundary chatter on large radii (titan
 * asteroids, station modules) where small numerical drift would otherwise
 * trigger another collision the next tick. */
#define COLLISION_SKIN 1.5f

static void resolve_ship_circle(world_t *w, server_player_t *sp, vec2 center, float radius) {
    float minimum = radius + ship_hull_def(&sp->ship)->ship_radius;
    vec2 delta = v2_sub(sp->ship.pos, center);
    float d_sq = v2_len_sq(delta);
    if (d_sq >= minimum * minimum) return;
    float d = sqrtf(d_sq);
    vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
    /* Push past the surface by the skin width so we're cleanly outside. */
    sp->ship.pos = v2_add(center, v2_scale(normal, minimum + COLLISION_SKIN));
    float vel_toward = v2_dot(sp->ship.vel, normal);
    if (vel_toward < 0.0f) {
        float impact = -vel_toward;
        if (!sp->docked && impact > SHIP_COLLISION_DAMAGE_THRESHOLD)
            apply_ship_damage(w, sp, (impact - SHIP_COLLISION_DAMAGE_THRESHOLD) * SHIP_COLLISION_DAMAGE_SCALE);
        /* Clamp inward velocity component to zero — slide along the surface
         * tangent on the next tick instead of bouncing back through it. */
        sp->ship.vel = v2_sub(sp->ship.vel, v2_scale(normal, vel_toward));
    }
    ship_collision_count++;
}

/* Annular sector collision: test ship against the corridor band exactly
 * matching the visual arc. No ghost walls — geometry is pixel-exact. */
static void resolve_ship_annular_sector(world_t *w, server_player_t *sp,
                                         vec2 center, float ring_r,
                                         float angle_a, float angle_b) {
    float ship_r = ship_hull_def(&sp->ship)->ship_radius;
    vec2 delta = v2_sub(sp->ship.pos, center);
    float dist = sqrtf(v2_len_sq(delta));
    if (dist < 1.0f) return;

    /* Radial test: ship within inflated band? */
    float r_inner = ring_r - STATION_CORRIDOR_HW - ship_r;
    float r_outer = ring_r + STATION_CORRIDOR_HW + ship_r;
    if (dist <= r_inner || dist >= r_outer) return;

    /* Angular test: ship angle within the arc?
     * Expand the arc by the ship's angular footprint at this radius
     * so ships near the arc edge don't clip through. */
    float ship_angle = atan2f(delta.y, delta.x);
    float angular_margin = (dist > 1.0f) ? asinf(fminf(ship_r / dist, 1.0f)) : 0.0f;
    float da = angle_b - angle_a;
    while (da > PI_F) da -= TWO_PI_F;
    while (da < -PI_F) da += TWO_PI_F;
    /* Expand arc by angular margin on both sides */
    float expanded_start = angle_a - (da > 0 ? angular_margin : -angular_margin);
    float expanded_da = da + (da > 0 ? 2.0f : -2.0f) * angular_margin;
    if (angle_in_arc(ship_angle, expanded_start, expanded_da) < 0.0f) return;

    /* Ship is inside corridor — push radially to nearest edge.
     * Add COLLISION_SKIN past the edge so the next frame's tangential
     * slide doesn't immediately re-trigger this same corridor. */
    vec2 radial = v2_scale(delta, 1.0f / dist);
    float d_inner = dist - (ring_r - STATION_CORRIDOR_HW);
    float d_outer = (ring_r + STATION_CORRIDOR_HW) - dist;
    vec2 push_normal;
    if (d_inner < d_outer) {
        sp->ship.pos = v2_add(center, v2_scale(radial, ring_r - STATION_CORRIDOR_HW - ship_r - COLLISION_SKIN));
        push_normal = v2_scale(radial, -1.0f);
    } else {
        sp->ship.pos = v2_add(center, v2_scale(radial, ring_r + STATION_CORRIDOR_HW + ship_r + COLLISION_SKIN));
        push_normal = radial;
    }

    float vel_toward = v2_dot(sp->ship.vel, push_normal);
    if (vel_toward < 0.0f) {
        float impact = -vel_toward;
        if (!sp->docked && impact > SHIP_COLLISION_DAMAGE_THRESHOLD)
            apply_ship_damage(w, sp, (impact - SHIP_COLLISION_DAMAGE_THRESHOLD) * SHIP_COLLISION_DAMAGE_SCALE);
        sp->ship.vel = v2_sub(sp->ship.vel, v2_scale(push_normal, vel_toward * 1.0f));
    }
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

static void try_sell_station_cargo(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    float payout = 0.0f;
    /* Optional one-shot filter: if the client requested selective
     * delivery via NET_ACTION_DELIVER_COMMODITY, only commodities
     * matching `filter` are delivered. COMMODITY_COUNT (the default)
     * means "deliver everything that fits", which is the legacy
     * "deliver all" behavior triggered by NET_ACTION_SELL_CARGO. */
    commodity_t filter = sp->input.service_sell_only;
    bool selective = (filter < COMMODITY_COUNT);

    /* Deliver any cargo matching active supply contracts at this station */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->action != CONTRACT_TRACTOR) continue;
        if (ct->station_index != sp->current_station) continue;
        commodity_t c = ct->commodity;
        if (selective && filter != c) continue;
        if (sp->ship.cargo[c] < 0.01f) continue;
        float capacity = (c < COMMODITY_RAW_ORE_COUNT)
            ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
        float space = fmaxf(0.0f, capacity - st->inventory[c]);
        if (space < 0.01f) continue;
        float deliver = fminf(fminf(sp->ship.cargo[c], ct->quantity_needed), space);
        payout += deliver * contract_price(ct);
        sp->ship.cargo[c] -= deliver;
        st->inventory[c] += deliver;
        ct->quantity_needed -= deliver;
        if (ct->quantity_needed <= 0.01f) {
            /* Don't close if scaffold modules still need this material */
            bool scaffold_still_needs = false;
            for (int m2 = 0; m2 < st->module_count; m2++) {
                if (st->modules[m2].scaffold && st->modules[m2].build_progress < 1.0f
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

    /* Fallback: deliver the station's primary input commodity at base price,
     * even without an active contract. Fab stations always accept their input
     * (e.g. frame press accepts ferrite ingots). */
    commodity_t buy = station_primary_buy(st);
    if ((int)buy >= 0 && sp->ship.cargo[buy] > 0.01f &&
        (!selective || filter == buy)) {
        float capacity = MAX_PRODUCT_STOCK;
        float space = fmaxf(0.0f, capacity - st->inventory[buy]);
        if (space > 0.01f) {
            float accepted = fminf(sp->ship.cargo[buy], space);
            float price = station_buy_price(st, buy);
            payout += accepted * price;
            sp->ship.cargo[buy] -= accepted;
            st->inventory[buy] += accepted;
        }
    }

    if (payout > 0.01f) {
        /* Pay from station credit pool — cap at what station can afford */
        if (payout > st->credit_pool) payout = st->credit_pool;
        if (payout > 0.01f) {
            st->credit_pool -= payout;
            ledger_earn(st, sp->session_token, payout);
            sp->ship.stat_credits_earned += payout;
            SIM_LOG("[sim] player %d sold cargo for %.0f cr at %s\n", sp->id, payout, st->name);
            emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = sp->id});
        }
    }
    /* Clear the one-shot filter so the next plain SELL_CARGO press
     * resumes the default "deliver all" behavior. */
    sp->input.service_sell_only = COMMODITY_COUNT;
}

static void try_repair_ship(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    if (!station_has_service(st, STATION_SERVICE_REPAIR)) return;
    float cost = sim_station_repair_cost(&sp->ship);
    if (cost <= 0.0f) return;
    if (!ledger_spend(st, sp->session_token, cost, &sp->ship)) return;
    sp->ship.hull = ship_max_hull(&sp->ship);
    SIM_LOG("[sim] player %d repaired for %.0f cr\n", sp->id, cost);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_REPAIR, .player_id = sp->id});
}

static void try_apply_ship_upgrade(world_t *w, server_player_t *sp, ship_upgrade_t upgrade) {
    station_t *st = &w->stations[sp->current_station];
    uint32_t req_svc = station_upgrade_service(upgrade);
    if (!station_has_service(st, req_svc)) return;
    if (ship_upgrade_maxed(&sp->ship, upgrade)) return;

    product_t required = upgrade_required_product(upgrade);
    float pcost = upgrade_product_cost(&sp->ship, upgrade);
    if (st->inventory[COMMODITY_FRAME + required] < pcost - 0.01f) return;
    int cost = ship_upgrade_cost(&sp->ship, upgrade);
    if (!ledger_spend(st, sp->session_token, (float)cost, &sp->ship)) return;
    st->inventory[COMMODITY_FRAME + required] -= pcost;

    switch (upgrade) {
    case SHIP_UPGRADE_MINING:  sp->ship.mining_level++;  break;
    case SHIP_UPGRADE_HOLD:    sp->ship.hold_level++;    break;
    case SHIP_UPGRADE_TRACTOR: sp->ship.tractor_level++; break;
    default: break;
    }
    SIM_LOG("[sim] player %d upgraded %d to level %d\n", sp->id, (int)upgrade,
           ship_upgrade_level(&sp->ship, upgrade));
    emit_event(w, (sim_event_t){.type = SIM_EVENT_UPGRADE, .player_id = sp->id, .upgrade.upgrade = upgrade});
}

/* ================================================================== */
/* Per-player per-step functions                                      */
/* ================================================================== */

static void step_ship_rotation(ship_t *s, float dt, float turn_input) {
    s->angle = wrap_angle(s->angle + (turn_input * ship_hull_def(s)->turn_speed * dt));
}

static void step_ship_thrust(ship_t *s, float dt, float thrust_input, vec2 forward, bool boost) {
    const hull_def_t *hull = ship_hull_def(s);
    float mult = boost ? 1.6f : 1.0f;
    if (thrust_input > 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, hull->accel * thrust_input * mult * dt));
    } else if (thrust_input < 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
    }
}

/* Boost hull drain: 0.1 HP/s baseline, +1.4 HP/s per unit of |turn_input|.
 * Straight-line cruise costs ~1 HP every 10 seconds; yanking the stick at
 * full turn costs ~1.5 HP/s. Silent drain (no DAMAGE event) — emitting
 * one per tick would spam screen-shake and damage audio. If the drain
 * empties the hull, route through emergency_recover_ship so the usual
 * death/respawn UX fires. */
static void step_ship_boost_drain(world_t *w, server_player_t *sp, float dt, bool boost, float turn_input) {
    if (!boost || sp->ship.hull <= 0.0f) return;
    float turn_abs = turn_input < 0.0f ? -turn_input : turn_input;
    float drain = (0.1f + 1.4f * turn_abs) * dt;
    sp->ship.hull = fmaxf(0.0f, sp->ship.hull - drain);
    if (sp->ship.hull <= 0.01f) emergency_recover_ship(w, sp);
}

static void step_ship_motion(ship_t *s, float dt, const world_t *w, float cached_signal) {
    s->vel = v2_scale(s->vel, 1.0f / (1.0f + (ship_hull_def(s)->drag * dt)));
    s->pos = v2_add(s->pos, v2_scale(s->vel, dt));

    /* Signal-based boundary: push back when in frontier zone */
    float sig = cached_signal;
    float boundary = signal_boundary_push(sig);
    if (boundary > 0.0f) {
        float best_d_sq = 1e18f;
        int best_s = 0;
        for (int i = 0; i < MAX_STATIONS; i++) {
            float d_sq = v2_dist_sq(s->pos, w->stations[i].pos);
            if (d_sq < best_d_sq) { best_d_sq = d_sq; best_s = i; }
        }
        vec2 to_station = v2_sub(w->stations[best_s].pos, s->pos);
        float d = sqrtf(v2_len_sq(to_station));
        if (d > 0.001f) {
            /* Strong pull — at zero signal this is the only way back.
             * Scales with boundary (0 at frontier, 1 at zero signal). */
            float push_strength = boundary * 2.0f;
            vec2 push = v2_scale(to_station, push_strength / d);
            s->vel = v2_add(s->vel, push);
        }
    }
}

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
                ring_r, geom.corridors[ci].angle_a, geom.corridors[ci].angle_b);
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
        if (asteroid_is_collectible(&w->asteroids[i])) {
            /* Only collide if moving fast enough (hurled or whiplashed) */
            if (v2_len_sq(w->asteroids[i].vel) < 40.0f * 40.0f) continue;
        }
        resolve_ship_circle(w, sp, w->asteroids[i].pos, w->asteroids[i].radius);
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
        /* Passive hull repair while docked */
        float max_hull = ship_max_hull(&sp->ship);
        if (sp->ship.hull < max_hull) {
            sp->ship.hull = fminf(max_hull, sp->ship.hull + 8.0f * dt);
        }
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

static void step_fragment_collection(world_t *w, server_player_t *sp, float dt) {
    float nearby_sq = FRAGMENT_NEARBY_RANGE * FRAGMENT_NEARBY_RANGE;
    float tr = ship_tractor_range(&sp->ship);
    float tr_sq = tr * tr;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    /* Update towed fragments: spring physics to trail behind ship */
    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) {
            /* Remove invalid towed fragment */
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
            t--;
            continue;
        }
        asteroid_t *a = &w->asteroids[idx];
        /* Tractor drag: pull toward ship, maintaining safe distance.
         * No velocity blending — fragments trail naturally via spring
         * forces and heavy damping. Feels heavy and tactile. */
        float tractor_r = ship_tractor_range(&sp->ship);
        float safe_dist = 40.0f + a->radius + ship_hull_def(&sp->ship)->ship_radius;
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float dist_to_ship = v2_len(to_ship);

        /* Two-zone pull: gentle within range, strong when lagging */
        if (dist_to_ship > tractor_r * 0.7f) {
            vec2 pull = v2_scale(to_ship, 4.0f);
            a->vel = v2_add(a->vel, v2_scale(pull, dt));
        } else if (dist_to_ship > safe_dist) {
            vec2 pull = v2_scale(to_ship, 1.5f);
            a->vel = v2_add(a->vel, v2_scale(pull, dt));
        }

        /* Push away if too close (don't slam into ship) */
        if (dist_to_ship < safe_dist && dist_to_ship > 0.1f) {
            vec2 push = v2_scale(to_ship, -(safe_dist - dist_to_ship) * 8.0f);
            a->vel = v2_add(a->vel, v2_scale(push, dt));
        }

        /* Heavy drag — fragments bleed speed, trail behind naturally */
        a->vel = v2_scale(a->vel, 1.0f / (1.0f + 2.0f * dt));

        /* Never faster than the ship + some slack */
        float ship_spd = v2_len(sp->ship.vel);
        float frag_spd = v2_len(a->vel);
        float max_frag_spd = ship_spd + 60.0f;
        if (frag_spd > max_frag_spd && frag_spd > 0.1f)
            a->vel = v2_scale(a->vel, max_frag_spd / frag_spd);
        sp->tractor_fragments++;

        /* Fragment-ship collision: keep fragment from overlapping ship */
        float ship_r = ship_hull_def(&sp->ship)->ship_radius;
        float min_d = a->radius + ship_r + 4.0f;
        vec2 frag_to_ship = v2_sub(sp->ship.pos, a->pos);
        float ds = v2_len_sq(frag_to_ship);
        if (ds < min_d * min_d && ds > 0.1f) {
            float dd = sqrtf(ds);
            vec2 push = v2_scale(frag_to_ship, -((min_d - dd) / dd));
            a->pos = v2_add(a->pos, push);
        }

        /* Fragment-fragment collision: towed fragments push apart */
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
             * No drift phase — if it's in range and there's room, grab it. */
            if (sp->ship.towed_count < max_tow) {
                sp->ship.towed_fragments[sp->ship.towed_count] = (int16_t)i;
                sp->ship.towed_count++;
                a->last_towed_by = (int8_t)sp->id;
                sp->ship.stat_ore_mined += a->ore;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_PICKUP, .player_id = sp->id,
                                            .pickup = {.ore = a->ore, .fragments = 1}});
            }
        }
    }
}

/* ---- Leashed fragment physics ---- */

/* When fragments are towed but tractor is OFF (LEASHED state):
 * elastic band physics with slack zone, quadratic ramp, and snap. */
static void step_leashed_fragments(world_t *w, server_player_t *sp, float dt) {
    float tractor_r = ship_tractor_range(&sp->ship);
    float slack_length = tractor_r * 0.5f;  /* no force below 50% range */
    float band_range = tractor_r - slack_length;
    int max_tow = 2 + sp->ship.tractor_level * 2;

    for (int t = sp->ship.towed_count - 1; t >= 0; t--) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
        asteroid_t *a = &w->asteroids[idx];
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float dist = v2_len(to_ship);

        /* Snap: beam breaks at 150% tractor range — generous leash
         * so fragments don't immediately pop off after release */
        if (dist > tractor_r * 1.5f) {
            sp->ship.towed_count--;
            sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
            sp->ship.towed_fragments[sp->ship.towed_count] = -1;
            continue;
        }

        /* Slack zone: no force below slack_length */
        if (dist <= slack_length || dist < 0.1f) continue;

        /* Elastic: quadratic ramp from slack to max */
        float stretch = (dist - slack_length) / band_range;  /* 0..1 */
        stretch = clampf(stretch, 0.0f, 1.0f);

        /* Pull fragment toward ship */
        vec2 dir = v2_scale(to_ship, 1.0f / dist);
        float pull_strength = stretch * stretch * 200.0f;
        a->vel = v2_add(a->vel, v2_scale(dir, pull_strength * dt));

        /* Drag on ship: reduce ship velocity proportionally */
        float drag = 0.3f * stretch * ((float)(sp->ship.towed_count) / (float)max_tow);
        sp->ship.vel = v2_scale(sp->ship.vel, 1.0f - drag * dt);
    }

    /* Light drag on leashed fragments so they don't orbit forever */
    for (int t = 0; t < sp->ship.towed_count; t++) {
        int idx = sp->ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        w->asteroids[idx].vel = v2_scale(w->asteroids[idx].vel, 1.0f / (1.0f + 0.5f * dt));
    }
}

/* Deposit towed fragments: when the SHIP is near an ore buyer module,
 * all towed fragments get consumed (ore → station, credits → player).
 * Fragments don't need to individually reach the hopper — the ship does. */
/* HOPPER_PULL_RANGE, HOPPER_PULL_ACCEL → game_sim.h */
#define FURNACE_SMELT_RANGE 250.0f  /* fragment counts as "held" by furnace within this range */

static void release_towed_fragments(server_player_t *sp);

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

/* Release all towed fragments (manual dump). */
static void release_towed_fragments(server_player_t *sp) {
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

    /* Materialize a nearby planned station if scaffold is close to it. */
    {
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
            add_module_at(st, MODULE_SIGNAL_RELAY, 0, 0xFF);
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

    /* Not near an outpost — found a new station if in signal range */
    if (signal_strength_at(w, sc->pos) > 0.0f && can_place_outpost(w, sc->pos)) {
        int slot = -1;
        for (int s = 3; s < MAX_STATIONS; s++) {
            if (!station_exists(&w->stations[s])) { slot = s; break; }
        }
        if (slot >= 0) {
            station_t *st = &w->stations[slot];
            memset(st, 0, sizeof(*st));
            generate_outpost_name(st->name, sizeof(st->name), sc->pos, slot);
            st->pos = sc->pos;
            st->radius = OUTPOST_RADIUS;
            st->dock_radius = OUTPOST_DOCK_RADIUS;
            st->signal_range = OUTPOST_SIGNAL_RANGE;
            /* Outpost is born under construction — needs frames delivered to
             * activate. The scaffold you towed becomes the first module
             * (pre-paid), but it can't go live until the station does. */
            st->scaffold = true;
            st->scaffold_progress = 0.0f;
            add_module_at(st, MODULE_DOCK, 0, 0xFF);
            add_module_at(st, MODULE_SIGNAL_RELAY, 0, 0xFF);
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
    /* Can't place here — do nothing, keep towing */
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

    /* Check station modules */
    for (int si = 0; si < MAX_STATIONS; si++) {
        const station_t *st = &w->stations[si];
        if (st->signal_range <= 0.0f) continue;
        /* Check core */
        vec2 to_core = v2_sub(st->pos, muzzle);
        float proj = v2_dot(to_core, forward);
        if (proj > 0.0f && proj < best_dist) {
            vec2 closest = v2_add(muzzle, v2_scale(forward, proj));
            float perp = v2_len(v2_sub(closest, st->pos));
            if (perp < st->radius) {
                best_dist = proj;
                sp->scan_target_type = 1;
                sp->scan_target_index = si;
                sp->scan_module_index = -1; /* core */
                sp->beam_end = v2_sub(st->pos, v2_scale(v2_norm(to_core), st->radius * 0.9f));
            }
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
            vec2 hit = v2_add(muzzle, v2_scale(forward, t_hit));
            float hit_dist = v2_len(v2_sub(hit, st->pos));
            if (fabsf(hit_dist - rr) > ring_thickness) continue;
            best_dist = t_hit;
            sp->scan_target_type = 1;
            sp->scan_target_index = si;
            sp->scan_module_index = -1;
            sp->beam_end = hit;
        }
        /* Check individual modules */
        for (int mi = 0; mi < st->module_count; mi++) {
            if (st->modules[mi].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[mi].ring, st->modules[mi].slot);
            vec2 to_mod = v2_sub(mp, muzzle);
            float mproj = v2_dot(to_mod, forward);
            if (mproj > 0.0f && mproj < best_dist) {
                vec2 closest = v2_add(muzzle, v2_scale(forward, mproj));
                float perp = v2_len(v2_sub(closest, mp));
                if (perp < STATION_MODULE_COL_RADIUS) {
                    best_dist = mproj;
                    sp->scan_target_type = 1;
                    sp->scan_target_index = si;
                    sp->scan_module_index = mi;
                    sp->beam_end = v2_sub(mp, v2_scale(v2_norm(to_mod), STATION_MODULE_COL_RADIUS * 0.9f));
                }
            }
        }
    }

    /* Check NPC ships */
    for (int ni = 0; ni < MAX_NPC_SHIPS; ni++) {
        const npc_ship_t *npc = &w->npc_ships[ni];
        if (!npc->active) continue;
        vec2 to_npc = v2_sub(npc->pos, muzzle);
        float proj = v2_dot(to_npc, forward);
        float npc_r = HULL_DEFS[npc->hull_class].render_scale * 16.0f;
        if (proj > 0.0f && proj < best_dist) {
            vec2 closest = v2_add(muzzle, v2_scale(forward, proj));
            float perp = v2_len(v2_sub(closest, npc->pos));
            if (perp < npc_r) {
                best_dist = proj;
                sp->scan_target_type = 2;
                sp->scan_target_index = ni;
                sp->scan_module_index = -1;
                sp->beam_end = v2_sub(npc->pos, v2_scale(v2_norm(to_npc), npc_r * 0.9f));
            }
        }
    }

    /* Check other players */
    for (int pi = 0; pi < MAX_PLAYERS; pi++) {
        const server_player_t *other = &w->players[pi];
        if (!other->connected || other->id == sp->id) continue;
        vec2 to_p = v2_sub(other->ship.pos, muzzle);
        float proj = v2_dot(to_p, forward);
        float pr = HULL_DEFS[other->ship.hull_class].ship_radius;
        if (proj > 0.0f && proj < best_dist) {
            vec2 closest = v2_add(muzzle, v2_scale(forward, proj));
            float perp = v2_len(v2_sub(closest, other->ship.pos));
            if (perp < pr) {
                best_dist = proj;
                sp->scan_target_type = 3;
                sp->scan_target_index = pi;
                sp->scan_module_index = -1;
                sp->beam_end = v2_sub(other->ship.pos, v2_scale(v2_norm(to_p), pr * 0.9f));
            }
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
        vec2 to_a = v2_sub(a->pos, muzzle);
        vec2 normal = v2_norm(to_a);
        sp->beam_end = v2_sub(a->pos, v2_scale(normal, a->radius * 0.85f));
        sp->beam_hit = true;
        /* Check if laser is powerful enough for this tier */
        asteroid_tier_t max_tier = max_mineable_tier(sp->ship.mining_level);
        if (a->tier < max_tier) {
            /* Beam hits but does no damage — too tough */
            sp->beam_ineffective = true;
        } else {
            emit_event(w, (sim_event_t){.type = SIM_EVENT_MINING_TICK, .player_id = sp->id});
            if (!w->player_only_mode) {
                float mined = ship_mining_rate(&sp->ship) * dt * signal_mining_efficiency(cached_signal);
                mined = fminf(mined, a->hp);
                a->hp -= mined;
                a->net_dirty = true;
                if (a->hp <= 0.01f) {
                    fracture_asteroid(w, sp->hover_asteroid, normal, (int8_t)sp->id);
                    sp->ship.stat_asteroids_fractured++;
                }
            }
        }
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

/* Find or create a ledger entry for a player at a station */
static int ledger_find_or_create(station_t *st, const uint8_t *token) {
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_token, token, 8) == 0) return i;
    }
    if (st->ledger_count >= 16) return -1;
    int idx = st->ledger_count++;
    memcpy(st->ledger[idx].player_token, token, 8);
    st->ledger[idx].balance = 0.0f;
    st->ledger[idx].lifetime_supply = 0.0f;
    return idx;
}

/* Credit a player's ledger when they supply ore to a station.
 * Pays from the station's credit pool — no credits created. */
void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value) {
    int idx = ledger_find_or_create(st, token);
    if (idx < 0) return;
    /* Station keeps 35% cut for smelting — supplier gets 65% */
    float supplier_share = ore_value * 0.65f;
    /* Cap payout at what the station can afford */
    if (supplier_share > st->credit_pool)
        supplier_share = st->credit_pool;
    if (supplier_share < 0.01f) return;
    st->credit_pool -= supplier_share;
    st->ledger[idx].balance += supplier_share;
    st->ledger[idx].lifetime_supply += ore_value;
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

    bool has_payload = (sp->ship.towed_count > 0);
    if (!has_payload) {
        for (int k = 0; k < COMMODITY_COUNT; k++) {
            if (sp->ship.cargo[k] > 0.1f) { has_payload = true; break; }
        }
    }

    contract_t *c = &w->contracts[slot];
    memset(c, 0, sizeof(*c));

    if (has_payload) {
        /* TRACTOR: deliver raw ore to the nearest station with a furnace. */
        int dest = -1;
        float best_d = 1e18f;
        for (int s = 0; s < MAX_STATIONS; s++) {
            station_t *st = &w->stations[s];
            if (!station_is_active(st)) continue;
            if (!station_has_module(st, MODULE_FURNACE) &&
                !station_has_module(st, MODULE_FURNACE_CU) &&
                !station_has_module(st, MODULE_FURNACE_CR)) continue;
            float d = v2_dist_sq(sp->ship.pos, st->pos);
            if (d < best_d) { best_d = d; dest = s; }
        }
        if (dest < 0) return -1;
        c->active = true;
        c->action = CONTRACT_TRACTOR;
        c->station_index = (uint8_t)dest;
        c->commodity = COMMODITY_FERRITE_ORE;
        c->quantity_needed = 10.0f;
        c->base_price = w->stations[dest].base_price[COMMODITY_FERRITE_ORE] * 1.2f;
        c->target_index = -1;
        c->claimed_by = (int8_t)sp->id;
    } else {
        /* FRACTURE: nearest mineable rock in signal coverage. */
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
    }
    return slot;
}

static void handle_hail(world_t *w, server_player_t *sp) {
    if (sp->docked) return;

    /* Ship-based ping: the player is the transmitter. Stations inside
     * comm_range respond normally; stations within 2× comm_range chirp
     * back "out of range" with a bearing so the player knows which way
     * to fly. Stations further than that stay silent.
     *
     * credits == -1.0f is the out-of-range sentinel on the wire; the
     * client renders a short "too far — nearest: <name>" overlay
     * instead of the full hail UI. */
    float comm = (sp->ship.comm_range > 0.0f) ? sp->ship.comm_range : 1500.0f;
    float comm_sq = comm * comm;
    float chirp_sq = (2.0f * comm) * (2.0f * comm);

    int nearest_in = -1;    float best_in = 1e18f;
    int nearest_chirp = -1; float best_chirp = 1e18f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (st->signal_range <= 0.0f) continue;
        float d_sq = v2_dist_sq(sp->ship.pos, st->pos);
        if (d_sq <= comm_sq) {
            if (d_sq < best_in) { best_in = d_sq; nearest_in = s; }
        } else if (d_sq <= chirp_sq) {
            if (d_sq < best_chirp) { best_chirp = d_sq; nearest_chirp = s; }
        }
    }

    if (nearest_in >= 0) {
        float balance = ledger_balance(&w->stations[nearest_in], sp->session_token);
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
    }
    /* No station even in chirp range → silent ping. Client already
     * drew its local expanding ring so the press isn't lost. */
}

static void step_station_interaction_system(world_t *w, server_player_t *sp, const input_intent_t *intent) {
    /* Order scaffold from shipyard: queues build + generates material contract */
    if (intent->buy_scaffold_kit && sp->docked && !w->player_only_mode) {
        module_type_t kit_type = intent->scaffold_kit_module;
        station_t *st = &w->stations[sp->current_station];
        if (!station_sells_scaffold(st, kit_type)) {
            emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id});
        } else if (st->pending_scaffold_count >= 4) {
            emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id});
        } else if (!module_unlocked_for_player(sp->ship.unlocked_modules, kit_type)) {
            /* Tech tree gate: prereq not yet unlocked */
            emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id});
        } else {
            float fee = (float)scaffold_order_fee(kit_type);
            if (!ledger_spend(st, sp->session_token, fee, &sp->ship)) {
                emit_event(w, (sim_event_t){.type = SIM_EVENT_ORDER_REJECTED, .player_id = sp->id});
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
        /* Deliver to scaffolds/modules first, then sell remaining */
        step_scaffold_delivery(w, sp);
        step_module_delivery(w, docked_st, sp->current_station, &sp->ship);
        try_sell_station_cargo(w, sp);
    }
    else if (intent->service_repair) try_repair_ship(w, sp);
    else if (intent->upgrade_mining) try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_MINING);
    else if (intent->upgrade_hold)   try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_HOLD);
    else if (intent->upgrade_tractor)try_apply_ship_upgrade(w, sp, SHIP_UPGRADE_TRACTOR);
    /* Buy ingots from station inventory */
    if (intent->buy_product && !w->player_only_mode) {
        commodity_t c = intent->buy_commodity;
        if (c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT
            && station_produces(docked_st, c)) {
            float available = docked_st->inventory[c];
            float space = ship_cargo_capacity(&sp->ship) - ship_total_cargo(&sp->ship);
            float price_per = station_sell_price(docked_st, c);
            /* Buy as much as you can afford and carry */
            float bal = ledger_balance(docked_st, sp->session_token);
            float afford = (price_per > 0.01f) ? floorf(bal / price_per) : 0.0f;
            float amount = fminf(fminf(available, space), afford);
            float total_cost = amount * price_per;
            if (amount > 0.01f && ledger_spend(docked_st, sp->session_token, total_cost, &sp->ship)) {
                sp->ship.cargo[c] += amount;
                docked_st->inventory[c] -= amount;
                SIM_LOG("[sim] player %d bought %.0f of commodity %d for %.0f cr\n",
                        sp->id, amount, c, total_cost);
            }
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
        step_ship_thrust(&sp->ship, dt, thrust_input, forward, boost);
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
                    float available = nearby_st->inventory[c];
                    float price_per = station_sell_price(nearby_st, c);
                    float nbal = ledger_balance(nearby_st, sp->session_token);
                    float afford = (price_per > FLOAT_EPSILON) ? floorf(nbal / price_per) : 0.0f;
                    float amount = fminf(fminf(available, 1.0f), afford); /* buy 1 at a time */
                    if (amount > FLOAT_EPSILON) {
                        float cost = amount * price_per;
                        if (ledger_spend(nearby_st, sp->session_token, cost, &sp->ship)) {
                            sp->ship.cargo[c] += amount;
                            nearby_st->inventory[c] -= amount;
                            emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = sp->id});
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
                    release_towed_fragments(sp);
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
                memset(old, 0, sizeof(*old));
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
        float plan_sig = signal_strength_at(w, pos);
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
                memset(st, 0, sizeof(*st));
                st->id = w->next_station_id++;
                generate_outpost_name(st->name, sizeof(st->name), pos, slot);
                st->pos = pos;
                st->planned = true;
                st->planned_owner = (int8_t)sp->id;
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
                memset(st, 0, sizeof(*st));
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
                if (!st->modules[m].scaffold) continue;
                if (st->modules[m].build_progress >= 1.0f) continue;
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
                    if (st->modules[m].build_progress < 1.0f) { all_supplied = false; break; }
                }
                if (st->scaffold && c == COMMODITY_FRAME && st->scaffold_progress < 1.0f)
                    all_supplied = false;
                if (all_supplied) {
                    w->contracts[i].active = false;
                    emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_TRACTOR});
                }
            } else {
                /* Non-construction: close on generic inventory threshold */
                float current = st->inventory[c];
                float threshold = (c < COMMODITY_RAW_ORE_COUNT) ? REFINERY_HOPPER_CAPACITY * 0.8f : MAX_PRODUCT_STOCK * 0.8f;
                if (current >= threshold) {
                    w->contracts[i].active = false;
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

        /* Check which contract types this station already has */
        bool has_ore_contract = false;
        bool has_production_contract = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active || w->contracts[k].station_index != s) continue;
            if (w->contracts[k].commodity < COMMODITY_RAW_ORE_COUNT)
                has_ore_contract = true;
            else
                has_production_contract = true;
        }
        if (has_ore_contract && has_production_contract) continue;

        /* Skip contract generation if station is nearly broke */
        if (st->credit_pool < 100.0f) continue;

        /* Pool factor: rich stations offer better prices.
         * 0.2x at 1000 cr, 1.0x at 5000 cr, 1.5x at 10000+ cr */
        float pool_factor = st->credit_pool / 5000.0f;
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
                float remaining = cost * (1.0f - st->modules[m].build_progress);
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
        if (!need.active && !has_ore_contract && (station_has_module(st, MODULE_FURNACE)
            || station_has_module(st, MODULE_FURNACE_CU)
            || station_has_module(st, MODULE_FURNACE_CR))) {
            float worst_deficit = 0.0f;
            int worst_ore = -1;
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                if (!sim_can_smelt_ore(st, (commodity_t)c)) continue;
                float deficit = REFINERY_HOPPER_CAPACITY * 0.5f - st->inventory[c];
                if (deficit > worst_deficit) { worst_deficit = deficit; worst_ore = c; }
            }
            if (worst_ore >= 0) {
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = (commodity_t)worst_ore,
                    .quantity_needed = 0.0f, /* inventory-driven, not delivery-driven */
                    .base_price = st->base_price[worst_ore] * pool_factor,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 4: ingot buffer deficit (production slot) */
        if (!need.active && !has_production_contract) {
            struct { module_type_t mod; commodity_t ingot; } checks[] = {
                { MODULE_FRAME_PRESS, COMMODITY_FERRITE_INGOT },
                { MODULE_LASER_FAB, COMMODITY_CUPRITE_INGOT },
                { MODULE_TRACTOR_FAB, COMMODITY_CRYSTAL_INGOT },
            };
            float worst_deficit = 0.0f;
            int worst_idx = -1;
            for (int j = 0; j < 3; j++) {
                if (!station_has_module(st, checks[j].mod)) continue;
                float deficit = MAX_PRODUCT_STOCK * 0.5f - st->inventory[checks[j].ingot];
                if (deficit > worst_deficit) { worst_deficit = deficit; worst_idx = j; }
            }
            if (worst_idx >= 0) {
                need = (contract_t){
                    .active = true, .action = CONTRACT_TRACTOR,
                    .station_index = (uint8_t)s,
                    .commodity = checks[worst_idx].ingot,
                    .quantity_needed = worst_deficit,
                    .base_price = st->base_price[checks[worst_idx].ingot] * 1.15f * pool_factor,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Post the contract if we found a need */
        if (need.active) {
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) {
                    w->contracts[k] = need;
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

/* What commodity does a producer module output? */
static module_type_t producer_module_for_commodity(commodity_t c) {
    switch (c) {
        case COMMODITY_FRAME:         return MODULE_FRAME_PRESS;
        case COMMODITY_FERRITE_INGOT: return MODULE_FURNACE;
        case COMMODITY_CUPRITE_INGOT: return MODULE_FURNACE_CU;
        case COMMODITY_CRYSTAL_INGOT: return MODULE_FURNACE_CR;
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
            if (pull > st->inventory[mat]) pull = st->inventory[mat];
            float room = needed - nascent->build_amount;
            if (pull > room) pull = room;
            if (pull > 0.0f) {
                st->inventory[mat] -= pull;
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
static const float SCAFFOLD_SNAP_RANGE = 200.0f;
/* How fast the station's tendrils pull a scaffold into position. */
static const float SCAFFOLD_SNAP_PULL = 4.0f;
/* Distance threshold to finalize placement. */
static const float SCAFFOLD_SNAP_ARRIVE = 8.0f;

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
                if (dist < 1.0f) dist = 1.0f;
                vec2 norm = v2_scale(delta, 1.0f / dist);
                /* Strong direct pull — no orbit, tractor-like */
                float pull_strength = 25.0f * (1.0f + 2.0f * (1.0f - dist / PLAN_PULL_RANGE));
                sc->vel = v2_add(sc->vel, v2_scale(norm, pull_strength * dt));
                sc->vel = v2_scale(sc->vel, 1.0f / (1.0f + 3.0f * dt)); /* heavy damping */
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

            /* Accelerating pull — gets stronger as it gets closer (tendril grip tightens) */
            float pull_strength = SCAFFOLD_SNAP_PULL * (1.0f + 2.0f * (1.0f - dist / SCAFFOLD_SNAP_RANGE));
            if (pull_strength < SCAFFOLD_SNAP_PULL) pull_strength = SCAFFOLD_SNAP_PULL;
            vec2 pull_dir = v2_scale(delta, pull_strength);
            sc->vel = v2_add(sc->vel, v2_scale(pull_dir, dt));
            /* Heavy damping so it doesn't overshoot */
            sc->vel = v2_scale(sc->vel, 1.0f / (1.0f + 5.0f * dt));
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
uint64_t signal_channel_post(world_t *w, int sender_station, const char *text, const char *audio_url) {
    if (!w || !text || text[0] == '\0') return 0;
    signal_channel_t *ch = &w->signal_channel;
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
    ch->head = (ch->head + 1) % SIGNAL_CHANNEL_CAPACITY;
    if (ch->count < SIGNAL_CHANNEL_CAPACITY) ch->count++;
    return m->id;
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

void world_sim_step(world_t *w, float dt) {
    w->events.count = 0;
    w->time += dt;
    /* Derive ring rotation from world time — deterministic, no drift between
     * client and server since both share the same world.time. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) continue;
        float base = w->stations[s].arm_speed[0] * w->time;
        base = base - floorf(base / TWO_PI_F) * TWO_PI_F;
        for (int r = 0; r < STATION_NUM_RINGS && r < MAX_ARMS; r++) {
            w->stations[s].arm_rotation[r] = base;
        }
    }
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
    step_furnace_smelting(w, dt);
    sim_step_refinery_production(w, dt);
    sim_step_station_production(w, dt);
    step_module_flow(w, dt);
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
                /* Ramming damage — both ships take damage based on impact speed */
                if (impact > SHIP_COLLISION_DAMAGE_THRESHOLD * 0.7f) {
                    float dmg = (impact - SHIP_COLLISION_DAMAGE_THRESHOLD * 0.7f) * SHIP_COLLISION_DAMAGE_SCALE;
                    apply_ship_damage(w, &w->players[i], dmg);
                    apply_ship_damage(w, &w->players[j], dmg);
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
    free(w->signal_cache.strength);
    w->signal_cache.strength = NULL;
    w->signal_cache.valid = false;
    free(w->asteroid_grid.entries);
    w->asteroid_grid.entries = NULL;
}

void world_reset(world_t *w) {
    uint32_t seed = w->rng;  /* caller may pre-set seed; 0 = default */
    float *sig_buf = w->signal_cache.strength; /* preserve heap allocation */
    memset(w, 0, sizeof(*w));
    w->signal_cache.strength = sig_buf; /* restore — signal_grid_build reuses it */
    w->rng = seed ? seed : 2037u;
    belt_field_init(&w->belt, w->rng, BELT_SCALE);

    /* --- Stations --- */
    w->next_station_id = 1; /* IDs start at 1; 0 = unassigned */
    w->stations[0].id = w->next_station_id++;
    snprintf(w->stations[0].name, sizeof(w->stations[0].name), "%s", "Prospect Refinery");
    w->stations[0].pos         = v2(0.0f, -2400.0f);
    w->stations[0].radius      = 40.0f;
    w->stations[0].dock_radius = 240.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[0].signal_range = 18000.0f;
    /* Ring 1: dock + relay + furnace (furnace beams to Ring 2 ore silo) */
    add_module_at(&w->stations[0], MODULE_DOCK, 1, 0);
    add_module_at(&w->stations[0], MODULE_SIGNAL_RELAY, 1, 1);
    add_module_at(&w->stations[0], MODULE_FURNACE, 1, 2);
    /* Ring 2: ore silo (incomplete ring — gaps are natural) */
    add_module_at(&w->stations[0], MODULE_ORE_SILO, 2, 3);
    w->stations[0].arm_count = 2;
    w->stations[0].arm_speed[0] = STATION_RING_SPEED;
    w->stations[0].ring_offset[0] = 0.0f;
    w->stations[0].ring_offset[1] = 1.05f;  /* ~60° offset — unique silhouette */
    rebuild_station_services(&w->stations[0]);
    /* Seed inventory and credit pool */
    w->stations[0].inventory[COMMODITY_FERRITE_INGOT] = 20.0f;
    w->stations[0].credit_pool = 10000.0f;
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
    w->stations[1].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[1].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[1].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[1].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[1].base_price[COMMODITY_FRAME] = 20.0f;
    /* Ring 1: dock + relay + ore silo */
    add_module_at(&w->stations[1], MODULE_DOCK, 1, 0);
    add_module_at(&w->stations[1], MODULE_SIGNAL_RELAY, 1, 1);
    add_module_at(&w->stations[1], MODULE_ORE_SILO, 1, 2);
    /* Ring 2 (industrial): fabrication + services */
    add_module_at(&w->stations[1], MODULE_FRAME_PRESS, 2, 0);
    add_module_at(&w->stations[1], MODULE_LASER_FAB, 2, 1);
    add_module_at(&w->stations[1], MODULE_TRACTOR_FAB, 2, 2);
    add_module_at(&w->stations[1], MODULE_SHIPYARD, 2, 3);
    w->stations[1].arm_count = 2;
    w->stations[1].arm_speed[0] = STATION_RING_SPEED;
    w->stations[1].ring_offset[0] = 0.0f;
    w->stations[1].ring_offset[1] = 2.40f;  /* ~137° offset */
    rebuild_station_services(&w->stations[1]);
    /* Seed inventory and credit pool */
    w->stations[1].inventory[COMMODITY_FERRITE_INGOT] = 15.0f;
    w->stations[1].inventory[COMMODITY_FRAME] = 12.0f;
    w->stations[1].credit_pool = 10000.0f;
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
    w->stations[2].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[2].base_price[COMMODITY_LASER_MODULE] = 28.0f;
    w->stations[2].base_price[COMMODITY_TRACTOR_MODULE] = 36.0f;
    /* Ring 1: dock + relay + ferrite furnace */
    add_module_at(&w->stations[2], MODULE_DOCK, 1, 0);
    add_module_at(&w->stations[2], MODULE_SIGNAL_RELAY, 1, 1);
    add_module_at(&w->stations[2], MODULE_FURNACE, 1, 2);
    /* Ring 2: ore silo + copper/crystal furnaces + services */
    add_module_at(&w->stations[2], MODULE_ORE_SILO, 2, 0);
    add_module_at(&w->stations[2], MODULE_FURNACE_CU, 2, 1);
    add_module_at(&w->stations[2], MODULE_FURNACE_CR, 2, 2);
    add_module_at(&w->stations[2], MODULE_LASER_FAB, 2, 3);
    add_module_at(&w->stations[2], MODULE_TRACTOR_FAB, 2, 4);
    /* Ring 3: silos + advanced smelting overflow + services */
    add_module_at(&w->stations[2], MODULE_ORE_SILO, 3, 0);
    add_module_at(&w->stations[2], MODULE_ORE_SILO, 3, 1);
    add_module_at(&w->stations[2], MODULE_FURNACE_CU, 3, 2);
    add_module_at(&w->stations[2], MODULE_FURNACE_CR, 3, 3);
    add_module_at(&w->stations[2], MODULE_SHIPYARD, 3, 4);
    w->stations[2].arm_count = 3;
    w->stations[2].arm_speed[0] = STATION_RING_SPEED;
    w->stations[2].ring_offset[0] = 0.0f;
    w->stations[2].ring_offset[1] = 0.52f;  /* ~30° offset */
    w->stations[2].ring_offset[2] = 1.83f;  /* ~105° offset */
    rebuild_station_services(&w->stations[2]);
    /* Seed inventory and credit pool */
    w->stations[2].inventory[COMMODITY_CUPRITE_INGOT] = 15.0f;
    w->stations[2].inventory[COMMODITY_CRYSTAL_INGOT] = 15.0f;
    w->stations[2].inventory[COMMODITY_LASER_MODULE] = 10.0f;
    w->stations[2].inventory[COMMODITY_TRACTOR_MODULE] = 10.0f;
    w->stations[2].credit_pool = 10000.0f;
    snprintf(w->stations[2].station_slug, sizeof(w->stations[2].station_slug), "helios");
    snprintf(w->stations[2].currency_name, sizeof(w->stations[2].currency_name), "helios credits");
    snprintf(w->stations[2].hail_message, sizeof(w->stations[2].hail_message),
             "Helios Works. Advanced smelting. Copper and crystal refined here.");
    w->station_count = 3; /* 3 starter stations */
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
                        materialize_asteroid(w, slot, &rocks[ri], cx, cy);
                        slot++;
                    }
                }
            }
        }
    }

    /* --- NPC ships: 2 miners at refinery, 2 haulers for logistics,
     *     1 tow drone at each shipyard for autonomous scaffold delivery --- */
    spawn_npc(w, 0, NPC_ROLE_MINER);
    spawn_npc(w, 0, NPC_ROLE_MINER);
    spawn_npc(w, 0, NPC_ROLE_HAULER);
    spawn_npc(w, 0, NPC_ROLE_HAULER);
    spawn_npc(w, 1, NPC_ROLE_TOW); /* Kepler shipyard */
    spawn_npc(w, 2, NPC_ROLE_TOW); /* Helios shipyard */

    /* Precompute station nav meshes now that geometry is finalized. */
    station_rebuild_all_nav(w);

    SIM_LOG("[sim] world reset complete (%d asteroids, 6 NPCs)\n", FIELD_ASTEROID_TARGET);
}

/* ================================================================== */
/* Public: player_init_ship                                           */
/* ================================================================== */

void player_init_ship(server_player_t *sp, world_t *w) {
    memset(&sp->ship, 0, sizeof(sp->ship));
    sp->ship.hull_class = HULL_CLASS_MINER;
    sp->ship.hull       = HULL_DEFS[HULL_CLASS_MINER].max_hull;
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
    sp->autopilot_mode = 0;
    sp->autopilot_state = 0;
    sp->autopilot_target = -1;
    sp->autopilot_timer = 0.0f;
    anchor_ship_in_station(sp, w);
}

/* Grant starting credits at the docked station. Call AFTER session_token is set. */
void player_seed_credits(server_player_t *sp, world_t *w) {
    int st = sp->current_station;
    if (st < 0 || st >= MAX_STATIONS) st = 0;
    /* Already has balance at this station? Skip (e.g., loaded from save). */
    if (ledger_balance(&w->stations[st], sp->session_token) > 0.01f) return;
    float seed = fminf(50.0f, w->stations[st].credit_pool);
    w->stations[st].credit_pool -= seed;
    ledger_earn(&w->stations[st], sp->session_token, seed);
}
