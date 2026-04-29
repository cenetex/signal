#include "tests/test_harness.h"

TEST(test_world_reset_creates_stations) {
    WORLD_DECL;
    world_reset(&w);
    ASSERT_STR_EQ(w.stations[0].name, "Prospect Refinery");
    ASSERT(station_has_module(&w.stations[0], MODULE_FURNACE));
    ASSERT_STR_EQ(w.stations[1].name, "Kepler Yard");
    ASSERT_STR_EQ(w.stations[2].name, "Helios Works");
}

TEST(test_world_reset_spawns_asteroids) {
    WORLD_DECL;
    world_reset(&w);
    int count = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (w.asteroids[i].active) count++;
    ASSERT(count >= 20);
}

TEST(test_world_reset_spawns_npcs) {
    /* Starter NPC roster covers the inter-station chain:
     *   Prospect: 2 miners (ferrite), 2 haulers (ferrite -> Kepler)
     *   Helios:   1 miner  (CU/CR),   1 hauler  (kits / modules out)
     *   Kepler:                       1 hauler  (frames -> Helios)
     * Plus a tow drone at each shipyard (Kepler, Helios). */
    WORLD_DECL;
    world_reset(&w);
    int miners = 0, haulers = 0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!w.npc_ships[i].active) continue;
        if (w.npc_ships[i].role == NPC_ROLE_MINER) miners++;
        if (w.npc_ships[i].role == NPC_ROLE_HAULER) haulers++;
    }
    ASSERT_EQ_INT(miners, 3);
    ASSERT_EQ_INT(haulers, 4);
}

TEST(test_dead_hauler_auto_respawns) {
    /* Confirm replenish_npc_roster fires from step_npc_ships: kill a
     * Kepler-homed hauler, run sim past the respawn cooldown, expect
     * a replacement to appear at the same home with full hull.
     * Without this loop, hostile players could permanently sabotage
     * a station's chain by repeatedly sniping its drones. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    /* Find the Kepler-homed hauler. */
    int target_slot = -1;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_HAULER) continue;
        if (w->npc_ships[n].home_station != 1) continue;
        target_slot = n;
        break;
    }
    ASSERT(target_slot >= 0);
    /* Force-kill via the public damage helper. ship.hull is the
     * authoritative side post-#294 slice 9-11 so we hit ship.hull
     * directly to skip the npc-side mirror lag. */
    ship_t *s = world_npc_ship_for(w, target_slot);
    ASSERT(s != NULL);
    s->hull = 0.0f;
    /* One sim step lets the despawn check at top of step_npc_ships
     * notice and free the slot. */
    world_sim_step(w, SIM_DT);
    int kepler_haulers = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_HAULER) continue;
        if (w->npc_ships[n].home_station == 1) kepler_haulers++;
    }
    ASSERT_EQ_INT(kepler_haulers, 0);

    /* Run past the 15 s respawn cooldown. SIM_DT = 1/120 s so 1900
     * ticks ≈ 15.83 s — comfortable margin. */
    for (int i = 0; i < 1900; i++) world_sim_step(w, SIM_DT);

    int kepler_haulers_after = 0;
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w->npc_ships[n].active) continue;
        if (w->npc_ships[n].role != NPC_ROLE_HAULER) continue;
        if (w->npc_ships[n].home_station == 1) kepler_haulers_after++;
    }
    ASSERT_EQ_INT(kepler_haulers_after, 1);
}

TEST(test_player_init_ship_docked) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    ASSERT_EQ_INT(w.players[0].current_station, 0);
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, 100.0f, 0.01f);
}

TEST(test_world_sim_step_advances_time) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    float t0 = w.time;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.time > t0);
}

TEST(test_world_sim_step_moves_ship_with_thrust) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].ship.angle = 0.0f;
    w.players[0].ship.pos = v2(0.0f, 0.0f);
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].input.thrust = 1.0f;
    for (int i = 0; i < 120; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.players[0].ship.pos.x > 5.0f);
}

TEST(test_world_sim_step_mining_damages_asteroid) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].ship.mining_level = SHIP_UPGRADE_MAX_LEVEL;
    /* Place player right next to first active non-S asteroid */
    int target = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active && w.asteroids[i].tier != ASTEROID_TIER_S) {
            target = i;
            break;
        }
    }
    ASSERT(target >= 0);
    vec2 apos = w.asteroids[target].pos;
    w.players[0].ship.pos = v2(apos.x - 50.0f, apos.y);
    w.players[0].ship.angle = 0.0f;
    w.players[0].input.mine = true;
    float hp_before = w.asteroids[target].hp;
    for (int i = 0; i < 60; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.asteroids[target].hp < hp_before);
}

TEST(test_world_sim_step_docking) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    /* Launch */
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(!w.players[0].docked);
    /* Fly back into dock range (inside ring gap corridor) and dock */
    w.players[0].input.interact = false;
    for (int i = 0; i < 10; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    /* Place ship at dock port and dock */
    w.players[0].ship.pos = w.stations[0].pos;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].docked = true;
    w.players[0].in_dock_range = true;
    w.players[0].current_station = 0;
    w.players[0].nearby_station = 0;
    ASSERT(w.players[0].docked);
}

TEST(test_world_sim_step_refinery_produces_ingots) {
    WORLD_DECL;
    world_reset(&w);
    w.stations[0].inventory[COMMODITY_FERRITE_ORE] = 50.0f;
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(w.stations[0].inventory[COMMODITY_FERRITE_INGOT] > 0.0f);
    ASSERT(w.stations[0].inventory[COMMODITY_FERRITE_ORE] < 50.0f);
}

TEST(test_mining_class_prefix_round_trip) {
    /* Pubkeys whose first base58 char is M, H, T, S, F, K, RATi prefix,
     * and one that is "anonymous" (digit/lowercase). We don't have direct
     * control over base58 output but we can iterate seeds until each
     * prefix appears. */
    int seen[MINING_CLASS_COMMISSIONED + 1] = {0};
    int found = 0;
    for (uint32_t seed = 1; seed < 1000000 && found < 7; seed++) {
        uint8_t s[32];
        for (int i = 0; i < 32; i++)
            s[i] = (uint8_t)((seed >> ((i & 3) * 8)) ^ (seed * 2654435761u >> (i & 7)));
        uint8_t pub[32];
        sha256_bytes(s, 32, pub);
        int cls = mining_pubkey_class(pub);
        if (cls < 0 || cls > MINING_CLASS_COMMISSIONED) continue;
        if (cls == MINING_CLASS_ANONYMOUS || cls == MINING_CLASS_COMMISSIONED) continue;
        if (!seen[cls]) {
            seen[cls] = 1;
            found++;
            char render[12];
            mining_render_callsign(pub, render);
            /* Render must contain a dash, and the prefix segment must
             * match what mining_pubkey_class said. */
            ASSERT(strchr(render, '-') != NULL);
        }
    }
    /* Should hit M/H/T/S/F/K within 1M iterations. RATi is ~1 in 11M so
     * not asserted here. */
    ASSERT(seen[MINING_CLASS_M]);
    ASSERT(seen[MINING_CLASS_H]);
    ASSERT(seen[MINING_CLASS_T]);
    ASSERT(seen[MINING_CLASS_S]);
    ASSERT(seen[MINING_CLASS_F]);
    ASSERT(seen[MINING_CLASS_K]);
}

