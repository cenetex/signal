/* Layer A.1 of #479 — verify the pluggable Ed25519 wrapper round-trips
 * and rejects the obvious tamper cases. */
#include "test_harness.h"

#include "base64.h"
#include "pubkey_proof.h"
#include "signal_crypto.h"

typedef struct {
    uint8_t fill;
    size_t bytes_to_write;
} crypto_entropy_fault_t;

static bool crypto_entropy_fail_after_partial_write(
    uint8_t *buf, size_t len, void *user) {
    crypto_entropy_fault_t *fault = (crypto_entropy_fault_t *)user;
    size_t write_len = fault && fault->bytes_to_write < len
        ? fault->bytes_to_write : len;
    if (buf && write_len > 0)
        memset(buf, fault ? fault->fill : 0xA5, write_len);
    return false;
}

TEST(test_crypto_keypair_distinct) {
    uint8_t pub_a[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec_a[SIGNAL_CRYPTO_SECRET_BYTES];
    uint8_t pub_b[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec_b[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pub_a, sec_a));
    ASSERT(signal_crypto_keypair(pub_b, sec_b));
    /* Two fresh keypairs should differ — if randombytes() is broken
     * we'd see identical keys here. */
    ASSERT(memcmp(pub_a, pub_b, SIGNAL_CRYPTO_PUBKEY_BYTES) != 0);
    /* NaCl convention: trailing 32 bytes of secret == pubkey. */
    ASSERT(memcmp(pub_a, sec_a + 32, SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);
    ASSERT(memcmp(pub_b, sec_b + 32, SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);
}

TEST(test_crypto_random_bytes_distinct_and_nonzero) {
    uint8_t a[8] = {0};
    uint8_t b[8] = {0};
    uint8_t zero[8] = {0};

    ASSERT(signal_crypto_random_bytes(a, sizeof(a)));
    ASSERT(signal_crypto_random_bytes(b, sizeof(b)));

    ASSERT(memcmp(a, zero, sizeof(a)) != 0);
    ASSERT(memcmp(b, zero, sizeof(b)) != 0);
    ASSERT(memcmp(a, b, sizeof(a)) != 0);
}

TEST(test_crypto_entropy_failure_clears_random_output) {
    uint8_t out[48];
    uint8_t zero[sizeof(out)];
    memset(out, 0xCC, sizeof(out));
    memset(zero, 0, sizeof(zero));
    crypto_entropy_fault_t fault = {
        .fill = 0x7B,
        .bytes_to_write = 17,
    };

    signal_crypto_test_set_entropy_provider(
        crypto_entropy_fail_after_partial_write, &fault);
    bool ok = signal_crypto_random_bytes(out, sizeof(out));
    signal_crypto_test_reset_entropy_provider();

    ASSERT(!ok);
    ASSERT(memcmp(out, zero, sizeof(out)) == 0);
}

TEST(test_crypto_entropy_failure_clears_keypair_outputs) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    uint8_t zero_pub[sizeof(pub)];
    uint8_t zero_sec[sizeof(sec)];
    memset(pub, 0xCC, sizeof(pub));
    memset(sec, 0xDD, sizeof(sec));
    memset(zero_pub, 0, sizeof(zero_pub));
    memset(zero_sec, 0, sizeof(zero_sec));
    crypto_entropy_fault_t fault = {
        .fill = 0xE1,
        .bytes_to_write = 11,
    };

    signal_crypto_test_set_entropy_provider(
        crypto_entropy_fail_after_partial_write, &fault);
    bool ok = signal_crypto_keypair(pub, sec);
    signal_crypto_test_reset_entropy_provider();

    ASSERT(!ok);
    ASSERT(memcmp(pub, zero_pub, sizeof(pub)) == 0);
    ASSERT(memcmp(sec, zero_sec, sizeof(sec)) == 0);
}

TEST(test_crypto_sign_verify_roundtrip) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pub, sec));

    uint8_t msg[32];
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)(i * 7 + 3);

    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), sec);

    ASSERT(signal_crypto_verify(sig, msg, sizeof(msg), pub));
}

TEST(test_crypto_verify_rejects_msg_tamper) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pub, sec));

    uint8_t msg[32];
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)i;
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), sec);

    msg[5] ^= 0x01; /* flip one bit */
    ASSERT(!signal_crypto_verify(sig, msg, sizeof(msg), pub));
}

TEST(test_crypto_verify_rejects_sig_tamper) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pub, sec));

    uint8_t msg[32];
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)i;
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), sec);

    sig[10] ^= 0x40;
    ASSERT(!signal_crypto_verify(sig, msg, sizeof(msg), pub));
}

TEST(test_crypto_verify_rejects_pub_tamper) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pub, sec));

    uint8_t msg[32];
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)i;
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), sec);

    pub[0] ^= 0x80;
    ASSERT(!signal_crypto_verify(sig, msg, sizeof(msg), pub));
}

