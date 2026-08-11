#include "test_harness.h"
#include "settlement_engine.h"
#include "../server/chain_log.h"
#include "sha256.h"
#include "signal_crypto.h"
#include "signal_memzero.h"
#include <stdlib.h>
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

typedef struct {
    cargo_receipt_t receipt;
    cargo_receipt_origin_proof_t origin;
    settlement_cargo_trust_evidence_t cargo;
    settlement_event_trust_evidence_t event;
} settlement_trust_fixture_t;

static bool make_trust_fixture(
    const uint8_t cargo_pub[32],
    uint8_t cargo_kind,
    cargo_receipt_authority_lifecycle_t lifecycle,
    cargo_receipt_authority_trust_t authority_trust,
    settlement_trust_fixture_t *fixture) {
    if (!cargo_pub || !fixture) return false;
    memset(fixture, 0, sizeof(*fixture));

    uint8_t seed[32];
    uint8_t station_pub[32];
    uint8_t station_secret[64];
    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (size_t i = 0; i < sizeof(seed); i++) {
        seed[i] = (uint8_t)(0x31u + i);
        recipient[i] = (uint8_t)(0x71u + i);
        origin_pin[i] = (uint8_t)(0xB1u + i);
    }
    signal_crypto_keypair_from_seed(
        seed, station_pub, station_secret);

    cargo_receipt_t *receipt = &fixture->receipt;
    memcpy(receipt->cargo_pub, cargo_pub, 32);
    memcpy(receipt->authoring_station, station_pub, 32);
    memcpy(receipt->recipient_pubkey, recipient, 32);
    receipt->event_id = 41;
    receipt->epoch = 640;
    memcpy(receipt->prev_receipt_hash, origin_pin, 32);
    uint8_t unsigned_receipt[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(
        receipt, unsigned_receipt);
    signal_crypto_sign(
        receipt->signature, unsigned_receipt,
        sizeof(unsigned_receipt), station_secret);

    cargo_receipt_origin_proof_t *origin =
        &fixture->origin;
    origin->event_type =
        cargo_kind == CARGO_KIND_INGOT
            ? CARGO_RECEIPT_ORIGIN_EVENT_SMELT
            : CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
    origin->authority_lifecycle = lifecycle;
    origin->event_id = 17;
    origin->epoch = 600;
    if (origin->event_type ==
            CARGO_RECEIPT_ORIGIN_EVENT_CRAFT) {
        origin->craft_recipe_id = 7;
        origin->craft_input_count = 2;
    }
    memcpy(origin->event_hash, origin_pin, 32);
    memcpy(origin->output_cargo_pub, cargo_pub, 32);
    origin->output_semantics_version =
        CARGO_RECEIPT_ORIGIN_SEMANTICS_V1;
    memcpy(origin->output_cargo.pub, cargo_pub, 32);
    origin->output_cargo.kind = cargo_kind;
    origin->output_cargo.commodity =
        cargo_kind == CARGO_KIND_INGOT
            ? COMMODITY_FERRITE_INGOT
            : COMMODITY_FRAME;
    origin->output_cargo.quantity = 1;
    origin->output_cargo.mined_block = 600;
    memcpy(origin->authority, station_pub, 32);

    fixture->cargo.receipt_chain = receipt;
    fixture->cargo.receipt_count = 1;
    fixture->cargo.origin = origin;
    fixture->cargo.authority_trust =
        authority_trust;
    fixture->event.cargo = &fixture->cargo;
    fixture->event.cargo_count = 1;

    cargo_receipt_trust_result_t verified =
        cargo_receipt_trust_verify(
            receipt, 1, cargo_pub, origin,
            authority_trust);
    signal_memzero_explicit(
        station_secret, sizeof(station_secret));
    signal_memzero_explicit(seed, sizeof(seed));
    return verified.status ==
               CARGO_RECEIPT_TRUST_VALID_TRUSTED ||
           verified.status ==
               CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED;
}

static void make_trade_payload(
    uint8_t payload[112],
    const uint8_t cargo_pub[32],
    uint8_t cargo_kind,
    uint8_t direction,
    int64_t ledger_delta) {
    memset(payload, 0, 112);
    memcpy(payload, cargo_pub, 32);
    for (size_t i = 0; i < 32; i++)
        payload[32 + i] = (uint8_t)(0x71u + i);
    memset(payload + 64, 0xA2, 32);
    memcpy(payload + 96, &ledger_delta,
           sizeof(ledger_delta));
    payload[104] = cargo_kind;
    payload[105] = direction;
}

static bool rejected_segment_preserves_bytes(
    const settlement_state_t *baseline,
    const chain_event_header_t *events,
    const uint8_t **payloads,
    const uint16_t *payload_lens,
    const settlement_event_trust_evidence_t *evidence,
    uint32_t event_count,
    const settlement_checkpoint_t *checkpoint_sentinel,
    settlement_apply_status_t expected_status,
    cargo_receipt_trust_status_t expected_trust_status,
    uint32_t expected_event,
    uint8_t expected_cargo) {
    settlement_state_t *candidate =
        (settlement_state_t *)malloc(sizeof(*candidate));
    if (!candidate) return false;
    *candidate = *baseline;
    settlement_checkpoint_t checkpoint =
        *checkpoint_sentinel;
    settlement_apply_result_t result;
    memset(&result, 0xC3, sizeof(result));
    if (settlement_apply_segment_trusted(
            candidate, events, payloads, payload_lens,
            evidence, event_count, &checkpoint, &result)) {
        free(candidate);
        return false;
    }
    if (memcmp(candidate, baseline,
               sizeof(*candidate)) != 0 ||
        memcmp(&checkpoint, checkpoint_sentinel,
               sizeof(checkpoint)) != 0 ||
        result.status != expected_status ||
        result.event_index != expected_event ||
        result.cargo_index != expected_cargo) {
        free(candidate);
        return false;
    }
    if (expected_status ==
            SETTLEMENT_APPLY_REJECT_CARGO_TRUST &&
        result.cargo_trust.status !=
            expected_trust_status) {
        free(candidate);
        return false;
    }
    free(candidate);
    return true;
}

static bool rejected_segment_preserves_trust_detail(
    const settlement_state_t *baseline,
    const chain_event_header_t *event,
    const uint8_t *payload,
    uint16_t payload_len,
    const settlement_event_trust_evidence_t *evidence,
    const settlement_checkpoint_t *checkpoint_sentinel,
    cargo_receipt_trust_status_t expected_trust,
    cargo_receipt_result_t expected_chain) {
    settlement_state_t *candidate =
        (settlement_state_t *)malloc(sizeof(*candidate));
    if (!candidate) return false;
    *candidate = *baseline;
    settlement_checkpoint_t checkpoint =
        *checkpoint_sentinel;
    settlement_apply_result_t result;
    const uint8_t *payloads[1] = {payload};
    uint16_t payload_lens[1] = {payload_len};
    bool accepted = settlement_apply_segment_trusted(
        candidate, event, payloads, payload_lens,
        evidence, 1, &checkpoint, &result);
    bool valid = !accepted &&
        memcmp(candidate, baseline,
               sizeof(*candidate)) == 0 &&
        memcmp(&checkpoint, checkpoint_sentinel,
               sizeof(checkpoint)) == 0 &&
        result.status ==
            SETTLEMENT_APPLY_REJECT_CARGO_TRUST &&
        result.event_index == 0 &&
        result.cargo_index == 0 &&
        result.cargo_trust.status == expected_trust &&
        result.cargo_trust.chain_result ==
            expected_chain;
    free(candidate);
    return valid;
}

TEST(test_init_produces_deterministic_root) {
    settlement_state_t *a =
        (settlement_state_t *)malloc(sizeof(*a));
    settlement_state_t *b =
        (settlement_state_t *)malloc(sizeof(*b));
    ASSERT(a != NULL);
    ASSERT(b != NULL);
    settlement_state_init(a);
    settlement_state_init(b);

    uint8_t root_a[32], root_b[32];
    settlement_compute_root(a, root_a);
    settlement_compute_root(b, root_b);

    ASSERT(memcmp(root_a, root_b, 32) == 0);
    free(a);
    free(b);
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
    payload[104] = CARGO_KIND_INGOT;
    payload[105] = 0; /* direction = BUY */

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

TEST(test_manifest_root_sorts_by_complete_cargo_pubkey) {
    settlement_state_t *a =
        (settlement_state_t *)malloc(sizeof(*a));
    settlement_state_t *b =
        (settlement_state_t *)malloc(sizeof(*b));
    ASSERT(a != NULL);
    ASSERT(b != NULL);
    settlement_state_init(a);
    settlement_state_init(b);

    cargo_unit_t low = {0};
    cargo_unit_t high = {0};
    low.kind = high.kind = CARGO_KIND_INGOT;
    low.commodity = high.commodity =
        COMMODITY_FERRITE_INGOT;
    low.quantity = high.quantity = 1;
    memset(low.pub, 0x44, 16);
    memset(high.pub, 0x44, 16);
    low.pub[31] = 0x10;
    high.pub[31] = 0x20;

    a->station_manifests[0][0] = low;
    a->station_manifests[0][1] = high;
    b->station_manifests[0][0] = high;
    b->station_manifests[0][1] = low;
    a->station_manifest_counts[0] = 2;
    b->station_manifest_counts[0] = 2;

    uint8_t root_a[32];
    uint8_t root_b[32];
    settlement_compute_root(a, root_a);
    settlement_compute_root(b, root_b);
    ASSERT(memcmp(root_a, root_b, 32) == 0);

    settlement_state_init(a);
    settlement_state_init(b);
    settl_construction_site_t site_low = {0};
    settl_construction_site_t site_high = {0};
    memset(site_low.scaffold_id, 0x55, 32);
    memset(site_high.scaffold_id, 0x55, 32);
    site_low.station_pubkey[31] = 0x10;
    site_high.station_pubkey[31] = 0x20;
    site_low.active = site_high.active = true;
    a->construction_sites[0] = site_low;
    a->construction_sites[1] = site_high;
    b->construction_sites[0] = site_high;
    b->construction_sites[1] = site_low;
    a->construction_site_count = 2;
    b->construction_site_count = 2;
    settlement_compute_root(a, root_a);
    settlement_compute_root(b, root_b);
    ASSERT(memcmp(root_a, root_b, 32) == 0);

    settlement_state_init(a);
    settlement_state_init(b);
    settl_death_record_t death_early = {0};
    settl_death_record_t death_late = {0};
    memset(death_early.player_pubkey, 0x67, 32);
    memset(death_late.player_pubkey, 0x67, 32);
    death_early.death_tick = 100;
    death_late.death_tick = 200;
    a->death_records[0] = death_early;
    a->death_records[1] = death_late;
    b->death_records[0] = death_late;
    b->death_records[1] = death_early;
    a->death_record_count = 2;
    b->death_record_count = 2;
    settlement_compute_root(a, root_a);
    settlement_compute_root(b, root_b);
    ASSERT(memcmp(root_a, root_b, 32) == 0);
    free(a);
    free(b);
}

TEST(test_settlement_payload_hash_rejection_is_inert) {
    settlement_state_t baseline;
    settlement_state_init(&baseline);
    baseline.segment_index = 7;
    baseline.last_event_id = 29;

    uint8_t payload[112] = {0};
    memset(payload, 0x3A, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 30, payload, sizeof(payload));
    payload[111] ^= 0x01;

    settlement_state_t direct = baseline;
    ASSERT(!settlement_apply_event(
        &direct, &hdr, payload, sizeof(payload)));
    ASSERT(memcmp(&direct, &baseline,
                  sizeof(direct)) == 0);

    const uint8_t *payloads[1] = {payload};
    uint16_t lens[1] = {sizeof(payload)};
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0xA5, sizeof(sentinel));
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens, NULL, 1,
        &sentinel,
        SETTLEMENT_APPLY_REJECT_PAYLOAD_HASH,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));
}

