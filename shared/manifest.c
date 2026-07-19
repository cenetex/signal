#include "manifest.h"
#include "cargo_receipt.h"  /* Layer D of #479 — ship receipt store */

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Whole finished goods live only in the manifest. The separate residue is
 * constrained to the fractional interval and can never become a second
 * whole-unit authority. */
static void assert_finished_invariant(const station_t *st, commodity_t c) {
    (void)st; (void)c;
#ifndef NDEBUG
    if ((int)c < (int)COMMODITY_RAW_ORE_COUNT) return;
    if ((int)c >= (int)COMMODITY_COUNT) return;
    assert(st->_finished_residue[c] >= 0.0f);
    assert(st->_finished_residue[c] < 1.0f);
#endif
}

enum {
    HASH_BYTES = 32,
};

static const uint8_t MANIFEST_DOMAIN[8] = {
    'S', 'I', 'G', 'N', 'A', 'L', 'v', '1'
};

static const recipe_def_t RECIPE_TABLE[RECIPE_COUNT] = {
    [RECIPE_SMELT] = {
        .id = RECIPE_SMELT,
        .name = "smelt",
        .output_kind = CARGO_KIND_INGOT,
        .output_commodity = COMMODITY_COUNT,
        .output_count = 1,
        .input_count = 1,
        .input_commodities = { COMMODITY_COUNT, COMMODITY_COUNT },
    },
    [RECIPE_FRAME_BASIC] = {
        .id = RECIPE_FRAME_BASIC,
        .name = "frame/basic",
        .output_kind = CARGO_KIND_FRAME,
        .output_commodity = COMMODITY_FRAME,
        .output_count = CELL_STRUTS_PER_INGOT,
        .input_count = 1,
        .input_commodities = { COMMODITY_FERRITE_INGOT, COMMODITY_COUNT },
    },
    [RECIPE_LASER_BASIC] = {
        .id = RECIPE_LASER_BASIC,
        .name = "laser/basic",
        .output_kind = CARGO_KIND_LASER,
        .output_commodity = COMMODITY_LASER_MODULE,
        .output_count = 1,
        .input_count = 2,
        .input_commodities = { COMMODITY_CRYSTAL_INGOT, COMMODITY_FRAME },
    },
    [RECIPE_TRACTOR_COIL] = {
        .id = RECIPE_TRACTOR_COIL,
        .name = "tractor/coil",
        .output_kind = CARGO_KIND_TRACTOR,
        .output_commodity = COMMODITY_TRACTOR_MODULE,
        .output_count = 1,
        .input_count = 2,
        .input_commodities = { COMMODITY_CUPRITE_INGOT, COMMODITY_FRAME },
    },
    [RECIPE_REPAIR_KIT_FAB] = {
        .id = RECIPE_REPAIR_KIT_FAB,
        .name = "kit/dock-fab",
        .output_kind = CARGO_KIND_REPAIR_KIT,
        .output_commodity = COMMODITY_REPAIR_KIT,
        .output_count = 100,
        /* 1 frame + 1 laser + 1 tractor → 100 kits. The triple input
         * spreads kit demand across all three Helios/Kepler outputs,
         * so building outposts AND repairing both pull on every
         * production line. Output multiplier (×100) lives in the
         * dock production loop, not the recipe — recipes describe
         * unit-level identity, not bulk yield. */
        .input_count = 3,
        .input_commodities = { COMMODITY_FRAME, COMMODITY_LASER_MODULE,
                               COMMODITY_TRACTOR_MODULE },
    },
    [RECIPE_LEGACY_MIGRATE] = {
        .id = RECIPE_LEGACY_MIGRATE,
        .name = "legacy/migrate",
        .output_kind = CARGO_KIND_INGOT,
        .output_commodity = COMMODITY_COUNT,
        .output_count = 1,
        .input_count = 0,
        .input_commodities = { COMMODITY_COUNT, COMMODITY_COUNT },
    },
};

static int compare_pub_32(const void *lhs, const void *rhs) {
    return memcmp(lhs, rhs, HASH_BYTES);
}

static bool commodity_is_ingot(commodity_t commodity) {
    return commodity == COMMODITY_FERRITE_INGOT ||
           commodity == COMMODITY_CUPRITE_INGOT ||
           commodity == COMMODITY_CRYSTAL_INGOT;
}

static bool cargo_kind_matches_commodity(cargo_kind_t kind, commodity_t commodity) {
    switch (kind) {
    case CARGO_KIND_INGOT:
        return commodity_is_ingot(commodity);
    case CARGO_KIND_FRAME:
        return commodity == COMMODITY_FRAME;
    case CARGO_KIND_LASER:
        return commodity == COMMODITY_LASER_MODULE;
    case CARGO_KIND_TRACTOR:
        return commodity == COMMODITY_TRACTOR_MODULE;
    case CARGO_KIND_REPAIR_KIT:
        return commodity == COMMODITY_REPAIR_KIT;
    default:
        return false;
    }
}

static mining_grade_t min_input_grade(const cargo_unit_t *inputs, size_t count) {
    mining_grade_t grade = MINING_GRADE_COMMISSIONED;
    for (size_t i = 0; i < count; i++) {
        mining_grade_t next = (mining_grade_t)inputs[i].grade;
        if (next < grade) grade = next;
    }
    return grade;
}

static bool finished_good_commodity(commodity_t c) {
    return (int)c >= (int)COMMODITY_RAW_ORE_COUNT &&
           (int)c < (int)COMMODITY_COUNT;
}

