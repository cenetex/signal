/*
 * signal_brain.c -- Minimal .nnckpt loader and WASD scorer for server bots.
 */
#include "signal_brain.h"
#include "sim_autopilot.h"
#include "sim_nav.h"
#include "station_util.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    SB_ACTION_COUNT = SIGNAL_BRAIN_FLIGHT_ACTION_COUNT,
    SB_FEATURE_COUNT = SIGNAL_BRAIN_FLIGHT_FEATURE_COUNT,
    SB_LAYER_COUNT = 4,
    SB_HIDDEN0 = 32,
    SB_HIDDEN1 = 16,
};

typedef struct {
    size_t previous;
    size_t current;
    double *weights; /* current-major: weights[neuron * previous + input] */
    double *biases;
} signal_brain_layer_t;

typedef struct {
    int loaded;
    uint32_t hidden_activation;
    uint32_t output_activation;
    uint32_t feature_encoder_version;
    char feature_set[32];
    signal_brain_layer_t layers[SB_LAYER_COUNT - 1];
    uint64_t inference_count;
} signal_brain_model_t;

typedef signal_brain_flight_action_t signal_brain_action_t;

typedef struct {
    vec2 pos;
    float standoff;
} signal_brain_target_t;

typedef struct {
    double dist;
    double control_dist;
    double heading_cos;
    double heading_sin;
    double control_heading_cos;
    double control_heading_sin;
    double speed;
    double forward_speed;
    double lateral_speed;
    double brake_distance;
    double fwd_clear;
    double left_clear;
    double right_clear;
    double vel_clear;
    double signal;
    double hull_ratio;
    double path_count;
    double path_current;
    double heading_error;
    double fwd_path_blocked;
    double left_path_blocked;
    double right_path_blocked;
} signal_brain_state_t;

static signal_brain_model_t g_brain;
bool g_neural_singleplayer = false;
static hnn_holonet_t g_npc_holonets[MAX_NPC_SHIPS];
static bool g_npc_holonet_ready[MAX_NPC_SHIPS];

static bool signal_hnn_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *value = getenv("SIGNAL_HNN_DEBUG");
        cached = (value && value[0] != '\0' &&
                  strcmp(value, "0") != 0) ? 1 : 0;
    }
    return cached != 0;
}

#define SIGNAL_HNN_DEBUG_LOG(...) \
    do { \
        if (signal_hnn_debug_enabled()) fprintf(stderr, __VA_ARGS__); \
    } while (0)

static const signal_brain_action_t SB_ACTIONS[SB_ACTION_COUNT] = {
    {"NONE", 0, 0},
    {"W", 0, 1},
    {"A", -1, 0},
    {"D", 1, 0},
    {"S", 0, -1},
    {"WA", -1, 1},
    {"WD", 1, 1},
    {"SA", -1, -1},
    {"SD", 1, -1},
};

_Static_assert((int)HNN_ACTION_COUNT == (int)SB_ACTION_COUNT,
               "HNN action vocabulary must match Signal flight actions");

static int signal_brain_npc_slot(const world_t *w, const npc_ship_t *npc) {
    if (!w || !npc) return -1;
    ptrdiff_t slot = npc - w->npc_ships;
    if (slot < 0 || slot >= MAX_NPC_SHIPS) return -1;
    return (int)slot;
}

static void signal_brain_reset_npc_holonet_slot(int slot) {
    if (slot < 0 || slot >= MAX_NPC_SHIPS) return;
    hnn_holonet_init(&g_npc_holonets[slot]);
    g_npc_holonet_ready[slot] = true;
}

static hnn_holonet_t *signal_brain_npc_holonet_for(world_t *w,
                                                   npc_ship_t *npc) {
    int slot = signal_brain_npc_slot(w, npc);
    if (slot < 0) return NULL;
    if (!g_npc_holonet_ready[slot])
        signal_brain_reset_npc_holonet_slot(slot);
    if (npc->hnn_mem.experience_count <= 0 &&
        hnn_holonet_active_count(&g_npc_holonets[slot]) > 0) {
        signal_brain_reset_npc_holonet_slot(slot);
    }
    return &g_npc_holonets[slot];
}

int signal_brain_holographic_npc_holonet_active_count(const world_t *w,
                                                      const npc_ship_t *npc) {
    int slot = signal_brain_npc_slot(w, npc);
    if (slot < 0 || !g_npc_holonet_ready[slot]) return 0;
    return hnn_holonet_active_count(&g_npc_holonets[slot]);
}

int signal_brain_flight_action_count(void) {
    return SB_ACTION_COUNT;
}

const signal_brain_flight_action_t *signal_brain_flight_action(int index) {
    if (index < 0 || index >= SB_ACTION_COUNT) return NULL;
    return &SB_ACTIONS[index];
}

int signal_brain_flight_action_index_from_intent(const input_intent_t *intent) {
    if (!intent) return -1;
    int turn = 0;
    int thrust = 0;
    if (intent->turn < -0.01f) turn = -1;
    else if (intent->turn > 0.01f) turn = 1;
    if (intent->thrust < -0.01f) thrust = -1;
    else if (intent->thrust > 0.01f) thrust = 1;

    for (int i = 0; i < SB_ACTION_COUNT; i++) {
        if (SB_ACTIONS[i].turn == turn && SB_ACTIONS[i].thrust == thrust)
            return i;
    }
    return -1;
}

static void set_err(char *err, size_t err_size, const char *msg) {
    if (!err || err_size == 0) return;
    snprintf(err, err_size, "%s", msg ? msg : "unknown error");
}

static void signal_brain_free(void) {
    for (int i = 0; i < SB_LAYER_COUNT - 1; i++) {
        free(g_brain.layers[i].weights);
        free(g_brain.layers[i].biases);
    }
    memset(&g_brain, 0, sizeof(g_brain));
}

static int read_exact(FILE *fp, void *dst, size_t len) {
    return fread(dst, 1, len, fp) == len;
}

static int read_u32_le(FILE *fp, uint32_t *out) {
    uint8_t b[4];
    if (!read_exact(fp, b, sizeof(b))) return 0;
    *out = ((uint32_t)b[0]) |
           ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return 1;
}

static int read_u64_le(FILE *fp, uint64_t *out) {
    uint8_t b[8];
    if (!read_exact(fp, b, sizeof(b))) return 0;
    *out = ((uint64_t)b[0]) |
           ((uint64_t)b[1] << 8) |
           ((uint64_t)b[2] << 16) |
           ((uint64_t)b[3] << 24) |
           ((uint64_t)b[4] << 32) |
           ((uint64_t)b[5] << 40) |
           ((uint64_t)b[6] << 48) |
           ((uint64_t)b[7] << 56);
    return 1;
}

