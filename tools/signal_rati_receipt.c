/*
 * signal_rati_receipt -- off-chain RATi mining receipt builder.
 *
 * Reads a verified station chain log and emits deterministic JSON receipt
 * records for CHAIN_EVT_SMELT events. This is wedge 1 for the
 * Bitcoin/Arweave RATi anchor flow: local proof material first, external
 * anchoring later.
 */

#include "chain_log.h"

#include "base58.h"
#include "mining.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAIN_UNSIGNED_HEADER_SIZE 120

typedef struct {
    const char *path;
    const char *station_pubkey_b58_override;
    bool event_id_set;
    uint64_t event_id;
    bool segment_id_set;
    uint64_t segment_id;
    bool cargo_pub_set;
    uint8_t cargo_pub[32];
    uint8_t min_prefix;
} cli_opts_t;

typedef struct {
    uint64_t epoch;
    uint64_t event_id;
    uint8_t type;
    uint8_t authority[32];
    uint8_t payload_hash[32];
    uint8_t prev_hash[32];
    uint8_t signature[64];
    uint8_t raw_header[CHAIN_EVENT_HEADER_SIZE];
} parsed_header_t;

typedef struct {
    uint32_t world_id;
    uint32_t world_seq;
    char build_id[32];
} receipt_context_t;

typedef struct {
    uint64_t segment_id;
    parsed_header_t hdr;
    uint8_t event_hash[32];
    uint8_t fracture_seed[32];
    uint8_t fragment_pub[32];
    uint8_t claimant_pubkey[32];
    uint32_t fracture_id;
    uint32_t burst_nonce;
    uint16_t burst_cap;
    uint8_t grade;
    uint8_t asteroid_slot;
    bool fragment_verified;
    bool grade_verified;
    uint8_t computed_fragment_pub[32];
    uint8_t computed_grade;
    char callsign[8];
} claim_record_t;

typedef struct {
    uint64_t segment_id;
    parsed_header_t hdr;
    uint8_t event_hash[32];
    uint8_t receipt_hash[32];
    uint8_t fragment_pub[32];
    uint8_t ingot_pub[32];
    uint8_t prefix_class;
    uint64_t mined_block;
    receipt_context_t context;
    bool has_claim;
    claim_record_t claim;
} receipt_t;

typedef struct {
    receipt_t *items;
    size_t count;
    size_t cap;
} receipt_list_t;

typedef struct {
    claim_record_t *items;
    size_t count;
    size_t cap;
} claim_list_t;

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_rati_receipt [options] <chain-log-path>\n"
        "\n"
        "Options:\n"
        "  --station-pubkey=<base58>   Override pubkey (default: from filename)\n"
        "  --event-id=<n>              Emit only matching event_id\n"
        "  --segment-id=<n>            Emit only matching segment id (default first segment = 0)\n"
        "  --cargo-pub=<hex|base58>    Emit only matching smelted cargo/ingot pub\n"
        "  --min-prefix=<class>        Minimum ingot prefix class (default: anonymous)\n"
        "                              classes: anonymous, named, M, H, T, S, F, K,\n"
        "                              RATi, commissioned\n"
        "  -h, --help                  This message\n"
        "\n"
        "JSON output schema: signal.rati_mining_receipts.v1\n");
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static void hash_u16_le(sha256_ctx_t *ctx, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    sha256_update(ctx, b, sizeof(b));
}

static void hash_u32_le(sha256_ctx_t *ctx, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)v, (uint8_t)(v >> 8),
        (uint8_t)(v >> 16), (uint8_t)(v >> 24)
    };
    sha256_update(ctx, b, sizeof(b));
}

static void hash_u64_le(sha256_ctx_t *ctx, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (i * 8));
    sha256_update(ctx, b, sizeof(b));
}

