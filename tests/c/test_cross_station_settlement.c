/*
 * test_cross_station_settlement.c -- Layer D of #479: portable cargo
 * receipts. See server/cargo_receipt_issue.h + shared/cargo_receipt.h
 * for the design.
 *
 * Tests exercise the receipt issuance + verification machinery
 * directly (the WS handler in server/main.c is the user). The flow:
 *
 *   1. Station A signs a receipt for cargo C bound to player P.
 *   2. P presents that receipt to station B.
 *   3. B verifies the chain end-to-end before accepting and issues
 *      its OWN receipt for the receiving leg.
 *
 * We synthesize each hop via cargo_receipt_emit_transfer (the same
 * primitive main.c calls), then verify the resulting chain.
 */
#include "test_harness.h"

#include "cargo_receipt.h"
#include "cargo_receipt_issue.h"
#include "chain_log.h"
#include "station_authority.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#  include <direct.h>
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

/* Per-test scratch dir helpers — mirror the chain_test_setup pattern
 * used by test_chain_log.c so concurrent shards stay isolated. */
static void crs_setup(const char *suffix) {
    char path[256];
    snprintf(path, sizeof(path), "%s_crs_%s", TMP("crs"), suffix);
    chain_log_set_dir(path);
}

static void crs_teardown(void) {
    chain_log_set_dir(NULL);
}

static void crs_wipe_logs(world_t *w) {
    for (int s = 0; s < MAX_STATIONS; s++)
        chain_log_reset(&w->stations[s]);
}

/* Initialize an already-allocated world with seeded chain state so
 * each test starts from a clean per-station chain head.
 * Caller owns the world_t (typically via WORLD_HEAP / calloc). */
static void crs_world_init(world_t *w, uint32_t seed) {
    w->rng = seed;
    world_reset(w);
    crs_wipe_logs(w);
    for (int s = 0; s < 3; s++) {
        w->stations[s].chain_event_count = 0;
        memset(w->stations[s].chain_last_hash, 0, 32);
    }
}

/* Synthesize a deterministic player pubkey + cargo pub for a test. */
static void fill_test_pubkey(uint8_t out[32], uint8_t seed) {
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(seed + i);
}

/* ---------------- Test 1: single-hop receipt ----------------------- */

