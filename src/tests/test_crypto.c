/* Layer A.1 of #479 — verify the pluggable Ed25519 wrapper round-trips
 * and rejects the obvious tamper cases. */
#include "test_harness.h"

#include "signal_crypto.h"

TEST(test_crypto_keypair_distinct) {
    uint8_t pub_a[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec_a[SIGNAL_CRYPTO_SECRET_BYTES];
    uint8_t pub_b[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec_b[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pub_a, sec_a);
    signal_crypto_keypair(pub_b, sec_b);
    /* Two fresh keypairs should differ — if randombytes() is broken
     * we'd see identical keys here. */
    ASSERT(memcmp(pub_a, pub_b, SIGNAL_CRYPTO_PUBKEY_BYTES) != 0);
    /* NaCl convention: trailing 32 bytes of secret == pubkey. */
    ASSERT(memcmp(pub_a, sec_a + 32, SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);
    ASSERT(memcmp(pub_b, sec_b + 32, SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);
}

TEST(test_crypto_sign_verify_roundtrip) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pub, sec);

    uint8_t msg[32];
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)(i * 7 + 3);

    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), sec);

    ASSERT(signal_crypto_verify(sig, msg, sizeof(msg), pub));
}

TEST(test_crypto_verify_rejects_msg_tamper) {
    uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES];
    uint8_t sec[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pub, sec);

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
    signal_crypto_keypair(pub, sec);

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
    signal_crypto_keypair(pub, sec);

    uint8_t msg[32];
    for (int i = 0; i < 32; i++) msg[i] = (uint8_t)i;
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), sec);

    pub[0] ^= 0x80;
    ASSERT(!signal_crypto_verify(sig, msg, sizeof(msg), pub));
}

void register_crypto_tests(void);
void register_crypto_tests(void) {
    TEST_SECTION("\nCrypto (Ed25519) tests:\n");
    RUN(test_crypto_keypair_distinct);
    RUN(test_crypto_sign_verify_roundtrip);
    RUN(test_crypto_verify_rejects_msg_tamper);
    RUN(test_crypto_verify_rejects_sig_tamper);
    RUN(test_crypto_verify_rejects_pub_tamper);
}
