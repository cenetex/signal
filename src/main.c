#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SOKOL_IMPL
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_GL_IMPL
#define SOKOL_DEBUGTEXT_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "sokol_log.h"

enum {
    KEY_COUNT = 512,
    MAX_ASTEROIDS = 48,
    MAX_STARS = 120,
};

enum {
    STATION_SERVICE_MARKET = 1 << 0,
    STATION_SERVICE_REPAIR = 1 << 1,
    STATION_SERVICE_OUTFIT = 1 << 2,
};

typedef struct {
    float x;
    float y;
} vec2;

typedef struct {
    vec2 pos;
    vec2 vel;
    float angle;
    float hull;
    float cargo;
    float credits;
    int mining_level;
    int hold_level;
    int tractor_level;
} ship_t;

typedef struct {
    char name[32];
    vec2 pos;
    float radius;
    float dock_radius;
    float ore_price;
    uint32_t services;
} station_t;

typedef enum {
    ASTEROID_TIER_XL,
    ASTEROID_TIER_L,
    ASTEROID_TIER_M,
    ASTEROID_TIER_S,
    ASTEROID_TIER_COUNT,
} asteroid_tier_t;

typedef enum {
    SHIP_UPGRADE_MINING,
    SHIP_UPGRADE_HOLD,
    SHIP_UPGRADE_TRACTOR,
    SHIP_UPGRADE_COUNT,
} ship_upgrade_t;

typedef struct {
    bool active;
    bool fracture_child;
    asteroid_tier_t tier;
    vec2 pos;
    vec2 vel;
    float radius;
    float hp;
    float max_hp;
    float ore;
    float max_ore;
    float rotation;
    float spin;
    float seed;
    float age;
} asteroid_t;

typedef struct {
    vec2 pos;
    float depth;
    float size;
    float brightness;
} star_t;

typedef struct {
    bool key_down[KEY_COUNT];
    bool key_pressed[KEY_COUNT];
} input_state_t;

typedef struct {
    float turn;
    float thrust;
    bool mine;
    bool interact;
    bool service_sell;
    bool service_repair;
    bool upgrade_mining;
    bool upgrade_hold;
    bool upgrade_tractor;
    bool reset;
} input_intent_t;

typedef struct {
    float accumulator;
} runtime_state_t;

typedef struct {
    input_state_t input;
    ship_t ship;
    station_t station;
    asteroid_t asteroids[MAX_ASTEROIDS];
    star_t stars[MAX_STARS];
    uint32_t rng;
    int hover_asteroid;
    bool beam_active;
    bool beam_hit;
    bool thrusting;
    bool in_dock_range;
    bool docked;
    vec2 beam_start;
    vec2 beam_end;
    char notice[128];
    float notice_timer;
    int nearby_fragments;
    int tractor_fragments;
    float collection_feedback_ore;
    int collection_feedback_fragments;
    float collection_feedback_timer;
    float time;
    float field_spawn_timer;
    runtime_state_t runtime;
    sg_pass_action pass_action;
} game_t;

static game_t g;

static const float PI_F = 3.14159265359f;
static const float TWO_PI_F = 6.28318530718f;
static const float WORLD_RADIUS = 2200.0f;
static const float SHIP_RADIUS = 16.0f;
static const float SHIP_ACCEL = 300.0f;
static const float SHIP_BRAKE = 180.0f;
static const float SHIP_TURN_SPEED = 2.75f;
static const float SHIP_DRAG = 0.45f;
static const float SHIP_BASE_HULL = 100.0f;
static const float SHIP_BASE_CARGO_MAX = 120.0f;
static const float SHIP_HOLD_UPGRADE_STEP = 24.0f;
static const float MINING_RANGE = 170.0f;
static const float SHIP_BASE_MINING_RATE = 28.0f;
static const float SHIP_MINING_UPGRADE_STEP = 7.0f;
static const float STATION_DEFAULT_ORE_PRICE = 12.0f;
static const float HUD_MARGIN = 28.0f;
static const float HUD_CELL = 8.0f;
static const float SIM_DT = 1.0f / 120.0f;
static const int MAX_SIM_STEPS_PER_FRAME = 8;
static const int FIELD_ASTEROID_TARGET = 16;
static const float FIELD_ASTEROID_RESPAWN_DELAY = 1.4f;
static const float FRACTURE_CHILD_CLEANUP_AGE = 22.0f;
static const float FRACTURE_CHILD_CLEANUP_DISTANCE = 940.0f;
static const float FRAGMENT_NEARBY_RANGE = 220.0f;
static const float SHIP_BASE_TRACTOR_RANGE = 150.0f;
static const float SHIP_TRACTOR_UPGRADE_STEP = 24.0f;
static const float SHIP_BASE_COLLECT_RADIUS = 30.0f;
static const float SHIP_COLLECT_UPGRADE_STEP = 5.0f;
static const float FRAGMENT_TRACTOR_ACCEL = 380.0f;
static const float FRAGMENT_MAX_SPEED = 210.0f;
static const float COLLECTION_FEEDBACK_TIME = 1.1f;
static const int SHIP_UPGRADE_MAX_LEVEL = 3;
static const float STATION_REPAIR_COST_PER_HULL = 2.0f;
static const float STATION_DOCK_APPROACH_OFFSET = 34.0f;
static const float SHIP_COLLISION_DAMAGE_THRESHOLD = 115.0f;
static const float SHIP_COLLISION_DAMAGE_SCALE = 0.12f;

static float clampf(float value, float min_value, float max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static vec2 v2(float x, float y) {
    vec2 result = { x, y };
    return result;
}

static vec2 v2_add(vec2 a, vec2 b) {
    return v2(a.x + b.x, a.y + b.y);
}

static vec2 v2_sub(vec2 a, vec2 b) {
    return v2(a.x - b.x, a.y - b.y);
}

static vec2 v2_scale(vec2 value, float scale) {
    return v2(value.x * scale, value.y * scale);
}

static vec2 v2_perp(vec2 value) {
    return v2(-value.y, value.x);
}

static float v2_dist_sq(vec2 a, vec2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

static float v2_dot(vec2 a, vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

static float v2_cross(vec2 a, vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

static float v2_len_sq(vec2 value) {
    return v2_dot(value, value);
}

static float v2_len(vec2 value) {
    return sqrtf(v2_len_sq(value));
}

static vec2 v2_norm(vec2 value) {
    float len = v2_len(value);
    if (len > 0.00001f) {
        return v2_scale(value, 1.0f / len);
    }
    return v2(1.0f, 0.0f);
}

static vec2 v2_from_angle(float angle) {
    return v2(cosf(angle), sinf(angle));
}

static float wrap_angle(float angle) {
    while (angle > PI_F) {
        angle -= TWO_PI_F;
    }
    while (angle < -PI_F) {
        angle += TWO_PI_F;
    }
    return angle;
}

static uint32_t rng_next(void) {
    if (g.rng == 0) {
        g.rng = 0xA341316Cu;
    }
    uint32_t x = g.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.rng = x;
    return x;
}

static float randf(void) {
    return (float)(rng_next() & 0x00FFFFFFu) / 16777215.0f;
}

static float rand_range(float min_value, float max_value) {
    return lerpf(min_value, max_value, randf());
}

static int rand_int(int min_value, int max_value) {
    return min_value + (int)(rng_next() % (uint32_t)(max_value - min_value + 1));
}

// Drop transient and held input when the app loses focus.
static void clear_input_state(void) {
    memset(g.input.key_down, 0, sizeof(g.input.key_down));
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
}

static void consume_pressed_input(void) {
    memset(g.input.key_pressed, 0, sizeof(g.input.key_pressed));
}

static void set_notice(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g.notice, sizeof(g.notice), fmt, args);
    va_end(args);
    g.notice_timer = 3.0f;
}

static asteroid_tier_t asteroid_next_tier(asteroid_tier_t tier) {
    if (tier >= ASTEROID_TIER_S) {
        return ASTEROID_TIER_S;
    }
    return (asteroid_tier_t)(tier + 1);
}

static bool asteroid_is_collectible(const asteroid_t* asteroid) {
    return asteroid->active && (asteroid->tier == ASTEROID_TIER_S);
}

static float asteroid_progress_ratio(const asteroid_t* asteroid) {
    if (asteroid_is_collectible(asteroid) && (asteroid->max_ore > 0.0f)) {
        return clampf(asteroid->ore / asteroid->max_ore, 0.0f, 1.0f);
    }
    if (asteroid->max_hp > 0.0f) {
        return clampf(asteroid->hp / asteroid->max_hp, 0.0f, 1.0f);
    }
    return 0.0f;
}

static float ship_max_hull(void) {
    return SHIP_BASE_HULL;
}

static float ship_cargo_capacity(void) {
    return SHIP_BASE_CARGO_MAX + ((float)g.ship.hold_level * SHIP_HOLD_UPGRADE_STEP);
}

static float ship_mining_rate(void) {
    return SHIP_BASE_MINING_RATE + ((float)g.ship.mining_level * SHIP_MINING_UPGRADE_STEP);
}

static float ship_tractor_range(void) {
    return SHIP_BASE_TRACTOR_RANGE + ((float)g.ship.tractor_level * SHIP_TRACTOR_UPGRADE_STEP);
}

static float ship_collect_radius(void) {
    return SHIP_BASE_COLLECT_RADIUS + ((float)g.ship.tractor_level * SHIP_COLLECT_UPGRADE_STEP);
}

static int ship_upgrade_level(ship_upgrade_t upgrade) {
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return g.ship.mining_level;
        case SHIP_UPGRADE_HOLD:
            return g.ship.hold_level;
        case SHIP_UPGRADE_TRACTOR:
            return g.ship.tractor_level;
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

static bool ship_upgrade_maxed(ship_upgrade_t upgrade) {
    return ship_upgrade_level(upgrade) >= SHIP_UPGRADE_MAX_LEVEL;
}

static int ship_upgrade_cost(ship_upgrade_t upgrade) {
    int level = ship_upgrade_level(upgrade);
    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            return 110 + (level * 120);
        case SHIP_UPGRADE_HOLD:
            return 135 + (level * 145);
        case SHIP_UPGRADE_TRACTOR:
            return 95 + (level * 125);
        case SHIP_UPGRADE_COUNT:
        default:
            return 0;
    }
}

static vec2 station_dock_anchor(void) {
    return v2_add(g.station.pos, v2(0.0f, -(g.station.radius + SHIP_RADIUS + STATION_DOCK_APPROACH_OFFSET)));
}

static bool station_has_service(uint32_t service) {
    return (g.station.services & service) != 0;
}

static float station_cargo_sale_value(void) {
    return g.ship.cargo * g.station.ore_price;
}

static float station_repair_cost(void) {
    float missing_hull = fmaxf(0.0f, ship_max_hull() - g.ship.hull);
    return ceilf(missing_hull * STATION_REPAIR_COST_PER_HULL);
}

static void apply_ship_damage(float damage);

static float ship_cargo_space(void) {
    return fmaxf(0.0f, ship_cargo_capacity() - g.ship.cargo);
}

static void clear_collection_feedback(void) {
    g.collection_feedback_ore = 0.0f;
    g.collection_feedback_fragments = 0;
    g.collection_feedback_timer = 0.0f;
}

static void push_collection_feedback(float recovered_ore, int recovered_fragments) {
    if (recovered_ore <= 0.0f) {
        return;
    }
    if (g.collection_feedback_timer <= 0.0f) {
        clear_collection_feedback();
    }
    g.collection_feedback_ore += recovered_ore;
    g.collection_feedback_fragments += recovered_fragments;
    g.collection_feedback_timer = COLLECTION_FEEDBACK_TIME;
}

static const char* asteroid_tier_name(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return "XL";
        case ASTEROID_TIER_L:
            return "L";
        case ASTEROID_TIER_M:
            return "M";
        case ASTEROID_TIER_S:
            return "S";
        default:
            return "?";
    }
}

static const char* asteroid_tier_kind(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return "rock";
        case ASTEROID_TIER_L:
            return "chunk";
        case ASTEROID_TIER_M:
            return "shard";
        case ASTEROID_TIER_S:
            return "fragment";
        default:
            return "piece";
    }
}

static float asteroid_spin_limit(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return 0.16f;
        case ASTEROID_TIER_L:
            return 0.24f;
        case ASTEROID_TIER_M:
            return 0.38f;
        case ASTEROID_TIER_S:
            return 0.62f;
        default:
            return 0.2f;
    }
}

