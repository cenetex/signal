/* Layer A.1 of #479 — verify save/load of player_identity_t through a
 * filesystem path and corruption recovery (.bad rename). */
#include "test_harness.h"

#include <errno.h>
#include <stdio.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "identity.h"
#include "signal_crypto.h"

static void identity_remove_lock_file(const char *path) {
    char lock_path[1024];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    remove(lock_path);
}

static bool identity_entropy_fail_after_partial_write(
    uint8_t *buf, size_t len, void *user) {
    (void)user;
    size_t partial = len < 9 ? len : 9;
    if (buf && partial > 0) memset(buf, 0xA7, partial);
    return false;
}

TEST(test_identity_save_then_load) {
    const char *path = TMP("identity_roundtrip.key");
    remove(path); /* ensure fresh-generate path */
    identity_remove_lock_file(path);

    player_identity_t a;
    ASSERT(identity_load_or_generate_at(&a, path));

    /* Pubkey is the trailing 32 bytes of secret (NaCl convention). */
    ASSERT(memcmp(a.pubkey,
                  a.secret + (SIGNAL_CRYPTO_SECRET_BYTES - SIGNAL_CRYPTO_PUBKEY_BYTES),
                  SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);

    player_identity_t b;
    ASSERT(identity_load_or_generate_at(&b, path));

    ASSERT(memcmp(a.pubkey, b.pubkey, SIGNAL_CRYPTO_PUBKEY_BYTES) == 0);
    ASSERT(memcmp(a.secret, b.secret, SIGNAL_CRYPTO_SECRET_BYTES) == 0);

    /* Round-trip the keypair through sign/verify so we know the persisted
     * secret is functional, not just bit-equal. */
    uint8_t msg[16];
    for (int i = 0; i < 16; i++) msg[i] = (uint8_t)i;
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), b.secret);
    ASSERT(signal_crypto_verify(sig, msg, sizeof(msg), b.pubkey));

    remove(path);
    identity_remove_lock_file(path);
}

