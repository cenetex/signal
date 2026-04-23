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
    int _seq = g_test_seq++; \
    if ((_seq % g_shard_total) != g_shard_index) break; \
    int _failed_before = tests_failed; \
    tests_run++; \
    printf("  %s ... ", #name); \
    name(); \
    if (tests_failed == _failed_before) { \
        tests_passed++; \
        printf("ok\n"); \
    } \
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
