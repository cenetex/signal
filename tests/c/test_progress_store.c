#include "test_harness.h"
#include "progress_store.h"
#include "story_loop.h"

TEST(test_progress_scope_separates_players_and_authorities)
{
    uint8_t alice[32] = {1};
    uint8_t bob[32] = {2};
    char a[65], same[65], b[65], other[65], local[65];
    ASSERT(client_progress_scope_key(a, alice, "wss://relay.example/ws"));
    ASSERT(client_progress_scope_key(same, alice, "wss://relay.example/ws"));
    ASSERT(strcmp(a, same) == 0);
    ASSERT(client_progress_scope_key(b, bob, "wss://relay.example/ws"));
    ASSERT(client_progress_scope_key(other, alice, "wss://other.example/ws"));
    ASSERT(client_progress_scope_key(local, alice, NULL));
    ASSERT(strcmp(a, b) != 0);
    ASSERT(strcmp(a, other) != 0);
    ASSERT(strcmp(a, local) != 0);
    ASSERT_EQ_INT(strlen(a), 64);
    uint8_t anonymous[32] = {0};
    ASSERT(!client_progress_scope_key(a, anonymous, NULL));
    ASSERT(a[0] == '\0');
}

TEST(test_progress_restart_restores_story_and_guide)
{
    const char *path = TMP("progress-restart.txt");
    remove(path);
    client_progress_t saved = { .story = 31, .guide = 1023 };
    ASSERT(client_progress_write_at(path, &saved));
    client_progress_t loaded = {0};
    ASSERT(client_progress_read_at(path, &loaded));
    ASSERT_EQ_INT(loaded.story, saved.story);
    ASSERT_EQ_INT(loaded.guide, saved.guide);
    worker_story_state_t story = { .flags = loaded.story };
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_REWARD);
    ASSERT(worker_story_mark_outpost_active(&story));
    saved.story = story.flags;
    ASSERT(client_progress_write_at(path, &saved));
    ASSERT(client_progress_read_at(path, &loaded));
    ASSERT_EQ_INT(loaded.story, 63);
    ASSERT_EQ_INT(loaded.guide, 1023);
    remove(path);
}

TEST(test_progress_corruption_preserves_a_known_start)
{
    const char *bad[] = {
        "", "SGP2:ff:3ff\n", "SGP2:01:3f", "SGP1:01:3ff",
        "SGP2:0z:000", "SGP2:02:000", "SGP2:ff:400", "SGP2:ff:fff"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        client_progress_t progress = {255, 1023};
        ASSERT(!client_progress_decode(bad[i], &progress));
        ASSERT_EQ_INT(progress.story, 0);
        ASSERT_EQ_INT(progress.guide, 0);
    }
    const char *path = TMP("progress-corrupt.txt");
    FILE *file = fopen(path, "wb");
    ASSERT(file != NULL);
    ASSERT(fwrite("SGP2:ff:3", 1, 9, file) == 9);
    ASSERT(fclose(file) == 0);
    client_progress_t loaded = {255, 1023};
    ASSERT(!client_progress_read_at(path, &loaded));
    ASSERT_EQ_INT(loaded.story, 0);
    ASSERT_EQ_INT(loaded.guide, 0);
    remove(path);
}

TEST(test_progress_invalid_update_keeps_the_last_saved_record)
{
    const char *path = TMP("progress-preserve.txt");
    client_progress_t saved = {7, 35};
    ASSERT(client_progress_write_at(path, &saved));
    client_progress_t invalid = {2, 35};
    ASSERT(!client_progress_write_at(path, &invalid));
    client_progress_t loaded = {0};
    ASSERT(client_progress_read_at(path, &loaded));
    ASSERT_EQ_INT(loaded.story, saved.story);
    ASSERT_EQ_INT(loaded.guide, saved.guide);
    remove(path);
}

void register_progress_store_tests(void)
{
    RUN(test_progress_scope_separates_players_and_authorities);
    RUN(test_progress_restart_restores_story_and_guide);
    RUN(test_progress_corruption_preserves_a_known_start);
    RUN(test_progress_invalid_update_keeps_the_last_saved_record);
}
