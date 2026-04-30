/*
 * chain_log.c -- Per-station signed event chain log (Layer C of #479).
 * See chain_log.h for the high-level scheme.
 */
#include "chain_log.h"

#include "game_sim.h"          /* world_t, SIM_LOG */
#include "station_authority.h" /* station_sign / station_verify */
#include "sha256.h"
#include "base58.h"
#include "signal_crypto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0775)
#endif

/* Compile-time guarantee that the header serializes exactly the way
 * we read/write it. If anyone reorders or repads the struct, this
 * fires before the on-disk format silently drifts. */
_Static_assert(sizeof(chain_event_header_t) == CHAIN_EVENT_HEADER_SIZE,
               "chain_event_header_t must be exactly 184 bytes — "
               "the on-disk + signed-message format depends on it");

/* The unsigned-header span signed by the station: epoch (8) + event_id
 * (8) + type (1) + pad (7) + authority (32) + payload_hash (32) +
 * prev_hash (32) = 120 bytes — i.e. everything BEFORE signature[]. */
#define CHAIN_UNSIGNED_HEADER_SIZE 120
_Static_assert(CHAIN_UNSIGNED_HEADER_SIZE ==
                   CHAIN_EVENT_HEADER_SIZE - 64,
               "unsigned-header span must equal sizeof header minus the "
               "64-byte trailing signature");

/* ------------------------------------------------------------------ */
/* Configurable on-disk root                                           */
/* ------------------------------------------------------------------ */

static char g_chain_dir[256] = "chain";

void chain_log_set_dir(const char *dir) {
    if (!dir || dir[0] == '\0') {
        snprintf(g_chain_dir, sizeof(g_chain_dir), "chain");
    } else {
        snprintf(g_chain_dir, sizeof(g_chain_dir), "%s", dir);
    }
}

const char *chain_log_get_dir(void) {
    return g_chain_dir;
}

/* mkdir -p the chain dir. Best-effort — collisions / permission
 * failures are logged once and emits then fail at fopen time. */
static void ensure_chain_dir(void) {
    struct stat st;
    if (stat(g_chain_dir, &st) == 0) return;
    if (MKDIR(g_chain_dir) != 0 && errno != EEXIST) {
        SIM_LOG("[chain] mkdir(%s) failed: %s\n", g_chain_dir, strerror(errno));
    }
}

bool chain_log_path_for(const uint8_t pubkey[32], char *out, size_t cap) {
    if (!pubkey || !out || cap < 64) return false;
    char b58[64];
    size_t n = base58_encode(pubkey, 32, b58, sizeof(b58));
    if (n == 0 || n >= sizeof(b58)) return false;
    int written = snprintf(out, cap, "%s/%s.log", g_chain_dir, b58);
    return written > 0 && (size_t)written < cap;
}

/* ------------------------------------------------------------------ */
/* Header serialization helpers                                        */
/* ------------------------------------------------------------------ */

/* Pack the header into the canonical on-disk byte order. We pack
 * little-endian so the format is portable regardless of host
 * endianness. */
static void chain_event_header_pack(const chain_event_header_t *h,
                                    uint8_t out[CHAIN_EVENT_HEADER_SIZE]) {
    size_t off = 0;
    /* epoch */
    for (int i = 0; i < 8; i++) out[off + i] = (uint8_t)(h->epoch >> (i * 8));
    off += 8;
    /* event_id */
    for (int i = 0; i < 8; i++) out[off + i] = (uint8_t)(h->event_id >> (i * 8));
    off += 8;
    /* type + pad */
    out[off++] = h->type;
    memset(&out[off], 0, 7);
    off += 7;
    /* authority */
    memcpy(&out[off], h->authority, 32); off += 32;
    /* payload_hash */
    memcpy(&out[off], h->payload_hash, 32); off += 32;
    /* prev_hash */
    memcpy(&out[off], h->prev_hash, 32); off += 32;
    /* signature */
    memcpy(&out[off], h->signature, 64); off += 64;
    (void)off;
}

