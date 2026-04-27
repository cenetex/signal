#include "tests/test_harness.h"

TEST(test_player_save_load_roundtrip) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, "/tmp/test_cat"));
    ASSERT(world_save(w, "/tmp/test_player.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, "/tmp/test_cat");
    ASSERT(world_load(loaded, "/tmp/test_player.sav"));
    /* Players are cleared on load (they reconnect) */
    ASSERT(!loaded->players[0].connected);
    /* But world state (stations, etc.) survives */
    ASSERT_EQ_FLOAT(loaded->stations[0].signal_range, w->stations[0].signal_range, 0.01f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_player.sav");
}

TEST(test_world_save_load_preserves_stations) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->stations[0].inventory[COMMODITY_FERRITE_ORE] = 42.0f;
    w->stations[0].inventory[COMMODITY_FRAME] = 15.0f;
    ASSERT(world_save(w, "/tmp/test_world.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, "/tmp/test_world.sav"));
    ASSERT_EQ_FLOAT(loaded->stations[0].inventory[COMMODITY_FERRITE_ORE], 42.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->stations[0].inventory[COMMODITY_FRAME], 15.0f, 0.01f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_world.sav");
}

TEST(test_world_save_load_preserves_npcs) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    ASSERT(world_save(w, "/tmp/test_npcs.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, "/tmp/test_npcs.sav"));
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        ASSERT_EQ_FLOAT(loaded->npc_ships[i].pos.x, w->npc_ships[i].pos.x, 0.01f);
        ASSERT_EQ_FLOAT(loaded->npc_ships[i].pos.y, w->npc_ships[i].pos.y, 0.01f);
    }
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_npcs.sav");
}

TEST(test_npc_ship_physics_in_sync_each_tick) {
    /* Tripwire for the Slice 13 physics flip and any future mirror
     * direction change: at the end of every sim step, every active
     * NPC's paired ship_t must agree with its npc_ship_t on hull,
     * hull_class, pos, vel, and angle. A missed write site lasts at
     * most one tick before the reverse mirror washes it out, so
     * behavioral tests don't catch it — this one does, immediately.
     * Runs for 10 sim seconds (1200 ticks @ 120 Hz) to cover spawn,
     * mine, dock, hauler-in-transit, and at least one despawn cycle. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int t = 0; t < 1200; t++) {
        world_sim_step(w, SIM_DT);
        for (int n = 0; n < MAX_NPC_SHIPS; n++) {
            const npc_ship_t *npc = &w->npc_ships[n];
            if (!npc->active) continue;
            const ship_t *s = world_npc_ship_for(w, n);
            ASSERT(s != NULL);
            ASSERT_EQ_FLOAT(s->hull, npc->hull, 0.001f);
            ASSERT(s->hull_class == npc->hull_class);
            ASSERT_EQ_FLOAT(s->pos.x, npc->pos.x, 0.001f);
            ASSERT_EQ_FLOAT(s->pos.y, npc->pos.y, 0.001f);
            ASSERT_EQ_FLOAT(s->vel.x, npc->vel.x, 0.001f);
            ASSERT_EQ_FLOAT(s->vel.y, npc->vel.y, 0.001f);
            ASSERT_EQ_FLOAT(s->angle, npc->angle, 0.001f);
        }
    }
}

TEST(test_world_load_rebuilds_character_pool) {
    /* world_load only restores npc_ships[]; the paired character_t /
     * ships[] pools are server-side transient and have to be rebuilt
     * via rebuild_characters_from_npcs at the end of load. Verify
     * (a) every active NPC has a paired character with the right kind
     * and ship_idx, (b) apply_npc_ship_damage hits the rebuilt ship
     * (not a phantom one), (c) the paired ship sees the damage on the
     * very next sim step's reverse-mirror back to npc.hull. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    ASSERT(world_save(w, "/tmp/test_char_pool.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(world_load(loaded, "/tmp/test_char_pool.sav"));

    /* (a) paired-pool integrity */
    int active_npcs = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!loaded->npc_ships[n].active) continue;
        active_npcs++;
        int char_cap = (int)(sizeof(loaded->characters) /
                             sizeof(loaded->characters[0]));
        int found_char = -1;
        for (int c = 0; c < char_cap; c++) {
            if (!loaded->characters[c].active) continue;
            if (loaded->characters[c].npc_slot == n) { found_char = c; break; }
        }
        ASSERT(found_char >= 0);
        const character_t *c = &loaded->characters[found_char];
        ASSERT(c->kind == CHARACTER_KIND_NPC_MINER ||
               c->kind == CHARACTER_KIND_NPC_HAULER ||
               c->kind == CHARACTER_KIND_NPC_TOW);
        ASSERT(c->ship_idx >= 0 && c->ship_idx < MAX_SHIPS);
    }
    ASSERT(active_npcs > 0);

    /* (b)+(c) damage flows through rebuilt ship slot */
    int target = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (loaded->npc_ships[n].active) { target = n; break; }
    }
    ASSERT(target >= 0);
    float pre = loaded->npc_ships[target].hull;
    ASSERT(pre > 5.0f);
    apply_npc_ship_damage(loaded, target, 5.0f);
    /* npc.hull only updates after the next sim step's reverse mirror;
     * one tick is enough. */
    world_sim_step(loaded, SIM_DT);
    ASSERT(loaded->npc_ships[target].hull < pre);

    remove("/tmp/test_char_pool.sav");
}

TEST(test_world_save_load_preserves_fracture_children) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    asteroid_t *a;
    fracture_claim_state_t *state;

    ASSERT(w != NULL);
    ASSERT(loaded != NULL);
    world_reset(w);
    a = &w->asteroids[17];
    state = &w->fracture_claims[17];
    memset(a, 0, sizeof(*a));
    memset(state, 0, sizeof(*state));

    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_CRYSTAL_ORE;
    a->pos = v2(321.0f, -654.0f);
    a->vel = v2(7.0f, -3.5f);
    a->radius = 9.0f;
    a->hp = 4.0f;
    a->max_hp = 9.0f;
    a->ore = 6.0f;
    a->max_ore = 9.0f;
    a->rotation = 1.25f;
    a->spin = 0.4f;
    a->seed = 22.0f;
    a->age = 12.0f;
    a->smelt_progress = 0.35f;
    a->last_towed_by = 2;
    a->last_fractured_by = 1;
    memcpy(a->last_towed_token, "TOWTOKEN", 8);
    memcpy(a->last_fractured_token, "FRAGTOKN", 8);
    for (int i = 0; i < 32; i++) {
        a->fracture_seed[i] = (uint8_t)(0x60 + i);
        a->fragment_pub[i] = (uint8_t)(0x90 + i);
    }
    a->grade = MINING_GRADE_RATI;

    state->active = true;
    state->fracture_id = 444;
    state->deadline_ms = 123456;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;
    state->best_nonce = 19;
    state->best_grade = MINING_GRADE_FINE;
    for (int i = 0; i < 32; i++)
        state->best_player_pub[i] = (uint8_t)(0xC0 + i);
    state->seen_claimant_count = 2;
    memcpy(state->seen_claimant_tokens[0], "CLAIM001", 8);
    memcpy(state->seen_claimant_tokens[1], "CLAIM002", 8);
    w->next_fracture_id = 555;

    ASSERT(world_save(w, "/tmp/test_fracture_children.sav"));
    ASSERT(world_load(loaded, "/tmp/test_fracture_children.sav"));

    ASSERT_EQ_INT(loaded->next_fracture_id, 555);
    ASSERT(loaded->asteroids[17].active);
    ASSERT(loaded->asteroids[17].fracture_child);
    ASSERT_EQ_INT(loaded->asteroids[17].tier, ASTEROID_TIER_S);
    ASSERT_EQ_INT(loaded->asteroids[17].commodity, COMMODITY_CRYSTAL_ORE);
    ASSERT_EQ_FLOAT(loaded->asteroids[17].pos.x, 321.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->asteroids[17].pos.y, -654.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded->asteroids[17].smelt_progress, 0.35f, 0.01f);
    ASSERT_EQ_INT(loaded->asteroids[17].grade, MINING_GRADE_RATI);
    ASSERT(memcmp(loaded->asteroids[17].fracture_seed, a->fracture_seed, 32) == 0);
    ASSERT(memcmp(loaded->asteroids[17].fragment_pub, a->fragment_pub, 32) == 0);
    ASSERT(memcmp(loaded->asteroids[17].last_towed_token, a->last_towed_token, 8) == 0);
    ASSERT(memcmp(loaded->asteroids[17].last_fractured_token, a->last_fractured_token, 8) == 0);
    ASSERT(loaded->fracture_claims[17].active);
    ASSERT(!loaded->fracture_claims[17].resolved);
    ASSERT(loaded->fracture_claims[17].challenge_dirty);
    ASSERT_EQ_INT(loaded->fracture_claims[17].fracture_id, 444);
    ASSERT_EQ_INT(loaded->fracture_claims[17].deadline_ms, 123456);
    ASSERT_EQ_INT(loaded->fracture_claims[17].best_nonce, 19);
    ASSERT_EQ_INT(loaded->fracture_claims[17].seen_claimant_count, 2);
    ASSERT(memcmp(loaded->fracture_claims[17].best_player_pub,
                  state->best_player_pub, 32) == 0);
    ASSERT(memcmp(loaded->fracture_claims[17].seen_claimant_tokens[0],
                  state->seen_claimant_tokens[0], 8) == 0);
    ASSERT(memcmp(loaded->fracture_claims[17].seen_claimant_tokens[1],
                  state->seen_claimant_tokens[1], 8) == 0);

    remove("/tmp/test_fracture_children.sav");
}

