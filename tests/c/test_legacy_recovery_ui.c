#include "test_harness.h"

#include "legacy_recovery_ui.h"

static legacy_recovery_ui_input_t no_recovery_input(void) {
    legacy_recovery_ui_input_t input = {0};
    return input;
}

static void arm_recovery_offer(
    legacy_recovery_ui_t *ui,
    uint32_t now_ms)
{
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(
            ui, now_ms, no_recovery_input()),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    ASSERT(legacy_recovery_ui_can_confirm(ui));
    ASSERT(legacy_recovery_ui_can_cancel(ui));
}

TEST(test_legacy_recovery_ui_rejects_zero_and_duplicate_offers) {
    legacy_recovery_ui_t ui = {0};
    ASSERT(!legacy_recovery_ui_begin_offer(&ui, 100, 0));
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_IDLE);
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 100, 30));
    ASSERT(!legacy_recovery_ui_begin_offer(&ui, 101, 30));
    ASSERT(legacy_recovery_ui_visible(&ui));
    ASSERT(legacy_recovery_ui_blocks_gameplay(&ui));
    ASSERT_EQ_INT(ui.semantic, LEGACY_RECOVERY_UI_SEMANTIC_OFFER);
    ASSERT_EQ_INT(
        legacy_recovery_ui_seconds_remaining(&ui, 100), 30);
    ASSERT_EQ_INT(
        legacy_recovery_ui_seconds_remaining(&ui, 101), 30);
    ASSERT_EQ_INT(
        legacy_recovery_ui_seconds_remaining(&ui, 1099), 30);
    ASSERT_EQ_INT(
        legacy_recovery_ui_seconds_remaining(&ui, 1100), 29);
}

TEST(test_legacy_recovery_ui_requires_release_then_fresh_confirm) {
    legacy_recovery_ui_t ui = {0};
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 1000, 30));

    legacy_recovery_ui_input_t held = {
        .confirm_down = true,
        .confirm_pressed = true,
    };
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 1000, held),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    ASSERT(!legacy_recovery_ui_can_confirm(&ui));

    arm_recovery_offer(&ui, 1001);
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 1002, held),
        LEGACY_RECOVERY_UI_ACTION_CONFIRM);
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_CONFIRMING);
    ASSERT(!legacy_recovery_ui_can_confirm(&ui));
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 1003, held),
        LEGACY_RECOVERY_UI_ACTION_NONE);
}

TEST(test_legacy_recovery_ui_cancel_wins_same_frame_and_is_one_shot) {
    legacy_recovery_ui_t ui = {0};
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 20, 10));
    arm_recovery_offer(&ui, 20);
    legacy_recovery_ui_input_t both = {
        .confirm_down = true,
        .cancel_down = true,
        .confirm_pressed = true,
        .cancel_pressed = true,
    };
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 21, both),
        LEGACY_RECOVERY_UI_ACTION_CANCEL);
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_RESULT);
    ASSERT_EQ_INT(
        ui.semantic, LEGACY_RECOVERY_UI_SEMANTIC_CANCELLED);
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 22, both),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    ASSERT(strstr(legacy_recovery_ui_body(&ui), "locally") != NULL);
    ASSERT(strstr(legacy_recovery_ui_body(&ui), "untouched") != NULL);
}

TEST(test_legacy_recovery_ui_transport_rejection_is_retryable) {
    legacy_recovery_ui_t ui = {0};
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 500, 20));
    arm_recovery_offer(&ui, 500);
    legacy_recovery_ui_input_t confirm = {
        .confirm_down = true,
        .confirm_pressed = true,
    };
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 501, confirm),
        LEGACY_RECOVERY_UI_ACTION_CONFIRM);
    legacy_recovery_ui_note_send(&ui, false);
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_OFFERED);
    ASSERT_EQ_INT(
        ui.semantic,
        LEGACY_RECOVERY_UI_SEMANTIC_RETRYABLE_SEND);
    ASSERT(!legacy_recovery_ui_can_confirm(&ui));
    ASSERT(strstr(legacy_recovery_ui_body(&ui), "Nothing changed") != NULL);

    /* A held/double-click edge cannot immediately retry. */
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 502, confirm),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    arm_recovery_offer(&ui, 503);
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(&ui, 504, confirm),
        LEGACY_RECOVERY_UI_ACTION_CONFIRM);
    legacy_recovery_ui_note_send(&ui, true);
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_CONFIRMING);
}

TEST(test_legacy_recovery_ui_expiry_is_terminal_and_wrap_safe) {
    legacy_recovery_ui_t ui = {0};
    uint32_t now = UINT32_MAX - 500u;
    ASSERT(legacy_recovery_ui_begin_offer(&ui, now, 1));
    arm_recovery_offer(&ui, now);
    ASSERT_EQ_INT(
        legacy_recovery_ui_seconds_remaining(&ui, now), 1);
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(
            &ui, now + 999u, no_recovery_input()),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(
            &ui, now + 1000u, no_recovery_input()),
        LEGACY_RECOVERY_UI_ACTION_EXPIRE);
    ASSERT_EQ_INT(
        ui.semantic,
        LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER);
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(
            &ui, now + 1001u,
            (legacy_recovery_ui_input_t){
                .confirm_down = true,
                .confirm_pressed = true,
            }),
        LEGACY_RECOVERY_UI_ACTION_NONE);
}

