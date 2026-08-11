#include "test_harness.h"

#include "public_actor_resolver.h"
#include "sim_ai.h"

static void resolver_stable_id(
    uint8_t out[ACTOR_PRINCIPAL_ID_SIZE],
    uint8_t seed) {
    memset(out, seed, ACTOR_PRINCIPAL_ID_SIZE);
    out[0] = (uint8_t)(seed ^ 0x5au);
    out[ACTOR_PRINCIPAL_ID_SIZE - 1] =
        (uint8_t)(seed ^ 0xa5u);
}

static void resolver_make_verified_player(
    server_player_t *player,
    uint8_t pubkey_seed,
    uint8_t token_seed,
    bool connected,
    bool grace) {
    memset(player, 0, sizeof(*player));
    player->connected = connected;
    player->grace_period = grace;
    player->session_ready = true;
    memset(player->session_token, token_seed,
           sizeof(player->session_token));
    resolver_stable_id(player->pubkey, pubkey_seed);
    player->pubkey_set = true;
    player->pubkey_proof_ok = true;
    player->pubkey_challenge_consumed = true;
    player->pubkey_identity_finalized = true;
}

static bool resolver_principal_is_none(
    const actor_principal_t *principal) {
    actor_principal_t none = actor_principal_none();
    return actor_principal_equal(principal, &none);
}

TEST(test_public_actor_verified_player_survives_token_rotation) {
    WORLD_DECL;
    server_player_t *player = &w.players[3];
    resolver_make_verified_player(
        player, 0x21, 0x31, true, false);

    public_actor_id_t before = public_actor_id_none();
    public_actor_id_t after = public_actor_id_none();
    ASSERT(public_actor_id_from_verified_player(player, &before));

    memset(player->session_token, 0x91,
           sizeof(player->session_token));
    ASSERT(public_actor_id_from_verified_player(player, &after));
    ASSERT(public_actor_id_equal(&before, &after));

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &before);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 3);
    ASSERT_EQ_INT(result.principal.kind, ACTOR_PRINCIPAL_PLAYER);
    ASSERT(memcmp(result.principal.id, player->pubkey,
                  sizeof(player->pubkey)) == 0);

    player->grace_period = true;
    result = world_resolve_public_actor_id(&w, &before);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_GRACE);
    ASSERT_EQ_INT(result.slot, 3);

    player->connected = false;
    result = world_resolve_public_actor_id(&w, &before);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);
}

TEST(test_public_actor_tracks_slot_transfer_and_reuse) {
    WORLD_DECL;
    resolver_make_verified_player(
        &w.players[1], 0x22, 0x32, true, false);
    public_actor_id_t transferred = public_actor_id_none();
    ASSERT(public_actor_id_from_verified_player(
        &w.players[1], &transferred));

    resolver_make_verified_player(
        &w.players[7], 0x22, 0xa2, true, false);
    memset(&w.players[1], 0, sizeof(w.players[1]));
    resolver_make_verified_player(
        &w.players[1], 0x23, 0x33, true, false);

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &transferred);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 7);
    ASSERT(memcmp(result.principal.id, w.players[7].pubkey,
                  sizeof(w.players[7].pubkey)) == 0);

    public_actor_id_t reused = public_actor_id_none();
    ASSERT(public_actor_id_from_verified_player(
        &w.players[1], &reused));
    ASSERT(!public_actor_id_equal(&transferred, &reused));
    result = world_resolve_public_actor_id(&w, &reused);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 1);
}

TEST(test_public_actor_resolves_offline_pubkey_registry) {
    WORLD_DECL;
    uint8_t pubkey[ACTOR_PRINCIPAL_ID_SIZE];
    resolver_stable_id(pubkey, 0x24);
    w.pubkey_registry[5].in_use = true;
    memcpy(w.pubkey_registry[5].pubkey, pubkey, sizeof(pubkey));
    memset(w.pubkey_registry[5].session_token, 0x34,
           sizeof(w.pubkey_registry[5].session_token));

    actor_principal_t principal = actor_principal_none();
    public_actor_id_t actor = public_actor_id_none();
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, pubkey, &principal));
    ASSERT(public_actor_id_from_principal(&principal, &actor));

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);
    ASSERT(actor_principal_equal(&result.principal, &principal));

    memset(w.pubkey_registry[5].session_token, 0xe4,
           sizeof(w.pubkey_registry[5].session_token));
    result = world_resolve_public_actor_id(&w, &actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT(actor_principal_equal(&result.principal, &principal));
}