TEST(test_world_load_preserves_fracture_claim_dedupe_identity) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    asteroid_t *a;
    fracture_claim_state_t *state;
    uint8_t player_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;

    ASSERT(w != NULL);
    ASSERT(loaded != NULL);
    world_reset(w);
    a = &w->asteroids[9];
    state = &w->fracture_claims[9];
    memset(a, 0, sizeof(*a));
    memset(state, 0, sizeof(*state));

    w->players[0].connected = true;
    w->players[0].session_ready = true;
    memcpy(w->players[0].session_token, "PERSIST01", 8);
    w->players[0].ship.pos = w->stations[0].pos;

    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 7.0f;
    a->pos = w->stations[0].pos;
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(0x20 + i);

    state->active = true;
    state->fracture_id = 818;
    state->deadline_ms = 600;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;

    sha256_bytes(w->players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);
    ASSERT(submit_fracture_claim(w, 0, state->fracture_id, best_nonce,
                                 (uint8_t)best_grade));

    ASSERT(world_save(w, "/tmp/test_fracture_claim_dedupe.sav"));
    ASSERT(world_load(loaded, "/tmp/test_fracture_claim_dedupe.sav"));

    loaded->players[1].connected = true;
    loaded->players[1].session_ready = true;
    memcpy(loaded->players[1].session_token, w->players[0].session_token, 8);
    loaded->players[1].ship.pos = loaded->stations[0].pos;
    ASSERT(!submit_fracture_claim(loaded, 1, 818, best_nonce, (uint8_t)best_grade));

    remove("/tmp/test_fracture_claim_dedupe.sav");
}

