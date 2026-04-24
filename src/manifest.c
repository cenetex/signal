#include "manifest.h"

#include <stdlib.h>
#include <string.h>

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
        .input_count = 1,
        .input_commodities = { COMMODITY_COUNT, COMMODITY_COUNT },
    },
    [RECIPE_FRAME_BASIC] = {
        .id = RECIPE_FRAME_BASIC,
        .name = "frame/basic",
        .output_kind = CARGO_KIND_FRAME,
        .output_commodity = COMMODITY_FRAME,
        .input_count = 2,
        .input_commodities = { COMMODITY_FERRITE_INGOT, COMMODITY_FERRITE_INGOT },
    },
    [RECIPE_LASER_BASIC] = {
        .id = RECIPE_LASER_BASIC,
        .name = "laser/basic",
        .output_kind = CARGO_KIND_LASER,
        .output_commodity = COMMODITY_LASER_MODULE,
        .input_count = 2,
        .input_commodities = { COMMODITY_CUPRITE_INGOT, COMMODITY_CRYSTAL_INGOT },
    },
    [RECIPE_TRACTOR_COIL] = {
        .id = RECIPE_TRACTOR_COIL,
        .name = "tractor/coil",
        .output_kind = CARGO_KIND_TRACTOR,
        .output_commodity = COMMODITY_TRACTOR_MODULE,
        .input_count = 2,
        .input_commodities = { COMMODITY_CUPRITE_INGOT, COMMODITY_CUPRITE_INGOT },
    },
    [RECIPE_LEGACY_MIGRATE] = {
        .id = RECIPE_LEGACY_MIGRATE,
        .name = "legacy/migrate",
        .output_kind = CARGO_KIND_INGOT,
        .output_commodity = COMMODITY_COUNT,
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

static bool recipe_inputs_match(const recipe_def_t *recipe,
                                const cargo_unit_t *inputs,
                                size_t input_count) {
    bool matched[2] = { false, false };

    if (!recipe || input_count != recipe->input_count || input_count > 2) return false;
    for (size_t i = 0; i < input_count; i++) {
        if ((cargo_kind_t)inputs[i].kind != CARGO_KIND_INGOT) return false;
        bool found = false;
        for (size_t j = 0; j < input_count; j++) {
            if (matched[j]) continue;
            if ((commodity_t)inputs[i].commodity != recipe->input_commodities[j]) continue;
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

void ship_cleanup(ship_t *ship) {
    if (!ship) return;
    manifest_free(&ship->manifest);
}

bool ship_manifest_bootstrap(ship_t *ship) {
    if (!ship) return false;
    if (ship->manifest.cap == SHIP_MANIFEST_DEFAULT_CAP && ship->manifest.units) return true;
    manifest_free(&ship->manifest);
    return manifest_init(&ship->manifest, SHIP_MANIFEST_DEFAULT_CAP);
}

bool ship_copy(ship_t *dst, const ship_t *src) {
    manifest_t manifest = {0};

    if (!dst || !src) return false;
    if (dst == src) return true;
    if (!manifest_clone(&manifest, &src->manifest)) return false;
    ship_cleanup(dst);
    *dst = *src;
    dst->manifest = manifest;
    return true;
}

void station_cleanup(station_t *station) {
    if (!station) return;
    manifest_free(&station->manifest);
}

bool station_manifest_bootstrap(station_t *station) {
    if (!station) return false;
    if (station->manifest.cap == STATION_MANIFEST_DEFAULT_CAP && station->manifest.units) return true;
    manifest_free(&station->manifest);
    return manifest_init(&station->manifest, STATION_MANIFEST_DEFAULT_CAP);
}

bool station_copy(station_t *dst, const station_t *src) {
    manifest_t manifest = {0};

    if (!dst || !src) return false;
    if (dst == src) return true;
    if (!manifest_clone(&manifest, &src->manifest)) return false;
    station_cleanup(dst);
    *dst = *src;
    dst->manifest = manifest;
    return true;
}

bool manifest_push(manifest_t *manifest, const cargo_unit_t *unit) {
    if (!manifest || !unit || manifest->count >= manifest->cap || !manifest->units) return false;
    manifest->units[manifest->count++] = *unit;
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
    memcpy(out_unit->parent_merkle, fragment_pub, HASH_BYTES);
    hash_recipe_pub(RECIPE_SMELT, fragment_pub, output_index, out_unit->pub);
    return true;
}

bool hash_product(recipe_id_t recipe_id, const cargo_unit_t *inputs,
                  size_t input_count, uint16_t output_index,
                  cargo_unit_t *out_unit) {
    const recipe_def_t *recipe = recipe_get(recipe_id);
    uint8_t merkle_root[HASH_BYTES];

    if (!recipe || !out_unit || !inputs) return false;
    if (recipe_id == RECIPE_SMELT || recipe_id == RECIPE_LEGACY_MIGRATE) return false;
    if (!cargo_kind_matches_commodity(recipe->output_kind, recipe->output_commodity))
        return false;
    if (!recipe_inputs_match(recipe, inputs, input_count)) return false;
    if (!inputs_parent_merkle(inputs, input_count, merkle_root)) return false;

    memset(out_unit, 0, sizeof(*out_unit));
    out_unit->kind = (uint8_t)recipe->output_kind;
    out_unit->commodity = (uint8_t)recipe->output_commodity;
    out_unit->grade = (uint8_t)min_input_grade(inputs, input_count);
    out_unit->recipe_id = (uint16_t)recipe_id;
    memcpy(out_unit->parent_merkle, merkle_root, HASH_BYTES);
    hash_recipe_pub(recipe_id, merkle_root, output_index, out_unit->pub);
    return true;
}

bool cargo_kind_for_commodity(commodity_t commodity, cargo_kind_t *out_kind) {
    if (!out_kind) return false;
    if (commodity_is_ingot(commodity))     { *out_kind = CARGO_KIND_INGOT;   return true; }
    if (commodity == COMMODITY_FRAME)          { *out_kind = CARGO_KIND_FRAME;   return true; }
    if (commodity == COMMODITY_LASER_MODULE)   { *out_kind = CARGO_KIND_LASER;   return true; }
    if (commodity == COMMODITY_TRACTOR_MODULE) { *out_kind = CARGO_KIND_TRACTOR; return true; }
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
