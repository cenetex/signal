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

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HANDOFF_TICKET_VERSION       1u
#define HANDOFF_TICKET_UNSIGNED_SIZE 188u
#define HANDOFF_TICKET_SIZE          252u

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
