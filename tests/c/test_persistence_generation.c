#include "test_harness.h"

#include "cargo_receipt_issue.h"
#include "persistence_generation.h"
#include "persistence_writer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define TEST_MKDIR(path) _mkdir(path)
#define TEST_RMDIR(path) _rmdir(path)
#else
#include <unistd.h>
#define TEST_MKDIR(path) mkdir((path), 0700)
#define TEST_RMDIR(path) rmdir(path)
#endif

enum {
    TEST_POINTER_PAYLOAD_SIZE = 92,
    TEST_POINTER_FILE_SIZE = 124,
};

static bool test_path_join(char *out, size_t out_size,
                           const char *left, const char *right) {
    if (!out || !left || !right || out_size == 0) return false;
    int n = snprintf(out, out_size, "%s/%s", left, right);
    return n > 0 && (size_t)n < out_size;
}

static bool test_generation_dir_path(
    char *out, size_t out_size, const char *root, uint64_t generation) {
    if (!out || !root || generation == 0) return false;
    int n = snprintf(out, out_size, "%s/generation-%020llu", root,
                     (unsigned long long)generation);
    return n > 0 && (size_t)n < out_size;
}

static bool test_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool test_prepare_generation_world(
    world_t *world,
    const uint8_t token[8],
    float world_time,
    float player_hull,
    float credits) {
    if (!world || !token) return false;
    world_reset(world);
    world->time = world_time;
    server_player_t *player = &world->players[0];
    player->connected = true;
    player->session_ready = true;
    memcpy(player->session_token, token, 8);
    player_init_ship(player, world);
    if (!player->ship) return false;
    player->ship->hull = player_hull;
    ledger_earn(&world->stations[0], token, credits);
    return true;
}

static bool test_set_exact_generation_payload(
    world_t *world,
    uint8_t tag,
    uint64_t receipt_event_id,
    uint64_t chain_event_count,
    float player_hull) {
    if (!world || !world->players[0].ship) return false;
    station_t *station = &world->stations[0];
    while (station->manifest.count > 0) {
        if (!station_manifest_remove_with_chain(
                station, (uint16_t)(station->manifest.count - 1u),
                NULL, NULL)) {
            return false;
        }
    }

    cargo_unit_t unit = {0};
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_RARE;
    unit.quantity = 1;
    unit.recipe_id = (uint16_t)RECIPE_SMELT;
    for (size_t i = 0; i < sizeof(unit.pub); i++)
        unit.pub[i] = (uint8_t)(tag + i);

    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (size_t i = 0; i < sizeof(recipient); i++) {
        recipient[i] = (uint8_t)(0x40u + tag + i);
        origin_pin[i] = (uint8_t)(0x90u + tag + i);
    }
    cargo_receipt_chain_t chain = {0};
    if (!cargo_receipt_issue(
            station, 1, receipt_event_id, unit.pub,
            recipient, origin_pin, &chain.links[0])) {
        return false;
    }
    chain.len = 1;
    if (!station_manifest_push_with_chain(station, &unit, &chain))
        return false;

    station->chain_event_count = chain_event_count;
    for (size_t i = 0; i < sizeof(station->chain_last_hash); i++)
        station->chain_last_hash[i] = (uint8_t)(0xc0u + tag + i);
    world->players[0].ship->hull = player_hull;
    return true;
}

static bool test_load_generation_world(
    const persistence_generation_paths_t *paths,
    world_t *loaded) {
    if (!paths || !loaded) return false;
    world_reset(loaded);
    return station_catalog_load_all(
               loaded->stations, MAX_STATIONS,
               paths->catalog_dir) >= 0 &&
           world_load(loaded, paths->world_path);
}

static bool test_load_generation_player(
    const persistence_generation_paths_t *paths,
    world_t *loaded,
    const uint8_t token[8]) {
    if (!paths || !loaded || !token) return false;
    server_player_t *player = &loaded->players[0];
    player->connected = true;
    player->session_ready = true;
    memcpy(player->session_token, token, 8);
    if (!player->ship &&
        !world_player_ship_slot_activate(loaded, 0)) {
        return false;
    }
    return player_load_by_token(
        player, loaded, paths->player_dir, token);
}

