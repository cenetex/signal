#include <stdarg.h>
#include <stdlib.h>

#include "client.h"
#include "audio.h"
#include "npc.h"
#include "render.h"
#include "net.h"

/* SOKOL_IMPL must appear in exactly one .c file.
 * The declaration-only headers are already pulled in by client.h,
 * so we just define the _IMPL macros and re-include for the bodies. */
#define SOKOL_IMPL
#define SOKOL_APP_IMPL
#define SOKOL_GFX_IMPL
#define SOKOL_GL_IMPL
#define SOKOL_DEBUGTEXT_IMPL
#define SOKOL_AUDIO_IMPL
#include "sokol_app.h"
#include "sokol_audio.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "sokol_log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

/* Types, game_t, and HUD constants are in client.h */

game_t g;

static const int MAX_SIM_STEPS_PER_FRAME = 8;

static uint32_t rng_next(void) {
    if (g.world.rng == 0) {
        g.world.rng = 0xA341316Cu;
    }
    uint32_t x = g.world.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g.world.rng = x;
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

/* asteroid_next_tier, asteroid_is_collectible, asteroid_progress_ratio: see asteroid.h/c */

/* commodity_refined_form, commodity_name, commodity_code, commodity_short_name: see commodity.h/c */

static commodity_t random_raw_ore(void) {
    return (commodity_t)rand_int((int)COMMODITY_FERRITE_ORE, (int)COMMODITY_CRYSTAL_ORE);
}

/* ship_total_cargo, ship_raw_ore_total, ship_cargo_amount, station_buy_price, station_inventory_amount: see commodity.h/c */

static void clear_ship_cargo(void) {
    memset(LOCAL_PLAYER.ship.cargo, 0, sizeof(LOCAL_PLAYER.ship.cargo));
}

/* format_ore_manifest ... format_refinery_price_line: see station_ui.c */
/* station_at ... navigation_station_ptr: see station_ui.c */
/* station_role_name, station_role_short_name: see station_ui.c */
/* station_has_service, station_upgrade_service: see station_ui.c */
/* build_station_ui_state, format_station_* helpers: see station_ui.c */
/* station_role_hub_label, station_role_market_title, station_role_fit_title: see station_ui.c */
/* station_role_color: see station_ui.c */
/* build_station_service_lines, draw_station_service_text_line: see station_ui.c */
/* can_afford_upgrade: see economy.h/c */

static vec2 station_dock_anchor(void) {
    const station_t* station = current_station_ptr();
    if (station == NULL) {
        return v2(0.0f, 0.0f);
    }
    return v2_add(station->pos, v2(0.0f, -(station->radius + ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius + STATION_DOCK_APPROACH_OFFSET)));
}

/* station_has_service: see station_ui.c */
/* station_cargo_sale_value, station_repair_cost: see economy.h/c */

static float ship_cargo_space(void) {
    return fmaxf(0.0f, ship_cargo_capacity(&LOCAL_PLAYER.ship) - ship_total_cargo(&LOCAL_PLAYER.ship));
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

static asteroid_tier_t random_field_asteroid_tier(void) {
    float roll = randf();
    if (roll < 0.03f) {
        return ASTEROID_TIER_XXL;
    }
    if (roll < 0.26f) {
        return ASTEROID_TIER_XL;
    }
    if (roll < 0.70f) {
        return ASTEROID_TIER_L;
    }
    return ASTEROID_TIER_M;
}

static float client_max_signal_range(void) {
    float best = 0.0f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        if (g.world.stations[i].signal_range > best) best = g.world.stations[i].signal_range;
    }
    return best > 0.0f ? best : WORLD_RADIUS;
}

static void spawn_field_asteroid_of_tier(asteroid_t* asteroid, asteroid_tier_t tier) {
    float angle = rand_range(0.0f, TWO_PI_F);
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, random_raw_ore(), &g.world.rng);
    asteroid->fracture_child = false;
    /* Pick a random station and spawn within its signal range */
    int stn = rand_int(0, MAX_STATIONS - 1);
    float sr = g.world.stations[stn].signal_range;
    if (sr <= 0.0f) sr = client_max_signal_range();
    if (tier == ASTEROID_TIER_XXL) {
        vec2 center = g.world.stations[stn].pos;
        asteroid->pos = v2_add(center, v2(cosf(angle) * sr, sinf(angle) * sr));
        float inward_speed = rand_range(15.0f, 30.0f);
        asteroid->vel = v2(-cosf(angle) * inward_speed, -sinf(angle) * inward_speed);
    } else {
        vec2 center = g.world.stations[stn].pos;
        float distance = rand_range(420.0f, sr - 180.0f);
        asteroid->pos = v2_add(center, v2(cosf(angle) * distance, sinf(angle) * distance));
        asteroid->vel = v2(rand_range(-4.0f, 4.0f), rand_range(-4.0f, 4.0f));
    }
}

static void spawn_field_asteroid(asteroid_t* asteroid) {
    spawn_field_asteroid_of_tier(asteroid, random_field_asteroid_tier());
}

static void spawn_child_asteroid(asteroid_t* asteroid, asteroid_tier_t tier, commodity_t commodity, vec2 pos, vec2 vel) {
    clear_asteroid(asteroid);
    configure_asteroid_tier(asteroid, tier, commodity, &g.world.rng);
    asteroid->fracture_child = true;
    asteroid->pos = pos;
    asteroid->vel = vel;
}

static int desired_child_count(asteroid_tier_t tier) {
    switch (tier) {
        case ASTEROID_TIER_XXL:
            return rand_int(8, 14);
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
        if (!g.world.asteroids[i].active) {
            if (*first_inactive_slot < 0) {
                *first_inactive_slot = i;
            }
            continue;
        }

        if (!g.world.asteroids[i].fracture_child) {
            (*seeded_count)++;
        }
    }
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
    world_reset(&g.world);
    player_init_ship(&LOCAL_PLAYER, &g.world);
    LOCAL_PLAYER.connected = true;

    g.tracked_contract = -1;
    memset(&g.asteroid_interp, 0, sizeof(g.asteroid_interp));
    g.asteroid_interp.interval = 0.1f;
    memset(&g.npc_interp, 0, sizeof(g.npc_interp));
    g.npc_interp.interval = 0.1f;

    g.thrusting = false;
    g.notice[0] = '\0';
    g.notice_timer = 0.0f;
    audio_clear_voices(&g.audio);
    clear_collection_feedback();

    set_notice("%s online. Press E to launch.", g.world.stations[LOCAL_PLAYER.current_station].name);
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
}

