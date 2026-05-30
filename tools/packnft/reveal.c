#include "reveal.h"
#include "catalog.h"
#include "../../shared/sha256.h"
#include <string.h>
#include <stdio.h>

static void sha256_hex(const uint8_t hash[32], char out[65]) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2] = hx[hash[i]>>4]; out[i*2+1] = hx[hash[i]&15]; }
    out[64] = 0;
}

static void update_str(sha256_ctx_t *ctx, const char *s) {
    sha256_update(ctx, s, strlen(s));
}

static void update_sep(sha256_ctx_t *ctx) {
    sha256_update(ctx, "|", 1);
}

void reveal_pack_commitment(
    const char *catalog_hash,
    const char *asset_address,
    const char *mint_signature,
    const char *owner_address,
    const char *product_id,
    int card_count,
    const char *nonce,
    char commitment_out[65])
{
    sha256_ctx_t ctx;
    uint8_t hash[32];
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", card_count);

    sha256_init(&ctx);
    update_str(&ctx, REVEAL_VERSION); update_sep(&ctx);
    sha256_update(&ctx, "commit", 6); update_sep(&ctx);
    update_str(&ctx, catalog_hash);   update_sep(&ctx);
    update_str(&ctx, asset_address);  update_sep(&ctx);
    update_str(&ctx, mint_signature); update_sep(&ctx);
    update_str(&ctx, owner_address);  update_sep(&ctx);
    update_str(&ctx, product_id);     update_sep(&ctx);
    sha256_update(&ctx, count_str, strlen(count_str)); update_sep(&ctx);
    update_str(&ctx, nonce);
    sha256_final(&ctx, hash);
    sha256_hex(hash, commitment_out);
}

void reveal_pack_seed(
    const char *commitment,
    const char *asset_address,
    const char *transaction_id,
    const char *nonce,
    char seed_out[65])
{
    sha256_ctx_t ctx;
    uint8_t hash[32];

    sha256_init(&ctx);
    update_str(&ctx, REVEAL_VERSION); update_sep(&ctx);
    sha256_update(&ctx, "seed", 4);   update_sep(&ctx);
    update_str(&ctx, commitment);     update_sep(&ctx);
    update_str(&ctx, asset_address);  update_sep(&ctx);
    update_str(&ctx, transaction_id); update_sep(&ctx);
    sha256_update(&ctx, "", 0);       update_sep(&ctx);
    update_str(&ctx, nonce);
    sha256_final(&ctx, hash);
    sha256_hex(hash, seed_out);
}

void reveal_card_slot(
    const char *commitment,
    const char *reveal_seed,
    const char *asset_address,
    int slot_index,
    char proof_out[65],
    int *card_index_out)
{
    sha256_ctx_t ctx;
    uint8_t hash[32];
    char slot_str[16];
    snprintf(slot_str, sizeof(slot_str), "%d", slot_index < 0 ? 0 : slot_index);

    sha256_init(&ctx);
    update_str(&ctx, REVEAL_VERSION); update_sep(&ctx);
    update_str(&ctx, commitment);     update_sep(&ctx);
    update_str(&ctx, reveal_seed);    update_sep(&ctx);
    update_str(&ctx, asset_address);  update_sep(&ctx);
    sha256_update(&ctx, slot_str, strlen(slot_str));
    sha256_final(&ctx, hash);
    sha256_hex(hash, proof_out);

    /* Use the proof hex as entropy to pick a card. */
    unsigned int seed_val = 0;
    for (int i = 0; i < 8 && proof_out[i]; i++) {
        char c = proof_out[i];
        seed_val = (seed_val << 4) | (unsigned int)((c >= 'a') ? (c - 'a' + 10) : (c - '0'));
    }
    *card_index_out = (int)(seed_val % FIRST_BELL_LIVE_COUNT);
}
