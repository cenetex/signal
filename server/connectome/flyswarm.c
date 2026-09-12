/*
 * flyswarm.c -- Vendored into signal from the flybrain project
 * (connectome/flybrain in the develop tree), snapshot of 2026-09-11.
 * Local change on top of that snapshot: the largest-remainder leftover
 * pass now skips zero-stake agents, so a sleeping (docked, stake 0)
 * fly is never handed free brain time it did not earn -- which is what
 * makes "idle flies rent their compute to the swarm" a real property
 * rather than a rounding leak.
 * Re-sync with upstream before making further kernel changes.
 */
#include "flyswarm.h"

#include <stdlib.h>
#include <string.h>

int fb_swarm_init(fb_swarm *sw, const fb_circuit *fast, const fb_circuit *deep,
                  uint32_t n_agents, int32_t dt_us, uint64_t seed)
{
    memset(sw, 0, sizeof(*sw));
    if (!n_agents || (!fast && !deep)) return -1;
    sw->fast = fast; sw->deep = deep; sw->n_agents = n_agents;
    if (fast && fb_sim_init(&sw->sim_fast, fast, n_agents, dt_us, seed) != 0)
        return -1;
    if (deep && fb_sim_init(&sw->sim_deep, deep, n_agents, dt_us, seed) != 0) {
        fb_swarm_free(sw); return -1;
    }
    sw->stake   = (uint32_t *)calloc(n_agents, sizeof(uint32_t));
    sw->units   = (uint32_t *)calloc(n_agents, sizeof(uint32_t));
    sw->steps   = (uint32_t *)calloc(n_agents, sizeof(uint32_t));
    sw->credit  = (uint32_t *)calloc(n_agents, sizeof(uint32_t));
    sw->deep_on = (uint8_t  *)calloc(n_agents, sizeof(uint8_t));
    if (!sw->stake || !sw->units || !sw->steps || !sw->credit || !sw->deep_on) {
        fb_swarm_free(sw); return -1;
    }
    /* Defaults measured on one core: a whole-brain step costs about 25 nav
     * steps. Budget is deliberately small -- it is a slice of an 8.33 ms tick
     * that the rest of the sim also needs. */
    sw->budget_units = 80;
    sw->floor_units = 1;
    sw->cap_units = 0;
    sw->deep_cost = 25;
    sw->promote_stake = 0;
    sw->demote_stake = 0;
    sw->deep_slots = 0;
    sw->incumbent_edge = 2;
    return 0;
}

void fb_swarm_free(fb_swarm *sw)
{
    if (!sw) return;
    if (sw->fast) fb_sim_free(&sw->sim_fast);
    if (sw->deep) fb_sim_free(&sw->sim_deep);
    free(sw->stake); free(sw->units); free(sw->steps); free(sw->credit);
    free(sw->deep_on);
    memset(sw, 0, sizeof(*sw));
}

void fb_swarm_set_stake(fb_swarm *sw, uint32_t agent, uint32_t stake)
{
    if (agent < sw->n_agents) sw->stake[agent] = stake;
}

fb_sim *fb_swarm_sim_for(fb_swarm *sw, uint32_t agent)
{
    if (agent >= sw->n_agents) return NULL;
    if (sw->deep_on[agent] && sw->deep) return &sw->sim_deep;
    return sw->fast ? &sw->sim_fast : &sw->sim_deep;
}

static uint32_t cost_of(const fb_swarm *sw, uint32_t agent)
{
    return (sw->deep_on[agent] && sw->deep) ? sw->deep_cost : 1u;
}