static bool recipe_inputs_match(const recipe_def_t *recipe,
                                const cargo_unit_t *inputs,
                                size_t input_count) {
    bool matched[RECIPE_INPUT_MAX] = { false };

    if (!recipe || !inputs ||
        input_count != recipe->input_count ||
        input_count > RECIPE_INPUT_MAX) {
        return false;
    }
    for (size_t i = 0; i < input_count; i++) {
        cargo_kind_t input_kind = (cargo_kind_t)inputs[i].kind;
        commodity_t input_commodity = (commodity_t)inputs[i].commodity;
        if (!cargo_kind_matches_commodity(input_kind, input_commodity)) return false;
        bool found = false;
        for (size_t j = 0; j < input_count; j++) {
            if (matched[j]) continue;
            if (input_commodity != recipe->input_commodities[j]) continue;
            matched[j] = true;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

static void hash_recipe_pub(recipe_id_t recipe_id, const uint8_t merkle_root[32],
                            uint16_t output_index, uint8_t out_pub[32]) {
    uint8_t buf[8 + 2 + HASH_BYTES + 2];

    memcpy(buf, MANIFEST_DOMAIN, sizeof(MANIFEST_DOMAIN));
    buf[8] = (uint8_t)((uint16_t)recipe_id);
    buf[9] = (uint8_t)((uint16_t)recipe_id >> 8);
    memcpy(&buf[10], merkle_root, HASH_BYTES);
    buf[42] = (uint8_t)output_index;
    buf[43] = (uint8_t)(output_index >> 8);
    sha256_bytes(buf, sizeof(buf), out_pub);
}

static bool inputs_parent_merkle(const cargo_unit_t *inputs, size_t input_count,
                                 uint8_t out_root[32]) {
    uint8_t *pubs = NULL;
    bool ok;

    if (!inputs || input_count == 0) {
        memset(out_root, 0, HASH_BYTES);
        return false;
    }
    pubs = (uint8_t *)malloc(input_count * HASH_BYTES);
    if (!pubs) return false;
    for (size_t i = 0; i < input_count; i++)
        memcpy(&pubs[i * HASH_BYTES], inputs[i].pub, HASH_BYTES);
    ok = hash_merkle_root((const uint8_t (*)[32])pubs, input_count, out_root);
    free(pubs);
    return ok;
}

const char *cargo_kind_name(cargo_kind_t kind) {
    switch (kind) {
    case CARGO_KIND_INGOT:   return "ingot";
    case CARGO_KIND_FRAME:   return "frame";
    case CARGO_KIND_LASER:   return "laser";
    case CARGO_KIND_TRACTOR: return "tractor";
    default:                 return "unknown";
    }
}

const recipe_def_t *recipe_get(recipe_id_t id) {
    if ((unsigned)id >= RECIPE_COUNT) return NULL;
    return &RECIPE_TABLE[id];
}

bool manifest_init(manifest_t *manifest, uint16_t cap) {
    if (!manifest) return false;
    manifest->count = 0;
    manifest->cap = 0;
    manifest->units = NULL;
    if (cap == 0) return true;
    manifest->units = (cargo_unit_t *)calloc(cap, sizeof(cargo_unit_t));
    if (!manifest->units) return false;
    manifest->cap = cap;
    return true;
}

void manifest_free(manifest_t *manifest) {
    if (!manifest) return;
    free(manifest->units);
    manifest->units = NULL;
    manifest->count = 0;
    manifest->cap = 0;
}

void manifest_clear(manifest_t *manifest) {
    if (!manifest) return;
    if (manifest->units && manifest->cap > 0)
        memset(manifest->units, 0, manifest->cap * sizeof(cargo_unit_t));
    manifest->count = 0;
}

bool manifest_reserve(manifest_t *manifest, uint16_t cap) {
    cargo_unit_t *resized;
    size_t old_cap;

    if (!manifest) return false;
    if (cap <= manifest->cap) return true;
    old_cap = manifest->cap;
    resized = (cargo_unit_t *)realloc(manifest->units, cap * sizeof(cargo_unit_t));
    if (!resized) return false;
    manifest->units = resized;
    memset(&manifest->units[old_cap], 0, (cap - old_cap) * sizeof(cargo_unit_t));
    manifest->cap = cap;
    assert(manifest->count <= manifest->cap);
    return true;
}

bool manifest_clone(manifest_t *dst, const manifest_t *src) {
    manifest_t tmp = {0};

    if (!dst || !src) return false;
    if (dst == src) return true;
    if (!manifest_init(&tmp, src->cap)) return false;
    tmp.count = src->count;
    if (src->count > 0 && src->units)
        memcpy(tmp.units, src->units, src->count * sizeof(cargo_unit_t));
    manifest_free(dst);
    *dst = tmp;
    return true;
}

/* Layer D of #479 — receipt store helpers. Kept inline here so
 * ship_cleanup/_bootstrap/_copy maintain the manifest+receipt parity
 * invariant in lockstep. */
static ship_receipts_t *ship_receipts_alloc(void) {
    ship_receipts_t *r = (ship_receipts_t *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (!ship_receipts_init(r, SHIP_MANIFEST_DEFAULT_CAP)) {
        free(r);
        return NULL;
    }
    return r;
}

static void ship_receipts_destroy(ship_receipts_t *r) {
    if (!r) return;
    ship_receipts_free(r);
    free(r);
}

ship_receipts_t *ship_get_receipts(ship_t *ship) {
    return ship ? cargo_store_receipts(&ship->cargo_store) : NULL;
}

const ship_receipts_t *ship_get_receipts_const(const ship_t *ship) {
    return ship ? cargo_store_receipts_const(&ship->cargo_store) : NULL;
}

ship_receipts_t *station_get_receipts(station_t *station) {
    return station ? cargo_store_receipts(&station->cargo_store) : NULL;
}

const ship_receipts_t *station_get_receipts_const(const station_t *station) {
    return station
        ? cargo_store_receipts_const(&station->cargo_store) : NULL;
}

static bool receipt_store_reconcile(ship_receipts_t *r, const manifest_t *m) {
    if (!r || !m) return false;
    if (r->count < m->count) {
        if (!ship_receipts_reserve(r, m->count)) return false;
        while (r->count < m->count) {
            if (!ship_receipts_push_empty(r)) return false;
        }
    }
    while (r->count > m->count) {
        if (!ship_receipts_remove(r, (uint16_t)(r->count - 1u), NULL))
            return false;
    }
    return true;
}

static bool receipt_store_push_chain_or_empty(ship_receipts_t *r,
                                              const cargo_receipt_chain_t *chain) {
    if (!r) return false;
    if (chain && chain->len > 0) {
        return ship_receipts_push_chain(r, chain->links, chain->len);
    }
    return ship_receipts_push_empty(r);
}

static bool holder_manifest_push_with_chain(manifest_t *manifest,
                                            ship_receipts_t *receipts,
                                            const cargo_unit_t *unit,
                                            const cargo_receipt_chain_t *chain) {
    uint16_t pushed_index;

    if (!manifest || !receipts || !unit) return false;
    if (!receipt_store_reconcile(receipts, manifest)) return false;
    pushed_index = manifest->count;
    if (!manifest_push(manifest, unit)) return false;
    if (!receipt_store_push_chain_or_empty(receipts, chain)) {
        cargo_unit_t rollback = {0};
        (void)manifest_remove(manifest, pushed_index, &rollback);
        return false;
    }
    return true;
}

static bool holder_manifest_remove_with_chain(manifest_t *manifest,
                                              ship_receipts_t *receipts,
                                              uint16_t index,
                                              cargo_unit_t *out_unit,
                                              cargo_receipt_chain_t *out_chain) {
    cargo_unit_t unit = {0};
    cargo_receipt_chain_t chain = {0};

    if (!manifest || !receipts || !manifest->units) return false;
    if (!receipt_store_reconcile(receipts, manifest)) return false;
    if (index >= manifest->count) return false;
    if (!ship_receipts_remove(receipts, index, &chain)) return false;
    if (!manifest_remove(manifest, index, &unit)) {
        (void)receipt_store_push_chain_or_empty(receipts, &chain);
        return false;
    }
    if (out_unit) *out_unit = unit;
    if (out_chain) *out_chain = chain;
    return true;
}

static int holder_manifest_consume_by_commodity(manifest_t *manifest,
                                                ship_receipts_t *receipts,
                                                commodity_t c, int n) {
    if (!manifest || !manifest->units || !receipts || n <= 0) return 0;
    if (!receipt_store_reconcile(receipts, manifest)) return 0;
    int removed = 0;
    for (int16_t i = (int16_t)manifest->count - 1; i >= 0 && removed < n; i--) {
        if (manifest->units[i].commodity == (uint8_t)c) {
            if (holder_manifest_remove_with_chain(manifest, receipts,
                                                  (uint16_t)i, NULL, NULL)) {
                removed++;
            }
        }
    }
    return removed;
}

ship_receipts_t *cargo_store_receipts(cargo_store_t *store) {
    return store ? (ship_receipts_t *)store->receipts_opaque : NULL;
}

const ship_receipts_t *cargo_store_receipts_const(const cargo_store_t *store) {
    return store ? (const ship_receipts_t *)store->receipts_opaque : NULL;
}

bool cargo_store_bootstrap(cargo_store_t *store, uint16_t default_cap) {
    if (!store || default_cap == 0) return false;
    bool manifest_ok = store->manifest.cap > 0 && store->manifest.units;
    if (!manifest_ok) {
        manifest_free(&store->manifest);
        if (!manifest_init(&store->manifest, default_cap)) return false;
    }
    if (!store->receipts_opaque) {
        store->receipts_opaque = ship_receipts_alloc();
        if (!store->receipts_opaque) return false;
    }
    return receipt_store_reconcile(cargo_store_receipts(store),
                                   &store->manifest);
}

void cargo_store_cleanup(cargo_store_t *store) {
    if (!store) return;
    manifest_free(&store->manifest);
    if (store->receipts_opaque) {
        ship_receipts_destroy(cargo_store_receipts(store));
        store->receipts_opaque = NULL;
    }
}

bool cargo_store_clone(cargo_store_t *dst, const cargo_store_t *src) {
    if (!dst || !src) return false;
    if (dst == src) return true;
    cargo_store_t cloned = {0};
    if (!manifest_clone(&cloned.manifest, &src->manifest)) return false;
    if (src->receipts_opaque) {
        cloned.receipts_opaque = calloc(1, sizeof(ship_receipts_t));
        if (!cloned.receipts_opaque ||
            !ship_receipts_clone(cargo_store_receipts(&cloned),
                                 cargo_store_receipts_const(src))) {
            if (cloned.receipts_opaque) free(cloned.receipts_opaque);
            manifest_free(&cloned.manifest);
            return false;
        }
    }
    cargo_store_cleanup(dst);
    *dst = cloned;
    return true;
}

bool cargo_store_push_with_chain(cargo_store_t *store,
                                 const cargo_unit_t *unit,
                                 const cargo_receipt_chain_t *chain) {
    if (!store || !unit) return false;
    if (chain && chain->len > 0 &&
        cargo_receipt_chain_verify(chain->links, chain->len, unit->pub) !=
            CARGO_RECEIPT_OK) return false;
    if (!store->receipts_opaque) return false;
    return holder_manifest_push_with_chain(
        &store->manifest, cargo_store_receipts(store), unit, chain);
}

bool cargo_store_remove_with_chain(cargo_store_t *store, uint16_t index,
                                   cargo_unit_t *out_unit,
                                   cargo_receipt_chain_t *out_chain) {
    if (!store || !store->receipts_opaque) return false;
    return holder_manifest_remove_with_chain(
        &store->manifest, cargo_store_receipts(store), index,
        out_unit, out_chain);
}

int cargo_store_consume_by_commodity(cargo_store_t *store,
                                     commodity_t commodity, int n) {
    if (!store || !store->receipts_opaque || n <= 0) return 0;
    return holder_manifest_consume_by_commodity(
        &store->manifest, cargo_store_receipts(store), commodity, n);
}

void ship_cleanup(ship_t *ship) {
    if (!ship) return;
    cargo_store_cleanup(&ship->cargo_store);
}

bool ship_manifest_bootstrap(ship_t *ship) {
    return ship && cargo_store_bootstrap(
        &ship->cargo_store, SHIP_MANIFEST_DEFAULT_CAP);
}

bool ship_copy(ship_t *dst, const ship_t *src) {
    cargo_store_t cargo_store = {0};

    if (!dst || !src) return false;
    if (dst == src) return true;
    if (!cargo_store_clone(&cargo_store, &src->cargo_store)) return false;
    ship_cleanup(dst);
    *dst = *src;
    dst->cargo_store = cargo_store;
    return true;
}

bool ship_manifest_push_with_chain(ship_t *ship, const cargo_unit_t *unit,
                                   const cargo_receipt_chain_t *chain) {
    if (!ship || !unit) return false;
    if (!ship_manifest_bootstrap(ship)) return false;
    return cargo_store_push_with_chain(&ship->cargo_store, unit, chain);
}

bool ship_manifest_remove_with_chain(ship_t *ship, uint16_t index,
                                     cargo_unit_t *out_unit,
                                     cargo_receipt_chain_t *out_chain) {
    if (!ship) return false;
    if (!ship_manifest_bootstrap(ship)) return false;
    return cargo_store_remove_with_chain(
        &ship->cargo_store, index, out_unit, out_chain);
}

int ship_manifest_consume_by_commodity(ship_t *ship, commodity_t c, int n) {
    if (!ship || n <= 0) return 0;
    if (!ship_manifest_bootstrap(ship)) return 0;
    return cargo_store_consume_by_commodity(&ship->cargo_store, c, n);
}

void station_cleanup(station_t *station) {
    if (!station) return;
    cargo_store_cleanup(&station->cargo_store);
}

bool station_manifest_bootstrap(station_t *station) {
    return station && cargo_store_bootstrap(
        &station->cargo_store, STATION_MANIFEST_DEFAULT_CAP);
}

bool station_copy(station_t *dst, const station_t *src) {
    cargo_store_t cargo_store = {0};

    if (!dst || !src) return false;
    if (dst == src) return true;
    if (!cargo_store_clone(&cargo_store, &src->cargo_store)) return false;
    station_cleanup(dst);
    *dst = *src;
    dst->cargo_store = cargo_store;
    return true;
}

bool station_manifest_push_with_chain(station_t *station,
                                      const cargo_unit_t *unit,
                                      const cargo_receipt_chain_t *chain) {
    if (!station || !unit) return false;
    if (!station_manifest_bootstrap(station)) return false;
    if (!cargo_store_push_with_chain(&station->cargo_store, unit, chain)) {
        return false;
    }
    station->manifest_dirty = true;
    return true;
}

bool station_manifest_remove_with_chain(station_t *station, uint16_t index,
                                        cargo_unit_t *out_unit,
                                        cargo_receipt_chain_t *out_chain) {
    if (!station) return false;
    if (!station_manifest_bootstrap(station)) return false;
    if (!cargo_store_remove_with_chain(
            &station->cargo_store, index, out_unit, out_chain)) {
        return false;
    }
    station->manifest_dirty = true;
    return true;
}

int station_manifest_consume_by_commodity(station_t *station,
                                          commodity_t c, int n) {
    if (!station || n <= 0) return 0;
    if (!station_manifest_bootstrap(station)) return 0;
    int removed = cargo_store_consume_by_commodity(
        &station->cargo_store, c, n);
    if (removed > 0) station->manifest_dirty = true;
    return removed;
}

bool manifest_push(manifest_t *manifest, const cargo_unit_t *unit) {
    if (!manifest || !unit) return false;
    /* Grow on demand. Without this, a ship that's accumulated 32 manifest
     * entries (the default ship cap) would silently reject every BUY of a
     * finished good — the picker would advertise stock, the player would
     * press F, and the transfer would fail because dst was full while
     * the float side suggested cargo room. Doubling keeps the realloc
     * cost amortized O(1). */
    if (manifest->count >= manifest->cap) {
        uint16_t new_cap = manifest->cap > 0 ? (uint16_t)(manifest->cap * 2u) : 32;
        if (new_cap <= manifest->cap) return false; /* uint16 overflow */
        if (!manifest_reserve(manifest, new_cap)) return false;
    }
    if (!manifest->units) return false;
    manifest->units[manifest->count++] = *unit;
    assert(manifest->count <= manifest->cap);
    return true;
}

bool manifest_remove(manifest_t *manifest, uint16_t index, cargo_unit_t *out_unit) {
    if (!manifest || index >= manifest->count || !manifest->units) return false;
    if (out_unit) *out_unit = manifest->units[index];
    if (index + 1u < manifest->count) {
        memmove(&manifest->units[index], &manifest->units[index + 1],
                (manifest->count - index - 1u) * sizeof(cargo_unit_t));
    }
    manifest->count--;
    memset(&manifest->units[manifest->count], 0, sizeof(cargo_unit_t));
    return true;
}

int manifest_find(const manifest_t *manifest, const uint8_t pub[32]) {
    if (!manifest || !pub || !manifest->units) return -1;
    for (uint16_t i = 0; i < manifest->count; i++) {
        if (memcmp(manifest->units[i].pub, pub, HASH_BYTES) == 0)
            return (int)i;
    }
    return -1;
}

int manifest_find_first_cg(const manifest_t *manifest,
                           commodity_t commodity,
                           mining_grade_t grade)
{
    if (!manifest || !manifest->units) return -1;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (u->commodity == (uint8_t)commodity && u->grade == (uint8_t)grade)
            return (int)i;
    }
    return -1;
}

int manifest_count_by_commodity(const manifest_t *manifest, commodity_t commodity) {
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++)
        if (manifest->units[i].commodity == (uint8_t)commodity) n++;
    return n;
}

int manifest_count_by_commodity_grade(const manifest_t *manifest,
                                      commodity_t commodity,
                                      mining_grade_t grade) {
    if (!manifest || !manifest->units ||
        commodity >= COMMODITY_COUNT || grade >= MINING_GRADE_COUNT) {
        return 0;
    }
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *unit = &manifest->units[i];
        if (unit->commodity == (uint8_t)commodity &&
            unit->grade == (uint8_t)grade) {
            n++;
        }
    }
    return n;
}

bool cargo_pod_has_exact_manifest(const cargo_pod_t *pod,
                                  commodity_t commodity) {
    if (!pod || !pod->active || pod->kind != CARGO_POD_CARGO) return false;
    if (pod->shipment_id != 0 || pod->commodity != commodity) return false;
    if (pod->manifest_count == 0 || pod->manifest_count != pod->quantity ||
        pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
        return false;
    }
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if ((commodity_t)pod->manifest_units[i].commodity != commodity)
            return false;
    }
    return true;
}

