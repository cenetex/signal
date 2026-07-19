#include "cell_stress.h"

#include <math.h>
#include <string.h>

enum { CELL_STRESS_HEADER_BYTES = 8, CELL_STRESS_JOIN_BYTES = 24 };

static int node_index_for_identity(const cell_graph_t *graph, uint64_t id) {
    for (uint8_t i = 0; graph && i < graph->count; i++)
        if (graph->nodes[i].identity == id) return i;
    return -1;
}

static bool join_is_hub(const cell_graph_t *graph,
                        const cell_join_stress_t *join) {
    int a = node_index_for_identity(graph, join->a);
    int b = node_index_for_identity(graph, join->b);
    return (a >= 0 && graph->nodes[a].shape == CELL_SHAPE_REINFORCED_HEX) ||
           (b >= 0 && graph->nodes[b].shape == CELL_SHAPE_REINFORCED_HEX);
}

static bool join_is_triangle(const cell_graph_t *graph,
                             const cell_join_stress_t *join) {
    int a = node_index_for_identity(graph, join->a);
    int b = node_index_for_identity(graph, join->b);
    return (a >= 0 && graph->nodes[a].shape == CELL_SHAPE_TRIANGLE) ||
           (b >= 0 && graph->nodes[b].shape == CELL_SHAPE_TRIANGLE);
}

static float join_failure_threshold(const cell_graph_t *graph,
                                    const cell_join_stress_t *join) {
    if (join_is_hub(graph, join)) return (float)CELL_STRESS_HUB_FAILURE;
    if (join_is_triangle(graph, join))
        return (float)CELL_STRESS_TRIANGLE_FAILURE;
    return (float)CELL_STRESS_STANDARD_FAILURE;
}

bool cell_stress_init(const cell_graph_t *graph, cell_stress_state_t *out) {
    if (!cell_graph_validate(graph) || !out) return false;
    memset(out, 0, sizeof(*out));
    out->version = CELL_STRESS_VERSION;
    for (uint8_t i = 0; i < graph->count; i++) {
        for (uint8_t j = i + 1; j < graph->count; j++) {
            if (!cell_nodes_join(&graph->nodes[i], &graph->nodes[j])) continue;
            if (out->join_count >= CELL_STRESS_MAX_JOINS) return false;
            cell_join_stress_t *join = &out->joins[out->join_count++];
            join->a = graph->nodes[i].identity;
            join->b = graph->nodes[j].identity;
        }
    }
    return true;
}

static bool stress_state_matches(const cell_graph_t *graph,
                                 const cell_stress_state_t *state) {
    if (!graph || !state || state->version != CELL_STRESS_VERSION ||
        state->join_count > CELL_STRESS_MAX_JOINS) return false;
    for (uint8_t i = 0; i < state->join_count; i++) {
        if (node_index_for_identity(graph, state->joins[i].a) < 0 ||
            node_index_for_identity(graph, state->joins[i].b) < 0)
            return false;
    }
    return true;
}

static int root_index(const cell_graph_t *graph) {
    for (uint8_t i = 0; graph && i < graph->count; i++) {
        cell_role_t role = (cell_role_t)graph->nodes[i].role;
        if (role == CELL_ROLE_CONTROL || role == CELL_ROLE_HUB) return i;
    }
    return 0;
}

static bool join_failed_between(const cell_stress_state_t *state,
                                uint64_t a, uint64_t b) {
    for (uint8_t i = 0; state && i < state->join_count; i++) {
        const cell_join_stress_t *join = &state->joins[i];
        if (!join->failed) continue;
        if ((join->a == a && join->b == b) ||
            (join->a == b && join->b == a)) return true;
    }
    return false;
}

static void graph_reach(const cell_graph_t *graph,
                        const cell_stress_state_t *state,
                        int start, bool reached[CELL_GRAPH_MAX_NODES]) {
    uint8_t queue[CELL_GRAPH_MAX_NODES] = {0};
    uint8_t head = 0, tail = 0;
    reached[start] = true;
    queue[tail++] = (uint8_t)start;
    while (head < tail) {
        int i = queue[head++];
        for (uint8_t j = 0; j < graph->count; j++) {
            if (reached[j] || !cell_nodes_join(&graph->nodes[i],
                                                &graph->nodes[j]) ||
                join_failed_between(state, graph->nodes[i].identity,
                                     graph->nodes[j].identity)) continue;
            reached[j] = true;
            queue[tail++] = j;
        }
    }
}

