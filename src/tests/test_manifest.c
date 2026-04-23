#include "tests/test_harness.h"

TEST(test_manifest_push_find_remove_preserves_order) {
    manifest_t manifest = {0};
    cargo_unit_t first = {0};
    cargo_unit_t second = {0};
    cargo_unit_t removed = {0};

    ASSERT(manifest_init(&manifest, 2));
    first.kind = (uint8_t)CARGO_KIND_INGOT;
    first.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    first.pub[31] = 0x11;
    second.kind = (uint8_t)CARGO_KIND_FRAME;
    second.commodity = (uint8_t)COMMODITY_FRAME;
    second.pub[31] = 0x22;

    ASSERT(manifest_push(&manifest, &first));
    ASSERT(manifest_push(&manifest, &second));
    ASSERT_EQ_INT(manifest.count, 2);
    ASSERT_EQ_INT(manifest_find(&manifest, first.pub), 0);
    ASSERT_EQ_INT(manifest_find(&manifest, second.pub), 1);

    ASSERT(manifest_remove(&manifest, 0, &removed));
    ASSERT_EQ_INT(manifest.count, 1);
    ASSERT_EQ_INT(removed.pub[31], 0x11);
    ASSERT_EQ_INT(manifest.units[0].pub[31], 0x22);
    ASSERT_EQ_INT(manifest_find(&manifest, first.pub), -1);

    manifest_free(&manifest);
}

TEST(test_manifest_clone_detaches_storage) {
    manifest_t src = {0};
    manifest_t dst = {0};
    cargo_unit_t unit = {0};

    ASSERT(manifest_init(&src, 1));
    unit.kind = (uint8_t)CARGO_KIND_LASER;
    unit.commodity = (uint8_t)COMMODITY_LASER_MODULE;
    unit.pub[0] = 0xAB;
    ASSERT(manifest_push(&src, &unit));
    ASSERT(manifest_clone(&dst, &src));

    ASSERT(dst.units != src.units);
    dst.units[0].pub[0] = 0xCD;
    ASSERT_EQ_INT(src.units[0].pub[0], 0xAB);
    ASSERT_EQ_INT(dst.units[0].pub[0], 0xCD);

    manifest_free(&dst);
    manifest_free(&src);
}

TEST(test_ship_copy_clones_manifest_storage) {
    ship_t src = {0};
    ship_t dst = {0};
    cargo_unit_t unit = {0};

    ASSERT(manifest_init(&src.manifest, 1));
    unit.kind = (uint8_t)CARGO_KIND_INGOT;
    unit.commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    unit.pub[0] = 0x11;
    ASSERT(manifest_push(&src.manifest, &unit));
    ASSERT(ship_copy(&dst, &src));

    ASSERT(dst.manifest.units != src.manifest.units);
    dst.manifest.units[0].pub[0] = 0x22;
    ASSERT_EQ_INT(src.manifest.units[0].pub[0], 0x11);

    ship_cleanup(&dst);
    ship_cleanup(&src);
}

TEST(test_station_copy_clones_manifest_storage) {
    station_t src = {0};
    station_t dst = {0};
    cargo_unit_t unit = {0};

    ASSERT(manifest_init(&src.manifest, 1));
    unit.kind = (uint8_t)CARGO_KIND_FRAME;
    unit.commodity = (uint8_t)COMMODITY_FRAME;
    unit.pub[0] = 0x33;
    ASSERT(manifest_push(&src.manifest, &unit));
    ASSERT(station_copy(&dst, &src));

    ASSERT(dst.manifest.units != src.manifest.units);
    dst.manifest.units[0].pub[0] = 0x44;
    ASSERT_EQ_INT(src.manifest.units[0].pub[0], 0x33);

    station_cleanup(&dst);
    station_cleanup(&src);
}

