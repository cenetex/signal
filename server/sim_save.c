#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

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
 *   - Player save: PLAYER_MAGIC "PLY2", atomic temp+rename like world.sav.
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
#include "faction.h"
#include "gossip.h"
#include "manifest.h"
#include "ship.h"
#include "sim_ai.h"
#include "sim_construction.h"
#include "base58.h"
#include "protocol.h"
#include "station_authority.h"
#include "actor_principal_resolver.h"
#include "contract_ownership.h"
#include "chain_log.h"
#include "legacy_save_recovery.h"
#include "ownership_quarantine.h"
#include "persistence_generation.h"
#include "persistence_io.h"
#include "sha256.h"
#include "signal_memzero.h"
#include "wire_codec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <fcntl.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define mkdir_700(p) _mkdir(p)
#else
#include <unistd.h>
#include <fcntl.h>
#define mkdir_700(p) mkdir((p), 0700)
#endif

/* ================================================================== */
/* World persistence                                                   */
/* ================================================================== */

#define crc32_update       persistence_crc32_update
#define crc32_file         persistence_crc32_file
#define crc32_file_prefix  persistence_crc32_file_prefix
#define save_flush_durable persistence_flush_durable
#define save_replace_file  persistence_replace_file

#define SAVE_MAGIC     0x5349474E  /* "SIGN" */
#define SAVE_CRC_MAGIC 0x43524332u /* "CRC2" */
#define SAVE_STATION_SLOTS_V25 64
#define OWNERSHIP_QUARANTINE_AUTO_REPORT_ROWS 32
#define SAVE_VERSION 84  /* v84: append the durable station payout journal so
                          * a source/action identity cannot credit twice across
                          * retries, reconnects, or save/load.
                          * v83: append Engine commodity/module identities and
                          * expand station module slots from 16 to 18.
                          * v82: cargo-pod player tow custody uses canonical
                          * actor principals. Runtime player slots are
                          * projections only; ambiguous v81 slots migrate into
                          * bound, inert ownership quarantine rows.
                          * v81: contract claimants and delivery debtors use
                          * canonical actor principals. Legacy player-slot
                          * owners are quarantined rather than rebound to
                          * whichever identity later occupies that slot.
                          * v80: active station-custody cargo pods persist
                          * their aggregate integer purchase-price anchor,
                          * original-unit progress, and exact ordered remaining
                          * manifest digest so bounded PRESENT batches conserve
                          * one quote across restart.
                          * v79: durable ship builds/assets use canonical
                          * actor principals, station actors have immutable
                          * IDs, in-flight birth assemblies and completed
                          * birth provenance survive restart, and ambiguous
                          * v73-v78 build modes are quarantined inert.
                          * v78: appends a bounded, server-only ownership
                          * quarantine before CRC2. Legacy actor rows that
                          * cannot be proven are retained as inert public
                          * locators for operator review; no bearer tokens are
                          * stored in this table.
                          * v77: stations persist a bounded, versioned public
                          * authority lifecycle/trust registry. Private station
                          * keys remain memory-only.
                          * v76: cargo pods persist their named tow hardpoint.
                          * v75: station finished-goods residue is persisted
                          * separately; whole stock exists only in manifests.
                          * v74: cargo pods persist their active module tractor
                          * owner so parking/handoff continuity survives restart.
                          * v73: active cargo pods persist station custody so
                          * market theft/debt survives restart.
                          * v72: backfill Kepler starter Laser Module reserve
                          * for the first mining refit.
                          * v71: backfill starter frame pods for live saves
                          * that were already rewritten after the v69
                          * Prospect shell seed but never received it.
                          * v70: cargo pod saves persist the folded frame
                          * unit unfolded into each pod shell, so emptied pods
                          * can fold back into their exact frame.
                          * v69: cargo pod saves persist exact manifest units
                          * when a pod wraps real cargo_unit_t payloads.
                          * v68: active cargo pods persist as durable economy
                          * objects, including shipment_id and towed_by so
                          * in-flight pod cargo survives restarts.
                          * v67: active delivery shipments persist their exact
                          * cargo_unit_t payloads and receipt chains while
                          * cargo rides in towable pods instead of ship holds.
                          * v66: station faction identity/diplomacy persists.
                          * v65: pending shipyard hull builds persist captured
                          * owner identity (pubkey/session) instead of only a
                          * mutable player slot. v64: contract-origin ship
                          * assets persist as a
                          * fixed registry tail after NPC ship manifests.
                          * Older worlds mint legacy assets for active
                          * inline NPC ships on load. v63: station
                          * session data persists pending shipyard hull
                          * commissions.
                          * v62: station player ledgers expand from 16 to
                          * STATION_LEDGER_MAX entries. v61 and older saves
                          * still read their historical 16-entry table.
                          * v61: contracts persist target_pub so fracture
                          * bounties survive asteroid slot reuse/remap by
                          * stable rock identity.
                          * v60: active fracture-child sidecars persist
                          * thrown_by_token + thrown_timer_q for
                          * time-bounded rock-combat ownership.
                          * v59: delivery credit shipment sidecar table
                          * persists outstanding origin debt, delivery
                          * proof, default, and black-market states.
                          * v58: station session section expanded from
                          * 64 to MAX_STATIONS=128 slots. v25-v57 saves
                          * still read exactly their historical 64 slots.
                          * v57: contracts gained forbidden origin masks.
                          * v56: contracts gained optional provenance
                          * requirements (proof flags, recipe, prefix
                          * class, and parent_merkle) for heritage jobs.
                          * Older contracts migrate with zero flags, so
                          * they remain commodity/grade-only.
                          * v55: crystal fragments gained a two-stage
                          * furnace process. Active fracture-child
                          * sidecars persist crystal_stage plus source
                          * station/module so a staged crystal must still
                          * go to a different crystal furnace after
                          * save/load. Fresh world.sav size is unchanged
                          * when no fracture children are active.
                          * v54: world_seq added — monotonic u32 written
                          * after belt_seed for total ordering across
                          * worlds (newer-world-wins highscore policy).
                          * Pre-v54 saves migrate by defaulting world_seq=0,
                          * making them lose to any explicit world.
                          * v53 was the station receipt store:
                          * each station manifest entry now carries a
                          * receipt-chain payload inline
                          * ([len:u8] + len * cargo_receipt_t), matching
                          * ship manifest parity so station-held cargo can
                          * later dispatch by extending the exact incoming
                          * chain. v52 was the NPC hauler manifest tail:
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
                          * 3+ → 2×CRYSTAL+rest CUPRITE — and missing
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
 * rederivable from the station authority secret plus world seed or
 * saved outpost provenance and is deliberately NOT written to disk.
 * v39: Layer A.3 — per-player last_signed_nonce in the player save
 * (PLY6); world.sav layout itself was unchanged.
 * v38 added destroyed_rocks destroyed_at_ms timestamps (#285 slice 2). */
/* v31 widened inventory[] / base_price[] by one slot (REPAIR_KIT). v32
 * appends npc_ship_t.hull (a single float, version-gated read so v31
 * saves still load with default hull). MIN stays at 31 so we don't
 * wipe v31 worlds on this bump.
 *
 * v35: dropped station.named_ingots[64] + named_ingots_count + the
 * named_ingot_t struct. Those world saves are now below
 * MIN_SAVE_VERSION; the matching legacy layout remains only for accepted
 * PLY4 player saves. */
/* Bumped to 49: per-hopper commodity tag changes station_module_t
 * shape. Catalog files persist modules so they need re-bootstrap.
 * Per-player saves under saves/pubkey/ live in their own files and
 * are unaffected. */
#define MIN_SAVE_VERSION 49 /* v49 → v50 is layout-preserving (npc fields
                              * moved into embedded ship_t at the same
                              * byte offsets); read_npc lands them in
                              * n->ship->* identically for both versions. */

/* Legacy named-ingot block layout — preserved for accepted PLY4 player
 * saves. The original named_ingot_t was
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
               "legacy_named_ingot_t must match the accepted PLY4 layout");
#define LEGACY_SHIP_HOLD_INGOTS_MAX     8

typedef enum {
    LEGACY_SHIP_ASSET_OWNER_NONE = 0,
    LEGACY_SHIP_ASSET_OWNER_STATION = 1,
    LEGACY_SHIP_ASSET_OWNER_PLAYER_PUBKEY = 2,
    LEGACY_SHIP_ASSET_OWNER_PLAYER_SESSION = 3,
} legacy_ship_asset_owner_kind_t;

typedef struct {
    hull_class_t hull_class;
    int8_t owner;
    uint8_t _pad[3];
    float build_progress;
} legacy_pending_ship_build_v63_t;

typedef struct {
    hull_class_t hull_class;
    int8_t owner;
    uint8_t owner_kind;
    uint8_t _pad[2];
    float build_progress;
    uint8_t owner_pubkey[32];
    uint8_t owner_session[8];
} legacy_pending_ship_build_v65_t;

_Static_assert(sizeof(legacy_pending_ship_build_v63_t) == 12,
               "v63-v64 pending ship build layout changed");
_Static_assert(sizeof(legacy_pending_ship_build_v65_t) == 52,
               "v65-v78 pending ship build layout changed");

typedef struct {
    bool present;
    bool slot_only;
    int8_t owner_code;
    uint8_t owner_kind;
    uint8_t owner_pubkey[32];
    uint8_t owner_session[8];
} legacy_pending_ship_owner_evidence_t;

typedef struct {
    bool present;
    uint8_t owner_kind;
    int16_t owner_station;
    uint8_t owner_pubkey[32];
    uint8_t owner_session[8];
} legacy_ship_asset_owner_evidence_t;

typedef struct {
    bool present;
    bool module_bound;
    int8_t player_slot;
} legacy_cargo_pod_tow_evidence_t;

enum {
    OWNERSHIP_V79_STAGED_ROW_CAP =
        MAX_STATIONS * 4 * 2 + MAX_SHIP_ASSETS +
        MAX_CONTRACTS + MAX_DELIVERY_SHIPMENTS +
        MAX_CARGO_PODS,
    SHIP_BIRTH_ASSEMBLY_SAVE_CAP = MAX_STATIONS * 4,
    SHIP_BIRTH_ASSEMBLY_WIRE_SIZE =
        2 + 2 * (int)sizeof(float) +
        3 * (int)sizeof(float) + 3 * 32,
};

_Static_assert(SHIP_BIRTH_ASSEMBLY_WIRE_SIZE == 118,
               "v79 birth assembly row must stay explicit");

typedef struct {
    legacy_pending_ship_owner_evidence_t pending[MAX_STATIONS][4];
    legacy_ship_asset_owner_evidence_t assets[MAX_SHIP_ASSETS];
    bool legacy_contract_present[MAX_CONTRACTS];
    int16_t legacy_contract_actor_code[MAX_CONTRACTS];
    bool legacy_delivery_present[MAX_DELIVERY_SHIPMENTS];
    uint16_t legacy_delivery_actor_code[MAX_DELIVERY_SHIPMENTS];
    legacy_cargo_pod_tow_evidence_t
        cargo_pods[MAX_CARGO_PODS];
    ownership_quarantine_entry_t
        staged_rows[OWNERSHIP_V79_STAGED_ROW_CAP];
    size_t staged_row_count;
} ownership_v79_migration_t;

/* Set by world_load() before read_station() so per-station readers know
 * which version they're parsing and can handle field additions. */
static int g_loaded_save_version = SAVE_VERSION;
static int g_writing_save_version = SAVE_VERSION;
/* Exclusive end of the CRC-covered payload for the active world decode.
 * Count-driven readers use it to prove their complete variable-width section
 * exists before reserving dynamic storage. Restored after every load. */
static long g_loaded_save_payload_end = -1;

/* Current hauler capacity is 40 ingots; keep the on-disk corruption
 * guard comfortably above that while avoiding hostile giant receipt
 * allocations from malformed saves. */
_Static_assert(
    WORLD_SAVE_SHIP_MANIFEST_MAX_UNITS >= 512u,
    "world-save ship manifest guard must cover the reviewed hauler bound");

/* v51 cargo-in-space schema migration (Slice 1):
 * - Tag every untagged FURNACE (commodity == COMMODITY_COUNT) with an
 *   output ingot using a station-furnace-count heuristic for legacy
 *   layouts:
 *     1 furnace → FERRITE_INGOT
 *     2 furnaces → 1× FERRITE + 1× CUPRITE
 *     3+ furnaces → 2× CRYSTAL + rest CUPRITE
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
        return seen < 2 ? COMMODITY_CRYSTAL_INGOT
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
#define WRITE_FIELD(f, val) do { if (fwrite(&(val), sizeof(val), 1, (f)) != 1) return false; } while(0)
#define READ_FIELD(f, val)  do { if (fread(&(val), sizeof(val), 1, (f)) != 1) return false; } while(0)

enum {
    SAVE_V82_COMMODITY_COUNT = 10,
    SAVE_V82_MODULE_COUNT = 16,
};

static bool write_commodity_floats(FILE *f,
                                   const float values[COMMODITY_COUNT]) {
    size_t count = g_writing_save_version >= 83
        ? COMMODITY_COUNT : SAVE_V82_COMMODITY_COUNT;
    return fwrite(values, sizeof(float), count, f) == count;
}

static bool read_commodity_floats(FILE *f,
                                  float values[COMMODITY_COUNT]) {
    size_t count = g_loaded_save_version >= 83
        ? COMMODITY_COUNT : SAVE_V82_COMMODITY_COUNT;
    memset(values, 0, sizeof(float) * COMMODITY_COUNT);
    return fread(values, sizeof(float), count, f) == count;
}

static int save_module_slot_count_for_version(int version) {
    return version >= 83 ? MAX_MODULES_PER_STATION : SAVE_V82_MODULE_COUNT;
}

static bool world_load_payload_has_bytes(FILE *f, size_t byte_count) {
    if (!f || g_loaded_save_payload_end < 0) return false;
    long offset = ftell(f);
    if (offset < 0 || offset > g_loaded_save_payload_end) return false;
    return (uint64_t)byte_count <=
           (uint64_t)(g_loaded_save_payload_end - offset);
}

/*
 * Validate a complete variable-width manifest section before its count can
 * drive manifest/receipt allocations. The second decode pass still performs
 * all semantic validation and production insertion; this pass only proves
 * that every declared row and receipt chain is contained in the CRC-covered
 * payload.
 */
static bool world_load_preflight_manifest(
    FILE *f,
    uint16_t count,
    bool has_receipt_chains) {
    if (!f) return false;
    long start = ftell(f);
    if (start < 0) return false;

    bool ok = true;
    for (uint16_t i = 0; i < count && ok; i++) {
        size_t fixed_bytes = sizeof(cargo_unit_t) +
            (has_receipt_chains ? sizeof(uint8_t) : 0u);
        if (!world_load_payload_has_bytes(f, fixed_bytes) ||
            fseek(f, (long)sizeof(cargo_unit_t), SEEK_CUR) != 0) {
            ok = false;
            break;
        }
        if (!has_receipt_chains) continue;

        uint8_t chain_len = 0;
        if (fread(&chain_len, sizeof(chain_len), 1, f) != 1 ||
            chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
            ok = false;
            break;
        }
        size_t receipt_bytes =
            (size_t)chain_len * sizeof(cargo_receipt_t);
        if (!world_load_payload_has_bytes(f, receipt_bytes) ||
            (receipt_bytes > 0 &&
             fseek(f, (long)receipt_bytes, SEEK_CUR) != 0)) {
            ok = false;
        }
    }

    if (fseek(f, start, SEEK_SET) != 0) return false;
    return ok;
}

static bool world_load_record_oom(world_load_result_t *result) {
    if (result) *result = WORLD_LOAD_RESULT_OUT_OF_MEMORY;
    return false;
}

static bool write_actor_principal(
    FILE *f,
    const actor_principal_t *principal) {
    uint8_t wire[ACTOR_PRINCIPAL_WIRE_SIZE];
    return f && actor_principal_pack(principal, wire) &&
           fwrite(wire, sizeof(wire), 1, f) == 1;
}

static bool read_actor_principal(
    FILE *f,
    actor_principal_t *principal) {
    uint8_t wire[ACTOR_PRINCIPAL_WIRE_SIZE];
    if (!f || !principal ||
        fread(wire, sizeof(wire), 1, f) != 1) {
        if (principal) *principal = actor_principal_none();
        return false;
    }
    return actor_principal_unpack(wire, principal);
}

static bool save_bytes_any(const uint8_t *bytes, size_t len) {
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < len; i++) any |= bytes[i];
    return any != 0;
}

static bool ship_asset_birth_proof_is_canonical(
    const ship_asset_t *asset) {
    if (!asset) return false;
    bool grades = save_bytes_any(
        asset->birth_fragment_grades,
        sizeof(asset->birth_fragment_grades));
    bool soul = save_bytes_any(
        asset->birth_soul_pub, sizeof(asset->birth_soul_pub));
    bool material = save_bytes_any(
        asset->birth_material_root,
        sizeof(asset->birth_material_root));
    bool fragment[3] = {
        save_bytes_any(asset->birth_fragment_pubs[0], 32),
        save_bytes_any(asset->birth_fragment_pubs[1], 32),
        save_bytes_any(asset->birth_fragment_pubs[2], 32),
    };
    if (asset->provenance == SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY) {
        uint8_t expected_soul[32];
        uint8_t expected_material[32];
        if (asset->birth_proof_version !=
                SHIP_BIRTH_PROOF_VERSION_V1 ||
            !soul || !material ||
            !ship_birth_proof_compute_v1(
                asset->birth_fragment_pubs,
                asset->birth_fragment_grades,
                expected_soul, expected_material)) {
            return false;
        }
        return memcmp(
                   asset->birth_soul_pub,
                   expected_soul, sizeof(expected_soul)) == 0 &&
               memcmp(
                   asset->birth_material_root,
                   expected_material,
                   sizeof(expected_material)) == 0;
    }
    return asset->birth_proof_version == 0 &&
           !grades && !soul && !material &&
           !fragment[0] && !fragment[1] && !fragment[2];
}