static float asteroid_radius_min(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return 54.0f;
        case ASTEROID_TIER_L:
            return 34.0f;
        case ASTEROID_TIER_M:
            return 20.0f;
        case ASTEROID_TIER_S:
            return 11.0f;
        default:
            return 16.0f;
    }
}

static float asteroid_radius_max(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return 78.0f;
        case ASTEROID_TIER_L:
            return 48.0f;
        case ASTEROID_TIER_M:
            return 30.0f;
        case ASTEROID_TIER_S:
            return 16.0f;
        default:
            return 18.0f;
    }
}

static float asteroid_hp_min(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return 120.0f;
        case ASTEROID_TIER_L:
            return 68.0f;
        case ASTEROID_TIER_M:
            return 32.0f;
        case ASTEROID_TIER_S:
            return 10.0f;
        default:
            return 8.0f;
    }
}

static float asteroid_hp_max(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return 170.0f;
        case ASTEROID_TIER_L:
            return 96.0f;
        case ASTEROID_TIER_M:
            return 46.0f;
        case ASTEROID_TIER_S:
            return 18.0f;
        default:
            return 12.0f;
    }
}

static asteroid_tier_t random_field_asteroid_tier(void) {
    float roll = randf();
    if (roll < 0.24f) {
        return ASTEROID_TIER_XL;
    }
    if (roll < 0.70f) {
        return ASTEROID_TIER_L;
    }
    return ASTEROID_TIER_M;
}

static void clear_asteroid(asteroid_t* asteroid) {
    memset(asteroid, 0, sizeof(*asteroid));
}

static void configure_asteroid_tier(asteroid_t* asteroid, asteroid_tier_t tier) {
    float spin_limit = asteroid_spin_limit(tier);
    asteroid->active = true;
    asteroid->tier = tier;
    asteroid->radius = rand_range(asteroid_radius_min(tier), asteroid_radius_max(tier));
    asteroid->max_hp = rand_range(asteroid_hp_min(tier), asteroid_hp_max(tier));
    asteroid->hp = asteroid->max_hp;
    asteroid->max_ore = 0.0f;
    asteroid->ore = 0.0f;
    if (tier == ASTEROID_TIER_S) {
        asteroid->max_ore = rand_range(8.0f, 14.0f);
        asteroid->ore = asteroid->max_ore;
    }
    asteroid->rotation = rand_range(0.0f, TWO_PI_F);
    asteroid->spin = rand_range(-spin_limit, spin_limit);
    asteroid->seed = rand_range(0.0f, 100.0f);
    asteroid->age = 0.0f;
}

static void spawn_field_asteroid_of_tier(asteroid_t* asteroid, asteroid_tier_t tier) {
    float distance = rand_range(420.0f, WORLD_RADIUS - 180.0f);
    float angle = rand_range(0.0f, TWO_PI_F);
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier);
    asteroid->fracture_child = false;
    asteroid->pos = v2(cosf(angle) * distance, sinf(angle) * distance);
    asteroid->vel = v2(rand_range(-4.0f, 4.0f), rand_range(-4.0f, 4.0f));
}

static void spawn_field_asteroid(asteroid_t* asteroid) {
    spawn_field_asteroid_of_tier(asteroid, random_field_asteroid_tier());
}

static void spawn_child_asteroid(asteroid_t* asteroid, asteroid_tier_t tier, vec2 pos, vec2 vel) {
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier);
    asteroid->fracture_child = true;
    asteroid->pos = pos;
    asteroid->vel = vel;
}

static int desired_child_count(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XL:
            return rand_int(2, 3);
        case ASTEROID_TIER_L:
            return rand_int(2, 3);
        case ASTEROID_TIER_M:
            return rand_int(2, 4);
        case ASTEROID_TIER_S:
        default:
            return 0;
    }
}

static void inspect_asteroid_field(int* seeded_count, int* first_inactive_slot) {
    *seeded_count = 0;
    *first_inactive_slot = -1;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.asteroids[i].active) {
            if (*first_inactive_slot < 0) {
                *first_inactive_slot = i;
            }
            continue;
        }

        if (!g.asteroids[i].fracture_child) {
            (*seeded_count)++;
        }
    }
}

static void fracture_asteroid(int asteroid_index, vec2 outward_dir) {
    asteroid_t parent = g.asteroids[asteroid_index];
    asteroid_tier_t child_tier = asteroid_next_tier(parent.tier);
    int desired = desired_child_count(parent.tier);
    int child_slots[4] = { asteroid_index, -1, -1, -1 };
    int child_count = 1;

    for (int i = 0; (i < MAX_ASTEROIDS) && (child_count < desired); i++) {
        if ((i == asteroid_index) || g.asteroids[i].active) {
            continue;
        }
        child_slots[child_count++] = i;
    }

    float base_angle = atan2f(outward_dir.y, outward_dir.x);
    for (int i = 0; i < child_count; i++) {
        float spread_t = (child_count == 1) ? 0.0f : (((float)i / (float)(child_count - 1)) - 0.5f);
        float child_angle = base_angle + (spread_t * 1.35f) + rand_range(-0.14f, 0.14f);
        vec2 dir = v2_from_angle(child_angle);
        vec2 tangent = v2_perp(dir);
        asteroid_t* child = &g.asteroids[child_slots[i]];
        spawn_child_asteroid(child, child_tier, parent.pos, parent.vel);
        vec2 child_pos = v2_add(parent.pos, v2_scale(dir, (parent.radius * 0.28f) + (child->radius * 0.85f)));
        float drift = rand_range(22.0f, 56.0f);
        vec2 child_vel = v2_add(parent.vel, v2_add(v2_scale(dir, drift), v2_scale(tangent, rand_range(-10.0f, 10.0f))));
        child->pos = child_pos;
        child->vel = child_vel;
    }

    set_notice("%s %s fractured into %d %s %s%s.",
        asteroid_tier_name(parent.tier),
        asteroid_tier_kind(parent.tier),
        child_count,
        asteroid_tier_name(child_tier),
        asteroid_tier_kind(child_tier),
        child_count == 1 ? "" : "s");
}

static void maintain_asteroid_field(float dt) {
    int seeded_count = 0;
    int first_inactive_slot = -1;
    inspect_asteroid_field(&seeded_count, &first_inactive_slot);

    if (seeded_count >= FIELD_ASTEROID_TARGET) {
        g.field_spawn_timer = 0.0f;
        return;
    }

    g.field_spawn_timer += dt;
    if (g.field_spawn_timer < FIELD_ASTEROID_RESPAWN_DELAY) {
        return;
    }

    if (first_inactive_slot >= 0) {
        spawn_field_asteroid(&g.asteroids[first_inactive_slot]);
    }
    g.field_spawn_timer = 0.0f;
}

static void init_starfield(void) {
    for (int i = 0; i < MAX_STARS; i++) {
        float distance = rand_range(100.0f, WORLD_RADIUS * 2.0f);
        float angle = rand_range(0.0f, TWO_PI_F);
        g.stars[i].pos = v2(cosf(angle) * distance, sinf(angle) * distance);
        g.stars[i].depth = rand_range(0.16f, 0.9f);
        g.stars[i].size = rand_range(0.9f, 2.2f);
        g.stars[i].brightness = rand_range(0.45f, 1.0f);
    }
}

