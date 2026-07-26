/*
 * test_station_authority.c -- Tests for per-station Ed25519 identity.
 *
 * Layer B of #479. Covers operator-secret-derived determinism, outpost
 * derivation, sign/verify round trips, save/load secret rederivation,
 * and the wire-format omission discipline (station_secret never on
 * the wire, never on disk).
 */
#include "test_harness.h"

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

TEST(test_station_authority_registry_current_and_unknown) {
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
    }

    uint8_t unknown[32];
    memset(unknown, 0xA5, sizeof(unknown));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &w->stations[0], unknown),
                  CARGO_RECEIPT_AUTHORITY_UNKNOWN);

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
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, beta_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    /* A deliberate rollback to a verified historical key is a rotation,
     * not a duplicate row. */
    station_authority_configure_secret("registry-operator-alpha");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REKEYED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, beta_pub),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
    ASSERT_EQ_INT(st.authority_registry_count, 2);

    /* Rotate away once more, then make alpha's distrust monotonic. */
    station_authority_configure_secret("registry-operator-beta");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REKEYED);
    ASSERT(station_authority_registry_set_trust(
        &st, alpha_pub, CARGO_RECEIPT_AUTHORITY_UNTRUSTED));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT(station_authority_registry_set_trust(
        &st, alpha_pub, CARGO_RECEIPT_AUTHORITY_REVOKED));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, alpha_pub),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);
    ASSERT(!station_authority_registry_set_trust(
        &st, alpha_pub, CARGO_RECEIPT_AUTHORITY_UNTRUSTED));

    /* Reconfiguring the operator secret must not reactivate a revoked
     * key or overwrite the in-memory secret before rejection. */
    uint8_t beta_secret[64];
    memcpy(beta_secret, st.station_secret, sizeof(beta_secret));
    station_authority_configure_secret("registry-operator-alpha");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REJECTED);
    ASSERT(memcmp(st.station_pubkey, beta_pub, 32) == 0);
    ASSERT(memcmp(st.station_secret, beta_secret, 64) == 0);

    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_duplicate_fails_closed) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_init_seeded(&st, 638u, 0);
    st.authority_registry_count = 2;
    st.authority_registry[1] = st.authority_registry[0];
    st.authority_registry[1].state = STATION_AUTHORITY_TRUST_REVOKED;

    ASSERT(!station_authority_registry_validate(&st));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_REVOKED);
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

    /* With no rotated row to evict, a rekey must reject rather than
     * erase an explicit deny-list decision. */
    station_authority_configure_secret("registry-capacity-beta");
    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_REJECTED);
    for (int i = 0; i < STATION_AUTHORITY_REGISTRY_CAP - 1; i++) {
        ASSERT_EQ_INT(station_authority_trust_for_pubkey(&st, denied[i]),
                      CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    }
    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_legacy_synthesizes_current_only) {
    station_t st;
    memset(&st, 0, sizeof(st));
    station_authority_init_seeded(&st, 638u, 0);
    memset(st.authority_registry, 0, sizeof(st.authority_registry));
    memset(st.authority_registry_pad, 0, sizeof(st.authority_registry_pad));
    st.authority_registry_version = 0;
    st.authority_registry_count = 0;

    ASSERT_EQ_INT(station_authority_rederive_secret(&st, 638u, 0),
                  STATION_AUTHORITY_REDERIVE_UNCHANGED);
    ASSERT(station_authority_registry_validate(&st));
    ASSERT_EQ_INT(st.authority_registry_count, 1);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &st, st.station_pubkey),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT);
    station_authority_use_dev_secret();
}

TEST(test_station_authority_registry_save_load_lifecycle) {
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
    ASSERT(world_save(original, first_path));

    station_authority_configure_secret("save-registry-beta");
    WORLD_HEAP rotated = calloc(1, sizeof(world_t));
    ASSERT(rotated);
    ASSERT(world_load(rotated, first_path));
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &rotated->stations[0], old_station0),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);
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
    ASSERT(world_save(rotated, second_path));

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
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &loaded->stations[0], denied_unknown),
                  CARGO_RECEIPT_AUTHORITY_UNTRUSTED);
    ASSERT_EQ_INT(station_authority_trust_for_pubkey(
                      &loaded->stations[1], old_station1),
                  CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED);

    station_authority_use_dev_secret();
    remove(first_path);
    remove(second_path);
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
    RUN(test_station_authority_registry_current_and_unknown);
    RUN(test_station_authority_registry_rekey_and_monotonic_distrust);
    RUN(test_station_authority_registry_duplicate_fails_closed);
    RUN(test_station_authority_registry_capacity_preserves_distrust);
    RUN(test_station_authority_registry_legacy_synthesizes_current_only);
    RUN(test_station_authority_registry_save_load_lifecycle);
}
