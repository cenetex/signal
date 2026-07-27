#include "test_harness.h"

#include "ownership_quarantine.h"

static ownership_quarantine_entry_t quarantine_row(
    uint8_t source_kind,
    uint8_t reason,
    uint16_t station_index,
    uint16_t row_index,
    uint16_t legacy_actor_code) {
    ownership_quarantine_entry_t row = {
        .record_id =
            ((uint64_t)source_kind << 48) |
            ((uint64_t)station_index << 32) |
            ((uint64_t)row_index << 16) |
            (uint64_t)legacy_actor_code,
        .source_kind = source_kind,
        .reason = reason,
        .station_index = station_index,
        .row_index = row_index,
        .legacy_actor_code = legacy_actor_code,
    };
    return row;
}

TEST(test_ownership_quarantine_tags_and_row_size_are_stable) {
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_NONE, 0);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_CONTRACT, 1);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT, 2);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_STATION_PLANNED_OWNER, 3);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD, 4);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD, 5);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN, 6);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET, 7);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR, 8);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_TOWED, 9);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_FRACTURED, 10);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_THROWN_BY, 11);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_SCAFFOLD_OWNER, 12);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER, 13);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_SOURCE_COUNT, 14);

    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_REASON_NONE, 0);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN, 1);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN, 2);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL, 3);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL, 4);
    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_REASON_COUNT, 5);

    ASSERT_EQ_INT(OWNERSHIP_QUARANTINE_CAP, 16384);
    ASSERT_EQ_INT(
        OWNERSHIP_QUARANTINE_HEADER_WIRE_SIZE, 10);
    ASSERT_EQ_INT(
        OWNERSHIP_QUARANTINE_ENTRY_WIRE_SIZE, 16);
    ASSERT_EQ_INT(
        (int)sizeof(((ownership_quarantine_t *)0)->count),
        (int)sizeof(uint16_t));
}

TEST(test_ownership_quarantine_canonical_matrix_and_invalid_locators) {
    ownership_quarantine_entry_t valid[] = {
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                       OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN,
                       OWNERSHIP_QUARANTINE_NA, MAX_CONTRACTS - 1,
                       MAX_PLAYERS - 1),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT,
                       OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN,
                       OWNERSHIP_QUARANTINE_NA,
                       MAX_DELIVERY_SHIPMENTS - 1,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_STATION_PLANNED_OWNER,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       SIGNAL_FIRST_OUTPOST_INDEX,
                       OWNERSHIP_QUARANTINE_NA, 0),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD,
                       OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL,
                       MAX_STATIONS - 1, 3,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_PENDING_SHIP_BUILD,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       MAX_STATIONS - 1, 3, MAX_PLAYERS - 1),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       MAX_STATIONS - 1, 7, OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_SHIP_ASSET,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       OWNERSHIP_QUARANTINE_NA, MAX_SHIP_ASSETS - 1,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_CARGO_POD_TRACTOR,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       OWNERSHIP_QUARANTINE_NA, MAX_CARGO_PODS - 1,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_TOWED,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       OWNERSHIP_QUARANTINE_NA, MAX_ASTEROIDS - 1,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(
            OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_LAST_FRACTURED,
            OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
            OWNERSHIP_QUARANTINE_NA, MAX_ASTEROIDS - 1,
            OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_FRACTURE_THROWN_BY,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       OWNERSHIP_QUARANTINE_NA, MAX_ASTEROIDS - 1,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_SCAFFOLD_OWNER,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       OWNERSHIP_QUARANTINE_NA, MAX_SCAFFOLDS - 1,
                       OWNERSHIP_QUARANTINE_NA),
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       MAX_STATIONS - 1, OWNERSHIP_QUARANTINE_NA,
                       OWNERSHIP_QUARANTINE_NA),
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        ASSERT(ownership_quarantine_entry_is_canonical(&valid[i]));

    ownership_quarantine_entry_t row = valid[0];
    row.record_id = 0;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.source_kind = OWNERSHIP_QUARANTINE_SOURCE_NONE;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.source_kind = OWNERSHIP_QUARANTINE_SOURCE_COUNT;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.reason = OWNERSHIP_QUARANTINE_REASON_NONE;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.reason = OWNERSHIP_QUARANTINE_REASON_COUNT;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));

    row = valid[0];
    row.station_index = 0;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.row_index = MAX_CONTRACTS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.legacy_actor_code = MAX_PLAYERS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[0];
    row.legacy_actor_code = OWNERSHIP_QUARANTINE_NA;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));

    row = valid[1];
    row.legacy_actor_code = MAX_PLAYERS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[2];
    row.station_index = SIGNAL_FIRST_OUTPOST_INDEX - 1;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[2];
    row.row_index = 0;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[3];
    row.row_index = 4;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[4];
    row.row_index = 4;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[5];
    row.row_index = 8;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[6];
    row.row_index = MAX_SHIP_ASSETS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[7];
    row.row_index = MAX_CARGO_PODS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[8];
    row.row_index = MAX_ASTEROIDS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[11];
    row.row_index = MAX_SCAFFOLDS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    row = valid[12];
    row.station_index = MAX_STATIONS;
    ASSERT(!ownership_quarantine_entry_is_canonical(&row));
    ASSERT(!ownership_quarantine_entry_is_canonical(NULL));
}