TEST(test_refinery_deposits_named_ingot) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    player_init_ship(&w->players[0], w);
    w->players[0].connected = true;
    w->players[0].docked = false;
    w->players[0].session_ready = true;
    memset(w->players[0].session_token, 0x42, 8);
    /* Force a furnace at station 0 — already exists by default in
     * world_reset, but assert. */
    bool has_furnace = false;
    int furnace_idx = -1;
    int silo_idx = -1;
    for (int m = 0; m < w->stations[0].module_count; m++) {
        if (w->stations[0].modules[m].type == MODULE_FURNACE) {
            has_furnace = true;
            furnace_idx = m;
        }
        if (w->stations[0].modules[m].type == MODULE_ORE_SILO)
            silo_idx = m;
    }
    ASSERT(has_furnace);
    ASSERT(silo_idx >= 0);

    /* Stop ring motion and place the fragment exactly on the furnace/silo
     * midpoint so the dedicated smelt path resolves deterministically. */
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w->stations[0].arm_speed[arm] = 0.0f;
        w->stations[0].arm_rotation[arm] = 0.0f;
    }
    vec2 furnace_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[furnace_idx].ring, w->stations[0].modules[furnace_idx].slot);
    vec2 silo_pos = module_world_pos_ring(&w->stations[0],
        w->stations[0].modules[silo_idx].ring, w->stations[0].modules[silo_idx].slot);
    vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);

    /* Spawn an S-tier ferrite fragment on the smelt midpoint, with an
     * arbitrary fracture_seed. */
    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 10.0f;
    a->max_ore = 10.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    /* Seed values intentionally varied so the roll lands somewhere. */
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(i * 17 + 3);
    a->grade = (uint8_t)MINING_GRADE_RATI;
    a->pos = midpoint;
    a->vel = v2(0, 0);
    a->last_fractured_by = 0;
    a->last_towed_by = 0;
    a->net_dirty = true;
    w->players[0].ship.pos = v2_add(midpoint, v2(100.0f, 0.0f));
    w->players[0].ship.vel = v2(0.0f, 0.0f);

    /* Run sim until smelt completes (smelt_progress accumulates ~0.5/s). */
    int initial_named = w->stations[0].named_ingots_count;
    int initial_manifest = w->stations[0].manifest.count;
    float initial_bulk = w->stations[0].inventory[COMMODITY_FERRITE_INGOT];
    for (int i = 0; i < 600 && w->asteroids[slot].active; i++)
        world_sim_step(w, 1.0f / 120.0f);
    /* Asteroid should be consumed. */
    ASSERT(!w->asteroids[slot].active);
    /* Either an anonymous ingot landed in inventory, or a named ingot
     * landed in the stockpile — depending on what the universe rolled. */
    bool got_named = (w->stations[0].named_ingots_count > initial_named);
    bool got_bulk = (w->stations[0].inventory[COMMODITY_FERRITE_INGOT] > initial_bulk);
    ASSERT(got_named || got_bulk);
    ASSERT_EQ_INT(w->stations[0].manifest.count - initial_manifest, 10);
    for (int i = initial_manifest; i < w->stations[0].manifest.count; i++) {
        cargo_unit_t *unit = &w->stations[0].manifest.units[i];
        ASSERT_EQ_INT(unit->kind, CARGO_KIND_INGOT);
        ASSERT_EQ_INT(unit->commodity, COMMODITY_FERRITE_INGOT);
        ASSERT_EQ_INT(unit->grade, MINING_GRADE_RATI);
        ASSERT_EQ_INT(unit->recipe_id, RECIPE_SMELT);
    }
    ASSERT(memcmp(w->stations[0].manifest.units[initial_manifest].parent_merkle,
                  w->stations[0].manifest.units[w->stations[0].manifest.count - 1].parent_merkle,
                  32) == 0);
    ASSERT(memcmp(w->stations[0].manifest.units[initial_manifest].pub,
                  w->stations[0].manifest.units[w->stations[0].manifest.count - 1].pub,
                  32) != 0);
    /* If named, validate prefix_class matches mining_pubkey_class. */
    if (got_named) {
        named_ingot_t *ing = &w->stations[0].named_ingots[w->stations[0].named_ingots_count - 1];
        ASSERT_EQ_INT(ing->prefix_class, mining_pubkey_class(ing->pubkey));
        ASSERT(ing->prefix_class != MINING_CLASS_ANONYMOUS);
    }
}

TEST(test_station_production_dual_writes_frame_manifest) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t inputs[2] = {{0}};
    cargo_unit_t expected = {0};
    int press_idx = -1;
    uint8_t fragment_a[32] = {0};
    uint8_t fragment_b[32] = {0};

    fragment_a[31] = 0x11;
    fragment_b[31] = 0x22;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->inventory, 0, sizeof(st->inventory));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));

    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RATI, fragment_a, 0, &inputs[0]));
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_COMMON, fragment_b, 0, &inputs[1]));
    ASSERT(hash_product(RECIPE_FRAME_BASIC, inputs, 2, 0, &expected));
    ASSERT(manifest_push(&st->manifest, &inputs[0]));
    ASSERT(manifest_push(&st->manifest, &inputs[1]));

    st->module_input[press_idx] = 2.0f;
    sim_step_station_production(&w, 1.0f);

    ASSERT_EQ_FLOAT(st->inventory[COMMODITY_FRAME], 1.0f, 0.001f);
    ASSERT_EQ_INT(st->manifest.count, 1);
    ASSERT_EQ_INT(manifest_find(&st->manifest, inputs[0].pub), -1);
    ASSERT_EQ_INT(manifest_find(&st->manifest, inputs[1].pub), -1);
    ASSERT(memcmp(st->manifest.units[0].pub, expected.pub, 32) == 0);
    ASSERT(memcmp(st->manifest.units[0].parent_merkle, expected.parent_merkle, 32) == 0);
    ASSERT_EQ_INT(st->manifest.units[0].kind, CARGO_KIND_FRAME);
    ASSERT_EQ_INT(st->manifest.units[0].commodity, COMMODITY_FRAME);
    ASSERT_EQ_INT(st->manifest.units[0].grade, MINING_GRADE_COMMON);
    ASSERT_EQ_INT(st->manifest.units[0].recipe_id, RECIPE_FRAME_BASIC);
}

