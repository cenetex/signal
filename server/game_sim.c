/*
 * game_sim.c -- Game simulation for Signal Space Miner.
 * Used by both the authoritative server and the client (local sim).
 * All rendering, audio, and sokol references are excluded.
 * Global state replaced with world_t *w and server_player_t *sp parameters.
 */
#include "game_sim.h"
#include "signal_model.h"
#include "rng.h"
#include <stdlib.h>

#define MODULE_COLLISION_RADIUS 34.0f  /* matches 1.4x scaled module half-size */
#define CORRIDOR_HW 10.0f             /* must match visual hw in draw_corridor_arc */

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
/* RNG -- thin wrappers over shared rng.h (pass &w->rng)             */
/* ================================================================== */

static float w_randf(world_t *w)                          { return randf(&w->rng); }
static float w_rand_range(world_t *w, float lo, float hi) { return rand_range(&w->rng, lo, hi); }
static int   w_rand_int(world_t *w, int lo, int hi)       { return rand_int(&w->rng, lo, hi); }

/* ================================================================== */
/* Spatial grid helpers                                                */
/* ================================================================== */

static void spatial_grid_clear(spatial_grid_t *g) {
    memset(g->cells, 0, sizeof(g->cells));
}

static void spatial_grid_cell(const spatial_grid_t *g, vec2 pos, int *cx, int *cy) {
    *cx = (int)((pos.x + g->offset_x) / SPATIAL_CELL_SIZE);
    *cy = (int)((pos.y + g->offset_y) / SPATIAL_CELL_SIZE);
    if (*cx < 0) *cx = 0;
    if (*cy < 0) *cy = 0;
    if (*cx >= SPATIAL_GRID_DIM) *cx = SPATIAL_GRID_DIM - 1;
    if (*cy >= SPATIAL_GRID_DIM) *cy = SPATIAL_GRID_DIM - 1;
}

static void spatial_grid_insert(spatial_grid_t *g, int idx, vec2 pos) {
    int cx, cy;
    spatial_grid_cell(g, pos, &cx, &cy);
    spatial_cell_t *cell = &g->cells[cy][cx];
    if (cell->count < SPATIAL_MAX_PER_CELL) {
        cell->indices[cell->count++] = (int16_t)idx;
    }
}

static void spatial_grid_build(world_t *w) {
    spatial_grid_t *g = &w->asteroid_grid;
    g->offset_x = (SPATIAL_GRID_DIM * SPATIAL_CELL_SIZE) * 0.5f;
    g->offset_y = (SPATIAL_GRID_DIM * SPATIAL_CELL_SIZE) * 0.5f;
    spatial_grid_clear(g);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) continue;
        spatial_grid_insert(g, i, w->asteroids[i].pos);
    }
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
        if (v2_dist_sq(pos, w->stations[s].pos) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) return false;
    }
    /* Must have a free station slot */
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) return true;
    }
    return false;
}

static void add_module_at(station_t *st, module_type_t type, uint8_t arm, uint8_t chain_pos) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    station_module_t *m = &st->modules[st->module_count++];
    m->type = type;
    m->ring = arm;
    m->slot = chain_pos;
    m->scaffold = false;
    m->build_progress = 1.0f;
}

static int spawn_npc(world_t *w, int station_idx, npc_role_t role);

static void activate_outpost(world_t *w, int station_idx) {
    station_t *st = &w->stations[station_idx];
    st->scaffold = false;
    st->scaffold_progress = 1.0f;
    st->signal_range = OUTPOST_SIGNAL_RANGE;
    /* First module: signal relay on ring 1, orbiting the placement point */
    add_module_at(st, MODULE_SIGNAL_RELAY, 1, 0);
    st->arm_count = 1;
    st->arm_speed[0] = STATION_RING_SPEED;
    rebuild_station_services(st);
    rebuild_signal_chain(w);
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
void begin_module_construction_at(world_t *w, station_t *st, int station_idx, module_type_t type, int arm, int chain_pos) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;

    station_module_t *m = &st->modules[st->module_count++];
    m->type = type;
    m->ring = (uint8_t)arm;
    m->slot = (uint8_t)chain_pos;
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
                .base_price = st->base_price[material] * 1.15f, .age = 0.0f,
                .target_index = -1, .claimed_by = -1,
            };
            break;
        }
    }
    SIM_LOG("[sim] began construction of module %d at station %d ring %d slot %d\n",
            type, station_idx, arm, chain_pos);
}

void begin_module_construction(world_t *w, station_t *st, int station_idx, module_type_t type) {
    if (st->module_count >= MAX_MODULES_PER_STATION) return;
    int target_ring = 1;
    for (int r = MAX_RING_COUNT - 1; r >= 1; r--) {
        if (station_has_ring(st, r)) { target_ring = r; break; }
    }
    int target_slot = station_ring_free_slot(st, target_ring, RING_PORT_COUNT[target_ring]);
    if (target_slot < 0) target_slot = 0xFF; /* ring module or full */
    begin_module_construction_at(w, st, station_idx, type, target_ring, target_slot);
}

/* Deliver materials directly to scaffold modules. Materials are consumed
 * immediately from cargo but build progress advances at a fixed rate —
 * delivery fills the module's internal hopper (tracked via build_progress
 * vs the total cost), construction ticks over time in step_module_construction. */
void step_module_delivery(world_t *w, station_t *st, int station_idx, ship_t *ship) {
    (void)w; (void)station_idx;
    for (int i = 0; i < st->module_count; i++) {
        if (!st->modules[i].scaffold) continue;
        commodity_t mat = module_build_material(st->modules[i].type);
        if (ship->cargo[mat] < 0.01f) continue;
        float cost = module_build_cost(st->modules[i].type);
        float needed = cost - st->modules[i].build_progress * cost;
        if (needed < 0.01f) continue;
        float deliver = fminf(ship->cargo[mat], needed);
        ship->cargo[mat] -= deliver;
        /* Store delivered amount as fractional progress toward 1.0.
         * build_progress tracks total delivered / cost. Construction
         * activation is gated by build_timer in step_module_construction. */
        st->modules[i].build_progress += deliver / cost;
        if (st->modules[i].build_progress > 1.0f)
            st->modules[i].build_progress = 1.0f;
    }
}

/* Activate scaffold modules once fully supplied and enough time has elapsed.
 * Construction takes MODULE_BUILD_TIME seconds after materials are fully delivered. */
static const float MODULE_BUILD_TIME = 10.0f;  /* seconds after full delivery */