mining_grade_t cargo_pod_manifest_best_grade(const cargo_pod_t *pod) {
    mining_grade_t best = MINING_GRADE_COMMON;
    if (!pod || pod->manifest_count == 0 ||
        pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
        return best;
    }
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        mining_grade_t grade = (mining_grade_t)pod->manifest_units[i].grade;
        if (grade < MINING_GRADE_COUNT && grade > best) best = grade;
    }
    return best;
}

mining_grade_t cargo_pod_display_grade(const cargo_pod_t *pod) {
    if (!pod || !pod->active) return MINING_GRADE_COMMON;

    bool complete_local = pod->manifest_count > 0 &&
                          pod->manifest_count == pod->quantity &&
                          pod->manifest_count <= CARGO_POD_MANIFEST_CAP;
    if (complete_local) {
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            if ((commodity_t)pod->manifest_units[i].commodity != pod->commodity) {
                complete_local = false;
                break;
            }
        }
    }
    if (complete_local) return cargo_pod_manifest_best_grade(pod);
    if (pod->summary_grade < (uint8_t)MINING_GRADE_COUNT)
        return (mining_grade_t)pod->summary_grade;
    return cargo_pod_manifest_best_grade(pod);
}

static cargo_pod_content_shape_t cargo_content_shape_for_kind(
    cargo_kind_t kind) {
    switch (kind) {
    case CARGO_KIND_INGOT:
    case CARGO_KIND_ORE:
        return CARGO_POD_CONTENT_INGOT;
    case CARGO_KIND_FRAME:
        return CARGO_POD_CONTENT_STRUT;
    case CARGO_KIND_LASER:
    case CARGO_KIND_TRACTOR:
        return CARGO_POD_CONTENT_ACTIVE;
    case CARGO_KIND_REPAIR_KIT:
        return CARGO_POD_CONTENT_SERVICE;
    default:
        return CARGO_POD_CONTENT_MIXED;
    }
}

