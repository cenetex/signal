#ifndef ECONOMY_H
#define ECONOMY_H

#include "types.h"
#include "commodity.h"
#include "ship.h"

void step_refinery_production(station_t* stations, int count, float dt);
void step_station_production(station_t* stations, int count, float dt);

float station_cargo_sale_value(const ship_t* ship, const station_t* station);
float station_repair_cost(const ship_t* ship, const station_t* station);
bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, uint32_t service, int credit_cost);

#endif
