#ifndef SHIP_H
#define SHIP_H

#include "types.h"

/* Hull-class lookups. The class accessor is the source of truth; the
 * ship/npc-typed wrappers are kept so existing callsites compile and
 * intent stays readable at the use site. Slice 1 of ship/NPC unify. */
const hull_def_t* hull_def_for_class(hull_class_t hc);
float             hull_max_for_class(hull_class_t hc);

const hull_def_t* ship_hull_def(const ship_t* ship);
const hull_def_t* npc_hull_def(const npc_ship_t* npc);

vec2 ship_forward(float angle);
vec2 ship_muzzle(vec2 pos, float angle, const ship_t* ship);

float ship_max_hull(const ship_t* ship);
float npc_max_hull(const npc_ship_t* npc);
float ship_cargo_capacity(const ship_t* ship);
float ship_mining_rate(const ship_t* ship);
float ship_tractor_range(const ship_t* ship);
float ship_collect_radius(const ship_t* ship);

int ship_upgrade_level(const ship_t* ship, ship_upgrade_t upgrade);
bool ship_upgrade_maxed(const ship_t* ship, ship_upgrade_t upgrade);
int ship_upgrade_cost(const ship_t* ship, ship_upgrade_t upgrade);

product_t upgrade_required_product(ship_upgrade_t upgrade);
float upgrade_product_cost(const ship_t* ship, ship_upgrade_t upgrade);
const char* product_name(product_t product);

#endif
