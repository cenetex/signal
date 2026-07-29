#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "persistence_generation.h"

#include "base58.h"
#include "persistence_io.h"
#include "sha256.h"
#include "sim_catalog.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define GENERATION_MKDIR(path) _mkdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#define GENERATION_MKDIR(path) mkdir((path), 0700)
#endif

#define GENERATION_MANIFEST_MAGIC "SIGGEN1\0"
#define GENERATION_POINTER_MAGIC  "SIGCUR1\0"
#define GENERATION_FORMAT_VERSION 1u
#define GENERATION_PREFIX "generation-"
#define GENERATION_MANIFEST_NAME "MANIFEST"
#define GENERATION_POINTER_NAME "CURRENT"
#define GENERATION_RECOVERY_MARKER_NAME "LEGACY-RECOVERY-CONSUMED"
#define GENERATION_RECOVERY_MARKER_MAGIC "SIGLRC1\0"
#define GENERATION_MAX_ENTRIES 65536u
#define GENERATION_MAX_DEPTH 8

typedef enum {
    FS_NODE_REGULAR = 1,
    FS_NODE_DIRECTORY,
    FS_NODE_UNSUPPORTED,
} fs_node_kind_t;

typedef struct {
    char *name;
    fs_node_kind_t kind;
} directory_entry_t;

typedef struct {
    directory_entry_t *items;
    size_t count;
    size_t capacity;
} directory_entries_t;

typedef struct {
    char *path;
    uint64_t size;
    uint8_t sha256[32];
} manifest_entry_t;

typedef struct {
    manifest_entry_t *items;
    size_t count;
    size_t capacity;
} manifest_entries_t;

typedef struct {
    uint64_t current_generation;
    uint8_t current_manifest_sha256[32];
    uint64_t previous_generation;
    uint8_t previous_manifest_sha256[32];
} generation_pointer_t;

static bool path_join(char *out, size_t out_size,
                      const char *left, const char *right) {
    if (!out || out_size == 0 || !left || !right) return false;
    size_t left_len = strlen(left);
    const char *separator =
        left_len > 0 && left[left_len - 1] == '/' ? "" : "/";
    int n = snprintf(out, out_size, "%s%s%s", left, separator, right);
    return n > 0 && (size_t)n < out_size;
}

static bool generation_dir_path(char *out, size_t out_size,
                                const char *root_dir, uint64_t generation) {
    char name[64];
    int n = snprintf(name, sizeof(name), GENERATION_PREFIX "%020" PRIu64,
                     generation);
    return n > 0 && (size_t)n < sizeof(name) &&
           path_join(out, out_size, root_dir, name);
}

static bool generation_paths_fill(
    const char *root_dir,
    uint64_t generation,
    const uint8_t manifest_sha256[32],
    persistence_generation_paths_t *out) {
    if (!root_dir || generation == 0 || !manifest_sha256 || !out)
        return false;
    char generation_dir[PERSISTENCE_GENERATION_PATH_MAX];
    if (!generation_dir_path(generation_dir, sizeof(generation_dir),
                             root_dir, generation) ||
        !path_join(out->world_path, sizeof(out->world_path),
                   generation_dir, "world.sav") ||
        !path_join(out->catalog_dir, sizeof(out->catalog_dir),
                   generation_dir, "stations") ||
        !path_join(out->player_dir, sizeof(out->player_dir),
                   generation_dir, "players")) {
        return false;
    }
    out->generation = generation;
    memcpy(out->manifest_sha256, manifest_sha256,
           sizeof(out->manifest_sha256));
    return true;
}

static fs_node_kind_t fs_node_kind(const char *path) {
    if (!path || !path[0]) return FS_NODE_UNSUPPORTED;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return FS_NODE_UNSUPPORTED;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        ? FS_NODE_DIRECTORY : FS_NODE_REGULAR;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return FS_NODE_UNSUPPORTED;
    if (S_ISREG(st.st_mode)) return FS_NODE_REGULAR;
    if (S_ISDIR(st.st_mode)) return FS_NODE_DIRECTORY;
    return FS_NODE_UNSUPPORTED;
#endif
}

static bool fs_path_missing(const char *path) {
    if (!path || !path[0]) return false;
#ifdef _WIN32
    DWORD attributes = GetFileAttributesA(path);
    if (attributes != INVALID_FILE_ATTRIBUTES) return false;
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ||
           error == ERROR_PATH_NOT_FOUND;
#else
    struct stat st;
    return lstat(path, &st) != 0 && errno == ENOENT;
#endif
}

static bool ensure_directory(const char *path) {
    if (!path || !path[0]) return false;
    if (GENERATION_MKDIR(path) == 0)
        return persistence_sync_parent_dir(path);
    return errno == EEXIST && fs_node_kind(path) == FS_NODE_DIRECTORY;
}

static void directory_entries_free(directory_entries_t *entries) {
    if (!entries) return;
    for (size_t i = 0; i < entries->count; i++)
        free(entries->items[i].name);
    free(entries->items);
    memset(entries, 0, sizeof(*entries));
}

static bool directory_entries_push(directory_entries_t *entries,
                                   const char *name,
                                   fs_node_kind_t kind) {
    if (!entries || !name || !name[0]) return false;
    if (entries->count >= entries->capacity) {
        size_t next = entries->capacity ? entries->capacity * 2u : 16u;
        if (next < entries->capacity ||
            next > SIZE_MAX / sizeof(*entries->items)) {
            return false;
        }
        directory_entry_t *grown = realloc(
            entries->items, next * sizeof(*entries->items));
        if (!grown) return false;
        entries->items = grown;
        entries->capacity = next;
    }
    size_t len = strlen(name);
    char *copy = malloc(len + 1u);
    if (!copy) return false;
    memcpy(copy, name, len + 1u);
    entries->items[entries->count++] = (directory_entry_t){
        .name = copy,
        .kind = kind,
    };
    return true;
}

static int directory_entry_compare(const void *left, const void *right) {
    const directory_entry_t *a = left;
    const directory_entry_t *b = right;
    return strcmp(a->name, b->name);
}

static bool directory_entries_read(const char *dir,
                                   directory_entries_t *entries,
                                   bool missing_is_empty) {
    if (!dir || !entries) return false;
    memset(entries, 0, sizeof(*entries));
#ifdef _WIN32
    char pattern[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(pattern, sizeof(pattern), dir, "*")) return false;
    WIN32_FIND_DATAA found;
    HANDLE search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return missing_is_empty &&
               (error == ERROR_FILE_NOT_FOUND ||
                error == ERROR_PATH_NOT_FOUND);
    }
    bool ok = true;
    do {
        const char *name = found.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        fs_node_kind_t kind = FS_NODE_REGULAR;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            kind = FS_NODE_UNSUPPORTED;
        } else if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            kind = FS_NODE_DIRECTORY;
        }
        if (!directory_entries_push(entries, name, kind)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(search, &found) != 0);
    if (GetLastError() != ERROR_NO_MORE_FILES) ok = false;
    FindClose(search);
#else
    DIR *stream = opendir(dir);
    if (!stream) return missing_is_empty && errno == ENOENT;
    bool ok = true;
    errno = 0;
    struct dirent *found;
    while ((found = readdir(stream)) != NULL) {
        const char *name = found->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char path[PERSISTENCE_GENERATION_PATH_MAX];
        if (!path_join(path, sizeof(path), dir, name) ||
            !directory_entries_push(entries, name, fs_node_kind(path))) {
            ok = false;
            break;
        }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(stream) != 0) ok = false;
#endif
    if (!ok) {
        directory_entries_free(entries);
        return false;
    }
    if (entries->count > 1u) {
        qsort(entries->items, entries->count, sizeof(*entries->items),
              directory_entry_compare);
    }
    return true;
}

static bool has_suffix(const char *text, const char *suffix) {
    if (!text || !suffix) return false;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcmp(text + text_len - suffix_len, suffix) == 0;
}