TEST(test_sensitive_events_fail_closed_without_evidence) {
    settlement_state_t baseline;
    settlement_state_init(&baseline);

    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x61, sizeof(cargo_pub));
    uint8_t sell[112];
    make_trade_payload(
        sell, cargo_pub, CARGO_KIND_INGOT, 1, 35);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x11, 1, sell, sizeof(sell));

    settlement_state_t candidate = baseline;
    ASSERT(!settlement_apply_event(
        &candidate, &hdr, sell, sizeof(sell)));
    ASSERT(memcmp(&candidate, &baseline,
                  sizeof(candidate)) == 0);

    const uint8_t *payloads[1] = {sell};
    uint16_t lens[1] = {sizeof(sell)};
    settlement_checkpoint_t checkpoint;
    memset(&checkpoint, 0xB6, sizeof(checkpoint));
    settlement_checkpoint_t before = checkpoint;
    candidate = baseline;
    ASSERT(!settlement_apply_segment(
        &candidate, &hdr, payloads, lens, 1,
        &checkpoint));
    ASSERT(memcmp(&candidate, &baseline,
                  sizeof(candidate)) == 0);
    ASSERT(memcmp(&checkpoint, &before,
                  sizeof(checkpoint)) == 0);

    uint8_t transfer[144] = {0};
    memcpy(transfer, cargo_pub, 32);
    memset(transfer + 32, 0x72, 32);
    transfer[96] = CARGO_KIND_INGOT;
    make_hdr(&hdr, 0x04, 2,
             transfer, sizeof(transfer));
    const uint8_t *transfer_payloads[1] = {
        transfer,
    };
    uint16_t transfer_lens[1] = {
        sizeof(transfer),
    };
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, transfer_payloads,
        transfer_lens, NULL, 1, &before,
        SETTLEMENT_APPLY_REJECT_MISSING_TRUST_EVIDENCE,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    uint8_t construction[168] = {0};
    memcpy(construction + 64, cargo_pub, 32);
    construction[160] = 1;
    make_hdr(&hdr, 0x21, 3,
             construction, sizeof(construction));
    const uint8_t *construction_payloads[1] = {
        construction,
    };
    uint16_t construction_lens[1] = {
        sizeof(construction),
    };
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, construction_payloads,
        construction_lens, NULL, 1, &before,
        SETTLEMENT_APPLY_REJECT_MISSING_TRUST_EVIDENCE,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));
}

