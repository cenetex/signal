/* Tests for MOTD rarity tier selection based on signal strength. */
#include "tests/test_harness.h"
#include "avatar.h"
#include "signal_model.h"

TEST(test_motd_tier_label) {
    /* Verify tier labels are correct. */
    ASSERT_STR_EQ(avatar_motd_tier_label(0), "COMMON");
    ASSERT_STR_EQ(avatar_motd_tier_label(1), "UNCOMMON");
    ASSERT_STR_EQ(avatar_motd_tier_label(2), "RARE");
    ASSERT_STR_EQ(avatar_motd_tier_label(3), "ULTRA_RARE");
    ASSERT_STR_EQ(avatar_motd_tier_label(-1), "UNKNOWN");
    ASSERT_STR_EQ(avatar_motd_tier_label(99), "UNKNOWN");
}

TEST(test_motd_tier_for_signal) {
    /* Create a test avatar cache entry with tier bands. */
    avatar_cache_t av = {0};
    memset(&av, 0, sizeof(av));

    av.tiers[0].band_min = 0.80f;
    av.tiers[0].band_max = 1.00f;
    strcpy(av.tiers[0].text, "COMMON");

    av.tiers[1].band_min = 0.50f;
    av.tiers[1].band_max = 0.80f;
    strcpy(av.tiers[1].text, "UNCOMMON");

    av.tiers[2].band_min = 0.20f;
    av.tiers[2].band_max = 0.50f;
    strcpy(av.tiers[2].text, "RARE");

    av.tiers[3].band_min = 0.00f;
    av.tiers[3].band_max = 0.20f;
    strcpy(av.tiers[3].text, "ULTRA_RARE");

    /* Test boundary conditions for each tier. */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 1.00f), 0);  /* CORE lower bound */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.90f), 0);  /* CORE middle */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.80f), 0);  /* CORE lower edge */

    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.79f), 1);  /* OPERATIONAL */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.65f), 1);  /* OPERATIONAL middle */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.50f), 1);  /* OPERATIONAL lower edge */

    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.49f), 2);  /* FRINGE */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.35f), 2);  /* FRINGE middle */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.20f), 2);  /* FRINGE lower edge */

    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.19f), 3);  /* FRONTIER */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.10f), 3);  /* FRONTIER middle */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, 0.00f), 3);  /* FRONTIER at zero */

    /* Test with NULL avatar (should return -1). */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(NULL, 0.50f), -1);

    /* Test with negative signal (should return -1). */
    ASSERT_EQ_INT(avatar_motd_tier_for_signal(&av, -0.1f), -1);
}

void register_motd_rarity_tests(void) {
    TEST_SECTION("\nMOTD rarity tier selection:\n");
    RUN(test_motd_tier_label);
    RUN(test_motd_tier_for_signal);
}
