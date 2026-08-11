#include "test_harness.h"

#include "reconciliation_diagnostics.h"

static net_reconcile_sample_t matching_reconciliation_sample(void)
{
    net_reconcile_pose_bits_t pose =
        net_reconcile_pose_bits(10.0f, -20.0f, 3.0f, -4.0f, 0.5f);
    return (net_reconcile_sample_t){
        .frontier_known = true,
        .entity_id = 2,
        .server_tick = 44,
        .prediction_tick = 44,
        .predicted_input_seq = 8,
        .authoritative_input_seq = 8,
        .predicted = pose,
        .authoritative = pose,
    };
}

TEST(test_reconciliation_diagnostics_require_bit_exact_pose)
{
    net_reconcile_sample_t sample = matching_reconciliation_sample();
    ASSERT_EQ_INT(net_reconcile_classify(&sample), NET_RECONCILE_EXACT);

    sample.predicted.pos_x ^= 1u;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_NUMERIC_DRIFT);
    ASSERT(!net_reconcile_pose_equal(&sample.predicted,
                                     &sample.authoritative));
}

TEST(test_reconciliation_diagnostics_classify_direct_frontier_mismatch)
{
    net_reconcile_sample_t sample = matching_reconciliation_sample();
    sample.predicted.pos_y ^= 1u;

    sample.prediction_tick++;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);
    sample.prediction_tick = sample.server_tick;

    sample.predicted_input_seq++;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);
    sample.predicted_input_seq = sample.authoritative_input_seq;

    sample.frontier_known = false;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);
}

TEST(test_reconciliation_diagnostics_frontier_window_is_bounded)
{
    net_reconcile_sample_t sample = matching_reconciliation_sample();
    sample.predicted.vel_x ^= 1u;
    sample.input_frontier = net_reconcile_tick_window(
        44, 45, NET_RECONCILE_CAUSE_REPLAY_GAP);
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);

    /* The same old window must not become a sticky excuse for later drift. */
    sample.server_tick = 46;
    sample.prediction_tick = 46;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_NUMERIC_DRIFT);
}

TEST(test_reconciliation_diagnostics_classify_bootstrap_and_recovery)
{
    net_reconcile_sample_t sample = matching_reconciliation_sample();
    sample.predicted.pos_y ^= 1u;

    sample.bootstrap = true;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_BOOTSTRAP);

    sample.bootstrap = false;
    sample.transport_recovery = net_reconcile_tick_window(
        44, 44, NET_RECONCILE_CAUSE_REBASE);
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_TRANSPORT_RECOVERY);

    sample.server_tick = 45;
    sample.prediction_tick = 45;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_NUMERIC_DRIFT);
}

TEST(test_reconciliation_diagnostics_stable_dock_is_not_semantic)
{
    net_reconcile_sample_t sample = matching_reconciliation_sample();
    sample.predicted.vel_y ^= 1u;
    sample.semantic = net_reconcile_tick_window(
        44, 44, NET_RECONCILE_CAUSE_DOCK);
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_SEMANTIC_DISCONTINUITY);

    /* A dock transition is one proven tick, not a permanent docked state. */
    sample.server_tick = 45;
    sample.prediction_tick = 45;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_NUMERIC_DRIFT);

    sample.semantic = net_reconcile_tick_window(
        45, 45, NET_RECONCILE_CAUSE_TOW_ATTACH);
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_SEMANTIC_DISCONTINUITY);
    sample.server_tick = 46;
    sample.prediction_tick = 46;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_NUMERIC_DRIFT);
}

TEST(test_reconciliation_diagnostics_pending_semantic_expires)
{
    net_reconcile_diagnostics_t diagnostics;
    net_reconcile_diagnostics_reset(&diagnostics);
    net_reconcile_note_semantic_window(
        &diagnostics, 50, 51, NET_RECONCILE_CAUSE_DEATH_RESPAWN);

    net_reconcile_tick_window_t before =
        net_reconcile_take_semantic_window(&diagnostics, 49);
    ASSERT(!before.valid);
    ASSERT(diagnostics.pending_semantic.valid);

    net_reconcile_tick_window_t event =
        net_reconcile_take_semantic_window(&diagnostics, 51);
    ASSERT(event.valid);
    ASSERT_EQ_INT(event.cause_mask, NET_RECONCILE_CAUSE_DEATH_RESPAWN);
    ASSERT(!diagnostics.pending_semantic.valid);

    net_reconcile_note_semantic_window(
        &diagnostics, 60, 61, NET_RECONCILE_CAUSE_TOW_RELEASE);
    net_reconcile_tick_window_t expired =
        net_reconcile_take_semantic_window(&diagnostics, 62);
    ASSERT(!expired.valid);
    ASSERT(!diagnostics.pending_semantic.valid);
}

TEST(test_reconciliation_diagnostics_compare_exact_movement_intent)
{
    input_intent_t a = {
        .thrust = 1.0f,
        .mining_target_hint = -1,
    };
    input_intent_t b = a;
    b.mining_target_hint = 42;
    ASSERT(net_reconcile_movement_intent_equal(&a, &b));

    a.mine = true;
    b.mine = true;
    ASSERT(!net_reconcile_movement_intent_equal(&a, &b));
    b.mining_target_hint = a.mining_target_hint;
    ASSERT(net_reconcile_movement_intent_equal(&a, &b));

    b.thrust = nextafterf(b.thrust, 2.0f);
    ASSERT(!net_reconcile_movement_intent_equal(&a, &b));
}

