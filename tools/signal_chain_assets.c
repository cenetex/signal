/*
 * signal_chain_assets -- chain-log asset inventory exporter.
 *
 * Walks station chain logs, keeps parsing across linkage breaks, and emits
 * one row per unique cargo_unit_t.pub. This is intentionally an analysis
 * tool, not a repair tool: it reports the observed segment/linkage shape
 * and asset provenance without rewriting logs.
 */

#include "chain_log.h"

#include "base58.h"
#include "sha256.h"
#include "signal_crypto.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

#define CHAIN_UNSIGNED_HEADER_SIZE 120
#define MAX_PATH_TEXT 512

typedef enum {
    FORMAT_JSON = 0,
    FORMAT_CSV = 1,
} output_format_t;

typedef struct {
    output_format_t format;
    const char *out_path;
    const char *station_pubkey_b58;
    bool verify_signatures;
} cli_opts_t;

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} path_list_t;

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
    char path[MAX_PATH_TEXT];
    char station_pubkey_b58[64];
    bool expected_authority_known;
    uint8_t expected_authority[32];

    uint64_t events_total;
    uint64_t events_payload_ok;
    uint64_t events_signature_ok;
    uint64_t segments;
    uint64_t segment_resets;
    uint64_t linkage_breaks;
    uint64_t monotonic_breaks;
    uint64_t payload_hash_failures;
    uint64_t signature_failures;
    uint64_t authority_failures;
    uint64_t malformed_headers;
    uint64_t truncated_records;
    uint64_t event_type_counts[CHAIN_EVT_TYPE_COUNT];
    char first_failure[160];
} file_summary_t;

typedef enum {
    ASSET_SOURCE_UNKNOWN_TRANSFER = 0,
    ASSET_SOURCE_SMELT = 1,
    ASSET_SOURCE_CRAFT = 2,
} asset_source_t;

typedef struct {
    uint8_t cargo_pub[32];
    asset_source_t source;
    uint64_t mint_count;
    uint64_t transfer_count;
    uint64_t first_transfer_event_id;
    uint64_t last_transfer_event_id;

    int source_file_index;
    uint64_t source_event_id;
    uint64_t source_epoch;
    uint64_t source_world_id;
    uint64_t source_world_seq;
    uint64_t source_segment_id;
    bool source_strict_ok;
    uint8_t source_station_pubkey[32];

    int kind;
    int commodity;
    int recipe_id;
    int prefix_class;
    uint64_t mined_block;
    uint8_t parent_fragment_pub[32];
    bool has_parent_fragment;
    uint8_t input_pubs[RECIPE_INPUT_MAX][32];
    uint8_t input_count;
} asset_row_t;

typedef struct {
    file_summary_t *files;
    size_t file_count;
    size_t file_cap;
    asset_row_t *assets;
    size_t asset_count;
    size_t asset_cap;
} analysis_t;

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_chain_assets [options] <chain-log-or-dir>...\n"
        "\n"
        "Options:\n"
        "  --format=<json|csv>         Output format (default: json)\n"
        "  --out=<path>                Write output to file instead of stdout\n"
        "  --station-pubkey=<base58>   Override expected pubkey for all logs\n"
        "  --no-signatures             Skip Ed25519 signature checks\n"
        "  -h, --help                  This message\n"
        "\n"
        "JSON output schema: signal.chain_assets.v1\n");
}

static uint64_t read_le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static uint32_t read_le32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (i * 8);
    return v;
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
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

static void pub_hex(const uint8_t pub[32], char out[65]) {
    hex_bytes(pub, 32, out, 65);
}

static void pub_b58(const uint8_t pub[32], char out[64]) {
    if (base58_encode(pub, 32, out, 64) == 0)
        out[0] = '\0';
}

static const char *event_type_name(uint8_t type) {
    switch (type) {
    case CHAIN_EVT_SMELT: return "SMELT";
    case CHAIN_EVT_CRAFT: return "CRAFT";
    case CHAIN_EVT_TRANSFER: return "TRANSFER";
    case CHAIN_EVT_TRADE: return "TRADE";
    case CHAIN_EVT_LEDGER: return "LEDGER";
    case CHAIN_EVT_ROCK_DESTROY: return "ROCK_DESTROY";
    case CHAIN_EVT_OPERATOR_POST: return "OPERATOR_POST";
    case CHAIN_EVT_FRAGMENT_TOW: return "FRAGMENT_TOW";
    case CHAIN_EVT_FRAGMENT_RELEASE: return "FRAGMENT_RELEASE";
    case CHAIN_EVT_DEATH: return "DEATH";
    default: return "UNKNOWN";
    }
}

