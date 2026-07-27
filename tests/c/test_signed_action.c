/*
 * test_signed_action.c — Layer A.3 of #479: signed state-changing actions.
 *
 * The wire dispatcher lives in server/main.c; these tests exercise the
 * authenticator (signed_action_verify) directly with hand-built buffers,
 * plus the nonce persistence path through player_save / player_load.
 *
 * What the wire flow looks like once these primitives compose:
 *   client                                   server
 *   ------                                   ------
 *   sign(nonce||type||len||payload, sk)
 *   send NET_MSG_SIGNED_ACTION             -> verify(pubkey, sig)
 *                                          -> nonce > last_signed_nonce ?
 *                                          -> apply intent, bump nonce
 *
 * Each test below covers one rejection path (or the happy path).
 */
#include "test_harness.h"

#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "protocol.h"
#include "manifest.h"
#include "signal_crypto.h"

/* ---- helpers ----------------------------------------------------- */

static void fill_token(uint8_t tok[8], uint8_t seed) {
    for (int i = 0; i < 8; i++) tok[i] = (uint8_t)(seed * 7 + i);
}

/* Pack a NET_MSG_SIGNED_ACTION onto the wire.
 *   buf must be at least SIGNED_ACTION_HEADER_SIZE + payload_len + 64 bytes.
 * Returns the written length. The signature is computed with `secret`. */
static int build_signed_action(uint8_t *buf, size_t buf_cap,
                               uint64_t nonce, uint8_t action_type,
                               const uint8_t *payload, uint16_t payload_len,
                               const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    int total = SIGNED_ACTION_HEADER_SIZE + (int)payload_len +
                (int)SIGNED_ACTION_SIG_SIZE;
    if (buf_cap < (size_t)total) return 0;
    buf[0] = NET_MSG_SIGNED_ACTION;
    /* nonce LE */
    for (int i = 0; i < 8; i++) buf[1 + i] = (uint8_t)(nonce >> (i * 8));
    buf[9]  = action_type;
    buf[10] = (uint8_t)(payload_len & 0xFF);
    buf[11] = (uint8_t)(payload_len >> 8);
    if (payload && payload_len) memcpy(&buf[12], payload, payload_len);
    /* Signature covers (nonce || action_type || payload_len || payload) =
     * exactly bytes [1..12+payload_len). */
    signal_crypto_sign(&buf[12 + payload_len],
                       &buf[1], (size_t)(11 + (int)payload_len),
                       secret);
    return total;
}

/* Stand up a world with a single registered player at slot 0 whose
 * pubkey + session_token are returned via *out_secret. */
static void setup_player_with_keypair(world_t *w, int slot,
                                      uint8_t out_secret[SIGNAL_CRYPTO_SECRET_BYTES],
                                      uint8_t out_pubkey[32]) {
    signal_crypto_keypair(out_pubkey, out_secret);
    server_player_t *sp = &w->players[slot];
    sp->connected = true;
    sp->id = (uint8_t)slot;
    fill_token(sp->session_token, (uint8_t)(slot + 1));
    sp->session_ready = true;
    memcpy(sp->pubkey, out_pubkey, 32);
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    ASSERT(server_finalize_pubkey_identity(w, slot));
}

/* ---- tests ------------------------------------------------------- */

TEST(test_signed_action_happy_path) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    uint8_t payload[2] = { (uint8_t)COMMODITY_FERRITE_INGOT,
                           (uint8_t)MINING_GRADE_COMMON };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), /*nonce=*/100,
                                SIGNED_ACTION_BUY_PRODUCT,
                                payload, sizeof(payload), sk);
    ASSERT(n == (int)sizeof(buf));

    uint8_t got_type = 0;
    uint64_t got_nonce = 0;
    const uint8_t *got_payload = NULL;
    uint16_t got_len = 0;
    signed_action_result_t res =
        signed_action_verify(w, /*player_idx=*/0, buf, n,
                             &got_type, &got_nonce, &got_payload, &got_len);
    ASSERT_EQ_INT(res, SIGNED_ACTION_OK);
    ASSERT_EQ_INT(got_type, SIGNED_ACTION_BUY_PRODUCT);
    ASSERT(got_nonce == 100);
    ASSERT_EQ_INT(got_len, 2);
    ASSERT(got_payload[0] == COMMODITY_FERRITE_INGOT);
    ASSERT(got_payload[1] == MINING_GRADE_COMMON);
}