TEST(test_trusted_current_and_rotated_sell_import) {
    const cargo_receipt_authority_lifecycle_t lifecycles[2] = {
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED,
    };
    const cargo_receipt_authority_trust_t policies[2] = {
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED,
    };

    for (int mode = 0; mode < 2; mode++) {
        uint8_t cargo_pub[32];
        memset(cargo_pub, (uint8_t)(0x70 + mode),
               sizeof(cargo_pub));
        settlement_trust_fixture_t fixture;
        ASSERT(make_trust_fixture(
            cargo_pub, CARGO_KIND_INGOT,
            lifecycles[mode], policies[mode],
            &fixture));

        uint8_t sell[112];
        make_trade_payload(
            sell, cargo_pub, CARGO_KIND_INGOT,
            1, 37);
        chain_event_header_t hdr;
        make_hdr(&hdr, 0x11, 9, sell, sizeof(sell));
        const uint8_t *payloads[1] = {sell};
        uint16_t lens[1] = {sizeof(sell)};

        settlement_state_t state;
        settlement_state_init(&state);
        uint8_t root_before[32];
        settlement_compute_root(&state, root_before);
        settlement_checkpoint_t checkpoint;
        memset(&checkpoint, 0xCC, sizeof(checkpoint));
        settlement_apply_result_t result;
        ASSERT(settlement_apply_segment_trusted(
            &state, &hdr, payloads, lens,
            &fixture.event, 1, &checkpoint,
            &result));
        ASSERT_EQ_INT(result.status,
                      SETTLEMENT_APPLY_OK);
        ASSERT_EQ_INT(state.segment_index, 1);
        ASSERT_EQ_INT(state.last_event_id, 9);
        ASSERT_EQ_INT(
            state.station_manifest_counts[0], 1);
        ASSERT(memcmp(
            &state.station_manifests[0][0],
            &fixture.origin.output_cargo,
            sizeof(cargo_unit_t)) == 0);
        ASSERT_EQ_INT(state.station_ledger_counts[0], 1);
        ASSERT_EQ_FLOAT(
            state.station_ledgers[0][0].balance,
            37.0f, 0.001f);
        ASSERT(memcmp(
            checkpoint.prev_segment_root,
            root_before, 32) == 0);

        uint8_t state_root[32];
        settlement_compute_root(&state, state_root);
        ASSERT(memcmp(
            checkpoint.state_root,
            state_root, 32) == 0);

        /*
         * The borrowed receipt/origin storage may disappear immediately
         * after the call; settlement state and its root remain self-contained.
         */
        memset(&fixture, 0, sizeof(fixture));
        uint8_t root_after_evidence_wipe[32];
        settlement_compute_root(
            &state, root_after_evidence_wipe);
        ASSERT(memcmp(
            state_root, root_after_evidence_wipe,
            32) == 0);
    }
}

