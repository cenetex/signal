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

_Static_assert(sizeof(station_authority_record_t) == 36,
               "public authority registry record must remain fixed-width");
_Static_assert((int)STATION_AUTHORITY_LIFECYCLE_CURRENT ==
                   (int)CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT,
               "station current lifecycle must match receipt lifecycle");
_Static_assert((int)STATION_AUTHORITY_LIFECYCLE_ROTATED ==
                   (int)CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED,
               "station rotated lifecycle must match receipt lifecycle");
_Static_assert((int)STATION_AUTHORITY_LIFECYCLE_REVOKED ==
                   (int)CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED,
               "station revoked lifecycle must match receipt lifecycle");
_Static_assert((int)STATION_AUTHORITY_TRUST_CURRENT ==
                   (int)CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT,
               "station current trust must match receipt trust");
_Static_assert((int)STATION_AUTHORITY_TRUST_ROTATED ==
                   (int)CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED,
               "station rotated trust must match receipt trust");
_Static_assert((int)STATION_AUTHORITY_TRUST_UNTRUSTED ==
                   (int)CARGO_RECEIPT_AUTHORITY_UNTRUSTED,
               "station untrusted state must match receipt trust");
_Static_assert((int)STATION_AUTHORITY_TRUST_REVOKED ==
                   (int)CARGO_RECEIPT_AUTHORITY_REVOKED,
               "station revoked state must match receipt trust");

/* Domain-separation strings. Bumping these (e.g. "-v2") would
 * invalidate every previously-derived station pubkey, so keep them
 * frozen unless a deliberate identity migration is in flight. */
static const char STATION_SEED_DOMAIN[]  = "signal-station-v1";
static const char OUTPOST_SEED_DOMAIN[]  = "signal-outpost-v1";
static const char SECRET_SEED_DOMAIN[]   = "signal-station-operator-secret-v1";
static const char DEV_SECRET[]           = "signal-dev-station-authority-secret";

#define STATION_AUTH_NAME_HASH_LEN 16

static uint8_t station_auth_secret_root[32];
static bool station_auth_secret_ready = false;

static bool station_authority_pubkey_is_zero(const uint8_t pubkey[32]) {
    static const uint8_t zero[32] = {0};
    return !pubkey || memcmp(pubkey, zero, sizeof(zero)) == 0;
}

static void station_authority_record_clear(station_authority_record_t *record) {
    if (record) memset(record, 0, sizeof(*record));
}

static bool station_authority_record_is_zero(
    const station_authority_record_t *record) {
    static const station_authority_record_t zero = {0};
    return record && memcmp(record, &zero, sizeof(zero)) == 0;
}

static int station_authority_registry_find(const station_t *s,
                                           const uint8_t pubkey[32]) {
    if (!s || !pubkey ||
        s->authority_registry_count > STATION_AUTHORITY_REGISTRY_CAP) {
        return -1;
    }
    for (uint8_t i = 0; i < s->authority_registry_count; i++) {
        if (memcmp(s->authority_registry[i].pubkey, pubkey, 32) == 0)
            return (int)i;
    }
    return -1;
}

void station_authority_registry_init(station_t *s) {
    if (!s) return;
    s->authority_registry_version = 0;
    s->authority_registry_count = 0;
    memset(s->authority_registry_pad, 0,
           sizeof(s->authority_registry_pad));
    memset(s->authority_registry, 0, sizeof(s->authority_registry));
    if (station_authority_pubkey_is_zero(s->station_pubkey)) return;
    s->authority_registry_version = STATION_AUTHORITY_REGISTRY_VERSION;
    memcpy(s->authority_registry[0].pubkey, s->station_pubkey, 32);
    s->authority_registry[0].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_CURRENT;
    s->authority_registry[0].trust = STATION_AUTHORITY_TRUST_CURRENT;
    s->authority_registry_count = 1;
}

