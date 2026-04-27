/*
 * sim_ai.h -- NPC ship subsystem declarations.
 * Extracted from game_sim.c to reduce file size.
 */
#ifndef SIM_AI_H
#define SIM_AI_H

#include "game_sim.h"

void step_npc_ships(world_t *w, float dt);
void generate_npc_distress_contracts(world_t *w);
int  spawn_npc(world_t *w, int station_idx, npc_role_t role);
const hull_def_t *npc_hull_def(const npc_ship_t *npc);
/* Repopulate world.characters[] from world.npc_ships[]. Called by
 * world_load after npc_ships have been read so the paired controller
 * pool stays in sync with NPC lifecycle (#294 Slice 6). */
void rebuild_characters_from_npcs(world_t *w);

#endif /* SIM_AI_H */
