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
/* The shared spatial grid retains every asteroid so navigation cannot miss
 * dense obstacles. The 30 Hz pair solver keeps a deterministic per-cell
 * workload matching the established flight model and browser frame budget. */
#define ASTEROID_PHYSICS_CELL_BUDGET 16u

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

/* Physics API — called from world_sim_step */
void step_asteroid_gravity(world_t *w, float dt);
void resolve_asteroid_collisions(world_t *w);
void resolve_asteroid_station_collisions(world_t *w);

void asteroid_mark_thrown(asteroid_t *a, const uint8_t token[8], float seconds);
void asteroid_clear_thrown(asteroid_t *a);
bool asteroid_is_ballistic(const asteroid_t *a);
void asteroid_step_thrown_state(world_t *w);

#endif /* SIM_PHYSICS_H */
