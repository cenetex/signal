/*
 * identity.c -- See identity.h.
 *
 * On-disk format: 64 raw bytes — exactly the secret[64] (seed||pub).
 * The pubkey is recoverable as the trailing 32 bytes; we don't write
 * a separate header so a corrupt file is detectable purely by length.
 */

/* glibc on Linux only declares fchmod / mkdir-with-mode under one of
 * these feature-test macros; without them, -Werror=implicit-function-
 * declaration kills the build (CI failure mode that macOS libc hides
 * because Apple libc declares fchmod by default). */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "identity.h"
#include "signal_memzero.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__EMSCRIPTEN__)
  #include <emscripten.h>
#elif defined(_WIN32)
  #include <windows.h>
  #include <shlobj.h>
  #include <direct.h>
  #include <io.h>
  #define mkdir_compat(p) _mkdir(p)
#else
  #include <fcntl.h>
  #include <pthread.h>
  #include <pwd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define mkdir_compat(p) mkdir((p), 0700)
#endif

/* ---------------------------------------------------------------------
 * Platform path resolution
 * ------------------------------------------------------------------ */

#if !defined(__EMSCRIPTEN__)
static bool resolve_default_path(char *out, size_t cap) {
#if defined(_WIN32)
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        return false;
    }
    int n = snprintf(out, cap, "%s\\signal", base);
    if (n <= 0 || (size_t)n >= cap) return false;
    _mkdir(out);
    char file[MAX_PATH];
    n = snprintf(file, sizeof(file), "%s\\identity.key", out);
    if (n <= 0 || (size_t)n >= sizeof(file)) return false;
    if ((size_t)n >= cap) return false;
    memcpy(out, file, (size_t)n + 1);
    return true;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    if (!home) return false;
    char dir[1024];
    int n = snprintf(dir, sizeof(dir),
                     "%s/Library/Application Support/signal", home);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return false;
    /* mkdir each component we control. Library/Application Support
     * already exists on every macOS install. */
    mkdir(dir, 0700);
    n = snprintf(out, cap, "%s/identity.key", dir);
    return n > 0 && (size_t)n < cap;
#else
    /* XDG. */
    const char *xdg = getenv("XDG_DATA_HOME");
    char fallback[1024];
    if (!xdg || !*xdg) {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : NULL;
        }
        if (!home) return false;
        int n = snprintf(fallback, sizeof(fallback),
                         "%s/.local/share", home);
        if (n <= 0 || (size_t)n >= sizeof(fallback)) return false;
        xdg = fallback;
    }
    char dir[1280];
    int n = snprintf(dir, sizeof(dir), "%s/signal", xdg);
    if (n <= 0 || (size_t)n >= sizeof(dir)) return false;
    /* Best-effort mkdir parents; XDG_DATA_HOME may not exist. */
    char parent[1280];
    snprintf(parent, sizeof(parent), "%s", xdg);
    mkdir(parent, 0700);
    mkdir(dir, 0700);
    n = snprintf(out, cap, "%s/identity.key", dir);
    return n > 0 && (size_t)n < cap;
#endif
}
#endif /* !__EMSCRIPTEN__ */

/* ---------------------------------------------------------------------
 * File I/O — POSIX/Windows path
 * ------------------------------------------------------------------ */

#if !defined(__EMSCRIPTEN__)
typedef struct {
    bool process_lock_held;
#if defined(_WIN32)
    HANDLE handle;
    OVERLAPPED overlapped;
#else
    int fd;
#endif
} identity_path_lock_t;

#if defined(_WIN32)
static SRWLOCK identity_process_lock = SRWLOCK_INIT;
#else
static pthread_mutex_t identity_process_lock =
    PTHREAD_MUTEX_INITIALIZER;
#endif

static bool identity_process_lock_acquire(
    identity_path_lock_t *lock) {
    if (!lock) return false;
#if defined(_WIN32)
    AcquireSRWLockExclusive(&identity_process_lock);
    lock->process_lock_held = true;
    return true;
#else
    if (pthread_mutex_lock(&identity_process_lock) != 0)
        return false;
    lock->process_lock_held = true;
    return true;
#endif
}

