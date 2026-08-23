#include "test_harness.h"

#include "gameplay_observability.h"

TEST(test_gameplay_observability_reports_bounded_runtime_evidence)
{
    gameplay_observability_configure(true, 60.0);
    gameplay_observability_frame_begin();
    double simulation_started = gameplay_observability_now() - 0.004;
    gameplay_observability_phase_end(
        GAMEPLAY_PHASE_SIMULATION, simulation_started);
    gameplay_observability_record_sim_steps(8, 3, 3);
    gameplay_observability_record_packet(0x4b, 321);
    gameplay_observability_record_entity_correction(
        GAMEPLAY_ENTITY_ASTEROID, 2.5f, 18.0f, 0.1f);
    gameplay_observability_frame_end();

    const char *report = gameplay_observability_report_json();
    ASSERT(report != NULL);
    ASSERT(strstr(report, "\"schema\":\"signal.gameplay-jank.v1\"") != NULL);
    ASSERT(strstr(report, "\"completed\":8") != NULL);
    ASSERT(strstr(report, "\"missed\":3") != NULL);
    ASSERT(strstr(report, "\"accumulator_dropped\":3") != NULL);
    ASSERT(strstr(report, "\"packets\":1") != NULL);
    ASSERT(strstr(report, "\"max_packet_bytes\":321") != NULL);
    ASSERT(strstr(report, "\"asteroid\":{\"samples\":1") != NULL);
    ASSERT(strstr(report, "\"simulation\":{\"avg_ms\":") != NULL);
}

TEST(test_gameplay_observability_disabled_path_stays_empty)
{
    gameplay_observability_configure(false, 60.0);
    gameplay_observability_frame_begin();
    gameplay_observability_record_sim_steps(4, 2, 2);
    gameplay_observability_record_packet(1, 99);
    gameplay_observability_frame_end();

    const char *report = gameplay_observability_report_json();
    ASSERT(strstr(report, "\"enabled\":false") != NULL);
    ASSERT(strstr(report, "\"frames\":0") != NULL);
    ASSERT(strstr(report, "\"completed\":0") != NULL);
    ASSERT(strstr(report, "\"packets\":0") != NULL);
}

void register_gameplay_observability_tests(void)
{
    TEST_SECTION("\nGameplay jank observability (#687):\n");
    RUN(test_gameplay_observability_reports_bounded_runtime_evidence);
    RUN(test_gameplay_observability_disabled_path_stays_empty);
}