static bool test_generation_payload_matches(
    const persistence_generation_paths_t *paths,
    world_t *loaded,
    const uint8_t token[8],
    uint8_t tag,
    uint64_t receipt_event_id,
    uint64_t chain_event_count,
    float player_hull) {
    if (!test_load_generation_world(paths, loaded) ||
        !test_load_generation_player(paths, loaded, token)) {
        return false;
    }
    const station_t *station = &loaded->stations[0];
    if (station->manifest.count != 1u ||
        !station->manifest.units ||
        station->manifest.units[0].commodity !=
            (uint8_t)COMMODITY_FERRITE_INGOT ||
        station->manifest.units[0].quantity != 1u ||
        station->chain_event_count != chain_event_count ||
        !loaded->players[0].ship ||
        fabsf(loaded->players[0].ship->hull - player_hull) > 0.001f) {
        return false;
    }
    for (size_t i = 0;
         i < sizeof(station->manifest.units[0].pub); i++) {
        if (station->manifest.units[0].pub[i] !=
            (uint8_t)(tag + i)) {
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(station->chain_last_hash); i++) {
        if (station->chain_last_hash[i] !=
            (uint8_t)(0xc0u + tag + i)) {
            return false;
        }
    }
    const ship_receipts_t *receipts =
        station_get_receipts_const(station);
    return receipts && receipts->count == 1u &&
           receipts->chains[0].len == 1u &&
           receipts->chains[0].links[0].event_id ==
               receipt_event_id &&
           cargo_receipt_chain_verify(
               receipts->chains[0].links,
               receipts->chains[0].len,
               station->manifest.units[0].pub) ==
               CARGO_RECEIPT_OK;
}

static bool test_read_file(const char *path,
                           uint8_t **bytes_out,
                           size_t *size_out) {
    if (!path || !bytes_out || !size_out) return false;
    *bytes_out = NULL;
    *size_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long end = ftell(f);
    if (end <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint8_t *bytes = malloc((size_t)end);
    if (!bytes) {
        fclose(f);
        return false;
    }
    bool ok = fread(bytes, 1, (size_t)end, f) == (size_t)end &&
              fclose(f) == 0;
    if (!ok) {
        free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *size_out = (size_t)end;
    return true;
}

static bool test_write_file(const char *path,
                            const uint8_t *bytes,
                            size_t size) {
    if (!path || !bytes || size == 0) return false;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(bytes, 1, size, f) == size &&
              fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool test_file_sha256(const char *path, uint8_t digest[32]) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!test_read_file(path, &bytes, &size)) return false;
    sha256_bytes(bytes, size, digest);
    free(bytes);
    return true;
}

static bool test_corrupt_file_byte(const char *path) {
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
    uint8_t byte = 0;
    bool ok = fseek(f, 16, SEEK_SET) == 0 &&
              fread(&byte, 1, 1, f) == 1 &&
              fseek(f, 16, SEEK_SET) == 0;
    byte ^= 0x5au;
    if (ok) ok = fwrite(&byte, 1, 1, f) == 1 && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/*
 * Keep CURRENT cryptographically self-consistent while placing an embedded
 * NUL in the first declared manifest path.  This reaches the parser's path
 * canonicalization rather than being rejected by the outer digest.
 */
static bool test_inject_manifest_embedded_nul(
    const char *root,
    const persistence_generation_paths_t *paths) {
    char generation_dir[PERSISTENCE_GENERATION_PATH_MAX];
    int n = snprintf(generation_dir, sizeof(generation_dir),
                     "%s/generation-%020llu", root,
                     (unsigned long long)paths->generation);
    if (n <= 0 || (size_t)n >= sizeof(generation_dir)) return false;
    char manifest_path[PERSISTENCE_GENERATION_PATH_MAX];
    char pointer_path[PERSISTENCE_GENERATION_PATH_MAX];
    if (!test_path_join(manifest_path, sizeof(manifest_path),
                        generation_dir, "MANIFEST") ||
        !test_path_join(pointer_path, sizeof(pointer_path),
                        root, "CURRENT")) {
        return false;
    }

    uint8_t *manifest = NULL;
    size_t manifest_size = 0;
    if (!test_read_file(
            manifest_path, &manifest, &manifest_size) ||
        manifest_size < 68u) {
        free(manifest);
        return false;
    }
    uint16_t first_path_len =
        (uint16_t)manifest[24] |
        ((uint16_t)manifest[25] << 8);
    size_t first_path = 24u + 2u + 8u + 32u;
    if (first_path_len < 2u ||
        first_path + first_path_len > manifest_size) {
        free(manifest);
        return false;
    }
    manifest[first_path + 1u] = '\0';
    bool ok = test_write_file(
        manifest_path, manifest, manifest_size);
    free(manifest);
    if (!ok) return false;

    uint8_t pointer[TEST_POINTER_FILE_SIZE];
    FILE *f = fopen(pointer_path, "rb");
    if (!f) return false;
    ok = fread(pointer, 1, sizeof(pointer), f) == sizeof(pointer) &&
         fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) return false;

    uint8_t manifest_digest[32];
    if (!test_file_sha256(manifest_path, manifest_digest))
        return false;
    memcpy(pointer + 20, manifest_digest, 32);
    sha256_bytes(pointer, TEST_POINTER_PAYLOAD_SIZE,
                 pointer + TEST_POINTER_PAYLOAD_SIZE);
    return test_write_file(pointer_path, pointer, sizeof(pointer));
}

TEST(test_persistence_generation_publish_boundaries_and_player_carry) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_publish_boundaries") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_publish_legacy") > 0);

    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_NONE);

    static const uint8_t token[8] =
        {0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 10.0f, 41.0f, 125.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;

    persistence_generation_paths_t published = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &published));
    ASSERT(published.generation == 1u);
    char runtime_player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(runtime_player_dir, sizeof(runtime_player_dir), "%s",
                    published.player_dir) > 0);

    world->time = 20.0f;
    world->players[0].ship->hull = 77.0f;
    ledger_earn(&world->stations[0], token, 25.0f);
    static const persistence_generation_fault_t pre_publish_faults[] = {
        PERSISTENCE_GENERATION_FAULT_AFTER_ARTIFACTS,
        PERSISTENCE_GENERATION_FAULT_AFTER_MANIFEST,
        PERSISTENCE_GENERATION_FAULT_BEFORE_POINTER_PUBLISH,
    };
    for (size_t i = 0;
         i < sizeof(pre_publish_faults) / sizeof(pre_publish_faults[0]);
         i++) {
        persistence_generation_paths_t failed = {0};
        ASSERT(!persistence_generation_commit(
            root, legacy, world, save_slots,
            pre_publish_faults[i], &failed));
        ASSERT(failed.generation == 0);
        ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                      PERSISTENCE_GENERATION_CURRENT);
        ASSERT(selected.generation == 1u);
        ASSERT_STR_EQ(runtime_player_dir, selected.player_dir);
    }

    /* The three unpublished directories consume IDs 2..4.  A subsequent
     * commit must skip them, carry the old player save while slot 0 is
     * inactive, and publish generation 5 with generation 1 as its parent. */
    save_slots[0] = false;
    persistence_generation_paths_t carried = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &carried));
    ASSERT(carried.generation == 5u);
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_CURRENT);
    ASSERT(selected.generation == carried.generation);

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    ASSERT(test_load_generation_world(&selected, loaded));
    ASSERT_EQ_FLOAT(loaded->time, 20.0f, 0.001f);
    ASSERT_EQ_FLOAT(
        ledger_balance(&loaded->stations[0], token),
        150.0f, 0.001f);
    ASSERT(test_load_generation_player(&selected, loaded, token));
    ASSERT_EQ_FLOAT(loaded->players[0].ship->hull, 41.0f, 0.001f);

    /* A rename-success / directory-fsync-failure must adopt visible CURRENT,
     * so runtime reconnects do not keep reading generation 5. */
    save_slots[0] = true;
    persistence_generation_paths_t ambiguous = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_POINTER_DIR_SYNC_FAILURE,
        &ambiguous));
    ASSERT(ambiguous.generation == 6u);
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_CURRENT);
    ASSERT(selected.generation == ambiguous.generation);
    ASSERT_STR_EQ(selected.player_dir, ambiguous.player_dir);
}

