/*
 * signal_checkpoint -- compute a Signalspace checkpoint root over station
 * chain logs (see shared/signal_checkpoint.h).
 *
 *   signal_checkpoint [--prev=<64 hex>] chain/<station>.log ...
 *
 * Each log is verified in full (signatures, linkage, payload hashes,
 * authority). The leaf commits to the head hash and a SHA-256 of exactly the
 * bytes that verification read, so no file can change between being checked
 * and being committed. One failing log fails the whole checkpoint: a
 * checkpoint never covers history that did not verify.
 *
 * Output is deterministic JSON (no clocks), so anyone holding the same logs
 * recomputes the same root. Each station carries its inclusion proof.
 *
 * Exit: 0 ok, 1 a log failed verification, 2 usage or input error.
 */

#include "chain_log.h"

#include "base58.h"
#include "signal_checkpoint.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECKPOINT_MAX_LOGS 4096

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_checkpoint [--prev=<64 hex>] <chain-log-path>...\n"
        "\n"
        "Verifies every station chain log and prints a signal_checkpoint_v1\n"
        "JSON object. The station pubkey comes from each log's filename.\n"
        "--prev chains this checkpoint to the previous checkpoint root\n"
        "(default: all zero for the first checkpoint).\n"
        "\n"
        "Exit: 0 ok, 1 a log failed verification, 2 usage or input error.\n");
}

static bool parse_hex32(const char *text, uint8_t out[32]) {
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            char c = text[2 * i + j];
            unsigned nibble;
            if (c >= '0' && c <= '9') nibble = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') nibble = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') nibble = (unsigned)(c - 'A' + 10);
            else return false;
            value = value * 16u + nibble;
        }
        out[i] = (uint8_t)value;
    }
    return true;
}

static void print_hex(FILE *out, const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) fprintf(out, "%02x", bytes[i]);
}

static bool pubkey_from_filename(const char *path, uint8_t out[32]) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    char stem[80] = {0};
    size_t n = 0;
    while (base[n] && base[n] != '.' && n + 1u < sizeof(stem)) {
        stem[n] = base[n];
        n++;
    }
    stem[n] = '\0';
    return n > 0 && n < 64u && base58_decode(stem, out, 32) == 32;
}

static int compare_stations(const void *a, const void *b) {
    return memcmp(((const signal_checkpoint_station_t *)a)->station_pubkey,
                  ((const signal_checkpoint_station_t *)b)->station_pubkey, 32);
}

/* 0 ok, 1 verification failure, 2 input error. */
static int load_station(const char *path, signal_checkpoint_station_t *out) {
    memset(out, 0, sizeof(*out));
    if (!pubkey_from_filename(path, out->station_pubkey)) {
        fprintf(stderr, "signal_checkpoint: cannot read a station pubkey from %s\n", path);
        return 2;
    }
    FILE *log = fopen(path, "rb");
    if (!log) {
        fprintf(stderr, "signal_checkpoint: cannot open %s\n", path);
        return 2;
    }
    chain_log_verify_report_t report;
    bool verified = chain_log_verify_with_pubkey(log, out->station_pubkey, &report);
    bool read_error = ferror(log) != 0;
    if (fclose(log) != 0) read_error = true;
    if (read_error) {
        fprintf(stderr, "signal_checkpoint: read error on %s\n", path);
        return 2;
    }
    if (!verified || report.valid_events != report.total_events) {
        fprintf(stderr, "signal_checkpoint: %s failed verification: %s\n", path,
                report.first_fail_reason[0] ? report.first_fail_reason : "unknown");
        return 1;
    }
    out->total_events = report.valid_events;
    out->segment_count = report.segment_count;
    out->tail_event_id = report.tail_event_id;
    memcpy(out->head_hash, report.tail_hash, 32);
    memcpy(out->log_sha256, report.valid_bytes_sha256, 32);
    out->log_bytes = report.valid_bytes;
    return 0;
}