static bool station_authority_record_state_valid(
    const station_authority_record_t *record,
    uint8_t index) {
    if (!record) return false;
    if (index == 0) {
        return record->lifecycle == STATION_AUTHORITY_LIFECYCLE_CURRENT &&
               record->trust == STATION_AUTHORITY_TRUST_CURRENT;
    }
    switch ((station_authority_trust_state_t)record->trust) {
        case STATION_AUTHORITY_TRUST_ROTATED:
            return record->lifecycle ==
                   STATION_AUTHORITY_LIFECYCLE_ROTATED;
        case STATION_AUTHORITY_TRUST_UNTRUSTED:
            return record->lifecycle ==
                       STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED ||
                   record->lifecycle ==
                       STATION_AUTHORITY_LIFECYCLE_ROTATED;
        case STATION_AUTHORITY_TRUST_REVOKED:
            return record->lifecycle ==
                       STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED ||
                   record->lifecycle ==
                       STATION_AUTHORITY_LIFECYCLE_REVOKED;
        default:
            return false;
    }
}

bool station_authority_registry_validate(const station_t *s) {
    if (!s) return false;
    bool station_empty =
        station_authority_pubkey_is_zero(s->station_pubkey);
    if (station_empty) {
        if (s->authority_registry_version != 0 ||
            s->authority_registry_count != 0) {
            return false;
        }
    } else if (s->authority_registry_version !=
                   STATION_AUTHORITY_REGISTRY_VERSION ||
               s->authority_registry_count == 0 ||
               s->authority_registry_count >
                   STATION_AUTHORITY_REGISTRY_CAP) {
        return false;
    }

    for (size_t i = 0; i < sizeof(s->authority_registry_pad); i++) {
        if (s->authority_registry_pad[i] != 0) return false;
    }
    if (station_empty) {
        for (uint8_t i = 0; i < STATION_AUTHORITY_REGISTRY_CAP; i++) {
            if (!station_authority_record_is_zero(
                    &s->authority_registry[i])) {
                return false;
            }
        }
        return true;
    }

    if (memcmp(s->authority_registry[0].pubkey,
               s->station_pubkey, 32) != 0) {
        return false;
    }
    for (uint8_t i = 0; i < s->authority_registry_count; i++) {
        const station_authority_record_t *record =
            &s->authority_registry[i];
        if (station_authority_pubkey_is_zero(record->pubkey) ||
            !station_authority_record_state_valid(record, i)) {
            return false;
        }
        for (size_t p = 0; p < sizeof(record->_pad); p++) {
            if (record->_pad[p] != 0) return false;
        }
        for (uint8_t prior = 0; prior < i; prior++) {
            if (memcmp(s->authority_registry[prior].pubkey,
                       record->pubkey, 32) == 0) {
                return false;
            }
        }
    }
    for (uint8_t i = s->authority_registry_count;
         i < STATION_AUTHORITY_REGISTRY_CAP; i++) {
        if (!station_authority_record_is_zero(
                &s->authority_registry[i])) {
            return false;
        }
    }
    return true;
}

cargo_receipt_authority_trust_t station_authority_trust_for_pubkey(
    const station_t *s,
    const uint8_t pubkey[32]) {
    if (!s || station_authority_pubkey_is_zero(pubkey))
        return CARGO_RECEIPT_AUTHORITY_UNKNOWN;
    if (!station_authority_registry_validate(s))
        return CARGO_RECEIPT_AUTHORITY_REVOKED;
    int found = station_authority_registry_find(s, pubkey);
    if (found < 0) return CARGO_RECEIPT_AUTHORITY_UNKNOWN;
    return (cargo_receipt_authority_trust_t)
        s->authority_registry[found].trust;
}

cargo_receipt_authority_lifecycle_t
station_authority_lifecycle_for_pubkey(
    const station_t *s,
    const uint8_t pubkey[32]) {
    if (!s || station_authority_pubkey_is_zero(pubkey))
        return CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED;
    if (!station_authority_registry_validate(s))
        return CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED;
    int found = station_authority_registry_find(s, pubkey);
    if (found < 0)
        return CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED;
    return (cargo_receipt_authority_lifecycle_t)
        s->authority_registry[found].lifecycle;
}

static void station_authority_registry_remove(station_t *s, int index) {
    if (!s || index < 0 ||
        index >= (int)s->authority_registry_count) {
        return;
    }
    for (int i = index + 1; i < (int)s->authority_registry_count; i++)
        s->authority_registry[i - 1] = s->authority_registry[i];
    s->authority_registry_count--;
    station_authority_record_clear(
        &s->authority_registry[s->authority_registry_count]);
}

