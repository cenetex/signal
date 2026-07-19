#include "test_harness.h"

#include "cargo_package.h"

static cargo_unit_t package_test_unit(int seed) {
    cargo_unit_t unit = {
        .kind = CARGO_KIND_INGOT,
        .commodity = COMMODITY_FERRITE_INGOT,
        .grade = MINING_GRADE_COMMON,
        .quantity = 1,
    };
    for (int i = 0; i < 32; i++)
        unit.pub[i] = (uint8_t)(seed * 17 + i);
    return unit;
}

TEST(test_cargo_package_known_vector_is_canonical_and_domain_separated) {
    cargo_unit_t ordered[4] = {
        package_test_unit(1), package_test_unit(2),
        package_test_unit(3), package_test_unit(4),
    };
    cargo_unit_t shuffled[4] = {
        ordered[2], ordered[0], ordered[3], ordered[1],
    };
    cargo_package_t a, b;
    ASSERT(cargo_package_pack(ordered, 4, &a));
    ASSERT(cargo_package_pack(shuffled, 4, &b));
    ASSERT(!cargo_package_is_partial(&a));
    ASSERT_EQ_INT(a.provenance, CARGO_PACKAGE_PROVENANCE_KNOWN);
    ASSERT(memcmp(a.package_pub, b.package_pub, 32) == 0);
    ASSERT_HEX32_EQ(a.package_pub,
        "ecfcc08d93fa94a1e4cf732819c6469410c33ba807dd131a78f61d32cbbe9ffe");
    for (int i = 0; i < 4; i++)
        ASSERT(memcmp(a.package_pub, a.members[i].pub, 32) != 0);
}

TEST(test_cargo_package_pack_unpack_preserves_every_partial_member) {
    cargo_unit_t input[3] = {
        package_test_unit(3), package_test_unit(1), package_test_unit(2),
    };
    cargo_unit_t output[4] = {0};
    cargo_package_t package;
    size_t count = 0;
    ASSERT(cargo_package_pack(input, 3, &package));
    ASSERT(cargo_package_is_partial(&package));
    ASSERT(cargo_package_unpack(&package, output, 4, &count));
    ASSERT_EQ_INT(count, 3);
    for (size_t i = 0; i < count; i++) {
        bool found = false;
        for (size_t j = 0; j < count; j++)
            if (memcmp(input[i].pub, output[j].pub, 32) == 0) found = true;
        ASSERT(found);
    }
}

TEST(test_cargo_package_shell_and_custody_are_separate_from_payload_identity) {
    cargo_unit_t members[2] = {package_test_unit(1), package_test_unit(2)};
    cargo_package_t package;
    uint8_t root[32], shell[32];
    memset(shell, 0xA5, sizeof(shell));
    ASSERT(cargo_package_pack(members, 2, &package));
    memcpy(root, package.package_pub, 32);
    ASSERT(cargo_package_attach_shell(&package, shell));
    ASSERT(memcmp(package.carrier_shell_pub, shell, 32) == 0);
    ASSERT(memcmp(package.package_pub, shell, 32) != 0);

    ASSERT(cargo_package_move(&package, CARGO_PACKAGE_CUSTODY_CARRIER, 9));
    ASSERT(cargo_package_move(&package, CARGO_PACKAGE_CUSTODY_SHIP, 77));
    ASSERT(cargo_package_move(&package, CARGO_PACKAGE_CUSTODY_STATION, 3));
    ASSERT_EQ_INT(package.custody, CARGO_PACKAGE_CUSTODY_STATION);
    ASSERT_EQ_INT(package.custody_index, 3);
    ASSERT(memcmp(package.package_pub, root, 32) == 0);
}

TEST(test_cargo_package_codec_round_trips_and_legacy_stays_unknown) {
    cargo_unit_t legacy[2] = {0};
    legacy[0].kind = legacy[1].kind = CARGO_KIND_INGOT;
    legacy[0].commodity = legacy[1].commodity = COMMODITY_FERRITE_INGOT;
    legacy[0].quantity = legacy[1].quantity = 1;
    cargo_package_t package, decoded;
    uint8_t bytes[512] = {0};
    size_t written = 0, consumed = 0;
    ASSERT(cargo_package_pack(legacy, 2, &package));
    ASSERT_EQ_INT(package.provenance, CARGO_PACKAGE_PROVENANCE_UNKNOWN);
    uint8_t zero[32] = {0};
    ASSERT(memcmp(package.package_pub, zero, 32) == 0);
    ASSERT(cargo_package_encode(&package, bytes, sizeof(bytes), &written));
    ASSERT(cargo_package_decode(bytes, written, &decoded, &consumed));
    ASSERT_EQ_INT(consumed, written);
    ASSERT_EQ_INT(decoded.member_count, 2);
    ASSERT_EQ_INT(decoded.provenance, CARGO_PACKAGE_PROVENANCE_UNKNOWN);
    ASSERT(memcmp(decoded.members, legacy, sizeof(legacy)) == 0);
}

void register_cargo_package_tests(void) {
    TEST_SECTION("\nCargo Merkle package tests:\n");
    RUN(test_cargo_package_known_vector_is_canonical_and_domain_separated);
    RUN(test_cargo_package_pack_unpack_preserves_every_partial_member);
    RUN(test_cargo_package_shell_and_custody_are_separate_from_payload_identity);
    RUN(test_cargo_package_codec_round_trips_and_legacy_stays_unknown);
}