TEST(test_persistence_generation_prunes_bounded_history_after_publish) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_bounded_history") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_bounded_history_legacy") > 0);
    static const uint8_t token[8] =
        {0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 1.0f, 40.0f, 10.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;

    persistence_generation_paths_t published = {0};
    for (uint64_t generation = 1; generation <= 12u; generation++) {
        world->time = (float)generation;
        ASSERT(persistence_generation_commit(
            root, legacy, world, save_slots,
            PERSISTENCE_GENERATION_FAULT_NONE, &published));
        ASSERT(published.generation == generation);
    }

    for (uint64_t generation = 1; generation <= 12u; generation++) {
        char path[PERSISTENCE_GENERATION_PATH_MAX];
        ASSERT(test_generation_dir_path(
            path, sizeof(path), root, generation));
        ASSERT_EQ_INT(test_path_exists(path), generation >= 5u);
    }

    ASSERT(test_corrupt_file_byte(published.world_path));
    persistence_generation_paths_t fallback = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &fallback),
                  PERSISTENCE_GENERATION_PREVIOUS);
    ASSERT(fallback.generation == 11u);
}

TEST(test_persistence_generation_recovery_follows_published_lineage) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_lineage") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_lineage_legacy") > 0);
    static const uint8_t token[8] =
        {0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 1.0f, 30.0f, 10.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    ASSERT(test_set_exact_generation_payload(
        world, 0x11u, 1101u, 11u, 31.0f));
    persistence_generation_paths_t first = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &first));

    world->time = 2.0f;
    ASSERT(test_set_exact_generation_payload(
        world, 0x22u, 2202u, 22u, 62.0f));
    persistence_generation_paths_t second = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &second));
    ASSERT(second.generation == first.generation + 1u);

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    ASSERT(test_generation_payload_matches(
        &second, loaded, token,
        0x22u, 2202u, 22u, 62.0f));
    world_cleanup(loaded);
    memset(loaded, 0, sizeof(*loaded));

    world->time = 3.0f;
    ASSERT(test_set_exact_generation_payload(
        world, 0x33u, 3303u, 33u, 93.0f));
    persistence_generation_paths_t unpublished = {0};
    ASSERT(!persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_AFTER_MANIFEST,
        &unpublished));
    ASSERT(test_corrupt_file_byte(second.world_path));

    persistence_generation_paths_t recovered = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &recovered),
                  PERSISTENCE_GENERATION_PREVIOUS);
    ASSERT(recovered.generation == first.generation);
    ASSERT(test_generation_payload_matches(
        &recovered, loaded, token,
        0x11u, 1101u, 11u, 31.0f));
    ASSERT_EQ_FLOAT(loaded->time, 1.0f, 0.001f);

    /*
     * Heal from the authenticated previous lineage. Generation 3 remains
     * unpublished debris, so the next commit is generation 4 and pins
     * generation 1 as its predecessor. The in-memory exact state becomes the
     * new current generation as one unit.
     */
    world_cleanup(loaded);
    memset(loaded, 0, sizeof(*loaded));
    persistence_generation_paths_t healed = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &healed));
    ASSERT(healed.generation == 4u);
    ASSERT(test_generation_payload_matches(
        &healed, loaded, token,
        0x33u, 3303u, 33u, 93.0f));
    ASSERT_EQ_FLOAT(loaded->time, 3.0f, 0.001f);
}

