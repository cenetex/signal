#include "tests/test_harness.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#endif

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

int g_shard_total = 1;
int g_shard_index = 0;
int g_test_seq    = 0;

int g_quiet = 0;
int g_warnings = 0;

const char *g_filter = NULL;
int g_soak_enabled = 0;
int g_only_soak    = 0;

bool parse_hex32(const char *hex, uint8_t out[32]) {
    static const char digits[] = "0123456789abcdef";
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        const char *hi = strchr(digits, (int)hex[i * 2] | 32);
        const char *lo = strchr(digits, (int)hex[i * 2 + 1] | 32);
        if (!hi || !lo) return false;
        out[i] = (uint8_t)(((hi - digits) << 4) | (lo - digits));
    }
    return true;
}

void assert_hex32_eq(const uint8_t actual[32], const char *expected_hex,
                     const char *expr, const char *file, int line) {
    uint8_t expected[32];
    if (!parse_hex32(expected_hex, expected) || memcmp(actual, expected, 32) != 0) {
        char actual_hex[65];
        static const char digits[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            actual_hex[i * 2] = digits[actual[i] >> 4];
            actual_hex[i * 2 + 1] = digits[actual[i] & 0x0F];
        }
        actual_hex[64] = '\0';
        printf("FAIL\n    %s:%d: %s == %s, expected %s\n",
               file, line, expr, actual_hex, expected_hex);
        tests_failed++;
    }
}

void mining_find_best_claim(const uint8_t seed[32], const uint8_t player_pub[32],
                            uint16_t burst_cap, uint32_t *out_nonce,
                            mining_grade_t *out_grade) {
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;

    for (uint32_t i = 0; i < burst_cap; i++) {
        mining_keypair_t kp;
        char callsign[8];
        mining_grade_t grade;

        mining_keypair_derive(seed, player_pub, i, &kp);
        mining_callsign_from_pubkey(kp.pub, callsign);
        grade = mining_classify_base58(callsign);
        if (grade > best_grade) {
            best_grade = grade;
            best_nonce = i;
        }
    }

    if (out_nonce) *out_nonce = best_nonce;
    if (out_grade) *out_grade = best_grade;
}

/* Place an outpost via the new tow flow without doing the full tow physics.
 * Spawns a scaffold near `pos`, attaches it to the player, and runs
 * place_towed_scaffold via place_outpost intent. */
int test_place_outpost_via_tow(world_t *w, server_player_t *sp, vec2 pos) {
    int sidx = spawn_scaffold(w, MODULE_SIGNAL_RELAY, pos, sp->id);
    if (sidx < 0) return -1;
    sp->ship.towed_scaffold = (int16_t)sidx;
    w->scaffolds[sidx].state = SCAFFOLD_TOWING;
    w->scaffolds[sidx].towed_by = sp->id;
    sp->input.place_outpost = true;
    sp->input.place_target_station = -1;
    sp->input.place_target_ring = -1;
    sp->input.place_target_slot = -1;
    world_sim_step(w, SIM_DT);
    /* Find the new outpost (slot >= 3) */
    for (int s = 3; s < MAX_STATIONS; s++) {
        if (station_exists(&w->stations[s])
            && fabsf(w->stations[s].pos.x - pos.x) < 5.0f
            && fabsf(w->stations[s].pos.y - pos.y) < 5.0f) {
            return s;
        }
    }
    return -1;
}

world_t *setup_collision_world_heap(void) {
    world_t *w = calloc(1, sizeof(world_t));
    world_reset(w);
    /* Clear asteroids and NPCs to isolate collision testing */
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    /* Player at station 0 */
    w->players[0].connected = true;
    w->players[0].id = 0;
    player_init_ship(&w->players[0], w);
    w->players[0].docked = false;
    return w;
}

int test_setup_placed_scaffold(world_t *w, int *out_mod_idx) {
    w->players[0].connected = true;
    player_init_ship(&w->players[0], w);
    w->players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    vec2 outpost_pos = v2_add(w->stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(w, &w->players[0], outpost_pos);
    if (outpost < 3) return -1;
    w->stations[outpost].scaffold = false;
    w->stations[outpost].scaffold_progress = 1.0f;
    w->stations[outpost].signal_range = 6000.0f;
    w->stations[outpost].arm_count = 1;
    w->stations[outpost].arm_speed[0] = 0.04f;
    /* Pre-supply any founding module scaffolds so they don't interfere */
    for (int i = 0; i < w->stations[outpost].module_count; i++) {
        if (w->stations[outpost].modules[i].scaffold)
            w->stations[outpost].modules[i].build_progress = 1.0f;
    }
    rebuild_signal_chain(w);
    int before = w->stations[outpost].module_count;
    vec2 ring1_near = v2_add(outpost_pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(w, MODULE_FURNACE, ring1_near, 0);
    if (idx < 0) return -1;
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    if (w->stations[outpost].module_count != before + 1) return -1;
    *out_mod_idx = before;
    return outpost;
}

int run_autopilot_ticks(world_t *w, server_player_t *sp, float seconds) {
    int ticks = (int)(seconds * 120.0f); /* 120 Hz */
    for (int i = 0; i < ticks; i++) {
        world_sim_step(w, 1.0f / 120.0f);
    }
    return sp->autopilot_state;
}

/* Per-process scratch dir for filesystem-touching tests. The first call
 * computes "/tmp/signal-test-<pid>" and mkdir's it; subsequent calls
 * just format `<dir>/<name>` into one of TMP_RING buffers. The ring lets
 * a single statement that calls TMP() multiple times (e.g. save+load
 * pairs in arguments) get distinct buffers. */
#define TMP_RING 8
#define TMP_BUFLEN 128
static char g_tmp_dir[64];
static int  g_tmp_dir_ready = 0;

static void ensure_tmp_dir(void) {
    if (g_tmp_dir_ready) return;
#ifdef _WIN32
    /* On Windows tests don't run the filesystem suites, but keep
     * the helper safe — fall back to "tmp_<pid>". */
    snprintf(g_tmp_dir, sizeof(g_tmp_dir), "tmp_%lu", (unsigned long)_getpid());
#else
    snprintf(g_tmp_dir, sizeof(g_tmp_dir), "/tmp/signal-test-%ld", (long)getpid());
    if (mkdir(g_tmp_dir, 0700) != 0 && errno != EEXIST) {
        /* Fall back to plain /tmp on weird CI; tests will still
         * collide there, but at least they won't crash. */
        snprintf(g_tmp_dir, sizeof(g_tmp_dir), "/tmp");
    }
#endif
    g_tmp_dir_ready = 1;
}

const char *test_tmp_dir(void) {
    ensure_tmp_dir();
    return g_tmp_dir;
}

const char *test_tmp_path(const char *name) {
    static char buffers[TMP_RING][TMP_BUFLEN];
    static int next = 0;

    ensure_tmp_dir();
    char *buf = buffers[next];
    next = (next + 1) % TMP_RING;
    snprintf(buf, TMP_BUFLEN, "%s/%s", g_tmp_dir, name);
    return buf;
}

double econ_total_credits(const world_t *w) {
    double total = 0.0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (st->id == 0) continue;
        total += (double)station_credit_pool(st);
        for (int i = 0; i < st->ledger_count; i++)
            total += (double)st->ledger[i].balance;
    }
    return total;
}
