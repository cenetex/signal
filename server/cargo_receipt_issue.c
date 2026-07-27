#if defined(_WIN32)
#  if !defined(_WIN32_WINNT)
#    define _WIN32_WINNT 0x0602
#  elif _WIN32_WINNT < 0x0602
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0602
#  endif
#endif
#if defined(__EMSCRIPTEN__) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

/*
 * cargo_receipt_issue.c -- Server-side cargo_receipt_t issuance + emit.
 *
 * See cargo_receipt_issue.h for the public contract. This file glues
 * shared/cargo_receipt.h (wire format + verify) to server-only
 * primitives: station_authority signing and chain_log_emit.
 */
#include "cargo_receipt_issue.h"

#include "cargo_receipt_trust.h"
#include "game_sim.h"
#include "manifest.h"
#include "sha256.h"
#include "station_authority.h"
#include "wire_codec.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#  include <io.h>
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

enum {
    CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP = 8,
    CARGO_RECEIPT_ORIGIN_INDEX_INITIAL_CAPACITY = 64,
    CARGO_RECEIPT_ORIGIN_INDEX_MAX_RECORDS = 16384,
};

typedef struct {
    bool exists;
#if defined(_WIN32)
    bool uses_digest;
    uint64_t volume_serial;
    uint8_t file_id[16];
    uint64_t size;
    int64_t last_write_time;
    int64_t change_time;
    uint8_t digest[32];
#else
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    int64_t mtime_seconds;
    int64_t mtime_nanoseconds;
    int64_t ctime_seconds;
    int64_t ctime_nanoseconds;
#endif
} cargo_receipt_origin_file_state_t;

typedef struct {
    uint8_t cargo_pub[32];
    uint8_t event_hash[32];
    uint64_t event_id;
    uint64_t epoch;
    uint16_t craft_recipe_id;
    uint8_t event_type;
    uint8_t craft_input_count;
    uint8_t output_semantics_version;
    cargo_unit_t output_cargo;
} cargo_receipt_origin_index_record_t;

/*
 * Each authority retains at most 16,384 168-byte transform rows:
 * 2,752,512 bytes per entry and exactly 21 MiB across all eight LRU slots.
 * Pin the row size so later metadata additions cannot silently invalidate
 * that process-wide memory bound.
 */
_Static_assert(sizeof(cargo_receipt_origin_index_record_t) == 168,
               "origin index record size must preserve the cache budget");
_Static_assert(
    CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP *
        CARGO_RECEIPT_ORIGIN_INDEX_MAX_RECORDS *
        sizeof(cargo_receipt_origin_index_record_t) ==
        21u * 1024u * 1024u,
    "origin index vectors must remain bounded to 21 MiB total");

typedef struct {
    bool valid;
    bool bind_station_tail;
    uint64_t instance_generation;
    uint64_t last_used;
    uint64_t configuration_generation;
    uint64_t station_chain_event_count;
    uint8_t station_chain_last_hash[32];
    uint8_t registry_fingerprint[32];
    uint8_t authority[32];
    cargo_receipt_origin_file_state_t file;
    cargo_receipt_origin_resolve_status_t history_status;
    size_t record_count;
    size_t record_capacity;
    cargo_receipt_origin_index_record_t *records;
} cargo_receipt_origin_authority_cache_entry_t;

static cargo_receipt_origin_authority_cache_entry_t
    g_cargo_receipt_origin_authority_cache[
        CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP];
static cargo_receipt_origin_cache_stats_t
    g_cargo_receipt_origin_cache_stats;
static uint64_t g_cargo_receipt_origin_cache_clock;
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
static cargo_receipt_origin_cache_test_build_hook_t
    g_cargo_receipt_origin_cache_test_build_hook;
static void *g_cargo_receipt_origin_cache_test_build_hook_user;
static cargo_receipt_origin_cache_test_snapshot_hook_t
    g_cargo_receipt_origin_cache_test_snapshot_hook;
static void *g_cargo_receipt_origin_cache_test_snapshot_hook_user;
static size_t g_cargo_receipt_origin_cache_test_record_limit;
static bool g_cargo_receipt_origin_cache_test_fail_next_record_allocation;
static bool g_cargo_receipt_origin_cache_test_fail_next_snapshot_open;
#endif

void cargo_receipt_origin_cache_reset(void) {
    for (size_t i = 0;
         i < CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP; i++) {
        free(g_cargo_receipt_origin_authority_cache[i].records);
    }
    memset(g_cargo_receipt_origin_authority_cache, 0,
           sizeof(g_cargo_receipt_origin_authority_cache));
    memset(&g_cargo_receipt_origin_cache_stats, 0,
           sizeof(g_cargo_receipt_origin_cache_stats));
    g_cargo_receipt_origin_cache_clock = 0;
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    g_cargo_receipt_origin_cache_test_build_hook = NULL;
    g_cargo_receipt_origin_cache_test_build_hook_user = NULL;
    g_cargo_receipt_origin_cache_test_snapshot_hook = NULL;
    g_cargo_receipt_origin_cache_test_snapshot_hook_user = NULL;
    g_cargo_receipt_origin_cache_test_record_limit = 0;
    g_cargo_receipt_origin_cache_test_fail_next_record_allocation = false;
    g_cargo_receipt_origin_cache_test_fail_next_snapshot_open = false;
#endif
}

cargo_receipt_origin_cache_stats_t
cargo_receipt_origin_cache_stats(void) {
    return g_cargo_receipt_origin_cache_stats;
}

#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
void cargo_receipt_origin_cache_test_set_build_hook(
    cargo_receipt_origin_cache_test_build_hook_t hook,
    void *user) {
    g_cargo_receipt_origin_cache_test_build_hook = hook;
    g_cargo_receipt_origin_cache_test_build_hook_user = user;
}

void cargo_receipt_origin_cache_test_set_snapshot_hook(
    cargo_receipt_origin_cache_test_snapshot_hook_t hook,
    void *user) {
    g_cargo_receipt_origin_cache_test_snapshot_hook = hook;
    g_cargo_receipt_origin_cache_test_snapshot_hook_user = user;
}

void cargo_receipt_origin_cache_test_set_record_limit(
    size_t max_records) {
    g_cargo_receipt_origin_cache_test_record_limit = max_records;
}

void cargo_receipt_origin_cache_test_fail_next_record_allocation(void) {
    g_cargo_receipt_origin_cache_test_fail_next_record_allocation = true;
}

void cargo_receipt_origin_cache_test_fail_next_snapshot_open(void) {
    g_cargo_receipt_origin_cache_test_fail_next_snapshot_open = true;
}
#endif

static uint64_t cargo_receipt_origin_cache_tick(void) {
    g_cargo_receipt_origin_cache_clock++;
    if (g_cargo_receipt_origin_cache_clock == 0)
        g_cargo_receipt_origin_cache_clock = 1;
    return g_cargo_receipt_origin_cache_clock;
}

static bool cargo_receipt_origin_file_state_equal(
    const cargo_receipt_origin_file_state_t *a,
    const cargo_receipt_origin_file_state_t *b) {
    if (!a || !b || a->exists != b->exists) return false;
    if (!a->exists) return true;
#if defined(_WIN32)
    if (a->uses_digest != b->uses_digest) return false;
    if (a->uses_digest) {
        return memcmp(a->digest, b->digest,
                      sizeof(a->digest)) == 0;
    }
    return
        a->volume_serial == b->volume_serial &&
        memcmp(a->file_id, b->file_id, sizeof(a->file_id)) == 0 &&
        a->size == b->size &&
        a->last_write_time == b->last_write_time &&
        a->change_time == b->change_time;
#else
    return
        a->device == b->device &&
        a->inode == b->inode &&
        a->size == b->size &&
        a->mtime_seconds == b->mtime_seconds &&
        a->mtime_nanoseconds == b->mtime_nanoseconds &&
        a->ctime_seconds == b->ctime_seconds &&
        a->ctime_nanoseconds == b->ctime_nanoseconds;
#endif
}

#if defined(_WIN32)
static bool cargo_receipt_origin_file_digest_open(
    FILE *file,
    uint8_t out[32]) {
    if (!file || !out || fseek(file, 0, SEEK_SET) != 0)
        return false;
    clearerr(file);
    g_cargo_receipt_origin_cache_stats.file_digest_scans++;
    sha256_ctx_t digest;
    sha256_init(&digest);
    uint8_t buffer[8192];
    for (;;) {
        size_t read_count = fread(buffer, 1, sizeof(buffer), file);
        if (read_count > 0)
            sha256_update(&digest, buffer, read_count);
        if (read_count < sizeof(buffer)) {
            if (ferror(file)) return false;
            break;
        }
    }
    sha256_final(&digest, out);
    if (fseek(file, 0, SEEK_SET) != 0) return false;
    clearerr(file);
    return true;
}

typedef enum {
    CARGO_RECEIPT_ORIGIN_NATIVE_STATE_OK = 0,
    CARGO_RECEIPT_ORIGIN_NATIVE_STATE_UNSUPPORTED,
    CARGO_RECEIPT_ORIGIN_NATIVE_STATE_ERROR,
} cargo_receipt_origin_native_state_result_t;

