#include "cell_geometry.h"
#include "fixpoint.h"

#include <math.h>
#include <string.h>

enum {
    CELL_GRAPH_VERSION = 1,
    CELL_GRAPH_HEADER_BYTES = 8,
    CELL_GRAPH_NODE_BYTES = 18,
};

static const int16_t CELL_DQ[CELL_ORIENTATION_COUNT] = {
    1, 0, -1, -1, 0, 1,
};
static const int16_t CELL_DR[CELL_ORIENTATION_COUNT] = {
    0, 1, 1, 0, -1, -1,
};

static bool cell_shape_is_volume(cell_shape_t shape) {
    return shape == CELL_SHAPE_HEX || shape == CELL_SHAPE_REINFORCED_HEX;
}

static bool cell_role_is_directional(cell_role_t role) {
    return role == CELL_ROLE_ENGINE || role == CELL_ROLE_TOW ||
           role == CELL_ROLE_WEAPON || role == CELL_ROLE_SENSOR ||
           role == CELL_ROLE_BRACE;
}

int cell_orientation_normalize(int orientation) {
    int result = orientation % CELL_ORIENTATION_COUNT;
    return result < 0 ? result + CELL_ORIENTATION_COUNT : result;
}

cell_coord_t cell_coord_neighbor(cell_coord_t origin, int orientation) {
    int o = cell_orientation_normalize(orientation);
    return (cell_coord_t){
        .q = (int16_t)(origin.q + CELL_DQ[o]),
        .r = (int16_t)(origin.r + CELL_DR[o]),
    };
}

int cell_coord_distance(cell_coord_t a, cell_coord_t b) {
    int dq = (int)a.q - (int)b.q;
    int dr = (int)a.r - (int)b.r;
    int ds = -dq - dr;
    if (dq < 0) dq = -dq;
    if (dr < 0) dr = -dr;
    if (ds < 0) ds = -ds;
    return (dq + dr + ds) / 2;
}

cell_point_t cell_coord_world(cell_coord_t coord, float edge_length) {
    const float root3 = 1.7320508075688772f;
    return (cell_point_t){
        .x = edge_length * root3 *
             ((float)coord.q + 0.5f * (float)coord.r),
        .y = edge_length * 1.5f * (float)coord.r,
    };
}

cell_point_t cell_triangle_world(const cell_node_t *triangle,
                                 float edge_length) {
    if (!triangle || triangle->shape != CELL_SHAPE_TRIANGLE)
        return (cell_point_t){0.0f, 0.0f};
    cell_point_t host = cell_coord_world(triangle->coord, edge_length);
    float angle = (float)cell_orientation_normalize(triangle->orientation) *
                  1.0471975511965976f;
    /* Hex apothem + one third of an equilateral triangle altitude. */
    float offset = edge_length * 1.1547005383792515f;
    return (cell_point_t){
        .x = host.x + fixp_cosf(angle) * offset,
        .y = host.y + fixp_sinf(angle) * offset,
    };
}

cell_point_t cell_triangle_active_vector(const cell_node_t *triangle) {
    if (!triangle || triangle->shape != CELL_SHAPE_TRIANGLE)
        return (cell_point_t){0.0f, 0.0f};
    float angle = (float)cell_orientation_normalize(triangle->orientation) *
                  1.0471975511965976f;
    float sign = triangle->role == CELL_ROLE_ENGINE ? -1.0f : 1.0f;
    return (cell_point_t){fixp_cosf(angle) * sign,
                          fixp_sinf(angle) * sign};
}

cell_point_t cell_graph_center_of_mass(const cell_graph_t *graph) {
    cell_point_t center = {0.0f, 0.0f};
    float total_mass = 0.0f;
    if (!graph) return center;
    for (uint8_t i = 0; i < graph->count; i++) {
        const cell_node_t *node = &graph->nodes[i];
        cell_point_t p = node->shape == CELL_SHAPE_TRIANGLE
            ? cell_triangle_world(node, CELL_EDGE_LENGTH)
            : cell_coord_world(node->coord, CELL_EDGE_LENGTH);
        float mass = cell_shape_shell_mass((cell_shape_t)node->shape) +
                     (float)node->payload_units;
        center.x += p.x * mass;
        center.y += p.y * mass;
        total_mass += mass;
    }
    if (total_mass > 0.0f) {
        center.x /= total_mass;
        center.y /= total_mass;
    }
    return center;
}

