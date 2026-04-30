#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "math_util.h"
#include "types.h"
#include "commodity.h"
#include "ship.h"
#include "economy.h"
#include "game_sim.h"
#include "sim_asteroid.h"
#include "sim_production.h"
#include "sim_catalog.h"
#include "sim_nav.h"
#include "sim_autopilot.h"
#include "sim_flight.h"
#include "sim_ai.h"
#include "chunk.h"
#include "net_protocol.h"
#include "mining.h"
#include "manifest.h"
#include "sha256.h"

extern int tests_run;
extern int tests_passed;
extern int tests_failed;

/* Shard filtering so N workers can fan out the suite via xargs -P N.
 * Each RUN() bumps a sequential index; only indices where
 * (index % g_shard_total) == g_shard_index actually execute. When
 * unset (default), all tests run. */
extern int g_shard_total;
extern int g_shard_index;
extern int g_test_seq;

/* Quiet mode: suppress per-test "ok" lines, section banners, and inline
 * [WARN] noise. ASSERT failures still print full file:line context, and
 * the post-test summary always prints. Set with --quiet on the cmdline.
 * Default Makefile `make test` enables it; override with TEST_VERBOSE=1. */
extern int g_quiet;
extern int g_warnings;

/* Substring filter: when non-NULL, only tests whose name (the literal
 * stringified token passed to RUN) contains this substring run. Set
 * with --filter=<pat> on the command line. Composes with --shard: the
 * filter check happens BEFORE the shard mod check, so filtered tests
 * don't burn shard slots and `--filter=foo --shard=K/N` runs the Nth
 * slice of matching tests, not the Nth slice of the full suite. */
extern const char *g_filter;

/* Soak gate. Tests that run multi-second sim scenarios (multi-player
 * autopilot, e2e contract lifecycle, multi-thousand-tick conservation
 * runs) account for ~75% of the suite's wall-clock and are tagged with
 * RUN_SOAK instead of RUN. They're skipped by default so the pre-push
 * loop stays fast; CI runs them in a dedicated job.
 *
 *   make test         g_soak_enabled=0, g_only_soak=0  fast tests only
 *   make test-soak    g_soak_enabled=1, g_only_soak=1  soak tests only
 *   make test-all     g_soak_enabled=1, g_only_soak=0  everything
 *
 * CLI flags: --no-soak (default), --soak (run both), --soak-only. */
extern int g_soak_enabled;
extern int g_only_soak;

/* Auto-cleanup for world_t — frees the heap-allocated signal cache grid.
 * Uses __attribute__((cleanup)) on GCC/Clang; on MSVC, leaks are
 * acceptable in tests (no cleanup on early ASSERT return). */
#ifdef _MSC_VER
#define WORLD_DECL world_t w = {0}
#define WORLD_DECL_NAME(name) world_t name = {0}
#define WORLD_HEAP world_t *
#define SERVER_PLAYER_DECL(name) server_player_t name = {0}
#else
static inline void world_auto_cleanup(world_t *w) { world_cleanup(w); }
static inline void world_ptr_auto_cleanup(world_t **wp) {
    if (*wp) { world_cleanup(*wp); free(*wp); *wp = NULL; }
}
static inline void server_player_auto_cleanup(server_player_t *sp) { ship_cleanup(&sp->ship); }
#define WORLD_DECL world_t __attribute__((cleanup(world_auto_cleanup))) w = {0}
#define WORLD_DECL_NAME(name) world_t __attribute__((cleanup(world_auto_cleanup))) name = {0}
#define WORLD_HEAP __attribute__((cleanup(world_ptr_auto_cleanup))) world_t *
#define SERVER_PLAYER_DECL(name) \
    server_player_t __attribute__((cleanup(server_player_auto_cleanup))) name = {0}
#endif

#define TEST(name) static void name(void)
/* RUN snapshots tests_failed before/after the test body. The body uses
 * `return` from inside ASSERT* on failure, so RUN cannot return a value
 * — instead we observe whether the global failure counter moved. This
 * matters: the previous version unconditionally incremented tests_passed
 * and printed "ok" even after an ASSERT had already failed and bumped
 * tests_failed, causing the summary line to lie. (#261) */