static int read_scalar(FILE *fp, uint32_t scalar_type, double *out) {
    if (scalar_type == 1) {
        uint8_t b[4];
        float f = 0.0f;
        if (!read_exact(fp, b, sizeof(b))) return 0;
        memcpy(&f, b, sizeof(f));
        *out = (double)f;
        return 1;
    }
    if (scalar_type == 2) {
        uint8_t b[8];
        double d = 0.0;
        if (!read_exact(fp, b, sizeof(b))) return 0;
        memcpy(&d, b, sizeof(d));
        *out = d;
        return 1;
    }
    return 0;
}

static int skip_bytes(FILE *fp, uint64_t len) {
    uint8_t scratch[256];
    while (len > 0) {
        size_t n = len < sizeof(scratch) ? (size_t)len : sizeof(scratch);
        if (!read_exact(fp, scratch, n)) return 0;
        len -= (uint64_t)n;
    }
    return 1;
}

static int read_fixed_string(FILE *fp, char *dst, size_t dst_size, size_t len) {
    char *tmp = (char *)malloc(len);
    if (!tmp) return 0;
    int ok = read_exact(fp, tmp, len);
    if (ok && dst && dst_size > 0) {
        size_t copy = 0;
        while (copy < len && tmp[copy] != '\0' && copy + 1 < dst_size) copy++;
        memcpy(dst, tmp, copy);
        dst[copy] = '\0';
    }
    free(tmp);
    return ok;
}

static double activation(double x, uint32_t kind) {
    switch (kind) {
    case 1:
        return x > 0.0 ? x : 0.0;
    case 2:
        return tanh(x);
    case 3:
        return x;
    case 0:
    default:
        if (x >= 0.0) {
            return 1.0 / (1.0 + exp(-x));
        }
        {
            double ex = exp(x);
            return ex / (1.0 + ex);
        }
    }
}

bool signal_brain_load_checkpoint(const char *path, char *err, size_t err_size) {
    if (!path || path[0] == '\0') {
        set_err(err, err_size, "missing checkpoint path");
        return false;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        char buf[256];
        snprintf(buf, sizeof(buf), "open failed: %s", strerror(errno));
        set_err(err, err_size, buf);
        return false;
    }

    signal_brain_free();
    char magic[8];
    uint32_t checkpoint_version = 0;
    uint32_t scalar_type = 0;
    uint64_t layer_count = 0;
    uint64_t sizes[SB_LAYER_COUNT] = {0};
    uint32_t loss = 0;
    bool ok = read_exact(fp, magic, sizeof(magic)) &&
              memcmp(magic, "NNCKPT01", sizeof(magic)) == 0 &&
              read_u32_le(fp, &checkpoint_version) &&
              checkpoint_version == 1 &&
              read_u32_le(fp, &scalar_type) &&
              read_u64_le(fp, &layer_count) &&
              layer_count == SB_LAYER_COUNT;
    for (int i = 0; ok && i < SB_LAYER_COUNT; i++) ok = read_u64_le(fp, &sizes[i]);
    ok = ok && sizes[0] == SB_FEATURE_COUNT && sizes[1] == SB_HIDDEN0 &&
         sizes[2] == SB_HIDDEN1 && sizes[3] == 1 &&
         read_u32_le(fp, &g_brain.hidden_activation) &&
         read_u32_le(fp, &g_brain.output_activation) &&
         read_u32_le(fp, &loss);
    (void)loss;

    for (int layer = 1; ok && layer < SB_LAYER_COUNT; layer++) {
        signal_brain_layer_t *dst = &g_brain.layers[layer - 1];
        dst->previous = (size_t)sizes[layer - 1];
        dst->current = (size_t)sizes[layer];
        size_t weight_count = dst->previous * dst->current;
        dst->weights = (double *)calloc(weight_count, sizeof(double));
        dst->biases = (double *)calloc(dst->current, sizeof(double));
        if (!dst->weights || !dst->biases) {
            ok = 0;
            break;
        }
        for (size_t i = 0; ok && i < weight_count; i++)
            ok = read_scalar(fp, scalar_type, &dst->weights[i]);
        for (size_t i = 0; ok && i < dst->current; i++)
            ok = read_scalar(fp, scalar_type, &dst->biases[i]);
    }

    uint64_t metadata_size = 0;
    ok = ok && read_u64_le(fp, &metadata_size) &&
         (metadata_size == 455u || metadata_size == 487u) &&
         read_u32_le(fp, &g_brain.feature_encoder_version) &&
         skip_bytes(fp, 65u * 3u + 256u);
    if (ok && metadata_size == 487u) {
        ok = read_fixed_string(fp, g_brain.feature_set,
                               sizeof(g_brain.feature_set), 32u);
    }

    fclose(fp);

    if (!ok) {
        signal_brain_free();
        set_err(err, err_size, "unsupported or truncated signal-flight checkpoint");
        return false;
    }
    if (g_brain.feature_encoder_version != 2u ||
        strcmp(g_brain.feature_set, "signal-flight-live-v2") != 0) {
        signal_brain_free();
        set_err(err, err_size, "checkpoint is not signal-flight-live-v2");
        return false;
    }

    g_brain.loaded = 1;
    return true;
}

bool signal_brain_loaded(void) {
    return g_brain.loaded != 0;
}

uint64_t signal_brain_inference_count(void) {
    return g_brain.inference_count;
}