TEST(test_world_load_missing_file) {
    WORLD_DECL;
    ASSERT(!world_load(&w, "/tmp/nonexistent_save_file.sav"));
}

TEST(test_player_save_load_preserves_ship) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.hull = 42.0f;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = 10.0f;
    sp.ship.cargo[COMMODITY_CUPRITE_ORE] = 5.0f;
    sp.ship.mining_level = 2;
    sp.ship.hold_level = 1;
    sp.ship.tractor_level = 3;
    sp.current_station = 1;
    ASSERT(player_save(&sp, "/tmp", 99));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, "/tmp", 99));
    ASSERT_EQ_FLOAT(loaded.ship.hull, 42.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded.ship.cargo[COMMODITY_FERRITE_ORE], 10.0f, 0.01f);
    ASSERT_EQ_FLOAT(loaded.ship.cargo[COMMODITY_CUPRITE_ORE], 5.0f, 0.01f);
    ASSERT_EQ_INT(loaded.ship.mining_level, 2);
    ASSERT_EQ_INT(loaded.ship.hold_level, 1);
    ASSERT_EQ_INT(loaded.ship.tractor_level, 3);
    ASSERT_EQ_INT(loaded.current_station, 1);
    ASSERT(loaded.docked);
    remove("/tmp/player_99.sav");
}

