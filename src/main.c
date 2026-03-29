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
    MAX_ASTEROIDS = 20,
    MAX_STARS = 120,
};

typedef struct {
    float x;
    float y;
} vec2;

typedef struct {
    vec2 pos;
    vec2 vel;
    float angle;
    float cargo;
    float credits;
} ship_t;

typedef struct {
    vec2 pos;
    float radius;
    float dock_radius;
} station_t;

typedef struct {
    vec2 pos;
    float radius;
    float ore;
    float max_ore;
    float rotation;
    float spin;
    float seed;
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
    bool sell;
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
    bool docked;
    vec2 beam_start;
    vec2 beam_end;
    char notice[128];
    float notice_timer;
    float time;
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
static const float SHIP_CARGO_MAX = 120.0f;
static const float MINING_RANGE = 170.0f;
static const float MINING_RATE = 28.0f;
static const float ORE_PRICE = 12.0f;
static const float HUD_MARGIN = 28.0f;
static const float HUD_CELL = 8.0f;
static const float SIM_DT = 1.0f / 120.0f;
static const int MAX_SIM_STEPS_PER_FRAME = 8;

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

static void respawn_asteroid(asteroid_t* asteroid) {
    float distance = rand_range(420.0f, WORLD_RADIUS - 180.0f);
    float angle = rand_range(0.0f, TWO_PI_F);
    asteroid->pos = v2(cosf(angle) * distance, sinf(angle) * distance);
    asteroid->radius = rand_range(26.0f, 52.0f);
    asteroid->max_ore = rand_range(55.0f, 150.0f);
    asteroid->ore = asteroid->max_ore;
    asteroid->rotation = rand_range(0.0f, TWO_PI_F);
    asteroid->spin = rand_range(-0.45f, 0.45f);
    asteroid->seed = rand_range(0.0f, 100.0f);
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
    g.ship.cargo = 0.0f;
    g.ship.credits = 0.0f;

    g.station.pos = v2(0.0f, 0.0f);
    g.station.radius = 58.0f;
    g.station.dock_radius = 125.0f;

    g.hover_asteroid = -1;
    g.beam_active = false;
    g.beam_hit = false;
    g.thrusting = false;
    g.docked = true;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        respawn_asteroid(&g.asteroids[i]);
    }

    set_notice("Mine ore, dock, sell.");
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
    draw_circle_outline(g.station.pos, g.station.dock_radius, 48, 0.25f, 0.85f, 1.0f, 0.15f + pulse);
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

static void draw_asteroid(const asteroid_t* asteroid, bool targeted) {
    float ore_ratio = asteroid->ore / asteroid->max_ore;
    float body_r = lerpf(0.24f, 0.38f, ore_ratio);
    float body_g = lerpf(0.23f, 0.34f, ore_ratio);
    float body_b = lerpf(0.24f, 0.31f, ore_ratio);

    sgl_c4f(body_r, body_g, body_b, 1.0f);
    sgl_begin_triangles();
    for (int i = 0; i < 18; i++) {
        float a0 = asteroid->rotation + ((float)i / 18.0f) * TWO_PI_F;
        float a1 = asteroid->rotation + ((float)(i + 1) / 18.0f) * TWO_PI_F;
        float r0 = asteroid_profile(asteroid, a0);
        float r1 = asteroid_profile(asteroid, a1);
        sgl_v2f(asteroid->pos.x, asteroid->pos.y);
        sgl_v2f(asteroid->pos.x + cosf(a0) * r0, asteroid->pos.y + sinf(a0) * r0);
        sgl_v2f(asteroid->pos.x + cosf(a1) * r1, asteroid->pos.y + sinf(a1) * r1);
    }
    sgl_end();

    float rim_r = targeted ? 0.45f : 0.22f;
    float rim_g = targeted ? 0.94f : 0.24f;
    float rim_b = targeted ? 1.0f : 0.30f;
    float rim_a = targeted ? 1.0f : 0.8f;

    sgl_c4f(rim_r, rim_g, rim_b, rim_a);
    sgl_begin_line_strip();
    for (int i = 0; i <= 18; i++) {
        float angle = asteroid->rotation + ((float)i / 18.0f) * TWO_PI_F;
        float radius = asteroid_profile(asteroid, angle);
        sgl_v2f(asteroid->pos.x + cosf(angle) * radius, asteroid->pos.y + sinf(angle) * radius);
    }
    sgl_end();

    if (targeted) {
        draw_circle_outline(asteroid->pos, asteroid->radius + 12.0f, 24, 0.35f, 1.0f, 0.92f, 0.75f);
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
}

static void draw_hud(void) {
    float screen_w = sapp_widthf();
    float screen_h = sapp_heightf();
    bool compact = screen_w < 760.0f;
    float hud_margin = compact ? 16.0f : HUD_MARGIN;
    float left_text_x = (hud_margin + 18.0f) / HUD_CELL;
    float top_text_y = (hud_margin + 18.0f) / HUD_CELL;
    float bottom_text_y = (screen_h - hud_margin - 20.0f) / HUD_CELL;
    int cargo_units = (int)lroundf(g.ship.cargo);
    int credits = (int)lroundf(g.ship.credits);
    int station_distance = (int)lroundf(v2_len(v2_sub(g.station.pos, g.ship.pos)));
    int cargo_capacity = (int)SHIP_CARGO_MAX;

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
        sdtx_printf("CR %d  CARGO %d/%d", credits, cargo_units, cargo_capacity);
    } else {
        sdtx_printf("Credits %d cr   Cargo %d/%d ore", credits, cargo_units, cargo_capacity);
    }
    sdtx_crlf();

    if (g.docked) {
        sdtx_color3b(112, 255, 214);
        if (compact) {
            sdtx_puts("Station docked, press E to sell");
        } else {
            sdtx_puts("Station docked. Press E to sell cargo.");
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

    if (g.hover_asteroid >= 0) {
        const asteroid_t* asteroid = &g.asteroids[g.hover_asteroid];
        int ore_left = (int)lroundf(asteroid->ore);
        sdtx_color3b(130, 255, 235);
        if (compact) {
            sdtx_printf("Target asteroid, %d ore", ore_left);
        } else {
            sdtx_printf("Target asteroid, %d ore remaining", ore_left);
        }
    } else {
        sdtx_color3b(169, 179, 204);
        sdtx_puts("No target lock. Line up an asteroid.");
    }
    sdtx_crlf();

    if (cargo_units >= cargo_capacity) {
        sdtx_color3b(255, 221, 119);
        sdtx_puts("Cargo hold full. Return to station.");
    } else if (g.notice_timer > 0.0f) {
        sdtx_color3b(114, 255, 192);
        sdtx_puts(g.notice);
    } else {
        sdtx_color3b(164, 177, 205);
        sdtx_puts("Mine ore, dock, sell.");
    }

    sdtx_pos(left_text_x, bottom_text_y);
    sdtx_color3b(145, 160, 188);
    sdtx_puts("W/S thrust  A/D turn  SPACE mine  E sell  R reset  ESC quit");
}

static void resolve_ship_circle(vec2 center, float radius) {
    vec2 delta = v2_sub(g.ship.pos, center);
    float distance = v2_len(delta);
    float minimum = radius + SHIP_RADIUS;
    if (distance >= minimum) {
        return;
    }

    vec2 normal = v2_norm(delta);
    g.ship.pos = v2_add(center, v2_scale(normal, minimum));

    float velocity_towards_surface = v2_dot(g.ship.vel, normal);
    if (velocity_towards_surface < 0.0f) {
        g.ship.vel = v2_sub(g.ship.vel, v2_scale(normal, velocity_towards_surface * 1.2f));
    }
}

static int find_mining_target(vec2 origin, vec2 forward) {
    int best_index = -1;
    float best_projection = MINING_RANGE + 1.0f;

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t* asteroid = &g.asteroids[i];
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

static void reset_step_feedback(void) {
    g.hover_asteroid = -1;
    g.beam_active = false;
    g.beam_hit = false;
    g.thrusting = false;
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
    intent.sell = is_key_pressed(SAPP_KEYCODE_E);
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

    float world_distance = v2_len(g.ship.pos);
    if (world_distance > WORLD_RADIUS) {
        vec2 push_home = v2_scale(v2_norm(g.ship.pos), -(world_distance - WORLD_RADIUS) * 0.08f);
        g.ship.vel = v2_add(g.ship.vel, push_home);
    }
}

static void step_asteroid_spin(float dt) {
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        g.asteroids[i].rotation += g.asteroids[i].spin * dt;
    }
}

static void resolve_world_collisions(void) {
    resolve_ship_circle(g.station.pos, g.station.radius + 4.0f);
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        resolve_ship_circle(g.asteroids[i].pos, g.asteroids[i].radius);
    }
}

static void update_docking_state(float dt) {
    g.docked = (v2_len(v2_sub(g.ship.pos, g.station.pos)) <= g.station.dock_radius);
    if (g.docked) {
        g.ship.vel = v2_scale(g.ship.vel, 1.0f / (1.0f + (dt * 2.2f)));
    }
}

static void update_targeting_state(vec2 forward) {
    g.hover_asteroid = find_mining_target(ship_muzzle(forward), forward);
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

        if (g.ship.cargo < SHIP_CARGO_MAX) {
            float mined = MINING_RATE * dt;
            mined = fminf(mined, asteroid->ore);
            mined = fminf(mined, SHIP_CARGO_MAX - g.ship.cargo);
            g.ship.cargo += mined;
            asteroid->ore -= mined;
            if (asteroid->ore <= 0.01f) {
                respawn_asteroid(asteroid);
                set_notice("Asteroid depleted. New ore drifted in.");
            }
        }
    } else {
        g.beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
    }
}

static void step_station_interaction_system(bool sell) {
    if (!sell) {
        return;
    }

    if (!g.docked) {
        set_notice("Enter station ring to sell.");
    } else if (g.ship.cargo <= 0.01f) {
        set_notice("Cargo hold empty.");
    } else {
        int sold_units = (int)lroundf(g.ship.cargo);
        int payout = (int)lroundf(g.ship.cargo * ORE_PRICE);
        g.ship.credits += g.ship.cargo * ORE_PRICE;
        g.ship.cargo = 0.0f;
        set_notice("Sold %d ore for %d cr.", sold_units, payout);
    }
}

static void step_notice_timer(float dt) {
    if (g.notice_timer > 0.0f) {
        g.notice_timer = fmaxf(0.0f, g.notice_timer - dt);
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

    step_ship_rotation(dt, intent.turn);
    vec2 forward = ship_forward();
    step_ship_thrust(dt, intent.thrust, forward);
    step_ship_motion(dt);
    step_asteroid_spin(dt);
    resolve_world_collisions();
    update_docking_state(dt);

    forward = ship_forward();
    update_targeting_state(forward);
    step_mining_system(dt, intent.mine, forward);
    step_station_interaction_system(intent.sell);
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
        draw_asteroid(&g.asteroids[i], i == g.hover_asteroid);
    }
    draw_beam();
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
