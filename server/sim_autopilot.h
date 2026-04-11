/*
 * sim_autopilot.h — Player autopilot state machine for Signal Space Miner.
 * Extracted from game_sim.c (#272 slice).
 */
#ifndef SIM_AUTOPILOT_H
#define SIM_AUTOPILOT_H

#include "game_sim.h"

/* Autopilot state machine cursor (mode 1: mining loop). */
enum {
    AUTOPILOT_STEP_FIND_TARGET = 0,
    AUTOPILOT_STEP_FLY_TO_TARGET,
    AUTOPILOT_STEP_MINE,
    AUTOPILOT_STEP_COLLECT,
    AUTOPILOT_STEP_RETURN_TO_REFINERY,
    AUTOPILOT_STEP_DOCK,
    AUTOPILOT_STEP_SELL,
    AUTOPILOT_STEP_LAUNCH,
};

/* Drive the player's ship via simulated input. The autopilot writes
 * sp->input each tick, and the existing physics/mining/dock systems
 * consume those intents like they would for a human player. */
void step_autopilot(world_t *w, server_player_t *sp, float dt);

#endif /* SIM_AUTOPILOT_H */
