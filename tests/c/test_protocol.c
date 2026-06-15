#include "test_harness.h"
#include "cargo_receipt_issue.h"

TEST(test_roundtrip_player_state) {
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.ship.pos = v2(123.45f, -678.9f);
    sp.ship.vel = v2(1.5f, -2.5f);
    sp.ship.angle = 2.34f;
    sp.docked = true;
    sp.actual_thrusting = true;
    sp.beam_active = true;
    sp.beam_hit = true;

    uint8_t buf[64];
    int len = serialize_player_state(buf, 7, &sp);

    /* Size must be 45 (widened towed_frags uint8→uint16 in #285 Phase 3) */
    ASSERT_EQ_INT(len, 45);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATE);
    ASSERT_EQ_INT(buf[1], 7);

    /* Verify floats roundtrip */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 123.45f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[6]), -678.9f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[10]), 1.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[14]), -2.5f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[18]), 2.34f, 0.01f);

    /* Verify flags byte */
    uint8_t flags = buf[22];
    ASSERT(flags & 1);   /* thrusting */
    ASSERT(flags & 2);   /* beam active + hit */
    ASSERT(flags & 4);   /* docked */
}

TEST(test_roundtrip_batched_player_states) {
    server_player_t players[MAX_PLAYERS];
    memset(players, 0, sizeof(players));

    /* Two connected players */
    players[0].connected = true;
    players[0].ship.pos = v2(100.0f, 200.0f);
    players[0].ship.vel = v2(1.0f, -1.0f);
    players[0].ship.angle = 1.5f;
    players[0].actual_thrusting = true;
    players[0].docked = false;
    players[0].last_input_seq = 321;
    players[0].last_input_tick = 12340u;

    players[3].connected = true;
    players[3].ship.pos = v2(-50.0f, 300.0f);
    players[3].ship.vel = v2(0.0f, 2.0f);
    players[3].ship.angle = 3.14f;
    players[3].docked = true;
    players[3].ship.tractor_active = true;
    players[3].ship.tractor_level = 2;
    players[3].ship.towed_count = 2;
    players[3].ship.towed_fragments[0] = 301;
    players[3].ship.towed_fragments[1] = 1024;

    uint8_t buf[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int len = serialize_all_player_states(buf, players, 12345u);

    /* Should have 2 records */
    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(len, 2 + 2 * PLAYER_RECORD_SIZE);

    /* First record: player 0 */
    uint8_t *p0 = &buf[2];
    ASSERT_EQ_INT(p0[0], 0);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[1]), 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[5]), 200.0f, 0.01f);
    ASSERT(p0[21] & 1); /* thrusting */
    ASSERT(!(p0[21] & 4)); /* not docked */
    ASSERT_EQ_INT((int)((uint16_t)p0[67] | ((uint16_t)p0[68] << 8)), 321);
    ASSERT_EQ_INT((int)read_u32_le(&p0[69]), 12345);
    ASSERT_EQ_INT((int)read_u32_le(&p0[73]), 12340);

    /* Second record: player 3 */
    uint8_t *p1 = &buf[2 + PLAYER_RECORD_SIZE];
    ASSERT_EQ_INT(p1[0], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&p1[1]), -50.0f, 0.01f);
    ASSERT(p1[21] & 4); /* docked */
    ASSERT(p1[21] & 16); /* tractor active */
    ASSERT_EQ_INT(p1[22], 2);
    ASSERT_EQ_INT(p1[23], 2);
    ASSERT_EQ_INT((int)((uint16_t)p1[24] | ((uint16_t)p1[25] << 8)), 301);
    ASSERT_EQ_INT((int)((uint16_t)p1[26] | ((uint16_t)p1[27] << 8)), 1024);
}

TEST(test_roundtrip_asteroids) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));

    /* Set up 3 active asteroids with different properties */
    asteroids[0].active = true;
    asteroids[0].net_dirty = true;
    asteroids[0].fracture_child = false;
    asteroids[0].tier = ASTEROID_TIER_XL;
    asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    asteroids[0].pos = v2(500.0f, -300.0f);
    asteroids[0].vel = v2(1.0f, -1.0f);
    asteroids[0].hp = 150.0f;
    asteroids[0].ore = 0.0f;
    asteroids[0].radius = 65.0f;

    asteroids[5].active = true;
    asteroids[5].net_dirty = true;
    asteroids[5].fracture_child = true;
    asteroids[5].tier = ASTEROID_TIER_S;
    asteroids[5].commodity = COMMODITY_CRYSTAL_ORE;
    asteroids[5].pos = v2(-100.0f, 200.0f);
    asteroids[5].vel = v2(-3.0f, 0.5f);
    asteroids[5].hp = 12.0f;
    asteroids[5].ore = 10.5f;
    asteroids[5].radius = 14.0f;
    asteroids[5].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[5].phase = ASTEROID_PHASE_GAS_RICH;

    uint8_t buf[ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
    bool sent[MAX_ASTEROIDS] = {0};
    vec2 view_pos = v2(0.0f, 0.0f); /* both asteroids are within 3000u */
    int len = serialize_asteroids_for_player(buf, asteroids, view_pos, sent);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    ASSERT_EQ_INT(buf[1] | (buf[2] << 8), 2);  /* 2 visible asteroids (uint16 count) */
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + 2 * ASTEROID_RECORD_SIZE);

    /* First asteroid (index 0) */
    uint8_t *p0 = &buf[ASTEROID_MSG_HEADER];
    ASSERT_EQ_INT(p0[0] | (p0[1] << 8), 0);  /* uint16 index */
    ASSERT(p0[2] & 1);         /* active */
    ASSERT(!(p0[2] & 2));      /* not fracture_child */
    ASSERT_EQ_INT((p0[2] >> 2) & 0x7, ASTEROID_TIER_XL);
    ASSERT_EQ_INT((p0[2] >> 5) & 0x7, COMMODITY_FERRITE_ORE);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[3]), 500.0f, 0.1f);   /* pos.x */
    ASSERT_EQ_FLOAT(read_f32_le(&p0[19]), 150.0f, 0.1f);  /* hp */

    /* Second asteroid (index 5) */
    uint8_t *p1 = &buf[ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE];
    ASSERT_EQ_INT(p1[0] | (p1[1] << 8), 5);  /* uint16 index */
    ASSERT(p1[2] & 1);         /* active */
    ASSERT(p1[2] & 2);         /* fracture_child */
    ASSERT_EQ_INT((p1[2] >> 2) & 0x7, ASTEROID_TIER_S);
    ASSERT_EQ_INT((p1[2] >> 5) & 0x7, COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_FLOAT(read_f32_le(&p1[23]), 10.5f, 0.1f);  /* ore */
    ASSERT_EQ_FLOAT(read_f32_le(&p1[27]), 14.0f, 0.1f);  /* radius */
    ASSERT_EQ_INT(p1[33], CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(p1[34], ASTEROID_PHASE_GAS_RICH);
}

TEST(test_roundtrip_asteroids_full_skips_inactive_slots) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));

    /* Join-time full sync is active-only; the client clears local asteroid
     * buffers before entering remote-authoritative mode. */
    asteroids[0].active = true;
    asteroids[0].tier = ASTEROID_TIER_L;
    asteroids[0].commodity = COMMODITY_CUPRITE_ORE;
    asteroids[0].pos = v2(42.0f, -9.0f);
    asteroids[0].hp = 77.0f;
    asteroids[0].radius = 33.0f;

    asteroids[5].active = true;
    asteroids[5].fracture_child = true;
    asteroids[5].tier = ASTEROID_TIER_M;
    asteroids[5].commodity = COMMODITY_CRYSTAL_ORE;
    asteroids[5].pos = v2(-12.0f, 88.0f);
    asteroids[5].ore = 11.0f;
    asteroids[5].radius = 21.0f;
    asteroids[5].crystal_stage = CRYSTAL_STAGE_INTERMEDIATE;
    asteroids[5].phase = ASTEROID_PHASE_GAS_RICH;

    uint8_t *buf = calloc(1, ASTEROID_MSG_HEADER + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE);
    int len = serialize_asteroids_full(buf, asteroids);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_ASTEROIDS);
    int full_count = buf[1] | (buf[2] << 8);
    ASSERT_EQ_INT(full_count, 2);  /* only active slots sent */
    ASSERT_EQ_INT(len, ASTEROID_MSG_HEADER + 2 * ASTEROID_RECORD_SIZE);

    /* First active slot (index 0) */
    uint8_t *p0 = &buf[ASTEROID_MSG_HEADER];
    ASSERT_EQ_INT(p0[0] | (p0[1] << 8), 0);
    ASSERT(p0[2] & 1);
    ASSERT_EQ_INT((p0[2] >> 2) & 0x7, ASTEROID_TIER_L);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[3]), 42.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p0[19]), 77.0f, 0.1f);

    /* Inactive slots are skipped in full snapshot (too many at 2048).
     * Second record should be the other active slot (index 5). */
    uint8_t *p5 = &buf[ASTEROID_MSG_HEADER + ASTEROID_RECORD_SIZE];
    ASSERT_EQ_INT(p5[0] | (p5[1] << 8), 5);
    ASSERT(p5[2] & 1);
    ASSERT(p5[2] & 2);
    ASSERT_EQ_INT((p5[2] >> 2) & 0x7, ASTEROID_TIER_M);
    ASSERT_EQ_FLOAT(read_f32_le(&p5[23]), 11.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p5[27]), 21.0f, 0.1f);
    ASSERT_EQ_INT(p5[33], CRYSTAL_STAGE_INTERMEDIATE);
    ASSERT_EQ_INT(p5[34], ASTEROID_PHASE_GAS_RICH);
    free(buf);
}

