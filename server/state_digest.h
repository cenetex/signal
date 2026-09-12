#ifndef SIGNAL_STATE_DIGEST_H
#define SIGNAL_STATE_DIGEST_H

#include <stdint.h>

#include "game_sim.h"

#define SIGNAL_AUTH_STATE_DIGEST_SCHEMA "signal.authoritative_state.v4"
#define SIGNAL_AUTH_STATE_DIGEST_VERSION 4u
#define SIGNAL_AUTH_STATE_DIGEST_SIZE 32u

const char *signal_authoritative_state_digest_schema(void);
uint32_t signal_authoritative_state_digest_version(void);
void signal_authoritative_state_digest(
    const world_t *world,
    uint8_t out[SIGNAL_AUTH_STATE_DIGEST_SIZE]);

#endif /* SIGNAL_STATE_DIGEST_H */
