/*
 * test_tractor.c — unit tests for the unified tractor primitive.
 *
 * Pure math tests with no world dependency. They pin the behavior
 * (push/pull/range/falloff/damping/reaction/cap) so the call-site
 * migrations in R2-R5 can be evaluated for "behavior preserved" or
 * "behavior changed by 1D damping" with a stable reference point.
 */
#include "test_harness.h"
#include "tractor.h"
#include "fixpoint.h"

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

TEST(test_tractor_tow_profile_is_shared_across_anchor_types) {
    tractor_beam_t ship = tractor_tow_beam(
        0.0f, TRACTOR_TOW_BAND_REST_LENGTH);
    tractor_beam_t module = tractor_tow_beam(300.0f, 0.0f);

    ASSERT_EQ_FLOAT(ship.pull_strength, module.pull_strength, 0.001f);
    ASSERT_EQ_FLOAT(ship.push_strength, module.push_strength, 0.001f);
    ASSERT_EQ_FLOAT(ship.axial_damping, module.axial_damping, 0.001f);
    ASSERT_EQ_FLOAT(ship.tangent_damping, module.tangent_damping, 0.001f);
    ASSERT_EQ_FLOAT(module.range, 300.0f, 0.001f);
    ASSERT_EQ_FLOAT(module.rest_length, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(module.speed_cap, 0.0f, 0.001f);
    ASSERT_EQ_INT(module.falloff, TRACTOR_FALLOFF_CONSTANT);
    ASSERT_EQ_FLOAT(tractor_beam_tautness(
                        v2(0.0f, 0.0f), v2(150.0f, 0.0f), &module),
                    0.5f, 0.001f);
    ASSERT_EQ_FLOAT(tractor_beam_tautness(
                        v2(0.0f, 0.0f),
                        v2(TRACTOR_TOW_BAND_REST_LENGTH, 0.0f), &ship),
                    0.0f, 0.001f);

    vec2 target_vel = v2(0.0f, 0.0f);
    tractor_anchor_t source = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t target = mk_body_anchor(
        v2(100.0f, 0.0f), &target_vel, 1.0f);
    ASSERT(tractor_apply(&source, &target, &module, 0.1f));
    ASSERT_EQ_FLOAT(target_vel.x, -40.0f, 0.001f);
    ASSERT_EQ_FLOAT(target_vel.y, 0.0f, 0.001f);
}

TEST(test_tractor_tether_wave_remains_visible_at_full_tension) {
    ASSERT_EQ_FLOAT(tractor_tether_wave_scale(-1.0f), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_scale(0.0f), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_scale(0.5f), 0.775f, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_scale(1.0f),
                    TRACTOR_TETHER_TAUT_WAVE_FLOOR, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_scale(2.0f),
                    TRACTOR_TETHER_TAUT_WAVE_FLOOR, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_amplitude(100.0f, 0.0f),
                    10.0f, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_amplitude(100.0f, 1.0f),
                    5.5f, 0.001f);
    ASSERT_EQ_FLOAT(tractor_tether_wave_amplitude(-100.0f, 1.0f),
                    4.0f * TRACTOR_TETHER_TAUT_WAVE_FLOOR, 0.001f);
}

TEST(test_tractor_binding_has_one_source_at_a_time) {
    cargo_pod_t pod = {0};
    cargo_pod_clear_tractor(&pod);
    cargo_pod_set_module_tractor(&pod, 2, 5);
    ASSERT(cargo_pod_is_tractored_by_module(&pod, 2, 5));
    ASSERT_EQ_INT(cargo_pod_player_tractor(&pod), -1);

    cargo_pod_set_player_tractor(&pod, 3);
    ASSERT_EQ_INT(cargo_pod_player_tractor(&pod), 3);
    ASSERT(!cargo_pod_has_module_tractor(&pod));

    scaffold_t scaffold = {0};
    scaffold_clear_tractor(&scaffold);
    scaffold_set_npc_tractor(&scaffold, 4);
    ASSERT_EQ_INT(scaffold_tractor_npc(&scaffold), 4);
    scaffold_set_player_tractor(&scaffold, 1);
    ASSERT_EQ_INT(scaffold_tractor_player(&scaffold), 1);
    ASSERT_EQ_INT(scaffold_tractor_npc(&scaffold), -1);

    asteroid_t fragment = {0};
    asteroid_clear_tractor(&fragment);
    asteroid_set_npc_tractor(&fragment, 6);
    ASSERT_EQ_INT(asteroid_tractor_npc(&fragment), 6);
    ASSERT_EQ_INT(asteroid_tractor_player(&fragment), -1);
    asteroid_set_player_tractor(&fragment, 2);
    ASSERT_EQ_INT(asteroid_tractor_player(&fragment), 2);
    ASSERT_EQ_INT(asteroid_tractor_npc(&fragment), -1);
}

TEST(test_tow_link_pool_is_authority_for_ship_projections) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    int fragment = MAX_ASTEROIDS - 1;
    w.asteroids[fragment] = (asteroid_t){
        .active = true,
        .fracture_child = true,
        .radius = 8.0f,
    };
    asteroid_set_player_tractor(&w.asteroids[fragment], 0);
    sp->ship->towed_fragments[0] = (int16_t)fragment;
    sp->ship->towed_count = 1;

    world_tow_links_reconcile(&w);

    entity_ref_t target = world_entity_ref_for_slot(
        &w, ENTITY_KIND_ASTEROID, fragment, -1);
    const tow_link_t *link = world_tow_link_for_target_const(&w, target);
    ASSERT(link != NULL);
    ASSERT(entity_ref_equal(link->source, sp->ship_ref));
    ASSERT_EQ_INT(link->profile, TOW_PROFILE_SHIP_FRAGMENT);
    ASSERT_EQ_INT(link->state, TOW_LINK_HELD);
    ASSERT_EQ_INT(sp->ship->towed_count, 1);
    ASSERT_EQ_INT(sp->ship->towed_fragments[0], fragment);
    ASSERT_EQ_INT(asteroid_tractor_player(&w.asteroids[fragment]), 0);
    ASSERT_EQ_INT(w.asteroids[fragment].tractor.source_generation,
                  sp->ship_ref.generation);

    asteroid_clear_tractor(&w.asteroids[fragment]);
    world_tow_links_reconcile(&w);
    ASSERT(world_tow_link_for_target_const(&w, target) == NULL);
    ASSERT_EQ_INT(sp->ship->towed_count, 0);
}