TEST(test_roundtrip_cargo_pods) {
    cargo_pod_t pods[MAX_CARGO_PODS];
    memset(pods, 0, sizeof(pods));
    pods[3].active = true;
    pods[3].kind = CARGO_POD_CARGO;
    pods[3].commodity = COMMODITY_REPAIR_KIT;
    pods[3].quantity = 20;
    pods[3].pos = v2(123.0f, -45.0f);
    pods[3].vel = v2(1.5f, -2.0f);
    pods[3].radius = 18.0f;
    pods[3].rotation = 0.75f;
    pods[3].towed_by = 2;

    uint8_t buf[2 + MAX_CARGO_PODS * CARGO_POD_RECORD_SIZE];
    int len = serialize_cargo_pods(buf, pods);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_CARGO_PODS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + CARGO_POD_RECORD_SIZE);
    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 3);
    ASSERT_EQ_INT(p[1], CARGO_POD_CARGO);
    ASSERT_EQ_INT(p[2], COMMODITY_REPAIR_KIT);
    ASSERT_EQ_INT(p[3], 2);
    ASSERT_EQ_FLOAT(read_f32_le(&p[4]), 123.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[8]), -45.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[20]), 18.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[24]), 0.75f, 0.01f);
    ASSERT_EQ_INT(read_u16_le(&p[28]), 20);
}

TEST(test_roundtrip_npcs) {
    npc_ship_t npcs[MAX_NPC_SHIPS];
    memset(npcs, 0, sizeof(npcs));

    npcs[0].active = true;
    npcs[0].role = NPC_ROLE_MINER;
    npcs[0].state = NPC_STATE_MINING;
    npcs[0].thrusting = true;
    npcs[0].ship.pos = v2(800.0f, 400.0f);
    npcs[0].ship.vel = v2(10.0f, -5.0f);
    npcs[0].ship.angle = 1.57f;
    npcs[0].target_asteroid = 512;
    npcs[0].towed_fragment = 1024;
    npcs[0].home_station = 2;
    memcpy(npcs[0].session_token, "NPC\002\000\005\064\022", 8);

    npcs[0].tint_r = 0.55f;
    npcs[0].tint_g = 0.25f;
    npcs[0].tint_b = 0.18f;

    uint8_t buf[2 + MAX_NPC_SHIPS * NPC_RECORD_SIZE];
    int len = serialize_npcs(buf, npcs);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + NPC_RECORD_SIZE);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    ASSERT(p[1] & 1);                              /* active */
    ASSERT_EQ_INT((p[1] >> 1) & 0x3, NPC_ROLE_MINER);
    ASSERT_EQ_INT((p[1] >> 3) & 0x7, NPC_STATE_MINING);
    ASSERT(p[1] & (1 << 6));                        /* thrusting */
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 800.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[18]), 1.57f, 0.01f);
    ASSERT_EQ_INT(read_u16_le(&p[22]), 512);       /* target_asteroid */
    ASSERT_EQ_INT(read_u16_le(&p[24]), 1024);      /* towed_fragment */
    ASSERT_EQ_INT(p[26], (int)(0.55f * 255.0f));
    ASSERT(memcmp(&p[29], npcs[0].session_token, 8) == 0);
    ASSERT_EQ_INT(p[37], 2);
}

TEST(test_npc_role_default_hull_mapping_covers_tow) {
    ASSERT_EQ_INT(npc_default_hull_class_for_role(NPC_ROLE_MINER),
                  HULL_CLASS_NPC_MINER);
    ASSERT_EQ_INT(npc_default_hull_class_for_role(NPC_ROLE_HAULER),
                  HULL_CLASS_HAULER);
    ASSERT_EQ_INT(npc_default_hull_class_for_role(NPC_ROLE_TOW),
                  HULL_CLASS_DRONE_TRACTOR);
}