TEST(test_hash_legacy_migrate_unit_deterministic) {
    /* Same inputs -> byte-identical cargo_unit_t. Independent origins
     * or indices must produce different pubs. Validates the Slice D
     * migration helper's stability across save/load cycles. */
    uint8_t origin_a[8] = { 'S','T','A','T','0','0','0','0' };
    uint8_t origin_b[8] = { 'S','T','A','T','0','0','0','1' };
    cargo_unit_t u1 = {0}, u2 = {0}, u3 = {0}, u4 = {0};

    ASSERT(hash_legacy_migrate_unit(origin_a, COMMODITY_FERRITE_INGOT, 0, &u1));
    ASSERT(hash_legacy_migrate_unit(origin_a, COMMODITY_FERRITE_INGOT, 0, &u2));
    ASSERT(memcmp(&u1, &u2, sizeof(u1)) == 0);

    ASSERT(hash_legacy_migrate_unit(origin_b, COMMODITY_FERRITE_INGOT, 0, &u3));
    ASSERT(memcmp(u1.pub, u3.pub, 32) != 0); /* different origin -> different pub */

    ASSERT(hash_legacy_migrate_unit(origin_a, COMMODITY_FERRITE_INGOT, 1, &u4));
    ASSERT(memcmp(u1.pub, u4.pub, 32) != 0); /* different index -> different pub */

    /* Shape checks: kind matches commodity, grade is common, recipe is
     * LEGACY_MIGRATE, parent_merkle is all-zero. */
    ASSERT_EQ_INT(u1.kind, CARGO_KIND_INGOT);
    ASSERT_EQ_INT(u1.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(u1.grade, MINING_GRADE_COMMON);
    ASSERT_EQ_INT(u1.recipe_id, RECIPE_LEGACY_MIGRATE);
    uint8_t zero[32] = {0};
    ASSERT(memcmp(u1.parent_merkle, zero, 32) == 0);
}

TEST(test_hash_legacy_migrate_unit_rejects_raw_ore) {
    uint8_t origin[8] = { 0 };
    cargo_unit_t unit = {0};
    /* Raw ore is not a cargo_unit — the helper must refuse. */
    ASSERT(!hash_legacy_migrate_unit(origin, COMMODITY_FERRITE_ORE, 0, &unit));
    ASSERT(!hash_legacy_migrate_unit(origin, COMMODITY_CUPRITE_ORE, 0, &unit));
    ASSERT(!hash_legacy_migrate_unit(origin, COMMODITY_CRYSTAL_ORE, 0, &unit));
}

TEST(test_manifest_migrate_legacy_inventory_synthesizes_entries) {
    /* Slice D: populate a manifest from a float inventory, skipping
     * raw ore and emitting one unit per integer count of finished
     * goods. Fractional remainders stay in float (caller's
     * responsibility). */
    manifest_t m = {0};
    float inventory[COMMODITY_COUNT] = {0};
    uint8_t origin[8] = { 'O','R','I','G','0','0','0','1' };

    ASSERT(manifest_init(&m, 16));
    inventory[COMMODITY_FERRITE_ORE]   = 12.0f; /* raw ore — must be skipped */
    inventory[COMMODITY_FERRITE_INGOT] = 3.7f;  /* expect 3 units synthesized */
    inventory[COMMODITY_CUPRITE_INGOT] = 0.4f;  /* expect 0 units (< 1) */
    inventory[COMMODITY_FRAME]         = 2.0f;  /* expect 2 units */
    inventory[COMMODITY_LASER_MODULE]  = 1.0f;  /* expect 1 unit */

    ASSERT(manifest_migrate_legacy_inventory(&m, inventory, COMMODITY_COUNT, origin));
    ASSERT_EQ_INT(m.count, 3 + 0 + 2 + 1);

    /* All synthesized units must be LEGACY_MIGRATE + grade common. */
    int fe_ingot = 0, frame = 0, laser = 0, cu_ingot = 0, ore = 0;
    for (uint16_t i = 0; i < m.count; i++) {
        ASSERT_EQ_INT(m.units[i].recipe_id, RECIPE_LEGACY_MIGRATE);
        ASSERT_EQ_INT(m.units[i].grade, MINING_GRADE_COMMON);
        switch (m.units[i].commodity) {
        case COMMODITY_FERRITE_INGOT: fe_ingot++; break;
        case COMMODITY_CUPRITE_INGOT: cu_ingot++; break;
        case COMMODITY_FRAME: frame++; break;
        case COMMODITY_LASER_MODULE: laser++; break;
        case COMMODITY_FERRITE_ORE:
        case COMMODITY_CUPRITE_ORE:
        case COMMODITY_CRYSTAL_ORE: ore++; break;
        default: break;
        }
    }
    ASSERT_EQ_INT(fe_ingot, 3);
    ASSERT_EQ_INT(cu_ingot, 0);
    ASSERT_EQ_INT(frame, 2);
    ASSERT_EQ_INT(laser, 1);
    ASSERT_EQ_INT(ore, 0); /* raw ore was correctly skipped */

    manifest_free(&m);
}

TEST(test_hash_merkle_root_sorts_and_duplicates_odd_leaf) {
    const uint8_t pubs[3][32] = {
        {
            0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
            0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
            0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
            0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
        },
        {
            0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
            0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
            0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
        },
        {
            0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
            0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
            0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
            0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
        },
    };
    uint8_t root[32];

    ASSERT(hash_merkle_root(pubs, 3, root));
    ASSERT_HEX32_EQ(root, "4337e6c1b1f6b3728eef23890f0f41379ae574d390ebc3211d14d7a451d28ecd");
}

TEST(test_hash_ingot_matches_known_vector) {
    uint8_t fragment_pub[32];
    cargo_unit_t ingot = {0};

    for (int i = 0; i < 32; i++) fragment_pub[i] = (uint8_t)i;
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_pub, 0, &ingot));

    ASSERT_EQ_INT(ingot.kind, CARGO_KIND_INGOT);
    ASSERT_EQ_INT(ingot.commodity, COMMODITY_FERRITE_INGOT);
    ASSERT_EQ_INT(ingot.grade, MINING_GRADE_RARE);
    ASSERT_EQ_INT(ingot.recipe_id, RECIPE_SMELT);
    ASSERT(memcmp(ingot.parent_merkle, fragment_pub, 32) == 0);
    ASSERT_HEX32_EQ(ingot.pub, "d869450f3625b4c095dabb2e60a7be66abc67c706a13d362496770890d21d725");
}

