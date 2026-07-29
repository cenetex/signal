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
#include <sys/stat.h>
#if defined(_WIN32)
#  include <direct.h>
#  include <sys/utime.h>
#else
#  include <sys/types.h>
#  include <utime.h>
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

static bool crs_restore_file_times_seconds(
    const char *path,
    const struct stat *state) {
    if (!path || !state) return false;
#if defined(_WIN32)
    struct _utimbuf times = {
        state->st_atime,
        state->st_mtime,
    };
    return _utime(path, &times) == 0;
#else
    struct utimbuf times = {
        .actime = state->st_atime,
        .modtime = state->st_mtime,
    };
    return utime(path, &times) == 0;
#endif
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

enum { CRS_CARGO_FIXTURE_CAP = 128 };

typedef struct {
    uint8_t pub[32];
    cargo_unit_t unit;
} crs_cargo_fixture_t;

static crs_cargo_fixture_t
    crs_cargo_fixtures[CRS_CARGO_FIXTURE_CAP];
static size_t crs_cargo_fixture_count;

/*
 * Receipt tests historically started with arbitrary cargo pubs.  Keep the
 * convenient one-byte seeds, but turn them into real hash_ingot identities
 * and retain the canonical preimage for later hops.
 */
static void fill_test_cargo_pubkey(uint8_t out[32], uint8_t seed) {
    uint8_t fragment_pub[32];
    cargo_unit_t unit = {0};
    fill_test_pubkey(fragment_pub, seed);
    if (!hash_ingot(
            COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
            fragment_pub, 0, &unit)) {
        memset(out, 0, 32);
        return;
    }
    memcpy(out, unit.pub, 32);
    for (size_t i = 0; i < crs_cargo_fixture_count; i++) {
        if (memcmp(crs_cargo_fixtures[i].pub, out, 32) == 0)
            return;
    }
    if (crs_cargo_fixture_count < CRS_CARGO_FIXTURE_CAP) {
        crs_cargo_fixture_t *fixture =
            &crs_cargo_fixtures[crs_cargo_fixture_count++];
        memcpy(fixture->pub, out, 32);
        fixture->unit = unit;
    }
}

static void fill_indexed_test_pubkey(
    uint8_t out[32], uint8_t domain, uint16_t index) {
    for (int i = 0; i < 32; i++) {
        out[i] = (uint8_t)(
            domain + (uint8_t)(i * 17) +
            (uint8_t)(index * 29u));
    }
    out[0] = domain;
    out[1] = (uint8_t)(index & 0xffu);
    out[2] = (uint8_t)(index >> 8);
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

static cargo_unit_t crs_test_ingot_at(
    const uint8_t cargo_pk[32],
    int origin_station) {
    cargo_unit_t unit = {0};
    for (size_t i = 0; i < crs_cargo_fixture_count; i++) {
        if (memcmp(crs_cargo_fixtures[i].pub,
                   cargo_pk, 32) == 0) {
            unit = crs_cargo_fixtures[i].unit;
            break;
        }
    }
    unit.origin_station = (uint8_t)origin_station;
    return unit;
}

static cargo_unit_t crs_test_ingot(
    const uint8_t cargo_pk[32]) {
    return crs_test_ingot_at(cargo_pk, 2);
}

static int crs_world_station_index(
    const world_t *w,
    const station_t *station) {
    if (!w || !station) return -1;
    int count = w->station_count;
    if (count > MAX_STATIONS) count = MAX_STATIONS;
    for (int i = 0; i < count; i++) {
        if (station == &w->stations[i]) return i;
    }
    return -1;
}

static int crs_receipt_origin_station(
    const world_t *w,
    const cargo_receipt_t *receipt) {
    if (!w || !receipt) return -1;
    int count = w->station_count;
    if (count > MAX_STATIONS) count = MAX_STATIONS;
    for (int station_idx = 0;
         station_idx < count; station_idx++) {
        const station_t *station =
            &w->stations[station_idx];
        for (uint8_t record_idx = 0;
             record_idx <
                 station->authority_registry_count;
             record_idx++) {
            if (memcmp(
                    station->authority_registry[
                        record_idx].pubkey,
                    receipt->authoring_station,
                    32) == 0) {
                return station_idx;
            }
        }
    }
    return -1;
}

static bool crs_prepare_player_carrier(world_t *w,
                                       const uint8_t player_pk[32],
                                       const uint8_t cargo_pk[32],
                                       const cargo_receipt_chain_t *chain) {
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->docked = true;
    sp->current_station = 0;
    memcpy(sp->pubkey, player_pk, 32);
    if (!ship_manifest_bootstrap(sp->ship)) return false;

    cargo_unit_t cu = crs_test_ingot(cargo_pk);
    return ship_manifest_push_with_chain(sp->ship, &cu, chain);
}

static void crs_init_foreign_station(station_t *foreign) {
    memset(foreign, 0, sizeof(*foreign));
    snprintf(foreign->name, sizeof(foreign->name), "Foreign");
    uint8_t founder_pk[32];
    fill_test_pubkey(founder_pk, 0xE0);
    station_authority_init_outpost(foreign, founder_pk, 777);
}

typedef struct {
    world_t *world;
    station_t *station;
    uint64_t emitted_event_id;
} crs_origin_cache_build_mutation_t;

static void crs_mutate_chain_during_origin_cache_build(void *user) {
    crs_origin_cache_build_mutation_t *mutation =
        (crs_origin_cache_build_mutation_t *)user;
    static const uint8_t payload[] = "cache-build-race";
    mutation->emitted_event_id = chain_log_emit(
        mutation->world, mutation->station,
        CHAIN_EVT_LEDGER, payload, sizeof(payload));
}

#if !defined(_WIN32)
typedef struct {
    char live_path[320];
    char original_hold_path[320];
    char alternate_path[320];
    int original_to_hold;
    int alternate_to_live;
    int live_to_alternate;
    int hold_to_live;
} crs_origin_cache_aba_swap_t;

static void crs_swap_origin_path_around_snapshot_open(
    cargo_receipt_origin_cache_test_snapshot_phase_t phase,
    void *user) {
    crs_origin_cache_aba_swap_t *swap =
        (crs_origin_cache_aba_swap_t *)user;
    if (phase ==
        CARGO_RECEIPT_ORIGIN_CACHE_TEST_BEFORE_SNAPSHOT_OPEN) {
        swap->original_to_hold =
            rename(swap->live_path, swap->original_hold_path);
        swap->alternate_to_live =
            rename(swap->alternate_path, swap->live_path);
    } else {
        swap->live_to_alternate =
            rename(swap->live_path, swap->alternate_path);
        swap->hold_to_live =
            rename(swap->original_hold_path, swap->live_path);
    }
}
#endif

/* ---------------- Test 1: single-hop receipt ----------------------- */

TEST(test_cross_station_single_hop_receipt) {
    crs_setup("single_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD001);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x10);
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0x40);
    cargo_unit_t unit = crs_test_ingot_at(cargo_pk, 2);

    /* Helios cannot issue a first-hop receipt without a matching local
     * production event. */
    station_t *helios = &w->stations[2];
    cargo_receipt_t r;
    memset(&r, 0xA5, sizeof(r));
    cargo_receipt_chain_t empty_chain = {0};
    cargo_receipt_transfer_link_t missing_link =
        cargo_receipt_prepare_transfer_link(
            helios, helios->station_pubkey, cargo_pk,
            &empty_chain);
    ASSERT_EQ_INT(missing_link.status,
                  CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN);
    ASSERT_EQ_INT(missing_link.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    uint64_t event_id = cargo_receipt_emit_transfer(
        w, helios,
        helios->station_pubkey, player_pk,
        &unit, &empty_chain, &r);
    ASSERT_EQ_INT((int)event_id, 0);
    cargo_receipt_t zero_receipt = {0};
    ASSERT(memcmp(&r, &zero_receipt, sizeof(r)) == 0);
    ASSERT_EQ_INT((int)helios->chain_event_count, 0);

    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, unit.parent_merkle, 0, &unit));
    ASSERT(chain_log_emit(w, helios, CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) >= 1);
    uint8_t origin_hash[32];
    memcpy(origin_hash, helios->chain_last_hash, sizeof(origin_hash));
    ASSERT(chain_log_emit(w, helios, CHAIN_EVT_LEDGER, "later", 5) >= 1);
    uint8_t unrelated_hash[32];
    memcpy(unrelated_hash, helios->chain_last_hash,
           sizeof(unrelated_hash));
    cargo_receipt_transfer_link_t origin_link =
        cargo_receipt_prepare_transfer_link(
            helios, helios->station_pubkey, cargo_pk,
            &empty_chain);
    ASSERT_EQ_INT(origin_link.status, CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(origin_link.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT(memcmp(origin_link.prev_receipt_hash, origin_hash,
                  sizeof(origin_hash)) == 0);

    event_id = cargo_receipt_emit_transfer(
        w, helios,
        helios->station_pubkey, player_pk,
        &unit, &empty_chain, &r);
    ASSERT(event_id >= 1);
    ASSERT(cargo_receipt_verify_signature(&r));
    ASSERT(memcmp(r.prev_receipt_hash, origin_hash, sizeof(origin_hash)) == 0);
    ASSERT(memcmp(r.prev_receipt_hash, unrelated_hash,
                  sizeof(unrelated_hash)) != 0);
    /* Receipt fields are populated correctly. */
    ASSERT(memcmp(r.cargo_pub, cargo_pk, 32) == 0);
    ASSERT(memcmp(r.recipient_pubkey, player_pk, 32) == 0);
    ASSERT(memcmp(r.authoring_station, helios->station_pubkey, 32) == 0);
    /* Single-hop chain verifies. */
    ASSERT(cargo_receipt_chain_verify(&r, 1, cargo_pk) == CARGO_RECEIPT_OK);
    cargo_receipt_origin_proof_t proof;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(helios, cargo_pk, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_ALREADY_TRANSFERRED);
    ASSERT(memcmp(&proof, &(cargo_receipt_origin_proof_t){0},
                  sizeof(proof)) == 0);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority_pinned(
            helios, r.authoring_station, cargo_pk,
            r.prev_receipt_hash, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_trust_result_t trust = cargo_receipt_trust_verify(
        &r, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(trust.status, CARGO_RECEIPT_TRUST_VALID_TRUSTED);

    crs_teardown();
}

/* ---------------- Test 2: two-hop receipt chain --------------------- */

/*
 * Test-only constructor for historical/foreign authorities that deliberately
 * are not live members of `world.stations[]`. Production callers must use
 * cargo_receipt_emit_transfer(), which now requires full-unit evaluation.
 */
static bool crs_emit_external_test_transfer(
    world_t *w,
    station_t *station,
    const uint8_t from_pubkey[32],
    const uint8_t to_pubkey[32],
    const cargo_unit_t *unit,
    const uint8_t previous_hash[32],
    cargo_receipt_t *out) {
    if (!w || !station || !from_pubkey || !to_pubkey ||
        !unit || !previous_hash || !out ||
        station->chain_event_count == UINT64_MAX) {
        return false;
    }
    uint64_t event_id =
        station->chain_event_count + 1u;
    uint64_t epoch =
        (uint64_t)(w->time * 120.0);
    if (!cargo_receipt_issue(
            station, epoch, event_id, unit->pub,
            to_pubkey, previous_hash, out)) {
        return false;
    }
    chain_payload_transfer_t transfer = {0};
    memcpy(transfer.from_pubkey, from_pubkey, 32);
    memcpy(transfer.to_pubkey, to_pubkey, 32);
    memcpy(transfer.cargo_pub, unit->pub, 32);
    transfer.kind = unit->kind;
    return chain_log_emit(
               w, station, CHAIN_EVT_TRANSFER,
               &transfer, (uint16_t)sizeof(transfer)) ==
           event_id;
}

/* Helper: emit cargo issuance from station + first hop receipt for player. */
static bool crs_first_hop(world_t *w, station_t *st, const uint8_t player_pk[32],
                          const uint8_t cargo_pk[32], cargo_receipt_t *out) {
    int station_index =
        crs_world_station_index(w, st);
    cargo_unit_t unit = crs_test_ingot_at(
        cargo_pk, station_index >= 0 ? station_index : 2);
    chain_payload_smelt_t smelt = {0};
    if (!chain_payload_smelt_bind_output(
            &smelt, unit.parent_merkle, 0, &unit)) {
        return false;
    }
    if (chain_log_emit(w, st, CHAIN_EVT_SMELT,
                       &smelt, sizeof(smelt)) == 0) return false;
    uint8_t origin_hash[32];
    memcpy(origin_hash, st->chain_last_hash,
           sizeof(origin_hash));
    cargo_receipt_chain_t empty_chain = {0};
    if (station_index >= 0) {
        return cargo_receipt_emit_transfer(
                   w, st, st->station_pubkey,
                   player_pk, &unit,
                   &empty_chain, out) != 0;
    }
    return crs_emit_external_test_transfer(
        w, st, st->station_pubkey, player_pk,
        &unit, origin_hash, out);
}

static cargo_receipt_origin_proof_t crs_origin_proof(
    const cargo_receipt_t *receipt,
    cargo_receipt_origin_event_t event_type) {
    cargo_receipt_origin_proof_t proof = {
        .event_type = event_type,
        .authority_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
        .event_id = 1,
        .epoch = receipt->epoch,
        .output_semantics_version =
            CARGO_RECEIPT_ORIGIN_SEMANTICS_V1,
    };
    memcpy(proof.event_hash, receipt->prev_receipt_hash, 32);
    memcpy(proof.output_cargo_pub, receipt->cargo_pub, 32);
    memcpy(proof.output_cargo.pub, receipt->cargo_pub, 32);
    memcpy(proof.authority, receipt->authoring_station, 32);
    return proof;
}

static bool crs_next_hop(world_t *w, station_t *dst,
                         const uint8_t from_pk[32],
                         const uint8_t cargo_pk[32],
                         const cargo_receipt_t *incoming,
                         uint8_t incoming_len,
                         cargo_receipt_t *out);

TEST(test_receipt_trust_accepts_smelt_craft_and_rotated_authority) {
    crs_setup("trust_accept");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD101);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x11);
    uint8_t cargo_pk[32]; fill_test_cargo_pubkey(cargo_pk, 0x41);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));

    const cargo_receipt_t receipt_before = receipt;
    const uint8_t cargo_before[32] = {
        cargo_pk[0], cargo_pk[1], cargo_pk[2], cargo_pk[3],
        cargo_pk[4], cargo_pk[5], cargo_pk[6], cargo_pk[7],
        cargo_pk[8], cargo_pk[9], cargo_pk[10], cargo_pk[11],
        cargo_pk[12], cargo_pk[13], cargo_pk[14], cargo_pk[15],
        cargo_pk[16], cargo_pk[17], cargo_pk[18], cargo_pk[19],
        cargo_pk[20], cargo_pk[21], cargo_pk[22], cargo_pk[23],
        cargo_pk[24], cargo_pk[25], cargo_pk[26], cargo_pk[27],
        cargo_pk[28], cargo_pk[29], cargo_pk[30], cargo_pk[31],
    };
    cargo_receipt_origin_proof_t proof = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    cargo_receipt_origin_proof_t proof_before = proof;
    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT(result.chain_checked);
    ASSERT_EQ_INT(result.chain_result, CARGO_RECEIPT_OK);
    ASSERT_EQ_INT(result.origin_event, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    ASSERT_EQ_INT(result.authority_trust,
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);

    proof.event_type = CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
    proof_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT_EQ_INT(result.origin_event, CARGO_RECEIPT_ORIGIN_EVENT_CRAFT);

    proof.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED;
    proof_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);
    ASSERT_EQ_INT(result.authority_trust,
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    ASSERT(memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0);
    ASSERT(memcmp(cargo_pk, cargo_before, sizeof(cargo_pk)) == 0);
    ASSERT(memcmp(&proof, &proof_before, sizeof(proof)) == 0);
    crs_teardown();
}

TEST(test_receipt_trust_distinguishes_origin_proof_failures) {
    crs_setup("trust_origin_failures");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD102);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x12);
    uint8_t cargo_pk[32]; fill_test_cargo_pubkey(cargo_pk, 0x42);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));
    const cargo_receipt_t receipt_before = receipt;
    uint8_t cargo_before[32];
    memcpy(cargo_before, cargo_pk, sizeof(cargo_before));
    cargo_receipt_origin_proof_t valid = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    const cargo_receipt_origin_proof_t valid_before = valid;

    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, NULL,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN);

    cargo_receipt_origin_proof_t proof = valid;
    proof.event_type = CARGO_RECEIPT_ORIGIN_EVENT_NONE;
    const cargo_receipt_origin_proof_t wrong_type_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE);
    ASSERT(memcmp(&proof, &wrong_type_before, sizeof(proof)) == 0);

    proof = valid;
    proof.output_cargo_pub[0] ^= 0x80u;
    const cargo_receipt_origin_proof_t wrong_cargo_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO);
    ASSERT(memcmp(&proof, &wrong_cargo_before, sizeof(proof)) == 0);

    proof = valid;
    proof.output_semantics_version =
        CARGO_RECEIPT_ORIGIN_SEMANTICS_UNBOUND;
    const cargo_receipt_origin_proof_t unbound_metadata_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA);
    ASSERT(memcmp(&proof, &unbound_metadata_before, sizeof(proof)) == 0);

    proof = valid;
    proof.output_cargo.pub[3] ^= 0x10u;
    const cargo_receipt_origin_proof_t wrong_metadata_cargo_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA);
    ASSERT(memcmp(&proof, &wrong_metadata_cargo_before, sizeof(proof)) == 0);

    proof = valid;
    proof.event_hash[1] ^= 0x40u;
    const cargo_receipt_origin_proof_t wrong_hash_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN);
    ASSERT(memcmp(&proof, &wrong_hash_before, sizeof(proof)) == 0);

    proof = valid;
    proof.authority[2] ^= 0x20u;
    const cargo_receipt_origin_proof_t wrong_authority_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY);
    ASSERT(memcmp(&proof, &wrong_authority_before, sizeof(proof)) == 0);

    proof = valid;
    proof.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED;
    const cargo_receipt_origin_proof_t wrong_lifecycle_before = proof;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(
        result.status,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY_LIFECYCLE);
    ASSERT(memcmp(&proof, &wrong_lifecycle_before, sizeof(proof)) == 0);

    ASSERT(memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0);
    ASSERT(memcmp(cargo_pk, cargo_before, sizeof(cargo_pk)) == 0);
    ASSERT(memcmp(&valid, &valid_before, sizeof(valid)) == 0);
    crs_teardown();
}

