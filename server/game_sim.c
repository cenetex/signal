/*
 * game_sim.c -- Game simulation for Signal Space Miner.
 * Used by both the authoritative server and the client (local sim).
 * All rendering, audio, and sokol references are excluded.
 * Global state replaced with world_t *w and server_player_t *sp parameters.
 */
#include "game_sim.h"
#include <stdlib.h>

#ifdef GAME_SIM_VERBOSE
#define SIM_LOG(...) printf(__VA_ARGS__)
#else
#define SIM_LOG(...) ((void)0)
#endif


static void emit_event(world_t *w, sim_event_t ev) {
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
        .ore_capacity  = 120.0f,
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
        .ore_capacity  = 0.0f,
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
        .ore_capacity  = 40.0f,
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
/* RNG -- world-local xorshift                                        */
/* ================================================================== */

static uint32_t rng_next(world_t *w) {
    if (w->rng == 0) w->rng = 0xA341316Cu;
    uint32_t x = w->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    w->rng = x;
    return x;
}

static float randf(world_t *w) {
    return (float)(rng_next(w) & 0x00FFFFFFu) / 16777215.0f;
}

static float rand_range(world_t *w, float lo, float hi) {
    return lerpf(lo, hi, randf(w));
}

static int rand_int(world_t *w, int lo, int hi) {
    return lo + (int)(rng_next(w) % (uint32_t)(hi - lo + 1));
}

/* ================================================================== */
/* Signal strength                                                    */
/* ================================================================== */

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
                float dist = sqrtf(v2_dist_sq(w->stations[s].pos, w->stations[o].pos));
                if (dist < w->stations[o].signal_range) {
                    w->stations[s].signal_connected = true;
                    changed = true;
                    break;
                }
            }
        }
    }
}

float signal_strength_at(const world_t *w, vec2 pos) {
    float best = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float dist = sqrtf(v2_dist_sq(pos, w->stations[s].pos));
        float strength = fmaxf(0.0f, 1.0f - (dist / w->stations[s].signal_range));
        if (strength > best) best = strength;
    }
    return best;
}

/* ================================================================== */
/* Station construction                                               */
/* ================================================================== */

bool can_place_outpost(const world_t *w, vec2 pos) {
    /* Must be within signal range of an existing station */
    if (signal_strength_at(w, pos) <= 0.0f) return false;
    /* Must not overlap existing stations */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) continue;
        float d = sqrtf(v2_dist_sq(pos, w->stations[s].pos));
        if (d < OUTPOST_MIN_DISTANCE) return false;
    }
    /* Must have a free station slot */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) return true;
    }
    return false;
}

static void add_module_at(station_t *st, module_type_t type, uint8_t ring, uint8_t slot) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    station_module_t *m = &st->modules[st->module_count++];
    m->type = type;
    m->ring = ring;
    m->slot = slot;
    m->scaffold = false;
    m->build_progress = 1.0f;
}

static int spawn_npc(world_t *w, int station_idx, npc_role_t role);

static void activate_outpost(world_t *w, int station_idx) {
    station_t *st = &w->stations[station_idx];
    st->scaffold = false;
    st->scaffold_progress = 1.0f;
    st->signal_range = OUTPOST_SIGNAL_RANGE;
    add_module_at(st, MODULE_REPAIR_BAY, 0, 0xFF);
    rebuild_station_services(st);
    rebuild_signal_chain(w);
    /* Spawn NPCs based on installed modules */
    if (station_has_module(st, MODULE_FURNACE) || station_has_module(st, MODULE_FURNACE_CU) || station_has_module(st, MODULE_FURNACE_CR))
        spawn_npc(w, station_idx, NPC_ROLE_MINER);
    if (station_has_module(st, MODULE_FRAME_PRESS) || station_has_module(st, MODULE_LASER_FAB) || station_has_module(st, MODULE_TRACTOR_FAB))
        spawn_npc(w, station_idx, NPC_ROLE_HAULER);
    SIM_LOG("[sim] outpost %d activated (signal_range=%.0f)\n", station_idx, OUTPOST_SIGNAL_RANGE);
}

/* What material each module requires for construction */
static commodity_t module_build_material(module_type_t type) {
    switch (type) {
        case MODULE_FURNACE_CU:  return COMMODITY_CUPRITE_INGOT;
        case MODULE_FURNACE_CR:  return COMMODITY_CRYSTAL_INGOT;
        case MODULE_LASER_FAB:   return COMMODITY_CUPRITE_INGOT;
        case MODULE_TRACTOR_FAB: return COMMODITY_CRYSTAL_INGOT;
        default:                 return COMMODITY_FRAME;
    }
}

/* Module construction cost in ingots */
static float module_build_cost(module_type_t type) {
    switch (type) {
        case MODULE_REPAIR_BAY:     return 30.0f;
        case MODULE_ORE_BUYER:      return 40.0f;
        case MODULE_FURNACE:        return 60.0f;
        case MODULE_FURNACE_CU:     return 100.0f;
        case MODULE_FURNACE_CR:     return 140.0f;
        case MODULE_FRAME_PRESS:    return 80.0f;
        case MODULE_LASER_FAB:      return 80.0f;
        case MODULE_TRACTOR_FAB:    return 80.0f;
        case MODULE_CONTRACT_BOARD: return 20.0f;
        case MODULE_BLUEPRINT_DESK: return 50.0f;
        case MODULE_SIGNAL_RELAY:   return 40.0f;
        case MODULE_ORE_SILO:       return 30.0f;
        default:                    return 20.0f;
    }
}

/* Credit cost to begin module construction */
static float module_credit_cost(module_type_t type) {
    switch (type) {
        case MODULE_FURNACE:     return 200.0f;
        case MODULE_FURNACE_CU:  return 400.0f;
        case MODULE_FURNACE_CR:  return 600.0f;
        case MODULE_FRAME_PRESS: return 300.0f;
        case MODULE_LASER_FAB:   return 300.0f;
        case MODULE_TRACTOR_FAB: return 300.0f;
        default:                 return 100.0f;
    }
}

/* Add a scaffold module to a station and generate a supply contract */
void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    if (station_has_module(st, type)) return;

    /* Find the highest complete ring and assign to next free slot */
    int target_ring = 1;
    for (int r = MAX_RING_COUNT - 1; r >= 1; r--) {
        if (station_has_ring(st, r)) { target_ring = r; break; }
    }
    int target_slot = station_ring_free_slot(st, target_ring, RING_PORT_COUNT[target_ring]);
    if (target_slot < 0) return; /* ring is full */

    station_module_t *m = &st->modules[st->module_count++];
    m->type = type;
    m->ring = (uint8_t)target_ring;
    m->slot = (uint8_t)target_slot;
    m->scaffold = true;
    m->build_progress = 0.0f;

    /* Generate a supply contract for the required material */
    float cost = module_build_cost(type);
    commodity_t material = module_build_material(type);
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active) {
            w->contracts[k] = (contract_t){
                .active = true, .action = CONTRACT_SUPPLY,
                .station_index = (uint8_t)station_idx,
                .commodity = material,
                .quantity_needed = cost,
                .base_price = 25.0f, .age = 0.0f,
                .target_index = -1, .claimed_by = -1,
            };
            break;
        }
    }
    SIM_LOG("[sim] began construction of module %d at station %d (cost %.0f of commodity %d)\n",
            type, station_idx, cost, material);
}

/* Deliver ingots to scaffold modules at a station */
void step_module_delivery(world_t *w, station_t *st, int station_idx, ship_t *ship) {
    for (int i = 0; i < st->module_count; i++) {
        if (!st->modules[i].scaffold) continue;
        commodity_t mat = module_build_material(st->modules[i].type);
        if (ship->cargo[mat] < 0.01f) continue;
        float cost = module_build_cost(st->modules[i].type);
        float needed = cost * (1.0f - st->modules[i].build_progress);
        if (needed < 0.01f) continue;
        float deliver = fminf(ship->cargo[mat], needed);
        ship->cargo[mat] -= deliver;
        st->modules[i].build_progress += deliver / cost;
        if (st->modules[i].build_progress >= 1.0f) {
            st->modules[i].scaffold = false;
            st->modules[i].build_progress = 1.0f;
            rebuild_station_services(st);
            rebuild_signal_chain(w);
            /* Spawn NPC when production module activates */
            if (st->modules[i].type == MODULE_FURNACE || st->modules[i].type == MODULE_FURNACE_CU || st->modules[i].type == MODULE_FURNACE_CR)
                spawn_npc(w, station_idx, NPC_ROLE_MINER);
            if (st->modules[i].type == MODULE_FRAME_PRESS || st->modules[i].type == MODULE_LASER_FAB || st->modules[i].type == MODULE_TRACTOR_FAB)
                spawn_npc(w, station_idx, NPC_ROLE_HAULER);
            SIM_LOG("[sim] module %d activated at station %d\n", st->modules[i].type, station_idx);
        }
        if (ship->cargo[mat] < 0.01f) continue;
    }
}

/* Spawn an NPC at a station. Returns slot index or -1 if full. */
static int spawn_npc(world_t *w, int station_idx, npc_role_t role) {
    int slot = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w->npc_ships[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;
    station_t *st = &w->stations[station_idx];
    hull_class_t hc = (role == NPC_ROLE_MINER) ? HULL_CLASS_NPC_MINER : HULL_CLASS_HAULER;
    npc_ship_t *npc = &w->npc_ships[slot];
    memset(npc, 0, sizeof(*npc));
    npc->active = true;
    npc->role = role;
    npc->hull_class = hc;
    npc->state = NPC_STATE_DOCKED;
    npc->pos = v2_add(st->pos, v2(30.0f * (float)(slot % 3 - 1), -(st->radius + HULL_DEFS[hc].ship_radius + 50.0f)));
    npc->angle = PI_F * 0.5f;
    npc->target_asteroid = -1;
    npc->home_station = station_idx;
    npc->dest_station = station_idx;
    npc->state_timer = (role == NPC_ROLE_MINER) ? NPC_DOCK_TIME : HAULER_DOCK_TIME;
    npc->tint_r = 1.0f; npc->tint_g = 1.0f; npc->tint_b = 1.0f;
    SIM_LOG("[sim] spawned %s at station %d (slot %d)\n",
            role == NPC_ROLE_MINER ? "miner" : "hauler", station_idx, slot);
    return slot;
}

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

/* Place an outpost at pos using scaffold kit from cargo.
 * Returns the station slot index on success, -1 on failure.
 * Must NOT be called in player_only_mode (client prediction). */
int try_place_outpost(world_t *w, server_player_t *sp, vec2 pos) {
    if (w->player_only_mode) return -1;
    if (sp->docked) return -1;
    if (!sp->ship.has_scaffold_kit) return -1;
    if (!can_place_outpost(w, pos)) return -1;

    /* Find free slot */
    int slot = -1;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) { slot = s; break; }
    }
    if (slot < 0) return -1;

    station_t *st = &w->stations[slot];
    memset(st, 0, sizeof(*st));
    /* Generate a name from the position hash */
    {
        static const char *prefixes[] = {
            "Far", "Deep", "Outer", "Edge", "Drift", "Void", "Pale",
            "Iron", "Cold", "Dark", "High", "Low", "Red", "Dim",
            "Rust", "Ash", "Grim", "Last", "Lost", "Worn",
        };
        static const char *suffixes[] = {
            "Reach", "Point", "Gate", "Rock", "Anchor", "Post",
            "Haven", "Mark", "Light", "Hold", "Watch", "Ridge",
            "Cairn", "Spur", "Ledge", "Pike", "Notch", "Forge",
            "Well", "Yard",
        };
        uint32_t h = (uint32_t)(pos.x * 7.13f) ^ (uint32_t)(pos.y * 13.37f) ^ (uint32_t)slot;
        h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
        int pi = (int)(h % 20);
        int si = (int)((h >> 8) % 20);
        snprintf(st->name, sizeof(st->name), "%s %s", prefixes[pi], suffixes[si]);
    }
    st->pos = pos;
    st->radius = OUTPOST_RADIUS;
    st->dock_radius = OUTPOST_DOCK_RADIUS;
    st->signal_range = OUTPOST_SIGNAL_RANGE;
    st->scaffold = true;
    st->scaffold_progress = 0.0f;
    add_module_at(st, MODULE_DOCK, 0, 0xFF);
    add_module_at(st, MODULE_SIGNAL_RELAY, 0, 0xFF);
    rebuild_station_services(st);

    /* Generate supply contract for outpost construction */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active) {
            w->contracts[k] = (contract_t){
                .active = true, .action = CONTRACT_SUPPLY,
                .station_index = (uint8_t)slot,
                .commodity = COMMODITY_FRAME,
                .quantity_needed = SCAFFOLD_MATERIAL_NEEDED,
                .base_price = 25.0f, .age = 0.0f,
                .target_index = -1, .claimed_by = -1,
            };
            break;
        }
    }

    emit_event(w, (sim_event_t){
        .type = SIM_EVENT_OUTPOST_PLACED,
        .player_id = sp->id,
        .outpost_placed = { .slot = slot },
    });
    SIM_LOG("[sim] player %d placed outpost at (%.0f, %.0f) in slot %d\n",
            sp->id, pos.x, pos.y, slot);
    return slot;
}

