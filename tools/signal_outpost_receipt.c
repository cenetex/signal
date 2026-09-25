/*
 * signal_outpost_receipt -- prove an outpost was commissioned inside a
 * Signalspace checkpoint, and say whether play built it.
 *
 *   signal_outpost_receipt --checkpoint=<checkpoint.json> <outpost-log>
 *
 * The outpost log is read once into memory. Those exact bytes are verified
 * (signatures, linkage, payload hashes, authority), committed as a checkpoint
 * leaf, and proved against the checkpoint root with the station's inclusion
 * proof. The same bytes are then scanned for CHAIN_EVT_OUTPOST_COMMISSIONED
 * and the CONSTRUCTION events before it.
 *
 * play_earned is true when a registered player founded the outpost, a
 * player's delivery completed it, and at least SCAFFOLD_MATERIAL_NEEDED
 * distinct manifest units were consumed into the station before it was
 * commissioned. Consumers such as Forge apply their own policy to the facts.
 *
 * Exit: 0 receipt printed, 1 verification, checkpoint or commissioning
 * failure, 2 usage or input error.
 */

#include "chain_log.h"

#include "base58.h"
#include "economy_const.h"
#include "sha256.h"
#include "signal_checkpoint.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECEIPT_MAX_LOG_BYTES ((size_t)64 * 1024 * 1024)
#define RECEIPT_MAX_CHECKPOINT_BYTES ((size_t)16 * 1024 * 1024)
#define RECEIPT_MAX_CARGO 4096u

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_outpost_receipt --checkpoint=<checkpoint.json> <outpost-log>\n"
        "\n"
        "Proves the outpost's log is committed in a signal_checkpoint_v1\n"
        "checkpoint and prints a signal_outpost_receipt_v1 JSON object.\n"
        "\n"
        "Exit: 0 ok, 1 verification/checkpoint/commissioning failure, 2 input error.\n");
}

