#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

/*
 * sim_catalog.c -- Per-station identity catalog persistence.
 *
 * Each station's permanent identity (name, position, modules, geometry,
 * pricing, hail message, slug, connectivity) is saved to an individual
 * binary file in stations/{index}.cat.  Session-tier data (inventory,
 * production buffers, credit pool, ledger, scaffolds, plans, rotation
 * angles) is NOT persisted here — it lives in the world save.
 *
 * File format: STNC magic, version u32, field-by-field binary, CRC32
 * trailer — mirrors the pattern used by sim_save.c for world/player saves.
 */
#include "sim_catalog.h"
#include "manifest.h"
#include "sim_construction.h"
#include "station_util.h"
#include "persistence_io.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define crc32_update          persistence_crc32_update
#define crc32_file            persistence_crc32_file
#define catalog_flush_durable persistence_flush_durable
#define catalog_replace_file  persistence_replace_file

#define CATALOG_MAGIC   0x53544E43  /* "STNC" */
#define CATALOG_VERSION 7  /* v7: Helios furnace set is 2x crystal + 1x cuprite.
                            * v6: station-authored NPC/RATi hail text.
                            * v5: repair Helios smelter ore/furnace adjacency.
                            * v4: repair Helios seed to include shipyard + frame hopper.
                            * v3: per-module commodity tag (hopper specialization). */

/* ---- helper macros (same pattern as sim_save.c) ---- */
#define WRITE_FIELD(f, val) do { if (fwrite(&(val), sizeof(val), 1, (f)) != 1) return false; } while(0)
#define READ_FIELD(f, val)  do { if (fread(&(val), sizeof(val), 1, (f)) != 1)  { fclose(f); return false; } } while(0)

/* ---- directory helper ---- */
static void ensure_dir(const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

static bool catalog_move_module(station_module_t *m, int ring, int slot) {
    bool changed = false;
    if ((int)m->ring != ring) { m->ring = (uint8_t)ring; changed = true; }
    if ((int)m->slot != slot) { m->slot = (uint8_t)slot; changed = true; }
    return changed;
}

static int catalog_find_module(const station_t *st, module_type_t type) {
    if (!st) return -1;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == type) return i;
    }
    return -1;
}

static int catalog_find_hopper_for(const station_t *st, commodity_t commodity) {
    if (!st) return -1;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_HOPPER) continue;
        if ((commodity_t)st->modules[i].commodity == commodity) return i;
    }
    return -1;
}

static bool catalog_place_module(station_t *st, module_type_t type, int ring, int slot) {
    if (!st) return false;
    int idx = catalog_find_module(st, type);
    if (idx < 0) {
        if (st->module_count >= MAX_MODULES_PER_STATION) return false;
        add_module_at(st, type, (uint8_t)ring, (uint8_t)slot);
        return true;
    }
    return catalog_move_module(&st->modules[idx], ring, slot);
}

static bool catalog_place_hopper(station_t *st, commodity_t commodity, int ring, int slot) {
    if (!st) return false;
    int idx = catalog_find_hopper_for(st, commodity);
    if (idx < 0) {
        if (st->module_count >= MAX_MODULES_PER_STATION) return false;
        add_hopper_for(st, (uint8_t)ring, (uint8_t)slot, commodity);
        return true;
    }
    bool changed = catalog_move_module(&st->modules[idx], ring, slot);
    if ((commodity_t)st->modules[idx].commodity != commodity) {
        st->modules[idx].commodity = (uint8_t)commodity;
        changed = true;
    }
    return changed;
}

typedef struct {
    commodity_t ingot;
    int ring;
    int slot;
} catalog_furnace_spec_t;

static bool catalog_normalize_furnaces(station_t *st,
                                       const catalog_furnace_spec_t *specs,
                                       int spec_count) {
    if (!st) return false;
    bool changed = false;
    int furnace_indices[MAX_MODULES_PER_STATION];
    int furnace_count = 0;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type != MODULE_FURNACE) continue;
        furnace_indices[furnace_count++] = i;
    }

    for (int i = 0; i < spec_count; i++) {
        if (i >= furnace_count) {
            if (st->module_count >= MAX_MODULES_PER_STATION) return changed;
            add_furnace_for(st, (uint8_t)specs[i].ring,
                            (uint8_t)specs[i].slot, specs[i].ingot);
            changed = true;
            continue;
        }

        station_module_t *m = &st->modules[furnace_indices[i]];
        if ((commodity_t)m->commodity != specs[i].ingot) {
            m->commodity = (uint8_t)specs[i].ingot;
            changed = true;
        }
        changed |= catalog_move_module(m, specs[i].ring, specs[i].slot);
    }

    for (int i = furnace_count - 1; i >= spec_count; i--) {
        int idx = furnace_indices[i];
        changed |= station_module_remove(st, idx);
    }
    return changed;
}