static bool validate_ship_birth_provenance_uniqueness(
    const world_t *w) {
    if (!w) return false;
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w->ship_assets[i];
        if (!asset->active) continue;
        if (!ship_asset_birth_proof_is_canonical(asset))
            return false;
        if (asset->provenance !=
            SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY) {
            continue;
        }
        for (int j = i + 1; j < MAX_SHIP_ASSETS; j++) {
            const ship_asset_t *other = &w->ship_assets[j];
            if (!other->active ||
                other->provenance !=
                    SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY) {
                continue;
            }
            if (memcmp(asset->birth_soul_pub,
                       other->birth_soul_pub, 32) == 0 ||
                memcmp(asset->birth_material_root,
                       other->birth_material_root, 32) == 0) {
                return false;
            }
            for (int f = 0;
                 f < SHIP_BIRTH_PROOF_FRAGMENT_COUNT; f++) {
                for (int other_f = 0;
                     other_f < SHIP_BIRTH_PROOF_FRAGMENT_COUNT;
                     other_f++) {
                    if (memcmp(
                            asset->birth_fragment_pubs[f],
                            other->birth_fragment_pubs[other_f],
                            32) == 0) {
                        return false;
                    }
                }
            }
        }
        for (int station = 0; station < MAX_STATIONS; station++) {
            for (int row = 0; row < 4; row++) {
                const ship_birth_assembly_t *birth =
                    &w->ship_birth_assemblies[station][row];
                if (!birth->active) continue;
                for (int f = 0;
                     f < SHIP_BIRTH_PROOF_FRAGMENT_COUNT; f++) {
                    for (int birth_f = 0;
                         birth_f < SHIP_BIRTH_PROOF_FRAGMENT_COUNT;
                         birth_f++) {
                        if (memcmp(
                                asset->birth_fragment_pubs[f],
                                birth->fragment_pubs[birth_f],
                                32) == 0) {
                            return false;
                        }
                    }
                }
            }
        }
    }
    for (int station = 0; station < MAX_STATIONS; station++) {
        for (int row = 0; row < 4; row++) {
            const ship_birth_assembly_t *birth =
                &w->ship_birth_assemblies[station][row];
            if (!birth->active) continue;
            int key = station * 4 + row;
            for (int other_station = station;
                 other_station < MAX_STATIONS;
                 other_station++) {
                for (int other_row = 0; other_row < 4; other_row++) {
                    int other_key =
                        other_station * 4 + other_row;
                    if (other_key <= key) continue;
                    const ship_birth_assembly_t *other =
                        &w->ship_birth_assemblies[
                            other_station][other_row];
                    if (!other->active) continue;
                    for (int f = 0;
                         f < SHIP_BIRTH_PROOF_FRAGMENT_COUNT; f++) {
                        for (int other_f = 0;
                             other_f <
                                 SHIP_BIRTH_PROOF_FRAGMENT_COUNT;
                             other_f++) {
                            if (memcmp(
                                    birth->fragment_pubs[f],
                                    other->fragment_pubs[other_f],
                                    32) == 0) {
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

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
        /* v23 and earlier wrote the original 16-byte module record as a
         * blob. Keep that exact disk shape even though runtime state now
         * lives beside the identity in station_module_t. */
        struct legacy_station_module_v23 {
            module_type_t type;
            uint8_t ring;
            uint8_t slot;
            uint8_t scaffold;
            uint8_t last_smelt_commodity;
            uint8_t commodity;
            uint8_t pad[2];
            float build_progress;
        } disk_module;
        _Static_assert(sizeof(disk_module) == 16,
                       "legacy station module disk layout changed");
        READ_FIELD(f, disk_module);
        station_module_t *module = &s->modules[m];
        memset(module, 0, sizeof(*module));
        module->type = disk_module.type;
        module->ring = disk_module.ring;
        module->slot = disk_module.slot;
        module->scaffold = disk_module.scaffold != 0;
        module->last_smelt_commodity = disk_module.last_smelt_commodity;
        module->commodity = disk_module.commodity;
        module->build_progress = disk_module.build_progress;
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
    /* v20: single module_buffer[] → migrate to per-module input_buffer.
     * v21+: explicit input + output. */
    for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
        READ_FIELD(f, s->modules[m].input_buffer);
    }
    if (g_loaded_save_version >= 21) {
        for (int m = 0; m < MAX_MODULES_PER_STATION; m++) {
            READ_FIELD(f, s->modules[m].output_buffer);
        }
    } else {
        /* v20: no output buffers — initialize to 0 */
        for (int m = 0; m < MAX_MODULES_PER_STATION; m++)
            s->modules[m].output_buffer = 0.0f;
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
    /* Raw hopper inventory plus sub-unit finished-production residue. */
    if (!write_commodity_floats(f, s->_inventory_cache) ||
        !write_commodity_floats(f, s->_finished_residue)) return false;
    /* Per-module production buffers */
    for (int m = 0; m < save_module_slot_count_for_version(
                            g_writing_save_version); m++)
        WRITE_FIELD(f, s->modules[m].input_buffer);
    for (int m = 0; m < save_module_slot_count_for_version(
                            g_writing_save_version); m++)
        WRITE_FIELD(f, s->modules[m].output_buffer);
    /* (credit_pool field removed in v43 — derived from ledger now.) */
    /* Economy ledger */
    WRITE_FIELD(f, s->ledger_count);
    for (int i = 0; i < STATION_LEDGER_MAX; i++) {
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
    if (s->pending_ship_build_count < 0 ||
        s->pending_ship_build_count > 4) {
        return false;
    }
    WRITE_FIELD(f, s->pending_ship_build_count);
    for (int p = 0; p < 4; p++) {
        pending_ship_build_t empty = {0};
        const pending_ship_build_t *build =
            p < s->pending_ship_build_count
                ? &s->pending_ship_builds[p]
                : &empty;
        if ((unsigned)build->hull_class >= HULL_CLASS_COUNT ||
            !actor_principal_is_canonical(
                &build->owner_principal) ||
            !isfinite(build->build_progress) ||
            build->build_progress < 0.0f ||
            build->build_progress > 1.0f ||
            build->mode >= PENDING_SHIP_BUILD_MODE_COUNT) {
            return false;
        }
        WRITE_FIELD(f, build->hull_class);
        if (!write_actor_principal(
                f, &build->owner_principal)) {
            return false;
        }
        WRITE_FIELD(f, build->owner_quarantine_record_id);
        WRITE_FIELD(f, build->mode_quarantine_record_id);
        WRITE_FIELD(f, build->build_progress);
        WRITE_FIELD(f, build->mode);
    }
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
     * longer written; those world versions are below MIN_SAVE_VERSION. */
    /* Manifest (v29+, #339 slice A). Previously guarded to require
     * empty — now serialized as count + packed cargo_unit_t entries.
     * cap is NOT persisted; on load the manifest bootstraps at the
     * default capacity and grows as needed. */
    {
        uint16_t manifest_count = s->manifest.count;
        const ship_receipts_t *rcpts = station_get_receipts_const(s);
        WRITE_FIELD(f, manifest_count);
        for (uint16_t u = 0; u < manifest_count; u++) {
            WRITE_FIELD(f, s->manifest.units[u]);
            uint8_t len = 0;
            if (rcpts && u < rcpts->count) {
                len = rcpts->chains[u].len;
                if (len > CARGO_RECEIPT_CHAIN_MAX_LEN)
                    len = CARGO_RECEIPT_CHAIN_MAX_LEN;
            }
            WRITE_FIELD(f, len);
            for (uint8_t k = 0; k < len; k++) {
                const cargo_receipt_t *r = &rcpts->chains[u].links[k];
                if (fwrite(r, sizeof(*r), 1, f) != 1) {
                    return false;
                }
            }
        }
    }
    /* v40: per-station Ed25519 pubkey (#479 B) + outpost provenance
     * (founder pubkey + name + planted tick) so the matching private
     * key is rederivable on load without ever being persisted. The
     * 64-byte station_secret is deliberately omitted. The name is
     * written here (in addition to the catalog) so outpost identity
     * stays self-contained in world.sav — saves loaded without the
     * matching catalog still rederive a working keypair. */
    if (fwrite(s->station_pubkey, 32, 1, f) != 1) return false;
    if (fwrite(s->outpost_founder_pubkey, 32, 1, f) != 1) return false;
    WRITE_FIELD(f, s->outpost_planted_tick);
    WRITE_FIELD(f, s->id);
    if (fwrite(s->station_actor_id,
               sizeof(s->station_actor_id), 1, f) != 1) {
        return false;
    }
    WRITE_FIELD(f, s->name);
    WRITE_FIELD(f, s->faction_id);
    WRITE_FIELD(f, s->faction_allegiance);
    WRITE_FIELD(f, s->faction_ideology);
    WRITE_FIELD(f, s->faction_relations);
    /*
     * v77: fixed-width public station-authority lifecycle and trust history.
     * Validation makes the persisted representation canonical. The
     * station_secret field remains deliberately absent.
     */
    if (!station_authority_registry_validate(s)) return false;
    WRITE_FIELD(f, s->authority_registry_version);
    WRITE_FIELD(f, s->authority_registry_count);
    WRITE_FIELD(f, s->authority_registry_pad);
    for (int i = 0; i < STATION_AUTHORITY_REGISTRY_CAP; i++) {
        if (fwrite(s->authority_registry[i].pubkey, 32, 1, f) != 1)
            return false;
        WRITE_FIELD(f, s->authority_registry[i].lifecycle);
        WRITE_FIELD(f, s->authority_registry[i].trust);
        WRITE_FIELD(f, s->authority_registry[i]._pad);
    }
    /* v41: Layer C of #479 — chain log state. The actual events live in
     * side files under chain/<base58(pubkey)>.log; only the
     * continuation pointers (last full-record hash + monotonic event
     * counter) ride along with the world save so a restart can pick
     * up the chain without re-reading + re-hashing the entire log. */
    if (fwrite(s->chain_last_hash, 32, 1, f) != 1) return false;
    WRITE_FIELD(f, s->chain_event_count);
    return true;
}

static bool read_station_session(
    FILE *f,
    station_t *s,
    ownership_v79_migration_t *migration,
    int station_index,
    world_load_result_t *load_result) {
    /* Inventory */
    if (!read_commodity_floats(f, s->_inventory_cache)) return false;
    if (g_loaded_save_version >= 75)
        { if (!read_commodity_floats(f, s->_finished_residue)) return false; }
    else
        memset(s->_finished_residue, 0, sizeof(s->_finished_residue));
    /* Per-module production buffers */
    for (int m = 0; m < save_module_slot_count_for_version(
                            g_loaded_save_version); m++)
        READ_FIELD(f, s->modules[m].input_buffer);
    for (int m = 0; m < save_module_slot_count_for_version(
                            g_loaded_save_version); m++)
        READ_FIELD(f, s->modules[m].output_buffer);
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
    int disk_ledger_slots = (g_loaded_save_version >= 62) ? STATION_LEDGER_MAX : 16;
    if (s->ledger_count > disk_ledger_slots) s->ledger_count = disk_ledger_slots;
    for (int i = 0; i < disk_ledger_slots; i++) {
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
    for (int i = disk_ledger_slots; i < STATION_LEDGER_MAX; i++) {
        memset(&s->ledger[i], 0, sizeof(s->ledger[i]));
    }
    ledger_sanitize_station(s);
    /* Shipyard queue */
    READ_FIELD(f, s->pending_scaffold_count);
    if (s->pending_scaffold_count < 0) s->pending_scaffold_count = 0;
    if (s->pending_scaffold_count > 4) s->pending_scaffold_count = 4;
    for (int p = 0; p < 4; p++)
        READ_FIELD(f, s->pending_scaffolds[p]);
    if (g_loaded_save_version >= 63) {
        READ_FIELD(f, s->pending_ship_build_count);
        if (s->pending_ship_build_count < 0 ||
            s->pending_ship_build_count > 4) {
            return false;
        }
        for (int p = 0; p < 4; p++) {
            pending_ship_build_t decoded = {0};
            legacy_pending_ship_owner_evidence_t evidence = {0};
            if (g_loaded_save_version >= 79) {
                READ_FIELD(f, decoded.hull_class);
                if (!read_actor_principal(
                        f, &decoded.owner_principal)) {
                    return false;
                }
                READ_FIELD(
                    f, decoded.owner_quarantine_record_id);
                READ_FIELD(
                    f, decoded.mode_quarantine_record_id);
                READ_FIELD(f, decoded.build_progress);
                READ_FIELD(f, decoded.mode);
            } else if (g_loaded_save_version >= 65) {
                legacy_pending_ship_build_v65_t legacy = {0};
                READ_FIELD(f, legacy);
                decoded.hull_class = legacy.hull_class;
                decoded.build_progress = legacy.build_progress;
                decoded.mode = g_loaded_save_version >= 73
                    ? PENDING_SHIP_BUILD_MODE_UNKNOWN
                    : PENDING_SHIP_BUILD_MODE_MATERIAL;
                evidence.present = p < s->pending_ship_build_count;
                evidence.owner_code = legacy.owner;
                evidence.owner_kind = legacy.owner_kind;
                memcpy(evidence.owner_pubkey, legacy.owner_pubkey,
                       sizeof(evidence.owner_pubkey));
                memcpy(evidence.owner_session, legacy.owner_session,
                       sizeof(evidence.owner_session));
            } else {
                legacy_pending_ship_build_v63_t legacy = {0};
                READ_FIELD(f, legacy);
                decoded.hull_class = legacy.hull_class;
                decoded.build_progress = legacy.build_progress;
                decoded.mode = PENDING_SHIP_BUILD_MODE_MATERIAL;
                evidence.present = p < s->pending_ship_build_count;
                evidence.slot_only = true;
                evidence.owner_code = legacy.owner;
            }
            if (p >= s->pending_ship_build_count) {
                memset(&s->pending_ship_builds[p], 0,
                       sizeof(s->pending_ship_builds[p]));
                continue;
            }
            if ((unsigned)decoded.hull_class >= HULL_CLASS_COUNT ||
                !actor_principal_is_canonical(
                    &decoded.owner_principal) ||
                !isfinite(decoded.build_progress) ||
                decoded.build_progress < 0.0f ||
                decoded.build_progress > 1.0f ||
                decoded.mode >= PENDING_SHIP_BUILD_MODE_COUNT) {
                return false;
            }
            s->pending_ship_builds[p] = decoded;
            if (migration && g_loaded_save_version < 79 &&
                station_index >= 0 &&
                station_index < MAX_STATIONS) {
                migration->pending[station_index][p] = evidence;
            }
        }
    } else {
        s->pending_ship_build_count = 0;
        memset(s->pending_ship_builds, 0, sizeof(s->pending_ship_builds));
    }
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
    /* Every accepted world save (v49+) already uses the manifest layout.
     * The v26-v34 station named-ingot block and pre-v29 float migration are
     * unreachable below MIN_SAVE_VERSION and have been retired. */
    uint16_t manifest_count = 0;
    READ_FIELD(f, manifest_count);
    if (!world_load_preflight_manifest(
            f, manifest_count,
            g_loaded_save_version >= 53)) {
        return false;
    }
    if (!station_manifest_bootstrap(s))
        return world_load_record_oom(load_result);
    ship_receipts_t *rcpts = station_get_receipts(s);
    if (!rcpts) return false;
    manifest_clear(&s->manifest);
    ship_receipts_clear(rcpts);
    if (manifest_count > 0) {
        if (!manifest_reserve(&s->manifest, manifest_count))
            return world_load_record_oom(load_result);
        if (!ship_receipts_reserve(rcpts, manifest_count))
            return world_load_record_oom(load_result);
        for (uint16_t u = 0; u < manifest_count; u++) {
            cargo_unit_t unit = {0};
            cargo_receipt_chain_t chain = {0};
            READ_FIELD(f, unit);
            if (g_loaded_save_version >= 53) {
                READ_FIELD(f, chain.len);
                if (chain.len > CARGO_RECEIPT_CHAIN_MAX_LEN)
                    return false;
                for (uint8_t k = 0; k < chain.len; k++) {
                    if (fread(&chain.links[k], sizeof(chain.links[k]), 1, f) != 1) {
                        return false;
                    }
                }
            }
            if (!station_manifest_push_with_chain(
                    s, &unit, chain.len > 0 ? &chain : NULL)) {
                return false;
            }
        }
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
        if (g_loaded_save_version >= 79) {
            uint32_t saved_id = 0;
            uint8_t saved_actor[ACTOR_PRINCIPAL_ID_SIZE] = {0};
            uint8_t saved_actor_any = 0;
            READ_FIELD(f, saved_id);
            if (fread(saved_actor, sizeof(saved_actor), 1, f) != 1) {
                return false;
            }
            for (size_t i = 0; i < sizeof(saved_actor); i++) {
                saved_actor_any |= saved_actor[i];
            }
            bool saved_station_occupied =
                save_bytes_any(
                    s->station_pubkey,
                    sizeof(s->station_pubkey));
            bool saved_actor_pair =
                saved_id != 0 && saved_actor_any != 0;
            if (saved_station_occupied != saved_actor_pair) {
                return false;
            }
            if (s->station_actor_catalog_attested &&
                (saved_id != s->id ||
                 memcmp(saved_actor, s->station_actor_id,
                        sizeof(saved_actor)) != 0)) {
                return false;
            }
            s->id = saved_id;
            memcpy(s->station_actor_id, saved_actor,
                   sizeof(s->station_actor_id));
        }
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
        if (g_loaded_save_version >= 66) {
            READ_FIELD(f, s->faction_id);
            READ_FIELD(f, s->faction_allegiance);
            READ_FIELD(f, s->faction_ideology);
            READ_FIELD(f, s->faction_relations);
            if (s->faction_id >= (uint8_t)STATION_FACTION_COUNT)
                s->faction_id = (uint8_t)STATION_FACTION_UNALIGNED;
            if (s->faction_allegiance >= (uint8_t)STATION_FACTION_COUNT)
                s->faction_allegiance = s->faction_id;
            if (s->faction_ideology >= (uint8_t)STATION_IDEOLOGY_COUNT)
                s->faction_ideology = (uint8_t)STATION_IDEOLOGY_PRAGMATIC;
        }
    } else {
        memset(s->station_pubkey, 0, sizeof(s->station_pubkey));
        memset(s->outpost_founder_pubkey, 0, sizeof(s->outpost_founder_pubkey));
        s->outpost_planted_tick = 0;
    }
    if (g_loaded_save_version >= 77) {
        READ_FIELD(f, s->authority_registry_version);
        READ_FIELD(f, s->authority_registry_count);
        READ_FIELD(f, s->authority_registry_pad);
        for (int i = 0; i < STATION_AUTHORITY_REGISTRY_CAP; i++) {
            if (fread(s->authority_registry[i].pubkey, 32, 1, f) != 1)
                return false;
            READ_FIELD(f, s->authority_registry[i].lifecycle);
            READ_FIELD(f, s->authority_registry[i].trust);
            READ_FIELD(f, s->authority_registry[i]._pad);
        }
        if (!station_authority_registry_validate(s)) return false;
    } else {
        /*
         * Older saves know only the saved live key. Preserve it as current,
         * but never infer historical identities or trust.
         */
        station_authority_registry_init(s);
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
    chain_log_health_set(s, CHAIN_HEALTH_UNKNOWN, false, 0, NULL,
                         "chain not verified this boot");
    /* Migrate the retired finished slots. Old saves used them as a mirror;
     * v75+ stores only raw hopper amounts there. Keep real manifest units,
     * synthesize a missing whole-unit tail for old saves, and move the
     * fractional production residue into its dedicated component. */
    {
        uint8_t origin[8] = { 'R','E','P','A','I','R','v','1' };
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
            commodity_t commodity = (commodity_t)c;
            float legacy_amount = s->_inventory_cache[c];
            s->_inventory_cache[c] = 0.0f;
            if (g_loaded_save_version < 75) {
                if (!isfinite(legacy_amount) || legacy_amount < 0.0f)
                    legacy_amount = 0.0f;
                if (legacy_amount > 1000000.0f)
                    legacy_amount = 1000000.0f;
                int cache_units = (int)floorf(legacy_amount + 0.0001f);
                float residue = legacy_amount - (float)cache_units;
                if (residue < 0.0f || residue >= 1.0f) residue = 0.0f;
                s->_finished_residue[c] = residue;
                int manifest_units = manifest_count_by_commodity(&s->manifest,
                                                                 commodity);
                int missing = cache_units - manifest_units;
                if (missing > 0)
                    (void)station_finished_mint(s, commodity, missing, origin);
            } else if (s->_finished_residue[c] < 0.0f ||
                       s->_finished_residue[c] >= 1.0f) {
                s->_finished_residue[c] = 0.0f;
            }
        }
    }
    /* station_secret is rederived by the world loader, not persisted. */
    signal_memzero_explicit(s->station_secret,
                            sizeof(s->station_secret));
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
    WRITE_FIELD(f, a->thrown_by_token);
    WRITE_FIELD(f, a->thrown_timer_q);
    WRITE_FIELD(f, a->last_fractured_token);
    WRITE_FIELD(f, a->fracture_seed);
    WRITE_FIELD(f, a->fragment_pub);
    WRITE_FIELD(f, a->grade);
    WRITE_FIELD(f, a->crystal_stage);
    WRITE_FIELD(f, a->crystal_stage_station);
    WRITE_FIELD(f, a->crystal_stage_module);
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
    if (a->active) return false;
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
    if (g_loaded_save_version >= 60) {
        READ_FIELD(f, a->thrown_by_token);
        READ_FIELD(f, a->thrown_timer_q);
    } else {
        memset(a->thrown_by_token, 0, sizeof(a->thrown_by_token));
        a->thrown_timer_q = 0;
    }
    READ_FIELD(f, a->last_fractured_token);
    READ_FIELD(f, a->fracture_seed);
    READ_FIELD(f, a->fragment_pub);
    READ_FIELD(f, a->grade);
    if (g_loaded_save_version >= 55) {
        READ_FIELD(f, a->crystal_stage);
        READ_FIELD(f, a->crystal_stage_station);
        READ_FIELD(f, a->crystal_stage_module);
    } else {
        a->crystal_stage = CRYSTAL_STAGE_RAW;
        a->crystal_stage_station = 0xFFu;
        a->crystal_stage_module = 0xFFu;
    }
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

static bool write_ship_birth_assemblies(FILE *f, const world_t *w) {
    if (!f || !w) return false;
    if (!world_ship_birth_saved_assemblies_valid(w)) return false;
    uint16_t count = 0;
    for (int station = 0; station < MAX_STATIONS; station++) {
        const station_t *st = &w->stations[station];
        if (st->pending_ship_build_count < 0 ||
            st->pending_ship_build_count > 4) {
            return false;
        }
        for (int row = 0; row < 4; row++) {
            const ship_birth_assembly_t *birth =
                &w->ship_birth_assemblies[station][row];
            bool expects =
                row < st->pending_ship_build_count &&
                st->pending_ship_builds[row].mode ==
                    PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY;
            if (birth->active && !expects) return false;
            if (expects && !birth->active &&
                st->pending_ship_builds[row].build_progress != 0.0f) {
                return false;
            }
            if (!birth->active) continue;
            if (count == UINT16_MAX) {
                return false;
            }
            count++;
        }
    }
    uint8_t count_wire[2];
    wire_write_u16_le(count_wire, count);
    if (fwrite(count_wire, sizeof(count_wire), 1, f) != 1) {
        return false;
    }
    for (int station = 0; station < MAX_STATIONS; station++) {
        for (int row = 0; row < 4; row++) {
            const ship_birth_assembly_t *birth =
                &w->ship_birth_assemblies[station][row];
            if (!birth->active) continue;
            uint8_t wire[SHIP_BIRTH_ASSEMBLY_WIRE_SIZE] = {0};
            wire_writer_t writer =
                wire_writer_init(wire, sizeof(wire));
            if (!wire_writer_reserve(&writer, 2)) return false;
            wire[writer.offset++] = (uint8_t)station;
            wire[writer.offset++] = (uint8_t)row;
            wire_put_f32(&writer, birth->target.x);
            wire_put_f32(&writer, birth->target.y);
            for (int i = 0; i < 3; i++)
                wire_put_f32(&writer, birth->start_dist[i]);
            if (!wire_writer_reserve(
                    &writer, sizeof(birth->fragment_pubs))) {
                return false;
            }
            memcpy(wire + writer.offset, birth->fragment_pubs,
                   sizeof(birth->fragment_pubs));
            writer.offset += sizeof(birth->fragment_pubs);
            if (!writer.ok || writer.offset != sizeof(wire) ||
                fwrite(wire, sizeof(wire), 1, f) != 1) {
                return false;
            }
        }
    }
    return true;
}

static bool read_ship_birth_assemblies(FILE *f, world_t *w) {
    if (!f || !w) return false;
    memset(w->ship_birth_assemblies, 0,
           sizeof(w->ship_birth_assemblies));
    uint8_t count_wire[2];
    if (fread(count_wire, sizeof(count_wire), 1, f) != 1) {
        return false;
    }
    uint16_t count = wire_read_u16_le(count_wire);
    if (count > SHIP_BIRTH_ASSEMBLY_SAVE_CAP) return false;
    int previous_key = -1;
    for (uint16_t record = 0; record < count; record++) {
        uint8_t wire[SHIP_BIRTH_ASSEMBLY_WIRE_SIZE];
        if (fread(wire, sizeof(wire), 1, f) != 1) return false;
        wire_reader_t reader = wire_reader_init(wire, sizeof(wire));
        if (!wire_reader_require(&reader, 2)) return false;
        int station = wire[reader.offset++];
        int row = wire[reader.offset++];
        int key = station * 4 + row;
        if (station < 0 || station >= MAX_STATIONS ||
            row < 0 || row >= 4 ||
            key <= previous_key) {
            return false;
        }
        previous_key = key;
        station_t *st = &w->stations[station];
        if (row >= st->pending_ship_build_count ||
            st->pending_ship_builds[row].mode !=
                PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY) {
            return false;
        }
        ship_birth_assembly_t *birth =
            &w->ship_birth_assemblies[station][row];
        birth->active = true;
        birth->target.x = wire_get_f32(&reader);
        birth->target.y = wire_get_f32(&reader);
        for (int i = 0; i < 3; i++)
            birth->start_dist[i] = wire_get_f32(&reader);
        if (!wire_reader_require(
                &reader, sizeof(birth->fragment_pubs))) {
            return false;
        }
        memcpy(birth->fragment_pubs, wire + reader.offset,
               sizeof(birth->fragment_pubs));
        reader.offset += sizeof(birth->fragment_pubs);
        for (int i = 0; i < 3; i++) birth->fragment_slots[i] = -1;
        birth->age = 0.0f;
        if (!reader.ok || reader.offset != sizeof(wire)) {
            return false;
        }
    }
    for (int station = 0; station < MAX_STATIONS; station++) {
        const station_t *st = &w->stations[station];
        for (int row = 0; row < 4; row++) {
            bool expects =
                row < st->pending_ship_build_count &&
                st->pending_ship_builds[row].mode ==
                    PENDING_SHIP_BUILD_MODE_BIRTH_ASSEMBLY;
            bool active =
                w->ship_birth_assemblies[station][row].active;
            if (active && !expects) {
                return false;
            }
            if (expects && !active &&
                st->pending_ship_builds[row].build_progress != 0.0f) {
                return false;
            }
        }
    }
    /*
     * Resolve fragment slots only after persisted NPC state is decoded and
     * transient player tow state has been cleared. Doing it here would make
     * acceptance depend on whatever runtime happened to occupy the
     * destination world before load.
     */
    return true;
}

/* ---- npc_ship field-by-field I/O ----
 *
 * v50: pos/vel/angle/hull_class moved into the embedded ship_t. The
 * on-disk record format is unchanged (same field order, same widths) —
 * the read path just lands the bytes in n->ship->* and the write path
 * reads them back from there. v49 saves load identically since the
 * struct layout used to put pos/vel/angle/hull_class right where the
 * embedded ship_t now lives.
 */
static bool write_npc(FILE *f, const npc_ship_t *n) {
    ship_t empty_ship = {0};
    npc_ship_t empty_npc = {0};
    float cargo_snapshot[COMMODITY_COUNT];
    if (!n) {
        empty_npc.ship = &empty_ship;
        n = &empty_npc;
    } else if (!n->ship) {
        empty_npc = *n;
        empty_npc.ship = &empty_ship;
        n = &empty_npc;
    }
    WRITE_FIELD(f, n->active);
    WRITE_FIELD(f, n->role);
    WRITE_FIELD(f, n->ship->hull_class);
    WRITE_FIELD(f, n->state);
    WRITE_FIELD(f, n->ship->pos);
    WRITE_FIELD(f, n->ship->vel);
    WRITE_FIELD(f, n->ship->angle);
    ship_cargo_snapshot(n->ship, cargo_snapshot);
    if (!write_commodity_floats(f, cargo_snapshot)) return false;
    WRITE_FIELD(f, n->target_asteroid);
    WRITE_FIELD(f, n->home_station);
    WRITE_FIELD(f, n->dest_station);
    WRITE_FIELD(f, n->state_timer);
    WRITE_FIELD(f, n->thrusting);
    WRITE_FIELD(f, n->tint_r);
    WRITE_FIELD(f, n->tint_g);
    WRITE_FIELD(f, n->tint_b);
    WRITE_FIELD(f, n->ship->hull); /* v32+; authoritative ship component */
    WRITE_FIELD(f, n->session_token); /* v33+ */
    WRITE_FIELD(f, n->ship_asset_id); /* v64+ */
    return true;
}

static bool read_npc(FILE *f, npc_ship_t *n) {
    READ_FIELD(f, n->active);
    READ_FIELD(f, n->role);
    READ_FIELD(f, n->ship->hull_class);
    READ_FIELD(f, n->state);
    READ_FIELD(f, n->ship->pos);
    READ_FIELD(f, n->ship->vel);
    READ_FIELD(f, n->ship->angle);
    if (!read_commodity_floats(f, n->ship->cargo)) return false;
    READ_FIELD(f, n->target_asteroid);
    READ_FIELD(f, n->home_station);
    READ_FIELD(f, n->dest_station);
    READ_FIELD(f, n->state_timer);
    READ_FIELD(f, n->thrusting);
    READ_FIELD(f, n->tint_r);
    READ_FIELD(f, n->tint_g);
    READ_FIELD(f, n->tint_b);
    if (g_loaded_save_version >= 32) {
        READ_FIELD(f, n->ship->hull);
    } else {
        n->ship->hull = npc_max_hull(n);
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
    if (g_loaded_save_version >= 64) {
        READ_FIELD(f, n->ship_asset_id);
    } else {
        n->ship_asset_id = SHIP_ASSET_ID_NONE;
    }
    n->pickup_station = -1;
    n->pickup_commodity = COMMODITY_COUNT;
    n->pickup_action = (uint8_t)CONTRACT_TRACTOR;
    n->hnn_market_station = 0xffu;
    n->hnn_market_decay_tick = 0;
    n->hnn_experience_station = 0xffu;
    n->hnn_experience_uploaded_station = 0xffu;
    n->hnn_experience_uploaded_source_station = 0xffu;
    /* Validate after the full record is read so the file pointer is
     * always past this NPC's bytes. An active slot with garbage role
     * used to crashloop the server on first sim step (despawn check
     * fired with an invalid role through character_free_for_npc).
     * Drop the slot quietly; the spawn loop will refill it. */
    if (n->active && ((int)n->role < 0 || (int)n->role > (int)NPC_ROLE_TOW)) {
        n->active = false;
    }
    return true;
}

static const ship_t *world_save_npc_ship_for(const world_t *w, int npc_slot) {
    if (!w || npc_slot < 0 || npc_slot >= MAX_NPC_SHIPS) return NULL;
    if (!w->npc_ships[npc_slot].active) return NULL;
    return w->npc_ships[npc_slot].ship;
}

static void ship_retire_finished_cargo_slots(ship_t *ship) {
    if (!ship) return;
    for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++)
        ship->cargo[c] = 0.0f;
}

static bool write_npc_ship_manifest_payload(FILE *f, const ship_t *ship) {
    uint16_t count = 0;
    if (ship && ship->manifest.units) count = ship->manifest.count;
    if (count > WORLD_SAVE_SHIP_MANIFEST_MAX_UNITS) return false;
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

static bool read_npc_ship_manifest_payload(
    FILE *f,
    ship_t *ship,
    world_load_result_t *load_result) {
    uint16_t count = 0;
    if (fread(&count, sizeof(count), 1, f) != 1) return false;
    if (count > WORLD_SAVE_SHIP_MANIFEST_MAX_UNITS ||
        !world_load_preflight_manifest(f, count, true)) {
        return false;
    }

    ship_receipts_t *rcpts = NULL;
    if (ship) {
        if (!ship_manifest_bootstrap(ship))
            return world_load_record_oom(load_result);
        manifest_clear(&ship->manifest);
        rcpts = ship_get_receipts(ship);
        if (!rcpts) return false;
        ship_receipts_clear(rcpts);
        if (count > ship->manifest.cap &&
            !manifest_reserve(&ship->manifest, count)) {
            return world_load_record_oom(load_result);
        }
        if (count > rcpts->cap &&
            !ship_receipts_reserve(rcpts, count)) {
            return world_load_record_oom(load_result);
        }
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

static bool write_asset_ship_payload(FILE *f, const ship_t *ship) {
    ship_t empty = {0};
    float cargo_snapshot[COMMODITY_COUNT];
    if (!ship) ship = &empty;
    WRITE_FIELD(f, ship->pos);
    WRITE_FIELD(f, ship->vel);
    WRITE_FIELD(f, ship->angle);
    WRITE_FIELD(f, ship->hull);
    ship_cargo_snapshot(ship, cargo_snapshot);
    if (!write_commodity_floats(f, cargo_snapshot)) return false;
    WRITE_FIELD(f, ship->hull_class);
    WRITE_FIELD(f, ship->mining_level);
    WRITE_FIELD(f, ship->hold_level);
    WRITE_FIELD(f, ship->tractor_level);
    WRITE_FIELD(f, ship->towed_fragments);
    WRITE_FIELD(f, ship->towed_count);
    WRITE_FIELD(f, ship->towed_scaffold);
    WRITE_FIELD(f, ship->tractor_active);
    WRITE_FIELD(f, ship->comm_range);
    WRITE_FIELD(f, ship->unlocked_modules);
    WRITE_FIELD(f, ship->stat_ore_mined);
    WRITE_FIELD(f, ship->stat_credits_earned);
    WRITE_FIELD(f, ship->stat_credits_spent);
    WRITE_FIELD(f, ship->stat_asteroids_fractured);
    return write_npc_ship_manifest_payload(f, ship);
}

static bool read_asset_ship_payload(
    FILE *f,
    ship_t *ship,
    world_load_result_t *load_result) {
    if (!ship) return false;
    ship_cleanup(ship);
    memset(ship, 0, sizeof(*ship));
    READ_FIELD(f, ship->pos);
    READ_FIELD(f, ship->vel);
    READ_FIELD(f, ship->angle);
    READ_FIELD(f, ship->hull);
    if (!read_commodity_floats(f, ship->cargo)) return false;
    READ_FIELD(f, ship->hull_class);
    READ_FIELD(f, ship->mining_level);
    READ_FIELD(f, ship->hold_level);
    READ_FIELD(f, ship->tractor_level);
    READ_FIELD(f, ship->towed_fragments);
    READ_FIELD(f, ship->towed_count);
    READ_FIELD(f, ship->towed_scaffold);
    READ_FIELD(f, ship->tractor_active);
    READ_FIELD(f, ship->comm_range);
    READ_FIELD(f, ship->unlocked_modules);
    READ_FIELD(f, ship->stat_ore_mined);
    READ_FIELD(f, ship->stat_credits_earned);
    READ_FIELD(f, ship->stat_credits_spent);
    READ_FIELD(f, ship->stat_asteroids_fractured);
    if (ship->hull_class < 0 || ship->hull_class >= HULL_CLASS_COUNT)
        ship->hull_class = HULL_CLASS_MINER;
    if (!(ship->hull >= 0.0f) || ship->hull > ship_max_hull(ship))
        ship->hull = ship_max_hull(ship);
    if (ship->comm_range <= 0.0f) ship->comm_range = 1500.0f;
    if (ship->towed_count > 10) ship->towed_count = 0;
    if (!read_npc_ship_manifest_payload(
            f, ship, load_result)) {
        return false;
    }
    ship_retire_finished_cargo_slots(ship);
    return true;
}

static bool write_ship_asset(FILE *f, const ship_asset_t *asset,
                             const ship_t *live_ship) {
    ship_asset_t empty = {0};
    if (!asset) asset = &empty;
    if (!actor_principal_is_canonical(&asset->owner_principal) ||
        (asset->active &&
         (asset->asset_id == SHIP_ASSET_ID_NONE ||
          (unsigned)asset->hull_class >= HULL_CLASS_COUNT ||
          asset->status > SHIP_ASSET_STATUS_DESTROYED ||
          asset->operator_kind > SHIP_ASSET_OPERATOR_NPC ||
          asset->provenance >
              SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY ||
          !ship_asset_birth_proof_is_canonical(asset)))) {
        return false;
    }
    WRITE_FIELD(f, asset->active);
    WRITE_FIELD(f, asset->asset_id);
    WRITE_FIELD(f, asset->hull_class);
    if (!write_actor_principal(f, &asset->owner_principal)) {
        return false;
    }
    WRITE_FIELD(f, asset->owner_quarantine_record_id);
    WRITE_FIELD(f, asset->status);
    WRITE_FIELD(f, asset->operator_kind);
    WRITE_FIELD(f, asset->provenance);
    WRITE_FIELD(f, asset->custody_station);
    WRITE_FIELD(f, asset->operator_slot);
    WRITE_FIELD(f, asset->build_station);
    WRITE_FIELD(f, asset->loaner);
    WRITE_FIELD(f, asset->destroyed);
    WRITE_FIELD(f, asset->birth_proof_version);
    WRITE_FIELD(f, asset->birth_fragment_grades);
    if (fwrite(asset->birth_soul_pub,
               sizeof(asset->birth_soul_pub), 1, f) != 1 ||
        fwrite(asset->birth_material_root,
               sizeof(asset->birth_material_root), 1, f) != 1 ||
        fwrite(asset->birth_fragment_pubs,
               sizeof(asset->birth_fragment_pubs), 1, f) != 1) {
        return false;
    }
    const ship_t *ship = live_ship ? live_ship : &asset->stored_ship;
    return write_asset_ship_payload(f, asset->active ? ship : NULL);
}

static bool read_ship_asset(
    FILE *f,
    ship_asset_t *asset,
    ownership_v79_migration_t *migration,
    int asset_index,
    world_load_result_t *load_result) {
    if (!asset) return false;
    ship_cleanup(&asset->stored_ship);
    memset(asset, 0, sizeof(*asset));
    READ_FIELD(f, asset->active);
    READ_FIELD(f, asset->asset_id);
    READ_FIELD(f, asset->hull_class);
    if (g_loaded_save_version >= 79) {
        if (!read_actor_principal(f, &asset->owner_principal)) {
            return false;
        }
        READ_FIELD(f, asset->owner_quarantine_record_id);
        READ_FIELD(f, asset->status);
        READ_FIELD(f, asset->operator_kind);
        READ_FIELD(f, asset->provenance);
        READ_FIELD(f, asset->custody_station);
        READ_FIELD(f, asset->operator_slot);
        READ_FIELD(f, asset->build_station);
        READ_FIELD(f, asset->loaner);
        READ_FIELD(f, asset->destroyed);
        READ_FIELD(f, asset->birth_proof_version);
        READ_FIELD(f, asset->birth_fragment_grades);
        if (fread(asset->birth_soul_pub,
                  sizeof(asset->birth_soul_pub), 1, f) != 1 ||
            fread(asset->birth_material_root,
                  sizeof(asset->birth_material_root), 1, f) != 1 ||
            fread(asset->birth_fragment_pubs,
                  sizeof(asset->birth_fragment_pubs), 1, f) != 1) {
            return false;
        }
    } else {
        legacy_ship_asset_owner_evidence_t evidence = {0};
        READ_FIELD(f, evidence.owner_kind);
        READ_FIELD(f, asset->status);
        READ_FIELD(f, asset->operator_kind);
        READ_FIELD(f, asset->provenance);
        READ_FIELD(f, evidence.owner_station);
        READ_FIELD(f, asset->custody_station);
        READ_FIELD(f, asset->operator_slot);
        READ_FIELD(f, asset->build_station);
        READ_FIELD(f, asset->loaner);
        READ_FIELD(f, asset->destroyed);
        READ_FIELD(f, evidence.owner_pubkey);
        READ_FIELD(f, evidence.owner_session);
        evidence.present = asset->active;
        if (migration && asset_index >= 0 &&
            asset_index < MAX_SHIP_ASSETS) {
            migration->assets[asset_index] = evidence;
        }
        asset->owner_principal = actor_principal_none();
        asset->birth_proof_version = 0;
        memset(asset->birth_fragment_grades, 0,
               sizeof(asset->birth_fragment_grades));
        memset(asset->birth_soul_pub, 0,
               sizeof(asset->birth_soul_pub));
        memset(asset->birth_material_root, 0,
               sizeof(asset->birth_material_root));
        memset(asset->birth_fragment_pubs, 0,
               sizeof(asset->birth_fragment_pubs));
        if (asset->provenance ==
            SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY) {
            asset->provenance = SHIP_ASSET_PROVENANCE_LEGACY;
        }
    }
    if (!read_asset_ship_payload(
            f, &asset->stored_ship, load_result)) {
        return false;
    }
    asset->live_ship_ref = entity_ref_none();
    asset->ship = &asset->stored_ship;
    if (!asset->active) {
        ship_cleanup(&asset->stored_ship);
        memset(asset, 0, sizeof(*asset));
        return true;
    }
    if (asset->asset_id == SHIP_ASSET_ID_NONE ||
        (unsigned)asset->hull_class >= HULL_CLASS_COUNT ||
        asset->status > SHIP_ASSET_STATUS_DESTROYED ||
        asset->operator_kind > SHIP_ASSET_OPERATOR_NPC ||
        asset->provenance >
            SHIP_ASSET_PROVENANCE_BIRTH_ASSEMBLY ||
        !actor_principal_is_canonical(&asset->owner_principal) ||
        !ship_asset_birth_proof_is_canonical(asset)) {
        return false;
    }
    if (asset->destroyed)
        asset->status = SHIP_ASSET_STATUS_DESTROYED;
    return true;
}

/* ---- contract field-by-field I/O ---- */
static bool write_contract(FILE *f, const contract_t *c,
                           uint32_t version) {
    WRITE_FIELD(f, c->active);
    WRITE_FIELD(f, c->action);
    WRITE_FIELD(f, c->station_index);
    WRITE_FIELD(f, c->commodity);
    WRITE_FIELD(f, c->required_grade);
    WRITE_FIELD(f, c->proof_flags);
    WRITE_FIELD(f, c->required_prefix_class);
    WRITE_FIELD(f, c->required_recipe_id);
    if (fwrite(c->required_parent, 32, 1, f) != 1) return false;
    WRITE_FIELD(f, c->forbidden_origin_mask);
    WRITE_FIELD(f, c->quantity_needed);
    WRITE_FIELD(f, c->base_price);
    WRITE_FIELD(f, c->age);
    WRITE_FIELD(f, c->target_pos);
    WRITE_FIELD(f, c->target_index);
    if (fwrite(c->target_pub, 32, 1, f) != 1) return false;
    if (version >= 81) {
        if (!write_actor_principal(
                f, &c->claimed_by_principal)) {
            return false;
        }
        WRITE_FIELD(f, c->claimed_by_quarantine_record_id);
    } else {
        WRITE_FIELD(f, c->claimed_by);
    }
    return true;
}

static bool read_contract(FILE *f, contract_t *c,
                          ownership_v79_migration_t *migration,
                          int contract_index) {
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
    if (g_loaded_save_version >= 56) {
        READ_FIELD(f, c->proof_flags);
        READ_FIELD(f, c->required_prefix_class);
        READ_FIELD(f, c->required_recipe_id);
        if (fread(c->required_parent, 32, 1, f) != 1) return false;
    } else {
        c->proof_flags = 0;
        c->required_prefix_class = 0;
        c->required_recipe_id = 0;
        memset(c->required_parent, 0, sizeof(c->required_parent));
    }
    if (g_loaded_save_version >= 57) {
        READ_FIELD(f, c->forbidden_origin_mask);
    } else {
        c->forbidden_origin_mask = 0;
        c->proof_flags &=
            (uint8_t)(UINT8_MAX ^ CONTRACT_PROOF_FORBID_ORIGIN);
    }
    READ_FIELD(f, c->quantity_needed);
    READ_FIELD(f, c->base_price);
    READ_FIELD(f, c->age);
    READ_FIELD(f, c->target_pos);
    READ_FIELD(f, c->target_index);
    if (g_loaded_save_version >= 61) {
        if (fread(c->target_pub, 32, 1, f) != 1) return false;
    } else {
        memset(c->target_pub, 0, sizeof(c->target_pub));
    }
    c->claimed_by = -1;
    c->claimed_by_principal = actor_principal_none();
    c->claimed_by_quarantine_record_id = 0;
    if (g_loaded_save_version >= 81) {
        if (!read_actor_principal(
                f, &c->claimed_by_principal)) {
            return false;
        }
        READ_FIELD(f, c->claimed_by_quarantine_record_id);
    } else {
        int8_t legacy_claimed_by = -1;
        READ_FIELD(f, legacy_claimed_by);
        if (!migration || contract_index < 0 ||
            contract_index >= MAX_CONTRACTS) {
            return false;
        }
        migration->legacy_contract_present[contract_index] = true;
        migration->legacy_contract_actor_code[contract_index] =
            legacy_claimed_by;
    }
    return true;
}

static bool write_delivery_shipment(FILE *f,
                                    const delivery_shipment_t *s,
                                    uint32_t version) {
    WRITE_FIELD(f, s->active);
    WRITE_FIELD(f, s->shipment_id);
    WRITE_FIELD(f, s->origin_station);
    WRITE_FIELD(f, s->destination_station);
    WRITE_FIELD(f, s->contract_index);
    if (version >= 81) {
        if (!write_actor_principal(
                f, &s->debtor_principal)) {
            return false;
        }
        WRITE_FIELD(f, s->debtor_quarantine_record_id);
    } else {
        WRITE_FIELD(f, s->debtor_player);
    }
    WRITE_FIELD(f, s->commodity);
    WRITE_FIELD(f, s->quantity_total);
    WRITE_FIELD(f, s->quantity_bound);
    WRITE_FIELD(f, s->quantity_delivered);
    WRITE_FIELD(f, s->quantity_black_market_sold);
    WRITE_FIELD(f, s->debt_principal);
    WRITE_FIELD(f, s->destination_payout);
    WRITE_FIELD(f, s->origin_completion_credit);
    WRITE_FIELD(f, s->due_tick);
    WRITE_FIELD(f, s->status);
    if (fwrite(s->cargo_pub, sizeof(s->cargo_pub), 1, f) != 1) return false;
    if (s->active) {
        uint16_t payload_count = s->quantity_bound;
        if (payload_count > MAX_DELIVERY_BOUND_CARGO)
            payload_count = MAX_DELIVERY_BOUND_CARGO;
        WRITE_FIELD(f, payload_count);
        if (payload_count > 0) {
            if (fwrite(s->cargo_units, sizeof(s->cargo_units[0]),
                       payload_count, f) != payload_count) return false;
            if (fwrite(s->cargo_chains, sizeof(s->cargo_chains[0]),
                       payload_count, f) != payload_count) return false;
        }
    }
    return true;
}

static bool read_delivery_shipment(
    FILE *f,
    delivery_shipment_t *s,
    uint32_t version,
    ownership_v79_migration_t *migration,
    int shipment_index) {
    memset(s, 0, sizeof(*s));
    READ_FIELD(f, s->active);
    READ_FIELD(f, s->shipment_id);
    READ_FIELD(f, s->origin_station);
    READ_FIELD(f, s->destination_station);
    READ_FIELD(f, s->contract_index);
    s->debtor_player = UINT8_MAX;
    if (version >= 81) {
        if (!read_actor_principal(
                f, &s->debtor_principal)) {
            return false;
        }
        READ_FIELD(f, s->debtor_quarantine_record_id);
    } else {
        uint8_t legacy_debtor = UINT8_MAX;
        READ_FIELD(f, legacy_debtor);
        if (!migration || shipment_index < 0 ||
            shipment_index >= MAX_DELIVERY_SHIPMENTS) {
            return false;
        }
        migration->legacy_delivery_present[shipment_index] = true;
        migration->legacy_delivery_actor_code[shipment_index] =
            legacy_debtor;
    }
    READ_FIELD(f, s->commodity);
    READ_FIELD(f, s->quantity_total);
    READ_FIELD(f, s->quantity_bound);
    READ_FIELD(f, s->quantity_delivered);
    READ_FIELD(f, s->quantity_black_market_sold);
    READ_FIELD(f, s->debt_principal);
    READ_FIELD(f, s->destination_payout);
    READ_FIELD(f, s->origin_completion_credit);
    READ_FIELD(f, s->due_tick);
    READ_FIELD(f, s->status);
    if (fread(s->cargo_pub, sizeof(s->cargo_pub), 1, f) != 1) return false;
    if (s->quantity_bound > MAX_DELIVERY_BOUND_CARGO) return false;
    if (s->status > DELIVERY_SHIPMENT_DEFAULTED) return false;
    if (version >= 67 && s->active) {
        uint16_t payload_count = 0;
        READ_FIELD(f, payload_count);
        if (payload_count > MAX_DELIVERY_BOUND_CARGO) return false;
        if (payload_count > 0) {
            if (fread(s->cargo_units, sizeof(s->cargo_units[0]),
                      payload_count, f) != payload_count) return false;
            if (fread(s->cargo_chains, sizeof(s->cargo_chains[0]),
                      payload_count, f) != payload_count) return false;
        }
    }
    return true;
}

static bool write_cargo_pod(FILE *f, uint16_t index,
                            const cargo_pod_t *pod,
                            uint32_t version) {
    if (!cargo_pod_custody_charge_anchor_valid(pod) ||
        pod->custody_charge_total > (int64_t)LEDGER_FLOAT_LIMIT) {
        return false;
    }
    uint8_t kind = (uint8_t)pod->kind;
    uint8_t commodity = (uint8_t)pod->commodity;
    WRITE_FIELD(f, index);
    WRITE_FIELD(f, kind);
    WRITE_FIELD(f, commodity);
    WRITE_FIELD(f, pod->quantity);
    WRITE_FIELD(f, pod->shipment_id);
    WRITE_FIELD(f, pod->pos);
    WRITE_FIELD(f, pod->vel);
    WRITE_FIELD(f, pod->radius);
    WRITE_FIELD(f, pod->rotation);
    WRITE_FIELD(f, pod->spin);
    WRITE_FIELD(f, pod->age);
    if (version >= 82) {
        if (!write_actor_principal(
                f, &pod->tow_owner_principal)) {
            return false;
        }
        WRITE_FIELD(
            f, pod->tow_owner_quarantine_record_id);
    } else {
        int player_tractor =
            cargo_pod_player_tractor(pod);
        bool has_durable_player_owner =
            pod->tow_owner_principal.kind ==
                ACTOR_PRINCIPAL_PLAYER ||
            pod->tow_owner_principal.kind ==
                ACTOR_PRINCIPAL_UNATTRIBUTED;
        /*
         * Legacy output exists only for exact migration fixtures. Refuse a
         * downgrade that cannot encode the current custody: offline owners
         * and quarantine bindings must never be silently written as loose.
         */
        if (pod->tow_owner_quarantine_record_id != 0 ||
            (has_durable_player_owner &&
             (player_tractor < 0 ||
              player_tractor >= MAX_PLAYERS))) {
            return false;
        }
        int8_t towed_by =
            (player_tractor >= 0 &&
             player_tractor < MAX_PLAYERS)
            ? (int8_t)player_tractor : -1;
        WRITE_FIELD(f, towed_by);
    }
    WRITE_FIELD(f, pod->manifest_count);
    if (pod->manifest_count > 0) {
        if (pod->manifest_count > CARGO_POD_MANIFEST_CAP) return false;
        if (fwrite(pod->manifest_units, sizeof(pod->manifest_units[0]),
                   pod->manifest_count, f) != pod->manifest_count) {
            return false;
        }
    }
    {
        uint8_t has_shell_frame = pod->has_shell_frame ? 1u : 0u;
        WRITE_FIELD(f, has_shell_frame);
        if (has_shell_frame) {
            if (pod->shell_frame.commodity != (uint8_t)COMMODITY_FRAME)
                return false;
            if (fwrite(&pod->shell_frame, sizeof(pod->shell_frame), 1, f) != 1)
                return false;
        }
    }
    WRITE_FIELD(f, pod->custody_station);
    int tractor_station = -1;
    int tractor_module = -1;
    (void)cargo_pod_module_tractor_indices(
        pod, &tractor_station, &tractor_module);
    uint8_t tractor_station_tag = tractor_station >= 0
        ? (uint8_t)(tractor_station + 1) : 0;
    uint8_t tractor_module_tag = tractor_module >= 0
        ? (uint8_t)(tractor_module + 1) : 0;
    WRITE_FIELD(f, tractor_station_tag);
    WRITE_FIELD(f, tractor_module_tag);
    WRITE_FIELD(f, pod->tow_hardpoint_tag);
    WRITE_FIELD(f, pod->custody_charge_total);
    WRITE_FIELD(f, pod->custody_charge_unit_count);
    WRITE_FIELD(f, pod->custody_charge_units_processed);
    if (fwrite(pod->custody_charge_manifest_digest,
               sizeof(pod->custody_charge_manifest_digest), 1, f) != 1) {
        return false;
    }
    return true;
}

static bool read_cargo_pod(
    FILE *f, world_t *w, int version,
    ownership_v79_migration_t *migration) {
    uint16_t index = 0;
    uint8_t kind = 0;
    uint8_t commodity = 0;
    cargo_pod_t pod = {0};
    int8_t towed_by = -1;
    uint8_t tow_hardpoint_tag = 0;
    READ_FIELD(f, index);
    READ_FIELD(f, kind);
    READ_FIELD(f, commodity);
    READ_FIELD(f, pod.quantity);
    READ_FIELD(f, pod.shipment_id);
    READ_FIELD(f, pod.pos);
    READ_FIELD(f, pod.vel);
    READ_FIELD(f, pod.radius);
    READ_FIELD(f, pod.rotation);
    READ_FIELD(f, pod.spin);
    READ_FIELD(f, pod.age);
    if (version >= 82) {
        if (!read_actor_principal(
                f, &pod.tow_owner_principal)) {
            return false;
        }
        READ_FIELD(
            f, pod.tow_owner_quarantine_record_id);
    } else {
        READ_FIELD(f, towed_by);
    }
    if (version >= 69) {
        READ_FIELD(f, pod.manifest_count);
        if (pod.manifest_count > CARGO_POD_MANIFEST_CAP) return false;
        if (pod.manifest_count > 0) {
            if (fread(pod.manifest_units, sizeof(pod.manifest_units[0]),
                      pod.manifest_count, f) != pod.manifest_count) {
                return false;
            }
        }
    }
    if (version >= 70) {
        uint8_t has_shell_frame = 0;
        READ_FIELD(f, has_shell_frame);
        if (has_shell_frame) {
            if (fread(&pod.shell_frame, sizeof(pod.shell_frame), 1, f) != 1)
                return false;
            if (pod.shell_frame.commodity != (uint8_t)COMMODITY_FRAME)
                return false;
            pod.has_shell_frame = true;
        }
    }
    if (version >= 73) {
        READ_FIELD(f, pod.custody_station);
        if (pod.custody_station > MAX_STATIONS) pod.custody_station = 0;
    }
    if (version >= 74) {
        uint8_t tractor_station_tag = 0;
        uint8_t tractor_module_tag = 0;
        READ_FIELD(f, tractor_station_tag);
        READ_FIELD(f, tractor_module_tag);
        bool module_tags_present =
            tractor_station_tag != 0 ||
            tractor_module_tag != 0;
        bool module_tags_valid =
            tractor_station_tag > 0 &&
            tractor_station_tag <= MAX_STATIONS &&
            tractor_module_tag > 0 &&
            tractor_module_tag <= MAX_MODULES_PER_STATION;
        if (module_tags_valid) {
            cargo_pod_set_module_tractor(
                &pod, (int)tractor_station_tag - 1,
                (int)tractor_module_tag - 1);
        }
        if (version >= 82 &&
            module_tags_present &&
            !module_tags_valid) {
            return false;
        }
    }
    if (version >= 76) {
        READ_FIELD(f, tow_hardpoint_tag);
        if (tow_hardpoint_tag > CARGO_POD_HARDPOINT_COUNT)
            tow_hardpoint_tag = 0;
    }
    if (version >= 80) {
        READ_FIELD(f, pod.custody_charge_total);
        READ_FIELD(f, pod.custody_charge_unit_count);
        READ_FIELD(f, pod.custody_charge_units_processed);
        if (fread(pod.custody_charge_manifest_digest,
                  sizeof(pod.custody_charge_manifest_digest),
                  1, f) != 1) {
            return false;
        }
    }
    if (index >= MAX_CARGO_PODS) return false;
    if (w->cargo_pods[index].active ||
        (version < 82 && migration &&
         migration->cargo_pods[index].present)) {
        return false;
    }
    if (kind == CARGO_POD_NONE || kind > CARGO_POD_CARGO) return false;
    if (commodity >= COMMODITY_COUNT) return false;
    if (pod.manifest_count > pod.quantity) return false;
    for (uint16_t i = 0; i < pod.manifest_count; i++) {
        if (pod.manifest_units[i].commodity != commodity) return false;
        if (pod.manifest_units[i].quantity == 0)
            pod.manifest_units[i].quantity = 1;
    }
    pod.active = true;
    pod.kind = (cargo_pod_kind_t)kind;
    pod.commodity = (commodity_t)commodity;
    if (!cargo_pod_custody_charge_anchor_valid(&pod) ||
        pod.custody_charge_total > (int64_t)LEDGER_FLOAT_LIMIT) {
        return false;
    }
    if (version >= 82) {
        bool module_bound =
            cargo_pod_has_module_tractor(&pod);
        if (!actor_principal_is_canonical(
                &pod.tow_owner_principal) ||
            (pod.tow_owner_quarantine_record_id != 0 &&
             pod.tow_owner_principal.kind !=
                 ACTOR_PRINCIPAL_NONE) ||
            (module_bound &&
             (pod.tow_owner_quarantine_record_id != 0 ||
              pod.tow_owner_principal.kind !=
                  ACTOR_PRINCIPAL_NONE))) {
            return false;
        }
    } else if (migration) {
        migration->cargo_pods[index] =
            (legacy_cargo_pod_tow_evidence_t){
                .present = true,
                .module_bound =
                    cargo_pod_has_module_tractor(&pod),
                .player_slot = towed_by,
            };
        /*
         * A pre-v82 player slot is evidence only. Never project it into a live
         * player relation before the conservative ownership migration runs.
         */
        if (towed_by >= 0)
            tractor_binding_clear(&pod.tractor);
    }
    /* Tractor setters intentionally clear stale attachment state.  Restore the
     * persisted named edge only after selecting the loaded tractor owner. */
    pod.tow_hardpoint_tag = tow_hardpoint_tag;
    w->cargo_pods[index] = pod;
    return true;
}

static bool write_ownership_quarantine(
    FILE *f,
    const ownership_quarantine_t *quarantine) {
    if (!f || !ownership_quarantine_validate(quarantine))
        return false;
    WRITE_FIELD(f, quarantine->record_id_high_water);
    WRITE_FIELD(f, quarantine->count);
    for (uint16_t i = 0; i < quarantine->count; i++) {
        const ownership_quarantine_entry_t *row =
            &quarantine->entries[i];
        WRITE_FIELD(f, row->record_id);
        WRITE_FIELD(f, row->source_kind);
        WRITE_FIELD(f, row->reason);
        WRITE_FIELD(f, row->station_index);
        WRITE_FIELD(f, row->row_index);
        WRITE_FIELD(f, row->legacy_actor_code);
    }
    return true;
}

static bool read_ownership_quarantine(
    FILE *f,
    ownership_quarantine_t *quarantine) {
    if (!f || !quarantine) return false;
    ownership_quarantine_clear(quarantine);

    uint16_t count = 0;
    if (fread(&quarantine->record_id_high_water,
              sizeof(quarantine->record_id_high_water), 1, f) != 1 ||
        fread(&count, sizeof(count), 1, f) != 1 ||
        count > OWNERSHIP_QUARANTINE_CAP) {
        ownership_quarantine_clear(quarantine);
        return false;
    }
    for (uint16_t i = 0; i < count; i++) {
        ownership_quarantine_entry_t row = {0};
        if (fread(&row.record_id, sizeof(row.record_id), 1, f) != 1 ||
            fread(&row.source_kind, sizeof(row.source_kind), 1, f) != 1 ||
            fread(&row.reason, sizeof(row.reason), 1, f) != 1 ||
            fread(&row.station_index, sizeof(row.station_index), 1, f) != 1 ||
            fread(&row.row_index, sizeof(row.row_index), 1, f) != 1 ||
            fread(&row.legacy_actor_code,
                  sizeof(row.legacy_actor_code), 1, f) != 1 ||
            !ownership_quarantine_entry_is_canonical(&row)) {
            ownership_quarantine_clear(quarantine);
            return false;
        }
        quarantine->entries[i] = row;
        quarantine->count = (uint16_t)(i + 1);
    }
    if (!ownership_quarantine_validate(quarantine)) {
        ownership_quarantine_clear(quarantine);
        return false;
    }
    return true;
}

static int legacy_pending_station_slot(
    int station_index,
    int8_t owner_code) {
    if (owner_code == -1) return station_index;
    if (owner_code == INT8_MIN) return 0;
    if (owner_code <= -2) return -1 - (int)owner_code;
    return -1;
}

static bool ownership_migration_stage_row(
    ownership_v79_migration_t *migration,
    const ownership_quarantine_t *existing,
    uint64_t *next_record_id,
    bool *record_id_initialized,
    uint8_t source_kind,
    uint8_t reason,
    uint16_t station_index,
    uint16_t row_index,
    uint16_t legacy_actor_code,
    uint64_t *record_id_out) {
    if (record_id_out) *record_id_out = 0;
    if (!migration || !existing || !next_record_id ||
        !record_id_initialized ||
        migration->staged_row_count >=
            OWNERSHIP_V79_STAGED_ROW_CAP) {
        return false;
    }
    if (!*record_id_initialized) {
        if (!ownership_quarantine_next_record_id(
                existing, next_record_id)) {
            return false;
        }
        *record_id_initialized = true;
    }
    if (*next_record_id == 0) return false;
    ownership_quarantine_entry_t row = {
        .record_id = *next_record_id,
        .source_kind = source_kind,
        .reason = reason,
        .station_index = station_index,
        .row_index = row_index,
        .legacy_actor_code = legacy_actor_code,
    };
    if (!ownership_quarantine_entry_is_canonical(&row)) {
        return false;
    }
    migration->staged_rows[migration->staged_row_count++] = row;
    if (record_id_out) *record_id_out = row.record_id;
    if (*next_record_id == UINT64_MAX) {
        *next_record_id = 0;
    } else {
        (*next_record_id)++;
    }
    return true;
}

static uint8_t legacy_pending_owner_migrate(
    const world_t *w,
    int station_index,
    const legacy_pending_ship_owner_evidence_t *evidence,
    actor_principal_t *owner_out,
    uint16_t *legacy_actor_code_out) {
    if (owner_out) *owner_out = actor_principal_none();
    if (legacy_actor_code_out)
        *legacy_actor_code_out = OWNERSHIP_QUARANTINE_NA;
    if (!w || !evidence || !evidence->present || !owner_out) {
        return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
    }
    bool pub = save_bytes_any(
        evidence->owner_pubkey,
        sizeof(evidence->owner_pubkey));
    bool session = save_bytes_any(
        evidence->owner_session,
        sizeof(evidence->owner_session));
    bool valid_slot =
        evidence->owner_code >= 0 &&
        evidence->owner_code < MAX_PLAYERS;
    if (evidence->slot_only) {
        if (evidence->owner_code < 0) {
            int target = legacy_pending_station_slot(
                station_index, evidence->owner_code);
            if (target >= 0 && target < MAX_STATIONS &&
                actor_principal_from_station(w, target, owner_out)) {
                return OWNERSHIP_QUARANTINE_REASON_NONE;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        }
        if (valid_slot) {
            if (legacy_actor_code_out)
                *legacy_actor_code_out =
                    (uint16_t)evidence->owner_code;
            return OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN;
        }
        return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
    }

    switch ((legacy_ship_asset_owner_kind_t)evidence->owner_kind) {
        case LEGACY_SHIP_ASSET_OWNER_STATION: {
            if (pub || session || evidence->owner_code >= 0) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            int target = legacy_pending_station_slot(
                station_index, evidence->owner_code);
            if (target >= 0 && target < MAX_STATIONS &&
                actor_principal_from_station(w, target, owner_out)) {
                return OWNERSHIP_QUARANTINE_REASON_NONE;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        }
        case LEGACY_SHIP_ASSET_OWNER_PLAYER_PUBKEY:
            if (session || evidence->owner_code < 0) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            if (valid_slot && pub &&
                actor_principal_from_stable_id(
                    ACTOR_PRINCIPAL_PLAYER,
                    evidence->owner_pubkey, owner_out)) {
                return OWNERSHIP_QUARANTINE_REASON_NONE;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        case LEGACY_SHIP_ASSET_OWNER_PLAYER_SESSION:
            if (pub || evidence->owner_code < 0) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            if (valid_slot && session) {
                if (legacy_actor_code_out)
                    *legacy_actor_code_out =
                        (uint16_t)evidence->owner_code;
                return OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        case LEGACY_SHIP_ASSET_OWNER_NONE:
            if (pub || session) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            if (valid_slot) {
                if (legacy_actor_code_out)
                    *legacy_actor_code_out =
                        (uint16_t)evidence->owner_code;
                return OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        default:
            return (pub || session)
                ? OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL
                : OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
    }
}

static uint8_t legacy_ship_asset_owner_migrate(
    const world_t *w,
    const legacy_ship_asset_owner_evidence_t *evidence,
    actor_principal_t *owner_out) {
    if (owner_out) *owner_out = actor_principal_none();
    if (!w || !evidence || !evidence->present || !owner_out) {
        return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
    }
    bool pub = save_bytes_any(
        evidence->owner_pubkey,
        sizeof(evidence->owner_pubkey));
    bool session = save_bytes_any(
        evidence->owner_session,
        sizeof(evidence->owner_session));
    switch ((legacy_ship_asset_owner_kind_t)evidence->owner_kind) {
        case LEGACY_SHIP_ASSET_OWNER_STATION:
            if (pub || session) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            if (evidence->owner_station >= 0 &&
                evidence->owner_station < MAX_STATIONS &&
                actor_principal_from_station(
                    w, evidence->owner_station, owner_out)) {
                return OWNERSHIP_QUARANTINE_REASON_NONE;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        case LEGACY_SHIP_ASSET_OWNER_PLAYER_PUBKEY:
            if (session || evidence->owner_station != -1) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            if (pub && actor_principal_from_stable_id(
                           ACTOR_PRINCIPAL_PLAYER,
                           evidence->owner_pubkey, owner_out)) {
                return OWNERSHIP_QUARANTINE_REASON_NONE;
            }
            return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        case LEGACY_SHIP_ASSET_OWNER_PLAYER_SESSION:
            if (pub || evidence->owner_station != -1) {
                return OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
            }
            return session
                ? OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN
                : OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        case LEGACY_SHIP_ASSET_OWNER_NONE:
            return (pub || session || evidence->owner_station != -1)
                ? OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL
                : OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        default:
            return (pub || session)
                ? OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL
                : OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
    }
}

static uint8_t legacy_slot_actor_migrate(
    const world_t *w,
    int actor_code,
    bool open_sentinel_allowed,
    actor_principal_t *owner_out,
    uint16_t *legacy_actor_code_out) {
    if (owner_out) *owner_out = actor_principal_none();
    if (legacy_actor_code_out)
        *legacy_actor_code_out = OWNERSHIP_QUARANTINE_NA;
    if (!w || !owner_out) {
        return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
    }
    if (open_sentinel_allowed && actor_code == -1)
        return OWNERSHIP_QUARANTINE_REASON_NONE;
    if (actor_code >= 0 && actor_code < MAX_PLAYERS) {
        if (legacy_actor_code_out)
            *legacy_actor_code_out = (uint16_t)actor_code;
        return OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN;
    }
    if (actor_code >= MAX_PLAYERS &&
        actor_code < MAX_PLAYERS + MAX_NPC_SHIPS) {
        int npc_slot = actor_code - MAX_PLAYERS;
        bool token_conflict = false;
        if (!actor_principal_from_unique_npc_slot(
                w, npc_slot, owner_out,
                &token_conflict)) {
            return token_conflict
                ? OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL
                : OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        }
        return OWNERSHIP_QUARANTINE_REASON_NONE;
    }
    return OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
}

static bool migrate_legacy_ownership(
    world_t *w,
    ownership_v79_migration_t *migration,
    uint32_t loaded_version) {
    if (!w || !migration || loaded_version >= 82 ||
        !ownership_quarantine_validate(
            &w->ownership_quarantine)) {
        return loaded_version >= 82 && w && migration;
    }
    uint64_t next_record_id = 0;
    bool record_id_initialized = false;

    if (loaded_version < 79) {
      for (int station = 0; station < MAX_STATIONS; station++) {
        station_t *st = &w->stations[station];
        if (st->pending_ship_build_count < 0 ||
            st->pending_ship_build_count > 4) {
            return false;
        }
        for (int row = 0; row < st->pending_ship_build_count; row++) {
            pending_ship_build_t *build =
                &st->pending_ship_builds[row];
            uint16_t legacy_actor_code =
                OWNERSHIP_QUARANTINE_NA;
            uint8_t reason = legacy_pending_owner_migrate(
                w, station,
                &migration->pending[station][row],
                &build->owner_principal,
                &legacy_actor_code);
            if (reason != OWNERSHIP_QUARANTINE_REASON_NONE &&
                !ownership_migration_stage_row(
                    migration, &w->ownership_quarantine,
                    &next_record_id, &record_id_initialized,
                    OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD,
                    reason, (uint16_t)station, (uint16_t)row,
                    legacy_actor_code,
                    &build->owner_quarantine_record_id)) {
                return false;
            }
            if (loaded_version >= 73) {
                build->mode = PENDING_SHIP_BUILD_MODE_UNKNOWN;
                if (!ownership_migration_stage_row(
                        migration, &w->ownership_quarantine,
                        &next_record_id, &record_id_initialized,
                        OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD,
                        OWNERSHIP_QUARANTINE_REASON_LEGACY_BUILD_MODE_UNPROVEN,
                        (uint16_t)station, (uint16_t)row,
                        OWNERSHIP_QUARANTINE_NA,
                        &build->mode_quarantine_record_id)) {
                    return false;
                }
            } else {
                build->mode = PENDING_SHIP_BUILD_MODE_MATERIAL;
            }
        }
      }

      for (int row = 0; row < MAX_SHIP_ASSETS; row++) {
        ship_asset_t *asset = &w->ship_assets[row];
        if (!asset->active) continue;
        uint8_t reason = legacy_ship_asset_owner_migrate(
            w, &migration->assets[row],
            &asset->owner_principal);
        if (reason != OWNERSHIP_QUARANTINE_REASON_NONE) {
            if (!ownership_migration_stage_row(
                    migration, &w->ownership_quarantine,
                    &next_record_id, &record_id_initialized,
                    OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET,
                    reason, OWNERSHIP_QUARANTINE_NA,
                    (uint16_t)row,
                    OWNERSHIP_QUARANTINE_NA,
                    &asset->owner_quarantine_record_id)) {
                return false;
            }
            asset->status = asset->destroyed
                ? SHIP_ASSET_STATUS_DESTROYED
                : SHIP_ASSET_STATUS_STORED;
            asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
            asset->operator_slot = -1;
            asset->live_ship_ref = entity_ref_none();
            asset->ship = &asset->stored_ship;
            asset->loaner = false;
        }
      }
    }

    if (loaded_version < 81) {
      for (int row = 0; row < MAX_CONTRACTS; row++) {
        contract_t *contract = &w->contracts[row];
        if (!migration->legacy_contract_present[row])
            return false;
        if (!contract->active) {
            contract_ownership_clear(contract);
            continue;
        }
        uint16_t legacy_actor_code = OWNERSHIP_QUARANTINE_NA;
        uint8_t reason = legacy_slot_actor_migrate(
            w, migration->legacy_contract_actor_code[row],
            true, &contract->claimed_by_principal,
            &legacy_actor_code);
        contract->claimed_by = -1;
        if (reason != OWNERSHIP_QUARANTINE_REASON_NONE &&
            !ownership_migration_stage_row(
                migration, &w->ownership_quarantine,
                &next_record_id, &record_id_initialized,
                OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                reason, OWNERSHIP_QUARANTINE_NA,
                (uint16_t)row, legacy_actor_code,
                &contract->claimed_by_quarantine_record_id)) {
            return false;
        }
      }

      for (int row = 0; row < MAX_DELIVERY_SHIPMENTS; row++) {
        delivery_shipment_t *shipment =
            &w->delivery_shipments[row];
        if (!migration->legacy_delivery_present[row])
            return false;
        shipment->debtor_player = UINT8_MAX;
        if (!shipment->active) {
            shipment->debtor_principal =
                actor_principal_none();
            shipment->debtor_quarantine_record_id = 0;
            continue;
        }
        uint16_t legacy_actor_code = OWNERSHIP_QUARANTINE_NA;
        uint8_t reason = legacy_slot_actor_migrate(
            w, migration->legacy_delivery_actor_code[row],
            false, &shipment->debtor_principal,
            &legacy_actor_code);
        if (reason != OWNERSHIP_QUARANTINE_REASON_NONE &&
            !ownership_migration_stage_row(
                migration, &w->ownership_quarantine,
                &next_record_id, &record_id_initialized,
                OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT,
                reason, OWNERSHIP_QUARANTINE_NA,
                (uint16_t)row, legacy_actor_code,
                &shipment->debtor_quarantine_record_id)) {
            return false;
        }
      }
    }

    if (loaded_version < 82) {
      for (int row = 0; row < MAX_CARGO_PODS; row++) {
        cargo_pod_t *pod = &w->cargo_pods[row];
        const legacy_cargo_pod_tow_evidence_t *evidence =
            &migration->cargo_pods[row];
        if (!pod->active) {
            if (evidence->present) return false;
            continue;
        }
        if (!evidence->present) return false;
        pod->tow_owner_principal =
            actor_principal_none();
        pod->tow_owner_quarantine_record_id = 0;

        int actor_code = (int)evidence->player_slot;
        if (actor_code == -1 &&
            !evidence->module_bound) {
            continue;
        }
        if (actor_code == -1 &&
            evidence->module_bound) {
            continue;
        }

        uint8_t reason =
            OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL;
        uint16_t legacy_actor_code =
            OWNERSHIP_QUARANTINE_NA;
        if (actor_code >= 0 &&
            actor_code < MAX_PLAYERS) {
            legacy_actor_code =
                (uint16_t)actor_code;
            reason = evidence->module_bound
                ? OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL
                : OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN;
        }
        tractor_binding_clear(&pod->tractor);
        if (!ownership_migration_stage_row(
                migration, &w->ownership_quarantine,
                &next_record_id,
                &record_id_initialized,
                OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR,
                reason, OWNERSHIP_QUARANTINE_NA,
                (uint16_t)row, legacy_actor_code,
                &pod->tow_owner_quarantine_record_id)) {
            return false;
        }
      }
    }
    return ownership_quarantine_add_batch(
        &w->ownership_quarantine,
        migration->staged_rows,
        migration->staged_row_count);
}

static const ownership_quarantine_entry_t *
ownership_quarantine_find_record(
    const ownership_quarantine_t *quarantine,
    uint64_t record_id,
    uint16_t *index_out) {
    if (index_out) *index_out = UINT16_MAX;
    if (!quarantine || record_id == 0) return NULL;
    uint16_t low = 0;
    uint16_t high = quarantine->count;
    while (low < high) {
        uint16_t mid = (uint16_t)(
            low + (uint16_t)((high - low) / 2));
        const ownership_quarantine_entry_t *row =
            &quarantine->entries[mid];
        if (row->record_id < record_id) {
            low = (uint16_t)(mid + 1);
        } else {
            high = mid;
        }
    }
    if (low >= quarantine->count ||
        quarantine->entries[low].record_id != record_id) {
        return NULL;
    }
    if (index_out) *index_out = low;
    return &quarantine->entries[low];
}

static bool ownership_quarantine_owner_reason(uint8_t reason) {
    return reason ==
               OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN ||
           reason ==
               OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN ||
           reason ==
               OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL ||
           reason ==
               OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
}

static bool ownership_quarantine_bind_once(
    const ownership_quarantine_t *quarantine,
    uint64_t record_id,
    uint8_t expected_source,
    bool build_mode_reason,
    bool used[OWNERSHIP_QUARANTINE_CAP]) {
    uint16_t index = UINT16_MAX;
    const ownership_quarantine_entry_t *row =
        ownership_quarantine_find_record(
            quarantine, record_id, &index);
    if (!row || index >= OWNERSHIP_QUARANTINE_CAP ||
        used[index] ||
        row->source_kind != expected_source) {
        return false;
    }
    bool reason_ok = build_mode_reason
        ? row->reason ==
              OWNERSHIP_QUARANTINE_REASON_LEGACY_BUILD_MODE_UNPROVEN
        : ownership_quarantine_owner_reason(row->reason);
    if (!reason_ok) return false;
    used[index] = true;
    return true;
}

static bool validate_v79_ownership(const world_t *w) {
    if (!w ||
        !ownership_quarantine_validate(
            &w->ownership_quarantine)) {
        return false;
    }
    bool used[OWNERSHIP_QUARANTINE_CAP] = {false};
    for (int station = 0; station < MAX_STATIONS; station++) {
        const station_t *st = &w->stations[station];
        if (st->pending_ship_build_count < 0 ||
            st->pending_ship_build_count > 4) {
            return false;
        }
        for (int row = 0; row < st->pending_ship_build_count; row++) {
            const pending_ship_build_t *build =
                &st->pending_ship_builds[row];
            if (build->mode >=
                PENDING_SHIP_BUILD_MODE_COUNT) {
                return false;
            }
            bool unknown =
                build->mode == PENDING_SHIP_BUILD_MODE_UNKNOWN;
            bool owner_none =
                build->owner_principal.kind ==
                    ACTOR_PRINCIPAL_NONE;
            bool mode_bound =
                build->mode_quarantine_record_id != 0;
            bool owner_bound =
                build->owner_quarantine_record_id != 0;
            if (unknown != mode_bound ||
                owner_none != owner_bound) {
                return false;
            }
            if (mode_bound &&
                !ownership_quarantine_bind_once(
                    &w->ownership_quarantine,
                    build->mode_quarantine_record_id,
                    OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD,
                    true, used)) {
                return false;
            }
            if (owner_bound &&
                !ownership_quarantine_bind_once(
                    &w->ownership_quarantine,
                    build->owner_quarantine_record_id,
                    OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD,
                    false, used)) {
                return false;
            }
            if (!owner_none &&
                (build->owner_principal.kind !=
                     ACTOR_PRINCIPAL_PLAYER &&
                 build->owner_principal.kind !=
                     ACTOR_PRINCIPAL_STATION)) {
                return false;
            }
        }
    }
    for (int row = 0; row < MAX_SHIP_ASSETS; row++) {
        const ship_asset_t *asset = &w->ship_assets[row];
        if (!asset->active) continue;
        bool owner_none =
            asset->owner_principal.kind == ACTOR_PRINCIPAL_NONE;
        bool owner_bound =
            asset->owner_quarantine_record_id != 0;
        if (owner_none != owner_bound) return false;
        if (owner_none) {
            if (asset->status != SHIP_ASSET_STATUS_STORED &&
                asset->status != SHIP_ASSET_STATUS_DESTROYED) {
                return false;
            }
            if (asset->operator_kind != SHIP_ASSET_OPERATOR_NONE ||
                asset->operator_slot != -1 ||
                !ownership_quarantine_bind_once(
                    &w->ownership_quarantine,
                    asset->owner_quarantine_record_id,
                    OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET,
                    false, used)) {
                return false;
            }
        } else if (
            asset->owner_principal.kind != ACTOR_PRINCIPAL_PLAYER &&
            asset->owner_principal.kind != ACTOR_PRINCIPAL_STATION) {
            return false;
        }
        if (asset->loaner &&
            asset->owner_principal.kind !=
                ACTOR_PRINCIPAL_STATION) {
            return false;
        }
    }
    for (int row = 0; row < MAX_CONTRACTS; row++) {
        const contract_t *contract = &w->contracts[row];
        if (!contract_ownership_is_valid(contract))
            return false;
        if (contract->claimed_by_quarantine_record_id != 0 &&
            !ownership_quarantine_bind_once(
                &w->ownership_quarantine,
                contract->claimed_by_quarantine_record_id,
                OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                false, used)) {
            return false;
        }
    }
    for (int row = 0; row < MAX_DELIVERY_SHIPMENTS; row++) {
        const delivery_shipment_t *shipment =
            &w->delivery_shipments[row];
        if (!delivery_ownership_is_valid(shipment))
            return false;
        if (shipment->active &&
            shipment->debtor_principal.kind ==
                ACTOR_PRINCIPAL_NONE &&
            shipment->debtor_quarantine_record_id == 0) {
            return false;
        }
        if (shipment->debtor_quarantine_record_id != 0 &&
            !ownership_quarantine_bind_once(
                &w->ownership_quarantine,
                shipment->debtor_quarantine_record_id,
                OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT,
                false, used)) {
            return false;
        }
    }
    for (int row = 0; row < MAX_CARGO_PODS; row++) {
        const cargo_pod_t *pod =
            &w->cargo_pods[row];
        if (!pod->active) continue;
        if (!actor_principal_is_canonical(
                &pod->tow_owner_principal) ||
            (pod->tow_owner_principal.kind !=
                 ACTOR_PRINCIPAL_NONE &&
             pod->tow_owner_principal.kind !=
                 ACTOR_PRINCIPAL_UNATTRIBUTED &&
             pod->tow_owner_principal.kind !=
                 ACTOR_PRINCIPAL_PLAYER)) {
            return false;
        }

        bool quarantined =
            pod->tow_owner_quarantine_record_id != 0;
        if (quarantined) {
            const ownership_quarantine_entry_t *entry =
                ownership_quarantine_find_record(
                    &w->ownership_quarantine,
                    pod->tow_owner_quarantine_record_id,
                    NULL);
            if (pod->tow_owner_principal.kind !=
                    ACTOR_PRINCIPAL_NONE ||
                pod->tractor.kind !=
                    TRACTOR_SOURCE_NONE ||
                !entry ||
                entry->station_index !=
                    OWNERSHIP_QUARANTINE_NA ||
                entry->row_index != (uint16_t)row ||
                !ownership_quarantine_bind_once(
                    &w->ownership_quarantine,
                    pod->tow_owner_quarantine_record_id,
                    OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR,
                    false, used)) {
                return false;
            }
            continue;
        }

        if (pod->tractor.kind ==
            TRACTOR_SOURCE_PLAYER) {
            int player_idx =
                cargo_pod_player_tractor(pod);
            if (player_idx < 0 ||
                player_idx >= MAX_PLAYERS) {
                return false;
            }
            if (pod->tow_owner_principal.kind ==
                ACTOR_PRINCIPAL_PLAYER) {
                actor_principal_t player =
                    actor_principal_none();
                if (!actor_principal_from_verified_player(
                        &w->players[player_idx],
                        &player) ||
                    !actor_principal_equal(
                        &pod->tow_owner_principal,
                        &player)) {
                    return false;
                }
            } else if (
                pod->tow_owner_principal.kind !=
                    ACTOR_PRINCIPAL_UNATTRIBUTED) {
                return false;
            }
        } else if (
            pod->tractor.kind ==
            TRACTOR_SOURCE_STATION_MODULE) {
            if (pod->tow_owner_principal.kind !=
                    ACTOR_PRINCIPAL_NONE ||
                !cargo_pod_module_tractor_indices(
                    pod, NULL, NULL)) {
                return false;
            }
        } else if (
            pod->tractor.kind !=
            TRACTOR_SOURCE_NONE) {
            return false;
        }
    }
    return true;
}

static bool world_save_payload(const world_t *w, FILE *f,
                               uint32_t output_version) {
    if (output_version != SAVE_VERSION &&
        output_version != 81 &&
        output_version != 80)
        return false;
    if (!world_validate_station_actor_ids(w) ||
        !validate_v79_ownership(w) ||
        !world_ship_birth_saved_assemblies_valid(w) ||
        !validate_ship_birth_provenance_uniqueness(w)) {
        return false;
    }
    /* Header */
    uint32_t magic = SAVE_MAGIC;
    uint32_t version = output_version;
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
        if (!write_station_session(f, &w->stations[i])) return false;
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
                return false;
            }
        }
    }
    /* v79: sparse, identity-keyed in-flight ship-birth choreography. Raw
     * asteroid slots are transient and are reconstructed from fragment pubs. */
    if (!write_ship_birth_assemblies(f, w)) return false;
    /* Asteroids: terrain remains derived from belt seed */
    /* Scaffolds: removed in v24 — transient in-flight construction */
    /* v37: belt_seed (anchor for rock_pub derivation). v38: each
     * destroyed_rocks entry now carries a destroyed_at_ms timestamp;
     * the array is kept sorted ascending by rock_pub, so writing in
     * index order preserves order on read. Sparse: count + N tuples.
     * v54: world_seq written immediately after belt_seed. */
    WRITE_FIELD(f, w->belt_seed);
    WRITE_FIELD(f, w->world_seq);
    {
        uint16_t count = w->destroyed_rock_count;
        WRITE_FIELD(f, count);
        for (uint16_t i = 0; i < count; i++) {
            if (fwrite(w->destroyed_rocks[i].rock_pub, 32, 1, f) != 1) {
                return false;
            }
            WRITE_FIELD(f, w->destroyed_rocks[i].destroyed_at_ms);
        }
    }
    /* NPC ships */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!write_npc(f, &w->npc_ships[i])) return false;
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!write_contract(
                f, &w->contracts[i], output_version)) {
            return false;
        }
    }
    WRITE_FIELD(f, w->next_delivery_shipment_id);
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
        if (!write_delivery_shipment(
                f, &w->delivery_shipments[i], output_version)) {
            return false;
        }
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
                return false;
            }
            if (fwrite(w->pubkey_registry[r].session_token, 8, 1, f) != 1) {
                return false;
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
            return false;
        }
    }

    /* v64: durable contract-origin ship asset registry. Assigned assets
     * serialize their operator's authoritative live ship directly; the
     * embedded asset ship is a stored/persistence snapshot, not a second
     * live component that must be copied every simulation tick. */
    WRITE_FIELD(f, w->next_ship_asset_id);
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &w->ship_assets[i];
        const ship_t *live_ship = NULL;
        if (asset->active && !asset->destroyed &&
            asset->status == SHIP_ASSET_STATUS_ASSIGNED) {
            if (asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
                asset->operator_slot >= 0 && asset->operator_slot < MAX_PLAYERS) {
                const server_player_t *sp = &w->players[asset->operator_slot];
                if (sp->connected && sp->ship_asset_id == asset->asset_id)
                    live_ship = sp->ship;
            } else if (asset->operator_kind == SHIP_ASSET_OPERATOR_NPC &&
                       asset->operator_slot >= 0 &&
                       asset->operator_slot < MAX_NPC_SHIPS) {
                const npc_ship_t *npc = &w->npc_ships[asset->operator_slot];
                if (npc->active && npc->ship_asset_id == asset->asset_id)
                    live_ship = npc->ship;
            }
        }
        if (!write_ship_asset(f, asset, live_ship)) {
            return false;
        }
    }

    /* v68: active cargo pod tail. Sparse by slot so ship tow indices and
     * shipment-pod references remain stable across reload. */
    {
        uint16_t pod_count = 0;
        for (int i = 0; i < MAX_CARGO_PODS; i++)
            if (w->cargo_pods[i].active) pod_count++;
        WRITE_FIELD(f, pod_count);
        for (int i = 0; i < MAX_CARGO_PODS; i++) {
            if (!w->cargo_pods[i].active) continue;
            if (!write_cargo_pod(
                    f, (uint16_t)i,
                    &w->cargo_pods[i],
                    output_version)) {
                return false;
            }
        }
    }

    /* v84: append-only at-most-once station settlement receipts. Runtime
     * allocation capacity is deliberately omitted. */
    if (output_version >= 84) {
        if (w->payout_journal.count >
                STATION_PAYOUT_JOURNAL_MAX_ENTRIES ||
            w->payout_journal.count > w->payout_journal.capacity ||
            (w->payout_journal.count > 0 &&
             !w->payout_journal.entries)) {
            return false;
        }
        WRITE_FIELD(f, w->payout_journal.count);
        for (uint32_t i = 0; i < w->payout_journal.count; i++) {
            const station_payout_receipt_t *receipt =
                &w->payout_journal.entries[i];
            WRITE_FIELD(f, receipt->payout_id);
            WRITE_FIELD(f, receipt->recipient_hash);
            WRITE_FIELD(f, receipt->station_id);
            WRITE_FIELD(f, receipt->committed_tick);
            WRITE_FIELD(f, receipt->amount);
            WRITE_FIELD(f, receipt->action);
            WRITE_FIELD(f, receipt->authority_generation);
            WRITE_FIELD(f, receipt->_pad);
        }
    }

    /* v78: bounded, server-only legacy ownership quarantine. Keep this
     * immediately before CRC2 so its corruption fixtures remain a stable
     * tail contract across later append-only sections. */
    if (!write_ownership_quarantine(f, &w->ownership_quarantine))
        return false;

    return true;
}

void world_apply_starter_stock_migrations(world_t *w, uint32_t version) {
    if (!w) return;
    if (version < 71) {
        int seeded = world_ensure_starter_frame_pods(w);
        if (seeded > 0) {
            printf("[save] migrated v%d -> v71: restored %d starter frame pod%s\n",
                   (int)version, seeded, seeded == 1 ? "" : "s");
        }
    }
    if (version < 72) {
        int seeded = world_ensure_starter_laser_module_reserve(w);
        if (seeded > 0) {
            printf("[save] migrated v%d -> v72: restored %d starter laser module%s\n",
                   (int)version, seeded, seeded == 1 ? "" : "s");
        }
    }
    /*
     * The starter work-order marker predates its own save-version hook:
     * some otherwise-current saves therefore have the exact untouched
     * reserve but no order. This content-addressed ensure is safe on every
     * load. A matching inactive marker is a consumed one-shot tombstone and
     * blocks regeneration forever.
     */
    (void)world_ensure_starter_mining_refit_work_order(w);
}

static bool world_save_version(const world_t *w, const char *path,
                               uint32_t output_version) {
    if (!w || !path || !path[0]) return false;
    char tmp_path[272];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) return false;

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return false;
    int prior_writing_version = g_writing_save_version;
    g_writing_save_version = (int)output_version;
    bool ok = world_save_payload(w, f, output_version);
    g_writing_save_version = prior_writing_version;
    if (ok) ok = fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

    FILE *rf = fopen(tmp_path, "rb");
    if (!rf) {
        remove(tmp_path);
        return false;
    }
    uint32_t crc = crc32_file(rf);
    ok = !ferror(rf);
    if (fclose(rf) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

    FILE *af = fopen(tmp_path, "ab");
    if (!af) {
        remove(tmp_path);
        return false;
    }
    uint32_t crc_magic = SAVE_CRC_MAGIC;
    ok = fwrite(&crc_magic, sizeof(crc_magic), 1, af) == 1 &&
         fwrite(&crc, sizeof(crc), 1, af) == 1 &&
         save_flush_durable(af);
    if (fclose(af) != 0) ok = false;
    if (!ok) {
        remove(tmp_path);
        return false;
    }

    /* POSIX rename atomically replaces the destination. The temporary file is
     * durable before replacement, and the parent directory is synced after. */
    if (!save_replace_file(tmp_path, path)) {
        remove(tmp_path);
        return false;
    }
    return true;
}

bool world_save(const world_t *w, const char *path) {
    return world_save_version(w, path, SAVE_VERSION);
}

#if defined(SIGNAL_SAVE_TESTING)
bool world_save_legacy_v80_for_test(
    const world_t *w,
    const char *path) {
    return world_save_version(w, path, 80);
}

bool world_save_legacy_v81_for_test(
    const world_t *w,
    const char *path) {
    return world_save_version(w, path, 81);
}
#endif

static bool world_load_payload(
    world_t *w,
    FILE *f,
    world_load_result_t *result) {
    uint32_t magic, version;
    ownership_v79_migration_t ownership_migration = {0};
    if (!w || !f || !result) return false;
    *result = WORLD_LOAD_RESULT_MALFORMED;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&version, sizeof(version), 1, f) != 1) {
        *result = WORLD_LOAD_RESULT_TRUNCATED;
        return false;
    }
    if (magic != SAVE_MAGIC) {
        return false;
    }
    if (version < MIN_SAVE_VERSION || version > SAVE_VERSION) {
        printf("[save] rejected save: magic=0x%08x version=%u (need %d-%d)\n",
               magic, version, MIN_SAVE_VERSION, SAVE_VERSION);
        *result = WORLD_LOAD_RESULT_UNSUPPORTED_VERSION;
        return false;
    }
    g_loaded_save_version = (int)version;

    READ_FIELD(f, w->rng);
    READ_FIELD(f, w->time);
    if (!isfinite(w->time) || w->time < 0.0f) return false;
    double loaded_tick = (double)w->time / (double)SIM_DT;
    if (!isfinite(loaded_tick) ||
        loaded_tick > (double)UINT32_MAX) {
        return false;
    }
    w->tick = (uint32_t)llround(loaded_tick);
    READ_FIELD(f, w->field_spawn_timer);
    if (!isfinite(w->field_spawn_timer)) return false;

    /* v25+: station_count header; v24: fixed at 8 */
    int save_station_slots = 8; /* v24 and earlier had MAX_STATIONS=8 */
    if (version >= 25) {
        int32_t sc;
        READ_FIELD(f, sc);
        if (sc < 0 || sc > MAX_STATIONS) return false;
        w->station_count = (int)sc;
        READ_FIELD(f, w->next_station_id);
        if (version >= 27) READ_FIELD(f, w->next_fracture_id);
        else w->next_fracture_id = 0;
        save_station_slots = (version >= 58)
            ? MAX_STATIONS
            : SAVE_STATION_SLOTS_V25;
    } else {
        w->next_fracture_id = 0;
    }

    if (version >= 24) {
        /* v24+: station identity comes from catalog; read session only */
        for (int i = 0; i < save_station_slots; i++) {
            if (!read_station_session(
                    f, &w->stations[i],
                    &ownership_migration, i, result)) {
                return false;
            }
        }
        /* v24→v25 migration: scan for active stations to set station_count */
        if (version < 25) {
            w->station_count = SIGNAL_ROOT_STATION_COUNT;
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
                return false;
            }
            for (uint32_t i = 0; i < fracture_child_count; i++) {
                if (!read_fracture_child(f, w)) {
                    return false;
                }
            }
        }
        if (version >= 79) {
            if (!read_ship_birth_assemblies(f, w)) {
                return false;
            }
        } else {
            memset(w->ship_birth_assemblies, 0,
                   sizeof(w->ship_birth_assemblies));
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
            return world_load_record_oom(result);
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
        if (version >= 54) {
            READ_FIELD(f, w->world_seq);
        } else {
            w->world_seq = 0;
        }
        uint16_t count = 0;
        READ_FIELD(f, count);
        if (count > MAX_DESTROYED_ROCKS) return false;
        for (uint16_t i = 0; i < count; i++) {
            if (fread(w->destroyed_rocks[i].rock_pub, 32, 1, f) != 1) {
                return false;
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
        world_npc_ship_slot_release(w, i);
        memset(&w->npc_ships[i], 0, sizeof(w->npc_ships[i]));
        if (!world_npc_ship_slot_activate(w, i))
            return world_load_record_oom(result);
        if (!read_npc(f, &w->npc_ships[i])) return false;
        if (!w->npc_ships[i].active)
            world_npc_ship_slot_release(w, i);
    }
    /* Contracts */
    for (int i = 0; i < MAX_CONTRACTS; i++) {
        if (!read_contract(
                f, &w->contracts[i],
                &ownership_migration, i)) {
            return false;
        }
    }
    memset(w->delivery_shipments, 0, sizeof(w->delivery_shipments));
    w->next_delivery_shipment_id = 1;
    if (version < 59) {
        for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
            ownership_migration.legacy_delivery_present[i] = true;
            ownership_migration.legacy_delivery_actor_code[i] =
                UINT8_MAX;
        }
    }
    if (version >= 59) {
        READ_FIELD(f, w->next_delivery_shipment_id);
        if (w->next_delivery_shipment_id == 0)
            w->next_delivery_shipment_id = 1;
        for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++) {
            if (!read_delivery_shipment(
                    f, &w->delivery_shipments[i], version,
                    &ownership_migration, i)) {
                return false;
            }
        }
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
            return false;
        }
        for (uint32_t r = 0; r < reg_count; r++) {
            if (fread(w->pubkey_registry[r].pubkey, 32, 1, f) != 1) {
                return false;
            }
            if (fread(w->pubkey_registry[r].session_token, 8, 1, f) != 1) {
                return false;
            }
            w->pubkey_registry[r].in_use = true;
        }
    }

    bool characters_rebuilt = false;
    if (version >= 52) {
        /* The v52 NPC manifest tail belongs to each NPC's embedded ship.
         * Rebuild transient character registrations before consuming the
         * tail, then skip the final rebuild. */
        rebuild_characters_from_npcs(w);
        characters_rebuilt = true;
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            ship_t *ship = NULL;
            if (w->npc_ships[i].active)
                ship = world_npc_ship_for(w, i);
            if (!read_npc_ship_manifest_payload(
                    f, ship, result)) {
                return false;
            }
            ship_retire_finished_cargo_slots(ship);
        }
    }

    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        ship_cleanup(&w->ship_assets[i].stored_ship);
        memset(&w->ship_assets[i], 0, sizeof(w->ship_assets[i]));
    }
    w->next_ship_asset_id = 1;
    if (version >= 64) {
        READ_FIELD(f, w->next_ship_asset_id);
        if (w->next_ship_asset_id == SHIP_ASSET_ID_NONE)
            w->next_ship_asset_id = 1;
        for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
            if (!read_ship_asset(
                    f, &w->ship_assets[i],
                    &ownership_migration, i, result)) {
                return false;
            }
        }
    }

    if (version >= 68) {
        uint16_t pod_count = 0;
        memset(w->cargo_pods, 0, sizeof(w->cargo_pods));
        READ_FIELD(f, pod_count);
        if (pod_count > MAX_CARGO_PODS) {
            return false;
        }
        for (uint16_t i = 0; i < pod_count; i++) {
            if (!read_cargo_pod(
                    f, w, version,
                    &ownership_migration)) {
                return false;
            }
        }
    }

    free(w->payout_journal.entries);
    w->payout_journal = (station_payout_journal_t){0};
    if (version >= 84) {
        uint32_t count = 0;
        READ_FIELD(f, count);
        if (count > STATION_PAYOUT_JOURNAL_MAX_ENTRIES) {
            return false;
        }
        if (count > 0) {
            w->payout_journal.entries = calloc(
                count, sizeof(*w->payout_journal.entries));
            if (!w->payout_journal.entries)
                return world_load_record_oom(result);
        }
        for (uint32_t i = 0; i < count; i++) {
            station_payout_receipt_t *receipt =
                &w->payout_journal.entries[i];
            READ_FIELD(f, receipt->payout_id);
            READ_FIELD(f, receipt->recipient_hash);
            READ_FIELD(f, receipt->station_id);
            READ_FIELD(f, receipt->committed_tick);
            READ_FIELD(f, receipt->amount);
            READ_FIELD(f, receipt->action);
            READ_FIELD(f, receipt->authority_generation);
            READ_FIELD(f, receipt->_pad);
            bool payout_nonzero = false;
            bool recipient_nonzero = false;
            bool station_known = false;
            for (size_t b = 0; b < 32; b++) {
                payout_nonzero |= receipt->payout_id[b] != 0;
                recipient_nonzero |=
                    receipt->recipient_hash[b] != 0;
            }
            for (int station_idx = 0;
                 station_idx < MAX_STATIONS; station_idx++) {
                if (station_exists(&w->stations[station_idx]) &&
                    w->stations[station_idx].id == receipt->station_id) {
                    station_known = true;
                    break;
                }
            }
            if (!payout_nonzero || !recipient_nonzero ||
                receipt->station_id == 0 || !station_known ||
                !isfinite(receipt->amount) || receipt->amount <= 0.0f ||
                receipt->amount > LEDGER_FLOAT_LIMIT ||
                receipt->action <= STATION_PAYOUT_NONE ||
                receipt->action >= STATION_PAYOUT_COUNT ||
                receipt->_pad[0] != 0 || receipt->_pad[1] != 0) {
                return false;
            }
            for (uint32_t prior = 0; prior < i; prior++) {
                if (memcmp(
                        w->payout_journal.entries[prior].payout_id,
                        receipt->payout_id, 32) == 0) {
                    return false;
                }
            }
        }
        w->payout_journal.count = count;
        w->payout_journal.capacity = count;
    }

    if (version >= 78) {
        if (!read_ownership_quarantine(
                f, &w->ownership_quarantine)) {
            return false;
        }
    } else {
        ownership_quarantine_clear(&w->ownership_quarantine);
    }

    /* Every supported world-save version has a CRC32 trailer. Require it at
     * the exact payload boundary so appended or truncated data cannot be
     * mistaken for a legacy save. */
    {
        long data_end = ftell(f);
        uint32_t trail_magic = 0;
        uint32_t stored_crc = 0;
        if (data_end >= 0 &&
            g_loaded_save_payload_end >= 0 &&
            data_end < g_loaded_save_payload_end) {
            printf("[save] trailing data after CRC32 trailer\n");
            *result = WORLD_LOAD_RESULT_TRAILING_DATA;
            return false;
        }
        if (data_end < 0 ||
            data_end != g_loaded_save_payload_end) {
            return false;
        }
        if (fread(&trail_magic, sizeof(trail_magic), 1, f) != 1 ||
            trail_magic != SAVE_CRC_MAGIC ||
            fread(&stored_crc, sizeof(stored_crc), 1, f) != 1) {
            printf("[save] missing or truncated CRC32 trailer\n");
            *result = WORLD_LOAD_RESULT_TRUNCATED;
            return false;
        }
        uint32_t crc = 0;
        if (!crc32_file_prefix(f, data_end, &crc)) {
            return false;
        }
        if (crc != stored_crc) {
            printf("[save] CRC32 mismatch: computed=0x%08x stored=0x%08x -- save may be corrupt\n",
                   crc, stored_crc);
            *result = WORLD_LOAD_RESULT_CHECKSUM;
            return false;
        }
    }

    /*
     * Freeze stable station actors from the saved pre-rekey public identity,
     * then migrate every legacy hull/build owner against that immutable
     * namespace. Signing authority may rotate later in this load.
     */
    if (version >= 79) {
        if (!world_validate_station_actor_ids(w)) return false;
    } else {
        if (!world_migrate_legacy_station_actor_ids(w)) return false;
    }
    if (version < 82 &&
        !migrate_legacy_ownership(
            w, &ownership_migration, version)) {
        return false;
    }
    if (!validate_v79_ownership(w)) return false;

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

    if (version < 66) {
        for (int i = 0; i < MAX_STATIONS; i++) {
            if (i >= w->station_count && !station_exists(&w->stations[i]))
                continue;
            station_faction_seed_station(&w->stations[i], i);
        }
    }

    world_apply_starter_stock_migrations(w, version);

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
                }
                st->modules[kept].type = (module_type_t)new_t;
                kept++;
            }
            for (int m = kept; m < st->module_count; m++) {
                memset(&st->modules[m], 0, sizeof(st->modules[m]));
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
        world_player_ship_slot_release(w, i);
        world_player_runtime_slot_reset(w, i);
        if (!world_player_ship_slot_activate(w, i))
            return world_load_record_oom(result);
    }
    if (!world_ship_birth_rebind_saved_assemblies(w) ||
        !validate_ship_birth_provenance_uniqueness(w)) {
        return false;
    }

    belt_field_init(&w->belt, w->rng, BELT_SCALE);
    rebuild_signal_chain(w);
    if (!characters_rebuilt)
        rebuild_characters_from_npcs(w);
    (void)world_ship_assets_ensure_legacy_bindings(w);
    /* Layer B of #479: rederive every station's private key from the
     * operator-held station authority secret plus persisted provenance.
     * The secret was never written to disk — this is what makes a save
     * leak NOT a key leak. v39 and earlier saves additionally rederive
     * the pubkey itself (indices below SIGNAL_SEEDED_STATION_COUNT from the
     * world seed; outposts from a zero-founder placeholder, accepted v39
     * provenance gap).
     *
     * We rederive seeded slots unconditionally — they always exist in
     * any reachable world state — and also any outpost slot
     * whose pubkey is non-zero (i.e. the slot was occupied at save
     * time). station_exists() depends on geometry fields that may
     * legitimately be zeroed in catalog-less test scenarios, so it's
     * not the right gate here. */
    static const uint8_t zero_pub[32] = {0};
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (i < SIGNAL_SEEDED_STATION_COUNT ||
            memcmp(w->stations[i].station_pubkey, zero_pub, 32) != 0) {
            station_authority_rederive_result_t rederive_result =
                station_authority_rederive_secret(&w->stations[i],
                                                  w->belt_seed, i);
            if (rederive_result ==
                STATION_AUTHORITY_REDERIVE_REJECTED) {
                SIM_LOG("[chain] station %d (%s): configured authority "
                        "rekey rejected by lifecycle registry\n",
                        i, w->stations[i].name);
                return false;
            }
            if (rederive_result ==
                STATION_AUTHORITY_REDERIVE_REKEYED) {
                station_t *st = &w->stations[i];
                st->chain_event_count = 0;
                memset(st->chain_last_hash, 0, sizeof(st->chain_last_hash));
                chain_log_health_set(st, CHAIN_HEALTH_EMPTY, false, 0,
                                     st->chain_last_hash,
                                     "station authority rekeyed at load");
                SIM_LOG("[chain] station %d (%s): station authority rekeyed; "
                        "starting fresh chain identity\n",
                        i, st->name);
            }
        }
    }
    world_ensure_seeded_freeport(w);
    rebuild_signal_chain(w);
    /* Layer C of #479: walk every station's chain log on disk and
     * verify it against its station_pubkey. A corrupt chain (bad
     * signature, broken prev_hash linkage, or last_hash mismatch
     * vs. the saved continuation pointer) is now an explicit runtime
     * health state. Unsafe stations block future appends until an
     * operator repairs the save/log pairing; otherwise the first startup
     * anchor could fork an already-divergent log. */
    for (int i = 0; i < MAX_STATIONS; i++) {
        station_t *st = &w->stations[i];
        if (memcmp(st->station_pubkey, zero_pub, 32) == 0) continue;
        uint64_t walked = 0;
        uint8_t walked_last[32] = {0};
        chain_log_verify_report_t report;
        if (!chain_log_verify_station(st, &walked, walked_last, &report)) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "verification failed after %llu valid events: %s",
                     (unsigned long long)walked,
                     report.first_fail_reason[0]
                         ? report.first_fail_reason
                         : "unknown verifier failure");
            chain_log_health_set(st, CHAIN_HEALTH_FAILED, true,
                                 walked, walked_last, msg);
            SIM_LOG("[chain] OPERATOR WARNING station %d (%s): %s; "
                    "chain appends blocked, log untouched\n",
                    i, st->name, msg);
            continue;
        }
        /*
         * A verified disk tail ahead of the selected snapshot is not safe to
         * adopt. Durable chain append happens before several gameplay commits
         * (cargo moves, ledger updates, construction progress, production),
         * so the extra records do not prove that their mutations reached the
         * selected world/player generation. Advancing only the continuation
         * pointer would silently bless an event whose gameplay effect may be
         * absent. Preserve both sides for recovery and block further appends
         * until a matching generation is restored or an exactly-once
         * replay/rollback path resolves the tail.
         */
        uint64_t saved_count = st->chain_event_count;
        if (walked > saved_count) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "verified tail ahead of snapshot "
                     "(disk: %llu events, save: %llu; mutation not replayed)",
                     (unsigned long long)walked,
                     (unsigned long long)saved_count);
            chain_log_health_set(st, CHAIN_HEALTH_MISMATCH, true,
                                 walked, walked_last, msg);
            SIM_LOG("[chain] OPERATOR WARNING station %d (%s): %s; "
                    "chain appends blocked, log untouched\n",
                    i, st->name, msg);
        } else if (walked < saved_count
                   || memcmp(walked_last, st->chain_last_hash, 32) != 0) {
            /* Save claims more events than disk, or same length but a
             * different tail. Either is a real divergence (truncated
             * log, restored save against a different chain dir). Loud
             * + non-fatal: appends would fork either way. */
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "continuation mismatch (disk: %llu events, save: %llu)",
                     (unsigned long long)walked,
                     (unsigned long long)saved_count);
            chain_log_health_set(st, CHAIN_HEALTH_MISMATCH, true,
                                 walked, walked_last, msg);
            SIM_LOG("[chain] OPERATOR WARNING station %d (%s): %s; "
                    "chain appends blocked to avoid forking from the saved head\n",
                    i, st->name, msg);
        } else {
            chain_log_health_set(st, walked == 0 ? CHAIN_HEALTH_EMPTY : CHAIN_HEALTH_OK,
                                 false, walked, walked_last,
                                 walked == 0
                                     ? "verified empty chain"
                                     : "verified chain tail matches save");
        }
    }
    /* Gossip pools are ephemeral (not serialized). Rebuild each
     * station's local view from loaded station/contract state so
     * reset/load does not invent peer-station radio. */
    for (int i = 0; i < MAX_CONTRACTS; i++)
        contract_ownership_refresh_projection(
            w, &w->contracts[i]);
    for (int i = 0; i < MAX_DELIVERY_SHIPMENTS; i++)
        delivery_ownership_refresh_projection(
            w, &w->delivery_shipments[i]);
    /* Physical pod stock is derived and deliberately absent from the save.
     * Rebuild it from the loaded custody/manifest state before any UI,
     * pricing, replay digest, or automation can observe the world. */
    world_refresh_station_physical_inventories(w);
    gossip_bootstrap_world_stations(w);
    *result = WORLD_LOAD_RESULT_OK;
    return true;
}

