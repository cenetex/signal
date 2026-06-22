#ifndef SIGNAL_CLIENT_BRAIN_H
#define SIGNAL_CLIENT_BRAIN_H

#include <stddef.h>

#include "signal_client_flight.h"
#include "signal_client_strategic.h"
#include "signal_client_tactical.h"

#ifdef __cplusplus
extern "C" {
#endif

enum signal_brain_task {
    SIGNAL_BRAIN_TASK_FLIGHT = 0,
    SIGNAL_BRAIN_TASK_TACTICAL = 1,
    SIGNAL_BRAIN_TASK_STRATEGIC = 2,
    SIGNAL_BRAIN_TASK_COUNT = 3
};

typedef enum signal_brain_task signal_brain_task;

#define SIGNAL_CLIENT_BRAIN_MAX_INPUT_COUNT SIGNAL_CLIENT_STRATEGIC_INPUT_COUNT

typedef struct signal_client_brain_task_contract {
    const char *task_name;
    const char *feature_set;
    const char *checkpoint_hash;
    unsigned feature_encoder_version;
    size_t input_count;
} signal_client_brain_task_contract;

signal_client_brain_task_contract signal_client_brain_contract(enum signal_brain_task task);
size_t signal_client_brain_input_count(enum signal_brain_task task);
const char *signal_client_brain_feature_set(enum signal_brain_task task);
unsigned signal_client_brain_feature_encoder_version(enum signal_brain_task task);
const char *signal_client_brain_checkpoint_hash(enum signal_brain_task task);

float signal_client_brain_score(enum signal_brain_task task,
                                const float *features);
int signal_client_brain_select_best(enum signal_brain_task task,
                                    const float *features,
                                    size_t candidate_count,
                                    float *scores_out);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_CLIENT_BRAIN_H */