static double clip(double x, double lo, double hi) {
    if (!isfinite(x)) return 0.0;
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static double scale(double x, double denom) {
    return denom > 0.0 ? clip(x / denom, 0.0, 1.0) : 0.0;
}

static int autopilot_station_phase(int state) {
    return state == AUTOPILOT_STEP_RETURN_TO_REFINERY ||
           state == AUTOPILOT_STEP_DOCK ||
           state == AUTOPILOT_STEP_SELL ||
           state == AUTOPILOT_STEP_LAUNCH ||
           state == AUTOPILOT_STEP_LOGISTICS_BUY ||
           state == AUTOPILOT_STEP_LOGISTICS_TRAVEL ||
           state == AUTOPILOT_STEP_LOGISTICS_DOCK ||
           state == AUTOPILOT_STEP_LOGISTICS_DELIVER ||
           state == AUTOPILOT_STEP_LOGISTICS_WAIT ||
           state == AUTOPILOT_STEP_EXIT_STATION;
}

static double signed_scale(double x, double denom) {
    return denom > 0.0 ? clip(x / denom, -1.0, 1.0) : 0.0;
}

static double forward_model(const double input[SB_FEATURE_COUNT]) {
    double hidden0[SB_HIDDEN0] = {0.0};
    double hidden1[SB_HIDDEN1] = {0.0};
    double output[1] = {0.0};
    const double *src = input;
    double *dsts[SB_LAYER_COUNT - 1] = {hidden0, hidden1, output};

    for (int li = 0; li < SB_LAYER_COUNT - 1; li++) {
        const signal_brain_layer_t *layer = &g_brain.layers[li];
        double *dst = dsts[li];
        uint32_t act = (li == SB_LAYER_COUNT - 2)
            ? g_brain.output_activation
            : g_brain.hidden_activation;
        for (size_t neuron = 0; neuron < layer->current; neuron++) {
            double sum = layer->biases[neuron];
            const double *w = &layer->weights[neuron * layer->previous];
            for (size_t i = 0; i < layer->previous; i++) sum += w[i] * src[i];
            dst[neuron] = activation(sum, act);
        }
        src = dst;
    }
    g_brain.inference_count++;
    return output[0];
}

static vec2 asteroid_approach_point(const server_player_t *sp,
                                    const asteroid_t *a,
                                    float *out_standoff) {
    vec2 delta = v2_sub(sp->ship.pos, a->pos);
    float d = v2_len(delta);
    if (d < 1.0f) delta = v2_from_angle(sp->ship.angle);
    else delta = v2_scale(delta, 1.0f / d);
    float standoff = fmaxf(a->radius + 118.0f, 210.0f);
    if (out_standoff) *out_standoff = standoff;
    return v2_add(a->pos, v2_scale(delta, standoff));
}

static float station_clearance_radius(const station_t *st) {
    float by_radius = st->radius + 650.0f;
    float by_dock = st->dock_radius + 455.0f;
    float r = fmaxf(by_radius, by_dock);
    return fmaxf(850.0f, r);
}

static vec2 station_approach_point(const station_t *st,
                                   const server_player_t *sp,
                                   float *out_standoff) {
    vec2 delta = v2_sub(sp->ship.pos, st->pos);
    float d = v2_len(delta);
    if (d < 1.0f) delta = v2_from_angle(sp->ship.angle);
    else delta = v2_scale(delta, 1.0f / d);
    float radius = fmaxf(st->dock_radius * 0.82f, st->radius + 160.0f);
    if (out_standoff) *out_standoff = 96.0f;
    return v2_add(st->pos, v2_scale(delta, radius));
}

static int brain_target_for(const world_t *w,
                            const server_player_t *sp,
                            signal_brain_target_t *out) {
    if (!w || !sp || !out || sp->docked) return 0;
    int target = sp->autopilot_target;

    switch (sp->autopilot_state) {
    case AUTOPILOT_STEP_FLY_TO_TARGET:
    case AUTOPILOT_STEP_MINE:
    case AUTOPILOT_STEP_COLLECT:
        if (target >= 0 && target < MAX_ASTEROIDS &&
            w->asteroids[target].active) {
            out->pos = asteroid_approach_point(sp, &w->asteroids[target],
                                               &out->standoff);
            return 1;
        }
        break;
    case AUTOPILOT_STEP_RETURN_TO_REFINERY:
    case AUTOPILOT_STEP_DOCK:
    case AUTOPILOT_STEP_SELL:
    case AUTOPILOT_STEP_LOGISTICS_TRAVEL:
    case AUTOPILOT_STEP_LOGISTICS_DOCK:
    case AUTOPILOT_STEP_LOGISTICS_DELIVER:
    case AUTOPILOT_STEP_EXIT_STATION:
        if (target < 0 || target >= MAX_STATIONS) target = sp->nearby_station;
        if (target < 0 || target >= MAX_STATIONS) target = sp->current_station;
        if (target >= 0 && target < MAX_STATIONS &&
            station_exists(&w->stations[target])) {
            out->pos = station_approach_point(&w->stations[target], sp,
                                              &out->standoff);
            return 1;
        }
        break;
    default:
        break;
    }

    return 0;
}

static double station_forward_clearance(const world_t *w,
                                        vec2 pos,
                                        vec2 vel,
                                        float ship_radius,
                                        float heading) {
    vec2 fwd = v2_from_angle(heading);
    vec2 perp = v2(-fwd.y, fwd.x);
    float speed = v2_len(vel);
    float lookahead = fmaxf(100.0f, fminf(speed * 1.5f, 500.0f));
    double worst = 1.0;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &w->stations[i];
        if (!station_exists(st)) continue;
        vec2 to_st = v2_sub(st->pos, pos);
        float fd = v2_dot(to_st, fwd);
        float clearance = station_clearance_radius(st) + ship_radius;
        if (fd < -clearance || fd > lookahead) continue;
        float lateral = fabsf(v2_dot(to_st, perp));
        if (lateral >= clearance) continue;
        float along = fixp_sqrtf(fmaxf(0.0f, clearance * clearance - lateral * lateral));
        float entry = fd - along;
        float exit = fd + along;
        if (entry >= 0.0f) {
            double v = (double)(entry / lookahead);
            if (v < worst) worst = v;
        } else if (exit >= 0.0f) {
            worst = 0.0;
        }
    }
    return clip(worst, 0.0, 1.0);
}

static double clearance_at(const world_t *w,
                           const server_player_t *sp,
                           float ship_radius,
                           float heading) {
    double asteroids = nav_forward_clearance(w, sp->ship.pos, sp->ship.vel,
                                             ship_radius, heading);
    double stations = station_forward_clearance(w, sp->ship.pos, sp->ship.vel,
                                                ship_radius, heading);
    return asteroids < stations ? asteroids : stations;
}

static double path_blocked_at(const world_t *w,
                              const server_player_t *sp,
                              float ship_radius,
                              float heading) {
    vec2 fwd = v2_from_angle(heading);
    float speed = v2_len(sp->ship.vel);
    float lookahead = fmaxf(100.0f, fminf(speed * 1.5f, 500.0f));
    vec2 probe_end = v2_add(sp->ship.pos, v2_scale(fwd, lookahead));
    return nav_segment_clear(w, sp->ship.pos, probe_end, ship_radius + 30.0f)
        ? 0.0 : 1.0;
}

