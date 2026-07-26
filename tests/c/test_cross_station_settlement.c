/*
 * test_cross_station_settlement.c -- Layer D of #479: portable cargo
 * receipts. See server/cargo_receipt_issue.h + shared/cargo_receipt.h
 * for the design.
 *
 * Tests exercise the receipt issuance + verification machinery
 * directly (the WS handler in server/main.c is the user). The flow:
 *
 *   1. Station A signs a receipt for cargo C bound to player P.
 *   2. P presents that receipt to station B.
 *   3. B verifies the chain end-to-end before accepting and issues
 *      its OWN receipt for the receiving leg.
 *
 * We synthesize each hop via cargo_receipt_emit_transfer (the same
 * primitive main.c calls), then verify the resulting chain.
 */
#include "test_harness.h"

#include "cargo_receipt.h"
#include "cargo_receipt_issue.h"
#include "cargo_receipt_trust.h"
#include "chain_log.h"
#include "handoff_flow.h"
#include "handoff_ticket.h"
#include "protocol.h"
#include "signal_crypto.h"
#include "station_authority.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

/* Per-test scratch dir helpers — mirror the chain_test_setup pattern
 * used by test_chain_log.c so concurrent shards stay isolated. */
static void crs_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_crs_%s", TMP("crs"), suffix);
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(path);
}

static void crs_teardown(void) {
    chain_log_set_disk_enabled(true);
    chain_log_set_dir(NULL);
}

static void crs_wipe_logs(world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w->stations[s]);
}

/* Initialize an already-allocated world with seeded chain state so
 * each test starts from a clean per-station chain head.
 * Caller owns the world_t (typically via WORLD_HEAP / calloc). */
static void crs_world_init(world_t *w, uint32_t seed) {
    w->rng = seed;
    world_reset(w);
    crs_wipe_logs(w);
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }
}

/* Synthesize a deterministic player pubkey + cargo pub for a test. */
static void fill_test_pubkey(uint8_t out[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed + i);
}

static void crs_write_u32_le(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void crs_write_u16_le(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
}

typedef struct {
    int calls;
    uint8_t status;
    uint8_t source_station;
    uint8_t dest_station;
    bool has_ticket;
    handoff_ticket_t ticket;
} crs_handoff_ticket_capture_t;

static void crs_capture_handoff_ticket(void *user, uint8_t status,
                                       uint8_t source_station,
                                       uint8_t dest_station,
                                       const handoff_ticket_t *ticket) {
    crs_handoff_ticket_capture_t *cap =
        (crs_handoff_ticket_capture_t *)user;
    cap->calls++;
    cap->status = status;
    cap->source_station = source_station;
    cap->dest_station = dest_station;
    cap->has_ticket = ticket != NULL;
    if (ticket) cap->ticket = *ticket;
}

typedef struct {
    int calls;
    uint8_t status;
    uint8_t reason;
    uint8_t dest_station;
    uint8_t ticket_hash[32];
} crs_handoff_result_capture_t;

static void crs_capture_handoff_result(void *user, uint8_t status,
                                       uint8_t reason,
                                       uint8_t dest_station,
                                       const uint8_t ticket_hash[32]) {
    crs_handoff_result_capture_t *cap =
        (crs_handoff_result_capture_t *)user;
    cap->calls++;
    cap->status = status;
    cap->reason = reason;
    cap->dest_station = dest_station;
    if (ticket_hash) memcpy(cap->ticket_hash, ticket_hash, 32);
    else memset(cap->ticket_hash, 0, 32);
}

static bool crs_prepare_player_carrier(world_t *w,
                                       const uint8_t player_pk[32],
                                       const uint8_t cargo_pk[32],
                                       const cargo_receipt_chain_t *chain) {
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->pubkey_set = true;
    memcpy(sp->pubkey, player_pk, 32);
    if (!ship_manifest_bootstrap(sp->ship)) return false;

    cargo_unit_t cu = {0};
    cu.kind = CARGO_KIND_INGOT;
    cu.commodity = COMMODITY_FERRITE_INGOT;
    cu.grade = MINING_GRADE_COMMON;
    cu.recipe_id = RECIPE_SMELT;
    cu.prefix_class = INGOT_PREFIX_M;
    cu.quantity = 1;
    memcpy(cu.pub, cargo_pk, 32);
    return ship_manifest_push_with_chain(sp->ship, &cu, chain);
}

static void crs_assert_cargo_store_equal(const cargo_store_t *actual,
                                         const cargo_store_t *expected) {
    ASSERT(actual != NULL);
    ASSERT(expected != NULL);
    ASSERT_EQ_INT(actual->manifest.count, expected->manifest.count);
    ASSERT_EQ_INT(actual->manifest.cap, expected->manifest.cap);
    if (actual->manifest.count > 0) {
        ASSERT(memcmp(actual->manifest.units, expected->manifest.units,
                      (size_t)actual->manifest.count *
                          sizeof(actual->manifest.units[0])) == 0);
    }
    const ship_receipts_t *actual_receipts =
        cargo_store_receipts_const(actual);
    const ship_receipts_t *expected_receipts =
        cargo_store_receipts_const(expected);
    ASSERT_EQ_INT(actual_receipts != NULL, expected_receipts != NULL);
    if (!actual_receipts || !expected_receipts) return;
    ASSERT_EQ_INT(actual_receipts->count, expected_receipts->count);
    ASSERT_EQ_INT(actual_receipts->cap, expected_receipts->cap);
    if (actual_receipts->count > 0) {
        ASSERT(memcmp(actual_receipts->chains, expected_receipts->chains,
                      (size_t)actual_receipts->count *
                          sizeof(actual_receipts->chains[0])) == 0);
    }
}

static void crs_init_foreign_station(station_t *foreign) {
    memset(foreign, 0, sizeof(*foreign));
    snprintf(foreign->name, sizeof(foreign->name), "Foreign");
    uint8_t founder_pk[32];
    fill_test_pubkey(founder_pk, 0xE0);
    station_authority_init_outpost(foreign, founder_pk, 777);
}

/* ---------------- Test 1: single-hop receipt ----------------------- */

TEST(test_cross_station_single_hop_receipt) {
    crs_setup("single_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD001);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x10);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x40);

    /* Helios cannot issue a first-hop receipt without a matching local
     * production event. */
    station_t *helios = &w->stations[2];
    cargo_receipt_t r = {0};
    cargo_receipt_chain_t empty_chain = {0};
    uint64_t event_id = cargo_receipt_emit_transfer(
        w, helios,
        helios->station_pubkey, player_pk,
        cargo_pk, (uint8_t)CARGO_KIND_INGOT,
        &empty_chain, &r);
    ASSERT_EQ_INT((int)event_id, 0);

    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pk, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(w, helios, CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) >= 1);
    uint8_t origin_hash[32];
    memcpy(origin_hash, helios->chain_last_hash, sizeof(origin_hash));
    ASSERT(chain_log_emit(w, helios, CHAIN_EVT_LEDGER, "later", 5) >= 1);
    event_id = cargo_receipt_emit_transfer(
        w, helios,
        helios->station_pubkey, player_pk,
        cargo_pk, (uint8_t)CARGO_KIND_INGOT,
        &empty_chain, &r);
    ASSERT(event_id >= 1);
    ASSERT(cargo_receipt_verify_signature(&r));
    ASSERT(memcmp(r.prev_receipt_hash, origin_hash, sizeof(origin_hash)) == 0);
    ASSERT(memcmp(r.prev_receipt_hash, helios->chain_last_hash,
                  sizeof(origin_hash)) != 0);
    /* Receipt fields are populated correctly. */
    ASSERT(memcmp(r.cargo_pub, cargo_pk, 32) == 0);
    ASSERT(memcmp(r.recipient_pubkey, player_pk, 32) == 0);
    ASSERT(memcmp(r.authoring_station, helios->station_pubkey, 32) == 0);
    /* Single-hop chain verifies. */
    ASSERT(cargo_receipt_chain_verify(&r, 1, cargo_pk) == CARGO_RECEIPT_OK);

    crs_teardown();
}

/* ---------------- Test 2: two-hop receipt chain --------------------- */

/* Helper: emit cargo issuance from station + first hop receipt for player. */
static bool crs_first_hop(world_t *w, station_t *st, const uint8_t player_pk[32],
                          const uint8_t cargo_pk[32], cargo_receipt_t *out) {
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pk, sizeof(smelt.ingot_pub));
    if (chain_log_emit(w, st, CHAIN_EVT_SMELT,
                       &smelt, sizeof(smelt)) == 0) return false;
    cargo_receipt_chain_t empty_chain = {0};
    uint64_t id = cargo_receipt_emit_transfer(
        w, st, st->station_pubkey, player_pk, cargo_pk,
        (uint8_t)CARGO_KIND_INGOT, &empty_chain, out);
    return id != 0;
}

