#include "signal_field.h"

#include "fixpoint.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIGNAL_FIELD_EMPTY_EPSILON 0.0001f

static bool signal_field_kind_valid(signal_field_kind_t kind) {
    int k = (int)kind;
    return k >= 0 && k < SIGNAL_FIELD_KIND_COUNT;
}

static int signal_field_cell_index(int cell_x, int cell_y) {
    return cell_y * SIGNAL_FIELD_WIDTH + cell_x;
}

static const signal_field_cell_t *signal_field_cell_const(const signal_field_t *field,
                                                          int cell_x,
                                                          int cell_y) {
    if (!field) return NULL;
    if (cell_x < 0 || cell_x >= SIGNAL_FIELD_WIDTH) return NULL;
    if (cell_y < 0 || cell_y >= SIGNAL_FIELD_HEIGHT) return NULL;
    return &field->cells[signal_field_cell_index(cell_x, cell_y)];
}

static signal_field_cell_t *signal_field_cell(signal_field_t *field,
                                              int cell_x,
                                              int cell_y) {
    if (!field) return NULL;
    if (cell_x < 0 || cell_x >= SIGNAL_FIELD_WIDTH) return NULL;
    if (cell_y < 0 || cell_y >= SIGNAL_FIELD_HEIGHT) return NULL;
    return &field->cells[signal_field_cell_index(cell_x, cell_y)];
}

static float signal_field_decay_multiplier(uint32_t elapsed,
                                           uint32_t half_life_ticks) {
    if (elapsed == 0 || half_life_ticks == 0) return 1.0f;
    return fixp_powf(0.5f, (float)elapsed / (float)half_life_ticks);
}

const char *signal_field_kind_label(signal_field_kind_t kind) {
    switch (kind) {
    case SIGNAL_FIELD_KIND_DEMAND:   return "demand";
    case SIGNAL_FIELD_KIND_SUPPLY:   return "supply";
    case SIGNAL_FIELD_KIND_ROUTE:    return "route";
    case SIGNAL_FIELD_KIND_PROOF:    return "proof";
    case SIGNAL_FIELD_KIND_HOLOGRAM: return "hologram";
    case SIGNAL_FIELD_KIND_RISK:     return "risk";
    default:                         return "?";
    }
}

void signal_field_init(signal_field_t *field) {
    if (!field) return;
    memset(field, 0, sizeof *field);
}

bool signal_field_world_to_cell(vec2 pos, int *out_x, int *out_y) {
    float fx = (pos.x - SIGNAL_FIELD_WORLD_MIN_X) / SIGNAL_FIELD_CELL_SIZE;
    float fy = (pos.y - SIGNAL_FIELD_WORLD_MIN_Y) / SIGNAL_FIELD_CELL_SIZE;
    if (!isfinite(fx) || !isfinite(fy)) return false;
    if (fx < 0.0f || fy < 0.0f) return false;
    if (fx >= (float)SIGNAL_FIELD_WIDTH ||
        fy >= (float)SIGNAL_FIELD_HEIGHT) {
        return false;
    }
    if (out_x) *out_x = (int)floorf(fx);
    if (out_y) *out_y = (int)floorf(fy);
    return true;
}

vec2 signal_field_cell_center(int cell_x, int cell_y) {
    return v2(SIGNAL_FIELD_WORLD_MIN_X +
                  ((float)cell_x + 0.5f) * SIGNAL_FIELD_CELL_SIZE,
              SIGNAL_FIELD_WORLD_MIN_Y +
                  ((float)cell_y + 0.5f) * SIGNAL_FIELD_CELL_SIZE);
}

bool signal_field_observe(signal_field_t *field,
                          vec2 pos,
                          signal_field_kind_t kind,
                          float strength,
                          uint32_t tick) {
    if (!signal_field_kind_valid(kind)) return false;
    int cell_x = 0;
    int cell_y = 0;
    if (!signal_field_world_to_cell(pos, &cell_x, &cell_y)) return false;
    signal_field_cell_t *cell = signal_field_cell(field, cell_x, cell_y);
    if (!cell) return false;

    float s = clampf(strength, 0.0f, 1.0f);
    float *slot = &cell->strength[kind];
    *slot = clampf(*slot + (1.0f - *slot) * s, 0.0f, 1.0f);
    cell->last_tick[kind] = tick;
    if (cell->observations[kind] < UINT16_MAX) {
        cell->observations[kind]++;
    }
    return true;
}

