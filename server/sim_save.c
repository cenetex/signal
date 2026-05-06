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
 * v27: active fracture children return as a counted sidecar section.
 *   - Only already-fractured children persist; terrain asteroids remain
 *     derived from the belt seed.
 *   - Open fracture claim windows and resolved fragment provenance survive
 *     crashes/restarts.
 */
#include "game_sim.h"
#include "manifest.h"
#include "ship.h"
#include "sim_ai.h"
#include "sim_construction.h"
#include "base58.h"
#include "protocol.h"
#include "station_authority.h"
#include "chain_log.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir_700(p) _mkdir(p)
#else
#include <unistd.h>
#include <dirent.h>
#define mkdir_700(p) mkdir((p), 0700)
#endif

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
            /* MSVC C4146: unary minus on unsigned — use (0u - x) form
             * to spell out the two's-complement intent explicitly. */
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

#define SAVE_MAGIC 0x5349474E  /* "SIGN" */
#define SAVE_VERSION 52  /* NPC hauler manifest tail:
                          * appends a fixed per-NPC paired-ship manifest
                          * payload after the pubkey registry tail and
                          * before CRC2. This preserves real cargo_unit_t
                          * identity for manifest-backed NPC hauler cargo
                          * across save/load, instead of degrading
                          * in-flight loads back to synthetic legacy units.
                          * v51 was cargo-in-space schema (Slice 1):
                          * FURNACE per-instance commodity tag drives
                          * its output ingot type. v50 saves loaded
                          * with untagged furnaces (commodity ==
                          * COMMODITY_COUNT) get tagged on load by a
                          * station-furnace-count heuristic — 1 furnace
                          * → FERRITE_INGOT, 2 → 1×FERRITE+1×CUPRITE,
                          * 3+ → CUPRITE+CRYSTAL split — and missing
                          * output hoppers are auto-spawned into free
                          * outer-ring slots. Layout-preserving on
                          * disk. v50 was: Hoppers tag a single commodity each, the
                          * pair rule becomes "all required input
                          * commodities have a hopper on the station,"
                          * and producers emit one spoke per input
                          * commodity. station_module_t grows from 12
                          * to 16 bytes (commodity + pad). Catalog
                          * version bumped 2 → 3 to write the new
                          * byte. v48 spoke + drag dynamics:
                          * Ring 1 is now spine-only (DOCK + RELAY +
                          * REPAIR_BAY); producers (FURNACE / FRAME_PRESS
                          * / LASER_FAB / TRACTOR_FAB / SHIPYARD) are
                          * banned on ring 1 and require a paired
                          * intake module (HOPPER) at the canonical
                          * 180°-opposite slot on the same ring.
                          * Prospect/Kepler/Helios re-seeded under the
                          * new rule. Save layout itself is unchanged
                          * vs v46 — but the seeded module set is not,
                          * so MIN_SAVE_VERSION moves to 47 to wipe
                          * pre-rule worlds rather than try to migrate
                          * them in place. v46 (#257):
                          * Ledger entries keyed by player_pubkey[32] instead
                          * of player_token[8]. Adds first_dock_tick,
                          * last_dock_tick, total_docks, lifetime_ore_units,
                          * lifetime_credits_in/out, and top_commodity per
                          * relationship. v45 saves: migrate session_token →
                          * pubkey where available (zero pubkey→zero ledger).
                          * v45: cargo_unit_t._pad repurposed as quantity (u8).
                          * v44 saves wrote zero into _pad on every unit;
                          * loaders rewrite quantity == 0 → 1 so existing
                          * named units stay individually addressable.
                          * Foundation for the upcoming raw-ore-as-crate
                          * migration; no production path emits ore-kind
                          * units yet. The cargo_unit_t binary size is
                          * unchanged (still 80 bytes), so PLY7 / chain-log
                          * payloads stay byte-compatible.
                          * v46 (#257): Station-player relationship data.
                          * v44: MODULE_ORE_SILO (= 8) and MODULE_CARGO_BAY (= 10)
                          * dropped; both remapped to MODULE_HOPPER (= 1)
                          * on load. The hopper now serves as the unified
                          * ore-intake-and-storage module. v43 saves load
                          * with the remap applied automatically.
                          * v43: credit_pool field eliminated — pool is now derived
                          * from -Σ(ledger.balance) via station_credit_pool().
                          * world.sav drops 4 bytes per existing station in
                          * write_station_session; v42 saves still load (the
                          * stored value is read into a discard then ignored —
                          * the value is recomputable from the loaded ledger).
                          * v42 (Layer D #479): per-ship cargo_receipt_t
                          * chains persisted alongside the ship manifest
                          * tail in each player save (PLY7).
                          * v41 (Layer C): per-station chain log state
                          * (chain_last_hash 32B + chain_event_count 8B). */
/* v40: Layer B of #479 — per-station Ed25519 pubkey + outpost
 * provenance tail in the session block. The matching private key is
 * rederivable from the world seed (or saved provenance, for outposts)
 * and is deliberately NOT written to disk.
 * v39: Layer A.3 — per-player last_signed_nonce in the player save
 * (PLY6); world.sav layout itself was unchanged.
 * v38 added destroyed_rocks destroyed_at_ms timestamps (#285 slice 2). */
/* v31 widened inventory[] / base_price[] by one slot (REPAIR_KIT). v32
 * appends npc_ship_t.hull (a single float, version-gated read so v31
 * saves still load with default hull). MIN stays at 31 so we don't
 * wipe v31 worlds on this bump.
 *
 * v35: dropped station.named_ingots[64] + named_ingots_count + the
 * named_ingot_t struct. Old saves are migrated by reading the legacy
 * named-ingot block (52B per slot, fixed layout) and converting each
 * non-empty entry into a manifest unit. */
/* Bumped to 49: per-hopper commodity tag changes station_module_t
 * shape. Catalog files persist modules so they need re-bootstrap.
 * Per-player saves under saves/pubkey/ live in their own files and
 * are unaffected. */
#define MIN_SAVE_VERSION 49 /* v49 → v50 is layout-preserving (npc fields
                              * moved into embedded ship_t at the same
                              * byte offsets); read_npc lands them in
                              * n->ship.* identically for both versions. */

/* Legacy named-ingot block layout — preserved here only so v25..v34
 * saves can be migrated forward. The original named_ingot_t was
 * field-by-field WRITE_FIELD'd, so the on-disk record matches the
 * struct's natural layout with 8-byte alignment for mined_block. That
 * came out to 56 bytes per record (the 52-byte WIRE record packs
 * tighter; only the disk used the natural padding). */
typedef struct {
    uint8_t  pubkey[32];      /* 0..31 */
    uint8_t  prefix_class;    /* 32 */
    uint8_t  metal;           /* 33 */
    uint8_t  _pad[2];         /* 34..35 */
    /* compiler inserts 4 bytes here to 8-align mined_block */
    uint64_t mined_block;     /* 40..47 */
    uint8_t  origin_station;  /* 48 */
    uint8_t  _pad2[7];        /* 49..55 */
} legacy_named_ingot_t;
_Static_assert(sizeof(legacy_named_ingot_t) == 56,
               "legacy_named_ingot_t must match the on-disk v34 layout");
#define LEGACY_STATION_NAMED_INGOTS_MAX 64
#define LEGACY_SHIP_HOLD_INGOTS_MAX     8

/* Set by world_load() before read_station() so per-station readers know
 * which version they're parsing and can handle field additions. */
static int g_loaded_save_version = SAVE_VERSION;

/* Current hauler capacity is 40 ingots; keep the on-disk corruption
 * guard comfortably above that while avoiding hostile giant receipt
 * allocations from malformed saves. */
#define NPC_SHIP_MANIFEST_SAVE_MAX 512u

/* v51 cargo-in-space schema migration (Slice 1):
 * - Tag every untagged FURNACE (commodity == COMMODITY_COUNT) with an
 *   output ingot using a station-furnace-count heuristic that matches
 *   the existing count-tier smelt rules:
 *     1 furnace → FERRITE_INGOT
 *     2 furnaces → 1× FERRITE + 1× CUPRITE
 *     3+ furnaces → 1× CUPRITE + 1× CRYSTAL + rest CUPRITE
 * - Auto-spawn missing output hoppers in free outer-ring slots so the
 *   seeded layout invariant (every producer has a tagged output
 *   hopper) holds for migrated saves too.
 *
 * Idempotent — running it again on an already-migrated world is a
 * no-op (tagged furnaces are skipped; existing output hoppers
 * satisfy the search).
 */
static bool cargo_schema_live_furnace(const station_module_t *mod) {
    return mod->type == MODULE_FURNACE && !mod->scaffold;
}

static bool cargo_schema_ingot_furnace_tag(commodity_t c) {
    return c == COMMODITY_FERRITE_INGOT ||
           c == COMMODITY_CUPRITE_INGOT ||
           c == COMMODITY_CRYSTAL_INGOT;
}

static int cargo_schema_live_furnace_count(const station_t *st) {
    int n_furnaces = 0;
    for (int m = 0; m < st->module_count; m++) {
        if (cargo_schema_live_furnace(&st->modules[m])) n_furnaces++;
    }
    return n_furnaces;
}

static commodity_t cargo_schema_furnace_tag(int n_furnaces, int seen) {
    if (n_furnaces >= 3) {
        return seen == 1 ? COMMODITY_CRYSTAL_INGOT
                         : COMMODITY_CUPRITE_INGOT;
    }
    if (n_furnaces == 2) {
        return seen == 0 ? COMMODITY_FERRITE_INGOT
                         : COMMODITY_CUPRITE_INGOT;
    }
    return COMMODITY_FERRITE_INGOT;
}

static void cargo_schema_tag_furnaces(station_t *st) {
    int n_furnaces = cargo_schema_live_furnace_count(st);
    int seen = 0;
    for (int m = 0; m < st->module_count; m++) {
        station_module_t *mod = &st->modules[m];
        if (!cargo_schema_live_furnace(mod)) continue;
        if (cargo_schema_ingot_furnace_tag((commodity_t)mod->commodity)) {
            seen++;
            continue;
        }
        mod->commodity = (uint8_t)cargo_schema_furnace_tag(n_furnaces, seen);
        seen++;
    }
}

static bool cargo_schema_find_hopper_slot(const station_t *st,
                                          int *out_ring,
                                          int *out_slot) {
    for (int r = 2; r <= STATION_NUM_RINGS; r++) {
        int slot = station_ring_free_slot(st, r, STATION_RING_SLOTS[r]);
        if (slot < 0) continue;
        *out_ring = r;
        *out_slot = slot;
        return true;
    }

    int slot = station_ring_free_slot(st, 1, STATION_RING_SLOTS[1]);
    if (slot < 0) return false;
    *out_ring = 1;
    *out_slot = slot;
    return true;
}

static void cargo_schema_add_missing_hoppers(station_t *st) {
    /* Snapshot module_count to avoid iterating into freshly-added hoppers. */
    int snap = st->module_count;
    for (int m = 0; m < snap; m++) {
        const station_module_t *mod = &st->modules[m];
        if (mod->scaffold) continue;
        if (!module_is_producer(mod->type)) continue;
        commodity_t out = module_instance_output(mod);
        if (out == COMMODITY_COUNT) continue; /* shipyard etc. exempt */
        if (station_find_hopper_for(st, out) >= 0) continue;
        int placed_ring = -1;
        int placed_slot = -1;
        if (!cargo_schema_find_hopper_slot(st, &placed_ring, &placed_slot))
            continue;
        add_hopper_for(st, (uint8_t)placed_ring, (uint8_t)placed_slot, out);
    }
}

/* Exposed (non-static) so tests can break a fresh world to look
 * pre-Slice-1 and exercise this directly. */
void world_apply_cargo_schema_migration(world_t *w) {
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        if (st->module_count <= 0) continue;
        cargo_schema_tag_furnaces(st);
        cargo_schema_add_missing_hoppers(st);
        rebuild_station_services(st);
    }
}

/* ---- helper macros for explicit field I/O ---- */
#define WRITE_FIELD(f, val) do { if (fwrite(&(val), sizeof(val), 1, (f)) != 1) { fclose(f); return false; } } while(0)
#define READ_FIELD(f, val)  do { if (fread(&(val), sizeof(val), 1, (f)) != 1)  { fclose(f); return false; } } while(0)

/* ---- station field-by-field I/O ---- */
/* write_station removed in v24 — station identity now persisted via
 * sim_catalog.c; session-tier data via write_station_session(). The
 * read_station() below is kept for loading v23 saves. */

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
    READ_FIELD(f, s->_inventory_cache);
    READ_FIELD(f, s->services);
    /* Modules */
    READ_FIELD(f, s->module_count);
    if (s->module_count < 0) s->module_count = 0;
    if (s->module_count > MAX_MODULES_PER_STATION) s->module_count = MAX_MODULES_PER_STATION;
    for (int m = 0; m < s->module_count; m++) {
        READ_FIELD(f, s->modules[m]);
        /* Sanitize bool — old saves may have non-0/1 byte values, and
         * reading those as _Bool is undefined behavior. Read the raw
         * byte to avoid UB on the load itself. */
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
    /* credit_pool field was stored in v23..v42 but is derived now (#refactor).
     * Read and discard for older saves; v43+ doesn't include it on disk. */
    if (g_loaded_save_version >= 23 && g_loaded_save_version <= 42) {
        float discard;
        READ_FIELD(f, discard);
        (void)discard;
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
    WRITE_FIELD(f, s->_inventory_cache);
    /* Per-module production buffers */
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        WRITE_FIELD(f, s->module_input[m]);
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        WRITE_FIELD(f, s->module_output[m]);
    /* (credit_pool field removed in v43 — derived from ledger now.) */
    /* Economy ledger */
    WRITE_FIELD(f, s->ledger_count);
    for (int i = 0; i < 16; i++) {
        WRITE_FIELD(f, s->ledger[i].player_pubkey);
        WRITE_FIELD(f, s->ledger[i].balance);
        WRITE_FIELD(f, s->ledger[i].lifetime_supply);
        WRITE_FIELD(f, s->ledger[i].first_dock_tick);
        WRITE_FIELD(f, s->ledger[i].last_dock_tick);
        WRITE_FIELD(f, s->ledger[i].total_docks);
        WRITE_FIELD(f, s->ledger[i].lifetime_ore_units);
        WRITE_FIELD(f, s->ledger[i].lifetime_credits_in);
        WRITE_FIELD(f, s->ledger[i].lifetime_credits_out);
        WRITE_FIELD(f, s->ledger[i].top_commodity);
        WRITE_FIELD(f, s->ledger[i]._pad);
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
    /* Live rotation angles and speeds. arm_omega is the passive-ring
     * angular velocity state for the spoke + drag dynamics (v48+);
     * driver rings ignore it, but persisting it keeps loaded saves
     * from briefly transient-spinning back up. */
    for (int a = 0; a < MAX_ARMS; a++) {
        WRITE_FIELD(f, s->arm_rotation[a]);
        WRITE_FIELD(f, s->arm_speed[a]);
        WRITE_FIELD(f, s->arm_omega[a]);
    }
    /* v35: named-ingot stockpile collapsed into manifest. The
     * v25..v34 dual-store fields (count + 64 × named_ingot_t) are no
     * longer written. Migration on load converts old saves forward. */
    /* Manifest (v29+, #339 slice A). Previously guarded to require
     * empty — now serialized as count + packed cargo_unit_t entries.
     * cap is NOT persisted; on load the manifest bootstraps at the
     * default capacity and grows as needed. */
    {
        uint16_t manifest_count = s->manifest.count;
        WRITE_FIELD(f, manifest_count);
        for (uint16_t u = 0; u < manifest_count; u++)
            WRITE_FIELD(f, s->manifest.units[u]);
    }
    /* v40: per-station Ed25519 pubkey (#479 B) + outpost provenance
     * (founder pubkey + name + planted tick) so the matching private
     * key is rederivable on load without ever being persisted. The
     * 64-byte station_secret is deliberately omitted. The name is
     * written here (in addition to the catalog) so outpost identity
     * stays self-contained in world.sav — saves loaded without the
     * matching catalog still rederive a working keypair. */
    if (fwrite(s->station_pubkey, 32, 1, f) != 1) { fclose(f); return false; }
    if (fwrite(s->outpost_founder_pubkey, 32, 1, f) != 1) { fclose(f); return false; }
    WRITE_FIELD(f, s->outpost_planted_tick);
    WRITE_FIELD(f, s->name);
    /* v41: Layer C of #479 — chain log state. The actual events live in
     * side files under chain/<base58(pubkey)>.log; only the
     * continuation pointers (last full-record hash + monotonic event
     * counter) ride along with the world save so a restart can pick
     * up the chain without re-reading + re-hashing the entire log. */
    if (fwrite(s->chain_last_hash, 32, 1, f) != 1) { fclose(f); return false; }
    WRITE_FIELD(f, s->chain_event_count);
    return true;
}

static bool read_station_session(FILE *f, station_t *s) {
    /* Inventory */
    READ_FIELD(f, s->_inventory_cache);
    /* Per-module production buffers */
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        READ_FIELD(f, s->module_input[m]);
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
        READ_FIELD(f, s->module_output[m]);
    /* credit_pool was stored v23..v42; dropped in v43 (derived field).
     * For older saves, read and discard; the value is recoverable from
     * the ledger entries below. */
    if (g_loaded_save_version >= 23 && g_loaded_save_version <= 42) {
        float discard;
        READ_FIELD(f, discard);
        (void)discard;
    }
    /* Economy ledger */
    READ_FIELD(f, s->ledger_count);
    if (s->ledger_count < 0) s->ledger_count = 0;
    if (s->ledger_count > 16) s->ledger_count = 16;
    for (int i = 0; i < 16; i++) {
        if (g_loaded_save_version >= 46) {
            /* v46+: ledger keyed by player_pubkey with relationship data */
            READ_FIELD(f, s->ledger[i].player_pubkey);
            READ_FIELD(f, s->ledger[i].balance);
            READ_FIELD(f, s->ledger[i].lifetime_supply);
            READ_FIELD(f, s->ledger[i].first_dock_tick);
            READ_FIELD(f, s->ledger[i].last_dock_tick);
            READ_FIELD(f, s->ledger[i].total_docks);
            READ_FIELD(f, s->ledger[i].lifetime_ore_units);
            READ_FIELD(f, s->ledger[i].lifetime_credits_in);
            READ_FIELD(f, s->ledger[i].lifetime_credits_out);
            READ_FIELD(f, s->ledger[i].top_commodity);
            READ_FIELD(f, s->ledger[i]._pad);
        } else {
            /* v45 migration: session_token → pubkey (stay zero), initialize
             * relationship fields. The ledger still lives across versions, so
             * we don't lose balance/lifetime_supply. */
            uint8_t player_token[8];
            READ_FIELD(f, player_token);
            memset(s->ledger[i].player_pubkey, 0, 32);
            READ_FIELD(f, s->ledger[i].balance);
            READ_FIELD(f, s->ledger[i].lifetime_supply);
            s->ledger[i].first_dock_tick = 0;
            s->ledger[i].last_dock_tick = 0;
            s->ledger[i].total_docks = 0;
            s->ledger[i].lifetime_ore_units = 0;
            s->ledger[i].lifetime_credits_in = 0;
            s->ledger[i].lifetime_credits_out = 0;
            s->ledger[i].top_commodity = 0;
            memset(s->ledger[i]._pad, 0, 3);
        }
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
    /* Live rotation angles and speeds (v48+ also persists arm_omega). */
    for (int a = 0; a < MAX_ARMS; a++) {
        READ_FIELD(f, s->arm_rotation[a]);
        READ_FIELD(f, s->arm_speed[a]);
        READ_FIELD(f, s->arm_omega[a]);
    }
    /* v26..v34 wrote a named-ingot stockpile block (count + 64
     * fixed-size records). v35 dropped the dual store; the manifest
     * is now the single source of truth. We still read the legacy
     * block off disk so subsequent fields stay byte-aligned, but its
     * contents are migrated into the manifest only when the file
     * predates the manifest (v26..v28); for v29..v34 the manifest
     * already contains the same units (dual-write at smelt time) and
     * the legacy block is discarded to avoid double-counting. */
    if (g_loaded_save_version >= 26 && g_loaded_save_version <= 34) {
        int legacy_count = 0;
        legacy_named_ingot_t legacy[LEGACY_STATION_NAMED_INGOTS_MAX];
        READ_FIELD(f, legacy_count);
        for (int i = 0; i < LEGACY_STATION_NAMED_INGOTS_MAX; i++)
            READ_FIELD(f, legacy[i]);
        if (g_loaded_save_version < 29) {
            /* No manifest in the file; lift the named records into the
             * manifest as smelt-recipe units so the trade picker sees
             * them after the world reset. parent_merkle is unknown for
             * legacy entries — leave it zero. */
            if (!station_manifest_bootstrap(s)) return false;
            if (legacy_count < 0) legacy_count = 0;
            if (legacy_count > LEGACY_STATION_NAMED_INGOTS_MAX)
                legacy_count = LEGACY_STATION_NAMED_INGOTS_MAX;
            for (int i = 0; i < legacy_count; i++) {
                const legacy_named_ingot_t *src = &legacy[i];
                /* Empty slots in the legacy array were zero-initialized
                 * (pubkey all zero); skip those. */
                static const uint8_t zero_pk[32] = {0};
                if (memcmp(src->pubkey, zero_pk, 32) == 0) continue;
                if (s->manifest.count >= s->manifest.cap) break;
                cargo_unit_t u = {0};
                u.kind = (uint8_t)CARGO_KIND_INGOT;
                u.commodity = src->metal;
                u.grade = (uint8_t)MINING_GRADE_COMMON;
                u.prefix_class = src->prefix_class;
                u.recipe_id = (uint16_t)RECIPE_SMELT;
                u.origin_station = src->origin_station;
                u.quantity = 1;
                u.mined_block = src->mined_block;
                memcpy(u.pub, src->pubkey, 32);
                (void)manifest_push(&s->manifest, &u);
            }
        }
        (void)legacy; /* silence unused warning when nothing is migrated */
    }
    /* Manifest (v29+). For v29+, allocate via bootstrap + reserve, then
     * read entries. Pre-v29 saves get Slice D migration: their float
     * inventory becomes synthetic RECIPE_LEGACY_MIGRATE units so the
     * manifest layer sees a consistent state from tick 0. */
    if (!station_manifest_bootstrap(s)) return false;
    if (g_loaded_save_version >= 29) {
        uint16_t manifest_count = 0;
        READ_FIELD(f, manifest_count);
        if (manifest_count > 0) {
            if (!manifest_reserve(&s->manifest, manifest_count)) return false;
            for (uint16_t u = 0; u < manifest_count; u++)
                READ_FIELD(f, s->manifest.units[u]);
            s->manifest.count = manifest_count;
            /* v45 repurposed cargo_unit_t._pad as quantity. v44 and earlier
             * wrote zero there; rewrite to 1 so units stay addressable. */
            if (g_loaded_save_version < 45)
                manifest_migrate_quantity(&s->manifest);
        }
    } else {
        /* Slice D: synthesize manifest entries from float inventory for
         * pre-v29 saves. Origin salt = first 8 chars of station.name so
         * the same save reloads to the same pubs deterministically. */
        uint8_t origin[8] = {0};
        memcpy(origin, s->name, sizeof(origin));
        (void)manifest_migrate_legacy_inventory(&s->manifest, s->_inventory_cache,
                                                COMMODITY_COUNT, origin);
    }
    /* v40: per-station Ed25519 pubkey + outpost provenance (#479 B).
     * v39 and earlier saves don't carry these fields — leave them
     * zeroed and let the world loader rederive both pubkey and secret
     * from world seed (seeded stations) or zero-founder fallback
     * (outposts; v39-era outposts accept a slight provenance gap). */
    if (g_loaded_save_version >= 40) {
        if (fread(s->station_pubkey, 32, 1, f) != 1) return false;
        if (fread(s->outpost_founder_pubkey, 32, 1, f) != 1) return false;
        READ_FIELD(f, s->outpost_planted_tick);
        /* v40 stamps the station name into the session save too, so
         * outpost rederivation has the name input even when the
         * catalog isn't loaded alongside the world save. The catalog
         * remains the canonical source for seeded stations — but
         * writing the name here is harmless and a load without the
         * catalog still gets a usable name + working keypair. */
        char saved_name[sizeof(s->name)];
        READ_FIELD(f, saved_name);
        if (s->name[0] == '\0')
            memcpy(s->name, saved_name, sizeof(s->name));
    } else {
        memset(s->station_pubkey, 0, sizeof(s->station_pubkey));
        memset(s->outpost_founder_pubkey, 0, sizeof(s->outpost_founder_pubkey));
        s->outpost_planted_tick = 0;
    }
    /* v41: Layer C of #479 — chain log state. v40 and earlier saves
     * don't carry the continuation pointers; treat the chain as fresh
     * on load (the first emit after migration starts a new chain). */
    if (g_loaded_save_version >= 41) {
        if (fread(s->chain_last_hash, 32, 1, f) != 1) return false;
        READ_FIELD(f, s->chain_event_count);
    } else {
        memset(s->chain_last_hash, 0, sizeof(s->chain_last_hash));
        s->chain_event_count = 0;
    }
    /* station_secret is rederived by the world loader, not persisted. */
    memset(s->station_secret, 0, sizeof(s->station_secret));
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

/* ---- fracture-child sidecar I/O (v27+) ---- */
static bool write_fracture_child(FILE *f, uint16_t slot,
                                 const asteroid_t *a,
                                 const fracture_claim_state_t *state) {
    uint8_t claim_flags = 0;
    WRITE_FIELD(f, slot);
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
    WRITE_FIELD(f, a->last_towed_by);
    WRITE_FIELD(f, a->last_fractured_by);
    WRITE_FIELD(f, a->smelt_progress);
    WRITE_FIELD(f, a->last_towed_token);
    WRITE_FIELD(f, a->last_fractured_token);
    WRITE_FIELD(f, a->fracture_seed);
    WRITE_FIELD(f, a->fragment_pub);
    WRITE_FIELD(f, a->grade);
    if (state) {
        if (state->active) claim_flags |= 1u;
        if (state->resolved) claim_flags |= 2u;
    }
    WRITE_FIELD(f, claim_flags);
    if (state) {
        WRITE_FIELD(f, state->fracture_id);
        WRITE_FIELD(f, state->deadline_ms);
        WRITE_FIELD(f, state->burst_cap);
        WRITE_FIELD(f, state->best_nonce);
        WRITE_FIELD(f, state->best_grade);
        WRITE_FIELD(f, state->best_player_pub);
        WRITE_FIELD(f, state->seen_claimant_count);
        WRITE_FIELD(f, state->_pad1);
        WRITE_FIELD(f, state->seen_claimant_tokens);
    } else {
        uint32_t zero32 = 0;
        uint16_t zero16 = 0;
        uint8_t zero8 = 0;
        uint8_t zero_pad[3] = {0};
        uint8_t zero_pub[32] = {0};
        uint8_t zero_tokens[MAX_PLAYERS][8] = {{0}};
        WRITE_FIELD(f, zero32);
        WRITE_FIELD(f, zero32);
        WRITE_FIELD(f, zero16);
        WRITE_FIELD(f, zero32);
        WRITE_FIELD(f, zero8);
        WRITE_FIELD(f, zero_pub);
        WRITE_FIELD(f, zero8);
        WRITE_FIELD(f, zero_pad);
        WRITE_FIELD(f, zero_tokens);
    }
    return true;
}

static bool read_fracture_child(FILE *f, world_t *w) {
    uint16_t slot;
    uint8_t claim_flags = 0;
    asteroid_t *a;
    fracture_claim_state_t *state;

    READ_FIELD(f, slot);
    if (slot >= MAX_ASTEROIDS) return false;
    a = &w->asteroids[slot];
    state = &w->fracture_claims[slot];
    memset(a, 0, sizeof(*a));
    memset(state, 0, sizeof(*state));
    a->active = true;
    a->fracture_child = true;
    a->net_dirty = true;
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
    READ_FIELD(f, a->last_towed_by);
    READ_FIELD(f, a->last_fractured_by);
    READ_FIELD(f, a->smelt_progress);
    READ_FIELD(f, a->last_towed_token);
    READ_FIELD(f, a->last_fractured_token);
    READ_FIELD(f, a->fracture_seed);
    READ_FIELD(f, a->fragment_pub);
    READ_FIELD(f, a->grade);
    READ_FIELD(f, claim_flags);
    READ_FIELD(f, state->fracture_id);
    READ_FIELD(f, state->deadline_ms);
    READ_FIELD(f, state->burst_cap);
    READ_FIELD(f, state->best_nonce);
    if (g_loaded_save_version >= 28) {
        READ_FIELD(f, state->best_grade);
        READ_FIELD(f, state->best_player_pub);
        READ_FIELD(f, state->seen_claimant_count);
        READ_FIELD(f, state->_pad1);
        if (state->seen_claimant_count > MAX_PLAYERS) return false;
        READ_FIELD(f, state->seen_claimant_tokens);
    } else {
        uint32_t legacy_seen_players_mask;
        int8_t legacy_best_player_id;
        READ_FIELD(f, legacy_seen_players_mask);
        READ_FIELD(f, state->best_grade);
        READ_FIELD(f, legacy_best_player_id);
        READ_FIELD(f, state->best_player_pub);
        (void)legacy_seen_players_mask;
        (void)legacy_best_player_id;
        state->seen_claimant_count = 0;
        memset(state->_pad1, 0, sizeof(state->_pad1));
        memset(state->seen_claimant_tokens, 0, sizeof(state->seen_claimant_tokens));
    }
    state->active = (claim_flags & 1u) != 0;
    state->resolved = (claim_flags & 2u) != 0;
    state->challenge_dirty = state->active;
    state->resolved_dirty = false;
    w->asteroid_origin[slot].chunk_x = 0;
    w->asteroid_origin[slot].chunk_y = 0;
    w->asteroid_origin[slot].from_chunk = false;
    return true;
}

/* ---- npc_ship field-by-field I/O ----
 *
 * v50: pos/vel/angle/hull_class moved into the embedded ship_t. The
 * on-disk record format is unchanged (same field order, same widths) —
 * the read path just lands the bytes in n->ship.* and the write path
 * reads them back from there. v49 saves load identically since the
 * struct layout used to put pos/vel/angle/hull_class right where the
 * embedded ship_t now lives.
 */
static bool write_npc(FILE *f, const npc_ship_t *n) {
    WRITE_FIELD(f, n->active);
    WRITE_FIELD(f, n->role);
    WRITE_FIELD(f, n->ship.hull_class);
    WRITE_FIELD(f, n->state);
    WRITE_FIELD(f, n->ship.pos);
    WRITE_FIELD(f, n->ship.vel);
    WRITE_FIELD(f, n->ship.angle);
    WRITE_FIELD(f, n->cargo);
    WRITE_FIELD(f, n->target_asteroid);
    WRITE_FIELD(f, n->home_station);
    WRITE_FIELD(f, n->dest_station);
    WRITE_FIELD(f, n->state_timer);
    WRITE_FIELD(f, n->thrusting);
    WRITE_FIELD(f, n->tint_r);
    WRITE_FIELD(f, n->tint_g);
    WRITE_FIELD(f, n->tint_b);
    WRITE_FIELD(f, n->hull); /* v32+ */
    WRITE_FIELD(f, n->session_token); /* v33+ */
    return true;
}

static bool read_npc(FILE *f, npc_ship_t *n) {
    READ_FIELD(f, n->active);
    READ_FIELD(f, n->role);
    READ_FIELD(f, n->ship.hull_class);
    READ_FIELD(f, n->state);
    READ_FIELD(f, n->ship.pos);
    READ_FIELD(f, n->ship.vel);
    READ_FIELD(f, n->ship.angle);
    READ_FIELD(f, n->cargo);
    READ_FIELD(f, n->target_asteroid);
    READ_FIELD(f, n->home_station);
    READ_FIELD(f, n->dest_station);
    READ_FIELD(f, n->state_timer);
    READ_FIELD(f, n->thrusting);
    READ_FIELD(f, n->tint_r);
    READ_FIELD(f, n->tint_g);
    READ_FIELD(f, n->tint_b);
    if (g_loaded_save_version >= 32) {
        READ_FIELD(f, n->hull);
    } else {
        n->hull = npc_max_hull(n);
    }
    if (g_loaded_save_version >= 33) {
        READ_FIELD(f, n->session_token);
    } else {
        /* v32 saves predate per-NPC accounts. Zero out so the post-
         * load pass in rebuild_characters_from_npcs can reissue a
         * fresh token via the world-side counter. The NPC starts
         * with no ledger entries; previous deliveries (which never
         * had a token to credit anyway) are not retroactive. */
        memset(n->session_token, 0, sizeof(n->session_token));
    }
    /* Validate after the full record is read so the file pointer is
     * always past this NPC's bytes. An active slot with garbage role
     * used to crashloop the server on first sim step (despawn check
     * fired with an invalid role through character_free_for_npc).
     * Drop the slot quietly; the spawn loop will refill it. */
    if (n->active && ((int)n->role < 0 || (int)n->role > (int)NPC_ROLE_TOW)) {
        memset(n, 0, sizeof(*n));
    }
    return true;
}

static const ship_t *world_save_npc_ship_for(const world_t *w, int npc_slot) {
    if (!w || npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return NULL;
    int cap = (int)(sizeof(w->characters) / sizeof(w->characters[0]));
    for (int i = 0; i < cap; i++) {
        const character_t *ch = &w->characters[i];
        if (!ch->active) continue;
        if (ch->npc_slot != npc_slot) continue;
        if (ch->kind != CHARACTER_KIND_NPC_MINER &&
            ch->kind != CHARACTER_KIND_NPC_HAULER &&
            ch->kind != CHARACTER_KIND_NPC_TOW) continue;
        if (ch->ship_idx < 0 || ch->ship_idx >= MAX_SHIPS) return NULL;
        return &w->ships[ch->ship_idx];
    }
    return NULL;
}

static void npc_ship_manifest_sync_cargo(npc_ship_t *npc, ship_t *ship) {
    if (!npc || !ship || ship->manifest.count == 0) return;
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
        npc->cargo[c] = 0.0f;
        ship->cargo[c] = 0.0f;
    }
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (u->commodity < COMMODITY_RAW_ORE_COUNT ||
            u->commodity >= COMMODITY_COUNT) {
            continue;
        }
        npc->cargo[u->commodity] += 1.0f;
        ship->cargo[u->commodity] += 1.0f;
    }
}

static bool write_npc_ship_manifest_payload(FILE *f, const ship_t *ship) {
    uint16_t count = 0;
    if (ship && ship->manifest.units) count = ship->manifest.count;
    if (fwrite(&count, sizeof(count), 1, f) != 1) return false;

    const ship_receipts_t *rcpts = ship_get_receipts_const(ship);
    for (uint16_t u = 0; u < count; u++) {
        const cargo_unit_t *cu = &ship->manifest.units[u];
        if (fwrite(cu, sizeof(*cu), 1, f) != 1) return false;

        uint8_t len = 0;
        if (rcpts && u < rcpts->count) {
            len = rcpts->chains[u].len;
            if (len > CARGO_RECEIPT_CHAIN_MAX_LEN)
                len = CARGO_RECEIPT_CHAIN_MAX_LEN;
        }
        if (fwrite(&len, sizeof(len), 1, f) != 1) return false;
        for (uint8_t k = 0; k < len; k++) {
            const cargo_receipt_t *r = &rcpts->chains[u].links[k];
            if (fwrite(r, sizeof(*r), 1, f) != 1) return false;
        }
    }
    return true;
}

static bool read_npc_ship_manifest_payload(FILE *f, ship_t *ship) {
    uint16_t count = 0;
    if (fread(&count, sizeof(count), 1, f) != 1) return false;
    if (count > NPC_SHIP_MANIFEST_SAVE_MAX) return false;

    ship_receipts_t *rcpts = NULL;
    if (ship) {
        if (!ship_manifest_bootstrap(ship)) return false;
        manifest_clear(&ship->manifest);
        rcpts = ship_get_receipts(ship);
        if (!rcpts) return false;
        ship_receipts_clear(rcpts);
        if (count > ship->manifest.cap &&
            !manifest_reserve(&ship->manifest, count)) {
            return false;
        }
        if (count > rcpts->cap && !ship_receipts_reserve(rcpts, count))
            return false;
    }

    for (uint16_t u = 0; u < count; u++) {
        cargo_unit_t cu = {0};
        if (fread(&cu, sizeof(cu), 1, f) != 1) return false;

        uint8_t len = 0;
        if (fread(&len, sizeof(len), 1, f) != 1) return false;
        if (len > CARGO_RECEIPT_CHAIN_MAX_LEN) return false;

        cargo_receipt_t links[CARGO_RECEIPT_CHAIN_MAX_LEN];
        memset(links, 0, sizeof(links));
        for (uint8_t k = 0; k < len; k++) {
            if (fread(&links[k], sizeof(links[k]), 1, f) != 1)
                return false;
        }

        if (ship) {
            if (!manifest_push(&ship->manifest, &cu)) return false;
            if (len > 0) {
                if (!ship_receipts_push_chain(rcpts, links, len))
                    return false;
            } else {
                if (!ship_receipts_push_empty(rcpts)) return false;
            }
        }
    }
    return true;
}

/* ---- contract field-by-field I/O ---- */
static bool write_contract(FILE *f, const contract_t *c) {
    WRITE_FIELD(f, c->active);
    WRITE_FIELD(f, c->action);
    WRITE_FIELD(f, c->station_index);
    WRITE_FIELD(f, c->commodity);
    WRITE_FIELD(f, c->required_grade);
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
    /* v30+ persists required_grade. Older saves default to COMMON. */
    if (g_loaded_save_version >= 30) {
        READ_FIELD(f, c->required_grade);
    } else {
        c->required_grade = (uint8_t)MINING_GRADE_COMMON;
    }
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
    /* v25: station count + next ID counter */
    { int32_t sc = (int32_t)w->station_count;
      WRITE_FIELD(f, sc); }
    WRITE_FIELD(f, w->next_station_id);
    WRITE_FIELD(f, w->next_fracture_id);

    /* Stations — session-tier only (identity lives in station catalog) */
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!write_station_session(f, &w->stations[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* Active fracture children (v27+): counted sidecar section.
     * Terrain asteroids still remain derived from the belt seed. */
    {
        uint32_t fracture_child_count = 0;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (w->asteroids[i].active && w->asteroids[i].fracture_child)
                fracture_child_count++;
        }
        WRITE_FIELD(f, fracture_child_count);
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (!w->asteroids[i].active || !w->asteroids[i].fracture_child) continue;
            if (!write_fracture_child(f, (uint16_t)i, &w->asteroids[i],
                                      &w->fracture_claims[i])) {
                fclose(f);
                remove(tmp_path);
                return false;
            }
        }
    }
    /* Asteroids: terrain remains derived from belt seed */
    /* Scaffolds: removed in v24 — transient in-flight construction */
    /* v37: belt_seed (anchor for rock_pub derivation). v38: each
     * destroyed_rocks entry now carries a destroyed_at_ms timestamp;
     * the array is kept sorted ascending by rock_pub, so writing in
     * index order preserves order on read. Sparse: count + N tuples. */
    WRITE_FIELD(f, w->belt_seed);
    {
        uint16_t count = w->destroyed_rock_count;
        WRITE_FIELD(f, count);
        for (uint16_t i = 0; i < count; i++) {
            if (fwrite(w->destroyed_rocks[i].rock_pub, 32, 1, f) != 1) {
                fclose(f); remove(tmp_path); return false;
            }
            WRITE_FIELD(f, w->destroyed_rocks[i].destroyed_at_ms);
        }
    }
    /* NPC ships */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!write_npc(f, &w->npc_ships[i])) { fclose(f); remove(tmp_path); return false; }
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!write_contract(f, &w->contracts[i])) { fclose(f); remove(tmp_path); return false; }
    }

    /* v36: pubkey registry tail (#479 A.2). Variable-length: count + N
     * entries of (pubkey:32 + session_token:8). Loader for older saves
     * skips this section and starts with an empty registry — clients
     * will rebuild it on first REGISTER_PUBKEY of the next session. */
    {
        uint32_t reg_count = 0;
        for (int r = 0; r < MAX_PLAYERS; r++)
            if (w->pubkey_registry[r].in_use) reg_count++;
        WRITE_FIELD(f, reg_count);
        for (int r = 0; r < MAX_PLAYERS; r++) {
            if (!w->pubkey_registry[r].in_use) continue;
            if (fwrite(w->pubkey_registry[r].pubkey, 32, 1, f) != 1) {
                fclose(f); remove(tmp_path); return false;
            }
            if (fwrite(w->pubkey_registry[r].session_token, 8, 1, f) != 1) {
                fclose(f); remove(tmp_path); return false;
            }
        }
    }

    /* v52: paired NPC ship manifest tail. Fixed by NPC slot so a
     * mid-transit hauler reload keeps the exact cargo_unit_t pubs it
     * took from station inventory. Empty slots write just count=0. */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const ship_t *ship = NULL;
        if (w->npc_ships[i].active)
            ship = world_save_npc_ship_for(w, i);
        if (!write_npc_ship_manifest_payload(f, ship)) {
            fclose(f);
            remove(tmp_path);
            return false;
        }
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

    /* v25+: station_count header; v24: fixed at 8 */
    int save_station_slots = 8; /* v24 and earlier had MAX_STATIONS=8 */
    if (version >= 25) {
        int32_t sc;
        READ_FIELD(f, sc);
        w->station_count = (int)sc;
        READ_FIELD(f, w->next_station_id);
        if (version >= 27) READ_FIELD(f, w->next_fracture_id);
        else w->next_fracture_id = 0;
        save_station_slots = MAX_STATIONS; /* v25 writes all 64 slots */
    } else {
        w->next_fracture_id = 0;
    }

    if (version >= 24) {
        /* v24+: station identity comes from catalog; read session only */
        for (int i = 0; i < save_station_slots; i++) {
            if (!read_station_session(f, &w->stations[i])) return false;
        }
        /* v24→v25 migration: scan for active stations to set station_count */
        if (version < 25) {
            w->station_count = 3;
            for (int i = 3; i < save_station_slots; i++)
                if (station_exists(&w->stations[i]) && i >= w->station_count)
                    w->station_count = i + 1;
        }
        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(w->fracture_claims, 0, sizeof(w->fracture_claims));
        memset(w->asteroid_origin, 0, sizeof(w->asteroid_origin));
        if (version >= 27) {
            uint32_t fracture_child_count = 0;
            READ_FIELD(f, fracture_child_count);
            if (fracture_child_count > MAX_ASTEROIDS) {
                fclose(f);
                return false;
            }
            for (uint32_t i = 0; i < fracture_child_count; i++) {
                if (!read_fracture_child(f, w)) {
                    fclose(f);
                    return false;
                }
            }
        }
        /* No terrain asteroids or scaffolds in v24+ */
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
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_manifest_bootstrap(&w->stations[i])) {
            fclose(f);
            return false;
        }
    }
    /* v37+: belt_seed + destroyed_rocks ledger (#285 slice 1). v38
     * adds the destroyed_at_ms timestamp per entry; v37 entries load
     * with timestamp=0 so the ledger still works for membership but
     * loses the "destroyed before epoch N" bound on those records. */
    memset(w->destroyed_rocks, 0, sizeof(w->destroyed_rocks));
    w->destroyed_rock_count = 0;
    if (version >= 37) {
        READ_FIELD(f, w->belt_seed);
        uint16_t count = 0;
        READ_FIELD(f, count);
        int cap = (int)(sizeof(w->destroyed_rocks) / sizeof(w->destroyed_rocks[0]));
        if (count > cap) { fclose(f); return false; }
        for (uint16_t i = 0; i < count; i++) {
            if (fread(w->destroyed_rocks[i].rock_pub, 32, 1, f) != 1) {
                fclose(f); return false;
            }
            if (version >= 38) {
                READ_FIELD(f, w->destroyed_rocks[i].destroyed_at_ms);
            } else {
                w->destroyed_rocks[i].destroyed_at_ms = 0;
            }
        }
        w->destroyed_rock_count = count;
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

    /* v36: pubkey registry tail (#479 A.2). v35 and earlier saves end
     * with the contracts section; the registry stays zero-initialized
     * and rebuilds itself on first REGISTER_PUBKEY of the next session. */
    memset(w->pubkey_registry, 0, sizeof(w->pubkey_registry));
    if (version >= 36) {
        uint32_t reg_count = 0;
        READ_FIELD(f, reg_count);
        if (reg_count > MAX_PLAYERS) {
            fclose(f);
            return false;
        }
        for (uint32_t r = 0; r < reg_count; r++) {
            if (fread(w->pubkey_registry[r].pubkey, 32, 1, f) != 1) {
                fclose(f);
                return false;
            }
            if (fread(w->pubkey_registry[r].session_token, 8, 1, f) != 1) {
                fclose(f);
                return false;
            }
            w->pubkey_registry[r].in_use = true;
        }
    }

    bool characters_rebuilt = false;
    if (version >= 52) {
        /* The v52 NPC manifest tail belongs to paired ships[] entries,
         * which are runtime-derived from npc_ships[]. Build that pool
         * before consuming the tail, then skip the final rebuild. */
        rebuild_characters_from_npcs(w);
        characters_rebuilt = true;
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            ship_t *ship = NULL;
            if (w->npc_ships[i].active)
                ship = world_npc_ship_for(w, i);
            if (!read_npc_ship_manifest_payload(f, ship)) {
                fclose(f);
                return false;
            }
            if (ship && ship->manifest.count > 0)
                npc_ship_manifest_sync_cargo(&w->npc_ships[i], ship);
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

    /* v34: MODULE_FURNACE_CU/_CR were collapsed into a single
     * MODULE_FURNACE — the count of furnaces on a station now decides
     * what it can smelt. Old saves stored module type bytes from the
     * pre-collapse enum where:
     *   0 DOCK, 1 HOPPER, 2 FURNACE, 3 FURNACE_CU, 4 FURNACE_CR,
     *   5 REPAIR_BAY, 6 SIGNAL_RELAY, 7 FRAME_PRESS, 8 LASER_FAB,
     *   9 TRACTOR_FAB, 10 ORE_SILO, 11 SHIPYARD, 12 CARGO_BAY.
     * Map old 3 and 4 to FURNACE (new 2), shift 5..12 down by two. The
     * per-ring 1-furnace cap is enforced going forward; old saves that
     * carry two furnace subtypes on the same ring will keep both — the
     * runtime never tries to *add* extras and `station_furnace_count`
     * just reads what's there. (Helios's seeded layout is the only
     * known case, and the fresh seed below already drops it to 1/ring.) */
    if (version < 34) {
        static const int FURNACE_REMAP[13] = {
            0,  /* old 0  DOCK         -> DOCK         (new 0) */
            1,  /* old 1  HOPPER       -> HOPPER       (new 1) */
            2,  /* old 2  FURNACE      -> FURNACE      (new 2) */
            2,  /* old 3  FURNACE_CU   -> FURNACE      (new 2) */
            2,  /* old 4  FURNACE_CR   -> FURNACE      (new 2) */
            3,  /* old 5  REPAIR_BAY   -> REPAIR_BAY   (new 3) */
            4,  /* old 6  SIGNAL_RELAY -> SIGNAL_RELAY (new 4) */
            5,  /* old 7  FRAME_PRESS  -> FRAME_PRESS  (new 5) */
            6,  /* old 8  LASER_FAB    -> LASER_FAB    (new 6) */
            7,  /* old 9  TRACTOR_FAB  -> TRACTOR_FAB  (new 7) */
            8,  /* old 10 ORE_SILO     -> ORE_SILO     (new 8) */
            9,  /* old 11 SHIPYARD     -> SHIPYARD     (new 9) */
            10, /* old 12 CARGO_BAY    -> CARGO_BAY    (new 10) */
        };
        for (int i = 0; i < MAX_STATIONS; i++) {
            station_t *st = &w->stations[i];
            for (int m = 0; m < st->module_count; m++) {
                int old_t = (int)st->modules[m].type;
                if (old_t >= 0 && old_t < 13)
                    st->modules[m].type = (module_type_t)FURNACE_REMAP[old_t];
            }
            for (int p = 0; p < st->pending_scaffold_count; p++) {
                int old_t = (int)st->pending_scaffolds[p].type;
                if (old_t >= 0 && old_t < 13)
                    st->pending_scaffolds[p].type = (module_type_t)FURNACE_REMAP[old_t];
            }
            for (int p = 0; p < st->placement_plan_count; p++) {
                int old_t = (int)st->placement_plans[p].type;
                if (old_t >= 0 && old_t < 13)
                    st->placement_plans[p].type = (module_type_t)FURNACE_REMAP[old_t];
            }
            rebuild_station_services(st);
        }
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            if (!w->scaffolds[i].active) continue;
            int old_t = (int)w->scaffolds[i].module_type;
            if (old_t >= 0 && old_t < 13)
                w->scaffolds[i].module_type = (module_type_t)FURNACE_REMAP[old_t];
        }
    }

    /* v44 silo cleanup: MODULE_ORE_SILO (was 8) and MODULE_CARGO_BAY
     * (was 10) were dropped; HOPPER absorbs both storage roles. The
     * other enum positions stayed put (DOCK=0, HOPPER=1, FURNACE=2,
     * REPAIR_BAY=3, SIGNAL_RELAY=4, FRAME_PRESS=5, LASER_FAB=6,
     * TRACTOR_FAB=7, SHIPYARD=9), so the only operation needed is
     * remapping any module/scaffold/plan that used 8 or 10 → 1. */
    if (version < 44) {
        for (int i = 0; i < MAX_STATIONS; i++) {
            station_t *st = &w->stations[i];
            for (int m = 0; m < st->module_count; m++) {
                int t = (int)st->modules[m].type;
                if (t == 8 || t == 10) st->modules[m].type = MODULE_HOPPER;
            }
            for (int p = 0; p < st->pending_scaffold_count; p++) {
                int t = (int)st->pending_scaffolds[p].type;
                if (t == 8 || t == 10) st->pending_scaffolds[p].type = MODULE_HOPPER;
            }
            for (int p = 0; p < st->placement_plan_count; p++) {
                int t = (int)st->placement_plans[p].type;
                if (t == 8 || t == 10) st->placement_plans[p].type = MODULE_HOPPER;
            }
            rebuild_station_services(st);
        }
        for (int i = 0; i < MAX_SCAFFOLDS; i++) {
            if (!w->scaffolds[i].active) continue;
            int t = (int)w->scaffolds[i].module_type;
            if (t == 8 || t == 10) w->scaffolds[i].module_type = MODULE_HOPPER;
        }
    }

    /* v51 cargo-in-space schema (Slice 1) — see helper. */
    if (version < 51) {
        world_apply_cargo_schema_migration(w);
        printf("[save] migrated v%d -> v51: tagged furnaces + auto-spawned output hoppers\n",
               (int)version);
    }

    /* v24-v26: asteroids and scaffolds no longer saved — ensure arrays are
     * clean whether we read-and-discarded legacy data or skipped them.
     * v27 brings back already-fractured children only. */
    if (version < 27) {
        memset(w->asteroids, 0, sizeof(w->asteroids));
        memset(w->fracture_claims, 0, sizeof(w->fracture_claims));
        memset(w->asteroid_origin, 0, sizeof(w->asteroid_origin));
    }
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
        ship_cleanup(&w->players[i].ship);
        memset(&w->players[i], 0, sizeof(w->players[i]));
    }

    fclose(f);
    belt_field_init(&w->belt, w->rng, BELT_SCALE);
    rebuild_signal_chain(w);
    if (!characters_rebuilt)
        rebuild_characters_from_npcs(w);
    /* Layer B of #479: rederive every station's private key from its
     * persisted pubkey + provenance. The secret was never written to
     * disk — this is what makes a save leak NOT a key leak. v39 and
     * earlier saves additionally rederive the pubkey itself (seeded
     * indices 0/1/2 from world seed; outposts from a zero-founder
     * placeholder, accepted v39 provenance gap).
     *
     * We rederive seeded slots 0/1/2 unconditionally — they always
     * exist in any reachable world state — and also any outpost slot
     * whose pubkey is non-zero (i.e. the slot was occupied at save
     * time). station_exists() depends on geometry fields that may
     * legitimately be zeroed in catalog-less test scenarios, so it's
     * not the right gate here. */
    static const uint8_t zero_pub[32] = {0};
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (i < 3 ||
            memcmp(w->stations[i].station_pubkey, zero_pub, 32) != 0) {
            station_authority_rederive_secret(&w->stations[i],
                                              w->belt_seed, i);
        }
    }
    /* Layer C of #479: walk every station's chain log on disk and
     * verify it against its station_pubkey. A corrupt chain (bad
     * signature, broken prev_hash linkage, or last_hash mismatch
     * vs. the saved continuation pointer) is loud — we log a warning
     * but do NOT silently rebuild the log. Operators must investigate.
     * An empty log on disk is the post-migration v40 case and is fine. */
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &w->stations[i];
        if (memcmp(st->station_pubkey, zero_pub, 32) == 0) continue;
        uint64_t walked = 0;
        uint8_t walked_last[32] = {0};
        if (!chain_log_verify(st, &walked, walked_last)) {
            SIM_LOG("[chain] station %d: chain log VERIFICATION FAILED "
                    "after %llu events — investigate; log untouched\n",
                    i, (unsigned long long)walked);
            continue;
        }
        /* Cross-check the disk-walked tail against the saved
         * continuation pointer. A mismatch means the save and the log
         * file got separated (e.g. someone restored world.sav from a
         * backup but kept the current chain/ dir). Loud + non-fatal. */
        if (walked != st->chain_event_count ||
            (walked > 0 &&
             memcmp(walked_last, st->chain_last_hash, 32) != 0)) {
            SIM_LOG("[chain] station %d: chain continuation mismatch "
                    "(disk: %llu events, save: %llu) — events appended "
                    "after this point will form a fork from the saved "
                    "head\n",
                    i, (unsigned long long)walked,
                    (unsigned long long)st->chain_event_count);
        }
    }
    return true;
}

