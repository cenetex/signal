/*
 * test_pvp_rocks.c — scenario tests for PvP rock-throwing.
 *
 * Covers the launch feature: tow a rock, release with momentum, hit
 * another ship (player or NPC), credit the kill via last_towed_token.
 *
 * Behaviors locked in:
 *   - Releasing a tow throws the rock with ship.vel + forward * fling.
 *   - The rock retains last_towed_token across release; that token is
 *     the kill attribution if the rock damages a ship.
 *   - Self-damage prevented: your own thrown rock can't hurt you.
 *   - Damage scales with rock radius (size_mult 0.5..2.5).
 *   - SIM_EVENT_DEATH carries killer_token + cause.
 *   - SIM_EVENT_NPC_KILL fires when a player's thrown rock kills an NPC.
 */

#include "tests/test_harness.h"

/* Helpers ---------------------------------------------------------- */

/* Spawn a fragment-tier asteroid at pos, owned by sp's session token,
 * fully towed (in the ship's tow array). Returns the asteroid index.
 *
 * We bypass the normal mining flow (fracture-claim window, mining beam,
 * tractor pull) because none of that is what these tests exercise — we
 * just need a fragment in tow with the player's token stamped on it. */
static int spawn_towed_fragment(world_t *w, server_player_t *sp,
                                 vec2 pos, float radius) {
    int idx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;
    asteroid_t *a = &w->asteroids[idx];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;
    a->pos = pos;
    a->vel = v2(0.0f, 0.0f);
    a->radius = radius;
    a->hp = 1.0f;
    a->max_hp = 1.0f;
    a->ore = 1.0f;
    a->max_ore = 1.0f;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->last_towed_by = (int8_t)sp->id;
    memcpy(a->last_towed_token, sp->session_token, 8);
    /* Drop into the tow array. Caller is responsible for towed_count. */
    if (sp->ship.towed_count < (int)(sizeof(sp->ship.towed_fragments)/sizeof(sp->ship.towed_fragments[0]))) {
        sp->ship.towed_fragments[sp->ship.towed_count++] = (int16_t)idx;
    }
    return idx;
}

/* Find the first SIM_EVENT_DEATH for a given player_id in this tick's
 * event buffer. Returns NULL if not present. */
static const sim_event_t *find_death_event(const world_t *w, int player_id) {
    for (int i = 0; i < w->events.count; i++) {
        const sim_event_t *e = &w->events.events[i];
        if (e->type == SIM_EVENT_DEATH && e->player_id == player_id) return e;
    }
    return NULL;
}

static const sim_event_t *find_npc_kill_event(const world_t *w) {
    for (int i = 0; i < w->events.count; i++) {
        const sim_event_t *e = &w->events.events[i];
        if (e->type == SIM_EVENT_NPC_KILL) return e;
    }
    return NULL;
}

/* Minimal two-player setup. Both connected, both undocked, both with
 * unique session tokens. */
static void setup_two_players(world_t *w) {
    world_reset(w);
    player_init_ship(&w->players[0], w);
    player_init_ship(&w->players[1], w);
    w->players[0].connected = true;
    w->players[1].connected = true;
    /* Session tokens — avoid all-zero so attribution checks see them. */
    memcpy(w->players[0].session_token, "PLAYER_A", 8);
    memcpy(w->players[1].session_token, "PLAYER_B", 8);
    /* Pull both off-dock so collisions actually fire. */
    w->players[0].docked = false;
    w->players[1].docked = false;
    w->players[0].current_station = -1;
    w->players[1].current_station = -1;
    w->players[0].ship.pos = v2(0.0f, 0.0f);
    w->players[1].ship.pos = v2(500.0f, 0.0f);
}

/* Tests ------------------------------------------------------------ */

