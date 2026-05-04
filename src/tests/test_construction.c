#include "tests/test_harness.h"

TEST(test_outpost_requires_signal_range) {
    WORLD_DECL;
    world_reset(&w);
    /* Can't place outside signal range */
    bool ok = can_place_outpost(&w, v2(100000.0f, 100000.0f));
    ASSERT(!ok);
    /* Can place within signal range (near refinery at (0,-2400), range 18000) */
    bool ok2 = can_place_outpost(&w, v2_add(w.stations[0].pos, v2(5000.0f, 0.0f)));
    ASSERT(ok2);
}

TEST(test_outpost_extends_signal_range) {
    WORLD_DECL;
    world_reset(&w);
    /* Place point at edge of refinery signal — within range but far */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(16000.0f, 0.0f));
    /* Verify the point is in signal before placing */
    ASSERT(signal_strength_at(&w, outpost_pos) > 0.0f);

    /* Set up a player docked at Kepler Yard (station 1, has BLUEPRINT) */
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    

    int slot = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(slot >= 3);
    /* Scaffold doesn't provide signal — only the parent refinery + a sliver
     * of Helios cover this far-east fringe point. The overlap boost applies
     * (2 stations), so the effective strength can reach ~0.2-0.3 even though
     * each individual contribution is near the edge. */
    ASSERT(signal_strength_at(&w, outpost_pos) > 0.0f);
    ASSERT(signal_strength_at(&w, outpost_pos) < 0.3f);
    /* Complete construction to activate signal */
    w.stations[slot].scaffold = false;
    w.stations[slot].scaffold_progress = 1.0f;
    rebuild_signal_chain(&w);
    /* Now the outpost itself provides strong signal at its own position */
    float s = signal_strength_at(&w, outpost_pos);
    ASSERT(s > 0.9f);
    /* Signal should extend beyond the outpost */
    float s2 = signal_strength_at(&w, v2_add(outpost_pos, v2(3000.0f, 0.0f)));
    ASSERT(s2 > 0.0f);
}

TEST(test_disconnected_station_goes_dark) {
    WORLD_DECL;
    world_reset(&w);
    /* All 3 starter stations should be connected */
    ASSERT(w.stations[0].signal_connected);
    ASSERT(w.stations[1].signal_connected);
    ASSERT(w.stations[2].signal_connected);

    /* Place an outpost within signal range of station 0 */
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* credits are station-local (ledger) — no ship.credits field */
    w.players[0].docked = false;

    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(5000.0f, 0.0f));
    int slot = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(slot >= 0);
    /* Finish construction */
    w.stations[slot].scaffold_progress = 1.0f;
    w.stations[slot].scaffold = false;
    w.stations[slot].signal_range = 6000.0f;
    w.stations[slot].signal_connected = false;
    w.stations[slot].modules[w.stations[slot].module_count++] = (station_module_t){ .type = MODULE_REPAIR_BAY };
    rebuild_signal_chain(&w);
    ASSERT(w.stations[slot].signal_connected);
    ASSERT(station_provides_signal(&w.stations[slot]));

    /* Shrink ALL root stations so the outpost is disconnected */
    float saved[3];
    for (int i = 0; i < 3; i++) {
        saved[i] = w.stations[i].signal_range;
        w.stations[i].signal_range = 1.0f;
    }
    rebuild_signal_chain(&w);
    ASSERT(!w.stations[slot].signal_connected);
    ASSERT(!station_provides_signal(&w.stations[slot]));

    /* Restore — outpost should reconnect */
    for (int i = 0; i < 3; i++)
        w.stations[i].signal_range = saved[i];
    rebuild_signal_chain(&w);
    ASSERT(w.stations[slot].signal_connected);
}

TEST(test_outpost_requires_undocked) {
    /* Must be undocked to place an outpost */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    /* credits are station-local (ledger) — no ship.credits field */
    

    /* Docked — should fail */
    w.players[0].docked = true;
    int slot = test_place_outpost_via_tow(&w, &w.players[0], v2(6000.0f, -2400.0f));
    ASSERT_EQ_INT(slot, -1);

    /* Undocked — should succeed */
    w.players[0].docked = false;
    slot = test_place_outpost_via_tow(&w, &w.players[0], v2(6000.0f, -2400.0f));
    ASSERT(slot >= 3);
}

TEST(test_outpost_requires_towed_scaffold) {
    /* Without a towed scaffold, place_outpost intent is a no-op. */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    /* No spawn_scaffold call — ship has no towed_scaffold */
    w.players[0].input.place_outpost = true;
    world_sim_step(&w, SIM_DT);
    /* No new outpost should exist */
    bool any_new = false;
    for (int s = 3; s < MAX_STATIONS; s++)
        if (station_exists(&w.stations[s])) { any_new = true; break; }
    ASSERT(!any_new);
}

TEST(test_outpost_min_distance) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */
    
    /* Too close to Prospect Refinery at (0,-2400) — within OUTPOST_MIN_DISTANCE (800) */
    int slot = test_place_outpost_via_tow(&w, &w.players[0], v2_add(w.stations[0].pos, v2(500.0f, 0.0f)));
    ASSERT_EQ_INT(slot, -1);
}

TEST(test_module_build_material_types) {
    /* Verify each module requires the correct ingot type. LASER_FAB
     * needs cuprite + crystal ingot hoppers. Plant both, then queue
     * the laser fab. */
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    add_hopper_for(st, 3, 1, COMMODITY_CUPRITE_INGOT);
    add_hopper_for(st, 3, 7, COMMODITY_CRYSTAL_INGOT);
    begin_module_construction_at(&w, st, 1, MODULE_LASER_FAB, 2, 4);
    bool found_cu = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].commodity == COMMODITY_CUPRITE_INGOT) {
            found_cu = true; break;
        }
    }
    ASSERT(found_cu);
}

TEST(test_module_construction_and_delivery) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1]; /* Kepler */
    int mc_before = st->module_count;
    /* TRACTOR_FAB needs cuprite ingot hopper. */
    add_hopper_for(st, 3, 1, COMMODITY_CUPRITE_INGOT);
    int producer_idx = mc_before + 1;
    begin_module_construction_at(&w, st, 1, MODULE_TRACTOR_FAB, 2, 4);
    ASSERT_EQ_INT(st->module_count, mc_before + 2);
    ASSERT(st->modules[producer_idx].scaffold);
    ASSERT_EQ_INT((int)st->modules[producer_idx].type, (int)MODULE_TRACTOR_FAB);
    /* Deliver the required crystal ingots (goes into station inventory) */
    ship_t ship = {0};
    ship.cargo[COMMODITY_CRYSTAL_INGOT] = 200.0f;
    step_module_delivery(&w, st, 1, &ship, COMMODITY_COUNT);
    ASSERT(ship.cargo[COMMODITY_CRYSTAL_INGOT] < 200.0f);  /* consumed from ship */
    ASSERT_EQ_FLOAT(st->modules[producer_idx].build_progress, 1.0f, 0.01f); /* fully supplied */
    ASSERT(st->modules[producer_idx].scaffold);  /* still building — not instant */
    /* Run sim for 15 seconds (MODULE_BUILD_TIME = 10s + margin) */
    for (int i = 0; i < (int)(15.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    ASSERT(!st->modules[producer_idx].scaffold);  /* activated after build time */
}

/* Regression: a frame delivered into a scaffold via service_sell must
 * also have its matching cargo_unit_t removed from the ship manifest.
 * Without the consume, the named frame stays in the ship's manifest
 * and could be sold or transferred again. */
TEST(test_construction_consumes_manifest_units) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[0];
    st->scaffold = true;
    st->scaffold_progress = 0.0f;

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    memset(sp->session_token, 0xCC, sizeof(sp->session_token));
    sp->docked = true;
    sp->current_station = 0;
    ASSERT(manifest_init(&sp->ship.manifest, 16));
    sp->ship.cargo[COMMODITY_FRAME] = 5.0f;
    cargo_unit_t u = {0};
    u.kind = CARGO_KIND_FRAME;
    u.commodity = COMMODITY_FRAME;
    for (int i = 0; i < 5; i++) {
        u.pub[0] = (uint8_t)(i + 1);
        ASSERT(manifest_push(&sp->ship.manifest, &u));
    }
    ASSERT_EQ_INT(manifest_count_by_commodity(&sp->ship.manifest, COMMODITY_FRAME), 5);

    sp->input.service_sell = true;
    sp->input.service_sell_only = COMMODITY_COUNT;
    world_sim_step(&w, SIM_DT);

    int frames_left = manifest_count_by_commodity(&sp->ship.manifest, COMMODITY_FRAME);
    int cargo_left = (int)floorf(sp->ship.cargo[COMMODITY_FRAME] + 0.0001f);
    ASSERT_EQ_INT(cargo_left, frames_left);
    /* Some frames consumed by the scaffold (it needs them). */
    ASSERT(frames_left < 5);
}

/* Regression: a single buy_product intent must purchase exactly one
 * unit, not as-many-as-the-player-can-afford. The TRADE picker
 * advertises rows as "buy 1 frame for $X"; bulk-buy from one keypress
 * was charging the row's grade-multiplied price across the whole drain. */
TEST(test_docked_buy_one_unit_per_intent) {
    WORLD_DECL;
    world_reset(&w);
    world_seed_station_manifests(&w);
    station_t *st = &w.stations[1]; /* Kepler — produces frames */
    /* Mint manifest entries (and float in lockstep) — manifest is the
     * truth for finished-good BUY availability, so seeding only the
     * float would leave the BUY check reading 0. */
    station_finished_mint(st, COMMODITY_FRAME, 50, NULL);

    server_player_t *sp = &w.players[0];
    sp->connected = true;
    sp->session_ready = true;
    sp->id = 0;
    sp->docked = true;
    sp->current_station = 1;
    memset(sp->session_token, 0xAA, sizeof(sp->session_token));
    ASSERT(manifest_init(&sp->ship.manifest, 16));
    ledger_credit_supply(st, sp->session_token, 5000.0f);
    float bal_before = ledger_balance(st, sp->session_token);
    float cargo_before = sp->ship.cargo[COMMODITY_FRAME];

    sp->input.buy_product = true;
    sp->input.buy_commodity = COMMODITY_FRAME;
    sp->input.buy_grade = MINING_GRADE_COMMON;
    world_sim_step(&w, SIM_DT);

    float cargo_delta = sp->ship.cargo[COMMODITY_FRAME] - cargo_before;
    float bal_delta = bal_before - ledger_balance(st, sp->session_token);
    ASSERT_EQ_FLOAT(cargo_delta, 1.0f, 0.01f);
    ASSERT(bal_delta < 100.0f);
}

/* Regression: world_seed_station_manifests populates each active
 * station's manifest from its float inventory so the manifest-only
 * TRADE picker has rows to surface. The singleplayer init path must
 * call this for parity with the dedicated server. */
TEST(test_world_seed_station_manifests_matches_float) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(w.stations[i].manifest.count, 0);
    }
    world_seed_station_manifests(&w);
    for (int s = 0; s < 3; s++) {
        for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
            int expected = (int)floorf(w.stations[s]._inventory_cache[c] + 0.0001f);
            int got = manifest_count_by_commodity(&w.stations[s].manifest,
                                                  (commodity_t)c);
            ASSERT_EQ_INT(got, expected);
        }
    }
}

