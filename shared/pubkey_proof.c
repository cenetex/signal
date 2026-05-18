#include "pubkey_proof.h"

#include <string.h>

bool pubkey_proof_message(uint8_t out[PUBKEY_PROOF_MESSAGE_SIZE],
                          const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                          const uint8_t session_token[8]) {
    if (!out || !pubkey || !session_token) return false;
    memcpy(out, PUBKEY_PROOF_DOMAIN, PUBKEY_PROOF_DOMAIN_LEN);
    memcpy(out + PUBKEY_PROOF_DOMAIN_LEN, pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES);
    memcpy(out + PUBKEY_PROOF_DOMAIN_LEN + SIGNAL_CRYPTO_PUBKEY_BYTES,
           session_token, 8);
    return true;
}

bool pubkey_proof_sign(uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
                       const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                       const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
                       const uint8_t session_token[8]) {
    if (!out_sig || !pubkey || !secret || !session_token) return false;
    uint8_t msg[PUBKEY_PROOF_MESSAGE_SIZE];
    if (!pubkey_proof_message(msg, pubkey, session_token)) return false;
    signal_crypto_sign(out_sig, msg, sizeof(msg), secret);
    return true;
}

bool pubkey_proof_verify(const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                         const uint8_t session_token[8],
                         const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]) {
    if (!pubkey || !session_token || !sig) return false;
    uint8_t msg[PUBKEY_PROOF_MESSAGE_SIZE];
    if (!pubkey_proof_message(msg, pubkey, session_token)) return false;
    return signal_crypto_verify(sig, msg, sizeof(msg), pubkey);
}
