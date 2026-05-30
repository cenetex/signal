#include "metadata.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int safe_snprintf(char *out, int cap, int *pos, const char *fmt, ...) {
    if (*pos >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *pos, cap - *pos, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    *pos += n;
    if (*pos > cap) *pos = cap;
    return n;
}

static void json_string(char *out, int cap, int *pos, const char *s) {
    safe_snprintf(out, cap, pos, "\"");
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
            case '"':  safe_snprintf(out, cap, pos, "\\\""); break;
            case '\\': safe_snprintf(out, cap, pos, "\\\\"); break;
            case '\n': safe_snprintf(out, cap, pos, "\\n"); break;
            default: {
                if (*pos < cap) out[*pos] = *p;
                (*pos)++;
            }
        }
    }
    safe_snprintf(out, cap, pos, "\"");
}

int metadata_pack_json(
    const char *name, const char *symbol,
    const char *image_uri, const char *collection_address,
    const char *product_id, int pack_count, int card_count, int serial,
    const reveal_provenance_t *prov,
    char *out, int out_cap)
{
    (void)collection_address;
    int p = 0;
    safe_snprintf(out, out_cap, &p, "{");
    safe_snprintf(out, out_cap, &p, "\"name\":"); json_string(out, out_cap, &p, name);
    safe_snprintf(out, out_cap, &p, ",\"symbol\":"); json_string(out, out_cap, &p, symbol);
    safe_snprintf(out, out_cap, &p, ",\"description\":"); json_string(out, out_cap, &p, "A sealed Ruby High card pack. Open to reveal.");
    safe_snprintf(out, out_cap, &p, ",\"image\":"); json_string(out, out_cap, &p, image_uri);
    safe_snprintf(out, out_cap, &p, ",\"seller_fee_basis_points\":0");
    safe_snprintf(out, out_cap, &p, ",\"attributes\":[");
    safe_snprintf(out, out_cap, &p, "{\"trait_type\":\"Product\",\"value\":"); json_string(out, out_cap, &p, product_id); safe_snprintf(out, out_cap, &p, "}");
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Pack Count\",\"value\":%d}", pack_count);
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Cards Per Pack\",\"value\":%d}", card_count);
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Serial\",\"value\":\"%d\"}", serial);
    safe_snprintf(out, out_cap, &p, "]");
    if (prov && prov->commitment[0]) {
        safe_snprintf(out, out_cap, &p, ",\"properties\":{\"provenance\":{");
        safe_snprintf(out, out_cap, &p, "\"packRevealVersion\":"); json_string(out, out_cap, &p, prov->pack_reveal_version);
        safe_snprintf(out, out_cap, &p, ",\"commitment\":"); json_string(out, out_cap, &p, prov->commitment);
        safe_snprintf(out, out_cap, &p, ",\"entropySource\":"); json_string(out, out_cap, &p, prov->entropy_source);
        safe_snprintf(out, out_cap, &p, "}}");
    }
    safe_snprintf(out, out_cap, &p, "}");
    if (p >= out_cap) out[out_cap-1] = 0;
    return p;
}

int metadata_card_json(
    const card_profile_t *card,
    const char *image_uri, const char *collection_address,
    const reveal_provenance_t *prov,
    char *out, int out_cap)
{
    (void)collection_address;
    int p = 0;
    safe_snprintf(out, out_cap, &p, "{");
    safe_snprintf(out, out_cap, &p, "\"name\":"); json_string(out, out_cap, &p, card->card_name);
    safe_snprintf(out, out_cap, &p, ",\"symbol\":"); json_string(out, out_cap, &p, "RUBY");
    safe_snprintf(out, out_cap, &p, ",\"description\":"); json_string(out, out_cap, &p, 
        card->nft_description ? card->nft_description : card->blurb);
    safe_snprintf(out, out_cap, &p, ",\"image\":"); json_string(out, out_cap, &p, image_uri);
    safe_snprintf(out, out_cap, &p, ",\"seller_fee_basis_points\":0");
    safe_snprintf(out, out_cap, &p, ",\"attributes\":[");
    safe_snprintf(out, out_cap, &p, "{\"trait_type\":\"Character\",\"value\":"); json_string(out, out_cap, &p, card->character_name); safe_snprintf(out, out_cap, &p, "}");
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Role\",\"value\":"); json_string(out, out_cap, &p, card->role); safe_snprintf(out, out_cap, &p, "}");
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Rarity\",\"value\":"); json_string(out, out_cap, &p, card->rarity); safe_snprintf(out, out_cap, &p, "}");
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Subject\",\"value\":"); json_string(out, out_cap, &p, card->subject); safe_snprintf(out, out_cap, &p, "}");
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Set\",\"value\":"); json_string(out, out_cap, &p, FIRST_BELL_SET_NAME); safe_snprintf(out, out_cap, &p, "}");
    safe_snprintf(out, out_cap, &p, ",{\"trait_type\":\"Set Number\",\"value\":\"%d\"}", card->set_number);
    safe_snprintf(out, out_cap, &p, "]");
    if (prov && prov->reveal_proof[0]) {
        safe_snprintf(out, out_cap, &p, ",\"properties\":{\"provenance\":{");
        safe_snprintf(out, out_cap, &p, "\"packRevealVersion\":"); json_string(out, out_cap, &p, prov->pack_reveal_version);
        safe_snprintf(out, out_cap, &p, ",\"commitment\":"); json_string(out, out_cap, &p, prov->commitment);
        safe_snprintf(out, out_cap, &p, ",\"revealProof\":"); json_string(out, out_cap, &p, prov->reveal_proof);
        safe_snprintf(out, out_cap, &p, ",\"packAssetAddress\":"); json_string(out, out_cap, &p, prov->pack_asset_address);
        if (prov->reveal_slot) safe_snprintf(out, out_cap, &p, ",\"revealSlot\":%llu", (unsigned long long)prov->reveal_slot);
        safe_snprintf(out, out_cap, &p, "}}");
    }
    safe_snprintf(out, out_cap, &p, "}");
    if (p >= out_cap) out[out_cap-1] = 0;
    return p;
}

int metadata_collection_json(
    const char *name, const char *family, const char *symbol,
    const char *image_uri, const char *description,
    char *out, int out_cap)
{
    int p = 0;
    safe_snprintf(out, out_cap, &p, "{");
    safe_snprintf(out, out_cap, &p, "\"name\":"); json_string(out, out_cap, &p, name);
    safe_snprintf(out, out_cap, &p, ",\"symbol\":"); json_string(out, out_cap, &p, symbol);
    safe_snprintf(out, out_cap, &p, ",\"description\":"); json_string(out, out_cap, &p, description);
    safe_snprintf(out, out_cap, &p, ",\"image\":"); json_string(out, out_cap, &p, image_uri);
    safe_snprintf(out, out_cap, &p, ",\"seller_fee_basis_points\":0");
    safe_snprintf(out, out_cap, &p, ",\"attributes\":[");
    safe_snprintf(out, out_cap, &p, "{\"trait_type\":\"Family\",\"value\":"); json_string(out, out_cap, &p, family);
    safe_snprintf(out, out_cap, &p, "}]}");
    if (p >= out_cap) out[out_cap-1] = 0;
    return p;
}
