#include "player_identity_registry.h"

#include "signal_memzero.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG = 0x50495247u,
    PLAYER_IDENTITY_REGISTRY_INITIAL_CAPACITY = 8,
};

_Static_assert(sizeof(size_t) <= sizeof(uint64_t),
               "registry export requires size_t to fit in uint64_t");
_Static_assert(
    PLAYER_IDENTITY_CANONICAL_WIRE_SIZE ==
        PLAYER_IDENTITY_PUBKEY_SIZE +
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE,
    "canonical registry wire-size constant is stale");
_Static_assert(
    PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE ==
        (int)sizeof(uint64_t) + (int)sizeof(uint32_t) +
        2 * (int)sizeof(uint16_t) + (int)sizeof(uint64_t) +
        PLAYER_IDENTITY_PUBKEY_SIZE +
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE,
    "quarantine registry wire-size constant is stale");
_Static_assert(
    PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE == sizeof(uint64_t),
    "actor uniqueness key assumes an eight-byte actor ID");

static bool bytes_nonzero(const uint8_t *bytes, size_t size) {
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < size; i++) any |= bytes[i];
    return any != 0;
}

static void quarantine_entry_copy_values(
    player_identity_quarantine_entry_t *destination,
    const player_identity_quarantine_entry_t *source) {
    memset(destination, 0, sizeof(*destination));
    destination->record_id = source->record_id;
    destination->source_version = source->source_version;
    destination->source_ordinal = source->source_ordinal;
    destination->reason = source->reason;
    memcpy(
        destination->pubkey, source->pubkey,
        sizeof(destination->pubkey));
    memcpy(
        destination->sensitive_actor_id,
        source->sensitive_actor_id,
        sizeof(destination->sensitive_actor_id));
}

static void increment_saturating(uint64_t *value) {
    if (value && *value != UINT64_MAX) (*value)++;
}

bool player_identity_registry_status_is_success(
    player_identity_registry_status_t status) {
    return status >= PLAYER_IDENTITY_REGISTRY_OK &&
           status <= PLAYER_IDENTITY_REGISTRY_OK_CLONED;
}

static player_identity_canonical_store_t *canonical_store_for_tier(
    player_identity_registry_t *reg,
    player_identity_registry_tier_t tier) {
    if (!reg) return NULL;
    if (tier == PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE)
        return &reg->canonical_active;
    if (tier == PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE)
        return &reg->canonical_archive;
    return NULL;
}

static player_identity_registry_status_t registry_failure(
    player_identity_registry_t *reg,
    player_identity_registry_tier_t tier,
    player_identity_registry_status_t status) {
    if (!reg ||
        reg->initialized_tag !=
            PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG ||
        status < 0 ||
        status >= PLAYER_IDENTITY_REGISTRY_STATUS_COUNT ||
        player_identity_registry_status_is_success(status)) {
        return status;
    }

    increment_saturating(&reg->failure_status_counts[status]);
    if (tier == PLAYER_IDENTITY_TIER_QUARANTINE) {
        increment_saturating(&reg->quarantine.failure_count);
        if (status ==
            PLAYER_IDENTITY_REGISTRY_QUARANTINE_BUDGET_EXHAUSTED) {
            increment_saturating(
                &reg->quarantine.budget_failure_count);
        }
        if (status == PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY) {
            increment_saturating(
                &reg->quarantine.allocation_failure_count);
        }
    } else {
        player_identity_canonical_store_t *store =
            canonical_store_for_tier(reg, tier);
        if (store) {
            increment_saturating(&store->failure_count);
            if ((tier ==
                     PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE &&
                 status ==
                     PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED) ||
                (tier ==
                     PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE &&
                 status ==
                     PLAYER_IDENTITY_REGISTRY_ARCHIVE_BUDGET_EXHAUSTED)) {
                increment_saturating(
                    &store->budget_failure_count);
            }
            if (status == PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY) {
                increment_saturating(
                    &store->allocation_failure_count);
            }
        }
    }
    return status;
}

static bool count_limit_fits(size_t limit, size_t entry_size) {
    return entry_size != 0 && limit <= SIZE_MAX / entry_size;
}

static player_identity_registry_status_t config_status(
    const player_identity_registry_config_t *config) {
    if (!config) return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    if (!count_limit_fits(
            config->canonical_active_limit,
            sizeof(player_identity_canonical_entry_t)) ||
        !count_limit_fits(
            config->canonical_archive_limit,
            sizeof(player_identity_canonical_entry_t)) ||
        !count_limit_fits(
            config->quarantine_limit,
            sizeof(player_identity_quarantine_entry_t))) {
        return PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW;
    }
    return PLAYER_IDENTITY_REGISTRY_OK;
}

player_identity_registry_status_t player_identity_registry_init(
    player_identity_registry_t *reg,
    const player_identity_registry_config_t *config) {
    if (!reg || !config)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    if (reg->initialized_tag != 0)
        return PLAYER_IDENTITY_REGISTRY_INVALID_STATE;

    player_identity_registry_status_t status = config_status(config);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) return status;

    memset(reg, 0, sizeof(*reg));
    reg->canonical_active.hard_limit =
        config->canonical_active_limit;
    reg->canonical_archive.hard_limit =
        config->canonical_archive_limit;
    reg->quarantine.hard_limit = config->quarantine_limit;
    reg->initialized_tag =
        PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG;
    return PLAYER_IDENTITY_REGISTRY_OK;
}

void player_identity_registry_cleanup(player_identity_registry_t *reg) {
    if (!reg) return;
    if (reg->initialized_tag ==
        PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG) {
        if (reg->canonical_active.entries &&
            count_limit_fits(
                reg->canonical_active.capacity,
                sizeof(*reg->canonical_active.entries))) {
            signal_memzero_explicit(
                reg->canonical_active.entries,
                reg->canonical_active.capacity *
                    sizeof(*reg->canonical_active.entries));
        }
        if (reg->canonical_archive.entries &&
            count_limit_fits(
                reg->canonical_archive.capacity,
                sizeof(*reg->canonical_archive.entries))) {
            signal_memzero_explicit(
                reg->canonical_archive.entries,
                reg->canonical_archive.capacity *
                    sizeof(*reg->canonical_archive.entries));
        }
        if (reg->quarantine.entries &&
            count_limit_fits(
                reg->quarantine.capacity,
                sizeof(*reg->quarantine.entries))) {
            signal_memzero_explicit(
                reg->quarantine.entries,
                reg->quarantine.capacity *
                    sizeof(*reg->quarantine.entries));
        }
        free(reg->canonical_active.entries);
        free(reg->canonical_archive.entries);
        free(reg->quarantine.entries);
    }
    signal_memzero_explicit(reg, sizeof(*reg));
}