static bool cargo_receipt_origin_windows_api_unsupported(DWORD error) {
    return error == ERROR_INVALID_PARAMETER ||
           error == ERROR_NOT_SUPPORTED ||
           error == ERROR_INVALID_FUNCTION ||
           error == ERROR_CALL_NOT_IMPLEMENTED;
}

static cargo_receipt_origin_native_state_result_t
cargo_receipt_origin_windows_file_state(
    HANDLE handle,
    cargo_receipt_origin_file_state_t *out) {
    FILE_ID_INFO identity;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    if (!GetFileInformationByHandleEx(
            handle, FileIdInfo, &identity,
            (DWORD)sizeof(identity))) {
        return cargo_receipt_origin_windows_api_unsupported(
                   GetLastError())
            ? CARGO_RECEIPT_ORIGIN_NATIVE_STATE_UNSUPPORTED
            : CARGO_RECEIPT_ORIGIN_NATIVE_STATE_ERROR;
    }
    if (!GetFileInformationByHandleEx(
            handle, FileBasicInfo, &basic,
            (DWORD)sizeof(basic))) {
        return cargo_receipt_origin_windows_api_unsupported(
                   GetLastError())
            ? CARGO_RECEIPT_ORIGIN_NATIVE_STATE_UNSUPPORTED
            : CARGO_RECEIPT_ORIGIN_NATIVE_STATE_ERROR;
    }
    if (!GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard,
            (DWORD)sizeof(standard))) {
        return cargo_receipt_origin_windows_api_unsupported(
                   GetLastError())
            ? CARGO_RECEIPT_ORIGIN_NATIVE_STATE_UNSUPPORTED
            : CARGO_RECEIPT_ORIGIN_NATIVE_STATE_ERROR;
    }
    if (standard.Directory || standard.EndOfFile.QuadPart < 0)
        return CARGO_RECEIPT_ORIGIN_NATIVE_STATE_ERROR;
    static const uint8_t zero_file_id[16] = {0};
    if (memcmp(identity.FileId.Identifier, zero_file_id,
               sizeof(zero_file_id)) == 0) {
        return CARGO_RECEIPT_ORIGIN_NATIVE_STATE_UNSUPPORTED;
    }

    out->exists = true;
    out->uses_digest = false;
    out->volume_serial = (uint64_t)identity.VolumeSerialNumber;
    memcpy(out->file_id, identity.FileId.Identifier,
           sizeof(out->file_id));
    out->size = (uint64_t)standard.EndOfFile.QuadPart;
    out->last_write_time = (int64_t)basic.LastWriteTime.QuadPart;
    out->change_time = (int64_t)basic.ChangeTime.QuadPart;
    g_cargo_receipt_origin_cache_stats.file_native_metadata_queries++;
    return CARGO_RECEIPT_ORIGIN_NATIVE_STATE_OK;
}
#endif

static bool cargo_receipt_origin_file_state_open(
    FILE *file,
    cargo_receipt_origin_file_state_t *out) {
    if (!file || !out) return false;
    memset(out, 0, sizeof(*out));
#if defined(_WIN32)
    int descriptor = _fileno(file);
    if (descriptor < 0) return false;
    intptr_t raw_handle = _get_osfhandle(descriptor);
    if (raw_handle == -1) return false;
    cargo_receipt_origin_native_state_result_t native =
        cargo_receipt_origin_windows_file_state(
            (HANDLE)raw_handle, out);
    if (native == CARGO_RECEIPT_ORIGIN_NATIVE_STATE_OK)
        return true;
    if (native != CARGO_RECEIPT_ORIGIN_NATIVE_STATE_UNSUPPORTED)
        return false;
    memset(out, 0, sizeof(*out));
    out->exists = true;
    out->uses_digest = true;
    return cargo_receipt_origin_file_digest_open(file, out->digest);
#else
    int descriptor = fileno(file);
    if (descriptor < 0) return false;
    struct stat st;
    if (fstat(descriptor, &st) != 0 ||
        st.st_size < 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    out->exists = true;
    out->device = (uint64_t)st.st_dev;
    out->inode = (uint64_t)st.st_ino;
    out->size = (uint64_t)st.st_size;
    out->mtime_seconds = (int64_t)st.st_mtime;
    out->ctime_seconds = (int64_t)st.st_ctime;
#  if defined(__APPLE__)
    out->mtime_nanoseconds = (int64_t)st.st_mtimespec.tv_nsec;
    out->ctime_nanoseconds = (int64_t)st.st_ctimespec.tv_nsec;
#  else
    out->mtime_nanoseconds = (int64_t)st.st_mtim.tv_nsec;
    out->ctime_nanoseconds = (int64_t)st.st_ctim.tv_nsec;
#  endif
    return true;
#endif
}

static bool cargo_receipt_origin_file_state(
    const char *path,
    cargo_receipt_origin_file_state_t *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
#if defined(_WIN32)
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT;
    bool ok = cargo_receipt_origin_file_state_open(file, out);
    if (fclose(file) != 0) ok = false;
    return ok;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT;
    }
    if (st.st_size < 0 || !S_ISREG(st.st_mode)) return false;
    out->exists = true;
    out->device = (uint64_t)st.st_dev;
    out->inode = (uint64_t)st.st_ino;
    out->size = (uint64_t)st.st_size;
    out->mtime_seconds = (int64_t)st.st_mtime;
    out->ctime_seconds = (int64_t)st.st_ctime;
#  if defined(__APPLE__)
    out->mtime_nanoseconds = (int64_t)st.st_mtimespec.tv_nsec;
    out->ctime_nanoseconds = (int64_t)st.st_ctimespec.tv_nsec;
#  else
    out->mtime_nanoseconds = (int64_t)st.st_mtim.tv_nsec;
    out->ctime_nanoseconds = (int64_t)st.st_ctim.tv_nsec;
#  endif
    return true;
#endif
}

static bool cargo_receipt_origin_cache_file_matches(
    const cargo_receipt_origin_authority_cache_entry_t *entry,
    const cargo_receipt_origin_file_state_t *file) {
    return entry &&
        cargo_receipt_origin_file_state_equal(&entry->file, file);
}

static bool cargo_receipt_origin_file_has_native_identity(
    const cargo_receipt_origin_file_state_t *file) {
    if (!file || !file->exists) return false;
#if defined(_WIN32)
    return !file->uses_digest;
#else
    return true;
#endif
}

static bool cargo_receipt_origin_file_same_object(
    const cargo_receipt_origin_file_state_t *a,
    const cargo_receipt_origin_file_state_t *b) {
    if (!cargo_receipt_origin_file_has_native_identity(a) ||
        !cargo_receipt_origin_file_has_native_identity(b)) {
        return false;
    }
#if defined(_WIN32)
    return a->volume_serial == b->volume_serial &&
        memcmp(a->file_id, b->file_id, sizeof(a->file_id)) == 0;
#else
    return a->device == b->device &&
        a->inode == b->inode;
#endif
}

static void cargo_receipt_origin_registry_fingerprint(
    const station_t *station,
    uint8_t out[32]) {
    uint8_t packed[
        2 + 32 +
        STATION_AUTHORITY_REGISTRY_CAP * (32 + 2)] = {0};
    size_t offset = 0;
    packed[offset++] = station->authority_registry_version;
    packed[offset++] = station->authority_registry_count;
    memcpy(&packed[offset], station->station_pubkey, 32);
    offset += 32;
    for (uint8_t i = 0; i < STATION_AUTHORITY_REGISTRY_CAP; i++) {
        const station_authority_record_t *record =
            &station->authority_registry[i];
        memcpy(&packed[offset], record->pubkey, 32);
        offset += 32;
        packed[offset++] = record->lifecycle;
        packed[offset++] = record->trust;
    }
    sha256_bytes(packed, offset, out);
}

static void cargo_receipt_origin_authority_cache_release(
    cargo_receipt_origin_authority_cache_entry_t *entry) {
    if (!entry) return;
    free(entry->records);
    memset(entry, 0, sizeof(*entry));
}

/*
 * A transfer append contains no origin-producing event, so a previously
 * verified current-authority index remains semantically unchanged. Under the
 * chain log's synchronous single-writer model, preserve that proof only when
 * this exact process owns an uninterrupted, exact append from the cached file
 * and station heads. Arbitrary file growth never enters this path; any
 * mismatch drops the entry so the next lookup performs a full cryptographic
 * verification.
 */
typedef struct {
    cargo_receipt_origin_authority_cache_entry_t *entry;
    FILE *snapshot;
    uint64_t entry_instance_generation;
    uint64_t configuration_generation;
    uint64_t station_chain_event_count;
    uint64_t expected_file_growth;
    uint16_t expected_event_count;
    uint8_t authority[32];
    uint8_t registry_fingerprint[32];
    uint8_t station_chain_last_hash[32];
    cargo_receipt_origin_file_state_t file_before;
    char path[256];
} cargo_receipt_origin_trusted_append_t;