TEST(test_tow_target_generation_changes_after_recycle) {
    WORLD_DECL;
    world_reset(&w);
    int pod = MAX_CARGO_PODS - 1;
    w.cargo_pods[pod].active = true;
    entity_ref_t first = world_entity_ref_for_slot(
        &w, ENTITY_KIND_CARGO_POD, pod, -1);
    ASSERT(!entity_ref_is_none(first));

    w.cargo_pods[pod].active = false;
    world_tow_links_reconcile(&w);
    ASSERT(!world_entity_ref_is_live(&w, first));
    w.cargo_pods[pod].active = true;
    entity_ref_t second = world_entity_ref_for_slot(
        &w, ENTITY_KIND_CARGO_POD, pod, -1);
    ASSERT(!entity_ref_equal(first, second));
    ASSERT(second.generation != first.generation);
}

TEST(test_tow_link_revision_is_monotonic_and_idempotent) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    int pod = MAX_CARGO_PODS - 2;
    w.cargo_pods[pod].active = true;
    w.cargo_pods[pod].kind = CARGO_POD_CARGO;
    w.tick = 1234;
    entity_ref_t target = world_entity_ref_for_slot(
        &w, ENTITY_KIND_CARGO_POD, pod, -1);
    ASSERT(world_tow_link_set(
        &w, sp->ship_ref, target, TOW_PROFILE_SHIP_POD, 0, TOW_LINK_HELD));
    const tow_link_t *link = world_tow_link_for_target_const(&w, target);
    ASSERT(link != NULL);
    uint32_t first_world_revision = w.tow_revision;
    uint32_t first_link_revision = link->revision;
    ASSERT(first_world_revision != 0);
    ASSERT(first_link_revision == first_world_revision);
    ASSERT(link->attached_tick == 1234u);
    ASSERT(w.tow_revision_tick == 1234u);

    w.tick = 1235;
    ASSERT(world_tow_link_set(
        &w, sp->ship_ref, target, TOW_PROFILE_SHIP_POD, 0, TOW_LINK_HELD));
    link = world_tow_link_for_target_const(&w, target);
    ASSERT(link != NULL);
    ASSERT(w.tow_revision == first_world_revision);
    ASSERT(link->revision == first_link_revision);
    ASSERT(link->attached_tick == 1234u);
    ASSERT(w.tow_revision_tick == 1234u);

    ASSERT(world_tow_link_set(
        &w, sp->ship_ref, target, TOW_PROFILE_SHIP_POD, 0,
        TOW_LINK_RELEASING));
    link = world_tow_link_for_target_const(&w, target);
    ASSERT(link != NULL);
    ASSERT(w.tow_revision > first_world_revision);
    ASSERT(link->revision == w.tow_revision);
    ASSERT(link->attached_tick == 1234u);
    ASSERT(w.tow_revision_tick == 1235u);

    uint32_t release_revision = w.tow_revision;
    w.tick = 1236;
    ASSERT(world_tow_link_clear_target(&w, target));
    ASSERT(world_tow_link_for_target_const(&w, target) == NULL);
    ASSERT(w.tow_revision > release_revision);
    ASSERT(w.tow_revision_tick == 1236u);

    release_revision = w.tow_revision;
    w.tick = 1237;
    ASSERT(!world_tow_link_clear_target(&w, target));
    ASSERT(w.tow_revision == release_revision);
    ASSERT(w.tow_revision_tick == 1236u);
}

