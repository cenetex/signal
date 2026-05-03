/*
 * test_tractor.c — unit tests for the unified tractor primitive.
 *
 * Pure math tests with no world dependency. They pin the behavior
 * (push/pull/range/falloff/damping/reaction/cap) so the call-site
 * migrations in R2-R5 can be evaluated for "behavior preserved" or
 * "behavior changed by 1D damping" with a stable reference point.
 */
#include "tests/test_harness.h"
#include "tractor.h"

/* Convenience: anchor that's body-attached with given vel + inv_mass. */
static tractor_anchor_t mk_body_anchor(vec2 pos, vec2 *vel, float inv_mass) {
    tractor_anchor_t a = { .pos = pos, .vel = vel, .inv_mass = inv_mass };
    return a;
}

/* Convenience: anchor that's world-pinned (no reaction force possible). */
static tractor_anchor_t mk_world_anchor(vec2 pos) {
    tractor_anchor_t a = { .pos = pos, .vel = NULL, .inv_mass = 0.0f };
    return a;
}

TEST(test_tractor_pull_engages_beyond_rest) {
    /* Body at d=10 along +X from origin. rest=5, pull=2, push=0.
     * stretch = 5; spring_mag = -2 * 5 = -10 (toward source, i.e. -X).
     * Single tick at dt=1 → vel goes from 0 to -10 along x. */
    vec2 tgt_vel = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(10.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 5.0f, .pull_strength = 2.0f, .push_strength = 0.0f,
        .range = 100.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    bool active = tractor_apply(&src, &tgt, &beam, 1.0f);
    ASSERT(active);
    ASSERT_EQ_FLOAT(tgt_vel.x, -10.0f, 0.001f);
    ASSERT_EQ_FLOAT(tgt_vel.y, 0.0f, 0.001f);
}

TEST(test_tractor_push_engages_below_rest) {
    /* Body at d=2 along +X. rest=5, pull=0, push=3.
     * stretch = -3; spring_mag = -3 * -3 = +9 (away from source, +X). */
    vec2 tgt_vel = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(2.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 5.0f, .pull_strength = 0.0f, .push_strength = 3.0f,
        .range = 100.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    ASSERT_EQ_FLOAT(tgt_vel.x, 9.0f, 0.001f);
    ASSERT_EQ_FLOAT(tgt_vel.y, 0.0f, 0.001f);
}

TEST(test_tractor_constant_pull_independent_of_stretch) {
    /* pull_constant=10 with no spring → same force regardless of how
     * far past rest the body is. Models a "thruster on the rope"
     * that yanks the fragment in at a fixed rate (NPC pickup tow). */
    vec2 vel_near = v2(0.0f, 0.0f);
    vec2 vel_far  = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_beam_t   beam = {
        .rest_length = 5.0f, .pull_strength = 0.0f, .push_strength = 0.0f,
        .pull_constant = 10.0f, .push_constant = 0.0f,
        .range = 1000.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    tractor_anchor_t tgt_near = mk_body_anchor(v2(7.0f,   0.0f), &vel_near, 1.0f);
    tractor_anchor_t tgt_far  = mk_body_anchor(v2(500.0f, 0.0f), &vel_far,  1.0f);
    ASSERT(tractor_apply(&src, &tgt_near, &beam, 1.0f));
    ASSERT(tractor_apply(&src, &tgt_far,  &beam, 1.0f));
    /* Both bodies pulled toward source at the same constant rate. */
    ASSERT_EQ_FLOAT(vel_near.x, -10.0f, 0.001f);
    ASSERT_EQ_FLOAT(vel_far.x,  -10.0f, 0.001f);
}

TEST(test_tractor_zero_strength_no_force) {
    /* pull=0, push=0 → no spring force regardless of distance. With
     * zero damping too, the body's velocity is unchanged. */
    vec2 tgt_vel = v2(7.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(50.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 0.0f, .pull_strength = 0.0f, .push_strength = 0.0f,
        .range = 1000.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    ASSERT_EQ_FLOAT(tgt_vel.x, 7.0f, 0.001f);
    ASSERT_EQ_FLOAT(tgt_vel.y, 0.0f, 0.001f);
}

TEST(test_tractor_range_gate_disengages) {
    /* Body at d=20, range=15 → tractor_apply returns false and target
     * velocity is untouched. */
    vec2 tgt_vel = v2(3.0f, 4.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(20.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 0.0f, .pull_strength = 99.0f, .push_strength = 0.0f,
        .range = 15.0f, .axial_damping = 99.0f, .tangent_damping = 99.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    bool active = tractor_apply(&src, &tgt, &beam, 1.0f);
    ASSERT(!active);
    ASSERT_EQ_FLOAT(tgt_vel.x, 3.0f, 0.001f);
    ASSERT_EQ_FLOAT(tgt_vel.y, 4.0f, 0.001f);
}

TEST(test_tractor_linear_falloff_halves_at_half_range) {
    /* d = range/2 with FALLOFF_LINEAR → spring scaled by (1 - 0.5) = 0.5.
     * Compare against a reference run with FALLOFF_CONSTANT (no scaling). */
    vec2 tgt_vel_const = v2(0.0f, 0.0f);
    vec2 tgt_vel_lin   = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));

    tractor_beam_t base = {
        .rest_length = 0.0f, .pull_strength = 1.0f, .push_strength = 0.0f,
        .range = 20.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    tractor_anchor_t tgt_c = mk_body_anchor(v2(10.0f, 0.0f), &tgt_vel_const, 1.0f);
    ASSERT(tractor_apply(&src, &tgt_c, &base, 1.0f));

    base.falloff = TRACTOR_FALLOFF_LINEAR;
    tractor_anchor_t tgt_l = mk_body_anchor(v2(10.0f, 0.0f), &tgt_vel_lin, 1.0f);
    ASSERT(tractor_apply(&src, &tgt_l, &base, 1.0f));

    /* Linear value should be exactly half the constant value. */
    ASSERT_EQ_FLOAT(tgt_vel_lin.x, tgt_vel_const.x * 0.5f, 0.001f);
}

TEST(test_tractor_axial_vs_tangent_damping_isolation) {
    /* Beam along +X. Axial-only damping applied: a body moving along
     * +X gets slowed; a body moving along +Y is untouched. Then swap:
     * tangent-only damping applied: body moving +X untouched, body
     * moving +Y gets slowed. Tests that the two damping knobs are
     * independent and act along orthogonal axes. */
    vec2 vel_along  = v2(5.0f, 0.0f);
    vec2 vel_tangent = v2(0.0f, 5.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));

    /* Use a position offset from origin so the line-of-action is
     * defined and the body is in range. Disable spring + falloff so
     * only damping moves the velocity. */
    tractor_beam_t beam_axial = {
        .rest_length = 100.0f, .pull_strength = 0.0f, .push_strength = 0.0f,
        .range = 100.0f, .axial_damping = 1.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    tractor_beam_t beam_tangent = beam_axial;
    beam_tangent.axial_damping = 0.0f;
    beam_tangent.tangent_damping = 1.0f;

    tractor_anchor_t tgt_along  = mk_body_anchor(v2(10.0f, 0.0f), &vel_along, 1.0f);
    tractor_anchor_t tgt_tangent = mk_body_anchor(v2(10.0f, 0.0f), &vel_tangent, 1.0f);

    /* Axial damping case: body moving along the beam line slows; body
     * moving perpendicular is untouched. */
    vec2 saved_along  = vel_along;
    vec2 saved_tangent = vel_tangent;
    ASSERT(tractor_apply(&src, &tgt_along,  &beam_axial, 1.0f));
    ASSERT(tractor_apply(&src, &tgt_tangent, &beam_axial, 1.0f));
    ASSERT(vel_along.x  < saved_along.x);   /* axial slowed */
    ASSERT_EQ_FLOAT(vel_tangent.y, saved_tangent.y, 0.001f); /* tangent untouched */

    /* Reset velocities and retry with tangent-only damping. */
    vel_along  = v2(5.0f, 0.0f);
    vel_tangent = v2(0.0f, 5.0f);
    ASSERT(tractor_apply(&src, &tgt_along,  &beam_tangent, 1.0f));
    ASSERT(tractor_apply(&src, &tgt_tangent, &beam_tangent, 1.0f));
    ASSERT_EQ_FLOAT(vel_along.x, 5.0f, 0.001f);   /* axial untouched */
    ASSERT(vel_tangent.y < 5.0f);                  /* tangent slowed */
}

TEST(test_tractor_reaction_symmetry_conserves_momentum) {
    /* Both anchors body-attached with inv_mass=1 → impulses are
     * equal-and-opposite, so total momentum (vel sum) is preserved. */
    vec2 src_vel = v2(0.0f, 0.0f);
    vec2 tgt_vel = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_body_anchor(v2(0.0f, 0.0f), &src_vel, 1.0f);
    tractor_anchor_t tgt = mk_body_anchor(v2(10.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 5.0f, .pull_strength = 2.0f, .push_strength = 0.0f,
        .range = 100.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    /* Target pulled toward source; source kicked away from target.
     * Momentum sum stays zero. */
    ASSERT_EQ_FLOAT(src_vel.x + tgt_vel.x, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(src_vel.y + tgt_vel.y, 0.0f, 0.001f);
    ASSERT(tgt_vel.x < 0.0f);   /* target pulled toward source (-X) */
    ASSERT(src_vel.x > 0.0f);   /* source pulled toward target (+X) */
}

TEST(test_tractor_world_pinned_source_no_reaction) {
    /* src.vel = NULL → no reaction force computed even if the math
     * would otherwise apply. Target gets full impulse; source state
     * unchanged. */
    vec2 tgt_vel = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(10.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 5.0f, .pull_strength = 2.0f, .push_strength = 0.0f,
        .range = 100.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 0.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    ASSERT_EQ_FLOAT(tgt_vel.x, -10.0f, 0.001f);
    /* Source has no vel field — nothing to assert on it directly,
     * but the symmetry test above is the canary for accidental
     * reaction-force mutation. */
}

TEST(test_tractor_speed_cap_clamps_target) {
    /* Apply an impulse that would drive vel.x past the cap. The cap
     * is on |target.vel| (isotropic), so the result is normalized to
     * the cap magnitude along whatever direction the velocity ends up. */
    vec2 tgt_vel = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(100.0f, 0.0f), &tgt_vel, 1.0f);
    tractor_beam_t   beam = {
        .rest_length = 0.0f, .pull_strength = 100.0f, .push_strength = 0.0f,
        .range = 1000.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 50.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };
    /* Without cap the impulse would push vel.x to -10000. With cap=50
     * the |vel| should land at 50 along the line of action (-X). */
    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    float spd = sqrtf(tgt_vel.x * tgt_vel.x + tgt_vel.y * tgt_vel.y);
    ASSERT_EQ_FLOAT(spd, 50.0f, 0.001f);
    ASSERT(tgt_vel.x < 0.0f);  /* still pulled toward source */
}

void register_tractor_tests(void) {
    TEST_SECTION("\nTractor primitive (R1):\n");
    RUN(test_tractor_pull_engages_beyond_rest);
    RUN(test_tractor_push_engages_below_rest);
    RUN(test_tractor_constant_pull_independent_of_stretch);
    RUN(test_tractor_zero_strength_no_force);
    RUN(test_tractor_range_gate_disengages);
    RUN(test_tractor_linear_falloff_halves_at_half_range);
    RUN(test_tractor_axial_vs_tangent_damping_isolation);
    RUN(test_tractor_reaction_symmetry_conserves_momentum);
    RUN(test_tractor_world_pinned_source_no_reaction);
    RUN(test_tractor_speed_cap_clamps_target);
}