TEST(test_persistence_generation_rejects_missing_namespace_directory) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_missing_namespace") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_missing_namespace_legacy") > 0);
    static const uint8_t token[8] = {1, 3, 5, 7, 9, 11, 13, 15};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 4.0f, 50.0f, 20.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    persistence_generation_paths_t published = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &published));

    char pubkey_dir[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(test_path_join(pubkey_dir, sizeof(pubkey_dir),
                          published.player_dir, "pubkey"));
    ASSERT(TEST_RMDIR(pubkey_dir) == 0);
    persistence_generation_paths_t rejected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &rejected),
                  PERSISTENCE_GENERATION_INVALID);
}

TEST(test_persistence_generation_rejects_embedded_nul_manifest_path) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_manifest_nul") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_manifest_nul_legacy") > 0);
    static const uint8_t token[8] = {2, 4, 6, 8, 10, 12, 14, 16};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 5.0f, 60.0f, 30.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    persistence_generation_paths_t published = {0};
    ASSERT(persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &published));
    ASSERT(test_inject_manifest_embedded_nul(root, &published));

    persistence_generation_paths_t rejected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &rejected),
                  PERSISTENCE_GENERATION_INVALID);
}

#ifndef _WIN32
TEST(test_persistence_generation_rejects_symlinked_legacy_namespace) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    char target[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_symlink_root") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_symlink_legacy") > 0);
    ASSERT(snprintf(target, sizeof(target), "%s/%s",
                    test_tmp_dir(), "generation_symlink_target") > 0);
    ASSERT((TEST_MKDIR(legacy) == 0 || errno == EEXIST) &&
           (TEST_MKDIR(target) == 0 || errno == EEXIST));
    char link_path[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(test_path_join(link_path, sizeof(link_path), legacy, "pubkey"));
    ASSERT(symlink(target, link_path) == 0);

    static const uint8_t token[8] = {17, 18, 19, 20, 21, 22, 23, 24};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 6.0f, 70.0f, 40.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    persistence_generation_paths_t failed = {0};
    ASSERT(!persistence_generation_commit(
        root, legacy, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &failed));
    ASSERT_EQ_INT(persistence_generation_resolve(root, &failed),
                  PERSISTENCE_GENERATION_NONE);
}
#endif

