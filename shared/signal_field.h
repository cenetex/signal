#ifndef SIGNAL_FIELD_H
#define SIGNAL_FIELD_H

#include <stdbool.h>
#include <stdint.h>

#include "math_util.h"

enum {
    SIGNAL_FIELD_WIDTH = 32,
    SIGNAL_FIELD_HEIGHT = 32,
    SIGNAL_FIELD_CELL_COUNT = SIGNAL_FIELD_WIDTH * SIGNAL_FIELD_HEIGHT,
    SIGNAL_FIELD_KIND_COUNT = 6,
};

#define SIGNAL_FIELD_CELL_SIZE 4096.0f
#define SIGNAL_FIELD_WORLD_MIN_X \
    (-(SIGNAL_FIELD_WIDTH * SIGNAL_FIELD_CELL_SIZE) * 0.5f)
#define SIGNAL_FIELD_WORLD_MIN_Y \
    (-(SIGNAL_FIELD_HEIGHT * SIGNAL_FIELD_CELL_SIZE) * 0.5f)
#define SIGNAL_FIELD_LOAD_WARN 0.75f
#define SIGNAL_FIELD_MARGIN_WARN 0.08f
#define SIGNAL_FIELD_SNR_WARN 1.50f

typedef enum {
    SIGNAL_FIELD_KIND_DEMAND = 0,
    SIGNAL_FIELD_KIND_SUPPLY,
    SIGNAL_FIELD_KIND_ROUTE,
    SIGNAL_FIELD_KIND_PROOF,
    SIGNAL_FIELD_KIND_HOLOGRAM,
    SIGNAL_FIELD_KIND_RISK,
} signal_field_kind_t;

typedef struct {
    float strength[SIGNAL_FIELD_KIND_COUNT];
    uint32_t last_tick[SIGNAL_FIELD_KIND_COUNT];
    uint16_t observations[SIGNAL_FIELD_KIND_COUNT];
} signal_field_cell_t;

typedef struct {
    int occupied_slots;
    int capacity_slots;
    float load;
    float top_strength;
    float second_strength;
    float top_margin;
    float recall_snr_estimate;
    bool noisy;
} signal_field_diagnostics_t;

typedef struct {
    signal_field_cell_t cells[SIGNAL_FIELD_CELL_COUNT];
} signal_field_t;

const char *signal_field_kind_label(signal_field_kind_t kind);

void signal_field_init(signal_field_t *field);
bool signal_field_world_to_cell(vec2 pos, int *out_x, int *out_y);
vec2 signal_field_cell_center(int cell_x, int cell_y);

bool signal_field_observe(signal_field_t *field,
                          vec2 pos,
                          signal_field_kind_t kind,
                          float strength,
                          uint32_t tick);

float signal_field_query(const signal_field_t *field,
                         vec2 pos,
                         signal_field_kind_t kind,
                         int radius_cells);

void signal_field_decay(signal_field_t *field,
                        uint32_t current_tick,
                        uint32_t half_life_ticks);

signal_field_diagnostics_t signal_field_diagnostics(const signal_field_t *field,
                                                    vec2 pos,
                                                    int radius_cells);

#endif /* SIGNAL_FIELD_H */
