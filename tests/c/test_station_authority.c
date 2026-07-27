/*
 * test_station_authority.c -- Tests for per-station Ed25519 identity.
 *
 * Layer B of #479. Covers operator-secret-derived determinism, outpost
 * derivation, sign/verify round trips, save/load secret rederivation,
 * and the wire-format omission discipline (station_secret never on
 * the wire, never on disk).
 */
#include "test_harness.h"

#include "cargo_receipt_issue.h"
#include "cargo_receipt_trust.h"
#include "persistence_io.h"
#include "station_authority.h"
#include "signal_crypto.h"
#include "net_protocol.h"

TEST(test_station_authority_seeded_determinism) {
    /* Two world_resets with the same seed must produce identical
     * pubkeys for indices 0/1/2. */
    WORLD_HEAP w1 = calloc(1, sizeof(world_t));
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w1 && w2);
    w1->rng = 4242u;
    w2->rng = 4242u;
    world_reset(w1);
    world_reset(w2);
    for (int i = 0; i < 3; i++) {
        ASSERT(memcmp(w1->stations[i].station_pubkey,
                      w2->stations[i].station_pubkey, 32) == 0);
        /* And the pubkey must be non-zero — i.e. actually derived. */
        uint8_t zero[32] = {0};
        ASSERT(memcmp(w1->stations[i].station_pubkey, zero, 32) != 0);
    }
}

TEST(test_station_authority_seeded_distinct_seeds) {
    /* Different world seeds produce distinct pubkeys per station, and
     * within a world the three seeded stations have distinct pubkeys. */
    WORLD_HEAP w1 = calloc(1, sizeof(world_t));
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w1 && w2);
    w1->rng = 1111u;
    w2->rng = 9999u;
    world_reset(w1);
    world_reset(w2);
    /* Distinct across seeds */
    for (int i = 0; i < 3; i++) {
        ASSERT(memcmp(w1->stations[i].station_pubkey,
                      w2->stations[i].station_pubkey, 32) != 0);
    }
    /* Distinct across station indices within one world */
    ASSERT(memcmp(w1->stations[0].station_pubkey,
                  w1->stations[1].station_pubkey, 32) != 0);
    ASSERT(memcmp(w1->stations[1].station_pubkey,
                  w1->stations[2].station_pubkey, 32) != 0);
    ASSERT(memcmp(w1->stations[0].station_pubkey,
                  w1->stations[2].station_pubkey, 32) != 0);
}

TEST(test_station_authority_operator_secret_affects_pubkey) {
    /* Public world data alone must not be enough to reproduce station
     * signing keys. Same world seed + station index under different
     * operator secrets should produce different pubkeys. */
    station_authority_configure_secret("operator-secret-alpha");
    WORLD_HEAP w1 = calloc(1, sizeof(world_t));
    ASSERT(w1);
    w1->rng = 424242u;
    world_reset(w1);

    station_authority_configure_secret("operator-secret-beta");
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2);
    w2->rng = 424242u;
    world_reset(w2);

    ASSERT(memcmp(w1->stations[0].station_pubkey,
                  w2->stations[0].station_pubkey, 32) != 0);
    station_authority_use_dev_secret();
}

TEST(test_station_authority_outpost_derivation) {
    /* Place an outpost (manually constructed to avoid driving the full
     * scaffold-tow flow) and assert the pubkey matches an independent
     * recomputation from the same (founder, name, tick) triple. */
    station_t st;
    memset(&st, 0, sizeof(st));
    snprintf(st.name, sizeof(st.name), "Outpost Alpha");
    uint8_t founder[32];
    for (int i = 0; i < 32; i++) founder[i] = (uint8_t)(0x10 + i);
    uint64_t tick = 1234567ULL;

    station_authority_init_outpost(&st, founder, tick);

    /* Independent recomputation. */
    uint8_t expected_seed[32];
    station_authority_outpost_seed(founder, "Outpost Alpha", tick, expected_seed);
    uint8_t expected_pub[32];
    uint8_t expected_secret[64];
    signal_crypto_keypair_from_seed(expected_seed, expected_pub, expected_secret);

    ASSERT(memcmp(st.station_pubkey, expected_pub, 32) == 0);
    /* Provenance fields stamped for save/load rederivation. */
    ASSERT(memcmp(st.outpost_founder_pubkey, founder, 32) == 0);
    ASSERT_EQ_INT((int)st.outpost_planted_tick, (int)tick);
}

TEST(test_station_authority_sign_verify_roundtrip) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    w->rng = 2037u;
    world_reset(w);

    const uint8_t msg[] = "prospect refinery says hello";
    size_t len = sizeof(msg) - 1;
    uint8_t sig[64];
    station_sign(&w->stations[0], msg, len, sig);

    /* Valid signature verifies. */
    ASSERT(station_verify(&w->stations[0], msg, len, sig));
    /* Wrong station's pubkey must reject. */
    ASSERT(!station_verify(&w->stations[1], msg, len, sig));
    /* Tampered message fails. */
    uint8_t tampered_msg[sizeof(msg)];
    memcpy(tampered_msg, msg, sizeof(msg));
    tampered_msg[0] ^= 0x01;
    ASSERT(!station_verify(&w->stations[0], tampered_msg, len, sig));
    /* Tampered sig fails. */
    uint8_t tampered_sig[64];
    memcpy(tampered_sig, sig, 64);
    tampered_sig[0] ^= 0x80;
    ASSERT(!station_verify(&w->stations[0], msg, len, tampered_sig));
}