const char *world_load_result_name(world_load_result_t result) {
    switch (result) {
    case WORLD_LOAD_RESULT_OK:
        return "ok";
    case WORLD_LOAD_RESULT_INVALID_ARGUMENT:
        return "invalid-argument";
    case WORLD_LOAD_RESULT_IO:
        return "io";
    case WORLD_LOAD_RESULT_TOO_LARGE:
        return "too-large";
    case WORLD_LOAD_RESULT_TRUNCATED:
        return "truncated";
    case WORLD_LOAD_RESULT_CHECKSUM:
        return "checksum";
    case WORLD_LOAD_RESULT_UNSUPPORTED_VERSION:
        return "unsupported-version";
    case WORLD_LOAD_RESULT_MALFORMED:
        return "malformed";
    case WORLD_LOAD_RESULT_TRAILING_DATA:
        return "trailing-data";
    case WORLD_LOAD_RESULT_OUT_OF_MEMORY:
        return "out-of-memory";
    default:
        return "unknown";
    }
}

static bool world_load_size_error_is_too_large(void) {
#ifdef EOVERFLOW
    if (errno == EOVERFLOW) return true;
#endif
#ifdef EFBIG
    if (errno == EFBIG) return true;
#endif
    return false;
}

/*
 * Measure with the platform's 64-bit seek API before entering the legacy
 * long-offset CRC/parser path. This keeps an LLP64 path above LONG_MAX from
 * being misreported as generic I/O; the public cap guarantees that the cast
 * to long below is safe after this check succeeds.
 */
