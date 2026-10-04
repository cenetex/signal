/*
 * signal_outpost_receipt -- prove an outpost was commissioned inside a
 * Signalspace checkpoint, and say whether play built it.
 *
 *   signal_outpost_receipt --checkpoint=<checkpoint.json> \
 *                          --expected-root=<published root hex> <outpost-log>
 *
 * The outpost log is read once into memory. Those exact bytes are verified
 * (signatures, linkage, payload hashes, authority), committed as a checkpoint
 * leaf, and proved against the checkpoint root with the station's inclusion
 * proof. The checkpoint root must equal --expected-root, the root the caller
 * got from a published source; a checkpoint file alone proves nothing.
 *
 * The same bytes are then scanned. Only the segment holding the single
 * CHAIN_EVT_OUTPOST_COMMISSIONED event counts, and within it the
 * OUTPOST_PLANTED event and the CONSTRUCTION events before the commission.
 *
 * play_earned is true when the planting record names a verified player
 * founder (fixed when the outpost was planted), a player's delivery completed
 * it, and at least SCAFFOLD_MATERIAL_NEEDED distinct units delivered by
 * players were consumed into the station. Consumers such as Forge apply
 * their own policy to the facts.
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
        "usage: signal_outpost_receipt --checkpoint=<checkpoint.json>\n"
        "                              --expected-root=<64 hex> <outpost-log>\n"
        "\n"
        "Proves the outpost's log is committed in a signal_checkpoint_v1\n"
        "checkpoint whose root is the published --expected-root, and prints a\n"
        "signal_outpost_receipt_v2 JSON object.\n"
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

/* A cursor over signal_checkpoint_v1 output. The parser accepts exactly the
 * layout signal_checkpoint prints (no whitespace, fixed key order), so a
 * crafted file cannot hide a second value for any key. */
typedef struct {
    const char *at;
} cursor_t;

static bool take(cursor_t *c, const char *literal) {
    size_t n = strlen(literal);
    if (strncmp(c->at, literal, n) != 0) return false;
    c->at += n;
    return true;
}

static bool take_hex32(cursor_t *c, uint8_t out[32]) {
    if (strlen(c->at) < 64 || !parse_hex32(c->at, out)) return false;
    c->at += 64;
    return true;
}

/* A decimal with no sign, no leading zero and no overflow. */
static bool take_u64(cursor_t *c, uint64_t *out) {
    const char *p = c->at;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return false;
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        unsigned d = (unsigned)(*p - '0');
        if (v > (UINT64_MAX - d) / 10u) return false;
        v = v * 10u + d;
        p++;
    }
    c->at = p;
    *out = v;
    return true;
}

static bool take_b58(cursor_t *c, char out[64]) {
    size_t n = 0;
    while (c->at[n] && c->at[n] != '"') {
        if (n + 1u >= 64u) return false;
        out[n] = c->at[n];
        n++;
    }
    out[n] = '\0';
    c->at += n;
    return n > 0;
}

typedef struct {
    uint8_t root[32];
    uint8_t prev_root[32];
    uint64_t station_count;
    signal_checkpoint_station_t station; /* the record listed for this outpost */
    signal_checkpoint_proof_step_t proof[SIGNAL_CHECKPOINT_PROOF_MAX];
    size_t proof_len;
} checkpoint_view_t;

/* Parse the whole checkpoint and pick out this station's record. Fails on
 * any deviation from the canonical layout, a station listed twice, or a
 * record count that disagrees with station_count. */