TEST(test_station_authority_save_load_rederives_secret) {
    /* Save a world, zero the in-memory secret, load it back, and
     * assert the loaded station can sign correctly — proving the
     * world loader rederived the private key from the world seed
     * without ever reading it from disk. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    w->rng = 7777u;
    world_reset(w);
    uint8_t pub_before[32];
    memcpy(pub_before, w->stations[0].station_pubkey, 32);

    ASSERT(world_save(w, TMP("test_station_auth.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded);
    ASSERT(world_load(loaded, TMP("test_station_auth.sav")));

    /* Pubkey survived the roundtrip. */
    ASSERT(memcmp(loaded->stations[0].station_pubkey, pub_before, 32) == 0);
    /* Secret was wiped before save and rederived on load — verify it
     * actually works by signing and checking the sig. */
    const uint8_t msg[] = "post-load signing check";
    uint8_t sig[64];
    station_sign(&loaded->stations[0], msg, sizeof(msg) - 1, sig);
    ASSERT(station_verify(&loaded->stations[0], msg, sizeof(msg) - 1, sig));

    remove(TMP("test_station_auth.sav"));
}

TEST(test_station_authority_wire_omits_secret) {
    /* Serialize the wire-format station identity message and confirm
     * the 64-byte station_secret never appears anywhere in the
     * payload. Crude but effective — even if a future refactor
     * accidentally splatted the whole struct into the buffer, this
     * test catches it. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    w->rng = 555u;
    world_reset(w);

    uint8_t buf[STATION_IDENTITY_SIZE + 64] = {0};
    int n = serialize_station_identity(buf, 0, &w->stations[0]);
    ASSERT_EQ_INT(n, STATION_IDENTITY_SIZE);

    /* Search for any 64-byte run that matches station_secret. */
    const uint8_t *secret = w->stations[0].station_secret;
    bool found_secret = false;
    /* The first 32 bytes of the secret are the seed (private). The
     * last 32 bytes are the pubkey, which IS legitimately on the
     * wire — searching for the full 64-byte secret would always
     * miss. Search for the first 33 bytes instead: that includes
     * 1 byte of seed and 32 bytes of "after the seed", which is
     * unique enough to catch a wholesale struct splat. */
    for (int off = 0; off + 33 <= n; off++) {
        if (memcmp(&buf[off], secret, 33) == 0) {
            found_secret = true;
            break;
        }
    }
    ASSERT(!found_secret);

    /* Conversely, the pubkey MUST appear somewhere. */
    bool found_pub = false;
    for (int off = 0; off + 32 <= n; off++) {
        if (memcmp(&buf[off], w->stations[0].station_pubkey, 32) == 0) {
            found_pub = true;
            break;
        }
    }
    ASSERT(found_pub);
}

