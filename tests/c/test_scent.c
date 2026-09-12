/*
 * test_scent.c -- gates on the physical scent traces that replaced the
 * omniscient asteroid sweep.
 *
 * The behaviour these protect is the whole reason scent exists: a miner
 * must not be able to know about ore it cannot sense, and a laden ship
 * must leave something behind that another ship can follow.
 */
#include "test_harness.h"

#include "signal_field.h"
#include "sim_scent.h"

#include <math.h>

/* ---- per-kind decay ---------------------------------------------------- */

TEST(test_scent_wake_goes_stale_faster_than_ore) {
    signal_field_t field;
    signal_field_init(&field);
    vec2 here = v2(0.0f, 0.0f);

    ASSERT(signal_field_observe(&field, here,
                                SIGNAL_FIELD_KIND_ORE_SCENT, 1.0f, 100));
    ASSERT(signal_field_observe(&field, here,
                                SIGNAL_FIELD_KIND_CARGO_WAKE, 1.0f, 100));

    signal_field_decay(&field, 1100, 1000);

    float ore  = signal_field_query(&field, here,
                                    SIGNAL_FIELD_KIND_ORE_SCENT, 0);
    float wake = signal_field_query(&field, here,
                                    SIGNAL_FIELD_KIND_CARGO_WAKE, 0);

    /* Rock does not move, so its scent outlasts a fly's attention span.
     * A wake is only worth following while fresh -- that is what makes
     * running down a laden hauler a chase and not a lookup. */
    ASSERT(ore > wake);
    ASSERT(ore > 0.75f);    /* 4x half-life: barely faded */
    ASSERT(wake < 0.10f);   /* 0.25x half-life: nearly cold */
}

TEST(test_scent_kinds_do_not_bleed_into_the_gossip_kinds) {
    signal_field_t field;
    signal_field_init(&field);
    vec2 here = v2(0.0f, 0.0f);

    ASSERT(signal_field_observe(&field, here,
                                SIGNAL_FIELD_KIND_ORE_SCENT, 1.0f, 10));

    /* Scent is a physical trace; the older kinds are things somebody was
     * told. They share a grid and must not share a value. */
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_DEMAND, 0),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_CARGO_WAKE, 0),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(signal_field_kind_half_life_scale(
                        SIGNAL_FIELD_KIND_DEMAND), 1.0f, 0.001f);
}

/* ---- emission ---------------------------------------------------------- */

TEST(test_scent_rocks_emit_and_mined_out_rock_goes_quiet) {
    WORLD_DECL;
    world_reset(&w);

    /* Any live rock. Note this must NOT require ore > 0: only S-tier
     * fragments carry that field, and an unfractured rock -- the thing a
     * miner actually goes out to hit -- reads zero. */
    int rock = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active) { rock = i; break; }
    }
    ASSERT(rock >= 0);
    vec2 at = w.asteroids[rock].pos;

    for (int t = 0; t < 240; t++) { w.tick++; scent_step(&w); }
    float smell = scent_sample(&w, at, SIGNAL_FIELD_KIND_ORE_SCENT);
    ASSERT(smell > 0.0f);

    /* Clear the belt. Emission stops with the rock, which is what makes a
     * worked-out patch fade instead of luring miners to it forever. */
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    float before = scent_sample(&w, at, SIGNAL_FIELD_KIND_ORE_SCENT);
    for (int t = 0; t < 600; t++) {
        w.tick++;
        scent_step(&w);
        signal_field_decay(&w.signal_field, w.tick, 120);
    }
    float after = scent_sample(&w, at, SIGNAL_FIELD_KIND_ORE_SCENT);
    ASSERT(after < before);
}