static signal_brain_state_t brain_state_for(const world_t *w,
                                            const server_player_t *sp,
                                            vec2 target) {
    signal_brain_state_t s;
    memset(&s, 0, sizeof(s));
    vec2 to_goal = v2_sub(target, sp->ship.pos);
    float dist = v2_len(to_goal);
    float desired = dist > 0.001f ? fixp_atan2f(to_goal.y, to_goal.x) : sp->ship.angle;
    float heading_error = wrap_angle(desired - sp->ship.angle);
    vec2 forward = v2_from_angle(sp->ship.angle);
    vec2 right = v2(-forward.y, forward.x);
    float speed = v2_len(sp->ship.vel);
    float forward_speed = v2_dot(sp->ship.vel, forward);
    float lateral_speed = v2_dot(sp->ship.vel, right);
    float brake_distance = (speed * speed) / (2.0f * SHIP_BRAKE);
    const hull_def_t *hull = ship_hull_def(&sp->ship);
    float ship_radius = hull ? hull->ship_radius : 16.0f;
    nav_path_t *path = nav_player_path(sp->id);

    s.dist = dist;
    s.control_dist = dist;
    s.heading_error = heading_error;
    s.heading_cos = cos(heading_error);
    s.heading_sin = sin(heading_error);
    s.control_heading_cos = s.heading_cos;
    s.control_heading_sin = s.heading_sin;
    s.speed = speed;
    s.forward_speed = forward_speed;
    s.lateral_speed = lateral_speed;
    s.brake_distance = brake_distance;
    s.fwd_clear = clearance_at(w, sp, ship_radius, sp->ship.angle);
    s.left_clear = clearance_at(w, sp, ship_radius, sp->ship.angle + 0.7f);
    s.right_clear = clearance_at(w, sp, ship_radius, sp->ship.angle - 0.7f);
    s.fwd_path_blocked = path_blocked_at(w, sp, ship_radius, sp->ship.angle);
    s.left_path_blocked = path_blocked_at(w, sp, ship_radius, sp->ship.angle + 0.7f);
    s.right_path_blocked = path_blocked_at(w, sp, ship_radius, sp->ship.angle - 0.7f);
    s.vel_clear = speed > 0.5f
        ? clearance_at(w, sp, ship_radius, fixp_atan2f(sp->ship.vel.y, sp->ship.vel.x))
        : s.fwd_clear;
    s.signal = signal_strength_at(w, sp->ship.pos);
    {
        float max_hull = ship_max_hull(&sp->ship);
        s.hull_ratio = max_hull > 0.0f ? clip(sp->ship.hull / max_hull, 0.0, 1.0) : 1.0;
    }
    s.path_count = path ? path->count : 0;
    s.path_current = path ? path->current : 0;
    return s;
}

static void fill_features(const signal_brain_state_t *s,
                          const signal_brain_action_t *a,
                          size_t action_index,
                          double row[SB_FEATURE_COUNT]) {
    double dist = scale(s->dist, 6000.0);
    double control_dist = scale(s->control_dist, 6000.0);
    double speed = signed_scale(s->speed, 350.0);
    double forward_speed = signed_scale(s->forward_speed, 350.0);
    double lateral_speed = signed_scale(s->lateral_speed, 350.0);
    double brake_distance = scale(s->brake_distance, 700.0);
    double fwd_clear = clip(s->fwd_clear, 0.0, 1.0);
    double left_clear = clip(s->left_clear, 0.0, 1.0);
    double right_clear = clip(s->right_clear, 0.0, 1.0);
    double vel_clear = clip(s->vel_clear, 0.0, 1.0);
    double signal = clip(s->signal, 0.0, 1.0);
    double hull = clip(s->hull_ratio, 0.0, 1.0);
    double path_count = scale(s->path_count, 16.0);
    double path_current = scale(s->path_current, 16.0);
    double progress = 0.0;
    double next_dist = dist;
    double next_speed = speed;
    double close_goal = 1.0 - dist;
    double turn = (double)a->turn;
    double thrust = (double)a->thrust;
    double heading_cos = clip(s->heading_cos, -1.0, 1.0);
    double heading_sin = clip(s->heading_sin, -1.0, 1.0);
    double control_heading_cos = clip(s->control_heading_cos, -1.0, 1.0);
    double control_heading_sin = clip(s->control_heading_sin, -1.0, 1.0);

    memset(row, 0, SB_FEATURE_COUNT * sizeof(*row));
    row[0] = 1.0;
    row[1] = (double)action_index / (double)(SB_ACTION_COUNT - 1);
    row[2] = turn;
    row[3] = thrust;
    row[4] = turn < 0.0 ? 1.0 : 0.0;
    row[5] = turn > 0.0 ? 1.0 : 0.0;
    row[6] = thrust > 0.0 ? 1.0 : 0.0;
    row[7] = thrust < 0.0 ? 1.0 : 0.0;
    row[8] = action_index == 0 ? 1.0 : 0.0;
    row[9] = dist;
    row[10] = control_dist;
    row[11] = heading_cos;
    row[12] = heading_sin;
    row[13] = control_heading_cos;
    row[14] = control_heading_sin;
    row[15] = speed;
    row[16] = forward_speed;
    row[17] = lateral_speed;
    row[18] = brake_distance;
    row[19] = fwd_clear;
    row[20] = left_clear;
    row[21] = right_clear;
    row[22] = vel_clear;
    row[23] = signal;
    row[24] = hull;
    row[25] = path_count;
    row[26] = path_current;
    row[27] = progress;
    row[28] = next_dist;
    row[29] = next_speed;
    row[30] = turn * control_heading_sin;
    row[31] = thrust * control_heading_cos;
    row[32] = thrust * fwd_clear;
    row[33] = (thrust < 0.0 ? 1.0 : 0.0) * brake_distance;
    row[34] = (turn < 0.0 ? 1.0 : 0.0) * left_clear;
    row[35] = (turn > 0.0 ? 1.0 : 0.0) * right_clear;
    row[36] = (action_index == 0 ? 1.0 : 0.0) * fabs(speed);
    row[37] = turn * lateral_speed;
    row[38] = thrust * forward_speed;
    row[39] = thrust * (1.0 - hull);
    row[40] = thrust * next_speed;
    row[41] = thrust * progress;
    row[42] = thrust * close_goal;
    row[43] = turn * heading_sin;
    row[44] = thrust * heading_cos;
    row[45] = (thrust < 0.0 ? 1.0 : 0.0) * forward_speed;
    row[46] = fabs(turn);
    row[47] = fabs(thrust);
}

static int action_allowed(const signal_brain_state_t *s,
                          const signal_brain_action_t *a,
                          size_t action_index) {
    if (a->thrust > 0 && s->fwd_path_blocked > 0.5)
        return 0;
    if (s->dist <= 450.0 || s->speed >= 20.0) return 1;
    if (action_index == 0 || a->thrust < 0) return 0;
    if (fabs(s->heading_error) > 0.35 && a->thrust > 0 && a->turn == 0)
        return 0;
    return 1;
}

bool signal_brain_build_flight_candidate_features(
    const world_t *w,
    const server_player_t *sp,
    float features_out[SB_ACTION_COUNT * SB_FEATURE_COUNT],
    uint8_t allowed_out[SB_ACTION_COUNT],
    int *forward_blocked_out) {
    if (forward_blocked_out) *forward_blocked_out = 0;
    if (!w || !sp || !features_out || sp->docked || sp->autopilot_mode == 0)
        return false;
    if (autopilot_station_phase(sp->autopilot_state)) return false;

    signal_brain_target_t target = {0};
    if (!brain_target_for(w, sp, &target)) return false;

    signal_brain_state_t state = brain_state_for(w, sp, target.pos);
    if (forward_blocked_out)
        *forward_blocked_out = state.fwd_path_blocked > 0.5 ? 1 : 0;

    double row[SB_FEATURE_COUNT];
    for (int i = 0; i < SB_ACTION_COUNT; i++) {
        fill_features(&state, &SB_ACTIONS[i], (size_t)i, row);
        for (int j = 0; j < SB_FEATURE_COUNT; j++)
            features_out[i * SB_FEATURE_COUNT + j] = (float)row[j];
        if (allowed_out)
            allowed_out[i] = (uint8_t)action_allowed(&state, &SB_ACTIONS[i], (size_t)i);
    }
    return true;
}