TEST(test_cross_station_single_hop_receipt) {
    crs_setup("single_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD001);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x10);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x40);

    /* Helios (idx 2) issues a receipt for cargo to the player. */
    station_t *helios = &w->stations[2];
    cargo_receipt_t r;
    uint64_t event_id = cargo_receipt_emit_transfer(
        w, helios,
        helios->station_pubkey, player_pk,
        cargo_pk, (uint8_t)CARGO_KIND_INGOT,
        helios->chain_last_hash, /* origin pin to event already in log? — empty log → 0
                                    * — use a non-zero string-of-zeros placeholder via SMELT first */
        &r);
    /* chain_last_hash is all-zero for an empty log; the helper uses it
     * as the origin pin. cargo_receipt_chain_verify rejects all-zero
     * pins, so first emit a synthetic "anchor" event so the receipt's
     * origin pin is the hash of THAT event. */
    ASSERT(event_id == 0 || event_id >= 1);
    /* If the empty-log path triggered, redo with a SMELT seed first. */
    if (event_id != 0) {
        /* Receipt was issued; verify signature in isolation first. */
        ASSERT(cargo_receipt_verify_signature(&r));
        /* prev_receipt_hash equals the post-emit chain_last_hash, which
         * is the hash of THIS transfer event itself (anchored). */
    } else {
        /* Re-emit a SMELT first so the chain has a non-zero last hash. */
        ASSERT(chain_log_emit(w, helios, CHAIN_EVT_SMELT, "x", 1) >= 1);
        event_id = cargo_receipt_emit_transfer(
            w, helios,
            helios->station_pubkey, player_pk,
            cargo_pk, (uint8_t)CARGO_KIND_INGOT,
            helios->chain_last_hash, &r);
        ASSERT(event_id >= 1);
        ASSERT(cargo_receipt_verify_signature(&r));
    }
    /* Receipt fields are populated correctly. */
    ASSERT(memcmp(r.cargo_pub, cargo_pk, 32) == 0);
    ASSERT(memcmp(r.recipient_pubkey, player_pk, 32) == 0);
    ASSERT(memcmp(r.authoring_station, helios->station_pubkey, 32) == 0);
    /* Single-hop chain verifies. */
    ASSERT(cargo_receipt_chain_verify(&r, 1, cargo_pk) == CARGO_RECEIPT_OK);

    crs_teardown();
}

/* ---------------- Test 2: two-hop receipt chain --------------------- */

/* Helper: emit cargo issuance from station + first hop receipt for player. */
static bool crs_first_hop(world_t *w, station_t *st, const uint8_t player_pk[32],
                          const uint8_t cargo_pk[32], cargo_receipt_t *out) {
    /* Anchor: emit a synthetic SMELT first so chain_last_hash is non-zero. */
    if (chain_log_emit(w, st, CHAIN_EVT_SMELT, "smelt", 5) == 0) return false;
    uint64_t id = cargo_receipt_emit_transfer(
        w, st, st->station_pubkey, player_pk, cargo_pk,
        (uint8_t)CARGO_KIND_INGOT, st->chain_last_hash, out);
    return id != 0;
}

/* Helper: emit destination-station receipt for the second hop. */
static bool crs_next_hop(world_t *w, station_t *dst, const uint8_t from_pk[32],
                         const uint8_t cargo_pk[32],
                         const cargo_receipt_t *prev, cargo_receipt_t *out) {
    uint8_t prev_hash[32];
    cargo_receipt_hash(prev, prev_hash);
    uint64_t id = cargo_receipt_emit_transfer(
        w, dst, from_pk, dst->station_pubkey, cargo_pk,
        (uint8_t)CARGO_KIND_INGOT, prev_hash, out);
    return id != 0;
}

TEST(test_cross_station_two_hop_chain) {
    crs_setup("two_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD002);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x20);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x50);

    /* Hop 1: Helios -> player. */
    station_t *helios = &w->stations[2];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Hop 2: player -> Kepler. Kepler verifies r1 then signs r2 whose
     * prev_receipt_hash = SHA-256(r1). */
    station_t *kepler = &w->stations[1];
    cargo_receipt_t r2;
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, &r2));

    /* Two-hop chain verifies. */
    cargo_receipt_t chain[2] = { r1, r2 };
    ASSERT(cargo_receipt_chain_verify(chain, 2, cargo_pk) == CARGO_RECEIPT_OK);

    /* Each hop is signed by the right station. */
    ASSERT(memcmp(r1.authoring_station, helios->station_pubkey, 32) == 0);
    ASSERT(memcmp(r2.authoring_station, kepler->station_pubkey, 32) == 0);
    /* Kepler's chain log has the EVT_TRANSFER in it. */
    uint64_t walked = 0;
    ASSERT(chain_log_verify(kepler, &walked, NULL));
    ASSERT(walked >= 1);

    crs_teardown();
}

/* ---------------- Test 3: forged receipt rejection ------------------ */

TEST(test_cross_station_forged_receipt_rejected) {
    crs_setup("forged");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD003);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x30);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x60);

    /* Hop 1: Helios -> player. */
    station_t *helios = &w->stations[2];
    station_t *prospect = &w->stations[0];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Tamper: rewrite r1.authoring_station to claim it was Prospect.
     * Signature was made with Helios's secret over a body that named
     * Helios. After this overwrite, Prospect's pubkey gets fed to
     * Ed25519 verify against Helios's signature → should fail. */
    memcpy(r1.authoring_station, prospect->station_pubkey, 32);
    ASSERT(!cargo_receipt_verify_signature(&r1));
    ASSERT(cargo_receipt_chain_verify(&r1, 1, cargo_pk)
           == CARGO_RECEIPT_REJECT_BAD_SIGNATURE);

    crs_teardown();
}

/* ---------------- Test 4: tampered chain rejection ------------------ */

