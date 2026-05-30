#include "catalog.h"
#include "reveal.h"
#include "metadata.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(test) do { tests_run++; test(); } while(0)
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); tests_failed++; return; } } while(0)
#define CHECK_STR(a, b) do { if (strcmp((a),(b)) != 0) { printf("FAIL %s:%d: '%s' != '%s'\n", __FILE__, __LINE__, (a), (b)); tests_failed++; return; } } while(0)

TEST(test_catalog_lookup_by_id) {
    const card_profile_t *c = catalog_by_profile_id("ruby");
    CHECK(c != NULL);
    CHECK_STR(c->character_name, "Ruby");
    CHECK_STR(c->role, "teacher");
    CHECK_STR(c->rarity, "common");
    CHECK(c->set_number == 1);
}

TEST(test_catalog_lookup_by_set_number) {
    const card_profile_t *c = catalog_by_set_number(12);
    CHECK(c != NULL);
    CHECK_STR(c->character_name, "Captain Null");
    CHECK_STR(c->rarity, "ultra-rare");
}

TEST(test_catalog_lookup_missing) {
    const card_profile_t *c = catalog_by_profile_id("nonexistent");
    CHECK(c == NULL);
}

TEST(test_catalog_profiles_count) {
    CHECK(catalog_card_count() == FIRST_BELL_PROFILE_COUNT);
    CHECK(FIRST_BELL_LIVE_COUNT == 24);
}

TEST(test_catalog_hash_deterministic) {
    char h1[65], h2[65];
    catalog_hash(h1);
    catalog_hash(h2);
    CHECK_STR(h1, h2);
    /* Hash should be 64 hex chars */
    CHECK(strlen(h1) == 64);
}

TEST(test_reveal_pack_commitment_deterministic) {
    char c1[65], c2[65];
    reveal_pack_commitment(
        "abc123", "asset1", "sig1", "owner1", "prod1", 5, "nonce1", c1);
    reveal_pack_commitment(
        "abc123", "asset1", "sig1", "owner1", "prod1", 5, "nonce1", c2);
    CHECK_STR(c1, c2);
    CHECK(strlen(c1) == 64);
}

TEST(test_reveal_different_inputs_different_output) {
    char c1[65], c2[65];
    reveal_pack_commitment(
        "abc123", "asset1", "sig1", "owner1", "prod1", 5, "nonce1", c1);
    reveal_pack_commitment(
        "abc123", "asset1", "sig1", "owner1", "prod1", 5, "nonce2", c2);
    CHECK(strcmp(c1, c2) != 0);
}

TEST(test_reveal_card_slot_in_range) {
    char proof[65];
    int card_index;
    reveal_card_slot("commit1", "seed1", "asset1", 0, proof, &card_index);
    CHECK(strlen(proof) == 64);
    CHECK(card_index >= 0);
    CHECK(card_index < FIRST_BELL_LIVE_COUNT);
}

TEST(test_reveal_card_slot_different_slots) {
    char p0[65], p1[65];
    int i0, i1;
    reveal_card_slot("commit1", "seed1", "asset1", 0, p0, &i0);
    reveal_card_slot("commit1", "seed1", "asset1", 1, p1, &i1);
    CHECK(strcmp(p0, p1) != 0);
    /* Might be same card_index by chance, but proofs must differ */
}

TEST(test_reveal_seed_deterministic) {
    char s1[65], s2[65];
    reveal_pack_seed("commit1", "asset1", "tx1", "nonce1", s1);
    reveal_pack_seed("commit1", "asset1", "tx1", "nonce1", s2);
    CHECK_STR(s1, s2);
    CHECK(strlen(s1) == 64);
}

TEST(test_metadata_pack_json_output) {
    char buf[METADATA_JSON_MAX];
    int len = metadata_pack_json("Test Pack", "TEST", "http://img", "coll123",
        "pack-1", 1, 5, 42, NULL, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "\"name\":\"Test Pack\"") != NULL);
    CHECK(strstr(buf, "\"symbol\":\"TEST\"") != NULL);
    CHECK(strstr(buf, "\"Serial\"") != NULL);
    CHECK(strstr(buf, "\"value\":\"42\"") != NULL);
}

TEST(test_metadata_card_json_output) {
    const card_profile_t *c = catalog_by_profile_id("captain-null");
    CHECK(c != NULL);
    char buf[METADATA_JSON_MAX];
    int len = metadata_card_json(c, "http://img", "coll123", NULL, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "Captain Null") != NULL);
    CHECK(strstr(buf, "ultra-rare") != NULL);
    CHECK(strstr(buf, "special") != NULL);
}

TEST(test_metadata_collection_json_output) {
    char buf[METADATA_JSON_MAX];
    int len = metadata_collection_json("Ruby High", "Ruby High", "RUBY",
        "http://img", "A school collection", buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "\"name\":\"Ruby High\"") != NULL);
    CHECK(strstr(buf, "\"symbol\":\"RUBY\"") != NULL);
}

TEST(test_pack_shape_well_defined) {
    for (int i = 0; i < CARDS_PER_PACK; i++) {
        CHECK(strlen(FIRST_BELL_PACK_SHAPE[i]) > 0);
    }
}

TEST(test_provenance_roundtrip) {
    reveal_provenance_t prov = {0};
    strncpy(prov.pack_reveal_version, REVEAL_VERSION, 31);
    strncpy(prov.commitment, "abc123commitment", 64);
    strncpy(prov.reveal_proof, "def456revealproof", 64);
    strncpy(prov.pack_asset_address, "PackAssetAddr123", 47);
    prov.reveal_slot = 3;

    const card_profile_t *c = catalog_by_set_number(1);
    CHECK(c != NULL);
    char buf[METADATA_JSON_MAX];
    int len = metadata_card_json(c, "http://img", "coll", &prov, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "abc123commitment") != NULL);
    CHECK(strstr(buf, "def456revealproof") != NULL);
    CHECK(strstr(buf, "PackAssetAddr123") != NULL);
}

int main(void) {
    RUN(test_catalog_lookup_by_id);
    RUN(test_catalog_lookup_by_set_number);
    RUN(test_catalog_lookup_missing);
    RUN(test_catalog_profiles_count);
    RUN(test_catalog_hash_deterministic);
    RUN(test_reveal_pack_commitment_deterministic);
    RUN(test_reveal_different_inputs_different_output);
    RUN(test_reveal_card_slot_in_range);
    RUN(test_reveal_card_slot_different_slots);
    RUN(test_reveal_seed_deterministic);
    RUN(test_metadata_pack_json_output);
    RUN(test_metadata_card_json_output);
    RUN(test_metadata_collection_json_output);
    RUN(test_pack_shape_well_defined);
    RUN(test_provenance_roundtrip);

    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
