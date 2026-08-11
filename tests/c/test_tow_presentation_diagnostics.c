#include "test_harness.h"

#include "tow_presentation_diagnostics.h"

static void build_schedule_packets(
    tow_adverse_packet_t packets[48])
{
    for (int i = 0; i < 48; i++) {
        packets[i] = (tow_adverse_packet_t){
            .sequence = (uint32_t)i + 1u,
            .send_ms = (uint32_t)i * 25u,
            .payload_index = (uint32_t)i / 2u,
            .channel = (i & 1)
                ? TOW_ADVERSE_CHANNEL_RELATION
                : TOW_ADVERSE_CHANNEL_TARGET,
        };
    }
}

TEST(test_tow_adverse_scheduler_is_deterministic_and_bounded)
{
    static const uint16_t latencies[] = {50, 125, 250};
    tow_adverse_packet_t packets[48];
    build_schedule_packets(packets);
    for (size_t profile_index = 0;
         profile_index < sizeof(latencies) / sizeof(latencies[0]);
         profile_index++) {
        tow_adverse_profile_t profile =
            tow_adverse_profile(latencies[profile_index]);
        tow_adverse_delivery_t first[TOW_ADVERSE_MAX_DELIVERIES];
        tow_adverse_delivery_t second[TOW_ADVERSE_MAX_DELIVERIES];
        tow_adverse_schedule_stats_t first_stats = {0};
        tow_adverse_schedule_stats_t second_stats = {0};
        int first_count = tow_adverse_schedule(
            &profile, packets, 48, first,
            TOW_ADVERSE_MAX_DELIVERIES, &first_stats);
        int second_count = tow_adverse_schedule(
            &profile, packets, 48, second,
            TOW_ADVERSE_MAX_DELIVERIES, &second_stats);

        ASSERT(first_count > 0);
        ASSERT_EQ_INT(first_count, second_count);
        for (int i = 0; i < first_count; i++) {
            ASSERT(first[i].packet.sequence ==
                   second[i].packet.sequence);
            ASSERT(first[i].packet.send_ms ==
                   second[i].packet.send_ms);
            ASSERT(first[i].packet.payload_index ==
                   second[i].packet.payload_index);
            ASSERT_EQ_INT(first[i].packet.channel,
                          second[i].packet.channel);
            ASSERT(first[i].delivery_ms == second[i].delivery_ms);
            ASSERT_EQ_INT(first[i].jitter_ms, second[i].jitter_ms);
            ASSERT_EQ_INT(first[i].reorder_ms, second[i].reorder_ms);
            ASSERT_EQ_INT(first[i].duplicate_ordinal,
                          second[i].duplicate_ordinal);
        }
        ASSERT(first_stats.input_packets ==
               second_stats.input_packets);
        ASSERT(first_stats.delivered_packets ==
               second_stats.delivered_packets);
        ASSERT(first_stats.dropped_packets ==
               second_stats.dropped_packets);
        ASSERT(first_stats.duplicated_packets ==
               second_stats.duplicated_packets);
        ASSERT(first_stats.reordered_packets ==
               second_stats.reordered_packets);
        ASSERT_EQ_INT(first_stats.max_abs_jitter_ms,
                      second_stats.max_abs_jitter_ms);
        ASSERT_EQ_INT(first_stats.max_abs_reorder_ms,
                      second_stats.max_abs_reorder_ms);
        ASSERT(first_stats.dropped_packets > 0);
        ASSERT(first_stats.duplicated_packets > 0);
        ASSERT(first_stats.reordered_packets > 0);
        ASSERT(first_stats.max_abs_jitter_ms <=
               profile.max_jitter_ms);
        ASSERT(first_stats.max_abs_reorder_ms <=
               profile.max_reorder_ms);
        for (int i = 1; i < first_count; i++)
            ASSERT(first[i - 1].delivery_ms <= first[i].delivery_ms);
    }
}

TEST(test_tow_adverse_scheduler_rejects_timestamp_overflow)
{
    tow_adverse_profile_t profile = tow_adverse_profile(250);
    tow_adverse_packet_t packet = {
        .sequence = 5,
        .send_ms = UINT32_MAX,
        .payload_index = 0,
        .channel = TOW_ADVERSE_CHANNEL_TARGET,
    };
    tow_adverse_delivery_t deliveries[2];
    ASSERT_EQ_INT(
        tow_adverse_schedule(
            &profile, &packet, 1, deliveries, 2, NULL),
        -1);
}