static void reset_world(void) {
    g.ship.pos = v2(0.0f, -110.0f);
    g.ship.vel = v2(0.0f, 0.0f);
    g.ship.angle = PI_F * 0.5f;
    g.ship.hull = ship_max_hull();
    g.ship.cargo = 0.0f;
    g.ship.credits = 0.0f;
    g.ship.mining_level = 0;
    g.ship.hold_level = 0;
    g.ship.tractor_level = 0;

    snprintf(g.station.name, sizeof(g.station.name), "%s", "Prospect Station");
    g.station.pos = v2(0.0f, 0.0f);
    g.station.radius = 58.0f;
    g.station.dock_radius = 125.0f;
    g.station.ore_price = STATION_DEFAULT_ORE_PRICE;
    g.station.services = STATION_SERVICE_MARKET | STATION_SERVICE_REPAIR | STATION_SERVICE_OUTFIT;

    g.hover_asteroid = -1;
    g.beam_active = false;
    g.beam_hit = false;
    g.thrusting = false;
    g.in_dock_range = true;
    g.docked = true;
    g.ship.pos = station_dock_anchor();
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    g.nearby_fragments = 0;
    g.tractor_fragments = 0;
    clear_collection_feedback();
    g.field_spawn_timer = 0.0f;

    memset(g.asteroids, 0, sizeof(g.asteroids));
    if (FIELD_ASTEROID_TARGET > 0) {
        spawn_field_asteroid_of_tier(&g.asteroids[0], ASTEROID_TIER_XL);
    }
    if (FIELD_ASTEROID_TARGET > 1) {
        spawn_field_asteroid_of_tier(&g.asteroids[1], ASTEROID_TIER_L);
    }
    for (int i = 2; i < FIELD_ASTEROID_TARGET; i++) {
        spawn_field_asteroid(&g.asteroids[i]);
    }

    set_notice("%s services online. Press E to launch.", g.station.name);
}

static void draw_circle_filled(vec2 center, float radius, int segments, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_triangles();
    for (int i = 0; i < segments; i++) {
        float a0 = ((float)i / (float)segments) * TWO_PI_F;
        float a1 = ((float)(i + 1) / (float)segments) * TWO_PI_F;
        sgl_v2f(center.x, center.y);
        sgl_v2f(center.x + cosf(a0) * radius, center.y + sinf(a0) * radius);
        sgl_v2f(center.x + cosf(a1) * radius, center.y + sinf(a1) * radius);
    }
    sgl_end();
}

static void draw_circle_outline(vec2 center, float radius, int segments, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_line_strip();
    for (int i = 0; i <= segments; i++) {
        float angle = ((float)i / (float)segments) * TWO_PI_F;
        sgl_v2f(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
    }
    sgl_end();
}

static void draw_rect_centered(vec2 center, float half_w, float half_h, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_quads();
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y + half_h);
    sgl_end();
}

static void draw_rect_outline(vec2 center, float half_w, float half_h, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_line_strip();
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_end();
}

static void draw_segment(vec2 start, vec2 end, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_lines();
    sgl_v2f(start.x, start.y);
    sgl_v2f(end.x, end.y);
    sgl_end();
}

static float asteroid_profile(const asteroid_t* asteroid, float angle) {
    float bump1 = sinf(angle * 3.0f + asteroid->seed);
    float bump2 = sinf(angle * 7.0f + asteroid->seed * 1.71f);
    float bump3 = cosf(angle * 5.0f + asteroid->seed * 0.63f);
    float profile = 1.0f + (bump1 * 0.08f) + (bump2 * 0.06f) + (bump3 * 0.04f);
    return asteroid->radius * profile;
}

static void draw_background(vec2 camera) {
    for (int i = 0; i < MAX_STARS; i++) {
        const star_t* star = &g.stars[i];
        vec2 parallax_pos = v2_add(star->pos, v2_scale(camera, 1.0f - star->depth));
        float tint = star->brightness;
        draw_rect_centered(parallax_pos, star->size, star->size, 0.65f * tint, 0.75f * tint, tint, 0.9f);
    }

    draw_circle_filled(v2_add(camera, v2(-420.0f, 310.0f)), 210.0f, 28, 0.06f, 0.08f, 0.18f, 0.24f);
    draw_circle_filled(v2_add(camera, v2(540.0f, -260.0f)), 260.0f, 28, 0.10f, 0.05f, 0.15f, 0.16f);
}

static void draw_station(void) {
    float pulse = 0.35f + (sinf(g.time * 2.5f) * 0.15f);
    float dock_alpha = g.docked ? 0.62f : (g.in_dock_range ? 0.46f : 0.15f + pulse);
    draw_circle_outline(g.station.pos, g.station.dock_radius, 48, 0.25f, 0.85f, 1.0f, dock_alpha);
    draw_circle_filled(g.station.pos, g.station.radius, 28, 0.08f, 0.12f, 0.17f, 1.0f);
    draw_circle_outline(g.station.pos, g.station.radius + 8.0f, 28, 0.45f, 0.85f, 1.0f, 0.9f);
    draw_circle_filled(g.station.pos, 18.0f, 18, 0.22f, 0.86f, 0.96f, 1.0f);

    for (int i = 0; i < 6; i++) {
        float angle = (TWO_PI_F / 6.0f) * (float)i + g.time * 0.18f;
        vec2 inner = v2_add(g.station.pos, v2(cosf(angle) * 28.0f, sinf(angle) * 28.0f));
        vec2 outer = v2_add(g.station.pos, v2(cosf(angle) * 48.0f, sinf(angle) * 48.0f));
        draw_segment(inner, outer, 0.35f, 0.78f, 1.0f, 0.85f);
    }
}

static void asteroid_tier_body_color(asteroid_tier_t tier, float hp_ratio, float* r, float* g0, float* b) {
    float base_r = 0.30f;
    float base_g = 0.31f;
    float base_b = 0.34f;

    switch (tier) {
        case ASTEROID_TIER_XL:
            base_r = 0.29f;
            base_g = 0.31f;
            base_b = 0.42f;
            break;
        case ASTEROID_TIER_L:
            base_r = 0.31f;
            base_g = 0.33f;
            base_b = 0.38f;
            break;
        case ASTEROID_TIER_M:
            base_r = 0.26f;
            base_g = 0.36f;
            base_b = 0.42f;
            break;
        case ASTEROID_TIER_S:
            base_r = 0.28f;
            base_g = 0.44f;
            base_b = 0.36f;
            break;
        default:
            break;
    }

    *r = lerpf(base_r * 0.72f, base_r * 1.16f, hp_ratio);
    *g0 = lerpf(base_g * 0.72f, base_g * 1.16f, hp_ratio);
    *b = lerpf(base_b * 0.72f, base_b * 1.16f, hp_ratio);
}

static void draw_asteroid(const asteroid_t* asteroid, bool targeted) {
    float progress_ratio = asteroid_progress_ratio(asteroid);
    float body_r = 0.3f;
    float body_g = 0.3f;
    float body_b = 0.3f;
    int segments = 18;
    asteroid_tier_body_color(asteroid->tier, progress_ratio, &body_r, &body_g, &body_b);

    switch (asteroid->tier) {
        case ASTEROID_TIER_XL:
            segments = 22;
            break;
        case ASTEROID_TIER_L:
            segments = 18;
            break;
        case ASTEROID_TIER_M:
            segments = 15;
            break;
        case ASTEROID_TIER_S:
            segments = 12;
            break;
        default:
            break;
    }

    sgl_c4f(body_r, body_g, body_b, 1.0f);
    sgl_begin_triangles();
    for (int i = 0; i < segments; i++) {
        float a0 = asteroid->rotation + ((float)i / (float)segments) * TWO_PI_F;
        float a1 = asteroid->rotation + ((float)(i + 1) / (float)segments) * TWO_PI_F;
        float r0 = asteroid_profile(asteroid, a0);
        float r1 = asteroid_profile(asteroid, a1);
        sgl_v2f(asteroid->pos.x, asteroid->pos.y);
        sgl_v2f(asteroid->pos.x + cosf(a0) * r0, asteroid->pos.y + sinf(a0) * r0);
        sgl_v2f(asteroid->pos.x + cosf(a1) * r1, asteroid->pos.y + sinf(a1) * r1);
    }
    sgl_end();

    float rim_r = targeted ? 0.45f : (body_r * 0.85f);
    float rim_g = targeted ? 0.94f : (body_g * 0.95f);
    float rim_b = targeted ? 1.0f : fminf(1.0f, body_b * 1.2f);
    float rim_a = targeted ? 1.0f : 0.8f;

    sgl_c4f(rim_r, rim_g, rim_b, rim_a);
    sgl_begin_line_strip();
    for (int i = 0; i <= segments; i++) {
        float angle = asteroid->rotation + ((float)i / (float)segments) * TWO_PI_F;
        float radius = asteroid_profile(asteroid, angle);
        sgl_v2f(asteroid->pos.x + cosf(angle) * radius, asteroid->pos.y + sinf(angle) * radius);
    }
    sgl_end();

    if (asteroid->tier == ASTEROID_TIER_S) {
        draw_circle_filled(asteroid->pos, asteroid->radius * lerpf(0.14f, 0.24f, progress_ratio), 10, 0.48f, 0.96f, 0.78f, lerpf(0.35f, 0.8f, progress_ratio));
    } else if (asteroid->tier == ASTEROID_TIER_M) {
        draw_circle_filled(asteroid->pos, asteroid->radius * 0.16f, 8, 0.36f, 0.78f, 0.98f, 0.4f);
    }

    if (targeted) {
        draw_circle_outline(asteroid->pos, asteroid->radius + 12.0f, 24, 0.35f, 1.0f, 0.92f, 0.75f);
    }
}