/* ================================================================== */
/* Player persistence                                                  */
/* ================================================================== */

#define PLAYER_MAGIC    0x504C5937u  /* "PLY7" — #479 D: appends per-ship receipt chains */
#define PLAYER_MAGIC_V6 0x504C5936u  /* "PLY6" — #479 A.3: appends last_signed_nonce */
#define PLAYER_MAGIC_V5 0x504C5935u  /* "PLY5" — #339 A.2: adds ship.manifest tail */
#define PLAYER_MAGIC_V4 0x504C5934u  /* "PLY4" — explicit ship payload, no runtime manifest pointers */
#define PLAYER_MAGIC_V3 0x504C5933u  /* "PLY3" — v25: station-local credits (#312) */
#define PLAYER_MAGIC_V2 0x504C5932u  /* "PLY2" — v22-v24: post #280 enum cleanup */
#define PLAYER_MAGIC_V1 0x504C5952u  /* "PLYR" — v21 and earlier */

/* PLY3 ship layout — pre-hold-ingot ship payload, kept explicit so we
 * can read older files without depending on the current ship_t layout. */
typedef struct {
    vec2 pos; vec2 vel; float angle; float hull;
    float cargo[COMMODITY_COUNT];
    hull_class_t hull_class;
    int mining_level, hold_level, tractor_level;
    int16_t towed_fragments[10]; uint8_t towed_count;
    int16_t towed_scaffold; bool tractor_active;
    float comm_range;
    uint32_t unlocked_modules;
    float stat_ore_mined, stat_credits_earned, stat_credits_spent;
    int stat_asteroids_fractured;
} ship_v3_t;

