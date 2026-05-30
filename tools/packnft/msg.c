#include "msg.h"
#include <string.h>

void solana_message_init(solana_message_t *msg,
                         const uint8_t blockhash[SOLANA_BLOCKHASH_SIZE]) {
    memset(msg, 0, sizeof(*msg));
    memcpy(msg->recent_blockhash, blockhash, SOLANA_BLOCKHASH_SIZE);
}

static int find_account_index(const solana_message_t *msg,
                              const uint8_t pubkey[SOLANA_PUBKEY_SIZE]) {
    for (int i = 0; i < msg->account_count; i++) {
        if (memcmp(msg->account_pubkeys[i], pubkey, SOLANA_PUBKEY_SIZE) == 0)
            return i;
    }
    return -1;
}

int solana_message_add_account(solana_message_t *msg,
                               const uint8_t pubkey[SOLANA_PUBKEY_SIZE],
                               bool is_signer, bool is_writable) {
    if (msg->built) return -1;
    int idx = find_account_index(msg, pubkey);
    if (idx >= 0) {
        if (is_signer) msg->account_is_signer[idx] = true;
        if (is_writable) msg->account_is_writable[idx] = true;
        return idx;
    }
    if (msg->account_count >= SOLANA_MAX_ACCOUNTS) return -1;
    idx = msg->account_count;
    memcpy(msg->account_pubkeys[idx], pubkey, SOLANA_PUBKEY_SIZE);
    msg->account_is_signer[idx] = is_signer;
    msg->account_is_writable[idx] = is_writable;
    msg->account_count++;
    return idx;
}

int solana_message_add_instruction(solana_message_t *msg,
                                   const uint8_t program_id[SOLANA_PUBKEY_SIZE],
                                   const uint8_t *account_pubkeys[],
                                   const bool     account_is_signer[],
                                   const bool     account_is_writable[],
                                   uint8_t        account_count,
                                   const uint8_t *ix_data,
                                   uint16_t       ix_data_len) {
    if (msg->built) return -1;
    if (msg->instruction_count >= SOLANA_MAX_IXS) return -1;
    solana_message_add_account(msg, program_id, false, false);
    for (int i = 0; i < account_count; i++) {
        solana_message_add_account(msg, account_pubkeys[i],
                                   account_is_signer[i],
                                   account_is_writable[i]);
    }
    solana_instruction_t *ix = &msg->instructions[msg->instruction_count];
    memcpy(ix->program_id, program_id, SOLANA_PUBKEY_SIZE);
    ix->account_count = account_count;
    for (int i = 0; i < account_count; i++) {
        ix->account_indexes[i] = (uint8_t)find_account_index(msg, account_pubkeys[i]);
    }
    memcpy(ix->data, ix_data, ix_data_len);
    ix->data_len = ix_data_len;
    msg->instruction_count++;
    return msg->instruction_count - 1;
}

/* Account ordering: signers first, then writable non-signers,
   then readonly non-signers. Within each group, insertion order. */
static int account_order_key(const solana_message_t *msg, int idx) {
    if (msg->account_is_signer[idx])
        return msg->account_is_writable[idx] ? 0 : 1;
    return msg->account_is_writable[idx] ? 2 : 3;
}