static void draw_ship_tractor_field(void) {
    if (g.nearby_fragments <= 0) {
        return;
    }

    float pulse = 0.28f + (sinf(g.time * 7.0f) * 0.08f);
    draw_circle_outline(g.ship.pos, ship_tractor_range(), 40, 0.24f, 0.86f, 1.0f, pulse);
    if (g.tractor_fragments > 0) {
        draw_circle_outline(g.ship.pos, ship_collect_radius() + 6.0f, 28, 0.50f, 1.0f, 0.82f, 0.75f);
    }
}

static void draw_ship(void) {
    sgl_push_matrix();
    sgl_translate(g.ship.pos.x, g.ship.pos.y, 0.0f);
    sgl_rotate(g.ship.angle, 0.0f, 0.0f, 1.0f);

    if (g.thrusting) {
        float flicker = 10.0f + sinf(g.time * 42.0f) * 3.0f;
        sgl_c4f(1.0f, 0.74f, 0.24f, 0.95f);
        sgl_begin_triangles();
        sgl_v2f(-12.0f, 0.0f);
        sgl_v2f(-26.0f - flicker, 6.0f);
        sgl_v2f(-26.0f - flicker, -6.0f);
        sgl_end();
    }

    sgl_c4f(0.86f, 0.93f, 1.0f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(22.0f, 0.0f);
    sgl_v2f(-14.0f, 12.0f);
    sgl_v2f(-14.0f, -12.0f);
    sgl_end();

    sgl_c4f(0.12f, 0.20f, 0.28f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-5.0f, 5.5f);
    sgl_v2f(-5.0f, -5.5f);
    sgl_end();

    draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), 0.55f, 0.72f, 0.92f, 0.85f);
    draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), 0.55f, 0.72f, 0.92f, 0.85f);

    sgl_pop_matrix();
}

static void draw_beam(void) {
    if (!g.beam_active) {
        return;
    }

    if (g.beam_hit) {
        draw_segment(g.beam_start, g.beam_end, 0.45f, 1.0f, 0.92f, 0.95f);
        draw_segment(g.beam_start, g.beam_end, 0.12f, 0.78f, 1.0f, 0.35f);
    } else {
        draw_segment(g.beam_start, g.beam_end, 0.9f, 0.75f, 0.30f, 0.55f);
    }
}

static void draw_ui_panel(float x, float y, float width, float height, float accent) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.03f, 0.06f, 0.10f, 0.86f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.16f, 0.30f + accent, 0.42f + accent, 0.95f);
    draw_rect_centered(v2(x + 54.0f, y + 12.0f), 40.0f, 1.5f, 0.28f, 0.74f + accent, 1.0f, 0.75f);
}

static void get_station_panel_rect(float* x, float* y, float* width, float* height) {
    float screen_w = sapp_widthf();
    float screen_h = sapp_heightf();
    bool compact = screen_w < 760.0f;
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_height = compact ? 30.0f : 34.0f;
    float top_height = compact ? 92.0f : 106.0f;
    float panel_width = fminf(compact ? (screen_w - (hud_margin * 2.0f)) : 560.0f, screen_w - (hud_margin * 2.0f));
    float panel_height = compact ? 260.0f : 320.0f;
    float panel_x = (screen_w - panel_width) * 0.5f;
    float min_y = hud_margin + top_height + 16.0f;
    float max_y = screen_h - hud_margin - bottom_height - panel_height - 14.0f;
    float panel_y = clampf((screen_h - panel_height) * 0.5f, min_y, fmaxf(min_y, max_y));

    *x = panel_x;
    *y = panel_y;
    *width = panel_width;
    *height = panel_height;
}

static void draw_ui_scrim(float alpha) {
    draw_rect_centered(v2(sapp_widthf() * 0.5f, sapp_heightf() * 0.5f), sapp_widthf() * 0.5f, sapp_heightf() * 0.5f, 0.01f, 0.03f, 0.06f, alpha);
}

static void draw_ui_meter(float x, float y, float width, float height, float fill, float r, float g0, float b) {
    float clamped_fill = clampf(fill, 0.0f, 1.0f);
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.04f, 0.07f, 0.12f, 0.96f);
    if (clamped_fill > 0.0f) {
        float fill_width = width * clamped_fill;
        draw_rect_centered(v2(x + (fill_width * 0.5f), y + (height * 0.5f)), fill_width * 0.5f, height * 0.5f, r, g0, b, 0.92f);
    }
    draw_rect_outline(center, width * 0.5f, height * 0.5f, 0.18f, 0.30f, 0.42f, 0.95f);
}

static void draw_upgrade_pips(float x, float y, int level, float r, float g0, float b) {
    const float pip_w = 16.0f;
    const float pip_h = 7.0f;
    const float gap = 6.0f;

    for (int i = 0; i < SHIP_UPGRADE_MAX_LEVEL; i++) {
        float px = x + ((pip_w + gap) * (float)i);
        float alpha = i < level ? 0.95f : 0.35f;
        draw_rect_centered(v2(px + (pip_w * 0.5f), y + (pip_h * 0.5f)), pip_w * 0.5f, pip_h * 0.5f,
            i < level ? r : 0.12f,
            i < level ? g0 : 0.18f,
            i < level ? b : 0.24f,
            alpha);
        draw_rect_outline(v2(px + (pip_w * 0.5f), y + (pip_h * 0.5f)), pip_w * 0.5f, pip_h * 0.5f, 0.18f, 0.30f, 0.42f, 0.88f);
    }
}

static void draw_service_card(float x, float y, float width, float height, float accent_r, float accent_g, float accent_b, bool hot) {
    vec2 center = v2(x + (width * 0.5f), y + (height * 0.5f));
    float border_a = hot ? 0.96f : 0.62f;
    float accent_a = hot ? 0.92f : 0.38f;
    float body_tint = hot ? 0.11f : 0.07f;

    draw_rect_centered(center, width * 0.5f, height * 0.5f, 0.03f, body_tint, 0.11f, 0.92f);
    draw_rect_outline(center, width * 0.5f, height * 0.5f, accent_r * 0.45f, accent_g * 0.45f, accent_b * 0.45f, border_a);
    draw_rect_centered(v2(x + 3.0f, y + (height * 0.5f)), 3.0f, height * 0.5f, accent_r, accent_g, accent_b, accent_a);
}

static void draw_hud_panels(void) {
    float screen_w = sapp_widthf();
    float screen_h = sapp_heightf();
    bool compact = screen_w < 760.0f;
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float bottom_height = compact ? 30.0f : 34.0f;
    float top_width = compact ? (screen_w - (hud_margin * 2.0f)) : 360.0f;
    float top_height = compact ? 92.0f : 106.0f;

    draw_ui_panel(hud_margin, hud_margin, top_width, top_height, 0.03f);

    draw_ui_panel(hud_margin, screen_h - hud_margin - bottom_height, screen_w - (hud_margin * 2.0f), bottom_height, 0.02f);

    if (g.docked) {
        float panel_x = 0.0f;
        float panel_y = 0.0f;
        float panel_w = 0.0f;
        float panel_h = 0.0f;
        float inner_x = 0.0f;
        float inner_y = 0.0f;
        float inner_w = 0.0f;
        float inner_h = 0.0f;
        float market_x = 0.0f;
        float market_y = 0.0f;
        float market_w = 0.0f;
        float market_h = 0.0f;
        float services_x = 0.0f;
        float services_y = 0.0f;
        float services_w = 0.0f;
        float services_h = 0.0f;
        float fit_x = 0.0f;
        float fit_y = 0.0f;
        float fit_w = 0.0f;
        float fit_h = 0.0f;
        float card_gap = compact ? 3.0f : 8.0f;
        float card_h = compact ? 16.0f : 30.0f;
        float sell_y = 0.0f;
        float repair_y = 0.0f;
        float mining_y = 0.0f;
        float hold_y = 0.0f;
        float tractor_y = 0.0f;
        bool can_sell = station_has_service(STATION_SERVICE_MARKET) && (g.ship.cargo > 0.01f);
        bool can_repair = station_has_service(STATION_SERVICE_REPAIR) && (station_repair_cost() > 0.0f) && (g.ship.credits + 0.01f >= station_repair_cost());
        bool can_upgrade_mining = station_has_service(STATION_SERVICE_OUTFIT) && !ship_upgrade_maxed(SHIP_UPGRADE_MINING) && (g.ship.credits + 0.01f >= (float)ship_upgrade_cost(SHIP_UPGRADE_MINING));
        bool can_upgrade_hold = station_has_service(STATION_SERVICE_OUTFIT) && !ship_upgrade_maxed(SHIP_UPGRADE_HOLD) && (g.ship.credits + 0.01f >= (float)ship_upgrade_cost(SHIP_UPGRADE_HOLD));
        bool can_upgrade_tractor = station_has_service(STATION_SERVICE_OUTFIT) && !ship_upgrade_maxed(SHIP_UPGRADE_TRACTOR) && (g.ship.credits + 0.01f >= (float)ship_upgrade_cost(SHIP_UPGRADE_TRACTOR));

        get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
        draw_ui_scrim(0.44f);
        draw_ui_panel(panel_x, panel_y, panel_w, panel_h, 0.08f);

        inner_x = panel_x + 18.0f;
        inner_y = panel_y + 18.0f;
        inner_w = panel_w - 36.0f;
        inner_h = panel_h - 36.0f;

        fit_w = compact ? inner_w : 180.0f;
        fit_x = compact ? inner_x : (panel_x + panel_w - fit_w - 18.0f);
        fit_y = inner_y + 34.0f;
        fit_h = compact ? 94.0f : (inner_h - 34.0f);

        market_x = inner_x;
        market_y = fit_y;
        market_w = compact ? inner_w : (fit_x - inner_x - 14.0f);
        market_h = compact ? 60.0f : 76.0f;

        services_x = inner_x;
        services_y = market_y + market_h + 12.0f;
        services_w = compact ? inner_w : market_w;
        services_h = compact ? 112.0f : (panel_y + panel_h - 18.0f - services_y);

        draw_ui_panel(market_x, market_y, market_w, market_h, 0.04f);
        draw_ui_panel(services_x, services_y, services_w, services_h, 0.03f);
        draw_ui_panel(fit_x, fit_y, fit_w, fit_h, 0.05f);

        sell_y = services_y + 14.0f;
        repair_y = sell_y + card_h + card_gap;
        mining_y = repair_y + card_h + card_gap;
        hold_y = mining_y + card_h + card_gap;
        tractor_y = hold_y + card_h + card_gap;

        draw_service_card(services_x + 12.0f, sell_y, services_w - 24.0f, card_h, 0.24f, 0.90f, 0.70f, can_sell);
        draw_service_card(services_x + 12.0f, repair_y, services_w - 24.0f, card_h, 0.98f, 0.72f, 0.26f, can_repair);
        draw_service_card(services_x + 12.0f, mining_y, services_w - 24.0f, card_h, 0.34f, 0.88f, 1.0f, can_upgrade_mining);
        draw_service_card(services_x + 12.0f, hold_y, services_w - 24.0f, card_h, 0.50f, 0.82f, 1.0f, can_upgrade_hold);
        draw_service_card(services_x + 12.0f, tractor_y, services_w - 24.0f, card_h, 0.42f, 1.0f, 0.86f, can_upgrade_tractor);

        draw_ui_meter(fit_x + 16.0f, fit_y + 54.0f, fit_w - 32.0f, 12.0f, g.ship.hull / ship_max_hull(), 0.96f, 0.54f, 0.28f);
        draw_ui_meter(fit_x + 16.0f, fit_y + 94.0f, fit_w - 32.0f, 12.0f, g.ship.cargo / fmaxf(1.0f, ship_cargo_capacity()), 0.26f, 0.90f, 0.72f);
        if (!compact) {
            draw_upgrade_pips(fit_x + 18.0f, fit_y + 146.0f, g.ship.mining_level, 0.34f, 0.88f, 1.0f);
            draw_upgrade_pips(fit_x + 18.0f, fit_y + 184.0f, g.ship.hold_level, 0.50f, 0.82f, 1.0f);
            draw_upgrade_pips(fit_x + 18.0f, fit_y + 222.0f, g.ship.tractor_level, 0.42f, 1.0f, 0.86f);
        }
    }
}