typedef struct {
    uint32_t magic;
    ship_v3_t ship;
    int last_station;
    vec2 last_pos;
    float last_angle;
} player_save_v3_t;

static void migrate_v3_ship(ship_t *dst, const ship_v3_t *src) {
    /* ship_t had hold_ingots[] from PLY3 forward; v35 collapsed that
     * dual store into the ship manifest. The runtime-only manifest is
     * not loaded here. */
    ship_cleanup(dst);
    memset(dst, 0, sizeof(*dst));
    (void)ship_manifest_bootstrap(dst);
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
    dst->comm_range = src->comm_range;
    dst->unlocked_modules = src->unlocked_modules;
    dst->stat_ore_mined = src->stat_ore_mined;
    dst->stat_credits_earned = src->stat_credits_earned;
    dst->stat_credits_spent = src->stat_credits_spent;
    dst->stat_asteroids_fractured = src->stat_asteroids_fractured;
}

/* PLY4 ship layout — the pre-manifest ship_t payload kept explicit so
 * adding runtime-only fields to ship_t doesn't change the on-disk bytes.
 * v35 dropped hold_ingots from ship_t but the on-disk PLY4/PLY5 ship
 * blob still embeds the legacy hold-ingot array, so the bytes stay
 * stable for old saves. */
typedef struct {
    vec2 pos; vec2 vel; float angle; float hull;
    float cargo[COMMODITY_COUNT];
    hull_class_t hull_class;
    int mining_level, hold_level, tractor_level;
    int16_t towed_fragments[10]; uint8_t towed_count;
    int16_t towed_scaffold; bool tractor_active;
    float comm_range;
    uint32_t unlocked_modules;
    float stat_ore_mined, stat_credits_earned, stat_credits_spent;
    int stat_asteroids_fractured;
    legacy_named_ingot_t hold_ingots[LEGACY_SHIP_HOLD_INGOTS_MAX];
    int hold_ingots_count;
} ship_v4_t;