TEST(test_hash_product_matches_known_vector_and_min_grade) {
    cargo_unit_t inputs[2] = {0};
    cargo_unit_t frame = {0};

    inputs[0].kind = (uint8_t)CARGO_KIND_INGOT;
    inputs[0].commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    inputs[0].grade = (uint8_t)MINING_GRADE_RARE;
    for (int i = 0; i < 32; i++) inputs[0].pub[i] = (uint8_t)(0x20 + i);

    inputs[1].kind = (uint8_t)CARGO_KIND_INGOT;
    inputs[1].commodity = (uint8_t)COMMODITY_FERRITE_INGOT;
    inputs[1].grade = (uint8_t)MINING_GRADE_FINE;
    for (int i = 0; i < 32; i++) inputs[1].pub[i] = (uint8_t)(0x40 + i);

    ASSERT(hash_product(RECIPE_FRAME_BASIC, inputs, 2, 0, &frame));
    ASSERT_EQ_INT(frame.kind, CARGO_KIND_FRAME);
    ASSERT_EQ_INT(frame.commodity, COMMODITY_FRAME);
    ASSERT_EQ_INT(frame.grade, MINING_GRADE_FINE);
    ASSERT_EQ_INT(frame.recipe_id, RECIPE_FRAME_BASIC);
    ASSERT_HEX32_EQ(frame.parent_merkle, "ae02e99bbdd3713ac87427589a48fc45818ef9a7ecd27941142d8f6f61afb7c1");
    ASSERT_HEX32_EQ(frame.pub, "afd71562654d3d5a973927c68df0b3187fc3651a2296cd4b48b52e74925bf2d2");
}

