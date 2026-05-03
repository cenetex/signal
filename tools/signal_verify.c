/*
 * signal_verify -- standalone post-mortem chain-log watchdog
 * (Layer E of #479).
 *
 * Independently verifies a station's signed event chain log without
 * needing a live world_t. Anyone with a copy of the log file (and the
 * station's pubkey, which is encoded in the log filename) can confirm
 * signatures, prev_hash linkage, monotonic event_id, and a small set
 * of cross-event invariants.
 *
 * This is the prototype of the verifier client that #480's Solana
 * programs will eventually be able to delegate to. It is deliberately
 * kept small and dependency-light: tweetnacl + SHA-256 + base58.
 *
 * Out of scope, by design (see #479 Layer E spec):
 *   - replay-the-sim mode (rerun world ticks and assert convergence)
 *   - cross-server divergence detection (Layer D + federation)
 *   - on-chain anchor cross-check (Layer F / #480)
 *   - GUI / web UI (text + JSON only)
 */

#include "chain_log.h"

#include "base58.h"
#include "sha256.h"
#include "signal_crypto.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* CLI options                                                         */
/* ------------------------------------------------------------------ */

typedef enum { REPORT_TEXT, REPORT_JSON } report_fmt_t;

/* Bitmask of optional cross-event invariants. The signature + linkage
 * + monotonic checks are baked into chain_log_verify_with_pubkey()
 * itself; the bits here are *additional* predicates the tool can
 * apply on a second pass. */
enum {
    INV_SIGNATURES           = 1 << 0,  /* baked-in */
    INV_LINKAGE              = 1 << 1,  /* baked-in */
    INV_MONOTONIC_EVENT_ID   = 1 << 2,  /* baked-in */
    INV_SMELT_INPUT_CONSUMED = 1 << 3,
    INV_TRANSFER_BALANCED    = 1 << 4,
    INV_NO_ORPHAN_EVENTS     = 1 << 5,
    /* Every EVT_FRAGMENT_TOW for fragment F should be paired by a
     * subsequent EVT_FRAGMENT_RELEASE, EVT_SMELT, or EVT_ROCK_DESTROY
     * for F at the same station. Unpaired TOWs ("orphan tows") signal
     * either a sim bug, log truncation, or — most often — a fragment
     * that was towed in this station's signal range but resolved at a
     * different station (cross-station tow → smelt). The verifier
     * counts orphans without failing the report; a high count is a
     * weak warning, not a hard error. */
    INV_TOWER_CONSISTENT     = 1 << 6,
    INV_ALL                  = 0x7F,
};

typedef struct {
    report_fmt_t  fmt;
    uint32_t      invariants;
    bool          strict;
    uint64_t      since;     /* 0 = no lower bound */
    uint64_t      until;     /* 0 = no upper bound */
    bool          since_set;
    bool          until_set;
    bool          multi_station;
    bool          dump_text; /* print operator_post text to stdout */
    const char   *registry_path;
    const char   *station_pubkey_b58; /* explicit override */
} cli_opts_t;

static void print_usage(FILE *out) {
    fprintf(out,
        "usage: signal_verify [options] <chain-log-path>...\n"
        "\n"
        "Options:\n"
        "  --station-pubkey=<base58>   Override pubkey (default: from filename)\n"
        "  --report=<json|text>        Output format (default: text)\n"
        "  --invariants=<list>         Comma-separated subset of:\n"
        "                                signatures, linkage, monotonic_event_id,\n"
        "                                smelt_input_consumed, transfer_balanced,\n"
        "                                no_orphan_events, tower_chain_consistent,\n"
        "                                all (default: all)\n"
        "  --strict                    Treat warnings as errors\n"
        "  --since=<epoch>             Only events at or after sim tick\n"
        "  --until=<epoch>             Only events up to sim tick\n"
        "  --dump-text                 Print decoded text from OPERATOR_POST events\n"
        "  --multi-station             Walk all logs together; check\n"
        "                              cross-station invariants\n"
        "  --registry=<file>           Optional pubkey -> name mapping\n"
        "  -h, --help                  This message\n"
        "\n"
        "Exit: 0 ok, 1 verification failure, 2 malformed input.\n");
}

