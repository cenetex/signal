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
 * SIGNAL_FUZZ_MODE=receipt-chain|receipt-store|handoff forces one decoder
 * mode for bounded per-mode exploration. Mode-specific crash artifacts use
 * the same names as prefixes so standalone replay can infer the mode.
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

enum {
    FUZZ_MODE_RECEIPT_CHAIN = 0,
    FUZZ_MODE_RECEIPT_STORE = 1,
    FUZZ_MODE_HANDOFF = 2,
    FUZZ_MODE_DISPATCH = -1,
    FUZZ_MODE_UNINITIALIZED = -2,
};

static int fuzz_parse_mode(const char *value) {
    if (!value || value[0] == '\0') return FUZZ_MODE_DISPATCH;
    if (strcmp(value, "receipt-chain") == 0 || strcmp(value, "0") == 0)
        return FUZZ_MODE_RECEIPT_CHAIN;
    if (strcmp(value, "receipt-store") == 0 || strcmp(value, "1") == 0)
        return FUZZ_MODE_RECEIPT_STORE;
    if (strcmp(value, "handoff") == 0 || strcmp(value, "2") == 0)
        return FUZZ_MODE_HANDOFF;
    fprintf(stderr,
            "invalid SIGNAL_FUZZ_MODE=%s "
            "(use receipt-chain, receipt-store, or handoff)\n",
            value);
    exit(2);
}

static int fuzz_environment_mode(void) {
    static int mode = FUZZ_MODE_UNINITIALIZED;
    if (mode == FUZZ_MODE_UNINITIALIZED)
        mode = fuzz_parse_mode(getenv("SIGNAL_FUZZ_MODE"));
    return mode;
}

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

static int fuzz_one_input(const uint8_t *data, size_t size,
                          int forced_mode) {
    int mode = forced_mode;
    if (mode == FUZZ_MODE_DISPATCH) {
        if (size < 1) return FUZZ_MODE_DISPATCH;
        mode = (int)(data[0] % 3u);
        data++;
        size--;
    }
    switch (mode) {
    case FUZZ_MODE_RECEIPT_CHAIN:
        fuzz_receipt_chain(data, size);
        break;
    case FUZZ_MODE_RECEIPT_STORE:
        fuzz_receipt_store(data, size);
        break;
    default: fuzz_handoff(data, size); break;
    }
    return mode;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)fuzz_one_input(data, size, fuzz_environment_mode());
    return 0;
}

#ifdef SIGNAL_FUZZ_STANDALONE
static int fuzz_artifact_mode(const char *path) {
    if (!path) return FUZZ_MODE_DISPATCH;
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!base || (backslash && backslash > base)) base = backslash;
#endif
    base = base ? base + 1 : path;
    static const char *names[] = {
        "receipt-chain-",
        "receipt-store-",
        "handoff-",
    };
    for (int mode = 0; mode < 3; mode++) {
        size_t prefix_len = strlen(names[mode]);
        if (strncmp(base, names[mode], prefix_len) == 0)
            return mode;
    }
    return FUZZ_MODE_DISPATCH;
}

/* Replay files from argv through the harness (crash-triage mode). */
int main(int argc, char **argv) {
    size_t replayed[3] = {0, 0, 0};
    size_t skipped = 0;
    int environment_mode = fuzz_environment_mode();
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) {
            perror(argv[i]);
            skipped++;
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            skipped++;
            continue;
        }
        long n = ftell(f);
        if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            skipped++;
            continue;
        }
        uint8_t *buf = (uint8_t *)malloc((size_t)(n > 0 ? n : 1));
        if (!buf) {
            fclose(f);
            skipped++;
            continue;
        }
        size_t got = fread(buf, 1, (size_t)n, f);
        fclose(f);
        if (got == (size_t)n) {
            int mode = environment_mode;
            if (mode == FUZZ_MODE_DISPATCH)
                mode = fuzz_artifact_mode(argv[i]);
            mode = fuzz_one_input(buf, (size_t)n, mode);
            if (mode >= 0 && mode < 3)
                replayed[mode]++;
            else
                skipped++;
        } else {
            skipped++;
        }
        free(buf);
    }
    fprintf(stderr,
            "replayed receipt-chain=%zu receipt-store=%zu "
            "handoff=%zu skipped=%zu\n",
            replayed[FUZZ_MODE_RECEIPT_CHAIN],
            replayed[FUZZ_MODE_RECEIPT_STORE],
            replayed[FUZZ_MODE_HANDOFF],
            skipped);
    return skipped == 0 ? 0 : 1;
}
#endif