TEST(test_cargo_tow_slot_teardown_preserves_owner_until_explicit_release) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->pubkey_set = true;
    sp->pubkey_proof_ok = true;
    sp->pubkey_challenge_consumed = true;
    sp->pubkey_identity_finalized = true;
    sp->id = 0;
    memset(sp->pubkey, 0x5d, sizeof(sp->pubkey));
    player_init_ship(sp, &w);

    int pod_idx = MAX_CARGO_PODS - 3;
    w.cargo_pods[pod_idx].active = true;
    w.cargo_pods[pod_idx].kind =
        CARGO_POD_CARGO;
    ASSERT(world_cargo_pod_set_player_tractor(
        &w, pod_idx, 0));
    actor_principal_t owner =
        w.cargo_pods[pod_idx]
            .tow_owner_principal;
    ASSERT_EQ_INT(
        owner.kind, ACTOR_PRINCIPAL_PLAYER);
    entity_ref_t target =
        world_entity_ref_for_slot(
            &w, ENTITY_KIND_CARGO_POD,
            pod_idx, -1);
    ASSERT(world_tow_link_for_target_const(
               &w, target) != NULL);

    /*
     * Disconnect/grace-expiry slot teardown retires only the transient ship
     * projection. A generic clear of an already-absent link must not become
     * an ownership mutation.
     */
    world_player_ship_slot_release(&w, 0);
    ASSERT(world_tow_link_for_target_const(
               &w, target) == NULL);
    ASSERT_EQ_INT(
        cargo_pod_player_tractor(
            &w.cargo_pods[pod_idx]), -1);
    ASSERT(actor_principal_equal(
        &w.cargo_pods[pod_idx]
             .tow_owner_principal,
        &owner));
    ASSERT(!world_tow_link_clear_target(
        &w, target));
    ASSERT(actor_principal_equal(
        &w.cargo_pods[pod_idx]
             .tow_owner_principal,
        &owner));

    world_cargo_pod_clear_tractor(
        &w, pod_idx);
    ASSERT_EQ_INT(
        w.cargo_pods[pod_idx]
            .tow_owner_principal.kind,
        ACTOR_PRINCIPAL_NONE);
}

