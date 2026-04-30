/*
 * chain_log_verify.c -- Standalone post-mortem chain-log verifier
 * (Layer E of #479).
 *
 * Lifts the per-event verification loop out of chain_log.c so it can
 * be linked into the `signal_verify` CLI without dragging the rest of
 * the simulation (world_t, SIM_LOG, station authority key derivation,
 * etc). The live-sim chain_log_verify wrapper in chain_log.c calls
 * this function too — single source of truth for "is this byte stream
 * a valid signed chain log for pubkey X".
 *
 * Verifies, per event, in order:
 *   1. authority field equals expected pubkey
 *   2. prev_hash links to SHA-256 of the previous event's full header
 *   3. event_id is monotonic from 1
 *   4. payload_hash equals SHA-256(payload bytes)
 *   5. Ed25519 signature over the 120-byte unsigned-header span
 */
#include "chain_log.h"

#include "sha256.h"
#include "signal_crypto.h"

#include <stdio.h>
#include <string.h>

#define CHAIN_UNSIGNED_HEADER_SIZE 120

/* Local copy of the on-disk header packer/unpacker. Kept in sync with
 * chain_log.c by the _Static_assert there on header size; if anyone
 * reorders the struct both copies must follow. The format is little-
 * endian for portability. */
static void verify_pack_full(const chain_event_header_t *h,
                             uint8_t out[CHAIN_EVENT_HEADER_SIZE]) {
    size_t off = 0;
    for (int i = 0; i < 8; i++) out[off + i] = (uint8_t)(h->epoch >> (i * 8));
    off += 8;
    for (int i = 0; i < 8; i++) out[off + i] = (uint8_t)(h->event_id >> (i * 8));
    off += 8;
    out[off++] = h->type;
    memset(&out[off], 0, 7);
    off += 7;
    memcpy(&out[off], h->authority, 32);    off += 32;
    memcpy(&out[off], h->payload_hash, 32); off += 32;
    memcpy(&out[off], h->prev_hash, 32);    off += 32;
    memcpy(&out[off], h->signature, 64);    off += 64;
    (void)off;
}

static bool verify_unpack(const uint8_t in[CHAIN_EVENT_HEADER_SIZE],
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
    for (int i = 0; i < 7; i++) {
        if (in[off + i] != 0) return false;
    }
    memset(out->_pad, 0, sizeof(out->_pad));
    off += 7;
    memcpy(out->authority,    &in[off], 32); off += 32;
    memcpy(out->payload_hash, &in[off], 32); off += 32;
    memcpy(out->prev_hash,    &in[off], 32); off += 32;
    memcpy(out->signature,    &in[off], 64); off += 64;
    (void)off;
    return true;
}

static void verify_pack_unsigned(const chain_event_header_t *h,
                                 uint8_t out[CHAIN_UNSIGNED_HEADER_SIZE]) {
    uint8_t full[CHAIN_EVENT_HEADER_SIZE];
    verify_pack_full(h, full);
    memcpy(out, full, CHAIN_UNSIGNED_HEADER_SIZE);
}

/* Public hash of the full header (also defined in chain_log.c). The
 * two definitions are linker-distinguished by the unique TU; we
 * provide our own in case the verify-only build doesn't link
 * chain_log.c. We declare a static alias so we don't double-define
 * chain_event_header_hash. The standalone tool DOES NOT link
 * chain_log.c, so this is the only definition there. */
static void verify_hash_full(const chain_event_header_t *h, uint8_t out[32]) {
    uint8_t packed[CHAIN_EVENT_HEADER_SIZE];
    verify_pack_full(h, packed);
    sha256_bytes(packed, CHAIN_EVENT_HEADER_SIZE, out);
}

