#include "story_loop.h"

#include "types.h"

#include <stdio.h>

static bool story_set(worker_story_state_t *story, worker_story_flag_t flag)
{
    if (!story) return false;
    uint16_t bit = (uint16_t)flag;
    if ((story->flags & bit) != 0u) return false;
    story->flags |= bit;
    return true;
}

static bool story_has(const worker_story_state_t *story,
                      worker_story_flag_t flag)
{
    return story && (story->flags & (uint16_t)flag) != 0u;
}

worker_story_beat_t worker_story_beat(const worker_story_state_t *story)
{
    if (!story_has(story, WORKER_STORY_HELIOS_HAILED))
        return WORKER_STORY_CALL;
    if (!story_has(story, WORKER_STORY_KEPLER_HAILED))
        return WORKER_STORY_MENTOR;
    if (!story_has(story, WORKER_STORY_SIGNAL_GAP_CROSSED))
        return WORKER_STORY_THRESHOLD;
    if (!story_has(story, WORKER_STORY_BLACKGLASS_HAILED))
        return WORKER_STORY_ORDEAL;
    if (!story_has(story, WORKER_STORY_OUTPOST_PLACED))
        return WORKER_STORY_APPROACH;
    if (!story_has(story, WORKER_STORY_OUTPOST_ACTIVE))
        return WORKER_STORY_REWARD;
    if (!story_has(story, WORKER_STORY_ROUTE_PROVEN))
        return WORKER_STORY_ROAD_BACK;
    if (!story_has(story, WORKER_STORY_RETURNED_PROSPECT))
        return WORKER_STORY_RETURN;
    return WORKER_STORY_COMPLETE;
}

bool worker_story_is_complete(const worker_story_state_t *story)
{
    return worker_story_beat(story) == WORKER_STORY_COMPLETE;
}

bool worker_story_mark_hail(worker_story_state_t *story, int station_index)
{
    worker_story_beat_t beat = worker_story_beat(story);
    if (station_index == 2 && beat == WORKER_STORY_CALL)
        return story_set(story, WORKER_STORY_HELIOS_HAILED);
    if (station_index == 1 && beat == WORKER_STORY_MENTOR)
        return story_set(story, WORKER_STORY_KEPLER_HAILED);
    if (station_index == SIGNAL_FREEPORT_STATION_INDEX &&
        beat == WORKER_STORY_ORDEAL) {
        return story_set(story, WORKER_STORY_BLACKGLASS_HAILED);
    }
    return false;
}

bool worker_story_mark_signal_gap(worker_story_state_t *story)
{
    if (worker_story_beat(story) != WORKER_STORY_THRESHOLD) return false;
    return story_set(story, WORKER_STORY_SIGNAL_GAP_CROSSED);
}

bool worker_story_mark_outpost_placed(worker_story_state_t *story)
{
    if (worker_story_beat(story) != WORKER_STORY_APPROACH) return false;
    return story_set(story, WORKER_STORY_OUTPOST_PLACED);
}

bool worker_story_mark_outpost_active(worker_story_state_t *story)
{
    if (worker_story_beat(story) != WORKER_STORY_REWARD) return false;
    return story_set(story, WORKER_STORY_OUTPOST_ACTIVE);
}

bool worker_story_mark_delivery(worker_story_state_t *story)
{
    if (worker_story_beat(story) != WORKER_STORY_ROAD_BACK) return false;
    return story_set(story, WORKER_STORY_ROUTE_PROVEN);
}

bool worker_story_mark_dock(worker_story_state_t *story, int station_index)
{
    if (station_index != 0 ||
        worker_story_beat(story) != WORKER_STORY_RETURN) {
        return false;
    }
    return story_set(story, WORKER_STORY_RETURNED_PROSPECT);
}

bool worker_story_directive(const worker_story_state_t *story,
                            char *label, size_t label_size,
                            char *message, size_t message_size)
{
    if (!story || !label || label_size == 0 ||
        !message || message_size == 0) {
        return false;
    }
    label[0] = '\0';
    message[0] = '\0';

    switch (worker_story_beat(story)) {
    case WORKER_STORY_CALL:
        snprintf(label, label_size, "STORY // THE CALL");
        snprintf(message, message_size,
                 "KRX-472 missed a corridor check-in. Reach Helios signal and hail [H].");
        return true;
    case WORKER_STORY_MENTOR:
        snprintf(label, label_size, "STORY // THE MENTOR");
        snprintf(message, message_size,
                 "Kepler has a relay plan for the broken corridor. Reach the yard and hail [H].");
        return true;
    case WORKER_STORY_THRESHOLD:
        snprintf(label, label_size, "STORY // THRESHOLD");
        snprintf(message, message_size,
                 "Fly from Kepler toward Helios. Cross the weak-signal gap.");
        return true;
    case WORKER_STORY_ORDEAL:
        snprintf(label, label_size, "STORY // THE ORDEAL");
        snprintf(message, message_size,
                 "Find Blackglass in the gap. Hail [H] and choose what your work means.");
        return true;
    case WORKER_STORY_APPROACH:
        snprintf(label, label_size, "STORY // APPROACH");
        snprintf(message, message_size,
                 "Place a relay outpost in the Helios gap. Use [B], then [E] to lock it.");
        return true;
    case WORKER_STORY_REWARD:
        snprintf(label, label_size, "STORY // THE REWARD");
        snprintf(message, message_size,
                 "Tow a signal relay to the blueprint. Supply the frames that wake it.");
        return true;
    case WORKER_STORY_ROAD_BACK:
        snprintf(label, label_size, "STORY // ROAD BACK");
        snprintf(message, message_size,
                 "Carry one paid delivery through the new corridor. Let the route prove itself.");
        return true;
    case WORKER_STORY_RETURN:
        snprintf(label, label_size, "STORY // RETURN");
        snprintf(message, message_size,
                 "Return to Prospect and dock [E]. Bring the route home.");
        return true;
    case WORKER_STORY_COMPLETE:
    default:
        return false;
    }
}

const char *worker_story_transition_line(worker_story_beat_t completed_beat)
{
    switch (completed_beat) {
    case WORKER_STORY_CALL:
        return "HELIOS // KRX-472 missed the corridor check-in. Kepler has the relay plan.";
    case WORKER_STORY_MENTOR:
        return "KEPLER // Plan is clean. Cross the weak-signal gap and read the route yourself.";
    case WORKER_STORY_THRESHOLD:
        return "KRX-472 // Signal fading. Blackglass is the only light ahead.";
    case WORKER_STORY_ORDEAL:
        return "BLACKGLASS // Cargo sells now. A relay earns trust later. Your call.";
    case WORKER_STORY_APPROACH:
        return "KEPLER // Good placement. Bring frames and wake the relay.";
    case WORKER_STORY_REWARD:
        return "GULL-7 // I see your light. Taking the corridor now.";
    case WORKER_STORY_ROAD_BACK:
        return "GULL-7 // Corridor runs clean. Prospect should hear who built it.";
    case WORKER_STORY_RETURN:
        return "PROSPECT // KRX-472 checked in. Dock is open, route-builder.";
    case WORKER_STORY_COMPLETE:
    default:
        return "";
    }
}
