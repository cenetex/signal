/*
 * signal_connectome_brain.c -- adapter between signal's NPC flight loop
 * and the vendored fly connectome kernel (server/connectome/).
 *
 * Data flow per 120 Hz tick, in order:
 *
 *   1. The NPC state machines steer as usual (npc_steer_with_path).
 *      CONNECTOME NPCs call signal_connectome_flight_cmd(), which reads
 *      the descending-bus command their brain computed last tick,
 *      applies it as a bounded, clearance-scaled BIAS on the reflex
 *      controller (see flight_cmd for why the brain is not allowed to
 *      steer outright), gates thrust on arousal, and records the reflex
 *      turn as the sensory drive for this tick's injection.
 *   2. At the end of step_npc_ships, signal_connectome_tick() updates
 *      each fly's drives (hunger/lust/fear/pain) from world state, sets
 *      its economic stake, injects tonic + steering + fear drive into
 *      the shared circuit, and spends the tick's brain budget through
 *      flyswarm (stake-proportional; docked flies sleep at stake 0 and
 *      rent their share to the swarm).
 *
 * The one-tick latency between injection and readout is not a hack; it
 * is the synapse. Everything here is integer in the kernel and float
 * only where the rest of the sim is already float.
 *
 * Env (read once on first init):
 *   SIGNAL_CONNECTOME_FAST        path, required to enable (nav .cnx)
 *   SIGNAL_CONNECTOME_DEEP        path, optional (full-brain .cnx)
 *   SIGNAL_CONNECTOME_DT_US       brain step, default 4000 (0.48x realtime)
 *   SIGNAL_CONNECTOME_TONIC_UV    columnar arousal, default 2200 (must
 *                                 clear the ignition cliff at this dt)
 *   SIGNAL_CONNECTOME_STEER_UV    ring steering amplitude, default 2500
 *   SIGNAL_CONNECTOME_NOISE_UV    per-step membrane jitter, default 0
 *   SIGNAL_CONNECTOME_TURN_GAIN_X100  brain steering bias ceiling,
 *                                 default 35 (=0.35 of the turn axis)
 *   SIGNAL_CONNECTOME_BUDGET      kernel-step units per tick, default 120
 *                                 (one fast step = 1 unit, deep = 25)
 *   SIGNAL_CONNECTOME_DEEP_SLOTS  scarce deliberation slots, default 3
 *   SIGNAL_CONNECTOME_PROMOTE_STAKE  default 400
 *   SIGNAL_CONNECTOME_DEMOTE_STAKE  default 200
 *   SIGNAL_CONNECTOME_AWAKE_HZ_X100  descending-drive gate, default 10 (=0.10 Hz)
 *   SIGNAL_CONNECTOME_SEED        default 42
 */
#include "signal_connectome_brain.h"
#include "signal_connectome_drives.h"
#include "connectome/flybrain.h"
#include "connectome/flyswarm.h"
#include "sim_nav.h"
#include "signal_npc_worker_brain.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CB_MAX_AGENTS MAX_NPC_SHIPS

typedef struct {
    int      enabled;
    int      init_attempted;

    fb_circuit fast;
    int      has_fast;
    fb_circuit deep;
    int      has_deep;
    fb_swarm  swarm;

    /* config */
    int32_t  dt_us;
    int32_t  tonic_uv;
    int32_t  steer_uv;
    int32_t  noise_uv;
    uint32_t budget;
    int32_t  turn_gain;   /* Q16 ceiling on the brain's steering bias */
    uint32_t debug_every; /* ticks between diagnostic dumps, 0 = off */
    uint32_t deep_slots;
    uint32_t promote_stake;
    uint32_t demote_stake;
    int32_t  awake_hz_q16;
    uint64_t seed;

    /* Strategic policy (hybrid brain): each station is the planner and
     * broadcasts one posture to the flies that call it home, attenuated
     * by the signal those flies can actually hear. A station learns which
     * posture pays off (station_value). Runtime-only. */
    uint32_t strategy_period;  /* ticks between re-samples, 0 = disabled */
    uint32_t strategy_model_weight; /* 0..100: model bias ceiling, 0 = ignore model */
    uint8_t  station_strategy[MAX_STATIONS];
    uint32_t station_ttl[MAX_STATIONS];
    uint8_t  station_has_strategy[MAX_STATIONS];
    uint32_t station_reward[MAX_STATIONS];                   /* run reward */
    uint32_t station_value[MAX_STATIONS][CB_STRAT_COUNT];    /* learned value */

    /* cached populations of the fast circuit, for injection */
    const uint32_t *ring_idx;   uint32_t ring_n;
    const uint32_t *col_idx;    uint32_t col_n;

    /* injection scratch, sized to the fast circuit's populations */
    int32_t *col_scratch;
    int32_t *ring_scratch;

    /* per-agent drives */
    cb_agent_state_t agent[CB_MAX_AGENTS];

    /* stats */
    signal_connectome_stats_t stats;
} cb_state_t;

static cb_state_t g_cb;

/* ---------------- small helpers ---------------- */

static int32_t cb_env_int(const char *name, int32_t fallback)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return fallback;
    char *end = NULL;
    long n = strtol(v, &end, 0);
    if (end == v) return fallback;
    return (int32_t)n;
}