static void draw_station_services(void) {
    if (!g.docked) {
        return;
    }

    float panel_x = 0.0f;
    float panel_y = 0.0f;
    float panel_w = 0.0f;
    float panel_h = 0.0f;
    float inner_x = 0.0f;
    float inner_y = 0.0f;
    float inner_w = 0.0f;
    float market_x = 0.0f;
    float market_y = 0.0f;
    float market_h = 0.0f;
    float services_x = 0.0f;
    float services_y = 0.0f;
    float fit_x = 0.0f;
    float fit_y = 0.0f;
    float fit_w = 0.0f;
    float card_gap = 0.0f;
    float card_h = 0.0f;
    float sell_y = 0.0f;
    float repair_y = 0.0f;
    float mining_y = 0.0f;
    float hold_y = 0.0f;
    float tractor_y = 0.0f;
    bool compact = sapp_widthf() < 760.0f;
    int hull_now = (int)lroundf(g.ship.hull);
    int hull_max = (int)lroundf(ship_max_hull());
    int cargo_units = (int)lroundf(g.ship.cargo);
    int cargo_capacity = (int)lroundf(ship_cargo_capacity());
    int payout = (int)lroundf(station_cargo_sale_value());
    int ore_price = (int)lroundf(g.station.ore_price);
    int repair_cost = (int)lroundf(station_repair_cost());
    int mining_cost = ship_upgrade_cost(SHIP_UPGRADE_MINING);
    int hold_cost = ship_upgrade_cost(SHIP_UPGRADE_HOLD);
    int tractor_cost = ship_upgrade_cost(SHIP_UPGRADE_TRACTOR);

    get_station_panel_rect(&panel_x, &panel_y, &panel_w, &panel_h);
    inner_x = panel_x + 18.0f;
    inner_y = panel_y + 18.0f;
    inner_w = panel_w - 36.0f;
    fit_w = compact ? inner_w : 180.0f;
    fit_x = compact ? inner_x : (panel_x + panel_w - fit_w - 18.0f);
    fit_y = inner_y + 34.0f;
    market_x = inner_x;
    market_y = fit_y;
    market_h = compact ? 60.0f : 76.0f;
    services_x = inner_x;
    services_y = market_y + market_h + 12.0f;
    card_gap = compact ? 3.0f : 8.0f;
    card_h = compact ? 16.0f : 30.0f;
    sell_y = services_y + 14.0f;
    repair_y = sell_y + card_h + card_gap;
    mining_y = repair_y + card_h + card_gap;
    hold_y = mining_y + card_h + card_gap;
    tractor_y = hold_y + card_h + card_gap;

    sdtx_color3b(232, 241, 255);
    sdtx_pos((panel_x + 20.0f) / HUD_CELL, (panel_y + 18.0f) / HUD_CELL);
    sdtx_puts(g.station.name);
    sdtx_pos((panel_x + 20.0f) / HUD_CELL, (panel_y + 34.0f) / HUD_CELL);
    sdtx_color3b(118, 255, 221);
    sdtx_puts("RUN HUB // market, repair, outfit");

    sdtx_pos((panel_x + panel_w - (compact ? 132.0f : 156.0f)) / HUD_CELL, (panel_y + 18.0f) / HUD_CELL);
    sdtx_color3b(203, 220, 248);
    sdtx_printf("BOARD %d CR", ore_price);
    sdtx_pos((panel_x + panel_w - (compact ? 132.0f : 156.0f)) / HUD_CELL, (panel_y + 34.0f) / HUD_CELL);
    sdtx_color3b(145, 160, 188);
    sdtx_puts("Press E to launch");

    sdtx_pos((market_x + 16.0f) / HUD_CELL, (market_y + 16.0f) / HUD_CELL);
    sdtx_color3b(130, 255, 235);
    sdtx_puts("Cargo board");
    sdtx_pos((market_x + 16.0f) / HUD_CELL, (market_y + 34.0f) / HUD_CELL);
    sdtx_color3b(203, 220, 248);
    if (compact) {
        sdtx_printf("Ore %d/%d  Payout %d", cargo_units, cargo_capacity, payout);
    } else {
        sdtx_printf("Ore manifest %d/%d units   Payout preview %d cr", cargo_units, cargo_capacity, payout);
    }
    sdtx_pos((market_x + 16.0f) / HUD_CELL, (market_y + 50.0f) / HUD_CELL);
    sdtx_color3b(145, 160, 188);
    if (cargo_units > 0) {
        sdtx_puts("Return value is locked before you sell.");
    } else {
        sdtx_puts("Bring in fragments from the belt to post a sale.");
    }

    sdtx_pos((services_x + 16.0f) / HUD_CELL, (services_y + 16.0f) / HUD_CELL);
    sdtx_color3b(130, 255, 235);
    sdtx_puts("Service bay");

    sdtx_pos((services_x + 24.0f) / HUD_CELL, (sell_y + (compact ? 4.0f : 9.0f)) / HUD_CELL);
    sdtx_color3b(cargo_units > 0 ? 114 : 169, cargo_units > 0 ? 255 : 179, cargo_units > 0 ? 192 : 204);
    sdtx_printf("[1] Sell cargo manifest      %s", cargo_units > 0 ? "ready" : "empty");

    sdtx_pos((services_x + 24.0f) / HUD_CELL, (repair_y + (compact ? 4.0f : 9.0f)) / HUD_CELL);
    if (repair_cost > 0) {
        sdtx_color3b(255, 221, 119);
        sdtx_printf("[2] Repair hull             %d cr", repair_cost);
    } else {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("[2] Repair hull             nominal");
    }

    sdtx_pos((services_x + 24.0f) / HUD_CELL, (mining_y + (compact ? 4.0f : 9.0f)) / HUD_CELL);
    if (ship_upgrade_maxed(SHIP_UPGRADE_MINING)) {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("[3] Mining lattice          maxed");
    } else {
        sdtx_color3b(203, 220, 248);
        sdtx_printf("[3] Mining lattice          %d cr", mining_cost);
    }

    sdtx_pos((services_x + 24.0f) / HUD_CELL, (hold_y + (compact ? 4.0f : 9.0f)) / HUD_CELL);
    if (ship_upgrade_maxed(SHIP_UPGRADE_HOLD)) {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("[4] Cargo racks             maxed");
    } else {
        sdtx_color3b(203, 220, 248);
        sdtx_printf("[4] Cargo racks             %d cr", hold_cost);
    }

    sdtx_pos((services_x + 24.0f) / HUD_CELL, (tractor_y + (compact ? 4.0f : 9.0f)) / HUD_CELL);
    if (ship_upgrade_maxed(SHIP_UPGRADE_TRACTOR)) {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("[5] Tractor field           maxed");
    } else {
        sdtx_color3b(203, 220, 248);
        sdtx_printf("[5] Tractor field           %d cr", tractor_cost);
    }

    sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 16.0f) / HUD_CELL);
    sdtx_color3b(130, 255, 235);
    sdtx_puts("Ship fit");
    sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 34.0f) / HUD_CELL);
    sdtx_color3b(203, 220, 248);
    if (compact) {
        sdtx_printf("Hull %d/%d  Hold %d/%d", hull_now, hull_max, cargo_units, cargo_capacity);
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 74.0f) / HUD_CELL);
        sdtx_color3b(145, 160, 188);
        sdtx_printf("Laser %d/s  Tractor %du", (int)lroundf(ship_mining_rate()), (int)lroundf(ship_tractor_range()));
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 92.0f) / HUD_CELL);
        sdtx_printf("Upgrades L%d H%d T%d", g.ship.mining_level, g.ship.hold_level, g.ship.tractor_level);
    } else {
        sdtx_printf("Hull %d/%d", hull_now, hull_max);
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 74.0f) / HUD_CELL);
        sdtx_color3b(203, 220, 248);
        sdtx_printf("Cargo %d/%d", cargo_units, cargo_capacity);
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 126.0f) / HUD_CELL);
        sdtx_color3b(145, 160, 188);
        sdtx_printf("Mining %d ore/sec", (int)lroundf(ship_mining_rate()));
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 164.0f) / HUD_CELL);
        sdtx_printf("Hold capacity %d ore", cargo_capacity);
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 202.0f) / HUD_CELL);
        sdtx_printf("Tractor range %d u", (int)lroundf(ship_tractor_range()));
        sdtx_pos((fit_x + 16.0f) / HUD_CELL, (fit_y + 240.0f) / HUD_CELL);
        sdtx_color3b(169, 179, 204);
        sdtx_puts("Every upgrade carries into the next sortie.");
    }
}

