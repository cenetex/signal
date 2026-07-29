/*
 * station_ui.c -- Station lookup helpers, formatting, and the docked station
 * services text renderer.  Split from main.c for issue #99.
 */
#include "client.h"
#include "render.h"
#include "palette.h"
#include "mining_client.h"
#include "contract_objective.h"
#include "manifest.h"
#include "module_schema.h"
#include "faction.h"
#include "station_policy.h"
#include "cargo_lineage.h"
#include "station_authority.h"
#include "contract_fit.h"
#include "chain_log.h"
#include "route_history_labels.h"
#include "ui_clarity.h"
#include <stdarg.h>
/* Grade palette lives in shared/mining.h (pulled in via client.h →
 * types.h → mining.h) alongside the grade enum + label + multiplier. */

#if defined(__GNUC__) || defined(__clang__)
#define SIGNAL_MAYBE_UNUSED __attribute__((unused))
#else
#define SIGNAL_MAYBE_UNUSED
#endif

static const uint8_t COL_TRACKED_JOB[3] = { 255, 222, 51 };

static void station_ui_append_text(char *out, size_t cap, const char *text) {
    if (!out || cap == 0 || !text) return;
    size_t len = strlen(out);
    if (len >= cap) return;
    snprintf(out + len, cap - len, "%s", text);
}

/* ------------------------------------------------------------------ */
/* Station lookup helpers                                              */
/* ------------------------------------------------------------------ */

const station_t* station_at(int station_index) {
    if ((station_index < 0) || (station_index >= MAX_STATIONS)) {
        return NULL;
    }
    return &g.world.stations[station_index];
}

float client_station_stock_amount(const station_t *station,
                                  commodity_t commodity) {
    if (!station || (unsigned)commodity >= COMMODITY_COUNT) return 0.0f;
    if (station >= &g.world.stations[0] &&
        station < &g.world.stations[MAX_STATIONS]) {
        int index = (int)(station - &g.world.stations[0]);
        if (g.station_stock_summary_valid[index])
            return g.station_stock_summary[index][commodity];
    }
    return station_inventory_amount(station, commodity);
}

const station_t* current_station_ptr(void) {
    return station_at(LOCAL_PLAYER.current_station);
}

const station_t* nearby_station_ptr(void) {
    return station_at(LOCAL_PLAYER.nearby_station);
}

int nearest_station_index(vec2 pos) {
    float best_distance_sq = 0.0f;
    int best_index = -1;

    for (int i = 0; i < MAX_STATIONS; i++) {
        float distance_sq = v2_dist_sq(pos, g.world.stations[i].pos);
        if ((best_index < 0) || (distance_sq < best_distance_sq)) {
            best_distance_sq = distance_sq;
            best_index = i;
        }
    }

    return best_index;
}

int player_planned_types(module_type_t *out, int max) {
    if (!out || max <= 0) return 0;
    /* Faction-shared blueprints: count distinct types across all
     * planned slots regardless of who placed them. */
    int count = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        for (int p = 0; p < st->placement_plan_count; p++) {
            module_type_t t = st->placement_plans[p].type;
            bool dup = false;
            for (int k = 0; k < count; k++) {
                if (out[k] == t) { dup = true; break; }
            }
            if (dup) continue;
            if (count >= max) return count;
            out[count++] = t;
        }
    }
    return count;
}

const station_t* navigation_station_ptr(void) {
    if (LOCAL_PLAYER.docked) {
        return current_station_ptr();
    }
    if (LOCAL_PLAYER.nearby_station >= 0) {
        return nearby_station_ptr();
    }
    return station_at(nearest_station_index(LOCAL_PLAYER.ship->pos));
}

/* ------------------------------------------------------------------ */
/* Station role labels and colors                                      */
/* ------------------------------------------------------------------ */

const char* station_role_name(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "Refinery";
        case MODULE_FRAME_PRESS: return "Yard";
        case MODULE_LASER_FAB:   return "Beamworks";
        case MODULE_TRACTOR_FAB: return "Beamworks";
        case MODULE_SIGNAL_RELAY:return "Outpost";
        default:                 return "Station";
    }
}

const char* station_role_short_name(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "REF";
        case MODULE_FRAME_PRESS: return "YARD";
        case MODULE_LASER_FAB:   return "BEAM";
        case MODULE_TRACTOR_FAB: return "BEAM";
        case MODULE_SIGNAL_RELAY:return "OTP";
        default:                 return "STN";
    }
}

const char* station_role_hub_label(const station_t* station) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     return "REFINERY // smelts ore";
        case MODULE_FRAME_PRESS: return "YARD // presses frames";
        case MODULE_LASER_FAB:   return "BEAMWORKS // builds lasers";
        case MODULE_TRACTOR_FAB: return "BEAMWORKS // builds tractors";
        case MODULE_SIGNAL_RELAY:return "OUTPOST // broadcasts signal";
        default:                 return "STATION";
    }
}

void station_role_color(const station_t* station, float* r, float* g0, float* b) {
    module_type_t dom = station_dominant_module(station);
    switch (dom) {
        case MODULE_FURNACE:     PAL_UNPACK3(PAL_MODULE_FURNACE,     *r, *g0, *b); break;
        case MODULE_FRAME_PRESS: PAL_UNPACK3(PAL_MODULE_FRAME_PRESS, *r, *g0, *b); break;
        case MODULE_LASER_FAB:   PAL_UNPACK3(PAL_MODULE_LASER_FAB,   *r, *g0, *b); break;
        case MODULE_TRACTOR_FAB: PAL_UNPACK3(PAL_MODULE_TRACTOR_FAB, *r, *g0, *b); break;
        case MODULE_SIGNAL_RELAY:PAL_UNPACK3(PAL_MODULE_SIGNAL_RELAY, *r, *g0, *b); break;
        default:                 PAL_UNPACK3(PAL_STATION_NEUTRAL,     *r, *g0, *b); break;
    }
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

static int station_manifest_count_c(const station_t *st, commodity_t commodity);
static int station_manifest_count_cg(const station_t *st,
                                     commodity_t commodity,
                                     mining_grade_t grade);
static int ship_manifest_count_c(const ship_t *ship, commodity_t commodity);
static float ship_manifest_backed_cargo_volume(const ship_t *ship);
static void ui_station_name_short(int station_index, char *out, size_t cap);

static void ui_join_module_inputs(module_inputs_t req, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    for (int i = 0; i < req.count; i++) {
        commodity_t c = req.commodities[i];
        if (c >= COMMODITY_COUNT) continue;
        size_t len = strlen(out);
        if (out[0] && len + 1 < cap) {
            station_ui_append_text(out, cap, " + ");
            len = strlen(out);
        }
        if (len + 1 >= cap) break;
        station_ui_append_text(out, cap, commodity_name(c));
    }
}

static bool ui_module_recipe_label(const station_module_t *m,
                                   char *out,
                                   size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!m || m->type >= MODULE_COUNT) return false;

    if (m->scaffold) {
        commodity_t mat = module_build_material_lookup(m->type);
        float cost = module_build_cost_lookup(m->type);
        if (mat < COMMODITY_COUNT) {
            snprintf(out, cap, "%s needs %.0f %s",
                     module_type_name(m->type), cost, commodity_name(mat));
            return true;
        }
        return false;
    }

    if (m->type == MODULE_HOPPER) {
        commodity_t tag = (commodity_t)m->commodity;
        if (tag < COMMODITY_COUNT) {
            snprintf(out, cap, "Hopper feeds %s", commodity_name(tag));
            return true;
        }
        return false;
    }

    if (m->type == MODULE_SHIPYARD) {
        module_inputs_t req = module_required_inputs(MODULE_SHIPYARD);
        char inputs[112];
        ui_join_module_inputs(req, inputs, sizeof(inputs));
        if (inputs[0]) {
            snprintf(out, cap, "%s -> ships/kits", inputs);
            return true;
        }
        return false;
    }

    module_inputs_t req = module_instance_required_inputs(m);
    commodity_t output = module_instance_output(m);
    if (req.count <= 0 || output >= COMMODITY_COUNT) return false;

    char inputs[112];
    ui_join_module_inputs(req, inputs, sizeof(inputs));
    if (!inputs[0]) return false;
    snprintf(out, cap, "%s -> %s", inputs, commodity_name(output));
    return true;
}

static int ui_first_station_recipe_module(const station_t *st)
{
    if (!st) return -1;
    for (int i = 0; i < st->module_count && i < MAX_MODULES_PER_STATION; i++) {
        char recipe[128];
        if (ui_module_recipe_label(&st->modules[i], recipe, sizeof(recipe)))
            return i;
    }
    return -1;
}

static bool ui_station_production_summary_for(const station_t *st,
                                              bool mirrored_authoritative,
                                              char *out,
                                              size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!st) return false;

    station_flow_summary_t summary;
    bool has_flow = station_flow_summary(st, mirrored_authoritative, &summary);
    int module_index = has_flow ? summary.module_index : -1;
    if (module_index < 0)
        module_index = ui_first_station_recipe_module(st);
    if (module_index < 0 || module_index >= st->module_count ||
        module_index >= MAX_MODULES_PER_STATION) {
        if (has_flow)
            return station_flow_summary_format(&summary, out, cap);
        return false;
    }

    char recipe[128];
    if (!ui_module_recipe_label(&st->modules[module_index],
                                recipe, sizeof(recipe))) {
        if (has_flow)
            return station_flow_summary_format(&summary, out, cap);
        return false;
    }

    const char *prefix = has_flow &&
        summary.diag == STATION_FLOW_DIAG_AWAITING_SUPPLY
        ? "Need: " : "Production: ";
    if (has_flow && summary.diag != STATION_FLOW_DIAG_NONE) {
        const char *status = summary.diag == STATION_FLOW_DIAG_RUNNING
            ? "running" : station_flow_diag_label(summary.diag);
        snprintf(out, cap, "%s%s; %s", prefix, recipe, status);
    } else {
        snprintf(out, cap, "%s%s", prefix, recipe);
    }
    return true;
}

bool station_production_summary(char *out, size_t out_size)
{
    bool mirrored_authoritative = g.net_authority_enabled && net_is_connected();
    return ui_station_production_summary_for(current_station_ptr(),
                                             mirrored_authoritative,
                                             out, out_size);
}

static commodity_t ui_upgrade_product_commodity(ship_upgrade_t upgrade)
{
    return (commodity_t)(COMMODITY_FRAME + upgrade_required_product(upgrade));
}

static const char *ui_upgrade_effect_label(ship_upgrade_t upgrade,
                                           const ship_t *ship)
{
    static char label[64];
    label[0] = '\0';
    if (!ship) return "";
    switch (upgrade) {
    case SHIP_UPGRADE_MINING: {
        int next = ship->mining_level + 1;
        if (next == 1) return "unlocks L rocks + Cuprite";
        if (next == 2) return "unlocks XL rocks + Crystal";
        if (next == 3) return "unlocks XXL rocks";
        return "faster fracture";
    }
    case SHIP_UPGRADE_HOLD:
        snprintf(label, sizeof(label), "+%d cargo capacity",
                 (int)SHIP_HOLD_UPGRADE_STEP);
        return label;
    case SHIP_UPGRADE_TRACTOR:
        snprintf(label, sizeof(label), "+%d tractor range",
                 (int)SHIP_TRACTOR_UPGRADE_STEP);
        return label;
    default:
        return "";
    }
}

static module_type_t ui_product_producer_module(commodity_t commodity)
{
    switch (commodity) {
    case COMMODITY_FRAME:          return MODULE_FRAME_PRESS;
    case COMMODITY_LASER_MODULE:   return MODULE_LASER_FAB;
    case COMMODITY_TRACTOR_MODULE: return MODULE_TRACTOR_FAB;
    default:                       return MODULE_COUNT;
    }
}

static bool ui_upgrade_source_label(ship_upgrade_t upgrade,
                                    char *out,
                                    size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';

    commodity_t product = ui_upgrade_product_commodity(upgrade);
    module_type_t producer = ui_product_producer_module(product);
    if (product >= COMMODITY_COUNT || producer >= MODULE_COUNT)
        return false;

    module_inputs_t req = module_required_inputs(producer);
    if (req.count <= 0) return false;

    char inputs[96] = "";
    for (int i = 0; i < req.count; i++) {
        if (req.commodities[i] >= COMMODITY_COUNT) continue;
        if (inputs[0])
            station_ui_append_text(inputs, sizeof(inputs), " + ");
        station_ui_append_text(inputs, sizeof(inputs),
                               commodity_name(req.commodities[i]));
    }
    if (!inputs[0]) return false;

    snprintf(out, cap, "%s: %s", commodity_name(product), inputs);
    return true;
}

static bool ui_upgrade_input_gate_label(ship_upgrade_t upgrade,
                                        const ship_t *ship,
                                        char *out,
                                        size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (!ship || upgrade != SHIP_UPGRADE_MINING || ship->mining_level != 0)
        return false;

    commodity_t product = ui_upgrade_product_commodity(upgrade);
    module_type_t producer = ui_product_producer_module(product);
    if (producer >= MODULE_COUNT) return false;

    module_inputs_t req = module_required_inputs(producer);
    commodity_t gated_source = COMMODITY_COUNT;
    int required = ship->mining_level;
    for (int i = 0; i < req.count; i++) {
        commodity_t source = commodity_ore_for_ingot(req.commodities[i]);
        if (source >= COMMODITY_COUNT)
            source = commodity_ore_form(req.commodities[i]);
        if (source >= COMMODITY_RAW_ORE_COUNT)
            continue;
        int source_required = mining_required_level_for_commodity(source);
        if (source_required > required) {
            required = source_required;
            gated_source = source;
        }
    }
    if (gated_source >= COMMODITY_COUNT) return false;
    if (required <= ship->mining_level) return false;
    snprintf(out, cap, "%s source requires L%d laser",
             commodity_short_name(gated_source), required + 1);
    return true;
}

static bool ui_first_mining_refit_stock_source_label(
    ship_upgrade_t upgrade,
    const ship_t *ship,
    int station_units,
    char *out,
    size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (upgrade != SHIP_UPGRADE_MINING ||
        !ship || ship->mining_level != 0 || station_units <= 0) {
        return false;
    }

    int best_station = -1;
    int best_stock = 0;
    float best_distance_sq = 0.0f;
    int station_count = g.world.station_count;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    for (int s = 0; s < station_count; s++) {
        const station_t *candidate = &g.world.stations[s];
        if (!station_is_active(candidate) ||
            !station_provides_docking(candidate)) {
            continue;
        }
        int stock = (int)floorf(
            client_station_stock_amount(
                candidate, COMMODITY_LASER_MODULE) +
            FLOAT_EPSILON);
        if (stock < station_units) continue;
        float distance_sq = v2_dist_sq(ship->pos, candidate->pos);
        if (best_station < 0 ||
            distance_sq < best_distance_sq) {
            best_station = s;
            best_stock = stock;
            best_distance_sq = distance_sq;
        }
    }
    if (best_station < 0) return false;

    char station_name[16];
    ui_station_name_short(
        best_station, station_name, sizeof(station_name));
    const station_t *source = &g.world.stations[best_station];
    int price = (int)lroundf(
        upgrade_station_credit_cost(
            source, ship, upgrade, station_units));
    const contract_t *work_order = NULL;
    if (strcmp(source->station_slug, "kepler") == 0) {
        for (int i = 0; i < MAX_CONTRACTS; i++) {
            const contract_t *candidate = &g.world.contracts[i];
            if (!candidate->active ||
                !starter_refit_work_order_matches(candidate)) {
                continue;
            }
            if (!work_order ||
                contract_price(candidate) >
                    contract_price(work_order)) {
                work_order = candidate;
            }
        }
    }
    if (work_order && contract_price(work_order) > FLOAT_EPSILON) {
        /*
         * The marked order is an atomic bulk handoff. Its price ages like
         * every visible contract quote, but that must never make the UI
         * advertise fewer units than the server-side quota still requires.
         */
        int haul_units = (int)ceilf(
            work_order->quantity_needed - FLOAT_EPSILON);
        if (haul_units > 0) {
            snprintf(
                out, cap,
                "%s has %d; %d local cr; WORK: haul all %d FE Ingots "
                "from Prospect together; dock + [M]",
                station_name, best_stock, price, haul_units);
            return true;
        }
    }
    snprintf(
        out, cap, "%s has %d; %d cr; dock + [M]",
        station_name, best_stock, price);
    return true;
}

static bool ui_first_mining_refit_production_source_label(
    ship_upgrade_t upgrade,
    const ship_t *ship,
    char *out,
    size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (upgrade != SHIP_UPGRADE_MINING ||
        !ship || ship->mining_level != 0) {
        return false;
    }

    int station_count = g.world.station_count;
    if (station_count > MAX_STATIONS) station_count = MAX_STATIONS;
    for (int s = 0; s < station_count; s++) {
        const station_t *candidate = &g.world.stations[s];
        if (!station_is_active(candidate) ||
            !station_has_module(candidate, MODULE_LASER_FAB)) {
            continue;
        }
        char station_name[16];
        ui_station_name_short(s, station_name, sizeof(station_name));
        snprintf(
            out, cap,
            "%s production pending; check WORK / haul",
            station_name);
        return true;
    }
    return false;
}

static int ui_required_mining_level_for_tier(asteroid_tier_t tier)
{
    switch (tier) {
    case ASTEROID_TIER_XXL: return 3;
    case ASTEROID_TIER_XL:  return 2;
    case ASTEROID_TIER_L:   return 1;
    default:                return 0;
    }
}

static int ui_required_mining_level_for_asteroid(const asteroid_t *a)
{
    if (!a || !a->active || asteroid_is_collectible(a))
        return 0;
    int material = mining_required_level_for_commodity(a->commodity);
    int size = ui_required_mining_level_for_tier((asteroid_tier_t)a->tier);
    return material > size ? material : size;
}

static bool ui_contract_fracture_target(const contract_t *ct,
                                        const asteroid_t **out)
{
    if (out) *out = NULL;
    if (!ct || ct->action != CONTRACT_FRACTURE) return false;
    int idx = ct->target_index;
    if (idx >= 0 && idx < MAX_ASTEROIDS &&
        g.world.asteroids[idx].active &&
        contract_asteroid_target_matches(ct, &g.world.asteroids[idx])) {
        if (out) *out = &g.world.asteroids[idx];
        return true;
    }
    if (!contract_target_pub_is_set(ct)) return false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active) continue;
        if (!contract_asteroid_target_matches(ct, a)) continue;
        if (out) *out = a;
        return true;
    }
    return false;
}

static int ui_contract_required_mining_level(const contract_t *ct)
{
    if (!ct || !ct->active) return 0;
    if (ct->action == CONTRACT_FRACTURE) {
        const asteroid_t *target = NULL;
        if (ui_contract_fracture_target(ct, &target))
            return ui_required_mining_level_for_asteroid(target);
        if (ct->commodity < COMMODITY_COUNT)
            return mining_required_level_for_commodity(ct->commodity);
        return 0;
    }
    if (ct->action == CONTRACT_TRACTOR &&
        ct->commodity < COMMODITY_RAW_ORE_COUNT) {
        return mining_required_level_for_commodity(ct->commodity);
    }
    return 0;
}

static bool ui_contract_laser_gate_note(const contract_t *ct,
                                        char *out,
                                        size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    int required = ui_contract_required_mining_level(ct);
    if (required <= LOCAL_PLAYER.ship->mining_level) return false;
    snprintf(out, cap, "requires L%d laser", required + 1);
    return true;
}

bool station_laser_refit_summary(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';

    station_ui_state_t ui = { 0 };
    build_station_ui_state(&ui);
    const ship_t *ship = LOCAL_PLAYER.ship;
    if (!ui.station || ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING))
        return false;

    char source[112];
    char gate[80];
    char supply[128];
    bool has_source = ui_upgrade_source_label(SHIP_UPGRADE_MINING,
                                              source, sizeof(source));
    bool has_gate = ui_upgrade_input_gate_label(SHIP_UPGRADE_MINING, ship,
                                                gate, sizeof(gate));
    int short_by = ui.mining_units_needed -
        (ui.mining_units_in_cargo + ui.mining_units_at_station);
    if (short_by < 0) short_by = 0;
    int from_station = ui.mining_units_needed -
        (ui.mining_units_needed < ui.mining_units_in_cargo
             ? ui.mining_units_needed
             : ui.mining_units_in_cargo);
    if (from_station < 0) from_station = 0;
    bool has_supply = ui_first_mining_refit_stock_source_label(
        SHIP_UPGRADE_MINING, ship, from_station,
        supply, sizeof(supply));
    if (!has_supply) {
        has_supply = ui_first_mining_refit_production_source_label(
            SHIP_UPGRADE_MINING, ship, supply, sizeof(supply));
    }

    if (has_supply && has_source && has_gate) {
        snprintf(out, out_size,
                 "need %d Laser Modules; ship %d station %d; %s; %s; %s",
                 short_by, ui.mining_units_in_cargo,
                 ui.mining_units_at_station, supply, source, gate);
    } else if (has_supply && has_source) {
        snprintf(out, out_size,
                 "need %d Laser Modules; ship %d station %d; %s; %s",
                 short_by, ui.mining_units_in_cargo,
                 ui.mining_units_at_station, supply, source);
    } else if (has_source && has_gate) {
        snprintf(out, out_size,
                 "need %d Laser Modules; ship %d station %d; %s; %s",
                 short_by, ui.mining_units_in_cargo,
                 ui.mining_units_at_station, source, gate);
    } else if (has_source) {
        snprintf(out, out_size,
                 "need %d Laser Modules; ship %d station %d; %s",
                 short_by, ui.mining_units_in_cargo,
                 ui.mining_units_at_station, source);
    } else {
        snprintf(out, out_size,
                 "need %d Laser Modules; ship %d station %d",
                 short_by, ui.mining_units_in_cargo,
                 ui.mining_units_at_station);
    }
    return true;
}

static const NetDeliveryLedgerEntry *ui_delivery_ledger_for_contract(
    int contract_index)
{
    for (int i = 0; i < g.delivery_ledger_count; i++) {
        const NetDeliveryLedgerEntry *entry = &g.delivery_ledger[i];
        if (entry->contract_index == (uint8_t)contract_index)
            return entry;
    }
    return NULL;
}

static const NetDeliveryLedgerEntry *ui_delivery_ledger_for_shipment(
    uint16_t shipment_id)
{
    if (shipment_id == 0) return NULL;
    for (int i = 0; i < g.delivery_ledger_count; i++) {
        const NetDeliveryLedgerEntry *entry = &g.delivery_ledger[i];
        if (entry->shipment_id == shipment_id) return entry;
    }
    return NULL;
}

static bool ui_credit_cargo_route_label(const contract_t *ct,
                                        char *out,
                                        size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!ct || ct->action != CONTRACT_DELIVERY ||
        ct->target_index < 0 || ct->target_index >= MAX_STATIONS ||
        ct->station_index >= MAX_STATIONS) {
        return false;
    }
    char origin[12];
    char dest[12];
    ui_station_name_short(ct->target_index, origin, sizeof(origin));
    ui_station_name_short(ct->station_index, dest, sizeof(dest));
    snprintf(out, out_size, "cargo %s>%s", origin, dest);
    return true;
}

static int ui_contract_quantity_goal(const contract_t *ct)
{
    int qty = (ct && ct->quantity_needed > 0.5f)
            ? (int)ceilf(ct->quantity_needed)
            : 1;
    return qty > 0 ? qty : 1;
}

static bool ui_cargo_unit_is_named_ingot(const cargo_unit_t *unit)
{
    return unit && (cargo_kind_t)unit->kind == CARGO_KIND_INGOT &&
           (ingot_prefix_t)unit->prefix_class != INGOT_PREFIX_ANONYMOUS;
}

int station_contract_source_stock_count(const station_t *st,
                                        const contract_t *ct)
{
    if (!st || !ct ||
        ct->commodity < 0 || ct->commodity >= COMMODITY_COUNT) {
        return 0;
    }

    if (st->manifest.units && st->manifest.count > 0) {
        int count = 0;
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            const cargo_unit_t *unit = &st->manifest.units[i];
            if (!contract_fit_is_ok(contract_fit_cargo_unit(ct, unit)))
                continue;
            if (ct->proof_flags == 0 && ui_cargo_unit_is_named_ingot(unit))
                continue;
            count++;
        }
        return count;
    }

    if (ct->proof_flags != 0) return 0;
    if (ct->required_grade >= MINING_GRADE_COUNT)
        return 0;

    int count = 0;
    for (int gi = (int)ct->required_grade; gi < MINING_GRADE_COUNT; gi++) {
        count += station_manifest_count_cg(st, ct->commodity,
                                           (mining_grade_t)gi);
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* CONTRACTS-panel slot builder                                        */
/* ------------------------------------------------------------------ */

int build_work_slots(int here_idx, vec2 here_pos,
                     int out_contracts[3],
                     bool out_fulfillable[3],
                     int out_held[3])
{
    for (int i = 0; i < 3; i++) {
        out_contracts[i]   = -1;
        out_fulfillable[i] = false;
        out_held[i]        = 0;
    }
    int count = 0;

    /* Pass 1: contracts at this station the player can act on
     * right now. Raw ore lives in towed S-tier fragments; finished goods
     * are counted from the ship manifest. */
    for (int ci = 0; ci < MAX_CONTRACTS && count < 3; ci++) {
        /* Gossip filter: only show contracts the player has heard
         * about via dock contact. Mask is set by the per-player
         * NET_MSG_PLAYER_KNOWN_CONTRACTS message. */
        if (!(g.player_known_contract_mask & (1u << ci))) continue;
        const contract_t *ct = &g.world.contracts[ci];
        if (!ct->active) continue;
        int held_int = 0;
        bool actionable_here = false;
        if (ct->action == CONTRACT_DELIVERY) {
            const NetDeliveryLedgerEntry *ledger =
                ui_delivery_ledger_for_contract(ci);
            bool at_origin = here_idx >= 0 && ct->target_index == here_idx;
            bool at_dest = here_idx >= 0 && ct->station_index == here_idx;
            held_int = ledger ? (int)ledger->held_bound : 0;
            if (at_origin && ledger &&
                ledger->status == DELIVERY_SHIPMENT_DELIVERED) {
                actionable_here = true;
                held_int = ledger->quantity_total > 0
                    ? (int)ledger->quantity_total
                    : ui_contract_quantity_goal(ct);
            } else if (at_origin && !ledger) {
                int source_stock = station_contract_source_stock_count(
                    &g.world.stations[here_idx], ct);
                if (source_stock > 0) {
                    actionable_here = true;
                    held_int = source_stock;
                    int goal = ui_contract_quantity_goal(ct);
                    if (held_int > goal) held_int = goal;
                }
            } else if (at_dest && ledger &&
                       ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
                       held_int > 0) {
                actionable_here = true;
            }
        } else if (ct->action == CONTRACT_TRACTOR) {
            if (here_idx < 0 || ct->station_index != here_idx) continue;
            if (ct->commodity < COMMODITY_RAW_ORE_COUNT) {
                float held_ore = 0.0f;
                const ship_t *ship = LOCAL_PLAYER.ship;
                for (int t = 0; t < ship->towed_count; t++) {
                    int fi = ship->towed_fragments[t];
                    if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
                    const asteroid_t *a = &g.world.asteroids[fi];
                    if (!contract_fit_is_ok(contract_fit_fragment(ct, a))) continue;
                    held_ore += a->ore;
                }
                held_int = (int)lroundf(held_ore);
            } else {
                held_int = contract_fit_manifest_count(
                    ct, &LOCAL_PLAYER.ship->manifest);
            }
            actionable_here = held_int > 0;
        }
        if (!actionable_here) continue;
        out_contracts[count]   = ci;
        out_fulfillable[count] = true;
        out_held[count]        = held_int;
        count++;
    }

    /* Pass 2: fill any remaining slots with the nearest active
     * contracts (any station) by squared distance from `here_pos`,
     * skipping anything already in pass 1. */
    if (count < 3) {
        int nearest[3] = {-1, -1, -1};
        float nearest_d[3] = {1e18f, 1e18f, 1e18f};
        for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
            if (!(g.player_known_contract_mask & (1u << ci))) continue; /* gossip filter */
            const contract_t *ct = &g.world.contracts[ci];
            if (!ct->active) continue;
            if (ct->action == CONTRACT_DELIVERY) {
                if (ct->station_index >= MAX_STATIONS ||
                    ct->target_index < 0 ||
                    ct->target_index >= MAX_STATIONS) {
                    continue;
                }
                if (!station_exists(&g.world.stations[ct->station_index]) ||
                    !station_exists(&g.world.stations[ct->target_index])) {
                    continue;
                }
            } else {
                if (ct->station_index >= MAX_STATIONS) continue;
                if (!station_exists(&g.world.stations[ct->station_index])) continue;
            }
            bool already = false;
            for (int s = 0; s < count; s++)
                if (out_contracts[s] == ci) { already = true; break; }
            if (already) continue;
            vec2 target = ct->target_pos;
            if (ct->action == CONTRACT_TRACTOR) {
                target = g.world.stations[ct->station_index].pos;
            } else if (ct->action == CONTRACT_DELIVERY) {
                const NetDeliveryLedgerEntry *ledger =
                    ui_delivery_ledger_for_contract(ci);
                int station_idx = ct->target_index;
                if (ledger && ledger->status == DELIVERY_SHIPMENT_PICKED_UP)
                    station_idx = ct->station_index;
                if (ledger && ledger->status == DELIVERY_SHIPMENT_DELIVERED)
                    station_idx = ct->target_index;
                target = g.world.stations[station_idx].pos;
            }
            float d = v2_dist_sq(here_pos, target);
            for (int s = 0; s < 3; s++) {
                if (d < nearest_d[s]) {
                    for (int j = 2; j > s; j--) {
                        nearest[j] = nearest[j-1];
                        nearest_d[j] = nearest_d[j-1];
                    }
                    nearest[s] = ci;
                    nearest_d[s] = d;
                    break;
                }
            }
        }
        for (int s = 0; s < 3 && count < 3; s++) {
            if (nearest[s] < 0) continue;
            out_contracts[count]   = nearest[s];
            out_fulfillable[count] = false;
            out_held[count]        = 0;
            count++;
        }
    }

    return count;
}

