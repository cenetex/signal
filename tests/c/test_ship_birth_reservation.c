#include "test_harness.h"
#include "ship_birth_reservation.h"

static void birth_test_fill_pub(uint8_t pub[32], uint8_t seed) {
    for (size_t i = 0; i < 32; i++)
        pub[i] = (uint8_t)(seed + (uint8_t)i);
}

static asteroid_t *birth_test_fragment(world_t *w, int slot,
                                       commodity_t commodity, vec2 pos,
                                       uint8_t pub_seed) {
    asteroid_t *fragment = &w->asteroids[slot];
    memset(fragment, 0, sizeof(*fragment));
    fragment->active = true;
    fragment->fracture_child = true;
    fragment->tier = ASTEROID_TIER_S;
    fragment->commodity = commodity;
    fragment->grade = (uint8_t)MINING_GRADE_COMMON;
    fragment->ore = 4.0f;
    fragment->max_ore = 4.0f;
    fragment->radius = 6.0f;
    fragment->pos = pos;
    birth_test_fill_pub(fragment->fragment_pub, pub_seed);
    return fragment;
}

static ship_birth_assembly_t *birth_test_reserve(world_t *w, int slot,
                                                  uint8_t pub_seed) {
    ship_birth_assembly_t *birth = &w->ship_birth_assemblies[0][0];
    memset(birth, 0, sizeof(*birth));
    birth->active = true;
    birth->fragment_slots[0] = (int16_t)slot;
    birth->fragment_slots[1] = -1;
    birth->fragment_slots[2] = -1;
    birth_test_fill_pub(birth->fragment_pubs[0], pub_seed);
    return birth;
}

TEST(test_ship_birth_reservation_uses_exact_fragment_identity) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.ship_birth_assemblies, 0, sizeof(w.ship_birth_assemblies));

    asteroid_t *fragment = birth_test_fragment(
        &w, 7, COMMODITY_FERRITE_ORE, w.stations[0].pos, 0x31);
    ship_birth_assembly_t *birth = birth_test_reserve(&w, 7, 0x31);

    ASSERT(world_ship_birth_fragment_reserved(&w, 7));

    /* A new stable object in the old slot must not inherit the reservation. */
    birth_test_fill_pub(fragment->fragment_pub, 0x91);
    ASSERT(!world_ship_birth_fragment_reserved(&w, 7));

    /* Identity, not a transient slot, remains authoritative. */
    birth->fragment_slots[0] = 11;
    birth_test_fill_pub(fragment->fragment_pub, 0x31);
    ASSERT(world_ship_birth_fragment_reserved(&w, 7));

    /* Missing identity on a referenced live slot fails closed. */
    memset(fragment->fragment_pub, 0, sizeof(fragment->fragment_pub));
    birth->fragment_slots[0] = 7;
    ASSERT(world_ship_birth_fragment_reserved(&w, 7));
}

TEST(test_ship_birth_reservation_rejects_new_player_and_npc_tows) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.ship_birth_assemblies, 0, sizeof(w.ship_birth_assemblies));

    server_player_t *player = &w.players[0];
    player->connected = true;
    player->session_ready = true;
    player->id = 0;
    player_init_ship(player, &w);
    ASSERT(world_npc_ship_slot_activate(&w, 0));
    w.npc_ships[0].active = true;

    asteroid_t *fragment = birth_test_fragment(
        &w, 9, COMMODITY_CUPRITE_ORE, w.stations[0].pos, 0x42);
    ship_birth_assembly_t *birth = birth_test_reserve(&w, 9, 0x42);

    ASSERT(!world_asteroid_set_player_tractor(&w, 9, 0));
    ASSERT(!world_asteroid_set_npc_tractor(&w, 9, 0));
    ASSERT(!asteroid_has_tractor(fragment));

    birth->active = false;
    ASSERT(world_asteroid_set_player_tractor(&w, 9, 0));
    ASSERT_EQ_INT(asteroid_tractor_player(fragment), 0);
}

