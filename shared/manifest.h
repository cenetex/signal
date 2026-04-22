#ifndef SHARED_MANIFEST_H
#define SHARED_MANIFEST_H

#include <stddef.h>

#include "types.h"

_Static_assert(sizeof(cargo_unit_t) == 72, "cargo_unit_t must stay 72 bytes");
_Static_assert(offsetof(cargo_unit_t, pub) == 8, "cargo_unit_t pub offset changed");
_Static_assert(offsetof(cargo_unit_t, parent_merkle) == 40,
               "cargo_unit_t parent_merkle offset changed");

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

void ship_cleanup(ship_t *ship);
bool ship_manifest_bootstrap(ship_t *ship);
bool ship_copy(ship_t *dst, const ship_t *src);
void station_cleanup(station_t *station);
bool station_manifest_bootstrap(station_t *station);
bool station_copy(station_t *dst, const station_t *src);

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

#endif /* SHARED_MANIFEST_H */
