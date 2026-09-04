#include "test_harness.h"

#include "story_loop.h"

TEST(test_story_loop_guides_a_complete_worker_journey)
{
    worker_story_state_t story = {0};
    char label[64];
    char message[256];

    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_CALL);
    ASSERT(worker_story_directive(&story, label, sizeof(label),
                                  message, sizeof(message)));
    ASSERT_STR_EQ(label, "STORY // THE CALL");
    ASSERT(strstr(message, "KRX-472") != NULL);
    ASSERT(strstr(message, "Helios") != NULL);

    ASSERT(worker_story_mark_hail(&story, 2));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_MENTOR);
    ASSERT(worker_story_mark_hail(&story, 1));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_THRESHOLD);
    ASSERT(worker_story_mark_signal_gap(&story));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_ORDEAL);
    ASSERT(worker_story_mark_hail(&story, SIGNAL_FREEPORT_STATION_INDEX));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_APPROACH);
    ASSERT(worker_story_mark_outpost_placed(&story));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_REWARD);
    ASSERT(worker_story_mark_outpost_active(&story));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_ROAD_BACK);
    ASSERT(worker_story_mark_delivery(&story));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_RETURN);
    ASSERT(!worker_story_mark_dock(&story, 1));
    ASSERT(worker_story_mark_dock(&story, 0));
    ASSERT(worker_story_is_complete(&story));
    ASSERT(!worker_story_directive(&story, label, sizeof(label),
                                   message, sizeof(message)));
}

TEST(test_story_loop_keeps_character_beats_in_story_order)
{
    worker_story_state_t story = {0};

    ASSERT(!worker_story_mark_hail(&story, SIGNAL_FREEPORT_STATION_INDEX));
    ASSERT(!worker_story_mark_hail(&story, 1));
    ASSERT(!worker_story_mark_signal_gap(&story));
    ASSERT(!worker_story_mark_outpost_placed(&story));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_CALL);

    ASSERT(worker_story_mark_hail(&story, 2));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_MENTOR);
    ASSERT(!worker_story_mark_hail(&story, SIGNAL_FREEPORT_STATION_INDEX));
    ASSERT(worker_story_mark_hail(&story, 1));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_THRESHOLD);
    ASSERT(worker_story_mark_signal_gap(&story));
    ASSERT_EQ_INT(worker_story_beat(&story), WORKER_STORY_ORDEAL);
}

TEST(test_story_loop_requires_built_route_before_public_return)
{
    worker_story_state_t story = {0};

    ASSERT(!worker_story_mark_outpost_active(&story));
    ASSERT(!worker_story_mark_delivery(&story));
    ASSERT(!worker_story_mark_dock(&story, 0));

    story.flags = WORKER_STORY_HELIOS_HAILED |
                  WORKER_STORY_KEPLER_HAILED |
                  WORKER_STORY_SIGNAL_GAP_CROSSED |
                  WORKER_STORY_BLACKGLASS_HAILED;
    ASSERT(worker_story_mark_outpost_placed(&story));
    ASSERT(worker_story_mark_outpost_active(&story));
    ASSERT(worker_story_mark_delivery(&story));
    ASSERT(worker_story_mark_dock(&story, 0));
    ASSERT((story.flags & WORKER_STORY_ROUTE_PROVEN) != 0u);
    ASSERT((story.flags & WORKER_STORY_RETURNED_PROSPECT) != 0u);
}

TEST(test_story_loop_names_each_character_at_their_turn)
{
    ASSERT(strstr(worker_story_transition_line(WORKER_STORY_CALL),
                  "HELIOS") != NULL);
    ASSERT(strstr(worker_story_transition_line(WORKER_STORY_CALL),
                  "KRX-472") != NULL);
    ASSERT(strstr(worker_story_transition_line(WORKER_STORY_MENTOR),
                  "KEPLER") != NULL);
    ASSERT(strstr(worker_story_transition_line(WORKER_STORY_ORDEAL),
                  "BLACKGLASS") != NULL);
    ASSERT(strstr(worker_story_transition_line(WORKER_STORY_REWARD),
                  "GULL-7") != NULL);
    ASSERT(strstr(worker_story_transition_line(WORKER_STORY_RETURN),
                  "route-builder") != NULL);
}

void register_story_loop_tests(void)
{
    RUN(test_story_loop_guides_a_complete_worker_journey);
    RUN(test_story_loop_keeps_character_beats_in_story_order);
    RUN(test_story_loop_requires_built_route_before_public_return);
    RUN(test_story_loop_names_each_character_at_their_turn);
}
