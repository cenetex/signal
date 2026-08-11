/*
 * signal_npc_worker_brain.h -- Optional neural strategic scorer for NPC
 * worker assignment. The planner enumerates legal high-level actions; this
 * module scores that small candidate set from a loaded crlplrimes checkpoint.
 */
#ifndef SIGNAL_NPC_WORKER_BRAIN_H
#define SIGNAL_NPC_WORKER_BRAIN_H

#include "game_sim.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SIGNAL_NPC_WORKER_OPTION_WAIT = 0,
    SIGNAL_NPC_WORKER_OPTION_MINE_HOME,
    SIGNAL_NPC_WORKER_OPTION_HAUL_CONTRACT,
    SIGNAL_NPC_WORKER_OPTION_GOSSIP_COURIER,
    SIGNAL_NPC_WORKER_OPTION_SELF_REFIT_HOME,
    SIGNAL_NPC_WORKER_OPTION_IMPORT_FRAME,
    SIGNAL_NPC_WORKER_OPTION_IMPORT_LASER,
    SIGNAL_NPC_WORKER_OPTION_IMPORT_TRACTOR,
    SIGNAL_NPC_WORKER_OPTION_SUPPLY_FRONTIER,
    SIGNAL_NPC_WORKER_OPTION_ESCORT_CONVOY,
    SIGNAL_NPC_WORKER_OPTION_PATROL_ROUTE,
    SIGNAL_NPC_WORKER_OPTION_TAKE_RISKY_PROFIT,
    SIGNAL_NPC_WORKER_OPTION_COUNT,
} signal_npc_worker_option_t;

enum {
    SIGNAL_NPC_WORKER_FEATURE_ENCODER_VERSION = 2,
};

#define SIGNAL_NPC_WORKER_FEATURE_SET "signal-npc-worker-v2"

/*
 * Encoder v2 is deliberately expressed in semantic facts. Station array
 * slots and faction slots are not features: generated outposts may occupy any
 * slot while presenting the same capabilities and relative topology.
 *
 * The option one-hot width participates in the final feature-count assertion
 * in signal_npc_worker_brain.c. Adding an option therefore requires an
 * intentional schema update instead of silently aliasing it to an old option.
 */
typedef enum {
    SIGNAL_NPC_WORKER_FEATURE_BIAS = 0,
    SIGNAL_NPC_WORKER_FEATURE_OPTION_ORDINAL,
    SIGNAL_NPC_WORKER_FEATURE_OPTION_ONE_HOT_BEGIN,
    SIGNAL_NPC_WORKER_FEATURE_OPTION_ONE_HOT_END =
        SIGNAL_NPC_WORKER_FEATURE_OPTION_ONE_HOT_BEGIN +
        SIGNAL_NPC_WORKER_OPTION_COUNT,
    SIGNAL_NPC_WORKER_FEATURE_ROLE_MINER =
        SIGNAL_NPC_WORKER_FEATURE_OPTION_ONE_HOT_END,
    SIGNAL_NPC_WORKER_FEATURE_ROLE_HAULER,
    SIGNAL_NPC_WORKER_FEATURE_MINING_LEVEL,
    SIGNAL_NPC_WORKER_FEATURE_HOLD_LEVEL,
    SIGNAL_NPC_WORKER_FEATURE_TRACTOR_LEVEL,
    SIGNAL_NPC_WORKER_FEATURE_HAS_BEST_CONTRACT,
    SIGNAL_NPC_WORKER_FEATURE_HOME_BALANCE,
    SIGNAL_NPC_WORKER_FEATURE_HOME_REFIT_STOCK,
    SIGNAL_NPC_WORKER_FEATURE_REMOTE_REFIT_STOCK,
    SIGNAL_NPC_WORKER_FEATURE_DESIRED_UNITS,
    SIGNAL_NPC_WORKER_FEATURE_REFIT_COST,
    SIGNAL_NPC_WORKER_FEATURE_CAN_PAY_REFIT,
    SIGNAL_NPC_WORKER_FEATURE_UPGRADE_MINING,
    SIGNAL_NPC_WORKER_FEATURE_UPGRADE_HOLD,
    SIGNAL_NPC_WORKER_FEATURE_UPGRADE_TRACTOR,
    SIGNAL_NPC_WORKER_FEATURE_DESIRED_FRAME,
    SIGNAL_NPC_WORKER_FEATURE_DESIRED_LASER,
    SIGNAL_NPC_WORKER_FEATURE_DESIRED_TRACTOR,
    SIGNAL_NPC_WORKER_FEATURE_BEST_CONTRACT_VALUE,
    SIGNAL_NPC_WORKER_FEATURE_BEST_CONTRACT_STOCK,
    SIGNAL_NPC_WORKER_FEATURE_BEST_CONTRACT_REMOTE,
    SIGNAL_NPC_WORKER_FEATURE_MINE_PRESSURE,
    SIGNAL_NPC_WORKER_FEATURE_PERSONA_RISK,
    SIGNAL_NPC_WORKER_FEATURE_PERSONA_GROWTH,
    SIGNAL_NPC_WORKER_FEATURE_PERSONA_PATIENCE,
    SIGNAL_NPC_WORKER_FEATURE_ROUTE_DISTANCE,
    SIGNAL_NPC_WORKER_FEATURE_HOME_HAS_DOCK,
    SIGNAL_NPC_WORKER_FEATURE_HOME_HAS_SHIPYARD,
    SIGNAL_NPC_WORKER_FEATURE_HOME_HAS_FURNACE,
    SIGNAL_NPC_WORKER_FEATURE_HOME_HAS_FRAME_PRESS,
    SIGNAL_NPC_WORKER_FEATURE_HOME_HAS_LASER_FAB,
    SIGNAL_NPC_WORKER_FEATURE_HOME_HAS_TRACTOR_FAB,
    SIGNAL_NPC_WORKER_FEATURE_OPTION_WAIT,
    SIGNAL_NPC_WORKER_FEATURE_HOME_REFIT_READY,
    SIGNAL_NPC_WORKER_FEATURE_OPTION_IMPORT,
    SIGNAL_NPC_WORKER_FEATURE_LEGAL,
    SIGNAL_NPC_WORKER_FEATURE_TRAVEL,
    SIGNAL_NPC_WORKER_FEATURE_SELF_UPGRADE,
    SIGNAL_NPC_WORKER_FEATURE_IMPORT_MODULE,
    SIGNAL_NPC_WORKER_FEATURE_CREDIT_DELTA,
    SIGNAL_NPC_WORKER_FEATURE_REFIT_PROGRESS,
    SIGNAL_NPC_WORKER_FEATURE_CONTRACT_VALUE,
    SIGNAL_NPC_WORKER_FEATURE_CARGO_MOVED,
    SIGNAL_NPC_WORKER_FEATURE_FRONTIER_PRESSURE,
    SIGNAL_NPC_WORKER_FEATURE_ROUTE_SUCCESS,
    SIGNAL_NPC_WORKER_FEATURE_ROUTE_DANGER,
    SIGNAL_NPC_WORKER_FEATURE_ROUTE_PROOF,
    SIGNAL_NPC_WORKER_FEATURE_HOLOGRAM_RESONANCE,
    SIGNAL_NPC_WORKER_FEATURE_SOURCE_MEMORY,
    SIGNAL_NPC_WORKER_FEATURE_PROVENANCE_PRESSURE,
    SIGNAL_NPC_WORKER_FEATURE_TRUST_BIAS,
    SIGNAL_NPC_WORKER_FEATURE_BLACK_MARKET_ACCEPTANCE,
    SIGNAL_NPC_WORKER_FEATURE_ESCORT_BONUS,
    SIGNAL_NPC_WORKER_FEATURE_CONVOY_BONUS,
    SIGNAL_NPC_WORKER_FEATURE_POLICY_SCREENING,
    SIGNAL_NPC_WORKER_FEATURE_BLACK_MARKET_STATION,
    SIGNAL_NPC_WORKER_FEATURE_CONTRABAND_OPPORTUNITY,
    SIGNAL_NPC_WORKER_FEATURE_FRONTIER_SUPPLY,
    SIGNAL_NPC_WORKER_FEATURE_ESCORT,
    SIGNAL_NPC_WORKER_FEATURE_PATROL,
    SIGNAL_NPC_WORKER_FEATURE_RISKY_PROFIT,
    SIGNAL_NPC_WORKER_FEATURE_BEST_FERRITE_CONTRACT,
    SIGNAL_NPC_WORKER_FEATURE_BEST_CUPRITE_CONTRACT,
    SIGNAL_NPC_WORKER_FEATURE_BEST_CRYSTAL_CONTRACT,
    SIGNAL_NPC_WORKER_FEATURE_COUNT,
} signal_npc_worker_feature_t;

