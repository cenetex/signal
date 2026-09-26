#include "wallet_link.h"

#include "base58.h"

#include <string.h>

static const char WALLET_LINK_NOTICE[] =
    "This links the identity to the wallet. It does not authorize a transaction.";

static bool key_nonzero(const uint8_t key[SIGNAL_CRYPTO_PUBKEY_BYTES]) {
    uint8_t any = 0;
    for (size_t i = 0; i < SIGNAL_CRYPTO_PUBKEY_BYTES; i++) any |= key[i];
    return any != 0;
}

static size_t append(uint8_t *out, size_t at, const char *text, size_t len) {
    memcpy(out + at, text, len);
    return at + len;
}

size_t wallet_link_message(uint8_t out[WALLET_LINK_MESSAGE_MAX],
                           const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
                           uint64_t sequence) {
    if (!out || !identity || !wallet) return 0;
    char identity_b58[64], wallet_b58[64];
    size_t identity_len = base58_encode(identity, SIGNAL_CRYPTO_PUBKEY_BYTES,
                                        identity_b58, sizeof(identity_b58));
    size_t wallet_len = base58_encode(wallet, SIGNAL_CRYPTO_PUBKEY_BYTES,
                                      wallet_b58, sizeof(wallet_b58));
    if (identity_len == 0 || wallet_len == 0) return 0;
    char digits[20];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + sequence % 10u);
        sequence /= 10u;
    } while (sequence != 0);

    size_t at = 0;
    at = append(out, at, "RATi link v1\nApp: " WALLET_LINK_APP "\nIdentity: ",
                sizeof("RATi link v1\nApp: " WALLET_LINK_APP "\nIdentity: ") - 1);
    at = append(out, at, identity_b58, identity_len);
    at = append(out, at, "\nWallet: ", sizeof("\nWallet: ") - 1);
    at = append(out, at, wallet_b58, wallet_len);
    at = append(out, at, "\nSequence: ", sizeof("\nSequence: ") - 1);
    while (digit_count > 0) out[at++] = (uint8_t)digits[--digit_count];
    at = append(out, at, "\n", 1);
    at = append(out, at, WALLET_LINK_NOTICE, sizeof(WALLET_LINK_NOTICE) - 1);
    return at;
}

bool wallet_link_sign(uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
                      const uint8_t signer[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
                      const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
                      uint64_t sequence) {
    if (!out_sig) return false;
    memset(out_sig, 0, SIGNAL_CRYPTO_SIG_BYTES);
    if (!signer || !secret || !identity || !wallet) return false;
    /* NaCl secrets end with their public key. */
    if (memcmp(secret + 32, signer, SIGNAL_CRYPTO_PUBKEY_BYTES) != 0) return false;
    if (memcmp(signer, identity, SIGNAL_CRYPTO_PUBKEY_BYTES) != 0 &&
        memcmp(signer, wallet, SIGNAL_CRYPTO_PUBKEY_BYTES) != 0) {
        return false;
    }
    if (!key_nonzero(identity) || !key_nonzero(wallet)) return false;
    uint8_t msg[WALLET_LINK_MESSAGE_MAX];
    size_t len = wallet_link_message(msg, identity, wallet, sequence);
    if (len == 0) return false;
    signal_crypto_sign(out_sig, msg, len, secret);
    return true;
}

bool wallet_link_verify_signature(
    const uint8_t signer[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
    uint64_t sequence,
    const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]) {
    if (!signer || !identity || !wallet || !sig) return false;
    if (!key_nonzero(identity) || !key_nonzero(wallet)) return false;
    uint8_t msg[WALLET_LINK_MESSAGE_MAX];
    size_t len = wallet_link_message(msg, identity, wallet, sequence);
    if (len == 0) return false;
    return signal_crypto_verify(sig, msg, len, signer);
}

bool wallet_link_verify(const uint8_t identity[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        const uint8_t wallet[SIGNAL_CRYPTO_PUBKEY_BYTES],
                        uint64_t sequence,
                        const uint8_t identity_sig[SIGNAL_CRYPTO_SIG_BYTES],
                        const uint8_t wallet_sig[SIGNAL_CRYPTO_SIG_BYTES]) {
    return wallet_link_verify_signature(identity, identity, wallet, sequence,
                                        identity_sig) &&
           wallet_link_verify_signature(wallet, identity, wallet, sequence,
                                        wallet_sig);
}