TEST(test_ownership_quarantine_add_is_monotonic_and_transactional) {
    ownership_quarantine_t *table = calloc(1, sizeof(*table));
    ownership_quarantine_t *before = malloc(sizeof(*before));
    ASSERT(table && before);

    ownership_quarantine_entry_t placement =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       2, 7, OWNERSHIP_QUARANTINE_NA);
    ownership_quarantine_entry_t contract_late =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                       OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN,
                       OWNERSHIP_QUARANTINE_NA, 9, 1);
    ownership_quarantine_entry_t contract_early =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                       OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN,
                       OWNERSHIP_QUARANTINE_NA, 2, 3);
    ownership_quarantine_entry_t scaffold =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       5, 1, OWNERSHIP_QUARANTINE_NA);
    placement.record_id = 10;
    contract_late.record_id = 20;
    scaffold.record_id = 30;
    contract_early.record_id = 40;

    ASSERT(ownership_quarantine_add(table, &placement));
    ASSERT(ownership_quarantine_add(table, &contract_late));
    ASSERT(ownership_quarantine_add(table, &scaffold));
    ASSERT(ownership_quarantine_add(table, &contract_early));
    ASSERT(ownership_quarantine_validate(table));
    ASSERT_EQ_INT(table->count, 4);
    ASSERT_EQ_INT(table->entries[0].source_kind,
                  OWNERSHIP_QUARANTINE_SOURCE_PLACEMENT_PLAN);
    ASSERT_EQ_INT(table->entries[1].source_kind,
                  OWNERSHIP_QUARANTINE_SOURCE_CONTRACT);
    ASSERT_EQ_INT(table->entries[1].row_index, 9);
    ASSERT_EQ_INT(table->entries[2].source_kind,
                  OWNERSHIP_QUARANTINE_SOURCE_PENDING_SCAFFOLD);
    ASSERT_EQ_INT(table->entries[3].source_kind,
                  OWNERSHIP_QUARANTINE_SOURCE_CONTRACT);
    ASSERT_EQ_INT(table->entries[3].row_index, 2);
    ASSERT(table->record_id_high_water == 40);

    memcpy(before, table, sizeof(*before));
    ownership_quarantine_entry_t duplicate = contract_early;
    duplicate.reason =
        OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL;
    duplicate.legacy_actor_code = OWNERSHIP_QUARANTINE_NA;
    ASSERT(!ownership_quarantine_add(table, &duplicate));
    ASSERT(memcmp(table, before, sizeof(*table)) == 0);

    /*
     * Recycled source locators remain distinct through stable record IDs.
     * Build the next candidate in the unused table slot to cover aliasing
     * with the append destination.
     */
    ownership_quarantine_clear(table);
    ownership_quarantine_entry_t recycled_early = contract_early;
    ownership_quarantine_entry_t recycled_middle = contract_early;
    ownership_quarantine_entry_t recycled_late = contract_early;
    recycled_early.record_id = 10;
    recycled_middle.record_id = 20;
    recycled_late.record_id = 30;
    ASSERT(ownership_quarantine_add(table, &recycled_early));
    ASSERT(ownership_quarantine_add(table, &recycled_middle));
    table->entries[table->count] = recycled_late;
    ASSERT(ownership_quarantine_add(
        table, &table->entries[table->count]));
    ASSERT(ownership_quarantine_validate(table));
    ASSERT_EQ_INT(table->count, 3);
    ASSERT(table->entries[0].record_id == 10);
    ASSERT(table->entries[1].record_id == 20);
    ASSERT(table->entries[2].record_id == 30);
    ASSERT_EQ_INT(table->entries[0].row_index,
                  table->entries[2].row_index);
    ASSERT(table->record_id_high_water == 30);

    ownership_quarantine_entry_t duplicate_id = placement;
    duplicate_id.record_id = 20;
    memcpy(before, table, sizeof(*before));
    ASSERT(!ownership_quarantine_add(table, &duplicate_id));
    ASSERT(memcmp(table, before, sizeof(*table)) == 0);

    /*
     * Resolving/removing a row does not lower the persisted high-water mark.
     * The removed ID remains retired and normal allocation advances past it.
     */
    table->count = 2;
    ASSERT(ownership_quarantine_validate(table));
    uint64_t next_record_id = 0;
    ASSERT(ownership_quarantine_next_record_id(
        table, &next_record_id));
    ASSERT(next_record_id == 31);
    ASSERT(!ownership_quarantine_add(table, &recycled_late));
    recycled_late.record_id = next_record_id;
    ASSERT(ownership_quarantine_add(table, &recycled_late));
    ASSERT(table->record_id_high_water == 31);

    table->entries[1] = table->entries[0];
    ASSERT(!ownership_quarantine_validate(table));
    memcpy(before, table, sizeof(*before));
    placement.record_id = 41;
    ASSERT(!ownership_quarantine_add(table, &placement));
    ASSERT(memcmp(table, before, sizeof(*table)) == 0);
    ASSERT(!ownership_quarantine_next_record_id(table, &next_record_id));
    ASSERT(!ownership_quarantine_next_record_id(NULL, &next_record_id));
    ASSERT(!ownership_quarantine_next_record_id(table, NULL));

    free(before);
    free(table);
}