typedef struct {
    uint32_t magic;
    ship_v4_t ship;
    int last_station;
    vec2 last_pos;
    float last_angle;
} player_save_data_t;

static void encode_v4_ship(ship_v4_t *dst, const ship_t *src) {
    if (!dst || !src) return;
    memset(dst, 0, sizeof(*dst));
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
    dst->comm_range = src->comm_range;
    dst->unlocked_modules = src->unlocked_modules;
    dst->stat_ore_mined = src->stat_ore_mined;
    dst->stat_credits_earned = src->stat_credits_earned;
    dst->stat_credits_spent = src->stat_credits_spent;
    dst->stat_asteroids_fractured = src->stat_asteroids_fractured;
    /* Legacy hold-ingot array stays zero on save: the ship manifest
     * is the single source of truth post-v35. The bytes still occupy
     * the on-disk slot so PLY4/PLY5 readers stay byte-aligned. */
}

static void migrate_v4_ship(ship_t *dst, const ship_v4_t *src) {
    if (!dst || !src) return;
    ship_cleanup(dst);
    memset(dst, 0, sizeof(*dst));
    (void)ship_manifest_bootstrap(dst);
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
    dst->comm_range = src->comm_range;
    dst->unlocked_modules = src->unlocked_modules;
    dst->stat_ore_mined = src->stat_ore_mined;
    dst->stat_credits_earned = src->stat_credits_earned;
    dst->stat_credits_spent = src->stat_credits_spent;
    dst->stat_asteroids_fractured = src->stat_asteroids_fractured;
    /* v35 migration: lift legacy hold-ingot rows into the ship manifest
     * as smelt-recipe units so the player keeps custody. Empty slots
     * (zero pubkey) skip. PLY5 saves wrote zeros here under the new
     * encode_v4_ship; this only matters for older PLY3/PLY4 saves
     * captured before the unification. */
    int n = src->hold_ingots_count;
    if (n < 0) n = 0;
    if (n > LEGACY_SHIP_HOLD_INGOTS_MAX) n = LEGACY_SHIP_HOLD_INGOTS_MAX;
    static const uint8_t zero_pk[32] = {0};
    for (int i = 0; i < n; i++) {
        const legacy_named_ingot_t *lg = &src->hold_ingots[i];
        if (memcmp(lg->pubkey, zero_pk, 32) == 0) continue;
        cargo_unit_t u = {0};
        u.kind = (uint8_t)CARGO_KIND_INGOT;
        u.commodity = lg->metal;
        u.grade = (uint8_t)MINING_GRADE_COMMON;
        u.prefix_class = lg->prefix_class;
        u.recipe_id = (uint16_t)RECIPE_SMELT;
        u.origin_station = lg->origin_station;
        u.quantity = 1;
        u.mined_block = lg->mined_block;
        memcpy(u.pub, lg->pubkey, 32);
        (void)manifest_push(&dst->manifest, &u);
    }
}

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

