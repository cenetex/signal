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
/* Public API                                                         */
/* ------------------------------------------------------------------ */

/* Path cache accessors — hide storage, callers don't touch globals. */
nav_path_t *nav_npc_path(int npc_idx);
nav_path_t *nav_player_path(int player_id);
void nav_force_replan(nav_path_t *path);

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

/* ------------------------------------------------------------------ */
/* Shared flight helpers — reusable movement primitives               */
/* ------------------------------------------------------------------ */

/* Result of path-follow steering: desired heading + waypoint info. */
typedef struct {
    float desired_heading;   /* angle to steer toward (radians) */
    float wp_dist;           /* distance to current waypoint */
    bool  at_intermediate;   /* true if current wp is not the final target */
} nav_steer_t;

/* Ensure a path is fresh: replan if destination changed or stale.
 * Returns the next waypoint to steer toward. */
vec2 nav_follow_path(const world_t *w, nav_path_t *path,
                     vec2 ship_pos, vec2 destination,
                     float clearance, float dt);

/* Compute steering toward the current A* waypoint. */
nav_steer_t nav_steer_toward_waypoint(nav_path_t *path, vec2 ship_pos,
                                       vec2 destination, float dt);

/* Velocity-controlled approach: desired speed from distance using
 * sqrt(2 * decel * dist), capped at max_speed, floor at 30 u/s. */
float nav_approach_speed(float dist, float max_speed);

/* Speed control: returns thrust command (-1..1) to hold target speed.
 * Deadband at 85-110% avoids oscillation. */
float nav_speed_control(float current_speed, float target_speed);

/* Build/rebuild the precomputed nav mesh for a single station. */
void station_build_nav(const world_t *w, int station_idx);

/* Rebuild nav meshes for all stations. */
void station_rebuild_all_nav(const world_t *w);

#endif /* SIM_NAV_H */