TEST(test_pubkey_proof_legacy_v1_is_domain_separated_from_v3) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pub, sec));
    uint8_t token[8];
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    for (size_t i = 0; i < sizeof(token); i++)
        token[i] = (uint8_t)(0x30u + i);
    for (size_t i = 0; i < sizeof(challenge); i++)
        challenge[i] = (uint8_t)(0x80u + i);

    uint8_t legacy_msg[PUBKEY_PROOF_V1_MESSAGE_SIZE];
    ASSERT(pubkey_proof_v1_message(legacy_msg, pub, token));
    ASSERT_EQ_INT(
        memcmp(legacy_msg, "prove-pubkey-v1", PUBKEY_PROOF_V1_DOMAIN_LEN),
        0);
    ASSERT_EQ_INT(
        memcmp(&legacy_msg[PUBKEY_PROOF_V1_DOMAIN_LEN],
               pub, sizeof(pub)),
        0);
    ASSERT_EQ_INT(
        memcmp(&legacy_msg[PUBKEY_PROOF_V1_DOMAIN_LEN + sizeof(pub)],
               token, sizeof(token)),
        0);

    uint8_t legacy_sig[SIGNAL_CRYPTO_SIG_BYTES];
    uint8_t challenge_sig[SIGNAL_CRYPTO_SIG_BYTES];
    ASSERT(pubkey_proof_v1_sign(legacy_sig, pub, sec, token));
    ASSERT(pubkey_proof_sign(
        challenge_sig, pub, sec, token, challenge));
    ASSERT(pubkey_proof_v1_verify(pub, token, legacy_sig));
    ASSERT(!pubkey_proof_verify(
        pub, token, challenge, legacy_sig));
    ASSERT(pubkey_proof_verify(
        pub, token, challenge, challenge_sig));
    ASSERT(!pubkey_proof_v1_verify(pub, token, challenge_sig));
}

TEST(test_pubkey_proof_client_negotiation_and_send_admission) {
    pubkey_proof_client_state_t state;
    pubkey_proof_client_state_reset(&state);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_NONE);

    /* A v3 advertisement cannot trigger an unchallenged proof. */
    pubkey_proof_client_note_protocol(&state, 3);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_NONE);
    pubkey_proof_client_record_send(
        &state, PUBKEY_PROOF_SCHEME_LEGACY_V1, true);
    ASSERT(!state.proof_admitted);
    pubkey_proof_client_note_protocol(&state, 2);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_NONE);

    /* Explicit v2 discovery enables the new-client -> old-server fallback. */
    pubkey_proof_client_state_reset(&state);
    pubkey_proof_client_note_protocol(&state, 0);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_NONE);
    pubkey_proof_client_state_reset(&state);
    pubkey_proof_client_note_protocol(&state, 2);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_LEGACY_V1);

    /* A transport rejection is not proof admission and remains retryable. */
    pubkey_proof_client_record_send(
        &state, PUBKEY_PROOF_SCHEME_LEGACY_V1, false);
    ASSERT(!state.proof_admitted);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_LEGACY_V1);
    pubkey_proof_client_record_send(
        &state, PUBKEY_PROOF_SCHEME_LEGACY_V1, true);
    ASSERT(state.proof_admitted);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_NONE);

    /* Challenge receipt is authoritative and supersedes a legacy latch. */
    pubkey_proof_client_note_challenge(&state);
    ASSERT(!state.proof_admitted);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_CHALLENGE_V2);
    pubkey_proof_client_record_send(
        &state, PUBKEY_PROOF_SCHEME_CHALLENGE_V2, false);
    ASSERT(!state.proof_admitted);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_CHALLENGE_V2);
    pubkey_proof_client_record_send(
        &state, PUBKEY_PROOF_SCHEME_CHALLENGE_V2, true);
    ASSERT(state.proof_admitted);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_NONE);

    /* Local loopback order is challenge first, PROTOCOL_INFO second. */
    pubkey_proof_client_state_reset(&state);
    pubkey_proof_client_note_challenge(&state);
    pubkey_proof_client_note_protocol(&state, 2);
    ASSERT_EQ_INT(
        pubkey_proof_client_next_scheme(&state),
        PUBKEY_PROOF_SCHEME_CHALLENGE_V2);
}

TEST(test_base64_decode_rejects_short_output_buffer) {
    const uint8_t raw[3] = {1, 2, 3};
    char encoded[8];
    uint8_t decoded[3] = {0};
    uint8_t short_decoded[2] = {0};

    ASSERT_EQ_INT(base64_encode(raw, sizeof(raw), encoded, sizeof(encoded)), 4);
    ASSERT(strcmp(encoded, "AQID") == 0);
    ASSERT_EQ_INT(base64_decode(encoded, decoded, sizeof(decoded)), 3);
    ASSERT(memcmp(decoded, raw, sizeof(raw)) == 0);
    ASSERT_EQ_INT(base64_decode(encoded, short_decoded, sizeof(short_decoded)), -1);
}

void register_crypto_tests(void);
void register_crypto_tests(void) {
    TEST_SECTION("\nCrypto (Ed25519) tests:\n");
    RUN(test_crypto_keypair_distinct);
    RUN(test_crypto_random_bytes_distinct_and_nonzero);
    RUN(test_crypto_entropy_failure_clears_random_output);
    RUN(test_crypto_entropy_failure_clears_keypair_outputs);
    RUN(test_crypto_sign_verify_roundtrip);
    RUN(test_crypto_verify_rejects_msg_tamper);
    RUN(test_crypto_verify_rejects_sig_tamper);
    RUN(test_crypto_verify_rejects_pub_tamper);
    RUN(test_pubkey_proof_legacy_v1_is_domain_separated_from_v3);
    RUN(test_pubkey_proof_client_negotiation_and_send_admission);
    RUN(test_base64_decode_rejects_short_output_buffer);
}