static world_load_result_t world_load_stream_size(
    FILE *f,
    uint64_t *out_size) {
    if (!f || !out_size) return WORLD_LOAD_RESULT_INVALID_ARGUMENT;
    *out_size = 0;
    errno = 0;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        return world_load_size_error_is_too_large()
            ? WORLD_LOAD_RESULT_TOO_LARGE
            : WORLD_LOAD_RESULT_IO;
    }
    errno = 0;
    __int64 end = _ftelli64(f);
#else
    if (fseeko(f, (off_t)0, SEEK_END) != 0) {
        return world_load_size_error_is_too_large()
            ? WORLD_LOAD_RESULT_TOO_LARGE
            : WORLD_LOAD_RESULT_IO;
    }
    errno = 0;
    off_t end = ftello(f);
#endif
    if (end < 0) {
        return world_load_size_error_is_too_large()
            ? WORLD_LOAD_RESULT_TOO_LARGE
            : WORLD_LOAD_RESULT_IO;
    }
    *out_size = (uint64_t)end;
    if (*out_size > WORLD_SAVE_MAX_BYTES)
        return WORLD_LOAD_RESULT_TOO_LARGE;
    return WORLD_LOAD_RESULT_OK;
}

static world_load_result_t world_load_precheck_crc(
    FILE *f,
    long *payload_end) {
    if (!f || !payload_end) return WORLD_LOAD_RESULT_INVALID_ARGUMENT;
    *payload_end = -1;
    uint64_t stream_size = 0;
    world_load_result_t size_result =
        world_load_stream_size(f, &stream_size);
    if (size_result != WORLD_LOAD_RESULT_OK) return size_result;
    long len = (long)stream_size;
    if (len < (long)(sizeof(uint32_t) * 4)) {
        printf("[save] missing or truncated CRC32 trailer\n");
        return WORLD_LOAD_RESULT_TRUNCATED;
    }

    long trailer = len - (long)(sizeof(uint32_t) * 2);
    uint32_t magic = 0;
    uint32_t stored_crc = 0;
    if (fseek(f, trailer, SEEK_SET) != 0 ||
        fread(&magic, sizeof(magic), 1, f) != 1 ||
        fread(&stored_crc, sizeof(stored_crc), 1, f) != 1) {
        return WORLD_LOAD_RESULT_IO;
    }
    if (magic != SAVE_CRC_MAGIC) {
        printf("[save] missing or truncated CRC32 trailer\n");
        return WORLD_LOAD_RESULT_TRUNCATED;
    }
    uint32_t crc = 0;
    if (!crc32_file_prefix(f, trailer, &crc)) {
        return WORLD_LOAD_RESULT_IO;
    }
    if (crc != stored_crc) {
        printf("[save] CRC32 mismatch before decode: computed=0x%08x stored=0x%08x\n",
               crc, stored_crc);
        return WORLD_LOAD_RESULT_CHECKSUM;
    }
    if (fseek(f, 0, SEEK_SET) != 0) return WORLD_LOAD_RESULT_IO;
    *payload_end = trailer;
    return WORLD_LOAD_RESULT_OK;
}

