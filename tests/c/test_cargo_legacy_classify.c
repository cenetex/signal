#include "cargo_legacy_classify.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(CARGO_LEGACY_CLASSIFY_STANDALONE_TEST)
static int cargo_legacy_test_run;
static int cargo_legacy_test_passed;
static int cargo_legacy_test_failed;

#define TEST(name) static void name(void)
#define TEST_SECTION(banner) ((void)(banner))
#define RUN(name) do { \
    int before = cargo_legacy_test_failed; \
    cargo_legacy_test_run++; \
    name(); \
    if (cargo_legacy_test_failed == before) cargo_legacy_test_passed++; \
} while (0)
#define ASSERT(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        cargo_legacy_test_failed++; \
        return; \
    } \
} while (0)
#define ASSERT_EQ_INT(actual, expected) do { \
    int actual_value = (int)(actual); \
    int expected_value = (int)(expected); \
    if (actual_value != expected_value) { \
        fprintf(stderr, "%s:%d: got %d, expected %d\n", \
                __FILE__, __LINE__, actual_value, expected_value); \
        cargo_legacy_test_failed++; \
        return; \
    } \
} while (0)
#define ASSERT_STR_EQ(actual, expected) do { \
    const char *actual_value = (actual); \
    const char *expected_value = (expected); \
    if (strcmp(actual_value, expected_value) != 0) { \
        fprintf(stderr, "%s:%d: got \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, actual_value, expected_value); \
        cargo_legacy_test_failed++; \
        return; \
    } \
} while (0)
#else
#include "test_harness.h"
#endif

#define ASSERT_BYTES_EQ(actual, expected, length) \
    ASSERT(memcmp((actual), (expected), (length)) == 0)

/*
 * These golden hashes were calculated independently with Node's
 * crypto.createHash("sha256") from the 44-byte preimages documented by
 * b06e372^:shared/manifest.c.  They do not call the classifier to construct
 * their expected identities.
 */
static bool cargo_legacy_test_parse_hex32(
    const char *hex,
    uint8_t out[CARGO_LEGACY_PUBKEY_SIZE]) {
    if (!hex || !out || strlen(hex) != 64u) return false;
    for (size_t i = 0; i < CARGO_LEGACY_PUBKEY_SIZE; i++) {
        unsigned high;
        unsigned low;
        char hc = hex[i * 2u];
        char lc = hex[i * 2u + 1u];
        if (hc >= '0' && hc <= '9') high = (unsigned)(hc - '0');
        else if (hc >= 'a' && hc <= 'f')
            high = (unsigned)(hc - 'a') + 10u;
        else return false;
        if (lc >= '0' && lc <= '9') low = (unsigned)(lc - '0');
        else if (lc >= 'a' && lc <= 'f')
            low = (unsigned)(lc - 'a') + 10u;
        else return false;
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static void cargo_legacy_test_fill_sequence(
    uint8_t out[CARGO_LEGACY_PUBKEY_SIZE],
    uint8_t first) {
    for (size_t i = 0; i < CARGO_LEGACY_PUBKEY_SIZE; i++)
        out[i] = (uint8_t)(first + (uint8_t)i);
}

static void cargo_legacy_test_write_u16_le(
    uint8_t out[2],
    uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void cargo_legacy_test_write_u64_le(
    uint8_t out[8],
    uint64_t value) {
    for (unsigned i = 0; i < 8; i++)
        out[i] = (uint8_t)(value >> (i * 8u));
}

static bool cargo_legacy_test_build_smelt(
    uint8_t payload[CARGO_LEGACY_SMELT_V0_PAYLOAD_SIZE],
    uint8_t fragment_first,
    const char *output_hex,
    uint8_t prefix,
    uint64_t mined_block) {
    memset(payload, 0, CARGO_LEGACY_SMELT_V0_PAYLOAD_SIZE);
    cargo_legacy_test_fill_sequence(&payload[0], fragment_first);
    if (!cargo_legacy_test_parse_hex32(output_hex, &payload[32]))
        return false;
    payload[64] = prefix;
    cargo_legacy_test_write_u64_le(&payload[72], mined_block);
    return true;
}

static bool cargo_legacy_test_build_craft(
    uint8_t payload[CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE],
    uint16_t recipe_id,
    const uint8_t input_firsts[CARGO_LEGACY_RECIPE_INPUT_MAX],
    uint8_t input_count,
    const char *output_hex) {
    memset(payload, 0, CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE);
    cargo_legacy_test_write_u16_le(&payload[0], recipe_id);
    payload[2] = input_count;
    if (!cargo_legacy_test_parse_hex32(output_hex, &payload[8]))
        return false;
    for (size_t i = 0;
         i < input_count && i < CARGO_LEGACY_RECIPE_INPUT_MAX;
         i++) {
        cargo_legacy_test_fill_sequence(&payload[40 + i * 32u],
                                        input_firsts[i]);
    }
    return true;
}

TEST(test_cargo_legacy_status_codes_and_names_are_stable) {
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_ANONYMOUS, 0);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_M, 1);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_H, 2);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_T, 3);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_S, 4);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_F, 5);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_K, 6);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_RATI, 7);
    ASSERT_EQ_INT(CARGO_LEGACY_PREFIX_COMMISSIONED, 8);

    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_BAD_ARGUMENTS, 0);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_MALFORMED, 1);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_CURRENT_V1, 2);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_UNBOUND_V0, 3);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_NONE, 4);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_AMBIGUOUS, 5);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_RESOURCE, 6);
    ASSERT_EQ_INT(CARGO_LEGACY_STATUS_COUNT, 7);

    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_BAD_ARGUMENTS),
                  "bad_arguments");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_MALFORMED),
                  "malformed");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_CURRENT_V1),
                  "current_v1");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_UNBOUND_V0),
                  "unbound_v0");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_NONE),
                  "none");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_AMBIGUOUS),
                  "ambiguous");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_RESOURCE),
                  "resource");
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      (cargo_legacy_status_t)UINT8_MAX),
                  "unknown");
}