TEST(test_fracture_claim_resolves_best_verified_grade) {
    WORLD_DECL;
    asteroid_t *a = &w.asteroids[0];
    fracture_claim_state_t *state = &w.fracture_claims[0];
    uint8_t player_pub[32];
    uint8_t expected_pub[32];
    uint8_t zero_pub[32] = {0};
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;

    world_reset(&w);
    clear_asteroid(a);
    memset(state, 0, sizeof(*state));

    w.players[0].connected = true;
    w.players[0].session_ready = true;
    memset(w.players[0].session_token, 0x5A, sizeof(w.players[0].session_token));
    w.players[0].ship.pos = w.stations[0].pos;

    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 8.0f;
    a->pos = w.stations[0].pos;
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(0x30 + i);

    state->active = true;
    state->fracture_id = 77;
    state->deadline_ms = 500;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;

    sha256_bytes(w.players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);

    ASSERT(!submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                  (uint8_t)(best_grade + 1)));
    ASSERT(submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                 (uint8_t)best_grade));
    ASSERT_EQ_INT(state->best_nonce, best_nonce);
    ASSERT_EQ_INT(state->best_grade, best_grade);
    ASSERT(memcmp(state->best_player_pub, zero_pub, 32) != 0);
    ASSERT_EQ_INT(state->seen_claimant_count, 1);
    ASSERT(memcmp(state->seen_claimant_tokens[0],
                  w.players[0].session_token, 8) == 0);

    w.time = 1.0f;
    step_fracture_claims(&w);
    mining_fragment_pub_compute(a->fracture_seed, player_pub, best_nonce, expected_pub);

    ASSERT(!state->active);
    ASSERT(state->resolved);
    ASSERT_EQ_INT(a->grade, best_grade);
    ASSERT(memcmp(a->fragment_pub, expected_pub, 32) == 0);
}

TEST(test_fracture_claim_fallback_resolves_without_claims) {
    WORLD_DECL;
    asteroid_t *a = &w.asteroids[0];
    fracture_claim_state_t *state = &w.fracture_claims[0];
    uint8_t zero_pub[32] = {0};
    uint8_t expected_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;

    world_reset(&w);
    clear_asteroid(a);
    memset(state, 0, sizeof(*state));

    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 8.0f;
    a->pos = w.stations[0].pos;
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(0x50 + i);

    state->active = true;
    state->fracture_id = 91;
    state->deadline_ms = 0;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;

    mining_find_best_claim(a->fracture_seed, zero_pub, MINING_BURST_PER_FRAGMENT,
                           &best_nonce, &best_grade);
    w.time = 1.0f;
    step_fracture_claims(&w);
    mining_fragment_pub_compute(a->fracture_seed, zero_pub, best_nonce, expected_pub);

    ASSERT(!state->active);
    ASSERT(state->resolved);
    ASSERT(memcmp(state->best_player_pub, zero_pub, 32) == 0);
    ASSERT_EQ_INT(a->grade, best_grade);
    ASSERT(memcmp(a->fragment_pub, expected_pub, 32) == 0);
}

TEST(test_smelt_manifest_uses_resolved_fragment_pub) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    cargo_unit_t expected = {0};
    uint8_t fragment_pub[32];
    int furnace_idx = -1;
    int silo_idx = -1;
    int slot = -1;
    int initial_manifest;
    vec2 midpoint;

    ASSERT(w != NULL);
    world_reset(w);
    for (int m = 0; m < w->stations[0].module_count; m++) {
        if (w->stations[0].modules[m].type == MODULE_FURNACE) furnace_idx = m;
        if (w->stations[0].modules[m].type == MODULE_ORE_SILO) silo_idx = m;
    }
    ASSERT(furnace_idx >= 0);
    ASSERT(silo_idx >= 0);
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w->stations[0].arm_speed[arm] = 0.0f;
        w->stations[0].arm_rotation[arm] = 0.0f;
    }
    midpoint = v2_scale(v2_add(
        module_world_pos_ring(&w->stations[0],
                              w->stations[0].modules[furnace_idx].ring,
                              w->stations[0].modules[furnace_idx].slot),
        module_world_pos_ring(&w->stations[0],
                              w->stations[0].modules[silo_idx].ring,
                              w->stations[0].modules[silo_idx].slot)), 0.5f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);
    for (int i = 0; i < 32; i++) fragment_pub[i] = (uint8_t)(0xA0 + i);
    ASSERT(hash_ingot(COMMODITY_FERRITE_INGOT, MINING_GRADE_RARE,
                      fragment_pub, 0, &expected));

    asteroid_t *a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = MINING_GRADE_RARE;
    a->pos = midpoint;
    memcpy(a->fragment_pub, fragment_pub, sizeof(fragment_pub));

    initial_manifest = w->stations[0].manifest.count;
    for (int i = 0; i < 600 && w->asteroids[slot].active; i++)
        world_sim_step(w, 1.0f / 120.0f);

    ASSERT(!w->asteroids[slot].active);
    ASSERT_EQ_INT(w->stations[0].manifest.count - initial_manifest, 1);
    ASSERT(memcmp(w->stations[0].manifest.units[initial_manifest].parent_merkle,
                  expected.parent_merkle, 32) == 0);
    ASSERT(memcmp(w->stations[0].manifest.units[initial_manifest].pub,
                  expected.pub, 32) == 0);
    ASSERT_EQ_INT(w->stations[0].manifest.units[initial_manifest].grade,
                  MINING_GRADE_RARE);
}

