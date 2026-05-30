#include "test_harness.h"
#include "settlement_engine.h"
#include "../server/chain_log.h"
#include "sha256.h"
#include <string.h>

/* Build a minimal event header */
static void make_hdr(chain_event_header_t *hdr, uint8_t type, uint64_t id,
                      const uint8_t payload[32], uint16_t plen) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->event_id = id;
    hdr->epoch = id; /* use id as tick for simplicity */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, payload, plen);
    sha256_final(&ctx, hdr->payload_hash);
}

TEST(test_init_produces_deterministic_root) {
    settlement_state_t a, b;
    settlement_state_init(&a);
    settlement_state_init(&b);

    uint8_t root_a[32], root_b[32];
    settlement_compute_root(&a, root_a);
    settlement_compute_root(&b, root_b);

    ASSERT(memcmp(root_a, root_b, 32) == 0);
}

TEST(test_claim_fragment_adds_owner) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t payload[112] = {0};
    memset(payload, 0x01, 32); /* fragment_pub */
    memset(payload + 32, 0x02, 32); /* winner_pubkey */
    memset(payload + 64, 0x03, 32); /* rock_pub */
    payload[96] = 2; /* grade */

    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 1, payload, sizeof(payload));

    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));
    ASSERT(s.fragment_owner_count == 1);
    ASSERT(memcmp(s.fragment_owners[0].fragment_pub, payload, 32) == 0);
    ASSERT(s.fragment_owners[0].grade == 2);
}

TEST(test_claim_fragment_rejects_duplicate) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t payload[112] = {0};
    memset(payload, 0x01, 32);

    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    chain_event_header_t hdr2;
    make_hdr(&hdr2, 0x01, 2, payload, sizeof(payload));
    ASSERT(!settlement_apply_event(&s, &hdr2, payload, sizeof(payload)));
}

TEST(test_smelt_ingot_adds_to_manifest) {
    settlement_state_t s;
    settlement_state_init(&s);

    /* First claim a fragment */
    uint8_t claim[112] = {0};
    memset(claim, 0xAA, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 1, claim, sizeof(claim));
    ASSERT(settlement_apply_event(&s, &hdr, claim, sizeof(claim)));

    /* Then smelt it */
    uint8_t smelt[80] = {0};
    memcpy(smelt, claim, 32); /* fragment_pub = same */
    memset(smelt + 32, 0xBB, 32); /* ingot_pub */
    smelt[64] = 1; /* prefix */

    chain_event_header_t hdr2;
    make_hdr(&hdr2, 0x02, 2, smelt, sizeof(smelt));
    ASSERT(settlement_apply_event(&s, &hdr2, smelt, sizeof(smelt)));

    ASSERT(s.fragment_owner_count == 0); /* consumed */
    ASSERT(s.station_manifest_counts[0] == 1);
    ASSERT(s.station_manifests[0][0].kind == CARGO_KIND_INGOT);
}

TEST(test_produce_output_consumes_inputs) {
    settlement_state_t s;
    settlement_state_init(&s);

    /* Add two input units to manifest manually */
    memset(s.station_manifests[0][0].pub, 0x11, 32);
    s.station_manifests[0][0].kind = CARGO_KIND_INGOT;
    s.station_manifests[0][0].quantity = 1;
    memset(s.station_manifests[0][1].pub, 0x22, 32);
    s.station_manifests[0][1].kind = CARGO_KIND_INGOT;
    s.station_manifests[0][1].quantity = 1;
    s.station_manifest_counts[0] = 2;

    uint8_t payload[12 + 2*32 + 1*32] = {0};
    payload[2] = 2; /* input_count */
    payload[3] = 1; /* output_count */
    memcpy(payload + 12, s.station_manifests[0][0].pub, 32);
    memcpy(payload + 12 + 32, s.station_manifests[0][1].pub, 32);
    memset(payload + 12 + 64, 0x33, 32); /* output pub */

    chain_event_header_t hdr;
    make_hdr(&hdr, 0x03, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    ASSERT(s.station_manifest_counts[0] == 1); /* 2 in - 2 consumed + 1 out */
}

TEST(test_unknown_event_rejected) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t payload[1] = {0};
    chain_event_header_t hdr;
    make_hdr(&hdr, 0xFF, 1, payload, 1);
    ASSERT(!settlement_apply_event(&s, &hdr, payload, 1));
}

TEST(test_segment_apply_produces_checkpoint) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t payload[112] = {0};
    memset(payload, 0x01, 32);

    chain_event_header_t hdrs[2];
    make_hdr(&hdrs[0], 0x01, 1, payload, sizeof(payload));
    make_hdr(&hdrs[1], 0x01, 2, payload, sizeof(payload));
    /* Different fragment_pub for second event to avoid duplicate */
    payload[0] = 0x02;
    make_hdr(&hdrs[1], 0x01, 2, payload, sizeof(payload));
    /* Actually, we need different payload for the second event hash */
    uint8_t payload2[112] = {0};
    memset(payload2, 0x02, 32);
    make_hdr(&hdrs[1], 0x01, 2, payload2, sizeof(payload2));

    const uint8_t *pls[2] = {payload, payload2};
    uint16_t lens[2] = {sizeof(payload), sizeof(payload2)};

    settlement_checkpoint_t cp;
    ASSERT(settlement_apply_segment(&s, hdrs, pls, lens, 2, &cp));
    ASSERT(cp.event_count == 2);
    ASSERT(s.segment_index == 1);
    ASSERT(s.fragment_owner_count == 2);
}

