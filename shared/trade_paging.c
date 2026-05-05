#include "trade_paging.h"

void trade_page_range_for_kinds(const uint8_t kinds[], int row_count,
                                int rows_per_page, int page,
                                int *out_first, int *out_last,
                                int *out_total)
{
    if (row_count < 0) row_count = 0;
    if (rows_per_page < 1) rows_per_page = 1;

    int sell_start = row_count;
    for (int i = 0; i < row_count; i++) {
        if (kinds && kinds[i] == TRADE_ROW_KIND_SELL) {
            sell_start = i;
            break;
        }
    }

    int buy_pages = (sell_start + rows_per_page - 1) / rows_per_page;
    int sell_count = row_count - sell_start;
    int sell_pages = (sell_count + rows_per_page - 1) / rows_per_page;
    int total = buy_pages + sell_pages;
    if (total < 1) total = 1;
    if (page < 0 || page >= total) page = 0;

    int first = 0;
    int last = 0;
    if (page < buy_pages) {
        first = page * rows_per_page;
        last = first + rows_per_page;
        if (last > sell_start) last = sell_start;
    } else {
        int sell_page = page - buy_pages;
        first = sell_start + sell_page * rows_per_page;
        last = first + rows_per_page;
        if (last > row_count) last = row_count;
    }

    if (out_first) *out_first = first;
    if (out_last) *out_last = last;
    if (out_total) *out_total = total;
}