static bool point_within_signal_margin(const world_t *w, vec2 pos, float margin) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w->stations[s])) continue;
        float range = w->stations[s].signal_range;
        float max_dist = range + margin;
        if (v2_dist_sq(pos, w->stations[s].pos) <= max_dist * max_dist) {
            return true;
        }
    }
    return false;
}

/* ================================================================== */
/* Commodity / ship helpers                                           */
/* ================================================================== */

static void clear_ship_cargo(ship_t *s) {
    memset(s->cargo, 0, sizeof(s->cargo));
}

static float ship_cargo_space(const ship_t *s) {
    return fmaxf(0.0f, ship_cargo_capacity(s) - ship_total_cargo(s));
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

static vec2 station_dock_anchor(const station_t *station, const hull_def_t *hull) {
    if (!station) return v2(0.0f, 0.0f);
    return v2_add(station->pos, v2(0.0f, -(station->radius + hull->ship_radius + STATION_DOCK_APPROACH_OFFSET)));
}

static bool station_has_service(const station_t *station, uint32_t service) {
    return station && ((station->services & service) != 0);
}

static float sim_station_repair_cost(const ship_t *s) {
    float missing = fmaxf(0.0f, ship_max_hull(s) - s->hull);
    return ceilf(missing * STATION_REPAIR_COST_PER_HULL);
}

/* ================================================================== */
/* Asteroid lifecycle                                                 */
/* ================================================================== */

static void sim_configure_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity) {
    float sl = asteroid_spin_limit(tier);
    a->active    = true;
    a->tier      = tier;
    a->commodity = commodity;
    a->radius    = rand_range(w, asteroid_radius_min(tier), asteroid_radius_max(tier));
    a->max_hp    = rand_range(w, asteroid_hp_min(tier), asteroid_hp_max(tier));
    a->hp        = a->max_hp;
    a->max_ore   = 0.0f;
    a->ore       = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        a->max_ore = rand_range(w, 8.0f, 14.0f);
        a->ore     = a->max_ore;
    }
    a->rotation = rand_range(w, 0.0f, TWO_PI_F);
    a->spin     = rand_range(w, -sl, sl);
    a->seed     = rand_range(w, 0.0f, 100.0f);
    a->age      = 0.0f;
    a->net_dirty = true;
}

static asteroid_tier_t random_field_asteroid_tier(world_t *w) {
    float roll = randf(w);
    if (roll < 0.03f) return ASTEROID_TIER_XXL;
    if (roll < 0.26f) return ASTEROID_TIER_XL;
    if (roll < 0.70f) return ASTEROID_TIER_L;
    return ASTEROID_TIER_M;
}

static float max_signal_range(const world_t *w) {
    float best = 0.0f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (w->stations[i].signal_range > best) best = w->stations[i].signal_range;
    }
    return best > 0.0f ? best : WORLD_RADIUS;
}

/* Pick a random active station (skip empty slots). */
static int pick_active_station(world_t *w) {
    int active[MAX_STATIONS];
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++)
        if (station_provides_signal(&w->stations[s])) active[count++] = s;
    if (count == 0) return 0;
    return active[rand_int(w, 0, count - 1)];
}

/* Find a good clump center in the belt density field near signal-covered space.
 * Uses gradient walk: sample random points, then step toward higher density. */
static vec2 find_belt_clump_center(world_t *w, float *out_density) {
    vec2 best_pos = v2(0.0f, 0.0f);
    float best_density = 0.0f;
    for (int attempt = 0; attempt < 16; attempt++) {
        int stn = pick_active_station(w);
        float angle = rand_range(w, 0.0f, TWO_PI_F);
        float distance = rand_range(w, 200.0f, w->stations[stn].signal_range * 0.85f);
        vec2 pos = v2_add(w->stations[stn].pos, v2(cosf(angle) * distance, sinf(angle) * distance));
        float d = belt_density_at(&w->belt, pos.x, pos.y);
        /* Gradient walk: take 4 steps toward higher density */
        float step = 200.0f;
        for (int g = 0; g < 4; g++) {
            float dx = belt_density_at(&w->belt, pos.x + step, pos.y) - belt_density_at(&w->belt, pos.x - step, pos.y);
            float dy = belt_density_at(&w->belt, pos.x, pos.y + step) - belt_density_at(&w->belt, pos.x, pos.y - step);
            float glen = sqrtf(dx * dx + dy * dy);
            if (glen > 0.001f) {
                pos.x += dx / glen * step;
                pos.y += dy / glen * step;
            }
            d = belt_density_at(&w->belt, pos.x, pos.y);
            step *= 0.6f;
        }
        if (d > best_density) {
            best_density = d;
            best_pos = pos;
        }
        if (d > 0.3f) break;
    }
    if (out_density) *out_density = best_density;
    return best_pos;
}

/*
 * Seed a clump of asteroids at a belt position.
 * Clumps are irregular blobs: 1 anchor (XL/XXL), several medium, debris fill.
 * Returns number of asteroids placed.
 */
static int seed_asteroid_clump(world_t *w, int first_slot) {
    float density = 0.0f;
    vec2 center = find_belt_clump_center(w, &density);
    if (density < 0.05f) return 0;

    commodity_t ore = belt_ore_at(&w->belt, center.x, center.y);

    /* Clump size scales with density: 3-12 rocks */
    int clump_size = 3 + (int)(density * 9.0f);
    float clump_radius = 200.0f + density * 400.0f;

    /* Elongation: stretch the clump along a random axis */
    float stretch_angle = rand_range(w, 0.0f, TWO_PI_F);
    float stretch_factor = rand_range(w, 1.0f, 2.5f);
    float cos_s = cosf(stretch_angle);
    float sin_s = sinf(stretch_angle);

    /* Shared drift velocity for the clump */
    vec2 drift = v2(rand_range(w, -3.0f, 3.0f), rand_range(w, -3.0f, 3.0f));

    int placed = 0;
    for (int i = 0; i < clump_size && (first_slot + placed) < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[first_slot + placed];
        if (a->active) continue;

        /* Pick tier: first rock is the anchor, rest are smaller */
        asteroid_tier_t tier;
        if (i == 0) {
            tier = (randf(w) < 0.15f) ? ASTEROID_TIER_XXL : ASTEROID_TIER_XL;
        } else if (i <= 3) {
            tier = (randf(w) < 0.4f) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
        } else {
            tier = (randf(w) < 0.3f) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
        }

        clear_asteroid(a);
        sim_configure_asteroid(w, a, tier, ore);
        a->fracture_child = false;

        /* Scatter around center with elongation */
        float r = rand_range(w, 0.0f, clump_radius) * sqrtf(randf(w)); /* sqrt for uniform disk */
        float theta = rand_range(w, 0.0f, TWO_PI_F);
        float lx = cosf(theta) * r;
        float ly = sinf(theta) * r;
        /* Apply stretch */
        float sx = lx * cos_s - ly * sin_s;
        float sy = lx * sin_s + ly * cos_s;
        sx *= stretch_factor;
        float fx = sx * cos_s + sy * sin_s;
        float fy = -sx * sin_s + sy * cos_s;

        a->pos = v2_add(center, v2(fx, fy));
        a->vel = v2_add(drift, v2(rand_range(w, -2.0f, 2.0f), rand_range(w, -2.0f, 2.0f)));
        placed++;
    }
    return placed;
}

/* Seed a single asteroid at a belt position (for respawn/compat). */
static void seed_field_asteroid_of_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier) {
    float density = 0.0f;
    vec2 pos = find_belt_clump_center(w, &density);
    commodity_t ore = belt_ore_at(&w->belt, pos.x, pos.y);
    clear_asteroid(a);
    sim_configure_asteroid(w, a, tier, ore);
    a->fracture_child = false;
    a->pos = pos;
    a->vel = v2(rand_range(w, -4.0f, 4.0f), rand_range(w, -4.0f, 4.0f));
}

static void set_inbound_field_velocity(world_t *w, asteroid_t *a, vec2 inward) {
    float speed_lo = 12.0f, speed_hi = 20.0f, tangent_jitter = 6.0f;
    switch (a->tier) {
    case ASTEROID_TIER_XXL:
        speed_lo = 18.0f; speed_hi = 30.0f; tangent_jitter = 4.0f;
        break;
    case ASTEROID_TIER_XL:
        speed_lo = 16.0f; speed_hi = 26.0f; tangent_jitter = 5.0f;
        break;
    case ASTEROID_TIER_L:
        speed_lo = 14.0f; speed_hi = 22.0f; tangent_jitter = 6.0f;
        break;
    case ASTEROID_TIER_M:
        speed_lo = 12.0f; speed_hi = 18.0f; tangent_jitter = 7.0f;
        break;
    case ASTEROID_TIER_S:
    default:
        break;
    }
    vec2 tangent = v2_perp(inward);
    a->vel = v2_add(v2_scale(inward, rand_range(w, speed_lo, speed_hi)),
                    v2_scale(tangent, rand_range(w, -tangent_jitter, tangent_jitter)));
}

static void spawn_inbound_field_asteroid_of_tier(world_t *w, asteroid_t *a, asteroid_tier_t tier) {
    clear_asteroid(a);
    a->fracture_child = false;

    /* Spawn at 30-60% of signal range — close enough to reach the action
     * in 1-3 minutes, not 10-15. Prefer belt-dense positions. */
    int stn = pick_active_station(w);
    vec2 center = w->stations[stn].pos;
    float sr = w->stations[stn].signal_range;
    if (sr <= 0.0f) sr = max_signal_range(w);

    vec2 spawn_pos = center;
    vec2 inward = v2(-1.0f, 0.0f);
    float best_density = 0.0f;

    for (int attempt = 0; attempt < 32; attempt++) {
        float angle = rand_range(w, 0.0f, TWO_PI_F);
        vec2 outward = v2_from_angle(angle);
        float dist = rand_range(w, sr * 0.30f, sr * 0.60f);
        vec2 pos = v2_add(center, v2_scale(outward, dist));
        float d = belt_density_at(&w->belt, pos.x, pos.y);
        if (d > best_density) {
            best_density = d;
            spawn_pos = pos;
            inward = v2_scale(outward, -1.0f);
        }
        if (d > 0.15f) break;
    }

    commodity_t ore = belt_ore_at(&w->belt, spawn_pos.x, spawn_pos.y);
    sim_configure_asteroid(w, a, tier, ore);
    a->pos = spawn_pos;
    set_inbound_field_velocity(w, a, inward);
}

static void spawn_field_asteroid(world_t *w, asteroid_t *a) {
    spawn_inbound_field_asteroid_of_tier(w, a, random_field_asteroid_tier(w));
}

static void spawn_child_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid(a);
    sim_configure_asteroid(w, a, tier, commodity);
    a->fracture_child = true;
    a->pos = pos;
    a->vel = vel;
}