static int32_t cb_q16_from_x100(int32_t x100)
{
    if (x100 <= 0) return 0;
    return (int32_t)(((int64_t)x100 * Q16_ONE) / 100);
}

static float cb_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---------------- init ---------------- */

bool signal_connectome_enabled(void)
{
    return g_cb.enabled != 0;
}

bool signal_connectome_strategy_requested(void)
{
    const char *v = getenv("SIGNAL_CONNECTOME_STRATEGY");
    return v && v[0] && v[0] != '0';
}

bool signal_connectome_strategy_enabled(void)
{
    return g_cb.enabled && signal_connectome_strategy_requested();
}

bool signal_connectome_init(void)
{
    if (g_cb.init_attempted) return g_cb.enabled != 0;

    const char *fast_path = getenv("SIGNAL_CONNECTOME_FAST");
    if (!fast_path || !fast_path[0]) {
        g_cb.init_attempted = 1;   /* not configured: mode stays off */
        return false;
    }

    memset(&g_cb, 0, sizeof(g_cb));
    g_cb.init_attempted = 1;

    cb_state_t *cb = &g_cb;
    cb->dt_us         = cb_env_int("SIGNAL_CONNECTOME_DT_US", 4000);
    /* Tonic must clear the circuit's ignition threshold AT THIS dt, and
     * that threshold is a cliff, not a ramp. A neuron driven with a
     * constant uv per step settles at uv/(1 - exp(-dt/tau)); at the
     * default dt=4000 that is uv/0.1813, so the old 900 uV default
     * settled at 4964 uV against a 7000 uV threshold and NOTHING in the
     * circuit ever fired. Every connectome NPC then read drive=0, failed
     * the arousal gate below, and sat motionless with thrust clamped to
     * zero. Measured on nav.cnx: silent below ~1600 uV, ~28-31 Hz from
     * 2200 uV up. Noise defaults off because the steering asymmetry it
     * has to compete with is only ~0.29 wide (see flight_cmd). */
    cb->tonic_uv      = cb_env_int("SIGNAL_CONNECTOME_TONIC_UV", 2200);
    cb->steer_uv      = cb_env_int("SIGNAL_CONNECTOME_STEER_UV", 2500);
    cb->noise_uv      = cb_env_int("SIGNAL_CONNECTOME_NOISE_UV", 0);
    cb->turn_gain     = cb_q16_from_x100(
        cb_env_int("SIGNAL_CONNECTOME_TURN_GAIN_X100", 35));
    cb->budget        = (uint32_t)cb_env_int("SIGNAL_CONNECTOME_BUDGET", 120);
    cb->deep_slots    = (uint32_t)cb_env_int("SIGNAL_CONNECTOME_DEEP_SLOTS", 3);
    cb->promote_stake = (uint32_t)cb_env_int("SIGNAL_CONNECTOME_PROMOTE_STAKE", 400);
    cb->demote_stake  = (uint32_t)cb_env_int("SIGNAL_CONNECTOME_DEMOTE_STAKE", 200);
    cb->awake_hz_q16  = cb_q16_from_x100(
        cb_env_int("SIGNAL_CONNECTOME_AWAKE_HZ_X100", 10));
    cb->seed          = (uint64_t)cb_env_int("SIGNAL_CONNECTOME_SEED", 42);
    cb->debug_every   = (uint32_t)cb_env_int("SIGNAL_CONNECTOME_DEBUG", 0);
    if (signal_connectome_strategy_requested()) {
        int32_t period = cb_env_int("SIGNAL_CONNECTOME_STRATEGY_PERIOD", 300);
        cb->strategy_period = period > 0 ? (uint32_t)period : 300u;
        int32_t mw = cb_env_int("SIGNAL_CONNECTOME_STRATEGY_MODEL_WEIGHT", 50);
        if (mw < 0) mw = 0;
        if (mw > 100) mw = 100;
        cb->strategy_model_weight = (uint32_t)mw;
    }

    if (fb_circuit_load(&cb->fast, fast_path) != 0) {
        fprintf(stderr, "[connectome] [FATAL] cannot load "
                        "SIGNAL_CONNECTOME_FAST=%s\n", fast_path);
        return false;
    }
    cb->has_fast = 1;

    const char *deep_path = getenv("SIGNAL_CONNECTOME_DEEP");
    if (deep_path && deep_path[0]) {
        if (fb_circuit_load(&cb->deep, deep_path) != 0) {
            fprintf(stderr, "[connectome] [FATAL] cannot load "
                            "SIGNAL_CONNECTOME_DEEP=%s\n", deep_path);
            fb_circuit_free(&cb->fast);
            return false;
        }
        cb->has_deep = 1;
    }

    if (fb_swarm_init(&cb->swarm, &cb->fast, cb->has_deep ? &cb->deep : NULL,
                      CB_MAX_AGENTS, cb->dt_us, cb->seed) != 0) {
        fprintf(stderr, "[connectome] [FATAL] swarm init failed\n");
        fb_circuit_free(&cb->fast);
        if (cb->has_deep) fb_circuit_free(&cb->deep);
        return false;
    }

    /* Brain-time is rented, not granted: no reflex floor. Every unit a
     * fly gets comes from its own stake or from a sleeping fly's share. */
    cb->swarm.floor_units   = 0;
    cb->swarm.budget_units  = cb->budget;
    cb->swarm.deep_slots    = cb->deep_slots;
    cb->swarm.promote_stake = cb->promote_stake;
    cb->swarm.demote_stake  = cb->demote_stake;

    cb->swarm.sim_fast.noise_uv = (uint32_t)cb->noise_uv;
    if (cb->has_deep)
        cb->swarm.sim_deep.noise_uv = (uint32_t)cb->noise_uv;

    /* Cache ring/columnar geometry of the fast circuit for injection. */
    cb->ring_n = fb_population(&cb->fast, "ring", &cb->ring_idx);
    cb->col_n  = fb_population(&cb->fast, "columnar", &cb->col_idx);
    cb->col_scratch  = (int32_t *)malloc(sizeof(int32_t) *
                                         (cb->col_n ? cb->col_n : 1));
    cb->ring_scratch = (int32_t *)malloc(sizeof(int32_t) *
                                         (cb->ring_n ? cb->ring_n : 1));
    if (!cb->col_scratch || !cb->ring_scratch) {
        fprintf(stderr, "[connectome] [FATAL] scratch alloc failed\n");
        signal_connectome_shutdown();
        return false;
    }

    /* Calibrate the steering axis on both circuits. The descending bus
     * has a fixed anatomical left/right bias; calibration maps it onto
     * [-1,1] so the game reads a command, not an artifact. */
    int settle_steps = (int)(600000 / (cb->dt_us > 0 ? cb->dt_us : 4000));
    if (settle_steps < 50) settle_steps = 50;
    int32_t span_fast = fb_sim_calibrate(&cb->swarm.sim_fast,
                                         cb->tonic_uv, cb->steer_uv,
                                         settle_steps);
    int32_t span_deep = 0;
    if (cb->has_deep)
        span_deep = fb_sim_calibrate(&cb->swarm.sim_deep,
                                     cb->tonic_uv, cb->steer_uv,
                                     settle_steps);

    cb->enabled = 1;
    cb->stats.fast_neurons = cb->fast.n;
    cb->stats.deep_neurons = cb->has_deep ? cb->deep.n : 0;
    cb->stats.has_deep = cb->has_deep;

    printf("[connectome] fly brain online: fast=%u neurons "
           "(%u in / %u out)\n",
           cb->fast.n, cb->fast.n_in, cb->fast.n_out);
    printf("[connectome] steering calibration: fast half-span=%.4f, "
           "deep half-span=%.4f (Q16 units)\n",
           span_fast / 65536.0, span_deep / 65536.0);
    /* A zero half-span means the circuit never ignited at this tonic/dt
     * pair: the brain is loaded but dead, and every NPC will fail the
     * arousal gate. Say so loudly rather than shipping motionless NPCs. */
    if (span_fast == 0)
        fprintf(stderr, "[connectome] [WARN] fast circuit measured a zero "
                        "steering span -- it is almost certainly silent at "
                        "tonic=%d uv / dt=%d us. Raise "
                        "SIGNAL_CONNECTOME_TONIC_UV.\n",
                cb->tonic_uv, cb->dt_us);
    if (cb->has_deep)
        printf("[connectome] deep circuit: %u neurons\n", cb->deep.n);
    printf("[connectome] budget %u units/tick, %u deep slot(s), "
           "promote@%u demote@%u, dt=%d us, awake gate=%.2f Hz\n",
           cb->budget, cb->deep_slots,
           cb->promote_stake, cb->demote_stake, cb->dt_us,
           cb->awake_hz_q16 / 65536.0);
    if (signal_connectome_strategy_requested())
        printf("[connectome] combined brain: connectome flight + strategic "
               "planner (period %u ticks, model weight %u%%)\n",
               cb->strategy_period, cb->strategy_model_weight);
    return true;
}

