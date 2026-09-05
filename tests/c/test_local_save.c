#include "test_harness.h"
#include "local_save.h"
#include "persistence_generation.h"

static const uint8_t local_test_pubkey[32] = {19, 7, 11};

static bool local_test_authenticate(world_t *world, int slot, uint8_t token) {
    server_player_t *player = &world->players[slot];
    player->id = (uint8_t)slot;
    player->connected = true;
    player_init_ship(player, world);
    player->session_ready = true;
    memset(player->session_token, token, 8);
    memcpy(player->pubkey, local_test_pubkey, 32);
    player->pubkey_set = true;
    player->pubkey_proof_ok = true;
    player->pubkey_challenge_consumed = true;
    return server_finalize_pubkey_identity(world, slot);
}

TEST(test_local_save_restart_restores_world_player_and_currency) {
    WORLD_DECL;
    w.rng = 1234;
    world_reset(&w);
    uint32_t seed = w.belt_seed;
    bool fresh = false;
    const char *root = TMP("local-restart");
    local_save_t *save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    ASSERT(fresh);
    world_seed_station_manifests(&w);
    ASSERT(local_test_authenticate(&w, 0, 3));
    ASSERT(local_save_restore_player(save, &w, 0));
    player_seed_credits(&w.players[0], &w);
    w.time = 123.0f;
    w.players[0].ship->hull = 42.0f;
    ledger_earn_by_pubkey(&w.stations[0], local_test_pubkey, 200.0f);
    float balance = ledger_balance_by_pubkey(&w.stations[0], local_test_pubkey);
    ASSERT(local_save_request(save, &w, true));
    ASSERT(local_save_generation(save) > 0);
    local_save_close(save, NULL);

    world_reset(&w);
    save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    ASSERT(!fresh);
    ASSERT_EQ_INT(w.belt_seed, seed);
    ASSERT_EQ_FLOAT(w.time, 123.0f, 0.01f);
    ASSERT(local_test_authenticate(&w, 0, 9));
    ASSERT(local_save_restore_player(save, &w, 0));
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, 42.0f, 0.01f);
    player_seed_credits(&w.players[0], &w);
    ASSERT_EQ_FLOAT(ledger_balance_by_pubkey(&w.stations[0], local_test_pubkey), balance, 0.01f);
    ASSERT(strcmp(w.stations[0].currency_name, "prospect vouchers") == 0);
    local_save_close(save, &w);
}

TEST(test_local_save_rejects_overlapping_owner_and_wrong_player) {
    WORLD_DECL;
    world_reset(&w);
    bool fresh;
    const char *root = TMP("local-exclusive");
    local_save_t *save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    local_save_t *second = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(second == NULL);
    ASSERT(local_test_authenticate(&w, 0, 4));
    w.players[0].pubkey[0] ^= 1;
    ASSERT(!local_save_restore_player(save, &w, 0));
    ASSERT(!local_save_request(save, &w, true));
    local_save_close(save, NULL);
    save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    ASSERT(fresh);
    local_save_close(save, NULL);
}

TEST(test_local_save_recovers_previous_complete_generation) {
    WORLD_DECL;
    world_reset(&w);
    bool fresh;
    const char *root = TMP("local-previous");
    local_save_t *save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    ASSERT(local_test_authenticate(&w, 0, 5));
    ASSERT(local_save_restore_player(save, &w, 0));
    w.time = 10.0f;
    w.players[0].ship->hull = 33.0f;
    ASSERT(local_save_request(save, &w, true));
    w.time = 20.0f;
    w.players[0].ship->hull = 66.0f;
    ASSERT(local_save_request(save, &w, true));
    persistence_generation_paths_t selected;
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected), PERSISTENCE_GENERATION_CURRENT);
    local_save_close(save, NULL);
    FILE *broken = fopen(selected.world_path, "wb");
    ASSERT(broken != NULL);
    ASSERT(fputs("interrupted", broken) >= 0);
    ASSERT_EQ_INT(fclose(broken), 0);
    world_reset(&w);
    save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    ASSERT(!fresh);
    ASSERT_EQ_FLOAT(w.time, 10.0f, 0.01f);
    ASSERT(local_test_authenticate(&w, 0, 6));
    ASSERT(local_save_restore_player(save, &w, 0));
    ASSERT_EQ_FLOAT(w.players[0].ship->hull, 33.0f, 0.01f);
    local_save_close(save, NULL);
}

TEST(test_local_save_keeps_invalid_generation_for_recovery) {
    WORLD_DECL;
    world_reset(&w);
    bool fresh;
    const char *root = TMP("local-invalid");
    local_save_t *save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save != NULL);
    ASSERT(local_test_authenticate(&w, 0, 7));
    ASSERT(local_save_restore_player(save, &w, 0));
    ASSERT(local_save_request(save, &w, true));
    persistence_generation_paths_t selected;
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected), PERSISTENCE_GENERATION_CURRENT);
    local_save_close(save, NULL);
    FILE *broken = fopen(selected.world_path, "wb");
    ASSERT(broken != NULL);
    ASSERT(fputs("recovery-needed", broken) >= 0);
    ASSERT_EQ_INT(fclose(broken), 0);
    save = local_save_open(root, &w, local_test_pubkey, &fresh);
    ASSERT(save == NULL);
    ASSERT(!fresh);
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected), PERSISTENCE_GENERATION_INVALID);
}

void register_local_save_tests(void) {
    RUN(test_local_save_restart_restores_world_player_and_currency);
    RUN(test_local_save_rejects_overlapping_owner_and_wrong_player);
    RUN(test_local_save_recovers_previous_complete_generation);
    RUN(test_local_save_keeps_invalid_generation_for_recovery);
}