static int desired_child_count(world_t *w, asteroid_tier_t tier) {
    switch (tier) {
    case ASTEROID_TIER_XXL: return rand_int(w, 8, 14);
    case ASTEROID_TIER_XL: return rand_int(w, 2, 3);
    case ASTEROID_TIER_L:  return rand_int(w, 2, 3);
    case ASTEROID_TIER_M:  return rand_int(w, 2, 4);
    default: return 0;
    }
}

static void inspect_asteroid_field(world_t *w, int *seeded_count, int *first_inactive_slot) {
    *seeded_count = 0;
    *first_inactive_slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) {
            if (*first_inactive_slot < 0) *first_inactive_slot = i;
            continue;
        }
        if (!w->asteroids[i].fracture_child) (*seeded_count)++;
    }
}

static void fracture_asteroid(world_t *w, int idx, vec2 outward_dir) {
    asteroid_t parent = w->asteroids[idx];
    asteroid_tier_t child_tier = asteroid_next_tier(parent.tier);
    int desired = desired_child_count(w, parent.tier);
    int child_slots[16] = { idx, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    int child_count = 1;

    for (int i = 0; i < MAX_ASTEROIDS && child_count < desired; i++) {
        if (i == idx || w->asteroids[i].active) continue;
        child_slots[child_count++] = i;
    }

    float base_angle = atan2f(outward_dir.y, outward_dir.x);
    for (int i = 0; i < child_count; i++) {
        float spread_t = (child_count == 1) ? 0.0f : (((float)i / (float)(child_count - 1)) - 0.5f);
        float child_angle = base_angle + (spread_t * 1.35f) + rand_range(w, -0.14f, 0.14f);
        vec2 dir = v2_from_angle(child_angle);
        vec2 tangent = v2_perp(dir);
        asteroid_t *child = &w->asteroids[child_slots[i]];
        spawn_child_asteroid(w, child, child_tier, parent.commodity, parent.pos, parent.vel);
        vec2 cpos = v2_add(parent.pos, v2_scale(dir, (parent.radius * 0.28f) + (child->radius * 0.85f)));
        float drift = rand_range(w, 22.0f, 56.0f);
        vec2 cvel = v2_add(parent.vel, v2_add(v2_scale(dir, drift), v2_scale(tangent, rand_range(w, -10.0f, 10.0f))));
        child->pos = cpos;
        child->vel = cvel;
    }

    /* audio_play_fracture removed */
    SIM_LOG("[sim] asteroid %d fractured into %d children\n", idx, child_count);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_FRACTURE, .fracture.tier = parent.tier});
}

/* ================================================================== */
/* Per-frame world systems                                            */
/* ================================================================== */

static void sim_step_asteroid_dynamics(world_t *w, float dt) {
    float cleanup_d_sq = FRACTURE_CHILD_CLEANUP_DISTANCE * FRACTURE_CHILD_CLEANUP_DISTANCE;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;

        a->rotation += a->spin * dt;
        a->pos = v2_add(a->pos, v2_scale(a->vel, dt));
        a->vel = v2_scale(a->vel, 1.0f / (1.0f + (0.42f * dt)));
        a->age += dt;

        /* Despawn asteroids that leave station-supported space. */
        if (!point_within_signal_margin(w, a->pos, a->radius + 260.0f)) {
            clear_asteroid(a);
            continue;
        }

        /* Cleanup old fracture children far from ALL players */
        if (a->fracture_child && a->age >= FRACTURE_CHILD_CLEANUP_AGE) {
            bool near_player = false;
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (!w->players[p].connected) continue;
                if (v2_dist_sq(a->pos, w->players[p].ship.pos) <= cleanup_d_sq) {
                    near_player = true;
                    break;
                }
            }
            if (!near_player) clear_asteroid(a);
        }
    }
}

static void maintain_asteroid_field(world_t *w, float dt) {
    int seeded = 0, first_slot = -1;
    inspect_asteroid_field(w, &seeded, &first_slot);
    if (seeded >= FIELD_ASTEROID_TARGET) { w->field_spawn_timer = 0.0f; return; }
    w->field_spawn_timer += dt;
    if (w->field_spawn_timer < FIELD_ASTEROID_RESPAWN_DELAY) return;
    if (first_slot >= 0) {
        /* Spawn a small wave of 2-4 inbound rocks from the belt edge */
        int wave = 2 + rand_int(w, 0, 2);
        int spawned = 0;
        for (int i = first_slot; i < MAX_ASTEROIDS && spawned < wave; i++) {
            if (w->asteroids[i].active) continue;
            spawn_field_asteroid(w, &w->asteroids[i]);
            spawned++;
        }
    }
    w->field_spawn_timer = 0.0f;
}

static bool sim_can_smelt_ore(const station_t *st, commodity_t ore) {
    switch (ore) {
        case COMMODITY_FERRITE_ORE: return station_has_module(st, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE: return station_has_module(st, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE: return station_has_module(st, MODULE_FURNACE_CR);
        default: return false;
    }
}

static void sim_step_refinery_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_has_module(st, MODULE_FURNACE)
            && !station_has_module(st, MODULE_FURNACE_CU)
            && !station_has_module(st, MODULE_FURNACE_CR)) continue;

        int active = 0;
        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++)
            if (st->inventory[i] > 0.01f && sim_can_smelt_ore(st, (commodity_t)i)) active++;
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;
        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            commodity_t ore = (commodity_t)i;
            if (!sim_can_smelt_ore(st, ore)) continue;
            if (st->inventory[ore] <= 0.01f) continue;
            commodity_t ingot = commodity_refined_form(ore);
            float room = MAX_PRODUCT_STOCK - st->inventory[ingot];
            if (room <= 0.01f) continue;
            float consume = fminf(fminf(st->inventory[ore], rate * dt), room);
            st->inventory[ore] -= consume;
            st->inventory[ingot] += consume;
        }
    }
}

static void sim_step_station_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (station_has_module(st, MODULE_FRAME_PRESS)) {
            if (st->inventory[COMMODITY_FRAME] < MAX_PRODUCT_STOCK) {
                float buf = st->inventory[COMMODITY_FERRITE_INGOT];
                if (buf > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->inventory[COMMODITY_FRAME];
                    float consume = fminf(buf, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->inventory[COMMODITY_FERRITE_INGOT] -= consume;
                    st->inventory[COMMODITY_FRAME] += consume;
                }
            }
        }
        if (station_has_module(st, MODULE_LASER_FAB)) {
            if (st->inventory[COMMODITY_LASER_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_co = st->inventory[COMMODITY_CUPRITE_INGOT];
                if (buf_co > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->inventory[COMMODITY_LASER_MODULE];
                    float consume = fminf(buf_co, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->inventory[COMMODITY_CUPRITE_INGOT] -= consume;
                    st->inventory[COMMODITY_LASER_MODULE] += consume;
                }
            }
        }
        if (station_has_module(st, MODULE_TRACTOR_FAB)) {
            if (st->inventory[COMMODITY_TRACTOR_MODULE] < MAX_PRODUCT_STOCK) {
                float buf_ln = st->inventory[COMMODITY_CRYSTAL_INGOT];
                if (buf_ln > 0.01f) {
                    float room = MAX_PRODUCT_STOCK - st->inventory[COMMODITY_TRACTOR_MODULE];
                    float consume = fminf(buf_ln, fminf(STATION_PRODUCTION_RATE * dt, room));
                    st->inventory[COMMODITY_CRYSTAL_INGOT] -= consume;
                    st->inventory[COMMODITY_TRACTOR_MODULE] += consume;
                }
            }
        }
    }
}

/* ================================================================== */
/* NPC ships                                                          */
/* ================================================================== */

static float npc_total_cargo(const npc_ship_t *npc) {
    float t = 0.0f;
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) t += npc->cargo[i];
    return t;
}

static bool npc_target_valid(const world_t *w, const npc_ship_t *npc) {
    if (npc->target_asteroid < 0 || npc->target_asteroid >= MAX_ASTEROIDS) return false;
    const asteroid_t *a = &w->asteroids[npc->target_asteroid];
    return a->active && a->tier != ASTEROID_TIER_S;
}

static int npc_find_mineable_asteroid(const world_t *w, const npc_ship_t *npc) {
    /* Priority: DESTROY contract targets first */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (!w->contracts[k].active || w->contracts[k].action != CONTRACT_DESTROY) continue;
        int idx = w->contracts[k].target_index;
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active) continue;
        /* Check not already taken by another miner */
        bool taken = false;
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            if (&w->npc_ships[n] == npc) continue;
            if (w->npc_ships[n].active && w->npc_ships[n].role == NPC_ROLE_MINER &&
                w->npc_ships[n].target_asteroid == idx) { taken = true; break; }
        }
        if (!taken) return idx;
    }

    /* Normal: find nearest mineable asteroid */
    int best = -1;
    float best_d = 1e18f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        if (signal_strength_at(w, a->pos) < 0.66f) continue;
        /* Skip asteroids already targeted by another miner */
        bool taken = false;
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            if (&w->npc_ships[n] == npc) continue;
            if (w->npc_ships[n].active && w->npc_ships[n].role == NPC_ROLE_MINER &&
                w->npc_ships[n].target_asteroid == i) { taken = true; break; }
        }
        if (taken) continue;
        float d = v2_dist_sq(npc->pos, a->pos);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void npc_steer_toward(npc_ship_t *npc, vec2 target, float accel, float turn_speed, float dt) {
    vec2 delta = v2_sub(target, npc->pos);
    float desired = atan2f(delta.y, delta.x);
    float diff = wrap_angle(desired - npc->angle);
    float max_turn = turn_speed * dt;
    if (diff > max_turn) diff = max_turn;
    else if (diff < -max_turn) diff = -max_turn;
    npc->angle = wrap_angle(npc->angle + diff);
    vec2 fwd = v2_from_angle(npc->angle);
    npc->vel = v2_add(npc->vel, v2_scale(fwd, accel * dt));
    npc->thrusting = accel > 0.0f;
}

static void npc_apply_physics(npc_ship_t *npc, float drag, float dt, const world_t *w) {
    npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (drag * dt)));
    npc->pos = v2_add(npc->pos, v2_scale(npc->vel, dt));
    /* Signal-based boundary: NPCs stay in strong signal (66%+) */
    float sig = signal_strength_at(w, npc->pos);
    if (sig < 0.66f) {
        /* Find nearest station and its signal edge distance */
        float best_d_sq = 1e18f;
        int best_s = 0;
        for (int i = 0; i < MAX_STATIONS; i++) {
            float d_sq = v2_dist_sq(npc->pos, w->stations[i].pos);
            if (d_sq < best_d_sq) { best_d_sq = d_sq; best_s = i; }
        }
        vec2 to_station = v2_sub(w->stations[best_s].pos, npc->pos);
        float d = sqrtf(v2_len_sq(to_station));
        if (d > 0.001f) {
            /* Push proportional to overshoot past signal edge (like old WORLD_RADIUS) */
            float edge = w->stations[best_s].signal_range;
            float overshoot = fmaxf(0.0f, d - edge);
            float push_strength = overshoot * 0.08f;
            vec2 push = v2_scale(to_station, push_strength / d);
            npc->vel = v2_add(npc->vel, push);
        }
    }
}

static void npc_resolve_station_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        float minimum = st->radius + 4.0f + hull->ship_radius;
        vec2 delta = v2_sub(npc->pos, st->pos);
        float d_sq = v2_len_sq(delta);
        if (d_sq >= minimum * minimum) continue;
        float d = sqrtf(d_sq);
        vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
        npc->pos = v2_add(st->pos, v2_scale(normal, minimum));
        float vel_toward = v2_dot(npc->vel, normal);
        if (vel_toward < 0.0f)
            npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.2f));
    }
}

