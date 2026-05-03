/*
 * test_laser.c — unit tests for the unified laser primitive.
 *
 * Pure math tests pinning the behavior (geometry hit/miss, effect
 * accumulation + clamp + decay) so the migration sites in L2/L3
 * have a stable reference.
 */
#include "tests/test_harness.h"
#include "laser.h"

TEST(test_laser_target_in_thin_ray_hits_on_axis) {
    /* Ray along +X from origin, range 100, thin (cone=0). Target at
     * (50, 0) with radius 5 — directly on the axis, in range. */
    laser_ray_t ray = {
        .source_pos = v2(0.0f, 0.0f),
        .source_dir = v2(1.0f, 0.0f),
        .range = 100.0f,
        .cone_half_angle = 0.0f,
    };
    vec2 hit;
    ASSERT(laser_target_in_beam(&ray, v2(50.0f, 0.0f), 5.0f, &hit, NULL));
    /* Hit position biased 85% of radius back toward source from
     * target center → x = 50 - 5*0.85 = 45.75. */
    ASSERT_EQ_FLOAT(hit.x, 45.75f, 0.01f);
    ASSERT_EQ_FLOAT(hit.y, 0.0f,   0.01f);
}

TEST(test_laser_target_in_thin_ray_misses_off_axis) {
    /* Same ray, target offset perpendicular to the axis by more than
     * the target radius — should miss. */
    laser_ray_t ray = {
        .source_pos = v2(0.0f, 0.0f),
        .source_dir = v2(1.0f, 0.0f),
        .range = 100.0f,
        .cone_half_angle = 0.0f,
    };
    /* Target center 20u off axis, radius 5 → perp 20 > radius 5 → miss. */
    ASSERT(!laser_target_in_beam(&ray, v2(50.0f, 20.0f), 5.0f, NULL, NULL));
    /* Just barely on-axis — perp 4.9 < radius 5 → hit. */
    ASSERT(laser_target_in_beam(&ray, v2(50.0f, 4.9f), 5.0f, NULL, NULL));
}

TEST(test_laser_cone_widens_with_distance) {
    /* Cone half-angle = 30°. At along=10u the cone has tolerance
     * 10*tan(30°) ≈ 5.77u + target radius. At along=100u: ~57.7u. */
    laser_ray_t ray = {
        .source_pos = v2(0.0f, 0.0f),
        .source_dir = v2(1.0f, 0.0f),
        .range = 200.0f,
        .cone_half_angle = 30.0f * (PI_F / 180.0f),
    };
    /* Target 10u perpendicular at d=10 → allowed 5.77 + 1 = 6.77; perp
     * 10 > 6.77 → miss. */
    ASSERT(!laser_target_in_beam(&ray, v2(10.0f, 10.0f), 1.0f, NULL, NULL));
    /* Same target at d=100 → allowed ~58.7; perp 10 < 58.7 → hit. */
    ASSERT(laser_target_in_beam(&ray, v2(100.0f, 10.0f), 1.0f, NULL, NULL));
}

TEST(test_laser_range_gate_disengages) {
    /* Target past the ray's range — no hit even when on-axis. */
    laser_ray_t ray = {
        .source_pos = v2(0.0f, 0.0f),
        .source_dir = v2(1.0f, 0.0f),
        .range = 100.0f,
        .cone_half_angle = 0.0f,
    };
    /* Center at d=120, radius 5 → effective_range = 100 + 5 = 105; out. */
    ASSERT(!laser_target_in_beam(&ray, v2(120.0f, 0.0f), 5.0f, NULL, NULL));
    /* Center at d=104 with radius 5 → effective_range covers it. */
    ASSERT(laser_target_in_beam(&ray, v2(104.0f, 0.0f), 5.0f, NULL, NULL));
}

TEST(test_laser_target_behind_source_misses) {
    /* Ray fires +X. Target at -X (behind source) — never a hit. */
    laser_ray_t ray = {
        .source_pos = v2(0.0f, 0.0f),
        .source_dir = v2(1.0f, 0.0f),
        .range = 100.0f,
        .cone_half_angle = 0.0f,
    };
    ASSERT(!laser_target_in_beam(&ray, v2(-50.0f, 0.0f), 5.0f, NULL, NULL));
}

TEST(test_laser_apply_effect_accumulates) {
    /* Positive effect adds to the field. With dt=1 and effect=10/sec,
     * one tick adds 10. */
    float field = 0.0f;
    laser_apply_effect(&field, 10.0f, 0.0f, 1.0f);
    ASSERT_EQ_FLOAT(field, 10.0f, 0.001f);
    /* Another tick stacks. */
    laser_apply_effect(&field, 10.0f, 0.0f, 1.0f);
    ASSERT_EQ_FLOAT(field, 20.0f, 0.001f);
}

TEST(test_laser_apply_effect_clamps_to_max) {
    /* With max_value=15 and field=10, applying 10 more clamps at 15. */
    float field = 10.0f;
    laser_apply_effect(&field, 10.0f, 15.0f, 1.0f);
    ASSERT_EQ_FLOAT(field, 15.0f, 0.001f);
    /* max_value=0 disables the upper clamp. */
    field = 10.0f;
    laser_apply_effect(&field, 10.0f, 0.0f, 1.0f);
    ASSERT_EQ_FLOAT(field, 20.0f, 0.001f);
}

TEST(test_laser_apply_effect_floors_at_zero) {
    /* Negative effect (decay) but field never goes below zero. */
    float field = 0.3f;
    laser_apply_effect(&field, -1.0f, 0.0f, 1.0f);
    ASSERT_EQ_FLOAT(field, 0.0f, 0.001f);
    /* Already zero stays zero. */
    laser_apply_effect(&field, -1.0f, 0.0f, 1.0f);
    ASSERT_EQ_FLOAT(field, 0.0f, 0.001f);
}

TEST(test_laser_apply_effect_null_target_safe) {
    /* Sanity: passing NULL target_field is a no-op (no crash). */
    laser_apply_effect(NULL, 10.0f, 0.0f, 1.0f);
    /* Reaching this line means no SIGSEGV. */
    ASSERT(true);
}

void register_laser_tests(void) {
    TEST_SECTION("\nLaser primitive (L1):\n");
    RUN(test_laser_target_in_thin_ray_hits_on_axis);
    RUN(test_laser_target_in_thin_ray_misses_off_axis);
    RUN(test_laser_cone_widens_with_distance);
    RUN(test_laser_range_gate_disengages);
    RUN(test_laser_target_behind_source_misses);
    RUN(test_laser_apply_effect_accumulates);
    RUN(test_laser_apply_effect_clamps_to_max);
    RUN(test_laser_apply_effect_floors_at_zero);
    RUN(test_laser_apply_effect_null_target_safe);
}