static void draw_station(const station_t* station, bool is_current, bool is_nearby) {
    if (!station_exists(station)) return;

    float role_r = 0.45f;
    float role_g = 0.85f;
    float role_b = 1.0f;
    module_type_t dom = station_dominant_module(station);
    float pulse = 0.35f + (sinf((g.world.time * 2.1f) + (float)dom) * 0.15f);
    int spoke_count = 6;

    station_role_color(station, &role_r, &role_g, &role_b);
    if (dom == MODULE_FRAME_PRESS) {
        spoke_count = 4;
    } else if (dom == MODULE_LASER_FAB || dom == MODULE_TRACTOR_FAB) {
        spoke_count = 5;
    } else if (dom == MODULE_SIGNAL_RELAY) {
        spoke_count = 3;
    }

    /* Scaffold rendering: dashed outline, translucent */
    if (station->scaffold) {
        float alpha = 0.3f + 0.2f * sinf(g.world.time * 1.5f);
        float prog = station->scaffold_progress;
        /* Dashed dock circle */
        int dash_segs = 24;
        float step = TWO_PI_F / (float)dash_segs;
        for (int i = 0; i < dash_segs; i += 2) {
            float a0 = (float)i * step;
            float a1 = (float)(i + 1) * step;
            vec2 p0 = v2_add(station->pos, v2(cosf(a0) * station->dock_radius, sinf(a0) * station->dock_radius));
            vec2 p1 = v2_add(station->pos, v2(cosf(a1) * station->dock_radius, sinf(a1) * station->dock_radius));
            draw_segment(p0, p1, role_r * 0.5f, role_g * 0.5f, role_b * 0.5f, alpha);
        }
        /* Station body outline */
        draw_circle_outline(station->pos, station->radius, 18, role_r * 0.6f, role_g * 0.6f, role_b * 0.6f, alpha + 0.15f);
        /* Progress ring: filled portion */
        if (prog > 0.01f) {
            int filled_segs = (int)(prog * 24.0f);
            float fill_step = TWO_PI_F / 24.0f;
            for (int i = 0; i < filled_segs && i < 24; i++) {
                float a0 = (float)i * fill_step;
                float a1 = (float)(i + 1) * fill_step;
                vec2 p0 = v2_add(station->pos, v2(cosf(a0) * (station->radius + 12.0f), sinf(a0) * (station->radius + 12.0f)));
                vec2 p1 = v2_add(station->pos, v2(cosf(a1) * (station->radius + 12.0f), sinf(a1) * (station->radius + 12.0f)));
                draw_segment(p0, p1, role_r, role_g, role_b, 0.8f);
            }
        }
        return;
    }

    float dock_alpha = is_current ? 0.62f : (is_nearby ? 0.50f : 0.16f + pulse);
    draw_circle_outline(station->pos, station->dock_radius, 48, role_r * 0.72f, role_g, role_b, dock_alpha);
    draw_circle_filled(station->pos, station->radius, 28, 0.08f, 0.12f, 0.17f, 1.0f);
    draw_circle_outline(station->pos, station->radius + 8.0f, 28, role_r, role_g, role_b, 0.92f);
    draw_circle_filled(station->pos, 18.0f, 18, role_r * 0.68f, role_g * 0.88f, role_b * 0.96f, 1.0f);

    for (int i = 0; i < spoke_count; i++) {
        float angle = (TWO_PI_F / (float)spoke_count) * (float)i + g.world.time * (0.14f + ((float)dom * 0.02f));
        vec2 inner = v2_add(station->pos, v2(cosf(angle) * 28.0f, sinf(angle) * 28.0f));
        vec2 outer = v2_add(station->pos, v2(cosf(angle) * 48.0f, sinf(angle) * 48.0f));
        draw_segment(inner, outer, role_r * 0.8f, role_g * 0.92f, role_b, 0.85f);

        if (dom == MODULE_FURNACE) {
            vec2 mid = v2_add(station->pos, v2(cosf(angle) * 58.0f, sinf(angle) * 58.0f));
            draw_rect_centered(mid, 4.0f, 4.0f, role_r * 0.8f, role_g, role_b * 0.9f, 0.85f);
        }
    }

    if (dom == MODULE_FRAME_PRESS) {
        draw_rect_outline(station->pos, 18.0f, 18.0f, role_r * 0.82f, role_g * 0.86f, role_b * 0.76f, 0.75f);
    } else if (dom == MODULE_LASER_FAB || dom == MODULE_TRACTOR_FAB) {
        draw_segment(v2_add(station->pos, v2(-24.0f, 0.0f)), v2_add(station->pos, v2(24.0f, 0.0f)), role_r, role_g, role_b, 0.78f);
        draw_segment(v2_add(station->pos, v2(0.0f, -24.0f)), v2_add(station->pos, v2(0.0f, 24.0f)), role_r, role_g, role_b, 0.40f);
    } else if (dom == MODULE_SIGNAL_RELAY) {
        /* Triangle marker for activated outpost */
        float s = 14.0f;
        draw_segment(v2_add(station->pos, v2(0.0f, -s)), v2_add(station->pos, v2(s, s * 0.6f)), role_r, role_g, role_b, 0.75f);
        draw_segment(v2_add(station->pos, v2(s, s * 0.6f)), v2_add(station->pos, v2(-s, s * 0.6f)), role_r, role_g, role_b, 0.75f);
        draw_segment(v2_add(station->pos, v2(-s, s * 0.6f)), v2_add(station->pos, v2(0.0f, -s)), role_r, role_g, role_b, 0.75f);
    }

}

static void draw_asteroid(const asteroid_t* asteroid, bool targeted, bool ineffective) {
    float progress_ratio = asteroid_progress_ratio(asteroid);
    float body_r = 0.3f;
    float body_g = 0.3f;
    float body_b = 0.3f;
    int segments = 18;
    asteroid_body_color(asteroid->tier, asteroid->commodity, progress_ratio, &body_r, &body_g, &body_b);

    switch (asteroid->tier) {
        case ASTEROID_TIER_XXL:
            segments = 28;
            break;
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
    {
        float step = TWO_PI_F / (float)segments;
        float a0 = asteroid->rotation;
        float r0 = asteroid_profile(asteroid, a0);
        float prev_x = asteroid->pos.x + cosf(a0) * r0;
        float prev_y = asteroid->pos.y + sinf(a0) * r0;
        for (int i = 1; i <= segments; i++) {
            float a1 = asteroid->rotation + (float)i * step;
            float r1 = asteroid_profile(asteroid, a1);
            float cx = asteroid->pos.x + cosf(a1) * r1;
            float cy = asteroid->pos.y + sinf(a1) * r1;
            sgl_v2f(asteroid->pos.x, asteroid->pos.y);
            sgl_v2f(prev_x, prev_y);
            sgl_v2f(cx, cy);
            prev_x = cx;
            prev_y = cy;
        }
    }
    sgl_end();

    float rim_r = targeted ? (ineffective ? 1.0f : 0.45f) : (body_r * 0.85f);
    float rim_g = targeted ? (ineffective ? 0.15f : 0.94f) : (body_g * 0.95f);
    float rim_b = targeted ? (ineffective ? 0.1f : 1.0f) : fminf(1.0f, body_b * 1.2f);
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
        float cr, cg, cb;
        commodity_material_tint(asteroid->commodity, &cr, &cg, &cb);
        float glow_r = lerpf(0.48f, cr * 1.6f, 0.5f);
        float glow_g = lerpf(0.96f, cg * 1.6f, 0.5f);
        float glow_b = lerpf(0.78f, cb * 1.6f, 0.5f);
        draw_circle_filled(asteroid->pos, asteroid->radius * lerpf(0.14f, 0.24f, progress_ratio), 10, glow_r, glow_g, glow_b, lerpf(0.35f, 0.8f, progress_ratio));
    } else if (asteroid->tier == ASTEROID_TIER_M) {
        float cr, cg, cb;
        commodity_material_tint(asteroid->commodity, &cr, &cg, &cb);
        float glow_r = lerpf(0.36f, cr * 1.4f, 0.4f);
        float glow_g = lerpf(0.78f, cg * 1.4f, 0.4f);
        float glow_b = lerpf(0.98f, cb * 1.4f, 0.4f);
        draw_circle_filled(asteroid->pos, asteroid->radius * 0.16f, 8, glow_r, glow_g, glow_b, 0.4f);
    }

    if (targeted && ineffective) {
        draw_circle_outline(asteroid->pos, asteroid->radius + 12.0f, 24, 1.0f, 0.2f, 0.15f, 0.75f);
    } else if (targeted) {
        draw_circle_outline(asteroid->pos, asteroid->radius + 12.0f, 24, 0.35f, 1.0f, 0.92f, 0.75f);
    }
}

