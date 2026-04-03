/*
 * world_draw.c -- World-space rendering: camera/frustum, VFX, ships,
 * asteroids, stations, and multiplayer players.
 * Split from main.c for Phase 3 refactoring.
 */
#include "client.h"
#include "render.h"
#include "npc.h"
#include "net.h"
#include "net_sync.h"

/* --- Frustum culling: skip objects entirely off-screen --- */
static float g_cam_left, g_cam_right, g_cam_top, g_cam_bottom;

void set_camera_bounds(vec2 camera, float half_w, float half_h) {
    g_cam_left   = camera.x - half_w;
    g_cam_right  = camera.x + half_w;
    g_cam_top    = camera.y - half_h;
    g_cam_bottom = camera.y + half_h;
}

bool on_screen(float x, float y, float radius) {
    return x + radius > g_cam_left  && x - radius < g_cam_right &&
           y + radius > g_cam_top   && y - radius < g_cam_bottom;
}

float cam_left(void)   { return g_cam_left; }
float cam_right(void)  { return g_cam_right; }
float cam_top(void)    { return g_cam_top; }
float cam_bottom(void) { return g_cam_bottom; }

/* --- LOD: reduce asteroid segments when small on screen --- */
int lod_segments(int base_segments, float radius) {
    float half_w = (g_cam_right - g_cam_left) * 0.5f;
    float screen_ratio = radius / half_w;
    if (screen_ratio < 0.005f) return 6;
    if (screen_ratio < 0.015f) return base_segments / 2;
    if (screen_ratio < 0.03f)  return (base_segments * 3) / 4;
    return base_segments;
}

float asteroid_profile(const asteroid_t* asteroid, float angle) {
    float bump1 = sinf(angle * 3.0f + asteroid->seed);
    float bump2 = sinf(angle * 7.0f + asteroid->seed * 1.71f);
    float bump3 = cosf(angle * 5.0f + asteroid->seed * 0.63f);
    float profile = 1.0f + (bump1 * 0.08f) + (bump2 * 0.06f) + (bump3 * 0.04f);
    return asteroid->radius * profile;
}

void draw_background(vec2 camera) {
    for (int i = 0; i < MAX_STARS; i++) {
        const star_t* star = &g.stars[i];
        vec2 parallax_pos = v2_add(star->pos, v2_scale(camera, 1.0f - star->depth));
        float tint = star->brightness;
        draw_rect_centered(parallax_pos, star->size, star->size, 0.65f * tint, 0.75f * tint, tint, 0.9f);
    }
}

/* ------------------------------------------------------------------ */
/* Module type color palette                                          */
/* ------------------------------------------------------------------ */

static void module_color(module_type_t type, float *r, float *g, float *b) {
    switch (type) {
    case MODULE_ORE_BUYER:       *r=0.75f; *g=0.50f; *b=0.20f; return;
    case MODULE_FURNACE:         *r=0.70f; *g=0.30f; *b=0.12f; return;
    case MODULE_FURNACE_CU:      *r=0.60f; *g=0.40f; *b=0.15f; return;
    case MODULE_FURNACE_CR:      *r=0.35f; *g=0.25f; *b=0.60f; return;
    case MODULE_FRAME_PRESS:     *r=0.40f; *g=0.50f; *b=0.65f; return;
    case MODULE_LASER_FAB:       *r=0.65f; *g=0.22f; *b=0.22f; return;
    case MODULE_TRACTOR_FAB:     *r=0.22f; *g=0.60f; *b=0.35f; return;
    case MODULE_SIGNAL_RELAY:    *r=0.25f; *g=0.55f; *b=0.70f; return;
    case MODULE_REPAIR_BAY:      *r=0.30f; *g=0.45f; *b=0.70f; return;
    case MODULE_CONTRACT_BOARD:  *r=0.65f; *g=0.60f; *b=0.25f; return;
    case MODULE_BLUEPRINT_DESK:  *r=0.35f; *g=0.55f; *b=0.70f; return;
    case MODULE_ORE_SILO:        *r=0.50f; *g=0.45f; *b=0.30f; return;
    case MODULE_INGOT_SELLER:    *r=0.55f; *g=0.50f; *b=0.40f; return;
    default:                     *r=0.30f; *g=0.35f; *b=0.40f; return;
    }
}