/* Forward-declared so the rebroadcast / pending-queue tests below can
 * use it before its definition a few tests down. Keeps the test file
 * in narrative order (simple cases first, edge-cases later). */
static void setup_fracture_claim_scenario(world_t *w, int asteroid_idx, int player_slot,
                                          uint8_t seed_fill, uint8_t token_fill,
                                          uint32_t fracture_id);

TEST(test_fracture_claim_rebroadcasts_challenge_for_late_joiners) {
    /* Late joiners to the 500ms claim window must still see the
     * challenge. The initial broadcast is fired by
     * fracture_begin_claim_window; step_fracture_claims re-arms
     * challenge_dirty at FRACTURE_CHALLENGE_REBROADCAST_MS cadence
     * while the window is open so subsequent ticks push the challenge
     * to anyone who entered range in the meantime. */
    WORLD_DECL;
    asteroid_t *a;
    fracture_claim_state_t *state;

    world_reset(&w);
    a = &w.asteroids[0];
    state = &w.fracture_claims[0];
    clear_asteroid(a);
    memset(state, 0, sizeof(*state));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 8.0f;
    a->pos = w.stations[0].pos;
    state->active = true;
    state->fracture_id = 501;
    /* Window still open — deadline 500ms. */
    state->deadline_ms = 500;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;
    /* Simulate "initial broadcast consumed by transport at t=0". */
    state->challenge_dirty = false;
    state->challenge_last_ms = 0;

    /* Tick shortly after — cadence not elapsed yet, no re-arm. */
    w.time = 0.05f; /* 50ms */
    step_fracture_claims(&w);
    ASSERT(!state->challenge_dirty);

    /* After REBROADCAST_MS elapses, dirty must be re-armed so transport
     * retransmits to anyone who entered range since the initial send. */
    w.time = 0.2f; /* 200ms — past the 100ms rebroadcast period */
    step_fracture_claims(&w);
    ASSERT(state->challenge_dirty);
    /* Timestamp must advance so the next tick doesn't re-arm again
     * until another 100ms passes (cadence is monotonic, not spammy). */
    ASSERT_EQ_INT(state->challenge_last_ms, 200);
}

TEST(test_fracture_resolve_pushes_to_pending_queue) {
    /* fracture_commit_resolution must push to pending_resolves so the
     * NET_MSG_FRACTURE_RESOLVED packet can survive an asteroid clear
     * that happens in the same tick (the gnarly resolve-then-smelt
     * race). Verify the queue entry carries the final fragment_pub
     * and winner identity regardless of asteroid lifecycle. */
    WORLD_DECL;
    asteroid_t *a;
    fracture_claim_state_t *state;
    uint8_t player_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;

    setup_fracture_claim_scenario(&w, 0, 0, 0x70, 0x7E, 777);
    state = &w.fracture_claims[0];
    a = &w.asteroids[0];
    sha256_bytes(w.players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);

    ASSERT(submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                 (uint8_t)best_grade));

    /* Advance past deadline and resolve. */
    w.time = 1.0f;
    step_fracture_claims(&w);
    ASSERT(state->resolved);

    /* The queue must now carry this resolve. */
    bool found = false;
    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        const pending_resolve_t *pr = &w.pending_resolves[p];
        if (!pr->active) continue;
        if (pr->fracture_id != state->fracture_id) continue;
        ASSERT(memcmp(pr->fragment_pub, a->fragment_pub, 32) == 0);
        ASSERT(memcmp(pr->winner_pub, state->best_player_pub, 32) == 0);
        ASSERT_EQ_INT(pr->grade, state->best_grade);
        found = true;
        break;
    }
    ASSERT(found);

    /* Now simulate the asteroid getting smelted-and-cleared before
     * transport could flush the queue. The pending_resolve_t entry
     * still carries everything needed to send NET_MSG_FRACTURE_RESOLVED. */
    clear_asteroid(a);
    fracture_claim_state_reset(state);

    found = false;
    for (int p = 0; p < MAX_PENDING_RESOLVES; p++) {
        const pending_resolve_t *pr = &w.pending_resolves[p];
        if (!pr->active) continue;
        if (pr->fracture_id != 777) continue;
        /* Still have the fragment_pub and winner_pub even though the
         * asteroid and its claim state are gone. This is the point. */
        ASSERT_EQ_INT(pr->grade, (int)best_grade);
        found = true;
        break;
    }
    ASSERT(found);
}