TEST(test_tow_links_ignore_stale_projection_capacity_and_collect_stably) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    /* Poison the compatibility projection. Relationship admission must use
     * tow_links, then rebuild this view rather than treating it as authority. */
    sp->ship->towed_count = 10;
    sp->ship->towed_pod_count = 10;
    memset(sp->ship->towed_fragments, 0, sizeof(sp->ship->towed_fragments));
    memset(sp->ship->towed_pods, 0, sizeof(sp->ship->towed_pods));

    w.cargo_pods[7].active = true;
    w.cargo_pods[7].kind = CARGO_POD_CARGO;
    w.cargo_pods[3].active = true;
    w.cargo_pods[3].kind = CARGO_POD_CARGO;
    ASSERT(world_cargo_pod_set_player_tractor(&w, 7, 0));
    ASSERT(world_cargo_pod_set_player_tractor(&w, 3, 0));
    ASSERT_EQ_INT(world_tow_link_count_for_source(
                      &w, sp->ship_ref, TOW_PROFILE_SHIP_POD), 2);

    entity_ref_t one[1] = {entity_ref_none()};
    ASSERT_EQ_INT(world_tow_collect_targets(
                      &w, sp->ship_ref, TOW_PROFILE_SHIP_POD, one, 1), 2);
    ASSERT_EQ_INT(one[0].kind, ENTITY_KIND_CARGO_POD);
    ASSERT_EQ_INT(one[0].index, 3);
    ASSERT_EQ_INT(sp->ship->towed_pod_count, 2);
    ASSERT_EQ_INT(sp->ship->towed_pods[0], 3);
    ASSERT_EQ_INT(sp->ship->towed_pods[1], 7);
}

