/*
 * story_loop.h -- The first worker hero loop.
 *
 * The story reads completed simulation events. Each flag records one piece
 * of player-visible proof.
 */
#ifndef SIGNAL_SHARED_STORY_LOOP_H
#define SIGNAL_SHARED_STORY_LOOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WORKER_STORY_CALL = 0,
    WORKER_STORY_MENTOR,
    WORKER_STORY_THRESHOLD,
    WORKER_STORY_ORDEAL,
    WORKER_STORY_APPROACH,
    WORKER_STORY_REWARD,
    WORKER_STORY_ROAD_BACK,
    WORKER_STORY_RETURN,
    WORKER_STORY_COMPLETE,
} worker_story_beat_t;

typedef enum {
    WORKER_STORY_HELIOS_HAILED      = 1u << 0,
    WORKER_STORY_KEPLER_HAILED      = 1u << 1,
    WORKER_STORY_SIGNAL_GAP_CROSSED = 1u << 2,
    WORKER_STORY_BLACKGLASS_HAILED  = 1u << 3,
    WORKER_STORY_OUTPOST_PLACED     = 1u << 4,
    WORKER_STORY_OUTPOST_ACTIVE     = 1u << 5,
    WORKER_STORY_ROUTE_PROVEN       = 1u << 6,
    WORKER_STORY_RETURNED_PROSPECT  = 1u << 7,
} worker_story_flag_t;

typedef struct {
    uint16_t flags;
    bool loaded;
} worker_story_state_t;

worker_story_beat_t worker_story_beat(const worker_story_state_t *story);
bool worker_story_is_complete(const worker_story_state_t *story);

bool worker_story_mark_hail(worker_story_state_t *story, int station_index);
bool worker_story_mark_signal_gap(worker_story_state_t *story);
bool worker_story_mark_outpost_placed(worker_story_state_t *story);
bool worker_story_mark_outpost_active(worker_story_state_t *story);
bool worker_story_mark_delivery(worker_story_state_t *story);
bool worker_story_mark_dock(worker_story_state_t *story, int station_index);

bool worker_story_directive(const worker_story_state_t *story,
                            char *label, size_t label_size,
                            char *message, size_t message_size);
const char *worker_story_transition_line(worker_story_beat_t completed_beat);

#endif /* SIGNAL_SHARED_STORY_LOOP_H */