static cargo_receipt_origin_trusted_append_t
cargo_receipt_origin_cache_begin_trusted_append(
    const station_t *station,
    uint16_t expected_event_count,
    uint64_t expected_file_growth) {
    cargo_receipt_origin_trusted_append_t token = {0};
    if (!station || expected_event_count == 0 ||
        expected_file_growth == 0 || !chain_log_disk_enabled() ||
        !station_authority_registry_validate(station)) {
        return token;
    }

    uint64_t configuration_generation =
        chain_log_configuration_generation();
    uint8_t registry_fingerprint[32];
    cargo_receipt_origin_registry_fingerprint(
        station, registry_fingerprint);
    cargo_receipt_origin_authority_cache_entry_t *entry = NULL;
    for (size_t i = 0;
         i < CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP; i++) {
        cargo_receipt_origin_authority_cache_entry_t *candidate =
            &g_cargo_receipt_origin_authority_cache[i];
        if (!candidate->valid ||
            !candidate->bind_station_tail ||
            candidate->history_status !=
                CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED ||
            candidate->configuration_generation !=
                configuration_generation ||
            candidate->station_chain_event_count !=
                station->chain_event_count ||
            memcmp(candidate->station_chain_last_hash,
                   station->chain_last_hash, 32) != 0 ||
            memcmp(candidate->registry_fingerprint,
                   registry_fingerprint, 32) != 0 ||
            memcmp(candidate->authority,
                   station->station_pubkey, 32) != 0 ||
            !cargo_receipt_origin_file_has_native_identity(
                &candidate->file)) {
            continue;
        }
        entry = candidate;
        break;
    }
    if (!entry ||
        !chain_log_path_for(
            station->station_pubkey,
            token.path, sizeof(token.path))) {
        return token;
    }

    FILE *snapshot = fopen(token.path, "rb");
    cargo_receipt_origin_file_state_t opened;
    cargo_receipt_origin_file_state_t path_state;
    bool exact =
        snapshot &&
        cargo_receipt_origin_file_state_open(snapshot, &opened) &&
        cargo_receipt_origin_file_state(token.path, &path_state) &&
        cargo_receipt_origin_file_state_equal(&entry->file, &opened) &&
        cargo_receipt_origin_file_state_equal(&opened, &path_state);
    if (!exact) {
        if (snapshot) (void)fclose(snapshot);
        cargo_receipt_origin_authority_cache_release(entry);
        memset(&token, 0, sizeof(token));
        return token;
    }

    token.entry = entry;
    token.snapshot = snapshot;
    token.entry_instance_generation =
        entry->instance_generation;
    token.configuration_generation =
        configuration_generation;
    token.station_chain_event_count =
        station->chain_event_count;
    token.expected_file_growth = expected_file_growth;
    token.expected_event_count = expected_event_count;
    memcpy(token.authority, station->station_pubkey, 32);
    memcpy(token.registry_fingerprint,
           registry_fingerprint, 32);
    memcpy(token.station_chain_last_hash,
           station->chain_last_hash, 32);
    token.file_before = opened;
    return token;
}

static bool cargo_receipt_origin_trusted_append_entry_is_same(
    const cargo_receipt_origin_trusted_append_t *token) {
    return token && token->entry && token->entry->valid &&
        token->entry->instance_generation ==
            token->entry_instance_generation &&
        token->entry->bind_station_tail &&
        token->entry->configuration_generation ==
            token->configuration_generation &&
        token->entry->station_chain_event_count ==
            token->station_chain_event_count &&
        memcmp(token->entry->station_chain_last_hash,
               token->station_chain_last_hash, 32) == 0 &&
        memcmp(token->entry->registry_fingerprint,
               token->registry_fingerprint, 32) == 0 &&
        memcmp(token->entry->authority,
               token->authority, 32) == 0 &&
        cargo_receipt_origin_file_state_equal(
            &token->entry->file, &token->file_before);
}

static void cargo_receipt_origin_cache_finish_trusted_append(
    cargo_receipt_origin_trusted_append_t *token,
    const station_t *station,
    const chain_log_append_result_t *append) {
    if (!token || !token->snapshot) return;

    bool same_entry =
        cargo_receipt_origin_trusted_append_entry_is_same(token);
    bool accepted = same_entry && station && append &&
        append->status == CHAIN_LOG_APPEND_OK &&
        append->event_count == token->expected_event_count &&
        token->station_chain_event_count <=
            UINT64_MAX - token->expected_event_count &&
        append->first_event_id ==
            token->station_chain_event_count + 1u &&
        append->last_event_id ==
            token->station_chain_event_count +
                token->expected_event_count &&
        station->chain_event_count == append->last_event_id &&
        memcmp(append->last_hash,
               station->chain_last_hash, 32) == 0 &&
        memcmp(station->station_pubkey,
               token->authority, 32) == 0 &&
        chain_log_configuration_generation() ==
            token->configuration_generation &&
        station_authority_registry_validate(station);

    uint8_t registry_fingerprint[32] = {0};
    if (accepted) {
        cargo_receipt_origin_registry_fingerprint(
            station, registry_fingerprint);
        accepted =
            memcmp(registry_fingerprint,
                   token->registry_fingerprint, 32) == 0;
    }

    cargo_receipt_origin_file_state_t opened_after = {0};
    cargo_receipt_origin_file_state_t path_after = {0};
    if (accepted) {
        accepted =
            token->file_before.size <=
                UINT64_MAX - token->expected_file_growth &&
            cargo_receipt_origin_file_state_open(
                token->snapshot, &opened_after) &&
            cargo_receipt_origin_file_state(
                token->path, &path_after) &&
            cargo_receipt_origin_file_same_object(
                &token->file_before, &opened_after) &&
            cargo_receipt_origin_file_state_equal(
                &opened_after, &path_after) &&
            opened_after.size ==
                token->file_before.size +
                    token->expected_file_growth;
    }

    if (fclose(token->snapshot) != 0)
        accepted = false;
    token->snapshot = NULL;

    same_entry =
        cargo_receipt_origin_trusted_append_entry_is_same(token);
    if (accepted && same_entry) {
        token->entry->file = opened_after;
        token->entry->station_chain_event_count =
            append->last_event_id;
        memcpy(token->entry->station_chain_last_hash,
               append->last_hash, 32);
        token->entry->last_used =
            cargo_receipt_origin_cache_tick();
    } else if (same_entry) {
        cargo_receipt_origin_authority_cache_release(
            token->entry);
    }
    memset(token, 0, sizeof(*token));
}

static cargo_receipt_origin_authority_cache_entry_t *
cargo_receipt_origin_authority_cache_lookup(
    const station_t *station,
    const uint8_t registry_fingerprint[32],
    const uint8_t authority[32],
    const cargo_receipt_origin_file_state_t *file) {
    uint64_t generation = chain_log_configuration_generation();
    for (size_t i = 0;
         i < CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP; i++) {
        cargo_receipt_origin_authority_cache_entry_t *entry =
            &g_cargo_receipt_origin_authority_cache[i];
        if (!entry->valid ||
            memcmp(entry->authority, authority, 32) != 0) {
            continue;
        }
        bool current =
            entry->configuration_generation == generation &&
            (!entry->bind_station_tail ||
             (entry->station_chain_event_count ==
                  station->chain_event_count &&
              memcmp(entry->station_chain_last_hash,
                     station->chain_last_hash, 32) == 0)) &&
            memcmp(entry->registry_fingerprint,
                   registry_fingerprint, 32) == 0 &&
            cargo_receipt_origin_cache_file_matches(
                entry, file);
        if (!current) {
            cargo_receipt_origin_authority_cache_release(entry);
            continue;
        }
        entry->last_used = cargo_receipt_origin_cache_tick();
        g_cargo_receipt_origin_cache_stats.hits++;
        return entry;
    }
    g_cargo_receipt_origin_cache_stats.misses++;
    return NULL;
}

static cargo_receipt_origin_authority_cache_entry_t *
cargo_receipt_origin_authority_cache_victim(void) {
    size_t victim = 0;
    bool found_invalid = false;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0;
         i < CARGO_RECEIPT_ORIGIN_AUTHORITY_CACHE_CAP; i++) {
        const cargo_receipt_origin_authority_cache_entry_t *entry =
            &g_cargo_receipt_origin_authority_cache[i];
        if (!entry->valid) {
            victim = i;
            found_invalid = true;
            break;
        }
        if (entry->last_used < oldest) {
            oldest = entry->last_used;
            victim = i;
        }
    }
    if (!found_invalid)
        g_cargo_receipt_origin_cache_stats.evictions++;
    cargo_receipt_origin_authority_cache_entry_t *entry =
        &g_cargo_receipt_origin_authority_cache[victim];
    cargo_receipt_origin_authority_cache_release(entry);
    return entry;
}

typedef enum {
    CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_OK = 0,
    CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_LIMIT,
    CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_ALLOC,
} cargo_receipt_origin_index_reserve_result_t;

