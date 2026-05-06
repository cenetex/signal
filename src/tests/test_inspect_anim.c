/* Regression tests for the per-row content fingerprint that gates the
 * inspect-pane scramble-resolve animation. The animation re-triggers
 * iff a row's signature changes — identical-content frames must hash
 * to the same value, or the eye sees the row scramble every frame. */
#include "tests/test_harness.h"
#include "net.h"
#include "inspect_anim.h"

/* Build a representative row with non-trivial pubkeys + flags so a
 * field-by-field perturbation test has something to flip. */
static void seed_row(NetInspectSnapshotRow *row) {
    memset(row, 0, sizeof(*row));
    row->commodity = 3;
    row->grade = 2;
    row->chain_len = 4;
    row->flags = INSPECT_ROW_HAS_RECEIPT;
    row->event_id = 4422;
    row->quantity = 7;
    for (int i = 0; i < 32; i++) {
        row->cargo_pub[i]       = (uint8_t)(0xA0 + i);
        row->receipt_head[i]    = (uint8_t)(0x10 + i);
        row->origin_station[i]  = (uint8_t)(0x40 + i);
        row->latest_station[i]  = (uint8_t)(0x80 + i);
    }
}

TEST(test_inspect_row_signature_stable_under_identity) {
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    /* Identical content → identical signature. */
    ASSERT(hud_row_signature(&a) == hud_row_signature(&b));
    /* Repeated calls return the same hash. */
    ASSERT(hud_row_signature(&a) == hud_row_signature(&a));
}

TEST(test_inspect_row_signature_diff_on_quantity_change) {
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    b.quantity = (uint16_t)(a.quantity + 1);
    ASSERT(hud_row_signature(&a) != hud_row_signature(&b));
}

TEST(test_inspect_row_signature_diff_on_cargo_pub_change) {
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    b.cargo_pub[0] ^= 0x01u;
    ASSERT(hud_row_signature(&a) != hud_row_signature(&b));
}

TEST(test_inspect_row_signature_diff_on_receipt_head_change) {
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    b.receipt_head[31] ^= 0x80u;
    ASSERT(hud_row_signature(&a) != hud_row_signature(&b));
}

TEST(test_inspect_row_signature_diff_on_chain_len_change) {
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    b.chain_len = (uint8_t)(a.chain_len + 1);
    ASSERT(hud_row_signature(&a) != hud_row_signature(&b));
}

TEST(test_inspect_row_signature_diff_on_grouped_flag) {
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    b.flags ^= INSPECT_ROW_GROUPED;
    ASSERT(hud_row_signature(&a) != hud_row_signature(&b));
}

TEST(test_inspect_row_signature_diff_on_event_id_high_bits) {
    /* Regression: the signature must mix all 8 bytes of event_id. An
     * earlier draft only fed the bottom 16 bits in, which meant two
     * distinct events colliding on those bits would suppress the
     * row's scramble re-trigger. */
    NetInspectSnapshotRow a, b;
    seed_row(&a);
    memcpy(&b, &a, sizeof(b));
    b.event_id = a.event_id ^ ((uint64_t)1u << 40);
    ASSERT(hud_row_signature(&a) != hud_row_signature(&b));
    /* Also a low-byte-aliased pair — same low 16 bits, different
     * upper bits — must hash differently. */
    NetInspectSnapshotRow c, d;
    seed_row(&c);
    seed_row(&d);
    c.event_id = 0x0000000000004422ull;
    d.event_id = 0xDEADBEEF00004422ull;
    ASSERT(hud_row_signature(&c) != hud_row_signature(&d));
}

TEST(test_inspect_row_signature_zero_row_nonzero_hash) {
    /* All-zero row must still produce a non-zero signature so the
     * "uninitialized slot" sentinel (sig == 0 in client state) isn't
     * confused with a real all-zero row. */
    NetInspectSnapshotRow z;
    memset(&z, 0, sizeof(z));
    ASSERT(hud_row_signature(&z) != 0);
}

void register_inspect_anim_tests(void) {
    TEST_SECTION("\nInspect snapshot row animation:\n");
    RUN(test_inspect_row_signature_stable_under_identity);
    RUN(test_inspect_row_signature_diff_on_quantity_change);
    RUN(test_inspect_row_signature_diff_on_cargo_pub_change);
    RUN(test_inspect_row_signature_diff_on_receipt_head_change);
    RUN(test_inspect_row_signature_diff_on_chain_len_change);
    RUN(test_inspect_row_signature_diff_on_grouped_flag);
    RUN(test_inspect_row_signature_diff_on_event_id_high_bits);
    RUN(test_inspect_row_signature_zero_row_nonzero_hash);
}