static int station_authority_registry_oldest_trusted_rotated(
    const station_t *s) {
    if (!s) return -1;
    for (int i = (int)s->authority_registry_count - 1; i >= 1; i--) {
        if (s->authority_registry[i].lifecycle ==
                STATION_AUTHORITY_LIFECYCLE_ROTATED &&
            s->authority_registry[i].trust ==
                STATION_AUTHORITY_TRUST_ROTATED) {
            return i;
        }
    }
    return -1;
}

static bool station_authority_registry_rekey(
    station_t *s,
    const uint8_t new_pubkey[32]) {
    if (!station_authority_registry_validate(s) ||
        station_authority_pubkey_is_zero(new_pubkey)) {
        return false;
    }
    if (memcmp(s->station_pubkey, new_pubkey, 32) == 0)
        return true;

    int existing = station_authority_registry_find(s, new_pubkey);
    if (existing >= 0) {
        const station_authority_record_t *record =
            &s->authority_registry[existing];
        if (existing == 0 ||
            record->lifecycle != STATION_AUTHORITY_LIFECYCLE_ROTATED ||
            record->trust != STATION_AUTHORITY_TRUST_ROTATED) {
            return false;
        }
        station_authority_registry_remove(s, existing);
    }

    if (s->authority_registry_count >=
        STATION_AUTHORITY_REGISTRY_CAP) {
        int evict =
            station_authority_registry_oldest_trusted_rotated(s);
        if (evict < 0) return false;
        station_authority_registry_remove(s, evict);
    }

    for (int i = (int)s->authority_registry_count; i > 1; i--)
        s->authority_registry[i] = s->authority_registry[i - 1];
    s->authority_registry[1] = s->authority_registry[0];
    s->authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_ROTATED;
    s->authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    station_authority_record_clear(&s->authority_registry[0]);
    memcpy(s->authority_registry[0].pubkey, new_pubkey, 32);
    s->authority_registry[0].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_CURRENT;
    s->authority_registry[0].trust =
        STATION_AUTHORITY_TRUST_CURRENT;
    s->authority_registry_count++;

    /* Preserve the old key in row one before changing the live identity. */
    memcpy(s->station_pubkey, new_pubkey, 32);
    return true;
}

bool station_authority_registry_set_trust(
    station_t *s,
    const uint8_t pubkey[32],
    cargo_receipt_authority_trust_t trust) {
    if (!station_authority_registry_validate(s) ||
        station_authority_pubkey_is_zero(pubkey) ||
        (trust != CARGO_RECEIPT_AUTHORITY_UNTRUSTED &&
         trust != CARGO_RECEIPT_AUTHORITY_REVOKED)) {
        return false;
    }
    int found = station_authority_registry_find(s, pubkey);
    if (found == 0) return false;
    if (found > 0) {
        station_authority_record_t *record =
            &s->authority_registry[found];
        if (record->trust == STATION_AUTHORITY_TRUST_REVOKED)
            return trust == CARGO_RECEIPT_AUTHORITY_REVOKED;
        if (record->trust == STATION_AUTHORITY_TRUST_UNTRUSTED &&
            trust == CARGO_RECEIPT_AUTHORITY_UNTRUSTED) {
            return true;
        }
        record->trust = (uint8_t)trust;
        if (trust == CARGO_RECEIPT_AUTHORITY_REVOKED &&
            record->lifecycle !=
                STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED) {
            record->lifecycle =
                STATION_AUTHORITY_LIFECYCLE_REVOKED;
        }
        return true;
    }

    int slot = (int)s->authority_registry_count;
    if (slot >= STATION_AUTHORITY_REGISTRY_CAP) {
        slot = station_authority_registry_oldest_trusted_rotated(s);
        if (slot < 0) return false;
    } else {
        s->authority_registry_count++;
    }
    station_authority_record_clear(&s->authority_registry[slot]);
    memcpy(s->authority_registry[slot].pubkey, pubkey, 32);
    s->authority_registry[slot].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED;
    s->authority_registry[slot].trust = (uint8_t)trust;
    return true;
}

static void station_authority_hash_secret(const char *secret,
                                          uint8_t out_root[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, SECRET_SEED_DOMAIN, sizeof(SECRET_SEED_DOMAIN) - 1);
    sha256_update(&c, secret, strlen(secret));
    sha256_final(&c, out_root);
}

bool station_authority_configure_secret(const char *secret) {
    if (!secret || secret[0] == '\0') return false;
    station_authority_hash_secret(secret, station_auth_secret_root);
    station_auth_secret_ready = true;
    return true;
}

