/*
 * trade_paging.h -- Pure pagination helper for dock trade rows.
 *
 * The client renders BUY and SELL rows from one unified list, but SELL must
 * start on a fresh page so numeric hotkeys never cross the transaction side
 * boundary. This helper keeps that rule testable outside the Sokol client.
 */
#ifndef TRADE_PAGING_H
#define TRADE_PAGING_H

#include <stdint.h>

enum {
    TRADE_ROW_KIND_BUY  = 0,
    TRADE_ROW_KIND_SELL = 1,
};

#define TRADE_ROWS_PER_PAGE 5
#define TRADE_MAX_ROWS      96

void trade_page_range_for_kinds(const uint8_t kinds[], int row_count,
                                int rows_per_page, int page,
                                int *out_first, int *out_last,
                                int *out_total);

#endif