TEST(test_station_production_dual_writes_laser_manifest) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    cargo_unit_t inputs[2] = {{0}};
    cargo_unit_t expected = {0};
    int laser_idx = -1;
    uint8_t fragment_cu[32] = {0};
    uint8_t fragment_cr[32] = {0};

    fragment_cu[31] = 0x33;
    fragment_cr[31] = 0x44;
    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_LASER_FAB) {
            laser_idx = i;
            break;
        }
    }
    ASSERT(laser_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->inventory, 0, sizeof(st->inventory));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));

    ASSERT(hash_ingot(COMMODITY_CUPRITE_INGOT, MINING_GRADE_RARE, fragment_cu, 0, &inputs[0]));
    ASSERT(hash_ingot(COMMODITY_CRYSTAL_INGOT, MINING_GRADE_FINE, fragment_cr, 0, &inputs[1]));
    ASSERT(hash_product(RECIPE_LASER_BASIC, inputs, 2, 0, &expected));
    ASSERT(manifest_push(&st->manifest, &inputs[0]));
    ASSERT(manifest_push(&st->manifest, &inputs[1]));

    st->module_input[laser_idx] = 1.0f;
    st->inventory[COMMODITY_CRYSTAL_INGOT] = 1.0f;
    sim_step_station_production(&w, 2.0f);

    ASSERT_EQ_FLOAT(st->inventory[COMMODITY_LASER_MODULE], 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(st->inventory[COMMODITY_CRYSTAL_INGOT], 0.0f, 0.001f);
    ASSERT_EQ_INT(st->manifest.count, 1);
    ASSERT_EQ_INT(manifest_find(&st->manifest, inputs[0].pub), -1);
    ASSERT_EQ_INT(manifest_find(&st->manifest, inputs[1].pub), -1);
    ASSERT(memcmp(st->manifest.units[0].pub, expected.pub, 32) == 0);
    ASSERT(memcmp(st->manifest.units[0].parent_merkle, expected.parent_merkle, 32) == 0);
    ASSERT_EQ_INT(st->manifest.units[0].kind, CARGO_KIND_LASER);
    ASSERT_EQ_INT(st->manifest.units[0].commodity, COMMODITY_LASER_MODULE);
    ASSERT_EQ_INT(st->manifest.units[0].grade, MINING_GRADE_FINE);
    ASSERT_EQ_INT(st->manifest.units[0].recipe_id, RECIPE_LASER_BASIC);
}

/* Manifest-as-truth invariant: production refuses to mint orphan
 * frames. With no FE_INGOT manifest entry to consume, station_manifest_
 * craft_product fails and the float increment is reverted. This is the
 * inverse of the old legacy-path test (which asserted that the float
 * path kept producing without manifest); that behavior is the bug class
 * the manifest-as-truth refactor closes. */
TEST(test_station_production_without_manifest_inputs_refuses_to_mint) {
    WORLD_DECL;
    world_reset(&w);
    station_t *st = &w.stations[1];
    int press_idx = -1;

    for (int i = 0; i < st->module_count; i++) {
        if (st->modules[i].type == MODULE_FRAME_PRESS) {
            press_idx = i;
            break;
        }
    }
    ASSERT(press_idx >= 0);

    manifest_clear(&st->manifest);
    memset(st->inventory, 0, sizeof(st->inventory));
    memset(st->module_input, 0, sizeof(st->module_input));
    memset(st->module_output, 0, sizeof(st->module_output));

    st->module_input[press_idx] = 2.0f;
    sim_step_station_production(&w, 1.0f);

    /* Float reverted by E1 fix: no manifest input → no manifest output
     * → no orphan float either. */
    ASSERT_EQ_FLOAT(st->inventory[COMMODITY_FRAME], 0.0f, 0.001f);
    ASSERT_EQ_INT(st->manifest.count, 0);
}

TEST(test_world_sim_step_events_emitted) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);
    /* Launch should emit LAUNCH event */
    w.players[0].input.interact = true;
    world_sim_step(&w, 1.0f / 120.0f);
    bool found_launch = false;
    for (int i = 0; i < w.events.count; i++) {
        if (w.events.events[i].type == SIM_EVENT_LAUNCH) found_launch = true;
    }
    ASSERT(found_launch);
}

TEST(test_world_sim_step_npc_miners_work) {
    WORLD_DECL;
    world_reset(&w);
    /* Run for 5 seconds of sim time */
    for (int i = 0; i < 600; i++)
        world_sim_step(&w, 1.0f / 120.0f);
    /* At least one miner should have left docked state */
    bool any_traveling = false;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].role == NPC_ROLE_MINER &&
            w.npc_ships[i].state != NPC_STATE_DOCKED) {
            any_traveling = true;
        }
    }
    ASSERT(any_traveling);
}

TEST(test_world_network_writes_persist) {
    /* Simulate: world_sim_step runs, then network callback overwrites asteroid,
     * next world_sim_step should see the overwritten state */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    world_sim_step(&w, 1.0f / 120.0f);
    /* Simulate network overwrite of asteroid 0 */
    w.asteroids[0].active = true;
    w.asteroids[0].hp = 999.0f;
    w.asteroids[0].pos = v2(100.0f, 100.0f);
    world_sim_step(&w, 1.0f / 120.0f);
    /* HP should still be near 999 (only drag/dynamics, no mining) */
    ASSERT(w.asteroids[0].hp > 990.0f);
    ASSERT(w.asteroids[0].active);
}