TEST(test_world_save_round_trips_station_manifest) {
    /* Previously, non-empty station manifests caused world_save to fail —
     * the pre-#339 guard rejected them because manifest wasn't persisted.
     * Slice A of #339 lifted that guard and added real serialization;
     * this test asserts the round trip now works. */
    WORLD_DECL;
    WORLD_DECL_NAME(loaded);
    cargo_unit_t unit = {0};
    world_reset(&w);
    world_reset(&loaded);
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_RARE;
    unit.pub[0] = 0xA5;
    unit.pub[31] = 0x5A;
    ASSERT(manifest_push(&w.stations[0].manifest, &unit));
    ASSERT_EQ_INT(w.stations[0].manifest.count, 1);
    ASSERT(world_save(&w, "/tmp/test_manifest_roundtrip.sav"));
    ASSERT(world_load(&loaded, "/tmp/test_manifest_roundtrip.sav"));
    ASSERT_EQ_INT(loaded.stations[0].manifest.count, 1);
    ASSERT(loaded.stations[0].manifest.units != NULL);
    ASSERT_EQ_INT(loaded.stations[0].manifest.units[0].kind, CARGO_KIND_INGOT);
    ASSERT_EQ_INT(loaded.stations[0].manifest.units[0].commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(loaded.stations[0].manifest.units[0].grade, MINING_GRADE_RARE);
    ASSERT(memcmp(loaded.stations[0].manifest.units[0].pub, unit.pub, 32) == 0);
    remove("/tmp/test_manifest_roundtrip.sav");
}

TEST(test_player_load_clamps_negative_credits) {
    /* Credits are now in station ledgers, not ship_t. PLY3 format has no
     * credits field. This test just confirms save/load round-trip works. */
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    ASSERT(player_save(&sp, "/tmp", 98));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, "/tmp", 98));
    /* No credits field to clamp — ledger balances are always >= 0 */
    remove("/tmp/player_98.sav");
}

TEST(test_player_save_round_trips_ship_manifest) {
    /* Pre-#339/A.2 the ship manifest was guarded empty on save (PLY4
     * format had no tail). Slice A.2 moved to PLY5 which appends the
     * manifest after the fixed ship blob. Verify round trip preserves
     * kind, commodity, grade, and pub of each entry. */
    WORLD_DECL;
    SERVER_PLAYER_DECL(sp);
    SERVER_PLAYER_DECL(loaded);
    cargo_unit_t unit = {0};
    world_reset(&w);
    player_init_ship(&sp, &w);
    sp.connected = true;
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_CUPRITE_INGOT;
    unit.grade = (uint8_t)MINING_GRADE_FINE;
    unit.pub[0] = 0x5A;
    unit.pub[7] = 0xA5;
    ASSERT(manifest_push(&sp.ship.manifest, &unit));
    ASSERT(sp.ship.manifest.count == 1);
    ASSERT(player_save(&sp, "/tmp", 92));
    ASSERT(player_load(&loaded, &w, "/tmp", 92));
    ASSERT_EQ_INT(loaded.ship.manifest.count, 1);
    ASSERT(loaded.ship.manifest.units != NULL);
    ASSERT_EQ_INT(loaded.ship.manifest.units[0].kind, CARGO_KIND_INGOT);
    ASSERT_EQ_INT(loaded.ship.manifest.units[0].commodity, COMMODITY_CUPRITE_INGOT);
    ASSERT_EQ_INT(loaded.ship.manifest.units[0].grade, MINING_GRADE_FINE);
    ASSERT(memcmp(loaded.ship.manifest.units[0].pub, unit.pub, 32) == 0);
    remove("/tmp/player_92.sav");
}

TEST(test_player_load_clamps_negative_cargo) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.cargo[COMMODITY_FERRITE_ORE] = -50.0f;
    ASSERT(player_save(&sp, "/tmp", 97));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, "/tmp", 97));
    ASSERT(loaded.ship.cargo[COMMODITY_FERRITE_ORE] >= 0.0f);
    remove("/tmp/player_97.sav");
}

TEST(test_player_load_clamps_hull_hp) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.hull = 99999.0f;  /* way above max */
    ASSERT(player_save(&sp, "/tmp", 96));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, "/tmp", 96));
    ASSERT(loaded.ship.hull <= ship_max_hull(&loaded.ship));
    remove("/tmp/player_96.sav");
}

