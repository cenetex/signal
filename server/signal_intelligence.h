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

typedef enum {
    SIGNAL_DECISION_REASON_USED_TEACHER      = 1u << 0,
    SIGNAL_DECISION_REASON_USED_NEURAL       = 1u << 1,
    SIGNAL_DECISION_REASON_ADVISORY_ONLY     = 1u << 2,
    SIGNAL_DECISION_REASON_HARD_APPROVED     = 1u << 3,
    SIGNAL_DECISION_REASON_HAS_HOLOGRAM      = 1u << 4,
    SIGNAL_DECISION_REASON_HAS_SOURCE_MEMORY = 1u << 5,
    SIGNAL_DECISION_REASON_HAS_PROOF_MEMORY  = 1u << 6,
    SIGNAL_DECISION_REASON_HAS_ROUTE_RISK    = 1u << 7,
    SIGNAL_DECISION_REASON_HAS_TRUST_BIAS    = 1u << 8,
    SIGNAL_DECISION_REASON_FALLBACK_SCORE    = 1u << 9,
    SIGNAL_DECISION_REASON_HAS_SIGNAL_CONTEXT = 1u << 10,
    SIGNAL_DECISION_REASON_HARD_OVERRIDE     = 1u << 11,
    SIGNAL_DECISION_REASON_HAS_FRONTIER_PRESSURE = 1u << 12,
} signal_intelligence_decision_reason_flags_t;

/*
 * Compact explanation for AI pressure. The selected action is still only
 * advisory: authoritative contracts, ledgers, manifests, station state, and
 * physics decide whether the world may actually mutate.
 */
typedef struct {
    signal_intelligence_task_t task;
    int selected_index;
    int candidate_count;
    float selected_score;
    float teacher_score;
    float neural_score;
    float hologram_resonance;
    float source_memory;
    float proof_memory;
    float route_success;
    float route_risk;
    float trust_bias;
    float signal_quality;
    float frontier_pressure;
    uint64_t source_memory_id;
    uint32_t flags;
} signal_intelligence_decision_reason_t;

const char *signal_intelligence_backend_name(void);

void signal_intelligence_holographic_init(void);

void signal_intelligence_step_frontier_director(world_t *w, float dt);
bool signal_intelligence_step_frontier_director_with_reason(
    world_t *w,
    float dt,
    signal_intelligence_decision_reason_t *reason);

bool signal_intelligence_flight_builtin_available(void);
const char *signal_intelligence_flight_feature_set(void);
uint32_t signal_intelligence_flight_feature_encoder_version(void);
const char *signal_intelligence_flight_checkpoint_hash(void);

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
bool signal_intelligence_drive_player_with_reason(
    world_t *w,
    server_player_t *sp,
    float dt,
    signal_intelligence_decision_reason_t *reason);
bool signal_intelligence_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target);
void signal_intelligence_drive_npc(world_t *w, npc_ship_t *npc, float dt);

int signal_intelligence_choose_hail_station(
    const world_t *w,
    const server_player_t *sp);
int signal_intelligence_choose_hail_station_with_reason(
    const world_t *w,
    const server_player_t *sp,
    signal_intelligence_decision_reason_t *reason);

int signal_intelligence_choose_contract(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count);
int signal_intelligence_choose_contract_with_reason(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count,
    signal_intelligence_decision_reason_t *reason);

const char *signal_intelligence_npc_worker_option_name(
    signal_npc_worker_option_t option);
int signal_intelligence_choose_npc_worker(
    const signal_npc_worker_candidate_t *candidates,
    int count);
int signal_intelligence_choose_npc_worker_with_reason(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    signal_intelligence_decision_reason_t *reason);
int signal_intelligence_choose_npc_worker_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count);
int signal_intelligence_choose_npc_worker_with_scores_and_reason(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count,
    signal_intelligence_decision_reason_t *reason);

#endif /* SIGNAL_INTELLIGENCE_H */