TEST(test_signed_action_dispatch_buy_product_payload) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);
    server_player_t *sp = &w->players[0];

    uint8_t payload[4] = {
        (uint8_t)COMMODITY_FERRITE_INGOT,
        (uint8_t)MINING_GRADE_RARE,
        0x34,
        0x12,
    };
    server_signed_action_dispatch_result_t result;
    ASSERT(server_dispatch_signed_action_payload(
        w, 0, SIGNED_ACTION_BUY_PRODUCT, payload, sizeof(payload),
        NULL, NULL, &result));

    ASSERT(sp->input.buy_product);
    ASSERT_EQ_INT(sp->input.buy_commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(sp->input.buy_grade, MINING_GRADE_RARE);
    ASSERT(sp->pending_action_result_valid);
    ASSERT_EQ_INT(sp->pending_action_result_id, 0x1234);
    ASSERT_EQ_INT(sp->pending_action_result_action,
                  NET_ACTION_BUY_PRODUCT + COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(result.station_identity_dirty, -1);
}

TEST(test_signed_action_dispatch_input_action_marks_station_dirty) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);
    server_player_t *sp = &w->players[0];
    sp->docked = true;
    sp->current_station = 2;

    uint8_t payload[7] = {
        (uint8_t)(NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_FURNACE),
        MINING_GRADE_COUNT,
        0xFF,
        0xFF,
        0xFF,
        0x78,
        0x56,
    };
    server_signed_action_dispatch_result_t result;
    ASSERT(server_dispatch_signed_action_payload(
        w, 0, SIGNED_ACTION_INPUT_ACTION, payload, sizeof(payload),
        NULL, NULL, &result));

    ASSERT(sp->input.buy_scaffold_kit);
    ASSERT_EQ_INT(sp->input.scaffold_kit_module, MODULE_FURNACE);
    ASSERT(sp->pending_action_result_valid);
    ASSERT_EQ_INT(sp->pending_action_result_id, 0x5678);
    ASSERT_EQ_INT(sp->pending_action_result_action,
                  NET_ACTION_BUY_SCAFFOLD_TYPED + MODULE_FURNACE);
    ASSERT_EQ_INT(result.station_identity_dirty, 2);
}

TEST(test_legacy_unsigned_dispatch_rejects_pubkey_player) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    uint8_t plan_msg[NET_PLAN_MSG_SIZE] = { NET_MSG_PLAN };
    uint8_t buy_msg[33] = { NET_MSG_BUY_INGOT };
    uint8_t deliver_msg[2] = { NET_MSG_DELIVER_INGOT, 0 };
    uint8_t fracture_msg[FRACTURE_CLAIM_SIZE] = { NET_MSG_FRACTURE_CLAIM };
    server_unsigned_dispatch_result_t result;

    ASSERT(server_dispatch_legacy_plan_message(
        w, 0, plan_msg, sizeof(plan_msg), &result));
    ASSERT(result.rejected_unsigned_action);
    result.rejected_unsigned_action = false;

    ASSERT(server_dispatch_legacy_buy_ingot_message(
        w, 0, buy_msg, sizeof(buy_msg), NULL, NULL, &result));
    ASSERT(result.rejected_unsigned_action);
    result.rejected_unsigned_action = false;

    ASSERT(server_dispatch_legacy_deliver_ingot_message(
        w, 0, deliver_msg, sizeof(deliver_msg), NULL, NULL, &result));
    ASSERT(result.rejected_unsigned_action);
    result.rejected_unsigned_action = false;

    ASSERT(server_dispatch_fracture_claim_message(
        w, 0, fracture_msg, sizeof(fracture_msg), &result));
    ASSERT(result.rejected_unsigned_action);
}