/* Helper for the edge-case claim tests: set up an S-tier fragment at
 * station 0 with an open claim window, and configure player `slot`
 * as connected + session-ready near the station. */
static void setup_fracture_claim_scenario(world_t *w, int asteroid_idx, int player_slot,
                                          uint8_t seed_fill, uint8_t token_fill,
                                          uint32_t fracture_id) {
    asteroid_t *a = &w->asteroids[asteroid_idx];
    fracture_claim_state_t *state = &w->fracture_claims[asteroid_idx];

    world_reset(w);
    clear_asteroid(a);
    memset(state, 0, sizeof(*state));

    w->players[player_slot].connected = true;
    w->players[player_slot].session_ready = true;
    memset(w->players[player_slot].session_token, token_fill,
           sizeof(w->players[player_slot].session_token));
    w->players[player_slot].ship.pos = w->stations[0].pos;

    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 8.0f;
    a->pos = w->stations[0].pos;
    for (int i = 0; i < 32; i++) a->fracture_seed[i] = (uint8_t)(seed_fill + i);

    state->active = true;
    state->fracture_id = fracture_id;
    state->deadline_ms = 500;
    state->burst_cap = FRACTURE_CHALLENGE_BURST_CAP;
}

TEST(test_fracture_claim_rejects_past_deadline) {
    /* Once w->time has crossed deadline_ms, submit_fracture_claim must
     * refuse the claim — even before step_fracture_claims runs. This
     * guards against timing-race ordering between wire delivery and
     * the server's per-tick claim sweep. */
    WORLD_DECL;
    uint8_t player_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;
    fracture_claim_state_t *state;
    asteroid_t *a;

    setup_fracture_claim_scenario(&w, 0, 0, 0x30, 0x5A, 77);
    state = &w.fracture_claims[0];
    a = &w.asteroids[0];
    sha256_bytes(w.players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);

    w.time = (float)(state->deadline_ms + 1u) / 1000.0f;
    ASSERT(!submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                  (uint8_t)best_grade));
    /* State should still be pristine (no best recorded, no seen claimants). */
    ASSERT_EQ_INT(state->seen_claimant_count, 0);
    ASSERT_EQ_INT(state->best_grade, MINING_GRADE_COMMON);
}

TEST(test_fracture_claim_rejects_out_of_signal_range) {
    /* Signal-range gating is the "dark space can't claim" invariant.
     * Park the player beyond every station's signal radius and verify
     * the claim bounces even with a correctly-verified grade. */
    WORLD_DECL;
    uint8_t player_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;
    fracture_claim_state_t *state;
    asteroid_t *a;

    setup_fracture_claim_scenario(&w, 0, 0, 0x31, 0x5B, 78);
    state = &w.fracture_claims[0];
    a = &w.asteroids[0];
    /* Push the player far enough from every station that signal_radius
     * at the asteroid excludes them. 1e6 world-units is well past any
     * station's signal_range in test setup. */
    w.players[0].ship.pos = v2(w.stations[0].pos.x + 1.0e6f,
                                w.stations[0].pos.y + 1.0e6f);
    sha256_bytes(w.players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);

    ASSERT(!submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                  (uint8_t)best_grade));
    ASSERT_EQ_INT(state->seen_claimant_count, 0);
}

