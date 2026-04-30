/*
 * test_save_keyed_by_pubkey.c — Layer A.4 of #479.
 *
 * After A.4, per-player saves live at:
 *   saves/pubkey/<base58(pubkey)>.sav   (when REGISTER_PUBKEY happened)
 *   saves/legacy/<token_hex>.sav        (anonymous / pre-A.1 client)
 *
 * Migration from the old <token>.sav layout is claim-by-signature: the
 * client signs ("claim-legacy-save-v1" || token_hex) with its identity
 * secret, and the server renames legacy/<basename>.sav to
 * pubkey/<base58(pubkey)>.sav. First-claim-wins: if two clients race on
 * the same legacy save, the second sees ENOENT.
 *
 * These tests exercise the save-path computation, the pubkey-keyed
 * round-trip, and the claim flow at the file-rename layer (the wire
 * dispatcher that drives the rename lives in server/main.c and is
 * covered manually in the smoke test described in the PR body).
 */

#include "test_harness.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
#else
#include <unistd.h>
#endif

#include "base58.h"
#include "protocol.h"
#include "signal_crypto.h"

/* ---- helpers ----------------------------------------------------- */

static void mkdir_p(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0700);
#endif
}

static void make_save_dir(const char *dir) {
    mkdir_p(dir);
    char sub[512];
    snprintf(sub, sizeof(sub), "%s/legacy", dir);
    mkdir_p(sub);
    snprintf(sub, sizeof(sub), "%s/pubkey", dir);
    mkdir_p(sub);
}

static void fill_token(uint8_t tok[8], uint8_t seed) {
    for (int i = 0; i < 8; i++) tok[i] = (uint8_t)(seed * 7 + i);
}

static void session_token_to_hex_local(const uint8_t token[8], char hex[17]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        hex[i * 2]     = digits[token[i] >> 4];
        hex[i * 2 + 1] = digits[token[i] & 0x0F];
    }
    hex[16] = '\0';
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Drop a sentinel byte block at saves/legacy/player_<token_hex>.sav so
 * we can confirm the right legacy file got renamed. The file is the
 * shape of a real save (PLY6 magic + ship blob + crc trailer) but we
 * don't actually load it via player_load_from_path here — only test
 * the rename mechanics. The pubkey-keyed round-trip test below uses
 * the real save path. */
static void write_sentinel_legacy(const char *dir, const uint8_t token[8],
                                  uint8_t marker) {
    char hex[17];
    session_token_to_hex_local(token, hex);
    char path[512];
    snprintf(path, sizeof(path), "%s/legacy/player_%s.sav", dir, hex);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t buf[16];
    memset(buf, marker, sizeof(buf));
    fwrite(buf, sizeof(buf), 1, f);
    fclose(f);
}

static uint8_t read_first_byte(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t b = 0;
    (void)fread(&b, 1, 1, f);
    fclose(f);
    return b;
}

/* ---- tests ------------------------------------------------------- */

/* 1. Pubkey-keyed save round-trip. */
TEST(test_save_keyed_by_pubkey_roundtrip) {
    const char *dir = TMP("a4_pubkey_rt");
    make_save_dir(dir);

    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk, sk);

    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->id = 0;
    fill_token(sp->session_token, 1);
    sp->session_ready = true;
    memcpy(sp->pubkey, pk, 32);
    sp->pubkey_set = true;
    sp->last_signed_nonce = 12345;
    /* Stamp something on the ship so we know we loaded the right file. */
    sp->ship.cargo[COMMODITY_FERRITE_ORE] = 7.0f;

    ASSERT(player_save(sp, dir, 0));

    /* Confirm the file landed under pubkey/. */
    char b58[64];
    ASSERT(base58_encode(pk, 32, b58, sizeof(b58)) > 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/pubkey/%s.sav", dir, b58);
    ASSERT(file_exists(path));

    /* Fresh world, fresh slot, reload by pubkey. */
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    world_reset(w2);
    server_player_t *sp2 = &w2->players[0];
    sp2->connected = true;
    sp2->id = 0;
    memcpy(sp2->pubkey, pk, 32);
    sp2->pubkey_set = true;
    ASSERT(player_load_by_pubkey(sp2, w2, dir, pk));
    ASSERT_EQ_FLOAT(sp2->ship.cargo[COMMODITY_FERRITE_ORE], 7.0f, 0.001f);
    ASSERT(sp2->last_signed_nonce == 12345);

    /* Cleanup */
    remove(path);
}

