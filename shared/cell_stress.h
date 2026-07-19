/* cell_stress.h — bounded join stress, deterministic shear, and salvage. */
#ifndef CELL_STRESS_H
#define CELL_STRESS_H

#include "cell_geometry.h"
#include "math_util.h"

enum {
    CELL_STRESS_VERSION = 1,
    CELL_STRESS_MAX_JOINS = 72,
    CELL_STRESS_STANDARD_FAILURE = 120,
    CELL_STRESS_TRIANGLE_FAILURE = 80,
    CELL_STRESS_HUB_STAGE = 120,
    CELL_STRESS_HUB_FAILURE = 360,
    CELL_SALVAGE_HEADER_BYTES = 96,
};

typedef enum {
    CELL_PROVENANCE_UNKNOWN = 0,
    CELL_PROVENANCE_KNOWN = 1,
} cell_provenance_t;

typedef struct {
    uint64_t a;
    uint64_t b;
    float stress;
    uint8_t stage;
    uint8_t failed;
    uint8_t _reserved[2];
} cell_join_stress_t;

typedef struct {
    uint8_t version;
    uint8_t join_count;
    uint8_t _reserved[2];
    cell_join_stress_t joins[CELL_STRESS_MAX_JOINS];
} cell_stress_state_t;

typedef struct {
    uint64_t impacted_identity;
    float impulse;
    vec2 normal;
    vec2 point;
    vec2 assembly_velocity;
    float assembly_rotation;
    float assembly_spin;
    uint8_t provenance; /* cell_provenance_t */
    uint8_t shell_manifest_root[32];
    uint8_t payload_manifest_root[32];
} cell_impact_t;

typedef struct {
    bool active;
    uint8_t provenance; /* cell_provenance_t */
    uint8_t _reserved[2];
    cell_graph_t graph;
    vec2 pos;
    vec2 vel;
    float rotation;
    float spin;
    uint8_t shell_manifest_root[32];
    uint8_t payload_manifest_root[32];
} cell_salvage_t;

typedef struct {
    bool sheared;
    uint8_t failed_join_stage;
    uint8_t _reserved[2];
    uint64_t failed_a;
    uint64_t failed_b;
    cell_graph_t remaining;
    cell_salvage_t salvage;
} cell_shear_result_t;

bool cell_stress_init(const cell_graph_t *graph, cell_stress_state_t *out);
bool cell_stress_apply_impact(const cell_graph_t *graph,
                              cell_stress_state_t *state,
                              const cell_impact_t *impact,
                              cell_shear_result_t *out);
bool cell_stress_reattach(cell_graph_t *remaining,
                          const cell_salvage_t *salvage,
                          cell_stress_state_t *state);

size_t cell_stress_encoded_size(const cell_stress_state_t *state);
bool cell_stress_encode(const cell_stress_state_t *state,
                        uint8_t *out, size_t cap, size_t *written);
bool cell_stress_decode(const uint8_t *data, size_t len,
                        cell_stress_state_t *out, size_t *consumed);
size_t cell_salvage_encoded_size(const cell_salvage_t *salvage);
bool cell_salvage_encode(const cell_salvage_t *salvage,
                         uint8_t *out, size_t cap, size_t *written);
bool cell_salvage_decode(const uint8_t *data, size_t len,
                         cell_salvage_t *out, size_t *consumed);

#endif /* CELL_STRESS_H */