TEST(test_station_authority_outpost_save_load) {
    /* Manually plant an outpost via the helper (no need to drive the
     * full tow flow). Save, garble the in-memory secret, load, and
     * verify the outpost can still sign — proving outpost
     * rederivation reads the saved founder + name + tick. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    w->rng = 8181u;
    world_reset(w);
    /* Outposts live after the seeded relay roots plus Freeport. */
    station_t *out = &w->stations[SIGNAL_FIRST_OUTPOST_INDEX];
    snprintf(out->name, sizeof(out->name), "Outpost Beta");
    out->pos = v2(10000.0f, 0.0f);
    out->radius = 30.0f;
    out->dock_radius = 200.0f;
    out->signal_range = 8000.0f;
    out->id = w->next_station_id++;
    if (w->station_count <= SIGNAL_FIRST_OUTPOST_INDEX)
        w->station_count = SIGNAL_FIRST_OUTPOST_INDEX + 1;
    uint8_t founder[32];
    for (int i = 0; i < 32; i++) founder[i] = (uint8_t)(0xA0 + i);
    station_authority_init_outpost(out, founder, 9999ULL);
    uint8_t out_pub_before[32];
    memcpy(out_pub_before, out->station_pubkey, 32);

    ASSERT(world_save(w, TMP("test_outpost_auth.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded);
    ASSERT(world_load(loaded, TMP("test_outpost_auth.sav")));

    station_t *out_loaded = &loaded->stations[SIGNAL_FIRST_OUTPOST_INDEX];
    ASSERT(memcmp(out_loaded->station_pubkey, out_pub_before, 32) == 0);
    ASSERT(memcmp(out_loaded->outpost_founder_pubkey, founder, 32) == 0);
    ASSERT_EQ_INT((int)out_loaded->outpost_planted_tick, 9999);

    /* Sign with the rederived secret. */
    const uint8_t msg[] = "outpost beta speaks";
    uint8_t sig[64];
    station_sign(out_loaded, msg, sizeof(msg) - 1, sig);
    ASSERT(station_verify(out_loaded, msg, sizeof(msg) - 1, sig));

    remove(TMP("test_outpost_auth.sav"));
}

static bool authority_file_contains_span(const char *path,
                                         const uint8_t *needle,
                                         size_t needle_len) {
    if (!path || !needle || needle_len == 0) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    size_t len = (size_t)end;
    uint8_t *bytes = malloc(len > 0 ? len : 1);
    if (!bytes) {
        fclose(f);
        return false;
    }
    bool found = false;
    if (fread(bytes, 1, len, f) == len && needle_len <= len) {
        for (size_t i = 0; i + needle_len <= len; i++) {
            if (memcmp(&bytes[i], needle, needle_len) == 0) {
                found = true;
                break;
            }
        }
    }
    free(bytes);
    fclose(f);
    return found;
}

enum {
    AUTHORITY_REGISTRY_WIRE_SIZE =
        2 + 6 + STATION_AUTHORITY_REGISTRY_CAP * 36
};
_Static_assert(AUTHORITY_REGISTRY_WIRE_SIZE == 296,
               "v77 registry wire span must remain 296 bytes");

static size_t authority_registry_pack_for_save(
    const station_t *station,
    uint8_t out[AUTHORITY_REGISTRY_WIRE_SIZE]) {
    size_t off = 0;
    out[off++] = station->authority_registry_version;
    out[off++] = station->authority_registry_count;
    memcpy(&out[off], station->authority_registry_pad,
           sizeof(station->authority_registry_pad));
    off += sizeof(station->authority_registry_pad);
    for (int i = 0; i < STATION_AUTHORITY_REGISTRY_CAP; i++) {
        const station_authority_record_t *record =
            &station->authority_registry[i];
        memcpy(&out[off], record->pubkey, sizeof(record->pubkey));
        off += sizeof(record->pubkey);
        out[off++] = record->lifecycle;
        out[off++] = record->trust;
        memcpy(&out[off], record->_pad, sizeof(record->_pad));
        off += sizeof(record->_pad);
    }
    return off;
}

static size_t authority_find_span(const uint8_t *haystack,
                                  size_t haystack_len,
                                  size_t start,
                                  const uint8_t *needle,
                                  size_t needle_len) {
    if (!haystack || !needle || needle_len == 0 ||
        start > haystack_len || needle_len > haystack_len - start) {
        return SIZE_MAX;
    }
    for (size_t i = start; i + needle_len <= haystack_len; i++) {
        if (memcmp(&haystack[i], needle, needle_len) == 0)
            return i;
    }
    return SIZE_MAX;
}

/*
 * Construct a real v76 byte stream from a canonical v77 save by deleting only
 * the 296-byte registry span introduced in v77 for each of the 128 serialized
 * station slots, changing the header version, and recomputing the CRC trailer.
 * Every pre-v77 byte remains byte-for-byte in its original order, so loading
 * the result exercises the actual old on-disk offsets rather than an in-memory
 * zero-field shortcut.
 */
static bool authority_construct_v76_save(
    const world_t *world,
    const char *v77_path,
    const char *v76_path) {
    if (!world || !v77_path || !v76_path) return false;
    FILE *f = fopen(v77_path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long end = ftell(f);
    if (end < 8 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    size_t file_len = (size_t)end;
    uint8_t *v77 = malloc(file_len);
    if (!v77) {
        fclose(f);
        return false;
    }
    bool ok = fread(v77, 1, file_len, f) == file_len;
    fclose(f);
    if (!ok) {
        free(v77);
        return false;
    }

    const uint32_t crc_magic_expected = 0x43524332u;
    uint32_t crc_magic = 0;
    memcpy(&crc_magic, &v77[file_len - 8], sizeof(crc_magic));
    if (crc_magic != crc_magic_expected) {
        free(v77);
        return false;
    }
    size_t payload_len = file_len - 8;
    size_t registry_offsets[MAX_STATIONS];
    size_t cursor = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        uint8_t registry[AUTHORITY_REGISTRY_WIRE_SIZE];
        if (authority_registry_pack_for_save(
                &world->stations[i], registry) != sizeof(registry)) {
            free(v77);
            return false;
        }
        size_t found = authority_find_span(
            v77, payload_len, cursor, registry, sizeof(registry));
        if (found == SIZE_MAX) {
            free(v77);
            return false;
        }
        registry_offsets[i] = found;
        cursor = found + sizeof(registry);
    }

    size_t registry_bytes =
        (size_t)MAX_STATIONS * AUTHORITY_REGISTRY_WIRE_SIZE;
    if (payload_len < registry_bytes) {
        free(v77);
        return false;
    }
    size_t v76_payload_len = payload_len - registry_bytes;
    uint8_t *v76 = malloc(v76_payload_len + 8);
    if (!v76) {
        free(v77);
        return false;
    }
    size_t src = 0;
    size_t dst = 0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        size_t keep = registry_offsets[i] - src;
        memcpy(&v76[dst], &v77[src], keep);
        dst += keep;
        src = registry_offsets[i] + AUTHORITY_REGISTRY_WIRE_SIZE;
    }
    memcpy(&v76[dst], &v77[src], payload_len - src);
    dst += payload_len - src;
    free(v77);
    if (dst != v76_payload_len || v76_payload_len < 8) {
        free(v76);
        return false;
    }

    uint32_t version = 76;
    memcpy(&v76[4], &version, sizeof(version));
    uint32_t crc = persistence_crc32_update(
        0, v76, v76_payload_len);
    memcpy(&v76[v76_payload_len], &crc_magic_expected,
           sizeof(crc_magic_expected));
    memcpy(&v76[v76_payload_len + 4], &crc, sizeof(crc));

    f = fopen(v76_path, "wb");
    if (!f) {
        free(v76);
        return false;
    }
    ok = fwrite(v76, 1, v76_payload_len + 8, f) ==
         v76_payload_len + 8;
    if (fclose(f) != 0) ok = false;
    free(v76);
    return ok;
}

TEST(test_station_authority_registry_current_unknown_and_deny_only) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    w->rng = 638u;
    world_reset(w);

    for (int i = 0; i < SIGNAL_SEEDED_STATION_COUNT; i++) {
        const station_t *st = &w->stations[i];
        ASSERT(station_authority_registry_validate(st));
        ASSERT_EQ_INT(st->authority_registry_version,
                      STATION_AUTHORITY_REGISTRY_VERSION);
        ASSERT_EQ_INT(st->authority_registry_count, 1);
        ASSERT(memcmp(st->authority_registry[0].pubkey,
                      st->station_pubkey, 32) == 0);
        ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                          st, st->station_pubkey),
                      CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
        ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                          st, st->station_pubkey),
                      CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    }

    station_t *st = &w->stations[0];
    uint8_t unknown[32];
    memset(unknown, 0xA5, sizeof(unknown));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(st, unknown),
                  CARGO_RECEIPT_AUTHORITY_UNKNOWN);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(st, unknown),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      st, w->stations[1].station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_UNKNOWN);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                      st, w->stations[1].station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED);

    ASSERT(station_authority_registry_set_trust(
        st, unknown, CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(st, unknown),
                  CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(st, unknown),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED);

    uint8_t denied_revoked[32];
    memset(denied_revoked, 0x5A, sizeof(denied_revoked));
    ASSERT(station_authority_registry_set_trust(
        st, denied_revoked, CARGO_RECEIPT_AUTHORITY_REVOKED));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(st, denied_revoked),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);
    /*
     * A deny-only key has a policy decision but no verified historical
     * lifecycle. Resolution must not fabricate one.
     */
    ASSERT_EQ_INT(
        station_authority_lifecycle_for_pubkey(st, denied_revoked),
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED);

    station_t empty;
    memset(&empty, 0, sizeof(empty));
    ASSERT(station_authority_registry_validate(&empty));
}