TEST(test_cargo_legacy_identity_matches_frozen_golden_vectors) {
    uint8_t parent[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t actual[CARGO_LEGACY_PUBKEY_SIZE] = {0};
    uint8_t expected[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t before[CARGO_LEGACY_PUBKEY_SIZE];

    cargo_legacy_test_fill_sequence(parent, 0x00);
    ASSERT(cargo_legacy_test_parse_hex32(
        "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
        expected));
    ASSERT(cargo_legacy_identity_v0_derive(
        CARGO_LEGACY_RECIPE_SMELT_ID, parent, 0x1234u, actual));
    ASSERT_BYTES_EQ(actual, expected, sizeof(actual));

    ASSERT(cargo_legacy_test_parse_hex32(
        "c81bf2fc36c98ba6885dad2ef6e95aada8aef55f9f84b4efbc5e51302f59c061",
        expected));
    ASSERT(cargo_legacy_identity_v0_derive(
        0x1234u, parent, 0xabcdu, actual));
    ASSERT_BYTES_EQ(actual, expected, sizeof(actual));

    ASSERT(cargo_legacy_test_parse_hex32(
        "add60a381fd23af80ba8e3770f7e27ca20990a2d255cded1c545a27a75be99f2",
        parent));
    ASSERT(cargo_legacy_test_parse_hex32(
        "6275eaef22846d44907fbdfffe55e7a0dcf3ab938f7785e05da1c328dec73710",
        expected));
    ASSERT(cargo_legacy_identity_v0_derive(
        4u, parent, 99u, actual));
    ASSERT_BYTES_EQ(actual, expected, sizeof(actual));

    memset(actual, 0x5a, sizeof(actual));
    memcpy(before, actual, sizeof(before));
    ASSERT(!cargo_legacy_identity_v0_derive(
        4u, NULL, 99u, actual));
    ASSERT_BYTES_EQ(actual, before, sizeof(actual));
    ASSERT(!cargo_legacy_identity_v0_derive(
        4u, parent, 99u, NULL));
}

TEST(test_cargo_legacy_prefix_derivation_matches_frozen_classes) {
    static const struct {
        const char *pub_hex;
        uint8_t expected_class;
    } vectors[] = {
        {
            "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
            CARGO_LEGACY_PREFIX_ANONYMOUS,
        },
        {
            "05503a6343cb2fc7668caef625bf9bcf65afbb544836b36ee0122eed903c6e5a",
            CARGO_LEGACY_PREFIX_M,
        },
        {
            "f066d8524a3a9e98ea5137bf0fcad3bfcf991bccd8f0e2178c8ced9322092801",
            CARGO_LEGACY_PREFIX_H,
        },
        {
            "06c60f536e0d152a7f2cdfcb029bc7817f91c573f36d8ea465abaeff7dc0adac",
            CARGO_LEGACY_PREFIX_T,
        },
        {
            "068ee37cf55c8902cf531da7f785a66398c87a491383e3e7275ffc684dca104e",
            CARGO_LEGACY_PREFIX_S,
        },
        {
            "d869450f3625b4c095dabb2e60a7be66abc67c706a13d362496770890d21d725",
            CARGO_LEGACY_PREFIX_F,
        },
        {
            "04a26af0b4b1dd311733eda0c3f69a423865e15a1c27b2f028e43cfa74b0d003",
            CARGO_LEGACY_PREFIX_K,
        },
        {
            "0630a2eafedac17942928b6ee5a6538f0fdab94714843515f6d5928000000000",
            CARGO_LEGACY_PREFIX_RATI,
        },
    };
    uint8_t pub[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t actual_class = UINT8_MAX;

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        ASSERT(cargo_legacy_test_parse_hex32(vectors[i].pub_hex, pub));
        ASSERT(cargo_legacy_prefix_class_v0_derive(
            pub, &actual_class));
        ASSERT_EQ_INT(actual_class, vectors[i].expected_class);
        ASSERT(actual_class != CARGO_LEGACY_PREFIX_COMMISSIONED);
    }

    actual_class = 0x5au;
    ASSERT(!cargo_legacy_prefix_class_v0_derive(
        NULL, &actual_class));
    ASSERT_EQ_INT(actual_class, 0x5a);
    ASSERT(!cargo_legacy_prefix_class_v0_derive(pub, NULL));
}

TEST(test_cargo_legacy_index_recovery_is_bounded_and_transactional) {
    uint8_t parent[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t target[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t missing[CARGO_LEGACY_PUBKEY_SIZE];
    uint16_t index = 0xbeefu;

    cargo_legacy_test_fill_sequence(parent, 0x00);
    ASSERT(cargo_legacy_test_parse_hex32(
        "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
        target));
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, target, 0x1235u, &index),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT_EQ_INT(index, 0x1234);

    memcpy(missing, target, sizeof(missing));
    missing[31] ^= 0x80u;
    index = 0xbeefu;
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, missing, 0x1235u, &index),
                  CARGO_LEGACY_STATUS_NONE);
    ASSERT_EQ_INT(index, 0xbeef);

    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, target, 0u, &index),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, target,
                      CARGO_LEGACY_OUTPUT_INDEX_COUNT + 1u,
                      &index),
                  CARGO_LEGACY_STATUS_RESOURCE);
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, NULL, target, 1u, &index),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, NULL, 1u, &index),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, target, 1u, NULL),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(index, 0xbeef);

    cargo_legacy_test_fill_sequence(parent, 0xa0);
    ASSERT(cargo_legacy_test_parse_hex32(
        "9bc333d7e5a4476a317cd2d25f81afa46f7ccd63e624f0f7f577d9e736e5a3ea",
        target));
    ASSERT_EQ_INT(cargo_legacy_recover_output_index_v0(
                      0u, parent, target,
                      CARGO_LEGACY_OUTPUT_INDEX_COUNT,
                      &index),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT_EQ_INT(index, UINT16_MAX);

    /*
     * A real AMBIGUOUS vector would be a SHA-256 collision between two
     * distinct uint16_t index preimages, so it is not constructible as a
     * deterministic regression fixture.  The recovery loop nevertheless
     * counts past its first match and returns the stable AMBIGUOUS verdict if
     * one is ever observed.
     */
    ASSERT_STR_EQ(cargo_legacy_status_name(
                      CARGO_LEGACY_STATUS_AMBIGUOUS),
                  "ambiguous");
}

