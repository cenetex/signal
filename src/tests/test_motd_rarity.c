/* Tests for MOTD rarity tier selection + JSON parser. */
#include "tests/test_harness.h"
#include "avatar.h"
#include "motd_json.h"
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

/* ------------------------------------------------------------------ */
/* JSON parser tests                                                    */
/* ------------------------------------------------------------------ */

TEST(test_motd_parse_roundtrip) {
    /* Happy path: full JSON with all four tiers, bands, and metadata. */
    const char json[] =
        "{\"messages\":{"
        "\"common\":\"hello pilot\","
        "\"uncommon\":\"weak signal here\","
        "\"rare\":\"old stories\","
        "\"ultra_rare\":\"end of the line\""
        "},\"bands\":{"
        "\"common\":[0.85,1.00],"
        "\"uncommon\":[0.55,0.85],"
        "\"rare\":[0.25,0.55],"
        "\"ultra_rare\":[0.00,0.25]"
        "},\"generated_at\":1735689600,\"seed\":42}";

    avatar_cache_t av = {0};
    ASSERT(motd_parse(&av, json, sizeof(json) - 1));
    ASSERT_STR_EQ(av.tiers[0].text, "hello pilot");
    ASSERT_STR_EQ(av.tiers[1].text, "weak signal here");
    ASSERT_STR_EQ(av.tiers[2].text, "old stories");
    ASSERT_STR_EQ(av.tiers[3].text, "end of the line");
    ASSERT_EQ_FLOAT(av.tiers[0].band_min, 0.85f, 0.001f);
    ASSERT_EQ_FLOAT(av.tiers[0].band_max, 1.00f, 0.001f);
    ASSERT_EQ_FLOAT(av.tiers[3].band_min, 0.00f, 0.001f);
    ASSERT_EQ_FLOAT(av.tiers[3].band_max, 0.25f, 0.001f);
    ASSERT_EQ_INT((int)av.generated_at, 1735689600);
    ASSERT_EQ_INT((int)av.seed, 42);
}

TEST(test_motd_parse_handles_escaped_quote) {
    /* The previous strstr-based parser silently truncated tier text at
     * the first quote, even when escaped. This is a regression guard:
     * the message contains \" and the parsed text must include the
     * literal quote character. */
    const char json[] =
        "{\"messages\":{"
        "\"common\":\"she said \\\"go\\\" and we went\","
        "\"uncommon\":\"u\","
        "\"rare\":\"r\","
        "\"ultra_rare\":\"x\""
        "}}";

    avatar_cache_t av = {0};
    ASSERT(motd_parse(&av, json, sizeof(json) - 1));
    ASSERT_STR_EQ(av.tiers[0].text, "she said \"go\" and we went");
}

TEST(test_motd_parse_handles_other_escapes) {
    /* Cover \\, \n, \t, and \/. All four must decode rather than appear
     * literally in the output. */
    const char json[] =
        "{\"messages\":{"
        "\"common\":\"path\\\\to\\nthing\","
        "\"uncommon\":\"tab\\there\","
        "\"rare\":\"slash\\/ok\","
        "\"ultra_rare\":\"x\""
        "}}";

    avatar_cache_t av = {0};
    ASSERT(motd_parse(&av, json, sizeof(json) - 1));
    ASSERT_STR_EQ(av.tiers[0].text, "path\\to\nthing");
    ASSERT_STR_EQ(av.tiers[1].text, "tab\there");
    ASSERT_STR_EQ(av.tiers[2].text, "slash/ok");
}

TEST(test_motd_parse_missing_messages_object_fails) {
    /* No "messages" key → parse fails entirely; the caller must NOT
     * mark motd_fetched (callers fall back to the legacy single
     * message path). */
    const char json[] = "{\"bands\":{\"common\":[0,1]}}";

    avatar_cache_t av = {0};
    ASSERT(!motd_parse(&av, json, sizeof(json) - 1));
}

TEST(test_motd_parse_missing_one_tier_fails) {
    /* If any of the four tiers is absent, the document is malformed
     * for our purposes. The parser refuses rather than silently
     * leaving one tier blank. */
    const char json[] =
        "{\"messages\":{"
        "\"common\":\"a\","
        "\"uncommon\":\"b\","
        "\"rare\":\"c\""
        /* ultra_rare missing */
        "}}";

    avatar_cache_t av = {0};
    ASSERT(!motd_parse(&av, json, sizeof(json) - 1));
}