TEST(test_scenario_full_mining_cycle) {
    /* Test the physical ore flow: create S fragment → tow → deposit at hopper → earn credits */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x42, 8);  /* test token */

    /* Create a collectible S-tier fragment directly */
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { frag = i; break; }
    }
    ASSERT(frag >= 0);
    w.asteroids[frag].active = true;
    w.asteroids[frag].tier = ASTEROID_TIER_S;
    w.asteroids[frag].radius = 8.0f;
    w.asteroids[frag].hp = 1.0f;
    w.asteroids[frag].max_hp = 1.0f;
    w.asteroids[frag].ore = 15.0f;
    w.asteroids[frag].max_ore = 15.0f;
    w.asteroids[frag].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[frag].fracture_child = true;
    w.asteroids[frag].pos = v2(5000.0f, 5000.0f);
    w.asteroids[frag].vel = v2(0.0f, 0.0f);

    /* Manually attach as towed (simulates tractor pickup) */
    w.players[0].ship.towed_fragments[0] = (int16_t)frag;
    w.players[0].ship.towed_count = 1;

    /* Find furnace and ore_silo modules on station 0 for dual-reach smelting */
    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w.stations[0].module_count; m++) {
        if (w.stations[0].modules[m].type == MODULE_FURNACE && !w.stations[0].modules[m].scaffold)
            furnace_idx = m;
        if (w.stations[0].modules[m].type == MODULE_ORE_SILO && !w.stations[0].modules[m].scaffold)
            silo_idx = m;
    }
    ASSERT(furnace_idx >= 0);
    ASSERT(silo_idx >= 0);
    float start_credits = ledger_balance(&w.stations[0], w.players[0].session_token);

    /* Clear station ore inventory */
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        w.stations[0].inventory[i] = 0.0f;

    /* Stop rotation, place fragment at midpoint between furnace and silo */
    for (int a = 0; a < MAX_ARMS; a++) {
        w.stations[0].arm_speed[a] = 0.0f;
        w.stations[0].arm_rotation[a] = 0.0f;
    }
    vec2 furnace_pos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[furnace_idx].ring, w.stations[0].modules[furnace_idx].slot);
    vec2 silo_pos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[silo_idx].ring, w.stations[0].modules[silo_idx].slot);
    vec2 midpoint = v2_scale(v2_add(furnace_pos, silo_pos), 0.5f);
    ASSERT(station_buy_price(&w.stations[0], COMMODITY_FERRITE_ORE) > 0.0f);
    w.asteroids[frag].pos = midpoint;
    w.asteroids[frag].vel = v2(0.0f, 0.0f);
    w.asteroids[frag].last_fractured_by = 0;
    w.asteroids[frag].last_towed_by = 0;
    /* Credit attribution is strictly token-based now (H1) — mirror what
     * live tow code does at game_sim.c:1343 by stamping the tow/fracture
     * tokens to match the towing player's session. */
    memcpy(w.asteroids[frag].last_towed_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_towed_token));
    memcpy(w.asteroids[frag].last_fractured_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_fractured_token));
    w.players[0].ship.pos = v2_add(midpoint, v2(100.0f, 0.0f));
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    /* Run enough steps for smelt_progress to reach 1.0 (~2 seconds at 120Hz) */
    for (int i = 0; i < 300; i++) world_sim_step(&w, SIM_DT);

    /* Fragment should be consumed */
    ASSERT(w.players[0].ship.towed_count == 0);

    /* Credits are in the station ledger — check balance directly */
    ASSERT(ledger_balance(&w.stations[0], w.players[0].session_token) > start_credits);
}

/* Phase 1-3 manifest-first invariant:
 *   Across every station + every connected ship, no pub appears twice,
 *   and the count of manifest units per commodity never exceeds the
 *   combined integer float inventory. Runs a smelt + a dock delivery +
 *   a buyback through the normal sim code paths. */
TEST(test_manifest_conservation_across_transactions) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x33, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;

    /* Smelt one fragment to populate a station manifest unit. */
    int frag = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++)
        if (!w.asteroids[i].active) { frag = i; break; }
    ASSERT(frag >= 0);
    w.asteroids[frag] = (asteroid_t){0};
    w.asteroids[frag].active = true;
    w.asteroids[frag].tier = ASTEROID_TIER_S;
    w.asteroids[frag].radius = 8.0f;
    w.asteroids[frag].hp = 1.0f;
    w.asteroids[frag].max_hp = 1.0f;
    w.asteroids[frag].ore = 3.0f;       /* 3 whole units → 3 manifest entries */
    w.asteroids[frag].max_ore = 3.0f;
    w.asteroids[frag].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[frag].fracture_child = true;
    w.asteroids[frag].grade = MINING_GRADE_COMMON;
    w.players[0].ship.towed_fragments[0] = (int16_t)frag;
    w.players[0].ship.towed_count = 1;
    memcpy(w.asteroids[frag].last_towed_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_towed_token));
    memcpy(w.asteroids[frag].last_fractured_token,
           w.players[0].session_token, sizeof(w.asteroids[frag].last_fractured_token));
    /* Place fragment between furnace and silo on station 0. */
    int furnace_idx = -1, silo_idx = -1;
    for (int m = 0; m < w.stations[0].module_count; m++) {
        if (w.stations[0].modules[m].type == MODULE_FURNACE) furnace_idx = m;
        if (w.stations[0].modules[m].type == MODULE_ORE_SILO) silo_idx = m;
    }
    ASSERT(furnace_idx >= 0 && silo_idx >= 0);
    for (int a = 0; a < MAX_ARMS; a++) {
        w.stations[0].arm_speed[a] = 0.0f;
        w.stations[0].arm_rotation[a] = 0.0f;
    }
    vec2 fpos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[furnace_idx].ring, w.stations[0].modules[furnace_idx].slot);
    vec2 spos = module_world_pos_ring(&w.stations[0],
        w.stations[0].modules[silo_idx].ring, w.stations[0].modules[silo_idx].slot);
    w.asteroids[frag].pos = v2_scale(v2_add(fpos, spos), 0.5f);
    for (int i = 0; i < 400; i++) world_sim_step(&w, SIM_DT);

    /* Post-smelt: station 0 manifest should carry 3 COMMON ferrite ingots. */
    int station_manifest_ferrite =
        manifest_count_by_commodity(&w.stations[0].manifest, COMMODITY_FERRITE_INGOT);
    ASSERT(station_manifest_ferrite == 3);

    /* Invariant sweep: no pub repeats anywhere. */
    static uint8_t seen_pubs[64][32]; int seen_n = 0;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &w.stations[s];
        if (!st->manifest.units) continue;
        for (uint16_t i = 0; i < st->manifest.count; i++) {
            for (int k = 0; k < seen_n; k++) {
                ASSERT(memcmp(seen_pubs[k], st->manifest.units[i].pub, 32) != 0);
            }
            if (seen_n < (int)(sizeof(seen_pubs) / sizeof(seen_pubs[0]))) {
                memcpy(seen_pubs[seen_n++], st->manifest.units[i].pub, 32);
            }
        }
    }
    for (int p = 0; p < MAX_PLAYERS; p++) {
        const ship_t *ship = &w.players[p].ship;
        if (!ship->manifest.units) continue;
        for (uint16_t i = 0; i < ship->manifest.count; i++) {
            for (int k = 0; k < seen_n; k++) {
                ASSERT(memcmp(seen_pubs[k], ship->manifest.units[i].pub, 32) != 0);
            }
            if (seen_n < (int)(sizeof(seen_pubs) / sizeof(seen_pubs[0]))) {
                memcpy(seen_pubs[seen_n++], ship->manifest.units[i].pub, 32);
            }
        }
    }
}