bool player_identity_canonical_entry_is_valid(
    const player_identity_canonical_entry_t *entry) {
    return entry &&
        bytes_nonzero(entry->pubkey, sizeof(entry->pubkey)) &&
        bytes_nonzero(
            entry->sensitive_actor_id,
            sizeof(entry->sensitive_actor_id));
}

bool player_identity_quarantine_entry_is_valid(
    const player_identity_quarantine_entry_t *entry) {
    return entry &&
        entry->record_id != 0 &&
        entry->source_version != 0 &&
        entry->reason >
            PLAYER_IDENTITY_QUARANTINE_REASON_NONE &&
        entry->reason <
            PLAYER_IDENTITY_QUARANTINE_REASON_COUNT;
}

static int pubkey_compare(const uint8_t left[PLAYER_IDENTITY_PUBKEY_SIZE],
                          const uint8_t right[PLAYER_IDENTITY_PUBKEY_SIZE]) {
    return memcmp(left, right, PLAYER_IDENTITY_PUBKEY_SIZE);
}

static bool canonical_store_actor_in_use_by_other(
    const player_identity_canonical_store_t *store,
    const uint8_t
        sensitive_actor_id[
            PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE],
    const player_identity_canonical_entry_t *exempt) {
    for (size_t i = 0; i < store->count; i++) {
        const player_identity_canonical_entry_t *candidate =
            &store->entries[i];
        if (candidate != exempt &&
            memcmp(
                candidate->sensitive_actor_id,
                sensitive_actor_id,
                PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE) == 0) {
            return true;
        }
    }
    return false;
}

static bool canonical_actor_in_use_by_other(
    const player_identity_registry_t *reg,
    const uint8_t
        sensitive_actor_id[
            PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE],
    const player_identity_canonical_entry_t *exempt) {
    return canonical_store_actor_in_use_by_other(
               &reg->canonical_active,
               sensitive_actor_id, exempt) ||
        canonical_store_actor_in_use_by_other(
               &reg->canonical_archive,
               sensitive_actor_id, exempt);
}

static bool canonical_store_shape_is_valid(
    const player_identity_canonical_store_t *store) {
    if (!store ||
        !count_limit_fits(
            store->hard_limit,
            sizeof(player_identity_canonical_entry_t)) ||
        store->count > store->capacity ||
        store->capacity > store->hard_limit ||
        store->high_water < store->count ||
        store->high_water > store->hard_limit) {
        return false;
    }
    return (store->capacity == 0) == (store->entries == NULL);
}

static bool quarantine_store_shape_is_valid(
    const player_identity_quarantine_store_t *store) {
    if (!store ||
        !count_limit_fits(
            store->hard_limit,
            sizeof(player_identity_quarantine_entry_t)) ||
        store->count > store->capacity ||
        store->capacity > store->hard_limit ||
        store->high_water < store->count ||
        store->high_water > store->hard_limit) {
        return false;
    }
    return (store->capacity == 0) == (store->entries == NULL);
}

static bool registry_shape_is_valid(
    const player_identity_registry_t *reg) {
    return reg &&
        reg->initialized_tag ==
            PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG &&
        canonical_store_shape_is_valid(
            &reg->canonical_active) &&
        canonical_store_shape_is_valid(
            &reg->canonical_archive) &&
        quarantine_store_shape_is_valid(&reg->quarantine);
}

static bool canonical_store_is_valid(
    const player_identity_canonical_store_t *store) {
    if (!canonical_store_shape_is_valid(store)) return false;
    for (size_t i = 0; i < store->count; i++) {
        if (!player_identity_canonical_entry_is_valid(
                &store->entries[i])) {
            return false;
        }
        if (i > 0 &&
            pubkey_compare(store->entries[i - 1].pubkey,
                           store->entries[i].pubkey) >= 0) {
            return false;
        }
    }
    return true;
}

static bool canonical_tiers_are_disjoint(
    const player_identity_canonical_store_t *active,
    const player_identity_canonical_store_t *archive) {
    size_t active_index = 0;
    size_t archive_index = 0;
    while (active_index < active->count &&
           archive_index < archive->count) {
        int comparison = pubkey_compare(
            active->entries[active_index].pubkey,
            archive->entries[archive_index].pubkey);
        if (comparison == 0) return false;
        if (comparison < 0)
            active_index++;
        else
            archive_index++;
    }
    return true;
}

static int actor_key_compare(const void *left, const void *right) {
    const uint64_t left_value = *(const uint64_t *)left;
    const uint64_t right_value = *(const uint64_t *)right;
    if (left_value < right_value) return -1;
    if (left_value > right_value) return 1;
    return 0;
}

static player_identity_registry_status_t
canonical_actor_uniqueness_status(
    const player_identity_registry_t *reg) {
    if (reg->canonical_active.count >
        SIZE_MAX - reg->canonical_archive.count) {
        return PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW;
    }
    size_t total = reg->canonical_active.count +
        reg->canonical_archive.count;
    if (total < 2) return PLAYER_IDENTITY_REGISTRY_OK;
    if (total > SIZE_MAX / sizeof(uint64_t))
        return PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW;

    uint64_t *actors = malloc(total * sizeof(*actors));
    if (!actors) return PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY;
    size_t offset = 0;
    for (size_t i = 0;
         i < reg->canonical_active.count; i++) {
        memcpy(
            &actors[offset++],
            reg->canonical_active.entries[i].sensitive_actor_id,
            sizeof(actors[0]));
    }
    for (size_t i = 0;
         i < reg->canonical_archive.count; i++) {
        memcpy(
            &actors[offset++],
            reg->canonical_archive.entries[i].sensitive_actor_id,
            sizeof(actors[0]));
    }

    qsort(actors, total, sizeof(*actors), actor_key_compare);
    player_identity_registry_status_t status =
        PLAYER_IDENTITY_REGISTRY_OK;
    for (size_t i = 1; i < total; i++) {
        if (actors[i - 1] == actors[i]) {
            status = PLAYER_IDENTITY_REGISTRY_INVALID_STATE;
            break;
        }
    }
    signal_memzero_explicit(
        actors, total * sizeof(*actors));
    free(actors);
    return status;
}