static bool copy_file_durable(const char *source, const char *destination) {
    if (!source || !destination ||
        fs_node_kind(source) != FS_NODE_REGULAR) {
        return false;
    }
    char temporary[PERSISTENCE_GENERATION_PATH_MAX];
    int n = snprintf(temporary, sizeof(temporary), "%s.copytmp", destination);
    if (n <= 0 || (size_t)n >= sizeof(temporary)) return false;

    FILE *input = fopen(source, "rb");
    if (!input) return false;
    FILE *output = fopen(temporary, "wb");
    if (!output) {
        fclose(input);
        return false;
    }
    bool ok = true;
    uint8_t chunk[16384];
    size_t count = 0;
    while ((count = fread(chunk, 1, sizeof(chunk), input)) > 0) {
        if (fwrite(chunk, 1, count, output) != count) {
            ok = false;
            break;
        }
    }
    if (ferror(input)) ok = false;
    if (ok) ok = persistence_flush_durable(output);
    if (fclose(input) != 0) ok = false;
    if (fclose(output) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    if (!persistence_replace_file(temporary, destination)) {
        remove(temporary);
        return false;
    }
    return true;
}

static bool copy_player_save_files_filtered(
    const char *source_dir,
    const char *destination_dir,
    bool missing_is_empty,
    const char *skip_name,
    const char *forbid_name,
    bool *saw_skip,
    bool *saw_forbidden);

static bool copy_player_save_files(const char *source_dir,
                                   const char *destination_dir,
                                   bool missing_is_empty) {
    return copy_player_save_files_filtered(
        source_dir, destination_dir, missing_is_empty,
        NULL, NULL, NULL, NULL);
}

static bool copy_player_save_files_filtered(
    const char *source_dir,
    const char *destination_dir,
    bool missing_is_empty,
    const char *skip_name,
    const char *forbid_name,
    bool *saw_skip,
    bool *saw_forbidden) {
    directory_entries_t entries;
    if (!directory_entries_read(source_dir, &entries, missing_is_empty))
        return false;
    bool ok = true;
    for (size_t i = 0; i < entries.count; i++) {
        const directory_entry_t *entry = &entries.items[i];
        if (!has_suffix(entry->name, ".sav")) continue;
        if (entry->kind != FS_NODE_REGULAR) {
            ok = false;
            break;
        }
        if (skip_name && strcmp(entry->name, skip_name) == 0) {
            if (saw_skip) *saw_skip = true;
            continue;
        }
        if (forbid_name && strcmp(entry->name, forbid_name) == 0) {
            if (saw_forbidden) *saw_forbidden = true;
            ok = false;
            break;
        }
        char source[PERSISTENCE_GENERATION_PATH_MAX];
        char destination[PERSISTENCE_GENERATION_PATH_MAX];
        if (!path_join(source, sizeof(source), source_dir, entry->name) ||
            !path_join(destination, sizeof(destination),
                       destination_dir, entry->name) ||
            !copy_file_durable(source, destination)) {
            ok = false;
            break;
        }
    }
    directory_entries_free(&entries);
    return ok;
}

static bool copy_player_namespace(const char *source_dir,
                                  const char *destination_dir) {
    if (!destination_dir || !ensure_directory(destination_dir)) return false;
    if (source_dir &&
        fs_node_kind(source_dir) != FS_NODE_DIRECTORY &&
        !fs_path_missing(source_dir)) {
        return false;
    }
    if (source_dir &&
        !copy_player_save_files(source_dir, destination_dir, true)) {
        return false;
    }
    static const char *subdirs[] = {"legacy", "pubkey"};
    for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        char destination[PERSISTENCE_GENERATION_PATH_MAX];
        if (!path_join(destination, sizeof(destination),
                       destination_dir, subdirs[i]) ||
            !ensure_directory(destination)) {
            return false;
        }
        if (!source_dir) continue;
        char source[PERSISTENCE_GENERATION_PATH_MAX];
        if (!path_join(source, sizeof(source), source_dir, subdirs[i]))
            return false;
        if (fs_node_kind(source) != FS_NODE_DIRECTORY &&
            !fs_path_missing(source)) {
            return false;
        }
        if (!copy_player_save_files(source, destination, true)) {
            return false;
        }
    }
    return true;
}

static bool recovery_source_name(
    char out[sizeof("player_") + 16 + sizeof(".sav")],
    const uint8_t token[8]) {
    static const char digits[] = "0123456789abcdef";
    if (!out || !token) return false;
    memcpy(out, "player_", 7);
    for (size_t i = 0; i < 8; i++) {
        out[7 + i * 2] = digits[token[i] >> 4];
        out[8 + i * 2] = digits[token[i] & 0x0fu];
    }
    memcpy(out + 23, ".sav", 5);
    return true;
}

static bool recovery_destination_name(
    char out[72], const uint8_t pubkey[32]) {
    if (!out || !pubkey) return false;
    char encoded[64];
    size_t len = base58_encode(pubkey, 32, encoded, sizeof(encoded));
    if (len == 0 || len + 4u >= 72u) return false;
    int n = snprintf(out, 72, "%s.sav", encoded);
    return n > 0 && n < 72;
}

#if defined(SIGNAL_SAVE_TESTING)
static persistence_recovery_test_hook_fn
    recovery_before_source_bind_hook;
static void *recovery_before_source_bind_user;
static persistence_recovery_test_hook_fn
    recovery_before_destination_publish_hook;
static void *recovery_before_destination_publish_user;

void persistence_recovery_test_set_before_source_bind_hook(
    persistence_recovery_test_hook_fn hook, void *user) {
    recovery_before_source_bind_hook = hook;
    recovery_before_source_bind_user = user;
}

void persistence_recovery_test_set_before_destination_publish_hook(
    persistence_recovery_test_hook_fn hook, void *user) {
    recovery_before_destination_publish_hook = hook;
    recovery_before_destination_publish_user = user;
}

void persistence_recovery_test_reset_hooks(void) {
    recovery_before_source_bind_hook = NULL;
    recovery_before_source_bind_user = NULL;
    recovery_before_destination_publish_hook = NULL;
    recovery_before_destination_publish_user = NULL;
}
#endif

typedef struct {
    bool active;
    char source_player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char source_path[PERSISTENCE_GENERATION_PATH_MAX];
    char destination_path[PERSISTENCE_GENERATION_PATH_MAX];
    char source_leaf[sizeof("player_") + 16 + sizeof(".sav")];
    char destination_leaf[72];
    uint64_t expected_size;
    uint8_t expected_sha256[32];
#ifdef _WIN32
    HANDLE source_handle;
    BY_HANDLE_FILE_INFORMATION identity;
    HANDLE parent_handles[64];
    size_t parent_count;
#else
    int player_fd;
    int parent_fd;
    int destination_parent_fd;
    int source_fd;
    dev_t player_device;
    ino_t player_inode;
    dev_t legacy_device;
    ino_t legacy_inode;
    dev_t pubkey_device;
    ino_t pubkey_inode;
    dev_t device;
    ino_t inode;
#endif
} recovery_source_guard_t;

#ifndef _WIN32
static bool recovery_secure_namespace_fds(
    const char *player_dir,
    int *player_out,
    int *legacy_out,
    int *pubkey_out) {
    if (player_out) *player_out = -1;
    if (legacy_out) *legacy_out = -1;
    if (pubkey_out) *pubkey_out = -1;
    if (!player_dir || !player_dir[0] ||
        !player_out || !legacy_out || !pubkey_out) {
        return false;
    }
#ifndef O_NOFOLLOW
    return false;
#else
    int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int player_fd = open(player_dir, flags);
    if (player_fd < 0) return false;
    int legacy_fd = openat(player_fd, "legacy", flags);
    int pubkey_fd = openat(player_fd, "pubkey", flags);
    if (legacy_fd < 0 || pubkey_fd < 0) {
        if (legacy_fd >= 0) close(legacy_fd);
        if (pubkey_fd >= 0) close(pubkey_fd);
        close(player_fd);
        return false;
    }
    *player_out = player_fd;
    *legacy_out = legacy_fd;
    *pubkey_out = pubkey_fd;
    return true;
#endif
}

static bool recovery_source_hash_fd(
    int fd, uint64_t *size_out, uint8_t digest[32]) {
    if (fd < 0 || !size_out || !digest ||
        lseek(fd, 0, SEEK_SET) < 0) {
        return false;
    }
    sha256_ctx_t sha;
    sha256_init(&sha);
    uint64_t size = 0;
    uint8_t chunk[16384];
    for (;;) {
        ssize_t count = read(fd, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return false;
        if (count == 0) break;
        if (UINT64_MAX - size < (uint64_t)count)
            return false;
        size += (uint64_t)count;
        sha256_update(&sha, chunk, (size_t)count);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) return false;
    sha256_final(&sha, digest);
    *size_out = size;
    return true;
}
#else
static void recovery_source_guard_close(
    recovery_source_guard_t *guard);

static bool recovery_windows_lock_parents(
    const char *player_dir, recovery_source_guard_t *guard) {
    if (!player_dir || !guard) return false;
    char legacy_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char pubkey_dir[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(
            legacy_dir, sizeof(legacy_dir),
            player_dir, "legacy") ||
        !path_join(
            pubkey_dir, sizeof(pubkey_dir),
            player_dir, "pubkey")) {
        return false;
    }
    const char *directories[] = {
        player_dir, legacy_dir, pubkey_dir
    };
    for (size_t i = 0;
         i < sizeof(directories) / sizeof(directories[0]); i++) {
        HANDLE directory = CreateFileA(
            directories[i], FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            NULL, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT,
            NULL);
        if (directory == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(directory, &info) ||
            (info.dwFileAttributes &
             FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes &
             FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            CloseHandle(directory);
            return false;
        }
        guard->parent_handles[guard->parent_count++] =
            directory;
    }
    return true;
}

static bool recovery_source_hash_handle(
    HANDLE handle, uint64_t *size_out, uint8_t digest[32]) {
    if (handle == INVALID_HANDLE_VALUE ||
        !size_out || !digest) {
        return false;
    }
    LARGE_INTEGER zero = {.QuadPart = 0};
    if (!SetFilePointerEx(handle, zero, NULL, FILE_BEGIN))
        return false;
    sha256_ctx_t sha;
    sha256_init(&sha);
    uint64_t size = 0;
    uint8_t chunk[16384];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(handle, chunk, sizeof(chunk),
                      &count, NULL)) {
            return false;
        }
        if (count == 0) break;
        size += count;
        sha256_update(&sha, chunk, count);
    }
    if (!SetFilePointerEx(handle, zero, NULL, FILE_BEGIN))
        return false;
    sha256_final(&sha, digest);
    *size_out = size;
    return true;
}
#endif

