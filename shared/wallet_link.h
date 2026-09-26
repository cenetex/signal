/*
 * wallet_link.h -- A player's Signal identity key names a Solana wallet.
 *
 * The identity key signs
 *
 *     "signal-wallet-link-v2" || signal_pubkey || solana_pubkey || sequence
 *
 * with sequence as a little-endian u64. Anyone can check the link with the
 * two public keys alone; no server is involved. Forge verifies it on chain
 * before it mints play supply earned by `signal_pubkey` to `solana_pubkey`.
 * The wallet shows its consent by signing the Forge transaction.
 *
 * A later link with a higher sequence replaces an earlier one, so a player
 * can move to a new wallet. Verifiers that store links must refuse a link
 * whose sequence is not higher than the one they hold.
 *
 * Version 1 of this domain was the server-mediated ceremony in
 * docs/signal-solana-bridge.md. There the identity key signed a server
 * challenge that did not name the wallet, so only the server could vouch
 * for the pairing. It was never built.
 */
#ifndef SHARED_WALLET_LINK_H
#define SHARED_WALLET_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "signal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WALLET_LINK_DOMAIN "signal-wallet-link-v2"
#define WALLET_LINK_DOMAIN_LEN 21
#define WALLET_LINK_MESSAGE_SIZE \
    (WALLET_LINK_DOMAIN_LEN + 2 * SIGNAL_CRYPTO_PUBKEY_BYTES + 8)

bool wallet_link_message(uint8_t out[WALLET_LINK_MESSAGE_SIZE],
                         const uint8_t signal_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                         const uint8_t solana_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                         uint64_t sequence);

/* `secret` is the identity's NaCl secret (seed || pubkey). Fails, clearing
 * out_sig, when the secret's public half is not `signal_pubkey` or the
 * wallet key is all zeros. */
bool wallet_link_sign(uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
                      const uint8_t signal_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
                      const uint8_t solana_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      uint64_t sequence);

bool wallet_link_verify(const uint8_t signal_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        const uint8_t solana_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        uint64_t sequence,
                        const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_WALLET_LINK_H */
