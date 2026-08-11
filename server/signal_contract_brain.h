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
    SIGNAL_CONTRACT_ACTION_COUNT,
} signal_contract_action_t;

enum {
    SIGNAL_CONTRACT_FEATURE_ENCODER_VERSION = 2,
};

#define SIGNAL_CONTRACT_FEATURE_SET "signal-contract-live-v2"

typedef enum {
    SIGNAL_CONTRACT_FEATURE_BIAS = 0,
    SIGNAL_CONTRACT_FEATURE_ACTION_ORDINAL,
    SIGNAL_CONTRACT_FEATURE_SOURCE_ACTIVE,
    SIGNAL_CONTRACT_FEATURE_DEST_ACTIVE,
    SIGNAL_CONTRACT_FEATURE_SAME_STATION,
    SIGNAL_CONTRACT_FEATURE_COMMODITY_ORDINAL,
    SIGNAL_CONTRACT_FEATURE_SOURCE_STOCK,
    SIGNAL_CONTRACT_FEATURE_DEST_STOCK,
    SIGNAL_CONTRACT_FEATURE_QUANTITY_NEEDED,
    SIGNAL_CONTRACT_FEATURE_CONTRACT_PRICE,
    SIGNAL_CONTRACT_FEATURE_SOURCE_PRICE,
    SIGNAL_CONTRACT_FEATURE_PROFIT,
    SIGNAL_CONTRACT_FEATURE_LEDGER_BALANCE,
    SIGNAL_CONTRACT_FEATURE_CAN_AFFORD,
    SIGNAL_CONTRACT_FEATURE_HAS_CARGO,
    SIGNAL_CONTRACT_FEATURE_FREE_CARGO,
    SIGNAL_CONTRACT_FEATURE_DISTANCE,
    SIGNAL_CONTRACT_FEATURE_AGE,
    SIGNAL_CONTRACT_FEATURE_HULL_RATIO,
    SIGNAL_CONTRACT_FEATURE_TOWED_BODY_COUNT,
    SIGNAL_CONTRACT_FEATURE_SOURCE_PRODUCES,
    SIGNAL_CONTRACT_FEATURE_DEST_CONSUMES,
    SIGNAL_CONTRACT_FEATURE_SOURCE_HAS_DOCK,
    SIGNAL_CONTRACT_FEATURE_SOURCE_HAS_FURNACE,
    SIGNAL_CONTRACT_FEATURE_SOURCE_HAS_SHIPYARD,
    SIGNAL_CONTRACT_FEATURE_DEST_HAS_DOCK,
    SIGNAL_CONTRACT_FEATURE_DEST_HAS_FURNACE,
    SIGNAL_CONTRACT_FEATURE_DEST_HAS_SHIPYARD,
    SIGNAL_CONTRACT_FEATURE_WORLD_TIME,
    SIGNAL_CONTRACT_FEATURE_WORLD_TICK,
    SIGNAL_CONTRACT_FEATURE_TEACHER_SCORE,
    SIGNAL_CONTRACT_FEATURE_ACTION_ONE_HOT_BEGIN,
    SIGNAL_CONTRACT_FEATURE_ACTION_ONE_HOT_END =
        SIGNAL_CONTRACT_FEATURE_ACTION_ONE_HOT_BEGIN +
        SIGNAL_CONTRACT_ACTION_COUNT,
    SIGNAL_CONTRACT_FEATURE_COMMODITY_FERRITE =
        SIGNAL_CONTRACT_FEATURE_ACTION_ONE_HOT_END,
    SIGNAL_CONTRACT_FEATURE_COMMODITY_FRAME,
    SIGNAL_CONTRACT_FEATURE_COMMODITY_LASER,
    SIGNAL_CONTRACT_FEATURE_COMMODITY_TRACTOR,
    SIGNAL_CONTRACT_FEATURE_COMMODITY_REPAIR_KIT,
    SIGNAL_CONTRACT_FEATURE_COUNT,
} signal_contract_feature_t;

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

bool signal_contract_build_features(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidate,
    float features[SIGNAL_CONTRACT_FEATURE_COUNT]);
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