TEST(test_fracture_claim_rejects_duplicate_token) {
    /* One claim per player per fracture. Submitting again from the
     * same session_token must bounce even with a better nonce. */
    WORLD_DECL;
    uint8_t player_pub[32];
    uint32_t best_nonce = 0;
    mining_grade_t best_grade = MINING_GRADE_COMMON;
    fracture_claim_state_t *state;
    asteroid_t *a;

    setup_fracture_claim_scenario(&w, 0, 0, 0x32, 0x5C, 79);
    state = &w.fracture_claims[0];
    a = &w.asteroids[0];
    sha256_bytes(w.players[0].session_token, 8, player_pub);
    mining_find_best_claim(a->fracture_seed, player_pub, state->burst_cap,
                           &best_nonce, &best_grade);

    ASSERT(submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                 (uint8_t)best_grade));
    ASSERT_EQ_INT(state->seen_claimant_count, 1);
    /* Second submission from same token — even an identical (nonce,grade)
     * must return false rather than touching state. */
    ASSERT(!submit_fracture_claim(&w, 0, state->fracture_id, best_nonce,
                                  (uint8_t)best_grade));
    ASSERT_EQ_INT(state->seen_claimant_count, 1);
}

TEST(test_fracture_claim_tie_break_prefers_first_claimant) {
    /* Two players submit claims at the same grade. The first valid
     * submission wins — later equal-grade claims must not overwrite
     * best_player_pub, because that would churn the fragment_pub. */
    WORLD_DECL;
    uint8_t pub_a[32];
    uint8_t pub_b[32];
    uint32_t nonce_a = 0, nonce_b = 0;
    mining_grade_t grade_a = MINING_GRADE_COMMON, grade_b = MINING_GRADE_COMMON;
    fracture_claim_state_t *state;
    asteroid_t *a;

    setup_fracture_claim_scenario(&w, 0, 0, 0x33, 0x5D, 80);
    state = &w.fracture_claims[0];
    a = &w.asteroids[0];
    /* Add a second player, also in range. */
    w.players[1].connected = true;
    w.players[1].session_ready = true;
    memset(w.players[1].session_token, 0x6E, sizeof(w.players[1].session_token));
    w.players[1].ship.pos = w.stations[0].pos;

    sha256_bytes(w.players[0].session_token, 8, pub_a);
    sha256_bytes(w.players[1].session_token, 8, pub_b);
    mining_find_best_claim(a->fracture_seed, pub_a, state->burst_cap, &nonce_a, &grade_a);
    mining_find_best_claim(a->fracture_seed, pub_b, state->burst_cap, &nonce_b, &grade_b);

    /* Clamp both to the same grade (COMMON) to force a tie. */
    ASSERT(submit_fracture_claim(&w, 0, state->fracture_id, nonce_a,
                                 (uint8_t)MINING_GRADE_COMMON));
    ASSERT(memcmp(state->best_player_pub, pub_a, 32) == 0);

    ASSERT(submit_fracture_claim(&w, 1, state->fracture_id, nonce_b,
                                 (uint8_t)MINING_GRADE_COMMON));
    /* best_grade must not regress, and best_player_pub must still be A's
     * — tie goes to first-seen so the fragment_pub doesn't churn. */
    ASSERT(memcmp(state->best_player_pub, pub_a, 32) == 0);
    ASSERT_EQ_INT(state->seen_claimant_count, 2);
}

