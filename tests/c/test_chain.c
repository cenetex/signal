/* Authenticated, bounded signal-channel persistence tests. */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#include "test_harness.h"
#include "game_sim.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static char *enter_scratch_dir(const char *label) {
    char *prev = getcwd(NULL, 0);
    char tmpl[128];
    snprintf(tmpl, sizeof(tmpl), "%s", TMP("signal_chain_test_XXXXXX"));
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        tests_failed++;
        printf("FAIL: mkdtemp for %s\n", label);
        free(prev);
        return NULL;
    }
    if (chdir(dir) != 0) {
        tests_failed++;
        printf("FAIL: chdir %s\n", dir);
        free(prev);
        return NULL;
    }
    signal_chain_set_disk_enabled(true);
    signal_chain_test_set_write_failure(false);
    return prev;
}

static void leave_scratch_dir(char *prev) {
    if (!prev) return;
    signal_chain_test_set_write_failure(false);
    signal_chain_set_disk_enabled(false);
    int rc = system("rm -rf chain");
    (void)rc;
    int chdir_rc = chdir(prev);
    (void)chdir_rc;
    free(prev);
}

static void post_messages(world_t *w, int count) {
    for (int i = 0; i < count; i++) {
        char text[32];
        snprintf(text, sizeof(text), "message-%d", i + 1);
        w->time = (float)(i + 1);
        ASSERT(signal_channel_post(w, i % 4, text, "") ==
               (uint64_t)(i + 1));
    }
}

TEST(test_signal_chain_load_returns_on_null_world) {
    signal_chain_load(NULL);
}

TEST(test_signal_chain_load_no_chain_dir_returns_silently) {
    char *prev = enter_scratch_dir("no_dir");
    if (!prev) return;
    WORLD_DECL;
    signal_chain_load(&w);
    ASSERT_EQ_INT(w.signal_channel.count, 0);
    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_accepts_verified_ordered_feed) {
    char *prev = enter_scratch_dir("verified");
    if (!prev) return;
    WORLD_DECL_NAME(source);
    post_messages(&source, 6);

    WORLD_DECL_NAME(loaded);
    signal_chain_load(&loaded);
    ASSERT_EQ_INT(loaded.signal_channel.count, 6);
    ASSERT(loaded.signal_channel.next_id == 6);
    for (int i = 0; i < 6; i++) {
        const signal_channel_msg_t *m = signal_channel_at(&loaded, i);
        ASSERT(m != NULL);
        ASSERT(m->id == (uint64_t)(i + 1));
    }
    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_is_bounded_to_ring_capacity) {
    char *prev = enter_scratch_dir("bounded");
    if (!prev) return;
    WORLD_DECL_NAME(source);
    const int total = SIGNAL_CHANNEL_CAPACITY + 20;
    post_messages(&source, total);

    WORLD_DECL_NAME(loaded);
    signal_chain_load(&loaded);
    ASSERT_EQ_INT(loaded.signal_channel.count, SIGNAL_CHANNEL_CAPACITY);
    ASSERT(loaded.signal_channel.next_id == (uint64_t)total);
    const signal_channel_msg_t *first = signal_channel_at(&loaded, 0);
    const signal_channel_msg_t *last =
        signal_channel_at(&loaded, SIGNAL_CHANNEL_CAPACITY - 1);
    ASSERT(first && first->id == 21);
    ASSERT(last && last->id == (uint64_t)total);
    ASSERT(signal_channel_at(&loaded, -1) == NULL);
    ASSERT(signal_channel_at(&loaded, SIGNAL_CHANNEL_CAPACITY) == NULL);
    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_rejects_tampered_content) {
    char *prev = enter_scratch_dir("tamper");
    if (!prev) return;
    WORLD_DECL_NAME(source);
    source.time = 1.0f;
    ASSERT(signal_channel_post(&source, 0, "voice", "https://audio") == 1);

    FILE *f = fopen("chain/signal-channel.v2", "rb+");
    ASSERT(f != NULL);
    ASSERT(fseek(f, 256, SEEK_SET) == 0);
    int byte = fgetc(f);
    ASSERT(byte != EOF);
    ASSERT(fseek(f, 256, SEEK_SET) == 0);
    ASSERT(fputc(byte ^ 1, f) != EOF);
    ASSERT(fclose(f) == 0);

    WORLD_DECL_NAME(loaded);
    signal_chain_load(&loaded);
    ASSERT_EQ_INT(loaded.signal_channel.count, 0);
    ASSERT(loaded.signal_channel.next_id == 0);
    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_rejects_partial_record) {
    char *prev = enter_scratch_dir("partial");
    if (!prev) return;
    ASSERT(mkdir("chain", 0777) == 0);
    FILE *f = fopen("chain/signal-channel.v2", "wb");
    ASSERT(f != NULL);
    ASSERT(fputc(1, f) != EOF);
    ASSERT(fclose(f) == 0);

    WORLD_DECL;
    signal_chain_load(&w);
    ASSERT_EQ_INT(w.signal_channel.count, 0);
    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_ignores_unverifiable_legacy_files) {
    char *prev = enter_scratch_dir("legacy");
    if (!prev) return;
    ASSERT(mkdir("chain", 0777) == 0);
    signal_channel_msg_t legacy = {.id = 99};
    FILE *f = fopen("chain/0.chain", "wb");
    ASSERT(f != NULL);
    ASSERT(fwrite(&legacy, sizeof(legacy), 1, f) == 1);
    ASSERT(fclose(f) == 0);

    WORLD_DECL;
    signal_chain_load(&w);
    ASSERT_EQ_INT(w.signal_channel.count, 0);
    leave_scratch_dir(prev);
}

