/*
 * signal_contract_brain.h -- On-demand neural scorer for protocol-level
 * contract decisions. The caller enumerates valid actions; this module
 * chooses among them with a loaded checkpoint or the teacher score.
 */
#ifndef SIGNAL_CONTRACT_BRAIN_H
#define SIGNAL_CONTRACT_BRAIN_H

#include "game_sim.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SIGNAL_CONTRACT_ACTION_NONE = 0,
    SIGNAL_CONTRACT_ACTION_DELIVER_LOCAL,
    SIGNAL_CONTRACT_ACTION_BUY_AND_DELIVER,
    SIGNAL_CONTRACT_ACTION_WAIT_FOR_STOCK,
} signal_contract_action_t;

typedef struct {
    signal_contract_action_t action;
    int source_station;
    int dest_station;
    commodity_t commodity;
    float quantity_needed;
    float contract_price;
    float source_price;
    float source_stock;
    float dest_stock;
    float ledger_balance;
    float free_cargo;
    float distance;
    float age;
    float hull_ratio;
    float hologram_resonance;
    float source_memory;
    float route_success_memory;
    float route_danger_memory;
    float route_proof_memory;
    float trust_bias;
    uint64_t source_memory_id;
    float teacher_score;
} signal_contract_candidate_t;

bool signal_contract_brain_load_checkpoint(const char *path,
                                           char *err,
                                           size_t err_size);
bool signal_contract_brain_loaded(void);
uint64_t signal_contract_brain_inference_count(void);
uint64_t signal_contract_brain_decision_count(void);
uint64_t signal_contract_brain_teacher_decision_count(void);

int signal_contract_brain_choose(const world_t *w,
                                 const server_player_t *sp,
                                 const signal_contract_candidate_t *candidates,
                                 int count);
int signal_contract_brain_choose_with_scores(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count,
    double *scores,
    int score_count);

#endif /* SIGNAL_CONTRACT_BRAIN_H */
