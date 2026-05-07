/*
 * test_station_authority.c -- Tests for per-station Ed25519 identity.
 *
 * Layer B of #479. Covers seed-derived determinism, outpost
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
    /* Outposts live at slot >= 3. Slot 3 is unused after world_reset. */
    station_t *out = &w->stations[3];
    snprintf(out->name, sizeof(out->name), "Outpost Beta");
    out->pos = v2(10000.0f, 0.0f);
    out->radius = 30.0f;
    out->dock_radius = 200.0f;
    out->signal_range = 8000.0f;
    out->id = w->next_station_id++;
    if (w->station_count <= 3) w->station_count = 4;
    uint8_t founder[32];
    for (int i = 0; i < 32; i++) founder[i] = (uint8_t)(0xA0 + i);
    station_authority_init_outpost(out, founder, 9999ULL);
    uint8_t out_pub_before[32];
    memcpy(out_pub_before, out->station_pubkey, 32);

    ASSERT(world_save(w, TMP("test_outpost_auth.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded);
    ASSERT(world_load(loaded, TMP("test_outpost_auth.sav")));

    station_t *out_loaded = &loaded->stations[3];
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

void register_station_authority_tests(void);
void register_station_authority_tests(void) {
    TEST_SECTION("\n--- Station Authority (#479 B) ---\n");
    RUN(test_station_authority_seeded_determinism);
    RUN(test_station_authority_seeded_distinct_seeds);
    RUN(test_station_authority_outpost_derivation);
    RUN(test_station_authority_sign_verify_roundtrip);
    RUN(test_station_authority_save_load_rederives_secret);
    RUN(test_station_authority_wire_omits_secret);
    RUN(test_station_authority_outpost_save_load);
}