TEST(test_public_actor_canonicalizes_duplicate_stale_registry_rows) {
    WORLD_DECL;
    uint8_t pubkey[ACTOR_PRINCIPAL_ID_SIZE];
    resolver_stable_id(pubkey, 0x25);
    for (int row = 0; row < 3; row++) {
        w.pubkey_registry[row].in_use = true;
        memcpy(w.pubkey_registry[row].pubkey,
               pubkey, sizeof(pubkey));
        memset(w.pubkey_registry[row].session_token,
               0x40 + row,
               sizeof(w.pubkey_registry[row].session_token));
    }

    actor_principal_t principal = actor_principal_none();
    public_actor_id_t actor = public_actor_id_none();
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, pubkey, &principal));
    ASSERT(public_actor_id_from_principal(&principal, &actor));

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT(actor_principal_equal(&result.principal, &principal));

    resolver_make_verified_player(
        &w.players[4], 0x25, 0xf5, true, false);
    result = world_resolve_public_actor_id(&w, &actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 4);
    ASSERT(actor_principal_equal(&result.principal, &principal));
}

TEST(test_public_actor_fails_closed_on_duplicate_runtime_actor) {
    WORLD_DECL;
    resolver_make_verified_player(
        &w.players[2], 0x26, 0x36, true, false);
    resolver_make_verified_player(
        &w.players[8], 0x26, 0x96, true, false);
    public_actor_id_t actor = public_actor_id_none();
    ASSERT(public_actor_id_from_verified_player(
        &w.players[2], &actor));

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);
    ASSERT(resolver_principal_is_none(&result.principal));
}

TEST(test_public_actor_npc_bearer_never_projects_or_resolves) {
    WORLD_DECL;
    w.npc_ships[3].active = true;
    memset(w.npc_ships[3].session_token, 0x46,
           sizeof(w.npc_ships[3].session_token));
    public_actor_id_t projected = public_actor_id_unattributed();
    ASSERT(!public_actor_id_from_unique_npc_slot(
        &w, 3, &projected));
    ASSERT_EQ_INT(projected.kind, PUBLIC_ACTOR_ID_NONE);

    memset(w.npc_ships[3].session_token, 0xa6,
           sizeof(w.npc_ships[3].session_token));
    ASSERT(!public_actor_id_from_unique_npc_slot(
        &w, 3, &projected));
    ASSERT_EQ_INT(projected.kind, PUBLIC_ACTOR_ID_NONE);

    actor_principal_t token_principal = actor_principal_none();
    public_actor_id_t token_actor = public_actor_id_none();
    ASSERT(actor_principal_from_npc(
        &w.npc_ships[3], &token_principal));
    ASSERT(!public_actor_id_from_principal(
        &token_principal, &token_actor));
    ASSERT_EQ_INT(token_actor.kind, PUBLIC_ACTOR_ID_NONE);
}

