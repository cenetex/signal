#include "test_harness.h"
#include "signal_field.h"

static int field_test_index(int cell_x, int cell_y) {
    return cell_y * SIGNAL_FIELD_WIDTH + cell_x;
}

TEST(test_signal_field_maps_centered_world_to_cells) {
    int x = -1;
    int y = -1;
    ASSERT(signal_field_world_to_cell(v2(0.0f, 0.0f), &x, &y));
    ASSERT_EQ_INT(x, SIGNAL_FIELD_WIDTH / 2);
    ASSERT_EQ_INT(y, SIGNAL_FIELD_HEIGHT / 2);

    vec2 center = signal_field_cell_center(x, y);
    int cx = -1;
    int cy = -1;
    ASSERT(signal_field_world_to_cell(center, &cx, &cy));
    ASSERT_EQ_INT(cx, x);
    ASSERT_EQ_INT(cy, y);

    ASSERT(!signal_field_world_to_cell(
        v2(SIGNAL_FIELD_WORLD_MIN_X - 1.0f, 0.0f), NULL, NULL));
    ASSERT(!signal_field_world_to_cell(
        v2(0.0f,
           SIGNAL_FIELD_WORLD_MIN_Y +
               SIGNAL_FIELD_HEIGHT * SIGNAL_FIELD_CELL_SIZE),
        NULL,
        NULL));
}

TEST(test_signal_field_observe_is_local_and_bounded) {
    signal_field_t field;
    signal_field_init(&field);

    vec2 here = v2(0.0f, 0.0f);
    ASSERT(signal_field_observe(&field, here, SIGNAL_FIELD_KIND_DEMAND,
                                0.60f, 10));
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_DEMAND, 0),
                    0.60f, 0.001f);

    ASSERT(signal_field_observe(&field, here, SIGNAL_FIELD_KIND_DEMAND,
                                0.60f, 11));
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_DEMAND, 0),
                    0.84f, 0.001f);

    int x = -1;
    int y = -1;
    ASSERT(signal_field_world_to_cell(here, &x, &y));
    signal_field_cell_t *cell = &field.cells[field_test_index(x, y)];
    ASSERT_EQ_INT(cell->observations[SIGNAL_FIELD_KIND_DEMAND], 2);
    ASSERT_EQ_INT((int)cell->last_tick[SIGNAL_FIELD_KIND_DEMAND], 11);

    vec2 far = signal_field_cell_center(0, 0);
    ASSERT_EQ_FLOAT(signal_field_query(&field, far,
                                       SIGNAL_FIELD_KIND_DEMAND, 0),
                    0.0f, 0.001f);
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_SUPPLY, 0),
                    0.0f, 0.001f);
}

TEST(test_signal_field_neighbor_query_blends_without_global_spread) {
    signal_field_t field;
    signal_field_init(&field);

    int center_x = SIGNAL_FIELD_WIDTH / 2;
    int center_y = SIGNAL_FIELD_HEIGHT / 2;
    vec2 center = signal_field_cell_center(center_x, center_y);
    vec2 neighbor = signal_field_cell_center(center_x + 1, center_y);
    vec2 far = signal_field_cell_center(center_x + 6, center_y);

    ASSERT(signal_field_observe(&field, center, SIGNAL_FIELD_KIND_ROUTE,
                                0.80f, 20));
    ASSERT_EQ_FLOAT(signal_field_query(&field, neighbor,
                                       SIGNAL_FIELD_KIND_ROUTE, 0),
                    0.0f, 0.001f);

    float center_blend = signal_field_query(&field, center,
                                            SIGNAL_FIELD_KIND_ROUTE, 1);
    float neighbor_blend = signal_field_query(&field, neighbor,
                                              SIGNAL_FIELD_KIND_ROUTE, 1);
    ASSERT(center_blend > neighbor_blend);
    ASSERT(neighbor_blend > 0.0f);
    ASSERT_EQ_FLOAT(signal_field_query(&field, far,
                                       SIGNAL_FIELD_KIND_ROUTE, 1),
                    0.0f, 0.001f);
}