static void hex_bytes(const uint8_t *in, size_t len, char *out, size_t cap) {
    static const char h[] = "0123456789abcdef";
    if (!out || cap == 0) return;
    if (!in || cap < len * 2u + 1u) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2u] = h[in[i] >> 4];
        out[i * 2u + 1u] = h[in[i] & 0x0F];
    }
    out[len * 2u] = '\0';
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

static bool parse_pub_arg(const char *text, uint8_t out[32]) {
    if (parse_hex32(text, out)) return true;
    return base58_decode(text, out, 32) == 32;
}

static bool parse_u64_arg(const char *text, uint64_t *out) {
    if (!text || !*text || !out) return false;
    uint64_t v = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        uint64_t d = (uint64_t)(*p - '0');
        if (v > (UINT64_MAX - d) / 10u) return false;
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

static const char *prefix_name(uint8_t prefix) {
    switch (prefix) {
    case INGOT_PREFIX_ANONYMOUS: return "anonymous";
    case INGOT_PREFIX_M: return "M";
    case INGOT_PREFIX_H: return "H";
    case INGOT_PREFIX_T: return "T";
    case INGOT_PREFIX_S: return "S";
    case INGOT_PREFIX_F: return "F";
    case INGOT_PREFIX_K: return "K";
    case INGOT_PREFIX_RATI: return "RATi";
    case INGOT_PREFIX_COMMISSIONED: return "commissioned";
    default: return "unknown";
    }
}

static const char *grade_name(uint8_t grade) {
    switch ((mining_grade_t)grade) {
    case MINING_GRADE_COMMON: return "common";
    case MINING_GRADE_FINE: return "fine";
    case MINING_GRADE_RARE: return "rare";
    case MINING_GRADE_RATI: return "RATi";
    case MINING_GRADE_COMMISSIONED: return "commissioned";
    default: return "unknown";
    }
}

static bool parse_prefix_class(const char *text, uint8_t *out) {
    if (!text || !out) return false;
    if (strcmp(text, "anonymous") == 0) { *out = INGOT_PREFIX_ANONYMOUS; return true; }
    if (strcmp(text, "named") == 0) { *out = INGOT_PREFIX_M; return true; }
    if (strcmp(text, "M") == 0) { *out = INGOT_PREFIX_M; return true; }
    if (strcmp(text, "H") == 0) { *out = INGOT_PREFIX_H; return true; }
    if (strcmp(text, "T") == 0) { *out = INGOT_PREFIX_T; return true; }
    if (strcmp(text, "S") == 0) { *out = INGOT_PREFIX_S; return true; }
    if (strcmp(text, "F") == 0) { *out = INGOT_PREFIX_F; return true; }
    if (strcmp(text, "K") == 0) { *out = INGOT_PREFIX_K; return true; }
    if (strcmp(text, "RATi") == 0 || strcmp(text, "rati") == 0) {
        *out = INGOT_PREFIX_RATI;
        return true;
    }
    if (strcmp(text, "commissioned") == 0) {
        *out = INGOT_PREFIX_COMMISSIONED;
        return true;
    }
    return false;
}

static bool parse_header(const uint8_t raw[CHAIN_EVENT_HEADER_SIZE],
                         parsed_header_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->raw_header, raw, CHAIN_EVENT_HEADER_SIZE);
    out->epoch = read_le64(&raw[0]);
    out->event_id = read_le64(&raw[8]);
    out->type = raw[16];
    for (int i = 17; i < 24; i++) {
        if (raw[i] != 0) return false;
    }
    memcpy(out->authority, &raw[24], 32);
    memcpy(out->payload_hash, &raw[56], 32);
    memcpy(out->prev_hash, &raw[88], 32);
    memcpy(out->signature, &raw[120], 64);
    return true;
}

static bool hash_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static bool pubkey_from_filename(const char *path, uint8_t out[32],
                                 char out_b58[64]) {
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
    if (n == 0 || n >= 64u || base58_decode(stem, out, 32) != 32) return false;
    if (out_b58) memcpy(out_b58, stem, n + 1u);
    return true;
}