static void signal_brain_apply_player_action(server_player_t *sp, int action) {
    if (!sp || action < 0 || action >= SB_ACTION_COUNT) return;
    sp->input.turn = (float)SB_ACTIONS[action].turn;
    sp->input.thrust = (float)SB_ACTIONS[action].thrust;
    sp->input.reverse_thrust = false;
}

bool signal_brain_drive_with_decision(world_t *w,
                                      server_player_t *sp,
                                      float dt,
                                      signal_brain_flight_decision_t *decision) {
    (void)dt;
    if (decision) memset(decision, 0, sizeof(*decision));
    if (!g_brain.loaded || !w || !sp ||
        sp->server_brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT ||
        sp->autopilot_mode == 0 || sp->docked) {
        return false;
    }

    if (autopilot_station_phase(sp->autopilot_state)) {
        return false;
    }

    signal_brain_target_t target = {0};
    if (!brain_target_for(w, sp, &target)) return false;

    signal_brain_state_t state = brain_state_for(w, sp, target.pos);
    uint16_t allowed_mask = 0;
    if (decision) {
        decision->candidate_count = SB_ACTION_COUNT;
        decision->selected_index = -1;
        decision->raw_index = -1;
        decision->signal_quality = (float)clip(state.signal, 0.0, 1.0);
        decision->route_risk = (float)clip(1.0 - state.fwd_clear, 0.0, 1.0);
        decision->hull_ratio = (float)clip(state.hull_ratio, 0.0, 1.0);
        decision->forward_blocked = state.fwd_path_blocked > 0.5;
    }
    if (state.fwd_path_blocked > 0.5) {
        sp->input.turn = 0.0f;
        sp->input.thrust = -1.0f;
        sp->input.reverse_thrust = true;
        sp->input.mine = false;
        if (decision) {
            decision->selected_index = 4;
            decision->raw_index = 4;
            decision->selected_score = 0.0f;
            decision->raw_score = 0.0f;
            decision->route_risk = 1.0f;
            decision->hard_override = true;
        }
        return true;
    }

    double row[SB_FEATURE_COUNT];
    double best_score = -INFINITY;
    int best = 0;
    double best_raw_score = -INFINITY;
    int best_raw = 0;

    for (int i = 0; i < SB_ACTION_COUNT; i++) {
        fill_features(&state, &SB_ACTIONS[i], (size_t)i, row);
        double score = forward_model(row);
        if (score > best_raw_score) {
            best_raw_score = score;
            best_raw = i;
        }
        bool allowed = action_allowed(&state, &SB_ACTIONS[i], (size_t)i) != 0;
        if (allowed) allowed_mask |= (uint16_t)(1u << i);
        if (!allowed) continue;
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    if (!isfinite(best_score)) best = best_raw;
    signal_brain_apply_player_action(sp, best);
    if (decision) {
        decision->selected_index = best;
        decision->raw_index = best_raw;
        decision->selected_score = (float)(isfinite(best_score)
            ? best_score
            : best_raw_score);
        decision->raw_score = (float)best_raw_score;
        decision->allowed_mask = allowed_mask;
    }
    return true;
}

void signal_brain_drive(world_t *w, server_player_t *sp, float dt) {
    (void)signal_brain_drive_with_decision(w, sp, dt, NULL);
}


/* Simplified feature fill for NPC ships — avoids server_player_t dependency. */
static void fill_npc_features(const npc_ship_t *npc, const vec2 target,
                              const signal_brain_action_t *action, double row[SB_FEATURE_COUNT]) {
    const ship_t *s = &npc->ship;
    memset(row, 0, SB_FEATURE_COUNT * sizeof(double));
    
    float dx = target.x - s->pos.x;
    float dy = target.y - s->pos.y;
    float dist = v2_len(v2(dx, dy));
    float target_heading = fixp_atan2f(dy, dx);
    float heading_error = target_heading - s->angle;
    while (heading_error > 3.14159265f) heading_error -= 2.0f * 3.14159265f;
    while (heading_error < -3.14159265f) heading_error += 2.0f * 3.14159265f;
    
    float speed = v2_len(s->vel);
    vec2 forward = v2_from_angle(s->angle);
    float fwd_speed = v2_dot(s->vel, forward);
    
    row[0] = 1.0;  /* bias */
    row[1] = dist / 5000.0;
    row[2] = heading_error / 3.14159265f;
    row[3] = fixp_cosf(heading_error);
    row[4] = fixp_sinf(heading_error);
    row[5] = speed / 300.0;
    row[6] = fwd_speed / 300.0;
    row[7] = action->turn;
    row[8] = action->thrust;
    row[9] = npc->hull > 0.0f ? s->hull / npc->hull : 0.0f;
    row[10] = (npc->brain_mode == 1) ? 1.0 : 0.0;
}

static void signal_brain_drive_npc_holographic(world_t *w, npc_ship_t *npc);

bool signal_brain_drive_npc_to(world_t *w, npc_ship_t *npc, vec2 target) {
    if (!w || !npc || !npc->active) return false;
    if (!g_brain.loaded || npc->brain_mode != SERVER_BRAIN_MODE_NEURAL_FLIGHT)
        return false;

    double row[SB_FEATURE_COUNT];
    double best_score = -1e300;
    int best = 0;

    for (int i = 0; i < SB_ACTION_COUNT; i++) {
        fill_npc_features(npc, target, &SB_ACTIONS[i], row);
        double score = forward_model(row);
        if (isfinite(score) && score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (!isfinite(best_score))
        return false;

    npc->input.turn = (float)SB_ACTIONS[best].turn;
    npc->input.thrust = (float)SB_ACTIONS[best].thrust;
    npc->thrusting = (SB_ACTIONS[best].thrust > 0);
    return true;
}

void signal_brain_drive_npc(world_t *w, npc_ship_t *npc, float dt) {
    (void)dt;
    if (!w || !npc || !npc->active) return;
    if (npc->state == NPC_STATE_DOCKED) return;

    if (npc->brain_mode == SERVER_BRAIN_MODE_HOLOGRAPHIC) {
        signal_brain_drive_npc_holographic(w, npc);
        return;
    }

    /* Target: asteroid if assigned, destination station while hauling,
     * otherwise home station. */
    vec2 target = {0};
    if (npc->target_asteroid >= 0 && npc->target_asteroid < MAX_ASTEROIDS) {
        target = w->asteroids[npc->target_asteroid].pos;
    } else if (npc->state == NPC_STATE_TRAVEL_TO_DEST &&
               npc->dest_station >= 0 && npc->dest_station < MAX_STATIONS) {
        target = w->stations[npc->dest_station].pos;
    } else {
        int home = npc->home_station;
        target = (home >= 0 && home < MAX_STATIONS) ? w->stations[home].pos : v2(0,0);
    }

    (void)signal_brain_drive_npc_to(w, npc, target);
}

/* ---- Holographic pilot brain ---- */

static hnn_action_table_t g_hnn_actions;
static bool g_hnn_actions_initialized = false;

void signal_brain_holographic_init(void) {
    if (g_hnn_actions_initialized) return;
    SIGNAL_HNN_DEBUG_LOG("[hnn] holographic init: generating action vectors...\n");
    hnn_action_table_init(&g_hnn_actions);
    SIGNAL_HNN_DEBUG_LOG("[hnn] holographic init: done\n");
    g_hnn_actions_initialized = true;
}

/*
 * Build holographic pilot features from an NPC ship's current state.
 * Mirrors the feature set used by signal_brain_drive but compressed
 * into the hnn_pilot_features_t struct for VSA encoding.
 */
static void hnn_fill_npc_features(const world_t *w,
                                  const npc_ship_t *npc,
                                  vec2 target,
                                  int action_turn,
                                  int action_thrust,
                                  int action_idx,
                                  hnn_pilot_features_t *f) {
    const ship_t *s = &npc->ship;
    memset(f, 0, sizeof(*f));

    float dx = target.x - s->pos.x;
    float dy = target.y - s->pos.y;
    float dist = v2_len(v2(dx, dy));
    float target_heading = fixp_atan2f(dy, dx);
    float heading_error = target_heading - s->angle;
    while (heading_error > 3.14159265f) heading_error -= 2.0f * 3.14159265f;
    while (heading_error < -3.14159265f) heading_error += 2.0f * 3.14159265f;

    float speed = v2_len(s->vel);
    vec2 forward = v2_from_angle(s->angle);
    float fwd_speed = v2_dot(s->vel, forward);
    vec2 right = v2(-forward.y, forward.x);
    float lat_speed = v2_dot(s->vel, right);
    float brake_dist = (speed * speed) / (2.0f * SHIP_BRAKE);
    const hull_def_t *hull = npc_hull_def(npc);
    float ship_radius = hull ? hull->ship_radius : 16.0f;
    double fwd_clear = nav_forward_clearance(w, s->pos, s->vel,
                                             ship_radius, s->angle);
    double fwd_station_clear = station_forward_clearance(
        w, s->pos, s->vel, ship_radius, s->angle);
    if (fwd_station_clear < fwd_clear) fwd_clear = fwd_station_clear;
    double left_clear = nav_forward_clearance(w, s->pos, s->vel,
                                              ship_radius, s->angle + 0.7f);
    double left_station_clear = station_forward_clearance(
        w, s->pos, s->vel, ship_radius, s->angle + 0.7f);
    if (left_station_clear < left_clear) left_clear = left_station_clear;
    double right_clear = nav_forward_clearance(w, s->pos, s->vel,
                                               ship_radius, s->angle - 0.7f);
    double right_station_clear = station_forward_clearance(
        w, s->pos, s->vel, ship_radius, s->angle - 0.7f);
    if (right_station_clear < right_clear) right_clear = right_station_clear;

    float lookahead = fmaxf(100.0f, fminf(speed * 1.5f, 500.0f));
    vec2 fwd_probe = v2_add(s->pos, v2_scale(v2_from_angle(s->angle), lookahead));
    vec2 left_probe = v2_add(s->pos, v2_scale(v2_from_angle(s->angle + 0.7f),
                                              lookahead));
    vec2 right_probe = v2_add(s->pos, v2_scale(v2_from_angle(s->angle - 0.7f),
                                               lookahead));
    float path_clearance = ship_radius + 30.0f;
    float fwd_blocked = nav_segment_clear(w, s->pos, fwd_probe, path_clearance)
        ? 0.0f : 1.0f;
    float left_blocked = nav_segment_clear(w, s->pos, left_probe, path_clearance)
        ? 0.0f : 1.0f;
    float right_blocked = nav_segment_clear(w, s->pos, right_probe, path_clearance)
        ? 0.0f : 1.0f;

    float signal_q = signal_strength_at(w, s->pos);
    float max_hull = hull ? hull->max_hull : 100.0f;
    float hull_r = max_hull > 0.0f ? s->hull / max_hull : 1.0f;

    f->target_dist     = dist / 6000.0f;
    f->heading_error   = heading_error / 3.14159265f;
    f->heading_cos     = fixp_cosf(heading_error);
    f->heading_sin     = fixp_sinf(heading_error);
    f->speed           = speed / 350.0f;
    f->forward_speed   = fwd_speed / 350.0f;
    f->lateral_speed   = lat_speed / 350.0f;
    f->brake_distance  = (brake_dist > 700.0f) ? 1.0f : brake_dist / 700.0f;
    f->fwd_clear       = (float)clip(fwd_clear, 0.0, 1.0);
    f->left_clear      = (float)clip(left_clear, 0.0, 1.0);
    f->right_clear     = (float)clip(right_clear, 0.0, 1.0);
    f->signal_quality  = signal_q;
    f->hull_ratio      = hull_r;
    f->path_count      = 0.0f;
    f->path_current    = 0.0f;
    f->fwd_blocked     = fwd_blocked;
    f->left_blocked    = left_blocked;
    f->right_blocked   = right_blocked;
    f->goal_close      = 1.0f - (dist / 6000.0f);
    f->action_delta_turn   = (float)action_turn;
    f->action_delta_thrust = (float)action_thrust;
    f->action_is_none      = (action_idx == 0) ? 1.0f : 0.0f;
    f->action_is_reverse   = (action_thrust < 0) ? 1.0f : 0.0f;
    f->composite_dot       = (float)action_turn * f->heading_sin;
}

static bool hnn_npc_action_allowed(const hnn_pilot_features_t *state,
                                   const signal_brain_action_t *action,
                                   int action_index) {
    if (!state || !action || action_index < 0 ||
        action_index >= HNN_ACTION_COUNT) {
        return false;
    }

    const float slow_speed = 20.0f / 350.0f;
    bool forward_blocked =
        state->fwd_blocked > 0.5f || state->fwd_clear < 0.15f;
    bool positive_turn_blocked =
        state->left_blocked > 0.5f || state->left_clear < 0.12f;
    bool negative_turn_blocked =
        state->right_blocked > 0.5f || state->right_clear < 0.12f;

    if (forward_blocked) {
        if (action->thrust > 0) return false;
        if (state->speed > slow_speed && action->thrust >= 0) return false;
    }

    if (action->thrust >= 0) {
        if (action->turn > 0 && positive_turn_blocked) return false;
        if (action->turn < 0 && negative_turn_blocked) return false;
    }

    if (state->target_dist > (450.0f / 6000.0f) &&
        state->speed < slow_speed) {
        if (action_index == 0 || action->thrust < 0) return false;
        if (fabsf(state->heading_error) > 0.35f &&
            action->thrust > 0 &&
            action->turn == 0) {
            return false;
        }
    }

    return true;
}

static uint16_t hnn_npc_allowed_mask(const hnn_pilot_features_t *state,
                                     uint8_t allowed[HNN_ACTION_COUNT]) {
    uint16_t mask = 0;
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        const signal_brain_action_t *action = &SB_ACTIONS[i];
        bool ok = hnn_npc_action_allowed(state, action, i);
        if (allowed) allowed[i] = ok ? 1u : 0u;
        if (ok) mask |= (uint16_t)(1u << i);
    }
    return mask;
}

static int hnn_npc_best_allowed_action(
    const float scores[HNN_ACTION_COUNT],
    const uint8_t allowed[HNN_ACTION_COUNT],
    float *out_score,
    float *out_margin) {
    int best = -1;
    float best_score = -INFINITY;
    float second_score = -INFINITY;
    if (!scores || !allowed) {
        if (out_score) *out_score = 0.0f;
        if (out_margin) *out_margin = 0.0f;
        return -1;
    }
    for (int i = 0; i < HNN_ACTION_COUNT; i++) {
        if (!allowed[i] || !isfinite(scores[i])) continue;
        if (scores[i] > best_score) {
            second_score = best_score;
            best_score = scores[i];
            best = i;
        } else if (scores[i] > second_score) {
            second_score = scores[i];
        }
    }
    if (out_score)
        *out_score = isfinite(best_score) ? best_score : 0.0f;
    if (out_margin) {
        *out_margin = (isfinite(best_score) && isfinite(second_score))
            ? best_score - second_score
            : 0.0f;
    }
    return best;
}

static void hnn_npc_store_experience(npc_ship_t *npc,
                                     hnn_holonet_t *holonet,
                                     const hnn_pilot_features_t *route_state,
                                     const hnn_pilot_features_t *assoc_state,
                                     int action_index) {
    if (!npc || !route_state || !assoc_state ||
        action_index < 0 || action_index >= HNN_ACTION_COUNT) {
        return;
    }

    float route_vec[HNN_DIM];
    float assoc_vec[HNN_DIM];
    hnn_encode_state(route_state, route_vec);
    hnn_encode_state(assoc_state, assoc_vec);
    hnn_memory_store(&npc->hnn_mem, assoc_vec,
                     g_hnn_actions.vecs[action_index]);
    if (holonet)
        hnn_holonet_store(holonet, route_vec, assoc_vec,
                          g_hnn_actions.vecs[action_index]);
    npc->hnn_experience_local_version++;
}


/* Holographic NPC flight controller.
 *
 * Each tick:
 *   1. Build state features (without action info)
 *   2. Retrieve best action from holographic memory
 *   3. Apply turn/thrust to NPC input
 *
 * The state vector is encoded by bundling (feature_idx, value) pairs.
 * Retrieval unbinds the state from the holographic memory to get an
 * action vector, then cosine-similarity picks the closest action.
 */
static void signal_brain_drive_npc_holographic(world_t *w, npc_ship_t *npc) {
    if (!w || !npc) return;
    hnn_holonet_t *holonet = signal_brain_npc_holonet_for(w, npc);
    if (signal_hnn_debug_enabled()) {
        static int debug_call_count = 0;
        debug_call_count++;
        if (debug_call_count == 1 || debug_call_count % 600 == 0)
            SIGNAL_HNN_DEBUG_LOG("[hnn] call #%d state=%d tgt=%d timer=%.1f exp=%d pos=(%.0f,%.0f)\n",
                                 debug_call_count,
                                 npc->state,
                                 npc->target_asteroid,
                                 npc->state_timer,
                                 npc->hnn_mem.experience_count,
                                 npc->ship.pos.x,
                                 npc->ship.pos.y);
    }

    /* Dock-return cycle: after accumulating experience, head home to
     * share it via the gossip dock handshake. Use state_timer as a
     * flight-duration counter. The state machine may have set
     * state_timer to a recheck delay (e.g. 5.0 for IDLE); we reset it
     * on the first holographic-brain tick after undocking by checking
     * if we have no target and state_timer is suspiciously large. */
    int home = npc->home_station;
    vec2 home_pos = (home >= 0 && home < MAX_STATIONS) ? w->stations[home].pos : v2(0, 0);

    /* Reset flight timer if state machine left a stale recheck delay.
     * The state machine sets state_timer >= 2.0 for IDLE rechecks;
     * we want a clean clock starting from 0. */
    if (npc->target_asteroid < 0 && npc->state_timer > 1.0f) {
        npc->state_timer = 0.0f;
    }

    /* Count flight ticks */
    npc->state_timer += 1.0f / 120.0f; /* SIM_DT */

    /* Return home after ~30 seconds of flight, or sooner if we have
     * good experience, have been flying at least 5 seconds, and happen
     * to be nearby. The 5-second floor prevents immediate re-docking
     * right after undocking. */
    bool time_to_dock = (npc->state_timer > 30.0f);
    float dist_to_home = v2_len(v2_sub(npc->ship.pos, home_pos));
    bool near_home = (dist_to_home < 400.0f && npc->hnn_mem.experience_count > 10
                      && npc->state_timer > 5.0f);

    if ((time_to_dock || near_home) && dist_to_home < 250.0f) {
        /* Close enough — transition to docked. The next tick the
         * state machine will handle the dock handshake including
         * holographic experience exchange. */
        SIGNAL_HNN_DEBUG_LOG("[hnn] slot=%ld ->DOCKED exp=%d t=%.1f dist=%.0f\n",
                             (long)(npc - w->npc_ships),
                             npc->hnn_mem.experience_count,
                             npc->state_timer,
                             dist_to_home);
        npc->state = NPC_STATE_DOCKED;
        npc->state_timer = NPC_DOCK_TIME;
        npc->target_asteroid = -1;
        /* state_timer will be 0 after state machine runs the dock
         * cycle, so the holographic brain gets a clean counter on
         * the next undock. */
        return;
    }

    /* Pick an exploration target: if we have no target, find a
     * nearby asteroid to fly toward. When it's time to return, steer
     * home. This only triggers once per flight cycle since we set
     * target_asteroid after picking. */
    vec2 target;
    if (npc->target_asteroid < 0) {
        /* Need a destination — find a nearby asteroid to explore */
        float best_d = 999999.0f;
        int best_ast = -1;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            if (!w->asteroids[i].active || asteroid_is_collectible(&w->asteroids[i]))
                continue;
            float d = v2_dist_sq(npc->ship.pos, w->asteroids[i].pos);
            /* Pick something 500-3000 units away — not too close, not
             * too far. Weight toward mid-range. */
            if (d > 250000.0f && d < 9000000.0f && d < best_d) {
                best_d = d;
                best_ast = i;
            }
        }
        if (best_ast >= 0) {
            npc->target_asteroid = best_ast;
            SIGNAL_HNN_DEBUG_LOG("[hnn] slot=%ld exploring asteroid %d (dist=%.0f)\n",
                                 (long)(npc - w->npc_ships),
                                 best_ast,
                                 fixp_sqrtf(best_d));
        }
    }

    if (time_to_dock) {
        target = home_pos;
        npc->target_asteroid = -1;
    } else if (npc->target_asteroid >= 0 && npc->target_asteroid < MAX_ASTEROIDS) {
        target = w->asteroids[npc->target_asteroid].pos;
    } else {
        target = home_pos;
    }

    /* Score each action by encoding (state, action) and comparing
     * against the holographic memory. The action that best matches
     * stored experiences wins. */
    /* If memory is empty (first few ticks), use heading correction
     * to bootstrap. Store these early experiences so the memory
     * can start learning immediately. */
    if (npc->hnn_mem.experience_count == 0 &&
        (!holonet || hnn_holonet_active_count(holonet) == 0)) {
        float dx = target.x - npc->ship.pos.x;
        float dy = target.y - npc->ship.pos.y;
        float target_heading = fixp_atan2f(dy, dx);
        float heading_error = target_heading - npc->ship.angle;
        while (heading_error > 3.14159265f) heading_error -= 2.0f * 3.14159265f;
        while (heading_error < -3.14159265f) heading_error += 2.0f * 3.14159265f;

        int boot_turn = 0, boot_thrust = 0;
        if (fabsf(heading_error) > 0.3f) {
            boot_turn = (heading_error > 0.0f) ? 1 : -1;
            boot_thrust = 1;
        } else {
            boot_turn = 0;
            boot_thrust = 1;
        }
        /* Map (turn,thrust) to action index for storage */
        int boot_action = 0;
        if (boot_turn == 0 && boot_thrust == 0) boot_action = 0;
        else if (boot_turn == 0 && boot_thrust == 1) boot_action = 1;
        else if (boot_turn == -1 && boot_thrust == 0) boot_action = 2;
        else if (boot_turn == 1 && boot_thrust == 0) boot_action = 3;
        else if (boot_turn == 0 && boot_thrust == -1) boot_action = 4;
        else if (boot_turn == -1 && boot_thrust == 1) boot_action = 5;
        else if (boot_turn == 1 && boot_thrust == 1) boot_action = 6;
        else if (boot_turn == -1 && boot_thrust == -1) boot_action = 7;
        else if (boot_turn == 1 && boot_thrust == -1) boot_action = 8;

        hnn_pilot_features_t gate_state;
        hnn_fill_npc_features(w, npc, target, 0, 0, 0, &gate_state);
        uint8_t allowed[HNN_ACTION_COUNT];
        uint16_t allowed_mask = hnn_npc_allowed_mask(&gate_state, allowed);
        if (!allowed[boot_action]) {
            int preferred_turn = heading_error > 0.0f ? 3 : 2;
            if (fabsf(heading_error) > 0.3f && allowed[preferred_turn]) {
                boot_action = preferred_turn;
            } else if (allowed[4]) {
                boot_action = 4;
            } else {
                for (int i = 0; i < HNN_ACTION_COUNT; i++) {
                    if (allowed[i]) {
                        boot_action = i;
                        break;
                    }
                }
            }
            if (allowed_mask == 0) boot_action = 0;
            boot_turn = SB_ACTIONS[boot_action].turn;
            boot_thrust = SB_ACTIONS[boot_action].thrust;
        }

        npc->input.turn = (float)boot_turn;
        npc->input.thrust = (float)boot_thrust;
        npc->thrusting = (boot_thrust > 0);

        /* Store bootstrap experience */
        hnn_pilot_features_t fs;
        SIGNAL_HNN_DEBUG_LOG("[hnn] bootstrap: filling features...\n");
        hnn_fill_npc_features(w, npc, target, boot_turn, boot_thrust,
                              boot_action, &fs);
        SIGNAL_HNN_DEBUG_LOG("[hnn] bootstrap: encoding state...\n");
        SIGNAL_HNN_DEBUG_LOG("[hnn] bootstrap: storing in memory...\n");
        hnn_npc_store_experience(npc, holonet, &gate_state, &fs, boot_action);
        SIGNAL_HNN_DEBUG_LOG("[hnn] bootstrap: done! exp=%d\n",
                             npc->hnn_mem.experience_count);
        return;
    }

    /* Encode state-only features (action-independent) */
    hnn_pilot_features_t state_only;
    hnn_fill_npc_features(w, npc, target, 0, 0, 0, &state_only);

    float scores[HNN_ACTION_COUNT] = {0.0f};
    float margin = 0.0f;
    float fidelity = 0.0f;
    int best_action = -1;
    if (holonet && hnn_holonet_active_count(holonet) > 0) {
        best_action = hnn_holonet_score_actions(holonet,
                                                &g_hnn_actions,
                                                &state_only,
                                                scores,
                                                &margin,
                                                &fidelity,
                                                3);
    }
    if (best_action < 0) {
        best_action = hnn_score_actions(&npc->hnn_mem,
                                        &g_hnn_actions,
                                        &state_only,
                                        scores,
                                        &margin,
                                        &fidelity,
                                        3);
    }
    (void)fidelity;
    if (best_action < 0) best_action = 0;
    int raw_action = best_action;
    float selected_score = scores[raw_action];
    float selected_margin = margin;
    uint8_t allowed[HNN_ACTION_COUNT];
    uint16_t allowed_mask = hnn_npc_allowed_mask(&state_only, allowed);
    best_action = hnn_npc_best_allowed_action(
        scores, allowed, &selected_score, &selected_margin);
    if (best_action < 0) {
        best_action = (allowed_mask & (uint16_t)(1u << 4)) ? 4 : raw_action;
        selected_score = scores[best_action];
        selected_margin = margin;
    }

    npc->hnn_mem.last_retrieval_similarity = selected_score;
    npc->hnn_mem.last_margin = selected_margin;
    npc->input.turn = (float)SB_ACTIONS[best_action].turn;
    npc->input.thrust = (float)SB_ACTIONS[best_action].thrust;
    npc->thrusting = (SB_ACTIONS[best_action].thrust > 0);

    /* Store this experience into holographic memory for future recall.
     * Encode the full (state, action) pair and bundle it in. */
    {
        hnn_pilot_features_t full_state;
        hnn_fill_npc_features(w, npc, target,
                              SB_ACTIONS[best_action].turn,
                              SB_ACTIONS[best_action].thrust,
                              best_action,
                              &full_state);
        hnn_npc_store_experience(npc, holonet, &state_only, &full_state,
                                 best_action);
    }
}