TEST(test_cross_station_tampered_cargo_pub_rejected) {
    crs_setup("tampered");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD004);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x40);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x70);

    station_t *helios = &w->stations[2];
    cargo_receipt_t r1;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));

    /* Flip a bit in cargo_pub — the signature was over the original
     * 144-byte unsigned span, so verify must fail. */
    r1.cargo_pub[7] ^= 0xFF;
    ASSERT(!cargo_receipt_verify_signature(&r1));
    ASSERT(cargo_receipt_chain_verify(&r1, 1, NULL)
           == CARGO_RECEIPT_REJECT_BAD_SIGNATURE);

    crs_teardown();
}

/* ---------------- Test 5: broken linkage rejection ------------------ */

TEST(test_cross_station_broken_linkage_rejected) {
    crs_setup("linkage");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD005);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x50);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x80);

    station_t *helios = &w->stations[2];
    station_t *kepler = &w->stations[1];
    cargo_receipt_t r1, r2;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, &r2));

    /* Replace r1 with a different first-hop receipt issued by Helios
     * for the same cargo but in a fresh chain (different chain_last_hash
     * → different prev_receipt_hash). r2's prev_receipt_hash still
     * points at the original r1, so the linkage check must fail. */
    cargo_receipt_t r1_alt;
    ASSERT(chain_log_emit(w, helios, CHAIN_EVT_SMELT, "alt", 3) >= 1);
    uint64_t alt_id = cargo_receipt_emit_transfer(
        w, helios, helios->station_pubkey, player_pk, cargo_pk,
        (uint8_t)CARGO_KIND_INGOT, helios->chain_last_hash, &r1_alt);
    ASSERT(alt_id != 0);
    /* Sanity: the alternative receipt verifies on its own. */
    ASSERT(cargo_receipt_verify_signature(&r1_alt));

    cargo_receipt_t chain[2] = { r1_alt, r2 };
    ASSERT(cargo_receipt_chain_verify(chain, 2, cargo_pk)
           == CARGO_RECEIPT_REJECT_BROKEN_LINKAGE);

    crs_teardown();
}

/* ---------------- Test 6: three-hop chain --------------------------- */

TEST(test_cross_station_three_hop_chain) {
    crs_setup("three_hop");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD006);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x60);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0x90);

    /* Hop 1: Prospect -> player. */
    station_t *prospect = &w->stations[0];
    station_t *kepler   = &w->stations[1];
    station_t *helios   = &w->stations[2];

    cargo_receipt_t r1, r2, r3;
    ASSERT(crs_first_hop(w, prospect, player_pk, cargo_pk, &r1));
    /* Hop 2: player -> Kepler. */
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, &r2));
    /* Hop 3: player -> Helios (re-extract from Kepler back through
     * the player; in real flow Kepler issues to player, player carries
     * to Helios). */
    ASSERT(crs_next_hop(w, helios, player_pk, cargo_pk, &r2, &r3));

    cargo_receipt_t chain[3] = { r1, r2, r3 };
    ASSERT(cargo_receipt_chain_verify(chain, 3, cargo_pk) == CARGO_RECEIPT_OK);
    ASSERT(memcmp(r1.authoring_station, prospect->station_pubkey, 32) == 0);
    ASSERT(memcmp(r2.authoring_station, kepler->station_pubkey, 32) == 0);
    ASSERT(memcmp(r3.authoring_station, helios->station_pubkey, 32) == 0);

    crs_teardown();
}

/* ---------------- Test 7: NPC-mediated transfer -------------------- */
/* The wire path for NPC haulers is the same primitive — they issue
 * via cargo_receipt_emit_transfer just like a player does. We model
 * an NPC by using a deterministic NPC pubkey instead of a player one;
 * the math is identical. */