cargo_pod_content_shape_t cargo_pod_content_shape(const cargo_pod_t *pod) {
    if (!pod || !pod->active || pod->quantity == 0)
        return CARGO_POD_CONTENT_EMPTY;
    if (pod->kind == CARGO_POD_GAS)
        return CARGO_POD_CONTENT_GAS;

    bool detailed = pod->manifest_count > 0 &&
                    pod->manifest_count <= CARGO_POD_MANIFEST_CAP;
    cargo_pod_content_shape_t shape = CARGO_POD_CONTENT_EMPTY;
    if (detailed) {
        for (uint16_t i = 0; i < pod->manifest_count; i++) {
            const cargo_unit_t *unit = &pod->manifest_units[i];
            if (unit->kind >= (uint8_t)CARGO_KIND_COUNT ||
                unit->commodity != (uint8_t)pod->commodity) {
                detailed = false;
                break;
            }
            cargo_pod_content_shape_t unit_shape =
                cargo_content_shape_for_kind((cargo_kind_t)unit->kind);
            if (shape == CARGO_POD_CONTENT_EMPTY) {
                shape = unit_shape;
            } else if (shape != unit_shape) {
                shape = CARGO_POD_CONTENT_MIXED;
            }
        }
        if (detailed && shape != CARGO_POD_CONTENT_EMPTY)
            return shape;
    }

    cargo_kind_t summary_kind;
    if (cargo_kind_for_commodity(pod->commodity, &summary_kind))
        return cargo_content_shape_for_kind(summary_kind);

    /* Raw-ore compatibility pods are physical bulk. Normal live raw ore is
     * still an asteroid fragment and never arrives here as a manifest row. */
    if (pod->commodity < COMMODITY_RAW_ORE_COUNT)
        return CARGO_POD_CONTENT_INGOT;
    return CARGO_POD_CONTENT_MIXED;
}

