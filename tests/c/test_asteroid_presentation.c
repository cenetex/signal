#include "test_harness.h"

#include "asteroid_presentation.h"
#include "sim_physics.h"

static asteroid_t presentation_asteroid(vec2 pos, vec2 vel)
{
    return (asteroid_t){
        .active = true,
        .fracture_child = true,
        .tier = ASTEROID_TIER_S,
        .pos = pos,
        .vel = vel,
        .radius = 12.0f,
        .hp = 8.0f,
        .max_hp = 8.0f,
        .ore = 4.0f,
        .max_ore = 4.0f,
        .commodity = COMMODITY_FERRITE_ORE,
        .spin = 0.5f,
        .age = 1.0f,
    };
}

TEST(test_asteroid_presentation_uses_authority_pose_without_mutation)
{
    WORLD_DECL;
    asteroid_t client = presentation_asteroid(
        v2(10.0f, 20.0f), v2(1.0f, 0.0f));
    w.asteroids[3] = presentation_asteroid(
        v2(100.0f, 200.0f), v2(30.0f, -10.0f));
    asteroid_t authority_before = w.asteroids[3];
    asteroid_t presented = {0};
    asteroid_motion_class_t motion_class = ASTEROID_MOTION_CLASS_COUNT;

    asteroid_presentation_action_t action =
        asteroid_presentation_resolve(
            &w, &client, 3, SIM_DT * 0.5f,
            &presented, &motion_class);

    ASSERT_EQ_INT(action, ASTEROID_PRESENTATION_PRESENT);
    ASSERT_EQ_INT(motion_class, ASTEROID_MOTION_LOOSE);
    ASSERT(v2_dist_sq(presented.pos, client.pos) > 1.0f);
    ASSERT_EQ_FLOAT(presented.age,
                    authority_before.age + SIM_DT * 0.5f, 0.0001f);
    ASSERT(memcmp(&w.asteroids[3], &authority_before,
                  sizeof(authority_before)) == 0);
    ASSERT_EQ_FLOAT(client.pos.x, 10.0f, 0.0001f);
}

TEST(test_asteroid_presentation_tracks_tow_classes_and_station_force)
{
    WORLD_DECL;
    asteroid_t client = presentation_asteroid(
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    w.asteroids[4] = presentation_asteroid(
        v2(5.0f, 6.0f), v2(12.0f, -3.0f));
    asteroid_set_player_tractor(&w.asteroids[4], 0);

    asteroid_t presented = {0};
    asteroid_motion_class_t motion_class = ASTEROID_MOTION_LOOSE;
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 4, SIM_DT,
            &presented, &motion_class),
        ASTEROID_PRESENTATION_PRESENT);
    ASSERT_EQ_INT(motion_class, ASTEROID_MOTION_PLAYER_TOW);
    ASSERT_EQ_FLOAT(presented.pos.x,
                    w.asteroids[4].pos.x +
                        w.asteroids[4].vel.x * SIM_DT,
                    0.0001f);

    asteroid_clear_tractor(&w.asteroids[4]);
    w.interactions.count = 1;
    w.interactions.items[0] = (sim_interaction_t){
        .type = SIM_INTERACTION_TRACTOR_BEAM,
        .visual = SIM_INTERACTION_VISUAL_STATION_FRAGMENT_TRACTOR,
        .target = {
            .type = SIM_INTERACTION_ENTITY_ASTEROID,
            .index = 4,
        },
    };
    ASSERT_EQ_INT(
        asteroid_motion_classify(&w, 4),
        ASTEROID_MOTION_STATION_TOW);
}

TEST(test_asteroid_presentation_waits_for_identity_and_retires_atomically)
{
    WORLD_DECL;
    asteroid_t client = presentation_asteroid(
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    w.asteroids[5] = client;
    w.asteroids[5].commodity = COMMODITY_CUPRITE_ORE;
    asteroid_t presented = {0};

    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 5, 0.0f,
            &presented, NULL),
        ASTEROID_PRESENTATION_SKIP);
    w.asteroids[5].active = false;
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 5, 0.0f,
            &presented, NULL),
        ASTEROID_PRESENTATION_RETIRE);
    client.active = false;
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 5, 0.0f,
            &presented, NULL),
        ASTEROID_PRESENTATION_SKIP);
}

