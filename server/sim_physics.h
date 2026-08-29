/*
 * sim_physics.h -- Asteroid physics: N-body gravity, asteroid-asteroid
 * collision, and asteroid-station collision.  Extracted from game_sim.c.
 */
#ifndef SIM_PHYSICS_H
#define SIM_PHYSICS_H

#include "game_sim.h"

/* Rock throws keep combat ownership for a short, quantized window.
 * Smelt provenance remains on last_towed_token; only this state credits
 * thrown-rock damage and kills. */
#define ROCK_THROW_BALLISTIC_SECONDS 6.0f
#define ASTEROID_THROW_TIMER_TICKS 12u  /* 120 Hz sim -> 0.1s timer quantum */
#define ASTEROID_AMBIENT_DRAG 0.42f

/* The 30 Hz asteroid pair plan is exhaustive for ordinary cells, then uses
 * four rotating cyclic-distance bands / bipartite matchings for dense cells.
 * Its phase comes from world.tick (four 120 Hz ticks per epoch); save load
 * reconstructs that tick from persisted world.time.
 *
 * The 3x3 parent-cell topology has at most eight cross-cell neighbors. Each
 * self/cross ownership edge costs at most 8*(left_count+right_count), so the
 * whole immutable plan is bounded by 72 candidates per active body:
 * 8 self + 8 neighbors * 8. No pair array or hot-path allocation is needed. */
#define ASTEROID_PAIR_EXHAUSTIVE_CELL_LIMIT 16u
#define ASTEROID_PAIR_EXHAUSTIVE_CROSS_LIMIT 256u
#define ASTEROID_PAIR_DISTANCE_BANDS 4u
#define ASTEROID_PAIR_TICKS_PER_EPOCH 4u
#define ASTEROID_PAIR_MAX_CANDIDATES_PER_BODY 72u
#define ASTEROID_PAIR_MAX_CANDIDATES \
    (ASTEROID_PAIR_MAX_CANDIDATES_PER_BODY * (uint32_t)MAX_ASTEROIDS)

typedef struct {
    int32_t cell_x;
    int32_t cell_y;
    uint16_t begin;
    uint16_t count;
} asteroid_pair_cell_t;

/* Fixed-capacity, immutable snapshot shared by gravity and collision. Bodies
 * within each cell are identity-first canonical; cells are coordinate-sorted.
 * Slot is only the final tie-break for physics-identical, identity-identical
 * bodies. Anonymous identities are a legacy/test-only fallback and are
 * exchangeable only when their slot metadata and external references are
 * absent/equal (the regression fixture asserts that boundary explicitly). */
typedef struct {
    int16_t indices[MAX_ASTEROIDS];
    asteroid_pair_cell_t cells[MAX_ASTEROIDS];
    uint64_t epoch;
    uint32_t candidate_pair_count;
    uint16_t active_count;
    uint16_t cell_count;
    uint16_t max_cell_count;
} asteroid_pair_plan_t;

typedef void (*asteroid_pair_visitor_t)(
    int asteroid_a, int asteroid_b, void *context);

typedef enum {
    SIM_BODY_FLAG_DYNAMIC   = 1u << 0,
    SIM_BODY_FLAG_SPIN      = 1u << 1,
    SIM_BODY_FLAG_AGE       = 1u << 2,
    SIM_BODY_FLAG_DRAG      = 1u << 3,
    SIM_BODY_FLAG_KINEMATIC = 1u << 4,
    SIM_BODY_FLAG_TOWED     = 1u << 5,
} sim_body_flags_t;

typedef enum {
    SIM_BODY_COLLIDE_NONE     = 0,
    SIM_BODY_COLLIDE_SHIP     = 1u << 0,
    SIM_BODY_COLLIDE_STATION  = 1u << 1,
    SIM_BODY_COLLIDE_ASTEROID = 1u << 2,
    SIM_BODY_COLLIDE_CARGO    = 1u << 3,
} sim_body_collision_mask_t;

typedef enum {
    SIM_BODY_PHASE_ASTEROIDS = 0,
    SIM_BODY_PHASE_CARGO_PODS,
    SIM_BODY_PHASE_SCAFFOLD_AMBIENT,
    SIM_BODY_PHASE_SCAFFOLD_SNAPPING,
} sim_body_phase_t;

/* Common mutable component view used by asteroids, cargo pods, and
 * scaffolds. Gameplay entities retain their domain fields; all motion,
 * spin, age, drag, kinematic state, and collision participation are
 * projected into this one integrator shape. */
typedef struct {
    vec2 *pos;
    vec2 *vel;
    float *rotation;
    float *spin;
    float *age;
    float radius;
    float velocity_multiplier;
    uint16_t flags;          /* sim_body_flags_t */
    uint16_t collision_mask; /* sim_body_collision_mask_t */
} sim_body_t;

typedef struct {
    bool collided;
    vec2 normal;
    float closing_speed;
} sim_body_contact_t;

void sim_body_integrate(sim_body_t body, float dt,
                        float velocity_multiplier);
void sim_body_advance(sim_body_t body, float dt);
sim_body_t sim_body_from_asteroid(asteroid_t *asteroid);
sim_body_t sim_body_from_cargo_pod(cargo_pod_t *pod);
sim_body_t sim_body_from_scaffold(scaffold_t *scaffold);
void sim_world_integrate_bodies(world_t *w, sim_body_phase_t phase,
                                float dt);
sim_body_contact_t sim_body_resolve_static_circle(
    sim_body_t body, vec2 center, float obstacle_radius,
    vec2 obstacle_vel, float bounce_scale, float skin);

/* Physics API — called from world_sim_step. Build once from the already-built
 * asteroid_grid, then pass the same immutable plan to both pair paths. */
bool asteroid_pair_plan_build(const world_t *w, asteroid_pair_plan_t *plan);
uint32_t asteroid_pair_plan_visit(
    const asteroid_pair_plan_t *plan,
    asteroid_pair_visitor_t visitor,
    void *context);
uint32_t asteroid_pair_self_revisit_epochs(uint16_t body_count);
uint32_t asteroid_pair_cross_revisit_epochs(
    uint16_t body_count_a, uint16_t body_count_b);
void step_asteroid_gravity(
    world_t *w, float dt, const asteroid_pair_plan_t *plan);
void resolve_asteroid_collisions(
    world_t *w, const asteroid_pair_plan_t *plan);
void resolve_asteroid_station_collisions(world_t *w);
void resolve_asteroid_station_collisions_frequent(world_t *w);
void resolve_asteroid_station_collision(world_t *w, int asteroid_idx);

void asteroid_mark_thrown(asteroid_t *a, const uint8_t token[8], float seconds);
void asteroid_clear_thrown(asteroid_t *a);
bool asteroid_is_ballistic(const asteroid_t *a);
void asteroid_step_thrown_state(world_t *w);

#endif /* SIM_PHYSICS_H */