float signal_field_query(const signal_field_t *field,
                         vec2 pos,
                         signal_field_kind_t kind,
                         int radius_cells) {
    if (!field || !signal_field_kind_valid(kind)) return 0.0f;
    int center_x = 0;
    int center_y = 0;
    if (!signal_field_world_to_cell(pos, &center_x, &center_y)) return 0.0f;
    if (radius_cells < 0) radius_cells = 0;

    float weighted = 0.0f;
    float total_weight = 0.0f;
    for (int dy = -radius_cells; dy <= radius_cells; dy++) {
        for (int dx = -radius_cells; dx <= radius_cells; dx++) {
            const signal_field_cell_t *cell =
                signal_field_cell_const(field, center_x + dx, center_y + dy);
            if (!cell) continue;
            float dist = (float)(abs(dx) + abs(dy));
            float weight = 1.0f / (1.0f + dist);
            weighted += cell->strength[kind] * weight;
            total_weight += weight;
        }
    }
    if (total_weight <= 0.0f) return 0.0f;
    return clampf(weighted / total_weight, 0.0f, 1.0f);
}

void signal_field_decay(signal_field_t *field,
                        uint32_t current_tick,
                        uint32_t half_life_ticks) {
    if (!field || half_life_ticks == 0) return;
    for (int i = 0; i < SIGNAL_FIELD_CELL_COUNT; i++) {
        signal_field_cell_t *cell = &field->cells[i];
        for (int kind = 0; kind < SIGNAL_FIELD_KIND_COUNT; kind++) {
            float s = cell->strength[kind];
            if (s <= SIGNAL_FIELD_EMPTY_EPSILON) {
                cell->strength[kind] = 0.0f;
                continue;
            }
            uint32_t last = cell->last_tick[kind];
            if (current_tick <= last) continue;
            uint32_t elapsed = current_tick - last;
            cell->strength[kind] =
                s * signal_field_decay_multiplier(elapsed, half_life_ticks);
            cell->last_tick[kind] = current_tick;
            if (cell->strength[kind] <= SIGNAL_FIELD_EMPTY_EPSILON) {
                cell->strength[kind] = 0.0f;
            }
        }
    }
}

signal_field_diagnostics_t signal_field_diagnostics(const signal_field_t *field,
                                                    vec2 pos,
                                                    int radius_cells) {
    signal_field_diagnostics_t diag = {0};
    diag.capacity_slots = SIGNAL_FIELD_CELL_COUNT * SIGNAL_FIELD_KIND_COUNT;
    if (!field) return diag;

    for (int i = 0; i < SIGNAL_FIELD_CELL_COUNT; i++) {
        for (int kind = 0; kind < SIGNAL_FIELD_KIND_COUNT; kind++) {
            if (field->cells[i].strength[kind] > SIGNAL_FIELD_EMPTY_EPSILON) {
                diag.occupied_slots++;
            }
        }
    }
    if (diag.capacity_slots > 0) {
        diag.load = (float)diag.occupied_slots / (float)diag.capacity_slots;
    }

    for (int kind = 0; kind < SIGNAL_FIELD_KIND_COUNT; kind++) {
        float s = signal_field_query(field, pos, (signal_field_kind_t)kind,
                                     radius_cells);
        if (s > diag.top_strength) {
            diag.second_strength = diag.top_strength;
            diag.top_strength = s;
        } else if (s > diag.second_strength) {
            diag.second_strength = s;
        }
    }
    diag.top_margin = diag.top_strength - diag.second_strength;

    float noise = 0.02f + diag.second_strength + diag.load * 0.25f;
    diag.recall_snr_estimate =
        (noise > 0.0f) ? clampf(diag.top_strength / noise, 0.0f, 64.0f) : 64.0f;
    diag.noisy =
        (diag.load >= SIGNAL_FIELD_LOAD_WARN) ||
        (diag.top_strength > SIGNAL_FIELD_EMPTY_EPSILON &&
         diag.top_margin < SIGNAL_FIELD_MARGIN_WARN) ||
        (diag.top_strength > SIGNAL_FIELD_EMPTY_EPSILON &&
         diag.recall_snr_estimate < SIGNAL_FIELD_SNR_WARN);
    return diag;
}
