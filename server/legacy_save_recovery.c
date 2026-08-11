#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "legacy_save_recovery.h"

#include "persistence_io.h"
#include "sha256.h"
#include "signal_crypto.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <fcntl.h>
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

#define LEGACY_RECOVERY_AUDIT_NAME "legacy-recovery.audit"

static bool recovery_bytes_nonzero(const uint8_t *bytes, size_t size) {
    uint8_t any = 0;
    if (!bytes) return false;
    for (size_t i = 0; i < size; i++) any |= bytes[i];
    return any != 0;
}

static void recovery_hash_u64(sha256_ctx_t *ctx, uint64_t value) {
    uint8_t encoded[8];
    for (size_t i = 0; i < sizeof(encoded); i++)
        encoded[i] = (uint8_t)(value >> (8u * i));
    sha256_update(ctx, encoded, sizeof(encoded));
}

static bool legacy_recovery_binding_digest(
    const legacy_recovery_offer_t *offer,
    const server_player_t *player,
    uint8_t out[32]) {
    static const char domain[] = "SIGNAL-legacy-recovery-offer-v1";
    if (!offer || !player || !out ||
        offer->connection_generation == 0 ||
        offer->expires_at_ms == 0 ||
        !server_player_can_use_pubkey_persistence(player) ||
        player->pubkey_identity_finalized ||
        !recovery_bytes_nonzero(
            player->pubkey_proof_transcript,
            sizeof(player->pubkey_proof_transcript)) ||
        !recovery_bytes_nonzero(
            offer->offer_id, sizeof(offer->offer_id))) {
        if (out) memset(out, 0, 32);
        return false;
    }
    sha256_ctx_t hash;
    sha256_init(&hash);
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    sha256_update(&hash, offer->offer_id, sizeof(offer->offer_id));
    recovery_hash_u64(&hash, offer->connection_generation);
    recovery_hash_u64(&hash, offer->expires_at_ms);
    sha256_update(&hash, player->session_token,
                  sizeof(player->session_token));
    sha256_update(&hash, player->pubkey, sizeof(player->pubkey));
    sha256_update(&hash, player->pubkey_proof_transcript,
                  sizeof(player->pubkey_proof_transcript));
    sha256_final(&hash, out);
    return true;
}

void legacy_recovery_offer_clear(legacy_recovery_offer_t *offer) {
    if (offer) memset(offer, 0, sizeof(*offer));
}

bool legacy_recovery_offer_expired(
    const legacy_recovery_offer_t *offer,
    uint64_t now_ms) {
    return offer &&
           offer->phase == LEGACY_RECOVERY_OFFER_AVAILABLE &&
           (offer->expires_at_ms == 0 ||
            now_ms >= offer->expires_at_ms);
}

bool legacy_recovery_offer_blocks_persistence(
    const legacy_recovery_offer_t *offer,
    uint64_t now_ms) {
    if (!offer) return false;
    if (offer->phase == LEGACY_RECOVERY_OFFER_IN_FLIGHT)
        return true;
    return offer->phase ==
               LEGACY_RECOVERY_OFFER_AVAILABLE &&
           !legacy_recovery_offer_expired(offer, now_ms);
}

