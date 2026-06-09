#include "catalog.h"
#include "reveal.h"
#include "metadata.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

#define PACKNFT_MAX_INPUT 65536

static char input_buf[PACKNFT_MAX_INPUT];

static void copy_cstr(char *dst, size_t dst_cap, const char *src) {
    if (dst_cap == 0) return;
    int written = snprintf(dst, dst_cap, "%s", src);
    if (written < 0) dst[0] = 0;
}

/* Minimal JSON string extractor — copies value into caller-provided buffer.
   Returns 0 on success, -1 if key not found or not a string value. */
static int json_get_copy(const char *json, const char *key, char *out, int out_cap) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) { if (out_cap > 0) out[0] = 0; return -1; }
    p += strlen(search);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p != '"') { if (out_cap > 0) out[0] = 0; return -1; }
    p++;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\') p++;
        if (*p) p++;
    }
    int len = (int)(p - start);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, start, len);
    out[len] = 0;
    return len;
}

static int json_get_int(const char *json, const char *key, int def) {
    char buf[32];
    if (json_get_copy(json, key, buf, sizeof(buf)) < 0) return def;
    char *end = NULL;
    errno = 0;
    long value = strtol(buf, &end, 10);
    if (errno || end == buf || *end != '\0' || value < INT_MIN || value > INT_MAX) return def;
    return (int)value;
}

static void write_ok(const char *result_json) {
    printf("{\"ok\":true,%s}\n", result_json);
}

static void write_err(const char *msg) {
    printf("{\"ok\":false,\"error\":\"%s\"}\n", msg);
}