static void draw_hud(void) {
    float screen_w = sapp_widthf();
    float screen_h = sapp_heightf();
    bool compact = screen_w < 760.0f;
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float left_text_x = (hud_margin + 18.0f) / HUD_CELL;
    float top_text_y = (hud_margin + 18.0f) / HUD_CELL;
    float bottom_text_y = (screen_h - hud_margin - 20.0f) / HUD_CELL;
    int hull_units = (int)lroundf(g.ship.hull);
    int hull_capacity = (int)lroundf(ship_max_hull());
    int cargo_units = (int)lroundf(g.ship.cargo);
    int credits = (int)lroundf(g.ship.credits);
    int station_distance = (int)lroundf(v2_len(v2_sub(g.station.pos, g.ship.pos)));
    int cargo_capacity = (int)lroundf(ship_cargo_capacity());
    int payout_preview = (int)lroundf(station_cargo_sale_value());

    vec2 forward = v2_from_angle(g.ship.angle);
    vec2 home = v2_norm(v2_sub(g.station.pos, g.ship.pos));
    float bearing = atan2f(v2_cross(forward, home), v2_dot(forward, home));
    int bearing_degrees = (int)lroundf(fabsf(bearing) * (180.0f / PI_F));
    const char* bearing_side = "ahead";
    if (bearing > 0.12f) {
        bearing_side = "left";
    } else if (bearing < -0.12f) {
        bearing_side = "right";
    } else {
        bearing_degrees = 0;
    }

    sdtx_canvas(sapp_widthf(), sapp_heightf());
    sdtx_font(0);
    sdtx_origin(0.0f, 0.0f);
    sdtx_home();

    sdtx_pos(left_text_x, top_text_y);
    sdtx_color3b(232, 241, 255);
    sdtx_puts("SHIP STATUS");
    sdtx_crlf();

    sdtx_color3b(203, 220, 248);
    if (compact) {
        sdtx_printf("CR %d  H %d/%d  C %d/%d", credits, hull_units, hull_capacity, cargo_units, cargo_capacity);
    } else {
        sdtx_printf("Credits %d cr   Hull %d/%d   Cargo %d/%d ore", credits, hull_units, hull_capacity, cargo_units, cargo_capacity);
    }
    sdtx_crlf();

    if (g.docked) {
        sdtx_color3b(112, 255, 214);
        if (compact) {
            sdtx_puts("Docked. Press E to launch");
        } else {
            sdtx_printf("Docked at %s. Press E to launch.", g.station.name);
        }
    } else if (g.in_dock_range) {
        sdtx_color3b(112, 255, 214);
        if (compact) {
            sdtx_puts("Dock ring hot, press E to dock");
        } else {
            sdtx_printf("%s docking ring acquired. Press E to dock.", g.station.name);
        }
    } else {
        sdtx_color3b(199, 222, 255);
        if (compact) {
            sdtx_printf("Station %d units, %d deg %s", station_distance, bearing_degrees, bearing_side);
        } else {
            sdtx_printf("Station %d units away, %d deg %s", station_distance, bearing_degrees, bearing_side);
        }
    }
    sdtx_crlf();

    if (g.docked) {
        sdtx_color3b(130, 255, 235);
        sdtx_printf("Cargo board ready. Current payout preview %d cr.", payout_preview);
    } else if ((g.hover_asteroid >= 0) && g.asteroids[g.hover_asteroid].active) {
        const asteroid_t* asteroid = &g.asteroids[g.hover_asteroid];
        int integrity_left = (int)lroundf(asteroid->hp);
        sdtx_color3b(130, 255, 235);
        sdtx_printf("Target %s %s, %d integrity", asteroid_tier_name(asteroid->tier), asteroid_tier_kind(asteroid->tier), integrity_left);
    } else if (g.nearby_fragments > 0) {
        sdtx_color3b(130, 255, 235);
        if (g.tractor_fragments > 0) {
            sdtx_printf("Tractor lock on %d fragment%s", g.tractor_fragments, g.tractor_fragments == 1 ? "" : "s");
        } else {
            sdtx_printf("Nearby fragments %d", g.nearby_fragments);
        }
    } else {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("No target lock. Line up a rock.");
    }
    sdtx_crlf();

    if (g.docked) {
        if (g.notice_timer > 0.0f) {
            sdtx_color3b(114, 255, 192);
            sdtx_puts(g.notice);
        } else {
            sdtx_color3b(164, 177, 205);
            sdtx_puts("Sell, repair, refit, then launch the next sortie.");
        }
    } else if (g.collection_feedback_timer > 0.0f) {
        int recovered_ore = (int)lroundf(g.collection_feedback_ore);
        sdtx_color3b(114, 255, 192);
        if (g.collection_feedback_fragments > 0) {
            sdtx_printf("Recovered %d ore from %d fragment%s.", recovered_ore, g.collection_feedback_fragments, g.collection_feedback_fragments == 1 ? "" : "s");
        } else {
            sdtx_printf("Recovered %d ore.", recovered_ore);
        }
    } else if ((cargo_units >= cargo_capacity) && (g.nearby_fragments > 0)) {
        sdtx_color3b(255, 221, 119);
        sdtx_puts("Hold full. Nearby fragments drifting out there.");
    } else if (cargo_units >= cargo_capacity) {
        sdtx_color3b(255, 221, 119);
        sdtx_puts("Cargo hold full. Return to station.");
    } else if (g.nearby_fragments > 0) {
        sdtx_color3b(114, 255, 192);
        if (g.tractor_fragments > 0) {
            sdtx_puts("Sweep through fragments to collect them.");
        } else {
            sdtx_puts("Close in and let the tractor pull fragments in.");
        }
    } else if (g.notice_timer > 0.0f) {
        sdtx_color3b(114, 255, 192);
        sdtx_puts(g.notice);
    } else {
        sdtx_color3b(164, 177, 205);
        sdtx_puts("Crack rocks, sweep fragments, dock, sell.");
    }

    sdtx_pos(left_text_x, bottom_text_y);
    sdtx_color3b(145, 160, 188);
    if (g.docked) {
        sdtx_puts("1 sell  2 repair  3 laser  4 hold  5 tractor  E launch  R reset  ESC quit");
    } else {
        sdtx_puts("W/S thrust  A/D turn  SPACE mine  E dock  R reset  ESC quit");
    }

    draw_station_services();
}

static void resolve_ship_circle(vec2 center, float radius) {
    vec2 delta = v2_sub(g.ship.pos, center);
    float minimum = radius + SHIP_RADIUS;
    float distance_sq = v2_len_sq(delta);
    float minimum_sq = minimum * minimum;
    if (distance_sq >= minimum_sq) {
        return;
    }

    float distance = sqrtf(distance_sq);
    vec2 normal = distance > 0.00001f ? v2_scale(delta, 1.0f / distance) : v2(1.0f, 0.0f);
    g.ship.pos = v2_add(center, v2_scale(normal, minimum));

    float velocity_towards_surface = v2_dot(g.ship.vel, normal);
    if (velocity_towards_surface < 0.0f) {
        float impact_speed = -velocity_towards_surface;
        if (!g.docked && (impact_speed > SHIP_COLLISION_DAMAGE_THRESHOLD)) {
            apply_ship_damage((impact_speed - SHIP_COLLISION_DAMAGE_THRESHOLD) * SHIP_COLLISION_DAMAGE_SCALE);
        }
        g.ship.vel = v2_sub(g.ship.vel, v2_scale(normal, velocity_towards_surface * 1.2f));
    }
}

static int find_mining_target(vec2 origin, vec2 forward) {
    int best_index = -1;
    float best_projection = MINING_RANGE + 1.0f;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t* asteroid = &g.asteroids[i];
        if (!asteroid->active || asteroid_is_collectible(asteroid)) {
            continue;
        }
        vec2 to_asteroid = v2_sub(asteroid->pos, origin);
        float projection = v2_dot(to_asteroid, forward);
        if ((projection < 0.0f) || (projection > MINING_RANGE)) {
            continue;
        }

        float distance_from_ray = fabsf(v2_cross(to_asteroid, forward));
        if (distance_from_ray > (asteroid->radius + 9.0f)) {
            continue;
        }

        if (projection < best_projection) {
            best_projection = projection;
            best_index = i;
        }
    }

    return best_index;
}