void station_authority_use_dev_secret(void) {
    station_authority_hash_secret(DEV_SECRET, station_auth_secret_root);
    station_auth_secret_ready = true;
}

static const uint8_t *station_authority_secret_root(void) {
    if (!station_auth_secret_ready)
        station_authority_use_dev_secret();
    return station_auth_secret_root;
}

void station_authority_seeded_seed(uint32_t world_seed,
                                   uint32_t station_index,
                                   uint8_t out_seed[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, STATION_SEED_DOMAIN, sizeof(STATION_SEED_DOMAIN) - 1);
    sha256_update(&c, station_authority_secret_root(), 32);
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
    sha256_update(&c, station_authority_secret_root(), 32);
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
    station_authority_registry_init(s);
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
    station_authority_registry_init(s);
}

station_authority_rederive_result_t station_authority_rederive_secret(
    station_t *s,
    uint32_t world_seed,
    int station_index) {
    if (!s) return STATION_AUTHORITY_REDERIVE_REJECTED;
    uint8_t seed[32];
    if (station_index >= 0 && station_index < SIGNAL_SEEDED_STATION_COUNT) {
        station_authority_seeded_seed(world_seed,
                                       (uint32_t)station_index, seed);
    } else {
        /* Outpost — rederive from the saved founder / name / tick. */
        station_authority_outpost_seed(s->outpost_founder_pubkey,
                                        s->name,
                                        s->outpost_planted_tick, seed);
    }
    uint8_t derived_pub[32];
    uint8_t derived_secret[64];
    signal_crypto_keypair_from_seed(seed, derived_pub, derived_secret);
    /* If the saved pubkey is zero (pre-v40 save with no station
     * identity field), stamp the rederived pubkey so the station has a
     * usable identity. If a non-zero saved pubkey no longer matches the
     * configured operator secret, rotate it deliberately instead of
     * keeping a public key that cannot verify signatures from the
     * rederived private key. */
    bool saved_zero = station_authority_pubkey_is_zero(s->station_pubkey);
    if (saved_zero) {
        if (!station_authority_registry_validate(s))
            return STATION_AUTHORITY_REDERIVE_REJECTED;
        memcpy(s->station_pubkey, derived_pub, 32);
        memcpy(s->station_secret, derived_secret, 64);
        station_authority_registry_init(s);
        return STATION_AUTHORITY_REDERIVE_UNCHANGED;
    }

    /*
     * v76 and earlier synthesize this in the reader. Also accept the exact
     * all-zero legacy representation for focused callers, but reject partial
     * or malformed legacy-looking state.
     */
    if (s->authority_registry_version == 0 &&
        s->authority_registry_count == 0) {
        static const uint8_t zero_registry[
            sizeof(s->authority_registry)] = {0};
        static const uint8_t zero_pad[
            sizeof(s->authority_registry_pad)] = {0};
        if (memcmp(s->authority_registry, zero_registry,
                   sizeof(s->authority_registry)) == 0 &&
            memcmp(s->authority_registry_pad, zero_pad,
                   sizeof(s->authority_registry_pad)) == 0) {
            station_authority_registry_init(s);
        }
    }
    if (!station_authority_registry_validate(s))
        return STATION_AUTHORITY_REDERIVE_REJECTED;

    if (memcmp(s->station_pubkey, derived_pub, 32) == 0) {
        memcpy(s->station_secret, derived_secret, 64);
        return STATION_AUTHORITY_REDERIVE_UNCHANGED;
    }
    if (!station_authority_registry_rekey(s, derived_pub))
        return STATION_AUTHORITY_REDERIVE_REJECTED;
    memcpy(s->station_secret, derived_secret, 64);
    return STATION_AUTHORITY_REDERIVE_REKEYED;
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

void station_authority_init_outpost_keypair(station_t *s,
                                            const uint8_t founder_pub[32],
                                            const uint8_t nacl_secret[64]) {
    if (!s) return;
    if (founder_pub)
        memcpy(s->outpost_founder_pubkey, founder_pub, 32);
    else
        memset(s->outpost_founder_pubkey, 0, 32);
    /* nacl_secret format: seed[0..31] || pubkey[32..63] */
    memcpy(s->station_secret, nacl_secret, 64);
    memcpy(s->station_pubkey, nacl_secret + 32, 32);
    s->outpost_planted_tick = 0; /* imported keypair */
    station_authority_registry_init(s);
}