typedef struct {
    cargo_receipt_origin_authority_cache_entry_t *entry;
    cargo_receipt_origin_index_reserve_result_t reserve_result;
} cargo_receipt_origin_index_build_t;

static size_t cargo_receipt_origin_index_record_limit(void) {
    size_t limit = CARGO_RECEIPT_ORIGIN_INDEX_MAX_RECORDS;
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    if (g_cargo_receipt_origin_cache_test_record_limit != 0 &&
        g_cargo_receipt_origin_cache_test_record_limit < limit) {
        limit = g_cargo_receipt_origin_cache_test_record_limit;
    }
#endif
    return limit;
}

static cargo_receipt_origin_index_reserve_result_t
cargo_receipt_origin_index_reserve(
    cargo_receipt_origin_authority_cache_entry_t *entry,
    size_t needed) {
    if (!entry) return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_ALLOC;
    if (needed <= entry->record_capacity)
        return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_OK;
    size_t limit = cargo_receipt_origin_index_record_limit();
    if (needed > limit ||
        needed > SIZE_MAX / sizeof(*entry->records)) {
        return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_LIMIT;
    }

    size_t capacity = entry->record_capacity;
    if (capacity == 0)
        capacity = CARGO_RECEIPT_ORIGIN_INDEX_INITIAL_CAPACITY;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    if (capacity > limit) capacity = limit;
    if (capacity < needed ||
        capacity > SIZE_MAX / sizeof(*entry->records)) {
        return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_LIMIT;
    }

#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    if (g_cargo_receipt_origin_cache_test_fail_next_record_allocation) {
        g_cargo_receipt_origin_cache_test_fail_next_record_allocation =
            false;
        return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_ALLOC;
    }
#endif
    cargo_receipt_origin_index_record_t *records = realloc(
        entry->records, capacity * sizeof(*entry->records));
    if (!records) return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_ALLOC;
    entry->records = records;
    entry->record_capacity = capacity;
    return CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_OK;
}

static int cargo_receipt_origin_index_record_compare(
    const void *left,
    const void *right) {
    const cargo_receipt_origin_index_record_t *a =
        (const cargo_receipt_origin_index_record_t *)left;
    const cargo_receipt_origin_index_record_t *b =
        (const cargo_receipt_origin_index_record_t *)right;
    int compared = memcmp(a->cargo_pub, b->cargo_pub, 32);
    if (compared != 0) return compared;
    compared = memcmp(a->event_hash, b->event_hash, 32);
    if (compared != 0) return compared;
    if (a->event_id < b->event_id) return -1;
    if (a->event_id > b->event_id) return 1;
    if (a->event_type < b->event_type) return -1;
    if (a->event_type > b->event_type) return 1;
    return 0;
}

static bool cargo_receipt_origin_index_add(
    const chain_cargo_transform_t *transform,
    void *user) {
    cargo_receipt_origin_index_build_t *build =
        (cargo_receipt_origin_index_build_t *)user;
    if (!transform || !build || !build->entry) return false;
    cargo_receipt_origin_authority_cache_entry_t *entry =
        build->entry;
    const uint8_t *cargo_pub = NULL;
    cargo_receipt_origin_event_t event_type =
        CARGO_RECEIPT_ORIGIN_EVENT_NONE;
    if (transform->type == CHAIN_EVT_SMELT) {
        cargo_pub = transform->smelt.ingot_pub;
        event_type = CARGO_RECEIPT_ORIGIN_EVENT_SMELT;
    } else if (transform->type == CHAIN_EVT_CRAFT) {
        cargo_pub = transform->craft.output_pub;
        event_type = CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
    } else {
        return true;
    }
    if (entry->record_count == SIZE_MAX) {
        build->reserve_result =
            CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_LIMIT;
        return false;
    }
    cargo_receipt_origin_index_reserve_result_t reserve_result =
        cargo_receipt_origin_index_reserve(
            entry, entry->record_count + 1u);
    if (reserve_result != CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_OK) {
        build->reserve_result = reserve_result;
        return false;
    }
    cargo_receipt_origin_index_record_t *record =
        &entry->records[entry->record_count++];
    memset(record, 0, sizeof(*record));
    memcpy(record->cargo_pub, cargo_pub, 32);
    memcpy(record->event_hash, transform->header_hash, 32);
    record->event_id = transform->event_id;
    record->epoch = transform->epoch;
    record->event_type = (uint8_t)event_type;
    record->output_semantics_version =
        transform->output_semantics_version;
    record->output_cargo = transform->output_cargo;
    if (transform->type == CHAIN_EVT_CRAFT) {
        record->craft_recipe_id = transform->craft.recipe_id;
        record->craft_input_count = transform->craft.input_count;
    }
    return true;
}

#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
static void cargo_receipt_origin_cache_run_snapshot_hook(
    cargo_receipt_origin_cache_test_snapshot_phase_t phase) {
    cargo_receipt_origin_cache_test_snapshot_hook_t hook =
        g_cargo_receipt_origin_cache_test_snapshot_hook;
    void *user =
        g_cargo_receipt_origin_cache_test_snapshot_hook_user;
    if (!hook) return;
    if (phase ==
        CARGO_RECEIPT_ORIGIN_CACHE_TEST_AFTER_SNAPSHOT_OPEN) {
        g_cargo_receipt_origin_cache_test_snapshot_hook = NULL;
        g_cargo_receipt_origin_cache_test_snapshot_hook_user = NULL;
    }
    hook(phase, user);
}

static void cargo_receipt_origin_cache_run_build_hook(void) {
    cargo_receipt_origin_cache_test_build_hook_t hook =
        g_cargo_receipt_origin_cache_test_build_hook;
    void *user = g_cargo_receipt_origin_cache_test_build_hook_user;
    if (!hook) return;
    g_cargo_receipt_origin_cache_test_build_hook = NULL;
    g_cargo_receipt_origin_cache_test_build_hook_user = NULL;
    hook(user);
}
#endif

static cargo_receipt_origin_authority_cache_entry_t *
cargo_receipt_origin_authority_cache_build(
    const station_t *station,
    const uint8_t registry_fingerprint[32],
    const uint8_t authority[32],
    const char *path,
    const cargo_receipt_origin_file_state_t *before,
    bool *out_state_changed) {
    if (out_state_changed) *out_state_changed = false;
    cargo_receipt_origin_authority_cache_entry_t *entry =
        cargo_receipt_origin_authority_cache_victim();
    entry->valid = true;
    entry->instance_generation = cargo_receipt_origin_cache_tick();
    entry->last_used = entry->instance_generation;
    entry->configuration_generation =
        chain_log_configuration_generation();
    entry->bind_station_tail =
        memcmp(authority, station->station_pubkey, 32) == 0;
    if (entry->bind_station_tail) {
        entry->station_chain_event_count =
            station->chain_event_count;
        memcpy(entry->station_chain_last_hash,
               station->chain_last_hash, 32);
    }
    memcpy(entry->registry_fingerprint,
           registry_fingerprint, 32);
    memcpy(entry->authority, authority, 32);
    entry->file = *before;
    entry->history_status =
        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    if (!before->exists) return entry;

    /*
     * The pathname state is only a hint until the exact opened object has
     * been identified. Comparing the descriptor before and after the scan,
     * then comparing the final pathname while that descriptor is still open,
     * closes the stat/fopen A->B->A race for rotated authority histories.
     */
#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    cargo_receipt_origin_cache_run_snapshot_hook(
        CARGO_RECEIPT_ORIGIN_CACHE_TEST_BEFORE_SNAPSHOT_OPEN);
    bool fail_snapshot_open =
        g_cargo_receipt_origin_cache_test_fail_next_snapshot_open;
    g_cargo_receipt_origin_cache_test_fail_next_snapshot_open = false;
    FILE *snapshot = fail_snapshot_open ? NULL : fopen(path, "rb");
    cargo_receipt_origin_cache_run_snapshot_hook(
        CARGO_RECEIPT_ORIGIN_CACHE_TEST_AFTER_SNAPSHOT_OPEN);
#else
    FILE *snapshot = fopen(path, "rb");
