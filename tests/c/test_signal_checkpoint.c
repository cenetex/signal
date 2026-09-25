/*
 * test_signal_checkpoint.c -- Signalspace checkpoint roots.
 *
 *   1. The verify report commits to exactly the verified bytes and head.
 *   2. Checkpoint math: ordering, every leaf field, previous-root chaining.
 *   3. Inclusion proofs for every station in trees of 1..9 stations.
 *   4. The signal_checkpoint CLI agrees with the library and fails closed
 *      on a tampered log.
 */
#include "test_harness.h"

#include "chain_log.h"
#include "game_sim.h"
#include "sha256.h"
#include "signal_checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
extern FILE *popen(const char *command, const char *type);
extern int   pclose(FILE *stream);
#endif

static void cp_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_cp_%s", TMP("clog"), suffix);
    chain_log_set_dir(path);
}

static void cp_teardown(void) { chain_log_set_dir(NULL); }

static world_t *cp_world(uint32_t seed) {
    world_t *w = calloc(1, sizeof(world_t));
    if (!w) return NULL;
    w->rng = seed;
    world_reset(w);
    for (int s = 0; s < MAX_STATIONS; s++) {
        chain_log_reset(&w->stations[s]);
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }
    return w;
}

/* Read a whole file; returns its length, or -1. */
static long cp_read_file(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    return (long)got;
}

static void cp_station(signal_checkpoint_station_t *s, uint8_t fill) {
    memset(s, 0, sizeof(*s));
    memset(s->station_pubkey, fill, 32);
    s->total_events = fill;
    s->segment_count = 1;
    s->tail_event_id = fill;
    memset(s->head_hash, (uint8_t)(fill ^ 0x5a), 32);
    memset(s->log_sha256, (uint8_t)(fill ^ 0xa5), 32);
    s->log_bytes = (uint64_t)186u * fill;
}

TEST(test_checkpoint_report_commits_to_verified_bytes) {
    cp_setup("report");
    world_t *w = cp_world(61001u);
    ASSERT(w != NULL);
    station_t *st = &w->stations[0];
    uint8_t pl[24] = "checkpoint-payload";
    for (int i = 0; i < 5; i++)
        ASSERT(chain_log_emit(w, st, CHAIN_EVT_LEDGER, pl, sizeof(pl)) == (uint64_t)(i + 1));

    char path[256];
    ASSERT(chain_log_path_for(st->station_pubkey, path, sizeof(path)));
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);
    chain_log_verify_report_t r;
    ASSERT(chain_log_verify_with_pubkey(f, st->station_pubkey, &r));
    fclose(f);

    /* The head is the station's own continuation hash. */
    ASSERT(memcmp(r.tail_hash, st->chain_last_hash, 32) == 0);
    static uint8_t bytes[1 << 16];
    long len = cp_read_file(path, bytes, sizeof(bytes));
    ASSERT(len > 0);
    ASSERT_EQ_INT((int)r.valid_bytes, (int)len);
    uint8_t digest[32];
    sha256_bytes(bytes, (size_t)len, digest);
    ASSERT(memcmp(r.valid_bytes_sha256, digest, 32) == 0);

    /* An empty log commits to nothing: zero head, digest of no bytes. */
    FILE *empty = tmpfile();
    ASSERT(empty != NULL);
    ASSERT(chain_log_verify_with_pubkey(empty, st->station_pubkey, &r));
    fclose(empty);
    uint8_t zero[32] = {0};
    sha256_bytes("", 0, digest);
    ASSERT(memcmp(r.tail_hash, zero, 32) == 0);
    ASSERT(memcmp(r.valid_bytes_sha256, digest, 32) == 0);
    ASSERT_EQ_INT((int)r.valid_bytes, 0);

    free(w);
    cp_teardown();
}

