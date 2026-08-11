#include "test_harness.h"

#include "actor_principal.h"
#include "actor_principal_resolver.h"

static bool bytes_are_zero(const void *value, size_t size) {
    const uint8_t *bytes = value;
    if (!bytes) return false;
    uint8_t any = 0;
    for (size_t i = 0; i < size; i++)
        any |= bytes[i];
    return any == 0;
}

static void stable_id(uint8_t out[ACTOR_PRINCIPAL_ID_SIZE],
                      uint8_t discriminator) {
    memset(out, 0, ACTOR_PRINCIPAL_ID_SIZE);
    out[0] = discriminator;
    out[ACTOR_PRINCIPAL_ID_SIZE - 1] =
        (uint8_t)(discriminator ^ 0xA5u);
}

static bool make_verified_player(server_player_t *sp,
                                 uint8_t discriminator) {
    if (!sp) return false;
    sp->connected = true;
    sp->grace_period = false;
    sp->session_ready = true;
    sp->session_token[0] = discriminator;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_identity_finalized = true;
    stable_id(sp->pubkey, discriminator);
    return true;
}

TEST(test_actor_principal_sentinels_are_canonical_and_distinct) {
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_NONE, 0);
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_UNATTRIBUTED, 1);
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_PLAYER, 2);
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_NPC, 3);
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_STATION, 4);
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_SYSTEM, 5);
    ASSERT_EQ_INT(ACTOR_PRINCIPAL_KIND_COUNT, 6);

    actor_principal_t none = actor_principal_none();
    actor_principal_t unattributed = actor_principal_unattributed();

    ASSERT(actor_principal_is_canonical(&none));
    ASSERT(actor_principal_is_canonical(&unattributed));
    ASSERT_EQ_INT(none.kind, ACTOR_PRINCIPAL_NONE);
    ASSERT_EQ_INT(unattributed.kind, ACTOR_PRINCIPAL_UNATTRIBUTED);
    ASSERT(bytes_are_zero(none.id, sizeof(none.id)));
    ASSERT(bytes_are_zero(unattributed.id, sizeof(unattributed.id)));
    ASSERT(!actor_principal_equal(&none, &unattributed));

    uint8_t zero_id[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    actor_principal_t constructed = {.kind = UINT8_MAX};
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NONE, zero_id, &constructed));
    ASSERT(actor_principal_equal(&none, &constructed));
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_UNATTRIBUTED, zero_id, &constructed));
    ASSERT(actor_principal_equal(&unattributed, &constructed));
}

TEST(test_actor_principal_rejects_noncanonical_values_and_zeroes_output) {
    uint8_t id[ACTOR_PRINCIPAL_ID_SIZE];
    stable_id(id, 0x31);
    uint8_t zero_id[ACTOR_PRINCIPAL_ID_SIZE] = {0};
    actor_principal_t out;

    memset(&out, 0xA5, sizeof(out));
    ASSERT(!actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NONE, id, &out));
    ASSERT(bytes_are_zero(&out, sizeof(out)));

    memset(&out, 0xA5, sizeof(out));
    ASSERT(!actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, zero_id, &out));
    ASSERT(bytes_are_zero(&out, sizeof(out)));

    memset(&out, 0xA5, sizeof(out));
    ASSERT(!actor_principal_from_stable_id(
        (actor_principal_kind_t)UINT8_MAX, id, &out));
    ASSERT(bytes_are_zero(&out, sizeof(out)));

    memset(&out, 0xA5, sizeof(out));
    ASSERT(!actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, NULL, &out));
    ASSERT(bytes_are_zero(&out, sizeof(out)));
    ASSERT(!actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, id, NULL));

    actor_principal_t invalid = actor_principal_none();
    invalid.kind = UINT8_MAX;
    ASSERT(!actor_principal_is_canonical(&invalid));
    ASSERT(!actor_principal_equal(&invalid, &invalid));

    invalid = actor_principal_none();
    invalid.kind = ACTOR_PRINCIPAL_UNATTRIBUTED;
    invalid.id[0] = 1;
    ASSERT(!actor_principal_is_canonical(&invalid));
}

TEST(test_actor_principal_kind_domain_separates_stable_ids) {
    uint8_t id[ACTOR_PRINCIPAL_ID_SIZE];
    stable_id(id, 0x42);
    actor_principal_t player;
    actor_principal_t same_player;
    actor_principal_t npc;
    actor_principal_t station;

    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, id, &player));
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, id, &same_player));
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NPC, id, &npc));
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_STATION, id, &station));

    ASSERT(actor_principal_equal(&player, &same_player));
    ASSERT(!actor_principal_equal(&player, &npc));
    ASSERT(!actor_principal_equal(&player, &station));
    ASSERT(!actor_principal_equal(&npc, &station));
}