TEST(test_ownership_quarantine_capacity_and_clear_fail_closed) {
    ownership_quarantine_t *table = calloc(1, sizeof(*table));
    ownership_quarantine_t *before = malloc(sizeof(*before));
    ASSERT(table && before);

    table->count = OWNERSHIP_QUARANTINE_CAP;
    table->entries[0].source_kind = UINT8_MAX;
    memcpy(before, table, sizeof(*before));
    ownership_quarantine_entry_t row =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                       OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN,
                       OWNERSHIP_QUARANTINE_NA, 0, 0);
    ASSERT(!ownership_quarantine_add(table, &row));
    ASSERT(memcmp(table, before, sizeof(*table)) == 0);

    table->count = (uint16_t)(OWNERSHIP_QUARANTINE_CAP + 1);
    ASSERT(!ownership_quarantine_validate(table));
    ownership_quarantine_clear(table);
    ASSERT_EQ_INT(table->count, 0);
    uint8_t *bytes = (uint8_t *)table;
    uint8_t any = 0;
    for (size_t i = 0; i < sizeof(*table); i++)
        any |= bytes[i];
    ASSERT_EQ_INT(any, 0);
    ownership_quarantine_clear(NULL);

    free(before);
    free(table);
}

TEST(test_ownership_quarantine_names_and_report_are_public_only) {
    ASSERT_STR_EQ(
        ownership_quarantine_source_name(
            OWNERSHIP_QUARANTINE_SOURCE_DELIVERY_SHIPMENT),
        "delivery_shipment");
    ASSERT_STR_EQ(
        ownership_quarantine_source_name(
            OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER),
        "outpost_founder");
    ASSERT_STR_EQ(ownership_quarantine_source_name(UINT8_MAX), "unknown");
    ASSERT_STR_EQ(
        ownership_quarantine_reason_name(
            OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN),
        "legacy_slot_unproven");
    ASSERT_STR_EQ(
        ownership_quarantine_reason_name(
            OWNERSHIP_QUARANTINE_REASON_CONFLICTING_PRINCIPAL),
        "conflicting_principal");
    ASSERT_STR_EQ(ownership_quarantine_reason_name(UINT8_MAX), "unknown");
    ASSERT_STR_EQ(
        ownership_quarantine_reason_description(
            OWNERSHIP_QUARANTINE_REASON_LEGACY_SESSION_UNPROVEN),
        "legacy session token is bearer material, not durable ownership proof");
    ASSERT_STR_EQ(
        ownership_quarantine_reason_description(UINT8_MAX),
        "unknown quarantine reason");

    ownership_quarantine_t *table = calloc(1, sizeof(*table));
    ASSERT(table);
    ownership_quarantine_entry_t contract =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_CONTRACT,
                       OWNERSHIP_QUARANTINE_REASON_LEGACY_SLOT_UNPROVEN,
                       OWNERSHIP_QUARANTINE_NA, 7, 3);
    ownership_quarantine_entry_t outpost =
        quarantine_row(OWNERSHIP_QUARANTINE_SOURCE_OUTPOST_FOUNDER,
                       OWNERSHIP_QUARANTINE_REASON_INVALID_PRINCIPAL,
                       SIGNAL_FIRST_OUTPOST_INDEX,
                       OWNERSHIP_QUARANTINE_NA,
                       OWNERSHIP_QUARANTINE_NA);
    ASSERT(ownership_quarantine_add(table, &contract));
    outpost.record_id = contract.record_id + 1;
    ASSERT(ownership_quarantine_add(table, &outpost));

    FILE *report = tmpfile();
    ASSERT(report);
    ASSERT(ownership_quarantine_report(report, table));
    ASSERT(fflush(report) == 0);
    ASSERT(fseek(report, 0, SEEK_SET) == 0);
    char text[1024] = {0};
    size_t len = fread(text, 1, sizeof(text) - 1, report);
    ASSERT(!ferror(report));
    text[len] = '\0';
    ASSERT(fclose(report) == 0);

    ASSERT(strstr(text, "ownership_quarantine count=2") != NULL);
    ASSERT(strstr(text, "record_id_high_water=") != NULL);
    ASSERT(strstr(text, "id=") != NULL);
    ASSERT(strstr(text, "source=contract") != NULL);
    ASSERT(strstr(text, "reason=legacy_slot_unproven") != NULL);
    ASSERT(strstr(
        text,
        "detail=\"legacy runtime slot had no unambiguous stable actor proof\"")
        != NULL);
    ASSERT(strstr(text, "station=n/a row=7 legacy_actor=3") != NULL);
    ASSERT(strstr(text, "source=outpost_founder") != NULL);
    ASSERT(strstr(text, "station=4 row=n/a legacy_actor=n/a") != NULL);
    ASSERT(strstr(text,
                  "00112233445566778899aabbccddeeff") == NULL);
    ASSERT(strstr(text,
                  "fedcba98765432100123456789abcdef") == NULL);
    ASSERT(!ownership_quarantine_report(NULL, table));

    report = tmpfile();
    ASSERT(report);
    ASSERT(ownership_quarantine_report_bounded(report, table, 1));
    ASSERT(fflush(report) == 0);
    ASSERT(fseek(report, 0, SEEK_SET) == 0);
    memset(text, 0, sizeof(text));
    len = fread(text, 1, sizeof(text) - 1, report);
    ASSERT(!ferror(report));
    text[len] = '\0';
    ASSERT(fclose(report) == 0);
    ASSERT(strstr(text, "source=contract") != NULL);
    ASSERT(strstr(text, "source=outpost_founder") == NULL);
    ASSERT(strstr(text, "... omitted=1") != NULL);
    ASSERT(!ownership_quarantine_report_bounded(NULL, table, 1));

    free(table);
}

void register_ownership_quarantine_tests(void) {
    TEST_SECTION("\nOwnership quarantine tests:\n");
    RUN(test_ownership_quarantine_tags_and_row_size_are_stable);
    RUN(test_ownership_quarantine_canonical_matrix_and_invalid_locators);
    RUN(test_ownership_quarantine_add_is_monotonic_and_transactional);
    RUN(test_ownership_quarantine_capacity_and_clear_fail_closed);
    RUN(test_ownership_quarantine_names_and_report_are_public_only);
}
