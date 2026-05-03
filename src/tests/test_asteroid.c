#include "tests/test_harness.h"
#include "asteroid.h"

TEST(test_asteroid_tier_name_all) {
    ASSERT_STR_EQ(asteroid_tier_name(ASTEROID_TIER_XXL), "Titan");
    ASSERT_STR_EQ(asteroid_tier_name(ASTEROID_TIER_XL),  "XL");
    ASSERT_STR_EQ(asteroid_tier_name(ASTEROID_TIER_L),   "L");
    ASSERT_STR_EQ(asteroid_tier_name(ASTEROID_TIER_M),   "M");
    ASSERT_STR_EQ(asteroid_tier_name(ASTEROID_TIER_S),   "S");
    ASSERT_STR_EQ(asteroid_tier_name(ASTEROID_TIER_COUNT), "?");
}

TEST(test_asteroid_tier_kind_all) {
    ASSERT_STR_EQ(asteroid_tier_kind(ASTEROID_TIER_XXL), "titan");
    ASSERT_STR_EQ(asteroid_tier_kind(ASTEROID_TIER_XL),  "body");
    ASSERT_STR_EQ(asteroid_tier_kind(ASTEROID_TIER_L),   "rock");
    ASSERT_STR_EQ(asteroid_tier_kind(ASTEROID_TIER_M),   "shard");
    ASSERT_STR_EQ(asteroid_tier_kind(ASTEROID_TIER_S),   "fragment");
    ASSERT_STR_EQ(asteroid_tier_kind(ASTEROID_TIER_COUNT), "debris");
}

TEST(test_asteroid_next_tier_walks_to_S_then_clamps) {
    ASSERT_EQ_INT(asteroid_next_tier(ASTEROID_TIER_XXL), ASTEROID_TIER_XL);
    ASSERT_EQ_INT(asteroid_next_tier(ASTEROID_TIER_XL),  ASTEROID_TIER_L);
    ASSERT_EQ_INT(asteroid_next_tier(ASTEROID_TIER_L),   ASTEROID_TIER_M);
    ASSERT_EQ_INT(asteroid_next_tier(ASTEROID_TIER_M),   ASTEROID_TIER_S);
    /* Smallest tier clamps — fragments don't fracture further. */
    ASSERT_EQ_INT(asteroid_next_tier(ASTEROID_TIER_S),   ASTEROID_TIER_S);
}

TEST(test_asteroid_is_collectible) {
    asteroid_t a = {0};
    a.active = true; a.tier = ASTEROID_TIER_S;
    ASSERT(asteroid_is_collectible(&a));
    a.tier = ASTEROID_TIER_M;
    ASSERT(!asteroid_is_collectible(&a));
    a.tier = ASTEROID_TIER_S; a.active = false;
    ASSERT(!asteroid_is_collectible(&a));
}

TEST(test_asteroid_progress_ratio_tier_s_uses_ore) {
    asteroid_t a = {0};
    a.active = true; a.tier = ASTEROID_TIER_S;
    a.max_ore = 10.0f; a.ore = 4.0f;
    ASSERT_EQ_FLOAT(asteroid_progress_ratio(&a), 0.4f, 0.001f);
    a.ore = 0.0f;
    ASSERT_EQ_FLOAT(asteroid_progress_ratio(&a), 0.0f, 0.001f);
    /* Clamp: ore briefly above max shouldn't return >1. */
    a.ore = 99.0f;
    ASSERT_EQ_FLOAT(asteroid_progress_ratio(&a), 1.0f, 0.001f);
}

TEST(test_asteroid_progress_ratio_falls_back_to_hp) {
    asteroid_t a = {0};
    a.active = true; a.tier = ASTEROID_TIER_L;
    a.max_hp = 100.0f; a.hp = 25.0f;
    ASSERT_EQ_FLOAT(asteroid_progress_ratio(&a), 0.25f, 0.001f);
}

TEST(test_asteroid_progress_ratio_zero_when_uninitialized) {
    asteroid_t a = {0};
    /* No max_hp, no max_ore — hits the trailing return 0 branch. */
    ASSERT_EQ_FLOAT(asteroid_progress_ratio(&a), 0.0f, 0.001f);
}

TEST(test_asteroid_geom_ladder_strictly_decreases) {
    /* Bigger tiers must have a strictly larger radius/HP envelope than
     * smaller ones — guards the per-tier tables from accidental reorder. */
    asteroid_tier_t order[] = {ASTEROID_TIER_XXL, ASTEROID_TIER_XL,
                               ASTEROID_TIER_L,  ASTEROID_TIER_M,
                               ASTEROID_TIER_S};
    for (int i = 1; i < (int)(sizeof order / sizeof order[0]); i++) {
        ASSERT(asteroid_radius_min(order[i-1]) > asteroid_radius_min(order[i]));
        ASSERT(asteroid_radius_max(order[i-1]) > asteroid_radius_max(order[i]));
        ASSERT(asteroid_hp_min(order[i-1])     > asteroid_hp_min(order[i]));
        ASSERT(asteroid_hp_max(order[i-1])     > asteroid_hp_max(order[i]));
    }
    /* Default branch (TIER_COUNT) returns sane fallbacks rather than zero. */
    ASSERT(asteroid_radius_min(ASTEROID_TIER_COUNT) > 0.0f);
    ASSERT(asteroid_radius_max(ASTEROID_TIER_COUNT) > 0.0f);
    ASSERT(asteroid_hp_min(ASTEROID_TIER_COUNT)     > 0.0f);
    ASSERT(asteroid_hp_max(ASTEROID_TIER_COUNT)     > 0.0f);
    ASSERT(asteroid_spin_limit(ASTEROID_TIER_COUNT) > 0.0f);
}

