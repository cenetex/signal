/*
 * sim_npc.h -- NPC ship subsystem declarations.
 * Extracted from game_sim.c to reduce file size.
 */
#ifndef SIM_NPC_H
#define SIM_NPC_H

#include "game_sim.h"

void step_npc_ships(world_t *w, float dt);
void generate_npc_distress_contracts(world_t *w);
int  spawn_npc(world_t *w, int station_idx, npc_role_t role);
const hull_def_t *npc_hull_def(const npc_ship_t *npc);

#endif /* SIM_NPC_H */
