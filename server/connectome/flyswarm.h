/*
 * flyswarm.h -- compute as an economic resource.
 *
 * A connectome brain costs real cycles, and `signal` runs a 120 Hz fixed-step
 * sim on one Fly CPU. Measured, one nav-circuit agent-step costs ~0.024 ms and
 * one whole-brain step ~0.5 ms, so a 2 ms brain slice per tick buys roughly 83
 * nav steps -- against the ~400 a hundred drones would need to track real time.
 * The budget cannot be met by running everyone. It has to be rationed.
 *
 * So rather than declaring tiers, this rations thinking by earned stake and lets
 * the hierarchy fall out:
 *
 *   - every agent gets a floor of steps, so nothing is ever brain-dead
 *   - the remainder is shared in proportion to stake, which the game sets from
 *     world state (station balance, cargo value, contract value)
 *   - an agent whose stake clears a threshold is promoted to the whole-brain
 *     circuit, which costs ~25x more per step and so consumes its whole share
 *
 * A productive station thinks deeply and slowly; a broke drone drops to reflex.
 * Feedback does the rest. There is a cap so one winner cannot starve the swarm.
 *
 * Determinism: allocation is integer arithmetic over stakes that come from
 * replayed world state. No wall clock, no floats, no rounding drift. The budget
 * is a fixed integer chosen once for the deployment target, so two machines with
 * different speeds still produce identical simulations -- a slow machine falls
 * behind in wall time rather than diverging.
 */
#ifndef FLYSWARM_H
#define FLYSWARM_H

#include "flybrain.h"

typedef struct {
    /* Two circuits: cheap reflex, expensive deliberation. Either may be NULL. */
    const fb_circuit *fast;
    const fb_circuit *deep;
    fb_sim sim_fast;
    fb_sim sim_deep;

    uint32_t n_agents;
    uint32_t *stake;      /* set by the game each tick, arbitrary integer units */
    uint32_t *units;      /* last allocation, in cost units */
    uint32_t *steps;      /* last allocation, in brain steps */
    uint32_t *credit;     /* banked units, so a slow thinker can burst */
    uint8_t  *deep_on;    /* 1 if this agent currently runs the deep circuit */

    /* Budget, in cost units per tick. One fast step costs 1 unit. */
    uint32_t budget_units;
    uint32_t floor_units;   /* guaranteed per agent -- the reflex floor */
    uint32_t cap_units;     /* ceiling per agent, 0 for none */
    uint32_t deep_cost;     /* cost units per deep-circuit step */

    /* Promotion needs more stake than demotion, so agents near the line do not
     * flap between circuits every tick. */
    uint32_t promote_stake;
    uint32_t demote_stake;

    /* Deep circuits are SCARCE: at most this many agents may hold one, ranked by
     * stake. 0 means unlimited, which is a mistake in production -- if everyone
     * can deliberate, the budget shatters and promotion stops meaning anything.
     * Scarcity is also what makes a hierarchy emerge at all: linear returns to
     * thinking converge to uniform, and the discrete jump of winning a slot is
     * the only genuinely increasing return in the system. */
    uint32_t deep_slots;
    /* An incumbent must be out-staked by this factor to lose its slot. */
    uint32_t incumbent_edge;

    uint64_t tick;
    uint64_t units_spent;
    uint64_t steps_run;
    uint32_t promotions;
    uint32_t demotions;
} fb_swarm;

int  fb_swarm_init(fb_swarm *sw, const fb_circuit *fast, const fb_circuit *deep,
                   uint32_t n_agents, int32_t dt_us, uint64_t seed);
void fb_swarm_free(fb_swarm *sw);

/* Set an agent's stake. Units are the game's own; only ratios matter. */
void fb_swarm_set_stake(fb_swarm *sw, uint32_t agent, uint32_t stake);

/* Apply promotion/demotion, then divide budget_units across agents by stake.
 * Writes units[] and steps[]. Returns total units allocated. */
uint32_t fb_swarm_allocate(fb_swarm *sw);

/* Allocate, then run each agent for its allotted steps. */
void fb_swarm_tick(fb_swarm *sw);

/* The sim an agent's state currently lives in, for injection and readout. */
fb_sim *fb_swarm_sim_for(fb_swarm *sw, uint32_t agent);

/* Units an agent has banked but not yet spent. A deep thinker accumulates for
 * several ticks and then bursts, which is what "slower and smarter" means here. */
uint32_t fb_swarm_credit(const fb_swarm *sw, uint32_t agent);

/* Brain-time an agent actually received last tick, in microseconds. Agents
 * below their share run in slow motion, which is the intended flavour: a cheap
 * drone is visibly sluggish. */
uint32_t fb_swarm_brain_us(const fb_swarm *sw, uint32_t agent);

uint64_t fb_swarm_checksum(const fb_swarm *sw);

#endif /* FLYSWARM_H */