bool legacy_recovery_offer_issue(
    legacy_recovery_offer_t *offer,
    const world_t *world,
    int player_idx,
    uint64_t connection_generation,
    uint64_t now_ms,
    uint64_t ttl_ms) {
    if (!offer || !world ||
        player_idx < 0 || player_idx >= MAX_PLAYERS ||
        connection_generation == 0 ||
        ttl_ms == 0 || ttl_ms > LEGACY_RECOVERY_MAX_TTL_MS ||
        UINT64_MAX - now_ms < ttl_ms) {
        legacy_recovery_offer_clear(offer);
        return false;
    }
    const server_player_t *player = &world->players[player_idx];
    if (!server_player_can_use_pubkey_persistence(player) ||
        player->pubkey_identity_finalized ||
        !recovery_bytes_nonzero(
            player->pubkey_proof_transcript,
            sizeof(player->pubkey_proof_transcript))) {
        legacy_recovery_offer_clear(offer);
        return false;
    }

    legacy_recovery_offer_t staged = {
        .phase = LEGACY_RECOVERY_OFFER_AVAILABLE,
        .connection_generation = connection_generation,
        .expires_at_ms = now_ms + ttl_ms,
    };
    if (!signal_crypto_random_bytes(
            staged.offer_id, sizeof(staged.offer_id)) ||
        !recovery_bytes_nonzero(
            staged.offer_id, sizeof(staged.offer_id)) ||
        !legacy_recovery_binding_digest(
            &staged, player, staged.binding_digest)) {
        legacy_recovery_offer_clear(offer);
        return false;
    }
    *offer = staged;
    return true;
}

legacy_recovery_result_status_t legacy_recovery_offer_begin(
    legacy_recovery_offer_t *offer,
    const world_t *world,
    int player_idx,
    uint64_t connection_generation,
    uint64_t now_ms,
    const uint8_t *payload,
    uint16_t payload_len) {
    if (!offer) return LEGACY_RECOVERY_RESULT_STALE_OFFER;
    if (offer->phase == LEGACY_RECOVERY_OFFER_IN_FLIGHT) {
        legacy_recovery_offer_clear(offer);
        return LEGACY_RECOVERY_RESULT_REPLAY;
    }
    if (!world ||
        player_idx < 0 || player_idx >= MAX_PLAYERS ||
        connection_generation == 0 ||
        offer->phase != LEGACY_RECOVERY_OFFER_AVAILABLE ||
        offer->connection_generation != connection_generation ||
        !payload ||
        payload_len != LEGACY_RECOVERY_OFFER_ID_SIZE) {
        legacy_recovery_offer_clear(offer);
        return LEGACY_RECOVERY_RESULT_STALE_OFFER;
    }
    if (legacy_recovery_offer_expired(offer, now_ms)) {
        legacy_recovery_offer_clear(offer);
        return LEGACY_RECOVERY_RESULT_STALE_OFFER;
    }
    const server_player_t *player = &world->players[player_idx];
    uint8_t expected[32];
    if (!legacy_recovery_binding_digest(offer, player, expected) ||
        memcmp(expected, offer->binding_digest, sizeof(expected)) != 0 ||
        memcmp(payload, offer->offer_id,
               LEGACY_RECOVERY_OFFER_ID_SIZE) != 0) {
        legacy_recovery_offer_clear(offer);
        return LEGACY_RECOVERY_RESULT_STALE_OFFER;
    }
    offer->phase = LEGACY_RECOVERY_OFFER_IN_FLIGHT;
    return LEGACY_RECOVERY_RESULT_SUCCESS;
}

void legacy_recovery_offer_finish(legacy_recovery_offer_t *offer) {
    legacy_recovery_offer_clear(offer);
}

int legacy_recovery_serialize_offer(
    uint8_t out[NET_LEGACY_RECOVERY_OFFER_SIZE],
    const legacy_recovery_offer_t *offer,
    uint64_t now_ms) {
    if (!out || !offer ||
        offer->phase != LEGACY_RECOVERY_OFFER_AVAILABLE ||
        legacy_recovery_offer_expired(offer, now_ms)) {
        return 0;
    }
    uint64_t remaining_ms = offer->expires_at_ms - now_ms;
    uint64_t seconds = (remaining_ms + 999u) / 1000u;
    if (seconds == 0) seconds = 1;
    if (seconds > UINT16_MAX) seconds = UINT16_MAX;
    out[0] = NET_MSG_LEGACY_RECOVERY_OFFER;
    memcpy(&out[1], offer->offer_id, sizeof(offer->offer_id));
    out[1 + LEGACY_RECOVERY_OFFER_ID_SIZE] =
        (uint8_t)(seconds & 0xffu);
    out[2 + LEGACY_RECOVERY_OFFER_ID_SIZE] =
        (uint8_t)(seconds >> 8);
    return NET_LEGACY_RECOVERY_OFFER_SIZE;
}