TEST(test_cross_station_npc_mediated_transfer) {
    crs_setup("npc");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD007);
    ASSERT(w != NULL);

    uint8_t npc_pk[32];   fill_test_pubkey(npc_pk,   0x70);
    uint8_t cargo_pk[32]; fill_test_pubkey(cargo_pk, 0xA0);

    station_t *helios = &w->stations[2];
    station_t *kepler = &w->stations[1];

    cargo_receipt_t r1, r2;
    ASSERT(crs_first_hop(w, helios, npc_pk, cargo_pk, &r1));
    ASSERT(crs_next_hop(w, kepler, npc_pk, cargo_pk, &r1, &r2));

    /* Kepler accepts: chain validates. */
    cargo_receipt_t chain[2] = { r1, r2 };
    ASSERT(cargo_receipt_chain_verify(chain, 2, cargo_pk) == CARGO_RECEIPT_OK);
    /* Recipient on the first leg is the NPC; on the second leg, Kepler. */
    ASSERT(memcmp(r1.recipient_pubkey, npc_pk, 32) == 0);
    ASSERT(memcmp(r2.recipient_pubkey, kepler->station_pubkey, 32) == 0);

    crs_teardown();
}

/* ---------------- Test 8: save/load preserves receipts -------------- */

TEST(test_cross_station_save_load_preserves_receipts) {
    crs_setup("save_load");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD008);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x80);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xB0);

    station_t *helios = &w->stations[2];
    station_t *kepler = &w->stations[1];

    /* Build a 2-hop chain and attach to ship.manifest + ship.receipts
     * for player slot 0. */
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    ASSERT(ship_manifest_bootstrap(&sp->ship));
    ship_receipts_t *rcpts = ship_get_receipts(&sp->ship);
    ASSERT(rcpts != NULL);

    cargo_unit_t cu = {0};
    cu.kind = CARGO_KIND_INGOT;
    cu.commodity = COMMODITY_FERRITE_INGOT;
    cu.grade = MINING_GRADE_COMMON;
    cu.recipe_id = RECIPE_SMELT;
    cu.prefix_class = INGOT_PREFIX_M;
    memcpy(cu.pub, cargo_pk, 32);
    ASSERT(manifest_push(&sp->ship.manifest, &cu));

    cargo_receipt_t r1, r2;
    ASSERT(crs_first_hop(w, helios, player_pk, cargo_pk, &r1));
    ASSERT(crs_next_hop(w, kepler, player_pk, cargo_pk, &r1, &r2));
    cargo_receipt_t chain[2] = { r1, r2 };
    ASSERT(ship_receipts_push_chain(rcpts, chain, 2));
    /* Parity: receipts.count == manifest.count == 1. */
    ASSERT_EQ_INT((int)rcpts->count, 1);
    ASSERT_EQ_INT((int)sp->ship.manifest.count, 1);

    /* Persist + reload via player_save / player_load. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s_crs_save", TMP("crs_dir"));
    /* Ensure dir exists; player_save creates the file but not a fresh dir hierarchy. */
    /* Use legacy save path (token-keyed) since no pubkey is registered. */
    /* The token must be set so player_save_path picks legacy path. */
    for (int i = 0; i < 8; i++) sp->session_token[i] = (uint8_t)(0x42 + i);
    sp->session_ready = true;

    /* world_save handles the world-side state; player_save persists
     * the ship + manifest + receipts tail. */
    ASSERT(world_save(w, TMP("crs_world.sav")));
    /* player_save's ensure_save_subdirs only creates pubkey/ and
     * legacy/ INSIDE `dir`; it does NOT create `dir` itself. The
     * test_tmp_path scratch root already exists, but our nested
     * directory under it does not — create it before saving. */
    {
#if defined(_WIN32)
        (void)_mkdir(dir);
#else
        (void)mkdir(dir, 0700);
#endif
    }
    ASSERT(player_save(sp, dir, 0));

    /* Fresh load. */
    WORLD_HEAP w2 = calloc(1, sizeof(world_t));
    ASSERT(w2 != NULL);
    ASSERT(world_load(w2, TMP("crs_world.sav")));
    server_player_t sp2 = {0};
    memcpy(sp2.session_token, sp->session_token, 8);
    sp2.session_ready = true;
    ASSERT(player_load_by_token(&sp2, w2, dir, sp->session_token));

    ASSERT_EQ_INT((int)sp2.ship.manifest.count, 1);
    ship_receipts_t *rcpts2 = ship_get_receipts(&sp2.ship);
    ASSERT(rcpts2 != NULL);
    ASSERT_EQ_INT((int)rcpts2->count, 1);
    ASSERT_EQ_INT((int)rcpts2->chains[0].len, 2);
    /* The reloaded chain must still verify end-to-end. */
    ASSERT(cargo_receipt_chain_verify(rcpts2->chains[0].links,
                                      rcpts2->chains[0].len, cargo_pk)
           == CARGO_RECEIPT_OK);

    /* Cleanup. */
    ship_cleanup(&sp2.ship);
    remove(TMP("crs_world.sav"));
    crs_teardown();
}

