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

void register_asteroid_tests(void) {
    TEST_SECTION("Asteroid helper tests:\n");
    RUN(test_asteroid_tier_name_all);
    RUN(test_asteroid_tier_kind_all);
    RUN(test_asteroid_next_tier_walks_to_S_then_clamps);
    RUN(test_asteroid_is_collectible);
    RUN(test_asteroid_progress_ratio_tier_s_uses_ore);
    RUN(test_asteroid_progress_ratio_falls_back_to_hp);
    RUN(test_asteroid_progress_ratio_zero_when_uninitialized);
    RUN(test_asteroid_geom_ladder_strictly_decreases);
    RUN(test_clear_asteroid_marks_net_dirty_when_was_active);
}
