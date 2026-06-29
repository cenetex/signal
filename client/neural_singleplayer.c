#include "integration/work/signal/signal_client_brain.h"
#include "neural_singleplayer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

_Static_assert((int)HNN_ACTION_COUNT == (int)SIGNAL_BRAIN_FLIGHT_ACTION_COUNT,
               "HNN and flight action vocabularies must stay aligned");

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define NEURAL_PI 3.14159265358979323846f

static bool neural_static_brain_ready = false;
static unsigned long long neural_flight_shadow_scored = 0;
static int neural_flight_shadow_last_best = -1;
static int neural_flight_shadow_last_intent = -2;
static bool neural_hnn_shadow_ready = false;
static hnn_memory_t neural_hnn_shadow_memory;
static hnn_holonet_t neural_hnn_shadow_holonet;
static hnn_action_table_t neural_hnn_shadow_actions;
static unsigned long long neural_hnn_shadow_scored = 0;
static int neural_hnn_shadow_last_allowed = -1;
static int neural_hnn_shadow_last_teacher = -2;

static void neural_hnn_shadow_init(void) {
    if (neural_hnn_shadow_ready) return;
    hnn_memory_init(&neural_hnn_shadow_memory);
    hnn_holonet_init(&neural_hnn_shadow_holonet);
    hnn_action_table_init(&neural_hnn_shadow_actions);
    hnn_memory_contract_t c = hnn_memory_contract(&neural_hnn_shadow_memory);
    printf("[neural] hnn shadow contract dim=%d seed=%016llx keygen=%u "
           "encoder=%u trace=%u action_hash=%016llx capacity=%u\n",
           c.dim,
           (unsigned long long)c.seed,
           c.keygen_version,
           c.encoder_version,
           c.trace_format_version,
           (unsigned long long)c.action_vocabulary_hash,
           (unsigned)HNN_TRACE_CAPACITY);
    neural_hnn_shadow_ready = true;
}

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
    neural_hnn_shadow_init();
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

static float neural_clampf(float value, float lo, float hi) {
    if (!isfinite(value)) return 0.0f;
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void neural_hnn_features_from_flight_row(
    const float *features,
    int action,
    int forward_blocked,
    hnn_pilot_features_t *out) {
    const float *row = features;
    const signal_brain_flight_action_t *a = signal_brain_flight_action(action);
    if (!out || !features) return;
    if (action >= 0 && action < SIGNAL_BRAIN_FLIGHT_ACTION_COUNT) {
        row = &features[action * SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT];
    }
    memset(out, 0, sizeof(*out));

    float heading_cos = neural_clampf(row[11], -1.0f, 1.0f);
    float heading_sin = neural_clampf(row[12], -1.0f, 1.0f);
    int turn = a ? a->turn : 0;
    int thrust = a ? a->thrust : 0;

    out->target_dist = neural_clampf(row[9], 0.0f, 1.0f);
    out->heading_error = neural_clampf(atan2f(heading_sin, heading_cos) /
                                       NEURAL_PI,
                                       -1.0f,
                                       1.0f);
    out->heading_cos = heading_cos;
    out->heading_sin = heading_sin;
    out->speed = neural_clampf(row[15], 0.0f, 1.0f);
    out->forward_speed = neural_clampf(row[16], -1.0f, 1.0f);
    out->lateral_speed = neural_clampf(row[17], -1.0f, 1.0f);
    out->brake_distance = neural_clampf(row[18], 0.0f, 1.0f);
    out->fwd_clear = neural_clampf(row[19], 0.0f, 1.0f);
    out->left_clear = neural_clampf(row[20], 0.0f, 1.0f);
    out->right_clear = neural_clampf(row[21], 0.0f, 1.0f);
    out->signal_quality = neural_clampf(row[23], 0.0f, 1.0f);
    out->hull_ratio = neural_clampf(row[24], 0.0f, 1.0f);
    out->path_count = neural_clampf(row[25], 0.0f, 1.0f);
    out->path_current = neural_clampf(row[26], 0.0f, 1.0f);
    out->fwd_blocked = forward_blocked ? 1.0f : 0.0f;
    out->left_blocked = 0.0f;
    out->right_blocked = 0.0f;
    out->goal_close = neural_clampf(1.0f - out->target_dist, 0.0f, 1.0f);
    out->action_delta_turn = (float)turn;
    out->action_delta_thrust = (float)thrust;
    out->action_is_none = (action == 0) ? 1.0f : 0.0f;
    out->action_is_reverse = (thrust < 0) ? 1.0f : 0.0f;
    out->composite_dot = (float)turn * out->heading_sin;
}

static int neural_hnn_best_allowed(
    const float scores[HNN_ACTION_COUNT],
    const uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT],
    float *out_score,
    float *out_margin) {
    int best = -1;
    float best_score = -INFINITY;
    float second_score = -INFINITY;
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        if (!allowed[i] || !isfinite(scores[i])) continue;
        if (scores[i] > best_score) {
            second_score = best_score;
            best_score = scores[i];
            best = i;
        } else if (scores[i] > second_score) {
            second_score = scores[i];
        }
    }
    if (out_score) *out_score = best_score;
    if (out_margin) {
        *out_margin = (isfinite(best_score) && isfinite(second_score))
            ? best_score - second_score
            : 0.0f;
    }
    return best;
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