static cargo_receipt_origin_proof_t crs_origin_proof(
    const cargo_receipt_t *receipt,
    cargo_receipt_origin_event_t event_type) {
    cargo_receipt_origin_proof_t proof = {
        .event_type = event_type,
        .event_id = 1,
        .epoch = receipt->epoch,
    };
    memcpy(proof.event_hash, receipt->prev_receipt_hash, 32);
    memcpy(proof.output_cargo_pub, receipt->cargo_pub, 32);
    memcpy(proof.authority, receipt->authoring_station, 32);
    return proof;
}

static bool crs_next_hop(world_t *w, station_t *dst,
                         const uint8_t from_pk[32],
                         const uint8_t cargo_pk[32],
                         const cargo_receipt_t *incoming,
                         uint8_t incoming_len,
                         cargo_receipt_t *out);

TEST(test_local_origin_resolver_distinguishes_history_states) {
    crs_setup("origin_resolver_states");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD016);
    station_t *st = &w->stations[2];

    uint8_t smelt_pub[32];
    uint8_t craft_pub[32];
    uint8_t unknown_pub[32];
    fill_test_pubkey(smelt_pub, 0x31);
    fill_test_pubkey(craft_pub, 0x61);
    fill_test_pubkey(unknown_pub, 0x91);
    cargo_receipt_origin_proof_t proof = {0};

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, smelt_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    ASSERT(strcmp(cargo_receipt_origin_resolve_status_name(
                      CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE),
                  "history_unavailable") == 0);

    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, smelt_pub, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(w, st, CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) == 1);
    uint8_t smelt_hash[32];
    memcpy(smelt_hash, st->chain_last_hash, sizeof(smelt_hash));

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, smelt_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    ASSERT_EQ_INT((int)proof.event_id, 1);
    ASSERT(memcmp(proof.event_hash, smelt_hash, sizeof(smelt_hash)) == 0);
    ASSERT(memcmp(proof.output_cargo_pub, smelt_pub, sizeof(smelt_pub)) == 0);
    ASSERT(memcmp(proof.authority, st->station_pubkey, 32) == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, unknown_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND);

    chain_payload_craft_t craft = {0};
    craft.recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    craft.input_count = 1;
    memcpy(craft.input_pubs[0], smelt_pub, sizeof(smelt_pub));
    memcpy(craft.output_pub, craft_pub, sizeof(craft.output_pub));
    ASSERT(chain_log_emit(w, st, CHAIN_EVT_CRAFT,
                          &craft, sizeof(craft)) == 2);
    uint8_t craft_hash[32];
    memcpy(craft_hash, st->chain_last_hash, sizeof(craft_hash));
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, craft_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_CRAFT);
    ASSERT_EQ_INT((int)proof.event_id, 2);
    ASSERT(memcmp(proof.event_hash, craft_hash, sizeof(craft_hash)) == 0);
    ASSERT(memcmp(proof.output_cargo_pub, craft_pub, sizeof(craft_pub)) == 0);

    char path[256];
    ASSERT(chain_log_path_for(st->station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "r+b");
    ASSERT(f != NULL);
    ASSERT(fseek(f, CHAIN_EVENT_HEADER_SIZE + (long)sizeof(uint16_t) + 5,
                 SEEK_SET) == 0);
    int byte = fgetc(f);
    ASSERT(byte != EOF);
    ASSERT(fseek(f, -1, SEEK_CUR) == 0);
    ASSERT(fputc(byte ^ 0x01, f) != EOF);
    ASSERT(fclose(f) == 0);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, smelt_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);

    station_t *memory_only = &w->stations[1];
    chain_log_set_disk_enabled(false);
    chain_payload_smelt_t memory_smelt = {0};
    memcpy(memory_smelt.ingot_pub, unknown_pub,
           sizeof(memory_smelt.ingot_pub));
    ASSERT(chain_log_emit(w, memory_only, CHAIN_EVT_SMELT,
                          &memory_smelt, sizeof(memory_smelt)) == 1);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(memory_only, unknown_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    chain_log_set_disk_enabled(true);

    crs_teardown();
}

TEST(test_receipt_trust_accepts_smelt_craft_and_rotated_authority) {
    crs_setup("trust_accept");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD101);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x11);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x41);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));

    cargo_receipt_origin_proof_t proof = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT_EQ_INT(result.chain_result, CARGO_RECEIPT_OK);
    ASSERT_EQ_INT(result.origin_event, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    ASSERT_STR_EQ(cargo_receipt_trust_status_name(result.status),
                  "valid_trusted");
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, true),
                  "accepted/trusted");

    proof.event_type = CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT_EQ_INT(result.origin_event, CARGO_RECEIPT_ORIGIN_EVENT_CRAFT);

    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);
    ASSERT_STR_EQ(cargo_receipt_trust_status_name(result.status),
                  "valid_trusted_rotated");
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, true),
                  "accepted/rotated");
    crs_teardown();
}

TEST(test_receipt_trust_distinguishes_origin_proof_failures) {
    crs_setup("trust_origin_failures");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD102);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x12);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x42);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));
    cargo_receipt_t receipt_before = receipt;
    cargo_receipt_origin_proof_t valid = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    cargo_receipt_origin_proof_t proof_before = valid;

    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, NULL,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN);
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/no-origin");

    cargo_receipt_origin_proof_t proof = valid;
    proof.event_type = CARGO_RECEIPT_ORIGIN_EVENT_NONE;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE);
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/origin");

    proof = valid;
    proof.output_cargo_pub[0] ^= 0x80u;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO);

    proof = valid;
    proof.event_hash[1] ^= 0x40u;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN);

    proof = valid;
    proof.authority[2] ^= 0x20u;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY);
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/seal");

    ASSERT(memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0);
    ASSERT(memcmp(&valid, &proof_before, sizeof(valid)) == 0);
    crs_teardown();
}

TEST(test_receipt_trust_distinguishes_authority_policy) {
    crs_setup("trust_authority");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD103);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x13);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x43);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));
    cargo_receipt_origin_proof_t proof = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);

    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof, CARGO_RECEIPT_AUTHORITY_UNKNOWN);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY);
    ASSERT_STR_EQ(cargo_receipt_trust_status_name(result.status),
                  "reject_unknown_authority");
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/unknown");

    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof, CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY);
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/untrusted");

    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof, CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);
    ASSERT_STR_EQ(cargo_receipt_trust_status_name(result.status),
                  "reject_revoked_authority");
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/revoked");

    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        (cargo_receipt_authority_trust_t)99);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS);
    ASSERT_STR_EQ(cargo_receipt_trust_status_name(
                      (cargo_receipt_trust_status_t)99),
                  "unknown");
    crs_teardown();
}

TEST(test_receipt_trust_preserves_cryptographic_chain_failure) {
    crs_setup("trust_chain_failure");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD104);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x14);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x44);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));
    cargo_receipt_origin_proof_t proof = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);

    cargo_receipt_t tampered = receipt;
    tampered.signature[0] ^= 0x01u;
    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &tampered, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_REJECT_CHAIN);
    ASSERT_EQ_INT(result.chain_result, CARGO_RECEIPT_REJECT_BAD_SIGNATURE);
    ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(result.status, false),
                  "rejected/witness");

    result = cargo_receipt_trust_verify(
        &receipt, 1, NULL, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS);
    ASSERT_EQ_INT(result.chain_result, CARGO_RECEIPT_OK);
    crs_teardown();
}