static void observe_smooth_presentation(
    tow_presentation_diagnostics_t *diagnostics,
    int frames,
    bool refresh_snapshots)
{
    const float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < frames; frame++) {
        float time = (float)frame * dt;
        vec2 pos = v2(10.0f * time, 2.0f);
        vec2 vel = v2(10.0f, 0.0f);
        if (refresh_snapshots && frame % 3 == 0) {
            tow_presentation_diagnostics_snapshot(
                diagnostics, pos, vel,
                v2(pos.x + 0.25f, pos.y), vel);
        }
        tow_presentation_diagnostics_frame(
            diagnostics, true, pos, dt,
            TOW_PRESENTATION_TEST_PIXELS_PER_WORLD);
    }
}

TEST(test_tow_presentation_metrics_pass_fixed_smooth_thresholds)
{
    tow_presentation_diagnostics_t diagnostics;
    tow_presentation_diagnostics_reset(&diagnostics);
    observe_smooth_presentation(&diagnostics, 30, true);

    ASSERT(diagnostics.snapshot_samples >= 10);
    ASSERT(diagnostics.jerk_samples > 0);
    ASSERT(diagnostics.max_correction_world < 0.3f);
    ASSERT(diagnostics.max_snapshot_gap_sec < 0.06f);
    ASSERT(tow_presentation_diagnostics_within_thresholds(
        &diagnostics));
}

TEST(test_tow_presentation_metrics_fail_authored_threshold_breaches)
{
    tow_presentation_diagnostics_t correction;
    tow_presentation_diagnostics_reset(&correction);
    observe_smooth_presentation(&correction, 8, true);
    tow_presentation_diagnostics_snapshot(
        &correction, v2(0.0f, 0.0f), v2(0.0f, 0.0f),
        v2(TOW_PRESENTATION_MAX_CORRECTION_WORLD + 1.0f, 0.0f),
        v2(0.0f, 0.0f));
    ASSERT(!tow_presentation_diagnostics_within_thresholds(
        &correction));

    tow_presentation_diagnostics_t starvation;
    tow_presentation_diagnostics_reset(&starvation);
    tow_presentation_diagnostics_snapshot(
        &starvation, v2(0.0f, 0.0f), v2(1.0f, 0.0f),
        v2(0.0f, 0.0f), v2(1.0f, 0.0f));
    observe_smooth_presentation(&starvation, 20, false);
    ASSERT(starvation.starvation_events > 0);
    ASSERT(starvation.max_snapshot_gap_sec >
           TOW_PRESENTATION_MAX_SNAPSHOT_GAP_SEC);
    ASSERT(!tow_presentation_diagnostics_within_thresholds(
        &starvation));

    tow_presentation_diagnostics_t jerk;
    tow_presentation_diagnostics_reset(&jerk);
    tow_presentation_diagnostics_snapshot(
        &jerk, v2(0.0f, 0.0f), v2(0.0f, 0.0f),
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    const float dt = 1.0f / 60.0f;
    tow_presentation_diagnostics_frame(
        &jerk, true, v2(0.0f, 0.0f), dt, 2.0f);
    tow_presentation_diagnostics_frame(
        &jerk, true, v2(0.0f, 0.0f), dt, 2.0f);
    tow_presentation_diagnostics_frame(
        &jerk, true, v2(0.0f, 0.0f), dt, 2.0f);
    tow_presentation_diagnostics_frame(
        &jerk, true, v2(100.0f, 0.0f), dt, 2.0f);
    ASSERT(jerk.max_world_jerk >
           TOW_PRESENTATION_MAX_WORLD_JERK);
    ASSERT(!tow_presentation_diagnostics_within_thresholds(&jerk));
}

void register_tow_presentation_diagnostics_tests(void)
{
    TEST_SECTION("\nTow presentation adverse-network diagnostics:\n");
    RUN(test_tow_adverse_scheduler_is_deterministic_and_bounded);
    RUN(test_tow_adverse_scheduler_rejects_timestamp_overflow);
    RUN(test_tow_presentation_metrics_pass_fixed_smooth_thresholds);
    RUN(test_tow_presentation_metrics_fail_authored_threshold_breaches);
}
