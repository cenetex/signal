#include "test_harness.h"

#include "actor_principal.h"
#include "actor_principal_resolver.h"

enum {
    TEST_CATALOG_HEADER_SIZE = 8,
    TEST_CATALOG_ID_OFFSET = TEST_CATALOG_HEADER_SIZE,
    TEST_CATALOG_CRC_SIZE = 4,
};

static uint32_t station_catalog_test_crc32_update(
    uint32_t crc,
    const void *buf,
    size_t len) {
    const uint8_t *bytes = buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^
                (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool station_catalog_test_rewrite_crc(const char *path) {
    FILE *f = fopen(path, "rb+");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < TEST_CATALOG_CRC_SIZE) {
        fclose(f);
        return false;
    }

    long payload_size = size - TEST_CATALOG_CRC_SIZE;
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint32_t crc = 0;
    uint8_t chunk[4096];
    long remaining = payload_size;
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk)
            ? (size_t)remaining
            : sizeof(chunk);
        size_t read_size = fread(chunk, 1, want, f);
        if (read_size != want) {
            fclose(f);
            return false;
        }
        crc = station_catalog_test_crc32_update(crc, chunk, read_size);
        remaining -= (long)read_size;
    }
    bool ok = fseek(f, payload_size, SEEK_SET) == 0 &&
        fwrite(&crc, sizeof(crc), 1, f) == 1 &&
        fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool station_catalog_test_patch(
    const char *path,
    long offset,
    const void *bytes,
    size_t size) {
    FILE *f = fopen(path, "rb+");
    if (!f) return false;
    bool ok = fseek(f, offset, SEEK_SET) == 0 &&
        fwrite(bytes, 1, size, f) == size &&
        fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok && station_catalog_test_rewrite_crc(path);
}

static bool station_catalog_test_patch_actor(
    const char *path,
    const uint8_t actor_id[ACTOR_PRINCIPAL_ID_SIZE]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long size = ok ? ftell(f) : -1;
    if (fclose(f) != 0) ok = false;
    if (!ok ||
        size < TEST_CATALOG_CRC_SIZE + ACTOR_PRINCIPAL_ID_SIZE) {
        return false;
    }
    long actor_offset =
        size - TEST_CATALOG_CRC_SIZE - ACTOR_PRINCIPAL_ID_SIZE;
    return station_catalog_test_patch(
        path, actor_offset, actor_id, ACTOR_PRINCIPAL_ID_SIZE);
}

static bool station_catalog_test_downgrade_to_v7(const char *path) {
    FILE *f = fopen(path, "rb+");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size < TEST_CATALOG_CRC_SIZE + ACTOR_PRINCIPAL_ID_SIZE) {
        fclose(f);
        return false;
    }
    long legacy_payload_size =
        size - TEST_CATALOG_CRC_SIZE - ACTOR_PRINCIPAL_ID_SIZE;
    uint32_t version = 7;
    if (fseek(f, sizeof(uint32_t), SEEK_SET) != 0 ||
        fwrite(&version, sizeof(version), 1, f) != 1 ||
        fflush(f) != 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    uint32_t crc = 0;
    uint8_t chunk[4096];
    long remaining = legacy_payload_size;
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk)
            ? (size_t)remaining
            : sizeof(chunk);
        size_t read_size = fread(chunk, 1, want, f);
        if (read_size != want) {
            fclose(f);
            return false;
        }
        crc = station_catalog_test_crc32_update(crc, chunk, read_size);
        remaining -= (long)read_size;
    }
    bool ok = fseek(f, legacy_payload_size, SEEK_SET) == 0 &&
        fwrite(&crc, sizeof(crc), 1, f) == 1 &&
        fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

TEST(test_station_catalog_v8_round_trips_immutable_actor_identity) {
    const char *dir = TMP("test_station_catalog_v8_roundtrip");
    WORLD_HEAP source = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(source != NULL);
    ASSERT(loaded != NULL);
    world_reset(source);
    world_reset(loaded);

    uint8_t expected_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(expected_actor, source->stations[0].station_actor_id,
           sizeof(expected_actor));
    uint32_t expected_id = source->stations[0].id;
    memset(loaded->stations[0].station_actor_id, 0xA7,
           sizeof(loaded->stations[0].station_actor_id));

    ASSERT(station_catalog_save_all(
        source->stations, MAX_STATIONS, dir));
    ASSERT_EQ_INT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS, dir), SIGNAL_SEEDED_STATION_COUNT);
    ASSERT_EQ_INT((int)loaded->stations[0].id, (int)expected_id);
    ASSERT(memcmp(loaded->stations[0].station_actor_id,
                  expected_actor, sizeof(expected_actor)) == 0);
}