void signal_connectome_shutdown(void)
{
    if (!g_cb.has_fast) { memset(&g_cb, 0, sizeof(g_cb)); return; }
    fb_swarm_free(&g_cb.swarm);
    fb_circuit_free(&g_cb.fast);
    if (g_cb.has_deep) fb_circuit_free(&g_cb.deep);
    free(g_cb.col_scratch);
    free(g_cb.ring_scratch);
    memset(&g_cb, 0, sizeof(g_cb));
}

/* ---------------- drives ---------------- */

/* Largest closing-rock threat nearby, plus low-hull dread. Rocks are
 * signal's only weapon, so a rock on a closing course is the only
 * thing in the world with a reason to be feared. */
static int32_t cb_compute_fear(const world_t *w, const npc_ship_t *npc)
{
    const ship_t *s = npc->ship;
    int32_t fear = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &w->asteroids[i];
        if (!a->active) continue;
        /* Only moving rocks threaten. thrown_timer_q marks the
         * deliberately released ones -- a weapon should scare more
         * than drift, so it gets the wider radius. */
        float vx = a->vel.x - s->vel.x;
        float vy = a->vel.y - s->vel.y;
        float rel_sq = vx * vx + vy * vy;
        if (rel_sq < 400.0f) continue;          /* < 20 u/s relative */
        float rx = a->pos.x - s->pos.x;
        float ry = a->pos.y - s->pos.y;
        float r2 = rx * rx + ry * ry;
        float radius = (a->thrown_timer_q > 0) ? 900.0f : 500.0f;
        if (r2 > radius * radius) continue;
        float closing = -(rx * vx + ry * vy);   /* >0 = approaching */
        if (closing <= 0.0f) continue;
        float d = v2_len(v2(rx, ry));
        float prox = 1.0f - d / radius;          /* 0..1 */
        float menace = cb_clampf(v2_len(v2(vx, vy)) / 250.0f, 0.0f, 1.0f);
        int32_t f = (int32_t)(prox * menace * (float)Q16_ONE);
        if (f > fear) fear = f;
    }
    float max_hull = npc_max_hull(npc);
    if (max_hull > 0.0f && s->hull < max_hull * 0.25f) {
        int32_t dread = Q16_ONE / 2;
        if (dread > fear) fear = dread;
    }
    return fear;
}