/* ------------------------------------------------------------------ */
/* Station UI state builder                                            */
/* ------------------------------------------------------------------ */

void build_station_ui_state(station_ui_state_t* ui) {
    memset(ui, 0, sizeof(*ui));
    ui->station = current_station_ptr();
    if (!ui->station) {
        return;
    }

    ui->hull_now = (int)lroundf(LOCAL_PLAYER.ship->hull);
    ui->hull_max = (int)lroundf(ship_max_hull(LOCAL_PLAYER.ship));
    float repair = station_repair_cost(LOCAL_PLAYER.ship, current_station_ptr());
    ui->repair_cost = (int)lroundf(repair);

    /* Compute per-upgrade module accounting (cargo first, dock fallback).
     * Finished goods are manifest-backed; floats are only derived caches. */
    struct { ship_upgrade_t up; int *needed, *cargo, *atstation, *credit; } slots[3] = {
        { SHIP_UPGRADE_MINING,
          &ui->mining_units_needed,  &ui->mining_units_in_cargo,
          &ui->mining_units_at_station, &ui->mining_credit_cost },
        { SHIP_UPGRADE_HOLD,
          &ui->hold_units_needed,    &ui->hold_units_in_cargo,
          &ui->hold_units_at_station,   &ui->hold_credit_cost },
        { SHIP_UPGRADE_TRACTOR,
          &ui->tractor_units_needed, &ui->tractor_units_in_cargo,
          &ui->tractor_units_at_station,&ui->tractor_credit_cost },
    };
    for (int i = 0; i < 3; i++) {
        commodity_t c = (commodity_t)(COMMODITY_FRAME +
                        upgrade_required_product(slots[i].up));
        int need = (int)ceilf(upgrade_product_cost(LOCAL_PLAYER.ship, slots[i].up));
        int in_cargo  = ship_manifest_count_c(LOCAL_PLAYER.ship, c);
        int at_station = ui->station ? station_manifest_count_c(ui->station, c) : 0;
        int from_station = need - (need < in_cargo ? need : in_cargo);
        if (from_station < 0) from_station = 0;
        float credit = ui->station
            ? upgrade_station_credit_cost(ui->station, LOCAL_PLAYER.ship,
                                          slots[i].up, from_station)
            : 0.0f;
        *slots[i].needed    = need;
        *slots[i].cargo     = in_cargo;
        *slots[i].atstation = at_station;
        *slots[i].credit    = (int)lroundf(credit);
    }
    /* Any dock installs kits — gate is whether there are kits available
     * (computed below) and whether the quoted cost is affordable. */
    ui->can_repair = (repair > 0.0f) && (player_current_balance() + FLOAT_EPSILON >= repair);

    /* Kit availability for the [R] row — drives "X kits ship / Y kits
     * station" hint and the partial-repair warning. */
    ui->ship_kits    = ship_manifest_count_c(LOCAL_PLAYER.ship,
                                             COMMODITY_REPAIR_KIT);
    ui->station_kits = (ui->station)
        ? station_manifest_count_c(ui->station, COMMODITY_REPAIR_KIT) : 0;
    int hp_needed = ui->hull_max - ui->hull_now;
    if (hp_needed < 0) hp_needed = 0;
    int kits_avail = ui->ship_kits + ui->station_kits;
    ui->kits_short_by = (hp_needed > kits_avail) ? (hp_needed - kits_avail) : 0;
    float bal = player_current_balance();
    ui->can_upgrade_mining =
        can_afford_upgrade(ui->station, LOCAL_PLAYER.ship,
                           SHIP_UPGRADE_MINING, bal);
    ui->can_upgrade_hold =
        can_afford_upgrade(ui->station, LOCAL_PLAYER.ship,
                           SHIP_UPGRADE_HOLD, bal);
    ui->can_upgrade_tractor =
        can_afford_upgrade(ui->station, LOCAL_PLAYER.ship,
                           SHIP_UPGRADE_TRACTOR, bal);
}

/* ------------------------------------------------------------------ */
/* Currency + ingot stock helpers                                      */
/* ------------------------------------------------------------------ */

/* Station-local currency label, falls back to "cr". */
static const char *ui_station_currency(const station_t *st) {
    if (!st) return "cr";
    return (st->currency_name[0]) ? st->currency_name : "cr";
}

/* Compact form of the currency name — first word, lowercased and
 * trimmed to <= 4 chars. Used by the header band when the full label
 * ("prospect vouchers", 17 chars) won't fit. Falls back to "cr".
 * Caller-owned buffer must be >= 5 bytes. */
static void ui_station_currency_short(const station_t *st, char *out, size_t cap) {
    if (cap == 0) return;
    if (!st || !st->currency_name[0]) {
        snprintf(out, cap, "cr");
        return;
    }
    /* Take the first whitespace-delimited word. "prospect vouchers" -> "prospect" */
    size_t i = 0, j = 0;
    while (st->currency_name[i] != '\0' && st->currency_name[i] != ' ' &&
           j < cap - 1 && j < 4) {
        char c = st->currency_name[i];
        out[j++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        i++;
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, cap, "cr");
}

static void ui_station_name_short(int station_index, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (station_index < 0 || station_index >= MAX_STATIONS) {
        snprintf(out, cap, "station");
        return;
    }
    const station_t *st = &g.world.stations[station_index];
    if (!station_exists(st) || !st->name[0]) {
        snprintf(out, cap, "station");
        return;
    }
    size_t i = 0, j = 0;
    while (st->name[i] != '\0' && st->name[i] != ' ' && j < cap - 1) {
        out[j++] = st->name[i++];
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, cap, "station");
}

static void ui_fit_text(const char *src, int max_chars, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!src || max_chars <= 0) return;

    size_t limit = (size_t)max_chars;
    if (limit >= cap) limit = cap - 1;
    size_t len = strlen(src);
    if (len <= limit) {
        snprintf(out, cap, "%s", src);
        return;
    }

    if (limit <= 3) {
        for (size_t i = 0; i < limit; i++) out[i] = '.';
        out[limit] = '\0';
        return;
    }

    snprintf(out, cap, "%.*s...", (int)(limit - 3), src);
}

static void ui_append_bounded(char *out, size_t cap, size_t *used,
                              const char *src) {
    if (!out || cap == 0 || !used || !src) return;
    if (*used >= cap) {
        out[cap - 1] = '\0';
        *used = cap - 1;
        return;
    }
    size_t room = cap - *used - 1;
    size_t len = strlen(src);
    if (len > room) len = room;
    memcpy(out + *used, src, len);
    *used += len;
    out[*used] = '\0';
}

static void ui_write_parts(char *out, size_t cap,
                           const char *a,
                           const char *b,
                           const char *c) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    size_t used = 0;
    ui_append_bounded(out, cap, &used, a);
    ui_append_bounded(out, cap, &used, b);
    ui_append_bounded(out, cap, &used, c);
}

static bool ui_ledger_snapshot_enabled(void) {
    /* Loopback deliberately uses the same serialized private-state lane as
     * multiplayer. Reading its recipient-scoped snapshot here keeps the
     * single-player acceptance path honest instead of falling back to a
     * client-world ledger that a remote client never receives. */
    return g.net_authority_enabled;
}

static bool ui_known_station_balance(int station_idx, float *out) {
    if (out) *out = 0.0f;
    if (station_idx < 0 || station_idx >= MAX_STATIONS) return false;
    for (int i = 0; i < g.known_station_ledger_count; i++) {
        if (g.known_station_ledger[i].station != (uint8_t)station_idx)
            continue;
        if (out) *out = g.known_station_ledger[i].balance;
        return true;
    }
    return false;
}

static bool ui_local_player_pubkey(uint8_t out[32]) {
    if (!out) return false;
    if (g.local_player_slot < 0 || g.local_player_slot >= MAX_PLAYERS)
        return false;
    if (ui_ledger_snapshot_enabled()) {
        memset(out, 0, 32);
        return false;
    }
    client_session_pseudo_pubkey(
        g.world.players[g.local_player_slot].session_token, out);
    return true;
}

static bool ui_station_balance_for_pubkey(int station_idx,
                                          const uint8_t pubkey[32],
                                          float *out)
{
    if (out) *out = 0.0f;
    if (!pubkey || station_idx < 0 || station_idx >= MAX_STATIONS)
        return false;
    const station_t *st = &g.world.stations[station_idx];
    if (!station_exists(st)) return false;
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_pubkey, pubkey, 32) == 0) {
            if (out) *out = st->ledger[i].balance;
            return true;
        }
    }
    return false;
}

static bool ui_station_balance_for_player(int station_idx, float *out) {
    if (out) *out = 0.0f;
    if (ui_ledger_snapshot_enabled())
        return ui_known_station_balance(station_idx, out);
    uint8_t pubkey[32];
    if (!ui_local_player_pubkey(pubkey))
        return false;
    return ui_station_balance_for_pubkey(station_idx, pubkey, out);
}

static int ui_build_ledger_strip(int current_station,
                                 char *out,
                                 size_t cap)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';

    const station_t *cur = (current_station >= 0 && current_station < MAX_STATIONS)
        ? &g.world.stations[current_station] : NULL;
    if (!cur || !station_exists(cur)) return 0;

    char cur_name[16], cur_cur[8];
    ui_station_name_short(current_station, cur_name, sizeof(cur_name));
    ui_station_currency_short(cur, cur_cur, sizeof(cur_cur));
    int cur_bal = (int)lroundf(player_current_balance());

    int written = snprintf(out, cap, "%s %d %s", cur_name, cur_bal, cur_cur);
    if (written < 0) {
        out[0] = '\0';
        return 0;
    }
    size_t used = (size_t)written < cap ? (size_t)written : cap - 1;
    int rows = 1;

    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == current_station) continue;
        float bal = 0.0f;
        if (!ui_station_balance_for_player(s, &bal)) continue;
        if (fabsf(bal) < 0.5f) continue;
        char name[16], cur_short[8], piece[48];
        ui_station_name_short(s, name, sizeof(name));
        ui_station_currency_short(&g.world.stations[s], cur_short,
                                  sizeof(cur_short));
        snprintf(piece, sizeof(piece), "   %s %d %s",
                 name, (int)lroundf(bal), cur_short);
        size_t need = strlen(piece);
        if (used + need >= cap) break;
        memcpy(out + used, piece, need + 1);
        used += need;
        rows++;
        if (rows >= 4) break;
    }
    return rows;
}

/* ====================================================================
 * STATION DOCKED UI — redesigned (#redesign)
 *
 * Layout:
 *   ┌─ persistent header band (always rendered) ─┐
 *   │ name · role · balance here · signal · LAUNCH │
 *   │ hull X/Y · cargo X/Y                        │
 *   │ ⌁ "ticker line" (faded)                     │
 *   ├─────────────────────────────────────────────┤
 *   │ view content (descriptor-selected):         │
 *   │   SHIP  — hull, hold, repair, refit         │
 *   │   TRADE — station market                    │
 *   │   CONTRACTS — station contract board        │
 *   │   YARD  — scaffold orders and queue         │
 *   └─────────────────────────────────────────────┘
 *
 * Every visible row maps to a single keypress that actually does something
 * at THIS dock. Passive rows explain missing stock, cargo, credits, or
 * materials directly on the right side.
 * ==================================================================== */

