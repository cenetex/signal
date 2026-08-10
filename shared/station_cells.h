/*
 * station_cells.h — deterministic station-module projection onto the
 * canonical 60-degree cell lattice.
 *
 * Existing station catalogs persist module identity as ordered module records
 * (type/ring/slot).  This adapter makes that topology physical without adding
 * a second mutable source of truth: the same ordered records deterministically
 * round-trip to the same axial graph on native, wire-mirrored, and wasm builds.
 * types.h includes this header only after station_t is complete.
 */
#ifndef STATION_CELLS_H
#define STATION_CELLS_H

#include <string.h>

enum {
    CELL_NODE_FLAG_SCAFFOLD = 1u << 0,
};

static inline cell_role_t station_cell_role_for_module(module_type_t type) {
    switch (type) {
    case MODULE_DOCK:        return CELL_ROLE_TOW;
    case MODULE_HOPPER:      return CELL_ROLE_CARGO;
    case MODULE_SIGNAL_RELAY:return CELL_ROLE_SENSOR;
    case MODULE_REPAIR_BAY:  return CELL_ROLE_HABITAT;
    case MODULE_FURNACE:
    case MODULE_FRAME_PRESS:
    case MODULE_LASER_FAB:
    case MODULE_TRACTOR_FAB:
    case MODULE_ENGINE_FAB:
    case MODULE_SHIPYARD:    return CELL_ROLE_SYSTEM;
    default:                 return CELL_ROLE_SYSTEM;
    }
}

/* First six cells complete the hub flower.  Further modules occupy a
 * connected radius-two arc; MAX_MODULES_PER_STATION=16 fits the table. */
static inline cell_coord_t station_cell_coord_for_ordinal(int ordinal) {
    static const cell_coord_t coords[MAX_MODULES_PER_STATION] = {
        { 1, 0}, { 0, 1}, {-1, 1}, {-1, 0}, { 0,-1}, { 1,-1},
        { 2, 0}, { 1, 1}, { 0, 2}, {-1, 2}, {-2, 2}, {-2, 1},
        {-2, 0}, {-1,-1}, { 0,-2}, { 1,-2},
    };
    if (ordinal < 0) ordinal = 0;
    if (ordinal >= MAX_MODULES_PER_STATION)
        ordinal = MAX_MODULES_PER_STATION - 1;
    return coords[ordinal];
}

static inline bool station_cell_graph(const station_t *station,
                                      cell_graph_t *out) {
    if (!station || !out) return false;
    memset(out, 0, sizeof(*out));
    out->version = 1;
    out->kind = CELL_LAYOUT_STATION_HUB_7;

    uint64_t owner = station->id ? (uint64_t)station->id : 1u;
    out->nodes[out->count++] = (cell_node_t){
        .identity = (owner << 32) | 1u,
        .shape = CELL_SHAPE_REINFORCED_HEX,
        .role = CELL_ROLE_HUB,
    };

    int module_count = station->module_count;
    if (module_count < 0) module_count = 0;
    if (module_count > MAX_MODULES_PER_STATION)
        module_count = MAX_MODULES_PER_STATION;
    int volume_ordinal = 0;
    int directional_ordinal = 0;
    for (int i = 0; i < module_count; i++) {
        const station_module_t *module = &station->modules[i];
        float payload = module->input_buffer + module->output_buffer;
        if (payload < 0.0f) payload = 0.0f;
        if (payload > (float)UINT16_MAX) payload = (float)UINT16_MAX;
        bool directional = (module->type == MODULE_DOCK ||
                            module->type == MODULE_SIGNAL_RELAY) &&
                           directional_ordinal < CELL_ORIENTATION_COUNT;
        cell_shape_t shape = directional ? CELL_SHAPE_TRIANGLE
                                         : CELL_SHAPE_HEX;
        float visible_capacity = (float)cell_shape_payload_capacity(shape);
        if (payload > visible_capacity) payload = visible_capacity;
        cell_role_t role = directional
            ? station_cell_role_for_module(module->type)
            : (module->type == MODULE_HOPPER ? CELL_ROLE_CARGO
                                             : CELL_ROLE_SYSTEM);
        cell_coord_t coord = directional
            ? (cell_coord_t){0, 0}
            : station_cell_coord_for_ordinal(volume_ordinal++);
        uint8_t orientation = directional
            ? (uint8_t)directional_ordinal++ : 0;
        out->nodes[out->count++] = (cell_node_t){
            .identity = (owner << 32) | (uint64_t)(i + 2),
            .coord = coord,
            .payload_units = shape == CELL_SHAPE_HEX ? (uint16_t)payload : 0,
            .shape = shape,
            .role = role,
            .orientation = orientation,
            .flags = module->scaffold ? CELL_NODE_FLAG_SCAFFOLD : 0,
        };
    }
    return cell_graph_validate(out);
}

/* Low identity bits are intentionally the stable module-record index + 2. */
static inline int station_cell_module_index(const cell_node_t *node) {
    if (!node || node->role == CELL_ROLE_HUB) return -1;
    uint32_t local = (uint32_t)node->identity;
    return (local >= 2u && local <= (uint32_t)MAX_MODULES_PER_STATION + 1u)
        ? (int)local - 2 : -1;
}

#endif /* STATION_CELLS_H */
