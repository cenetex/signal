#include <math.h>
#include <stddef.h>
#include "economy.h"
#include "manifest.h"

/* Check if station can smelt a specific ore type */
static bool can_smelt_ore(const station_t* station, commodity_t ore) {
    switch (ore) {
        case COMMODITY_FERRITE_ORE: return station_has_module(station, MODULE_FURNACE);
        case COMMODITY_CUPRITE_ORE: return station_has_module(station, MODULE_FURNACE_CU);
        case COMMODITY_CRYSTAL_ORE: return station_has_module(station, MODULE_FURNACE_CR);
        default: return false;
    }
}

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

void step_refinery_production(station_t* stations, int count, float dt) {
    for (int s = 0; s < count; s++) {
        station_t* station = &stations[s];
        /* Need at least one furnace type */
        if (!station_has_module(station, MODULE_FURNACE)
            && !station_has_module(station, MODULE_FURNACE_CU)
            && !station_has_module(station, MODULE_FURNACE_CR)) continue;

        int active = 0;
        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            if (station->inventory[i] > FLOAT_EPSILON && can_smelt_ore(station, (commodity_t)i)) active++;
        }
        if (active == 0) continue;
        if (active > REFINERY_MAX_FURNACES) active = REFINERY_MAX_FURNACES;

        float rate = REFINERY_BASE_SMELT_RATE / (float)active;

        for (int i = COMMODITY_FERRITE_ORE; i < COMMODITY_RAW_ORE_COUNT; i++) {
            commodity_t ore = (commodity_t)i;
            if (!can_smelt_ore(station, ore)) continue;
            if (station->inventory[ore] <= FLOAT_EPSILON) continue;
            float consume = fminf(station->inventory[ore], rate * dt);
            station->inventory[ore] -= consume;
            station->inventory[commodity_refined_form(ore)] += consume;
        }
    }
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
    if (!(station->services & STATION_SERVICE_REPAIR)) return 0.0f;
    float damage = ship_max_hull(ship) - ship->hull;
    if (damage <= 0.0f) return 0.0f;
    return damage * STATION_REPAIR_COST_PER_HULL;
}

bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, uint32_t service, int credit_cost, float balance) {
    if (!station || !(station->services & service)) return false;
    if (ship_upgrade_maxed(ship, upgrade)) return false;
    if (balance + FLOAT_EPSILON < (float)credit_cost) return false;
    if (station->inventory[COMMODITY_FRAME + upgrade_required_product(upgrade)] + FLOAT_EPSILON < upgrade_product_cost(ship, upgrade)) return false;
    return true;
}
