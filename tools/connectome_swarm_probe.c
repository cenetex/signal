/*
 * connectome_swarm_probe.c -- headless NPC-swarm probe for the fly
 * connectome brain (SERVER_BRAIN_MODE_CONNECTOME).
 *
 * The unit tests deliberately never enable the adapter: it is process
 * global, so switching it on inside the test binary would flip every
 * other sim test's NPCs into connectome mode. That left the mode with
 * no end-to-end coverage at all, which is how it came to ship with a
 * tonic drive below the circuit's ignition threshold -- a silent brain,
 * drive=0 at the arousal gate, and NPCs that never thrust.
 *
 * This runs the real world_sim_step loop headless and reports what the
 * swarm actually did, so "the miners work" is a measurement instead of
 * a hope. Run it both ways to compare:
 *
 *   ./build/connectome_swarm_probe 3600
 *   SIGNAL_CONNECTOME_FAST=path/to/nav.cnx ./build/connectome_swarm_probe 3600
 *
 * A healthy swarm moves, changes state, and tows rock. A swarm whose
 * brain is dead sits still with every NPC pinned in one state.
 */
#include "game_sim.h"
#include "signal_connectome_brain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *state_name(int s)
{
    switch (s) {
    case NPC_STATE_IDLE:               return "IDLE";
    case NPC_STATE_TRAVEL_TO_ASTEROID: return "TRAVEL_AST";
    case NPC_STATE_MINING:             return "MINING";
    case NPC_STATE_RETURN_TO_STATION:  return "RETURN";
    case NPC_STATE_DOCKED:             return "DOCKED";
    case NPC_STATE_TRAVEL_TO_DEST:     return "TRAVEL_DEST";
    case NPC_STATE_UNLOADING:          return "UNLOADING";
    default:                           return "?";
    }
}

static world_t g_world;

int main(int argc, char **argv)
{
    int ticks = (argc > 1) ? atoi(argv[1]) : 3600;
    if (ticks <= 0) ticks = 3600;

    /* Must run before world_reset so NPC spawn stamps the brain mode. */
    bool on = signal_connectome_init();

    world_t *w = &g_world;
    memset(w, 0, sizeof(*w));
    w->rng = 2037u;
    world_reset(w);

    printf("connectome brain: %s\n", on ? "ENABLED" : "disabled (baseline)");
    printf("running %d ticks at 120 Hz (%.1f s of sim)\n\n",
           ticks, (double)ticks / 120.0);

    vec2 prev[MAX_NPC_SHIPS];
    double travelled[MAX_NPC_SHIPS];
    int    transitions[MAX_NPC_SHIPS];
    int    last_state[MAX_NPC_SHIPS];
    int    tow_ticks[MAX_NPC_SHIPS];
    long   state_hist[8];
    memset(travelled, 0, sizeof(travelled));
    memset(transitions, 0, sizeof(transitions));
    memset(tow_ticks, 0, sizeof(tow_ticks));
    memset(state_hist, 0, sizeof(state_hist));

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *n = &w->npc_ships[i];
        prev[i] = (n->active && n->ship) ? n->ship->pos : (vec2){0.0f, 0.0f};
        last_state[i] = n->state;
    }

    for (int t = 0; t < ticks; t++) {
        world_sim_step(w, 1.0f / 120.0f);
        for (int i = 0; i < MAX_NPC_SHIPS; i++) {
            npc_ship_t *n = &w->npc_ships[i];
            if (!n->active || !n->ship) continue;
            float dx = n->ship->pos.x - prev[i].x;
            float dy = n->ship->pos.y - prev[i].y;
            travelled[i] += sqrt((double)(dx * dx + dy * dy));
            prev[i] = n->ship->pos;
            if (n->state != last_state[i]) { transitions[i]++; last_state[i] = n->state; }
            if ((int)n->state < 8) state_hist[(int)n->state]++;
            if (n->ship->towed_count > 0) tow_ticks[i]++;
        }
    }

    printf("%-5s %-8s %-12s %10s %8s %8s %7s\n",
           "slot", "role", "state", "travelled", "moves", "towing", "brain");
    int active = 0, moving = 0;
    double total_travel = 0.0;
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *n = &w->npc_ships[i];
        if (!n->active || !n->ship) continue;
        active++;
        total_travel += travelled[i];
        if (travelled[i] > 1.0) moving++;
        printf("%-5d %-8s %-12s %10.0f %8d %7.1f%% %7d\n",
               i,
               n->role == NPC_ROLE_MINER ? "miner"
                 : n->role == NPC_ROLE_HAULER ? "hauler" : "tow",
               state_name(n->state), travelled[i], transitions[i],
               100.0 * tow_ticks[i] / (double)ticks,
               (int)n->brain_mode);
    }

    printf("\nactive NPCs: %d, of which moved: %d\n", active, moving);
    printf("total distance flown: %.0f units\n", total_travel);
    printf("state occupancy: ");
    for (int s = 0; s < 8; s++)
        if (state_hist[s]) printf("%s=%ld ", state_name(s), state_hist[s]);
    printf("\n");

    signal_connectome_stats_t st;
    if (signal_connectome_stats(&st)) {
        printf("\nconnectome: %llu ticks, %llu kernel steps, %llu units spent\n",
               (unsigned long long)st.ticks, (unsigned long long)st.steps,
               (unsigned long long)st.units_spent);
        printf("            awake=%u sleeping=%u deep=%u  promotions=%u\n",
               st.active_flies, st.sleeping_flies, st.deep_flies, st.promotions);
        printf("            rented while asleep: %llu units\n",
               (unsigned long long)st.rented_units);
        printf("            checksum: %016llx\n",
               (unsigned long long)signal_connectome_checksum());
    }
    return (active > 0 && moving > 0) ? 0 : 1;
}
