/*
 * test_connectome_brain.c -- unit gates for the vendored fly connectome
 * kernel and swarm economics, on a synthetic CNX4 circuit.
 *
 * Why synthetic: the real blobs (nav.cnx/full.cnx) are built from the
 * multi-GB FlyWire export and are not part of this repo, and the test
 * binary must never enable the process-global adapter (that would flip
 * every other sim test's NPCs into connectome mode). The synthetic
 * circuit below has the same population names and the same left/right
 * inhibitory logic as the real one:
 *
 *   ring L/R (GABAergic) --suppress--> columnar L/R --excite--> dn L/R
 *
 * so calibrate(), the steering sign convention, determinism, and the
 * stake/renting economy can all be asserted without the real data.
 * Adapter-level behaviour is verified by running the server with
 * SIGNAL_CONNECTOME_FAST set (see docs/connectome-brain.md).
 */
#include "test_harness.h"

#include "connectome/flybrain.h"
#include "connectome/flyswarm.h"
#include "signal_connectome_brain.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- synthetic circuit layout -------------------------------------------
 * 20 neurons:
 *   0..2   ring left   (sign -1, side -1)  --|-- each -> col L {6,7,8} w8
 *   3..5   ring right  (sign -1, side +1)  --|-- each -> col R {9,10,11} w8
 *   6..8   columnar left  (sign +1, side -1)
 *   9..11  columnar right (sign +1, side +1)
 *   12..14 dn left  (sign +1, side -1)   col L -> each dn L w3; col L -> col L w2
 *   15..17 dn right (sign +1, side +1)   col R -> each dn R w3; col R -> col R w2
 *   18     escape left  (sign +1, side -1)  (fires only under fear injection)
 *   19     escape right (sign +1, side +1)
 * ------------------------------------------------------------------------ */
#define SYN_N 20
#define SYN_NNZ ((3 * 3) + (3 * 3) + (3 * 3) + (3 * 3) + (3 * 3) + (3 * 3))

static void syn_put32(FILE *f, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF),
                           (unsigned char)((v >> 16) & 0xFF), (unsigned char)((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, f);
}
static void syn_put16(FILE *f, uint16_t v)
{
    unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
    fwrite(b, 1, 2, f);
}
static void syn_put8(FILE *f, int8_t v) { fputc((unsigned char)v, f); }

static void syn_pop(FILE *f, const char *name, uint32_t count,
                    const uint32_t *idx)
{
    char nb[16];
    memset(nb, 0, sizeof(nb));
    strncpy(nb, name, 15);
    fwrite(nb, 1, 16, f);
    syn_put32(f, count);
    for (uint32_t i = 0; i < count; i++) syn_put32(f, idx[i]);
}

