#include "rx_envelope.h"

#include <string.h>

rc_status rx_station_fill_actor(
    rc_header *h,
    const uint8_t station_sk[RC_SECRET_SIZE],
    const uint8_t world_pk[RC_PUBKEY_SIZE],
    const uint8_t envelope_id[RC_ID_SIZE],
    uint64_t ts_ms)
{
    const uint8_t *pk = station_sk + RC_PUBKEY_SIZE;
    return rc_header_actor(h, RC_KIND_SYSTEM, pk, RC_KIND_SYSTEM, world_pk,
                          envelope_id, ts_ms);
}

rc_status rx_station_seal(
    const uint8_t station_sk[RC_SECRET_SIZE],
    const uint8_t world_pk[RC_PUBKEY_SIZE],
    const uint8_t envelope_id[RC_ID_SIZE],
    uint64_t ts_ms,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len)
{
    rc_header h;
    rc_status st;

    st = rx_station_fill_actor(&h, station_sk, world_pk, envelope_id, ts_ms);
    if (st != RC_OK) {
        return st;
    }
    return rc_message_seal(&h, payload, payload_len, station_sk,
                          out, out_cap, out_len);
}