TEST(test_station_catalog_v7_does_not_attest_bootstrap_actor) {
    const char *dir = TMP("test_station_catalog_v7_actor_reconcile");
    WORLD_HEAP source = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(source != NULL);
    ASSERT(loaded != NULL);
    world_reset(source);
    world_reset(loaded);
    ASSERT(station_catalog_save(
        &source->stations[0], 0, dir));
    char path[256];
    ASSERT(snprintf(path, sizeof(path), "%s/0.cat", dir) > 0);
    ASSERT(station_catalog_test_downgrade_to_v7(path));
    ASSERT_EQ_INT(station_catalog_load_all(
        loaded->stations, MAX_STATIONS, dir), 1);
    ASSERT(memcmp(loaded->stations[0].station_actor_id,
                  (const uint8_t[ACTOR_PRINCIPAL_ID_SIZE]){0},
                  ACTOR_PRINCIPAL_ID_SIZE) == 0);
    ASSERT(!loaded->stations[0].station_actor_catalog_attested);
}

TEST(test_station_catalog_v8_save_rejects_ambiguous_identity_sets) {
    WORLD_HEAP world = calloc(1, sizeof(world_t));
    ASSERT(world != NULL);
    world_reset(world);

    uint8_t saved_actor[ACTOR_PRINCIPAL_ID_SIZE];
    uint8_t second_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(saved_actor, world->stations[0].station_actor_id,
           sizeof(saved_actor));
    memcpy(second_actor, world->stations[1].station_actor_id,
           sizeof(second_actor));
    memset(world->stations[0].station_actor_id, 0,
           sizeof(world->stations[0].station_actor_id));
    ASSERT(!station_catalog_save(
        &world->stations[0], 0, TMP("test_station_catalog_zero_save")));

    memcpy(world->stations[0].station_actor_id, saved_actor,
           sizeof(saved_actor));
    uint32_t first_id = world->stations[0].id;
    world->stations[0].id = 0;
    ASSERT(!station_catalog_save(
        &world->stations[0], 0, TMP("test_station_catalog_zero_id_save")));
    world->stations[0].id = first_id;

    memcpy(world->stations[1].station_actor_id, saved_actor,
           sizeof(saved_actor));
    ASSERT(!station_catalog_save_all(
        world->stations, MAX_STATIONS,
        TMP("test_station_catalog_duplicate_actor_save")));

    memcpy(world->stations[1].station_actor_id, second_actor,
           sizeof(second_actor));
    world->stations[1].id = world->stations[0].id;
    ASSERT(!station_catalog_save_all(
        world->stations, MAX_STATIONS,
        TMP("test_station_catalog_duplicate_id_save")));
}