static void npc_resolve_asteroid_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || asteroid_is_collectible(a)) continue;
        float minimum = a->radius + hull->ship_radius;
        vec2 delta = v2_sub(npc->pos, a->pos);
        float d_sq = v2_len_sq(delta);
        if (d_sq >= minimum * minimum) continue;
        float d = sqrtf(d_sq);
        vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
        npc->pos = v2_add(a->pos, v2_scale(normal, minimum));
        float vel_toward = v2_dot(npc->vel, normal);
        if (vel_toward < 0.0f)
            npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.2f));
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
    if (npc->dest_station < 0 || npc->dest_station >= MAX_STATIONS ||
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
            for (int c = 0; c < INGOT_COUNT; c++) carried += npc->ingots[c];
            float space = hull->ingot_capacity - carried;
            bool loaded = false;

            /* Contract-driven routing: find highest-value fillable contract */
            int best_contract = -1;
            float best_score = 0.0f;
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (!w->contracts[k].active) continue;
                if (w->contracts[k].action != CONTRACT_SUPPLY) continue;
                if (w->contracts[k].station_index >= MAX_STATIONS) continue;
                commodity_t c = w->contracts[k].commodity;
                if (c < COMMODITY_RAW_ORE_COUNT) continue; /* haulers carry ingots only */
                if (home->inventory[c] < 0.5f) continue; /* no stock to fill */
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
                float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                float take = fminf(avail, space);
                if (take > 0.5f) {
                    npc->ingots[INGOT_IDX(ingot)] += take;
                    home->inventory[ingot] -= take;
                    loaded = true;
                }
            } else {
                /* Fallback: original round-trip behavior (leave reserve for players) */
                station_t *dest = &w->stations[npc->dest_station];
                if (station_has_module(dest, MODULE_FRAME_PRESS)) {
                    commodity_t ingot = COMMODITY_FERRITE_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->ingots[INGOT_IDX(ingot)] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
                    }
                }
                if (!loaded && station_has_module(dest, MODULE_LASER_FAB)) {
                    commodity_t ingot = COMMODITY_CUPRITE_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->ingots[INGOT_IDX(ingot)] += take;
                        home->inventory[ingot] -= take;
                        space -= take;
                        loaded = true;
                    }
                }
                if (!loaded && station_has_module(dest, MODULE_TRACTOR_FAB)) {
                    commodity_t ingot = COMMODITY_CRYSTAL_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->ingots[INGOT_IDX(ingot)] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
                    }
                }
            }
            float total_carried = 0.0f;
            for (int c = 0; c < INGOT_COUNT; c++) total_carried += npc->ingots[c];
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) total_carried += npc->cargo[c];
            if (total_carried < 0.01f) {
                /* Nothing at home — relocate to a station with surplus ingots */
                int best_src = -1;
                float best_stock = 0.0f;
                for (int s = 0; s < MAX_STATIONS; s++) {
                    if (s == npc->home_station) continue;
                    if (!station_is_active(&w->stations[s])) continue;
                    float stock = 0.0f;
                    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                        stock += fmaxf(0.0f, w->stations[s].inventory[c] - HAULER_RESERVE);
                    if (stock > best_stock) { best_stock = stock; best_src = s; }
                }
                if (best_src >= 0 && best_stock > 0.5f) {
                    /* Relocate: fly to the surplus station, dock, and load next cycle */
                    npc->home_station = best_src;
                    npc->state = NPC_STATE_RETURN_TO_STATION;
                } else {
                    npc->state_timer = HAULER_DOCK_TIME;  /* nothing anywhere, wait */
                }
            } else {
                npc->state = NPC_STATE_TRAVEL_TO_DEST;
            }
        }
        break;
    }
    case NPC_STATE_TRAVEL_TO_DEST: {
        station_t *dest = &w->stations[npc->dest_station];
        npc_steer_toward(npc, dest->pos, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt, w);
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
            for (int i = 0; i < INGOT_COUNT; i++) {
                dest->inventory[COMMODITY_RAW_ORE_COUNT + i] += npc->ingots[i];
                if (dest->inventory[COMMODITY_RAW_ORE_COUNT + i] > MAX_PRODUCT_STOCK)
                    dest->inventory[COMMODITY_RAW_ORE_COUNT + i] = MAX_PRODUCT_STOCK;
                npc->ingots[i] = 0.0f;
            }
            /* Hauler also delivers ingots to scaffold station and modules */
            if (dest->scaffold || dest->module_count > 0) {
                /* Feed from station inventory into scaffolds */
                ship_t hauler_ship = {0};
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
                    hauler_ship.cargo[c] = dest->inventory[c];
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
                step_module_delivery(w, dest, npc->dest_station, &hauler_ship);
                /* Put remaining back */
                for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                    float consumed = dest->inventory[c] - hauler_ship.cargo[c];
                    if (consumed > 0.01f) dest->inventory[c] -= consumed;
                }
            }
            npc->state = NPC_STATE_RETURN_TO_STATION;
        }
        break;
    }
    case NPC_STATE_RETURN_TO_STATION: {
        station_t *home = &w->stations[npc->home_station];
        npc_steer_toward(npc, home->pos, hull->accel, hull->turn_speed, dt);
        npc_apply_physics(npc, hull->drag, dt, w);
        float dock_r = home->dock_radius * 0.7f;
        if (v2_dist_sq(npc->pos, home->pos) < dock_r * dock_r) {
            npc->vel = v2(0.0f, 0.0f);
            npc->pos = v2_add(home->pos, v2(50.0f * (float)(n % 2 == 0 ? -1 : 1), -(home->radius + hull->ship_radius + 70.0f)));
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

static void step_npc_ships(world_t *w, float dt) {
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        npc_ship_t *npc = &w->npc_ships[n];
        if (!npc->active) continue;
        npc->thrusting = false;
        npc_validate_stations(w, npc);

        if (npc->role == NPC_ROLE_HAULER) {
            step_hauler(w, npc, n, dt);
            if (npc->state != NPC_STATE_DOCKED) {
                npc_resolve_station_collisions(w, npc);
                npc_resolve_asteroid_collisions(w, npc);
            }
            continue;
        }

        const hull_def_t *hull = npc_hull_def(npc);
        switch (npc->state) {
        case NPC_STATE_DOCKED: {
            npc->state_timer -= dt;
            npc->vel = v2(0.0f, 0.0f);
            if (npc->state_timer <= 0.0f) {
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
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) npc->target_asteroid = target;
                else { npc->target_asteroid = -1; npc->state = NPC_STATE_RETURN_TO_STATION; break; }
            }
            asteroid_t *a = &w->asteroids[npc->target_asteroid];
            npc_steer_toward(npc, a->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt, w);
            if (v2_dist_sq(npc->pos, a->pos) < MINING_RANGE * MINING_RANGE)
                npc->state = NPC_STATE_MINING;
            break;
        }
        case NPC_STATE_MINING: {
            if (!npc_target_valid(w, npc)) {
                if (npc_total_cargo(npc) > 0.5f) {
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
            float approach = standoff + 20.0f;

            if (dist_sq > approach * approach) {
                npc_steer_toward(npc, a->pos, hull->accel, hull->turn_speed, dt);
                npc_apply_physics(npc, hull->drag, dt, w);
                break;
            }

            vec2 face_dir = v2_sub(a->pos, npc->pos);
            float desired = atan2f(face_dir.y, face_dir.x);
            float diff = wrap_angle(desired - npc->angle);
            float max_turn = hull->turn_speed * dt;
            if (diff > max_turn) diff = max_turn;
            else if (diff < -max_turn) diff = -max_turn;
            npc->angle = wrap_angle(npc->angle + diff);

            if (dist_sq < standoff * standoff) {
                vec2 away = v2_norm(v2_sub(npc->pos, a->pos));
                npc->vel = v2_add(npc->vel, v2_scale(away, hull->accel * 0.5f * dt));
            }
            npc->vel = v2_scale(npc->vel, 1.0f / (1.0f + (4.0f * dt)));
            npc_apply_physics(npc, hull->drag, dt, w);

            float mined = hull->mining_rate * dt;
            mined = fminf(mined, a->hp);
            a->hp -= mined;
            a->net_dirty = true;

            float cs = hull->ore_capacity - npc_total_cargo(npc);
            float ore_gained = fminf(mined * 0.15f, cs);
            if (ore_gained > 0.0f) npc->cargo[a->commodity] += ore_gained;

            if (a->hp <= 0.01f) {
                vec2 outward = v2_norm(v2_sub(a->pos, npc->pos));
                fracture_asteroid(w, npc->target_asteroid, outward);
                npc->target_asteroid = -1;
            }
            if (cs <= 0.5f) {
                npc->state = NPC_STATE_RETURN_TO_STATION;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_RETURN_TO_STATION: {
            station_t *home = &w->stations[npc->home_station];
            npc_steer_toward(npc, home->pos, hull->accel, hull->turn_speed, dt);
            npc_apply_physics(npc, hull->drag, dt, w);
            float dock_r = home->dock_radius * 0.7f;
            if (v2_dist_sq(npc->pos, home->pos) < dock_r * dock_r) {
                npc->vel = v2(0.0f, 0.0f);
                npc->pos = v2_add(home->pos, v2(30.0f * (float)(n % 3 - 1), -(home->radius + hull->ship_radius + 50.0f)));
                if (station_has_module(home, MODULE_FURNACE) || station_has_module(home, MODULE_FURNACE_CU) || station_has_module(home, MODULE_FURNACE_CR)) {
                    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
                        float space = REFINERY_HOPPER_CAPACITY - home->inventory[i];
                        float deposit = fminf(npc->cargo[i], fmaxf(0.0f, space));
                        home->inventory[i] += deposit;
                        npc->cargo[i] -= deposit;
                    }
                }
                npc->state = NPC_STATE_DOCKED;
                npc->state_timer = NPC_DOCK_TIME;
                npc->target_asteroid = -1;
            }
            break;
        }
        case NPC_STATE_IDLE: {
            npc_apply_physics(npc, hull->drag, dt, w);
            npc->state_timer -= dt;
            if (npc->state_timer <= 0.0f) {
                int target = npc_find_mineable_asteroid(w, npc);
                if (target >= 0) { npc->target_asteroid = target; npc->state = NPC_STATE_TRAVEL_TO_ASTEROID; }
                else npc->state_timer = 3.0f;
            }
            break;
        }
        default: break;
        }

        /* NPC collision with stations and asteroids */
        if (npc->state != NPC_STATE_DOCKED) {
            npc_resolve_station_collisions(w, npc);
            npc_resolve_asteroid_collisions(w, npc);
        }

        /* Blend tint toward dominant cargo color.
         * Ore colors: ferrite=(0.55, 0.25, 0.18), cuprite=(0.22, 0.30, 0.50), crystal=(0.25, 0.48, 0.30) */
        static const float ore_r[3] = {0.55f, 0.22f, 0.25f};
        static const float ore_g[3] = {0.25f, 0.30f, 0.48f};
        static const float ore_b[3] = {0.18f, 0.50f, 0.30f};
        float total = 0.0f;
        float target_r = 1.0f, target_g = 1.0f, target_b = 1.0f;
        if (npc->role == NPC_ROLE_MINER) {
            for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) total += npc->cargo[c];
        } else {
            for (int c = 0; c < INGOT_COUNT; c++) total += npc->ingots[c];
        }
        if (total > 1.0f) {
            target_r = 0.0f; target_g = 0.0f; target_b = 0.0f;
            int count = (npc->role == NPC_ROLE_MINER) ? COMMODITY_RAW_ORE_COUNT : INGOT_COUNT;
            const float *cargo = (npc->role == NPC_ROLE_MINER) ? npc->cargo : npc->ingots;
            for (int c = 0; c < count; c++) {
                float w_c = cargo[c] / total;
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
static void generate_npc_distress_contracts(world_t *w) {
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
            if (w->contracts[k].active && w->contracts[k].action == CONTRACT_DESTROY
                && w->contracts[k].target_index == blocker) {
                exists = true; break;
            }
        }
        if (exists) continue;
        /* Post distress contract */
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (!w->contracts[k].active) {
                w->contracts[k] = (contract_t){
                    .active = true, .action = CONTRACT_DESTROY,
                    .station_index = (uint8_t)npc->home_station,
                    .target_pos = w->asteroids[blocker].pos,
                    .target_index = blocker,
                    .base_price = 30.0f, .age = 0.0f,
                    .claimed_by = -1,
                };
                break;
            }
        }
    }
}

/* ================================================================== */
/* Player ship helpers                                                */
/* ================================================================== */

/* ship_forward, ship_muzzle: see ship.h/c */

static bool try_spend_credits(ship_t *s, float amount) {
    if (amount <= 0.0f) return true;
    if (s->credits + 0.01f < amount) return false;
    s->credits = fmaxf(0.0f, s->credits - amount);
    return true;
}

static void anchor_ship_in_station(server_player_t *sp, world_t *w) {
    const station_t *st = &w->stations[sp->current_station];
    const hull_def_t *hull = ship_hull_def(&sp->ship);
    vec2 base = station_dock_anchor(st, hull);
    /* Offset by player ID so multiple docked players don't overlap */
    float offset = (float)(sp->id % 4) * (hull->ship_radius * 2.5f) - (hull->ship_radius * 3.75f);
    sp->ship.pos = v2_add(base, v2(offset, 0.0f));
    sp->ship.vel = v2(0.0f, 0.0f);
}

static void apply_ship_damage(world_t *w, server_player_t *sp, float damage);

static void dock_ship(world_t *w, server_player_t *sp) {
    if (sp->nearby_station >= 0) sp->current_station = sp->nearby_station;
    sp->docked = true;
    sp->in_dock_range = true;
    /* Keep ship at its current position (already in dock range) — just stop it */
    sp->ship.vel = v2(0.0f, 0.0f);
    SIM_LOG("[sim] player %d docked at station %d\n", sp->id, sp->current_station);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_DOCK, .player_id = sp->id});
}

static void launch_ship(world_t *w, server_player_t *sp) {
    sp->docked = false;
    sp->in_dock_range = false;  /* prevent immediate re-dock */
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
    SIM_LOG("[sim] player %d launched\n", sp->id);
    emit_event(w, (sim_event_t){.type = SIM_EVENT_LAUNCH, .player_id = sp->id});
}

static void emergency_recover_ship(world_t *w, server_player_t *sp) {
    clear_ship_cargo(&sp->ship);
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.angle = PI_F * 0.5f;
    dock_ship(w, sp);
    SIM_LOG("[sim] player %d emergency recovered\n", sp->id);
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

static void resolve_ship_circle(world_t *w, server_player_t *sp, vec2 center, float radius) {
    float minimum = radius + ship_hull_def(&sp->ship)->ship_radius;
    vec2 delta = v2_sub(sp->ship.pos, center);
    float d_sq = v2_len_sq(delta);
    if (d_sq >= minimum * minimum) return;
    float d = sqrtf(d_sq);
    vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
    sp->ship.pos = v2_add(center, v2_scale(normal, minimum));
    float vel_toward = v2_dot(sp->ship.vel, normal);
    if (vel_toward < 0.0f) {
        float impact = -vel_toward;
        if (!sp->docked && impact > SHIP_COLLISION_DAMAGE_THRESHOLD)
            apply_ship_damage(w, sp, (impact - SHIP_COLLISION_DAMAGE_THRESHOLD) * SHIP_COLLISION_DAMAGE_SCALE);
        sp->ship.vel = v2_sub(sp->ship.vel, v2_scale(normal, vel_toward * 1.2f));
    }
}

/* ================================================================== */
/* Mining target                                                      */
/* ================================================================== */

/* Max asteroid tier mineable at each laser level:
 * Level 0: M, Level 1: L, Level 2: XL, Level 3: XXL, Level 4: all */
static asteroid_tier_t max_mineable_tier(int mining_level) {
    switch (mining_level) {
        case 0: return ASTEROID_TIER_M;
        case 1: return ASTEROID_TIER_L;
        case 2: return ASTEROID_TIER_XL;
        default: return ASTEROID_TIER_XXL;
    }
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

static void try_sell_station_cargo(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    float payout = 0.0f;

    /* Station buys its primary input commodity from the player */
    commodity_t buy = station_primary_buy(st);
    if ((int)buy >= 0 && sp->ship.cargo[buy] > 0.01f) {
        float capacity = (buy < COMMODITY_RAW_ORE_COUNT)
            ? REFINERY_HOPPER_CAPACITY : MAX_PRODUCT_STOCK;
        float space = capacity - st->inventory[buy];
        if (space > 0.01f) {
            float accepted = fminf(sp->ship.cargo[buy], space);
            float price = station_buy_price(st, buy);
            /* Check for active contract bonus */
            for (int k = 0; k < MAX_CONTRACTS; k++) {
                if (w->contracts[k].active && w->contracts[k].action == CONTRACT_SUPPLY
                    && w->contracts[k].station_index == sp->current_station
                    && w->contracts[k].commodity == buy) {
                    price = contract_price(&w->contracts[k]);
                    w->contracts[k].quantity_needed -= accepted;
                    if (w->contracts[k].quantity_needed <= 0.01f) {
                        w->contracts[k].active = false;
                        emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE,
                            .contract_complete.action = CONTRACT_SUPPLY});
                    }
                    break;
                }
            }
            payout += accepted * price;
            st->inventory[buy] += accepted;
            sp->ship.cargo[buy] -= accepted;
        }
    }

    /* Also deliver any cargo matching active supply contracts at this station */
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        contract_t *ct = &w->contracts[k];
        if (!ct->active || ct->action != CONTRACT_SUPPLY) continue;
        if (ct->station_index != sp->current_station) continue;
        commodity_t c = ct->commodity;
        if (c == buy) continue; /* already handled above */
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
            ct->active = false;
            emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE,
                .contract_complete.action = CONTRACT_SUPPLY});
        }
    }

    if (payout > 0.01f) {
        sp->ship.credits += payout;
        SIM_LOG("[sim] player %d sold cargo for %.0f cr\n", sp->id, payout);
        emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = sp->id});
    }
}

