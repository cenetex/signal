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

/* Crystal-ore asteroids use constructed rectangle geometry instead of
 * the polar profile path (rectangles can't be expressed as a single
 * radius-per-angle without rounding the corners). The fill helper is
 * called inside an open sgl_begin_triangles() / sgl_end() batch; the
 * outline helper opens its own sgl_begin_lines/end. */
void draw_crystal_asteroid_fill(const asteroid_t *a);
void draw_crystal_asteroid_outline(const asteroid_t *a, float r, float g, float b, float alpha);

/* Number of rectangle "spikes" a crystal asteroid renders with — 3 for
 * fragments and small clusters, 5 for the larger ones. Same math is
 * used by the AABB / hit-test code if any. */
int  crystal_spike_count(const asteroid_t *a);

/* Float-RGB grade tint for sokol_gl callers. Delegates to the canonical
 * mining_grade_rgb in shared/mining.h. UI code should call that directly
 * instead of routing grade colors through world_draw. */
void grade_tint(uint8_t grade, float *r, float *g, float *b);

/* --- World object drawing --- */
void draw_background(vec2 camera);
void draw_station(const station_t* station, bool is_current, bool is_nearby);
void draw_station_rings(const station_t* station, bool is_current, bool is_nearby);
void draw_ship_tractor_field(void);
void draw_ship(void);
void draw_death_wreckage(void);
void draw_npc_ship(const npc_ship_t* npc);
void draw_npc_mining_beam(const npc_ship_t* npc);
void draw_npc_ships(void);
void draw_hopper_tractors(void);
void draw_beam(void);
void draw_collision_sparks(void);
void draw_spark_burst(vec2 pos, float intensity, bool red, float seed);
void draw_autopilot_path(void);
void draw_towed_tethers(void);

/* --- Module visuals --- */
void module_color_fn(module_type_t type, float *r, float *g, float *b);

/* --- Scaffolds --- */
void draw_scaffolds(void);
void draw_scaffold_tether(void);
void draw_shipyard_intake_beams(void);
void draw_placement_reticle(void);

/* --- Signal field helpers --- */
int nearest_signal_station(vec2 pos);
void draw_signal_borders(void);

/* --- Compass ring --- */
void draw_compass_ring(void);

/* --- In-world yellow ring at the tracked contract's next objective --- */
void draw_tracked_contract_highlight(void);

/* --- Multiplayer rendering --- */
void draw_remote_players(void);
void draw_callsigns(void);
void draw_npc_chatter(void);

/* --- Sell FX: floating "+$N" popups on SIM_EVENT_SELL --- */
void spawn_sell_fx(const vec2 *origin, int amount, mining_grade_t grade, bool by_contract);
void update_sell_fx(float dt);
void draw_sell_fx(void);

#endif /* WORLD_DRAW_H */
