#include "test_harness.h"
#include "settlement_engine.h"
#include "../server/chain_log.h"
#include "sha256.h"
#include "signal_crypto.h"
#include <string.h>

/* Build a minimal event header */
static void make_hdr(chain_event_header_t *hdr, uint8_t type, uint64_t id,
                      const uint8_t *payload, uint16_t plen) {
    memset(hdr, 0, sizeof(*hdr));
    hdr->type = type;
    hdr->event_id = id;
    hdr->epoch = id; /* use id as tick for simplicity */
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, payload, plen);
    sha256_final(&ctx, hdr->payload_hash);
}

static void make_receipt_evidence(
    settlement_cargo_trust_evidence_t *evidence,
    cargo_receipt_t *receipt,
    const uint8_t cargo_pub[32],
    uint64_t event_id,
    uint8_t seed_tag,
    cargo_receipt_authority_trust_t authority_trust) {
    uint8_t seed[32];
    uint8_t secret[64];
    for (size_t i = 0; i < sizeof(seed); i++)
        seed[i] = (uint8_t)(seed_tag + i);

    memset(receipt, 0, sizeof(*receipt));
    signal_crypto_keypair_from_seed(
        seed, receipt->authoring_station, secret);
    memcpy(receipt->cargo_pub, cargo_pub, 32);
    memset(receipt->recipient_pubkey, (uint8_t)(seed_tag ^ 0x5au), 32);
    receipt->event_id = event_id;
    receipt->epoch = event_id + 100;
    sha256_bytes(cargo_pub, 32, receipt->prev_receipt_hash);
    uint8_t unsigned_receipt[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(receipt, unsigned_receipt);
    signal_crypto_sign(receipt->signature, unsigned_receipt,
                       sizeof(unsigned_receipt), secret);

    memset(evidence, 0, sizeof(*evidence));
    evidence->event_id = event_id;
    memcpy(evidence->cargo_pub, cargo_pub, 32);
    evidence->receipt_chain = receipt;
    evidence->receipt_count = 1;
    evidence->origin_present = true;
    evidence->origin.event_type = CARGO_RECEIPT_ORIGIN_EVENT_SMELT;
    evidence->origin.event_id = event_id - 1;
    evidence->origin.epoch = receipt->epoch - 1;
    memcpy(evidence->origin.event_hash,
           receipt->prev_receipt_hash, 32);
    memcpy(evidence->origin.output_cargo_pub, cargo_pub, 32);
    memcpy(evidence->origin.authority,
           receipt->authoring_station, 32);
    evidence->authority_trust = authority_trust;

    memset(secret, 0, sizeof(secret));
}

static void make_sell_payload(uint8_t payload[112],
                              const uint8_t cargo_pub[32],
                              uint8_t player_tag,
                              int64_t ledger_delta) {
    memset(payload, 0, 112);
    memcpy(payload, cargo_pub, 32);
    memset(payload + 32, player_tag, 32);
    memset(payload + 64, 0xf0u, 32);
    memcpy(payload + 96, &ledger_delta, sizeof(ledger_delta));
    payload[105] = 1; /* direction = SELL */
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
    uint8_t payload2[112] = {0};
    memset(payload2, 0x02, 32);

    chain_event_header_t hdrs[2];
    make_hdr(&hdrs[0], 0x01, 1, payload, sizeof(payload));
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

TEST(test_trusted_sell_accepts_current_and_rotated_deterministically) {
    static const cargo_receipt_authority_trust_t policies[] = {
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED,
    };

    for (size_t policy = 0; policy < sizeof(policies) / sizeof(policies[0]);
         policy++) {
        uint8_t cargo_pub[32];
        memset(cargo_pub, (uint8_t)(0x61 + policy), sizeof(cargo_pub));
        uint8_t payload[112];
        make_sell_payload(payload, cargo_pub, 0x91, 25);
        chain_event_header_t hdr;
        make_hdr(&hdr, 0x11, 41, payload, sizeof(payload));

        cargo_receipt_t receipt;
        settlement_cargo_trust_evidence_t cargo_evidence;
        make_receipt_evidence(&cargo_evidence, &receipt, cargo_pub,
                              hdr.event_id, (uint8_t)(0x11 + policy),
                              policies[policy]);
        settlement_event_trust_evidence_t event_evidence = {
            .cargo = &cargo_evidence,
            .cargo_count = 1,
        };
        const uint8_t *payloads[1] = {payload};
        uint16_t lengths[1] = {sizeof(payload)};

        settlement_state_t a, b;
        settlement_state_init(&a);
        settlement_state_init(&b);
        uint8_t previous_root[32];
        settlement_compute_root(&a, previous_root);
        settlement_checkpoint_t cp_a, cp_b;
        settlement_import_result_t result_a, result_b;
        ASSERT(settlement_apply_segment_trusted(
            &a, &hdr, payloads, lengths, &event_evidence, 1,
            &cp_a, &result_a));
        ASSERT(settlement_apply_segment_trusted(
            &b, &hdr, payloads, lengths, &event_evidence, 1,
            &cp_b, &result_b));

        ASSERT_EQ_INT(result_a.status, SETTL_IMPORT_OK);
        ASSERT_EQ_INT(result_b.status, SETTL_IMPORT_OK);
        ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
        ASSERT(memcmp(&cp_a, &cp_b, sizeof(cp_a)) == 0);
        ASSERT(memcmp(cp_a.prev_segment_root, previous_root, 32) == 0);
        ASSERT_EQ_INT(a.station_manifest_counts[0], 1);
        ASSERT_EQ_INT(a.station_ledger_counts[0], 1);
        ASSERT_EQ_FLOAT(a.station_ledgers[0][0].balance, 25.0f, 0.001f);

        uint8_t root_before_receipt_destroy[32];
        uint8_t root_after_receipt_destroy[32];
        settlement_compute_root(&a, root_before_receipt_destroy);
        memset(&receipt, 0, sizeof(receipt));
        memset(&cargo_evidence, 0, sizeof(cargo_evidence));
        settlement_compute_root(&a, root_after_receipt_destroy);
        ASSERT(memcmp(root_before_receipt_destroy,
                      root_after_receipt_destroy, 32) == 0);
    }
}

TEST(test_trust_rejections_are_semantic_and_byte_inert) {
    typedef enum {
        FAILURE_MISSING_EVIDENCE,
        FAILURE_CARGO_MISMATCH,
        FAILURE_MISSING_ORIGIN,
        FAILURE_UNKNOWN,
        FAILURE_UNTRUSTED,
        FAILURE_REVOKED,
        FAILURE_BAD_SIGNATURE,
    } failure_kind_t;
    static const struct {
        failure_kind_t kind;
        settlement_import_status_t expected;
        cargo_receipt_result_t chain_result;
    } cases[] = {
        {FAILURE_MISSING_EVIDENCE,
         SETTL_IMPORT_REJECT_MISSING_EVIDENCE, CARGO_RECEIPT_OK},
        {FAILURE_CARGO_MISMATCH,
         SETTL_IMPORT_REJECT_EVIDENCE_CARGO, CARGO_RECEIPT_OK},
        {FAILURE_MISSING_ORIGIN,
         SETTL_IMPORT_REJECT_TRUST_MISSING_ORIGIN, CARGO_RECEIPT_OK},
        {FAILURE_UNKNOWN,
         SETTL_IMPORT_REJECT_TRUST_UNKNOWN_AUTHORITY, CARGO_RECEIPT_OK},
        {FAILURE_UNTRUSTED,
         SETTL_IMPORT_REJECT_TRUST_UNTRUSTED_AUTHORITY, CARGO_RECEIPT_OK},
        {FAILURE_REVOKED,
         SETTL_IMPORT_REJECT_TRUST_REVOKED_AUTHORITY, CARGO_RECEIPT_OK},
        {FAILURE_BAD_SIGNATURE,
         SETTL_IMPORT_REJECT_TRUST_CHAIN,
         CARGO_RECEIPT_REJECT_BAD_SIGNATURE},
    };

    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x72, sizeof(cargo_pub));
    uint8_t payload[112];
    make_sell_payload(payload, cargo_pub, 0x92, 30);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x11, 52, payload, sizeof(payload));
    const uint8_t *payloads[1] = {payload};
    uint16_t lengths[1] = {sizeof(payload)};

    settlement_state_t baseline;
    settlement_state_init(&baseline);
    baseline.segment_index = 8;
    baseline.last_event_id = 51;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cargo_receipt_t receipt;
        settlement_cargo_trust_evidence_t cargo_evidence;
        make_receipt_evidence(
            &cargo_evidence, &receipt, cargo_pub, hdr.event_id,
            (uint8_t)(0x30 + i),
            CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
        settlement_event_trust_evidence_t event_evidence = {
            .cargo = &cargo_evidence,
            .cargo_count = 1,
        };
        const settlement_event_trust_evidence_t *evidence_ptr =
            &event_evidence;
        switch (cases[i].kind) {
        case FAILURE_MISSING_EVIDENCE:
            evidence_ptr = NULL;
            break;
        case FAILURE_CARGO_MISMATCH:
            cargo_evidence.cargo_pub[0] ^= 0x80u;
            break;
        case FAILURE_MISSING_ORIGIN:
            cargo_evidence.origin_present = false;
            break;
        case FAILURE_UNKNOWN:
            cargo_evidence.authority_trust =
                CARGO_RECEIPT_AUTHORITY_UNKNOWN;
            break;
        case FAILURE_UNTRUSTED:
            cargo_evidence.authority_trust =
                CARGO_RECEIPT_AUTHORITY_UNTRUSTED;
            break;
        case FAILURE_REVOKED:
            cargo_evidence.authority_trust =
                CARGO_RECEIPT_AUTHORITY_REVOKED;
            break;
        case FAILURE_BAD_SIGNATURE:
            receipt.signature[0] ^= 0x01u;
            break;
        }

        settlement_state_t state = baseline;
        settlement_checkpoint_t checkpoint;
        memset(&checkpoint, 0xa5, sizeof(checkpoint));
        settlement_checkpoint_t checkpoint_before = checkpoint;
        settlement_import_result_t result;
        ASSERT(!settlement_apply_segment_trusted(
            &state, &hdr, payloads, lengths, evidence_ptr, 1,
            &checkpoint, &result));
        ASSERT_EQ_INT(result.status, cases[i].expected);
        ASSERT_EQ_INT(result.chain_result, cases[i].chain_result);
        ASSERT_EQ_INT((int)result.event_index, 0);
        ASSERT(memcmp(&state, &baseline, sizeof(state)) == 0);
        ASSERT(memcmp(&checkpoint, &checkpoint_before,
                      sizeof(checkpoint)) == 0);
    }
    ASSERT_STR_EQ(settlement_import_status_name(
                      SETTL_IMPORT_REJECT_TRUST_REVOKED_AUTHORITY),
                  "reject_trust_revoked_authority");
}

