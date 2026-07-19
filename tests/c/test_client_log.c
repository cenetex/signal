#include "test_harness.h"
#include "client_log.h"

static long client_log_test_file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long size = ftell(file);
    fclose(file);
    return size;
}

TEST(test_client_log_keeps_file_below_rotation_limit) {
    char path[256];
    snprintf(path, sizeof(path), "%s", TMP("client-log-small.log"));
    FILE *file = fopen(path, "wb");
    ASSERT(file != NULL);
    ASSERT_EQ_INT((int)fwrite("telemetry\n", 1, 10, file), 10);
    ASSERT_EQ_INT(fclose(file), 0);

    ASSERT(client_log_rotate_file(path, 64));
    ASSERT_EQ_INT((int)client_log_test_file_size(path), 10);
    remove(path);
}

TEST(test_client_log_rotates_full_file_to_single_backup) {
    char path[256];
    char backup[260];
    snprintf(path, sizeof(path), "%s", TMP("client-log-full.log"));
    snprintf(backup, sizeof(backup), "%s.1", path);
    FILE *file = fopen(path, "wb");
    ASSERT(file != NULL);
    ASSERT_EQ_INT((int)fwrite("0123456789abcdef", 1, 16, file), 16);
    ASSERT_EQ_INT(fclose(file), 0);

    ASSERT(client_log_rotate_file(path, 16));
    ASSERT_EQ_INT((int)client_log_test_file_size(path), -1);
    ASSERT_EQ_INT((int)client_log_test_file_size(backup), 16);
    remove(backup);
}

void register_client_log_tests(void) {
    TEST_SECTION("\nClient persistent log tests:\n");
    RUN(test_client_log_keeps_file_below_rotation_limit);
    RUN(test_client_log_rotates_full_file_to_single_backup);
}
