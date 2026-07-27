/* Layer A.1 of #479 — verify save/load of player_identity_t through a
 * filesystem path and corruption recovery (.bad rename). */
#include "test_harness.h"

#include <stdio.h>

#include "identity.h"
#include "signal_crypto.h"

static bool identity_entropy_fail_after_partial_write(
    uint8_t *buf, size_t len, void *user) {
    (void)user;
    size_t partial = len < 9 ? len : 9;
    if (buf && partial > 0) memset(buf, 0xA7, partial);
    return false;
}

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

TEST(test_identity_entropy_failure_does_not_create_file) {
    const char *path = TMP("identity_entropy_failure.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);

    player_identity_t id;
    memset(&id, 0xCC, sizeof(id));
    player_identity_t zero = {0};

    signal_crypto_test_set_entropy_provider(
        identity_entropy_fail_after_partial_write, NULL);
    bool ok = identity_load_or_generate_at(&id, path);
    signal_crypto_test_reset_entropy_provider();

    ASSERT(!ok);
    ASSERT(memcmp(&id, &zero, sizeof(id)) == 0);
    FILE *fp = fopen(path, "rb");
    ASSERT(fp == NULL);
    fp = fopen(bad_path, "rb");
    ASSERT(fp == NULL);
}

TEST(test_identity_entropy_failure_preserves_corrupt_file) {
    const char *path = TMP("identity_entropy_preserve_corrupt.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);

    const uint8_t original[] = {0x53, 0x49, 0x47, 0x4E, 0x41, 0x4C};
    FILE *fp = fopen(path, "wb");
    ASSERT(fp != NULL);
    ASSERT(fwrite(original, 1, sizeof(original), fp) == sizeof(original));
    fclose(fp);

    player_identity_t id;
    memset(&id, 0xCC, sizeof(id));
    player_identity_t zero = {0};
    signal_crypto_test_set_entropy_provider(
        identity_entropy_fail_after_partial_write, NULL);
    bool ok = identity_load_or_generate_at(&id, path);
    signal_crypto_test_reset_entropy_provider();

    ASSERT(!ok);
    ASSERT(memcmp(&id, &zero, sizeof(id)) == 0);
    fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    uint8_t got[sizeof(original)] = {0};
    ASSERT(fread(got, 1, sizeof(got), fp) == sizeof(got));
    ASSERT(fgetc(fp) == EOF);
    fclose(fp);
    ASSERT(memcmp(got, original, sizeof(got)) == 0);
    fp = fopen(bad_path, "rb");
    ASSERT(fp == NULL);

    remove(path);
}

void register_identity_tests(void);
void register_identity_tests(void) {
    TEST_SECTION("\nIdentity (player keypair) tests:\n");
    RUN(test_identity_save_then_load);
    RUN(test_identity_corrupt_file_renamed_to_bad);
    RUN(test_identity_entropy_failure_does_not_create_file);
    RUN(test_identity_entropy_failure_preserves_corrupt_file);
}
