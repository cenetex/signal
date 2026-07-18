#ifndef ECONOMY_H
#define ECONOMY_H

#include "types.h"
#include "commodity.h"
#include "ship.h"

/* Canonical fixed-recipe view for station producer modules.  Furnace recipes
 * remain instance-selected and intentionally do not resolve here. */
typedef struct {
    recipe_id_t recipe_id;
    commodity_t primary_input;
    float primary_units_per_batch;
    commodity_t secondary_input;
    float secondary_units_per_batch;
    commodity_t output;
    float output_units_per_batch;
} producer_recipe_t;

bool producer_recipe_for_module(module_type_t module,
                                producer_recipe_t *out_recipe);

void step_station_production(station_t* stations, int count, float dt);

float station_repair_cost(const ship_t* ship, const station_t* station);
bool upgrade_uses_starter_refit_subsidy(const station_t* station,
                                        const ship_t* ship,
                                        ship_upgrade_t upgrade,
                                        int station_units);
float upgrade_station_credit_cost(const station_t* station,
                                  const ship_t* ship,
                                  ship_upgrade_t upgrade,
                                  int station_units);
bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, float balance);

#endif