TEST(test_receipt_trust_semantic_labels_cover_stable_verdict_contract) {
    static const struct {
        cargo_receipt_trust_status_t status;
        bool accepted;
        const char *label;
    } vectors[] = {
        {CARGO_RECEIPT_TRUST_VALID_TRUSTED, true, "accepted/trusted"},
        {CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED, true,
         "accepted/rotated"},
        {CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS, false,
         "rejected/evidence"},
        {CARGO_RECEIPT_TRUST_REJECT_CHAIN, false, "rejected/witness"},
        {CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN, false,
         "rejected/no-origin"},
        {CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE, false,
         "rejected/origin"},
        {CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO, false,
         "rejected/origin"},
        {CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN, false,
         "rejected/origin"},
        {CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY, false,
         "rejected/seal"},
        {CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY, false,
         "rejected/unknown"},
        {CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY, false,
         "rejected/untrusted"},
        {CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY, false,
         "rejected/revoked"},
        {CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY, true,
         "accepted/unknown"},
        {CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY, true,
         "accepted/untrusted"},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        ASSERT_STR_EQ(cargo_receipt_trust_semantic_label(
                          vectors[i].status, vectors[i].accepted),
                      vectors[i].label);
    }
}

static cargo_unit_t crs_test_ingot(const uint8_t cargo_pk[32]) {
    cargo_unit_t unit = {
        .kind = CARGO_KIND_INGOT,
        .commodity = COMMODITY_FERRITE_INGOT,
        .grade = MINING_GRADE_COMMON,
        .recipe_id = RECIPE_SMELT,
        .prefix_class = INGOT_PREFIX_M,
        .quantity = 1,
    };
    memcpy(unit.pub, cargo_pk, sizeof(unit.pub));
    return unit;
}

TEST(test_station_trust_evaluator_accepts_current_and_local_origin) {
    crs_setup("station_eval_current_local");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD105);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x15);
    uint8_t remote_pub[32]; fill_test_pubkey(remote_pub, 0x45);
    cargo_receipt_t receipt = {0};
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk,
                         remote_pub, &receipt));
    cargo_unit_t remote = crs_test_ingot(remote_pub);
    cargo_receipt_chain_t remote_chain = {.len = 1};
    remote_chain.links[0] = receipt;

    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, 1, &remote, &remote_chain);
    ASSERT(evaluated.accepted);
    ASSERT(!evaluated.local_origin_without_receipt);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT_EQ_INT(evaluated.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(evaluated.origin_station, 2);

    uint8_t local_pub[32]; fill_test_pubkey(local_pub, 0x75);
    chain_payload_craft_t craft = {0};
    craft.recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    memcpy(craft.output_pub, local_pub, sizeof(craft.output_pub));
    ASSERT(chain_log_emit(w, &w->stations[1], CHAIN_EVT_CRAFT,
                          &craft, sizeof(craft)) >= 1);
    cargo_unit_t local = crs_test_ingot(local_pub);
    local.kind = CARGO_KIND_FRAME;
    local.commodity = COMMODITY_FRAME;
    local.recipe_id = RECIPE_FRAME_BASIC;
    cargo_receipt_chain_t empty = {0};
    evaluated = cargo_receipt_evaluate_at_station(w, 1, &local, &empty);
    ASSERT(evaluated.accepted);
    ASSERT(evaluated.local_origin_without_receipt);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT_EQ_INT(evaluated.origin_station, 1);

    uint8_t missing_pub[32]; fill_test_pubkey(missing_pub, 0xA5);
    cargo_unit_t missing = crs_test_ingot(missing_pub);
    evaluated = cargo_receipt_evaluate_at_station(
        w, 1, &missing, &empty);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN);
    crs_teardown();
}

TEST(test_station_trust_evaluator_accepts_rotated_origin_key) {
    crs_setup("station_eval_rotated");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD106);
    station_t historical = {0};
    crs_init_foreign_station(&historical);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x16);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x46);
    cargo_receipt_t receipt = {0};
    ASSERT(crs_first_hop(w, &historical, player_pk, cargo_pk, &receipt));

    station_t *owner = &w->stations[2];
    ASSERT_EQ_INT(owner->authority_registry_count, 1);
    memcpy(owner->authority_registry[1].pubkey,
           historical.station_pubkey, 32);
    owner->authority_registry[1].state =
        STATION_AUTHORITY_TRUST_ROTATED;
    owner->authority_registry_count = 2;
    ASSERT(station_authority_registry_validate(owner));

    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_chain_t chain = {.len = 1};
    chain.links[0] = receipt;
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(w, 1, &unit, &chain);
    ASSERT(evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);
    ASSERT_EQ_INT(evaluated.origin_station, 2);
    crs_teardown();
}

TEST(test_station_trust_evaluator_applies_unknown_untrusted_and_revoked_policy) {
    crs_setup("station_eval_policy");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD107);
    station_t foreign = {0};
    crs_init_foreign_station(&foreign);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x17);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x47);
    cargo_receipt_t receipt = {0};
    ASSERT(crs_first_hop(w, &foreign, player_pk, cargo_pk, &receipt));
    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_chain_t chain = {.len = 1};
    chain.links[0] = receipt;

    int viewer_idx = -1;
    for (int i = 0; i < w->station_count; i++) {
        if (!cargo_legality_station_tolerates_contraband(
                &w->stations[i], i)) {
            viewer_idx = i;
            break;
        }
    }
    ASSERT(viewer_idx >= 0);
    station_t *viewer = &w->stations[viewer_idx];
    bool screens = cargo_legality_station_screens(viewer, viewer_idx);

    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, viewer_idx, &unit, &chain);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY);
    ASSERT_EQ_INT(evaluated.accepted, !screens);

    ASSERT(station_authority_registry_set_trust(
        viewer, foreign.station_pubkey,
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    evaluated = cargo_receipt_evaluate_at_station(
        w, viewer_idx, &unit, &chain);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY);

    ASSERT(station_authority_registry_set_trust(
        viewer, foreign.station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));
    evaluated = cargo_receipt_evaluate_at_station(
        w, viewer_idx, &unit, &chain);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);
    crs_teardown();
}

TEST(test_station_trust_evaluator_rejects_revoked_intermediate_author) {
    crs_setup("station_eval_revoked_intermediate");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD108);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x18);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0x48);
    cargo_receipt_t first = {0};
    cargo_receipt_t second = {0};
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk,
                         cargo_pk, &first));
    ASSERT(crs_next_hop(w, &w->stations[0], player_pk,
                        cargo_pk, &first, 1, &second));
    ASSERT(station_authority_registry_set_trust(
        &w->stations[1], w->stations[0].station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));

    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_chain_t chain = {.len = 2};
    chain.links[0] = first;
    chain.links[1] = second;
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(w, 1, &unit, &chain);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.first_rejected_link, 1);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);
    crs_teardown();
}

/* Helper: emit destination-station receipt for the second hop. */
static bool crs_next_hop(world_t *w, station_t *dst, const uint8_t from_pk[32],
                         const uint8_t cargo_pk[32],
                         const cargo_receipt_t *incoming, uint8_t incoming_len,
                         cargo_receipt_t *out) {
    if (!incoming || incoming_len == 0 ||
        incoming_len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
        return false;
    }
    cargo_receipt_chain_t chain = {0};
    memcpy(chain.links, incoming,
           (size_t)incoming_len * sizeof(chain.links[0]));
    chain.len = incoming_len;
    uint64_t id = cargo_receipt_emit_transfer(
        w, dst, from_pk, dst->station_pubkey, cargo_pk,
        (uint8_t)CARGO_KIND_INGOT, &chain, out);
    return id != 0;
}