TEST(test_actor_principal_wire_encoding_is_explicit_and_alias_safe) {
    uint8_t id[ACTOR_PRINCIPAL_ID_SIZE];
    stable_id(id, 0x53);
    actor_principal_t principal;
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, id, &principal));

    uint8_t wire[ACTOR_PRINCIPAL_WIRE_SIZE];
    ASSERT(actor_principal_pack(&principal, wire));
    ASSERT_EQ_INT(wire[0], 2);
    ASSERT(memcmp(&wire[1], id, sizeof(id)) == 0);

    actor_principal_t decoded;
    ASSERT(actor_principal_unpack(wire, &decoded));
    ASSERT(actor_principal_equal(&principal, &decoded));

    actor_principal_t aliased = principal;
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NPC, aliased.id, &aliased));
    ASSERT_EQ_INT(aliased.kind, ACTOR_PRINCIPAL_NPC);
    ASSERT(memcmp(aliased.id, id, sizeof(id)) == 0);
    ASSERT(actor_principal_pack(&aliased, (uint8_t *)&aliased));
    ASSERT(actor_principal_unpack((const uint8_t *)&aliased, &aliased));
    ASSERT_EQ_INT(aliased.kind, ACTOR_PRINCIPAL_NPC);
    ASSERT(memcmp(aliased.id, id, sizeof(id)) == 0);

    memset(wire, 0, sizeof(wire));
    wire[0] = ACTOR_PRINCIPAL_PLAYER;
    memset(&decoded, 0xA5, sizeof(decoded));
    ASSERT(!actor_principal_unpack(wire, &decoded));
    ASSERT(bytes_are_zero(&decoded, sizeof(decoded)));

    actor_principal_t invalid = actor_principal_none();
    invalid.kind = UINT8_MAX;
    memset(wire, 0xA5, sizeof(wire));
    ASSERT(!actor_principal_pack(&invalid, wire));
    ASSERT(bytes_are_zero(wire, sizeof(wire)));

    memset(wire, 0xA5, sizeof(wire));
    ASSERT(!actor_principal_pack(NULL, wire));
    ASSERT(bytes_are_zero(wire, sizeof(wire)));
    ASSERT(!actor_principal_pack(&principal, NULL));

    memset(&decoded, 0xA5, sizeof(decoded));
    ASSERT(!actor_principal_unpack(NULL, &decoded));
    ASSERT(bytes_are_zero(&decoded, sizeof(decoded)));
    ASSERT(!actor_principal_unpack(wire, NULL));
}

TEST(test_verified_player_principal_requires_finalized_proof) {
    SERVER_PLAYER_DECL(sp);
    actor_principal_t principal;

    sp.session_ready = true;
    sp.session_token[0] = 1;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));

    stable_id(sp.pubkey, 0x64);
    sp.pubkey_set = true;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));

    sp.pubkey_proof_ok = true;
    sp.pubkey_challenge_consumed = true;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));

    sp.pubkey_identity_finalized = true;
    uint8_t input_before[sizeof(sp)];
    memcpy(input_before, &sp, sizeof(input_before));
    ASSERT(actor_principal_from_verified_player(&sp, &principal));
    ASSERT_EQ_INT(principal.kind, ACTOR_PRINCIPAL_PLAYER);
    ASSERT(memcmp(principal.id, sp.pubkey, sizeof(sp.pubkey)) == 0);
    ASSERT(memcmp(&sp, input_before, sizeof(input_before)) == 0);

    sp.pubkey_proof_ok = false;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));
    sp.pubkey_proof_ok = true;

    sp.pubkey_challenge_consumed = false;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));
    sp.pubkey_challenge_consumed = true;

    sp.session_ready = false;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));
    sp.session_ready = true;

    sp.pubkey_set = false;
    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(&sp, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));

    memset(&principal, 0xA5, sizeof(principal));
    ASSERT(!actor_principal_from_verified_player(NULL, &principal));
    ASSERT(bytes_are_zero(&principal, sizeof(principal)));
    ASSERT(!actor_principal_from_verified_player(&sp, NULL));
}

