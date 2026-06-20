/* Regression tests for readable route-history presentation labels. */
#include "test_harness.h"
#include "route_history_labels.h"

TEST(test_route_history_labels_name_route_reputation) {
    ASSERT_STR_EQ(route_history_memory_kind_label(MARKET_MEMORY_ROUTE_REPUTATION),
                  "route reputation");
    ASSERT_STR_EQ(route_history_action_label(CONTRACT_TRACTOR), "haul");
    ASSERT_STR_EQ(route_history_action_label(CONTRACT_DELIVERY), "delivery");
    ASSERT_STR_EQ(route_history_certainty_label(240, 230), "known");
    ASSERT_STR_EQ(route_history_certainty_label(190, 180), "fresh");
    ASSERT_STR_EQ(route_history_certainty_label(120, 120), "heard");
    ASSERT_STR_EQ(route_history_certainty_label(80, 70), "faint");
}

TEST(test_route_history_summary_names_route_and_evidence) {
    char out[160];
    route_history_summary_fields(
        (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        0,
        1,
        (uint8_t)COMMODITY_FERRITE_INGOT,
        (uint8_t)CONTRACT_TRACTOR,
        4,
        220,
        out,
        sizeof(out));
    ASSERT_STR_EQ(out,
                  "route reputation: Prospect -> Kepler FR via haul, 4 receipts, conf 220");
}

TEST(test_route_history_compact_fields_fit_station_board) {
    char left[72];
    char right[48];
    route_history_compact_fields(
        (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS,
        0,
        1,
        (uint8_t)COMMODITY_FERRITE_INGOT,
        (uint8_t)CONTRACT_DELIVERY,
        7,
        199,
        left,
        sizeof(left),
        right,
        sizeof(right));
    ASSERT_STR_EQ(left, "route success Prospect>Kepler");
    ASSERT_STR_EQ(right, "delivery FR x7 c199");
}

TEST(test_route_history_detail_fields_show_signed_event_context) {
    char title[96];
    char evidence[96];
    char meta[96];
    route_history_detail_fields(
        42,
        9,
        (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        0,
        2,
        (uint8_t)COMMODITY_FRAME,
        (uint8_t)CONTRACT_DELIVERY,
        4,
        221,
        180,
        77,
        12345,
        title, sizeof(title),
        evidence, sizeof(evidence),
        meta, sizeof(meta));
    ASSERT_STR_EQ(title, "route reputation Prospect>Helios FM");
    ASSERT_STR_EQ(evidence, "signed proof: delivery via FM, 4 receipts");
    ASSERT_STR_EQ(meta, "fresh event 42 epoch 9 tick 12345 value 77");
}

TEST(test_route_history_aggregate_fields_show_institution_memory) {
    char title[96];
    char evidence[112];
    char freshness[96];
    route_history_aggregate_fields(
        (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        1,
        3,
        (uint8_t)COMMODITY_CUPRITE_INGOT,
        (uint8_t)CONTRACT_TRACTOR,
        3,
        19,
        240,
        201,
        98765,
        title, sizeof(title),
        evidence, sizeof(evidence),
        freshness, sizeof(freshness));
    ASSERT_STR_EQ(title, "route reputation Kepler>Blackglass CO");
    ASSERT_STR_EQ(evidence, "institution memory: haul via CO, 3 signed rows, 19 receipts");
    ASSERT_STR_EQ(freshness, "known, latest tick 98765");
}

TEST(test_route_history_aggregate_groups_and_ranks_rows) {
    route_history_aggregate_row_t rows[4];
    memset(rows, 0, sizeof(rows));

    route_history_aggregate_add_fields(rows, 4,
        (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        0, 1, (uint8_t)COMMODITY_FERRITE_INGOT,
        (uint8_t)CONTRACT_DELIVERY,
        4, 180, 120, 1000);
    route_history_aggregate_add_fields(rows, 4,
        (uint8_t)MARKET_MEMORY_ROUTE_REPUTATION,
        0, 1, (uint8_t)COMMODITY_FERRITE_INGOT,
        (uint8_t)CONTRACT_DELIVERY,
        6, 210, 140, 1200);
    route_history_aggregate_add_fields(rows, 4,
        (uint8_t)MARKET_MEMORY_ROUTE_RISK,
        2, 3, (uint8_t)COMMODITY_CRYSTAL_INGOT,
        (uint8_t)CONTRACT_TRACTOR,
        1, 90, 80, 2000);

    int count = route_history_aggregate_sort(rows, 4);
    ASSERT_EQ_INT(count, 2);
    ASSERT(rows[0].used);
    ASSERT_EQ_INT(rows[0].memory_kind, MARKET_MEMORY_ROUTE_REPUTATION);
    ASSERT_EQ_INT(rows[0].origin_station, 0);
    ASSERT_EQ_INT(rows[0].destination_station, 1);
    ASSERT_EQ_INT(rows[0].event_count, 2);
    ASSERT_EQ_INT(rows[0].evidence_sum, 10);
    ASSERT_EQ_INT(rows[0].confidence_peak, 210);
    ASSERT_EQ_INT(rows[0].salience_peak, 140);
    ASSERT_EQ_INT(rows[0].latest_tick, 1200);
    ASSERT_EQ_INT(rows[1].memory_kind, MARKET_MEMORY_ROUTE_RISK);
}

void register_route_history_label_tests(void) {
    TEST_SECTION("\nRoute history labels:\n");
    RUN(test_route_history_labels_name_route_reputation);
    RUN(test_route_history_summary_names_route_and_evidence);
    RUN(test_route_history_compact_fields_fit_station_board);
    RUN(test_route_history_detail_fields_show_signed_event_context);
    RUN(test_route_history_aggregate_fields_show_institution_memory);
    RUN(test_route_history_aggregate_groups_and_ranks_rows);
}
