#ifndef PACKNFT_CORE_IX_H
#define PACKNFT_CORE_IX_H
#include "msg.h"

#define CORE_VARIANT_CREATE_V1          0x14
#define CORE_VARIANT_CREATE_COLLECTION  0x15
#define CORE_VARIANT_BURN               0x0c
#define CORE_VARIANT_UPDATE             0x1e

#define CORE_PROGRAM_ID_STR "CoREENxT6tW1HoK8ypY1SxRMZTcVPm7R94rH4PZNhX7d"

void core_program_id(uint8_t out[32]);

int core_create_asset_ix(
    const uint8_t asset_signer[32],
    const uint8_t owner[32],
    const uint8_t collection[32],
    bool has_collection,
    const char *name,
    const char *uri,
    solana_instruction_t *ix_out);

int core_create_collection_ix(
    const uint8_t collection_signer[32],
    const uint8_t fee_payer[32],
    const char *name,
    const char *uri,
    solana_instruction_t *ix_out);

int core_burn_asset_ix(
    const uint8_t asset[32],
    const uint8_t collection[32],
    const uint8_t owner[32],
    solana_instruction_t *ix_out);

int core_update_asset_ix(
    const uint8_t asset[32],
    const uint8_t collection[32],
    const uint8_t authority[32],
    const char *new_name,
    const char *new_uri,
    solana_instruction_t *ix_out);

#endif