static bool quarantine_store_is_valid(
    const player_identity_quarantine_store_t *store) {
    if (!quarantine_store_shape_is_valid(store)) return false;
    for (size_t i = 0; i < store->count; i++) {
        if (!player_identity_quarantine_entry_is_valid(
                &store->entries[i])) {
            return false;
        }
        if (i > 0 &&
            store->entries[i - 1].record_id >=
                store->entries[i].record_id) {
            return false;
        }
    }
    if (store->count > 0 &&
        store->entries[store->count - 1].record_id >
            store->record_id_high_water) {
        return false;
    }
    return true;
}

static player_identity_registry_status_t registry_validation_status(
    const player_identity_registry_t *reg) {
    if (!registry_shape_is_valid(reg) ||
        !canonical_store_is_valid(&reg->canonical_active) ||
        !canonical_store_is_valid(&reg->canonical_archive) ||
        !canonical_tiers_are_disjoint(
            &reg->canonical_active,
            &reg->canonical_archive) ||
        !quarantine_store_is_valid(&reg->quarantine)) {
        return PLAYER_IDENTITY_REGISTRY_INVALID_STATE;
    }
    return canonical_actor_uniqueness_status(reg);
}

bool player_identity_registry_validate(
    const player_identity_registry_t *reg) {
    return registry_validation_status(reg) ==
        PLAYER_IDENTITY_REGISTRY_OK;
}

static size_t canonical_lower_bound(
    const player_identity_canonical_store_t *store,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    bool *found) {
    size_t low = 0;
    size_t high = store->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int comparison =
            pubkey_compare(store->entries[middle].pubkey, pubkey);
        if (comparison < 0)
            low = middle + 1;
        else
            high = middle;
    }
    if (found) {
        *found = low < store->count &&
            pubkey_compare(store->entries[low].pubkey, pubkey) == 0;
    }
    return low;
}

static bool canonical_output_aliases_occupied_row(
    const player_identity_registry_t *reg,
    const player_identity_canonical_entry_t *out_entry) {
    if (!out_entry) return false;
    for (size_t i = 0;
         i < reg->canonical_active.count; i++) {
        if (out_entry == &reg->canonical_active.entries[i])
            return true;
    }
    for (size_t i = 0;
         i < reg->canonical_archive.count; i++) {
        if (out_entry == &reg->canonical_archive.entries[i])
            return true;
    }
    return false;
}

player_identity_registry_status_t
player_identity_registry_find_sensitive(
    const player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    player_identity_canonical_entry_t *out_entry,
    player_identity_registry_tier_t *out_tier) {
    uint8_t pubkey_snapshot[PLAYER_IDENTITY_PUBKEY_SIZE] = {0};
    if (pubkey)
        memcpy(pubkey_snapshot, pubkey, sizeof(pubkey_snapshot));
    if (!reg || !pubkey ||
        !bytes_nonzero(
            pubkey_snapshot, PLAYER_IDENTITY_PUBKEY_SIZE)) {
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    }
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        signal_memzero_explicit(
            pubkey_snapshot, sizeof(pubkey_snapshot));
        return validation_status;
    }
    if (canonical_output_aliases_occupied_row(
            reg, out_entry)) {
        signal_memzero_explicit(
            pubkey_snapshot, sizeof(pubkey_snapshot));
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    }
    if (out_entry) memset(out_entry, 0, sizeof(*out_entry));
    if (out_tier) *out_tier = PLAYER_IDENTITY_TIER_NONE;

    bool found = false;
    size_t index = canonical_lower_bound(
        &reg->canonical_active, pubkey_snapshot, &found);
    if (found) {
        if (out_entry)
            *out_entry = reg->canonical_active.entries[index];
        if (out_tier)
            *out_tier =
                PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE;
        signal_memzero_explicit(
            pubkey_snapshot, sizeof(pubkey_snapshot));
        return PLAYER_IDENTITY_REGISTRY_OK;
    }

    index = canonical_lower_bound(
        &reg->canonical_archive, pubkey_snapshot, &found);
    if (found) {
        if (out_entry)
            *out_entry = reg->canonical_archive.entries[index];
        if (out_tier)
            *out_tier =
                PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE;
        signal_memzero_explicit(
            pubkey_snapshot, sizeof(pubkey_snapshot));
        return PLAYER_IDENTITY_REGISTRY_OK;
    }
    signal_memzero_explicit(
        pubkey_snapshot, sizeof(pubkey_snapshot));
    return PLAYER_IDENTITY_REGISTRY_NOT_FOUND;
}

player_identity_registry_status_t player_identity_registry_find(
    const player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    player_identity_registry_tier_t *out_tier) {
    return player_identity_registry_find_sensitive(
        reg, pubkey, NULL, out_tier);
}