TEST(test_roundtrip_inspect_snapshot_npc_manifest_chain) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[31] = 0x42;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_pub, 7, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_H;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 2;
    memcpy(chain.links[0].cargo_pub, unit.pub, 32);
    memcpy(chain.links[1].cargo_pub, unit.pub, 32);
    memset(chain.links[0].authoring_station, 0xA1, 32);
    memset(chain.links[1].authoring_station, 0xB2, 32);
    chain.links[0].event_id = 7001;
    chain.links[1].event_id = 7002;
    ASSERT(ship_manifest_push_with_chain(&ship, &unit, &chain));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[2], 3);
    ASSERT_EQ_INT(buf[3], 0xFF);
    ASSERT_EQ_INT(buf[4], NPC_ROLE_HAULER);
    ASSERT_EQ_INT(buf[5], NPC_STATE_TRAVEL_TO_DEST);
    ASSERT_EQ_INT(buf[6], 0);
    ASSERT_EQ_INT(buf[7], 1);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 1);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(p[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[1], MINING_GRADE_RARE);
    ASSERT_EQ_INT(p[2], 2);
    ASSERT(p[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT(read_u16_le(&p[12]), 1);
    uint64_t event_id = 0;
    for (int i = 0; i < 8; i++) event_id |= ((uint64_t)p[4 + i]) << (8 * i);
    ASSERT_EQ_INT((int)event_id, 7002);
    ASSERT(memcmp(&p[14], unit.pub, 32) == 0);
    uint8_t expected_head[32];
    cargo_receipt_hash(&chain.links[1], expected_head);
    ASSERT(memcmp(&p[46], expected_head, 32) == 0);
    ASSERT(memcmp(&p[78], chain.links[0].authoring_station, 32) == 0);
    ASSERT(memcmp(&p[110], chain.links[1].authoring_station, 32) == 0);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_expands_matching_receipt_chain) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[31] = 0x67;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_pub, 9, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_H;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 2;
    memcpy(chain.links[0].cargo_pub, unit.pub, 32);
    memcpy(chain.links[1].cargo_pub, unit.pub, 32);
    memset(chain.links[0].authoring_station, 0xA4, 32);
    memset(chain.links[1].authoring_station, 0xB5, 32);
    memset(chain.links[0].recipient_pubkey, 0xC6, 32);
    memset(chain.links[1].recipient_pubkey, 0xD7, 32);
    chain.links[0].event_id = 7101;
    chain.links[1].event_id = 7102;
    ASSERT(ship_manifest_push_with_chain(&ship, &unit, &chain));

    uint8_t expected_head[32];
    cargo_receipt_hash(&chain.links[1], expected_head);
    npc.job_diag_count = 1;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 190;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_FERRITE_INGOT;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_RECEIPT_PROOF;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    memcpy(npc.job_diag_proof_hash[0], expected_head, 32);

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 4);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 1);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 4 * INSPECT_SNAPSHOT_ROW);

    uint8_t *job = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(job[0], INSPECT_DIAG_JOB_HAUL);
    ASSERT(job[3] & INSPECT_ROW_DIAGNOSTIC);

    uint8_t *receipt = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(receipt[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(receipt[1], MINING_GRADE_RARE);
    ASSERT_EQ_INT(receipt[2], 2);
    ASSERT(receipt[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT(!(receipt[3] & INSPECT_ROW_DIAGNOSTIC));
    ASSERT_EQ_INT(read_u16_le(&receipt[12]), 1);
    ASSERT(memcmp(&receipt[14], unit.pub, 32) == 0);
    ASSERT(memcmp(&receipt[46], expected_head, 32) == 0);
    ASSERT(memcmp(&receipt[78], chain.links[0].authoring_station, 32) == 0);
    ASSERT(memcmp(&receipt[110], chain.links[1].authoring_station, 32) == 0);

    uint8_t *link0 = receipt + INSPECT_SNAPSHOT_ROW;
    ASSERT_EQ_INT(link0[0], INSPECT_DIAG_RECEIPT_LINK);
    ASSERT_EQ_INT(link0[1], 1);
    ASSERT_EQ_INT(link0[2], 2);
    ASSERT(link0[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(link0[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT((int)read_u64_le(&link0[4]), 7101);
    ASSERT_EQ_INT(read_u16_le(&link0[12]), 1);
    ASSERT(memcmp(&link0[14], unit.pub, 32) == 0);
    uint8_t link0_hash[32];
    cargo_receipt_hash(&chain.links[0], link0_hash);
    ASSERT(memcmp(&link0[46], link0_hash, 32) == 0);
    ASSERT(memcmp(&link0[78], chain.links[0].authoring_station, 32) == 0);
    ASSERT(memcmp(&link0[110], chain.links[0].recipient_pubkey, 32) == 0);

    uint8_t *link1 = link0 + INSPECT_SNAPSHOT_ROW;
    ASSERT_EQ_INT(link1[0], INSPECT_DIAG_RECEIPT_LINK);
    ASSERT_EQ_INT(link1[1], 2);
    ASSERT_EQ_INT(link1[2], 2);
    ASSERT(link1[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(link1[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT((int)read_u64_le(&link1[4]), 7102);
    ASSERT_EQ_INT(read_u16_le(&link1[12]), 2);
    ASSERT(memcmp(&link1[14], unit.pub, 32) == 0);
    ASSERT(memcmp(&link1[46], expected_head, 32) == 0);
    ASSERT(memcmp(&link1[78], chain.links[1].authoring_station, 32) == 0);
    ASSERT(memcmp(&link1[110], chain.links[1].recipient_pubkey, 32) == 0);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_retrieves_matching_station_receipt_chain) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    WORLD_DECL;
    world_reset(&w);

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[31] = 0x91;
    ASSERT(hash_ingot(COMMODITY_CUPRITE_INGOT, MINING_GRADE_COMMON,
                      fragment_pub, 14, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_K;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    uint8_t recipient[32];
    uint8_t origin_pin[32];
    for (int i = 0; i < 32; i++) {
        recipient[i] = (uint8_t)(0x53 + i);
        origin_pin[i] = (uint8_t)(0x91 + i);
    }
    ASSERT(cargo_receipt_issue(&w.stations[1], 1, 7201, unit.pub,
                               recipient, origin_pin, &chain.links[0]));
    chain.len = 1;
    ASSERT(cargo_receipt_chain_verify(chain.links, chain.len, unit.pub) ==
           CARGO_RECEIPT_OK);
    ASSERT(station_manifest_push_with_chain(&w.stations[1], &unit, &chain));

    uint8_t expected_head[32];
    cargo_receipt_hash(&chain.links[0], expected_head);
    npc.job_diag_count = 1;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 190;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_CUPRITE_INGOT;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_RECEIPT_PROOF;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    memcpy(npc.job_diag_proof_hash[0], expected_head, 32);

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc_with_station_receipts(
        buf, 3, &npc, &ship, w.stations, MAX_STATIONS);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *job = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(job[0], INSPECT_DIAG_JOB_HAUL);
    ASSERT(job[3] & INSPECT_ROW_DIAGNOSTIC);

    uint8_t *receipt = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(receipt[0], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(receipt[1], MINING_GRADE_COMMON);
    ASSERT_EQ_INT(receipt[2], 1);
    ASSERT(receipt[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT(receipt[3] & INSPECT_ROW_STATION_RECEIPT);
    ASSERT(!(receipt[3] & INSPECT_ROW_DIAGNOSTIC));
    ASSERT(memcmp(&receipt[14], unit.pub, 32) == 0);
    ASSERT(memcmp(&receipt[46], expected_head, 32) == 0);

    uint8_t *link0 = receipt + INSPECT_SNAPSHOT_ROW;
    ASSERT_EQ_INT(link0[0], INSPECT_DIAG_RECEIPT_LINK);
    ASSERT_EQ_INT(link0[1], 1);
    ASSERT_EQ_INT(link0[2], 1);
    ASSERT(link0[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(link0[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT((int)read_u64_le(&link0[4]), 7201);

    ship_cleanup(&ship);
}

TEST(test_roundtrip_inspect_snapshot_player_manifest_chain) {
    server_player_t player;
    memset(&player, 0, sizeof(player));
    player.connected = true;
    player.current_station = 2;
    player.nearby_station = 1;
    player.ship.hull_class = HULL_CLASS_HAULER;
    player.ship.hull = 149.6f;
    ASSERT(ship_manifest_bootstrap(&player.ship));

    cargo_unit_t unit;
    memset(&unit, 0, sizeof(unit));
    uint8_t fragment_pub[32] = {0};
    fragment_pub[0] = 0x9A;
    ASSERT(hash_ingot(COMMODITY_CUPRITE_INGOT, MINING_GRADE_RATI,
                      fragment_pub, 11, &unit));
    unit.prefix_class = (uint8_t)INGOT_PREFIX_M;

    cargo_receipt_chain_t chain;
    memset(&chain, 0, sizeof(chain));
    chain.len = 1;
    memcpy(chain.links[0].cargo_pub, unit.pub, 32);
    memset(chain.links[0].authoring_station, 0xC3, 32);
    chain.links[0].event_id = 8001;
    ASSERT(ship_manifest_push_with_chain(&player.ship, &unit, &chain));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_player(buf, 5, &player);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_PLAYER);
    ASSERT_EQ_INT(buf[2], 5);
    ASSERT_EQ_INT(buf[3], 0xFF);
    ASSERT_EQ_INT(buf[4], HULL_CLASS_HAULER);
    ASSERT_EQ_INT(buf[5], 150);
    ASSERT_EQ_INT(buf[6], 2);
    ASSERT_EQ_INT(buf[7], 1);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 1);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *p = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(p[0], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(p[1], MINING_GRADE_RATI);
    ASSERT_EQ_INT(p[2], 1);
    ASSERT(p[3] & INSPECT_ROW_HAS_RECEIPT);
    ASSERT_EQ_INT(read_u16_le(&p[12]), 1);
    ASSERT(memcmp(&p[14], unit.pub, 32) == 0);

    ship_cleanup(&player.ship);
}

TEST(test_inspect_snapshot_npc_includes_market_memory_diagnostics) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;
    npc.knowledge.capacity = SHIP_KNOWN_ITEM_CAP;
    npc.knowledge.count = 1;

    market_memory_t memory;
    memset(&memory, 0, sizeof(memory));
    memory.active = true;
    memory.memory_kind = (uint8_t)MARKET_MEMORY_DELIVERY_RECEIPT;
    memory.station_a = 3;
    memory.station_b = 1;
    memory.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    memory.action = (uint8_t)CONTRACT_DELIVERY;
    memory.confidence = 210;
    memory.salience = 180;
    memory.quantity_hint = 2;
    memory.value_hint = 77;

    knowledge_item_t *item = &npc.knowledge.items[0];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = memory.confidence;
    item->salience = memory.salience;
    for (int b = 0; b < 32; b++) {
        item->subject_hash[b] = (uint8_t)(0x10 + b);
        item->chain_anchor[b] = (uint8_t)(0x40 + b);
        item->source_hash[b] = (uint8_t)(0x70 + b);
        item->witness_hash[b] = (uint8_t)(0xA0 + b);
    }
    memcpy(item->payload, &memory, sizeof(memory));

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 1);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW);

    uint8_t *row = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(row[0], INSPECT_DIAG_DELIVERY_RECEIPT);
    ASSERT_EQ_INT(row[1], 210);
    ASSERT_EQ_INT(row[2], 180);
    ASSERT(row[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT(!(row[3] & INSPECT_ROW_GROUPED));
    ASSERT_EQ_INT(read_u16_le(&row[12]), 77);
    ASSERT_EQ_INT(row[4], 3);
    ASSERT_EQ_INT(row[5], 1);
    ASSERT_EQ_INT(row[6], CONTRACT_DELIVERY);
    ASSERT_EQ_INT(row[7], COMMODITY_FERRITE_INGOT);
    for (int b = 0; b < 32; b++) {
        ASSERT_EQ_INT(row[14 + b], (uint8_t)(0x10 + b));
        ASSERT_EQ_INT(row[46 + b], (uint8_t)(0x40 + b));
        ASSERT_EQ_INT(row[78 + b], (uint8_t)(0x70 + b));
        ASSERT_EQ_INT(row[110 + b], (uint8_t)(0xA0 + b));
    }

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_expands_matching_job_source_memory) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;
    npc.job_diag_count = 1;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 180;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_FERRITE_INGOT;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_ROUTE_MEMORY;
    npc.job_diag_memory_kind[0] = (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    for (int b = 0; b < 32; b++)
        npc.job_diag_proof_hash[0][b] = (uint8_t)(0xC0 + b);

    npc.knowledge.capacity = SHIP_KNOWN_ITEM_CAP;
    npc.knowledge.count = 2;
    market_memory_t first;
    memset(&first, 0, sizeof(first));
    first.active = true;
    first.memory_kind = (uint8_t)MARKET_MEMORY_DEMAND;
    first.station_a = 2;
    first.station_b = 0xffu;
    first.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    first.action = (uint8_t)CONTRACT_TRACTOR;
    first.confidence = 120;
    first.salience = 90;
    first.value_hint = 11;
    knowledge_item_t *item = &npc.knowledge.items[0];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = first.confidence;
    item->salience = first.salience;
    memcpy(item->payload, &first, sizeof(first));

    market_memory_t route;
    memset(&route, 0, sizeof(route));
    route.active = true;
    route.memory_kind = (uint8_t)MARKET_MEMORY_ROUTE_SUCCESS;
    route.station_a = 1;
    route.station_b = 0;
    route.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    route.action = (uint8_t)CONTRACT_TRACTOR;
    route.confidence = 230;
    route.salience = 210;
    route.quantity_hint = 3;
    route.value_hint = 88;
    item = &npc.knowledge.items[1];
    memset(item, 0, sizeof(*item));
    item->kind = (uint8_t)KNOW_MARKET;
    item->payload_kind = (uint8_t)KNOW_PAYLOAD_MARKET_MEMORY;
    item->confidence = route.confidence;
    item->salience = route.salience;
    for (int b = 0; b < 32; b++) {
        item->subject_hash[b] = (uint8_t)(0x20 + b);
        item->chain_anchor[b] = (uint8_t)(0xC0 + b);
        item->source_hash[b] = (uint8_t)(0x60 + b);
        item->witness_hash[b] = (uint8_t)(0x90 + b);
    }
    memcpy(item->payload, &route, sizeof(route));

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *job = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(job[0], INSPECT_DIAG_JOB_HAUL);

    uint8_t *source = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(source[0], INSPECT_DIAG_ROUTE_SUCCESS);
    ASSERT_EQ_INT(source[1], 230);
    ASSERT_EQ_INT(source[2], 210);
    ASSERT_EQ_INT(source[4], 1);
    ASSERT_EQ_INT(source[5], 0);
    ASSERT_EQ_INT(source[6], CONTRACT_TRACTOR);
    ASSERT_EQ_INT(source[7], COMMODITY_FERRITE_INGOT);
    for (int b = 0; b < 32; b++) {
        ASSERT_EQ_INT(source[14 + b], (uint8_t)(0x20 + b));
        ASSERT_EQ_INT(source[46 + b], (uint8_t)(0xC0 + b));
        ASSERT_EQ_INT(source[78 + b], (uint8_t)(0x60 + b));
        ASSERT_EQ_INT(source[110 + b], (uint8_t)(0x90 + b));
    }

    uint8_t *general = &buf[INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(general[0], INSPECT_DIAG_MARKET_DEMAND);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_npc_includes_job_offer_diagnostics) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_DOCKED;
    npc.home_station = 0;
    npc.dest_station = 1;
    npc.job_diag_count = 2;
    npc.job_diag_kind[0] = (uint8_t)INSPECT_DIAG_JOB_HAUL;
    npc.job_diag_score[0] = 212;
    npc.job_diag_selected[0] = 255;
    npc.job_diag_source[0] = 0;
    npc.job_diag_dest[0] = 1;
    npc.job_diag_commodity[0] = (uint8_t)COMMODITY_FERRITE_INGOT;
    npc.job_diag_hint[0] = 25;
    npc.job_diag_factor_value[0] = 201;
    npc.job_diag_factor_demand[0] = 202;
    npc.job_diag_factor_supply[0] = 203;
    npc.job_diag_factor_route[0] = 204;
    npc.job_diag_factor_freshness[0] = 205;
    npc.job_diag_factor_capability[0] = 206;
    npc.job_diag_factor_proof[0] = 207;
    npc.job_diag_factor_hologram[0] = 208;
    npc.job_diag_reason[0] = (uint8_t)INSPECT_JOB_REASON_REMOTE_SUPPLY;
    npc.job_diag_memory_kind[0] = (uint8_t)MARKET_MEMORY_SUPPLY;
    npc.job_diag_memory_hops[0] = 3;
    npc.job_diag_memory_age[0] = 12;
    npc.job_diag_memory_station[0] = 1;
    npc.job_diag_proof_kind[0] = (uint8_t)INSPECT_JOB_PROOF_CHAIN_ANCHOR;
    npc.job_diag_proof_prefix[0][0] = 0xA1;
    npc.job_diag_proof_prefix[0][1] = 0xB2;
    npc.job_diag_proof_prefix[0][2] = 0xC3;
    npc.job_diag_proof_prefix[0][3] = 0xD4;
    for (int b = 0; b < 32; b++)
        npc.job_diag_proof_hash[0][b] = (uint8_t)(0x80 + b);
    npc.job_diag_kind[1] = (uint8_t)INSPECT_DIAG_JOB_MINE;
    npc.job_diag_score[1] = 118;
    npc.job_diag_selected[1] = 96;
    npc.job_diag_source[1] = 0;
    npc.job_diag_dest[1] = 0;
    npc.job_diag_commodity[1] = (uint8_t)COMMODITY_FERRITE_ORE;
    npc.job_diag_hint[1] = 6;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[0], NET_MSG_INSPECT_SNAPSHOT);
    ASSERT_EQ_INT(buf[1], INSPECT_TARGET_NPC);
    ASSERT_EQ_INT(buf[8], 2);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 0);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW);

    uint8_t *haul = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(haul[0], INSPECT_DIAG_JOB_HAUL);
    ASSERT_EQ_INT(haul[1], 212);
    ASSERT_EQ_INT(haul[2], 255);
    ASSERT(haul[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT_EQ_INT(read_u16_le(&haul[12]), 25);
    ASSERT_EQ_INT(haul[4], 0);
    ASSERT_EQ_INT(haul[5], 1);
    ASSERT_EQ_INT(haul[6], INSPECT_DIAG_JOB_HAUL);
    ASSERT_EQ_INT(haul[7], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_VALUE], 201);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_DEMAND], 202);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_SUPPLY], 203);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_ROUTE], 204);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_FRESHNESS], 205);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_CAPABILITY], 206);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_PROOF], 207);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_FACTOR_HOLOGRAM], 208);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_REASON],
                  INSPECT_JOB_REASON_REMOTE_SUPPLY);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_MEMORY_KIND],
                  MARKET_MEMORY_SUPPLY);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_HOPS], 3);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_AGE], 12);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_SOURCE_STATION], 1);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF_KIND],
                  INSPECT_JOB_PROOF_CHAIN_ANCHOR);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF0], 0xA1);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF1], 0xB2);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF2], 0xC3);
    ASSERT_EQ_INT(haul[14 + INSPECT_JOB_META_PROOF3], 0xD4);
    for (int b = 0; b < 32; b++)
        ASSERT_EQ_INT(haul[46 + b], (uint8_t)(0x80 + b));

    uint8_t *mine = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(mine[0], INSPECT_DIAG_JOB_MINE);
    ASSERT_EQ_INT(mine[1], 118);
    ASSERT_EQ_INT(mine[2], 96);
    ASSERT(mine[3] & INSPECT_ROW_DIAGNOSTIC);
    ASSERT_EQ_INT(read_u16_le(&mine[12]), 6);
    ASSERT_EQ_INT(mine[4], 0);
    ASSERT_EQ_INT(mine[5], 0);
    ASSERT_EQ_INT(mine[6], INSPECT_DIAG_JOB_MINE);
    ASSERT_EQ_INT(mine[7], COMMODITY_FERRITE_ORE);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_groups_anonymous_ingots_by_grade) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t fragment_pub[32] = {0};
    for (int i = 0; i < 3; i++) {
        cargo_unit_t u;
        memset(&u, 0, sizeof(u));
        fragment_pub[31] = (uint8_t)(0x10 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                          fragment_pub, (uint16_t)i, &u));
        u.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
        ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
    }

    cargo_unit_t named;
    memset(&named, 0, sizeof(named));
    fragment_pub[31] = 0x40;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                      fragment_pub, 9, &named));
    named.prefix_class = (uint8_t)INGOT_PREFIX_H;
    ASSERT(ship_manifest_push_with_chain(&ship, &named, NULL));

    for (int i = 0; i < 2; i++) {
        cargo_unit_t u;
        memset(&u, 0, sizeof(u));
        fragment_pub[31] = (uint8_t)(0x70 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                          fragment_pub, (uint16_t)i, &u));
        u.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
        ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
    }

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 6);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *bulk_common = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(bulk_common[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(bulk_common[1], MINING_GRADE_COMMON);
    ASSERT(bulk_common[3] & INSPECT_ROW_GROUPED);
    ASSERT(!(bulk_common[3] & INSPECT_ROW_HAS_RECEIPT));
    ASSERT_EQ_INT(read_u16_le(&bulk_common[12]), 3);

    uint8_t *named_common = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(named_common[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(named_common[1], MINING_GRADE_COMMON);
    ASSERT(!(named_common[3] & INSPECT_ROW_GROUPED));
    ASSERT_EQ_INT(read_u16_le(&named_common[12]), 1);
    ASSERT(memcmp(&named_common[14], named.pub, 32) == 0);

    uint8_t *bulk_rare = &buf[INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(bulk_rare[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(bulk_rare[1], MINING_GRADE_RARE);
    ASSERT(bulk_rare[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&bulk_rare[12]), 2);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_groups_finished_goods_by_grade) {
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    const struct {
        cargo_kind_t kind;
        commodity_t commodity;
        int count;
    } buckets[] = {
        { CARGO_KIND_FRAME,   COMMODITY_FRAME,          4 },
        { CARGO_KIND_LASER,   COMMODITY_LASER_MODULE,   2 },
        { CARGO_KIND_TRACTOR, COMMODITY_TRACTOR_MODULE, 3 },
    };
    uint8_t pub_seed = 0x30;
    for (int b = 0; b < 3; b++) {
        for (int i = 0; i < buckets[b].count; i++) {
            cargo_unit_t u;
            memset(&u, 0, sizeof(u));
            u.kind = (uint8_t)buckets[b].kind;
            u.commodity = (uint8_t)buckets[b].commodity;
            u.grade = (uint8_t)MINING_GRADE_FINE;
            u.prefix_class = (uint8_t)INGOT_PREFIX_ANONYMOUS;
            u.quantity = 1;
            u.pub[31] = pub_seed++;
            ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
        }
    }

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 3);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 9);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW);

    uint8_t *frames = &buf[INSPECT_SNAPSHOT_HEADER];
    ASSERT_EQ_INT(frames[0], COMMODITY_FRAME);
    ASSERT_EQ_INT(frames[1], MINING_GRADE_FINE);
    ASSERT(frames[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&frames[12]), 4);

    uint8_t *lasers = &buf[INSPECT_SNAPSHOT_HEADER + INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(lasers[0], COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(lasers[1], MINING_GRADE_FINE);
    ASSERT(lasers[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&lasers[12]), 2);

    uint8_t *tractors = &buf[INSPECT_SNAPSHOT_HEADER + 2 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(tractors[0], COMMODITY_TRACTOR_MODULE);
    ASSERT_EQ_INT(tractors[1], MINING_GRADE_FINE);
    ASSERT(tractors[3] & INSPECT_ROW_GROUPED);
    ASSERT_EQ_INT(read_u16_le(&tractors[12]), 3);

    ship_cleanup(&ship);
}

TEST(test_inspect_snapshot_keeps_named_ingots_individual) {
    /* Hauler scan should group common anonymous bulk, but every named
     * / prefix-class ingot stays per-unit so the hash and provenance can
     * be inspected. */
    npc_ship_t npc;
    memset(&npc, 0, sizeof(npc));
    npc.active = true;
    npc.role = NPC_ROLE_HAULER;
    npc.state = NPC_STATE_TRAVEL_TO_DEST;
    npc.home_station = 0;
    npc.dest_station = 1;

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ASSERT(ship_manifest_bootstrap(&ship));

    uint8_t fragment_pub[32] = {0};
    cargo_unit_t h_units[3];
    /* Three H-class units at (FERRITE, COMMON). */
    for (int i = 0; i < 3; i++) {
        cargo_unit_t u;
        memset(&u, 0, sizeof(u));
        fragment_pub[31] = (uint8_t)(0xA0 + i);
        ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                          fragment_pub, (uint16_t)i, &u));
        u.prefix_class = (uint8_t)INGOT_PREFIX_H;
        h_units[i] = u;
        ASSERT(ship_manifest_push_with_chain(&ship, &u, NULL));
    }

    /* One RATI singleton at the same bucket. */
    cargo_unit_t solo;
    memset(&solo, 0, sizeof(solo));
    fragment_pub[31] = 0xB0;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON,
                      fragment_pub, 50, &solo));
    solo.prefix_class = (uint8_t)INGOT_PREFIX_RATI;
    ASSERT(ship_manifest_push_with_chain(&ship, &solo, NULL));

    uint8_t buf[INSPECT_SNAPSHOT_MAX_SIZE];
    int len = serialize_inspect_snapshot_npc(buf, 3, &npc, &ship);

    ASSERT_EQ_INT(buf[8], 4);
    ASSERT_EQ_INT(read_u16_le(&buf[9]), 4);
    ASSERT_EQ_INT(len, INSPECT_SNAPSHOT_HEADER + 4 * INSPECT_SNAPSHOT_ROW);

    for (int i = 0; i < 3; i++) {
        uint8_t *row = &buf[INSPECT_SNAPSHOT_HEADER + i * INSPECT_SNAPSHOT_ROW];
        ASSERT_EQ_INT(row[0], COMMODITY_FERRITE_INGOT);
        ASSERT_EQ_INT(row[1], MINING_GRADE_COMMON);
        ASSERT(!(row[3] & INSPECT_ROW_GROUPED));
        ASSERT_EQ_INT(read_u16_le(&row[12]), 1);
        ASSERT(memcmp(&row[14], h_units[i].pub, 32) == 0);
    }

    /* Row 3: RATI singleton, ungrouped, full pub. */
    uint8_t *single = &buf[INSPECT_SNAPSHOT_HEADER + 3 * INSPECT_SNAPSHOT_ROW];
    ASSERT_EQ_INT(single[0], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(single[1], MINING_GRADE_COMMON);
    ASSERT(!(single[3] & INSPECT_ROW_GROUPED));
    ASSERT_EQ_INT(read_u16_le(&single[12]), 1);
    ASSERT(memcmp(&single[14], solo.pub, 32) == 0);

    ship_cleanup(&ship);
}

TEST(test_roundtrip_stations) {
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));

    /* Mark station 0 as active so it gets serialized */
    stations[0].signal_range = 2200.0f;
    stations[0]._inventory_cache[0] = 45.5f;
    stations[0]._inventory_cache[1] = 12.3f;
    stations[0]._inventory_cache[2] = 78.9f;
    stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] = 20.0f;
    stations[0]._inventory_cache[COMMODITY_FRAME] = 15.5f;

    uint8_t buf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int len = serialize_stations(buf, stations);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_STATIONS);
    ASSERT_EQ_INT(buf[1], 1); /* only 1 active station */
    ASSERT_EQ_INT(len, 2 + 1 * STATION_RECORD_SIZE);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    /* inventory starts at byte 1, each commodity is 4 bytes */
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_FERRITE_ORE * 4]), 45.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_CUPRITE_ORE * 4]), 12.3f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_CRYSTAL_ORE * 4]), 78.9f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_FERRITE_INGOT * 4]), 20.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[1 + COMMODITY_FRAME * 4]), 15.5f, 0.1f);
}