static void try_repair_ship(world_t *w, server_player_t *sp) {
    station_t *st = &w->stations[sp->current_station];
    if (!station_has_service(st, STATION_SERVICE_REPAIR)) return;
    float cost = sim_station_repair_cost(&sp->ship);
    if (cost <= 0.0f) return;
    if (!try_spend_credits(&sp->ship, cost)) return;
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
    if (!try_spend_credits(&sp->ship, (float)cost)) return;
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

static void step_ship_thrust(ship_t *s, float dt, float thrust_input, vec2 forward) {
    const hull_def_t *hull = ship_hull_def(s);
    if (thrust_input > 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, hull->accel * thrust_input * dt));
    } else if (thrust_input < 0.0f) {
        s->vel = v2_add(s->vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
    }
}

static void step_ship_motion(ship_t *s, float dt, const world_t *w) {
    s->vel = v2_scale(s->vel, 1.0f / (1.0f + (ship_hull_def(s)->drag * dt)));
    s->pos = v2_add(s->pos, v2_scale(s->vel, dt));

    /* Signal-based boundary: push back when signal is weak */
    float sig = signal_strength_at(w, s->pos);
    if (sig < 0.15f) {
        /* Find nearest station and its signal edge distance */
        float best_d_sq = 1e18f;
        int best_s = 0;
        for (int i = 0; i < MAX_STATIONS; i++) {
            float d_sq = v2_dist_sq(s->pos, w->stations[i].pos);
            if (d_sq < best_d_sq) { best_d_sq = d_sq; best_s = i; }
        }
        vec2 to_station = v2_sub(w->stations[best_s].pos, s->pos);
        float d = sqrtf(v2_len_sq(to_station));
        if (d > 0.001f) {
            /* Push proportional to overshoot past signal edge (like old WORLD_RADIUS) */
            float edge = w->stations[best_s].signal_range;
            float overshoot = fmaxf(0.0f, d - edge);
            float push_strength = overshoot * 0.08f;
            vec2 push = v2_scale(to_station, push_strength / d);
            s->vel = v2_add(s->vel, push);
        }
    }
}

static void resolve_world_collisions(world_t *w, server_player_t *sp) {
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_collides(&w->stations[i])) continue;
        resolve_ship_circle(w, sp, w->stations[i].pos, w->stations[i].radius + 4.0f);
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active || asteroid_is_collectible(&w->asteroids[i])) continue;
        resolve_ship_circle(w, sp, w->asteroids[i].pos, w->asteroids[i].radius);
    }
}

static void update_docking_state(world_t *w, server_player_t *sp, float dt) {
    if (sp->docked) {
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
        sp->ship.vel = v2(0.0f, 0.0f);  /* hold position, don't teleport */
        return;
    }
    float best_d = 0.0f;
    sp->nearby_station = -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        float dr_sq = w->stations[i].dock_radius * w->stations[i].dock_radius;
        float d = v2_dist_sq(sp->ship.pos, w->stations[i].pos);
        if (d <= dr_sq && (sp->nearby_station < 0 || d < best_d)) { best_d = d; sp->nearby_station = i; }
    }
    sp->in_dock_range = sp->nearby_station >= 0;
    if (sp->in_dock_range)
        sp->ship.vel = v2_scale(sp->ship.vel, 1.0f / (1.0f + (dt * 2.2f)));
}

static void update_targeting_state(world_t *w, server_player_t *sp, vec2 forward) {
    vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);
    /* Prefer client's mining target hint if valid, in range, and in front */
    int hint = sp->input.mining_target_hint;
    if (hint >= 0 && hint < MAX_ASTEROIDS && w->asteroids[hint].active
        && !asteroid_is_collectible(&w->asteroids[hint])) {
        const asteroid_t *a = &w->asteroids[hint];
        vec2 to_a = v2_sub(a->pos, muzzle);
        float proj = v2_dot(to_a, forward);
        float perp = fabsf(v2_cross(to_a, forward));
        float surface_dist = proj - sqrtf(fmaxf(0.0f, a->radius * a->radius - perp * perp));
        if (perp <= a->radius && surface_dist >= -a->radius && surface_dist <= MINING_RANGE) {
            sp->hover_asteroid = hint;
            return;
        }
    }
    sp->hover_asteroid = sim_find_mining_target(w, muzzle, forward, sp->ship.mining_level);
}

static void step_fragment_collection(world_t *w, server_player_t *sp, float dt) {
    float nearby_sq = FRAGMENT_NEARBY_RANGE * FRAGMENT_NEARBY_RANGE;
    float tr = ship_tractor_range(&sp->ship);
    float tr_sq = tr * tr;
    float cs = ship_cargo_space(&sp->ship);
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!asteroid_is_collectible(a)) continue;
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float d_sq = v2_len_sq(to_ship);
        if (d_sq <= nearby_sq) sp->nearby_fragments++;
        if (cs <= 0.0f) continue;
        if (d_sq <= tr_sq) {
            float d = sqrtf(d_sq);
            float pull = 1.0f - clampf(d / tr, 0.0f, 1.0f);
            vec2 pull_dir = d > 0.001f ? v2_scale(to_ship, 1.0f / d) : ship_forward(sp->ship.angle);
            sp->tractor_fragments++;
            a->vel = v2_add(a->vel, v2_scale(pull_dir, FRAGMENT_TRACTOR_ACCEL * lerpf(0.35f, 1.0f, pull) * dt));
            float speed = v2_len(a->vel);
            if (speed > FRAGMENT_MAX_SPEED) a->vel = v2_scale(v2_norm(a->vel), FRAGMENT_MAX_SPEED);
        }
        float cr = ship_collect_radius(&sp->ship) + a->radius;
        if (d_sq <= cr * cr) {
            float recovered = fminf(a->ore, cs);
            if (recovered <= 0.0f) continue;
            sp->ship.cargo[a->commodity] += recovered;
            cs -= recovered;
            a->ore -= recovered;
            if (a->ore <= 0.01f) {
                clear_asteroid(a);
            } else if (a->max_ore > 0.0f) {
                a->radius = lerpf(asteroid_radius_min(ASTEROID_TIER_S) * 0.72f,
                                  asteroid_radius_max(ASTEROID_TIER_S),
                                  asteroid_progress_ratio(a));
            }
        }
    }
}