TEST(test_legacy_recovery_ui_maps_all_server_results) {
    static const struct {
        legacy_recovery_result_status_t wire;
        legacy_recovery_ui_semantic_t semantic;
        const char *copy_fragment;
    } cases[] = {
        {
            LEGACY_RECOVERY_RESULT_NO_MATCH,
            LEGACY_RECOVERY_UI_SEMANTIC_NO_MATCH,
            "No matching",
        },
        {
            LEGACY_RECOVERY_RESULT_STALE_OFFER,
            LEGACY_RECOVERY_UI_SEMANTIC_STALE_OFFER,
            "older connection",
        },
        {
            LEGACY_RECOVERY_RESULT_REPLAY,
            LEGACY_RECOVERY_UI_SEMANTIC_REPLAY,
            "already used",
        },
        {
            LEGACY_RECOVERY_RESULT_INVALID_SOURCE,
            LEGACY_RECOVERY_UI_SEMANTIC_INVALID_SOURCE,
            "corrupt or unsupported",
        },
        {
            LEGACY_RECOVERY_RESULT_DESTINATION_CONFLICT,
            LEGACY_RECOVERY_UI_SEMANTIC_DESTINATION_CONFLICT,
            "not overwritten",
        },
        {
            LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE,
            LEGACY_RECOVERY_UI_SEMANTIC_MIGRATION_FAILURE,
            "could not be completed",
        },
        {
            LEGACY_RECOVERY_RESULT_SUCCESS,
            LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS,
            "state refreshed",
        },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        legacy_recovery_ui_t ui = {0};
        ASSERT(legacy_recovery_ui_begin_offer(&ui, 100, 30));
        arm_recovery_offer(&ui, 100);
        ASSERT(legacy_recovery_ui_apply_result(
            &ui, cases[i].wire, 200));
        ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_RESULT);
        ASSERT_EQ_INT(ui.semantic, cases[i].semantic);
        ASSERT(strstr(
            legacy_recovery_ui_body(&ui),
            cases[i].copy_fragment) != NULL);
        ASSERT(!legacy_recovery_ui_can_confirm(&ui));
    }
}

TEST(test_legacy_recovery_ui_ignores_unsolicited_invalid_and_duplicate_results) {
    legacy_recovery_ui_t ui = {0};
    ASSERT(!legacy_recovery_ui_apply_result(
        &ui, LEGACY_RECOVERY_RESULT_SUCCESS, 0));
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 0, 30));
    ASSERT(!legacy_recovery_ui_apply_result(
        &ui, (legacy_recovery_result_status_t)0, 1));
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_OFFERED);
    ASSERT(legacy_recovery_ui_apply_result(
        &ui, LEGACY_RECOVERY_RESULT_SUCCESS, 2));
    ASSERT(!legacy_recovery_ui_apply_result(
        &ui, LEGACY_RECOVERY_RESULT_MIGRATION_FAILURE, 3));
    ASSERT_EQ_INT(
        ui.semantic, LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS);
}

TEST(test_legacy_recovery_ui_result_closes_and_reset_allows_reconnect) {
    legacy_recovery_ui_t ui = {0};
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 10, 30));
    ASSERT(legacy_recovery_ui_apply_result(
        &ui, LEGACY_RECOVERY_RESULT_SUCCESS, 20));
    ASSERT(legacy_recovery_ui_visible(&ui));
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(
            &ui, 4019, no_recovery_input()),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    ASSERT(legacy_recovery_ui_visible(&ui));
    ASSERT_EQ_INT(
        legacy_recovery_ui_update(
            &ui, 4020, no_recovery_input()),
        LEGACY_RECOVERY_UI_ACTION_NONE);
    ASSERT(!legacy_recovery_ui_visible(&ui));

    ASSERT(legacy_recovery_ui_begin_offer(&ui, 5000, 30));
    legacy_recovery_ui_reset(&ui);
    ASSERT_EQ_INT(ui.phase, LEGACY_RECOVERY_UI_IDLE);
    ASSERT(legacy_recovery_ui_begin_offer(&ui, 6000, 30));
}

TEST(test_legacy_recovery_ui_exports_only_bounded_semantics) {
    for (int semantic = LEGACY_RECOVERY_UI_SEMANTIC_NONE;
         semantic <= LEGACY_RECOVERY_UI_SEMANTIC_SUCCESS;
         semantic++) {
        const char *name = legacy_recovery_ui_semantic_name(
            (legacy_recovery_ui_semantic_t)semantic);
        ASSERT(name != NULL);
        ASSERT(strlen(name) > 0);
        ASSERT(strlen(name) < 32);
        ASSERT(strchr(name, '/') == NULL);
        ASSERT(strchr(name, '\\') == NULL);
    }
}

void register_legacy_recovery_ui_tests(void) {
    TEST_SECTION("\nLegacy recovery UI tests:\n");
    RUN(test_legacy_recovery_ui_rejects_zero_and_duplicate_offers);
    RUN(test_legacy_recovery_ui_requires_release_then_fresh_confirm);
    RUN(test_legacy_recovery_ui_cancel_wins_same_frame_and_is_one_shot);
    RUN(test_legacy_recovery_ui_transport_rejection_is_retryable);
    RUN(test_legacy_recovery_ui_expiry_is_terminal_and_wrap_safe);
    RUN(test_legacy_recovery_ui_maps_all_server_results);
    RUN(test_legacy_recovery_ui_ignores_unsolicited_invalid_and_duplicate_results);
    RUN(test_legacy_recovery_ui_result_closes_and_reset_allows_reconnect);
    RUN(test_legacy_recovery_ui_exports_only_bounded_semantics);
}
