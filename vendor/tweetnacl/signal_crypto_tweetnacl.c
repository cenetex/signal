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

void signal_crypto_keypair(uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    /* TweetNaCl's secret key is already (seed||pub). */
    crypto_sign_keypair(pub, secret);
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
