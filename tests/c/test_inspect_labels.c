/* Regression tests for player-readable inspect provenance labels. */
#include "test_harness.h"
#include "inspect_labels.h"
#include "npc_identity.h"

TEST(test_inspect_job_source_label_names_relay_and_anchor) {
    NetInspectSnapshotRow row;
    memset(&row, 0, sizeof(row));
    row.cargo_pub[INSPECT_JOB_META_MEMORY_KIND] =
        (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    row.cargo_pub[INSPECT_JOB_META_HOPS] = 2;
    row.cargo_pub[INSPECT_JOB_META_AGE] = 17;
    row.cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++) {
        row.receipt_head[i] = (uint8_t)(0x10 + i);
        row.cargo_pub[INSPECT_JOB_META_PROOF0 + i % 4] =
            (uint8_t)(0xA0 + i);
    }

    char label[96];
    ASSERT(inspect_label_job_source_chain(&row, "Prospect Refinery",
                                          label, sizeof(label)));
    ASSERT(strcmp(label,
                  "heard route @Prospe h2 age17 anchor 10111213") == 0);
}

TEST(test_inspect_market_source_label_names_relay_and_witness) {
    NetInspectSnapshotRow row;
    memset(&row, 0, sizeof(row));
    for (int i = 0; i < 32; i++) {
        row.receipt_head[i] = (uint8_t)(0x40 + i);
        row.origin_station[i] = (uint8_t)(0x70 + i);
        row.latest_station[i] = (uint8_t)(0xA0 + i);
        row.cargo_pub[i] = (uint8_t)(0xD0 + i);
    }

    char label[128];
    ASSERT(inspect_label_market_source_chain(&row, label, sizeof(label)));
    ASSERT(strcmp(label,
                  "anchor 40414243 relay 70717273 witness a0a1a2a3") == 0);
}

TEST(test_inspect_market_source_label_falls_back_to_subject) {
    NetInspectSnapshotRow row;
    memset(&row, 0, sizeof(row));
    for (int i = 0; i < 32; i++)
        row.cargo_pub[i] = (uint8_t)(0x22 + i);

    char label[64];
    ASSERT(inspect_label_market_source_chain(&row, label, sizeof(label)));
    ASSERT(strcmp(label, "subject 22232425") == 0);
}

TEST(test_inspect_job_cause_stitches_source_and_receipt_rows) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 4;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_META_MEMORY_KIND] =
        (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    job->cargo_pub[INSPECT_JOB_META_HOPS] = 2;
    job->cargo_pub[INSPECT_JOB_META_AGE] = 17;
    job->cargo_pub[INSPECT_JOB_META_SOURCE_STATION] = 0;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xC0 + i);

    NetInspectSnapshotRow *source = &snap.rows[1];
    source->commodity = (uint8_t)INSPECT_DIAG_ROUTE_SUCCESS;
    source->flags = INSPECT_ROW_DIAGNOSTIC;
    for (int i = 0; i < 32; i++) {
        source->cargo_pub[i] = (uint8_t)(0x20 + i);
        source->receipt_head[i] = (uint8_t)(0xC0 + i);
        source->origin_station[i] = (uint8_t)(0x60 + i);
        source->latest_station[i] = (uint8_t)(0x90 + i);
    }

    NetInspectSnapshotRow *receipt = &snap.rows[2];
    receipt->flags = INSPECT_ROW_HAS_RECEIPT;
    receipt->chain_len = 2;
    for (int i = 0; i < 32; i++)
        receipt->receipt_head[i] = (uint8_t)(0xC0 + i);

    NetInspectSnapshotRow *link = &snap.rows[3];
    link->commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    link->flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT(cause.job == job);
    ASSERT(cause.source_memory == source);
    ASSERT(cause.receipt == receipt);
    ASSERT(cause.first_receipt_link == link);
    ASSERT(cause.last_receipt_link == link);
    ASSERT_EQ_INT(cause.receipt_link_count, 1);

    char a[112], b[112], c[96];
    ASSERT(inspect_label_job_cause_lines(&cause, "Prospect Refinery",
                                         a, sizeof(a),
                                         b, sizeof(b),
                                         c, sizeof(c)));
    ASSERT(strcmp(a, "cause: heard route @Prospe h2 age17 anchor c0c1c2c3") == 0);
    ASSERT(strcmp(b, "source: anchor c0c1c2c3 relay 60616263 witness 90919293") == 0);
    ASSERT(strcmp(c, "receipt: carried chain 2 links") == 0);
}