TEST(test_module_activation_spawns_npc) {
    WORLD_DECL;
    world_reset(&w);
    int npc_before = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) if (w.npc_ships[i].active) npc_before++;
    /* Build a furnace on Kepler. FURNACE accepts any ore — plant a
     * ferrite ore hopper to satisfy its input. */
    station_t *st = &w.stations[1];
    add_hopper_for(st, 3, 1, COMMODITY_FERRITE_ORE);
    begin_module_construction_at(&w, st, 1, MODULE_FURNACE, 2, 4);
    /* Deliver materials to station inventory */
    ship_t ship = {0};
    ship.cargo[COMMODITY_FRAME] = 200.0f;
    step_module_delivery(&w, st, 1, &ship, COMMODITY_COUNT);
    /* Run sim long enough for construction to complete (~60 frames / 4 per sec = 15s) */
    for (int i = 0; i < (int)(20.0f / SIM_DT); i++)
        world_sim_step(&w, SIM_DT);
    /* A miner should have been spawned on activation */
    int npc_after = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) if (w.npc_ships[i].active) npc_after++;
    ASSERT(npc_after > npc_before);
}

TEST(test_238_station_core_blocks_player) {
    /* Issue 1: player should not fly through station center.
     * Place player on a collision course with station 0 core. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;
    float st_r = w->stations[0].radius; /* 40 */
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius; /* 16 */

    /* Start 200 units away, heading straight at center */
    w->players[0].ship.pos = v2(st_pos.x + 200.0f, st_pos.y);
    w->players[0].ship.vel = v2(-500.0f, 0.0f);

    /* Run 120 ticks (~1 second) */
    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    float dist = v2_len(v2_sub(w->players[0].ship.pos, st_pos));
    float min_allowed = st_r + 4.0f + ship_r;
    /* Player must be outside the core collision boundary */
    ASSERT(dist >= min_allowed - 1.0f);
}

TEST(test_238_module_circle_blocks_player) {
    /* Module collision circles should block the player.
     * Fly directly at the signal relay on ring 1, slot 1 of station 0. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 mod_pos = module_world_pos_ring(&w->stations[0], 1, 1);
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius;

    /* Approach from outside, heading at module */
    vec2 approach_dir = v2_norm(v2_sub(mod_pos, w->stations[0].pos));
    w->players[0].ship.pos = v2_add(mod_pos, v2_scale(approach_dir, 100.0f));
    w->players[0].ship.vel = v2_scale(approach_dir, -400.0f);

    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    float dist = v2_len(v2_sub(w->players[0].ship.pos, mod_pos));
    float min_allowed = 34.0f /* MODULE_COLLISION_RADIUS */ + ship_r;
    ASSERT(dist >= min_allowed - 2.0f);
}

TEST(test_238_corridor_blocks_radial_approach) {
    /* Corridor between relay@1 and furnace@2 on ring 1 of station 0.
     * Dock@0 is skipped, so the relay-furnace corridor should block.
     * Approach radially — should be pushed out. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;

    /* Midpoint angle between slot 1 and slot 2 on ring 1 (accounts for ring_offset) */
    float ang1 = module_angle_ring(&w->stations[0], 1, 1);
    float ang2 = module_angle_ring(&w->stations[0], 1, 2);
    float mid_ang = (ang1 + ang2) * 0.5f;
    float ring_r = 180.0f; /* STATION_RING_RADIUS[1] */

    /* Place player at the ring radius at the corridor midpoint, approaching inward */
    w->players[0].ship.pos = v2_add(st_pos, v2(cosf(mid_ang) * (ring_r + 60.0f), sinf(mid_ang) * (ring_r + 60.0f)));
    vec2 inward = v2_norm(v2_sub(st_pos, w->players[0].ship.pos));
    w->players[0].ship.vel = v2_scale(inward, 300.0f);

    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    /* Player should have been pushed to outer edge of corridor band.
     * Corridor outer edge = ring_r + CORRIDOR_HW + ship_r */
    float dist_from_center = v2_len(v2_sub(w->players[0].ship.pos, st_pos));
    float corridor_hw = 10.0f; /* CORRIDOR_HW */
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius;
    float outer_edge = ring_r + corridor_hw + ship_r;
    /* Player should be at or beyond the outer edge (pushed out) */
    ASSERT(dist_from_center >= outer_edge - 2.0f);
}

TEST(test_238_dock_gap_allows_entry) {
    /* Rings are intentionally always open — the wrap-around corridor
     * is never emitted, so the largest empty arc is the entry gap.
     * Prospect ring 1: dock@0, relay@1, furnace@2. Corridors are
     * dock→relay and relay→furnace. The open gap is from furnace@2
     * (240°) wrapping back to dock@0 (0°/360°), midpoint ~300°. */
    WORLD_HEAP w = setup_collision_world_heap();

    vec2 st_pos = w->stations[0].pos;
    float ring_r = 180.0f; /* STATION_RING_RADIUS[1] */

    float furnace_ang = module_angle_ring(&w->stations[0], 1, 2);
    /* Forward arc from slot 2 around to slot 0 spans (3-2+0)/3 of
     * the circle = 1/3 = 120°. Midpoint sits 60° past furnace. */
    float gap_mid = furnace_ang + (TWO_PI_F / 3.0f) * 0.5f;
    vec2 outside = v2_add(st_pos, v2(cosf(gap_mid) * (ring_r + 80.0f), sinf(gap_mid) * (ring_r + 80.0f)));
    vec2 inside_target = v2_add(st_pos, v2(cosf(gap_mid) * (ring_r - 80.0f), sinf(gap_mid) * (ring_r - 80.0f)));

    w->players[0].ship.pos = outside;
    vec2 dir = v2_norm(v2_sub(inside_target, outside));
    w->players[0].ship.vel = v2_scale(dir, 200.0f);

    for (int i = 0; i < 120; i++)
        world_sim_step(w, SIM_DT);

    float dist_from_center = v2_len(v2_sub(w->players[0].ship.pos, st_pos));
    ASSERT(dist_from_center < ring_r);
}

TEST(test_238_corridor_angular_edge_no_clip) {
    /* Corridor between relay@1 and furnace@2 on ring 1.
     * Approach at the angular edge near the furnace end — should not clip through. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;
    float ring_r = 180.0f; /* STATION_RING_RADIUS[1] */

    /* Furnace at slot 2 on ring 1 — approach from just before its angle */
    float slot2_ang = module_angle_ring(&w->stations[0], 1, 2);
    float test_ang = slot2_ang - 0.02f; /* just inside corridor end */
    w->players[0].ship.pos = v2_add(st_pos, v2(cosf(test_ang) * (ring_r + 50.0f), sinf(test_ang) * (ring_r + 50.0f)));
    vec2 inward = v2_norm(v2_sub(st_pos, w->players[0].ship.pos));
    w->players[0].ship.vel = v2_scale(inward, 300.0f);

    for (int i = 0; i < 60; i++)
        world_sim_step(w, SIM_DT);

    float dist = v2_len(v2_sub(w->players[0].ship.pos, st_pos));
    float ship_r = HULL_DEFS[HULL_CLASS_MINER].ship_radius;
    float outer_edge = ring_r + 10.0f + ship_r;
    ASSERT(dist >= outer_edge - 2.0f);
}

TEST(test_238_module_corridor_junction_no_jitter) {
    /* Place player at the junction between a module circle and a corridor arc.
     * Run 240 ticks. Ship should settle — not oscillate between collision handlers. */
    WORLD_HEAP w = setup_collision_world_heap();
    
    vec2 st_pos = w->stations[0].pos;
    float ring_r = 340.0f;

    /* Stop ring rotation so module positions are stable during test */
    w->stations[0].arm_speed[0] = 0.0f;
    w->stations[0].arm_speed[1] = 0.0f;
    w->stations[0].arm_rotation[0] = 0.0f;
    w->stations[0].arm_rotation[1] = 0.0f;

    /* Furnace at slot 2 on ring 2 — get actual module angle */
    float mod_ang = module_angle_ring(&w->stations[0], 2, 2);
    /* Place ship just corridor-side of the module at the ring radius */
    float junction_ang = mod_ang - 0.05f;
    w->players[0].ship.pos = v2_add(st_pos, v2(cosf(junction_ang) * ring_r, sinf(junction_ang) * ring_r));
    w->players[0].ship.vel = v2(0.0f, 0.0f);

    /* Record position every 30 ticks, check for oscillation */
    vec2 positions[8];
    for (int snap = 0; snap < 8; snap++) {
        positions[snap] = w->players[0].ship.pos;
        for (int i = 0; i < 30; i++)
            world_sim_step(w, SIM_DT);
    }

    /* Check that ship settled — last 4 snapshots should be within 5 units of each other */
    float max_drift = 0.0f;
    for (int i = 5; i < 8; i++) {
        float d = v2_len(v2_sub(positions[i], positions[4]));
        if (d > max_drift) max_drift = d;
    }
    /* FAILS if collision handlers are fighting (ship jitters > 5 units) */
    ASSERT(max_drift < 5.0f);
}

TEST(test_238_invisible_wall_repro) {
    /* The original bug: player flying parallel to a corridor at the inflated
     * collision distance bounces off "nothing visible".
     * Test: fly tangentially just outside the visual corridor width (ring_r + hw)
     * but inside the collision band (ring_r + hw + ship_r). Should collide. */
    WORLD_HEAP w = setup_collision_world_heap();
    /* Suppress chunk materialization so terrain doesn't interfere with collision test */
    w->field_spawn_timer = -9999.0f;

    vec2 st_pos = w->stations[0].pos;
    float ring_r = 340.0f;
    float corridor_hw = 10.0f;
    (void)HULL_DEFS; /* ship_r available if needed */

    /* Midpoint of corridor between slot 1 and slot 2 */
    float mid_ang = TWO_PI_F * 1.5f / 6.0f;
    /* Place at ring_r + corridor_hw + 5 (inside collision band but outside visual) */
    float test_r = ring_r + corridor_hw + 5.0f; /* between visual edge and collision edge */
    w->players[0].ship.pos = v2_add(st_pos, v2(cosf(mid_ang) * test_r, sinf(mid_ang) * test_r));
    /* Fly tangentially (no radial component) */
    vec2 radial = v2_norm(v2_sub(w->players[0].ship.pos, st_pos));
    vec2 tangent = v2(-radial.y, radial.x);
    w->players[0].ship.vel = v2_scale(tangent, 200.0f);

    vec2 start_pos = w->players[0].ship.pos;
    for (int i = 0; i < 60; i++)
        world_sim_step(w, SIM_DT);

    /* The ship is inside the collision band (ring_r+hw+ship_r) but outside
     * the visual corridor (ring_r+hw). This IS the "invisible wall" —
     * the collision is correct (ship has physical radius) but the visual
     * doesn't show it. Verify the collision fires: ship should be pushed outward. */
    float start_r = v2_len(v2_sub(start_pos, st_pos));
    float end_r = v2_len(v2_sub(w->players[0].ship.pos, st_pos));
    /* Ship should have been pushed outward (end_r >= start_r) because it was
     * inside the collision band. If this FAILS, the collision isn't detecting
     * the ship at this distance. */
    ASSERT(end_r >= start_r - 1.0f);
}

TEST(test_station_geom_emitter_prospect) {
    /* Verify the geometry emitter produces correct shapes for Prospect.
     * Cross-ring pair layout:
     *   Ring 1: DOCK(0) + SIGNAL_RELAY(1) + FURNACE(2)
     *   Ring 2: HOPPER(4)  — paired with the ring-1 furnace
     */
    WORLD_HEAP w = setup_collision_world_heap();
    w->rng = 2037u;
    world_reset(w);

    station_geom_t geom;
    station_build_geom(&w->stations[0], &geom);

    /* Core: Prospect has radius 40 */
    ASSERT(geom.has_core == true);

    /* Circles: dock (half-size) + relay + furnace (ring 1) + 1 hopper
     * (ring 2: ferrite-ore intake @ slot 4) = 4. */
    ASSERT_EQ_INT(geom.circle_count, 4);
    /* Corridors: ring 1 = 3 modules → 2 corridors. Ring 2 has only one
     * module so no within-ring corridor. */
    ASSERT_EQ_INT(geom.corridor_count, 2);

    /* Docks: 1 dock on ring 1 */
    ASSERT_EQ_INT(geom.dock_count, 1);
}