TEST(test_segment_rollback_on_failure_structured) {
    settlement_state_t s;
    settlement_state_init(&s);

    chain_event_header_t hdrs[2];
    uint8_t good[112] = {0};
    memset(good, 0xAA, 32);
    make_hdr(&hdrs[0], 0x01, 1, good, sizeof(good));

    uint8_t bad[1] = {0};
    make_hdr(&hdrs[1], 0xFF, 2, bad, 1);

    const uint8_t *pls[2] = {good, bad};
    uint16_t lens[2] = {sizeof(good), 1};

    settlement_checkpoint_t cp;
    ASSERT(!settlement_apply_segment(&s, hdrs, pls, lens, 2, &cp));
    /* State should be unchanged after rollback */
    ASSERT(s.fragment_owner_count == 0);
    ASSERT(s.segment_index == 0);
}

TEST(test_state_root_changes_after_event) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t root_before[32], root_after[32];
    settlement_compute_root(&s, root_before);

    uint8_t payload[112] = {0};
    memset(payload, 0x01, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    settlement_compute_root(&s, root_after);
    ASSERT(memcmp(root_before, root_after, 32) != 0);
}

TEST(test_buy_removes_from_manifest) {
    settlement_state_t s;
    settlement_state_init(&s);

    /* Add a unit to manifest */
    memset(s.station_manifests[0][0].pub, 0x42, 32);
    s.station_manifests[0][0].kind = CARGO_KIND_INGOT;
    s.station_manifests[0][0].quantity = 1;
    s.station_manifest_counts[0] = 1;

    /* Add a ledger entry for the player */
    memset(s.station_ledgers[0][0].player_pubkey, 0x99, 32);
    s.station_ledgers[0][0].balance = 100.0f;
    s.station_ledger_counts[0] = 1;

    uint8_t payload[112] = {0};
    memcpy(payload, s.station_manifests[0][0].pub, 32); /* cargo_pub */
    memset(payload + 32, 0x99, 32); /* player_pubkey */
    memset(payload + 64, 0xFF, 32); /* station_pubkey */
    int64_t delta = -50;
    memcpy(payload + 96, &delta, 8);
    payload[104] = 0; /* direction = BUY */

    chain_event_header_t hdr;
    make_hdr(&hdr, 0x10, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    ASSERT(s.station_manifest_counts[0] == 0); /* removed */
    ASSERT(s.station_ledgers[0][0].balance == 50.0f); /* debited */
}

TEST(test_issue_credit_note) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t payload[120] = {0};
    memset(payload, 0x11, 32); /* note_id */
    int64_t amount = 1000;
    memcpy(payload + 96, &amount, 8);

    chain_event_header_t hdr;
    make_hdr(&hdr, 0x12, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    ASSERT(s.credit_note_count == 1);
    ASSERT(s.credit_notes[0].amount == 1000);
    ASSERT(!s.credit_notes[0].redeemed);
}

TEST(test_redeem_credit_note) {
    settlement_state_t s;
    settlement_state_init(&s);

    /* Issue */
    uint8_t payload[120] = {0};
    memset(payload, 0x11, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x12, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    /* Redeem */
    uint8_t redeem[32];
    memset(redeem, 0x11, 32);
    chain_event_header_t hdr2;
    make_hdr(&hdr2, 0x13, 2, redeem, sizeof(redeem));
    ASSERT(settlement_apply_event(&s, &hdr2, redeem, sizeof(redeem)));

    ASSERT(s.credit_notes[0].redeemed);
}

TEST(test_double_redeem_rejected) {
    settlement_state_t s;
    settlement_state_init(&s);

    uint8_t payload[120] = {0};
    memset(payload, 0x11, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x12, 1, payload, sizeof(payload));
    ASSERT(settlement_apply_event(&s, &hdr, payload, sizeof(payload)));

    uint8_t redeem[32];
    memset(redeem, 0x11, 32);
    chain_event_header_t hdr2;
    make_hdr(&hdr2, 0x13, 2, redeem, sizeof(redeem));
    ASSERT(settlement_apply_event(&s, &hdr2, redeem, sizeof(redeem)));

    chain_event_header_t hdr3;
    make_hdr(&hdr3, 0x13, 3, redeem, sizeof(redeem));
    ASSERT(!settlement_apply_event(&s, &hdr3, redeem, sizeof(redeem)));
}


void register_settlement_engine_tests(void) {
    TEST_SECTION("\nSettlement engine:\n");
    RUN(test_init_produces_deterministic_root);
    RUN(test_claim_fragment_adds_owner);
    RUN(test_claim_fragment_rejects_duplicate);
    RUN(test_smelt_ingot_adds_to_manifest);
    RUN(test_produce_output_consumes_inputs);
    RUN(test_unknown_event_rejected);
    RUN(test_segment_apply_produces_checkpoint);
    RUN(test_segment_rollback_on_failure_structured);
    RUN(test_state_root_changes_after_event);
    RUN(test_buy_removes_from_manifest);
    RUN(test_issue_credit_note);
    RUN(test_redeem_credit_note);
    RUN(test_double_redeem_rejected);
}
