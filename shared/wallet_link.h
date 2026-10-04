/*
 * wallet_link.h -- A RATi link from a Signal identity to a Solana wallet.
 *
 * The RATi link (Forge docs/rati-link.md) is one readable message that every
 * RATi app uses to link an identity to a wallet:
 *
 *     RATi link v1
 *     App: signal
 *     Identity: <base58 identity key>
 *     Wallet: <base58 wallet>
 *     Sequence: <decimal u64>
 *     This links the identity to the wallet. It does not authorize a transaction.
 *
 * Lines end with "\n"; the last has none. Both the identity key and the
 * wallet sign these exact bytes with plain Ed25519. The identity's signature
 * shows it chose the wallet; the wallet's shows the wallet agreed. Forge's
 * LinkWallet checks both on chain before play supply earned by the identity
 * goes to the wallet.
 *
 * The sequence is normally the Unix time in seconds when the link is made.
 * A link with a higher sequence replaces an older one, so a player can move
 * to a new wallet.
 */
#ifndef SHARED_WALLET_LINK_H
#define SHARED_WALLET_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "signal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WALLET_LINK_APP "signal"
/* The Signal message is at most 250 bytes: 44-character keys and a 20-digit
 * sequence. The buffer matches Forge's LINK_MESSAGE_MAX for any app. */
#define WALLET_LINK_MESSAGE_MAX 352

/* Write the message for `identity` and `wallet` into `out` and return its
 * length, or 0 on a NULL argument. The message is not NUL-terminated. */
size_t wallet_link_message(uint8_t out[WALLET_LINK_MESSAGE_MAX],
                           const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           uint64_t sequence);

/* Sign the message with `secret` (NaCl seed || pubkey). `signer` must be the
 * secret's public half and either the identity or the wallet. Fails,
 * clearing out_sig, on a mismatched secret or an all-zero key. */
bool wallet_link_sign(uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
                      const uint8_t signer[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
                      const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      uint64_t sequence);

/* True when `sig` is `signer`'s signature over the message. */
bool wallet_link_verify_signature(
    const uint8_t signer[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
    uint64_t sequence,
    const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]);

/* True when both the identity and the wallet signed the message: a complete
 * link. */
bool wallet_link_verify(const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        uint64_t sequence,
                        const uint8_t identity_sig[SIGNAL_CRYPTO_SIG_BYTES],
                        const uint8_t wallet_sig[SIGNAL_CRYPTO_SIG_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_WALLET_LINK_H */