TEST(test_scaffold_spawn) {
    WORLD_DECL;
    world_reset(&w);

    /* Spawn a furnace scaffold near station 0 */
    vec2 spawn_pos = v2_add(w.stations[0].pos, v2(100.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, spawn_pos, 0);
    ASSERT(idx >= 0);
    ASSERT(idx < MAX_SCAFFOLDS);
    ASSERT(w.scaffolds[idx].active);
    ASSERT_EQ_INT(w.scaffolds[idx].module_type, MODULE_FURNACE);
    ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_LOOSE);
    ASSERT_EQ_INT(w.scaffolds[idx].owner, 0);
    ASSERT_EQ_INT(w.scaffolds[idx].placed_station, -1);
    ASSERT_EQ_INT(w.scaffolds[idx].towed_by, -1);
    ASSERT(w.scaffolds[idx].radius > 0.0f);

    /* Spawn fills slots until full */
    for (int i = 1; i < MAX_SCAFFOLDS; i++) {
        int s = spawn_scaffold(&w, MODULE_DOCK, spawn_pos, 0);
        ASSERT(s >= 0);
    }
    /* No free slots left */
    int overflow = spawn_scaffold(&w, MODULE_DOCK, spawn_pos, 0);
    ASSERT_EQ_INT(overflow, -1);
}

TEST(test_scaffold_physics_loose) {
    WORLD_DECL;
    world_reset(&w);

    /* Spawn scaffold with initial velocity */
    vec2 spawn_pos = v2_add(w.stations[0].pos, v2(200.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FRAME_PRESS, spawn_pos, 0);
    ASSERT(idx >= 0);
    w.scaffolds[idx].vel = v2(50.0f, 0.0f);

    vec2 start_pos = w.scaffolds[idx].pos;

    /* Run a few sim steps */
    for (int i = 0; i < 120; i++) {
        world_sim_step(&w, SIM_DT);
    }

    /* Scaffold should have moved from its starting position */
    float dist = v2_dist_sq(w.scaffolds[idx].pos, start_pos);
    ASSERT(dist > 1.0f);

    /* Age should have advanced */
    ASSERT(w.scaffolds[idx].age > 0.5f);

    /* Rotation should have advanced */
    ASSERT(w.scaffolds[idx].rotation != 0.0f);
}

TEST(test_scaffold_towed_scaffold_init) {
    WORLD_DECL;
    world_reset(&w);

    /* Player ship should start with no towed scaffold */
    SERVER_PLAYER_DECL(sp);
    sp.connected = true;
    player_init_ship(&sp, &w);
    ASSERT_EQ_INT(sp.ship.towed_scaffold, -1);
}

TEST(test_scaffold_tow_pickup) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;  /* hold R to grab */

    /* Spawn scaffold very close to the player */
    vec2 player_pos = w.players[0].ship.pos;
    vec2 scaffold_pos = v2_add(player_pos, v2(50.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, scaffold_pos, 0);
    ASSERT(idx >= 0);

    /* Run sim — player should pick up the scaffold */
    for (int i = 0; i < 10; i++) world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship.towed_scaffold, idx);
    ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_TOWING);
    ASSERT_EQ_INT(w.scaffolds[idx].towed_by, 0);
}

TEST(test_scaffold_tow_release_on_r) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;  /* hold R to grab */

    /* Spawn and attach scaffold */
    vec2 player_pos = w.players[0].ship.pos;
    int idx = spawn_scaffold(&w, MODULE_FURNACE, v2_add(player_pos, v2(50.0f, 0.0f)), 0);
    ASSERT(idx >= 0);
    for (int i = 0; i < 10; i++) world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(w.players[0].ship.towed_scaffold, idx);

    /* Tap R = release scaffold */
    w.players[0].input.tractor_hold = false;
    w.players[0].input.release_tow = true;
    world_sim_step(&w, SIM_DT);

    ASSERT_EQ_INT(w.players[0].ship.towed_scaffold, -1);
    ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_LOOSE);
    ASSERT_EQ_INT(w.scaffolds[idx].towed_by, -1);
}

TEST(test_scaffold_tow_release_on_dock) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].in_dock_range = false;
    w.players[0].input.tractor_hold = true;

    /* Spawn and manually attach scaffold */
    vec2 near_station = v2_add(w.stations[0].pos, v2(100.0f, 0.0f));
    w.players[0].ship.pos = near_station;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    int idx = spawn_scaffold(&w, MODULE_DOCK, v2_add(near_station, v2(50.0f, 0.0f)), 0);
    ASSERT(idx >= 0);
    /* Manually attach to avoid needing sim steps in dock approach range */
    w.players[0].ship.towed_scaffold = (int16_t)idx;
    w.scaffolds[idx].state = SCAFFOLD_TOWING;
    w.scaffolds[idx].towed_by = 0;

    /* Now dock — scaffold should be released */
    w.players[0].nearby_station = 0;
    w.players[0].in_dock_range = true;
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);

    /* After docking, scaffold should be loose */
    if (w.players[0].docked) {
        ASSERT_EQ_INT(w.players[0].ship.towed_scaffold, -1);
        ASSERT_EQ_INT(w.scaffolds[idx].state, SCAFFOLD_LOOSE);
    }
}

TEST(test_scaffold_tow_speed_cap) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;

    /* Place player far from stations to avoid docking interference */
    w.players[0].ship.pos = v2(5000.0f, 5000.0f);
    w.players[0].ship.vel = v2(200.0f, 0.0f); /* moving fast */

    /* Spawn and manually attach scaffold */
    int idx = spawn_scaffold(&w, MODULE_FURNACE, v2(5050.0f, 5000.0f), 0);
    ASSERT(idx >= 0);
    w.players[0].ship.towed_scaffold = (int16_t)idx;
    w.scaffolds[idx].state = SCAFFOLD_TOWING;
    w.scaffolds[idx].towed_by = 0;

    /* Run sim for a while */
    for (int i = 0; i < 240; i++) world_sim_step(&w, SIM_DT);

    /* Scaffold speed should be capped */
    float spd = v2_len(w.scaffolds[idx].vel);
    ASSERT(spd <= 60.0f); /* slightly above cap due to spring forces in single frame */
}

TEST(test_scaffold_snap_to_slot) {
    WORLD_DECL;
    world_reset(&w);

    /* We need a player outpost (index >= 3). Place one. */
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    
    /* credits are station-local (ledger) — no ship.credits field */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(outpost >= 3);
    /* Activate the outpost so it can accept scaffolds */
    w.stations[outpost].scaffold = false;
    w.stations[outpost].scaffold_progress = 1.0f;
    w.stations[outpost].signal_range = 6000.0f;
    w.stations[outpost].arm_count = 1;
    w.stations[outpost].arm_speed[0] = 0.04f;
    rebuild_signal_chain(&w);

    /* Count existing modules */
    int before_count = w.stations[outpost].module_count;

    /* Spawn a scaffold near ring 1 of the outpost */
    vec2 ring1_near = v2_add(outpost_pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, ring1_near, 0);
    ASSERT(idx >= 0);

    /* Run sim — station should grab it and pull it into a slot */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);

    /* Scaffold should have been consumed (deactivated) */
    ASSERT(!w.scaffolds[idx].active);

    /* Station should have a new module */
    ASSERT(w.stations[outpost].module_count == before_count + 1);

    /* The new module should be a furnace scaffold (under construction) */
    station_module_t *m = &w.stations[outpost].modules[before_count];
    ASSERT_EQ_INT(m->type, MODULE_FURNACE);
    ASSERT(m->scaffold); /* still under construction */
    ASSERT(m->ring >= 1);
}

TEST(test_scaffold_snap_ignores_starter_stations) {
    WORLD_DECL;
    world_reset(&w);

    /* Spawn scaffold near station 0 (starter station, index < 3) */
    vec2 near_prospect = v2_add(w.stations[0].pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, near_prospect, 0);
    ASSERT(idx >= 0);

    /* Run sim */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);

    /* Scaffold should still be active (not grabbed by starter station) */
    ASSERT(w.scaffolds[idx].active);
    ASSERT(w.scaffolds[idx].state != SCAFFOLD_SNAPPING);
}

TEST(test_scaffold_full_pipeline) {
    /* End-to-end: spawn → snap → supply → build timer → activate */
    WORLD_DECL;
    world_reset(&w);

    /* Create and activate a player outpost */
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;

    /* credits are station-local (ledger) — no ship.credits field */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(&w, &w.players[0], outpost_pos);
    ASSERT(outpost >= 3);
    w.stations[outpost].scaffold = false;
    w.stations[outpost].scaffold_progress = 1.0f;
    w.stations[outpost].signal_range = 6000.0f;
    w.stations[outpost].arm_count = 1;
    w.stations[outpost].arm_speed[0] = 0.04f;
    /* Pre-supply founding module scaffolds so they don't compete */
    for (int mi = 0; mi < w.stations[outpost].module_count; mi++) {
        if (w.stations[outpost].modules[mi].scaffold)
            w.stations[outpost].modules[mi].build_progress = 1.0f;
    }
    rebuild_signal_chain(&w);

    int before_count = w.stations[outpost].module_count;

    /* Step 1: Spawn scaffold near ring 1 → station grabs it */
    vec2 ring1_near = v2_add(outpost_pos, v2(180.0f, 0.0f));
    int idx = spawn_scaffold(&w, MODULE_FURNACE, ring1_near, 0);
    ASSERT(idx >= 0);

    /* Run until scaffold is consumed (snapped + placed as module) */
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!w.scaffolds[idx].active);
    ASSERT(w.stations[outpost].module_count == before_count + 1);

    /* Post-placement: module enters supply phase (build_progress = 0) */
    station_module_t *m = &w.stations[outpost].modules[before_count];
    ASSERT_EQ_INT(m->type, MODULE_FURNACE);
    ASSERT(m->scaffold);
    ASSERT(!module_is_fully_supplied(m)); /* in supply phase, not pre-paid */

    /* Step 2: Deliver build material to advance supply phase.
     * Furnaces need frames — deposit into station inventory,
     * step_module_activation will route it to the scaffold. */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    w.stations[outpost]._inventory_cache[mat] = cost;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied, timer may have started */

    /* Step 3: Run construction timer (10s = 1200 ticks at 120Hz) */
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);

    /* Module should be fully activated */
    ASSERT(!m->scaffold);
    ASSERT_EQ_FLOAT(m->build_progress, 1.0f, 0.01f);
}

/* End-to-end: a player plants an outpost via tow, supplies its founding
 * frame quota by docking and dumping cargo, sims through scaffold +
 * seed-module activation, then plants and supplies a second module
 * (furnace). Asserts no credits leak, the outpost becomes dockable, the
 * furnace activates, and the activation spawn loop produces an NPC. */