int cell_shape_strut_cost(cell_shape_t shape) {
    switch (shape) {
    case CELL_SHAPE_TRIANGLE:       return CELL_TRIANGLE_STRUT_COST;
    case CELL_SHAPE_HEX:            return CELL_HEX_STRUT_COST;
    case CELL_SHAPE_REINFORCED_HEX: return CELL_REINFORCED_HEX_STRUT_COST;
    default:                        return 0;
    }
}

int cell_shape_payload_capacity(cell_shape_t shape) {
    switch (shape) {
    case CELL_SHAPE_HEX:            return CELL_HEX_PAYLOAD_CAPACITY;
    case CELL_SHAPE_REINFORCED_HEX: return CELL_REINFORCED_HEX_PAYLOAD_CAPACITY;
    default:                        return 0;
    }
}

float cell_shape_shell_mass(cell_shape_t shape) {
    /* Structural mass is expressed in conserved-strut mass units. */
    return (float)cell_shape_strut_cost(shape);
}

bool cell_nodes_join(const cell_node_t *a, const cell_node_t *b) {
    if (!a || !b) return false;
    cell_shape_t as = (cell_shape_t)a->shape;
    cell_shape_t bs = (cell_shape_t)b->shape;
    if (cell_shape_is_volume(as) && cell_shape_is_volume(bs))
        return cell_coord_distance(a->coord, b->coord) == 1;
    if (as == CELL_SHAPE_TRIANGLE && cell_shape_is_volume(bs))
        return a->coord.q == b->coord.q && a->coord.r == b->coord.r;
    if (bs == CELL_SHAPE_TRIANGLE && cell_shape_is_volume(as))
        return b->coord.q == a->coord.q && b->coord.r == a->coord.r;
    return false;
}

static bool cell_node_valid(const cell_node_t *node) {
    if (!node || node->identity == 0) return false;
    cell_shape_t shape = (cell_shape_t)node->shape;
    cell_role_t role = (cell_role_t)node->role;
    if (shape <= CELL_SHAPE_NONE || shape > CELL_SHAPE_REINFORCED_HEX)
        return false;
    if (role <= CELL_ROLE_NONE || role > CELL_ROLE_BRACE) return false;
    if (node->payload_units >
        (uint16_t)cell_shape_payload_capacity(shape)) return false;
    if (shape == CELL_SHAPE_TRIANGLE) {
        if (node->orientation >= CELL_ORIENTATION_COUNT) return false;
        if (!cell_role_is_directional(role)) return false;
    } else {
        if (node->orientation != 0) return false;
        if (cell_role_is_directional(role)) return false;
    }
    if (shape == CELL_SHAPE_REINFORCED_HEX && role != CELL_ROLE_HUB &&
        role != CELL_ROLE_CONTROL) return false;
    return true;
}

static bool cell_nodes_conflict(const cell_node_t *a, const cell_node_t *b) {
    if (a->identity == b->identity) return true;
    cell_shape_t as = (cell_shape_t)a->shape;
    cell_shape_t bs = (cell_shape_t)b->shape;
    if (cell_shape_is_volume(as) && cell_shape_is_volume(bs))
        return a->coord.q == b->coord.q && a->coord.r == b->coord.r;
    if (as == CELL_SHAPE_TRIANGLE && bs == CELL_SHAPE_TRIANGLE)
        return a->coord.q == b->coord.q && a->coord.r == b->coord.r &&
               a->orientation == b->orientation;
    return false;
}

