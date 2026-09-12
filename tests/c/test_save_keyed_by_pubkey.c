/*
 * test_save_keyed_by_pubkey.c — durable identity save boundaries.
 *
 * Pubkey-keyed saves remain the canonical authenticated path. Anonymous
 * token-keyed saves remain readable for reconnects with the same token, but
 * the old claimant-chosen basename recovery message is retired: it disclosed
 * unrelated names and proved only possession of an unrelated pubkey.
 *
 * These tests keep the ordinary save/load coverage and prove the containment
 * boundary for the retired wire value: token A/B, mixed-case, slot, and path
 * payloads cannot alter authoritative memory or either save namespace.
 */

#include "test_harness.h"
#include "persistence_io.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define rmdir _rmdir
/* MSVC's <sys/stat.h> doesn't define S_ISREG. The mode-bit value matches
 * POSIX semantics; the macro shape is the standard fallback. */
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#else
#include <unistd.h>
#endif

#include "base58.h"
#include "legacy_save_recovery.h"
#include "ownership_quarantine.h"
#include "persistence_generation.h"
#include "pubkey_proof.h"
#include "protocol.h"
#include "signal_crypto.h"
#include "sim_catalog.h"
#include "state_digest.h"

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
static bool write_marker_file(const char *path, uint8_t marker) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint8_t buf[16];
    memset(buf, marker, sizeof(buf));
    bool ok = fwrite(buf, sizeof(buf), 1, f) == 1;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static bool write_sentinel_legacy(const char *dir, const uint8_t token[8],
                                  uint8_t marker) {
    char hex[17];
    session_token_to_hex_local(token, hex);
    char path[512];
    snprintf(path, sizeof(path), "%s/legacy/player_%s.sav", dir, hex);
    return write_marker_file(path, marker);
}

static bool file_is_marker_block(const char *path, uint8_t marker) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[16];
    bool ok = fread(buf, sizeof(buf), 1, f) == 1;
    if (ok) {
        for (size_t i = 0; i < sizeof(buf); i++) {
            if (buf[i] != marker) {
                ok = false;
                break;
            }
        }
    }
    if (ok && fgetc(f) != EOF) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok;
}

static int build_retired_claim_packet(
    uint8_t out[2 + 64 + SIGNAL_CRYPTO_SIG_BYTES],
    const char *basename,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    static const char retired_domain[] = "claim-legacy-save-v1";
    if (!out || !basename || !secret) return 0;
    size_t name_len = strlen(basename);
    size_t domain_len = sizeof(retired_domain) - 1u;
    if (name_len == 0u || name_len > 64u) return 0;
    uint8_t signed_bytes[sizeof(retired_domain) - 1u + 64u];
    memcpy(signed_bytes, retired_domain, domain_len);
    memcpy(signed_bytes + domain_len, basename, name_len);
    out[0] = NET_MSG_CLAIM_LEGACY_SAVE;
    out[1] = (uint8_t)name_len;
    memcpy(&out[2], basename, name_len);
    signal_crypto_sign(
        &out[2 + name_len], signed_bytes, domain_len + name_len, secret);
    return (int)(2u + name_len + SIGNAL_CRYPTO_SIG_BYTES);
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
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->last_signed_nonce = 12345;
    ASSERT(server_finalize_pubkey_identity(w, 0));
    /* Stamp something on the ship so we know we loaded the right file. */
    sp->ship->cargo[COMMODITY_FERRITE_ORE] = 7.0f;

    ASSERT(player_save(sp, dir, 0));
    ASSERT(world_save(w, TMP("pubkey-roundtrip-world.sav")));

    /* Confirm the file landed under pubkey/. */
    char b58[64];
    ASSERT(base58_encode(pk, 32, b58, sizeof(b58)) > 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/pubkey/%s.sav", dir, b58);
    ASSERT(file_exists(path));

    /* Restore the matching world and then authenticate a fresh session. */
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    world_reset(w2);
    ASSERT(world_load(w2, TMP("pubkey-roundtrip-world.sav")));
    server_player_t *sp2 = &w2->players[0];
    player_init_ship(sp2, w2);
    sp2->connected = true;
    sp2->id = 0;
    memcpy(sp2->pubkey, pk, 32);
    sp2->pubkey_set = true;
    sp2->pubkey_proof_ok = true;
    sp2->pubkey_challenge_consumed = true;
    sp2->session_ready = true;
    fill_token(sp2->session_token, 2);
    ASSERT(server_finalize_pubkey_identity(w2, 0));
    ASSERT(player_load_by_pubkey(sp2, w2, dir, pk));
    ASSERT_EQ_FLOAT(sp2->ship->cargo[COMMODITY_FERRITE_ORE], 7.0f, 0.001f);
    ASSERT(sp2->last_signed_nonce == 12345);

    /* Cleanup */
    remove(path);
    remove(TMP("pubkey-roundtrip-world.sav"));
}

TEST(test_save_legacy_claim_wire_value_is_semantically_disabled) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    uint8_t packet[2 + 64 + SIGNAL_CRYPTO_SIG_BYTES] = {
        NET_MSG_CLAIM_LEGACY_SAVE,
    };
    server_legacy_save_claim_result_t result;
    ASSERT(!server_dispatch_legacy_save_claim_message(
        w, 0, packet, 1, &result));
    ASSERT_EQ_INT(result.status, SERVER_LEGACY_SAVE_CLAIM_DISABLED);

    packet[0] = NET_MSG_INPUT;
    ASSERT(!server_dispatch_legacy_save_claim_message(
        w, 0, packet, 1, &result));
    ASSERT_EQ_INT(result.status, SERVER_LEGACY_SAVE_CLAIM_MALFORMED);
    ASSERT(!server_dispatch_legacy_save_claim_message(
        w, -1, packet, 1, &result));
    ASSERT_EQ_INT(result.status, SERVER_LEGACY_SAVE_CLAIM_MALFORMED);
}