TEST(test_build_outpost_full_economy) {
    WORLD_DECL;
    world_reset(&w);

    server_player_t *sp = &w.players[0];
    uint8_t token[8] = {0xB1, 0xD9, 0x07, 0x12, 0x33, 0x44, 0x55, 0x66};
    memcpy(sp->session_token, token, 8);
    sp->session_ready = true;
    /* Synthesize a non-zero pubkey so ledger ops use the pubkey path. */
    for (int i = 0; i < 32; i++) sp->pubkey[i] = (uint8_t)(0xA0 + i);
    sp->pubkey_set = true;
    ASSERT(registry_register_pubkey(&w, sp->pubkey, sp->session_token));

    sp->connected = true;
    sp->id = 0;
    player_init_ship(sp, &w);
    sp->docked = false;

    double credits_start = econ_total_credits(&w);

    /* Step 1 — plant an outpost ~6kU east of Prospect via the tow flow.
     * The harness spawns a SIGNAL_RELAY scaffold, attaches it to the
     * player, and trips place_outpost — i.e. exactly what the client
     * does when the player presses E with a relay in tow. */
    vec2 outpost_pos = v2_add(w.stations[0].pos, v2(6000.0f, 0.0f));
    int outpost = test_place_outpost_via_tow(&w, sp, outpost_pos);
    ASSERT(outpost >= 3);
    station_t *st_out = &w.stations[outpost];
    ASSERT(st_out->scaffold);              /* under construction */
    ASSERT(st_out->signal_range > 0.0f);   /* relay seeded its range */
    int seed_mod_count = st_out->module_count; /* DOCK + seed SIGNAL_RELAY */
    ASSERT(seed_mod_count >= 2);

    /* The founding flow opens a CONTRACT_TRACTOR for FRAMES at the
     * outpost; the same flow we'd use to deliver to it. */
    bool found_frame_contract = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active
            && w.contracts[k].station_index == outpost
            && w.contracts[k].commodity == COMMODITY_FRAME) {
            found_frame_contract = true; break;
        }
    }
    ASSERT(found_frame_contract);

    /* Step 2 — load up frames. In a live session the player would have
     * earned credits at Prospect, hauled ingots to Kepler, and bought
     * frames there (covered by the buy-flow tests). Drop them straight
     * into cargo for this test — the value here is what happens *after*
     * the materials reach the outpost. */
    float frame_budget =
        SCAFFOLD_MATERIAL_NEEDED                          /* outpost scaffold */
        + module_build_cost_lookup(MODULE_SIGNAL_RELAY)   /* seed module */
        + module_build_cost_lookup(MODULE_FURNACE)        /* second module */
        + 10.0f;                                          /* slack */
    ASSERT(test_set_ship_finished_units(&sp->ship, COMMODITY_FRAME,
                                        (int)ceilf(frame_budget),
                                        MINING_GRADE_COMMON));

    /* Step 3 — dock at the outpost and pour the frames in.
     * The outpost has an OUTPOST_DOCK module stamped on by
     * place_towed_scaffold so docked-mode is valid here. */
    sp->docked = true;
    sp->current_station = outpost;
    sp->ship.pos = st_out->pos;

    /* service_sell triggers step_scaffold_delivery (advances the
     * station scaffold) and step_module_delivery (advances any module
     * scaffold). Run a few sells back-to-back — each sim step the
     * server clears the intent flag, so we re-arm it. */
    for (int i = 0; i < 10 && st_out->scaffold; i++) {
        sp->input.service_sell = true;
        world_sim_step(&w, SIM_DT);
    }

    /* Step 4 — sim until outpost-level scaffolding finishes. The
     * activation step lifts st->scaffold once scaffold_progress hits
     * 1.0 and the seeded module's frames are in. */
    for (int i = 0; i < 60 * 120 && st_out->scaffold; i++) {
        world_sim_step(&w, SIM_DT);
    }
    ASSERT(!st_out->scaffold);
    ASSERT(st_out->signal_connected); /* tied back into the chain */

    /* Step 5 — wait for the seed SIGNAL_RELAY module to finish its
     * 10s build timer and activate. */
    for (int i = 0; i < 30 * 120; i++) {
        bool any_seed_scaffolded = false;
        for (int m = 0; m < st_out->module_count; m++) {
            if (st_out->modules[m].scaffold) { any_seed_scaffolded = true; break; }
        }
        if (!any_seed_scaffolded) break;
        world_sim_step(&w, SIM_DT);
    }
    for (int m = 0; m < st_out->module_count; m++) {
        ASSERT(!st_out->modules[m].scaffold);
    }

    /* Step 6 — plant a second module: a furnace. Spawn a furnace
     * scaffold near ring 1 and let the snap-to-slot path consume it,
     * the same flow test_scaffold_full_pipeline exercises. */
    int npc_before = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) if (w.npc_ships[i].active) npc_before++;

    sp->docked = false; /* leave so the snap step has clean state */
    vec2 ring1_near = v2_add(outpost_pos, v2(180.0f, 0.0f));
    int sc_idx = spawn_scaffold(&w, MODULE_FURNACE, ring1_near, sp->id);
    ASSERT(sc_idx >= 0);
    int mod_count_pre = st_out->module_count;
    for (int i = 0; i < 600 && w.scaffolds[sc_idx].active; i++) {
        world_sim_step(&w, SIM_DT);
    }
    ASSERT(!w.scaffolds[sc_idx].active);
    ASSERT_EQ_INT(st_out->module_count, mod_count_pre + 1);
    station_module_t *furn = &st_out->modules[mod_count_pre];
    ASSERT_EQ_INT(furn->type, MODULE_FURNACE);
    ASSERT(furn->scaffold);

    /* Step 7 — re-dock and supply the furnace's build material. Ship
     * still has plenty of frames from the budget. */
    sp->docked = true;
    sp->current_station = outpost;
    for (int i = 0; i < 10 && furn->scaffold; i++) {
        sp->input.service_sell = true;
        world_sim_step(&w, SIM_DT);
    }
    /* Build timer: ~10s, plus a comfortable runway for the activation
     * spawn loop to drop in an NPC. */
    for (int i = 0; i < 30 * 120 && furn->scaffold; i++) {
        world_sim_step(&w, SIM_DT);
    }
    ASSERT(!furn->scaffold);
    ASSERT_EQ_FLOAT(furn->build_progress, 1.0f, 0.01f);

    /* Step 8 — activation should have spawned a worker NPC at the
     * outpost (same invariant test_module_activation_spawns_npc proves
     * for in-place builds). */
    int npc_after = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) if (w.npc_ships[i].active) npc_after++;
    ASSERT(npc_after > npc_before);

    /* Step 9 — plant a HOPPER on a ring-1 slot adjacent to the furnace.
     * station_can_smelt requires both a hopper and a furnace, so the
     * hopper completes the smelt prerequisite. The furnace's tag
     * defaults to ferrite (1-furnace tier, module_furnace_default_output);
     * we tag the hopper for FERRITE_ORE explicitly so the post-snap
     * commodity is unambiguous regardless of where the byte zero-init
     * came from. */
    sp->docked = false;
    /* Need slack in cargo: bring the budget for the hopper module up
     * front so the player has frames left to deliver. The earlier
     * frame_budget purposely covered SCAFFOLD_MATERIAL_NEEDED + relay +
     * furnace + 10 slack. The hopper costs more than that 10 slack on
     * its own, so top the cargo back up here — same shortcut as step 2. */
    ASSERT(test_set_ship_finished_units(
        &sp->ship, COMMODITY_FRAME,
        ship_finished_count(&sp->ship, COMMODITY_FRAME) +
            (int)ceilf(module_build_cost_lookup(MODULE_HOPPER)),
        MINING_GRADE_COMMON));

    vec2 ring1_other = v2_add(outpost_pos, v2(-180.0f, 60.0f));
    int hop_idx = spawn_scaffold(&w, MODULE_HOPPER, ring1_other, sp->id);
    ASSERT(hop_idx >= 0);
    int mod_count_pre_hop = st_out->module_count;
    for (int i = 0; i < 600 && w.scaffolds[hop_idx].active; i++) {
        world_sim_step(&w, SIM_DT);
    }
    ASSERT(!w.scaffolds[hop_idx].active);
    ASSERT_EQ_INT(st_out->module_count, mod_count_pre_hop + 1);
    station_module_t *hop = &st_out->modules[mod_count_pre_hop];
    ASSERT_EQ_INT(hop->type, MODULE_HOPPER);
    ASSERT(hop->scaffold);
    /* Tag the hopper for ferrite ore now (the snap path doesn't call
     * add_module_at, so auto_pick_hopper_commodity never runs). */
    hop->commodity = (uint8_t)COMMODITY_FERRITE_ORE;

    sp->docked = true;
    sp->current_station = outpost;
    for (int i = 0; i < 10 && hop->scaffold; i++) {
        sp->input.service_sell = true;
        world_sim_step(&w, SIM_DT);
    }
    for (int i = 0; i < 30 * 120 && hop->scaffold; i++) {
        world_sim_step(&w, SIM_DT);
    }
    ASSERT(!hop->scaffold);
    ASSERT(station_can_smelt(st_out, COMMODITY_FERRITE_ORE));

    /* Step 10 — process ore. Drop raw ferrite ore into the station's
     * bulk inventory and tick the sim; sim_step_refinery_production
     * consumes ore and mints FERRITE_INGOT manifest entries. Rate is
     * REFINERY_BASE_SMELT_RATE per furnace, so a few seconds is plenty
     * to clear our 30-unit stockpile. */
    sp->docked = false;
    sp->input.service_sell = false;
    float ore_in = 30.0f;
    st_out->_inventory_cache[COMMODITY_FERRITE_ORE] = ore_in;
    int ingots_before = manifest_count_by_commodity(&st_out->manifest,
                                                    COMMODITY_FERRITE_INGOT);
    for (int i = 0; i < 30 * 120; i++) {
        world_sim_step(&w, SIM_DT);
        if (st_out->_inventory_cache[COMMODITY_FERRITE_ORE] < 0.5f) break;
    }
    int ingots_after = manifest_count_by_commodity(&st_out->manifest,
                                                   COMMODITY_FERRITE_INGOT);
    ASSERT(ingots_after > ingots_before); /* smelter actually produced */
    ASSERT(st_out->_inventory_cache[COMMODITY_FERRITE_ORE] < ore_in - 1.0f);

    /* Step 11 — credit conservation. The whole pipeline runs through
     * ledger paths; nothing should leak. econ_total_credits sums every
     * station pool plus every player ledger row, so the diff captures
     * any silent mint or burn. */
    double credits_end = econ_total_credits(&w);
    if (fabs(credits_end - credits_start) > 5.0) {
        printf("    credit drift: start=%.2f end=%.2f delta=%.2f\n",
               credits_start, credits_end, credits_end - credits_start);
    }
    ASSERT(fabs(credits_end - credits_start) <= 5.0);
}

TEST(test_scaffold_ship_drag) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    w.players[0].input.tractor_hold = true;
    w.players[0].ship.pos = v2(5000.0f, 5000.0f);
    w.players[0].ship.vel = v2(0.0f, 0.0f);

    /* Spawn and attach scaffold */
    int idx = spawn_scaffold(&w, MODULE_FURNACE, v2(5050.0f, 5000.0f), 0);
    w.players[0].ship.towed_scaffold = (int16_t)idx;
    w.scaffolds[idx].state = SCAFFOLD_TOWING;
    w.scaffolds[idx].towed_by = 0;

    /* Thrust for a while */
    for (int i = 0; i < 600; i++) {
        w.players[0].input.thrust = 1.0f;
        world_sim_step(&w, SIM_DT);
    }

    /* Ship speed should be capped (much slower than free flight). Cap
     * is now engine-coupled — miner accel 300 → tow cap ~82 u/s. */
    float spd = v2_len(w.players[0].ship.vel);
    ASSERT(spd <= 100.0f); /* engine-coupled cap + thrust/drag balance */

    /* Compare to free-flight speed: reset and thrust without scaffold */
    w.players[0].ship.towed_scaffold = -1;
    w.scaffolds[idx].state = SCAFFOLD_LOOSE;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    for (int i = 0; i < 600; i++) {
        w.players[0].input.thrust = 1.0f;
        world_sim_step(&w, SIM_DT);
    }
    float free_spd = v2_len(w.players[0].ship.vel);

    /* Free flight should be significantly faster */
    ASSERT(free_spd > spd * 1.5f);
}