static cell_graph_t graph_component(const cell_graph_t *graph,
                                    const bool include[CELL_GRAPH_MAX_NODES]) {
    cell_graph_t out = {.version = 1, .kind = CELL_LAYOUT_NONE};
    for (uint8_t i = 0; graph && i < graph->count; i++)
        if (include[i]) out.nodes[out.count++] = graph->nodes[i];
    return out;
}

bool cell_stress_apply_impact(const cell_graph_t *graph,
                              cell_stress_state_t *state,
                              const cell_impact_t *impact,
                              cell_shear_result_t *out) {
    if (!cell_graph_validate(graph) || !state || !impact || !out ||
        !isfinite(impact->impulse) || impact->impulse <= 0.0f ||
        node_index_for_identity(graph, impact->impacted_identity) < 0)
        return false;
    if (!stress_state_matches(graph, state) &&
        !cell_stress_init(graph, state)) return false;
    memset(out, 0, sizeof(*out));
    out->remaining = *graph;

    /* Graph distance from the impact bounds propagation. */
    int distance[CELL_GRAPH_MAX_NODES];
    for (int i = 0; i < CELL_GRAPH_MAX_NODES; i++) distance[i] = -1;
    int impacted = node_index_for_identity(graph, impact->impacted_identity);
    uint8_t queue[CELL_GRAPH_MAX_NODES] = {(uint8_t)impacted};
    uint8_t head = 0, tail = 1;
    distance[impacted] = 0;
    while (head < tail) {
        int i = queue[head++];
        for (uint8_t j = 0; j < graph->count; j++) {
            if (distance[j] >= 0 ||
                !cell_nodes_join(&graph->nodes[i], &graph->nodes[j])) continue;
            distance[j] = distance[i] + 1;
            queue[tail++] = j;
        }
    }

    int fail_index = -1;
    float fail_ratio = 1.0f;
    for (uint8_t i = 0; i < state->join_count; i++) {
        cell_join_stress_t *join = &state->joins[i];
        if (join->failed) continue;
        int a = node_index_for_identity(graph, join->a);
        int b = node_index_for_identity(graph, join->b);
        int d = distance[a] < distance[b] ? distance[a] : distance[b];
        if (d < 0) continue;
        float attenuation = 1.0f;
        for (int step = 0; step < d; step++) attenuation *= 0.5f;
        join->stress += impact->impulse * attenuation;
        if (join_is_hub(graph, join)) {
            int stage = (int)(join->stress / (float)CELL_STRESS_HUB_STAGE);
            join->stage = (uint8_t)(stage > 2 ? 2 : stage);
        }
        float threshold = join_failure_threshold(graph, join);
        float ratio = join->stress / threshold;
        if (ratio >= fail_ratio) {
            fail_ratio = ratio;
            fail_index = i;
        }
    }
    if (fail_index < 0) return true;

    cell_join_stress_t *failed = &state->joins[fail_index];
    failed->failed = 1;
    out->failed_a = failed->a;
    out->failed_b = failed->b;
    out->failed_join_stage = failed->stage;

    bool root_reached[CELL_GRAPH_MAX_NODES] = {false};
    graph_reach(graph, state, root_index(graph), root_reached);
    if (root_reached[impacted]) return true; /* staged crack, no free body yet */

    bool detached[CELL_GRAPH_MAX_NODES] = {false};
    graph_reach(graph, state, impacted, detached);
    bool remaining[CELL_GRAPH_MAX_NODES] = {false};
    for (uint8_t i = 0; i < graph->count; i++) remaining[i] = !detached[i];
    out->remaining = graph_component(graph, remaining);
    out->salvage.graph = graph_component(graph, detached);
    if (!cell_graph_validate(&out->remaining) ||
        !cell_graph_validate(&out->salvage.graph)) return false;

    cell_graph_totals_t totals;
    cell_graph_totals(&out->salvage.graph, &totals);
    vec2 normal = v2_len_sq(impact->normal) > 0.0001f
        ? v2_norm(impact->normal) : v2(1.0f, 0.0f);
    float mass = totals.total_mass > 0.0f ? totals.total_mass : 1.0f;
    out->salvage.active = true;
    out->salvage.provenance = impact->provenance == CELL_PROVENANCE_KNOWN
        ? CELL_PROVENANCE_KNOWN : CELL_PROVENANCE_UNKNOWN;
    out->salvage.pos = impact->point;
    out->salvage.vel = v2_add(impact->assembly_velocity,
                              v2_scale(normal, impact->impulse / mass));
    out->salvage.rotation = impact->assembly_rotation;
    out->salvage.spin = impact->assembly_spin;
    if (out->salvage.provenance == CELL_PROVENANCE_KNOWN) {
        memcpy(out->salvage.shell_manifest_root,
               impact->shell_manifest_root, 32);
        memcpy(out->salvage.payload_manifest_root,
               impact->payload_manifest_root, 32);
    }
    out->sheared = true;
    return true;
}