TEST(test_release_imparts_throw_velocity) {
    /* Slingshot release fires the rock along the BAND AXIS (from
     * rock toward ship, then beyond). Set up the ship moving +X with
     * a rock 100 u behind it (-X) — the band pulls the rock toward
     * the ship, which is +X, so on release the rock fires +X past
     * the ship's current velocity. 100 u of stretch beyond
     * BAND_REST_LEN (80) gives ~20 u of elastic stretch -> ~50 m/s
     * elastic + 40 m/s base = ~90 m/s along +X, on top of ship's
     * +30 m/s velocity. Comfortably > ship.vel + 30. */
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *sp = &w.players[0];
    sp->ship.pos   = v2(0.0f, 0.0f);
    sp->ship.angle = 0.0f;            /* +X facing */
    sp->ship.vel   = v2(30.0f, 0.0f);
    int aidx = spawn_towed_fragment(&w, sp, v2(-100.0f, 0.0f), 12.0f);
    ASSERT(aidx >= 0);

    /* Trigger release via the input intent path. Run one sim tick so
     * the server consumes the intent. */
    sp->input.release_tow = true;
    world_sim_step(&w, 1.0f / 120.0f);

    asteroid_t *a = &w.asteroids[aidx];
    /* Towed array was cleared. */
    ASSERT_EQ_INT(sp->ship.towed_count, 0);
    /* Rock fires along band axis (+X toward and past the ship), at
     * meaningfully more than ship's own velocity. */
    ASSERT(a->vel.x > sp->ship.vel.x + 30.0f);
    /* last_towed_token preserved on release. */
    ASSERT(memcmp(a->last_towed_token, sp->session_token, 8) == 0);
}

TEST(test_thrown_rock_damages_target_player) {
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *thrower = &w.players[0];
    server_player_t *target  = &w.players[1];

    /* Place fragment between the two players, owned by thrower, moving
     * fast toward target. Skip the tow + release dance — just simulate
     * the post-release state. */
    int aidx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { aidx = i; break; }
    }
    ASSERT(aidx >= 0);
    asteroid_t *a = &w.asteroids[aidx];
    memset(a, 0, sizeof(*a));
    a->active = true; a->fracture_child = true;
    a->tier = ASTEROID_TIER_M;
    a->radius = 30.0f;
    a->hp = 1.0f; a->max_hp = 1.0f; a->ore = 1.0f; a->max_ore = 1.0f;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->pos = v2(target->ship.pos.x - 50.0f, target->ship.pos.y);
    a->vel = v2(400.0f, 0.0f);
    memcpy(a->last_towed_token, thrower->session_token, 8);
    a->last_towed_by = (int8_t)thrower->id;

    float hull_before = target->ship.hull;
    /* Step a few ticks so the rock crosses the gap and hits target. */
    for (int t = 0; t < 60; t++) world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(target->ship.hull < hull_before);
}

TEST(test_thrown_rock_self_damage_prevented) {
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *sp = &w.players[0];

    /* Rock owned by sp, flying directly into sp's hull. Should bounce
     * (or pass through geometrically) but apply zero damage. */
    int aidx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { aidx = i; break; }
    }
    ASSERT(aidx >= 0);
    asteroid_t *a = &w.asteroids[aidx];
    memset(a, 0, sizeof(*a));
    a->active = true; a->fracture_child = true;
    a->tier = ASTEROID_TIER_M;
    a->radius = 30.0f;
    a->hp = 1.0f; a->max_hp = 1.0f; a->ore = 1.0f; a->max_ore = 1.0f;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->pos = v2(sp->ship.pos.x - 50.0f, sp->ship.pos.y);
    a->vel = v2(500.0f, 0.0f);
    memcpy(a->last_towed_token, sp->session_token, 8);

    float hull_before = sp->ship.hull;
    for (int t = 0; t < 60; t++) world_sim_step(&w, 1.0f / 120.0f);
    ASSERT_EQ_FLOAT(sp->ship.hull, hull_before, 0.01f);
}

