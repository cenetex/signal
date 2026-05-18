/*
 * pubkey_proof.h -- Session-bound Ed25519 proof for pubkey registration.
 *
 * REGISTER_PUBKEY is an identity assertion. PROVE_PUBKEY signs a stable
 * domain plus the asserted pubkey and live session token so the server can
 * safely bind registry/persistence to a private-key holder.
 */
#ifndef SHARED_PUBKEY_PROOF_H
#define SHARED_PUBKEY_PROOF_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "signal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

bool pubkey_proof_message(uint8_t out[PUBKEY_PROOF_MESSAGE_SIZE],
                          const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                          const uint8_t session_token[8]);

bool pubkey_proof_sign(uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
                       const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                       const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
                       const uint8_t session_token[8]);

bool pubkey_proof_verify(const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                         const uint8_t session_token[8],
                         const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_PUBKEY_PROOF_H */