bool cell_graph_validate(const cell_graph_t *graph) {
    if (!graph || graph->version != CELL_GRAPH_VERSION || graph->count == 0 ||
        graph->count > CELL_GRAPH_MAX_NODES) return false;
    for (uint8_t i = 0; i < graph->count; i++) {
        if (!cell_node_valid(&graph->nodes[i])) return false;
        for (uint8_t j = 0; j < i; j++)
            if (cell_nodes_conflict(&graph->nodes[i], &graph->nodes[j]))
                return false;
    }

    bool reached[CELL_GRAPH_MAX_NODES] = {false};
    uint8_t queue[CELL_GRAPH_MAX_NODES] = {0};
    uint8_t head = 0, tail = 0;
    reached[0] = true;
    queue[tail++] = 0;
    while (head < tail) {
        uint8_t i = queue[head++];
        for (uint8_t j = 0; j < graph->count; j++) {
            if (reached[j] || !cell_nodes_join(&graph->nodes[i],
                                                &graph->nodes[j])) continue;
            reached[j] = true;
            queue[tail++] = j;
        }
    }
    for (uint8_t i = 0; i < graph->count; i++)
        if (!reached[i]) return false;
    return true;
}

static cell_node_t authored_node(cell_layout_kind_t kind, int index,
                                 int q, int r, cell_shape_t shape,
                                 cell_role_t role, int orientation) {
    return (cell_node_t){
        .identity = ((uint64_t)(uint32_t)kind << 32) | (uint32_t)(index + 1),
        .coord = {(int16_t)q, (int16_t)r},
        .shape = (uint8_t)shape,
        .role = (uint8_t)role,
        .orientation = (uint8_t)orientation,
    };
}

static void authored_push(cell_graph_t *graph, cell_node_t node) {
    if (graph->count < CELL_GRAPH_MAX_NODES)
        graph->nodes[graph->count++] = node;
}

bool cell_graph_authored(cell_layout_kind_t kind, cell_graph_t *out) {
    if (!out || kind <= CELL_LAYOUT_NONE || kind >= CELL_LAYOUT_COUNT)
        return false;
    memset(out, 0, sizeof(*out));
    out->version = CELL_GRAPH_VERSION;
    out->kind = (uint8_t)kind;

#define PUSH(q_, r_, shape_, role_, orientation_) \
    authored_push(out, authored_node(kind, out->count, (q_), (r_), \
                                     (shape_), (role_), (orientation_)))
    switch (kind) {
    case CELL_LAYOUT_TUG:
        PUSH(0, 0, CELL_SHAPE_HEX, CELL_ROLE_CONTROL, 0);
        PUSH(0, 0, CELL_SHAPE_TRIANGLE, CELL_ROLE_ENGINE, 3);
        break;
    case CELL_LAYOUT_LIGHT_FREIGHTER:
        PUSH(0, 0, CELL_SHAPE_HEX, CELL_ROLE_CONTROL, 0);
        PUSH(1, 0, CELL_SHAPE_HEX, CELL_ROLE_CARGO, 0);
        PUSH(2, 0, CELL_SHAPE_HEX, CELL_ROLE_CARGO, 0);
        PUSH(0, 0, CELL_SHAPE_TRIANGLE, CELL_ROLE_ENGINE, 3);
        break;
    case CELL_LAYOUT_HEAVY_FREIGHTER:
        PUSH(0, 0, CELL_SHAPE_REINFORCED_HEX, CELL_ROLE_CONTROL, 0);
        for (int o = 0; o < CELL_ORIENTATION_COUNT; o++) {
            cell_coord_t c = cell_coord_neighbor((cell_coord_t){0, 0}, o);
            PUSH(c.q, c.r, CELL_SHAPE_HEX, CELL_ROLE_CARGO, 0);
        }
        PUSH(-1, 0, CELL_SHAPE_TRIANGLE, CELL_ROLE_ENGINE, 3);
        PUSH(0, -1, CELL_SHAPE_TRIANGLE, CELL_ROLE_ENGINE, 4);
        PUSH(-1, 1, CELL_SHAPE_TRIANGLE, CELL_ROLE_ENGINE, 2);
        break;
    case CELL_LAYOUT_UTILITY:
        PUSH(0, 0, CELL_SHAPE_HEX, CELL_ROLE_CONTROL, 0);
        PUSH(0, 0, CELL_SHAPE_TRIANGLE, CELL_ROLE_ENGINE, 3);
        PUSH(0, 0, CELL_SHAPE_TRIANGLE, CELL_ROLE_TOW, 0);
        PUSH(0, 0, CELL_SHAPE_TRIANGLE, CELL_ROLE_SENSOR, 5);
        break;
    case CELL_LAYOUT_STATION_HUB_7:
        PUSH(0, 0, CELL_SHAPE_REINFORCED_HEX, CELL_ROLE_HUB, 0);
        for (int o = 0; o < CELL_ORIENTATION_COUNT; o++) {
            cell_coord_t c = cell_coord_neighbor((cell_coord_t){0, 0}, o);
            cell_role_t role = (o == 0) ? CELL_ROLE_CARGO :
                               (o == 3) ? CELL_ROLE_HABITAT :
                                          CELL_ROLE_SYSTEM;
            PUSH(c.q, c.r, CELL_SHAPE_HEX, role, 0);
        }
        break;
    default:
        return false;
    }
#undef PUSH
    return cell_graph_validate(out);
}

