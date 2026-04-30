/*
 * gen_chain_fixture.c -- Regenerate the committed chain-log fixture
 * used by the CI verify-chain-logs gate (#479 Layer E).
 *
 * Not built by default — rebuild and run only when sim event semantics
 * intentionally change. The output is a small, deterministic chain
 * log covering all 6 event types, with deterministic station keys
 * (seed=0x42).
 *
 * Build:
 *   cc -std=c11 -I shared -I server -I vendor/tweetnacl \
 *      tools/gen_chain_fixture.c \
 *      server/chain_log_verify.c \
 *      vendor/tweetnacl/tweetnacl.c \
 *      vendor/tweetnacl/randombytes.c \
 *      vendor/tweetnacl/signal_crypto_tweetnacl.c \
 *      -o /tmp/gen_chain_fixture
 *
 * Run:
 *   /tmp/gen_chain_fixture tests/fixtures/chain_log_v41.log
 *   # then derive station pubkey:
 *   /tmp/gen_chain_fixture --pubkey
 *
 * Deliberately self-contained: doesn't pull in game_sim.h or any of
 * the live sim. Builds the chain_event_header_t bytes directly so the
 * fixture has zero dependency on sim drift.
 */

#include "chain_log.h"
#include "sha256.h"
#include "signal_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNSIGNED_HEADER_SIZE 120

static void pack_full(const chain_event_header_t *h, uint8_t out[CHAIN_EVENT_HEADER_SIZE]) {
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

static void hash_full(const chain_event_header_t *h, uint8_t out[32]) {
    uint8_t packed[CHAIN_EVENT_HEADER_SIZE];
    pack_full(h, packed);
    sha256_bytes(packed, CHAIN_EVENT_HEADER_SIZE, out);
}

int main(int argc, char **argv) {
    /* Deterministic station key: seed = repeating 0x42. */
    uint8_t seed[32];
    memset(seed, 0x42, sizeof(seed));
    uint8_t pub[32], secret[64];
    signal_crypto_keypair_from_seed(seed, pub, secret);

    if (argc >= 2 && strcmp(argv[1], "--pubkey") == 0) {
        for (int i = 0; i < 32; i++) printf("%02x", pub[i]);
        printf("\n");
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: gen_chain_fixture <out.log> | --pubkey\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror("fopen"); return 1; }

    /* Deterministic 48-event log (just under 10 KB at 202 bytes/entry).
     * Types rotate over all 6 known kinds so event_type_counts fully
     * populates. */
    uint8_t prev_hash[32] = {0};
    static const uint8_t types[] = {
        CHAIN_EVT_SMELT, CHAIN_EVT_CRAFT, CHAIN_EVT_TRANSFER,
        CHAIN_EVT_TRADE, CHAIN_EVT_LEDGER, CHAIN_EVT_ROCK_DESTROY,
    };

    for (int i = 0; i < 48; i++) {
        chain_event_header_t hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.epoch = 100 + (uint64_t)i * 7;
        hdr.event_id = (uint64_t)(i + 1);
        hdr.type = types[i % 6];
        memcpy(hdr.authority, pub, 32);

        uint8_t payload[16];
        for (int b = 0; b < 16; b++) payload[b] = (uint8_t)((i * 13 + b) & 0xFF);
        sha256_bytes(payload, sizeof(payload), hdr.payload_hash);
        memcpy(hdr.prev_hash, prev_hash, 32);

        uint8_t unsigned_blob[UNSIGNED_HEADER_SIZE];
        uint8_t full[CHAIN_EVENT_HEADER_SIZE];
        pack_full(&hdr, full);
        memcpy(unsigned_blob, full, UNSIGNED_HEADER_SIZE);
        signal_crypto_sign(hdr.signature, unsigned_blob, UNSIGNED_HEADER_SIZE, secret);

        uint8_t packed[CHAIN_EVENT_HEADER_SIZE];
        pack_full(&hdr, packed);
        if (fwrite(packed, CHAIN_EVENT_HEADER_SIZE, 1, f) != 1) { fclose(f); return 1; }
        uint16_t plen = (uint16_t)sizeof(payload);
        if (fwrite(&plen, sizeof(plen), 1, f) != 1) { fclose(f); return 1; }
        if (fwrite(payload, sizeof(payload), 1, f) != 1) { fclose(f); return 1; }

        hash_full(&hdr, prev_hash);
    }
    fclose(f);
    fprintf(stderr, "Wrote 48 events. Pubkey hex: ");
    for (int i = 0; i < 32; i++) fprintf(stderr, "%02x", pub[i]);
    fprintf(stderr, "\n");
    return 0;
}