static player_identity_registry_status_t reserve_canonical(
    player_identity_registry_t *reg,
    player_identity_registry_tier_t tier,
    size_t target_count) {
    player_identity_canonical_store_t *store =
        canonical_store_for_tier(reg, tier);
    if (!store)
        return registry_failure(
            reg, tier, PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);

    player_identity_registry_status_t budget_status =
        tier == PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE
            ? PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED
            : PLAYER_IDENTITY_REGISTRY_ARCHIVE_BUDGET_EXHAUSTED;
    if (target_count > store->hard_limit)
        return registry_failure(reg, tier, budget_status);
    if (target_count <= store->capacity)
        return PLAYER_IDENTITY_REGISTRY_OK;

    size_t new_capacity = store->capacity;
    if (new_capacity == 0) {
        new_capacity =
            store->hard_limit <
                PLAYER_IDENTITY_REGISTRY_INITIAL_CAPACITY
            ? store->hard_limit
            : PLAYER_IDENTITY_REGISTRY_INITIAL_CAPACITY;
    }
    while (new_capacity < target_count) {
        if (new_capacity > store->hard_limit / 2)
            new_capacity = store->hard_limit;
        else
            new_capacity *= 2;
    }
    if (!count_limit_fits(
            new_capacity,
            sizeof(player_identity_canonical_entry_t))) {
        return registry_failure(
            reg, tier, PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW);
    }

    size_t bytes =
        new_capacity * sizeof(player_identity_canonical_entry_t);
    void *grown = malloc(bytes);
    if (!grown) {
        return registry_failure(
            reg, tier, PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY);
    }
    if (store->count > 0) {
        memcpy(grown, store->entries,
               store->count * sizeof(*store->entries));
    }
    if (store->entries) {
        signal_memzero_explicit(
            store->entries,
            store->capacity * sizeof(*store->entries));
        free(store->entries);
    }
    store->entries = grown;
    store->capacity = new_capacity;
    return PLAYER_IDENTITY_REGISTRY_OK;
}

static player_identity_registry_status_t reserve_quarantine(
    player_identity_registry_t *reg,
    size_t target_count) {
    player_identity_quarantine_store_t *store = &reg->quarantine;
    if (target_count > store->hard_limit) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_QUARANTINE,
            PLAYER_IDENTITY_REGISTRY_QUARANTINE_BUDGET_EXHAUSTED);
    }
    if (target_count <= store->capacity)
        return PLAYER_IDENTITY_REGISTRY_OK;

    size_t new_capacity = store->capacity;
    if (new_capacity == 0) {
        new_capacity =
            store->hard_limit <
                PLAYER_IDENTITY_REGISTRY_INITIAL_CAPACITY
            ? store->hard_limit
            : PLAYER_IDENTITY_REGISTRY_INITIAL_CAPACITY;
    }
    while (new_capacity < target_count) {
        if (new_capacity > store->hard_limit / 2)
            new_capacity = store->hard_limit;
        else
            new_capacity *= 2;
    }
    if (!count_limit_fits(
            new_capacity,
            sizeof(player_identity_quarantine_entry_t))) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_QUARANTINE,
            PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW);
    }

    size_t bytes =
        new_capacity * sizeof(player_identity_quarantine_entry_t);
    void *grown = malloc(bytes);
    if (!grown) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_QUARANTINE,
            PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY);
    }
    if (store->count > 0) {
        memcpy(grown, store->entries,
               store->count * sizeof(*store->entries));
    }
    if (store->entries) {
        signal_memzero_explicit(
            store->entries,
            store->capacity * sizeof(*store->entries));
        free(store->entries);
    }
    store->entries = grown;
    store->capacity = new_capacity;
    return PLAYER_IDENTITY_REGISTRY_OK;
}

static void canonical_insert_at(
    player_identity_canonical_store_t *store,
    size_t index,
    const player_identity_canonical_entry_t *entry) {
    memmove(&store->entries[index + 1],
            &store->entries[index],
            (store->count - index) * sizeof(*store->entries));
    store->entries[index] = *entry;
    store->count++;
    if (store->high_water < store->count)
        store->high_water = store->count;
}

static void canonical_remove_at(
    player_identity_canonical_store_t *store,
    size_t index) {
    memmove(&store->entries[index],
            &store->entries[index + 1],
            (store->count - index - 1) * sizeof(*store->entries));
    store->count--;
    signal_memzero_explicit(
        &store->entries[store->count],
        sizeof(*store->entries));
}

player_identity_registry_status_t player_identity_registry_insert_active(
    player_identity_registry_t *reg,
    const player_identity_canonical_entry_t *entry) {
    if (!reg || !entry)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_canonical_entry_t candidate = *entry;
    player_identity_registry_status_t status =
        PLAYER_IDENTITY_REGISTRY_OK;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            validation_status);
        goto done;
    }
    if (!player_identity_canonical_entry_is_valid(&candidate)) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
        goto done;
    }

    bool found = false;
    size_t active_index = canonical_lower_bound(
        &reg->canonical_active, candidate.pubkey, &found);
    if (found) {
        if (memcmp(
                reg->canonical_active.entries[active_index]
                    .sensitive_actor_id,
                candidate.sensitive_actor_id,
                PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE) == 0) {
            status = PLAYER_IDENTITY_REGISTRY_OK_EXISTING;
            goto done;
        }
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_CONFLICT);
        goto done;
    }

    (void)canonical_lower_bound(
        &reg->canonical_archive, candidate.pubkey, &found);
    if (found) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_CONFLICT);
        goto done;
    }
    if (canonical_actor_in_use_by_other(
            reg, candidate.sensitive_actor_id, NULL)) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_CONFLICT);
        goto done;
    }

    status = reserve_canonical(
        reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
        reg->canonical_active.count + 1);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) goto done;

    canonical_insert_at(
        &reg->canonical_active, active_index, &candidate);
    status = PLAYER_IDENTITY_REGISTRY_OK_INSERTED;

done:
    signal_memzero_explicit(&candidate, sizeof(candidate));
    return status;
}

player_identity_registry_status_t player_identity_registry_update(
    player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE],
    const uint8_t
        sensitive_actor_id[
            PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE]) {
    if (!reg || !pubkey || !sensitive_actor_id)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    uint8_t pubkey_snapshot[PLAYER_IDENTITY_PUBKEY_SIZE];
    uint8_t actor_snapshot[
        PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE];
    memcpy(pubkey_snapshot, pubkey, sizeof(pubkey_snapshot));
    memcpy(
        actor_snapshot, sensitive_actor_id,
        sizeof(actor_snapshot));
    player_identity_registry_status_t status =
        PLAYER_IDENTITY_REGISTRY_OK;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_NONE,
            validation_status);
        goto done;
    }
    if (!bytes_nonzero(
            pubkey_snapshot, PLAYER_IDENTITY_PUBKEY_SIZE) ||
        !bytes_nonzero(
            actor_snapshot,
            PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE)) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_NONE,
            PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
        goto done;
    }

    bool found = false;
    size_t index = canonical_lower_bound(
        &reg->canonical_active, pubkey_snapshot, &found);
    player_identity_canonical_store_t *store =
        &reg->canonical_active;
    player_identity_registry_tier_t tier =
        PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE;
    if (!found) {
        index = canonical_lower_bound(
            &reg->canonical_archive, pubkey_snapshot, &found);
        store = &reg->canonical_archive;
        tier = PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE;
    }
    if (!found) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_NONE,
            PLAYER_IDENTITY_REGISTRY_NOT_FOUND);
        goto done;
    }
    if (memcmp(
               store->entries[index].sensitive_actor_id,
               actor_snapshot,
               PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE) == 0) {
        status = PLAYER_IDENTITY_REGISTRY_OK_EXISTING;
        goto done;
    }
    if (canonical_actor_in_use_by_other(
            reg, actor_snapshot,
            &store->entries[index])) {
        status = registry_failure(
            reg, tier, PLAYER_IDENTITY_REGISTRY_CONFLICT);
        goto done;
    }
    memcpy(store->entries[index].sensitive_actor_id,
           actor_snapshot,
           PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE);
    status = PLAYER_IDENTITY_REGISTRY_OK_UPDATED;