float cargo_pod_load_ratio(const cargo_pod_t *pod) {
    if (!pod || !pod->active || pod->quantity == 0)
        return 0.0f;
    float ratio = (float)pod->quantity / (float)CARGO_POD_UNIT_CAPACITY;
    return ratio < 1.0f ? ratio : 1.0f;
}

bool manifest_rarity_tint(const manifest_t *manifest, float fill_ratio,
                          float neutral_r, float neutral_g, float neutral_b,
                          float *out_r, float *out_g, float *out_b) {
    if (!out_r || !out_g || !out_b) return false;

    *out_r = neutral_r;
    *out_g = neutral_g;
    *out_b = neutral_b;

    if (!manifest || !manifest->units || manifest->count == 0)
        return false;

    float fill = clampf(fill_ratio, 0.0f, 1.0f);
    if (fill <= 0.0f)
        return false;

    float avg_r = 0.0f, avg_g = 0.0f, avg_b = 0.0f;
    int count = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *unit = &manifest->units[i];
        float grade_r, grade_g, grade_b;
        mining_grade_rgb_f((mining_grade_t)unit->grade,
                           &grade_r, &grade_g, &grade_b);
        avg_r += grade_r;
        avg_g += grade_g;
        avg_b += grade_b;
        count++;
    }

    if (count == 0)
        return false;

    avg_r /= (float)count;
    avg_g /= (float)count;
    avg_b /= (float)count;

    *out_r = lerpf(neutral_r, avg_r, fill);
    *out_g = lerpf(neutral_g, avg_g, fill);
    *out_b = lerpf(neutral_b, avg_b, fill);
    return true;
}