static void step_module_construction(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        for (int i = 0; i < st->module_count; i++) {
            if (!st->modules[i].scaffold) continue;
            if (st->modules[i].build_progress < 1.0f) continue; /* not fully supplied */
            /* Count down build time using build_progress > 1.0 as timer.
             * 1.0 = just finished delivery, 2.0 = construction complete. */
            st->modules[i].build_progress += dt / MODULE_BUILD_TIME;
            if (st->modules[i].build_progress >= 2.0f) {
                st->modules[i].scaffold = false;
                st->modules[i].build_progress = 1.0f;
                rebuild_station_services(st);
                rebuild_signal_chain(w);
                /* New ring: set shared rotation speed + random offset */
                if (st->modules[i].type == MODULE_RING) {
                    int r = st->modules[i].ring;
                    if (r >= 1 && r <= STATION_NUM_RINGS && r - 1 < MAX_ARMS) {
                        st->arm_count = r;
                        st->arm_speed[0] = STATION_RING_SPEED;
                        /* Deterministic offset from station position + ring index */
                        uint32_t h = (uint32_t)(st->pos.x * 1000.0f) ^ ((uint32_t)(st->pos.y * 1000.0f) << 16) ^ (uint32_t)r;
                        h = h * 2654435761u;
                        st->ring_offset[r - 1] = TWO_PI_F * (float)(h & 0xFFFFu) / 65536.0f;
                    }
                }
                if (st->modules[i].type == MODULE_FURNACE || st->modules[i].type == MODULE_FURNACE_CU || st->modules[i].type == MODULE_FURNACE_CR)
                    spawn_npc(w, s, NPC_ROLE_MINER);
                if (st->modules[i].type == MODULE_FRAME_PRESS || st->modules[i].type == MODULE_LASER_FAB || st->modules[i].type == MODULE_TRACTOR_FAB)
                    spawn_npc(w, s, NPC_ROLE_HAULER);
                SIM_LOG("[sim] module %d activated at station %d\n", st->modules[i].type, s);
            }
        }
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
            /* distance/direction */
            "Far", "Deep", "Outer", "Edge", "Inner", "High", "Low",
            "Near", "Mid", "Upper", "Lower", "North", "South",
            /* void/emptiness */
            "Void", "Drift", "Pale", "Dim", "Faint", "Thin", "Hollow",
            "Blank", "Null", "Silent", "Still", "Quiet", "Hush",
            /* material/industrial */
            "Iron", "Rust", "Ash", "Slag", "Ore", "Copper", "Tin",
            "Lead", "Salt", "Flint", "Basalt", "Granite", "Cobalt",
            "Carbon", "Nickel", "Sulfur", "Zinc",
            /* temperature/light */
            "Cold", "Dark", "Red", "Black", "Grey", "White", "Burnt",
            "Ember", "Cinder", "Frost", "Char",
            /* mood/frontier */
            "Grim", "Last", "Lost", "Worn", "Lone", "Stark", "Bleak",
            "Gaunt", "Bare", "Stern", "Hard", "Grit", "Dusk", "Dawn",
            "Wane", "Rift", "Brink", "Fringe", "Verge", "Scarp",
            /* celestial */
            "Sol", "Arc", "Zenith", "Nadir", "Apex", "Nova", "Vega",
            "Polar", "Umbra", "Halo", "Corona", "Nebula",
            /* structural */
            "Bolt", "Rivet", "Weld", "Truss", "Strut", "Keel",
            "Anvil", "Hammer", "Crucible",
        };
        static const char *suffixes[] = {
            /* position/geography */
            "Reach", "Point", "Gate", "Rock", "Ridge", "Ledge",
            "Spur", "Pike", "Notch", "Gap", "Pass", "Shelf",
            "Rim", "Crest", "Bluff", "Mesa", "Knoll", "Butte",
            /* structure/settlement */
            "Anchor", "Post", "Haven", "Hold", "Watch", "Keep",
            "Fort", "Camp", "Rest", "Berth", "Dock", "Pier",
            "Mooring", "Station", "Depot", "Outpost",
            /* industrial */
            "Forge", "Yard", "Works", "Mill", "Foundry", "Smelter",
            "Refinery", "Quarry", "Pit", "Mine", "Shaft", "Kiln",
            "Furnace", "Press", "Crucible",
            /* light/signal */
            "Light", "Mark", "Beacon", "Signal", "Relay", "Spark",
            "Flare", "Pulse", "Lantern", "Lamp",
            /* water/well (frontier) */
            "Well", "Spring", "Basin", "Cistern", "Trough",
            /* path/boundary */
            "Cairn", "Marker", "Waypoint", "Crossing", "Threshold",
            "Border", "Margin", "Line", "Terminus",
            /* shelter */
            "Hollow", "Shelter", "Cove", "Nook", "Pocket", "Nest",
        };
        enum { NUM_PREFIXES = sizeof(prefixes) / sizeof(prefixes[0]) };
        enum { NUM_SUFFIXES = sizeof(suffixes) / sizeof(suffixes[0]) };
        uint32_t h = (uint32_t)(pos.x * 7.13f) ^ (uint32_t)(pos.y * 13.37f) ^ (uint32_t)slot;
        h ^= h >> 16; h *= 0x45d9f3bu; h ^= h >> 16;
        int pi = (int)(h % NUM_PREFIXES);
        int si = (int)((h >> 8) % NUM_SUFFIXES);
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
                .base_price = 23.0f, .age = 0.0f,
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

/* ================================================================== */
/* Asteroid lifecycle                                                 */
/* ================================================================== */

static void sim_configure_asteroid(world_t *w, asteroid_t *a, asteroid_tier_t tier, commodity_t commodity) {
    float sl = asteroid_spin_limit(tier);
    a->active    = true;
    a->tier      = tier;
    a->commodity = commodity;
    a->radius    = w_rand_range(w, asteroid_radius_min(tier), asteroid_radius_max(tier));
    a->max_hp    = w_rand_range(w, asteroid_hp_min(tier), asteroid_hp_max(tier));
    a->hp        = a->max_hp;
    a->max_ore   = 0.0f;
    a->ore       = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        a->max_ore = w_rand_range(w, 8.0f, 14.0f);
        a->ore     = a->max_ore;
    }
    a->rotation = w_rand_range(w, 0.0f, TWO_PI_F);
    a->spin     = w_rand_range(w, -sl, sl);
    a->seed     = w_rand_range(w, 0.0f, 100.0f);
    a->age      = 0.0f;
    a->net_dirty = true;
}

static asteroid_tier_t random_field_asteroid_tier(world_t *w) {
    float roll = w_randf(w);
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
    return active[w_rand_int(w, 0, count - 1)];
}

/* Find a good clump center in the belt density field near signal-covered space.
 * Uses gradient walk: sample random points, then step toward higher density. */