static bool catalog_normalize_helios_furnaces(station_t *st) {
    static const catalog_furnace_spec_t specs[] = {
        { COMMODITY_CRYSTAL_INGOT, 1, 2 },
        { COMMODITY_CRYSTAL_INGOT, 3, 4 },
        { COMMODITY_CUPRITE_INGOT, 3, 6 },
    };
    return catalog_normalize_furnaces(st, specs,
                                      (int)(sizeof(specs) / sizeof(specs[0])));
}

static bool station_catalog_migrate_v7_helios(station_t *st, int index, uint32_t ver) {
    if (!st || index != 2 || ver >= 7) return false;
    bool changed = catalog_normalize_helios_furnaces(st);
    if (changed) rebuild_station_services(st);
    return changed;
}

static bool station_catalog_migrate_v5_helios(station_t *st, int index, uint32_t ver) {
    if (!st || index != 2 || ver >= 5) return false;
    bool changed = false;

    changed |= catalog_place_module(st, MODULE_LASER_FAB, 2, 0);
    changed |= catalog_place_hopper(st, COMMODITY_CUPRITE_INGOT, 2, 1);
    changed |= catalog_place_module(st, MODULE_SHIPYARD, 2, 2);
    changed |= catalog_place_hopper(st, COMMODITY_CRYSTAL_ORE, 2, 3);
    changed |= catalog_place_hopper(st, COMMODITY_CUPRITE_ORE, 2, 4);
    changed |= catalog_place_module(st, MODULE_TRACTOR_FAB, 2, 5);

    changed |= catalog_place_hopper(st, COMMODITY_LASER_MODULE, 3, 2);
    changed |= catalog_place_hopper(st, COMMODITY_FRAME, 3, 3);
    changed |= catalog_place_hopper(st, COMMODITY_CRYSTAL_INGOT, 3, 5);
    changed |= catalog_place_hopper(st, COMMODITY_TRACTOR_MODULE, 3, 7);
    changed |= catalog_normalize_helios_furnaces(st);

    if (changed) rebuild_station_services(st);
    return changed;
}

/* ================================================================== */
/* Save                                                                */
/* ================================================================== */

static bool station_catalog_write_payload(const station_t *st, FILE *f) {
    /* Header */
    { uint32_t magic = CATALOG_MAGIC; WRITE_FIELD(f, magic); }
    { uint32_t ver   = CATALOG_VERSION; WRITE_FIELD(f, ver); }

    /* Identity fields */
    WRITE_FIELD(f, st->id);
    WRITE_FIELD(f, st->name);
    WRITE_FIELD(f, st->pos);
    WRITE_FIELD(f, st->radius);
    WRITE_FIELD(f, st->dock_radius);
    WRITE_FIELD(f, st->signal_range);
    WRITE_FIELD(f, st->signal_connected);
    WRITE_FIELD(f, st->base_price);
    WRITE_FIELD(f, st->services);

    /* Modules — identity only (type, ring, slot); skip scaffold/build_progress */
    WRITE_FIELD(f, st->module_count);
    for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
        WRITE_FIELD(f, st->modules[m].type);
        WRITE_FIELD(f, st->modules[m].ring);
        WRITE_FIELD(f, st->modules[m].slot);
        WRITE_FIELD(f, st->modules[m].commodity); /* v3: hopper commodity tag */
    }

    /* Ring geometry */
    WRITE_FIELD(f, st->arm_count);
    for (int a = 0; a < MAX_ARMS; a++) {
        WRITE_FIELD(f, st->ring_offset[a]);
    }

    /* Hail message + slug */
    WRITE_FIELD(f, st->hail_message);
    WRITE_FIELD(f, st->miner_chatter);
    WRITE_FIELD(f, st->hauler_chatter);
    WRITE_FIELD(f, st->rati_hail_message);
    WRITE_FIELD(f, st->station_slug);

    return true;
}