/* ---- strategic policy (hybrid brain) ---------------------------------- */

int signal_connectome_weighted_pick(const uint32_t *weights, int count,
                                    uint64_t *rng)
{
    if (!weights || !rng || count <= 0) return -1;
    uint64_t total = 0;
    for (int i = 0; i < count; i++) total += weights[i];
    if (total == 0) return -1;
    uint64_t x = *rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *rng = x;
    uint64_t pick = x % total;
    for (int i = 0; i < count; i++) {
        if (pick < weights[i]) return i;
        pick -= weights[i];
    }
    return count - 1;
}

typedef struct {
    uint64_t hunger, lust, fear;  /* summed Q16 drives of the station's awake flies */
    uint32_t count;
    uint32_t productive;          /* flies mining or towing this tick */
} cb_station_agg_t;

#define CB_BANDIT_MAX 4096u

void signal_connectome_bandit_reward(uint32_t *values, int count, int arm,
                                     uint32_t reward)
{
    if (!values || count <= 0 || arm < 0 || arm >= count) return;
    uint64_t v = (uint64_t)values[arm] + reward;
    values[arm] = v > CB_BANDIT_MAX ? CB_BANDIT_MAX : (uint32_t)v;
}

void signal_connectome_bandit_decay(uint32_t *values, int count)
{
    if (!values || count <= 0) return;
    for (int i = 0; i < count; i++)
        values[i] -= values[i] >> 4;   /* ~6% per window */
}

#define CB_MODEL_BIAS_MAX 16u

void signal_connectome_model_bias(const double *scores, int count,
                                  uint32_t weight_pct, uint32_t *bias)
{
    if (!scores || !bias || count <= 0) return;
    for (int i = 0; i < count; i++) bias[i] = 0;
    if (weight_pct == 0) return;
    if (weight_pct > 100) weight_pct = 100;

    double lo = scores[0], hi = scores[0], second = -1.0e300;
    for (int i = 1; i < count; i++) {
        if (scores[i] > hi) { second = hi; hi = scores[i]; }
        else if (scores[i] > second) second = scores[i];
        if (scores[i] < lo) lo = scores[i];
    }
    double span = hi - lo;
    if (span <= 0.0) return;

    /* Taper by how separated the best is from the runner-up: a model that
     * cannot rank confidently should not outvote the drive weights. */
    double conf = (hi - second) / span;
    if (conf < 0.0) conf = 0.0;
    if (conf > 1.0) conf = 1.0;
    conf = 0.25 + 0.75 * conf;

    uint32_t ceiling = (CB_MODEL_BIAS_MAX * weight_pct) / 100u;
    for (int i = 0; i < count; i++) {
        double t = (scores[i] - lo) / span;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        bias[i] = (uint32_t)(t * (double)ceiling * conf);
    }
}

static uint64_t cb_station_seed(int station, uint32_t tick)
{
    uint64_t h = 1469598103934665603ULL;
    h ^= (uint64_t)(station + 1); h *= 1099511628211ULL;
    h ^= tick;                    h *= 1099511628211ULL;
    return h ? h : 1u;
}

/* Posture weights from the station's own swarm -- what its flies, on
 * average, are feeling -- plus what the station has learned is working.
 * Every posture keeps a floor so exploration never dies. */
/* ---- station policy: worker-model bridge ------------------------------ */

/* A posture maps onto the worker option the runtime model was trained to
 * rank. The model's learned option prior then becomes the station's posture
 * prior; the drive weights and the bandit still act on top of it. */
typedef struct {
    signal_npc_worker_option_t option;
    npc_role_t role;
    bool travel;
    bool mine;
    bool frontier_supply;
    bool escort;
    bool patrol;
    bool risky_profit;
} cb_posture_map_t;