TEST(test_station_authority_registry_rekey_and_monotonic_distrust) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_configure_secret("registry-operator-alpha");
    station_authority_init_seeded(&st, 638u, 0);

    uint8_t alpha_pub[32];
    memcpy(alpha_pub, st.station_pubkey, sizeof(alpha_pub));

    station_authority_configure_secret("registry-operator-beta");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REKEYED);
    uint8_t beta_pub[32];
    memcpy(beta_pub, st.station_pubkey, sizeof(beta_pub));
    ASSERT(memcmp(alpha_pub, beta_pub, 32) != 0);
    ASSERT(station_authority_registry_validate(&st));
    ASSERT_EQ_INT(st.authority_registry_count, 2);
    ASSERT(memcmp(st.authority_registry[1].pubkey, alpha_pub, 32) == 0);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, beta_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(&st, beta_pub),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);

    /* Deliberate rollback to a trusted historical key avoids duplicates. */
    station_authority_configure_secret("registry-operator-alpha");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REKEYED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, beta_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(st.authority_registry_count, 2);

    station_authority_configure_secret("registry-operator-beta");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REKEYED);
    ASSERT(station_authority_registry_set_trust(
        &st, alpha_pub, CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    ASSERT(station_authority_registry_set_trust(
        &st, alpha_pub, CARGO_RECEIPT_AUTHORITY_REVOKED));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED);
    ASSERT(!station_authority_registry_set_trust(
        &st, alpha_pub, CARGO_RECEIPT_AUTHORITY_UNTRUSTED));

    /* Reconfiguration cannot reactivate the revoked key or alter secrets. */
    uint8_t beta_secret[64];
    memcpy(beta_secret, st.station_secret, sizeof(beta_secret));
    station_authority_configure_secret("registry-operator-alpha");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REJECTED);
    ASSERT(memcmp(st.station_pubkey, beta_pub, 32) == 0);
    ASSERT(memcmp(st.station_secret, beta_secret, 64) == 0);

    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_conflicts_fail_closed) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_init_seeded(&st, 638u, 0);
    st.authority_registry_count = 2;
    st.authority_registry[1] = st.authority_registry[0];
    st.authority_registry[1].lifecycle =
        STATION_AUTHORITY_LIFECYCLE_REVOKED;
    st.authority_registry[1].trust =
        STATION_AUTHORITY_TRUST_REVOKED;

    ASSERT(!station_authority_registry_validate(&st));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED);
    uint8_t cargo_pub[32];
    memset(cargo_pub, 0xC6, sizeof(cargo_pub));
    cargo_receipt_chain_t incoming = {0};
    cargo_receipt_transfer_link_t link =
        cargo_receipt_prepare_transfer_link(
            &st, st.station_pubkey, cargo_pub, &incoming);
    ASSERT_EQ_INT(link.status,
                  CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN_AUTHORITY);
    ASSERT_EQ_INT(link.origin_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED);
    ASSERT_EQ_INT(link.origin_trust,
                  CARGO_RECEIPT_AUTHORITY_REVOKED);

    station_authority_registry_init(&st);
    st.authority_registry[0].trust =
        STATION_AUTHORITY_TRUST_ROTATED;
    ASSERT(!station_authority_registry_validate(&st));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);

    station_authority_registry_init(&st);
    st.authority_registry_pad[0] = 1;
    ASSERT(!station_authority_registry_validate(&st));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);

    station_authority_registry_init(&st);
    st.authority_registry[0]._pad[1] = 1;
    ASSERT(!station_authority_registry_validate(&st));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);

    station_authority_registry_init(&st);
    st.authority_registry[STATION_AUTHORITY_REGISTRY_CAP - 1].pubkey[0] = 1;
    ASSERT(!station_authority_registry_validate(&st));
}