TEST(test_save_legacy_claim_token_ab_and_name_inputs_are_inert) {
    const char *dir = TMP("legacy_claim_disabled");
    make_save_dir(dir);

    uint8_t token_a[8], token_b[8];
    fill_token(token_a, 9);
    fill_token(token_b, 21);
    ASSERT(write_sentinel_legacy(dir, token_a, 0xA1));
    ASSERT(write_sentinel_legacy(dir, token_b, 0xB2));

    char hex_a[17], hex_b[17];
    session_token_to_hex_local(token_a, hex_a);
    session_token_to_hex_local(token_b, hex_b);
    char src_a[512], src_b[512];
    snprintf(src_a, sizeof(src_a), "%s/legacy/player_%s.sav", dir, hex_a);
    snprintf(src_b, sizeof(src_b), "%s/legacy/player_%s.sav", dir, hex_b);

    char slot_src[512], mixed_src[512];
    snprintf(slot_src, sizeof(slot_src), "%s/legacy/player_7.sav", dir);
    snprintf(mixed_src, sizeof(mixed_src),
             "%s/legacy/player_ABCDEF0123456789.sav", dir);
    ASSERT(write_marker_file(slot_src, 0x71));
    ASSERT(write_marker_file(mixed_src, 0xC3));

    uint8_t pk_a[32], sk_a[SIGNAL_CRYPTO_SECRET_BYTES];
    uint8_t pk_b[32], sk_b[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk_a, sk_a));
    ASSERT(signal_crypto_keypair(pk_b, sk_b));

    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    for (int i = 0; i < 2; i++) {
        server_player_t *sp = &w->players[i];
        sp->connected = true;
        sp->session_ready = true;
        sp->pubkey_set = true;
        sp->pubkey_proof_ok = true;
        sp->pubkey_challenge_consumed = true;
    }
    memcpy(w->players[0].session_token, token_a, sizeof(token_a));
    memcpy(w->players[0].pubkey, pk_a, sizeof(pk_a));
    memcpy(w->players[1].session_token, token_b, sizeof(token_b));
    memcpy(w->players[1].pubkey, pk_b, sizeof(pk_b));

    char b58_a[64], b58_b[64], dst_a[512], dst_b[512], audit[512];
    ASSERT(base58_encode(pk_a, 32, b58_a, sizeof(b58_a)) > 0);
    ASSERT(base58_encode(pk_b, 32, b58_b, sizeof(b58_b)) > 0);
    snprintf(dst_a, sizeof(dst_a), "%s/pubkey/%s.sav", dir, b58_a);
    snprintf(dst_b, sizeof(dst_b), "%s/pubkey/%s.sav", dir, b58_b);
    snprintf(audit, sizeof(audit), "%s/legacy_claims.log", dir);
    ASSERT(!file_exists(dst_a));
    ASSERT(write_marker_file(dst_b, 0xD4));
    ASSERT(!file_exists(audit));

    uint8_t *world_before = malloc(sizeof(*w));
    ASSERT(world_before != NULL);
    memcpy(world_before, w, sizeof(*w));

    struct retired_claim_case {
        int player_idx;
        const char *name;
        const uint8_t *secret;
    };
    const struct retired_claim_case cases[] = {
        /* A cannot name or claim B, even with A's valid signature. */
        {0, hex_b, sk_a},
        /* B cannot name or claim A; B's existing destination is untouched. */
        {1, hex_a, sk_b},
        {0, "7", sk_a},
        {0, "ABCDEF0123456789", sk_a},
        {0, "../player_7", sk_a},
        {0, "..\\player_7", sk_a},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t packet[2 + 64 + SIGNAL_CRYPTO_SIG_BYTES];
        int packet_len = build_retired_claim_packet(
            packet, cases[i].name, cases[i].secret);
        ASSERT(packet_len > 0);
        server_legacy_save_claim_result_t result;
        ASSERT(!server_dispatch_legacy_save_claim_message(
            w, cases[i].player_idx, packet, packet_len, &result));
        ASSERT_EQ_INT(result.status, SERVER_LEGACY_SAVE_CLAIM_DISABLED);
        ASSERT(memcmp(world_before, w, sizeof(*w)) == 0);
        ASSERT(file_is_marker_block(src_a, 0xA1));
        ASSERT(file_is_marker_block(src_b, 0xB2));
        ASSERT(file_is_marker_block(slot_src, 0x71));
        ASSERT(file_is_marker_block(mixed_src, 0xC3));
        ASSERT(!file_exists(dst_a));
        ASSERT(file_is_marker_block(dst_b, 0xD4));
        ASSERT(!file_exists(audit));
    }

    free(world_before);
    remove(src_a);
    remove(src_b);
    remove(slot_src);
    remove(mixed_src);
    remove(dst_b);
}

TEST(test_pubkey_proof_is_session_and_challenge_bound) {
    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(pk, sk));

    uint8_t token[8];
    uint8_t other_token[8];
    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    uint8_t other_challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    fill_token(token, 14);
    fill_token(other_token, 15);
    for (int i = 0; i < PUBKEY_PROOF_CHALLENGE_SIZE; i++) {
        challenge[i] = (uint8_t)(0x40 + i);
        other_challenge[i] = (uint8_t)(0x90 + i);
    }

    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    ASSERT(pubkey_proof_sign(sig, pk, sk, token, challenge));
    ASSERT(pubkey_proof_verify(pk, token, challenge, sig));
    ASSERT(!pubkey_proof_verify(
        pk, other_token, challenge, sig));
    ASSERT(!pubkey_proof_verify(
        pk, token, other_challenge, sig));

    uint8_t other_pk[32], other_sk[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(signal_crypto_keypair(other_pk, other_sk));
    ASSERT(!pubkey_proof_verify(
        other_pk, token, challenge, sig));
}

TEST(test_pubkey_persistence_gate_requires_verified_proof) {
    uint8_t pk[32], sk[SIGNAL_CRYPTO_SECRET_BYTES];
    signal_crypto_keypair(pk, sk);
    (void)sk;

    SERVER_PLAYER_DECL(sp);
    sp.id = 7;
    fill_token(sp.session_token, 16);
    ASSERT(!server_player_can_use_pubkey_persistence(&sp));

    sp.session_ready = true;
    ASSERT(!server_player_can_use_pubkey_persistence(&sp));

    memcpy(sp.pubkey, pk, 32);
    sp.pubkey_set = true;
    ASSERT(!server_player_can_use_pubkey_persistence(&sp));

    char path[512];
    ASSERT(player_save_path(path, sizeof(path), TMP("a4_gate"), &sp, 7));
    ASSERT(strstr(path, "/legacy/") != NULL);

    sp.pubkey_proof_ok = true;
    ASSERT(!server_player_can_use_pubkey_persistence(&sp));
    sp.pubkey_challenge_consumed = true;
    ASSERT(server_player_can_use_pubkey_persistence(&sp));
    ASSERT(player_save_path(path, sizeof(path), TMP("a4_gate"), &sp, 7));
    ASSERT(strstr(path, "/pubkey/") != NULL);
}

/* Anonymous fallback — a pre-A.1 client (no pubkey) round-trips
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
    sp->ship->cargo[COMMODITY_CUPRITE_ORE] = 4.5f;

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
    ASSERT_EQ_FLOAT(sp2->ship->cargo[COMMODITY_CUPRITE_ORE], 4.5f, 0.001f);

    remove(path);
}

static bool recovery_entropy_failure(
    uint8_t *bytes, size_t size, void *user) {
    (void)user;
    if (bytes && size > 0) memset(bytes, 0xA5, size);
    return false;
}

static bool recovery_prove_player_with_key(
    world_t *world,
    int slot,
    const uint8_t token[8],
    const uint8_t pubkey[32],
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    if (!world || !token || !pubkey || !secret ||
        slot < 0 || slot >= MAX_PLAYERS) {
        return false;
    }
    /* Match the dedicated server's fresh transport-slot preparation. */
    world_player_ship_slot_release(world, slot);
    world_player_runtime_slot_reset(world, slot);
    server_player_t *player = &world->players[slot];
    player->connected = true;
    player->id = (uint8_t)slot;
    player->session_ready = true;
    memcpy(player->session_token, token, 8);

    uint8_t registration[REGISTER_PUBKEY_MSG_SIZE] = {
        NET_MSG_REGISTER_PUBKEY,
    };
    memcpy(&registration[1], pubkey, 32);
    server_pubkey_register_result_t register_result;
    if (!server_dispatch_register_pubkey_message(
            world, slot, registration, sizeof(registration),
            &register_result)) {
        return false;
    }

    uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE];
    if (!server_issue_pubkey_challenge(world, slot, challenge))
        return false;
    uint8_t signature[SIGNAL_CRYPTO_SIG_BYTES];
    if (!pubkey_proof_sign(
            signature, pubkey, secret, token, challenge)) {
        return false;
    }
    uint8_t proof[PROVE_PUBKEY_MSG_SIZE] = {
        NET_MSG_PROVE_PUBKEY,
    };
    memcpy(&proof[PROVE_PUBKEY_PUBKEY_OFFSET], pubkey, 32);
    memcpy(&proof[PROVE_PUBKEY_TOKEN_OFFSET], token, 8);
    memcpy(&proof[PROVE_PUBKEY_SIG_OFFSET], signature,
           sizeof(signature));
    server_pubkey_proof_result_t proof_result;
    return server_dispatch_pubkey_proof_message(
               world, slot, proof, sizeof(proof), &proof_result) &&
           proof_result.verified;
}

static bool recovery_authenticate_player(
    world_t *world,
    int slot,
    const uint8_t token[8],
    uint8_t pubkey[32],
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    return pubkey && secret &&
           signal_crypto_keypair(pubkey, secret) &&
           recovery_prove_player_with_key(
               world, slot, token, pubkey, secret);
}