static bool append_receipt(receipt_list_t *list, const receipt_t *receipt) {
    if (list->count >= list->cap) {
        size_t next = list->cap ? list->cap * 2u : 16u;
        receipt_t *items = (receipt_t *)realloc(list->items, next * sizeof(receipt_t));
        if (!items) return false;
        list->items = items;
        list->cap = next;
    }
    list->items[list->count++] = *receipt;
    return true;
}

static bool append_claim(claim_list_t *list, const claim_record_t *claim) {
    if (list->count >= list->cap) {
        size_t next = list->cap ? list->cap * 2u : 16u;
        claim_record_t *items =
            (claim_record_t *)realloc(list->items, next * sizeof(claim_record_t));
        if (!items) return false;
        list->items = items;
        list->cap = next;
    }
    list->items[list->count++] = *claim;
    return true;
}

static const claim_record_t *find_claim_for_fragment(const claim_list_t *list,
                                                     uint64_t segment_id,
                                                     const uint8_t fragment_pub[32]) {
    if (!list || !fragment_pub) return NULL;
    for (size_t i = list->count; i > 0; i--) {
        const claim_record_t *claim = &list->items[i - 1u];
        if (claim->segment_id != segment_id) continue;
        if (memcmp(claim->fragment_pub, fragment_pub, 32) == 0)
            return claim;
    }
    return NULL;
}

static void json_string(FILE *out, const char *text) {
    fputc('"', out);
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
            switch (*p) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\b': fputs("\\b", out); break;
            case '\f': fputs("\\f", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20)
                    fprintf(out, "\\u%04x", (unsigned)*p);
                else
                    fputc((int)*p, out);
                break;
            }
        }
    }
    fputc('"', out);
}

static void copy_printable_text(char *dst, size_t cap,
                                const uint8_t *src, size_t len) {
    if (!dst || cap == 0) return;
    size_t n = 0;
    while (n + 1u < cap && n < len) {
        uint8_t c = src[n];
        if (c < 0x20 || c > 0x7e) break;
        dst[n] = (char)c;
        n++;
    }
    dst[n] = '\0';
}

static void update_context(receipt_context_t *ctx,
                           const uint8_t *payload, uint16_t payload_len) {
    if (!ctx || !payload ||
        payload_len < offsetof(chain_payload_operator_post_t, text))
        return;
    uint8_t kind = payload[0];
    uint16_t text_len = read_le16(&payload[36]);
    size_t text_off = offsetof(chain_payload_operator_post_t, text);
    if ((size_t)payload_len < text_off + (size_t)text_len) return;

    if (kind == OPERATOR_POST_BUILD_INFO) {
        copy_printable_text(ctx->build_id, sizeof(ctx->build_id),
                            &payload[text_off], text_len);
    } else if (kind == OPERATOR_POST_WORLD_INFO) {
        if (text_len >= 4u)
            ctx->world_id = read_le32(&payload[text_off]);
        if (text_len >= 8u)
            ctx->world_seq = read_le32(&payload[text_off + 4u]);
        if (text_len > 8u) {
            copy_printable_text(ctx->build_id, sizeof(ctx->build_id),
                                &payload[text_off + 8u],
                                (size_t)text_len - 8u);
        }
    }
}