TEST(test_tow_drone_delivers_to_planned_outpost) {
    WORLD_DECL;
    world_reset(&w);
    w.field_spawn_timer = -9999.0f; /* suppress chunk re-materialization */
    w.players[0].connected = true;
    player_init_ship(&w.players[0], &w);
    w.players[0].docked = false;
    /* credits are station-local (ledger) — no ship.credits field */

    /* Create a planned outpost within signal range of station 0 */
    vec2 plan_pos = v2_add(w.stations[0].pos, v2(4000.0f, 0.0f));
    w.players[0].input.create_planned_outpost = true;
    w.players[0].input.planned_outpost_pos = plan_pos;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.create_planned_outpost = false;

    int plan_slot = -1;
    for (int s = 3; s < MAX_STATIONS; s++) {
        if (w.stations[s].planned) { plan_slot = s; break; }
    }
    ASSERT(plan_slot >= 0);

    /* Spawn a loose signal relay near Kepler (station 1) */
    vec2 near_kepler = v2_add(w.stations[1].pos, v2(200.0f, 0.0f));
    int sc_idx = spawn_scaffold(&w, MODULE_SIGNAL_RELAY, near_kepler, 0);
    ASSERT(sc_idx >= 0);
    w.scaffolds[sc_idx].state = SCAFFOLD_LOOSE;
    w.scaffolds[sc_idx].towed_by = -1;

    /* Find Kepler's tow drone */
    int drone_idx = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_TOW
            && w.npc_ships[i].home_station == 1) {
            drone_idx = i; break;
        }
    }
    ASSERT(drone_idx >= 0);
    w.npc_ships[drone_idx].state = NPC_STATE_DOCKED;
    w.npc_ships[drone_idx].state_timer = 0.0f;
    w.npc_ships[drone_idx].ship.pos = w.stations[1].pos;
    /* Slice 13: also seed the paired ship_t so the pre-mirror at the
     * top of step_npc_ships doesn't drag the drone back to its
     * spawn position next tick. */
    {
        ship_t *drone_ship = world_npc_ship_for(&w, drone_idx);
        ASSERT(drone_ship != NULL);
        drone_ship->pos = w.stations[1].pos;
    }

    /* Run up to 30s — wait for drone to grab the scaffold */
    npc_ship_t *drone = &w.npc_ships[drone_idx];
    for (int i = 0; i < 120 * 30 && drone->towed_scaffold < 0; i++)
        world_sim_step(&w, SIM_DT);

    /* Drone must have grabbed the scaffold */
    ASSERT(drone->towed_scaffold >= 0);
    /* Destination must be the planned outpost, not a starter station */
    ASSERT(drone->dest_station >= 3);
    ASSERT_EQ_INT(drone->dest_station, plan_slot);

    /* Run 4 minutes — drone tows at 60 u/s, distance is ~8500u ≈ 142s */
    for (int i = 0; i < 120 * 240; i++) world_sim_step(&w, SIM_DT);

    station_t *outpost = &w.stations[plan_slot];
    bool materialized = (!outpost->planned && outpost->scaffold) ||
                        station_is_active(outpost);
    ASSERT(materialized);
}

TEST(test_save_preserves_pending_scaffolds) {
    /* Save/load round-trip should preserve shipyard pending orders,
     * per-module buffers, and active scaffolds. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);

    /* Add a pending order at Kepler (station 1, has shipyard) */
    w->stations[1].pending_scaffolds[0].type = MODULE_FURNACE;
    w->stations[1].pending_scaffolds[0].owner = 0;
    w->stations[1].pending_scaffold_count = 1;
    /* Some module buffer state */
    w->stations[1].module_input[3] = 42.5f;
    w->stations[1].module_output[5] = 17.0f;
    /* Spawn a nascent scaffold */
    int sidx = spawn_scaffold(w, MODULE_FRAME_PRESS, w->stations[1].pos, 0);
    ASSERT(sidx >= 0);
    w->scaffolds[sidx].state = SCAFFOLD_NASCENT;
    w->scaffolds[sidx].built_at_station = 1;
    w->scaffolds[sidx].build_amount = 17.0f;

    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, TMP("test_pendcat")));
    ASSERT(world_save(w, TMP("test_pending.sav")));

    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, TMP("test_pendcat"));
    ASSERT(world_load(loaded, TMP("test_pending.sav")));

    /* Verify pending order survived (session-tier data) */
    ASSERT_EQ_INT(loaded->stations[1].pending_scaffold_count, 1);
    ASSERT_EQ_INT(loaded->stations[1].pending_scaffolds[0].type, MODULE_FURNACE);
    ASSERT_EQ_INT(loaded->stations[1].pending_scaffolds[0].owner, 0);
    ASSERT_EQ_FLOAT(loaded->stations[1].module_input[3], 42.5f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->stations[1].module_output[5], 17.0f, 0.01f);

    /* Scaffolds are transient in v24 — not persisted in world save.
     * Nascent scaffolds are regenerated from pending orders on restart. */
    (void)sidx;

    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove(TMP("test_pending.sav"));
}

TEST(test_placed_scaffold_supply_phase) {
    /* After snap, module starts at build_progress=0. Delivering material
     * advances it to 1.0, then the 10s build timer runs 1.0 → 2.0. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= 3);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    ASSERT(m->scaffold);
    ASSERT(m->build_progress < 0.01f); /* supply phase start */

    /* Deliver half the material */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    w.stations[outpost]._inventory_cache[mat] = cost * 0.5f;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(m->build_progress > 0.4f && m->build_progress < 0.6f);
    ASSERT(m->scaffold); /* still building */

    /* Deliver the rest */
    w.stations[outpost]._inventory_cache[mat] = cost * 0.5f;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied, timer may have started */

    /* Build timer: 10s = 1200 ticks */
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!m->scaffold);
    ASSERT_EQ_FLOAT(m->build_progress, 1.0f, 0.01f);
}

TEST(test_placed_scaffold_player_delivery) {
    /* A docked player delivering the build material should advance the
     * scaffold's build_progress via step_module_delivery. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= 3);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    ASSERT(m->scaffold);

    /* Dock the player at the outpost with the required cargo */
    w.players[0].docked = true;
    w.players[0].current_station = outpost;
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    w.players[0].ship.cargo[mat] = cost;

    /* Trigger sell action — step_module_delivery pulls from cargo */
    w.players[0].input.service_sell = true;
    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_FLOAT(m->build_progress, 1.0f, 0.01f);
    ASSERT(w.players[0].ship.cargo[mat] < 0.01f); /* cargo consumed */
}

TEST(test_construction_contract_closes_on_activation) {
    /* When the scaffold module activates, any supply contract at
     * this station for the build material should close. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= 3);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);

    /* There should be a supply contract for this station+material */
    bool found_contract = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            found_contract = true; break;
        }
    }
    ASSERT(found_contract);

    /* Supply and activate */
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    w.stations[outpost]._inventory_cache[mat] = cost;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied */
    /* Run build timer */
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!m->scaffold); /* activated */

    /* Contract should now be closed */
    bool contract_alive = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            contract_alive = true; break;
        }
    }
    ASSERT(!contract_alive);
}

TEST(test_stale_contract_does_not_block_next_need) {
    /* After a construction contract completes, the station should be
     * able to generate its next need contract (e.g. ore hopper). */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= 3);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];

    /* Supply, build, activate */
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);
    w.stations[outpost]._inventory_cache[mat] = cost;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    for (int i = 0; i < 2400; i++) world_sim_step(&w, SIM_DT);
    ASSERT(!m->scaffold);

    /* Run a few more ticks for step_contracts to generate the next need */
    for (int i = 0; i < 240; i++) world_sim_step(&w, SIM_DT);

    /* The station should be able to post a new contract (not blocked).
     * A furnace station needs ore — check if any contract exists or
     * at least that no stale construction contract is blocking. */
    bool stale_construction = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            /* A supply contract for the build material should not linger */
            stale_construction = true; break;
        }
    }
    ASSERT(!stale_construction);
}

TEST(test_construction_contract_checks_scaffold_not_threshold) {
    /* A construction supply contract should NOT close based on the
     * 80% station inventory threshold — it should stay open while
     * the scaffold still needs material, regardless of inventory level. */
    WORLD_DECL;
    world_reset(&w);
    int mod_idx;
    int outpost = test_setup_placed_scaffold(&w, &mod_idx);
    ASSERT(outpost >= 3);
    station_module_t *m = &w.stations[outpost].modules[mod_idx];
    commodity_t mat = module_build_material_lookup(MODULE_FURNACE);
    float cost = module_build_cost_lookup(MODULE_FURNACE);

    /* Deliver a partial amount (not enough to fully supply).
     * But make the station inventory exceed the 80% generic threshold
     * by adding a different commodity that fills the buffer. */
    w.stations[outpost]._inventory_cache[mat] = cost * 0.3f; /* partial supply */
    world_sim_step(&w, SIM_DT);

    /* After one tick, step_module_activation routed the partial amount
     * into the scaffold. Scaffold is partially supplied, not full. */
    ASSERT(m->build_progress > 0.2f && m->build_progress < 0.4f);
    ASSERT(m->scaffold);

    /* Contract must still be open — scaffold isn't fully supplied */
    bool contract_alive = false;
    for (int k = 0; k < MAX_CONTRACTS; k++) {
        if (w.contracts[k].active && w.contracts[k].action == CONTRACT_TRACTOR
            && w.contracts[k].station_index == outpost && w.contracts[k].commodity == mat) {
            contract_alive = true; break;
        }
    }
    ASSERT(contract_alive);

    /* Now deliver the rest */
    w.stations[outpost]._inventory_cache[mat] = cost;
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);
    ASSERT(module_is_fully_supplied(m)); /* fully supplied */
}

/* #307: module build state helpers — verify the lifecycle predicates
 * agree with the underlying float convention without leaking it. */
TEST(test_module_build_state_lifecycle) {
    station_module_t m = {0};
    /* Active scaffold, no supply yet. */
    m.scaffold = true;
    m.build_progress = 0.0f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_AWAITING_SUPPLY);
    ASSERT(!module_is_complete(&m));
    ASSERT(!module_is_fully_supplied(&m));
    ASSERT_EQ_FLOAT(module_supply_fraction(&m), 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 0.0f, 0.001f);

    /* Half supplied. */
    m.build_progress = 0.5f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_AWAITING_SUPPLY);
    ASSERT_EQ_FLOAT(module_supply_fraction(&m), 0.5f, 0.001f);

    /* Just hit full supply — moves to BUILDING. */
    m.build_progress = 1.0f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_BUILDING);
    ASSERT(module_is_fully_supplied(&m));
    ASSERT(!module_is_complete(&m));
    ASSERT_EQ_FLOAT(module_supply_fraction(&m), 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 0.0f, 0.001f);

    /* Mid-build. */
    m.build_progress = 1.5f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_BUILDING);
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 0.5f, 0.001f);

    /* Activated — scaffold cleared, build_progress reset to 1.0. */
    m.scaffold = false;
    m.build_progress = 1.0f;
    ASSERT_EQ_INT(module_build_state(&m), MODULE_BUILD_COMPLETE);
    ASSERT(module_is_complete(&m));
    ASSERT(module_is_fully_supplied(&m));
    ASSERT_EQ_FLOAT(module_build_timer_fraction(&m), 1.0f, 0.001f);
}