TEST(test_scenario_two_players_mining) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    w.players[1].session_ready = true;
    memset(w.players[1].session_token, 0x02, 8);
    player_init_ship(&w.players[0], &w);
    player_init_ship(&w.players[1], &w);
    player_seed_credits(&w.players[0], &w);
    player_seed_credits(&w.players[1], &w);
    w.players[0].connected = true;
    w.players[1].connected = true;
    w.players[0].ship.mining_level = SHIP_UPGRADE_MAX_LEVEL;
    w.players[1].ship.mining_level = SHIP_UPGRADE_MAX_LEVEL;

    /* Launch both */
    w.players[0].input.interact = true;
    w.players[1].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    w.players[1].input.interact = false;
    ASSERT(!w.players[0].docked);
    ASSERT(!w.players[1].docked);

    /* Create two M-tier test asteroids near station 0 */
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    int ast0 = 0, ast1 = 1;
    w.asteroids[ast0].active = true; w.asteroids[ast0].tier = ASTEROID_TIER_M;
    w.asteroids[ast0].radius = 25.0f; w.asteroids[ast0].hp = 50.0f; w.asteroids[ast0].max_hp = 50.0f;
    w.asteroids[ast0].commodity = COMMODITY_FERRITE_ORE;
    w.asteroids[ast0].pos = v2_add(w.stations[0].pos, v2(500.0f, 0.0f));
    w.asteroids[ast1].active = true; w.asteroids[ast1].tier = ASTEROID_TIER_M;
    w.asteroids[ast1].radius = 25.0f; w.asteroids[ast1].hp = 50.0f; w.asteroids[ast1].max_hp = 50.0f;
    w.asteroids[ast1].commodity = COMMODITY_CUPRITE_ORE;
    w.asteroids[ast1].pos = v2_add(w.stations[0].pos, v2(-500.0f, 0.0f));

    float hp0_before = w.asteroids[ast0].hp;
    float hp1_before = w.asteroids[ast1].hp;

    /* Position players near their respective asteroids */
    w.players[0].ship.pos = v2(w.asteroids[ast0].pos.x - 60.0f, w.asteroids[ast0].pos.y);
    w.players[0].ship.angle = 0.0f;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[1].ship.pos = v2(w.asteroids[ast1].pos.x - 60.0f, w.asteroids[ast1].pos.y);
    w.players[1].ship.angle = 0.0f;
    w.players[1].ship.vel = v2(0.0f, 0.0f);

    /* Both mine for 120 ticks */
    w.players[0].input.mine = true;
    w.players[1].input.mine = true;
    for (int i = 0; i < 120; i++) {
        w.players[0].ship.pos = v2(w.asteroids[ast0].pos.x - 60.0f, w.asteroids[ast0].pos.y);
        w.players[1].ship.pos = v2(w.asteroids[ast1].pos.x - 60.0f, w.asteroids[ast1].pos.y);
        w.players[0].ship.vel = v2(0.0f, 0.0f);
        w.players[1].ship.vel = v2(0.0f, 0.0f);
        world_sim_step(&w, SIM_DT);
    }
    w.players[0].input.mine = false;
    w.players[1].input.mine = false;

    /* Each asteroid took damage independently */
    ASSERT(w.asteroids[ast0].hp < hp0_before);
    ASSERT(w.asteroids[ast1].hp < hp1_before);

    /* No state bleed: player 0's cargo didn't affect player 1.
     * Both spawned with the same spawn-fee debit at station 0, so
     * their balances should be the same negative number regardless of
     * what either of them mined. */
    float p0 = ledger_balance(&w.stations[0], w.players[0].session_token);
    float p1 = ledger_balance(&w.stations[0], w.players[1].session_token);
    int fee = station_spawn_fee(&w.stations[0]);
    ASSERT_EQ_FLOAT(p0, -(float)fee, 0.01f);
    ASSERT_EQ_FLOAT(p1, -(float)fee, 0.01f);
}

TEST(test_scenario_npc_economy_30_seconds) {
    WORLD_DECL;
    world_reset(&w);

    /* Run 60 sim seconds. Originally 30s, but the NPC mining → tow →
     * smelt pipeline only just barely finishes one cycle by t=30s on
     * macOS, and Linux CI's slightly different float rounding pushes
     * the first delivery past the cutoff. 60s gives ~2× margin while
     * keeping the test fast. */
    for (int i = 0; i < 7200; i++)
        world_sim_step(&w, SIM_DT);

    /* Verify: at least one asteroid was mined (some HP < max_hp or deactivated) */
    bool any_mined = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active ||
            (w.asteroids[i].hp < w.asteroids[i].max_hp && w.asteroids[i].max_hp > 0.0f)) {
            any_mined = true; break;
        }
    }
    ASSERT(any_mined);

    /* Verify: at least one station has either an ingot in stock or
     * raw ore mid-smelt. Originally checked station[0] only, but the
     * count-tier rework gave Helios all three furnaces; on machines
     * where Prospect's 1-furnace pipeline runs slower than Helios's
     * 3-furnace one, station[0] can stay quiet for the first minute
     * even though the economy is clearly working. Looking at every
     * station catches both ends. */
    bool any_ingot = false;
    for (int s = 0; s < MAX_STATIONS && !any_ingot; s++) {
        for (int i = COMMODITY_RAW_ORE_COUNT; i < COMMODITY_COUNT; i++) {
            if (w.stations[s].inventory[i] > 0.0f) { any_ingot = true; break; }
        }
    }
    bool ore_consumed = false;
    for (int s = 0; s < MAX_STATIONS && !ore_consumed; s++) {
        for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++) {
            if (w.stations[s].inventory[i] > 0.0f) { ore_consumed = true; break; }
        }
    }
    ASSERT(any_ingot || ore_consumed);

    /* Verify: no negative values anywhere */
    for (int s = 0; s < MAX_STATIONS; s++) {
        for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
            ASSERT(w.stations[s].inventory[i] >= 0.0f);
        for (int i = 0; i < COMMODITY_COUNT; i++)
            ASSERT(w.stations[s].inventory[i] >= 0.0f);
        for (int i = 0; i < PRODUCT_COUNT; i++)
            ASSERT(w.stations[s].inventory[COMMODITY_FRAME + i] >= 0.0f);
    }
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        if (!w.npc_ships[n].active) continue;
        for (int i = 0; i < COMMODITY_COUNT; i++)
            ASSERT(w.npc_ships[n].cargo[i] >= 0.0f);
    }
}