TEST(test_public_actor_verified_token_locator_is_stable_and_fail_closed) {
    WORLD_DECL;
    resolver_make_verified_player(
        &w.players[5], 0x63, 0x73, true, false);
    uint8_t original_token[8];
    memcpy(original_token, w.players[5].session_token,
           sizeof(original_token));

    public_actor_id_t before = public_actor_id_none();
    int slot = -1;
    ASSERT(public_actor_id_from_verified_player_token(
        &w, original_token, &before, &slot));
    ASSERT_EQ_INT(slot, 5);

    memset(w.players[5].session_token, 0xb3,
           sizeof(w.players[5].session_token));
    public_actor_id_t after = public_actor_id_none();
    ASSERT(!public_actor_id_from_verified_player_token(
        &w, original_token, &after, &slot));
    ASSERT_EQ_INT(after.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT_EQ_INT(slot, -1);
    ASSERT(public_actor_id_from_verified_player_token(
        &w, w.players[5].session_token, &after, &slot));
    ASSERT(public_actor_id_equal(&before, &after));
    ASSERT_EQ_INT(slot, 5);

    resolver_make_verified_player(
        &w.players[9], 0x64, 0xb3, true, false);
    ASSERT(!public_actor_id_from_verified_player_token(
        &w, w.players[5].session_token, &after, &slot));
    ASSERT_EQ_INT(after.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT_EQ_INT(slot, -1);
}

TEST(test_public_actor_namespace_separation_and_station_identity) {
    WORLD_DECL;
    uint8_t stable[ACTOR_PRINCIPAL_ID_SIZE];
    resolver_stable_id(stable, 0x27);
    resolver_make_verified_player(
        &w.players[6], 0x27, 0x37, true, false);

    station_t *station = &w.stations[2];
    station->planned = true;
    station->id = 3;
    memcpy(station->station_actor_id, stable, sizeof(stable));
    public_actor_id_t player_actor = public_actor_id_none();
    public_actor_id_t station_actor = public_actor_id_none();
    ASSERT(public_actor_id_from_verified_player(
        &w.players[6], &player_actor));
    ASSERT(public_actor_id_from_station(
        &w, 2, &station_actor));
    ASSERT(!public_actor_id_equal(&player_actor, &station_actor));

    actor_principal_t npc_principal = actor_principal_none();
    public_actor_id_t npc_actor = public_actor_id_none();
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NPC, stable, &npc_principal));
    ASSERT(!public_actor_id_from_principal(
        &npc_principal, &npc_actor));
    ASSERT_EQ_INT(npc_actor.kind, PUBLIC_ACTOR_ID_NONE);

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &player_actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 6);
    ASSERT_EQ_INT(result.principal.kind, ACTOR_PRINCIPAL_PLAYER);
    result = world_resolve_public_actor_id(&w, &station_actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 2);
    ASSERT_EQ_INT(result.principal.kind, ACTOR_PRINCIPAL_STATION);
}

TEST(test_public_actor_station_duplicate_is_not_authority) {
    WORLD_DECL;
    for (int slot = 0; slot < 2; slot++) {
        w.stations[slot].planned = true;
        w.stations[slot].id = (uint32_t)(slot + 1);
        resolver_stable_id(
            w.stations[slot].station_actor_id, 0x28);
    }
    public_actor_id_t actor = public_actor_id_none();
    ASSERT(public_actor_id_from_station(&w, 0, &actor));

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(&w, &actor);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);
    ASSERT(resolver_principal_is_none(&result.principal));
}

TEST(test_public_actor_invalid_and_sentinel_ids_never_resolve) {
    WORLD_DECL;
    resolver_make_verified_player(
        &w.players[0], 0x29, 0x39, true, false);
    public_actor_id_t valid = public_actor_id_none();
    ASSERT(public_actor_id_from_verified_player(
        &w.players[0], &valid));

    public_actor_id_t values[] = {
        public_actor_id_none(),
        public_actor_id_unattributed(),
        public_actor_id_legacy_unattributed(),
        {.kind = PUBLIC_ACTOR_ID_DERIVED, .id = {0}},
        {.kind = UINT8_MAX, .id = {1}},
    };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        public_actor_resolution_result_t result =
            world_resolve_public_actor_id(&w, &values[i]);
        ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
        ASSERT_EQ_INT(result.slot, -1);
        ASSERT(resolver_principal_is_none(&result.principal));
    }

    public_actor_resolution_result_t result =
        world_resolve_public_actor_id(NULL, &valid);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT(resolver_principal_is_none(&result.principal));
    result = world_resolve_public_actor_id(&w, NULL);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT(resolver_principal_is_none(&result.principal));

    server_player_t unproven = {0};
    resolver_stable_id(unproven.pubkey, 0x2a);
    unproven.session_ready = true;
    unproven.pubkey_set = true;
    public_actor_id_t out = public_actor_id_unattributed();
    ASSERT(!public_actor_id_from_verified_player(
        &unproven, &out));
    ASSERT_EQ_INT(out.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT(!public_actor_id_from_verified_player(
        &w.players[0], NULL));
}

