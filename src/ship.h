#ifndef SHIP_H
#define SHIP_H

#include "types.h"

const hull_def_t* ship_hull_def(const ship_t* ship);
const hull_def_t* npc_hull_def(const npc_ship_t* npc);

float ship_max_hull(const ship_t* ship);
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