TEST(test_persistence_writer_commits_immutable_snapshot) {
    char root[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy[PERSISTENCE_GENERATION_PATH_MAX];
    ASSERT(snprintf(root, sizeof(root), "%s/%s",
                    test_tmp_dir(), "generation_async_snapshot") > 0);
    ASSERT(snprintf(legacy, sizeof(legacy), "%s/%s",
                    test_tmp_dir(), "generation_async_legacy") > 0);
    static const uint8_t token[8] =
        {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38};
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    ASSERT(test_prepare_generation_world(
        world, token, 12.0f, 44.0f, 75.0f));
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;

    persistence_writer_t *writer = persistence_writer_create();
    ASSERT(writer != NULL);
    ASSERT(persistence_writer_start(
        writer, root, legacy, world, save_slots));
    ASSERT(!persistence_writer_start(
        writer, root, legacy, world, save_slots));

    /* These changes happen after snapshot capture and must not leak into the
     * generation being serialized by the worker. */
    world->time = 99.0f;
    world->players[0].ship->hull = 9.0f;
    persistence_generation_paths_t published = {0};
    ASSERT_EQ_INT(
        persistence_writer_wait(writer, &published),
        PERSISTENCE_WRITER_SUCCEEDED);
    ASSERT(!persistence_writer_active(writer));
    persistence_writer_destroy(writer);

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    ASSERT(test_load_generation_world(&published, loaded));
    ASSERT_EQ_FLOAT(loaded->time, 12.0f, 0.001f);
    ASSERT(test_load_generation_player(&published, loaded, token));
    ASSERT_EQ_FLOAT(loaded->players[0].ship->hull, 44.0f, 0.001f);
}

void register_persistence_generation_tests(void) {
    RUN(test_persistence_writer_commits_immutable_snapshot);
    RUN(test_persistence_generation_publish_boundaries_and_player_carry);
    RUN(test_persistence_generation_prunes_bounded_history_after_publish);
    RUN(test_persistence_generation_recovery_follows_published_lineage);
    RUN(test_persistence_generation_rejects_missing_namespace_directory);
    RUN(test_persistence_generation_rejects_embedded_nul_manifest_path);
#ifndef _WIN32
    RUN(test_persistence_generation_rejects_symlinked_legacy_namespace);
#endif
}
