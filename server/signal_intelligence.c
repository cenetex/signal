/*
 * signal_intelligence.c -- Central AI/decision façade for Signal.
 */
#include "signal_intelligence.h"

const char *signal_intelligence_backend_name(void) {
    return "legacy-checkpoint-facade";
}

void signal_intelligence_holographic_init(void) {
    signal_brain_holographic_init();
}

bool signal_intelligence_load_flight_checkpoint(const char *path,
                                                char *err,
                                                size_t err_size) {
    return signal_brain_load_checkpoint(path, err, err_size);
}

bool signal_intelligence_flight_loaded(void) {
    return signal_brain_loaded();
}

uint64_t signal_intelligence_flight_inference_count(void) {
    return signal_brain_inference_count();
}

bool signal_intelligence_load_contract_checkpoint(const char *path,
                                                  char *err,
                                                  size_t err_size) {
    return signal_contract_brain_load_checkpoint(path, err, err_size);
}

bool signal_intelligence_contract_loaded(void) {
    return signal_contract_brain_loaded();
}

uint64_t signal_intelligence_contract_inference_count(void) {
    return signal_contract_brain_inference_count();
}

uint64_t signal_intelligence_contract_decision_count(void) {
    return signal_contract_brain_decision_count();
}

uint64_t signal_intelligence_contract_teacher_decision_count(void) {
    return signal_contract_brain_teacher_decision_count();
}

bool signal_intelligence_load_npc_worker_checkpoint(const char *path,
                                                    char *err,
                                                    size_t err_size) {
    return signal_npc_worker_brain_load_checkpoint(path, err, err_size);
}

bool signal_intelligence_npc_worker_loaded(void) {
    return signal_npc_worker_brain_loaded();
}

uint64_t signal_intelligence_npc_worker_inference_count(void) {
    return signal_npc_worker_brain_inference_count();
}

uint64_t signal_intelligence_npc_worker_decision_count(void) {
    return signal_npc_worker_brain_decision_count();
}

uint64_t signal_intelligence_npc_worker_teacher_decision_count(void) {
    return signal_npc_worker_brain_teacher_decision_count();
}

void signal_intelligence_drive_player(world_t *w, server_player_t *sp, float dt) {
    signal_brain_drive(w, sp, dt);
}

bool signal_intelligence_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target) {
    return signal_brain_drive_npc_to(w, npc, target);
}

void signal_intelligence_drive_npc(world_t *w, npc_ship_t *npc, float dt) {
    signal_brain_drive_npc(w, npc, dt);
}

int signal_intelligence_choose_contract(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count) {
    return signal_contract_brain_choose(w, sp, candidates, count);
}

const char *signal_intelligence_npc_worker_option_name(
    signal_npc_worker_option_t option) {
    return signal_npc_worker_option_name(option);
}

int signal_intelligence_choose_npc_worker(
    const signal_npc_worker_candidate_t *candidates,
    int count) {
    return signal_npc_worker_brain_choose(candidates, count);
}

int signal_intelligence_choose_npc_worker_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count) {
    return signal_npc_worker_brain_choose_with_scores(
        candidates, count, scores, score_count);
}