TEST(test_station_identity_serializes_module_commodities) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.services = STATION_SERVICE_REPAIR;
    st.pos = v2(10.0f, -20.0f);
    st.radius = 60.0f;
    st.dock_radius = 110.0f;
    st.signal_range = 2000.0f;
    snprintf(st.name, sizeof(st.name), "Wire Test");
    st.module_count = 3;
    st.modules[0] = (station_module_t){
        .type = MODULE_HOPPER,
        .ring = 1,
        .slot = 0,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_CUPRITE_ORE,
        .build_progress = 1.0f,
    };
    st.modules[1] = (station_module_t){
        .type = MODULE_HOPPER,
        .ring = 2,
        .slot = 1,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_FRAME,
        .build_progress = 1.0f,
    };
    st.modules[2] = (station_module_t){
        .type = MODULE_FURNACE,
        .ring = 3,
        .slot = 2,
        .scaffold = false,
        .commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT,
        .build_progress = 1.0f,
    };

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 4, &st);

    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);
    ASSERT_EQ_INT(STATION_MODULE_RECORD_SIZE, 9);

    int moff = 59 + COMMODITY_COUNT * 4 + 4;
    ASSERT_EQ_INT(buf[moff], 3);
    moff++;
    ASSERT_EQ_INT(buf[moff + 8], COMMODITY_CUPRITE_ORE);
    moff += STATION_MODULE_RECORD_SIZE;
    ASSERT_EQ_INT(buf[moff + 8], COMMODITY_FRAME);
    moff += STATION_MODULE_RECORD_SIZE;
    ASSERT_EQ_INT(buf[moff + 8], COMMODITY_CRYSTAL_INGOT);
}

