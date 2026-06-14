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
    SIGNAL_NPC_WORKER_OPTION_COUNT,
} signal_npc_worker_option_t;

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
int signal_npc_worker_brain_choose(const signal_npc_worker_candidate_t *candidates,
                                   int count);
int signal_npc_worker_brain_choose_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count);

#endif /* SIGNAL_NPC_WORKER_BRAIN_H */