static vec2 find_belt_clump_center(world_t *w, float *out_density) {
    vec2 best_pos = v2(0.0f, 0.0f);
    float best_density = 0.0f;
    for (int attempt = 0; attempt < 16; attempt++) {
        int stn = pick_active_station(w);
        float angle = w_rand_range(w, 0.0f, TWO_PI_F);
        float distance = w_rand_range(w, 200.0f, w->stations[stn].signal_range * 0.85f);
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
    float stretch_angle = w_rand_range(w, 0.0f, TWO_PI_F);
    float stretch_factor = w_rand_range(w, 1.0f, 2.5f);
    float cos_s = cosf(stretch_angle);
    float sin_s = sinf(stretch_angle);

    /* Shared drift velocity for the clump */
    vec2 drift = v2(w_rand_range(w, -3.0f, 3.0f), w_rand_range(w, -3.0f, 3.0f));

    int placed = 0;
    for (int i = 0; i < clump_size && (first_slot + placed) < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[first_slot + placed];
        if (a->active) continue;

        /* Pick tier: first rock is the anchor, rest are smaller */
        asteroid_tier_t tier;
        if (i == 0) {
            tier = (w_randf(w) < 0.15f) ? ASTEROID_TIER_XXL : ASTEROID_TIER_XL;
        } else if (i <= 3) {
            tier = (w_randf(w) < 0.4f) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
        } else {
            tier = (w_randf(w) < 0.3f) ? ASTEROID_TIER_L : ASTEROID_TIER_M;
        }

        clear_asteroid(a);
        sim_configure_asteroid(w, a, tier, ore);
        a->fracture_child = false;

        /* Scatter around center with elongation */
        float r = w_rand_range(w, 0.0f, clump_radius) * sqrtf(w_randf(w)); /* sqrt for uniform disk */
        float theta = w_rand_range(w, 0.0f, TWO_PI_F);
        float lx = cosf(theta) * r;
        float ly = sinf(theta) * r;
        /* Apply stretch */
        float sx = lx * cos_s - ly * sin_s;
        float sy = lx * sin_s + ly * cos_s;
        sx *= stretch_factor;
        float fx = sx * cos_s + sy * sin_s;
        float fy = -sx * sin_s + sy * cos_s;

        a->pos = v2_add(center, v2(fx, fy));
        a->vel = v2_add(drift, v2(w_rand_range(w, -2.0f, 2.0f), w_rand_range(w, -2.0f, 2.0f)));
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
    a->vel = v2(w_rand_range(w, -4.0f, 4.0f), w_rand_range(w, -4.0f, 4.0f));
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
    a->vel = v2_add(v2_scale(inward, w_rand_range(w, speed_lo, speed_hi)),
                    v2_scale(tangent, w_rand_range(w, -tangent_jitter, tangent_jitter)));
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
        float angle = w_rand_range(w, 0.0f, TWO_PI_F);
        vec2 outward = v2_from_angle(angle);
        float dist = w_rand_range(w, sr * 0.30f, sr * 0.60f);
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
    case ASTEROID_TIER_XXL: return w_rand_int(w, 8, 14);
    case ASTEROID_TIER_XL: return w_rand_int(w, 2, 3);
    case ASTEROID_TIER_L:  return w_rand_int(w, 2, 3);
    case ASTEROID_TIER_M:  return w_rand_int(w, 2, 4);
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

static void fracture_asteroid(world_t *w, int idx, vec2 outward_dir, int8_t fractured_by) {
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
        float child_angle = base_angle + (spread_t * 1.35f) + w_rand_range(w, -0.14f, 0.14f);
        vec2 dir = v2_from_angle(child_angle);
        vec2 tangent = v2_perp(dir);
        asteroid_t *child = &w->asteroids[child_slots[i]];
        spawn_child_asteroid(w, child, child_tier, parent.commodity, parent.pos, parent.vel);
        vec2 cpos = v2_add(parent.pos, v2_scale(dir, (parent.radius * 0.28f) + (child->radius * 0.85f)));
        float drift = w_rand_range(w, 22.0f, 56.0f);
        vec2 cvel = v2_add(parent.vel, v2_add(v2_scale(dir, drift), v2_scale(tangent, w_rand_range(w, -10.0f, 10.0f))));
        child->pos = cpos;
        child->vel = cvel;
        child->last_fractured_by = fractured_by;
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

        /* Station vortex: asteroids near stations get caught in orbit.
         * Large asteroids orbit outside the perimeter.
         * S fragments spiral inward toward hoppers. */
        for (int s = 0; s < MAX_STATIONS; s++) {
            const station_t *st = &w->stations[s];
            if (!station_exists(st)) continue;
            float d_sq = v2_dist_sq(a->pos, st->pos);
            float vortex_range = st->dock_radius * 2.0f;
            if (d_sq > vortex_range * vortex_range || d_sq < 1.0f) continue;
            float d = sqrtf(d_sq);
            vec2 radial = v2_scale(v2_sub(a->pos, st->pos), 1.0f / d);
            vec2 tangent = v2(-radial.y, radial.x);
            if (a->tier == ASTEROID_TIER_S) {
                /* Fragments: strong spiral inward toward center/hoppers */
                a->vel = v2_add(a->vel, v2_scale(tangent, 12.0f * dt));
                a->vel = v2_sub(a->vel, v2_scale(radial, 6.0f * dt));
            } else {
                /* Large asteroids: gentle orbit outside perimeter */
                a->vel = v2_add(a->vel, v2_scale(tangent, 4.0f * dt));
                /* Push outward if inside dock radius (keep clear of station) */
                if (d < st->dock_radius)
                    a->vel = v2_add(a->vel, v2_scale(radial, 15.0f * dt));
            }
            break;
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
        int wave = 2 + w_rand_int(w, 0, 2);
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

/* What ore type a furnace module smelts. Returns -1 for non-furnaces. */
static commodity_t furnace_ore_type(module_type_t mt) {
    switch (mt) {
        case MODULE_FURNACE:    return COMMODITY_FERRITE_ORE;
        case MODULE_FURNACE_CU: return COMMODITY_CUPRITE_ORE;
        case MODULE_FURNACE_CR: return COMMODITY_CRYSTAL_ORE;
        default: return (commodity_t)-1;
    }
}

/* Check if a furnace at (ring, slot) has an adjacent hopper (slot ±1, wrapping). */
static bool furnace_has_adjacent_hopper(const station_t *st, int ring, int slot) {
    int slots = STATION_RING_SLOTS[ring];
    if (slots <= 0) return false;
    int prev_slot = (slot - 1 + slots) % slots;
    int next_slot = (slot + 1) % slots;
    for (int m = 0; m < st->module_count; m++) {
        if (st->modules[m].type != MODULE_ORE_BUYER) continue;
        if (st->modules[m].scaffold) continue;
        if (st->modules[m].ring != ring) continue;
        if (st->modules[m].slot == prev_slot || st->modules[m].slot == next_slot)
            return true;
    }
    return false;
}

/* Per-furnace smelting with hopper adjacency.
 * Each furnace only smelts if an adjacent hopper exists on the same ring.
 * Smelting rate is split across active furnaces to avoid instant consumption. */
static void sim_step_refinery_production(world_t *w, float dt) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];

        /* Count active furnaces (those with adjacent hoppers and ore to smelt) */
        int active = 0;
        for (int m = 0; m < st->module_count; m++) {
            module_type_t mt = st->modules[m].type;
            if (mt != MODULE_FURNACE && mt != MODULE_FURNACE_CU && mt != MODULE_FURNACE_CR) continue;
            if (st->modules[m].scaffold) continue;
            if (!furnace_has_adjacent_hopper(st, st->modules[m].ring, st->modules[m].slot)) continue;
            commodity_t ore = furnace_ore_type(mt);
            if (ore < 0 || st->inventory[ore] <= 0.01f) continue;
            active++;
        }
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;
        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        /* Smelt per furnace */
        for (int m = 0; m < st->module_count; m++) {
            module_type_t mt = st->modules[m].type;
            if (mt != MODULE_FURNACE && mt != MODULE_FURNACE_CU && mt != MODULE_FURNACE_CR) continue;
            if (st->modules[m].scaffold) continue;
            if (!furnace_has_adjacent_hopper(st, st->modules[m].ring, st->modules[m].slot)) continue;
            commodity_t ore = furnace_ore_type(mt);
            if (ore < 0 || st->inventory[ore] <= 0.01f) continue;
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
    for (int i = 0; i < COMMODITY_COUNT; i++) t += npc->cargo[i];
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
        if (signal_npc_confidence(signal_strength_at(w, a->pos)) < 0.1f) continue;
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

/* NPC approach: aim for the station core center. */
static vec2 station_approach_target(const station_t *st, vec2 from) {
    (void)from;
    return st->pos;
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
    /* Signal-based boundary: NPCs pushed back when confidence is low */
    float sig = signal_strength_at(w, npc->pos);
    float npc_conf = signal_npc_confidence(sig);
    if (npc_conf < 1.0f) {
        float best_d_sq = 1e18f;
        int best_s = 0;
        for (int i = 0; i < MAX_STATIONS; i++) {
            float d_sq = v2_dist_sq(npc->pos, w->stations[i].pos);
            if (d_sq < best_d_sq) { best_d_sq = d_sq; best_s = i; }
        }
        vec2 to_station = v2_sub(w->stations[best_s].pos, npc->pos);
        float d = sqrtf(v2_len_sq(to_station));
        if (d > 0.001f) {
            float edge = w->stations[best_s].signal_range;
            float overshoot = fmaxf(0.0f, d - edge);
            float push_strength = overshoot * 0.08f + (1.0f - npc_conf) * 0.05f;
            vec2 push = v2_scale(to_station, push_strength / d);
            npc->vel = v2_add(npc->vel, push);
        }
    }
}


static void npc_resolve_station_collisions(world_t *w, npc_ship_t *npc) {
    const hull_def_t *hull = npc_hull_def(npc);
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        /* Core collision */
        float minimum = st->radius + 4.0f + hull->ship_radius;
        vec2 delta = v2_sub(npc->pos, st->pos);
        float d_sq = v2_len_sq(delta);
        if (d_sq < minimum * minimum) {
            float d = sqrtf(d_sq);
            vec2 normal = d > 0.00001f ? v2_scale(delta, 1.0f / d) : v2(1.0f, 0.0f);
            npc->pos = v2_add(st->pos, v2_scale(normal, minimum));
            float vel_toward = v2_dot(npc->vel, normal);
            if (vel_toward < 0.0f)
                npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.0f));
        }
        /* Per-module collision for NPCs */
        for (int m = 0; m < st->module_count; m++) {
            int ring = st->modules[m].ring;
            if (ring < 1 || ring > STATION_NUM_RINGS) continue;
            vec2 mod_pos = module_world_pos_ring(st, ring, st->modules[m].slot);
            float mod_min = MODULE_COLLISION_RADIUS + hull->ship_radius;
            vec2 md = v2_sub(npc->pos, mod_pos);
            float md_sq = v2_len_sq(md);
            if (md_sq >= mod_min * mod_min) continue;
            float mdd = sqrtf(md_sq);
            vec2 mn = mdd > 0.001f ? v2_scale(md, 1.0f / mdd) : v2(1.0f, 0.0f);
            npc->pos = v2_add(mod_pos, v2_scale(mn, mod_min));
            float mvt = v2_dot(npc->vel, mn);
            if (mvt < 0.0f)
                npc->vel = v2_sub(npc->vel, v2_scale(mn, mvt * 1.0f));
        }
        /* Corridor annular sectors for NPCs */
        for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
            int slots = STATION_RING_SLOTS[ring];
            int cidx[MAX_MODULES_PER_STATION];
            int ccount = 0;
            for (int m = 0; m < st->module_count; m++)
                if (st->modules[m].ring == ring) cidx[ccount++] = m;
            if (ccount < 2) continue;
            for (int ci = 1; ci < ccount; ci++) {
                int tmp = cidx[ci]; int cj = ci - 1;
                while (cj >= 0 && st->modules[cidx[cj]].slot > st->modules[tmp].slot)
                    { cidx[cj+1] = cidx[cj]; cj--; }
                cidx[cj+1] = tmp;
            }
            for (int ci = 0; ci + 1 < ccount; ci++) {
                if (st->modules[cidx[ci+1]].slot - st->modules[cidx[ci]].slot != 1) continue;
                /* Docks create gaps — no corridor collision on either side */
                if (st->modules[cidx[ci]].type == MODULE_DOCK) continue;
                if (st->modules[cidx[ci+1]].type == MODULE_DOCK) continue;
                float ca = module_angle_ring(st, ring, st->modules[cidx[ci]].slot);
                float cb = module_angle_ring(st, ring, st->modules[cidx[ci+1]].slot);
                /* Annular sector test for NPC */
                vec2 nd = v2_sub(npc->pos, st->pos);
                float ndist = sqrtf(v2_len_sq(nd));
                if (ndist < 1.0f) continue;
                float nr_inner = STATION_RING_RADIUS[ring] - CORRIDOR_HW - hull->ship_radius;
                float nr_outer = STATION_RING_RADIUS[ring] + CORRIDOR_HW + hull->ship_radius;
                if (ndist <= nr_inner || ndist >= nr_outer) continue;
                float nang = atan2f(nd.y, nd.x);
                float nda = cb - ca;
                while (nda > PI_F) nda -= TWO_PI_F;
                while (nda < -PI_F) nda += TWO_PI_F;
                if (angle_in_arc(nang, ca, nda) < 0.0f) continue;
                /* Push radially */
                vec2 nrad = v2_scale(nd, 1.0f / ndist);
                float d_in = ndist - (STATION_RING_RADIUS[ring] - CORRIDOR_HW);
                float d_out = (STATION_RING_RADIUS[ring] + CORRIDOR_HW) - ndist;
                if (d_in < d_out) {
                    npc->pos = v2_add(st->pos, v2_scale(nrad, nr_inner));
                    float vt = v2_dot(npc->vel, nrad);
                    if (vt > 0.0f) npc->vel = v2_sub(npc->vel, v2_scale(nrad, vt * 1.0f));
                } else {
                    npc->pos = v2_add(st->pos, v2_scale(nrad, nr_outer));
                    float vt = v2_dot(npc->vel, nrad);
                    if (vt < 0.0f) npc->vel = v2_sub(npc->vel, v2_scale(nrad, vt * 1.0f));
                }
            }
            if (ccount == slots && ring >= 1
                && st->modules[cidx[ccount-1]].type != MODULE_DOCK
                && st->modules[cidx[0]].type != MODULE_DOCK) {
                /* Wrap-around corridor — same test (skip if dock at either end) */
                float ca = module_angle_ring(st, ring, st->modules[cidx[ccount-1]].slot);
                float cb = module_angle_ring(st, ring, st->modules[cidx[0]].slot);
                vec2 nd = v2_sub(npc->pos, st->pos);
                float ndist = sqrtf(v2_len_sq(nd));
                if (ndist > 1.0f) {
                    float nr_inner = STATION_RING_RADIUS[ring] - CORRIDOR_HW - hull->ship_radius;
                    float nr_outer = STATION_RING_RADIUS[ring] + CORRIDOR_HW + hull->ship_radius;
                    if (ndist > nr_inner && ndist < nr_outer) {
                        float nang = atan2f(nd.y, nd.x);
                        float nda = cb - ca;
                        while (nda > PI_F) nda -= TWO_PI_F;
                        while (nda < -PI_F) nda += TWO_PI_F;
                        if (angle_in_arc(nang, ca, nda) >= 0.0f) {
                            vec2 nrad = v2_scale(nd, 1.0f / ndist);
                            float d_in = ndist - (STATION_RING_RADIUS[ring] - CORRIDOR_HW);
                            float d_out = (STATION_RING_RADIUS[ring] + CORRIDOR_HW) - ndist;
                            if (d_in < d_out) {
                                npc->pos = v2_add(st->pos, v2_scale(nrad, nr_inner));
                            } else {
                                npc->pos = v2_add(st->pos, v2_scale(nrad, nr_outer));
                            }
                            float vt = v2_dot(npc->vel, nrad);
                            if ((d_in < d_out && vt > 0.0f) || (d_in >= d_out && vt < 0.0f))
                                npc->vel = v2_sub(npc->vel, v2_scale(nrad, vt * 1.0f));
                        }
                    }
                }
            }
        }
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
            npc->vel = v2_sub(npc->vel, v2_scale(normal, vel_toward * 1.0f));
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
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) carried += npc->cargo[c];
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
                    npc->cargo[ingot] += take;
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
                        npc->cargo[ingot] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
                    }
                }
                if (!loaded && station_has_module(dest, MODULE_LASER_FAB)) {
                    commodity_t ingot = COMMODITY_CUPRITE_INGOT;
                    float avail = fmaxf(0.0f, home->inventory[ingot] - HAULER_RESERVE);
                    float take = fminf(avail, space);
                    if (take > 0.5f) {
                        npc->cargo[ingot] += take;
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
                        npc->cargo[ingot] += take;
                        home->inventory[ingot] -= take;
                        loaded = true;
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
        vec2 approach = station_approach_target(dest, npc->pos);
        npc_steer_toward(npc, approach, hull->accel, hull->turn_speed, dt);
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
            for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
                dest->inventory[i] += npc->cargo[i];
                if (dest->inventory[i] > MAX_PRODUCT_STOCK)
                    dest->inventory[i] = MAX_PRODUCT_STOCK;
                npc->cargo[i] = 0.0f;
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
        vec2 approach_home = station_approach_target(home, npc->pos);
        npc_steer_toward(npc, approach_home, hull->accel, hull->turn_speed, dt);
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
                fracture_asteroid(w, npc->target_asteroid, outward, -1); /* NPC: no player credit */
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
            vec2 miner_approach = station_approach_target(home, npc->pos);
            npc_steer_toward(npc, miner_approach, hull->accel, hull->turn_speed, dt);
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
                    .base_price = 20.0f, .age = 0.0f,
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
    s->stat_credits_spent += amount;
    return true;
}

static void anchor_ship_in_station(server_player_t *sp, world_t *w) {
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
        sp->ship.vel = v2_sub(sp->ship.vel, v2_scale(normal, vel_toward * 1.0f));
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
    float r_inner = ring_r - CORRIDOR_HW - ship_r;
    float r_outer = ring_r + CORRIDOR_HW + ship_r;
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

    /* Ship is inside corridor — push radially to nearest edge */
    vec2 radial = v2_scale(delta, 1.0f / dist);
    float d_inner = dist - (ring_r - CORRIDOR_HW);
    float d_outer = (ring_r + CORRIDOR_HW) - dist;
    vec2 push_normal;
    if (d_inner < d_outer) {
        sp->ship.pos = v2_add(center, v2_scale(radial, ring_r - CORRIDOR_HW - ship_r));
        push_normal = v2_scale(radial, -1.0f);
    } else {
        sp->ship.pos = v2_add(center, v2_scale(radial, ring_r + CORRIDOR_HW + ship_r));
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
static asteroid_tier_t max_mineable_tier(int mining_level) {
    switch (mining_level) {
        case 0: return ASTEROID_TIER_M;
        case 1: return ASTEROID_TIER_L;
        case 2: return ASTEROID_TIER_XL;
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

/* Forward declarations for ledger functions (defined below step_station_interaction_system) */
static void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value);

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
            /* Credit ledger for passive income on ore supply */
            if (sp->session_ready && buy < COMMODITY_RAW_ORE_COUNT)
                ledger_credit_supply(st, sp->session_token, accepted * price);
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
        sp->ship.stat_credits_earned += payout;
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
            float push_strength = boundary * 0.5f;
            vec2 push = v2_scale(to_station, push_strength / d);
            s->vel = v2_add(s->vel, push);
        }
    }
}

/* Resolve ship vs station: core + module circles + corridor annular sectors. */
static void resolve_module_collisions(world_t *w, server_player_t *sp, const station_t *st) {
    float ship_r = ship_hull_def(&sp->ship)->ship_radius;
    /* Station core collision (same as NPCs get) */
    if (st->radius > 0.0f) {
        resolve_ship_circle(w, sp, st->pos, st->radius + 4.0f);
    }
    /* Module circles — dock modules only collide from the inside (inward half)
     * so ships can enter through dock gaps from outside the ring. */
    for (int i = 0; i < st->module_count; i++) {
        int ring = st->modules[i].ring;
        if (ring < 1 || ring > STATION_NUM_RINGS) continue;
        vec2 mod_pos = module_world_pos_ring(st, ring, st->modules[i].slot);
        if (st->modules[i].type == MODULE_DOCK) {
            /* Dock: only collide if ship is inside the ring (closer to center
             * than the module). This lets ships fly in from outside. */
            float ship_dist_sq = v2_dist_sq(sp->ship.pos, st->pos);
            float mod_dist_sq = v2_dist_sq(mod_pos, st->pos);
            if (ship_dist_sq < mod_dist_sq)
                resolve_ship_circle(w, sp, mod_pos, MODULE_COLLISION_RADIUS);
        } else {
            resolve_ship_circle(w, sp, mod_pos, MODULE_COLLISION_RADIUS);
        }
    }

    /* Precompute module angular positions for corridor junction suppression */
    float mod_angles[MAX_MODULES_PER_STATION];
    float mod_angular_size[MAX_MODULES_PER_STATION];
    for (int i = 0; i < st->module_count; i++) {
        int ring = st->modules[i].ring;
        if (ring < 1 || ring > STATION_NUM_RINGS) { mod_angles[i] = 0; mod_angular_size[i] = 0; continue; }
        vec2 mp = module_world_pos_ring(st, ring, st->modules[i].slot);
        vec2 d = v2_sub(mp, st->pos);
        mod_angles[i] = atan2f(d.y, d.x);
        float ring_r = STATION_RING_RADIUS[ring];
        mod_angular_size[i] = (ring_r > 1.0f) ? (MODULE_COLLISION_RADIUS + ship_r) / ring_r : 0.0f;
    }

    /* Corridor annular sectors — same enumeration as rendering.
     * Skip corridors adjacent to dock modules — docks create entry gaps.
     * Also skip corridor test when ship is within a module's angular footprint
     * (module circle takes priority, prevents junction jitter). */
    float ship_dist = sqrtf(v2_dist_sq(sp->ship.pos, st->pos));
    vec2 ship_delta = v2_sub(sp->ship.pos, st->pos);
    float ship_ang = atan2f(ship_delta.y, ship_delta.x);

    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        int slots = STATION_RING_SLOTS[ring];
        int cidx[MAX_MODULES_PER_STATION];
        int count = 0;
        for (int i = 0; i < st->module_count; i++)
            if (st->modules[i].ring == ring) cidx[count++] = i;
        if (count < 2) continue;
        for (int i = 1; i < count; i++) {
            int tmp = cidx[i]; int j = i - 1;
            while (j >= 0 && st->modules[cidx[j]].slot > st->modules[tmp].slot)
                { cidx[j+1] = cidx[j]; j--; }
            cidx[j+1] = tmp;
        }

        /* Check if ship is near any module on this ring (suppress corridor) */
        bool near_module = false;
        float ring_r = STATION_RING_RADIUS[ring];
        if (fabsf(ship_dist - ring_r) < CORRIDOR_HW + ship_r + MODULE_COLLISION_RADIUS) {
            for (int ci = 0; ci < count; ci++) {
                float ang_diff = wrap_angle(ship_ang - mod_angles[cidx[ci]]);
                if (fabsf(ang_diff) < mod_angular_size[cidx[ci]]) {
                    near_module = true;
                    break;
                }
            }
        }

        if (!near_module) {
            for (int i = 0; i + 1 < count; i++) {
                if (st->modules[cidx[i+1]].slot - st->modules[cidx[i]].slot != 1) continue;
                bool has_dock = (st->modules[cidx[i]].type == MODULE_DOCK)
                             || (st->modules[cidx[i+1]].type == MODULE_DOCK);
                if (has_dock && ship_dist >= ring_r) continue; /* dock corridor: inside only */
                float a = module_angle_ring(st, ring, st->modules[cidx[i]].slot);
                float b = module_angle_ring(st, ring, st->modules[cidx[i+1]].slot);
                resolve_ship_annular_sector(w, sp, st->pos, ring_r, a, b);
            }
            if (count == slots) {
                bool wrap_dock = (st->modules[cidx[count-1]].type == MODULE_DOCK)
                              || (st->modules[cidx[0]].type == MODULE_DOCK);
                if (!wrap_dock || ship_dist < ring_r) {
                    float a = module_angle_ring(st, ring, st->modules[cidx[count-1]].slot);
                    float b = module_angle_ring(st, ring, st->modules[cidx[0]].slot);
                    resolve_ship_annular_sector(w, sp, st->pos, ring_r, a, b);
                }
            }
        }
    }
}