#endif
    if (!snapshot) {
        cargo_receipt_origin_authority_cache_release(entry);
        return NULL;
    }

    bool transient_failure = false;
    bool state_changed = false;
    cargo_receipt_origin_file_state_t snapshot_before;
    bool snapshot_before_ready =
        cargo_receipt_origin_file_state_open(
            snapshot, &snapshot_before);
    if (!snapshot_before_ready) {
        transient_failure = true;
    } else if (!cargo_receipt_origin_file_state_equal(
                   before, &snapshot_before)) {
        state_changed = true;
    } else {
        entry->file = snapshot_before;
        chain_log_verify_report_t report;
        memset(&report, 0, sizeof(report));
        g_cargo_receipt_origin_cache_stats.full_verifications++;
        bool verified = chain_log_verify_with_pubkey(
            snapshot, authority, &report);
        bool verification_io_failure = ferror(snapshot) != 0;
        if (!verified) {
            if (verification_io_failure) {
                transient_failure = true;
            } else {
                entry->history_status =
                    CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
            }
        } else {
            cargo_receipt_origin_index_build_t build = {
                .entry = entry,
            };
            size_t transform_count = 0;
            uint8_t verified_last_hash[32] = {0};
            g_cargo_receipt_origin_cache_stats.index_builds++;
            bool indexed =
                chain_log_visit_cargo_transforms_from_verified_file(
                    snapshot, report.valid_events,
                    cargo_receipt_origin_index_add, &build,
                    &transform_count, verified_last_hash);
            bool index_io_failure = ferror(snapshot) != 0;
            if (!indexed ||
                transform_count != entry->record_count) {
                if (build.reserve_result ==
                    CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_ALLOC) {
                    transient_failure = true;
                } else if (build.reserve_result ==
                           CARGO_RECEIPT_ORIGIN_INDEX_RESERVE_LIMIT) {
                    entry->history_status =
                        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
                } else if (index_io_failure) {
                    transient_failure = true;
                } else {
                    entry->history_status =
                        CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
                }
            } else if (entry->bind_station_tail &&
                       (report.tail_event_id !=
                            station->chain_event_count ||
                        memcmp(verified_last_hash,
                               station->chain_last_hash, 32) != 0)) {
                entry->history_status =
                    CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
            } else {
                if (entry->record_count > 1u) {
                    qsort(
                        entry->records, entry->record_count,
                        sizeof(*entry->records),
                        cargo_receipt_origin_index_record_compare);
                }
                entry->history_status =
                    CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED;
            }
        }
    }

#if defined(SIGNAL_CARGO_RECEIPT_TESTING)
    cargo_receipt_origin_cache_run_build_hook();
#endif

    cargo_receipt_origin_file_state_t snapshot_after;
    bool snapshot_after_ready =
        cargo_receipt_origin_file_state_open(
            snapshot, &snapshot_after);
    if (!snapshot_after_ready) {
        transient_failure = true;
    } else if (snapshot_before_ready &&
               !cargo_receipt_origin_file_state_equal(
                   &snapshot_before, &snapshot_after)) {
        state_changed = true;
    }

    cargo_receipt_origin_file_state_t path_after;
    bool path_after_ready =
        cargo_receipt_origin_file_state(path, &path_after);
    if (!path_after_ready) {
        transient_failure = true;
    } else if (snapshot_after_ready &&
               !cargo_receipt_origin_file_state_equal(
                   &snapshot_after, &path_after)) {
        state_changed = true;
    }

    if (fclose(snapshot) != 0)
        transient_failure = true;

    if (state_changed) {
        cargo_receipt_origin_authority_cache_release(entry);
        if (out_state_changed) *out_state_changed = true;
        return NULL;
    }
    if (transient_failure) {
        cargo_receipt_origin_authority_cache_release(entry);
        return NULL;
    }

    entry->file = snapshot_after;
    if (entry->history_status !=
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
        free(entry->records);
        entry->records = NULL;
        entry->record_count = 0;
        entry->record_capacity = 0;
    }
    return entry;
}

static cargo_receipt_origin_resolve_status_t
cargo_receipt_origin_proof_from_record(
    const station_t *station,
    const uint8_t authority[32],
    const cargo_receipt_origin_index_record_t *record,
    cargo_receipt_origin_proof_t *out_proof) {
    if (!station || !authority || !record || !out_proof)
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
    cargo_receipt_authority_lifecycle_t lifecycle =
        station_authority_lifecycle_for_pubkey(
            station, authority);
    if (lifecycle ==
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
    }
    memset(out_proof, 0, sizeof(*out_proof));
    out_proof->event_type =
        (cargo_receipt_origin_event_t)record->event_type;
    out_proof->authority_lifecycle = lifecycle;
    out_proof->event_id = record->event_id;
    out_proof->epoch = record->epoch;
    out_proof->craft_recipe_id = record->craft_recipe_id;
    out_proof->craft_input_count = record->craft_input_count;
    out_proof->output_semantics_version =
        record->output_semantics_version;
    out_proof->output_cargo = record->output_cargo;
    memcpy(out_proof->event_hash, record->event_hash, 32);
    memcpy(out_proof->output_cargo_pub,
           record->cargo_pub, 32);
    memcpy(out_proof->authority, authority, 32);
    return CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED;
}

static cargo_receipt_origin_resolve_status_t
cargo_receipt_origin_authority_index_lookup(
    const station_t *station,
    cargo_receipt_origin_authority_cache_entry_t *entry,
    const uint8_t cargo_pub[32],
    const uint8_t event_hash_pin[32],
    cargo_receipt_origin_proof_t *out_proof) {
    static const uint8_t zero_hash[32] = {0};
    if (entry->history_status !=
        CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
        return entry->history_status;
    }
    if (entry->record_count > 0 && !entry->records)
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;

    size_t low = 0;
    size_t high = entry->record_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (memcmp(entry->records[middle].cargo_pub,
                   cargo_pub, 32) < 0) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    size_t first = low;
    if (first == entry->record_count ||
        memcmp(entry->records[first].cargo_pub,
               cargo_pub, 32) != 0) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
    }

    low = first;
    high = entry->record_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2u;
        if (memcmp(entry->records[middle].cargo_pub,
                   cargo_pub, 32) <= 0) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    size_t end = low;
    bool pinned = event_hash_pin &&
        memcmp(event_hash_pin, zero_hash, 32) != 0;
    if (!pinned && end - first != 1u)
        return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS;

    const cargo_receipt_origin_index_record_t *selected = NULL;
    for (size_t record_index = first;
         record_index < end; record_index++) {
        const cargo_receipt_origin_index_record_t *record =
            &entry->records[record_index];
        if (!pinned ||
            memcmp(record->event_hash,
                   event_hash_pin, 32) == 0) {
            if (selected)
                return
                    CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS;
            selected = record;
        }
    }
    if (!selected)
        return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
    return cargo_receipt_origin_proof_from_record(
        station, entry->authority, selected, out_proof);
}

