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
#include "signal_memzero.h"

#include <limits.h>
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

#if SIZE_MAX > ULLONG_MAX
    if (len > ULLONG_MAX) {
        signal_memzero_explicit(buf, len);
        return false;
    }
#endif

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
    if (!ok) signal_memzero_explicit(buf, len);
    return ok;
}

static bool signal_crypto_attached_lengths(
    size_t message_len,
    size_t *attached_size,
    unsigned long long *message_len_ull,
    unsigned long long *attached_len_ull) {
    if (!attached_size || !message_len_ull || !attached_len_ull ||
        message_len > SIZE_MAX - SIGNAL_CRYPTO_SIG_BYTES) {
        return false;
    }

    size_t total = message_len + SIGNAL_CRYPTO_SIG_BYTES;
#if SIZE_MAX > ULLONG_MAX
    if (message_len > ULLONG_MAX || total > ULLONG_MAX) return false;
#endif
    *attached_size = total;
    *message_len_ull = (unsigned long long)message_len;
    *attached_len_ull = (unsigned long long)total;
    return true;
}

bool signal_crypto_keypair(uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    if (!pub || !secret) {
        if (pub)
            signal_memzero_explicit(pub, SIGNAL_CRYPTO_PUBKEY_BYTES);
        if (secret)
            signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
        return false;
    }

    uint8_t seed[SIGNAL_CRYPTO_PUBKEY_BYTES] = {0};
    if (!signal_crypto_entropy_fill(seed, sizeof(seed))) {
        signal_memzero_explicit(seed, sizeof(seed));
        signal_memzero_explicit(pub, SIGNAL_CRYPTO_PUBKEY_BYTES);
        signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
        return false;
    }
    int rc = crypto_sign_keypair_from_seed(pub, secret, seed);
    signal_memzero_explicit(seed, sizeof(seed));
    if (rc != 0) {
        signal_memzero_explicit(pub, SIGNAL_CRYPTO_PUBKEY_BYTES);
        signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
        return false;
    }
    return true;
}

void signal_crypto_keypair_from_seed(const uint8_t seed[SIGNAL_CRYPTO_PUBKEY_BYTES],
                                     uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                                     uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    if (!seed || !pub || !secret) {
        if (pub)
            signal_memzero_explicit(pub, SIGNAL_CRYPTO_PUBKEY_BYTES);
        if (secret)
            signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
        return;
    }
    /* Deterministic — replays the same key derivation as
     * crypto_sign_keypair but skips the random seed step. */
    if (crypto_sign_keypair_from_seed(pub, secret, seed) != 0) {
        signal_memzero_explicit(pub, SIGNAL_CRYPTO_PUBKEY_BYTES);
        signal_memzero_explicit(secret, SIGNAL_CRYPTO_SECRET_BYTES);
    }
}

bool signal_crypto_random_bytes(uint8_t *buf, size_t len) {
    return signal_crypto_entropy_fill(buf, len);
}

void signal_crypto_sign(uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES],
                        const uint8_t *msg, size_t len,
                        const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    /* crypto_sign writes (sig||msg) into a buffer of size len + 64. */
    if (!sig) return;

    size_t attached_size = 0;
    unsigned long long message_len_ull = 0;
    unsigned long long attached_len_ull = 0;
    if (!secret || (!msg && len > 0) ||
        !signal_crypto_attached_lengths(
            len, &attached_size, &message_len_ull, &attached_len_ull)) {
        signal_memzero_explicit(sig, SIGNAL_CRYPTO_SIG_BYTES);
        return;
    }

    unsigned long long smlen = 0;
    /* Stack alloc up to a small bound; otherwise heap. The signing
     * buffer is bounded by message size + 64. */
    uint8_t  stack_buf[1024];
    uint8_t *sm = stack_buf;
    if (attached_size > sizeof(stack_buf)) {
        sm = (uint8_t *)malloc(attached_size);
        if (!sm) {
            signal_memzero_explicit(sig, SIGNAL_CRYPTO_SIG_BYTES);
            return;
        }
    }
    int rc = crypto_sign(sm, &smlen, msg, message_len_ull, secret);
    if (rc == 0 && smlen == attached_len_ull) {
        memcpy(sig, sm, SIGNAL_CRYPTO_SIG_BYTES);
    } else {
        signal_memzero_explicit(sig, SIGNAL_CRYPTO_SIG_BYTES);
    }
    signal_memzero_explicit(sm, attached_size);
    if (sm != stack_buf) free(sm);
}

bool signal_crypto_verify(const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES],
                          const uint8_t *msg, size_t len,
                          const uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES]) {
    /* Re-build a (sig || msg) buffer for crypto_sign_open. */
    if (!sig || !pub || (!msg && len > 0)) return false;

    size_t attached_size = 0;
    unsigned long long message_len_ull = 0;
    unsigned long long attached_len_ull = 0;
    if (!signal_crypto_attached_lengths(
            len, &attached_size, &message_len_ull, &attached_len_ull)) {
        return false;
    }

    uint8_t  stack_sm[1024];
    uint8_t  stack_m [1024];
    uint8_t *sm = stack_sm;
    uint8_t *m  = stack_m;
    if (attached_size > sizeof(stack_sm)) {
        sm = (uint8_t *)malloc(attached_size);
        m  = (uint8_t *)malloc(attached_size);
        if (!sm || !m) {
            free(sm);
            free(m);
            return false;
        }
    }
    memcpy(sm, sig, SIGNAL_CRYPTO_SIG_BYTES);
    if (len) memcpy(sm + SIGNAL_CRYPTO_SIG_BYTES, msg, len);

    unsigned long long mlen = 0;
    int rc = crypto_sign_open(m, &mlen,
                              sm, attached_len_ull,
                              pub);
    bool valid = rc == 0 && mlen == message_len_ull;
    signal_memzero_explicit(sm, attached_size);
    signal_memzero_explicit(m, attached_size);
    if (sm != stack_sm) { free(sm); free(m); }
    return valid;
}