static bool is_already_towed(const ship_t *ship, int asteroid_idx);

static void resolve_world_collisions(world_t *w, server_player_t *sp) {
    ship_collision_count = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_collides(&w->stations[i])) continue;
        /* Skip collision with the station we're docking at */
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
#define DOCK_APPROACH_RANGE 300.0f /* range to detect station for docking */

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

    /* Docking approach: directly lerp toward berth (berth may be rotating) */
    if (sp->docking_approach && sp->in_dock_range) {
        const station_t *dock_st = &w->stations[sp->nearby_station];
        int berth = find_best_berth(w, dock_st, sp->nearby_station, sp->ship.pos);
        vec2 target = dock_berth_pos(dock_st, berth);
        float dist = sqrtf(v2_dist_sq(sp->ship.pos, target));

        /* Kill velocity, move directly toward berth */
        sp->ship.vel = v2(0.0f, 0.0f);
        float step = fminf(120.0f * dt, dist); /* max 120 units/sec approach */
        if (dist > 1.0f) {
            vec2 dir = v2_scale(v2_sub(target, sp->ship.pos), step / dist);
            sp->ship.pos = v2_add(sp->ship.pos, dir);
        }

        /* Rotate toward berth angle */
        float desired = dock_berth_angle(dock_st, berth);
        sp->ship.angle = wrap_angle(sp->ship.angle + wrap_angle(desired - sp->ship.angle) * 6.0f * dt);

        /* Snap when close */
        if (dist < 10.0f) {
            sp->dock_berth = berth;
            dock_ship(w, sp);
            sp->docking_approach = false;
        }
    }
}