TEST(test_trust_rejections_are_stable_and_byte_inert) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x82, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));

    uint8_t sell[112];
    make_trade_payload(
        sell, cargo_pub, CARGO_KIND_INGOT, 1, 19);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x11, 12, sell, sizeof(sell));
    const uint8_t *payloads[1] = {sell};
    uint16_t lens[1] = {sizeof(sell)};
    settlement_state_t baseline;
    settlement_state_init(&baseline);
    baseline.segment_index = 4;
    baseline.last_event_id = 11;
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0xD7, sizeof(sentinel));

    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens, NULL, 1,
        &sentinel,
        SETTLEMENT_APPLY_REJECT_MISSING_TRUST_EVIDENCE,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    settlement_event_trust_evidence_t wrong_count =
        fixture.event;
    wrong_count.cargo_count = 2;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &wrong_count, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_TRUST_EVIDENCE_COUNT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    sell[32] ^= 0x01;
    make_hdr(&hdr, 0x11, 12, sell, sizeof(sell));
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_RECEIPT_HOLDER,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, 0));
    sell[32] ^= 0x01;
    make_hdr(&hdr, 0x11, 12, sell, sizeof(sell));

    const cargo_receipt_origin_proof_t *saved_origin =
        fixture.cargo.origin;
    fixture.cargo.origin = NULL;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
        CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN,
        0, 0));
    fixture.cargo.origin = saved_origin;

    fixture.origin.output_cargo_pub[0] ^= 0x01;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO,
        0, 0));
    fixture.origin.output_cargo_pub[0] ^= 0x01;

    fixture.cargo.authority_trust =
        CARGO_RECEIPT_AUTHORITY_UNKNOWN;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
        CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY,
        0, 0));

    fixture.cargo.authority_trust =
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
        CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY,
        0, 0));

    fixture.cargo.authority_trust =
        CARGO_RECEIPT_AUTHORITY_REVOKED;
    fixture.origin.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
        CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY,
        0, 0));

    fixture.origin.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT;
    fixture.cargo.authority_trust =
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT;
    fixture.origin.output_semantics_version =
        CARGO_RECEIPT_ORIGIN_SEMANTICS_UNBOUND;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_CARGO_TRUST,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA,
        0, 0));
}

