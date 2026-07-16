/*
 * signal_intelligence.c -- Central AI/decision façade for Signal.
 */
#include "signal_intelligence.h"

#include "fixpoint.h"
#include "sim_ai.h"
#include "station_util.h"

#include <math.h>
#include <string.h>

#ifdef SIGNAL_HAS_STATIC_FLIGHT_BRAIN
#include "signal_client_flight.h"
#endif

static uint64_t g_static_flight_inference_count = 0;

static void signal_intelligence_reason_reset(
    signal_intelligence_decision_reason_t *reason,
    signal_intelligence_task_t task,
    int count) {
    if (!reason) return;
    memset(reason, 0, sizeof(*reason));
    reason->task = task;
    reason->selected_index = -1;
    reason->candidate_count = count > 0 ? count : 0;
}

static uint64_t signal_intelligence_reason_id(uint32_t task,
                                              int source_station,
                                              int dest_station,
                                              int commodity,
                                              uint32_t extra) {
    uint64_t x = 1469598103934665603ull;
    uint32_t fields[5] = {
        task,
        (uint32_t)source_station,
        (uint32_t)dest_station,
        (uint32_t)commodity,
        extra,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        x ^= (uint64_t)fields[i];
        x *= 1099511628211ull;
    }
    return x;
}

static void signal_intelligence_reason_flag_pressure(
    signal_intelligence_decision_reason_t *reason) {
    if (!reason) return;
    if (reason->hologram_resonance > 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_HOLOGRAM;
    if (reason->source_memory > 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_SOURCE_MEMORY;
    if (reason->proof_memory > 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_PROOF_MEMORY;
    if (reason->route_risk > 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_ROUTE_RISK;
    if (reason->trust_bias != 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_TRUST_BIAS;
    if (reason->signal_quality > 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_SIGNAL_CONTEXT;
    if (reason->frontier_pressure > 0.0f)
        reason->flags |= SIGNAL_DECISION_REASON_HAS_FRONTIER_PRESSURE;
}

static void signal_intelligence_reason_from_flight(
    signal_intelligence_decision_reason_t *reason,
    const signal_brain_flight_decision_t *flight,
    bool used_neural) {
    if (!reason || !flight) return;
    reason->task = SIGNAL_INTEL_TASK_FLIGHT_CONTROL;
    reason->selected_index = flight->selected_index;
    reason->candidate_count = flight->candidate_count;
    reason->selected_score = flight->selected_score;
    reason->route_risk = flight->route_risk;
    reason->signal_quality = flight->signal_quality;
    if (used_neural) {
        reason->neural_score = flight->selected_score;
        reason->flags |= SIGNAL_DECISION_REASON_USED_NEURAL;
    }
    if (flight->hard_override)
        reason->flags |= SIGNAL_DECISION_REASON_HARD_OVERRIDE;
    reason->flags |= SIGNAL_DECISION_REASON_ADVISORY_ONLY |
                     SIGNAL_DECISION_REASON_HARD_APPROVED;
    signal_intelligence_reason_flag_pressure(reason);
}

static void signal_intelligence_reason_from_frontier(
    signal_intelligence_decision_reason_t *reason,
    const frontier_director_decision_t *frontier) {
    if (!reason || !frontier) return;
    reason->task = SIGNAL_INTEL_TASK_FRONTIER_PLAN;
    reason->selected_index = (int)frontier->action;
    reason->candidate_count = frontier->plan_limit;
    reason->selected_score = frontier->frontier_pressure;
    reason->teacher_score = frontier->frontier_pressure;
    reason->frontier_pressure = frontier->frontier_pressure;
    reason->source_memory_id = signal_intelligence_reason_id(
        (uint32_t)SIGNAL_INTEL_TASK_FRONTIER_PLAN,
        frontier->planned_after,
        frontier->plan_limit,
        frontier->virtual_pilots,
        (uint32_t)frontier->action);
    reason->flags |= SIGNAL_DECISION_REASON_USED_TEACHER |
                     SIGNAL_DECISION_REASON_ADVISORY_ONLY |
                     SIGNAL_DECISION_REASON_HARD_APPROVED;
    signal_intelligence_reason_flag_pressure(reason);
}

static void signal_intelligence_snapshot_frontier_reason(
    world_t *w,
    const signal_intelligence_decision_reason_t *reason) {
    if (!w || !reason || reason->task != SIGNAL_INTEL_TASK_FRONTIER_PLAN)
        return;
    w->frontier_decision_valid = 1u;
    w->frontier_decision_action = reason->selected_index < 0
        ? 0u
        : (uint8_t)reason->selected_index;
    w->frontier_decision_plan_limit = reason->candidate_count < 0
        ? 0u
        : (uint16_t)reason->candidate_count;
    w->frontier_decision_flags = reason->flags;
    w->frontier_decision_score = reason->selected_score;
    w->frontier_decision_pressure = reason->frontier_pressure;
    w->frontier_decision_source_id = reason->source_memory_id;
}

void signal_intelligence_step_frontier_director(world_t *w, float dt) {
    signal_intelligence_decision_reason_t reason;
    (void)signal_intelligence_step_frontier_director_with_reason(w, dt, &reason);
}

bool signal_intelligence_step_frontier_director_with_reason(
    world_t *w,
    float dt,
    signal_intelligence_decision_reason_t *reason) {
    signal_intelligence_reason_reset(
        reason, SIGNAL_INTEL_TASK_FRONTIER_PLAN, 0);
    frontier_director_decision_t frontier;
    bool ran = frontier_director_step_with_decision(w, dt, &frontier);
    if (!ran) return false;
    if (reason)
        signal_intelligence_reason_from_frontier(reason, &frontier);
    else {
        signal_intelligence_decision_reason_t local;
        signal_intelligence_reason_reset(
            &local, SIGNAL_INTEL_TASK_FRONTIER_PLAN, 0);
        signal_intelligence_reason_from_frontier(&local, &frontier);
        signal_intelligence_snapshot_frontier_reason(w, &local);
        return true;
    }
    signal_intelligence_snapshot_frontier_reason(w, reason);
    return true;
}

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

static bool signal_intelligence_drive_static_flight(
    world_t *w,
    server_player_t *sp,
    signal_intelligence_decision_reason_t *reason) {
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
        if (reason) {
            signal_brain_flight_decision_t flight = {0};
            flight.selected_index = 4;
            flight.raw_index = 4;
            flight.candidate_count = SIGNAL_BRAIN_FLIGHT_ACTION_COUNT;
            flight.route_risk = 1.0f;
            flight.signal_quality = features[23];
            flight.hard_override = true;
            flight.forward_blocked = true;
            signal_intelligence_reason_from_flight(reason, &flight, false);
        }
        return true;
    }

    float scores[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT] = {0.0f};
    int best_raw = signal_client_flight_select_best(
        features, SIGNAL_BRAIN_FLIGHT_ACTION_COUNT, scores);
    if (best_raw < 0) return false;

    g_static_flight_inference_count += SIGNAL_BRAIN_FLIGHT_ACTION_COUNT;

    int best = -1;
    float best_score = -INFINITY;
    uint16_t allowed_mask = 0;
    for (int i = 0; i < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT; i++) {
        if (allowed[i]) allowed_mask |= (uint16_t)(1u << i);
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
    if (reason) {
        signal_brain_flight_decision_t flight = {0};
        flight.selected_index = best;
        flight.raw_index = best_raw;
        flight.candidate_count = SIGNAL_BRAIN_FLIGHT_ACTION_COUNT;
        flight.selected_score = best >= 0 ? scores[best] : 0.0f;
        flight.raw_score = best_raw >= 0 ? scores[best_raw] : 0.0f;
        flight.signal_quality = features[23];
        flight.route_risk = 1.0f - features[19];
        if (flight.route_risk < 0.0f) flight.route_risk = 0.0f;
        if (flight.route_risk > 1.0f) flight.route_risk = 1.0f;
        flight.hull_ratio = features[24];
        flight.allowed_mask = allowed_mask;
        signal_intelligence_reason_from_flight(reason, &flight, true);
    }
    return true;
#else
    (void)w;
    (void)sp;
    (void)reason;
    return false;
#endif
}

void signal_intelligence_drive_player(world_t *w, server_player_t *sp, float dt) {
    (void)signal_intelligence_drive_player_with_reason(w, sp, dt, NULL);
}

bool signal_intelligence_drive_player_with_reason(
    world_t *w,
    server_player_t *sp,
    float dt,
    signal_intelligence_decision_reason_t *reason) {
    signal_intelligence_reason_reset(
        reason, SIGNAL_INTEL_TASK_FLIGHT_CONTROL,
        SIGNAL_BRAIN_FLIGHT_ACTION_COUNT);
    if (signal_intelligence_drive_static_flight(w, sp, reason)) return true;

    signal_brain_flight_decision_t flight = {0};
    bool drove = signal_brain_drive_with_decision(
        w, sp, dt, reason ? &flight : NULL);
    if (drove && reason)
        signal_intelligence_reason_from_flight(reason, &flight, true);
    return drove;
}

bool signal_intelligence_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target) {
    return signal_brain_drive_npc_to(w, npc, target);
}

void signal_intelligence_drive_npc(world_t *w, npc_ship_t *npc, float dt) {
    signal_brain_drive_npc(w, npc, dt);
}

static void signal_intelligence_finish_hail_reason(
    signal_intelligence_decision_reason_t *reason,
    const server_player_t *sp,
    int selected_station,
    int candidate_count,
    float score,
    uint32_t mode) {
    if (!reason) return;
    reason->selected_index = selected_station;
    reason->candidate_count = candidate_count > 0 ? candidate_count : 0;
    reason->selected_score = score;
    reason->teacher_score = score;
    reason->signal_quality = score;
    reason->flags |= SIGNAL_DECISION_REASON_USED_TEACHER;
    if (selected_station >= 0) {
        int nearby = sp ? sp->nearby_station : -1;
        reason->source_memory_id = signal_intelligence_reason_id(
            (uint32_t)SIGNAL_INTEL_TASK_HAIL_CHOICE,
            selected_station,
            nearby,
            -1,
            mode);
        reason->flags |= SIGNAL_DECISION_REASON_ADVISORY_ONLY |
                         SIGNAL_DECISION_REASON_HARD_APPROVED;
    }
    signal_intelligence_reason_flag_pressure(reason);
}

int signal_intelligence_choose_hail_station(
    const world_t *w,
    const server_player_t *sp) {
    return signal_intelligence_choose_hail_station_with_reason(w, sp, NULL);
}

int signal_intelligence_choose_hail_station_with_reason(
    const world_t *w,
    const server_player_t *sp,
    signal_intelligence_decision_reason_t *reason) {
    signal_intelligence_reason_reset(reason, SIGNAL_INTEL_TASK_HAIL_CHOICE, 0);
    if (!w || !sp) return -1;

    if (sp->docked) {
        int station_idx = sp->current_station;
        bool valid = station_idx >= 0 && station_idx < MAX_STATIONS &&
            station_is_active(&w->stations[station_idx]);
        signal_intelligence_finish_hail_reason(
            reason, sp, valid ? station_idx : -1, valid ? 1 : 0,
            valid ? 1.0f : 0.0f, 1u);
        return valid ? station_idx : -1;
    }

    if (sp->in_dock_range && sp->nearby_station >= 0 &&
        sp->nearby_station < MAX_STATIONS &&
        station_is_active(&w->stations[sp->nearby_station])) {
        signal_intelligence_finish_hail_reason(
            reason, sp, sp->nearby_station, 1, 1.0f, 2u);
        return sp->nearby_station;
    }

    float comm = (sp->ship->comm_range > 0.0f) ? sp->ship->comm_range : 1500.0f;
    int best_station = -1;
    float best_d = INFINITY;
    float best_quality = 0.0f;
    int candidate_count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w->stations[s];
        if (!station_is_active(st)) continue;

        float scan = st->signal_range;
        float comm_fallback = comm * 2.0f;
        if (scan < comm_fallback) scan = comm_fallback;
        if (scan <= 0.0f) continue;

        float d_sq = v2_dist_sq(sp->ship->pos, st->pos);
        if (d_sq > scan * scan) continue;
        candidate_count++;
        if (d_sq < best_d) {
            best_d = d_sq;
            best_station = s;
            float d = fixp_sqrtf(d_sq);
            best_quality = clampf(1.0f - d / scan, 0.0f, 1.0f);
        }
    }

    signal_intelligence_finish_hail_reason(
        reason, sp, best_station, candidate_count, best_quality, 3u);
    return best_station;
}

int signal_intelligence_choose_contract(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count) {
    return signal_intelligence_choose_contract_with_reason(
        w, sp, candidates, count, NULL);
}

int signal_intelligence_choose_contract_with_reason(
    const world_t *w,
    const server_player_t *sp,
    const signal_contract_candidate_t *candidates,
    int count,
    signal_intelligence_decision_reason_t *reason) {
    signal_intelligence_reason_reset(
        reason, SIGNAL_INTEL_TASK_CONTRACT_CHOICE, count);
    double scores[MAX_CONTRACTS + 1] = {0.0};
    int score_count = count <= MAX_CONTRACTS + 1 ? count : MAX_CONTRACTS + 1;
    bool loaded = signal_contract_brain_loaded();
    int choice = signal_contract_brain_choose_with_scores(
        w, sp, candidates, count, reason ? scores : NULL, score_count);
    if (!reason || choice < 0 || choice >= count) return choice;

    const signal_contract_candidate_t *picked = &candidates[choice];
    reason->selected_index = choice;
    reason->teacher_score = picked->teacher_score;
    reason->selected_score = choice < score_count
        ? (float)scores[choice]
        : picked->teacher_score;
    if (loaded) {
        reason->neural_score = reason->selected_score;
        reason->flags |= SIGNAL_DECISION_REASON_USED_NEURAL;
    } else {
        reason->flags |= SIGNAL_DECISION_REASON_USED_TEACHER;
    }
    reason->hologram_resonance = picked->hologram_resonance;
    reason->source_memory = picked->source_memory;
    reason->proof_memory = picked->route_proof_memory;
    reason->route_success = picked->route_success_memory;
    reason->route_risk = picked->route_danger_memory;
    reason->trust_bias = picked->trust_bias;
    reason->source_memory_id = picked->source_memory_id;
    if (reason->source_memory_id == 0ull &&
        (picked->source_memory > 0.0f ||
         picked->route_proof_memory > 0.0f ||
         picked->route_success_memory > 0.0f ||
         picked->route_danger_memory > 0.0f)) {
        reason->source_memory_id = signal_intelligence_reason_id(
            (uint32_t)SIGNAL_INTEL_TASK_CONTRACT_CHOICE,
            picked->source_station,
            picked->dest_station,
            (int)picked->commodity,
            (uint32_t)picked->action);
    }
    reason->flags |= SIGNAL_DECISION_REASON_ADVISORY_ONLY |
                     SIGNAL_DECISION_REASON_HARD_APPROVED;
    signal_intelligence_reason_flag_pressure(reason);
    return choice;
}

const char *signal_intelligence_npc_worker_option_name(
    signal_npc_worker_option_t option) {
    return signal_npc_worker_option_name(option);
}

int signal_intelligence_choose_npc_worker(
    const signal_npc_worker_candidate_t *candidates,
    int count) {
    return signal_intelligence_choose_npc_worker_with_reason(
        candidates, count, NULL);
}

int signal_intelligence_choose_npc_worker_with_reason(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    signal_intelligence_decision_reason_t *reason) {
    return signal_intelligence_choose_npc_worker_with_scores_and_reason(
        candidates, count, NULL, 0, reason);
}

int signal_intelligence_choose_npc_worker_with_scores(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count) {
    return signal_intelligence_choose_npc_worker_with_scores_and_reason(
        candidates, count, scores, score_count, NULL);
}

int signal_intelligence_choose_npc_worker_with_scores_and_reason(
    const signal_npc_worker_candidate_t *candidates,
    int count,
    double *scores,
    int score_count,
    signal_intelligence_decision_reason_t *reason) {
    signal_intelligence_reason_reset(
        reason, SIGNAL_INTEL_TASK_NPC_WORKER_ASSIGNMENT, count);
    double local_scores[SIGNAL_NPC_WORKER_OPTION_COUNT] = {0.0};
    double *score_dst = scores;
    int score_dst_count = score_count;
    if (reason && (!score_dst || score_dst_count < count) &&
        count <= SIGNAL_NPC_WORKER_OPTION_COUNT) {
        score_dst = local_scores;
        score_dst_count = SIGNAL_NPC_WORKER_OPTION_COUNT;
    }

    bool loaded = signal_npc_worker_brain_loaded();
    int choice = signal_npc_worker_brain_choose_with_scores(
        candidates, count, score_dst, score_dst_count);
    if (reason && scores && score_dst == local_scores && score_count > 0) {
        int copy = score_count < count ? score_count : count;
        for (int i = 0; i < copy; i++) scores[i] = local_scores[i];
    }
    if (!reason || choice < 0 || choice >= count) return choice;

    const signal_npc_worker_candidate_t *picked = &candidates[choice];
    reason->selected_index = choice;
    reason->teacher_score = picked->teacher_score;
    reason->selected_score = choice < score_dst_count
        ? (float)score_dst[choice]
        : picked->teacher_score;
    bool neural_score_used =
        loaded &&
        (int)picked->option >= 0 &&
        (int)picked->option < SIGNAL_NPC_WORKER_OPTION_V1_COUNT;
    if (neural_score_used) {
        reason->neural_score = reason->selected_score;
        reason->flags |= SIGNAL_DECISION_REASON_USED_NEURAL;
    } else {
        reason->flags |= SIGNAL_DECISION_REASON_USED_TEACHER;
        if (loaded)
            reason->flags |= SIGNAL_DECISION_REASON_FALLBACK_SCORE;
    }
    reason->hologram_resonance = picked->hologram_resonance;
    reason->source_memory = picked->source_memory;
    reason->proof_memory = picked->route_proof_memory;
    reason->route_success = picked->route_success_memory;
    reason->route_risk = picked->route_danger_memory;
    reason->trust_bias = picked->trust_bias;
    if (picked->source_memory > 0.0f ||
        picked->route_proof_memory > 0.0f ||
        picked->route_success_memory > 0.0f ||
        picked->route_danger_memory > 0.0f ||
        picked->hologram_resonance > 0.0f) {
        reason->source_memory_id = signal_intelligence_reason_id(
            (uint32_t)SIGNAL_INTEL_TASK_NPC_WORKER_ASSIGNMENT,
            picked->home_station,
            picked->best_contract_dest,
            (int)picked->best_contract_commodity,
            (uint32_t)picked->option);
    }
    reason->flags |= SIGNAL_DECISION_REASON_ADVISORY_ONLY |
                     SIGNAL_DECISION_REASON_HARD_APPROVED;
    signal_intelligence_reason_flag_pressure(reason);
    return choice;
}