TEST(test_module_schema_basic_kinds) {
    /* Each kind classification should be consistent */
    ASSERT_EQ_INT(module_kind(MODULE_DOCK), MODULE_KIND_SERVICE);
    ASSERT_EQ_INT(module_kind(MODULE_REPAIR_BAY), MODULE_KIND_SERVICE);
    ASSERT_EQ_INT(module_kind(MODULE_SIGNAL_RELAY), MODULE_KIND_SERVICE);
    ASSERT_EQ_INT(module_kind(MODULE_FURNACE), MODULE_KIND_PRODUCER);
    ASSERT_EQ_INT(module_kind(MODULE_FRAME_PRESS), MODULE_KIND_PRODUCER);
    ASSERT_EQ_INT(module_kind(MODULE_LASER_FAB), MODULE_KIND_PRODUCER);
    ASSERT_EQ_INT(module_kind(MODULE_HOPPER), MODULE_KIND_STORAGE);
    ASSERT_EQ_INT(module_kind(MODULE_SHIPYARD), MODULE_KIND_SHIPYARD);
}

TEST(test_module_schema_producer_io) {
    /* Producers expose their primary input and output commodity. */
    /* Furnace exposes its primary (ferrite) recipe in the schema; the
     * cuprite/crystal tiers live in the runtime sim_can_smelt rules,
     * not the static schema. */
    ASSERT_EQ_INT(module_schema_input(MODULE_FURNACE), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(module_schema_output(MODULE_FURNACE), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_schema_input(MODULE_FRAME_PRESS), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_schema_output(MODULE_FRAME_PRESS), COMMODITY_FRAME);
    ASSERT_EQ_INT(module_schema_input(MODULE_LASER_FAB), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(module_schema_output(MODULE_LASER_FAB), COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(module_schema_input(MODULE_TRACTOR_FAB), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(module_schema_output(MODULE_TRACTOR_FAB), COMMODITY_TRACTOR_MODULE);
    /* Services have no input/output */
    ASSERT_EQ_INT(module_schema_input(MODULE_DOCK), COMMODITY_COUNT);
    ASSERT_EQ_INT(module_schema_output(MODULE_DOCK), COMMODITY_COUNT);
}

TEST(test_module_schema_required_output) {
    /* Slice 1 — every non-shipyard producer declares a single output
     * commodity at the schema level. SHIPYARD is exempt (output is a
     * physical scaffold, not a commodity). */
    ASSERT_EQ_INT(module_required_output(MODULE_FURNACE),     COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_required_output(MODULE_FRAME_PRESS), COMMODITY_FRAME);
    ASSERT_EQ_INT(module_required_output(MODULE_LASER_FAB),   COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(module_required_output(MODULE_TRACTOR_FAB), COMMODITY_TRACTOR_MODULE);
    ASSERT_EQ_INT(module_required_output(MODULE_SHIPYARD),    COMMODITY_COUNT);
    ASSERT_EQ_INT(module_required_output(MODULE_DOCK),        COMMODITY_COUNT);
    ASSERT_EQ_INT(module_required_output(MODULE_HOPPER),      COMMODITY_COUNT);
}

TEST(test_module_furnace_instance_tag) {
    /* Furnace output follows the per-instance commodity tag. Untagged
     * (legacy COMMODITY_COUNT) falls back to FERRITE_INGOT. Each ingot
     * tag implies a matching input ore. Non-furnace producers ignore
     * the tag and read schema. */
    station_module_t m = { .type = MODULE_FURNACE, .commodity = (uint8_t)COMMODITY_COUNT };
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_FERRITE_ORE);

    m.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_CUPRITE_ORE);

    m.commodity = (uint8_t)COMMODITY_CRYSTAL_INGOT;
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_CRYSTAL_ORE);

    /* Garbage tag → fallback to default. */
    m.commodity = (uint8_t)COMMODITY_FRAME;
    ASSERT_EQ_INT(module_instance_output(&m),    COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(module_instance_input_ore(&m), COMMODITY_FERRITE_ORE);

    /* Frame press: instance output ignores tag, uses schema. */
    station_module_t fp = { .type = MODULE_FRAME_PRESS, .commodity = (uint8_t)COMMODITY_LASER_MODULE };
    ASSERT_EQ_INT(module_instance_output(&fp),    COMMODITY_FRAME);
    ASSERT_EQ_INT(module_instance_input_ore(&fp), COMMODITY_COUNT); /* not a furnace */
}

TEST(test_commodity_ore_ingot_pairing) {
    /* Round-trip: ingot ↔ ore. Non-pairs return COMMODITY_COUNT. */
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_FERRITE_ORE), COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_CUPRITE_ORE), COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_CRYSTAL_ORE), COMMODITY_CRYSTAL_INGOT);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_FERRITE_INGOT), COMMODITY_FERRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_CUPRITE_INGOT), COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_CRYSTAL_INGOT), COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_INT(commodity_ingot_for_ore(COMMODITY_FRAME),         COMMODITY_COUNT);
    ASSERT_EQ_INT(commodity_ore_for_ingot(COMMODITY_LASER_MODULE),  COMMODITY_COUNT);
}

TEST(test_station_module_layout_status_missing_output) {
    /* Synthetic station: TRACTOR_FAB with input hopper, AND a SHIPYARD
     * on the station that consumes TRACTOR_MODULE — so the TRACTOR_FAB
     * has a local downstream consumer and an output hopper IS required.
     * Without the hopper → MISSING_OUTPUT_HOPPER. Adding it restores OK. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 2, 0, COMMODITY_CUPRITE_INGOT);
    add_module_at(&st, MODULE_TRACTOR_FAB, 2, 1);
    add_module_at(&st, MODULE_SHIPYARD,    2, 5);   /* downstream consumer */
    /* Shipyard also needs FRAME and LASER_MODULE input hoppers to be OK
     * for itself, but we're testing the TRACTOR_FAB module specifically. */
    add_hopper_for(&st, 3, 0, COMMODITY_FRAME);
    add_hopper_for(&st, 3, 1, COMMODITY_LASER_MODULE);
    const station_module_t *fab = &st.modules[1];   /* TRACTOR_FAB */
    ASSERT_EQ_INT(station_module_layout_status(&st, fab),
                  STATION_LAYOUT_MISSING_OUTPUT_HOPPER);
    add_hopper_for(&st, 2, 2, COMMODITY_TRACTOR_MODULE);
    ASSERT_EQ_INT(station_module_layout_status(&st, fab), STATION_LAYOUT_OK);
}

TEST(test_station_module_layout_status_no_local_consumer_is_ok) {
    /* The mirror case: a producer with NO local downstream consumer
     * doesn't need an output hopper. Smelted ingots ride out via
     * haulers from station inventory. Models Prospect's furnace —
     * 1-furnace ferrite station with no on-station frame press. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 2, 0, COMMODITY_FERRITE_ORE);   /* input only */
    add_furnace_for(&st, 2, 1, COMMODITY_FERRITE_INGOT);
    const station_module_t *furnace = &st.modules[1];
    /* No FRAME_PRESS / LASER_FAB / TRACTOR_FAB on the station, so
     * nothing locally consumes ferrite ingots. Layout is OK without
     * an output hopper. */
    ASSERT_EQ_INT(station_module_layout_status(&st, furnace),
                  STATION_LAYOUT_OK);
}

TEST(test_station_module_layout_status_furnace_uses_tag) {
    /* A furnace tagged for CUPRITE_INGOT needs CUPRITE_ORE in (not any
     * ore) and CUPRITE_INGOT out (because we add a TRACTOR_FAB to give
     * the cuprite ingot a local downstream consumer). FERRITE_ORE alone
     * is missing-input. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 2, 0, COMMODITY_FERRITE_ORE); /* wrong ore for a CU furnace */
    add_furnace_for(&st, 2, 1, COMMODITY_CUPRITE_INGOT);
    add_module_at(&st, MODULE_TRACTOR_FAB, 2, 5);     /* consumes CUPRITE_INGOT */
    const station_module_t *fc = &st.modules[1];     /* the furnace */
    ASSERT_EQ_INT(station_module_layout_status(&st, fc),
                  STATION_LAYOUT_MISSING_INPUT_HOPPER);
    add_hopper_for(&st, 2, 2, COMMODITY_CUPRITE_ORE);
    ASSERT_EQ_INT(station_module_layout_status(&st, fc),
                  STATION_LAYOUT_MISSING_OUTPUT_HOPPER);
    add_hopper_for(&st, 2, 3, COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(station_module_layout_status(&st, fc), STATION_LAYOUT_OK);
}

TEST(test_seeded_stations_layout_ok) {
    /* Slice 1 — every producer module on every seeded station reports
     * STATION_LAYOUT_OK (i.e., its inputs and output have matching
     * tagged hoppers, except SHIPYARD which is exempt from the output
     * rule). This is the end-state validator on a fresh world. */
    WORLD_DECL;
    world_reset(&w);
    for (int s = 0; s < 3; s++) {
        const station_t *st = &w.stations[s];
        for (int i = 0; i < st->module_count; i++) {
            const station_module_t *m = &st->modules[i];
            if (m->scaffold) continue;
            if (!module_is_producer(m->type) && !module_is_shipyard(m->type)) continue;
            station_layout_status_t status = station_module_layout_status(st, m);
            if (status != STATION_LAYOUT_OK) {
                printf("station %d (%s) module %d (type=%d, commodity=%u) layout status %d\n",
                       s, st->name, i, m->type, m->commodity, status);
            }
            ASSERT_EQ_INT(status, STATION_LAYOUT_OK);
        }
    }
}

TEST(test_seeded_furnaces_tagged) {
    /* Slice 1 — seeded stations tag every furnace with its output ingot.
     * Prospect runs ferrite tier (1 furnace → FERRITE_INGOT). Helios runs
     * the 3-furnace tier and tags 2× CUPRITE_INGOT + 1× CRYSTAL_INGOT. */
    WORLD_DECL;
    world_reset(&w);
    int prospect_furnaces = 0;
    for (int i = 0; i < w.stations[0].module_count; i++) {
        if (w.stations[0].modules[i].type != MODULE_FURNACE) continue;
        if (w.stations[0].modules[i].scaffold) continue;
        prospect_furnaces++;
        ASSERT_EQ_INT((int)w.stations[0].modules[i].commodity,
                      (int)COMMODITY_FERRITE_INGOT);
    }
    ASSERT_EQ_INT(prospect_furnaces, 1);

    int helios_cu = 0, helios_cr = 0;
    for (int i = 0; i < w.stations[2].module_count; i++) {
        if (w.stations[2].modules[i].type != MODULE_FURNACE) continue;
        if (w.stations[2].modules[i].scaffold) continue;
        commodity_t tag = (commodity_t)w.stations[2].modules[i].commodity;
        if (tag == COMMODITY_CUPRITE_INGOT) helios_cu++;
        else if (tag == COMMODITY_CRYSTAL_INGOT) helios_cr++;
        else ASSERT(false /* unexpected Helios furnace tag */);
    }
    ASSERT_EQ_INT(helios_cu, 2);
    ASSERT_EQ_INT(helios_cr, 1);
}

TEST(test_seeded_helios_output_hoppers) {
    /* Helios's LASER_FAB and TRACTOR_FAB each have a dedicated
     * commodity-tagged output hopper on ring 3. */
    WORLD_DECL;
    world_reset(&w);
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_LASER_MODULE)   >= 0);
    ASSERT(station_find_hopper_for(&w.stations[2], COMMODITY_TRACTOR_MODULE) >= 0);
    /* All Helios producers report OK under the new layout rule. */
    for (int i = 0; i < w.stations[2].module_count; i++) {
        const station_module_t *m = &w.stations[2].modules[i];
        if (m->scaffold) continue;
        if (!module_is_producer(m->type) && !module_is_shipyard(m->type)) continue;
        station_layout_status_t s = station_module_layout_status(&w.stations[2], m);
        ASSERT_EQ_INT(s, STATION_LAYOUT_OK);
    }
}