TEST(test_trusted_transfer_and_construction_consume_exact_cargo) {
    uint8_t transfer_pub[32];
    memset(transfer_pub, 0x91, sizeof(transfer_pub));
    settlement_trust_fixture_t transfer_fixture;
    ASSERT(make_trust_fixture(
        transfer_pub, CARGO_KIND_FRAME,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &transfer_fixture));

    settlement_state_t transfer_state;
    settlement_state_init(&transfer_state);
    transfer_state.station_manifests[0][0] =
        transfer_fixture.origin.output_cargo;
    transfer_state.station_manifest_counts[0] = 1;
    uint8_t transfer[144] = {0};
    memcpy(transfer, transfer_pub, 32);
    memcpy(transfer + 32,
           transfer_fixture.receipt.recipient_pubkey,
           32);
    memset(transfer + 64, 0x33, 32);
    transfer[96] = CARGO_KIND_FRAME;
    memset(transfer + 97, 0x44, 32);
    transfer[129] = 1;
    chain_event_header_t transfer_hdr;
    make_hdr(&transfer_hdr, 0x04, 1,
             transfer, sizeof(transfer));
    const uint8_t *transfer_payloads[1] = {
        transfer,
    };
    uint16_t transfer_lens[1] = {
        sizeof(transfer),
    };
    settlement_checkpoint_t transfer_cp;
    ASSERT(settlement_apply_segment_trusted(
        &transfer_state, &transfer_hdr,
        transfer_payloads, transfer_lens,
        &transfer_fixture.event, 1,
        &transfer_cp, NULL));
    ASSERT_EQ_INT(
        transfer_state.station_manifest_counts[0], 1);
    ASSERT(memcmp(
        transfer_state.station_manifests[0][0].pub,
        transfer_pub, 32) == 0);

    uint8_t input_a[32];
    uint8_t input_b[32];
    uint8_t input_c[32];
    memset(input_a, 0xA1, sizeof(input_a));
    memset(input_b, 0xB2, sizeof(input_b));
    memset(input_c, 0xC4, sizeof(input_c));
    settlement_trust_fixture_t fixture_a;
    settlement_trust_fixture_t fixture_b;
    settlement_trust_fixture_t fixture_c;
    ASSERT(make_trust_fixture(
        input_a, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture_a));
    ASSERT(make_trust_fixture(
        input_b, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture_b));
    ASSERT(make_trust_fixture(
        input_c, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture_c));

    settlement_state_init(&transfer_state);
    memset(
        transfer_state.construction_sites[0].scaffold_id,
        0xC3, 32);
    memset(
        transfer_state.construction_sites[0].station_pubkey,
        0, 32);
    memcpy(
        transfer_state.construction_sites[0].station_pubkey,
        fixture_a.receipt.recipient_pubkey, 32);
    transfer_state.construction_sites[0].active = true;
    transfer_state.construction_site_count = 1;
    transfer_state.station_manifests[0][0] =
        fixture_a.origin.output_cargo;
    transfer_state.station_manifests[0][1] =
        fixture_b.origin.output_cargo;
    transfer_state.station_manifests[0][2] =
        fixture_c.origin.output_cargo;
    transfer_state.station_manifest_counts[0] = 3;

    uint8_t construction[168] = {0};
    memset(construction, 0xC3, 32);
    memcpy(construction + 32,
           fixture_a.receipt.recipient_pubkey,
           32);
    memcpy(construction + 64, input_a, 32);
    memcpy(construction + 96, input_b, 32);
    memcpy(construction + 128, input_c, 32);
    construction[160] = 3;
    construction[161] = 3;
    construction[162] = 1;
    construction[163] = 2;
    chain_event_header_t construction_hdr;
    make_hdr(&construction_hdr, 0x21, 2,
             construction, sizeof(construction));
    const uint8_t *construction_payloads[1] = {
        construction,
    };
    uint16_t construction_lens[1] = {
        sizeof(construction),
    };
    settlement_cargo_trust_evidence_t cargo_evidence[3] = {
        fixture_a.cargo,
        fixture_b.cargo,
        fixture_c.cargo,
    };
    settlement_event_trust_evidence_t event_evidence = {
        .cargo = cargo_evidence,
        .cargo_count = 3,
    };
    settlement_checkpoint_t construction_cp;
    ASSERT(settlement_apply_segment_trusted(
        &transfer_state, &construction_hdr,
        construction_payloads, construction_lens,
        &event_evidence, 1,
        &construction_cp, NULL));
    ASSERT_EQ_INT(
        transfer_state.station_manifest_counts[0], 0);
    ASSERT_EQ_FLOAT(
        transfer_state.construction_sites[0]
            .build_progress,
        0.3f, 0.0001f);
}

