#include <math.h>
#include <stddef.h>
#include "economy.h"
#include "manifest.h"

typedef struct {
    commodity_t primary_input;
    float primary_units_per_output;
    commodity_t secondary_input;
    float secondary_units_per_output;
    commodity_t output;
} producer_recipe_t;

static bool producer_recipe_for_module(module_type_t mt, producer_recipe_t *out_recipe) {
    recipe_id_t recipe_id;
    const recipe_def_t *recipe;
    commodity_t primary;

    if (!out_recipe) return false;
    memset(out_recipe, 0, sizeof(*out_recipe));
    out_recipe->secondary_input = COMMODITY_COUNT;

    switch (mt) {
        case MODULE_FRAME_PRESS: recipe_id = RECIPE_FRAME_BASIC; break;
        case MODULE_LASER_FAB:   recipe_id = RECIPE_LASER_BASIC; break;
        case MODULE_TRACTOR_FAB: recipe_id = RECIPE_TRACTOR_COIL; break;
        default: return false;
    }

    recipe = recipe_get(recipe_id);
    if (!recipe) return false;
    primary = module_schema_input(mt);
    out_recipe->primary_input = primary;
    out_recipe->output = recipe->output_commodity;

    for (size_t i = 0; i < recipe->input_count; i++) {
        commodity_t input = recipe->input_commodities[i];
        if (input == primary) {
            out_recipe->primary_units_per_output += 1.0f;
            continue;
        }
        if (out_recipe->secondary_input == COMMODITY_COUNT ||
            out_recipe->secondary_input == input) {
            out_recipe->secondary_input = input;
            out_recipe->secondary_units_per_output += 1.0f;
            continue;
        }
        return false;
    }

    return out_recipe->primary_units_per_output > 0.0f &&
           out_recipe->output == module_schema_output(mt);
}

void step_station_production(station_t* stations, int count, float dt) {
    for (int s = 0; s < count; s++) {
        station_t* station = &stations[s];

        for (int m = 0; m < station->module_count; m++) {
            module_type_t mt = station->modules[m].type;
            const module_schema_t *schema;
            producer_recipe_t recipe;
            float room, produce, rate;

            if (station->modules[m].scaffold) continue;
            if (!producer_recipe_for_module(mt, &recipe)) continue;

            schema = module_schema(mt);
            room = MAX_PRODUCT_STOCK - station->inventory[recipe.output];
            if (room <= FLOAT_EPSILON) continue;

            rate = schema->rate > 0.0f ? schema->rate : STATION_PRODUCTION_RATE;
            produce = fminf(rate * dt, room);
            produce = fminf(produce,
                            station->inventory[recipe.primary_input] /
                            recipe.primary_units_per_output);
            if (recipe.secondary_input < COMMODITY_COUNT) {
                produce = fminf(produce,
                                station->inventory[recipe.secondary_input] /
                                recipe.secondary_units_per_output);
            }
            if (produce <= FLOAT_EPSILON) continue;

            station->inventory[recipe.primary_input] -=
                produce * recipe.primary_units_per_output;
            if (recipe.secondary_input < COMMODITY_COUNT) {
                station->inventory[recipe.secondary_input] -=
                    produce * recipe.secondary_units_per_output;
            }
            station->inventory[recipe.output] += produce;
        }
    }
}

float station_repair_cost(const ship_t* ship, const station_t* station) {
    if (!station) return 0.0f;
    if (!station_has_module(station, MODULE_DOCK)) return 0.0f;
    float damage = ship_max_hull(ship) - ship->hull;
    if (damage <= 0.0f) return 0.0f;

    /* Quote: assume station-sourced kits (worst case for the player —
     * the actual repair will be cheaper if they brought their own).
     * Labor fee is zero at shipyards, LABOR_FEE_PER_HP elsewhere. */
    float kit_price = station_sell_price(station, COMMODITY_REPAIR_KIT);
    bool is_shipyard = station_has_module(station, MODULE_SHIPYARD);
    float labor = is_shipyard ? 0.0f : LABOR_FEE_PER_HP;
    return damage * (kit_price + labor);
}

bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, uint32_t service, int credit_cost, float balance) {
    if (!station || !(station->services & service)) return false;
    if (ship_upgrade_maxed(ship, upgrade)) return false;
    if (balance + FLOAT_EPSILON < (float)credit_cost) return false;
    if (station->inventory[COMMODITY_FRAME + upgrade_required_product(upgrade)] + FLOAT_EPSILON < upgrade_product_cost(ship, upgrade)) return false;
    return true;
}
