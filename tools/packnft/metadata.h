#ifndef PACKNFT_METADATA_H
#define PACKNFT_METADATA_H
#include "catalog.h"
#include "reveal.h"

#define METADATA_JSON_MAX 4096

int metadata_pack_json(
    const char *name, const char *symbol,
    const char *image_uri, const char *collection_address,
    const char *product_id, int pack_count, int card_count, int serial,
    const reveal_provenance_t *provenance,
    char *out, int out_cap);

int metadata_card_json(
    const card_profile_t *card,
    const char *image_uri, const char *collection_address,
    const reveal_provenance_t *provenance,
    char *out, int out_cap);

int metadata_collection_json(
    const char *name, const char *family, const char *symbol,
    const char *image_uri, const char *description,
    char *out, int out_cap);

#endif