static world_t *world_load_candidate_create(const world_t *source) {
    if (!source) return NULL;
    world_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) return NULL;

    /*
     * Supported legacy saves intentionally overlay a reset/catalog baseline:
     * fields introduced after their version retain those baseline defaults.
     * Start from the exact caller state, but detach every owned allocation
     * before cloning so candidate cleanup can never free source storage.
     */
    *candidate = *source;
    for (int i = 0; i < MAX_STATIONS; i++) {
        candidate->stations[i].cargo_store = (cargo_store_t){0};
    }
    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        candidate->ships[i].component.cargo_store =
            (cargo_store_t){0};
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        candidate->ship_assets[i].stored_ship.cargo_store =
            (cargo_store_t){0};
    }
    memset(&candidate->signal_cache, 0,
           sizeof(candidate->signal_cache));
    memset(&candidate->asteroid_grid, 0,
           sizeof(candidate->asteroid_grid));
    candidate->payout_journal = (station_payout_journal_t){0};

    for (int i = 0; i < MAX_STATIONS; i++) {
        if (!station_copy(&candidate->stations[i],
                          &source->stations[i])) {
            world_cleanup(candidate);
            free(candidate);
            return NULL;
        }
    }
    for (int i = 0; i < WORLD_SHIP_CAP; i++) {
        if (!ship_copy(&candidate->ships[i].component,
                       &source->ships[i].component)) {
            world_cleanup(candidate);
            free(candidate);
            return NULL;
        }
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (!ship_copy(
                &candidate->ship_assets[i].stored_ship,
                &source->ship_assets[i].stored_ship)) {
            world_cleanup(candidate);
            free(candidate);
            return NULL;
        }
    }
    if (source->payout_journal.count > 0) {
        if (source->payout_journal.count > source->payout_journal.capacity ||
            source->payout_journal.count >
                STATION_PAYOUT_JOURNAL_MAX_ENTRIES ||
            !source->payout_journal.entries) {
            world_cleanup(candidate);
            free(candidate);
            return NULL;
        }
        candidate->payout_journal.entries = calloc(
            source->payout_journal.count,
            sizeof(*candidate->payout_journal.entries));
        if (!candidate->payout_journal.entries) {
            world_cleanup(candidate);
            free(candidate);
            return NULL;
        }
        memcpy(candidate->payout_journal.entries,
               source->payout_journal.entries,
               (size_t)source->payout_journal.count *
                   sizeof(*candidate->payout_journal.entries));
        candidate->payout_journal.count = source->payout_journal.count;
        candidate->payout_journal.capacity = source->payout_journal.count;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        candidate->players[i].connection =
            &candidate->connections[i];
        candidate->players[i].replication =
            &candidate->replications[i];
    }
    world_rebind_ship_controllers(candidate);
    return candidate;
}