TEST(test_public_actor_ledger_projection_never_exposes_legacy_bearer) {
    WORLD_DECL;
    uint8_t legacy_a[ACTOR_PRINCIPAL_ID_SIZE] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    uint8_t legacy_b[ACTOR_PRINCIPAL_ID_SIZE] = {
        0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
    };
    uint8_t zero[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    public_actor_id_t actor_a = public_actor_id_none();
    public_actor_id_t actor_b = public_actor_id_none();
    public_actor_id_t actor_zero = public_actor_id_none();

    ASSERT(public_actor_id_from_ledger_projection(
        &w, legacy_a, &actor_a));
    ASSERT(public_actor_id_from_ledger_projection(
        &w, legacy_b, &actor_b));
    ASSERT(public_actor_id_from_ledger_projection(
        &w, zero, &actor_zero));
    ASSERT_EQ_INT(actor_a.kind, PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED);
    ASSERT_EQ_INT(actor_b.kind, PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED);
    ASSERT_EQ_INT(actor_zero.kind, PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED);
    ASSERT(public_actor_id_equal(&actor_a, &actor_b));
    ASSERT(public_actor_id_equal(&actor_a, &actor_zero));
}

TEST(test_public_actor_ledger_projection_derives_only_full_pubkeys) {
    WORLD_DECL;
    uint8_t pubkey[ACTOR_PRINCIPAL_ID_SIZE];
    resolver_stable_id(pubkey, 0x6a);
    public_actor_id_t projected = public_actor_id_none();
    public_actor_id_t expected = public_actor_id_none();
    actor_principal_t principal = actor_principal_none();

    w.pubkey_registry[0].in_use = true;
    memcpy(w.pubkey_registry[0].pubkey, pubkey, sizeof(pubkey));
    ASSERT(public_actor_id_from_ledger_projection(
        &w, pubkey, &projected));
    ASSERT_EQ_INT(projected.kind, PUBLIC_ACTOR_ID_DERIVED);
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, pubkey, &principal));
    ASSERT(public_actor_id_from_principal(
        &principal, &expected));
    ASSERT(public_actor_id_equal(&projected, &expected));

    projected = public_actor_id_unattributed();
    ASSERT(!public_actor_id_from_ledger_projection(
        &w, NULL, &projected));
    ASSERT_EQ_INT(projected.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT(!public_actor_id_from_ledger_projection(
        &w, pubkey, NULL));
    ASSERT(!public_actor_id_from_ledger_projection(
        NULL, pubkey, &projected));

    resolver_stable_id(pubkey, 0x6b);
    ASSERT(public_actor_id_from_ledger_projection(
        &w, pubkey, &projected));
    ASSERT_EQ_INT(
        projected.kind, PUBLIC_ACTOR_ID_LEGACY_UNATTRIBUTED);
}

TEST(test_public_actor_npc_worker_trace_contract_has_no_bearer) {
    ASSERT(strcmp(SIGNAL_NPC_WORKER_TRACE_SCHEMA,
                  "signal.npc_worker_shadow.v3") == 0);
    ASSERT(strstr(SIGNAL_NPC_WORKER_TRACE_PUBLIC_IDENTITY_JSON,
                  "\"actor_kind\":\"unattributed\"") != NULL);
    ASSERT(strstr(SIGNAL_NPC_WORKER_TRACE_PUBLIC_IDENTITY_JSON,
                  "\"public_actor\":null") != NULL);
    ASSERT(strstr(SIGNAL_NPC_WORKER_TRACE_PUBLIC_IDENTITY_JSON,
                  "session_token") == NULL);
    ASSERT(strstr(SIGNAL_NPC_WORKER_TRACE_PUBLIC_IDENTITY_JSON,
                  "%02x") == NULL);
}

void register_public_actor_resolver_tests(void) {
    TEST_SECTION("\nPublic actor resolver tests (#669):\n");
    RUN(test_public_actor_verified_player_survives_token_rotation);
    RUN(test_public_actor_tracks_slot_transfer_and_reuse);
    RUN(test_public_actor_resolves_offline_pubkey_registry);
    RUN(test_public_actor_canonicalizes_duplicate_stale_registry_rows);
    RUN(test_public_actor_fails_closed_on_duplicate_runtime_actor);
    RUN(test_public_actor_npc_bearer_never_projects_or_resolves);
    RUN(test_public_actor_verified_token_locator_is_stable_and_fail_closed);
    RUN(test_public_actor_namespace_separation_and_station_identity);
    RUN(test_public_actor_station_duplicate_is_not_authority);
    RUN(test_public_actor_invalid_and_sentinel_ids_never_resolve);
    RUN(test_public_actor_ledger_projection_never_exposes_legacy_bearer);
    RUN(test_public_actor_ledger_projection_derives_only_full_pubkeys);
    RUN(test_public_actor_npc_worker_trace_contract_has_no_bearer);
}