static const char *source_name(asset_source_t source) {
    switch (source) {
    case ASSET_SOURCE_SMELT: return "smelt";
    case ASSET_SOURCE_CRAFT: return "craft";
    case ASSET_SOURCE_UNKNOWN_TRANSFER:
    default: return "unknown_transfer";
    }
}

static const char *prefix_name(int prefix) {
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

static const char *commodity_code_for(int commodity) {
    switch (commodity) {
    case COMMODITY_FERRITE_ORE: return "FE_ORE";
    case COMMODITY_CUPRITE_ORE: return "CU_ORE";
    case COMMODITY_CRYSTAL_ORE: return "CR_ORE";
    case COMMODITY_FERRITE_INGOT: return "FE_INGOT";
    case COMMODITY_CUPRITE_INGOT: return "CU_INGOT";
    case COMMODITY_CRYSTAL_INGOT: return "CR_INGOT";
    case COMMODITY_FRAME: return "FRAME";
    case COMMODITY_LASER_MODULE: return "LASER";
    case COMMODITY_TRACTOR_MODULE: return "TRACTOR";
    case COMMODITY_REPAIR_KIT: return "REPAIR_KIT";
    default: return "";
    }
}

static const char *recipe_name_for(int recipe_id) {
    switch (recipe_id) {
    case RECIPE_SMELT: return "smelt";
    case RECIPE_FRAME_BASIC: return "frame_basic";
    case RECIPE_LASER_BASIC: return "laser_basic";
    case RECIPE_TRACTOR_COIL: return "tractor_coil";
    case RECIPE_REPAIR_KIT_FAB: return "repair_kit_fab";
    case RECIPE_LEGACY_MIGRATE: return "legacy_migrate";
    default: return "";
    }
}

static int recipe_output_commodity(int recipe_id) {
    switch (recipe_id) {
    case RECIPE_FRAME_BASIC: return COMMODITY_FRAME;
    case RECIPE_LASER_BASIC: return COMMODITY_LASER_MODULE;
    case RECIPE_TRACTOR_COIL: return COMMODITY_TRACTOR_MODULE;
    case RECIPE_REPAIR_KIT_FAB: return COMMODITY_REPAIR_KIT;
    default: return -1;
    }
}

static int recipe_output_kind(int recipe_id) {
    switch (recipe_id) {
    case RECIPE_FRAME_BASIC: return CARGO_KIND_FRAME;
    case RECIPE_LASER_BASIC: return CARGO_KIND_LASER;
    case RECIPE_TRACTOR_COIL: return CARGO_KIND_TRACTOR;
    case RECIPE_REPAIR_KIT_FAB: return CARGO_KIND_REPAIR_KIT;
    default: return -1;
    }
}

static char *dup_text(const char *s) {
    size_t n = strlen(s) + 1u;
    char *out = (char *)malloc(n);
    if (out) memcpy(out, s, n);
    return out;
}

static bool path_list_append(path_list_t *list, const char *path) {
    if (!list || !path) return false;
    if (list->count >= list->cap) {
        size_t next = list->cap ? list->cap * 2u : 16u;
        char **items = (char **)realloc(list->items, next * sizeof(char *));
        if (!items) return false;
        list->items = items;
        list->cap = next;
    }
    list->items[list->count] = dup_text(path);
    if (!list->items[list->count]) return false;
    list->count++;
    return true;
}

static void path_list_free(path_list_t *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int path_cmp(const void *a, const void *b) {
    const char *const *pa = (const char *const *)a;
    const char *const *pb = (const char *const *)b;
    return strcmp(*pa, *pb);
}

static bool ends_with_log(const char *path) {
    size_t n = strlen(path);
    return n >= 4u && strcmp(path + n - 4u, ".log") == 0;
}

static bool add_input_path(path_list_t *list, const char *path) {
#ifndef _WIN32
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "signal_chain_assets: cannot open dir %s: %s\n",
                    path, strerror(errno));
            return false;
        }
        struct dirent *ent;
        bool ok = true;
        while ((ent = readdir(dir)) != NULL) {
            if (!ends_with_log(ent->d_name)) continue;
            char full[MAX_PATH_TEXT];
            int written = snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
            if (written <= 0 || (size_t)written >= sizeof(full) ||
                !path_list_append(list, full)) {
                ok = false;
                break;
            }
        }
        closedir(dir);
        return ok;
    }