static bool parse_invariant_list(const char *s, uint32_t *out) {
    *out = 0;
    const char *p = s;
    while (*p) {
        const char *q = p;
        while (*q && *q != ',') q++;
        size_t n = (size_t)(q - p);
        if (n == 3 && strncmp(p, "all", 3) == 0)                 *out |= INV_ALL;
        else if (n == 10 && strncmp(p, "signatures", 10) == 0)   *out |= INV_SIGNATURES;
        else if (n == 7 && strncmp(p, "linkage", 7) == 0)        *out |= INV_LINKAGE;
        else if (n == 19 && strncmp(p, "monotonic_event_id", 18) == 0)
            *out |= INV_MONOTONIC_EVENT_ID;
        else if (n == 18 && strncmp(p, "monotonic_event_id", 18) == 0)
            *out |= INV_MONOTONIC_EVENT_ID;
        else if (n == 20 && strncmp(p, "smelt_input_consumed", 20) == 0)
            *out |= INV_SMELT_INPUT_CONSUMED;
        else if (n == 17 && strncmp(p, "transfer_balanced", 17) == 0)
            *out |= INV_TRANSFER_BALANCED;
        else if (n == 16 && strncmp(p, "no_orphan_events", 16) == 0)
            *out |= INV_NO_ORPHAN_EVENTS;
        else if (n == 22 && strncmp(p, "tower_chain_consistent", 22) == 0)
            *out |= INV_TOWER_CONSISTENT;
        else {
            fprintf(stderr, "signal_verify: unknown invariant '%.*s'\n", (int)n, p);
            return false;
        }
        if (*q == 0) break;
        p = q + 1;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Registry (optional pubkey -> friendly name map)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t pubkey[32];
    char    name[64];
} registry_entry_t;

#define MAX_REGISTRY 32
static registry_entry_t g_registry[MAX_REGISTRY];
static size_t g_registry_count = 0;

static void registry_load(const char *path) {
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "signal_verify: --registry: cannot open %s\n", path);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && g_registry_count < MAX_REGISTRY) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char b58[80] = {0};
        char name[64] = {0};
        if (sscanf(line, "%79s %63[^\n]", b58, name) >= 1) {
            uint8_t pub[32];
            if (base58_decode(b58, pub, 32) == 32) {
                memcpy(g_registry[g_registry_count].pubkey, pub, 32);
                snprintf(g_registry[g_registry_count].name,
                         sizeof(g_registry[g_registry_count].name),
                         "%s", name);
                g_registry_count++;
            }
        }
    }
    fclose(f);
}