TEST(test_station_identity_serializes_operator_text) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.signal_range = 1000.0f;
    snprintf(st.name, sizeof(st.name), "Voice Test");
    snprintf(st.hail_message, sizeof(st.hail_message), "station motd");
    snprintf(st.miner_chatter[3], sizeof(st.miner_chatter[3]), "miner line");
    snprintf(st.hauler_chatter[5], sizeof(st.hauler_chatter[5]), "hauler line");
    snprintf(st.rati_hail_message, sizeof(st.rati_hail_message), "rati line");
    snprintf(st.currency_name, sizeof(st.currency_name), "voice scrip");

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 2, &st);
    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);

    int moff = 59 + COMMODITY_COUNT * 4 + 4
        + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE
        + 1 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4
        + 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE
        + 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT * STATION_PENDING_SCAFFOLD_RECORD_SIZE
        + 1 + STATION_PENDING_SHIP_RECORD_COUNT * STATION_PENDING_SHIP_RECORD_SIZE;

    ASSERT(memcmp(&buf[moff], "station motd", strlen("station motd")) == 0);
    moff += STATION_IDENTITY_HAIL_MESSAGE_LEN;
    ASSERT(memcmp(&buf[moff + 3 * STATION_IDENTITY_CHATTER_LINE_LEN],
                  "miner line", strlen("miner line")) == 0);
    moff += STATION_IDENTITY_CHATTER_LINES * STATION_IDENTITY_CHATTER_LINE_LEN;
    ASSERT(memcmp(&buf[moff + 5 * STATION_IDENTITY_CHATTER_LINE_LEN],
                  "hauler line", strlen("hauler line")) == 0);
    moff += STATION_IDENTITY_CHATTER_LINES * STATION_IDENTITY_CHATTER_LINE_LEN;
    ASSERT(memcmp(&buf[moff], "rati line", strlen("rati line")) == 0);
    moff += STATION_IDENTITY_RATI_HAIL_LEN;
    ASSERT(memcmp(&buf[moff], "voice scrip", strlen("voice scrip")) == 0);
}

TEST(test_station_identity_serializes_pending_ship_builds) {
    station_t st;
    memset(&st, 0, sizeof(st));
    st.pending_ship_build_count = 2;
    st.pending_ship_builds[0].hull_class = HULL_CLASS_HAULER;
    st.pending_ship_builds[0].owner = 3;
    st.pending_ship_builds[0].build_progress = 0.25f;
    st.pending_ship_builds[1].hull_class = HULL_CLASS_DRONE_TRACTOR;
    st.pending_ship_builds[1].owner = -1;
    st.pending_ship_builds[1].build_progress = 0.0f;

    uint8_t buf[STATION_IDENTITY_SIZE] = {0};
    int len = serialize_station_identity(buf, 2, &st);
    ASSERT_EQ_INT(len, STATION_IDENTITY_SIZE);

    int moff = 59 + COMMODITY_COUNT * 4 + 4
        + 1 + MAX_MODULES_PER_STATION * STATION_MODULE_RECORD_SIZE
        + 1 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4 + MAX_ARMS * 4
        + 1 + STATION_PLAN_RECORD_COUNT * STATION_PLAN_RECORD_SIZE
        + 1 + STATION_PENDING_SCAFFOLD_RECORD_COUNT * STATION_PENDING_SCAFFOLD_RECORD_SIZE;

    ASSERT_EQ_INT(buf[moff], 2);
    moff++;
    ASSERT_EQ_INT(buf[moff + 0], HULL_CLASS_HAULER);
    ASSERT_EQ_INT(buf[moff + 1], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[moff + 2]), 0.25f, 0.001f);
    moff += STATION_PENDING_SHIP_RECORD_SIZE;
    ASSERT_EQ_INT(buf[moff + 0], HULL_CLASS_DRONE_TRACTOR);
    ASSERT_EQ_INT(buf[moff + 1], 0xFF);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[moff + 2]), 0.0f, 0.001f);
}

TEST(test_bug92_station_record_size_matches_buffer) {
    /* Bug 92: station broadcast buffer must match serialized record size.
     * STATION_RECORD_SIZE is validated at compile time via _Static_assert,
     * but verify at runtime that serialize_stations writes exactly the
     * expected number of bytes. */
    station_t stations[MAX_STATIONS];
    memset(stations, 0, sizeof(stations));
    /* Empty stations should produce 0 records */
    uint8_t buf[2 + MAX_STATIONS * STATION_RECORD_SIZE];
    int len = serialize_stations(buf, stations);
    ASSERT_EQ_INT(len, 2); /* header only, no records */
    /* With active stations */
    for (int i = 0; i < 3; i++) stations[i].signal_range = 1000.0f;
    len = serialize_stations(buf, stations);
    ASSERT_EQ_INT(len, 2 + 3 * STATION_RECORD_SIZE);
    ASSERT((size_t)len <= sizeof(buf));
}

TEST(test_player_known_contract_mask_uses_compact_contract_ordinals) {
    contract_t contracts[MAX_CONTRACTS];
    memset(contracts, 0, sizeof(contracts));

    contracts[3] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 1,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 5.0f,
        .base_price = 10.0f,
        .target_index = -1,
    };
    contracts[7] = (contract_t){
        .active = true,
        .action = CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = COMMODITY_CUPRITE_INGOT,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE |
                                 CONTRACT_PROOF_FORBID_ORIGIN),
        .required_recipe_id = (uint16_t)RECIPE_SMELT,
        .forbidden_origin_mask = 1ULL << 0,
        .quantity_needed = 8.0f,
        .base_price = 20.0f,
        .target_index = -1,
    };
    for (int i = 0; i < 32; i++)
        contracts[7].target_pub[i] = (uint8_t)(0x40u + (uint8_t)i);

    uint8_t cbuf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int clen = serialize_contracts(cbuf, contracts);
    ASSERT_EQ_INT(clen, 2 + 2 * CONTRACT_RECORD_SIZE);
    ASSERT_EQ_INT(cbuf[1], 2);
    ASSERT_EQ_INT(cbuf[2 + CONTRACT_RECORD_SIZE + 1], 2);
    ASSERT_EQ_INT(cbuf[2 + CONTRACT_RECORD_SIZE + 2], COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(cbuf[2 + CONTRACT_RECORD_SIZE + 4],
                  CONTRACT_PROOF_REQUIRE_PROOF |
                  CONTRACT_PROOF_REQUIRE_RECIPE |
                  CONTRACT_PROOF_FORBID_ORIGIN);
    ASSERT_EQ_INT(read_u16_le(&cbuf[2 + CONTRACT_RECORD_SIZE + 6]), RECIPE_SMELT);
    ASSERT_EQ_INT((int)read_u64_le(&cbuf[2 + CONTRACT_RECORD_SIZE + 64]), 1);
    ASSERT(memcmp(&cbuf[2 + CONTRACT_RECORD_SIZE + 72],
                  contracts[7].target_pub, 32) == 0);

    ship_t ship;
    memset(&ship, 0, sizeof(ship));
    ship.known_contract_count = 1;
    ship.known_contracts[0] = (contract_summary_t){
        .active = true,
        .action = (uint8_t)CONTRACT_TRACTOR,
        .station_index = 2,
        .commodity = (uint8_t)COMMODITY_CUPRITE_INGOT,
        .proof_flags = (uint8_t)(CONTRACT_PROOF_REQUIRE_PROOF |
                                 CONTRACT_PROOF_REQUIRE_RECIPE |
                                 CONTRACT_PROOF_FORBID_ORIGIN),
        .required_recipe_id = (uint16_t)RECIPE_SMELT,
        .forbidden_origin_mask = 1ULL << 0,
        .quantity_needed = 8.0f,
        .base_price = 20.0f,
    };
    memcpy(ship.known_contracts[0].target_pub, contracts[7].target_pub, 32);

    uint8_t kbuf[5];
    int klen = serialize_player_known_contracts(kbuf, contracts, &ship);
    ASSERT_EQ_INT(klen, 5);
    ASSERT_EQ_INT(kbuf[0], NET_MSG_PLAYER_KNOWN_CONTRACTS);
    uint32_t mask = read_u32_le(&kbuf[1]);
    ASSERT_EQ_INT((int)mask, 1 << 1);
    ASSERT_EQ_INT(contract_compact_index_for_slot(contracts, 3), 0);
    ASSERT_EQ_INT(contract_compact_index_for_slot(contracts, 7), 1);
}