int manifest_consume_by_commodity(manifest_t *manifest, commodity_t commodity, int n) {
    if (!manifest || !manifest->units || n <= 0) return 0;
    int removed = 0;
    /* Walk backwards so manifest_remove (which compacts via tail-swap)
     * doesn't shift later indices we still need to scan. */
    for (int16_t i = (int16_t)manifest->count - 1; i >= 0 && removed < n; i--) {
        if (manifest->units[i].commodity == (uint8_t)commodity) {
            if (manifest_remove(manifest, (uint16_t)i, NULL)) removed++;
        }
    }
    return removed;
}

bool hash_merkle_root(const uint8_t pubs[][32], size_t count, uint8_t out_root[32]) {
    uint8_t *level = NULL;
    uint8_t *next = NULL;
    size_t level_count = count;
    bool ok = false;

    if (!out_root) return false;
    if (!pubs || count == 0) {
        memset(out_root, 0, HASH_BYTES);
        return false;
    }
    if (count == 1) {
        memcpy(out_root, pubs[0], HASH_BYTES);
        return true;
    }

    level = (uint8_t *)malloc(count * HASH_BYTES);
    next = (uint8_t *)malloc(count * HASH_BYTES);
    if (!level || !next) goto done;

    memcpy(level, pubs, count * HASH_BYTES);
    qsort(level, count, HASH_BYTES, compare_pub_32);

    while (level_count > 1) {
        size_t next_count = 0;
        for (size_t i = 0; i < level_count; i += 2) {
            const uint8_t *left = &level[i * HASH_BYTES];
            const uint8_t *right = (i + 1 < level_count)
                ? &level[(i + 1) * HASH_BYTES]
                : left;
            uint8_t pair[HASH_BYTES * 2];
            memcpy(pair, left, HASH_BYTES);
            memcpy(&pair[HASH_BYTES], right, HASH_BYTES);
            sha256_bytes(pair, sizeof(pair), &next[next_count * HASH_BYTES]);
            next_count++;
        }
        memcpy(level, next, next_count * HASH_BYTES);
        level_count = next_count;
    }

    memcpy(out_root, level, HASH_BYTES);
    ok = true;

done:
    free(next);
    free(level);
    return ok;
}

bool hash_ingot(commodity_t commodity, mining_grade_t grade,
                const uint8_t fragment_pub[32], uint16_t output_index,
                cargo_unit_t *out_unit) {
    if (!fragment_pub || !out_unit || !cargo_kind_matches_commodity(CARGO_KIND_INGOT, commodity))
        return false;

    memset(out_unit, 0, sizeof(*out_unit));
    out_unit->kind = (uint8_t)CARGO_KIND_INGOT;
    out_unit->commodity = (uint8_t)commodity;
    out_unit->grade = (uint8_t)grade;
    out_unit->recipe_id = (uint16_t)RECIPE_SMELT;
    out_unit->quantity = 1;  /* finished goods always pool-of-one */
    /* origin_station / mined_block default to 0; smelt-side caller fills
     * them in from the refinery context. */
    memcpy(out_unit->parent_merkle, fragment_pub, HASH_BYTES);
    hash_recipe_pub(RECIPE_SMELT, fragment_pub, output_index, out_unit->pub);
    /* prefix_class is derived from the leading char of base58(pub) — set
     * it once here so callers don't have to recompute. ingot_prefix_t
     * mirrors mining_pubkey_class() one-to-one. */
    out_unit->prefix_class = (uint8_t)mining_pubkey_class(out_unit->pub);
    return true;
}