TEST(test_construction_duplicate_input_and_station_mismatch_are_inert) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0xE5, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));

    settlement_state_t baseline;
    settlement_state_init(&baseline);
    memset(baseline.construction_sites[0].scaffold_id,
           0x66, 32);
    memset(baseline.construction_sites[0].station_pubkey,
           0, 32);
    memcpy(
        baseline.construction_sites[0].station_pubkey,
        fixture.receipt.recipient_pubkey, 32);
    baseline.construction_sites[0].active = true;
    baseline.construction_site_count = 1;
    baseline.station_manifests[0][0] =
        fixture.origin.output_cargo;
    baseline.station_manifest_counts[0] = 1;
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0xE8, sizeof(sentinel));

    uint8_t construction[168] = {0};
    memset(construction, 0x66, 32);
    memcpy(construction + 32,
           fixture.receipt.recipient_pubkey,
           32);
    memcpy(construction + 64, cargo_pub, 32);
    memcpy(construction + 96, cargo_pub, 32);
    construction[160] = 2;
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x21, 3,
             construction, sizeof(construction));
    const uint8_t *payloads[1] = {construction};
    uint16_t lens[1] = {sizeof(construction)};
    settlement_cargo_trust_evidence_t cargo_evidence[2] = {
        fixture.cargo,
        fixture.cargo,
    };
    settlement_event_trust_evidence_t evidence = {
        .cargo = cargo_evidence,
        .cargo_count = 2,
    };
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &evidence, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    construction[160] = 1;
    memset(construction + 96, 0, 64);
    construction[32] ^= 0x01;
    make_hdr(&hdr, 0x21, 4,
             construction, sizeof(construction));
    evidence.cargo_count = 1;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &evidence, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_RECEIPT_HOLDER,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, 0));

    construction[32] ^= 0x01;
    baseline.construction_sites[0]
        .station_pubkey[0] ^= 0x01;
    make_hdr(&hdr, 0x21, 5,
             construction, sizeof(construction));
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &evidence, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    baseline.construction_sites[0]
        .station_pubkey[0] ^= 0x01;
    uint8_t absent_pub[32];
    memset(absent_pub, 0xF6, sizeof(absent_pub));
    settlement_trust_fixture_t absent_fixture;
    ASSERT(make_trust_fixture(
        absent_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &absent_fixture));
    memcpy(construction + 64, absent_pub, 32);
    make_hdr(&hdr, 0x21, 6,
             construction, sizeof(construction));
    evidence.cargo = &absent_fixture.cargo;
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &hdr, payloads, lens,
        &evidence, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));
}

TEST(test_later_apply_failure_rolls_back_state_and_checkpoint) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x39, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));

    settlement_state_t baseline;
    settlement_state_init(&baseline);
    baseline.station_ledger_counts[0] =
        SETTL_MAX_LEDGER_ENTRIES;
    for (uint8_t i = 0;
         i < SETTL_MAX_LEDGER_ENTRIES; i++) {
        baseline.station_ledgers[0][i]
            .player_pubkey[0] = (uint8_t)(i + 1);
    }

    uint8_t claim[112] = {0};
    memset(claim, 0x4A, 32);
    uint8_t sell[112];
    make_trade_payload(
        sell, cargo_pub, CARGO_KIND_INGOT, 1, 50);
    chain_event_header_t headers[2];
    make_hdr(&headers[0], 0x01, 1,
             claim, sizeof(claim));
    make_hdr(&headers[1], 0x11, 2,
             sell, sizeof(sell));
    const uint8_t *payloads[2] = {claim, sell};
    uint16_t lens[2] = {
        sizeof(claim), sizeof(sell),
    };
    settlement_event_trust_evidence_t evidence[2] = {
        {0},
        fixture.event,
    };
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0xF9, sizeof(sentinel));
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, headers, payloads, lens,
        evidence, 2, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        1, UINT8_MAX));
}

TEST(test_duplicate_sell_replay_rolls_back_entire_segment) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x5B, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));

    uint8_t sell[112];
    make_trade_payload(
        sell, cargo_pub, CARGO_KIND_INGOT, 1, 23);
    chain_event_header_t headers[2];
    make_hdr(&headers[0], 0x11, 1,
             sell, sizeof(sell));
    make_hdr(&headers[1], 0x11, 2,
             sell, sizeof(sell));
    const uint8_t *payloads[2] = {sell, sell};
    uint16_t lens[2] = {
        sizeof(sell), sizeof(sell),
    };
    settlement_event_trust_evidence_t evidence[2] = {
        fixture.event,
        fixture.event,
    };
    settlement_state_t baseline;
    settlement_state_init(&baseline);
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0x1A, sizeof(sentinel));
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, headers, payloads, lens,
        evidence, 2, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        1, UINT8_MAX));
}

