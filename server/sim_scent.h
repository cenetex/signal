/*
 * sim_scent.h -- physical traces left in space, and the senses that read
 * them.
 *
 * Before this, an NPC miner picked its rock by sweeping every asteroid in
 * the world and ranking them by its home station's demand. That is not a
 * sense, it is omniscience: a rock on the far side of the map advertised
 * itself as loudly as one off the bow, and nothing was ever discovered
 * because nothing was ever hidden.
 *
 * Scent replaces the sweep. Rocks emit into the shared signal field by ore
 * load and grade; laden ships drag a wake behind them. Both are physical
 * traces in the world rather than gossip, which is why they need nothing
 * carried between stations to be smelled -- they sit in the same field the
 * gossip kinds use, and obey the same decay.
 *
 * Two senses read them, at two ranges:
 *
 *   SMELL -- scent_gradient(), long range and coarse. The field is a 32x32
 *            grid of 4096-unit cells, so a gradient says "richer that way",
 *            never "that rock". Good for finding a patch, and for running
 *            down a hauler whose wake has not gone cold.
 *   SIGHT -- a bounded local scan out to scent_sight_radius(), gated on
 *            signal quality. That lives in sim_ai.c with the claim
 *            bookkeeping; this header supplies the reach. Sight is what
 *            actually picks the rock, once smell has got the fly close
 *            enough for there to be one.
 *
 * Direction comes from comparing samples, not from one reading, which is
 * also how the fly brain has to work: its descending bus carries a usable
 * "is it good here" and no usable heading at all.
 */
#ifndef SIM_SCENT_H
#define SIM_SCENT_H

#include "game_sim.h"
#include "signal_field.h"

#include <stdbool.h>

/* Emit this tick's traces: rock scent (amortised across ticks) and the
 * wake of every laden ship. Called once per sim step. */
void scent_step(world_t *w);

/* Scent strength at a point, 0..1, smoothed over nearby cells. */
float scent_sample(const world_t *w, vec2 pos, signal_field_kind_t kind);

/* Steepest-ascent direction for a scent, by comparing neighbouring cells.
 * Writes a unit vector and the strength at pos. Returns false when there
 * is nothing to smell, which is the fly's cue to cast around rather than
 * commit. */
bool scent_gradient(const world_t *w,
                    vec2 pos,
                    signal_field_kind_t kind,
                    vec2 *out_dir,
                    float *out_strength);

/* How far this ship can see. Scales with signal quality where it sits, so
 * a fly out past the relay chain goes half-blind and has to rely on smell. */
float scent_sight_radius(const world_t *w, const ship_t *ship);

#endif /* SIM_SCENT_H */