static bool recovery_source_path(
    char *out, size_t out_size, const char *player_dir,
    const uint8_t token[8]) {
    char hex[17];
    session_token_to_hex_local(token, hex);
    int n = snprintf(out, out_size,
                     "%s/legacy/player_%s.sav",
                     player_dir, hex);
    return n > 0 && (size_t)n < out_size;
}

static bool recovery_destination_path(
    char *out, size_t out_size, const char *player_dir,
    const uint8_t pubkey[32]) {
    char encoded[64];
    if (base58_encode(
            pubkey, 32, encoded, sizeof(encoded)) == 0) {
        return false;
    }
    int n = snprintf(out, out_size,
                     "%s/pubkey/%s.sav",
                     player_dir, encoded);
    return n > 0 && (size_t)n < out_size;
}

static bool recovery_rewrite_current_save_as(
    const char *path, uint32_t magic, size_t trim_bytes) {
    if (!path) return false;
    FILE *input = fopen(path, "rb");
    if (!input) return false;
    bool ok = fseek(input, 0, SEEK_END) == 0;
    long end = ok ? ftell(input) : -1;
    if (end < 0 || (size_t)end <= trim_bytes ||
        fseek(input, 0, SEEK_SET) != 0) {
        fclose(input);
        return false;
    }
    size_t output_size = (size_t)end - trim_bytes;
    uint8_t *bytes = malloc((size_t)end);
    if (!bytes) {
        fclose(input);
        return false;
    }
    ok = fread(bytes, (size_t)end, 1, input) == 1;
    if (fclose(input) != 0) ok = false;
    if (!ok) {
        free(bytes);
        return false;
    }
    memcpy(bytes, &magic, sizeof(magic));
    if (magic == 0x504C5937u) {
        if (output_size < 8u) { free(bytes); return false; }
        uint32_t crc_magic = 0x43524332u;
        uint32_t crc = persistence_crc32_update(0, bytes, output_size - 8u);
        memcpy(bytes + output_size - 8u, &crc_magic, sizeof(crc_magic));
        memcpy(bytes + output_size - 4u, &crc, sizeof(crc));
    }
    FILE *output = fopen(path, "wb");
    if (!output) {
        free(bytes);
        return false;
    }
    ok = fwrite(bytes, output_size, 1, output) == 1;
    if (fclose(output) != 0) ok = false;
    free(bytes);
    return ok;
}

#if defined(SIGNAL_SAVE_TESTING)
typedef struct {
    const char *replacement_path;
    uint8_t marker;
    bool completed;
    char observed_destination[512];
} recovery_race_hook_state_t;

static void recovery_source_swap_test_hook(
    const char *source_path,
    const char *destination_path,
    void *user) {
    (void)destination_path;
    recovery_race_hook_state_t *state = user;
    if (!state || !source_path ||
        !state->replacement_path) {
        return;
    }
    char original[512];
    int n = snprintf(
        original, sizeof(original), "%s.original",
        source_path);
    state->completed =
        n > 0 && (size_t)n < sizeof(original) &&
        rename(source_path, original) == 0 &&
        rename(state->replacement_path, source_path) == 0;
}

static void recovery_destination_race_test_hook(
    const char *source_path,
    const char *destination_path,
    void *user) {
    (void)source_path;
    recovery_race_hook_state_t *state = user;
    if (!state || !destination_path) return;
    int n = snprintf(
        state->observed_destination,
        sizeof(state->observed_destination),
        "%s", destination_path);
    state->completed =
        n > 0 &&
        (size_t)n < sizeof(state->observed_destination) &&
        write_marker_file(destination_path, state->marker);
}

static void recovery_selected_destination_race_test_hook(
    const char *source_path,
    const char *destination_path,
    void *user) {
    (void)source_path;
    (void)destination_path;
    recovery_race_hook_state_t *state = user;
    if (!state || !state->replacement_path) return;
    int n = snprintf(
        state->observed_destination,
        sizeof(state->observed_destination),
        "%s", state->replacement_path);
    state->completed =
        n > 0 &&
        (size_t)n < sizeof(state->observed_destination) &&
        write_marker_file(state->replacement_path, state->marker);
}

#ifndef _WIN32
static void recovery_destination_parent_symlink_test_hook(
    const char *source_path,
    const char *destination_path,
    void *user) {
    (void)source_path;
    recovery_race_hook_state_t *state = user;
    if (!state || !state->replacement_path ||
        !destination_path) {
        return;
    }
    int n = snprintf(
        state->observed_destination,
        sizeof(state->observed_destination),
        "%s", destination_path);
    if (n <= 0 ||
        (size_t)n >= sizeof(state->observed_destination)) {
        return;
    }
    char parent[512];
    n = snprintf(parent, sizeof(parent), "%s",
                 destination_path);
    if (n <= 0 || (size_t)n >= sizeof(parent))
        return;
    char *slash = strrchr(parent, '/');
    if (!slash) return;
    *slash = '\0';
    char original_parent[512];
    n = snprintf(
        original_parent, sizeof(original_parent),
        "%s.original", parent);
    state->completed =
        n > 0 &&
        (size_t)n < sizeof(original_parent) &&
        rename(parent, original_parent) == 0 &&
        symlink(state->replacement_path, parent) == 0;
}
#endif
#endif

static bool recovery_prepare_legacy_player(
    world_t *world,
    const char *player_dir,
    const uint8_t token[8],
    uint8_t pubkey[32],
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    float saved_hull,
    float live_hull,
    uint64_t saved_nonce,
    uint64_t live_nonce) {
    if (!world || !player_dir || !token || !pubkey || !secret)
        return false;
    make_save_dir(player_dir);
    world_reset(world);
    server_player_t *player = &world->players[0];
    player->connected = true;
    player->id = 0;
    player->session_ready = true;
    memcpy(player->session_token, token, 8);
    player_init_ship(player, world);
    if (!player->ship) return false;
    player->ship->hull = saved_hull;
    player->ship->cargo[COMMODITY_FERRITE_ORE] = 7.0f;
    player->last_signed_nonce = saved_nonce;
    if (!player_save(player, player_dir, 0) ||
        !signal_crypto_keypair(pubkey, secret)) {
        return false;
    }

    memcpy(player->pubkey, pubkey, 32);
    player->pubkey_set = true;
    player->pubkey_proof_ok = true;
    player->pubkey_challenge_consumed = true;
    memset(player->pubkey_proof_transcript, 0x5A,
           sizeof(player->pubkey_proof_transcript));
    player->last_signed_nonce = live_nonce;
    player->ship->hull = live_hull;
    player->ship->cargo[COMMODITY_FERRITE_ORE] = 0.0f;
    player->legacy_recovery_save_pending = true;
    return server_finalize_pubkey_identity(world, 0);
}