static void compute_receipt_hash(receipt_t *receipt) {
    static const char domain[] = "SIGNAL:RATI:RECEIPT:v1";
    sha256_ctx_t ctx;
    uint16_t build_len = (uint16_t)strlen(receipt->context.build_id);
    sha256_init(&ctx);
    sha256_update(&ctx, domain, sizeof(domain) - 1u);
    sha256_update(&ctx, receipt->hdr.authority, 32);
    hash_u64_le(&ctx, receipt->segment_id);
    hash_u64_le(&ctx, receipt->hdr.event_id);
    hash_u64_le(&ctx, receipt->hdr.epoch);
    sha256_update(&ctx, receipt->event_hash, 32);
    sha256_update(&ctx, receipt->hdr.payload_hash, 32);
    sha256_update(&ctx, receipt->hdr.prev_hash, 32);
    sha256_update(&ctx, receipt->hdr.signature, 64);
    hash_u32_le(&ctx, receipt->context.world_id);
    hash_u32_le(&ctx, receipt->context.world_seq);
    hash_u16_le(&ctx, build_len);
    if (build_len)
        sha256_update(&ctx, receipt->context.build_id, build_len);
    sha256_update(&ctx, receipt->fragment_pub, 32);
    sha256_update(&ctx, receipt->ingot_pub, 32);
    sha256_update(&ctx, &receipt->prefix_class, 1);
    hash_u64_le(&ctx, receipt->mined_block);
    sha256_update(&ctx, &receipt->has_claim, 1);
    if (receipt->has_claim) {
        sha256_update(&ctx, receipt->claim.event_hash, 32);
        hash_u64_le(&ctx, receipt->claim.segment_id);
        hash_u64_le(&ctx, receipt->claim.hdr.event_id);
        hash_u64_le(&ctx, receipt->claim.hdr.epoch);
        sha256_update(&ctx, receipt->claim.fracture_seed, 32);
        sha256_update(&ctx, receipt->claim.claimant_pubkey, 32);
        hash_u32_le(&ctx, receipt->claim.fracture_id);
        hash_u32_le(&ctx, receipt->claim.burst_nonce);
        hash_u16_le(&ctx, receipt->claim.burst_cap);
        sha256_update(&ctx, &receipt->claim.grade, 1);
        sha256_update(&ctx, &receipt->claim.fragment_verified, 1);
        sha256_update(&ctx, &receipt->claim.grade_verified, 1);
    }
    sha256_final(&ctx, receipt->receipt_hash);
}

static claim_record_t build_claim_record(uint64_t segment_id,
                                         const parsed_header_t *hdr,
                                         const uint8_t *payload) {
    claim_record_t claim;
    memset(&claim, 0, sizeof(claim));
    claim.segment_id = segment_id;
    claim.hdr = *hdr;
    sha256_bytes(hdr->raw_header, CHAIN_EVENT_HEADER_SIZE, claim.event_hash);
    memcpy(claim.fracture_seed, &payload[0], 32);
    memcpy(claim.fragment_pub, &payload[32], 32);
    memcpy(claim.claimant_pubkey, &payload[64], 32);
    claim.fracture_id = read_le32(&payload[96]);
    claim.burst_nonce = read_le32(&payload[100]);
    claim.burst_cap = read_le16(&payload[104]);
    claim.grade = payload[106];
    claim.asteroid_slot = payload[107];

    mining_fragment_pub_compute(claim.fracture_seed,
                                claim.claimant_pubkey,
                                claim.burst_nonce,
                                claim.computed_fragment_pub);
    claim.fragment_verified =
        memcmp(claim.computed_fragment_pub, claim.fragment_pub, 32) == 0;

    mining_keypair_t kp;
    mining_keypair_derive(claim.fracture_seed, claim.claimant_pubkey,
                          claim.burst_nonce, &kp);
    mining_callsign_from_pubkey(kp.pub, claim.callsign);
    claim.computed_grade = (uint8_t)mining_classify_base58(claim.callsign);
    claim.grade_verified = claim.fragment_verified &&
                            claim.burst_cap > 0 &&
                            claim.burst_nonce < claim.burst_cap &&
                            claim.grade == claim.computed_grade &&
                            claim.grade < MINING_GRADE_COUNT;
    return claim;
}

static bool receipt_matches_filters(const cli_opts_t *opts,
                                    uint64_t segment_id,
                                    const parsed_header_t *hdr,
                                    const uint8_t ingot_pub[32],
                                    uint8_t prefix_class) {
    if (opts->event_id_set && hdr->event_id != opts->event_id) return false;
    if (opts->segment_id_set && segment_id != opts->segment_id) return false;
    if (opts->cargo_pub_set && memcmp(opts->cargo_pub, ingot_pub, 32) != 0) return false;
    if (prefix_class < opts->min_prefix) return false;
    return true;
}