#endif
    return path_list_append(list, path);
}

static bool parse_pubkey_from_filename(const char *path, uint8_t out[32],
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
    if (n == 0 || n >= 64 || base58_decode(stem, out, 32) != 32) return false;
    if (out_b58) {
        memcpy(out_b58, stem, n + 1u);
    }
    return true;
}

static bool analysis_add_file(analysis_t *a, const file_summary_t *summary) {
    if (a->file_count >= a->file_cap) {
        size_t next = a->file_cap ? a->file_cap * 2u : 8u;
        file_summary_t *files =
            (file_summary_t *)realloc(a->files, next * sizeof(file_summary_t));
        if (!files) return false;
        a->files = files;
        a->file_cap = next;
    }
    a->files[a->file_count++] = *summary;
    return true;
}

static asset_row_t *analysis_find_asset(analysis_t *a, const uint8_t cargo_pub[32]) {
    for (size_t i = 0; i < a->asset_count; i++) {
        if (memcmp(a->assets[i].cargo_pub, cargo_pub, 32) == 0)
            return &a->assets[i];
    }
    return NULL;
}

static asset_row_t *analysis_upsert_asset(analysis_t *a, const uint8_t cargo_pub[32]) {
    asset_row_t *existing = analysis_find_asset(a, cargo_pub);
    if (existing) return existing;
    if (a->asset_count >= a->asset_cap) {
        size_t next = a->asset_cap ? a->asset_cap * 2u : 256u;
        asset_row_t *assets =
            (asset_row_t *)realloc(a->assets, next * sizeof(asset_row_t));
        if (!assets) return NULL;
        a->assets = assets;
        a->asset_cap = next;
    }
    asset_row_t *row = &a->assets[a->asset_count++];
    memset(row, 0, sizeof(*row));
    memcpy(row->cargo_pub, cargo_pub, 32);
    row->source = ASSET_SOURCE_UNKNOWN_TRANSFER;
    row->source_file_index = -1;
    row->kind = -1;
    row->commodity = -1;
    row->recipe_id = -1;
    row->prefix_class = -1;
    return row;
}

static bool parse_header(const uint8_t raw[CHAIN_EVENT_HEADER_SIZE],
                         parsed_header_t *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->raw_header, raw, CHAIN_EVENT_HEADER_SIZE);
    out->epoch = read_le64(&raw[0]);
    out->event_id = read_le64(&raw[8]);
    out->type = raw[16];
    memcpy(out->authority, &raw[24], 32);
    memcpy(out->payload_hash, &raw[56], 32);
    memcpy(out->prev_hash, &raw[88], 32);
    memcpy(out->signature, &raw[120], 64);
    for (int i = 17; i < 24; i++) {
        if (raw[i] != 0) return false;
    }
    return true;
}

static void header_hash(const parsed_header_t *hdr, uint8_t out[32]) {
    sha256_bytes(hdr->raw_header, CHAIN_EVENT_HEADER_SIZE, out);
}

static void remember_failure(file_summary_t *summary, const char *reason,
                             uint64_t event_id) {
    if (summary->first_failure[0] != '\0') return;
    snprintf(summary->first_failure, sizeof(summary->first_failure),
             "event %llu: %s", (unsigned long long)event_id, reason);
}

static bool payload_hash_ok(const parsed_header_t *hdr, const uint8_t *payload,
                            uint16_t payload_len) {
    uint8_t computed[32];
    static const uint8_t empty[1] = {0};
    sha256_bytes(payload_len ? payload : empty, payload_len, computed);
    return memcmp(computed, hdr->payload_hash, 32) == 0;
}

static bool hash_is_zero(const uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        if (hash[i] != 0) return false;
    }
    return true;
}

static bool signature_ok(const parsed_header_t *hdr) {
    return signal_crypto_verify(hdr->signature, hdr->raw_header,
                                CHAIN_UNSIGNED_HEADER_SIZE, hdr->authority);
}

static void update_world_cursor(uint32_t *world_id, uint32_t *world_seq,
                                const uint8_t *payload, uint16_t payload_len) {
    if (!payload || payload_len < offsetof(chain_payload_operator_post_t, text))
        return;
    if (payload[0] != OPERATOR_POST_WORLD_INFO) return;
    uint16_t text_len = read_le16(&payload[36]);
    size_t text_off = offsetof(chain_payload_operator_post_t, text);
    if ((size_t)payload_len < text_off + text_len) return;
    if (text_len >= 4)
        *world_id = read_le32(&payload[text_off]);
    if (text_len >= 8)
        *world_seq = read_le32(&payload[text_off + 4u]);
}