static bool is_key_down(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_down[key];
}

static bool is_key_pressed(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_pressed[key];
}

static vec2 ship_forward(void) {
    return v2_from_angle(g.ship.angle);
}

static vec2 ship_muzzle(vec2 forward) {
    return v2_add(g.ship.pos, v2_scale(forward, SHIP_RADIUS + 8.0f));
}

static bool try_spend_credits(float amount) {
    if (amount <= 0.0f) {
        return true;
    }
    if (g.ship.credits + 0.01f < amount) {
        return false;
    }
    g.ship.credits = fmaxf(0.0f, g.ship.credits - amount);
    return true;
}

static void anchor_ship_in_station(void) {
    g.ship.pos = station_dock_anchor();
    g.ship.vel = v2(0.0f, 0.0f);
}

static void dock_ship(void) {
    g.docked = true;
    g.in_dock_range = true;
    anchor_ship_in_station();
    set_notice("Docked at %s.", g.station.name);
}

static void launch_ship(void) {
    g.docked = false;
    g.in_dock_range = true;
    anchor_ship_in_station();
    set_notice("Launch corridor clear. Press thrust when ready.");
}

static void emergency_recover_ship(void) {
    int lost_units = (int)lroundf(g.ship.cargo);
    g.ship.cargo = 0.0f;
    g.ship.hull = ship_max_hull();
    g.ship.angle = PI_F * 0.5f;
    dock_ship();
    if (lost_units > 0) {
        set_notice("Tow drones recovered the hull. Lost %d ore in the breach.", lost_units);
    } else {
        set_notice("Tow drones recovered the hull. Repair crews have you docked.");
    }
}

static void apply_ship_damage(float damage) {
    if (damage <= 0.0f) {
        return;
    }

    g.ship.hull = fmaxf(0.0f, g.ship.hull - damage);
    if (g.ship.hull <= 0.01f) {
        emergency_recover_ship();
    }
}

static void try_sell_station_cargo(void) {
    if (!station_has_service(STATION_SERVICE_MARKET)) {
        set_notice("%s has no cargo market online.", g.station.name);
        return;
    }
    if (g.ship.cargo <= 0.01f) {
        set_notice("Cargo hold empty.");
        return;
    }

    int sold_units = (int)lroundf(g.ship.cargo);
    int payout = (int)lroundf(station_cargo_sale_value());
    g.ship.credits += station_cargo_sale_value();
    g.ship.cargo = 0.0f;
    set_notice("Sold %d ore for %d cr.", sold_units, payout);
}

static void try_repair_ship(void) {
    float repair_cost = station_repair_cost();
    if (!station_has_service(STATION_SERVICE_REPAIR)) {
        set_notice("%s repair crews are offline.", g.station.name);
        return;
    }
    if (repair_cost <= 0.0f) {
        set_notice("Hull already nominal.");
        return;
    }
    if (!try_spend_credits(repair_cost)) {
        set_notice("Need %d cr for hull repair.", (int)lroundf(repair_cost));
        return;
    }

    g.ship.hull = ship_max_hull();
    set_notice("Hull restored for %d cr.", (int)lroundf(repair_cost));
}

static void try_apply_ship_upgrade(ship_upgrade_t upgrade) {
    if (!station_has_service(STATION_SERVICE_OUTFIT)) {
        set_notice("%s outfitting bay is offline.", g.station.name);
        return;
    }
    if (ship_upgrade_maxed(upgrade)) {
        switch (upgrade) {
            case SHIP_UPGRADE_MINING:
                set_notice("Mining lattice already tuned to spec.");
                break;
            case SHIP_UPGRADE_HOLD:
                set_notice("Cargo racks already at station limit.");
                break;
            case SHIP_UPGRADE_TRACTOR:
                set_notice("Tractor field already at station limit.");
                break;
            case SHIP_UPGRADE_COUNT:
            default:
                break;
        }
        return;
    }

    int cost = ship_upgrade_cost(upgrade);
    if (!try_spend_credits((float)cost)) {
        switch (upgrade) {
            case SHIP_UPGRADE_MINING:
                set_notice("Need %d cr to retune the mining lattice.", cost);
                break;
            case SHIP_UPGRADE_HOLD:
                set_notice("Need %d cr for expanded cargo racks.", cost);
                break;
            case SHIP_UPGRADE_TRACTOR:
                set_notice("Need %d cr for a tractor field retune.", cost);
                break;
            case SHIP_UPGRADE_COUNT:
            default:
                break;
        }
        return;
    }

    switch (upgrade) {
        case SHIP_UPGRADE_MINING:
            g.ship.mining_level++;
            set_notice("Mining lattice tuned. Beam output now %d ore/sec.", (int)lroundf(ship_mining_rate()));
            break;
        case SHIP_UPGRADE_HOLD:
            g.ship.hold_level++;
            set_notice("Cargo racks expanded. Hold now fits %d ore.", (int)lroundf(ship_cargo_capacity()));
            break;
        case SHIP_UPGRADE_TRACTOR:
            g.ship.tractor_level++;
            set_notice("Tractor field widened to %d units.", (int)lroundf(ship_tractor_range()));
            break;
        case SHIP_UPGRADE_COUNT:
        default:
            break;
    }
}

static void reset_step_feedback(void) {
    g.hover_asteroid = -1;
    g.beam_active = false;
    g.beam_hit = false;
    g.thrusting = false;
    g.nearby_fragments = 0;
    g.tractor_fragments = 0;
}

static input_intent_t sample_input_intent(void) {
    input_intent_t intent = { 0 };

    if (is_key_down(SAPP_KEYCODE_A) || is_key_down(SAPP_KEYCODE_LEFT)) {
        intent.turn += 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_D) || is_key_down(SAPP_KEYCODE_RIGHT)) {
        intent.turn -= 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_W) || is_key_down(SAPP_KEYCODE_UP)) {
        intent.thrust += 1.0f;
    }
    if (is_key_down(SAPP_KEYCODE_S) || is_key_down(SAPP_KEYCODE_DOWN)) {
        intent.thrust -= 1.0f;
    }

    intent.mine = is_key_down(SAPP_KEYCODE_SPACE);
    intent.interact = is_key_pressed(SAPP_KEYCODE_E);
    intent.service_sell = is_key_pressed(SAPP_KEYCODE_1);
    intent.service_repair = is_key_pressed(SAPP_KEYCODE_2);
    intent.upgrade_mining = is_key_pressed(SAPP_KEYCODE_3);
    intent.upgrade_hold = is_key_pressed(SAPP_KEYCODE_4);
    intent.upgrade_tractor = is_key_pressed(SAPP_KEYCODE_5);
    intent.reset = is_key_pressed(SAPP_KEYCODE_R);
    return intent;
}

static void step_ship_rotation(float dt, float turn_input) {
    g.ship.angle = wrap_angle(g.ship.angle + (turn_input * SHIP_TURN_SPEED * dt));
}

static void step_ship_thrust(float dt, float thrust_input, vec2 forward) {
    if (thrust_input > 0.0f) {
        g.ship.vel = v2_add(g.ship.vel, v2_scale(forward, SHIP_ACCEL * thrust_input * dt));
        g.thrusting = true;
    } else if (thrust_input < 0.0f) {
        g.ship.vel = v2_add(g.ship.vel, v2_scale(forward, SHIP_BRAKE * thrust_input * dt));
    }
}

static void step_ship_motion(float dt) {
    g.ship.vel = v2_scale(g.ship.vel, 1.0f / (1.0f + (SHIP_DRAG * dt)));
    g.ship.pos = v2_add(g.ship.pos, v2_scale(g.ship.vel, dt));

    float world_distance_sq = v2_len_sq(g.ship.pos);
    float world_radius_sq = WORLD_RADIUS * WORLD_RADIUS;
    if (world_distance_sq > world_radius_sq) {
        float world_distance = sqrtf(world_distance_sq);
        vec2 push_dir = v2_scale(g.ship.pos, 1.0f / world_distance);
        vec2 push_home = v2_scale(push_dir, -(world_distance - WORLD_RADIUS) * 0.08f);
        g.ship.vel = v2_add(g.ship.vel, push_home);
    }
}

static void step_asteroid_dynamics(float dt) {
    float cleanup_distance_sq = FRACTURE_CHILD_CLEANUP_DISTANCE * FRACTURE_CHILD_CLEANUP_DISTANCE;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t* asteroid = &g.asteroids[i];
        if (!asteroid->active) {
            continue;
        }

        asteroid->rotation += asteroid->spin * dt;
        asteroid->pos = v2_add(asteroid->pos, v2_scale(asteroid->vel, dt));
        asteroid->vel = v2_scale(asteroid->vel, 1.0f / (1.0f + (0.42f * dt)));
        asteroid->age += dt;

        float max_distance = WORLD_RADIUS + asteroid->radius + 260.0f;
        if (v2_len_sq(asteroid->pos) > (max_distance * max_distance)) {
            clear_asteroid(asteroid);
            continue;
        }

        if (asteroid->fracture_child && (asteroid->age >= FRACTURE_CHILD_CLEANUP_AGE)) {
            if (v2_dist_sq(asteroid->pos, g.ship.pos) > cleanup_distance_sq) {
                clear_asteroid(asteroid);
            }
        }
    }
}

static void resolve_world_collisions(void) {
    resolve_ship_circle(g.station.pos, g.station.radius + 4.0f);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.asteroids[i].active || asteroid_is_collectible(&g.asteroids[i])) {
            continue;
        }
        resolve_ship_circle(g.asteroids[i].pos, g.asteroids[i].radius);
    }
}

