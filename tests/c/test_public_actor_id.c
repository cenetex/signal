#include "test_harness.h"

#include <string.h>

#include "../../shared/public_actor_id.h"

static actor_principal_t public_actor_test_principal(
    actor_principal_kind_t kind,
    uint8_t seed) {
    actor_principal_t principal = {
        .kind = (uint8_t)kind,
    };
    memset(principal.id, seed, sizeof(principal.id));
    return principal;
}

TEST(test_public_actor_id_is_domain_separated_and_stable) {
    actor_principal_t player = public_actor_test_principal(
        ACTOR_PRINCIPAL_PLAYER, 0x5a);
    actor_principal_t station = public_actor_test_principal(
        ACTOR_PRINCIPAL_STATION, 0x5a);
    actor_principal_t system = public_actor_test_principal(
        ACTOR_PRINCIPAL_SYSTEM, 0x5a);
    public_actor_id_t player_a = public_actor_id_none();
    public_actor_id_t player_b = public_actor_id_none();
    public_actor_id_t station_actor = public_actor_id_none();
    public_actor_id_t system_actor = public_actor_id_none();

    ASSERT(public_actor_id_from_principal(&player, &player_a));
    ASSERT(public_actor_id_from_principal(&player, &player_b));
    ASSERT(public_actor_id_from_principal(&station, &station_actor));
    ASSERT(public_actor_id_from_principal(&system, &system_actor));
    ASSERT(public_actor_id_equal(&player_a, &player_b));
    ASSERT(!public_actor_id_equal(&player_a, &station_actor));
    ASSERT(!public_actor_id_equal(&player_a, &system_actor));
    ASSERT(!public_actor_id_equal(&station_actor, &system_actor));
    ASSERT(memcmp(player_a.id, player.id, sizeof(player_a.id)) != 0);
}

TEST(test_public_actor_id_known_vector) {
    actor_principal_t player = public_actor_test_principal(
        ACTOR_PRINCIPAL_PLAYER, 0x01);
    public_actor_id_t actor = public_actor_id_none();
    static const uint8_t expected[PUBLIC_ACTOR_ID_SIZE] = {
        0x4a, 0xb5, 0xed, 0xe8, 0x92, 0x90, 0x71, 0x8b,
        0xea, 0x80, 0x59, 0xb4, 0x34, 0xfb, 0xb6, 0x2c,
        0xd5, 0x6e, 0x8e, 0x71, 0x94, 0x4d, 0x09, 0x9e,
        0xaa, 0x60, 0x5f, 0x38, 0xb1, 0xff, 0x20, 0xf3,
    };

    ASSERT(public_actor_id_from_principal(&player, &actor));
    ASSERT(memcmp(actor.id, expected, sizeof(expected)) == 0);
}

TEST(test_public_actor_id_sentinels_are_explicit_and_distinct) {
    public_actor_id_t none = public_actor_id_none();
    public_actor_id_t unattributed = public_actor_id_unattributed();
    public_actor_id_t legacy = public_actor_id_legacy_unattributed();

    ASSERT(public_actor_id_is_canonical(&none));
    ASSERT(public_actor_id_is_canonical(&unattributed));
    ASSERT(public_actor_id_is_canonical(&legacy));
    ASSERT(!public_actor_id_equal(&none, &none));
    ASSERT(public_actor_id_equal(&unattributed, &unattributed));
    ASSERT(public_actor_id_equal(&legacy, &legacy));
    ASSERT(!public_actor_id_equal(&unattributed, &legacy));
}

TEST(test_public_actor_id_rejects_sentinel_principals) {
    actor_principal_t none = actor_principal_none();
    actor_principal_t unattributed = actor_principal_unattributed();
    actor_principal_t invalid = public_actor_test_principal(
        ACTOR_PRINCIPAL_PLAYER, 0x44);
    actor_principal_t npc = public_actor_test_principal(
        ACTOR_PRINCIPAL_NPC, 0x45);
    public_actor_id_t out = public_actor_id_unattributed();

    ASSERT(!public_actor_id_from_principal(&none, &out));
    ASSERT_EQ_INT(out.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT(!public_actor_id_from_principal(&unattributed, &out));
    ASSERT_EQ_INT(out.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT(!public_actor_id_from_principal(&npc, &out));
    ASSERT_EQ_INT(out.kind, PUBLIC_ACTOR_ID_NONE);
    invalid.kind = ACTOR_PRINCIPAL_KIND_COUNT;
    out = public_actor_id_unattributed();
    ASSERT(!public_actor_id_from_principal(&invalid, &out));
    ASSERT_EQ_INT(out.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT(!public_actor_id_from_principal(NULL, &out));
    ASSERT_EQ_INT(out.kind, PUBLIC_ACTOR_ID_NONE);
    ASSERT(!public_actor_id_from_principal(&invalid, NULL));
}

TEST(test_public_actor_id_wire_round_trip_and_fail_closed) {
    actor_principal_t player = public_actor_test_principal(
        ACTOR_PRINCIPAL_PLAYER, 0xa5);
    public_actor_id_t actor = public_actor_id_none();
    public_actor_id_t decoded = public_actor_id_none();
    uint8_t wire[PUBLIC_ACTOR_ID_WIRE_SIZE];

    ASSERT(public_actor_id_from_principal(&player, &actor));
    ASSERT(public_actor_id_pack(&actor, wire));
    ASSERT(public_actor_id_unpack(wire, &decoded));
    ASSERT(public_actor_id_equal(&actor, &decoded));

    public_actor_id_t none = public_actor_id_none();
    memset(wire, 0x5a, sizeof(wire));
    ASSERT(!public_actor_id_pack(&none, wire));
    uint8_t zero_wire[PUBLIC_ACTOR_ID_WIRE_SIZE] = {0};
    ASSERT(memcmp(wire, zero_wire, sizeof(wire)) == 0);
    ASSERT(public_actor_id_unpack(zero_wire, &decoded));
    ASSERT_EQ_INT(decoded.kind, PUBLIC_ACTOR_ID_NONE);

    memset(wire, 0, sizeof(wire));
    wire[0] = PUBLIC_ACTOR_ID_DERIVED;
    ASSERT(!public_actor_id_unpack(wire, &decoded));
    ASSERT_EQ_INT(decoded.kind, PUBLIC_ACTOR_ID_NONE);

    memset(wire, 0xff, sizeof(wire));
    ASSERT(!public_actor_id_unpack(wire, &decoded));
    ASSERT_EQ_INT(decoded.kind, PUBLIC_ACTOR_ID_NONE);
}

void register_public_actor_id_tests(void) {
    RUN(test_public_actor_id_is_domain_separated_and_stable);
    RUN(test_public_actor_id_known_vector);
    RUN(test_public_actor_id_sentinels_are_explicit_and_distinct);
    RUN(test_public_actor_id_rejects_sentinel_principals);
    RUN(test_public_actor_id_wire_round_trip_and_fail_closed);
}
