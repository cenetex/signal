#ifndef PACKNFT_REVEAL_H
#define PACKNFT_REVEAL_H
#include <stdint.h>

#define REVEAL_VERSION "ruby-high-pack-reveal-v1.1"
#define ENTROPY_SOURCE "ruby-high-server-commit-v1"

typedef struct {
    char pack_reveal_version[32];
    char catalog_hash[65];
    char commitment[65];
    char entropy_source[64];
    char reveal_seed[65];
    char reveal_proof[65];
    char pack_asset_address[48];
    uint64_t reveal_slot;
} reveal_provenance_t;

void reveal_pack_commitment(
    const char *catalog_hash,
    const char *asset_address,
    const char *mint_signature,
    const char *owner_address,
    const char *product_id,
    int card_count,
    const char *nonce,
    char commitment_out[65]);

void reveal_pack_seed(
    const char *commitment,
    const char *asset_address,
    const char *transaction_id,
    const char *nonce,
    char seed_out[65]);

void reveal_card_slot(
    const char *commitment,
    const char *reveal_seed,
    const char *asset_address,
    int slot_index,
    char proof_out[65],
    int *card_index_out);

#endif
