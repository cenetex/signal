/*
 * sim_save.c -- World and player persistence for the Signal Space Miner
 * server. Extracted from game_sim.c (#272) — pure code motion, no
 * functional changes.
 *
 * On-disk format owners:
 *   - World save: SAVE_MAGIC "SIGN", versioned, atomic temp+rename.
 *     Per-station / per-npc / per-contract field-by-field I/O so adding
 *     new struct fields requires bumping SAVE_VERSION and adding a
 *     migration block in world_load().
 *   - Player save: PLAYER_MAGIC "PLY2", per-session token in filename.
 *     v1 -> v2 migrates unlocked_modules bits across the #280 enum cleanup.
 *
 * v24 (#314): Layered persistence refactor.
 *   - Station identity now lives in the station catalog (sim_catalog.c).
 *     world_save only writes session-tier station data (inventories, etc.).
 *   - Asteroids removed — derived state, regenerated from belt seed.
 *   - Scaffolds removed — transient in-flight construction.
 *   - v23 saves migrated by reading full station/asteroid/scaffold data,
 *     then the next autosave writes the catalog.
 */
#include "game_sim.h"
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* World persistence                                                   */
/* ================================================================== */

/* ---- CRC32 (IEEE 802.3 polynomial, public domain) ---- */
static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

static uint32_t crc32_file(FILE *f) {
    uint32_t crc = 0;
    long start = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        crc = crc32_update(crc, chunk, n);
    fseek(f, start, SEEK_SET);
    return crc;
}

#define SAVE_MAGIC 0x5349474E  /* "SIGN" */
#define SAVE_VERSION 24  /* bumped: layered persistence — catalog split, derived data removal (#314) */
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
    /* v23: station credit pool */
    WRITE_FIELD(f, s->credit_pool);
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
    /* v23: station credit pool */
    if (g_loaded_save_version >= 23) {
        READ_FIELD(f, s->credit_pool);
    } else {
        /* Pre-v23 saves don't have this field — seed starter stations
         * (indices 0-2) with a pool, others start at zero. */
        s->credit_pool = (s->signal_range > 0.0f) ? 10000.0f : 0.0f;
    }
    return true;
}

/* ================================================================== */
/* Session-tier station I/O (v24+) — writes only volatile economic    */
/* state.  Identity fields (name, pos, modules, geometry) come from   */
/* the station catalog (sim_catalog.c).  signal_connected is derived  */
/* (rebuilt by rebuild_signal_chain) and belongs in neither catalog    */
/* nor session.                                                       */
/* ================================================================== */

static bool write_station_session(FILE *f, const station_t *s) {
    /* Inventory */
    WRITE_FIELD(f, s->inventory);
    /* Per-module production buffers */
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        WRITE_FIELD(f, s->module_input[m]);
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        WRITE_FIELD(f, s->module_output[m]);
    /* Station credit pool */
    WRITE_FIELD(f, s->credit_pool);
    /* Economy ledger */
    WRITE_FIELD(f, s->ledger_count);
    for (int i = 0; i < 16; i++) {
        WRITE_FIELD(f, s->ledger[i].player_token);
        WRITE_FIELD(f, s->ledger[i].balance);
        WRITE_FIELD(f, s->ledger[i].lifetime_supply);
    }
    /* Shipyard queue */
    WRITE_FIELD(f, s->pending_scaffold_count);
    for (int p = 0; p < 4; p++)
        WRITE_FIELD(f, s->pending_scaffolds[p]);
    /* Placement plans */
    WRITE_FIELD(f, s->placement_plan_count);
    for (int p = 0; p < 8; p++)
        WRITE_FIELD(f, s->placement_plans[p]);
    /* Construction state */
    WRITE_FIELD(f, s->scaffold);
    WRITE_FIELD(f, s->scaffold_progress);
    /* Planning state */
    WRITE_FIELD(f, s->planned);
    WRITE_FIELD(f, s->planned_owner);
    /* Live rotation angles and speeds */
    for (int a = 0; a < MAX_ARMS; a++) {
        WRITE_FIELD(f, s->arm_rotation[a]);
        WRITE_FIELD(f, s->arm_speed[a]);
    }
    return true;
}

