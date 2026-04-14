/*
 * autopilot_eval.c — Standalone autopilot evaluation harness.
 *
 * Runs N autopilot ships for M seconds and reports per-ship and
 * fleet-level metrics. Not part of the game — build and run separately:
 *
 *   gcc -O2 -o autopilot_eval server/autopilot_eval.c server/game_sim.c \
 *       server/sim_autopilot.c server/sim_flight.c server/sim_ai.c \
 *       server/sim_nav.c server/sim_save.c server/sim_asteroid.c \
 *       server/sim_physics.c server/sim_production.c server/sim_construction.c \
 *       src/commodity.c src/ship.c src/economy.c src/asteroid.c src/rng.c \
 *       -Ishared -Isrc -Iserver -lm
 *
 *   ./autopilot_eval [num_ships] [seconds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "game_sim.h"
#include "sim_nav.h"
#include "sim_autopilot.h"

#define SIM_DT (1.0f / 120.0f)

typedef struct {
    float credits_start;
    float credits_end;
    int   cycles_completed;    /* transitions through SELL state */
    float time_in_state[8];   /* seconds per AUTOPILOT_STEP_* */
    float min_signal;
    float max_stuck_time;      /* longest time in one state without progress */
    int   stuck_events;        /* times stuck > 15s */
    bool  died;
    vec2  final_pos;
} ship_eval_t;

int main(int argc, char **argv) {
    int num_ships = (argc > 1) ? atoi(argv[1]) : 3;
    int sim_seconds = (argc > 2) ? atoi(argv[2]) : 300;
    if (num_ships < 1) num_ships = 1;
    if (num_ships > MAX_PLAYERS) num_ships = MAX_PLAYERS;

    printf("=== Autopilot Eval: %d ships, %d seconds ===\n\n", num_ships, sim_seconds);

    world_t *w = calloc(1, sizeof(world_t));
    world_reset(w);

    ship_eval_t evals[MAX_PLAYERS] = {0};
    int prev_state[MAX_PLAYERS] = {0};
    float state_timer[MAX_PLAYERS] = {0};

    for (int p = 0; p < num_ships; p++) {
        player_init_ship(&w->players[p], w);
        w->players[p].id = (uint8_t)p;
        w->players[p].connected = true;
        w->players[p].autopilot_mode = 1;
        w->players[p].autopilot_state = AUTOPILOT_STEP_FIND_TARGET;
        evals[p].credits_start = w->players[p].ship.stat_credits_earned;
        evals[p].min_signal = 1.0f;
        prev_state[p] = AUTOPILOT_STEP_FIND_TARGET;
    }

    int total_ticks = sim_seconds * 120;
    int report_interval = 30 * 120; /* report every 30s */

    for (int tick = 0; tick < total_ticks; tick++) {
        world_sim_step(w, SIM_DT);

        for (int p = 0; p < num_ships; p++) {
            server_player_t *sp = &w->players[p];
            int st = sp->autopilot_state;

            /* Track time in each state. */
            if (st >= 0 && st < 8)
                evals[p].time_in_state[st] += SIM_DT;

            /* Cycle detection: entering SELL means a cycle completed. */
            if (st == AUTOPILOT_STEP_SELL && prev_state[p] != AUTOPILOT_STEP_SELL)
                evals[p].cycles_completed++;

            /* Stuck detection. */
            if (st == prev_state[p]) {
                state_timer[p] += SIM_DT;
                if (state_timer[p] > evals[p].max_stuck_time)
                    evals[p].max_stuck_time = state_timer[p];
                if (state_timer[p] > 15.0f && (int)(state_timer[p] * 120) % 120 == 0)
                    evals[p].stuck_events++;
            } else {
                state_timer[p] = 0.0f;
            }
            prev_state[p] = st;

            /* Signal tracking. */
            float sig = signal_strength_at(w, sp->ship.pos);
            if (sig < evals[p].min_signal) evals[p].min_signal = sig;

            /* Death detection. */
            if (sp->ship.hull <= 0.0f) evals[p].died = true;
        }

        /* Periodic report. */
        if ((tick + 1) % report_interval == 0) {
            float t = (float)(tick + 1) / 120.0f;
            printf("--- t=%.0fs ---\n", t);
            for (int p = 0; p < num_ships; p++) {
                server_player_t *sp = &w->players[p];
                float earned = sp->ship.stat_credits_earned - evals[p].credits_start;
                printf("  P%d: state=%d credits=+%.0f cycles=%d towed=%d sig=%.2f pos=(%.0f,%.0f)\n",
                    p, sp->autopilot_state, earned, evals[p].cycles_completed,
                    sp->ship.towed_count, signal_strength_at(w, sp->ship.pos),
                    sp->ship.pos.x, sp->ship.pos.y);
            }
        }
    }

    /* Final report. */
    printf("\n=== FINAL RESULTS (%ds) ===\n", sim_seconds);
    const char *state_names[] = {
        "FIND", "FLY", "MINE", "COLLECT", "RETURN", "DOCK", "SELL", "LAUNCH"
    };
    int total_cycles = 0;
    float total_credits = 0;
    int ships_stuck = 0;
    int ships_dead = 0;

    for (int p = 0; p < num_ships; p++) {
        server_player_t *sp = &w->players[p];
        evals[p].credits_end = sp->ship.stat_credits_earned;
        evals[p].final_pos = sp->ship.pos;
        float earned = evals[p].credits_end - evals[p].credits_start;
        total_credits += earned;
        total_cycles += evals[p].cycles_completed;
        if (earned <= 0) ships_stuck++;
        if (evals[p].died) ships_dead++;

        printf("\nShip %d:\n", p);
        printf("  Credits: %.0f -> %.0f (+%.0f)\n",
            evals[p].credits_start, evals[p].credits_end, earned);
        printf("  Cycles completed: %d\n", evals[p].cycles_completed);
        printf("  Time breakdown:\n");
        for (int s = 0; s < 8; s++) {
            if (evals[p].time_in_state[s] > 0.1f)
                printf("    %-8s %5.1fs (%4.1f%%)\n", state_names[s],
                    evals[p].time_in_state[s],
                    evals[p].time_in_state[s] / (float)sim_seconds * 100.0f);
        }
        printf("  Min signal: %.2f\n", evals[p].min_signal);
        printf("  Max stuck: %.1fs  Stuck events: %d\n",
            evals[p].max_stuck_time, evals[p].stuck_events);
        printf("  Died: %s\n", evals[p].died ? "YES" : "no");
        printf("  Final pos: (%.0f, %.0f)\n", evals[p].final_pos.x, evals[p].final_pos.y);
    }

    printf("\n=== FLEET SUMMARY ===\n");
    printf("  Ships: %d  Stuck: %d  Dead: %d\n", num_ships, ships_stuck, ships_dead);
    printf("  Total cycles: %d  Total credits: +%.0f\n", total_cycles, total_credits);
    printf("  Avg credits/ship: %.0f\n", total_credits / num_ships);
    printf("  Avg cycles/ship: %.1f\n", (float)total_cycles / num_ships);

    /* Grade. */
    float health = (float)(num_ships - ships_stuck) / (float)num_ships;
    printf("\n  HEALTH: %.0f%% (%d/%d ships productive)\n",
        health * 100.0f, num_ships - ships_stuck, num_ships);
    if (health >= 0.8f && total_cycles >= num_ships)
        printf("  GRADE: PASS\n");
    else
        printf("  GRADE: FAIL\n");

    free(w);
    return (health >= 0.8f) ? 0 : 1;
}