TEST(test_delivery_contract_action_serializes) {
    contract_t contracts[MAX_CONTRACTS];
    memset(contracts, 0, sizeof(contracts));
    contracts[0] = (contract_t){
        .active = true,
        .action = CONTRACT_DELIVERY,
        .station_index = 2,
        .target_index = 0,
        .commodity = COMMODITY_FERRITE_INGOT,
        .quantity_needed = 3.0f,
        .base_price = 42.0f,
    };

    uint8_t buf[2 + MAX_CONTRACTS * CONTRACT_RECORD_SIZE];
    int len = serialize_contracts(buf, contracts);
    ASSERT_EQ_INT(len, 2 + CONTRACT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_CONTRACTS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(buf[2], CONTRACT_DELIVERY);
    ASSERT_EQ_INT(buf[3], 2);
    ASSERT_EQ_INT((int)read_u32_le(&buf[2 + 28]), 0);
    ASSERT_EQ_INT(CONTRACT_RECORD_SIZE, 104);
}

TEST(test_delivery_ledger_serializes_player_shipments) {
    WORLD_DECL;
    world_reset(&w);
    w.delivery_shipments[0] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 77,
        .origin_station = 0,
        .destination_station = 2,
        .contract_index = 4,
        .debtor_player = 1,
        .commodity = (uint8_t)COMMODITY_FERRITE_INGOT,
        .quantity_total = 3,
        .quantity_bound = 3,
        .quantity_delivered = 1,
        .debt_principal = 60.0f,
        .destination_payout = 150.0f,
        .origin_completion_credit = 6.0f,
        .due_tick = 900,
        .status = DELIVERY_SHIPMENT_PICKED_UP,
    };
    w.delivery_shipments[1] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 88,
        .debtor_player = 1,
        .status = DELIVERY_SHIPMENT_CLEARED,
    };
    w.delivery_shipments[2] = (delivery_shipment_t){
        .active = true,
        .shipment_id = 99,
        .debtor_player = 0,
        .status = DELIVERY_SHIPMENT_PICKED_UP,
    };

    uint8_t buf[DELIVERY_LEDGER_HEADER +
                DELIVERY_LEDGER_MAX_RECORDS * DELIVERY_LEDGER_RECORD_SIZE];
    int len = serialize_delivery_ledger(buf, &w, 1);
    ASSERT_EQ_INT(len, DELIVERY_LEDGER_HEADER + DELIVERY_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_DELIVERY_LEDGER);
    ASSERT_EQ_INT(buf[1], 1);
    const uint8_t *p = &buf[DELIVERY_LEDGER_HEADER];
    ASSERT_EQ_INT(read_u16_le(&p[0]), 77);
    ASSERT_EQ_INT(p[2], DELIVERY_SHIPMENT_PICKED_UP);
    ASSERT_EQ_INT(p[3], 0);
    ASSERT_EQ_INT(p[4], 2);
    ASSERT_EQ_INT(p[5], 4);
    ASSERT_EQ_INT(p[6], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(read_u16_le(&p[7]), 3);
    ASSERT_EQ_INT(read_u16_le(&p[9]), 1);
    ASSERT_EQ_INT(read_u16_le(&p[11]), 3);
    ASSERT_EQ_FLOAT(read_f32_le(&p[13]), 60.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[17]), 150.0f, 0.001f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[21]), 6.0f, 0.001f);
    ASSERT_EQ_INT((int)read_u32_le(&p[25]), 900);
}

TEST(test_bug93_hint_mines_small_shard_with_minor_desync) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.npc_ships, 0, sizeof(w.npc_ships));

    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].id = 0;
    w.players[0].docked = false;
    w.players[0].in_dock_range = false;
    w.players[0].nearby_station = -1;
    w.players[0].ship.pos = v2(0.0f, 0.0f);
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].ship.angle = 0.0f;
    w.players[0].ship.mining_level = 0;
    w.players[0].input.mine = true;
    w.players[0].input.mining_target_hint = 0;

    /* Place an M-tier shard just outside the exact server ray, as would
     * happen when the client view is a few units behind a fast fracture child.
     * Exact fallback targeting should miss it; the explicit hint should still
     * be accepted and mine it. */
    w.asteroids[0].active = true;
    w.asteroids[0].fracture_child = true;
    w.asteroids[0].tier = ASTEROID_TIER_M;
    w.asteroids[0].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[0].pos = v2(80.0f, 26.0f);
    w.asteroids[0].vel = v2(0.0f, 0.0f);
    w.asteroids[0].radius = 20.0f;
    w.asteroids[0].hp = 40.0f;
    w.asteroids[0].max_hp = 40.0f;

    float hp_before = w.asteroids[0].hp;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].hover_asteroid, 0);
    ASSERT(w.asteroids[0].hp < hp_before);
}

TEST(test_roundtrip_player_ship) {
    server_player_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.ship.hull = 85.5f;
    sp.docked = true;
    sp.current_station = 2;
    sp.ship.mining_level = 3;
    sp.ship.hold_level = 2;
    sp.ship.tractor_level = 1;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = 45.0f;
    sp.ship.cargo[COMMODITY_CUPRITE_ORE] = 12.5f;
    sp.ship.cargo[COMMODITY_CRYSTAL_ORE] = 8.0f;
    sp.ship.cargo[COMMODITY_FERRITE_INGOT] = 20.0f;

    uint8_t buf[PLAYER_SHIP_SIZE];
    int len = serialize_player_ship_bal(buf, 3, &sp, 1234.0f);

    ASSERT(len <= PLAYER_SHIP_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_PLAYER_SHIP);
    ASSERT_EQ_INT(buf[1], 3);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[2]), 85.5f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&buf[6]), 1234.0f, 0.1f);
    ASSERT_EQ_INT(buf[10], 1);   /* docked */
    ASSERT_EQ_INT(buf[11], 2);   /* station */
    ASSERT_EQ_INT(buf[12], 3);   /* mining_level */
    ASSERT_EQ_INT(buf[13], 2);   /* hold_level */
    ASSERT_EQ_INT(buf[14], 1);   /* tractor_level */
    ASSERT_EQ_INT(buf[15], 0);   /* reserved (was has_scaffold_kit) */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[16]), 45.0f, 0.1f);   /* ferrite ore */
    ASSERT_EQ_FLOAT(read_f32_le(&buf[16 + 3*4]), 20.0f, 0.1f); /* ferrite ingot */
}

TEST(test_named_ingot_record_serializes_grade) {
    station_t st;
    memset(&st, 0, sizeof(st));
    ASSERT(station_manifest_bootstrap(&st));

    cargo_unit_t unit = {0};
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_RARE;
    unit.prefix_class = (uint8_t)INGOT_PREFIX_M;
    unit.recipe_id = (uint16_t)RECIPE_SMELT;
    unit.origin_station = 7;
    unit.quantity = 1;
    unit.mined_block = 0x0102030405060708ull;
    for (int i = 0; i < 32; i++) unit.pub[i] = (uint8_t)(0xA0 + i);
    ASSERT(manifest_push(&st.manifest, &unit));

    uint8_t buf[STATION_INGOTS_HEADER + NAMED_INGOT_RECORD_SIZE];
    int len = serialize_station_ingots(buf, 3, &st);
    ASSERT_EQ_INT(len, STATION_INGOTS_HEADER + NAMED_INGOT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_STATION_INGOTS);
    ASSERT_EQ_INT(buf[1], 3);
    ASSERT_EQ_INT(buf[2], 1);

    const uint8_t *p = &buf[STATION_INGOTS_HEADER];
    ASSERT_EQ_INT(p[32], INGOT_PREFIX_M);
    ASSERT_EQ_INT(p[33], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[34], MINING_GRADE_RARE);
    ASSERT_EQ_INT(p[44], 7);
    ASSERT_EQ_INT(p[36], 0x08);
    ASSERT_EQ_INT(p[43], 0x01);

    station_cleanup(&st);
}

TEST(test_parse_input_valid) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST | NET_INPUT_LEFT | NET_INPUT_FIRE | NET_INPUT_BOOST,
        NET_ACTION_SELL_CARGO,
        0xFF  /* no mining target */
    };

    parse_input(msg, 4, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(intent.turn, 1.0f, 0.01f);
    ASSERT(intent.mine);
    ASSERT(intent.boost);
    ASSERT(intent.service_sell);
}

TEST(test_parse_input_reverse_flag) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = {
        NET_MSG_INPUT,
        NET_INPUT_BRAKE,
        NET_ACTION_NONE,
        0xFF
    };

    parse_input(msg, 4, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, -1.0f, 0.01f);
    ASSERT(!intent.reverse_thrust);

    msg[1] = NET_INPUT_BRAKE | NET_INPUT_REVERSE;
    parse_input(msg, 4, &intent);
    ASSERT_EQ_FLOAT(intent.thrust, -1.0f, 0.01f);
    ASSERT(intent.reverse_thrust);
}

