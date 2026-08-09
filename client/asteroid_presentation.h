#ifndef SIGNAL_ASTEROID_PRESENTATION_H
#define SIGNAL_ASTEROID_PRESENTATION_H

#include <stdbool.h>
#include <stdint.h>

#include "game_sim.h"
#include "tow_presentation_diagnostics.h"

/*
 * Local singleplayer keeps the multiplayer packet cadence, but it also owns
 * the in-process authoritative world.  This module turns that world into a
 * render-only pose feed.  Lifecycle/gameplay metadata still arrives through
 * normal replication; only position, velocity, rotation, spin, and age are
 * sampled from authority between packets.
 */
typedef enum {
    ASTEROID_MOTION_LOOSE = 0,
    ASTEROID_MOTION_PLAYER_TOW,
    ASTEROID_MOTION_NPC_TOW,
    ASTEROID_MOTION_STATION_TOW,
    ASTEROID_MOTION_BALLISTIC,
    ASTEROID_MOTION_CLASS_COUNT,
} asteroid_motion_class_t;

typedef enum {
    ASTEROID_PRESENTATION_SKIP = 0,
    ASTEROID_PRESENTATION_PRESENT,
    ASTEROID_PRESENTATION_RETIRE,
} asteroid_presentation_action_t;

/* Final feed error is expected to be floating-point noise only. Screen jerk
 * is measured on that correction error, not on physical asteroid motion, so
 * collisions and tow attachment impulses do not create false failures. */
#define ASTEROID_PRESENTATION_MAX_CORRECTION_WORLD        0.01f
#define ASTEROID_PRESENTATION_MAX_VELOCITY_DISCONTINUITY  0.01f
#define ASTEROID_PRESENTATION_MAX_SCREEN_JERK        500000.0f

asteroid_motion_class_t asteroid_motion_classify(
    const world_t *authority, int asteroid_index);

void asteroid_presentation_predict_motion(
    const asteroid_t *base, float elapsed, bool tow_driven,
    vec2 *out_pos, vec2 *out_vel);

asteroid_presentation_action_t asteroid_presentation_resolve(
    const world_t *authority, const asteroid_t *client_asteroid,
    int asteroid_index, float render_ahead,
    asteroid_t *out_presented,
    asteroid_motion_class_t *out_motion_class);

typedef struct {
    tow_presentation_diagnostics_t kinematics;
    asteroid_motion_class_t motion_class;
    bool motion_class_valid;
} asteroid_presentation_slot_track_t;

typedef struct {
    uint32_t frame_samples;
    uint32_t presented_samples;
    uint32_t skipped_samples;
    uint32_t retired_samples;
    uint32_t class_transitions;
    uint32_t starvation_events;
    uint32_t class_samples[ASTEROID_MOTION_CLASS_COUNT];
    float max_correction_world;
    float max_velocity_discontinuity;
    float max_screen_jerk;
    float max_legacy_correction_avoided;
    asteroid_presentation_slot_track_t slots[MAX_ASTEROIDS];
} asteroid_presentation_diagnostics_t;

void asteroid_presentation_diagnostics_reset(
    asteroid_presentation_diagnostics_t *diagnostics);
void asteroid_presentation_diagnostics_begin_frame(
    asteroid_presentation_diagnostics_t *diagnostics);
void asteroid_presentation_diagnostics_present(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index, asteroid_motion_class_t motion_class,
    const asteroid_t *legacy_presented,
    const asteroid_t *final_presented,
    const asteroid_t *authoritative_target,
    float frame_dt, float pixels_per_world);
void asteroid_presentation_diagnostics_skip(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index, asteroid_motion_class_t motion_class,
    const asteroid_t *legacy_presented,
    float frame_dt, float pixels_per_world);
void asteroid_presentation_diagnostics_retire(
    asteroid_presentation_diagnostics_t *diagnostics,
    int asteroid_index);
bool asteroid_presentation_diagnostics_within_thresholds(
    const asteroid_presentation_diagnostics_t *diagnostics);

#endif /* SIGNAL_ASTEROID_PRESENTATION_H */
