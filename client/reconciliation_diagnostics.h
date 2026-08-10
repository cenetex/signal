#ifndef SIGNAL_RECONCILIATION_DIAGNOSTICS_H
#define SIGNAL_RECONCILIATION_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

#define NET_RECONCILE_ROOT_SIZE 32u
#define NET_RECONCILE_JSON_SIZE 1536u

typedef enum {
    NET_RECONCILE_EXACT = 0,
    NET_RECONCILE_BOOTSTRAP,
    NET_RECONCILE_INPUT_FRONTIER,
    NET_RECONCILE_SEMANTIC_DISCONTINUITY,
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

enum {
    NET_RECONCILE_CAUSE_NONE = 0u,
    NET_RECONCILE_CAUSE_TICK_FRONTIER = 1u << 0,
    NET_RECONCILE_CAUSE_INPUT_SEQUENCE = 1u << 1,
    NET_RECONCILE_CAUSE_INPUT_INTENT = 1u << 2,
    NET_RECONCILE_CAUSE_REPLAY_GAP = 1u << 3,
    NET_RECONCILE_CAUSE_UNACKED_INPUT = 1u << 4,
    NET_RECONCILE_CAUSE_LAUNCH = 1u << 5,
    NET_RECONCILE_CAUSE_DOCK = 1u << 6,
    NET_RECONCILE_CAUSE_TOW_ATTACH = 1u << 7,
    NET_RECONCILE_CAUSE_TOW_RELEASE = 1u << 8,
    NET_RECONCILE_CAUSE_DEATH_RESPAWN = 1u << 9,
    NET_RECONCILE_CAUSE_PREDICTED_ACTION = 1u << 10,
    NET_RECONCILE_CAUSE_REBASE = 1u << 11,
};

/*
 * A causal classification is valid only inside this closed tick interval.
 * Windows are deliberately copied into each sample rather than retained as
 * an unbounded "tainted" bit. A scheduling mismatch at tick N therefore
 * cannot hide an unrelated numeric divergence at tick N+k.
 */
typedef struct {
    bool valid;
    uint32_t first_tick;
    uint32_t last_tick;
    uint32_t cause_mask;
} net_reconcile_tick_window_t;

typedef struct {
    uint32_t pos_x;
    uint32_t pos_y;
    uint32_t vel_x;
    uint32_t vel_y;
    uint32_t angle;
} net_reconcile_pose_bits_t;

typedef struct {
    bool bootstrap;
    bool frontier_known;
    uint8_t entity_id;
    uint32_t server_tick;
    uint32_t prediction_tick;
    uint16_t predicted_input_seq;
    uint16_t authoritative_input_seq;
    net_reconcile_tick_window_t input_frontier;
    net_reconcile_tick_window_t semantic;
    net_reconcile_tick_window_t transport_recovery;
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

    bool authoritative_state_known;
    bool authoritative_docked;
    uint8_t authoritative_towed_count;
    net_reconcile_tick_window_t pending_semantic;

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
    uint32_t first_input_cause_mask;
    uint32_t first_semantic_cause_mask;
    uint32_t first_transport_cause_mask;
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

net_reconcile_tick_window_t net_reconcile_tick_window(
    uint32_t first_tick, uint32_t last_tick, uint32_t cause_mask);
bool net_reconcile_tick_window_contains(
    const net_reconcile_tick_window_t *window, uint32_t tick);

net_reconcile_class_t net_reconcile_classify(
    const net_reconcile_sample_t *sample);
void net_reconcile_diagnostics_reset(net_reconcile_diagnostics_t *diagnostics);
net_reconcile_class_t net_reconcile_diagnostics_observe(
    net_reconcile_diagnostics_t *diagnostics,
    const net_reconcile_sample_t *sample);
void net_reconcile_note_semantic_window(
    net_reconcile_diagnostics_t *diagnostics,
    uint32_t first_tick,
    uint32_t last_tick,
    uint32_t cause_mask);
net_reconcile_tick_window_t net_reconcile_take_semantic_window(
    net_reconcile_diagnostics_t *diagnostics, uint32_t sample_tick);

const char *net_reconcile_class_name(net_reconcile_class_t classification);
const char *net_reconcile_domain_name(net_reconcile_domain_t domain);
int net_reconcile_first_drift_json(
    const net_reconcile_diagnostics_t *diagnostics,
    const char *root_schema,
    char *out,
    size_t out_size);

#endif /* SIGNAL_RECONCILIATION_DIAGNOSTICS_H */
