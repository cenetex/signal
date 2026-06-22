/*
 * signal_intelligence.h -- Central AI/decision façade for Signal.
 *
 * This layer is the stable seam between the simulation and whatever
 * intelligence backend is active. Today it delegates to the existing
 * checkpoint, teacher, and holographic implementations. Future NSRL packs
 * should replace those backends here instead of teaching each sim subsystem
 * about a new model runtime.
 */
#ifndef SIGNAL_INTELLIGENCE_H
#define SIGNAL_INTELLIGENCE_H

#include "signal_brain.h"
#include "signal_contract_brain.h"
#include "signal_npc_worker_brain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SIGNAL_INTEL_TASK_FLIGHT_CONTROL = 0,
    SIGNAL_INTEL_TASK_CONTRACT_CHOICE,
    SIGNAL_INTEL_TASK_NPC_WORKER_ASSIGNMENT,
    SIGNAL_INTEL_TASK_HAIL_CHOICE,
    SIGNAL_INTEL_TASK_OPERATOR_POST,
    SIGNAL_INTEL_TASK_FRONTIER_PLAN,
    SIGNAL_INTEL_TASK_COUNT,
} signal_intelligence_task_t;

const char *signal_intelligence_backend_name(void);

void signal_intelligence_holographic_init(void);

bool signal_intelligence_load_flight_checkpoint(const char *path,
                                                char *err,
                                                size_t err_size);
bool signal_intelligence_flight_loaded(void);
uint64_t signal_intelligence_flight_inference_count(void);

bool signal_intelligence_load_contract_checkpoint(const char *path,
                                                  char *err,
                                                  size_t err_size);
bool signal_intelligence_contract_loaded(void);
uint64_t signal_intelligence_contract_inference_count(void);
uint64_t signal_intelligence_contract_decision_count(void);
uint64_t signal_intelligence_contract_teacher_decision_count(void);

bool signal_intelligence_load_npc_worker_checkpoint(const char *path,
                                                    char *err,
                                                    size_t err_size);
bool signal_intelligence_npc_worker_loaded(void);
uint64_t signal_intelligence_npc_worker_inference_count(void);
uint64_t signal_intelligence_npc_worker_decision_count(void);
uint64_t signal_intelligence_npc_worker_teacher_decision_count(void);

void signal_intelligence_drive_player(world_t *w, server_player_t *sp, float dt);
bool signal_intelligence_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target);
void signal_intelligence_drive_npc(world_t *w, npc_ship_t *npc, float dt);

int signal_intelligence_choose_contract(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count);

const char *signal_intelligence_npc_worker_option_name(
    signal_npc_worker_option_t option);
int signal_intelligence_choose_npc_worker(
    const signal_npc_worker_candidate_t *candidates,
    int count);
int signal_intelligence_choose_npc_worker_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count);

#endif /* SIGNAL_INTELLIGENCE_H */