typedef struct {
    signal_npc_worker_option_t option;
    npc_role_t role;
    int home_station;
    int mining_level;
    int hold_level;
    int tractor_level;
    int desired_upgrade;
    commodity_t desired_commodity;
    int desired_units;
    float home_balance;
    float home_refit_stock;
    float remote_refit_stock;
    float refit_cost;
    float best_contract_value;
    float best_contract_stock;
    int best_contract_dest;
    commodity_t best_contract_commodity;
    bool mine_pressure;
    float frontier_pressure;
    float route_success_memory;
    float route_danger_memory;
    float route_proof_memory;
    float hologram_resonance;
    float source_memory;
    float provenance_pressure;
    float trust_bias;
    float black_market_acceptance;
    float escort_bonus;
    float convoy_bonus;
    float persona_risk;
    float persona_growth;
    float persona_patience;
    float route_km;
    bool home_has_dock;
    bool home_has_shipyard;
    bool home_has_furnace;
    bool home_has_frame_press;
    bool home_has_laser_fab;
    bool home_has_tractor_fab;
    bool legal;
    bool travel;
    bool self_upgrade;
    bool import_module;
    bool policy_screening;
    bool black_market_station;
    bool contraband_opportunity;
    bool frontier_supply;
    bool escort;
    bool patrol;
    bool risky_profit;
    float credit_delta;
    float refit_progress;
    float contract_value;
    float cargo_moved;
    float teacher_score;
} signal_npc_worker_candidate_t;

bool signal_npc_worker_brain_load_checkpoint(const char *path,
                                             char *err,
                                             size_t err_size);
bool signal_npc_worker_brain_loaded(void);
uint64_t signal_npc_worker_brain_inference_count(void);
uint64_t signal_npc_worker_brain_decision_count(void);
uint64_t signal_npc_worker_brain_teacher_decision_count(void);

const char *signal_npc_worker_option_name(signal_npc_worker_option_t option);
bool signal_npc_worker_build_features(
    const signal_npc_worker_candidate_t *candidate,
    float features[SIGNAL_NPC_WORKER_FEATURE_COUNT]);
int signal_npc_worker_brain_choose(const signal_npc_worker_candidate_t *candidates,
                                   int count);
int signal_npc_worker_brain_choose_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count);

#endif /* SIGNAL_NPC_WORKER_BRAIN_H */
