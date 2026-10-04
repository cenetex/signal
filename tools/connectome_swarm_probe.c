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
#include "signal_npc_worker_brain.h"
#include "signal_intelligence.h"
#include "holographic_nn_backend.h"
#include "connectome_probe_metrics.h"
#include "chain_log.h"
#include <inttypes.h>

#include <errno.h>
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

static bool probe_chain_counts(const world_t *w, uint64_t *counts)
{
    memset(counts, 0, sizeof(uint64_t) * CHAIN_EVT_TYPE_COUNT);
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &w->stations[i];
        if (st->chain_event_count == 0) continue;
        chain_log_verify_report_t report = {0};
        if (!chain_log_verify_station(st, NULL, NULL, &report) ||
            report.valid_events != st->chain_event_count) return false;
        for (int k = 0; k < CHAIN_EVT_TYPE_COUNT; k++)
            counts[k] += report.event_type_counts[k];
    }
    return true;
}

static bool probe_uint(const char *s, uint32_t max, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(s, &end, 10);
    if (!s[0] || s[0] == '-' || errno || *end || !value || value > max) return false;
    *out = (uint32_t)value;
    return true;
}

int main(int argc, char **argv)
{
    uint32_t ticks = 3600, seed = 2037;
    const char *json_path = NULL, *chain_dir = NULL;
    if (argc > 1 && !probe_uint(argv[1], 1000000, &ticks)) return 2;
    for (int arg = 2; arg < argc; arg += 2) {
        if (arg + 1 >= argc) return 2;
        if (!strcmp(argv[arg], "--seed")) {
            if (!probe_uint(argv[arg + 1], UINT32_MAX, &seed)) return 2;
        } else if (!strcmp(argv[arg], "--json")) json_path = argv[arg + 1];
        else if (!strcmp(argv[arg], "--chain-dir")) chain_dir = argv[arg + 1];
        else return 2;
    }
    /* Study runs own a fresh history directory, with the same genesis as
     * a fresh server. The runner provides a separate directory per run. */
    if (json_path && !chain_dir) return 2;
    if (chain_dir) {
        chain_log_set_dir(chain_dir);
        chain_log_set_disk_enabled(true);
    }

    /* Must run before world_reset so NPC spawn stamps the brain mode. */
    bool on = signal_connectome_init();

    /* Optional: load the trained worker model so the station policy can be
     * biased by it (SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT). */
    {
        const char *ckpt = getenv("SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT");
        if (ckpt && ckpt[0]) {
            char err[256];
            if (signal_npc_worker_brain_load_checkpoint(ckpt, err, sizeof(err)))
                printf("worker model: loaded %s\n", ckpt);
            else
                fprintf(stderr, "worker model: load failed: %s\n", err);
        }
    }

    world_t *w = &g_world;
    memset(w, 0, sizeof(*w));
    w->rng = seed;
    world_reset(w);
    uint64_t initial_chain[CHAIN_EVT_TYPE_COUNT] = {0};
    connectome_probe_metrics_t metrics = {0};
    if (json_path) {
        if (!on) { fprintf(stderr, "study requires a loaded connectome\n"); return 2; }
        world_seed_station_chain_genesis(w);
        for (int i = 0; i < w->station_count && i < SIGNAL_ROOT_STATION_COUNT; i++)
            if (!w->stations[i].chain_event_count) return 3;
        if (!probe_chain_counts(w, initial_chain)) return 3;
        connectome_probe_sample(&metrics, w, true);
    }
    /* Match the server's independent worker checkpoint loading path. */
    signal_intelligence_holographic_init();
    const char *worker_path = getenv("SIGNAL_BOT_NPC_WORKER_BRAIN_CHECKPOINT");
    if (worker_path && worker_path[0]) {
        char error[512] = {0};
        if (!signal_intelligence_load_npc_worker_checkpoint(worker_path, error, sizeof(error))) {
            fprintf(stderr, "worker checkpoint load failed: %s\n", error);
            return 2;
        }
    }
    uint32_t initial_rng = w->rng;

    printf("connectome brain: %s\n", on ? "ENABLED" : "disabled (baseline)");
    printf("running %u ticks at 120 Hz (%.1f s of sim)\n\n",
           ticks, (double)ticks / 120.0);

    vec2 prev[MAX_NPC_SHIPS];
    double travelled[MAX_NPC_SHIPS];
    int    transitions[MAX_NPC_SHIPS];
    npc_state_t last_state[MAX_NPC_SHIPS];
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

    for (uint32_t t = 0; t < ticks; t++) {
        world_sim_step(w, 1.0f / 120.0f);
        if (json_path) connectome_probe_sample(&metrics, w, false);
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
        if (st.strategy_changes) {
            static const char *names[SIGNAL_CONNECTOME_STRATEGY_COUNT] =
                { "forage", "prospect", "caution", "haul", "regroup" };
            printf("            strategy re-samples=%u  postures:",
                   st.strategy_changes);
            if (st.strategy_model_scores)
                printf(" (model-biased=%u)", st.strategy_model_scores);
            for (int k = 0; k < SIGNAL_CONNECTOME_STRATEGY_COUNT; k++)
                printf(" %s=%u", names[k], st.strategy_counts[k]);
            printf("\n");
        }
        printf("            checksum: %016llx\n",
               (unsigned long long)signal_connectome_checksum());
    }
    if (json_path) {
        uint64_t counts[CHAIN_EVT_TYPE_COUNT] = {0};
        if (!probe_chain_counts(w, counts)) return 3;
        for (int k = 0; k < CHAIN_EVT_TYPE_COUNT; k++) {
            if (counts[k] < initial_chain[k]) return 3;
            counts[k] -= initial_chain[k];
        }
        FILE *out = fopen(json_path, "w");
        if (!out) return 3;
        fprintf(out, "{\n  \"schema\": 1, \"seed\": %u, \"ticks\": %u, "
                     "\"strategy\": %s, \"initial_rng\": %u,\n",
                seed, ticks, signal_connectome_strategy_enabled() ? "true" : "false", initial_rng);
        fprintf(out, "  \"chain_verified\": true, \"smelt_output_units\": %" PRIu64
                     ", \"craft_events\": %" PRIu64 ", \"construction_contributions\": %" PRIu64 ",\n",
                counts[CHAIN_EVT_SMELT], counts[CHAIN_EVT_CRAFT], counts[CHAIN_EVT_CONSTRUCTION]);
        fprintf(out, "  \"delivered_units\": %" PRIu64 ", \"destroyed_ships\": %" PRIu64
                     ", \"contract_completions\": %" PRIu64 ",\n",
                metrics.delivered_units, metrics.destroyed_ships, metrics.contract_completions);
        fprintf(out, "  \"distance\": %.9f, \"observed_hull_loss\": %.9f,\n",
                metrics.distance, metrics.observed_hull_loss);
        fprintf(out, "  \"active_ticks\": %" PRIu64 ", \"travel_ticks\": %" PRIu64
                     ", \"docked_ticks\": %" PRIu64 ", \"idle_ticks\": %" PRIu64
                     ", \"towing_ticks\": %" PRIu64 ", \"event_capacity_ticks\": %" PRIu64 ",\n",
                metrics.active_ticks, metrics.travel_ticks, metrics.docked_ticks,
                metrics.idle_ticks, metrics.towing_ticks, metrics.event_capacity_ticks);
        fprintf(out, "  \"brains\": {\"fast_neurons\": %u, \"deep_neurons\": %u, "
                     "\"connectome_steps\": %" PRIu64 ", \"deep_promotions\": %u, "
                     "\"hnn_backend\": \"%s\", \"flight_builtin_available\": %s, "
                     "\"flight_inferences\": %" PRIu64 ", \"contract_loaded\": %s, "
                     "\"contract_inferences\": %" PRIu64 ", \"worker_loaded\": %s, "
                     "\"worker_inferences\": %" PRIu64 ", \"worker_decisions\": %" PRIu64 ", "
                     "\"worker_teacher_decisions\": %" PRIu64 "},\n",
                st.fast_neurons, st.deep_neurons, st.steps, st.promotions,
                hnn_backend_kind_name(hnn_backend_active_kind()),
                signal_intelligence_flight_builtin_available() ? "true" : "false",
                signal_intelligence_flight_inference_count(),
                signal_intelligence_contract_loaded() ? "true" : "false",
                signal_intelligence_contract_inference_count(),
                signal_intelligence_npc_worker_loaded() ? "true" : "false",
                signal_intelligence_npc_worker_inference_count(),
                signal_intelligence_npc_worker_decision_count(),
                signal_intelligence_npc_worker_teacher_decision_count());
        fprintf(out, "  \"active_at_end\": %d, \"strategy_changes\": %u, "
                     "\"connectome_checksum\": \"%016" PRIx64 "\"\n}\n",
                active, st.strategy_changes, signal_connectome_checksum());
        if (fclose(out)) return 3;
    }
    return json_path ? 0 : ((active > 0 && moving > 0) ? 0 : 1);
}