/* Simple insertion sort — account_count is always small */
static void sort_accounts(solana_message_t *msg, int *indices, int count) {
    for (int i = 1; i < count; i++) {
        int key = indices[i];
        int key_order = account_order_key(msg, key);
        int j = i - 1;
        while (j >= 0) {
            int cur = indices[j];
            int cur_order = account_order_key(msg, cur);
            if (cur_order < key_order) break;
            if (cur_order == key_order && cur < key) break;
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }
}

void solana_message_build(solana_message_t *msg) {
    if (msg->built) return;

    int indices[SOLANA_MAX_ACCOUNTS];
    for (int i = 0; i < msg->account_count; i++) indices[i] = i;
    sort_accounts(msg, indices, msg->account_count);

    /* Reorder accounts and build old→new mapping */
    uint8_t new_keys[SOLANA_MAX_ACCOUNTS][SOLANA_PUBKEY_SIZE];
    bool    new_sig[SOLANA_MAX_ACCOUNTS];
    bool    new_wr[SOLANA_MAX_ACCOUNTS];
    int     old_to_new[SOLANA_MAX_ACCOUNTS];

    for (int i = 0; i < msg->account_count; i++) {
        int old = indices[i];
        memcpy(new_keys[i], msg->account_pubkeys[old], SOLANA_PUBKEY_SIZE);
        new_sig[i] = msg->account_is_signer[old];
        new_wr[i] = msg->account_is_writable[old];
        old_to_new[old] = i;
    }
    memcpy(msg->account_pubkeys, new_keys, sizeof(new_keys));
    memcpy(msg->account_is_signer, new_sig, sizeof(new_sig));
    memcpy(msg->account_is_writable, new_wr, sizeof(new_wr));

    /* Update instruction account indexes */
    for (int i = 0; i < msg->instruction_count; i++) {
        solana_instruction_t *ix = &msg->instructions[i];
        for (int j = 0; j < ix->account_count; j++) {
            ix->account_indexes[j] = (uint8_t)old_to_new[ix->account_indexes[j]];
        }
    }

    /* Compute header counts */
    msg->num_required_signatures = 0;
    msg->num_readonly_signed = 0;
    msg->num_readonly_unsigned = 0;
    for (int i = 0; i < msg->account_count; i++) {
        if (msg->account_is_signer[i]) {
            msg->num_required_signatures++;
            if (!msg->account_is_writable[i]) msg->num_readonly_signed++;
        } else {
            if (!msg->account_is_writable[i]) msg->num_readonly_unsigned++;
        }
    }
    msg->built = true;
}

int solana_message_serialize(const solana_message_t *msg,
                             uint8_t *out, size_t out_cap) {
    size_t pos = 0;
    uint8_t c16[3]; int cl;

    /* Header */
    if (pos + 3 > out_cap) return -1;
    out[pos++] = msg->num_required_signatures;
    out[pos++] = msg->num_readonly_signed;
    out[pos++] = msg->num_readonly_unsigned;

    /* Account count */
    cl = compact_u16_encode(msg->account_count, c16);
    if (pos + cl > out_cap) return -1;
    memcpy(out + pos, c16, cl); pos += cl;

    /* Account pubkeys */
    if (pos + msg->account_count * 32 > out_cap) return -1;
    for (int i = 0; i < msg->account_count; i++) {
        memcpy(out + pos, msg->account_pubkeys[i], 32);
        pos += 32;
    }

    /* Blockhash */
    if (pos + 32 > out_cap) return -1;
    memcpy(out + pos, msg->recent_blockhash, 32);
    pos += 32;

    /* Instruction count */
    cl = compact_u16_encode(msg->instruction_count, c16);
    if (pos + cl > out_cap) return -1;
    memcpy(out + pos, c16, cl); pos += cl;

    /* Instructions */
    for (int i = 0; i < msg->instruction_count; i++) {
        const solana_instruction_t *ix = &msg->instructions[i];

        /* Program ID index — find in sorted account list */
        int pi = -1;
        for (int k = 0; k < msg->account_count; k++) {
            if (memcmp(msg->account_pubkeys[k], ix->program_id, 32) == 0) {
                pi = k; break;
            }
        }
        if (pi < 0) return -1;
        if (pos + 1 > out_cap) return -1;
        out[pos++] = (uint8_t)pi;

        /* Account indexes */
        cl = compact_u16_encode(ix->account_count, c16);
        if (pos + cl + ix->account_count > out_cap) return -1;
        memcpy(out + pos, c16, cl); pos += cl;
        memcpy(out + pos, ix->account_indexes, ix->account_count);
        pos += ix->account_count;

        /* Data */
        cl = compact_u16_encode(ix->data_len, c16);
        if (pos + cl + ix->data_len > out_cap) return -1;
        memcpy(out + pos, c16, cl); pos += cl;
        memcpy(out + pos, ix->data, ix->data_len);
        pos += ix->data_len;
    }
    return (int)pos;
}
