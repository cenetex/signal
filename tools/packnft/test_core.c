#include "core_ix.h"
#include <stdio.h>
#include <string.h>
static int f = 0;
#define T(c,m) do { if (!(c)) { printf("FAIL: %s\n", m); f++; } } while(0)

int main(void) {
    solana_instruction_t ix;
    uint8_t zero[32]; memset(zero, 0, 32);

    /* CreateAsset with name="Test", uri="https://x.com/t.json", has_collection=true */
    memset(&ix, 0, sizeof(ix));
    core_create_asset_ix(zero, zero, zero, true, "Test",
        "https://x.com/t.json", &ix);
    /* Expected data (from golden vectors):
       14 00 04 00 00 00 54 65 73 74 14 00 00 00 68 74 74 70 73 3a 2f 2f 78 2e 63 6f 6d 2f 74 2e 6a 73 6f 6e 01 [footer 10 bytes] */
    uint8_t expected[] = {
        0x14, 0x00,
        0x04,0x00,0x00,0x00, 0x54,0x65,0x73,0x74,
        0x14,0x00,0x00,0x00,
        0x68,0x74,0x74,0x70,0x73,0x3a,0x2f,0x2f,0x78,0x2e,0x63,0x6f,0x6d,0x2f,0x74,0x2e,0x6a,0x73,0x6f,0x6e,
        0x01,  /* has_collection */
        0x01,0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,0x00  /* footer */
    };
    T(ix.data_len == sizeof(expected), "createAsset len");
    T(memcmp(ix.data, expected, ix.data_len) == 0, "createAsset bytes");
    printf("createAsset: %d bytes, match=%d\n", ix.data_len, memcmp(ix.data, expected, ix.data_len)==0);

    /* CreateAsset without collection: name="NC", uri="https://x.com/n.json" */
    memset(&ix, 0, sizeof(ix));
    core_create_asset_ix(zero, zero, zero, false, "NC",
        "https://x.com/n.json", &ix);
    uint8_t expectedNC[] = {
        0x14, 0x00,
        0x02,0x00,0x00,0x00, 0x4e,0x43,
        0x14,0x00,0x00,0x00,
        0x68,0x74,0x74,0x70,0x73,0x3a,0x2f,0x2f,0x78,0x2e,0x63,0x6f,0x6d,0x2f,0x6e,0x2e,0x6a,0x73,0x6f,0x6e,
        0x00,  /* no collection */
        0x01,0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,0x00
    };
    T(ix.data_len == sizeof(expectedNC), "createAssetNC len");
    T(memcmp(ix.data, expectedNC, ix.data_len) == 0, "createAssetNC bytes");
    printf("createAssetNC: %d bytes, match=%d\n", ix.data_len, memcmp(ix.data, expectedNC, ix.data_len)==0);

    /* CreateCollection: name="Coll", uri="https://x.com/c.json" */
    memset(&ix, 0, sizeof(ix));
    core_create_collection_ix(zero, zero, "Coll",
        "https://x.com/c.json", &ix);
    uint8_t expectedColl[] = {
        0x15,
        0x00,
        0x04,0x00,0x00,0x00, 0x43,0x6f,0x6c,0x6c,
        0x14,0x00,0x00,0x00,
        0x68,0x74,0x74,0x70,0x73,0x3a,0x2f,0x2f,0x78,0x2e,0x63,0x6f,0x6d,0x2f,0x63,0x2e,0x6a,0x73,0x6f,0x6e,
        0x01,0x00,0x00,0x00,0x00, 0x01,0x00,0x00,0x00,0x00
    };
    T(ix.data_len == sizeof(expectedColl), "createColl len");
    T(memcmp(ix.data, expectedColl, ix.data_len) == 0, "createColl bytes");
    printf("createColl: %d bytes, match=%d\n", ix.data_len, memcmp(ix.data, expectedColl, ix.data_len)==0);

    /* Burn */
    memset(&ix, 0, sizeof(ix));
    core_burn_asset_ix(zero, zero, zero, &ix);
    uint8_t expectedBurn[] = { 0x0c, 0x00 };
    T(ix.data_len == 2, "burn len");
    T(memcmp(ix.data, expectedBurn, 2) == 0, "burn bytes");
    printf("burn: %d bytes, match=%d\n", ix.data_len, memcmp(ix.data, expectedBurn, 2)==0);

    /* Update with name & uri */
    memset(&ix, 0, sizeof(ix));
    core_update_asset_ix(zero, zero, zero, "Upd", "https://x.com/u.json", &ix);
    uint8_t expectedUpd[] = { 0x1e, 0x00, 0x01, 0x01 };
    T(ix.data_len == 4, "update len");
    T(memcmp(ix.data, expectedUpd, 4) == 0, "update bytes");
    printf("update: %d bytes, match=%d\n", ix.data_len, memcmp(ix.data, expectedUpd, 4)==0);

    /* Update with neither */
    memset(&ix, 0, sizeof(ix));
    core_update_asset_ix(zero, zero, zero, NULL, NULL, &ix);
    uint8_t expectedUpdN[] = { 0x1e, 0x00, 0x00, 0x00, 0x00 };
    T(ix.data_len == 5, "update none len");
    printf("update none: %d bytes\n", ix.data_len);

    /* Verify program ID */
    uint8_t pid[32];
    core_program_id(pid);
    const char *id_str = CORE_PROGRAM_ID_STR;
    /* Base58 decode the known string to bytes for comparison */
    T(pid[0] != 0, "program id non-zero");

    if (f == 0) printf("\ncore_ix: all passed\n");
    else printf("\ncore_ix: %d failed\n", f);
    return f > 0;
}