TEST(test_cross_station_two_hop_chain) {
    crs_setup("two_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD002);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x20);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x50);

    /* Hop 1: Helios -> player. */
    station_t *helios = &w->stations[2];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Hop 2: player -> Kepler. Kepler verifies r1 then signs r2 whose
     * prev_receipt_hash = SHA-256(r1). */
    station_t *kepler = &w->stations[1];
    cargo_receipt_t r2;
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, 1, &r2));

    /* Two-hop chain verifies. */
    cargo_receipt_t chain[2] = { r1, r2 };
    ASSERT(cargo_receipt_chain_verify(chain, 2, cargo_pk) == CARGO_RECEIPT_OK);

    /* Each hop is signed by the right station. */
    ASSERT(memcmp(r1.authoring_station, helios->station_pubkey, 32) == 0);
    ASSERT(memcmp(r2.authoring_station, kepler->station_pubkey, 32) == 0);
    /* Kepler's chain log has the EVT_TRANSFER in it. */
    uint64_t walked = 0;
    ASSERT(chain_log_verify(kepler, &walked, NULL));
    ASSERT(walked >= 1);

    crs_teardown();
}

/* ---------------- Test 3: forged receipt rejection ------------------ */

TEST(test_cross_station_forged_receipt_rejected) {
    crs_setup("forged");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD003);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x30);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x60);

    /* Hop 1: Helios -> player. */
    station_t *helios = &w->stations[2];
    station_t *prospect = &w->stations[0];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Tamper: rewrite r1.authoring_station to claim it was Prospect.
     * Signature was made with Helios's secret over a body that named
     * Helios. After this overwrite, Prospect's pubkey gets fed to
     * Ed25519 verify against Helios's signature → should fail. */
    memcpy(r1.authoring_station, prospect->station_pubkey, 32);
    ASSERT(!cargo_receipt_verify_signature(&r1));
    ASSERT(cargo_receipt_chain_verify(&r1, 1, cargo_pk)
           == CARGO_RECEIPT_REJECT_BAD_SIGNATURE);

    crs_teardown();
}

/* ---------------- Test 4: tampered chain rejection ------------------ */

TEST(test_cross_station_tampered_cargo_pub_rejected) {
    crs_setup("tampered");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD004);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x40);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x70);

    station_t *helios = &w->stations[2];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Flip a bit in cargo_pub — the signature was over the original
     * 144-byte unsigned span, so verify must fail. */
    r1.cargo_pub[7] ^= 0xFF;
    ASSERT(!cargo_receipt_verify_signature(&r1));
    ASSERT(cargo_receipt_chain_verify(&r1, 1, NULL)
           == CARGO_RECEIPT_REJECT_BAD_SIGNATURE);

    crs_teardown();
}

/* ---------------- Test 5: broken linkage rejection ------------------ */

TEST(test_cross_station_broken_linkage_rejected) {
    crs_setup("linkage");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD005);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x50);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x80);

    station_t *helios = &w->stations[2];
    station_t *kepler = &w->stations[1];
    cargo_receipt_t r1, r2;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, 1, &r2));

    /* Replace r1 with a different first-hop receipt issued by Helios
     * for the same cargo but in a fresh chain (different chain_last_hash
     * → different prev_receipt_hash). r2's prev_receipt_hash still
     * points at the original r1, so the linkage check must fail. */
    cargo_receipt_t r1_alt;
    chain_payload_smelt_t alt_smelt = {0};
    memcpy(alt_smelt.ingot_pub, cargo_pk, sizeof(alt_smelt.ingot_pub));
    ASSERT(chain_log_emit(w, helios, CHAIN_EVT_SMELT,
                          &alt_smelt, sizeof(alt_smelt)) >= 1);
    cargo_receipt_chain_t empty_chain = {0};
    uint64_t alt_id = cargo_receipt_emit_transfer(
        w, helios, helios->station_pubkey, player_pk, cargo_pk,
        (uint8_t)CARGO_KIND_INGOT, &empty_chain, &r1_alt);
    ASSERT(alt_id != 0);
    /* Sanity: the alternative receipt verifies on its own. */
    ASSERT(cargo_receipt_verify_signature(&r1_alt));

    cargo_receipt_t chain[2] = { r1_alt, r2 };
    ASSERT(cargo_receipt_chain_verify(chain, 2, cargo_pk)
           == CARGO_RECEIPT_REJECT_BROKEN_LINKAGE);

    crs_teardown();
}

/* ---------------- Test 6: three-hop chain --------------------------- */

TEST(test_cross_station_three_hop_chain) {
    crs_setup("three_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD006);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x60);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x90);

    /* Hop 1: Prospect -> player. */
    station_t *prospect = &w->stations[0];
    station_t *kepler   = &w->stations[1];
    station_t *helios   = &w->stations[2];

    cargo_receipt_t r1, r2, r3;
    ASSERT(crs_first_hop(w, prospect, player_pk, cargo_pk, &r1));
    /* Hop 2: player -> Kepler. */
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, 1, &r2));
    /* Hop 3: player -> Helios (re-extract from Kepler back through
     * the player; in real flow Kepler issues to player, player carries
     * to Helios). */
    cargo_receipt_t first_two[2] = {r1, r2};
    ASSERT(crs_next_hop(w, helios, player_pk, cargo_pk,
                        first_two, 2, &r3));

    cargo_receipt_t chain[3] = { r1, r2, r3 };
    ASSERT(cargo_receipt_chain_verify(chain, 3, cargo_pk) == CARGO_RECEIPT_OK);
    ASSERT(memcmp(r1.authoring_station, prospect->station_pubkey, 32) == 0);
    ASSERT(memcmp(r2.authoring_station, kepler->station_pubkey, 32) == 0);
    ASSERT(memcmp(r3.authoring_station, helios->station_pubkey, 32) == 0);

    crs_teardown();
}

/* ---------------- Test 7: NPC-mediated transfer -------------------- */
/* The wire path for NPC haulers is the same primitive — they issue
 * via cargo_receipt_emit_transfer just like a player does. We model
 * an NPC by using a deterministic NPC pubkey instead of a player one;
 * the math is identical. */

TEST(test_cross_station_npc_mediated_transfer) {
    crs_setup("npc");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD007);
    ASSERT(w != NULL);

    uint8_t npc_pk[32];   fill_test_pubkey(npc_pk,   0x70);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0xA0);

    station_t *helios = &w->stations[2];
    station_t *kepler = &w->stations[1];

    cargo_receipt_t r1, r2;
    ASSERT(crs_first_hop(w, helios, npc_pk, cargo_pk, &r1));
    ASSERT(crs_next_hop(w, kepler, npc_pk, cargo_pk, &r1, 1, &r2));

    /* Kepler accepts: chain validates. */
    cargo_receipt_t chain[2] = { r1, r2 };
    ASSERT(cargo_receipt_chain_verify(chain, 2, cargo_pk) == CARGO_RECEIPT_OK);
    /* Recipient on the first leg is the NPC; on the second leg, Kepler. */
    ASSERT(memcmp(r1.recipient_pubkey, npc_pk, 32) == 0);
    ASSERT(memcmp(r2.recipient_pubkey, kepler->station_pubkey, 32) == 0);

    crs_teardown();
}

/* ---------------- Test 8: save/load preserves receipts -------------- */

