/*
 * mining_sim.c -- Standalone distribution simulator for the RATi
 * grade ladder. Rolls N fragments of K candidates each, prints
 * histogram. Reads classifier from shared/mining.h directly.
 *
 * Build:
 *   gcc -O2 -I shared tools/mining_sim.c -o build/mining_sim
 * Run:
 *   build/mining_sim [fragments=10000] [candidates_per_fragment=1000]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "mining.h"

/* Single-roll classifier (prefix-anchored). One candidate per
 * fragment — no best-of-N inflation. Designed so common is the
 * dominant outcome and rare/RATi feel earned.
 *
 *   fine = first two chars identical          (~1/58 = 1.7%)
 *   rare = first three identical              (~1/58^2 = 0.03%)
 *   RATi = starts with 'R' and 'A' anywhere in first 4 chars
 *          (looser than exact 'RATi' so it's reachable)
 *   commissioned = exact 'RATi' prefix        (very rare lottery) */
static mining_grade_t classify_strict(const char *s) {
    if (!s || !s[0]) return MINING_GRADE_COMMON;
    size_t n = strlen(s);
    /* Commissioned: exact 'RATi' prefix — true brand match, lottery rare. */
    if (n >= 4 && s[0]=='R' && s[1]=='A' && s[2]=='T' && s[3]=='i')
        return MINING_GRADE_COMMISSIONED;
    /* RATi: starts with 'R', has 'A' in first 4 chars. ~1/58 × 4/58 ≈ 1/843. */
    if (n >= 4 && s[0]=='R') {
        for (size_t i = 1; i < 4; i++) {
            if (s[i] == 'A') return MINING_GRADE_RATI;
        }
    }
    if (n >= 3 && s[0]==s[1] && s[1]==s[2])
        return MINING_GRADE_RARE;
    if (n >= 2 && s[0]==s[1])
        return MINING_GRADE_FINE;
    return MINING_GRADE_COMMON;
}

int main(int argc, char **argv) {
    int n_fragments = (argc > 1) ? atoi(argv[1]) : 10000;
    int candidates_per_fragment = (argc > 2) ? atoi(argv[2]) : 1000;
    int use_strict = (argc > 3) ? atoi(argv[3]) : 0;

    /* Histograms: count of each grade as the BEST in a fragment. */
    long fragment_best[MINING_GRADE_COUNT] = {0};
    /* Per-candidate counts: how many of EACH grade get rolled overall. */
    long candidate_class[MINING_GRADE_COUNT] = {0};

    /* Use a fixed universe key like the real game does. */
    uint8_t universe_key[32] = {
        'R','A','T','i', '/','o','r','e', '/','u','n','i', 'v','e','r','s',
        'e','/','v','1', 0,0,0,0,         0,0,0,0,         0,0,0,0
    };

    /* Each fragment gets a unique fracture_seed; here we just hash the
     * iteration index to mock the per-rock variance. */
    for (int f = 0; f < n_fragments; f++) {
        uint8_t seed[32];
        uint8_t seed_input[8];
        for (int b = 0; b < 8; b++)
            seed_input[b] = (uint8_t)((f >> (b * 4)) ^ (f * 2654435761u >> (b * 8)));
        sha256_bytes(seed_input, 8, seed);

        mining_grade_t best = MINING_GRADE_COMMON;
        for (int i = 0; i < candidates_per_fragment; i++) {
            mining_keypair_t kp;
            mining_keypair_derive(seed, universe_key, (uint32_t)i, &kp);
            char callsign[8];
            mining_callsign_from_pubkey(kp.pub, callsign);
            mining_grade_t g = use_strict
                ? classify_strict(callsign)
                : mining_classify_base58(callsign);
            candidate_class[g]++;
            if (g > best) best = g;
        }
        fragment_best[best]++;
    }

    static const char *labels[MINING_GRADE_COUNT] = {
        "common", "fine", "rare", "RATi", "commissioned"
    };

    long total_candidates = (long)n_fragments * candidates_per_fragment;

    printf("Sim: %d fragments × %d candidates = %ld total rolls\n\n",
           n_fragments, candidates_per_fragment, total_candidates);

    printf("Per-candidate classification (out of %ld):\n", total_candidates);
    for (int g = 0; g < MINING_GRADE_COUNT; g++) {
        double pct = 100.0 * candidate_class[g] / (double)total_candidates;
        long denom_one_in = candidate_class[g] > 0
            ? (long)(total_candidates / candidate_class[g]) : 0;
        printf("  %-13s  %10ld  %7.4f%%   1 in %ld\n",
               labels[g], candidate_class[g], pct,
               denom_one_in);
    }

    printf("\nFragment best-grade distribution (each frag = 1 roll cap):\n");
    for (int g = MINING_GRADE_COUNT - 1; g >= 0; g--) {
        double pct = 100.0 * fragment_best[g] / (double)n_fragments;
        long denom_one_in = fragment_best[g] > 0
            ? (long)(n_fragments / fragment_best[g]) : 0;
        printf("  %-13s  %10ld  %7.4f%%   1 in %ld fragments\n",
               labels[g], fragment_best[g], pct, denom_one_in);
    }

    return 0;
}