TEST(test_cargo_legacy_smelt_v0_golden_and_boundary) {
    uint8_t payload[CARGO_LEGACY_SMELT_V0_PAYLOAD_SIZE];
    cargo_legacy_smelt_v0_result_t result;
    uint8_t expected_fragment[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t expected_output[CARGO_LEGACY_PUBKEY_SIZE];

    ASSERT(cargo_legacy_test_build_smelt(
        payload, 0x00,
        "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
        CARGO_LEGACY_PREFIX_ANONYMOUS,
        UINT64_C(0x0123456789abcdef)));
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    cargo_legacy_test_fill_sequence(expected_fragment, 0x00);
    ASSERT(cargo_legacy_test_parse_hex32(
        "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
        expected_output));
    ASSERT_BYTES_EQ(result.fragment_pub, expected_fragment,
                    sizeof(expected_fragment));
    ASSERT_BYTES_EQ(result.legacy_output_pub, expected_output,
                    sizeof(expected_output));
    ASSERT_EQ_INT(result.output_index, 0x1234);
    ASSERT_EQ_INT(result.legacy_prefix_class,
                  CARGO_LEGACY_PREFIX_ANONYMOUS);
    ASSERT(result.legacy_mined_block ==
           UINT64_C(0x0123456789abcdef));

    ASSERT(cargo_legacy_test_build_smelt(
        payload, 0x00,
        "d869450f3625b4c095dabb2e60a7be66abc67c706a13d362496770890d21d725",
        CARGO_LEGACY_PREFIX_F, 17u));
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT_EQ_INT(result.output_index, 0);
    ASSERT_EQ_INT(result.legacy_prefix_class,
                  CARGO_LEGACY_PREFIX_F);

    ASSERT(cargo_legacy_test_build_smelt(
        payload, 0xa0,
        "9bc333d7e5a4476a317cd2d25f81afa46f7ccd63e624f0f7f577d9e736e5a3ea",
        0u, UINT64_MAX));
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT_EQ_INT(result.output_index, UINT16_MAX);
    ASSERT(result.legacy_mined_block == UINT64_MAX);
}

TEST(test_cargo_legacy_smelt_rejects_non_v0_and_tampering) {
    uint8_t payload[CARGO_LEGACY_SMELT_V0_PAYLOAD_SIZE];
    uint8_t tampered_prefix;
    cargo_legacy_smelt_v0_result_t result;
    cargo_legacy_smelt_v0_result_t before;

    ASSERT(cargo_legacy_test_build_smelt(
        payload, 0x00,
        "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
        CARGO_LEGACY_PREFIX_ANONYMOUS, 42u));
    memset(&result, 0x5a, sizeof(result));
    before = result;

    payload[65] = 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_CURRENT_V1);
    ASSERT_BYTES_EQ(&result, &before, sizeof(result));
    payload[65] = 2u;
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    ASSERT_BYTES_EQ(&result, &before, sizeof(result));
    payload[65] = 0u;

    for (size_t i = 66; i < 72; i++) {
        payload[i] = 1u;
        ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                          payload, sizeof(payload), &result),
                      CARGO_LEGACY_STATUS_MALFORMED);
        payload[i] = 0u;
    }

    payload[64] = CARGO_LEGACY_PREFIX_M;
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    payload[64] = CARGO_LEGACY_PREFIX_COMMISSIONED;
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    payload[64] = UINT8_MAX;
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    payload[64] = CARGO_LEGACY_PREFIX_ANONYMOUS;

    memset(&payload[0], 0, 32);
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    cargo_legacy_test_fill_sequence(&payload[0], 0x00);
    memset(&payload[32], 0, 32);
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    ASSERT(cargo_legacy_test_parse_hex32(
        "2956edb1be7d0c6672e008936eb95f4d72310e053581b2dcd21c013f6b9ceca4",
        &payload[32]));

    payload[63] ^= 0x01u;
    ASSERT(cargo_legacy_prefix_class_v0_derive(
        &payload[32], &tampered_prefix));
    payload[64] = tampered_prefix;
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_NONE);
    ASSERT_BYTES_EQ(&result, &before, sizeof(result));
    payload[63] ^= 0x01u;
    payload[64] = CARGO_LEGACY_PREFIX_ANONYMOUS;

    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload) - 1u, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload) + 1u, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      NULL, sizeof(payload), &result),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(cargo_legacy_classify_smelt_v0(
                      payload, sizeof(payload), NULL),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_BYTES_EQ(&result, &before, sizeof(result));
}