TEST(test_cross_station_save_load_preserves_receipts) {
    crs_setup("save_load");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD008);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x80);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xB0);

    station_t *helios = &w->stations[2];
    station_t *kepler = &w->stations[1];

    /* Build a 2-hop chain and attach to ship.manifest + ship.receipts
     * for player slot 0. */
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    ASSERT(ship_manifest_bootstrap(sp->ship));
    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);

    cargo_unit_t cu = {0};
    cu.kind = CARGO_KIND_INGOT;
    cu.commodity = COMMODITY_FERRITE_INGOT;
    cu.grade = MINING_GRADE_COMMON;
    cu.recipe_id = RECIPE_SMELT;
    cu.prefix_class = INGOT_PREFIX_M;
    memcpy(cu.pub, cargo_pk, 32);
    ASSERT(manifest_push(&sp->ship->manifest, &cu));

    cargo_receipt_t r1, r2;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, 1, &r2));
    cargo_receipt_t chain[2] = { r1, r2 };
    ASSERT(ship_receipts_push_chain(rcpts, chain, 2));
    /* Parity: receipts.count == manifest.count == 1. */
    ASSERT_EQ_INT((int)rcpts->count, 1);
    ASSERT_EQ_INT((int)sp->ship->manifest.count, 1);

    /* Persist + reload via player_save / player_load. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s_crs_save", TMP("crs_dir"));
    /* Ensure dir exists; player_save creates the file but not a fresh dir hierarchy. */
    /* Use legacy save path (token-keyed) since no pubkey is registered. */
    /* The token must be set so player_save_path picks legacy path. */
    for (int i = 0; i < 8; i++) sp->session_token[i] = (uint8_t)(0x42 + i);
    sp->session_ready = true;

    /* world_save handles the world-side state; player_save persists
     * the ship + manifest + receipts tail. */
    ASSERT(world_save(w, TMP("crs_world.sav")));
    /* player_save's ensure_save_subdirs only creates pubkey/ and
     * legacy/ INSIDE `dir`; it does NOT create `dir` itself. The
     * test_tmp_path scratch root already exists, but our nested
     * directory under it does not — create it before saving. */
    {
#if defined(_WIN32)
        (void)_mkdir(dir);
#else
        (void)mkdir(dir, 0700);
#endif
    }
    ASSERT(player_save(sp, dir, 0));

    /* Fresh load. */
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    ASSERT(world_load(w2, TMP("crs_world.sav")));
    ship_t sp2_ship = {0};
    server_player_t sp2 = {.ship = &sp2_ship};
    memcpy(sp2.session_token, sp->session_token, 8);
    sp2.session_ready = true;
    ASSERT(player_load_by_token(&sp2, w2, dir, sp->session_token));

    ASSERT_EQ_INT((int)sp2.ship->manifest.count, 1);
    ship_receipts_t *rcpts2 = ship_get_receipts(sp2.ship);
    ASSERT(rcpts2 != NULL);
    ASSERT_EQ_INT((int)rcpts2->count, 1);
    ASSERT_EQ_INT((int)rcpts2->chains[0].len, 2);
    /* The reloaded chain must still verify end-to-end. */
    ASSERT(cargo_receipt_chain_verify(rcpts2->chains[0].links,
                                      rcpts2->chains[0].len, cargo_pk)
           == CARGO_RECEIPT_OK);

    /* Cleanup. */
    ship_cleanup(sp2.ship);
    remove(TMP("crs_world.sav"));
    crs_teardown();
}

/* ---------------- Test 9: chain length cap -------------------------- */

TEST(test_cross_station_chain_length_cap) {
    crs_setup("cap");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD009);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x90);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xC0);

    /* Try to build a 17-hop chain. The 17th transfer must be refused
     * by ship_receipts_extend (cap = 16). We check both: the verifier
     * rejects a 17-element chain and ship_receipts_extend refuses to
     * grow beyond 16. */
    cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN + 1] = {0};

    station_t *st = &w->stations[2];
    ASSERT(crs_first_hop(w, st, player_pk, cargo_pk, &chain[0]));
    for (int i = 1; i < CARGO_RECEIPT_CHAIN_MAX_LEN; i++) {
        /* Alternate stations 1 and 2 for each subsequent hop so each
         * hop is signed by a real keyed station. */
        station_t *next = &w->stations[(i % 2 == 0) ? 2 : 1];
        ASSERT(crs_next_hop(w, next, player_pk, cargo_pk,
                            chain, i, &chain[i]));
    }
    station_t *overflow = &w->stations[1];
    ASSERT(!crs_next_hop(w, overflow, player_pk, cargo_pk,
                         chain, CARGO_RECEIPT_CHAIN_MAX_LEN,
                         &chain[CARGO_RECEIPT_CHAIN_MAX_LEN]));
    /* The 17-element chain trips the TOO_LONG cap in chain_verify. */
    ASSERT(cargo_receipt_chain_verify(chain, CARGO_RECEIPT_CHAIN_MAX_LEN + 1,
                                      cargo_pk)
           == CARGO_RECEIPT_REJECT_TOO_LONG);
    /* The 16-element prefix verifies. */
    ASSERT(cargo_receipt_chain_verify(chain, CARGO_RECEIPT_CHAIN_MAX_LEN,
                                      cargo_pk)
           == CARGO_RECEIPT_OK);

    /* ship_receipts_extend refuses past the cap. */
    ship_receipts_t r = {0};
    ASSERT(ship_receipts_init(&r, 4));
    ASSERT(ship_receipts_push_chain(&r, chain, CARGO_RECEIPT_CHAIN_MAX_LEN));
    /* Attempting to extend a CHAIN_MAX_LEN-deep chain by one more must fail. */
    ASSERT(!ship_receipts_extend(&r, 0, &chain[CARGO_RECEIPT_CHAIN_MAX_LEN]));
    ship_receipts_free(&r);

    crs_teardown();
}

/* ---------------- Test 10: presented peer chain attaches ------------ */

TEST(test_present_receipt_chain_to_carried_cargo) {
    crs_setup("present_attach");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00A);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA0);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD0);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(w, 2, sp, cargo_pk, &r1, 1)
           == CARGO_RECEIPT_PRESENT_OK);

    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->count, 1);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 1);
    ASSERT(memcmp(&rcpts->chains[0].links[0], &r1, sizeof(r1)) == 0);

    crs_teardown();
}

/* ---------------- Test 11: presented chain dispatch attaches -------- */

TEST(test_present_receipt_chain_dispatch_attaches) {
    crs_setup("present_dispatch_attach");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD01F);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xB2);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xE2);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));
    w->players[0].docked = true;
    w->players[0].current_station = 2;

    uint8_t msg[35 + CARGO_RECEIPT_SIZE];
    msg[0] = NET_MSG_PRESENT_RECEIPT_CHAIN;
    memcpy(&msg[1], cargo_pk, 32);
    crs_write_u16_le(&msg[33], 1);
    cargo_receipt_pack(&r1, &msg[35]);

    server_receipt_presentation_dispatch_result_t result;
    ASSERT(server_dispatch_receipt_presentation_message(
        w, 0, msg, sizeof(msg), &result));
    ASSERT(result.evaluated);
    ASSERT_EQ_INT(result.result, CARGO_RECEIPT_PRESENT_OK);

    ship_receipts_t *rcpts = ship_get_receipts(w->players[0].ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->count, 1);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 1);
    ASSERT(memcmp(&rcpts->chains[0].links[0], &r1, sizeof(r1)) == 0);

    crs_teardown();
}

/* ---------------- Test 12: foreign authority is accepted ------------ */

TEST(test_present_foreign_authority_receipt_chain) {
    crs_setup("present_foreign");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00B);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA1);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD1);
    station_t foreign;
    crs_init_foreign_station(&foreign);
    for (int i = 0; i < 3; i++) {
        ASSERT(memcmp(foreign.station_pubkey,
                      w->stations[i].station_pubkey, 32) != 0);
    }

    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pk, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(w, &foreign, CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) >= 1);
    cargo_receipt_chain_t empty_chain = {0};
    cargo_receipt_t r1;
    ASSERT(cargo_receipt_emit_transfer(w, &foreign,
                                       foreign.station_pubkey, player_pk,
                                       cargo_pk, (uint8_t)CARGO_KIND_INGOT,
                                       &empty_chain, &r1) != 0);
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

    int evaluating_station = -1;
    for (int i = 0; i < w->station_count; i++) {
        if (!cargo_legality_station_screens(&w->stations[i], i) ||
            cargo_legality_station_tolerates_contraband(
                &w->stations[i], i)) {
            evaluating_station = i;
            break;
        }
    }
    ASSERT(evaluating_station >= 0);
    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(
               w, evaluating_station, sp, cargo_pk, &r1, 1)
           == CARGO_RECEIPT_PRESENT_OK);
    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 1);
    ASSERT(memcmp(rcpts->chains[0].links[0].authoring_station,
                  foreign.station_pubkey, 32) == 0);

    crs_teardown();
}

/* ---------------- Test 12: presented chain must name bearer --------- */

TEST(test_present_receipt_chain_rejects_wrong_recipient) {
    crs_setup("present_recipient");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00C);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA2);
    uint8_t other_pk[32];  fill_test_pubkey(other_pk,  0xB2);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD2);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], other_pk, cargo_pk, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(w, 2, sp, cargo_pk, &r1, 1)
           == CARGO_RECEIPT_PRESENT_REJECT_RECIPIENT);
    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 0);

    crs_teardown();
}