TEST(test_motd_parse_no_bands_uses_defaults) {
    /* "bands" object is OPTIONAL. When absent, parser populates
     * tier bands from the documented MOTD_DEFAULT_BANDS table. */
    const char json[] =
        "{\"messages\":{"
        "\"common\":\"a\",\"uncommon\":\"b\","
        "\"rare\":\"c\",\"ultra_rare\":\"d\""
        "}}";

    avatar_cache_t av = {0};
    ASSERT(motd_parse(&av, json, sizeof(json) - 1));
    /* Defaults match the band layout shipped on the server side. */
    ASSERT_EQ_FLOAT(av.tiers[0].band_min, 0.80f, 0.001f);
    ASSERT_EQ_FLOAT(av.tiers[0].band_max, 1.00f, 0.001f);
    ASSERT_EQ_FLOAT(av.tiers[3].band_min, 0.00f, 0.001f);
    ASSERT_EQ_FLOAT(av.tiers[3].band_max, 0.20f, 0.001f);
}

TEST(test_motd_parse_truncated_buffer_fails) {
    /* Buffer cut off mid-string. Length-bounded parser must NOT walk
     * past the end and read garbage; it must report a parse error. */
    const char full[] =
        "{\"messages\":{"
        "\"common\":\"this is a long message that gets truncated"
        "\"uncommon\":\"unused\"}}";

    /* Pass only the first 30 bytes — the parser shouldn't see a
     * closing quote, brace, or any of the later tier keys. */
    avatar_cache_t av = {0};
    ASSERT(!motd_parse(&av, full, 30));
}

TEST(test_motd_parse_long_text_truncates_to_buffer) {
    /* Tier text > 255 chars must NOT overflow the 256-byte tiers[i].text
     * field. Today the parser refuses overlong inputs (returns false);
     * the regression guard is "no crash, no out-of-bounds write." */
    char json[1024];
    snprintf(json, sizeof(json),
        "{\"messages\":{"
        "\"common\":\"%.*s\","
        "\"uncommon\":\"u\",\"rare\":\"r\",\"ultra_rare\":\"x\""
        "}}",
        300, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
             "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    avatar_cache_t av = {0};
    /* Either rejects or accepts truncated; both are correct. The thing
     * we're asserting is NO buffer overflow — the test passes if the
     * call returns at all without crashing. */
    (void)motd_parse(&av, json, strlen(json));
    /* And the field must remain NUL-terminated within its bounds. */
    ASSERT(av.tiers[0].text[sizeof(av.tiers[0].text) - 1] == '\0');
}

TEST(test_motd_parse_unknown_keys_ignored) {
    /* Schema evolution: extra top-level keys must not break parsing. */
    const char json[] =
        "{\"version\":2,"
        "\"messages\":{"
        "\"common\":\"a\",\"uncommon\":\"b\","
        "\"rare\":\"c\",\"ultra_rare\":\"d\""
        "},"
        "\"future_field\":[1,2,3],"
        "\"seed\":99}";

    avatar_cache_t av = {0};
    ASSERT(motd_parse(&av, json, sizeof(json) - 1));
    ASSERT_STR_EQ(av.tiers[0].text, "a");
    ASSERT_EQ_INT((int)av.seed, 99);
}

void register_motd_rarity_tests(void) {
    TEST_SECTION("\nMOTD rarity tier selection:\n");
    RUN(test_motd_tier_label);
    RUN(test_motd_tier_for_signal);
    TEST_SECTION("\nMOTD JSON parser:\n");
    RUN(test_motd_parse_roundtrip);
    RUN(test_motd_parse_handles_escaped_quote);
    RUN(test_motd_parse_handles_other_escapes);
    RUN(test_motd_parse_missing_messages_object_fails);
    RUN(test_motd_parse_missing_one_tier_fails);
    RUN(test_motd_parse_no_bands_uses_defaults);
    RUN(test_motd_parse_truncated_buffer_fails);
    RUN(test_motd_parse_long_text_truncates_to_buffer);
    RUN(test_motd_parse_unknown_keys_ignored);
}
