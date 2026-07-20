/*
 * rock_usefulness.h -- Pure priority grammar for collectible-rock HUD reasons.
 *
 * World and knowledge inspection stays in the client.  Keeping the ordering
 * here makes target and tow presentation share one deterministic rule and lets
 * the precedence be tested without linking the renderer.
 */
#ifndef SIGNAL_ROCK_USEFULNESS_H
#define SIGNAL_ROCK_USEFULNESS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ROCK_USEFULNESS_NONE = 0,
    ROCK_USEFULNESS_RARE_GRADE,
    ROCK_USEFULNESS_SMELT_PATH,
    ROCK_USEFULNESS_REMEMBERED_ROUTE,
    ROCK_USEFULNESS_DIRECT_DEMAND,
    ROCK_USEFULNESS_TRACKED_CONTRACT,
} rock_usefulness_kind_t;

typedef struct {
    rock_usefulness_kind_t kind;
    int station_a;          /* destination for demand/route, smelter for path */
    int station_b;          /* route origin, otherwise -1 */
    int commodity;          /* route commodity or smelt output */
    uint16_t strength;      /* severity, proximity, or grade within one kind */
    uint8_t confidence;     /* remembered-route evidence */
    uint8_t salience;       /* remembered-route evidence */
    uint8_t hops;           /* remembered-route relay distance */
    uint64_t subject_nonce; /* stable seed for clarity degradation */
} rock_usefulness_candidate_t;

static inline uint32_t rock_usefulness_evidence_score(
    const rock_usefulness_candidate_t *candidate) {
    if (!candidate || candidate->kind == ROCK_USEFULNESS_NONE) return 0;
    if (candidate->kind != ROCK_USEFULNESS_REMEMBERED_ROUTE)
        return candidate->strength;

    /* Confidence and salience are already decayed by the knowledge system.
     * Penalize carried relays again so a fresh firsthand route wins a tie. */
    uint32_t evidence = ((uint32_t)candidate->confidence + 1u) *
                        ((uint32_t)candidate->salience + 1u);
    return evidence / (1u + (uint32_t)candidate->hops);
}

static inline bool rock_usefulness_is_stronger(
    const rock_usefulness_candidate_t *candidate,
    const rock_usefulness_candidate_t *current) {
    if (!candidate || candidate->kind == ROCK_USEFULNESS_NONE) return false;
    if (!current || current->kind == ROCK_USEFULNESS_NONE) return true;
    if (candidate->kind != current->kind)
        return candidate->kind > current->kind;
    return rock_usefulness_evidence_score(candidate) >
           rock_usefulness_evidence_score(current);
}

static inline void rock_usefulness_select(
    rock_usefulness_candidate_t *current,
    const rock_usefulness_candidate_t *candidate) {
    if (current && rock_usefulness_is_stronger(candidate, current))
        *current = *candidate;
}

#endif /* SIGNAL_ROCK_USEFULNESS_H */