TEST(test_receipt_trust_distinguishes_authority_policy) {
    crs_setup("trust_authority");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD103);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x13);
    uint8_t cargo_pk[32]; fill_test_cargo_pubkey(cargo_pk, 0x43);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));
    const cargo_receipt_t receipt_before = receipt;
    cargo_receipt_origin_proof_t proof = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    const cargo_receipt_origin_proof_t proof_before = proof;

    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof, CARGO_RECEIPT_AUTHORITY_UNKNOWN);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY);

    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof, CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY);

    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof, CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);

    proof.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(
        result.status,
        CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY_LIFECYCLE);

    proof.authority_lifecycle =
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);

    proof = proof_before;
    result = cargo_receipt_trust_verify(
        &receipt, 1, cargo_pk, &proof,
        (cargo_receipt_authority_trust_t)99);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS);
    ASSERT(!result.chain_checked);
    ASSERT_EQ_INT(result.authority_trust, 99);

    ASSERT(memcmp(&receipt, &receipt_before, sizeof(receipt)) == 0);
    ASSERT(memcmp(&proof, &proof_before, sizeof(proof)) == 0);
    crs_teardown();
}

TEST(test_receipt_trust_preserves_cryptographic_chain_failure) {
    crs_setup("trust_chain_failure");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD104);
    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x14);
    uint8_t cargo_pk[32]; fill_test_cargo_pubkey(cargo_pk, 0x44);
    cargo_receipt_t receipt;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &receipt));
    cargo_receipt_origin_proof_t proof = crs_origin_proof(
        &receipt, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);

    cargo_receipt_t tampered = receipt;
    tampered.signature[0] ^= 0x01u;
    const cargo_receipt_t tampered_before = tampered;
    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        &tampered, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status, CARGO_RECEIPT_TRUST_REJECT_CHAIN);
    ASSERT(result.chain_checked);
    ASSERT_EQ_INT(result.chain_result, CARGO_RECEIPT_REJECT_BAD_SIGNATURE);
    ASSERT(memcmp(&tampered, &tampered_before, sizeof(tampered)) == 0);

    result = cargo_receipt_trust_verify(
        &receipt, 1, NULL, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS);
    ASSERT(!result.chain_checked);
    ASSERT_EQ_INT(result.chain_result, CARGO_RECEIPT_REJECT_EMPTY);

    result = cargo_receipt_trust_verify(
        NULL, 1, cargo_pk, &proof,
        CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(result.status,
                  CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS);
    ASSERT(!result.chain_checked);
    crs_teardown();
}

TEST(test_receipt_trust_status_names_cover_every_verdict) {
    static const char *const expected[CARGO_RECEIPT_TRUST_STATUS_COUNT] = {
        [CARGO_RECEIPT_TRUST_VALID_TRUSTED] =
            "valid_trusted",
        [CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED] =
            "valid_trusted_rotated",
        [CARGO_RECEIPT_TRUST_REJECT_BAD_ARGUMENTS] =
            "reject_bad_arguments",
        [CARGO_RECEIPT_TRUST_REJECT_CHAIN] =
            "reject_chain",
        [CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN] =
            "reject_missing_origin",
        [CARGO_RECEIPT_TRUST_REJECT_ORIGIN_EVENT_TYPE] =
            "reject_origin_event_type",
        [CARGO_RECEIPT_TRUST_REJECT_ORIGIN_CARGO] =
            "reject_origin_cargo",
        [CARGO_RECEIPT_TRUST_REJECT_ORIGIN_PIN] =
            "reject_origin_pin",
        [CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY] =
            "reject_origin_authority",
        [CARGO_RECEIPT_TRUST_REJECT_ORIGIN_AUTHORITY_LIFECYCLE] =
            "reject_origin_authority_lifecycle",
        [CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY] =
            "reject_unknown_authority",
        [CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY] =
            "reject_untrusted_authority",
        [CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY] =
            "reject_revoked_authority",
        [CARGO_RECEIPT_TRUST_REJECT_ORIGIN_METADATA] =
            "reject_origin_metadata",
    };
    for (int i = 0; i < CARGO_RECEIPT_TRUST_STATUS_COUNT; i++) {
        ASSERT_STR_EQ(cargo_receipt_trust_status_name(
                          (cargo_receipt_trust_status_t)i),
                      expected[i]);
    }
    ASSERT_STR_EQ(cargo_receipt_trust_status_name(
                      (cargo_receipt_trust_status_t)99),
                  "unknown");
}

TEST(test_station_trust_evaluator_accepts_current_and_local_origin) {
    crs_setup("station_eval_current_local");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD105);
    uint8_t player_pk[32];
    uint8_t remote_pub[32];
    fill_test_pubkey(player_pk, 0x15);
    fill_test_cargo_pubkey(remote_pub, 0x45);
    cargo_receipt_t receipt = {0};
    ASSERT(crs_first_hop(
        w, &w->stations[2], player_pk, remote_pub, &receipt));
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

    uint8_t input_pub[32];
    fill_test_cargo_pubkey(input_pub, 0x74);
    cargo_unit_t input =
        crs_test_ingot_at(input_pub, 1);
    cargo_unit_t local = {0};
    ASSERT(hash_product(
        RECIPE_FRAME_BASIC, &input, 1, 0, &local));
    local.origin_station = 1;
    chain_payload_craft_t craft = {0};
    ASSERT(chain_payload_craft_bind_output(
        &craft, &input, 1, &local));
    ASSERT(chain_log_emit(w, &w->stations[1], CHAIN_EVT_CRAFT,
                          &craft, sizeof(craft)) >= 1);
    cargo_receipt_chain_t empty = {0};
    evaluated = cargo_receipt_evaluate_at_station(
        w, 1, &local, &empty);
    ASSERT(evaluated.accepted);
    ASSERT(evaluated.local_origin_without_receipt);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED);
    ASSERT_EQ_INT(evaluated.origin_station, 1);

    uint8_t missing_pub[32];
    fill_test_pubkey(missing_pub, 0xA5);
    cargo_unit_t missing = crs_test_ingot(missing_pub);
    evaluated = cargo_receipt_evaluate_at_station(
        w, 1, &missing, &empty);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN);
    crs_teardown();
}