bool station_catalog_save(const station_t *st, int index, const char *dir) {
    if (!station_exists(st) || !dir) return false;

    ensure_dir(dir);

    char tmp_path[256], final_path[256];
    int final_n = snprintf(final_path, sizeof(final_path), "%s/%d.cat", dir, index);
    int tmp_n = snprintf(tmp_path, sizeof(tmp_path), "%s/%d.cat.tmp", dir, index);
    if (final_n <= 0 || (size_t)final_n >= sizeof(final_path) ||
        tmp_n <= 0 || (size_t)tmp_n >= sizeof(tmp_path)) return false;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    bool ok = station_catalog_write_payload(st, f);
    if (ok) ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

    /* CRC32 trailer — close and reopen to ensure all data is on disk */
    FILE *rf = fopen(tmp_path, "rb");
    if (!rf) { remove(tmp_path); return false; }
    uint32_t crc = crc32_file(rf);
    ok = !ferror(rf);
    if (fclose(rf) != 0) ok = false;
    if (!ok) { remove(tmp_path); return false; }

    FILE *af = fopen(tmp_path, "ab");
    if (!af) { remove(tmp_path); return false; }
    ok = fwrite(&crc, sizeof(crc), 1, af) == 1 &&
         catalog_flush_durable(af);
    if (fclose(af) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

    if (!catalog_replace_file(tmp_path, final_path)) {
        remove(tmp_path);
        return false;
    }
    return true;
}

/* ================================================================== */
/* Load                                                                */
/* ================================================================== */

static bool station_catalog_load_one(station_t *st, int index, const char *dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%d.cat", dir, index);

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Zero the entire struct so session-tier fields start clean */
    station_cleanup(st);
    memset(st, 0, sizeof(*st));
    if (!station_manifest_bootstrap(st)) {
        fclose(f);
        return false;
    }

    /* Read and verify header */
    uint32_t magic, ver;
    READ_FIELD(f, magic);
    READ_FIELD(f, ver);
    if (magic != CATALOG_MAGIC || ver < 1 || ver > CATALOG_VERSION) {
        fclose(f);
        return false;
    }

    /* Identity fields */
    if (ver >= 2) {
        READ_FIELD(f, st->id);
    } else {
        st->id = 0; /* v1 catalogs have no ID — assigned on next save */
    }
    READ_FIELD(f, st->name);
    READ_FIELD(f, st->pos);
    READ_FIELD(f, st->radius);
    READ_FIELD(f, st->dock_radius);
    READ_FIELD(f, st->signal_range);
    READ_FIELD(f, st->signal_connected);
    READ_FIELD(f, st->base_price);
    READ_FIELD(f, st->services);

    /* Modules */
    READ_FIELD(f, st->module_count);
    if (st->module_count < 0) st->module_count = 0;
    if (st->module_count > MAX_MODULES_PER_STATION) st->module_count = MAX_MODULES_PER_STATION;
    for (int m = 0; m < st->module_count; m++) {
        READ_FIELD(f, st->modules[m].type);
        READ_FIELD(f, st->modules[m].ring);
        READ_FIELD(f, st->modules[m].slot);
        if (ver >= 3) {
            READ_FIELD(f, st->modules[m].commodity);
        } else {
            st->modules[m].commodity = (uint8_t)COMMODITY_COUNT;
        }
        st->modules[m].scaffold = false;
        st->modules[m].build_progress = 1.0f; /* loaded modules are complete */
    }

    /* Ring geometry */
    READ_FIELD(f, st->arm_count);
    if (st->arm_count < 0) st->arm_count = 0;
    if (st->arm_count > MAX_ARMS) st->arm_count = MAX_ARMS;
    for (int a = 0; a < MAX_ARMS; a++) {
        READ_FIELD(f, st->ring_offset[a]);
    }

    /* Hail message + slug */
    READ_FIELD(f, st->hail_message);
    if (ver >= 6) {
        READ_FIELD(f, st->miner_chatter);
        READ_FIELD(f, st->hauler_chatter);
        READ_FIELD(f, st->rati_hail_message);
    }
    READ_FIELD(f, st->station_slug);

    /* Verify CRC32 trailer */
    long payload_end = ftell(f);
    uint32_t stored_crc;
    READ_FIELD(f, stored_crc);

    /* Compute CRC over everything before the trailer */
    fseek(f, 0, SEEK_SET);
    uint32_t crc = 0;
    uint8_t chunk[4096];
    long remaining = payload_end;
    while (remaining > 0) {
        size_t to_read = (remaining < (long)sizeof(chunk)) ? (size_t)remaining : sizeof(chunk);
        size_t n = fread(chunk, 1, to_read, f);
        if (n == 0) break;
        crc = crc32_update(crc, chunk, n);
        remaining -= (long)n;
    }
    fclose(f);

    if (crc != stored_crc) {
        printf("[catalog] CRC mismatch for %s — skipping\n", path);
        station_cleanup(st);
        memset(st, 0, sizeof(*st));
        (void)station_manifest_bootstrap(st);
        return false;
    }

    if (station_catalog_migrate_v5_helios(st, index, ver)) {
        printf("[catalog] migrated station %d to v5 Helios smelter layout\n", index);
    }
    if (station_catalog_migrate_v7_helios(st, index, ver)) {
        printf("[catalog] migrated station %d to v7 Helios crystal layout\n", index);
    }

    /* Rebuild service flags from module list */
    rebuild_station_services(st);

    return true;
}

int station_catalog_load_all(station_t *stations, int max_stations, const char *dir) {
    int loaded = 0;
    for (int i = 0; i < max_stations; i++) {
        station_t staged = {0};
        if (station_catalog_load_one(&staged, i, dir)) {
            if (station_copy(&stations[i], &staged))
                loaded++;
        } else {
            /* Keep the caller's already-seeded fallback station intact. A
             * corrupt per-station catalog should not erase relay geometry. */
            station_cleanup(&staged);
            continue;
        }
        station_cleanup(&staged);
    }
    return loaded;
}

/* ================================================================== */
/* Save all                                                            */
/* ================================================================== */

bool station_catalog_save_all(const station_t *stations, int count, const char *dir) {
    bool ok = true;
    for (int i = 0; i < count; i++) {
        if (station_exists(&stations[i])) {
            if (!station_catalog_save(&stations[i], i, dir))
                ok = false;
        }
    }
    return ok;
}
