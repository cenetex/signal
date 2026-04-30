/* Layer A.1 of #479 — verify save/load of player_identity_t through a
 * filesystem path and corruption recovery (.bad rename). */
#include "test_harness.h"

#include <stdio.h>

#include "identity.h"
#include "signal_crypto.h"

TEST(test_identity_save_then_load) {
    const char *path = TMP("identity_roundtrip.key");
    remove(path); /* ensure fresh-generate path */

    player_identity_t a;
    ASSERT(identity_load_or_generate_at(&a, path));

    /* Pubkey is the trailing 32 bytes of secret (NaCl convention). */
    ASSERT(memcmp(a.pubkey,
                  a.secret + (SIGNAL_CRYPTO_SECRET_BYTES - SIGNAL_CRYPTO_PUBKEY_BYTES),
                  SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);

    player_identity_t b;
    ASSERT(identity_load_or_generate_at(&b, path));

    ASSERT(memcmp(a.pubkey, b.pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);
    ASSERT(memcmp(a.secret, b.secret, SIGNAL_CRYPTO_SECRET_BYTES) == 0);

    /* Round-trip the keypair through sign/verify so we know the persisted
     * secret is functional, not just bit-equal. */
    uint8_t msg[16];
    for (int i = 0; i < 16; i++) msg[i] = (uint8_t)i;
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), b.secret);
    ASSERT(signal_crypto_verify(sig, msg, sizeof(msg), b.pubkey));

    remove(path);
}

TEST(test_identity_corrupt_file_renamed_to_bad) {
    const char *path = TMP("identity_corrupt.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);

    /* Write a non-64-byte file: clearly corrupt. */
    FILE *fp = fopen(path, "wb");
    ASSERT(fp != NULL);
    const char garbage[] = "this-is-not-a-keypair";
    fwrite(garbage, 1, sizeof(garbage) - 1, fp);
    fclose(fp);

    player_identity_t id;
    ASSERT(identity_load_or_generate_at(&id, path));

    /* The original file should have been renamed to .bad and a fresh
     * 64-byte key file written in its place. */
    fp = fopen(bad_path, "rb");
    ASSERT(fp != NULL);
    fclose(fp);

    fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    uint8_t buf[SIGNAL_CRYPTO_SECRET_BYTES + 1];
    size_t got = fread(buf, 1, sizeof(buf), fp);
    int eof = feof(fp);
    fclose(fp);
    ASSERT(got == SIGNAL_CRYPTO_SECRET_BYTES);
    ASSERT(eof);

    /* And the in-memory identity should be a usable, regenerated keypair. */
    uint8_t msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), id.secret);
    ASSERT(signal_crypto_verify(sig, msg, sizeof(msg), id.pubkey));

    remove(path);
    remove(bad_path);
}

void register_identity_tests(void);
void register_identity_tests(void) {
    TEST_SECTION("\nIdentity (player keypair) tests:\n");
    RUN(test_identity_save_then_load);
    RUN(test_identity_corrupt_file_renamed_to_bad);
}