static void draw_header_band(const station_ui_state_t *ui,
                             float panel_x, float panel_y,
                             float panel_w, bool compact)
{
    const station_t *st = ui->station;
    float left_x = panel_x + 20.0f;
    float right_margin = 20.0f;
    const float cell_w = 8.0f;

    /* Header lines sit below the outer panel's top chrome (corner brackets
     * and title rule at y+14). Keep line 1 >= panel_y + 22 to avoid clipping.
     * Layout (per redesign):
     *   Line 1: station name (left)   ·   [E] LAUNCH (right)
     *   Line 2: station role (left)   ·   balance N cur · sig X.XX (right)
     *   Line 3: ticker (full width)
     * Ship hull/cargo/modules live in the footer + the SHIP panel. */
    const float HEADER_L1 = 26.0f;
    const float HEADER_L2 = 42.0f;
    const float HEADER_L3 = 58.0f;

    /* Line 1: station name (+ Ed25519 pubkey prefix) (left)  ·
     *         [E] LAUNCH (right).
     * The pubkey suffix lets the player visually confirm they're
     * docked at a legitimately-keyed station, not a spoof with the
     * same name (#479 B). */
    sdtx_color3b(PAL_TEXT_PRIMARY);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L1));
    {
        const char *launch = "[E] LAUNCH";
        float launch_w = (panel_w >= 360.0f) ? (float)strlen(launch) * cell_w : 0.0f;
        float title_right = panel_x + panel_w - right_margin
                          - (launch_w > 0.0f ? launch_w + 16.0f : 0.0f);
        int title_chars = (int)floorf((title_right - left_x) / cell_w);
        char name_with_pub[96];
        char name_only[64];
        char title_fit[96];
        char pub_prefix[16];
        station_pubkey_b58_prefix(st, pub_prefix);
        if (pub_prefix[0])
            snprintf(name_with_pub, sizeof(name_with_pub), "%s (%s...)",
                     st->name, pub_prefix);
        else
            snprintf(name_with_pub, sizeof(name_with_pub), "%s", st->name);
        snprintf(name_only, sizeof(name_only), "%s", st->name);

        const char *title = (!compact && (int)strlen(name_with_pub) <= title_chars)
            ? name_with_pub : name_only;
        ui_fit_text(title, title_chars, title_fit, sizeof(title_fit));
        sdtx_puts(title_fit);
    }

    if (panel_w >= 360.0f) {
        const char *launch = "[E] LAUNCH";
        float lw = (float)strlen(launch) * cell_w;
        sdtx_pos(ui_text_pos(panel_x + panel_w - right_margin - lw),
                 ui_text_pos(panel_y + HEADER_L1));
        sdtx_color3b(PAL_STATION_HINT);
        sdtx_puts(launch);
    }

    /* Line 2: role (left)  ·  balance + signal (right).
     * Role labels are long ("BEAMWORKS // builds tractors"), and the right
     * side can run to ~35 chars. Measure both and skip the right-side
     * data if they'd overlap — compact panels land there. The balance
     * remains visible in the SHIP panel's SHIP BAY section. */
    const char *role_label = station_role_hub_label(st);
    float right_limit = panel_x + panel_w - right_margin;
    int role_chars = (int)floorf((right_limit - left_x) / cell_w);
    char role_fit[64];
    ui_fit_text(role_label, role_chars, role_fit, sizeof(role_fit));
    float role_w = (float)strlen(role_fit) * cell_w;
    sdtx_color3b(PAL_HOLD_CYAN);
    sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L2));
    sdtx_puts(role_fit);

    if (panel_w >= 360.0f) {
        int balance = (int)lroundf(player_current_balance());
        float sig = signal_strength_at(&g.world, st->pos);
        float gap = 16.0f;
        float left_used = left_x + role_w + gap;

        /* Try four progressively shorter forms, each guaranteed to
         * convey at least the balance. Pick the longest that fits. */
        char short_cur[8];
        ui_station_currency_short(st, short_cur, sizeof(short_cur));
        const char *full_cur = ui_station_currency(st);
        const char *forms[4];
        char buf0[64], buf1[64], buf2[64], buf3[32];
        snprintf(buf0, sizeof(buf0), "balance %d %s   sig %.2f", balance, full_cur,  sig);
        snprintf(buf1, sizeof(buf1), "balance %d %s",            balance, full_cur);
        snprintf(buf2, sizeof(buf2), "%d %s   sig %.2f",         balance, short_cur, sig);
        snprintf(buf3, sizeof(buf3), "%d %s",                    balance, short_cur);
        forms[0] = buf0; forms[1] = buf1; forms[2] = buf2; forms[3] = buf3;

        for (int i = 0; i < 4; i++) {
            float w = (float)strlen(forms[i]) * cell_w;
            if (left_used + w > right_limit) continue;
            sdtx_pos(ui_text_pos(right_limit - w),
                     ui_text_pos(panel_y + HEADER_L2));
            sdtx_color3b(PAL_TEXT_SECONDARY);
            sdtx_puts(forms[i]);
            break;
        }
    }

    /* Line 3: current + known station-local balances. If there are no
     * cross-station balances yet, fall back to ticker/political identity. */
    {
        const signal_channel_t *ch = &g.world.signal_channel;
        char line[200];
        bool have_line = ui_build_ledger_strip(
            LOCAL_PLAYER.current_station, line, sizeof(line)) > 1;
        bool ledger_line = have_line;
        if (!have_line && ch->count > 0) {
            int slot_idx = ch->count - 1;
            int start = (ch->head - ch->count + SIGNAL_CHANNEL_CAPACITY) % SIGNAL_CHANNEL_CAPACITY;
            int slot = (start + slot_idx) % SIGNAL_CHANNEL_CAPACITY;
            const signal_channel_msg_t *m = &ch->msgs[slot];
            const char *sender = "SYSTEM";
            if (m->sender_station >= 0 && m->sender_station < MAX_STATIONS
                && station_exists(&g.world.stations[m->sender_station])) {
                sender = g.world.stations[m->sender_station].name;
            }
            snprintf(line, sizeof(line), "[%s] %s", sender, m->text);
            have_line = true;
        } else {
            int blackglass_rel = station_faction_relation_to(
                st, (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE);
            snprintf(line, sizeof(line), "%s // %s // blackglass %s",
                     station_faction_name(st->faction_id),
                     station_ideology_name(st->faction_ideology),
                     station_faction_relation_label(blackglass_rel));
            have_line = true;
        }
        if (have_line) {
            int line_chars = (int)floorf((panel_x + panel_w - right_margin - left_x) / cell_w);
            char line_fit[200];
            ui_fit_text(line, line_chars, line_fit, sizeof(line_fit));
            if (ledger_line)
                sdtx_color3b(PAL_STATION_HINT);
            else
                sdtx_color3b(PAL_TEXT_FADED);
            sdtx_pos(ui_text_pos(left_x), ui_text_pos(panel_y + HEADER_L3));
            sdtx_puts(line_fit);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Shared row helpers                                                   */
/* ------------------------------------------------------------------ */

/* Small section header: label on the left, faded rule filling the row
 * to the inner right edge. Returns the y-advance consumed. */
static float draw_section_header(float cx, float my, float inner_right,
                                 const char *label, const uint8_t label_rgb[3])
{
    const float cell_w = 8.0f;
    sdtx_color3b(label_rgb[0], label_rgb[1], label_rgb[2]);
    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
    sdtx_puts(label);
    float label_w = (float)strlen(label) * cell_w;
    /* Faded dash rule after the label. */
    sdtx_color3b(PAL_TEXT_FADED);
    int chars_avail = (int)((inner_right - (cx + label_w + 8.0f)) / cell_w);
    if (chars_avail > 0) {
        char rule[128];
        int n = (chars_avail < (int)sizeof(rule) - 1)
              ? chars_avail : (int)sizeof(rule) - 1;
        for (int i = 0; i < n; i++) rule[i] = '-';
        rule[n] = '\0';
        sdtx_pos(ui_text_pos(cx + label_w + 8.0f), ui_text_pos(my));
        sdtx_puts(rule);
    }
    return 14.0f;
}

/* Forward decl — yard view lives below (own tab). */
static void draw_yard_view(const station_ui_state_t *ui,
                           float cx, float cy, float inner_w, bool compact);
void station_panel_input_dock(input_intent_t *intent);
void station_panel_input_trade(input_intent_t *intent);
void station_panel_input_work(input_intent_t *intent);
void station_panel_input_history(input_intent_t *intent);
void station_panel_input_yard(input_intent_t *intent);

/* ------------------------------------------------------------------ */
/* Row grammar — every tab uses this shape:                            */
/*   left (hotkey + verb/label)    middle optional    right status     */
/* Monospace cell width is 8 px.                                       */
/* ------------------------------------------------------------------ */

/* Two-column row: left-aligned label at cx, right-aligned status at
 * inner_right. Either side may be NULL to skip it. */
static void draw_row_lr(float cx, float my, float inner_right,
                        const uint8_t left_rgb[3], const char *left_txt,
                        const uint8_t right_rgb[3], const char *right_txt)
{
    const float cell_w = 8.0f;
    char right_fit[96];
    const char *right_draw = NULL;
    float right_w = 0.0f;
    float right_x = inner_right;

    if (right_txt && right_txt[0] && right_rgb) {
        int right_chars = (int)floorf((inner_right - cx) / cell_w);
        ui_fit_text(right_txt, right_chars, right_fit, sizeof(right_fit));
        if (right_fit[0]) {
            right_draw = right_fit;
            right_w = (float)strlen(right_draw) * cell_w;
            right_x = inner_right - right_w;
        }
    }

    if (left_txt && left_txt[0]) {
        char left_fit[96];
        const char *left_draw = left_fit;
        int left_chars;
        if (right_draw) {
            left_chars = (int)floorf((right_x - cx - 8.0f) / cell_w);
        } else {
            left_chars = (int)floorf((inner_right - cx) / cell_w);
        }
        ui_fit_text(left_txt, left_chars, left_fit, sizeof(left_fit));
        if (left_draw[0]) {
            sdtx_color3b(left_rgb[0], left_rgb[1], left_rgb[2]);
            sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
            sdtx_puts(left_draw);
        }
    }
    if (right_draw && right_rgb) {
        sdtx_color3b(right_rgb[0], right_rgb[1], right_rgb[2]);
        sdtx_pos(ui_text_pos(right_x), ui_text_pos(my));
        sdtx_puts(right_draw);
    }
}

static bool station_row_has_room(float my, float row_h, float content_bottom)
{
    return my + row_h <= content_bottom;
}

static void draw_more_rows_hint(float cx, float my, const char *label)
{
    sdtx_color3b(PAL_TEXT_FADED);
    sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
    sdtx_puts(label ? label : "more rows hidden");
}

/* Cell-grid row: writes each field at a fixed column offset (in chars)
 * measured from cx. NULL field = skip. */
typedef struct { int col; const char *text; const uint8_t *rgb; } cell_t;
static void draw_row_cells(float cx, float my, const cell_t *cells, int n)
{
    const float cell_w = 8.0f;
    for (int i = 0; i < n; i++) {
        if (!cells[i].text || !cells[i].text[0]) continue;
        sdtx_color3b(cells[i].rgb[0], cells[i].rgb[1], cells[i].rgb[2]);
        sdtx_pos(ui_text_pos(cx + (float)cells[i].col * cell_w), ui_text_pos(my));
        sdtx_puts(cells[i].text);
    }
}

/* Section-header color families, shared by DOCK and TRADE views. */
static const uint8_t HDR_TRADE[3]   = { PAL_CONTRACT_AFFORD };
static const uint8_t HDR_SERVICE[3] = { PAL_ORE_AMBER };
static const uint8_t HDR_FIT[3]     = { PAL_NAV_BLUE };
static const uint8_t HDR_YARD[3]    = { PAL_HOLD_CYAN };

/* Station manifest readers — unified through the client-side summary
 * (g.station_manifest_summary), populated by the network stream or by the
 * offline fallback. UI no longer pokes at station_t.manifest directly;
 * the summary is the only contract. */
static int station_index_of(const station_t *st) {
    return (int)(st - g.world.stations);
}

void reset_trade_session_rows(int station_index) {
    g.trade_session_station = station_index;
    g.trade_page = 0;
    trade_lineage_close();
}

static int trade_session_station_index(const station_t *st) {
    if (!st) return -1;
    int s = station_index_of(st);
    if (s < 0 || s >= MAX_STATIONS) return -1;
    if (g.trade_session_station != s) reset_trade_session_rows(s);
    return s;
}

static int station_manifest_count_cg(const station_t *st,
                                     commodity_t commodity,
                                     mining_grade_t grade)
{
    if (!st) return 0;
    int s = station_index_of(st);
    if (s < 0 || s >= MAX_STATIONS) return 0;
    if ((int)commodity < 0 || (int)commodity >= COMMODITY_COUNT) return 0;
    if ((int)grade < 0 || (int)grade >= MINING_GRADE_COUNT) return 0;
    return (int)g.station_manifest_summary[s][commodity][grade];
}

static int station_manifest_count_c(const station_t *st, commodity_t commodity)
{
    int total = 0;
    for (int gi = 0; gi < MINING_GRADE_COUNT; gi++)
        total += station_manifest_count_cg(st, commodity, (mining_grade_t)gi);
    return total;
}

static int SIGNAL_MAYBE_UNUSED
manifest_lineage_count_cg(const manifest_t *manifest,
                          commodity_t commodity,
                          mining_grade_t grade)
{
    if (!manifest || !manifest->units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (u->commodity != (uint8_t)commodity || u->grade != (uint8_t)grade) continue;
        if (u->mined_block != 0) n++;
    }
    return n;
}

static bool trade_unit_matches_cg(const cargo_unit_t *u,
                                  commodity_t commodity,
                                  mining_grade_t grade)
{
    return u && u->commodity == (uint8_t)commodity &&
           u->grade == (uint8_t)grade;
}

static bool station_ui_hash_is_zero(const uint8_t hash[32])
{
    return cargo_lineage_hash_is_zero(hash);
}

static bool cargo_unit_has_player_origin(const cargo_unit_t *u)
{
    if (!u) return false;
    if (u->recipe_id == (uint16_t)RECIPE_LEGACY_MIGRATE) return false;
    if ((cargo_kind_t)u->kind != CARGO_KIND_INGOT && u->mined_block == 0)
        return false;
    if (station_short_name((int)u->origin_station)[0] == '?') return false;
    return !station_ui_hash_is_zero(u->pub);
}

static void trade_row_attach_inspect(trade_row_t *row,
                                     const cargo_unit_t *unit,
                                     const cargo_receipt_chain_t *chain)
{
    if (!row || !unit || station_ui_hash_is_zero(unit->pub)) return;
    row->has_inspect = true;
    row->inspect_kind = unit->kind;
    row->inspect_recipe_id = unit->recipe_id;
    row->inspect_chain_len = (chain && chain->len <= CARGO_RECEIPT_CHAIN_MAX_LEN)
        ? chain->len : 0;
    memcpy(row->inspect_pub, unit->pub, sizeof(row->inspect_pub));
    memcpy(row->inspect_parent, unit->parent_merkle, sizeof(row->inspect_parent));
}

enum {
    TRADE_LINEAGE_STORY_MAX = 12,
    TRADE_LINEAGE_GAP_MAX = 6,
    TRADE_LINEAGE_PROOF_MAX = 64,
    TRADE_LINEAGE_LINE_CAP = 128,
    TRADE_LINEAGE_WALK_MAX = 12,
};

typedef struct {
    char title[96];
    char story[TRADE_LINEAGE_STORY_MAX][TRADE_LINEAGE_LINE_CAP];
    int story_count;
    char gaps[TRADE_LINEAGE_GAP_MAX][TRADE_LINEAGE_LINE_CAP];
    int gap_count;
    char current[TRADE_LINEAGE_LINE_CAP];
    char custody[TRADE_LINEAGE_LINE_CAP];
    char proof[TRADE_LINEAGE_PROOF_MAX][TRADE_LINEAGE_LINE_CAP];
    int proof_count;
} trade_lineage_view_t;

typedef struct {
    bool valid;
    uint8_t cargo_pub[32];
    int holder_station;
    uint8_t row_kind;
    bool station_pod;
    bool towed_pod;
    trade_lineage_view_t view;
} trade_lineage_cache_t;

static trade_lineage_cache_t g_trade_lineage_cache;

void trade_lineage_close(void) {
    g.trade_lineage_row = -1;
    g.trade_lineage_proof = false;
    g.trade_lineage_proof_page = 0;
    g_trade_lineage_cache.valid = false;
}

static void trade_lineage_append(char lines[][TRADE_LINEAGE_LINE_CAP],
                                 int *count, int cap, const char *fmt, ...) {
    if (!lines || !count || *count < 0 || *count >= cap || !fmt) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(lines[*count], TRADE_LINEAGE_LINE_CAP, fmt, args);
    va_end(args);
    (*count)++;
}

static void trade_lineage_story_add(trade_lineage_view_t *view,
                                    const char *fmt, ...) {
    if (!view || view->story_count >= TRADE_LINEAGE_STORY_MAX || !fmt) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(view->story[view->story_count], TRADE_LINEAGE_LINE_CAP, fmt, args);
    va_end(args);
    view->story_count++;
}

static void trade_lineage_gap_add(trade_lineage_view_t *view,
                                  const char *fmt, ...) {
    if (!view || view->gap_count >= TRADE_LINEAGE_GAP_MAX || !fmt) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(view->gaps[view->gap_count], TRADE_LINEAGE_LINE_CAP, fmt, args);
    va_end(args);
    view->gap_count++;
}

static void trade_lineage_hex(const uint8_t bytes[32], char out[65]) {
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2] = HEX[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[bytes[i] & 0x0F];
    }
    out[64] = '\0';
}

static void trade_lineage_proof_hash(trade_lineage_view_t *view,
                                     const char *label,
                                     const uint8_t bytes[32]) {
    if (!view || !label || !bytes) return;
    char hex[65];
    trade_lineage_hex(bytes, hex);
    trade_lineage_append(view->proof, &view->proof_count,
                         TRADE_LINEAGE_PROOF_MAX, "%s", label);
    trade_lineage_append(view->proof, &view->proof_count,
                         TRADE_LINEAGE_PROOF_MAX, "  %.32s", hex);
    trade_lineage_append(view->proof, &view->proof_count,
                         TRADE_LINEAGE_PROOF_MAX, "  %.32s", hex + 32);
}

static bool trade_lineage_find_transform(const uint8_t cargo_pub[32],
                                         int preferred_station,
                                         chain_cargo_transform_t *out,
                                         int *out_station) {
    if (!cargo_pub || !out) return false;
    if (preferred_station >= 0 && preferred_station < MAX_STATIONS &&
        chain_log_find_cargo_transform(&g.world.stations[preferred_station],
                                       cargo_pub, out)) {
        if (out_station) *out_station = preferred_station;
        return true;
    }
    for (int si = 0; si < MAX_STATIONS; si++) {
        if (si == preferred_station) continue;
        if (chain_log_find_cargo_transform(&g.world.stations[si],
                                           cargo_pub, out)) {
            if (out_station) *out_station = si;
            return true;
        }
    }
    return false;
}

static bool trade_lineage_seen(const uint8_t seen[][32], int seen_count,
                               const uint8_t cargo_pub[32]) {
    for (int i = 0; i < seen_count; i++)
        if (memcmp(seen[i], cargo_pub, 32) == 0) return true;
    return false;
}

static void trade_lineage_walk(trade_lineage_view_t *view,
                               const uint8_t cargo_pub[32],
                               commodity_t commodity_hint,
                               int preferred_station,
                               uint8_t seen[][32], int *seen_count,
                               int depth) {
    if (!view || !cargo_pub || !seen || !seen_count) return;
    if (depth >= 5 || *seen_count >= TRADE_LINEAGE_WALK_MAX) {
        trade_lineage_gap_add(view,
                              "Gap: earlier transformations exceed the local view.");
        return;
    }
    if (trade_lineage_seen((const uint8_t (*)[32])seen, *seen_count,
                           cargo_pub)) {
        trade_lineage_gap_add(view, "Gap: repeated parent link stopped safely.");
        return;
    }
    memcpy(seen[*seen_count], cargo_pub, 32);
    (*seen_count)++;

    chain_cargo_transform_t transform;
    int station_idx = -1;
    if (!trade_lineage_find_transform(cargo_pub, preferred_station,
                                      &transform, &station_idx)) {
        char call[8];
        mining_callsign_from_pubkey(cargo_pub, call);
        trade_lineage_gap_add(view,
                              "Gap: %s event for %s is not in local station history.",
                              commodity_short_name(commodity_hint), call);
        trade_lineage_proof_hash(view, "Missing transform output ID", cargo_pub);
        return;
    }

    const char *station = station_short_name(station_idx);
    if (transform.type == CHAIN_EVT_SMELT) {
        char fragment[8];
        mining_callsign_from_pubkey(transform.smelt.fragment_pub, fragment);
        trade_lineage_story_add(view, "Fragment %s -> %s at %s",
                                fragment, commodity_short_name(commodity_hint),
                                station);
        trade_lineage_append(view->proof, &view->proof_count,
                             TRADE_LINEAGE_PROOF_MAX,
                             "SMELT event %llu @ %s / epoch %llu",
                             (unsigned long long)transform.event_id, station,
                             (unsigned long long)transform.epoch);
        trade_lineage_proof_hash(view, "SMELT output ID",
                                 transform.smelt.ingot_pub);
        trade_lineage_proof_hash(view, "Fragment ID",
                                 transform.smelt.fragment_pub);
        return;
    }

    if (transform.type != CHAIN_EVT_CRAFT) return;
    if (transform.craft_provenance.status !=
        CARGO_CRAFT_PROVENANCE_STATION_ATTESTED_V1) {
        trade_lineage_gap_add(
            view,
            "Gap: CRAFT event rejected (%s).",
            cargo_craft_provenance_status_name(
                transform.craft_provenance.status));
        return;
    }
    const recipe_def_t *recipe = recipe_get((recipe_id_t)transform.craft.recipe_id);
    int input_count = transform.craft.input_count;
    if (input_count > RECIPE_INPUT_MAX) input_count = RECIPE_INPUT_MAX;
    for (int i = 0; i < input_count; i++) {
        commodity_t input_commodity = COMMODITY_COUNT;
        if (recipe && i < recipe->input_count)
            input_commodity = recipe->input_commodities[i];
        trade_lineage_walk(view, transform.craft.input_pubs[i],
                           input_commodity, -1, seen, seen_count, depth + 1);
    }

    char inputs[80] = "";
    if (recipe && recipe->input_count > 0) {
        for (int i = 0; i < recipe->input_count && i < RECIPE_INPUT_MAX; i++) {
            if (i > 0) station_ui_append_text(inputs, sizeof(inputs), " + ");
            station_ui_append_text(inputs, sizeof(inputs),
                                   commodity_short_name(recipe->input_commodities[i]));
        }
    } else {
        snprintf(inputs, sizeof(inputs), "signed inputs");
    }
    trade_lineage_story_add(view, "Station-attested V1: %s -> %s at %s", inputs,
                            commodity_short_name(commodity_hint), station);
    trade_lineage_append(view->proof, &view->proof_count,
                         TRADE_LINEAGE_PROOF_MAX,
                         "CRAFT event %llu @ %s / epoch %llu / recipe %u",
                         (unsigned long long)transform.event_id, station,
                         (unsigned long long)transform.epoch,
                         (unsigned)transform.craft.recipe_id);
    trade_lineage_append(
        view->proof, &view->proof_count,
        TRADE_LINEAGE_PROOF_MAX,
        "CRAFT provenance %s / input_lineage_proven=false / "
        "conservation_proven=false",
        cargo_craft_provenance_status_name(
            transform.craft_provenance.status));
    trade_lineage_proof_hash(view, "CRAFT output ID",
                             transform.craft.output_pub);
    for (int i = 0; i < input_count; i++) {
        char label[48];
        snprintf(label, sizeof(label), "CRAFT input %d ID", i + 1);
        trade_lineage_proof_hash(view, label, transform.craft.input_pubs[i]);
    }
}

static void trade_lineage_unit_from_row(const trade_row_t *row,
                                        cargo_unit_t *unit) {
    if (!unit) return;
    memset(unit, 0, sizeof(*unit));
    if (!row) return;
    unit->kind = row->inspect_kind;
    unit->commodity = (uint8_t)row->commodity;
    unit->grade = (uint8_t)row->grade;
    unit->recipe_id = row->inspect_recipe_id;
    unit->origin_station = row->origin_station_idx;
    unit->quantity = 1;
    unit->mined_block = row->mined_block;
    memcpy(unit->pub, row->inspect_pub, sizeof(unit->pub));
    memcpy(unit->parent_merkle, row->inspect_parent,
           sizeof(unit->parent_merkle));
}

static void trade_lineage_build_view(const station_t *holder,
                                     const trade_row_t *row,
                                     trade_lineage_view_t *view) {
    memset(view, 0, sizeof(*view));
    cargo_unit_t unit;
    trade_lineage_unit_from_row(row, &unit);

    char serial[12];
    cargo_lineage_serial_label(&unit, serial, sizeof(serial));
    snprintf(view->title, sizeof(view->title), "%s %s",
             commodity_name(row->commodity), serial);

    uint8_t seen[TRADE_LINEAGE_WALK_MAX][32] = {{0}};
    int seen_count = 0;
    trade_lineage_walk(view, unit.pub, row->commodity,
                       (int)unit.origin_station, seen, &seen_count, 0);

    if (view->story_count == 0) {
        char manifest_story[96];
        cargo_lineage_story_label(&unit, manifest_story,
                                  sizeof(manifest_story));
        trade_lineage_story_add(view, "Manifest: %s", manifest_story);
    }

    const char *holder_name = holder
        ? station_short_name(station_index_of(holder)) : "this dock";
    if (row->is_station_pod) {
        snprintf(view->current, sizeof(view->current),
                 "Now: %s crate for sale at %s dock",
                 commodity_short_name(row->commodity), holder_name);
    } else if (row->is_towed_pod) {
        snprintf(view->current, sizeof(view->current),
                 "Now: %s crate towed by you",
                 commodity_short_name(row->commodity));
    } else {
        snprintf(view->current, sizeof(view->current),
                 "Now: %s held by you",
                 commodity_short_name(row->commodity));
    }

    if (row->inspect_chain_len > 0) {
        snprintf(view->custody, sizeof(view->custody),
                 "Custody: %u signed handoff%s attached",
                 (unsigned)row->inspect_chain_len,
                 row->inspect_chain_len == 1 ? "" : "s");
    } else {
        snprintf(view->custody, sizeof(view->custody),
                 "Custody gap: portable receipt links are not local here");
    }

    trade_lineage_proof_hash(view, "Selected cargo ID", unit.pub);
    if (!station_ui_hash_is_zero(unit.parent_merkle))
        trade_lineage_proof_hash(view, "Manifest parent root",
                                 unit.parent_merkle);
    else
        trade_lineage_append(view->proof, &view->proof_count,
                             TRADE_LINEAGE_PROOF_MAX,
                             "Manifest parent root: none (legacy or origin gap)");
    trade_lineage_append(view->proof, &view->proof_count,
                         TRADE_LINEAGE_PROOF_MAX,
                         "Manifest recipe %u (%s) / origin station %u",
                         (unsigned)unit.recipe_id,
                         cargo_lineage_recipe_label(&unit),
                         (unsigned)unit.origin_station);
    trade_lineage_append(view->proof, &view->proof_count,
                         TRADE_LINEAGE_PROOF_MAX,
                         "Mint epoch %llu / local receipt seals %u",
                         (unsigned long long)unit.mined_block,
                         (unsigned)row->inspect_chain_len);
}

static const trade_lineage_view_t *trade_lineage_view_for_row(
    const station_t *holder, const trade_row_t *row) {
    if (!holder || !row || !row->has_inspect) return NULL;
    int holder_idx = station_index_of(holder);
    if (g_trade_lineage_cache.valid &&
        g_trade_lineage_cache.holder_station == holder_idx &&
        g_trade_lineage_cache.row_kind == row->kind &&
        g_trade_lineage_cache.station_pod == row->is_station_pod &&
        g_trade_lineage_cache.towed_pod == row->is_towed_pod &&
        memcmp(g_trade_lineage_cache.cargo_pub, row->inspect_pub, 32) == 0) {
        return &g_trade_lineage_cache.view;
    }
    memset(&g_trade_lineage_cache, 0, sizeof(g_trade_lineage_cache));
    g_trade_lineage_cache.valid = true;
    g_trade_lineage_cache.holder_station = holder_idx;
    g_trade_lineage_cache.row_kind = row->kind;
    g_trade_lineage_cache.station_pod = row->is_station_pod;
    g_trade_lineage_cache.towed_pod = row->is_towed_pod;
    memcpy(g_trade_lineage_cache.cargo_pub, row->inspect_pub, 32);
    trade_lineage_build_view(holder, row, &g_trade_lineage_cache.view);
    return &g_trade_lineage_cache.view;
}

/* station_manifest_has_commodity / ship_manifest_has_commodity removed —
 * after the manifest-first TRADE rewrite the rows always probe the
 * full grade range directly, so the "any-grade?" predicate is no
 * longer needed. */

/* Ship manifest helpers — iterate directly. In network-authoritative sessions,
 * net_sync.c rebuilds the local read model from one atomic PLAYER_MANIFEST
 * summary/detail packet, so the trade UI can use the same path in every mode. */
static int SIGNAL_MAYBE_UNUSED
ship_manifest_count_cg(const ship_t *ship,
                       commodity_t commodity,
                       mining_grade_t grade)
{
    if (!ship || !ship->manifest.units) return 0;
    int n = 0;
    for (uint16_t i = 0; i < ship->manifest.count; i++) {
        const cargo_unit_t *u = &ship->manifest.units[i];
        if (u->commodity == (uint8_t)commodity && u->grade == (uint8_t)grade) n++;
    }
    return n;
}

static int ship_manifest_count_c(const ship_t *ship, commodity_t commodity)
{
    return manifest_count_by_commodity(ship ? &ship->manifest : NULL, commodity);
}

static float ship_manifest_backed_cargo_volume(const ship_t *ship)
{
    if (!ship) return 0.0f;
    float total = 0.0f;
    for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++)
        total += ship->cargo[c] * commodity_volume((commodity_t)c);
    if (ship->manifest.units) {
        for (uint16_t u = 0; u < ship->manifest.count; u++) {
            const cargo_unit_t *cu = &ship->manifest.units[u];
            if (cu->commodity >= COMMODITY_COUNT) continue;
            total += commodity_volume((commodity_t)cu->commodity);
        }
    }
    return total;
}

static bool local_ship_lists_towed_pod(const ship_t *ship, int pod_idx)
{
    if (!ship || pod_idx < 0) return false;
    for (int i = 0; i < ship->towed_pod_count && i < 10; i++) {
        if (ship->towed_pods[i] == pod_idx) return true;
    }
    return false;
}

static bool local_player_tows_pod(const ship_t *ship, int pod_idx)
{
    if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) return false;
    const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
    if (!pod->active || pod->quantity == 0 ||
        pod->commodity >= COMMODITY_COUNT) {
        return false;
    }
    if (cargo_pod_player_tractor(pod) == LOCAL_PLAYER.id) return true;
    return local_ship_lists_towed_pod(ship, pod_idx);
}

static int station_index_local(const station_t *st)
{
    if (!st) return -1;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (st == &g.world.stations[i]) return i;
    }
    return -1;
}

static bool station_shipyard_pod_staged_at_hopper_local(const station_t *st,
                                                        const cargo_pod_t *pod,
                                                        commodity_t commodity)
{
    if (!st || !pod || commodity >= COMMODITY_COUNT) return false;
    int station_idx = station_index_local(st);
    const float hopper_range_sq =
        HOPPER_INTAKE_STAGING_RANGE * HOPPER_INTAKE_STAGING_RANGE;
    const float yard_range_sq = HOPPER_PULL_RANGE * HOPPER_PULL_RANGE;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold || hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity != commodity) continue;
        vec2 hopper_pos = module_world_pos_ring(st, hopper->ring,
                                                hopper->slot);
        bool serves_yard = false;
        for (int y = 0; y < st->module_count; y++) {
            const station_module_t *yard = &st->modules[y];
            if (yard->scaffold || yard->type != MODULE_SHIPYARD) continue;
            module_inputs_t req = module_required_inputs(MODULE_SHIPYARD);
            bool accepts = false;
            for (int r = 0; r < req.count; r++) {
                if (req.commodities[r] == commodity) {
                    accepts = true;
                    break;
                }
            }
            if (!accepts) continue;
            vec2 yard_pos = module_world_pos_ring(st, yard->ring,
                                                  yard->slot);
            if (v2_dist_sq(hopper_pos, yard_pos) <= yard_range_sq) {
                serves_yard = true;
                break;
            }
        }
        if (!serves_yard) continue;
        if (station_idx >= 0 &&
            cargo_pod_is_tractored_by_module(pod, station_idx, i))
            return true;
        if (cargo_pod_has_player_tractor(pod) &&
            v2_dist_sq(pod->pos, hopper_pos) <= hopper_range_sq)
            return true;
    }
    return false;
}

static bool station_shipyard_pod_is_exact_material_local(const cargo_pod_t *pod,
                                                        commodity_t commodity)
{
    if (!pod || commodity >= COMMODITY_COUNT) return false;
    if (!pod->active || pod->kind != CARGO_POD_CARGO) return false;
    if (pod->shipment_id != 0 || pod->commodity != commodity) return false;
    if (pod->quantity == 0) return false;
    if (pod->summary_flags & CARGO_POD_SUMMARY_EXACT_MATERIAL)
        return pod->manifest_count == pod->quantity;
    if (pod->manifest_count == 0 ||
        pod->manifest_count != pod->quantity ||
        pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
        return false;
    }
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if ((commodity_t)pod->manifest_units[i].commodity != commodity)
            return false;
    }
    return true;
}

static int station_shipyard_station_material_available_local(const station_t *st,
                                                             commodity_t commodity)
{
    if (!st || commodity >= COMMODITY_COUNT) return 0;
    int stored = station_finished_count(st, commodity);
    if (stored <= 0) return 0;
    for (int i = 0; i < st->module_count; i++) {
        const station_module_t *hopper = &st->modules[i];
        if (hopper->scaffold || hopper->type != MODULE_HOPPER) continue;
        if ((commodity_t)hopper->commodity == commodity) return stored;
    }
    return 0;
}

int station_shipyard_material_available_local(const station_t *st,
                                              commodity_t commodity)
{
    if (!st || commodity >= COMMODITY_COUNT) return 0;
    int total =
        station_shipyard_station_material_available_local(st, commodity);
    const ship_t *ship = LOCAL_PLAYER.ship;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        const cargo_pod_t *pod = &g.world.cargo_pods[i];
        if (!station_shipyard_pod_is_exact_material_local(pod, commodity))
            continue;
        if (cargo_pod_has_player_tractor(pod) &&
            !local_player_tows_pod(ship, i))
            continue;
        if (!station_shipyard_pod_staged_at_hopper_local(st, pod, commodity))
            continue;
        total += (int)pod->quantity;
    }
    return total;
}

bool station_shipyard_can_commission_hull_local(const station_t *st,
                                                hull_class_t hull_class)
{
    if (!st || station_active_shipyard_count(st) < 1) return false;
    int frames = 0, lasers = 0, tractors = 0;
    if (!shipyard_hull_cost(hull_class, &frames, &lasers, &tractors))
        return false;
    return station_shipyard_material_available_local(st, COMMODITY_FRAME) >= frames &&
           station_shipyard_material_available_local(st, COMMODITY_LASER_MODULE) >= lasers &&
           station_shipyard_material_available_local(st, COMMODITY_TRACTOR_MODULE) >= tractors;
}

static bool trade_is_finished_good(commodity_t c)
{
    return c >= COMMODITY_RAW_ORE_COUNT && c < COMMODITY_COUNT;
}

static bool local_pod_has_exact_commodity_manifest(const cargo_pod_t *pod)
{
    if (!pod || pod->manifest_count == 0 ||
        pod->manifest_count != pod->quantity ||
        pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
        return false;
    }
    if (pod->summary_flags & CARGO_POD_SUMMARY_EXACT_MATERIAL)
        return true;
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if ((commodity_t)pod->manifest_units[i].commodity != pod->commodity)
            return false;
    }
    return true;
}

static bool local_pod_has_detailed_manifest(const cargo_pod_t *pod)
{
    if (!pod || pod->manifest_count == 0 ||
        pod->manifest_count != pod->quantity ||
        pod->manifest_count > CARGO_POD_MANIFEST_CAP) {
        return false;
    }
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if ((commodity_t)pod->manifest_units[i].commodity != pod->commodity)
            return false;
    }
    return true;
}

static bool local_pod_has_selection_token(const cargo_pod_t *pod)
{
    if (!pod) return false;
    for (size_t i = 0; i < sizeof(pod->selection_token); i++)
        if (pod->selection_token[i] != 0) return true;
    return false;
}

static mining_grade_t local_pod_summary_grade(const cargo_pod_t *pod)
{
    if (!pod || pod->summary_grade >= (uint8_t)MINING_GRADE_COUNT)
        return MINING_GRADE_COMMON;
    return (mining_grade_t)pod->summary_grade;
}

static float trade_unit_price_with_summary_grade(float unit_price,
                                                 const cargo_pod_t *pod)
{
    if (unit_price <= FLOAT_EPSILON) return 0.0f;
    return unit_price * mining_payout_multiplier(local_pod_summary_grade(pod));
}

static bool trade_cargo_pod_fits_contract_exact(const cargo_pod_t *pod,
                                                const contract_t *ct)
{
    if (!pod || !ct || !ct->active) return false;
    if (ct->action != CONTRACT_TRACTOR) return false;
    if (ct->commodity < COMMODITY_RAW_ORE_COUNT) return false;
    if (pod->shipment_id != 0 || pod->commodity != ct->commodity)
        return false;
    if (!local_pod_has_exact_commodity_manifest(pod)) return false;
    int needed = (int)floorf(ct->quantity_needed + 0.0001f);
    if (needed < (int)pod->quantity) return false;
    if (!local_pod_has_detailed_manifest(pod)) {
        if (ct->proof_flags != 0 || ct->required_recipe_id != 0)
            return false;
        if (ct->required_grade < MINING_GRADE_COUNT &&
            local_pod_summary_grade(pod) < (mining_grade_t)ct->required_grade) {
            return false;
        }
        return true;
    }
    for (uint16_t i = 0; i < pod->manifest_count; i++) {
        if (!contract_fit_is_ok(contract_fit_cargo_unit(
                ct, &pod->manifest_units[i]))) {
            return false;
        }
    }
    return true;
}

static const contract_t *trade_matching_pod_contract(const station_t *st,
                                                     const cargo_pod_t *pod)
{
    int station_idx = station_index_of(st);
    if (station_idx < 0) return NULL;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &g.world.contracts[k];
        if (!ct->active || ct->station_index != station_idx) continue;
        if (trade_cargo_pod_fits_contract_exact(pod, ct)) return ct;
    }
    return NULL;
}

