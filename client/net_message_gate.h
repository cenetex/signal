#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "protocol.h"
#include "pubkey_proof.h"

/*
 * Pure client transport-admission policy. Authentication bootstrap messages
 * are admitted after exact protocol negotiation. Gameplay messages remain
 * blocked until the authoritative local state arrives, with one deliberately
 * narrow exception: the exact signed confirmation for the opaque recovery
 * offer received on this authenticated connection.
 */
typedef struct {
    bool protocol_ready;
    bool gameplay_ready;
    bool proof_admitted;
    pubkey_proof_scheme_t proof_scheme;
    bool legacy_recovery_offer_ready;
    uint8_t legacy_recovery_offer_id[LEGACY_RECOVERY_OFFER_ID_SIZE];
} net_client_message_gate_state_t;

static inline bool net_client_message_gate_allows(
    const net_client_message_gate_state_t *state,
    const uint8_t *data,
    int len)
{
    if (!state || !data || len <= 0 || !state->protocol_ready)
        return false;

    if (data[0] == NET_MSG_REGISTER_PUBKEY ||
        data[0] == NET_MSG_SESSION ||
        data[0] == NET_MSG_PROVE_PUBKEY) {
        return true;
    }

    if (state->gameplay_ready)
        return true;

    const int recovery_len =
        SIGNED_ACTION_HEADER_SIZE +
        LEGACY_RECOVERY_OFFER_ID_SIZE +
        SIGNED_ACTION_SIG_SIZE;
    if (!state->proof_admitted ||
        state->proof_scheme != PUBKEY_PROOF_SCHEME_CHALLENGE_V2 ||
        !state->legacy_recovery_offer_ready ||
        len != recovery_len ||
        data[0] != NET_MSG_SIGNED_ACTION ||
        data[9] != SIGNED_ACTION_RECOVER_LEGACY_SAVE ||
        data[10] != LEGACY_RECOVERY_OFFER_ID_SIZE ||
        data[11] != 0) {
        return false;
    }

    return memcmp(&data[SIGNED_ACTION_HEADER_SIZE],
                  state->legacy_recovery_offer_id,
                  LEGACY_RECOVERY_OFFER_ID_SIZE) == 0;
}
