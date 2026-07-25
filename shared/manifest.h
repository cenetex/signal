#ifndef SHARED_MANIFEST_H
#define SHARED_MANIFEST_H

#include <stddef.h>

#include "types.h"
#include "cargo_receipt.h"  /* ship_receipts_t — accessor return type */

_Static_assert(sizeof(cargo_unit_t) == 80, "cargo_unit_t must stay 80 bytes");
_Static_assert(offsetof(cargo_unit_t, mined_block) == 8,
               "cargo_unit_t mined_block offset changed");
_Static_assert(offsetof(cargo_unit_t, pub) == 16, "cargo_unit_t pub offset changed");
_Static_assert(offsetof(cargo_unit_t, parent_merkle) == 48,
               "cargo_unit_t parent_merkle offset changed");

#define CARGO_UNIT_WIRE_SIZE 80u

/* Canonical field-packed cargo identity used by manifest replication and
 * handoff snapshots. Keep persistence free to version independently; this
 * encoding is explicitly little-endian and never relies on struct padding. */
void cargo_unit_wire_pack(const cargo_unit_t *unit,
                          uint8_t out[CARGO_UNIT_WIRE_SIZE]);
void cargo_unit_wire_unpack(const uint8_t in[CARGO_UNIT_WIRE_SIZE],
                            cargo_unit_t *out);

const char *cargo_kind_name(cargo_kind_t kind);
const recipe_def_t *recipe_get(recipe_id_t id);

bool manifest_init(manifest_t *manifest, uint16_t cap);
void manifest_free(manifest_t *manifest);
void manifest_clear(manifest_t *manifest);
bool manifest_reserve(manifest_t *manifest, uint16_t cap);
bool manifest_clone(manifest_t *dst, const manifest_t *src);
bool manifest_push(manifest_t *manifest, const cargo_unit_t *unit);
bool manifest_remove(manifest_t *manifest, uint16_t index, cargo_unit_t *out_unit);
int manifest_find(const manifest_t *manifest, const uint8_t pub[32]);

bool cargo_store_bootstrap(cargo_store_t *store, uint16_t default_cap);
void cargo_store_cleanup(cargo_store_t *store);
bool cargo_store_clone(cargo_store_t *dst, const cargo_store_t *src);
ship_receipts_t *cargo_store_receipts(cargo_store_t *store);
const ship_receipts_t *cargo_store_receipts_const(const cargo_store_t *store);
bool cargo_store_push_with_chain(cargo_store_t *store,
                                 const cargo_unit_t *unit,
                                 const cargo_receipt_chain_t *chain);
bool cargo_store_remove_with_chain(cargo_store_t *store, uint16_t index,
                                   cargo_unit_t *out_unit,
                                   cargo_receipt_chain_t *out_chain);
int cargo_store_consume_by_commodity(cargo_store_t *store,
                                     commodity_t commodity, int n);

/* Return the index of the first unit in `manifest` matching the given
 * commodity+grade, or -1 if none. Used by transaction paths to pick a
 * unit to transfer (FIFO — oldest first). O(manifest.count). */
int manifest_find_first_cg(const manifest_t *manifest,
                           commodity_t commodity,
                           mining_grade_t grade);

/* Count units in `manifest` matching the given commodity. Used by the
 * station-summary builder + sanity tests. O(manifest.count). */
int manifest_count_by_commodity(const manifest_t *manifest,
                                commodity_t commodity);
int manifest_count_by_commodity_grade(const manifest_t *manifest,
                                      commodity_t commodity,
                                      mining_grade_t grade);

/* Cargo-pod content queries shared by simulation, protocol summaries, HUD,
 * and rendering.  `cargo_pod_has_exact_manifest` deliberately rejects
 * shipment-bound pods: their credit-cargo envelope is not interchangeable
 * material even when every visible unit has the same commodity. */
bool cargo_pod_has_exact_manifest(const cargo_pod_t *pod,
                                  commodity_t commodity);
mining_grade_t cargo_pod_manifest_best_grade(const cargo_pod_t *pod);
mining_grade_t cargo_pod_display_grade(const cargo_pod_t *pod);

/* Coarse physical-content grammar shared by world rendering and tests.
 * The carrier shell is deliberately not part of this enum: shell and
 * payload are separate identities, and every non-gas cargo pod renders the
 * same structural holder around one of these payload treatments. */