TEST(test_smelt_credit_ignores_claim_winner_identity) {
    /* Provenance (fragment_pub) is independent from credit. The claim
     * winner only stamps the fragment's identity — smelt pays whoever
     * towed and whoever fractured, each via their own session_token.
     *
     * Scenario: a single worker (player 1) both fractured and towed
     * the rock. A bystander (player 0) owns the pubkey that's stamped
     * into fragment_pub (as if they'd won the claim race). Bystander
     * must receive zero credit at smelt; the worker must receive the
     * full split. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    asteroid_t *a;
    int bystander = 0, worker = 1;
    int slot = -1;
    int furnace_idx = -1;
    float bystander_delta, worker_delta;
    float initial_bystander, initial_worker;

    ASSERT(w != NULL);
    world_reset(w);
    for (int m = 0; m < w->stations[0].module_count; m++) {
        if (w->stations[0].modules[m].type == MODULE_FURNACE) { furnace_idx = m; break; }
    }
    ASSERT(furnace_idx >= 0);
    for (int arm = 0; arm < MAX_ARMS; arm++) {
        w->stations[0].arm_speed[arm] = 0.0f;
        w->stations[0].arm_rotation[arm] = 0.0f;
    }
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { slot = i; break; }
    }
    ASSERT(slot >= 0);

    w->players[bystander].connected = true;
    w->players[bystander].session_ready = true;
    memset(w->players[bystander].session_token, 0xAA,
           sizeof(w->players[bystander].session_token));
    w->players[worker].connected = true;
    w->players[worker].session_ready = true;
    memset(w->players[worker].session_token, 0xBB,
           sizeof(w->players[worker].session_token));

    a = &w->asteroids[slot];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->tier = ASTEROID_TIER_S;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->radius = 6.0f;
    a->fracture_child = true;
    a->grade = MINING_GRADE_COMMON;
    a->pos = module_world_pos_ring(&w->stations[0],
                                   w->stations[0].modules[furnace_idx].ring,
                                   w->stations[0].modules[furnace_idx].slot);
    /* fragment_pub is sha256-of-bystander-pub — as if the bystander
     * had won the claim race. This must NOT route any credit to them. */
    sha256_bytes(w->players[bystander].session_token, 8, a->fragment_pub);
    /* Worker did both fracture and tow. */
    memcpy(a->last_towed_token, w->players[worker].session_token, 8);
    a->last_towed_by = (int8_t)worker;
    memcpy(a->last_fractured_token, w->players[worker].session_token, 8);
    a->last_fractured_by = (int8_t)worker;

    initial_bystander = ledger_balance(&w->stations[0],
                                       w->players[bystander].session_token);
    initial_worker    = ledger_balance(&w->stations[0],
                                       w->players[worker].session_token);

    for (int i = 0; i < 600 && w->asteroids[slot].active; i++)
        world_sim_step(w, 1.0f / 120.0f);
    ASSERT(!w->asteroids[slot].active);

    bystander_delta = ledger_balance(&w->stations[0],
                                     w->players[bystander].session_token)
                    - initial_bystander;
    worker_delta    = ledger_balance(&w->stations[0],
                                     w->players[worker].session_token)
                    - initial_worker;

    /* Bystander — no work, no credit, even though they "own" fragment_pub. */
    ASSERT_EQ_FLOAT(bystander_delta, 0.0f, 0.01f);
    /* Worker did both roles, so they receive the full tower+fracturer split. */
    ASSERT(worker_delta > 0.0f);
}
void register_manifest_tests(void) {
    TEST_SECTION("\nManifest tests:\n");
    RUN(test_manifest_push_find_remove_preserves_order);
    RUN(test_manifest_clone_detaches_storage);
    RUN(test_hash_legacy_migrate_unit_deterministic);
    RUN(test_hash_legacy_migrate_unit_rejects_raw_ore);
    RUN(test_manifest_migrate_legacy_inventory_synthesizes_entries);
    RUN(test_ship_copy_clones_manifest_storage);
    RUN(test_station_copy_clones_manifest_storage);
    RUN(test_hash_merkle_root_sorts_and_duplicates_odd_leaf);
    RUN(test_hash_ingot_matches_known_vector);
    RUN(test_hash_product_matches_known_vector_and_min_grade);
    RUN(test_fracture_claim_resolves_best_verified_grade);
    RUN(test_fracture_claim_fallback_resolves_without_claims);
    RUN(test_fracture_claim_rejects_past_deadline);
    RUN(test_fracture_claim_rejects_out_of_signal_range);
    RUN(test_fracture_claim_rejects_duplicate_token);
    RUN(test_fracture_claim_tie_break_prefers_first_claimant);
    RUN(test_fracture_claim_rebroadcasts_challenge_for_late_joiners);
    RUN(test_fracture_resolve_pushes_to_pending_queue);
    RUN(test_smelt_manifest_uses_resolved_fragment_pub);
    RUN(test_smelt_credit_ignores_claim_winner_identity);
}