TEST(test_inspect_job_detail_lines_are_compact_and_explanatory) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 5;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->grade = 255;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_FACTOR_ROUTE] = 250;
    job->cargo_pub[INSPECT_JOB_FACTOR_DEMAND] = 180;
    job->cargo_pub[INSPECT_JOB_FACTOR_PROOF] = 130;
    job->cargo_pub[INSPECT_JOB_META_REASON] =
        (uint8_t)INSPECT_JOB_REASON_ROUTE_MEMORY;
    job->cargo_pub[INSPECT_JOB_META_MEMORY_KIND] =
        (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    job->cargo_pub[INSPECT_JOB_META_HOPS] = 2;
    job->cargo_pub[INSPECT_JOB_META_AGE] = 17;
    job->cargo_pub[INSPECT_JOB_META_SOURCE_STATION] = 0;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xC0 + i);

    NetInspectSnapshotRow *source = &snap.rows[1];
    source->commodity = (uint8_t)INSPECT_DIAG_ROUTE_SUCCESS;
    source->flags = INSPECT_ROW_DIAGNOSTIC;
    for (int i = 0; i < 32; i++) {
        source->receipt_head[i] = (uint8_t)(0xC0 + i);
        source->origin_station[i] = (uint8_t)(0x60 + i);
        source->latest_station[i] = (uint8_t)(0x90 + i);
    }

    NetInspectSnapshotRow *receipt = &snap.rows[2];
    receipt->flags = INSPECT_ROW_HAS_RECEIPT;
    receipt->chain_len = 2;
    for (int i = 0; i < 32; i++)
        receipt->receipt_head[i] = (uint8_t)(0xC0 + i);

    NetInspectSnapshotRow *link = &snap.rows[3];
    link->commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    link->flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
    link->grade = 1;
    link->chain_len = 2;
    link->event_id = 7101;
    for (int i = 0; i < 32; i++) {
        link->receipt_head[i] = (uint8_t)(0xD0 + i);
        link->origin_station[i] = (uint8_t)(0x70 + i);
        link->latest_station[i] = (uint8_t)(0xA0 + i);
    }
    NetInspectSnapshotRow *link2 = &snap.rows[4];
    link2->commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    link2->flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
    link2->grade = 2;
    link2->chain_len = 2;
    link2->event_id = 7102;
    for (int i = 0; i < 32; i++) {
        link2->receipt_head[i] = (uint8_t)(0xE0 + i);
        link2->origin_station[i] = (uint8_t)(0x80 + i);
        link2->latest_station[i] = (uint8_t)(0xB0 + i);
    }

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT(cause.first_receipt_link == link);
    ASSERT(cause.last_receipt_link == link2);
    ASSERT_EQ_INT(cause.receipt_link_count, 2);

    char a[64], b[64], c[64], d[64], e[64], f[64];
    ASSERT(inspect_label_job_detail_lines(&cause, "Prospect Refinery",
                                          a, sizeof(a),
                                          b, sizeof(b),
                                          c, sizeof(c),
                                          d, sizeof(d),
                                          e, sizeof(e),
                                          f, sizeof(f)));
    ASSERT(strcmp(a, "job: haul selected sc255") == 0);
    ASSERT(strcmp(b, "why: route memory") == 0);
    ASSERT(strcmp(c, "top: route9 demand6 proof5") == 0);
    ASSERT(strcmp(d, "heard: route @Prospe h2 age17") == 0);
    ASSERT(strcmp(e, "proof: c0c1c2c3 relay 60616263") == 0);
    ASSERT(strcmp(f, "links: 2 local signed rows") == 0);

    char links[3][64];
    int link_count = inspect_label_receipt_link_lines_for_cause(
        &snap, &cause, links, 3);
    ASSERT_EQ_INT(link_count, 2);
    ASSERT(strcmp(links[0], "link: 1/2 ev7101 70717273>a0a1a2a3") == 0);
    ASSERT(strcmp(links[1], "link: 2/2 ev7102 80818283>b0b1b2b3") == 0);
}

