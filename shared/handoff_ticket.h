/*
 * handoff_ticket.h -- Signed cross-zone ship handoff envelope.
 *
 * A handoff ticket is authored by the source authority before a ship leaves
 * its zone. It binds the player pubkey, source/destination authorities,
 * expiry, a ship-state hash, and a cargo/receipt root. The destination
 * authority can verify the signature and compare the hashes against the
 * presented ship snapshot plus receipt chains before accepting the handoff.
 */
#ifndef SHARED_HANDOFF_TICKET_H
#define SHARED_HANDOFF_TICKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cargo_receipt.h"
#include "manifest.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HANDOFF_TICKET_VERSION       1u
#define HANDOFF_TICKET_UNSIGNED_SIZE 188u
#define HANDOFF_TICKET_SIZE          252u
#define HANDOFF_SHIP_WIRE_SIZE       (24u + COMMODITY_COUNT * 4u + 16u + 1u + 20u + 2u + 4u + 4u)
#define HANDOFF_CARGO_UNIT_WIRE_SIZE CARGO_UNIT_WIRE_SIZE
#define HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE (HANDOFF_SHIP_WIRE_SIZE + 2u)
#define HANDOFF_SHIP_SNAPSHOT_MAX_CARGO SHIP_MANIFEST_DEFAULT_CAP
#define HANDOFF_SHIP_SNAPSHOT_MAX_SIZE \
    (HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE + \
     HANDOFF_SHIP_SNAPSHOT_MAX_CARGO * \
     (HANDOFF_CARGO_UNIT_WIRE_SIZE + 1u + \
      CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE))

typedef struct {
    uint8_t  source_authority[32];
    uint8_t  dest_authority[32];
    uint8_t  player_pubkey[32];
    uint64_t issued_tick;
    uint64_t expires_tick;
    uint32_t source_zone;
    uint32_t dest_zone;
    uint16_t cargo_count;
    uint16_t flags;
    uint8_t  ship_state_hash[32];
    uint8_t  cargo_root[32];
    uint8_t  signature[64];
} handoff_ticket_t;

typedef enum {
    HANDOFF_TICKET_OK = 0,
    HANDOFF_TICKET_REJECT_BAD_ARGS,
    HANDOFF_TICKET_REJECT_EXPIRED,
    HANDOFF_TICKET_REJECT_ZERO_AUTHORITY,
    HANDOFF_TICKET_REJECT_SOURCE,
    HANDOFF_TICKET_REJECT_DEST,
    HANDOFF_TICKET_REJECT_PLAYER,
    HANDOFF_TICKET_REJECT_SHIP_STATE,
    HANDOFF_TICKET_REJECT_CARGO_ROOT,
    HANDOFF_TICKET_REJECT_BAD_SIGNATURE
} handoff_ticket_result_t;

void handoff_ticket_unsigned_pack(
    const handoff_ticket_t *ticket,
    uint8_t out[HANDOFF_TICKET_UNSIGNED_SIZE]);
void handoff_ticket_pack(const handoff_ticket_t *ticket,
                         uint8_t out[HANDOFF_TICKET_SIZE]);
bool handoff_ticket_unpack(const uint8_t in[HANDOFF_TICKET_SIZE],
                           handoff_ticket_t *out);

void handoff_ticket_ship_state_hash(const ship_t *ship, uint8_t out[32]);
void handoff_ticket_cargo_root(const ship_t *ship, uint8_t out[32]);
void handoff_ticket_hash(const handoff_ticket_t *ticket, uint8_t out[32]);

/* Field-packed ship snapshot carried with NET_MSG_HANDOFF_PRESENT. The
 * snapshot includes exactly the ship fields bound by handoff_ticket_*_hash,
 * plus manifest cargo units and their receipt chains. `unpack` writes an owned
 * ship_t into `out`; callers should pass a zeroed ship_t and later call
 * ship_cleanup(out). */
size_t handoff_ship_snapshot_size(const ship_t *ship);
bool handoff_ship_snapshot_pack(const ship_t *ship, uint8_t *out, size_t cap,
                                size_t *out_len);
bool handoff_ship_snapshot_unpack(const uint8_t *data, size_t len,
                                  ship_t *out, size_t *consumed);

bool handoff_ticket_issue_for_ship(
    const uint8_t source_authority[32],
    const uint8_t source_secret[64],
    const uint8_t dest_authority[32],
    const uint8_t player_pubkey[32],
    uint32_t source_zone,
    uint32_t dest_zone,
    uint64_t issued_tick,
    uint64_t expires_tick,
    const ship_t *ship,
    handoff_ticket_t *out);

handoff_ticket_result_t handoff_ticket_verify_hashes(
    const handoff_ticket_t *ticket,
    uint64_t now_tick,
    const uint8_t expected_source_authority[32],
    const uint8_t expected_dest_authority[32],
    const uint8_t expected_player_pubkey[32],
    const uint8_t expected_ship_state_hash[32],
    const uint8_t expected_cargo_root[32]);

handoff_ticket_result_t handoff_ticket_verify_for_ship(
    const handoff_ticket_t *ticket,
    uint64_t now_tick,
    const uint8_t expected_source_authority[32],
    const uint8_t expected_dest_authority[32],
    const uint8_t expected_player_pubkey[32],
    const ship_t *ship);

const char *handoff_ticket_result_name(handoff_ticket_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_HANDOFF_TICKET_H */