/* ------------------------------------------------------------------ */
/* Solid module block + corridor to core                              */
/* ------------------------------------------------------------------ */

static void draw_module_at(vec2 pos, float angle, module_type_t type, bool scaffold, float progress, vec2 station_center) {
    float hw = 32.0f;  /* half-width */
    float hh = 32.0f;  /* half-height — square */
    float ch = 8.0f;   /* chamfer size (corner cut) */
    float mr, mg, mb;
    module_color(type, &mr, &mg, &mb);
    float alpha = scaffold ? 0.25f : 0.92f;
    (void)station_center;

    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, 0.0f);
    sgl_rotate(angle, 0.0f, 0.0f, 1.0f);

    /* Rounded square body — 8 vertices (chamfered corners) */
    /* Vertices clockwise from top-left chamfer:
     * (-hw+ch,-hh) (hw-ch,-hh) (hw,-hh+ch) (hw,hh-ch)
     * (hw-ch,hh) (-hw+ch,hh) (-hw,hh-ch) (-hw,-hh+ch) */
    float vx[] = { -hw+ch, hw-ch, hw, hw, hw-ch, -hw+ch, -hw, -hw };
    float vy[] = { -hh, -hh, -hh+ch, hh-ch, hh, hh, hh-ch, -hh+ch };

    /* Body fill — fan from center */
    sgl_c4f(mr * 0.35f, mg * 0.35f, mb * 0.35f, alpha);
    sgl_begin_triangles();
    for (int i = 0; i < 8; i++) {
        int n = (i + 1) % 8;
        sgl_v2f(0, 0); sgl_v2f(vx[i], vy[i]); sgl_v2f(vx[n], vy[n]);
    }
    sgl_end();

    /* Lighter upper half overlay */
    sgl_c4f(mr * 0.50f, mg * 0.50f, mb * 0.50f, alpha);
    sgl_begin_triangles();
    sgl_v2f(vx[0], vy[0]); sgl_v2f(vx[1], vy[1]); sgl_v2f(vx[2], vy[2]);
    sgl_v2f(vx[0], vy[0]); sgl_v2f(vx[2], vy[2]); sgl_v2f(0, 0);
    sgl_v2f(0, 0); sgl_v2f(vx[7], vy[7]); sgl_v2f(vx[0], vy[0]);
    sgl_end();

    /* Outward face plate (bright accent strip along top edge) */
    sgl_c4f(mr * 0.9f, mg * 0.9f, mb * 0.9f, alpha);
    sgl_begin_triangles();
    sgl_v2f(vx[0], vy[0]); sgl_v2f(vx[1], vy[1]); sgl_v2f(vx[1], vy[1]+5);
    sgl_v2f(vx[0], vy[0]); sgl_v2f(vx[1], vy[1]+5); sgl_v2f(vx[0], vy[0]+5);
    sgl_end();

    /* Edge outline */
    sgl_c4f(mr * 0.7f, mg * 0.7f, mb * 0.7f, alpha);
    sgl_begin_lines();
    for (int i = 0; i < 8; i++) {
        int n = (i + 1) % 8;
        sgl_v2f(vx[i], vy[i]); sgl_v2f(vx[n], vy[n]);
    }
    sgl_end();

    /* Docking ports — small nubs on each side */
    float port_w = 5.0f, port_h = 8.0f;
    sgl_c4f(mr * 0.6f, mg * 0.7f, mb * 0.8f, alpha * 0.9f);
    sgl_begin_triangles();
    /* Left port */
    sgl_v2f(-hw-port_w, -port_h); sgl_v2f(-hw, -port_h); sgl_v2f(-hw, port_h);
    sgl_v2f(-hw-port_w, -port_h); sgl_v2f(-hw, port_h); sgl_v2f(-hw-port_w, port_h);
    /* Right port */
    sgl_v2f(hw, -port_h); sgl_v2f(hw+port_w, -port_h); sgl_v2f(hw+port_w, port_h);
    sgl_v2f(hw, -port_h); sgl_v2f(hw+port_w, port_h); sgl_v2f(hw, port_h);
    /* Inner port (toward core) */
    sgl_v2f(-port_h, hh); sgl_v2f(port_h, hh); sgl_v2f(port_h, hh+port_w);
    sgl_v2f(-port_h, hh); sgl_v2f(port_h, hh+port_w); sgl_v2f(-port_h, hh+port_w);
    sgl_end();

    if (scaffold && progress > 0.01f) {
        float bar_w = hw * 2.0f * progress;
        sgl_c4f(0.3f, 1.0f, 0.6f, 0.7f);
        sgl_begin_triangles();
        sgl_v2f(-hw, hh + 8.0f); sgl_v2f(-hw + bar_w, hh + 8.0f); sgl_v2f(-hw + bar_w, hh + 12.0f);
        sgl_v2f(-hw, hh + 8.0f); sgl_v2f(-hw + bar_w, hh + 12.0f); sgl_v2f(-hw, hh + 12.0f);
        sgl_end();
    }

    sgl_pop_matrix();
}