static void identity_process_lock_release(
    identity_path_lock_t *lock) {
    if (!lock || !lock->process_lock_held) return;
    lock->process_lock_held = false;
#if defined(_WIN32)
    ReleaseSRWLockExclusive(&identity_process_lock);
#else
    (void)pthread_mutex_unlock(&identity_process_lock);
#endif
}

static bool identity_path_lock_acquire(const char *path,
                                       identity_path_lock_t *lock) {
    if (!path || !lock) return false;
    memset(lock, 0, sizeof(*lock));
#if defined(_WIN32)
    lock->handle = INVALID_HANDLE_VALUE;
#else
    lock->fd = -1;
#endif
    if (!identity_process_lock_acquire(lock)) return false;
    char lock_path[2048];
    int n = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    if (n <= 0 || (size_t)n >= sizeof(lock_path)) {
        identity_process_lock_release(lock);
        return false;
    }
#if defined(_WIN32)
    lock->handle = CreateFileA(
        lock_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN, NULL);
    if (lock->handle == INVALID_HANDLE_VALUE) {
        identity_process_lock_release(lock);
        return false;
    }
    if (!LockFileEx(lock->handle, LOCKFILE_EXCLUSIVE_LOCK, 0,
                    MAXDWORD, MAXDWORD, &lock->overlapped)) {
        CloseHandle(lock->handle);
        lock->handle = INVALID_HANDLE_VALUE;
        identity_process_lock_release(lock);
        return false;
    }
    return true;
#else
    int open_flags = O_CREAT | O_RDWR;
#if defined(O_CLOEXEC)
    open_flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    open_flags |= O_NOFOLLOW;
#endif
    lock->fd = open(lock_path, open_flags, 0600);
    if (lock->fd < 0) {
        identity_process_lock_release(lock);
        return false;
    }
    (void)fchmod(lock->fd, 0600);
    struct flock file_lock = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    int rc;
    do {
        rc = fcntl(lock->fd, F_SETLKW, &file_lock);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        close(lock->fd);
        lock->fd = -1;
        identity_process_lock_release(lock);
        return false;
    }
    return true;
#endif
}

static void identity_path_lock_release(identity_path_lock_t *lock) {
    if (!lock) return;
#if defined(_WIN32)
    if (lock->handle != INVALID_HANDLE_VALUE) {
        (void)UnlockFileEx(lock->handle, 0, MAXDWORD, MAXDWORD,
                           &lock->overlapped);
        CloseHandle(lock->handle);
        lock->handle = INVALID_HANDLE_VALUE;
    }
#else
    if (lock->fd >= 0) {
        struct flock file_lock = {
            .l_type = F_UNLCK,
            .l_whence = SEEK_SET,
            .l_start = 0,
            .l_len = 0,
        };
        (void)fcntl(lock->fd, F_SETLK, &file_lock);
        close(lock->fd);
        lock->fd = -1;
    }
#endif
    identity_process_lock_release(lock);
}

typedef enum {
    IDENTITY_READ_VALID = 0,
    IDENTITY_READ_MISSING,
    IDENTITY_READ_CORRUPT,
    IDENTITY_READ_ERROR,
} identity_read_result_t;

static identity_read_result_t read_secret_file(
    const char *path,
    uint8_t out[SIGNAL_CRYPTO_SECRET_BYTES]) {
#if defined(_WIN32)
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return errno == ENOENT
            ? IDENTITY_READ_MISSING
            : IDENTITY_READ_ERROR;
    }
#else
    struct stat path_info;
    if (lstat(path, &path_info) != 0) {
        return errno == ENOENT
            ? IDENTITY_READ_MISSING
            : IDENTITY_READ_ERROR;
    }
    if (!S_ISREG(path_info.st_mode)) {
        return IDENTITY_READ_ERROR;
    }
    int flags = O_RDONLY;
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return IDENTITY_READ_ERROR;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 ||
        !S_ISREG(info.st_mode) ||
        info.st_dev != path_info.st_dev ||
        info.st_ino != path_info.st_ino) {
        (void)close(fd);
        return IDENTITY_READ_ERROR;
    }
    FILE *fp = fdopen(fd, "rb");
    if (!fp) {
        (void)close(fd);
        return IDENTITY_READ_ERROR;
    }