int legacy_recovery_serialize_result(
    uint8_t out[NET_LEGACY_RECOVERY_RESULT_SIZE],
    legacy_recovery_result_status_t status) {
    if (!out ||
        status < LEGACY_RECOVERY_RESULT_NO_MATCH ||
        status > LEGACY_RECOVERY_RESULT_SUCCESS) {
        return 0;
    }
    out[0] = NET_MSG_LEGACY_RECOVERY_RESULT;
    out[1] = (uint8_t)status;
    return NET_LEGACY_RECOVERY_RESULT_SIZE;
}

const char *legacy_recovery_result_name(
    legacy_recovery_result_status_t status) {
    switch (status) {
    case LEGACY_RECOVERY_RESULT_NO_MATCH:
        return "no-match";
    case LEGACY_RECOVERY_RESULT_STALE_OFFER:
        return "stale-offer";
    case LEGACY_RECOVERY_RESULT_REPLAY:
        return "replay";
    case LEGACY_RECOVERY_RESULT_INVALID_SOURCE:
        return "invalid-source";
    case LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT:
        return "destination-conflict";
    case LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE:
        return "migration-failure";
    case LEGACY_RECOVERY_RESULT_SUCCESS:
        return "success";
    default:
        return "unknown";
    }
}

static bool recovery_audit_path(
    char *out, size_t out_size, const char *root_dir) {
    if (!out || out_size == 0 || !root_dir || !root_dir[0])
        return false;
    int n = snprintf(out, out_size, "%s/%s",
                     root_dir, LEGACY_RECOVERY_AUDIT_NAME);
    return n > 0 && (size_t)n < out_size;
}

bool legacy_recovery_audit_append(
    const char *root_dir,
    legacy_recovery_result_status_t status) {
    const char *name = legacy_recovery_result_name(status);
    if (strcmp(name, "unknown") == 0) return false;
    char path[512];
    if (!recovery_audit_path(path, sizeof(path), root_dir))
        return false;
    char line[64];
    int line_len = snprintf(line, sizeof(line),
                            "v1 status=%s\n", name);
    if (line_len <= 0 || (size_t)line_len >= sizeof(line))
        return false;

#ifdef _WIN32
    HANDLE handle = CreateFileA(
        path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, NULL, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileType(handle) == FILE_TYPE_DISK &&
              GetFileInformationByHandle(handle, &info) != 0 &&
              (info.dwFileAttributes &
               (FILE_ATTRIBUTE_DIRECTORY |
                FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
              info.nNumberOfLinks == 1;
    size_t offset = 0;
    while (ok && offset < (size_t)line_len) {
        DWORD written = 0;
        if (!WriteFile(handle, line + offset,
                       (DWORD)((size_t)line_len - offset),
                       &written, NULL) ||
            written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(handle) != 0;
    if (!CloseHandle(handle)) ok = false;
#else
    int flags = O_WRONLY | O_CREAT | O_APPEND;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags, 0600);
    if (fd < 0) return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 &&
              S_ISREG(st.st_mode) &&
              st.st_nlink == 1;
#ifndef O_NOFOLLOW
    struct stat named;
    if (ok) {
        ok = lstat(path, &named) == 0 &&
             S_ISREG(named.st_mode) &&
             named.st_dev == st.st_dev &&
             named.st_ino == st.st_ino;
    }
#endif
    size_t offset = 0;
    while (ok && offset < (size_t)line_len) {
        ssize_t written = write(
            fd, line + offset, (size_t)line_len - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += (size_t)written;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
#endif
    return ok && persistence_sync_parent_dir(path);
}