/* Layer A.4 of #479 — per-player save layout.
 *
 *   <dir>/pubkey/<base58(pubkey)>.sav   if pubkey_set
 *   <dir>/legacy/<token_hex>.sav        otherwise (anonymous / pre-A.1 client)
 *
 * Subdirectories are created on demand with 0700. The "pubkey" tier is
 * the persistent identity story; "legacy" exists so an A.0/A.1 client
 * (no registered pubkey) doesn't lose its save, and so existing v39
 * saves survive the migration to be claimed-by-signature later. */
#define LEGACY_SUBDIR "legacy"
#define PUBKEY_SUBDIR "pubkey"

static void ensure_save_subdirs(const char *dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", dir, PUBKEY_SUBDIR);
    (void)mkdir_700(path);
    snprintf(path, sizeof(path), "%s/%s", dir, LEGACY_SUBDIR);
    (void)mkdir_700(path);
}

static bool pubkey_is_zero32(const uint8_t pk[32]) {
    for (int i = 0; i < 32; i++) if (pk[i]) return false;
    return true;
}

/* Compute the on-disk save path for this player. Returns true if a path
 * was produced; false only if the player has neither a pubkey nor a
 * session_token (a wholly fresh slot — nothing to persist yet). */
bool player_save_path(char *out, size_t outlen, const char *dir,
                      const server_player_t *sp, int slot) {
    static const uint8_t zero_token[8] = {0};
    if (sp->pubkey_set && !pubkey_is_zero32(sp->pubkey)) {
        char b58[64];
        if (base58_encode(sp->pubkey, 32, b58, sizeof(b58)) == 0) return false;
        snprintf(out, outlen, "%s/%s/%s.sav", dir, PUBKEY_SUBDIR, b58);
        return true;
    }
    if (sp->session_ready && memcmp(sp->session_token, zero_token, 8) != 0) {
        char hex[17];
        session_token_to_hex(sp->session_token, hex);
        snprintf(out, outlen, "%s/%s/player_%s.sav", dir, LEGACY_SUBDIR, hex);
        return true;
    }
    /* Fully anonymous fresh slot — fall back to the slot-numbered path,
     * also under legacy/, so non-token disconnects don't pollute the
     * top-level directory. */
    snprintf(out, outlen, "%s/%s/player_%d.sav", dir, LEGACY_SUBDIR, slot);
    return true;
}