TEST(test_inspect_receipt_link_lines_can_page_local_rows) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 7;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xC0 + i);

    NetInspectSnapshotRow *receipt = &snap.rows[1];
    receipt->flags = INSPECT_ROW_HAS_RECEIPT;
    receipt->chain_len = 5;
    for (int i = 0; i < 32; i++)
        receipt->receipt_head[i] = (uint8_t)(0xC0 + i);

    for (int li = 0; li < 5; li++) {
        NetInspectSnapshotRow *link = &snap.rows[2 + li];
        link->commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
        link->flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
        link->grade = (uint8_t)(li + 1);
        link->chain_len = 5;
        link->event_id = (uint64_t)(7200 + li);
        for (int i = 0; i < 32; i++) {
            link->receipt_head[i] = (uint8_t)(0xD0 + li + i);
            link->origin_station[i] = (uint8_t)(0x40 + li + i);
            link->latest_station[i] = (uint8_t)(0x80 + li + i);
        }
    }

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT_EQ_INT(cause.receipt_link_count, 5);

    char page[3][64];
    int count = inspect_label_receipt_link_lines_for_cause_page(
        &snap, &cause, 3, page, 3);
    ASSERT_EQ_INT(count, 2);
    ASSERT(strcmp(page[0], "link: 4/5 ev7203 43444546>83848586") == 0);
    ASSERT(strcmp(page[1], "link: 5/5 ev7204 44454647>84858687") == 0);
}

TEST(test_inspect_job_detail_marks_anchor_known_when_receipt_not_local) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 2;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_DELIVER_PROOF;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->grade = 188;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_META_REASON] =
        (uint8_t)INSPECT_JOB_REASON_DELIVERY_PROOF;
    job->cargo_pub[INSPECT_JOB_META_MEMORY_KIND] =
        (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT;
    job->cargo_pub[INSPECT_JOB_META_HOPS] = 3;
    job->cargo_pub[INSPECT_JOB_META_AGE] = 9;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xA0 + i);

    NetInspectSnapshotRow *source = &snap.rows[1];
    source->commodity = (uint8_t)INSPECT_DIAG_DELIVERY_RECEIPT;
    source->flags = INSPECT_ROW_DIAGNOSTIC;
    for (int i = 0; i < 32; i++) {
        source->receipt_head[i] = (uint8_t)(0xA0 + i);
        source->origin_station[i] = (uint8_t)(0x60 + i);
        source->latest_station[i] = (uint8_t)(0x90 + i);
    }

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT(cause.source_memory == source);
    ASSERT(cause.receipt == NULL);
    ASSERT_EQ_INT(cause.receipt_link_count, 0);

    char a[80], b[80], c[80];
    ASSERT(inspect_label_job_cause_lines(&cause, "Kepler Shipyard",
                                         a, sizeof(a),
                                         b, sizeof(b),
                                         c, sizeof(c)));
    ASSERT(strcmp(c, "receipt: anchor known, not local") == 0);

    char d1[64], d2[64], d3[64], d4[64], d5[64], d6[64];
    ASSERT(inspect_label_job_detail_lines(&cause, "Kepler Shipyard",
                                          d1, sizeof(d1),
                                          d2, sizeof(d2),
                                          d3, sizeof(d3),
                                          d4, sizeof(d4),
                                          d5, sizeof(d5),
                                          d6, sizeof(d6)));
    ASSERT(strcmp(d5, "proof: a0a1a2a3 relay 60616263") == 0);
    ASSERT(strcmp(d6, "links: anchor known, chain not local") == 0);
}