uint32_t fb_swarm_allocate(fb_swarm *sw)
{
    const uint32_t n = sw->n_agents;

    /* Deep slots are scarce and awarded by stake rank. Incumbents carry an edge
     * so a near-tie does not swap them out every tick. Selection is a bounded
     * pass over agents rather than a sort, so it stays deterministic and cheap. */
    if (sw->deep && sw->deep_slots) {
        uint32_t slots = sw->deep_slots > n ? n : sw->deep_slots;
        uint32_t edge = sw->incumbent_edge ? sw->incumbent_edge : 1;
        uint8_t *want = (uint8_t *)calloc(n, 1);
        if (want) {
            for (uint32_t k = 0; k < slots; k++) {
                uint32_t best = n; uint64_t best_w = 0;
                for (uint32_t i = 0; i < n; i++) {
                    if (want[i] || sw->stake[i] < sw->promote_stake) continue;
                    uint64_t w = (uint64_t)sw->stake[i] *
                                 (sw->deep_on[i] ? edge : 1u);
                    if (best == n || w > best_w) { best = i; best_w = w; }
                }
                if (best == n) break;
                want[best] = 1;
            }
            for (uint32_t i = 0; i < n; i++) {
                if (want[i] && !sw->deep_on[i]) {
                    sw->deep_on[i] = 1; sw->promotions++; sw->credit[i] = 0;
                    fb_sim_reset_agent(&sw->sim_deep, i, sw->stake[i] ^ (i + 1));
                } else if (!want[i] && sw->deep_on[i]) {
                    sw->deep_on[i] = 0; sw->demotions++; sw->credit[i] = 0;
                    if (sw->fast) fb_sim_reset_agent(&sw->sim_fast, i,
                                                     sw->stake[i] ^ (i + 1));
                }
            }
            free(want);
        }
    } else if (sw->deep && sw->promote_stake) {
        for (uint32_t i = 0; i < n; i++) {
            if (!sw->deep_on[i] && sw->stake[i] >= sw->promote_stake) {
                sw->deep_on[i] = 1; sw->promotions++; sw->credit[i] = 0;
                fb_sim_reset_agent(&sw->sim_deep, i, sw->stake[i] ^ (i + 1));
            } else if (sw->deep_on[i] && sw->stake[i] < sw->demote_stake) {
                sw->deep_on[i] = 0; sw->demotions++; sw->credit[i] = 0;
                if (sw->fast) fb_sim_reset_agent(&sw->sim_fast, i,
                                                 sw->stake[i] ^ (i + 1));
            }
        }
    }

    memset(sw->units, 0, n * sizeof(uint32_t));

    /* The reflex floor comes out first, so an agent with zero stake still
     * twitches. If the floor alone exceeds the budget, everyone shares equally
     * and nothing is allocated by stake. */
    uint64_t budget = sw->budget_units;
    uint64_t floor_total = (uint64_t)sw->floor_units * n;
    uint32_t base = sw->floor_units;
    if (floor_total > budget) {
        base = (uint32_t)(budget / n);
        floor_total = (uint64_t)base * n;
    }
    for (uint32_t i = 0; i < n; i++) sw->units[i] = base;
    uint64_t pool = budget - floor_total;

    /* Largest-remainder proportional share. Integer throughout: the floor
     * division gives each agent its whole units, then the leftover goes to the
     * largest remainders, ties broken by index. Sums exactly, no drift. */
    uint64_t total_stake = 0;
    for (uint32_t i = 0; i < n; i++) total_stake += sw->stake[i];

    if (pool && total_stake) {
        uint64_t given = 0;
        uint32_t *rem = (uint32_t *)calloc(n, sizeof(uint32_t));
        if (rem) {
            for (uint32_t i = 0; i < n; i++) {
                uint64_t exact = (uint64_t)sw->stake[i] * pool;
                uint32_t whole = (uint32_t)(exact / total_stake);
                rem[i] = (uint32_t)(exact % total_stake);
                sw->units[i] += whole;
                given += whole;
            }
            uint64_t leftover = pool - given;
            while (leftover--) {
                uint32_t best = 0; int found = 0;
                for (uint32_t i = 0; i < n; i++) {
                    if (!sw->stake[i]) continue;   /* sleeping flies rent out, */
                    if (!found || rem[i] > rem[best]) { best = i; found = 1; }
                                                 /* they don't get handouts  */
                }
                if (!found) break;
                sw->units[best]++;
                rem[best] = 0;
            }
            free(rem);
        }
        /* On allocation failure the floor share stands and the pool goes
         * unspent; the cap/bank pass below still runs. */
    }

    /* Cap, so a single winner cannot starve the swarm. Reclaimed units are not
     * redistributed: the budget is a ceiling, not a quota to be spent. */
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (sw->cap_units && sw->units[i] > sw->cap_units)
            sw->units[i] = sw->cap_units;
        allocated += sw->units[i];

        /* Bank, then spend whole steps. An agent allotted less than one step of
         * its circuit accumulates until it can afford one, so promotion can
         * never make an agent think LESS than it did as a reflex -- it makes it
         * think less OFTEN and more deeply. Credit is capped so a long-idle
         * agent cannot bank an unbounded burst. */
        uint32_t cost = cost_of(sw, i);
        uint64_t bank = (uint64_t)sw->credit[i] + sw->units[i];
        uint64_t bank_cap = (uint64_t)cost * 64u;
        if (bank > bank_cap) bank = bank_cap;
        sw->steps[i] = (uint32_t)(bank / cost);
        sw->credit[i] = (uint32_t)(bank % cost);
    }
    return allocated;
}

void fb_swarm_tick(fb_swarm *sw)
{
    sw->units_spent += fb_swarm_allocate(sw);
    for (uint32_t i = 0; i < sw->n_agents; i++) {
        fb_sim *s = fb_swarm_sim_for(sw, i);
        if (!s) continue;
        for (uint32_t k = 0; k < sw->steps[i]; k++) fb_sim_step_agent(s, i);
        sw->steps_run += sw->steps[i];
    }
    sw->tick++;
}

uint32_t fb_swarm_credit(const fb_swarm *sw, uint32_t agent)
{
    return agent < sw->n_agents ? sw->credit[agent] : 0;
}

uint32_t fb_swarm_brain_us(const fb_swarm *sw, uint32_t agent)
{
    if (agent >= sw->n_agents) return 0;
    const fb_sim *s = (sw->deep_on[agent] && sw->deep) ? &sw->sim_deep
                                                       : &sw->sim_fast;
    return (uint32_t)(sw->steps[agent] * (uint32_t)s->dt_us);
}

uint64_t fb_swarm_checksum(const fb_swarm *sw)
{
    uint64_t h = 1469598103934665603ULL;
    if (sw->fast) h ^= fb_sim_checksum(&sw->sim_fast), h *= 1099511628211ULL;
    if (sw->deep) h ^= fb_sim_checksum(&sw->sim_deep), h *= 1099511628211ULL;
    for (uint32_t i = 0; i < sw->n_agents; i++) {
        h ^= sw->units[i]; h *= 1099511628211ULL;
        h ^= sw->credit[i]; h *= 1099511628211ULL;
        h ^= sw->deep_on[i]; h *= 1099511628211ULL;
    }
    return h;
}