/* ---------------- Test 13: local chain cannot be rewritten ---------- */

TEST(test_present_receipt_chain_rejects_existing_mismatch) {
    crs_setup("present_mismatch");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00D);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA3);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD3);
    cargo_receipt_t original;
    cargo_receipt_t alternate;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &original));
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &alternate));
    ASSERT(memcmp(&original, &alternate, sizeof(original)) != 0);

    cargo_receipt_chain_t existing = {0};
    existing.links[0] = original;
    existing.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &existing));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(
               w, 2, sp, cargo_pk, &alternate, 1)
           == CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH);
    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 1);
    ASSERT(memcmp(&rcpts->chains[0].links[0], &original, sizeof(original)) == 0);

    crs_teardown();
}

TEST(test_present_receipt_chain_trust_rejection_is_transactionally_inert) {
    crs_setup("present_trust_atomic");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD020);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA4);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0xD4);
    cargo_receipt_t receipt = {0};
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk,
                         cargo_pk, &receipt));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));
    ASSERT(station_authority_registry_set_trust(
        &w->stations[1], w->stations[2].station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));

    cargo_store_t before = {0};
    ASSERT(cargo_store_clone(
        &before, &w->players[0].ship->cargo_store));
    ASSERT_EQ_INT(
        cargo_receipt_present_to_ship(
            w, 1, &w->players[0], cargo_pk, &receipt, 1),
        CARGO_RECEIPT_PRESENT_REJECT_TRUST);
    crs_assert_cargo_store_equal(
        &w->players[0].ship->cargo_store, &before);
    cargo_store_cleanup(&before);
    crs_teardown();
}

TEST(test_named_trade_trust_rejection_preserves_cargo_and_ledger) {
    crs_setup("named_trade_trust_atomic");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD021);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA5);
    uint8_t buy_pub[32]; fill_test_pubkey(buy_pub, 0xD5);

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_identity_finalized = true;
    memcpy(sp->pubkey, player_pk, sizeof(sp->pubkey));
    sp->docked = true;
    sp->current_station = 1;
    ASSERT(ship_manifest_bootstrap(sp->ship));

    cargo_receipt_t buy_receipt = {0};
    ASSERT(crs_first_hop(w, &w->stations[2],
                         w->stations[1].station_pubkey,
                         buy_pub, &buy_receipt));
    cargo_receipt_chain_t buy_chain = {.len = 1};
    buy_chain.links[0] = buy_receipt;
    cargo_unit_t buy_unit = crs_test_ingot(buy_pub);
    ASSERT(station_manifest_push_with_chain(
        &w->stations[1], &buy_unit, &buy_chain));
    ledger_earn_by_pubkey(&w->stations[1], player_pk, 10000.0f);
    ASSERT(station_authority_registry_set_trust(
        &w->stations[1], w->stations[2].station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));

    cargo_store_t station_before = {0};
    cargo_store_t ship_before = {0};
    ASSERT(cargo_store_clone(
        &station_before, &w->stations[1].cargo_store));
    ASSERT(cargo_store_clone(&ship_before, &sp->ship->cargo_store));
    float balance_before =
        ledger_balance_by_pubkey(&w->stations[1], player_pk);
    server_signed_action_dispatch_result_t dispatch = {0};
    ASSERT(server_dispatch_signed_action_payload(
        w, 0, SIGNED_ACTION_BUY_INGOT, buy_pub, sizeof(buy_pub),
        NULL, NULL, &dispatch));
    crs_assert_cargo_store_equal(
        &w->stations[1].cargo_store, &station_before);
    crs_assert_cargo_store_equal(&sp->ship->cargo_store, &ship_before);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(&w->stations[1], player_pk),
        balance_before, 0.001f);
    cargo_store_cleanup(&station_before);
    cargo_store_cleanup(&ship_before);

    /* Use a fresh world because revoked lifecycle state is monotonic. */
    crs_teardown();
    crs_setup("named_deliver_trust_atomic");
    WORLD_HEAP delivery_world = calloc(1, sizeof(world_t));
    ASSERT(delivery_world != NULL);
    crs_world_init(delivery_world, 0xD022);
    uint8_t deliver_pub[32]; fill_test_pubkey(deliver_pub, 0xE5);
    cargo_receipt_t deliver_receipt = {0};
    ASSERT(crs_first_hop(
        delivery_world, &delivery_world->stations[2],
        player_pk, deliver_pub, &deliver_receipt));
    cargo_receipt_chain_t deliver_chain = {.len = 1};
    deliver_chain.links[0] = deliver_receipt;
    ASSERT(crs_prepare_player_carrier(
        delivery_world, player_pk, deliver_pub, &deliver_chain));
    server_player_t *deliverer = &delivery_world->players[0];
    deliverer->pubkey_proof_ok = true;
    deliverer->pubkey_identity_finalized = true;
    deliverer->docked = true;
    deliverer->current_station = 1;
    ASSERT(station_manifest_bootstrap(&delivery_world->stations[1]));
    ASSERT(station_authority_registry_set_trust(
        &delivery_world->stations[1],
        delivery_world->stations[2].station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));

    station_before = (cargo_store_t){0};
    ship_before = (cargo_store_t){0};
    ASSERT(cargo_store_clone(
        &station_before,
        &delivery_world->stations[1].cargo_store));
    ASSERT(cargo_store_clone(
        &ship_before, &deliverer->ship->cargo_store));
    balance_before = ledger_balance_by_pubkey(
        &delivery_world->stations[1], player_pk);
    uint8_t target = 0;
    ASSERT(server_dispatch_signed_action_payload(
        delivery_world, 0, SIGNED_ACTION_DELIVER,
        &target, sizeof(target), NULL, NULL, &dispatch));
    crs_assert_cargo_store_equal(
        &delivery_world->stations[1].cargo_store, &station_before);
    crs_assert_cargo_store_equal(
        &deliverer->ship->cargo_store, &ship_before);
    ASSERT_EQ_FLOAT(
        ledger_balance_by_pubkey(
            &delivery_world->stations[1], player_pk),
        balance_before, 0.001f);
    cargo_store_cleanup(&station_before);
    cargo_store_cleanup(&ship_before);
    crs_teardown();
}

/* ---------------- Test 14: handoff ticket round-trip --------------- */