/* One-shot startup migration: any top-level .sav files left behind from
 * the v39-and-earlier layout get moved into <dir>/legacy/ so the new
 * layout takes effect. Idempotent: missing source dir or missing files
 * are no-ops. Files already in legacy/ or pubkey/ are untouched. */
void player_save_migrate_legacy_layout(const char *dir) {
    ensure_save_subdirs(dir);
#ifdef _WIN32
    /* Win32 dir scan is OS-specific; we don't ship the dedicated server
     * on Windows. Document the limitation and skip — operators on Win32
     * with v39 saves will need to move them into legacy/ by hand. */
    (void)dir;
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcmp(name + len - 4, ".sav") != 0) continue;
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s", dir, name);
        snprintf(dst, sizeof(dst), "%s/" LEGACY_SUBDIR "/%s", dir, name);
        struct stat sst;
        if (stat(src, &sst) != 0) continue;
        if (!S_ISREG(sst.st_mode)) continue;
        if (rename(src, dst) == 0) {
            SIM_LOG("[sim] migrated legacy save %s -> %s\n", src, dst);
        } else if (errno != ENOENT) {
            /* If destination already exists, leave the source — operator
             * can resolve. */
        }
    }
    closedir(d);
#endif
}

/* Enumerate up to `cap` legacy saves. Each entry's prefix
 * (LEGACY_SAVES_PREFIX_LEN chars) and the full base name (without .sav
 * suffix) are written into the parallel arrays. Returns the count. */