bool hash_product(recipe_id_t recipe_id, const cargo_unit_t *inputs,
                  size_t input_count, uint16_t output_index,
                  cargo_unit_t *out_unit) {
    const recipe_def_t *recipe = recipe_get(recipe_id);
    uint8_t merkle_root[HASH_BYTES];

    if (!recipe || !out_unit || !inputs) return false;
    if (recipe_id == RECIPE_SMELT || recipe_id == RECIPE_LEGACY_MIGRATE) return false;
    if (output_index >= recipe->output_count) return false;
    if (!cargo_kind_matches_commodity(recipe->output_kind, recipe->output_commodity))
        return false;
    if (!recipe_inputs_match(recipe, inputs, input_count)) return false;
    if (!inputs_parent_merkle(inputs, input_count, merkle_root)) return false;

    memset(out_unit, 0, sizeof(*out_unit));
    out_unit->kind = (uint8_t)recipe->output_kind;
    out_unit->commodity = (uint8_t)recipe->output_commodity;
    out_unit->grade = (uint8_t)min_input_grade(inputs, input_count);
    out_unit->recipe_id = (uint16_t)recipe_id;
    out_unit->quantity = 1;  /* finished goods always pool-of-one */
    memcpy(out_unit->parent_merkle, merkle_root, HASH_BYTES);
    hash_recipe_pub(recipe_id, merkle_root, output_index, out_unit->pub);
    return true;
}

bool cargo_kind_for_commodity(commodity_t commodity, cargo_kind_t *out_kind) {
    if (!out_kind) return false;
    if (commodity_is_ingot(commodity))         { *out_kind = CARGO_KIND_INGOT;      return true; }
    if (commodity == COMMODITY_FRAME)          { *out_kind = CARGO_KIND_FRAME;      return true; }
    if (commodity == COMMODITY_LASER_MODULE)   { *out_kind = CARGO_KIND_LASER;      return true; }
    if (commodity == COMMODITY_TRACTOR_MODULE) { *out_kind = CARGO_KIND_TRACTOR;    return true; }
    if (commodity == COMMODITY_REPAIR_KIT)     { *out_kind = CARGO_KIND_REPAIR_KIT; return true; }
    return false;
}

bool hash_legacy_migrate_unit(const uint8_t origin[8], commodity_t commodity,
                              uint16_t output_index, cargo_unit_t *out_unit) {
    cargo_kind_t kind;
    uint8_t buf[8 + 8 + 8 + 1 + 2]; /* domain + tag + origin + commodity + index */
    size_t o = 0;
    static const uint8_t domain[8]  = { 'S','I','G','N','A','L','v','1' };
    static const uint8_t tag[8]     = { 'L','E','G','A','C','Y','v','1' };

    if (!origin || !out_unit) return false;
    if (!cargo_kind_for_commodity(commodity, &kind)) return false;

    memcpy(&buf[o], domain, sizeof(domain)); o += sizeof(domain);
    memcpy(&buf[o], tag,    sizeof(tag));    o += sizeof(tag);
    memcpy(&buf[o], origin, 8);              o += 8;
    buf[o++] = (uint8_t)commodity;
    buf[o++] = (uint8_t)(output_index);
    buf[o++] = (uint8_t)(output_index >> 8);

    memset(out_unit, 0, sizeof(*out_unit));
    out_unit->kind      = (uint8_t)kind;
    out_unit->commodity = (uint8_t)commodity;
    out_unit->grade     = (uint8_t)MINING_GRADE_COMMON;
    out_unit->recipe_id = (uint16_t)RECIPE_LEGACY_MIGRATE;
    out_unit->quantity  = 1;  /* finished goods always pool-of-one */
    /* parent_merkle stays zero — legacy units have no provable parents. */
    sha256_bytes(buf, o, out_unit->pub);
    return true;
}

bool manifest_migrate_legacy_inventory(manifest_t *manifest,
                                       const float *inventory,
                                       size_t inventory_count,
                                       const uint8_t origin[8]) {
    if (!manifest || !inventory || !origin) return false;
    if (inventory_count == 0 || inventory_count > 255) return false; /* bound the byte cast */

    /* Count how many synthetic units we'll push so we can reserve. */
    uint32_t total = 0;
    for (size_t c = 0; c < inventory_count; c++) {
        cargo_kind_t kind;
        if (!cargo_kind_for_commodity((commodity_t)c, &kind)) continue;
        if (inventory[c] < 1.0f) continue;
        total += (uint32_t)inventory[c];
    }
    if (total == 0) return true;
    if (total > 0xFFFF) total = 0xFFFF; /* manifest.count is uint16 */

    uint16_t needed = (uint16_t)(manifest->count + total);
    if (needed < manifest->count) needed = 0xFFFF; /* overflow guard */
    if (!manifest_reserve(manifest, needed)) return false;

    for (size_t c = 0; c < inventory_count; c++) {
        cargo_kind_t kind;
        if (!cargo_kind_for_commodity((commodity_t)c, &kind)) continue;
        int units = (int)inventory[c];
        for (int i = 0; i < units; i++) {
            if (manifest->count >= manifest->cap) break;
            cargo_unit_t unit = {0};
            if (!hash_legacy_migrate_unit(origin, (commodity_t)c,
                                          (uint16_t)i, &unit))
                continue;
            if (!manifest_push(manifest, &unit)) return false;
        }
    }
    return true;
}