TEST(test_legacy_recovery_offer_is_proof_connection_and_expiry_bound) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    uint8_t token[8];
    fill_token(token, 21);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_authenticate_player(
        world, 0, token, pubkey, secret));
    ASSERT(!world->players[0].pubkey_identity_finalized);
    ASSERT(world->players[0].ship == NULL);
    for (int i = 0; i < MAX_PLAYERS; i++)
        ASSERT(!world->pubkey_registry[i].in_use);

    uint8_t zero[32] = {0};
    ASSERT(memcmp(world->players[0].pubkey_proof_transcript,
                  zero, sizeof(zero)) != 0);

    legacy_recovery_offer_t offer = {0};
    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 17, 1000, 100));
    uint8_t offer_id[LEGACY_RECOVERY_OFFER_ID_SIZE];
    memcpy(offer_id, offer.offer_id, sizeof(offer_id));
    uint8_t packet[NET_LEGACY_RECOVERY_OFFER_SIZE];
    ASSERT_EQ_INT(legacy_recovery_serialize_offer(
                      packet, &offer, 1099),
                  NET_LEGACY_RECOVERY_OFFER_SIZE);
    ASSERT_EQ_INT(packet[0], NET_MSG_LEGACY_RECOVERY_OFFER);
    ASSERT_EQ_INT(packet[1 + LEGACY_RECOVERY_OFFER_ID_SIZE], 1);
    ASSERT_EQ_INT(packet[2 + LEGACY_RECOVERY_OFFER_ID_SIZE], 0);
    ASSERT_EQ_INT(legacy_recovery_serialize_offer(
                      packet, &offer, 1100), 0);
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 17, 1100,
                      offer_id, sizeof(offer_id)),
                  LEGACY_RECOVERY_RESULT_STALE_OFFER);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_NONE);

    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 18, 2000, 1000));
    memcpy(offer_id, offer.offer_id, sizeof(offer_id));
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 19, 2001,
                      offer_id, sizeof(offer_id)),
                  LEGACY_RECOVERY_RESULT_STALE_OFFER);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_NONE);

    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 20, 3000, 1000));
    memcpy(offer_id, offer.offer_id, sizeof(offer_id));
    world->players[0].pubkey_proof_transcript[0] ^= 0x01u;
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 20, 3001,
                      offer_id, sizeof(offer_id)),
                  LEGACY_RECOVERY_RESULT_STALE_OFFER);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_NONE);
}

TEST(test_legacy_recovery_expiry_releases_global_pause_until_reconnect) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    uint8_t token[8];
    fill_token(token, 35);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_authenticate_player(
        world, 0, token, pubkey, secret));
    world->players[0].last_signed_nonce = 77;

    legacy_recovery_offer_t offer = {0};
    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 91, 1000, 100));
    uint8_t expired_id[LEGACY_RECOVERY_OFFER_ID_SIZE];
    memcpy(expired_id, offer.offer_id, sizeof(expired_id));
    ASSERT(!legacy_recovery_offer_expired(&offer, 1099));
    ASSERT(legacy_recovery_offer_blocks_persistence(
        &offer, 1099));
    ASSERT(legacy_recovery_offer_expired(&offer, 1100));
    ASSERT(!legacy_recovery_offer_blocks_persistence(
        &offer, 1100));
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 91, 1100,
                      expired_id, sizeof(expired_id)),
                  LEGACY_RECOVERY_RESULT_STALE_OFFER);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_NONE);
    ASSERT(world->players[0].last_signed_nonce == 77);
    ASSERT(!legacy_recovery_offer_blocks_persistence(
        &offer, 1100));

    /* Only a new transport generation receives another one-time offer. */
    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 92, 1200, 100));
    ASSERT(memcmp(expired_id, offer.offer_id,
                  sizeof(expired_id)) != 0);
    uint8_t refreshed_id[LEGACY_RECOVERY_OFFER_ID_SIZE];
    memcpy(refreshed_id, offer.offer_id, sizeof(refreshed_id));
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 92, 1201,
                      refreshed_id, sizeof(refreshed_id)),
                  LEGACY_RECOVERY_RESULT_SUCCESS);
    ASSERT(world->players[0].last_signed_nonce == 77);
}

TEST(test_legacy_recovery_expiry_abort_is_generation_byte_safe) {
    const char *players =
        TMP("legacy_recovery_expiry_abort_players");
    const char *root =
        TMP("legacy_recovery_expiry_abort_root");
    uint8_t claimant_token[8];
    uint8_t unrelated_token[8];
    fill_token(claimant_token, 49);
    fill_token(unrelated_token, 50);

    WORLD_HEAP source_world = calloc(1, sizeof(*source_world));
    WORLD_HEAP live = calloc(1, sizeof(*live));
    WORLD_HEAP restarted = calloc(1, sizeof(*restarted));
    ASSERT(source_world != NULL);
    ASSERT(live != NULL);
    ASSERT(restarted != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        source_world, players, claimant_token,
        pubkey, secret, 66.0f, 1.0f, 90, 2));

    world_reset(live);
    world_player_ship_slot_release(live, 0);
    world_player_runtime_slot_reset(live, 0);
    uint8_t baseline[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after_abort[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(live, baseline);
    ASSERT(recovery_prove_player_with_key(
        live, 0, claimant_token, pubkey, secret));
    server_player_t *claimant = &live->players[0];
    claimant->legacy_recovery_save_pending = true;
    ASSERT(!claimant->pubkey_identity_finalized);
    ASSERT(claimant->ship == NULL);
    ASSERT(claimant->ship_asset_id == SHIP_ASSET_ID_NONE);

    legacy_recovery_offer_t offer = {0};
    ASSERT(legacy_recovery_offer_issue(
        &offer, live, 0, 101, 1000, 100));
    uint8_t expired_id[LEGACY_RECOVERY_OFFER_ID_SIZE];
    memcpy(expired_id, offer.offer_id, sizeof(expired_id));
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, live, 0, 101, 1100,
                      expired_id, sizeof(expired_id)),
                  LEGACY_RECOVERY_RESULT_STALE_OFFER);
    ASSERT(world_player_abort_provisional_legacy_recovery(
        live, 0));
    signal_authoritative_state_digest(live, after_abort);
    ASSERT_EQ_INT(memcmp(baseline, after_abort, sizeof(baseline)), 0);

    server_player_t *unrelated = &live->players[1];
    unrelated->connected = true;
    unrelated->id = 1;
    unrelated->session_ready = true;
    memcpy(unrelated->session_token, unrelated_token, 8);
    player_init_ship(unrelated, live);
    ASSERT(unrelated->ship != NULL);
    unrelated->ship->hull = 39.0f;
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[1] = true;
    persistence_generation_paths_t published = {0};
    ASSERT(persistence_generation_commit(
        root, players, live, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE, &published));

    world_reset(restarted);
    ASSERT(station_catalog_load_all(
               restarted->stations, MAX_STATIONS,
               published.catalog_dir) >= 0);
    ASSERT(world_load(restarted, published.world_path));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        ASSERT(!restarted->pubkey_registry[i].in_use ||
               memcmp(restarted->pubkey_registry[i].pubkey,
                      pubkey, 32) != 0);
    }
    for (int i = 0; i < MAX_SHIP_ASSETS; i++) {
        const ship_asset_t *asset = &restarted->ship_assets[i];
        ASSERT(!asset->active ||
               asset->owner_principal.kind !=
                   ACTOR_PRINCIPAL_PLAYER ||
               memcmp(asset->owner_principal.id,
                      pubkey, 32) != 0);
        ASSERT(!asset->active ||
               asset->operator_kind !=
                   SHIP_ASSET_OPERATOR_PLAYER ||
               asset->operator_slot != 0);
    }
    ASSERT(!restarted->characters[0].active);
    ASSERT_EQ_INT(legacy_recovery_source_probe(
                      root, players, claimant_token, pubkey),
                  LEGACY_RECOVERY_SOURCE_CANDIDATE);

    ASSERT(recovery_prove_player_with_key(
        restarted, 0, claimant_token, pubkey, secret));
    restarted->players[0].legacy_recovery_save_pending = true;
    memset(save_slots, 0, sizeof(save_slots));
    save_slots[0] = true;
    ASSERT_EQ_INT(legacy_recovery_execute(
                      restarted, 0, 91, root, players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE, NULL),
                  LEGACY_RECOVERY_RESULT_SUCCESS);
    ASSERT(restarted->players[0].pubkey_identity_finalized);
    ASSERT_EQ_FLOAT(restarted->players[0].ship->hull,
                    66.0f, 0.001f);
}

