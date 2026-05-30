#include "catalog.h"
#include "reveal.h"
#include "metadata.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    int ok = 1;

    /* Catalog */
    if (catalog_card_count() != 36) { printf("FAIL card count\n"); ok = 0; }
    const card_profile_t *c = catalog_by_profile_id("ruby");
    if (!c || strcmp(c->character_name, "Ruby") != 0) { printf("FAIL ruby lookup\n"); ok = 0; }
    c = catalog_by_set_number(12);
    if (!c || strcmp(c->rarity, "ultra-rare") != 0) { printf("FAIL captain null\n"); ok = 0; }

    /* Reveal */
    char commit[65];
    reveal_pack_commitment("abc", "addr", "sig", "owner", "prod", 5, "n", commit);
    if (strlen(commit) != 64) { printf("FAIL commitment len\n"); ok = 0; }

    char seed[65];
    reveal_pack_seed(commit, "addr", "tx", "n", seed);
    if (strlen(seed) != 64) { printf("FAIL seed len\n"); ok = 0; }

    char proof[65]; int ci;
    reveal_card_slot(commit, seed, "addr", 0, proof, &ci);
    if (ci < 0 || ci >= 24) { printf("FAIL card index %d\n", ci); ok = 0; }

    /* Metadata */
    char meta[4096];
    int ml = metadata_pack_json("Test", "TST", "img", "coll", "p1", 1, 5, 42, NULL, meta, sizeof(meta));
    if (ml <= 0 || !strstr(meta, "Test")) { printf("FAIL pack json\n"); ok = 0; }

    c = catalog_by_profile_id("captain-null");
    ml = metadata_card_json(c, "img", "coll", NULL, meta, sizeof(meta));
    if (ml <= 0 || !strstr(meta, "Captain Null")) { printf("FAIL card json\n"); ok = 0; }

    ml = metadata_collection_json("RH", "Ruby High", "RUBY", "img", "desc", meta, sizeof(meta));
    if (ml <= 0 || !strstr(meta, "Ruby High")) { printf("FAIL collection json\n"); ok = 0; }

    if (ok) printf("ALL SMOKE TESTS PASSED\n");
    else    printf("SOME TESTS FAILED\n");
    return ok ? 0 : 1;
}