static bool collect_receipts(const cli_opts_t *opts, receipt_list_t *out) {
    FILE *f = fopen(opts->path, "rb");
    if (!f) {
        fprintf(stderr, "signal_rati_receipt: cannot open %s\n", opts->path);
        return false;
    }

    claim_list_t claims;
    memset(&claims, 0, sizeof(claims));
    uint64_t segment_id = 0;
    uint64_t events_seen = 0;
    receipt_context_t context;
    memset(&context, 0, sizeof(context));

    for (;;) {
        uint8_t raw[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(raw, 1, sizeof(raw), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(raw)) {
            fprintf(stderr, "signal_rati_receipt: truncated header\n");
            free(claims.items);
            fclose(f);
            return false;
        }

        parsed_header_t hdr;
        if (!parse_header(raw, &hdr)) {
            fprintf(stderr, "signal_rati_receipt: malformed header padding\n");
            free(claims.items);
            fclose(f);
            return false;
        }

        uint8_t len_bytes[2];
        if (fread(len_bytes, 1, sizeof(len_bytes), f) != sizeof(len_bytes)) {
            fprintf(stderr, "signal_rati_receipt: missing payload length\n");
            free(claims.items);
            fclose(f);
            return false;
        }
        uint16_t payload_len = read_le16(len_bytes);
        uint8_t *payload = NULL;
        if (payload_len) {
            payload = (uint8_t *)malloc(payload_len);
            if (!payload) {
                free(claims.items);
                fclose(f);
                return false;
            }
            if (fread(payload, 1, payload_len, f) != payload_len) {
                free(payload);
                fprintf(stderr, "signal_rati_receipt: truncated payload\n");
                free(claims.items);
                fclose(f);
                return false;
            }
        }

        bool segment_reset = events_seen > 0 &&
                             hdr.event_id == 1 &&
                             hash_is_zero(hdr.prev_hash);
        if (segment_reset) {
            segment_id++;
            memset(&context, 0, sizeof(context));
        }
        events_seen++;

        if (hdr.type == CHAIN_EVT_OPERATOR_POST) {
            update_context(&context, payload, payload_len);
        } else if (hdr.type == CHAIN_EVT_CLAIM_FRAGMENT &&
                   payload_len == sizeof(chain_payload_claim_fragment_t)) {
            claim_record_t claim = build_claim_record(segment_id, &hdr, payload);
            if (!append_claim(&claims, &claim)) {
                free(payload);
                free(claims.items);
                fclose(f);
                return false;
            }
        } else if (hdr.type == CHAIN_EVT_SMELT &&
                   payload_len == sizeof(chain_payload_smelt_t)) {
            const uint8_t *fragment_pub = &payload[0];
            const uint8_t *ingot_pub = &payload[32];
            uint8_t prefix_class = payload[64];
            if (receipt_matches_filters(opts, segment_id, &hdr, ingot_pub,
                                        prefix_class)) {
                receipt_t receipt;
                memset(&receipt, 0, sizeof(receipt));
                receipt.segment_id = segment_id;
                receipt.hdr = hdr;
                sha256_bytes(hdr.raw_header, CHAIN_EVENT_HEADER_SIZE,
                             receipt.event_hash);
                memcpy(receipt.fragment_pub, fragment_pub, 32);
                memcpy(receipt.ingot_pub, ingot_pub, 32);
                receipt.prefix_class = prefix_class;
                receipt.mined_block = read_le64(&payload[72]);
                receipt.context = context;
                const claim_record_t *claim =
                    find_claim_for_fragment(&claims, segment_id, fragment_pub);
                if (claim) {
                    receipt.has_claim = true;
                    receipt.claim = *claim;
                }
                compute_receipt_hash(&receipt);
                if (!append_receipt(out, &receipt)) {
                    free(payload);
                    free(claims.items);
                    fclose(f);
                    return false;
                }
            }
        }

        free(payload);
    }

    fclose(f);
    free(claims.items);
    return true;
}

static void print_hex_field(FILE *out, const char *name, const uint8_t *bytes,
                            size_t len, bool comma) {
    char hex[129];
    hex_bytes(bytes, len, hex, sizeof(hex));
    fprintf(out, "\"%s\":\"%s\"%s", name, hex, comma ? "," : "");
}

static void print_receipt(FILE *out, const receipt_t *r,
                          const char station_b58[64]) {
    char hex[129];
    fprintf(out, "{\n");
    fprintf(out, "      \"version\":\"rati_mining_receipt_v1\",\n");
    fprintf(out, "      ");
    print_hex_field(out, "receipt_hash", r->receipt_hash, 32, true);
    fprintf(out, "\n");
    fprintf(out, "      ");
    print_hex_field(out, "station_pubkey", r->hdr.authority, 32, true);
    fprintf(out, "\n");
    fprintf(out, "      \"station_pubkey_b58\":\"%s\",\n", station_b58);
    fprintf(out, "      \"world\":{\"world_id\":%u,\"world_seq\":%u,\"build_id\":",
            (unsigned)r->context.world_id, (unsigned)r->context.world_seq);
    if (r->context.build_id[0])
        json_string(out, r->context.build_id);
    else
        fputs("null", out);
    fprintf(out, "},\n");

    fprintf(out, "      \"event\":{");
    fprintf(out, "\"kind\":\"CHAIN_EVT_SMELT\",");
    fprintf(out, "\"event_id\":%llu,",
            (unsigned long long)r->hdr.event_id);
    fprintf(out, "\"segment_id\":%llu,",
            (unsigned long long)r->segment_id);
    fprintf(out, "\"epoch\":%llu,",
            (unsigned long long)r->hdr.epoch);
    print_hex_field(out, "event_hash", r->event_hash, 32, true);
    print_hex_field(out, "payload_hash", r->hdr.payload_hash, 32, true);
    print_hex_field(out, "prev_hash", r->hdr.prev_hash, 32, true);
    print_hex_field(out, "signature", r->hdr.signature, 64, false);
    fprintf(out, "},\n");

    fprintf(out, "      \"mining\":{");
    print_hex_field(out, "fragment_pub", r->fragment_pub, 32, true);
    print_hex_field(out, "cargo_pub", r->ingot_pub, 32, true);
    print_hex_field(out, "parent_merkle", r->fragment_pub, 32, true);
    fprintf(out, "\"grade\":");
    if (r->has_claim && r->claim.grade_verified)
        json_string(out, grade_name(r->claim.grade));
    else
        fputs("null", out);
    fprintf(out, ",\"grade_verified\":%s,",
            (r->has_claim && r->claim.grade_verified) ? "true" : "false");
    fprintf(out, "\"grade_note\":");
    if (r->has_claim)
        json_string(out, r->claim.grade_verified
            ? "verified from CHAIN_EVT_CLAIM_FRAGMENT"
            : "claim fragment proof did not verify");
    else
        json_string(out, "no CHAIN_EVT_CLAIM_FRAGMENT matched this smelt");
    fprintf(out, ",");
    fprintf(out, "\"prefix_class\":\"%s\",", prefix_name(r->prefix_class));
    fprintf(out, "\"prefix_class_id\":%u,", (unsigned)r->prefix_class);
    fprintf(out, "\"mined_tick\":%llu",
            (unsigned long long)r->mined_block);
    fprintf(out, "},\n");

    fprintf(out, "      \"claim\":");
    if (r->has_claim) {
        fprintf(out, "{");
        fprintf(out, "\"kind\":\"CHAIN_EVT_CLAIM_FRAGMENT\",");
        fprintf(out, "\"event_id\":%llu,",
                (unsigned long long)r->claim.hdr.event_id);
        fprintf(out, "\"segment_id\":%llu,",
                (unsigned long long)r->claim.segment_id);
        fprintf(out, "\"epoch\":%llu,",
                (unsigned long long)r->claim.hdr.epoch);
        print_hex_field(out, "event_hash", r->claim.event_hash, 32, true);
        print_hex_field(out, "fracture_seed", r->claim.fracture_seed, 32, true);
        print_hex_field(out, "claimant_pubkey", r->claim.claimant_pubkey, 32, true);
        fprintf(out, "\"fracture_id\":%u,", (unsigned)r->claim.fracture_id);
        fprintf(out, "\"burst_nonce\":%u,", (unsigned)r->claim.burst_nonce);
        fprintf(out, "\"burst_cap\":%u,", (unsigned)r->claim.burst_cap);
        fprintf(out, "\"asteroid_slot\":%u,", (unsigned)r->claim.asteroid_slot);
        fprintf(out, "\"callsign\":");
        json_string(out, r->claim.callsign);
        fprintf(out, ",");
        fprintf(out, "\"claimed_grade\":\"%s\",", grade_name(r->claim.grade));
        fprintf(out, "\"computed_grade\":\"%s\",", grade_name(r->claim.computed_grade));
        print_hex_field(out, "computed_fragment_pub",
                        r->claim.computed_fragment_pub, 32, true);
        fprintf(out, "\"fragment_verified\":%s,",
                r->claim.fragment_verified ? "true" : "false");
        fprintf(out, "\"grade_verified\":%s",
                r->claim.grade_verified ? "true" : "false");
        fprintf(out, "}");
    } else {
        fputs("null", out);
    }
    fprintf(out, ",\n");

    fprintf(out, "      \"arweave\":{\"segment_tx\":null,\"checkpoint_tx\":null,\"manifest_tx\":null},\n");
    fprintf(out, "      \"bitcoin\":{\"batch_root\":null,\"anchor_txid\":null,\"block_height\":null,\"block_hash\":null}\n");
    hex_bytes(r->receipt_hash, 32, hex, sizeof(hex));
    (void)hex;
    fprintf(out, "    }");
}

static void print_output(FILE *out, const cli_opts_t *opts,
                         const char station_b58[64],
                         const uint8_t station_pubkey[32],
                         const chain_log_verify_report_t *report,
                         const receipt_list_t *receipts) {
    char station_hex[65];
    hex_bytes(station_pubkey, 32, station_hex, sizeof(station_hex));

    fprintf(out, "{\n");
    fprintf(out, "  \"schema\":\"signal.rati_mining_receipts.v1\",\n");
    fprintf(out, "  \"source\":{");
    fprintf(out, "\"chain_log_path\":");
    json_string(out, opts->path);
    fprintf(out, ",\"station_pubkey\":\"%s\",", station_hex);
    fprintf(out, "\"station_pubkey_b58\":\"%s\",", station_b58);
    fprintf(out, "\"verified\":true,");
    fprintf(out, "\"valid_events\":%llu,",
            (unsigned long long)report->valid_events);
    fprintf(out, "\"segment_count\":%llu",
            (unsigned long long)report->segment_count);
    fprintf(out, "},\n");
    fprintf(out, "  \"filter\":{");
    fprintf(out, "\"min_prefix\":\"%s\"", prefix_name(opts->min_prefix));
    if (opts->event_id_set)
        fprintf(out, ",\"event_id\":%llu", (unsigned long long)opts->event_id);
    if (opts->segment_id_set)
        fprintf(out, ",\"segment_id\":%llu", (unsigned long long)opts->segment_id);
    if (opts->cargo_pub_set) {
        char cargo_hex[65];
        hex_bytes(opts->cargo_pub, 32, cargo_hex, sizeof(cargo_hex));
        fprintf(out, ",\"cargo_pub\":\"%s\"", cargo_hex);
    }
    fprintf(out, "},\n");
    fprintf(out, "  \"receipt_count\":%zu,\n", receipts->count);
    fprintf(out, "  \"receipts\":[\n");
    for (size_t i = 0; i < receipts->count; i++) {
        print_receipt(out, &receipts->items[i], station_b58);
        if (i + 1u < receipts->count) fprintf(out, ",");
        fprintf(out, "\n");
    }
    fprintf(out, "  ]\n");
    fprintf(out, "}\n");
}

static bool parse_args(int argc, char **argv, cli_opts_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->min_prefix = INGOT_PREFIX_ANONYMOUS;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout);
            exit(0);
        } else if (strncmp(arg, "--station-pubkey=", 17) == 0) {
            opts->station_pubkey_b58_override = arg + 17;
        } else if (strncmp(arg, "--event-id=", 11) == 0) {
            opts->event_id_set = true;
            if (!parse_u64_arg(arg + 11, &opts->event_id)) {
                fprintf(stderr, "signal_rati_receipt: invalid --event-id\n");
                return false;
            }
        } else if (strncmp(arg, "--segment-id=", 13) == 0) {
            opts->segment_id_set = true;
            if (!parse_u64_arg(arg + 13, &opts->segment_id)) {
                fprintf(stderr, "signal_rati_receipt: invalid --segment-id\n");
                return false;
            }
        } else if (strncmp(arg, "--cargo-pub=", 12) == 0) {
            opts->cargo_pub_set = true;
            if (!parse_pub_arg(arg + 12, opts->cargo_pub)) {
                fprintf(stderr, "signal_rati_receipt: invalid --cargo-pub\n");
                return false;
            }
        } else if (strncmp(arg, "--min-prefix=", 13) == 0) {
            if (!parse_prefix_class(arg + 13, &opts->min_prefix)) {
                fprintf(stderr, "signal_rati_receipt: invalid --min-prefix\n");
                return false;
            }
        } else if (arg[0] == '-') {
            fprintf(stderr, "signal_rati_receipt: unknown option %s\n", arg);
            return false;
        } else if (!opts->path) {
            opts->path = arg;
        } else {
            fprintf(stderr, "signal_rati_receipt: only one chain log path is supported\n");
            return false;
        }
    }
    if (!opts->path) {
        print_usage(stderr);
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    cli_opts_t opts;
    if (!parse_args(argc, argv, &opts)) return 2;

    uint8_t station_pubkey[32];
    char station_b58[64] = {0};
    if (opts.station_pubkey_b58_override) {
        if (base58_decode(opts.station_pubkey_b58_override, station_pubkey, 32) != 32) {
            fprintf(stderr, "signal_rati_receipt: invalid --station-pubkey\n");
            return 2;
        }
        snprintf(station_b58, sizeof(station_b58), "%s",
                 opts.station_pubkey_b58_override);
    } else if (!pubkey_from_filename(opts.path, station_pubkey, station_b58)) {
        fprintf(stderr, "signal_rati_receipt: cannot infer station pubkey from filename; use --station-pubkey\n");
        return 2;
    }

    FILE *vf = fopen(opts.path, "rb");
    if (!vf) {
        fprintf(stderr, "signal_rati_receipt: cannot open %s\n", opts.path);
        return 2;
    }
    chain_log_verify_report_t report;
    bool verified = chain_log_verify_with_pubkey(vf, station_pubkey, &report);
    fclose(vf);
    if (!verified) {
        fprintf(stderr, "signal_rati_receipt: chain verification failed: %s\n",
                report.first_fail_reason[0] ? report.first_fail_reason : "unknown");
        return 1;
    }

    receipt_list_t receipts;
    memset(&receipts, 0, sizeof(receipts));
    if (!collect_receipts(&opts, &receipts)) {
        free(receipts.items);
        return 1;
    }

    print_output(stdout, &opts, station_b58, station_pubkey, &report, &receipts);
    free(receipts.items);
    return 0;
}
