/*
 * cell_geometry.h — canonical one-size 60-degree construction grammar.
 *
 * This is the shared design authority for rock-derived structure.  It is
 * intentionally independent of ship_t and station_t so authored layouts,
 * save/wire codecs, rendering, and deterministic tests can all use the same
 * lattice without inheriting gameplay-state headers.
 */
#ifndef CELL_GEOMETRY_H
#define CELL_GEOMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CELL_ORIENTATION_COUNT = 6,
    CELL_GRAPH_MAX_NODES = 19,

    /* Visible matter algebra: one fragment becomes four named ingots and
     * each ingot fans out into four conserved structural struts. */
    CELL_INGOTS_PER_FRAGMENT = 4,
    CELL_STRUTS_PER_INGOT = 4,
    CELL_STRUTS_PER_FRAGMENT = 16,

    CELL_TRIANGLE_STRUT_COST = 3,
    CELL_HEX_STRUT_COST = 6,
    CELL_REINFORCED_HEX_STRUT_COST = 12,

    /* One ordinary volume cell matches the starter ship's 24-unit hold and
     * divides evenly across its six visible occupancy rails. */
    CELL_HEX_PAYLOAD_CAPACITY = 24,
    CELL_REINFORCED_HEX_PAYLOAD_CAPACITY = 12,
};

/* One physical edge length everywhere.  Larger assemblies are cell graphs or
 * render LOD groupings; they are never scaled-up primitive cells. */
#define CELL_EDGE_LENGTH 18.0f

typedef enum {
    CELL_SHAPE_NONE = 0,
    CELL_SHAPE_TRIANGLE,
    CELL_SHAPE_HEX,
    CELL_SHAPE_REINFORCED_HEX,
} cell_shape_t;

typedef enum {
    CELL_ROLE_NONE = 0,
    CELL_ROLE_CONTROL,
    CELL_ROLE_CARGO,
    CELL_ROLE_SYSTEM,
    CELL_ROLE_HABITAT,
    CELL_ROLE_HUB,
    CELL_ROLE_ENGINE,
    CELL_ROLE_TOW,
    CELL_ROLE_WEAPON,
    CELL_ROLE_SENSOR,
    CELL_ROLE_BRACE,
} cell_role_t;

typedef enum {
    CELL_LAYOUT_NONE = 0,
    CELL_LAYOUT_TUG,
    CELL_LAYOUT_LIGHT_FREIGHTER,
    CELL_LAYOUT_HEAVY_FREIGHTER,
    CELL_LAYOUT_UTILITY,
    CELL_LAYOUT_STATION_HUB_7,
    CELL_LAYOUT_COUNT,
} cell_layout_kind_t;

typedef struct {
    int16_t q;
    int16_t r;
} cell_coord_t;

typedef struct {
    float x;
    float y;
} cell_point_t;

/* A triangle occupies a named edge slot on its host volume cell: q/r names
 * the host and orientation names one of its six complete edges.  Hex cells
 * occupy the axial q/r coordinate and always serialize with orientation 0. */
typedef struct {
    uint64_t identity;
    cell_coord_t coord;
    uint16_t payload_units;
    uint8_t shape;       /* cell_shape_t */
    uint8_t role;        /* cell_role_t */
    uint8_t orientation; /* 0..5, clockwise from +X; triangles only */
    uint8_t flags;
} cell_node_t;

typedef struct {
    uint8_t version;
    uint8_t kind; /* cell_layout_kind_t for authored graphs, NONE otherwise */
    uint8_t count;
    uint8_t _reserved;
    cell_node_t nodes[CELL_GRAPH_MAX_NODES];
} cell_graph_t;

typedef struct {
    int struts;
    int cargo_capacity;
    int payload_units;
    int active_modules;
    float shell_mass;
    float payload_mass;
    float total_mass;
    float thrust_units;
} cell_graph_totals_t;

typedef struct {
    int struts;
    int ingots_to_press;
    int fragments_to_smelt;
    float fragment_equivalent;
} cell_matter_cost_t;

int cell_orientation_normalize(int orientation);
cell_coord_t cell_coord_neighbor(cell_coord_t origin, int orientation);
int cell_coord_distance(cell_coord_t a, cell_coord_t b);
cell_point_t cell_coord_world(cell_coord_t coord, float edge_length);
cell_point_t cell_triangle_world(const cell_node_t *triangle,
                                 float edge_length);
cell_point_t cell_triangle_active_vector(const cell_node_t *triangle);
cell_point_t cell_graph_center_of_mass(const cell_graph_t *graph);

int cell_shape_strut_cost(cell_shape_t shape);
int cell_shape_payload_capacity(cell_shape_t shape);
float cell_shape_shell_mass(cell_shape_t shape);
bool cell_nodes_join(const cell_node_t *a, const cell_node_t *b);

bool cell_graph_validate(const cell_graph_t *graph);
bool cell_graph_authored(cell_layout_kind_t kind, cell_graph_t *out);
bool cell_graph_add_node(cell_graph_t *graph, const cell_node_t *node);
bool cell_graph_remove_node(cell_graph_t *graph, uint64_t identity,
                            cell_node_t *detached);
void cell_graph_totals(const cell_graph_t *graph, cell_graph_totals_t *out);
int cell_graph_role_count(const cell_graph_t *graph, cell_role_t role);
cell_matter_cost_t cell_matter_cost_for_struts(int struts);
cell_matter_cost_t cell_graph_matter_cost(const cell_graph_t *graph);

/* Canonical little-endian encoding.  It is deliberately byte-oriented and
 * free of struct padding so equivalent layouts have identical bytes on native
 * and wasm builds. */
size_t cell_graph_encoded_size(const cell_graph_t *graph);
bool cell_graph_encode(const cell_graph_t *graph, uint8_t *out, size_t cap,
                       size_t *written);
bool cell_graph_decode(const uint8_t *data, size_t len, cell_graph_t *out,
                       size_t *consumed);

#endif /* CELL_GEOMETRY_H */