done:
    signal_memzero_explicit(
        actor_snapshot, sizeof(actor_snapshot));
    signal_memzero_explicit(
        pubkey_snapshot, sizeof(pubkey_snapshot));
    return status;
}

player_identity_registry_status_t player_identity_registry_archive(
    player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE]) {
    if (!reg || !pubkey)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
            validation_status);
    }
    if (!bytes_nonzero(pubkey, PLAYER_IDENTITY_PUBKEY_SIZE)) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
            PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
    }

    bool found = false;
    size_t source_index = canonical_lower_bound(
        &reg->canonical_active, pubkey, &found);
    if (!found) {
        (void)canonical_lower_bound(
            &reg->canonical_archive, pubkey, &found);
        if (found) return PLAYER_IDENTITY_REGISTRY_OK_EXISTING;
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_NOT_FOUND);
    }

    size_t target_index = canonical_lower_bound(
        &reg->canonical_archive, pubkey, &found);
    if (found) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
            PLAYER_IDENTITY_REGISTRY_CONFLICT);
    }
    player_identity_registry_status_t status =
        reserve_canonical(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
            reg->canonical_archive.count + 1);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) return status;

    player_identity_canonical_entry_t moved =
        reg->canonical_active.entries[source_index];
    canonical_insert_at(
        &reg->canonical_archive, target_index, &moved);
    canonical_remove_at(
        &reg->canonical_active, source_index);
    signal_memzero_explicit(&moved, sizeof(moved));
    return PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED;
}

player_identity_registry_status_t player_identity_registry_restore(
    player_identity_registry_t *reg,
    const uint8_t pubkey[PLAYER_IDENTITY_PUBKEY_SIZE]) {
    if (!reg || !pubkey)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            validation_status);
    }
    if (!bytes_nonzero(pubkey, PLAYER_IDENTITY_PUBKEY_SIZE)) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
    }

    bool found = false;
    size_t source_index = canonical_lower_bound(
        &reg->canonical_archive, pubkey, &found);
    if (!found) {
        (void)canonical_lower_bound(
            &reg->canonical_active, pubkey, &found);
        if (found) return PLAYER_IDENTITY_REGISTRY_OK_EXISTING;
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
            PLAYER_IDENTITY_REGISTRY_NOT_FOUND);
    }

    size_t target_index = canonical_lower_bound(
        &reg->canonical_active, pubkey, &found);
    if (found) {
        return registry_failure(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            PLAYER_IDENTITY_REGISTRY_CONFLICT);
    }
    player_identity_registry_status_t status =
        reserve_canonical(
            reg, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
            reg->canonical_active.count + 1);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) return status;

    player_identity_canonical_entry_t moved =
        reg->canonical_archive.entries[source_index];
    canonical_insert_at(
        &reg->canonical_active, target_index, &moved);
    canonical_remove_at(
        &reg->canonical_archive, source_index);
    signal_memzero_explicit(&moved, sizeof(moved));
    return PLAYER_IDENTITY_REGISTRY_OK_RESTORED;
}

player_identity_registry_status_t
player_identity_registry_next_quarantine_id(
    const player_identity_registry_t *reg,
    uint64_t *out_record_id) {
    if (out_record_id) *out_record_id = 0;
    if (!reg || !out_record_id)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK)
        return validation_status;
    if (reg->quarantine.record_id_high_water == UINT64_MAX)
        return PLAYER_IDENTITY_REGISTRY_QUARANTINE_ID_EXHAUSTED;
    *out_record_id =
        reg->quarantine.record_id_high_water + 1;
    return PLAYER_IDENTITY_REGISTRY_OK;
}

player_identity_registry_status_t
player_identity_registry_quarantine_append(
    player_identity_registry_t *reg,
    const player_identity_quarantine_entry_t *entry) {
    if (!reg || !entry)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_quarantine_entry_t candidate;
    quarantine_entry_copy_values(&candidate, entry);
    player_identity_registry_status_t status =
        PLAYER_IDENTITY_REGISTRY_OK;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_QUARANTINE,
            validation_status);
        goto done;
    }
    if (!player_identity_quarantine_entry_is_valid(&candidate)) {
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_QUARANTINE,
            PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT);
        goto done;
    }
    if (candidate.record_id <=
        reg->quarantine.record_id_high_water) {
        status =
            reg->quarantine.record_id_high_water == UINT64_MAX
            ? PLAYER_IDENTITY_REGISTRY_QUARANTINE_ID_EXHAUSTED
            : PLAYER_IDENTITY_REGISTRY_CONFLICT;
        status = registry_failure(
            reg, PLAYER_IDENTITY_TIER_QUARANTINE, status);
        goto done;
    }

    status = reserve_quarantine(
        reg, reg->quarantine.count + 1);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) goto done;

    quarantine_entry_copy_values(
        &reg->quarantine.entries[reg->quarantine.count],
        &candidate);
    reg->quarantine.count++;
    if (reg->quarantine.high_water <
        reg->quarantine.count) {
        reg->quarantine.high_water =
            reg->quarantine.count;
    }
    reg->quarantine.record_id_high_water =
        candidate.record_id;
    status = PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED;