TEST(test_inspect_job_detail_labels_station_retrieved_receipt_chain) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 3;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xD8 + i);

    NetInspectSnapshotRow *receipt = &snap.rows[1];
    receipt->flags = INSPECT_ROW_HAS_RECEIPT | INSPECT_ROW_STATION_RECEIPT;
    receipt->chain_len = 2;
    for (int i = 0; i < 32; i++)
        receipt->receipt_head[i] = (uint8_t)(0xD8 + i);

    NetInspectSnapshotRow *link = &snap.rows[2];
    link->commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    link->flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
    link->grade = 1;
    link->chain_len = 2;
    link->event_id = 7201;
    for (int i = 0; i < 32; i++)
        link->receipt_head[i] = (uint8_t)(0xE8 + i);

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT(cause.receipt == receipt);
    ASSERT(cause.first_receipt_link == link);
    ASSERT_EQ_INT(cause.receipt_link_count, 1);

    char a[80], b[80], c[80];
    ASSERT(inspect_label_job_cause_lines(&cause, "Prospect",
                                         a, sizeof(a),
                                         b, sizeof(b),
                                         c, sizeof(c)));
    ASSERT(strcmp(c, "receipt: station chain 2 links") == 0);

    char d1[64], d2[64], d3[64], d4[64], d5[64], d6[64];
    ASSERT(inspect_label_job_detail_lines(&cause, "Prospect",
                                          d1, sizeof(d1),
                                          d2, sizeof(d2),
                                          d3, sizeof(d3),
                                          d4, sizeof(d4),
                                          d5, sizeof(d5),
                                          d6, sizeof(d6)));
    ASSERT(strcmp(d6, "links: 1 station signed row") == 0);
}

TEST(test_inspect_job_detail_marks_proof_anchor_only_without_source_memory) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 1;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xB0 + i);

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT(cause.source_memory == NULL);
    ASSERT(cause.receipt == NULL);

    char a[80], b[80], c[80];
    ASSERT(inspect_label_job_cause_lines(&cause, NULL,
                                         a, sizeof(a),
                                         b, sizeof(b),
                                         c, sizeof(c)));
    ASSERT(strcmp(b, "source: proof anchor only") == 0);
    ASSERT(strcmp(c, "receipt: proof anchor only") == 0);

    char d1[64], d2[64], d3[64], d4[64], d5[64], d6[64];
    ASSERT(inspect_label_job_detail_lines(&cause, NULL,
                                          d1, sizeof(d1),
                                          d2, sizeof(d2),
                                          d3, sizeof(d3),
                                          d4, sizeof(d4),
                                          d5, sizeof(d5),
                                          d6, sizeof(d6)));
    ASSERT(strcmp(d5, "proof: b0b1b2b3 anchor only") == 0);
    ASSERT(strcmp(d6, "links: proof anchor only") == 0);
}

TEST(test_inspect_job_detail_marks_carried_chain_without_local_link_rows) {
    NetInspectSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_type = INSPECT_TARGET_NPC;
    snap.row_count = 2;

    NetInspectSnapshotRow *job = &snap.rows[0];
    job->commodity = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    job->flags = INSPECT_ROW_DIAGNOSTIC;
    job->chain_len = 255;
    job->cargo_pub[INSPECT_JOB_META_PROOF_KIND] =
        (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int i = 0; i < 32; i++)
        job->receipt_head[i] = (uint8_t)(0xC8 + i);

    NetInspectSnapshotRow *receipt = &snap.rows[1];
    receipt->flags = INSPECT_ROW_HAS_RECEIPT;
    receipt->chain_len = 4;
    for (int i = 0; i < 32; i++)
        receipt->receipt_head[i] = (uint8_t)(0xC8 + i);

    InspectJobCause cause;
    ASSERT(inspect_label_find_job_cause(&snap, &cause));
    ASSERT(cause.receipt == receipt);
    ASSERT_EQ_INT(cause.receipt_link_count, 0);

    char d1[64], d2[64], d3[64], d4[64], d5[64], d6[64];
    ASSERT(inspect_label_job_detail_lines(&cause, NULL,
                                          d1, sizeof(d1),
                                          d2, sizeof(d2),
                                          d3, sizeof(d3),
                                          d4, sizeof(d4),
                                          d5, sizeof(d5),
                                          d6, sizeof(d6)));
    ASSERT(strcmp(d6, "links: carried chain, no local rows") == 0);
}