bool cargo_receipt_issue(const station_t *s,
                         uint64_t epoch,
                         uint64_t event_id,
                         const uint8_t cargo_pub[32],
                         const uint8_t recipient_pubkey[32],
                         const uint8_t prev_receipt_hash[32],
                         cargo_receipt_t *out) {
    static const uint8_t zero32[32] = {0};
    if (out) memset(out, 0, sizeof(*out));
    if (!s || !out || event_id == 0 || !cargo_pub ||
        !recipient_pubkey || !prev_receipt_hash)
        return false;
    if (memcmp(s->station_pubkey, zero32, 32) == 0 ||
        memcmp(cargo_pub, zero32, 32) == 0 ||
        memcmp(recipient_pubkey, zero32, 32) == 0 ||
        memcmp(prev_receipt_hash, zero32, 32) == 0) {
        return false;
    }

    memcpy(out->cargo_pub, cargo_pub, 32);
    memcpy(out->authoring_station, s->station_pubkey, 32);
    memcpy(out->recipient_pubkey, recipient_pubkey, 32);
    out->event_id = event_id;
    out->epoch = epoch;
    memcpy(out->prev_receipt_hash, prev_receipt_hash, 32);

    uint8_t blob[CARGO_RECEIPT_UNSIGNED_SIZE];
    cargo_receipt_unsigned_pack(out, blob);
    station_sign(s, blob, sizeof(blob), out->signature);
    if (!cargo_receipt_verify_signature(out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

uint64_t cargo_receipt_emit_transfer(world_t *w, station_t *s,
                                     const uint8_t from_pubkey[32],
                                     const uint8_t to_pubkey[32],
                                     const cargo_unit_t *unit,
                                     const cargo_receipt_chain_t *incoming_chain,
                                     cargo_receipt_t *out_receipt) {
    if (out_receipt) memset(out_receipt, 0, sizeof(*out_receipt));
    if (!w || !s || !unit || !out_receipt) return 0;

    int station_index = -1;
    int station_count = w->station_count;
    if (station_count > MAX_STATIONS)
        station_count = MAX_STATIONS;
    for (int i = 0; i < station_count; i++) {
        if (s == &w->stations[i]) {
            station_index = i;
            break;
        }
    }
    if (station_index < 0) {
        return 0;
    }

    cargo_receipt_transfer_commit_result_t committed =
        cargo_receipt_commit_transfer(
            w, station_index, from_pubkey, to_pubkey,
            unit, incoming_chain, false, 0, NULL);
    if (committed.link_status !=
            CARGO_RECEIPT_TRANSFER_LINK_READY ||
        committed.append.status != CHAIN_LOG_APPEND_OK ||
        committed.append.event_count != 1u) {
        return 0;
    }
    *out_receipt = committed.receipt;
    return committed.append.first_event_id;
}

static cargo_receipt_transfer_link_t
cargo_receipt_prepare_evaluated_transfer_link(
    const station_t *station,
    const uint8_t from_pubkey[32],
    const uint8_t cargo_pub[32],
    const cargo_receipt_chain_t *incoming_chain,
    const cargo_receipt_station_evaluation_t *evaluated);

static void cargo_receipt_preparation_digest(
    const cargo_receipt_prepared_transfer_t *prepared,
    uint8_t out[32]) {
    static const uint8_t domain[8] = {
        'S', 'I', 'G', 'P', 'R', 'E', 'P', '1'
    };
    uint8_t packed[
        sizeof(domain) + 4 + 1 + 8 + 32 +
        sizeof(chain_payload_transfer_t) +
        sizeof(chain_payload_trade_t) +
        CARGO_RECEIPT_SIZE];
    size_t offset = 0;

    memcpy(&packed[offset], domain, sizeof(domain));
    offset += sizeof(domain);
    wire_write_u32_le(
        &packed[offset], (uint32_t)prepared->station_index);
    offset += 4;
    packed[offset++] = prepared->event_count;
    wire_write_u64_le(
        &packed[offset], prepared->expected_chain_event_count);
    offset += 8;
    memcpy(&packed[offset], prepared->expected_chain_last_hash, 32);
    offset += 32;
    memcpy(&packed[offset], &prepared->transfer,
           sizeof(prepared->transfer));
    offset += sizeof(prepared->transfer);
    wire_write_u64_le(
        &packed[offset], prepared->trade.transfer_event_id);
    offset += 8;
    wire_write_u64_le(
        &packed[offset],
        (uint64_t)prepared->trade.ledger_delta_signed);
    offset += 8;
    memcpy(&packed[offset], prepared->trade.ledger_pubkey, 32);
    offset += 32;
    cargo_receipt_pack(&prepared->receipt, &packed[offset]);
    offset += CARGO_RECEIPT_SIZE;
    sha256_bytes(packed, offset, out);
}

cargo_receipt_prepared_transfer_t cargo_receipt_prepare_transfer(
    const world_t *w,
    int evaluating_station,
    const uint8_t from_pubkey[32],
    const uint8_t to_pubkey[32],
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *incoming_chain,
    bool include_trade,
    int64_t ledger_delta_signed,
    const uint8_t ledger_pubkey[32]) {
    cargo_receipt_prepared_transfer_t out = {
        .link_status =
            CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
        .preflight_status = CHAIN_LOG_APPEND_BAD_ARGUMENTS,
        .station_index = -1,
    };
    static const uint8_t zero32[32] = {0};
    if (!w || !unit || evaluating_station < 0 ||
        evaluating_station >= w->station_count ||
        evaluating_station >= MAX_STATIONS ||
        !to_pubkey ||
        memcmp(to_pubkey, zero32, sizeof(zero32)) == 0 ||
        (include_trade &&
         (!ledger_pubkey ||
          memcmp(ledger_pubkey, zero32,
                 sizeof(zero32)) == 0))) {
        return out;
    }
    const station_t *station = &w->stations[evaluating_station];
    cargo_receipt_station_evaluation_t evaluated =
        cargo_receipt_evaluate_at_station(
            w, evaluating_station, unit, incoming_chain);
    if (!evaluated.accepted) {
        out.link_status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_TRUST;
        return out;
    }

    cargo_receipt_transfer_link_t link;
    if (incoming_chain && incoming_chain->len > 0) {
        link = cargo_receipt_prepare_evaluated_transfer_link(
            station, from_pubkey, unit->pub,
            incoming_chain, &evaluated);
    } else {
        link = cargo_receipt_prepare_transfer_link(
            station, from_pubkey, unit->pub, incoming_chain);
    }
    out.link_status = link.status;
    if (link.status != CARGO_RECEIPT_TRANSFER_LINK_READY)
        return out;

    const size_t event_count = include_trade ? 2u : 1u;
    if (station->chain_event_count >
        UINT64_MAX - (uint64_t)event_count) {
        out.preflight_status = CHAIN_LOG_APPEND_EVENT_ID_OVERFLOW;
        return out;
    }
    uint64_t transfer_event_id = station->chain_event_count + 1u;
    uint64_t epoch_ticks = (uint64_t)(w->time * 120.0);
    if (!cargo_receipt_issue(
            station, epoch_ticks, transfer_event_id, unit->pub,
            to_pubkey,
            link.prev_receipt_hash, &out.receipt)) {
        out.preflight_status = CHAIN_LOG_APPEND_SIGNING_FAILED;
        return out;
    }

    chain_payload_transfer_t transfer = {0};
    if (from_pubkey)
        memcpy(transfer.from_pubkey, from_pubkey, 32);
    memcpy(transfer.to_pubkey, to_pubkey, 32);
    memcpy(transfer.cargo_pub, unit->pub, 32);
    transfer.kind = unit->kind;

    chain_payload_trade_t trade = {0};
    trade.transfer_event_id = transfer_event_id;
    trade.ledger_delta_signed = ledger_delta_signed;
    if (ledger_pubkey)
        memcpy(trade.ledger_pubkey, ledger_pubkey, 32);

    out.link_status = CARGO_RECEIPT_TRANSFER_LINK_READY;
    out.preflight_status = CHAIN_LOG_APPEND_OK;
    out.station_index = evaluating_station;
    out.event_count = (uint8_t)event_count;
    out.expected_chain_event_count = station->chain_event_count;
    memcpy(out.expected_chain_last_hash, station->chain_last_hash,
           sizeof(out.expected_chain_last_hash));
    out.transfer = transfer;
    out.trade = trade;
    cargo_receipt_preparation_digest(
        &out, out.preparation_digest);
    return out;
}

chain_log_append_result_t cargo_receipt_commit_prepared_transfer(
    world_t *w,
    const cargo_receipt_prepared_transfer_t *prepared) {
    chain_log_append_result_t rejected = {
        .status = CHAIN_LOG_APPEND_BAD_ARGUMENTS,
    };
    if (!w || !prepared ||
        prepared->link_status != CARGO_RECEIPT_TRANSFER_LINK_READY ||
        prepared->preflight_status != CHAIN_LOG_APPEND_OK ||
        prepared->station_index < 0 ||
        prepared->station_index >= w->station_count ||
        prepared->station_index >= MAX_STATIONS ||
        (prepared->event_count != 1u &&
         prepared->event_count != 2u)) {
        return rejected;
    }
    station_t *station = &w->stations[prepared->station_index];
    static const uint8_t zero32[32] = {0};
    uint8_t expected_digest[32];
    cargo_receipt_preparation_digest(
        prepared, expected_digest);
    if (memcmp(prepared->preparation_digest,
               expected_digest, sizeof(expected_digest)) != 0 ||
        memcmp(prepared->transfer.from_pubkey,
               zero32, sizeof(zero32)) == 0 ||
        memcmp(prepared->transfer.to_pubkey,
               zero32, sizeof(zero32)) == 0 ||
        memcmp(prepared->transfer.cargo_pub,
               zero32, sizeof(zero32)) == 0 ||
        prepared->transfer.kind >= (uint8_t)CARGO_KIND_COUNT ||
        memcmp(prepared->receipt.authoring_station,
               station->station_pubkey, 32) != 0 ||
        prepared->receipt.event_id !=
            prepared->expected_chain_event_count + 1u ||
        memcmp(prepared->receipt.cargo_pub,
               prepared->transfer.cargo_pub, 32) != 0 ||
        memcmp(prepared->receipt.recipient_pubkey,
               prepared->transfer.to_pubkey, 32) != 0 ||
        !cargo_receipt_verify_signature(&prepared->receipt) ||
        (prepared->event_count == 2u &&
         (prepared->trade.transfer_event_id !=
              prepared->receipt.event_id ||
          memcmp(prepared->trade.ledger_pubkey,
                 zero32, sizeof(zero32)) == 0))) {
        return rejected;
    }
    if (station->chain_event_count !=
            prepared->expected_chain_event_count ||
        memcmp(station->chain_last_hash,
               prepared->expected_chain_last_hash, 32) != 0) {
        rejected.status = CHAIN_LOG_APPEND_BLOCKED;
        return rejected;
    }
    chain_log_batch_event_t events[2] = {
        {
            .type = CHAIN_EVT_TRANSFER,
            .payload = &prepared->transfer,
            .payload_len = (uint16_t)sizeof(prepared->transfer),
        },
        {
            .type = CHAIN_EVT_TRADE,
            .payload = &prepared->trade,
            .payload_len = (uint16_t)sizeof(prepared->trade),
        },
    };
    uint64_t expected_file_growth =
        CHAIN_EVENT_HEADER_SIZE + sizeof(uint16_t) +
        sizeof(prepared->transfer);
    if (prepared->event_count == 2u) {
        expected_file_growth +=
            CHAIN_EVENT_HEADER_SIZE + sizeof(uint16_t) +
            sizeof(prepared->trade);
    }
    cargo_receipt_origin_trusted_append_t trusted_append =
        cargo_receipt_origin_cache_begin_trusted_append(
            station, prepared->event_count,
            expected_file_growth);
    chain_log_append_result_t append = chain_log_emit_batch(
        w, station, events, prepared->event_count);
    cargo_receipt_origin_cache_finish_trusted_append(
        &trusted_append, station, &append);
    return append;
}

cargo_receipt_transfer_commit_result_t cargo_receipt_commit_transfer(
    world_t *w,
    int evaluating_station,
    const uint8_t from_pubkey[32],
    const uint8_t to_pubkey[32],
    const cargo_unit_t *unit,
    const cargo_receipt_chain_t *incoming_chain,
    bool include_trade,
    int64_t ledger_delta_signed,
    const uint8_t ledger_pubkey[32]) {
    cargo_receipt_transfer_commit_result_t out = {
        .link_status =
            CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
        .append = {
            .status = CHAIN_LOG_APPEND_BAD_ARGUMENTS,
        },
    };
    cargo_receipt_prepared_transfer_t prepared =
        cargo_receipt_prepare_transfer(
            w, evaluating_station, from_pubkey, to_pubkey,
            unit, incoming_chain, include_trade,
            ledger_delta_signed, ledger_pubkey);
    out.link_status = prepared.link_status;
    out.append.status = prepared.preflight_status;
    if (prepared.link_status != CARGO_RECEIPT_TRANSFER_LINK_READY ||
        prepared.preflight_status != CHAIN_LOG_APPEND_OK) {
        return out;
    }
    out.append = cargo_receipt_commit_prepared_transfer(w, &prepared);
    if (out.append.status != CHAIN_LOG_APPEND_OK ||
        out.append.first_event_id !=
            prepared.expected_chain_event_count + 1u) {
        memset(&out.receipt, 0, sizeof(out.receipt));
    } else {
        out.receipt = prepared.receipt;
    }
    return out;
}

static cargo_receipt_origin_resolve_status_t
cargo_receipt_resolve_exact_identity(
    const station_t *station,
    const uint8_t authority[32],
    const uint8_t cargo_pub[32],
    const uint8_t event_hash_pin[32],
    cargo_receipt_origin_proof_t *out_proof) {
    char path[256];
    if (!chain_log_path_for(authority, path, sizeof(path)))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    cargo_receipt_origin_file_state_t before;
    if (!cargo_receipt_origin_file_state(path, &before))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;

    uint8_t registry_fingerprint[32];
    cargo_receipt_origin_registry_fingerprint(
        station, registry_fingerprint);
    cargo_receipt_origin_authority_cache_entry_t *entry =
        cargo_receipt_origin_authority_cache_lookup(
            station, registry_fingerprint, authority,
            &before);
    if (!entry) {
        bool state_changed = false;
        entry = cargo_receipt_origin_authority_cache_build(
            station, registry_fingerprint, authority,
            path, &before, &state_changed);
        if (!entry) {
            memset(out_proof, 0, sizeof(*out_proof));
            return state_changed
                ? CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID
                : CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
        }
    }
    cargo_receipt_origin_resolve_status_t status =
        cargo_receipt_origin_authority_index_lookup(
            station, entry, cargo_pub,
            event_hash_pin, out_proof);

    cargo_receipt_origin_file_state_t after;
    if (!cargo_receipt_origin_file_state(path, &after)) {
        cargo_receipt_origin_authority_cache_release(entry);
        memset(out_proof, 0, sizeof(*out_proof));
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    }
    if (!cargo_receipt_origin_cache_file_matches(entry, &after)) {
        cargo_receipt_origin_authority_cache_release(entry);
        memset(out_proof, 0, sizeof(*out_proof));
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
    }
    return status;
}

cargo_receipt_origin_resolve_status_t
cargo_receipt_resolve_origin_for_authority(
    const station_t *station,
    const uint8_t authority[32],
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof) {
    static const uint8_t zero32[32] = {0};
    if (out_proof) memset(out_proof, 0, sizeof(*out_proof));
    if (!station || !authority || !cargo_pub || !out_proof ||
        memcmp(authority, zero32, sizeof(zero32)) == 0 ||
        memcmp(cargo_pub, zero32, sizeof(zero32)) == 0) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS;
    }
    if (!chain_log_disk_enabled())
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    if (!station_authority_registry_validate(station))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
    if (station_authority_lifecycle_for_pubkey(station, authority) ==
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
    }
    return cargo_receipt_resolve_exact_identity(
        station, authority, cargo_pub, NULL, out_proof);
}

cargo_receipt_origin_resolve_status_t
cargo_receipt_resolve_origin_for_authority_pinned(
    const station_t *station,
    const uint8_t authority[32],
    const uint8_t cargo_pub[32],
    const uint8_t event_hash_pin[32],
    cargo_receipt_origin_proof_t *out_proof) {
    static const uint8_t zero32[32] = {0};
    if (out_proof) memset(out_proof, 0, sizeof(*out_proof));
    if (!station || !authority || !cargo_pub ||
        !event_hash_pin || !out_proof ||
        memcmp(authority, zero32, sizeof(zero32)) == 0 ||
        memcmp(cargo_pub, zero32, sizeof(zero32)) == 0 ||
        memcmp(event_hash_pin, zero32, sizeof(zero32)) == 0) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS;
    }
    if (!chain_log_disk_enabled())
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    if (!station_authority_registry_validate(station))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;
    if (station_authority_lifecycle_for_pubkey(
            station, authority) ==
        CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
    }
    return cargo_receipt_resolve_exact_identity(
        station, authority, cargo_pub,
        event_hash_pin, out_proof);
}

cargo_receipt_origin_resolve_status_t cargo_receipt_resolve_local_origin(
    const station_t *station,
    const uint8_t cargo_pub[32],
    cargo_receipt_origin_proof_t *out_proof) {
    static const uint8_t zero32[32] = {0};
    if (out_proof) memset(out_proof, 0, sizeof(*out_proof));
    if (!station || !cargo_pub || !out_proof ||
        memcmp(cargo_pub, zero32, sizeof(zero32)) == 0) {
        return CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS;
    }
    if (!chain_log_disk_enabled())
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    if (!station_authority_registry_validate(station))
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID;

    bool saw_history = false;
    bool missing_history = false;
    bool missing_empty_current = false;
    bool found_origin = false;
    cargo_receipt_origin_proof_t found_proof = {0};
    for (uint8_t i = 0; i < station->authority_registry_count; i++) {
        const station_authority_record_t *record =
            &station->authority_registry[i];
        if (record->lifecycle ==
            STATION_AUTHORITY_LIFECYCLE_UNSPECIFIED) {
            continue;
        }
        cargo_receipt_origin_proof_t proof = {0};
        cargo_receipt_origin_resolve_status_t status =
            cargo_receipt_resolve_exact_identity(
                station, record->pubkey, cargo_pub,
                NULL, &proof);
        if (status == CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
            if (found_origin)
                return
                    CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS;
            found_origin = true;
            found_proof = proof;
            saw_history = true;
            continue;
        }
        if (status == CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID ||
            status ==
                CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS) {
            return status;
        }
        if (status == CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE) {
            /*
             * A freshly rotated current identity has a committed empty
             * continuation but no log file yet. That is complete evidence of
             * an empty history, not an unknown competing origin. A non-empty
             * missing current tail and every missing historical identity
             * remain fail-closed.
             */
            if (memcmp(record->pubkey,
                       station->station_pubkey, 32) == 0 &&
                station->chain_event_count == 0 &&
                memcmp(station->chain_last_hash,
                       zero32, sizeof(zero32)) == 0) {
                missing_empty_current = true;
            } else {
                missing_history = true;
            }
        } else if (status ==
                   CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND) {
            saw_history = true;
        }
    }
    if (found_origin && !missing_history) {
        *out_proof = found_proof;
        return CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED;
    }
    if (!saw_history || missing_history || missing_empty_current)
        return CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE;
    return CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND;
}

const char *cargo_receipt_origin_resolve_status_name(
    cargo_receipt_origin_resolve_status_t status) {
    switch (status) {
        case CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED:
            return "not_attempted";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED:
            return "verified";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS:
            return "bad_arguments";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_UNAVAILABLE:
            return "history_unavailable";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_HISTORY_INVALID:
            return "history_invalid";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_NOT_FOUND:
            return "transform_not_found";
        case CARGO_RECEIPT_ORIGIN_RESOLVE_TRANSFORM_AMBIGUOUS:
            return "transform_ambiguous";
        default:
            return "unknown";
    }
}

cargo_receipt_transfer_link_t cargo_receipt_prepare_transfer_link(
    const station_t *station,
    const uint8_t from_pubkey[32],
    const uint8_t cargo_pub[32],
    const cargo_receipt_chain_t *incoming_chain) {
    static const uint8_t zero32[32] = {0};
    cargo_receipt_transfer_link_t out = {
        .status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
        .chain_result = CARGO_RECEIPT_REJECT_EMPTY,
        .origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS,
        .origin_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED,
        .origin_trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
    };
    if (!station || !cargo_pub) return out;
    if (!station_authority_registry_validate(station)) {
        out.status =
            CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN_AUTHORITY;
        out.origin_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED;
        out.origin_trust = CARGO_RECEIPT_AUTHORITY_REVOKED;
        return out;
    }

    if (incoming_chain && incoming_chain->len > 0) {
        if (incoming_chain->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
            out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN_FULL;
            return out;
        }
        out.chain_result = cargo_receipt_chain_verify(
            incoming_chain->links, incoming_chain->len, cargo_pub);
        if (out.chain_result != CARGO_RECEIPT_OK) {
            out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN;
            return out;
        }
        if (!from_pubkey ||
            memcmp(from_pubkey, zero32, sizeof(zero32)) == 0 ||
            memcmp(incoming_chain->links[
                       incoming_chain->len - 1].recipient_pubkey,
                   from_pubkey, sizeof(zero32)) != 0) {
            out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY;
            return out;
        }
        cargo_receipt_hash(
            &incoming_chain->links[incoming_chain->len - 1],
            out.prev_receipt_hash);
        out.origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED;
        out.status = CARGO_RECEIPT_TRANSFER_LINK_READY;
        return out;
    }

    if (!from_pubkey ||
        memcmp(from_pubkey, zero32, sizeof(zero32)) == 0 ||
        memcmp(from_pubkey, station->station_pubkey,
               sizeof(zero32)) != 0) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY;
        return out;
    }

    cargo_receipt_origin_proof_t proof;
    out.origin_status = cargo_receipt_resolve_local_origin(
        station, cargo_pub, &proof);
    if (out.origin_status != CARGO_RECEIPT_ORIGIN_RESOLVE_VERIFIED) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN;
        return out;
    }
    out.origin_lifecycle = proof.authority_lifecycle;
    out.origin_trust =
        station_authority_trust_for_pubkey(station, proof.authority);
    if (out.origin_lifecycle !=
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_CURRENT ||
        out.origin_trust !=
            CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT ||
        memcmp(proof.authority, station->station_pubkey, 32) != 0) {
        out.status =
            CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN_AUTHORITY;
        return out;
    }
    memcpy(out.prev_receipt_hash, proof.event_hash,
           sizeof(out.prev_receipt_hash));
    out.chain_result = CARGO_RECEIPT_OK;
    out.status = CARGO_RECEIPT_TRANSFER_LINK_READY;
    return out;
}