TEST(test_station_module_layout_status_shipyard_exempt) {
    /* SHIPYARD output is a physical scaffold body, not a commodity —
     * so it doesn't need an output hopper. With its 3 input hoppers
     * present (frame, laser, tractor module), layout is OK. */
    station_t st = {0};
    st.signal_range = 1.0f;
    add_hopper_for(&st, 3, 0, COMMODITY_FRAME);
    add_hopper_for(&st, 3, 1, COMMODITY_LASER_MODULE);
    add_hopper_for(&st, 3, 2, COMMODITY_TRACTOR_MODULE);
    add_module_at(&st, MODULE_SHIPYARD, 3, 3);
    const station_module_t *sy = &st.modules[st.module_count - 1];
    ASSERT_EQ_INT(station_module_layout_status(&st, sy), STATION_LAYOUT_OK);
}

TEST(test_module_schema_valid_rings) {
    /* Service modules can go anywhere */
    ASSERT(module_valid_on_ring(MODULE_DOCK, 0));
    ASSERT(module_valid_on_ring(MODULE_DOCK, 1));
    ASSERT(module_valid_on_ring(MODULE_DOCK, 3));
    ASSERT(module_valid_on_ring(MODULE_SIGNAL_RELAY, 0));
    ASSERT(module_valid_on_ring(MODULE_SIGNAL_RELAY, 2));
    /* Outer-only modules reject ring 0 */
    ASSERT(!module_valid_on_ring(MODULE_FURNACE, 0));
    ASSERT(module_valid_on_ring(MODULE_FURNACE, 1));
    ASSERT(module_valid_on_ring(MODULE_FURNACE, 3));
    /* Industrial modules need ring 2+ */
    ASSERT(!module_valid_on_ring(MODULE_FRAME_PRESS, 1));
    ASSERT(module_valid_on_ring(MODULE_FRAME_PRESS, 2));
    ASSERT(module_valid_on_ring(MODULE_FRAME_PRESS, 3));
    ASSERT(!module_valid_on_ring(MODULE_SHIPYARD, 1));
    ASSERT(module_valid_on_ring(MODULE_SHIPYARD, 2));
}

TEST(test_module_schema_helpers) {
    /* Boolean kind helpers */
    ASSERT(module_is_producer(MODULE_FURNACE));
    ASSERT(module_is_producer(MODULE_FRAME_PRESS));
    ASSERT(!module_is_producer(MODULE_DOCK));
    ASSERT(module_is_service(MODULE_DOCK));
    ASSERT(module_is_service(MODULE_REPAIR_BAY));
    ASSERT(!module_is_service(MODULE_FURNACE));
    ASSERT(module_is_storage(MODULE_HOPPER));
    ASSERT(module_is_shipyard(MODULE_SHIPYARD));
    ASSERT(!module_is_dead(MODULE_FURNACE));
}

TEST(test_module_schema_build_costs_match) {
    /* Schema build costs match the existing lookup helpers for ALL
     * non-dead modules. Once production code starts reading from the
     * schema in commit 2, drift would change behavior — this test
     * catches it. */
    for (int t = 0; t < MODULE_COUNT; t++) {
        if (module_is_dead((module_type_t)t)) continue;
        const module_schema_t *s = module_schema((module_type_t)t);
        ASSERT_EQ_FLOAT(s->build_material,
                        module_build_cost_lookup((module_type_t)t), 0.01f);
        ASSERT_EQ_INT(s->build_commodity,
                      module_build_material_lookup((module_type_t)t));
        ASSERT_EQ_INT(s->order_fee,
                      scaffold_order_fee((module_type_t)t));
    }
}

TEST(test_module_flow_same_ring_transfer) {
    /* Furnace produces ferrite ingots into output_buffer.
     * Frame Press accepts ferrite ingots as input.
     * On the same ring, material should flow at ~5/sec adjacent. */
    WORLD_DECL;
    world_reset(&w);
    /* Use Kepler (station 1) which has both furnace logic and ring layout.
     * Find a furnace and a frame press by index. */
    int furnace_idx = -1, press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS && press_idx < 0)
            press_idx = i;
    }
    /* Manually create a furnace adjacent to the press if not present */
    if (press_idx >= 0 && furnace_idx < 0) {
        /* Add a furnace at the same ring as the press */
        if (w.stations[1].module_count < MAX_MODULES_PER_STATION) {
            int idx = w.stations[1].module_count++;
            w.stations[1].modules[idx].type = MODULE_FURNACE;
            w.stations[1].modules[idx].ring = w.stations[1].modules[press_idx].ring;
            w.stations[1].modules[idx].slot = (uint8_t)
                ((w.stations[1].modules[press_idx].slot + 1)
                 % STATION_RING_SLOTS[w.stations[1].modules[press_idx].ring]);
            w.stations[1].modules[idx].scaffold = false;
            w.stations[1].modules[idx].build_progress = 1.0f;
            furnace_idx = idx;
        }
    }
    if (furnace_idx < 0 || press_idx < 0) return; /* setup failed, skip */

    /* Seed the furnace's output with ferrite ingots */
    w.stations[1].module_output[furnace_idx] = 10.0f;
    w.stations[1].module_input[press_idx] = 0.0f;

    /* Run one full second of sim */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);

    /* Material should have moved from furnace output to press input.
     * The press now actively consumes from its input buffer to produce
     * frames at a 2-ingot recipe, so we just check that flow happened
     * and some downstream work is visible. */
    ASSERT(w.stations[1].module_output[furnace_idx] < 10.0f);
    ASSERT(w.stations[1].module_input[press_idx] > 0.0f ||
           w.stations[1].module_output[press_idx] > 0.0f ||
           w.stations[1]._inventory_cache[COMMODITY_FRAME] > 0.0f);
}

TEST(test_module_flow_production_fills_buffers) {
    /* Real production should fill module output buffers (mirrored from
     * station inventory). Run sim with seeded ore at Kepler and verify
     * the frame press output buffer fills. */
    WORLD_DECL;
    world_reset(&w);
    /* Seed Kepler with frame-press input */
    w.stations[1]._inventory_cache[COMMODITY_FERRITE_INGOT] = 50.0f;
    float frames_before = w.stations[1]._inventory_cache[COMMODITY_FRAME];
    /* Find frame press */
    int press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i; break;
        }
    }
    if (press_idx < 0) return;

    /* Run a few seconds of sim — production should mirror to output buffer */
    for (int i = 0; i < 240; i++) world_sim_step(&w, SIM_DT);

    /* Production should have pulled ferrite into the chain, and either
     * buffered or stocked some downstream result. */
    ASSERT(w.stations[1]._inventory_cache[COMMODITY_FERRITE_INGOT] < 50.0f);
    ASSERT(w.stations[1].module_input[press_idx] > 0.0f ||
           w.stations[1].module_output[press_idx] > 0.0f ||
           w.stations[1]._inventory_cache[COMMODITY_FRAME] > frames_before);
}

TEST(test_module_flow_does_not_overflow_capacity) {
    /* Material should never exceed buffer capacity at the consumer. */
    WORLD_DECL;
    world_reset(&w);
    int furnace_idx = -1, press_idx = -1;
    for (int i = 0; i < w.stations[1].module_count; i++) {
        if (w.stations[1].modules[i].type == MODULE_FRAME_PRESS) press_idx = i;
    }
    if (press_idx < 0) return;
    if (w.stations[1].module_count < MAX_MODULES_PER_STATION) {
        int idx = w.stations[1].module_count++;
        w.stations[1].modules[idx].type = MODULE_FURNACE;
        w.stations[1].modules[idx].ring = w.stations[1].modules[press_idx].ring;
        w.stations[1].modules[idx].slot = (uint8_t)
            ((w.stations[1].modules[press_idx].slot + 1)
             % STATION_RING_SLOTS[w.stations[1].modules[press_idx].ring]);
        furnace_idx = idx;
    }
    if (furnace_idx < 0) return;

    /* Seed a huge amount of output, run for many ticks */
    w.stations[1].module_output[furnace_idx] = 1000.0f;
    for (int i = 0; i < 600; i++) world_sim_step(&w, SIM_DT);

    /* Press input must not exceed its capacity */
    float cap = module_buffer_capacity(MODULE_FRAME_PRESS);
    ASSERT(w.stations[1].module_input[press_idx] <= cap + 0.01f);
}

/* #280: storage modules must participate in flow as buffers, not be
 * pure sinks. Place a hopper next to a furnace, seed station inventory
 * with raw ferrite, and verify the hopper pulls from inventory and
 * pushes into the furnace's input buffer. */
TEST(test_module_flow_storage_feeds_consumer) {
    WORLD_DECL;
    world_reset(&w);

    /* Use Prospect (station 0): it has a hopper as part of its default
     * layout, plus a furnace. Find them. */
    int hopper_idx = -1, furnace_idx = -1;
    for (int i = 0; i < w.stations[0].module_count; i++) {
        if (w.stations[0].modules[i].type == MODULE_HOPPER && hopper_idx < 0)
            hopper_idx = i;
        if (w.stations[0].modules[i].type == MODULE_FURNACE && furnace_idx < 0)
            furnace_idx = i;
    }
    if (hopper_idx < 0 || furnace_idx < 0) return; /* layout drift, skip */

    /* Seed the hopper-side: put raw ferrite in station inventory and
     * verify it actually moves into the flow graph. */
    w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] = 50.0f;
    w.stations[0].module_output[hopper_idx] = 0.0f;
    w.stations[0].module_input[furnace_idx] = 0.0f;
    float ore_before = w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE];

    /* One second of sim — hopper should refill its output from inventory
     * and the flow stepper should push it onward. */
    for (int i = 0; i < 120; i++) world_sim_step(&w, SIM_DT);

    /* Either the hopper buffer carries ore, the furnace input does, or
     * the furnace already smelted some into ingots. Any of those means
     * storage→flow is connected. */
    bool flowed = w.stations[0].module_output[hopper_idx] > 0.0f
               || w.stations[0].module_input[furnace_idx] > 0.0f
               || w.stations[0]._inventory_cache[COMMODITY_FERRITE_INGOT] > 0.0f
               || w.stations[0]._inventory_cache[COMMODITY_FERRITE_ORE] < ore_before - 0.5f;
    ASSERT(flowed);
}

void register_construction_outposts_tests(void) {
    TEST_SECTION("\nStation construction (#83):\n");
    RUN(test_outpost_requires_signal_range);
    RUN(test_outpost_extends_signal_range);
    RUN(test_disconnected_station_goes_dark);
    RUN(test_outpost_requires_undocked);
    RUN(test_outpost_requires_towed_scaffold);
    RUN(test_outpost_min_distance);
}

void register_construction_modules_tests(void) {
    TEST_SECTION("\nModule construction:\n");
    RUN(test_module_build_material_types);
    RUN(test_module_construction_and_delivery);
    RUN(test_construction_consumes_manifest_units);
    RUN(test_docked_buy_one_unit_per_intent);
    RUN(test_world_seed_station_manifests_matches_float);
    RUN(test_module_activation_spawns_npc);
}

void register_construction_collision238_tests(void) {
    TEST_SECTION("\nCollision accuracy (#238):\n");
    RUN(test_238_station_core_blocks_player);
    RUN(test_238_module_circle_blocks_player);
    RUN(test_238_corridor_blocks_radial_approach);
    RUN(test_238_dock_gap_allows_entry);
    RUN(test_238_corridor_angular_edge_no_clip);
    RUN(test_238_module_corridor_junction_no_jitter);
    RUN(test_238_invisible_wall_repro);
}

void register_construction_station_geom_tests(void) {
    TEST_SECTION("\nStation geometry emitter:\n");
    RUN(test_station_geom_emitter_prospect);
}