TEST(test_kill_attribution_via_last_towed_token) {
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *thrower = &w.players[0];
    server_player_t *target  = &w.players[1];
    /* Pre-damage target so a single hit kills. */
    target->ship.hull = 1.0f;

    int aidx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { aidx = i; break; }
    }
    ASSERT(aidx >= 0);
    asteroid_t *a = &w.asteroids[aidx];
    memset(a, 0, sizeof(*a));
    a->active = true; a->fracture_child = true;
    a->tier = ASTEROID_TIER_L;
    a->radius = 50.0f;
    a->hp = 1.0f; a->max_hp = 1.0f; a->ore = 1.0f; a->max_ore = 1.0f;
    a->commodity = COMMODITY_FERRITE_ORE;
    a->pos = v2(target->ship.pos.x - 80.0f, target->ship.pos.y);
    a->vel = v2(500.0f, 0.0f);
    memcpy(a->last_towed_token, thrower->session_token, 8);

    /* Step until the death event fires. */
    const sim_event_t *death = NULL;
    for (int t = 0; t < 120 && !death; t++) {
        world_sim_step(&w, 1.0f / 120.0f);
        death = find_death_event(&w, target->id);
    }
    ASSERT(death != NULL);
    ASSERT_EQ_INT(death->death.cause, DEATH_CAUSE_THROWN_ROCK);
    ASSERT(memcmp(death->death.killer_token, thrower->session_token, 8) == 0);
}

TEST(test_ramming_attributes_kill) {
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *rammer = &w.players[0];
    server_player_t *target = &w.players[1];

    /* Put them next to each other, rammer flying into target hard,
     * target pre-damaged so a single hit kills. */
    rammer->ship.pos = v2(target->ship.pos.x - 30.0f, target->ship.pos.y);
    rammer->ship.vel = v2(500.0f, 0.0f);
    target->ship.hull = 1.0f;
    target->ship.vel = v2(0.0f, 0.0f);

    const sim_event_t *death = NULL;
    for (int t = 0; t < 30 && !death; t++) {
        world_sim_step(&w, 1.0f / 120.0f);
        death = find_death_event(&w, target->id);
    }
    ASSERT(death != NULL);
    ASSERT_EQ_INT(death->death.cause, DEATH_CAUSE_RAM);
    ASSERT(memcmp(death->death.killer_token, rammer->session_token, 8) == 0);
}

