#ifndef EPISODE_LIFECYCLE_H
#define EPISODE_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#define EPISODE_COUNT 10

typedef uint64_t episode_attempt_token_t;

typedef enum {
    EPISODE_FAILURE_NONE = 0,
    EPISODE_FAILURE_FETCH,
    EPISODE_FAILURE_FILE_READ,
    EPISODE_FAILURE_ALLOCATION,
    EPISODE_FAILURE_DECODER,
    EPISODE_FAILURE_TEXTURE,
} episode_failure_t;

/*
 * Pure episode-attempt state. Media loading and rendering live in episode.c;
 * this small state machine keeps persistence decisions testable without
 * linking pl_mpeg or Sokol.
 */
typedef struct {
    bool watched[EPISODE_COUNT];
    int current;                 /* playback that reached its first frame */
    int pending;                 /* uncommitted load/decode attempt */
    episode_attempt_token_t revision;
    episode_attempt_token_t pending_token;
    episode_failure_t last_failure;
} episode_lifecycle_t;

void episode_lifecycle_init(episode_lifecycle_t *state);

bool episode_lifecycle_begin(episode_lifecycle_t *state, int index,
                             episode_attempt_token_t *out_token);
bool episode_lifecycle_matches(const episode_lifecycle_t *state, int index,
                               episode_attempt_token_t token);
bool episode_lifecycle_fail(episode_lifecycle_t *state, int index,
                            episode_attempt_token_t token,
                            episode_failure_t failure);
bool episode_lifecycle_start(episode_lifecycle_t *state, int index,
                             episode_attempt_token_t token);

/*
 * Explicit skip and natural completion share this transition. It is valid
 * only after start committed watched state; a pending technical failure is
 * never treated as a skip.
 */
bool episode_lifecycle_stop_started(episode_lifecycle_t *state);

/* Invalidates every outstanding token while preserving watched history. */
void episode_lifecycle_reset(episode_lifecycle_t *state);

#endif