TEST(test_station_trust_evaluator_accepts_rotated_origin_and_link_keys) {
    crs_setup("station_eval_rotated");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD106);
    station_t historical_origin = {0};
    station_t historical_link = {0};
    crs_init_foreign_station(&historical_origin);
    uint8_t distinct_founder[32];
    fill_test_pubkey(distinct_founder, 0x91);
    station_authority_init_outpost(
        &historical_link, distinct_founder, 778);
    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    fill_test_pubkey(player_pk, 0x16);
    fill_test_cargo_pubkey(cargo_pk, 0x46);
    cargo_receipt_t first = {0};
    cargo_receipt_t second = {0};
    ASSERT(crs_first_hop(
        w, &historical_origin, player_pk, cargo_pk, &first));

    station_t *origin_owner = &w->stations[2];
    memcpy(origin_owner->authority_registry[1].pubkey,
           historical_origin.station_pubkey, 32);
    origin_owner->authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_ROTATED;
    origin_owner->authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    origin_owner->authority_registry_count = 2;
    ASSERT(station_authority_registry_validate(origin_owner));

    station_t *link_owner = &w->stations[0];
    memcpy(link_owner->authority_registry[1].pubkey,
           historical_link.station_pubkey, 32);
    link_owner->authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_ROTATED;
    link_owner->authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    link_owner->authority_registry_count = 2;
    ASSERT(station_authority_registry_validate(link_owner));
    ASSERT(crs_next_hop(
        w, &historical_link, player_pk, cargo_pk,
        &first, 1, &second));

    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_chain_t chain = {.len = 2};
    chain.links[0] = first;
    chain.links[1] = second;
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(w, 1, &unit, &chain);
    ASSERT(evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);
    ASSERT_EQ_INT(evaluated.origin_station, 2);
    ASSERT_EQ_INT(evaluated.first_rejected_link, -1);
    crs_teardown();
}

TEST(test_station_trust_evaluator_applies_intermediate_author_policy) {
    crs_setup("station_eval_link_policy");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD107);
    station_t unknown_link = {0};
    crs_init_foreign_station(&unknown_link);
    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    fill_test_pubkey(player_pk, 0x17);
    fill_test_cargo_pubkey(cargo_pk, 0x47);
    cargo_receipt_t first = {0};
    cargo_receipt_t second = {0};
    ASSERT(crs_first_hop(
        w, &w->stations[2], player_pk, cargo_pk, &first));
    ASSERT(crs_next_hop(
        w, &unknown_link, player_pk, cargo_pk,
        &first, 1, &second));
    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_chain_t chain = {.len = 2};
    chain.links[0] = first;
    chain.links[1] = second;

    int permissive = -1;
    int tolerant = -1;
    int strict = -1;
    for (int i = 0; i < w->station_count; i++) {
        bool screens = cargo_legality_station_screens(
            &w->stations[i], i);
        bool tolerates =
            cargo_legality_station_tolerates_contraband(
                &w->stations[i], i);
        if ((!screens || tolerates) && permissive < 0)
            permissive = i;
        if (tolerates && tolerant < 0) tolerant = i;
        if (screens && !tolerates && strict < 0) strict = i;
    }
    ASSERT(permissive >= 0);
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, permissive, &unit, &chain);
    ASSERT(evaluated.accepted);
    ASSERT_EQ_INT(evaluated.first_rejected_link, -1);

    if (strict >= 0) {
        evaluated = cargo_receipt_evaluate_at_station(
            w, strict, &unit, &chain);
        ASSERT(!evaluated.accepted);
        ASSERT_EQ_INT(evaluated.first_rejected_link, 1);
        ASSERT_EQ_INT(evaluated.trust.status,
                      CARGO_RECEIPT_TRUST_REJECT_UNKNOWN_AUTHORITY);
    }

    ASSERT(tolerant >= 0);
    ASSERT(station_authority_registry_set_trust(
        &w->stations[tolerant],
        unknown_link.station_pubkey,
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    evaluated = cargo_receipt_evaluate_at_station(
        w, tolerant, &unit, &chain);
    ASSERT(evaluated.accepted);
    ASSERT_EQ_INT(evaluated.first_rejected_link, -1);

    ASSERT(station_authority_registry_set_trust(
        &w->stations[tolerant],
        unknown_link.station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));
    evaluated = cargo_receipt_evaluate_at_station(
        w, tolerant, &unit, &chain);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.first_rejected_link, 1);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);
    crs_teardown();
}

TEST(test_tolerant_station_applies_origin_author_distrust_semantically) {
    crs_setup("station_eval_origin_policy");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD109);
    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    fill_test_pubkey(player_pk, 0x19);
    fill_test_cargo_pubkey(cargo_pk, 0x49);
    cargo_receipt_t receipt = {0};
    ASSERT(crs_first_hop(
        w, &w->stations[2], player_pk, cargo_pk, &receipt));
    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_chain_t chain = {.len = 1};
    chain.links[0] = receipt;

    int tolerant = -1;
    for (int i = 0; i < w->station_count; i++) {
        if (i != 2 &&
            cargo_legality_station_tolerates_contraband(
                &w->stations[i], i)) {
            tolerant = i;
            break;
        }
    }
    ASSERT(tolerant >= 0);
    ASSERT(station_authority_registry_set_trust(
        &w->stations[tolerant],
        w->stations[2].station_pubkey,
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, tolerant, &unit, &chain);
    ASSERT(evaluated.accepted);
    ASSERT_EQ_INT(evaluated.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(evaluated.origin_station, 2);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY);

    ASSERT(station_authority_registry_set_trust(
        &w->stations[tolerant],
        w->stations[2].station_pubkey,
        CARGO_RECEIPT_AUTHORITY_REVOKED));
    evaluated = cargo_receipt_evaluate_at_station(
        w, tolerant, &unit, &chain);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);
    crs_teardown();
}

TEST(test_tolerant_station_rejects_foreign_chainless_cargo) {
    crs_setup("station_eval_foreign_chainless");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD108);
    station_t foreign = {0};
    crs_init_foreign_station(&foreign);
    uint8_t cargo_pk[32];
    fill_test_pubkey(cargo_pk, 0x48);
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pk, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(
        w, &foreign, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) != 0);

    int tolerant = -1;
    for (int i = 0; i < w->station_count; i++) {
        if (cargo_legality_station_tolerates_contraband(
                &w->stations[i], i)) {
            tolerant = i;
            break;
        }
    }
    ASSERT(tolerant >= 0);
    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    unit.origin_station = (uint8_t)((tolerant + 1) %
                                    w->station_count);
    cargo_receipt_chain_t empty = {0};
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, tolerant, &unit, &empty);
    ASSERT(!evaluated.accepted);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN);
    ASSERT((evaluated.legality.reasons &
            CARGO_LEGALITY_REASON_MISSING_RECEIPT) != 0);
    crs_teardown();
}

TEST(test_local_origin_resolver_exact_proofs_and_structured_states) {
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
    cargo_receipt_origin_proof_t proof;
    memset(&proof, 0xA5, sizeof(proof));

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, smelt_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    cargo_receipt_origin_proof_t zero_proof = {0};
    ASSERT(memcmp(&proof, &zero_proof, sizeof(proof)) == 0);

    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, smelt_pub, sizeof(smelt.ingot_pub));
    w->time = 1.25f;
    ASSERT(chain_log_emit(w, st, CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) == 1);
    uint8_t smelt_hash[32];
    memcpy(smelt_hash, st->chain_last_hash, sizeof(smelt_hash));

    chain_payload_craft_t craft = {0};
    craft.recipe_id = (uint16_t)RECIPE_FRAME_BASIC;
    craft.input_count = 1;
    memcpy(craft.input_pubs[0], smelt_pub, sizeof(smelt_pub));
    memcpy(craft.output_pub, craft_pub, sizeof(craft.output_pub));
    w->time = 2.5f;
    ASSERT(chain_log_emit(w, st, CHAIN_EVT_CRAFT,
                          &craft, sizeof(craft)) == 2);
    uint8_t craft_hash[32];
    memcpy(craft_hash, st->chain_last_hash, sizeof(craft_hash));

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, smelt_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    ASSERT_EQ_INT(proof.authority_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    ASSERT_EQ_INT((int)proof.event_id, 1);
    ASSERT_EQ_INT((int)proof.epoch, 150);
    ASSERT(memcmp(proof.event_hash, smelt_hash, sizeof(smelt_hash)) == 0);
    ASSERT(memcmp(proof.output_cargo_pub, smelt_pub, sizeof(smelt_pub)) == 0);
    ASSERT(memcmp(proof.authority, st->station_pubkey, 32) == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, craft_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_CRAFT);
    ASSERT_EQ_INT(proof.authority_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    ASSERT_EQ_INT((int)proof.event_id, 2);
    ASSERT_EQ_INT((int)proof.epoch, 300);
    ASSERT(memcmp(proof.event_hash, craft_hash, sizeof(craft_hash)) == 0);
    ASSERT(memcmp(proof.output_cargo_pub, craft_pub, sizeof(craft_pub)) == 0);
    ASSERT(memcmp(proof.authority, st->station_pubkey, 32) == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, unknown_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND);
    ASSERT(memcmp(&proof, &zero_proof, sizeof(proof)) == 0);

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

    station_t *truncated = &w->stations[1];
    chain_payload_smelt_t truncated_smelt = {0};
    memcpy(truncated_smelt.ingot_pub, unknown_pub,
           sizeof(truncated_smelt.ingot_pub));
    ASSERT(chain_log_emit(w, truncated, CHAIN_EVT_SMELT,
                          &truncated_smelt, sizeof(truncated_smelt)) == 1);
    ASSERT(chain_log_path_for(truncated->station_pubkey,
                              path, sizeof(path)));
    f = fopen(path, "ab");
    ASSERT(f != NULL);
    ASSERT(fputc(0xA5, f) != EOF);
    ASSERT(fclose(f) == 0);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(
            truncated, unknown_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);

    station_t *disk_disabled = &w->stations[0];
    chain_payload_smelt_t disabled_smelt = {0};
    uint8_t disabled_pub[32];
    fill_test_pubkey(disabled_pub, 0xB1);
    memcpy(disabled_smelt.ingot_pub, disabled_pub,
           sizeof(disabled_smelt.ingot_pub));
    ASSERT(chain_log_emit(w, disk_disabled, CHAIN_EVT_SMELT,
                          &disabled_smelt, sizeof(disabled_smelt)) == 1);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(
            disk_disabled, disabled_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    chain_log_set_disk_enabled(false);
    ASSERT(!chain_log_disk_enabled());
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(
            disk_disabled, disabled_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    chain_log_set_disk_enabled(true);

    uint8_t zero_pub[32] = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(st, zero_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS);
    crs_teardown();
}

TEST(test_origin_cache_is_bounded_by_verified_disk_and_registry_state) {
    crs_setup("origin_cache_invalidation");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD116);
    station_t *st = &w->stations[2];
    uint8_t cargo_pub[32];
    fill_test_pubkey(cargo_pub, 0x37);
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pub, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(
        w, st, CHAIN_EVT_SMELT, &smelt, sizeof(smelt)) == 1);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t stats =
        cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 1);
    ASSERT(stats.misses == 1);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    stats = cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 1);
    ASSERT(stats.hits == 1);

    uint64_t verified_before = stats.full_verifications;
    ASSERT(chain_log_emit(
        w, st, CHAIN_EVT_LEDGER, "append", 6) == 2);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    stats = cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications > verified_before);

    char original_dir[256];
    snprintf(original_dir, sizeof(original_dir), "%s",
             chain_log_get_dir());
    char alternate_dir[256];
    snprintf(alternate_dir, sizeof(alternate_dir), "%s_alt",
             original_dir);
    chain_log_set_dir(alternate_dir);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    chain_log_set_dir(original_dir);
    verified_before =
        cargo_receipt_origin_cache_stats().full_verifications;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT(cargo_receipt_origin_cache_stats().full_verifications >
           verified_before);

    chain_log_reset(st);
    st->chain_event_count = 0;
    memset(st->chain_last_hash, 0, sizeof(st->chain_last_hash));
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    ASSERT(chain_log_emit(
        w, st, CHAIN_EVT_SMELT, &smelt, sizeof(smelt)) == 1);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);

    char path[256];
    ASSERT(chain_log_path_for(
        st->station_pubkey, path, sizeof(path)));
    struct stat cached_file_state;
    ASSERT(stat(path, &cached_file_state) == 0);
    FILE *history = fopen(path, "r+b");
    ASSERT(history != NULL);
    ASSERT(fseek(history, -1, SEEK_END) == 0);
    int original_byte = fgetc(history);
    ASSERT(original_byte != EOF);
    ASSERT(fseek(history, -1, SEEK_CUR) == 0);
    ASSERT(fputc(original_byte ^ 0x01, history) != EOF);
    ASSERT(fclose(history) == 0);
    ASSERT(crs_restore_file_times_seconds(
        path, &cached_file_state));
    verified_before =
        cargo_receipt_origin_cache_stats().full_verifications;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);
    cargo_receipt_origin_cache_stats_t invalid_cached =
        cargo_receipt_origin_cache_stats();
    ASSERT(invalid_cached.full_verifications > verified_before);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);
    cargo_receipt_origin_cache_stats_t invalid_hit =
        cargo_receipt_origin_cache_stats();
    ASSERT(invalid_hit.full_verifications ==
           invalid_cached.full_verifications);
    ASSERT(invalid_hit.hits == invalid_cached.hits + 1u);

    history = fopen(path, "r+b");
    ASSERT(history != NULL);
    ASSERT(fseek(history, -1, SEEK_END) == 0);
    ASSERT(fputc(original_byte, history) != EOF);
    ASSERT(fclose(history) == 0);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, st->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);

    uint8_t historical_authority[32];
    memcpy(historical_authority, st->station_pubkey, 32);
    station_t replacement = {0};
    crs_init_foreign_station(&replacement);
    memcpy(st->station_pubkey, replacement.station_pubkey, 32);
    memcpy(st->station_secret, replacement.station_secret, 64);
    memset(st->authority_registry, 0,
           sizeof(st->authority_registry));
    st->authority_registry_version =
        STATION_AUTHORITY_REGISTRY_VERSION;
    st->authority_registry_count = 2;
    memcpy(st->authority_registry[0].pubkey,
           replacement.station_pubkey, 32);
    st->authority_registry[0].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_CURRENT;
    st->authority_registry[0].trust =
        STATION_AUTHORITY_TRUST_CURRENT;
    memcpy(st->authority_registry[1].pubkey,
           historical_authority, 32);
    st->authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_ROTATED;
    st->authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    ASSERT(station_authority_registry_validate(st));
    verified_before =
        cargo_receipt_origin_cache_stats().full_verifications;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            st, historical_authority, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(
        proof.authority_lifecycle,
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    ASSERT(cargo_receipt_origin_cache_stats().full_verifications >
           verified_before);
    crs_teardown();
}

