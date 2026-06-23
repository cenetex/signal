/*
 * signal_intelligence.c -- Central AI/decision façade for Signal.
 */
#include "signal_intelligence.h"

#include <math.h>
#include <string.h>

#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
#include "signal_client_flight.h"
#endif

static uint64_t g_static_flight_inference_count = 0;

const char *signal_intelligence_backend_name(void) {
#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
    if (signal_intelligence_flight_builtin_available())
        return "crlplrimes-static-flight+legacy-facade";
#endif
    return "legacy-checkpoint-facade";
}

void signal_intelligence_holographic_init(void) {
    signal_brain_holographic_init();
}

bool signal_intelligence_flight_builtin_available(void) {
#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
    return signal_client_flight_feature_set &&
           strcmp(signal_client_flight_feature_set,
                  "signal-flight-live-v2") == 0 &&
           signal_client_flight_feature_encoder_version == 2u &&
           SIGNAL_CLIENT_FLIGHT_INPUT_COUNT ==
               SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT;
#else
    return false;
#endif
}

const char *signal_intelligence_flight_feature_set(void) {
#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
    if (signal_intelligence_flight_builtin_available())
        return signal_client_flight_feature_set;
#endif
    return "legacy-checkpoint";
}

uint32_t signal_intelligence_flight_feature_encoder_version(void) {
#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
    if (signal_intelligence_flight_builtin_available())
        return (uint32_t)signal_client_flight_feature_encoder_version;
#endif
    return 0;
}

const char *signal_intelligence_flight_checkpoint_hash(void) {
#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
    if (signal_intelligence_flight_builtin_available())
        return signal_client_flight_checkpoint_hash;
#endif
    return "";
}

bool signal_intelligence_load_flight_checkpoint(const char *path,
                                                char *err,
                                                size_t err_size) {
    if ((!path || path[0] == '\0') &&
        signal_intelligence_flight_builtin_available()) {
        if (err && err_size > 0) err[0] = '\0';
        return true;
    }
    return signal_brain_load_checkpoint(path, err, err_size);
}

bool signal_intelligence_flight_loaded(void) {
    return signal_intelligence_flight_builtin_available() ||
           signal_brain_loaded();
}

uint64_t signal_intelligence_flight_inference_count(void) {
    return g_static_flight_inference_count +
           signal_brain_inference_count();
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

static bool signal_intelligence_drive_static_flight(world_t *w,
                                                    server_player_t *sp) {
#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
    if (!signal_intelligence_flight_builtin_available() || !w || !sp ||
        sp->server_brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT ||
        sp->autopilot_mode == 0 || sp->docked) {
        return false;
    }

    float features[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT *
                   SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT] = {0.0f};
    uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT] = {0};
    int forward_blocked = 0;
    if (!signal_brain_build_flight_candidate_features(
            w, sp, features, allowed, &forward_blocked)) {
        return false;
    }

    if (forward_blocked) {
        sp->input.turn = 0.0f;
        sp->input.thrust = -1.0f;
        sp->input.reverse_thrust = true;
        sp->input.mine = false;
        return true;
    }

    float scores[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT] = {0.0f};
    int best_raw = signal_client_flight_select_best(
        features, SIGNAL_BRAIN_FLIGHT_ACTION_COUNT, scores);
    if (best_raw < 0) return false;

    g_static_flight_inference_count += SIGNAL_BRAIN_FLIGHT_ACTION_COUNT;

    int best = -1;
    float best_score = -INFINITY;
    for (int i = 0; i < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT; i++) {
        if (!allowed[i] || !isfinite(scores[i])) continue;
        if (best < 0 || scores[i] > best_score) {
            best = i;
            best_score = scores[i];
        }
    }
    if (best < 0) best = best_raw;

    const signal_brain_flight_action_t *action =
        signal_brain_flight_action(best);
    if (!action) return false;
    sp->input.turn = (float)action->turn;
    sp->input.thrust = (float)action->thrust;
    sp->input.reverse_thrust = false;
    return true;
#else
    (void)w;
    (void)sp;
    return false;
#endif
}

void signal_intelligence_drive_player(world_t *w, server_player_t *sp, float dt) {
    if (signal_intelligence_drive_static_flight(w, sp)) return;
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