static const cb_posture_map_t CB_POSTURE_MAP[CB_STRAT_COUNT] = {
    { SIGNAL_NPC_WORKER_OPTION_MINE_HOME,         NPC_ROLE_MINER,  false, true,  false, false, false, false },
    { SIGNAL_NPC_WORKER_OPTION_TAKE_RISKY_PROFIT, NPC_ROLE_HAULER, true,  false, false, false, false, true  },
    { SIGNAL_NPC_WORKER_OPTION_PATROL_ROUTE,      NPC_ROLE_HAULER, true,  false, false, false, true,  false },
    { SIGNAL_NPC_WORKER_OPTION_HAUL_CONTRACT,     NPC_ROLE_HAULER, true,  false, false, false, false, false },
    { SIGNAL_NPC_WORKER_OPTION_WAIT,              NPC_ROLE_MINER,  false, false, false, false, false, false },
};

/* Ask the loaded worker model to rank the postures and return its score
 * spread as posture bias in [0, 32]. Returns false when no model is loaded,
 * so an unloaded server keeps exactly the drive+bandit weights. */
static bool cb_station_model_bias(int station, const cb_station_agg_t *a,
                                  uint32_t *bias)
{
    if (!signal_npc_worker_brain_loaded()) return false;
    if (g_cb.strategy_model_weight == 0u) return false;   /* model explicitly off */
    uint32_t n = a->count ? a->count : 1u;
    float hunger = (float)((a->hunger / n) >> 12) / 16.0f;
    float fear   = (float)((a->fear / n) >> 12) / 16.0f;

    signal_npc_worker_candidate_t c[CB_STRAT_COUNT];
    double scores[CB_STRAT_COUNT] = {0.0};
    for (int p = 0; p < CB_STRAT_COUNT; p++) {
        const cb_posture_map_t *m = &CB_POSTURE_MAP[p];
        memset(&c[p], 0, sizeof(c[p]));
        c[p].option = m->option;
        c[p].role = m->role;
        c[p].home_station = station;
        c[p].legal = true;
        c[p].travel = m->travel;
        c[p].mine_pressure = m->mine || hunger > 0.5f;
        c[p].frontier_supply = m->frontier_supply;
        c[p].escort = m->escort;
        c[p].patrol = m->patrol;
        c[p].risky_profit = m->risky_profit;
        c[p].frontier_pressure = fear;
        c[p].route_danger_memory = fear;
        c[p].best_contract_dest = -1;
        c[p].persona_risk = 0.5f;
        c[p].persona_growth = 0.5f;
        c[p].persona_patience = 0.5f;
    }
    (void)signal_npc_worker_brain_choose_with_scores(
        c, CB_STRAT_COUNT, scores, CB_STRAT_COUNT);

    signal_connectome_model_bias(scores, CB_STRAT_COUNT,
                                 g_cb.strategy_model_weight, bias);
    return true;
}

static void cb_station_weights(const cb_station_agg_t *a,
                               const uint32_t *value, uint32_t *w)
{
    uint32_t n = a->count ? a->count : 1u;
    uint32_t hunger = (uint32_t)((a->hunger / n) >> 12);  /* 0..16 */
    uint32_t lust   = (uint32_t)((a->lust / n) >> 12);
    uint32_t fear   = (uint32_t)((a->fear / n) >> 12);
    uint32_t learned[CB_STRAT_COUNT];
    for (int k = 0; k < CB_STRAT_COUNT; k++)
        learned[k] = value ? value[k] >> 3 : 0u;   /* scale so the floor still matters */
    w[CB_STRAT_FORAGE]   = 8u + hunger + learned[CB_STRAT_FORAGE];
    w[CB_STRAT_PROSPECT] = 4u + hunger + learned[CB_STRAT_PROSPECT];
    w[CB_STRAT_CAUTION]  = 4u + fear * 2u + learned[CB_STRAT_CAUTION];
    w[CB_STRAT_HAUL]     = 4u + lust * 2u + learned[CB_STRAT_HAUL];
    w[CB_STRAT_REGROUP]  = 4u + learned[CB_STRAT_REGROUP];
}

/* Each station is the planner. It samples one posture for its own swarm
 * and holds it for strategy_period ticks; different stations diverge. */
static void cb_station_strategy_advance(const world_t *w,
                                        const cb_station_agg_t *agg)
{
    cb_state_t *cb = &g_cb;
    for (int s = 0; s < MAX_STATIONS; s++) {
        if (agg[s].count == 0) { cb->station_ttl[s] = 0; continue; }
        /* Reward the posture by how many of the station's flies it kept
         * productive this window. */
        cb->station_reward[s] += agg[s].productive;
        if (cb->station_ttl[s] == 0) {
            if (cb->station_has_strategy[s]) {
                signal_connectome_bandit_reward(
                    cb->station_value[s], CB_STRAT_COUNT,
                    (int)cb->station_strategy[s], cb->station_reward[s]);
                signal_connectome_bandit_decay(
                    cb->station_value[s], CB_STRAT_COUNT);
            }
            uint32_t weights[CB_STRAT_COUNT];
            uint32_t bias[CB_STRAT_COUNT] = {0};
            uint64_t rng = cb_station_seed(s, w->tick);
            cb_station_weights(&agg[s], cb->station_value[s], weights);
            if (cb_station_model_bias(s, &agg[s], bias)) {
                for (int k = 0; k < CB_STRAT_COUNT; k++)
                    weights[k] += bias[k];
                cb->stats.strategy_model_scores++;
            }
            int pick = signal_connectome_weighted_pick(weights, CB_STRAT_COUNT, &rng);
            cb->station_strategy[s] = (uint8_t)(pick < 0 ? CB_STRAT_FORAGE : pick);
            cb->station_has_strategy[s] = 1;
            cb->station_ttl[s] = cb->strategy_period;
            cb->station_reward[s] = 0;
            cb->stats.strategy_changes++;
        } else {
            cb->station_ttl[s]--;
        }
        cb->stats.strategy_counts[cb->station_strategy[s]] += agg[s].count;
    }
}