static int syn_write_blob(const char *path)
{
#define SYN_CHECK(cond) do { if (!(cond)) { fclose(f); return -1; } } while (0)
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    uint32_t n = SYN_N, nnz = SYN_NNZ, n_in = 0, n_out = 6;
    fputc('C', f); fputc('N', f); fputc('X', f); fputc('4', f);
    syn_put32(f, n);
    syn_put32(f, nnz);
    syn_put32(f, n_in);
    syn_put32(f, n_out);

    /* CSR by presynaptic neuron, in index order. */
    uint32_t row_len[SYN_N] = {0};
    row_len[0] = row_len[1] = row_len[2] = 3;      /* ring L -> col L */
    row_len[3] = row_len[4] = row_len[5] = 3;      /* ring R -> col R */
    row_len[6] = row_len[7] = row_len[8] = 6;      /* col L -> dn L + col L */
    row_len[9] = row_len[10] = row_len[11] = 6;    /* col R -> dn R + col R */
    /* 12..19 have no outgoing edges */
    uint32_t rp = 0;
    syn_put32(f, rp);
    for (int i = 0; i < SYN_N; i++) { rp += row_len[i]; syn_put32(f, rp); }
    SYN_CHECK(rp == nnz);

    for (int i = 0; i < 3; i++) for (int t = 0; t < 3; t++) syn_put32(f, (uint32_t)(6 + t));
    for (int i = 0; i < 3; i++) for (int t = 0; t < 3; t++) syn_put32(f, (uint32_t)(9 + t));
    for (int i = 0; i < 3; i++) {
        for (int t = 0; t < 3; t++) syn_put32(f, (uint32_t)(12 + t));
        for (int t = 0; t < 3; t++) syn_put32(f, (uint32_t)(6 + t));
    }
    for (int i = 0; i < 3; i++) {
        for (int t = 0; t < 3; t++) syn_put32(f, (uint32_t)(15 + t));
        for (int t = 0; t < 3; t++) syn_put32(f, (uint32_t)(9 + t));
    }

    /* Weights are flat in CSR order, so they must follow the same
     * per-row interleave the col_idx block above used: each columnar
     * row emits its three dn targets before its three recurrent ones. */
    for (int i = 0; i < 3 * 3; i++) syn_put16(f, 8);   /* ring L -> col L w8 */
    for (int i = 0; i < 3 * 3; i++) syn_put16(f, 8);   /* ring R -> col R w8 */
    for (int i = 0; i < 3; i++) {
        for (int t = 0; t < 3; t++) syn_put16(f, 3);   /* col L -> dn L w3 */
        for (int t = 0; t < 3; t++) syn_put16(f, 2);   /* col L -> col L w2 */
    }
    for (int i = 0; i < 3; i++) {
        for (int t = 0; t < 3; t++) syn_put16(f, 3);   /* col R -> dn R w3 */
        for (int t = 0; t < 3; t++) syn_put16(f, 2);   /* col R -> col R w2 */
    }
    SYN_CHECK(nnz == (uint32_t)(3 * 3 * 6));

    for (uint32_t i = 0; i < n; i++) syn_put8(f, (i < 6) ? (int8_t)-1 : (int8_t)1);
    for (uint32_t i = 0; i < n; i++)
        syn_put8(f, (i % 6 < 3) ? (int8_t)-1 : (int8_t)1);
    for (uint32_t i = 0; i < n; i++) syn_put32(f, 0);  /* bias */
    /* in_idx: none. out_idx: the dn bus. */
    for (int i = 0; i < 6; i++) syn_put32(f, (uint32_t)(12 + i));
    for (uint32_t i = 0; i < n; i++) {
        fputc(0, f); fputc(0, f); fputc(0, f); fputc(0, f);
        fputc(0, f); fputc(0, f); fputc(0, f); fputc(0, f);
    }

    /* Population table. */
    uint32_t ring[6] = {0,1,2,3,4,5}, col[6] = {6,7,8,9,10,11};
    uint32_t dnl[3] = {12,13,14}, dnr[3] = {15,16,17};
    uint32_t escl[1] = {18}, escr[1] = {19};
    syn_put32(f, 6);
    syn_pop(f, "ring", 6, ring);
    syn_pop(f, "columnar", 6, col);
    syn_pop(f, "dn_left", 3, dnl);
    syn_pop(f, "dn_right", 3, dnr);
    syn_pop(f, "escape_left", 1, escl);
    syn_pop(f, "escape_right", 1, escr);

    fclose(f);
    return 0;
#undef SYN_CHECK
}

static fb_circuit g_syn;
static char g_syn_path[256];

