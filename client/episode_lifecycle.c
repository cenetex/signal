#include "episode_lifecycle.h"

#include <string.h>

static episode_attempt_token_t episode_next_revision(
    episode_attempt_token_t revision) {
    revision++;
    if (revision == 0) revision = 1;
    return revision;
}

void episode_lifecycle_init(episode_lifecycle_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    state->current = -1;
    state->pending = -1;
}

bool episode_lifecycle_begin(episode_lifecycle_t *state, int index,
                             episode_attempt_token_t *out_token) {
    if (!state || index < 0 || index >= EPISODE_COUNT ||
        state->watched[index] || state->current >= 0 ||
        state->pending >= 0) {
        return false;
    }

    state->revision = episode_next_revision(state->revision);
    state->pending = index;
    state->pending_token = state->revision;
    state->last_failure = EPISODE_FAILURE_NONE;
    if (out_token) *out_token = state->pending_token;
    return true;
}

bool episode_lifecycle_matches(const episode_lifecycle_t *state, int index,
                               episode_attempt_token_t token) {
    return state && token != 0 && state->pending == index &&
           state->pending_token == token;
}

bool episode_lifecycle_fail(episode_lifecycle_t *state, int index,
                            episode_attempt_token_t token,
                            episode_failure_t failure) {
    if (!episode_lifecycle_matches(state, index, token)) return false;
    if (failure == EPISODE_FAILURE_NONE) return false;

    state->pending = -1;
    state->pending_token = 0;
    state->last_failure = failure;
    return true;
}

bool episode_lifecycle_start(episode_lifecycle_t *state, int index,
                             episode_attempt_token_t token) {
    if (!episode_lifecycle_matches(state, index, token)) return false;

    state->watched[index] = true;
    state->current = index;
    state->pending = -1;
    state->pending_token = 0;
    state->last_failure = EPISODE_FAILURE_NONE;
    return true;
}

bool episode_lifecycle_stop_started(episode_lifecycle_t *state) {
    if (!state || state->current < 0 ||
        state->current >= EPISODE_COUNT) {
        return false;
    }
    state->current = -1;
    state->revision = episode_next_revision(state->revision);
    return true;
}

void episode_lifecycle_reset(episode_lifecycle_t *state) {
    if (!state) return;
    state->current = -1;
    state->pending = -1;
    state->pending_token = 0;
    state->last_failure = EPISODE_FAILURE_NONE;
    state->revision = episode_next_revision(state->revision);
}