TEST(test_later_invalid_trust_event_prevents_earlier_commit) {
    uint8_t cargo_a[32], cargo_b[32];
    memset(cargo_a, 0x81, sizeof(cargo_a));
    memset(cargo_b, 0x82, sizeof(cargo_b));
    uint8_t payload_a[112], payload_b[112];
    make_sell_payload(payload_a, cargo_a, 0xa1, 10);
    make_sell_payload(payload_b, cargo_b, 0xa2, 20);
    chain_event_header_t hdrs[2];
    make_hdr(&hdrs[0], 0x11, 61, payload_a, sizeof(payload_a));
    make_hdr(&hdrs[1], 0x11, 62, payload_b, sizeof(payload_b));

    cargo_receipt_t receipts[2];
    settlement_cargo_trust_evidence_t cargo_evidence[2];
    make_receipt_evidence(&cargo_evidence[0], &receipts[0], cargo_a, 61,
                          0x41, CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    make_receipt_evidence(&cargo_evidence[1], &receipts[1], cargo_b, 62,
                          0x42, CARGO_RECEIPT_AUTHORITY_REVOKED);
    settlement_event_trust_evidence_t event_evidence[2] = {
        {.cargo = &cargo_evidence[0], .cargo_count = 1},
        {.cargo = &cargo_evidence[1], .cargo_count = 1},
    };
    const uint8_t *payloads[2] = {payload_a, payload_b};
    uint16_t lengths[2] = {sizeof(payload_a), sizeof(payload_b)};

    settlement_state_t state;
    settlement_state_init(&state);
    state.segment_index = 4;
    state.last_event_id = 60;
    settlement_state_t before = state;
    settlement_checkpoint_t checkpoint;
    memset(&checkpoint, 0x6c, sizeof(checkpoint));
    settlement_checkpoint_t checkpoint_before = checkpoint;
    settlement_import_result_t result;
    ASSERT(!settlement_apply_segment_trusted(
        &state, hdrs, payloads, lengths, event_evidence, 2,
        &checkpoint, &result));
    ASSERT_EQ_INT(result.status,
                  SETTL_IMPORT_REJECT_TRUST_REVOKED_AUTHORITY);
    ASSERT_EQ_INT((int)result.event_index, 1);
    ASSERT(memcmp(&state, &before, sizeof(state)) == 0);
    ASSERT(memcmp(&checkpoint, &checkpoint_before,
                  sizeof(checkpoint)) == 0);
}

TEST(test_payload_hash_rejection_preserves_state_and_checkpoint) {
    uint8_t payload[112] = {0};
    memset(payload, 0x31, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 71, payload, sizeof(payload));
    payload[80] ^= 0x01u;
    const uint8_t *payloads[1] = {payload};
    uint16_t lengths[1] = {sizeof(payload)};

    settlement_state_t state;
    settlement_state_init(&state);
    state.segment_index = 3;
    state.last_event_id = 70;
    settlement_state_t before = state;
    settlement_checkpoint_t checkpoint;
    memset(&checkpoint, 0x7d, sizeof(checkpoint));
    settlement_checkpoint_t checkpoint_before = checkpoint;
    settlement_import_result_t result;
    ASSERT(!settlement_apply_segment_trusted(
        &state, &hdr, payloads, lengths, NULL, 1,
        &checkpoint, &result));
    ASSERT_EQ_INT(result.status, SETTL_IMPORT_REJECT_PAYLOAD_HASH);
    ASSERT(memcmp(&state, &before, sizeof(state)) == 0);
    ASSERT(memcmp(&checkpoint, &checkpoint_before,
                  sizeof(checkpoint)) == 0);
}

TEST(test_construction_input_consumes_only_receipt_backed_units) {
    settlement_state_t state;
    settlement_state_init(&state);
    state.construction_site_count = 1;
    memset(state.construction_sites[0].scaffold_id, 0x51, 32);
    memset(state.construction_sites[0].station_pubkey, 0x52, 32);
    state.construction_sites[0].active = true;
    state.station_manifest_counts[0] = 2;
    memset(state.station_manifests[0][0].pub, 0xb1, 32);
    memset(state.station_manifests[0][1].pub, 0xb2, 32);
    state.station_manifests[0][0].quantity = 1;
    state.station_manifests[0][1].quantity = 1;

    uint8_t payload[168] = {0};
    memcpy(payload, state.construction_sites[0].scaffold_id, 32);
    memcpy(payload + 32, state.construction_sites[0].station_pubkey, 32);
    memcpy(payload + 64, state.station_manifests[0][0].pub, 32);
    memcpy(payload + 96, state.station_manifests[0][1].pub, 32);
    payload[160] = 2;
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x21, 81, payload, sizeof(payload));

    cargo_receipt_t receipts[2];
    settlement_cargo_trust_evidence_t cargo_evidence[2];
    make_receipt_evidence(
        &cargo_evidence[0], &receipts[0], payload + 64, hdr.event_id,
        0x61, CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    make_receipt_evidence(
        &cargo_evidence[1], &receipts[1], payload + 96, hdr.event_id,
        0x62, CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    settlement_event_trust_evidence_t event_evidence = {
        .cargo = cargo_evidence,
        .cargo_count = 2,
    };
    const uint8_t *payloads[1] = {payload};
    uint16_t lengths[1] = {sizeof(payload)};
    settlement_checkpoint_t checkpoint;
    settlement_import_result_t result;
    ASSERT(settlement_apply_segment_trusted(
        &state, &hdr, payloads, lengths, &event_evidence, 1,
        &checkpoint, &result));
    ASSERT_EQ_INT(result.status, SETTL_IMPORT_OK);
    ASSERT_EQ_INT(state.station_manifest_counts[0], 0);
    ASSERT_EQ_FLOAT(
        state.construction_sites[0].build_progress, 0.2f, 0.001f);
}

TEST(test_all_provenance_event_types_require_evidence) {
    static const struct {
        uint8_t type;
        uint16_t length;
    } cases[] = {
        {0x04, 144},
        {0x11, 112},
        {0x21, 168},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t payload[168] = {0};
        if (cases[i].type == 0x21) payload[160] = 1;
        chain_event_header_t hdr;
        make_hdr(&hdr, cases[i].type, (uint64_t)(91 + i),
                 payload, cases[i].length);
        const uint8_t *payloads[1] = {payload};
        uint16_t lengths[1] = {cases[i].length};
        settlement_state_t state;
        settlement_state_init(&state);
        settlement_state_t before = state;
        ASSERT(!settlement_apply_event(
            &state, &hdr, payload, cases[i].length));
        ASSERT(memcmp(&state, &before, sizeof(state)) == 0);
        settlement_checkpoint_t checkpoint;
        memset(&checkpoint, 0x8e, sizeof(checkpoint));
        settlement_checkpoint_t checkpoint_before = checkpoint;
        settlement_import_result_t result;
        ASSERT(!settlement_apply_segment_trusted(
            &state, &hdr, payloads, lengths, NULL, 1,
            &checkpoint, &result));
        ASSERT_EQ_INT(result.status, SETTL_IMPORT_REJECT_MISSING_EVIDENCE);
        ASSERT(memcmp(&state, &before, sizeof(state)) == 0);
        ASSERT(memcmp(&checkpoint, &checkpoint_before,
                      sizeof(checkpoint)) == 0);
    }
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
    RUN(test_trusted_sell_accepts_current_and_rotated_deterministically);
    RUN(test_trust_rejections_are_semantic_and_byte_inert);
    RUN(test_later_invalid_trust_event_prevents_earlier_commit);
    RUN(test_payload_hash_rejection_preserves_state_and_checkpoint);
    RUN(test_construction_input_consumes_only_receipt_backed_units);
    RUN(test_all_provenance_event_types_require_evidence);
}
