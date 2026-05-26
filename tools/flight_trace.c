/*
 * flight_trace.c -- Offline WASD flight-controller trace generator.
 *
 * Produces supervised labels for a tiny ship "brain" that presses the same
 * low-level movement controls as a player. The teacher is the existing
 * deterministic flight_steer_to controller; its continuous turn/thrust
 * command is quantized into the nearest WASD key combination and then applied
 * through world_sim_step_player_only(), so the resulting trajectory uses the
 * normal player input -> ship physics -> collision path.
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game_sim.h"
#include "manifest.h"
#include "sim_flight.h"
#include "sim_nav.h"

typedef struct {
    int turn;
    int thrust;
    const char *name;
} flight_trace_action_t;

typedef enum {
    TRACE_FORMAT_CSV = 0,
    TRACE_FORMAT_JSONL = 1,
} trace_format_t;

typedef struct {
    int episodes;
    int ticks;
    uint32_t seed;
    int shard_index;
    int shard_total;
    trace_format_t format;
    const char *out_path;
    float spawn_min;
    float spawn_max;
    float goal_min;
    float goal_max;
    float max_speed;
    float standoff;
    int counterfactual;
    int sample_stride;
    int horizon_ticks;
    float explore_random;
    float utility_progress_scale;
    float utility_forward_bonus;
    float utility_reverse_penalty;
    float utility_lateral_penalty;
    float utility_damage_penalty;
    float utility_death_penalty;
    float utility_wall_penalty;
    int pain_probe;
    int wall_probe;
} flight_trace_config_t;

typedef struct {
    uint32_t world_seed;
    int station_index;
    vec2 spawn_pos;
    vec2 spawn_vel;
    float spawn_angle;
    vec2 goal;
} flight_trace_episode_t;

typedef struct {
    int damage_events;
    int death_events;
    float damage_amount;
} flight_trace_pain_t;

typedef struct {
    int episode;
    int tick;
    uint32_t seed;
    int shard_index;
    int shard_total;
    int action;
    const char *action_name;
    int turn_key;
    int thrust_key;
    uint16_t candidate_mask;
    float teacher_turn;
    float teacher_thrust;
    float dist;
    float control_dist;
    float heading_cos;
    float heading_sin;
    float control_heading_cos;
    float control_heading_sin;
    float speed;
    float forward_speed;
    float lateral_speed;
    float brake_distance;
    float fwd_clear;
    float left_clear;
    float right_clear;
    float vel_clear;
    float signal;
    float hull_ratio;
    int path_count;
    int path_current;
    float progress;
    float next_dist;
    float next_speed;
    float x;
    float y;
    float vx;
    float vy;
    float angle;
    float goal_x;
    float goal_y;
    float control_x;
    float control_y;
    uint64_t group_id;
    int horizon_ticks;
    float utility;
    int damage_events;
    int death_events;
    float damage_amount;
    float hull_loss;
    float end_hull_ratio;
} flight_trace_row_t;

static const flight_trace_action_t FLIGHT_TRACE_ACTIONS[] = {
    { 0,  0, "NONE"},
    { 0,  1, "W"},
    {-1,  0, "A"},
    { 1,  0, "D"},
    { 0, -1, "S"},
    {-1,  1, "WA"},
    { 1,  1, "WD"},
    {-1, -1, "SA"},
    { 1, -1, "SD"},
};

#define FLIGHT_TRACE_ACTION_COUNT \
    ((int)(sizeof(FLIGHT_TRACE_ACTIONS) / sizeof(FLIGHT_TRACE_ACTIONS[0])))

static void print_usage(FILE *fp)
{
    fprintf(fp,
            "usage: flight_trace [options]\n"
            "\n"
            "Options:\n"
            "  --episodes N      total episode ids to consider (default 1000)\n"
            "  --ticks N         max ticks per episode at 120 Hz (default 600)\n"
            "  --seed N          deterministic world/trace seed (default 2037)\n"
            "  --shard I/N       generate only episode ids where id %% N == I\n"
            "  --format csv|jsonl output format (default csv)\n"
            "  --out PATH        write to PATH instead of stdout\n"
            "  --spawn-min X     min spawn radius around sampled station (default 800)\n"
            "  --spawn-max X     max spawn radius around sampled station (default 4200)\n"
            "  --goal-min X      min goal radius around sampled station (default 800)\n"
            "  --goal-max X      max goal radius around sampled station (default 5200)\n"
            "  --max-speed X     teacher cruise speed (default 220)\n"
            "  --standoff X      teacher standoff distance (default 0)\n"
            "  --counterfactual  enumerate WASD hypotheses and emit replay-certified utility\n"
            "  --sample-stride N sample every N ticks in counterfactual mode (default 12)\n"
            "  --horizon-ticks N replay horizon per action in counterfactual mode (default 36)\n"
            "  --explore-random X random action probability for state discovery (default 0.35)\n"
            "  --pain-probe      start counterfactual episodes near asteroids to certify crash pain\n"
            "  --wall-probe      start counterfactual episodes near station walls to certify impact pain\n"
            "  --utility-wall-penalty X penalty for ending replay near a wall at speed (default 0.75)\n"
            "  --help            show this help\n"
            "\n"
            "Example:\n"
            "  flight_trace --episodes 100000 --ticks 600 --seed 4242 \\\n"
            "      --shard 0/8 --format csv --out /tmp/flight-0.csv\n");
}

static int parse_int_arg(const char *text, int min_value, int *out)
{
    char *end = NULL;
    long value;
    if (text == NULL || *text == '\0' || out == NULL) return 0;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < min_value ||
        value > INT32_MAX) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

static int parse_u32_arg(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value;
    if (text == NULL || *text == '\0' || out == NULL) return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int parse_float_arg(const char *text, float min_value, float *out)
{
    char *end = NULL;
    float value;
    if (text == NULL || *text == '\0' || out == NULL) return 0;
    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value) ||
        value < min_value) {
        return 0;
    }
    *out = value;
    return 1;
}

static int parse_shard_arg(const char *text, int *out_index, int *out_total)
{
    const char *slash;
    char left[32];
    char right[32];
    size_t left_len;
    int index = 0;
    int total = 0;

    if (text == NULL || out_index == NULL || out_total == NULL) return 0;
    slash = strchr(text, '/');
    if (slash == NULL) return 0;
    left_len = (size_t)(slash - text);
    if (left_len == 0 || left_len >= sizeof(left)) return 0;
    if (strlen(slash + 1) == 0 || strlen(slash + 1) >= sizeof(right)) return 0;
    memcpy(left, text, left_len);
    left[left_len] = '\0';
    memcpy(right, slash + 1, strlen(slash + 1) + 1u);
    if (!parse_int_arg(left, 0, &index) || !parse_int_arg(right, 1, &total)) return 0;
    if (index >= total) return 0;
    *out_index = index;
    *out_total = total;
    return 1;
}

static int parse_args(int argc, char **argv, flight_trace_config_t *config)
{
    *config = (flight_trace_config_t){
        .episodes = 1000,
        .ticks = 600,
        .seed = 2037u,
        .shard_index = 0,
        .shard_total = 1,
        .format = TRACE_FORMAT_CSV,
        .out_path = NULL,
        .spawn_min = 800.0f,
        .spawn_max = 4200.0f,
        .goal_min = 800.0f,
        .goal_max = 5200.0f,
        .max_speed = 220.0f,
        .standoff = 0.0f,
        .counterfactual = 0,
        .sample_stride = 12,
        .horizon_ticks = 36,
        .explore_random = 0.35f,
        .utility_progress_scale = 300.0f,
        .utility_forward_bonus = 0.65f,
        .utility_reverse_penalty = 0.35f,
        .utility_lateral_penalty = 0.12f,
        .utility_damage_penalty = 0.10f,
        .utility_death_penalty = 10.0f,
        .utility_wall_penalty = 0.75f,
        .pain_probe = 0,
        .wall_probe = 0,
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(stdout);
            exit(0);
        } else if (strcmp(arg, "--episodes") == 0 && value != NULL) {
            if (!parse_int_arg(value, 1, &config->episodes)) return 0;
            i++;
        } else if (strcmp(arg, "--ticks") == 0 && value != NULL) {
            if (!parse_int_arg(value, 1, &config->ticks)) return 0;
            i++;
        } else if (strcmp(arg, "--seed") == 0 && value != NULL) {
            if (!parse_u32_arg(value, &config->seed)) return 0;
            i++;
        } else if (strcmp(arg, "--shard") == 0 && value != NULL) {
            if (!parse_shard_arg(value, &config->shard_index, &config->shard_total)) return 0;
            i++;
        } else if (strcmp(arg, "--format") == 0 && value != NULL) {
            if (strcmp(value, "csv") == 0) {
                config->format = TRACE_FORMAT_CSV;
            } else if (strcmp(value, "jsonl") == 0) {
                config->format = TRACE_FORMAT_JSONL;
            } else {
                return 0;
            }
            i++;
        } else if (strcmp(arg, "--out") == 0 && value != NULL) {
            config->out_path = value;
            i++;
        } else if (strcmp(arg, "--spawn-min") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->spawn_min)) return 0;
            i++;
        } else if (strcmp(arg, "--spawn-max") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->spawn_max)) return 0;
            i++;
        } else if (strcmp(arg, "--goal-min") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->goal_min)) return 0;
            i++;
        } else if (strcmp(arg, "--goal-max") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->goal_max)) return 0;
            i++;
        } else if (strcmp(arg, "--max-speed") == 0 && value != NULL) {
            if (!parse_float_arg(value, 1.0f, &config->max_speed)) return 0;
            i++;
        } else if (strcmp(arg, "--standoff") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->standoff)) return 0;
            i++;
        } else if (strcmp(arg, "--counterfactual") == 0) {
            config->counterfactual = 1;
        } else if (strcmp(arg, "--sample-stride") == 0 && value != NULL) {
            if (!parse_int_arg(value, 1, &config->sample_stride)) return 0;
            i++;
        } else if (strcmp(arg, "--horizon-ticks") == 0 && value != NULL) {
            if (!parse_int_arg(value, 1, &config->horizon_ticks)) return 0;
            i++;
        } else if (strcmp(arg, "--explore-random") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->explore_random)) return 0;
            if (config->explore_random > 1.0f) return 0;
            i++;
        } else if (strcmp(arg, "--pain-probe") == 0) {
            config->pain_probe = 1;
        } else if (strcmp(arg, "--wall-probe") == 0) {
            config->wall_probe = 1;
        } else if (strcmp(arg, "--utility-wall-penalty") == 0 && value != NULL) {
            if (!parse_float_arg(value, 0.0f, &config->utility_wall_penalty)) return 0;
            i++;
        } else {
            return 0;
        }
    }

    return config->spawn_min <= config->spawn_max &&
           config->goal_min <= config->goal_max;
}

static uint32_t rng_next(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9e3779b9u;
    return *state;
}

static float rng_unit(uint32_t *state)
{
    return (float)(rng_next(state) >> 8) / 16777215.0f;
}

static float rng_range(uint32_t *state, float lo, float hi)
{
    return lo + (hi - lo) * rng_unit(state);
}

static vec2 random_station_nearby(uint32_t *rng,
                                  const station_t *station,
                                  float min_radius,
                                  float max_radius)
{
    float angle = rng_range(rng, -PI_F, PI_F);
    float radius = rng_range(rng, min_radius, max_radius);
    return v2_add(station->pos, v2(cosf(angle) * radius, sinf(angle) * radius));
}

static float trace_wall_clearance(const world_t *w,
                                  vec2 pos,
                                  vec2 vel,
                                  float ship_radius,
                                  float heading)
{
    vec2 fwd = v2(cosf(heading), sinf(heading));
    vec2 perp = v2(-fwd.y, fwd.x);
    float speed = sqrtf(v2_len_sq(vel));
    float lookahead = fmaxf(100.0f, fminf(speed * 1.5f, 500.0f));
    float worst = 1.0f;
    int station_count = w->station_count > 0 ? w->station_count : 3;

    for (int i = 0; i < station_count && i < MAX_STATIONS; i++) {
        const station_t *st = &w->stations[i];
        vec2 to_station;
        float fd;
        float lateral;
        float along;
        float entry;
        float exit_dist;
        float clearance;

        if (st->module_count <= 0 && st->radius <= 0.0f && st->dock_radius <= 0.0f) {
            continue;
        }

        clearance = fmaxf(850.0f,
                          fmaxf(st->radius + 360.0f,
                                st->dock_radius + 252.0f)) + ship_radius;
        to_station = v2_sub(st->pos, pos);
        fd = v2_dot(to_station, fwd);
        if (fd < -clearance || fd > lookahead) continue;
        lateral = fabsf(v2_dot(to_station, perp));
        if (lateral >= clearance) continue;

        along = sqrtf(fmaxf(0.0f, clearance * clearance - lateral * lateral));
        entry = fd - along;
        exit_dist = fd + along;
        if (entry >= 0.0f) {
            worst = fminf(worst, entry / lookahead);
        } else if (exit_dist >= 0.0f) {
            worst = 0.0f;
        }
    }

    return fmaxf(0.0f, fminf(1.0f, worst));
}

static float trace_forward_clearance(const world_t *w,
                                     vec2 pos,
                                     vec2 vel,
                                     float ship_radius,
                                     float heading)
{
    float asteroid_clearance =
        nav_forward_clearance(w, pos, vel, ship_radius, heading);
    float wall_clearance =
        trace_wall_clearance(w, pos, vel, ship_radius, heading);
    return fminf(asteroid_clearance, wall_clearance);
}

static int quantize_action(flight_cmd_t cmd)
{
    int turn = 0;
    int thrust = 0;

    if (cmd.turn < -0.25f) {
        turn = -1;
    } else if (cmd.turn > 0.25f) {
        turn = 1;
    }

    if (cmd.thrust < -0.25f) {
        thrust = -1;
    } else if (cmd.thrust > 0.25f) {
        thrust = 1;
    }

    for (int i = 0; i < FLIGHT_TRACE_ACTION_COUNT; i++) {
        if (FLIGHT_TRACE_ACTIONS[i].turn == turn &&
            FLIGHT_TRACE_ACTIONS[i].thrust == thrust) {
            return i;
        }
    }
    return 0;
}

static uint16_t candidate_action_mask(void)
{
    uint16_t mask = 0;

    for (int i = 0; i < FLIGHT_TRACE_ACTION_COUNT; i++) {
        mask |= (uint16_t)(1u << (unsigned)i);
    }
    return mask;
}

static void reset_trace_player(world_t *w, server_player_t *sp);

static bool trace_reverse_allowed(const server_player_t *sp)
{
    const float reverse_start_speed = 2.0f;
    vec2 forward = ship_forward(sp->ship.angle);
    float forward_speed = v2_dot(sp->ship.vel, forward);
    return forward_speed <= reverse_start_speed;
}

static void apply_trace_action(server_player_t *sp, int action)
{
    const flight_trace_action_t *keys = &FLIGHT_TRACE_ACTIONS[action];
    memset(&sp->input, 0, sizeof(sp->input));
    sp->input.turn = (float)keys->turn;
    sp->input.thrust = (float)keys->thrust;
    sp->input.reverse_thrust = keys->thrust < 0 && trace_reverse_allowed(sp);
}

static void scan_pain_events(const world_t *w, flight_trace_pain_t *pain)
{
    if (w == NULL || pain == NULL) return;
    for (int i = 0; i < w->events.count; i++) {
        const sim_event_t *ev = &w->events.events[i];
        if (ev->player_id != 0) continue;
        if (ev->type == SIM_EVENT_DAMAGE) {
            pain->damage_events++;
            pain->damage_amount += ev->damage.amount;
        } else if (ev->type == SIM_EVENT_DEATH) {
            pain->death_events++;
        }
    }
}

static int choose_active_asteroid(const world_t *w, uint32_t *rng)
{
    int seen = 0;
    int selected = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!w->asteroids[i].active || w->asteroids[i].radius <= 0.0f) {
            continue;
        }
        seen++;
        if ((int)(rng_next(rng) % (uint32_t)seen) == 0) {
            selected = i;
        }
    }
    return selected;
}

static void init_trace_episode(const flight_trace_config_t *config,
                               int episode,
                               uint32_t *rng,
                               flight_trace_episode_t *init)
{
    world_t template_world = {0};
    uint32_t world_seed =
        config->seed ^ ((uint32_t)episode * 0x9e3779b9u) ^ 0x66d15eedu;

    memset(init, 0, sizeof(*init));
    template_world.rng = world_seed;
    world_reset(&template_world);
    init->world_seed = world_seed;
    init->station_index = (int)(rng_next(rng) % 3u);

    station_t *station = &template_world.stations[init->station_index];
    int wall_probe = config->wall_probe && (!config->pain_probe || ((episode & 1) == 0));
    int asteroid_idx = (!wall_probe && config->pain_probe)
                     ? choose_active_asteroid(&template_world, rng)
                     : -1;
    if (wall_probe) {
        float outer = STATION_RING_RADIUS[STATION_NUM_RINGS] + 100.0f;
        float wall_edge = STATION_RING_RADIUS[STATION_NUM_RINGS] + 58.0f;
        float angle = rng_range(rng, -PI_F, PI_F);
        float goal_angle = angle + PI_F + rng_range(rng, -0.45f, 0.45f);
        float spawn_radius = rng_range(rng, wall_edge - 15.0f, wall_edge + 55.0f);
        float goal_radius = rng_range(rng, outer + 650.0f, outer + 1600.0f);
        vec2 spawn_dir = v2(cosf(angle), sinf(angle));
        vec2 goal_dir = v2(cosf(goal_angle), sinf(goal_angle));
        vec2 to_goal;
        init->spawn_pos = v2_add(station->pos, v2_scale(spawn_dir, spawn_radius));
        init->goal = v2_add(station->pos, v2_scale(goal_dir, goal_radius));
        to_goal = v2_sub(init->goal, init->spawn_pos);
        init->spawn_angle = atan2f(to_goal.y, to_goal.x);
        init->spawn_vel = v2_scale(ship_forward(init->spawn_angle),
                                   rng_range(rng, 180.0f, 260.0f));
    } else if (asteroid_idx >= 0) {
        const asteroid_t *asteroid = &template_world.asteroids[asteroid_idx];
        float angle = rng_range(rng, -PI_F, PI_F);
        vec2 dir = v2(cosf(angle), sinf(angle));
        float near = asteroid->radius + 95.0f;
        init->spawn_pos = v2_sub(asteroid->pos, v2_scale(dir, near));
        init->spawn_vel = v2_scale(dir, 90.0f);
        init->spawn_angle = angle;
        init->goal = v2_add(asteroid->pos, v2_scale(dir, 1600.0f));
    } else {
        init->spawn_pos = random_station_nearby(rng,
                                                station,
                                                config->spawn_min,
                                                config->spawn_max);
        init->spawn_vel = v2(rng_range(rng, -70.0f, 70.0f),
                             rng_range(rng, -70.0f, 70.0f));
        init->spawn_angle = rng_range(rng, -PI_F, PI_F);
        init->goal = random_station_nearby(rng,
                                           station,
                                           config->goal_min,
                                           config->goal_max);
    }
    world_cleanup(&template_world);
}

static void setup_trace_world(const flight_trace_episode_t *init,
                              world_t *w,
                              server_player_t **out_sp)
{
    server_player_t *sp;

    memset(w, 0, sizeof(*w));
    w->rng = init->world_seed;
    world_reset(w);
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        w->npc_ships[n].active = false;
    }

    sp = &w->players[0];
    reset_trace_player(w, sp);
    sp->ship.pos = init->spawn_pos;
    sp->ship.vel = init->spawn_vel;
    sp->ship.angle = init->spawn_angle;
    sp->ship.hull = ship_max_hull(&sp->ship);
    sp->ship.towed_count = 0;
    sp->ship.towed_scaffold = -1;
    sp->docked = false;
    sp->boost_hold_timer = 0.0f;
    memset(&sp->input, 0, sizeof(sp->input));

    if (out_sp != NULL) {
        *out_sp = sp;
    }
}

static int goal_explore_action(const server_player_t *sp, vec2 goal)
{
    vec2 to_goal = v2_sub(goal, sp->ship.pos);
    float desired = atan2f(to_goal.y, to_goal.x);
    float heading_diff = wrap_angle(desired - sp->ship.angle);
    int turn = 0;
    int thrust = fabsf(heading_diff) < 0.75f ? 1 : 0;

    if (heading_diff < -0.18f) {
        turn = -1;
    } else if (heading_diff > 0.18f) {
        turn = 1;
    }

    for (int i = 0; i < FLIGHT_TRACE_ACTION_COUNT; i++) {
        if (FLIGHT_TRACE_ACTIONS[i].turn == turn &&
            FLIGHT_TRACE_ACTIONS[i].thrust == thrust) {
            return i;
        }
    }
    return 0;
}

static int choose_explore_action(const flight_trace_config_t *config,
                                 uint32_t *rng,
                                 const server_player_t *sp,
                                 vec2 goal)
{
    if (rng_unit(rng) < config->explore_random) {
        return (int)(rng_next(rng) % (uint32_t)FLIGHT_TRACE_ACTION_COUNT);
    }
    return goal_explore_action(sp, goal);
}

static void fill_state_row(const flight_trace_config_t *config,
                           const world_t *w,
                           const server_player_t *sp,
                           const nav_path_t *path,
                           vec2 goal,
                           int episode,
                           int tick,
                           int action,
                           flight_trace_row_t *row)
{
    const hull_def_t *hull = ship_hull_def(&sp->ship);
    const flight_trace_action_t *keys = &FLIGHT_TRACE_ACTIONS[action];
    vec2 to_goal = v2_sub(goal, sp->ship.pos);
    vec2 control_target = goal;
    vec2 to_control;
    vec2 forward = ship_forward(sp->ship.angle);
    vec2 right = v2(-forward.y, forward.x);
    float speed = sqrtf(v2_len_sq(sp->ship.vel));
    float max_hull = ship_max_hull(&sp->ship);
    float goal_angle = atan2f(to_goal.y, to_goal.x);
    float heading_diff = wrap_angle(goal_angle - sp->ship.angle);
    float control_angle;
    float control_heading_diff;

    if (path != NULL && path->count > 0 && path->current < path->count) {
        control_target = path->waypoints[path->current];
    }
    to_control = v2_sub(control_target, sp->ship.pos);
    control_angle = atan2f(to_control.y, to_control.x);
    control_heading_diff = wrap_angle(control_angle - sp->ship.angle);

    memset(row, 0, sizeof(*row));
    row->episode = episode;
    row->tick = tick;
    row->seed = config->seed;
    row->shard_index = config->shard_index;
    row->shard_total = config->shard_total;
    row->action = action;
    row->action_name = keys->name;
    row->turn_key = keys->turn;
    row->thrust_key = keys->thrust;
    row->candidate_mask = candidate_action_mask();
    row->dist = sqrtf(v2_len_sq(to_goal));
    row->control_dist = sqrtf(v2_len_sq(to_control));
    row->heading_cos = cosf(heading_diff);
    row->heading_sin = sinf(heading_diff);
    row->control_heading_cos = cosf(control_heading_diff);
    row->control_heading_sin = sinf(control_heading_diff);
    row->speed = speed;
    row->forward_speed = v2_dot(sp->ship.vel, forward);
    row->lateral_speed = v2_dot(sp->ship.vel, right);
    row->brake_distance = (speed * speed) / (2.0f * SHIP_BRAKE);
    row->fwd_clear = trace_forward_clearance(w,
                                             sp->ship.pos,
                                             sp->ship.vel,
                                             hull->ship_radius,
                                             sp->ship.angle);
    row->left_clear = trace_forward_clearance(w,
                                              sp->ship.pos,
                                              sp->ship.vel,
                                              hull->ship_radius,
                                              sp->ship.angle + 0.7f);
    row->right_clear = trace_forward_clearance(w,
                                               sp->ship.pos,
                                               sp->ship.vel,
                                               hull->ship_radius,
                                               sp->ship.angle - 0.7f);
    row->vel_clear = speed > 0.5f
                   ? trace_forward_clearance(w,
                                             sp->ship.pos,
                                             sp->ship.vel,
                                             hull->ship_radius,
                                             atan2f(sp->ship.vel.y, sp->ship.vel.x))
                   : row->fwd_clear;
    row->signal = signal_strength_at(w, sp->ship.pos);
    row->hull_ratio = max_hull > 0.0f ? sp->ship.hull / max_hull : 1.0f;
    row->path_count = path != NULL ? path->count : 0;
    row->path_current = path != NULL ? path->current : 0;
    row->x = sp->ship.pos.x;
    row->y = sp->ship.pos.y;
    row->vx = sp->ship.vel.x;
    row->vy = sp->ship.vel.y;
    row->angle = sp->ship.angle;
    row->goal_x = goal.x;
    row->goal_y = goal.y;
    row->control_x = control_target.x;
    row->control_y = control_target.y;
}

static void print_csv_header(FILE *out)
{
    fputs("schema,episode,tick,seed,shard_index,shard_total,"
          "action,action_name,turn_key,thrust_key,candidate_mask,"
          "teacher_turn,teacher_thrust,"
          "dist,control_dist,heading_cos,heading_sin,"
          "control_heading_cos,control_heading_sin,"
          "speed,forward_speed,lateral_speed,brake_distance,"
          "fwd_clear,left_clear,right_clear,vel_clear,signal,hull_ratio,"
          "path_count,path_current,progress,next_dist,next_speed,"
          "x,y,vx,vy,angle,goal_x,goal_y,control_x,control_y\n",
          out);
}

static void print_counterfactual_csv_header(FILE *out)
{
    fputs("schema,episode,tick,seed,shard_index,shard_total,"
          "action,action_name,turn_key,thrust_key,candidate_mask,"
          "teacher_turn,teacher_thrust,"
          "dist,control_dist,heading_cos,heading_sin,"
          "control_heading_cos,control_heading_sin,"
          "speed,forward_speed,lateral_speed,brake_distance,"
          "fwd_clear,left_clear,right_clear,vel_clear,signal,hull_ratio,"
          "path_count,path_current,progress,next_dist,next_speed,"
          "x,y,vx,vy,angle,goal_x,goal_y,control_x,control_y,"
          "group_id,horizon_ticks,utility,damage_events,death_events,"
          "damage_amount,hull_loss,end_hull_ratio,certificate_status,proof_kind\n",
          out);
}

static void print_csv_row(FILE *out, const flight_trace_row_t *row)
{
    fprintf(out,
            "signal.flight_trace.v1,%d,%d,%u,%d,%d,"
            "%d,%s,%d,%d,%u,"
            "%.6f,%.6f,"
            "%.3f,%.3f,%.6f,%.6f,"
            "%.6f,%.6f,"
            "%.3f,%.3f,%.3f,%.3f,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%d,%d,%.3f,%.3f,%.3f,"
            "%.3f,%.3f,%.3f,%.3f,%.6f,%.3f,%.3f,%.3f,%.3f\n",
            row->episode,
            row->tick,
            row->seed,
            row->shard_index,
            row->shard_total,
            row->action,
            row->action_name,
            row->turn_key,
            row->thrust_key,
            (unsigned)row->candidate_mask,
            row->teacher_turn,
            row->teacher_thrust,
            row->dist,
            row->control_dist,
            row->heading_cos,
            row->heading_sin,
            row->control_heading_cos,
            row->control_heading_sin,
            row->speed,
            row->forward_speed,
            row->lateral_speed,
            row->brake_distance,
            row->fwd_clear,
            row->left_clear,
            row->right_clear,
            row->vel_clear,
            row->signal,
            row->hull_ratio,
            row->path_count,
            row->path_current,
            row->progress,
            row->next_dist,
            row->next_speed,
            row->x,
            row->y,
            row->vx,
            row->vy,
            row->angle,
            row->goal_x,
            row->goal_y,
            row->control_x,
            row->control_y);
}

static void print_jsonl_row(FILE *out, const flight_trace_row_t *row)
{
    fprintf(out,
            "{\"schema\":\"signal.flight_trace.v1\","
            "\"episode\":%d,\"tick\":%d,\"seed\":%u,"
            "\"shard_index\":%d,\"shard_total\":%d,"
            "\"action\":%d,\"action_name\":\"%s\","
            "\"turn_key\":%d,\"thrust_key\":%d,\"candidate_mask\":%u,"
            "\"teacher_turn\":%.6f,\"teacher_thrust\":%.6f,"
            "\"dist\":%.3f,\"control_dist\":%.3f,"
            "\"heading_cos\":%.6f,\"heading_sin\":%.6f,"
            "\"control_heading_cos\":%.6f,\"control_heading_sin\":%.6f,"
            "\"speed\":%.3f,\"forward_speed\":%.3f,"
            "\"lateral_speed\":%.3f,\"brake_distance\":%.3f,"
            "\"fwd_clear\":%.6f,\"left_clear\":%.6f,"
            "\"right_clear\":%.6f,\"vel_clear\":%.6f,"
            "\"signal\":%.6f,\"hull_ratio\":%.6f,"
            "\"path_count\":%d,\"path_current\":%d,"
            "\"progress\":%.3f,\"next_dist\":%.3f,\"next_speed\":%.3f,"
            "\"x\":%.3f,\"y\":%.3f,\"vx\":%.3f,\"vy\":%.3f,"
            "\"angle\":%.6f,\"goal_x\":%.3f,\"goal_y\":%.3f,"
            "\"control_x\":%.3f,\"control_y\":%.3f}\n",
            row->episode,
            row->tick,
            row->seed,
            row->shard_index,
            row->shard_total,
            row->action,
            row->action_name,
            row->turn_key,
            row->thrust_key,
            (unsigned)row->candidate_mask,
            row->teacher_turn,
            row->teacher_thrust,
            row->dist,
            row->control_dist,
            row->heading_cos,
            row->heading_sin,
            row->control_heading_cos,
            row->control_heading_sin,
            row->speed,
            row->forward_speed,
            row->lateral_speed,
            row->brake_distance,
            row->fwd_clear,
            row->left_clear,
            row->right_clear,
            row->vel_clear,
            row->signal,
            row->hull_ratio,
            row->path_count,
            row->path_current,
            row->progress,
            row->next_dist,
            row->next_speed,
            row->x,
            row->y,
            row->vx,
            row->vy,
            row->angle,
            row->goal_x,
            row->goal_y,
            row->control_x,
            row->control_y);
}

static void print_counterfactual_csv_row(FILE *out, const flight_trace_row_t *row)
{
    fprintf(out,
            "signal.flight_counterfactual.v1,%d,%d,%u,%d,%d,"
            "%d,%s,%d,%d,%u,"
            "%.6f,%.6f,"
            "%.3f,%.3f,%.6f,%.6f,"
            "%.6f,%.6f,"
            "%.3f,%.3f,%.3f,%.3f,"
            "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
            "%d,%d,%.3f,%.3f,%.3f,"
            "%.3f,%.3f,%.3f,%.3f,%.6f,%.3f,%.3f,%.3f,%.3f,"
            "%" PRIu64 ",%d,%.6f,%d,%d,%.6f,%.6f,%.6f,certified,deterministic_authoritative_replay\n",
            row->episode,
            row->tick,
            row->seed,
            row->shard_index,
            row->shard_total,
            row->action,
            row->action_name,
            row->turn_key,
            row->thrust_key,
            (unsigned)row->candidate_mask,
            row->teacher_turn,
            row->teacher_thrust,
            row->dist,
            row->control_dist,
            row->heading_cos,
            row->heading_sin,
            row->control_heading_cos,
            row->control_heading_sin,
            row->speed,
            row->forward_speed,
            row->lateral_speed,
            row->brake_distance,
            row->fwd_clear,
            row->left_clear,
            row->right_clear,
            row->vel_clear,
            row->signal,
            row->hull_ratio,
            row->path_count,
            row->path_current,
            row->progress,
            row->next_dist,
            row->next_speed,
            row->x,
            row->y,
            row->vx,
            row->vy,
            row->angle,
            row->goal_x,
            row->goal_y,
            row->control_x,
            row->control_y,
            row->group_id,
            row->horizon_ticks,
            row->utility,
            row->damage_events,
            row->death_events,
            row->damage_amount,
            row->hull_loss,
            row->end_hull_ratio);
}

static void reset_trace_player(world_t *w, server_player_t *sp)
{
    ship_cleanup(&sp->ship);
    memset(sp, 0, sizeof(*sp));
    player_init_ship(sp, w);
    sp->id = 0;
    sp->connected = true;
    sp->docked = false;
    sp->autopilot_mode = 0;
    sp->was_in_signal = true;
    sp->boost_hold_timer = 0.0f;
    memset(&sp->input, 0, sizeof(sp->input));
}

static int run_trace(const flight_trace_config_t *config, FILE *out)
{
    world_t w = {0};
    server_player_t *sp = &w.players[0];
    uint32_t rng = config->seed ^ 0xa5a55a5au;

    w.rng = config->seed;
    world_reset(&w);
    for (int n = 0; n < MAX_NPC_SHIPS; n++) {
        w.npc_ships[n].active = false;
    }
    reset_trace_player(&w, sp);

    if (config->format == TRACE_FORMAT_CSV) {
        print_csv_header(out);
    }

    for (int episode = 0; episode < config->episodes; episode++) {
        if ((episode % config->shard_total) != config->shard_index) {
            continue;
        }

        station_t *station = &w.stations[(int)(rng_next(&rng) % 3u)];
        vec2 goal;
        nav_path_t path = {0};

        reset_trace_player(&w, sp);
        sp->ship.pos = random_station_nearby(&rng,
                                             station,
                                             config->spawn_min,
                                             config->spawn_max);
        sp->ship.vel = v2(rng_range(&rng, -70.0f, 70.0f),
                          rng_range(&rng, -70.0f, 70.0f));
        sp->ship.angle = rng_range(&rng, -PI_F, PI_F);
        sp->ship.hull = ship_max_hull(&sp->ship);
        sp->ship.towed_count = 0;
        sp->ship.towed_scaffold = -1;
        sp->docked = false;
        sp->boost_hold_timer = 0.0f;
        memset(&sp->input, 0, sizeof(sp->input));

        goal = random_station_nearby(&rng,
                                     station,
                                     config->goal_min,
                                     config->goal_max);

        for (int tick = 0; tick < config->ticks; tick++) {
            const hull_def_t *hull = ship_hull_def(&sp->ship);
            vec2 to_goal = v2_sub(goal, sp->ship.pos);
            float dist = sqrtf(v2_len_sq(to_goal));
            flight_cmd_t teacher;
            int action;
            const flight_trace_action_t *keys;
            vec2 control_target;
            vec2 to_control;
            float control_dist;
            vec2 forward;
            vec2 right;
            float speed;
            float max_hull;
            float goal_angle;
            float heading_diff;
            float control_angle;
            float control_heading_diff;
            float fwd_clear;
            float left_clear;
            float right_clear;
            float vel_clear;
            float next_dist;
            float next_speed;
            flight_trace_row_t row;

            if (dist < 80.0f || sp->ship.hull <= 0.0f) {
                break;
            }

            teacher = flight_steer_to(&w,
                                      &sp->ship,
                                      &path,
                                      goal,
                                      config->standoff,
                                      config->max_speed,
                                      SIM_DT);
            action = quantize_action(teacher);
            keys = &FLIGHT_TRACE_ACTIONS[action];

            control_target = goal;
            if (path.count > 0 && path.current < path.count) {
                control_target = path.waypoints[path.current];
            }
            to_control = v2_sub(control_target, sp->ship.pos);
            control_dist = sqrtf(v2_len_sq(to_control));
            forward = ship_forward(sp->ship.angle);
            right = v2(-forward.y, forward.x);
            speed = sqrtf(v2_len_sq(sp->ship.vel));
            max_hull = ship_max_hull(&sp->ship);
            goal_angle = atan2f(to_goal.y, to_goal.x);
            heading_diff = wrap_angle(goal_angle - sp->ship.angle);
            control_angle = atan2f(to_control.y, to_control.x);
            control_heading_diff = wrap_angle(control_angle - sp->ship.angle);
            fwd_clear = trace_forward_clearance(&w,
                                                sp->ship.pos,
                                                sp->ship.vel,
                                                hull->ship_radius,
                                                sp->ship.angle);
            left_clear = trace_forward_clearance(&w,
                                                 sp->ship.pos,
                                                 sp->ship.vel,
                                                 hull->ship_radius,
                                                 sp->ship.angle + 0.7f);
            right_clear = trace_forward_clearance(&w,
                                                  sp->ship.pos,
                                                  sp->ship.vel,
                                                  hull->ship_radius,
                                                  sp->ship.angle - 0.7f);
            vel_clear = speed > 0.5f
                      ? trace_forward_clearance(&w,
                                                sp->ship.pos,
                                                sp->ship.vel,
                                                hull->ship_radius,
                                                atan2f(sp->ship.vel.y, sp->ship.vel.x))
                      : fwd_clear;

            memset(&row, 0, sizeof(row));
            row.episode = episode;
            row.tick = tick;
            row.seed = config->seed;
            row.shard_index = config->shard_index;
            row.shard_total = config->shard_total;
            row.action = action;
            row.action_name = keys->name;
            row.turn_key = keys->turn;
            row.thrust_key = keys->thrust;
            row.teacher_turn = teacher.turn;
            row.teacher_thrust = teacher.thrust;
            row.dist = dist;
            row.control_dist = control_dist;
            row.heading_cos = cosf(heading_diff);
            row.heading_sin = sinf(heading_diff);
            row.control_heading_cos = cosf(control_heading_diff);
            row.control_heading_sin = sinf(control_heading_diff);
            row.speed = speed;
            row.forward_speed = v2_dot(sp->ship.vel, forward);
            row.lateral_speed = v2_dot(sp->ship.vel, right);
            row.brake_distance = (speed * speed) / (2.0f * SHIP_BRAKE);
            row.fwd_clear = fwd_clear;
            row.left_clear = left_clear;
            row.right_clear = right_clear;
            row.vel_clear = vel_clear;
            row.signal = signal_strength_at(&w, sp->ship.pos);
            row.hull_ratio = max_hull > 0.0f ? sp->ship.hull / max_hull : 1.0f;
            row.path_count = path.count;
            row.path_current = path.current;
            row.candidate_mask = candidate_action_mask();
            row.x = sp->ship.pos.x;
            row.y = sp->ship.pos.y;
            row.vx = sp->ship.vel.x;
            row.vy = sp->ship.vel.y;
            row.angle = sp->ship.angle;
            row.goal_x = goal.x;
            row.goal_y = goal.y;
            row.control_x = control_target.x;
            row.control_y = control_target.y;

            apply_trace_action(sp, action);
            world_sim_step_player_only(&w, 0, SIM_DT);

            next_dist = sqrtf(v2_dist_sq(sp->ship.pos, goal));
            next_speed = sqrtf(v2_len_sq(sp->ship.vel));
            row.progress = dist - next_dist;
            row.next_dist = next_dist;
            row.next_speed = next_speed;

            if (config->format == TRACE_FORMAT_CSV) {
                print_csv_row(out, &row);
            } else {
                print_jsonl_row(out, &row);
            }
        }
    }

    world_cleanup(&w);
    return ferror(out) ? 0 : 1;
}

static int replay_history(const flight_trace_episode_t *init,
                          const int *history,
                          int history_count,
                          world_t *w,
                          server_player_t **out_sp)
{
    server_player_t *sp = NULL;

    setup_trace_world(init, w, &sp);
    for (int i = 0; i < history_count; i++) {
        flight_trace_pain_t pain = {0};
        apply_trace_action(sp, history[i]);
        world_sim_step(w, SIM_DT);
        scan_pain_events(w, &pain);
        if (pain.death_events > 0 || sp->docked || sp->ship.hull <= 0.0f) {
            if (out_sp != NULL) *out_sp = sp;
            return 0;
        }
    }

    if (out_sp != NULL) {
        *out_sp = sp;
    }
    return 1;
}

static int evaluate_counterfactual_action(const flight_trace_config_t *config,
                                          const flight_trace_episode_t *init,
                                          const int *history,
                                          int history_count,
                                          int episode,
                                          int tick,
                                          int action,
                                          uint64_t group_id,
                                          flight_trace_row_t *row)
{
    world_t w = {0};
    server_player_t *sp = NULL;
    nav_path_t path = {0};
    const hull_def_t *hull;
    float start_dist;
    float start_hull;
    float max_hull;
    float forward_flight_score;
    float reverse_flight_penalty;
    float lateral_flight_penalty;
    float wall_flight_penalty;
    flight_trace_pain_t pain = {0};

    if (!replay_history(init, history, history_count, &w, &sp)) {
        world_cleanup(&w);
        return 0;
    }

    hull = ship_hull_def(&sp->ship);
    fill_state_row(config, &w, sp, &path, init->goal, episode, tick, action, row);
    start_dist = row->dist;
    max_hull = ship_max_hull(&sp->ship);
    start_hull = sp->ship.hull;

    for (int h = 0; h < config->horizon_ticks; h++) {
        apply_trace_action(sp, action);
        world_sim_step(&w, SIM_DT);
        scan_pain_events(&w, &pain);
        if (pain.death_events > 0 || sp->docked || sp->ship.hull <= 0.0f) {
            break;
        }
    }

    row->next_dist = sqrtf(v2_dist_sq(sp->ship.pos, init->goal));
    row->next_speed = sqrtf(v2_len_sq(sp->ship.vel));
    row->progress = start_dist - row->next_dist;
    row->group_id = group_id;
    row->horizon_ticks = config->horizon_ticks;
    row->damage_events = pain.damage_events;
    row->death_events = pain.death_events;
    row->damage_amount = pain.damage_amount;
    row->end_hull_ratio = max_hull > 0.0f ? sp->ship.hull / max_hull : 1.0f;
    row->hull_loss = start_hull - sp->ship.hull;
    if (row->hull_loss < 0.0f || pain.death_events > 0) {
        row->hull_loss = pain.death_events > 0 ? start_hull : 0.0f;
    }
    {
        vec2 end_to_goal = v2_sub(init->goal, sp->ship.pos);
        vec2 end_forward = ship_forward(sp->ship.angle);
        float end_dist = sqrtf(v2_len_sq(end_to_goal));
        float goal_dir_x = end_dist > 0.001f ? end_to_goal.x / end_dist : end_forward.x;
        float goal_dir_y = end_dist > 0.001f ? end_to_goal.y / end_dist : end_forward.y;
        float heading_cos = end_forward.x * goal_dir_x + end_forward.y * goal_dir_y;
        float closing_speed = sp->ship.vel.x * goal_dir_x + sp->ship.vel.y * goal_dir_y;
        float forward_speed = v2_dot(sp->ship.vel, end_forward);
        float lateral_speed_sq = row->next_speed * row->next_speed - closing_speed * closing_speed;
        float lateral_speed = lateral_speed_sq > 0.0f ? sqrtf(lateral_speed_sq) : 0.0f;
        float positive_progress = row->progress > 0.0f ? row->progress : 0.0f;
        float wall_clearance = trace_wall_clearance(&w,
                                                    sp->ship.pos,
                                                    sp->ship.vel,
                                                    hull->ship_radius,
                                                    sp->ship.angle);
        float speed_ratio = config->max_speed > 0.0f
                          ? fminf(row->next_speed / config->max_speed, 1.5f)
                          : 0.0f;

        forward_flight_score =
            (positive_progress / config->utility_progress_scale) *
            fmaxf(0.0f, heading_cos) * config->utility_forward_bonus;
        reverse_flight_penalty =
            (fmaxf(0.0f, -forward_speed) / config->max_speed) *
            config->utility_reverse_penalty;
        reverse_flight_penalty +=
            (positive_progress / config->utility_progress_scale) *
            fmaxf(0.0f, -heading_cos) * config->utility_reverse_penalty;
        reverse_flight_penalty +=
            (fmaxf(0.0f, -closing_speed) / config->max_speed) *
            config->utility_reverse_penalty;
        lateral_flight_penalty =
            (lateral_speed / config->max_speed) * config->utility_lateral_penalty;
        wall_flight_penalty =
            (1.0f - wall_clearance) * speed_ratio * config->utility_wall_penalty;
    }

    row->utility = (row->progress / config->utility_progress_scale) +
                   forward_flight_score -
                   reverse_flight_penalty -
                   lateral_flight_penalty -
                   wall_flight_penalty -
                   (pain.damage_amount * config->utility_damage_penalty) -
                   ((float)pain.death_events * config->utility_death_penalty);

    world_cleanup(&w);
    return 1;
}

static int run_counterfactual_trace(const flight_trace_config_t *config, FILE *out)
{
    uint32_t rng = config->seed ^ 0xc0ffee11u;
    int *history = NULL;

    if (config->format != TRACE_FORMAT_CSV) {
        fprintf(stderr, "flight_trace: counterfactual mode currently emits csv only\n");
        return 0;
    }

    history = calloc((size_t)config->ticks, sizeof(*history));
    if (history == NULL) {
        return 0;
    }

    print_counterfactual_csv_header(out);

    for (int episode = 0; episode < config->episodes; episode++) {
        flight_trace_episode_t init;
        world_t main_world = {0};
        server_player_t *sp = NULL;
        int history_count = 0;

        if ((episode % config->shard_total) != config->shard_index) {
            continue;
        }

        init_trace_episode(config, episode, &rng, &init);
        setup_trace_world(&init, &main_world, &sp);

        for (int tick = 0; tick < config->ticks; tick++) {
            float dist = sqrtf(v2_dist_sq(sp->ship.pos, init.goal));
            flight_trace_pain_t pain = {0};
            int action;

            if (dist < 80.0f || sp->ship.hull <= 0.0f || sp->docked) {
                break;
            }

            if ((tick % config->sample_stride) == 0) {
                uint64_t group_id =
                    ((uint64_t)(uint32_t)episode << 32) | (uint32_t)tick;
                for (int candidate = 0; candidate < FLIGHT_TRACE_ACTION_COUNT; candidate++) {
                    flight_trace_row_t row;
                    if (evaluate_counterfactual_action(config,
                                                       &init,
                                                       history,
                                                       history_count,
                                                       episode,
                                                       tick,
                                                       candidate,
                                                       group_id,
                                                       &row)) {
                        print_counterfactual_csv_row(out, &row);
                    }
                }
            }

            action = choose_explore_action(config, &rng, sp, init.goal);
            history[history_count++] = action;
            apply_trace_action(sp, action);
            world_sim_step(&main_world, SIM_DT);
            scan_pain_events(&main_world, &pain);
            if (pain.death_events > 0) {
                break;
            }
        }

        world_cleanup(&main_world);
    }

    free(history);
    return ferror(out) ? 0 : 1;
}

int main(int argc, char **argv)
{
    flight_trace_config_t config;
    FILE *out = NULL;
    int ok;

    if (!parse_args(argc, argv, &config)) {
        print_usage(stderr);
        return 2;
    }

    out = config.out_path != NULL ? fopen(config.out_path, "w") : stdout;
    if (out == NULL) {
        perror("flight_trace: open output");
        return 1;
    }

    ok = config.counterfactual ? run_counterfactual_trace(&config, out)
                               : run_trace(&config, out);
    if (config.out_path != NULL && fclose(out) != 0) {
        perror("flight_trace: close output");
        return 1;
    }

    if (!ok) {
        fprintf(stderr, "flight_trace: write failed\n");
        return 1;
    }
    return 0;
}
