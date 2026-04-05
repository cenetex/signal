/*
 * world_draw.h -- World-space rendering: camera/frustum, VFX, ships,
 * asteroids, stations, and multiplayer players.
 * Split from main.c for Phase 3 refactoring.
 */
#ifndef WORLD_DRAW_H
#define WORLD_DRAW_H

#include "types.h"

/* --- Camera / frustum culling --- */
void set_camera_bounds(vec2 camera, float half_w, float half_h);
bool on_screen(float x, float y, float radius);
int  lod_segments(int base_segments, float radius);

/* Frustum bounds (set by set_camera_bounds, read by render_world) */
float cam_left(void);
float cam_right(void);
float cam_top(void);
float cam_bottom(void);

/* --- Asteroid helpers --- */
float asteroid_profile(const asteroid_t* asteroid, float angle);

/* --- World object drawing --- */
void draw_background(vec2 camera);
void draw_station(const station_t* station, bool is_current, bool is_nearby);
void draw_station_rings(const station_t* station, bool is_current, bool is_nearby);
void draw_ship_tractor_field(void);
void draw_ship(void);
void draw_npc_ship(const npc_ship_t* npc);
void draw_npc_mining_beam(const npc_ship_t* npc);
void draw_npc_ships(void);
void draw_hopper_tractors(void);
void draw_beam(void);
void draw_towed_tethers(void);

/* --- Module visuals --- */
void module_color_fn(module_type_t type, float *r, float *g, float *b);

/* --- Multiplayer rendering --- */
void draw_remote_players(void);

#endif /* WORLD_DRAW_H */
