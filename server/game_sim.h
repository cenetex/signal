/*
 * game_sim.h -- Headless game simulation types and API for the
 * Signal Space Miner authoritative server.
 *
 * Shared types (vec2, ship_t, station_t, etc.) come from shared/types.h.
 * Server-only types (server_player_t, world_t) are defined here.
 */
#ifndef GAME_SIM_H
#define GAME_SIM_H

#include <stdio.h>
#include <string.h>
#include "math_util.h"
#include "types.h"

/* ------------------------------------------------------------------ */
/* Constants (server-only)                                            */
/* ------------------------------------------------------------------ */

enum {
    MAX_PLAYERS = 32,
};

static const float WORLD_RADIUS = 2200.0f;
static const float SIM_DT = 1.0f / 120.0f;
static const float MINING_RANGE = 170.0f;
static const float SHIP_BRAKE = 180.0f;
static const float FRAGMENT_TRACTOR_ACCEL = 380.0f;
static const float FRAGMENT_MAX_SPEED = 210.0f;
static const float FRAGMENT_NEARBY_RANGE = 220.0f;
static const int FIELD_ASTEROID_TARGET = 32;
static const float FIELD_ASTEROID_RESPAWN_DELAY = 0.6f;
static const float FRACTURE_CHILD_CLEANUP_AGE = 22.0f;
static const float FRACTURE_CHILD_CLEANUP_DISTANCE = 940.0f;
static const float STATION_DOCK_APPROACH_OFFSET = 34.0f;
static const float SHIP_COLLISION_DAMAGE_THRESHOLD = 115.0f;
static const float SHIP_COLLISION_DAMAGE_SCALE = 0.12f;
static const float NPC_DOCK_TIME = 3.0f;
static const float HAULER_DOCK_TIME = 4.0f;
static const float HAULER_LOAD_TIME = 2.0f;
static const float COLLECTION_FEEDBACK_TIME = 1.1f;

/* ------------------------------------------------------------------ */
/* Server-specific types                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    float turn;
    float thrust;
    bool mine;
    bool interact;
    bool service_sell;
    bool service_repair;
    bool upgrade_mining;
    bool upgrade_hold;
    bool upgrade_tractor;
    bool reset;
} input_intent_t;

typedef struct {
    bool connected;
    uint8_t id;
    void *conn;
    ship_t ship;
    input_intent_t input;
    int current_station;
    int nearby_station;
    bool docked;
    bool in_dock_range;
    bool beam_active;
    bool beam_hit;
    int hover_asteroid;
    vec2 beam_start;
    vec2 beam_end;
    float cargo_sale_value;
    int nearby_fragments;
    int tractor_fragments;
} server_player_t;

typedef struct {
    station_t stations[MAX_STATIONS];
    asteroid_t asteroids[MAX_ASTEROIDS];
    npc_ship_t npc_ships[MAX_NPC_SHIPS];
    server_player_t players[MAX_PLAYERS];
    uint32_t rng;
    float time;
    float field_spawn_timer;
} world_t;

/* ------------------------------------------------------------------ */
/* Hull definitions (declared in shared/types.h, defined in game_sim.c) */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void world_reset(world_t *w);
void world_sim_step(world_t *w, float dt);
void player_init_ship(server_player_t *sp, world_t *w);

#endif /* GAME_SIM_H */