static void update_docking_state(float dt) {
    if (g.docked) {
        g.in_dock_range = true;
        anchor_ship_in_station();
        return;
    }

    float dock_radius_sq = g.station.dock_radius * g.station.dock_radius;
    g.in_dock_range = v2_dist_sq(g.ship.pos, g.station.pos) <= dock_radius_sq;
    if (g.in_dock_range) {
        g.ship.vel = v2_scale(g.ship.vel, 1.0f / (1.0f + (dt * 2.2f)));
    }
}

static void update_targeting_state(vec2 forward) {
    g.hover_asteroid = find_mining_target(ship_muzzle(forward), forward);
}

static void step_fragment_collection(float dt) {
    float nearby_range_sq = FRAGMENT_NEARBY_RANGE * FRAGMENT_NEARBY_RANGE;
    float tractor_range = ship_tractor_range();
    float tractor_range_sq = tractor_range * tractor_range;
    float cargo_space = ship_cargo_space();
    float collected_ore = 0.0f;
    int collected_fragments = 0;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t* asteroid = &g.asteroids[i];
        if (!asteroid_is_collectible(asteroid)) {
            continue;
        }

        vec2 to_ship = v2_sub(g.ship.pos, asteroid->pos);
        float distance_sq = v2_len_sq(to_ship);
        if (distance_sq <= nearby_range_sq) {
            g.nearby_fragments++;
        }

        if (cargo_space <= 0.0f) {
            continue;
        }

        if (distance_sq <= tractor_range_sq) {
            float distance = sqrtf(distance_sq);
            float pull_ratio = 1.0f - clampf(distance / tractor_range, 0.0f, 1.0f);
            vec2 pull_dir = distance > 0.001f ? v2_scale(to_ship, 1.0f / distance) : ship_forward();
            g.tractor_fragments++;
            asteroid->vel = v2_add(asteroid->vel, v2_scale(pull_dir, FRAGMENT_TRACTOR_ACCEL * lerpf(0.35f, 1.0f, pull_ratio) * dt));
            float speed = v2_len(asteroid->vel);
            if (speed > FRAGMENT_MAX_SPEED) {
                asteroid->vel = v2_scale(v2_norm(asteroid->vel), FRAGMENT_MAX_SPEED);
            }
        }

        float collect_radius = ship_collect_radius() + asteroid->radius;
        if (distance_sq <= (collect_radius * collect_radius)) {
            float recovered = fminf(asteroid->ore, cargo_space);
            if (recovered <= 0.0f) {
                continue;
            }

            g.ship.cargo += recovered;
            cargo_space -= recovered;
            asteroid->ore -= recovered;
            collected_ore += recovered;

            if (asteroid->ore <= 0.01f) {
                clear_asteroid(asteroid);
                collected_fragments++;
            } else if (asteroid->max_ore > 0.0f) {
                asteroid->radius = lerpf(asteroid_radius_min(ASTEROID_TIER_S) * 0.72f, asteroid_radius_max(ASTEROID_TIER_S), asteroid_progress_ratio(asteroid));
            }
        }
    }

    push_collection_feedback(collected_ore, collected_fragments);
}

static void step_mining_system(float dt, bool mining, vec2 forward) {
    if (!mining) {
        return;
    }

    vec2 muzzle = ship_muzzle(forward);
    g.beam_active = true;
    g.beam_start = muzzle;

    if (g.hover_asteroid >= 0) {
        asteroid_t* asteroid = &g.asteroids[g.hover_asteroid];
        vec2 to_asteroid = v2_sub(asteroid->pos, muzzle);
        vec2 normal = v2_norm(to_asteroid);
        g.beam_end = v2_sub(asteroid->pos, v2_scale(normal, asteroid->radius * 0.85f));
        g.beam_hit = true;

        float mined = ship_mining_rate() * dt;
        if (mined > 0.0f) {
            mined = fminf(mined, asteroid->hp);
            asteroid->hp -= mined;
            if (asteroid->hp <= 0.01f) {
                fracture_asteroid(g.hover_asteroid, normal);
            }
        }
    } else {
        g.beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
    }
}

static void step_station_interaction_system(const input_intent_t* intent) {
    if (intent->interact) {
        if (g.docked) {
            launch_ship();
            return;
        }
        if (!g.in_dock_range) {
            set_notice("Enter station ring to dock.");
            return;
        }
        dock_ship();
        return;
    }

    if (!g.docked) {
        return;
    }

    if (intent->service_sell) {
        try_sell_station_cargo();
    } else if (intent->service_repair) {
        try_repair_ship();
    } else if (intent->upgrade_mining) {
        try_apply_ship_upgrade(SHIP_UPGRADE_MINING);
    } else if (intent->upgrade_hold) {
        try_apply_ship_upgrade(SHIP_UPGRADE_HOLD);
    } else if (intent->upgrade_tractor) {
        try_apply_ship_upgrade(SHIP_UPGRADE_TRACTOR);
    }
}

static void step_notice_timer(float dt) {
    if (g.notice_timer > 0.0f) {
        g.notice_timer = fmaxf(0.0f, g.notice_timer - dt);
    }

    if (g.collection_feedback_timer > 0.0f) {
        g.collection_feedback_timer = fmaxf(0.0f, g.collection_feedback_timer - dt);
        if (g.collection_feedback_timer <= 0.0f) {
            clear_collection_feedback();
        }
    }
}

static void sim_step(float dt) {
    reset_step_feedback();
    g.time += dt;

    input_intent_t intent = sample_input_intent();
    if (intent.reset) {
        reset_world();
        consume_pressed_input();
        return;
    }

    step_asteroid_dynamics(dt);
    maintain_asteroid_field(dt);

    if (!g.docked) {
        step_ship_rotation(dt, intent.turn);
        vec2 forward = ship_forward();
        step_ship_thrust(dt, intent.thrust, forward);
        step_ship_motion(dt);
        resolve_world_collisions();
    }

    update_docking_state(dt);
    step_station_interaction_system(&intent);

    if (!g.docked) {
        vec2 forward = ship_forward();
        update_targeting_state(forward);
        step_mining_system(dt, intent.mine, forward);
        step_fragment_collection(dt);
    }

    step_notice_timer(dt);
    consume_pressed_input();
}

static void init(void) {
    memset(&g, 0, sizeof(g));
    g.rng = 0xC0FFEE12u;

    sg_setup(&(sg_desc){
        .environment = sglue_environment(),
        .logger.func = slog_func,
    });

    sgl_setup(&(sgl_desc_t){
        .logger.func = slog_func,
    });

    sdtx_setup(&(sdtx_desc_t){
        .fonts[0] = sdtx_font_oric(),
        .logger.func = slog_func,
    });

    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value = (sg_color){ 0.018f, 0.024f, 0.045f, 1.0f };

    init_starfield();
    reset_world();
}

static void render_world(void) {
    vec2 camera = g.ship.pos;
    float half_w = sapp_widthf() * 0.5f;
    float half_h = sapp_heightf() * 0.5f;

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(camera.x - half_w, camera.x + half_w, camera.y - half_h, camera.y + half_h, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    draw_background(camera);
    draw_station();
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.asteroids[i].active) {
            continue;
        }
        draw_asteroid(&g.asteroids[i], i == g.hover_asteroid);
    }
    draw_beam();
    draw_ship_tractor_field();
    draw_ship();
}

static void render_ui(void) {
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, sapp_widthf(), sapp_heightf(), 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    draw_hud_panels();
    draw_hud();
}

static void render_frame(void) {
    render_world();
    render_ui();

    sg_begin_pass(&(sg_pass){
        .action = g.pass_action,
        .swapchain = sglue_swapchain(),
    });
    sgl_draw();
    sdtx_draw();
    sg_end_pass();
    sg_commit();
}

static void advance_simulation_frame(float frame_dt) {
    g.runtime.accumulator += frame_dt;

    int sim_steps = 0;
    while ((g.runtime.accumulator >= SIM_DT) && (sim_steps < MAX_SIM_STEPS_PER_FRAME)) {
        sim_step(SIM_DT);
        g.runtime.accumulator -= SIM_DT;
        sim_steps++;
    }

    if (g.runtime.accumulator >= SIM_DT) {
        g.runtime.accumulator = 0.0f;
    }
}

static void frame(void) {
    float max_frame_dt = SIM_DT * (float)MAX_SIM_STEPS_PER_FRAME;
    float frame_dt = clampf((float)sapp_frame_duration(), 0.0f, max_frame_dt);
    advance_simulation_frame(frame_dt);
    render_frame();
}

static void cleanup(void) {
    sdtx_shutdown();
    sgl_shutdown();
    sg_shutdown();
}

static void event(const sapp_event* event) {
    switch (event->type) {
        case SAPP_EVENTTYPE_KEY_DOWN:
            if ((event->key_code >= 0) && (event->key_code < KEY_COUNT)) {
                g.input.key_down[event->key_code] = true;
                if (!event->key_repeat) {
                    g.input.key_pressed[event->key_code] = true;
                }
            }
            if (event->key_code == SAPP_KEYCODE_ESCAPE) {
                sapp_request_quit();
            }
            break;

        case SAPP_EVENTTYPE_KEY_UP:
            if ((event->key_code >= 0) && (event->key_code < KEY_COUNT)) {
                g.input.key_down[event->key_code] = false;
            }
            break;

        case SAPP_EVENTTYPE_UNFOCUSED:
        case SAPP_EVENTTYPE_SUSPENDED:
        case SAPP_EVENTTYPE_ICONIFIED:
            clear_input_state();
            break;

        default:
            break;
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = event,
        .width = 1280,
        .height = 720,
        .sample_count = 4,
        .high_dpi = true,
        .window_title = "Sokol Space Miner",
        .logger.func = slog_func,
    };
}