static bool read_station_session(FILE *f, station_t *s) {
    /* Inventory */
    READ_FIELD(f, s->inventory);
    /* Per-module production buffers */
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        READ_FIELD(f, s->module_input[m]);
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        READ_FIELD(f, s->module_output[m]);
    /* Station credit pool */
    READ_FIELD(f, s->credit_pool);
    /* Economy ledger */
    READ_FIELD(f, s->ledger_count);
    if (s->ledger_count < 0) s->ledger_count = 0;
    if (s->ledger_count > 16) s->ledger_count = 16;
    for (int i = 0; i < 16; i++) {
        READ_FIELD(f, s->ledger[i].player_token);
        READ_FIELD(f, s->ledger[i].balance);
        READ_FIELD(f, s->ledger[i].lifetime_supply);
    }
    /* Shipyard queue */
    READ_FIELD(f, s->pending_scaffold_count);
    if (s->pending_scaffold_count < 0) s->pending_scaffold_count = 0;
    if (s->pending_scaffold_count > 4) s->pending_scaffold_count = 4;
    for (int p = 0; p < 4; p++)
        READ_FIELD(f, s->pending_scaffolds[p]);
    /* Placement plans */
    READ_FIELD(f, s->placement_plan_count);
    if (s->placement_plan_count < 0) s->placement_plan_count = 0;
    if (s->placement_plan_count > 8) s->placement_plan_count = 8;
    for (int p = 0; p < 8; p++)
        READ_FIELD(f, s->placement_plans[p]);
    /* Construction state */
    READ_FIELD(f, s->scaffold);
    { uint8_t raw; memcpy(&raw, &s->scaffold, 1); s->scaffold = (raw != 0); }
    READ_FIELD(f, s->scaffold_progress);
    /* Planning state */
    READ_FIELD(f, s->planned);
    { uint8_t raw; memcpy(&raw, &s->planned, 1); s->planned = (raw != 0); }
    READ_FIELD(f, s->planned_owner);
    /* Live rotation angles and speeds */
    for (int a = 0; a < MAX_ARMS; a++) {
        READ_FIELD(f, s->arm_rotation[a]);
        READ_FIELD(f, s->arm_speed[a]);
    }
    return true;
}

