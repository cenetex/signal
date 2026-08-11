/* Shared durable-file and CRC primitives for server persistence. */
#ifndef SIGNAL_PERSISTENCE_IO_H
#define SIGNAL_PERSISTENCE_IO_H

#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static inline uint32_t persistence_crc32_update(uint32_t crc,
                                                const void *buf,
                                                size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline uint32_t persistence_crc32_file(FILE *f) {
    if (!f) return 0;
    uint32_t crc = 0;
    long start = ftell(f);
    if (start < 0 || fseek(f, 0, SEEK_SET) != 0) return 0;
    uint8_t chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        crc = persistence_crc32_update(crc, chunk, n);
    (void)fseek(f, start, SEEK_SET);
    return crc;
}

static inline bool persistence_crc32_file_prefix(FILE *f, long end,
                                                 uint32_t *out_crc) {
    if (!f || !out_crc || end < 0) return false;
    uint32_t crc = 0;
    long start = ftell(f);
    if (start < 0 || fseek(f, 0, SEEK_SET) != 0) return false;
    uint8_t chunk[4096];
    long remaining = end;
    while (remaining > 0) {
        size_t want = remaining < (long)sizeof(chunk)
                    ? (size_t)remaining : sizeof(chunk);
        size_t n = fread(chunk, 1, want, f);
        if (n == 0) {
            (void)fseek(f, start, SEEK_SET);
            return false;
        }
        crc = persistence_crc32_update(crc, chunk, n);
        remaining -= (long)n;
    }
    if (fseek(f, start, SEEK_SET) != 0) return false;
    *out_crc = crc;
    return true;
}

static inline bool persistence_flush_durable(FILE *f) {
    if (!f || fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static inline bool persistence_sync_parent_dir(const char *path) {
#ifdef _WIN32
    (void)path;
    return true;
#else
    if (!path || !path[0]) return false;
    char dir[512];
    int n = snprintf(dir, sizeof(dir), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return false;
    char *slash = strrchr(dir, '/');
    if (slash) {
        if (slash == dir) slash[1] = '\0';
        else *slash = '\0';
    } else {
        (void)snprintf(dir, sizeof(dir), ".");
    }
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}

static inline bool persistence_replace_file(const char *tmp_path,
                                            const char *final_path) {
    if (!tmp_path || !final_path) return false;
#ifdef _WIN32
    return MoveFileExA(tmp_path, final_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0 &&
           persistence_sync_parent_dir(final_path);
#else
    if (rename(tmp_path, final_path) != 0) return false;
    return persistence_sync_parent_dir(final_path);
#endif
}

typedef enum {
    PERSISTENCE_PUBLISH_NO_REPLACE_OK = 0,
    PERSISTENCE_PUBLISH_NO_REPLACE_CONFLICT,
    PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE,
} persistence_publish_no_replace_result_t;

/*
 * Publish a fully flushed temporary file without ever replacing an existing
 * final name. POSIX link(2) and Windows MoveFileEx without
 * MOVEFILE_REPLACE_EXISTING both make the absence check part of the atomic
 * namespace operation.
 */
static inline persistence_publish_no_replace_result_t
persistence_publish_file_no_replace(const char *tmp_path,
                                    const char *final_path) {
    if (!tmp_path || !final_path)
        return PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE;
#ifdef _WIN32
    if (!MoveFileExA(tmp_path, final_path, MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_EXISTS ||
               error == ERROR_ALREADY_EXISTS
            ? PERSISTENCE_PUBLISH_NO_REPLACE_CONFLICT
            : PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE;
    }
    return persistence_sync_parent_dir(final_path)
        ? PERSISTENCE_PUBLISH_NO_REPLACE_OK
        : PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE;
#else
    if (link(tmp_path, final_path) != 0) {
        return errno == EEXIST
            ? PERSISTENCE_PUBLISH_NO_REPLACE_CONFLICT
            : PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE;
    }
    if (unlink(tmp_path) != 0 ||
        !persistence_sync_parent_dir(final_path)) {
        return PERSISTENCE_PUBLISH_NO_REPLACE_IO_FAILURE;
    }
    return PERSISTENCE_PUBLISH_NO_REPLACE_OK;
#endif
}

#endif
