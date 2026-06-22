#include "integration/work/signal/signal_client_brain.h"
#include "neural_singleplayer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

static bool neural_static_brain_ready = false;
static unsigned long long neural_flight_shadow_scored = 0;
static int neural_flight_shadow_last_best = -1;
static int neural_flight_shadow_last_intent = -2;

static bool neural_validate_contract(enum signal_brain_task task,
                                     const char *expected_name,
                                     const char *expected_feature_set,
                                     unsigned expected_encoder_version,
                                     size_t expected_input_count) {
    signal_client_brain_task_contract c = signal_client_brain_contract(task);
    float probe[SIGNAL_CLIENT_BRAIN_MAX_INPUT_COUNT] = {0.0f};
    float score = signal_client_brain_score(task, probe);
    const char *name = c.task_name ? c.task_name : "unknown";
    const char *feature_set = c.feature_set ? c.feature_set : "unknown";
    const char *hash = c.checkpoint_hash ? c.checkpoint_hash : "unknown";

    printf("[neural] static task=%s feature_set=%s encoder=%u inputs=%u "
           "hash=%.12s probe=%s%.6f\n",
           name,
           feature_set,
           c.feature_encoder_version,
           (unsigned)c.input_count,
           hash,
           isfinite(score) ? "" : "!",
           isfinite(score) ? score : 0.0f);

    if (!c.task_name || strcmp(c.task_name, expected_name) != 0 ||
        !c.feature_set || strcmp(c.feature_set, expected_feature_set) != 0 ||
        c.feature_encoder_version != expected_encoder_version ||
        c.input_count != expected_input_count ||
        !isfinite(score)) {
        printf("[neural] static contract mismatch for task=%s\n", expected_name);
        return false;
    }
    return true;
}

EXPORT
bool neural_singleplayer_init(void) {
    if (neural_static_brain_ready) return true;

    if (!neural_validate_contract(SIGNAL_BRAIN_TASK_FLIGHT,
                                  "flight",
                                  "signal-flight-live-v2",
                                  2u,
                                  SIGNAL_CLIENT_FLIGHT_INPUT_COUNT) ||
        !neural_validate_contract(SIGNAL_BRAIN_TASK_TACTICAL,
                                  "tactical",
                                  "signal-mining-grammar-v1",
                                  3u,
                                  SIGNAL_CLIENT_TACTICAL_INPUT_COUNT) ||
        !neural_validate_contract(SIGNAL_BRAIN_TASK_STRATEGIC,
                                  "strategic",
                                  "signal-npc-worker-v2",
                                  1u,
                                  SIGNAL_CLIENT_STRATEGIC_INPUT_COUNT)) {
        return false;
    }

    neural_static_brain_ready = true;
    printf("[neural] static client brain ready; backend=signal_client_brain\n");
    return true;
}

EXPORT
bool neural_singleplayer_ready(void) {
    return neural_static_brain_ready;
}

float neural_singleplayer_score(enum signal_brain_task task, const float *features) {
    if (!neural_static_brain_ready && !neural_singleplayer_init()) {
        return -INFINITY;
    }
    return signal_client_brain_score(task, features);
}

static const char *neural_flight_action_name(int action) {
    const signal_brain_flight_action_t *a = signal_brain_flight_action(action);
    return (a && a->name) ? a->name : "?";
}

static uint64_t neural_fnv1a64(const void *data, size_t len, uint64_t hash) {
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t neural_float_feature_hash(const float *features, size_t count) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < count; i++) {
        uint32_t bits = 0;
        unsigned char le[4];
        memcpy(&bits, &features[i], sizeof(bits));
        le[0] = (unsigned char)(bits & 0xffu);
        le[1] = (unsigned char)((bits >> 8) & 0xffu);
        le[2] = (unsigned char)((bits >> 16) & 0xffu);
        le[3] = (unsigned char)((bits >> 24) & 0xffu);
        hash = neural_fnv1a64(le, sizeof(le), hash);
    }
    return hash;
}

static uint16_t neural_allowed_mask(const uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT]) {
    uint16_t mask = 0;
    for (int i = 0; i < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT; i++) {
        if (allowed[i]) mask |= (uint16_t)(1u << i);
    }
    return mask;
}