typedef enum {
    CARGO_POD_CONTENT_EMPTY = 0,
    CARGO_POD_CONTENT_INGOT,
    CARGO_POD_CONTENT_STRUT,
    CARGO_POD_CONTENT_ACTIVE,
    CARGO_POD_CONTENT_SERVICE,
    CARGO_POD_CONTENT_GAS,
    CARGO_POD_CONTENT_MIXED,
} cargo_pod_content_shape_t;

/* Resolve detailed local manifest rows when available and fall back to the
 * commodity summary for remote pods. Raw-ore compatibility pods use the bulk
 * ingot/block treatment; live fragments remain asteroid_t world objects. */
cargo_pod_content_shape_t cargo_pod_content_shape(const cargo_pod_t *pod);

/* Normal production carriers hold CARGO_POD_UNIT_CAPACITY units. Rich legacy
 * or refinery pods may exceed it; display load clamps at 1 rather than
 * inventing a larger structural size. */
float cargo_pod_load_ratio(const cargo_pod_t *pod);

/* Compute the manifest's grade-weighted display tint. `fill_ratio`
 * controls how far the returned color blends from the neutral holder
 * color toward the average `cargo_unit_t.grade` color, matching the
 * player-ship hull tint rule used by world rendering. Returns false
 * when no manifest grade was available; outputs still receive the
 * neutral color. */
bool manifest_rarity_tint(const manifest_t *manifest, float fill_ratio,
                          float neutral_r, float neutral_g, float neutral_b,
                          float *out_r, float *out_g, float *out_b);

/* Remove up to `n` units of `commodity` from `manifest` (no destination
 * — the units are destroyed). Used by construction delivery, where the
 * cargo unit's identity is consumed into a built module rather than
 * transferred to another holder. Walks backwards so removing doesn't
 * disturb earlier indices. Returns the number actually removed. */
int manifest_consume_by_commodity(manifest_t *manifest,
                                  commodity_t commodity, int n);

/* Ship-side finished-good helpers. Finished goods live in the manifest;
 * finished ship.cargo[c] slots are retired compatibility storage. */
int ship_finished_count(const ship_t *ship, commodity_t c);
void ship_finished_sync(ship_t *ship, commodity_t c);
int ship_finished_drain(ship_t *ship, commodity_t c, int n);

void ship_cleanup(ship_t *ship);
bool ship_manifest_bootstrap(ship_t *ship);
bool ship_copy(ship_t *dst, const ship_t *src);

/* Layer D of #479 — typed accessors for the parallel receipt store
 * stashed in ship_t.receipts_opaque. The ship_receipts_t type itself
 * lives in shared/cargo_receipt.h; these accessors return pointers
 * the caller must NOT free (lifetime tracks the ship). Returns NULL
 * if ship_manifest_bootstrap hasn't been called yet. */
ship_receipts_t *ship_get_receipts(ship_t *ship);
const ship_receipts_t *ship_get_receipts_const(const ship_t *ship);
bool ship_manifest_push_with_chain(ship_t *ship, const cargo_unit_t *unit,
                                   const cargo_receipt_chain_t *chain);
bool ship_manifest_remove_with_chain(ship_t *ship, uint16_t index,
                                     cargo_unit_t *out_unit,
                                     cargo_receipt_chain_t *out_chain);
int ship_manifest_consume_by_commodity(ship_t *ship, commodity_t c, int n);
void station_cleanup(station_t *station);
bool station_manifest_bootstrap(station_t *station);
bool station_copy(station_t *dst, const station_t *src);
ship_receipts_t *station_get_receipts(station_t *station);
const ship_receipts_t *station_get_receipts_const(const station_t *station);
bool station_manifest_push_with_chain(station_t *station,
                                      const cargo_unit_t *unit,
                                      const cargo_receipt_chain_t *chain);
bool station_manifest_remove_with_chain(station_t *station, uint16_t index,
                                        cargo_unit_t *out_unit,
                                        cargo_receipt_chain_t *out_chain);
int station_manifest_consume_by_commodity(station_t *station,
                                          commodity_t c, int n);

