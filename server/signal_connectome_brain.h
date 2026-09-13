/*
 * signal_connectome_brain.h -- FlyWire connectome as signal's fourth
 * server brain mode (SERVER_BRAIN_MODE_CONNECTOME).
 *
 * A deterministic integer leaky-integrate-and-fire kernel (server/connectome/)
 * runs a Drosophila connectome subgraph per NPC. The wiring is the program:
 * there is no training, no float in the step path, and the same inputs give
 * bit-identical output on every machine, which is what the lockstep-replay
 * determinism story requires.
 *
 * Biology mapping, because the point is to be honest about what this is:
 *
 *   HUNGER  -- empty-handed foraging flies get a rising tonic arousal
 *              (octopamine is foraging drive in a real fly).
 *   LUST    -- a fly towing ore is courtship-aroused: the cargo IS the
 *              fly's beloved; tonic gain rises and the awake threshold
 *              drops until delivery. Dopamine is not subtle either.
 *   FEAR    -- nearby closing rocks (signal's only weapon) drive the
 *              escape/freeze descending channels (DNp07/DNp10/DNp09),
 *              which can override thrust with a freeze.
 *   PAIN    -- hull damage spikes arousal for ~a second and keeps the
 *              fly awake even at low descending drive.
 *
 * BRAIN-TIME AS AN ECONOMY (flyswarm): one shared read-only connectome,
 * N agent states. A per-tick budget of kernel steps is divided by stake;
 * a docked fly sleeps at stake 0 and its would-be share is rented out to
 * working flies through the stake pool. Flies that clear a stake
 * threshold win one of a few scarce whole-brain "deliberation" slots
 * (the 138k-neuron circuit, ~25x cost) -- the hierarchy is earned, not
 * assigned.
 *
 * Enabling: set SIGNAL_CONNECTOME_FAST=<path to nav .cnx blob>. Optional:
 * SIGNAL_CONNECTOME_DEEP=<full brain .cnx> for deliberation slots, plus the
 * tuning knobs documented in signal_connectome_brain.c. With no env set the
 * mode is disabled and NPCs fall back to the neural/heuristic brains.
 */
#ifndef SIGNAL_CONNECTOME_BRAIN_H
#define SIGNAL_CONNECTOME_BRAIN_H

#include "game_sim.h"

#include <stdbool.h>
#include <stdint.h>

/* Number of strategic postures the hybrid brain samples between. Must
 * match the posture enum in signal_connectome_brain.c. */
#define SIGNAL_CONNECTOME_STRATEGY_COUNT 5

/* Idempotent. Reads env on first call; returns true when the connectome
 * brain is loaded and ready to fly NPCs. Prints one status block. */
bool signal_connectome_init(void);

/* True when the connectome brain is loaded (does not trigger init). */
bool signal_connectome_enabled(void);

/* Combined brain: when requested, connectome flies also go through the
 * strategic worker planner in server/sim_ai.c, so the job is chosen by the
 * learned (or teacher-fallback) planner while the connectome flies it.
 * Opt-in via SIGNAL_CONNECTOME_STRATEGY=1, and inert unless the adapter
 * actually loaded. */
bool signal_connectome_strategy_requested(void);
bool signal_connectome_strategy_enabled(void);

void signal_connectome_shutdown(void);

/* Called from step_npc_ships after the per-NPC loop: updates drives,
 * sets stakes, injects sensory drive, and advances the swarm by the
 * tick's brain budget. Skipping it when disabled is free. */
void signal_connectome_tick(world_t *w);

/* Called from npc_steer_with_path for CONNECTOME NPCs, after
 * flight_steer_to produced the reflex controller's answer.
 *
 *   turn_in  -- the reflex turn (post-avoidance), used both as the
 *               sensory drive recorded for next tick's injection and,
 *               blended by forward clearance, as an escape reflex that
 *               overrides the brain near rock faces.
 *   turn_out / thrust_out -- the connectome's final flight decision.
 *
 * Returns true when it drove the axes (always, when enabled). */
bool signal_connectome_flight_cmd(const world_t *w,
                                   int npc_idx,
                                   npc_ship_t *npc,
                                   float turn_in,
                                   float *turn_out,
                                   float *thrust_out);

/* FNV-style checksum over all agent state -- the determinism gate. */
uint64_t signal_connectome_checksum(void);

typedef struct {
    uint64_t ticks;        /* connectome ticks run */
    uint64_t steps;        /* kernel steps taken (fast + deep) */
    uint64_t units_spent;  /* budget units consumed */
    uint64_t rented_units; /* pool units granted while flies slept */
    uint32_t strategy_counts[SIGNAL_CONNECTOME_STRATEGY_COUNT];
    uint32_t strategy_changes; /* posture re-samples this session */
    uint32_t strategy_model_scores; /* re-samples biased by the worker model */
    uint32_t promotions;   /* flies promoted to the deep circuit */
    uint32_t demotions;
    uint32_t active_flies;   /* connectome NPCs awake this tick */
    uint32_t sleeping_flies; /* docked, stake 0, compute rented out */
    uint32_t deep_flies;     /* currently deliberating on full brain */
    int      has_deep;
    uint32_t fast_neurons;
    uint32_t deep_neurons;
} signal_connectome_stats_t;

bool signal_connectome_stats(signal_connectome_stats_t *out);

/* Deterministic weighted pick: weights[i] >= 0. Returns the chosen index,
 * or -1 when count <= 0 or every weight is zero. Advances *rng with one
 * xorshift64 step. Integer-only, so the strategic layer stays inside the
 * lockstep replay determinism gate. */
int signal_connectome_weighted_pick(const uint32_t *weights, int count,
                                    uint64_t *rng);

/* Station-level reward-weighted posture learning (a bandit). values[arm]
 * is reinforced by the outcome its posture produced and every arm decays
 * each window, so a station learns what pays off without losing the
 * ability to switch. Integer-only and bounded. */
void signal_connectome_bandit_reward(uint32_t *values, int count, int arm,
                                     uint32_t reward);
void signal_connectome_bandit_decay(uint32_t *values, int count);

#endif /* SIGNAL_CONNECTOME_BRAIN_H */