int main(int argc, char **argv) {
    uint8_t prev_root[32] = {0};
    const char *paths[CHECKPOINT_MAX_LOGS];
    size_t count = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strncmp(argv[i], "--prev=", 7) == 0) {
            if (!parse_hex32(argv[i] + 7, prev_root)) {
                fprintf(stderr, "signal_checkpoint: --prev needs 64 hex characters\n");
                return 2;
            }
            continue;
        }
        if (argv[i][0] == '-') {
            print_usage(stderr);
            return 2;
        }
        if (count == CHECKPOINT_MAX_LOGS) {
            fprintf(stderr, "signal_checkpoint: more than %d logs\n", CHECKPOINT_MAX_LOGS);
            return 2;
        }
        paths[count++] = argv[i];
    }
    if (count == 0) {
        print_usage(stderr);
        return 2;
    }

    signal_checkpoint_station_t *stations = calloc(count, sizeof(*stations));
    uint8_t (*scratch)[32] = calloc(count, 32);
    signal_checkpoint_proof_step_t proof[SIGNAL_CHECKPOINT_PROOF_MAX];
    int status = 0;
    if (!stations || !scratch) {
        fprintf(stderr, "signal_checkpoint: out of memory\n");
        status = 2;
        goto done;
    }
    for (size_t i = 0; i < count; i++) {
        int loaded = load_station(paths[i], &stations[i]);
        if (loaded != 0) {
            status = loaded;
            goto done;
        }
    }
    qsort(stations, count, sizeof(*stations), compare_stations);
    if (!signal_checkpoint_ordered(stations, count)) {
        fprintf(stderr, "signal_checkpoint: the same station log was given twice\n");
        status = 2;
        goto done;
    }

    uint8_t stations_root[32];
    uint8_t root[32];
    if (!signal_checkpoint_root(stations, count, prev_root, scratch, stations_root, root)) {
        fprintf(stderr, "signal_checkpoint: cannot compute the root\n");
        status = 2;
        goto done;
    }

    printf("{\"version\":\"signal_checkpoint_v%d\",\"checkpoint_root\":\"",
           SIGNAL_CHECKPOINT_VERSION);
    print_hex(stdout, root, 32);
    printf("\",\"prev_checkpoint_root\":\"");
    print_hex(stdout, prev_root, 32);
    printf("\",\"stations_root\":\"");
    print_hex(stdout, stations_root, 32);
    printf("\",\"station_count\":%zu,\"stations\":[", count);
    for (size_t i = 0; i < count; i++) {
        const signal_checkpoint_station_t *s = &stations[i];
        char b58[64] = {0};
        uint8_t leaf[32];
        size_t proof_len = 0;
        base58_encode(s->station_pubkey, 32, b58, sizeof(b58));
        signal_checkpoint_leaf(s, leaf);
        if (!signal_checkpoint_proof(stations, count, i, scratch, proof, &proof_len)) {
            fprintf(stderr, "signal_checkpoint: cannot build a proof\n");
            status = 2;
            goto done;
        }
        printf("%s{\"station_pubkey\":\"%s\",\"total_events\":%llu,"
               "\"segment_count\":%llu,\"tail_event_id\":%llu,\"head_hash\":\"",
               i ? "," : "", b58, (unsigned long long)s->total_events,
               (unsigned long long)s->segment_count,
               (unsigned long long)s->tail_event_id);
        print_hex(stdout, s->head_hash, 32);
        printf("\",\"log_sha256\":\"");
        print_hex(stdout, s->log_sha256, 32);
        printf("\",\"log_bytes\":%llu,\"leaf\":\"", (unsigned long long)s->log_bytes);
        print_hex(stdout, leaf, 32);
        printf("\",\"proof\":[");
        for (size_t p = 0; p < proof_len; p++) {
            printf("%s{\"side\":\"%s\",\"hash\":\"", p ? "," : "",
                   proof[p].sibling_on_left ? "left" : "right");
            print_hex(stdout, proof[p].sibling, 32);
            printf("\"}");
        }
        printf("]}");
    }
    printf("]}\n");
    if (fflush(stdout) != 0) status = 2;

done:
    free(stations);
    free(scratch);
    return status;
}
