#include "signal_client_brain.h"

#include <math.h>
#include <stddef.h>

static int signal_client_brain_task_is_valid(enum signal_brain_task task)
{
    return task >= SIGNAL_BRAIN_TASK_FLIGHT && task < SIGNAL_BRAIN_TASK_COUNT;
}

signal_client_brain_task_contract signal_client_brain_contract(enum signal_brain_task task)
{
    signal_client_brain_task_contract contract = {0};

    switch (task) {
    case SIGNAL_BRAIN_TASK_FLIGHT:
        contract.task_name = "flight";
        contract.feature_set = signal_client_flight_feature_set;
        contract.checkpoint_hash = signal_client_flight_checkpoint_hash;
        contract.feature_encoder_version = signal_client_flight_feature_encoder_version;
        contract.input_count = SIGNAL_CLIENT_FLIGHT_INPUT_COUNT;
        return contract;
    case SIGNAL_BRAIN_TASK_TACTICAL:
        contract.task_name = "tactical";
        contract.feature_set = signal_client_tactical_feature_set;
        contract.checkpoint_hash = signal_client_tactical_checkpoint_hash;
        contract.feature_encoder_version = signal_client_tactical_feature_encoder_version;
        contract.input_count = SIGNAL_CLIENT_TACTICAL_INPUT_COUNT;
        return contract;
    case SIGNAL_BRAIN_TASK_STRATEGIC:
        contract.task_name = "strategic";
        contract.feature_set = signal_client_strategic_feature_set;
        contract.checkpoint_hash = signal_client_strategic_checkpoint_hash;
        contract.feature_encoder_version = signal_client_strategic_feature_encoder_version;
        contract.input_count = SIGNAL_CLIENT_STRATEGIC_INPUT_COUNT;
        return contract;
    case SIGNAL_BRAIN_TASK_COUNT:
    default:
        return contract;
    }
}

size_t signal_client_brain_input_count(enum signal_brain_task task)
{
    return signal_client_brain_contract(task).input_count;
}

const char *signal_client_brain_feature_set(enum signal_brain_task task)
{
    return signal_client_brain_contract(task).feature_set;
}

unsigned signal_client_brain_feature_encoder_version(enum signal_brain_task task)
{
    return signal_client_brain_contract(task).feature_encoder_version;
}

const char *signal_client_brain_checkpoint_hash(enum signal_brain_task task)
{
    return signal_client_brain_contract(task).checkpoint_hash;
}

float signal_client_brain_score(enum signal_brain_task task,
                                const float *features)
{
    if (!signal_client_brain_task_is_valid(task) || features == NULL) {
        return -INFINITY;
    }

    switch (task) {
    case SIGNAL_BRAIN_TASK_FLIGHT:
        return signal_client_flight_score(features);
    case SIGNAL_BRAIN_TASK_TACTICAL:
        return signal_client_tactical_score(features);
    case SIGNAL_BRAIN_TASK_STRATEGIC:
        return signal_client_strategic_score(features);
    case SIGNAL_BRAIN_TASK_COUNT:
    default:
        return -INFINITY;
    }
}

int signal_client_brain_select_best(enum signal_brain_task task,
                                    const float *features,
                                    size_t candidate_count,
                                    float *scores_out)
{
    if (!signal_client_brain_task_is_valid(task) ||
        features == NULL ||
        candidate_count == 0u) {
        return -1;
    }

    switch (task) {
    case SIGNAL_BRAIN_TASK_FLIGHT:
        return signal_client_flight_select_best(features, candidate_count, scores_out);
    case SIGNAL_BRAIN_TASK_TACTICAL:
        return signal_client_tactical_select_best(features, candidate_count, scores_out);
    case SIGNAL_BRAIN_TASK_STRATEGIC:
        return signal_client_strategic_select_best(features, candidate_count, scores_out);
    case SIGNAL_BRAIN_TASK_COUNT:
    default:
        return -1;
    }
}