static void draw_ship_tractor_field(void) {
    if (LOCAL_PLAYER.nearby_fragments <= 0) {
        return;
    }

    float pulse = 0.28f + (sinf(g.world.time * 7.0f) * 0.08f);
    draw_circle_outline(LOCAL_PLAYER.ship.pos, ship_tractor_range(&LOCAL_PLAYER.ship), 40, 0.24f, 0.86f, 1.0f, pulse);
    if (LOCAL_PLAYER.tractor_fragments > 0) {
        draw_circle_outline(LOCAL_PLAYER.ship.pos, ship_collect_radius(&LOCAL_PLAYER.ship) + 6.0f, 28, 0.50f, 1.0f, 0.82f, 0.75f);
    }
}

static void draw_ship(void) {
    sgl_push_matrix();
    sgl_translate(LOCAL_PLAYER.ship.pos.x, LOCAL_PLAYER.ship.pos.y, 0.0f);
    sgl_rotate(LOCAL_PLAYER.ship.angle, 0.0f, 0.0f, 1.0f);

    if (g.thrusting) {
        float flicker = 10.0f + sinf(g.world.time * 42.0f) * 3.0f;
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

static void draw_npc_ship(const npc_ship_t* npc) {
    const hull_def_t* hull = npc_hull_def(npc);
    bool is_hauler = npc->hull_class == HULL_CLASS_HAULER;
    float scale = hull->render_scale;
    /* Use accumulated ore tint — starts white, absorbs cargo colors over time */
    float hull_r = npc->tint_r;
    float hull_g = npc->tint_g;
    float hull_b = npc->tint_b;

    sgl_push_matrix();
    sgl_translate(npc->pos.x, npc->pos.y, 0.0f);
    sgl_rotate(npc->angle, 0.0f, 0.0f, 1.0f);
    sgl_scale(scale, scale, 1.0f);

    if (npc->thrusting) {
        float flicker = 8.0f + sinf(g.world.time * 38.0f + npc->pos.x) * 2.5f;
        sgl_c4f(1.0f, 0.6f, 0.15f, 0.9f);
        sgl_begin_triangles();
        sgl_v2f(-12.0f, 0.0f);
        sgl_v2f(-26.0f - flicker, 6.0f);
        sgl_v2f(-26.0f - flicker, -6.0f);
        sgl_end();
    }

    sgl_c4f(hull_r, hull_g, hull_b, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(22.0f, 0.0f);
    sgl_v2f(-14.0f, 12.0f);
    sgl_v2f(-14.0f, -12.0f);
    sgl_end();

    sgl_c4f(hull_r * 0.3f, hull_g * 0.3f, hull_b * 0.3f, 1.0f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-5.0f, 5.5f);
    sgl_v2f(-5.0f, -5.5f);
    sgl_end();

    draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), hull_r * 0.9f, hull_g * 0.8f, hull_b * 0.3f, 0.85f);
    draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), hull_r * 0.9f, hull_g * 0.8f, hull_b * 0.3f, 0.85f);

    sgl_pop_matrix();
}

static void draw_npc_mining_beam(const npc_ship_t* npc) {
    if (npc->state != NPC_STATE_MINING) return;
    if (npc->target_asteroid < 0) return;
    const asteroid_t* asteroid = &g.world.asteroids[npc->target_asteroid];
    if (!asteroid->active) return;

    vec2 forward = v2_from_angle(npc->angle);
    vec2 muzzle = v2_add(npc->pos, v2_scale(forward, npc_hull_def(npc)->ship_radius + 5.0f));
    vec2 to_target = v2_sub(asteroid->pos, muzzle);
    vec2 hit = v2_sub(asteroid->pos, v2_scale(v2_norm(to_target), asteroid->radius * 0.85f));

    draw_segment(muzzle, hit, 0.92f, 0.68f, 0.28f, 0.85f);
    draw_segment(muzzle, hit, 0.45f, 0.30f, 0.10f, 0.35f);
}

static void draw_npc_ships(void) {
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!g.world.npc_ships[i].active) continue;
        draw_npc_ship(&g.world.npc_ships[i]);
        draw_npc_mining_beam(&g.world.npc_ships[i]);
    }
}

static void draw_beam(void) {
    if (!LOCAL_PLAYER.beam_active) {
        return;
    }

    if (LOCAL_PLAYER.beam_hit && LOCAL_PLAYER.beam_ineffective) {
        /* Red beam: hitting a rock too tough for current laser */
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 1.0f, 0.2f, 0.15f, 0.85f);
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.8f, 0.1f, 0.05f, 0.30f);
    } else if (LOCAL_PLAYER.beam_hit) {
        /* Normal mining beam: teal */
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.45f, 1.0f, 0.92f, 0.95f);
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.12f, 0.78f, 1.0f, 0.35f);
    } else {
        /* Beam into empty space */
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.9f, 0.75f, 0.30f, 0.55f);
    }
}

/* draw_ui_scanlines ... draw_hud: see hud.c */
/* draw_station_services: see station_ui.c */


static bool is_key_down(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_down[key];
}

static bool is_key_pressed(sapp_keycode key) {
    return (key >= 0) && (key < KEY_COUNT) && g.input.key_pressed[key];
}

static vec2 ship_forward(void) {
    return v2_from_angle(LOCAL_PLAYER.ship.angle);
}

