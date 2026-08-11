/*
 * pubkey_proof.h -- Session-bound Ed25519 proof for pubkey registration.
 *
 * REGISTER_PUBKEY is an identity assertion. PROVE_PUBKEY signs a stable
 * domain plus the asserted pubkey, live session token, and (on protocol v3+)
 * a server challenge so the server can safely bind registry/persistence to a
 * private-key holder.
 */
#ifndef SHARED_PUBKEY_PROOF_H
#define SHARED_PUBKEY_PROOF_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "signal_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

bool pubkey_proof_message(uint8_t out[PUBKEY_PROOF_MESSAGE_SIZE],
                          const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                          const uint8_t session_token[8],
                          const uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE]);

bool pubkey_proof_sign(
    uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    const uint8_t session_token[8],
    const uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE]);

bool pubkey_proof_verify(
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t session_token[8],
    const uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE],
    const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]);

/* Compatibility primitive for servers that explicitly advertise protocol
 * version 2 or older. Protocol v3+ servers must never accept this unchallenged
 * signature. */
bool pubkey_proof_v1_message(
    uint8_t out[PUBKEY_PROOF_V1_MESSAGE_SIZE],
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t session_token[8]);

bool pubkey_proof_v1_sign(
    uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    const uint8_t session_token[8]);

bool pubkey_proof_v1_verify(
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t session_token[8],
    const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]);

typedef enum {
    PUBKEY_PROOF_SCHEME_NONE = 0,
    PUBKEY_PROOF_SCHEME_LEGACY_V1,
    PUBKEY_PROOF_SCHEME_CHALLENGE_V2,
} pubkey_proof_scheme_t;

/* Client negotiation state. Challenge receipt is authoritative even if a
 * legacy proof was already admitted after an old PROTOCOL_INFO packet.
 * Only the first PROTOCOL_INFO version is used for authentication downgrade
 * decisions, and failed transport admission never advances the proof latch. */
typedef struct {
    uint16_t server_protocol_version;
    bool protocol_info_received;
    bool challenge_received;
    bool proof_admitted;
    pubkey_proof_scheme_t admitted_scheme;
} pubkey_proof_client_state_t;

void pubkey_proof_client_state_reset(pubkey_proof_client_state_t *state);
void pubkey_proof_client_note_protocol(
    pubkey_proof_client_state_t *state,
    uint16_t server_protocol_version);
void pubkey_proof_client_note_challenge(
    pubkey_proof_client_state_t *state);
pubkey_proof_scheme_t pubkey_proof_client_next_scheme(
    const pubkey_proof_client_state_t *state);
void pubkey_proof_client_record_send(
    pubkey_proof_client_state_t *state,
    pubkey_proof_scheme_t scheme,
    bool admitted);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_PUBKEY_PROOF_H */