static bool chain_event_header_unpack(const uint8_t in[CHAIN_EVENT_HEADER_SIZE],
                                      chain_event_header_t *out) {
    size_t off = 0;
    out->epoch = 0;
    for (int i = 0; i < 8; i++)
        out->epoch |= (uint64_t)in[off + i] << (i * 8);
    off += 8;
    out->event_id = 0;
    for (int i = 0; i < 8; i++)
        out->event_id |= (uint64_t)in[off + i] << (i * 8);
    off += 8;
    out->type = in[off++];
    /* pad must be zero */
    for (int i = 0; i < 7; i++) {
        if (in[off + i] != 0) return false;
    }
    memset(out->_pad, 0, sizeof(out->_pad));
    off += 7;
    memcpy(out->authority, &in[off], 32); off += 32;
    memcpy(out->payload_hash, &in[off], 32); off += 32;
    memcpy(out->prev_hash, &in[off], 32); off += 32;
    memcpy(out->signature, &in[off], 64); off += 64;
    (void)off;
    return true;
}

void chain_event_header_hash(const chain_event_header_t *h, uint8_t out[32]) {
    uint8_t packed[CHAIN_EVENT_HEADER_SIZE];
    chain_event_header_pack(h, packed);
    sha256_bytes(packed, CHAIN_EVENT_HEADER_SIZE, out);
}

/* Pack just the unsigned-header span (the 120 bytes that get signed). */
static void chain_event_unsigned_pack(const chain_event_header_t *h,
                                      uint8_t out[CHAIN_UNSIGNED_HEADER_SIZE]) {
    uint8_t full[CHAIN_EVENT_HEADER_SIZE];
    chain_event_header_pack(h, full);
    memcpy(out, full, CHAIN_UNSIGNED_HEADER_SIZE);
}

/* ------------------------------------------------------------------ */
/* Emit                                                                */
/* ------------------------------------------------------------------ */

uint64_t chain_log_emit(world_t *w, station_t *s, chain_event_type_t type,
                        const void *payload, uint16_t payload_len) {
    static const uint8_t zero_pub[32] = {0};

    if (!s) return 0;
    if (type <= CHAIN_EVT_NONE || type >= CHAIN_EVT_TYPE_COUNT) return 0;
    if (payload_len > 0 && !payload) return 0;
    /* Stations that haven't been keyed up yet (catalog-less test
     * scenarios, freshly-seeded slots before world_init runs the
     * authority bootstrap) must not emit — their signatures would
     * verify against zero. */
    if (memcmp(s->station_pubkey, zero_pub, 32) == 0) return 0;
    /* If the secret slot is all-zero the keypair was never derived;
     * skip rather than emit a forgery-friendly all-zero signature. */
    static const uint8_t zero_secret[64] = {0};
    if (memcmp(s->station_secret, zero_secret, 64) == 0) return 0;

    chain_event_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    /* epoch = sim tick. world.time is in seconds at 120 Hz; we
     * round to ticks for stability across save/load reboots that
     * stamp world.time as a float. */
    uint64_t epoch_ticks = w ? (uint64_t)(w->time * 120.0) : 0;
    hdr.epoch = epoch_ticks;
    hdr.event_id = s->chain_event_count + 1;
    hdr.type = (uint8_t)type;
    memcpy(hdr.authority, s->station_pubkey, 32);
    sha256_bytes(payload_len > 0 ? payload : (const void *)"", payload_len, hdr.payload_hash);
    memcpy(hdr.prev_hash, s->chain_last_hash, 32);

    uint8_t unsigned_blob[CHAIN_UNSIGNED_HEADER_SIZE];
    chain_event_unsigned_pack(&hdr, unsigned_blob);
    station_sign(s, unsigned_blob, CHAIN_UNSIGNED_HEADER_SIZE, hdr.signature);

    /* Self-verify before persisting — paranoia, but cheap and catches
     * a corrupt key situation where rederive failed silently. */
    if (!station_verify(s, unsigned_blob, CHAIN_UNSIGNED_HEADER_SIZE, hdr.signature)) {
        SIM_LOG("[chain] self-verify failed for station; skipping emit\n");
        return 0;
    }

    /* Open the log in append mode; create dir on first emit. */
    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) {
        SIM_LOG("[chain] could not build log path\n");
        return 0;
    }
    ensure_chain_dir();
    FILE *f = fopen(path, "ab");
    if (!f) {
        SIM_LOG("[chain] fopen(%s) failed: %s\n", path, strerror(errno));
        return 0;
    }
    uint8_t packed[CHAIN_EVENT_HEADER_SIZE];
    chain_event_header_pack(&hdr, packed);
    if (fwrite(packed, CHAIN_EVENT_HEADER_SIZE, 1, f) != 1) {
        SIM_LOG("[chain] write header failed: %s\n", strerror(errno));
        fclose(f);
        return 0;
    }
    if (fwrite(&payload_len, sizeof(payload_len), 1, f) != 1) {
        SIM_LOG("[chain] write payload_len failed: %s\n", strerror(errno));
        fclose(f);
        return 0;
    }
    if (payload_len > 0 &&
        fwrite(payload, payload_len, 1, f) != 1) {
        SIM_LOG("[chain] write payload failed: %s\n", strerror(errno));
        fclose(f);
        return 0;
    }
    fflush(f);
    fclose(f);

    /* Update in-memory chain state — the next event's prev_hash. */
    chain_event_header_hash(&hdr, s->chain_last_hash);
    s->chain_event_count = hdr.event_id;
    return hdr.event_id;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