TEST(test_sensitive_schemas_are_exact_and_buy_cannot_sell) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x6C, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));
    settlement_state_t baseline;
    settlement_state_init(&baseline);
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0x2B, sizeof(sentinel));

    uint8_t buy_as_sell[112];
    make_trade_payload(
        buy_as_sell, cargo_pub,
        CARGO_KIND_INGOT, 1, 20);
    chain_event_header_t buy_hdr;
    make_hdr(&buy_hdr, 0x10, 1,
             buy_as_sell, sizeof(buy_as_sell));
    const uint8_t *buy_payloads[1] = {buy_as_sell};
    uint16_t buy_lens[1] = {sizeof(buy_as_sell)};
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &buy_hdr, buy_payloads,
        buy_lens, NULL, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    uint8_t sell_extra[113] = {0};
    make_trade_payload(
        sell_extra, cargo_pub,
        CARGO_KIND_INGOT, 1, 20);
    chain_event_header_t sell_hdr;
    make_hdr(&sell_hdr, 0x11, 2,
             sell_extra, sizeof(sell_extra));
    const uint8_t *sell_payloads[1] = {
        sell_extra,
    };
    uint16_t sell_lens[1] = {
        sizeof(sell_extra),
    };
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &sell_hdr, sell_payloads,
        sell_lens, &fixture.event, 1,
        &sentinel, SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    uint8_t transfer_extra[145] = {0};
    memcpy(transfer_extra, cargo_pub, 32);
    transfer_extra[96] = CARGO_KIND_INGOT;
    chain_event_header_t transfer_hdr;
    make_hdr(&transfer_hdr, 0x04, 3,
             transfer_extra,
             sizeof(transfer_extra));
    const uint8_t *transfer_payloads[1] = {
        transfer_extra,
    };
    uint16_t transfer_lens[1] = {
        sizeof(transfer_extra),
    };
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &transfer_hdr,
        transfer_payloads, transfer_lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    uint8_t construction_extra[169] = {0};
    memcpy(construction_extra + 64,
           cargo_pub, 32);
    construction_extra[160] = 1;
    chain_event_header_t construction_hdr;
    make_hdr(&construction_hdr, 0x21, 4,
             construction_extra,
             sizeof(construction_extra));
    const uint8_t *construction_payloads[1] = {
        construction_extra,
    };
    uint16_t construction_lens[1] = {
        sizeof(construction_extra),
    };
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &construction_hdr,
        construction_payloads, construction_lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));

    uint8_t construction_count_four[168] = {0};
    memcpy(construction_count_four + 32,
           fixture.receipt.recipient_pubkey, 32);
    memcpy(construction_count_four + 64,
           cargo_pub, 32);
    construction_count_four[160] = 4;
    make_hdr(&construction_hdr, 0x21, 5,
             construction_count_four,
             sizeof(construction_count_four));
    construction_payloads[0] =
        construction_count_four;
    construction_lens[0] =
        sizeof(construction_count_four);
    ASSERT(rejected_segment_preserves_bytes(
        &baseline, &construction_hdr,
        construction_payloads, construction_lens,
        &fixture.event, 1, &sentinel,
        SETTLEMENT_APPLY_REJECT_EVENT,
        CARGO_RECEIPT_TRUST_STATUS_COUNT,
        0, UINT8_MAX));
}

TEST(test_receipt_and_origin_rejection_detail_is_preserved) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x8E, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));
    uint8_t sell[112];
    make_trade_payload(
        sell, cargo_pub, CARGO_KIND_INGOT, 1, 42);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x11, 1, sell, sizeof(sell));
    settlement_state_t baseline;
    settlement_state_init(&baseline);
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0x3C, sizeof(sentinel));

    fixture.receipt.signature[0] ^= 0x01;
    ASSERT(rejected_segment_preserves_trust_detail(
        &baseline, &hdr, sell, sizeof(sell),
        &fixture.event, &sentinel,
        CARGO_RECEIPT_TRUST_REJECT_CHAIN,
        CARGO_RECEIPT_REJECT_BAD_SIGNATURE));
    fixture.receipt.signature[0] ^= 0x01;

    sell[0] ^= 0x01;
    make_hdr(&hdr, 0x11, 2, sell, sizeof(sell));
    ASSERT(rejected_segment_preserves_trust_detail(
        &baseline, &hdr, sell, sizeof(sell),
        &fixture.event, &sentinel,
        CARGO_RECEIPT_TRUST_REJECT_CHAIN,
        CARGO_RECEIPT_REJECT_CARGO_MISMATCH));
    sell[0] ^= 0x01;
    make_hdr(&hdr, 0x11, 3, sell, sizeof(sell));

    fixture.origin.event_hash[0] ^= 0x01;
    ASSERT(rejected_segment_preserves_trust_detail(
        &baseline, &hdr, sell, sizeof(sell),
        &fixture.event, &sentinel,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN,
        CARGO_RECEIPT_OK));
    fixture.origin.event_hash[0] ^= 0x01;

    fixture.origin.authority[0] ^= 0x01;
    ASSERT(rejected_segment_preserves_trust_detail(
        &baseline, &hdr, sell, sizeof(sell),
        &fixture.event, &sentinel,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY,
        CARGO_RECEIPT_OK));
    fixture.origin.authority[0] ^= 0x01;

    fixture.origin.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED;
    ASSERT(rejected_segment_preserves_trust_detail(
        &baseline, &hdr, sell, sizeof(sell),
        &fixture.event, &sentinel,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY_LIFECYCLE,
        CARGO_RECEIPT_OK));
}