TEST(test_anonymous_legacy_named_cargo_actions_are_inert) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->id = 0;
    fill_token(sp->session_token, 10);
    sp->session_ready = true;
    sp->docked = true;
    sp->current_station = 0;
    sp->nearby_station = 0;
    /* No verified pubkey: session tokens must not become cargo custody. */
    sp->pubkey_set = false;
    sp->pubkey_proof_ok = false;
    sp->pubkey_challenge_consumed = false;
    memset(sp->pubkey, 0, sizeof(sp->pubkey));

    station_t *station = &w->stations[0];
    ASSERT(station_manifest_bootstrap(station));
    ASSERT(ship_manifest_bootstrap(sp->ship));
    manifest_clear(&station->manifest);
    ship_receipts_clear(station_get_receipts(station));
    manifest_clear(&sp->ship->manifest);
    ship_receipts_clear(ship_get_receipts(sp->ship));

    cargo_unit_t named[2] = {{0}};
    uint8_t fragment_pub[32] = {0};
    int found = 0;
    for (uint16_t index = 0; index < UINT16_MAX && found < 2;
         index++) {
        fragment_pub[0] = (uint8_t)index;
        fragment_pub[1] = (uint8_t)(index >> 8);
        cargo_unit_t candidate = {0};
        ASSERT(hash_ingot(
            COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
            fragment_pub, index, &candidate));
        if (candidate.prefix_class ==
            (uint8_t)INGOT_PREFIX_ANONYMOUS) {
            continue;
        }
        candidate.origin_station = 0;
        candidate.mined_block = (uint64_t)index + 1u;
        named[found++] = candidate;
    }
    ASSERT_EQ_INT(found, 2);
    ASSERT(station_manifest_push_with_chain(
        station, &named[0], NULL));
    ASSERT(ship_manifest_push_with_chain(
        sp->ship, &named[1], NULL));
    ledger_earn(station, sp->session_token, 50000.0f);

    const float balance_before =
        ledger_balance(station, sp->session_token);
    const uint16_t station_count_before =
        station->manifest.count;
    const uint16_t ship_count_before =
        sp->ship->manifest.count;
    const uint64_t chain_count_before =
        station->chain_event_count;
    uint8_t chain_head_before[32];
    memcpy(chain_head_before, station->chain_last_hash, 32);
    cargo_unit_t station_unit_before =
        station->manifest.units[0];
    cargo_unit_t ship_unit_before =
        sp->ship->manifest.units[0];

    uint8_t buy_msg[33] = {NET_MSG_BUY_INGOT};
    memcpy(&buy_msg[1], named[0].pub, 32);
    server_legacy_cargo_dispatch_result_t dispatch = {0};
    ASSERT(server_dispatch_legacy_buy_ingot_message(
        w, 0, buy_msg, sizeof(buy_msg),
        NULL, NULL, &dispatch));
    ASSERT(!dispatch.rejected_unsigned_action);

    uint8_t deliver_msg[2] = {
        NET_MSG_DELIVER_INGOT, 0
    };
    ASSERT(server_dispatch_legacy_deliver_ingot_message(
        w, 0, deliver_msg, sizeof(deliver_msg),
        NULL, NULL, &dispatch));
    ASSERT(!dispatch.rejected_unsigned_action);

    ASSERT_EQ_INT(
        station->manifest.count, station_count_before);
    ASSERT_EQ_INT(
        sp->ship->manifest.count, ship_count_before);
    ASSERT(memcmp(&station->manifest.units[0],
                  &station_unit_before,
                  sizeof(station_unit_before)) == 0);
    ASSERT(memcmp(&sp->ship->manifest.units[0],
                  &ship_unit_before,
                  sizeof(ship_unit_before)) == 0);
    ASSERT_EQ_FLOAT(
        ledger_balance(station, sp->session_token),
        balance_before, 0.001f);
    ASSERT_EQ_INT(
        (int)station->chain_event_count,
        (int)chain_count_before);
    ASSERT(memcmp(station->chain_last_hash,
                  chain_head_before, 32) == 0);
}