TEST(test_legacy_recovery_offer_wrong_input_consumes_and_reuse_is_replay) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    uint8_t token[8];
    fill_token(token, 22);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_authenticate_player(
        world, 0, token, pubkey, secret));

    legacy_recovery_offer_t offer = {0};
    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 31, 1000, 1000));
    uint8_t wrong[LEGACY_RECOVERY_OFFER_ID_SIZE];
    memcpy(wrong, offer.offer_id, sizeof(wrong));
    wrong[0] ^= 0x80u;
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 31, 1001,
                      wrong, sizeof(wrong)),
                  LEGACY_RECOVERY_RESULT_STALE_OFFER);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_NONE);

    ASSERT(legacy_recovery_offer_issue(
        &offer, world, 0, 32, 2000, 1000));
    uint8_t exact[LEGACY_RECOVERY_OFFER_ID_SIZE];
    memcpy(exact, offer.offer_id, sizeof(exact));
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 32, 2001,
                      exact, sizeof(exact)),
                  LEGACY_RECOVERY_RESULT_SUCCESS);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_IN_FLIGHT);
    ASSERT_EQ_INT(legacy_recovery_offer_begin(
                      &offer, world, 0, 32, 2002,
                      exact, sizeof(exact)),
                  LEGACY_RECOVERY_RESULT_REPLAY);
    ASSERT_EQ_INT(offer.phase, LEGACY_RECOVERY_OFFER_NONE);
}

TEST(test_legacy_recovery_offer_entropy_failure_clears_state) {
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    world_reset(world);
    uint8_t token[8];
    fill_token(token, 23);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_authenticate_player(
        world, 0, token, pubkey, secret));

    legacy_recovery_offer_t offer;
    memset(&offer, 0xCC, sizeof(offer));
    signal_crypto_test_set_entropy_provider(
        recovery_entropy_failure, NULL);
    bool issued = legacy_recovery_offer_issue(
        &offer, world, 0, 41, 1000, 1000);
    signal_crypto_test_reset_entropy_provider();
    legacy_recovery_offer_t zero = {0};
    ASSERT(!issued);
    ASSERT_EQ_INT(memcmp(&offer, &zero, sizeof(offer)), 0);
}

TEST(test_legacy_recovery_success_publishes_atomic_pubkey_save) {
    const char *player_dir = TMP("legacy_recovery_success_players");
    const char *generation_root = TMP("legacy_recovery_success_generations");
    uint8_t token_a[8], token_b[8];
    fill_token(token_a, 24);
    fill_token(token_b, 25);

    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, player_dir, token_a, pubkey, secret,
        71.0f, 9.0f, 500, 100));

    server_player_t *other = &world->players[1];
    other->connected = true;
    other->id = 1;
    other->session_ready = true;
    memcpy(other->session_token, token_b, 8);
    player_init_ship(other, world);
    ASSERT(other->ship != NULL);
    other->ship->hull = 44.0f;
    ASSERT(player_save(other, player_dir, 1));

    ASSERT_EQ_INT(legacy_recovery_source_probe(
                      generation_root, player_dir,
                      token_a, pubkey),
                  LEGACY_RECOVERY_SOURCE_CANDIDATE);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    persistence_generation_paths_t published = {0};
    ASSERT_EQ_INT(legacy_recovery_execute(
                      world, 0, 200,
                      generation_root, player_dir,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE,
                      &published),
                  LEGACY_RECOVERY_RESULT_SUCCESS);
    ASSERT_EQ_FLOAT(world->players[0].ship->hull, 71.0f, 0.001f);
    ASSERT_EQ_FLOAT(
        world->players[0].ship->cargo[COMMODITY_FERRITE_ORE],
        7.0f, 0.001f);
    ASSERT(world->players[0].last_signed_nonce == 500);
    ASSERT(!world->players[0].legacy_recovery_save_pending);

    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(
                      generation_root, &selected),
                  PERSISTENCE_GENERATION_CURRENT);
    ASSERT(selected.generation == published.generation);
    char source_a[512], source_b[512], destination[512];
    ASSERT(recovery_source_path(
        source_a, sizeof(source_a), selected.player_dir, token_a));
    ASSERT(recovery_source_path(
        source_b, sizeof(source_b), selected.player_dir, token_b));
    ASSERT(recovery_destination_path(
        destination, sizeof(destination),
        selected.player_dir, pubkey));
    ASSERT(!file_exists(source_a));
    ASSERT(file_exists(source_b));
    ASSERT(file_exists(destination));

    WORLD_HEAP loaded = calloc(1, sizeof(*loaded));
    ASSERT(loaded != NULL);
    world_reset(loaded);
    ASSERT(station_catalog_load_all(loaded->stations, MAX_STATIONS, selected.catalog_dir) >= 0);
    ASSERT(world_load(loaded, selected.world_path));
    server_player_t *loaded_player = &loaded->players[0];
    player_init_ship(loaded_player, loaded);
    loaded_player->connected = true;
    loaded_player->session_ready = true;
    memcpy(loaded_player->session_token, token_a, 8);
    memcpy(loaded_player->pubkey, pubkey, 32);
    loaded_player->pubkey_set = true;
    loaded_player->pubkey_proof_ok = true;
    loaded_player->pubkey_challenge_consumed = true;
    ASSERT(server_finalize_pubkey_identity(loaded, 0));
    ASSERT(player_load_by_pubkey(
        loaded_player, loaded, selected.player_dir, pubkey));
    ASSERT_EQ_FLOAT(loaded_player->ship->hull, 71.0f, 0.001f);
    ASSERT(loaded_player->last_signed_nonce == 500);
}

TEST(test_legacy_recovery_consumption_fences_source_bearing_fallback) {
    const char *players =
        TMP("legacy_recovery_fallback_fence_players");
    const char *root =
        TMP("legacy_recovery_fallback_fence_root");
    uint8_t token[8];
    fill_token(token, 51);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    WORLD_HEAP restarted = calloc(1, sizeof(*restarted));
    ASSERT(world != NULL);
    ASSERT(restarted != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        73.0f, 8.0f, 120, 100));

    bool save_slots[MAX_PLAYERS] = {0};
    persistence_generation_paths_t source_generation = {0};
    ASSERT(persistence_generation_commit(
        root, players, world, save_slots,
        PERSISTENCE_GENERATION_FAULT_NONE,
        &source_generation));
    char archived_source[512];
    ASSERT(recovery_source_path(
        archived_source, sizeof(archived_source),
        source_generation.player_dir, token));
    ASSERT(file_exists(archived_source));

    save_slots[0] = true;
    persistence_generation_paths_t recovered_generation = {0};
    ASSERT_EQ_INT(legacy_recovery_execute(
                      world, 0, 121, root, players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE,
                      &recovered_generation),
                  LEGACY_RECOVERY_RESULT_SUCCESS);
    char marker[512];
    int marker_len = snprintf(
        marker, sizeof(marker), "%s/../LEGACY-RECOVERY-CONSUMED",
        recovered_generation.player_dir);
    ASSERT(marker_len > 0 &&
           (size_t)marker_len < sizeof(marker));
    ASSERT(file_exists(marker));

    /*
     * The old generation physically retains the token save for archival
     * rollback analysis. Removing the new world's required artifact makes
     * the current generation invalid; the recovery pointer intentionally has
     * no previous edge, so restart cannot select and spend that archive.
     */
    ASSERT(remove(recovered_generation.world_path) == 0);
    persistence_generation_paths_t unavailable = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(
                      root, &unavailable),
                  PERSISTENCE_GENERATION_INVALID);
    ASSERT(file_exists(archived_source));
    ASSERT_EQ_INT(legacy_recovery_source_probe(
                      root, players, token, pubkey),
                  LEGACY_RECOVERY_SOURCE_INVALID);

    world_reset(restarted);
    ASSERT(recovery_prove_player_with_key(
        restarted, 0, token, pubkey, secret));
    restarted->players[0].legacy_recovery_save_pending = true;
    memset(save_slots, 0, sizeof(save_slots));
    save_slots[0] = true;
    ASSERT_EQ_INT(legacy_recovery_execute(
                      restarted, 0, 122, root, players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE, NULL),
                  LEGACY_RECOVERY_RESULT_INVALID_SOURCE);
    ASSERT(!restarted->players[0].pubkey_identity_finalized);
    ASSERT(restarted->players[0].last_signed_nonce == 0);
}