static bool read_checkpoint(const char *json, size_t json_len, const char *station_b58,
                            checkpoint_view_t *out) {
    memset(out, 0, sizeof(*out));
    if (strlen(json) != json_len) return false; /* embedded NUL */
    cursor_t c = {json};
    uint8_t stations_root[32];
    if (!take(&c, "{\"version\":\"signal_checkpoint_v1\",\"checkpoint_root\":\"") ||
        !take_hex32(&c, out->root) || !take(&c, "\",\"prev_checkpoint_root\":\"") ||
        !take_hex32(&c, out->prev_root) || !take(&c, "\",\"stations_root\":\"") ||
        !take_hex32(&c, stations_root) || !take(&c, "\",\"station_count\":") ||
        !take_u64(&c, &out->station_count) || !take(&c, ",\"stations\":["))
        return false;
    uint64_t records = 0;
    bool found = false;
    while (*c.at != ']') {
        if (records > 0 && !take(&c, ",")) return false;
        signal_checkpoint_station_t rec;
        signal_checkpoint_proof_step_t proof[SIGNAL_CHECKPOINT_PROOF_MAX];
        size_t proof_len = 0;
        uint8_t leaf[32];
        char b58[64];
        memset(&rec, 0, sizeof(rec));
        if (!take(&c, "{\"station_pubkey\":\"") || !take_b58(&c, b58) ||
            base58_decode(b58, rec.station_pubkey, 32) != 32 ||
            !take(&c, "\",\"total_events\":") || !take_u64(&c, &rec.total_events) ||
            !take(&c, ",\"segment_count\":") || !take_u64(&c, &rec.segment_count) ||
            !take(&c, ",\"tail_event_id\":") || !take_u64(&c, &rec.tail_event_id) ||
            !take(&c, ",\"head_hash\":\"") || !take_hex32(&c, rec.head_hash) ||
            !take(&c, "\",\"log_sha256\":\"") || !take_hex32(&c, rec.log_sha256) ||
            !take(&c, "\",\"log_bytes\":") || !take_u64(&c, &rec.log_bytes) ||
            !take(&c, ",\"leaf\":\"") || !take_hex32(&c, leaf) || !take(&c, "\",\"proof\":["))
            return false;
        while (*c.at != ']') {
            if (proof_len > 0 && !take(&c, ",")) return false;
            if (proof_len >= SIGNAL_CHECKPOINT_PROOF_MAX) return false;
            signal_checkpoint_proof_step_t *step = &proof[proof_len++];
            if (take(&c, "{\"side\":\"left\",\"hash\":\""))
                step->sibling_on_left = 1;
            else if (take(&c, "{\"side\":\"right\",\"hash\":\""))
                step->sibling_on_left = 0;
            else
                return false;
            if (!take_hex32(&c, step->sibling) || !take(&c, "\"}")) return false;
        }
        if (!take(&c, "]}")) return false;
        records++;
        if (strcmp(b58, station_b58) == 0) {
            if (found) return false;
            found = true;
            out->station = rec;
            memcpy(out->proof, proof, sizeof(proof));
            out->proof_len = proof_len;
        }
    }
    if (!take(&c, "]}")) return false;
    (void)take(&c, "\n");
    return *c.at == '\0' && found && records == out->station_count;
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

static uint64_t read_u64_le(const uint8_t *p) {
    uint64_t v = 0;
    for (int b = 7; b >= 0; b--) v = (v << 8) | p[b];
    return v;
}

int main(int argc, char **argv) {
    const char *checkpoint_path = NULL;
    const char *expected_root_hex = NULL;
    const char *log_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strncmp(argv[i], "--checkpoint=", 13) == 0) {
            checkpoint_path = argv[i] + 13;
        } else if (strncmp(argv[i], "--expected-root=", 16) == 0) {
            expected_root_hex = argv[i] + 16;
        } else if (argv[i][0] != '-' && !log_path) {
            log_path = argv[i];
        } else {
            print_usage(stderr);
            return 2;
        }
    }
    uint8_t expected_root[32];
    if (!checkpoint_path || !log_path || !expected_root_hex ||
        strlen(expected_root_hex) != 64 || !parse_hex32(expected_root_hex, expected_root)) {
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
        report.valid_events == 0 || report.valid_events != report.total_events ||
        report.valid_bytes != log_len) {
        fprintf(stderr, "signal_outpost_receipt: %s failed verification: %s\n", log_path,
                report.first_fail_reason[0] ? report.first_fail_reason
                : report.valid_events == 0 ? "no events" : "unknown");
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

    /* The checkpoint must be the published one, list this station exactly
     * once with the log's own figures, and prove it under that root. */
    checkpoint_view_t checkpoint;
    if (!read_checkpoint(json, json_len, station_b58, &checkpoint)) {
        fprintf(stderr, "signal_outpost_receipt: %s is not signal_checkpoint_v1 output "
                "listing station %s once\n", checkpoint_path, station_b58);
        status = 1;
        goto done;
    }
    if (memcmp(checkpoint.root, expected_root, 32) != 0) {
        fprintf(stderr, "signal_outpost_receipt: the checkpoint root is not the expected "
                "published root\n");
        status = 1;
        goto done;
    }
    if (memcmp(&checkpoint.station, &leaf_station, sizeof(leaf_station)) != 0 ||
        !signal_checkpoint_verify_proof(&leaf_station, checkpoint.proof, checkpoint.proof_len,
                                        checkpoint.prev_root, checkpoint.station_count,
                                        checkpoint.root)) {
        fprintf(stderr, "signal_outpost_receipt: this log is not the one committed in the "
                "checkpoint\n");
        status = 1;
        goto done;
    }

    /* Scan the verified bytes. Only the segment that holds the commission
     * counts: a restarted log cannot carry planting or frames across. */
    cargo = calloc(RECEIPT_MAX_CARGO, 32);
    if (!cargo) {
        status = 2;
        goto done;
    }
    static const uint8_t zero[32] = {0};
    size_t cargo_count = 0, player_units = 0, construction_events = 0, commissions = 0;
    size_t plantings = 0;
    chain_payload_outpost_commissioned_t commission;
    chain_payload_outpost_planted_t planted;
    uint64_t commission_event_id = 0, planted_event_id = 0;
    uint8_t commission_hash[32];
    memset(&commission, 0, sizeof(commission));
    memset(&planted, 0, sizeof(planted));
    for (size_t off = 0; off < log_len;) {
        const uint8_t *hdr = log + off;
        uint16_t len = (uint16_t)(hdr[CHAIN_EVENT_HEADER_SIZE] |
                                  (hdr[CHAIN_EVENT_HEADER_SIZE + 1] << 8));
        const uint8_t *payload = hdr + CHAIN_EVENT_HEADER_SIZE + 2;
        uint8_t type = hdr[16];
        uint64_t event_id = read_u64_le(hdr + 8);
        const size_t prev_hash_at = 24 + 32 + 32;
        bool segment_start = event_id == 1 && memcmp(hdr + prev_hash_at, zero, 32) == 0;
        if (segment_start && commissions == 0) {
            /* A new segment before the commission: forget the old one. */
            cargo_count = player_units = construction_events = plantings = 0;
            memset(&planted, 0, sizeof(planted));
        } else if (segment_start) {
            break; /* segments after the commission do not count */
        }
        if (type == CHAIN_EVT_OUTPOST_COMMISSIONED) {
            if (len != sizeof(commission) || commissions++ > 0) {
                commissions = 2;
                break;
            }
            memcpy(&commission, payload, sizeof(commission));
            commission_event_id = event_id;
            sha256_bytes(hdr, CHAIN_EVENT_HEADER_SIZE, commission_hash);
        } else if (type == CHAIN_EVT_OUTPOST_PLANTED && commissions == 0) {
            if (len != sizeof(planted) || plantings++ > 0) {
                plantings = 2;
            } else {
                memcpy(&planted, payload, sizeof(planted));
                planted_event_id = event_id;
            }
        } else if (type == CHAIN_EVT_CONSTRUCTION && commissions == 0 &&
                   (len == sizeof(chain_payload_construction_t) ||
                    len == sizeof(chain_payload_construction_player_t))) {
            /* A player's delivery appends the player's pubkey; the leading
             * 56 bytes are the same in both forms. */
            chain_payload_construction_t c;
            memcpy(&c, payload, sizeof(c));
            if (c.target_kind == CONSTRUCTION_TARGET_STATION) {
                construction_events++;
                bool seen = false;
                for (size_t k = 0; k < cargo_count && !seen; k++)
                    seen = memcmp(cargo[k], c.cargo_pub, 32) == 0;
                if (!seen && cargo_count < RECEIPT_MAX_CARGO) {
                    memcpy(cargo[cargo_count++], c.cargo_pub, 32);
                    if (c.deliverer == CONSTRUCTION_DELIVERER_PLAYER) player_units++;
                }
            }
        }
        off += CHAIN_EVENT_HEADER_SIZE + 2u + len;
    }
    if (commissions != 1) {
        fprintf(stderr, "signal_outpost_receipt: %s has %s OUTPOST_COMMISSIONED event\n",
                log_path, commissions == 0 ? "no" : "more than one");
        status = 1;
        goto done;
    }

    /* The planting record must be unique and name the same founder and
     * tick as the commission. */
    bool planted_ok = plantings == 1 &&
        memcmp(planted.founder_pubkey, commission.founder_pubkey, 32) == 0 &&
        planted.planted_tick == commission.planted_tick;
    uint8_t founder_kind = planted_ok ? planted.founder_kind : OUTPOST_FOUNDER_NONE;
    size_t needed = (size_t)ceilf(SCAFFOLD_MATERIAL_NEEDED);
    bool play_earned =
        founder_kind == OUTPOST_FOUNDER_REGISTERED_PLAYER &&
        commission.completion == OUTPOST_COMPLETION_PLAYER_DELIVERY &&
        player_units >= needed;

    printf("{\"version\":\"signal_outpost_receipt_v2\",\"outpost_pubkey\":\"%s\","
           "\"checkpoint_root\":\"", station_b58);
    print_hex(checkpoint.root, 32);
    printf("\",\"play_earned\":%s,\"planted\":", play_earned ? "true" : "false");
    if (planted_ok)
        printf("{\"event_id\":%llu,\"founder_kind\":\"%s\"}",
               (unsigned long long)planted_event_id, founder_kind_name(founder_kind));
    else
        printf("null");
    printf(",\"commission\":{\"event_id\":%llu,\"header_hash\":\"",
           (unsigned long long)commission_event_id);
    print_hex(commission_hash, 32);
    printf("\",\"founder_pubkey\":");
    print_pubkey_or_null(commission.founder_pubkey);
    printf(",\"completion\":\"%s\",\"completed_by\":", completion_name(commission.completion));
    print_pubkey_or_null(commission.completed_by_pubkey);
    printf(",\"planted_tick\":%llu,\"activated_tick\":%llu},"
           "\"labor\":{\"construction_events\":%zu,\"distinct_units\":%zu,"
           "\"player_units\":%zu,\"units_needed\":%zu},\"station\":{\"total_events\":%llu,"
           "\"segment_count\":%llu,\"tail_event_id\":%llu,\"head_hash\":\"",
           (unsigned long long)commission.planted_tick,
           (unsigned long long)commission.activated_tick,
           construction_events, cargo_count, player_units, needed,
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
