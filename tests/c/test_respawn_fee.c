/* Respawn-fee debit + death-event payload tests.
 *
 * On every respawn (emergency_recover_ship), the player owes the
 * spawn fee at the station they respawn at — even if they already had
 * a ledger entry there. The fee scales with station ring count
 * (50 / 100 / 300) so the death overlay can surface "[E] launch
 * -300 Helios credits" with a number that's actually been debited.
 */

#include "test_harness.h"

static void undock_at(world_t *w, server_player_t *sp, vec2 pos) {
    (void)w;
    sp->docked = false;
    sp->current_station = -1;
    sp->ship->pos = pos;
    sp->ship->vel = v2(0.0f, 0.0f);
}

TEST(test_respawn_debits_station_ledger) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x42, 8);

    /* Position the ship next to station 0 so the respawn picks station 0. */
    int s0 = 0;
    int expected_fee = station_spawn_fee(&w->stations[s0]);
    ASSERT(expected_fee > 0);
    vec2 near_st0 = w->stations[s0].pos;
    near_st0.x += 50.0f;
    undock_at(w, sp, near_st0);

    float balance_before = ledger_balance(&w->stations[s0], sp->session_token);

    /* Trigger self-destruct → emergency_recover_ship. */
    sp->input.reset = true;
    world_sim_step(w, SIM_DT);

    float balance_after = ledger_balance(&w->stations[s0], sp->session_token);
    ASSERT_EQ_FLOAT(balance_before - balance_after, (float)expected_fee, 0.5f);
    ASSERT(sp->docked);
    ASSERT_EQ_INT(sp->current_station, s0);
}

TEST(test_respawn_fee_persists_negative_balance) {
    /* Force-debit lets the ledger go negative — confirm a player who
     * already owes still pays the fee and goes deeper into debt. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x77, 8);

    int s0 = 0;
    int fee = station_spawn_fee(&w->stations[s0]);

    /* Pre-existing debt of 100. */
    ledger_force_debit(&w->stations[s0], sp->session_token, 100.0f, sp->ship);
    ASSERT_EQ_FLOAT(ledger_balance(&w->stations[s0], sp->session_token), -100.0f, 0.5f);

    vec2 near = w->stations[s0].pos;
    near.x += 50.0f;
    undock_at(w, sp, near);
    sp->input.reset = true;
    world_sim_step(w, SIM_DT);

    /* Balance should be -100 - fee (or close) — no clamp at zero. */
    float bal = ledger_balance(&w->stations[s0], sp->session_token);
    ASSERT_EQ_FLOAT(bal, -100.0f - (float)fee, 0.5f);
}

TEST(test_respawn_event_carries_station_and_fee) {
    /* The SIM_EVENT_DEATH carries respawn_station + respawn_fee so the
     * client overlay can render the per-station label. */
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    memset(sp->session_token, 0x33, 8);

    int s0 = 0;
    int expected_fee = station_spawn_fee(&w->stations[s0]);

    vec2 near = w->stations[s0].pos;
    near.x += 50.0f;
    undock_at(w, sp, near);

    /* Drain any pre-existing events. */
    w->events.count = 0;

    sp->input.reset = true;
    world_sim_step(w, SIM_DT);

    bool found = false;
    for (int i = 0; i < w->events.count; i++) {
        const sim_event_t *ev = &w->events.events[i];
        if (ev->type != SIM_EVENT_DEATH) continue;
        ASSERT_EQ_INT(ev->death.respawn_station, s0);
        ASSERT_EQ_FLOAT(ev->death.respawn_fee, (float)expected_fee, 0.5f);
        found = true;
        break;
    }
    ASSERT(found);
}

TEST(test_self_destruct_is_consumed_before_respawn_launch) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;

    sp->input.launch = true;
    world_sim_step(w, SIM_DT);
    ASSERT(!sp->docked);
    sp->input.reset = true;
    world_sim_step(w, SIM_DT);
    ASSERT(sp->docked);
    ASSERT(!sp->input.reset);
    uint32_t replacement_id = sp->ship_asset_id;
    ASSERT(replacement_id != SHIP_ASSET_ID_NONE);

    /* Keep the server's retained input across the death screen and launch. */
    for (int i = 0; i < 12; i++) world_sim_step(w, SIM_DT);
    sp->input.launch = true;
    world_sim_step(w, SIM_DT);
    ASSERT(!sp->docked);
    for (int i = 0; i < 12; i++) world_sim_step(w, SIM_DT);
    ASSERT(!sp->docked);
    ASSERT_EQ_INT(sp->ship_asset_id, replacement_id);
    ASSERT(sp->ship->hull > 0.0f);
}

TEST(test_docked_self_destruct_is_consumed_before_launch) {
    WORLD_HEAP w = calloc(1, sizeof(world_t));
    ASSERT(w != NULL);
    world_reset(w);
    server_player_t *sp = &w->players[0];
    player_init_ship(sp, w);
    sp->connected = true;
    sp->session_ready = true;
    uint32_t asset_id = sp->ship_asset_id;

    /* A reset can arrive after another cause has already docked the ship. */
    sp->input.reset = true;
    world_sim_step(w, SIM_DT);
    ASSERT(sp->docked);
    ASSERT(!sp->input.reset);
    sp->input.launch = true;
    world_sim_step(w, SIM_DT);
    for (int i = 0; i < 12; i++) world_sim_step(w, SIM_DT);
    ASSERT(!sp->docked);
    ASSERT_EQ_INT(sp->ship_asset_id, asset_id);
    ASSERT(sp->ship->hull > 0.0f);
}

void register_respawn_fee_tests(void) {
    TEST_SECTION("\nRespawn fee + death payload:\n");
    RUN(test_respawn_debits_station_ledger);
    RUN(test_respawn_fee_persists_negative_balance);
    RUN(test_respawn_event_carries_station_and_fee);
    RUN(test_self_destruct_is_consumed_before_respawn_launch);
    RUN(test_docked_self_destruct_is_consumed_before_launch);
}