/* 2. Legacy save claim — the rename-by-pubkey primitive. */
TEST(test_save_legacy_claim_renames_to_pubkey) {
    const char *dir = TMP("a4_claim_ok");
    make_save_dir(dir);

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk, sk);

    uint8_t token[8];
    fill_token(token, 9);
    write_sentinel_legacy(dir, token, 0xAB);

    char hex[17];
    session_token_to_hex_local(token, hex);
    char src[512], dst[512], b58[64];
    snprintf(src, sizeof(src), "%s/legacy/player_%s.sav", dir, hex);
    ASSERT(file_exists(src));
    ASSERT(base58_encode(pk, 32, b58, sizeof(b58)) > 0);
    snprintf(dst, sizeof(dst), "%s/pubkey/%s.sav", dir, b58);
    ASSERT(!file_exists(dst));

    char basename[80];
    snprintf(basename, sizeof(basename), "player_%s", hex);
    ASSERT(player_save_rename_legacy_to_pubkey(dir, basename, pk));

    ASSERT(!file_exists(src));
    ASSERT(file_exists(dst));
    ASSERT(read_first_byte(dst) == 0xAB);

    remove(dst);
}

/* 3. Bad signature on claim — verified at the wire layer. We exercise
 *    signal_crypto_verify directly so we know a forged signature does
 *    NOT verify; the wire dispatcher in server/main.c uses exactly that
 *    check before calling rename. The rename primitive itself is
 *    auth-free by design, so this test asserts the auth boundary. */
TEST(test_save_legacy_claim_bad_signature_rejected) {
    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk, sk);

    /* Build the message + an attacker's bogus signature. */
    const char *domain = CLAIM_LEGACY_SAVE_DOMAIN;
    const char *token_hex = "0123456789abcdef";
    size_t dlen = strlen(domain);
    size_t tlen = strlen(token_hex);
    uint8_t msg[64];
    memcpy(msg, domain, dlen);
    memcpy(msg + dlen, token_hex, tlen);
    uint8_t bad_sig[SIGNAL_CRYPTO_SIG_BYTES];
    memset(bad_sig, 0x42, sizeof(bad_sig));
    ASSERT(!signal_crypto_verify(bad_sig, msg, dlen + tlen, pk));

    /* And a real signature against the wrong pubkey is also rejected. */
    uint8_t pk2[32], sk2[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk2, sk2);
    uint8_t real_sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(real_sig, msg, dlen + tlen, sk2);
    ASSERT(signal_crypto_verify(real_sig, msg, dlen + tlen, pk2));
    ASSERT(!signal_crypto_verify(real_sig, msg, dlen + tlen, pk));
}

/* 4. Wrong-pubkey-claims-someone-else's-save — first-claim-wins.
 *    Pubkey Q signs the claim for legacy save P, Q's signature verifies
 *    against Q's pubkey, and the rename proceeds — into Q's pubkey
 *    file, not P's. That's the documented A.4 semantics: signature
 *    proves the claimant holds *some* identity, not that they were
 *    ever the legacy session's owner. Auditability is a TODO(#479-A.5). */
TEST(test_save_legacy_claim_wrong_pubkey_first_claim_wins) {
    const char *dir = TMP("a4_first_claim");
    make_save_dir(dir);

    uint8_t qk[32], sk_q[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(qk, sk_q);

    /* Sentinel marker so we can prove the file was renamed, not duped. */
    uint8_t tok[8];
    fill_token(tok, 5);
    write_sentinel_legacy(dir, tok, 0xCD);

    char hex[17];
    session_token_to_hex_local(tok, hex);
    char basename[80];
    snprintf(basename, sizeof(basename), "player_%s", hex);

    /* Q renames the legacy save into their own pubkey path. */
    ASSERT(player_save_rename_legacy_to_pubkey(dir, basename, qk));

    char b58q[64], dst_q[512];
    ASSERT(base58_encode(qk, 32, b58q, sizeof(b58q)) > 0);
    snprintf(dst_q, sizeof(dst_q), "%s/pubkey/%s.sav", dir, b58q);
    ASSERT(file_exists(dst_q));
    ASSERT(read_first_byte(dst_q) == 0xCD);

    /* The legacy file is gone now. Source pubkey P trying the same claim
     * fails — first-claim-wins. */
    char src[512];
    snprintf(src, sizeof(src), "%s/legacy/player_%s.sav", dir, hex);
    ASSERT(!file_exists(src));

    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk, sk);
    ASSERT(!player_save_rename_legacy_to_pubkey(dir, basename, pk));

    remove(dst_q);
}

/* 5. Race: two CLAIM messages for the same legacy save. First wins;
 *    second gets ENOENT (rename returns false). */