TEST(test_station_catalog_v8_zero_actor_is_hard_transactional_failure) {
    const char *dir = TMP("test_station_catalog_zero_actor_load");
    WORLD_HEAP source = calloc(1, sizeof(world_t));
    WORLD_HEAP destination = calloc(1, sizeof(world_t));
    ASSERT(source != NULL);
    ASSERT(destination != NULL);
    world_reset(source);
    world_reset(destination);
    ASSERT(station_catalog_save_all(
        source->stations, MAX_STATIONS, dir));

    char path[256];
    ASSERT(snprintf(path, sizeof(path), "%s/0.cat", dir) > 0);
    uint8_t zero_actor[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    ASSERT(station_catalog_test_patch_actor(path, zero_actor));

    snprintf(destination->stations[0].name,
             sizeof(destination->stations[0].name), "fallback sentinel");
    uint8_t before_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(before_actor, destination->stations[0].station_actor_id,
           sizeof(before_actor));

    ASSERT_EQ_INT(station_catalog_load_all(
        destination->stations, MAX_STATIONS, dir), -1);
    ASSERT_STR_EQ(destination->stations[0].name, "fallback sentinel");
    ASSERT(memcmp(destination->stations[0].station_actor_id,
                  before_actor, sizeof(before_actor)) == 0);
}

TEST(test_station_catalog_v8_duplicate_id_is_hard_transactional_failure) {
    const char *dir = TMP("test_station_catalog_duplicate_id_load");
    WORLD_HEAP source = calloc(1, sizeof(world_t));
    WORLD_HEAP destination = calloc(1, sizeof(world_t));
    ASSERT(source != NULL);
    ASSERT(destination != NULL);
    world_reset(source);
    world_reset(destination);
    ASSERT(station_catalog_save_all(
        source->stations, MAX_STATIONS, dir));

    char path[256];
    ASSERT(snprintf(path, sizeof(path), "%s/1.cat", dir) > 0);
    uint32_t duplicate_id = source->stations[0].id;
    ASSERT(station_catalog_test_patch(
        path, TEST_CATALOG_ID_OFFSET,
        &duplicate_id, sizeof(duplicate_id)));

    snprintf(destination->stations[1].name,
             sizeof(destination->stations[1].name), "fallback sentinel");
    ASSERT_EQ_INT(station_catalog_load_all(
        destination->stations, MAX_STATIONS, dir), -1);
    ASSERT_STR_EQ(destination->stations[1].name, "fallback sentinel");
}

TEST(test_station_catalog_v8_duplicate_actor_is_hard_failure) {
    const char *dir = TMP("test_station_catalog_duplicate_actor_load");
    WORLD_HEAP source = calloc(1, sizeof(world_t));
    WORLD_HEAP destination = calloc(1, sizeof(world_t));
    ASSERT(source != NULL);
    ASSERT(destination != NULL);
    world_reset(source);
    world_reset(destination);
    ASSERT(station_catalog_save_all(
        source->stations, MAX_STATIONS, dir));

    char path[256];
    ASSERT(snprintf(path, sizeof(path), "%s/1.cat", dir) > 0);
    ASSERT(station_catalog_test_patch_actor(
        path, source->stations[0].station_actor_id));
    ASSERT_EQ_INT(station_catalog_load_all(
        destination->stations, MAX_STATIONS, dir), -1);
}

TEST(test_station_actor_fill_clears_stale_identity_from_empty_slots) {
    WORLD_HEAP world = calloc(1, sizeof(world_t));
    ASSERT(world != NULL);
    world_reset(world);

    station_t *empty = &world->stations[MAX_STATIONS - 1];
    ASSERT(!station_exists(empty));
    empty->id = UINT32_C(0x7ffffffe);
    memset(empty->station_actor_id, 0xA5,
           sizeof(empty->station_actor_id));

    ASSERT(!world_validate_station_actor_ids(world));
    ASSERT(world_ensure_station_actor_ids(world));
    ASSERT_EQ_INT((int)empty->id, 0);
    ASSERT(memcmp(empty->station_actor_id,
                  (const uint8_t[ACTOR_PRINCIPAL_ID_SIZE]){0},
                  sizeof(empty->station_actor_id)) == 0);
    ASSERT(world_validate_station_actor_ids(world));
}

TEST(test_station_actor_rejects_occupied_slot_without_signing_identity) {
    WORLD_HEAP world = calloc(1, sizeof(world_t));
    ASSERT(world != NULL);
    world_reset(world);

    station_t *station =
        &world->stations[MAX_STATIONS - 1];
    station->planned = true;
    station->id = world->next_station_id;
    memset(station->station_actor_id, 0x6C,
           sizeof(station->station_actor_id));
    uint32_t before_next_id = world->next_station_id;

    ASSERT(!world_validate_station_actor_ids(world));
    ASSERT(!world_ensure_station_actor_ids(world));
    ASSERT_EQ_INT((int)world->next_station_id,
                  (int)before_next_id);
    ASSERT_EQ_INT((int)station->id, (int)before_next_id);
}

TEST(test_station_actor_legacy_migration_is_deterministic_and_idempotent) {
    WORLD_HEAP world = calloc(1, sizeof(world_t));
    ASSERT(world != NULL);
    world_reset(world);

    uint8_t bootstrap_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(bootstrap_actor, world->stations[0].station_actor_id,
           sizeof(bootstrap_actor));

    ASSERT(world_migrate_legacy_station_actor_ids(world));
    ASSERT(world_validate_station_actor_ids(world));
    ASSERT(memcmp(bootstrap_actor, world->stations[0].station_actor_id,
                  sizeof(bootstrap_actor)) != 0);

    uint8_t migrated_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(migrated_actor, world->stations[0].station_actor_id,
           sizeof(migrated_actor));
    uint32_t migrated_id = world->stations[0].id;
    uint32_t migrated_next_id = world->next_station_id;
    ASSERT(world_migrate_legacy_station_actor_ids(world));
    ASSERT_EQ_INT((int)world->stations[0].id, (int)migrated_id);
    ASSERT_EQ_INT((int)world->next_station_id, (int)migrated_next_id);
    ASSERT(memcmp(migrated_actor, world->stations[0].station_actor_id,
                  sizeof(migrated_actor)) == 0);
}

TEST(test_station_actor_legacy_migration_rejects_attested_mismatch_transactionally) {
    WORLD_HEAP world = calloc(1, sizeof(world_t));
    ASSERT(world != NULL);
    world_reset(world);

    world->stations[0].station_actor_catalog_attested = true;
    uint8_t before_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(before_actor, world->stations[0].station_actor_id,
           sizeof(before_actor));
    uint32_t before_id = world->stations[0].id;
    uint32_t before_next_id = world->next_station_id;

    ASSERT(!world_migrate_legacy_station_actor_ids(world));
    ASSERT_EQ_INT((int)world->stations[0].id, (int)before_id);
    ASSERT_EQ_INT((int)world->next_station_id, (int)before_next_id);
    ASSERT(memcmp(before_actor, world->stations[0].station_actor_id,
                  sizeof(before_actor)) == 0);
}

TEST(test_v79_world_rejects_mismatched_attested_catalog_transactionally) {
    const char *world_path =
        TMP("test_catalog_world_mismatch.sav");
    const char *catalog_dir =
        TMP("test_catalog_world_mismatch_catalog");
    WORLD_HEAP source = calloc(1, sizeof(world_t));
    WORLD_HEAP other = calloc(1, sizeof(world_t));
    WORLD_HEAP destination = calloc(1, sizeof(world_t));
    ASSERT(source != NULL);
    ASSERT(other != NULL);
    ASSERT(destination != NULL);

    source->rng = 0x11111111u;
    other->rng = 0x22222222u;
    world_reset(source);
    world_reset(other);
    ASSERT(world_save(source, world_path));
    ASSERT(station_catalog_save_all(
        other->stations, MAX_STATIONS, catalog_dir));
    ASSERT_EQ_INT(station_catalog_load_all(
        destination->stations, MAX_STATIONS, catalog_dir),
        SIGNAL_SEEDED_STATION_COUNT);

    uint8_t before_actor[ACTOR_PRINCIPAL_ID_SIZE];
    memcpy(before_actor,
           destination->stations[0].station_actor_id,
           sizeof(before_actor));
    char before_name[sizeof(destination->stations[0].name)];
    memcpy(before_name, destination->stations[0].name,
           sizeof(before_name));

    ASSERT(!world_load(destination, world_path));
    ASSERT(memcmp(
        destination->stations[0].station_actor_id,
        before_actor, sizeof(before_actor)) == 0);
    ASSERT(memcmp(destination->stations[0].name,
                  before_name, sizeof(before_name)) == 0);

    remove(world_path);
}

void register_station_catalog_tests(void) {
    TEST_SECTION("\nStation catalog actor identity tests:\n");
    RUN(test_station_catalog_v8_round_trips_immutable_actor_identity);
    RUN(test_station_catalog_v7_does_not_attest_bootstrap_actor);
    RUN(test_station_catalog_v8_save_rejects_ambiguous_identity_sets);
    RUN(test_station_catalog_v8_zero_actor_is_hard_transactional_failure);
    RUN(test_station_catalog_v8_duplicate_id_is_hard_transactional_failure);
    RUN(test_station_catalog_v8_duplicate_actor_is_hard_failure);
    RUN(test_station_actor_fill_clears_stale_identity_from_empty_slots);
    RUN(test_station_actor_rejects_occupied_slot_without_signing_identity);
    RUN(test_station_actor_legacy_migration_is_deterministic_and_idempotent);
    RUN(test_station_actor_legacy_migration_rejects_attested_mismatch_transactionally);
    RUN(test_v79_world_rejects_mismatched_attested_catalog_transactionally);
}