/*
 * `cargo_receipt_prepare_transfer()` calls this only after the immediately
 * preceding station evaluation accepted this exact unit and chain. That
 * evaluation already performed the complete signature/linkage/cargo walk.
 * Recheck the independent live-station and custody invariants here, but do
 * not repeat an O(chain length) Ed25519 verification pass.
 */
static cargo_receipt_transfer_link_t
cargo_receipt_prepare_evaluated_transfer_link(
    const station_t *station,
    const uint8_t from_pubkey[32],
    const uint8_t cargo_pub[32],
    const cargo_receipt_chain_t *incoming_chain,
    const cargo_receipt_station_evaluation_t *evaluated) {
    static const uint8_t zero32[32] = {0};
    cargo_receipt_transfer_link_t out = {
        .status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_BAD_ARGUMENTS,
        .chain_result = CARGO_RECEIPT_REJECT_EMPTY,
        .origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_BAD_ARGUMENTS,
        .origin_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_UNSPECIFIED,
        .origin_trust = CARGO_RECEIPT_AUTHORITY_UNKNOWN,
    };
    if (!station || !cargo_pub || !incoming_chain || !evaluated ||
        incoming_chain->len == 0) {
        return out;
    }
    if (!station_authority_registry_validate(station)) {
        out.status =
            CARGO_RECEIPT_TRANSFER_LINK_REJECT_ORIGIN_AUTHORITY;
        out.origin_lifecycle =
            CARGO_RECEIPT_AUTHORITY_LIFECYCLE_REVOKED;
        out.origin_trust = CARGO_RECEIPT_AUTHORITY_REVOKED;
        return out;
    }
    if (incoming_chain->len >= CARGO_RECEIPT_CHAIN_MAX_LEN) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN_FULL;
        return out;
    }
    if (!evaluated->accepted ||
        !evaluated->trust.chain_checked ||
        evaluated->trust.chain_result != CARGO_RECEIPT_OK) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN;
        out.chain_result = evaluated->trust.chain_result;
        return out;
    }

    const cargo_receipt_t *tail =
        &incoming_chain->links[incoming_chain->len - 1u];
    if (memcmp(tail->cargo_pub, cargo_pub, sizeof(zero32)) != 0) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CHAIN;
        out.chain_result = CARGO_RECEIPT_REJECT_CARGO_MISMATCH;
        return out;
    }
    out.chain_result = CARGO_RECEIPT_OK;
    if (!from_pubkey ||
        memcmp(from_pubkey, zero32, sizeof(zero32)) == 0 ||
        memcmp(tail->recipient_pubkey,
               from_pubkey, sizeof(zero32)) != 0) {
        out.status = CARGO_RECEIPT_TRANSFER_LINK_REJECT_CUSTODY;
        return out;
    }

    cargo_receipt_hash(tail, out.prev_receipt_hash);
    out.origin_status = CARGO_RECEIPT_ORIGIN_RESOLVE_NOT_ATTEMPTED;
    out.status = CARGO_RECEIPT_TRANSFER_LINK_READY;
    return out;
}

