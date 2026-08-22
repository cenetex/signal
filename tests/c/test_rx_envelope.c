/*
 * test_rx_envelope.c — raticross station envelope round-trip.
 *
 * A Signal station can seal a raticross message and a verifier
 * with the public key can open it. The station speaks as a SYSTEM
 * actor — no platform id, no session handle.
 */
#include "test_harness.h"

#include "rx_envelope.h"
#include "station_authority.h"
#include "signal_crypto.h"

static void fill_id(uint8_t *p, uint8_t tag) {
    memset(p, 0, RC_ID_SIZE);
    p[0] = tag;
    p[15] = (uint8_t)(0xa0u + tag);
}

TEST(test_rx_envelope_station_seal_and_open) {
    station_t station = {0};
    station_authority_init_seeded(&station, 42, 0);

    uint8_t world_pk[RC_PUBKEY_SIZE];
    memset(world_pk, 0xab, sizeof(world_pk));
    world_pk[0] = 0xfe;

    uint8_t eid[RC_ID_SIZE];
    fill_id(eid, 0xca);

    const uint8_t payload[] = { 's', 'i', 'g', 'n', 'a', 'l' };

    /* Seal. */
    uint8_t msg[RX_ENVELOPE_BUFSZ(sizeof(payload))];
    size_t n = 0;
    rc_status st;

    st = rx_station_seal(station.station_secret, world_pk, eid, 1000,
                        payload, sizeof(payload),
                        msg, sizeof(msg), &n);
    ASSERT(st == RC_OK);
    ASSERT(n >= RC_HEADER_V1_SIZE + sizeof(payload) + RC_SIG_SIZE);

    /* Open with pubkey. */
    rc_header h;
    const uint8_t *opened = NULL;
    uint16_t olen = 0;

    st = rc_message_open(msg, n, &h, &opened, &olen);
    ASSERT(st == RC_OK);

    ASSERT(h.from_kind == RC_KIND_SYSTEM);
    ASSERT(h.as_kind == RC_KIND_SYSTEM);
    ASSERT(memcmp(h.as_pubkey, station.station_pubkey, RC_PUBKEY_SIZE) == 0);
    ASSERT(memcmp(h.from_pubkey, station.station_pubkey, RC_PUBKEY_SIZE) == 0);
    ASSERT(olen == sizeof(payload));
    ASSERT(opened != NULL);
    ASSERT(memcmp(opened, payload, sizeof(payload)) == 0);

    /* Tampered header byte fails. */
    msg[RC_OFF_TS_MS] ^= 1;
    st = rc_message_open(msg, n, &h, &opened, &olen);
    ASSERT(st == RC_ERR_VERIFY);

    station_cleanup(&station);
}

void register_rx_envelope_tests(void) {
    TEST_SECTION("\n--- Raticross Station Envelope ---\n");
    RUN(test_rx_envelope_station_seal_and_open);
}
