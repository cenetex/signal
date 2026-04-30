/*
 * signal_crypto.h -- Pluggable Ed25519 crypto interface for Signal.
 *
 * The current backend is TweetNaCl (vendor/tweetnacl/). The interface
 * is intentionally minimal: keypair generation, detached sign, detached
 * verify. Ed25519 is a frozen algorithm; this surface should never need
 * to grow. If a future iteration wants libsodium (e.g. for Argon2 or
 * faster batch verify), only signal_crypto_<backend>.c changes.
 *
 * Layer A.1 of the off-chain decentralization roadmap (#479) — see
 * src/identity.[ch] for the on-disk player identity that uses this.
 */
#ifndef SHARED_SIGNAL_CRYPTO_H
#define SHARED_SIGNAL_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SIGNAL_CRYPTO_PUBKEY_BYTES  32
#define SIGNAL_CRYPTO_SECRET_BYTES  64
#define SIGNAL_CRYPTO_SIG_BYTES     64

#ifdef __cplusplus
extern "C" {
#endif

/* Ed25519 keypair generation. secret[64] holds (seed[32] || pub[32])
 * per the standard NaCl convention. pub[32] is the public key. */
void signal_crypto_keypair(uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]);

/* Deterministic Ed25519 keypair derivation from a 32-byte seed.
 *
 * The seed plays the role of the random bytes used by signal_crypto_keypair;
 * the same seed always produces the same (pub, secret) pair. This is the
 * primitive Layer B of #479 uses to derive station identities from the
 * world seed (so every server with the same world seed agrees on which
 * pubkey speaks for "Prospect Refinery") and outpost identities from
 * (founder_pubkey || station_name || planted_tick).
 *
 * secret[64] is laid out as (seed[32] || pub[32]) per the NaCl convention
 * — same shape as signal_crypto_keypair's output. */
void signal_crypto_keypair_from_seed(const uint8_t seed[SIGNAL_CRYPTO_PUBKEY_BYTES],
                                     uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES],
                                     uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]);

/* Detached Ed25519 signature over msg[0..len). sig[64] is the result. */
void signal_crypto_sign(uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES],
                        const uint8_t *msg, size_t len,
                        const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]);

/* Returns true iff sig[64] is a valid Ed25519 signature on msg[0..len)
 * by the holder of pub[32]. */
bool signal_crypto_verify(const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES],
                          const uint8_t *msg, size_t len,
                          const uint8_t pub[SIGNAL_CRYPTO_PUBKEY_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_SIGNAL_CRYPTO_H */