TEST(test_thrown_rock_kills_npc_emits_event) {
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *thrower = &w.players[0];

    /* Find an active hauler — they always run the collision path
     * (vs miners, where it's gated on state). Move it far from any
     * station so the asteroid doesn't bounce off a module mid-flight. */
    int npc_idx = -1;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (w.npc_ships[i].active && w.npc_ships[i].role == NPC_ROLE_HAULER) {
            npc_idx = i; break;
        }
    }
    ASSERT(npc_idx >= 0);
    npc_ship_t *npc = &w.npc_ships[npc_idx];
    /* Pin BOTH the npc-side hull and the paired ship_t hull. Damage is
     * routed to the paired ship.hull (slice 9+); the npc.hull is just
     * a mirror that gets overwritten at end-of-tick. With the slice-3a
     * mass-equal asteroid bounce, the rock loses momentum each impact,
     * so a single 800 u/s hit doesn't escalate to multiple lethal
     * hits — start the ship at hull 1.0 so the very first contact is
     * fatal, regardless of how many bounces follow. */
    npc->hull = 1.0f;
    npc->vel  = v2(0.0f, 0.0f);
    /* Use Prospect's position (station 0) so we're inside signal coverage
     * — the chunk-streaming layer culls asteroids outside materialized
     * (signal-covered) chunks. Slightly offset so the asteroid path
     * doesn't run through station modules. */
    npc->pos  = v2(w.stations[0].pos.x + 600.0f, w.stations[0].pos.y);
    npc->state = NPC_STATE_TRAVEL_TO_DEST; /* force collision pass to run */

    /* Place a flying rock just behind the NPC, owned by thrower. */
    int aidx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { aidx = i; break; }
    }
    ASSERT(aidx >= 0);
    asteroid_t *a = &w.asteroids[aidx];
    memset(a, 0, sizeof(*a));
    a->active = true; a->fracture_child = true;
    a->tier = ASTEROID_TIER_L;
    a->radius = 50.0f;
    a->hp = 1.0f; a->max_hp = 1.0f; a->ore = 1.0f; a->max_ore = 1.0f;
    a->commodity = COMMODITY_FERRITE_ORE;
    /* Place rock already overlapping the NPC and moving toward it —
     * one tick is enough to register the hit. */
    const hull_def_t *hull = npc_hull_def(npc);
    a->pos = v2(npc->pos.x - (hull->ship_radius + a->radius - 5.0f), npc->pos.y);
    a->vel = v2(800.0f, 0.0f);
    memcpy(a->last_towed_token, thrower->session_token, 8);

    const sim_event_t *kill = NULL;
    /* Pin npc.pos and re-pin asteroid each tick so neither hauler nav
     * nor any belt physics drifts them out of the contact window. The
     * test only cares about kill attribution, not nav stability. */
    vec2 npc_pin = npc->pos;
    ship_t *npc_ship = world_npc_ship_for(&w, npc_idx);
    ASSERT(npc_ship != NULL);
    npc_ship->pos  = npc_pin;
    npc_ship->vel  = v2(0.0f, 0.0f);
    npc_ship->hull = 1.0f;
    for (int t = 0; t < 120 && !kill; t++) {
        npc->vel = v2(0.0f, 0.0f);
        npc->pos = npc_pin;
        npc_ship->vel = v2(0.0f, 0.0f);
        npc_ship->pos = npc_pin;
        world_sim_step(&w, 1.0f / 120.0f);
        kill = find_npc_kill_event(&w);
    }
    ASSERT(kill != NULL);
    ASSERT_EQ_INT(kill->npc_kill.cause, DEATH_CAUSE_THROWN_ROCK);
    ASSERT(memcmp(kill->npc_kill.killer_token, thrower->session_token, 8) == 0);
}

/* Small collectible-tier fragments must damage ships too — the previous
 * "ignore collectible asteroids below 40 u/s" gate let parked tow-balls
 * pass straight through hulls. Now the resolver runs for every active
 * asteroid; the closing-velocity + threshold filter handles drifts. */
TEST(test_collectible_fragment_damages_ship) {
    WORLD_DECL;
    setup_two_players(&w);
    server_player_t *thrower = &w.players[0];
    server_player_t *target  = &w.players[1];

    int aidx = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w.asteroids[i].active) { aidx = i; break; }
    }
    ASSERT(aidx >= 0);
    asteroid_t *a = &w.asteroids[aidx];
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->fracture_child = true;
    a->tier = ASTEROID_TIER_S;        /* small / collectible */
    a->radius = 8.0f;
    a->hp = 1.0f; a->max_hp = 1.0f; a->ore = 1.0f; a->max_ore = 1.0f;
    a->commodity = COMMODITY_FERRITE_ORE;
    /* Confirm precondition: this fragment IS a collectible (the case
     * the old gate skipped). */
    ASSERT(asteroid_is_collectible(a));
    a->pos = v2(target->ship.pos.x - 40.0f, target->ship.pos.y);
    a->vel = v2(400.0f, 0.0f);
    memcpy(a->last_towed_token, thrower->session_token, 8);
    a->last_towed_by = (int8_t)thrower->id;

    float hull_before = target->ship.hull;
    for (int t = 0; t < 60; t++) world_sim_step(&w, 1.0f / 120.0f);
    ASSERT(target->ship.hull < hull_before);
}

void register_pvp_rocks_tests(void) {
    TEST_SECTION("\n=== PvP rock-throwing ===\n");
    RUN(test_release_imparts_throw_velocity);
    RUN(test_thrown_rock_damages_target_player);
    RUN(test_thrown_rock_self_damage_prevented);
    RUN(test_kill_attribution_via_last_towed_token);
    RUN(test_ramming_attributes_kill);
    RUN(test_thrown_rock_kills_npc_emits_event);
    RUN(test_collectible_fragment_damages_ship);
}