TEST(test_identity_corrupt_file_renamed_to_bad) {
    const char *path = TMP("identity_corrupt.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);
    identity_remove_lock_file(path);

    /* Write a non-64-byte file: clearly corrupt. */
    FILE *fp = fopen(path, "wb");
    ASSERT(fp != NULL);
    const char garbage[] = "this-is-not-a-keypair";
    fwrite(garbage, 1, sizeof(garbage) - 1, fp);
    fclose(fp);

    player_identity_t id;
    ASSERT(identity_load_or_generate_at(&id, path));

    /* The original file should have been renamed to .bad and a fresh
     * 64-byte key file written in its place. */
    fp = fopen(bad_path, "rb");
    ASSERT(fp != NULL);
    uint8_t bad_bytes[sizeof(garbage)] = {0};
    ASSERT(fread(bad_bytes, 1, sizeof(garbage) - 1, fp) ==
           sizeof(garbage) - 1);
    ASSERT(fgetc(fp) == EOF);
    fclose(fp);
    ASSERT(memcmp(bad_bytes, garbage, sizeof(garbage) - 1) == 0);

    fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    uint8_t buf[SIGNAL_CRYPTO_SECRET_BYTES + 1];
    size_t got = fread(buf, 1, sizeof(buf), fp);
    int eof = feof(fp);
    fclose(fp);
    ASSERT(got == SIGNAL_CRYPTO_SECRET_BYTES);
    ASSERT(eof);

    /* And the in-memory identity should be a usable, regenerated keypair. */
    uint8_t msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t sig[SIGNAL_CRYPTO_SIG_BYTES];
    signal_crypto_sign(sig, msg, sizeof(msg), id.secret);
    ASSERT(signal_crypto_verify(sig, msg, sizeof(msg), id.pubkey));

    remove(path);
    remove(bad_path);
    identity_remove_lock_file(path);
}

TEST(test_identity_entropy_failure_does_not_create_file) {
    const char *path = TMP("identity_entropy_failure.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);
    identity_remove_lock_file(path);

    player_identity_t id;
    memset(&id, 0xCC, sizeof(id));
    player_identity_t zero = {0};

    signal_crypto_test_set_entropy_provider(
        identity_entropy_fail_after_partial_write, NULL);
    bool ok = identity_load_or_generate_at(&id, path);
    signal_crypto_test_reset_entropy_provider();

    ASSERT(!ok);
    ASSERT(memcmp(&id, &zero, sizeof(id)) == 0);
    FILE *fp = fopen(path, "rb");
    ASSERT(fp == NULL);
    fp = fopen(bad_path, "rb");
    ASSERT(fp == NULL);
    identity_remove_lock_file(path);
}

TEST(test_identity_entropy_failure_preserves_corrupt_file) {
    const char *path = TMP("identity_entropy_preserve_corrupt.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);
    identity_remove_lock_file(path);

    const uint8_t original[] = {0x53, 0x49, 0x47, 0x4E, 0x41, 0x4C};
    FILE *fp = fopen(path, "wb");
    ASSERT(fp != NULL);
    ASSERT(fwrite(original, 1, sizeof(original), fp) == sizeof(original));
    fclose(fp);

    player_identity_t id;
    memset(&id, 0xCC, sizeof(id));
    player_identity_t zero = {0};
    signal_crypto_test_set_entropy_provider(
        identity_entropy_fail_after_partial_write, NULL);
    bool ok = identity_load_or_generate_at(&id, path);
    signal_crypto_test_reset_entropy_provider();

    ASSERT(!ok);
    ASSERT(memcmp(&id, &zero, sizeof(id)) == 0);
    fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    uint8_t got[sizeof(original)] = {0};
    ASSERT(fread(got, 1, sizeof(got), fp) == sizeof(got));
    ASSERT(fgetc(fp) == EOF);
    fclose(fp);
    ASSERT(memcmp(got, original, sizeof(got)) == 0);
    fp = fopen(bad_path, "rb");
    ASSERT(fp == NULL);

    remove(path);
    identity_remove_lock_file(path);
}

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
TEST(test_identity_io_error_does_not_replace_path) {
    const char *path = TMP("identity_io_error.key");
    char bad_path[1024];
    char lock_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    remove(path);
    remove(bad_path);
    remove(lock_path);
    (void)rmdir(path);
    ASSERT(mkdir(path, 0700) == 0);

    player_identity_t identity;
    memset(&identity, 0xCC, sizeof(identity));
    const player_identity_t zero = {0};
    ASSERT(!identity_load_or_generate_at(&identity, path));
    ASSERT(memcmp(&identity, &zero, sizeof(identity)) == 0);

    struct stat info;
    ASSERT(stat(path, &info) == 0);
    ASSERT(S_ISDIR(info.st_mode));
    FILE *bad = fopen(bad_path, "rb");
    ASSERT(bad == NULL);

    ASSERT(rmdir(path) == 0);
    remove(lock_path);
}

TEST(test_identity_symlink_path_is_rejected_without_replacement) {
    const char *path = TMP("identity_symlink.key");
    const char *target = TMP("identity_symlink_target.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(target);
    remove(bad_path);
    identity_remove_lock_file(path);

    const uint8_t original[] = {
        0x53, 0x49, 0x47, 0x4E, 0x41, 0x4C, 0x2D, 0x53,
        0x59, 0x4D, 0x4C, 0x49, 0x4E, 0x4B,
    };
    FILE *source = fopen(target, "wb");
    ASSERT(source != NULL);
    ASSERT(fwrite(original, 1, sizeof(original), source) ==
           sizeof(original));
    ASSERT(fclose(source) == 0);
    ASSERT(symlink(target, path) == 0);

    player_identity_t identity;
    memset(&identity, 0xCC, sizeof(identity));
    const player_identity_t zero = {0};
    ASSERT(!identity_load_or_generate_at(&identity, path));
    ASSERT(memcmp(&identity, &zero, sizeof(identity)) == 0);

    struct stat info;
    ASSERT(lstat(path, &info) == 0);
    ASSERT(S_ISLNK(info.st_mode));
    source = fopen(target, "rb");
    ASSERT(source != NULL);
    uint8_t preserved[sizeof(original)] = {0};
    ASSERT(fread(preserved, 1, sizeof(preserved), source) ==
           sizeof(preserved));
    ASSERT(fgetc(source) == EOF);
    ASSERT(fclose(source) == 0);
    ASSERT(memcmp(preserved, original, sizeof(original)) == 0);
    ASSERT(access(bad_path, F_OK) != 0);

    remove(path);
    remove(target);
    identity_remove_lock_file(path);
}

TEST(test_identity_backup_failure_preserves_corrupt_path) {
    const char *path = TMP("identity_backup_failure.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    (void)rmdir(bad_path);
    remove(bad_path);
    identity_remove_lock_file(path);

    const uint8_t original[] = {
        0x53, 0x49, 0x47, 0x4E, 0x41, 0x4C, 0x2D, 0x42,
        0x41, 0x43, 0x4B, 0x55, 0x50,
    };
    FILE *source = fopen(path, "wb");
    ASSERT(source != NULL);
    ASSERT(fwrite(original, 1, sizeof(original), source) ==
           sizeof(original));
    ASSERT(fclose(source) == 0);
    ASSERT(mkdir(bad_path, 0700) == 0);

    player_identity_t identity;
    memset(&identity, 0xCC, sizeof(identity));
    const player_identity_t zero = {0};
    ASSERT(!identity_load_or_generate_at(&identity, path));
    ASSERT(memcmp(&identity, &zero, sizeof(identity)) == 0);

    source = fopen(path, "rb");
    ASSERT(source != NULL);
    uint8_t preserved[sizeof(original)] = {0};
    ASSERT(fread(preserved, 1, sizeof(preserved), source) ==
           sizeof(preserved));
    ASSERT(fgetc(source) == EOF);
    ASSERT(fclose(source) == 0);
    ASSERT(memcmp(preserved, original, sizeof(original)) == 0);

    struct stat info;
    ASSERT(stat(bad_path, &info) == 0);
    ASSERT(S_ISDIR(info.st_mode));

    remove(path);
    ASSERT(rmdir(bad_path) == 0);
    identity_remove_lock_file(path);
}

typedef struct {
    uint8_t ok;
    uint8_t pubkey[SIGNAL_CRYPTO_PUBKEY_BYTES];
} identity_child_result_t;

static bool identity_read_exact(int fd, void *out, size_t size) {
    uint8_t *bytes = out;
    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(fd, bytes + offset, size - offset);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        offset += (size_t)got;
    }
    return true;
}

typedef struct {
    int start_fd;
    const char *path;
    identity_child_result_t result;
} identity_thread_case_t;

static void *identity_thread_first_launch(void *opaque) {
    identity_thread_case_t *test_case = opaque;
    uint8_t release = 0;
    if (!test_case ||
        !identity_read_exact(
            test_case->start_fd, &release, sizeof(release))) {
        return NULL;
    }
    player_identity_t identity = {0};
    test_case->result.ok =
        identity_load_or_generate_at(
            &identity, test_case->path) ? 1u : 0u;
    if (test_case->result.ok) {
        memcpy(test_case->result.pubkey, identity.pubkey,
               sizeof(test_case->result.pubkey));
    }
    return NULL;
}

TEST(test_identity_concurrent_threads_converge) {
    enum { IDENTITY_THREAD_COUNT = 8 };
    const char *path = TMP("identity_thread_concurrent.key");
    char bad_path[1024];
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(bad_path);
    identity_remove_lock_file(path);

    int start[2];
    ASSERT(pipe(start) == 0);
    pthread_t threads[IDENTITY_THREAD_COUNT];
    identity_thread_case_t cases[IDENTITY_THREAD_COUNT];
    memset(cases, 0, sizeof(cases));
    for (int i = 0; i < IDENTITY_THREAD_COUNT; i++) {
        cases[i].start_fd = start[0];
        cases[i].path = path;
        ASSERT(pthread_create(
                   &threads[i], NULL,
                   identity_thread_first_launch,
                   &cases[i]) == 0);
    }
    const uint8_t release[IDENTITY_THREAD_COUNT] = {
        1, 1, 1, 1, 1, 1, 1, 1,
    };
    ASSERT(write(start[1], release, sizeof(release)) ==
           (ssize_t)sizeof(release));
    close(start[1]);
    for (int i = 0; i < IDENTITY_THREAD_COUNT; i++) {
        ASSERT(pthread_join(threads[i], NULL) == 0);
    }
    close(start[0]);

    ASSERT(cases[0].result.ok);
    for (int i = 1; i < IDENTITY_THREAD_COUNT; i++) {
        ASSERT(cases[i].result.ok);
        ASSERT(memcmp(
                   cases[0].result.pubkey,
                   cases[i].result.pubkey,
                   sizeof(cases[i].result.pubkey)) == 0);
    }
    player_identity_t persisted = {0};
    ASSERT(identity_load_or_generate_at(&persisted, path));
    ASSERT(memcmp(
               cases[0].result.pubkey, persisted.pubkey,
               sizeof(persisted.pubkey)) == 0);

    remove(path);
    remove(bad_path);
    identity_remove_lock_file(path);
}

TEST(test_identity_concurrent_first_launch_converges) {
    const char *path = TMP("identity_concurrent.key");
    char lock_path[1024];
    char bad_path[1024];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    remove(path);
    remove(lock_path);
    remove(bad_path);

    int start[2];
    int results[2];
    ASSERT(pipe(start) == 0);
    ASSERT(pipe(results) == 0);
    pid_t children[2] = {-1, -1};
    for (int i = 0; i < 2; i++) {
        children[i] = fork();
        ASSERT(children[i] >= 0);
        if (children[i] == 0) {
            close(start[1]);
            close(results[0]);
            uint8_t release = 0;
            if (!identity_read_exact(
                    start[0], &release, sizeof(release))) {
                _exit(3);
            }
            close(start[0]);
            player_identity_t identity = {0};
            identity_child_result_t result = {0};
            result.ok = identity_load_or_generate_at(&identity, path) ? 1u : 0u;
            if (result.ok) {
                memcpy(result.pubkey, identity.pubkey,
                       sizeof(result.pubkey));
            }
            size_t offset = 0;
            while (offset < sizeof(result)) {
                ssize_t wrote = write(
                    results[1],
                    (const uint8_t *)&result + offset,
                    sizeof(result) - offset);
                if (wrote < 0 && errno == EINTR) continue;
                if (wrote <= 0) _exit(2);
                offset += (size_t)wrote;
            }
            close(results[1]);
            _exit(result.ok ? 0 : 1);
        }
    }
    close(start[0]);
    const uint8_t release[2] = {1, 1};
    ASSERT(write(start[1], release, sizeof(release)) ==
           (ssize_t)sizeof(release));
    close(start[1]);
    close(results[1]);

    identity_child_result_t first = {0};
    identity_child_result_t second = {0};
    ASSERT(identity_read_exact(results[0], &first, sizeof(first)));
    ASSERT(identity_read_exact(results[0], &second, sizeof(second)));
    close(results[0]);
    for (int i = 0; i < 2; i++) {
        int status = 0;
        ASSERT(waitpid(children[i], &status, 0) == children[i]);
        ASSERT(WIFEXITED(status));
        ASSERT_EQ_INT(WEXITSTATUS(status), 0);
    }

    ASSERT(first.ok);
    ASSERT(second.ok);
    ASSERT(memcmp(first.pubkey, second.pubkey,
                  sizeof(first.pubkey)) == 0);

    player_identity_t persisted = {0};
    ASSERT(identity_load_or_generate_at(&persisted, path));
    ASSERT(memcmp(first.pubkey, persisted.pubkey,
                  sizeof(first.pubkey)) == 0);

    remove(path);
    remove(lock_path);
    remove(bad_path);
}
#endif

void register_identity_tests(void);
void register_identity_tests(void) {
    TEST_SECTION("\nIdentity (player keypair) tests:\n");
    RUN(test_identity_save_then_load);
    RUN(test_identity_corrupt_file_renamed_to_bad);
    RUN(test_identity_entropy_failure_does_not_create_file);
    RUN(test_identity_entropy_failure_preserves_corrupt_file);
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    RUN(test_identity_io_error_does_not_replace_path);
    RUN(test_identity_symlink_path_is_rejected_without_replacement);
    RUN(test_identity_backup_failure_preserves_corrupt_path);
    RUN(test_identity_concurrent_threads_converge);
    RUN(test_identity_concurrent_first_launch_converges);
#endif
}