#define RUN(name) do { \
    if (g_only_soak) break; \
    if (g_filter && !strstr(#name, g_filter)) break; \
    int _seq = g_test_seq++; \
    if ((_seq % g_shard_total) != g_shard_index) break; \
    int _failed_before = tests_failed; \
    tests_run++; \
    if (!g_quiet) printf("  %s ... ", #name); \
    name(); \
    if (tests_failed == _failed_before) { \
        tests_passed++; \
        if (!g_quiet) printf("ok\n"); \
    } else if (g_quiet) { \
        printf("  ^^^ %s\n", #name); \
    } \
} while(0)

/* RUN_SOAK — same as RUN but only fires when --soak / --soak-only is
 * set. Use for tests that run multi-second sim scenarios (autopilot
 * stress, e2e contract lifecycle, conservation over thousands of
 * ticks). Default `make test` skips these. */
#define RUN_SOAK(name) do { \
    if (!g_soak_enabled) break; \
    if (g_filter && !strstr(#name, g_filter)) break; \
    int _seq = g_test_seq++; \
    if ((_seq % g_shard_total) != g_shard_index) break; \
    int _failed_before = tests_failed; \
    tests_run++; \
    if (!g_quiet) printf("  %s ... ", #name); \
    name(); \
    if (tests_failed == _failed_before) { \
        tests_passed++; \
        if (!g_quiet) printf("ok\n"); \
    } else if (g_quiet) { \
        printf("  ^^^ %s\n", #name); \
    } \
} while(0)

/* Section banner — suppressed in quiet mode. Use this from
 * register_<subsystem>_tests() functions instead of a raw printf. */
#define TEST_SECTION(banner) do { \
    if (!g_quiet) printf(banner); \
} while(0)

/* Soft warning: timing-sensitive observation that shouldn't fail the
 * test but is worth noting. Counted globally and surfaced in the
 * summary; inline message suppressed in quiet mode. Two flavors so we
 * stay portable across compilers without relying on GNU
 * ##__VA_ARGS__ comma elision. */
#define TEST_WARN(msg) do { \
    g_warnings++; \
    if (!g_quiet) printf("      [WARN] %s\n", (msg)); \
} while(0)
#define TEST_WARNF(fmt, ...) do { \
    g_warnings++; \
    if (!g_quiet) printf("      [WARN] " fmt "\n", __VA_ARGS__); \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    int _a = (a), _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    %s:%d: %s == %d, expected %d\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_FLOAT(a, b, eps) do { \
    float _a = (a), _b = (b); \
    if (fabsf(_a - _b) > (eps)) { \
        printf("FAIL\n    %s:%d: %s == %.4f, expected %.4f\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    const char* _a = (a); const char* _b = (b); \
    if (strcmp(_a, _b) != 0) { \
        printf("FAIL\n    %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a, _b); \
        tests_failed++; \
        return; \
    } \
} while(0)

bool parse_hex32(const char *hex, uint8_t out[32]);
void assert_hex32_eq(const uint8_t actual[32], const char *expected_hex,
                     const char *expr, const char *file, int line);

#define ASSERT_HEX32_EQ(actual, expected_hex) do { \
    assert_hex32_eq((actual), (expected_hex), #actual, __FILE__, __LINE__); \
    if (tests_failed) return; \
} while(0)

void mining_find_best_claim(const uint8_t seed[32], const uint8_t player_pub[32],
                            uint16_t burst_cap, uint32_t *out_nonce,
                            mining_grade_t *out_grade);

/* Cross-file test helpers used by tests in multiple subsystem files. */
int test_place_outpost_via_tow(world_t *w, server_player_t *sp, vec2 pos);
world_t *setup_collision_world_heap(void);
int test_setup_placed_scaffold(world_t *w, int *out_mod_idx);
int run_autopilot_ticks(world_t *w, server_player_t *sp, float seconds);
double econ_total_credits(const world_t *w);

/* Per-process scratch path helper for tests that touch the filesystem.
 * Returns a pointer into a small ring of static buffers, so multiple
 * TMP() calls in the same expression don't clobber each other (e.g.
 * `world_save(w, TMP("a")); world_load(w2, TMP("a"));` is fine).
 *
 * The first call mkdir's `/tmp/signal-test-<pid>/` so each shard /
 * worker process has its own isolated scratch dir. Sharded test runs
 * therefore cannot race on the same `/tmp/test_*.sav` file.
 *
 * Note: callers that need a *mutable* buffer (e.g. mkdtemp) should
 * `strcpy` the result into a local `char[]` first — TMP()'s storage is
 * shared and may be reused on later calls. */
const char *test_tmp_path(const char *name);
#define TMP(name) test_tmp_path(name)

/* Per-process scratch directory itself, with no trailing slash. Use this
 * when an API takes a directory argument separately from a filename
 * (e.g. player_save(&sp, dir, slot) writes "<dir>/player_<slot>.sav").
 * Returns the same string each call; do not free. */
const char *test_tmp_dir(void);