bool chain_log_verify_with_pubkey(FILE *f,
                                  const uint8_t station_pubkey[32],
                                  chain_log_verify_report_t *out_report) {
    chain_log_verify_report_t local;
    chain_log_verify_report_t *r = out_report ? out_report : &local;
    memset(r, 0, sizeof(*r));
    if (!f || !station_pubkey) {
        snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                 "null FILE* or pubkey");
        return false;
    }

    uint8_t prev_hash[32] = {0};
    uint64_t expected_event_id = 1;
    bool ok = true;

    for (;;) {
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, CHAIN_EVENT_HEADER_SIZE, f);
        if (got == 0 && feof(f)) break;
        if (got != CHAIN_EVENT_HEADER_SIZE) {
            ok = false;
            r->first_fail_event_id = expected_event_id;
            snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                     "truncated header (read %zu of %d bytes)",
                     got, CHAIN_EVENT_HEADER_SIZE);
            break;
        }
        chain_event_header_t hdr;
        if (!verify_unpack(hdr_bytes, &hdr)) {
            ok = false;
            r->first_fail_event_id = expected_event_id;
            snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                     "header unpack failed (non-zero pad byte?)");
            break;
        }

        uint16_t payload_len = 0;
        if (fread(&payload_len, sizeof(payload_len), 1, f) != 1) {
            ok = false;
            r->first_fail_event_id = hdr.event_id;
            snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                     "missing payload_len after header");
            break;
        }
        uint8_t payload_buf[4096];
        if (payload_len > sizeof(payload_buf)) {
            ok = false;
            r->first_fail_event_id = hdr.event_id;
            snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                     "implausible payload_len=%u", (unsigned)payload_len);
            break;
        }
        if (payload_len > 0 &&
            fread(payload_buf, payload_len, 1, f) != 1) {
            ok = false;
            r->first_fail_event_id = hdr.event_id;
            snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                     "truncated payload (%u bytes)", (unsigned)payload_len);
            break;
        }

        r->total_events++;

        if (memcmp(hdr.authority, station_pubkey, 32) != 0) {
            r->bad_authority++;
            ok = false;
            if (r->first_fail_event_id == 0) {
                r->first_fail_event_id = hdr.event_id;
                snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                         "authority pubkey mismatch at event %llu",
                         (unsigned long long)hdr.event_id);
            }
            break;
        }
        if (memcmp(hdr.prev_hash, prev_hash, 32) != 0) {
            r->bad_linkage++;
            ok = false;
            if (r->first_fail_event_id == 0) {
                r->first_fail_event_id = hdr.event_id;
                snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                         "prev_hash linkage broken at event %llu",
                         (unsigned long long)hdr.event_id);
            }
            break;
        }
        if (hdr.event_id != expected_event_id) {
            r->monotonic_violations++;
            ok = false;
            if (r->first_fail_event_id == 0) {
                r->first_fail_event_id = hdr.event_id;
                snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                         "event_id non-monotonic: got %llu, expected %llu",
                         (unsigned long long)hdr.event_id,
                         (unsigned long long)expected_event_id);
            }
            break;
        }
        uint8_t computed_payload_hash[32];
        sha256_bytes(payload_len > 0 ? payload_buf : (const void *)"",
                     payload_len, computed_payload_hash);
        if (memcmp(computed_payload_hash, hdr.payload_hash, 32) != 0) {
            r->bad_payload_hash++;
            ok = false;
            if (r->first_fail_event_id == 0) {
                r->first_fail_event_id = hdr.event_id;
                snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                         "payload_hash mismatch at event %llu",
                         (unsigned long long)hdr.event_id);
            }
            break;
        }
        uint8_t unsigned_blob[CHAIN_UNSIGNED_HEADER_SIZE];
        verify_pack_unsigned(&hdr, unsigned_blob);
        if (!signal_crypto_verify(hdr.signature, unsigned_blob,
                                  CHAIN_UNSIGNED_HEADER_SIZE,
                                  hdr.authority)) {
            r->bad_signatures++;
            ok = false;
            if (r->first_fail_event_id == 0) {
                r->first_fail_event_id = hdr.event_id;
                snprintf(r->first_fail_reason, sizeof(r->first_fail_reason),
                         "signature verify failed at event %llu",
                         (unsigned long long)hdr.event_id);
            }
            break;
        }

        if (hdr.type < CHAIN_EVT_TYPE_COUNT)
            r->event_type_counts[hdr.type]++;

        verify_hash_full(&hdr, prev_hash);
        r->valid_events++;
        expected_event_id++;
    }

    return ok;
}