static void step_mining_system(world_t *w, server_player_t *sp, float dt, bool mining, vec2 forward) {
    sp->beam_active = false;
    sp->beam_hit = false;
    sp->beam_ineffective = false;
    if (!mining) return;

    vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);
    sp->beam_active = true;
    sp->beam_start = muzzle;

    if (sp->hover_asteroid >= 0) {
        asteroid_t *a = &w->asteroids[sp->hover_asteroid];
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
                float mining_sig = signal_strength_at(w, sp->ship.pos);
                float mined = ship_mining_rate(&sp->ship) * dt * (0.2f + 0.8f * mining_sig);
                mined = fminf(mined, a->hp);
                a->hp -= mined;
                a->net_dirty = true;
                if (a->hp <= 0.01f)
                    fracture_asteroid(w, sp->hover_asteroid, normal);
            }
        }
    } else {
        sp->beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
    }
}

static void step_station_interaction_system(world_t *w, server_player_t *sp, const input_intent_t *intent) {
    /* Buy scaffold kit: docked at station with blueprint desk */
    if (intent->buy_scaffold_kit && sp->docked && !w->player_only_mode) {
        station_t *st = &w->stations[sp->current_station];
        if (station_has_module(st, MODULE_BLUEPRINT_DESK)
            && sp->ship.credits >= OUTPOST_CREDIT_COST
            && !sp->ship.has_scaffold_kit) {
            sp->ship.credits -= OUTPOST_CREDIT_COST;
            sp->ship.has_scaffold_kit = true;
            SIM_LOG("[sim] player %d bought scaffold kit\n", sp->id);
        }
        /* Don't return — allow other intents (like interact/launch) to process */
    }
    /* Outpost placement: must be undocked with scaffold kit */
    if (intent->place_outpost && !sp->docked && sp->ship.has_scaffold_kit) {
        vec2 forward = v2_from_angle(sp->ship.angle);
        vec2 place_pos = v2_add(sp->ship.pos, v2_scale(forward, 150.0f));
        int slot = try_place_outpost(w, sp, place_pos);
        if (slot >= 0) sp->ship.has_scaffold_kit = false;
        return;
    }
    if (intent->interact) {
        if (sp->docked) { launch_ship(w, sp); return; }
        if (!sp->in_dock_range) return;
        dock_ship(w, sp);
        return;
    }
    if (!sp->docked) return;
    station_t *docked_st = &w->stations[sp->current_station];
    /* Module construction: player requests to build a module */
    if (intent->build_module && !w->player_only_mode) {
        float cost = module_credit_cost(intent->build_module_type);
        /* Find target ring and slot before committing credits */
        int target_ring = 1;
        for (int r = MAX_RING_COUNT - 1; r >= 1; r--) {
            if (station_has_ring(docked_st, r)) { target_ring = r; break; }
        }
        int target_slot = station_ring_free_slot(docked_st, target_ring, RING_PORT_COUNT[target_ring]);
        if (sp->ship.credits >= cost
            && !station_has_module(docked_st, intent->build_module_type)
            && docked_st->module_count < MAX_MODULES_PER_STATION
            && target_slot >= 0) {
            sp->ship.credits -= cost;
            begin_module_construction(w, docked_st, sp->current_station, intent->build_module_type);
        }
    }
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
            float afford = (price_per > 0.01f) ? floorf(sp->ship.credits / price_per) : 0.0f;
            float amount = fminf(fminf(available, space), afford);
            float total_cost = amount * price_per;
            if (amount > 0.01f) {
                sp->ship.credits -= total_cost;
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
        float d = sqrtf(v2_dist_sq(pos, w->players[i].ship.pos));
        if (d < 200.0f) {
            float strength = (200.0f - d) / 200.0f;
            interference += strength * 0.5f;
        }
    }

    /* Large asteroids — mass creates interference */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        float range = a->radius * 3.0f;
        float d = sqrtf(v2_dist_sq(pos, a->pos));
        if (d < range) {
            float strength = (range - d) / range;
            float mass_factor = a->radius / 80.0f;  /* bigger = more interference */
            interference += strength * mass_factor * 0.15f;
        }
    }

    return clampf(interference, 0.0f, 0.7f);  /* cap at 70% interference */
}

static void step_player(world_t *w, server_player_t *sp, float dt) {
    sp->hover_asteroid = -1;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    if (!sp->docked) {
        /* Signal attenuation: scale controls by station signal strength */
        float sig = signal_strength_at(w, sp->ship.pos);
        float signal_scale = 0.3f + 0.7f * sig; /* 30% minimum at zero signal */
        float turn_input = sp->input.turn * signal_scale;
        float thrust_input = sp->input.thrust * signal_scale;

        /* Signal interference: nearby objects add noise to controls */
        float interference = calc_signal_interference(w, sp);
        if (interference > 0.01f) {
            /* Add jitter to controls proportional to interference.
             * Use a local RNG seeded from player position to avoid
             * mutating world RNG state (bug 47). */
            uint32_t local_rng = (uint32_t)(sp->ship.pos.x * 1000.0f) ^ (uint32_t)(sp->ship.pos.y * 1000.0f) ^ ((uint32_t)sp->id * 0x9E3779B9u);
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
        step_ship_thrust(&sp->ship, dt, thrust_input, forward);
        step_ship_motion(&sp->ship, dt, w);
        resolve_world_collisions(w, sp);
        update_docking_state(w, sp, dt);
        /* In client prediction mode (player_only_mode), skip station
         * interactions — the server is authoritative for dock/launch,
         * sell, repair, and upgrades.  This prevents snap-back flicker
         * when the client predicts an action before the server confirms. */
        if (!w->player_only_mode)
            step_station_interaction_system(w, sp, &sp->input);
        if (!sp->docked) {
            update_targeting_state(w, sp, forward);
            step_mining_system(w, sp, dt, sp->input.mine, forward);
            if (!w->player_only_mode)
                step_fragment_collection(w, sp, dt);
        }
    } else {
        update_docking_state(w, sp, dt);
        if (!w->player_only_mode)
            step_station_interaction_system(w, sp, &sp->input);
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
    sp->input.build_module = false;
    sp->input.buy_product = false;
}

/* ================================================================== */
/* Asteroid-asteroid gravity                                          */
/* ================================================================== */

static void step_asteroid_gravity(world_t *w, float dt) {
    /* Asteroid-asteroid attraction (non-S tier, within 400 units) */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        for (int j = i + 1; j < MAX_ASTEROIDS; j++) {
            asteroid_t *b = &w->asteroids[j];
            if (!b->active || b->tier == ASTEROID_TIER_S) continue;
            vec2 delta = v2_sub(b->pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq > 800.0f * 800.0f || dist_sq < 1.0f) continue;
            float dist = sqrtf(dist_sq);
            /* Don't attract asteroids at or inside collision boundary */
            float min_dist = a->radius + b->radius;
            if (dist < min_dist * 1.3f) continue; /* dead zone: 30% beyond contact */
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float mass_a = a->radius * a->radius;
            float mass_b = b->radius * b->radius;
            /* Gravitational force proportional to both masses.
             * Clamp against the lighter body so swapping slots cannot
             * change the result while preserving equal/opposite force. */
            float force_mag = (mass_a * mass_b) / dist_sq * 14.0f;
            float max_force = 60.0f * fminf(mass_a, mass_b);
            if (force_mag > max_force) force_mag = max_force;
            /* F = ma, so acceleration = force / mass */
            vec2 accel_a = v2_scale(normal, (force_mag / mass_a) * dt);
            vec2 accel_b = v2_scale(normal, -(force_mag / mass_b) * dt);
            a->vel = v2_add(a->vel, accel_a);
            b->vel = v2_add(b->vel, accel_b);
        }
    }

    /* Station attraction (asteroids within 800 units of a station) */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            vec2 delta = v2_sub(w->stations[s].pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq > 800.0f * 800.0f || dist_sq < 1.0f) continue;
            float dist = sqrtf(dist_sq);
            /* Don't attract asteroids that are at or inside collision boundary */
            float min_dist = a->radius + w->stations[s].radius;
            if (dist < min_dist + 10.0f) continue;
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float force = w->stations[s].radius / (dist * 0.8f) * 2.0f;
            float mass_a = a->radius * a->radius;
            float accel = force / mass_a;
            a->vel = v2_add(a->vel, v2_scale(normal, accel * dt));
        }
    }

    /* Weak-signal current keeps isolated field rocks drifting inward. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;

        bool near_player = false;
        for (int p = 0; p < MAX_PLAYERS; p++) {
            if (!w->players[p].connected) continue;
            if (v2_dist_sq(a->pos, w->players[p].ship.pos) <= 600.0f * 600.0f) {
                near_player = true;
                break;
            }
        }
        if (near_player) continue;

        bool near_asteroid = false;
        for (int j = 0; j < MAX_ASTEROIDS; j++) {
            if (j == i || !w->asteroids[j].active) continue;
            if (v2_dist_sq(a->pos, w->asteroids[j].pos) <= 400.0f * 400.0f) {
                near_asteroid = true;
                break;
            }
        }
        if (near_asteroid) continue;

        float best_signal = 0.0f;
        int best_station = -1;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_provides_signal(&w->stations[s])) continue;
            float dist = sqrtf(v2_dist_sq(a->pos, w->stations[s].pos));
            float strength = fmaxf(0.0f, 1.0f - (dist / w->stations[s].signal_range));
            if (strength > best_signal) {
                best_signal = strength;
                best_station = s;
            }
        }
        if (best_station < 0 || best_signal <= 0.0f || best_signal >= 0.75f) continue;

        vec2 delta = v2_sub(w->stations[best_station].pos, a->pos);
        float dist_sq = v2_len_sq(delta);
        if (dist_sq < 1.0f) continue;
        float dist = sqrtf(dist_sq);
        float min_dist = a->radius + w->stations[best_station].radius;
        if (dist < min_dist + 10.0f) continue;

        vec2 normal = v2_scale(delta, 1.0f / dist);
        float current = (0.75f - best_signal) / 0.75f;
        a->vel = v2_add(a->vel, v2_scale(normal, 3.0f * current * dt));
    }
}

/* ================================================================== */
/* Asteroid-asteroid collision                                        */
/* ================================================================== */

static void resolve_asteroid_collisions(world_t *w) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int j = i + 1; j < MAX_ASTEROIDS; j++) {
            asteroid_t *b = &w->asteroids[j];
            if (!b->active) continue;
            /* Skip if both are S tier */
            if (a->tier == ASTEROID_TIER_S && b->tier == ASTEROID_TIER_S) continue;
            float min_dist = a->radius + b->radius;
            vec2 delta = v2_sub(a->pos, b->pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq >= min_dist * min_dist) continue;
            float dist = sqrtf(dist_sq);
            if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float overlap = min_dist - dist;
            /* Push apart: heavier (larger radius) moves less */
            float mass_a = a->radius * a->radius;
            float mass_b = b->radius * b->radius;
            float total_mass = mass_a + mass_b;
            float ratio_a = mass_b / total_mass; /* a moves proportional to b's mass */
            float ratio_b = mass_a / total_mass;
            a->pos = v2_add(a->pos, v2_scale(normal, overlap * ratio_a));
            b->pos = v2_sub(b->pos, v2_scale(normal, overlap * ratio_b));
            /* Transfer velocity along collision normal */
            float rel_vel = v2_dot(v2_sub(a->vel, b->vel), normal);
            if (rel_vel < 0.0f) {
                vec2 impulse_a = v2_scale(normal, rel_vel * ratio_a);
                vec2 impulse_b = v2_scale(normal, rel_vel * ratio_b);
                a->vel = v2_sub(a->vel, impulse_a);
                b->vel = v2_add(b->vel, impulse_b);
            }
        }
    }
}

/* ================================================================== */
/* Asteroid-station collision                                         */
/* ================================================================== */