TEST(test_origin_index_resolves_240_distinct_outputs_with_one_verification) {
    crs_setup("origin_index_distinct_outputs");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD117);
    station_t *origin = &w->stations[2];

    enum { OUTPUT_COUNT = 240 };
    uint8_t cargo_pubs[OUTPUT_COUNT][32];
    chain_payload_smelt_t payloads[OUTPUT_COUNT];
    chain_log_batch_event_t events[OUTPUT_COUNT];
    memset(payloads, 0, sizeof(payloads));
    for (size_t i = 0; i < OUTPUT_COUNT; i++) {
        fill_indexed_test_pubkey(
            cargo_pubs[i], 0xA7, (uint16_t)i);
        memcpy(payloads[i].ingot_pub, cargo_pubs[i], 32);
        events[i] = (chain_log_batch_event_t){
            CHAIN_EVT_SMELT,
            &payloads[i],
            (uint16_t)sizeof(payloads[i]),
        };
    }
    ASSERT_EQ_INT(
        chain_log_emit_batch(
            w, origin, events,
            CHAIN_LOG_BATCH_MAX_EVENTS).status,
        CHAIN_LOG_APPEND_OK);
    ASSERT_EQ_INT(
        chain_log_emit_batch(
            w, origin, &events[CHAIN_LOG_BATCH_MAX_EVENTS],
            OUTPUT_COUNT - CHAIN_LOG_BATCH_MAX_EVENTS).status,
        CHAIN_LOG_APPEND_OK);
    ASSERT(origin->chain_event_count == OUTPUT_COUNT);

    cargo_receipt_origin_proof_t proof = {0};
    cargo_receipt_origin_cache_reset();
    for (size_t i = 0; i < OUTPUT_COUNT; i++) {
        ASSERT_EQ_INT(
            cargo_receipt_resolve_origin_for_authority(
                origin, origin->station_pubkey,
                cargo_pubs[i], &proof),
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
        ASSERT(proof.event_id == i + 1u);
        ASSERT(memcmp(proof.output_cargo_pub,
                      cargo_pubs[i], 32) == 0);
    }
    cargo_receipt_origin_cache_stats_t stats =
        cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 1);
    ASSERT(stats.index_builds == 1);
    ASSERT(stats.misses == 1);
    ASSERT(stats.hits == OUTPUT_COUNT - 1u);
    ASSERT(stats.fallback_scans == 0);
    crs_teardown();
}

TEST(test_origin_index_grows_past_2048_without_fallback_scans) {
    crs_setup("origin_index_growth");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD11B);
    station_t *origin = &w->stations[2];

    enum {
        DISTINCT_OUTPUT_COUNT = 2049,
        HOT_LOOKUP_COUNT = 10000,
    };
    uint8_t (*cargo_pubs)[32] = calloc(
        DISTINCT_OUTPUT_COUNT, sizeof(*cargo_pubs));
    chain_payload_smelt_t *payloads = calloc(
        DISTINCT_OUTPUT_COUNT, sizeof(*payloads));
    chain_log_batch_event_t *events = calloc(
        DISTINCT_OUTPUT_COUNT, sizeof(*events));
    ASSERT(cargo_pubs != NULL);
    ASSERT(payloads != NULL);
    ASSERT(events != NULL);
    for (size_t i = 0; i < DISTINCT_OUTPUT_COUNT; i++) {
        fill_indexed_test_pubkey(
            cargo_pubs[i], 0xA8, (uint16_t)i);
        memcpy(payloads[i].ingot_pub, cargo_pubs[i], 32);
        events[i] = (chain_log_batch_event_t){
            CHAIN_EVT_SMELT,
            &payloads[i],
            (uint16_t)sizeof(payloads[i]),
        };
    }

    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &payloads[0], sizeof(payloads[0])) == 1);
    uint8_t first_hash[32];
    memcpy(first_hash, origin->chain_last_hash, 32);
    for (size_t offset = 1;
         offset < DISTINCT_OUTPUT_COUNT;) {
        size_t batch_count =
            DISTINCT_OUTPUT_COUNT - offset;
        if (batch_count > CHAIN_LOG_BATCH_MAX_EVENTS)
            batch_count = CHAIN_LOG_BATCH_MAX_EVENTS;
        chain_log_append_result_t appended =
            chain_log_emit_batch(
                w, origin, &events[offset], batch_count);
        ASSERT_EQ_INT(appended.status, CHAIN_LOG_APPEND_OK);
        ASSERT(appended.event_count == batch_count);
        offset += batch_count;
    }

    chain_payload_smelt_t duplicate = {0};
    memcpy(duplicate.ingot_pub, cargo_pubs[0], 32);
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &duplicate, sizeof(duplicate)) ==
        DISTINCT_OUTPUT_COUNT + 1u);
    uint8_t duplicate_hash[32];
    memcpy(duplicate_hash, origin->chain_last_hash, 32);
    ASSERT(origin->chain_event_count ==
           DISTINCT_OUTPUT_COUNT + 1u);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_proof_t proof = {0};
    size_t lookup_count = 0;
    const size_t probes[] = {
        1u,
        1024u,
        DISTINCT_OUTPUT_COUNT - 1u,
    };
    for (size_t i = 0;
         i < sizeof(probes) / sizeof(probes[0]); i++) {
        size_t index = probes[i];
        ASSERT_EQ_INT(
            cargo_receipt_resolve_origin_for_authority(
                origin, origin->station_pubkey,
                cargo_pubs[index], &proof),
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
        ASSERT(proof.event_id == index + 1u);
        lookup_count++;
    }

    uint8_t missing_pub[32];
    fill_indexed_test_pubkey(missing_pub, 0xE8, 0x5151);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            missing_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND);
    lookup_count++;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pubs[0], &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS);
    lookup_count++;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority_pinned(
            origin, origin->station_pubkey,
            cargo_pubs[0], first_hash, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT(proof.event_id == 1u);
    lookup_count++;
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority_pinned(
            origin, origin->station_pubkey,
            cargo_pubs[0], duplicate_hash, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT(proof.event_id == DISTINCT_OUTPUT_COUNT + 1u);
    lookup_count++;

    for (size_t i = 0; i < HOT_LOOKUP_COUNT; i++) {
        size_t index =
            1u + ((i * 4051u) %
                  (DISTINCT_OUTPUT_COUNT - 1u));
        ASSERT_EQ_INT(
            cargo_receipt_resolve_origin_for_authority(
                origin, origin->station_pubkey,
                cargo_pubs[index], &proof),
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
        ASSERT(proof.event_id == index + 1u);
        lookup_count++;
    }

    cargo_receipt_origin_cache_stats_t stats =
        cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 1);
    ASSERT(stats.index_builds == 1);
    ASSERT(stats.misses == 1);
    ASSERT(stats.hits == lookup_count - 1u);
    ASSERT(stats.fallback_scans == 0);
    free(events);
    free(payloads);
    free(cargo_pubs);
    crs_teardown();
}

TEST(test_origin_index_growth_failure_is_cached_fail_closed) {
    crs_setup("origin_index_growth_failure");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD11C);
    station_t *origin = &w->stations[2];

    enum { OUTPUT_COUNT = 65 };
    uint8_t cargo_pubs[OUTPUT_COUNT][32];
    chain_payload_smelt_t payloads[OUTPUT_COUNT];
    chain_log_batch_event_t events[OUTPUT_COUNT];
    memset(payloads, 0, sizeof(payloads));
    for (size_t i = 0; i < OUTPUT_COUNT; i++) {
        fill_indexed_test_pubkey(
            cargo_pubs[i], 0xA9, (uint16_t)i);
        memcpy(payloads[i].ingot_pub, cargo_pubs[i], 32);
        events[i] = (chain_log_batch_event_t){
            CHAIN_EVT_SMELT,
            &payloads[i],
            (uint16_t)sizeof(payloads[i]),
        };
    }
    ASSERT_EQ_INT(
        chain_log_emit_batch(
            w, origin, events, OUTPUT_COUNT).status,
        CHAIN_LOG_APPEND_OK);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_cache_test_set_record_limit(64);
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pubs[OUTPUT_COUNT - 1u], &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    cargo_receipt_origin_cache_stats_t before =
        cargo_receipt_origin_cache_stats();
    ASSERT(before.full_verifications == 1);
    ASSERT(before.index_builds == 1);
    ASSERT(before.misses == 1);
    ASSERT(before.hits == 0);
    ASSERT(before.fallback_scans == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pubs[0], &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    cargo_receipt_origin_cache_stats_t after =
        cargo_receipt_origin_cache_stats();
    ASSERT(after.full_verifications == before.full_verifications);
    ASSERT(after.index_builds == before.index_builds);
    ASSERT(after.misses == before.misses);
    ASSERT(after.hits == before.hits + 1u);
    ASSERT(after.fallback_scans == 0);

    cargo_receipt_origin_cache_reset();
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pubs[OUTPUT_COUNT - 1u], &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT(proof.event_id == OUTPUT_COUNT);
    crs_teardown();
}

TEST(test_origin_index_transient_allocation_failure_retries) {
    crs_setup("origin_index_allocation_retry");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD11D);
    station_t *origin = &w->stations[2];
    uint8_t cargo_pub[32];
    fill_indexed_test_pubkey(cargo_pub, 0xAA, 1);
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pub, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) == 1);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_cache_test_fail_next_record_allocation();
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    cargo_receipt_origin_cache_stats_t first =
        cargo_receipt_origin_cache_stats();
    ASSERT(first.full_verifications == 1);
    ASSERT(first.index_builds == 1);
    ASSERT(first.misses == 1);
    ASSERT(first.hits == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t second =
        cargo_receipt_origin_cache_stats();
    ASSERT(second.full_verifications == 2);
    ASSERT(second.index_builds == 2);
    ASSERT(second.misses == 2);
    ASSERT(second.hits == 0);
    ASSERT(proof.event_id == 1);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t third =
        cargo_receipt_origin_cache_stats();
    ASSERT(third.full_verifications == second.full_verifications);
    ASSERT(third.index_builds == second.index_builds);
    ASSERT(third.misses == second.misses);
    ASSERT(third.hits == second.hits + 1u);
    ASSERT(third.fallback_scans == 0);
    crs_teardown();
}

