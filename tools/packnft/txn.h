#ifndef PACKNFT_TXN_H
#define PACKNFT_TXN_H
#include "msg.h"
#include "../../shared/base64.h"

#define SOLANA_SIGNATURE_SIZE 64
#define SOLANA_KEYPAIR_SIZE 64   /* 32 secret + 32 public */
#define SOLANA_MAX_SIGNERS 16

typedef struct {
    uint8_t  signatures[SOLANA_MAX_SIGNERS][SOLANA_SIGNATURE_SIZE];
    uint8_t  signer_count;
    solana_message_t message;
} solana_transaction_t;

/* Each signer_keypair is 64 bytes: [secret(32)][public(32)] */
int solana_transaction_sign(solana_transaction_t *txn,
                            const uint8_t signer_keypairs[][SOLANA_KEYPAIR_SIZE],
                            uint8_t signer_count);

int solana_transaction_serialize(const solana_transaction_t *txn,
                                 uint8_t *out, size_t out_cap);

int solana_transaction_to_base64(const solana_transaction_t *txn,
                                 char *out, size_t out_cap);

#endif