static void neural_print_json_float(float value) {
    if (isfinite(value)) printf("%.9g", value);
    else printf("null");
}

static void neural_print_json_action(int action, float score) {
    printf("{\"index\":%d,\"name\":\"%s\",\"score\":",
           action,
           neural_flight_action_name(action));
    neural_print_json_float(score);
    printf("}");
}

static void neural_print_json_teacher(
    const neural_singleplayer_flight_shadow_t *shadow) {
    if (!shadow->teacher_available) {
        printf("null");
        return;
    }
    printf("{\"tick\":%u,\"index\":%d,\"name\":\"%s\",\"turn\":%d,"
           "\"thrust\":%d,\"score\":",
           (unsigned)shadow->teacher_tick,
           shadow->teacher_action,
           neural_flight_action_name(shadow->teacher_action),
           shadow->teacher_turn,
           shadow->teacher_thrust);
    neural_print_json_float(shadow->teacher_score);
    printf(",\"matches_best_allowed\":%s}",
           shadow->teacher_matches_best_allowed ? "true" : "false");
}

static void neural_log_flight_shadow_json(
    const world_t *w,
    const server_player_t *sp,
    const neural_singleplayer_flight_shadow_t *shadow) {
    const signal_client_brain_task_contract c =
        signal_client_brain_contract(SIGNAL_BRAIN_TASK_FLIGHT);
    printf("{\"schema\":\"crlp.signal_client_flight_shadow.v1\",");
    printf("\"sample\":%llu,", shadow->sample_index);
    printf("\"tick\":%u,", (unsigned)shadow->world_tick);
    printf("\"world_time\":");
    neural_print_json_float(w ? w->time : 0.0f);
    printf(",\"task\":\"flight\",");
    printf("\"feature_set\":\"%s\",", c.feature_set ? c.feature_set : "");
    printf("\"feature_encoder_version\":%u,", c.feature_encoder_version);
    printf("\"checkpoint_hash\":\"%s\",", c.checkpoint_hash ? c.checkpoint_hash : "");
    printf("\"autopilot_state\":%d,", shadow->autopilot_state);
    printf("\"autopilot_target\":%d,", shadow->autopilot_target);
    printf("\"autopilot_mode\":%u,", sp ? (unsigned)sp->autopilot_mode : 0u);
    printf("\"forward_blocked\":%s,", shadow->forward_blocked ? "true" : "false");
    printf("\"feature_hash\":\"%016llx\",", (unsigned long long)shadow->feature_hash);
    printf("\"allowed_mask\":\"0x%03x\",", (unsigned)shadow->allowed_mask);
    printf("\"best_raw\":");
    neural_print_json_action(shadow->best_raw_action, shadow->best_raw_score);
    printf(",\"best_allowed\":");
    neural_print_json_action(shadow->best_allowed_action, shadow->best_allowed_score);
    printf(",\"manual_intent\":");
    neural_print_json_action(shadow->intent_action, shadow->intent_score);
    printf(",\"teacher\":");
    neural_print_json_teacher(shadow);
    printf(",\"allowed_margin\":");
    neural_print_json_float(shadow->allowed_margin);
    printf(",\"actions\":[");
    for (int i = 0; i < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT; i++) {
        const signal_brain_flight_action_t *a = signal_brain_flight_action(i);
        if (i > 0) printf(",");
        printf("{\"index\":%d,\"name\":\"%s\",\"turn\":%d,\"thrust\":%d,"
               "\"allowed\":%s,\"score\":",
               i,
               a && a->name ? a->name : "?",
               a ? a->turn : 0,
               a ? a->thrust : 0,
               shadow->allowed[i] ? "true" : "false");
        neural_print_json_float(shadow->scores[i]);
        printf("}");
    }
    printf("]}\n");
}