static const char *registry_name(const uint8_t pubkey[32]) {
    for (size_t i = 0; i < g_registry_count; i++) {
        if (memcmp(g_registry[i].pubkey, pubkey, 32) == 0)
            return g_registry[i].name;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Pubkey discovery from filename                                      */
/* ------------------------------------------------------------------ */

/* Extract the leaf base58 stem from "<dir>/<b58>.log". Returns true on
 * success and writes the pubkey bytes (always 32). */
static bool pubkey_from_filename(const char *path, uint8_t out[32], char *out_b58, size_t out_b58_cap) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    /* Strip ".log" suffix if present. */
    char stem[80] = {0};
    size_t n = 0;
    while (base[n] && base[n] != '.' && n < sizeof(stem) - 1) {
        stem[n] = base[n];
        n++;
    }
    if (n == 0) return false;
    if (out_b58 && out_b58_cap > n) {
        memcpy(out_b58, stem, n + 1);
    }
    return base58_decode(stem, out, 32) == 32;
}

/* ------------------------------------------------------------------ */
/* Cross-event invariants (single station)                             */
/* ------------------------------------------------------------------ */

#define MAX_TRACKED_PUBS 8192

typedef struct {
    /* Registries of cargo_pubs / fragment_pubs / rock_pubs that have
     * been observed as outputs. Used to gate transfer_balanced and
     * no_orphan_events. */
    uint8_t cargo_outputs[MAX_TRACKED_PUBS][32];
    size_t  cargo_output_count;
    uint8_t consumed_fragments[MAX_TRACKED_PUBS][32];
    size_t  consumed_fragment_count;
    uint8_t destroyed_rocks[MAX_TRACKED_PUBS][32];
    size_t  destroyed_rock_count;
    /* Fragments that were towed (FRAGMENT_TOW seen) but not yet
     * resolved by a matching FRAGMENT_RELEASE / SMELT / ROCK_DESTROY.
     * Anything left in the set after the walk completes is an "orphan
     * tow" — counted, not fatal. */
    uint8_t pending_tows[MAX_TRACKED_PUBS][32];
    size_t  pending_tow_count;

    uint64_t inv_smelt_violations;
    uint64_t inv_transfer_violations;
    uint64_t inv_orphan_violations;
    uint64_t inv_tower_violations;
} inv_state_t;

static bool pub_is_zero(const uint8_t pub[32]) {
    for (int i = 0; i < 32; i++) if (pub[i]) return false;
    return true;
}

static bool pub_set_contains(const uint8_t (*set)[32], size_t count, const uint8_t pub[32]) {
    for (size_t i = 0; i < count; i++) if (memcmp(set[i], pub, 32) == 0) return true;
    return false;
}

static void pub_set_add(uint8_t (*set)[32], size_t *count, const uint8_t pub[32]) {
    if (*count >= MAX_TRACKED_PUBS) return;
    if (pub_set_contains((const uint8_t (*)[32])set, *count, pub)) return;
    memcpy(set[*count], pub, 32);
    (*count)++;
}

/* Remove pub from set if present. Returns true if removed (i.e., was
 * in the set). Order-preservation isn't important — we tail-swap. */
static bool pub_set_remove(uint8_t (*set)[32], size_t *count, const uint8_t pub[32]) {
    for (size_t i = 0; i < *count; i++) {
        if (memcmp(set[i], pub, 32) != 0) continue;
        if (i + 1 < *count) memcpy(set[i], set[*count - 1], 32);
        (*count)--;
        return true;
    }
    return false;
}

/* Dump operator_post text entries from the log if --dump-text was specified. */
static void dump_operator_post_text(const char *path,
                                    uint64_t since, bool since_set,
                                    uint64_t until, bool until_set) {
    FILE *f = fopen(path, "rb");
    if (!f) return;

    for (;;) {
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, CHAIN_EVENT_HEADER_SIZE, f);
        if (got != CHAIN_EVENT_HEADER_SIZE) break;
        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, f) != 1) break;
        uint8_t payload[4096];
        if (plen > sizeof(payload)) {
            fclose(f);
            return;
        }
        if (plen > 0 && fread(payload, plen, 1, f) != 1) break;

        /* Extract epoch from header (first 8 bytes) */
        uint64_t epoch = 0;
        for (int i = 0; i < 8; i++) epoch |= (uint64_t)hdr_bytes[i] << (i * 8);
        /* Extract type at offset 16 */
        uint8_t type = hdr_bytes[16];

        if (since_set && epoch < since) continue;
        if (until_set && epoch > until) continue;

        if (type == CHAIN_EVT_OPERATOR_POST && plen >= 38) {
            uint8_t kind = payload[0];
            uint8_t tier = payload[1];
            uint16_t ref_id = payload[2] | ((uint16_t)payload[3] << 8);
            uint16_t text_len = payload[36] | ((uint16_t)payload[37] << 8);

            if (text_len > 0 && 38 + text_len <= (uint16_t)plen) {
                const uint8_t *text_ptr = &payload[38];
                printf("[OPERATOR_POST] kind=%u tier=%u ref_id=%u: ",
                       (unsigned)kind, (unsigned)tier, (unsigned)ref_id);
                fwrite(text_ptr, 1, text_len, stdout);
                printf("\n");
            }
        }
    }
    fclose(f);
}

/* Walk the log a second time and apply optional invariants. The
 * baked-in checks (signatures, linkage, monotonic) have already been
 * validated; this pass is just for the semantic predicates. */