TEST(test_save_legacy_claim_race_second_loses) {
    const char *dir = TMP("a4_race");
    make_save_dir(dir);

    uint8_t pk_a[32], sk_a[SIGNAL_CRYPTO_SECRET_BYTES];
    uint8_t pk_b[32], sk_b[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk_a, sk_a);
    signal_crypto_keypair(pk_b, sk_b);

    uint8_t tok[8];
    fill_token(tok, 11);
    write_sentinel_legacy(dir, tok, 0xEF);

    char hex[17];
    session_token_to_hex_local(tok, hex);
    char basename[80];
    snprintf(basename, sizeof(basename), "player_%s", hex);

    /* A wins. */
    ASSERT(player_save_rename_legacy_to_pubkey(dir, basename, pk_a));
    /* B loses (legacy file no longer exists). */
    ASSERT(!player_save_rename_legacy_to_pubkey(dir, basename, pk_b));

    char b58a[64], dst_a[512];
    ASSERT(base58_encode(pk_a, 32, b58a, sizeof(b58a)) > 0);
    snprintf(dst_a, sizeof(dst_a), "%s/pubkey/%s.sav", dir, b58a);
    ASSERT(file_exists(dst_a));

    char b58b[64], dst_b[512];
    ASSERT(base58_encode(pk_b, 32, b58b, sizeof(b58b)) > 0);
    snprintf(dst_b, sizeof(dst_b), "%s/pubkey/%s.sav", dir, b58b);
    ASSERT(!file_exists(dst_b));

    remove(dst_a);
}

/* 6. Anonymous fallback — a pre-A.1 client (no pubkey) round-trips
 *    correctly under saves/legacy/player_<token_hex>.sav. */
TEST(test_save_anonymous_fallback_legacy_path) {
    const char *dir = TMP("a4_anon");
    make_save_dir(dir);

    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    server_player_t *sp = &w->players[2];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->id = 2;
    fill_token(sp->session_token, 3);
    sp->session_ready = true;
    /* No pubkey registered. */
    sp->pubkey_set = false;
    sp->ship.cargo[COMMODITY_CUPRITE_ORE] = 4.5f;

    ASSERT(player_save(sp, dir, 2));

    char hex[17];
    session_token_to_hex_local(sp->session_token, hex);
    char path[512];
    snprintf(path, sizeof(path), "%s/legacy/player_%s.sav", dir, hex);
    ASSERT(file_exists(path));

    /* Reload into a fresh slot, by token. */
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    world_reset(w2);
    server_player_t *sp2 = &w2->players[2];
    sp2->connected = true;
    sp2->id = 2;
    memcpy(sp2->session_token, sp->session_token, 8);
    sp2->session_ready = true;
    ASSERT(player_load_by_token(sp2, w2, dir, sp2->session_token));
    ASSERT_EQ_FLOAT(sp2->ship.cargo[COMMODITY_CUPRITE_ORE], 4.5f, 0.001f);

    remove(path);
}

/* 7. Startup migration: a top-level <dir>/player_<hex>.sav left over
 *    from the v39 layout gets moved into <dir>/legacy/ on first startup. */
TEST(test_save_migrate_legacy_layout_moves_top_level) {
    const char *dir = TMP("a4_migrate");
    mkdir_p(dir);
    /* Drop a sentinel save at the top level, mimicking the pre-A.4 layout. */
    char path[512];
    snprintf(path, sizeof(path), "%s/player_aa11bb22cc33dd44.sav", dir);
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    uint8_t marker = 0x77;
    fwrite(&marker, 1, 1, f);
    fclose(f);

    player_save_migrate_legacy_layout(dir);

    /* The file should now live under <dir>/legacy/. */
    ASSERT(!file_exists(path));
    char moved[512];
    snprintf(moved, sizeof(moved), "%s/legacy/player_aa11bb22cc33dd44.sav", dir);
    ASSERT(file_exists(moved));
    ASSERT(read_first_byte(moved) == 0x77);

    /* Idempotent: a second call is a no-op. */
    player_save_migrate_legacy_layout(dir);
    ASSERT(file_exists(moved));

    remove(moved);
}

void register_save_keyed_by_pubkey_tests(void);
void register_save_keyed_by_pubkey_tests(void) {
    TEST_SECTION("\nSave keyed-by-pubkey (#479 A.4):\n");
    RUN(test_save_keyed_by_pubkey_roundtrip);
    RUN(test_save_legacy_claim_renames_to_pubkey);
    RUN(test_save_legacy_claim_bad_signature_rejected);
    RUN(test_save_legacy_claim_wrong_pubkey_first_claim_wins);
    RUN(test_save_legacy_claim_race_second_loses);
    RUN(test_save_anonymous_fallback_legacy_path);
    RUN(test_save_migrate_legacy_layout_moves_top_level);
}