TEST(test_signal_channel_post_continues_verified_tail) {
    char *prev = enter_scratch_dir("continue");
    if (!prev) return;
    WORLD_DECL_NAME(source);
    post_messages(&source, 7);

    WORLD_DECL_NAME(loaded);
    signal_chain_load(&loaded);
    loaded.time = 8.0f;
    ASSERT(signal_channel_post(&loaded, 0, "next", "") == 8);
    ASSERT_EQ_INT(loaded.signal_channel.count, 8);

    WORLD_DECL_NAME(reloaded);
    signal_chain_load(&reloaded);
    ASSERT(reloaded.signal_channel.next_id == 8);
    const signal_channel_msg_t *last = signal_channel_at(&reloaded, 7);
    ASSERT(last && strcmp(last->text, "next") == 0);
    leave_scratch_dir(prev);
}

TEST(test_signal_channel_failed_write_is_inert) {
    char *prev = enter_scratch_dir("write_failure");
    if (!prev) return;
    WORLD_DECL;
    signal_channel_t before = w.signal_channel;
    signal_chain_test_set_write_failure(true);
    ASSERT(signal_channel_post(&w, 0, "must not publish", "") == 0);
    ASSERT(memcmp(&before, &w.signal_channel, sizeof(before)) == 0);
    ASSERT(access("chain/signal-channel.v2", F_OK) != 0);
    leave_scratch_dir(prev);
}

#endif

void register_signal_chain_tests(void) {
#ifndef _WIN32
    TEST_SECTION("\nSignal chain replay:\n");
    RUN(test_signal_chain_load_returns_on_null_world);
    RUN(test_signal_chain_load_no_chain_dir_returns_silently);
    RUN(test_signal_chain_load_accepts_verified_ordered_feed);
    RUN(test_signal_chain_load_is_bounded_to_ring_capacity);
    RUN(test_signal_chain_load_rejects_tampered_content);
    RUN(test_signal_chain_load_rejects_partial_record);
    RUN(test_signal_chain_load_ignores_unverifiable_legacy_files);
    RUN(test_signal_channel_post_continues_verified_tail);
    RUN(test_signal_channel_failed_write_is_inert);
#endif
}