TEST(test_station_authority_registry_capacity_preserves_distrust) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_configure_secret("registry-capacity-alpha");
    station_authority_init_seeded(&st, 638u, 0);

    uint8_t denied[STATION_AUTHORITY_REGISTRY_CAP - 1][32];
    for (int i = 0; i < STATION_AUTHORITY_REGISTRY_CAP - 1; i++) {
        memset(denied[i], 0, sizeof(denied[i]));
        denied[i][0] = (uint8_t)(0x40 + i);
        denied[i][31] = (uint8_t)(0xA0 + i);
        ASSERT(station_authority_registry_set_trust(
            &st, denied[i], CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    }
    ASSERT_EQ_INT(st.authority_registry_count,
                  STATION_AUTHORITY_REGISTRY_CAP);

    uint8_t live_pub[32];
    uint8_t live_secret[64];
    memcpy(live_pub, st.station_pubkey, sizeof(live_pub));
    memcpy(live_secret, st.station_secret, sizeof(live_secret));
    station_authority_configure_secret("registry-capacity-beta");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REJECTED);
    ASSERT(memcmp(st.station_pubkey, live_pub, sizeof(live_pub)) == 0);
    ASSERT(memcmp(st.station_secret, live_secret, sizeof(live_secret)) == 0);
    for (int i = 0; i < STATION_AUTHORITY_REGISTRY_CAP - 1; i++) {
        ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, denied[i]),
                      CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    }
    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_capacity_evicts_oldest_trusted_rotation) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_configure_secret("registry-rotation-0");
    station_authority_init_seeded(&st, 638u, 0);
    uint8_t oldest[32];
    uint8_t next_oldest[32] = {0};
    memcpy(oldest, st.station_pubkey, sizeof(oldest));

    for (int i = 1; i <= STATION_AUTHORITY_REGISTRY_CAP; i++) {
        char secret[48];
        snprintf(secret, sizeof(secret), "registry-rotation-%d", i);
        station_authority_configure_secret(secret);
        ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                      STATION_AUTHORITY_REDERIVE_REKEYED);
        ASSERT(station_authority_registry_validate(&st));
        ASSERT(st.authority_registry_count <=
               STATION_AUTHORITY_REGISTRY_CAP);
        if (i == 1)
            memcpy(next_oldest, st.station_pubkey, sizeof(next_oldest));
    }

    ASSERT_EQ_INT(st.authority_registry_count,
                  STATION_AUTHORITY_REGISTRY_CAP);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, oldest),
                  CARGO_RECEIPT_AUTHORITY_UNKNOWN);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(&st, oldest),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, next_oldest),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(&st, next_oldest),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_legacy_synthesizes_current_only) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_init_seeded(&st, 638u, 0);
    memset(st.authority_registry, 0, sizeof(st.authority_registry));
    memset(st.authority_registry_pad, 0,
           sizeof(st.authority_registry_pad));
    st.authority_registry_version = 0;
    st.authority_registry_count = 0;

    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_UNCHANGED);
    ASSERT(station_authority_registry_validate(&st));
    ASSERT_EQ_INT(st.authority_registry_count, 1);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_v76_byte_stream_migrates) {
    const char *v77_path = TMP("test_auth_registry_source_v77.sav");
    const char *v76_path = TMP("test_auth_registry_exact_v76.sav");
    station_authority_configure_secret("registry-v76-migration");

    WORLD_HEAP original = calloc(1, sizeof(world_t));
    ASSERT(original);
    original->rng = 7638u;
    world_reset(original);
    /*
     * Give every serialized station slot a unique canonical registry blob so
     * the test can mechanically remove the exact v77-only span without
     * relying on a production test hook or guessing variable-length offsets.
     */
    for (int i = SIGNAL_FIRST_OUTPOST_INDEX; i < MAX_STATIONS; i++) {
        station_t *slot = &original->stations[i];
        snprintf(slot->name, sizeof(slot->name), "Migration %03d", i);
        uint8_t founder[32] = {0};
        founder[0] = (uint8_t)i;
        founder[1] = (uint8_t)(i >> 8);
        founder[31] = (uint8_t)(0xA5u ^ (uint8_t)i);
        station_authority_init_outpost(
            slot, founder, (uint64_t)(760000 + i));
        slot->id = i + 1;
    }
    original->station_count = MAX_STATIONS;
    uint8_t expected_pubkeys[MAX_STATIONS][32];
    for (int i = 0; i < MAX_STATIONS; i++) {
        ASSERT(station_authority_registry_validate(
            &original->stations[i]));
        memcpy(expected_pubkeys[i],
               original->stations[i].station_pubkey, 32);
    }

    ASSERT(world_save(original, v77_path));
    ASSERT(authority_construct_v76_save(
        original, v77_path, v76_path));

    FILE *f = fopen(v76_path, "rb");
    ASSERT(f);
    uint32_t magic = 0;
    uint32_t version = 0;
    ASSERT_EQ_INT((int)fread(&magic, sizeof(magic), 1, f), 1);
    ASSERT_EQ_INT((int)fread(&version, sizeof(version), 1, f), 1);
    fclose(f);
    ASSERT_EQ_INT((int)magic, (int)0x5349474E);
    ASSERT_EQ_INT((int)version, 76);

    WORLD_HEAP migrated = calloc(1, sizeof(world_t));
    ASSERT(migrated);
    ASSERT(world_load(migrated, v76_path));
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *slot = &migrated->stations[i];
        ASSERT(memcmp(slot->station_pubkey, expected_pubkeys[i], 32) == 0);
        ASSERT(station_authority_registry_validate(slot));
        ASSERT_EQ_INT(slot->authority_registry_version,
                      STATION_AUTHORITY_REGISTRY_VERSION);
        ASSERT_EQ_INT(slot->authority_registry_count, 1);
        ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                          slot, expected_pubkeys[i]),
                      CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
        ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                          slot, expected_pubkeys[i]),
                      CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    }

    station_authority_use_dev_secret();
    remove(v77_path);
    remove(v76_path);
}