world_t *world_snapshot_clone_create(const world_t *source) {
    return world_load_candidate_create(source);
}

void world_snapshot_clone_destroy(world_t *snapshot) {
    if (!snapshot) return;
    world_cleanup(snapshot);
    free(snapshot);
}

static world_load_result_t world_load_owned_stream(
    world_t *w,
    FILE *f) {
    if (!w || !f) {
        if (f) (void)fclose(f);
        return WORLD_LOAD_RESULT_INVALID_ARGUMENT;
    }

    long payload_end = -1;
    world_load_result_t result =
        world_load_precheck_crc(f, &payload_end);
    if (result != WORLD_LOAD_RESULT_OK) {
        (void)fclose(f);
        return result;
    }

    world_t *candidate = world_load_candidate_create(w);
    if (!candidate) {
        (void)fclose(f);
        return WORLD_LOAD_RESULT_OUT_OF_MEMORY;
    }
    int previous_loaded_version = g_loaded_save_version;
    long previous_payload_end = g_loaded_save_payload_end;
    g_loaded_save_payload_end = payload_end;
    bool decoded = world_load_payload(candidate, f, &result);
    bool closed = fclose(f) == 0;
    g_loaded_save_version = previous_loaded_version;
    g_loaded_save_payload_end = previous_payload_end;
    if (!decoded && result == WORLD_LOAD_RESULT_OK)
        result = WORLD_LOAD_RESULT_MALFORMED;
    if (!closed && result == WORLD_LOAD_RESULT_OK)
        result = WORLD_LOAD_RESULT_IO;

    if (decoded && result == WORLD_LOAD_RESULT_OK) {
        spatial_grid_build(candidate);
        world_cleanup(w);
        *w = *candidate;
        memset(candidate, 0, sizeof(*candidate));
        for (int i = 0; i < MAX_PLAYERS; i++) {
            w->players[i].connection = &w->connections[i];
            w->players[i].replication = &w->replications[i];
        }
        world_rebind_ship_controllers(w);
        if (w->ownership_quarantine.count > 0) {
            (void)ownership_quarantine_report_bounded(
                stderr, &w->ownership_quarantine,
                OWNERSHIP_QUARANTINE_AUTO_REPORT_ROWS);
        }
    } else {
        world_cleanup(candidate);
    }
    free(candidate);
    return result;
}

world_load_result_t world_load_path(world_t *w, const char *path) {
    if (!w || !path || !path[0])
        return WORLD_LOAD_RESULT_INVALID_ARGUMENT;
    FILE *f = fopen(path, "rb");
    if (!f) return WORLD_LOAD_RESULT_IO;
    return world_load_owned_stream(w, f);
}

world_load_result_t world_load_bytes(
    world_t *w,
    const uint8_t *bytes,
    size_t size) {
    if (!w || (!bytes && size > 0))
        return WORLD_LOAD_RESULT_INVALID_ARGUMENT;
    if ((uint64_t)size > WORLD_SAVE_MAX_BYTES)
        return WORLD_LOAD_RESULT_TOO_LARGE;
    if (size == 0) return WORLD_LOAD_RESULT_TRUNCATED;

#ifdef _WIN32
    FILE *f = tmpfile();
    if (!f) return WORLD_LOAD_RESULT_IO;
    if (fwrite(bytes, 1, size, f) != size ||
        fseek(f, 0, SEEK_SET) != 0) {
        (void)fclose(f);
        return WORLD_LOAD_RESULT_IO;
    }
#else
    FILE *f = fmemopen((void *)bytes, size, "rb");
    if (!f) return WORLD_LOAD_RESULT_IO;
#endif
    return world_load_owned_stream(w, f);
}

bool world_load(world_t *w, const char *path) {
    return world_load_path(w, path) == WORLD_LOAD_RESULT_OK;
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
    /* PLY1-PLY7 fixed ship blobs predate Engine Modules. Finished cargo is
     * authoritative in the manifest tail, so keep the historical snapshot
     * width stable and let new commodities live in that tail. */
    float cargo[SAVE_V82_COMMODITY_COUNT];
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
    memcpy(dst->cargo, src->cargo, sizeof(src->cargo));
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
    float cargo[SAVE_V82_COMMODITY_COUNT];
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
    float cargo_snapshot[COMMODITY_COUNT] = {0};
    ship_cargo_snapshot(src, cargo_snapshot);
    memcpy(dst->cargo, cargo_snapshot, sizeof(dst->cargo));
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
    memcpy(dst->cargo, src->cargo, sizeof(src->cargo));
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
        (void)ship_manifest_push_with_chain(dst, &u, NULL);
    }
}