TEST(test_legacy_recovery_accepts_bounded_ply4_through_ply7) {
    static const struct {
        uint32_t magic;
        size_t trim_bytes;
        uint64_t expected_nonce;
    } versions[] = {
        {0x504C5937u, 8u, 40u},  /* PLY7: remove PLY8 asset and hints; refresh CRC. */
        {0x504C5936u, 16u, 40u}, /* PLY6: remove PLY8 asset, hints, and CRC. */
        {0x504C5935u, 24u, 30u}, /* PLY5: also remove nonce. */
        {0x504C5934u, 26u, 30u}, /* PLY4: also remove manifest count. */
    };
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        char players_name[64];
        char root_name[64];
        snprintf(players_name, sizeof(players_name),
                 "legacy_recovery_ply%u_players",
                 (unsigned)(7u - i));
        snprintf(root_name, sizeof(root_name),
                 "legacy_recovery_ply%u_root",
                 (unsigned)(7u - i));
        const char *players = TMP(players_name);
        const char *root = TMP(root_name);
        uint8_t token[8];
        fill_token(token, (uint8_t)(36 + i));
        WORLD_HEAP world = calloc(1, sizeof(*world));
        ASSERT(world != NULL);
        uint8_t pubkey[32];
        uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
        ASSERT(recovery_prepare_legacy_player(
            world, players, token, pubkey, secret,
            46.0f + (float)i, 17.0f, 40, 20));
        char source[512];
        ASSERT(recovery_source_path(
            source, sizeof(source), players, token));
        ASSERT(recovery_rewrite_current_save_as(
            source, versions[i].magic,
            versions[i].trim_bytes));
        bool save_slots[MAX_PLAYERS] = {0};
        save_slots[0] = true;
        ASSERT_EQ_INT(legacy_recovery_execute(
                          world, 0, 30,
                          root, players, save_slots,
                          PERSISTENCE_GENERATION_FAULT_NONE,
                          NULL),
                      LEGACY_RECOVERY_RESULT_SUCCESS);
        ASSERT_EQ_FLOAT(
            world->players[0].ship->hull,
            46.0f + (float)i, 0.001f);
        ASSERT(world->players[0].last_signed_nonce ==
               versions[i].expected_nonce);
    }
}

TEST(test_legacy_recovery_rejects_unbounded_ply1_through_ply3) {
    static const uint32_t magics[] = {
        0x504C5952u, 0x504C5932u, 0x504C5933u,
    };
    for (size_t i = 0; i < sizeof(magics) / sizeof(magics[0]); i++) {
        char players_name[64];
        char root_name[64];
        snprintf(players_name, sizeof(players_name),
                 "legacy_recovery_old_ply_players_%u",
                 (unsigned)i);
        snprintf(root_name, sizeof(root_name),
                 "legacy_recovery_old_ply_root_%u",
                 (unsigned)i);
        const char *players = TMP(players_name);
        const char *root = TMP(root_name);
        uint8_t token[8];
        fill_token(token, (uint8_t)(40 + i));
        WORLD_HEAP world = calloc(1, sizeof(*world));
        ASSERT(world != NULL);
        uint8_t pubkey[32];
        uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
        ASSERT(recovery_prepare_legacy_player(
            world, players, token, pubkey, secret,
            43.0f, 18.0f, 12, 13));
        char source[512];
        ASSERT(recovery_source_path(
            source, sizeof(source), players, token));
        ASSERT(recovery_rewrite_current_save_as(
            source, magics[i], 18u));
        bool save_slots[MAX_PLAYERS] = {0};
        save_slots[0] = true;
        uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
        uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
        signal_authoritative_state_digest(world, before);
        ASSERT_EQ_INT(legacy_recovery_execute(
                          world, 0, 14,
                          root, players, save_slots,
                          PERSISTENCE_GENERATION_FAULT_NONE,
                          NULL),
                      LEGACY_RECOVERY_RESULT_INVALID_SOURCE);
        ASSERT(world->players[0].last_signed_nonce == 13);
        signal_authoritative_state_digest(world, after);
        ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    }
}

