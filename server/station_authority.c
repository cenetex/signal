/*
 * station_authority.c -- Per-station Ed25519 identity.
 *
 * Layer B of #479. See station_authority.h for the high-level scheme.
 */
#include "station_authority.h"

#include <assert.h>
#include <string.h>

#include "base58.h"
#include "sha256.h"
#include "signal_crypto.h"

/* Domain-separation strings. Bumping these (e.g. "-v2") would
 * invalidate every previously-derived station pubkey, so keep them
 * frozen unless a deliberate identity migration is in flight. */
static const char STATION_SEED_DOMAIN[]  = "signal-station-v1";
static const char OUTPOST_SEED_DOMAIN[]  = "signal-outpost-v1";

#define STATION_AUTH_NAME_HASH_LEN 16

void station_authority_seeded_seed(uint32_t world_seed,
                                   uint32_t station_index,
                                   uint8_t out_seed[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, STATION_SEED_DOMAIN, sizeof(STATION_SEED_DOMAIN) - 1);
    /* Little-endian fixed-width — same byte order on every host so
     * the derivation is deterministic across architectures. */
    uint8_t seed_le[4];
    seed_le[0] = (uint8_t)(world_seed & 0xFFu);
    seed_le[1] = (uint8_t)((world_seed >> 8) & 0xFFu);
    seed_le[2] = (uint8_t)((world_seed >> 16) & 0xFFu);
    seed_le[3] = (uint8_t)((world_seed >> 24) & 0xFFu);
    sha256_update(&c, seed_le, 4);
    uint8_t idx_le[4];
    idx_le[0] = (uint8_t)(station_index & 0xFFu);
    idx_le[1] = (uint8_t)((station_index >> 8) & 0xFFu);
    idx_le[2] = (uint8_t)((station_index >> 16) & 0xFFu);
    idx_le[3] = (uint8_t)((station_index >> 24) & 0xFFu);
    sha256_update(&c, idx_le, 4);
    sha256_final(&c, out_seed);
}

void station_authority_outpost_seed(const uint8_t founder_pub[32],
                                    const char *station_name,
                                    uint64_t planted_tick,
                                    uint8_t out_seed[32]) {
    /* Pad the name to a fixed STATION_AUTH_NAME_HASH_LEN bytes so the
     * hash input is always the same length regardless of name length. */
    uint8_t name_buf[STATION_AUTH_NAME_HASH_LEN];
    memset(name_buf, 0, sizeof(name_buf));
    if (station_name) {
        size_t n = strlen(station_name);
        if (n > sizeof(name_buf)) n = sizeof(name_buf);
        memcpy(name_buf, station_name, n);
    }
    /* 64-bit tick, little-endian — see seeded variant for rationale. */
    uint8_t tick_le[8];
    for (int i = 0; i < 8; i++)
        tick_le[i] = (uint8_t)((planted_tick >> (i * 8)) & 0xFFu);

    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, OUTPOST_SEED_DOMAIN, sizeof(OUTPOST_SEED_DOMAIN) - 1);
    static const uint8_t zero_pub[32] = {0};
    sha256_update(&c, founder_pub ? founder_pub : zero_pub, 32);
    sha256_update(&c, name_buf, sizeof(name_buf));
    sha256_update(&c, tick_le, sizeof(tick_le));
    sha256_final(&c, out_seed);
}

void station_authority_init_seeded(station_t *s,
                                   uint32_t world_seed,
                                   uint32_t station_index) {
    if (!s) return;
    uint8_t seed[32];
    station_authority_seeded_seed(world_seed, station_index, seed);
    signal_crypto_keypair_from_seed(seed, s->station_pubkey, s->station_secret);
    /* Seeded stations have no founder / planted_tick provenance. */
    memset(s->outpost_founder_pubkey, 0, sizeof(s->outpost_founder_pubkey));
    s->outpost_planted_tick = 0;
}

void station_authority_init_outpost(station_t *s,
                                    const uint8_t founder_pub[32],
                                    uint64_t planted_tick) {
    if (!s) return;
    if (founder_pub)
        memcpy(s->outpost_founder_pubkey, founder_pub, 32);
    else
        memset(s->outpost_founder_pubkey, 0, 32);
    s->outpost_planted_tick = planted_tick;
    uint8_t seed[32];
    station_authority_outpost_seed(s->outpost_founder_pubkey, s->name,
                                    planted_tick, seed);
    signal_crypto_keypair_from_seed(seed, s->station_pubkey, s->station_secret);
}

void station_authority_rederive_secret(station_t *s,
                                       uint32_t world_seed,
                                       int station_index) {
    if (!s) return;
    uint8_t seed[32];
    if (station_index >= 0 && station_index < 3) {
        station_authority_seeded_seed(world_seed,
                                       (uint32_t)station_index, seed);
    } else {
        /* Outpost — rederive from the saved founder / name / tick. */
        station_authority_outpost_seed(s->outpost_founder_pubkey,
                                        s->name,
                                        s->outpost_planted_tick, seed);
    }
    uint8_t derived_pub[32];
    signal_crypto_keypair_from_seed(seed, derived_pub, s->station_secret);
    /* If the saved pubkey is zero (pre-v40 save with no station
     * identity field), stamp the rederived pubkey so the station
     * has a usable identity. If a pubkey was loaded from disk,
     * keep it as authoritative — for v40+ saves it's exactly
     * `derived_pub`; for v40 saves where `s->name` was lost (e.g.
     * catalog-less load) the saved pubkey is the canonical record
     * and the secret we just derived should pair with it. */
    static const uint8_t zero_pub[32] = {0};
    if (memcmp(s->station_pubkey, zero_pub, 32) == 0) {
        memcpy(s->station_pubkey, derived_pub, 32);
    }
}

void station_sign(const station_t *s, const uint8_t *msg, size_t len,
                  uint8_t sig[64]) {
    assert(s && sig);
    /* Defensive: a station with all-zero secret is uninitialized — sign
     * with zeros anyway, but the resulting signature won't verify (the
     * pubkey won't match the implied private key). Tests catch this. */
    signal_crypto_sign(sig, msg, len, s->station_secret);
}

bool station_verify(const station_t *s, const uint8_t *msg, size_t len,
                    const uint8_t sig[64]) {
    if (!s || !sig) return false;
    return signal_crypto_verify(sig, msg, len, s->station_pubkey);
}

void station_pubkey_b58_prefix(const station_t *s, char out[16]) {
    if (!s || !out) {
        if (out) out[0] = '\0';
        return;
    }
    /* Encode the leading 8 bytes of the pubkey — base58 of 8 bytes is
     * <= 12 chars, fits in 16 with a NUL. Short prefix is plenty for
     * visual confirmation in HUD / logs. */
    char tmp[20];
    size_t n = base58_encode(s->station_pubkey, 8, tmp, sizeof(tmp));
    if (n >= 16) n = 15;
    memcpy(out, tmp, n);
    out[n] = '\0';
}