TEST(test_cargo_legacy_craft_one_input_golden) {
    static const uint8_t inputs[CARGO_LEGACY_RECIPE_INPUT_MAX] = {
        0x20, 0x00, 0x00
    };
    uint8_t payload[CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE];
    cargo_legacy_recipe_shape_t shape = {
        .recipe_id = 1u,
        .input_count = 1u,
        .output_count = 4u,
    };
    cargo_legacy_craft_v0_result_t result;
    uint8_t expected_parent[CARGO_LEGACY_PUBKEY_SIZE];
    uint8_t expected_output[CARGO_LEGACY_PUBKEY_SIZE];

    ASSERT(cargo_legacy_test_build_craft(
        payload, 1u, inputs, 1u,
        "e7fe3aa59c5fae14da47480e1489b5ea80cf51ab1f36701deb5cd320d3fc0f57"));
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    cargo_legacy_test_fill_sequence(expected_parent, 0x20);
    ASSERT(cargo_legacy_test_parse_hex32(
        "e7fe3aa59c5fae14da47480e1489b5ea80cf51ab1f36701deb5cd320d3fc0f57",
        expected_output));
    ASSERT_EQ_INT(result.recipe_id, 1);
    ASSERT_EQ_INT(result.input_count, 1);
    ASSERT_EQ_INT(result.output_index, 3);
    ASSERT_BYTES_EQ(result.parent_merkle, expected_parent,
                    sizeof(expected_parent));
    ASSERT_BYTES_EQ(result.legacy_output_pub, expected_output,
                    sizeof(expected_output));
    ASSERT_BYTES_EQ(result.input_pubs[0], expected_parent,
                    sizeof(expected_parent));
}