bool cell_graph_add_node(cell_graph_t *graph, const cell_node_t *node) {
    if (!graph || !node || graph->count >= CELL_GRAPH_MAX_NODES) return false;
    cell_graph_t candidate = *graph;
    candidate.nodes[candidate.count++] = *node;
    if (!cell_graph_validate(&candidate)) return false;
    *graph = candidate;
    return true;
}

bool cell_graph_remove_node(cell_graph_t *graph, uint64_t identity,
                            cell_node_t *detached) {
    if (!graph || identity == 0 || graph->count <= 1) return false;
    int found = -1;
    for (uint8_t i = 0; i < graph->count; i++) {
        if (graph->nodes[i].identity == identity) {
            found = i;
            break;
        }
    }
    if (found < 0) return false;
    /* The authored control/hub cell is the graph root.  Removing it is a
     * whole-assembly destruction event, not a detachable-cell mutation;
     * callers must resolve that through the shear/breakup path instead. */
    cell_role_t removed_role = (cell_role_t)graph->nodes[found].role;
    if (removed_role == CELL_ROLE_CONTROL || removed_role == CELL_ROLE_HUB)
        return false;
    cell_graph_t candidate = *graph;
    cell_node_t removed = candidate.nodes[found];
    for (uint8_t i = (uint8_t)found; i + 1 < candidate.count; i++)
        candidate.nodes[i] = candidate.nodes[i + 1];
    memset(&candidate.nodes[candidate.count - 1], 0,
           sizeof(candidate.nodes[0]));
    candidate.count--;
    if (!cell_graph_validate(&candidate)) return false;
    *graph = candidate;
    if (detached) *detached = removed;
    return true;
}

int cell_graph_role_count(const cell_graph_t *graph, cell_role_t role) {
    if (!graph) return 0;
    int count = 0;
    for (uint8_t i = 0; i < graph->count; i++)
        if ((cell_role_t)graph->nodes[i].role == role) count++;
    return count;
}

void cell_graph_totals(const cell_graph_t *graph, cell_graph_totals_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!graph) return;
    for (uint8_t i = 0; i < graph->count; i++) {
        const cell_node_t *node = &graph->nodes[i];
        cell_shape_t shape = (cell_shape_t)node->shape;
        cell_role_t role = (cell_role_t)node->role;
        out->struts += cell_shape_strut_cost(shape);
        out->cargo_capacity += cell_shape_payload_capacity(shape);
        out->payload_units += node->payload_units;
        out->shell_mass += cell_shape_shell_mass(shape);
        out->payload_mass += (float)node->payload_units;
        if (cell_role_is_directional(role)) out->active_modules++;
        if (role == CELL_ROLE_ENGINE) out->thrust_units += 1.0f;
    }
    out->total_mass = out->shell_mass + out->payload_mass;
}