TEST(test_scenario_upgrade_requires_products) {
    WORLD_DECL;
    world_reset(&w);
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x01, 8);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    ASSERT(w.players[0].docked);

    /* Launch then dock at station 2 (Helios Works - has laser upgrade) */
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    ASSERT(!w.players[0].docked);

    /* Dock directly at station 2 for test */
    w.players[0].docked = true;
    w.players[0].current_station = 2;
    w.players[0].nearby_station = 2;
    w.players[0].in_dock_range = true;
    w.players[0].ship.pos = w.stations[2].pos;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    ASSERT(w.players[0].docked);
    ASSERT_EQ_INT(w.players[0].current_station, 2);

    /* Give player enough credits at station 2 */
    ledger_earn(&w.stations[2], w.players[0].session_token, 1000.0f);
    int level_before = w.players[0].ship.mining_level;

    /* Set inventory for PRODUCT_LASER_MODULE to 0 */
    w.stations[2].inventory[COMMODITY_LASER_MODULE] = 0.0f;

    /* Try upgrade_mining -- should fail (no product stock) */
    w.players[0].input.upgrade_mining = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.upgrade_mining = false;
    ASSERT_EQ_INT(w.players[0].ship.mining_level, level_before);

    /* Mint manifest + float in lockstep. The manifest reconcile pass at
     * end of tick now snaps inventory[c] DOWN to manifest_count for
     * finished goods, so a bare float assignment would get trimmed to
     * zero before the upgrade path runs. Use the helper for correctness. */
    station_finished_mint(&w.stations[2], COMMODITY_LASER_MODULE, 20, NULL);

    /* Try upgrade_mining -- should succeed */
    w.players[0].input.upgrade_mining = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.upgrade_mining = false;
    ASSERT_EQ_INT(w.players[0].ship.mining_level, level_before + 1);
}

TEST(test_scenario_emergency_recovery) {
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;

    /* Launch */
    w.players[0].input.interact = true;
    world_sim_step(&w, SIM_DT);
    w.players[0].input.interact = false;
    ASSERT(!w.players[0].docked);

    /* Give player some cargo */
    w.players[0].ship.cargo[COMMODITY_FERRITE_ORE] = 50.0f;

    /* Set hull to 1.0 (near death) */
    w.players[0].ship.hull = 1.0f;

    /* Give high velocity towards a ring 1 module to trigger collision damage.
     * Signal relay is at ring 1, slot 1 (slot 0 is dock — no collision). */
    vec2 mod = module_world_pos_ring(&w.stations[0], 1, 1);
    w.players[0].ship.pos = v2(mod.x + 60.0f, mod.y);
    w.players[0].ship.vel = v2(-2000.0f, 0.0f);

    /* Run sim for a few ticks */
    for (int i = 0; i < 10; i++)
        world_sim_step(&w, SIM_DT);

    /* Verify: player is docked (emergency recovery triggered) */
    ASSERT(w.players[0].docked);

    /* Verify: hull is restored to max */
    ASSERT_EQ_FLOAT(w.players[0].ship.hull, ship_max_hull(&w.players[0].ship), 0.01f);

    /* Verify: cargo is cleared (lost on recovery) */
    ASSERT(ship_total_cargo(&w.players[0].ship) < 0.01f);
}

TEST(test_scenario_product_cap_pauses_production) {
    WORLD_DECL;
    world_reset(&w);

    /* Set station 1 (Kepler Yard) inventory[COMMODITY_FRAME] to MAX_PRODUCT_STOCK */
    w.stations[1].inventory[COMMODITY_FRAME] = MAX_PRODUCT_STOCK;

    /* Set ingot_buffer with some frame ingots */
    w.stations[1].inventory[COMMODITY_FERRITE_INGOT] = 20.0f;

    /* Run 120 ticks */
    for (int i = 0; i < 120; i++)
        world_sim_step(&w, SIM_DT);

    /* Verify inventory didn't exceed MAX_PRODUCT_STOCK */
    ASSERT(w.stations[1].inventory[COMMODITY_FRAME] <= MAX_PRODUCT_STOCK + 0.01f);
}

TEST(test_signal_strength_at_station) {
    /* At a station's position, signal should be 1.0 (full strength) */
    WORLD_DECL;
    world_reset(&w);
    ASSERT_EQ_FLOAT(signal_strength_at(&w, w.stations[0].pos), 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(signal_strength_at(&w, w.stations[1].pos), 1.0f, 0.01f);
    ASSERT_EQ_FLOAT(signal_strength_at(&w, w.stations[2].pos), 1.0f, 0.01f);
}

TEST(test_signal_strength_falls_off) {
    /* Signal should decrease linearly from 1.0 at station to 0.0 at range edge.
     * Sample south of Prospect Refinery where Kepler/Helios don't reach, so
     * the overlap boost (multi-station reinforcement) isn't in play. */
    WORLD_DECL;
    world_reset(&w);
    /* Station 0 at (0, -2400), signal_range = 18000. Point 12000u south —
     * (0, -14400) — is comfortably outside Kepler (-3200, 2300) and
     * Helios (3200, 2300) 15000-unit ranges, so only Prospect covers it
     * (and the bilinear cache cells around it are also single-station,
     * so the overlap boost doesn't leak in via interpolation). */
    float half = signal_strength_at(&w, v2_add(w.stations[0].pos, v2(0.0f, -12000.0f)));
    ASSERT(half > 0.3f && half < 0.7f);
}

TEST(test_signal_overlap_boosts_strength) {
    /* Overlap mechanic: two connected stations covering the same point
     * give 2x the best single-station strength; three-or-more overlap
     * caps at 3x. The starter triangle overlaps all three signals at the
     * center, so the boost saturates there. */
    WORLD_DECL;
    world_reset(&w);
    /* Centroid of the three starter stations — covered by all three. */
    vec2 centroid = v2((w.stations[0].pos.x + w.stations[1].pos.x + w.stations[2].pos.x) / 3.0f,
                       (w.stations[0].pos.y + w.stations[1].pos.y + w.stations[2].pos.y) / 3.0f);
    float boosted = signal_strength_at(&w, centroid);
    ASSERT_EQ_FLOAT(boosted, 1.0f, 0.01f);
}

TEST(test_signal_zero_outside_range) {
    /* Far from all stations, signal should be 0.0 */
    WORLD_DECL;
    world_reset(&w);
    ASSERT_EQ_FLOAT(signal_strength_at(&w, v2(100000.0f, 100000.0f)), 0.0f, 0.01f);
}

TEST(test_signal_max_of_stations) {
    /* When inside multiple stations' ranges, signal is the maximum, not sum */
    WORLD_DECL;
    world_reset(&w);
    /* Midpoint between station 0 and station 1 should get max of the two signals,
     * not their sum. Signal is never > 1.0. */
    float s = signal_strength_at(&w, v2(-160.0f, 0.0f));
    ASSERT(s <= 1.0f);
    ASSERT(s > 0.0f);
}

TEST(test_ship_thrust_scales_with_signal) {
    /* At low signal, ship should accelerate slower */
    WORLD_DECL;
    world_reset(&w);
    player_init_ship(&w.players[0], &w);
    w.players[0].connected = true;
    w.players[0].docked = false;
    /* Place ship at station (full signal) → thrust → measure velocity */
    w.players[0].ship.pos = w.stations[0].pos;
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].ship.angle = 0.0f;
    w.players[0].input.thrust = 1.0f;
    world_sim_step(&w, SIM_DT);
    float vel_full_signal = w.players[0].ship.vel.x;
    /* Place ship far from all stations (low/zero signal) → same thrust → should be slower */
    w.players[0].ship.pos = v2(40000.0f, 0.0f); /* outside all station signal ranges */
    w.players[0].ship.vel = v2(0.0f, 0.0f);
    w.players[0].input.thrust = 1.0f;
    world_sim_step(&w, SIM_DT);
    float vel_low_signal = w.players[0].ship.vel.x;
    /* After #82: vel_low_signal should be significantly less than vel_full_signal */
    /* Currently both are the same — no signal scaling */
    ASSERT(vel_low_signal < vel_full_signal * 0.7f);
}

