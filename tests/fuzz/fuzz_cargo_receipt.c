/*
 * fuzz_cargo_receipt.c -- libFuzzer harness for untrusted decode paths
 * in the cargo provenance stack (#479 Layer D + handoff tickets).
 *
 * These functions parse bytes that arrive over the wire from other
 * players and, in the federated future, from other authorities. They
 * must never crash, overflow, or read out of bounds on malformed input:
 *
 *   - cargo_receipt_unpack / _verify_signature / _chain_verify
 *   - ship_receipts_{push_chain,extend,remove} (receipt store ops)
 *   - handoff_ticket_unpack / _verify_hashes
 *   - handoff_ship_snapshot_unpack (variable-length, allocates)
 *
 * Build (clang only):  make fuzz-receipts
 * Or manually:
 *   cmake -S . -B build-fuzz -DBUILD_TESTS_ONLY=ON -DBUILD_TOOLS=OFF \
 *     -DSIGNAL_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang
 *   cmake --build build-fuzz
 *   ./build-fuzz/fuzz_cargo_receipt -max_total_time=300 corpus_dir
 *
 * A standalone replay mode (-DSIGNAL_FUZZ_STANDALONE) compiles a main()
 * that runs the harness over files given on argv, so crash artifacts can
 * be reproduced under plain ASan/UBSan without libFuzzer. Any crash fixed
 * here must land as a regression test per docs/c_safety_policy.md.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cargo_receipt.h"
#include "handoff_ticket.h"
#include "manifest.h" /* ship_cleanup */

static void fuzz_receipt_chain(const uint8_t *data, size_t size) {
    cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN];
    size_t nlinks = size / CARGO_RECEIPT_SIZE;
    if (nlinks > CARGO_RECEIPT_CHAIN_MAX_LEN)
        nlinks = CARGO_RECEIPT_CHAIN_MAX_LEN;

    size_t unpacked = 0;
    for (size_t i = 0; i < nlinks; i++) {
        if (cargo_receipt_unpack(data + i * CARGO_RECEIPT_SIZE, &chain[i]))
            unpacked++;
    }
    if (unpacked == 0) return;

    /* Signature verify must be total: garbage in, verdict out, no crash. */
    for (size_t i = 0; i < unpacked; i++)
        (void)cargo_receipt_verify_signature(&chain[i]);

    /* Chain walk with and without the cargo binding. */
    (void)cargo_receipt_chain_verify(chain, unpacked, NULL);
    uint8_t expected[32];
    memcpy(expected, chain[0].cargo_pub, sizeof(expected));
    (void)cargo_receipt_chain_verify(chain, unpacked, expected);

    /* Hash/pack round trip. */
    uint8_t h[32];
    for (size_t i = 0; i < unpacked; i++)
        cargo_receipt_hash(&chain[i], h);
}

static void fuzz_receipt_store(const uint8_t *data, size_t size) {
    ship_receipts_t store;
    if (!ship_receipts_init(&store, 4)) return;

    size_t off = 0;
    while (off < size) {
        uint8_t op = data[off++];
        switch (op % 3) {
        case 0: { /* push_chain: len byte, then len * 208 bytes */
            if (off >= size) break;
            uint8_t want = (uint8_t)(1 + data[off++] % 4);
            cargo_receipt_t links[4];
            uint8_t got = 0;
            for (uint8_t i = 0; i < want; i++) {
                if (off + CARGO_RECEIPT_SIZE > size) break;
                if (cargo_receipt_unpack(data + off, &links[got])) got++;
                off += CARGO_RECEIPT_SIZE;
            }
            if (got > 0) (void)ship_receipts_push_chain(&store, links, got);
            break;
        }
        case 1: { /* extend: index byte, then one receipt */
            if (off + 1 + CARGO_RECEIPT_SIZE > size) break;
            uint16_t idx = data[off++];
            cargo_receipt_t r;
            if (cargo_receipt_unpack(data + off, &r))
                (void)ship_receipts_extend(&store, idx, &r);
            off += CARGO_RECEIPT_SIZE;
            break;
        }
        default: { /* remove: index byte */
            if (off >= size) break;
            uint16_t idx = data[off++];
            (void)ship_receipts_remove(&store, idx, NULL);
            break;
        }
        }
    }
    ship_receipts_free(&store);
}

static void fuzz_handoff(const uint8_t *data, size_t size) {
    if (size >= HANDOFF_TICKET_SIZE) {
        handoff_ticket_t t;
        if (handoff_ticket_unpack(data, &t)) {
            uint8_t h[32];
            handoff_ticket_hash(&t, h);
            /* Self-referential expectations: we're probing the verifier's
             * robustness on garbage fields, not acceptance logic. */
            (void)handoff_ticket_verify_hashes(
                &t, t.issued_tick, t.source_authority, t.dest_authority,
                t.player_pubkey, t.ship_state_hash, t.cargo_root);
        }
        data += HANDOFF_TICKET_SIZE;
        size -= HANDOFF_TICKET_SIZE;
    }

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    size_t consumed = 0;
    if (handoff_ship_snapshot_unpack(data, size, &ship, &consumed))
        ship_cleanup(&ship);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1) return 0;
    uint8_t mode = (uint8_t)(data[0] % 3u);
    data++;
    size--;
    switch (mode) {
    case 0: fuzz_receipt_chain(data, size); break;
    case 1: fuzz_receipt_store(data, size); break;
    default: fuzz_handoff(data, size); break;
    }
    return 0;
}

#ifdef SIGNAL_FUZZ_STANDALONE
/* Replay files from argv through the harness (crash-triage mode). */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); continue; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long n = ftell(f);
        if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        uint8_t *buf = (uint8_t *)malloc((size_t)(n > 0 ? n : 1));
        if (!buf) { fclose(f); continue; }
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        if (got == (size_t)n)
            LLVMFuzzerTestOneInput(buf, (size_t)n);
        free(buf);
    }
    return 0;
}
#endif