static bool birth_test_ferrite_smelt_midpoint(const station_t *station,
                                               vec2 *out) {
    if (!station || !out) return false;
    for (int furnace_idx = 0;
         furnace_idx < station->module_count;
         furnace_idx++) {
        const station_module_t *furnace =
            &station->modules[furnace_idx];
        if (furnace->scaffold ||
            furnace->type != MODULE_FURNACE ||
            module_instance_input_ore(furnace) !=
                COMMODITY_FERRITE_ORE) {
            continue;
        }
        vec2 furnace_pos = module_world_pos_ring(
            station, furnace->ring, furnace->slot);
        const int adjacent[2] = {
            (int)furnace->ring + 1,
            (int)furnace->ring - 1,
        };
        for (int ring_idx = 0; ring_idx < 2; ring_idx++) {
            const int ring = adjacent[ring_idx];
            if (ring < 1 || ring > STATION_NUM_RINGS) continue;
            for (int hopper_idx = 0;
                 hopper_idx < station->module_count;
                 hopper_idx++) {
                const station_module_t *hopper =
                    &station->modules[hopper_idx];
                if (hopper->scaffold ||
                    hopper->type != MODULE_HOPPER ||
                    hopper->ring != ring ||
                    (commodity_t)hopper->commodity !=
                        COMMODITY_FERRITE_ORE) {
                    continue;
                }
                vec2 hopper_pos = module_world_pos_ring(
                    station, hopper->ring, hopper->slot);
                *out = v2_scale(v2_add(furnace_pos, hopper_pos), 0.5f);
                return true;
            }
        }
    }
    return false;
}

TEST(test_ship_birth_reservation_blocks_furnace_smelting) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.ship_birth_assemblies, 0, sizeof(w.ship_birth_assemblies));

    vec2 midpoint = w.stations[0].pos;
    ASSERT(birth_test_ferrite_smelt_midpoint(&w.stations[0], &midpoint));
    asteroid_t *fragment = birth_test_fragment(
        &w, 13, COMMODITY_FERRITE_ORE, midpoint, 0x53);
    ship_birth_assembly_t *birth = birth_test_reserve(&w, 13, 0x53);

    step_furnace_smelting(&w, SIM_DT);
    ASSERT_EQ_FLOAT(fragment->smelt_progress, 0.0f, 0.0001f);
    ASSERT(fragment->active);

    birth->active = false;
    step_furnace_smelting(&w, SIM_DT);
    ASSERT(fragment->smelt_progress > 0.0f);
}

TEST(test_ship_birth_reservation_blocks_fracture_child_cleanup) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.ship_birth_assemblies, 0, sizeof(w.ship_birth_assemblies));
    for (int p = 0; p < MAX_PLAYERS; p++)
        w.players[p].connected = false;

    station_t *station = &w.stations[0];
    station->signal_connected = true;
    station->signal_range = 10000.0f;
    asteroid_t *fragment = birth_test_fragment(
        &w, 17, COMMODITY_CRYSTAL_ORE,
        v2_add(station->pos, v2(500.0f, 0.0f)), 0x64);
    fragment->age = FRACTURE_CHILD_CLEANUP_AGE + 1.0f;
    ship_birth_assembly_t *birth = birth_test_reserve(&w, 17, 0x64);

    sim_step_asteroid_dynamics(&w, 0.0f);
    ASSERT(fragment->active);

    birth->active = false;
    sim_step_asteroid_dynamics(&w, 0.0f);
    ASSERT(!fragment->active);
}

TEST(test_ship_birth_reservation_blocks_signal_space_despawn) {
    WORLD_DECL;
    world_reset(&w);
    memset(w.asteroids, 0, sizeof(w.asteroids));
    memset(w.ship_birth_assemblies, 0, sizeof(w.ship_birth_assemblies));
    for (int station = 0; station < MAX_STATIONS; station++)
        w.stations[station].signal_connected = false;

    asteroid_t *fragment = birth_test_fragment(
        &w, 19, COMMODITY_FERRITE_ORE,
        v2(WORLD_RADIUS, WORLD_RADIUS), 0x75);
    ship_birth_assembly_t *birth = birth_test_reserve(&w, 19, 0x75);

    sim_step_asteroid_dynamics(&w, 0.0f);
    ASSERT(fragment->active);

    birth->active = false;
    sim_step_asteroid_dynamics(&w, 0.0f);
    ASSERT(!fragment->active);
}

void register_ship_birth_reservation_tests(void) {
    RUN(test_ship_birth_reservation_uses_exact_fragment_identity);
    RUN(test_ship_birth_reservation_rejects_new_player_and_npc_tows);
    RUN(test_ship_birth_reservation_blocks_furnace_smelting);
    RUN(test_ship_birth_reservation_blocks_fracture_child_cleanup);
    RUN(test_ship_birth_reservation_blocks_signal_space_despawn);
}