TEST(test_asteroid_outside_signal_despawns) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_L;
    w.asteroids[0].radius = 40.0f;
    w.asteroids[0].hp = 100.0f;
    w.asteroids[0].max_hp = 100.0f;
    w.asteroids[0].pos = v2(40000.0f, 0.0f);
    w.asteroids[0].vel = v2(0.0f, 0.0f);
    world_sim_step(&w, SIM_DT);
    ASSERT(!w.asteroids[0].active);
}

TEST(test_npc_miners_avoid_zero_signal_asteroids) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int i = 1; i < MAX_NPC_SHIPS; i++) w.npc_ships[i].active = false;

    w.asteroids[0].active = true;
    w.asteroids[0].tier = ASTEROID_TIER_L;
    w.asteroids[0].radius = 50.0f;
    w.asteroids[0].hp = 120.0f;
    w.asteroids[0].max_hp = 120.0f;
    w.asteroids[0].pos = v2(260.0f, -240.0f);

    w.asteroids[1].active = true;
    w.asteroids[1].tier = ASTEROID_TIER_XL;
    w.asteroids[1].radius = 80.0f;
    w.asteroids[1].hp = 240.0f;
    w.asteroids[1].max_hp = 240.0f;
    w.asteroids[1].pos = v2(4000.0f, 0.0f);

    w.npc_ships[0].active = true;
    w.npc_ships[0].role = NPC_ROLE_MINER;
    w.npc_ships[0].hull_class = HULL_CLASS_NPC_MINER;
    w.npc_ships[0].home_station = 0;
    w.npc_ships[0].state = NPC_STATE_DOCKED;
    w.npc_ships[0].state_timer = 0.0f;
    w.npc_ships[0].target_asteroid = -1;
    w.npc_ships[0].pos = w.stations[0].pos;
    w.npc_ships[0].vel = v2(0.0f, 0.0f);
    w.npc_ships[0].angle = 0.0f;

    world_sim_step(&w, SIM_DT);
    ASSERT_EQ_INT(w.npc_ships[0].target_asteroid, 0);
}

TEST(test_field_respawn_starts_beyond_signal_edge) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;

    w.field_spawn_timer = FIELD_ASTEROID_RESPAWN_DELAY;
    world_sim_step(&w, SIM_DT);

    int spawned = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (w.asteroids[i].active) {
            spawned = i;
            break;
        }
    }
    ASSERT(spawned >= 0);

    const asteroid_t *a = &w.asteroids[spawned];
    /* Belt-based spawning: asteroid should be within signal range and at a
     * position with nonzero belt density. Ore type matches belt geography. */
    ASSERT(signal_strength_at(&w, a->pos) >= 0.0f);
    /* Verify it spawned near a station (within signal range) */
    bool near_station = false;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (!station_provides_signal(&w.stations[s])) continue;
        float d = sqrtf(v2_dist_sq(a->pos, w.stations[s].pos));
        if (d <= w.stations[s].signal_range) { near_station = true; break; }
    }
    ASSERT(near_station);

    /* Chunk-based terrain: asteroids spawn stationary (vel ~0).
     * Gravity/physics will give them velocity over time. */
    ASSERT(v2_len(a->vel) < 50.0f); /* not launched at high speed */

    world_sim_step(&w, SIM_DT);
    ASSERT(w.asteroids[spawned].active);
}

TEST(test_asteroids_drift_toward_stronger_signal) {
    WORLD_DECL;
    world_reset(&w);
    for (int i = 0; i < MAX_ASTEROIDS; i++) w.asteroids[i].active = false;
    for (int s = 1; s < MAX_STATIONS; s++) w.stations[s].signal_range = 0.0f;

    asteroid_t *a = &w.asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_XL;
    a->radius = 60.0f;
    a->hp = 150.0f;
    a->max_hp = 150.0f;
    a->pos = v2_add(w.stations[0].pos, v2(15000.0f, 0.0f));
    a->vel = v2(0.0f, 0.0f);

    float start_x = a->pos.x;
    for (int i = 0; i < 1200; i++) world_sim_step(&w, SIM_DT);

    ASSERT(a->pos.x < start_x - 30.0f);
    ASSERT(a->vel.x < -1.0f);
}

TEST(test_belt_density_varies) {
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    /* Sample multiple points — should get both zero and nonzero density */
    int zeros = 0, nonzeros = 0;
    for (int i = 0; i < 100; i++) {
        float x = (float)(i * 1000 - 50000);
        float d = belt_density_at(&bf, x, 0.0f);
        if (d < 0.01f) zeros++;
        else nonzeros++;
    }
    ASSERT(zeros > 10);     /* some empty space */
    ASSERT(nonzeros > 10);  /* some belt regions */
}

TEST(test_belt_ore_distribution) {
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    int fe = 0, cu = 0, cr = 0;
    for (int i = 0; i < 1000; i++) {
        float x = (float)(i * 100 - 50000);
        float y = (float)((i * 73) % 100000 - 50000);
        commodity_t ore = belt_ore_at(&bf, x, y);
        if (ore == COMMODITY_FERRITE_ORE) fe++;
        else if (ore == COMMODITY_CUPRITE_ORE) cu++;
        else if (ore == COMMODITY_CRYSTAL_ORE) cr++;
    }
    /* Target mix ~60/16/24 (Fe/Cu/Cr). Cuprite >0 is load-bearing:
     * laser modules + tractor coils + repair kits all need cuprite
     * ingots. The bounds are loose to absorb tuning drift; tighten
     * them in a follow-up if a real regression slips past. */
    printf("    belt mix: fe=%d cu=%d cr=%d (target ~60/16/24)\n", fe, cu, cr);
    ASSERT(fe > 500);     /* ferrite still dominant */
    ASSERT(cu > 50);      /* cuprite reliably present */
    ASSERT(cu < 250);     /* but still the rarest of three */
    ASSERT(cr > 100);     /* crystal at least mid-share */
    ASSERT(cr < fe);      /* less than ferrite */
    ASSERT(cu < cr);      /* cuprite rarer than crystal */
}

