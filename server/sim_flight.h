/*
 * sim_flight.h — Shared flight controller for autopilot and NPC ships.
 *
 * Provides high-level flight commands (steer-to, hover-near, brake,
 * face-heading) that encapsulate the A* path following, approach speed
 * control, forward clearance braking, and proportional turn logic that
 * was previously duplicated across sim_autopilot.c and sim_ai.c.
 */
#ifndef SIM_FLIGHT_H
#define SIM_FLIGHT_H

#include "game_sim.h"
#include "sim_nav.h"

/* A flight command: normalized turn (-1..1) and thrust (-1..1). */
typedef struct {
    float turn;
    float thrust;
} flight_cmd_t;

/* Steer the ship along an A* path toward `target`, slowing to a stop
 * at `standoff` distance from the target.  Handles path freshness,
 * waypoint following, proportional turn, velocity-controlled approach,
 * and forward-clearance braking.  `max_speed` caps the cruise speed. */
flight_cmd_t flight_steer_to(const world_t *w, const ship_t *ship,
                              nav_path_t *path, vec2 target,
                              float standoff, float max_speed, float dt);

/* Hover near `target` at `standoff` distance.  Pushes away if too
 * close, pulls in if too far, brakes residual velocity in the sweet
 * spot, and always faces the target. */
flight_cmd_t flight_hover_near(const world_t *w, const ship_t *ship,
                                vec2 target, float standoff);

/* Full brake: oppose current velocity. */
flight_cmd_t flight_brake(const ship_t *ship);

/* Return a proportional turn command (-1..1) to face `desired_angle`. */
float flight_face_heading(const ship_t *ship, float desired_angle);

#endif /* SIM_FLIGHT_H */
