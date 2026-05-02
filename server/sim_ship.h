/*
 * sim_ship.h — Shared ship physics primitives.
 *
 * Slice 2 of #294 (unified ship_t + separable controllers): the
 * controller substrate that both AI brains and human input feed into.
 * These functions take an `input_intent_t` worth of inputs (turn,
 * thrust, boost) and a `ship_t` and produce the next physics step,
 * with no knowledge of whether the controller is a player or an NPC.
 *
 * Today these are extracted as-is from game_sim.c so the player path
 * keeps working. Slice 3 (NPC migration) will route the existing
 * npc_steer_* helpers through the same primitives instead of mutating
 * `npc_ship_t.pos/vel/angle` directly.
 *
 * Keep functions here PURE in the sense that they only mutate
 * `ship_t`. Anything role-specific (player damage, NPC nav replan,
 * boost-drain accounting hooked to player-only state) lives outside.
 */
#ifndef SIM_SHIP_H
#define SIM_SHIP_H

#include "game_sim.h"  /* world_t */
#include "ship.h"
#include "signal_model.h"
#include "math_util.h"

/* Apply turn input to ship angle. turn_input ∈ [-1, +1]; the hull's
 * turn_speed sets the max rad/s. */
void step_ship_rotation(ship_t *s, float dt, float turn_input);

/* Apply forward/reverse thrust along `forward` direction. thrust_input
 * > 0 = main engine (× hull->accel × boost multiplier), < 0 = brake.
 * boost_hold is the held-boost duration in seconds (used for the
 * exponential takeoff kick). */
void step_ship_thrust(ship_t *s, float dt, float thrust_input,
                      vec2 forward, bool boost, float boost_hold);

/* Boost thrust multiplier curve. Exposed so callers that want to
 * report "current boost effect" to the HUD can read it without
 * duplicating the formula. */
float ship_boost_thrust_mult(bool boost, float hold_t);

/* Integrate velocity onto position with hull drag, plus apply the
 * signal-frontier inward push when the ship is past the boundary
 * (`cached_signal` is the precomputed signal strength at the ship's
 * current position; passed in so callers can amortize the lookup). */
void step_ship_motion(ship_t *s, float dt, const world_t *w,
                      float cached_signal);

/* Tiny gap pushed past every collision edge so sub-pixel tangential
 * drift on the next frame doesn't immediately re-trigger this same
 * surface. Both player + NPC corridor pushback now share this. */
#define SHIP_COLLISION_SKIN 1.5f

/* Push a ship out of a station corridor's annular sector if it's
 * inside one. arc_delta is the canonical forward span from the geom
 * emitter (no shortest-arc normalization). Mutates `pos` and zeroes
 * the inward component of `vel`.
 *
 * Returns the inward velocity magnitude at impact (≥ 0). 0 means no
 * contact, > 0 is the speed at which the ship hit the wall — caller
 * uses it to decide damage / nav-replan / etc. */
float resolve_ship_annular_pushback(ship_t *ship, vec2 center,
                                    float ring_r, float angle_a, float arc_delta);

#endif /* SIM_SHIP_H */