TEST(test_asteroid_presentation_covers_npc_throw_release_and_reentry)
{
    WORLD_DECL;
    asteroid_t client = presentation_asteroid(
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    w.asteroids[6] = client;
    asteroid_t presented = {0};
    asteroid_motion_class_t motion_class = ASTEROID_MOTION_LOOSE;

    asteroid_set_npc_tractor(&w.asteroids[6], 2);
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 6, 0.0f, &presented, &motion_class),
        ASTEROID_PRESENTATION_PRESENT);
    ASSERT_EQ_INT(motion_class, ASTEROID_MOTION_NPC_TOW);

    asteroid_clear_tractor(&w.asteroids[6]);
    const uint8_t thrower[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    asteroid_mark_thrown(&w.asteroids[6], thrower, 1.0f);
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 6, 0.0f, &presented, &motion_class),
        ASTEROID_PRESENTATION_PRESENT);
    ASSERT_EQ_INT(motion_class, ASTEROID_MOTION_BALLISTIC);

    asteroid_clear_thrown(&w.asteroids[6]);
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 6, 0.0f, &presented, &motion_class),
        ASTEROID_PRESENTATION_PRESENT);
    ASSERT_EQ_INT(motion_class, ASTEROID_MOTION_LOOSE);

    client.commodity = COMMODITY_CUPRITE_ORE;
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 6, 0.0f, &presented, &motion_class),
        ASTEROID_PRESENTATION_SKIP);
    client.commodity = w.asteroids[6].commodity;
    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 6, 0.0f, &presented, &motion_class),
        ASTEROID_PRESENTATION_PRESENT);
}

TEST(test_asteroid_presentation_applies_collision_impulse_without_smoothing)
{
    WORLD_DECL;
    asteroid_t client = presentation_asteroid(
        v2(10.0f, 20.0f), v2(1.0f, 2.0f));
    w.asteroids[8] = client;
    w.asteroids[8].pos = v2(18.0f, 23.0f);
    w.asteroids[8].vel = v2(-140.0f, 65.0f);
    asteroid_t presented = {0};

    ASSERT_EQ_INT(
        asteroid_presentation_resolve(
            &w, &client, 8, 0.0f, &presented, NULL),
        ASTEROID_PRESENTATION_PRESENT);
    ASSERT_EQ_FLOAT(presented.pos.x, 18.0f, 0.0001f);
    ASSERT_EQ_FLOAT(presented.pos.y, 23.0f, 0.0001f);
    ASSERT_EQ_FLOAT(presented.vel.x, -140.0f, 0.0001f);
    ASSERT_EQ_FLOAT(presented.vel.y, 65.0f, 0.0001f);
}

