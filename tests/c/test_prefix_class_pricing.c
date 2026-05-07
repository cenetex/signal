/* test_prefix_class_pricing.c — exercises the prefix-class price
 * multiplier table (#prefix-pricing) and the unit-aware pricing
 * functions station_buy_price_unit / station_sell_price_unit.
 *
 * The premise: a cargo unit's `prefix_class` (RATi / M / H / T / S /
 * F / K / anonymous), previously cosmetic, now scales the dynamic
 * station price. Anonymous-prefix units price identically to the
 * commodity-only path so the bulk economy is unchanged. M-prefix
 * doubles, RATi multiplies by 50, COMMISSIONED multiplies by 100.
 *
 * These tests pin the multiplier table values, the multiplicative
 * compose with the existing stock-fill curve, and the manifest
 * mixed-grade aggregate path. */
#include "test_harness.h"

/* Build a synthetic cargo_unit_t with a chosen commodity and prefix
 * class. Tests don't need a real fragment_pub — the pricing functions
 * read only commodity + prefix_class. */
static cargo_unit_t make_unit(commodity_t commodity, ingot_prefix_t prefix) {
    cargo_unit_t u = {0};
    u.kind = (uint8_t)CARGO_KIND_INGOT;
    u.commodity = (uint8_t)commodity;
    u.grade = (uint8_t)MINING_GRADE_COMMON;
    u.prefix_class = (uint8_t)prefix;
    return u;
}

/* Test 1: anonymous-prefix unit prices identically to the commodity-
 * only path. The whole multiplier system has to be transparent to
 * existing callers that only ever produce anonymous-class cargo. */
TEST(test_prefix_pricing_anonymous_baseline) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    /* Empty stockpile: sell price = 2× base = 48; buy price = 1× = 24. */
    cargo_unit_t anon = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_ANONYMOUS);
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &anon),
                    station_buy_price(&st, COMMODITY_FERRITE_INGOT), 0.01f);
    ASSERT_EQ_FLOAT(station_sell_price_unit(&st, &anon),
                    station_sell_price(&st, COMMODITY_FERRITE_INGOT), 0.01f);
}

/* Test 2: M-class ferrite ingot prices at 2× the commodity-only base.
 * Pins the M-row of the multiplier table. */
TEST(test_prefix_pricing_m_class_2x) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    cargo_unit_t m = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_M);
    float base_buy = station_buy_price(&st, COMMODITY_FERRITE_INGOT);
    float base_sell = station_sell_price(&st, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &m),  2.0f * base_buy, 0.01f);
    ASSERT_EQ_FLOAT(station_sell_price_unit(&st, &m), 2.0f * base_sell, 0.01f);
}

/* Test 3: RATi-class prices at 50× base. Pins the rare-tier value. */
TEST(test_prefix_pricing_rati_class_50x) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    cargo_unit_t r = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_RATI);
    float base_buy = station_buy_price(&st, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &r), 50.0f * base_buy, 0.01f);
}

/* Test 4: stock-fill multiplier and prefix multiplier compose
 * multiplicatively — neither overrides the other. At 50% hopper fill
 * the buy curve is 1 - 0.5*0.5 = 0.75×; under RATi prefix the
 * combined factor is 0.75 × 50 = 37.5. */
TEST(test_prefix_pricing_compose_with_stock_curve) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    /* Half-full PRODUCT stockpile (capacity = MAX_PRODUCT_STOCK). */
    st._inventory_cache[COMMODITY_FERRITE_INGOT] = MAX_PRODUCT_STOCK * 0.5f;
    float base_buy = station_buy_price(&st, COMMODITY_FERRITE_INGOT);
    /* Sanity: stock-fill curve gives 0.75× at 50% fill. */
    ASSERT_EQ_FLOAT(base_buy, 24.0f * 0.75f, 0.01f);
    cargo_unit_t r = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_RATI);
    /* Composed: 24 * 0.75 * 50.0 = 900. */
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &r), 24.0f * 0.75f * 50.0f, 0.01f);
}

/* Test 5: K-class is 2.5× (rarer letter), commissioned is 100×. Pins
 * the asymmetric row values so a future "make all classes 2x" change
 * fails this test instead of silently flattening. */
TEST(test_prefix_pricing_k_class_and_commissioned) {
    station_t st = {0};
    st.base_price[COMMODITY_CRYSTAL_INGOT] = 40.0f;
    cargo_unit_t k = make_unit(COMMODITY_CRYSTAL_INGOT, INGOT_PREFIX_K);
    cargo_unit_t cm = make_unit(COMMODITY_CRYSTAL_INGOT, INGOT_PREFIX_COMMISSIONED);
    float base = station_buy_price(&st, COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &k),    2.5f  * base, 0.01f);
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &cm),  100.0f * base, 0.01f);
}

/* Test 6: NULL guards — defensive checks at the API boundary. Both
 * NULL station and NULL unit return 0.0 without dereferencing. */
TEST(test_prefix_pricing_null_guards) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    cargo_unit_t u = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_M);
    ASSERT_EQ_FLOAT(station_buy_price_unit(NULL, &u), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, NULL), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_sell_price_unit(NULL, &u), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(station_sell_price_unit(&st, NULL), 0.0f, 0.001f);
}

/* Test 7: out-of-range prefix_class falls back to 1.0× (anonymous).
 * Defensive guard against malformed cargo_unit_t reaching pricing —
 * we never want a negative array index to escape the multiplier
 * lookup. */
TEST(test_prefix_pricing_out_of_range_falls_back_to_baseline) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    cargo_unit_t bad = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_ANONYMOUS);
    bad.prefix_class = 0xFF;  /* invalid */
    /* Falls back to 1.0× — same as commodity-only baseline. */
    ASSERT_EQ_FLOAT(station_buy_price_unit(&st, &bad),
                    station_buy_price(&st, COMMODITY_FERRITE_INGOT), 0.01f);
}

/* Test 8: manifest-style mixed-grade lineage. Sum of unit-scaled
 * prices for one anonymous + one M-class + one RATi ferrite ingot
 * matches the analytic 1+2+50 = 53× base. The intent is that haulers
 * who bring rare lineage cargo see the scaling reflected in the
 * aggregate sale value, not just the individual rows. */
TEST(test_prefix_pricing_manifest_mixed_lineage_aggregates) {
    station_t st = {0};
    st.base_price[COMMODITY_FERRITE_INGOT] = 24.0f;
    cargo_unit_t anon = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_ANONYMOUS);
    cargo_unit_t m    = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_M);
    cargo_unit_t r    = make_unit(COMMODITY_FERRITE_INGOT, INGOT_PREFIX_RATI);
    float total = station_buy_price_unit(&st, &anon)
                + station_buy_price_unit(&st, &m)
                + station_buy_price_unit(&st, &r);
    float base = station_buy_price(&st, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_FLOAT(total, (1.0f + 2.0f + 50.0f) * base, 0.01f);
}

void register_prefix_class_pricing_tests(void) {
    TEST_SECTION("Prefix-class price multiplier tests (#prefix-pricing):\n");
    RUN(test_prefix_pricing_anonymous_baseline);
    RUN(test_prefix_pricing_m_class_2x);
    RUN(test_prefix_pricing_rati_class_50x);
    RUN(test_prefix_pricing_compose_with_stock_curve);
    RUN(test_prefix_pricing_k_class_and_commissioned);
    RUN(test_prefix_pricing_null_guards);
    RUN(test_prefix_pricing_out_of_range_falls_back_to_baseline);
    RUN(test_prefix_pricing_manifest_mixed_lineage_aggregates);
}