static void record_smelt_asset(analysis_t *analysis, int file_index,
                               const parsed_header_t *hdr,
                               const uint8_t *payload, uint16_t payload_len,
                               uint64_t segment_id, bool strict_ok,
                               uint32_t world_id, uint32_t world_seq) {
    if (payload_len < sizeof(chain_payload_smelt_t)) return;
    const uint8_t *fragment_pub = &payload[0];
    const uint8_t *ingot_pub = &payload[32];
    asset_row_t *row = analysis_upsert_asset(analysis, ingot_pub);
    if (!row) return;
    row->mint_count++;
    if (row->source != ASSET_SOURCE_UNKNOWN_TRANSFER) return;

    row->source = ASSET_SOURCE_SMELT;
    row->source_file_index = file_index;
    row->source_event_id = hdr->event_id;
    row->source_epoch = hdr->epoch;
    row->source_world_id = world_id;
    row->source_world_seq = world_seq;
    row->source_segment_id = segment_id;
    row->source_strict_ok = strict_ok;
    memcpy(row->source_station_pubkey, hdr->authority, 32);
    row->kind = CARGO_KIND_INGOT;
    row->commodity = -1;
    row->recipe_id = RECIPE_SMELT;
    row->prefix_class = payload[64];
    row->mined_block = read_le64(&payload[72]);
    memcpy(row->parent_fragment_pub, fragment_pub, 32);
    row->has_parent_fragment = true;
}

static void record_craft_asset(analysis_t *analysis, int file_index,
                               const parsed_header_t *hdr,
                               const uint8_t *payload, uint16_t payload_len,
                               uint64_t segment_id, bool strict_ok,
                               uint32_t world_id, uint32_t world_seq) {
    if (payload_len < offsetof(chain_payload_craft_t, input_pubs)) return;
    int recipe_id = (int)read_le16(&payload[0]);
    uint8_t input_count = payload[2];
    const uint8_t *output_pub = &payload[8];
    asset_row_t *row = analysis_upsert_asset(analysis, output_pub);
    if (!row) return;
    row->mint_count++;
    if (row->source != ASSET_SOURCE_UNKNOWN_TRANSFER) return;

    size_t available_inputs = 0;
    if (payload_len > offsetof(chain_payload_craft_t, input_pubs))
        available_inputs = ((size_t)payload_len -
                            offsetof(chain_payload_craft_t, input_pubs)) / 32u;
    if (available_inputs > RECIPE_INPUT_MAX) available_inputs = RECIPE_INPUT_MAX;
    if (input_count > available_inputs) input_count = (uint8_t)available_inputs;

    row->source = ASSET_SOURCE_CRAFT;
    row->source_file_index = file_index;
    row->source_event_id = hdr->event_id;
    row->source_epoch = hdr->epoch;
    row->source_world_id = world_id;
    row->source_world_seq = world_seq;
    row->source_segment_id = segment_id;
    row->source_strict_ok = strict_ok;
    memcpy(row->source_station_pubkey, hdr->authority, 32);
    row->kind = recipe_output_kind(recipe_id);
    row->commodity = recipe_output_commodity(recipe_id);
    row->recipe_id = recipe_id;
    row->prefix_class = INGOT_PREFIX_ANONYMOUS;
    row->input_count = input_count;
    for (uint8_t i = 0; i < input_count; i++) {
        memcpy(row->input_pubs[i],
               &payload[offsetof(chain_payload_craft_t, input_pubs) + (size_t)i * 32u],
               32);
    }
}

static void record_transfer_asset(analysis_t *analysis,
                                  const parsed_header_t *hdr,
                                  const uint8_t *payload,
                                  uint16_t payload_len) {
    if (payload_len < sizeof(chain_payload_transfer_t)) return;
    const uint8_t *cargo_pub = &payload[64];
    asset_row_t *row = analysis_upsert_asset(analysis, cargo_pub);
    if (!row) return;
    row->transfer_count++;
    if (row->first_transfer_event_id == 0)
        row->first_transfer_event_id = hdr->event_id;
    row->last_transfer_event_id = hdr->event_id;
    if (row->source == ASSET_SOURCE_UNKNOWN_TRANSFER &&
        row->source_file_index < 0) {
        memcpy(row->source_station_pubkey, hdr->authority, 32);
    }
}