void register_construction_scaffold_tests(void) {
    TEST_SECTION("\nScaffold entity (#277):\n");
    RUN(test_scaffold_spawn);
    RUN(test_scaffold_physics_loose);
    RUN(test_scaffold_towed_scaffold_init);
    RUN(test_scaffold_tow_pickup);
    RUN(test_scaffold_tow_release_on_r);
    RUN(test_scaffold_tow_release_on_dock);
    RUN(test_scaffold_tow_speed_cap);
    RUN(test_scaffold_snap_to_slot);
    RUN(test_scaffold_snap_ignores_starter_stations);
    RUN(test_scaffold_full_pipeline);
    RUN(test_build_outpost_full_economy);
    RUN(test_scaffold_ship_drag);
    RUN(test_tow_drone_delivers_to_planned_outpost);
    RUN(test_save_preserves_pending_scaffolds);
}

void register_construction_placed_scaffold_tests(void) {
    TEST_SECTION("\nPlaced-scaffold supply (#277):\n");
    RUN(test_placed_scaffold_supply_phase);
    RUN(test_placed_scaffold_player_delivery);
    RUN(test_construction_contract_closes_on_activation);
    RUN(test_stale_contract_does_not_block_next_need);
    RUN(test_construction_contract_checks_scaffold_not_threshold);
}

TEST(test_pair_neighbors_geometry) {
    /* Cross-ring pair geometry — slot angles map across adjacent rings.
     * Producer on ring N at slot S → closest-angle slot on ring N±1.
     * Tie-break: lower slot index wins. */
    station_slot_pair_t out[2];

    /* Ring 1 has only ring 2 as a neighbor. */
    int n = station_pair_neighbors(1, 0, out);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0].ring, 2);
    ASSERT_EQ_INT(out[0].slot, 0);  /* 0° → ring-2 slot 0 (0°) */

    n = station_pair_neighbors(1, 2, out);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0].ring, 2);
    ASSERT_EQ_INT(out[0].slot, 4);  /* 240° → ring-2 slot 4 (240°) */

    /* Ring 3 has only ring 2 as a neighbor. */
    n = station_pair_neighbors(3, 6, out);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(out[0].ring, 2);
    ASSERT_EQ_INT(out[0].slot, 4);  /* 240° → ring-2 slot 4 (240°) */

    /* Ring 2 has both ring 1 and ring 3 as neighbors. Outer first. */
    n = station_pair_neighbors(2, 0, out);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(out[0].ring, 3);
    ASSERT_EQ_INT(out[0].slot, 0);
    ASSERT_EQ_INT(out[1].ring, 1);
    ASSERT_EQ_INT(out[1].slot, 0);

    /* Tie-break: ring-2 slot 1 (60°) is equidistant from ring-1 slot 0
     * (0°, 60° off) and slot 1 (120°, 60° off) — strict-less-than picks
     * the lower index, slot 0. Ring-3 slot 1 (40°, 20° off) and slot 2
     * (80°, 20° off) tie — picks slot 1. */
    n = station_pair_neighbors(2, 1, out);
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(out[0].ring, 3);
    ASSERT_EQ_INT(out[0].slot, 1);
    ASSERT_EQ_INT(out[1].ring, 1);
    ASSERT_EQ_INT(out[1].slot, 0);
}

TEST(test_pair_satisfied_cross_ring) {
    /* Producer pair-validation under the commodity-tagged hopper
     * model: a producer is satisfied when ALL its required input
     * commodities have a tagged hopper somewhere on the station.
     * For LASER_FAB that means BOTH cuprite ingot AND crystal ingot
     * hoppers must exist. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);

    station_t *st = &w->stations[5]; /* unused slot, completely empty */
    memset(st, 0, sizeof(*st));
    st->signal_range = 1.0f;

    /* Empty station — no hoppers, LASER_FAB cannot be paired. */
    ASSERT(!station_pair_satisfied(st, 2, 3, MODULE_LASER_FAB));

    /* Add only one of the two — still not satisfied. */
    add_hopper_for(st, 3, 4, COMMODITY_CUPRITE_INGOT);
    ASSERT(!station_pair_satisfied(st, 2, 3, MODULE_LASER_FAB));

    /* Add the second commodity — now satisfied. */
    add_hopper_for(st, 3, 5, COMMODITY_CRYSTAL_INGOT);
    ASSERT(station_pair_satisfied(st, 2, 3, MODULE_LASER_FAB));

    /* FURNACE accepts ANY ore — one ferrite-ore hopper is enough. */
    station_t *st2 = &w->stations[6];
    memset(st2, 0, sizeof(*st2));
    st2->signal_range = 1.0f;
    ASSERT(!station_pair_satisfied(st2, 2, 0, MODULE_FURNACE));
    add_hopper_for(st2, 3, 0, COMMODITY_FERRITE_ORE);
    ASSERT(station_pair_satisfied(st2, 2, 0, MODULE_FURNACE));

    /* Non-producer modules are always satisfied. */
    ASSERT(station_pair_satisfied(st, 2, 3, MODULE_DOCK));
    ASSERT(station_pair_satisfied(st, 1, 0, MODULE_SIGNAL_RELAY));
}

TEST(test_helios_ring2_rotates_under_dynamics) {
    /* Ring 2 of Helios carries the seeded drift bias (arm_speed[1]
     * = STATION_RING_SPEED = 0.04 rad/s) and must rotate continuously
     * under the all-passive dynamics (Slice 1.5a). After 2 sim seconds
     * at the bootstrapped omega, expect ~0.08 rad of rotation. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[2];
    ASSERT(st->arm_speed[1] > 0.0f);
    float r0 = st->arm_rotation[1];
    for (int i = 0; i < 240; i++) world_sim_step(w, 1.0f / 120.0f);
    float r1 = st->arm_rotation[1];
    /* Expect ~speed * 2.0s = 0.08 rad of rotation. */
    ASSERT(r1 - r0 > 0.05f);
}

TEST(test_all_rings_passive_under_spoke_load) {
    /* Slice 1.5a — every ring is passive. Drive Helios's ring 2 with
     * the seeded drift bias; ring 1 and ring 3 should also rotate via
     * spoke coupling (their producer↔hopper spokes pull them toward
     * ring 2's phase). The legacy code only spun the driver ring,
     * leaving ring 1 and ring 3 near-static. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    station_t *st = &w->stations[2];
    /* Force every Helios producer's pulse high so spokes are taut. */
    for (int m = 0; m < st->module_count; m++) {
        if (module_is_producer(st->modules[m].type)) st->module_active_pulse[m] = 1.0f;
    }
    float r1_0 = st->arm_rotation[0];  /* ring 1 */
    float r3_0 = st->arm_rotation[2];  /* ring 3 */
    for (int i = 0; i < 1200; i++) {  /* 10 sim seconds */
        for (int m = 0; m < st->module_count; m++) {
            if (module_is_producer(st->modules[m].type)) st->module_active_pulse[m] = 1.0f;
        }
        world_sim_step(w, 1.0f / 120.0f);
    }
    float r1_1 = st->arm_rotation[0];
    float r3_1 = st->arm_rotation[2];
    /* Ring 1 and ring 3 must each have moved measurably (>0.01 rad).
     * Direction follows ring 2's drift; with bootstrap omega = bias,
     * coupling pulls both passive rings into phase pursuit. */
    ASSERT(fabsf(r1_1 - r1_0) > 0.01f);
    ASSERT(fabsf(r3_1 - r3_0) > 0.01f);
}

TEST(test_output_hopper_spoke_contributes_torque) {
    /* Slice 1.5a — output hoppers participate in spoke physics. A
     * synthetic 2-ring station with only an output spoke (no input
     * spoke) must still apply torque to its passive ring when the
     * producer's pulse is hot. Asserts both magnitude AND direction:
     * a producer that's behind its hopper in phase pulls the hopper
     * ring backward (torque toward closing the phase gap), and the
     * producer ring forward — Newton's third. A sign-flip in the
     * spoke math would fail the direction assertion. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    station_t *st = &w->stations[0];
    st->signal_range = 1.0f;
    /* Frame press on ring 2 slot 0; frame output hopper on ring 3
     * slot 4 (160° ahead — hopper leads the producer in phase). */
    add_module_at(st, MODULE_FRAME_PRESS, 2, 0);
    add_hopper_for(st, 3, 4, COMMODITY_FRAME);
    /* No drift bias — isolate the spoke contribution. arm_omega all 0. */
    st->module_active_pulse[0] = 1.0f;

    /* Single tick: omega should become non-zero on both endpoint rings,
     * with opposite signs (Newton's third). */
    float r2_omega_pre = st->arm_omega[1];
    float r3_omega_pre = st->arm_omega[2];
    world_sim_step(w, 1.0f / 120.0f);
    float r2_omega_post = st->arm_omega[1];
    float r3_omega_post = st->arm_omega[2];
    float dr2 = r2_omega_post - r2_omega_pre;
    float dr3 = r3_omega_post - r3_omega_pre;

    /* Magnitude: spoke applied torque to both rings. */
    ASSERT(fabsf(dr2) > 1e-5f);
    ASSERT(fabsf(dr3) > 1e-5f);
    /* Direction: signs are opposite (Newton's third — a sign flip
     * would push both rings the same way, failing this). */
    ASSERT(dr2 * dr3 < 0.0f);
    /* Phase pursuit: hopper leads (slot 4 of 9 = ~160° vs 0°), so dr =
     * +160°, sin(dr) > 0, T = K*sin(dr) > 0. apply_spoke_torque adds
     * +T to producer ring (ra=2) and -T to hopper ring (rb=3). So ring 2
     * accelerates positive (toward the hopper) and ring 3 decelerates
     * negative (away from the producer). */
    ASSERT(dr2 > 0.0f);
    ASSERT(dr3 < 0.0f);
}

TEST(test_seed_stations_pair_complete) {
    /* Every producer on every starter station must have its cross-ring
     * pair-intake already satisfied at boot. This is the construction
     * regression catch — drift in either game_sim seeding or the pair
     * helper trips this. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    for (int s = 0; s < 3; s++) {
        const station_t *st = &w->stations[s];
        for (int m = 0; m < st->module_count; m++) {
            const station_module_t *mod = &st->modules[m];
            if (mod->scaffold) continue;
            if (!module_requires_pair(mod->type)) continue;
            ASSERT(station_pair_satisfied(st, mod->ring, mod->slot, mod->type));
        }
    }
}

void register_construction_module_schema_tests(void) {
    TEST_SECTION("\nModule schema (#280):\n");
    RUN(test_module_build_state_lifecycle);
    RUN(test_module_schema_basic_kinds);
    RUN(test_module_schema_producer_io);
    RUN(test_module_schema_required_output);
    RUN(test_module_furnace_instance_tag);
    RUN(test_commodity_ore_ingot_pairing);
    RUN(test_station_module_layout_status_missing_output);
    RUN(test_station_module_layout_status_no_local_consumer_is_ok);
    RUN(test_station_module_layout_status_furnace_uses_tag);
    RUN(test_station_module_layout_status_shipyard_exempt);
    RUN(test_seeded_furnaces_tagged);
    RUN(test_seeded_helios_output_hoppers);
    RUN(test_seeded_stations_layout_ok);
    RUN(test_module_schema_valid_rings);
    RUN(test_module_schema_helpers);
    RUN(test_module_schema_build_costs_match);
    RUN(test_module_flow_same_ring_transfer);
    RUN(test_module_flow_production_fills_buffers);
    RUN(test_module_flow_does_not_overflow_capacity);
    RUN(test_module_flow_storage_feeds_consumer);
    RUN(test_pair_neighbors_geometry);
    RUN(test_pair_satisfied_cross_ring);
    RUN(test_seed_stations_pair_complete);
    RUN(test_helios_ring2_rotates_under_dynamics);
    RUN(test_all_rings_passive_under_spoke_load);
    RUN(test_output_hopper_spoke_contributes_torque);
}
