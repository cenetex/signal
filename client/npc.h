#ifndef NPC_H
#define NPC_H

#include "types.h"
#include "ship.h"
#include "asteroid.h"

float npc_total_cargo(const npc_ship_t* npc);
bool npc_target_valid(const npc_ship_t* npc, const asteroid_t* asteroids, int count);
int npc_find_mineable_asteroid(const npc_ship_t* npc, const asteroid_t* asteroids, int count);
void npc_steer_toward(npc_ship_t* npc, vec2 target, float accel, float turn_speed, float dt);
void npc_apply_physics(npc_ship_t* npc, float drag, float dt);

#endif