TEST(test_checkpoint_root_binds_every_field_and_the_previous_root) {
    signal_checkpoint_station_t stations[3];
    uint8_t scratch[3][32];
    uint8_t prev[32] = {0};
    uint8_t stations_root[32], root[32], other_stations[32], other[32];
    for (int i = 0; i < 3; i++) cp_station(&stations[i], (uint8_t)(i + 1));
    ASSERT(signal_checkpoint_root(stations, 3, prev, scratch, stations_root, root));

    /* Deterministic. */
    ASSERT(signal_checkpoint_root(stations, 3, prev, scratch, other_stations, other));
    ASSERT(memcmp(root, other, 32) == 0);

    /* Each committed field moves the root. */
    for (int field = 0; field < 7; field++) {
        signal_checkpoint_station_t changed[3];
        memcpy(changed, stations, sizeof(changed));
        switch (field) {
        case 0: changed[1].total_events++; break;
        case 1: changed[1].segment_count++; break;
        case 2: changed[1].tail_event_id++; break;
        case 3: changed[1].head_hash[31] ^= 1; break;
        case 4: changed[1].log_sha256[0] ^= 1; break;
        case 5: changed[1].log_bytes++; break;
        default: changed[1].station_pubkey[31] ^= 1; break;
        }
        ASSERT(signal_checkpoint_root(changed, 3, prev, scratch, other_stations, other));
        ASSERT(memcmp(root, other, 32) != 0);
    }

    /* Checkpoints chain: a different previous root is a different root. */
    prev[0] = 1;
    ASSERT(signal_checkpoint_root(stations, 3, prev, scratch, other_stations, other));
    ASSERT(memcmp(stations_root, other_stations, 32) == 0);
    ASSERT(memcmp(root, other, 32) != 0);

    /* Unsorted, repeated or empty station lists are refused. */
    signal_checkpoint_station_t swapped[3] = {stations[1], stations[0], stations[2]};
    ASSERT(!signal_checkpoint_root(swapped, 3, prev, scratch, other_stations, other));
    signal_checkpoint_station_t repeated[2] = {stations[0], stations[0]};
    ASSERT(!signal_checkpoint_root(repeated, 2, prev, scratch, other_stations, other));
    ASSERT(!signal_checkpoint_root(stations, 0, prev, scratch, other_stations, other));
}

TEST(test_checkpoint_proofs_cover_every_station) {
    signal_checkpoint_station_t stations[9];
    uint8_t scratch[9][32];
    uint8_t prev[32];
    memset(prev, 7, sizeof(prev));
    for (size_t count = 1; count <= 9; count++) {
        for (size_t i = 0; i < count; i++) cp_station(&stations[i], (uint8_t)(i + 1));
        uint8_t stations_root[32], root[32];
        ASSERT(signal_checkpoint_root(stations, count, prev, scratch, stations_root, root));
        for (size_t index = 0; index < count; index++) {
            signal_checkpoint_proof_step_t proof[SIGNAL_CHECKPOINT_PROOF_MAX];
            size_t len = 0;
            ASSERT(signal_checkpoint_proof(stations, count, index, scratch, proof, &len));
            ASSERT(signal_checkpoint_verify_proof(&stations[index], proof, len, prev,
                                                  count, root));
            /* A changed station, count or sibling no longer proves. */
            signal_checkpoint_station_t forged = stations[index];
            forged.tail_event_id++;
            ASSERT(!signal_checkpoint_verify_proof(&forged, proof, len, prev, count, root));
            ASSERT(!signal_checkpoint_verify_proof(&stations[index], proof, len, prev,
                                                   count + 1, root));
            if (len > 0) {
                proof[0].sibling[0] ^= 1;
                ASSERT(!signal_checkpoint_verify_proof(&stations[index], proof, len, prev,
                                                       count, root));
            }
        }
    }
}

#ifndef _WIN32
static const char *cp_find_checkpoint_bin(void) {
    static const char *candidates[] = {
        "build-test/signal_checkpoint",
        "build-coverage/signal_checkpoint",
        "build/signal_checkpoint",
        "./signal_checkpoint",
        "../build-test/signal_checkpoint",
        "../build/signal_checkpoint",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) continue;
        fclose(f);
        return candidates[i];
    }
    return NULL;
}

/* Run the CLI; returns its exit status and copies stdout into out. */
static int cp_run(const char *cmd, char *out, size_t cap) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t got = fread(out, 1, cap - 1, p);
    out[got] = '\0';
    int status = pclose(p);
    return status == -1 ? -1 : (status >> 8) & 0xff;
}

