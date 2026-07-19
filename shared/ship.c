#include "ship.h"
#include "commodity.h"

const hull_def_t* hull_def_for_class(hull_class_t hc) {
    if ((unsigned)hc >= HULL_CLASS_COUNT) hc = HULL_CLASS_MINER;
    return &HULL_DEFS[hc];
}

float hull_max_for_class(hull_class_t hc) {
    return hull_def_for_class(hc)->max_hull;
}

const hull_def_t* ship_hull_def(const ship_t* ship) {
    return hull_def_for_class(ship->hull_class);
}

const hull_def_t* npc_hull_def(const npc_ship_t* npc) {
    return hull_def_for_class(npc->ship->hull_class);
}

uint8_t ship_module_socket_count(const ship_t *ship) {
    return ship_hull_def(ship)->module_slots;
}

uint8_t ship_module_mask(const ship_t *ship) {
    return ship_hull_def(ship)->module_mask;
}

bool ship_has_module(const ship_t *ship, ship_module_flags_t module) {
    return (ship_module_mask(ship) & (uint8_t)module) != 0;
}

const char* ship_loadout_name(hull_class_t hull_class) {
    return hull_def_for_class(hull_class)->name;
}

cell_layout_kind_t ship_cell_layout_kind(hull_class_t hull_class) {
    switch (hull_class) {
    case HULL_CLASS_HAULER:
    case HULL_CLASS_DRONE_CARGO:
        return CELL_LAYOUT_LIGHT_FREIGHTER;
    case HULL_CLASS_DRONE_TRACTOR:
        return CELL_LAYOUT_TUG;
    case HULL_CLASS_MINER:
    case HULL_CLASS_NPC_MINER:
    case HULL_CLASS_DRONE_LASER:
    default:
        return CELL_LAYOUT_UTILITY;
    }
}

bool ship_cell_graph_for_identity(const ship_t *ship, uint32_t asset_id,
                                  cell_graph_t *out) {
    if (!ship || !out ||
        !cell_graph_authored(ship_cell_layout_kind(ship->hull_class), out)) {
        return false;
    }
    if (asset_id != 0) {
        for (uint8_t i = 0; i < out->count; i++)
            out->nodes[i].identity =
                ((uint64_t)asset_id << 32) | (uint64_t)(i + 1);
    }
    return cell_graph_validate(out);
}

bool ship_cell_graph(const ship_t *ship, cell_graph_t *out) {
    return ship_cell_graph_for_identity(ship, 0, out);
}

void ship_cell_totals(const ship_t *ship, cell_graph_totals_t *out) {
    if (!out) return;
    cell_graph_t graph;
    if (!ship_cell_graph(ship, &graph)) {
        *out = (cell_graph_totals_t){0};
        return;
    }
    cell_graph_totals(&graph, out);
    out->payload_units = (int)ship_total_cargo(ship);
    out->payload_mass = ship_total_cargo(ship);
    out->total_mass = out->shell_mass + out->payload_mass;
}

float ship_cell_shell_mass(const ship_t *ship) {
    cell_graph_totals_t totals;
    ship_cell_totals(ship, &totals);
    return totals.shell_mass;
}

float ship_cell_total_mass(const ship_t *ship) {
    cell_graph_totals_t totals;
    ship_cell_totals(ship, &totals);
    return totals.total_mass;
}

float ship_cell_thrust_units(const ship_t *ship) {
    cell_graph_totals_t totals;
    ship_cell_totals(ship, &totals);
    return totals.thrust_units;
}

float ship_bulk_capacity(const ship_t *ship) {
    cell_graph_totals_t totals;
    ship_cell_totals(ship, &totals);
    return (float)totals.cargo_capacity;
}

vec2 ship_cell_center_of_mass(const ship_t *ship) {
    cell_graph_t graph;
    if (!ship_cell_graph(ship, &graph)) return v2(0.0f, 0.0f);
    /* Distribute live aggregate cargo over the graph's volume cells in
     * deterministic node order before asking the shared graph calculator. */
    float remaining = ship_total_cargo(ship);
    for (uint8_t i = 0; i < graph.count; i++) {
        int capacity = cell_shape_payload_capacity(
            (cell_shape_t)graph.nodes[i].shape);
        float held = fminf(remaining, (float)capacity);
        graph.nodes[i].payload_units = (uint16_t)held;
        remaining -= held;
    }
    cell_point_t center = cell_graph_center_of_mass(&graph);
    return v2(center.x, center.y);
}

vec2 ship_tow_hardpoint_local(const ship_t *ship) {
    cell_graph_t graph;
    if (!ship_cell_graph(ship, &graph)) return v2(0.0f, 0.0f);
    vec2 center = ship_cell_center_of_mass(ship);

    /* An explicit tow triangle owns the attachment when fitted. */
    for (uint8_t i = 0; i < graph.count; i++) {
        const cell_node_t *node = &graph.nodes[i];
        if (node->shape != CELL_SHAPE_TRIANGLE ||
            node->role != CELL_ROLE_TOW) continue;
        cell_point_t host = cell_coord_world(node->coord, CELL_EDGE_LENGTH);
        float angle = (float)node->orientation * 1.0471975511965976f;
        vec2 tip = v2_add(v2(host.x, host.y),
                          v2_scale(v2_from_angle(angle),
                                   CELL_EDGE_LENGTH * 1.7320508f));
        return v2_sub(tip, center);
    }

    /* Compatibility hulls without a fitted tow triangle expose the named
     * complete east edge of their forward-most volume cell. */
    vec2 forward = v2(0.0f, 0.0f);
    float best_x = -1.0e30f;
    for (uint8_t i = 0; i < graph.count; i++) {
        const cell_node_t *node = &graph.nodes[i];
        if (node->shape == CELL_SHAPE_TRIANGLE) continue;
        cell_point_t p = cell_coord_world(node->coord, CELL_EDGE_LENGTH);
        if (p.x > best_x) {
            best_x = p.x;
            forward = v2(p.x + CELL_EDGE_LENGTH * 0.8660254038f, p.y);
        }
    }
    return v2_sub(forward, center);
}

