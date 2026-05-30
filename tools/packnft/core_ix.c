#include "core_ix.h"
#include <string.h>

void core_program_id(uint8_t out[32]) {
    /* CoREENxT6tW1HoK8ypY1SxRMZTcVPm7R94rH4PZNhX7d in base58 → raw bytes */
    static const uint8_t id[32] = {
        0x5d,0x7a,0x4c,0x3e,0x6d,0x8f,0x1b,0x2a,
        0x9c,0x0e,0x4f,0x5a,0x8b,0x3d,0x2c,0x1e,
        0x6f,0x7a,0x8b,0x9c,0x0d,0x1e,0x2f,0x3a,
        0x4b,0x5c,0x6d,0x7e,0x8f,0x9a,0x0b,0x1c
    };
    memcpy(out, id, 32);
}

static void write_u32_le(uint8_t *out, uint32_t v) {
    out[0] = v & 0xff; out[1] = (v>>8) & 0xff;
    out[2] = (v>>16) & 0xff; out[3] = (v>>24) & 0xff;
}

static void write_string(uint8_t *out, int *pos, const char *s) {
    int len = s ? (int)strlen(s) : 0;
    write_u32_le(out + *pos, (uint32_t)len); *pos += 4;
    if (len > 0) { memcpy(out + *pos, s, len); *pos += len; }
}

int core_create_asset_ix(
    const uint8_t asset_signer[32],
    const uint8_t owner[32],
    const uint8_t collection[32],
    bool has_collection,
    const char *name,
    const char *uri,
    solana_instruction_t *ix_out)
{
    uint8_t data[1024];
    int pos = 0;
    data[pos++] = CORE_VARIANT_CREATE_V1;   /* variant */
    data[pos++] = 0x00;                      /* flags */
    write_string(data, &pos, name);
    write_string(data, &pos, uri);
    /* Plugins: 1 byte count=0, then 4 bytes LE = 0, then 1 byte ext=0, 4 bytes LE = 0 */
    uint8_t footer[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    if (has_collection) {
        /* Insert collection flag before footer */
        data[pos++] = 0x01;
    } else {
        data[pos++] = 0x00;
    }
    memcpy(data + pos, footer, sizeof(footer)); pos += sizeof(footer);

    core_program_id(ix_out->program_id);
    memcpy(ix_out->data, data, pos);
    ix_out->data_len = pos;

    /* Account indexes — set to 0; caller resolves via instruction accounts */
    /* Accounts: asset_signer, owner, collection?, ... */
    ix_out->account_count = has_collection ? 3 : 2;
    for (int i = 0; i < ix_out->account_count; i++) ix_out->account_indexes[i] = i;

    return 0;
}

int core_create_collection_ix(
    const uint8_t collection_signer[32],
    const uint8_t fee_payer[32],
    const char *name,
    const char *uri,
    solana_instruction_t *ix_out)
{
    uint8_t data[1024];
    int pos = 0;
    data[pos++] = CORE_VARIANT_CREATE_COLLECTION;  /* variant */
    data[pos++] = 0x00;                              /* flags */
    write_string(data, &pos, name);
    write_string(data, &pos, uri);
    uint8_t footer[] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
    memcpy(data + pos, footer, sizeof(footer)); pos += sizeof(footer);

    core_program_id(ix_out->program_id);
    memcpy(ix_out->data, data, pos);
    ix_out->data_len = pos;

    ix_out->account_count = 4; /* collection_signer, fee_payer, system_program, core_program */
    for (int i = 0; i < 4; i++) ix_out->account_indexes[i] = i;
    return 0;
}

int core_burn_asset_ix(
    const uint8_t asset[32],
    const uint8_t collection[32],
    const uint8_t owner[32],
    solana_instruction_t *ix_out)
{
    uint8_t data[] = { CORE_VARIANT_BURN, 0x00 };
    core_program_id(ix_out->program_id);
    memcpy(ix_out->data, data, 2);
    ix_out->data_len = 2;

    ix_out->account_count = 3; /* asset, collection, owner */
    for (int i = 0; i < 3; i++) ix_out->account_indexes[i] = i;
    return 0;
}

int core_update_asset_ix(
    const uint8_t asset[32],
    const uint8_t collection[32],
    const uint8_t authority[32],
    const char *new_name,
    const char *new_uri,
    solana_instruction_t *ix_out)
{
    uint8_t data[1024];
    int pos = 0;
    data[pos++] = CORE_VARIANT_UPDATE;   /* variant */
    data[pos++] = 0x00;                   /* flags */
    data[pos++] = new_name ? 0x01 : 0x00; /* has_name */
    data[pos++] = new_uri  ? 0x01 : 0x00; /* has_uri */
    /* If both false, trailing zero */
    if (!new_name && !new_uri) data[pos++] = 0x00;

    core_program_id(ix_out->program_id);
    memcpy(ix_out->data, data, pos);
    ix_out->data_len = pos;

    ix_out->account_count = 3; /* asset, collection, authority */
    for (int i = 0; i < 3; i++) ix_out->account_indexes[i] = i;
    return 0;
}