TEST(test_checkpoint_cli_matches_library_and_fails_closed) {
    const char *bin = cp_find_checkpoint_bin();
    if (!bin) {
        TEST_WARN("signal_checkpoint binary not built; skipping CLI check");
        return;
    }
    cp_setup("cli");
    world_t *w = cp_world(61002u);
    ASSERT(w != NULL);
    uint8_t pl[16] = "cli-payload";
    char paths[2][256];
    signal_checkpoint_station_t stations[2];
    for (int s = 0; s < 2; s++) {
        station_t *st = &w->stations[s];
        for (int i = 0; i < 3 + s; i++)
            ASSERT(chain_log_emit(w, st, CHAIN_EVT_LEDGER, pl, sizeof(pl)) != 0);
        ASSERT(chain_log_path_for(st->station_pubkey, paths[s], sizeof(paths[s])));
        FILE *f = fopen(paths[s], "rb");
        ASSERT(f != NULL);
        chain_log_verify_report_t r;
        ASSERT(chain_log_verify_with_pubkey(f, st->station_pubkey, &r));
        fclose(f);
        memset(&stations[s], 0, sizeof(stations[s]));
        memcpy(stations[s].station_pubkey, st->station_pubkey, 32);
        stations[s].total_events = r.valid_events;
        stations[s].segment_count = r.segment_count;
        stations[s].tail_event_id = r.tail_event_id;
        memcpy(stations[s].head_hash, r.tail_hash, 32);
        memcpy(stations[s].log_sha256, r.valid_bytes_sha256, 32);
        stations[s].log_bytes = r.valid_bytes;
    }
    if (memcmp(stations[0].station_pubkey, stations[1].station_pubkey, 32) > 0) {
        signal_checkpoint_station_t t = stations[0];
        stations[0] = stations[1];
        stations[1] = t;
    }
    uint8_t zero[32] = {0}, scratch[2][32], stations_root[32], root[32];
    ASSERT(signal_checkpoint_root(stations, 2, zero, scratch, stations_root, root));
    /* "checkpoint_root":" (19) + 64 hex + NUL. */
    char expected[19 + 64 + 1] = "\"checkpoint_root\":\"";
    for (int i = 0; i < 32; i++)
        snprintf(expected + 19 + (size_t)i * 2u, 3, "%02x", root[i]);

    static char out[1 << 15];
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s %s 2>/dev/null", bin, paths[0], paths[1]);
    ASSERT_EQ_INT(cp_run(cmd, out, sizeof(out)), 0);
    ASSERT(strstr(out, expected) != NULL);
    /* Order of arguments does not matter. */
    snprintf(cmd, sizeof(cmd), "%s %s %s 2>/dev/null", bin, paths[1], paths[0]);
    ASSERT_EQ_INT(cp_run(cmd, out, sizeof(out)), 0);
    ASSERT(strstr(out, expected) != NULL);

    /* One flipped payload byte fails the whole checkpoint. */
    FILE *f = fopen(paths[1], "r+b");
    ASSERT(f != NULL);
    ASSERT(fseek(f, CHAIN_EVENT_HEADER_SIZE + 2, SEEK_SET) == 0);
    int c = fgetc(f);
    ASSERT(c != EOF);
    ASSERT(fseek(f, CHAIN_EVENT_HEADER_SIZE + 2, SEEK_SET) == 0);
    ASSERT(fputc(c ^ 1, f) != EOF);
    fclose(f);
    snprintf(cmd, sizeof(cmd), "%s %s %s 2>/dev/null", bin, paths[0], paths[1]);
    ASSERT_EQ_INT(cp_run(cmd, out, sizeof(out)), 1);
    ASSERT(strstr(out, "checkpoint_root") == NULL);

    free(w);
    cp_teardown();
}
#endif

void register_signal_checkpoint_tests(void);
void register_signal_checkpoint_tests(void) {
    TEST_SECTION("\n--- Signal Checkpoint ---\n");
    RUN(test_checkpoint_report_commits_to_verified_bytes);
    RUN(test_checkpoint_root_binds_every_field_and_the_previous_root);
    RUN(test_checkpoint_proofs_cover_every_station);
#ifndef _WIN32
    RUN(test_checkpoint_cli_matches_library_and_fails_closed);
#endif
}