TEST(test_legacy_recovery_destination_conflict_and_corruption_are_inert) {
    const char *conflict_players =
        TMP("legacy_recovery_conflict_players");
    const char *conflict_root =
        TMP("legacy_recovery_conflict_generations");
    uint8_t token[8];
    fill_token(token, 26);
    WORLD_HEAP conflict_world = calloc(1, sizeof(*conflict_world));
    ASSERT(conflict_world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        conflict_world, conflict_players, token, pubkey, secret,
        63.0f, 11.0f, 9, 10));
    char destination[512];
    ASSERT(recovery_destination_path(
        destination, sizeof(destination),
        conflict_players, pubkey));
    ASSERT(write_marker_file(destination, 0xA7));
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(conflict_world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    ASSERT_EQ_INT(legacy_recovery_execute(
                      conflict_world, 0, 11,
                      conflict_root, conflict_players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE, NULL),
                  LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT);
    ASSERT(conflict_world->players[0].last_signed_nonce == 10);
    signal_authoritative_state_digest(conflict_world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    persistence_generation_paths_t absent = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(
                      conflict_root, &absent),
                  PERSISTENCE_GENERATION_NONE);

    const char *corrupt_players =
        TMP("legacy_recovery_corrupt_players");
    const char *corrupt_root =
        TMP("legacy_recovery_corrupt_generations");
    WORLD_HEAP corrupt_world = calloc(1, sizeof(*corrupt_world));
    ASSERT(corrupt_world != NULL);
    fill_token(token, 27);
    ASSERT(recovery_prepare_legacy_player(
        corrupt_world, corrupt_players, token, pubkey, secret,
        62.0f, 12.0f, 4, 5));
    char source[512];
    ASSERT(recovery_source_path(
        source, sizeof(source), corrupt_players, token));
    ASSERT(write_marker_file(source, 0xB8));
    signal_authoritative_state_digest(corrupt_world, before);
    ASSERT_EQ_INT(legacy_recovery_execute(
                      corrupt_world, 0, 6,
                      corrupt_root, corrupt_players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE, NULL),
                  LEGACY_RECOVERY_RESULT_INVALID_SOURCE);
    ASSERT(corrupt_world->players[0].last_signed_nonce == 5);
    signal_authoritative_state_digest(corrupt_world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
}

#ifndef _WIN32
TEST(test_legacy_recovery_rejects_source_symlink_and_hardlink) {
    const char *symlink_players =
        TMP("legacy_recovery_symlink_players");
    const char *symlink_root =
        TMP("legacy_recovery_symlink_generations");
    uint8_t token[8];
    fill_token(token, 28);
    WORLD_HEAP symlink_world = calloc(1, sizeof(*symlink_world));
    ASSERT(symlink_world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        symlink_world, symlink_players, token, pubkey, secret,
        55.0f, 13.0f, 1, 2));
    char source[512];
    char target[512];
    ASSERT(recovery_source_path(
        source, sizeof(source), symlink_players, token));
    snprintf(target, sizeof(target),
             "%s/legacy/target.bin", symlink_players);
    ASSERT(rename(source, target) == 0);
    ASSERT(symlink(target, source) == 0);
    ASSERT_EQ_INT(legacy_recovery_source_probe(
                      symlink_root, symlink_players,
                      token, pubkey),
                  LEGACY_RECOVERY_SOURCE_INVALID);

    const char *hardlink_players =
        TMP("legacy_recovery_hardlink_players");
    const char *hardlink_root =
        TMP("legacy_recovery_hardlink_generations");
    WORLD_HEAP hardlink_world = calloc(1, sizeof(*hardlink_world));
    ASSERT(hardlink_world != NULL);
    fill_token(token, 29);
    ASSERT(recovery_prepare_legacy_player(
        hardlink_world, hardlink_players, token, pubkey, secret,
        54.0f, 14.0f, 1, 2));
    ASSERT(recovery_source_path(
        source, sizeof(source), hardlink_players, token));
    snprintf(target, sizeof(target),
             "%s/legacy/hardlink.bin", hardlink_players);
    ASSERT(link(source, target) == 0);
    ASSERT_EQ_INT(legacy_recovery_source_probe(
                      hardlink_root, hardlink_players,
                      token, pubkey),
                  LEGACY_RECOVERY_SOURCE_INVALID);

    const char *parent_players =
        TMP("legacy_recovery_parent_symlink_players");
    const char *parent_root =
        TMP("legacy_recovery_parent_symlink_root");
    WORLD_HEAP parent_world = calloc(1, sizeof(*parent_world));
    ASSERT(parent_world != NULL);
    fill_token(token, 43);
    ASSERT(recovery_prepare_legacy_player(
        parent_world, parent_players, token, pubkey, secret,
        53.0f, 15.0f, 2, 3));
    char legacy_dir[512];
    char legacy_real[512];
    snprintf(legacy_dir, sizeof(legacy_dir),
             "%s/legacy", parent_players);
    snprintf(legacy_real, sizeof(legacy_real),
             "%s/legacy-real", parent_players);
    ASSERT(rename(legacy_dir, legacy_real) == 0);
    ASSERT(symlink("legacy-real", legacy_dir) == 0);
    ASSERT_EQ_INT(legacy_recovery_source_probe(
                      parent_root, parent_players,
                      token, pubkey),
                  LEGACY_RECOVERY_SOURCE_INVALID);
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(parent_world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    ASSERT_EQ_INT(legacy_recovery_execute(
                      parent_world, 0, 4,
                      parent_root, parent_players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE,
                      NULL),
                  LEGACY_RECOVERY_RESULT_INVALID_SOURCE);
    ASSERT(parent_world->players[0].last_signed_nonce == 3);
    signal_authoritative_state_digest(parent_world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
}
#endif

#if defined(SIGNAL_SAVE_TESTING)
TEST(test_legacy_recovery_source_swap_is_digest_bound_and_inert) {
    const char *players =
        TMP("legacy_recovery_source_swap_players");
    const char *root =
        TMP("legacy_recovery_source_swap_root");
    uint8_t token[8];
    uint8_t replacement_token[8];
    fill_token(token, 44);
    fill_token(replacement_token, 45);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    WORLD_HEAP replacement_world =
        calloc(1, sizeof(*replacement_world));
    ASSERT(world != NULL);
    ASSERT(replacement_world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    uint8_t replacement_pubkey[32];
    uint8_t replacement_secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        52.0f, 19.0f, 50, 20));
    ASSERT(recovery_prepare_legacy_player(
        replacement_world, players, replacement_token,
        replacement_pubkey, replacement_secret,
        21.0f, 20.0f, 1, 2));
    char replacement_path[512];
    ASSERT(recovery_source_path(
        replacement_path, sizeof(replacement_path),
        players, replacement_token));
    recovery_race_hook_state_t hook = {
        .replacement_path = replacement_path,
    };
    persistence_recovery_test_set_before_source_bind_hook(
        recovery_source_swap_test_hook, &hook);

    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    legacy_recovery_result_status_t status =
        legacy_recovery_execute(
            world, 0, 21, root, players, save_slots,
            PERSISTENCE_GENERATION_FAULT_NONE, NULL);
    persistence_recovery_test_reset_hooks();
    ASSERT(hook.completed);
    ASSERT_EQ_INT(status, LEGACY_RECOVERY_RESULT_INVALID_SOURCE);
    ASSERT(world->players[0].last_signed_nonce == 20);
    signal_authoritative_state_digest(world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_NONE);
}

TEST(test_legacy_recovery_racing_destination_wins_no_replace) {
    const char *players =
        TMP("legacy_recovery_destination_race_players");
    const char *root =
        TMP("legacy_recovery_destination_race_root");
    uint8_t token[8];
    fill_token(token, 46);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        49.0f, 22.0f, 60, 23));
    recovery_race_hook_state_t hook = {
        .marker = 0xD3,
    };
    persistence_recovery_test_set_before_destination_publish_hook(
        recovery_destination_race_test_hook, &hook);

    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    legacy_recovery_result_status_t status =
        legacy_recovery_execute(
            world, 0, 24, root, players, save_slots,
            PERSISTENCE_GENERATION_FAULT_NONE, NULL);
    persistence_recovery_test_reset_hooks();
    ASSERT(hook.completed);
    ASSERT_EQ_INT(
        status, LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT);
    ASSERT(world->players[0].last_signed_nonce == 23);
    signal_authoritative_state_digest(world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    ASSERT(file_is_marker_block(
        hook.observed_destination, 0xD3));
    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_NONE);
}

TEST(test_legacy_recovery_late_selected_destination_blocks_publish) {
    const char *players =
        TMP("legacy_recovery_selected_destination_race_players");
    const char *root =
        TMP("legacy_recovery_selected_destination_race_root");
    uint8_t token[8];
    fill_token(token, 52);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        46.0f, 25.0f, 84, 29));
    char selected_destination[512];
    ASSERT(recovery_destination_path(
        selected_destination, sizeof(selected_destination),
        players, pubkey));
    recovery_race_hook_state_t hook = {
        .replacement_path = selected_destination,
        .marker = 0xE4,
    };
    persistence_recovery_test_set_before_destination_publish_hook(
        recovery_selected_destination_race_test_hook, &hook);

    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    legacy_recovery_result_status_t status =
        legacy_recovery_execute(
            world, 0, 30, root, players, save_slots,
            PERSISTENCE_GENERATION_FAULT_NONE, NULL);
    persistence_recovery_test_reset_hooks();
    ASSERT(hook.completed);
    ASSERT_EQ_INT(
        status, LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT);
    ASSERT(file_is_marker_block(selected_destination, 0xE4));
    ASSERT(world->players[0].last_signed_nonce == 29);
    signal_authoritative_state_digest(world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_NONE);
}

#ifndef _WIN32
TEST(test_legacy_recovery_rejects_racing_destination_parent_symlink) {
    const char *players =
        TMP("legacy_recovery_destination_parent_players");
    const char *root =
        TMP("legacy_recovery_destination_parent_root");
    const char *attacker =
        TMP("legacy_recovery_destination_parent_attacker");
    mkdir_p(attacker);
    uint8_t token[8];
    fill_token(token, 48);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        45.0f, 26.0f, 80, 27));
    recovery_race_hook_state_t hook = {
        .replacement_path = attacker,
    };
    persistence_recovery_test_set_before_destination_publish_hook(
        recovery_destination_parent_symlink_test_hook, &hook);

    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    legacy_recovery_result_status_t status =
        legacy_recovery_execute(
            world, 0, 28, root, players, save_slots,
            PERSISTENCE_GENERATION_FAULT_NONE, NULL);
    persistence_recovery_test_reset_hooks();
    ASSERT(hook.completed);
    ASSERT_EQ_INT(
        status, LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE);
    ASSERT(world->players[0].last_signed_nonce == 27);
    signal_authoritative_state_digest(world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    const char *leaf = strrchr(
        hook.observed_destination, '/');
    ASSERT(leaf != NULL);
    char escaped[512];
    int n = snprintf(
        escaped, sizeof(escaped), "%s/%s",
        attacker, leaf + 1);
    ASSERT(n > 0 && (size_t)n < sizeof(escaped));
    ASSERT(!file_exists(escaped));
    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_NONE);
}
#endif
#endif

