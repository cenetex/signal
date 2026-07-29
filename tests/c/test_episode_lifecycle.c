#include "test_harness.h"
#include "episode_lifecycle.h"
#include "episode_media.h"

TEST(test_episode_missing_file_failure_stays_retryable) {
    episode_lifecycle_t state;
    episode_attempt_token_t first = 0;
    episode_attempt_token_t retry = 0;
    episode_lifecycle_init(&state);

    ASSERT(episode_lifecycle_begin(&state, 2, &first));
    ASSERT(first != 0);
    ASSERT_EQ_INT(state.pending, 2);
    ASSERT(!state.watched[2]);

    size_t file_size = 99;
    episode_failure_t failure = EPISODE_FAILURE_NONE;
    uint8_t *file_data = episode_media_read_file(
        TMP("episode-media-definitely-missing.mpg"),
        &file_size, &failure);
    ASSERT(file_data == NULL);
    ASSERT_EQ_INT((int)file_size, 0);
    ASSERT_EQ_INT(failure, EPISODE_FAILURE_FILE_READ);
    ASSERT(episode_lifecycle_fail(&state, 2, first, failure));
    ASSERT_EQ_INT(state.pending, -1);
    ASSERT(!state.watched[2]);
    ASSERT_EQ_INT(state.last_failure, EPISODE_FAILURE_FILE_READ);

    ASSERT(episode_lifecycle_begin(&state, 2, &retry));
    ASSERT(retry != 0);
    ASSERT(retry != first);
    ASSERT(!state.watched[2]);
}

TEST(test_episode_invalid_decoder_routes_failure_and_stays_retryable) {
    episode_lifecycle_t state;
    episode_attempt_token_t first = 0;
    episode_attempt_token_t retry = 0;
    episode_lifecycle_init(&state);
    ASSERT(episode_lifecycle_begin(&state, 3, &first));

    uint8_t *invalid_mpeg = (uint8_t *)malloc(64);
    ASSERT(invalid_mpeg != NULL);
    memset(invalid_mpeg, 0xA5, 64);
    episode_failure_t failure = EPISODE_FAILURE_NONE;
    void *decoder = episode_media_create_decoder(
        invalid_mpeg, 64, &failure);
    ASSERT(decoder == NULL);
    ASSERT_EQ_INT(failure, EPISODE_FAILURE_DECODER);
    ASSERT(episode_lifecycle_fail(&state, 3, first, failure));
    ASSERT_EQ_INT(state.pending, -1);
    ASSERT(!state.watched[3]);

    ASSERT(episode_lifecycle_begin(&state, 3, &retry));
    ASSERT(retry != first);
    ASSERT(episode_lifecycle_start(&state, 3, retry));
    ASSERT(state.watched[3]);
}

TEST(test_episode_startup_failures_do_not_commit_watched_state) {
    static const episode_failure_t failures[] = {
        EPISODE_FAILURE_ALLOCATION,
        EPISODE_FAILURE_DECODER,
        EPISODE_FAILURE_TEXTURE,
    };
    episode_lifecycle_t state;
    episode_lifecycle_init(&state);

    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        episode_attempt_token_t token = 0;
        ASSERT(episode_lifecycle_begin(&state, 4, &token));
        ASSERT(!state.watched[4]);
        ASSERT(episode_lifecycle_fail(&state, 4, token, failures[i]));
        ASSERT(!state.watched[4]);
        ASSERT_EQ_INT(state.pending, -1);
        ASSERT_EQ_INT(state.last_failure, failures[i]);
    }

    episode_attempt_token_t success = 0;
    ASSERT(episode_lifecycle_begin(&state, 4, &success));
    ASSERT(episode_lifecycle_start(&state, 4, success));
    ASSERT(state.watched[4]);
    ASSERT_EQ_INT(state.current, 4);
    ASSERT_EQ_INT(state.pending, -1);
    ASSERT(!episode_lifecycle_begin(&state, 4, NULL));
}

