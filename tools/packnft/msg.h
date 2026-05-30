#ifndef PACKNFT_MSG_H
#define PACKNFT_MSG_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../shared/compact.h"

#define SOLANA_PUBKEY_SIZE      32
#define SOLANA_BLOCKHASH_SIZE   32
#define SOLANA_SIGNATURE_SIZE   64
#define SOLANA_MAX_ACCOUNTS     64
#define SOLANA_MAX_IX_DATA      1024
#define SOLANA_MAX_IX_ACCOUNTS  16
#define SOLANA_MAX_IXS          16
#define SOLANA_MAX_MESSAGE_SIZE 2048

typedef struct {
    uint8_t  program_id[SOLANA_PUBKEY_SIZE];
    uint8_t  account_indexes[SOLANA_MAX_IX_ACCOUNTS];
    uint8_t  account_count;
    uint8_t  data[SOLANA_MAX_IX_DATA];
    uint16_t data_len;
} solana_instruction_t;

typedef struct {
    uint8_t  num_required_signatures;
    uint8_t  num_readonly_signed;
    uint8_t  num_readonly_unsigned;
    uint8_t  account_pubkeys[SOLANA_MAX_ACCOUNTS][SOLANA_PUBKEY_SIZE];
    bool     account_is_signer[SOLANA_MAX_ACCOUNTS];
    bool     account_is_writable[SOLANA_MAX_ACCOUNTS];
    uint8_t  account_count;
    uint8_t  recent_blockhash[SOLANA_BLOCKHASH_SIZE];
    solana_instruction_t instructions[SOLANA_MAX_IXS];
    uint8_t  instruction_count;
    bool     built;
} solana_message_t;

void solana_message_init(solana_message_t *msg,
                         const uint8_t blockhash[SOLANA_BLOCKHASH_SIZE]);

int  solana_message_add_account(solana_message_t *msg,
                                const uint8_t pubkey[SOLANA_PUBKEY_SIZE],
                                bool is_signer, bool is_writable);

int  solana_message_add_instruction(solana_message_t *msg,
                                    const uint8_t program_id[SOLANA_PUBKEY_SIZE],
                                    const uint8_t *account_pubkeys[],
                                    const bool     account_is_signer[],
                                    const bool     account_is_writable[],
                                    uint8_t        account_count,
                                    const uint8_t *ix_data,
                                    uint16_t       ix_data_len);

void solana_message_build(solana_message_t *msg);

int  solana_message_serialize(const solana_message_t *msg,
                              uint8_t *out, size_t out_cap);

#endif