static vec2 ship_muzzle(vec2 forward) {
    return v2_add(LOCAL_PLAYER.ship.pos, v2_scale(forward, ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius + 8.0f));
}

static void reset_step_feedback(void) {
    LOCAL_PLAYER.hover_asteroid = -1;
    LOCAL_PLAYER.beam_active = false;
    LOCAL_PLAYER.beam_hit = false;
    g.thrusting = false;
    LOCAL_PLAYER.nearby_fragments = 0;
    LOCAL_PLAYER.tractor_fragments = 0;
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
    /* Number keys: context-dependent */
    if (LOCAL_PLAYER.docked && g.build_overlay) {
        /* Build overlay: 1-8 select module, Esc/B closes */
        static const struct { module_type_t type; const char *name; } build_keys[] = {
            { MODULE_FURNACE,      "Furnace (FE)" },
            { MODULE_FURNACE_CU,   "Furnace (CU)" },
            { MODULE_FURNACE_CR,   "Furnace (CR)" },
            { MODULE_FRAME_PRESS,  "Frame Press" },
            { MODULE_LASER_FAB,    "Laser Fab" },
            { MODULE_TRACTOR_FAB,  "Tractor Fab" },
            { MODULE_ORE_BUYER,    "Ore Buyer" },
            { MODULE_SIGNAL_RELAY, "Signal Relay" },
        };
        for (int k = 0; k < 8; k++) {
            if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
            const station_t *st = current_station_ptr();
            if (station_has_module(st, build_keys[k].type)) {
                set_notice("%s already installed.", build_keys[k].name);
            } else if (st->module_count >= MAX_MODULES_PER_STATION) {
                set_notice("No module slots available.");
            } else {
                intent.build_module = true;
                intent.build_module_type = build_keys[k].type;
                set_notice("Blueprint placed: %s", build_keys[k].name);
            }
            break;
        }
        if (is_key_pressed(SAPP_KEYCODE_ESCAPE) || is_key_pressed(SAPP_KEYCODE_B))
            g.build_overlay = false;
    } else if (LOCAL_PLAYER.docked && g.station_tab == STATION_TAB_CONTRACTS) {
        /* Contracts tab: 1/2/3 track contract */
        for (int k = 0; k < 3; k++) {
            if (!is_key_pressed(SAPP_KEYCODE_1 + k)) continue;
            int nearest[3] = {-1, -1, -1};
            float nearest_d[3] = {1e18f, 1e18f, 1e18f};
            const station_t *here_st = current_station_ptr();
            if (!here_st) break;
            vec2 here = here_st->pos;
            for (int ci = 0; ci < MAX_CONTRACTS; ci++) {
                contract_t *ct = &g.world.contracts[ci];
                if (!ct->active || ct->station_index >= MAX_STATIONS) continue;
                if (!station_exists(&g.world.stations[ct->station_index])) continue;
                vec2 target = (ct->action == CONTRACT_SUPPLY) ? g.world.stations[ct->station_index].pos : ct->target_pos;
                float d = v2_dist_sq(here, target);
                for (int slot = 0; slot < 3; slot++) {
                    if (d < nearest_d[slot]) {
                        for (int j = 2; j > slot; j--) { nearest[j] = nearest[j-1]; nearest_d[j] = nearest_d[j-1]; }
                        nearest[slot] = ci;
                        nearest_d[slot] = d;
                        break;
                    }
                }
            }
            if (nearest[k] >= 0) {
                g.tracked_contract = nearest[k];
                set_notice("Contract tracked.");
            }
            break;
        }
    } else {
        /* Default: service keys */
        intent.service_sell = is_key_pressed(SAPP_KEYCODE_1);
        intent.service_repair = is_key_pressed(SAPP_KEYCODE_2);
        intent.upgrade_mining = is_key_pressed(SAPP_KEYCODE_3);
        intent.upgrade_hold = is_key_pressed(SAPP_KEYCODE_4);
        intent.upgrade_tractor = is_key_pressed(SAPP_KEYCODE_5);
    }
    /* Buy ingots from station (F key while docked) */
    if (LOCAL_PLAYER.docked && is_key_pressed(SAPP_KEYCODE_F)) {
        const station_t *st = current_station_ptr();
        if (st) {
            /* Buy the first available ingot type */
            for (int c = COMMODITY_RAW_ORE_COUNT; c < COMMODITY_COUNT; c++) {
                if (st->inventory[c] > 0.5f) {
                    float space = ship_cargo_capacity(&LOCAL_PLAYER.ship) - ship_total_cargo(&LOCAL_PLAYER.ship);
                    commodity_t src = commodity_ore_form((commodity_t)c);
                    float price = station_buy_price(st, src) * 2.0f;
                    if (space < 0.5f) {
                        set_notice("Hold full.");
                    } else if (LOCAL_PLAYER.ship.credits < price) {
                        set_notice("Need %d cr.", (int)lroundf(price));
                    } else {
                        float avail = st->inventory[c];
                        float afford = floorf(LOCAL_PLAYER.ship.credits / price);
                        int amount = (int)fminf(fminf(avail, space), afford);
                        intent.buy_product = true;
                        intent.buy_commodity = (commodity_t)c;
                        /* Optimistic client prediction */
                        LOCAL_PLAYER.ship.cargo[c] += (float)amount;
                        LOCAL_PLAYER.ship.credits -= (float)amount * price;
                        g.world.stations[LOCAL_PLAYER.current_station].inventory[c] -= (float)amount;
                        set_notice("Bought %d %s  -%d cr", amount, commodity_short_name((commodity_t)c), (int)(amount * price));
                    }
                    break;
                }
            }
        }
    }
    /* B key: build mode */
    if (g.placing_outpost) {
        /* Outpost placement: B/Enter confirms, Esc cancels */
        if (is_key_pressed(SAPP_KEYCODE_B) || is_key_pressed(SAPP_KEYCODE_ENTER) || is_key_pressed(SAPP_KEYCODE_KP_ENTER)) {
            intent.place_outpost = true;
            g.placing_outpost = false;
            vec2 fwd = v2_from_angle(LOCAL_PLAYER.ship.angle);
            g.nav_pip_active = true;
            g.nav_pip_pos = v2_add(LOCAL_PLAYER.ship.pos, v2_scale(fwd, 150.0f));
            g.nav_pip_is_blueprint = true;
        } else if (is_key_pressed(SAPP_KEYCODE_ESCAPE) || is_key_pressed(SAPP_KEYCODE_Q)) {
            g.placing_outpost = false;
        }
    } else if (is_key_pressed(SAPP_KEYCODE_B)) {
        if (LOCAL_PLAYER.docked) {
            if (g.build_overlay) {
                g.build_overlay = false;
            } else if (!LOCAL_PLAYER.ship.has_scaffold_kit) {
                /* Buy scaffold kit */
                const station_t *st = current_station_ptr();
                if (st && station_has_module(st, MODULE_BLUEPRINT_DESK)) {
                    if (LOCAL_PLAYER.ship.credits >= OUTPOST_CREDIT_COST) {
                        intent.buy_scaffold_kit = true;
                        set_notice("Scaffold kit purchased. Undock and press B to deploy.");
                    } else {
                        set_notice("Need %d cr for scaffold kit.", (int)OUTPOST_CREDIT_COST);
                    }
                } else {
                    g.build_overlay = true; /* show module build menu */
                }
            } else {
                g.build_overlay = true;
            }
        } else {
            if (LOCAL_PLAYER.ship.has_scaffold_kit) {
                g.placing_outpost = true;
            } else {
                set_notice("Buy a scaffold kit at a station first.");
            }
        }
    }
    intent.reset = is_key_pressed(SAPP_KEYCODE_R);
    return intent;
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

/* step_refinery_production, step_station_production: see economy.h/c */

/* No sync_globals_to_world — world_t is the source of truth in single player. */

/* sync_world_to_globals removed — everything reads from g.world directly */

static void sim_step(float dt) {
    reset_step_feedback();
    audio_step(&g.audio, dt);

    input_intent_t intent = sample_input_intent();
    if (intent.reset) {
        reset_world();
        consume_pressed_input();
        return;
    }

    /* Tab switching while docked */
    if (LOCAL_PLAYER.docked && !g.was_docked) {
        /* Just docked — reset to overview (or construction for scaffolds) */
        const station_t* st = &g.world.stations[LOCAL_PLAYER.current_station];
        g.station_tab = STATION_TAB_STATUS;
        g.build_overlay = false;
        g.placing_outpost = false;
        /* Clear blueprint pip if we docked at the blueprint station */
        if (g.nav_pip_is_blueprint) {
            float d = sqrtf(v2_dist_sq(st->pos, g.nav_pip_pos));
            if (d < 200.0f) {
                g.nav_pip_is_blueprint = false;
                g.nav_pip_pos = st->pos;
            }
            /* Otherwise keep the blueprint pip active */
        } else {
            g.nav_pip_active = true;
            g.nav_pip_pos = st->pos;
        }
    }
    g.was_docked = LOCAL_PLAYER.docked;
    if (LOCAL_PLAYER.docked && !g.build_overlay && (is_key_pressed(SAPP_KEYCODE_TAB) || is_key_pressed(SAPP_KEYCODE_Q))) {
        station_tab_t vtabs[STATION_TAB_COUNT];
        int vtab_count = 0;
        vtabs[vtab_count++] = STATION_TAB_STATUS;
        vtabs[vtab_count++] = STATION_TAB_MARKET;
        vtabs[vtab_count++] = STATION_TAB_CONTRACTS;
        int cur = 0;
        for (int i = 0; i < vtab_count; i++) { if (vtabs[i] == g.station_tab) { cur = i; break; } }
        int dir = is_key_pressed(SAPP_KEYCODE_TAB) ? 1 : (vtab_count - 1);
        g.station_tab = vtabs[(cur + dir) % vtab_count];
    }

    LOCAL_PLAYER.input = intent;
    if (g.multiplayer_enabled && net_is_connected()) {
        /* Multiplayer: only predict local player movement.
         * Asteroids, NPCs, production are server-authoritative
         * and interpolated from server snapshots. */
        world_sim_step_player_only(&g.world, g.local_player_slot, dt);

        /* Mining beam visual — step_player already set hover_asteroid and beam state.
         * Just ensure beam_ineffective is set for rendering. */

        /* Advance interpolation timers */
        g.asteroid_interp.t += dt / fmaxf(g.asteroid_interp.interval, 0.01f);
        g.npc_interp.t += dt / fmaxf(g.npc_interp.interval, 0.01f);
    } else {
        /* Single player: full authoritative sim */
        world_sim_step(&g.world, dt);
    }

    g.thrusting = (intent.thrust > 0.0f) && !LOCAL_PLAYER.docked;

    /* Play audio from sim events */
    for (int i = 0; i < g.world.events.count; i++) {
        sim_event_t* ev = &g.world.events.events[i];
        switch (ev->type) {
            case SIM_EVENT_FRACTURE:
                audio_play_fracture(&g.audio, ev->fracture.tier);
                break;
            case SIM_EVENT_MINING_TICK:
                if (ev->player_id == g.local_player_slot) audio_play_mining_tick(&g.audio);
                break;
            case SIM_EVENT_DOCK:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_dock(&g.audio);
                    set_notice("Docked at %s.", g.world.stations[LOCAL_PLAYER.current_station].name);
                }
                break;
            case SIM_EVENT_LAUNCH:
                if (ev->player_id == g.local_player_slot) {
                    audio_play_launch(&g.audio);
                    set_notice("Launch corridor clear.");
                }
                break;
            case SIM_EVENT_SELL:
                if (ev->player_id == g.local_player_slot) audio_play_sale(&g.audio);
                break;
            case SIM_EVENT_REPAIR:
                if (ev->player_id == g.local_player_slot) audio_play_repair(&g.audio);
                break;
            case SIM_EVENT_UPGRADE:
                if (ev->player_id == g.local_player_slot) audio_play_upgrade(&g.audio, ev->upgrade.upgrade);
                break;
            case SIM_EVENT_DAMAGE:
                if (ev->player_id == g.local_player_slot) audio_play_damage(&g.audio, ev->damage.amount);
                break;
            case SIM_EVENT_CONTRACT_COMPLETE:
                if (ev->contract_complete.action == CONTRACT_SUPPLY)
                    set_notice("Supply contract fulfilled.");
                else if (ev->contract_complete.action == CONTRACT_DESTROY)
                    set_notice("Target destroyed. Contract complete.");
                break;
            default:
                break;
        }
    }

    step_notice_timer(dt);
    if (g.dock_predict_timer > 0.0f)
        g.dock_predict_timer = fmaxf(0.0f, g.dock_predict_timer - dt);

    /* In multiplayer, also queue one-shot actions for network send. */
    if (g.multiplayer_enabled && net_is_connected()) {
        if (intent.interact) {
            g.pending_net_action = LOCAL_PLAYER.docked ? 2 : 1;
            g.dock_predict_timer = 0.5f;
        } else if (intent.service_sell)
            g.pending_net_action = 3;
        else if (intent.service_repair)
            g.pending_net_action = 4;
        else if (intent.upgrade_mining)
            g.pending_net_action = 5;
        else if (intent.upgrade_hold)
            g.pending_net_action = 6;
        else if (intent.upgrade_tractor)
            g.pending_net_action = 7;
        else if (intent.place_outpost)
            g.pending_net_action = 8;
        else if (intent.buy_scaffold_kit)
            g.pending_net_action = NET_ACTION_BUY_SCAFFOLD;
        else if (intent.build_module)
            g.pending_net_action = NET_ACTION_BUILD_MODULE + (uint8_t)intent.build_module_type;
        else if (intent.buy_product)
            g.pending_net_action = NET_ACTION_BUY_PRODUCT + (uint8_t)intent.buy_commodity;
    }

    consume_pressed_input();
}