static void resolve_asteroid_station_collisions(world_t *w) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            float min_dist = a->radius + w->stations[s].radius;
            vec2 delta = v2_sub(a->pos, w->stations[s].pos);
            float dist_sq = v2_len_sq(delta);
            if (dist_sq >= min_dist * min_dist) continue;
            float dist = sqrtf(dist_sq);
            if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
            vec2 normal = v2_scale(delta, 1.0f / dist);
            float overlap = min_dist - dist;
            /* Push asteroid out (station is immovable) with extra margin */
            float push = overlap + 8.0f;
            a->pos = v2_add(a->pos, v2_scale(normal, push));
            /* Bounce velocity with restitution 0.6 */
            float vel_along = v2_dot(a->vel, normal);
            float impact_speed = fabsf(vel_along);
            if (vel_along < 0.0f) {
                a->vel = v2_sub(a->vel, v2_scale(normal, vel_along * 1.6f));
            }
            /* High-speed impact damages asteroid */
            if (impact_speed > 100.0f) {
                float damage = impact_speed * 0.3f;
                a->hp -= damage;
                a->net_dirty = true;
                if (a->hp <= 0.0f) {
                    /* Fracture the asteroid */
                    vec2 outward = v2_scale(normal, -1.0f);
                    fracture_asteroid(w, i, outward);
                }
            }
        }
    }
}

/* ================================================================== */
/* Contract system                                                    */
/* ================================================================== */

float contract_price(const contract_t *c) {
    /* Price escalates with age: +20% per 5 minutes */
    float escalation = 1.0f + (c->age / 300.0f) * 0.2f;
    return c->base_price * escalation;
}

