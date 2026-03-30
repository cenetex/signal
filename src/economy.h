#ifndef ECONOMY_H
#define ECONOMY_H

#include "types.h"
#include "commodity.h"
#include "ship.h"

static const float REFINERY_HOPPER_CAPACITY = 100.0f;
static const float REFINERY_BASE_SMELT_RATE = 0.5f;
static const int REFINERY_MAX_FURNACES = 3;
static const float STATION_PRODUCTION_RATE = 0.3f;
static const float STATION_REPAIR_COST_PER_HULL = 2.0f;

void step_refinery_production(station_t* stations, int count, float dt);
void step_station_production(station_t* stations, int count, float dt);

float station_cargo_sale_value(const ship_t* ship, const station_t* station);
float station_repair_cost(const ship_t* ship, const station_t* station);
bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, uint32_t service, int credit_cost);

#endif