TEST(test_origin_cache_transient_snapshot_open_failure_retries) {
    crs_setup("origin_cache_snapshot_open_retry");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD11E);
    station_t *origin = &w->stations[2];
    uint8_t cargo_pub[32];
    fill_indexed_test_pubkey(cargo_pub, 0xAB, 1);
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pub, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) == 1);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_cache_test_fail_next_snapshot_open();
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE);
    cargo_receipt_origin_cache_stats_t first =
        cargo_receipt_origin_cache_stats();
    ASSERT(first.full_verifications == 0);
    ASSERT(first.index_builds == 0);
    ASSERT(first.misses == 1);
    ASSERT(first.hits == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t second =
        cargo_receipt_origin_cache_stats();
    ASSERT(second.full_verifications == 1);
    ASSERT(second.index_builds == 1);
    ASSERT(second.misses == 2);
    ASSERT(second.hits == 0);
    ASSERT_EQ_INT((int)proof.event_id, 1);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t third =
        cargo_receipt_origin_cache_stats();
    ASSERT(third.full_verifications == second.full_verifications);
    ASSERT(third.misses == second.misses);
    ASSERT(third.hits == second.hits + 1u);
    crs_teardown();
}

TEST(test_origin_cache_hits_avoid_content_scans_with_native_metadata) {
    crs_setup("origin_cache_native_metadata");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD11F);
    station_t *origin = &w->stations[2];
    uint8_t cargo_pub[32];
    fill_indexed_test_pubkey(cargo_pub, 0xAC, 1);
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pub, sizeof(smelt.ingot_pub));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) == 1);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t before =
        cargo_receipt_origin_cache_stats();

    enum { REPEATED_HITS = 16 };
    for (int i = 0; i < REPEATED_HITS; i++) {
        ASSERT_EQ_INT(
            cargo_receipt_resolve_origin_for_authority(
                origin, origin->station_pubkey, cargo_pub, &proof),
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    }
    cargo_receipt_origin_cache_stats_t after =
        cargo_receipt_origin_cache_stats();
    ASSERT(after.full_verifications == before.full_verifications);
    ASSERT(after.index_builds == before.index_builds);
    ASSERT(after.hits == before.hits + REPEATED_HITS);
    ASSERT(after.fallback_scans == 0);
#if defined(_WIN32)
    if (before.file_native_metadata_queries > 0) {
        ASSERT(after.file_native_metadata_queries >
               before.file_native_metadata_queries);
        ASSERT(before.file_digest_scans == 0);
        ASSERT(after.file_digest_scans == 0);
    } else {
        /* Unsupported filesystems deliberately retain the digest fallback. */
        ASSERT(after.file_digest_scans > before.file_digest_scans);
    }
#else
    ASSERT(after.file_native_metadata_queries == 0);
    ASSERT(after.file_digest_scans == 0);
#endif
    crs_teardown();
}

TEST(test_origin_cache_does_not_retain_build_race_failure) {
    crs_setup("origin_cache_build_race");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD119);
    station_t *origin = &w->stations[2];
    uint8_t cargo_pub[32];
    fill_test_pubkey(cargo_pub, 0x97);
    chain_payload_smelt_t smelt = {0};
    memcpy(smelt.ingot_pub, cargo_pub, 32);
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) == 1);

    cargo_receipt_origin_cache_reset();
    crs_origin_cache_build_mutation_t mutation = {
        .world = w,
        .station = origin,
    };
    cargo_receipt_origin_cache_test_set_build_hook(
        crs_mutate_chain_during_origin_cache_build,
        &mutation);
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);
    ASSERT(mutation.emitted_event_id == 2);
    cargo_receipt_origin_cache_stats_t stats =
        cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 1);
    ASSERT(stats.index_builds == 1);
    ASSERT(stats.misses == 1);
    ASSERT(stats.hits == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    stats = cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 2);
    ASSERT(stats.index_builds == 2);
    ASSERT(stats.misses == 2);
    ASSERT(stats.hits == 0);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    stats = cargo_receipt_origin_cache_stats();
    ASSERT(stats.full_verifications == 2);
    ASSERT(stats.hits == 1);
    crs_teardown();
}

#if !defined(_WIN32)
TEST(test_rotated_origin_cache_rejects_stat_fopen_aba) {
    crs_setup("origin_cache_rotated_aba");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD120);
    station_t *historical = &w->stations[2];
    station_t *owner = &w->stations[1];
    uint8_t historical_authority[32];
    memcpy(historical_authority, historical->station_pubkey, 32);

    uint8_t cargo_a[32];
    uint8_t cargo_b[32];
    fill_indexed_test_pubkey(cargo_a, 0xAD, 1);
    fill_indexed_test_pubkey(cargo_b, 0xAD, 2);
    chain_payload_smelt_t smelt_a = {0};
    chain_payload_smelt_t smelt_b = {0};
    memcpy(smelt_a.ingot_pub, cargo_a, sizeof(smelt_a.ingot_pub));
    memcpy(smelt_b.ingot_pub, cargo_b, sizeof(smelt_b.ingot_pub));
    ASSERT(chain_log_emit(
        w, historical, CHAIN_EVT_SMELT,
        &smelt_a, sizeof(smelt_a)) == 1);

    char original_dir[256];
    snprintf(original_dir, sizeof(original_dir), "%s",
             chain_log_get_dir());
    char live_path[320];
    ASSERT(chain_log_path_for(
        historical_authority, live_path, sizeof(live_path)));
    uint64_t original_event_count = historical->chain_event_count;
    uint8_t original_last_hash[32];
    memcpy(original_last_hash, historical->chain_last_hash,
           sizeof(original_last_hash));

    char alternate_dir[256];
    int alternate_dir_len = snprintf(
        alternate_dir, sizeof(alternate_dir),
        "%s_alternate", original_dir);
    ASSERT(alternate_dir_len > 0);
    ASSERT((size_t)alternate_dir_len < sizeof(alternate_dir));
    chain_log_set_dir(alternate_dir);
    historical->chain_event_count = 0;
    memset(historical->chain_last_hash, 0,
           sizeof(historical->chain_last_hash));
    ASSERT(chain_log_emit(
        w, historical, CHAIN_EVT_SMELT,
        &smelt_b, sizeof(smelt_b)) == 1);
    char alternate_path[320];
    ASSERT(chain_log_path_for(
        historical_authority, alternate_path,
        sizeof(alternate_path)));
    chain_log_set_dir(original_dir);
    historical->chain_event_count = original_event_count;
    memcpy(historical->chain_last_hash, original_last_hash,
           sizeof(historical->chain_last_hash));

    owner->authority_registry_count = 2;
    memcpy(owner->authority_registry[1].pubkey,
           historical_authority, 32);
    owner->authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_ROTATED;
    owner->authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    ASSERT(station_authority_registry_validate(owner));

    crs_origin_cache_aba_swap_t swap = {0};
    snprintf(swap.live_path, sizeof(swap.live_path),
             "%s", live_path);
    snprintf(swap.alternate_path, sizeof(swap.alternate_path),
             "%s", alternate_path);
    int hold_path_len = snprintf(
        swap.original_hold_path,
        sizeof(swap.original_hold_path),
        "%s.aba-hold", live_path);
    ASSERT(hold_path_len > 0);
    ASSERT((size_t)hold_path_len <
           sizeof(swap.original_hold_path));

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_cache_test_set_snapshot_hook(
        crs_swap_origin_path_around_snapshot_open, &swap);
    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            owner, historical_authority, cargo_b, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);
    ASSERT_EQ_INT(swap.original_to_hold, 0);
    ASSERT_EQ_INT(swap.alternate_to_live, 0);
    ASSERT_EQ_INT(swap.live_to_alternate, 0);
    ASSERT_EQ_INT(swap.hold_to_live, 0);
    cargo_receipt_origin_proof_t zero_proof = {0};
    ASSERT(memcmp(&proof, &zero_proof, sizeof(proof)) == 0);
    cargo_receipt_origin_cache_stats_t first =
        cargo_receipt_origin_cache_stats();
    ASSERT(first.misses == 1);
    ASSERT(first.hits == 0);
    ASSERT(first.full_verifications == 0);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            owner, historical_authority, cargo_a, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t second =
        cargo_receipt_origin_cache_stats();
    ASSERT(second.misses == 2);
    ASSERT(second.hits == 0);
    ASSERT(second.full_verifications == 1);
    ASSERT_EQ_INT((int)proof.event_id, 1);

    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            owner, historical_authority, cargo_b, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND);
    cargo_receipt_origin_cache_stats_t third =
        cargo_receipt_origin_cache_stats();
    ASSERT(third.full_verifications == second.full_verifications);
    ASSERT(third.misses == second.misses);
    ASSERT(third.hits == second.hits + 1u);
    ASSERT(third.fallback_scans == 0);
    crs_teardown();
}
#endif

