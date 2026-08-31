#ifndef HOLOGRAPHIC_NN_CONFIDENCE_H
#define HOLOGRAPHIC_NN_CONFIDENCE_H

#include <stdbool.h>

#include "holographic_nn_backend.h"

typedef enum hnn_confidence_mode {
    HNN_CONFIDENCE_MODE_SHADOW = 0,
    HNN_CONFIDENCE_MODE_MIXED = 1,
} hnn_confidence_mode_t;

typedef enum hnn_confidence_reason {
    HNN_CONFIDENCE_ACCEPTED = 0,
    HNN_CONFIDENCE_REJECT_EMPTY = 1,
    HNN_CONFIDENCE_REJECT_CONTRACT = 2,
    HNN_CONFIDENCE_REJECT_CAPACITY = 3,
    HNN_CONFIDENCE_REJECT_NONFINITE = 4,
    HNN_CONFIDENCE_REJECT_SCORE = 5,
    HNN_CONFIDENCE_REJECT_MARGIN = 6,
    HNN_CONFIDENCE_REJECT_ILLEGAL = 7,
    HNN_CONFIDENCE_REJECT_UNSAFE = 8,
} hnn_confidence_reason_t;

typedef struct hnn_confidence_thresholds {
    float min_score;
    float min_margin;
    float max_capacity_load;
} hnn_confidence_thresholds_t;

typedef struct hnn_confidence_decision {
    bool accepted;
    hnn_confidence_reason_t reason;
    hnn_confidence_thresholds_t thresholds;
} hnn_confidence_decision_t;

hnn_confidence_thresholds_t hnn_confidence_thresholds(
    hnn_backend_kind_t backend);
hnn_confidence_decision_t hnn_confidence_evaluate(
    hnn_backend_kind_t backend,
    const hnn_memory_contract_t *contract,
    float best_score,
    float margin,
    bool legal,
    bool safe);
hnn_confidence_mode_t hnn_confidence_mode_from_string(const char *value);
const char *hnn_confidence_mode_name(hnn_confidence_mode_t mode);
const char *hnn_confidence_reason_name(hnn_confidence_reason_t reason);
int hnn_confidence_select_action(hnn_confidence_mode_t mode,
                                 const hnn_confidence_decision_t *decision,
                                 int hnn_action,
                                 int teacher_action);

#endif /* HOLOGRAPHIC_NN_CONFIDENCE_H */