static float trade_black_market_pod_quote(const station_t *st,
                                          const cargo_pod_t *pod)
{
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->quantity == 0 || pod->commodity >= COMMODITY_COUNT ||
        !station_policy_accepts_contract_bound_cargo(st)) {
        return 0.0f;
    }

    commodity_t c = pod->commodity;
    float value = 0.0f;
    if (pod->manifest_count > 0) {
        if (!local_pod_has_exact_commodity_manifest(pod)) return 0.0f;
        if (local_pod_has_detailed_manifest(pod)) {
            for (uint16_t i = 0; i < pod->manifest_count; i++) {
                const cargo_unit_t *unit = &pod->manifest_units[i];
                float unit_value = station_buy_price_unit(st, unit);
                if (unit_value <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON) {
                    unit_value = st->base_price[c] *
                        prefix_class_price_multiplier((int)unit->prefix_class);
                }
                unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
                value += unit_value;
            }
        } else {
            float unit_value = station_buy_price(st, c);
            if (unit_value <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
                unit_value = st->base_price[c];
            value = trade_unit_price_with_summary_grade(unit_value, pod) *
                    (float)pod->quantity;
        }
    } else {
        float unit_value = station_buy_price(st, c);
        if (unit_value <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
            unit_value = st->base_price[c];
        value = unit_value * (float)pod->quantity;
    }

    return value > FLOAT_EPSILON
        ? value * BLACK_MARKET_CARGO_MARKDOWN
        : 0.0f;
}

static float trade_towed_pod_quote(const station_t *st,
                                   const cargo_pod_t *pod,
                                   bool *out_actionable,
                                   uint8_t *out_block_reason)
{
    if (out_actionable) *out_actionable = false;
    if (out_block_reason) *out_block_reason = TRADE_BLOCK_NONE;
    if (!st || !pod || !pod->active || pod->commodity >= COMMODITY_COUNT ||
        pod->quantity == 0) {
        if (out_block_reason) *out_block_reason = TRADE_BLOCK_NO_CARGO;
        return 0.0f;
    }

    commodity_t c = pod->commodity;
    int quantity = (int)pod->quantity;
    if (pod->shipment_id != 0) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_shipment(pod->shipment_id);
        int here_idx = station_index_of(st);
        bool at_dest = ledger && here_idx >= 0 &&
                       here_idx == (int)ledger->destination_station;
        bool black_market = station_policy_accepts_contract_bound_cargo(st);
        bool accepted = at_dest || black_market;
        if (!accepted) {
            if (out_block_reason) *out_block_reason = TRADE_BLOCK_NO_BUYER;
            return 0.0f;
        }
        float value = 0.0f;
        if (at_dest && ledger && ledger->quantity_total > 0) {
            value = (ledger->destination_payout /
                     (float)ledger->quantity_total) * (float)quantity;
        } else {
            float unit_price = station_buy_price(st, c);
            if (unit_price <= FLOAT_EPSILON &&
                st->base_price[c] > FLOAT_EPSILON) {
                unit_price = st->base_price[c];
            }
            value = unit_price * BLACK_MARKET_CARGO_MARKDOWN * (float)quantity;
        }
        if (value <= FLOAT_EPSILON) {
            if (out_block_reason) *out_block_reason = TRADE_BLOCK_NO_BUYER;
            return 0.0f;
        }
        if (out_actionable) *out_actionable = true;
        return value;
    }

    const contract_t *ct = trade_matching_pod_contract(st, pod);
    if (!ct && station_policy_accepts_contract_bound_cargo(st)) {
        float value = trade_black_market_pod_quote(st, pod);
        if (value > FLOAT_EPSILON) {
            if (out_actionable) *out_actionable = true;
            return value;
        }
    }
    float price = ct ? contract_price(ct) : station_buy_price(st, c);
    if (price <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
        price = st->base_price[c];
    if (price <= FLOAT_EPSILON) {
        if (out_block_reason) *out_block_reason = TRADE_BLOCK_NO_BUYER;
        return 0.0f;
    }

    float value = 0.0f;
    if (pod->manifest_count > 0) {
        if (!local_pod_has_exact_commodity_manifest(pod) ||
            !trade_is_finished_good(c)) {
            if (out_block_reason) *out_block_reason = TRADE_BLOCK_NO_BUYER;
            return 0.0f;
        }
        if (local_pod_has_detailed_manifest(pod)) {
            for (uint16_t i = 0; i < pod->manifest_count; i++) {
                const cargo_unit_t *unit = &pod->manifest_units[i];
                float unit_value = ct ? price : station_buy_price_unit(st, unit);
                unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
                value += unit_value;
            }
        } else {
            float unit_value = ct ? price : station_buy_price(st, c);
            if (unit_value <= FLOAT_EPSILON &&
                st->base_price[c] > FLOAT_EPSILON) {
                unit_value = st->base_price[c];
            }
            value = trade_unit_price_with_summary_grade(unit_value, pod) *
                    (float)quantity;
        }
    } else {
        value = price * (float)quantity;
    }
    if (value <= FLOAT_EPSILON) {
        if (out_block_reason) *out_block_reason = TRADE_BLOCK_NO_BUYER;
        return 0.0f;
    }
    if (out_actionable) *out_actionable = true;
    return value;
}

static bool trade_station_market_pod(const station_t *st, int station_idx,
                                     int pod_idx) {
    if (!st || station_idx < 0 || pod_idx < 0 ||
        pod_idx >= MAX_CARGO_PODS) {
        return false;
    }
    const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
    if (!pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->quantity == 0 || pod->commodity >= COMMODITY_COUNT ||
        cargo_pod_has_player_tractor(pod) || pod->shipment_id != 0) {
        return false;
    }
    int ps = -1;
    int pm = -1;
    if (!cargo_pod_module_tractor_indices(pod, &ps, &pm) ||
        ps != station_idx || pm < 0 || pm >= st->module_count ||
        pm >= MAX_MODULES_PER_STATION) {
        return false;
    }
    const station_module_t *module = &st->modules[pm];
    return !module->scaffold && module->type == MODULE_DOCK;
}

static float trade_station_pod_quote(const station_t *st,
                                     const cargo_pod_t *pod) {
    if (!st || !pod || !pod->active || pod->kind != CARGO_POD_CARGO ||
        pod->quantity == 0 || pod->commodity >= COMMODITY_COUNT ||
        pod->shipment_id != 0) {
        return 0.0f;
    }
    commodity_t c = pod->commodity;
    float price = station_sell_price(st, c);
    if (price <= FLOAT_EPSILON && st->base_price[c] > FLOAT_EPSILON)
        price = st->base_price[c];
    if (price <= FLOAT_EPSILON) return 0.0f;

    if (pod->manifest_count > 0) {
        if (!local_pod_has_exact_commodity_manifest(pod) ||
            !trade_is_finished_good(c)) {
            return 0.0f;
        }
        if (local_pod_has_detailed_manifest(pod)) {
            float value = 0.0f;
            for (uint16_t i = 0; i < pod->manifest_count; i++) {
                const cargo_unit_t *unit = &pod->manifest_units[i];
                float unit_value = station_sell_price_unit(st, unit);
                unit_value *= mining_payout_multiplier((mining_grade_t)unit->grade);
                value += unit_value;
            }
            return value;
        }
        return trade_unit_price_with_summary_grade(price, pod) *
               (float)pod->quantity;
    }
    return price * (float)pod->quantity;
}

static mining_grade_t trade_pod_display_grade(const cargo_pod_t *pod) {
    if (!pod || pod->manifest_count == 0)
        return MINING_GRADE_COUNT;
    if (!local_pod_has_detailed_manifest(pod))
        return local_pod_summary_grade(pod);
    mining_grade_t grade = (mining_grade_t)pod->manifest_units[0].grade;
    for (uint16_t i = 1; i < pod->manifest_count; i++) {
        if ((mining_grade_t)pod->manifest_units[i].grade != grade)
            return MINING_GRADE_COUNT;
    }
    return grade;
}

static int trade_station_market_pod_units(const station_t *st,
                                          int station_idx,
                                          commodity_t commodity,
                                          mining_grade_t grade)
{
    if (!st || station_idx < 0 || commodity >= COMMODITY_COUNT)
        return 0;
    int total = 0;
    for (int pod_idx = 0; pod_idx < MAX_CARGO_PODS; pod_idx++) {
        if (!trade_station_market_pod(st, station_idx, pod_idx))
            continue;
        const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
        if (pod->commodity != commodity) continue;
        if (grade < MINING_GRADE_COUNT) {
            mining_grade_t pod_grade = trade_pod_display_grade(pod);
            if (pod_grade != grade) continue;
        }
        total += (int)pod->quantity;
    }
    return total;
}

static bool trade_station_has_matching_intake(const station_t *st,
                                             const cargo_pod_t *pod)
{
    if (!st || !pod || pod->commodity >= COMMODITY_COUNT ||
        !local_pod_has_exact_commodity_manifest(pod)) {
        return false;
    }
    commodity_t c = pod->commodity;
    if (station_find_hopper_for(st, c) >= 0)
        return true;
    if (c == COMMODITY_FRAME) {
        for (int m = 0; m < st->module_count && m < MAX_MODULES_PER_STATION; m++) {
            const station_module_t *module = &st->modules[m];
            if (module->scaffold) continue;
            if (module->type == MODULE_FURNACE)
                return true;
            const module_schema_t *schema = module_schema(module->type);
            if (schema && schema->kind == MODULE_KIND_PRODUCER)
                return true;
        }
    }
    return false;
}

typedef struct {
    int towed_crates;
    int towed_units;
    int station_crates;
    int station_units;
    int accepted_crates;
    int accepted_units;
    int exact_contracts;
    int exact_ready_crates;
    bool black_market;
} trade_custody_board_t;

static void trade_custody_board_build(const station_t *st,
                                      const ship_t *ship,
                                      trade_custody_board_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    int station_idx = station_index_of(st);
    if (!st || !ship || station_idx < 0) return;
    out->black_market = station_policy_accepts_contract_bound_cargo(st);

    for (int pod_idx = 0; pod_idx < MAX_CARGO_PODS; pod_idx++) {
        const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
        if (!pod->active || pod->kind != CARGO_POD_CARGO ||
            pod->quantity == 0 || pod->commodity >= COMMODITY_COUNT) {
            continue;
        }
        if (local_player_tows_pod(ship, pod_idx)) {
            bool actionable = false;
            uint8_t blk = TRADE_BLOCK_NONE;
            out->towed_crates++;
            out->towed_units += (int)pod->quantity;
            (void)trade_towed_pod_quote(st, pod, &actionable, &blk);
            if (actionable && trade_station_has_matching_intake(st, pod)) {
                out->accepted_crates++;
                out->accepted_units += (int)pod->quantity;
            }
        }
        if (trade_station_market_pod(st, station_idx, pod_idx) &&
            trade_station_pod_quote(st, pod) > FLOAT_EPSILON) {
            out->station_crates++;
            out->station_units += (int)pod->quantity;
        }
    }

    for (int k = 0; k < MAX_CONTRACTS; k++) {
        const contract_t *ct = &g.world.contracts[k];
        if (!ct->active || ct->station_index != station_idx ||
            ct->action != CONTRACT_TRACTOR ||
            ct->commodity < COMMODITY_RAW_ORE_COUNT ||
            ct->quantity_needed <= 0.01f) {
            continue;
        }
        out->exact_contracts++;
        for (int t = 0; t < ship->towed_pod_count && t < 10; t++) {
            int pod_idx = ship->towed_pods[t];
            if (pod_idx < 0 || pod_idx >= MAX_CARGO_PODS) continue;
            const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
            if (trade_cargo_pod_fits_contract_exact(pod, ct)) {
                out->exact_ready_crates++;
                break;
            }
        }
    }
}

static float local_towed_cargo_volume(const ship_t *ship)
{
    float total = 0.0f;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!local_player_tows_pod(ship, i)) continue;
        const cargo_pod_t *pod = &g.world.cargo_pods[i];
        total += (float)pod->quantity *
                 commodity_volume((commodity_t)pod->commodity);
    }
    return total;
}

static void local_towed_cargo_volume_by_commodity(const ship_t *ship,
                                                  float out[COMMODITY_COUNT])
{
    if (!out) return;
    for (int c = 0; c < COMMODITY_COUNT; c++) out[c] = 0.0f;
    for (int i = 0; i < MAX_CARGO_PODS; i++) {
        if (!local_player_tows_pod(ship, i)) continue;
        const cargo_pod_t *pod = &g.world.cargo_pods[i];
        out[pod->commodity] += (float)pod->quantity *
                               commodity_volume((commodity_t)pod->commodity);
    }
}

/* Representative SELL unit for a (commodity, grade) row. The server's
 * per-row sell path picks the highest prefix multiplier in the bucket,
 * so the UI quotes that same unit instead of an arbitrary FIFO unit. */
static int SIGNAL_MAYBE_UNUSED
manifest_find_top_sell_unit_cg(const manifest_t *manifest,
                               commodity_t commodity,
                               mining_grade_t grade)
{
    if (!manifest || !manifest->units) return -1;
    int top_idx = -1;
    float top_mult = 1.0f;
    for (uint16_t i = 0; i < manifest->count; i++) {
        const cargo_unit_t *u = &manifest->units[i];
        if (!trade_unit_matches_cg(u, commodity, grade)) continue;
        float m = prefix_class_price_multiplier((int)u->prefix_class);
        if (top_idx < 0 || m > top_mult) {
            top_mult = m;
            top_idx = (int)i;
        }
    }
    return top_idx;
}

/* TRADE view — market. Unified list of rows; BUY rows (what the station
 * sells) first, then SELL rows (what the station buys). Hotkeys [1]..[5]
 * select a row on the current page. [F] pages forward; pages wrap at
 * the last page so the UI never runs out. At-station trades show the
 * generic "$" symbol — contract payouts in CONTRACTS still name the issuing
 * station's currency because that's where the money actually lives.
 *
 * trade_row_t and the pagination constants live in client.h so input.c
 * can index into the same row list — see build_trade_rows below. */

int build_trade_rows(const station_t *st, const ship_t *ship,
                     trade_row_t out[], int max) {
    if (!st || !ship || !out || max <= 0) return 0;
    int station_idx = trade_session_station_index(st);
    if (station_idx < 0) return 0;
    int row_count = 0;
    float credits = player_current_balance();
    int tow_space = ship_tow_body_space(ship);

    /* BUY rows backed by physical dock-held pods. These are the crate
     * economy's primary market surface: buying transfers custody of the
     * whole visible pod instead of minting a new pod from station stock. */
    for (int pod_idx = 0; pod_idx < MAX_CARGO_PODS && row_count < max; pod_idx++) {
        if (!trade_station_market_pod(st, station_idx, pod_idx)) continue;
        const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
        float quote = trade_station_pod_quote(st, pod);
        if (quote <= FLOAT_EPSILON) continue;
        int quantity = (int)pod->quantity;
        int total_price = (int)lroundf(quote);
        int unit_price = quantity > 0
            ? (int)lroundf(quote / (float)quantity)
            : total_price;
        uint8_t blk = TRADE_BLOCK_NONE;
        if (tow_space <= 0) blk = TRADE_BLOCK_TOW_FULL;
        else if (credits + FLOAT_EPSILON < quote) blk = TRADE_BLOCK_NO_FUNDS;

        uint8_t origin_idx = 0;
        uint64_t mined_blk = 0;
        bool has_lineage = false;
        const cargo_unit_t *inspect_unit = NULL;
        mining_grade_t row_grade = trade_pod_display_grade(pod);
        if (pod->manifest_count > 0) {
            inspect_unit = &pod->manifest_units[0];
            origin_idx = inspect_unit->origin_station;
            mined_blk = inspect_unit->mined_block;
            has_lineage = (mined_blk != 0) ||
                          cargo_unit_has_player_origin(inspect_unit);
        }

        trade_row_t row = (trade_row_t){
            .kind = 0, .commodity = pod->commodity, .grade = row_grade,
            .stock = quantity, .quantity = quantity,
            .unit_price = unit_price, .total_price = total_price,
            .actionable = (blk == TRADE_BLOCK_NONE),
            .station_stock = quantity,
            .station_capacity = CARGO_POD_UNIT_CAPACITY,
            .held = 0, .block_reason = blk,
            .towed_pod_quantity = quantity,
            .is_station_pod = true,
            .station_pod_index = (uint16_t)pod_idx,
            .prefix_class = inspect_unit
                ? inspect_unit->prefix_class
                : (uint8_t)INGOT_PREFIX_ANONYMOUS,
            .has_lineage = has_lineage,
            .origin_station_idx = origin_idx,
            .mined_block = mined_blk,
        };
        trade_row_attach_inspect(&row, inspect_unit, NULL);
        out[row_count++] = row;
    }

    /* Player-towed rows. Ordinary source-local pods become PRESENT/UNPACK
     * rows; shipment-bound pods retain the SELL/DELIVER path. */
    int tow_count = ship->towed_pod_count;
    if (tow_count > 10) tow_count = 10;
    for (int t = tow_count - 1; t >= 0 && row_count < max; t--) {
        int pod_idx = ship->towed_pods[t];
        if (!local_player_tows_pod(ship, pod_idx)) continue;
        const cargo_pod_t *pod = &g.world.cargo_pods[pod_idx];
        if (!pod->active || pod->kind != CARGO_POD_CARGO ||
            pod->commodity >= COMMODITY_COUNT || pod->quantity == 0) {
            continue;
        }
        commodity_t c = pod->commodity;
        if (pod->shipment_id == 0) {
            bool exact = local_pod_has_exact_commodity_manifest(pod);
            bool token_ready = local_pod_has_selection_token(pod);
            bool custody_here =
                pod->custody_station == 0 ||
                pod->custody_station ==
                    (uint8_t)(station_idx + 1);
            uint8_t blk = exact && token_ready && custody_here
                ? TRADE_BLOCK_NONE
                : TRADE_BLOCK_NO_RECEIPT_SOURCE;
            int quantity = (int)pod->quantity;
            trade_row_t row = (trade_row_t){
                .kind = 2,
                .commodity = c,
                .grade = trade_pod_display_grade(pod),
                .stock = quantity,
                .quantity = quantity,
                .actionable = blk == TRADE_BLOCK_NONE,
                .station_stock = 0,
                .station_capacity = CARGO_POD_UNIT_CAPACITY,
                .held = quantity,
                .towed_held = quantity,
                .towed_pod_quantity = quantity,
                .is_towed_pod = true,
                .towed_pod_index = (uint16_t)pod_idx,
                .block_reason = blk,
                .prefix_class =
                    (uint8_t)INGOT_PREFIX_ANONYMOUS,
            };
            memcpy(row.pod_selection_token,
                   pod->selection_token,
                   sizeof(row.pod_selection_token));
            out[row_count++] = row;
            continue;
        }
        bool finished_good = trade_is_finished_good(c);
        int row_capacity = finished_good
            ? (int)lroundf(MAX_PRODUCT_STOCK)
            : (int)lroundf(REFINERY_HOPPER_CAPACITY);
        int dock_stock_units = trade_station_market_pod_units(
            st, station_idx, c, MINING_GRADE_COUNT);
        float station_total_amount = client_station_stock_amount(st, c) +
                                     (float)dock_stock_units;
        int station_total_inv =
            (int)floorf(station_total_amount + 0.0001f);
        int station_space_units =
            (int)floorf((float)row_capacity - station_total_amount + 0.0001f);

        bool pod_actionable = false;
        uint8_t blk = TRADE_BLOCK_NONE;
        float quote = trade_towed_pod_quote(st, pod, &pod_actionable, &blk);
        if (!pod_actionable && blk == TRADE_BLOCK_NO_BUYER &&
            pod->shipment_id == 0) {
            continue;
        }

        int quantity = (int)pod->quantity;
        int total_price = (int)lroundf(quote);
        int unit_price = quantity > 0
            ? (int)lroundf(quote / (float)quantity)
            : total_price;

        uint8_t origin_idx = 0;
        uint64_t mined_blk = 0;
        bool has_lineage = false;
        const cargo_unit_t *inspect_unit = NULL;
        if (pod->manifest_count > 0) {
            inspect_unit = &pod->manifest_units[0];
            origin_idx = inspect_unit->origin_station;
            mined_blk = inspect_unit->mined_block;
            has_lineage = (mined_blk != 0) ||
                          cargo_unit_has_player_origin(inspect_unit);
        }

        trade_row_t row = (trade_row_t){
            .kind = 1, .commodity = c, .grade = MINING_GRADE_COMMON,
            .stock = quantity, .quantity = quantity,
            .unit_price = unit_price, .total_price = total_price,
            .actionable = pod_actionable,
            .station_stock = station_total_inv,
            .station_capacity = row_capacity,
            .held = quantity, .towed_held = quantity,
            .towed_pod_quantity = quantity,
            .is_towed_pod = true,
            .towed_pod_index = (uint16_t)pod_idx,
            .shipment_id = pod->shipment_id,
            .block_reason = blk,
            .prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS,
            .has_lineage = has_lineage,
            .origin_station_idx = origin_idx,
            .mined_block = mined_blk,
        };
        trade_row_attach_inspect(&row, inspect_unit, NULL);
        out[row_count++] = row;
        (void)station_space_units;
    }

    return row_count;
}

bool trade_lineage_available(const station_t *st, const ship_t *ship) {
    if (!st || !ship) return false;
    trade_row_t rows[TRADE_MAX_ROWS];
    int count = build_trade_rows(st, ship, rows, TRADE_MAX_ROWS);
    for (int i = 0; i < count; i++)
        if (rows[i].has_inspect) return true;
    return false;
}

bool trade_lineage_selected_text(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!LOCAL_PLAYER.docked || g.station_view != STATION_VIEW_TRADE ||
        g.trade_lineage_row < 0) return false;
    const station_t *st = current_station_ptr();
    const ship_t *ship = LOCAL_PLAYER.ship;
    if (!st || !ship) return false;

    trade_row_t rows[TRADE_MAX_ROWS];
    int count = build_trade_rows(st, ship, rows, TRADE_MAX_ROWS);
    if (g.trade_lineage_row >= count ||
        !rows[g.trade_lineage_row].has_inspect) return false;
    const trade_lineage_view_t *view =
        trade_lineage_view_for_row(st, &rows[g.trade_lineage_row]);
    if (!view) return false;

    station_ui_append_text(out, out_size, view->title);
    if (g.trade_lineage_proof) {
        for (int i = 0; i < view->proof_count; i++) {
            station_ui_append_text(out, out_size, " | ");
            station_ui_append_text(out, out_size, view->proof[i]);
        }
    } else {
        for (int i = 0; i < view->story_count; i++) {
            station_ui_append_text(out, out_size, " | ");
            station_ui_append_text(out, out_size, view->story[i]);
        }
        for (int i = 0; i < view->gap_count; i++) {
            station_ui_append_text(out, out_size, " | ");
            station_ui_append_text(out, out_size, view->gaps[i]);
        }
        station_ui_append_text(out, out_size, " | ");
        station_ui_append_text(out, out_size, view->current);
        station_ui_append_text(out, out_size, " | ");
        station_ui_append_text(out, out_size, view->custody);
    }
    return true;
}

/* Page resolver shared by renderer + input. Pages are computed by chunking
 * the BUY block and the SELL block independently so SELL always starts on
 * a fresh page — keeps the two halves visually distinct and prevents
 * "selling on the buy page" misclicks. */
void trade_page_range(const trade_row_t *rows, int row_count,
                      int page, int *out_first, int *out_last,
                      int *out_total) {
    uint8_t kinds[TRADE_MAX_ROWS];
    int capped = row_count;
    if (capped < 0) capped = 0;
    if (capped > TRADE_MAX_ROWS) capped = TRADE_MAX_ROWS;
    for (int i = 0; i < capped; i++)
        kinds[i] = rows && rows[i].kind != 0
            ? TRADE_ROW_KIND_SELL
            : TRADE_ROW_KIND_BUY;
    trade_page_range_for_kinds(kinds, capped, TRADE_ROWS_PER_PAGE,
                               page, out_first, out_last, out_total);
}

static const contract_t *tracked_contract_for_station_ui(contract_objective_t *objective)
{
    contract_objective_t local;
    if (!objective) objective = &local;
    if (!contract_objective_for_tracked(objective)) return NULL;
    int ci = objective->contract_index;
    if (ci < 0 || ci >= MAX_CONTRACTS) return NULL;
    const contract_t *ct = &g.world.contracts[ci];
    return ct->active ? ct : NULL;
}

static bool contract_accepts_trade_row(const contract_t *ct,
                                       const trade_row_t *row)
{
    if (!ct || !row || !ct->active) return false;
    if (ct->action != CONTRACT_TRACTOR && ct->action != CONTRACT_DELIVERY)
        return false;
    if (ct->commodity < COMMODITY_RAW_ORE_COUNT) return false;
    if (ct->commodity != row->commodity) return false;
    if (row->has_inspect) {
        cargo_unit_t unit = {0};
        unit.kind = row->inspect_kind;
        unit.commodity = (uint8_t)row->commodity;
        unit.grade = (uint8_t)row->grade;
        unit.prefix_class = row->prefix_class;
        unit.origin_station = row->origin_station_idx;
        unit.quantity = row->quantity > 0
            ? (uint8_t)(row->quantity > 255 ? 255 : row->quantity)
            : 1;
        unit.recipe_id = row->inspect_recipe_id;
        unit.mined_block = row->mined_block;
        memcpy(unit.pub, row->inspect_pub, sizeof(unit.pub));
        memcpy(unit.parent_merkle, row->inspect_parent,
               sizeof(unit.parent_merkle));
        return contract_fit_is_ok(contract_fit_cargo_unit(ct, &unit));
    }
    return contract_fit_is_ok(contract_fit_cargo_fields(
        ct, row->commodity, row->grade, (uint16_t)row->quantity, false));
}

static bool contract_origin_ban_label(const contract_t *ct,
                                      char *out,
                                      size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!ct || !(ct->proof_flags & CONTRACT_PROOF_FORBID_ORIGIN) ||
        ct->forbidden_origin_mask == 0) {
        return false;
    }
    for (int i = 0; i < MAX_STATIONS && i < 64; i++) {
        if ((ct->forbidden_origin_mask & (1ULL << i)) == 0) continue;
        snprintf(out, out_size, "no %s ", station_short_name(i));
        return true;
    }
    return false;
}

static bool contract_black_market_label(const contract_t *ct,
                                        char *out,
                                        size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!ct || ct->action != CONTRACT_TRACTOR ||
        ct->commodity < COMMODITY_RAW_ORE_COUNT ||
        ct->station_index >= MAX_STATIONS) {
        return false;
    }
    const station_t *dest = &g.world.stations[ct->station_index];
    if (!station_exists(dest)) return false;
    if (!station_faction_is_pirate_economy(dest) &&
        !station_policy_cached_has(dest, STATION_POLICY_CARD_BLACK_MARKET)) {
        return false;
    }
    snprintf(out, out_size, "black ");
    return true;
}

static bool trade_row_tracked_note(const station_t *st,
                                   const trade_row_t *row,
                                   char *out,
                                   size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (row && row->is_towed_pod && row->shipment_id != 0) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_shipment(row->shipment_id);
        int here_idx = station_index_of(st);
        if (ledger) {
            bool at_dest = here_idx >= 0 &&
                           here_idx == (int)ledger->destination_station;
            if (at_dest &&
                ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
                ledger->held_bound > 0) {
                snprintf(out, out_size, "ready to deliver");
                return true;
            }
            if (!at_dest && station_policy_accepts_contract_bound_cargo(st)) {
                snprintf(out, out_size, "black-market markdown");
                return true;
            }
            char dest[12];
            ui_station_name_short(ledger->destination_station,
                                  dest, sizeof(dest));
            snprintf(out, out_size, "to %s", dest);
            return true;
        }
        snprintf(out, out_size, "credit cargo");
        return true;
    }

    contract_objective_t objective;
    const contract_t *ct = tracked_contract_for_station_ui(&objective);
    if (!contract_accepts_trade_row(ct, row)) return false;

    int here_idx = station_index_of(st);
    bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;

    if (ct->action == CONTRACT_DELIVERY) {
        bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_contract(objective.contract_index);
        if (row->kind == 0 && at_origin && (!ledger ||
            ledger->status == DELIVERY_SHIPMENT_DELIVERED)) {
            snprintf(out, out_size, "use contracts for credit");
            return true;
        }
        if (row->kind == 1 && at_dest && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
            ledger->held_bound > 0) {
            snprintf(out, out_size, "ready to deliver");
            return true;
        }
        return false;
    }

    if (row->kind == 0) {
        if (at_dest) return false;
        if (row->block_reason == TRADE_BLOCK_NO_FUNDS) {
            snprintf(out, out_size, "need local balance");
            return true;
        }
        if (row->station_stock > 0) {
            return false;
        }
        return false;
    }

    if (row->held <= 0) return false;
    if (at_dest) {
        snprintf(out, out_size, "ready to deliver");
        return true;
    }
    snprintf(out, out_size, "wrong station");
    return true;
}

static bool trade_row_credit_cargo_label(const station_t *st,
                                         const trade_row_t *row,
                                         char *out,
                                         size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!row || row->kind != 1) return false;
    if (row->is_towed_pod && row->shipment_id != 0) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_shipment(row->shipment_id);
        if (!ledger) {
            snprintf(out, out_size, "cargo shipment");
            return true;
        }
        char origin[12];
        char dest[12];
        ui_station_name_short(ledger->origin_station, origin, sizeof(origin));
        ui_station_name_short(ledger->destination_station, dest, sizeof(dest));
        snprintf(out, out_size, "cargo %s>%s", origin, dest);
        (void)st;
        return true;
    }
    contract_objective_t objective;
    const contract_t *ct = tracked_contract_for_station_ui(&objective);
    if (!ct || ct->action != CONTRACT_DELIVERY ||
        ct->commodity != row->commodity) {
        return false;
    }
    int here_idx = station_index_of(st);
    if (here_idx < 0 || here_idx != (int)ct->station_index) return false;
    const NetDeliveryLedgerEntry *ledger =
        ui_delivery_ledger_for_contract(objective.contract_index);
    if (!ledger || ledger->status != DELIVERY_SHIPMENT_PICKED_UP ||
        ledger->held_bound == 0) {
        return false;
    }
    return ui_credit_cargo_route_label(ct, out, out_size);
}

static bool contract_row_tracked_note(int here_idx,
                                      int contract_index,
                                      bool fulfillable_here,
                                      bool selected,
                                      char *out,
                                      size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (contract_index < 0 || contract_index >= MAX_CONTRACTS) return false;
    const contract_t *ct = &g.world.contracts[contract_index];
    if (!ct->active) return false;
    if (!selected) return false;

    if (ct->action == CONTRACT_DELIVERY) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_contract(contract_index);
        bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
        bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;
        int held = ledger ? (int)ledger->held_bound : 0;
        if (at_origin && ledger &&
            ledger->status == DELIVERY_SHIPMENT_DELIVERED) {
            snprintf(out, out_size, "return proof");
            return true;
        }
        if (at_origin && !ledger) {
            int source_stock = station_contract_source_stock_count(
                &g.world.stations[here_idx], ct);
            if (source_stock > 0)
                snprintf(out, out_size, "load pod");
            else
                snprintf(out, out_size, "origin out of stock");
            return true;
        }
        if (at_dest && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP && held > 0) {
            snprintf(out, out_size, "unload pod");
            return true;
        }
        if (held > 0 && !at_dest) {
            snprintf(out, out_size, "wrong station");
            return true;
        }
        if (ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP) {
            snprintf(out, out_size, "deliver to destination");
            return true;
        }
    }
    if (fulfillable_here) {
        if (ct->action == CONTRACT_TRACTOR &&
            ct->commodity < COMMODITY_RAW_ORE_COUNT) {
            snprintf(out, out_size, "load ore");
        } else if (ct->action == CONTRACT_TRACTOR) {
            snprintf(out, out_size, "unload pod");
        } else if (ct->action == CONTRACT_FRACTURE) {
            snprintf(out, out_size, "claim bounty");
        } else {
            snprintf(out, out_size, "deliver cargo");
        }
        return true;
    }

    if (ct->action == CONTRACT_TRACTOR &&
        ct->commodity >= COMMODITY_RAW_ORE_COUNT &&
        contract_fit_manifest_count(ct, &LOCAL_PLAYER.ship->manifest) > 0 &&
        here_idx != (int)ct->station_index) {
        snprintf(out, out_size, "wrong station");
        return true;
    }

    return false;
}