done:
    signal_memzero_explicit(&candidate, sizeof(candidate));
    return status;
}

static size_t hard_bytes_or_max(size_t limit, size_t entry_size) {
    if (!count_limit_fits(limit, entry_size)) return SIZE_MAX;
    return limit * entry_size;
}

static player_identity_registry_tier_metrics_t canonical_metrics(
    const player_identity_canonical_store_t *store) {
    player_identity_registry_tier_metrics_t metrics = {
        .count = store->count,
        .capacity = store->capacity,
        .hard_limit = store->hard_limit,
        .hard_bytes = hard_bytes_or_max(
            store->hard_limit,
            sizeof(player_identity_canonical_entry_t)),
        .high_water = store->high_water,
        .failure_count = store->failure_count,
        .budget_failure_count =
            store->budget_failure_count,
        .allocation_failure_count =
            store->allocation_failure_count,
    };
    return metrics;
}

void player_identity_registry_get_metrics(
    const player_identity_registry_t *reg,
    player_identity_registry_metrics_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!reg ||
        reg->initialized_tag !=
            PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG) {
        return;
    }
    out->canonical_active =
        canonical_metrics(&reg->canonical_active);
    out->canonical_archive =
        canonical_metrics(&reg->canonical_archive);
    out->quarantine.count = reg->quarantine.count;
    out->quarantine.capacity = reg->quarantine.capacity;
    out->quarantine.hard_limit =
        reg->quarantine.hard_limit;
    out->quarantine.hard_bytes = hard_bytes_or_max(
        reg->quarantine.hard_limit,
        sizeof(player_identity_quarantine_entry_t));
    out->quarantine.high_water =
        reg->quarantine.high_water;
    out->quarantine.failure_count =
        reg->quarantine.failure_count;
    out->quarantine.budget_failure_count =
        reg->quarantine.budget_failure_count;
    out->quarantine.allocation_failure_count =
        reg->quarantine.allocation_failure_count;
    out->quarantine_record_id_high_water =
        reg->quarantine.record_id_high_water;
    memcpy(out->failure_status_counts,
           reg->failure_status_counts,
           sizeof(out->failure_status_counts));
}

player_identity_registry_status_t player_identity_registry_visit(
    const player_identity_registry_t *reg,
    player_identity_canonical_visit_fn canonical_visit,
    player_identity_quarantine_visit_fn quarantine_visit,
    void *user) {
    if (!reg || (!canonical_visit && !quarantine_visit))
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK)
        return validation_status;

    if (canonical_visit) {
        for (size_t i = 0;
             i < reg->canonical_active.count; i++) {
            player_identity_canonical_redacted_t redacted = {0};
            memcpy(
                redacted.pubkey,
                reg->canonical_active.entries[i].pubkey,
                sizeof(redacted.pubkey));
            if (!canonical_visit(
                    PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
                    i, &redacted,
                    user)) {
                return PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED;
            }
        }
        for (size_t i = 0;
             i < reg->canonical_archive.count; i++) {
            player_identity_canonical_redacted_t redacted = {0};
            memcpy(
                redacted.pubkey,
                reg->canonical_archive.entries[i].pubkey,
                sizeof(redacted.pubkey));
            if (!canonical_visit(
                    PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
                    i, &redacted,
                    user)) {
                return PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED;
            }
        }
    }
    if (quarantine_visit) {
        for (size_t i = 0;
             i < reg->quarantine.count; i++) {
            const player_identity_quarantine_entry_t *source =
                &reg->quarantine.entries[i];
            player_identity_quarantine_redacted_t redacted;
            memset(&redacted, 0, sizeof(redacted));
            redacted.record_id = source->record_id;
            redacted.source_version = source->source_version;
            redacted.source_ordinal = source->source_ordinal;
            redacted.reason = source->reason;
            memcpy(
                redacted.pubkey, source->pubkey,
                sizeof(redacted.pubkey));
            if (!quarantine_visit(
                    i, &redacted, user)) {
                return PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED;
            }
        }
    }
    return PLAYER_IDENTITY_REGISTRY_OK;
}

player_identity_registry_status_t
player_identity_registry_visit_sensitive(
    const player_identity_registry_t *reg,
    player_identity_canonical_sensitive_visit_fn
        canonical_visit,
    player_identity_quarantine_sensitive_visit_fn
        quarantine_visit,
    void *user) {
    if (!reg || (!canonical_visit && !quarantine_visit))
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK)
        return validation_status;

    if (canonical_visit) {
        for (size_t i = 0;
             i < reg->canonical_active.count; i++) {
            if (!canonical_visit(
                    PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
                    i, &reg->canonical_active.entries[i],
                    user)) {
                return PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED;
            }
        }
        for (size_t i = 0;
             i < reg->canonical_archive.count; i++) {
            if (!canonical_visit(
                    PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
                    i, &reg->canonical_archive.entries[i],
                    user)) {
                return PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED;
            }
        }
    }
    if (quarantine_visit) {
        for (size_t i = 0;
             i < reg->quarantine.count; i++) {
            player_identity_quarantine_entry_t normalized;
            quarantine_entry_copy_values(
                &normalized, &reg->quarantine.entries[i]);
            if (!quarantine_visit(
                    i, &normalized, user)) {
                signal_memzero_explicit(
                    &normalized, sizeof(normalized));
                return PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED;
            }
            signal_memzero_explicit(
                &normalized, sizeof(normalized));
        }
    }
    return PLAYER_IDENTITY_REGISTRY_OK;
}

static bool checked_size_add(
    size_t left, size_t right, size_t *out) {
    if (!out || left > SIZE_MAX - right) return false;
    *out = left + right;
    return true;
}

static bool checked_size_mul(
    size_t left, size_t right, size_t *out) {
    if (!out || (right != 0 && left > SIZE_MAX / right))
        return false;
    *out = left * right;
    return true;
}

