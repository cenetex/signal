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
uint8_t           ship_module_socket_count(const ship_t *ship);
uint8_t           ship_module_mask(const ship_t *ship);
bool              ship_has_module(const ship_t *ship, ship_module_flags_t module);
const char*       ship_loadout_name(hull_class_t hull_class);

/* Authored one-size cell graph behind each legacy hull class. Hull classes
 * remain control/loadout compatibility labels; physical capacity, mass, and
 * thrust accounting come from these cells. */
cell_layout_kind_t ship_cell_layout_kind(hull_class_t hull_class);
bool ship_cell_graph(const ship_t *ship, cell_graph_t *out);
bool ship_cell_graph_for_identity(const ship_t *ship, uint32_t asset_id,
                                  cell_graph_t *out);
void ship_cell_totals(const ship_t *ship, cell_graph_totals_t *out);
float ship_cell_shell_mass(const ship_t *ship);
float ship_cell_total_mass(const ship_t *ship);
float ship_cell_thrust_units(const ship_t *ship);
float ship_bulk_capacity(const ship_t *ship);
vec2 ship_cell_center_of_mass(const ship_t *ship);
vec2 ship_tow_hardpoint_local(const ship_t *ship);
vec2 ship_tow_hardpoint_world(const ship_t *ship);

vec2 ship_forward(float angle);
vec2 ship_muzzle(vec2 pos, float angle, const ship_t* ship);

float ship_max_hull(const ship_t* ship);
float npc_max_hull(const npc_ship_t* npc);
float ship_cargo_capacity(const ship_t* ship);
float ship_mining_rate(const ship_t* ship);
float ship_tractor_range(const ship_t* ship);
float ship_collect_radius(const ship_t* ship);
int ship_tow_body_capacity(const ship_t *ship);
int ship_towed_fragment_count(const ship_t *ship);
int ship_towed_pod_count(const ship_t *ship);
int ship_towed_body_count(const ship_t *ship);
int ship_tow_body_space(const ship_t *ship);
bool ship_has_towed_fragments(const ship_t *ship);
bool ship_has_towed_pods(const ship_t *ship);
bool ship_has_towed_bodies(const ship_t *ship);

int ship_upgrade_level(const ship_t* ship, ship_upgrade_t upgrade);
bool ship_upgrade_maxed(const ship_t* ship, ship_upgrade_t upgrade);
int ship_upgrade_cost(const ship_t* ship, ship_upgrade_t upgrade);

product_t upgrade_required_product(ship_upgrade_t upgrade);
float upgrade_product_cost(const ship_t* ship, ship_upgrade_t upgrade);
const char* product_name(product_t product);

#endif