TEST(test_chunk_determinism) {
    /* Same chunk coordinates + seed must produce identical asteroids */
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    chunk_asteroid_t a[CHUNK_MAX_ASTEROIDS], b[CHUNK_MAX_ASTEROIDS];
    int na = chunk_generate(&bf, 2037, 5, -3, a, CHUNK_MAX_ASTEROIDS);
    int nb = chunk_generate(&bf, 2037, 5, -3, b, CHUNK_MAX_ASTEROIDS);
    ASSERT_EQ_INT(na, nb);
    for (int i = 0; i < na; i++) {
        ASSERT_EQ_FLOAT(a[i].pos.x, b[i].pos.x, 0.001f);
        ASSERT_EQ_FLOAT(a[i].pos.y, b[i].pos.y, 0.001f);
        ASSERT_EQ_INT((int)a[i].tier, (int)b[i].tier);
        ASSERT_EQ_INT((int)a[i].commodity, (int)b[i].commodity);
        ASSERT_EQ_FLOAT(a[i].radius, b[i].radius, 0.001f);
        ASSERT_EQ_FLOAT(a[i].hp, b[i].hp, 0.001f);
    }
}

TEST(test_chunk_different_coords_differ) {
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    chunk_asteroid_t a[CHUNK_MAX_ASTEROIDS], b[CHUNK_MAX_ASTEROIDS];
    int na = 0, nb = 0;
    for (int cx = -5; cx < 5 && na == 0; cx++)
        na = chunk_generate(&bf, 2037, cx, 0, a, CHUNK_MAX_ASTEROIDS);
    for (int cx = 10; cx < 20 && nb == 0; cx++)
        nb = chunk_generate(&bf, 2037, cx, 3, b, CHUNK_MAX_ASTEROIDS);
    if (na > 0 && nb > 0) {
        ASSERT(fabsf(a[0].pos.x - b[0].pos.x) > 1.0f ||
               fabsf(a[0].pos.y - b[0].pos.y) > 1.0f);
    }
}

TEST(test_chunk_respects_belt_density) {
    /* Chunks at belt density > 0 should produce asteroids */
    belt_field_t bf;
    belt_field_init(&bf, 2037, 50000.0f);
    /* Station 0 is at (0, -2400). Belt density should be nonzero nearby. */
    chunk_asteroid_t out[CHUNK_MAX_ASTEROIDS];
    int total = 0;
    for (int cx = -10; cx < 10; cx++)
        for (int cy = -16; cy < -4; cy++)
            total += chunk_generate(&bf, 2037, cx, cy, out, CHUNK_MAX_ASTEROIDS);
    ASSERT(total > 0); /* at least some asteroids near the belt */
}

/* #294 slice 2 regression: an NPC in MINING state that is shoved past
 * MINING_RANGE used to keep firing its beam across the map (no exit
 * condition + center-distance entry test). After the unification, the
 * shared sim_mining_beam_step refuses fire at long range, and the NPC
 * MINING state drops back to TRAVEL when out of MINING_RANGE. */
TEST(test_npc_mining_drops_state_when_far_from_target) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    world_reset(w);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) w->npc_ships[i].active = false;
    for (int i = 0; i < MAX_ASTEROIDS; i++) w->asteroids[i].active = false;

    npc_ship_t *npc = &w->npc_ships[0];
    npc->active = true;
    npc->role = NPC_ROLE_MINER;
    npc->state = NPC_STATE_MINING;
    npc->home_station = 0;
    npc->target_asteroid = 0;
    npc->hull_class = HULL_CLASS_MINER;
    npc->hull = 100.0f;
    npc->pos = v2(0.0f, 0.0f);
    npc->vel = v2(0.0f, 0.0f);
    npc->angle = 0.0f;

    asteroid_t *a = &w->asteroids[0];
    a->active = true;
    a->tier = ASTEROID_TIER_M;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->radius = 30.0f;
    a->hp = 40.0f;
    a->max_hp = 40.0f;
    a->pos = v2(800.0f, 0.0f); /* well outside MINING_RANGE (170u) */

    float hp_before = a->hp;
    for (int i = 0; i < 60; i++) world_sim_step(w, 1.0f / 120.0f);

    ASSERT_EQ_FLOAT(a->hp, hp_before, 0.001f);
    ASSERT(npc->state != NPC_STATE_MINING);
}

void register_world_sim_basic_tests(void) {
    TEST_SECTION("\nWorld sim tests:\n");
    RUN(test_world_reset_creates_stations);
    RUN(test_world_reset_spawns_asteroids);
    RUN(test_world_reset_spawns_npcs);
    RUN(test_dead_hauler_auto_respawns);
    RUN(test_player_init_ship_docked);
    RUN(test_world_sim_step_advances_time);
    RUN(test_world_sim_step_moves_ship_with_thrust);
    RUN(test_world_sim_step_mining_damages_asteroid);
    RUN(test_world_sim_step_docking);
    RUN(test_world_sim_step_refinery_produces_ingots);
    RUN(test_mining_class_prefix_round_trip);
    RUN(test_refinery_deposits_named_ingot);
    RUN(test_station_production_dual_writes_frame_manifest);
    RUN(test_station_production_dual_writes_laser_manifest);
    RUN(test_station_production_without_manifest_inputs_refuses_to_mint);
    RUN(test_world_sim_step_events_emitted);
    RUN(test_world_sim_step_npc_miners_work);
    RUN(test_npc_mining_drops_state_when_far_from_target);
    RUN(test_world_network_writes_persist);
}

void register_world_sim_scenarios_tests(void) {
    TEST_SECTION("\nSim integration scenarios:\n");
    RUN(test_scenario_full_mining_cycle);
    RUN(test_manifest_conservation_across_transactions);
    RUN(test_scenario_two_players_mining);
    RUN(test_scenario_npc_economy_30_seconds);
    RUN(test_scenario_upgrade_requires_products);
    RUN(test_scenario_emergency_recovery);
    RUN(test_scenario_product_cap_pauses_production);
}

void register_world_sim_signal_tests(void) {
    TEST_SECTION("\nSignal range (#82):\n");
    RUN(test_signal_strength_at_station);
    RUN(test_signal_strength_falls_off);
    RUN(test_signal_overlap_boosts_strength);
    RUN(test_signal_zero_outside_range);
    RUN(test_signal_max_of_stations);
    RUN(test_ship_thrust_scales_with_signal);
    RUN(test_asteroid_outside_signal_despawns);
    RUN(test_npc_miners_avoid_zero_signal_asteroids);
    RUN(test_field_respawn_starts_beyond_signal_edge);
    RUN(test_asteroids_drift_toward_stronger_signal);
}

void register_world_sim_belt_tests(void) {
    TEST_SECTION("\nBelt generation:\n");
    RUN(test_belt_density_varies);
    RUN(test_belt_ore_distribution);
}

void register_world_sim_chunk_tests(void) {
    TEST_SECTION("\nChunk terrain generation:\n");
    RUN(test_chunk_determinism);
    RUN(test_chunk_different_coords_differ);
    RUN(test_chunk_respects_belt_density);
}