static void neural_print_json_hnn_contract(
    const hnn_memory_contract_t *contract) {
    printf("{\"dim\":%d,", contract->dim);
    printf("\"seed\":\"%016llx\",", (unsigned long long)contract->seed);
    printf("\"keygen_version\":%u,", contract->keygen_version);
    printf("\"encoder_version\":%u,", contract->encoder_version);
    printf("\"action_vocabulary_hash\":\"%016llx\",",
           (unsigned long long)contract->action_vocabulary_hash);
    printf("\"trace_format_version\":%u,", contract->trace_format_version);
    printf("\"stored_count\":%d,", contract->stored_count);
    printf("\"capacity_load\":");
    neural_print_json_float(contract->capacity_load);
    printf(",\"fidelity_estimate\":");
    neural_print_json_float(contract->fidelity_estimate);
    printf(",\"last_margin\":");
    neural_print_json_float(contract->last_margin);
    printf("}");
}

static void neural_print_json_hnn_teacher(int teacher_available,
                                          uint32_t teacher_tick,
                                          int teacher_action,
                                          int teacher_turn,
                                          int teacher_thrust,
                                          float teacher_score,
                                          bool matches_top,
                                          bool matches_allowed) {
    if (!teacher_available) {
        printf("null");
        return;
    }
    printf("{\"tick\":%u,\"index\":%d,\"name\":\"%s\",\"turn\":%d,"
           "\"thrust\":%d,\"score\":",
           (unsigned)teacher_tick,
           teacher_action,
           neural_flight_action_name(teacher_action),
           teacher_turn,
           teacher_thrust);
    neural_print_json_float(teacher_score);
    printf(",\"matches_hnn_top\":%s,\"matches_best_allowed\":%s}",
           matches_top ? "true" : "false",
           matches_allowed ? "true" : "false");
}

static void neural_log_hnn_shadow_json(
    const world_t *w,
    const hnn_memory_contract_t *contract,
    unsigned long long sample_index,
    uint64_t feature_hash,
    uint16_t allowed_mask,
    bool forward_blocked,
    int hnn_top,
    int hnn_top_allowed,
    int teacher_action,
    int teacher_turn,
    int teacher_thrust,
    uint32_t teacher_tick,
    float teacher_score,
    float hnn_top_allowed_score,
    float allowed_margin,
    const float scores[HNN_ACTION_COUNT],
    const uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT]) {
    printf("{\"schema\":\"crlp.signal_hnn_shadow.v1\",");
    printf("\"sample\":%llu,", sample_index);
    printf("\"tick\":%u,", w ? (unsigned)w->tick : 0u);
    printf("\"world_time\":");
    neural_print_json_float(w ? w->time : 0.0f);
    printf(",\"task\":\"flight\",");
    printf("\"feature_hash\":\"%016llx\",", (unsigned long long)feature_hash);
    printf("\"allowed_mask\":\"0x%03x\",", (unsigned)allowed_mask);
    printf("\"forward_blocked\":%s,", forward_blocked ? "true" : "false");
    printf("\"contract\":");
    neural_print_json_hnn_contract(contract);
    hnn_memory_contract_t holonet_contract =
        hnn_holonet_contract(&neural_hnn_shadow_holonet);
    printf(",\"holonet\":{\"enabled\":%s,\"active_count\":%d,"
           "\"last_route\":%d,\"scored_count\":%d,\"route_similarity\":",
           hnn_holonet_active_count(&neural_hnn_shadow_holonet) > 0
               ? "true" : "false",
           hnn_holonet_active_count(&neural_hnn_shadow_holonet),
           neural_hnn_shadow_holonet.last_route,
           neural_hnn_shadow_holonet.last_scored_count);
    neural_print_json_float(neural_hnn_shadow_holonet.last_route_similarity);
    printf(",\"contract\":");
    neural_print_json_hnn_contract(&holonet_contract);
    printf("}");
    printf(",\"stored_count\":%d,", contract->stored_count);
    printf("\"capacity_load\":");
    neural_print_json_float(contract->capacity_load);
    printf(",\"trace_fidelity\":");
    neural_print_json_float(contract->fidelity_estimate);
    printf(",\"last_margin\":");
    neural_print_json_float(contract->last_margin);
    printf(",\"hnn_top\":");
    neural_print_json_action(hnn_top, hnn_top >= 0 ? scores[hnn_top] : -INFINITY);
    printf(",\"hnn_top_allowed\":");
    neural_print_json_action(hnn_top_allowed, hnn_top_allowed_score);
    printf(",\"best_allowed\":");
    neural_print_json_action(hnn_top_allowed, hnn_top_allowed_score);
    printf(",\"teacher\":");
    neural_print_json_hnn_teacher(
        teacher_action >= 0,
        teacher_tick,
        teacher_action,
        teacher_turn,
        teacher_thrust,
        teacher_score,
        teacher_action == hnn_top,
        teacher_action == hnn_top_allowed);
    printf(",\"margin\":");
    neural_print_json_float(allowed_margin);
    printf(",\"allowed_margin\":");
    neural_print_json_float(allowed_margin);
    printf(",\"actions\":[");
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        const signal_brain_flight_action_t *a = signal_brain_flight_action(i);
        if (i > 0) printf(",");
        printf("{\"index\":%d,\"name\":\"%s\",\"turn\":%d,\"thrust\":%d,"
               "\"allowed\":%s,\"score\":",
               i,
               a && a->name ? a->name : "?",
               a ? a->turn : 0,
               a ? a->thrust : 0,
               allowed[i] ? "true" : "false");
        neural_print_json_float(scores[i]);
        printf("}");
    }
    printf("]}\n");
}