static bool receipt_chain_prefix_matches(const cargo_receipt_chain_t *existing,
                                         const cargo_receipt_t *presented,
                                         uint8_t presented_len) {
    if (!existing || existing->len == 0) return true;
    if (!presented || presented_len < existing->len) return false;
    for (uint8_t i = 0; i < existing->len; i++) {
        if (memcmp(&existing->links[i], &presented[i],
                   sizeof(existing->links[i])) != 0) {
            return false;
        }
    }
    return true;
}

cargo_receipt_present_result_t cargo_receipt_present_to_ship(
    const world_t *world,
    int evaluating_station,
    server_player_t *sp,
    const uint8_t cargo_pub[32],
    const cargo_receipt_t *chain,
    uint8_t chain_len) {
    static const uint8_t zero32[32] = {0};

    if (!world || !sp || !sp->ship || !cargo_pub || !chain ||
        chain_len == 0 ||
        chain_len > CARGO_RECEIPT_CHAIN_MAX_LEN) {
        return CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS;
    }
    if (!server_player_can_use_pubkey_persistence(sp) ||
        memcmp(sp->pubkey, zero32, 32) == 0)
        return CARGO_RECEIPT_PRESENT_REJECT_NO_PLAYER_KEY;
    if (memcmp(cargo_pub, zero32, 32) == 0)
        return CARGO_RECEIPT_PRESENT_REJECT_BAD_ARGS;

    if (cargo_receipt_chain_verify(chain, chain_len, cargo_pub) !=
        CARGO_RECEIPT_OK) {
        return CARGO_RECEIPT_PRESENT_REJECT_VERIFY;
    }
    if (memcmp(chain[chain_len - 1].recipient_pubkey, sp->pubkey, 32) != 0)
        return CARGO_RECEIPT_PRESENT_REJECT_RECIPIENT;

    int idx = manifest_find(&sp->ship->manifest, cargo_pub);
    if (idx < 0) return CARGO_RECEIPT_PRESENT_REJECT_NOT_CARRIED;

    cargo_receipt_chain_t presented = {.len = chain_len};
    memcpy(presented.links, chain,
           (size_t)chain_len * sizeof(presented.links[0]));
    cargo_receipt_station_evaluation_t trust =
        cargo_receipt_evaluate_at_station(
            world, evaluating_station,
            &sp->ship->manifest.units[idx], &presented);
    if (!trust.accepted) return CARGO_RECEIPT_PRESENT_REJECT_TRUST;

    cargo_store_t staged = {0};
    if (!cargo_store_clone(&staged, &sp->ship->cargo_store))
        return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;
    uint16_t default_cap = staged.manifest.cap > 0
        ? staged.manifest.cap : SHIP_MANIFEST_DEFAULT_CAP;
    if (!cargo_store_bootstrap(&staged, default_cap)) {
        cargo_store_cleanup(&staged);
        return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;
    }

    ship_receipts_t *receipts = cargo_store_receipts(&staged);
    if (!receipts || (uint16_t)idx >= receipts->count) {
        cargo_store_cleanup(&staged);
        return CARGO_RECEIPT_PRESENT_REJECT_RECEIPT_STORE;
    }
    cargo_receipt_chain_t *slot = &receipts->chains[idx];
    if (!receipt_chain_prefix_matches(slot, chain, chain_len)) {
        cargo_store_cleanup(&staged);
        return CARGO_RECEIPT_PRESENT_REJECT_EXISTING_MISMATCH;
    }

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->links, chain, (size_t)chain_len * sizeof(chain[0]));
    slot->len = chain_len;
    cargo_store_cleanup(&sp->ship->cargo_store);
    sp->ship->cargo_store = staged;
    return CARGO_RECEIPT_PRESENT_OK;
}