/* Update one fly's drives from world state. Q16 throughout. */
static void cb_update_drives(const world_t *w, npc_ship_t *npc, int i)
{
    cb_agent_state_t *st = &g_cb.agent[i];
    const ship_t *s = npc->ship;

    int carrying = (int)s->towed_count + (int)s->towed_pod_count +
                   (s->towed_scaffold >= 0 ? 1 : 0);

    cb_update_cargo_drives(st, carrying, npc->state == NPC_STATE_DOCKED);

    /* PAIN: hull drop since last tick spikes, then decays (~1 s). */
    if (st->last_hull > 0.0f && s->hull < st->last_hull - 0.5f)
        st->pain = Q16_ONE;
    st->last_hull = s->hull;
    st->pain -= st->pain >> 6;

    /* FEAR: recomputed from the world each tick. */
    st->fear = cb_compute_fear(w, npc);
}

static uint32_t cb_compute_stake(const npc_ship_t *npc, int i)
{
    const cb_agent_state_t *st = &g_cb.agent[i];
    const ship_t *s = npc->ship;

    /* A docked fly sleeps: stake 0, brain-time rented to the swarm. */
    if (npc->state == NPC_STATE_DOCKED) return 0;

    uint32_t stake = 100;                        /* awake and working */
    stake += 60u * (uint32_t)s->towed_count;     /* ore in tow */
    stake += 90u * (uint32_t)s->towed_pod_count; /* cargo in tow */
    stake += (s->towed_scaffold >= 0) ? 250u : 0u;
    if (npc->state == NPC_STATE_TRAVEL_TO_DEST && npc->dest_station >= 0)
        stake += 150u;                           /* contract in flight */
    stake += (uint32_t)(st->lust >> 10);         /* urgency buys thought */
    stake += (uint32_t)(st->fear >> 10);
    return stake;
}

/* ---------------- injection ---------------- */

/* Push this tick's sensory drive into the fly's current circuit. The
 * ring neurons are GABAergic: driving the LEFT ring suppresses the
 * left half of the central complex and the calibrated turn goes RIGHT,
 * so a desired right turn (u > 0) means more drive on the left ring. */
static void cb_inject(int i, fb_sim *sim)
{
    cb_state_t *cb = &g_cb;
    cb_agent_state_t *st = &cb->agent[i];
    if (!sim) return;

    /* Arousal: hunger and lust raise the tonic substrate, pain spikes
     * it briefly. Max ~+50% over the configured tonic. */
    int32_t arousal = st->lust + st->hunger;
    int32_t tonic = cb->tonic_uv +
        (int32_t)(((int64_t)cb->tonic_uv * arousal) >> 17) +
        (int32_t)(((int64_t)cb->steer_uv * st->pain) >> 17);

    /* Columnar tonic: the substrate the inhibitory ring sculpts. */
    if (cb->col_n) {
        for (uint32_t k = 0; k < cb->col_n; k++)
            cb->col_scratch[k] = tonic;
        fb_sim_inject_pop(sim, (uint32_t)i, "columnar", cb->col_scratch);
    }

    /* Steering: push-pull around the symmetric half point. */
    if (cb->ring_n) {
        const fb_circuit *c = sim->c;
        int32_t u = st->pending_u;               /* Q16 in [-1,1] */
        int32_t half = cb->steer_uv / 2;
        int32_t delta = (int32_t)(((int64_t)cb->steer_uv * u) >> 17);
        int32_t left  = half + delta;
        int32_t right = half - delta;
        for (uint32_t k = 0; k < cb->ring_n; k++) {
            int8_t sd = c->side[cb->ring_idx[k]];
            cb->ring_scratch[k] = (sd < 0) ? left : (sd > 0) ? right : half;
        }
        fb_sim_inject_pop(sim, (uint32_t)i, "ring", cb->ring_scratch);
    }

    /* FEAR: bilateral drive into the escape/freeze descending channels.
     * These are the fly's DNp07/DNp10/DNp09 analogues; the flight read
     * side watches them for a freeze override. */
    if (st->fear > 0) {
        int32_t fear_uv = (int32_t)(((int64_t)cb->steer_uv * st->fear) >> 16);
        int32_t ch[1];
        ch[0] = fear_uv;
        fb_sim_inject_pop(sim, (uint32_t)i, "escape_left", ch);
        fb_sim_inject_pop(sim, (uint32_t)i, "escape_right", ch);
        fb_sim_inject_pop(sim, (uint32_t)i, "escape2_left", ch);
        fb_sim_inject_pop(sim, (uint32_t)i, "escape2_right", ch);
        fb_sim_inject_pop(sim, (uint32_t)i, "brake_left", ch);
        fb_sim_inject_pop(sim, (uint32_t)i, "brake_right", ch);
    }
}

