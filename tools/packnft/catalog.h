#ifndef PACKNFT_CATALOG_H
#define PACKNFT_CATALOG_H
#include <stdint.h>

#define FIRST_BELL_PROFILE_COUNT 36
#define FIRST_BELL_LIVE_COUNT 24
#define FIRST_BELL_EXPANSION_COUNT 12
#define CARDS_PER_PACK 5

typedef struct {
    const char *character_id;
    const char *character_name;
    const char *role;
    const char *rarity;
    const char *title;
    const char *blurb;
    const char *color;
    const char *art_sheet;
    const char *art_position;
    const char *nft_description;
    int         set_number;
    const char *profile_id;
    const char *card_name;
    const char *subject;
    const char *image_id;
    int         mintable;
    const char *variant_of;
    const char *variant;
} card_profile_t;

extern const card_profile_t FIRST_BELL_CATALOG[FIRST_BELL_PROFILE_COUNT];
extern const char FIRST_BELL_SET_NAME[];
extern const char FIRST_BELL_SET_CODE[];
extern const char FIRST_BELL_SET_FAMILY[];
extern const char FIRST_BELL_PACK_SHAPE[CARDS_PER_PACK][24];

const card_profile_t* catalog_by_profile_id(const char *id);
const card_profile_t* catalog_by_set_number(int n);
void                  catalog_hash(char out_hex[65]);
int                   catalog_card_count(void);
#endif