TEST(test_scent_only_laden_ships_leave_a_wake) {
    WORLD_DECL;
    world_reset(&w);

    int npc = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].ship) { npc = i; break; }
    }
    ASSERT(npc >= 0);
    ship_t *ship = w.npc_ships[npc].ship;
    vec2 at = ship->pos;

    /* Empty hull: nearly silent. */
    ship->towed_count = 0;
    ship->towed_pod_count = 0;
    ship->towed_scaffold = -1;
    for (int t = 0; t < 120; t++) { w.tick++; scent_step(&w); }
    ASSERT_EQ_FLOAT(scent_sample(&w, at, SIGNAL_FIELD_KIND_CARGO_WAKE),
                    0.0f, 0.001f);

    /* Loaded: drags a line somebody can follow. */
    ship->towed_count = 3;
    for (int t = 0; t < 120; t++) { w.tick++; scent_step(&w); }
    ASSERT(scent_sample(&w, at, SIGNAL_FIELD_KIND_CARGO_WAKE) > 0.0f);
}

/* ---- sensing ----------------------------------------------------------- */

TEST(test_scent_gradient_points_up_the_plume) {
    WORLD_DECL;
    world_reset(&w);
    signal_field_init(&w.signal_field);

    /* Two cells apart, one much richer. The nose should point at it. */
    vec2 poor = v2(0.0f, 0.0f);
    vec2 rich = v2(SIGNAL_FIELD_CELL_SIZE * 2.0f, 0.0f);
    for (int i = 0; i < 40; i++) {
        (void)signal_field_observe(&w.signal_field, rich,
                                   SIGNAL_FIELD_KIND_ORE_SCENT, 0.2f, 1);
    }

    vec2 dir = v2(0.0f, 0.0f);
    float strength = -1.0f;
    ASSERT(scent_gradient(&w, poor, SIGNAL_FIELD_KIND_ORE_SCENT,
                          &dir, &strength));
    ASSERT(strength >= 0.0f);
    ASSERT(dir.x > 0.5f);              /* toward the rich cell */
    ASSERT(fabsf(dir.y) < 0.5f);
}

TEST(test_scent_flat_field_gives_no_heading) {
    WORLD_DECL;
    world_reset(&w);
    signal_field_init(&w.signal_field);

    /* Nothing to smell must be reported as nothing, not as an arbitrary
     * bearing off a cell boundary -- a fly with no gradient should cast,
     * and casting is what the caller does when this returns false. */
    vec2 dir = v2(9.0f, 9.0f);
    ASSERT(!scent_gradient(&w, v2(0.0f, 0.0f),
                           SIGNAL_FIELD_KIND_ORE_SCENT, &dir, NULL));
}

TEST(test_scent_sight_collapses_outside_signal) {
    WORLD_DECL;
    world_reset(&w);

    int npc = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].ship) { npc = i; break; }
    }
    ASSERT(npc >= 0);
    ship_t *ship = w.npc_ships[npc].ship;

    vec2 home = ship->pos;
    float near_station = scent_sight_radius(&w, ship);

    /* Far outside the relay chain the fly goes half-blind and has to work
     * by nose, which is what keeps expansion tied to signal coverage. */
    ship->pos = v2(SIGNAL_FIELD_WORLD_MIN_X + 1.0f,
                   SIGNAL_FIELD_WORLD_MIN_Y + 1.0f);
    float out_there = scent_sight_radius(&w, ship);
    ship->pos = home;

    ASSERT(near_station > out_there);
    ASSERT(out_there > 0.0f);          /* never fully blind */
}

void register_scent_tests(void);
void register_scent_tests(void) {
    RUN(test_scent_wake_goes_stale_faster_than_ore);
    RUN(test_scent_kinds_do_not_bleed_into_the_gossip_kinds);
    RUN(test_scent_rocks_emit_and_mined_out_rock_goes_quiet);
    RUN(test_scent_only_laden_ships_leave_a_wake);
    RUN(test_scent_gradient_points_up_the_plume);
    RUN(test_scent_flat_field_gives_no_heading);
    RUN(test_scent_sight_collapses_outside_signal);
}