/* Forward declarations for multiplayer callbacks (defined below init). */
static void on_player_join(uint8_t player_id);
static void on_player_leave(uint8_t player_id);
static void apply_remote_asteroids(const NetAsteroidState* asteroids, int count);
static void apply_remote_npcs(const NetNpcState* npcs, int count);
static void apply_remote_stations(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock);
static void apply_remote_station_identity(uint8_t index, uint8_t role, uint32_t services,
    float pos_x, float pos_y, float radius, float dock_radius, float signal_range, const char* name);
static void apply_remote_player_state(const NetPlayerState* state);
static void apply_remote_player_ship(const NetPlayerShipState* state);
static void sync_local_player_slot_from_network(void);

static void init(void) {
    memset(&g, 0, sizeof(g));
    g.world.rng = 0xC0FFEE12u;

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

    audio_init(&g.audio);

    g.pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.pass_action.colors[0].clear_value = (sg_color){ 0.018f, 0.024f, 0.045f, 1.0f };

    init_starfield();
    reset_world();

    /* --- Multiplayer: auto-connect if server URL is available --- */
    {
        const char* server_url = NULL;
#ifdef __EMSCRIPTEN__
        server_url = emscripten_run_script_string(
            "(() => {"
            "  const p = new URLSearchParams(window.location.search);"
            "  return p.get('server') || window.SIGNAL_SERVER || '';"
            "})()");
#else
        /* Native: check SIGNAL_SERVER environment variable or command line */
        server_url = getenv("SIGNAL_SERVER");
#endif
        if (server_url && server_url[0] != '\0') {
            NetCallbacks cbs = {0};
            cbs.on_join = on_player_join;
            cbs.on_leave = on_player_leave;
            cbs.on_state = apply_remote_player_state;
            cbs.on_asteroids = apply_remote_asteroids;
            cbs.on_npcs = apply_remote_npcs;
            cbs.on_stations = apply_remote_stations;
            cbs.on_station_identity = apply_remote_station_identity;
            cbs.on_player_ship = apply_remote_player_ship;
            g.multiplayer_enabled = net_init(server_url, &cbs);
        }
    }
}