TEST(test_station_authority_registry_save_load_and_secret_omission) {
    const char *first_path = TMP("test_auth_registry_first.sav");
    const char *second_path = TMP("test_auth_registry_second.sav");
    station_authority_configure_secret("save-registry-alpha");

    WORLD_HEAP original = calloc(1, sizeof(world_t));
    ASSERT(original);
    original->rng = 638u;
    world_reset(original);
    uint8_t old_station0[32];
    uint8_t old_station1[32];
    memcpy(old_station0, original->stations[0].station_pubkey, 32);
    memcpy(old_station1, original->stations[1].station_pubkey, 32);
    const station_t *empty = &original->stations[MAX_STATIONS - 1];
    ASSERT(memcmp(empty->station_pubkey,
                  (const uint8_t[32]){0}, 32) == 0);
    ASSERT_EQ_INT(empty->authority_registry_version, 0);
    ASSERT_EQ_INT(empty->authority_registry_count, 0);
    ASSERT(station_authority_registry_validate(empty));
    /* The v77 writer must accept canonical all-zero unused slots. */
    ASSERT(world_save(original, first_path));

    station_authority_configure_secret("save-registry-beta");
    WORLD_HEAP rotated = calloc(1, sizeof(world_t));
    ASSERT(rotated);
    ASSERT(world_load(rotated, first_path));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &rotated->stations[0], old_station0),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                      &rotated->stations[0], old_station0),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &rotated->stations[1], old_station1),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    uint8_t current_station0[32];
    memcpy(current_station0, rotated->stations[0].station_pubkey, 32);
    uint8_t denied_unknown[32];
    memset(denied_unknown, 0xD3, sizeof(denied_unknown));
    ASSERT(station_authority_registry_set_trust(
        &rotated->stations[0], old_station0,
        CARGO_RECEIPT_AUTHORITY_REVOKED));
    ASSERT(station_authority_registry_set_trust(
        &rotated->stations[0], denied_unknown,
        CARGO_RECEIPT_AUTHORITY_UNTRUSTED));

    uint8_t private_seed[32];
    memcpy(private_seed, rotated->stations[0].station_secret,
           sizeof(private_seed));
    ASSERT(world_save(rotated, second_path));
    ASSERT(!authority_file_contains_span(
        second_path, private_seed, sizeof(private_seed)));
    ASSERT(authority_file_contains_span(
        second_path, current_station0, sizeof(current_station0)));
    ASSERT(authority_file_contains_span(
        second_path, old_station0, sizeof(old_station0)));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded);
    ASSERT(world_load(loaded, second_path));
    ASSERT(station_authority_registry_validate(&loaded->stations[0]));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &loaded->stations[0], current_station0),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &loaded->stations[0], old_station0),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                      &loaded->stations[0], old_station0),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &loaded->stations[0], denied_unknown),
                  CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(station_authority_lifecycle_for_pubkey(
                      &loaded->stations[0], denied_unknown),
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &loaded->stations[1], old_station1),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    station_authority_use_dev_secret();
    remove(first_path);
    remove(second_path);
}