bool hash_merkle_root(const uint8_t pubs[][32], size_t count, uint8_t out_root[32]);
bool hash_ingot(commodity_t commodity, mining_grade_t grade,
                const uint8_t fragment_pub[32], uint16_t output_index,
                cargo_unit_t *out_unit);
bool hash_product(recipe_id_t recipe_id, const cargo_unit_t *inputs,
                  size_t input_count, uint16_t output_index,
                  cargo_unit_t *out_unit);

/* #339 slice D: synthesize a placeholder cargo_unit_t for a float-held
 * finished good loaded from a pre-manifest save. The unit gets a
 * RECIPE_LEGACY_MIGRATE recipe_id so readers can tell it predates the
 * provenance layer; grade stays MINING_GRADE_COMMON; parent_merkle is
 * all-zero; pub is a deterministic sha256 over an origin salt +
 * commodity + index so identical saves reload to identical pubs.
 *
 * origin[8] should be stable per producer (station index encoded, or
 * ship session token). Returns false on bad args. */
bool hash_legacy_migrate_unit(const uint8_t origin[8], commodity_t commodity,
                              uint16_t output_index, cargo_unit_t *out_unit);

/* Map a finished-good commodity to its cargo_kind. Returns false for
 * raw ore (ore never becomes a cargo_unit) and unknown commodities. */
bool cargo_kind_for_commodity(commodity_t commodity, cargo_kind_t *out_kind);

/* Populate `manifest` with synthesized RECIPE_LEGACY_MIGRATE units for
 * every integer unit of finished-good in `inventory[]`. Raw ore slots
 * are skipped (ore is never a manifest unit). Fractional remainders
 * stay in float. The manifest should already be bootstrapped. */
bool manifest_migrate_legacy_inventory(manifest_t *manifest,
                                       const float *inventory,
                                       size_t inventory_count,
                                       const uint8_t origin[8]);

/* Slice 0 of crate unification: pre-v45 saves wrote zero into the
 * cargo_unit_t._pad byte that's now repurposed as `quantity`. Walk
 * `manifest` and rewrite quantity == 0 → 1 so legacy-loaded units stay
 * individually addressable. Idempotent — safe to call on a manifest
 * that's already been migrated or written by a v45+ producer. */
void manifest_migrate_quantity(manifest_t *manifest);

/* ---------------------------------------------------------------- */
/* Manifest-as-truth helpers (PR: kill the float<->manifest drift)   */
/* ---------------------------------------------------------------- */
/* For finished-good commodities (c >= COMMODITY_RAW_ORE_COUNT) the
 * manifest is the authoritative store. Fractional production lives in a
 * separate sub-unit residue and can never represent a whole cargo unit.
 *
 * Raw-ore commodities (c < COMMODITY_RAW_ORE_COUNT) are NOT covered —
 * raw ore lives only in the float (hopper amounts) and never gains a
 * manifest entry. Calling these on a raw ore commodity returns 0 and
 * does nothing. */

/* Mint `n` whole units of finished `c` into the station. Pushes `n`
 * legacy-migrate manifest entries (origin = 8-byte provenance prefix,
 * may be NULL for a generic "STATION " stamp). Returns the number
 * actually minted (may be < n if manifest cap is hit). */
int station_finished_mint(station_t *st, commodity_t c, int n,
                          const uint8_t origin[8]);

/* Count/sync helpers for callers that need manifest-authoritative reads
 * or that consumed specific manifest entries themselves. */
int station_finished_count(const station_t *st, commodity_t c);
void station_finished_sync(station_t *st, commodity_t c);

/* Drain up to `n` units of finished `c` from the manifest (FIFO). */
int station_finished_drain(station_t *st, commodity_t c, int n);

/* Production accumulator for finished `c`. Adds `amount` (may be
 * fractional) to the residue; for any integer crossings, mints that
 * many manifest units (using origin).
 * Returns the integer count minted this call. */
int station_finished_accumulate(station_t *st, commodity_t c, float amount,
                                const uint8_t origin[8]);

/* Consume `amount` (may be fractional) of finished `c` from the
 * manifest plus residue. For integer crossings, drains manifest units
 * (FIFO). Returns the
 * integer count drained this call. */
int station_finished_consume(station_t *st, commodity_t c, float amount);

#endif /* SHARED_MANIFEST_H */