TEST(test_cargo_legacy_craft_merkle_golden_vectors) {
    static const uint8_t two_inputs[CARGO_LEGACY_RECIPE_INPUT_MAX] = {
        0x80, 0x40, 0x00
    };
    static const uint8_t three_inputs[CARGO_LEGACY_RECIPE_INPUT_MAX] = {
        0xf0, 0x10, 0x70
    };
    static const uint8_t permuted_inputs[CARGO_LEGACY_RECIPE_INPUT_MAX] = {
        0x70, 0xf0, 0x10
    };
    uint8_t payload[CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE];
    cargo_legacy_craft_v0_result_t result;
    cargo_legacy_recipe_shape_t shape = {
        .recipe_id = 2u,
        .input_count = 2u,
        .output_count = 1u,
    };
    uint8_t expected_parent[CARGO_LEGACY_PUBKEY_SIZE];

    ASSERT(cargo_legacy_test_build_craft(
        payload, 2u, two_inputs, 2u,
        "cabbdb364acd357123a4622b3f1d0d82fab00340e7d409d6b03518aa5a926e47"));
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT(cargo_legacy_test_parse_hex32(
        "0984dad7e24ef9a355be60edee5157ac153cc8f12d19bab20f26f47ef3885dfc",
        expected_parent));
    ASSERT_BYTES_EQ(result.parent_merkle, expected_parent,
                    sizeof(expected_parent));
    ASSERT_EQ_INT(result.output_index, 0);

    shape.recipe_id = 4u;
    shape.input_count = 3u;
    shape.output_count = 100u;
    ASSERT(cargo_legacy_test_build_craft(
        payload, 4u, three_inputs, 3u,
        "6275eaef22846d44907fbdfffe55e7a0dcf3ab938f7785e05da1c328dec73710"));
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT(cargo_legacy_test_parse_hex32(
        "add60a381fd23af80ba8e3770f7e27ca20990a2d255cded1c545a27a75be99f2",
        expected_parent));
    ASSERT_BYTES_EQ(result.parent_merkle, expected_parent,
                    sizeof(expected_parent));
    ASSERT_EQ_INT(result.output_index, 99);

    ASSERT(cargo_legacy_test_build_craft(
        payload, 4u, permuted_inputs, 3u,
        "6275eaef22846d44907fbdfffe55e7a0dcf3ab938f7785e05da1c328dec73710"));
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_UNBOUND_V0);
    ASSERT_BYTES_EQ(result.parent_merkle, expected_parent,
                    sizeof(expected_parent));
    ASSERT_EQ_INT(result.output_index, 99);

    shape.output_count = 99u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_NONE);
}