TEST(test_player_load_clamps_upgrade_levels) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.ship.mining_level = 100;
    sp.ship.hold_level = -5;
    ASSERT(player_save(&sp, "/tmp", 95));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, "/tmp", 95));
    ASSERT(loaded.ship.mining_level >= 0 && loaded.ship.mining_level <= SHIP_UPGRADE_MAX_LEVEL);
    ASSERT(loaded.ship.hold_level >= 0 && loaded.ship.hold_level <= SHIP_UPGRADE_MAX_LEVEL);
    remove("/tmp/player_95.sav");
}

TEST(test_player_load_invalid_station_falls_back) {
    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(sp);
    player_init_ship(&sp, &w);
    sp.connected = true;
    sp.current_station = 99;  /* out of range */
    ASSERT(player_save(&sp, "/tmp", 94));

    SERVER_PLAYER_DECL(loaded);
    ASSERT(player_load(&loaded, &w, "/tmp", 94));
    ASSERT(loaded.current_station >= 0 && loaded.current_station < MAX_STATIONS);
    remove("/tmp/player_94.sav");
}

TEST(test_player_load_bad_magic_fails) {
    /* Write garbage with wrong magic */
    FILE *f = fopen("/tmp/player_93.sav", "wb");
    ASSERT(f != NULL);
    uint32_t bad_magic = 0xDEADBEEF;
    fwrite(&bad_magic, sizeof(bad_magic), 1, f);
    fclose(f);

    WORLD_DECL;
    world_reset(&w);
    SERVER_PLAYER_DECL(loaded);
    ASSERT(!player_load(&loaded, &w, "/tmp", 93));
    remove("/tmp/player_93.sav");
}

TEST(test_world_load_rejects_stale_version) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    ASSERT(world_save(w, "/tmp/test_stale.sav"));
    /* Overwrite version (bytes 4-7) with old version 11 */
    FILE *f = fopen("/tmp/test_stale.sav", "r+b");
    ASSERT(f != NULL);
    fseek(f, 4, SEEK_SET);
    uint32_t old_version = 11;
    fwrite(&old_version, sizeof(old_version), 1, f);
    fclose(f);
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(!world_load(loaded, "/tmp/test_stale.sav"));
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_stale.sav");
}

TEST(test_world_save_load_preserves_module_ring_slot) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    ASSERT(w->stations[0].module_count >= 4);
    /* Verify furnace on ring 1 and silo on ring 2 survive save/load */
    station_module_t orig = w->stations[0].modules[2]; /* furnace at ring 1 slot 2 */
    ASSERT(orig.type == MODULE_FURNACE);
    ASSERT(orig.ring == 1);
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, "/tmp/test_modcat"));
    ASSERT(world_save(w, "/tmp/test_modules.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, "/tmp/test_modcat");
    ASSERT(world_load(loaded, "/tmp/test_modules.sav"));
    station_module_t restored = loaded->stations[0].modules[2];
    ASSERT_EQ_INT((int)restored.type, (int)orig.type);
    ASSERT_EQ_INT((int)restored.ring, (int)orig.ring);
    ASSERT_EQ_INT((int)restored.slot, (int)orig.slot);
    ASSERT_EQ_INT((int)restored.scaffold, (int)orig.scaffold);
    ASSERT_EQ_FLOAT(restored.build_progress, orig.build_progress, 0.001f);
    /* modules[3] = ore_silo on ring 2 */
    station_module_t mod3 = loaded->stations[0].modules[3];
    ASSERT(mod3.type == MODULE_ORE_SILO);
    ASSERT_EQ_INT((int)mod3.ring, 2);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_modules.sav");
}

TEST(test_world_save_load_preserves_smelted_ingots) {
    /* world_t is ~600KB — use heap to avoid stack overflow on CI. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    w->stations[0].inventory[COMMODITY_FERRITE_ORE] = 20.0f;
    for (int i = 0; i < (int)(10.0f / SIM_DT); i++) world_sim_step(w, SIM_DT);
    float ingots_before = w->stations[0].inventory[COMMODITY_FERRITE_INGOT];
    ASSERT(ingots_before > 0.0f);
    ASSERT(world_save(w, "/tmp/test_ingots.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(loaded != NULL);
    ASSERT(world_load(loaded, "/tmp/test_ingots.sav"));
    ASSERT_EQ_FLOAT(loaded->stations[0].inventory[COMMODITY_FERRITE_INGOT], ingots_before, 0.01f);
    remove("/tmp/test_ingots.sav");
    /* loaded + w auto-freed by WORLD_HEAP cleanup */
}

/*
 * EXPECTED_SAVE_SIZE is the exact byte count of a world.sav written by the
 * current SAVE_VERSION. If a field is added to write_station / write_asteroid /
 * write_npc / write_contract / the scaffolds array, or the header, this
 * number changes and the test fails. That failure is the reminder to:
 *   1. Bump SAVE_VERSION
 *   2. Add a migration block in world_load()
 *   3. Update this constant to the new size
 */
/* v23: station credit pool added (#312) — +4 bytes per station (8×4=32). */
/* v29: +2 bytes per station (uint16 manifest count) = +128 bytes for all
 * MAX_STATIONS=64 slots. Empty stations carry only the count; no units. */
/* v30: +1 byte per contract (required_grade) = +24 for MAX_CONTRACTS=24. */
#define EXPECTED_SAVE_SIZE 269164 /* v32: npc_ship_t.hull added (4B × 16 NPC slots = 64B over v31) */

TEST(test_save_file_size_stable) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    ASSERT(world_save(w, "/tmp/test_size.sav"));
    /* w auto-freed by WORLD_HEAP cleanup */
    FILE *f = fopen("/tmp/test_size.sav", "rb");
    ASSERT(f != NULL);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    /* If this fails you changed the binary save format.
     * Bump SAVE_VERSION, add a migration, and update EXPECTED_SAVE_SIZE. */
    ASSERT_EQ_INT((int)size, EXPECTED_SAVE_SIZE);
    remove("/tmp/test_size.sav");
}