/* --- Multiplayer: world state sync callbacks and broadcast --- */

static void on_player_join(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    g.world.players[player_id].connected = true;
    g.world.players[player_id].id = player_id;
    if ((int)player_id != g.local_player_slot)
        set_notice("Player %d joined.", (int)player_id);
}

static void on_player_leave(uint8_t player_id) {
    if (player_id >= MAX_PLAYERS) return;
    g.world.players[player_id].connected = false;
    if ((int)player_id != g.local_player_slot)
        set_notice("Player %d left.", (int)player_id);
}

static void apply_remote_asteroids(const NetAsteroidState* asteroids, int count) {
    /* Shift current -> previous for interpolation */
    memcpy(g.asteroid_interp.prev, g.asteroid_interp.curr, sizeof(g.asteroid_interp.prev));
    g.asteroid_interp.interval = fmaxf(g.asteroid_interp.t * g.asteroid_interp.interval, 0.05f);
    if (g.asteroid_interp.interval > 0.2f) g.asteroid_interp.interval = 0.1f;
    g.asteroid_interp.t = 0.0f;

    bool received[MAX_ASTEROIDS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = asteroids[i].index;
        if (idx >= MAX_ASTEROIDS) continue;
        received[idx] = true;

        asteroid_t* a = &g.asteroid_interp.curr[idx];
        a->active = (asteroids[i].flags & 1) != 0;
        a->fracture_child = (asteroids[i].flags & (1 << 1)) != 0;
        a->tier = (asteroid_tier_t)((asteroids[i].flags >> 2) & 0x7);
        a->commodity = (commodity_t)((asteroids[i].flags >> 5) & 0x7);
        a->pos.x = asteroids[i].x;
        a->pos.y = asteroids[i].y;
        a->vel.x = asteroids[i].vx;
        a->vel.y = asteroids[i].vy;
        a->hp    = asteroids[i].hp;
        a->ore   = asteroids[i].ore;
        a->radius = asteroids[i].radius;
        if (a->max_hp < a->hp) a->max_hp = a->hp;
        if (a->max_ore < a->ore) a->max_ore = a->ore;
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!received[i] && g.asteroid_interp.curr[i].active) {
            /* Not in this delta — extrapolate position from velocity.
             * Shift prev to current extrapolated position for smooth interp. */
            g.asteroid_interp.prev[i] = g.asteroid_interp.curr[i];
            g.asteroid_interp.curr[i].pos.x += g.asteroid_interp.curr[i].vel.x * g.asteroid_interp.interval;
            g.asteroid_interp.curr[i].pos.y += g.asteroid_interp.curr[i].vel.y * g.asteroid_interp.interval;
        }
    }

    /* Also copy to world for game logic (targeting, beam hit checks) */
    memcpy(g.world.asteroids, g.asteroid_interp.curr, sizeof(g.world.asteroids));
}

static void apply_remote_npcs(const NetNpcState* npcs, int count) {
    memcpy(g.npc_interp.prev, g.npc_interp.curr, sizeof(g.npc_interp.prev));
    g.npc_interp.interval = fmaxf(g.npc_interp.t * g.npc_interp.interval, 0.05f);
    if (g.npc_interp.interval > 0.2f) g.npc_interp.interval = 0.1f;
    g.npc_interp.t = 0.0f;

    bool received[MAX_NPC_SHIPS];
    memset(received, 0, sizeof(received));

    for (int i = 0; i < count; i++) {
        uint8_t idx = npcs[i].index;
        if (idx >= MAX_NPC_SHIPS) continue;
        received[idx] = true;

        npc_ship_t* n = &g.npc_interp.curr[idx];
        n->active = (npcs[i].flags & 1) != 0;
        n->role = (npc_role_t)((npcs[i].flags >> 1) & 0x3);
        n->state = (npc_state_t)((npcs[i].flags >> 3) & 0x7);
        n->thrusting = (npcs[i].flags & (1 << 6)) != 0;
        n->pos.x = npcs[i].x;
        n->pos.y = npcs[i].y;
        n->vel.x = npcs[i].vx;
        n->vel.y = npcs[i].vy;
        n->angle = npcs[i].angle;
        n->target_asteroid = (int)npcs[i].target_asteroid;
        n->tint_r = (float)npcs[i].tint_r / 255.0f;
        n->tint_g = (float)npcs[i].tint_g / 255.0f;
        n->tint_b = (float)npcs[i].tint_b / 255.0f;
    }

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!received[i]) {
            g.npc_interp.curr[i].active = false;
        }
    }

    memcpy(g.world.npc_ships, g.npc_interp.curr, sizeof(g.world.npc_ships));
}

