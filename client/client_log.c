/* client_log.c -- Persistent native client stdout/stderr logging. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "client_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__EMSCRIPTEN__)
/* Browser builds retain the JavaScript console. */
#elif defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

enum { CLIENT_LOG_PATH_CAP = 2048 };

static char active_log_path[CLIENT_LOG_PATH_CAP];
static bool log_initialized;

#if !defined(__EMSCRIPTEN__)
static bool copy_path(char *out, size_t cap, const char *path) {
    if (!out || cap == 0 || !path || !*path) return false;
    int n = snprintf(out, cap, "%s", path);
    return n > 0 && (size_t)n < cap;
}

static const char *native_home_dir(void) {
    const char *home = getenv("HOME");
#if !defined(_WIN32)
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
#endif
    return home && *home ? home : NULL;
}

static bool make_log_dir(const char *path) {
    if (!path || !*path) return false;
#if defined(_WIN32)
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0700) == 0 || errno == EEXIST;
#endif
}

static bool resolve_default_log_path(char *out, size_t cap) {
    const char *override = getenv("SIGNAL_LOG_PATH");
    if (override && *override) return copy_path(out, cap, override);

#if defined(_WIN32)
    char base[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        return false;
    char dir[MAX_PATH];
    int n = snprintf(dir, sizeof(dir), "%s\\signal", base);
    if (n <= 0 || (size_t)n >= sizeof(dir) || !make_log_dir(dir)) return false;
    n = snprintf(out, cap, "%s\\client.log", dir);
    return n > 0 && (size_t)n < cap;
#elif defined(__APPLE__)
    const char *home = native_home_dir();
    if (!home) return false;
    char dir[CLIENT_LOG_PATH_CAP];
    int n = snprintf(dir, sizeof(dir), "%s/Library/Logs/signal", home);
    if (n <= 0 || (size_t)n >= sizeof(dir) || !make_log_dir(dir)) return false;
    n = snprintf(out, cap, "%s/client.log", dir);
    return n > 0 && (size_t)n < cap;
#else
    const char *state_home = getenv("XDG_STATE_HOME");
    char state_fallback[CLIENT_LOG_PATH_CAP];
    if (!state_home || !*state_home) {
        const char *home = native_home_dir();
        if (!home) return false;
        char local_dir[CLIENT_LOG_PATH_CAP];
        int n = snprintf(local_dir, sizeof(local_dir), "%s/.local", home);
        if (n <= 0 || (size_t)n >= sizeof(local_dir) ||
            !make_log_dir(local_dir)) return false;
        n = snprintf(state_fallback, sizeof(state_fallback),
                     "%s/.local/state", home);
        if (n <= 0 || (size_t)n >= sizeof(state_fallback) ||
            !make_log_dir(state_fallback)) return false;
        state_home = state_fallback;
    } else if (!make_log_dir(state_home)) {
        return false;
    }
    char dir[CLIENT_LOG_PATH_CAP];
    int n = snprintf(dir, sizeof(dir), "%s/signal", state_home);
    if (n <= 0 || (size_t)n >= sizeof(dir) || !make_log_dir(dir)) return false;
    n = snprintf(out, cap, "%s/client.log", dir);
    return n > 0 && (size_t)n < cap;
#endif
}

static bool persistence_disabled(void) {
    const char *value = getenv("SIGNAL_LOG_PERSIST");
    return value && (strcmp(value, "0") == 0 ||
                     strcmp(value, "false") == 0 ||
                     strcmp(value, "FALSE") == 0);
}

static void format_utc_timestamp(char out[32]) {
    time_t now = time(NULL);
    struct tm utc;
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) != 0) {
        snprintf(out, 32, "unknown-time");
        return;
    }
#else
    if (!gmtime_r(&now, &utc)) {
        snprintf(out, 32, "unknown-time");
        return;
    }
#endif
    if (strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        snprintf(out, 32, "unknown-time");
}
#endif /* !__EMSCRIPTEN__ */

bool client_log_rotate_file(const char *path, size_t max_bytes) {
    if (!path || !*path || max_bytes == 0) return false;

    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT;
    bool ok = fseek(file, 0, SEEK_END) == 0;
    long size = ok ? ftell(file) : -1;
    if (fclose(file) != 0) ok = false;
    if (!ok || size < 0) return false;
    if ((size_t)size < max_bytes) return true;

    char backup[CLIENT_LOG_PATH_CAP + 3];
    int n = snprintf(backup, sizeof(backup), "%s.1", path);
    if (n <= 0 || (size_t)n >= sizeof(backup)) return false;
    (void)remove(backup);
    return rename(path, backup) == 0;
}

bool client_log_init(void) {
    if (log_initialized) return true;
    log_initialized = true;

    /* Newline-terminated telemetry should be observable immediately even when
     * persistence is explicitly disabled for a terminal-only run. */
    (void)setvbuf(stdout, NULL, _IOLBF, 0);
    (void)setvbuf(stderr, NULL, _IOLBF, 0);

#if defined(__EMSCRIPTEN__)
    return true;
#else
    if (persistence_disabled()) return true;

    char path[CLIENT_LOG_PATH_CAP];
    if (!resolve_default_log_path(path, sizeof(path))) {
        fprintf(stderr, "[client-log] could not resolve persistent log path\n");
        return false;
    }
    if (!client_log_rotate_file(path, SIGNAL_CLIENT_LOG_MAX_BYTES)) {
        fprintf(stderr, "[client-log] could not rotate %s: %s\n",
                path, strerror(errno));
    }

#if defined(_WIN32)
    int fd = _open(path, _O_CREAT | _O_WRONLY | _O_APPEND | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
    if (fd < 0 || _dup2(fd, _fileno(stdout)) != 0 ||
        _dup2(fd, _fileno(stderr)) != 0) {
        if (fd >= 0) _close(fd);
        fprintf(stderr, "[client-log] could not open %s: %s\n",
                path, strerror(errno));
        return false;
    }
    _close(fd);
#else
    int fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 ||
        dup2(fd, STDERR_FILENO) < 0) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "[client-log] could not open %s: %s\n",
                path, strerror(errno));
        return false;
    }
    (void)fchmod(fd, 0600);
    close(fd);
#endif

    if (!copy_path(active_log_path, sizeof(active_log_path), path))
        active_log_path[0] = '\0';
    char timestamp[32];
    format_utc_timestamp(timestamp);
#ifdef GIT_HASH
    fprintf(stdout, "\n[client-log] session start %s build=%s path=%s\n",
            timestamp, GIT_HASH, path);
#else
    fprintf(stdout, "\n[client-log] session start %s path=%s\n",
            timestamp, path);
#endif
    return true;
#endif /* __EMSCRIPTEN__ */
}

void client_log_shutdown(void) {
#if !defined(__EMSCRIPTEN__)
    if (active_log_path[0]) {
        char timestamp[32];
        format_utc_timestamp(timestamp);
        fprintf(stdout, "[client-log] session end %s\n", timestamp);
    }
#endif
    fflush(NULL);
}

const char *client_log_path(void) {
    return active_log_path;
}
