#include "holographic_nn_confidence.h"

#include <math.h>
#include <string.h>

#include "holographic_nn_calibration_data.h"

_Static_assert(HNN_CALIBRATION_CONTRACT_VERSION == HNN_CONTRACT_VERSION,
               "HNN calibration contract version is stale");
_Static_assert(HNN_CALIBRATION_DIMENSION == HNN_DIM,
               "HNN calibration dimension is stale");
_Static_assert(HNN_CALIBRATION_CONTRACT_SEED == HNN_CONTRACT_SEED,
               "HNN calibration seed is stale");
_Static_assert(HNN_CALIBRATION_KEYGEN_VERSION == HNN_KEYGEN_VERSION,
               "HNN calibration key generator is stale");
_Static_assert(HNN_CALIBRATION_ENCODER_VERSION == HNN_PILOT_ENCODER_VERSION,
               "HNN calibration encoder is stale");
_Static_assert(HNN_CALIBRATION_TRACE_FORMAT_VERSION == HNN_TRACE_FORMAT_VERSION,
               "HNN calibration trace format is stale");
_Static_assert(HNN_CALIBRATION_TRACE_CAPACITY == HNN_TRACE_CAPACITY,
               "HNN calibration trace capacity is stale");

static bool hnn_confidence_contract_matches(
    const hnn_memory_contract_t *contract) {
    return contract &&
           HNN_CONTRACT_VERSION == HNN_CALIBRATION_CONTRACT_VERSION &&
           contract->dim == HNN_CALIBRATION_DIMENSION &&
           contract->seed == HNN_CALIBRATION_CONTRACT_SEED &&
           contract->keygen_version == HNN_CALIBRATION_KEYGEN_VERSION &&
           contract->encoder_version == HNN_CALIBRATION_ENCODER_VERSION &&
           contract->action_vocabulary_hash ==
               HNN_CALIBRATION_ACTION_VOCABULARY_HASH &&
           contract->trace_format_version ==
               HNN_CALIBRATION_TRACE_FORMAT_VERSION;
}

hnn_confidence_thresholds_t hnn_confidence_thresholds(
    hnn_backend_kind_t backend) {
    hnn_confidence_thresholds_t out = {
        HNN_CALIBRATION_BUILTIN_MIN_SCORE,
        HNN_CALIBRATION_BUILTIN_MIN_MARGIN,
        HNN_CALIBRATION_MAX_CAPACITY_LOAD,
    };
    if (backend == HNN_BACKEND_LECORE_DIRECT) {
        out.min_score = HNN_CALIBRATION_DIRECT_MIN_SCORE;
        out.min_margin = HNN_CALIBRATION_DIRECT_MIN_MARGIN;
    } else if (backend == HNN_BACKEND_LECORE_RADIX2) {
        out.min_score = HNN_CALIBRATION_RADIX2_MIN_SCORE;
        out.min_margin = HNN_CALIBRATION_RADIX2_MIN_MARGIN;
    }
    return out;
}

hnn_confidence_decision_t hnn_confidence_evaluate(
    hnn_backend_kind_t backend,
    const hnn_memory_contract_t *contract,
    float best_score,
    float margin,
    bool legal,
    bool safe) {
    hnn_confidence_decision_t out = {
        false,
        HNN_CONFIDENCE_REJECT_EMPTY,
        hnn_confidence_thresholds(backend),
    };
    if (!contract || contract->stored_count <= 0) return out;
    if (!hnn_confidence_contract_matches(contract)) {
        out.reason = HNN_CONFIDENCE_REJECT_CONTRACT;
        return out;
    }
    if (!isfinite(contract->capacity_load) ||
        contract->capacity_load > out.thresholds.max_capacity_load ||
        contract->stored_count > (int)HNN_CALIBRATION_TRACE_CAPACITY) {
        out.reason = HNN_CONFIDENCE_REJECT_CAPACITY;
        return out;
    }
    if (!isfinite(best_score) || !isfinite(margin)) {
        out.reason = HNN_CONFIDENCE_REJECT_NONFINITE;
        return out;
    }
    if (best_score < out.thresholds.min_score) {
        out.reason = HNN_CONFIDENCE_REJECT_SCORE;
        return out;
    }
    if (margin < out.thresholds.min_margin) {
        out.reason = HNN_CONFIDENCE_REJECT_MARGIN;
        return out;
    }
    if (!legal) {
        out.reason = HNN_CONFIDENCE_REJECT_ILLEGAL;
        return out;
    }
    if (!safe) {
        out.reason = HNN_CONFIDENCE_REJECT_UNSAFE;
        return out;
    }
    out.accepted = true;
    out.reason = HNN_CONFIDENCE_ACCEPTED;
    return out;
}

hnn_confidence_mode_t hnn_confidence_mode_from_string(const char *value) {
    if (value && strcmp(value, "mixed") == 0)
        return HNN_CONFIDENCE_MODE_MIXED;
    return HNN_CONFIDENCE_MODE_SHADOW;
}

const char *hnn_confidence_mode_name(hnn_confidence_mode_t mode) {
    return mode == HNN_CONFIDENCE_MODE_MIXED ? "mixed" : "shadow";
}

const char *hnn_confidence_reason_name(hnn_confidence_reason_t reason) {
    switch (reason) {
    case HNN_CONFIDENCE_ACCEPTED: return "accepted";
    case HNN_CONFIDENCE_REJECT_EMPTY: return "empty";
    case HNN_CONFIDENCE_REJECT_CONTRACT: return "contract";
    case HNN_CONFIDENCE_REJECT_CAPACITY: return "capacity";
    case HNN_CONFIDENCE_REJECT_NONFINITE: return "nonfinite";
    case HNN_CONFIDENCE_REJECT_SCORE: return "score";
    case HNN_CONFIDENCE_REJECT_MARGIN: return "margin";
    case HNN_CONFIDENCE_REJECT_ILLEGAL: return "illegal";
    case HNN_CONFIDENCE_REJECT_UNSAFE: return "unsafe";
    default: return "unknown";
    }
}

int hnn_confidence_select_action(hnn_confidence_mode_t mode,
                                 const hnn_confidence_decision_t *decision,
                                 int hnn_action,
                                 int teacher_action) {
    if (mode == HNN_CONFIDENCE_MODE_MIXED && decision &&
        decision->accepted && hnn_action >= 0 &&
        hnn_action < HNN_ACTION_COUNT) {
        return hnn_action;
    }
    return teacher_action;
}
