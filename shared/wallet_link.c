#include "wallet_link.h"

#include <string.h>

_Static_assert(sizeof(WALLET_LINK_DOMAIN) - 1 == WALLET_LINK_DOMAIN_LEN,
               "wallet link domain length drifted");
_Static_assert(WALLET_LINK_MESSAGE_SIZE == 93,
               "wallet link message layout drifted");

static bool key_nonzero(const uint8_t key[SIGNAL_CRYPTO_PUBKEY_BYTES]) {
    uint8_t any = 0;
    for (size_t i = 0; i < SIGNAL_CRYPTO_PUBKEY_BYTES; i++) any |= key[i];
    return any != 0;
}

bool wallet_link_message(uint8_t out[WALLET_LINK_MESSAGE_SIZE],
                         const uint8_t signal_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                         const uint8_t solana_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                         uint64_t sequence) {
    if (!out || !signal_pubkey || !solana_pubkey) return false;
    uint8_t *p = out;
    memcpy(p, WALLET_LINK_DOMAIN, WALLET_LINK_DOMAIN_LEN);
    p += WALLET_LINK_DOMAIN_LEN;
    memcpy(p, signal_pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES);
    p += SIGNAL_CRYPTO_PUBKEY_BYTES;
    memcpy(p, solana_pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES);
    p += SIGNAL_CRYPTO_PUBKEY_BYTES;
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(sequence >> (8 * i));
    return true;
}

bool wallet_link_sign(uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
                      const uint8_t signal_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
                      const uint8_t solana_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      uint64_t sequence) {
    if (!out_sig) return false;
    memset(out_sig, 0, SIGNAL_CRYPTO_SIG_BYTES);
    if (!signal_pubkey || !secret || !solana_pubkey) return false;
    /* NaCl secrets end with their public key. */
    if (memcmp(secret + 32, signal_pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES) != 0)
        return false;
    if (!key_nonzero(signal_pubkey) || !key_nonzero(solana_pubkey))
        return false;
    uint8_t msg[WALLET_LINK_MESSAGE_SIZE];
    if (!wallet_link_message(msg, signal_pubkey, solana_pubkey, sequence))
        return false;
    signal_crypto_sign(out_sig, msg, sizeof(msg), secret);
    return true;
}

bool wallet_link_verify(const uint8_t signal_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        const uint8_t solana_pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        uint64_t sequence,
                        const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]) {
    if (!signal_pubkey || !solana_pubkey || !sig) return false;
    if (!key_nonzero(signal_pubkey) || !key_nonzero(solana_pubkey))
        return false;
    uint8_t msg[WALLET_LINK_MESSAGE_SIZE];
    if (!wallet_link_message(msg, signal_pubkey, solana_pubkey, sequence))
        return false;
    return signal_crypto_verify(sig, msg, sizeof(msg), signal_pubkey);
}