TEST(test_inspect_receipt_browser_footer_formats_controls) {
    char out[96];
    ASSERT(inspect_label_receipt_browser_footer(0, 0, 0, false,
                                                out, sizeof(out)));
    ASSERT(strcmp(out, "no local receipt links  [Shift+TAB] close") == 0);

    ASSERT(inspect_label_receipt_browser_footer(2, 0, 2, false,
                                                out, sizeof(out)));
    ASSERT(strcmp(out, "links 2/2  [Shift+TAB] close") == 0);

    ASSERT(inspect_label_receipt_browser_footer(9, 3, 3, true,
                                                out, sizeof(out)));
    ASSERT(strcmp(out, "links 4-6/9  [TAB] next  [Shift+TAB] close") == 0);
}

TEST(test_inspect_receipt_link_line_can_use_named_relays) {
    NetInspectSnapshotRow link;
    memset(&link, 0, sizeof(link));
    link.commodity = (uint8_t)INSPECT_DIAG_RECEIPT_LINK;
    link.flags = INSPECT_ROW_DIAGNOSTIC | INSPECT_ROW_HAS_RECEIPT;
    link.grade = 3;
    link.chain_len = 5;
    link.event_id = 7304;
    for (int i = 0; i < 32; i++)
        link.receipt_head[i] = (uint8_t)(0xE0 + i);

    char out[80];
    ASSERT(inspect_label_receipt_link_line_named(
        &link, "Prospect Refinery", "Kepler Shipyard", out, sizeof(out)));
    ASSERT(strcmp(out, "link: 3/5 ev7304 Prospect Ref>Kepler Shipy") == 0);

    ASSERT(inspect_label_receipt_link_line_named(
        &link, NULL, NULL, out, sizeof(out)));
    ASSERT(strcmp(out, "link: 3/5 ev7304 head e0e1e2e3") == 0);
}

TEST(test_inspect_npc_custody_pubkey_has_stable_golden_vector) {
    const uint8_t token[8] = { 'N', 'P', 'C', 2, 1, 7, 0x34, 0x12 };
    const uint8_t expected[32] = {
        0x1d, 0x03, 0xe0, 0xb1, 0xa2, 0xce, 0xc1, 0xf5,
        0xf7, 0x36, 0xaa, 0x38, 0x07, 0x4c, 0x0e, 0xfd,
        0xc1, 0xd7, 0x02, 0x12, 0x05, 0x7b, 0xc7, 0x75,
        0x78, 0xae, 0x2c, 0x4a, 0x5c, 0x24, 0xe8, 0xec,
    };
    uint8_t actual[32];
    npc_custody_pubkey_from_fields(token, 7, 1, 2, actual);
    ASSERT(memcmp(actual, expected, sizeof(expected)) == 0);

    uint8_t other_slot[32];
    npc_custody_pubkey_from_fields(token, 8, 1, 2, other_slot);
    ASSERT(memcmp(actual, other_slot, sizeof(actual)) != 0);
}

void register_inspect_label_tests(void) {
    TEST_SECTION("\nInspect provenance labels:\n");
    RUN(test_inspect_job_source_label_names_relay_and_anchor);
    RUN(test_inspect_market_source_label_names_relay_and_witness);
    RUN(test_inspect_market_source_label_falls_back_to_subject);
    RUN(test_inspect_job_cause_stitches_source_and_receipt_rows);
    RUN(test_inspect_job_detail_lines_are_compact_and_explanatory);
    RUN(test_inspect_receipt_link_lines_can_page_local_rows);
    RUN(test_inspect_job_detail_marks_anchor_known_when_receipt_not_local);
    RUN(test_inspect_job_detail_labels_station_retrieved_receipt_chain);
    RUN(test_inspect_job_detail_marks_proof_anchor_only_without_source_memory);
    RUN(test_inspect_job_detail_marks_carried_chain_without_local_link_rows);
    RUN(test_inspect_receipt_browser_footer_formats_controls);
    RUN(test_inspect_receipt_link_line_can_use_named_relays);
    RUN(test_inspect_npc_custody_pubkey_has_stable_golden_vector);
}