TEST(test_signed_action_invalid_signature_rejected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    uint8_t payload[2] = { 0, 0 };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), 1,
                                SIGNED_ACTION_BUY_PRODUCT,
                                payload, sizeof(payload), sk);
    /* Flip one bit in the signature. */
    buf[14 + 4] ^= 0x01;

    signed_action_result_t res =
        signed_action_verify(w, 0, buf, n, NULL, NULL, NULL, NULL);
    ASSERT_EQ_INT(res, SIGNED_ACTION_REJECT_BAD_SIG);
    /* The sim must NOT have updated the nonce on rejection. */
    ASSERT(w->players[0].last_signed_nonce == 0);
}

TEST(test_signed_action_replay_rejected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    uint8_t payload[2] = { 0, 0 };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), /*nonce=*/42,
                                SIGNED_ACTION_BUY_PRODUCT,
                                payload, sizeof(payload), sk);
    /* First send: accepted. Caller commits the nonce. */
    ASSERT_EQ_INT(signed_action_verify(w, 0, buf, n, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_OK);
    w->players[0].last_signed_nonce = 42;
    /* Second send (identical bytes): replayed nonce is rejected. */
    ASSERT_EQ_INT(signed_action_verify(w, 0, buf, n, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_REJECT_REPLAY);
}

TEST(test_signed_action_out_of_order_nonce_rejected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    uint8_t payload[2] = { 0, 0 };
    uint8_t buf_high[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    uint8_t buf_low [SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int nh = build_signed_action(buf_high, sizeof(buf_high), 10,
                                 SIGNED_ACTION_BUY_PRODUCT,
                                 payload, sizeof(payload), sk);
    int nl = build_signed_action(buf_low, sizeof(buf_low), 5,
                                 SIGNED_ACTION_BUY_PRODUCT,
                                 payload, sizeof(payload), sk);
    ASSERT_EQ_INT(signed_action_verify(w, 0, buf_high, nh, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_OK);
    w->players[0].last_signed_nonce = 10;
    ASSERT_EQ_INT(signed_action_verify(w, 0, buf_low, nl, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_REJECT_REPLAY);
}

TEST(test_signed_action_nonce_survives_live_slot_transfer) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);
    server_player_t *old = &w->players[0];
    player_init_ship(old, w);
    old->last_signed_nonce = 777;

    server_player_t *incoming = &w->players[1];
    incoming->connected = true;
    incoming->id = 1;
    fill_token(incoming->session_token, 19);
    incoming->session_ready = true;
    memcpy(incoming->pubkey, pk, sizeof(pk));
    incoming->pubkey_set = true;
    incoming->pubkey_proof_ok = true;
    incoming->pubkey_challenge_consumed = true;
    incoming->last_signed_nonce = 10;

    ASSERT(world_player_transfer_ship_state(w, 1, 0));
    ASSERT(incoming->last_signed_nonce == 777);

    uint8_t payload[2] = {0, 0};
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 +
                SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(
        buf, sizeof(buf), 777, SIGNED_ACTION_BUY_PRODUCT,
        payload, sizeof(payload), sk);
    ASSERT_EQ_INT(
        signed_action_verify(
            w, 1, buf, n, NULL, NULL, NULL, NULL),
        SIGNED_ACTION_REJECT_REPLAY);
}

TEST(test_signed_action_wrong_pubkey_rejected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    /* A second keypair that has NOT been registered against player 0. */
    uint8_t other_pk[32], other_sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(other_pk, other_sk);

    uint8_t payload[2] = { 0, 0 };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), 1,
                                SIGNED_ACTION_BUY_PRODUCT,
                                payload, sizeof(payload), other_sk);
    /* Player 0's pubkey != other_pk → verify fails. */
    ASSERT_EQ_INT(signed_action_verify(w, 0, buf, n, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_REJECT_BAD_SIG);
}

TEST(test_signed_action_no_pubkey_registered_rejected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    /* Stand up a player slot but DON'T register a pubkey. */
    server_player_t *sp = &w->players[2];
    sp->connected = true;
    sp->id = 2;
    fill_token(sp->session_token, 9);
    sp->session_ready = true;
    /* sp->pubkey stays zero, sp->pubkey_set stays false */

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk, sk);
    uint8_t payload[2] = { 0, 0 };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), 1,
                                SIGNED_ACTION_BUY_PRODUCT,
                                payload, sizeof(payload), sk);
    ASSERT_EQ_INT(signed_action_verify(w, 2, buf, n, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_REJECT_NO_PUBKEY);
}