static void apply_remote_stations(uint8_t index, const float* ore_buf, const float* inventory, const float* product_stock) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    for (int i = 0; i < COMMODITY_RAW_ORE_COUNT; i++)
        st->ore_buffer[i] = ore_buf[i];
    for (int i = 0; i < COMMODITY_COUNT; i++)
        st->inventory[i] = inventory[i];
    for (int i = 0; i < PRODUCT_COUNT; i++)
        st->product_stock[i] = product_stock[i];
    /* scaffold and scaffold_progress will be updated via a future network message */
}

static void apply_remote_station_identity(uint8_t index, uint8_t flags, uint32_t services,
    float pos_x, float pos_y, float radius, float dock_radius, float signal_range,
    const char* name) {
    if (index >= MAX_STATIONS) return;
    station_t* st = &g.world.stations[index];
    st->scaffold = (flags & 1) != 0;
    st->services = services;
    st->pos = v2(pos_x, pos_y);
    st->radius = radius;
    st->dock_radius = dock_radius;
    st->signal_range = signal_range;
    snprintf(st->name, sizeof(st->name), "%s", name);
}

static void apply_remote_player_state(const NetPlayerState* state) {
    /* Reconcile local prediction with server-authoritative position. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    /* Position reconciliation: blend gently for small divergence,
     * snap for large divergence (e.g., server teleported us). */
    float dx = state->x - sp->ship.pos.x;
    float dy = state->y - sp->ship.pos.y;
    float dist_sq = dx * dx + dy * dy;
    if (dist_sq > 500.0f * 500.0f) {
        /* Large divergence: snap to server (server probably docked/teleported us) */
        sp->ship.pos.x = state->x;
        sp->ship.pos.y = state->y;
        sp->ship.vel.x = state->vx;
        sp->ship.vel.y = state->vy;
    } else {
        /* Small divergence: gentle blend */
        float t = dist_sq > 100.0f * 100.0f ? 0.15f : 0.08f;
        sp->ship.pos.x = lerpf(sp->ship.pos.x, state->x, t);
        sp->ship.pos.y = lerpf(sp->ship.pos.y, state->y, t);
        sp->ship.vel.x = lerpf(sp->ship.vel.x, state->vx, t);
        sp->ship.vel.y = lerpf(sp->ship.vel.y, state->vy, t);
    }
    sp->ship.angle = lerp_angle(sp->ship.angle, state->angle, 0.1f);
}

static void apply_remote_player_ship(const NetPlayerShipState* state) {
    /* Apply server-authoritative ship state for the local player. */
    if (state->player_id != net_local_id() || state->player_id >= MAX_PLAYERS) return;

    server_player_t* sp = &g.world.players[state->player_id];
    sp->ship.hull = state->hull;
    sp->ship.credits = state->credits;
    sp->ship.mining_level = (int)state->mining_level;
    sp->ship.hold_level = (int)state->hold_level;
    sp->ship.tractor_level = (int)state->tractor_level;
    sp->ship.has_scaffold_kit = state->has_scaffold_kit;
    for (int c = 0; c < COMMODITY_COUNT; c++)
        sp->ship.cargo[c] = state->cargo[c];
    /* Dock-state reconciliation: the server is authoritative, but we
     * must not snap back to docked while the local sim has already
     * launched (the server may not have processed the action yet).
     * - Server says undocked → always accept.
     * - Server says docked  → only accept if we locally agree
     *   (still docked or within the dock-prediction guard window). */
    if (!state->docked) {
        sp->docked = false;
    } else if (sp->docked || g.dock_predict_timer <= 0.0f) {
        sp->docked = true;
        sp->current_station = (int)state->current_station;
        sp->in_dock_range = true;
        sp->nearby_station = sp->current_station;
    }
}

static void sync_local_player_slot_from_network(void) {
    uint8_t net_id = net_local_id();
    if (net_id == 0xFF || net_id >= MAX_PLAYERS) return;
    if (g.local_player_slot == (int)net_id) {
        LOCAL_PLAYER.connected = true;
        return;
    }

    server_player_t previous = g.world.players[g.local_player_slot];
    server_player_t* assigned = &g.world.players[net_id];
    memset(&g.world.players[g.local_player_slot], 0, sizeof(g.world.players[g.local_player_slot]));
    g.local_player_slot = (int)net_id;
    if (!assigned->connected && assigned->ship.hull <= 0.0f) {
        *assigned = previous;
    }
    LOCAL_PLAYER.id = net_id;
    LOCAL_PLAYER.connected = true;
    LOCAL_PLAYER.conn = NULL;
}

/* --- Multiplayer: draw remote players as colored triangles --- */
static void draw_remote_players(void) {
    if (!g.multiplayer_enabled) return;
    const NetPlayerState* players = net_get_players();
    static const float colors[][3] = {
        {1.0f, 0.45f, 0.25f},
        {0.25f, 1.0f, 0.55f},
        {0.55f, 0.35f, 1.0f},
        {1.0f, 0.85f, 0.15f},
        {0.15f, 0.85f, 1.0f},
        {1.0f, 0.35f, 0.75f},
    };
    for (int i = 0; i < NET_MAX_PLAYERS; i++) {
        if (!players[i].active) continue;
        if (i == (int)net_local_id()) continue;
        int ci = i % 6;
        float cr = colors[ci][0], cg = colors[ci][1], cb = colors[ci][2];
        bool thrusting = (players[i].flags & 1) != 0;
        bool mining = (players[i].flags & 2) != 0;

        sgl_push_matrix();
        sgl_translate(players[i].x, players[i].y, 0.0f);
        sgl_rotate(players[i].angle, 0.0f, 0.0f, 1.0f);

        /* Thrust flame */
        if (thrusting) {
            float flicker = 10.0f + sinf(g.world.time * 42.0f + (float)i * 7.0f) * 3.0f;
            sgl_c4f(1.0f, 0.74f, 0.24f, 0.9f);
            sgl_begin_triangles();
            sgl_v2f(-12.0f, 0.0f);
            sgl_v2f(-26.0f - flicker, 6.0f);
            sgl_v2f(-26.0f - flicker, -6.0f);
            sgl_end();
        }

        /* Hull */
        sgl_c4f(cr, cg, cb, 0.9f);
        sgl_begin_triangles();
        sgl_v2f(22.0f, 0.0f);
        sgl_v2f(-14.0f, 12.0f);
        sgl_v2f(-14.0f, -12.0f);
        sgl_end();

        /* Cockpit */
        sgl_c4f(cr * 0.3f, cg * 0.3f, cb * 0.3f, 1.0f);
        sgl_begin_triangles();
        sgl_v2f(8.0f, 0.0f);
        sgl_v2f(-5.0f, 5.5f);
        sgl_v2f(-5.0f, -5.5f);
        sgl_end();

        /* Wing struts */
        draw_segment(v2(-9.0f, 8.0f), v2(-15.0f, 17.0f), cr * 0.7f, cg * 0.7f, cb * 0.7f, 0.85f);
        draw_segment(v2(-9.0f, -8.0f), v2(-15.0f, -17.0f), cr * 0.7f, cg * 0.7f, cb * 0.7f, 0.85f);

        sgl_pop_matrix();

        /* Mining beam */
        if (mining) {
            vec2 pos = v2(players[i].x, players[i].y);
            vec2 forward = v2_from_angle(players[i].angle);
            vec2 muzzle = v2_add(pos, v2_scale(forward, 24.0f));
            vec2 beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
            draw_segment(muzzle, beam_end, cr, cg, cb, 0.6f);
        }
    }
}

