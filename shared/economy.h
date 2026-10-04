#ifndef ECONOMY_H
#define ECONOMY_H

#include "types.h"
#include "commodity.h"
#include "ship.h"

/* Canonical fixed-recipe view for station producer modules.  Furnace recipes
 * remain instance-selected and intentionally do not resolve here. */
typedef struct {
    recipe_id_t recipe_id;
    size_t input_count;
    commodity_t inputs[RECIPE_INPUT_MAX];
    float input_units_per_batch[RECIPE_INPUT_MAX];
    commodity_t output;
    float output_units_per_batch;
} producer_recipe_t;

bool producer_recipe_for_module(module_type_t module,
                                producer_recipe_t *out_recipe);

void step_station_production(station_t* stations, int count, float dt);

float station_pod_shell_quote(const station_t *station, const cargo_pod_t *pod,
                              bool station_sells);
float station_market_pod_sell_quote(const station_t *station, const cargo_pod_t *pod);

float station_repair_cost(const ship_t* ship, const station_t* station);
float upgrade_station_credit_cost(const station_t* station,
                                  const ship_t* ship,
                                  ship_upgrade_t upgrade,
                                  int station_units);
bool starter_refit_work_order_init(contract_t *out, int quantity,
                                   float unit_price);
bool starter_refit_work_order_matches(const contract_t *contract);
/* Inactive starter-order markers are durable one-shot tombstones, not
 * reusable contract slots. */
bool contract_slot_available_for_post(const contract_t *contract);
bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, float balance);

#endif