TEST(test_handoff_ticket_roundtrip_verifies_ship_and_cargo) {
    crs_setup("handoff_roundtrip");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00E);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA4);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD4);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &chain));

    server_player_t *sp = &w->players[0];
    sp->ship->pos = (vec2){123.0f, -45.5f};
    sp->ship->vel = (vec2){1.25f, 2.5f};
    sp->ship->angle = 0.75f;
    sp->ship->hull = 91.0f;

    handoff_ticket_t ticket;
    ASSERT(handoff_ticket_issue_for_ship(
        w->stations[2].station_pubkey, w->stations[2].station_secret,
        w->stations[1].station_pubkey, player_pk,
        2u, 1u, 100u, 160u, sp->ship, &ticket));
    ASSERT_EQ_INT((int)ticket.cargo_count, 1);
    ASSERT(handoff_ticket_verify_for_ship(
        &ticket, 120u,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_OK);

    uint8_t packed[HANDOFF_TICKET_SIZE];
    handoff_ticket_t unpacked;
    handoff_ticket_pack(&ticket, packed);
    ASSERT(handoff_ticket_unpack(packed, &unpacked));
    ASSERT(handoff_ticket_verify_for_ship(
        &unpacked, 120u,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_OK);

    crs_teardown();
}

/* ---------------- Test 15: handoff binds ship state ---------------- */

TEST(test_handoff_ticket_rejects_tampered_ship_state) {
    crs_setup("handoff_ship_tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00F);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA5);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD5);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &chain));

    server_player_t *sp = &w->players[0];
    handoff_ticket_t ticket;
    ASSERT(handoff_ticket_issue_for_ship(
        w->stations[2].station_pubkey, w->stations[2].station_secret,
        w->stations[1].station_pubkey, player_pk,
        2u, 1u, 100u, 160u, sp->ship, &ticket));
    sp->ship->hull -= 1.0f;
    ASSERT(handoff_ticket_verify_for_ship(
        &ticket, 120u,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_REJECT_SHIP_STATE);

    crs_teardown();
}

/* ---------------- Test 16: handoff binds receipt chains ------------ */

TEST(test_handoff_ticket_rejects_tampered_cargo_root) {
    crs_setup("handoff_cargo_tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD010);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA6);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD6);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &chain));

    server_player_t *sp = &w->players[0];
    handoff_ticket_t ticket;
    ASSERT(handoff_ticket_issue_for_ship(
        w->stations[2].station_pubkey, w->stations[2].station_secret,
        w->stations[1].station_pubkey, player_pk,
        2u, 1u, 100u, 160u, sp->ship, &ticket));

    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    rcpts->chains[0].links[0].signature[0] ^= 0x01u;
    ASSERT(handoff_ticket_verify_for_ship(
        &ticket, 120u,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_REJECT_CARGO_ROOT);

    crs_teardown();
}

/* ---------------- Test 17: handoff rejects stale or forged ticket --- */

TEST(test_handoff_ticket_rejects_expired_wrong_dest_and_forgery) {
    crs_setup("handoff_rejects");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD011);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA7);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD7);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &chain));

    server_player_t *sp = &w->players[0];
    handoff_ticket_t ticket;
    ASSERT(handoff_ticket_issue_for_ship(
        w->stations[2].station_pubkey, w->stations[2].station_secret,
        w->stations[1].station_pubkey, player_pk,
        2u, 1u, 100u, 160u, sp->ship, &ticket));

    ASSERT(handoff_ticket_verify_for_ship(
        &ticket, 161u,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_REJECT_EXPIRED);
    ASSERT(handoff_ticket_verify_for_ship(
        &ticket, 120u,
        w->stations[2].station_pubkey,
        w->stations[0].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_REJECT_DEST);

    ticket.dest_zone ^= 0x01u;
    ASSERT(handoff_ticket_verify_for_ship(
        &ticket, 120u,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_REJECT_BAD_SIGNATURE);

    crs_teardown();
}

/* ---------------- Test 18: handoff snapshot wire round-trip --------- */

TEST(test_handoff_snapshot_roundtrip_preserves_bound_hashes) {
    crs_setup("handoff_snapshot_roundtrip");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD012);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA8);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD8);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &chain));

    server_player_t *sp = &w->players[0];
    sp->ship->pos = (vec2){321.0f, -654.0f};
    sp->ship->vel = (vec2){7.0f, -3.0f};
    sp->ship->hull = 88.0f;
    sp->ship->mining_level = 2;
    sp->ship->hold_level = 3;
    sp->ship->unlocked_modules = 0x15u;

    uint8_t ship_hash_before[32], cargo_root_before[32];
    handoff_ticket_ship_state_hash(sp->ship, ship_hash_before);
    handoff_ticket_cargo_root(sp->ship, cargo_root_before);

    size_t len = handoff_ship_snapshot_size(sp->ship);
    ASSERT(len > HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE);
    uint8_t *buf = malloc(len); ASSERT(buf != NULL);
    size_t packed = 0;
    ASSERT(handoff_ship_snapshot_pack(sp->ship, buf, len, &packed));
    ASSERT_EQ_INT((int)packed, (int)len);

    ship_t unpacked = {0};
    size_t consumed = 0;
    ASSERT(handoff_ship_snapshot_unpack(buf, len, &unpacked, &consumed));
    ASSERT_EQ_INT((int)consumed, (int)len);

    uint8_t ship_hash_after[32], cargo_root_after[32];
    handoff_ticket_ship_state_hash(&unpacked, ship_hash_after);
    handoff_ticket_cargo_root(&unpacked, cargo_root_after);
    ASSERT(memcmp(ship_hash_before, ship_hash_after, 32) == 0);
    ASSERT(memcmp(cargo_root_before, cargo_root_after, 32) == 0);
    ASSERT_EQ_INT((int)unpacked.manifest.count, 1);

    ship_cleanup(&unpacked);
    free(buf);
    crs_teardown();
}

/* ---------------- Test 19: handoff issue/present/accept ------------ */

TEST(test_handoff_flow_accept_hydrates_destination_ship) {
    crs_setup("handoff_flow_accept");
    WORLD_HEAP src = calloc(1, sizeof(world_t)); ASSERT(src != NULL); crs_world_init(src, 0xD013);
    WORLD_HEAP dst = calloc(1, sizeof(world_t)); ASSERT(dst != NULL); crs_world_init(dst, 0xD013);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA9);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xD9);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(src, &src->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(src, player_pk, cargo_pk, &chain));

    server_player_t *source_sp = &src->players[0];
    source_sp->ship->pos = (vec2){900.0f, 100.0f};
    source_sp->ship->vel = (vec2){5.0f, 6.0f};
    source_sp->ship->hull = 77.0f;
    source_sp->ship->tractor_level = 2;

    handoff_ticket_t ticket;
    ASSERT(handoff_issue_ticket_to_station(src, 0, 2, 1, 240u, &ticket));

    server_player_t *dest_sp = &dst->players[0];
    player_init_ship(dest_sp, dst);
    dest_sp->connected = true;
    dest_sp->pubkey_set = true;
    memcpy(dest_sp->pubkey, player_pk, 32);
    ASSERT_EQ_INT((int)dest_sp->ship->manifest.count, 0);

    int dest_station = -1;
    ASSERT(handoff_accept_presented_ship(dst, 0, &ticket, source_sp->ship,
                                         &dest_station) == HANDOFF_FLOW_OK);
    ASSERT_EQ_INT(dest_station, 1);
    ASSERT_EQ_INT((int)dest_sp->ship->manifest.count, 1);
    ASSERT_EQ_FLOAT(dest_sp->ship->pos.x, source_sp->ship->pos.x, 0.001f);
    ASSERT_EQ_FLOAT(dest_sp->ship->hull, source_sp->ship->hull, 0.001f);
    ASSERT(!dest_sp->docked);
    ASSERT(dest_sp->replication->force_authoritative_resync);

    uint8_t dst_root[32], src_root[32];
    handoff_ticket_cargo_root(dest_sp->ship, dst_root);
    handoff_ticket_cargo_root(source_sp->ship, src_root);
    ASSERT(memcmp(dst_root, src_root, 32) == 0);

    crs_teardown();
}

/* ---------------- Test 20: shared handoff request dispatch ---------- */

TEST(test_handoff_dispatch_request_emits_ticket) {
    crs_setup("handoff_dispatch_request");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD020);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xB0);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xE0);
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));
    server_player_t *sp = &w->players[0];
    sp->current_station = 2;
    w->tick = 100u;

    uint8_t req[NET_HANDOFF_REQUEST_SIZE] = {
        NET_MSG_HANDOFF_REQUEST, 0xFFu, 1u, 0, 0, 0, 0
    };
    crs_write_u32_le(&req[3], 240u);

    crs_handoff_ticket_capture_t cap = {0};
    ASSERT(server_dispatch_handoff_request(w, 0, req, sizeof(req),
                                           crs_capture_handoff_ticket,
                                           &cap));
    ASSERT_EQ_INT(cap.calls, 1);
    ASSERT_EQ_INT(cap.status, NET_HANDOFF_STATUS_OK);
    ASSERT_EQ_INT(cap.source_station, 2);
    ASSERT_EQ_INT(cap.dest_station, 1);
    ASSERT(cap.has_ticket);
    ASSERT(handoff_ticket_verify_for_ship(
        &cap.ticket, (uint64_t)w->tick,
        w->stations[2].station_pubkey,
        w->stations[1].station_pubkey,
        player_pk, sp->ship) == HANDOFF_TICKET_OK);

    crs_teardown();
}

/* ---------------- Test 21: shared handoff present dispatch ---------- */

