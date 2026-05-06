#include "tests/test_harness.h"

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
    int len = serialize_all_player_states(buf, players);

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

    uint8_t buf[2 + MAX_ASTEROIDS * ASTEROID_RECORD_SIZE];
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
}

TEST(test_roundtrip_asteroids_full_includes_inactive_slots) {
    asteroid_t asteroids[MAX_ASTEROIDS];
    memset(asteroids, 0, sizeof(asteroids));

    /* Join-time full sync must include inactive slots so a client can clear
     * any locally seeded asteroid that the authoritative server no longer has. */
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
    free(buf);
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
    npcs[0].target_asteroid = 12;

    npcs[0].tint_r = 0.55f;
    npcs[0].tint_g = 0.25f;
    npcs[0].tint_b = 0.18f;

    uint8_t buf[2 + MAX_NPC_SHIPS * 26];
    int len = serialize_npcs(buf, npcs);

    ASSERT_EQ_INT(buf[0], NET_MSG_WORLD_NPCS);
    ASSERT_EQ_INT(buf[1], 1);
    ASSERT_EQ_INT(len, 2 + 26);

    uint8_t *p = &buf[2];
    ASSERT_EQ_INT(p[0], 0);
    ASSERT(p[1] & 1);                              /* active */
    ASSERT_EQ_INT((p[1] >> 1) & 0x3, NPC_ROLE_MINER);
    ASSERT_EQ_INT((p[1] >> 3) & 0x7, NPC_STATE_MINING);
    ASSERT(p[1] & (1 << 6));                        /* thrusting */
    ASSERT_EQ_FLOAT(read_f32_le(&p[2]), 800.0f, 0.1f);
    ASSERT_EQ_FLOAT(read_f32_le(&p[18]), 1.57f, 0.01f);
    ASSERT_EQ_INT((int8_t)p[22], 12);              /* target_asteroid */
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

TEST(test_parse_input_action_accumulates) {
    input_intent_t intent;
    memset(&intent, 0, sizeof(intent));

    /* First input: dock action */
    uint8_t msg1[4] = { NET_MSG_INPUT, 0, NET_ACTION_DOCK, 0xFF };
    parse_input(msg1, 4, &intent);
    ASSERT(intent.interact);

    /* Second input: sell action — should OR in, not replace */
    uint8_t msg2[4] = { NET_MSG_INPUT, 0, NET_ACTION_SELL_CARGO, 0xFF };
    parse_input(msg2, 4, &intent);
    ASSERT(intent.interact);       /* still true from first */
    ASSERT(intent.service_sell);   /* added by second */
}

void register_protocol_main_tests(void) {
    TEST_SECTION("\nProtocol roundtrip tests:\n");
    RUN(test_roundtrip_player_state);
    RUN(test_roundtrip_batched_player_states);
    RUN(test_roundtrip_asteroids);
    RUN(test_roundtrip_asteroids_full_includes_inactive_slots);
    RUN(test_roundtrip_npcs);
    RUN(test_roundtrip_inspect_snapshot_npc_manifest_chain);
    RUN(test_inspect_snapshot_groups_anonymous_ingots_by_grade);
    RUN(test_roundtrip_stations);
    RUN(test_station_identity_serializes_module_commodities);
    RUN(test_bug92_station_record_size_matches_buffer);
    RUN(test_bug93_hint_mines_small_shard_with_minor_desync);
    RUN(test_roundtrip_player_ship);
    RUN(test_named_ingot_record_serializes_grade);
    RUN(test_parse_input_valid);
    RUN(test_parse_input_reverse_flag);
    RUN(test_parse_input_too_short);
    RUN(test_parse_input_no_action);
    RUN(test_parse_input_action_accumulates);
}
