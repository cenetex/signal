#include "pubkey_proof.h"

#include <string.h>

_Static_assert(sizeof(PUBKEY_PROOF_DOMAIN) - 1 == PUBKEY_PROOF_DOMAIN_LEN,
               "challenge proof domain length drifted");
_Static_assert(sizeof(PUBKEY_PROOF_V1_DOMAIN) - 1 ==
                   PUBKEY_PROOF_V1_DOMAIN_LEN,
               "legacy proof domain length drifted");

bool pubkey_proof_message(uint8_t out[PUBKEY_PROOF_MESSAGE_SIZE],
                          const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
                          const uint8_t session_token[8],
                          const uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE]) {
    if (!out || !pubkey || !session_token || !challenge) return false;
    memcpy(out, PUBKEY_PROOF_DOMAIN, PUBKEY_PROOF_DOMAIN_LEN);
    memcpy(out + PUBKEY_PROOF_DOMAIN_LEN, pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES);
    memcpy(out + PUBKEY_PROOF_DOMAIN_LEN + SIGNAL_CRYPTO_PUBKEY_BYTES,
           session_token, 8);
    memcpy(out + PUBKEY_PROOF_DOMAIN_LEN + SIGNAL_CRYPTO_PUBKEY_BYTES + 8,
           challenge, PUBKEY_PROOF_CHALLENGE_SIZE);
    return true;
}

bool pubkey_proof_sign(
    uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    const uint8_t session_token[8],
    const uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE]) {
    if (!out_sig || !pubkey || !secret || !session_token || !challenge)
        return false;
    uint8_t msg[PUBKEY_PROOF_MESSAGE_SIZE];
    if (!pubkey_proof_message(msg, pubkey, session_token, challenge))
        return false;
    signal_crypto_sign(out_sig, msg, sizeof(msg), secret);
    return true;
}

bool pubkey_proof_verify(
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t session_token[8],
    const uint8_t challenge[PUBKEY_PROOF_CHALLENGE_SIZE],
    const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]) {
    if (!pubkey || !session_token || !challenge || !sig) return false;
    uint8_t msg[PUBKEY_PROOF_MESSAGE_SIZE];
    if (!pubkey_proof_message(msg, pubkey, session_token, challenge))
        return false;
    return signal_crypto_verify(sig, msg, sizeof(msg), pubkey);
}

bool pubkey_proof_v1_message(
    uint8_t out[PUBKEY_PROOF_V1_MESSAGE_SIZE],
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t session_token[8]) {
    if (!out || !pubkey || !session_token) return false;
    memcpy(out, PUBKEY_PROOF_V1_DOMAIN, PUBKEY_PROOF_V1_DOMAIN_LEN);
    memcpy(out + PUBKEY_PROOF_V1_DOMAIN_LEN,
           pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES);
    memcpy(out + PUBKEY_PROOF_V1_DOMAIN_LEN + SIGNAL_CRYPTO_PUBKEY_BYTES,
           session_token, 8);
    return true;
}

bool pubkey_proof_v1_sign(
    uint8_t out_sig[SIGNAL_CRYPTO_SIG_BYTES],
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    const uint8_t session_token[8]) {
    if (!out_sig || !pubkey || !secret || !session_token) return false;
    uint8_t msg[PUBKEY_PROOF_V1_MESSAGE_SIZE];
    if (!pubkey_proof_v1_message(msg, pubkey, session_token)) return false;
    signal_crypto_sign(out_sig, msg, sizeof(msg), secret);
    return true;
}

bool pubkey_proof_v1_verify(
    const uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES],
    const uint8_t session_token[8],
    const uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES]) {
    if (!pubkey || !session_token || !sig) return false;
    uint8_t msg[PUBKEY_PROOF_V1_MESSAGE_SIZE];
    if (!pubkey_proof_v1_message(msg, pubkey, session_token)) return false;
    return signal_crypto_verify(sig, msg, sizeof(msg), pubkey);
}

void pubkey_proof_client_state_reset(pubkey_proof_client_state_t *state) {
    if (state) memset(state, 0, sizeof(*state));
}

void pubkey_proof_client_note_protocol(
    pubkey_proof_client_state_t *state,
    uint16_t server_protocol_version) {
    if (!state || state->protocol_info_received) return;
    state->server_protocol_version = server_protocol_version;
    state->protocol_info_received = true;
}

void pubkey_proof_client_note_challenge(
    pubkey_proof_client_state_t *state) {
    if (!state) return;
    state->challenge_received = true;
    if (state->proof_admitted &&
        state->admitted_scheme == PUBKEY_PROOF_SCHEME_LEGACY_V1) {
        state->proof_admitted = false;
        state->admitted_scheme = PUBKEY_PROOF_SCHEME_NONE;
    }
}

pubkey_proof_scheme_t pubkey_proof_client_next_scheme(
    const pubkey_proof_client_state_t *state) {
    if (!state || state->proof_admitted)
        return PUBKEY_PROOF_SCHEME_NONE;
    if (state->challenge_received)
        return PUBKEY_PROOF_SCHEME_CHALLENGE_V2;
    if (state->protocol_info_received &&
        state->server_protocol_version > 0 &&
        state->server_protocol_version <
            SIGNAL_PROTOCOL_CHALLENGE_PUBKEY_PROOF_VERSION) {
        return PUBKEY_PROOF_SCHEME_LEGACY_V1;
    }
    return PUBKEY_PROOF_SCHEME_NONE;
}

void pubkey_proof_client_record_send(
    pubkey_proof_client_state_t *state,
    pubkey_proof_scheme_t scheme,
    bool admitted) {
    if (!state || !admitted || scheme == PUBKEY_PROOF_SCHEME_NONE ||
        scheme != pubkey_proof_client_next_scheme(state)) {
        return;
    }
    state->proof_admitted = true;
    state->admitted_scheme = scheme;
}