bool cell_stress_reattach(cell_graph_t *remaining,
                          const cell_salvage_t *salvage,
                          cell_stress_state_t *state) {
    if (!cell_graph_validate(remaining) || !salvage || !salvage->active ||
        !cell_graph_validate(&salvage->graph) || !state) return false;
    if ((int)remaining->count + (int)salvage->graph.count >
        CELL_GRAPH_MAX_NODES) return false;
    cell_graph_t merged = *remaining;
    /* Validate the completed weld as one transaction.  A multi-cell salvage
     * component may be ordered from its free end inward, so demanding that
     * every intermediate append already touch the remaining graph would make
     * a physically valid repair depend on serialized node order. */
    for (uint8_t i = 0; i < salvage->graph.count; i++)
        merged.nodes[merged.count++] = salvage->graph.nodes[i];
    if (!cell_graph_validate(&merged)) return false;
    if (!cell_stress_init(&merged, state)) return false;
    *remaining = merged;
    return true;
}

static void write_u16(uint8_t *out, uint16_t v) {
    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
}
static uint16_t read_u16(const uint8_t *in) {
    return (uint16_t)in[0] | ((uint16_t)in[1] << 8);
}
static void write_u32(uint8_t *out, uint32_t v) {
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(v >> (8 * i));
}
static uint32_t read_u32(const uint8_t *in) {
    uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)in[i] << (8*i);
    return v;
}
static void write_u64(uint8_t *out, uint64_t v) {
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t read_u64(const uint8_t *in) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)in[i] << (8*i);
    return v;
}
static void write_f32(uint8_t *out, float f) {
    uint32_t bits; memcpy(&bits, &f, 4); write_u32(out, bits);
}
static float read_f32(const uint8_t *in) {
    uint32_t bits = read_u32(in); float f; memcpy(&f, &bits, 4); return f;
}

size_t cell_stress_encoded_size(const cell_stress_state_t *state) {
    return state && state->join_count <= CELL_STRESS_MAX_JOINS
        ? CELL_STRESS_HEADER_BYTES +
          (size_t)state->join_count * CELL_STRESS_JOIN_BYTES : 0;
}

bool cell_stress_encode(const cell_stress_state_t *state,
                        uint8_t *out, size_t cap, size_t *written) {
    if (written) *written = 0;
    if (!state || state->version != CELL_STRESS_VERSION || !out) return false;
    size_t need = cell_stress_encoded_size(state);
    if (need == 0 || cap < need) return false;
    memcpy(out, "STR1", 4); out[4] = state->version;
    out[5] = state->join_count; out[6] = out[7] = 0;
    size_t off = CELL_STRESS_HEADER_BYTES;
    for (uint8_t i = 0; i < state->join_count; i++) {
        const cell_join_stress_t *j = &state->joins[i];
        write_u64(&out[off], j->a); off += 8;
        write_u64(&out[off], j->b); off += 8;
        write_f32(&out[off], j->stress); off += 4;
        out[off++] = j->stage; out[off++] = j->failed;
        out[off++] = 0; out[off++] = 0;
    }
    if (written) *written = off;
    return true;
}