TEST(test_player_principal_resolution_tracks_liveness_not_slot_identity) {
    WORLD_DECL;
    server_player_t *owner = &w.players[5];
    ASSERT(make_verified_player(owner, 0x75));

    actor_principal_t principal;
    ASSERT(actor_principal_from_verified_player(owner, &principal));
    actor_principal_t principal_before = principal;
    uint8_t owner_before[sizeof(*owner)];
    uint8_t other_before[sizeof(w.players[6])];
    uint8_t registry_before[sizeof(w.pubkey_registry)];
    memcpy(owner_before, owner, sizeof(owner_before));
    memcpy(other_before, &w.players[6], sizeof(other_before));
    memcpy(registry_before, w.pubkey_registry, sizeof(registry_before));

    actor_resolution_result_t result =
        world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 5);
    ASSERT(memcmp(owner, owner_before, sizeof(owner_before)) == 0);
    ASSERT(memcmp(&w.players[6], other_before, sizeof(other_before)) == 0);
    ASSERT(memcmp(w.pubkey_registry, registry_before,
                  sizeof(registry_before)) == 0);
    ASSERT(actor_principal_equal(&principal, &principal_before));

    memset(owner->session_token, 0xE1, sizeof(owner->session_token));
    result = world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 5);

    owner->grace_period = true;
    result = world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_GRACE);
    ASSERT_EQ_INT(result.slot, 5);

    owner->grace_period = false;
    owner->connected = false;
    result = world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);

    owner->grace_period = true;
    result = world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);

    server_player_clear_live_session_identity(owner);
    result = world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);

    world_player_runtime_slot_reset(&w, 5);
    owner = &w.players[5];
    ASSERT(make_verified_player(owner, 0x76));

    result = world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);

    actor_principal_t replacement;
    ASSERT(actor_principal_from_verified_player(owner, &replacement));
    result = world_resolve_player_principal(&w, &replacement);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_ONLINE);
    ASSERT_EQ_INT(result.slot, 5);
}

TEST(test_player_principal_resolution_fails_closed_on_ambiguity) {
    WORLD_DECL;
    ASSERT(make_verified_player(&w.players[2], 0x87));
    ASSERT(make_verified_player(&w.players[9], 0x87));

    actor_principal_t principal;
    ASSERT(actor_principal_from_verified_player(
        &w.players[2], &principal));
    actor_principal_t principal_before = principal;
    uint8_t first_before[sizeof(w.players[2])];
    uint8_t second_before[sizeof(w.players[9])];
    memcpy(first_before, &w.players[2], sizeof(first_before));
    memcpy(second_before, &w.players[9], sizeof(second_before));
    actor_resolution_result_t result =
        world_resolve_player_principal(&w, &principal);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);
    ASSERT(actor_principal_equal(&principal, &principal_before));
    ASSERT(memcmp(&w.players[2], first_before, sizeof(first_before)) == 0);
    ASSERT(memcmp(&w.players[9], second_before, sizeof(second_before)) == 0);
}

TEST(test_player_principal_resolution_distinguishes_offline_from_unknown) {
    WORLD_DECL;
    uint8_t id[ACTOR_PRINCIPAL_ID_SIZE];
    stable_id(id, 0x98);
    actor_principal_t player;
    actor_principal_t npc;
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_PLAYER, id, &player));
    ASSERT(actor_principal_from_stable_id(
        ACTOR_PRINCIPAL_NPC, id, &npc));

    /* A bounded registry row is not proof of a current live binding. */
    w.pubkey_registry[0].in_use = true;
    memcpy(w.pubkey_registry[0].pubkey, id, sizeof(id));
    w.pubkey_registry[0].session_token[0] = 1;

    actor_resolution_result_t result =
        world_resolve_player_principal(&w, &player);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_OFFLINE);
    ASSERT_EQ_INT(result.slot, -1);

    result = world_resolve_player_principal(&w, &npc);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);

    actor_principal_t unattributed = actor_principal_unattributed();
    result = world_resolve_player_principal(&w, &unattributed);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);

    actor_principal_t invalid = actor_principal_none();
    invalid.kind = ACTOR_PRINCIPAL_PLAYER;
    result = world_resolve_player_principal(&w, &invalid);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);

    result = world_resolve_player_principal(NULL, &player);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);

    result = world_resolve_player_principal(&w, NULL);
    ASSERT_EQ_INT(result.state, ACTOR_RESOLUTION_UNKNOWN);
    ASSERT_EQ_INT(result.slot, -1);
}

void register_actor_principal_tests(void) {
    TEST_SECTION("\nActor principal tests:\n");
    RUN(test_actor_principal_sentinels_are_canonical_and_distinct);
    RUN(test_actor_principal_rejects_noncanonical_values_and_zeroes_output);
    RUN(test_actor_principal_kind_domain_separates_stable_ids);
    RUN(test_actor_principal_wire_encoding_is_explicit_and_alias_safe);
    RUN(test_verified_player_principal_requires_finalized_proof);
    RUN(test_player_principal_resolution_tracks_liveness_not_slot_identity);
    RUN(test_player_principal_resolution_fails_closed_on_ambiguity);
    RUN(test_player_principal_resolution_distinguishes_offline_from_unknown);
}
