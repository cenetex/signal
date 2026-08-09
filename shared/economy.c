#include <math.h>
#include <stddef.h>
#include <string.h>
#include "economy.h"
#include "manifest.h"

bool producer_recipe_for_module(module_type_t mt,
                                producer_recipe_t *out_recipe) {
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
    out_recipe->recipe_id = recipe_id;
    primary = module_schema_input(mt);
    out_recipe->primary_input = primary;
    out_recipe->output = recipe->output_commodity;
    out_recipe->output_units_per_batch =
        recipe->output_count > 0 ? (float)recipe->output_count : 1.0f;

    for (size_t i = 0; i < recipe->input_count; i++) {
        commodity_t input = recipe->input_commodities[i];
        if (input == primary) {
            out_recipe->primary_units_per_batch += 1.0f;
            continue;
        }
        if (out_recipe->secondary_input == COMMODITY_COUNT ||
            out_recipe->secondary_input == input) {
            out_recipe->secondary_input = input;
            out_recipe->secondary_units_per_batch += 1.0f;
            continue;
        }
        return false;
    }

    return out_recipe->primary_units_per_batch > 0.0f &&
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
            float primary_avail, secondary_avail;
            float primary_use, secondary_use, output_made;

            if (station->modules[m].scaffold) continue;
            if (!producer_recipe_for_module(mt, &recipe)) continue;

            schema = module_schema(mt);
            room = (MAX_PRODUCT_STOCK - station_inventory_amount(station, recipe.output)) /
                   recipe.output_units_per_batch;
            if (room <= FLOAT_EPSILON) continue;

            rate = schema->rate > 0.0f ? schema->rate : STATION_PRODUCTION_RATE;
            produce = fminf(rate * dt, room);

            primary_avail = station_stored_inventory_amount(
                                station, recipe.primary_input) /
                            recipe.primary_units_per_batch;
            produce = fminf(produce, primary_avail);
            if (recipe.secondary_input < COMMODITY_COUNT) {
                secondary_avail =
                    station_stored_inventory_amount(
                        station, recipe.secondary_input) /
                    recipe.secondary_units_per_batch;
                produce = fminf(produce, secondary_avail);
            }
            if (produce <= FLOAT_EPSILON) continue;

            primary_use = produce * recipe.primary_units_per_batch;
            station_finished_consume(station, recipe.primary_input, primary_use);
            if (recipe.secondary_input < COMMODITY_COUNT) {
                secondary_use = produce * recipe.secondary_units_per_batch;
                station_finished_consume(station, recipe.secondary_input,
                                        secondary_use);
            }
            output_made = produce * recipe.output_units_per_batch;
            station_finished_accumulate(station, recipe.output, output_made,
                                        NULL);
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

bool can_afford_upgrade(const station_t* station, const ship_t* ship, ship_upgrade_t upgrade, float balance) {
    /* Any dock can install upgrades; the FAB-module gate is gone.
     * Modules (cargo or dock inventory) are the limiter, mirroring
     * the repair-kit "any dock" model from #373. */
    if (!station) return false;
    if (ship_upgrade_maxed(ship, upgrade)) return false;
    /* Manifest-backed cargo first, dock fallback at retail. Mirror the
     * server logic so the UI's "can afford?" matches what
     * try_apply_ship_upgrade will actually accept. */
    commodity_t comm = (commodity_t)(COMMODITY_FRAME + upgrade_required_product(upgrade));
    int units_needed = (int)ceilf(upgrade_product_cost(ship, upgrade));
    int in_cargo  = ship_finished_count(ship, comm);
    int at_station = station_finished_count(station, comm);
    if (in_cargo + at_station < units_needed) return false;
    int from_station = units_needed - (units_needed < in_cargo ? units_needed : in_cargo);
    float credit_cost = upgrade_station_credit_cost(station, ship, upgrade,
                                                    from_station);
    if (balance + FLOAT_EPSILON < credit_cost) return false;
    return true;
}

float upgrade_station_credit_cost(const station_t* station,
                                  const ship_t* ship,
                                  ship_upgrade_t upgrade,
                                  int station_units) {
    if (!station || !ship || station_units <= 0) return 0.0f;
    commodity_t comm =
        (commodity_t)(COMMODITY_FRAME + upgrade_required_product(upgrade));
    return (float)station_units * station_sell_price(station, comm);
}

/*
 * target_pub is otherwise unused for quota TRACTOR work
 * (target_index == -1), so a domain-separated marker gives the finite
 * onboarding order a durable identity without adding a save/wire field.
 */
static const uint8_t STARTER_REFIT_WORK_ORDER_MARKER[32] =
    "signal/starter-refit-work/v1";

bool starter_refit_work_order_init(contract_t *out, int quantity,
                                   float unit_price) {
    if (!out || quantity <= 0 ||
        !isfinite(unit_price) || unit_price <= 0.0f)
        return false;
    *out = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .proof_flags = (uint8_t)(
            CONTRACT_PROOF_REQUIRE_PROOF |
            CONTRACT_PROOF_REQUIRE_RECIPE),
        .required_recipe_id = RECIPE_SMELT,
        .quantity_needed = (float)quantity,
        .base_price = unit_price,
        .target_index = -1,
        .claimed_by = -1,
    };
    memcpy(out->target_pub, STARTER_REFIT_WORK_ORDER_MARKER,
           sizeof(out->target_pub));
    return true;
}

bool starter_refit_work_order_matches(const contract_t *contract) {
    return contract &&
           contract->action == CONTRACT_TRACTOR &&
           contract->station_index == 1 &&
           contract->commodity == COMMODITY_FERRITE_INGOT &&
           (contract->proof_flags &
            (CONTRACT_PROOF_REQUIRE_PROOF |
             CONTRACT_PROOF_REQUIRE_RECIPE)) ==
               (CONTRACT_PROOF_REQUIRE_PROOF |
                CONTRACT_PROOF_REQUIRE_RECIPE) &&
           contract->required_recipe_id == RECIPE_SMELT &&
           contract->target_index == -1 &&
           memcmp(contract->target_pub,
                  STARTER_REFIT_WORK_ORDER_MARKER,
                  sizeof(contract->target_pub)) == 0;
}

bool contract_slot_available_for_post(const contract_t *contract) {
    return contract && !contract->active &&
           !starter_refit_work_order_matches(contract);
}
