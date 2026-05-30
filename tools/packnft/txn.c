#include "txn.h"
#include "../../shared/signal_crypto.h"
#include "../../shared/compact.h"
#include <string.h>

int solana_transaction_sign(solana_transaction_t *txn,
                            const uint8_t signer_keypairs[][SOLANA_KEYPAIR_SIZE],
                            uint8_t signer_count) {
    if (signer_count > SOLANA_MAX_SIGNERS) return -1;
    if (!txn->message.built) return -1;

    uint8_t msg_bytes[SOLANA_MAX_MESSAGE_SIZE];
    int msg_len = solana_message_serialize(&txn->message, msg_bytes, sizeof(msg_bytes));
    if (msg_len < 0) return -1;

    for (int i = 0; i < signer_count; i++) {
        const uint8_t *seed = signer_keypairs[i];         /* first 32 bytes = Ed25519 seed */
        uint8_t nacl_secret[64];                           /* NaCl expanded secret */
        uint8_t check_pub[32];
        signal_crypto_keypair_from_seed(seed, check_pub, nacl_secret);
        /* The derived pubkey must match the one in the keypair (bytes 32..63) */
        if (memcmp(check_pub, signer_keypairs[i] + 32, 32) != 0) return -1;
        signal_crypto_sign(txn->signatures[i], msg_bytes, msg_len, nacl_secret);
    }
    txn->signer_count = signer_count;
    return 0;
}

int solana_transaction_serialize(const solana_transaction_t *txn,
                                 uint8_t *out, size_t out_cap) {
    uint8_t msg_bytes[SOLANA_MAX_MESSAGE_SIZE];
    int msg_len = solana_message_serialize(&txn->message, msg_bytes, sizeof(msg_bytes));
    if (msg_len < 0) return -1;
    size_t pos = 0;
    uint8_t c16[3]; int cl;
    cl = compact_u16_encode(txn->signer_count, c16);
    if (pos + cl > out_cap) return -1;
    memcpy(out + pos, c16, cl); pos += cl;
    if (pos + txn->signer_count * 64 > out_cap) return -1;
    for (int i = 0; i < txn->signer_count; i++) {
        memcpy(out + pos, txn->signatures[i], 64); pos += 64;
    }
    if (pos + msg_len > out_cap) return -1;
    memcpy(out + pos, msg_bytes, msg_len); pos += msg_len;
    return (int)pos;
}

int solana_transaction_to_base64(const solana_transaction_t *txn,
                                 char *out, size_t out_cap) {
    uint8_t raw[4096];
    int raw_len = solana_transaction_serialize(txn, raw, sizeof(raw));
    if (raw_len < 0) return -1;
    return base64_encode(raw, raw_len, out, out_cap);
}
