/*
 * signal_brain.h -- Server-side neural and holographic flight controllers.
 *
 * The mining objective loop can remain deterministic while this module
 * replaces per-tick WASD flight choices with a trained signal-flight-live-v2
 * checkpoint scorer or a holographic VSA-based associative memory.
 */
#ifndef SIGNAL_BRAIN_H
#define SIGNAL_BRAIN_H

#include "game_sim.h"
#include "holographic_nn.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct signal_brain_flight_action {
    const char *name;
    int turn;
    int thrust;
} signal_brain_flight_action_t;

typedef struct signal_brain_flight_decision {
    int selected_index;
    int raw_index;
    int candidate_count;
    float selected_score;
    float raw_score;
    float signal_quality;
    float route_risk;
    float hull_ratio;
    uint16_t allowed_mask;
    bool forward_blocked;
    bool hard_override;
} signal_brain_flight_decision_t;

bool signal_brain_load_checkpoint(const char *path, char *err, size_t err_size);
bool signal_brain_loaded(void);
uint64_t signal_brain_inference_count(void);
void signal_brain_drive(world_t *w, server_player_t *sp, float dt);
bool signal_brain_drive_with_decision(world_t *w,
                                      server_player_t *sp,
                                      float dt,
                                      signal_brain_flight_decision_t *decision);

int signal_brain_flight_action_count(void);
const signal_brain_flight_action_t *signal_brain_flight_action(int index);
int signal_brain_flight_action_index_from_intent(const input_intent_t *intent);
bool signal_brain_build_flight_candidate_features(
    const world_t *w,
    const server_player_t *sp,
    float features_out[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT * SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT],
    uint8_t allowed_out[SIGNAL_BRAIN_FLIGHT_ACTION_COUNT],
    int *forward_blocked_out);

/* Score NPC flight toward an explicit target. Returns true when a loaded
 * neural checkpoint produced turn/thrust intent for this tick. */
bool signal_brain_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target);

/* NPC holographic brain: uses VSA associative memory to select flight
 * actions. Initializes per-NPC memory on first call and stores
 * state->action associations into a bundled holographic trace.
 * Requires brain_mode == SERVER_BRAIN_MODE_HOLOGRAPHIC on the NPC. */
void signal_brain_drive_npc(world_t *w, npc_ship_t *npc, float dt);

/* Initialize the global holographic action table (called once at
 * server start). */
void signal_brain_holographic_init(void);

extern bool g_neural_singleplayer;

#endif /* SIGNAL_BRAIN_H */