#endif
    uint8_t buf[SIGNAL_CRYPTO_SECRET_BYTES + 1];
    size_t got = fread(buf, 1, sizeof(buf), fp);
    bool read_error = ferror(fp) != 0;
    bool eof = feof(fp) != 0;
    bool close_error = fclose(fp) != 0;
    identity_read_result_t result = IDENTITY_READ_VALID;
    if (read_error || close_error) {
        result = IDENTITY_READ_ERROR;
    } else if (got != SIGNAL_CRYPTO_SECRET_BYTES || !eof) {
        result = IDENTITY_READ_CORRUPT;
    } else {
        memcpy(out, buf, SIGNAL_CRYPTO_SECRET_BYTES);
    }
    signal_memzero_explicit(buf, sizeof(buf));
    return result;
}

#if !defined(_WIN32)
static bool sync_parent_directory(const char *path) {
    if (!path) return false;
    char directory[2048];
    int n = snprintf(directory, sizeof(directory), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(directory)) return false;
    char *slash = strrchr(directory, '/');
    if (slash) {
        if (slash == directory) {
            slash[1] = '\0';
        } else {
            *slash = '\0';
        }
    } else {
        directory[0] = '.';
        directory[1] = '\0';
    }
    int flags = O_RDONLY;
#if defined(O_DIRECTORY)
    flags |= O_DIRECTORY;
#endif
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    int dir_fd = open(directory, flags);
    if (dir_fd < 0) return false;
    int sync_result;
    do {
        sync_result = fsync(dir_fd);
    } while (sync_result != 0 && errno == EINTR);
    bool synced = sync_result == 0;
    if (close(dir_fd) != 0) synced = false;
    return synced;
}
#endif