TEST(test_signal_field_decay_halves_at_half_life) {
    signal_field_t field;
    signal_field_init(&field);

    vec2 here = v2(0.0f, 0.0f);
    ASSERT(signal_field_observe(&field, here, SIGNAL_FIELD_KIND_SUPPLY,
                                1.0f, 100));

    signal_field_decay(&field, 1100, 1000);
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_SUPPLY, 0),
                    0.50f, 0.01f);

    signal_field_decay(&field, 1100, 1000);
    ASSERT_EQ_FLOAT(signal_field_query(&field, here,
                                       SIGNAL_FIELD_KIND_SUPPLY, 0),
                    0.50f, 0.01f);
}

TEST(test_signal_field_diagnostics_report_load_margin_and_noise) {
    signal_field_t field;
    signal_field_init(&field);

    vec2 here = v2(0.0f, 0.0f);
    signal_field_diagnostics_t empty =
        signal_field_diagnostics(&field, here, 0);
    ASSERT_EQ_INT(empty.occupied_slots, 0);
    ASSERT_EQ_INT(empty.capacity_slots,
                  SIGNAL_FIELD_CELL_COUNT * SIGNAL_FIELD_KIND_COUNT);
    ASSERT(!empty.noisy);

    ASSERT(signal_field_observe(&field, here, SIGNAL_FIELD_KIND_PROOF,
                                0.80f, 1));
    signal_field_diagnostics_t clear =
        signal_field_diagnostics(&field, here, 0);
    ASSERT_EQ_INT(clear.occupied_slots, 1);
    ASSERT(clear.top_margin > SIGNAL_FIELD_MARGIN_WARN);
    ASSERT(clear.recall_snr_estimate > SIGNAL_FIELD_SNR_WARN);
    ASSERT(!clear.noisy);

    ASSERT(signal_field_observe(&field, here, SIGNAL_FIELD_KIND_DEMAND,
                                0.80f, 2));
    signal_field_diagnostics_t conflicted =
        signal_field_diagnostics(&field, here, 0);
    ASSERT_EQ_INT(conflicted.occupied_slots, 2);
    ASSERT(conflicted.top_margin < SIGNAL_FIELD_MARGIN_WARN);
    ASSERT(conflicted.recall_snr_estimate < SIGNAL_FIELD_SNR_WARN);
    ASSERT(conflicted.noisy);
}

TEST(test_signal_field_kind_labels_are_stable) {
    ASSERT_STR_EQ(signal_field_kind_label(SIGNAL_FIELD_KIND_DEMAND), "demand");
    ASSERT_STR_EQ(signal_field_kind_label(SIGNAL_FIELD_KIND_SUPPLY), "supply");
    ASSERT_STR_EQ(signal_field_kind_label(SIGNAL_FIELD_KIND_ROUTE), "route");
    ASSERT_STR_EQ(signal_field_kind_label(SIGNAL_FIELD_KIND_PROOF), "proof");
    ASSERT_STR_EQ(signal_field_kind_label(SIGNAL_FIELD_KIND_HOLOGRAM),
                  "hologram");
    ASSERT_STR_EQ(signal_field_kind_label(SIGNAL_FIELD_KIND_RISK), "risk");
    ASSERT_STR_EQ(signal_field_kind_label((signal_field_kind_t)99), "?");
}

void register_signal_field_tests(void) {
    TEST_SECTION("\n[signal_field]\n");
    RUN(test_signal_field_maps_centered_world_to_cells);
    RUN(test_signal_field_observe_is_local_and_bounded);
    RUN(test_signal_field_neighbor_query_blends_without_global_spread);
    RUN(test_signal_field_decay_halves_at_half_life);
    RUN(test_signal_field_diagnostics_report_load_margin_and_noise);
    RUN(test_signal_field_kind_labels_are_stable);
}