static void neural_hnn_shadow_observe(
    const world_t *w,
    const float *features,
    const uint8_t allowed[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT],
    int forward_blocked,
    int teacher_action,
    int teacher_turn,
    int teacher_thrust,
    uint32_t teacher_tick,
    uint64_t feature_hash,
    uint16_t allowed_mask) {
    if (!features || !allowed ||
        teacher_action < 0 ||
        teacher_action >= SIGNAL_BRAIN_FLIGHT_ACTION_COUNT) {
        return;
    }
    neural_hnn_shadow_init();

    hnn_pilot_features_t state_only;
    neural_hnn_features_from_flight_row(
        features, 0, forward_blocked, &state_only);

    float scores[HNN_ACTION_COUNT] = {0.0f};
    float raw_margin = 0.0f;
    int hnn_top = -1;
    if (hnn_holonet_active_count(&neural_hnn_shadow_holonet) > 0) {
        hnn_top = hnn_holonet_score_actions(&neural_hnn_shadow_holonet,
                                            &neural_hnn_shadow_actions,
                                            &state_only,
                                            scores,
                                            &raw_margin,
                                            NULL,
                                            0);
    }
    if (hnn_top < 0) {
        hnn_top = hnn_score_actions(&neural_hnn_shadow_memory,
                                    &neural_hnn_shadow_actions,
                                    &state_only,
                                    scores,
                                    &raw_margin,
                                    NULL,
                                    0);
    }
    if (hnn_top < 0) hnn_top = 0;

    float allowed_score = -INFINITY;
    float allowed_margin = 0.0f;
    int hnn_allowed = neural_hnn_best_allowed(
        scores, allowed, &allowed_score, &allowed_margin);
    float selected_margin = hnn_allowed >= 0 ? allowed_margin : raw_margin;
    neural_hnn_shadow_memory.last_retrieval_similarity =
        hnn_top >= 0 ? scores[hnn_top] : 0.0f;
    neural_hnn_shadow_memory.last_margin = selected_margin;

    hnn_memory_contract_t contract =
        hnn_memory_contract(&neural_hnn_shadow_memory);
    float teacher_score = scores[teacher_action];

    neural_hnn_shadow_scored++;
    if (hnn_allowed != neural_hnn_shadow_last_allowed ||
        teacher_action != neural_hnn_shadow_last_teacher ||
        (neural_hnn_shadow_scored % 240u) == 0u) {
        neural_log_hnn_shadow_json(w,
                                   &contract,
                                   neural_hnn_shadow_scored,
                                   feature_hash,
                                   allowed_mask,
                                   forward_blocked != 0,
                                   hnn_top,
                                   hnn_allowed,
                                   teacher_action,
                                   teacher_turn,
                                   teacher_thrust,
                                   teacher_tick,
                                   teacher_score,
                                   allowed_score,
                                   allowed_margin,
                                   scores,
                                   allowed);
        neural_hnn_shadow_last_allowed = hnn_allowed;
        neural_hnn_shadow_last_teacher = teacher_action;
    }

    hnn_pilot_features_t full_state;
    neural_hnn_features_from_flight_row(
        features, teacher_action, forward_blocked, &full_state);
    float route_vec[HNN_DIM];
    float state_vec[HNN_DIM];
    hnn_encode_state(&state_only, route_vec);
    hnn_encode_state(&full_state, state_vec);
    hnn_memory_store(&neural_hnn_shadow_memory,
                     state_vec,
                     neural_hnn_shadow_actions.vecs[teacher_action]);
    hnn_holonet_store(&neural_hnn_shadow_holonet,
                      route_vec,
                      state_vec,
                      neural_hnn_shadow_actions.vecs[teacher_action]);
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

    if (teacher_available) {
        neural_hnn_shadow_observe(w,
                                  features,
                                  allowed,
                                  forward_blocked,
                                  teacher_action,
                                  shadow.teacher_turn,
                                  shadow.teacher_thrust,
                                  shadow.teacher_tick,
                                  feature_hash,
                                  allowed_mask);
    }

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