static float ui_towed_matching_ore(const contract_t *ct)
{
    float held = 0.0f;
    const ship_t *ship = LOCAL_PLAYER.ship;
    if (!ct || ct->action != CONTRACT_TRACTOR ||
        ct->commodity >= COMMODITY_RAW_ORE_COUNT) {
        return 0.0f;
    }
    for (int t = 0; t < ship->towed_count; t++) {
        int fi = ship->towed_fragments[t];
        if (fi < 0 || fi >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[fi];
        if (contract_fit_is_ok(contract_fit_fragment(ct, a)))
            held += a->ore;
    }
    return held;
}

static bool contract_row_note_is_action(const char *note)
{
    if (!note || !note[0]) return false;
    return strcmp(note, "load pod") == 0 ||
           strcmp(note, "load ore") == 0 ||
           strcmp(note, "unload pod") == 0 ||
           strcmp(note, "deliver cargo") == 0 ||
           strcmp(note, "claim bounty") == 0 ||
           strcmp(note, "return proof") == 0;
}

static bool station_contract_s_action_label(const station_t *station,
                                            char *out,
                                            size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (g.selected_contract < 0 || g.selected_contract >= MAX_CONTRACTS)
        return false;
    const contract_t *ct = &g.world.contracts[g.selected_contract];
    if (!ct->active) return false;

    int here_idx = station_index_of(station);
    if (ct->action == CONTRACT_DELIVERY) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_contract(g.selected_contract);
        bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
        bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;
        int held = ledger ? (int)ledger->held_bound : 0;
        if (at_origin && ledger &&
            ledger->status == DELIVERY_SHIPMENT_DELIVERED) {
            snprintf(out, out_size, "return proof");
            return true;
        }
        if (at_origin && !ledger) {
            int source_stock = station_contract_source_stock_count(station, ct);
            snprintf(out, out_size, source_stock > 0 ? "accept cargo" : "check stock");
            return true;
        }
        if (at_dest && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP && held > 0) {
            snprintf(out, out_size, "unload pod");
            return true;
        }
        snprintf(out, out_size, "contact");
        return true;
    }

    if (ct->action == CONTRACT_TRACTOR) {
        bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;
        if (!at_dest) {
            snprintf(out, out_size, "track");
            return true;
        }
        if (ct->commodity < COMMODITY_RAW_ORE_COUNT) {
            snprintf(out, out_size,
                     ui_towed_matching_ore(ct) > 0.0f ? "load ore" : "track ore");
            return true;
        }
        snprintf(out, out_size,
                 contract_fit_manifest_count(
                     ct, &LOCAL_PLAYER.ship->manifest) > 0
                    ? "unload pod" : "track cargo");
        return true;
    }

    return false;
}

static const uint8_t COL_CONTRACT_TYPE_HAUL[3]   = { PAL_TRACTOR_OFF };
static const uint8_t COL_CONTRACT_TYPE_BOUNTY[3] = { PAL_WARNING };
static const uint8_t COL_CONTRACT_TYPE_CREDIT[3] = { PAL_DELIVERY_STATUS };

typedef struct {
    market_memory_t memory;
    uint8_t hops;
    int score;
} station_gossip_row_t;

static const char *contract_panel_type_label(const contract_t *ct)
{
    if (!ct) return "???";
    switch (ct->action) {
    case CONTRACT_TRACTOR:  return "HAUL";
    case CONTRACT_FRACTURE: return "BOUNTY";
    case CONTRACT_DELIVERY: return "CREDIT";
    default:                return "???";
    }
}

static const uint8_t *contract_panel_type_color(const contract_t *ct)
{
    if (!ct) return COL_CONTRACT_TYPE_HAUL;
    switch (ct->action) {
    case CONTRACT_FRACTURE: return COL_CONTRACT_TYPE_BOUNTY;
    case CONTRACT_DELIVERY: return COL_CONTRACT_TYPE_CREDIT;
    case CONTRACT_TRACTOR:
    default:                return COL_CONTRACT_TYPE_HAUL;
    }
}

static const char *station_gossip_sentence_prefix(uint8_t kind)
{
    switch ((market_memory_kind_t)kind) {
    case MARKET_MEMORY_DEMAND:             return "needs";
    case MARKET_MEMORY_SUPPLY:             return "stock";
    case MARKET_MEMORY_ROUTE_DANGER:       return "danger";
    case MARKET_MEMORY_ROUTE_SUCCESS:      return "route";
    case MARKET_MEMORY_DELIVERY_RECEIPT:   return "receipt";
    case MARKET_MEMORY_ROUTE_REPUTATION:   return "trusted";
    case MARKET_MEMORY_ROUTE_RISK:         return "risky";
    case MARKET_MEMORY_STATION_TRUST:      return "trusted";
    case MARKET_MEMORY_STATION_RISK:       return "risky";
    case MARKET_MEMORY_ORE_PRESSURE:       return "ore need";
    case MARKET_MEMORY_SCAFFOLD_PRESSURE:  return "build need";
    case MARKET_MEMORY_NONE:
    default:                               return "memory";
    }
}

static const char *station_gossip_action_label(uint8_t action)
{
    switch ((contract_action_t)action) {
    case CONTRACT_TRACTOR:  return "haul";
    case CONTRACT_FRACTURE: return "fracture";
    case CONTRACT_DELIVERY: return "deliver";
    default:                return "work";
    }
}

static mining_grade_t station_gossip_clarity_grade(float clarity)
{
    if (clarity >= 0.86f) return MINING_GRADE_RATI;
    if (clarity >= 0.66f) return MINING_GRADE_RARE;
    if (clarity >= 0.42f) return MINING_GRADE_FINE;
    return MINING_GRADE_COMMON;
}

static bool station_gossip_memory_from_item(const knowledge_item_t *item,
                                            market_memory_t *out)
{
    if (!item || !out) return false;
    if (item->kind != (uint8_t)KNOW_MARKET) return false;
    if (item->payload_kind != (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY) return false;
    market_memory_t memory;
    memset(&memory, 0, sizeof(memory));
    memcpy(&memory, item->payload, sizeof(memory));
    if (!memory.active) return false;
    if (memory.memory_kind == (uint8_t)MARKET_MEMORY_NONE) return false;
    memory.confidence = item->confidence;
    memory.salience = item->salience;
    *out = memory;
    return true;
}

static int station_gossip_collect_rows(const station_t *st,
                                       station_gossip_row_t *out,
                                       int cap)
{
    if (!st || !out || cap <= 0) return 0;
    int count = 0;
    int item_count = st->knowledge.count;
    if (item_count > KNOWLEDGE_VIEW_MAX_CAP) item_count = KNOWLEDGE_VIEW_MAX_CAP;
    for (int i = 0; i < item_count; i++) {
        market_memory_t memory;
        const knowledge_item_t *item = &st->knowledge.items[i];
        if (!station_gossip_memory_from_item(item, &memory))
            continue;
        if (memory.confidence == 0 || memory.salience == 0) continue;
        int score = (int)memory.confidence * (int)memory.salience;
        int insert = count;
        while (insert > 0 && out[insert - 1].score < score) insert--;
        if (insert >= cap) continue;
        if (count < cap) count++;
        for (int j = count - 1; j > insert; j--) out[j] = out[j - 1];
        out[insert].memory = memory;
        out[insert].hops = item->hops;
        out[insert].score = score;
    }
    return count;
}

static float draw_station_gossip_rows(const station_t *st,
                                      float cx, float my, float inner_right,
                                      bool compact)
{
    station_gossip_row_t rows[2];
    int count = station_gossip_collect_rows(st, rows, 2);
    if (count <= 0) return my;

    const uint8_t COL_MEMORY[3] = { PAL_CONTRACT_READY };
    const uint8_t COL_DIM[3]    = { PAL_TEXT_FADED };
    const float row_h = compact ? 14.0f : 15.0f;

    my += compact ? 8.0f : 12.0f;
    my += draw_section_header(cx, my, inner_right, "OVERHEARD", HDR_TRADE);
    for (int i = 0; i < count; i++) {
        const market_memory_t *m = &rows[i].memory;
        char station_name[12];
        ui_station_name_short(m->station_a, station_name, sizeof(station_name));
        const char *commodity = (m->commodity < COMMODITY_COUNT)
            ? commodity_short_name((commodity_t)m->commodity)
            : "signal";
        ui_clarity_t clarity = ui_clarity_from_evidence(
            m->confidence, m->salience, rows[i].hops, COL_MEMORY, COL_DIM);
        char station_seen[12];
        char commodity_seen[24];
        ui_clarity_degrade_text(station_name, clarity.clarity,
                                (uint32_t)(m->subject_nonce ^ (uint64_t)i),
                                station_seen, sizeof(station_seen));
        ui_clarity_degrade_text(commodity, clarity.clarity,
                                (uint32_t)(m->subject_nonce >> 32),
                                commodity_seen, sizeof(commodity_seen));
        char left[48];
        char right[48];
        if (m->memory_kind == (uint8_t)MARKET_MEMORY_DEMAND ||
            m->memory_kind == (uint8_t)MARKET_MEMORY_ORE_PRESSURE ||
            m->memory_kind == (uint8_t)MARKET_MEMORY_SCAFFOLD_PRESSURE)
            snprintf(left, sizeof(left), "%s wanted", commodity_seen);
        else
            snprintf(left, sizeof(left), "%s %s",
                     station_gossip_sentence_prefix(m->memory_kind),
                     commodity_seen);
        mining_grade_t rumor_grade =
            station_gossip_clarity_grade(clarity.clarity);
        uint8_t rumor_rgb[3];
        mining_grade_rgb(rumor_grade, &rumor_rgb[0],
                         &rumor_rgb[1], &rumor_rgb[2]);
        uint8_t rumor_dim[3];
        ui_clarity_mix_rgb(rumor_rgb, COL_DIM,
                           clarity.clarity * 0.70f, rumor_dim);
        snprintf(right, sizeof(right), "%.10s %s",
                 station_seen, station_gossip_action_label(m->action));
        draw_row_lr(cx, my, inner_right, rumor_rgb, left, rumor_dim, right);
        my += row_h;
    }
    return my;
}

static float draw_station_route_history_rows(const station_t *st,
                                             float cx, float my,
                                             float inner_right,
                                             bool compact)
{
    chain_route_history_tail_t rows[2];
    int count = chain_log_read_route_history_tail(st, rows, 2);
    if (count <= 0) return my;

    const uint8_t COL_HISTORY[3] = { 135, 220, 195 };
    const uint8_t COL_DIM[3]     = { PAL_TEXT_SECONDARY };
    const float row_h = compact ? 14.0f : 15.0f;

    my += compact ? 8.0f : 12.0f;
    my += draw_section_header(cx, my, inner_right, "HISTORY", HDR_TRADE);
    for (int i = count - 1; i >= 0; i--) {
        const chain_payload_route_history_t *p = &rows[i].payload;
        char left[72];
        char right[48];
        route_history_compact_fields(p->memory_kind,
                                     p->origin_station,
                                     p->destination_station,
                                     p->commodity,
                                     p->action,
                                     p->evidence_count,
                                     p->confidence,
                                     left, sizeof(left),
                                     right, sizeof(right));
        draw_row_lr(cx, my, inner_right, COL_HISTORY, left, COL_DIM, right);
        my += row_h;
    }
    return my;
}

static float draw_station_policy_rows(const station_t *st,
                                      float cx, float my,
                                      float inner_right,
                                      bool compact)
{
    if (!st) return my;

    const uint8_t COL_POLICY[3] = { 170, 210, 255 };
    const uint8_t COL_DIM[3]    = { PAL_TEXT_FADED };
    const float row_h = compact ? 14.0f : 15.0f;

    my += draw_section_header(cx, my, inner_right, "POLICY", HDR_TRADE);

    int blackglass_rel = station_faction_relation_to(
        st, (uint8_t)STATION_FACTION_BLACKGLASS_SYNDICATE);
    char left[96];
    char right[64];
    snprintf(left, sizeof(left), "%s // %s",
             station_faction_name(st->faction_id),
             station_ideology_name(st->faction_ideology));
    snprintf(right, sizeof(right), "blackglass %s",
             station_faction_relation_label(blackglass_rel));
    draw_row_lr(cx, my, inner_right, COL_POLICY, left, COL_DIM, right);
    my += row_h;

    int count = st->policy_card_count;
    if (count > STATION_IDENTITY_POLICY_CARD_COUNT)
        count = STATION_IDENTITY_POLICY_CARD_COUNT;
    if (count <= 0) {
        draw_row_lr(cx, my, inner_right, COL_DIM, "policy cards pending",
                    NULL, NULL);
        return my + row_h;
    }

    char cards[160] = {0};
    size_t cards_used = 0;
    for (int i = 0; i < count; i++) {
        uint8_t id = st->policy_card_ids[i];
        if (id >= (uint8_t)STATION_POLICY_CARD_COUNT) continue;
        const char *name = station_policy_card_name((station_policy_card_id_t)id);
        int written = snprintf(cards + cards_used,
                               sizeof(cards) - cards_used,
                               "%s%s",
                               cards_used > 0 ? " / " : "",
                               name);
        if (written < 0) break;
        size_t added = (size_t)written;
        if (added >= sizeof(cards) - cards_used) {
            cards_used = sizeof(cards) - 1;
            break;
        }
        cards_used += added;
    }
    draw_row_lr(cx, my, inner_right, COL_DIM, "active cards",
                COL_POLICY, cards[0] ? cards : "unknown");
    return my + row_h;
}

static int collect_route_history_aggregates(route_history_aggregate_row_t *out,
                                            int cap)
{
    if (!out || cap <= 0) return 0;
    memset(out, 0, (size_t)cap * sizeof(out[0]));

    for (int si = 0; si < MAX_STATIONS; si++) {
        chain_route_history_tail_t tail[16];
        int count = chain_log_read_route_history_tail(&g.world.stations[si],
                                                      tail, 16);
        for (int i = 0; i < count; i++) {
            const chain_payload_route_history_t *p = &tail[i].payload;
            route_history_aggregate_add_fields(out,
                                               cap,
                                               p->memory_kind,
                                               p->origin_station,
                                               p->destination_station,
                                               p->commodity,
                                               p->action,
                                               p->evidence_count,
                                               p->confidence,
                                               p->salience,
                                               p->observed_tick);
        }
    }

    return route_history_aggregate_sort(out, cap);
}

static bool station_player_memory_line(const station_t *st,
                                       char *out,
                                       size_t cap)
{
    if (!st || !out || cap == 0) return false;
    out[0] = '\0';
    uint8_t pubkey[32];
    if (!ui_local_player_pubkey(pubkey)) return false;
    for (int i = 0; i < st->ledger_count; i++) {
        if (memcmp(st->ledger[i].player_pubkey, pubkey, 32) != 0)
            continue;
        if (st->ledger[i].total_docks == 0 &&
            st->ledger[i].lifetime_credits_in == 0 &&
            st->ledger[i].lifetime_ore_units == 0) {
            return false;
        }
        const char *commodity = st->ledger[i].top_commodity < COMMODITY_COUNT
            ? commodity_short_name((commodity_t)st->ledger[i].top_commodity)
            : "cargo";
        snprintf(out, cap, "You here: %u docks, %u earned, %u %s",
                 (unsigned)st->ledger[i].total_docks,
                 (unsigned)st->ledger[i].lifetime_credits_in,
                 (unsigned)st->ledger[i].lifetime_ore_units,
                 commodity);
        return true;
    }
    return false;
}

static bool station_known_for_line(const station_t *st,
                                   char *out,
                                   size_t cap)
{
    if (!st || !out || cap == 0) return false;
    out[0] = '\0';
    int station_idx = station_index_of(st);
    route_history_aggregate_row_t aggregate[8];
    int count = collect_route_history_aggregates(
        aggregate, (int)(sizeof(aggregate) / sizeof(aggregate[0])));
    for (int i = 0; i < count; i++) {
        const route_history_aggregate_row_t *row = &aggregate[i];
        if (row->origin_station != station_idx &&
            row->destination_station != station_idx) {
            continue;
        }
        char title[96], evidence[112], freshness[96];
        route_history_aggregate_fields(row->memory_kind,
                                       row->origin_station,
                                       row->destination_station,
                                       row->commodity,
                                       row->action,
                                       row->event_count,
                                       row->evidence_sum,
                                       row->confidence_peak,
                                       row->salience_peak,
                                       row->latest_tick,
                                       title, sizeof(title),
                                       evidence, sizeof(evidence),
                                       freshness, sizeof(freshness));
        (void)evidence;
        (void)freshness;
        snprintf(out, cap, "Known for: %s", title);
        return true;
    }
    return false;
}

bool station_remembered_work_summary(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const station_t *st = current_station_ptr();
    if (!st) return false;

    chain_route_history_tail_t rows[8];
    int count = chain_log_read_route_history_tail(st, rows, 8);
    if (count <= 0) return false;
    const chain_route_history_tail_t *row = &rows[count - 1];
    const chain_payload_route_history_t *p = &row->payload;
    char title[96];
    char evidence[112];
    char meta[96];
    route_history_detail_fields(row->event_id,
                                row->epoch,
                                p->memory_kind,
                                p->origin_station,
                                p->destination_station,
                                p->commodity,
                                p->action,
                                p->evidence_count,
                                p->confidence,
                                p->salience,
                                p->value_hint,
                                p->observed_tick,
                                title, sizeof(title),
                                evidence, sizeof(evidence),
                                meta, sizeof(meta));
    snprintf(out, out_size, "LOCAL SIGNED PROOF | %s | %s | %s",
             title, evidence, meta);
    return true;
}

typedef struct {
    char text[128];
    const uint8_t *rgb;
} station_arrival_line_t;

static const uint8_t ARRIVAL_READY[3]  = { PAL_CONTRACT_READY };
static const uint8_t ARRIVAL_CREDIT[3] = { PAL_STATION_HINT };
static const uint8_t ARRIVAL_MEMORY[3] = { 135, 220, 195 };
static const uint8_t ARRIVAL_PRODUCTION[3] = { 120, 220, 170 };

static bool station_credit_bridge_line(int current_station,
                                       char *out,
                                       size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    if (current_station < 0 || current_station >= MAX_STATIONS)
        return false;
    if (player_current_balance() > 0.5f) return false;

    int best_station = -1;
    float best_balance = 0.0f;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (s == current_station) continue;
        float bal = 0.0f;
        if (!ui_station_balance_for_player(s, &bal)) continue;
        if (bal > best_balance) {
            best_balance = bal;
            best_station = s;
        }
    }
    if (best_station < 0 || best_balance <= 0.5f) return false;
    char source[16];
    ui_station_name_short(best_station, source, sizeof(source));
    snprintf(out, cap, "%s: buy > haul", source);
    return true;
}

bool station_credit_perception_summary(char *out, size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const station_t *st = current_station_ptr();
    if (!st) return false;

    char ledger[192];
    char bridge[128];
    int station_idx = station_index_of(st);
    if (ui_build_ledger_strip(station_idx, ledger, sizeof(ledger)) <= 0)
        return false;

    if (station_credit_bridge_line(station_idx, bridge, sizeof(bridge))) {
        snprintf(out, out_size, "Local balances: %s | %s", ledger, bridge);
    } else {
        snprintf(out, out_size, "Local balances: %s", ledger);
    }
    return true;
}

static const char *contract_ready_action_label(int here_idx,
                                               int contract_index,
                                               bool fulfillable_here)
{
    if (contract_index < 0 || contract_index >= MAX_CONTRACTS)
        return "track";
    const contract_t *ct = &g.world.contracts[contract_index];
    if (!ct->active) return "track";
    if (ct->action == CONTRACT_DELIVERY) {
        const NetDeliveryLedgerEntry *ledger =
            ui_delivery_ledger_for_contract(contract_index);
        bool at_origin = here_idx >= 0 && here_idx == ct->target_index;
        bool at_dest = here_idx >= 0 && here_idx == (int)ct->station_index;
        if (at_origin && ledger &&
            ledger->status == DELIVERY_SHIPMENT_DELIVERED)
            return "return proof";
        if (at_origin && !ledger)
            return "accept cargo";
        if (at_dest && ledger &&
            ledger->status == DELIVERY_SHIPMENT_PICKED_UP &&
            ledger->held_bound > 0)
            return "unload pod";
        return "track route";
    }
    if (fulfillable_here) {
        if (ct->action == CONTRACT_TRACTOR &&
            ct->commodity < COMMODITY_RAW_ORE_COUNT)
            return "load ore";
        if (ct->action == CONTRACT_TRACTOR)
            return "unload pod";
        if (ct->action == CONTRACT_FRACTURE)
            return "claim bounty";
    }
    return "track";
}

static bool station_contract_arrival_line(const station_t *st,
                                          char *out,
                                          size_t cap)
{
    if (!st || !out || cap == 0) return false;
    out[0] = '\0';
    char action[32];
    if (station_contract_s_action_label(st, action, sizeof(action))) {
        snprintf(out, cap, "Selected contract ready: [S] %s.", action);
        return true;
    }

    int slots[3] = {-1, -1, -1};
    bool fulfillable[3] = {false, false, false};
    int held[3] = {0, 0, 0};
    int here_idx = station_index_of(st);
    int count = build_work_slots(here_idx, st->pos, slots, fulfillable, held);
    if (count <= 0) return false;

    const contract_t *ct = &g.world.contracts[slots[0]];
    const char *type = contract_panel_type_label(ct);
    const char *verb = contract_ready_action_label(here_idx, slots[0],
                                                   fulfillable[0]);
    if (fulfillable[0]) {
        snprintf(out, cap, "Ready job: [%d] %s, then [S] %s.",
                 1, type, verb);
        return true;
    }

    char gate_note[64];
    if (ui_contract_laser_gate_note(ct, gate_note, sizeof(gate_note))) {
        snprintf(out, cap, "Nearest job: %s.", gate_note);
        return true;
    }

    contract_objective_t objective;
    if (contract_objective_for_contract(slots[0], &objective) &&
        objective.body[0]) {
        ui_write_parts(out, cap, "Nearest job: ", objective.body, ".");
        return true;
    }
    snprintf(out, cap, "Nearest job: [%d] %s contract.", 1, type);
    return true;
}

static bool station_trade_page_has_pod_rows(const trade_row_t *rows,
                                            int first,
                                            int last)
{
    if (!rows) return false;
    for (int i = first; i < last; i++) {
        if (rows[i].is_station_pod ||
            (rows[i].kind != 0 && rows[i].towed_pod_quantity > 0))
            return true;
    }
    return false;
}

bool station_panel_legend_text(station_view_t view,
                               const station_t *station,
                               char *out,
                               size_t out_size)
{
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const station_panel_descriptor_t *panel = station_panel_descriptor(view);
    if (!panel) return false;

    if (view == STATION_VIEW_TRADE && station) {
        if (g.trade_lineage_row >= 0) {
            snprintf(out, out_size, g.trade_lineage_proof
                ? "[L] next  [I] story  [F] proof page  [ESC] market  [TAB] panel"
                : "[L] next  [I] proof  [ESC] market  [TAB] panel");
            return true;
        }
        trade_row_t rows[TRADE_MAX_ROWS];
        int row_count = build_trade_rows(station, LOCAL_PLAYER.ship,
                                         rows, TRADE_MAX_ROWS);
        int first = 0, last = 0, total_pages = 1;
        trade_page_range(rows, row_count, (int)g.trade_page,
                         &first, &last, &total_pages);
        if ((int)g.trade_page >= total_pages) {
            trade_page_range(rows, row_count, 0,
                             &first, &last, &total_pages);
        }

        const char *row_action = "trade";
        if (first < last) {
            if (rows[first].kind != 0) {
                row_action = station_trade_page_has_pod_rows(rows, first, last)
                    ? (rows[first].kind == 2
                        ? "receipt + unpack"
                        : "unload freight")
                    : "sell unit";
            } else {
                row_action = "buy crate";
            }
        }
        const char *sell_action = LOCAL_PLAYER.ship->towed_pod_count > 0
            ? "tow to intake"
            : "sell all";
        bool has_lineage = false;
        for (int i = first; i < last; i++) {
            if (rows[i].has_inspect) { has_lineage = true; break; }
        }
        snprintf(out, out_size, has_lineage
                 ? "[1-5] %s  [F] page  [L] lineage  [S] %s  [TAB] panel"
                 : "[1-5] %s  [F] page  [S] %s  [TAB] panel",
                 row_action, sell_action);
        return true;
    }

    if (view == STATION_VIEW_WORK && station) {
        char action[32];
        if (station_contract_s_action_label(station, action,
                                            sizeof(action))) {
            snprintf(out, out_size,
                     "[1-3] select  [S] %s  [TAB] panel", action);
            return true;
        }
    }

    if (panel->legend && panel->legend[0]) {
        snprintf(out, out_size, "%s", panel->legend);
        return true;
    }
    return false;
}

static int build_station_arrival_lines(const station_t *st,
                                       station_arrival_line_t *lines,
                                       int cap)
{
    if (!st || !lines || cap <= 0) return 0;
    int n = 0;
    char buf[160];
    bool mirrored_authoritative = g.net_authority_enabled && net_is_connected();

    if (station_contract_arrival_line(st, buf, sizeof(buf)) && n < cap) {
        ui_write_parts(lines[n].text, sizeof(lines[n].text), buf, NULL, NULL);
        lines[n].rgb = ARRIVAL_READY;
        n++;
    }
    /* When a player has money elsewhere but none here, the cargo bridge is
     * the arrival fact that resolves the immediate confusion. Keep it ahead
     * of production so compact layouts do not hide it below the one-line
     * arrival budget. */
    if (station_credit_bridge_line(station_index_of(st), buf, sizeof(buf)) &&
        n < cap) {
        ui_write_parts(lines[n].text, sizeof(lines[n].text), buf, NULL, NULL);
        lines[n].rgb = ARRIVAL_CREDIT;
        n++;
    }
    if (ui_station_production_summary_for(st, mirrored_authoritative,
                                          buf, sizeof(buf)) &&
        n < cap) {
        ui_write_parts(lines[n].text, sizeof(lines[n].text), buf, NULL, NULL);
        lines[n].rgb = ARRIVAL_PRODUCTION;
        n++;
    }
    if (station_player_memory_line(st, buf, sizeof(buf)) && n < cap) {
        ui_write_parts(lines[n].text, sizeof(lines[n].text),
                       "Memory: ", buf, ".");
        lines[n].rgb = ARRIVAL_MEMORY;
        n++;
    } else if (station_known_for_line(st, buf, sizeof(buf)) && n < cap) {
        ui_write_parts(lines[n].text, sizeof(lines[n].text), buf, ".", NULL);
        lines[n].rgb = ARRIVAL_MEMORY;
        n++;
    }
    return n;
}