TEST(test_episode_end_before_first_frame_is_retryable_decoder_failure) {
    episode_lifecycle_t state;
    episode_attempt_token_t ended = 0;
    episode_attempt_token_t retry = 0;
    episode_lifecycle_init(&state);

    ASSERT(episode_lifecycle_begin(&state, 5, &ended));
    /* Mirrors episode_upload_frame when plm_has_ended() wins before start. */
    ASSERT(episode_lifecycle_fail(&state, 5, ended,
                                  EPISODE_FAILURE_DECODER));
    ASSERT(!state.watched[5]);
    ASSERT_EQ_INT(state.pending, -1);

    ASSERT(episode_lifecycle_begin(&state, 5, &retry));
    ASSERT(retry != ended);
    ASSERT(episode_lifecycle_start(&state, 5, retry));
    ASSERT(state.watched[5]);
}

TEST(test_episode_startup_failure_wins_over_later_frame_callback) {
    episode_lifecycle_t state;
    episode_attempt_token_t failed = 0;
    episode_attempt_token_t retry = 0;
    episode_lifecycle_init(&state);

    ASSERT(episode_lifecycle_begin(&state, 5, &failed));
    /*
     * plm_decode() can emit multiple video callbacks in one update. The
     * failure transition must happen before decoder teardown so a later
     * callback from that same update cannot commit the failed attempt.
     */
    ASSERT(episode_lifecycle_fail(&state, 5, failed,
                                  EPISODE_FAILURE_TEXTURE));
    ASSERT(!episode_lifecycle_start(&state, 5, failed));
    ASSERT(!state.watched[5]);
    ASSERT_EQ_INT(state.pending, -1);

    ASSERT(episode_lifecycle_begin(&state, 5, &retry));
    ASSERT(retry != failed);
    ASSERT(episode_lifecycle_start(&state, 5, retry));
    ASSERT(state.watched[5]);
}

TEST(test_episode_explicit_skip_only_applies_after_playback_start) {
    episode_lifecycle_t state;
    episode_attempt_token_t token = 0;
    episode_lifecycle_init(&state);

    ASSERT(episode_lifecycle_begin(&state, 6, &token));
    ASSERT(!episode_lifecycle_stop_started(&state));
    ASSERT_EQ_INT(state.pending, 6);
    ASSERT(!state.watched[6]);

    ASSERT(episode_lifecycle_start(&state, 6, token));
    ASSERT(state.watched[6]);
    ASSERT(episode_lifecycle_stop_started(&state));
    ASSERT_EQ_INT(state.current, -1);
    ASSERT(state.watched[6]);
    ASSERT(!episode_lifecycle_begin(&state, 6, NULL));
}

TEST(test_episode_stale_callbacks_cannot_commit_or_clear_new_attempt) {
    episode_lifecycle_t state;
    episode_attempt_token_t stale = 0;
    episode_attempt_token_t current = 0;
    episode_lifecycle_init(&state);

    ASSERT(episode_lifecycle_begin(&state, 1, &stale));
    episode_lifecycle_reset(&state);
    ASSERT(episode_lifecycle_begin(&state, 8, &current));
    ASSERT(current != stale);

    ASSERT(!episode_lifecycle_start(&state, 1, stale));
    ASSERT(!episode_lifecycle_fail(&state, 1, stale,
                                   EPISODE_FAILURE_FETCH));
    ASSERT_EQ_INT(state.pending, 8);
    ASSERT(!state.watched[1]);
    ASSERT(!state.watched[8]);

    ASSERT(episode_lifecycle_start(&state, 8, current));
    ASSERT(!state.watched[1]);
    ASSERT(state.watched[8]);
    ASSERT_EQ_INT(state.current, 8);
}

void register_episode_lifecycle_tests(void) {
    TEST_SECTION("\nEpisode playback lifecycle tests:\n");
    RUN(test_episode_missing_file_failure_stays_retryable);
    RUN(test_episode_invalid_decoder_routes_failure_and_stays_retryable);
    RUN(test_episode_startup_failures_do_not_commit_watched_state);
    RUN(test_episode_end_before_first_frame_is_retryable_decoder_failure);
    RUN(test_episode_startup_failure_wins_over_later_frame_callback);
    RUN(test_episode_explicit_skip_only_applies_after_playback_start);
    RUN(test_episode_stale_callbacks_cannot_commit_or_clear_new_attempt);
}