/* ------------------------------------------------------------------ */
/* Main station draw                                                  */
/* ------------------------------------------------------------------ */

/* Draw station core and dock range (below ships in render order). */
void draw_station(const station_t* station, bool is_current, bool is_nearby) {
    if (!station_exists(station) && !station->scaffold) return;
    (void)is_nearby;

    float role_r = 0.45f, role_g = 0.85f, role_b = 1.0f;
    station_role_color(station, &role_r, &role_g, &role_b);

    /* Scaffold rendering */
    if (station->scaffold) {
        float alpha = 0.3f + 0.2f * sinf(g.world.time * 1.5f);
        float prog = station->scaffold_progress;
        int dash_segs = 24;
        float step = TWO_PI_F / (float)dash_segs;
        for (int i = 0; i < dash_segs; i += 2) {
            float a0 = (float)i * step;
            float a1 = (float)(i + 1) * step;
            vec2 p0 = v2_add(station->pos, v2(cosf(a0) * station->dock_radius, sinf(a0) * station->dock_radius));
            vec2 p1 = v2_add(station->pos, v2(cosf(a1) * station->dock_radius, sinf(a1) * station->dock_radius));
            draw_segment(p0, p1, role_r * 0.5f, role_g * 0.5f, role_b * 0.5f, alpha);
        }
        draw_circle_outline(station->pos, station->radius, 18, role_r * 0.6f, role_g * 0.6f, role_b * 0.6f, alpha + 0.15f);
        if (prog > 0.01f) {
            int filled = (int)(prog * 24.0f);
            float fs = TWO_PI_F / 24.0f;
            for (int i = 0; i < filled && i < 24; i++) {
                vec2 p0 = v2_add(station->pos, v2(cosf(i*fs) * (station->radius+12.0f), sinf(i*fs) * (station->radius+12.0f)));
                vec2 p1 = v2_add(station->pos, v2(cosf((i+1)*fs) * (station->radius+12.0f), sinf((i+1)*fs) * (station->radius+12.0f)));
                draw_segment(p0, p1, role_r, role_g, role_b, 0.8f);
            }
        }
        return;
    }

    (void)is_current;

    /* Orbital center — faint signal pulse, not a physical structure */
    float pulse = 0.15f + 0.1f * sinf(g.world.time * 2.0f);
    draw_circle_outline(station->pos, 8.0f, 12, role_r * 0.4f, role_g * 0.4f, role_b * 0.4f, pulse);
    /* Faint ring orbit guides */
    for (int r = 1; r <= STATION_NUM_RINGS; r++) {
        bool has_modules = false;
        for (int i = 0; i < station->module_count; i++)
            if (station->modules[i].ring == r) { has_modules = true; break; }
        if (!has_modules) continue;
        draw_circle_outline(station->pos, STATION_RING_RADIUS[r], 48, role_r * 0.15f, role_g * 0.15f, role_b * 0.15f, 0.12f);
    }
}