TEST(test_ship_pointer_cache_rebinds_and_clears_stale_views) {
    WORLD_DECL;
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    player_init_ship(sp, &w);
    ASSERT(world_ship_cached_views_valid(&w));

    ship_t stale = {0};
    sp->ship_ref = entity_ref_none();
    sp->ship = &stale;
    ASSERT(world_player_ship_for(&w, 0) == NULL);
    ASSERT(sp->ship == NULL);
    ASSERT(world_ship_cached_views_valid(&w));
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

TEST(test_tractor_inverse_mass_scales_both_endpoints) {
    /* A 4-mass source and 2-mass target must receive equal-and-opposite
     * momentum even though their velocity changes differ. */
    vec2 src_vel = v2(0.0f, 0.0f);
    vec2 tgt_vel = v2(0.0f, 0.0f);
    tractor_anchor_t src = mk_body_anchor(v2(0.0f, 0.0f), &src_vel, 0.25f);
    tractor_anchor_t tgt = mk_body_anchor(v2(10.0f, 0.0f), &tgt_vel, 0.5f);
    tractor_beam_t beam = {
        .rest_length = 5.0f,
        .pull_strength = 2.0f,
        .range = 100.0f,
        .falloff = TRACTOR_FALLOFF_CONSTANT,
    };

    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    ASSERT_EQ_FLOAT(src_vel.x, 2.5f, 0.001f);
    ASSERT_EQ_FLOAT(tgt_vel.x, -5.0f, 0.001f);
    ASSERT_EQ_FLOAT(4.0f * src_vel.x + 2.0f * tgt_vel.x, 0.0f, 0.001f);
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
    float spd = v2_len(tgt_vel);
    ASSERT_EQ_FLOAT(spd, 50.0f, 0.001f);
    ASSERT(tgt_vel.x < 0.0f);  /* still pulled toward source */
}

TEST(test_tractor_speed_cap_uses_deterministic_length) {
    vec2 tgt_vel = v2(30.0f, 40.0f);
    tractor_anchor_t src = mk_world_anchor(v2(0.0f, 0.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(3.0f, 4.0f), &tgt_vel, 1.0f);
    tractor_beam_t beam = {
        .rest_length = 5.0f, .pull_strength = 0.0f, .push_strength = 0.0f,
        .range = 100.0f, .axial_damping = 0.0f, .tangent_damping = 0.0f,
        .speed_cap = 10.0f, .falloff = TRACTOR_FALLOFF_CONSTANT,
    };

    ASSERT_EQ_FLOAT(v2_len(v2_sub(tgt.pos, src.pos)),
                    fixp_sqrtf(v2_len_sq(v2_sub(tgt.pos, src.pos))), 0.0001f);
    ASSERT(tractor_apply(&src, &tgt, &beam, 1.0f));
    ASSERT_EQ_FLOAT(tgt_vel.x, 6.0f, 0.001f);
    ASSERT_EQ_FLOAT(tgt_vel.y, 8.0f, 0.001f);
    ASSERT_EQ_FLOAT(v2_len(tgt_vel), fixp_sqrtf(v2_len_sq(tgt_vel)), 0.0001f);
}

TEST(test_tractor_diagonal_pull_deterministic_reference) {
    vec2 tgt_vel = v2(1.25f, -2.5f);
    tractor_anchor_t src = mk_world_anchor(v2(-2.0f, 3.0f));
    tractor_anchor_t tgt = mk_body_anchor(v2(7.0f, 15.0f), &tgt_vel, 1.0f);
    tractor_beam_t beam = {
        .rest_length = 5.0f,
        .pull_strength = 1.25f,
        .push_strength = 0.0f,
        .pull_constant = 0.5f,
        .range = 100.0f,
        .axial_damping = 0.2f,
        .tangent_damping = 0.1f,
        .speed_cap = 0.0f,
        .falloff = TRACTOR_FALLOFF_LINEAR,
    };

    ASSERT(tractor_apply(&src, &tgt, &beam, 0.125f));
    ASSERT_EQ_FLOAT(tgt_vel.x, 0.415f, 0.002f);
    ASSERT_EQ_FLOAT(tgt_vel.y, -3.561f, 0.002f);
}

void register_tractor_tests(void) {
    TEST_SECTION("\nTractor primitive (R1):\n");
    RUN(test_tractor_tow_profile_is_shared_across_anchor_types);
    RUN(test_tractor_tether_wave_remains_visible_at_full_tension);
    RUN(test_tractor_binding_has_one_source_at_a_time);
    RUN(test_tow_link_pool_is_authority_for_ship_projections);
    RUN(test_tow_target_generation_changes_after_recycle);
    RUN(test_tow_link_revision_is_monotonic_and_idempotent);
    RUN(test_cargo_tow_slot_teardown_preserves_owner_until_explicit_release);
    RUN(test_tow_links_ignore_stale_projection_capacity_and_collect_stably);
    RUN(test_ship_pointer_cache_rebinds_and_clears_stale_views);
    RUN(test_tractor_pull_engages_beyond_rest);
    RUN(test_tractor_push_engages_below_rest);
    RUN(test_tractor_constant_pull_independent_of_stretch);
    RUN(test_tractor_zero_strength_no_force);
    RUN(test_tractor_range_gate_disengages);
    RUN(test_tractor_linear_falloff_halves_at_half_range);
    RUN(test_tractor_axial_vs_tangent_damping_isolation);
    RUN(test_tractor_reaction_symmetry_conserves_momentum);
    RUN(test_tractor_inverse_mass_scales_both_endpoints);
    RUN(test_tractor_world_pinned_source_no_reaction);
    RUN(test_tractor_speed_cap_clamps_target);
    RUN(test_tractor_speed_cap_uses_deterministic_length);
    RUN(test_tractor_diagonal_pull_deterministic_reference);
}
