#ifndef SIGNAL_ZERO_LATENCY_GATE_H
#define SIGNAL_ZERO_LATENCY_GATE_H

#include <stdint.h>

enum {
    SIGNAL_ZERO_LATENCY_FLIGHT = 1u << 0,
    SIGNAL_ZERO_LATENCY_BOOST = 1u << 1,
    SIGNAL_ZERO_LATENCY_MINING = 1u << 2,
    SIGNAL_ZERO_LATENCY_TOWING = 1u << 3,
    SIGNAL_ZERO_LATENCY_RELEASE = 1u << 4,
    SIGNAL_ZERO_LATENCY_DOCK = 1u << 5,
    SIGNAL_ZERO_LATENCY_LAUNCH = 1u << 6,
    SIGNAL_ZERO_LATENCY_DEATH_RESPAWN = 1u << 7,
    SIGNAL_ZERO_LATENCY_ASTEROID_MOTION = 1u << 8,
    SIGNAL_ZERO_LATENCY_NPC_MOTION = 1u << 9,
};

#define SIGNAL_ZERO_LATENCY_REQUIRED_MASK \
    (SIGNAL_ZERO_LATENCY_FLIGHT | \
     SIGNAL_ZERO_LATENCY_BOOST | \
     SIGNAL_ZERO_LATENCY_MINING | \
     SIGNAL_ZERO_LATENCY_TOWING | \
     SIGNAL_ZERO_LATENCY_RELEASE | \
     SIGNAL_ZERO_LATENCY_DOCK | \
     SIGNAL_ZERO_LATENCY_LAUNCH | \
     SIGNAL_ZERO_LATENCY_DEATH_RESPAWN | \
     SIGNAL_ZERO_LATENCY_ASTEROID_MOTION | \
     SIGNAL_ZERO_LATENCY_NPC_MOTION)

/*
 * Deterministic browser/native diagnostic entry points. The normal run uses
 * fixed SIM_DT steps and tick-addressed loopback inputs; no wall clock or
 * keyboard timing participates. The perturbation run flips one pos.x bit
 * after client prediction and before the matching authoritative snapshot.
 */
int signal_zero_latency_gate_run(void);
int signal_zero_latency_gate_perturb(void);
int signal_zero_latency_gate_ready(void);
int signal_zero_latency_gate_status(void);
int signal_zero_latency_gate_scenario_mask(void);
int get_net_reconcile_exact_samples(void);
int get_net_reconcile_bootstrap_samples(void);
int get_net_reconcile_input_frontier_samples(void);
int get_net_reconcile_semantic_samples(void);
int get_net_reconcile_transport_recovery_samples(void);
int get_net_reconcile_numeric_drift_samples(void);
int get_net_reconcile_asteroid_motion_samples(void);
int get_net_reconcile_npc_motion_samples(void);
int get_net_reconcile_death_respawn_events(void);
const char *get_net_reconcile_first_drift_json(void);
const char *signal_zero_latency_gate_report_json(void);

#endif /* SIGNAL_ZERO_LATENCY_GATE_H */