static void update_targeting_state(world_t *w, server_player_t *sp, vec2 forward) {
    vec2 muzzle = ship_muzzle(sp->ship.pos, sp->ship.angle, &sp->ship);
    /* Prefer client's mining target hint if valid, in range, and in front */
    int hint = sp->input.mining_target_hint;
    if (hint >= 0 && hint < MAX_ASTEROIDS && w->asteroids[hint].active
        && !asteroid_is_collectible(&w->asteroids[hint])) {
        const asteroid_t *a = &w->asteroids[hint];
        if (hinted_target_in_mining_cone(muzzle, forward, a)) {
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
        /* Tractor drag: pull toward a point behind the ship, maintaining
         * safe distance. Fragments feel heavy and tactile, not springy. */
        float tractor_r = ship_tractor_range(&sp->ship);
        float safe_dist = 40.0f + a->radius + ship_hull_def(&sp->ship)->ship_radius;
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float dist_to_ship = v2_len(to_ship);

        /* If too far, pull toward ship (but not closer than safe distance) */
        if (dist_to_ship > tractor_r * 0.7f) {
            /* Strong pull to catch up */
            vec2 pull = v2_scale(to_ship, 4.0f);
            a->vel = v2_add(a->vel, v2_scale(pull, dt));
        } else if (dist_to_ship > safe_dist) {
            /* Gentle pull — keep in tow range */
            vec2 pull = v2_scale(to_ship, 1.5f);
            a->vel = v2_add(a->vel, v2_scale(pull, dt));
        }

        /* Push away if too close (don't slam into ship) */
        if (dist_to_ship < safe_dist && dist_to_ship > 0.1f) {
            vec2 push = v2_scale(to_ship, -(safe_dist - dist_to_ship) * 8.0f);
            a->vel = v2_add(a->vel, v2_scale(push, dt));
        }

        /* Drag — fragments feel heavy, bleed speed over time */
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

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!asteroid_is_collectible(a)) continue;
        if (is_already_towed(&sp->ship, i)) continue;
        vec2 to_ship = v2_sub(sp->ship.pos, a->pos);
        float d_sq = v2_len_sq(to_ship);
        if (d_sq <= nearby_sq) sp->nearby_fragments++;
        int max_tow = 2 + sp->ship.tractor_level * 2; /* 2/4/6/8/10 */
        if (d_sq <= tr_sq) {
            sp->tractor_fragments++;
            /* Only pull fragments toward ship if tow chain has room.
             * Otherwise they drift naturally — no orbiting hazard. */
            if (sp->ship.towed_count < max_tow) {
                float d = sqrtf(d_sq);
                float pull = 1.0f - clampf(d / tr, 0.0f, 1.0f);
                vec2 pull_dir = d > 0.001f ? v2_scale(to_ship, 1.0f / d) : ship_forward(sp->ship.angle);
                a->vel = v2_add(a->vel, v2_scale(pull_dir, FRAGMENT_TRACTOR_ACCEL * lerpf(0.35f, 1.0f, pull) * dt));
                float speed = v2_len(a->vel);
                if (speed > FRAGMENT_MAX_SPEED) a->vel = v2_scale(v2_norm(a->vel), FRAGMENT_MAX_SPEED);
            }
        }
        /* Tow fragment: attach to ship's tow chain (ore stays in fragment) */
        float cr = ship_collect_radius(&sp->ship) + a->radius;
        if (d_sq <= cr * cr && sp->ship.towed_count < max_tow) {
            sp->ship.towed_fragments[sp->ship.towed_count] = (int16_t)i;
            sp->ship.towed_count++;
            a->last_towed_by = (int8_t)sp->id;
            sp->ship.stat_ore_mined += a->ore;
            emit_event(w, (sim_event_t){.type = SIM_EVENT_PICKUP, .player_id = sp->id,
                                        .pickup = {.ore = a->ore, .fragments = 1}});
        }
    }
}

/* Deposit towed fragments: when the SHIP is near an ore buyer module,
 * all towed fragments get consumed (ore → station, credits → player).
 * Fragments don't need to individually reach the hopper — the ship does. */
#define HOPPER_PULL_RANGE 300.0f   /* hopper attracts fragments from this far */
#define HOPPER_PULL_ACCEL 500.0f   /* pull strength — strong enough to overcome drift */
#define HOPPER_CONSUME_RANGE 50.0f /* fragment consumed when this close to hopper */

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
     * Hopper intake (step_hopper_intake) consumes nearby S-tier fragments
     * directly, crediting the towing player. */
}

