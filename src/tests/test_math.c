#include "tests/test_harness.h"

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

void register_math_tests(void) {
    TEST_SECTION("\nMath tests:\n");
    RUN(test_v2_add);
    RUN(test_v2_len);
    RUN(test_v2_norm);
    RUN(test_v2_norm_zero);
    RUN(test_wrap_angle);
    RUN(test_clampf);
    RUN(test_lerpf);

    TEST_SECTION("\nType tests:\n");
    RUN(test_ingot_idx);
}
