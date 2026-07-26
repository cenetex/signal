#include "test_harness.h"
#include "state_digest.h"

static void state_root(const world_t *world,
                       uint8_t out[SIGNAL_AUTH_STATE_DIGEST_SIZE])
{
    signal_authoritative_state_digest(world, out);
}

static bool roots_equal(
    const uint8_t a[SIGNAL_AUTH_STATE_DIGEST_SIZE],
    const uint8_t b[SIGNAL_AUTH_STATE_DIGEST_SIZE])
{
    return memcmp(a, b, SIGNAL_AUTH_STATE_DIGEST_SIZE) == 0;
}

TEST(test_state_digest_reports_versioned_schema_and_is_repeatable)
{
    WORLD_DECL;
    uint8_t first[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t second[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    ASSERT_STR_EQ(signal_authoritative_state_digest_schema(),
                  "signal.authoritative_state.v1");
    ASSERT_EQ_INT((int)signal_authoritative_state_digest_version(), 1);

    state_root(&w, first);
    state_root(&w, second);
    ASSERT(roots_equal(first, second));
}

TEST(test_state_digest_commits_each_authoritative_domain)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    ASSERT(world_player_ship_slot_activate(&w, 0));

    state_root(&w, before);
    w.rng++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    w.stations[0].base_price[COMMODITY_FERRITE_ORE] += 1.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    memcpy(before, after, sizeof(before));
    w.ships[WORLD_PLAYER_SHIP_BASE].component.mining_level++;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.asteroids[0].active = true;
    w.asteroids[0].max_hp = 10.0f;
    state_root(&w, before);
    w.asteroids[0].max_hp = 11.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.tow_links[0].active = true;
    w.tow_links[0].state = TOW_LINK_CAPTURE;
    state_root(&w, before);
    w.tow_links[0].state = TOW_LINK_HELD;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.cargo_pods[0].active = true;
    w.cargo_pods[0].radius = 20.0f;
    state_root(&w, before);
    w.cargo_pods[0].radius = 21.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.delivery_shipments[0].active = true;
    w.delivery_shipments[0].debt_principal = 100.0f;
    state_root(&w, before);
    w.delivery_shipments[0].debt_principal = 101.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.stations[0].hnn_market_memory.store[0] = 0.25f;
    state_root(&w, before);
    w.stations[0].hnn_market_memory.store[0] = 0.50f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.signal_channel.count = 1;
    w.signal_channel.head = 1;
    w.signal_channel.next_id = 2;
    w.signal_channel.msgs[0].id = 1;
    w.signal_channel.msgs[0].text_len = 1;
    w.signal_channel.msgs[0].text[0] = 'a';
    state_root(&w, before);
    w.signal_channel.msgs[0].text[0] = 'b';
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.players[0].movement_queue_count = 1;
    w.players[0].movement_queue[0].apply_tick = w.tick + 1;
    w.players[0].movement_queue[0].input_seq = 1;
    state_root(&w, before);
    w.players[0].movement_queue[0].intent.thrust = 1.0f;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));

    w.pubkey_registry[0].in_use = true;
    w.pubkey_registry[0].pubkey[0] = 1;
    state_root(&w, before);
    w.pubkey_registry[0].pubkey[0] = 2;
    state_root(&w, after);
    ASSERT(!roots_equal(before, after));
}

TEST(test_state_digest_excludes_transport_secrets_and_derived_views)
{
    WORLD_DECL;
    uint8_t before[SIGNAL_AUTH_STATE_DIGEST_SIZE];
    uint8_t after[SIGNAL_AUTH_STATE_DIGEST_SIZE];

    world_reset(&w);
    state_root(&w, before);

    w.connections[0].analytics_metrics_seq++;
    w.replications[0].world_time_sent = true;
    w.pending_resolves[0].active = true;
    w.pending_resolves[0].tx_count = 2;
    w.stations[0].station_secret[0] ^= 0x5au;
    w.stations[0].modules[0].flow_diag = STATION_FLOW_DIAG_NO_INPUT;
    w.stations[0].hnn_market_memory.last_margin = 0.75f;
    w.signal_field.cells[0].strength[0] = 0.5f;
    w.signal_field_decay_tick++;

    state_root(&w, after);
    ASSERT(roots_equal(before, after));

    w.cargo_pods[0].active = true;
    state_root(&w, before);
    w.cargo_pods[0].summary_flags = 1;
    w.cargo_pods[0].summary_grade = MINING_GRADE_RATI;
    w.cargo_pods[0].tractor.source_index = 7;
    w.cargo_pods[0].tractor.source_part = 3;
    w.cargo_pods[0].tractor.source_generation = 9;
    state_root(&w, after);
    ASSERT(roots_equal(before, after));

    w.asteroids[0].active = true;
    w.asteroid_origin[0].from_chunk = false;
    state_root(&w, before);
    w.asteroid_origin[0].chunk_x = 44;
    w.asteroid_origin[0].chunk_y = -12;
    state_root(&w, after);
    ASSERT(roots_equal(before, after));
}

void register_state_digest_tests(void)
{
    RUN(test_state_digest_reports_versioned_schema_and_is_repeatable);
    RUN(test_state_digest_commits_each_authoritative_domain);
    RUN(test_state_digest_excludes_transport_secrets_and_derived_views);
}