/* Release all towed fragments (manual dump). */
static void release_towed_fragments(server_player_t *sp) {
    sp->ship.towed_count = 0;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
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
        /* Check individual modules */
        for (int mi = 0; mi < st->module_count; mi++) {
            if (st->modules[mi].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[mi].ring, st->modules[mi].slot);
            vec2 to_mod = v2_sub(mp, muzzle);
            float mproj = v2_dot(to_mod, forward);
            if (mproj > 0.0f && mproj < best_dist) {
                vec2 closest = v2_add(muzzle, v2_scale(forward, mproj));
                float perp = v2_len(v2_sub(closest, mp));
                if (perp < MODULE_COLLISION_RADIUS) {
                    best_dist = mproj;
                    sp->scan_target_type = 1;
                    sp->scan_target_index = si;
                    sp->scan_module_index = mi;
                    sp->beam_end = v2_sub(mp, v2_scale(v2_norm(to_mod), MODULE_COLLISION_RADIUS * 0.9f));
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
    st->ledger[idx].pending_credits = 0.0f;
    st->ledger[idx].lifetime_supply = 0.0f;
    return idx;
}

/* Credit a player's ledger when they supply ore to a station */
static void ledger_credit_supply(station_t *st, const uint8_t *token, float ore_value) {
    int idx = ledger_find_or_create(st, token);
    if (idx < 0) return;
    /* Supplier gets 80% of ore value as passive income */
    float supplier_share = ore_value * 0.80f;
    st->ledger[idx].pending_credits += supplier_share;
    st->ledger[idx].lifetime_supply += ore_value;
}

/* Hail: collect pending credits from a station (requires signal > 90%) */
static void handle_hail(world_t *w, server_player_t *sp) {
    if (sp->docked) return;
    float sig = signal_strength_at(w, sp->ship.pos);
    if (sig < 0.90f) return;

    float total_collected = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (st->signal_range <= 0.0f) continue;
        float d = sqrtf(v2_dist_sq(sp->ship.pos, st->pos));
        if (d > st->signal_range) continue;
        for (int i = 0; i < st->ledger_count; i++) {
            if (memcmp(st->ledger[i].player_token, sp->session_token, 8) != 0) continue;
            if (st->ledger[i].pending_credits > 0.01f) {
                total_collected += st->ledger[i].pending_credits;
                st->ledger[i].pending_credits = 0.0f;
            }
        }
    }
    if (total_collected > 0.01f) {
        sp->ship.credits += total_collected;
        sp->ship.stat_credits_earned += total_collected;
        SIM_LOG("[sim] player %d hail collected %.0f credits\n", sp->id, total_collected);
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
        if (sp->in_dock_range) {
            /* Module-based docking: find nearest dock module berth */
            const station_t *dock_st = &w->stations[sp->nearby_station];
            int berth = find_best_berth(w, dock_st, sp->nearby_station, sp->ship.pos);
            vec2 bp = dock_berth_pos(dock_st, berth);
            float d = sqrtf(v2_dist_sq(sp->ship.pos, bp));
            if (d <= DOCK_SNAP_DISTANCE) {
                sp->dock_berth = berth;
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
    /* Module construction: player requests to build a module */
    if (intent->build_module && !w->player_only_mode) {
        int target_ring = (int)intent->build_ring;
        int target_slot = (int)intent->build_slot;
        if (target_ring < 1) target_ring = 1;
        if (target_ring >= MAX_RING_COUNT) target_ring = MAX_RING_COUNT - 1;

        if (intent->build_module_type == MODULE_RING) {
            /* Building a new ring */
            float cost = module_credit_cost(MODULE_RING);
            if (sp->ship.credits >= cost
                && !station_has_ring(docked_st, target_ring)
                && docked_st->module_count < MAX_MODULES_PER_STATION
                && (target_ring == 1 || ring_has_dock(docked_st, target_ring - 1))) {
                sp->ship.credits -= cost;
                begin_module_construction_at(w, docked_st, sp->current_station, MODULE_RING, target_ring, 0xFF);
            }
        } else {
            /* Building a module at a specific port */
            float cost = module_credit_cost(intent->build_module_type);
            bool slot_free = true;
            for (int m = 0; m < docked_st->module_count; m++) {
                if (docked_st->modules[m].ring == target_ring && docked_st->modules[m].slot == target_slot) {
                    slot_free = false;
                    break;
                }
            }
            /* Ring 1: slot 0 = relay (auto), slot 1 = dock only, slot 2 = player choice */
            bool ring1_ok = true;
            if (target_ring == 1) {
                if (target_slot == 0)
                    ring1_ok = false; /* relay is pre-placed */
                else if (target_slot == 1 && intent->build_module_type != MODULE_DOCK)
                    ring1_ok = false;
            }
            /* Every ring must have at least one dock for a gap.
             * If this would fill the last slot and the ring has no dock,
             * only allow it if this IS a dock. */
            bool dock_ok = true;
            if (target_ring >= 2 && intent->build_module_type != MODULE_DOCK) {
                int filled = ring_module_count(docked_st, target_ring);
                int capacity = RING_PORT_COUNT[target_ring];
                if (filled + 1 >= capacity && !ring_has_dock(docked_st, target_ring))
                    dock_ok = false; /* must reserve last slot for a dock */
            }
            if (sp->ship.credits >= cost
                && ring1_ok && dock_ok
                && station_has_ring(docked_st, target_ring)
                && docked_st->module_count < MAX_MODULES_PER_STATION
                && target_slot >= 0 && target_slot < RING_PORT_COUNT[target_ring]
                && slot_free) {
                sp->ship.credits -= cost;
                begin_module_construction_at(w, docked_st, sp->current_station,
                                            intent->build_module_type, target_ring, target_slot);
            }
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

static void step_player(world_t *w, server_player_t *sp, float dt) {
    sp->hover_asteroid = -1;
    sp->nearby_fragments = 0;
    sp->tractor_fragments = 0;

    if (!sp->docked) {
        /* Signal attenuation: scale controls by station signal strength */
        float sig = signal_strength_at(w, sp->ship.pos);
        float signal_scale = signal_control_scale(sig);
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
        step_ship_motion(&sp->ship, dt, w, sig);
        /* Tow drag: each fragment adds drag, slowing the ship */
        if (sp->ship.towed_count > 0) {
            float tow_drag = 0.15f * (float)sp->ship.towed_count;
            sp->ship.vel = v2_scale(sp->ship.vel, 1.0f / (1.0f + tow_drag * dt));
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
                    float afford = (price_per > FLOAT_EPSILON) ? floorf(sp->ship.credits / price_per) : 0.0f;
                    float amount = fminf(fminf(available, 1.0f), afford); /* buy 1 at a time */
                    if (amount > FLOAT_EPSILON) {
                        float cost = amount * price_per;
                        sp->ship.credits -= cost;
                        sp->ship.stat_credits_spent += cost;
                        sp->ship.cargo[c] += amount;
                        nearby_st->inventory[c] -= amount;
                        emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = sp->id});
                    }
                }
            }
            /* Repair is now passive while docked — no laser interaction needed */
        }
        if (!sp->docked) {
            update_targeting_state(w, sp, forward);
            step_mining_system(w, sp, dt, sp->input.mine, forward, sig);
            if (!w->player_only_mode) {
                /* R toggles tractor — OFF releases fragments */
                if (sp->input.release_tow) {
                    sp->ship.tractor_active = !sp->ship.tractor_active;
                    if (!sp->ship.tractor_active) release_towed_fragments(sp);
                }
                step_towed_cleanup(w, sp);
                if (sp->ship.tractor_active) step_fragment_collection(w, sp, dt);
            }
        }
    } else {
        update_docking_state(w, sp, dt);
        if (!w->player_only_mode)
            step_station_interaction_system(w, sp, &sp->input);
    }

    /* Hail: collect pending credits from nearby station(s) */
    if (sp->input.hail && !w->player_only_mode) {
        handle_hail(w, sp);
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
    sp->input.hail = false;
    sp->input.release_tow = false;
}

/* ================================================================== */
/* Asteroid-asteroid gravity                                          */
/* ================================================================== */

static void step_asteroid_gravity(world_t *w, float dt) {
    /* Build spatial grid for neighbor lookups */
    spatial_grid_build(w);
    const spatial_grid_t *g = &w->asteroid_grid;

    /* Asteroid-asteroid attraction (non-S tier, within 800 units) via spatial grid */
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active || a->tier == ASTEROID_TIER_S) continue;
        int cx, cy;
        spatial_grid_cell(g, a->pos, &cx, &cy);
        int x0 = (cx > 0) ? cx - 1 : 0;
        int x1 = (cx < SPATIAL_GRID_DIM - 1) ? cx + 1 : SPATIAL_GRID_DIM - 1;
        int y0 = (cy > 0) ? cy - 1 : 0;
        int y1 = (cy < SPATIAL_GRID_DIM - 1) ? cy + 1 : SPATIAL_GRID_DIM - 1;
        for (int gy = y0; gy <= y1; gy++) {
            for (int gx = x0; gx <= x1; gx++) {
                const spatial_cell_t *cell = &g->cells[gy][gx];
                for (int ci = 0; ci < cell->count; ci++) {
                    int j = cell->indices[ci];
                    if (j <= i) continue; /* avoid double-processing */
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
        }
    }

    /* Industrial pull: only stations with active intake/processing modules
     * generate asteroid attraction. Pull scales with industrial activity
     * and inversely with asteroid size (fragments pulled strongly). */
    /* Precompute per-station intake module count */
    int station_intake[MAX_STATIONS];
    for (int s = 0; s < MAX_STATIONS; s++) {
        station_intake[s] = 0;
        const station_t *st = &w->stations[s];
        if (st->scaffold) continue;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_type_t mt = st->modules[m].type;
            if (mt == MODULE_ORE_BUYER || mt == MODULE_FURNACE ||
                mt == MODULE_FURNACE_CU || mt == MODULE_FURNACE_CR)
                station_intake[s]++;
        }
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            int intake_modules = station_intake[s];
            if (intake_modules == 0) continue; /* relay-only or scaffold stations: no pull */
            const station_t *st = &w->stations[s];
            vec2 delta = v2_sub(st->pos, a->pos);
            float dist_sq = v2_len_sq(delta);
            float pull_range = 600.0f + (float)intake_modules * 100.0f;
            if (dist_sq > pull_range * pull_range || dist_sq < 1.0f) continue;
            float dist = sqrtf(dist_sq);
            float min_dist = a->radius + st->radius;
            if (dist < min_dist + 10.0f) continue;
            vec2 normal = v2_scale(delta, 1.0f / dist);
            /* Tier-dependent: smaller = more pulled. radius² inversely scales force */
            float mass_a = a->radius * a->radius;
            float base_force = (float)intake_modules * 2.5f;
            float force = base_force * st->radius / (dist * 0.8f);
            /* TIER_S fragments get extra pull for hopper feeding */
            if (a->tier == ASTEROID_TIER_S) force *= 3.0f;
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
        {
            int acx, acy;
            spatial_grid_cell(g, a->pos, &acx, &acy);
            int ax0 = (acx > 0) ? acx - 1 : 0;
            int ax1 = (acx < SPATIAL_GRID_DIM - 1) ? acx + 1 : SPATIAL_GRID_DIM - 1;
            int ay0 = (acy > 0) ? acy - 1 : 0;
            int ay1 = (acy < SPATIAL_GRID_DIM - 1) ? acy + 1 : SPATIAL_GRID_DIM - 1;
            for (int gy = ay0; gy <= ay1 && !near_asteroid; gy++) {
                for (int gx = ax0; gx <= ax1 && !near_asteroid; gx++) {
                    const spatial_cell_t *cell = &g->cells[gy][gx];
                    for (int ci = 0; ci < cell->count; ci++) {
                        int j = cell->indices[ci];
                        if (j == i || !w->asteroids[j].active) continue;
                        if (v2_dist_sq(a->pos, w->asteroids[j].pos) <= 400.0f * 400.0f) {
                            near_asteroid = true;
                            break;
                        }
                    }
                }
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

/* Hopper intake: ore buyer modules pull and consume nearby TIER_S fragments */
/* Hopper intake: pull nearby S-tier fragments and consume on contact.
 * Credits the player who was towing the fragment (if any). */
static void step_hopper_intake(world_t *w, float dt) {
    float pull_range = HOPPER_PULL_RANGE;
    float pull_sq = pull_range * pull_range;
    float consume_sq = HOPPER_CONSUME_RANGE * HOPPER_CONSUME_RANGE;

    for (int s = 0; s < MAX_STATIONS; s++) {
        station_t *st = &w->stations[s];
        if (st->scaffold) continue;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].type != MODULE_ORE_BUYER || st->modules[m].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[m].ring, st->modules[m].slot);

            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                asteroid_t *a = &w->asteroids[i];
                if (!a->active || a->tier != ASTEROID_TIER_S) continue;
                vec2 to_mod = v2_sub(mp, a->pos);
                float d_sq = v2_len_sq(to_mod);
                if (d_sq > pull_sq) continue;

                /* Pull fragment toward hopper */
                float d = sqrtf(d_sq);
                if (d > 0.5f) {
                    float strength = HOPPER_PULL_ACCEL * (1.0f - d / pull_range);
                    vec2 dir = v2_scale(to_mod, 1.0f / d);
                    a->vel = v2_add(a->vel, v2_scale(dir, strength * dt));
                    /* Heavy damping — kill tangential velocity so they don't orbit */
                    a->vel = v2_scale(a->vel, 1.0f / (1.0f + 6.0f * dt));
                    /* Speed cap */
                    float spd = v2_len(a->vel);
                    if (spd > 120.0f) a->vel = v2_scale(a->vel, 120.0f / spd);
                }

                /* Consume: within mouth range (generous) */
                if (d_sq <= consume_sq) {
                    float ore_value = a->ore * station_buy_price(st, a->commodity);
                    /* Split payment between fracturer and tower.
                     * Same player or only one exists: 100%. Different: 50/50. */
                    int tower = (a->last_towed_by >= 0 && a->last_towed_by < MAX_PLAYERS
                                 && w->players[a->last_towed_by].connected)
                                ? a->last_towed_by : -1;
                    int fracturer = (a->last_fractured_by >= 0 && a->last_fractured_by < MAX_PLAYERS
                                     && w->players[a->last_fractured_by].connected)
                                    ? a->last_fractured_by : -1;

                    if (ore_value > 0.0f) {
                        if (tower >= 0 && fracturer >= 0 && tower != fracturer) {
                            /* Cooperation bonus: each gets 75% (150% total) */
                            float share = ore_value * 0.75f;
                            w->players[tower].ship.credits += share;
                            w->players[tower].ship.stat_credits_earned += share;
                            emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = tower});
                            w->players[fracturer].ship.credits += share;
                            w->players[fracturer].ship.stat_credits_earned += share;
                            emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = fracturer});
                        } else {
                            /* Solo or only one role: 100% to that player */
                            int best_p = (tower >= 0) ? tower : fracturer;
                            if (best_p >= 0) {
                                w->players[best_p].ship.credits += ore_value;
                                w->players[best_p].ship.stat_credits_earned += ore_value;
                                emit_event(w, (sim_event_t){.type = SIM_EVENT_SELL, .player_id = best_p});
                            }
                        }
                    }

                    /* Clean from tower's tow list */
                    if (tower >= 0) {
                        server_player_t *sp = &w->players[tower];
                        for (int t = 0; t < sp->ship.towed_count; t++) {
                            if (sp->ship.towed_fragments[t] == i) {
                                sp->ship.towed_count--;
                                sp->ship.towed_fragments[t] = sp->ship.towed_fragments[sp->ship.towed_count];
                                sp->ship.towed_fragments[sp->ship.towed_count] = -1;
                                break;
                            }
                        }
                    }
                    st->inventory[a->commodity] += a->ore;
                    clear_asteroid(a);
                }
            }
        }
    }
}