static void step_contracts(world_t *w, float dt) {
    /* Age existing contracts and check fulfillment */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!w->contracts[i].active) continue;
        w->contracts[i].age += dt;

        switch (w->contracts[i].action) {
        case CONTRACT_SUPPLY: {
            /* Close when station buffer is sufficiently full */
            if (w->contracts[i].station_index >= MAX_STATIONS) break;
            station_t *st = &w->stations[w->contracts[i].station_index];
            commodity_t c = w->contracts[i].commodity;
            float current = st->inventory[c];
            float threshold = (c < COMMODITY_RAW_ORE_COUNT) ? REFINERY_HOPPER_CAPACITY * 0.8f : MAX_PRODUCT_STOCK * 0.8f;
            if (current >= threshold) {
                w->contracts[i].active = false;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_SUPPLY});
            }
            break;
        }
        case CONTRACT_DESTROY: {
            /* Close when target asteroid is gone or index invalid */
            int idx = w->contracts[i].target_index;
            bool target_gone = (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active);
            if (target_gone) {
                w->contracts[i].active = false;
                emit_event(w, (sim_event_t){.type = SIM_EVENT_CONTRACT_COMPLETE, .contract_complete.action = CONTRACT_DESTROY});
            }
            /* Expire after 60 seconds if unfulfilled */
            if (w->contracts[i].active && w->contracts[i].age > 60.0f) w->contracts[i].active = false;
            break;
        }
        case CONTRACT_SCAN: {
            /* Expire after 120 seconds */
            if (w->contracts[i].age > 120.0f) w->contracts[i].active = false;
            break;
        }
        }
    }

    /* Generate ONE contract per station — its top need.
     * Priority: scaffold modules > empty hoppers > empty ingot buffers.
     * Skip if station already has an active contract. */
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (!station_exists(st)) continue;

        /* Check if this station already has an active contract */
        bool has_contract = false;
        for (int k = 0; k < MAX_CONTRACTS; k++) {
            if (w->contracts[k].active && w->contracts[k].station_index == s) {
                has_contract = true; break;
            }
        }
        if (has_contract) continue;

        /* Evaluate station's top need */
        contract_t need = {0};
        need.target_index = -1;
        need.claimed_by = -1;

        /* Priority 1: scaffold modules need ingots */
        for (int m = 0; m < st->module_count; m++) {
            if (!st->modules[m].scaffold) continue;
            float cost = module_build_cost(st->modules[m].type);
            float remaining = cost * (1.0f - st->modules[m].build_progress);
            if (remaining > 0.5f) {
                need = (contract_t){
                    .active = true, .action = CONTRACT_SUPPLY,
                    .station_index = (uint8_t)s,
                    .commodity = module_build_material(st->modules[m].type),
                    .quantity_needed = remaining,
                    .base_price = 25.0f,
                    .target_index = -1, .claimed_by = -1,
                };
                break;
            }
        }

        /* Priority 2: station scaffold needs frames */
        if (!need.active && st->scaffold) {
            float remaining = SCAFFOLD_MATERIAL_NEEDED * (1.0f - st->scaffold_progress);
            if (remaining > 0.5f) {
                need = (contract_t){
                    .active = true, .action = CONTRACT_SUPPLY,
                    .station_index = (uint8_t)s,
                    .commodity = COMMODITY_FRAME,
                    .quantity_needed = remaining,
                    .base_price = 25.0f,
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 3: ore hopper with biggest deficit (only for ore types this station can smelt) */
        if (!need.active && (station_has_module(st, MODULE_FURNACE)
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
                    .active = true, .action = CONTRACT_SUPPLY,
                    .station_index = (uint8_t)s,
                    .commodity = (commodity_t)worst_ore,
                    .quantity_needed = worst_deficit,
                    .base_price = st->base_price[worst_ore],
                    .target_index = -1, .claimed_by = -1,
                };
            }
        }

        /* Priority 4: ingot buffer deficit (for production stations) */
        if (!need.active) {
            struct { module_type_t mod; commodity_t ingot; float price; } checks[] = {
                { MODULE_FRAME_PRESS, COMMODITY_FERRITE_INGOT, 20.0f },
                { MODULE_LASER_FAB, COMMODITY_CUPRITE_INGOT, 22.0f },
                { MODULE_TRACTOR_FAB, COMMODITY_CRYSTAL_INGOT, 22.0f },
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
                    .active = true, .action = CONTRACT_SUPPLY,
                    .station_index = (uint8_t)s,
                    .commodity = checks[worst_idx].ingot,
                    .quantity_needed = worst_deficit,
                    .base_price = checks[worst_idx].price,
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
/* Public: world_sim_step                                             */
/* ================================================================== */

void world_sim_step(world_t *w, float dt) {
    w->events.count = 0;
    w->time += dt;
    /* Advance ring rotations */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) continue;
        for (int r = 1; r < MAX_RING_COUNT; r++) {
            w->stations[s].ring_rotation[r] += RING_SPEED[r] * dt;
            if (w->stations[s].ring_rotation[r] > TWO_PI_F)
                w->stations[s].ring_rotation[r] -= TWO_PI_F;
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
    sim_step_refinery_production(w, dt);
    sim_step_station_production(w, dt);
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

void world_reset(world_t *w) {
    uint32_t seed = w->rng;  /* caller may pre-set seed; 0 = default */
    memset(w, 0, sizeof(*w));
    w->rng = seed ? seed : 2037u;
    belt_field_init(&w->belt, w->rng, WORLD_RADIUS);

    /* --- Stations --- */
    snprintf(w->stations[0].name, sizeof(w->stations[0].name), "%s", "Prospect Refinery");
    w->stations[0].pos         = v2(0.0f, -2400.0f);
    w->stations[0].radius      = 62.0f;
    w->stations[0].dock_radius = 132.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[0].signal_range = 18000.0f;
    add_module_at(&w->stations[0], MODULE_DOCK, 0, 0xFF);
    add_module_at(&w->stations[0], MODULE_RING, 1, 0xFF);
    add_module_at(&w->stations[0], MODULE_ORE_BUYER, 1, 0);
    add_module_at(&w->stations[0], MODULE_FURNACE, 1, 1);
    add_module_at(&w->stations[0], MODULE_REPAIR_BAY, 1, 2);
    add_module_at(&w->stations[0], MODULE_CONTRACT_BOARD, 1, 3);
    rebuild_station_services(&w->stations[0]);

    snprintf(w->stations[1].name, sizeof(w->stations[1].name), "%s", "Kepler Yard");
    w->stations[1].pos         = v2(-3200.0f, 2300.0f);
    w->stations[1].radius      = 56.0f;
    w->stations[1].dock_radius = 124.0f;
    w->stations[1].signal_range = 15000.0f;
    w->stations[1].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[1].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[1].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[1].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[1].base_price[COMMODITY_FRAME] = 20.0f;
    add_module_at(&w->stations[1], MODULE_DOCK, 0, 0xFF);
    add_module_at(&w->stations[1], MODULE_RING, 1, 0xFF);
    add_module_at(&w->stations[1], MODULE_FRAME_PRESS, 1, 0);
    add_module_at(&w->stations[1], MODULE_REPAIR_BAY, 1, 1);
    add_module_at(&w->stations[1], MODULE_CONTRACT_BOARD, 1, 2);
    add_module_at(&w->stations[1], MODULE_BLUEPRINT_DESK, 1, 3);
    rebuild_station_services(&w->stations[1]);

    snprintf(w->stations[2].name, sizeof(w->stations[2].name), "%s", "Helios Works");
    w->stations[2].pos         = v2(3200.0f, 2300.0f);
    w->stations[2].radius      = 56.0f;
    w->stations[2].dock_radius = 124.0f;
    w->stations[2].signal_range = 15000.0f;
    w->stations[2].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[2].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[2].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[2].base_price[COMMODITY_LASER_MODULE] = 28.0f;
    w->stations[2].base_price[COMMODITY_TRACTOR_MODULE] = 36.0f;
    add_module_at(&w->stations[2], MODULE_DOCK, 0, 0xFF);
    add_module_at(&w->stations[2], MODULE_RING, 1, 0xFF);
    add_module_at(&w->stations[2], MODULE_LASER_FAB, 1, 0);
    add_module_at(&w->stations[2], MODULE_TRACTOR_FAB, 1, 1);
    add_module_at(&w->stations[2], MODULE_REPAIR_BAY, 1, 2);
    add_module_at(&w->stations[2], MODULE_CONTRACT_BOARD, 1, 3);
    add_module_at(&w->stations[2], MODULE_BLUEPRINT_DESK, 1, 4);
    rebuild_station_services(&w->stations[2]);
    rebuild_signal_chain(w);

    /* --- Initial asteroid field: spawn as clumps along belt density --- */
    {
        int slot = 0;
        while (slot < FIELD_ASTEROID_TARGET && slot < MAX_ASTEROIDS) {
            int placed = seed_asteroid_clump(w, slot);
            if (placed == 0) {
                /* Fallback: single rock if no good clump center found */
                seed_field_asteroid_of_tier(w, &w->asteroids[slot], random_field_asteroid_tier(w));
                placed = 1;
            }
            slot += placed;
        }
    }

    /* --- NPC ships: 2 miners at refinery, 2 haulers for logistics --- */
    spawn_npc(w, 0, NPC_ROLE_MINER);
    spawn_npc(w, 0, NPC_ROLE_MINER);
    spawn_npc(w, 0, NPC_ROLE_HAULER);
    spawn_npc(w, 0, NPC_ROLE_HAULER);

    SIM_LOG("[sim] world reset complete (%d asteroids, 4 NPCs)\n", FIELD_ASTEROID_TARGET);
}

/* ================================================================== */
/* Public: player_init_ship                                           */
/* ================================================================== */

void player_init_ship(server_player_t *sp, world_t *w) {
    memset(&sp->ship, 0, sizeof(sp->ship));
    sp->ship.hull_class = HULL_CLASS_MINER;
    sp->ship.hull       = HULL_DEFS[HULL_CLASS_MINER].max_hull;
    sp->ship.credits    = 0.0f;
    sp->ship.angle      = PI_F * 0.5f;
    sp->docked          = true;
    sp->current_station = 0;
    sp->nearby_station  = 0;
    sp->in_dock_range   = true;
    sp->hover_asteroid  = -1;
    anchor_ship_in_station(sp, w);
}

/* ================================================================== */
/* World persistence                                                   */
/* ================================================================== */

#define SAVE_MAGIC 0x5349474E  /* "SIGN" */
#define SAVE_VERSION 12

/* ---- helper macros for explicit field I/O ---- */
#define WRITE_FIELD(f, val) do { if (fwrite(&(val), sizeof(val), 1, (f)) != 1) { fclose(f); return false; } } while(0)
#define READ_FIELD(f, val)  do { if (fread(&(val), sizeof(val), 1, (f)) != 1)  { fclose(f); return false; } } while(0)

/* ---- station field-by-field I/O ---- */
static bool write_station(FILE *f, const station_t *s) {
    WRITE_FIELD(f, s->name);
    { uint32_t reserved = 0; WRITE_FIELD(f, reserved); } /* was: role */
    WRITE_FIELD(f, s->pos);
    WRITE_FIELD(f, s->radius);
    WRITE_FIELD(f, s->dock_radius);
    WRITE_FIELD(f, s->signal_range);
    WRITE_FIELD(f, s->scaffold);
    WRITE_FIELD(f, s->scaffold_progress);
    WRITE_FIELD(f, s->base_price);
    WRITE_FIELD(f, s->inventory);
    WRITE_FIELD(f, s->services);
    /* Modules */
    WRITE_FIELD(f, s->module_count);
    for (int m = 0; m < s->module_count && m < MAX_MODULES_PER_STATION; m++) {
        WRITE_FIELD(f, s->modules[m]);
    }
    return true;
}

static bool read_station(FILE *f, station_t *s) {
    READ_FIELD(f, s->name);
    { uint32_t reserved; READ_FIELD(f, reserved); (void)reserved; } /* was: role */
    READ_FIELD(f, s->pos);
    READ_FIELD(f, s->radius);
    READ_FIELD(f, s->dock_radius);
    READ_FIELD(f, s->signal_range);
    READ_FIELD(f, s->scaffold);
    READ_FIELD(f, s->scaffold_progress);
    READ_FIELD(f, s->base_price);
    READ_FIELD(f, s->inventory);
    READ_FIELD(f, s->services);
    /* Modules */
    READ_FIELD(f, s->module_count);
    if (s->module_count < 0) s->module_count = 0;
    if (s->module_count > MAX_MODULES_PER_STATION) s->module_count = MAX_MODULES_PER_STATION;
    for (int m = 0; m < s->module_count; m++) {
        READ_FIELD(f, s->modules[m]);
    }
    return true;
}

/* ---- asteroid field-by-field I/O ---- */
static bool write_asteroid(FILE *f, const asteroid_t *a) {
    WRITE_FIELD(f, a->active);
    WRITE_FIELD(f, a->fracture_child);
    WRITE_FIELD(f, a->tier);
    WRITE_FIELD(f, a->pos);
    WRITE_FIELD(f, a->vel);
    WRITE_FIELD(f, a->radius);
    WRITE_FIELD(f, a->hp);
    WRITE_FIELD(f, a->max_hp);
    WRITE_FIELD(f, a->ore);
    WRITE_FIELD(f, a->max_ore);
    WRITE_FIELD(f, a->commodity);
    WRITE_FIELD(f, a->rotation);
    WRITE_FIELD(f, a->spin);
    WRITE_FIELD(f, a->seed);
    WRITE_FIELD(f, a->age);
    return true;
}

static bool read_asteroid(FILE *f, asteroid_t *a) {
    READ_FIELD(f, a->active);
    READ_FIELD(f, a->fracture_child);
    READ_FIELD(f, a->tier);
    READ_FIELD(f, a->pos);
    READ_FIELD(f, a->vel);
    READ_FIELD(f, a->radius);
    READ_FIELD(f, a->hp);
    READ_FIELD(f, a->max_hp);
    READ_FIELD(f, a->ore);
    READ_FIELD(f, a->max_ore);
    READ_FIELD(f, a->commodity);
    READ_FIELD(f, a->rotation);
    READ_FIELD(f, a->spin);
    READ_FIELD(f, a->seed);
    READ_FIELD(f, a->age);
    return true;
}

/* ---- npc_ship field-by-field I/O ---- */
static bool write_npc(FILE *f, const npc_ship_t *n) {
    WRITE_FIELD(f, n->active);
    WRITE_FIELD(f, n->role);
    WRITE_FIELD(f, n->hull_class);
    WRITE_FIELD(f, n->state);
    WRITE_FIELD(f, n->pos);
    WRITE_FIELD(f, n->vel);
    WRITE_FIELD(f, n->angle);
    WRITE_FIELD(f, n->cargo);
    WRITE_FIELD(f, n->ingots);
    WRITE_FIELD(f, n->target_asteroid);
    WRITE_FIELD(f, n->home_station);
    WRITE_FIELD(f, n->dest_station);
    WRITE_FIELD(f, n->state_timer);
    WRITE_FIELD(f, n->thrusting);
    WRITE_FIELD(f, n->tint_r);
    WRITE_FIELD(f, n->tint_g);
    WRITE_FIELD(f, n->tint_b);
    return true;
}

static bool read_npc(FILE *f, npc_ship_t *n) {
    READ_FIELD(f, n->active);
    READ_FIELD(f, n->role);
    READ_FIELD(f, n->hull_class);
    READ_FIELD(f, n->state);
    READ_FIELD(f, n->pos);
    READ_FIELD(f, n->vel);
    READ_FIELD(f, n->angle);
    READ_FIELD(f, n->cargo);
    READ_FIELD(f, n->ingots);
    READ_FIELD(f, n->target_asteroid);
    READ_FIELD(f, n->home_station);
    READ_FIELD(f, n->dest_station);
    READ_FIELD(f, n->state_timer);
    READ_FIELD(f, n->thrusting);
    READ_FIELD(f, n->tint_r);
    READ_FIELD(f, n->tint_g);
    READ_FIELD(f, n->tint_b);
    return true;
}

/* ---- contract field-by-field I/O ---- */
static bool write_contract(FILE *f, const contract_t *c) {
    WRITE_FIELD(f, c->active);
    WRITE_FIELD(f, c->action);
    WRITE_FIELD(f, c->station_index);
    WRITE_FIELD(f, c->commodity);
    WRITE_FIELD(f, c->quantity_needed);
    WRITE_FIELD(f, c->base_price);
    WRITE_FIELD(f, c->age);
    WRITE_FIELD(f, c->target_pos);
    WRITE_FIELD(f, c->target_index);
    WRITE_FIELD(f, c->claimed_by);
    return true;
}

static bool read_contract(FILE *f, contract_t *c) {
    READ_FIELD(f, c->active);
    READ_FIELD(f, c->action);
    READ_FIELD(f, c->station_index);
    READ_FIELD(f, c->commodity);
    READ_FIELD(f, c->quantity_needed);
    READ_FIELD(f, c->base_price);
    READ_FIELD(f, c->age);
    READ_FIELD(f, c->target_pos);
    READ_FIELD(f, c->target_index);
    READ_FIELD(f, c->claimed_by);
    return true;
}

bool world_save(const world_t *w, const char *path) {
    /* Write to a temp file first, then rename atomically to avoid
     * truncated saves if the process is interrupted mid-write. */
    char tmp_path[272];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;

    /* Header */
    uint32_t magic = SAVE_MAGIC;
    uint32_t version = SAVE_VERSION;
    WRITE_FIELD(f, magic);
    WRITE_FIELD(f, version);
    WRITE_FIELD(f, w->rng);
    WRITE_FIELD(f, w->time);
    WRITE_FIELD(f, w->field_spawn_timer);

    /* Stations */
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!write_station(f, &w->stations[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* Asteroids */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!write_asteroid(f, &w->asteroids[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* NPC ships */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!write_npc(f, &w->npc_ships[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!write_contract(f, &w->contracts[i])) { fclose(f); remove(tmp_path); return false; }
    }

    fclose(f);
    /* Atomic rename — on POSIX this is atomic; on Windows it overwrites. */
    remove(path);
    if (rename(tmp_path, path) != 0) { remove(tmp_path); return false; }
    return true;
}

bool world_load(world_t *w, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint32_t magic, version;
    READ_FIELD(f, magic);
    READ_FIELD(f, version);
    if (magic != SAVE_MAGIC || version < SAVE_VERSION || version > SAVE_VERSION) {
        printf("[save] rejected save: magic=0x%08x version=%u (need %d)\n", magic, version, SAVE_VERSION);
        fclose(f); return false;
    }

    READ_FIELD(f, w->rng);
    READ_FIELD(f, w->time);
    READ_FIELD(f, w->field_spawn_timer);

    /* Stations */
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!read_station(f, &w->stations[i])) return false;
    }
    /* Asteroids */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!read_asteroid(f, &w->asteroids[i])) return false;
    }
    /* NPC ships */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!read_npc(f, &w->npc_ships[i])) return false;
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!read_contract(f, &w->contracts[i])) return false;
    }

    /* Post-load migration: ensure built-in stations have blueprint service.
     * Saves created before the outpost feature lack this bit. */
    for (int i = 0; i < 3 && i < MAX_STATIONS; i++) {
        if (station_is_active(&w->stations[i]))
            w->stations[i].services |= STATION_SERVICE_BLUEPRINT;
    }

    /* Clear transient state */
    w->events.count = 0;
    w->player_only_mode = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        memset(&w->players[i], 0, sizeof(w->players[i]));
    }

    fclose(f);
    belt_field_init(&w->belt, w->rng, WORLD_RADIUS);
    rebuild_signal_chain(w);
    return true;
}

/* ================================================================== */
/* Player persistence                                                  */
/* ================================================================== */

#define PLAYER_MAGIC 0x504C5952u  /* "PLYR" */

typedef struct {
    uint32_t magic;
    ship_t ship;
    int last_station;
    vec2 last_pos;
    float last_angle;
} player_save_data_t;

bool player_save(const server_player_t *sp, const char *dir, int slot) {
    char path[256];
    snprintf(path, sizeof(path), "%s/player_%d.sav", dir, slot);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    player_save_data_t data = {
        .magic = PLAYER_MAGIC,
        .ship = sp->ship,
        .last_station = sp->current_station,
        .last_pos = sp->ship.pos,
        .last_angle = sp->ship.angle,
    };
    bool ok = fwrite(&data, sizeof(data), 1, f) == 1;
    fclose(f);
    if (ok) SIM_LOG("[sim] saved player %d\n", slot);
    return ok;
}

bool player_load(server_player_t *sp, world_t *w, const char *dir, int slot) {
    char path[256];
    snprintf(path, sizeof(path), "%s/player_%d.sav", dir, slot);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    player_save_data_t data;
    if (fread(&data, sizeof(data), 1, f) != 1) { fclose(f); return false; }
    fclose(f);
    if (data.magic != PLAYER_MAGIC) return false;
    sp->ship = data.ship;
    /* Validate hull class */
    if (sp->ship.hull_class < 0 || sp->ship.hull_class >= HULL_CLASS_COUNT)
        sp->ship.hull_class = HULL_CLASS_MINER;
    /* Validate station index */
    sp->current_station = data.last_station;
    if (sp->current_station < 0 || sp->current_station >= MAX_STATIONS ||
        !station_exists(&w->stations[sp->current_station]))
        sp->current_station = 0;
    /* Clamp upgrade levels */
    if (sp->ship.mining_level < 0 || sp->ship.mining_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship.mining_level = 0;
    if (sp->ship.hold_level < 0 || sp->ship.hold_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship.hold_level = 0;
    if (sp->ship.tractor_level < 0 || sp->ship.tractor_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship.tractor_level = 0;
    /* Clamp credits (no negative, no NaN) */
    if (!(sp->ship.credits >= 0.0f)) sp->ship.credits = 0.0f;
    /* Clamp hull HP */
    float max_hull = ship_max_hull(&sp->ship);
    if (!(sp->ship.hull > 0.0f)) sp->ship.hull = max_hull;
    if (sp->ship.hull > max_hull) sp->ship.hull = max_hull;
    /* Clamp cargo (no negative, no NaN, no exceeding capacity) */
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        if (!(sp->ship.cargo[i] >= 0.0f)) sp->ship.cargo[i] = 0.0f;
    }
    sp->ship.pos = data.last_pos;
    sp->ship.angle = data.last_angle;
    /* Dock the player at their last station for safety */
    sp->docked = true;
    sp->nearby_station = sp->current_station;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, w);
    SIM_LOG("[sim] loaded player %d (%.0f credits, station %d)\n",
            slot, sp->ship.credits, sp->current_station);
    return true;
}