TEST(test_cargo_legacy_craft_rejects_malformed_inputs_and_padding) {
    static const uint8_t inputs[CARGO_LEGACY_RECIPE_INPUT_MAX] = {
        0x80, 0x40, 0x00
    };
    uint8_t payload[CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE];
    uint8_t pristine[CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE];
    cargo_legacy_recipe_shape_t shape = {
        .recipe_id = 2u,
        .input_count = 2u,
        .output_count = 1u,
    };
    cargo_legacy_craft_v0_result_t result;
    cargo_legacy_craft_v0_result_t before;

    ASSERT(cargo_legacy_test_build_craft(
        payload, 2u, inputs, 2u,
        "cabbdb364acd357123a4622b3f1d0d82fab00340e7d409d6b03518aa5a926e47"));
    memcpy(pristine, payload, sizeof(pristine));
    memset(&result, 0x5a, sizeof(result));
    before = result;

    payload[3] = 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_CURRENT_V1);
    ASSERT_BYTES_EQ(&result, &before, sizeof(result));
    payload[3] = 2u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));

    for (size_t i = 4; i < 8; i++) {
        payload[i] = 1u;
        ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                          payload, sizeof(payload), &shape, &result),
                      CARGO_LEGACY_STATUS_MALFORMED);
        payload[i] = 0u;
    }

    payload[0] = 3u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));
    payload[2] = 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));
    memset(&payload[8], 0, 32);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));
    memset(&payload[40], 0, 32);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));
    memcpy(&payload[72], &payload[40], 32);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));
    payload[104] = 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    memcpy(payload, pristine, sizeof(payload));
    payload[39] ^= 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_NONE);
    ASSERT_BYTES_EQ(&result, &before, sizeof(result));
}

TEST(test_cargo_legacy_craft_bounds_and_bad_arguments) {
    static const uint8_t inputs[CARGO_LEGACY_RECIPE_INPUT_MAX] = {
        0x20, 0x00, 0x00
    };
    uint8_t payload[CARGO_LEGACY_CRAFT_V0_PAYLOAD_SIZE];
    cargo_legacy_recipe_shape_t shape = {
        .recipe_id = 1u,
        .input_count = 1u,
        .output_count = 4u,
    };
    cargo_legacy_craft_v0_result_t result;

    ASSERT(cargo_legacy_test_build_craft(
        payload, 1u, inputs, 1u,
        "e7fe3aa59c5fae14da47480e1489b5ea80cf51ab1f36701deb5cd320d3fc0f57"));

    shape.output_count = CARGO_LEGACY_OUTPUT_INDEX_COUNT + 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_RESOURCE);
    shape.output_count = 0u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    shape.output_count = 4u;
    shape.input_count = 0u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    shape.input_count = CARGO_LEGACY_RECIPE_INPUT_MAX + 1u;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    shape.input_count = 1u;

    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload) - 1u, &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload) + 1u, &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      NULL, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), NULL, &result),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, NULL),
                  CARGO_LEGACY_STATUS_BAD_ARGUMENTS);

    cargo_legacy_test_write_u16_le(&payload[0],
                                   CARGO_LEGACY_RECIPE_SMELT_ID);
    shape.recipe_id = CARGO_LEGACY_RECIPE_SMELT_ID;
    ASSERT_EQ_INT(cargo_legacy_classify_craft_v0(
                      payload, sizeof(payload), &shape, &result),
                  CARGO_LEGACY_STATUS_MALFORMED);
}

void register_cargo_legacy_classify_tests(void) {
    TEST_SECTION("\nLegacy cargo classifier tests:\n");
    RUN(test_cargo_legacy_status_codes_and_names_are_stable);
    RUN(test_cargo_legacy_identity_matches_frozen_golden_vectors);
    RUN(test_cargo_legacy_prefix_derivation_matches_frozen_classes);
    RUN(test_cargo_legacy_index_recovery_is_bounded_and_transactional);
    RUN(test_cargo_legacy_smelt_v0_golden_and_boundary);
    RUN(test_cargo_legacy_smelt_rejects_non_v0_and_tampering);
    RUN(test_cargo_legacy_craft_one_input_golden);
    RUN(test_cargo_legacy_craft_merkle_golden_vectors);
    RUN(test_cargo_legacy_craft_rejects_malformed_inputs_and_padding);
    RUN(test_cargo_legacy_craft_bounds_and_bad_arguments);
}

#if defined(CARGO_LEGACY_CLASSIFY_STANDALONE_TEST)
int main(void) {
    register_cargo_legacy_classify_tests();
    printf("%d/%d legacy cargo classifier tests passed\n",
           cargo_legacy_test_passed, cargo_legacy_test_run);
    return cargo_legacy_test_failed == 0 ? 0 : 1;
}
#endif