/* Old ship layout with global credits field — for PLY2 migration */
typedef struct {
    vec2 pos; vec2 vel; float angle; float hull;
    float cargo[SAVE_V82_COMMODITY_COUNT];
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
 *   <dir>/pubkey/<base58(pubkey)>.sav   if pubkey proof is verified
 *   <dir>/legacy/<token_hex>.sav        otherwise (anonymous / pre-A.1 client
 *                                       or asserted-but-unverified pubkey)
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

#ifdef _WIN32
static void player_save_windows_namespace_close(
    HANDLE handles[2]) {
    if (!handles) return;
    for (size_t i = 0; i < 2; i++) {
        if (handles[i] != INVALID_HANDLE_VALUE) {
            CloseHandle(handles[i]);
            handles[i] = INVALID_HANDLE_VALUE;
        }
    }
}

static bool player_save_windows_namespace_lock(
    const char *player_dir,
    const char *subdir,
    HANDLE handles[2]) {
    if (!player_dir || !subdir || !handles) return false;
    handles[0] = INVALID_HANDLE_VALUE;
    handles[1] = INVALID_HANDLE_VALUE;
    char namespace_path[256];
    int n = snprintf(
        namespace_path, sizeof(namespace_path),
        "%s/%s", player_dir, subdir);
    if (n <= 0 || (size_t)n >= sizeof(namespace_path))
        return false;
    const char *paths[2] = {player_dir, namespace_path};
    for (size_t i = 0; i < 2; i++) {
        handles[i] = CreateFileA(
            paths[i], FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT,
            NULL);
        BY_HANDLE_FILE_INFORMATION info = {0};
        if (handles[i] == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handles[i], &info) ||
            (info.dwFileAttributes &
             FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            player_save_windows_namespace_close(handles);
            return false;
        }
    }
    return true;
}
#endif

/* Compute the on-disk save path for this player. Returns true if a path
 * was produced; false only if the player has neither a pubkey nor a
 * session_token (a wholly fresh slot — nothing to persist yet). */
bool player_save_path(char *out, size_t outlen, const char *dir,
                      const server_player_t *sp, int slot) {
    static const uint8_t zero_token[8] = {0};
    if (!out || outlen == 0 || !dir || !sp) return false;
    int n = 0;
    if (server_player_can_use_pubkey_persistence(sp)) {
        char b58[64];
        if (base58_encode(sp->pubkey, 32, b58, sizeof(b58)) == 0) return false;
        n = snprintf(out, outlen, "%s/%s/%s.sav",
                     dir, PUBKEY_SUBDIR, b58);
        return n > 0 && (size_t)n < outlen;
    }
    if (sp->session_ready && memcmp(sp->session_token, zero_token, 8) != 0) {
        char hex[17];
        session_token_to_hex(sp->session_token, hex);
        n = snprintf(out, outlen, "%s/%s/player_%s.sav",
                     dir, LEGACY_SUBDIR, hex);
        return n > 0 && (size_t)n < outlen;
    }
    /* Fully anonymous fresh slot — fall back to the slot-numbered path,
     * also under legacy/, so non-token disconnects don't pollute the
     * top-level directory. */
    n = snprintf(out, outlen, "%s/%s/player_%d.sav",
                 dir, LEGACY_SUBDIR, slot);
    return n > 0 && (size_t)n < outlen;
}

static player_save_create_result_t player_save_internal(
    const server_player_t *sp, const char *dir, int slot,
    bool no_replace) {
    char path[256];
    char tmp_path[272];
    char final_leaf[80] = {0};
    char temporary_leaf[96] = {0};
#ifdef _WIN32
    HANDLE no_replace_parents[2] = {
        INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE
    };
#else
    int no_replace_namespace_fd = -1;
#endif
    ship_v4_t ship_disk;
    /* #339 slice A.2: PLY5 format lifts the empty-manifest guard and
     * appends a manifest tail (count + packed cargo_unit_t entries)
     * between the fixed ship blob and the CRC trailer.
     * #479 A.4: filename keyed by pubkey when registered, else by
     * legacy session_token under saves/legacy/. */
    ensure_save_subdirs(dir);
    if (!player_save_path(path, sizeof(path), dir, sp, slot))
        return PLAYER_SAVE_CREATE_IO_FAILURE;
    if (no_replace) {
        const char *leaf = strrchr(path, '/');
        if (!leaf || !leaf[1]) {
            return PLAYER_SAVE_CREATE_IO_FAILURE;
        }
        leaf++;
        int final_len = snprintf(
            final_leaf, sizeof(final_leaf), "%s", leaf);
        int temporary_leaf_len = snprintf(
            temporary_leaf, sizeof(temporary_leaf),
            "%s.recovery-tmp", leaf);
        if (final_len <= 0 ||
            (size_t)final_len >= sizeof(final_leaf) ||
            temporary_leaf_len <= 0 ||
            (size_t)temporary_leaf_len >=
                sizeof(temporary_leaf)) {
            return PLAYER_SAVE_CREATE_IO_FAILURE;
        }
#ifdef _WIN32
        if (!player_save_windows_namespace_lock(
                dir, PUBKEY_SUBDIR,
                no_replace_parents)) {
            return PLAYER_SAVE_CREATE_IO_FAILURE;
        }
#else
#ifndef O_NOFOLLOW
        return PLAYER_SAVE_CREATE_IO_FAILURE;
#else
        int directory_flags =
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
        directory_flags |= O_CLOEXEC;
#endif
        int player_dir_fd = open(dir, directory_flags);
        if (player_dir_fd >= 0) {
            no_replace_namespace_fd = openat(
                player_dir_fd, PUBKEY_SUBDIR,
                directory_flags);
            close(player_dir_fd);
        }
        if (no_replace_namespace_fd < 0)
            return PLAYER_SAVE_CREATE_IO_FAILURE;
#endif
#endif
    }
    int temporary_len = snprintf(
        tmp_path, sizeof(tmp_path), "%s%s", path,
        no_replace ? ".recovery-tmp" : ".tmp");
    if (temporary_len <= 0 ||
        (size_t)temporary_len >= sizeof(tmp_path)) {
#ifdef _WIN32
        player_save_windows_namespace_close(
            no_replace_parents);
#else
        if (no_replace_namespace_fd >= 0)
            close(no_replace_namespace_fd);
#endif
        return PLAYER_SAVE_CREATE_IO_FAILURE;
    }
    FILE *f = NULL;
    if (no_replace) {
#ifdef _WIN32
        int fd = _open(
            tmp_path,
            _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
            _S_IREAD | _S_IWRITE);
#else
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = openat(
            no_replace_namespace_fd,
            temporary_leaf, flags, 0600);
#endif
        if (fd >= 0) {
#ifdef _WIN32
            f = _fdopen(fd, "wb");
#else
            f = fdopen(fd, "wb");
#endif
            if (!f) {
#ifdef _WIN32
                _close(fd);
#else
                close(fd);
#endif
            }
        }
    } else {
        f = fopen(tmp_path, "wb");
    }
    if (!f) {
#ifdef _WIN32
        player_save_windows_namespace_close(
            no_replace_parents);
#else
        if (no_replace_namespace_fd >= 0)
            close(no_replace_namespace_fd);
#endif
        return PLAYER_SAVE_CREATE_IO_FAILURE;
    }
    encode_v4_ship(&ship_disk, sp->ship);
    /* This fixed blob is written and checksummed byte-for-byte.  A
     * designated initializer initializes every member, but C leaves struct
     * padding indeterminate; Valgrind therefore correctly rejects passing
     * the blob to fwrite.  Clear the complete object first so the save is
     * deterministic and never persists bytes from the stack. */
    player_save_data_t data;
    memset(&data, 0, sizeof(data));
    data.magic = PLAYER_MAGIC;
    data.ship = ship_disk;
    data.last_station = sp->current_station;
    data.last_pos = sp->ship->pos;
    data.last_angle = sp->ship->angle;
    bool ok = fwrite(&data, sizeof(data), 1, f) == 1;
    uint32_t crc = ok ? crc32_update(0, &data, sizeof(data)) : 0;
    /* Manifest tail (PLY5). Count + entries; CRC accumulates both. */
    if (ok) {
        uint16_t manifest_count = sp->ship->manifest.count;
        ok = fwrite(&manifest_count, sizeof(manifest_count), 1, f) == 1;
        if (ok) crc = crc32_update(crc, &manifest_count, sizeof(manifest_count));
        for (uint16_t u = 0; ok && u < manifest_count; u++) {
            const cargo_unit_t *cu = &sp->ship->manifest.units[u];
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
        const ship_receipts_t *rcpts = ship_get_receipts_const(sp->ship);
        uint16_t mc = sp->ship->manifest.count;
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
    if (ok) ok = save_flush_durable(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        if (no_replace) {
#ifdef _WIN32
            remove(tmp_path);
            player_save_windows_namespace_close(
                no_replace_parents);
#else
            (void)unlinkat(
                no_replace_namespace_fd,
                temporary_leaf, 0);
            close(no_replace_namespace_fd);
#endif
        } else {
            remove(tmp_path);
        }
        return PLAYER_SAVE_CREATE_IO_FAILURE;
    }
    if (no_replace) {
#ifdef _WIN32
        persistence_publish_no_replace_result_t publish =
            persistence_publish_file_no_replace(tmp_path, path);
        player_save_windows_namespace_close(
            no_replace_parents);
#else
        persistence_publish_no_replace_result_t publish =
            PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE;
        if (linkat(
                no_replace_namespace_fd, temporary_leaf,
                no_replace_namespace_fd, final_leaf, 0) == 0) {
            if (unlinkat(
                    no_replace_namespace_fd,
                    temporary_leaf, 0) == 0 &&
                fsync(no_replace_namespace_fd) == 0) {
                publish =
                    PERSISTENCE_PUBLISH_NO_REPLACE_OK;
            }
        } else if (errno == EEXIST) {
            publish =
                PERSISTENCE_PUBLISH_NO_REPLACE_CONFLICT;
        }
        if (publish != PERSISTENCE_PUBLISH_NO_REPLACE_OK) {
            (void)unlinkat(
                no_replace_namespace_fd,
                temporary_leaf, 0);
        }
        close(no_replace_namespace_fd);
#endif
        if (publish != PERSISTENCE_PUBLISH_NO_REPLACE_OK) {
#ifdef _WIN32
            remove(tmp_path);
#endif
            return publish ==
                       PERSISTENCE_PUBLISH_NO_REPLACE_CONFLICT
                ? PLAYER_SAVE_CREATE_DESTINATION_CONFLICT
                : PLAYER_SAVE_CREATE_IO_FAILURE;
        }
    } else if (!save_replace_file(tmp_path, path)) {
        /* Atomic rename matches world_save(): never remove the previous
         * player save before the replacement is complete. */
        remove(tmp_path);
        return PLAYER_SAVE_CREATE_IO_FAILURE;
    }
    if (ok) SIM_LOG("[sim] saved player %d\n", slot);
    return PLAYER_SAVE_CREATE_OK;
}

bool player_save(const server_player_t *sp, const char *dir, int slot) {
    return player_save_internal(sp, dir, slot, false) ==
           PLAYER_SAVE_CREATE_OK;
}

player_save_create_result_t player_save_no_replace(
    const server_player_t *sp, const char *dir, int slot) {
    if (!server_player_can_use_pubkey_persistence(sp))
        return PLAYER_SAVE_CREATE_IO_FAILURE;
    return player_save_internal(sp, dir, slot, true);
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
    memcpy(dst->cargo, src->cargo, sizeof(src->cargo));
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

static bool player_save_verify_crc32_trailer(FILE *f, const char *path) {
    (void)path;
    if (!f) return false;
    long start = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long len = ftell(f);
    if (len < (long)(sizeof(uint32_t) * 3)) {
        (void)fseek(f, start, SEEK_SET);
        return false;
    }
    long trailer = len - (long)(sizeof(uint32_t) * 2);
    if (fseek(f, trailer, SEEK_SET) != 0) return false;
    uint32_t crc_magic = 0, stored_crc = 0;
    if (fread(&crc_magic, sizeof(crc_magic), 1, f) != 1 ||
        fread(&stored_crc, sizeof(stored_crc), 1, f) != 1 ||
        crc_magic != 0x43524332u) {
        (void)fseek(f, start, SEEK_SET);
        return false;
    }
    uint32_t crc = 0;
    if (!crc32_file_prefix(f, trailer, &crc)) {
        (void)fseek(f, start, SEEK_SET);
        return false;
    }
    if (crc != stored_crc) {
        SIM_LOG("[sim] player save CRC mismatch\n");
        (void)fseek(f, start, SEEK_SET);
        return false;
    }
    return fseek(f, start, SEEK_SET) == 0;
}

static bool player_load_from_file_decode(
    server_player_t *sp,
    world_t *w,
    FILE *f,
    uint32_t magic,
    uint64_t file_size,
    int slot) {
    if (!sp || !w || !f || !sp->ship) return false;
    float migrated_credits = 0.0f;
    bool is_v1 = false;
    bool manifest_already_loaded = false;

    ship_cleanup(sp->ship);

    if (magic == PLAYER_MAGIC || magic == PLAYER_MAGIC_V6 || magic == PLAYER_MAGIC_V5) {
        /* PLY5 (manifest tail), PLY6 (manifest + last_signed_nonce),
         * PLY7 (manifest + last_signed_nonce + receipt chains). */
        player_save_data_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) return false;
        migrate_v4_ship(sp->ship, &data.ship);
        sp->current_station = data.last_station;
        sp->ship->pos = data.last_pos;
        sp->ship->angle = data.last_angle;
        /* Read manifest tail. Bootstrap was called by migrate_v4_ship. */
        uint16_t manifest_count = 0;
        if (fread(&manifest_count, sizeof(manifest_count), 1, f) != 1) {
            return false;
        }
        if (manifest_count > 0) {
            if (!manifest_reserve(&sp->ship->manifest, manifest_count)) {
                return false;
            }
            for (uint16_t u = 0; u < manifest_count; u++) {
                cargo_unit_t cu;
                if (fread(&cu, sizeof(cu), 1, f) != 1) {
                    return false;
                }
                sp->ship->manifest.units[u] = cu;
            }
            sp->ship->manifest.count = manifest_count;
            /* Cargo_unit_t byte 7 was _pad in pre-v45 saves and is now
             * `quantity`. Idempotent rewrite: 0 → 1 leaves v45+ saves
             * untouched and migrates the legacy zero to a valid count. */
            manifest_migrate_quantity(&sp->ship->manifest);
        }
        /* PLY6+ last_signed_nonce. PLY5 saves end here; the nonce stays
         * at zero, which lets the first signed action after the migration
         * use any non-zero nonce. */
        sp->last_signed_nonce = 0;
        if (magic == PLAYER_MAGIC || magic == PLAYER_MAGIC_V6) {
            uint64_t nonce = 0;
            if (fread(&nonce, sizeof(nonce), 1, f) != 1) {
                return false;
            }
            sp->last_signed_nonce = nonce;
        }
        /* PLY7 (#479 D): per-manifest-entry cargo receipt chains. We
         * expect exactly manifest_count chains. Each chain is [len:u8]
         * + len × CARGO_RECEIPT_SIZE.
         * v6 saves stop short here — the receipt store stays empty;
         * the next BUY/DELIVER for that cargo will sign a fresh
         * origin-attested receipt (one-time migration cost). */
        if (magic == PLAYER_MAGIC) {
            ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
            if (!rcpts) return false;
            ship_receipts_clear(rcpts);
            if (manifest_count > 0) {
                if (!ship_receipts_reserve(rcpts, manifest_count)) {
                    return false;
                }
                for (uint16_t u = 0; u < manifest_count; u++) {
                    uint8_t link_count = 0;
                    if (fread(&link_count, sizeof(link_count), 1, f) != 1) {
                        return false;
                    }
                    if (link_count > CARGO_RECEIPT_CHAIN_MAX_LEN) {
                        return false; /* corrupt */
                    }
                    cargo_receipt_t links[CARGO_RECEIPT_CHAIN_MAX_LEN];
                    for (uint8_t k = 0; k < link_count; k++) {
                        if (fread(&links[k], sizeof(cargo_receipt_t), 1, f) != 1) {
                            return false;
                        }
                    }
                    if (link_count > 0) {
                        if (!ship_receipts_push_chain(rcpts, links, link_count)) {
                            return false;
                        }
                    } else {
                        if (!ship_receipts_push_empty(rcpts)) {
                            return false;
                        }
                    }
                }
            }
            long decoded_end = ftell(f);
            if (decoded_end < 0 || file_size < 8u ||
                (uint64_t)decoded_end != file_size - 8u) {
                return false;
            }
        }
        manifest_already_loaded = true;
    } else if (magic == PLAYER_MAGIC_V4) {
        /* PLY4 → PLY5: read ship blob; manifest stays empty (was never
         * persisted in PLY4, lived only at runtime). */
        player_save_data_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) return false;
        migrate_v4_ship(sp->ship, &data.ship);
        sp->current_station = data.last_station;
        sp->ship->pos = data.last_pos;
        sp->ship->angle = data.last_angle;
    } else if (magic == PLAYER_MAGIC_V3) {
        /* PLY3 → PLY4: migrate ship_v3_t → ship_t, zero-init hold_ingots. */
        player_save_v3_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) return false;
        migrate_v3_ship(sp->ship, &data.ship);
        sp->current_station = data.last_station;
        sp->ship->pos = data.last_pos;
        sp->ship->angle = data.last_angle;
    } else if (magic == PLAYER_MAGIC_V2 || magic == PLAYER_MAGIC_V1) {
        /* PLY2 or PLY1 — old ship_t with global credits */
        player_save_v2_t data;
        if (fread(&data, sizeof(data), 1, f) != 1) return false;
        is_v1 = (magic == PLAYER_MAGIC_V1);
        migrate_v2_ship(sp->ship, &data.ship);
        migrated_credits = data.ship.credits;
        sp->current_station = data.last_station;
        sp->ship->pos = data.last_pos;
        sp->ship->angle = data.last_angle;
    } else {
        return false;
    }

    if (is_v1) {
        /* v1 → v2: remap unlocked_modules bits across the #280 enum cleanup */
        static const int REMAP[17] = {
            0, 1, 2, 3, 4, -1, 5, 6, 7, 8, 9, -1, 10, -1, -1, 11, 12,
        };
        uint32_t old_mask = sp->ship->unlocked_modules;
        uint32_t new_mask = 0;
        for (int b = 0; b < 17; b++) {
            if (!(old_mask & (1u << b))) continue;
            int new_t = REMAP[b];
            if (new_t >= 0) new_mask |= (1u << (uint32_t)new_t);
        }
        sp->ship->unlocked_modules = new_mask;
    }
    /* Validate hull class */
    if (sp->ship->hull_class < 0 || sp->ship->hull_class >= HULL_CLASS_COUNT)
        sp->ship->hull_class = HULL_CLASS_MINER;
    /* Validate station index */
    if (sp->current_station < 0 || sp->current_station >= MAX_STATIONS ||
        !station_exists(&w->stations[sp->current_station]))
        sp->current_station = 0;
    /* Clamp upgrade levels */
    if (sp->ship->mining_level < 0 || sp->ship->mining_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship->mining_level = 0;
    if (sp->ship->hold_level < 0 || sp->ship->hold_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship->hold_level = 0;
    if (sp->ship->tractor_level < 0 || sp->ship->tractor_level > SHIP_UPGRADE_MAX_LEVEL) sp->ship->tractor_level = 0;
    /* Clamp hull HP */
    float max_hull = ship_max_hull(sp->ship);
    if (!(sp->ship->hull > 0.0f)) sp->ship->hull = max_hull;
    if (sp->ship->hull > max_hull) sp->ship->hull = max_hull;
    /* Clamp cargo (no negative, no NaN, no exceeding capacity) */
    for (int i = 0; i < COMMODITY_COUNT; i++) {
        if (!(sp->ship->cargo[i] >= 0.0f)) sp->ship->cargo[i] = 0.0f;
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
        (void)manifest_migrate_legacy_inventory(&sp->ship->manifest,
                                                sp->ship->cargo,
                                                COMMODITY_COUNT, origin);
    }
    ship_retire_finished_cargo_slots(sp->ship);
    /* Dock the player at their last station for safety */
    sp->docked = true;
    sp->nearby_station = sp->current_station;
    sp->in_dock_range = true;
    sp->dock_berth = -1;
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

static bool ship_asset_owner_matches_player_save(const ship_asset_t *asset,
                                                 const server_player_t *sp) {
    if (!asset || !sp) return false;
    actor_principal_t player = actor_principal_none();
    return actor_principal_from_verified_player(sp, &player) &&
           actor_principal_equal(
               &asset->owner_principal, &player);
}

static bool ship_asset_load_candidate_assignable(
    const ship_asset_t *asset) {
    if (!asset || !asset->active || asset->destroyed) return false;
    return asset->status == SHIP_ASSET_STATUS_STORED;
}

static bool ship_asset_load_bound_candidate_assignable(
    const ship_asset_t *asset,
    int slot) {
    if (!asset || !asset->active || asset->destroyed) return false;
    return asset->status == SHIP_ASSET_STATUS_STORED ||
           (asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
            asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
            asset->operator_slot == slot);
}

static bool ship_asset_is_station_loaner_for_slot(
    const ship_asset_t *asset,
    const world_t *w,
    int slot) {
    actor_principal_t station = actor_principal_none();
    return asset && w &&
           asset->active && !asset->destroyed &&
           actor_principal_from_station(
               w, asset->custody_station, &station) &&
           actor_principal_equal(
               &asset->owner_principal, &station) &&
           asset->loaner &&
           asset->status == SHIP_ASSET_STATUS_ASSIGNED &&
           asset->operator_kind == SHIP_ASSET_OPERATOR_PLAYER &&
           asset->operator_slot == slot;
}

static void ship_asset_release_loaded_provisional(ship_asset_t *asset,
                                                  const world_t *w,
                                                  int slot) {
    if (!ship_asset_is_station_loaner_for_slot(
            asset, w, slot)) {
        return;
    }
    asset->status = SHIP_ASSET_STATUS_STORED;
    asset->operator_kind = SHIP_ASSET_OPERATOR_NONE;
    asset->operator_slot = -1;
}

static void player_bind_loaded_ship_asset(server_player_t *sp, world_t *w, int slot) {
    if (!sp || !w || slot < 0 || slot >= MAX_PLAYERS) return;
    if (sp != &w->players[slot]) return;
    actor_principal_t owner = actor_principal_none();
    bool has_stable_owner =
        actor_principal_from_verified_player(sp, &owner);

    ship_asset_t *asset = NULL;
    ship_asset_t *prior = world_ship_asset_by_id(w, sp->ship_asset_id);
    if (has_stable_owner &&
        ship_asset_load_bound_candidate_assignable(prior, slot) &&
        ship_asset_owner_matches_player_save(prior, sp)) {
        asset = prior;
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        if (asset) break;
        ship_asset_t *candidate = &w->ship_assets[i];
        if (!has_stable_owner ||
            !ship_asset_load_candidate_assignable(candidate)) {
            continue;
        }
        if (!ship_asset_owner_matches_player_save(candidate, sp)) continue;
        asset = candidate;
        break;
    }
    if (!asset && has_stable_owner) {
        asset = world_ship_asset_mint(
            w, sp->ship->hull_class, &owner,
            sp->current_station,
            SHIP_ASSET_PROVENANCE_LEGACY, false, -1);
    }
    if (!asset &&
        ship_asset_is_station_loaner_for_slot(
            prior, w, slot) &&
        prior->custody_station == sp->current_station) {
        asset = prior;
    }
    if (!asset) {
        if (ship_asset_is_station_loaner_for_slot(
                prior, w, slot)) {
            ship_asset_release_loaded_provisional(
                prior, w, slot);
            sp->ship_asset_id = SHIP_ASSET_ID_NONE;
        }
        return;
    }
    if (prior && prior != asset)
        ship_asset_release_loaded_provisional(
            prior, w, slot);
    asset->hull_class = sp->ship->hull_class;
    asset->status = SHIP_ASSET_STATUS_ASSIGNED;
    asset->operator_kind = SHIP_ASSET_OPERATOR_PLAYER;
    asset->operator_slot = (int16_t)slot;
    asset->live_ship_ref = sp->ship_ref;
    asset->ship = sp->ship;
    asset->custody_station = (int16_t)sp->current_station;
    ship_cleanup(&asset->stored_ship);
    memset(&asset->stored_ship, 0, sizeof(asset->stored_ship));
    sp->ship_asset_id = asset->asset_id;
}

static void player_restore_tow_links(server_player_t *sp,
                                     world_t *w,
                                     int slot) {
    if (!sp || !w || slot < 0 || slot >= MAX_PLAYERS) return;
    int16_t saved_fragments[10];
    int saved_fragment_count = ship_towed_fragment_count(sp->ship);
    memcpy(saved_fragments, sp->ship->towed_fragments,
           (size_t)saved_fragment_count * sizeof(saved_fragments[0]));
    int saved_scaffold = sp->ship->towed_scaffold;

    /* World saves decode historical target bindings and player saves decode
     * historical fragment/scaffold arrays. Cargo pods are deliberately
     * excluded: v82 world-side actor principals are their sole durable owner,
     * and reconcile projects them only after an exact verified match. */
    world_tow_links_reconcile(w);
    for (int t = 0; t < saved_fragment_count; t++) {
        int idx = saved_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS || !w->asteroids[idx].active ||
            asteroid_has_tractor(&w->asteroids[idx])) {
            continue;
        }
        (void)world_asteroid_set_player_tractor(w, idx, slot);
    }
    if (saved_scaffold >= 0 && saved_scaffold < MAX_SCAFFOLDS &&
        w->scaffolds[saved_scaffold].active &&
        !scaffold_has_tractor(&w->scaffolds[saved_scaffold])) {
        (void)world_scaffold_set_player_tractor(w, saved_scaffold, slot);
    }

    for (int tow_slot = 0; tow_slot < sp->ship->towed_pod_count; tow_slot++) {
        int i = sp->ship->towed_pods[tow_slot];
        if (i < 0 || i >= MAX_CARGO_PODS || !w->cargo_pods[i].active)
            continue;
        cargo_pod_t *pod = &w->cargo_pods[i];
        float angle = sp->ship->angle + PI_F + 0.18f * (float)(tow_slot - 1);
        float dist = 52.0f + 24.0f * (float)tow_slot;
        pod->pos = v2_add(sp->ship->pos, v2_scale(v2_from_angle(angle), dist));
        pod->vel = sp->ship->vel;
    }
}

static bool player_load_from_open_file(
    server_player_t *sp,
    world_t *w,
    FILE *f,
    uint32_t magic,
    uint64_t file_size,
    int slot) {
    if (!sp || !w || !f) return false;
    ship_t *live_ship = sp->ship;
    entity_ref_t live_ship_ref = sp->ship_ref;
    uint64_t live_last_signed_nonce = sp->last_signed_nonce;
    if (!live_ship) return false;
    ship_t staged_ship = {0};
    server_player_t staged = *sp;
    staged.ship = &staged_ship;
    bool ok = player_load_from_file_decode(
        &staged, w, f, magic, file_size, slot);
    if (!ok) {
        ship_cleanup(staged.ship);
        return false;
    }
    if (staged.last_signed_nonce < live_last_signed_nonce)
        staged.last_signed_nonce = live_last_signed_nonce;
    ship_cleanup(live_ship);
    *live_ship = staged_ship;
    memset(&staged_ship, 0, sizeof(staged_ship));
    *sp = staged;
    sp->ship = live_ship;
    sp->ship_ref = live_ship_ref;
    server_player_clear_transient_input(sp);
    player_bind_loaded_ship_asset(sp, w, slot);
    player_restore_tow_links(sp, w, slot);
    return true;
}

static bool player_load_from_path(
    server_player_t *sp,
    world_t *w,
    const char *path,
    int slot) {
    if (!sp || !w || !path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = false;
    uint32_t magic = 0;
    uint64_t file_size = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end >= 0) file_size = (uint64_t)end;
    }
    if (file_size > 0 &&
        fseek(f, 0, SEEK_SET) == 0 &&
        fread(&magic, sizeof(magic), 1, f) == 1 &&
        fseek(f, 0, SEEK_SET) == 0 &&
        (magic != PLAYER_MAGIC ||
         player_save_verify_crc32_trailer(f, path)) &&
        fseek(f, 0, SEEK_SET) == 0) {
        ok = player_load_from_open_file(
            sp, w, f, magic, file_size, slot);
    }
    if (fclose(f) != 0) ok = false;
    return ok;
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

/* ================================================================== */
/* Authenticated one-time legacy player-save recovery                  */
/* ================================================================== */

#if defined(SIGNAL_SERVER_PERSISTENCE)

enum {
    LEGACY_RECOVERY_PLAYER_SAVE_MAX_BYTES = 8 * 1024 * 1024,
    LEGACY_RECOVERY_PLAYER_MANIFEST_MAX = 4096,
};

static bool legacy_recovery_selected_player_dir(
    const char *generation_root,
    const char *legacy_player_dir,
    char out[PERSISTENCE_GENERATION_PATH_MAX]) {
    if (!generation_root || !generation_root[0] ||
        !legacy_player_dir || !legacy_player_dir[0] || !out) {
        return false;
    }
    persistence_generation_paths_t selected = {0};
    persistence_generation_status_t status =
        persistence_generation_resolve(generation_root, &selected);
    const char *source = status == PERSISTENCE_GENERATION_NONE
        ? legacy_player_dir
        : (status == PERSISTENCE_GENERATION_CURRENT ||
           status == PERSISTENCE_GENERATION_PREVIOUS)
            ? selected.player_dir : NULL;
    if (!source) return false;
    int n = snprintf(out, PERSISTENCE_GENERATION_PATH_MAX,
                     "%s", source);
    return n > 0 && n < PERSISTENCE_GENERATION_PATH_MAX;
}

static bool legacy_recovery_paths(
    const char *player_dir,
    const uint8_t token[8],
    const uint8_t pubkey[32],
    char source[PERSISTENCE_GENERATION_PATH_MAX],
    char destination[PERSISTENCE_GENERATION_PATH_MAX]) {
    if (!player_dir || !token || !pubkey || !source || !destination ||
        pubkey_is_zero32(pubkey)) {
        return false;
    }
    char token_hex[17];
    session_token_to_hex(token, token_hex);
    char pubkey_base58[64];
    if (base58_encode(
            pubkey, 32, pubkey_base58,
            sizeof(pubkey_base58)) == 0) {
        return false;
    }
    int source_len = snprintf(
        source, PERSISTENCE_GENERATION_PATH_MAX,
        "%s/" LEGACY_SUBDIR "/player_%s.sav",
        player_dir, token_hex);
    int destination_len = snprintf(
        destination, PERSISTENCE_GENERATION_PATH_MAX,
        "%s/" PUBKEY_SUBDIR "/%s.sav",
        player_dir, pubkey_base58);
    return source_len > 0 &&
           source_len < PERSISTENCE_GENERATION_PATH_MAX &&
           destination_len > 0 &&
           destination_len < PERSISTENCE_GENERATION_PATH_MAX;
}

#ifdef _WIN32
static void legacy_recovery_windows_close_parents(
    HANDLE handles[2]) {
    if (!handles) return;
    for (size_t i = 0; i < 2; i++) {
        if (handles[i] != INVALID_HANDLE_VALUE) {
            CloseHandle(handles[i]);
            handles[i] = INVALID_HANDLE_VALUE;
        }
    }
}

static bool legacy_recovery_windows_lock_namespace(
    const char *player_dir,
    const char *subdir,
    HANDLE handles[2]) {
    if (!player_dir || !subdir || !handles ||
        strchr(subdir, '/') || strchr(subdir, '\\')) {
        return false;
    }
    handles[0] = INVALID_HANDLE_VALUE;
    handles[1] = INVALID_HANDLE_VALUE;
    char namespace_path[PERSISTENCE_GENERATION_PATH_MAX];
    int n = snprintf(
        namespace_path, sizeof(namespace_path),
        "%s/%s", player_dir, subdir);
    if (n <= 0 || (size_t)n >= sizeof(namespace_path))
        return false;
    const char *paths[2] = {player_dir, namespace_path};
    for (size_t i = 0; i < 2; i++) {
        handles[i] = CreateFileA(
            paths[i], FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT,
            NULL);
        BY_HANDLE_FILE_INFORMATION info = {0};
        if (handles[i] == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(handles[i], &info) ||
            (info.dwFileAttributes &
             FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            legacy_recovery_windows_close_parents(handles);
            return false;
        }
    }
    return true;
}

static legacy_recovery_source_status_t
legacy_recovery_windows_node_status(
    const char *player_dir,
    const char *subdir,
    const char *name,
    bool source) {
    if (!name || !name[0] ||
        strchr(name, '/') || strchr(name, '\\')) {
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
    HANDLE parents[2];
    if (!legacy_recovery_windows_lock_namespace(
            player_dir, subdir, parents)) {
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
    char path[PERSISTENCE_GENERATION_PATH_MAX];
    int path_len = snprintf(
        path, sizeof(path), "%s/%s/%s",
        player_dir, subdir, name);
    if (path_len <= 0 ||
        (size_t)path_len >= sizeof(path)) {
        legacy_recovery_windows_close_parents(parents);
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        legacy_recovery_windows_close_parents(parents);
        return error == ERROR_FILE_NOT_FOUND ||
               error == ERROR_PATH_NOT_FOUND
            ? LEGACY_RECOVERY_SOURCE_NO_MATCH
            : LEGACY_RECOVERY_SOURCE_INVALID;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        legacy_recovery_windows_close_parents(parents);
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
    if (!source) {
        legacy_recovery_windows_close_parents(parents);
        return LEGACY_RECOVERY_SOURCE_DESTINATION_CONFLICT;
    }
    HANDLE handle = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        legacy_recovery_windows_close_parents(parents);
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
    BY_HANDLE_FILE_INFORMATION info = {0};
    bool ok = GetFileInformationByHandle(handle, &info) != 0 &&
              (info.dwFileAttributes &
               FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
              info.nNumberOfLinks == 1;
    uint64_t size =
        ((uint64_t)info.nFileSizeHigh << 32) |
        (uint64_t)info.nFileSizeLow;
    CloseHandle(handle);
    legacy_recovery_windows_close_parents(parents);
    return ok && size >= 12u &&
           size <= LEGACY_RECOVERY_PLAYER_SAVE_MAX_BYTES
        ? LEGACY_RECOVERY_SOURCE_CANDIDATE
        : LEGACY_RECOVERY_SOURCE_INVALID;
}
#else
/*
 * Treat the configured/selected player directory as the trusted anchor, then
 * walk the security-sensitive namespace component through directory
 * descriptors. O_NOFOLLOW on only the final save is insufficient because a
 * replaced legacy/ or pubkey/ directory could redirect the canonical name.
 */
static int legacy_recovery_posix_namespace_fd(
    const char *player_dir, const char *subdir) {
    if (!player_dir || !player_dir[0] ||
        !subdir || !subdir[0] || strchr(subdir, '/')) {
        return -1;
    }
#ifndef O_NOFOLLOW
    return -1;
#else
    int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    directory_flags |= O_CLOEXEC;
#endif
    int player_fd = open(player_dir, directory_flags);
    if (player_fd < 0) return -1;
    int namespace_fd = openat(
        player_fd, subdir, directory_flags);
    close(player_fd);
    return namespace_fd;
#endif
}

/*
 * Return 1 with a no-follow stat, 0 for an absent final component, and -1
 * for an invalid/error path. Missing parents are invalid because the
 * canonical player namespace itself must already exist.
 */
static int legacy_recovery_posix_secure_lstat(
    const char *player_dir,
    const char *subdir,
    const char *leaf,
    struct stat *st) {
    if (!leaf || !leaf[0] || strchr(leaf, '/') || !st)
        return -1;
    int parent_fd = legacy_recovery_posix_namespace_fd(
        player_dir, subdir);
    if (parent_fd < 0) return -1;
    int result = fstatat(
        parent_fd, leaf, st, AT_SYMLINK_NOFOLLOW);
    int saved_errno = errno;
    close(parent_fd);
    if (result == 0) return 1;
    return saved_errno == ENOENT ? 0 : -1;
}

static int legacy_recovery_posix_secure_open_read(
    const char *player_dir,
    const char *subdir,
    const char *leaf) {
    if (!leaf || !leaf[0] || strchr(leaf, '/')) return -1;
    int parent_fd = legacy_recovery_posix_namespace_fd(
        player_dir, subdir);
    if (parent_fd < 0) return -1;
    int flags = O_RDONLY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = openat(parent_fd, leaf, flags);
    close(parent_fd);
    return fd;
}

static legacy_recovery_source_status_t
legacy_recovery_posix_node_status(
    const char *player_dir,
    const char *subdir,
    const char *name,
    bool source) {
    struct stat st;
    int found = legacy_recovery_posix_secure_lstat(
        player_dir, subdir, name, &st);
    if (found <= 0)
        return found == 0
            ? LEGACY_RECOVERY_SOURCE_NO_MATCH
            : LEGACY_RECOVERY_SOURCE_INVALID;
    bool canonical_file =
        S_ISREG(st.st_mode) && st.st_nlink == 1;
    if (!source) {
        return canonical_file
            ? LEGACY_RECOVERY_SOURCE_DESTINATION_CONFLICT
            : LEGACY_RECOVERY_SOURCE_INVALID;
    }
    return canonical_file &&
           st.st_size >= 12 &&
           (uint64_t)st.st_size <=
               LEGACY_RECOVERY_PLAYER_SAVE_MAX_BYTES
        ? LEGACY_RECOVERY_SOURCE_CANDIDATE
        : LEGACY_RECOVERY_SOURCE_INVALID;
}
#endif

legacy_recovery_source_status_t legacy_recovery_source_probe(
    const char *generation_root,
    const char *legacy_player_dir,
    const uint8_t session_token[8],
    const uint8_t pubkey[32]) {
    if (!session_token || !pubkey)
        return LEGACY_RECOVERY_SOURCE_INVALID;
    char player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char source[PERSISTENCE_GENERATION_PATH_MAX];
    char destination[PERSISTENCE_GENERATION_PATH_MAX];
    if (!legacy_recovery_selected_player_dir(
            generation_root, legacy_player_dir, player_dir) ||
        !legacy_recovery_paths(
            player_dir, session_token, pubkey,
            source, destination)) {
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
#ifdef _WIN32
    const char *source_name = strrchr(source, '/');
    const char *destination_name = strrchr(destination, '/');
    if (!source_name || !destination_name)
        return LEGACY_RECOVERY_SOURCE_INVALID;
    source_name++;
    destination_name++;
    legacy_recovery_source_status_t source_status =
        legacy_recovery_windows_node_status(
            player_dir, LEGACY_SUBDIR, source_name, true);
    if (source_status != LEGACY_RECOVERY_SOURCE_CANDIDATE)
        return source_status;
    legacy_recovery_source_status_t destination_status =
        legacy_recovery_windows_node_status(
            player_dir, PUBKEY_SUBDIR,
            destination_name, false);
    return destination_status == LEGACY_RECOVERY_SOURCE_NO_MATCH
        ? LEGACY_RECOVERY_SOURCE_CANDIDATE
        : destination_status;
#else
    const char *source_name = strrchr(source, '/');
    const char *destination_name = strrchr(destination, '/');
    if (!source_name || !destination_name)
        return LEGACY_RECOVERY_SOURCE_INVALID;
    source_name++;
    destination_name++;
    legacy_recovery_source_status_t source_status =
        legacy_recovery_posix_node_status(
            player_dir, LEGACY_SUBDIR, source_name, true);
    if (source_status != LEGACY_RECOVERY_SOURCE_CANDIDATE)
        return source_status;
    legacy_recovery_source_status_t destination_status =
        legacy_recovery_posix_node_status(
            player_dir, PUBKEY_SUBDIR,
            destination_name, false);
    return destination_status == LEGACY_RECOVERY_SOURCE_NO_MATCH
        ? LEGACY_RECOVERY_SOURCE_CANDIDATE
        : destination_status;
#endif
}

legacy_recovery_source_status_t legacy_recovery_destination_probe(
    const char *generation_root,
    const char *legacy_player_dir,
    const uint8_t pubkey[32]) {
    if (!pubkey || pubkey_is_zero32(pubkey))
        return LEGACY_RECOVERY_SOURCE_INVALID;
    char player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    if (!legacy_recovery_selected_player_dir(
            generation_root, legacy_player_dir, player_dir)) {
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
    char destination_name[72];
    char encoded[64];
    size_t encoded_len =
        base58_encode(pubkey, 32, encoded, sizeof(encoded));
    int name_len = encoded_len > 0
        ? snprintf(destination_name, sizeof(destination_name),
                   "%s.sav", encoded)
        : -1;
    if (name_len <= 0 ||
        (size_t)name_len >= sizeof(destination_name)) {
        return LEGACY_RECOVERY_SOURCE_INVALID;
    }
#ifdef _WIN32
    return legacy_recovery_windows_node_status(
        player_dir, PUBKEY_SUBDIR, destination_name, false);
#else
    return legacy_recovery_posix_node_status(
        player_dir, PUBKEY_SUBDIR, destination_name, false);
#endif
}

static bool legacy_recovery_read_source_snapshot_posix(
    const char *player_dir,
    const char *path,
    uint8_t **bytes_out,
    size_t *size_out) {
#ifdef _WIN32
    (void)player_dir;
    (void)path;
    (void)bytes_out;
    (void)size_out;
    return false;
#else
    if (!path || !bytes_out || !size_out) return false;
    *bytes_out = NULL;
    *size_out = 0;
    const char *source_name = strrchr(path, '/');
    if (!source_name) return false;
    source_name++;
    int fd = legacy_recovery_posix_secure_open_read(
        player_dir, LEGACY_SUBDIR, source_name);
    if (fd < 0) return false;
    struct stat before;
    bool ok = fstat(fd, &before) == 0 &&
              S_ISREG(before.st_mode) &&
              before.st_nlink == 1 &&
              before.st_size >= 12 &&
              (uint64_t)before.st_size <=
                  LEGACY_RECOVERY_PLAYER_SAVE_MAX_BYTES;
    size_t size = ok ? (size_t)before.st_size : 0;
    uint8_t *bytes = ok ? malloc(size) : NULL;
    if (ok && !bytes) ok = false;
    size_t offset = 0;
    while (ok && offset < size) {
        ssize_t count = read(fd, bytes + offset, size - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            break;
        }
        offset += (size_t)count;
    }
    struct stat after;
    if (ok) {
        ok = fstat(fd, &after) == 0 &&
             S_ISREG(after.st_mode) &&
             after.st_nlink == 1 &&
             before.st_dev == after.st_dev &&
             before.st_ino == after.st_ino &&
             before.st_size == after.st_size;
    }
    if (close(fd) != 0) ok = false;
    if (!ok) {
        free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *size_out = size;
    return true;
#endif
}

#ifdef _WIN32
static bool legacy_recovery_read_source_snapshot_windows(
    const char *player_dir,
    const char *path,
    uint8_t **bytes_out,
    size_t *size_out) {
    if (!player_dir || !path || !bytes_out || !size_out)
        return false;
    *bytes_out = NULL;
    *size_out = 0;
    HANDLE parents[2];
    if (!legacy_recovery_windows_lock_namespace(
            player_dir, LEGACY_SUBDIR, parents)) {
        return false;
    }
    HANDLE handle = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        legacy_recovery_windows_close_parents(parents);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION before = {0};
    bool ok = GetFileInformationByHandle(handle, &before) != 0 &&
              (before.dwFileAttributes &
               FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
              (before.dwFileAttributes &
               FILE_ATTRIBUTE_DIRECTORY) == 0 &&
              before.nNumberOfLinks == 1;
    uint64_t size64 =
        ((uint64_t)before.nFileSizeHigh << 32) |
        (uint64_t)before.nFileSizeLow;
    if (!ok || size64 < 12u ||
        size64 > LEGACY_RECOVERY_PLAYER_SAVE_MAX_BYTES ||
        size64 > SIZE_MAX) {
        CloseHandle(handle);
        legacy_recovery_windows_close_parents(parents);
        return false;
    }
    size_t size = (size_t)size64;
    uint8_t *bytes = malloc(size);
    if (!bytes) {
        CloseHandle(handle);
        legacy_recovery_windows_close_parents(parents);
        return false;
    }
    size_t offset = 0;
    while (ok && offset < size) {
        DWORD chunk = (DWORD)(
            size - offset > 0x7fffffffu
                ? 0x7fffffffu : size - offset);
        DWORD count = 0;
        if (!ReadFile(handle, bytes + offset, chunk, &count, NULL) ||
            count == 0) {
            ok = false;
            break;
        }
        offset += count;
    }
    BY_HANDLE_FILE_INFORMATION after = {0};
    if (ok) {
        ok = GetFileInformationByHandle(handle, &after) != 0 &&
             after.nNumberOfLinks == 1 &&
             before.dwVolumeSerialNumber ==
                 after.dwVolumeSerialNumber &&
             before.nFileIndexHigh == after.nFileIndexHigh &&
             before.nFileIndexLow == after.nFileIndexLow &&
             before.nFileSizeHigh == after.nFileSizeHigh &&
             before.nFileSizeLow == after.nFileSizeLow;
    }
    if (!CloseHandle(handle)) ok = false;
    legacy_recovery_windows_close_parents(parents);
    if (!ok) {
        free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *size_out = size;
    return true;
}
#endif

static bool legacy_recovery_save_snapshot_valid(
    const uint8_t *bytes, size_t size, uint32_t *magic_out) {
    if (magic_out) *magic_out = 0;
    if (!bytes || size < sizeof(uint32_t) ||
        size > LEGACY_RECOVERY_PLAYER_SAVE_MAX_BYTES) {
        return false;
    }
    uint32_t magic = 0;
    memcpy(&magic, bytes, sizeof(magic));
    if (magic != PLAYER_MAGIC &&
        magic != PLAYER_MAGIC_V6 &&
        magic != PLAYER_MAGIC_V5 &&
        magic != PLAYER_MAGIC_V4) {
        return false;
    }
    size_t payload_end = size;
    if (magic == PLAYER_MAGIC) {
        if (size < 8u) return false;
        uint32_t crc_magic = 0;
        uint32_t stored_crc = 0;
        memcpy(&crc_magic, bytes + size - 8u, sizeof(crc_magic));
        memcpy(&stored_crc, bytes + size - 4u, sizeof(stored_crc));
        if (crc_magic != 0x43524332u ||
            persistence_crc32_update(0, bytes, size - 8u) != stored_crc) {
            return false;
        }
        payload_end -= 8u;
    }
    if (payload_end < sizeof(player_save_data_t)) return false;

    player_save_data_t fixed;
    memcpy(&fixed, bytes, sizeof(fixed));
    const ship_v4_t *ship = &fixed.ship;
    if (!isfinite(ship->pos.x) || !isfinite(ship->pos.y) ||
        !isfinite(ship->vel.x) || !isfinite(ship->vel.y) ||
        !isfinite(ship->angle) || !isfinite(ship->hull) ||
        !isfinite(fixed.last_pos.x) ||
        !isfinite(fixed.last_pos.y) ||
        !isfinite(fixed.last_angle) ||
        fixed.last_station < 0 ||
        fixed.last_station >= MAX_STATIONS ||
        ship->hull_class < 0 ||
        ship->hull_class >= HULL_CLASS_COUNT ||
        ship->mining_level < 0 ||
        ship->mining_level > SHIP_UPGRADE_MAX_LEVEL ||
        ship->hold_level < 0 ||
        ship->hold_level > SHIP_UPGRADE_MAX_LEVEL ||
        ship->tractor_level < 0 ||
        ship->tractor_level > SHIP_UPGRADE_MAX_LEVEL ||
        ship->towed_count > 10 ||
        ship->hold_ingots_count < 0 ||
        ship->hold_ingots_count >
            LEGACY_SHIP_HOLD_INGOTS_MAX) {
        return false;
    }
    for (int commodity = 0;
         commodity < SAVE_V82_COMMODITY_COUNT; commodity++) {
        if (!isfinite(ship->cargo[commodity]) ||
            ship->cargo[commodity] < 0.0f ||
            ship->cargo[commodity] >
                (float)LEGACY_RECOVERY_PLAYER_MANIFEST_MAX) {
            return false;
        }
    }
    for (int i = 0; i < ship->hold_ingots_count; i++) {
        const legacy_named_ingot_t *ingot =
            &ship->hold_ingots[i];
        if (pubkey_is_zero32(ingot->pubkey)) continue;
        if (ingot->prefix_class >= INGOT_PREFIX_COUNT ||
            ingot->metal >= COMMODITY_COUNT ||
            ingot->origin_station >= MAX_STATIONS) {
            return false;
        }
    }

    /*
     * PLY4 predates the variable manifest and has no checksum. Its exact
     * fixed layout plus the source SHA-256 binding is the complete accepted
     * envelope. PLY1-PLY3 remain ineligible because their raw ABI/global
     * credit layouts do not provide a bounded ownership-safe migration.
     */
    if (magic == PLAYER_MAGIC_V4) {
        if (payload_end != sizeof(fixed)) return false;
        if (magic_out) *magic_out = magic;
        return true;
    }
    if (payload_end <
        sizeof(fixed) + sizeof(uint16_t)) {
        return false;
    }
    uint16_t manifest_count = 0;
    memcpy(&manifest_count, bytes + sizeof(fixed),
           sizeof(manifest_count));
    if (manifest_count >
        LEGACY_RECOVERY_PLAYER_MANIFEST_MAX) {
        return false;
    }

    size_t offset = sizeof(fixed) + sizeof(manifest_count);
    size_t units_size =
        (size_t)manifest_count * sizeof(cargo_unit_t);
    if (units_size > payload_end ||
        offset > payload_end - units_size) {
        return false;
    }
    for (uint16_t i = 0; i < manifest_count; i++) {
        cargo_unit_t unit;
        memcpy(&unit, bytes + offset +
                      (size_t)i * sizeof(unit), sizeof(unit));
        if (unit.kind >= CARGO_KIND_COUNT ||
            unit.commodity >= COMMODITY_COUNT ||
            unit.grade >= MINING_GRADE_COUNT ||
            unit.prefix_class >= INGOT_PREFIX_COUNT ||
            unit.recipe_id >= RECIPE_COUNT ||
            unit.origin_station >= MAX_STATIONS ||
            (magic == PLAYER_MAGIC &&
             unit.quantity == 0) ||
            pubkey_is_zero32(unit.pub)) {
            return false;
        }
    }
    offset += units_size;
    if (magic == PLAYER_MAGIC_V5) {
        if (offset != payload_end) return false;
        if (magic_out) *magic_out = magic;
        return true;
    }
    if (sizeof(uint64_t) > payload_end - offset) return false;
    offset += sizeof(uint64_t);
    if (magic == PLAYER_MAGIC_V6) {
        if (offset != payload_end) return false;
        if (magic_out) *magic_out = magic;
        return true;
    }
    for (uint16_t i = 0; i < manifest_count; i++) {
        if (offset >= payload_end) return false;
        uint8_t chain_len = bytes[offset++];
        if (chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN)
            return false;
        size_t chain_size =
            (size_t)chain_len * sizeof(cargo_receipt_t);
        if (chain_size > payload_end ||
            offset > payload_end - chain_size) {
            return false;
        }
        offset += chain_size;
    }
    if (offset != payload_end) return false;
    if (magic_out) *magic_out = magic;
    return true;
}

static bool legacy_recovery_load_snapshot(
    server_player_t *player,
    world_t *world,
    int player_idx,
    const uint8_t *bytes,
    size_t size) {
    uint32_t magic = 0;
    if (!player || !world || !bytes ||
        !legacy_recovery_save_snapshot_valid(
            bytes, size, &magic)) {
        return false;
    }
    FILE *snapshot = tmpfile();
    if (!snapshot) return false;
    bool ok = fwrite(bytes, 1, size, snapshot) == size &&
              fflush(snapshot) == 0 &&
              fseek(snapshot, 0, SEEK_SET) == 0 &&
              player_load_from_open_file(
                  player, world, snapshot, magic,
                  (uint64_t)size, player_idx);
    if (fclose(snapshot) != 0) ok = false;
    return ok;
}

static bool legacy_recovery_read_source_snapshot(
    const char *player_dir,
    const char *path,
    uint8_t **bytes_out,
    size_t *size_out) {
#ifdef _WIN32
    return legacy_recovery_read_source_snapshot_windows(
        player_dir, path, bytes_out, size_out);
#else
    return legacy_recovery_read_source_snapshot_posix(
        player_dir, path, bytes_out, size_out);
#endif
}

static void legacy_recovery_candidate_dispose(world_t *candidate) {
    if (!candidate) return;
    world_cleanup(candidate);
    free(candidate);
}

static void legacy_recovery_adopt_candidate(
    world_t *destination, world_t *candidate) {
    world_cleanup(destination);
    *destination = *candidate;
    memset(candidate, 0, sizeof(*candidate));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        destination->players[i].connection =
            &destination->connections[i];
        destination->players[i].replication =
            &destination->replications[i];
    }
    world_rebind_ship_controllers(destination);
}

/*
 * v81 quarantine rows intentionally omit bearer/session material. There is
 * therefore no sound way to decide which claimant owns an unresolved legacy
 * contract, delivery, build, scaffold, asset, or transient ownership row.
 * Refuse partial promotion instead of rebinding by recycled player slot.
 */
static bool legacy_recovery_ownership_fully_attributable(
    const world_t *world) {
    return world &&
           ownership_quarantine_validate(
               &world->ownership_quarantine) &&
           world->ownership_quarantine.count == 0;
}

legacy_recovery_result_status_t legacy_recovery_execute(
    world_t *world,
    int player_idx,
    uint64_t confirmed_nonce,
    const char *generation_root,
    const char *legacy_player_dir,
    const bool save_player_slot[MAX_PLAYERS],
    persistence_generation_fault_t fault,
    persistence_generation_paths_t *published) {
    if (published) memset(published, 0, sizeof(*published));
    persistence_generation_paths_t published_scratch = {0};
    persistence_generation_paths_t *commit_published =
        published ? published : &published_scratch;
    if (!world || player_idx < 0 || player_idx >= MAX_PLAYERS ||
        confirmed_nonce == 0 || !generation_root ||
        !legacy_player_dir || !save_player_slot ||
        !save_player_slot[player_idx]) {
        return LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE;
    }
    const server_player_t *live = &world->players[player_idx];
    if (!server_player_can_use_pubkey_persistence(live) ||
        confirmed_nonce <= live->last_signed_nonce) {
        return LEGACY_RECOVERY_RESULT_REPLAY;
    }
    if (!legacy_recovery_ownership_fully_attributable(world))
        return LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE;
    uint8_t token[8];
    uint8_t pubkey[32];
    memcpy(token, live->session_token, sizeof(token));
    memcpy(pubkey, live->pubkey, sizeof(pubkey));

    legacy_recovery_source_status_t probe =
        legacy_recovery_source_probe(
            generation_root, legacy_player_dir, token, pubkey);
    if (probe == LEGACY_RECOVERY_SOURCE_NO_MATCH)
        return LEGACY_RECOVERY_RESULT_NO_MATCH;
    if (probe == LEGACY_RECOVERY_SOURCE_DESTINATION_CONFLICT)
        return LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT;
    if (probe != LEGACY_RECOVERY_SOURCE_CANDIDATE)
        return LEGACY_RECOVERY_RESULT_INVALID_SOURCE;

    char player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char source[PERSISTENCE_GENERATION_PATH_MAX];
    char destination[PERSISTENCE_GENERATION_PATH_MAX];
    if (!legacy_recovery_selected_player_dir(
            generation_root, legacy_player_dir, player_dir) ||
        !legacy_recovery_paths(
            player_dir, token, pubkey, source, destination)) {
        return LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE;
    }
    (void)destination;

    uint8_t *source_bytes = NULL;
    size_t source_size = 0;
    if (!legacy_recovery_read_source_snapshot(
            player_dir, source,
            &source_bytes, &source_size)) {
        return LEGACY_RECOVERY_RESULT_INVALID_SOURCE;
    }
    uint8_t source_sha256[32];
    sha256_bytes(source_bytes, source_size, source_sha256);

    world_t *candidate = world_load_candidate_create(world);
    if (!candidate) {
        free(source_bytes);
        return LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE;
    }
    server_player_t *staged = &candidate->players[player_idx];
    staged->last_signed_nonce = confirmed_nonce;
    bool activated =
        staged->ship != NULL ||
        (world_player_ship_slot_activate(candidate, player_idx) &&
         world_character_bind_player(candidate, player_idx));
    bool loaded = activated &&
        legacy_recovery_load_snapshot(
            staged, candidate, player_idx, source_bytes, source_size);
    free(source_bytes);
    if (!loaded ||
        memcmp(staged->session_token, token, sizeof(token)) != 0 ||
        memcmp(staged->pubkey, pubkey, sizeof(pubkey)) != 0 ||
        staged->last_signed_nonce < confirmed_nonce ||
        !server_finalize_pubkey_identity(candidate, player_idx)) {
        legacy_recovery_candidate_dispose(candidate);
        return loaded
            ? LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE
            : LEGACY_RECOVERY_RESULT_INVALID_SOURCE;
    }
    player_bind_loaded_ship_asset(
        staged, candidate, player_idx);
    if (staged->ship_asset_id == SHIP_ASSET_ID_NONE ||
        !world_ship_asset_sync_from_player(candidate, staged)) {
        legacy_recovery_candidate_dispose(candidate);
        return loaded
            ? LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE
            : LEGACY_RECOVERY_RESULT_INVALID_SOURCE;
    }
    staged->legacy_recovery_save_pending = false;
    rebuild_signal_chain(candidate);
    spatial_grid_build(candidate);

    persistence_recovery_commit_result_t commit_result =
        persistence_generation_commit_recovery(
            generation_root, legacy_player_dir, candidate,
            save_player_slot, player_idx, token, pubkey,
            (uint64_t)source_size, source_sha256,
            fault, commit_published);
    if (commit_result != PERSISTENCE_RECOVERY_COMMIT_OK) {
        legacy_recovery_candidate_dispose(candidate);
        if (commit_result == PERSISTENCE_RECOVERY_COMMIT_NO_SOURCE)
            return LEGACY_RECOVERY_RESULT_NO_MATCH;
        if (commit_result ==
            PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT) {
            return LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT;
        }
        if (commit_result ==
            PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED) {
            return LEGACY_RECOVERY_RESULT_INVALID_SOURCE;
        }
        return LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE;
    }

    legacy_recovery_adopt_candidate(world, candidate);
    free(candidate);
    return LEGACY_RECOVERY_RESULT_SUCCESS;
}

#endif /* SIGNAL_SERVER_PERSISTENCE */
