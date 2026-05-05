#include "tests/test_harness.h"
#include "trade_paging.h"

TEST(test_trade_paging_keeps_sell_on_fresh_page) {
    const uint8_t kinds[] = {
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
    };
    int first = -1, last = -1, total = -1;

    trade_page_range_for_kinds(kinds, 5, TRADE_ROWS_PER_PAGE,
                               0, &first, &last, &total);
    ASSERT_EQ_INT(first, 0);
    ASSERT_EQ_INT(last, 3);
    ASSERT_EQ_INT(total, 2);

    trade_page_range_for_kinds(kinds, 5, TRADE_ROWS_PER_PAGE,
                               1, &first, &last, &total);
    ASSERT_EQ_INT(first, 3);
    ASSERT_EQ_INT(last, 5);
    ASSERT_EQ_INT(total, 2);
}

TEST(test_trade_paging_chunks_each_side_independently) {
    const uint8_t kinds[] = {
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_BUY,
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
    };
    int first = -1, last = -1, total = -1;

    trade_page_range_for_kinds(kinds, 12, TRADE_ROWS_PER_PAGE,
                               0, &first, &last, &total);
    ASSERT_EQ_INT(first, 0);
    ASSERT_EQ_INT(last, 5);
    ASSERT_EQ_INT(total, 4);

    trade_page_range_for_kinds(kinds, 12, TRADE_ROWS_PER_PAGE,
                               1, &first, &last, &total);
    ASSERT_EQ_INT(first, 5);
    ASSERT_EQ_INT(last, 6);
    ASSERT_EQ_INT(total, 4);

    trade_page_range_for_kinds(kinds, 12, TRADE_ROWS_PER_PAGE,
                               2, &first, &last, &total);
    ASSERT_EQ_INT(first, 6);
    ASSERT_EQ_INT(last, 11);
    ASSERT_EQ_INT(total, 4);

    trade_page_range_for_kinds(kinds, 12, TRADE_ROWS_PER_PAGE,
                               3, &first, &last, &total);
    ASSERT_EQ_INT(first, 11);
    ASSERT_EQ_INT(last, 12);
    ASSERT_EQ_INT(total, 4);
}

TEST(test_trade_paging_wraps_invalid_page_to_first_page) {
    const uint8_t kinds[] = {
        TRADE_ROW_KIND_SELL,
        TRADE_ROW_KIND_SELL,
    };
    int first = -1, last = -1, total = -1;

    trade_page_range_for_kinds(kinds, 2, TRADE_ROWS_PER_PAGE,
                               99, &first, &last, &total);
    ASSERT_EQ_INT(first, 0);
    ASSERT_EQ_INT(last, 2);
    ASSERT_EQ_INT(total, 1);

    trade_page_range_for_kinds(kinds, 2, TRADE_ROWS_PER_PAGE,
                               -1, &first, &last, &total);
    ASSERT_EQ_INT(first, 0);
    ASSERT_EQ_INT(last, 2);
    ASSERT_EQ_INT(total, 1);
}

void register_trade_paging_tests(void);
void register_trade_paging_tests(void) {
    TEST_SECTION("\nTrade paging:\n");
    RUN(test_trade_paging_keeps_sell_on_fresh_page);
    RUN(test_trade_paging_chunks_each_side_independently);
    RUN(test_trade_paging_wraps_invalid_page_to_first_page);
}