/* ---------------- Test 9: chain length cap -------------------------- */

TEST(test_cross_station_chain_length_cap) {
    crs_setup("cap");
    WORLD_HEAP w = calloc(1, sizeof(world_t)); ASSERT(w != NULL); crs_world_init(w, 0xD009);
    ASSERT(w != NULL);

    uint8_t player_pk[32]; fill_test_pubkey(player_pk, 0x90);
    uint8_t cargo_pk[32];  fill_test_pubkey(cargo_pk,  0xC0);

    /* Try to build a 17-hop chain. The 17th transfer must be refused
     * by ship_receipts_extend (cap = 16). We check both: the verifier
     * rejects a 17-element chain and ship_receipts_extend refuses to
     * grow beyond 16. */
    cargo_receipt_t chain[CARGO_RECEIPT_CHAIN_MAX_LEN + 1];

    station_t *st = &w->stations[2];
    ASSERT(crs_first_hop(w, st, player_pk, cargo_pk, &chain[0]));
    for (int i = 1; i <= CARGO_RECEIPT_CHAIN_MAX_LEN; i++) {
        /* Alternate stations 1 and 2 for each subsequent hop so each
         * hop is signed by a real keyed station. */
        station_t *next = &w->stations[(i % 2 == 0) ? 2 : 1];
        ASSERT(crs_next_hop(w, next, player_pk, cargo_pk,
                            &chain[i - 1], &chain[i]));
    }
    /* The 17-element chain trips the TOO_LONG cap in chain_verify. */
    ASSERT(cargo_receipt_chain_verify(chain, CARGO_RECEIPT_CHAIN_MAX_LEN + 1,
                                      cargo_pk)
           == CARGO_RECEIPT_REJECT_TOO_LONG);
    /* The 16-element prefix verifies. */
    ASSERT(cargo_receipt_chain_verify(chain, CARGO_RECEIPT_CHAIN_MAX_LEN,
                                      cargo_pk)
           == CARGO_RECEIPT_OK);

    /* ship_receipts_extend refuses past the cap. */
    ship_receipts_t r = {0};
    ASSERT(ship_receipts_init(&r, 4));
    ASSERT(ship_receipts_push_chain(&r, chain, CARGO_RECEIPT_CHAIN_MAX_LEN));
    /* Attempting to extend a CHAIN_MAX_LEN-deep chain by one more must fail. */
    ASSERT(!ship_receipts_extend(&r, 0, &chain[CARGO_RECEIPT_CHAIN_MAX_LEN]));
    ship_receipts_free(&r);

    crs_teardown();
}

void register_cross_station_settlement_tests(void);
void register_cross_station_settlement_tests(void) {
    TEST_SECTION("\n--- Cross-Station Settlement (#479 D) ---\n");
    RUN(test_cross_station_single_hop_receipt);
    RUN(test_cross_station_two_hop_chain);
    RUN(test_cross_station_forged_receipt_rejected);
    RUN(test_cross_station_tampered_cargo_pub_rejected);
    RUN(test_cross_station_broken_linkage_rejected);
    RUN(test_cross_station_three_hop_chain);
    RUN(test_cross_station_npc_mediated_transfer);
    RUN(test_cross_station_save_load_preserves_receipts);
    RUN(test_cross_station_chain_length_cap);
}