TEST(test_handoff_dispatch_present_emits_result) {
    crs_setup("handoff_dispatch_present");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD021);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xB1);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xE1);
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));
    server_player_t *sp = &w->players[0];
    sp->current_station = 2;
    w->tick = 100u;

    handoff_ticket_t ticket;
    ASSERT(handoff_issue_ticket_to_station(w, 0, 2, 1, 240u, &ticket));
    size_t snapshot_len = handoff_ship_snapshot_size(sp->ship);
    ASSERT(snapshot_len > 0);
    size_t msg_len = 1u + HANDOFF_TICKET_SIZE + 4u + snapshot_len;
    uint8_t *msg = malloc(msg_len); ASSERT(msg != NULL);
    msg[0] = NET_MSG_HANDOFF_PRESENT;
    handoff_ticket_pack(&ticket, &msg[1]);
    crs_write_u32_le(&msg[1 + HANDOFF_TICKET_SIZE],
                     (uint32_t)snapshot_len);
    size_t packed = 0;
    ASSERT(handoff_ship_snapshot_pack(sp->ship,
                                      &msg[1 + HANDOFF_TICKET_SIZE + 4u],
                                      snapshot_len, &packed));
    ASSERT_EQ_INT((int)packed, (int)snapshot_len);

    uint8_t expected_hash[32];
    handoff_ticket_hash(&ticket, expected_hash);
    crs_handoff_result_capture_t cap = {0};
    ASSERT(server_dispatch_handoff_present(w, 0, msg, (int)msg_len,
                                           crs_capture_handoff_result,
                                           &cap));
    ASSERT_EQ_INT(cap.calls, 1);
    ASSERT_EQ_INT(cap.status, NET_HANDOFF_STATUS_OK);
    ASSERT_EQ_INT(cap.reason, HANDOFF_FLOW_OK);
    ASSERT_EQ_INT(cap.dest_station, 1);
    ASSERT(memcmp(cap.ticket_hash, expected_hash, 32) == 0);
    ASSERT_EQ_INT(sp->nearby_station, 1);
    ASSERT(!sp->docked);
    ASSERT(sp->replication->force_authoritative_resync);

    free(msg);
    crs_teardown();
}

/* ---------------- Test 22: handoff replay/tamper rejection ---------- */

TEST(test_handoff_flow_rejects_replay_and_tamper) {
    crs_setup("handoff_flow_rejects");
    WORLD_HEAP src = calloc(1, sizeof(world_t)); ASSERT(src != NULL); crs_world_init(src, 0xD014);
    WORLD_HEAP dst = calloc(1, sizeof(world_t)); ASSERT(dst != NULL); crs_world_init(dst, 0xD014);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xAA);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xDA);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(src, &src->stations[2], player_pk, cargo_pk, &r1));
    cargo_receipt_chain_t chain = {0};
    chain.links[0] = r1;
    chain.len = 1;
    ASSERT(crs_prepare_player_carrier(src, player_pk, cargo_pk, &chain));

    server_player_t *source_sp = &src->players[0];
    handoff_ticket_t ticket;
    ASSERT(handoff_issue_ticket_to_station(src, 0, 2, 1, 240u, &ticket));

    server_player_t *dest_sp = &dst->players[0];
    player_init_ship(dest_sp, dst);
    dest_sp->connected = true;
    dest_sp->pubkey_set = true;
    memcpy(dest_sp->pubkey, player_pk, 32);

    int dest_station = -1;
    ASSERT(handoff_accept_presented_ship(dst, 0, &ticket, source_sp->ship,
                                         &dest_station) == HANDOFF_FLOW_OK);
    ASSERT(handoff_accept_presented_ship(dst, 0, &ticket, source_sp->ship,
                                         &dest_station) == HANDOFF_FLOW_REJECT_REPLAY);

    handoff_ticket_t fresh;
    ASSERT(handoff_issue_ticket_to_station(src, 0, 2, 1, 240u, &fresh));
    ship_t tampered = {0};
    ASSERT(ship_copy(&tampered, source_sp->ship));
    tampered.hull -= 1.0f;
    ASSERT(handoff_accept_presented_ship(dst, 0, &fresh, &tampered,
                                         &dest_station) == HANDOFF_FLOW_REJECT_VERIFY);
    ship_cleanup(&tampered);

    crs_teardown();
}

TEST(test_handoff_flow_rejects_unknown_source_authority) {
    crs_setup("handoff_flow_unknown_source");
    WORLD_HEAP dst = calloc(1, sizeof(world_t)); ASSERT(dst != NULL); crs_world_init(dst, 0xD015);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xAB);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xDB);
    uint8_t rogue_seed[32]; fill_test_pubkey(rogue_seed, 0x3C);
    uint8_t rogue_pub[32];
    uint8_t rogue_secret[64];
    signal_crypto_keypair_from_seed(rogue_seed, rogue_pub, rogue_secret);

    cargo_receipt_chain_t chain = {0};
    ASSERT(crs_prepare_player_carrier(dst, player_pk, cargo_pk, &chain));
    server_player_t *sp = &dst->players[0];

    handoff_ticket_t ticket;
    ASSERT(handoff_ticket_issue_for_ship(
        rogue_pub, rogue_secret,
        dst->stations[1].station_pubkey,
        player_pk,
        777u, dst->stations[1].id,
        (uint64_t)dst->tick,
        (uint64_t)dst->tick + 240u,
        sp->ship, &ticket));

    int dest_station = -1;
    ASSERT(handoff_accept_presented_ship(dst, 0, &ticket, sp->ship,
                                         &dest_station) == HANDOFF_FLOW_REJECT_SOURCE);

    crs_teardown();
}

void register_cross_station_settlement_tests(void);
void register_cross_station_settlement_tests(void) {
    TEST_SECTION("\n--- Cross-Station Settlement (#479 D) ---\n");
    RUN(test_cross_station_single_hop_receipt);
    RUN(test_receipt_trust_accepts_smelt_craft_and_rotated_authority);
    RUN(test_receipt_trust_distinguishes_origin_proof_failures);
    RUN(test_receipt_trust_distinguishes_authority_policy);
    RUN(test_receipt_trust_preserves_cryptographic_chain_failure);
    RUN(test_receipt_trust_semantic_labels_cover_stable_verdict_contract);
    RUN(test_station_trust_evaluator_accepts_current_and_local_origin);
    RUN(test_station_trust_evaluator_accepts_rotated_origin_key);
    RUN(test_station_trust_evaluator_applies_unknown_untrusted_and_revoked_policy);
    RUN(test_station_trust_evaluator_rejects_revoked_intermediate_author);
    RUN(test_local_origin_resolver_distinguishes_history_states);
    RUN(test_cross_station_two_hop_chain);
    RUN(test_cross_station_forged_receipt_rejected);
    RUN(test_cross_station_tampered_cargo_pub_rejected);
    RUN(test_cross_station_broken_linkage_rejected);
    RUN(test_cross_station_three_hop_chain);
    RUN(test_cross_station_npc_mediated_transfer);
    RUN(test_cross_station_save_load_preserves_receipts);
    RUN(test_cross_station_chain_length_cap);
    RUN(test_present_receipt_chain_to_carried_cargo);
    RUN(test_present_receipt_chain_dispatch_attaches);
    RUN(test_present_foreign_authority_receipt_chain);
    RUN(test_present_receipt_chain_rejects_wrong_recipient);
    RUN(test_present_receipt_chain_rejects_existing_mismatch);
    RUN(test_present_receipt_chain_trust_rejection_is_transactionally_inert);
    RUN(test_named_trade_trust_rejection_preserves_cargo_and_ledger);
    RUN(test_handoff_ticket_roundtrip_verifies_ship_and_cargo);
    RUN(test_handoff_ticket_rejects_tampered_ship_state);
    RUN(test_handoff_ticket_rejects_tampered_cargo_root);
    RUN(test_handoff_ticket_rejects_expired_wrong_dest_and_forgery);
    RUN(test_handoff_snapshot_roundtrip_preserves_bound_hashes);
    RUN(test_handoff_flow_accept_hydrates_destination_ship);
    RUN(test_handoff_dispatch_request_emits_ticket);
    RUN(test_handoff_dispatch_present_emits_result);
    RUN(test_handoff_flow_rejects_replay_and_tamper);
    RUN(test_handoff_flow_rejects_unknown_source_authority);
}