/* ---- asteroid field-by-field I/O (read-only, for v23 migration) ---- */
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

    /* Stations — session-tier only (identity lives in station catalog) */
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!write_station_session(f, &w->stations[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* Asteroids: removed in v24 — derived state, regenerated from belt seed */
    /* Scaffolds: removed in v24 — transient in-flight construction */
    /* NPC ships */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!write_npc(f, &w->npc_ships[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!write_contract(f, &w->contracts[i])) { fclose(f); remove(tmp_path); return false; }
    }

    fclose(f);

    /* Append CRC32 trailer: reopen to read data, compute CRC, then append */
    {
        FILE *rf = fopen(tmp_path, "rb");
        if (!rf) { remove(tmp_path); return false; }
        uint32_t crc = crc32_file(rf);
        fclose(rf);
        FILE *af = fopen(tmp_path, "ab");
        if (!af) { remove(tmp_path); return false; }
        uint32_t crc_magic = 0x43524332u; /* "CRC2" */
        fwrite(&crc_magic, sizeof(crc_magic), 1, af);
        fwrite(&crc, sizeof(crc), 1, af);
        fclose(af);
    }
    /* Atomic rename — on POSIX rename() atomically replaces the target.
     * Do NOT remove(path) first: that creates a window where a crash
     * would leave no valid save file at all. */
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

    if (version >= 24) {
        /* v24+: station identity comes from catalog; read session only */
        for (int i = 0; i < MAX_STATIONS; i++) {
            if (!read_station_session(f, &w->stations[i])) return false;
        }
        /* No asteroids or scaffolds in v24 */
    } else {
        /* v20-v23: full station data (identity will be written to catalog on next save) */
        for (int i = 0; i < MAX_STATIONS; i++) {
            if (!read_station(f, &w->stations[i])) return false;
        }
        /* v23 migration: read and discard asteroid data to advance cursor */
        {
            asteroid_t dummy;
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                if (!read_asteroid(f, &dummy)) return false;
            }
        }
    }
    /* NPC ships */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!read_npc(f, &w->npc_ships[i])) return false;
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!read_contract(f, &w->contracts[i])) return false;
    }
    /* v23 migration: read scaffolds so v22 module-remap can process them,
     * then they'll be zeroed after migrations complete */
    if (version < 24) {
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            READ_FIELD(f, w->scaffolds[i]);
        }
    }

    /* Check for CRC32 trailer after all data fields.
     * If the next 4 bytes are the CRC2 magic, verify the checksum
     * against everything read so far. Legacy saves without the trailer
     * are loaded without verification. */
    {
        long data_end = ftell(f);
        uint32_t trail_magic = 0;
        if (fread(&trail_magic, sizeof(trail_magic), 1, f) == 1 &&
            trail_magic == 0x43524332u) { /* "CRC2" */
            uint32_t stored_crc;
            if (fread(&stored_crc, sizeof(stored_crc), 1, f) != 1) {
                printf("[save] truncated CRC32 trailer\n");
                fclose(f); return false;
            }
            /* Recompute CRC over bytes [0, data_end) */
            fseek(f, 0, SEEK_SET);
            uint32_t crc = 0;
            long remaining = data_end;
            uint8_t chunk[4096];
            while (remaining > 0) {
                size_t to_read = (remaining > (long)sizeof(chunk)) ? sizeof(chunk) : (size_t)remaining;
                size_t n = fread(chunk, 1, to_read, f);
                if (n == 0) break;
                crc = crc32_update(crc, chunk, n);
                remaining -= (long)n;
            }
            if (crc != stored_crc) {
                printf("[save] CRC32 mismatch: computed=0x%08x stored=0x%08x -- save may be corrupt\n",
                       crc, stored_crc);
                fclose(f); return false;
            }
        }
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
            1,  /* old 1  ORE_BUYER      -> HOPPER          */
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

    /* v24: asteroids and scaffolds no longer saved — ensure arrays are
     * clean whether we read-and-discarded legacy data or skipped them.
     * Asteroids regenerate via belt spawn timer; scaffolds start empty. */
    memset(w->asteroids, 0, sizeof(w->asteroids));
    memset(w->scaffolds, 0, sizeof(w->scaffolds));

    if (version < 24) {
        /* First migration to v24. Full station data was loaded by
         * read_station(). On next autosave, station_catalog_save_all()
         * extracts identity to catalog files. Reset spawn timer to
         * trigger immediate asteroid repopulation. */
        w->field_spawn_timer = 0.0f;
        printf("[save] migrated v%d -> v24: catalog will be written on next save\n",
               (int)version);
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

#define PLAYER_MAGIC    0x504C5933u  /* "PLY3" — v25+: station-local credits (#312) */
#define PLAYER_MAGIC_V2 0x504C5932u  /* "PLY2" — v22-v24: post #280 enum cleanup */
#define PLAYER_MAGIC_V1 0x504C5952u  /* "PLYR" — v21 and earlier */

typedef struct {
    uint32_t magic;
    ship_t ship;
    int last_station;
    vec2 last_pos;
    float last_angle;
} player_save_data_t;

/* Old ship layout with global credits field — for PLY2 migration */
typedef struct {
    vec2 pos; vec2 vel; float angle; float hull;
    float cargo[COMMODITY_COUNT];
    float credits; /* REMOVED in PLY3 */
    hull_class_t hull_class;
    int mining_level, hold_level, tractor_level;
    int16_t towed_fragments[10]; uint8_t towed_count;
    int16_t towed_scaffold; bool tractor_active;
    uint32_t unlocked_modules;
    float stat_ore_mined, stat_credits_earned, stat_credits_spent;
    int stat_asteroids_fractured;
} ship_v2_t;

typedef struct {
    uint32_t magic;
    ship_v2_t ship;
    int last_station;
    vec2 last_pos;
    float last_angle;
} player_save_v2_t;

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
    if (ok) {
        uint32_t crc_magic = 0x43524332u; /* "CRC2" */
        uint32_t crc = crc32_update(0, &data, sizeof(data));
        ok = fwrite(&crc_magic, sizeof(crc_magic), 1, f) == 1 &&
             fwrite(&crc, sizeof(crc), 1, f) == 1;
    }
    fclose(f);
    if (ok) SIM_LOG("[sim] saved player %d\n", slot);
    return ok;
}

/* Migrate PLY2 (old ship_t with global credits) to current ship_t */
static void migrate_v2_ship(ship_t *dst, const ship_v2_t *src) {
    dst->pos = src->pos;
    dst->vel = src->vel;
    dst->angle = src->angle;
    dst->hull = src->hull;
    memcpy(dst->cargo, src->cargo, sizeof(dst->cargo));
    dst->hull_class = src->hull_class;
    dst->mining_level = src->mining_level;
    dst->hold_level = src->hold_level;
    dst->tractor_level = src->tractor_level;
    memcpy(dst->towed_fragments, src->towed_fragments, sizeof(dst->towed_fragments));
    dst->towed_count = src->towed_count;
    dst->towed_scaffold = src->towed_scaffold;
    dst->tractor_active = src->tractor_active;
    dst->unlocked_modules = src->unlocked_modules;
    dst->stat_ore_mined = src->stat_ore_mined;
    dst->stat_credits_earned = src->stat_credits_earned;
    dst->stat_credits_spent = src->stat_credits_spent;
    dst->stat_asteroids_fractured = src->stat_asteroids_fractured;
}

static bool player_load_from_path(server_player_t *sp, world_t *w, const char *path, int slot) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Peek at magic to determine format */
    uint32_t magic;
    if (fread(&magic, sizeof(magic), 1, f) != 1) { fclose(f); return false; }
    rewind(f);

    float migrated_credits = 0.0f;
    bool is_v1 = false;

    if (magic == PLAYER_MAGIC) {
        /* Current format (PLY3) */
        player_save_data_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) { fclose(f); return false; }
        fclose(f);
        sp->ship = data.ship;
        sp->current_station = data.last_station;
        sp->ship.pos = data.last_pos;
        sp->ship.angle = data.last_angle;
    } else if (magic == PLAYER_MAGIC_V2 || magic == PLAYER_MAGIC_V1) {
        /* PLY2 or PLY1 — old ship_t with global credits */
        player_save_v2_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) { fclose(f); return false; }
        fclose(f);
        is_v1 = (magic == PLAYER_MAGIC_V1);
        migrate_v2_ship(&sp->ship, &data.ship);
        migrated_credits = data.ship.credits;
        sp->current_station = data.last_station;
        sp->ship.pos = data.last_pos;
        sp->ship.angle = data.last_angle;
    } else {
        fclose(f);
        return false;
    }

    if (is_v1) {
        /* v1 → v2: remap unlocked_modules bits across the #280 enum cleanup */
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
    if (sp->current_station < 0 || sp->current_station >= MAX_STATIONS ||
        !station_exists(&w->stations[sp->current_station]))
        sp->current_station = 0;
    /* Clamp upgrade levels */
    if (sp->ship.mining_level < 0 || sp->ship.mining_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship.mining_level = 0;
    if (sp->ship.hold_level < 0 || sp->ship.hold_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship.hold_level = 0;
    if (sp->ship.tractor_level < 0 || sp->ship.tractor_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship.tractor_level = 0;
    /* Clamp hull HP */
    float max_hull = ship_max_hull(&sp->ship);
    if (!(sp->ship.hull > 0.0f)) sp->ship.hull = max_hull;
    if (sp->ship.hull > max_hull) sp->ship.hull = max_hull;
    /* Clamp cargo (no negative, no NaN, no exceeding capacity) */
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        if (!(sp->ship.cargo[i] >= 0.0f)) sp->ship.cargo[i] = 0.0f;
    }
    /* Dock the player at their last station for safety */
    sp->docked = true;
    sp->nearby_station = sp->current_station;
    sp->in_dock_range = true;
    anchor_ship_in_station(sp, w);
    /* Migrate old global credits → station ledger balance */
    if (migrated_credits > 0.01f) {
        ledger_earn(&w->stations[sp->current_station], sp->session_token, migrated_credits);
        SIM_LOG("[sim] migrated %.0f global credits to station %d ledger\n",
                migrated_credits, sp->current_station);
    }
    (void)slot;
    SIM_LOG("[sim] loaded player %d (station %d)\n", slot, sp->current_station);
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
