#include "test_harness.h"
#include "hud_attention.h"
#include <string.h>

TEST(test_hud_attention_uses_one_deterministic_primary_surface) {
    hud_attention_flags_t flags = {
        .message = true,
        .scoreboard = true,
        .inspect = true,
        .docked = true,
        .death = true,
    };
    ASSERT_EQ_INT(hud_attention_select(flags), HUD_ATTENTION_DEATH);

    flags.death = false;
    ASSERT_EQ_INT(hud_attention_select(flags), HUD_ATTENTION_STATION);
    flags.docked = false;
    ASSERT_EQ_INT(hud_attention_select(flags), HUD_ATTENTION_INSPECT);
    flags.inspect = false;
    ASSERT_EQ_INT(hud_attention_select(flags), HUD_ATTENTION_SCOREBOARD);
    flags.scoreboard = false;
    ASSERT_EQ_INT(hud_attention_select(flags), HUD_ATTENTION_MESSAGE);
    flags.message = false;
    ASSERT_EQ_INT(hud_attention_select(flags), HUD_ATTENTION_FLIGHT);
}

TEST(test_hud_attention_surface_names_are_stable_for_browser_review) {
    ASSERT(strcmp(hud_attention_surface_name(HUD_ATTENTION_FLIGHT), "flight") == 0);
    ASSERT(strcmp(hud_attention_surface_name(HUD_ATTENTION_MESSAGE), "message") == 0);
    ASSERT(strcmp(hud_attention_surface_name(HUD_ATTENTION_SCOREBOARD), "scoreboard") == 0);
    ASSERT(strcmp(hud_attention_surface_name(HUD_ATTENTION_INSPECT), "inspect") == 0);
    ASSERT(strcmp(hud_attention_surface_name(HUD_ATTENTION_STATION), "station") == 0);
    ASSERT(strcmp(hud_attention_surface_name(HUD_ATTENTION_DEATH), "death") == 0);
}

TEST(test_hud_scan_budget_tracks_semantic_zoom) {
    ASSERT_EQ_INT(hud_scan_asteroid_budget(269.0f), 4);
    ASSERT_EQ_INT(hud_scan_npc_budget(269.0f), 2);
    ASSERT_EQ_INT(hud_scan_asteroid_budget(600.0f), 6);
    ASSERT_EQ_INT(hud_scan_npc_budget(600.0f), 3);
    ASSERT_EQ_INT(hud_scan_asteroid_budget(800.0f), 8);
    ASSERT_EQ_INT(hud_scan_npc_budget(800.0f), 4);
}

void register_hud_attention_tests(void) {
    TEST_SECTION("\nHUD attention:\n");
    RUN(test_hud_attention_uses_one_deterministic_primary_surface);
    RUN(test_hud_attention_surface_names_are_stable_for_browser_review);
    RUN(test_hud_scan_budget_tracks_semantic_zoom);
}