TEST(test_rotated_origin_cache_ignores_current_authority_append) {
    crs_setup("origin_cache_rotated_stability");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD11A);
    station_t *historical = &w->stations[2];
    station_t *owner = &w->stations[1];
    uint8_t historical_authority[32];
    memcpy(historical_authority, historical->station_pubkey, 32);

    enum { HISTORICAL_OUTPUT_COUNT = 3 };
    uint8_t cargo_pubs[HISTORICAL_OUTPUT_COUNT][32];
    chain_payload_smelt_t payloads[HISTORICAL_OUTPUT_COUNT];
    chain_log_batch_event_t events[HISTORICAL_OUTPUT_COUNT];
    memset(payloads, 0, sizeof(payloads));
    for (size_t i = 0; i < HISTORICAL_OUTPUT_COUNT; i++) {
        fill_indexed_test_pubkey(
            cargo_pubs[i], 0xB7, (uint16_t)i);
        memcpy(payloads[i].ingot_pub, cargo_pubs[i], 32);
        events[i] = (chain_log_batch_event_t){
            CHAIN_EVT_SMELT,
            &payloads[i],
            (uint16_t)sizeof(payloads[i]),
        };
    }
    ASSERT_EQ_INT(
        chain_log_emit_batch(
            w, historical, events,
            HISTORICAL_OUTPUT_COUNT).status,
        CHAIN_LOG_APPEND_OK);

    owner->authority_registry_count = 2;
    memcpy(owner->authority_registry[1].pubkey,
           historical_authority, 32);
    owner->authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_ROTATED;
    owner->authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    ASSERT(station_authority_registry_validate(owner));

    cargo_receipt_origin_cache_reset();
    cargo_receipt_origin_proof_t proof = {0};
    for (size_t i = 0; i < HISTORICAL_OUTPUT_COUNT; i++) {
        ASSERT_EQ_INT(
            cargo_receipt_resolve_origin_for_authority(
                owner, historical_authority,
                cargo_pubs[i], &proof),
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
        ASSERT_EQ_INT(
            proof.authority_lifecycle,
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    }
    cargo_receipt_origin_cache_stats_t before =
        cargo_receipt_origin_cache_stats();
    ASSERT(before.full_verifications == 1);
    ASSERT(before.index_builds == 1);

    static const uint8_t unrelated[] = "current-authority-append";
    ASSERT(chain_log_emit(
        w, owner, CHAIN_EVT_LEDGER,
        unrelated, sizeof(unrelated)) == 1);
    for (size_t i = 0; i < HISTORICAL_OUTPUT_COUNT; i++) {
        ASSERT_EQ_INT(
            cargo_receipt_resolve_origin_for_authority(
                owner, historical_authority,
                cargo_pubs[i], &proof),
            CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    }
    cargo_receipt_origin_cache_stats_t after =
        cargo_receipt_origin_cache_stats();
    ASSERT(after.full_verifications ==
           before.full_verifications);
    ASSERT(after.index_builds == before.index_builds);
    ASSERT(after.hits ==
           before.hits + HISTORICAL_OUTPUT_COUNT);
    crs_teardown();
}

TEST(test_current_origin_cache_advances_only_across_trusted_transfers) {
    crs_setup("origin_cache_trusted_transfer");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD11B);
    station_t *origin = &w->stations[2];

    uint8_t first_owner[32];
    uint8_t second_owner[32];
    uint8_t cargo_pub[32];
    fill_test_pubkey(first_owner, 0x61);
    fill_test_pubkey(second_owner, 0xA1);
    fill_test_cargo_pubkey(cargo_pub, 0xC1);
    cargo_unit_t unit = crs_test_ingot_at(cargo_pub, 2);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_chain_t chain = {.len = 1};
    ASSERT(crs_first_hop(
        w, origin, first_owner, cargo_pub, &chain.links[0]));
    cargo_receipt_origin_cache_stats_t initial =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT((int)initial.full_verifications, 1);
    ASSERT_EQ_INT((int)initial.index_builds, 1);

    cargo_receipt_transfer_commit_result_t returned =
        cargo_receipt_commit_transfer(
            w, 2, first_owner, origin->station_pubkey,
            &unit, &chain, true, 7, first_owner);
    ASSERT_EQ_INT(
        returned.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        returned.append.status, CHAIN_LOG_APPEND_OK);
    ASSERT_EQ_INT((int)returned.append.event_count, 2);
    chain.links[chain.len++] = returned.receipt;

    cargo_receipt_origin_cache_stats_t after_return =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT(
        (int)after_return.full_verifications,
        (int)initial.full_verifications);
    ASSERT_EQ_INT(
        (int)after_return.index_builds,
        (int)initial.index_builds);

    cargo_receipt_transfer_commit_result_t forwarded =
        cargo_receipt_commit_transfer(
            w, 2, origin->station_pubkey, second_owner,
            &unit, &chain, false, 0, NULL);
    ASSERT_EQ_INT(
        forwarded.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        forwarded.append.status, CHAIN_LOG_APPEND_OK);
    ASSERT_EQ_INT((int)forwarded.append.event_count, 1);

    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t after_forward =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT(
        (int)after_forward.full_verifications,
        (int)initial.full_verifications);
    ASSERT_EQ_INT(
        (int)after_forward.index_builds,
        (int)initial.index_builds);

    /*
     * The optimization is deliberately transfer-only. A new transform changes
     * the semantic index and therefore forces a complete verification/build.
     */
    uint8_t later_pub[32];
    fill_test_cargo_pubkey(later_pub, 0xC2);
    cargo_unit_t later_unit =
        crs_test_ingot_at(later_pub, 2);
    chain_payload_smelt_t later_smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &later_smelt, later_unit.parent_merkle,
        0, &later_unit));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &later_smelt, sizeof(later_smelt)) != 0);
    memset(&proof, 0, sizeof(proof));
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            later_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_origin_cache_stats_t after_transform =
        cargo_receipt_origin_cache_stats();
    ASSERT(
        after_transform.full_verifications >
        after_forward.full_verifications);
    ASSERT(
        after_transform.index_builds >
        after_forward.index_builds);
    crs_teardown();
}

TEST(test_trusted_transfer_cache_rejects_tampered_cached_prefix) {
    crs_setup("origin_cache_trusted_transfer_tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD11C);
    station_t *origin = &w->stations[2];

    uint8_t owner[32];
    uint8_t cargo_pub[32];
    fill_test_pubkey(owner, 0x62);
    fill_test_cargo_pubkey(cargo_pub, 0xC3);
    cargo_unit_t unit = crs_test_ingot_at(cargo_pub, 2);
    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, unit.parent_merkle, 0, &unit));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) != 0);

    cargo_receipt_origin_cache_reset();
    cargo_receipt_chain_t empty = {0};
    cargo_receipt_prepared_transfer_t prepared =
        cargo_receipt_prepare_transfer(
            w, 2, origin->station_pubkey, owner,
            &unit, &empty, false, 0, NULL);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        prepared.preflight_status, CHAIN_LOG_APPEND_OK);
    cargo_receipt_origin_cache_stats_t before =
        cargo_receipt_origin_cache_stats();
    ASSERT_EQ_INT((int)before.full_verifications, 1);
    ASSERT_EQ_INT((int)before.index_builds, 1);

    char path[256];
    ASSERT(chain_log_path_for(
        origin->station_pubkey, path, sizeof(path)));
    struct stat cached_state;
    ASSERT(stat(path, &cached_state) == 0);
    FILE *history = fopen(path, "r+b");
    ASSERT(history != NULL);
    ASSERT(fseek(
        history,
        CHAIN_EVENT_HEADER_SIZE + (long)sizeof(uint16_t),
        SEEK_SET) == 0);
    int original_byte = fgetc(history);
    ASSERT(original_byte != EOF);
    ASSERT(fseek(history, -1, SEEK_CUR) == 0);
    ASSERT(fputc(original_byte ^ 0x01, history) != EOF);
    ASSERT(fclose(history) == 0);
    ASSERT(crs_restore_file_times_seconds(
        path, &cached_state));

    /*
     * The normal writer can still append a correctly signed event from its
     * in-memory head. The trusted-cache token must notice that the prefix no
     * longer equals its snapshot and refuse to advance stale proof records.
     */
    chain_log_append_result_t append =
        cargo_receipt_commit_prepared_transfer(
            w, &prepared);
    ASSERT_EQ_INT(append.status, CHAIN_LOG_APPEND_OK);

    cargo_receipt_origin_proof_t proof;
    memset(&proof, 0xA5, sizeof(proof));
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID);
    cargo_receipt_origin_proof_t zero_proof = {0};
    ASSERT(memcmp(
        &proof, &zero_proof, sizeof(proof)) == 0);
    cargo_receipt_origin_cache_stats_t after =
        cargo_receipt_origin_cache_stats();
    ASSERT(
        after.full_verifications >
        before.full_verifications);
    crs_teardown();
}

TEST(test_duplicate_output_requires_exact_receipt_origin_pin) {
    crs_setup("origin_duplicate_pin");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL);
    crs_world_init(w, 0xD118);
    station_t *origin = &w->stations[2];
    uint8_t player_pub[32];
    uint8_t cargo_pub[32];
    fill_test_pubkey(player_pub, 0x57);
    fill_test_cargo_pubkey(cargo_pub, 0x87);
    cargo_unit_t rejected_unit =
        crs_test_ingot_at(cargo_pub, 2);

    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, rejected_unit.parent_merkle,
        0, &rejected_unit));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) == 1);
    uint8_t smelt_hash[32];
    memcpy(smelt_hash, origin->chain_last_hash, 32);

    chain_payload_craft_t craft = {0};
    craft.recipe_id = RECIPE_FRAME_BASIC;
    craft.input_count = 1;
    memcpy(craft.output_pub, cargo_pub, 32);
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_CRAFT,
        &craft, sizeof(craft)) == 2);
    uint8_t craft_hash[32];
    memcpy(craft_hash, origin->chain_last_hash, 32);

    cargo_receipt_origin_proof_t proof = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_local_origin(
            origin, cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority(
            origin, origin->station_pubkey,
            cargo_pub, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority_pinned(
            origin, origin->station_pubkey,
            cargo_pub, smelt_hash, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(
        proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_SMELT);
    ASSERT_EQ_INT((int)proof.event_id, 1);
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority_pinned(
            origin, origin->station_pubkey,
            cargo_pub, craft_hash, &proof),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(
        proof.event_type, CARGO_RECEIPT_ORIGIN_EVENT_CRAFT);
    ASSERT_EQ_INT((int)proof.event_id, 2);

    cargo_receipt_chain_t empty = {0};
    cargo_receipt_t rejected = {0};
    ASSERT(cargo_receipt_emit_transfer(
        w, origin, origin->station_pubkey,
        player_pub, &rejected_unit,
        &empty, &rejected) == 0);

    cargo_receipt_t pinned = {0};
    uint64_t transfer_id = origin->chain_event_count + 1u;
    ASSERT(cargo_receipt_issue(
        origin, 0, transfer_id, cargo_pub,
        player_pub, smelt_hash, &pinned));
    chain_payload_transfer_t transfer = {0};
    memcpy(transfer.from_pubkey,
           origin->station_pubkey, 32);
    memcpy(transfer.to_pubkey, player_pub, 32);
    memcpy(transfer.cargo_pub, cargo_pub, 32);
    transfer.kind = CARGO_KIND_INGOT;
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_TRANSFER,
        &transfer, sizeof(transfer)) == transfer_id);

    cargo_receipt_chain_t chain = {.len = 1};
    chain.links[0] = pinned;
    cargo_unit_t unit = crs_test_ingot(cargo_pub);
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, 1, &unit, &chain);
    ASSERT(evaluated.accepted);
    ASSERT_EQ_INT(
        evaluated.origin_status,
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    crs_teardown();
}

TEST(test_local_origin_resolver_status_names_cover_contract) {
    static const char *const expected[
        CARGO_RECEIPT_ORIGIN_RESOLVE_STATUS_COUNT] = {
        [CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED] =
            "not_attempted",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED] =
            "verified",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS] =
            "bad_arguments",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE] =
            "history_unavailable",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID] =
            "history_invalid",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND] =
            "transform_not_found",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS] =
            "transform_ambiguous",
        [CARGO_RECEIPT_ORIGIN_RESOLVE_ALREADY_TRANSFERRED] =
            "already_transferred",
    };
    for (int i = 0; i < CARGO_RECEIPT_ORIGIN_RESOLVE_STATUS_COUNT; i++) {
        ASSERT_STR_EQ(cargo_receipt_origin_resolve_status_name(
                          (cargo_receipt_origin_resolve_status_t)i),
                      expected[i]);
    }
    ASSERT_STR_EQ(cargo_receipt_origin_resolve_status_name(
                      (cargo_receipt_origin_resolve_status_t)99),
                  "unknown");
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
    int origin_station =
        crs_receipt_origin_station(w, &incoming[0]);
    if (origin_station < 0) origin_station = 2;
    cargo_unit_t unit =
        crs_test_ingot_at(cargo_pk, origin_station);
    if (crs_world_station_index(w, dst) >= 0) {
        return cargo_receipt_emit_transfer(
                   w, dst, from_pk,
                   dst->station_pubkey, &unit,
                   &chain, out) != 0;
    }
    if (memcmp(
            incoming[incoming_len - 1u].recipient_pubkey,
            from_pk, 32) != 0) {
        return false;
    }
    uint8_t previous_hash[32];
    cargo_receipt_hash(
        &incoming[incoming_len - 1u],
        previous_hash);
    return crs_emit_external_test_transfer(
        w, dst, from_pk, dst->station_pubkey,
        &unit, previous_hash, out);
}