static bool apply_invariants(const char *path,
                             uint32_t invariants,
                             const cli_opts_t *opts,
                             inv_state_t *st,
                             char *fail_reason, size_t fail_cap) {
    if ((invariants &
        (INV_SMELT_INPUT_CONSUMED | INV_TRANSFER_BALANCED | INV_NO_ORPHAN_EVENTS |
         INV_TOWER_CONSISTENT)) == 0)
        return true;

    FILE *f = fopen(path, "rb");
    if (!f) return true;
    bool ok = true;

    for (;;) {
        uint8_t hdr_bytes[CHAIN_EVENT_HEADER_SIZE];
        size_t got = fread(hdr_bytes, 1, CHAIN_EVENT_HEADER_SIZE, f);
        if (got != CHAIN_EVENT_HEADER_SIZE) break;
        uint16_t plen = 0;
        if (fread(&plen, sizeof(plen), 1, f) != 1) break;
        uint8_t payload[4096];
        if (plen > sizeof(payload)) break;
        if (plen > 0 && fread(payload, plen, 1, f) != 1) break;

        /* The header bytes we care about: epoch (8) + event_id (8) +
         * type at offset 16. We don't need the full unpack here. */
        uint64_t epoch = 0;
        for (int i = 0; i < 8; i++) epoch |= (uint64_t)hdr_bytes[i] << (i * 8);
        uint8_t type = hdr_bytes[16];

        if (opts->since_set && epoch < opts->since) continue;
        if (opts->until_set && epoch > opts->until) continue;

        switch (type) {
        case CHAIN_EVT_ROCK_DESTROY:
            /* payload begins with rock_pub[32]. */
            if (plen >= 32) {
                pub_set_add(st->destroyed_rocks, &st->destroyed_rock_count, payload);
                /* A rock_pub destroy resolves any outstanding tow for
                 * the same fragment_pub (rock and fragment share the
                 * same content hash). */
                if (invariants & INV_TOWER_CONSISTENT) {
                    pub_set_remove(st->pending_tows, &st->pending_tow_count, payload);
                }
            }
            break;
        case CHAIN_EVT_SMELT: {
            /* payload: fragment_pub[32], ingot_pub[32], prefix_class, pad[7], mined_block u64.
             * Hopper-path smelts emit fragment_pub == zero — that's not
             * a violation, just a "no fragment was consumed by this
             * smelt" signal. We register the ingot_pub as a cargo
             * output. */
            if (plen >= 64) {
                const uint8_t *frag = payload;
                const uint8_t *ingot = payload + 32;
                if (!pub_is_zero(frag)) {
                    if (invariants & INV_SMELT_INPUT_CONSUMED) {
                        if (pub_set_contains((const uint8_t (*)[32])st->consumed_fragments,
                                             st->consumed_fragment_count, frag)) {
                            st->inv_smelt_violations++;
                            if (ok && fail_cap > 0) {
                                snprintf(fail_reason, fail_cap,
                                         "smelt_input_consumed: fragment double-smelted");
                                ok = false;
                            }
                        }
                        pub_set_add(st->consumed_fragments,
                                    &st->consumed_fragment_count, frag);
                    }
                    /* Productive end of a tow — clear any pending TOW
                     * for this fragment_pub. */
                    if (invariants & INV_TOWER_CONSISTENT) {
                        pub_set_remove(st->pending_tows, &st->pending_tow_count, frag);
                    }
                }
                pub_set_add(st->cargo_outputs, &st->cargo_output_count, ingot);
            }
            break;
        }
        case CHAIN_EVT_CRAFT:
            /* payload begins with output ingot_pub[32]. */
            if (plen >= 32)
                pub_set_add(st->cargo_outputs, &st->cargo_output_count, payload);
            break;
        case CHAIN_EVT_TRANSFER: {
            /* payload: from_pubkey[32], to_pubkey[32], cargo_pub[32], kind, pad. */
            if (plen >= 96) {
                const uint8_t *cargo = payload + 64;
                if (invariants & INV_TRANSFER_BALANCED) {
                    if (!pub_set_contains((const uint8_t (*)[32])st->cargo_outputs, st->cargo_output_count, cargo)) {
                        st->inv_transfer_violations++;
                        if (ok && fail_cap > 0) {
                            snprintf(fail_reason, fail_cap,
                                     "transfer_balanced: cargo with no upstream output");
                            ok = false;
                        }
                    }
                }
                if (invariants & INV_NO_ORPHAN_EVENTS) {
                    if (!pub_set_contains((const uint8_t (*)[32])st->cargo_outputs, st->cargo_output_count, cargo)) {
                        st->inv_orphan_violations++;
                    }
                }
                /* A transfer also re-publishes the cargo_pub as a
                 * downstream provenance source. */
                pub_set_add(st->cargo_outputs, &st->cargo_output_count, cargo);
            }
            break;
        }
        case CHAIN_EVT_FRAGMENT_TOW:
            /* payload begins with fragment_pub[32]. Add to pending set;
             * a later RELEASE/SMELT/ROCK_DESTROY will remove it. */
            if (plen >= 32 && (invariants & INV_TOWER_CONSISTENT)) {
                pub_set_add(st->pending_tows, &st->pending_tow_count, payload);
            }
            break;
        case CHAIN_EVT_FRAGMENT_RELEASE:
            /* payload begins with fragment_pub[32]. Resolve any
             * pending TOW for this fragment. Stray RELEASE without
             * matching TOW is silently ignored (could legitimately
             * happen if the TOW was at a different station). */
            if (plen >= 32 && (invariants & INV_TOWER_CONSISTENT)) {
                pub_set_remove(st->pending_tows, &st->pending_tow_count, payload);
            }
            break;
        case CHAIN_EVT_TRADE:
        case CHAIN_EVT_LEDGER:
        case CHAIN_EVT_OPERATOR_POST:
            /* No invariants to check on these event types — the report
             * still counts them via event_type_counts (incremented
             * earlier in the loop) so a verifier sees the per-type
             * histogram. */
            break;
        default:
            break;
        }
    }
    fclose(f);

    /* Anything still in pending_tows after the walk is a TOW with no
     * resolution at this station. Count as a violation but don't fail
     * the report — orphan tows can legitimately occur when a player
     * tows in this station's range and resolves at a different one
     * (cross-station scenario). A high orphan rate is a soft signal,
     * not a hard error. */
    if (invariants & INV_TOWER_CONSISTENT) {
        st->inv_tower_violations += (uint64_t)st->pending_tow_count;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Output                                                              */
/* ------------------------------------------------------------------ */

static const char *type_name(unsigned t) {
    switch (t) {
    case CHAIN_EVT_SMELT:            return "SMELT";
    case CHAIN_EVT_CRAFT:            return "CRAFT";
    case CHAIN_EVT_TRANSFER:         return "TRANSFER";
    case CHAIN_EVT_TRADE:            return "TRADE";
    case CHAIN_EVT_LEDGER:           return "LEDGER";
    case CHAIN_EVT_ROCK_DESTROY:     return "ROCK_DESTROY";
    case CHAIN_EVT_OPERATOR_POST:    return "OPERATOR_POST";
    case CHAIN_EVT_FRAGMENT_TOW:     return "FRAGMENT_TOW";
    case CHAIN_EVT_FRAGMENT_RELEASE: return "FRAGMENT_RELEASE";
    default:                         return "UNKNOWN";
    }
}

static void print_text(const char *path,
                       const char *station_b58,
                       const char *station_name,
                       const chain_log_verify_report_t *r,
                       const inv_state_t *inv,
                       bool ok) {
    printf("=== Verifying %s%s%s%s ===\n",
           path,
           station_name ? " (" : "",
           station_name ? station_name : "",
           station_name ? ")" : "");
    if (!station_name && station_b58) {
        printf("station:          %s\n", station_b58);
    }
    printf("events:           %llu\n", (unsigned long long)r->total_events);
    printf("valid:            %llu\n", (unsigned long long)r->valid_events);
    printf("bad signatures:   %llu\n", (unsigned long long)r->bad_signatures);
    printf("bad linkage:      %llu\n", (unsigned long long)r->bad_linkage);
    printf("bad payload_hash: %llu\n", (unsigned long long)r->bad_payload_hash);
    printf("bad authority:    %llu\n", (unsigned long long)r->bad_authority);
    printf("monotonic viol.:  %llu\n", (unsigned long long)r->monotonic_violations);
    printf("event types:");
    for (unsigned t = 1; t < CHAIN_EVT_TYPE_COUNT; t++) {
        printf(" %s=%llu", type_name(t),
               (unsigned long long)r->event_type_counts[t]);
    }
    printf("\n");
    if (inv) {
        if (inv->inv_smelt_violations)
            printf("smelt invariant viol: %llu\n", (unsigned long long)inv->inv_smelt_violations);
        if (inv->inv_transfer_violations)
            printf("transfer invariant viol: %llu\n", (unsigned long long)inv->inv_transfer_violations);
        if (inv->inv_orphan_violations)
            printf("orphan invariant viol: %llu\n", (unsigned long long)inv->inv_orphan_violations);
        if (inv->inv_tower_violations)
            printf("tower chain orphans:  %llu  (TOW unmatched by SMELT/RELEASE/ROCK_DESTROY at this station)\n",
                   (unsigned long long)inv->inv_tower_violations);
    }
    if (!ok && r->first_fail_reason[0]) {
        printf("first failure:    %s\n", r->first_fail_reason);
    }
    printf("status: %s\n", ok ? "OK" : "FAIL");
}

static void print_json(const char *path,
                       const char *station_b58,
                       const char *station_name,
                       const chain_log_verify_report_t *r,
                       const inv_state_t *inv,
                       bool ok) {
    printf("{");
    printf("\"log\":\"%s\",", path);
    printf("\"station_pubkey\":\"%s\",", station_b58 ? station_b58 : "");
    printf("\"station_name\":\"%s\",", station_name ? station_name : "");
    printf("\"total_events\":%llu,", (unsigned long long)r->total_events);
    printf("\"valid_events\":%llu,", (unsigned long long)r->valid_events);
    printf("\"bad_signatures\":%llu,", (unsigned long long)r->bad_signatures);
    printf("\"bad_linkage\":%llu,", (unsigned long long)r->bad_linkage);
    printf("\"bad_payload_hash\":%llu,", (unsigned long long)r->bad_payload_hash);
    printf("\"bad_authority\":%llu,", (unsigned long long)r->bad_authority);
    printf("\"monotonic_violations\":%llu,", (unsigned long long)r->monotonic_violations);
    printf("\"event_type_counts\":{");
    bool first = true;
    for (unsigned t = 1; t < CHAIN_EVT_TYPE_COUNT; t++) {
        if (!first) printf(",");
        printf("\"%s\":%llu", type_name(t),
               (unsigned long long)r->event_type_counts[t]);
        first = false;
    }
    printf("},");
    printf("\"smelt_invariant_violations\":%llu,",
           (unsigned long long)(inv ? inv->inv_smelt_violations : 0));
    printf("\"transfer_invariant_violations\":%llu,",
           (unsigned long long)(inv ? inv->inv_transfer_violations : 0));
    printf("\"orphan_invariant_violations\":%llu,",
           (unsigned long long)(inv ? inv->inv_orphan_violations : 0));
    printf("\"tower_chain_violations\":%llu,",
           (unsigned long long)(inv ? inv->inv_tower_violations : 0));
    printf("\"status\":\"%s\"", ok ? "OK" : "FAIL");
    printf("}\n");
}

/* ------------------------------------------------------------------ */
/* Per-log driver                                                      */
/* ------------------------------------------------------------------ */

static int verify_one(const char *path, const cli_opts_t *opts) {
    uint8_t pubkey[32];
    char b58[80] = {0};
    bool have_pubkey = false;

    if (opts->station_pubkey_b58) {
        if (base58_decode(opts->station_pubkey_b58, pubkey, 32) != 32) {
            fprintf(stderr, "signal_verify: invalid --station-pubkey\n");
            return 2;
        }
        snprintf(b58, sizeof(b58), "%s", opts->station_pubkey_b58);
        have_pubkey = true;
    }
    if (!have_pubkey) {
        if (!pubkey_from_filename(path, pubkey, b58, sizeof(b58))) {
            fprintf(stderr, "signal_verify: cannot derive pubkey from filename '%s'\n", path);
            return 2;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "signal_verify: cannot open %s\n", path);
        return 2;
    }

    chain_log_verify_report_t report;
    bool ok = chain_log_verify_with_pubkey(f, pubkey, &report);
    fclose(f);

    inv_state_t inv = {0};
    char inv_fail[128] = {0};
    bool inv_ok = apply_invariants(path, opts->invariants, opts, &inv,
                                   inv_fail, sizeof(inv_fail));
    if (!inv_ok && opts->strict) ok = false;

    if (opts->dump_text) {
        dump_operator_post_text(path, opts->since, opts->since_set,
                                opts->until, opts->until_set);
    }

    const char *name = registry_name(pubkey);
    if (opts->fmt == REPORT_JSON)
        print_json(path, b58, name, &report, &inv, ok);
    else
        print_text(path, b58, name, &report, &inv, ok);

    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    cli_opts_t opts = {
        .fmt        = REPORT_TEXT,
        .invariants = INV_ALL,
        .strict     = false,
    };

    int positional_start = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strncmp(a, "--report=", 9) == 0) {
            const char *v = a + 9;
            if (strcmp(v, "json") == 0) opts.fmt = REPORT_JSON;
            else if (strcmp(v, "text") == 0) opts.fmt = REPORT_TEXT;
            else { fprintf(stderr, "signal_verify: bad --report\n"); return 2; }
        } else if (strncmp(a, "--invariants=", 13) == 0) {
            if (!parse_invariant_list(a + 13, &opts.invariants)) return 2;
        } else if (strncmp(a, "--station-pubkey=", 17) == 0) {
            opts.station_pubkey_b58 = a + 17;
        } else if (strncmp(a, "--since=", 8) == 0) {
            opts.since = strtoull(a + 8, NULL, 10);
            opts.since_set = true;
        } else if (strncmp(a, "--until=", 8) == 0) {
            opts.until = strtoull(a + 8, NULL, 10);
            opts.until_set = true;
        } else if (strcmp(a, "--strict") == 0) {
            opts.strict = true;
        } else if (strcmp(a, "--dump-text") == 0) {
            opts.dump_text = true;
        } else if (strcmp(a, "--multi-station") == 0) {
            opts.multi_station = true;
        } else if (strncmp(a, "--registry=", 11) == 0) {
            opts.registry_path = a + 11;
        } else if (a[0] == '-') {
            fprintf(stderr, "signal_verify: unknown option '%s'\n", a);
            print_usage(stderr);
            return 2;
        } else {
            positional_start = i;
            break;
        }
    }

    if (positional_start >= argc) {
        print_usage(stderr);
        return 2;
    }

    if (opts.registry_path) registry_load(opts.registry_path);

    int worst = 0;
    /* Multi-station mode: verify each log independently first; if all
     * pass, do a unified second pass for cross-station invariants
     * (currently TRANSFER provenance — same cargo_pub appearing on
     * sender and receiver logs). */
    for (int i = positional_start; i < argc; i++) {
        int rc = verify_one(argv[i], &opts);
        if (rc > worst) worst = rc;
    }

    if (opts.multi_station && worst == 0 && argc - positional_start >= 2) {
        /* Unified provenance set across all logs. Any TRANSFER whose
         * cargo_pub doesn't appear as an output anywhere is flagged. */
        inv_state_t merged = {0};
        cli_opts_t inv_opts = opts;
        char fail[128] = {0};
        bool inv_ok = true;
        /* First, collect outputs across all logs (SMELT/CRAFT/TRANSFER produce). */
        uint32_t inv_passes[2] = {
            INV_SMELT_INPUT_CONSUMED | INV_TRANSFER_BALANCED,
            opts.invariants
        };
        (void)inv_passes;
        for (int i = positional_start; i < argc; i++) {
            (void)apply_invariants(argv[i], opts.invariants, &inv_opts, &merged,
                                   fail, sizeof(fail));
        }
        /* If aggregate transfer violations remain, the cross-station
         * pass also fails. */
        if (merged.inv_transfer_violations > 0) {
            inv_ok = false;
            fprintf(stderr, "signal_verify: cross-station provenance: %llu transfer(s) lack upstream output\n",
                    (unsigned long long)merged.inv_transfer_violations);
        }
        if (!inv_ok) worst = 1;
    }

    return worst;
}