static bool analyze_file(analysis_t *analysis, const cli_opts_t *opts,
                         const char *path) {
    file_summary_t summary;
    memset(&summary, 0, sizeof(summary));
    snprintf(summary.path, sizeof(summary.path), "%s", path);
    if (opts->station_pubkey_b58) {
        if (base58_decode(opts->station_pubkey_b58, summary.expected_authority, 32) != 32) {
            fprintf(stderr, "signal_chain_assets: invalid --station-pubkey\n");
            return false;
        }
        summary.expected_authority_known = true;
        snprintf(summary.station_pubkey_b58, sizeof(summary.station_pubkey_b58),
                 "%s", opts->station_pubkey_b58);
    } else if (parse_pubkey_from_filename(path, summary.expected_authority,
                                          summary.station_pubkey_b58)) {
        summary.expected_authority_known = true;
    } else {
        snprintf(summary.station_pubkey_b58, sizeof(summary.station_pubkey_b58),
                 "unknown");
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "signal_chain_assets: cannot open %s: %s\n",
                path, strerror(errno));
        return false;
    }

    int file_index = (int)analysis->file_count;
    uint8_t expected_prev[32] = {0};
    uint64_t expected_event_id = 1;
    uint64_t segment_id = 0;
    uint32_t world_id = 0;
    uint32_t world_seq = 0;

    for (;;) {
        uint8_t raw[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(raw, 1, sizeof(raw), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(raw)) {
            summary.truncated_records++;
            remember_failure(&summary, "truncated header", expected_event_id);
            break;
        }

        parsed_header_t hdr;
        bool header_ok = parse_header(raw, &hdr);
        if (!header_ok) {
            summary.malformed_headers++;
            remember_failure(&summary, "malformed header padding", hdr.event_id);
        }

        uint8_t len_bytes[2];
        if (fread(len_bytes, 1, sizeof(len_bytes), f) != sizeof(len_bytes)) {
            summary.truncated_records++;
            remember_failure(&summary, "missing payload length", hdr.event_id);
            break;
        }
        uint16_t payload_len = read_le16(len_bytes);
        uint8_t *payload = NULL;
        if (payload_len > 0) {
            payload = (uint8_t *)malloc(payload_len);
            if (!payload) {
                fclose(f);
                return false;
            }
            if (fread(payload, 1, payload_len, f) != payload_len) {
                free(payload);
                summary.truncated_records++;
                remember_failure(&summary, "truncated payload", hdr.event_id);
                break;
            }
        }

        summary.events_total++;
        if (summary.events_total == 1) summary.segments = 1;
        if (hdr.type < CHAIN_EVT_TYPE_COUNT)
            summary.event_type_counts[hdr.type]++;

        bool segment_reset = summary.events_total > 1 &&
                             hdr.event_id == 1 &&
                             hash_is_zero(hdr.prev_hash);
        if (segment_reset) {
            segment_id++;
            summary.segments++;
            summary.segment_resets++;
            memset(expected_prev, 0, sizeof(expected_prev));
            expected_event_id = 1;
            world_id = 0;
            world_seq = 0;
        }

        bool bad_linkage = memcmp(hdr.prev_hash, expected_prev, 32) != 0;
        bool bad_event_id = hdr.event_id != expected_event_id;
        if (summary.events_total > 1 && !segment_reset &&
            (bad_linkage || bad_event_id)) {
            segment_id++;
            summary.segments++;
        }
        if (bad_linkage) {
            summary.linkage_breaks++;
            remember_failure(&summary, "prev_hash linkage break", hdr.event_id);
        }
        if (bad_event_id) {
            summary.monotonic_breaks++;
            remember_failure(&summary, "event_id monotonic break", hdr.event_id);
        }

        bool authority_ok = true;
        if (summary.expected_authority_known &&
            memcmp(hdr.authority, summary.expected_authority, 32) != 0) {
            authority_ok = false;
            summary.authority_failures++;
            remember_failure(&summary, "authority pubkey mismatch", hdr.event_id);
        }

        bool ph_ok = payload_hash_ok(&hdr, payload, payload_len);
        if (ph_ok) {
            summary.events_payload_ok++;
        } else {
            summary.payload_hash_failures++;
            remember_failure(&summary, "payload hash mismatch", hdr.event_id);
        }

        bool sig_ok = true;
        if (opts->verify_signatures) {
            sig_ok = signature_ok(&hdr);
            if (sig_ok) {
                summary.events_signature_ok++;
            } else {
                summary.signature_failures++;
                remember_failure(&summary, "signature verification failed", hdr.event_id);
            }
        }

        bool strict_ok = header_ok && authority_ok && ph_ok && sig_ok &&
                         !bad_linkage && !bad_event_id;

        if (hdr.type == CHAIN_EVT_OPERATOR_POST)
            update_world_cursor(&world_id, &world_seq, payload, payload_len);
        else if (hdr.type == CHAIN_EVT_SMELT)
            record_smelt_asset(analysis, file_index, &hdr, payload, payload_len,
                               segment_id, strict_ok, world_id, world_seq);
        else if (hdr.type == CHAIN_EVT_CRAFT)
            record_craft_asset(analysis, file_index, &hdr, payload, payload_len,
                               segment_id, strict_ok, world_id, world_seq);
        else if (hdr.type == CHAIN_EVT_TRANSFER)
            record_transfer_asset(analysis, &hdr, payload, payload_len);

        header_hash(&hdr, expected_prev);
        expected_event_id = hdr.event_id + 1u;
        free(payload);
    }

    fclose(f);
    if (summary.first_failure[0] == '\0')
        snprintf(summary.first_failure, sizeof(summary.first_failure), "none");
    return analysis_add_file(analysis, &summary);
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p == '\n') {
            fputs("\\n", out);
        } else if (*p == '\r') {
            fputs("\\r", out);
        } else if (*p == '\t') {
            fputs("\\t", out);
        } else if (*p < 0x20) {
            fprintf(out, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void json_hex32(FILE *out, const uint8_t pub[32]) {
    char hex[65];
    pub_hex(pub, hex);
    json_string(out, hex);
}

static void emit_json(const analysis_t *analysis, FILE *out) {
    uint64_t total_events = 0;
    uint64_t total_breaks = 0;
    uint64_t total_payload_failures = 0;
    uint64_t total_signature_failures = 0;
    uint64_t total_authority_failures = 0;
    uint64_t total_segments = 0;
    uint64_t total_segment_resets = 0;
    for (size_t i = 0; i < analysis->file_count; i++) {
        total_events += analysis->files[i].events_total;
        total_breaks += analysis->files[i].linkage_breaks;
        total_payload_failures += analysis->files[i].payload_hash_failures;
        total_signature_failures += analysis->files[i].signature_failures;
        total_authority_failures += analysis->files[i].authority_failures;
        total_segments += analysis->files[i].segments;
        total_segment_resets += analysis->files[i].segment_resets;
    }

    fprintf(out, "{\n  \"schema\":\"signal.chain_assets.v1\",\n");
    fprintf(out, "  \"totals\":{\"files\":%zu,\"assets\":%zu,"
                 "\"events\":%llu,\"segments\":%llu,\"segment_resets\":%llu,"
                 "\"linkage_breaks\":%llu,"
                 "\"payload_hash_failures\":%llu,\"signature_failures\":%llu,"
                 "\"authority_failures\":%llu},\n",
            analysis->file_count, analysis->asset_count,
            (unsigned long long)total_events,
            (unsigned long long)total_segments,
            (unsigned long long)total_segment_resets,
            (unsigned long long)total_breaks,
            (unsigned long long)total_payload_failures,
            (unsigned long long)total_signature_failures,
            (unsigned long long)total_authority_failures);

    fprintf(out, "  \"files\":[\n");
    for (size_t i = 0; i < analysis->file_count; i++) {
        const file_summary_t *f = &analysis->files[i];
        fprintf(out, "    {\"path\":");
        json_string(out, f->path);
        fprintf(out, ",\"station_pubkey_b58\":");
        json_string(out, f->station_pubkey_b58);
        fprintf(out, ",\"events\":%llu,\"segments\":%llu,"
                     "\"segment_resets\":%llu,"
                     "\"linkage_breaks\":%llu,\"monotonic_breaks\":%llu,"
                     "\"payload_hash_failures\":%llu,\"signature_failures\":%llu,"
                     "\"authority_failures\":%llu,\"truncated_records\":%llu,"
                     "\"first_failure\":",
                (unsigned long long)f->events_total,
                (unsigned long long)f->segments,
                (unsigned long long)f->segment_resets,
                (unsigned long long)f->linkage_breaks,
                (unsigned long long)f->monotonic_breaks,
                (unsigned long long)f->payload_hash_failures,
                (unsigned long long)f->signature_failures,
                (unsigned long long)f->authority_failures,
                (unsigned long long)f->truncated_records);
        json_string(out, f->first_failure);
        fprintf(out, ",\"event_type_counts\":{");
        bool first = true;
        for (unsigned t = 1; t < CHAIN_EVT_TYPE_COUNT; t++) {
            if (!first) fprintf(out, ",");
            first = false;
            fprintf(out, "\"%s\":%llu", event_type_name((uint8_t)t),
                    (unsigned long long)f->event_type_counts[t]);
        }
        fprintf(out, "}}%s\n", i + 1u == analysis->file_count ? "" : ",");
    }
    fprintf(out, "  ],\n");

    fprintf(out, "  \"assets\":[\n");
    for (size_t i = 0; i < analysis->asset_count; i++) {
        const asset_row_t *a = &analysis->assets[i];
        char cargo_b58[64];
        char station_b58[64];
        pub_b58(a->cargo_pub, cargo_b58);
        pub_b58(a->source_station_pubkey, station_b58);
        fprintf(out, "    {\"cargo_pub_hex\":");
        json_hex32(out, a->cargo_pub);
        fprintf(out, ",\"cargo_pub_b58\":");
        json_string(out, cargo_b58);
        fprintf(out, ",\"source_type\":");
        json_string(out, source_name(a->source));
        fprintf(out, ",\"source_file\":");
        if (a->source_file_index >= 0 &&
            (size_t)a->source_file_index < analysis->file_count) {
            json_string(out, analysis->files[a->source_file_index].path);
        } else {
            json_string(out, "");
        }
        fprintf(out, ",\"source_station_b58\":");
        json_string(out, station_b58);
        fprintf(out, ",\"source_event_id\":%llu,\"source_epoch\":%llu,"
                     "\"source_segment_id\":%llu,\"source_strict_ok\":%s,"
                     "\"world_id\":%llu,\"world_seq\":%llu,"
                     "\"mint_count\":%llu,\"transfer_count\":%llu,"
                     "\"kind\":%d,\"commodity\":%d,\"commodity_code\":",
                (unsigned long long)a->source_event_id,
                (unsigned long long)a->source_epoch,
                (unsigned long long)a->source_segment_id,
                a->source_strict_ok ? "true" : "false",
                (unsigned long long)a->source_world_id,
                (unsigned long long)a->source_world_seq,
                (unsigned long long)a->mint_count,
                (unsigned long long)a->transfer_count,
                a->kind,
                a->commodity);
        json_string(out, commodity_code_for(a->commodity));
        fprintf(out, ",\"recipe_id\":%d,\"recipe_name\":", a->recipe_id);
        json_string(out, recipe_name_for(a->recipe_id));
        fprintf(out, ",\"prefix_class\":%d,\"prefix_label\":",
                a->prefix_class);
        json_string(out, prefix_name(a->prefix_class));
        fprintf(out, ",\"mined_block\":%llu,\"parent_fragment_pub_hex\":",
                (unsigned long long)a->mined_block);
        if (a->has_parent_fragment) json_hex32(out, a->parent_fragment_pub);
        else json_string(out, "");
        fprintf(out, ",\"input_pubs_hex\":[");
        for (uint8_t j = 0; j < a->input_count; j++) {
            if (j) fprintf(out, ",");
            json_hex32(out, a->input_pubs[j]);
        }
        fprintf(out, "]}%s\n", i + 1u == analysis->asset_count ? "" : ",");
    }
    fprintf(out, "  ]\n}\n");
}

static void csv_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', out);
        fputc(*p, out);
    }
    fputc('"', out);
}