TEST(test_cross_station_two_hop_chain) {
    crs_setup("two_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD002);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x20);
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0x50);

    /* Hop 1: Helios -> player. */
    station_t *helios = &w->stations[2];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Hop 2: player -> Kepler. Kepler verifies r1 then signs r2 whose
     * prev_receipt_hash = SHA-256(r1). */
    station_t *kepler = &w->stations[1];
    cargo_receipt_chain_t incoming = {0};
    incoming.links[0] = r1;
    incoming.len = 1;
    cargo_receipt_transfer_link_t hop_link =
        cargo_receipt_prepare_transfer_link(
            kepler, player_pk, cargo_pk, &incoming);
    ASSERT_EQ_INT(hop_link.status, CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(hop_link.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED);
    cargo_receipt_t r2;
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, 1, &r2));
    uint8_t r1_hash[32];
    cargo_receipt_hash(&r1, r1_hash);
    ASSERT(memcmp(hop_link.prev_receipt_hash, r1_hash,
                  sizeof(r1_hash)) == 0);
    ASSERT(memcmp(r2.prev_receipt_hash, r1_hash, sizeof(r1_hash)) == 0);

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

TEST(test_transfer_rejects_valid_chain_stolen_from_recipient) {
    crs_setup("stolen_chain_custody");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD020);

    uint8_t owner_pk[32];
    uint8_t thief_pk[32];
    uint8_t cargo_pk[32];
    fill_test_pubkey(owner_pk, 0x21);
    fill_test_pubkey(thief_pk, 0x81);
    fill_test_cargo_pubkey(cargo_pk, 0x51);

    cargo_receipt_chain_t incoming = {.len = 1};
    ASSERT(crs_first_hop(
        w, &w->stations[2], owner_pk, cargo_pk,
        &incoming.links[0]));
    ASSERT_EQ_INT(
        cargo_receipt_chain_verify(
            incoming.links, incoming.len, cargo_pk),
        CARGO_RECEIPT_OK);
    ASSERT(memcmp(
        incoming.links[0].recipient_pubkey,
        owner_pk, sizeof(owner_pk)) == 0);

    station_t *destination = &w->stations[1];
    cargo_receipt_transfer_link_t owned =
        cargo_receipt_prepare_transfer_link(
            destination, owner_pk, cargo_pk, &incoming);
    ASSERT_EQ_INT(
        owned.status, CARGO_RECEIPT_TRANSFER_LINK_READY);

    cargo_receipt_transfer_link_t stolen =
        cargo_receipt_prepare_transfer_link(
            destination, thief_pk, cargo_pk, &incoming);
    ASSERT_EQ_INT(stolen.chain_result, CARGO_RECEIPT_OK);
    ASSERT_EQ_INT(
        stolen.status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);
    ASSERT_EQ_INT(
        cargo_receipt_prepare_transfer_link(
            destination, NULL, cargo_pk, &incoming).status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);

    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, 1, &unit, &incoming);
    ASSERT(evaluated.accepted);

    const uint64_t event_count_before =
        destination->chain_event_count;
    uint8_t chain_head_before[32];
    memcpy(chain_head_before, destination->chain_last_hash,
           sizeof(chain_head_before));

    cargo_receipt_prepared_transfer_t prepared =
        cargo_receipt_prepare_transfer(
            w, 1, thief_pk, destination->station_pubkey,
            &unit, &incoming, false, 0, NULL);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);
    ASSERT_EQ_INT(
        prepared.preflight_status,
        CHAIN_LOG_APPEND_BAD_ARGUMENTS);
    cargo_receipt_t zero_receipt = {0};
    ASSERT(memcmp(
        &prepared.receipt, &zero_receipt,
        sizeof(zero_receipt)) == 0);

    cargo_receipt_t emitted;
    memset(&emitted, 0xA5, sizeof(emitted));
    ASSERT_EQ_INT(
        cargo_receipt_emit_transfer(
            w, destination, thief_pk,
            destination->station_pubkey,
            &unit, &incoming, &emitted),
        0);
    ASSERT(memcmp(
        &emitted, &zero_receipt,
        sizeof(zero_receipt)) == 0);

    cargo_receipt_transfer_commit_result_t committed =
        cargo_receipt_commit_transfer(
            w, 1, thief_pk, destination->station_pubkey,
            &unit, &incoming, false, 0, NULL);
    ASSERT_EQ_INT(
        committed.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);
    ASSERT_EQ_INT(
        committed.append.status,
        CHAIN_LOG_APPEND_BAD_ARGUMENTS);
    ASSERT(memcmp(
        &committed.receipt, &zero_receipt,
        sizeof(zero_receipt)) == 0);

    ASSERT_EQ_INT(
        (int)destination->chain_event_count,
        (int)event_count_before);
    ASSERT(memcmp(
        destination->chain_last_hash, chain_head_before,
        sizeof(chain_head_before)) == 0);
    crs_teardown();
}

TEST(test_prepare_transfer_reuses_one_verified_chain_walk) {
    crs_setup("prepare_single_chain_walk");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD022);

    uint8_t owner_pk[32];
    uint8_t thief_pk[32];
    uint8_t cargo_pk[32];
    fill_test_pubkey(owner_pk, 0x23);
    fill_test_pubkey(thief_pk, 0x83);
    fill_test_cargo_pubkey(cargo_pk, 0x53);

    cargo_receipt_chain_t incoming = {.len = 1};
    ASSERT(crs_first_hop(
        w, &w->stations[2], owner_pk, cargo_pk,
        &incoming.links[0]));
    cargo_unit_t unit = crs_test_ingot(cargo_pk);
    station_t *destination = &w->stations[1];

#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    /* The standalone public helper remains a full validator. */
    cargo_receipt_test_reset_chain_verify_walks();
#endif
    cargo_receipt_transfer_link_t standalone =
        cargo_receipt_prepare_transfer_link(
            destination, owner_pk, cargo_pk, &incoming);
    ASSERT_EQ_INT(
        standalone.status, CARGO_RECEIPT_TRANSFER_LINK_READY);
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    ASSERT_EQ_INT(
        (int)cargo_receipt_test_chain_verify_walks(), 1);
    cargo_receipt_test_reset_chain_verify_walks();
#endif

    cargo_receipt_prepared_transfer_t prepared =
        cargo_receipt_prepare_transfer(
            w, 1, owner_pk, destination->station_pubkey,
            &unit, &incoming, false, 0, NULL);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        prepared.preflight_status,
        CHAIN_LOG_APPEND_OK);
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    /* Station trust walked the chain once; linkage reused that verdict. */
    ASSERT_EQ_INT(
        (int)cargo_receipt_test_chain_verify_walks(), 1);
    cargo_receipt_test_reset_chain_verify_walks();
#endif

    prepared = cargo_receipt_prepare_transfer(
        w, 1, thief_pk, destination->station_pubkey,
        &unit, &incoming, false, 0, NULL);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);
    ASSERT_EQ_INT(
        prepared.preflight_status,
        CHAIN_LOG_APPEND_BAD_ARGUMENTS);
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    ASSERT_EQ_INT(
        (int)cargo_receipt_test_chain_verify_walks(), 1);
    cargo_receipt_test_reset_chain_verify_walks();
#endif

    cargo_receipt_chain_t malformed = incoming;
    malformed.links[0].signature[0] ^= 0x80u;
    prepared = cargo_receipt_prepare_transfer(
        w, 1, owner_pk, destination->station_pubkey,
        &unit, &malformed, false, 0, NULL);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_TRUST);
    ASSERT_EQ_INT(
        prepared.preflight_status,
        CHAIN_LOG_APPEND_BAD_ARGUMENTS);
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    ASSERT_EQ_INT(
        (int)cargo_receipt_test_chain_verify_walks(), 1);
#endif

    crs_teardown();
}

TEST(test_first_hop_requires_station_custody_and_prepared_state_is_sealed) {
    crs_setup("first_hop_custody_and_prepare_seal");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    crs_world_init(w, 0xD021);

    station_t *origin = &w->stations[2];
    uint8_t player_pk[32];
    uint8_t cargo_pk[32];
    uint8_t zero32[32] = {0};
    fill_test_pubkey(player_pk, 0x22);
    fill_test_cargo_pubkey(cargo_pk, 0x52);
    cargo_unit_t unit = crs_test_ingot_at(cargo_pk, 2);
    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, unit.parent_merkle, 0, &unit));
    ASSERT(chain_log_emit(
        w, origin, CHAIN_EVT_SMELT,
        &smelt, sizeof(smelt)) != 0);

    cargo_receipt_chain_t empty = {0};
    const uint64_t count_before = origin->chain_event_count;
    uint8_t head_before[32];
    memcpy(head_before, origin->chain_last_hash, 32);
    cargo_receipt_t zero_receipt = {0};

    ASSERT_EQ_INT(
        cargo_receipt_prepare_transfer_link(
            origin, player_pk, cargo_pk, &empty).status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);

    cargo_receipt_prepared_transfer_t rejected =
        cargo_receipt_prepare_transfer(
            w, 2, player_pk, player_pk, &unit, &empty,
            false, 0, NULL);
    ASSERT_EQ_INT(
        rejected.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);
    ASSERT_EQ_INT(
        rejected.preflight_status,
        CHAIN_LOG_APPEND_BAD_ARGUMENTS);

    cargo_receipt_t emitted;
    memset(&emitted, 0xA5, sizeof(emitted));
    ASSERT_EQ_INT(
        cargo_receipt_emit_transfer(
            w, origin, player_pk, player_pk,
            &unit, &empty, &emitted),
        0);
    ASSERT(memcmp(&emitted, &zero_receipt,
                  sizeof(emitted)) == 0);

    cargo_receipt_transfer_commit_result_t committed =
        cargo_receipt_commit_transfer(
            w, 2, player_pk, player_pk, &unit, &empty,
            false, 0, NULL);
    ASSERT_EQ_INT(
        committed.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY);
    ASSERT_EQ_INT(
        committed.append.status,
        CHAIN_LOG_APPEND_BAD_ARGUMENTS);

    /* Recipients are durable identities, never a zero-key placeholder. */
    rejected = cargo_receipt_prepare_transfer(
        w, 2, origin->station_pubkey, zero32,
        &unit, &empty, false, 0, NULL);
    ASSERT_EQ_INT(
        rejected.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS);
    memset(&emitted, 0xA5, sizeof(emitted));
    ASSERT_EQ_INT(
        cargo_receipt_emit_transfer(
            w, origin, origin->station_pubkey, zero32,
            &unit, &empty, &emitted),
        0);
    ASSERT(memcmp(&emitted, &zero_receipt,
                  sizeof(emitted)) == 0);
    committed = cargo_receipt_commit_transfer(
        w, 2, origin->station_pubkey, zero32,
        &unit, &empty, false, 0, NULL);
    ASSERT_EQ_INT(
        committed.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS);

    cargo_receipt_t raw = {0};
    ASSERT(!cargo_receipt_issue(
        origin, 1, 0, cargo_pk, player_pk,
        head_before, &raw));
    ASSERT(!cargo_receipt_issue(
        origin, 1, count_before + 1u, zero32,
        player_pk, head_before, &raw));
    ASSERT(!cargo_receipt_issue(
        origin, 1, count_before + 1u, cargo_pk,
        zero32, head_before, &raw));
    ASSERT(!cargo_receipt_issue(
        origin, 1, count_before + 1u, cargo_pk,
        player_pk, zero32, &raw));
    ASSERT(memcmp(&raw, &zero_receipt, sizeof(raw)) == 0);

    cargo_receipt_prepared_transfer_t prepared =
        cargo_receipt_prepare_transfer(
            w, 2, origin->station_pubkey, player_pk,
            &unit, &empty, true, -17, player_pk);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        prepared.preflight_status,
        CHAIN_LOG_APPEND_OK);

    static const cargo_receipt_result_t
        zero_verdicts[3] = {
            CARGO_RECEIPT_REJECT_ZERO_CARGO,
            CARGO_RECEIPT_REJECT_ZERO_RECIPIENT,
            CARGO_RECEIPT_REJECT_ZERO_EVENT_ID,
        };
    for (int mutation = 0; mutation < 3; mutation++) {
        cargo_receipt_t invalid = prepared.receipt;
        if (mutation == 0)
            memset(invalid.cargo_pub, 0, 32);
        else if (mutation == 1)
            memset(invalid.recipient_pubkey, 0, 32);
        else
            invalid.event_id = 0;
        uint8_t unsigned_receipt[CARGO_RECEIPT_UNSIGNED_SIZE];
        cargo_receipt_unsigned_pack(
            &invalid, unsigned_receipt);
        station_sign(
            origin, unsigned_receipt,
            sizeof(unsigned_receipt), invalid.signature);
        ASSERT(cargo_receipt_verify_signature(&invalid));
        ASSERT_EQ_INT(
            cargo_receipt_chain_verify(
                &invalid, 1, cargo_pk),
            zero_verdicts[mutation]);
    }

    for (int mutation = 0; mutation < 4; mutation++) {
        cargo_receipt_prepared_transfer_t tampered = prepared;
        switch (mutation) {
            case 0:
                tampered.transfer.cargo_pub[0] ^= 0x80u;
                break;
            case 1:
                tampered.transfer.to_pubkey[0] ^= 0x40u;
                break;
            case 2:
                tampered.trade.ledger_delta_signed++;
                break;
            case 3:
                tampered.trade.transfer_event_id++;
                break;
            default:
                ASSERT(false);
        }
        chain_log_append_result_t append =
            cargo_receipt_commit_prepared_transfer(
                w, &tampered);
        ASSERT_EQ_INT(
            append.status, CHAIN_LOG_APPEND_BAD_ARGUMENTS);
        ASSERT_EQ_INT(
            (int)origin->chain_event_count,
            (int)count_before);
        ASSERT(memcmp(origin->chain_last_hash,
                      head_before, 32) == 0);
    }

    crs_teardown();
}