/* ---------------- tick ---------------- */

void signal_connectome_tick(world_t *w)
{
    if (!g_cb.enabled || !w) return;
    cb_state_t *cb = &g_cb;

    uint32_t active = 0, sleeping = 0;
    cb_station_agg_t agg[MAX_STATIONS];
    memset(agg, 0, sizeof(agg));

    /* Pass 1: sense. Update each fly's drives and fold them into its
     * station's aggregate, which is the input to that station's policy. */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *npc = &w->npc_ships[i];
        if (!npc->active ||
            npc->brain_mode != SERVER_BRAIN_MODE_CONNECTOME) {
            fb_swarm_set_stake(&cb->swarm, (uint32_t)i, 0);
            continue;
        }
        cb_update_drives(w, npc, i);
        int hs = npc->home_station;
        if (hs >= 0 && hs < MAX_STATIONS) {
            cb_agent_state_t *st = &cb->agent[i];
            agg[hs].hunger += (uint64_t)st->hunger;
            agg[hs].lust   += (uint64_t)st->lust;
            agg[hs].fear   += (uint64_t)st->fear;
            agg[hs].count++;
            if (npc->state == NPC_STATE_MINING ||
                npc->ship->towed_count > 0 ||
                npc->ship->towed_pod_count > 0 ||
                npc->ship->towed_scaffold >= 0)
                agg[hs].productive++;
        }
    }

    /* The stations are the planners: each samples a posture for its own
     * swarm and broadcasts it. */
    if (cb->strategy_period) cb_station_strategy_advance(w, agg);

    /* Pass 2: act. Apply the heard station posture, then publish stake. */
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *npc = &w->npc_ships[i];
        if (!npc->active ||
            npc->brain_mode != SERVER_BRAIN_MODE_CONNECTOME) continue;
        if (cb->strategy_period) {
            int hs = npc->home_station;
            if (hs >= 0 && hs < MAX_STATIONS) {
                float authority = cb_clampf(
                    signal_strength_at(w, npc->ship->pos), 0.0f, 1.0f);
                cb_strategy_modulate(&cb->agent[i],
                                     (int)cb->station_strategy[hs],
                                     authority);
            }
        }
        uint32_t stake = cb_compute_stake(npc, i);
        fb_swarm_set_stake(&cb->swarm, (uint32_t)i, stake);
        if (stake == 0) {
            /* Sleeping: membrane frozen, share rented out. */
            sleeping++;
            cb->agent[i].pending_u = 0;
            continue;
        }
        active++;
    }

    /* Brain-time market clears: allocate by stake, step every fly
     * exactly what it earned (or rented).
     *
     * We drive the step loop here rather than calling fb_swarm_tick,
     * because the sensory drive has to be re-injected for EVERY kernel
     * step, not once per sim tick. Injection adds current to the
     * membrane for a single step; a fly that earned twenty steps and got
     * drive on only the first one spends the other nineteen decaying
     * with a 20 ms time constant, and the circuit is silent long before
     * the tick ends. That is not a slow fly, it is a dead one: the
     * descending bus reads 0 Hz, every NPC fails the arousal gate in
     * flight_cmd, and thrust is clamped to zero forever. Sustained drive
     * is also what the circuit was characterised under.
     *
     * flyswarm owns the allocation policy; only the stepping moves here,
     * so stake, renting, promotion and the budget all behave as before. */
    uint32_t spent32 = fb_swarm_allocate(&cb->swarm);
    cb->swarm.units_spent += spent32;
    for (uint32_t i = 0; i < cb->swarm.n_agents; i++) {
        uint32_t steps = cb->swarm.steps[i];
        if (!steps) continue;                  /* asleep or unfunded */
        fb_sim *sim = fb_swarm_sim_for(&cb->swarm, i);
        if (!sim) continue;
        for (uint32_t k = 0; k < steps; k++) {
            cb_inject((int)i, sim);
            fb_sim_step_agent(sim, i);
        }
        cb->swarm.steps_run += steps;
    }
    cb->swarm.tick++;
    uint64_t spent = spent32;

    cb->stats.ticks++;
    cb->stats.steps = cb->swarm.steps_run;
    cb->stats.units_spent = cb->swarm.units_spent;
    /* Units granted while flies slept are what the swarm rented. */
    if (sleeping > 0) cb->stats.rented_units += spent;
    cb->stats.promotions = cb->swarm.promotions;
    cb->stats.demotions = cb->swarm.demotions;
    cb->stats.active_flies = active;
    cb->stats.sleeping_flies = sleeping;
    cb->stats.deep_flies = 0;
    for (uint32_t i = 0; i < cb->swarm.n_agents; i++)
        if (cb->swarm.deep_on[i]) cb->stats.deep_flies++;
}

/* ---------------- flight readout ---------------- */

