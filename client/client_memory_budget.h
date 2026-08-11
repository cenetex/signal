/*
 * Regression ceilings for client-resident memory.
 *
 * Keep these close to measured release values.  Raising a ceiling requires a
 * new measurement and an explicit explanation in review.
 */
#ifndef CLIENT_MEMORY_BUDGET_H
#define CLIENT_MEMORY_BUDGET_H

#define SIGNAL_WORLD_SIZE_BUDGET_BYTES 17000000u
#define SIGNAL_GAME_SIZE_BUDGET_BYTES  22250000u
#define SIGNAL_WASM_INITIAL_PAGE_BUDGET 896u

#endif /* CLIENT_MEMORY_BUDGET_H */