bool chain_log_verify(const station_t *s,
                      uint64_t *out_event_count,
                      uint8_t out_last_hash[32]) {
    static const uint8_t zero_pub[32] = {0};
    if (out_event_count) *out_event_count = 0;
    if (out_last_hash) memset(out_last_hash, 0, 32);
    if (!s) return false;
    if (memcmp(s->station_pubkey, zero_pub, 32) == 0) {
        /* Unkeyed station — log is trivially empty. */
        return true;
    }
    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* No log on disk = no events authored = trivially valid. */
        return true;
    }

    uint64_t count = 0;
    uint8_t prev_hash[32] = {0};
    uint8_t expected_id = 1; /* event_id starts at 1 */
    (void)expected_id;
    uint64_t expected_event_id = 1;
    bool ok = true;

    for (;;) {
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, CHAIN_EVENT_HEADER_SIZE, f);
        if (got == 0 && feof(f)) break;
        if (got != CHAIN_EVENT_HEADER_SIZE) { ok = false; break; }
        chain_event_header_t hdr;
        if (!chain_event_header_unpack(hdr_bytes, &hdr)) { ok = false; break; }

        uint16_t payload_len = 0;
        if (fread(&payload_len, sizeof(payload_len), 1, f) != 1) {
            ok = false; break;
        }
        uint8_t payload_buf[4096];
        if (payload_len > sizeof(payload_buf)) {
            /* Defensive: reject implausibly large payloads. */
            ok = false; break;
        }
        if (payload_len > 0 &&
            fread(payload_buf, payload_len, 1, f) != 1) {
            ok = false; break;
        }

        /* 1) Authority must match the station we're verifying. */
        if (memcmp(hdr.authority, s->station_pubkey, 32) != 0) {
            ok = false; break;
        }
        /* 2) prev_hash must chain. */
        if (memcmp(hdr.prev_hash, prev_hash, 32) != 0) {
            ok = false; break;
        }
        /* 3) event_id must be monotonic from 1. */
        if (hdr.event_id != expected_event_id) {
            ok = false; break;
        }
        /* 4) payload_hash must match the stored payload bytes. */
        uint8_t computed_payload_hash[32];
        sha256_bytes(payload_len > 0 ? payload_buf : (const void *)"",
                     payload_len, computed_payload_hash);
        if (memcmp(computed_payload_hash, hdr.payload_hash, 32) != 0) {
            ok = false; break;
        }
        /* 5) Signature must verify against authority pubkey over the
         *    unsigned header span. */
        uint8_t unsigned_blob[CHAIN_UNSIGNED_HEADER_SIZE];
        chain_event_unsigned_pack(&hdr, unsigned_blob);
        if (!signal_crypto_verify(hdr.signature, unsigned_blob,
                                  CHAIN_UNSIGNED_HEADER_SIZE,
                                  hdr.authority)) {
            ok = false; break;
        }

        chain_event_header_hash(&hdr, prev_hash);
        count++;
        expected_event_id++;
    }

    fclose(f);
    if (out_event_count) *out_event_count = count;
    if (out_last_hash) memcpy(out_last_hash, prev_hash, 32);
    return ok;
}

void chain_log_reset(const station_t *s) {
    static const uint8_t zero_pub[32] = {0};
    if (!s) return;
    if (memcmp(s->station_pubkey, zero_pub, 32) == 0) return;
    char path[256];
    if (!chain_log_path_for(s->station_pubkey, path, sizeof(path))) return;
    (void)remove(path);
}