bool neural_singleplayer_shadow_flight(
    const world_t *w,
    const server_player_t *sp,
    const input_intent_t *intent,
    neural_singleplayer_flight_shadow_t *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!neural_static_brain_ready || !w || !sp) return false;

    float features[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT *
                   SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT] = {0.0f};
    uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT] = {0};
    int forward_blocked = 0;
    bool teacher_available = sp->autopilot_teacher_valid != 0u;
    if (teacher_available) {
        memcpy(features, sp->autopilot_teacher_features, sizeof(features));
        for (int i = 0; i < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT; i++) {
            allowed[i] =
                (sp->autopilot_teacher_allowed_mask & (uint16_t)(1u << i))
                    ? 1u
                    : 0u;
        }
        forward_blocked = sp->autopilot_teacher_forward_blocked ? 1 : 0;
    } else {
        if (!signal_brain_build_flight_candidate_features(
                w, sp, features, allowed, &forward_blocked)) {
            return false;
        }
    }

    float scores[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT] = {0.0f};
    int best_raw = signal_client_brain_select_best(
        SIGNAL_BRAIN_TASK_FLIGHT,
        features,
        SIGNAL_BRAIN_FLIGHT_ACTION_COUNT,
        scores);

    int best_allowed = -1;
    float best_allowed_score = -INFINITY;
    float second_allowed_score = -INFINITY;
    for (int i = 0; i < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT; i++) {
        if (!allowed[i] || !isfinite(scores[i])) continue;
        if (scores[i] > best_allowed_score) {
            second_allowed_score = best_allowed_score;
            best_allowed_score = scores[i];
            best_allowed = i;
        } else if (scores[i] > second_allowed_score) {
            second_allowed_score = scores[i];
        }
    }

    int intent_action = signal_brain_flight_action_index_from_intent(intent);
    int teacher_action = teacher_available ? (int)sp->autopilot_teacher_action : -1;
    float intent_score = (intent_action >= 0)
        ? scores[intent_action]
        : -INFINITY;
    float teacher_score = (teacher_action >= 0 &&
                           teacher_action < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT)
        ? scores[teacher_action]
        : -INFINITY;
    float margin = (isfinite(best_allowed_score) && isfinite(second_allowed_score))
        ? best_allowed_score - second_allowed_score
        : 0.0f;
    uint64_t feature_hash = neural_float_feature_hash(
        features,
        SIGNAL_BRAIN_FLIGHT_ACTION_COUNT * SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT);
    uint16_t allowed_mask = neural_allowed_mask(allowed);

    neural_singleplayer_flight_shadow_t shadow = {0};
    shadow.scored = true;
    shadow.forward_blocked = forward_blocked != 0;
    shadow.world_tick = w->tick;
    shadow.autopilot_state = sp->autopilot_state;
    shadow.autopilot_target = sp->autopilot_target;
    shadow.best_raw_action = best_raw;
    shadow.best_allowed_action = best_allowed;
    shadow.intent_action = intent_action;
    shadow.teacher_available = teacher_available;
    shadow.teacher_tick = teacher_available ? sp->autopilot_teacher_tick : 0u;
    shadow.teacher_action = teacher_action;
    shadow.teacher_turn = teacher_available ? (int)sp->autopilot_teacher_turn : 0;
    shadow.teacher_thrust = teacher_available ? (int)sp->autopilot_teacher_thrust : 0;
    shadow.teacher_matches_best_allowed =
        teacher_available && teacher_action == best_allowed;
    shadow.best_raw_score = best_raw >= 0 ? scores[best_raw] : -INFINITY;
    shadow.best_allowed_score = best_allowed_score;
    shadow.intent_score = intent_score;
    shadow.teacher_score = teacher_score;
    shadow.allowed_margin = margin;
    shadow.feature_hash = feature_hash;
    shadow.allowed_mask = allowed_mask;
    memcpy(shadow.scores, scores, sizeof(shadow.scores));
    memcpy(shadow.allowed, allowed, sizeof(shadow.allowed));

    neural_flight_shadow_scored++;
    shadow.sample_index = neural_flight_shadow_scored;
    if (out) {
        *out = shadow;
    }

    if (best_allowed != neural_flight_shadow_last_best ||
        intent_action != neural_flight_shadow_last_intent ||
        (neural_flight_shadow_scored % 240u) == 0u) {
        neural_log_flight_shadow_json(w, sp, &shadow);
        neural_flight_shadow_last_best = best_allowed;
        neural_flight_shadow_last_intent = intent_action;
    }

    return best_allowed >= 0 || best_raw >= 0;
}