static int syn_ensure(void)
{
    static int done = 0, ok = 0;
    if (done) return ok;
    done = 1;
    snprintf(g_syn_path, sizeof(g_syn_path), "%s/syn_connectome_XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(g_syn_path);
    if (fd < 0) return 0;
    close(fd);
    if (syn_write_blob(g_syn_path) != 0) return 0;
    if (fb_circuit_load(&g_syn, g_syn_path) != 0) return 0;
    ok = 1;
    return ok;
}

/* ------------------------------------------------------------------------ */

TEST(test_connectome_blob_loader_rejects_garbage) {
    const char *path = "/tmp/syn_connectome_garbage.bin";
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    fwrite("this is not a connectome, sorry", 1, 30, f);
    fclose(f);
    fb_circuit c;
    ASSERT(fb_circuit_load(&c, path) != 0);
    remove(path);
}

TEST(test_connectome_blob_loader_round_trip) {
    ASSERT(syn_ensure());
    ASSERT_EQ_INT((int)g_syn.n, SYN_N);
    ASSERT(fb_population(&g_syn, "ring", NULL) == 6);
    ASSERT(fb_population(&g_syn, "columnar", NULL) == 6);
    ASSERT(fb_population(&g_syn, "dn_left", NULL) == 3);
    ASSERT(fb_population(&g_syn, "escape_right", NULL) == 1);
    ASSERT(fb_population(&g_syn, "no_such_pop", NULL) == 0);
}

TEST(test_connectome_quiescent_without_drive) {
    ASSERT(syn_ensure());
    fb_sim s;
    ASSERT(fb_sim_init(&s, &g_syn, 1, 1000, 7) == 0);
    uint64_t spikes = 0;
    for (int t = 0; t < 200; t++) {
        fb_sim_step(&s);
        spikes += fb_sim_active_count(&s, 0);
    }
    ASSERT_EQ_INT((int)spikes, 0);   /* balanced wiring: no input, no fire */
    fb_sim_free(&s);
}

TEST(test_connectome_calibration_and_steering_sign) {
    ASSERT(syn_ensure());
    fb_sim s;
    ASSERT(fb_sim_init(&s, &g_syn, 1, 1000, 11) == 0);

    /* Two properties of the kernel decide these amplitudes, and both bite
     * on a fixture this small and this uniform:
     *
     *  - Membrane voltage is CLAMPED to reset for the whole refractory
     *    window, so synaptic input landing in that window is discarded.
     *    Under equal tonic and steer, ring and columnar are identical
     *    neurons under identical drive: they phase-lock, every inhibitory
     *    volley lands inside the columnar clamp, and the ring sculpts
     *    nothing. Steer must outrun tonic for the ring to bite at all.
     *  - The descending bus only reaches threshold if the columnar volleys
     *    arrive faster than they decay, which puts a floor under tonic.
     *
     * The real blob has 138k heterogeneous neurons and neither degeneracy;
     * here we pick amplitudes either side of both, and carry the noise the
     * adapter always calibrates with (it sets noise_uv before calibrating).
     * Verified stable across 20 agent seeds. */
    const int32_t TONIC_UV = 2500;
    const int32_t STEER_UV = 5000;
    s.noise_uv = 20;

    /* Drive-left must produce a POSITIVE calibrated turn (steer right):
     * the ring neurons are inhibitory, so the suppressed side reads low. */
    int32_t span = fb_sim_calibrate(&s, TONIC_UV, STEER_UV, 300);
    ASSERT(span > 0);
    ASSERT(s.turn_span == span);

    const uint32_t *ring; uint32_t nring = fb_population(&g_syn, "ring", &ring);
    const uint32_t *col;  uint32_t ncol = fb_population(&g_syn, "columnar", &col);
    int32_t *inj = calloc(nring, sizeof(int32_t));
    int32_t *ton = calloc(ncol, sizeof(int32_t));
    ASSERT(inj && ton);

    int32_t turn[3]; /* 0 symmetric, 1 drive-left, 2 drive-right */
    for (int mode = 0; mode < 3; mode++) {
        fb_sim_reset_agent(&s, 0, 5);
        for (uint32_t k = 0; k < ncol; k++) ton[k] = TONIC_UV;
        for (uint32_t k = 0; k < nring; k++) {
            int8_t sd = g_syn.side[ring[k]];
            if (mode == 0) inj[k] = STEER_UV / 2;
            else if (mode == 1) inj[k] = (sd < 0) ? STEER_UV : 0;
            else inj[k] = (sd > 0) ? STEER_UV : 0;
        }
        for (int t = 0; t < 300; t++) {
            fb_sim_inject_pop(&s, 0, "columnar", ton);
            fb_sim_inject_pop(&s, 0, "ring", inj);
            fb_sim_step(&s);
        }
        fb_command cmd;
        fb_sim_command(&s, 0, &cmd);
        turn[mode] = cmd.turn;
        ASSERT(cmd.drive > 0);   /* tonic substrate keeps the bus alive */
    }
    ASSERT(turn[1] > turn[0]);
    ASSERT(turn[0] > turn[2]);
    ASSERT(turn[1] > 32768);   /* full drive reaches > +0.5 deflection */
    ASSERT(turn[2] < -32768);
    free(inj); free(ton);
    fb_sim_free(&s);
}

TEST(test_connectome_kernel_bit_exact) {
    ASSERT(syn_ensure());
    fb_sim a, b;
    ASSERT(fb_sim_init(&a, &g_syn, 4, 1000, 42) == 0);
    ASSERT(fb_sim_init(&b, &g_syn, 4, 1000, 42) == 0);
    a.noise_uv = b.noise_uv = 40;
    int32_t ton[6] = {2500, 2500, 2500, 2500, 2500, 2500};
    for (int t = 0; t < 200; t++) {
        /* Both sims must see the SAME drive -- the claim under test is
         * "same inputs, bit-identical output", not "input does nothing". */
        for (uint32_t ag = 0; ag < 4; ag++) {
            fb_sim_inject_pop(&a, ag, "columnar", ton);
            fb_sim_inject_pop(&b, ag, "columnar", ton);
        }
        fb_sim_step(&a);
        fb_sim_step(&b);
    }
    ASSERT(fb_sim_checksum(&a) == fb_sim_checksum(&b));
    fb_sim_free(&a); fb_sim_free(&b);
}

TEST(test_connectome_agents_do_not_leak) {
    ASSERT(syn_ensure());
    fb_sim solo, many;
    ASSERT(fb_sim_init(&solo, &g_syn, 1, 1000, 99) == 0);
    ASSERT(fb_sim_init(&many, &g_syn, 8, 1000, 99) == 0);
    int32_t ton[6] = {2500, 2500, 2500, 2500, 2500, 2500};
    int32_t hot[6] = {20000, 20000, 20000, 20000, 20000, 20000};
    for (int t = 0; t < 100; t++) {
        fb_sim_inject_pop(&solo, 0, "columnar", ton); fb_sim_step(&solo);
        fb_sim_inject_pop(&many, 0, "columnar", ton);
        for (uint32_t a = 1; a < 8; a++)
            fb_sim_inject_pop(&many, a, "columnar", hot);
        fb_sim_step(&many);
    }
    ASSERT(memcmp(solo.v, many.v, (size_t)g_syn.n * sizeof(int32_t)) == 0);
    fb_sim_free(&solo); fb_sim_free(&many);
}

TEST(test_connectome_swarm_sleeping_flies_rent_out) {
    ASSERT(syn_ensure());
    fb_swarm sw;
    ASSERT(fb_swarm_init(&sw, &g_syn, NULL, 3, 1000, 7) == 0);
    sw.floor_units = 0;      /* brain-time is rented, not granted */
    sw.budget_units = 30;

    fb_swarm_set_stake(&sw, 0, 0);     /* docked: asleep */
    fb_swarm_set_stake(&sw, 1, 100);
    fb_swarm_set_stake(&sw, 2, 100);

    uint32_t allocated = fb_swarm_allocate(&sw);
    ASSERT_EQ_INT((int)sw.units[0], 0);          /* sleeper gets nothing */
    ASSERT(sw.units[1] > 0 && sw.units[2] > 0);  /* workers share the pool */
    ASSERT(allocated <= 30);
    ASSERT_EQ_INT((int)allocated, (int)(sw.units[0] + sw.units[1] + sw.units[2]));
    ASSERT_EQ_INT((int)sw.units[1], (int)sw.units[2]);  /* equal stakes, equal share */

    /* Unequal stakes split proportionally: the hungrier fly thinks more. */
    fb_swarm_set_stake(&sw, 1, 300);
    fb_swarm_set_stake(&sw, 2, 100);
    (void)fb_swarm_allocate(&sw);
    ASSERT(sw.units[1] > sw.units[2]);
    fb_swarm_free(&sw);
}

TEST(test_connectome_swarm_deep_promotion_by_stake) {
    ASSERT(syn_ensure());
    fb_swarm sw;
    ASSERT(fb_swarm_init(&sw, &g_syn, &g_syn, 3, 1000, 7) == 0);
    sw.floor_units = 0;
    sw.budget_units = 120;
    sw.deep_cost = 25;
    sw.deep_slots = 1;         /* one deliberation slot */
    sw.promote_stake = 50;
    sw.demote_stake = 20;

    fb_swarm_set_stake(&sw, 0, 400);   /* rich fly */
    fb_swarm_set_stake(&sw, 1, 40);    /* below promote */
    fb_swarm_set_stake(&sw, 2, 10);
    (void)fb_swarm_allocate(&sw);
    ASSERT_EQ_INT((int)sw.deep_on[0], 1);
    ASSERT_EQ_INT((int)sw.deep_on[1], 0);
    ASSERT(fb_swarm_sim_for(&sw, 0) == &sw.sim_deep);   /* thinks deep */
    ASSERT(fb_swarm_sim_for(&sw, 1) == &sw.sim_fast);

    /* Demote when stake falls, and the freed slot goes to next in line. */
    fb_swarm_set_stake(&sw, 0, 10);
    fb_swarm_set_stake(&sw, 1, 400);
    (void)fb_swarm_allocate(&sw);
    ASSERT_EQ_INT((int)sw.deep_on[0], 0);
    ASSERT_EQ_INT((int)sw.deep_on[1], 1);
    fb_swarm_free(&sw);
}

TEST(test_connectome_swarm_checksum_deterministic) {
    ASSERT(syn_ensure());
    fb_swarm a, b;
    ASSERT(fb_swarm_init(&a, &g_syn, NULL, 4, 1000, 13) == 0);
    ASSERT(fb_swarm_init(&b, &g_syn, NULL, 4, 1000, 13) == 0);
    a.floor_units = b.floor_units = 0;
    a.budget_units = b.budget_units = 40;
    for (int t = 0; t < 50; t++) {
        for (uint32_t i = 0; i < 4; i++) {
            fb_swarm_set_stake(&a, i, (uint32_t)(100 + 37 * i));
            fb_swarm_set_stake(&b, i, (uint32_t)(100 + 37 * i));
        }
        fb_swarm_tick(&a);
        fb_swarm_tick(&b);
    }
    ASSERT(fb_swarm_checksum(&a) == fb_swarm_checksum(&b));
    fb_swarm_free(&a); fb_swarm_free(&b);
}

TEST(test_connectome_adapter_disabled_without_env) {
    /* No SIGNAL_CONNECTOME_FAST in the test environment: the adapter
     * must stay disabled so every other sim test is unaffected. */
    unsetenv("SIGNAL_CONNECTOME_FAST");
    ASSERT(!signal_connectome_enabled());
    /* The adapter header is included via test_harness -> sim_ai; call
     * the stats accessor to prove it fails cleanly when disabled. */
    signal_connectome_stats_t st;
    ASSERT(!signal_connectome_stats(&st));
}

void register_connectome_brain_tests(void);
void register_connectome_brain_tests(void) {
    RUN(test_connectome_blob_loader_rejects_garbage);
    RUN(test_connectome_blob_loader_round_trip);
    RUN(test_connectome_quiescent_without_drive);
    RUN(test_connectome_calibration_and_steering_sign);
    RUN(test_connectome_kernel_bit_exact);
    RUN(test_connectome_agents_do_not_leak);
    RUN(test_connectome_swarm_sleeping_flies_rent_out);
    RUN(test_connectome_swarm_deep_promotion_by_stake);
    RUN(test_connectome_swarm_checksum_deterministic);
    RUN(test_connectome_adapter_disabled_without_env);
}