/* Energy tether between lateral modules — pulsing field, not solid girder. */
static void draw_energy_tether(vec2 a, vec2 b, float cr, float cg, float cb, float alpha) {
    vec2 delta = v2_sub(b, a);
    float len = sqrtf(v2_len_sq(delta));
    if (len < 1.0f) return;
    vec2 dir = v2_scale(delta, 1.0f / len);
    vec2 perp = v2(-dir.y, dir.x);

    /* Central beam — thin, bright, pulsing */
    float pulse = 0.5f + 0.3f * sinf(g.world.time * 4.0f + len * 0.1f);
    draw_segment(a, b, cr * 0.6f, cg * 0.8f, cb, alpha * pulse);

    /* Outer field lines — wispy, offset, slower pulse */
    float field_w = 8.0f + 3.0f * sinf(g.world.time * 2.5f);
    vec2 ao = v2_add(a, v2_scale(perp, field_w));
    vec2 bo = v2_add(b, v2_scale(perp, field_w));
    vec2 ai = v2_sub(a, v2_scale(perp, field_w));
    vec2 bi = v2_sub(b, v2_scale(perp, field_w));
    float wisp = alpha * 0.2f * pulse;
    draw_segment(ao, bo, cr * 0.3f, cg * 0.5f, cb * 0.8f, wisp);
    draw_segment(ai, bi, cr * 0.3f, cg * 0.5f, cb * 0.8f, wisp);

    /* Spark nodes at endpoints */
    draw_circle_filled(a, 3.0f, 6, cr * 0.5f, cg * 0.7f, cb, alpha * 0.6f);
    draw_circle_filled(b, 3.0f, 6, cr * 0.5f, cg * 0.7f, cb, alpha * 0.6f);
}

/* Draw module rings (above ships in render order). */
void draw_station_rings(const station_t* station, bool is_current, bool is_nearby) {
    if (!station_exists(station) || station->scaffold) return;

    float role_r = 0.45f, role_g = 0.85f, role_b = 1.0f;
    station_role_color(station, &role_r, &role_g, &role_b);
    float base_alpha = is_current ? 0.9f : (is_nearby ? 0.7f : 0.5f);

    /* Find outermost populated ring */
    int max_ring = 0;
    for (int i = 0; i < station->module_count; i++)
        if (station->modules[i].ring >= 1 && station->modules[i].ring <= STATION_NUM_RINGS)
            if (station->modules[i].ring > max_ring) max_ring = station->modules[i].ring;

    (void)max_ring;

    /* Per-ring: tethers + modules (each ring rotates independently) */
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
        int slots = STATION_RING_SLOTS[ring];

        int mod_idx[MAX_MODULES_PER_STATION];
        int mod_count = 0;
        for (int i = 0; i < station->module_count; i++) {
            if (station->modules[i].ring == ring)
                mod_idx[mod_count++] = i;
        }
        if (mod_count == 0) continue;

        /* Sort modules by slot (insertion sort, small N) */
        for (int i = 1; i < mod_count; i++) {
            int key = mod_idx[i];
            int j = i - 1;
            while (j >= 0 && station->modules[mod_idx[j]].slot > station->modules[key].slot) {
                mod_idx[j + 1] = mod_idx[j]; j--;
            }
            mod_idx[j + 1] = key;
        }

        vec2 positions[MAX_MODULES_PER_STATION];
        int slot_ids[MAX_MODULES_PER_STATION];
        for (int i = 0; i < mod_count; i++) {
            slot_ids[i] = station->modules[mod_idx[i]].slot;
            positions[i] = module_world_pos_ring(station, ring, slot_ids[i]);
        }

        /* Energy tethers between adjacent occupied slots only.
         * Don't wrap across the gap (slot 0). */
        for (int i = 0; i + 1 < mod_count; i++) {
            if (slot_ids[i + 1] - slot_ids[i] == 1)
                draw_energy_tether(positions[i], positions[i + 1], role_r, role_g, role_b, base_alpha * 0.7f);
        }
        /* Wrap: last→first only if ring is completely full */
        if (mod_count == slots)
            draw_energy_tether(positions[mod_count - 1], positions[0], role_r, role_g, role_b, base_alpha * 0.7f);

        /* Modules + dock indicators */
        for (int i = 0; i < mod_count; i++) {
            const station_module_t *m = &station->modules[mod_idx[i]];
            float angle = module_angle_ring(station, ring, m->slot);
            draw_module_at(positions[i], angle, m->type, m->scaffold, m->build_progress, station->pos);

            /* Dock berth indicator: green target rectangle offset outward */
            if (m->type == MODULE_DOCK && is_nearby && !m->scaffold) {
                float dp = 0.5f + 0.4f * sinf(g.world.time * 4.0f);
                /* Berth position: outward from module */
                vec2 outward = v2_sub(positions[i], station->pos);
                float od = sqrtf(v2_len_sq(outward));
                if (od > 0.001f) outward = v2_scale(outward, 1.0f / od);
                vec2 berth = v2_add(positions[i], v2_scale(outward, 55.0f));
                /* Tangent direction */
                vec2 tang = v2(-outward.y, outward.x);
                /* Green rectangle outline at berth */
                float bw = 20.0f, bh = 10.0f;
                vec2 c0 = v2_add(berth, v2_add(v2_scale(tang, -bw), v2_scale(outward, -bh)));
                vec2 c1 = v2_add(berth, v2_add(v2_scale(tang, bw), v2_scale(outward, -bh)));
                vec2 c2 = v2_add(berth, v2_add(v2_scale(tang, bw), v2_scale(outward, bh)));
                vec2 c3 = v2_add(berth, v2_add(v2_scale(tang, -bw), v2_scale(outward, bh)));
                sgl_c4f(0.2f, 1.0f, 0.6f, dp);
                sgl_begin_lines();
                sgl_v2f(c0.x, c0.y); sgl_v2f(c1.x, c1.y);
                sgl_v2f(c1.x, c1.y); sgl_v2f(c2.x, c2.y);
                sgl_v2f(c2.x, c2.y); sgl_v2f(c3.x, c3.y);
                sgl_v2f(c3.x, c3.y); sgl_v2f(c0.x, c0.y);
                sgl_end();
            }
        }
    }
}

