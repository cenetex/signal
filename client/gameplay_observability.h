#ifndef SIGNAL_GAMEPLAY_OBSERVABILITY_H
#define SIGNAL_GAMEPLAY_OBSERVABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GAMEPLAY_PHASE_INPUT_NETWORK = 0,
    GAMEPLAY_PHASE_SIMULATION,
    GAMEPLAY_PHASE_INTERPOLATION,
    GAMEPLAY_PHASE_WORLD_RENDER,
    GAMEPLAY_PHASE_UI_RENDER,
    GAMEPLAY_PHASE_SUBMISSION,
    GAMEPLAY_PHASE_AUDIO_MEDIA,
    GAMEPLAY_PHASE_UNATTRIBUTED,
    /* Detail phases may sit inside simulation and are not double-counted
     * when assigning a slow frame to its primary cause. */
    GAMEPLAY_PHASE_AUTHORITY_SIM,
    GAMEPLAY_PHASE_LOOPBACK_CODEC,
    GAMEPLAY_PHASE_COUNT,
} gameplay_observability_phase_t;

#define GAMEPLAY_PRIMARY_PHASE_COUNT GAMEPLAY_PHASE_AUTHORITY_SIM

typedef enum {
    GAMEPLAY_ENTITY_ASTEROID = 0,
    GAMEPLAY_ENTITY_CARGO_POD,
    GAMEPLAY_ENTITY_SCAFFOLD,
    GAMEPLAY_ENTITY_NPC,
    GAMEPLAY_ENTITY_REMOTE_PLAYER,
    GAMEPLAY_ENTITY_COUNT,
} gameplay_observability_entity_t;

/* Disabled by default. The disabled path is one predictable branch. */
void gameplay_observability_configure(bool enabled, double report_interval_sec);
bool gameplay_observability_enabled(void);
void gameplay_observability_reset(void);

double gameplay_observability_now(void);
double gameplay_observability_phase_begin(void);
void gameplay_observability_phase_end(
    gameplay_observability_phase_t phase, double started_at);

void gameplay_observability_frame_begin(void);
void gameplay_observability_frame_end(void);
void gameplay_observability_record_sim_steps(
    uint32_t completed, uint32_t missed, uint32_t dropped);
void gameplay_observability_record_packet(uint8_t type, size_t bytes);
void gameplay_observability_record_entity_correction(
    gameplay_observability_entity_t entity,
    float correction_world, float velocity_discontinuity,
    float packet_gap_sec);

/* Stable until the next call. Intended for browser gates and bug reports. */
const char *gameplay_observability_report_json(void);

#endif /* SIGNAL_GAMEPLAY_OBSERVABILITY_H */