player_identity_registry_status_t
player_identity_registry_export_sensitive_size(
    const player_identity_registry_t *reg,
    size_t *out_size) {
    if (out_size) *out_size = 0;
    if (!reg || !out_size)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(reg);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK)
        return validation_status;

    size_t active_bytes = 0;
    size_t archive_bytes = 0;
    size_t quarantine_bytes = 0;
    size_t total = PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE;
    if (!checked_size_mul(
            reg->canonical_active.count,
            PLAYER_IDENTITY_CANONICAL_WIRE_SIZE,
            &active_bytes) ||
        !checked_size_mul(
            reg->canonical_archive.count,
            PLAYER_IDENTITY_CANONICAL_WIRE_SIZE,
            &archive_bytes) ||
        !checked_size_mul(
            reg->quarantine.count,
            PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE,
            &quarantine_bytes) ||
        !checked_size_add(total, active_bytes, &total) ||
        !checked_size_add(total, archive_bytes, &total) ||
        !checked_size_add(total, quarantine_bytes, &total)) {
        return PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW;
    }
    *out_size = total;
    return PLAYER_IDENTITY_REGISTRY_OK;
}

static void write_u16_le(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void write_u32_le(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static void write_u64_le(uint8_t *out, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out[shift / 8] =
            (uint8_t)((value >> shift) & UINT64_C(0xff));
}

static void export_canonical_entry(
    uint8_t *out,
    const player_identity_canonical_entry_t *entry) {
    memcpy(out, entry->pubkey, PLAYER_IDENTITY_PUBKEY_SIZE);
    memcpy(out + PLAYER_IDENTITY_PUBKEY_SIZE,
           entry->sensitive_actor_id,
           PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE);
}

static void export_quarantine_entry(
    uint8_t *out,
    const player_identity_quarantine_entry_t *entry) {
    write_u64_le(out, entry->record_id);
    write_u32_le(out + 8, entry->source_version);
    write_u16_le(out + 12, entry->reason);
    write_u16_le(out + 14, 0);
    write_u64_le(out + 16, entry->source_ordinal);
    memcpy(out + 24, entry->pubkey,
           PLAYER_IDENTITY_PUBKEY_SIZE);
    memcpy(out + 24 + PLAYER_IDENTITY_PUBKEY_SIZE,
           entry->sensitive_actor_id,
           PLAYER_IDENTITY_SENSITIVE_ACTOR_ID_SIZE);
}

player_identity_registry_status_t
player_identity_registry_export_sensitive(
    const player_identity_registry_t *reg,
    uint8_t *out,
    size_t out_capacity,
    size_t *out_written) {
    if (out_written) *out_written = 0;
    if (!reg || !out || !out_written)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;

    size_t required = 0;
    player_identity_registry_status_t status =
        player_identity_registry_export_sensitive_size(
            reg, &required);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) return status;
    if (out_capacity < required) {
        *out_written = required;
        return PLAYER_IDENTITY_REGISTRY_BUFFER_TOO_SMALL;
    }

    memset(out, 0, PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE);
    out[0] = 'P';
    out[1] = 'I';
    out[2] = 'R';
    out[3] = '1';
    write_u16_le(
        out + 4, PLAYER_IDENTITY_REGISTRY_EXPORT_VERSION);
    write_u16_le(
        out + 6, PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE);
    write_u16_le(
        out + 8, PLAYER_IDENTITY_CANONICAL_WIRE_SIZE);
    write_u16_le(
        out + 10, PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE);
    write_u32_le(
        out + 12,
        PLAYER_IDENTITY_REGISTRY_EXPORT_FLAG_SENSITIVE_BEARERS);
    write_u64_le(
        out + 16,
        (uint64_t)reg->canonical_active.hard_limit);
    write_u64_le(
        out + 24,
        (uint64_t)reg->canonical_archive.hard_limit);
    write_u64_le(
        out + 32,
        (uint64_t)reg->quarantine.hard_limit);
    write_u64_le(
        out + 40,
        (uint64_t)reg->canonical_active.high_water);
    write_u64_le(
        out + 48,
        (uint64_t)reg->canonical_archive.high_water);
    write_u64_le(
        out + 56,
        (uint64_t)reg->quarantine.high_water);
    write_u64_le(
        out + 64,
        reg->quarantine.record_id_high_water);
    write_u64_le(
        out + 72,
        (uint64_t)reg->canonical_active.count);
    write_u64_le(
        out + 80,
        (uint64_t)reg->canonical_archive.count);
    write_u64_le(
        out + 88,
        (uint64_t)reg->quarantine.count);

    size_t offset = PLAYER_IDENTITY_REGISTRY_EXPORT_HEADER_SIZE;
    for (size_t i = 0;
         i < reg->canonical_active.count; i++) {
        export_canonical_entry(
            out + offset, &reg->canonical_active.entries[i]);
        offset += PLAYER_IDENTITY_CANONICAL_WIRE_SIZE;
    }
    for (size_t i = 0;
         i < reg->canonical_archive.count; i++) {
        export_canonical_entry(
            out + offset, &reg->canonical_archive.entries[i]);
        offset += PLAYER_IDENTITY_CANONICAL_WIRE_SIZE;
    }
    for (size_t i = 0; i < reg->quarantine.count; i++) {
        export_quarantine_entry(
            out + offset, &reg->quarantine.entries[i]);
        offset += PLAYER_IDENTITY_QUARANTINE_WIRE_SIZE;
    }
    if (offset != required)
        return PLAYER_IDENTITY_REGISTRY_INVALID_STATE;
    *out_written = offset;
    return PLAYER_IDENTITY_REGISTRY_OK;
}

player_identity_registry_status_t player_identity_registry_clone(
    player_identity_registry_t *destination,
    const player_identity_registry_t *source) {
    if (!destination || !source)
        return PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT;
    player_identity_registry_status_t validation_status =
        registry_validation_status(source);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK)
        return validation_status;
    if (destination == source)
        return PLAYER_IDENTITY_REGISTRY_OK_EXISTING;

    bool destination_initialized =
        destination->initialized_tag ==
            PLAYER_IDENTITY_REGISTRY_INITIALIZED_TAG;
    if (destination->initialized_tag != 0 &&
        !destination_initialized) {
        return PLAYER_IDENTITY_REGISTRY_INVALID_STATE;
    }
    if (destination_initialized) {
        validation_status =
            registry_validation_status(destination);
        if (validation_status != PLAYER_IDENTITY_REGISTRY_OK)
            return validation_status;
    }

    player_identity_registry_t copy = {0};
    player_identity_registry_config_t config = {
        .canonical_active_limit =
            source->canonical_active.hard_limit,
        .canonical_archive_limit =
            source->canonical_archive.hard_limit,
        .quarantine_limit =
            source->quarantine.hard_limit,
    };
    player_identity_registry_status_t status =
        player_identity_registry_init(&copy, &config);
    if (status != PLAYER_IDENTITY_REGISTRY_OK) return status;

    status = reserve_canonical(
        &copy, PLAYER_IDENTITY_TIER_CANONICAL_ACTIVE,
        source->canonical_active.count);
    if (status == PLAYER_IDENTITY_REGISTRY_OK) {
        status = reserve_canonical(
            &copy, PLAYER_IDENTITY_TIER_CANONICAL_ARCHIVE,
            source->canonical_archive.count);
    }
    if (status == PLAYER_IDENTITY_REGISTRY_OK) {
        status = reserve_quarantine(
            &copy, source->quarantine.count);
    }
    if (status != PLAYER_IDENTITY_REGISTRY_OK) {
        player_identity_registry_cleanup(&copy);
        return status;
    }

    if (source->canonical_active.count > 0) {
        memcpy(copy.canonical_active.entries,
               source->canonical_active.entries,
               source->canonical_active.count *
                   sizeof(*source->canonical_active.entries));
    }
    if (source->canonical_archive.count > 0) {
        memcpy(copy.canonical_archive.entries,
               source->canonical_archive.entries,
               source->canonical_archive.count *
                   sizeof(*source->canonical_archive.entries));
    }
    if (source->quarantine.count > 0) {
        memcpy(copy.quarantine.entries,
               source->quarantine.entries,
               source->quarantine.count *
                   sizeof(*source->quarantine.entries));
    }

    copy.canonical_active.count =
        source->canonical_active.count;
    copy.canonical_active.high_water =
        source->canonical_active.high_water;
    copy.canonical_active.failure_count =
        source->canonical_active.failure_count;
    copy.canonical_active.budget_failure_count =
        source->canonical_active.budget_failure_count;
    copy.canonical_active.allocation_failure_count =
        source->canonical_active.allocation_failure_count;

    copy.canonical_archive.count =
        source->canonical_archive.count;
    copy.canonical_archive.high_water =
        source->canonical_archive.high_water;
    copy.canonical_archive.failure_count =
        source->canonical_archive.failure_count;
    copy.canonical_archive.budget_failure_count =
        source->canonical_archive.budget_failure_count;
    copy.canonical_archive.allocation_failure_count =
        source->canonical_archive.allocation_failure_count;

    copy.quarantine.count = source->quarantine.count;
    copy.quarantine.high_water =
        source->quarantine.high_water;
    copy.quarantine.record_id_high_water =
        source->quarantine.record_id_high_water;
    copy.quarantine.failure_count =
        source->quarantine.failure_count;
    copy.quarantine.budget_failure_count =
        source->quarantine.budget_failure_count;
    copy.quarantine.allocation_failure_count =
        source->quarantine.allocation_failure_count;
    memcpy(copy.failure_status_counts,
           source->failure_status_counts,
           sizeof(copy.failure_status_counts));

    validation_status = registry_validation_status(&copy);
    if (validation_status != PLAYER_IDENTITY_REGISTRY_OK) {
        player_identity_registry_cleanup(&copy);
        return validation_status;
    }
    if (destination_initialized)
        player_identity_registry_cleanup(destination);
    *destination = copy;
    memset(&copy, 0, sizeof(copy));
    return PLAYER_IDENTITY_REGISTRY_OK_CLONED;
}