#if defined(_WIN32)
static bool windows_stage_secret_file(
    const char *path,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    char *temp_path,
    size_t temp_path_cap) {
    static volatile LONG sequence = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        LONG serial = InterlockedIncrement(&sequence);
        int n = snprintf(
            temp_path, temp_path_cap, "%s.tmp.%lu.%lu.%ld",
            path, (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetTickCount(), (long)serial);
        if (n <= 0 || (size_t)n >= temp_path_cap) return false;
        file = CreateFileA(
            temp_path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD wrote = 0;
    bool ok = WriteFile(file, secret, SIGNAL_CRYPTO_SECRET_BYTES,
                        &wrote, NULL) &&
              wrote == SIGNAL_CRYPTO_SECRET_BYTES &&
              FlushFileBuffers(file);
    if (!CloseHandle(file)) ok = false;
    if (!ok) (void)DeleteFileA(temp_path);
    return ok;
}

static bool windows_stage_backup_copy(
    const char *path,
    const char *bad_path,
    char *temp_path,
    size_t temp_path_cap) {
    static volatile LONG sequence = 0;
    bool copied = false;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        LONG serial = InterlockedIncrement(&sequence);
        int n = snprintf(
            temp_path, temp_path_cap, "%s.tmp.%lu.%lu.%ld",
            bad_path, (unsigned long)GetCurrentProcessId(),
            (unsigned long)GetTickCount(), (long)serial);
        if (n <= 0 || (size_t)n >= temp_path_cap) return false;
        if (CopyFileA(path, temp_path, TRUE)) {
            copied = true;
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    if (!copied) return false;

    HANDLE file = CreateFileA(
        temp_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    bool ok = file != INVALID_HANDLE_VALUE &&
              FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE && !CloseHandle(file)) ok = false;
    if (!ok) (void)DeleteFileA(temp_path);
    return ok;
}

static bool write_secret_file(
    const char *path,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    char temp_path[MAX_PATH];
    if (!windows_stage_secret_file(
            path, secret, temp_path, sizeof(temp_path))) {
        return false;
    }
    bool ok = MoveFileExA(
        temp_path, path,
        MOVEFILE_REPLACE_EXISTING |
        MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) (void)DeleteFileA(temp_path);
    return ok;
}

static bool replace_corrupt_secret_file(
    const char *path,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    char bad_path[MAX_PATH];
    int n = snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    if (n <= 0 || (size_t)n >= sizeof(bad_path)) return false;

    char candidate_path[MAX_PATH];
    if (!windows_stage_secret_file(
            path, secret, candidate_path, sizeof(candidate_path))) {
        return false;
    }
    char backup_path[MAX_PATH];
    if (!windows_stage_backup_copy(
            path, bad_path, backup_path, sizeof(backup_path))) {
        (void)DeleteFileA(candidate_path);
        return false;
    }
    if (!MoveFileExA(
            backup_path, bad_path,
            MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileA(backup_path);
        (void)DeleteFileA(candidate_path);
        return false;
    }
    if (!MoveFileExA(
            candidate_path, path,
            MOVEFILE_REPLACE_EXISTING |
            MOVEFILE_WRITE_THROUGH)) {
        (void)DeleteFileA(candidate_path);
        return false;
    }
    return true;
}
#else
static bool posix_write_all(
    int fd, const uint8_t *bytes, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t wrote = write(fd, bytes + offset, size - offset);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return false;
        offset += (size_t)wrote;
    }
    return true;
}

static bool posix_sync_fd(int fd) {
    int result;
    do {
        result = fsync(fd);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

static bool posix_stage_secret_file(
    const char *path,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES],
    char *temp_path,
    size_t temp_path_cap) {
    int n = snprintf(temp_path, temp_path_cap, "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= temp_path_cap) return false;
    int fd = mkstemp(temp_path);
    if (fd < 0) return false;
    (void)fchmod(fd, 0600);
    bool ok = posix_write_all(
        fd, secret, SIGNAL_CRYPTO_SECRET_BYTES);
    if (ok && !posix_sync_fd(fd)) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) (void)unlink(temp_path);
    return ok;
}

static bool posix_stage_backup_copy(
    const char *path,
    const char *bad_path,
    char *temp_path,
    size_t temp_path_cap) {
    int n = snprintf(
        temp_path, temp_path_cap, "%s.tmp.XXXXXX", bad_path);
    if (n <= 0 || (size_t)n >= temp_path_cap) return false;

    int source = open(path, O_RDONLY);
    if (source < 0) return false;
    int destination = mkstemp(temp_path);
    if (destination < 0) {
        (void)close(source);
        return false;
    }
    (void)fchmod(destination, 0600);

    bool ok = true;
    uint8_t buffer[4096];
    for (;;) {
        ssize_t got = read(source, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!posix_write_all(
                destination, buffer, (size_t)got)) {
            ok = false;
            break;
        }
    }
    signal_memzero_explicit(buffer, sizeof(buffer));
    if (ok && !posix_sync_fd(destination)) ok = false;
    if (close(source) != 0) ok = false;
    if (close(destination) != 0) ok = false;
    if (!ok) (void)unlink(temp_path);
    return ok;
}

static bool posix_commit_staged_secret(
    const char *temp_path, const char *path) {
    if (rename(temp_path, path) != 0) return false;
    if (!sync_parent_directory(path)) {
        fprintf(stderr,
                "[identity] installed %s, but could not confirm "
                "directory durability\n",
                path);
    }
    return true;
}

static bool write_secret_file(
    const char *path,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    char temp_path[2048];
    if (!posix_stage_secret_file(
            path, secret, temp_path, sizeof(temp_path))) {
        return false;
    }
    if (!posix_commit_staged_secret(temp_path, path)) {
        (void)unlink(temp_path);
        return false;
    }
    return true;
}

static bool replace_corrupt_secret_file(
    const char *path,
    const uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES]) {
    char bad_path[2048];
    int n = snprintf(bad_path, sizeof(bad_path), "%s.bad", path);
    if (n <= 0 || (size_t)n >= sizeof(bad_path)) return false;

    char candidate_path[2048];
    if (!posix_stage_secret_file(
            path, secret, candidate_path,
            sizeof(candidate_path))) {
        return false;
    }
    char backup_path[2048];
    if (!posix_stage_backup_copy(
            path, bad_path, backup_path,
            sizeof(backup_path))) {
        (void)unlink(candidate_path);
        return false;
    }
    if (rename(backup_path, bad_path) != 0) {
        (void)unlink(backup_path);
        (void)unlink(candidate_path);
        return false;
    }
    if (!sync_parent_directory(path)) {
        (void)unlink(candidate_path);
        return false;
    }
    if (!posix_commit_staged_secret(candidate_path, path)) {
        (void)unlink(candidate_path);
        return false;
    }
    return true;
}
#endif
#endif /* !__EMSCRIPTEN__ */

/* ---------------------------------------------------------------------
 * Wasm path: localStorage["signal:identity"] base64 of secret bytes
 * ------------------------------------------------------------------ */

#if defined(__EMSCRIPTEN__)
/* Stash up to 96 base64 chars (88 needed for 64 raw bytes + null pad). */
EM_JS(int, signal_localstorage_load, (char *out, int cap), {
    try {
        var s = window.localStorage.getItem("signal:identity");
        if (s === null) return 0;
        var canonical = s.length === 88 &&
            s.substring(86) === "==";
        var alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (var i = 0; canonical && i < 86; i++) {
            canonical = alphabet.indexOf(s[i]) >= 0;
        }
        if (canonical) {
            canonical =
                (alphabet.indexOf(s[85]) & 15) === 0;
        }
        if (!canonical || s.length + 1 > cap) return -2;
        stringToUTF8(s, out, cap);
        return s.length;
    } catch (e) { return -1; }
})

EM_JS(int, signal_localstorage_save, (const char *s), {
    try {
        window.localStorage.setItem("signal:identity", UTF8ToString(s));
        return 1;
    } catch (e) { return 0; }
})

/* Keep the conditional install and immediate winner reread in one synchronous
 * JS call. This narrows last-writer divergence, but is not a cross-tab mutex;
 * full serialization needs an asynchronous browser startup lock. */
EM_JS(int, signal_localstorage_install_if_absent,
      (const char *candidate_ptr, char *winner_ptr, int winner_cap), {
    try {
        var candidate = UTF8ToString(candidate_ptr);
        var winner = window.localStorage.getItem("signal:identity");
        if (winner === null) {
            window.localStorage.setItem("signal:identity", candidate);
            winner = window.localStorage.getItem("signal:identity");
        }
        if (winner === null || winner.length + 1 > winner_cap) return -1;
        stringToUTF8(winner, winner_ptr, winner_cap);
        return winner.length;
    } catch (e) { return -1; }
})

/* Recover an empty or oversized entry without copying attacker-controlled
 * bytes through the fixed C buffer. A concurrently repaired canonical entry
 * wins; only a still-invalid value is preserved and replaced. */
EM_JS(int, signal_localstorage_replace_if_invalid,
      (const char *candidate_ptr, char *winner_ptr, int winner_cap), {
    try {
        var candidate = UTF8ToString(candidate_ptr);
        var current = window.localStorage.getItem("signal:identity");
        var canonical = current !== null &&
            current.length === 88 &&
            current.substring(86) === "==";
        var alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (var i = 0; canonical && i < 86; i++) {
            canonical = alphabet.indexOf(current[i]) >= 0;
        }
        if (canonical) {
            canonical =
                (alphabet.indexOf(current[85]) & 15) === 0;
        }
        if (!canonical) {
            if (current !== null) {
                window.localStorage.setItem(
                    "signal:identity.bad", current);
            }
            window.localStorage.setItem(
                "signal:identity", candidate);
        }
        var winner = window.localStorage.getItem("signal:identity");
        if (winner === null || winner.length + 1 > winner_cap) return -1;
        stringToUTF8(winner, winner_ptr, winner_cap);
        return winner.length;
    } catch (e) { return -1; }
})

static const char B64ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t len, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[o++] = B64ALPHA[(v >> 18) & 63];
        out[o++] = B64ALPHA[(v >> 12) & 63];
        out[o++] = B64ALPHA[(v >> 6)  & 63];
        out[o++] = B64ALPHA[ v        & 63];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i+1] << 8;
        out[o++] = B64ALPHA[(v >> 18) & 63];
        out[o++] = B64ALPHA[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64ALPHA[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool b64_decode(const char *s, uint8_t *out, size_t out_cap, size_t *out_len) {
    size_t i = 0, o = 0;
    while (s[i] && s[i] != '=') {
        int v0 = b64_decode_char(s[i]);
        int v1 = (s[i+1] && s[i+1] != '=') ? b64_decode_char(s[i+1]) : -1;
        int v2 = (s[i+1] && s[i+2] && s[i+2] != '=') ? b64_decode_char(s[i+2]) : -1;
        int v3 = (s[i+1] && s[i+2] && s[i+3] && s[i+3] != '=') ? b64_decode_char(s[i+3]) : -1;
        if (v0 < 0 || v1 < 0) return false;
        if (o >= out_cap) return false;
        out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (v2 >= 0) {
            if (o >= out_cap) return false;
            out[o++] = (uint8_t)(((v1 & 0xf) << 4) | (v2 >> 2));
        }
        if (v3 >= 0) {
            if (o >= out_cap) return false;
            out[o++] = (uint8_t)(((v2 & 0x3) << 6) | v3);
        }
        i += 4;
        if (s[i] == '\0') break;
    }
    *out_len = o;
    return true;
}

static bool identity_decode_base64(const char *encoded,
                                   player_identity_t *out) {
    if (!encoded || !out) return false;
    if (strlen(encoded) != 88 ||
        encoded[86] != '=' || encoded[87] != '=') {
        return false;
    }
    for (size_t i = 0; i < 86; i++) {
        if (b64_decode_char(encoded[i]) < 0) return false;
    }
    if ((b64_decode_char(encoded[85]) & 0x0f) != 0) return false;
    uint8_t decoded[SIGNAL_CRYPTO_SECRET_BYTES + 4] = {0};
    size_t decoded_len = 0;
    bool decoded_ok =
        b64_decode(encoded, decoded, sizeof(decoded), &decoded_len) &&
        decoded_len == SIGNAL_CRYPTO_SECRET_BYTES;
    if (decoded_ok) {
        memcpy(out->secret, decoded, SIGNAL_CRYPTO_SECRET_BYTES);
        memcpy(out->pubkey,
               out->secret + (SIGNAL_CRYPTO_SECRET_BYTES -
                              SIGNAL_CRYPTO_PUBKEY_BYTES),
               SIGNAL_CRYPTO_PUBKEY_BYTES);
    }
    signal_memzero_explicit(decoded, sizeof(decoded));
    return decoded_ok;
}
#endif /* __EMSCRIPTEN__ */

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

static bool generate_identity(player_identity_t *out) {
    if (!out) return false;
    identity_clear(out);
    if (!signal_crypto_keypair(out->pubkey, out->secret)) {
        /* signal_crypto_keypair already clears both arrays, but keep the
         * aggregate guarantee here so future backends cannot leak a partial
         * identity through this persistence boundary. */
        identity_clear(out);
        return false;
    }
    return true;
}

bool identity_save_to(const player_identity_t *id, const char *path) {
    if (!id) return false;
#if defined(__EMSCRIPTEN__)
    (void)path;
    char enc[128] = {0};
    b64_encode(id->secret, SIGNAL_CRYPTO_SECRET_BYTES, enc);
    bool saved = signal_localstorage_save(enc) == 1;
    signal_memzero_explicit(enc, sizeof(enc));
    return saved;
#else
    if (!path) return false;
    identity_path_lock_t lock;
    if (!identity_path_lock_acquire(path, &lock)) return false;
    bool saved = write_secret_file(path, id->secret);
    identity_path_lock_release(&lock);
    return saved;
#endif
}

bool identity_load_or_generate_at(player_identity_t *out, const char *path) {
    if (!out) return false;
    identity_clear(out);

#if defined(__EMSCRIPTEN__)
    (void)path;
    char enc[256] = {0};
    int got = signal_localstorage_load(enc, sizeof(enc));
    if (got == -1) {
        signal_memzero_explicit(enc, sizeof(enc));
        return false;
    }
    if (got == -2) {
        if (!generate_identity(out)) {
            signal_memzero_explicit(enc, sizeof(enc));
            return false;
        }
        char candidate[128] = {0};
        char winner[256] = {0};
        b64_encode(out->secret, SIGNAL_CRYPTO_SECRET_BYTES, candidate);
        int winner_len = signal_localstorage_replace_if_invalid(
            candidate, winner, sizeof(winner));
        signal_memzero_explicit(candidate, sizeof(candidate));
        identity_clear(out);
        bool loaded = winner_len > 0 &&
            identity_decode_base64(winner, out);
        signal_memzero_explicit(winner, sizeof(winner));
        signal_memzero_explicit(enc, sizeof(enc));
        if (!loaded) identity_clear(out);
        return loaded;
    }
    if (got > 0) {
        bool loaded = identity_decode_base64(enc, out);
        signal_memzero_explicit(enc, sizeof(enc));
        if (!loaded) identity_clear(out);
        return loaded;
    }
    if (!generate_identity(out)) {
        signal_memzero_explicit(enc, sizeof(enc));
        return false;
    }
    char candidate[128] = {0};
    char winner[256] = {0};
    b64_encode(out->secret, SIGNAL_CRYPTO_SECRET_BYTES, candidate);
    int winner_len = signal_localstorage_install_if_absent(
        candidate, winner, sizeof(winner));
    signal_memzero_explicit(candidate, sizeof(candidate));
    identity_clear(out);
    bool loaded = winner_len > 0 &&
        identity_decode_base64(winner, out);
    signal_memzero_explicit(winner, sizeof(winner));
    signal_memzero_explicit(enc, sizeof(enc));
    if (!loaded) identity_clear(out);
    return loaded;
#else
    if (!path) return false;
    identity_path_lock_t lock;
    if (!identity_path_lock_acquire(path, &lock)) return false;
    bool loaded = false;
    uint8_t secret[SIGNAL_CRYPTO_SECRET_BYTES] = {0};
    identity_read_result_t read_result =
        read_secret_file(path, secret);
    if (read_result == IDENTITY_READ_VALID) {
        memcpy(out->secret, secret, SIGNAL_CRYPTO_SECRET_BYTES);
        memcpy(out->pubkey,
               out->secret + (SIGNAL_CRYPTO_SECRET_BYTES -
                              SIGNAL_CRYPTO_PUBKEY_BYTES),
               SIGNAL_CRYPTO_PUBKEY_BYTES);
        loaded = true;
        goto identity_load_done;
    }
    if (read_result == IDENTITY_READ_ERROR) {
        goto identity_load_done;
    }
    if (read_result == IDENTITY_READ_CORRUPT) {
        /* Preserve the original bytes until secure key generation succeeds.
         * Entropy failure must not rename, truncate, or replace identity
         * state on disk. */
        if (!generate_identity(out)) goto identity_load_done;
        if (!replace_corrupt_secret_file(path, out->secret)) {
            goto identity_load_done;
        }
        fprintf(stderr,
                "[identity] %s was not 64 bytes; preserved it as %s.bad\n",
                path, path);
        loaded = true;
        goto identity_load_done;
    }
    if (!generate_identity(out)) goto identity_load_done;
    if (!write_secret_file(path, out->secret)) {
        goto identity_load_done;
    }
    loaded = true;

identity_load_done:
    signal_memzero_explicit(secret, sizeof(secret));
    identity_path_lock_release(&lock);
    if (!loaded) identity_clear(out);
    return loaded;
#endif
}

bool identity_load_or_generate(player_identity_t *out) {
#if defined(__EMSCRIPTEN__)
    return identity_load_or_generate_at(out, NULL);
#else
    char path[2048];
    if (!resolve_default_path(path, sizeof(path))) {
        identity_clear(out);
        fprintf(stderr,
                "[identity] could not resolve a writable identity path; "
                "authentication disabled to avoid an ephemeral identity\n");
        return false;
    }
    return identity_load_or_generate_at(out, path);
#endif
}

void identity_clear(player_identity_t *id) {
    if (!id) return;
    signal_memzero_explicit(id, sizeof(*id));
}