TEST(test_station_authority_historical_origin_composes_trust_verdicts) {
    station_authority_configure_secret("history-registry-alpha");
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w);
    w->rng = 6389u;
    world_reset(w);
    station_t *st = &w->stations[0];
    chain_log_reset(st);
    st->chain_event_count = 0;
    memset(st->chain_last_hash, 0, sizeof(st->chain_last_hash));

    uint8_t cargo_pub[32];
    uint8_t fragment_pub[32];
    uint8_t recipient[32];
    memset(fragment_pub, 0x23, sizeof(fragment_pub));
    memset(recipient, 0x19, sizeof(recipient));
    cargo_unit_t unit = {0};
    ASSERT(hash_ingot(
        COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
        fragment_pub, 0, &unit));
    unit.origin_station = 0;
    unit.mined_block = 240u;
    memcpy(cargo_pub, unit.pub, sizeof(cargo_pub));
    chain_payload_smelt_t smelt = {0};
    ASSERT(chain_payload_smelt_bind_output(
        &smelt, fragment_pub, 0, &unit));
    w->time = 2.0f;
    ASSERT(chain_log_emit(w, st, CHAIN_EVT_SMELT,
                          &smelt, (uint16_t)sizeof(smelt)) != 0);

    cargo_receipt_chain_t chain = {0};
    ASSERT(cargo_receipt_emit_transfer(
               w, st, st->station_pubkey, recipient, &unit, &chain,
               &chain.links[0]) != 0);
    chain.len = 1;

    cargo_receipt_origin_proof_t proof;
    ASSERT_EQ_INT(cargo_receipt_resolve_local_origin(
                      st, cargo_pub, &proof),
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.authority_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT);
    cargo_receipt_authority_trust_t trust =
        station_authority_trust_for_pubkey(st, proof.authority);
    ASSERT_EQ_INT(trust, CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(cargo_receipt_trust_verify(
                      chain.links, chain.len, cargo_pub, &proof, trust).status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED);

    uint8_t old_pub[32];
    memcpy(old_pub, st->station_pubkey, sizeof(old_pub));
    uint8_t old_tail[32];
    memcpy(old_tail, st->chain_last_hash, sizeof(old_tail));
    ASSERT(st->chain_event_count > 0);
    const char *rekey_path = TMP("test_auth_history_rekey.sav");
    ASSERT(world_save(w, rekey_path));

    station_authority_configure_secret("history-registry-beta");
    WORLD_HEAP rotated = calloc(1, sizeof(world_t));
    ASSERT(rotated);
    ASSERT(world_load(rotated, rekey_path));
    station_t *rotated_st = &rotated->stations[0];
    /* This authority-only fixture intentionally saves no catalog geometry;
     * mark the loaded station live before exercising the gameplay evaluator. */
    if (!station_exists(rotated_st))
        rotated_st->signal_range = 1.0f;
    ASSERT(memcmp(rotated_st->station_pubkey, old_pub, sizeof(old_pub)) != 0);
    ASSERT_EQ_INT((int)rotated_st->chain_event_count, 0);
    ASSERT(memcmp(rotated_st->chain_last_hash,
                  (const uint8_t[32]){0}, 32) == 0);
    ASSERT(memcmp(old_tail, rotated_st->chain_last_hash, 32) != 0);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      rotated_st, old_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    ASSERT_EQ_INT(cargo_receipt_resolve_local_origin(
                      rotated_st, cargo_pub, &proof),
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT(memcmp(proof.authority, old_pub, sizeof(old_pub)) == 0);
    ASSERT_EQ_INT(proof.authority_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    trust = station_authority_trust_for_pubkey(
        rotated_st, proof.authority);
    ASSERT_EQ_INT(trust, CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(cargo_receipt_trust_verify(
                      chain.links, chain.len, cargo_pub, &proof, trust).status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);

    cargo_receipt_chain_t empty = {0};
    cargo_receipt_transfer_link_t first_hop =
        cargo_receipt_prepare_transfer_link(
            rotated_st, rotated_st->station_pubkey,
            cargo_pub, &empty);
    ASSERT_EQ_INT(first_hop.status,
                  CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN_AUTHORITY);
    ASSERT_EQ_INT(first_hop.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(first_hop.origin_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    ASSERT_EQ_INT(first_hop.origin_trust,
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    cargo_receipt_transfer_link_t continuation =
        cargo_receipt_prepare_transfer_link(
            rotated_st, recipient, cargo_pub, &chain);
    ASSERT_EQ_INT(continuation.status,
                  CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(continuation.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED);
    ASSERT(memcmp(chain.links[0].authoring_station,
                  (const uint8_t[32]){0}, 32) != 0);
    ASSERT(memcmp(chain.links[0].prev_receipt_hash,
                  (const uint8_t[32]){0}, 32) != 0);
    ASSERT(memcmp(unit.pub, (const uint8_t[32]){0}, 32) != 0);
    cargo_receipt_origin_proof_t pinned = {0};
    ASSERT_EQ_INT(
        cargo_receipt_resolve_origin_for_authority_pinned(
            rotated_st, chain.links[0].authoring_station,
            unit.pub, chain.links[0].prev_receipt_hash,
            &pinned),
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            rotated, 0, &unit, &chain);
    ASSERT_EQ_INT(evaluated.origin_status,
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(evaluated.trust.status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);
    ASSERT(evaluated.accepted);
    cargo_receipt_prepared_transfer_t prepared =
        cargo_receipt_prepare_transfer(
            rotated, 0, recipient,
            rotated_st->station_pubkey, &unit,
            &chain, false, 0, NULL);
    ASSERT_EQ_INT(
        prepared.link_status,
        CARGO_RECEIPT_TRANSFER_LINK_READY);
    ASSERT_EQ_INT(
        prepared.preflight_status,
        CHAIN_LOG_APPEND_OK);
    cargo_receipt_t second = {0};
    ASSERT(cargo_receipt_emit_transfer(
               rotated, rotated_st, recipient,
               rotated_st->station_pubkey, &unit,
               &chain, &second) != 0);
    chain.links[chain.len++] = second;
    ASSERT_EQ_INT(cargo_receipt_chain_verify(
                      chain.links, chain.len, cargo_pub),
                  CARGO_RECEIPT_OK);
    ASSERT_EQ_INT(cargo_receipt_trust_verify(
                      chain.links, chain.len, cargo_pub, &proof, trust).status,
                  CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED);

    ASSERT(station_authority_registry_set_trust(
        rotated_st, old_pub, CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    ASSERT_EQ_INT(cargo_receipt_resolve_local_origin(
                      rotated_st, cargo_pub, &proof),
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.authority_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_ROTATED);
    trust = station_authority_trust_for_pubkey(
        rotated_st, proof.authority);
    ASSERT_EQ_INT(trust, CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(cargo_receipt_trust_verify(
                      chain.links, chain.len, cargo_pub, &proof, trust).status,
                  CARGO_RECEIPT_TRUST_REJECT_UNTRUSTED_AUTHORITY);

    ASSERT(station_authority_registry_set_trust(
        rotated_st, old_pub, CARGO_RECEIPT_AUTHORITY_REVOKED));
    ASSERT_EQ_INT(cargo_receipt_resolve_local_origin(
                      rotated_st, cargo_pub, &proof),
                  CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED);
    ASSERT_EQ_INT(proof.authority_lifecycle,
                  CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED);
    trust = station_authority_trust_for_pubkey(
        rotated_st, proof.authority);
    ASSERT_EQ_INT(trust, CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT_EQ_INT(cargo_receipt_trust_verify(
                      chain.links, chain.len, cargo_pub, &proof, trust).status,
                  CARGO_RECEIPT_TRUST_REJECT_REVOKED_AUTHORITY);

    station_authority_use_dev_secret();
    remove(rekey_path);
}

void register_station_authority_tests(void);
void register_station_authority_tests(void) {
    TEST_SECTION("\n--- Station Authority (#479 B) ---\n");
    RUN(test_station_authority_seeded_determinism);
    RUN(test_station_authority_seeded_distinct_seeds);
    RUN(test_station_authority_operator_secret_affects_pubkey);
    RUN(test_station_authority_outpost_derivation);
    RUN(test_station_authority_sign_verify_roundtrip);
    RUN(test_station_authority_save_load_rederives_secret);
    RUN(test_station_authority_wire_omits_secret);
    RUN(test_station_authority_outpost_save_load);
    RUN(test_station_authority_registry_current_unknown_and_deny_only);
    RUN(test_station_authority_registry_rekey_and_monotonic_distrust);
    RUN(test_station_authority_registry_conflicts_fail_closed);
    RUN(test_station_authority_registry_capacity_preserves_distrust);
    RUN(test_station_authority_registry_capacity_evicts_oldest_trusted_rotation);
    RUN(test_station_authority_registry_legacy_synthesizes_current_only);
    RUN(test_station_authority_registry_v76_byte_stream_migrates);
    RUN(test_station_authority_registry_save_load_and_secret_omission);
    RUN(test_station_authority_historical_origin_composes_trust_verdicts);
}