TEST(test_signed_action_save_load_persists_nonce) {
    /* Seventh test in the spec: nonce persistence across save/load.
     *
     * We use the per-player save (player_save / player_load_by_token)
     * because that's where last_signed_nonce lives in the on-disk format
     * (PLY6 tail). After a roundtrip, the same-nonce signed action must
     * be rejected as a replay. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 1, sk, pk);
    server_player_t *sp = &w->players[1];

    /* Simulate accepting a signed action with nonce=N. */
    sp->last_signed_nonce = 777;

    /* Save/load roundtrip via the per-player save. The save dir is just
     * a per-test scratch directory. */
    const char *dir = TMP("signed_action_saves");
    /* Mode bits ignored on Windows; harmless on POSIX. */
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0700);
#endif
    ASSERT(player_save(sp, dir, 1));

    /* Fresh world + slot, replay the registration so verify can find the
     * pubkey, then load the saved nonce off disk. */
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    world_reset(w2);
    server_player_t *sp2 = &w2->players[1];
    sp2->connected = true;
    sp2->id = 1;
    fill_token(sp2->session_token, 2); /* same as setup helper used */
    sp2->session_ready = true;
    memcpy(sp2->pubkey, pk, 32);
    sp2->pubkey_set = true;
    sp2->pubkey_proof_ok = true;
    sp2->pubkey_challenge_consumed = true;
    ASSERT(registry_register_pubkey(w2, pk, sp2->session_token));
    /* Layer A.4 of #479: when a pubkey is registered, the save is keyed
     * by pubkey, not by session_token. */
    ASSERT(player_load_by_pubkey(sp2, w2, dir, pk));
    ASSERT(sp2->last_signed_nonce == 777);

    /* Loading an older snapshot into a live authenticated slot must not
     * roll the replay high-water mark backward. */
    sp2->last_signed_nonce = 900;
    ASSERT(player_load_by_pubkey(sp2, w2, dir, pk));
    ASSERT(sp2->last_signed_nonce == 900);

    /* A signed action with the same nonce we already accepted is a replay. */
    uint8_t payload[2] = { 0, 0 };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), 900,
                                SIGNED_ACTION_BUY_PRODUCT,
                                payload, sizeof(payload), sk);
    ASSERT_EQ_INT(signed_action_verify(w2, 1, buf, n, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_REJECT_REPLAY);
}

TEST(test_signed_action_unknown_action_type_rejected) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    setup_player_with_keypair(w, 0, sk, pk);

    uint8_t payload[2] = { 0, 0 };
    uint8_t buf[SIGNED_ACTION_HEADER_SIZE + 2 + SIGNED_ACTION_SIG_SIZE];
    int n = build_signed_action(buf, sizeof(buf), 1,
                                /*type=*/0xFE,  /* not in signed_action_type_t */
                                payload, sizeof(payload), sk);
    ASSERT_EQ_INT(signed_action_verify(w, 0, buf, n, NULL, NULL, NULL, NULL),
                  SIGNED_ACTION_REJECT_UNKNOWN_TYPE);
}

void register_signed_action_tests(void);
void register_signed_action_tests(void) {
    TEST_SECTION("\nSigned actions (#479 A.3):\n");
    RUN(test_signed_action_happy_path);
    RUN(test_signed_action_dispatch_buy_product_payload);
    RUN(test_signed_action_dispatch_input_action_marks_station_dirty);
    RUN(test_legacy_unsigned_dispatch_rejects_pubkey_player);
    RUN(test_anonymous_legacy_named_cargo_actions_are_inert);
    RUN(test_signed_action_invalid_signature_rejected);
    RUN(test_signed_action_replay_rejected);
    RUN(test_signed_action_out_of_order_nonce_rejected);
    RUN(test_signed_action_nonce_survives_live_slot_transfer);
    RUN(test_signed_action_wrong_pubkey_rejected);
    RUN(test_signed_action_no_pubkey_registered_rejected);
    RUN(test_signed_action_save_load_persists_nonce);
    RUN(test_signed_action_unknown_action_type_rejected);
}