static void emit_csv(const analysis_t *analysis, FILE *out) {
    fprintf(out, "cargo_pub_hex,cargo_pub_b58,source_type,source_file,"
                 "source_station_b58,source_event_id,source_epoch,"
                 "source_segment_id,source_strict_ok,world_id,world_seq,"
                 "mint_count,transfer_count,kind,commodity,commodity_code,"
                 "recipe_id,recipe_name,prefix_class,prefix_label,mined_block,"
                 "parent_fragment_pub_hex,input_pub_1_hex,input_pub_2_hex,input_pub_3_hex\n");
    for (size_t i = 0; i < analysis->asset_count; i++) {
        const asset_row_t *a = &analysis->assets[i];
        char cargo_hex[65], cargo_b58[64], station_b58[64];
        char parent_hex[65] = "";
        char input_hex[RECIPE_INPUT_MAX][65];
        pub_hex(a->cargo_pub, cargo_hex);
        pub_b58(a->cargo_pub, cargo_b58);
        pub_b58(a->source_station_pubkey, station_b58);
        if (a->has_parent_fragment) pub_hex(a->parent_fragment_pub, parent_hex);
        for (uint8_t j = 0; j < RECIPE_INPUT_MAX; j++) {
            input_hex[j][0] = '\0';
            if (j < a->input_count) pub_hex(a->input_pubs[j], input_hex[j]);
        }
        csv_string(out, cargo_hex); fprintf(out, ",");
        csv_string(out, cargo_b58); fprintf(out, ",");
        csv_string(out, source_name(a->source)); fprintf(out, ",");
        if (a->source_file_index >= 0 &&
            (size_t)a->source_file_index < analysis->file_count)
            csv_string(out, analysis->files[a->source_file_index].path);
        else
            csv_string(out, "");
        fprintf(out, ",");
        csv_string(out, station_b58);
        fprintf(out, ",%llu,%llu,%llu,%s,%llu,%llu,%llu,%llu,%d,%d,",
                (unsigned long long)a->source_event_id,
                (unsigned long long)a->source_epoch,
                (unsigned long long)a->source_segment_id,
                a->source_strict_ok ? "true" : "false",
                (unsigned long long)a->source_world_id,
                (unsigned long long)a->source_world_seq,
                (unsigned long long)a->mint_count,
                (unsigned long long)a->transfer_count,
                a->kind,
                a->commodity);
        csv_string(out, commodity_code_for(a->commodity)); fprintf(out, ",");
        fprintf(out, "%d,", a->recipe_id);
        csv_string(out, recipe_name_for(a->recipe_id)); fprintf(out, ",");
        fprintf(out, "%d,", a->prefix_class);
        csv_string(out, prefix_name(a->prefix_class));
        fprintf(out, ",%llu,", (unsigned long long)a->mined_block);
        csv_string(out, parent_hex); fprintf(out, ",");
        csv_string(out, input_hex[0]); fprintf(out, ",");
        csv_string(out, input_hex[1]); fprintf(out, ",");
        csv_string(out, input_hex[2]); fprintf(out, "\n");
    }
}