static float draw_station_arrival_brief(const station_t *st,
                                        float panel_x,
                                        float panel_y,
                                        float panel_w,
                                        bool compact)
{
    station_arrival_line_t lines[3];
    int count = build_station_arrival_lines(st, lines, 3);
    if (count <= 0) return 0.0f;
    int shown = compact ? 1 : 2;
    if (shown > count) shown = count;

    const float cell_w = 8.0f;
    float left_x = panel_x + 20.0f;
    float right_x = panel_x + panel_w - 20.0f;
    float y = panel_y + 78.0f;
    int chars = (int)floorf((right_x - left_x) / cell_w);
    for (int i = 0; i < shown; i++) {
        char line[160];
        char fit[160];
        const char *prefix = i == 0
            ? (compact ? "// " : "ARRIVAL // ")
            : (compact ? "   " : "        // ");
        ui_write_parts(line, sizeof(line), prefix, lines[i].text, NULL);
        ui_fit_text(line, chars, fit, sizeof(fit));
        sdtx_color3b(lines[i].rgb[0], lines[i].rgb[1], lines[i].rgb[2]);
        sdtx_pos(ui_text_pos(left_x), ui_text_pos(y));
        sdtx_puts(fit);
        y += compact ? 14.0f : 15.0f;
    }
    return (float)shown * (compact ? 14.0f : 15.0f) + 8.0f;
}

static float draw_station_memory_summary(const station_t *st,
                                         float cx,
                                         float my,
                                         float inner_right,
                                         bool compact)
{
    const float row_h = compact ? 14.0f : 15.0f;
    const uint8_t COL_HISTORY[3] = { 135, 220, 195 };
    const uint8_t COL_DIM[3]     = { PAL_TEXT_FADED };
    char known[112];
    char player[112];
    bool have_known = station_known_for_line(st, known, sizeof(known));
    bool have_player = station_player_memory_line(st, player, sizeof(player));
    if (!have_known && !have_player) return my;

    my += draw_section_header(cx, my, inner_right, "STATION MEMORY", HDR_TRADE);
    int chars = (int)floorf((inner_right - cx) / 8.0f);
    if (have_known) {
        char fit[112];
        ui_fit_text(known, chars, fit, sizeof(fit));
        draw_row_lr(cx, my, inner_right, COL_HISTORY, fit, NULL, NULL);
        my += row_h;
    }
    if (have_player) {
        char fit[112];
        ui_fit_text(player, chars, fit, sizeof(fit));
        draw_row_lr(cx, my, inner_right, COL_DIM, fit, NULL, NULL);
        my += row_h;
    }
    return my + (compact ? 4.0f : 6.0f);
}

static const char *history_filter_title(uint8_t filter)
{
    switch (filter) {
    case 1:  return "OUTBOUND INSTITUTION MEMORY";
    case 2:  return "INBOUND INSTITUTION MEMORY";
    case 3:  return "LOCAL SIGNED PROOF";
    case 0:
    default: return "INSTITUTION MEMORY";
    }
}

static bool history_filter_matches_aggregate(
    const route_history_aggregate_row_t *row,
    uint8_t filter,
    int station_idx)
{
    if (!row || !row->used) return false;
    switch (filter) {
    case 1: return station_idx >= 0 && row->origin_station == (uint8_t)station_idx;
    case 2: return station_idx >= 0 && row->destination_station == (uint8_t)station_idx;
    case 3: return false;
    case 0:
    default: return true;
    }
}

static void draw_history_view(const station_ui_state_t *ui,
                              float cx, float cy, float inner_w,
                              bool compact)
{
    const station_t *st = ui->station;
    int station_idx = LOCAL_PLAYER.current_station;
    uint8_t filter = g.history_filter <= 3 ? g.history_filter : 0;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;
    const float row_h = compact ? 14.0f : 15.0f;
    const uint8_t COL_HISTORY[3] = { 135, 220, 195 };
    const uint8_t COL_DIM[3]     = { PAL_TEXT_SECONDARY };
    const uint8_t COL_FADED[3]   = { PAL_TEXT_FADED };

    my += draw_section_header(cx, my, inner_right,
                              history_filter_title(filter), HDR_TRADE);

    route_history_aggregate_row_t aggregate[8];
    int aggregate_count = collect_route_history_aggregates(
        aggregate, (int)(sizeof(aggregate) / sizeof(aggregate[0])));
    if (aggregate_count <= 0 && filter != 3) {
        draw_row_lr(cx, my, inner_right, COL_DIM,
                    "No institution memory yet.", NULL, NULL);
        my += row_h;
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "Repeat receipt-backed routes to make memory.",
                    NULL, NULL);
        return;
    }

    if (filter != 3) {
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "Aggregates route proof from station-signed rows.",
                    NULL, NULL);
        my += row_h + (compact ? 2.0f : 4.0f);

        int shown = 0;
        for (int i = 0; i < aggregate_count && shown < 4; i++) {
            const route_history_aggregate_row_t *row = &aggregate[i];
            if (!history_filter_matches_aggregate(row, filter, station_idx))
                continue;
            char title[96];
            char evidence[112];
            char freshness[96];
            route_history_aggregate_fields(row->memory_kind,
                                           row->origin_station,
                                           row->destination_station,
                                           row->commodity,
                                           row->action,
                                           row->event_count,
                                           row->evidence_sum,
                                           row->confidence_peak,
                                           row->salience_peak,
                                           row->latest_tick,
                                           title, sizeof(title),
                                           evidence, sizeof(evidence),
                                           freshness, sizeof(freshness));
            char title_fit[96];
            char evidence_fit[112];
            char freshness_fit[96];
            int chars = (int)floorf((inner_right - cx) / 8.0f);
            ui_fit_text(title, chars, title_fit, sizeof(title_fit));
            ui_fit_text(evidence, chars, evidence_fit, sizeof(evidence_fit));
            ui_fit_text(freshness, chars, freshness_fit, sizeof(freshness_fit));

            draw_row_lr(cx, my, inner_right, COL_HISTORY, title_fit, NULL, NULL);
            my += row_h;
            draw_row_lr(cx + 16.0f, my, inner_right, COL_DIM, evidence_fit, NULL, NULL);
            my += row_h;
            draw_row_lr(cx + 16.0f, my, inner_right, COL_FADED, freshness_fit, NULL, NULL);
            my += row_h + (compact ? 4.0f : 6.0f);
            shown++;
        }
        if (shown == 0) {
            draw_row_lr(cx, my, inner_right, COL_DIM,
                        "No matching institution memory yet.",
                        NULL, NULL);
            my += row_h;
            draw_row_lr(cx, my, inner_right, COL_FADED,
                        "Switch filters or create more signed receipts.",
                        NULL, NULL);
            my += row_h + (compact ? 4.0f : 6.0f);
        }
    }

    if (filter == 0 || filter == 3) {
        my += compact ? 2.0f : 4.0f;
        my += draw_section_header(cx, my, inner_right, "LOCAL SIGNED PROOF", HDR_TRADE);
    } else {
        return;
    }

    chain_route_history_tail_t rows[8];
    int count = chain_log_read_route_history_tail(st, rows, 8);
    if (count <= 0) {
        draw_row_lr(cx, my, inner_right, COL_DIM,
                    "This station has no local proof rows yet.", NULL, NULL);
        my += row_h;
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "Deliveries and repeated receipts create proof.",
                    NULL, NULL);
        return;
    }

    draw_row_lr(cx, my, inner_right, COL_FADED,
                "Newest route proof signed by this station.",
                NULL, NULL);
    my += row_h + (compact ? 2.0f : 4.0f);

    for (int i = count - 1; i >= 0; i--) {
        const chain_route_history_tail_t *row = &rows[i];
        const chain_payload_route_history_t *p = &row->payload;
        char title[96];
        char evidence[112];
        char meta[96];
        route_history_detail_fields(row->event_id,
                                    row->epoch,
                                    p->memory_kind,
                                    p->origin_station,
                                    p->destination_station,
                                    p->commodity,
                                    p->action,
                                    p->evidence_count,
                                    p->confidence,
                                    p->salience,
                                    p->value_hint,
                                    p->observed_tick,
                                    title, sizeof(title),
                                    evidence, sizeof(evidence),
                                    meta, sizeof(meta));
        char title_fit[96];
        char evidence_fit[112];
        char meta_fit[96];
        int chars = (int)floorf((inner_right - cx) / 8.0f);
        ui_fit_text(title, chars, title_fit, sizeof(title_fit));
        ui_fit_text(evidence, chars, evidence_fit, sizeof(evidence_fit));
        ui_fit_text(meta, chars, meta_fit, sizeof(meta_fit));

        draw_row_lr(cx, my, inner_right, COL_HISTORY, title_fit, NULL, NULL);
        my += row_h;
        draw_row_lr(cx + 16.0f, my, inner_right, COL_DIM, evidence_fit, NULL, NULL);
        my += row_h;
        draw_row_lr(cx + 16.0f, my, inner_right, COL_FADED, meta_fit, NULL, NULL);
        my += row_h + (compact ? 4.0f : 6.0f);
    }
}

static bool draw_trade_lineage_focus(const station_t *st,
                                     const trade_row_t *rows, int row_count,
                                     float cx, float *my, float inner_right,
                                     float row_h, float content_bottom,
                                     bool compact) {
    if (!st || !rows || !my || g.trade_lineage_row < 0) return false;
    if (g.trade_lineage_row >= row_count ||
        !rows[g.trade_lineage_row].has_inspect) {
        trade_lineage_close();
        return false;
    }
    const trade_lineage_view_t *view =
        trade_lineage_view_for_row(st, &rows[g.trade_lineage_row]);
    if (!view) {
        trade_lineage_close();
        return false;
    }

    const uint8_t COL_TITLE[3] = { 130, 210, 255 };
    const uint8_t COL_STORY[3] = { PAL_TEXT_SECONDARY };
    const uint8_t COL_CURRENT[3] = { 130, 230, 150 };
    const uint8_t COL_GAP[3] = { PAL_WARNING };
    const uint8_t COL_PROOF[3] = { PAL_TEXT_FADED };
    *my += draw_section_header(cx, *my, inner_right,
                               g.trade_lineage_proof
                                   ? "CARGO PROOF" : "CARGO LINEAGE",
                               HDR_TRADE);
    char title_fit[96];
    int chars = (int)floorf((inner_right - cx) / 8.0f);
    ui_fit_text(view->title, chars, title_fit, sizeof(title_fit));
    draw_row_lr(cx, *my, inner_right, COL_TITLE, title_fit, NULL, NULL);
    *my += row_h + (compact ? 2.0f : 4.0f);

    if (!g.trade_lineage_proof) {
        for (int i = 0; i < view->story_count; i++) {
            if (*my + row_h > content_bottom) break;
            char line[TRADE_LINEAGE_LINE_CAP];
            char fit[TRADE_LINEAGE_LINE_CAP];
            snprintf(line, sizeof(line), "%d. %s", i + 1, view->story[i]);
            ui_fit_text(line, chars, fit, sizeof(fit));
            draw_row_lr(cx, *my, inner_right, COL_STORY, fit, NULL, NULL);
            *my += row_h;
        }
        if (*my + row_h <= content_bottom) {
            char fit[TRADE_LINEAGE_LINE_CAP];
            ui_fit_text(view->current, chars, fit, sizeof(fit));
            draw_row_lr(cx, *my, inner_right, COL_CURRENT, fit, NULL, NULL);
            *my += row_h;
        }
        if (*my + row_h <= content_bottom) {
            char fit[TRADE_LINEAGE_LINE_CAP];
            ui_fit_text(view->custody, chars, fit, sizeof(fit));
            draw_row_lr(cx, *my, inner_right,
                        view->custody[0] && strstr(view->custody, "gap")
                            ? COL_GAP : COL_PROOF,
                        fit, NULL, NULL);
            *my += row_h;
        }
        for (int i = 0; i < view->gap_count; i++) {
            if (*my + row_h > content_bottom) break;
            char fit[TRADE_LINEAGE_LINE_CAP];
            ui_fit_text(view->gaps[i], chars, fit, sizeof(fit));
            draw_row_lr(cx, *my, inner_right, COL_GAP, fit, NULL, NULL);
            *my += row_h;
        }
        return true;
    }

    int rows_fit = (int)floorf((content_bottom - *my) / row_h);
    int per_page = rows_fit - 1; /* reserve the pager line */
    if (per_page < 4) per_page = 4;
    int pages = view->proof_count > 0
        ? (view->proof_count + per_page - 1) / per_page : 1;
    int page = (int)g.trade_lineage_proof_page;
    if (page >= pages) {
        page %= pages;
        g.trade_lineage_proof_page = (uint8_t)page;
    }
    char pager[64];
    snprintf(pager, sizeof(pager), "proof page %d/%d  [F] next",
             page + 1, pages);
    draw_row_lr(cx, *my, inner_right, COL_PROOF,
                "Identifiers behind the story", COL_PROOF, pager);
    *my += row_h;

    int first = page * per_page;
    int last = first + per_page;
    if (last > view->proof_count) last = view->proof_count;
    for (int i = first; i < last && *my + row_h <= content_bottom; i++) {
        char fit[TRADE_LINEAGE_LINE_CAP];
        ui_fit_text(view->proof[i], chars, fit, sizeof(fit));
        draw_row_lr(cx, *my, inner_right, COL_PROOF, fit, NULL, NULL);
        *my += row_h;
    }
    return true;
}

static void draw_trade_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = LOCAL_PLAYER.ship;
    float row_h = compact ? 14.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;
    const uint8_t COL_GAIN[3]  = { 130, 230, 150 };  /* + sell: green */
    const uint8_t COL_COST[3]  = { 230, 110, 110 };  /* - buy:  red   */
    const uint8_t COL_DIM[3]   = { PAL_AFFORD_INACTIVE };
    const uint8_t COL_FADED[3] = { PAL_TEXT_FADED };
    const uint8_t COL_TEXT[3]  = { PAL_TEXT_SECONDARY };
    const uint8_t COL_FLOW_OK[3]   = { 120, 220, 170 };
    const uint8_t COL_FLOW_WARN[3] = { 235, 195, 95 };
    const uint8_t COL_FLOW_BAD[3]  = { 235, 110, 110 };
    char flow_line[128] = "";
    const uint8_t *flow_rgb = NULL;
    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    (void)panel_x;
    (void)panel_w;
    float content_bottom = panel_y + panel_h - 42.0f;

    my += draw_section_header(cx, my, inner_right, "TRADE", HDR_TRADE);

    {
        station_flow_summary_t summary;
        bool mirrored_authoritative = g.net_authority_enabled && net_is_connected();
        bool has_flow = station_flow_summary(st, mirrored_authoritative,
                                             &summary);
        if (ui_station_production_summary_for(st, mirrored_authoritative,
                                              flow_line,
                                              sizeof(flow_line))) {
            flow_rgb = has_flow ? COL_FLOW_OK : COL_TEXT;
            if (has_flow && summary.diag == STATION_FLOW_DIAG_SLOW_FEED)
                flow_rgb = COL_FLOW_WARN;
            else if (has_flow && summary.diag != STATION_FLOW_DIAG_RUNNING)
                flow_rgb = COL_FLOW_BAD;
        }
    }

    /* Single source of truth for the row list (shared with input.c so
     * a [1] keypress always hits the same row drawn here). */
    trade_row_t rows[TRADE_MAX_ROWS];
    int row_count = build_trade_rows(st, ship, rows, TRADE_MAX_ROWS);
    if (draw_trade_lineage_focus(st, rows, row_count,
                                 cx, &my, inner_right, row_h,
                                 content_bottom, compact)) {
        return;
    }
    trade_custody_board_t board;
    trade_custody_board_build(st, ship, &board);

    {
        char right[64];
        snprintf(right, sizeof(right), "%d crates / %d units",
                 board.towed_crates, board.towed_units);
        draw_row_lr(cx, my, inner_right, COL_TEXT, "Your towed crates",
                    board.towed_crates > 0 ? COL_TEXT : COL_FADED, right);
        my += row_h;

        snprintf(right, sizeof(right), "%d crates / %d units",
                 board.station_crates, board.station_units);
        draw_row_lr(cx, my, inner_right, COL_TEXT, "Station crates for sale",
                    board.station_crates > 0 ? COL_TEXT : COL_FADED, right);
        my += row_h;

        snprintf(right, sizeof(right), "%d/%d crates",
                 board.accepted_crates, board.towed_crates);
        draw_row_lr(cx, my, inner_right,
                    board.accepted_crates > 0 ? COL_GAIN : COL_TEXT,
                    board.black_market ? "Intakes accept (black market)"
                                       : "Intakes accept",
                    board.accepted_crates > 0 ? COL_GAIN : COL_FADED,
                    right);
        my += row_h;

        snprintf(right, sizeof(right), "%d open / %d ready",
                 board.exact_contracts, board.exact_ready_crates);
        draw_row_lr(cx, my, inner_right,
                    board.exact_ready_crates > 0 ? COL_GAIN : COL_TEXT,
                    "Contracts needing exact crates",
                    board.exact_contracts > 0 ? COL_TEXT : COL_FADED,
                    right);
        my += row_h + (compact ? 3.0f : 5.0f);
    }

    /* Pagination — BUY and SELL are paginated independently so SELL
     * always starts on a fresh page (see trade_page_range). [F] still
     * walks one page at a time; wraps at the last page. */
    int total_pages = 1, first = 0, last = 0;
    int page = (int)g.trade_page;
    trade_page_range(rows, row_count, page, &first, &last, &total_pages);
    if (page >= total_pages) { page = 0; g.trade_page = 0; }

    if (row_count == 0) {
        if (flow_line[0]) {
            draw_row_lr(cx, my, inner_right, flow_rgb ? flow_rgb : COL_FADED,
                        flow_line, NULL, NULL);
            my += row_h;
        }
        draw_row_lr(cx, my, inner_right, COL_FADED,
                    "No local buy/sell rows right now.", NULL, NULL);
        return;
    }

    /* Side indicator. BUY/SELL lives here, so individual rows can stay
     * focused on the item, stock, and price. */
    {
        char pg[32];
        pg[0] = '\0';
        if (total_pages > 1)
            snprintf(pg, sizeof(pg), "page %d/%d   [F] next",
                     page + 1, total_pages);
        const uint8_t COL_ACTIVE[3] = { 130, 210, 255 };
        bool sell_page = first < last && rows[first].kind != 0;
        bool pod_page = station_trade_page_has_pod_rows(rows, first, last);
        const char *page_kind = sell_page
            ? (compact ? "SELL" : (pod_page ? "YOUR TOWED FREIGHT" : "DOCK SERVICE"))
            : (compact ? "BUY" : "DOCK CRATES FOR SALE");
        char page_left[128];
        const uint8_t *page_left_rgb = COL_ACTIVE;
        if (flow_line[0]) {
            snprintf(page_left, sizeof(page_left), "%s  %s",
                     page_kind, flow_line);
            page_left_rgb = flow_rgb ? flow_rgb : COL_ACTIVE;
        } else {
            snprintf(page_left, sizeof(page_left), "%s", page_kind);
        }
        draw_row_lr(cx, my, inner_right, page_left_rgb, page_left, COL_FADED,
                    pg[0] ? pg : NULL);
        my += row_h;
    }

    /* Hotkey numbering is by row position on the page, NOT by actionable
     * filter — a row keeps its number when it transitions blocked, so the
     * layout under the player's fingers doesn't shift mid-trade. Blocked
     * rows render their number dimmed; pressing it is a no-op. */
    for (int ri = first; ri < last; ri++) {
        const trade_row_t *r = &rows[ri];
        float min_row_h = compact ? (row_h * 2.0f + 4.0f) : row_h;
        if (my + min_row_h > content_bottom) {
            if (my + row_h <= content_bottom) {
                sdtx_color3b(PAL_TEXT_FADED);
                sdtx_pos(ui_text_pos(cx), ui_text_pos(my));
                sdtx_puts(total_pages > 1 ? "[F] more market rows" : "more rows hidden");
            }
            break;
        }
        int slot = (ri - first) + 1;
        char key_buf[16];
        if (r->actionable) snprintf(key_buf, sizeof(key_buf), "[%d]", slot);
        else               snprintf(key_buf, sizeof(key_buf), " - ");

        const uint8_t *info_rgb = r->actionable ? COL_TEXT : COL_FADED;

        /* Grade label + tint. */
        uint8_t ggr, ggg, ggb;
        mining_grade_rgb(r->grade, &ggr, &ggg, &ggb);
        uint8_t gr_rgb[3] = { ggr, ggg, ggb };
        const char *grade_label = mining_grade_label(r->grade);
        const uint8_t *grade_rgb_ptr = r->actionable ? gr_rgb : (uint8_t*)COL_FADED;

        /* Active rows: red for buy (cost), green for sell (gain). Passive
         * rows are dimmed regardless of direction. */
        const uint8_t *total_rgb = r->kind == 0
            ? COL_COST
            : (r->kind == 2 ? COL_TRACKED_JOB : COL_GAIN);
        const uint8_t *row_rgb   = r->actionable ? total_rgb : COL_DIM;

        /* Status column on the left of the right-aligned price:
         * BUY:  station X/MAX
         * SELL: station X/MAX, plus held count only when the player has
         * cargo. Empty SELL rows already explain themselves in the reason
         * column, so avoid "(0 held) (none held)" duplication. */
        char status_buf[40];
        if (r->kind == 0 && r->is_station_pod) {
            snprintf(status_buf, sizeof(status_buf), "dock crate x%d",
                     r->towed_pod_quantity > 0 ? r->towed_pod_quantity
                                               : r->quantity);
        } else if (r->kind == 0) {
            snprintf(status_buf, sizeof(status_buf), "%d/%d",
                     r->station_stock, r->station_capacity);
        } else if (r->is_towed_pod) {
            snprintf(status_buf, sizeof(status_buf), "%d/%d  (crate x%d)",
                     r->station_stock, r->station_capacity,
                     r->towed_pod_quantity > 0 ? r->towed_pod_quantity
                                               : r->quantity);
        } else if (r->held > 0) {
            int internal_held = r->held - r->towed_held;
            if (internal_held > 0 && r->towed_held > 0) {
                snprintf(status_buf, sizeof(status_buf), "%d/%d  (%d hold +%d tow)",
                         r->station_stock, r->station_capacity,
                         internal_held, r->towed_held);
            } else if (r->towed_held > 0) {
                snprintf(status_buf, sizeof(status_buf), "%d/%d  (%d tow)",
                         r->station_stock, r->station_capacity, r->towed_held);
            } else {
                snprintf(status_buf, sizeof(status_buf), "%d/%d  (%d held)",
                         r->station_stock, r->station_capacity, r->held);
            }
        } else {
            snprintf(status_buf, sizeof(status_buf), "%d/%d",
                     r->station_stock, r->station_capacity);
        }

        char total_buf[64];
        const char *trade_cur = ui_station_currency(st);
        if (r->actionable) {
            int total = r->total_price > 0 ? r->total_price : r->unit_price;
            if (r->kind == 2)
                snprintf(total_buf, sizeof(total_buf), "UNPACK");
            else if (r->kind == 0 && r->quantity > 1)
                snprintf(total_buf, sizeof(total_buf), "-%d %s x%d",
                         total, trade_cur, r->quantity);
            else if (r->kind == 0)
                snprintf(total_buf, sizeof(total_buf), "-%d %s", total, trade_cur);
            else
                snprintf(total_buf, sizeof(total_buf), "+%d %s", total, trade_cur);
        } else {
            const char *why = "";
            switch (r->block_reason) {
            case TRADE_BLOCK_STATION_FULL:  why = "(full)";       break;
            case TRADE_BLOCK_STATION_EMPTY: why = "(empty)";      break;
            case TRADE_BLOCK_TOW_FULL:      why = "(tow full)";   break;
            case TRADE_BLOCK_NO_FUNDS:      why = "(no funds)";   break;
            case TRADE_BLOCK_NO_CARGO:      why = "(none held)";  break;
            case TRADE_BLOCK_NO_BUYER:      why = "(no buyer)";   break;
            case TRADE_BLOCK_NO_SELLER:     why = "(no seller)";  break;
            case TRADE_BLOCK_NO_POD_FRAME:  why = "(need frame)"; break;
            case TRADE_BLOCK_NO_RECEIPT_SOURCE:
                why = "(wrong source)";
                break;
            default:                        why = "";             break;
            }
            snprintf(total_buf, sizeof(total_buf), "%s", why);
        }

        /* Prefix-class indicator (#prefix-pricing): drop "M-", "RATi-",
         * etc. before the commodity name so the row's premium price is
         * legible to the player. Anonymous-prefix rows render with no
         * indicator and behave like the legacy cargo path. */
        char commodity_label[48];
        const char *cname = commodity_short_name(r->commodity);
        bool credit_cargo_row =
            trade_row_credit_cargo_label(st, r, commodity_label,
                                         sizeof(commodity_label));
        if (!credit_cargo_row) {
            if (r->is_towed_pod || r->is_station_pod) {
                snprintf(commodity_label, sizeof(commodity_label), "%s crate",
                         cname);
            } else {
                const char *cls_prefix = NULL;
                switch ((ingot_prefix_t)r->prefix_class) {
                case INGOT_PREFIX_M:            cls_prefix = "M-"; break;
                case INGOT_PREFIX_H:            cls_prefix = "H-"; break;
                case INGOT_PREFIX_T:            cls_prefix = "T-"; break;
                case INGOT_PREFIX_S:            cls_prefix = "S-"; break;
                case INGOT_PREFIX_F:            cls_prefix = "F-"; break;
                case INGOT_PREFIX_K:            cls_prefix = "K-"; break;
                case INGOT_PREFIX_RATI:         cls_prefix = "RATi-"; break;
                case INGOT_PREFIX_COMMISSIONED: cls_prefix = "RATi*-"; break;
                case INGOT_PREFIX_ANONYMOUS:
                default:                        cls_prefix = NULL; break;
                }
                if (cls_prefix) {
                    snprintf(commodity_label, sizeof(commodity_label), "%s%s",
                             cls_prefix, cname);
                } else {
                    snprintf(commodity_label, sizeof(commodity_label), "%s", cname);
                }
            }
        } else {
            grade_label = "";
            grade_rgb_ptr = COL_FADED;
        }
        if ((r->is_towed_pod || r->is_station_pod) && !credit_cargo_row) {
            grade_label = "crate";
            grade_rgb_ptr = r->actionable ? COL_TRACKED_JOB : COL_FADED;
        }

        /* Lineage tag — default view keeps provenance player-readable.
         * The forensic chain fields live in HISTORY / inspect surfaces. */
        char lineage_buf[112];
        lineage_buf[0] = '\0';
        char job_note[64];
        (void)trade_row_tracked_note(st, r, job_note, sizeof(job_note));
        if (credit_cargo_row) {
            lineage_buf[0] = '\0';
        } else if (r->has_inspect) {
            cargo_unit_t inspect = {0};
            inspect.kind = r->inspect_kind;
            inspect.commodity = (uint8_t)r->commodity;
            inspect.grade = (uint8_t)r->grade;
            inspect.recipe_id = r->inspect_recipe_id;
            inspect.origin_station = r->origin_station_idx;
            inspect.quantity = 1;
            inspect.mined_block = r->mined_block;
            memcpy(inspect.pub, r->inspect_pub, sizeof(inspect.pub));
            memcpy(inspect.parent_merkle, r->inspect_parent,
                   sizeof(inspect.parent_merkle));
            bool origin_known = inspect.recipe_id != (uint16_t)RECIPE_LEGACY_MIGRATE &&
                ((cargo_kind_t)inspect.kind == CARGO_KIND_INGOT ||
                 inspect.mined_block != 0 || r->inspect_chain_len > 0);
            if (origin_known)
                cargo_lineage_story_label(&inspect, lineage_buf,
                                          sizeof(lineage_buf));
            else
                snprintf(lineage_buf, sizeof(lineage_buf), "origin unknown");
        } else if (r->has_lineage) {
            snprintf(lineage_buf, sizeof(lineage_buf),
                     "from %s, ep %llu",
                     station_short_name((int)r->origin_station_idx),
                     (unsigned long long)r->mined_block);
        }

        if (compact) {
            cell_t top[] = {
                {  0, key_buf,                            row_rgb },
                {  4, commodity_label,                    info_rgb },
                { 22, grade_label,                        grade_rgb_ptr },
            };
            draw_row_cells(cx, my, top, 3);
            my += row_h;
            draw_row_lr(cx + 32.0f, my, inner_right,
                        info_rgb, status_buf, row_rgb, total_buf);
            my += row_h;
            if (job_note[0] && my + row_h <= content_bottom) {
                char note_fit[64];
                int note_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(job_note, note_chars, note_fit, sizeof(note_fit));
                cell_t note[] = {
                    {  4, note_fit, COL_TRACKED_JOB },
                };
                draw_row_cells(cx, my, note, 1);
                my += row_h;
            }
            /* Optional third line — lineage flavor. Drawn dim so it
             * reads as background context, not actionable state. */
            if (lineage_buf[0] && my + row_h <= content_bottom) {
                char lineage_fit[112];
                int lineage_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(lineage_buf, lineage_chars, lineage_fit,
                            sizeof(lineage_fit));
                cell_t lineage[] = {
                    {  4, lineage_fit, COL_FADED },
                };
                draw_row_cells(cx, my, lineage, 1);
                my += row_h;
            }
            /* Inter-row gap so two-line rows don't blur together. */
            my += 4.0f;
        } else {
            cell_t row[] = {
                {  0, key_buf,                            row_rgb },
                {  4, commodity_label,                    info_rgb },
                { 22, grade_label,                        grade_rgb_ptr },
                { 31, status_buf,                         info_rgb },
            };
            draw_row_cells(cx, my, row, 4);
            draw_row_lr(cx, my, inner_right, NULL, NULL, row_rgb, total_buf);
            my += row_h;
            if (job_note[0] && my + row_h <= content_bottom) {
                char note_fit[64];
                int note_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(job_note, note_chars, note_fit, sizeof(note_fit));
                cell_t note[] = {
                    {  4, note_fit, COL_TRACKED_JOB },
                };
                draw_row_cells(cx, my, note, 1);
                my += row_h;
            }
            /* Wide mode also gets a lineage line beneath the row.
             * Same dim styling, indented under the commodity label. */
            if (lineage_buf[0] && my + row_h <= content_bottom) {
                char lineage_fit[112];
                int lineage_chars = (int)floorf((inner_right - (cx + 32.0f)) / 8.0f);
                ui_fit_text(lineage_buf, lineage_chars, lineage_fit,
                            sizeof(lineage_fit));
                cell_t lineage[] = {
                    {  4, lineage_fit, COL_FADED },
                };
                draw_row_cells(cx, my, lineage, 1);
                my += row_h;
            }
        }
    }
    return;
}

