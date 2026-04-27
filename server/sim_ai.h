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

/* Damage an NPC through its paired ship_t (#294 Slice 9 + 10). The
 * reverse mirror at end of the NPC's tick pushes the result into
 * npc->hull so the existing despawn check still fires. Safe to call
 * even if the NPC has no paired ship (falls back to npc->hull).
 * `dmg <= 0` is a no-op. */
void apply_npc_ship_damage(world_t *w, int npc_slot, float dmg);

/* Resolve an NPC slot to its paired ship_t (#294 Slice 8). Returns
 * NULL if the slot is out of range, the NPC isn't active, or no
 * character is currently paired. Const-overload via the same
 * underlying lookup is unnecessary today; tests and external readers
 * can take the non-const pointer. */
ship_t *world_npc_ship_for(world_t *w, int npc_slot);

#endif /* SIM_AI_H */