bool cell_stress_decode(const uint8_t *data, size_t len,
                        cell_stress_state_t *out, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!data || !out || len < CELL_STRESS_HEADER_BYTES ||
        memcmp(data, "STR1", 4) != 0 || data[4] != CELL_STRESS_VERSION ||
        data[5] > CELL_STRESS_MAX_JOINS) return false;
    size_t need = CELL_STRESS_HEADER_BYTES +
                  (size_t)data[5] * CELL_STRESS_JOIN_BYTES;
    if (len < need) return false;
    cell_stress_state_t state = {.version = data[4], .join_count = data[5]};
    size_t off = CELL_STRESS_HEADER_BYTES;
    for (uint8_t i = 0; i < state.join_count; i++) {
        cell_join_stress_t *j = &state.joins[i];
        j->a = read_u64(&data[off]); off += 8;
        j->b = read_u64(&data[off]); off += 8;
        j->stress = read_f32(&data[off]); off += 4;
        j->stage = data[off++]; j->failed = data[off++]; off += 2;
        if (!isfinite(j->stress) || j->stress < 0.0f || j->stage > 2 ||
            j->failed > 1) return false;
    }
    *out = state; if (consumed) *consumed = off; return true;
}

size_t cell_salvage_encoded_size(const cell_salvage_t *salvage) {
    size_t graph_size = salvage ? cell_graph_encoded_size(&salvage->graph) : 0;
    return salvage && salvage->active && graph_size > 0
        ? CELL_SALVAGE_HEADER_BYTES + graph_size : 0;
}

bool cell_salvage_encode(const cell_salvage_t *salvage,
                         uint8_t *out, size_t cap, size_t *written) {
    if (written) *written = 0;
    size_t need = cell_salvage_encoded_size(salvage);
    if (!out || need == 0 || cap < need ||
        salvage->provenance > CELL_PROVENANCE_KNOWN) return false;
    size_t graph_size = cell_graph_encoded_size(&salvage->graph);
    memcpy(out, "SAL1", 4); out[4] = 1; out[5] = salvage->provenance;
    write_u16(&out[6], (uint16_t)graph_size);
    write_f32(&out[8], salvage->pos.x); write_f32(&out[12], salvage->pos.y);
    write_f32(&out[16], salvage->vel.x); write_f32(&out[20], salvage->vel.y);
    write_f32(&out[24], salvage->rotation); write_f32(&out[28], salvage->spin);
    memcpy(&out[32], salvage->shell_manifest_root, 32);
    memcpy(&out[64], salvage->payload_manifest_root, 32);
    size_t graph_written = 0;
    if (!cell_graph_encode(&salvage->graph, &out[96], cap - 96,
                           &graph_written) || graph_written != graph_size)
        return false;
    if (written) *written = 96 + graph_written;
    return true;
}

bool cell_salvage_decode(const uint8_t *data, size_t len,
                         cell_salvage_t *out, size_t *consumed) {
    if (consumed) *consumed = 0;
    if (!data || !out || len < CELL_SALVAGE_HEADER_BYTES ||
        memcmp(data, "SAL1", 4) != 0 || data[4] != 1 ||
        data[5] > CELL_PROVENANCE_KNOWN) return false;
    uint16_t graph_size = read_u16(&data[6]);
    if (len < CELL_SALVAGE_HEADER_BYTES + graph_size) return false;
    cell_salvage_t salvage = {.active = true, .provenance = data[5]};
    salvage.pos = v2(read_f32(&data[8]), read_f32(&data[12]));
    salvage.vel = v2(read_f32(&data[16]), read_f32(&data[20]));
    salvage.rotation = read_f32(&data[24]); salvage.spin = read_f32(&data[28]);
    if (!isfinite(salvage.pos.x) || !isfinite(salvage.pos.y) ||
        !isfinite(salvage.vel.x) || !isfinite(salvage.vel.y) ||
        !isfinite(salvage.rotation) || !isfinite(salvage.spin)) return false;
    memcpy(salvage.shell_manifest_root, &data[32], 32);
    memcpy(salvage.payload_manifest_root, &data[64], 32);
    size_t graph_consumed = 0;
    if (!cell_graph_decode(&data[96], graph_size, &salvage.graph,
                           &graph_consumed) || graph_consumed != graph_size)
        return false;
    *out = salvage;
    if (consumed) *consumed = 96 + graph_consumed;
    return true;
}