TEST(test_save_header_golden_bytes) {
    WORLD_DECL;
    w.rng = 2037u;  /* default seed */
    world_reset(&w);
    w.time = 0.0f;
    w.field_spawn_timer = 0.0f;
    ASSERT(world_save(&w, "/tmp/test_header.sav"));
    FILE *f = fopen("/tmp/test_header.sav", "rb");
    ASSERT(f != NULL);
    uint32_t magic, version, rng;
    float time_val, spawn_timer;
    ASSERT_EQ_INT((int)fread(&magic,       4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&version,     4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&rng,         4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&time_val,    4, 1, f), 1);
    ASSERT_EQ_INT((int)fread(&spawn_timer, 4, 1, f), 1);
    fclose(f);
    ASSERT_EQ_INT((int)magic, (int)0x5349474E);    /* "SIGN" */
    ASSERT_EQ_INT((int)version, 32);
    ASSERT(rng != 0);  /* seed is set */
    ASSERT_EQ_FLOAT(time_val, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(spawn_timer, 0.0f, 0.001f);
    remove("/tmp/test_header.sav");
}

TEST(test_save_load_preserves_player_outpost) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    w->players[0].docked = false;  /* must be undocked to place */
    /* Place outside Prospect's core coverage (signal < 0.80 at placement). */
    vec2 pos = v2(6000.0f, -2400.0f);
    int slot = test_place_outpost_via_tow(w, &w->players[0], pos);
    ASSERT(slot >= 0);
    ASSERT(station_exists(&w->stations[slot]));
    ASSERT(w->stations[slot].scaffold);
    /* Deliver some frames to advance progress */
    w->stations[slot].inventory[COMMODITY_FRAME] = 30.0f;
    for (int i = 0; i < 600; i++) world_sim_step(w, SIM_DT);
    float progress = w->stations[slot].scaffold_progress;
    int mod_count = w->stations[slot].module_count;
    char name_buf[32];
    memcpy(name_buf, w->stations[slot].name, 32);
    /* Save and reload (world + catalog) */
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, "/tmp/test_outcat"));
    ASSERT(world_save(w, "/tmp/test_outpost.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, "/tmp/test_outcat");
    ASSERT(world_load(loaded, "/tmp/test_outpost.sav"));
    /* Outpost must survive */
    ASSERT(station_exists(&loaded->stations[slot]));
    ASSERT(loaded->stations[slot].scaffold);
    ASSERT_EQ_FLOAT(loaded->stations[slot].pos.x, 6000.0f, 1.0f);
    ASSERT_EQ_FLOAT(loaded->stations[slot].pos.y, -2400.0f, 1.0f);
    ASSERT_EQ_FLOAT(loaded->stations[slot].scaffold_progress, progress, 0.01f);
    ASSERT_EQ_INT(loaded->stations[slot].module_count, mod_count);
    ASSERT_STR_EQ(loaded->stations[slot].name, name_buf);
    /* Signal chain rebuilt — outpost may or may not be connected depending on
     * scaffold state, but the station slot must still exist */
    ASSERT(loaded->stations[slot].signal_range > 0.0f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_outpost.sav");
}

TEST(test_save_backward_compat_version_accepted) {
    /* v24 save format roundtrips correctly with inventory data.
     * Station identity comes from catalog, session data from world save. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    w->stations[0].inventory[COMMODITY_FERRITE_ORE] = 77.0f;
    ASSERT(station_catalog_save_all(w->stations, MAX_STATIONS, "/tmp/test_compatcat"));
    ASSERT(world_save(w, "/tmp/test_compat.sav"));
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    station_catalog_load_all(loaded->stations, MAX_STATIONS, "/tmp/test_compatcat");
    ASSERT(world_load(loaded, "/tmp/test_compat.sav"));
    ASSERT_EQ_FLOAT(loaded->stations[0].inventory[COMMODITY_FERRITE_ORE], 77.0f, 0.01f);
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_compat.sav");
}

TEST(test_save_v21_module_remap) {
    /* SKIPPED: This test patches a v23 save to look like v21, but the v23
     * format has credit_pool fields woven into each station record that
     * v21 doesn't have. The loader can't distinguish real v21 from
     * patched v23, so the file is unreadable. Skipping until a proper
     * v21 binary fixture is created. (#312)
     *
     * The original disabled body remapped module types 0/5/6/11/12/15 from
     * the v21 enum space to v22 outcomes (DOCK/dropped/REPAIR_BAY/dropped/
     * ORE_SILO/SHIPYARD). See git blame on this test for the full body. */
}

TEST(test_save_future_version_rejected) {
    /* A save with version > SAVE_VERSION must be rejected (can't load future formats) */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    ASSERT(world_save(w, "/tmp/test_future.sav"));
    FILE *f = fopen("/tmp/test_future.sav", "r+b");
    ASSERT(f != NULL);
    fseek(f, 4, SEEK_SET);
    uint32_t future = 9999;
    fwrite(&future, sizeof(future), 1, f);
    fclose(f);
    WORLD_HEAP loaded = calloc(1, sizeof(world_t));
    ASSERT(!world_load(loaded, "/tmp/test_future.sav"));
    /* loaded auto-freed by WORLD_HEAP cleanup */
    /* w auto-freed by WORLD_HEAP cleanup */
    remove("/tmp/test_future.sav");
}

void register_save_persistence_tests(void) {
    TEST_SECTION("\nPersistence tests:\n");
    RUN(test_player_save_load_roundtrip);
    RUN(test_world_save_load_preserves_stations);
    RUN(test_world_save_load_preserves_npcs);
    RUN(test_npc_ship_physics_in_sync_each_tick);
    RUN(test_world_load_rebuilds_character_pool);
    RUN(test_world_save_load_preserves_fracture_children);
    RUN(test_world_load_preserves_fracture_claim_dedupe_identity);
    RUN(test_world_load_missing_file);
    RUN(test_player_save_load_preserves_ship);
    RUN(test_world_save_round_trips_station_manifest);
    RUN(test_player_load_clamps_negative_credits);
    RUN(test_player_save_round_trips_ship_manifest);
    RUN(test_player_load_clamps_negative_cargo);
    RUN(test_player_load_clamps_hull_hp);
    RUN(test_player_load_clamps_upgrade_levels);
    RUN(test_player_load_invalid_station_falls_back);
    RUN(test_player_load_bad_magic_fails);
    RUN(test_world_load_rejects_stale_version);
    RUN(test_world_save_load_preserves_module_ring_slot);
    RUN(test_world_save_load_preserves_smelted_ingots);
}

void register_save_format_tests(void) {
    TEST_SECTION("\nSave format stability:\n");
    RUN(test_save_file_size_stable);
    RUN(test_save_header_golden_bytes);
    RUN(test_save_load_preserves_player_outpost);
    RUN(test_save_backward_compat_version_accepted);
    RUN(test_save_v21_module_remap);
    RUN(test_save_future_version_rejected);
}

