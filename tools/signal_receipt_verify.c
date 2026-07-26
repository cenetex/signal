/*
 * signal_receipt_verify -- semantic verifier for portable cargo receipts.
 *
 * Input is a raw concatenation of canonical 208-byte cargo_receipt_t records
 * in chronological order. Origin facts and authority policy are explicit CLI
 * inputs so the tool cannot imply that a self-consistent signature chain also
 * proves production origin or local issuer trust.
 */

#include "base58.h"
#include "cargo_receipt.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { REPORT_TEXT, REPORT_JSON } report_format_t;

typedef struct {
    const char *chain_path;
    report_format_t report;
    bool cargo_set;
    uint8_t cargo_pub[32];
    bool origin_event_set;
    cargo_receipt_origin_event_t origin_event;
    bool origin_hash_set;
    uint8_t origin_hash[32];
    bool origin_authority_set;
    uint8_t origin_authority[32];
    uint64_t origin_event_id;
    uint64_t origin_epoch;
    bool authority_trust_set;
    cargo_receipt_authority_trust_t authority_trust;
} cli_opts_t;

static void usage(FILE *out) {
    fprintf(out,
        "usage: signal_receipt_verify [options] <receipt-chain.bin>\n"
        "\n"
        "Required:\n"
        "  --cargo-pub=<hex|base58>\n"
        "  --origin-event=<smelt|craft|missing>\n"
        "  --authority-trust=<current|rotated|unknown|untrusted|revoked>\n"
        "\n"
        "Required unless origin-event=missing:\n"
        "  --origin-hash=<hex>          SHA-256 of producing event header\n"
        "  --origin-authority=<hex|base58>\n"
        "\n"
        "Optional:\n"
        "  --origin-event-id=<n>        Audit detail (default 0)\n"
        "  --origin-epoch=<n>           Audit detail (default 0)\n"
        "  --report=<text|json>         Default text\n"
        "  -h, --help\n"
        "\n"
        "Exit: 0 accepted, 1 rejected, 2 malformed input.\n");
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool parse_hex32(const char *text, uint8_t out[32]) {
    if (!text || strlen(text) != 64u) return false;
    for (size_t i = 0; i < 32u; i++) {
        int hi = hex_nibble(text[i * 2u]);
        int lo = hex_nibble(text[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool parse_pub(const char *text, uint8_t out[32]) {
    return parse_hex32(text, out) || base58_decode(text, out, 32) == 32;
}

static bool parse_u64(const char *text, uint64_t *out) {
    if (!text || !*text || !out) return false;
    uint64_t value = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        uint64_t digit = (uint64_t)(*p - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    *out = value;
    return true;
}

static bool parse_origin_event(const char *text,
                               cargo_receipt_origin_event_t *out) {
    if (strcmp(text, "missing") == 0) {
        *out = CARGO_RECEIPT_ORIGIN_EVENT_NONE;
        return true;
    }
    if (strcmp(text, "smelt") == 0) {
        *out = CARGO_RECEIPT_ORIGIN_EVENT_SMELT;
        return true;
    }
    if (strcmp(text, "craft") == 0) {
        *out = CARGO_RECEIPT_ORIGIN_EVENT_CRAFT;
        return true;
    }
    return false;
}

static bool parse_authority_trust(
    const char *text, cargo_receipt_authority_trust_t *out) {
    if (strcmp(text, "current") == 0) {
        *out = CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT;
        return true;
    }
    if (strcmp(text, "rotated") == 0) {
        *out = CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED;
        return true;
    }
    if (strcmp(text, "unknown") == 0) {
        *out = CARGO_RECEIPT_AUTHORITY_UNKNOWN;
        return true;
    }
    if (strcmp(text, "untrusted") == 0) {
        *out = CARGO_RECEIPT_AUTHORITY_UNTRUSTED;
        return true;
    }
    if (strcmp(text, "revoked") == 0) {
        *out = CARGO_RECEIPT_AUTHORITY_REVOKED;
        return true;
    }
    return false;
}

static bool parse_args(int argc, char **argv, cli_opts_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->report = REPORT_TEXT;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strncmp(arg, "--cargo-pub=", 12) == 0) {
            opts->cargo_set = parse_pub(arg + 12, opts->cargo_pub);
            if (!opts->cargo_set) return false;
        } else if (strncmp(arg, "--origin-event=", 15) == 0) {
            opts->origin_event_set =
                parse_origin_event(arg + 15, &opts->origin_event);
            if (!opts->origin_event_set) return false;
        } else if (strncmp(arg, "--origin-hash=", 14) == 0) {
            opts->origin_hash_set =
                parse_hex32(arg + 14, opts->origin_hash);
            if (!opts->origin_hash_set) return false;
        } else if (strncmp(arg, "--origin-authority=", 19) == 0) {
            opts->origin_authority_set =
                parse_pub(arg + 19, opts->origin_authority);
            if (!opts->origin_authority_set) return false;
        } else if (strncmp(arg, "--origin-event-id=", 18) == 0) {
            if (!parse_u64(arg + 18, &opts->origin_event_id)) return false;
        } else if (strncmp(arg, "--origin-epoch=", 15) == 0) {
            if (!parse_u64(arg + 15, &opts->origin_epoch)) return false;
        } else if (strncmp(arg, "--authority-trust=", 18) == 0) {
            opts->authority_trust_set =
                parse_authority_trust(arg + 18, &opts->authority_trust);
            if (!opts->authority_trust_set) return false;
        } else if (strcmp(arg, "--report=json") == 0) {
            opts->report = REPORT_JSON;
        } else if (strcmp(arg, "--report=text") == 0) {
            opts->report = REPORT_TEXT;
        } else if (arg[0] == '-') {
            return false;
        } else if (!opts->chain_path) {
            opts->chain_path = arg;
        } else {
            return false;
        }
    }
    if (!opts->chain_path || !opts->cargo_set ||
        !opts->origin_event_set || !opts->authority_trust_set)
        return false;
    if (opts->origin_event != CARGO_RECEIPT_ORIGIN_EVENT_NONE &&
        (!opts->origin_hash_set || !opts->origin_authority_set))
        return false;
    return true;
}

static bool load_chain(const char *path,
                       cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN],
                       size_t *count_out) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t count = 0;
    for (;;) {
        uint8_t packed[CARGO_RECEIPT_SIZE];
        size_t got = fread(packed, 1, sizeof(packed), file);
        if (got == 0 && feof(file)) break;
        if (got != sizeof(packed) ||
            count >= CARGO_RECEIPT_CHAIN_MAX_LEN ||
            !cargo_receipt_unpack(packed, &chain[count])) {
            fclose(file);
            return false;
        }
        count++;
    }
    fclose(file);
    *count_out = count;
    return true;
}

static void hex32(const uint8_t bytes[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2u] = digits[bytes[i] >> 4];
        out[i * 2u + 1u] = digits[bytes[i] & 0x0fu];
    }
    out[64] = '\0';
}

static const char *chain_result_name(cargo_receipt_result_t result) {
    switch (result) {
    case CARGO_RECEIPT_OK: return "ok";
    case CARGO_RECEIPT_REJECT_EMPTY: return "reject_empty";
    case CARGO_RECEIPT_REJECT_TOO_LONG: return "reject_too_long";
    case CARGO_RECEIPT_REJECT_BAD_SIGNATURE: return "reject_bad_signature";
    case CARGO_RECEIPT_REJECT_BROKEN_LINKAGE: return "reject_broken_linkage";
    case CARGO_RECEIPT_REJECT_CARGO_MISMATCH: return "reject_cargo_mismatch";
    case CARGO_RECEIPT_REJECT_ZERO_AUTHORITY: return "reject_zero_authority";
    case CARGO_RECEIPT_REJECT_ZERO_ORIGIN: return "reject_zero_origin";
    default: return "unknown";
    }
}

static const char *origin_event_name(cargo_receipt_origin_event_t event) {
    switch (event) {
    case CARGO_RECEIPT_ORIGIN_EVENT_SMELT: return "smelt";
    case CARGO_RECEIPT_ORIGIN_EVENT_CRAFT: return "craft";
    default: return "missing";
    }
}

static const char *authority_trust_name(
    cargo_receipt_authority_trust_t trust) {
    switch (trust) {
    case CARGO_RECEIPT_AUTHORITY_TRUSTED_CURRENT: return "current";
    case CARGO_RECEIPT_AUTHORITY_TRUSTED_ROTATED: return "rotated";
    case CARGO_RECEIPT_AUTHORITY_UNKNOWN: return "unknown";
    case CARGO_RECEIPT_AUTHORITY_UNTRUSTED: return "untrusted";
    case CARGO_RECEIPT_AUTHORITY_REVOKED: return "revoked";
    default: return "invalid";
    }
}

static bool trust_accepted(cargo_receipt_trust_status_t status) {
    return status == CARGO_RECEIPT_TRUST_VALID_TRUSTED ||
           status == CARGO_RECEIPT_TRUST_VALID_TRUSTED_ROTATED;
}

static void print_json(const cli_opts_t *opts,
                       const cargo_receipt_t *chain,
                       size_t count,
                       const cargo_receipt_trust_result_t *result) {
    char cargo[65], origin_hash[65] = {0}, origin_authority[65] = {0};
    char first_authority[65] = {0}, head_hash[65] = {0};
    hex32(opts->cargo_pub, cargo);
    if (opts->origin_hash_set) hex32(opts->origin_hash, origin_hash);
    if (opts->origin_authority_set)
        hex32(opts->origin_authority, origin_authority);
    if (count > 0) {
        uint8_t hash[32];
        hex32(chain[0].authoring_station, first_authority);
        cargo_receipt_hash(&chain[count - 1u], hash);
        hex32(hash, head_hash);
    }
    bool accepted = trust_accepted(result->status);
    printf("{\n");
    printf("  \"schema\":\"signal.cargo_receipt_trust.v1\",\n");
    printf("  \"accepted\":%s,\n", accepted ? "true" : "false");
    printf("  \"verdict\":{\"status_code\":%u,\"status\":\"%s\","
           "\"semantic\":\"%s\",\"chain_code\":%u,\"chain\":\"%s\","
           "\"origin_event_code\":%u,\"origin_event\":\"%s\","
           "\"authority_trust_code\":%u,\"authority_trust\":\"%s\"},\n",
           (unsigned)result->status,
           cargo_receipt_trust_status_name(result->status),
           cargo_receipt_trust_semantic_label(result->status, accepted),
           (unsigned)result->chain_result,
           chain_result_name(result->chain_result),
           (unsigned)result->origin_event,
           origin_event_name(result->origin_event),
           (unsigned)result->authority_trust,
           authority_trust_name(result->authority_trust));
    printf("  \"audit\":{\"cargo_pub\":\"%s\",\"receipt_count\":%zu,",
           cargo, count);
    if (opts->origin_event == CARGO_RECEIPT_ORIGIN_EVENT_NONE) {
        printf("\"origin_hash\":null,\"origin_authority\":null,");
    } else {
        printf("\"origin_hash\":\"%s\",\"origin_authority\":\"%s\",",
               origin_hash, origin_authority);
    }
    printf("\"origin_event_id\":%llu,\"origin_epoch\":%llu,",
           (unsigned long long)opts->origin_event_id,
           (unsigned long long)opts->origin_epoch);
    if (count > 0)
        printf("\"first_receipt_authority\":\"%s\","
               "\"receipt_head_hash\":\"%s\"}\n",
               first_authority, head_hash);
    else
        printf("\"first_receipt_authority\":null,"
               "\"receipt_head_hash\":null}\n");
    printf("}\n");
}

static void print_text(const cli_opts_t *opts,
                       size_t count,
                       const cargo_receipt_trust_result_t *result) {
    bool accepted = trust_accepted(result->status);
    printf("Receipt: %s\n",
           cargo_receipt_trust_semantic_label(result->status, accepted));
    printf("Origin: %s (%s)\n",
           origin_event_name(result->origin_event),
           result->status == CARGO_RECEIPT_TRUST_REJECT_MISSING_ORIGIN
               ? "proof missing" : "proof evaluated");
    printf("Seal: %s\n", authority_trust_name(result->authority_trust));
    printf("Witnesses: %zu signed receipt link%s\n",
           count, count == 1 ? "" : "s");
    printf("Audit: status=%u:%s chain=%u:%s event_id=%llu epoch=%llu\n",
           (unsigned)result->status,
           cargo_receipt_trust_status_name(result->status),
           (unsigned)result->chain_result,
           chain_result_name(result->chain_result),
           (unsigned long long)opts->origin_event_id,
           (unsigned long long)opts->origin_epoch);
}

int main(int argc, char **argv) {
    cli_opts_t opts;
    if (!parse_args(argc, argv, &opts)) {
        usage(stderr);
        return 2;
    }

    cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN];
    size_t count = 0;
    if (!load_chain(opts.chain_path, chain, &count)) {
        fprintf(stderr, "signal_receipt_verify: malformed receipt chain\n");
        return 2;
    }

    cargo_receipt_origin_proof_t origin = {
        .event_type = opts.origin_event,
        .event_id = opts.origin_event_id,
        .epoch = opts.origin_epoch,
    };
    memcpy(origin.event_hash, opts.origin_hash, 32);
    memcpy(origin.output_cargo_pub, opts.cargo_pub, 32);
    memcpy(origin.authority, opts.origin_authority, 32);
    cargo_receipt_trust_result_t result = cargo_receipt_trust_verify(
        chain, count, opts.cargo_pub,
        opts.origin_event == CARGO_RECEIPT_ORIGIN_EVENT_NONE ? NULL : &origin,
        opts.authority_trust);

    if (opts.report == REPORT_JSON)
        print_json(&opts, chain, count, &result);
    else
        print_text(&opts, count, &result);
    return trust_accepted(result.status) ? 0 : 1;
}