static void recovery_source_guard_close(
    recovery_source_guard_t *guard) {
    if (!guard) return;
#ifdef _WIN32
    if (guard->source_handle != INVALID_HANDLE_VALUE)
        CloseHandle(guard->source_handle);
    for (size_t i = 0; i < guard->parent_count; i++) {
        if (guard->parent_handles[i] != INVALID_HANDLE_VALUE)
            CloseHandle(guard->parent_handles[i]);
    }
#else
    if (guard->source_fd >= 0) close(guard->source_fd);
    if (guard->destination_parent_fd >= 0)
        close(guard->destination_parent_fd);
    if (guard->parent_fd >= 0) close(guard->parent_fd);
    if (guard->player_fd >= 0) close(guard->player_fd);
#endif
    memset(guard, 0, sizeof(*guard));
#ifdef _WIN32
    guard->source_handle = INVALID_HANDLE_VALUE;
#else
    guard->source_fd = -1;
    guard->parent_fd = -1;
    guard->destination_parent_fd = -1;
    guard->player_fd = -1;
#endif
}

static persistence_recovery_commit_result_t
recovery_source_guard_open(
    recovery_source_guard_t *guard,
    const char *source_player_dir,
    const char *source_path,
    const char *destination_path,
    uint64_t expected_size,
    const uint8_t expected_sha256[32]) {
    if (!guard || !source_player_dir || !source_path ||
        !destination_path || !expected_sha256 ||
        expected_size == 0) {
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }
    memset(guard, 0, sizeof(*guard));
#ifdef _WIN32
    guard->source_handle = INVALID_HANDLE_VALUE;
#else
    guard->source_fd = -1;
    guard->parent_fd = -1;
    guard->destination_parent_fd = -1;
    guard->player_fd = -1;
#endif
    int player_dir_len = snprintf(
        guard->source_player_dir,
        sizeof(guard->source_player_dir),
        "%s", source_player_dir);
    int n = snprintf(
        guard->source_path, sizeof(guard->source_path),
        "%s", source_path);
    int destination_len = snprintf(
        guard->destination_path,
        sizeof(guard->destination_path),
        "%s", destination_path);
    if (player_dir_len <= 0 ||
        (size_t)player_dir_len >=
            sizeof(guard->source_player_dir) ||
        n <= 0 || (size_t)n >= sizeof(guard->source_path) ||
        destination_len <= 0 ||
        (size_t)destination_len >=
            sizeof(guard->destination_path)) {
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }
    guard->expected_size = expected_size;
    memcpy(guard->expected_sha256, expected_sha256, 32);
    const char *source_leaf = strrchr(source_path, '/');
    if (!source_leaf || !source_leaf[1]) {
        recovery_source_guard_close(guard);
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }
    source_leaf++;
    const char *destination_leaf =
        strrchr(destination_path, '/');
    int leaf_len = snprintf(
        guard->source_leaf, sizeof(guard->source_leaf),
        "%s", source_leaf);
    int destination_leaf_len =
        destination_leaf && destination_leaf[1]
        ? snprintf(
            guard->destination_leaf,
            sizeof(guard->destination_leaf),
            "%s", destination_leaf + 1)
        : -1;
    if (leaf_len <= 0 ||
        (size_t)leaf_len >= sizeof(guard->source_leaf) ||
        destination_leaf_len <= 0 ||
        (size_t)destination_leaf_len >=
            sizeof(guard->destination_leaf)) {
        recovery_source_guard_close(guard);
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }

#ifdef _WIN32
    if (!recovery_windows_lock_parents(
            source_player_dir, guard)) {
        recovery_source_guard_close(guard);
        return PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
    }
    guard->source_handle = CreateFileA(
        source_path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        NULL);
    uint64_t actual_size = 0;
    uint8_t actual_digest[32];
    DWORD destination_attributes =
        GetFileAttributesA(destination_path);
    DWORD destination_error =
        destination_attributes == INVALID_FILE_ATTRIBUTES
        ? GetLastError() : ERROR_SUCCESS;
    bool destination_missing =
        destination_attributes == INVALID_FILE_ATTRIBUTES &&
        (destination_error == ERROR_FILE_NOT_FOUND ||
         destination_error == ERROR_PATH_NOT_FOUND);
    bool ok =
        guard->source_handle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(
            guard->source_handle, &guard->identity) != 0 &&
        (guard->identity.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY |
          FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        guard->identity.nNumberOfLinks == 1 &&
        recovery_source_hash_handle(
            guard->source_handle, &actual_size,
            actual_digest) &&
        actual_size == expected_size &&
        memcmp(actual_digest, expected_sha256, 32) == 0;
    if (ok && !destination_missing) {
        recovery_source_guard_close(guard);
        return destination_attributes != INVALID_FILE_ATTRIBUTES
            ? PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT
            : PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
    }
#else
    bool namespaces_open = recovery_secure_namespace_fds(
        source_player_dir,
        &guard->player_fd,
        &guard->parent_fd,
        &guard->destination_parent_fd);
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#else
    recovery_source_guard_close(guard);
    return PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    if (namespaces_open)
        guard->source_fd = openat(
            guard->parent_fd, guard->source_leaf, flags);
    struct stat player_info;
    struct stat legacy_info;
    struct stat pubkey_info;
    struct stat info;
    struct stat destination_info;
    int destination_found =
        namespaces_open
        ? fstatat(
            guard->destination_parent_fd,
            guard->destination_leaf,
            &destination_info,
            AT_SYMLINK_NOFOLLOW)
        : -1;
    int destination_errno = errno;
    uint64_t actual_size = 0;
    uint8_t actual_digest[32];
    bool ok =
        namespaces_open &&
        fstat(guard->player_fd, &player_info) == 0 &&
        S_ISDIR(player_info.st_mode) &&
        fstat(guard->parent_fd, &legacy_info) == 0 &&
        S_ISDIR(legacy_info.st_mode) &&
        fstat(guard->destination_parent_fd, &pubkey_info) == 0 &&
        S_ISDIR(pubkey_info.st_mode) &&
        guard->source_fd >= 0 &&
        fstat(guard->source_fd, &info) == 0 &&
        S_ISREG(info.st_mode) &&
        info.st_nlink == 1 &&
        recovery_source_hash_fd(
            guard->source_fd, &actual_size,
            actual_digest) &&
        actual_size == expected_size &&
        memcmp(actual_digest, expected_sha256, 32) == 0;
    if (ok && destination_found == 0) {
        recovery_source_guard_close(guard);
        return PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT;
    }
    if (ok && destination_errno != ENOENT) ok = false;
    if (ok) {
        guard->player_device = player_info.st_dev;
        guard->player_inode = player_info.st_ino;
        guard->legacy_device = legacy_info.st_dev;
        guard->legacy_inode = legacy_info.st_ino;
        guard->pubkey_device = pubkey_info.st_dev;
        guard->pubkey_inode = pubkey_info.st_ino;
        guard->device = info.st_dev;
        guard->inode = info.st_ino;
    }
#endif
    if (!ok) {
        recovery_source_guard_close(guard);
        return PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
    }
    guard->active = true;
    return PERSISTENCE_RECOVERY_COMMIT_OK;
}

static persistence_recovery_commit_result_t
recovery_source_guard_validate(
    recovery_source_guard_t *guard) {
    if (!guard || !guard->active)
        return PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
    uint64_t actual_size = 0;
    uint8_t actual_digest[32];
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    bool source_matches =
        GetFileInformationByHandle(
            guard->source_handle, &info) != 0 &&
        (info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY |
          FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        info.nNumberOfLinks == 1 &&
        info.dwVolumeSerialNumber ==
            guard->identity.dwVolumeSerialNumber &&
        info.nFileIndexHigh ==
            guard->identity.nFileIndexHigh &&
        info.nFileIndexLow ==
            guard->identity.nFileIndexLow &&
        recovery_source_hash_handle(
            guard->source_handle, &actual_size,
            actual_digest) &&
        actual_size == guard->expected_size &&
        memcmp(actual_digest,
               guard->expected_sha256, 32) == 0;
    if (!source_matches)
        return PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
    DWORD attributes =
        GetFileAttributesA(guard->destination_path);
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT;
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND ||
           error == ERROR_PATH_NOT_FOUND
        ? PERSISTENCE_RECOVERY_COMMIT_OK
        : PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
#else
    struct stat player_path_info;
    struct stat player_handle_info;
    struct stat legacy_handle_info;
    struct stat legacy_named_info;
    struct stat pubkey_handle_info;
    struct stat pubkey_named_info;
    struct stat handle_info;
    struct stat named_info;
    bool source_matches =
        lstat(guard->source_player_dir,
              &player_path_info) == 0 &&
        S_ISDIR(player_path_info.st_mode) &&
        player_path_info.st_dev == guard->player_device &&
        player_path_info.st_ino == guard->player_inode &&
        fstat(guard->player_fd, &player_handle_info) == 0 &&
        S_ISDIR(player_handle_info.st_mode) &&
        player_handle_info.st_dev == guard->player_device &&
        player_handle_info.st_ino == guard->player_inode &&
        fstat(guard->parent_fd, &legacy_handle_info) == 0 &&
        S_ISDIR(legacy_handle_info.st_mode) &&
        legacy_handle_info.st_dev == guard->legacy_device &&
        legacy_handle_info.st_ino == guard->legacy_inode &&
        fstatat(guard->player_fd, "legacy",
                &legacy_named_info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(legacy_named_info.st_mode) &&
        legacy_named_info.st_dev == guard->legacy_device &&
        legacy_named_info.st_ino == guard->legacy_inode &&
        fstat(guard->destination_parent_fd,
              &pubkey_handle_info) == 0 &&
        S_ISDIR(pubkey_handle_info.st_mode) &&
        pubkey_handle_info.st_dev == guard->pubkey_device &&
        pubkey_handle_info.st_ino == guard->pubkey_inode &&
        fstatat(guard->player_fd, "pubkey",
                &pubkey_named_info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISDIR(pubkey_named_info.st_mode) &&
        pubkey_named_info.st_dev == guard->pubkey_device &&
        pubkey_named_info.st_ino == guard->pubkey_inode &&
        fstat(guard->source_fd, &handle_info) == 0 &&
        S_ISREG(handle_info.st_mode) &&
        handle_info.st_nlink == 1 &&
        handle_info.st_dev == guard->device &&
        handle_info.st_ino == guard->inode &&
        fstatat(guard->parent_fd, guard->source_leaf,
                &named_info, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(named_info.st_mode) &&
        named_info.st_nlink == 1 &&
        named_info.st_dev == guard->device &&
        named_info.st_ino == guard->inode &&
        recovery_source_hash_fd(
            guard->source_fd, &actual_size,
            actual_digest) &&
        actual_size == guard->expected_size &&
        memcmp(actual_digest,
               guard->expected_sha256, 32) == 0;
    if (!source_matches)
        return PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
    struct stat destination_info;
    if (fstatat(
            guard->destination_parent_fd,
            guard->destination_leaf,
            &destination_info,
            AT_SYMLINK_NOFOLLOW) == 0) {
        return PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT;
    }
    return errno == ENOENT
        ? PERSISTENCE_RECOVERY_COMMIT_OK
        : PERSISTENCE_RECOVERY_COMMIT_SOURCE_CHANGED;
#endif
}

static persistence_recovery_commit_result_t
copy_player_namespace_for_recovery(
    const char *source_dir,
    const char *destination_dir,
    const uint8_t token[8],
    const uint8_t pubkey[32]) {
    if (!source_dir || !destination_dir || !token || !pubkey ||
        !ensure_directory(destination_dir) ||
        fs_node_kind(source_dir) != FS_NODE_DIRECTORY) {
        return PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    }
    if (!copy_player_save_files(
            source_dir, destination_dir, true)) {
        return PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    }

    char source_name[sizeof("player_") + 16 + sizeof(".sav")];
    char destination_name[72];
    if (!recovery_source_name(source_name, token) ||
        !recovery_destination_name(destination_name, pubkey)) {
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }

    char source_legacy[PERSISTENCE_GENERATION_PATH_MAX];
    char destination_legacy[PERSISTENCE_GENERATION_PATH_MAX];
    char source_pubkey[PERSISTENCE_GENERATION_PATH_MAX];
    char destination_pubkey[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(source_legacy, sizeof(source_legacy),
                   source_dir, "legacy") ||
        !path_join(destination_legacy, sizeof(destination_legacy),
                   destination_dir, "legacy") ||
        !path_join(source_pubkey, sizeof(source_pubkey),
                   source_dir, "pubkey") ||
        !path_join(destination_pubkey, sizeof(destination_pubkey),
                   destination_dir, "pubkey") ||
        !ensure_directory(destination_legacy) ||
        !ensure_directory(destination_pubkey)) {
        return PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    }
    if ((fs_node_kind(source_legacy) != FS_NODE_DIRECTORY &&
         !fs_path_missing(source_legacy)) ||
        (fs_node_kind(source_pubkey) != FS_NODE_DIRECTORY &&
         !fs_path_missing(source_pubkey))) {
        return PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    }

    bool saw_source = false;
    if (!copy_player_save_files_filtered(
            source_legacy, destination_legacy, true,
            source_name, NULL, &saw_source, NULL)) {
        return PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    }
    if (!saw_source) return PERSISTENCE_RECOVERY_COMMIT_NO_SOURCE;

    bool saw_destination = false;
    if (!copy_player_save_files_filtered(
            source_pubkey, destination_pubkey, true,
            NULL, destination_name, NULL, &saw_destination)) {
        return saw_destination
            ? PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT
            : PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    }
    return PERSISTENCE_RECOVERY_COMMIT_OK;
}

static void manifest_entries_free(manifest_entries_t *entries) {
    if (!entries) return;
    for (size_t i = 0; i < entries->count; i++)
        free(entries->items[i].path);
    free(entries->items);
    memset(entries, 0, sizeof(*entries));
}

static bool file_sha256(const char *path, uint64_t *size_out,
                        uint8_t digest[32]) {
    if (!path || !size_out || !digest ||
        fs_node_kind(path) != FS_NODE_REGULAR) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    sha256_ctx_t sha;
    sha256_init(&sha);
    uint64_t size = 0;
    uint8_t chunk[16384];
    size_t count;
    while ((count = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (UINT64_MAX - size < (uint64_t)count) {
            fclose(f);
            return false;
        }
        size += (uint64_t)count;
        sha256_update(&sha, chunk, count);
    }
    bool ok = !ferror(f) && fclose(f) == 0;
    if (!ok) return false;
    sha256_final(&sha, digest);
    *size_out = size;
    return true;
}

static bool manifest_path_valid(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\'))
        return false;
    if (strcmp(path, GENERATION_MANIFEST_NAME) == 0 ||
        strcmp(path, GENERATION_MANIFEST_NAME ".tmp") == 0) {
        return false;
    }
    const char *component = path;
    for (const char *p = path;; p++) {
        if (*p != '/' && *p != '\0') continue;
        size_t len = (size_t)(p - component);
        if (len == 0 ||
            (len == 1 && component[0] == '.') ||
            (len == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (*p == '\0') break;
        component = p + 1;
    }
    return true;
}

static bool manifest_entries_push(manifest_entries_t *entries,
                                  const char *relative_path,
                                  const char *absolute_path) {
    if (!entries || !manifest_path_valid(relative_path) ||
        entries->count >= GENERATION_MAX_ENTRIES) {
        return false;
    }
    if (entries->count >= entries->capacity) {
        size_t next = entries->capacity ? entries->capacity * 2u : 32u;
        if (next > GENERATION_MAX_ENTRIES)
            next = GENERATION_MAX_ENTRIES;
        if (next <= entries->capacity ||
            next > SIZE_MAX / sizeof(*entries->items)) {
            return false;
        }
        manifest_entry_t *grown = realloc(
            entries->items, next * sizeof(*entries->items));
        if (!grown) return false;
        entries->items = grown;
        entries->capacity = next;
    }
    size_t len = strlen(relative_path);
    if (len > UINT16_MAX) return false;
    char *copy = malloc(len + 1u);
    if (!copy) return false;
    memcpy(copy, relative_path, len + 1u);
    manifest_entry_t *entry = &entries->items[entries->count];
    memset(entry, 0, sizeof(*entry));
    entry->path = copy;
    if (!file_sha256(absolute_path, &entry->size, entry->sha256)) {
        free(copy);
        entry->path = NULL;
        return false;
    }
    entries->count++;
    return true;
}

static int manifest_entry_compare(const void *left, const void *right) {
    const manifest_entry_t *a = left;
    const manifest_entry_t *b = right;
    return strcmp(a->path, b->path);
}

static bool collect_generation_files_recursive(
    const char *generation_dir,
    const char *relative_dir,
    int depth,
    manifest_entries_t *entries) {
    if (depth > GENERATION_MAX_DEPTH) return false;
    char absolute_dir[PERSISTENCE_GENERATION_PATH_MAX];
    if (relative_dir[0]) {
        if (!path_join(absolute_dir, sizeof(absolute_dir),
                       generation_dir, relative_dir)) {
            return false;
        }
    } else {
        int n = snprintf(absolute_dir, sizeof(absolute_dir), "%s",
                         generation_dir);
        if (n <= 0 || (size_t)n >= sizeof(absolute_dir)) return false;
    }

    directory_entries_t directory;
    if (!directory_entries_read(absolute_dir, &directory, false))
        return false;
    bool ok = true;
    for (size_t i = 0; i < directory.count; i++) {
        const directory_entry_t *node = &directory.items[i];
        if (!relative_dir[0] &&
            (strcmp(node->name, GENERATION_MANIFEST_NAME) == 0 ||
             strcmp(node->name, GENERATION_MANIFEST_NAME ".tmp") == 0)) {
            continue;
        }
        char relative[PERSISTENCE_GENERATION_PATH_MAX];
        if (relative_dir[0]) {
            if (!path_join(relative, sizeof(relative),
                           relative_dir, node->name)) {
                ok = false;
                break;
            }
        } else {
            int n = snprintf(relative, sizeof(relative), "%s", node->name);
            if (n <= 0 || (size_t)n >= sizeof(relative)) {
                ok = false;
                break;
            }
        }
        char absolute[PERSISTENCE_GENERATION_PATH_MAX];
        if (!path_join(absolute, sizeof(absolute),
                       generation_dir, relative)) {
            ok = false;
            break;
        }
        if (node->kind == FS_NODE_DIRECTORY) {
            if (!collect_generation_files_recursive(
                    generation_dir, relative, depth + 1, entries)) {
                ok = false;
                break;
            }
        } else if (node->kind == FS_NODE_REGULAR) {
            if (!manifest_entries_push(entries, relative, absolute)) {
                ok = false;
                break;
            }
        } else {
            ok = false;
            break;
        }
    }
    directory_entries_free(&directory);
    return ok;
}

static bool collect_generation_files(const char *generation_dir,
                                     manifest_entries_t *entries) {
    if (!generation_dir || !entries) return false;
    memset(entries, 0, sizeof(*entries));
    if (!collect_generation_files_recursive(
            generation_dir, "", 0, entries)) {
        manifest_entries_free(entries);
        return false;
    }
    qsort(entries->items, entries->count, sizeof(*entries->items),
          manifest_entry_compare);
    bool saw_world = false;
    for (size_t i = 0; i < entries->count; i++) {
        if (i > 0 &&
            strcmp(entries->items[i - 1].path,
                   entries->items[i].path) >= 0) {
            manifest_entries_free(entries);
            return false;
        }
        if (strcmp(entries->items[i].path, "world.sav") == 0)
            saw_world = true;
    }
    if (!saw_world) {
        manifest_entries_free(entries);
        return false;
    }
    return true;
}

static bool write_exact(FILE *f, const void *data, size_t size) {
    return f && data && fwrite(data, 1, size, f) == size;
}

static bool read_exact(FILE *f, void *data, size_t size) {
    return f && data && fread(data, 1, size, f) == size;
}

static bool write_u16_le(FILE *f, uint16_t value) {
    uint8_t bytes[2] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
    };
    return write_exact(f, bytes, sizeof(bytes));
}

static bool write_u32_le(FILE *f, uint32_t value) {
    uint8_t bytes[4] = {
        (uint8_t)value,
        (uint8_t)(value >> 8),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 24),
    };
    return write_exact(f, bytes, sizeof(bytes));
}

static bool write_u64_le(FILE *f, uint64_t value) {
    uint8_t bytes[8];
    for (size_t i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t)(value >> (i * 8u));
    return write_exact(f, bytes, sizeof(bytes));
}

static bool read_u16_le(FILE *f, uint16_t *value) {
    uint8_t bytes[2];
    if (!value || !read_exact(f, bytes, sizeof(bytes))) return false;
    *value = (uint16_t)bytes[0] |
             ((uint16_t)bytes[1] << 8);
    return true;
}

static bool read_u32_le(FILE *f, uint32_t *value) {
    uint8_t bytes[4];
    if (!value || !read_exact(f, bytes, sizeof(bytes))) return false;
    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8) |
             ((uint32_t)bytes[2] << 16) |
             ((uint32_t)bytes[3] << 24);
    return true;
}

static bool read_u64_le(FILE *f, uint64_t *value) {
    uint8_t bytes[8];
    if (!value || !read_exact(f, bytes, sizeof(bytes))) return false;
    uint64_t decoded = 0;
    for (size_t i = 0; i < sizeof(bytes); i++)
        decoded |= (uint64_t)bytes[i] << (i * 8u);
    *value = decoded;
    return true;
}

static bool write_generation_manifest(
    const char *generation_dir,
    uint64_t generation,
    const manifest_entries_t *entries,
    uint8_t manifest_sha256[32]) {
    if (!generation_dir || generation == 0 || !entries ||
        entries->count > UINT32_MAX || !manifest_sha256) {
        return false;
    }
    char temporary[PERSISTENCE_GENERATION_PATH_MAX];
    char final[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(temporary, sizeof(temporary), generation_dir,
                   GENERATION_MANIFEST_NAME ".tmp") ||
        !path_join(final, sizeof(final), generation_dir,
                   GENERATION_MANIFEST_NAME)) {
        return false;
    }
    FILE *f = fopen(temporary, "wb");
    if (!f) return false;
    bool ok =
        write_exact(f, GENERATION_MANIFEST_MAGIC, 8) &&
        write_u32_le(f, GENERATION_FORMAT_VERSION) &&
        write_u64_le(f, generation) &&
        write_u32_le(f, (uint32_t)entries->count);
    for (size_t i = 0; ok && i < entries->count; i++) {
        size_t path_len = strlen(entries->items[i].path);
        ok = path_len <= UINT16_MAX &&
             write_u16_le(f, (uint16_t)path_len) &&
             write_u64_le(f, entries->items[i].size) &&
             write_exact(f, entries->items[i].sha256, 32) &&
             write_exact(f, entries->items[i].path, path_len);
    }
    if (ok) ok = persistence_flush_durable(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    if (!persistence_replace_file(temporary, final)) {
        remove(temporary);
        return false;
    }
    uint64_t ignored_size = 0;
    return file_sha256(final, &ignored_size, manifest_sha256);
}

/*
 * This bearer-free marker is covered by the immutable generation manifest.
 * The atomic CURRENT publication therefore selects either the old spendable
 * source or a generation that records its consumption, never an in-between
 * state. Recovery pointers also cut the rollback edge to the source-bearing
 * predecessor.
 */
static bool write_recovery_consumption_marker(
    const char *generation_dir,
    const uint8_t session_token[8],
    const uint8_t pubkey[32],
    uint64_t source_size,
    const uint8_t source_sha256[32]) {
    static const char claim_domain[] =
        "SIGNAL-legacy-recovery-consumed-v1";
    if (!generation_dir || !session_token || !pubkey ||
        source_size == 0 || !source_sha256) {
        return false;
    }
    uint8_t claim_digest[32];
    sha256_ctx_t claim_hash;
    sha256_init(&claim_hash);
    sha256_update(&claim_hash, claim_domain,
                  sizeof(claim_domain) - 1u);
    sha256_update(&claim_hash, session_token, 8);
    sha256_final(&claim_hash, claim_digest);

    uint8_t encoded[8 + 32 + 32 + 8 + 32];
    size_t offset = 0;
    memcpy(encoded + offset, GENERATION_RECOVERY_MARKER_MAGIC, 8);
    offset += 8;
    memcpy(encoded + offset, claim_digest, sizeof(claim_digest));
    offset += sizeof(claim_digest);
    memcpy(encoded + offset, pubkey, 32);
    offset += 32;
    for (size_t i = 0; i < 8; i++)
        encoded[offset + i] = (uint8_t)(source_size >> (i * 8u));
    offset += 8;
    memcpy(encoded + offset, source_sha256, 32);
    offset += 32;
    if (offset != sizeof(encoded)) return false;

    char temporary[PERSISTENCE_GENERATION_PATH_MAX];
    char final[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(temporary, sizeof(temporary), generation_dir,
                   GENERATION_RECOVERY_MARKER_NAME ".tmp") ||
        !path_join(final, sizeof(final), generation_dir,
                   GENERATION_RECOVERY_MARKER_NAME)) {
        return false;
    }
    FILE *f = fopen(temporary, "wb");
    if (!f) return false;
    bool ok = write_exact(f, encoded, sizeof(encoded)) &&
              persistence_flush_durable(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    if (!persistence_replace_file(temporary, final)) {
        remove(temporary);
        return false;
    }
    return true;
}

static bool load_generation_manifest(
    const char *generation_dir,
    uint64_t expected_generation,
    manifest_entries_t *entries) {
    if (!generation_dir || expected_generation == 0 || !entries)
        return false;
    memset(entries, 0, sizeof(*entries));
    char path[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(path, sizeof(path), generation_dir,
                   GENERATION_MANIFEST_NAME)) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t magic[8];
    uint32_t version = 0;
    uint64_t generation = 0;
    uint32_t count = 0;
    bool ok =
        read_exact(f, magic, sizeof(magic)) &&
        memcmp(magic, GENERATION_MANIFEST_MAGIC, sizeof(magic)) == 0 &&
        read_u32_le(f, &version) &&
        version == GENERATION_FORMAT_VERSION &&
        read_u64_le(f, &generation) &&
        generation == expected_generation &&
        read_u32_le(f, &count) &&
        count > 0 && count <= GENERATION_MAX_ENTRIES;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint16_t path_len = 0;
        uint64_t size = 0;
        uint8_t digest[32];
        ok = read_u16_le(f, &path_len) &&
             path_len > 0 &&
             path_len < PERSISTENCE_GENERATION_PATH_MAX &&
             read_u64_le(f, &size) &&
             read_exact(f, digest, sizeof(digest));
        if (!ok) break;
        char relative[PERSISTENCE_GENERATION_PATH_MAX];
        ok = read_exact(f, relative, path_len);
        if (!ok) break;
        relative[path_len] = '\0';
        char absolute[PERSISTENCE_GENERATION_PATH_MAX];
        ok = strlen(relative) == (size_t)path_len &&
             manifest_path_valid(relative) &&
             path_join(absolute, sizeof(absolute),
                       generation_dir, relative) &&
             manifest_entries_push(entries, relative, absolute);
        if (!ok) break;
        manifest_entry_t *loaded = &entries->items[entries->count - 1u];
        ok = loaded->size == size &&
             memcmp(loaded->sha256, digest, sizeof(digest)) == 0 &&
             (entries->count == 1u ||
              strcmp(entries->items[entries->count - 2u].path,
                     loaded->path) < 0);
    }
    int trailing = ok ? fgetc(f) : 0;
    if (ok) ok = trailing == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        manifest_entries_free(entries);
        return false;
    }
    return true;
}

static bool generation_manifest_valid(
    const char *root_dir,
    uint64_t generation,
    const uint8_t expected_manifest_sha256[32]) {
    char generation_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char manifest_path[PERSISTENCE_GENERATION_PATH_MAX];
    char world_path[PERSISTENCE_GENERATION_PATH_MAX];
    char catalog_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char legacy_player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    char pubkey_player_dir[PERSISTENCE_GENERATION_PATH_MAX];
    if (!generation_dir_path(generation_dir, sizeof(generation_dir),
                             root_dir, generation) ||
        !path_join(manifest_path, sizeof(manifest_path),
                   generation_dir, GENERATION_MANIFEST_NAME) ||
        !path_join(world_path, sizeof(world_path),
                   generation_dir, "world.sav") ||
        !path_join(catalog_dir, sizeof(catalog_dir),
                   generation_dir, "stations") ||
        !path_join(player_dir, sizeof(player_dir),
                   generation_dir, "players") ||
        !path_join(legacy_player_dir, sizeof(legacy_player_dir),
                   player_dir, "legacy") ||
        !path_join(pubkey_player_dir, sizeof(pubkey_player_dir),
                   player_dir, "pubkey") ||
        fs_node_kind(generation_dir) != FS_NODE_DIRECTORY ||
        fs_node_kind(world_path) != FS_NODE_REGULAR ||
        fs_node_kind(catalog_dir) != FS_NODE_DIRECTORY ||
        fs_node_kind(player_dir) != FS_NODE_DIRECTORY ||
        fs_node_kind(legacy_player_dir) != FS_NODE_DIRECTORY ||
        fs_node_kind(pubkey_player_dir) != FS_NODE_DIRECTORY) {
        return false;
    }
    uint64_t manifest_size = 0;
    uint8_t actual_manifest_sha256[32];
    if (!file_sha256(manifest_path, &manifest_size,
                     actual_manifest_sha256) ||
        memcmp(actual_manifest_sha256, expected_manifest_sha256, 32) != 0) {
        return false;
    }

    manifest_entries_t declared;
    manifest_entries_t actual;
    if (!load_generation_manifest(
            generation_dir, generation, &declared)) {
        return false;
    }
    bool ok = collect_generation_files(generation_dir, &actual);
    if (ok) ok = declared.count == actual.count;
    for (size_t i = 0; ok && i < declared.count; i++) {
        ok = strcmp(declared.items[i].path, actual.items[i].path) == 0 &&
             declared.items[i].size == actual.items[i].size &&
             memcmp(declared.items[i].sha256,
                    actual.items[i].sha256, 32) == 0;
    }
    manifest_entries_free(&actual);
    manifest_entries_free(&declared);
    return ok;
}

static void put_u32_le(uint8_t out[4], uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_u64_le(uint8_t out[8], uint64_t value) {
    for (size_t i = 0; i < 8; i++)
        out[i] = (uint8_t)(value >> (i * 8u));
}

static uint32_t get_u32_le(const uint8_t in[4]) {
    return (uint32_t)in[0] |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static uint64_t get_u64_le(const uint8_t in[8]) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++)
        value |= (uint64_t)in[i] << (i * 8u);
    return value;
}

enum {
    POINTER_PAYLOAD_SIZE = 8 + 4 + 8 + 32 + 8 + 32,
    POINTER_FILE_SIZE = POINTER_PAYLOAD_SIZE + 32,
};

static bool pointer_encode(const generation_pointer_t *pointer,
                           uint8_t encoded[POINTER_FILE_SIZE]) {
    if (!pointer || !encoded || pointer->current_generation == 0 ||
        (pointer->previous_generation != 0 &&
         pointer->previous_generation >= pointer->current_generation)) {
        return false;
    }
    size_t offset = 0;
    memcpy(encoded + offset, GENERATION_POINTER_MAGIC, 8);
    offset += 8;
    put_u32_le(encoded + offset, GENERATION_FORMAT_VERSION);
    offset += 4;
    put_u64_le(encoded + offset, pointer->current_generation);
    offset += 8;
    memcpy(encoded + offset, pointer->current_manifest_sha256, 32);
    offset += 32;
    put_u64_le(encoded + offset, pointer->previous_generation);
    offset += 8;
    memcpy(encoded + offset, pointer->previous_manifest_sha256, 32);
    offset += 32;
    if (offset != POINTER_PAYLOAD_SIZE) return false;
    sha256_bytes(encoded, POINTER_PAYLOAD_SIZE, encoded + offset);
    return true;
}

static bool pointer_decode(const uint8_t encoded[POINTER_FILE_SIZE],
                           generation_pointer_t *pointer) {
    if (!encoded || !pointer ||
        memcmp(encoded, GENERATION_POINTER_MAGIC, 8) != 0 ||
        get_u32_le(encoded + 8) != GENERATION_FORMAT_VERSION) {
        return false;
    }
    uint8_t digest[32];
    sha256_bytes(encoded, POINTER_PAYLOAD_SIZE, digest);
    if (memcmp(digest, encoded + POINTER_PAYLOAD_SIZE, 32) != 0)
        return false;
    memset(pointer, 0, sizeof(*pointer));
    pointer->current_generation = get_u64_le(encoded + 12);
    memcpy(pointer->current_manifest_sha256, encoded + 20, 32);
    pointer->previous_generation = get_u64_le(encoded + 52);
    memcpy(pointer->previous_manifest_sha256, encoded + 60, 32);
    return pointer->current_generation != 0 &&
           (pointer->previous_generation == 0 ||
            pointer->previous_generation < pointer->current_generation);
}

static bool pointer_read(const char *root_dir,
                         generation_pointer_t *pointer,
                         bool *missing) {
    if (missing) *missing = false;
    char path[PERSISTENCE_GENERATION_PATH_MAX];
    if (!path_join(path, sizeof(path), root_dir,
                   GENERATION_POINTER_NAME)) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (missing && errno == ENOENT) *missing = true;
        return false;
    }
    uint8_t encoded[POINTER_FILE_SIZE];
    bool ok = read_exact(f, encoded, sizeof(encoded));
    int trailing = ok ? fgetc(f) : 0;
    if (ok) ok = trailing == EOF && !ferror(f);
    if (fclose(f) != 0) ok = false;
    return ok && pointer_decode(encoded, pointer);
}

static bool pointer_write_temporary(
    const char *root_dir,
    const generation_pointer_t *pointer,
    char temporary[PERSISTENCE_GENERATION_PATH_MAX],
    char final[PERSISTENCE_GENERATION_PATH_MAX]) {
    if (!path_join(temporary, PERSISTENCE_GENERATION_PATH_MAX,
                   root_dir, GENERATION_POINTER_NAME ".tmp") ||
        !path_join(final, PERSISTENCE_GENERATION_PATH_MAX,
                   root_dir, GENERATION_POINTER_NAME)) {
        return false;
    }
    uint8_t encoded[POINTER_FILE_SIZE];
    if (!pointer_encode(pointer, encoded)) return false;
    FILE *f = fopen(temporary, "wb");
    if (!f) return false;
    bool ok = write_exact(f, encoded, sizeof(encoded)) &&
              persistence_flush_durable(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        remove(temporary);
        return false;
    }
    return true;
}

persistence_generation_status_t persistence_generation_resolve(
    const char *root_dir,
    persistence_generation_paths_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!root_dir || !root_dir[0] || !out)
        return PERSISTENCE_GENERATION_INVALID;

    generation_pointer_t pointer;
    bool missing = false;
    if (!pointer_read(root_dir, &pointer, &missing))
        return missing ? PERSISTENCE_GENERATION_NONE
                       : PERSISTENCE_GENERATION_INVALID;

    if (generation_manifest_valid(
            root_dir, pointer.current_generation,
            pointer.current_manifest_sha256) &&
        generation_paths_fill(
            root_dir, pointer.current_generation,
            pointer.current_manifest_sha256, out)) {
        return PERSISTENCE_GENERATION_CURRENT;
    }
    if (pointer.previous_generation != 0 &&
        generation_manifest_valid(
            root_dir, pointer.previous_generation,
            pointer.previous_manifest_sha256) &&
        generation_paths_fill(
            root_dir, pointer.previous_generation,
            pointer.previous_manifest_sha256, out)) {
        return PERSISTENCE_GENERATION_PREVIOUS;
    }
    memset(out, 0, sizeof(*out));
    return PERSISTENCE_GENERATION_INVALID;
}

static bool parse_generation_name(const char *name, uint64_t *generation) {
    if (!name || !generation) return false;
    size_t prefix_len = strlen(GENERATION_PREFIX);
    if (strncmp(name, GENERATION_PREFIX, prefix_len) != 0 ||
        strlen(name + prefix_len) != 20u) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < 20u; i++) {
        char c = name[prefix_len + i];
        if (c < '0' || c > '9') return false;
        uint8_t digit = (uint8_t)(c - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    if (value == 0) return false;
    *generation = value;
    return true;
}

static bool next_generation_create(
    const char *root_dir,
    uint64_t *generation_out,
    char generation_dir[PERSISTENCE_GENERATION_PATH_MAX]) {
    if (!ensure_directory(root_dir)) return false;
    directory_entries_t entries;
    if (!directory_entries_read(root_dir, &entries, false)) return false;
    uint64_t maximum = 0;
    for (size_t i = 0; i < entries.count; i++) {
        uint64_t value = 0;
        if (entries.items[i].kind == FS_NODE_DIRECTORY &&
            parse_generation_name(entries.items[i].name, &value) &&
            value > maximum) {
            maximum = value;
        }
    }
    directory_entries_free(&entries);
    if (maximum == UINT64_MAX) return false;

    uint64_t candidate = maximum + 1u;
    for (unsigned attempt = 0; attempt < 1024u; attempt++) {
        if (!generation_dir_path(
                generation_dir, PERSISTENCE_GENERATION_PATH_MAX,
                root_dir, candidate)) {
            return false;
        }
        if (GENERATION_MKDIR(generation_dir) == 0) {
            if (!persistence_sync_parent_dir(generation_dir))
                return false;
            *generation_out = candidate;
            return true;
        }
        if (errno != EEXIST || candidate == UINT64_MAX) return false;
        candidate++;
    }
    return false;
}

typedef struct {
    bool enabled;
    int player_slot;
    uint8_t session_token[8];
    uint8_t pubkey[32];
    uint64_t source_size;
    uint8_t source_sha256[32];
} persistence_recovery_spec_t;

static bool persistence_generation_commit_internal(
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS],
    persistence_generation_fault_t fault,
    const persistence_recovery_spec_t *recovery,
    persistence_recovery_commit_result_t *recovery_result,
    persistence_generation_paths_t *published) {
    if (published) memset(published, 0, sizeof(*published));
    if (recovery_result)
        *recovery_result = PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    if (!root_dir || !root_dir[0] || !world || !save_player_slot ||
        !published ||
        fault < PERSISTENCE_GENERATION_FAULT_NONE ||
        fault > PERSISTENCE_GENERATION_FAULT_POINTER_DIR_SYNC_FAILURE) {
        if (recovery_result)
            *recovery_result =
                PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
        return false;
    }
    if (recovery && recovery->enabled &&
        (recovery->player_slot < 0 ||
         recovery->player_slot >= MAX_PLAYERS ||
         !save_player_slot[recovery->player_slot])) {
        if (recovery_result)
            *recovery_result =
                PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
        return false;
    }

    persistence_generation_paths_t prior = {0};
    persistence_generation_status_t prior_status =
        persistence_generation_resolve(root_dir, &prior);
    if (prior_status == PERSISTENCE_GENERATION_INVALID) return false;
    const char *source_player_dir =
        prior_status == PERSISTENCE_GENERATION_NONE
        ? legacy_player_dir : prior.player_dir;

    recovery_source_guard_t source_guard;
    memset(&source_guard, 0, sizeof(source_guard));
#ifdef _WIN32
    source_guard.source_handle = INVALID_HANDLE_VALUE;
#else
    source_guard.player_fd = -1;
    source_guard.parent_fd = -1;
    source_guard.destination_parent_fd = -1;
    source_guard.source_fd = -1;
#endif
    char recovery_source_path[PERSISTENCE_GENERATION_PATH_MAX] = {0};
    char recovery_selected_destination[
        PERSISTENCE_GENERATION_PATH_MAX] = {0};
    char recovery_source_file[
        sizeof("player_") + 16 + sizeof(".sav")] = {0};
    char recovery_destination_file[72] = {0};
    if (recovery && recovery->enabled) {
        char source_legacy[PERSISTENCE_GENERATION_PATH_MAX];
        char source_pubkey[PERSISTENCE_GENERATION_PATH_MAX];
        if (!recovery_source_name(
                recovery_source_file,
                recovery->session_token) ||
            !recovery_destination_name(
                recovery_destination_file,
                recovery->pubkey) ||
            !path_join(source_legacy, sizeof(source_legacy),
                       source_player_dir, "legacy") ||
            !path_join(source_pubkey, sizeof(source_pubkey),
                       source_player_dir, "pubkey") ||
            !path_join(
                recovery_source_path,
                sizeof(recovery_source_path),
                source_legacy, recovery_source_file) ||
            !path_join(
                recovery_selected_destination,
                sizeof(recovery_selected_destination),
                source_pubkey, recovery_destination_file)) {
            if (recovery_result) {
                *recovery_result =
                    PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
            }
            return false;
        }
#if defined(SIGNAL_SAVE_TESTING)
        if (recovery_before_source_bind_hook) {
            recovery_before_source_bind_hook(
                recovery_source_path,
                recovery_selected_destination,
                recovery_before_source_bind_user);
        }
#endif
        persistence_recovery_commit_result_t guard_result =
            recovery_source_guard_open(
                &source_guard, source_player_dir,
                recovery_source_path,
                recovery_selected_destination,
                recovery->source_size,
                recovery->source_sha256);
        if (guard_result != PERSISTENCE_RECOVERY_COMMIT_OK) {
            if (recovery_result)
                *recovery_result = guard_result;
            return false;
        }
    }

    uint64_t generation = 0;
    char generation_dir[PERSISTENCE_GENERATION_PATH_MAX];
    if (!next_generation_create(
            root_dir, &generation, generation_dir)) {
        goto commit_failure;
    }
    persistence_generation_paths_t candidate = {0};
    uint8_t zero_digest[32] = {0};
    if (!generation_paths_fill(
            root_dir, generation, zero_digest, &candidate) ||
        !ensure_directory(candidate.catalog_dir)) {
        goto commit_failure;
    }
    if (recovery && recovery->enabled) {
        persistence_recovery_commit_result_t copy_result =
            copy_player_namespace_for_recovery(
                source_player_dir, candidate.player_dir,
                recovery->session_token, recovery->pubkey);
        if (copy_result != PERSISTENCE_RECOVERY_COMMIT_OK) {
            if (recovery_result) *recovery_result = copy_result;
            goto commit_failure;
        }
        persistence_recovery_commit_result_t guard_result =
            recovery_source_guard_validate(&source_guard);
        if (guard_result != PERSISTENCE_RECOVERY_COMMIT_OK) {
            if (recovery_result) *recovery_result = guard_result;
            goto commit_failure;
        }
    } else if (!copy_player_namespace(
                   source_player_dir, candidate.player_dir)) {
        goto commit_failure;
    }

#if defined(SIGNAL_SAVE_TESTING)
    if (recovery && recovery->enabled &&
        recovery_before_destination_publish_hook) {
        char candidate_pubkey[PERSISTENCE_GENERATION_PATH_MAX];
        char candidate_destination[PERSISTENCE_GENERATION_PATH_MAX];
        if (!path_join(
                candidate_pubkey, sizeof(candidate_pubkey),
                candidate.player_dir, "pubkey") ||
            !path_join(
                candidate_destination,
                sizeof(candidate_destination),
                candidate_pubkey,
                recovery_destination_file)) {
            goto commit_failure;
        }
        recovery_before_destination_publish_hook(
            recovery_source_path, candidate_destination,
            recovery_before_destination_publish_user);
    }
#endif

    if (!station_catalog_save_all(
            world->stations, MAX_STATIONS, candidate.catalog_dir) ||
        !world_save(world, candidate.world_path)) {
        goto commit_failure;
    }
    for (int slot = 0; slot < MAX_PLAYERS; slot++) {
        if (!save_player_slot[slot]) continue;
        if (recovery && recovery->enabled &&
            slot == recovery->player_slot) {
            player_save_create_result_t save_result =
                player_save_no_replace(
                    &world->players[slot],
                    candidate.player_dir, slot);
            if (save_result != PLAYER_SAVE_CREATE_OK) {
                if (recovery_result) {
                    *recovery_result =
                        save_result ==
                            PLAYER_SAVE_CREATE_DESTINATION_CONFLICT
                        ? PERSISTENCE_RECOVERY_COMMIT_DESTINATION_CONFLICT
                        : PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
                }
                goto commit_failure;
            }
        } else if (!player_save(
                       &world->players[slot],
                       candidate.player_dir, slot)) {
            goto commit_failure;
        }
    }
    if (recovery && recovery->enabled) {
        persistence_recovery_commit_result_t guard_result =
            recovery_source_guard_validate(&source_guard);
        if (guard_result != PERSISTENCE_RECOVERY_COMMIT_OK) {
            if (recovery_result) *recovery_result = guard_result;
            goto commit_failure;
        }
    }
    if (recovery && recovery->enabled &&
        !write_recovery_consumption_marker(
            generation_dir,
            recovery->session_token,
            recovery->pubkey,
            recovery->source_size,
            recovery->source_sha256)) {
        goto commit_failure;
    }
    if (fault == PERSISTENCE_GENERATION_FAULT_AFTER_ARTIFACTS)
        goto commit_failure;

    manifest_entries_t entries;
    if (!collect_generation_files(generation_dir, &entries))
        goto commit_failure;
    uint8_t manifest_sha256[32];
    bool manifest_ok = write_generation_manifest(
        generation_dir, generation, &entries, manifest_sha256);
    manifest_entries_free(&entries);
    if (!manifest_ok) goto commit_failure;
    if (fault == PERSISTENCE_GENERATION_FAULT_AFTER_MANIFEST)
        goto commit_failure;

    generation_pointer_t pointer = {
        .current_generation = generation,
        .previous_generation =
            recovery && recovery->enabled
            ? 0
            : prior_status == PERSISTENCE_GENERATION_NONE
            ? 0 : prior.generation,
    };
    memcpy(pointer.current_manifest_sha256, manifest_sha256, 32);
    if (pointer.previous_generation != 0)
        memcpy(pointer.previous_manifest_sha256,
               prior.manifest_sha256, 32);

    char pointer_temporary[PERSISTENCE_GENERATION_PATH_MAX];
    char pointer_final[PERSISTENCE_GENERATION_PATH_MAX];
    if (!pointer_write_temporary(
            root_dir, &pointer,
            pointer_temporary, pointer_final)) {
        goto commit_failure;
    }
    if (fault == PERSISTENCE_GENERATION_FAULT_BEFORE_POINTER_PUBLISH)
        goto commit_failure;
    if (recovery && recovery->enabled) {
        persistence_recovery_commit_result_t guard_result =
            recovery_source_guard_validate(&source_guard);
        if (guard_result != PERSISTENCE_RECOVERY_COMMIT_OK) {
            if (recovery_result) *recovery_result = guard_result;
            remove(pointer_temporary);
            goto commit_failure;
        }
    }
    bool pointer_published = false;
    if (fault ==
        PERSISTENCE_GENERATION_FAULT_POINTER_DIR_SYNC_FAILURE) {
#ifdef _WIN32
        pointer_published = MoveFileExA(
            pointer_temporary, pointer_final,
            MOVEFILE_REPLACE_EXISTING) != 0;
#else
        pointer_published = rename(
            pointer_temporary, pointer_final) == 0;
#endif
        /*
         * Deliberately skip the parent-directory sync.  The recovery below
         * models the real ambiguous case where rename succeeded but fsync
         * reported failure.
         */
        if (pointer_published) pointer_published = false;
    } else {
        pointer_published =
            persistence_replace_file(pointer_temporary, pointer_final);
    }
    if (!pointer_published) {
        /*
         * rename(2) may have succeeded even if the parent-directory fsync
         * failed.  Re-resolve so runtime and disk never select different
         * generations merely because durability reporting was ambiguous.
         */
        persistence_generation_paths_t visible = {0};
        persistence_generation_status_t visible_status =
            persistence_generation_resolve(root_dir, &visible);
        if (visible_status != PERSISTENCE_GENERATION_CURRENT ||
            visible.generation != generation) {
            remove(pointer_temporary);
            goto commit_failure;
        }
        *published = visible;
        if (recovery_result)
            *recovery_result = PERSISTENCE_RECOVERY_COMMIT_OK;
        recovery_source_guard_close(&source_guard);
        return true;
    }
    memcpy(candidate.manifest_sha256, manifest_sha256, 32);
    *published = candidate;
    if (recovery_result)
        *recovery_result = PERSISTENCE_RECOVERY_COMMIT_OK;
    recovery_source_guard_close(&source_guard);
    return true;

commit_failure:
    recovery_source_guard_close(&source_guard);
    return false;
}

bool persistence_generation_commit(
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS],
    persistence_generation_fault_t fault,
    persistence_generation_paths_t *published) {
    return persistence_generation_commit_internal(
        root_dir, legacy_player_dir, world, save_player_slot,
        fault, NULL, NULL, published);
}

persistence_recovery_commit_result_t
persistence_generation_commit_recovery(
    const char *root_dir,
    const char *legacy_player_dir,
    const world_t *world,
    const bool save_player_slot[MAX_PLAYERS],
    int player_slot,
    const uint8_t session_token[8],
    const uint8_t pubkey[32],
    uint64_t source_size,
    const uint8_t source_sha256[32],
    persistence_generation_fault_t fault,
    persistence_generation_paths_t *published) {
    if (published) memset(published, 0, sizeof(*published));
    if (!session_token || !pubkey || !source_sha256 ||
        source_size == 0) {
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }
    uint8_t token_any = 0;
    uint8_t pubkey_any = 0;
    for (size_t i = 0; i < 8; i++) token_any |= session_token[i];
    for (size_t i = 0; i < 32; i++) pubkey_any |= pubkey[i];
    if (token_any == 0 || pubkey_any == 0) {
        return PERSISTENCE_RECOVERY_COMMIT_INVALID_ARGUMENT;
    }
    persistence_recovery_spec_t recovery = {
        .enabled = true,
        .player_slot = player_slot,
    };
    memcpy(recovery.session_token, session_token,
           sizeof(recovery.session_token));
    memcpy(recovery.pubkey, pubkey, sizeof(recovery.pubkey));
    recovery.source_size = source_size;
    memcpy(recovery.source_sha256, source_sha256,
           sizeof(recovery.source_sha256));
    persistence_recovery_commit_result_t result =
        PERSISTENCE_RECOVERY_COMMIT_IO_FAILURE;
    if (!persistence_generation_commit_internal(
            root_dir, legacy_player_dir, world, save_player_slot,
            fault, &recovery, &result, published)) {
        return result;
    }
    return PERSISTENCE_RECOVERY_COMMIT_OK;
}