/* ================================================================== */
/* Asteroid-asteroid collision                                        */
/* ================================================================== */

static void resolve_asteroid_collisions(world_t *w) {
    const spatial_grid_t *g = &w->asteroid_grid;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        int cx, cy;
        spatial_grid_cell(g, a->pos, &cx, &cy);
        int x0 = (cx > 0) ? cx - 1 : 0;
        int x1 = (cx < SPATIAL_GRID_DIM - 1) ? cx + 1 : SPATIAL_GRID_DIM - 1;
        int y0 = (cy > 0) ? cy - 1 : 0;
        int y1 = (cy < SPATIAL_GRID_DIM - 1) ? cy + 1 : SPATIAL_GRID_DIM - 1;
        for (int gy = y0; gy <= y1; gy++) {
            for (int gx = x0; gx <= x1; gx++) {
                const spatial_cell_t *cell = &g->cells[gy][gx];
                for (int ci = 0; ci < cell->count; ci++) {
                    int j = cell->indices[ci];
                    if (j <= i) continue; /* avoid double-processing */
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
    }
}

/* ================================================================== */
/* Asteroid-station collision                                         */
/* ================================================================== */

static void resolve_asteroid_module_collision(asteroid_t *a, vec2 mod_pos, float mod_radius) {
    float min_dist = a->radius + mod_radius;
    vec2 delta = v2_sub(a->pos, mod_pos);
    float dist_sq = v2_len_sq(delta);
    if (dist_sq >= min_dist * min_dist) return;
    float dist = sqrtf(dist_sq);
    if (dist < 0.001f) { dist = 0.001f; delta = v2(1.0f, 0.0f); }
    vec2 normal = v2_scale(delta, 1.0f / dist);
    float overlap = min_dist - dist;
    a->pos = v2_add(a->pos, v2_scale(normal, overlap + 1.0f));
    float vel_along = v2_dot(a->vel, normal);
    if (vel_along < 0.0f)
        a->vel = v2_sub(a->vel, v2_scale(normal, vel_along * 1.0f));
    a->net_dirty = true;
}

static void resolve_asteroid_station_collisions(world_t *w) {
    /* Precompute module world positions */
    struct { vec2 pos; bool valid; } mod_cache[MAX_STATIONS][MAX_MODULES_PER_STATION];
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_exists(&w->stations[s])) {
            for (int m = 0; m < MAX_MODULES_PER_STATION; m++) mod_cache[s][m].valid = false;
            continue;
        }
        for (int m = 0; m < w->stations[s].module_count; m++) {
            int ring = w->stations[s].modules[m].ring;
            if (ring < 1 || ring > STATION_NUM_RINGS) { mod_cache[s][m].valid = false; continue; }
            mod_cache[s][m].pos = module_world_pos_ring(&w->stations[s], ring, w->stations[s].modules[m].slot);
            mod_cache[s][m].valid = true;
        }
        for (int m = w->stations[s].module_count; m < MAX_MODULES_PER_STATION; m++)
            mod_cache[s][m].valid = false;
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        for (int s = 0; s < MAX_STATIONS; s++) {
            if (!station_exists(&w->stations[s])) continue;
            for (int m = 0; m < w->stations[s].module_count; m++) {
                if (!mod_cache[s][m].valid) continue;
                resolve_asteroid_module_collision(a, mod_cache[s][m].pos, MODULE_COLLISION_RADIUS);
            }
        }
    }
}

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
                    .base_price = st->base_price[module_build_material(st->modules[m].type)] * 1.15f,
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
                    .base_price = 23.0f,
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
                    .active = true, .action = CONTRACT_SUPPLY,
                    .station_index = (uint8_t)s,
                    .commodity = checks[worst_idx].ingot,
                    .quantity_needed = worst_deficit,
                    .base_price = st->base_price[checks[worst_idx].ingot] * 1.15f,
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
    step_hopper_intake(w, dt);
    sim_step_refinery_production(w, dt);
    sim_step_station_production(w, dt);
    step_module_construction(w, dt);
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
    w->stations[0].radius      = 40.0f;
    w->stations[0].dock_radius = 240.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_ORE] = 10.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_ORE] = 14.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_ORE] = 18.0f;
    w->stations[0].base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    w->stations[0].base_price[COMMODITY_CUPRITE_INGOT] = 32.0f;
    w->stations[0].base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    w->stations[0].signal_range = 18000.0f;
    /* Ring 1 (service): dock + relay (center) + repair */
    add_module_at(&w->stations[0], MODULE_DOCK, 1, 0);
    add_module_at(&w->stations[0], MODULE_SIGNAL_RELAY, 1, 1);
    /* Slot 2 empty — gap for ship entry */
    /* Ring 2 (industrial): ore hopper + furnace */
    add_module_at(&w->stations[0], MODULE_ORE_BUYER, 2, 1);
    add_module_at(&w->stations[0], MODULE_FURNACE, 2, 2);
    w->stations[0].arm_count = 2;
    w->stations[0].arm_speed[0] = STATION_RING_SPEED;
    w->stations[0].ring_offset[0] = 0.0f;
    w->stations[0].ring_offset[1] = 1.05f;  /* ~60° offset — unique silhouette */
    rebuild_station_services(&w->stations[0]);
    /* Seed inventory: refinery starts with some smelted ingots */
    w->stations[0].inventory[COMMODITY_FERRITE_INGOT] = 20.0f;

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
    /* Ring 1 (service): dock + relay (center) + repair */
    add_module_at(&w->stations[1], MODULE_DOCK, 1, 0);
    add_module_at(&w->stations[1], MODULE_SIGNAL_RELAY, 1, 1);
    /* Slot 2 empty — gap for ship entry */
    /* Ring 2 (industrial): fabrication + services */
    add_module_at(&w->stations[1], MODULE_FRAME_PRESS, 2, 0);
    add_module_at(&w->stations[1], MODULE_LASER_FAB, 2, 1);
    add_module_at(&w->stations[1], MODULE_TRACTOR_FAB, 2, 2);
    add_module_at(&w->stations[1], MODULE_CONTRACT_BOARD, 2, 3);
    add_module_at(&w->stations[1], MODULE_BLUEPRINT_DESK, 2, 4);
    w->stations[1].arm_count = 2;
    w->stations[1].arm_speed[0] = STATION_RING_SPEED;
    w->stations[1].ring_offset[0] = 0.0f;
    w->stations[1].ring_offset[1] = 2.40f;  /* ~137° offset */
    rebuild_station_services(&w->stations[1]);
    /* Seed inventory: yard starts with frames for hold upgrades */
    w->stations[1].inventory[COMMODITY_FERRITE_INGOT] = 15.0f;
    w->stations[1].inventory[COMMODITY_FRAME] = 12.0f;

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
    /* Ring 1 (service): dock + relay (center) + repair */
    add_module_at(&w->stations[2], MODULE_DOCK, 1, 0);
    add_module_at(&w->stations[2], MODULE_SIGNAL_RELAY, 1, 1);
    /* Slot 2 empty — gap for ship entry */
    /* Ring 2 (industrial): production + services */
    add_module_at(&w->stations[2], MODULE_ORE_BUYER, 2, 0);
    add_module_at(&w->stations[2], MODULE_FURNACE, 2, 1);
    add_module_at(&w->stations[2], MODULE_LASER_FAB, 2, 2);
    add_module_at(&w->stations[2], MODULE_TRACTOR_FAB, 2, 3);
    add_module_at(&w->stations[2], MODULE_CONTRACT_BOARD, 2, 4);
    add_module_at(&w->stations[2], MODULE_BLUEPRINT_DESK, 2, 5);
    /* Ring 3: heavy industry — hopper feeds adjacent furnaces */
    add_module_at(&w->stations[2], MODULE_FURNACE_CU, 3, 1);
    add_module_at(&w->stations[2], MODULE_ORE_BUYER, 3, 2);
    add_module_at(&w->stations[2], MODULE_FURNACE_CR, 3, 3);
    add_module_at(&w->stations[2], MODULE_FRAME_PRESS, 3, 4);
    add_module_at(&w->stations[2], MODULE_ORE_SILO, 3, 5);
    w->stations[2].arm_count = 3;
    w->stations[2].arm_speed[0] = STATION_RING_SPEED;
    w->stations[2].ring_offset[0] = 0.0f;
    w->stations[2].ring_offset[1] = 0.52f;  /* ~30° offset */
    w->stations[2].ring_offset[2] = 1.83f;  /* ~105° offset */
    rebuild_station_services(&w->stations[2]);
    /* Seed inventory: works starts with modules for mining/tractor upgrades */
    w->stations[2].inventory[COMMODITY_CUPRITE_INGOT] = 15.0f;
    w->stations[2].inventory[COMMODITY_CRYSTAL_INGOT] = 15.0f;
    w->stations[2].inventory[COMMODITY_LASER_MODULE] = 10.0f;
    w->stations[2].inventory[COMMODITY_TRACTOR_MODULE] = 10.0f;
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
    sp->ship.credits    = 50.0f;
    sp->ship.angle      = PI_F * 0.5f;
    memset(sp->ship.towed_fragments, -1, sizeof(sp->ship.towed_fragments));
    sp->ship.tractor_active = true;
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
#define SAVE_VERSION 16  /* bumped: added ring_offset per station ring */

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
    /* Ring rotation */
    WRITE_FIELD(f, s->arm_count);
    for (int a = 0; a < MAX_ARMS; a++) {
        WRITE_FIELD(f, s->arm_rotation[a]);
        WRITE_FIELD(f, s->arm_speed[a]);
        WRITE_FIELD(f, s->ring_offset[a]);
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
    /* Ring rotation */
    READ_FIELD(f, s->arm_count);
    if (s->arm_count < 0) s->arm_count = 0;
    if (s->arm_count > MAX_ARMS) s->arm_count = MAX_ARMS;
    for (int a = 0; a < MAX_ARMS; a++) {
        READ_FIELD(f, s->arm_rotation[a]);
        READ_FIELD(f, s->arm_speed[a]);
        READ_FIELD(f, s->ring_offset[a]);
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

static void session_token_to_hex(const uint8_t token[8], char hex[17]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        hex[i * 2]     = digits[token[i] >> 4];
        hex[i * 2 + 1] = digits[token[i] & 0x0F];
    }
    hex[16] = '\0';
}

bool player_save(const server_player_t *sp, const char *dir, int slot) {
    char path[256];
    /* Use session token for filename if available, fall back to slot */
    static const uint8_t zero_token[8] = {0};
    if (sp->session_ready && memcmp(sp->session_token, zero_token, 8) != 0) {
        char hex[17];
        session_token_to_hex(sp->session_token, hex);
        snprintf(path, sizeof(path), "%s/player_%s.sav", dir, hex);
    } else {
        snprintf(path, sizeof(path), "%s/player_%d.sav", dir, slot);
    }
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

static bool player_load_from_path(server_player_t *sp, world_t *w, const char *path, int slot) {
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
    (void)slot;
    SIM_LOG("[sim] loaded player %d (%.0f credits, station %d)\n",
            slot, sp->ship.credits, sp->current_station);
    return true;
}

bool player_load(server_player_t *sp, world_t *w, const char *dir, int slot) {
    char path[256];
    snprintf(path, sizeof(path), "%s/player_%d.sav", dir, slot);
    return player_load_from_path(sp, w, path, slot);
}

bool player_load_by_token(server_player_t *sp, world_t *w, const char *dir,
                          const uint8_t token[8]) {
    char hex[17];
    session_token_to_hex(token, hex);
    char path[256];
    snprintf(path, sizeof(path), "%s/player_%s.sav", dir, hex);
    return player_load_from_path(sp, w, path, (int)sp->id);
}
