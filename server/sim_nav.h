/*
 * sim_nav.h — A* pathfinding and station nav mesh for Signal Space Miner.
 * Extracted from game_sim.c (#272 slice).
 */
#ifndef SIM_NAV_H
#define SIM_NAV_H

#include "game_sim.h"

/* ------------------------------------------------------------------ */
/* Nav path constants and types                                       */
/* ------------------------------------------------------------------ */

enum {
    NAV_MAX_PATH = 12,
};

typedef struct {
    vec2  waypoints[NAV_MAX_PATH];
    int   count;
    int   current;
    float age;
    vec2  goal;   /* destination this path was computed for */
} nav_path_t;

/* ------------------------------------------------------------------ */
/* Path arrays — one per NPC / player                                 */
/* ------------------------------------------------------------------ */

extern nav_path_t g_npc_paths[MAX_NPC_SHIPS];
extern nav_path_t g_player_paths[MAX_PLAYERS];

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* A* search: find a path from start to goal, avoiding station walls
 * and large asteroids. Returns true if a non-trivial path was found. */
bool nav_find_path(const world_t *w, vec2 start, vec2 goal,
                   float clearance, nav_path_t *out);

/* Advance along a computed path, returning the next waypoint. */
vec2 nav_next_waypoint(nav_path_t *path, vec2 ship_pos, vec2 final_target, float dt);

/* Compute an A* path and copy waypoints into caller buffer. */
int nav_compute_path(const world_t *w, vec2 start, vec2 goal, float clearance,
                     vec2 *out_waypoints, int max_count);

/* Retrieve the current A* path for a player (for rendering preview). */
int nav_get_player_path(int player_id, vec2 *out_waypoints, int max_count, int *out_current);

/* Build/rebuild the precomputed nav mesh for a single station. */
void station_build_nav(const world_t *w, int station_idx);

/* Rebuild nav meshes for all stations. */
void station_rebuild_all_nav(const world_t *w);

#endif /* SIM_NAV_H */
