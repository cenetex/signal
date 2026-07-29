#ifndef SIGNAL_TOW_PRESENTATION_DIAGNOSTICS_H
#define SIGNAL_TOW_PRESENTATION_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

enum {
    TOW_ADVERSE_MAX_PACKETS = 192,
    TOW_ADVERSE_MAX_DELIVERIES = 256,
};

/*
 * Fixed review gates for the deterministic 60 Hz diagnostic. These are
 * authored constants, not percentiles derived from the run being judged.
 */
#define TOW_PRESENTATION_MAX_CORRECTION_WORLD       24.0f
#define TOW_PRESENTATION_MAX_VELOCITY_DISCONTINUITY 18.0f
#define TOW_PRESENTATION_STARVATION_BEGIN_SEC        0.12f
#define TOW_PRESENTATION_MAX_SNAPSHOT_GAP_SEC        0.18f
#define TOW_PRESENTATION_MAX_WORLD_JERK          250000.0f
#define TOW_PRESENTATION_MAX_SCREEN_JERK         500000.0f
#define TOW_PRESENTATION_TEST_PIXELS_PER_WORLD        2.0f

typedef enum {
    TOW_ADVERSE_CHANNEL_TARGET = 1,
    TOW_ADVERSE_CHANNEL_RELATION = 2,
} tow_adverse_channel_t;

typedef struct {
    uint32_t sequence;
    uint32_t send_ms;
    uint32_t payload_index;
    uint8_t channel; /* tow_adverse_channel_t */
} tow_adverse_packet_t;

typedef struct {
    uint16_t base_latency_ms;
    uint16_t max_jitter_ms;
    uint16_t max_reorder_ms;
    uint8_t drop_modulus;
    uint8_t duplicate_modulus;
    uint8_t reorder_modulus;
} tow_adverse_profile_t;

typedef struct {
    tow_adverse_packet_t packet;
    uint32_t delivery_ms;
    int16_t jitter_ms;
    int16_t reorder_ms;
    uint8_t duplicate_ordinal;
} tow_adverse_delivery_t;

typedef struct {
    uint32_t input_packets;
    uint32_t delivered_packets;
    uint32_t dropped_packets;
    uint32_t duplicated_packets;
    uint32_t reordered_packets;
    uint16_t max_abs_jitter_ms;
    uint16_t max_abs_reorder_ms;
} tow_adverse_schedule_stats_t;

/*
 * Diagnostic-only scheduler. Loss drops one full-replacement snapshot
 * instance, never a relation delta: a later snapshot at the same or newer
 * revision contains the complete relation set and can recover independently.
 * This function is not connected to production transport.
 */
tow_adverse_profile_t tow_adverse_profile(uint16_t base_latency_ms);
int tow_adverse_schedule(
    const tow_adverse_profile_t *profile,
    const tow_adverse_packet_t *packets,
    int packet_count,
    tow_adverse_delivery_t *out,
    int out_cap,
    tow_adverse_schedule_stats_t *stats);

typedef struct {
    uint32_t snapshot_samples;
    uint32_t presentation_samples;
    uint32_t jerk_samples;
    uint32_t starvation_events;
    float max_correction_world;
    float max_velocity_discontinuity;
    float max_snapshot_gap_sec;
    float max_world_jerk;
    float max_screen_jerk;

    bool have_snapshot;
    bool have_position;
    bool have_frame_velocity;
    bool have_acceleration;
    bool starvation_active;
    float seconds_since_snapshot;
    vec2 last_position;
    vec2 last_frame_velocity;
    vec2 last_acceleration;
} tow_presentation_diagnostics_t;

void tow_presentation_diagnostics_reset(
    tow_presentation_diagnostics_t *diagnostics);
void tow_presentation_diagnostics_snapshot(
    tow_presentation_diagnostics_t *diagnostics,
    vec2 presented_pos,
    vec2 presented_vel,
    vec2 authoritative_pos,
    vec2 authoritative_vel);
void tow_presentation_diagnostics_frame(
    tow_presentation_diagnostics_t *diagnostics,
    bool visible,
    vec2 presented_pos,
    float dt,
    float pixels_per_world);
bool tow_presentation_diagnostics_within_thresholds(
    const tow_presentation_diagnostics_t *diagnostics);

#endif /* SIGNAL_TOW_PRESENTATION_DIAGNOSTICS_H */