int main(void) {
    /* Read all of stdin */
    size_t len = fread(input_buf, 1, PACKNFT_MAX_INPUT - 1, stdin);
    if (len == 0) { write_err("empty input"); return 1; }
    input_buf[len] = 0;

    char op[64];
    if (json_get_copy(input_buf, "op", op, sizeof(op)) < 0) { write_err("missing op field"); return 1; }

    if (strcmp(op, "catalog-hash") == 0) {
        char hash[65];
        catalog_hash(hash);
        char out[256];
        snprintf(out, sizeof(out), "\"hash\":\"%s\",\"profileCount\":%d,\"liveCount\":%d", hash, FIRST_BELL_PROFILE_COUNT, FIRST_BELL_LIVE_COUNT);
        write_ok(out);

    } else if (strcmp(op, "catalog-entry") == 0) {
        char id[64];
        json_get_copy(input_buf, "profileId", id, sizeof(id));
        const card_profile_t *card = catalog_by_profile_id(id);
        if (!card) { write_err("profile not found"); return 1; }
        char out[1024];
        snprintf(out, sizeof(out),
            "\"profileId\":\"%s\",\"cardName\":\"%s\",\"characterName\":\"%s\",\"role\":\"%s\",\"rarity\":\"%s\",\"setNumber\":%d,\"mintable\":%s",
            card->profile_id, card->card_name, card->character_name,
            card->role, card->rarity, card->set_number,
            card->mintable ? "true" : "false");
        write_ok(out);

    } else if (strcmp(op, "reveal-pack-commitment") == 0) {
        char catalog_hash[65], asset[48], mint_sig[96], owner[48], product[32], nonce[32];
        json_get_copy(input_buf, "catalogHash", catalog_hash, sizeof(catalog_hash));
        json_get_copy(input_buf, "assetAddress", asset, sizeof(asset));
        json_get_copy(input_buf, "mintSignature", mint_sig, sizeof(mint_sig));
        json_get_copy(input_buf, "ownerAddress", owner, sizeof(owner));
        json_get_copy(input_buf, "productId", product, sizeof(product));
        int card_count = json_get_int(input_buf, "cardCount", 5);
        json_get_copy(input_buf, "nonce", nonce, sizeof(nonce));

        if (!catalog_hash[0] || !asset[0] || !mint_sig[0] || !nonce[0]) {
            write_err("missing required fields"); return 1;
        }
        char commitment[65];
        reveal_pack_commitment(catalog_hash, asset, mint_sig, owner[0] ? owner : "", product[0] ? product : "", card_count, nonce, commitment);
        char out[256];
        snprintf(out, sizeof(out), "\"commitment\":\"%s\"", commitment);
        write_ok(out);

    } else if (strcmp(op, "reveal-pack-seed") == 0) {
        char commitment[65], asset[48], txid[96], nonce[32];
        json_get_copy(input_buf, "commitment", commitment, sizeof(commitment));
        json_get_copy(input_buf, "assetAddress", asset, sizeof(asset));
        json_get_copy(input_buf, "transactionId", txid, sizeof(txid));
        json_get_copy(input_buf, "nonce", nonce, sizeof(nonce));

        if (!commitment[0] || !asset[0] || !nonce[0]) {
            write_err("missing required fields"); return 1;
        }
        char seed[65];
        reveal_pack_seed(commitment, asset, txid, nonce, seed);
        char out[256];
        snprintf(out, sizeof(out), "\"revealSeed\":\"%s\"", seed);
        write_ok(out);

    } else if (strcmp(op, "reveal-card-slot") == 0) {
        char commitment[65], reveal_seed[65], asset[48];
        json_get_copy(input_buf, "commitment", commitment, sizeof(commitment));
        json_get_copy(input_buf, "revealSeed", reveal_seed, sizeof(reveal_seed));
        json_get_copy(input_buf, "assetAddress", asset, sizeof(asset));
        int slot = json_get_int(input_buf, "slotIndex", 0);

        if (!commitment[0] || !reveal_seed[0] || !asset[0]) {
            write_err("missing required fields"); return 1;
        }
        char proof[65];
        int card_index;
        reveal_card_slot(commitment, reveal_seed, asset, slot, proof, &card_index);
        const card_profile_t *card = &FIRST_BELL_CATALOG[card_index];
        reveal_provenance_t prov = {0};
        copy_cstr(prov.pack_reveal_version, sizeof(prov.pack_reveal_version), REVEAL_VERSION);
        copy_cstr(prov.commitment, sizeof(prov.commitment), commitment);
        copy_cstr(prov.reveal_proof, sizeof(prov.reveal_proof), proof);
        copy_cstr(prov.pack_asset_address, sizeof(prov.pack_asset_address), asset);
        prov.reveal_slot = (uint64_t)slot;
        char card_json[METADATA_JSON_MAX];
        metadata_card_json(card, "", "", &prov, card_json, sizeof(card_json));
        char out[METADATA_JSON_MAX + 256];
        snprintf(out, sizeof(out),
            "\"revealProof\":\"%s\",\"cardIndex\":%d,\"profileId\":\"%s\",\"cardName\":\"%s\",\"rarity\":\"%s\",\"metadata\":%s",
            proof, card_index, card->profile_id, card->card_name, card->rarity, card_json);
        write_ok(out);

    } else if (strcmp(op, "metadata-pack") == 0) {
        char name[64], symbol[16], image[256], coll[48], prod[32];
        json_get_copy(input_buf, "name", name, sizeof(name));
        json_get_copy(input_buf, "symbol", symbol, sizeof(symbol));
        json_get_copy(input_buf, "imageUri", image, sizeof(image));
        json_get_copy(input_buf, "collectionAddress", coll, sizeof(coll));
        json_get_copy(input_buf, "productId", prod, sizeof(prod));
        int pack_count = json_get_int(input_buf, "packCount", 1);
        int card_count = json_get_int(input_buf, "cardCount", 5);
        int serial     = json_get_int(input_buf, "serial", 1);

        if (!name[0]) copy_cstr(name, sizeof(name), "Ruby High Card Pack");
        if (!symbol[0]) copy_cstr(symbol, sizeof(symbol), "RUBY-PACK");
        char meta[METADATA_JSON_MAX];
        metadata_pack_json(name, symbol, image, coll, prod, pack_count, card_count, serial, NULL, meta, sizeof(meta));
        char out[METADATA_JSON_MAX + 64];
        snprintf(out, sizeof(out), "\"metadata\":%s", meta);
        write_ok(out);

    } else if (strcmp(op, "metadata-card") == 0) {
        char id[64], image[256], coll[48];
        json_get_copy(input_buf, "profileId", id, sizeof(id));
        json_get_copy(input_buf, "imageUri", image, sizeof(image));
        json_get_copy(input_buf, "collectionAddress", coll, sizeof(coll));

        const card_profile_t *card = catalog_by_profile_id(id);
        if (!card) { write_err("profile not found"); return 1; }
        char meta[METADATA_JSON_MAX];
        metadata_card_json(card, image, coll, NULL, meta, sizeof(meta));
        char out[METADATA_JSON_MAX + 64];
        snprintf(out, sizeof(out), "\"metadata\":%s", meta);
        write_ok(out);

    } else if (strcmp(op, "metadata-collection") == 0) {
        char name[64], family[32], symbol[16], image[256], desc[256];
        json_get_copy(input_buf, "name", name, sizeof(name));
        json_get_copy(input_buf, "family", family, sizeof(family));
        json_get_copy(input_buf, "symbol", symbol, sizeof(symbol));
        json_get_copy(input_buf, "imageUri", image, sizeof(image));
        json_get_copy(input_buf, "description", desc, sizeof(desc));

        if (!name[0]) copy_cstr(name, sizeof(name), "Ruby High: First Bell");
        if (!family[0]) copy_cstr(family, sizeof(family), FIRST_BELL_SET_FAMILY);
        if (!symbol[0]) copy_cstr(symbol, sizeof(symbol), "RUBY");
        if (!desc[0]) copy_cstr(desc, sizeof(desc), "The First Bell set from Ruby High.");
        char meta[METADATA_JSON_MAX];
        metadata_collection_json(name, family, symbol, image, desc, meta, sizeof(meta));
        char out[METADATA_JSON_MAX + 64];
        snprintf(out, sizeof(out), "\"metadata\":%s", meta);
        write_ok(out);

    } else {
        write_err("unknown op");
        return 1;
    }
    return 0;
}