static void render_world(void) {
    vec2 camera = LOCAL_PLAYER.ship.pos;
    float half_w = sapp_widthf() * 0.5f;
    float half_h = sapp_heightf() * 0.5f;

    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(camera.x - half_w, camera.x + half_w, camera.y - half_h, camera.y + half_h, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();

    draw_background(camera);
    for (int i = 0; i < MAX_STATIONS; i++) {
        bool is_current = LOCAL_PLAYER.docked && (i == LOCAL_PLAYER.current_station);
        bool is_nearby = (!LOCAL_PLAYER.docked) && (i == LOCAL_PLAYER.nearby_station);
        draw_station(&g.world.stations[i], is_current, is_nearby);
    }
    /* Outpost placement preview */
    if (g.placing_outpost && !LOCAL_PLAYER.docked) {
        vec2 forward = v2_from_angle(LOCAL_PLAYER.ship.angle);
        vec2 target = v2_add(LOCAL_PLAYER.ship.pos, v2_scale(forward, 150.0f));
        bool valid = can_place_outpost(&g.world, target)
                  && LOCAL_PLAYER.ship.credits >= OUTPOST_CREDIT_COST;
        float cr = valid ? 0.3f : 0.9f;
        float cg = valid ? 0.9f : 0.2f;
        float cb = valid ? 0.5f : 0.2f;
        float pulse = 0.5f + 0.3f * sinf(g.world.time * 4.0f);
        draw_circle_outline(target, OUTPOST_RADIUS, 18, cr, cg, cb, pulse);
        draw_circle_outline(target, OUTPOST_DOCK_RADIUS, 24, cr * 0.6f, cg * 0.6f, cb * 0.6f, pulse * 0.5f);
        /* Crosshair */
        draw_segment(v2_add(target, v2(-20.0f, 0.0f)), v2_add(target, v2(20.0f, 0.0f)), cr, cg, cb, pulse * 0.7f);
        draw_segment(v2_add(target, v2(0.0f, -20.0f)), v2_add(target, v2(0.0f, 20.0f)), cr, cg, cb, pulse * 0.7f);
    }

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        if (!g.world.asteroids[i].active) {
            continue;
        }
        bool is_target = (i == LOCAL_PLAYER.hover_asteroid);
        draw_asteroid(&g.world.asteroids[i], is_target, is_target && LOCAL_PLAYER.beam_ineffective);
    }
    draw_beam();
    draw_ship_tractor_field();
    draw_ship();
    draw_npc_ships();
    draw_remote_players(); /* Multiplayer: remote player ships */
}

static void render_ui(void) {
    float screen_w = ui_screen_width();
    float screen_h = ui_screen_height();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, screen_w, screen_h, 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
    draw_hud_panels();
    draw_hud();
}

static void interpolate_world_for_render(void) {
    if (!g.multiplayer_enabled || !net_is_connected()) return;
    float t = clampf(g.asteroid_interp.t, 0.0f, 1.2f);

    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        asteroid_t *dst = &g.world.asteroids[i];
        const asteroid_t *prev = &g.asteroid_interp.prev[i];
        const asteroid_t *curr = &g.asteroid_interp.curr[i];
        /* Use current state for everything except position */
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, t);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, t);
            dst->rotation = lerp_angle(prev->rotation, curr->rotation, t);
        }
    }

    float nt = clampf(g.npc_interp.t, 0.0f, 1.2f);
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        npc_ship_t *dst = &g.world.npc_ships[i];
        const npc_ship_t *prev = &g.npc_interp.prev[i];
        const npc_ship_t *curr = &g.npc_interp.curr[i];
        *dst = *curr;
        if (prev->active && curr->active) {
            dst->pos.x = lerpf(prev->pos.x, curr->pos.x, nt);
            dst->pos.y = lerpf(prev->pos.y, curr->pos.y, nt);
            dst->angle = lerp_angle(prev->angle, curr->angle, nt);
        }
    }
}

static void render_frame(void) {
    interpolate_world_for_render();
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

    /* --- Multiplayer: poll incoming and send input BEFORE sim --- */
    if (g.multiplayer_enabled) {
        bool was_connected = net_is_connected();
        net_poll();
        sync_local_player_slot_from_network();
        if (was_connected && !net_is_connected()) {
            set_notice("Connection lost. Continuing offline.");
        }
        /* Send input at ~30 Hz, or immediately if there's a one-shot action. */
        {
            uint8_t action = g.pending_net_action;
            g.net_input_timer -= frame_dt;
            if (g.net_input_timer <= 0.0f || action != 0) {
                g.net_input_timer = 1.0f / 30.0f;
                uint8_t flags = 0;
                if (g.input.key_down[SAPP_KEYCODE_W] || g.input.key_down[SAPP_KEYCODE_UP])
                    flags |= NET_INPUT_THRUST;
                if (g.input.key_down[SAPP_KEYCODE_S] || g.input.key_down[SAPP_KEYCODE_DOWN])
                    flags |= NET_INPUT_BRAKE;
                if (g.input.key_down[SAPP_KEYCODE_A] || g.input.key_down[SAPP_KEYCODE_LEFT])
                    flags |= NET_INPUT_LEFT;
                if (g.input.key_down[SAPP_KEYCODE_D] || g.input.key_down[SAPP_KEYCODE_RIGHT])
                    flags |= NET_INPUT_RIGHT;
                if (g.input.key_down[SAPP_KEYCODE_SPACE])
                    flags |= NET_INPUT_FIRE;
                g.pending_net_action = 0;
                net_send_input(flags, action);
            }
        }
    }

    advance_simulation_frame(frame_dt);
    audio_generate_stream(&g.audio);

    render_frame();
}

static void cleanup(void) {
    if (g.multiplayer_enabled) {
        net_shutdown();
    }
    saudio_shutdown();
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
        .width = 1600,
        .height = 900,
        .sample_count = 4,
        .high_dpi = true,
        .window_title = "SIGNAL",
        .logger.func = slog_func,
    };
}