/* DOCK — ship bay. Two sections, both always shown:
 *   SHIP BAY — class, hull, cargo, module levels (pure state, no verbs)
 *   SERVICES — repair + per-upgrade refit rows with right-aligned status
 *
 * Row grammar: hotkey+label on the left, status/cost on the right. */
static void draw_verbs_view(const station_ui_state_t *ui,
                            float cx, float cy, float inner_w,
                            bool compact)
{
    const station_t *st = ui->station;
    const ship_t *ship = LOCAL_PLAYER.ship;
    float row_h = compact ? 14.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;
    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    (void)panel_x;
    (void)panel_w;
    float content_bottom = panel_y + panel_h - (compact ? 72.0f : 78.0f);

    const uint8_t COL_TEXT[3]  = { PAL_TEXT_SECONDARY };
    const uint8_t COL_AMBER[3] = { PAL_ORE_AMBER };
    const uint8_t COL_NAV[3]   = { PAL_NAV_BLUE };
    const uint8_t COL_DIM[3]   = { PAL_AFFORD_INACTIVE };
    const uint8_t COL_FADED[3] = { PAL_TEXT_FADED };

    if (st->scaffold) {
        /* Special case: docked at a station still being built. The "verb"
         * here is delivering frames to advance construction. */
        int pct = (int)lroundf(st->scaffold_progress * 100.0f);
        station_construction_need_t need;
        bool has_need = station_construction_material_need(st, &need);
        commodity_t material = has_need ? need.material : COMMODITY_FRAME;
        int held = ship_manifest_count_c(ship, material);
        int remaining = has_need ? (int)ceilf(need.remaining - 0.001f) : 0;
        int required = has_need ? (int)ceilf(need.required - 0.001f)
                                : (int)ceilf(SCAFFOLD_MATERIAL_NEEDED);
        int supplied = has_need ? (int)lroundf(need.supplied)
                                : (int)lroundf(st->scaffold_progress *
                                              SCAFFOLD_MATERIAL_NEEDED);
        if (remaining < 0) remaining = 0;
        char left_buf[48], right_buf[64];
        snprintf(left_buf, sizeof(left_buf), "SCAFFOLD %d%%", pct);
        snprintf(right_buf, sizeof(right_buf), "%d/%d %s",
                 supplied, required, commodity_short_label(material));
        if (!station_row_has_room(my, row_h, content_bottom)) return;
        draw_row_lr(cx, my, inner_right, COL_AMBER, left_buf, COL_FADED,
                    right_buf);
        my += row_h * 1.5f;
        if (!station_row_has_room(my, row_h, content_bottom)) return;
        if (held > 0) {
            snprintf(left_buf, sizeof(left_buf), "carry %s",
                     commodity_short_label(material));
            snprintf(right_buf, sizeof(right_buf), "carry %d / need %d",
                     held, remaining);
            draw_row_lr(cx, my, inner_right, COL_AMBER, left_buf, COL_TEXT, right_buf);
        } else {
            snprintf(right_buf, sizeof(right_buf), "need %d", remaining);
            draw_row_lr(cx, my, inner_right, COL_DIM,
                        "Bring construction material here.", COL_TEXT, right_buf);
        }
        return;
    }

    /* -------- SHIP BAY (always visible) -------- */
    if (!station_row_has_room(my, row_h, content_bottom)) return;
    my += draw_section_header(cx, my, inner_right, "SHIP BAY", HDR_FIT);
    {
        const hull_def_t *def = ship_hull_def(ship);
        const char *class_name = def && def->name ? def->name : "-";
        char right_buf[48];

        if (!station_row_has_room(my, row_h, content_bottom)) return;
        draw_row_lr(cx, my, inner_right, COL_TEXT, "hull class", COL_TEXT, class_name);
        my += row_h;

        snprintf(right_buf, sizeof(right_buf), "%d / %d",
                 (int)lroundf(ship->hull), (int)lroundf(ship_max_hull(ship)));
        if (!station_row_has_room(my, row_h, content_bottom)) return;
        draw_row_lr(cx, my, inner_right, COL_TEXT, "hull", COL_TEXT, right_buf);
        my += row_h;

        float internal_volume = ship_manifest_backed_cargo_volume(ship);
        float towed_volume = local_towed_cargo_volume(ship);
        if (towed_volume > 0.001f) {
            snprintf(right_buf, sizeof(right_buf), "%d + tow %d / %d",
                     (int)lroundf(internal_volume),
                     (int)lroundf(towed_volume),
                     (int)lroundf(ship_cargo_capacity(ship)));
        } else {
            snprintf(right_buf, sizeof(right_buf), "%d / %d",
                     (int)lroundf(internal_volume),
                     (int)lroundf(ship_cargo_capacity(ship)));
        }
        if (!station_row_has_room(my, row_h, content_bottom)) return;
        draw_row_lr(cx, my, inner_right, COL_TEXT, "hold", COL_TEXT, right_buf);
        /* Grade-tinted cargo fill bar -- sits inside the cargo row, just
         * below the text baseline so it visually belongs to that row.
         * Segments are sized by manifest unit volume and colored per
         * grade; raw float cargo (legacy/non-manifest ore only) is folded
         * into common grade. */
        {
            float cap_v = ship_cargo_capacity(ship);
            if (cap_v > 0.0f) {
                float bar_x  = cx + 8.0f;
                float bar_w  = inner_right - bar_x - 8.0f;
                float bar_h  = towed_volume > 0.001f ? 6.0f : 3.0f;
                float bar_y  = my + row_h - bar_h - 2.0f;

                /* Background */
                sgl_begin_quads();
                sgl_c4f(0.10f, 0.10f, 0.12f, 0.85f);
                sgl_v2f(bar_x, bar_y);
                sgl_v2f(bar_x + bar_w, bar_y);
                sgl_v2f(bar_x + bar_w, bar_y + bar_h);
                sgl_v2f(bar_x, bar_y + bar_h);
                sgl_end();

                /* Internal-hold volume per grade. */
                float vol_by_grade[MINING_GRADE_COUNT] = {0};
                for (uint16_t u = 0; u < ship->manifest.count; u++) {
                    const cargo_unit_t *cu = &ship->manifest.units[u];
                    float vol = commodity_volume((commodity_t)cu->commodity);
                    int gi = cu->grade;
                    if (gi < 0 || gi >= MINING_GRADE_COUNT) gi = MINING_GRADE_COMMON;
                    vol_by_grade[gi] += vol;
                }
                for (int c = 0; c < COMMODITY_RAW_ORE_COUNT; c++) {
                    float raw_vol = ship->cargo[c] * commodity_volume((commodity_t)c);
                    if (raw_vol > 0.001f) vol_by_grade[MINING_GRADE_COMMON] += raw_vol;
                }

                /* Segments. Walk grade order so rare/RATi sit on the right. */
                float x = bar_x;
                sgl_begin_quads();
                for (int gi = 0; gi < MINING_GRADE_COUNT; gi++) {
                    if (vol_by_grade[gi] < 0.001f) continue;
                    float cr, cg, cb;
                    mining_grade_rgb_f((mining_grade_t)gi, &cr, &cg, &cb);
                    float seg_w = bar_w * (vol_by_grade[gi] / cap_v);
                    if (seg_w < 0.0f) seg_w = 0.0f;
                    if (x + seg_w > bar_x + bar_w) seg_w = (bar_x + bar_w) - x;
                    sgl_c4f(cr, cg, cb, 0.95f);
                    sgl_v2f(x, bar_y);
                    sgl_v2f(x + seg_w, bar_y);
                    sgl_v2f(x + seg_w, bar_y + (towed_volume > 0.001f ? 2.0f : bar_h));
                    sgl_v2f(x, bar_y + (towed_volume > 0.001f ? 2.0f : bar_h));
                    x += seg_w;
                }
                sgl_end();

                if (towed_volume > 0.001f) {
                    float vol_by_commodity[COMMODITY_COUNT];
                    local_towed_cargo_volume_by_commodity(ship, vol_by_commodity);
                    x = bar_x;
                    sgl_begin_quads();
                    for (int c = 0; c < COMMODITY_COUNT; c++) {
                        if (vol_by_commodity[c] < 0.001f) continue;
                        uint8_t cr, cg, cb;
                        commodity_color_u8((commodity_t)c, &cr, &cg, &cb);
                        float seg_w = bar_w * (vol_by_commodity[c] / cap_v);
                        if (seg_w < 0.0f) seg_w = 0.0f;
                        if (x + seg_w > bar_x + bar_w) seg_w = (bar_x + bar_w) - x;
                        sgl_c4f(cr / 255.0f, cg / 255.0f, cb / 255.0f, 0.82f);
                        sgl_v2f(x, bar_y + 3.0f);
                        sgl_v2f(x + seg_w, bar_y + 3.0f);
                        sgl_v2f(x + seg_w, bar_y + bar_h);
                        sgl_v2f(x, bar_y + bar_h);
                        x += seg_w;
                        if (x >= bar_x + bar_w) break;
                    }
                    if (internal_volume + towed_volume > cap_v + 0.001f) {
                        sgl_c4f(1.0f, 0.82f, 0.28f, 0.95f);
                        sgl_v2f(bar_x + bar_w - 2.0f, bar_y);
                        sgl_v2f(bar_x + bar_w,        bar_y);
                        sgl_v2f(bar_x + bar_w,        bar_y + bar_h);
                        sgl_v2f(bar_x + bar_w - 2.0f, bar_y + bar_h);
                    }
                    sgl_end();
                }
            }
        }
        my += row_h;

        if (compact) {
            snprintf(right_buf, sizeof(right_buf), "LSR %d  HLD %d  TRC %d",
                     ship->mining_level, ship->hold_level, ship->tractor_level);
        } else {
            snprintf(right_buf, sizeof(right_buf), "laser %d  hold %d  tractor %d",
                     ship->mining_level, ship->hold_level, ship->tractor_level);
        }
        if (!station_row_has_room(my, row_h, content_bottom)) return;
        draw_row_lr(cx, my, inner_right, COL_TEXT, "modules", COL_TEXT, right_buf);
        my += row_h;
    }
    my += 6.0f;
    /* Memory is useful context but the primary repair/refit rows must remain
     * visible. Reserve five rows for that section and omit memory when the
     * panel cannot fit both. */
    float service_core_h = row_h * 5.0f;
    float memory_max_h = row_h * 3.0f + 6.0f;
    if (!compact && my + memory_max_h + service_core_h <= content_bottom)
        my = draw_station_memory_summary(st, cx, my, inner_right, compact);

    station_construction_need_t build_need;
    if (station_construction_material_need(st, &build_need)) {
        int remaining = (int)ceilf(build_need.remaining - 0.001f);
        int supplied = (int)lroundf(build_need.supplied);
        int required = (int)ceilf(build_need.required - 0.001f);
        int held = ship_manifest_count_c(ship, build_need.material);
        if (remaining < 1) remaining = 1;
        char left_buf[64], right_buf[48];
        snprintf(left_buf, sizeof(left_buf), "%s r%d/s%d",
                 module_type_name(build_need.module_type),
                 st->modules[build_need.module_index].ring,
                 st->modules[build_need.module_index].slot);
        snprintf(right_buf, sizeof(right_buf), "%d/%d %s",
                 supplied, required, commodity_short_label(build_need.material));

        float full_construction_h = row_h * 3.0f + 6.0f;
        if (!compact &&
            my + full_construction_h + service_core_h <= content_bottom) {
            my += draw_section_header(cx, my, inner_right,
                                      "CONSTRUCTION", HDR_YARD);
            draw_row_lr(cx, my, inner_right, COL_AMBER, left_buf, COL_TEXT,
                        right_buf);
            my += row_h;

            if (held > 0) {
                snprintf(left_buf, sizeof(left_buf), "ready to supply %s",
                         commodity_short_label(build_need.material));
                snprintf(right_buf, sizeof(right_buf), "carry %d / need %d",
                         held, remaining);
                draw_row_lr(cx, my, inner_right, COL_AMBER, left_buf, COL_TEXT,
                            right_buf);
            } else {
                snprintf(right_buf, sizeof(right_buf), "need %d %s",
                         remaining, commodity_short_label(build_need.material));
                draw_row_lr(cx, my, inner_right, COL_DIM, "supply needed",
                            COL_FADED, right_buf);
            }
            my += row_h + 6.0f;
        } else if (my + row_h + 6.0f + service_core_h <= content_bottom) {
            char compact_left[64];
            snprintf(compact_left, sizeof(compact_left), "build %s",
                     module_type_name(build_need.module_type));
            if (held > 0)
                snprintf(right_buf, sizeof(right_buf), "carry %d / need %d",
                         held, remaining);
            else
                snprintf(right_buf, sizeof(right_buf), "need %d %s",
                         remaining, commodity_short_label(build_need.material));
            draw_row_lr(cx, my, inner_right, COL_AMBER, compact_left,
                        COL_FADED, right_buf);
            my += row_h + 6.0f;
        }
    }

    /* -------- SERVICES (always visible; rows always show their status) -------- */
    if (!station_row_has_room(my, row_h, content_bottom)) return;
    my += draw_section_header(cx, my, inner_right, "REPAIR + REFIT", HDR_SERVICE);

    /* [R] repair hull — same grammar as the upgrade rows:
     *   kits [ have / need ]  -N cr
     * "have" = ship cargo + dock inventory; "need" = HP missing (1
     * kit per HP). Append the credit cost when actionable. */
    {
        int kits_avail = ui->ship_kits + ui->station_kits;
        int kits_needed = ui->hull_max - ui->hull_now;
        if (kits_needed < 0) kits_needed = 0;
        char right_buf[32];
        char short_cur[5];
        ui_station_currency_short(st, short_cur, sizeof(short_cur));
        if (!station_row_has_room(my, row_h, content_bottom)) return;
        if (ui->hull_now >= ui->hull_max) {
            draw_row_lr(cx, my, inner_right, COL_DIM, "hull",
                        COL_FADED, "full");
        } else if (kits_avail <= 0) {
            /* Kits gate the repair regardless of credits — surface the
             * shortfall before the credit cost so the [R] row matches
             * what the action handler will say if pressed. */
            int n = kits_needed > 0 ? kits_needed : 1;
            snprintf(right_buf, sizeof(right_buf), "%d repair kit%s needed",
                     n, n == 1 ? "" : "s");
            draw_row_lr(cx, my, inner_right, COL_DIM, "repair hull",
                        COL_FADED, right_buf);
        } else if (ui->can_repair && ui->repair_cost > 0) {
            snprintf(right_buf, sizeof(right_buf), "%d %s",
                     ui->repair_cost, short_cur);
            draw_row_lr(cx, my, inner_right, COL_AMBER, "[R] repair hull",
                        COL_TEXT, right_buf);
        } else if (ui->repair_cost > 0) {
            snprintf(right_buf, sizeof(right_buf), "%d %s needed",
                     ui->repair_cost, short_cur);
            draw_row_lr(cx, my, inner_right, COL_DIM, "repair",
                        COL_FADED, right_buf);
        }
        my += row_h;
    }

    /* [M] tune laser, [C] expand hold, [T] tune tractor — same grammar.
     * Real cost is the modules themselves (frames / lasers / tractors
     * pulled from cargo). If cargo is short, the dock fills the gap
     * from its own inventory at retail. Any dock can install — module
     * supply is the only gate. */
    struct { ship_upgrade_t upgrade; const char *left; const char *passive_left;
             const char *unit_singular; const char *unit_plural;
             int needed, in_cargo, at_station, credit; bool can; bool maxed; } refit[3] = {
        { SHIP_UPGRADE_MINING, "[M] upgrade laser", "laser refit", "laser module",   "laser modules",
          ui->mining_units_needed, ui->mining_units_in_cargo,
          ui->mining_units_at_station, ui->mining_credit_cost,
          ui->can_upgrade_mining,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_MINING) },
        { SHIP_UPGRADE_HOLD, "[C] expand hold",   "hold refit",  "frame", "frames",
          ui->hold_units_needed, ui->hold_units_in_cargo,
          ui->hold_units_at_station, ui->hold_credit_cost,
          ui->can_upgrade_hold,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_HOLD) },
        { SHIP_UPGRADE_TRACTOR, "[T] upgrade tractor", "tractor refit", "tractor module", "tractor modules",
          ui->tractor_units_needed, ui->tractor_units_in_cargo,
          ui->tractor_units_at_station, ui->tractor_credit_cost,
          ui->can_upgrade_tractor,
          ship_upgrade_maxed(ship, SHIP_UPGRADE_TRACTOR) },
    };
    for (int i = 0; i < 3; i++) {
        if (!station_row_has_room(my, row_h, content_bottom)) {
            if (my < content_bottom)
                draw_more_rows_hint(cx, my, "more refit rows hidden");
            return;
        }
        char right_buf[40];
        char short_cur[5];
        ui_station_currency_short(st, short_cur, sizeof(short_cur));
        int avail  = refit[i].in_cargo + refit[i].at_station;
        int needed = refit[i].needed;
        const char *plural = refit[i].unit_plural;
        int from_station = needed -
            (needed < refit[i].in_cargo ? needed : refit[i].in_cargo);
        if (from_station < 0) from_station = 0;
        if (refit[i].can) {
            /* Actionable: show the hotkey + verb with cost. */
            if (refit[i].credit > 0)
                snprintf(right_buf, sizeof(right_buf), "%d %s",
                         refit[i].credit, short_cur);
            else
                snprintf(right_buf, sizeof(right_buf), "ready");
            draw_row_lr(cx, my, inner_right, COL_NAV, refit[i].left,
                        COL_TEXT, right_buf);
            my += row_h;
            if (!refit[i].maxed && refit[i].upgrade == SHIP_UPGRADE_MINING) {
                int remaining_primary = 2 - i;
                if (my + row_h + remaining_primary * row_h <= content_bottom) {
                    draw_row_lr(cx, my, inner_right, COL_DIM, "next laser",
                                COL_FADED,
                                ui_upgrade_effect_label(refit[i].upgrade, ship));
                    my += row_h;
                }
            }
            continue;
        }
        /* Non-actionable: hide the [verb] and keep the left label short;
         * the right side carries the concrete missing material. */
        const char *short_label = refit[i].passive_left;
        if (refit[i].maxed) {
            snprintf(right_buf, sizeof(right_buf), "maxed");
        } else if (avail <= 0) {
            int short_by = needed - avail;
            if (short_by <= 0) short_by = needed > 0 ? needed : 1;
            snprintf(right_buf, sizeof(right_buf), "need %d %s",
                     short_by, short_by == 1
                         ? refit[i].unit_singular : refit[i].unit_plural);
        } else if (refit[i].credit > 0) {
            snprintf(right_buf, sizeof(right_buf), "need %d %s",
                     refit[i].credit, short_cur);
        } else {
            snprintf(right_buf, sizeof(right_buf), "unavailable");
        }
        draw_row_lr(cx, my, inner_right, COL_DIM, short_label,
                    COL_FADED, right_buf);
        my += row_h;
        if (!refit[i].maxed && refit[i].upgrade == SHIP_UPGRADE_MINING) {
            int remaining_primary = 2 - i;
            if (my + row_h + remaining_primary * row_h <= content_bottom) {
                draw_row_lr(cx, my, inner_right, COL_DIM, "next laser",
                            COL_FADED,
                            ui_upgrade_effect_label(refit[i].upgrade, ship));
                my += row_h;
            }
            char supply[128];
            bool has_supply = ui_first_mining_refit_stock_source_label(
                refit[i].upgrade, ship, from_station,
                supply, sizeof(supply));
            if (!has_supply) {
                has_supply =
                    ui_first_mining_refit_production_source_label(
                        refit[i].upgrade, ship,
                        supply, sizeof(supply));
            }
            if (has_supply &&
                my + row_h + remaining_primary * row_h <=
                    content_bottom) {
                draw_row_lr(cx, my, inner_right, COL_DIM, "available",
                            COL_FADED, supply);
                my += row_h;
            }
            char source[112];
            if (ui_upgrade_source_label(refit[i].upgrade, source,
                                        sizeof(source)) &&
                my + row_h + remaining_primary * row_h <= content_bottom) {
                draw_row_lr(cx, my, inner_right, COL_DIM, "source",
                            COL_FADED, source);
                my += row_h;
            }
            char gate[80];
            if (ui_upgrade_input_gate_label(refit[i].upgrade, ship,
                                            gate, sizeof(gate)) &&
                my + row_h + remaining_primary * row_h <= content_bottom) {
                draw_row_lr(cx, my, inner_right, COL_DIM, "input gate",
                            COL_FADED, gate);
                my += row_h;
            }
        }
        (void)plural;
    }
}

/* ------------------------------------------------------------------ */
/* CONTRACTS panel — preserved contract picker, trimmed                */
/* ------------------------------------------------------------------ */

/* CONTRACTS view — dispatch board table.
 * Columns (monospace cells, 8px each):
 *   key(4) type(8) step(11) cargo(17) payout(right-aligned)
 * Rows are sorted: fulfillable here first, then nearest remaining. */
