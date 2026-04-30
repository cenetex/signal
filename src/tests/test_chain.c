/* Tests for signal_chain_load — the on-disk chain replay used at
 * server start. Hits the directory walk, the insertion sort, the ring
 * truncation, and the next_id watermark. POSIX-only (the function
 * itself no-ops on Windows, where the server isn't deployed). */
/* mkdtemp lives behind feature-test macros under glibc with -std=c11;
 * declare them before any header pulls in <features.h>. */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#include "tests/test_harness.h"
#include "game_sim.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>

static void write_msg_file(const char *path, const signal_channel_msg_t *msgs, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) { tests_failed++; printf("FAIL: cannot open %s\n", path); return; }
    fwrite(msgs, sizeof(*msgs), (size_t)n, f);
    fclose(f);
}

/* Move into a freshly-created scratch dir, returning the previous cwd
 * so the caller can restore it. Caller frees the result. */
static char *enter_scratch_dir(const char *label) {
    char *prev = getcwd(NULL, 0);
    /* mkdtemp mutates its argument in place, so copy TMP()'s shared
     * buffer into a local mutable array before calling it. */
    char tmpl[128];
    snprintf(tmpl, sizeof(tmpl), "%s", TMP("signal_chain_test_XXXXXX"));
    char *dir = mkdtemp(tmpl);
    if (!dir) { tests_failed++; printf("FAIL: mkdtemp for %s\n", label); free(prev); return NULL; }
    if (chdir(dir) != 0) { tests_failed++; printf("FAIL: chdir %s\n", dir); free(prev); return NULL; }
    mkdir("chain", 0777);
    return prev;
}

static void leave_scratch_dir(char *prev) {
    if (!prev) return;
    /* Best-effort cleanup — leftover scratch dirs in /tmp are harmless,
     * so we ignore the return values explicitly to satisfy
     * -Werror=unused-result on Linux glibc. */
    int rc1 = system("rm -rf chain"); (void)rc1;
    int rc2 = chdir(prev);            (void)rc2;
    free(prev);
}

TEST(test_signal_chain_load_returns_on_null_world) {
    /* The early NULL guard is part of the function's coverage too. */
    signal_chain_load(NULL);
}

TEST(test_signal_chain_load_no_chain_dir_returns_silently) {
    char *prev = enter_scratch_dir("no_dir");
    if (!prev) return;
    /* Remove the dir created by enter_scratch_dir to hit the !dir branch. */
    int rc = system("rm -rf chain"); (void)rc;
    WORLD_DECL;
    signal_chain_load(&w);
    ASSERT_EQ_INT(w.signal_channel.count, 0);
    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_orders_messages_by_id) {
    char *prev = enter_scratch_dir("ordering");
    if (!prev) return;

    /* Two files, ids interleaved on disk to force the sort to do work. */
    signal_channel_msg_t fileA[3] = {
        {.id = 5, .timestamp_ms = 500, .sender_station = 0},
        {.id = 1, .timestamp_ms = 100, .sender_station = 0},
        {.id = 4, .timestamp_ms = 400, .sender_station = 0},
    };
    signal_channel_msg_t fileB[3] = {
        {.id = 3, .timestamp_ms = 300, .sender_station = 1},
        {.id = 2, .timestamp_ms = 200, .sender_station = 1},
        {.id = 6, .timestamp_ms = 600, .sender_station = 1},
    };
    write_msg_file("chain/a.chain", fileA, 3);
    write_msg_file("chain/b.chain", fileB, 3);

    WORLD_DECL;
    signal_chain_load(&w);

    ASSERT_EQ_INT(w.signal_channel.count, 6);
    ASSERT(w.signal_channel.next_id == 6);
    /* Iteration order must be by id ascending. */
    for (int i = 0; i < 6; i++) {
        const signal_channel_msg_t *m = signal_channel_at(&w, i);
        ASSERT(m != NULL);
        ASSERT_EQ_INT((int)m->id, i + 1);
    }

    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_truncates_to_capacity) {
    char *prev = enter_scratch_dir("truncate");
    if (!prev) return;

    /* Write 120 messages with ids 1..120. Loader should keep only the
     * most recent SIGNAL_CHANNEL_CAPACITY (100), i.e. ids 21..120. */
    static const uint64_t TOTAL = 120;
    signal_channel_msg_t *msgs = calloc((size_t)TOTAL, sizeof(*msgs));
    ASSERT(msgs != NULL);
    for (uint64_t i = 0; i < TOTAL; i++) {
        msgs[i].id = i + 1;
        msgs[i].timestamp_ms = (uint32_t)((i + 1) * 10);
        msgs[i].sender_station = (int16_t)(i % 4);
    }
    write_msg_file("chain/all.chain", msgs, (int)TOTAL);
    free(msgs);

    WORLD_DECL;
    signal_chain_load(&w);

    ASSERT_EQ_INT(w.signal_channel.count, SIGNAL_CHANNEL_CAPACITY);
    ASSERT(w.signal_channel.next_id == TOTAL);
    /* Oldest retained should be id=21, newest id=120. */
    const signal_channel_msg_t *first = signal_channel_at(&w, 0);
    const signal_channel_msg_t *last  = signal_channel_at(&w, SIGNAL_CHANNEL_CAPACITY - 1);
    ASSERT(first && first->id == TOTAL - SIGNAL_CHANNEL_CAPACITY + 1);
    ASSERT(last  && last->id  == TOTAL);
    /* Out-of-range index returns NULL. */
    ASSERT(signal_channel_at(&w, -1) == NULL);
    ASSERT(signal_channel_at(&w, SIGNAL_CHANNEL_CAPACITY) == NULL);

    leave_scratch_dir(prev);
}

TEST(test_signal_chain_load_skips_non_chain_files) {
    char *prev = enter_scratch_dir("skip");
    if (!prev) return;

    signal_channel_msg_t valid[1] = { {.id = 7, .timestamp_ms = 700} };
    write_msg_file("chain/keep.chain", valid, 1);
    /* These should be ignored — wrong extension and too-short name. */
    FILE *f1 = fopen("chain/decoy.txt", "wb"); if (f1) { fputs("garbage", f1); fclose(f1); }
    FILE *f2 = fopen("chain/x", "wb");         if (f2) { fputs("garbage", f2); fclose(f2); }

    WORLD_DECL;
    signal_chain_load(&w);
    ASSERT_EQ_INT(w.signal_channel.count, 1);
    ASSERT(w.signal_channel.next_id == 7);

    leave_scratch_dir(prev);
}

#endif /* !_WIN32 */

void register_signal_chain_tests(void) {
#ifndef _WIN32
    TEST_SECTION("\nSignal chain replay:\n");
    RUN(test_signal_chain_load_returns_on_null_world);
    RUN(test_signal_chain_load_no_chain_dir_returns_silently);
    RUN(test_signal_chain_load_orders_messages_by_id);
    RUN(test_signal_chain_load_truncates_to_capacity);
    RUN(test_signal_chain_load_skips_non_chain_files);
#endif
}