TEST(test_asteroid_presentation_metrics_reject_10hz_sawtooth_not_real_motion)
{
    asteroid_presentation_diagnostics_t diagnostics;
    asteroid_presentation_diagnostics_reset(&diagnostics);
    asteroid_t legacy = presentation_asteroid(
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    asteroid_t target = legacy;
    asteroid_t final = legacy;
    const float dt = 1.0f / 60.0f;
    float packet_time = 0.0f;
    vec2 packet_pos = legacy.pos;
    vec2 packet_vel = legacy.vel;

    for (int frame = 0; frame < 90; frame++) {
        float t = (float)frame * dt;
        target.pos = v2(40.0f * t * t, 3.0f * t);
        target.vel = v2(80.0f * t, 3.0f);
        if (frame % 6 == 0) {
            packet_time = t;
            packet_pos = target.pos;
            packet_vel = target.vel;
        }
        legacy.pos = v2_add(
            packet_pos,
            v2_scale(packet_vel, t - packet_time));
        legacy.vel = packet_vel;
        final = target;

        asteroid_presentation_diagnostics_begin_frame(&diagnostics);
        asteroid_presentation_diagnostics_present(
            &diagnostics, 2, ASTEROID_MOTION_PLAYER_TOW,
            &legacy, &final, &target, dt, 1.0f);
    }

    ASSERT_EQ_INT(diagnostics.frame_samples, 90);
    ASSERT_EQ_INT(diagnostics.presented_samples, 90);
    ASSERT(diagnostics.max_legacy_correction_avoided > 0.1f);
    ASSERT(diagnostics.max_correction_world < 0.0001f);
    ASSERT(diagnostics.max_velocity_discontinuity < 0.0001f);
    ASSERT_EQ_INT(diagnostics.starvation_events, 0);
    ASSERT(asteroid_presentation_diagnostics_within_thresholds(
        &diagnostics));
}

TEST(test_asteroid_presentation_metrics_detect_starvation_and_reset_on_retire)
{
    asteroid_presentation_diagnostics_t diagnostics;
    asteroid_presentation_diagnostics_reset(&diagnostics);
    asteroid_t asteroid = presentation_asteroid(
        v2(0.0f, 0.0f), v2(1.0f, 0.0f));
    const float dt = 1.0f / 60.0f;

    asteroid_presentation_diagnostics_begin_frame(&diagnostics);
    asteroid_presentation_diagnostics_present(
        &diagnostics, 1, ASTEROID_MOTION_LOOSE,
        &asteroid, &asteroid, &asteroid, dt, 1.0f);
    for (int frame = 0; frame < 12; frame++) {
        asteroid.pos.x += dt;
        asteroid_presentation_diagnostics_begin_frame(&diagnostics);
        asteroid_presentation_diagnostics_skip(
            &diagnostics, 1, ASTEROID_MOTION_LOOSE,
            &asteroid, dt, 1.0f);
    }
    ASSERT(diagnostics.starvation_events > 0);
    ASSERT(!asteroid_presentation_diagnostics_within_thresholds(
        &diagnostics));

    asteroid_presentation_diagnostics_retire(&diagnostics, 1);
    ASSERT_EQ_INT(diagnostics.retired_samples, 1);
    ASSERT(!diagnostics.slots[1].motion_class_valid);
}

TEST(test_asteroid_presentation_metrics_reject_repeating_jerk_rhythm)
{
    asteroid_presentation_diagnostics_t diagnostics;
    asteroid_presentation_diagnostics_reset(&diagnostics);
    asteroid_t asteroid = presentation_asteroid(
        v2(0.0f, 0.0f), v2(0.0f, 0.0f));
    const float dt = 1.0f / 60.0f;

    for (int frame = 0; frame < 12; frame++) {
        asteroid_t final = asteroid;
        final.pos.x = (frame & 1) ? 200.0f : 0.0f;
        asteroid_presentation_diagnostics_begin_frame(&diagnostics);
        asteroid_presentation_diagnostics_present(
            &diagnostics, 7, ASTEROID_MOTION_PLAYER_TOW,
            &asteroid, &final, &asteroid, dt, 1.0f);
    }

    ASSERT(diagnostics.max_screen_jerk >
           ASTEROID_PRESENTATION_MAX_SCREEN_JERK);
    ASSERT(!asteroid_presentation_diagnostics_within_thresholds(
        &diagnostics));

    asteroid_presentation_diagnostics_retire(&diagnostics, 7);
    ASSERT(!diagnostics.slots[7].kinematics.have_position);
}

void register_asteroid_presentation_tests(void)
{
    TEST_SECTION("\nLocal asteroid presentation (#685):\n");
    RUN(test_asteroid_presentation_uses_authority_pose_without_mutation);
    RUN(test_asteroid_presentation_tracks_tow_classes_and_station_force);
    RUN(test_asteroid_presentation_waits_for_identity_and_retires_atomically);
    RUN(test_asteroid_presentation_covers_npc_throw_release_and_reentry);
    RUN(test_asteroid_presentation_applies_collision_impulse_without_smoothing);
    RUN(test_asteroid_presentation_metrics_reject_10hz_sawtooth_not_real_motion);
    RUN(test_asteroid_presentation_metrics_detect_starvation_and_reset_on_retire);
    RUN(test_asteroid_presentation_metrics_reject_repeating_jerk_rhythm);
}
