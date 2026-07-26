#include "test_harness.h"

#include "reconciliation_diagnostics.h"

static net_reconcile_sample_t matching_sample(void)
{
    net_reconcile_pose_bits_t pose =
        net_reconcile_pose_bits(10.0f, -20.0f, 3.0f, -4.0f, 0.5f);
    return (net_reconcile_sample_t){
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
    net_reconcile_sample_t sample = matching_sample();
    ASSERT_EQ_INT(net_reconcile_classify(&sample), NET_RECONCILE_EXACT);

    sample.predicted.pos_x ^= 1u;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_NUMERIC_DRIFT);
    ASSERT(!net_reconcile_pose_equal(&sample.predicted,
                                     &sample.authoritative));
}

TEST(test_reconciliation_diagnostics_classify_expected_corrections)
{
    net_reconcile_sample_t sample = matching_sample();
    sample.predicted.pos_y ^= 1u;

    sample.bootstrap = true;
    ASSERT_EQ_INT(net_reconcile_classify(&sample), NET_RECONCILE_BOOTSTRAP);
    sample.bootstrap = false;

    sample.semantic_discontinuity = true;
    ASSERT_EQ_INT(net_reconcile_classify(&sample), NET_RECONCILE_SEMANTIC);
    sample.semantic_discontinuity = false;

    sample.transport_recovery = true;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_TRANSPORT_RECOVERY);
    sample.transport_recovery = false;

    sample.input_frontier = true;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);
    sample.input_frontier = false;

    sample.predicted_input_seq++;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);
    sample.predicted_input_seq = sample.authoritative_input_seq;

    sample.prediction_tick++;
    ASSERT_EQ_INT(net_reconcile_classify(&sample),
                  NET_RECONCILE_INPUT_FRONTIER);
}

TEST(test_reconciliation_diagnostics_compare_only_active_movement_intent)
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

    b.boost = true;
    ASSERT(!net_reconcile_movement_intent_equal(&a, &b));
}

TEST(test_reconciliation_diagnostics_preserve_first_numeric_drift)
{
    const uint8_t root[NET_RECONCILE_ROOT_SIZE] = {
        0x12, 0x34, 0x56, 0x78,
    };
    net_reconcile_diagnostics_t diagnostics;
    net_reconcile_diagnostics_reset(&diagnostics);

    net_reconcile_sample_t exact = matching_sample();
    ASSERT_EQ_INT(net_reconcile_diagnostics_observe(&diagnostics, &exact),
                  NET_RECONCILE_EXACT);

    net_reconcile_sample_t drift = matching_sample();
    drift.predicted.vel_y ^= 1u;
    drift.authoritative_root = root;
    ASSERT_EQ_INT(net_reconcile_diagnostics_observe(&diagnostics, &drift),
                  NET_RECONCILE_NUMERIC_DRIFT);

    net_reconcile_sample_t later = matching_sample();
    later.server_tick = 45;
    later.prediction_tick = 45;
    later.predicted.angle ^= 2u;
    ASSERT_EQ_INT(net_reconcile_diagnostics_observe(&diagnostics, &later),
                  NET_RECONCILE_NUMERIC_DRIFT);

    ASSERT_EQ_INT(diagnostics.total_samples, 3);
    ASSERT_EQ_INT(diagnostics.total_corrections, 2);
    ASSERT_EQ_INT(diagnostics.class_count[NET_RECONCILE_EXACT], 1);
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
}

void register_reconciliation_diagnostics_tests(void)
{
    RUN(test_reconciliation_diagnostics_require_bit_exact_pose);
    RUN(test_reconciliation_diagnostics_classify_expected_corrections);
    RUN(test_reconciliation_diagnostics_compare_only_active_movement_intent);
    RUN(test_reconciliation_diagnostics_preserve_first_numeric_drift);
}
