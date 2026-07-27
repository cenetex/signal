/*
 * signal_crypto_tweetnacl.c -- TweetNaCl backend for shared/signal_crypto.h.
 *
 * TweetNaCl exposes the SignerCat-style attached API
 * (crypto_sign / crypto_sign_open) where the signature is prepended to
 * the message. We wrap that into the detached form Signal wants.
 *
 * Public domain. Layer A.1 of #479.
 */
#include "signal_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "tweetnacl.h"

extern int signal_randombytes_checked(uint8_t *buf, unsigned long long n);

#if defined(SIGNAL_CRYPTO_TESTING)
static signal_crypto_test_entropy_provider_fn test_entropy_provider;
static void *test_entropy_provider_user;

void signal_crypto_test_set_entropy_provider(
    signal_crypto_test_entropy_provider_fn provider, void *user) {
    test_entropy_provider = provider;
    test_entropy_provider_user = user;
}

void signal_crypto_test_reset_entropy_provider(void) {
    test_entropy_provider = NULL;
    test_entropy_provider_user = NULL;
}
#endif

static bool signal_crypto_entropy_fill(uint8_t *buf, size_t len) {
    if (!buf) return len == 0;
    if (len == 0) return true;

    bool ok;
#if defined(SIGNAL_CRYPTO_TESTING)
    if (test_entropy_provider) {
        ok = test_entropy_provider(buf, len, test_entropy_provider_user);
    } else
#endif
    {
        ok = signal_randombytes_checked(
                 buf, (unsigned long long)len) != 0;
    }
    if (!ok) memset(buf, 0, len);
    return ok;
}

bool signal_crypto_keypair(uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    if (!pub || !secret) {
        if (pub) memset(pub, 0, SIGNAL_CRYPTO_PUBKEY_BYTES);
        if (secret) memset(secret, 0, SIGNAL_CRYPTO_SECRET_BYTES);
        return false;
    }

    uint8_t seed[SIGNAL_CRYPTO_PUBKEY_BYTES] = {0};
    if (!signal_crypto_entropy_fill(seed, sizeof(seed))) {
        memset(pub, 0, SIGNAL_CRYPTO_PUBKEY_BYTES);
        memset(secret, 0, SIGNAL_CRYPTO_SECRET_BYTES);
        return false;
    }
    crypto_sign_keypair_from_seed(pub, secret, seed);
    memset(seed, 0, sizeof(seed));
    return true;
}

void signal_crypto_keypair_from_seed(const uint8_t seed[SIGNAL_CRYPTO_PUBKEY_BYTES],
                                     uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                                     uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    /* Deterministic — replays the same key derivation as
     * crypto_sign_keypair but skips the random seed step. */
    crypto_sign_keypair_from_seed(pub, secret, seed);
}

bool signal_crypto_random_bytes(uint8_t *buf, size_t len) {
    return signal_crypto_entropy_fill(buf, len);
}

void signal_crypto_sign(uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES],
                        const uint8_t *msg, size_t len,
                        const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    /* crypto_sign writes (sig||msg) into a buffer of size len + 64. */
    unsigned long long smlen = 0;
    /* Stack alloc up to a small bound; otherwise heap. The signing
     * buffer is bounded by message size + 64. */
    uint8_t  stack_buf[1024];
    uint8_t *sm = stack_buf;
    if (len + SIGNAL_CRYPTO_SIG_BYTES > sizeof(stack_buf)) {
        sm = (uint8_t *)malloc(len + SIGNAL_CRYPTO_SIG_BYTES);
        if (!sm) { memset(sig, 0, SIGNAL_CRYPTO_SIG_BYTES); return; }
    }
    crypto_sign(sm, &smlen, msg, (unsigned long long)len, secret);
    memcpy(sig, sm, SIGNAL_CRYPTO_SIG_BYTES);
    if (sm != stack_buf) free(sm);
}

bool signal_crypto_verify(const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES],
                          const uint8_t *msg, size_t len,
                          const uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES]) {
    /* Re-build a (sig || msg) buffer for crypto_sign_open. */
    uint8_t  stack_sm[1024];
    uint8_t  stack_m [1024];
    uint8_t *sm = stack_sm;
    uint8_t *m  = stack_m;
    size_t   smlen_in = len + SIGNAL_CRYPTO_SIG_BYTES;
    if (smlen_in > sizeof(stack_sm)) {
        sm = (uint8_t *)malloc(smlen_in);
        m  = (uint8_t *)malloc(smlen_in);
        if (!sm || !m) { free(sm); free(m); return false; }
    }
    memcpy(sm, sig, SIGNAL_CRYPTO_SIG_BYTES);
    if (len) memcpy(sm + SIGNAL_CRYPTO_SIG_BYTES, msg, len);

    unsigned long long mlen = 0;
    int rc = crypto_sign_open(m, &mlen,
                              sm, (unsigned long long)smlen_in,
                              pub);
    if (sm != stack_sm) { free(sm); free(m); }
    return rc == 0;
}