const char *player_identity_registry_status_name(
    player_identity_registry_status_t status) {
    switch (status) {
        case PLAYER_IDENTITY_REGISTRY_OK:
            return "ok";
        case PLAYER_IDENTITY_REGISTRY_OK_EXISTING:
            return "ok_existing";
        case PLAYER_IDENTITY_REGISTRY_OK_INSERTED:
            return "ok_inserted";
        case PLAYER_IDENTITY_REGISTRY_OK_UPDATED:
            return "ok_updated";
        case PLAYER_IDENTITY_REGISTRY_OK_ARCHIVED:
            return "ok_archived";
        case PLAYER_IDENTITY_REGISTRY_OK_RESTORED:
            return "ok_restored";
        case PLAYER_IDENTITY_REGISTRY_OK_QUARANTINED:
            return "ok_quarantined";
        case PLAYER_IDENTITY_REGISTRY_OK_CLONED:
            return "ok_cloned";
        case PLAYER_IDENTITY_REGISTRY_INVALID_ARGUMENT:
            return "invalid_argument";
        case PLAYER_IDENTITY_REGISTRY_INVALID_STATE:
            return "invalid_state";
        case PLAYER_IDENTITY_REGISTRY_NOT_FOUND:
            return "not_found";
        case PLAYER_IDENTITY_REGISTRY_CONFLICT:
            return "conflict";
        case PLAYER_IDENTITY_REGISTRY_ACTIVE_BUDGET_EXHAUSTED:
            return "canonical_active_budget_exhausted";
        case PLAYER_IDENTITY_REGISTRY_ARCHIVE_BUDGET_EXHAUSTED:
            return "canonical_archive_budget_exhausted";
        case PLAYER_IDENTITY_REGISTRY_QUARANTINE_BUDGET_EXHAUSTED:
            return "quarantine_budget_exhausted";
        case PLAYER_IDENTITY_REGISTRY_QUARANTINE_ID_EXHAUSTED:
            return "quarantine_id_exhausted";
        case PLAYER_IDENTITY_REGISTRY_SIZE_OVERFLOW:
            return "size_overflow";
        case PLAYER_IDENTITY_REGISTRY_OUT_OF_MEMORY:
            return "out_of_memory";
        case PLAYER_IDENTITY_REGISTRY_BUFFER_TOO_SMALL:
            return "buffer_too_small";
        case PLAYER_IDENTITY_REGISTRY_VISITOR_STOPPED:
            return "visitor_stopped";
        case PLAYER_IDENTITY_REGISTRY_STATUS_COUNT:
        default:
            return "unknown";
    }
}