TEST(test_legacy_recovery_refuses_unattributable_world_ownership) {
    const char *players =
        TMP("legacy_recovery_quarantine_players");
    const char *root =
        TMP("legacy_recovery_quarantine_root");
    uint8_t token[8];
    fill_token(token, 47);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        47.0f, 24.0f, 70, 25));
    ownership_quarantine_entry_t row = {
        .record_id = 1,
        .source_kind = OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
        .reason =
            OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN,
        .station_index = OWNERSHIP_QUARANTINE_NA,
        .row_index = 0,
        .legacy_actor_code = 0,
    };
    ASSERT(ownership_quarantine_add(
        &world->ownership_quarantine, &row));
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    signal_authoritative_state_digest(world, before);
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    ASSERT_EQ_INT(legacy_recovery_execute(
                      world, 0, 26, root, players,
                      save_slots,
                      PERSISTENCE_GENERATION_FAULT_NONE,
                      NULL),
                  LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE);
    ASSERT(world->players[0].last_signed_nonce == 25);
    signal_authoritative_state_digest(world, after);
    ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_NONE);
}

TEST(test_legacy_recovery_faults_preserve_selected_and_live_state) {
    static const persistence_generation_fault_t faults[] = {
        PERSISTENCE_GENERATION_FAULT_AFTER_ARTIFACTS,
        PERSISTENCE_GENERATION_FAULT_AFTER_MANIFEST,
        PERSISTENCE_GENERATION_FAULT_BEFORE_POINTER_PUBLISH,
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        char player_name[64];
        char root_name[64];
        snprintf(player_name, sizeof(player_name),
                 "legacy_recovery_fault_players_%u",
                 (unsigned)i);
        snprintf(root_name, sizeof(root_name),
                 "legacy_recovery_fault_root_%u",
                 (unsigned)i);
        const char *players = TMP(player_name);
        const char *root = TMP(root_name);
        uint8_t token[8];
        fill_token(token, (uint8_t)(30 + i));
        WORLD_HEAP world = calloc(1, sizeof(*world));
        ASSERT(world != NULL);
        uint8_t pubkey[32];
        uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
        ASSERT(recovery_prepare_legacy_player(
            world, players, token, pubkey, secret,
            51.0f + (float)i, 15.0f, 20, 21));
        uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
        uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];
        signal_authoritative_state_digest(world, before);
        bool save_slots[MAX_PLAYERS] = {0};
        save_slots[0] = true;
        ASSERT_EQ_INT(legacy_recovery_execute(
                          world, 0, 22,
                          root, players, save_slots,
                          faults[i], NULL),
                      LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE);
        ASSERT(world->players[0].last_signed_nonce == 21);
        signal_authoritative_state_digest(world, after);
        ASSERT_EQ_INT(memcmp(before, after, sizeof(before)), 0);
        ASSERT_EQ_FLOAT(world->players[0].ship->hull, 15.0f, 0.001f);
        persistence_generation_paths_t absent = {0};
        ASSERT_EQ_INT(persistence_generation_resolve(root, &absent),
                      PERSISTENCE_GENERATION_NONE);
        char source[512];
        ASSERT(recovery_source_path(
            source, sizeof(source), players, token));
        ASSERT(file_exists(source));
    }
}

TEST(test_legacy_recovery_adopts_visible_pointer_after_sync_failure) {
    const char *players =
        TMP("legacy_recovery_pointer_sync_players");
    const char *root =
        TMP("legacy_recovery_pointer_sync_root");
    uint8_t token[8];
    fill_token(token, 34);
    WORLD_HEAP world = calloc(1, sizeof(*world));
    ASSERT(world != NULL);
    uint8_t pubkey[32];
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES];
    ASSERT(recovery_prepare_legacy_player(
        world, players, token, pubkey, secret,
        48.0f, 16.0f, 30, 31));
    bool save_slots[MAX_PLAYERS] = {0};
    save_slots[0] = true;
    persistence_generation_paths_t published = {0};
    ASSERT_EQ_INT(legacy_recovery_execute(
                      world, 0, 32,
                      root, players, save_slots,
                      PERSISTENCE_GENERATION_FAULT_POINTER_DIR_SYNC_FAILURE,
                      &published),
                  LEGACY_RECOVERY_RESULT_SUCCESS);
    persistence_generation_paths_t selected = {0};
    ASSERT_EQ_INT(persistence_generation_resolve(root, &selected),
                  PERSISTENCE_GENERATION_CURRENT);
    ASSERT(selected.generation == published.generation);
    ASSERT_EQ_FLOAT(world->players[0].ship->hull, 48.0f, 0.001f);
}

TEST(test_legacy_recovery_audit_is_bearer_free_and_no_follow) {
    const char *root = TMP("legacy_recovery_audit");
    mkdir_p(root);
    ASSERT(legacy_recovery_audit_append(
        root, LEGACY_RECOVERY_RESULT_STALE_OFFER));
    ASSERT(legacy_recovery_audit_append(
        root, LEGACY_RECOVERY_RESULT_SUCCESS));
    char path[512];
    snprintf(path, sizeof(path), "%s/legacy-recovery.audit", root);
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    char contents[256] = {0};
    size_t count = fread(contents, 1, sizeof(contents) - 1u, f);
    ASSERT(fclose(f) == 0);
    contents[count] = '\0';
    ASSERT_STR_EQ(contents,
                  "v1 status=stale-offer\n"
                  "v1 status=success\n");

#ifndef _WIN32
    ASSERT(remove(path) == 0);
    char target[512];
    snprintf(target, sizeof(target), "%s/audit-target", root);
    ASSERT(write_marker_file(target, 0x41));
    ASSERT(symlink(target, path) == 0);
    ASSERT(!legacy_recovery_audit_append(
        root, LEGACY_RECOVERY_RESULT_REPLAY));
#endif
}

void register_save_keyed_by_pubkey_tests(void);
void register_save_keyed_by_pubkey_tests(void) {
    TEST_SECTION("\nSave identity and retired legacy claim boundary:\n");
    RUN(test_save_keyed_by_pubkey_roundtrip);
    RUN(test_save_legacy_claim_wire_value_is_semantically_disabled);
    RUN(test_save_legacy_claim_token_ab_and_name_inputs_are_inert);
    RUN(test_pubkey_proof_is_session_and_challenge_bound);
    RUN(test_pubkey_persistence_gate_requires_verified_proof);
    RUN(test_save_anonymous_fallback_legacy_path);
    RUN(test_legacy_recovery_offer_is_proof_connection_and_expiry_bound);
    RUN(test_legacy_recovery_expiry_releases_global_pause_until_reconnect);
    RUN(test_legacy_recovery_expiry_abort_is_generation_byte_safe);
    RUN(test_legacy_recovery_offer_wrong_input_consumes_and_reuse_is_replay);
    RUN(test_legacy_recovery_offer_entropy_failure_clears_state);
    RUN(test_legacy_recovery_success_publishes_atomic_pubkey_save);
    RUN(test_legacy_recovery_consumption_fences_source_bearing_fallback);
    RUN(test_legacy_recovery_accepts_bounded_ply4_through_ply7);
    RUN(test_legacy_recovery_rejects_unbounded_ply1_through_ply3);
    RUN(test_legacy_recovery_destination_conflict_and_corruption_are_inert);
#ifndef _WIN32
    RUN(test_legacy_recovery_rejects_source_symlink_and_hardlink);
#endif
#if defined(SIGNAL_SAVE_TESTING)
    RUN(test_legacy_recovery_source_swap_is_digest_bound_and_inert);
    RUN(test_legacy_recovery_racing_destination_wins_no_replace);
    RUN(test_legacy_recovery_late_selected_destination_blocks_publish);
#ifndef _WIN32
    RUN(test_legacy_recovery_rejects_racing_destination_parent_symlink);
#endif
#endif
    RUN(test_legacy_recovery_refuses_unattributable_world_ownership);
    RUN(test_legacy_recovery_faults_preserve_selected_and_live_state);
    RUN(test_legacy_recovery_adopts_visible_pointer_after_sync_failure);
    RUN(test_legacy_recovery_audit_is_bearer_free_and_no_follow);
}