cell_matter_cost_t cell_matter_cost_for_struts(int struts) {
    if (struts < 0) struts = 0;
    return (cell_matter_cost_t){
        .struts = struts,
        .ingots_to_press =
            (struts + CELL_STRUTS_PER_INGOT - 1) / CELL_STRUTS_PER_INGOT,
        .fragments_to_smelt =
            (struts + CELL_STRUTS_PER_FRAGMENT - 1) /
            CELL_STRUTS_PER_FRAGMENT,
        .fragment_equivalent =
            (float)struts / (float)CELL_STRUTS_PER_FRAGMENT,
    };
}

cell_matter_cost_t cell_graph_matter_cost(const cell_graph_t *graph) {
    cell_graph_totals_t totals;
    cell_graph_totals(graph, &totals);
    return cell_matter_cost_for_struts(totals.struts);
}

static void write_u16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void write_u64(uint8_t *out, uint64_t value) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (8 * i));
}

static uint16_t read_u16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint64_t read_u64(const uint8_t *data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value |= (uint64_t)data[i] << (8 * i);
    return value;
}

size_t cell_graph_encoded_size(const cell_graph_t *graph) {
    if (!graph || graph->count > CELL_GRAPH_MAX_NODES) return 0;
    return CELL_GRAPH_HEADER_BYTES + (size_t)graph->count * CELL_GRAPH_NODE_BYTES;
}

bool cell_graph_encode(const cell_graph_t *graph, uint8_t *out, size_t cap,
                       size_t *written) {
    if (written) *written = 0;
    if (!cell_graph_validate(graph) || !out) return false;
    size_t need = cell_graph_encoded_size(graph);
    if (cap < need) return false;
    out[0] = 'C'; out[1] = 'E'; out[2] = 'L'; out[3] = 'L';
    out[4] = graph->version;
    out[5] = graph->kind;
    out[6] = graph->count;
    out[7] = 0;
    size_t off = CELL_GRAPH_HEADER_BYTES;
    for (uint8_t i = 0; i < graph->count; i++) {
        const cell_node_t *node = &graph->nodes[i];
        write_u64(&out[off], node->identity); off += 8;
        write_u16(&out[off], (uint16_t)node->coord.q); off += 2;
        write_u16(&out[off], (uint16_t)node->coord.r); off += 2;
        write_u16(&out[off], node->payload_units); off += 2;
        out[off++] = node->shape;
        out[off++] = node->role;
        out[off++] = node->orientation;
        out[off++] = node->flags;
    }
    if (written) *written = off;
    return true;
}

bool cell_graph_decode(const uint8_t *data, size_t len, cell_graph_t *out,
                       size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!data || !out || len < CELL_GRAPH_HEADER_BYTES || data[0] != 'C' ||
        data[1] != 'E' || data[2] != 'L' || data[3] != 'L' ||
        data[4] != CELL_GRAPH_VERSION || data[6] == 0 ||
        data[6] > CELL_GRAPH_MAX_NODES) return false;
    size_t need = CELL_GRAPH_HEADER_BYTES +
                  (size_t)data[6] * CELL_GRAPH_NODE_BYTES;
    if (len < need) return false;
    cell_graph_t graph;
    memset(&graph, 0, sizeof(graph));
    graph.version = data[4];
    graph.kind = data[5];
    graph.count = data[6];
    size_t off = CELL_GRAPH_HEADER_BYTES;
    for (uint8_t i = 0; i < graph.count; i++) {
        cell_node_t *node = &graph.nodes[i];
        node->identity = read_u64(&data[off]); off += 8;
        node->coord.q = (int16_t)read_u16(&data[off]); off += 2;
        node->coord.r = (int16_t)read_u16(&data[off]); off += 2;
        node->payload_units = read_u16(&data[off]); off += 2;
        node->shape = data[off++];
        node->role = data[off++];
        node->orientation = data[off++];
        node->flags = data[off++];
    }
    if (!cell_graph_validate(&graph)) return false;
    *out = graph;
    if (consumed) *consumed = off;
    return true;
}