bool signal_connectome_flight_cmd(const world_t *w,
                                   int npc_idx,
                                   npc_ship_t *npc,
                                   float turn_in,
                                   float *turn_out,
                                   float *thrust_out)
{
    if (!g_cb.enabled || !npc || !npc->ship || !turn_out || !thrust_out)
        return false;
    if (npc_idx < 0 || npc_idx >= MAX_NPC_SHIPS) return false;
    cb_state_t *cb = &g_cb;
    cb_agent_state_t *st = &cb->agent[npc_idx];

    /* Record the reflex turn as this tick's sensory drive: what the fly
     * WANTED is what its ring neurons are asked about next tick. */
    float u = cb_clampf(turn_in, -1.0f, 1.0f);
    st->pending_u = (int32_t)(u * (float)Q16_ONE);

    fb_sim *sim = fb_swarm_sim_for(&cb->swarm, (uint32_t)npc_idx);
    if (!sim) {
        *turn_out = turn_in;
        return true;
    }

    fb_command cmd;
    fb_sim_command(sim, (uint32_t)npc_idx, &cmd);

    /* Brain turn, deadzone the noise floor. */
    float brain_turn = (float)cmd.turn / (float)Q16_ONE;
    if (fabsf(brain_turn) < 0.03f) brain_turn = 0.0f;
    brain_turn = cb_clampf(brain_turn, -1.0f, 1.0f);

    /* Smooth it. The descending readout is a decayed rate estimate over a
     * saturated recurrent network and it rattles hard tick to tick; the
     * EMA is the only thing standing between that and the rudder. */
    st->turn_ema += ((int32_t)(brain_turn * (float)Q16_ONE) - st->turn_ema) >> 4;
    float smooth = (float)st->turn_ema / (float)Q16_ONE;

    /* The brain BIASES the reflex; it does not replace it.
     *
     * Measured on the real nav blob, the descending turn axis gives a
     * repeatable answer at full drive -- suppress-left, symmetric and
     * suppress-right come out deterministically ordered, separated by
     * about 0.29 -- but every intermediate steering level in between is
     * chaotic. Removing the membrane noise does not fix it and neither
     * does averaging over 2000 steps: the response simply is not a
     * function of the input. That is the extraction artifact the flybrain
     * README reports as "flat, non-monotone" for this subgraph, and it is
     * a property of the connectome, not of this adapter.
     *
     * So the reflex controller stays authoritative and the fly leans on
     * it, bounded by turn_gain. That keeps the wiring genuinely in the
     * loop -- a frightened or lovestruck fly really does push the rudder
     * differently -- without letting a chaotic axis fly a loaded hauler
     * into a rock face. Clearance still scales it: near a rock face the
     * giant-fibre reflex gets the wheel to itself.
     */
    const hull_def_t *hull = ship_hull_def(npc->ship);
    float radius = hull ? hull->ship_radius : 16.0f;
    float clear = nav_forward_clearance(w, npc->ship->pos, npc->ship->vel,
                                        radius, npc->ship->angle);
    float authority = cb_clampf(clear, 0.0f, 1.0f);
    float gain = (float)cb->turn_gain / (float)Q16_ONE;
    *turn_out = cb_clampf(turn_in + smooth * authority * gain, -1.0f, 1.0f);

    /* Arousal gate: an unconscious fly cannot push the engine, no
     * matter what the reflex wants. Lust and pain wake it. */
    float thrust = *thrust_out;
    int32_t awake_thr = cb->awake_hz_q16 - (st->lust >> 2) - (st->pain >> 3);
    if (cmd.drive < awake_thr)
        thrust = fminf(thrust, 0.0f);

    /* FEAR freeze: a sufficiently frightened fly whose escape/freeze
     * channels are firing holds position (reverse thrust) instead of
     * blundering on. Rocks kill; freezing sometimes saves. */
    if (st->fear >= (Q16_ONE * 3) / 4) {
        fb_control ctl;
        fb_sim_control(sim, (uint32_t)npc_idx, &ctl);
        if (ctl.brake + ctl.escape >= 8192)   /* ~0.125 Hz on the pair */
            thrust = -1.0f;
    }

    /* SIGNAL_CONNECTOME_DEBUG=<n>: dump agent 0's decision every n ticks.
     * This subsystem fails silently -- a dead circuit just produces NPCs
     * that never thrust -- so there has to be a way to see the numbers. */
    if (cb->debug_every && npc_idx == 0 &&
        (w->tick % cb->debug_every) == 0)
        fprintf(stderr, "[connectome] t=%u drive=%.2fHz turn_brain=%+.3f "
                        "ema=%+.3f clear=%.2f turn_in=%+.3f -> turn=%+.3f "
                        "thrust %+.2f -> %+.2f  lust=%.2f fear=%.2f\n",
                w->tick, cmd.drive / 65536.0, brain_turn, smooth, authority,
                turn_in, *turn_out, *thrust_out, thrust,
                st->lust / 65536.0, st->fear / 65536.0);

    *thrust_out = thrust;
    return true;
}

/* ---------------- stats / checksum ---------------- */

uint64_t signal_connectome_checksum(void)
{
    if (!g_cb.enabled) return 0;
    return fb_swarm_checksum(&g_cb.swarm);
}

bool signal_connectome_stats(signal_connectome_stats_t *out)
{
    if (!out) return false;
    if (!g_cb.enabled) return false;
    *out = g_cb.stats;
    return true;
}