int player_save_list_legacy(const char *dir,
                            char prefixes[][LEGACY_SAVES_PREFIX_LEN + 1],
                            char names[][64],
                            int cap) {
    int count = 0;
#ifdef _WIN32
    (void)dir; (void)prefixes; (void)names; (void)cap; (void)count;
    return 0;
#else
    char path[512];
    snprintf(path, sizeof(path), "%s/" LEGACY_SUBDIR, dir);
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *de;
    while (count < cap && (de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (name[0] == '.') continue;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (strcmp(name + len - 4, ".sav") != 0) continue;
        size_t base_len = len - 4;
        if (base_len >= 64) base_len = 63;
        memcpy(names[count], name, base_len);
        names[count][base_len] = '\0';
        size_t pre = base_len < LEGACY_SAVES_PREFIX_LEN ?
                     base_len : (size_t)LEGACY_SAVES_PREFIX_LEN;
        memcpy(prefixes[count], names[count], pre);
        prefixes[count][pre] = '\0';
        count++;
    }
    closedir(d);
    return count;
#endif
}

/* Attempt to rename saves/legacy/<basename>.sav into
 * saves/pubkey/<base58(pubkey)>.sav. Returns true on success, false on
 * any failure (missing source, target exists, rename error, etc.).
 * The caller is responsible for verifying the claim signature first. */
bool player_save_rename_legacy_to_pubkey(const char *dir,
                                         const char *basename,
                                         const uint8_t pubkey[32]) {
    if (!basename || !basename[0]) return false;
    if (pubkey_is_zero32(pubkey)) return false;
    /* Reject path traversal in the basename. */
    for (const char *p = basename; *p; p++) {
        if (*p == '/' || *p == '\\') return false;
        if (*p == '.' && p[1] == '.') return false;
    }
    char b58[64];
    if (base58_encode(pubkey, 32, b58, sizeof(b58)) == 0) return false;
    char src[512], dst[512];
    snprintf(src, sizeof(src), "%s/" LEGACY_SUBDIR "/%s.sav", dir, basename);
    snprintf(dst, sizeof(dst), "%s/" PUBKEY_SUBDIR "/%s.sav", dir, b58);
    ensure_save_subdirs(dir);
    /* Refuse to clobber an existing pubkey save — first-claim-wins, and
     * the player on this pubkey already has a record. */
    struct stat dst_st;
    if (stat(dst, &dst_st) == 0) return false;
    if (rename(src, dst) != 0) return false;
    SIM_LOG("[sim] claimed legacy save %s -> %s\n", src, dst);
    return true;
}

bool player_save(const server_player_t *sp, const char *dir, int slot) {
    char path[256];
    ship_v4_t ship_disk;
    /* #339 slice A.2: PLY5 format lifts the empty-manifest guard and
     * appends a manifest tail (count + packed cargo_unit_t entries)
     * between the fixed ship blob and the CRC trailer.
     * #479 A.4: filename keyed by pubkey when registered, else by
     * legacy session_token under saves/legacy/. */
    ensure_save_subdirs(dir);
    if (!player_save_path(path, sizeof(path), dir, sp, slot)) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    encode_v4_ship(&ship_disk, &sp->ship);
    player_save_data_t data = {
        .magic = PLAYER_MAGIC,
        .ship = ship_disk,
        .last_station = sp->current_station,
        .last_pos = sp->ship.pos,
        .last_angle = sp->ship.angle,
    };
    bool ok = fwrite(&data, sizeof(data), 1, f) == 1;
    uint32_t crc = ok ? crc32_update(0, &data, sizeof(data)) : 0;
    /* Manifest tail (PLY5). Count + entries; CRC accumulates both. */
    if (ok) {
        uint16_t manifest_count = sp->ship.manifest.count;
        ok = fwrite(&manifest_count, sizeof(manifest_count), 1, f) == 1;
        if (ok) crc = crc32_update(crc, &manifest_count, sizeof(manifest_count));
        for (uint16_t u = 0; ok && u < manifest_count; u++) {
            const cargo_unit_t *cu = &sp->ship.manifest.units[u];
            ok = fwrite(cu, sizeof(*cu), 1, f) == 1;
            if (ok) crc = crc32_update(crc, cu, sizeof(*cu));
        }
    }
    /* PLY6 tail: last_signed_nonce (#479 A.3). Persisted so a server
     * restart can't replay-accept a signed action whose nonce was
     * already consumed. CRC accumulates these 8 bytes too. */
    if (ok) {
        uint64_t nonce = sp->last_signed_nonce;
        ok = fwrite(&nonce, sizeof(nonce), 1, f) == 1;
        if (ok) crc = crc32_update(crc, &nonce, sizeof(nonce));
    }
    /* PLY7 tail (#479 D): per-cargo receipt chains, one per manifest
     * unit, in manifest order. Each chain on disk is
     *   [len:u8] + len × cargo_receipt_t.
     * Empty chains (len=0) are valid — they signify "cargo never had
     * a receipt attached" (e.g. legacy migration) so the next transfer
     * mints a fresh origin-attested receipt. */
    if (ok) {
        const ship_receipts_t *rcpts = ship_get_receipts_const(&sp->ship);
        uint16_t mc = sp->ship.manifest.count;
        for (uint16_t u = 0; ok && u < mc; u++) {
            uint8_t len = 0;
            if (rcpts && u < rcpts->count) {
                len = rcpts->chains[u].len;
                if (len > CARGO_RECEIPT_CHAIN_MAX_LEN)
                    len = CARGO_RECEIPT_CHAIN_MAX_LEN;
            }
            ok = fwrite(&len, sizeof(len), 1, f) == 1;
            if (ok) crc = crc32_update(crc, &len, sizeof(len));
            for (uint8_t k = 0; ok && k < len; k++) {
                const cargo_receipt_t *r = &rcpts->chains[u].links[k];
                ok = fwrite(r, sizeof(*r), 1, f) == 1;
                if (ok) crc = crc32_update(crc, r, sizeof(*r));
            }
        }
    }
    if (ok) {
        uint32_t crc_magic = 0x43524332u; /* "CRC2" */
        ok = fwrite(&crc_magic, sizeof(crc_magic), 1, f) == 1 &&
             fwrite(&crc, sizeof(crc), 1, f) == 1;
    }
    fclose(f);
    if (ok) SIM_LOG("[sim] saved player %d\n", slot);
    return ok;
}

/* Migrate PLY2 (old ship_t with global credits) to current ship_t */
static void migrate_v2_ship(ship_t *dst, const ship_v2_t *src) {
    ship_cleanup(dst);
    memset(dst, 0, sizeof(*dst));
    (void)ship_manifest_bootstrap(dst);
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
    /* RATi v2 fields not present in PLY2 — zero-init. */
    dst->comm_range = 0.0f;
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
    bool manifest_already_loaded = false;

    ship_cleanup(&sp->ship);

    if (magic == PLAYER_MAGIC || magic == PLAYER_MAGIC_V6 || magic == PLAYER_MAGIC_V5) {
        /* PLY5 (manifest tail), PLY6 (manifest + last_signed_nonce),
         * PLY7 (manifest + last_signed_nonce + receipt chains). */
        player_save_data_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) { fclose(f); return false; }
        migrate_v4_ship(&sp->ship, &data.ship);
        sp->current_station = data.last_station;
        sp->ship.pos = data.last_pos;
        sp->ship.angle = data.last_angle;
        /* Read manifest tail. Bootstrap was called by migrate_v4_ship. */
        uint16_t manifest_count = 0;
        if (fread(&manifest_count, sizeof(manifest_count), 1, f) != 1) {
            fclose(f); return false;
        }
        if (manifest_count > 0) {
            if (!manifest_reserve(&sp->ship.manifest, manifest_count)) {
                fclose(f); return false;
            }
            for (uint16_t u = 0; u < manifest_count; u++) {
                cargo_unit_t cu;
                if (fread(&cu, sizeof(cu), 1, f) != 1) {
                    fclose(f); return false;
                }
                sp->ship.manifest.units[u] = cu;
            }
            sp->ship.manifest.count = manifest_count;
            /* Cargo_unit_t byte 7 was _pad in pre-v45 saves and is now
             * `quantity`. Idempotent rewrite: 0 → 1 leaves v45+ saves
             * untouched and migrates the legacy zero to a valid count. */
            manifest_migrate_quantity(&sp->ship.manifest);
        }
        /* PLY6+ last_signed_nonce. PLY5 saves end here; the nonce stays
         * at zero, which lets the first signed action after the migration
         * use any non-zero nonce. */
        sp->last_signed_nonce = 0;
        if (magic == PLAYER_MAGIC || magic == PLAYER_MAGIC_V6) {
            uint64_t nonce = 0;
            if (fread(&nonce, sizeof(nonce), 1, f) != 1) {
                fclose(f); return false;
            }
            sp->last_signed_nonce = nonce;
        }
        /* PLY7 (#479 D): per-ship cargo_receipt_t chains. The store
         * mirrors ship.manifest, so we expect exactly manifest_count
         * chains. Each chain is [len:u8] + len × CARGO_RECEIPT_SIZE.
         * v6 saves stop short here — the receipt store stays empty;
         * the next BUY/DELIVER for that cargo will sign a fresh
         * origin-attested receipt (one-time migration cost). */
        if (magic == PLAYER_MAGIC) {
            ship_receipts_t *rcpts = ship_get_receipts(&sp->ship);
            if (!rcpts) { fclose(f); return false; }
            ship_receipts_clear(rcpts);
            if (manifest_count > 0) {
                if (!ship_receipts_reserve(rcpts, manifest_count)) {
                    fclose(f); return false;
                }
                for (uint16_t u = 0; u < manifest_count; u++) {
                    uint8_t link_count = 0;
                    if (fread(&link_count, sizeof(link_count), 1, f) != 1) {
                        fclose(f); return false;
                    }
                    if (link_count > CARGO_RECEIPT_CHAIN_MAX_LEN) {
                        fclose(f); return false; /* corrupt */
                    }
                    cargo_receipt_t links[CARGO_RECEIPT_CHAIN_MAX_LEN];
                    for (uint8_t k = 0; k < link_count; k++) {
                        if (fread(&links[k], sizeof(cargo_receipt_t), 1, f) != 1) {
                            fclose(f); return false;
                        }
                    }
                    if (link_count > 0) {
                        if (!ship_receipts_push_chain(rcpts, links, link_count)) {
                            fclose(f); return false;
                        }
                    } else {
                        if (!ship_receipts_push_empty(rcpts)) {
                            fclose(f); return false;
                        }
                    }
                }
            }
        }
        manifest_already_loaded = true;
        fclose(f);
    } else if (magic == PLAYER_MAGIC_V4) {
        /* PLY4 → PLY5: read ship blob; manifest stays empty (was never
         * persisted in PLY4, lived only at runtime). */
        player_save_data_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) { fclose(f); return false; }
        fclose(f);
        migrate_v4_ship(&sp->ship, &data.ship);
        sp->current_station = data.last_station;
        sp->ship.pos = data.last_pos;
        sp->ship.angle = data.last_angle;
    } else if (magic == PLAYER_MAGIC_V3) {
        /* PLY3 → PLY4: migrate ship_v3_t → ship_t, zero-init hold_ingots. */
        player_save_v3_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) { fclose(f); return false; }
        fclose(f);
        migrate_v3_ship(&sp->ship, &data.ship);
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
    /* Slice D: pre-PLY5 saves had no ship manifest. Synthesize
     * RECIPE_LEGACY_MIGRATE units from the float-held finished goods
     * in ship.cargo[] so the manifest layer sees a consistent state.
     * Origin salt = session_token so the same save reloads to stable
     * pubs; falls back to a zero origin when the token isn't set yet
     * (early-session load). */
    if (!manifest_already_loaded) {
        uint8_t origin[8] = {0};
        if (sp->session_ready) memcpy(origin, sp->session_token, 8);
        (void)manifest_migrate_legacy_inventory(&sp->ship.manifest,
                                                sp->ship.cargo,
                                                COMMODITY_COUNT, origin);
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
    /* #491 moved slot-based saves into <dir>/legacy/. Try the new
     * location first; fall back to the historical top-level path so
     * any pre-A.4 save written before the migration still loads. */
    snprintf(path, sizeof(path), "%s/" LEGACY_SUBDIR "/player_%d.sav", dir, slot);
    if (player_load_from_path(sp, w, path, slot)) return true;
    snprintf(path, sizeof(path), "%s/player_%d.sav", dir, slot);
    return player_load_from_path(sp, w, path, slot);
}

bool player_load_by_token(server_player_t *sp, world_t *w, const char *dir,
                          const uint8_t token[8]) {
    char hex[17];
    session_token_to_hex(token, hex);
    /* #479 A.4: legacy saves moved into <dir>/legacy/. Try the new
     * location first, then the historical top-level path so any save
     * that escaped the startup migration still loads. */
    char path[256];
    snprintf(path, sizeof(path), "%s/" LEGACY_SUBDIR "/player_%s.sav", dir, hex);
    if (player_load_from_path(sp, w, path, (int)sp->id)) return true;
    snprintf(path, sizeof(path), "%s/player_%s.sav", dir, hex);
    return player_load_from_path(sp, w, path, (int)sp->id);
}

bool player_load_by_pubkey(server_player_t *sp, world_t *w, const char *dir,
                           const uint8_t pubkey[32]) {
    if (pubkey_is_zero32(pubkey)) return false;
    char b58[64];
    if (base58_encode(pubkey, 32, b58, sizeof(b58)) == 0) return false;
    char path[256];
    snprintf(path, sizeof(path), "%s/" PUBKEY_SUBDIR "/%s.sav", dir, b58);
    return player_load_from_path(sp, w, path, (int)sp->id);
}