static void draw_contracts_view(const station_ui_state_t *ui,
                                float cx, float cy, float inner_w,
                                bool compact)
{
    (void)inner_w;
    float row_h = compact ? 14.0f : 15.0f;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;

    const uint8_t COL_HDR[3]      = { PAL_TEXT_FADED };
    const uint8_t COL_READY[3]    = { PAL_CONTRACT_READY };
    const uint8_t COL_STATUS[3]   = { PAL_CONTRACT_STATUS };
    const uint8_t COL_DIM[3]      = { PAL_CONTRACT_HINT };
    const uint8_t COL_TEXT[3]     = { PAL_TEXT_SECONDARY };
    const uint8_t COL_FADED[3]    = { PAL_TEXT_FADED };

    my = draw_station_policy_rows(ui->station, cx, my, inner_right, compact);
    my += compact ? 6.0f : 8.0f;
    my += draw_section_header(cx, my, inner_right, "CONTRACTS", HDR_TRADE);

    /* Type is contract class; step is the immediate verb from the shared
     * objective resolver. Payout sits far right so cargo stays scannable. */
    if (!compact) {
        cell_t hdr[] = {
            {  0, "key",    COL_HDR },
            {  4, "type",   COL_HDR },
            { 12, "step",   COL_HDR },
            { 23, "cargo",  COL_HDR },
            { 40, "payout", COL_HDR },
        };
        draw_row_cells(cx, my, hdr, 5);
        my += row_h;
    }

    /* Build slot listing via the shared helper so the rows the player
     * sees here are exactly the rows [1]/[2]/[3] selects from in
     * input.c — no duplication, no drift. */
    int slots[3] = {-1, -1, -1};
    bool slot_fulfillable[3] = {false, false, false};
    int slot_held[3] = {0, 0, 0};
    int here_idx = LOCAL_PLAYER.current_station;
    int slot_count = build_work_slots(here_idx, ui->station->pos,
                                      slots, slot_fulfillable, slot_held);

    if (slot_count == 0) {
        draw_row_lr(cx, my, inner_right, COL_DIM, "No active contracts.", NULL, NULL);
        my += row_h;
        my = draw_station_gossip_rows(ui->station, cx, my, inner_right, compact);
        draw_station_route_history_rows(ui->station, cx, my, inner_right, compact);
        return;
    }

    for (int s = 0; s < slot_count; s++) {
        contract_t *ct = &g.world.contracts[slots[s]];
        float cprice = ct->base_price * (1.0f + fminf(ct->age / 300.0f, 1.0f) * 0.2f);
        bool tracked = (g.tracked_contract == slots[s]);
        bool selected = (g.selected_contract == slots[s]);
        const char *type_txt = contract_panel_type_label(ct);
        const uint8_t *type_rgb = contract_panel_type_color(ct);

        const uint8_t *row_rgb;
        if (selected)                 row_rgb = type_rgb;
        else if (slot_fulfillable[s]) row_rgb = COL_READY;
        else if (tracked)            row_rgb = COL_STATUS;
        else                         row_rgb = COL_DIM;

        /* Selected row accent bar (left edge). */
        if (selected) {
            sgl_begin_quads();
            sgl_c4f(row_rgb[0] / 255.0f, row_rgb[1] / 255.0f, row_rgb[2] / 255.0f, 0.95f);
            sgl_v2f(cx - 10.0f, my - 2.0f);
            sgl_v2f(cx - 7.0f,  my - 2.0f);
            sgl_v2f(cx - 7.0f,  my + 14.0f);
            sgl_v2f(cx - 10.0f, my + 14.0f);
            sgl_end();
        }

        char key_buf[8], cargo_buf[48], pay_buf[64]; /* 64 = room for "+%d %s" with 31-char currency name */
        snprintf(key_buf, sizeof(key_buf), "[%d]%s",
                 s + 1, tracked && !selected ? "*" : "");

        /* Step column doubles as the immediate next-step verb. The shared
         * objective resolver also drives SIGNAL copy and world markers, so
         * the station board cannot drift from the HUD hint. */
        const char *step_txt = "work";
        contract_objective_t objective;
        if (contract_objective_for_contract(slots[s], &objective) &&
            objective.job[0] != '\0') {
            step_txt = objective.job;
        }

        const station_t *dest = (ct->station_index < MAX_STATIONS)
            ? &g.world.stations[ct->station_index] : NULL;

        if (ct->action == CONTRACT_DELIVERY &&
            ui_credit_cargo_route_label(ct, cargo_buf, sizeof(cargo_buf))) {
            /* Credit cargo is a wrapped station-to-station shipment. */
        } else if (ct->action == CONTRACT_FRACTURE) {
            snprintf(cargo_buf, sizeof(cargo_buf), "asteroid field");
        } else if (objective.kind == CONTRACT_OBJECTIVE_PICKUP &&
                   objective.source_station >= 0 &&
                   objective.target_station >= 0) {
            char source_name[12], dest_name[12];
            char route_buf[32];
            ui_station_name_short(objective.source_station,
                                  source_name, sizeof(source_name));
            ui_station_name_short(objective.target_station,
                                  dest_name, sizeof(dest_name));
            snprintf(route_buf, sizeof(route_buf), "%s %s>%s",
                     commodity_short_name(ct->commodity),
                     source_name, dest_name);
            ui_fit_text(route_buf, 16, cargo_buf, sizeof(cargo_buf));
        } else {
            int qty = slot_fulfillable[s] ? slot_held[s]
                                          : (int)lroundf(ct->quantity_needed);
            /* Drop the grade word — color encodes rarity for the whole
             * row (see the grade-tint override below). Keeps the cargo
             * cell short so payout doesn't collide with it. */
            char req_prefix[32];
            if (!contract_origin_ban_label(ct, req_prefix, sizeof(req_prefix)) &&
                !contract_black_market_label(ct, req_prefix, sizeof(req_prefix))) {
                snprintf(req_prefix, sizeof(req_prefix), "%s",
                         ct->proof_flags ? "trace " : "");
            }
            snprintf(cargo_buf, sizeof(cargo_buf), "%s%s x%d",
                     req_prefix,
                     commodity_short_name(ct->commodity), qty);
        }

        /* Currency fallback chain: destination station's currency (that's
         * where the payout actually lands), then the current station's
         * (outpost destinations have empty currency_name today), then
         * "cr" if both are empty. */
        const char *pay_cur = (dest && dest->currency_name[0])
            ? dest->currency_name
            : (ui->station && ui->station->currency_name[0]
               ? ui->station->currency_name : "cr");
        if (!isfinite(cprice) || cprice < 0.0f || cprice > 1.0e7f) {
            snprintf(pay_buf, sizeof(pay_buf), "+??? %s", pay_cur);
        } else {
            snprintf(pay_buf, sizeof(pay_buf), "+%d %s",
                     (int)lroundf(cprice), pay_cur);
        }
        char gate_note[64];
        bool has_gate_note = !slot_fulfillable[s] &&
            ui_contract_laser_gate_note(ct, gate_note, sizeof(gate_note));
        char job_note[64];
        bool has_job_note = contract_row_tracked_note(here_idx, slots[s],
                                                      slot_fulfillable[s],
                                                      selected,
                                                      job_note,
                                                      sizeof(job_note));
        bool job_note_action = contract_row_note_is_action(job_note);
        if (has_gate_note)
            snprintf(pay_buf, sizeof(pay_buf), "%s", gate_note);
        else if (has_job_note && !job_note_action)
            snprintf(pay_buf, sizeof(pay_buf), "%s", job_note);

        const uint8_t *info_rgb = (row_rgb == COL_DIM) ? COL_FADED : COL_TEXT;
        /* Grade tint for the entire row when the contract is for a
         * rare grade. Common grade keeps the neutral row color so the
         * board doesn't constantly look "lit up". Selected/tracked
         * states already override row_rgb above, so those states win
         * over the grade tint (color is the action signal first,
         * rarity second). */
        uint8_t ggr, ggg, ggb;
        mining_grade_rgb((mining_grade_t)ct->required_grade, &ggr, &ggg, &ggb);
        uint8_t grade_rgb[3] = { ggr, ggg, ggb };
        bool rare_grade = ct->action != CONTRACT_FRACTURE
                       && ct->required_grade > (uint8_t)MINING_GRADE_COMMON;
        if (rare_grade && !selected && !tracked) {
            row_rgb = grade_rgb;
        }
        const uint8_t *cargo_rgb = (ct->action == CONTRACT_FRACTURE)
            ? info_rgb
            : (rare_grade ? grade_rgb : info_rgb);
        if (compact) {
            cell_t top[] = {
                {  0, key_buf,   row_rgb },
                {  4, type_txt,  type_rgb },
                { 12, step_txt,  row_rgb },
            };
            draw_row_cells(cx, my, top, 3);
            my += row_h;
            draw_row_lr(cx + 32.0f, my, inner_right,
                        cargo_rgb, cargo_buf, row_rgb, pay_buf);
            my += row_h;
            /* Group separator between multi-line rows. Without it the
             * pay line of row N hugs the key line of row N+1 visually
             * because they share the same row_h spacing. */
            my += 4.0f;
        } else {
            cell_t row[] = {
                {  0, key_buf,   row_rgb },
                {  4, type_txt,  type_rgb },
                { 12, step_txt,  row_rgb },
                { 23, cargo_buf, cargo_rgb },
                { 40, pay_buf,   row_rgb },
            };
            draw_row_cells(cx, my, row, 5);
            my += row_h;
        }
    }

    my = draw_station_gossip_rows(ui->station, cx, my, inner_right, compact);
    draw_station_route_history_rows(ui->station, cx, my, inner_right, compact);
}

/* ------------------------------------------------------------------ */
/* YARD view — fabrication tab: SCAFFOLD KITS catalog + QUEUE           */
/* ------------------------------------------------------------------ */

static const char *yard_module_effect_label(module_type_t type)
{
    switch (type) {
    case MODULE_DOCK:         return "dock traffic";
    case MODULE_HOPPER:       return "stores ore";
    case MODULE_FURNACE:      return "smelts ore";
    case MODULE_REPAIR_BAY:   return "repairs hull";
    case MODULE_SIGNAL_RELAY: return "extends signal";
    case MODULE_FRAME_PRESS:  return "presses frames";
    case MODULE_LASER_FAB:    return "builds lasers";
    case MODULE_TRACTOR_FAB:  return "builds tractors";
    case MODULE_SHIPYARD:     return "builds ships/kits";
    default:                  return "adds station function";
    }
}

static void draw_yard_view(const station_ui_state_t *ui,
                           float cx, float cy, float inner_w, bool compact)
{
    const station_t *st = ui->station;
    float inner_right = cx + inner_w - 36.0f;
    float my = cy;
    float row_h = compact ? 14.0f : 15.0f;
    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    (void)panel_x;
    (void)panel_w;
    float content_bottom = panel_y + panel_h - (compact ? 72.0f : 78.0f);

    if (!station_has_module(st, MODULE_SHIPYARD)) {
        draw_row_lr(cx, my, inner_right, (const uint8_t[3]){ PAL_SHIPYARD_HINT },
                    "No shipyard installed at this station.", NULL, NULL);
        my += row_h;
        draw_row_lr(cx, my, inner_right, (const uint8_t[3]){ PAL_TEXT_FADED },
                    "Visit or build a station with a shipyard module.",
                    NULL, NULL);
        my += row_h;
        draw_row_lr(cx, my, inner_right, (const uint8_t[3]){ PAL_TEXT_FADED },
                    "Fabrication rows will appear here.", NULL, NULL);
        return;
    }

    /* -------- SHIP COMMISSIONS -------- */
    my += draw_section_header(cx, my, inner_right, "SHIP COMMISSIONS", HDR_YARD);
    draw_row_lr(cx, my, inner_right, (const uint8_t[3]){ PAL_STATION_HINT },
                "[U/I/O] drones", (const uint8_t[3]){ PAL_STATION_HINT },
                "[Z/X/V] ships");
    my += row_h;

    static const struct {
        const char *key;
        hull_class_t hull;
    } hull_rows[] = {
        { "U", HULL_CLASS_DRONE_TRACTOR },
        { "I", HULL_CLASS_DRONE_LASER },
        { "O", HULL_CLASS_DRONE_CARGO },
        { "Z", HULL_CLASS_NPC_MINER },
        { "X", HULL_CLASS_HAULER },
        { "V", HULL_CLASS_MINER },
    };
    for (size_t i = 0; i < sizeof(hull_rows) / sizeof(hull_rows[0]); i++) {
        if (!station_row_has_room(my, row_h, content_bottom)) {
            draw_more_rows_hint(cx, my, "more yard rows hidden");
            return;
        }
        int frames = 0, lasers = 0, tractors = 0;
        (void)shipyard_hull_cost(hull_rows[i].hull, &frames, &lasers, &tractors);
        int have_frames =
            station_shipyard_material_available_local(st, COMMODITY_FRAME);
        int have_lasers =
            station_shipyard_material_available_local(st, COMMODITY_LASER_MODULE);
        int have_tractors =
            station_shipyard_material_available_local(st, COMMODITY_TRACTOR_MODULE);
        bool ready = station_shipyard_can_commission_hull_local(
            st, hull_rows[i].hull);
        char left[64], right[32];
        snprintf(left, sizeof(left), "[%s] %s",
                 hull_rows[i].key, ship_loadout_name(hull_rows[i].hull));
        snprintf(right, sizeof(right), "%d/%df %d/%dl %d/%dt",
                 have_frames, frames, have_lasers, lasers,
                 have_tractors, tractors);
        draw_row_lr(cx, my, inner_right,
                    ready ? (const uint8_t[3]){ PAL_TEXT_SECONDARY }
                          : (const uint8_t[3]){ PAL_CANNOT_AFFORD },
                    left,
                    ready ? (const uint8_t[3]){ PAL_TEXT_SECONDARY }
                          : (const uint8_t[3]){ PAL_TEXT_FADED },
                    right);
        my += row_h;
    }
    my += 10.0f;

    int stored_hulls = 0;
    for (int h = 0; h < HULL_CLASS_COUNT; h++)
        stored_hulls += st->stored_hull_count[h];
    if (stored_hulls > 0) {
        my += draw_section_header(cx, my, inner_right, "STORED HULLS", HDR_YARD);
        for (int h = 0; h < HULL_CLASS_COUNT; h++) {
            int count = st->stored_hull_count[h];
            if (count <= 0) continue;
            if (!station_row_has_room(my, row_h, content_bottom)) {
                draw_more_rows_hint(cx, my, "more stored hulls hidden");
                return;
            }
            char left[64], right[16];
            snprintf(left, sizeof(left), "%s", ship_loadout_name((hull_class_t)h));
            snprintf(right, sizeof(right), "x%d", count);
            draw_row_lr(cx, my, inner_right,
                        (const uint8_t[3]){ PAL_TEXT_SECONDARY }, left,
                        (const uint8_t[3]){ PAL_TEXT_SECONDARY }, right);
            my += row_h;
        }
        my += 10.0f;
    }

    /* -------- SCAFFOLD KITS -------- */
    my += draw_section_header(cx, my, inner_right, "SCAFFOLD KITS", HDR_YARD);
    if (station_active_shipyard_count(st) >= 2)
        draw_row_lr(cx, my, inner_right, (const uint8_t[3]){ PAL_STATION_HINT },
                    "[1-9] order scaffold kit", NULL, NULL);
    else
        draw_row_lr(cx, my, inner_right, (const uint8_t[3]){ PAL_STATION_HINT },
                    "Add a second shipyard to fabricate station modules.",
                    NULL, NULL);

    float ly = my + row_h;
    int credits = (int)lroundf(player_current_balance());
    int shown = 0;
    int locked = 0;
    bool any = false;
    for (int t = 0; t < MODULE_COUNT && shown < 9; t++) {
        module_type_t kit = (module_type_t)t;
        if (module_kind(kit) == MODULE_KIND_NONE) continue;
        if (!station_can_order_scaffold(ui->station, kit)) continue;
        any = true;
        bool unlocked = module_unlocked_for_player(LOCAL_PLAYER.ship->unlocked_modules, kit);
        if (!unlocked) { locked++; continue; }
        int fee = scaffold_order_fee(kit);
        int mat = (int)module_build_cost_lookup(kit);
        commodity_t mat_type = module_build_material_lookup(kit);
        const char *mat_name = commodity_short_label(mat_type);
        bool can_afford = credits >= fee;
        if (!station_row_has_room(ly, row_h, content_bottom)) {
            draw_more_rows_hint(cx, ly, "more kit rows hidden");
            return;
        }
        char left[64], right[64];
        snprintf(left, sizeof(left), "[%d] %s -> %s",
                 shown + 1, module_type_name(kit),
                 yard_module_effect_label(kit));
        if (compact) {
            char short_cur[8];
            ui_station_currency_short(ui->station, short_cur, sizeof(short_cur));
            snprintf(right, sizeof(right), "%d %s + %d %s",
                     fee, short_cur, mat, mat_name);
        } else {
            snprintf(right, sizeof(right), "%d %s + %d %s",
                     fee, ui_station_currency(ui->station), mat, mat_name);
        }
        draw_row_lr(cx, ly, inner_right,
                    can_afford ? (const uint8_t[3]){ PAL_TEXT_SECONDARY }
                               : (const uint8_t[3]){ PAL_CANNOT_AFFORD },
                    left,
                    can_afford ? (const uint8_t[3]){ PAL_TEXT_SECONDARY }
                               : (const uint8_t[3]){ PAL_TEXT_FADED },
                    right);
        shown++;
        ly += row_h;
    }
    if (!any) {
        if (!station_row_has_room(ly, row_h, content_bottom)) {
            draw_more_rows_hint(cx, ly, "more yard rows hidden");
            return;
        }
        draw_row_lr(cx, ly, inner_right, (const uint8_t[3]){ PAL_SHIPYARD_HINT },
                    "This yard has no production lines installed.", NULL, NULL);
        ly += row_h;
    }
    if (locked > 0) {
        if (!station_row_has_room(ly, row_h, content_bottom)) {
            draw_more_rows_hint(cx, ly, "more yard rows hidden");
            return;
        }
        char left[64];
        snprintf(left, sizeof(left), "+%d locked", locked);
        draw_row_lr(cx, ly, inner_right, (const uint8_t[3]){ PAL_AFFORD_INACTIVE },
                    left, (const uint8_t[3]){ PAL_TEXT_FADED },
                    "build prerequisites first");
        ly += row_h;
    }

    /* -------- QUEUE — pending construction orders -------- */
    if (ui->station->pending_ship_build_count > 0 ||
        ui->station->pending_scaffold_count > 0) {
        ly += 10.0f;
        ly += draw_section_header(cx, ly, inner_right, "QUEUE", HDR_YARD);
    }
    if (ui->station->pending_ship_build_count > 0) {
        for (int p = 0; p < ui->station->pending_ship_build_count; p++) {
            const hull_class_t hull = ui->station->pending_ship_builds[p].hull_class;
            float progress = ui->station->pending_ship_builds[p].build_progress;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            if (!station_row_has_room(ly, row_h, content_bottom)) {
                draw_more_rows_hint(cx, ly, "more queue rows hidden");
                return;
            }
            char left[64], right[32];
            snprintf(left, sizeof(left), "ship %d. %s",
                     p + 1, ship_loadout_name(hull));
            if (p == 0) {
                snprintf(right, sizeof(right), "%.0f%%", progress * 100.0f);
                draw_row_lr(cx, ly, inner_right,
                            (const uint8_t[3]){ PAL_DELIVERY_BLUE }, left,
                            (const uint8_t[3]){ PAL_DELIVERY_BLUE }, right);
            } else {
                draw_row_lr(cx, ly, inner_right,
                            (const uint8_t[3]){ PAL_SUPPLY_DIM }, left,
                            (const uint8_t[3]){ PAL_TEXT_FADED }, "queued");
            }
            ly += row_h;
        }
    }
    if (ui->station->pending_scaffold_count > 0) {
        const scaffold_t *nascent = NULL;
        int station_idx = station_index_of(ui->station);
        int nascent_idx = (station_idx >= 0 && station_idx < MAX_STATIONS)
            ? station_nascent_scaffold_index(g.world.scaffolds,
                                             MAX_SCAFFOLDS,
                                             station_idx)
            : -1;
        if (nascent_idx >= 0) nascent = &g.world.scaffolds[nascent_idx];
        int blocker_idx = -1;
        if (!nascent && station_idx >= 0 && station_idx < MAX_STATIONS) {
            blocker_idx = station_construction_blocker_index(ui->station,
                                                             g.world.scaffolds,
                                                             MAX_SCAFFOLDS);
        }
        for (int p = 0; p < ui->station->pending_scaffold_count; p++) {
            module_type_t t = ui->station->pending_scaffolds[p].type;
            commodity_t mat_type = module_build_material_lookup(t);
            float need = module_build_cost_lookup(t);
            float have = (p == 0 && nascent) ? nascent->build_amount : 0.0f;
            float station_have = client_station_stock_amount(ui->station,
                                                             mat_type);
            int got = (int)lroundf(have);
            int total = (int)lroundf(need);
            int remaining = (int)ceilf((need - have) - 0.001f);
            if (remaining < 0) remaining = 0;
            const char *mat_label = commodity_short_label(mat_type);
            if (!station_row_has_room(ly, row_h, content_bottom)) {
                draw_more_rows_hint(cx, ly, "more queue rows hidden");
                return;
            }
            char left[64], right[80];
            snprintf(left, sizeof(left), "%d. %s", p + 1, module_type_name(t));
            if (p == 0 && blocker_idx >= 0) {
                snprintf(right, sizeof(right), "blocked; need %d %s stock %d",
                         remaining, mat_label, (int)lroundf(station_have));
                draw_row_lr(cx, ly, inner_right,
                            (const uint8_t[3]){ PAL_WARNING }, left,
                            (const uint8_t[3]){ PAL_WARNING }, right);
            } else if (p == 0) {
                snprintf(right, sizeof(right), "intake %d/%d %s stock %d",
                         got, total, mat_label, (int)lroundf(station_have));
                draw_row_lr(cx, ly, inner_right,
                            (const uint8_t[3]){ PAL_DELIVERY_BLUE }, left,
                            (const uint8_t[3]){ PAL_DELIVERY_BLUE }, right);
            } else {
                draw_row_lr(cx, ly, inner_right,
                            (const uint8_t[3]){ PAL_SUPPLY_DIM }, left,
                            (const uint8_t[3]){ PAL_TEXT_FADED }, "queued");
            }
            ly += row_h;
        }
        if (blocker_idx >= 0) {
            if (!station_row_has_room(ly, row_h, content_bottom)) {
                draw_more_rows_hint(cx, ly, "more queue rows hidden");
                return;
            }
            draw_row_lr(cx, ly, inner_right, (const uint8_t[3]){ PAL_SHIPYARD_HINT },
                        "Tow loose scaffold clear to start next build.",
                        NULL, NULL);
            ly += row_h;
        }
    }
}

static bool station_panel_visible_always(const station_t *station)
{
    (void)station;
    return true;
}

static bool station_panel_visible_shipyard(const station_t *station)
{
    return station && station_has_module(station, MODULE_SHIPYARD);
}

static bool station_panel_visible_history(const station_t *station)
{
    (void)station;
    for (int si = 0; si < MAX_STATIONS; si++) {
        chain_route_history_tail_t row;
        if (chain_log_read_route_history_tail(&g.world.stations[si],
                                              &row, 1) > 0) {
            return true;
        }
    }
    return false;
}

static const station_panel_descriptor_t STATION_PANELS[STATION_VIEW_COUNT] = {
    [STATION_VIEW_DOCK] = {
        .view = STATION_VIEW_DOCK,
        .label = "SHIP",
        .legend = "[R] repair  [M/C/T] refit  [TAB] panel",
        .visible_fn = station_panel_visible_always,
        .draw_fn = draw_verbs_view,
        .input_fn = station_panel_input_dock,
    },
    [STATION_VIEW_TRADE] = {
        .view = STATION_VIEW_TRADE,
        .label = "TRADE",
        .legend = "[1-5] trade  [F] page  [S] sell  [TAB] panel",
        .visible_fn = station_panel_visible_always,
        .draw_fn = draw_trade_view,
        .input_fn = station_panel_input_trade,
    },
    [STATION_VIEW_WORK] = {
        .view = STATION_VIEW_WORK,
        .label = "CONTRACTS",
        .legend = "[1-3] select  [S] track  [TAB] panel",
        .visible_fn = station_panel_visible_always,
        .draw_fn = draw_contracts_view,
        .input_fn = station_panel_input_work,
    },
    [STATION_VIEW_HISTORY] = {
        .view = STATION_VIEW_HISTORY,
        .label = "HISTORY",
        .legend = "[TAB] panel",
        .visible_fn = station_panel_visible_history,
        .draw_fn = draw_history_view,
        .input_fn = station_panel_input_history,
    },
    [STATION_VIEW_YARD] = {
        .view = STATION_VIEW_YARD,
        .label = "YARD",
        .legend = "[U/I/O/Z/X/V] ships  [1-9] kits  [TAB]",
        .visible_fn = station_panel_visible_shipyard,
        .draw_fn = draw_yard_view,
        .input_fn = station_panel_input_yard,
    },
};

const station_panel_descriptor_t *station_panel_descriptor(station_view_t view)
{
    int index = (int)view;
    if (index < 0 || index >= (int)STATION_VIEW_COUNT) return NULL;
    const station_panel_descriptor_t *panel = &STATION_PANELS[index];
    return panel->label ? panel : NULL;
}

bool station_panel_visible(const station_panel_descriptor_t *panel,
                           const station_t *station)
{
    if (!panel) return false;
    return !panel->visible_fn || panel->visible_fn(station);
}

int station_panel_visible_count(const station_t *station)
{
    int count = 0;
    for (int i = 0; i < (int)STATION_VIEW_COUNT; i++) {
        const station_panel_descriptor_t *panel =
            station_panel_descriptor((station_view_t)i);
        if (station_panel_visible(panel, station)) count++;
    }
    return count;
}

const station_panel_descriptor_t *station_panel_visible_at(
    const station_t *station, int visible_index)
{
    if (visible_index < 0) return NULL;
    int count = 0;
    for (int i = 0; i < (int)STATION_VIEW_COUNT; i++) {
        const station_panel_descriptor_t *panel =
            station_panel_descriptor((station_view_t)i);
        if (!station_panel_visible(panel, station)) continue;
        if (count == visible_index) return panel;
        count++;
    }
    return NULL;
}

station_view_t station_panel_first_visible(const station_t *station)
{
    const station_panel_descriptor_t *panel =
        station_panel_visible_at(station, 0);
    return panel ? panel->view : STATION_VIEW_DOCK;
}

station_view_t station_panel_next_visible(station_view_t current,
                                          const station_t *station,
                                          int direction)
{
    int count = station_panel_visible_count(station);
    if (count <= 0) return STATION_VIEW_DOCK;

    int current_visible = -1;
    for (int i = 0; i < count; i++) {
        const station_panel_descriptor_t *panel =
            station_panel_visible_at(station, i);
        if (panel && panel->view == current) {
            current_visible = i;
            break;
        }
    }

    if (current_visible < 0)
        return station_panel_first_visible(station);

    int delta = direction < 0 ? -1 : 1;
    int next = (current_visible + delta + count) % count;
    const station_panel_descriptor_t *panel =
        station_panel_visible_at(station, next);
    return panel ? panel->view : STATION_VIEW_DOCK;
}

void station_panel_sample_current(input_intent_t *intent)
{
    if (!LOCAL_PLAYER.docked) return;
    const station_t *station = current_station_ptr();
    const station_panel_descriptor_t *panel =
        station_panel_descriptor(g.station_view);
    if (!station_panel_visible(panel, station)) {
        g.station_view = station_panel_first_visible(station);
        panel = station_panel_descriptor(g.station_view);
    }
    if (panel && panel->input_fn) panel->input_fn(intent);
}

/* ------------------------------------------------------------------ */
/* draw_station_services -- header band + view dispatch                */
/* ------------------------------------------------------------------ */

void draw_station_services(const station_ui_state_t* ui) {
    if (!LOCAL_PLAYER.docked) return;
    if (!ui->station) return;

    float panel_x = 0.0f, panel_y = 0.0f, panel_w = 0.0f, panel_h = 0.0f;
    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    bool compact = ui_is_compact();

    draw_header_band(ui, panel_x, panel_y, panel_w, compact);
    float brief_h = draw_station_arrival_brief(ui->station, panel_x, panel_y,
                                               panel_w, compact);

    /* View content begins below the 3-line header (last line at panel_y+58)
     * and the divider rule at panel_y+72. */
    float inner_x = panel_x + 18.0f;
    float inner_w = panel_w - 36.0f;
    float content_top = panel_y + 78.0f + brief_h;
    float cx = inner_x + 18.0f;

    /* Station-role tint — used sparingly: active tab latch + section rules
     * elsewhere. Not washed across the whole panel. */
    float rr = 1, rg = 1, rb = 1;
    station_role_color(ui->station, &rr, &rg, &rb);
    const station_panel_descriptor_t *active_panel =
        station_panel_descriptor(g.station_view);
    if (!station_panel_visible(active_panel, ui->station)) {
        g.station_view = station_panel_first_visible(ui->station);
        active_panel = station_panel_descriptor(g.station_view);
    }

    /* Tab strip, LEFT-aligned on the first content line.
     *   SHIP  — ship bay (repair / refit / current ship state)
     *   TRADE — market (buy / sell cargo)
     *   CONTRACTS — active contracts and routing
     *   YARD  — fabrication (kits + construction queue, shipyard stations only)
     * Active tab: station-role tint + a short underline latch. Inactive:
     * muted. The active panel's key legend sits at the bottom of the panel. */
    {
        const float cell_w = 8.0f;
        float ty = content_top + 2.0f;
        float tx = cx;
        bool tight_tabs = compact && panel_w < 280.0f;
        /* Record active tab geometry so we can draw the latch after the
         * text pass (so the quad doesn't clobber glyphs). */
        float active_x0 = 0.0f, active_x1 = 0.0f;
        bool active_seen = false;
        int panel_count = station_panel_visible_count(ui->station);
        for (int i = 0; i < panel_count; i++) {
            const station_panel_descriptor_t *panel =
                station_panel_visible_at(ui->station, i);
            if (!panel) continue;
            bool active = (panel->view == g.station_view);
            char cell[20];
            if (tight_tabs) {
                snprintf(cell, sizeof(cell), active ? "[%c]" : " %c ",
                         panel->label[0]);
            } else {
                snprintf(cell, sizeof(cell), active ? "[%s]" : " %s ",
                         panel->label);
            }
            float w = (float)strlen(cell) * cell_w;
            if (active) {
                sdtx_color3b((uint8_t)(rr * 255.0f),
                             (uint8_t)(rg * 255.0f),
                             (uint8_t)(rb * 255.0f));
                active_x0 = tx;
                active_x1 = tx + w;
                active_seen = true;
            } else {
                sdtx_color3b(PAL_TEXT_MUTED);
            }
            sdtx_pos(ui_text_pos(tx), ui_text_pos(ty));
            sdtx_puts(cell);
            tx += w + (tight_tabs ? 4.0f : 10.0f);
        }

        /* Latch underline beneath active tab — diegetic "channel selected"
         * affordance in the station-role tint. */
        if (active_seen) {
            float uy = ty + 14.0f;
            sgl_begin_quads();
            sgl_c4f(rr, rg, rb, 0.9f);
            sgl_v2f(active_x0, uy);
            sgl_v2f(active_x1, uy);
            sgl_v2f(active_x1, uy + 2.0f);
            sgl_v2f(active_x0, uy + 2.0f);
            sgl_end();
        }
    }
    float cy = content_top + 34.0f;

    if (active_panel && active_panel->draw_fn)
        active_panel->draw_fn(ui, cx, cy, inner_w, compact);

    char dynamic_hint[96];
    const char *hint = station_panel_legend_text(
        active_panel ? active_panel->view : STATION_VIEW_DOCK,
        ui->station, dynamic_hint, sizeof(dynamic_hint))
        ? dynamic_hint
        : "[TAB] panels";
    char hint_fit[96];
    int hint_chars = (int)floorf((panel_w - 40.0f) / 8.0f);
    ui_fit_text(hint, hint_chars, hint_fit, sizeof(hint_fit));
    sdtx_color3b(PAL_TEXT_FADED);
    sdtx_pos(ui_text_pos(panel_x + 20.0f),
             ui_text_pos(panel_y + panel_h - (compact ? 56.0f : 62.0f)));
    sdtx_puts(hint_fit);
}
