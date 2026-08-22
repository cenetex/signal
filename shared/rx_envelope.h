/*
 * rx_envelope.h — station-to-raticross bridge.
 *
 * A Signal station is already an Ed25519 identity. This header
 * exposes that identity as a raticross Actor so other hosts can
 * verify chain events and route to a station.
 *
 * This is additive — no chain log format change, no wire change.
 */
#ifndef SIGNAL_RX_ENVELOPE_H
#define SIGNAL_RX_ENVELOPE_H

#include <stddef.h>
#include <stdint.h>

#include "raticross.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RX_ENVELOPE_MAX_PAYLOAD  65535u
#define RX_ENVELOPE_BUFSZ(max_payload) \
    rc_message_size((uint16_t)(max_payload))

/*
 * Fill a raticross header so the station speaks as a SYSTEM actor to
 * `world_pk`.  Caller provides the station secret (64 bytes,
 * seed || pk, NaCl convention) and optional envelope metadata.
 *
 * Returns RC_OK on success.
 */
rc_status rx_station_fill_actor(
    rc_header *h,
    const uint8_t station_sk[RC_SECRET_SIZE],
    const uint8_t world_pk[RC_PUBKEY_SIZE],
    const uint8_t envelope_id[RC_ID_SIZE],
    uint64_t ts_ms);

/*
 * Seal payload bytes into a raticross message signed by the station.
 * Returns RC_OK on success; *out_len is set to the framed length.
 */
rc_status rx_station_seal(
    const uint8_t station_sk[RC_SECRET_SIZE],
    const uint8_t world_pk[RC_PUBKEY_SIZE],
    const uint8_t envelope_id[RC_ID_SIZE],
    uint64_t ts_ms,
    const uint8_t *payload,
    size_t payload_len,
    uint8_t *out,
    size_t out_cap,
    size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_RX_ENVELOPE_H */
