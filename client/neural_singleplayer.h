#ifndef NEURAL_SINGLEPLAYER_H
#define NEURAL_SINGLEPLAYER_H
#include "integration/work/signal/signal_client_brain.h"
#include "signal_brain.h"
#include <stdbool.h>

typedef struct neural_singleplayer_flight_shadow {
    bool scored;
    bool forward_blocked;
    uint32_t world_tick;
    int autopilot_state;
    int autopilot_target;
    int best_raw_action;
    int best_allowed_action;
    int intent_action;
    bool teacher_available;
    uint32_t teacher_tick;
    int teacher_action;
    int teacher_turn;
    int teacher_thrust;
    bool teacher_matches_best_allowed;
    float best_raw_score;
    float best_allowed_score;
    float intent_score;
    float teacher_score;
    float allowed_margin;
    uint64_t feature_hash;
    uint16_t allowed_mask;
    unsigned long long sample_index;
    float scores[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT];
    uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT];
} neural_singleplayer_flight_shadow_t;

bool neural_singleplayer_init(void);
bool neural_singleplayer_ready(void);
float neural_singleplayer_score(enum signal_brain_task task, const float *features);
bool neural_singleplayer_shadow_flight(
    const world_t *w,
    const server_player_t *sp,
    const input_intent_t *intent,
    neural_singleplayer_flight_shadow_t *out);
#endif