static bool parse_args(int argc, char **argv, cli_opts_t *opts, path_list_t *paths) {
    memset(opts, 0, sizeof(*opts));
    opts->format = FORMAT_JSON;
    opts->verify_signatures = true;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(stdout);
            exit(0);
        } else if (strncmp(arg, "--format=", 9) == 0) {
            const char *v = arg + 9;
            if (strcmp(v, "json") == 0) opts->format = FORMAT_JSON;
            else if (strcmp(v, "csv") == 0) opts->format = FORMAT_CSV;
            else {
                fprintf(stderr, "signal_chain_assets: bad --format\n");
                return false;
            }
        } else if (strncmp(arg, "--out=", 6) == 0) {
            opts->out_path = arg + 6;
        } else if (strncmp(arg, "--station-pubkey=", 17) == 0) {
            opts->station_pubkey_b58 = arg + 17;
        } else if (strcmp(arg, "--no-signatures") == 0) {
            opts->verify_signatures = false;
        } else if (arg[0] == '-') {
            fprintf(stderr, "signal_chain_assets: unknown option %s\n", arg);
            return false;
        } else if (!add_input_path(paths, arg)) {
            return false;
        }
    }
    if (paths->count == 0) {
        print_usage(stderr);
        return false;
    }
    qsort(paths->items, paths->count, sizeof(paths->items[0]), path_cmp);
    return true;
}

int main(int argc, char **argv) {
    cli_opts_t opts;
    path_list_t paths = {0};
    if (!parse_args(argc, argv, &opts, &paths)) {
        path_list_free(&paths);
        return 2;
    }

    analysis_t analysis;
    memset(&analysis, 0, sizeof(analysis));
    bool ok = true;
    for (size_t i = 0; i < paths.count; i++) {
        if (!analyze_file(&analysis, &opts, paths.items[i])) {
            ok = false;
            break;
        }
    }

    FILE *out = stdout;
    if (ok && opts.out_path) {
        out = fopen(opts.out_path, "w");
        if (!out) {
            fprintf(stderr, "signal_chain_assets: cannot write %s: %s\n",
                    opts.out_path, strerror(errno));
            ok = false;
        }
    }
    if (ok) {
        if (opts.format == FORMAT_CSV) emit_csv(&analysis, out);
        else emit_json(&analysis, out);
    }
    if (out && out != stdout) fclose(out);

    free(analysis.files);
    free(analysis.assets);
    path_list_free(&paths);
    return ok ? 0 : 1;
}