TEST(test_settlement_bad_arguments_are_byte_inert) {
    settlement_state_t baseline;
    settlement_state_init(&baseline);
    baseline.segment_index = 11;
    baseline.last_event_id = 23;
    settlement_state_t *candidate =
        (settlement_state_t *)malloc(sizeof(*candidate));
    ASSERT(candidate != NULL);

    uint8_t claim[112] = {0};
    memset(claim, 0x9F, 32);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x01, 24, claim, sizeof(claim));
    const uint8_t *payloads[1] = {claim};
    uint16_t lens[1] = {sizeof(claim)};
    settlement_checkpoint_t sentinel;
    memset(&sentinel, 0x4D, sizeof(sentinel));
    settlement_checkpoint_t checkpoint;
    settlement_apply_result_t result;

    *candidate = baseline;
    ASSERT(!settlement_apply_segment_trusted(
        candidate, &hdr, payloads, lens, NULL, 1,
        NULL, &result));
    ASSERT_EQ_INT(
        result.status,
        SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS);
    ASSERT(memcmp(candidate, &baseline,
                  sizeof(*candidate)) == 0);

    *candidate = baseline;
    checkpoint = sentinel;
    ASSERT(!settlement_apply_segment_trusted(
        candidate, NULL, payloads, lens, NULL, 1,
        &checkpoint, &result));
    ASSERT_EQ_INT(
        result.status,
        SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS);
    ASSERT(memcmp(candidate, &baseline,
                  sizeof(*candidate)) == 0);
    ASSERT(memcmp(&checkpoint, &sentinel,
                  sizeof(checkpoint)) == 0);

    const uint8_t *null_payloads[1] = {NULL};
    *candidate = baseline;
    checkpoint = sentinel;
    ASSERT(!settlement_apply_segment_trusted(
        candidate, &hdr, null_payloads, lens,
        NULL, 1, &checkpoint, &result));
    ASSERT_EQ_INT(
        result.status,
        SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS);
    ASSERT_EQ_INT(result.event_index, 0);
    ASSERT(memcmp(candidate, &baseline,
                  sizeof(*candidate)) == 0);
    ASSERT(memcmp(&checkpoint, &sentinel,
                  sizeof(checkpoint)) == 0);

    checkpoint = sentinel;
    ASSERT(!settlement_apply_segment_trusted(
        NULL, &hdr, payloads, lens, NULL, 1,
        &checkpoint, &result));
    ASSERT_EQ_INT(
        result.status,
        SETTLEMENT_APPLY_REJECT_BAD_ARGUMENTS);
    ASSERT(memcmp(&checkpoint, &sentinel,
                  sizeof(checkpoint)) == 0);
    free(candidate);
}

TEST(test_trusted_checkpoint_is_deterministic) {
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0x7D, sizeof(cargo_pub));
    settlement_trust_fixture_t fixture;
    ASSERT(make_trust_fixture(
        cargo_pub, CARGO_KIND_INGOT,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
        &fixture));
    uint8_t sell[112];
    make_trade_payload(
        sell, cargo_pub, CARGO_KIND_INGOT, 1, 31);
    chain_event_header_t hdr;
    make_hdr(&hdr, 0x11, 1, sell, sizeof(sell));
    const uint8_t *payloads[1] = {sell};
    uint16_t lens[1] = {sizeof(sell)};

    settlement_state_t *state_a =
        (settlement_state_t *)malloc(sizeof(*state_a));
    settlement_state_t *state_b =
        (settlement_state_t *)malloc(sizeof(*state_b));
    ASSERT(state_a != NULL);
    ASSERT(state_b != NULL);
    settlement_state_init(state_a);
    settlement_state_init(state_b);
    settlement_checkpoint_t checkpoint_a;
    settlement_checkpoint_t checkpoint_b;
    ASSERT(settlement_apply_segment_trusted(
        state_a, &hdr, payloads, lens,
        &fixture.event, 1, &checkpoint_a, NULL));
    ASSERT(settlement_apply_segment_trusted(
        state_b, &hdr, payloads, lens,
        &fixture.event, 1, &checkpoint_b, NULL));
    ASSERT(memcmp(state_a, state_b,
                  sizeof(*state_a)) == 0);
    ASSERT(memcmp(&checkpoint_a, &checkpoint_b,
                  sizeof(checkpoint_a)) == 0);
    free(state_a);
    free(state_b);
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
    RUN(test_manifest_root_sorts_by_complete_cargo_pubkey);
    RUN(test_settlement_payload_hash_rejection_is_inert);
    RUN(test_sensitive_events_fail_closed_without_evidence);
    RUN(test_trusted_current_and_rotated_sell_import);
    RUN(test_trust_rejections_are_stable_and_byte_inert);
    RUN(test_trusted_transfer_and_construction_consume_exact_cargo);
    RUN(test_construction_duplicate_input_and_station_mismatch_are_inert);
    RUN(test_later_apply_failure_rolls_back_state_and_checkpoint);
    RUN(test_duplicate_sell_replay_rolls_back_entire_segment);
    RUN(test_sensitive_schemas_are_exact_and_buy_cannot_sell);
    RUN(test_receipt_and_origin_rejection_detail_is_preserved);
    RUN(test_settlement_bad_arguments_are_byte_inert);
    RUN(test_trusted_checkpoint_is_deterministic);
}