TEST(test_clear_asteroid_marks_net_dirty_when_was_active) {
    asteroid_t a = {0};
    a.active = true; a.tier = ASTEROID_TIER_L; a.hp = 50.0f;
    clear_asteroid(&a);
    ASSERT(!a.active);
    ASSERT(a.net_dirty);
    ASSERT_EQ_INT((int)a.last_towed_by, -1);
    ASSERT_EQ_INT((int)a.last_fractured_by, -1);
    /* Clearing an already-inactive asteroid should NOT raise the
     * net_dirty flag — there's nothing to deactivate. */
    asteroid_t b = {0};
    clear_asteroid(&b);
    ASSERT(!b.net_dirty);
}

/* Distance-graded mining: rocks near the spawn cluster keep the
 * baseline burst cap (50), rocks at the frontier fracture with up to
 * 200 attempts, raising the probability tail on rare prefix classes.
 * This is the lever that makes pushing outward pay off — players who
 * plant outposts deeper find better-graded ore, not just more of the
 * same. */
TEST(test_burst_cap_baseline_at_origin) {
    vec2 origin = v2(0.0f, 0.0f);
    ASSERT_EQ_INT((int)mining_burst_cap_for_position(origin),
                  (int)FRACTURE_CHALLENGE_BURST_CAP);
    /* Anywhere within the seed cluster radius (5 kU) stays at baseline. */
    ASSERT_EQ_INT((int)mining_burst_cap_for_position(v2(3000.0f, 4000.0f)),
                  (int)FRACTURE_CHALLENGE_BURST_CAP);
}

TEST(test_burst_cap_clamps_at_far_frontier) {
    /* Past 30 kU the cap is at maximum — 4× the baseline. */
    uint16_t cap = mining_burst_cap_for_position(v2(50000.0f, 0.0f));
    ASSERT_EQ_INT((int)cap, (int)(FRACTURE_CHALLENGE_BURST_CAP * 4));
}

TEST(test_burst_cap_scales_monotonically_with_distance) {
    /* Strict monotonicity in the linear range so every kU outward is
     * a real (not just notional) bump. */
    uint16_t near    = mining_burst_cap_for_position(v2( 6000.0f, 0.0f));
    uint16_t mid     = mining_burst_cap_for_position(v2(15000.0f, 0.0f));
    uint16_t farish  = mining_burst_cap_for_position(v2(25000.0f, 0.0f));
    ASSERT(near < mid);
    ASSERT(mid  < farish);
    ASSERT(near >= FRACTURE_CHALLENGE_BURST_CAP);
    ASSERT(farish <= FRACTURE_CHALLENGE_BURST_CAP * 4);
}

TEST(test_burst_cap_isotropic) {
    /* Distance-only (not direction): the gradient must be radially
     * symmetric, otherwise frontier outposts on one axis would pay
     * differently than on another. */
    float r = 18000.0f;
    uint16_t east  = mining_burst_cap_for_position(v2( r,  0.0f));
    uint16_t north = mining_burst_cap_for_position(v2( 0.0f,  r));
    uint16_t sw    = mining_burst_cap_for_position(v2(-r * 0.7071f, -r * 0.7071f));
    ASSERT_EQ_INT((int)east, (int)north);
    /* sqrt(2)/2 ≈ 0.7071 keeps |sw| == r exactly so the cap matches too. */
    ASSERT(abs((int)east - (int)sw) <= 1);
}

void register_asteroid_tests(void) {
    TEST_SECTION("Asteroid helper tests:\n");
    RUN(test_asteroid_tier_name_all);
    RUN(test_asteroid_tier_kind_all);
    RUN(test_asteroid_next_tier_walks_to_S_then_clamps);
    RUN(test_asteroid_is_collectible);
    RUN(test_asteroid_progress_ratio_tier_s_uses_ore);
    RUN(test_asteroid_progress_ratio_falls_back_to_hp);
    RUN(test_asteroid_progress_ratio_zero_when_uninitialized);
    RUN(test_burst_cap_baseline_at_origin);
    RUN(test_burst_cap_clamps_at_far_frontier);
    RUN(test_burst_cap_scales_monotonically_with_distance);
    RUN(test_burst_cap_isotropic);
    RUN(test_asteroid_geom_ladder_strictly_decreases);
    RUN(test_clear_asteroid_marks_net_dirty_when_was_active);
}