/* ---------------- Test 3: forged receipt rejection ------------------ */

TEST(test_cross_station_forged_receipt_rejected) {
    crs_setup("forged");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD003);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x30);
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0x60);

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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0x70);

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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0x80);

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
    uint8_t alt_origin_hash[32];
    memcpy(alt_origin_hash, helios->chain_last_hash, 32);
    uint64_t alt_id = helios->chain_event_count + 1u;
    ASSERT(cargo_receipt_issue(
        helios, (uint64_t)(w->time * 120.0),
        alt_id, cargo_pk, player_pk,
        alt_origin_hash, &r1_alt));
    chain_payload_transfer_t alt_transfer = {0};
    memcpy(alt_transfer.from_pubkey,
           helios->station_pubkey, 32);
    memcpy(alt_transfer.to_pubkey, player_pk, 32);
    memcpy(alt_transfer.cargo_pub, cargo_pk, 32);
    alt_transfer.kind = (uint8_t)CARGO_KIND_INGOT;
    ASSERT(chain_log_emit(
        w, helios, CHAIN_EVT_TRANSFER,
        &alt_transfer, sizeof(alt_transfer)) == alt_id);
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0x90);

    /* Hop 1: Prospect -> player. */
    station_t *prospect = &w->stations[0];
    station_t *kepler   = &w->stations[1];
    station_t *helios   = &w->stations[2];

    cargo_receipt_t r1, r2, r3;
    ASSERT(crs_first_hop(w, prospect, player_pk, cargo_pk, &r1));
    /* Hop 2: player -> Kepler. */
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, 1, &r2));
    /* Hop 3: Kepler -> Helios. The extender must be the exact recipient
     * of r2; copying the chain back to the player would not transfer
     * custody. */
    cargo_receipt_t first_two[2] = {r1, r2};
    ASSERT(crs_next_hop(w, helios, kepler->station_pubkey, cargo_pk,
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
    uint8_t cargo_pk[32]; fill_test_cargo_pubkey(cargo_pk, 0xA0);

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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xB0);

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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xC0);

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
        ASSERT(crs_next_hop(
            w, next, chain[i - 1].recipient_pubkey, cargo_pk,
                            chain, (uint8_t)i, &chain[i]));
    }
    station_t *overflow = &w->stations[1];
    ASSERT(!crs_next_hop(
        w, overflow,
        chain[CARGO_RECEIPT_CHAIN_MAX_LEN - 1u].recipient_pubkey,
        cargo_pk,
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD0);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(w, 0, sp, cargo_pk, &r1, 1)
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xE2);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD1);
    station_t foreign;
    crs_init_foreign_station(&foreign);
    for (int i = 0; i < 3; i++) {
        ASSERT(memcmp(foreign.station_pubkey,
                      w->stations[i].station_pubkey, 32) != 0);
    }

    cargo_unit_t foreign_unit =
        crs_test_ingot_at(cargo_pk, 2);
    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, foreign_unit.parent_merkle,
        0, &foreign_unit));
    ASSERT(chain_log_emit(w, &foreign, CHAIN_EVT_SMELT,
                          &smelt, sizeof(smelt)) >= 1);
    uint8_t origin_hash[32];
    memcpy(origin_hash, foreign.chain_last_hash,
           sizeof(origin_hash));
    cargo_receipt_t r1;
    ASSERT(crs_emit_external_test_transfer(
        w, &foreign, foreign.station_pubkey,
        player_pk, &foreign_unit, origin_hash, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(w, 0, sp, cargo_pk, &r1, 1)
           == CARGO_RECEIPT_PRESENT_REJECT_TRUST);
    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 0);

    crs_teardown();
}

/* ---------------- Test 12: presented chain must name bearer --------- */

TEST(test_present_receipt_chain_rejects_wrong_recipient) {
    crs_setup("present_recipient");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00C);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA2);
    uint8_t other_pk[32];  fill_test_pubkey(other_pk,  0xB2);
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD2);
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, &w->stations[2], other_pk, cargo_pk, &r1));
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, NULL));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(w, 0, sp, cargo_pk, &r1, 1)
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD3);
    cargo_receipt_t original;
    cargo_receipt_t alternate;
    ASSERT(crs_first_hop(w, &w->stations[2], player_pk, cargo_pk, &original));
    /*
     * Canonical production now rejects a second empty-chain transfer for the
     * same durable identity. Exercise the defensive mismatch guard with a
     * deliberately off-log but correctly signed alternate first hop instead.
     */
    cargo_unit_t unit =
        crs_test_ingot_at(cargo_pk, 2);
    ASSERT(cargo_receipt_issue(
        &w->stations[2], original.epoch + 1u,
        original.event_id + 1u, unit.pub, player_pk,
        original.prev_receipt_hash, &alternate));
    ASSERT(memcmp(&original, &alternate, sizeof(original)) != 0);

    cargo_receipt_chain_t existing = {0};
    existing.links[0] = original;
    existing.len = 1;
    ASSERT(crs_prepare_player_carrier(w, player_pk, cargo_pk, &existing));

    server_player_t *sp = &w->players[0];
    ASSERT(cargo_receipt_present_to_ship(w, 0, sp, cargo_pk, &alternate, 1)
           == CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH);
    ship_receipts_t *rcpts = ship_get_receipts(sp->ship);
    ASSERT(rcpts != NULL);
    ASSERT_EQ_INT((int)rcpts->chains[0].len, 1);
    ASSERT(memcmp(&rcpts->chains[0].links[0], &original, sizeof(original)) == 0);

    crs_teardown();
}

/* ---------------- Test 14: handoff ticket round-trip --------------- */

TEST(test_handoff_ticket_roundtrip_verifies_ship_and_cargo) {
    crs_setup("handoff_roundtrip");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00E);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA4);
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD4);
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

TEST(test_handoff_ticket_issue_key_mismatch_fails_closed) {
    crs_setup("handoff_key_mismatch");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00E);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xB4);
    SHIP_DECL(ship);
    handoff_ticket_t ticket;
    handoff_ticket_t zero = {0};

    memset(&ticket, 0xA5, sizeof(ticket));
    ASSERT(!handoff_ticket_issue_for_ship(
        w->stations[2].station_pubkey,
        w->stations[1].station_secret,
        w->stations[1].station_pubkey,
        player_pk,
        2u, 1u, 100u, 160u, &ship, &ticket));
    ASSERT(memcmp(&ticket, &zero, sizeof(ticket)) == 0);

    memset(&ticket, 0xA5, sizeof(ticket));
    ASSERT(!handoff_ticket_issue_for_ship(
        NULL,
        w->stations[2].station_secret,
        w->stations[1].station_pubkey,
        player_pk,
        2u, 1u, 100u, 160u, &ship, &ticket));
    ASSERT(memcmp(&ticket, &zero, sizeof(ticket)) == 0);

    crs_teardown();
}

/* ---------------- Test 15: handoff binds ship state ---------------- */

TEST(test_handoff_ticket_rejects_tampered_ship_state) {
    crs_setup("handoff_ship_tamper");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD00F);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0xA5);
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD5);
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD6);
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
    cargo_receipt_chain_t tampered = rcpts->chains[0];
    tampered.links[0].signature[0] ^= 0x01u;
    ASSERT(ship_receipts_set_chain(rcpts, 0, &tampered));
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD7);
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD8);
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xD9);
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
    source_sp->pubkey_proof_ok = false;
    source_sp->pubkey_challenge_consumed = false;
    ASSERT(!handoff_issue_ticket_to_station(
        src, 0, 2, 1, 240u, &ticket));
    source_sp->pubkey_proof_ok = true;
    source_sp->pubkey_challenge_consumed = true;
    ASSERT(handoff_issue_ticket_to_station(src, 0, 2, 1, 240u, &ticket));

    server_player_t *dest_sp = &dst->players[0];
    player_init_ship(dest_sp, dst);
    dest_sp->connected = true;
    dest_sp->session_ready = true;
    dest_sp->pubkey_set = true;
    memcpy(dest_sp->pubkey, player_pk, 32);
    ASSERT_EQ_INT((int)dest_sp->ship->manifest.count, 0);

    int dest_station = -1;
    ASSERT(handoff_accept_presented_ship(dst, 0, &ticket, source_sp->ship,
                                         &dest_station) ==
           HANDOFF_FLOW_REJECT_NO_PLAYER_KEY);
    dest_sp->pubkey_proof_ok = true;
    dest_sp->pubkey_challenge_consumed = true;
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xE0);
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xE1);
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xDA);
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
    dest_sp->session_ready = true;
    dest_sp->pubkey_set = true;
    dest_sp->pubkey_proof_ok = true;
    dest_sp->pubkey_challenge_consumed = true;
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
    uint8_t cargo_pk[32];  fill_test_cargo_pubkey(cargo_pk,  0xDB);
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
    RUN(test_receipt_trust_status_names_cover_every_verdict);
    RUN(test_station_trust_evaluator_accepts_current_and_local_origin);
    RUN(test_station_trust_evaluator_accepts_rotated_origin_and_link_keys);
    RUN(test_station_trust_evaluator_applies_intermediate_author_policy);
    RUN(test_tolerant_station_applies_origin_author_distrust_semantically);
    RUN(test_tolerant_station_rejects_foreign_chainless_cargo);
    RUN(test_local_origin_resolver_exact_proofs_and_structured_states);
    RUN(test_origin_cache_is_bounded_by_verified_disk_and_registry_state);
    RUN(test_origin_index_resolves_240_distinct_outputs_with_one_verification);
    RUN(test_origin_index_grows_past_2048_without_fallback_scans);
    RUN(test_origin_index_growth_failure_is_cached_fail_closed);
    RUN(test_origin_index_transient_allocation_failure_retries);
    RUN(test_origin_cache_transient_snapshot_open_failure_retries);
    RUN(test_origin_cache_hits_avoid_content_scans_with_native_metadata);
    RUN(test_origin_cache_does_not_retain_build_race_failure);
#if !defined(_WIN32)
    RUN(test_rotated_origin_cache_rejects_stat_fopen_aba);
#endif
    RUN(test_rotated_origin_cache_ignores_current_authority_append);
    RUN(test_current_origin_cache_advances_only_across_trusted_transfers);
    RUN(test_trusted_transfer_cache_rejects_tampered_cached_prefix);
    RUN(test_duplicate_output_requires_exact_receipt_origin_pin);
    RUN(test_local_origin_resolver_status_names_cover_contract);
    RUN(test_cross_station_two_hop_chain);
    RUN(test_transfer_rejects_valid_chain_stolen_from_recipient);
    RUN(test_prepare_transfer_reuses_one_verified_chain_walk);
    RUN(test_first_hop_requires_station_custody_and_prepared_state_is_sealed);
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
    RUN(test_handoff_ticket_roundtrip_verifies_ship_and_cargo);
    RUN(test_handoff_ticket_issue_key_mismatch_fails_closed);
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
