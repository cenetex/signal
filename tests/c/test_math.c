#include "test_harness.h"
#include "safe_types.h"
#include "camera_model.h"
#include "fixpoint.h"

TEST(test_v2_add) {
    vec2 a = v2(1.0f, 2.0f);
    vec2 b = v2(3.0f, 4.0f);
    vec2 c = v2_add(a, b);
    ASSERT_EQ_FLOAT(c.x, 4.0f, 0.001f);
    ASSERT_EQ_FLOAT(c.y, 6.0f, 0.001f);
}

TEST(test_v2_len) {
    vec2 a = v2(3.0f, 4.0f);
    ASSERT_EQ_FLOAT(v2_len(a), 5.0f, 0.001f);
}

TEST(test_v2_norm) {
    vec2 a = v2(0.0f, 5.0f);
    vec2 n = v2_norm(a);
    ASSERT_EQ_FLOAT(n.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(n.y, 1.0f, 0.001f);
}

TEST(test_v2_norm_zero) {
    vec2 a = v2(0.0f, 0.0f);
    vec2 n = v2_norm(a);
    ASSERT_EQ_FLOAT(n.x, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(n.y, 0.0f, 0.001f);
}

TEST(test_v2_math_uses_deterministic_fixed_helpers) {
    vec2 a = v2(123.25f, -456.5f);
    ASSERT_EQ_FLOAT(v2_len(a), fixp_sqrtf(v2_len_sq(a)), 0.0001f);

    float angles[] = { -3.0f, -1.0f, 0.0f, 0.75f, 2.5f };
    for (int i = 0; i < (int)(sizeof(angles) / sizeof(angles[0])); i++) {
        vec2 dir = v2_from_angle(angles[i]);
        ASSERT_EQ_FLOAT(dir.x, fixp_cosf(angles[i]), 0.0001f);
        ASSERT_EQ_FLOAT(dir.y, fixp_sinf(angles[i]), 0.0001f);
        ASSERT_EQ_FLOAT(v2_len(dir), 1.0f, 0.01f);
    }
}

TEST(test_fixpoint_transcendentals_track_libm_envelope) {
    float values[] = { -3.0f, -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 3.0f };
    for (int i = 0; i < (int)(sizeof(values) / sizeof(values[0])); i++) {
        float x = values[i];
        ASSERT_EQ_FLOAT(fixp_sinf(x), sinf(x), 0.01f);
        ASSERT_EQ_FLOAT(fixp_cosf(x), cosf(x), 0.01f);
        ASSERT_EQ_FLOAT(fixp_sqrtf(fabsf(x)), sqrtf(fabsf(x)), 0.01f);
    }
    ASSERT_EQ_FLOAT(fixp_atan2f(1.0f, 1.0f), atan2f(1.0f, 1.0f), 0.02f);
    ASSERT_EQ_FLOAT(fixp_atan2f(-2.0f, 3.0f), atan2f(-2.0f, 3.0f), 0.02f);
}

TEST(test_fixpoint_transcendentals_handle_nonfinite_inputs) {
    ASSERT_EQ_FLOAT(fixp_sqrtf(INFINITY), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(fixp_sinf(INFINITY), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(fixp_cosf(-INFINITY), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(fixp_atan2f(INFINITY, 1.0f), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(fixp_tanf(NAN), 0.0f, 0.001f);

    vec2 dir = v2_from_angle(-INFINITY);
    ASSERT_EQ_FLOAT(dir.x, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(dir.y, 0.0f, 0.001f);
}

TEST(test_wrap_angle) {
    ASSERT_EQ_FLOAT(wrap_angle(0.0f), 0.0f, 0.001f);
    ASSERT(wrap_angle(4.0f) < PI_F);
    ASSERT(wrap_angle(-4.0f) > -PI_F);
}

TEST(test_clampf) {
    ASSERT_EQ_FLOAT(clampf(0.5f, 0.0f, 1.0f), 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(clampf(-1.0f, 0.0f, 1.0f), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(clampf(2.0f, 0.0f, 1.0f), 1.0f, 0.001f);
}

TEST(test_lerpf) {
    ASSERT_EQ_FLOAT(lerpf(0.0f, 10.0f, 0.5f), 5.0f, 0.001f);
    ASSERT_EQ_FLOAT(lerpf(0.0f, 10.0f, 0.0f), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(lerpf(0.0f, 10.0f, 1.0f), 10.0f, 0.001f);
}

TEST(test_ingot_idx) {
    ASSERT_EQ_INT(INGOT_IDX(COMMODITY_FERRITE_INGOT), 0);
    ASSERT_EQ_INT(INGOT_IDX(COMMODITY_CUPRITE_INGOT), 1);
    ASSERT_EQ_INT(INGOT_IDX(COMMODITY_CRYSTAL_INGOT), 2);
    ASSERT_EQ_INT(INGOT_COUNT, 7);
}

TEST(test_signal_checked_size_arithmetic) {
    size_t out = 0;
    ASSERT(signal_checked_add_size(40u, 2u, &out));
    ASSERT_EQ_INT((int)out, 42);
    ASSERT(!signal_checked_add_size((size_t)-1, 1u, &out));

    ASSERT(signal_checked_mul_size(6u, 7u, &out));
    ASSERT_EQ_INT((int)out, 42);
    ASSERT(!signal_checked_mul_size(((size_t)-1 / 2u) + 1u, 2u, &out));
    ASSERT(!signal_checked_mul_size(2u, 2u, NULL));
}

TEST(test_camera_narrow_focus_tightens_portrait_deadzone) {
    ASSERT_EQ_FLOAT(camera_narrow_focus(1280.0f, 720.0f), 0.0f, 0.001f);
    ASSERT(camera_narrow_focus(390.0f, 760.0f) > 0.99f);
    ASSERT(camera_deadzone_x_scale(1.0f) < camera_deadzone_x_scale(0.0f));
    ASSERT(camera_deadzone_y_scale(1.0f) < camera_deadzone_y_scale(0.0f));
    ASSERT_EQ_FLOAT(camera_deadzone_x_scale(1.0f), 0.23f, 0.001f);
    ASSERT_EQ_FLOAT(camera_deadzone_y_scale(1.0f), 0.22f, 0.001f);
    ASSERT_EQ_FLOAT(camera_narrow_center_strength(1.0f), 0.72f, 0.001f);
}

void register_math_tests(void) {
    TEST_SECTION("\nMath tests:\n");
    RUN(test_v2_add);
    RUN(test_v2_len);
    RUN(test_v2_norm);
    RUN(test_v2_norm_zero);
    RUN(test_v2_math_uses_deterministic_fixed_helpers);
    RUN(test_fixpoint_transcendentals_track_libm_envelope);
    RUN(test_fixpoint_transcendentals_handle_nonfinite_inputs);
    RUN(test_wrap_angle);
    RUN(test_clampf);
    RUN(test_lerpf);

    TEST_SECTION("\nType tests:\n");
    RUN(test_ingot_idx);
    RUN(test_signal_checked_size_arithmetic);

    TEST_SECTION("\nCamera model tests:\n");
    RUN(test_camera_narrow_focus_tightens_portrait_deadzone);
}