TEST(test_parse_input_too_short) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));
    intent.thrust = 99.0f;  /* canary value */

    uint8_t msg[3] = { NET_MSG_INPUT, 0xFF, 0 };
    parse_input(msg, 3, &intent);

    /* Too short (< 4 bytes) — should not modify intent */
    ASSERT_EQ_FLOAT(intent.thrust, 99.0f, 0.01f);
}

TEST(test_parse_input_no_action) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = { NET_MSG_INPUT, NET_INPUT_THRUST, NET_ACTION_NONE, 0xFF };
    parse_input(msg, 4, &intent);

    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT(!intent.service_sell);
    ASSERT(!intent.interact);
}

TEST(test_parse_input_v2_uint16_mining_target) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[12] = {
        NET_MSG_INPUT,
        NET_INPUT_FIRE,
        NET_ACTION_NONE,
        0x2C, /* legacy low byte for target 300 */
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x34, 0x12, /* input seq */
        0x2C, 0x01  /* mining target 300 */
    };

    parse_input(msg, sizeof(msg), &intent);
    ASSERT(intent.mine);
    ASSERT_EQ_INT(intent.mining_target_hint, 300);

    msg[10] = 0xFF;
    msg[11] = 0xFF;
    parse_input(msg, sizeof(msg), &intent);
    ASSERT_EQ_INT(intent.mining_target_hint, -1);
}

TEST(test_parse_input_v3_action_id) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[14] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST,
        NET_ACTION_LAUNCH,
        0xFF,
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x78, 0x56,
        0xFF, 0xFF,
        0x34, 0x12
    };

    parse_input(msg, sizeof(msg), &intent);
    ASSERT_EQ_FLOAT(intent.thrust, 1.0f, 0.01f);
    ASSERT(intent.launch);
    ASSERT_EQ_INT((int)input_action_id(msg, sizeof(msg)), 0x1234);
    ASSERT_EQ_INT((int)input_action_id(msg, 12), 0);
}

TEST(test_parse_input_v4_client_tick) {
    uint8_t msg[18] = {
        NET_MSG_INPUT,
        NET_INPUT_THRUST,
        NET_ACTION_NONE,
        0xFF,
        MINING_GRADE_COUNT,
        0xFF, 0xFF, 0xFF,
        0x78, 0x56,
        0xFF, 0xFF,
        0x34, 0x12,
        0xEF, 0xCD, 0xAB, 0x89
    };

    ASSERT_EQ_INT((int)input_client_tick(msg, sizeof(msg)), (int)0x89ABCDEFu);
    ASSERT_EQ_INT((int)input_client_tick(msg, 14), 0);
}

TEST(test_ticked_movement_input_applies_on_sim_tick) {
    world_t w;
    memset(&w, 0, sizeof(w));
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    input_intent_t intent = {0};
    intent.turn = 1.0f;
    intent.thrust = 1.0f;
    intent.mine = true;
    intent.mining_target_hint = 7;
    server_player_queue_movement_input(sp, &intent, 77, 2);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)w.tick, 1);
    ASSERT_EQ_INT((int)sp->last_input_seq, 0);
    ASSERT_EQ_FLOAT(sp->input.turn, 0.0f, 0.01f);

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)w.tick, 2);
    ASSERT_EQ_INT((int)sp->last_input_seq, 77);
    ASSERT_EQ_INT((int)sp->last_input_tick, 2);
    ASSERT_EQ_FLOAT(sp->input.turn, 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(sp->input.thrust, 1.0f, 0.01f);
    ASSERT(sp->input.mine);
    ASSERT_EQ_INT(sp->input.mining_target_hint, 7);

    world_cleanup(&w);
}

TEST(test_latency_pong_can_arrive_before_authoritative_input_ack) {
    world_t w;
    memset(&w, 0, sizeof(w));
    world_reset(&w);
    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, &w);

    input_intent_t intent = {0};
    intent.thrust = 1.0f;
    server_player_queue_movement_input(sp, &intent, 42, 6);

    uint8_t pong[NET_LATENCY_PONG_SIZE];
    int pong_len = serialize_latency_pong(pong, 99u, 1000u, 1001u, 1001u);
    ASSERT_EQ_INT(pong_len, NET_LATENCY_PONG_SIZE);
    ASSERT_EQ_INT(pong[0], NET_MSG_LATENCY_PONG);
    ASSERT_EQ_INT((int)read_u32_le(&pong[1]), 99);

    uint8_t players[2 + MAX_PLAYERS * PLAYER_RECORD_SIZE];
    int players_len = serialize_all_player_states(players, w.players, w.tick);
    ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(players[0], NET_MSG_WORLD_PLAYERS);
    ASSERT_EQ_INT(players[1], 1);
    ASSERT_EQ_INT((int)read_u16_le(&players[2 + 67]), 0);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 69]), 0);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 73]), 0);

    for (int tick = 1; tick < 6; tick++) {
        world_sim_step(&w, SIM_DT);
        players_len = serialize_all_player_states(players, w.players, w.tick);
        ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
        ASSERT_EQ_INT((int)w.tick, tick);
        ASSERT_EQ_INT((int)read_u16_le(&players[2 + 67]), 0);
    }

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT((int)w.tick, 6);
    players_len = serialize_all_player_states(players, w.players, w.tick);
    ASSERT_EQ_INT(players_len, 2 + PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT((int)read_u16_le(&players[2 + 67]), 42);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 69]), 6);
    ASSERT_EQ_INT((int)read_u32_le(&players[2 + 73]), 6);

    world_cleanup(&w);
}

TEST(test_action_ack_roundtrip) {
    uint8_t buf[NET_ACTION_ACK_SIZE];
    int len = serialize_action_ack(buf, 0x1234, 0x5678,
                                   NET_ACTION_ACK_DUPLICATE,
                                   NET_ACTION_DOCK);

    ASSERT_EQ_INT(len, NET_ACTION_ACK_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_ACTION_ACK);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), 0x1234);
    ASSERT_EQ_INT((int)read_u16_le(&buf[3]), 0x5678);
    ASSERT_EQ_INT(buf[5], NET_ACTION_ACK_DUPLICATE);
    ASSERT_EQ_INT(buf[6], NET_ACTION_DOCK);
}

TEST(test_action_result_roundtrip) {
    uint8_t buf[NET_ACTION_RESULT_SIZE];
    int len = serialize_action_result(buf, 0x1234, 0x5678,
                                      NET_ACTION_RESULT_OK,
                                      NET_ACTION_LAUNCH,
                                      0xAABBCCDDu);

    ASSERT_EQ_INT(len, NET_ACTION_RESULT_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_ACTION_RESULT);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), 0x1234);
    ASSERT_EQ_INT((int)read_u16_le(&buf[3]), 0x5678);
    ASSERT_EQ_INT(buf[5], NET_ACTION_RESULT_OK);
    ASSERT_EQ_INT(buf[6], NET_ACTION_LAUNCH);
    ASSERT_EQ_INT((int)read_u32_le(&buf[7]), (int)0xAABBCCDDu);
}

TEST(test_latency_pong_roundtrip) {
    uint8_t buf[NET_LATENCY_PONG_SIZE];
    int len = serialize_latency_pong(buf, 0x11223344u, 0x55667788u,
                                     0x99AABBCDu, 0xDDEEFF00u);

    ASSERT_EQ_INT(len, NET_LATENCY_PONG_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_LATENCY_PONG);
    ASSERT_EQ_INT((int)read_u32_le(&buf[1]), (int)0x11223344u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[5]), (int)0x55667788u);
    ASSERT_EQ_INT((int)read_u32_le(&buf[9]), (int)0x99AABBCDu);
    ASSERT_EQ_INT((int)read_u32_le(&buf[13]), (int)0xDDEEFF00u);
}

static const uint8_t *find_protocol_stream(const uint8_t *buf, uint8_t msg) {
    int count = buf[7];
    for (int i = 0; i < count; i++) {
        const uint8_t *p = &buf[PROTOCOL_INFO_HEADER_SIZE +
                                i * PROTOCOL_INFO_STREAM_RECORD_SIZE];
        if (p[0] == msg) return p;
    }
    return NULL;
}