static uint8_t *read_file(const char *path, size_t limit, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = (size_t)64 * 1024, len = 0;
    uint8_t *buf = malloc(cap + 1);
    while (buf) {
        if (len == cap) {
            if (cap >= limit) { free(buf); buf = NULL; break; }
            cap = cap * 2 > limit ? limit : cap * 2;
            uint8_t *grown = realloc(buf, cap + 1);
            if (!grown) { free(buf); buf = NULL; break; }
            buf = grown;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    if (buf && ferror(f)) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[len] = 0; *out_len = len; }
    return buf;
}

static bool parse_hex32(const char *text, uint8_t out[32]) {
    for (size_t i = 0; i < 64; i++) {
        char c = text[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    for (size_t i = 0; i < 32; i++) {
        unsigned v = 0;
        for (size_t j = 0; j < 2; j++) {
            char c = text[2 * i + j];
            v = v * 16u + (unsigned)(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

/* Find `"key":"<64 hex>"` at or after `from` and before `limit`. */
static bool json_hex32(const char *from, const char *limit, const char *key, uint8_t out[32]) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *at = strstr(from, needle);
    if (!at || (limit && at >= limit)) return false;
    at += strlen(needle);
    return strlen(at) >= 64 && at[64] == '"' && parse_hex32(at, out);
}

static bool json_u64(const char *from, const char *key, uint64_t *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *at = strstr(from, needle);
    if (!at) return false;
    at += strlen(needle);
    char *end = NULL;
    unsigned long long v = strtoull(at, &end, 10);
    if (end == at) return false;
    *out = (uint64_t)v;
    return true;
}

typedef struct {
    uint8_t root[32];
    uint8_t prev_root[32];
    uint64_t station_count;
    signal_checkpoint_proof_step_t proof[SIGNAL_CHECKPOINT_PROOF_MAX];
    size_t proof_len;
} checkpoint_view_t;

/* Read the checkpoint fields and this station's proof from
 * signal_checkpoint_v1 output. */
static bool read_checkpoint(const char *json, const char *station_b58, checkpoint_view_t *out) {
    memset(out, 0, sizeof(*out));
    if (!strstr(json, "\"version\":\"signal_checkpoint_v1\"")) return false;
    const char *stations = strstr(json, "\"stations\":[");
    if (!stations) return false;
    if (!json_hex32(json, stations, "checkpoint_root", out->root) ||
        !json_hex32(json, stations, "prev_checkpoint_root", out->prev_root) ||
        !json_u64(json, "station_count", &out->station_count))
        return false;
    char needle[96];
    snprintf(needle, sizeof(needle), "\"station_pubkey\":\"%s\"", station_b58);
    const char *station = strstr(stations, needle);
    if (!station) return false;
    const char *proof = strstr(station, "\"proof\":[");
    if (!proof) return false;
    proof += strlen("\"proof\":[");
    while (*proof == '{') {
        if (out->proof_len >= SIGNAL_CHECKPOINT_PROOF_MAX) return false;
        signal_checkpoint_proof_step_t *step = &out->proof[out->proof_len++];
        if (strncmp(proof, "{\"side\":\"left\",\"hash\":\"", 23) == 0) {
            step->sibling_on_left = 1;
            proof += 23;
        } else if (strncmp(proof, "{\"side\":\"right\",\"hash\":\"", 24) == 0) {
            step->sibling_on_left = 0;
            proof += 24;
        } else {
            return false;
        }
        if (!parse_hex32(proof, step->sibling) || strncmp(proof + 64, "\"}", 2) != 0)
            return false;
        proof += 66;
        if (*proof == ',') proof++;
    }
    return *proof == ']';
}

static bool pubkey_from_filename(const char *path, uint8_t out[32], char b58[64]) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    size_t n = 0;
    while (base[n] && base[n] != '.' && n + 1u < 64u) {
        b58[n] = base[n];
        n++;
    }
    b58[n] = '\0';
    return n > 0 && base58_decode(b58, out, 32) == 32;
}

static void print_hex(const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) printf("%02x", bytes[i]);
}

static void print_pubkey_or_null(const uint8_t key[32]) {
    static const uint8_t zero[32] = {0};
    if (memcmp(key, zero, 32) == 0) {
        printf("null");
        return;
    }
    char b58[64] = {0};
    base58_encode(key, 32, b58, sizeof(b58));
    printf("\"%s\"", b58);
}

static const char *completion_name(uint8_t v) {
    switch (v) {
    case OUTPOST_COMPLETION_PLAYER_DELIVERY: return "player_delivery";
    case OUTPOST_COMPLETION_NPC_DELIVERY:    return "npc_delivery";
    case OUTPOST_COMPLETION_VIRTUAL_SUPPLY:  return "virtual_supply";
    default:                                 return "unknown";
    }
}

static const char *founder_kind_name(uint8_t v) {
    switch (v) {
    case OUTPOST_FOUNDER_NONE:              return "none";
    case OUTPOST_FOUNDER_REGISTERED_PLAYER: return "registered_player";
    case OUTPOST_FOUNDER_UNREGISTERED:      return "unregistered";
    default:                                return "unknown";
    }
}

int main(int argc, char **argv) {
    const char *checkpoint_path = NULL;
    const char *log_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strncmp(argv[i], "--checkpoint=", 13) == 0) {
            checkpoint_path = argv[i] + 13;
        } else if (argv[i][0] != '-' && !log_path) {
            log_path = argv[i];
        } else {
            print_usage(stderr);
            return 2;
        }
    }
    if (!checkpoint_path || !log_path) {
        print_usage(stderr);
        return 2;
    }

    uint8_t station_pubkey[32];
    char station_b58[64];
    if (!pubkey_from_filename(log_path, station_pubkey, station_b58)) {
        fprintf(stderr, "signal_outpost_receipt: cannot read a station pubkey from %s\n", log_path);
        return 2;
    }
    size_t log_len = 0, json_len = 0;
    uint8_t *log = read_file(log_path, RECEIPT_MAX_LOG_BYTES, &log_len);
    char *json = (char *)read_file(checkpoint_path, RECEIPT_MAX_CHECKPOINT_BYTES, &json_len);
    int status = 0;
    FILE *snapshot = NULL;
    uint8_t (*cargo)[32] = NULL;
    if (!log || !json) {
        fprintf(stderr, "signal_outpost_receipt: cannot read %s\n", !log ? log_path : checkpoint_path);
        status = 2;
        goto done;
    }

    /* Verify the in-memory bytes through an anonymous file. */
    snapshot = tmpfile();
    if (!snapshot || (log_len && fwrite(log, 1, log_len, snapshot) != log_len) ||
        fflush(snapshot) != 0 || fseek(snapshot, 0, SEEK_SET) != 0) {
        fprintf(stderr, "signal_outpost_receipt: cannot stage the log\n");
        status = 2;
        goto done;
    }
    chain_log_verify_report_t report;
    if (!chain_log_verify_with_pubkey(snapshot, station_pubkey, &report) ||
        report.valid_events != report.total_events || report.valid_bytes != log_len) {
        fprintf(stderr, "signal_outpost_receipt: %s failed verification: %s\n", log_path,
                report.first_fail_reason[0] ? report.first_fail_reason : "unknown");
        status = 1;
        goto done;
    }

    signal_checkpoint_station_t leaf_station;
    memset(&leaf_station, 0, sizeof(leaf_station));
    memcpy(leaf_station.station_pubkey, station_pubkey, 32);
    leaf_station.total_events = report.valid_events;
    leaf_station.segment_count = report.segment_count;
    leaf_station.tail_event_id = report.tail_event_id;
    memcpy(leaf_station.head_hash, report.tail_hash, 32);
    memcpy(leaf_station.log_sha256, report.valid_bytes_sha256, 32);
    leaf_station.log_bytes = report.valid_bytes;

    checkpoint_view_t checkpoint;
    if (!read_checkpoint(json, station_b58, &checkpoint)) {
        fprintf(stderr, "signal_outpost_receipt: %s is not signal_checkpoint_v1 output "
                "listing station %s\n", checkpoint_path, station_b58);
        status = 1;
        goto done;
    }
    if (!signal_checkpoint_verify_proof(&leaf_station, checkpoint.proof, checkpoint.proof_len,
                                        checkpoint.prev_root, checkpoint.station_count,
                                        checkpoint.root)) {
        fprintf(stderr, "signal_outpost_receipt: this log is not the one committed in the "
                "checkpoint\n");
        status = 1;
        goto done;
    }

    /* Scan the verified bytes. */
    cargo = calloc(RECEIPT_MAX_CARGO, 32);
    if (!cargo) {
        status = 2;
        goto done;
    }
    size_t cargo_count = 0, construction_events = 0, commissions = 0;
    chain_payload_outpost_commissioned_t commission;
    uint64_t commission_event_id = 0;
    uint8_t commission_hash[32];
    memset(&commission, 0, sizeof(commission));
    for (size_t off = 0; off < log_len;) {
        const uint8_t *hdr = log + off;
        uint16_t len = (uint16_t)(hdr[CHAIN_EVENT_HEADER_SIZE] |
                                  (hdr[CHAIN_EVENT_HEADER_SIZE + 1] << 8));
        const uint8_t *payload = hdr + CHAIN_EVENT_HEADER_SIZE + 2;
        uint8_t type = hdr[16];
        if (type == CHAIN_EVT_OUTPOST_COMMISSIONED && len == sizeof(commission)) {
            if (commissions++ == 0) {
                memcpy(&commission, payload, sizeof(commission));
                commission_event_id = 0;
                for (int b = 7; b >= 0; b--)
                    commission_event_id = (commission_event_id << 8) | hdr[8 + b];
                sha256_bytes(hdr, CHAIN_EVENT_HEADER_SIZE, commission_hash);
            }
        } else if (type == CHAIN_EVT_CONSTRUCTION && commissions == 0 &&
                   len == sizeof(chain_payload_construction_t)) {
            chain_payload_construction_t c;
            memcpy(&c, payload, sizeof(c));
            if (c.target_kind == CONSTRUCTION_TARGET_STATION) {
                construction_events++;
                bool seen = false;
                for (size_t k = 0; k < cargo_count && !seen; k++)
                    seen = memcmp(cargo[k], c.cargo_pub, 32) == 0;
                if (!seen && cargo_count < RECEIPT_MAX_CARGO)
                    memcpy(cargo[cargo_count++], c.cargo_pub, 32);
            }
        }
        off += CHAIN_EVENT_HEADER_SIZE + 2u + len;
    }
    if (commissions == 0) {
        fprintf(stderr, "signal_outpost_receipt: %s has no OUTPOST_COMMISSIONED event\n", log_path);
        status = 1;
        goto done;
    }

    size_t needed = (size_t)ceilf(SCAFFOLD_MATERIAL_NEEDED);
    bool play_earned =
        commission.founder_kind == OUTPOST_FOUNDER_REGISTERED_PLAYER &&
        commission.completion == OUTPOST_COMPLETION_PLAYER_DELIVERY &&
        cargo_count >= needed;

    printf("{\"version\":\"signal_outpost_receipt_v1\",\"outpost_pubkey\":\"%s\","
           "\"checkpoint_root\":\"", station_b58);
    print_hex(checkpoint.root, 32);
    printf("\",\"play_earned\":%s,\"commission\":{\"event_id\":%llu,\"header_hash\":\"",
           play_earned ? "true" : "false", (unsigned long long)commission_event_id);
    print_hex(commission_hash, 32);
    printf("\",\"founder_pubkey\":");
    print_pubkey_or_null(commission.founder_pubkey);
    printf(",\"founder_kind\":\"%s\",\"completion\":\"%s\",\"completed_by\":",
           founder_kind_name(commission.founder_kind), completion_name(commission.completion));
    print_pubkey_or_null(commission.completed_by_pubkey);
    printf(",\"planted_tick\":%llu,\"activated_tick\":%llu,\"commission_events\":%zu},"
           "\"labor\":{\"construction_events\":%zu,\"distinct_units\":%zu,"
           "\"units_needed\":%zu},\"station\":{\"total_events\":%llu,\"segment_count\":%llu,"
           "\"tail_event_id\":%llu,\"head_hash\":\"",
           (unsigned long long)commission.planted_tick,
           (unsigned long long)commission.activated_tick, commissions,
           construction_events, cargo_count, needed,
           (unsigned long long)leaf_station.total_events,
           (unsigned long long)leaf_station.segment_count,
           (unsigned long long)leaf_station.tail_event_id);
    print_hex(leaf_station.head_hash, 32);
    printf("\",\"log_sha256\":\"");
    print_hex(leaf_station.log_sha256, 32);
    printf("\",\"log_bytes\":%llu,\"proof\":[", (unsigned long long)leaf_station.log_bytes);
    for (size_t p = 0; p < checkpoint.proof_len; p++) {
        printf("%s{\"side\":\"%s\",\"hash\":\"", p ? "," : "",
               checkpoint.proof[p].sibling_on_left ? "left" : "right");
        print_hex(checkpoint.proof[p].sibling, 32);
        printf("\"}");
    }
    printf("]}}\n");
    if (fflush(stdout) != 0) status = 2;

done:
    if (snapshot) fclose(snapshot);
    free(cargo);
    free(log);
    free(json);
    return status;
}