void draw_ship_tractor_field(void) {
    if (LOCAL_PLAYER.nearby_fragments <= 0) {
        return;
    }

    float pulse = 0.28f + (sinf(g.world.time * 7.0f) * 0.08f);
    draw_circle_outline(LOCAL_PLAYER.ship.pos, ship_tractor_range(&LOCAL_PLAYER.ship), 40, 0.24f, 0.86f, 1.0f, pulse);
    if (LOCAL_PLAYER.tractor_fragments > 0) {
        draw_circle_outline(LOCAL_PLAYER.ship.pos, ship_collect_radius(&LOCAL_PLAYER.ship) + 6.0f, 28, 0.50f, 1.0f, 0.82f, 0.75f);
    }
}

void draw_ship(void) {
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

void draw_npc_ship(const npc_ship_t* npc) {
    const hull_def_t* hull = npc_hull_def(npc);
    bool is_hauler = npc->hull_class == HULL_CLASS_HAULER;
    float scale = hull->render_scale;
    /* Use accumulated ore tint — starts white, absorbs cargo colors over time */
    float hull_r = npc->tint_r;
    float hull_g = npc->tint_g;
    float hull_b = npc->tint_b;

    (void)is_hauler;

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

void draw_npc_mining_beam(const npc_ship_t* npc) {
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

void draw_npc_ships(void) {
    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        if (!g.world.npc_ships[i].active) continue;
        if (!on_screen(g.world.npc_ships[i].pos.x, g.world.npc_ships[i].pos.y, 50.0f)) continue;
        draw_npc_ship(&g.world.npc_ships[i]);
        draw_npc_mining_beam(&g.world.npc_ships[i]);
    }
}

void draw_beam(void) {
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

/* --- Multiplayer: draw remote players as colored triangles --- */
void draw_remote_players(void) {
    if (!g.multiplayer_enabled) return;
    const NetPlayerState* players = net_get_interpolated_players();
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
        if (!on_screen(players[i].x, players[i].y, 50.0f)) continue;
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
