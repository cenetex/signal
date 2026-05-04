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
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* ---- CRC32 (IEEE 802.3, same implementation as sim_save.c) ---- */
static uint32_t crc32_update(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            /* MSVC C4146: see matching comment in sim_save.c. */
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
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

#define CATALOG_MAGIC   0x53544E43  /* "STNC" */
#define CATALOG_VERSION 4  /* v4: repair Helios seed to include shipyard + frame hopper.
                            * v3: per-module commodity tag (hopper specialization). */

/* ---- helper macros (same pattern as sim_save.c) ---- */
#define WRITE_FIELD(f, val) do { if (fwrite(&(val), sizeof(val), 1, (f)) != 1) { fclose(f); return false; } } while(0)
#define READ_FIELD(f, val)  do { if (fread(&(val), sizeof(val), 1, (f)) != 1)  { fclose(f); return false; } } while(0)

/* ---- directory helper ---- */
static void ensure_dir(const char *dir) {
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

static bool catalog_slot_free(const station_t *st, int ring, int slot) {
    if (!st || ring < 1 || ring > STATION_NUM_RINGS) return false;
    if (slot < 0 || slot >= STATION_RING_SLOTS[ring]) return false;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].ring == ring && st->modules[i].slot == slot)
            return false;
    }
    return true;
}

static bool catalog_add_module_prefer(station_t *st, module_type_t type,
                                      int preferred_ring, int preferred_slot) {
    if (!st || st->module_count >= MAX_MODULES_PER_STATION) return false;
    if (catalog_slot_free(st, preferred_ring, preferred_slot)) {
        add_module_at(st, type, (uint8_t)preferred_ring, (uint8_t)preferred_slot);
        return true;
    }
    for (int r = 2; r <= STATION_NUM_RINGS; r++) {
        int slot = station_ring_free_slot(st, r, STATION_RING_SLOTS[r]);
        if (slot >= 0) {
            add_module_at(st, type, (uint8_t)r, (uint8_t)slot);
            return true;
        }
    }
    return false;
}

static bool catalog_add_hopper_prefer(station_t *st, commodity_t c,
                                      int preferred_ring, int preferred_slot) {
    if (!st || st->module_count >= MAX_MODULES_PER_STATION) return false;
    if (catalog_slot_free(st, preferred_ring, preferred_slot)) {
        add_hopper_for(st, (uint8_t)preferred_ring, (uint8_t)preferred_slot, c);
        return true;
    }
    for (int r = 2; r <= STATION_NUM_RINGS; r++) {
        int slot = station_ring_free_slot(st, r, STATION_RING_SLOTS[r]);
        if (slot >= 0) {
            add_hopper_for(st, (uint8_t)r, (uint8_t)slot, c);
            return true;
        }
    }
    for (int r = 1; r <= STATION_NUM_RINGS; r++) {
        int slot = station_ring_free_slot(st, r, STATION_RING_SLOTS[r]);
        if (slot >= 0) {
            add_hopper_for(st, (uint8_t)r, (uint8_t)slot, c);
            return true;
        }
    }
    return false;
}

static bool station_catalog_migrate_v4_helios(station_t *st, int index, uint32_t ver) {
    if (!st || index != 2 || ver >= 4) return false;
    bool changed = false;
    if (!station_has_module(st, MODULE_SHIPYARD)) {
        changed |= catalog_add_module_prefer(st, MODULE_SHIPYARD, 2, 2);
    }
    if (station_find_hopper_for(st, COMMODITY_FRAME) < 0) {
        changed |= catalog_add_hopper_prefer(st, COMMODITY_FRAME, 3, 3);
    }
    if (changed) rebuild_station_services(st);
    return changed;
}

/* ================================================================== */
/* Save                                                                */
/* ================================================================== */

bool station_catalog_save(const station_t *st, int index, const char *dir) {
    if (!station_exists(st)) return false;

    ensure_dir(dir);

    /* Write to temp file, then rename for atomicity */
    char tmp_path[256], final_path[256];
    snprintf(final_path, sizeof(final_path), "%s/%d.cat", dir, index);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%d.cat.tmp", dir, index);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;

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
    WRITE_FIELD(f, st->station_slug);

    /* CRC32 trailer — close and reopen to ensure all data is on disk */
    fclose(f);
    {
        FILE *rf = fopen(tmp_path, "rb");
        if (!rf) { remove(tmp_path); return false; }
        uint32_t crc = crc32_file(rf);
        fclose(rf);
        FILE *af = fopen(tmp_path, "ab");
        if (!af) { remove(tmp_path); return false; }
        fwrite(&crc, sizeof(crc), 1, af);
        fclose(af);
    }
    /* Atomic rename */
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
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

    if (station_catalog_migrate_v4_helios(st, index, ver)) {
        printf("[catalog] migrated station %d to v4 Helios shipyard layout\n", index);
    }

    /* Rebuild service flags from module list */
    rebuild_station_services(st);

    return true;
}

int station_catalog_load_all(station_t *stations, int max_stations, const char *dir) {
    int loaded = 0;
    for (int i = 0; i < max_stations; i++) {
        if (station_catalog_load_one(&stations[i], i, dir)) {
            loaded++;
        }
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
