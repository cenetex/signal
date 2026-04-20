#ifndef SHARED_MANIFEST_H
#define SHARED_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "mining.h"

typedef enum {
    CARGO_KIND_INGOT   = 0,
    CARGO_KIND_FRAME   = 1,
    CARGO_KIND_LASER   = 2,
    CARGO_KIND_TRACTOR = 3,
    CARGO_KIND_COUNT
} cargo_kind_t;

typedef struct {
    uint8_t  kind;              /* cargo_kind_t */
    uint8_t  commodity;         /* commodity_t */
    uint8_t  grade;             /* mining_grade_t */
    uint8_t  _pad;              /* reserved, zero */
    uint16_t recipe_id;         /* recipe_id_t */
    uint16_t _pad2;             /* reserved, zero */
    uint8_t  pub[32];           /* content hash */
    uint8_t  parent_merkle[32]; /* sorted-input merkle root */
} cargo_unit_t;

typedef struct {
    uint16_t count;
    uint16_t cap;
    cargo_unit_t *units;
} manifest_t;

typedef enum {
    RECIPE_SMELT = 0,
    RECIPE_FRAME_BASIC,
    RECIPE_LASER_BASIC,
    RECIPE_TRACTOR_COIL,
    RECIPE_LEGACY_MIGRATE,
    RECIPE_COUNT
} recipe_id_t;

typedef struct {
    recipe_id_t   id;
    const char   *name;
    cargo_kind_t  output_kind;
    commodity_t   output_commodity; /* COMMODITY_COUNT = caller supplies */
    uint8_t       input_count;
    commodity_t   input_commodities[2];
} recipe_def_t;

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

bool hash_merkle_root(const uint8_t pubs[][32], size_t count, uint8_t out_root[32]);
bool hash_ingot(commodity_t commodity, mining_grade_t grade,
                const uint8_t fragment_pub[32], uint16_t output_index,
                cargo_unit_t *out_unit);
bool hash_product(recipe_id_t recipe_id, const cargo_unit_t *inputs,
                  size_t input_count, uint16_t output_index,
                  cargo_unit_t *out_unit);

#endif /* SHARED_MANIFEST_H */