void manifest_migrate_quantity(manifest_t *manifest) {
    if (!manifest || !manifest->units) return;
    for (uint16_t u = 0; u < manifest->count; u++) {
        if (manifest->units[u].quantity == 0)
            manifest->units[u].quantity = 1;
    }
}

/* ---------------------------------------------------------------- */
/* Manifest-as-truth helpers                                         */
/* ---------------------------------------------------------------- */

/* Default 8-byte origin tag when caller passes NULL. */
static const uint8_t kStationDefaultOrigin[8] = { 'S','T','A','T','I','O','N',' ' };

int station_finished_mint(station_t *st, commodity_t c, int n,
                          const uint8_t origin[8]) {
    if (!st || n <= 0) return 0;
    if (!finished_good_commodity(c)) return 0;
    cargo_kind_t kind;
    if (!cargo_kind_for_commodity(c, &kind)) return 0;
    if (st->manifest.cap == 0 && !station_manifest_bootstrap(st)) return 0;

    const uint8_t *use_origin = origin ? origin : kStationDefaultOrigin;
    int minted = 0;
    /* output_index is just provenance disambiguation per call — start at
     * the current manifest count so identical calls hash to distinct
     * units. */
    uint16_t base_idx = st->manifest.count;
    for (int i = 0; i < n; i++) {
        cargo_unit_t unit = {0};
        if (!hash_legacy_migrate_unit(use_origin, c,
                                      (uint16_t)(base_idx + i), &unit)) continue;
        if (!station_manifest_push_with_chain(st, &unit, NULL)) break;
        minted++;
    }
    if (minted > 0) {
        st->manifest_dirty = true;
    }
    assert_finished_invariant(st, c);
    return minted;
}

int ship_finished_count(const ship_t *ship, commodity_t c) {
    if (!ship || !finished_good_commodity(c)) return 0;
    return manifest_count_by_commodity(&ship->manifest, c);
}

void ship_finished_sync(ship_t *ship, commodity_t c) {
    /* Finished counts are queried from cargo_store.manifest. This function
     * remains as a source-compatible no-op while old callers are retired;
     * writing a second float authority here would recreate drift. */
    (void)ship;
    (void)c;
}

int ship_finished_drain(ship_t *ship, commodity_t c, int n) {
    if (!ship || n <= 0 || !finished_good_commodity(c)) return 0;
    return ship_manifest_consume_by_commodity(ship, c, n);
}

int station_finished_count(const station_t *st, commodity_t c) {
    if (!st || !finished_good_commodity(c)) return 0;
    return manifest_count_by_commodity(&st->manifest, c);
}

void station_finished_sync(station_t *st, commodity_t c) {
    if (!st || !finished_good_commodity(c)) return;
    st->manifest_dirty = true;
    assert_finished_invariant(st, c);
}

int station_finished_drain(station_t *st, commodity_t c, int n) {
    if (!st || n <= 0) return 0;
    if (!finished_good_commodity(c)) return 0;
    int drained = station_manifest_consume_by_commodity(st, c, n);
    if (drained > 0) st->manifest_dirty = true;
    assert_finished_invariant(st, c);
    return drained;
}

int station_finished_accumulate(station_t *st, commodity_t c, float amount,
                                const uint8_t origin[8]) {
    if (!st || amount <= 0.0f) return 0;
    if (!finished_good_commodity(c)) return 0;

    float total = st->_finished_residue[c] + amount;
    int delta = (int)floorf(total + 0.0001f);
    float residue = total - (float)delta;
    if (residue < 0.0f) residue = 0.0f;
    if (residue >= 1.0f) residue = 0.0f;
    st->_finished_residue[c] = residue;
    if (delta <= 0) {
        assert_finished_invariant(st, c);
        return 0;
    }
    int minted = station_finished_mint(st, c, delta, origin);
    /* Preserve any units that could not be materialized because the store
     * is full as fractional work only up to the representable interval. */
    if (minted < delta) st->_finished_residue[c] = 0.0f;
    assert_finished_invariant(st, c);
    return minted;
}

int station_finished_consume(station_t *st, commodity_t c, float amount) {
    if (!st || amount <= 0.0f) return 0;
    if (!finished_good_commodity(c)) return 0;

    int whole = station_finished_count(st, c);
    float total = (float)whole + st->_finished_residue[c];
    float after = total - amount;
    if (after < 0.0f) after = 0.0f;
    int target_whole = (int)floorf(after + 0.0001f);
    float residue = after - (float)target_whole;
    if (residue < 0.0f) residue = 0.0f;
    if (residue >= 1.0f) residue = 0.0f;
    int delta = whole - target_whole;
    st->_finished_residue[c] = residue;
    if (delta <= 0) {
        assert_finished_invariant(st, c);
        return 0;
    }
    int drained = station_manifest_consume_by_commodity(st, c, delta);
    if (drained > 0) st->manifest_dirty = true;
    assert_finished_invariant(st, c);
    return drained;
}