TEST(test_reconciliation_diagnostics_preserve_actionable_first_drift)
{
    const uint8_t root[NET_RECONCILE_ROOT_SIZE] = {
        0x12, 0x34, 0x56, 0x78,
    };
    net_reconcile_diagnostics_t diagnostics;
    net_reconcile_diagnostics_reset(&diagnostics);

    net_reconcile_sample_t drift = matching_reconciliation_sample();
    drift.predicted.vel_y ^= 1u;
    drift.authoritative_root = root;
    drift.input_frontier = net_reconcile_tick_window(
        42, 43, NET_RECONCILE_CAUSE_REPLAY_GAP);
    drift.semantic = net_reconcile_tick_window(
        45, 45, NET_RECONCILE_CAUSE_DOCK);
    drift.transport_recovery = net_reconcile_tick_window(
        46, 46, NET_RECONCILE_CAUSE_REBASE);
    ASSERT_EQ_INT(net_reconcile_diagnostics_observe(&diagnostics, &drift),
                  NET_RECONCILE_NUMERIC_DRIFT);

    net_reconcile_sample_t later = matching_reconciliation_sample();
    later.server_tick = 45;
    later.prediction_tick = 45;
    later.predicted.angle ^= 2u;
    ASSERT_EQ_INT(net_reconcile_diagnostics_observe(&diagnostics, &later),
                  NET_RECONCILE_NUMERIC_DRIFT);

    ASSERT_EQ_INT(diagnostics.total_samples, 2);
    ASSERT_EQ_INT(diagnostics.total_corrections, 2);
    ASSERT_EQ_INT(diagnostics.class_count[NET_RECONCILE_NUMERIC_DRIFT], 2);
    ASSERT(diagnostics.first_numeric_drift_valid);
    ASSERT_EQ_INT(diagnostics.first_domain,
                  NET_RECONCILE_DOMAIN_PLAYER_VEL_Y);
    ASSERT_EQ_INT(diagnostics.first_server_tick, 44);
    ASSERT_EQ_INT(diagnostics.first_prediction_tick, 44);
    ASSERT_EQ_INT(diagnostics.first_predicted_input_seq, 8);
    ASSERT_EQ_INT(diagnostics.first_authoritative_input_seq, 8);
    ASSERT_EQ_INT(diagnostics.first_authoritative_root[0], 0x12);
    ASSERT_EQ_INT(diagnostics.first_authoritative_root[3], 0x78);

    char json[NET_RECONCILE_JSON_SIZE];
    ASSERT(net_reconcile_first_drift_json(
        &diagnostics, "signal.authoritative_state.v1",
        json, sizeof(json)) > 0);
    ASSERT(strstr(json, "\"domain\":\"player.ship.vel.y\"") != NULL);
    ASSERT(strstr(json, "\"predicted_bits\":\"0x") != NULL);
    ASSERT(strstr(json, "\"input_cause_mask\":0") != NULL);
    ASSERT(strstr(json, "\"semantic_cause_mask\":0") != NULL);
    ASSERT(strstr(json, "\"transport_cause_mask\":0") != NULL);
    ASSERT(strstr(json, "\"authoritative_root\":\"12345678") != NULL);
}

TEST(test_reconciliation_diagnostics_nonfinite_values_are_valid_json)
{
    net_reconcile_diagnostics_t diagnostics;
    net_reconcile_diagnostics_reset(&diagnostics);

    net_reconcile_sample_t drift = matching_reconciliation_sample();
    drift.predicted.pos_x = 0x7fc00000u;
    drift.authoritative.pos_x = 0x7f800000u;
    ASSERT_EQ_INT(net_reconcile_diagnostics_observe(&diagnostics, &drift),
                  NET_RECONCILE_NUMERIC_DRIFT);

    char json[NET_RECONCILE_JSON_SIZE];
    ASSERT(net_reconcile_first_drift_json(
        &diagnostics, "signal.authoritative_state.v1",
        json, sizeof(json)) > 0);
    ASSERT(strstr(
        json, "\"predicted\":null,\"authoritative\":null") != NULL);
    ASSERT(strstr(json, "\"predicted_bits\":\"0x7fc00000\"") != NULL);
    ASSERT(strstr(json, "\"authoritative_bits\":\"0x7f800000\"") != NULL);
    ASSERT(strstr(json, "nan") == NULL);
    ASSERT(strstr(json, "inf") == NULL);
}

void register_reconciliation_diagnostics_tests(void)
{
    RUN(test_reconciliation_diagnostics_require_bit_exact_pose);
    RUN(test_reconciliation_diagnostics_classify_direct_frontier_mismatch);
    RUN(test_reconciliation_diagnostics_frontier_window_is_bounded);
    RUN(test_reconciliation_diagnostics_classify_bootstrap_and_recovery);
    RUN(test_reconciliation_diagnostics_stable_dock_is_not_semantic);
    RUN(test_reconciliation_diagnostics_pending_semantic_expires);
    RUN(test_reconciliation_diagnostics_compare_exact_movement_intent);
    RUN(test_reconciliation_diagnostics_preserve_actionable_first_drift);
    RUN(test_reconciliation_diagnostics_nonfinite_values_are_valid_json);
}
