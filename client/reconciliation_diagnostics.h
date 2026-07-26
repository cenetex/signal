#ifndef SIGNAL_RECONCILIATION_DIAGNOSTICS_H
#define SIGNAL_RECONCILIATION_DIAGNOSTICS_H

#include <stdbool.h>
#include <stdint.h>

#include "types.h"

#define NET_RECONCILE_ROOT_SIZE 32u

typedef enum {
    NET_RECONCILE_EXACT = 0,
    NET_RECONCILE_BOOTSTRAP,
    NET_RECONCILE_INPUT_FRONTIER,
    NET_RECONCILE_SEMANTIC,
    NET_RECONCILE_TRANSPORT_RECOVERY,
    NET_RECONCILE_NUMERIC_DRIFT,
    NET_RECONCILE_CLASS_COUNT,
} net_reconcile_class_t;

typedef enum {
    NET_RECONCILE_DOMAIN_NONE = 0,
    NET_RECONCILE_DOMAIN_PLAYER_POS_X,
    NET_RECONCILE_DOMAIN_PLAYER_POS_Y,
    NET_RECONCILE_DOMAIN_PLAYER_VEL_X,
    NET_RECONCILE_DOMAIN_PLAYER_VEL_Y,
    NET_RECONCILE_DOMAIN_PLAYER_ANGLE,
} net_reconcile_domain_t;

typedef struct {
    uint32_t pos_x;
    uint32_t pos_y;
    uint32_t vel_x;
    uint32_t vel_y;
    uint32_t angle;
} net_reconcile_pose_bits_t;

typedef struct {
    bool bootstrap;
    bool input_frontier;
    bool semantic_discontinuity;
    bool transport_recovery;
    uint8_t entity_id;
    uint32_t server_tick;
    uint32_t prediction_tick;
    uint16_t predicted_input_seq;
    uint16_t authoritative_input_seq;
    net_reconcile_pose_bits_t predicted;
    net_reconcile_pose_bits_t authoritative;
    const uint8_t *authoritative_root;
} net_reconcile_sample_t;

typedef struct {
    uint32_t total_samples;
    uint32_t total_corrections;
    uint32_t class_count[NET_RECONCILE_CLASS_COUNT];
    uint32_t asteroid_motion_samples;
    uint32_t npc_motion_samples;
    uint32_t death_respawn_events;

    bool first_numeric_drift_valid;
    uint8_t first_entity_id;
    net_reconcile_domain_t first_domain;
    uint32_t first_server_tick;
    uint32_t first_prediction_tick;
    uint16_t first_predicted_input_seq;
    uint16_t first_authoritative_input_seq;
    net_reconcile_pose_bits_t first_predicted_pose;
    net_reconcile_pose_bits_t first_authoritative_pose;
    uint32_t first_predicted_bits;
    uint32_t first_authoritative_bits;
    bool first_authoritative_root_valid;
    uint8_t first_authoritative_root[NET_RECONCILE_ROOT_SIZE];
} net_reconcile_diagnostics_t;

net_reconcile_pose_bits_t net_reconcile_pose_bits(float pos_x,
                                                  float pos_y,
                                                  float vel_x,
                                                  float vel_y,
                                                  float angle);
bool net_reconcile_pose_equal(const net_reconcile_pose_bits_t *a,
                              const net_reconcile_pose_bits_t *b);
bool net_reconcile_movement_intent_equal(const input_intent_t *a,
                                         const input_intent_t *b);
net_reconcile_class_t net_reconcile_classify(
    const net_reconcile_sample_t *sample);
void net_reconcile_diagnostics_reset(net_reconcile_diagnostics_t *diagnostics);
net_reconcile_class_t net_reconcile_diagnostics_observe(
    net_reconcile_diagnostics_t *diagnostics,
    const net_reconcile_sample_t *sample);
const char *net_reconcile_class_name(net_reconcile_class_t classification);
const char *net_reconcile_domain_name(net_reconcile_domain_t domain);

#endif /* SIGNAL_RECONCILIATION_DIAGNOSTICS_H */