vec2 ship_tow_hardpoint_world(const ship_t *ship) {
    if (!ship) return v2(0.0f, 0.0f);
    vec2 local = ship_tow_hardpoint_local(ship);
    vec2 basis = v2_from_angle(ship->angle);
    float c = basis.x, s = basis.y;
    return v2(ship->pos.x + local.x * c - local.y * s,
              ship->pos.y + local.x * s + local.y * c);
}

vec2 ship_forward(float angle) {
    return v2_from_angle(angle);
}

vec2 ship_muzzle(vec2 pos, float angle, const ship_t* ship) {
    vec2 forward = v2_from_angle(angle);
    return v2_add(pos, v2_scale(forward, ship_hull_def(ship)->ship_radius + 8.0f));
}

float ship_max_hull(const ship_t* ship) {
    return hull_max_for_class(ship->hull_class);
}

float npc_max_hull(const npc_ship_t* npc) {
    return hull_max_for_class(npc->ship->hull_class);
}

float ship_cargo_capacity(const ship_t* ship) {
    if (!ship) return 0.0f;
    return ship_bulk_capacity(ship) +
           ((float)ship->hold_level * SHIP_HOLD_UPGRADE_STEP);
}

float ship_mining_rate(const ship_t* ship) {
    return ship_hull_def(ship)->mining_rate + ((float)ship->mining_level * SHIP_MINING_UPGRADE_STEP);
}

float ship_tractor_range(const ship_t* ship) {
    return ship_hull_def(ship)->tractor_range + ((float)ship->tractor_level * SHIP_TRACTOR_UPGRADE_STEP);
}

float ship_collect_radius(const ship_t* ship) {
    return SHIP_BASE_COLLECT_RADIUS + ((float)ship->tractor_level * SHIP_COLLECT_UPGRADE_STEP);
}

int ship_tow_body_capacity(const ship_t *ship) {
    int cap = 2 + (ship ? ship->tractor_level : 0) * 2;
    if (cap < 0) cap = 0;
    if (cap > 10) cap = 10;
    return cap;
}

int ship_towed_fragment_count(const ship_t *ship) {
    if (!ship) return 0;
    int count = ship->towed_count;
    int cap = (int)(sizeof(ship->towed_fragments) /
                    sizeof(ship->towed_fragments[0]));
    if (count < 0) count = 0;
    if (count > cap) count = cap;
    return count;
}

int ship_towed_pod_count(const ship_t *ship) {
    if (!ship) return 0;
    int count = ship->towed_pod_count;
    int cap = (int)(sizeof(ship->towed_pods) /
                    sizeof(ship->towed_pods[0]));
    if (count < 0) count = 0;
    if (count > cap) count = cap;
    return count;
}

int ship_towed_body_count(const ship_t *ship) {
    return ship_towed_fragment_count(ship) + ship_towed_pod_count(ship);
}

int ship_tow_body_space(const ship_t *ship) {
    int space = ship_tow_body_capacity(ship) - ship_towed_body_count(ship);
    return space > 0 ? space : 0;
}

bool ship_has_towed_fragments(const ship_t *ship) {
    return ship_towed_fragment_count(ship) > 0;
}

bool ship_has_towed_pods(const ship_t *ship) {
    return ship_towed_pod_count(ship) > 0;
}

bool ship_has_towed_bodies(const ship_t *ship) {
    return ship_towed_body_count(ship) > 0;
}

int ship_upgrade_level(const ship_t* ship, ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return ship->mining_level;
        case SHIP_UPGRADE_HOLD:
            return ship->hold_level;
        case SHIP_UPGRADE_TRACTOR:
            return ship->tractor_level;
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

bool ship_upgrade_maxed(const ship_t* ship, ship_upgrade_t upgrade) {
    return ship_upgrade_level(ship, upgrade) >= SHIP_UPGRADE_MAX_LEVEL;
}

int ship_upgrade_cost(const ship_t* ship, ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(ship, upgrade);
    int tier = level + 1;
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return 180 + (tier * 110) + (level * level * 120);
        case SHIP_UPGRADE_HOLD:
            return 210 + (tier * 120) + (level * level * 135);
        case SHIP_UPGRADE_TRACTOR:
            return 160 + (tier * 100) + (level * level * 110);
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

product_t upgrade_required_product(ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_HOLD:
            return PRODUCT_FRAME;
        case SHIP_UPGRADE_MINING:
            return PRODUCT_LASER_MODULE;
        case SHIP_UPGRADE_TRACTOR:
            return PRODUCT_TRACTOR_MODULE;
        case SHIP_UPGRADE_COUNT:
        default:
            return PRODUCT_FRAME;
    }
}

float upgrade_product_cost(const ship_t* ship, ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(ship, upgrade);
    int next = level + 1;
    return UPGRADE_BASE_PRODUCT * (float)next;
}

const char* product_name(product_t product) {
    switch (product) {
        case PRODUCT_FRAME: return "Frames";
        case PRODUCT_LASER_MODULE: return "Laser Modules";
        case PRODUCT_TRACTOR_MODULE: return "Tractor Modules";
        case PRODUCT_COUNT:
        default: return "Products";
    }
}