TEST(test_protocol_info_serializes_stream_map) {
    uint8_t buf[PROTOCOL_INFO_SIZE];
    int len = serialize_protocol_info(buf, 8, 50, 100, 250, 300, 2000);

    ASSERT(len >= PROTOCOL_INFO_HEADER_SIZE);
    ASSERT(len <= PROTOCOL_INFO_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_PROTOCOL_INFO);
    ASSERT_EQ_INT((int)read_u16_le(&buf[1]), (int)SIGNAL_PROTOCOL_VERSION);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_PROTOCOL_INFO);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_STATION_DIAG);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_HANDOFF_TICKETS);
    ASSERT(read_u32_le(&buf[3]) & SIGNAL_PROTOCOL_CAP_DELIVERY_SHIPMENTS);
    ASSERT_EQ_INT(buf[7], (len - PROTOCOL_INFO_HEADER_SIZE) /
                          PROTOCOL_INFO_STREAM_RECORD_SIZE);
    ASSERT(buf[7] <= PROTOCOL_INFO_STREAM_CAPACITY);

    const uint8_t *diag = find_protocol_stream(buf, NET_MSG_STATION_DIAG);
    ASSERT(diag != NULL);
    ASSERT_EQ_INT(diag[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&diag[2]) & PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT(read_u16_le(&diag[2]) & PROTOCOL_STREAM_FLAG_FIXED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&diag[4]), 3);
    ASSERT_EQ_INT(read_u16_le(&diag[6]), 1);
    ASSERT_EQ_INT(read_u16_le(&diag[8]), MAX_MODULES_PER_STATION);
    ASSERT_EQ_INT(read_u16_le(&diag[10]), 300);

    const uint8_t *identity = find_protocol_stream(buf, NET_MSG_STATION_IDENTITY);
    ASSERT(identity != NULL);
    ASSERT_EQ_INT(identity[1], PROTOCOL_STREAM_CLASS_STATIC);
    ASSERT_EQ_INT(read_u16_le(&identity[4]), STATION_IDENTITY_SIZE);
    ASSERT_EQ_INT(read_u16_le(&identity[10]), 2000);

    const uint8_t *players = find_protocol_stream(buf, NET_MSG_WORLD_PLAYERS);
    ASSERT(players != NULL);
    ASSERT_EQ_INT(read_u16_le(&players[6]), PLAYER_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&players[10]), 50);

    const uint8_t *npcs = find_protocol_stream(buf, NET_MSG_WORLD_NPCS);
    ASSERT(npcs != NULL);
    ASSERT_EQ_INT(npcs[1], PROTOCOL_STREAM_CLASS_LIVE);
    ASSERT(read_u16_le(&npcs[2]) & PROTOCOL_STREAM_FLAG_RELEVANCE_FILTER);
    ASSERT_EQ_INT(read_u16_le(&npcs[4]), 2);
    ASSERT_EQ_INT(read_u16_le(&npcs[6]), NPC_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&npcs[8]), MAX_NPC_SHIPS);
    ASSERT_EQ_INT(read_u16_le(&npcs[10]), 100);

    const uint8_t *input = find_protocol_stream(buf, NET_MSG_INPUT);
    ASSERT(input != NULL);
    ASSERT_EQ_INT(read_u16_le(&input[4]), NET_INPUT_MSG_SIZE);
    ASSERT_EQ_INT(read_u16_le(&input[10]), 8);

    const uint8_t *contracts = find_protocol_stream(buf, NET_MSG_CONTRACTS);
    ASSERT(contracts != NULL);
    ASSERT_EQ_INT(read_u16_le(&contracts[6]), CONTRACT_RECORD_SIZE);
    ASSERT_EQ_INT(CONTRACT_RECORD_SIZE, 104);

    const uint8_t *player_manifest = find_protocol_stream(buf, NET_MSG_PLAYER_MANIFEST);
    ASSERT(player_manifest != NULL);
    ASSERT(read_u16_le(&player_manifest[2]) & PROTOCOL_STREAM_FLAG_PER_PLAYER);
    ASSERT_EQ_INT(read_u16_le(&player_manifest[4]), PLAYER_MANIFEST_HEADER);
    ASSERT_EQ_INT(read_u16_le(&player_manifest[6]), PLAYER_MANIFEST_ENTRY);

    const uint8_t *delivery_ledger = find_protocol_stream(buf, NET_MSG_DELIVERY_LEDGER);
    ASSERT(delivery_ledger != NULL);
    ASSERT_EQ_INT(delivery_ledger[1], PROTOCOL_STREAM_CLASS_PLAYER);
    ASSERT(read_u16_le(&delivery_ledger[2]) & PROTOCOL_STREAM_FLAG_PER_PLAYER);
    ASSERT(read_u16_le(&delivery_ledger[2]) & PROTOCOL_STREAM_FLAG_DIRTY_ONLY);
    ASSERT_EQ_INT(read_u16_le(&delivery_ledger[4]), DELIVERY_LEDGER_HEADER);
    ASSERT_EQ_INT(read_u16_le(&delivery_ledger[6]), DELIVERY_LEDGER_RECORD_SIZE);
    ASSERT_EQ_INT(read_u16_le(&delivery_ledger[8]), DELIVERY_LEDGER_MAX_RECORDS);

    const uint8_t *handoff_request = find_protocol_stream(buf, NET_MSG_HANDOFF_REQUEST);
    ASSERT(handoff_request != NULL);
    ASSERT_EQ_INT(handoff_request[1], PROTOCOL_STREAM_CLASS_AUTH);
    ASSERT(read_u16_le(&handoff_request[2]) & PROTOCOL_STREAM_FLAG_FIXED_SIZE);
    ASSERT_EQ_INT(read_u16_le(&handoff_request[4]), NET_HANDOFF_REQUEST_SIZE);

    const uint8_t *handoff_present = find_protocol_stream(buf, NET_MSG_HANDOFF_PRESENT);
    ASSERT(handoff_present != NULL);
    ASSERT_EQ_INT(handoff_present[1], PROTOCOL_STREAM_CLASS_AUTH);
    ASSERT_EQ_INT(read_u16_le(&handoff_present[4]),
                  1 + HANDOFF_TICKET_SIZE + 4 + HANDOFF_SHIP_SNAPSHOT_HEADER_SIZE);
    ASSERT_EQ_INT(read_u16_le(&handoff_present[6]),
                  HANDOFF_CARGO_UNIT_WIRE_SIZE + 1 +
                  CARGO_RECEIPT_CHAIN_MAX_LEN * CARGO_RECEIPT_SIZE);
    ASSERT_EQ_INT(read_u16_le(&handoff_present[8]),
                  HANDOFF_SHIP_SNAPSHOT_MAX_CARGO);
}

TEST(test_buy_event_serializes_cost_and_quantity) {
    sim_events_t events;
    memset(&events, 0, sizeof(events));
    events.count = 1;
    events.events[0] = (sim_event_t){
        .type = SIM_EVENT_BUY,
        .player_id = 3,
        .buy = {
            .station = 2,
            .commodity = COMMODITY_FERRITE_INGOT,
            .grade = MINING_GRADE_RARE,
            .cost = 127,
            .quantity = 4,
        },
    };

    uint8_t buf[2 + NET_EVENT_RECORD_SIZE];
    int len = serialize_events(buf, &events);

    ASSERT_EQ_INT(len, 2 + NET_EVENT_RECORD_SIZE);
    ASSERT_EQ_INT(buf[0], NET_MSG_EVENTS);
    ASSERT_EQ_INT(buf[1], 1);

    const uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], SIM_EVENT_BUY);
    ASSERT_EQ_INT(p[1], 3);
    ASSERT_EQ_INT(p[2], 2);
    ASSERT_EQ_INT(p[3], COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(p[4], MINING_GRADE_RARE);
    ASSERT_EQ_INT((int)read_u32_le(&p[5]), 127);
    ASSERT_EQ_INT((int)read_u16_le(&p[9]), 4);
}

TEST(test_parse_input_action_accumulates) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    /* First input: dock action */
    uint8_t msg1[4] = { NET_MSG_INPUT, 0, NET_ACTION_DOCK, 0xFF };
    parse_input(msg1, 4, &intent);
    ASSERT(intent.dock);
    ASSERT(!intent.launch);
    ASSERT(intent.interact);

    /* Second input: sell action — should OR in, not replace */
    uint8_t msg2[4] = { NET_MSG_INPUT, 0, NET_ACTION_SELL_CARGO, 0xFF };
    parse_input(msg2, 4, &intent);
    ASSERT(intent.dock);           /* still true from first */
    ASSERT(intent.interact);       /* still true from first */
    ASSERT(intent.service_sell);   /* added by second */
}

TEST(test_parse_input_launch_keeps_semantic_action) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    uint8_t msg[4] = { NET_MSG_INPUT, 0, NET_ACTION_LAUNCH, 0xFF };
    parse_input(msg, 4, &intent);

    ASSERT(intent.launch);
    ASSERT(!intent.dock);
    ASSERT(intent.interact);
}

void register_protocol_main_tests(void) {
    TEST_SECTION("\nProtocol roundtrip tests:\n");
    RUN(test_roundtrip_player_state);
    RUN(test_roundtrip_batched_player_states);
    RUN(test_roundtrip_asteroids);
    RUN(test_roundtrip_asteroids_full_skips_inactive_slots);
    RUN(test_roundtrip_cargo_pods);
    RUN(test_roundtrip_npcs);
    RUN(test_npc_role_default_hull_mapping_covers_tow);
    RUN(test_roundtrip_inspect_snapshot_npc_manifest_chain);
    RUN(test_inspect_snapshot_npc_expands_matching_receipt_chain);
    RUN(test_inspect_snapshot_npc_retrieves_matching_station_receipt_chain);
    RUN(test_roundtrip_inspect_snapshot_player_manifest_chain);
    RUN(test_inspect_snapshot_npc_includes_market_memory_diagnostics);
    RUN(test_inspect_snapshot_npc_expands_matching_job_source_memory);
    RUN(test_inspect_snapshot_npc_includes_job_offer_diagnostics);
    RUN(test_inspect_snapshot_groups_anonymous_ingots_by_grade);
    RUN(test_inspect_snapshot_groups_finished_goods_by_grade);
    RUN(test_inspect_snapshot_keeps_named_ingots_individual);
    RUN(test_roundtrip_stations);
    RUN(test_station_identity_serializes_module_commodities);
    RUN(test_station_identity_serializes_operator_text);
    RUN(test_station_identity_serializes_pending_ship_builds);
    RUN(test_bug92_station_record_size_matches_buffer);
    RUN(test_player_known_contract_mask_uses_compact_contract_ordinals);
    RUN(test_delivery_contract_action_serializes);
    RUN(test_delivery_ledger_serializes_player_shipments);
    RUN(test_bug93_hint_mines_small_shard_with_minor_desync);
    RUN(test_roundtrip_player_ship);
    RUN(test_named_ingot_record_serializes_grade);
    RUN(test_parse_input_valid);
    RUN(test_parse_input_reverse_flag);
    RUN(test_parse_input_too_short);
    RUN(test_parse_input_no_action);
    RUN(test_parse_input_v2_uint16_mining_target);
    RUN(test_parse_input_v3_action_id);
    RUN(test_parse_input_v4_client_tick);
    RUN(test_ticked_movement_input_applies_on_sim_tick);
    RUN(test_latency_pong_can_arrive_before_authoritative_input_ack);
    RUN(test_action_ack_roundtrip);
    RUN(test_action_result_roundtrip);
    RUN(test_latency_pong_roundtrip);
    RUN(test_protocol_info_serializes_stream_map);
    RUN(test_buy_event_serializes_cost_and_quantity);
    RUN(test_parse_input_action_accumulates);
    RUN(test_parse_input_launch_keeps_semantic_action);
}
