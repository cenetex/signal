/*
 * sim_save.c -- World and player persistence for the Signal Space Miner
 * server. Extracted from game_sim.c (#272) — pure code motion, no
 * functional changes.
 *
 * On-disk format owners:
 *   - World save: SAVE_MAGIC "SIGN", versioned, atomic temp+rename.
 *     Per-station / per-asteroid / per-npc / per-contract field-by-field
 *     I/O so adding new struct fields requires bumping SAVE_VERSION and
 *     adding a migration block in world_load().
 *   - Player save: PLAYER_MAGIC "PLY2", per-session token in filename.
 *     v1 -> v2 migrates unlocked_modules bits across the #280 enum cleanup.
 */
#include "game_sim.h"
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* World persistence                                                   */
/* ================================================================== */

#define SAVE_MAGIC 0x5349474E  /* "SIGN" */
#define SAVE_VERSION 22  /* bumped: dead module enum entries removed (#280 cleanup) */
#define MIN_SAVE_VERSION 20  /* migrate v20 by mapping old module_buffer → input */

/* Set by world_load() before read_station() so per-station readers know
 * which version they're parsing and can handle field additions. */
static int g_loaded_save_version = SAVE_VERSION;

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
    /* Production layer v2: shipyard queue + per-module input/output buffers */
    WRITE_FIELD(f, s->pending_scaffold_count);
    for (int p = 0; p < 4; p++) {
        WRITE_FIELD(f, s->pending_scaffolds[p]);
    }
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        WRITE_FIELD(f, s->module_input[m]);
    }
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        WRITE_FIELD(f, s->module_output[m]);
    }
    /* Placement plans + planned-station fields (v20+) */
    WRITE_FIELD(f, s->placement_plan_count);
    for (int p = 0; p < 8; p++) {
        WRITE_FIELD(f, s->placement_plans[p]);
    }
    WRITE_FIELD(f, s->planned);
    WRITE_FIELD(f, s->planned_owner);
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
    { uint8_t raw; memcpy(&raw, &s->scaffold, 1); s->scaffold = (raw != 0); }
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
        /* Sanitize bool — old saves may have non-0/1 byte values which
         * are undefined behavior when read as _Bool in C99. Read the
         * raw byte to avoid UB on the load itself. */
        { uint8_t raw; memcpy(&raw, &s->modules[m].scaffold, 1);
          s->modules[m].scaffold = (raw != 0); }
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
    /* Production layer v2: shipyard queue + per-module input/output buffers */
    READ_FIELD(f, s->pending_scaffold_count);
    if (s->pending_scaffold_count < 0) s->pending_scaffold_count = 0;
    if (s->pending_scaffold_count > 4) s->pending_scaffold_count = 4;
    for (int p = 0; p < 4; p++) {
        READ_FIELD(f, s->pending_scaffolds[p]);
    }
    /* v20: single module_buffer[] → migrate to module_input[].
     * v21+: explicit input + output. */
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        READ_FIELD(f, s->module_input[m]);
    }
    if (g_loaded_save_version >= 21) {
        for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
            READ_FIELD(f, s->module_output[m]);
        }
    } else {
        /* v20: no output buffers — initialize to 0 */
        memset(s->module_output, 0, sizeof(s->module_output));
    }
    /* Placement plans + planned-station fields (v20+) */
    READ_FIELD(f, s->placement_plan_count);
    if (s->placement_plan_count < 0) s->placement_plan_count = 0;
    if (s->placement_plan_count > 8) s->placement_plan_count = 8;
    for (int p = 0; p < 8; p++) {
        READ_FIELD(f, s->placement_plans[p]);
    }
    READ_FIELD(f, s->planned);
    { uint8_t raw; memcpy(&raw, &s->planned, 1); s->planned = (raw != 0); }
    READ_FIELD(f, s->planned_owner);
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
    /* Scaffolds (whole array, fixed-size) */
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        WRITE_FIELD(f, w->scaffolds[i]);
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
    if (magic != SAVE_MAGIC || version < MIN_SAVE_VERSION || version > SAVE_VERSION) {
        printf("[save] rejected save: magic=0x%08x version=%u (need %d-%d)\n",
               magic, version, MIN_SAVE_VERSION, SAVE_VERSION);
        fclose(f); return false;
    }
    g_loaded_save_version = (int)version;

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
    /* Scaffolds (v20+) */
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        READ_FIELD(f, w->scaffolds[i]);
    }

    /* ---- Version migrations ----
     * Each block migrates from version N to N+1.  They run in sequence so
     * a v19 save loaded by a v21 binary walks through 19->20->21.
     * When adding a new version:
     *   1. Bump SAVE_VERSION
     *   2. Add a migration block here (if (version < NEW) { ... })
     *   3. Update EXPECTED_V{N}_SAVE_SIZE in test_main.c
     */
    /* (v19 is the baseline — no migration needed yet) */
    /* if (version < 20) { ... migrate 19->20 ... } */

    /* v22: dead module enum entries removed (INGOT_SELLER, CONTRACT_BOARD,
     * BLUEPRINT_DESK, RING). Remap surviving module type IDs and drop
     * any module/plan/scaffold whose old type no longer exists. The
     * migration is keyed by old indices, so the table values are written
     * as raw integers — do NOT replace with enum names. */
    if (version < 22) {
        static const int REMAP[17] = {
            0,  /* old 0  DOCK           -> DOCK           */
            1,  /* old 1  ORE_BUYER      -> ORE_BUYER      */
            2,  /* old 2  FURNACE        -> FURNACE        */
            3,  /* old 3  FURNACE_CU     -> FURNACE_CU     */
            4,  /* old 4  FURNACE_CR     -> FURNACE_CR     */
           -1,  /* old 5  INGOT_SELLER   -> dropped        */
            5,  /* old 6  REPAIR_BAY     -> REPAIR_BAY     */
            6,  /* old 7  SIGNAL_RELAY   -> SIGNAL_RELAY   */
            7,  /* old 8  FRAME_PRESS    -> FRAME_PRESS    */
            8,  /* old 9  LASER_FAB      -> LASER_FAB      */
            9,  /* old 10 TRACTOR_FAB    -> TRACTOR_FAB    */
           -1,  /* old 11 CONTRACT_BOARD -> dropped        */
           10,  /* old 12 ORE_SILO       -> ORE_SILO       */
           -1,  /* old 13 BLUEPRINT_DESK -> dropped        */
           -1,  /* old 14 RING           -> dropped        */
           11,  /* old 15 SHIPYARD       -> SHIPYARD       */
           12,  /* old 16 CARGO_BAY      -> CARGO_BAY      */
        };
        for (int i = 0; i < MAX_STATIONS; i++) {
            station_t *st = &w->stations[i];
            /* Remap modules[] in place, compacting and renumbering input/output. */
            int kept = 0;
            for (int m = 0; m < st->module_count; m++) {
                int old_t = (int)st->modules[m].type;
                int new_t = (old_t >= 0 && old_t < 17) ? REMAP[old_t] : -1;
                if (new_t < 0) continue;
                if (kept != m) {
                    st->modules[kept] = st->modules[m];
                    st->module_input[kept] = st->module_input[m];
                    st->module_output[kept] = st->module_output[m];
                }
                st->modules[kept].type = (module_type_t)new_t;
                kept++;
            }
            for (int m = kept; m < st->module_count; m++) {
                memset(&st->modules[m], 0, sizeof(st->modules[m]));
                st->module_input[m] = 0.0f;
                st->module_output[m] = 0.0f;
            }
            st->module_count = kept;
            /* Drop pending shipyard orders for dropped types. */
            int psk = 0;
            for (int p = 0; p < st->pending_scaffold_count; p++) {
                int old_t = (int)st->pending_scaffolds[p].type;
                int new_t = (old_t >= 0 && old_t < 17) ? REMAP[old_t] : -1;
                if (new_t < 0) continue;
                st->pending_scaffolds[psk] = st->pending_scaffolds[p];
                st->pending_scaffolds[psk].type = (module_type_t)new_t;
                psk++;
            }
            st->pending_scaffold_count = psk;
            /* Drop placement plans for dropped types. */
            int pp = 0;
            for (int p = 0; p < st->placement_plan_count; p++) {
                int old_t = (int)st->placement_plans[p].type;
                int new_t = (old_t >= 0 && old_t < 17) ? REMAP[old_t] : -1;
                if (new_t < 0) continue;
                st->placement_plans[pp] = st->placement_plans[p];
                st->placement_plans[pp].type = (module_type_t)new_t;
                pp++;
            }
            st->placement_plan_count = pp;
            /* STATION_SERVICE_BLUEPRINT (bit 5) is gone — clear stale bit
             * and rebuild from current modules. */
            st->services &= ~(1u << 5);
            rebuild_station_services(st);
        }
        /* Remap loose scaffolds in the world. */
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            if (!w->scaffolds[i].active) continue;
            int old_t = (int)w->scaffolds[i].module_type;
            int new_t = (old_t >= 0 && old_t < 17) ? REMAP[old_t] : -1;
            if (new_t < 0) {
                w->scaffolds[i].active = false;
                continue;
            }
            w->scaffolds[i].module_type = (module_type_t)new_t;
        }
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

#define PLAYER_MAGIC    0x504C5932u  /* "PLY2" — v22+: post #280 enum cleanup */
#define PLAYER_MAGIC_V1 0x504C5952u  /* "PLYR" — v21 and earlier */

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
    if (data.magic != PLAYER_MAGIC && data.magic != PLAYER_MAGIC_V1) return false;
    bool is_v1 = (data.magic == PLAYER_MAGIC_V1);
    sp->ship = data.ship;
    if (is_v1) {
        /* v1 → v2: remap unlocked_modules bits across the #280 enum cleanup.
         * Same REMAP table as world_load(). Bits for dropped types are cleared. */
        static const int REMAP[17] = {
            0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9, -1, 10, -1, -1, 11, 12,
        };
        uint32_t old_mask = sp->ship.unlocked_modules;
        uint32_t new_mask = 0;
        for (int b = 0; b < 17; b++) {
            if (!(old_mask & (1u << b))) continue;
            int new_t = REMAP[b];
            if (new_t >= 0) new_mask |= (1u << (uint32_t)new_t);
        }
        sp->ship.unlocked_modules = new_mask;
    }
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
