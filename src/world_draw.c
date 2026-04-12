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
#include "station_voice.h"
#include "signal_model.h"
#include "palette.h"
#include <stdlib.h>

/* --- Frustum culling: skip objects entirely off-screen --- */
static float g_cam_left, g_cam_right, g_cam_top, g_cam_bottom;
static float g_cam_half_w; /* cached for LOD calculations */

void set_camera_bounds(vec2 camera, float half_w, float half_h) {
    g_cam_left   = camera.x - half_w;
    g_cam_right  = camera.x + half_w;
    g_cam_top    = camera.y - half_h;
    g_cam_bottom = camera.y + half_h;
    g_cam_half_w = half_w;
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
    float screen_ratio = radius / g_cam_half_w;
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
    sgl_begin_quads();
    for (int i = 0; i < MAX_STARS; i++) {
        const star_t* star = &g.stars[i];
        vec2 parallax_pos = v2_add(star->pos, v2_scale(camera, 1.0f - star->depth));
        if (!on_screen(parallax_pos.x, parallax_pos.y, star->size * 2.0f)) continue;
        float tint = star->brightness;
        float r = 0.65f * tint, g0 = 0.75f * tint, b = tint;
        sgl_c4f(r, g0, b, 0.9f);
        sgl_v2f(parallax_pos.x - star->size, parallax_pos.y - star->size);
        sgl_v2f(parallax_pos.x + star->size, parallax_pos.y - star->size);
        sgl_v2f(parallax_pos.x + star->size, parallax_pos.y + star->size);
        sgl_v2f(parallax_pos.x - star->size, parallax_pos.y + star->size);
    }
    sgl_end();
}

/* ------------------------------------------------------------------ */
/* Signal border rendering — union-of-circles arc clipping            */
/* ------------------------------------------------------------------ */

void draw_signal_borders(void) {
    /* S(p) >= t iff p is inside any circle of radius R_i*(1-t).
     * The contour is the boundary of the union of those circles.
     * For each station, draw only the arc NOT inside another circle.
     * Exact geometry — perfectly smooth, no grid, no sampling. */

    static const struct { float threshold; float r, g, b, a; float width; } bands[] = {
        { SIGNAL_BAND_OPERATIONAL, 0.12f, 0.22f, 0.45f, 0.18f, 3.0f },
        { SIGNAL_BAND_FRINGE,     0.45f, 0.28f, 0.08f, 0.15f, 2.5f },
        { SIGNAL_BAND_FRONTIER,   0.42f, 0.10f, 0.08f, 0.12f, 2.0f },
    };

    const int SEGS = 180;
    const float STEP = TWO_PI_F / (float)SEGS;

    for (int band = 0; band < 3; band++) {
        float thr = bands[band].threshold;
        float hw = bands[band].width;

        float radii[MAX_STATIONS];
        for (int s = 0; s < MAX_STATIONS; s++)
            radii[s] = station_provides_signal(&g.world.stations[s])
                ? g.world.stations[s].signal_range * (1.0f - thr) : 0.0f;

        for (int s = 0; s < MAX_STATIONS; s++) {
            if (radii[s] <= 0.0f) continue;
            const station_t *st = &g.world.stations[s];
            float r = radii[s];
            if (!on_screen(st->pos.x, st->pos.y, r)) continue;

            sgl_begin_triangle_strip();
            sgl_c4f(bands[band].r, bands[band].g, bands[band].b, bands[band].a);
            bool active = false;

            for (int i = 0; i <= SEGS; i++) {
                float a = (float)(i % SEGS) * STEP;
                float ca = cosf(a), sa = sinf(a);
                float px = st->pos.x + ca * r;
                float py = st->pos.y + sa * r;

                /* Clip: skip if inside another station's circle */
                bool clipped = false;
                for (int o = 0; o < MAX_STATIONS; o++) {
                    if (o == s || radii[o] <= 0.0f) continue;
                    float dx = px - g.world.stations[o].pos.x;
                    float dy = py - g.world.stations[o].pos.y;
                    if (dx*dx + dy*dy < radii[o]*radii[o]) { clipped = true; break; }
                }

                if (!clipped && (i % 4 < 3)) { /* 3 on, 1 off = dashed */
                    sgl_v2f(px - ca * hw, py - sa * hw);
                    sgl_v2f(px + ca * hw, py + sa * hw);
                    active = true;
                } else if (active) {
                    sgl_end();
                    sgl_begin_triangle_strip();
                    sgl_c4f(bands[band].r, bands[band].g, bands[band].b, bands[band].a);
                    active = false;
                }
            }
            sgl_end();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Module type color palette                                          */
/* ------------------------------------------------------------------ */

static void module_color(module_type_t type, float *r, float *g, float *b) {
    switch (type) {
    case MODULE_FURNACE:      PAL_UNPACK3(PAL_MODULE_FURNACE,      *r, *g, *b); return;
    case MODULE_ORE_BUYER:    PAL_UNPACK3(PAL_MODULE_ORE_BUYER,    *r, *g, *b); return;
    case MODULE_ORE_SILO:     PAL_UNPACK3(PAL_MODULE_ORE_SILO,     *r, *g, *b); return;
    case MODULE_FRAME_PRESS:  PAL_UNPACK3(PAL_MODULE_FRAME_PRESS,  *r, *g, *b); return;
    case MODULE_LASER_FAB:    PAL_UNPACK3(PAL_MODULE_LASER_FAB,    *r, *g, *b); return;
    case MODULE_TRACTOR_FAB:  PAL_UNPACK3(PAL_MODULE_TRACTOR_FAB,  *r, *g, *b); return;
    case MODULE_FURNACE_CU:   PAL_UNPACK3(PAL_MODULE_FURNACE_CU,   *r, *g, *b); return;
    case MODULE_FURNACE_CR:   PAL_UNPACK3(PAL_MODULE_FURNACE_CR,   *r, *g, *b); return;
    case MODULE_SIGNAL_RELAY: PAL_UNPACK3(PAL_MODULE_SIGNAL_RELAY,  *r, *g, *b); return;
    case MODULE_REPAIR_BAY:   PAL_UNPACK3(PAL_MODULE_REPAIR_BAY,    *r, *g, *b); return;
    case MODULE_SHIPYARD:     PAL_UNPACK3(PAL_MODULE_SHIPYARD,      *r, *g, *b); return;
    default:                  PAL_UNPACK3(PAL_MODULE_GENERIC,        *r, *g, *b); return;
    }
}

void module_color_fn(module_type_t type, float *r, float *g, float *b) {
    module_color(type, r, g, b);
}

/* ------------------------------------------------------------------ */
/* Solid module block + corridor to core                              */
/* ------------------------------------------------------------------ */

/* Helper: filled quad (two triangles) in local coords */
static void fill_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3) {
    sgl_begin_triangles();
    sgl_v2f(x0,y0); sgl_v2f(x1,y1); sgl_v2f(x2,y2);
    sgl_v2f(x0,y0); sgl_v2f(x2,y2); sgl_v2f(x3,y3);
    sgl_end();
}

/* Helper: filled circle in local coords */
static void fill_circle_local(float cx, float cy, float r, int segs, float cr, float cg, float cb, float ca) {
    sgl_c4f(cr, cg, cb, ca);
    sgl_begin_triangles();
    for (int i = 0; i < segs; i++) {
        float a0 = TWO_PI_F * (float)i / (float)segs;
        float a1 = TWO_PI_F * (float)(i+1) / (float)segs;
        sgl_v2f(cx, cy);
        sgl_v2f(cx + cosf(a0)*r, cy + sinf(a0)*r);
        sgl_v2f(cx + cosf(a1)*r, cy + sinf(a1)*r);
    }
    sgl_end();
}

/* 6-shape module system: shape=function, hull color=faction, accent=product/ore.
 *   Dock(+repair) = U-shape    Intake(hopper+silo) = Triangle
 *   Furnace       = Circle     Fabricator(all)     = Pentagon
 *   Relay         = Diamond    Shipyard            = Open frame
 * Drawn in local rotated space, ~64x64 bounding. */

/* Helper: outline a regular polygon */
static void outline_ngon(int n, float r, float cr, float cg, float cb, float ca) {
    sgl_c4f(cr, cg, cb, ca);
    sgl_begin_lines();
    for (int i = 0; i < n; i++) {
        float a0 = TWO_PI_F * (float)i / (float)n - PI_F * 0.5f;
        float a1 = TWO_PI_F * (float)(i+1) / (float)n - PI_F * 0.5f;
        sgl_v2f(cosf(a0)*r, sinf(a0)*r);
        sgl_v2f(cosf(a1)*r, sinf(a1)*r);
    }
    sgl_end();
}

/* Helper: fill a regular polygon */
static void fill_ngon(int n, float r, float cr, float cg, float cb, float ca) {
    sgl_c4f(cr, cg, cb, ca);
    sgl_begin_triangles();
    for (int i = 0; i < n; i++) {
        float a0 = TWO_PI_F * (float)i / (float)n - PI_F * 0.5f;
        float a1 = TWO_PI_F * (float)(i+1) / (float)n - PI_F * 0.5f;
        sgl_v2f(0, 0);
        sgl_v2f(cosf(a0)*r, sinf(a0)*r);
        sgl_v2f(cosf(a1)*r, sinf(a1)*r);
    }
    sgl_end();
}

static void draw_module_shape(module_type_t type, float mr, float mg, float mb, float alpha) {
    switch (type) {

    /* ---- DOCK (+repair): U-shape ---- */
    case MODULE_DOCK:
    case MODULE_REPAIR_BAY: {
        /* Solid backplate covers corridor end underneath */
        sgl_c4f(mr*0.12f, mg*0.12f, mb*0.12f, alpha);
        fill_quad(-30, -26, 30, -26, 30, 26, -30, 26);
        /* U-shape fill: back plate + two arms */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        fill_quad(-28, 24, 28, 24, 28, 12, -28, 12);   /* back plate */
        fill_quad(-28, -24, -14, -24, -14, 24, -28, 24); /* left arm */
        fill_quad(14, -24, 28, -24, 28, 24, 14, 24);     /* right arm */
        /* Clamp teeth */
        sgl_c4f(mr*0.55f, mg*0.55f, mb*0.55f, alpha);
        fill_quad(-14, -24, -6, -24, -6, -18, -14, -18);
        fill_quad(6, -24, 14, -24, 14, -18, 6, -18);
        /* Guide lights */
        fill_circle_local(-8, -4, 4, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha*0.7f);
        fill_circle_local( 8, -4, 4, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha*0.7f);
        /* Repair wrench hint (small, bottom-right) */
        if (type == MODULE_REPAIR_BAY) {
            sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha*0.5f);
            sgl_begin_lines();
            sgl_v2f(4, 20); sgl_v2f(14, 10);
            sgl_end();
            fill_circle_local(3, 21, 3, 6, mr*0.4f, mg*0.4f, mb*0.4f, alpha*0.4f);
        }
        /* Bold outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-28, 24); sgl_v2f(-28, -24);
        sgl_v2f(-28, -24); sgl_v2f(-6, -24);
        sgl_v2f(6, -24); sgl_v2f(28, -24);
        sgl_v2f(28, -24); sgl_v2f(28, 24);
        sgl_v2f(28, 24); sgl_v2f(-28, 24);
        sgl_end();
        break;
    }

    /* ---- INTAKE (hopper+silo): Triangle ---- */
    case MODULE_ORE_BUYER:
    case MODULE_ORE_SILO:
    case MODULE_CARGO_BAY: {
        /* Triangle pointing outward (-Y) = funnel mouth */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        sgl_begin_triangles();
        sgl_v2f(-32, -20); sgl_v2f(32, -20); sgl_v2f(0, 28);
        sgl_end();
        /* Mouth rim highlight */
        sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha);
        fill_quad(-32, -22, 32, -22, 32, -18, -32, -18);
        /* Ore accent glow in center */
        fill_circle_local(0, 0, 7, 8, mr*0.5f, mg*0.5f, mb*0.5f, alpha*0.2f);
        fill_circle_local(0, 0, 3, 6, mr*0.7f, mg*0.7f, mb*0.7f, alpha*0.35f);
        /* Bold outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-32, -20); sgl_v2f(32, -20);
        sgl_v2f(32, -20); sgl_v2f(0, 28);
        sgl_v2f(0, 28); sgl_v2f(-32, -20);
        sgl_end();
        break;
    }

    /* ---- FURNACE (all ore types): Circle ---- */
    case MODULE_FURNACE:
    case MODULE_FURNACE_CU:
    case MODULE_FURNACE_CR: {
        /* Filled circle hull */
        fill_circle_local(0, 0, 28, 20, mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        /* Ore-type accent glow — warm layered halo */
        fill_circle_local(0, 0, 24, 16, mr*0.12f, mg*0.04f, mb*0.02f, alpha*0.4f);
        fill_circle_local(0, 0, 18, 14, mr*0.25f, mg*0.10f, mb*0.04f, alpha*0.5f);
        fill_circle_local(0, 0, 12, 12, mr*0.50f, mg*0.20f, mb*0.08f, alpha*0.6f);
        fill_circle_local(0, 0, 7,  10, mr*0.80f, mg*0.40f, mb*0.15f, alpha*0.75f);
        fill_circle_local(0, 0, 3,  8,  mr*1.0f,  mg*0.70f, mb*0.30f, alpha*0.95f);
        /* Bright hot core dot */
        fill_circle_local(0, 0, 1.5f, 6, 1.0f, 0.95f, 0.7f, alpha*0.8f);
        /* Bold outline */
        outline_ngon(20, 28, mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        break;
    }

    /* ---- FABRICATOR (press, laser, tractor): Pentagon ---- */
    case MODULE_FRAME_PRESS:
    case MODULE_LASER_FAB:
    case MODULE_TRACTOR_FAB: {
        /* Filled pentagon hull */
        fill_ngon(5, 28, mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        /* Inner pentagon (product chamber) */
        fill_ngon(5, 14, mr*0.15f, mg*0.15f, mb*0.15f, alpha*0.8f);
        /* Product accent dot */
        fill_circle_local(0, 2, 4, 6, mr*0.8f, mg*0.8f, mb*0.8f, alpha*0.4f);
        /* Crosshair for press type */
        if (type == MODULE_FRAME_PRESS) {
            sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha*0.5f);
            sgl_begin_lines();
            sgl_v2f(-8, 2); sgl_v2f(8, 2);
            sgl_v2f(0, -6); sgl_v2f(0, 10);
            sgl_end();
        }
        /* Bold outline */
        outline_ngon(5, 28, mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        break;
    }

    /* ---- RELAY: Diamond ---- */
    case MODULE_SIGNAL_RELAY: {
        /* Filled diamond (rotated square) */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        sgl_begin_triangles();
        sgl_v2f(0, -32); sgl_v2f(26, 0); sgl_v2f(0, 32);
        sgl_v2f(0, -32); sgl_v2f(0, 32); sgl_v2f(-26, 0);
        sgl_end();
        /* Beacon center */
        fill_circle_local(0, 0, 7, 8, mr*0.4f, mg*0.4f, mb*0.4f, alpha*0.8f);
        fill_circle_local(0, 0, 3, 6, mr*1.0f, mg*1.0f, mb*1.0f, alpha*0.8f);
        /* Signal ripple diamond (inner echo) */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha*0.15f);
        sgl_begin_lines();
        sgl_v2f(0, -18); sgl_v2f(14, 0);
        sgl_v2f(14, 0); sgl_v2f(0, 18);
        sgl_v2f(0, 18); sgl_v2f(-14, 0);
        sgl_v2f(-14, 0); sgl_v2f(0, -18);
        sgl_end();
        /* Bold outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(0, -32); sgl_v2f(26, 0);
        sgl_v2f(26, 0); sgl_v2f(0, 32);
        sgl_v2f(0, 32); sgl_v2f(-26, 0);
        sgl_v2f(-26, 0); sgl_v2f(0, -32);
        sgl_end();
        break;
    }

    /* ---- SHIPYARD: Open square frame ---- */
    case MODULE_SHIPYARD: {
        /* Solid backplate covers corridor end underneath */
        sgl_c4f(mr*0.12f, mg*0.12f, mb*0.12f, alpha);
        fill_quad(-30, -30, 30, -30, 30, 30, -30, 30);
        /* Corner blocks (filled) */
        sgl_c4f(mr*0.30f, mg*0.30f, mb*0.30f, alpha);
        fill_quad(-28, -28, -16, -28, -16, -16, -28, -16); /* TL */
        fill_quad( 16, -28,  28, -28,  28, -16,  16, -16); /* TR */
        fill_quad(-28,  16, -16,  16, -16,  28, -28,  28); /* BL */
        fill_quad( 16,  16,  28,  16,  28,  28,  16,  28); /* BR */
        /* Cross-bracing */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha*0.4f);
        sgl_begin_lines();
        sgl_v2f(-16, -16); sgl_v2f(16, 16);
        sgl_v2f(16, -16); sgl_v2f(-16, 16);
        sgl_end();
        /* Work lights */
        fill_circle_local(-22, 0, 2.5f, 4, mr*0.9f, mg*0.7f, mb*0.2f, alpha*0.35f);
        fill_circle_local( 22, 0, 2.5f, 4, mr*0.9f, mg*0.7f, mb*0.2f, alpha*0.35f);
        /* Bold frame outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-28, -28); sgl_v2f(28, -28);
        sgl_v2f(28, -28); sgl_v2f(28, 28);
        sgl_v2f(28, 28); sgl_v2f(-28, 28);
        sgl_v2f(-28, 28); sgl_v2f(-28, -28);
        sgl_end();
        break;
    }

    default: {
        /* Generic chamfered square fallback */
        float ch = 6.0f;
        sgl_c4f(mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        fill_quad(-24+ch, -24, 24-ch, -24, 24-ch, 24, -24+ch, 24);
        fill_quad(-24, -24+ch, -24+ch, -24, -24+ch, 24, -24, 24-ch);
        fill_quad(24-ch, -24, 24, -24+ch, 24, 24-ch, 24-ch, 24);
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(-24+ch, -24); sgl_v2f(24-ch, -24);
        sgl_v2f(24-ch, -24); sgl_v2f(24, -24+ch);
        sgl_v2f(24, -24+ch); sgl_v2f(24, 24-ch);
        sgl_v2f(24, 24-ch); sgl_v2f(24-ch, 24);
        sgl_v2f(24-ch, 24); sgl_v2f(-24+ch, 24);
        sgl_v2f(-24+ch, 24); sgl_v2f(-24, 24-ch);
        sgl_v2f(-24, 24-ch); sgl_v2f(-24, -24+ch);
        sgl_v2f(-24, -24+ch); sgl_v2f(-24+ch, -24);
        sgl_end();
        break;
    }
    }
}

static void draw_module_at(vec2 pos, float angle, module_type_t type, bool scaffold, float progress, vec2 station_center) {
    float mr, mg, mb;
    module_color(type, &mr, &mg, &mb);
    (void)station_center;

    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, 0.0f);
    sgl_rotate(angle + PI_F * 0.5f, 0.0f, 0.0f, 1.0f);
    sgl_scale(1.4f, 1.4f, 1.0f);

    if (scaffold) {
        /* Wireframe outline circle — construction amber (#FFD977) */
        float amb_r = 1.0f, amb_g = 0.85f, amb_b = 0.47f;
        float pulse = 0.3f + 0.15f * sinf((float)(pos.x + pos.y) * 0.1f + progress * 10.0f);

        /* Progress fill: partial circle from bottom */
        float fill = fminf(progress, 1.0f);
        if (fill > 0.01f) {
            int segs = (int)(16.0f * fill);
            if (segs < 2) segs = 2;
            sgl_begin_triangles();
            sgl_c4f(amb_r * 0.3f, amb_g * 0.3f, amb_b * 0.3f, pulse * 0.6f);
            float fill_angle = fill * TWO_PI_F;
            float start = PI_F * 0.5f; /* bottom */
            for (int i = 0; i < segs; i++) {
                float a0 = start + fill_angle * (float)i / (float)segs;
                float a1 = start + fill_angle * (float)(i + 1) / (float)segs;
                sgl_v2f(0, 0);
                sgl_v2f(cosf(a0) * 22.0f, sinf(a0) * 22.0f);
                sgl_v2f(cosf(a1) * 22.0f, sinf(a1) * 22.0f);
            }
            sgl_end();
        }

        /* Wireframe circle outline */
        sgl_begin_lines();
        sgl_c4f(amb_r, amb_g, amb_b, pulse + 0.3f);
        int wire_segs = 16;
        for (int i = 0; i < wire_segs; i++) {
            float a0 = TWO_PI_F * (float)i / (float)wire_segs;
            float a1 = TWO_PI_F * (float)(i + 1) / (float)wire_segs;
            sgl_v2f(cosf(a0) * 22.0f, sinf(a0) * 22.0f);
            sgl_v2f(cosf(a1) * 22.0f, sinf(a1) * 22.0f);
        }
        /* Cross-hatch for scaffolding feel */
        sgl_v2f(-16, -16); sgl_v2f(16, 16);
        sgl_v2f(-16, 16); sgl_v2f(16, -16);
        sgl_end();

        /* Progress bar below */
        if (fill > 0.01f) {
            float bar_w = 48.0f * fill;
            sgl_c4f(amb_r * 0.8f, amb_g * 0.8f, amb_b * 0.4f, 0.7f);
            fill_quad(-24, 30, -24 + bar_w, 30, -24 + bar_w, 34, -24, 34);
        }
    } else {
        draw_module_shape(type, mr, mg, mb, 0.92f);
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

    /* Station center is empty space — the construction yard.
     * Just a faint marker so the player can locate the geometric center. */
    float pulse = 0.15f + 0.08f * sinf(g.world.time * 2.0f);
    draw_circle_outline(station->pos, 4.0f, 8, role_r * 0.4f, role_g * 0.4f, role_b * 0.4f, pulse);

    /* Radial spokes from core to ring 1 modules */
    for (int i = 0; i < station->module_count; i++) {
        if (station->modules[i].ring != 1) continue;
        vec2 mod_pos = module_world_pos_ring(station, 1, station->modules[i].slot);
        sgl_c4f(role_r * 0.2f, role_g * 0.2f, role_b * 0.2f, 0.25f);
        sgl_begin_lines();
        sgl_v2f(station->pos.x, station->pos.y);
        sgl_v2f(mod_pos.x, mod_pos.y);
        sgl_end();
    }

    /* Faint ring orbit guides */
    for (int r = 1; r <= STATION_NUM_RINGS; r++) {
        bool has_modules = false;
        for (int i = 0; i < station->module_count; i++)
            if (station->modules[i].ring == r) { has_modules = true; break; }
        if (!has_modules) continue;
        draw_circle_outline(station->pos, STATION_RING_RADIUS[r], 48, role_r * 0.08f, role_g * 0.08f, role_b * 0.08f, 0.08f);
    }
}

/* Solid corridor tube between adjacent modules on the same ring. */
/* Draw a curved corridor that arcs along the ring radius between two module positions. */
#define CORRIDOR_ARC_SEGMENTS 8

static void draw_corridor_arc(vec2 center, float ring_radius, float angle_a, float angle_b,
                               float cr, float cg, float cb, float alpha) {
    /* Corridor visual band — slightly wider than STATION_CORRIDOR_HW to account
     * for the angular margin expansion in collision (ship radius ~12-15 units). */
    float hw = STATION_CORRIDOR_HW + 4.0f;
    float r_inner = ring_radius - hw;
    float r_outer = ring_radius + hw;

    /* Tessellate the arc */
    float da = angle_b - angle_a;
    /* Normalize to shortest arc */
    while (da > PI_F) da -= TWO_PI_F;
    while (da < -PI_F) da += TWO_PI_F;

    /* Solid fill — triangle strip as quads */
    sgl_c4f(cr * 0.15f, cg * 0.15f, cb * 0.15f, alpha * 0.6f);
    sgl_begin_triangles();
    for (int i = 0; i < CORRIDOR_ARC_SEGMENTS; i++) {
        float t0 = (float)i / (float)CORRIDOR_ARC_SEGMENTS;
        float t1 = (float)(i + 1) / (float)CORRIDOR_ARC_SEGMENTS;
        float a0 = angle_a + da * t0;
        float a1 = angle_a + da * t1;
        vec2 i0 = v2_add(center, v2(cosf(a0) * r_inner, sinf(a0) * r_inner));
        vec2 o0 = v2_add(center, v2(cosf(a0) * r_outer, sinf(a0) * r_outer));
        vec2 i1 = v2_add(center, v2(cosf(a1) * r_inner, sinf(a1) * r_inner));
        vec2 o1 = v2_add(center, v2(cosf(a1) * r_outer, sinf(a1) * r_outer));
        sgl_v2f(i0.x,i0.y); sgl_v2f(o0.x,o0.y); sgl_v2f(o1.x,o1.y);
        sgl_v2f(i0.x,i0.y); sgl_v2f(o1.x,o1.y); sgl_v2f(i1.x,i1.y);
    }
    sgl_end();

    /* Edge lines (inner and outer arcs) — brighter than fill */
    sgl_c4f(cr * 0.55f, cg * 0.55f, cb * 0.55f, alpha * 0.7f);
    sgl_begin_line_strip();
    for (int i = 0; i <= CORRIDOR_ARC_SEGMENTS; i++) {
        float t = (float)i / (float)CORRIDOR_ARC_SEGMENTS;
        float a = angle_a + da * t;
        sgl_v2f(center.x + cosf(a) * r_inner, center.y + sinf(a) * r_inner);
    }
    sgl_end();
    sgl_begin_line_strip();
    for (int i = 0; i <= CORRIDOR_ARC_SEGMENTS; i++) {
        float t = (float)i / (float)CORRIDOR_ARC_SEGMENTS;
        float a = angle_a + da * t;
        sgl_v2f(center.x + cosf(a) * r_outer, center.y + sinf(a) * r_outer);
    }
    sgl_end();
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

    /* Per-ring dominant color for corridors */
    float ring_cr[STATION_NUM_RINGS + 1], ring_cg[STATION_NUM_RINGS + 1], ring_cb[STATION_NUM_RINGS + 1];
    for (int r = 0; r <= STATION_NUM_RINGS; r++) {
        ring_cr[r] = role_r; ring_cg[r] = role_g; ring_cb[r] = role_b;
    }
    {
        /* Most saturated module sets the base, others tint it.
         * 80% base + 20% influence from the rest — enough to
         * shift the hue without muddying it. */
        for (int r = 1; r <= STATION_NUM_RINGS; r++) {
            float colors[MAX_MODULES_PER_STATION][3];
            float sats[MAX_MODULES_PER_STATION];
            int count = 0;
            int best = 0;
            float best_sat = -1.0f;
            for (int i = 0; i < station->module_count; i++) {
                if (station->modules[i].ring != r) continue;
                if (station->modules[i].type == MODULE_DOCK) continue;
                module_color(station->modules[i].type, &colors[count][0], &colors[count][1], &colors[count][2]);
                float cmax = fmaxf(colors[count][0], fmaxf(colors[count][1], colors[count][2]));
                float cmin = fminf(colors[count][0], fminf(colors[count][1], colors[count][2]));
                sats[count] = (cmax > 0.001f) ? (cmax - cmin) / cmax : 0.0f;
                if (sats[count] > best_sat) { best_sat = sats[count]; best = count; }
                count++;
            }
            if (count == 0) continue;
            /* Start with the dominant color */
            ring_cr[r] = colors[best][0];
            ring_cg[r] = colors[best][1];
            ring_cb[r] = colors[best][2];
            if (count > 1) {
                /* Tint: lerp 20% toward the average of the others */
                float tr = 0, tg = 0, tb = 0;
                for (int c = 0; c < count; c++) {
                    if (c == best) continue;
                    tr += colors[c][0]; tg += colors[c][1]; tb += colors[c][2];
                }
                float n = (float)(count - 1);
                float blend = 0.2f;
                ring_cr[r] = ring_cr[r] * (1.0f - blend) + (tr / n) * blend;
                ring_cg[r] = ring_cg[r] * (1.0f - blend) + (tg / n) * blend;
                ring_cb[r] = ring_cb[r] * (1.0f - blend) + (tb / n) * blend;
            }
        }
    }

    /* Draw all corridors from the geometry emitter, colored per ring */
    station_geom_t geom;
    station_build_geom(station, &geom);
    for (int ci = 0; ci < geom.corridor_count; ci++) {
        int r = geom.corridors[ci].ring;
        draw_corridor_arc(station->pos, geom.corridors[ci].ring_radius,
            geom.corridors[ci].angle_a, geom.corridors[ci].angle_b,
            ring_cr[r], ring_cg[r], ring_cb[r], base_alpha * 0.7f);
    }

    /* Per-ring: tethers + modules (each ring rotates independently) */
    for (int ring = 1; ring <= STATION_NUM_RINGS; ring++) {
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
        for (int i = 0; i < mod_count; i++) {
            positions[i] = module_world_pos_ring(station, ring, station->modules[mod_idx[i]].slot);
        }

        /* Modules + dock indicators + furnace glow */
        for (int i = 0; i < mod_count; i++) {
            const station_module_t *m = &station->modules[mod_idx[i]];
            float angle = module_angle_ring(station, ring, m->slot);
            draw_module_at(positions[i], angle, m->type, m->scaffold, m->build_progress, station->pos);

            /* Furnace: glow + red laser beam to target module when smelting */
            if (!m->scaffold && (m->type == MODULE_FURNACE || m->type == MODULE_FURNACE_CU || m->type == MODULE_FURNACE_CR)) {
                float fr, fg, fb;
                module_color(m->type, &fr, &fg, &fb);
                float pulse = 0.3f + 0.15f * sinf(g.world.time * 3.0f + (float)m->slot);

                /* Always: warm glow at furnace */
                draw_circle_filled(positions[i], 44.0f, 12, fr * 0.6f, fg * 0.3f, fb * 0.15f, pulse * 0.3f);
                draw_circle_filled(positions[i], 28.0f, 10, fr * 0.9f, fg * 0.5f, fb * 0.2f, pulse * 0.4f);

                /* Find nearest module on an adjacent ring (inner or outer) */
                vec2 target = positions[i];
                {
                    float best_d = 1e18f;
                    int adj_rings[] = { ring + 1, ring - 1 };
                    for (int ri = 0; ri < 2; ri++) {
                        int adj = adj_rings[ri];
                        if (adj < 1 || adj > STATION_NUM_RINGS) continue;
                        for (int mi2 = 0; mi2 < station->module_count; mi2++) {
                            if (station->modules[mi2].ring != adj) continue;
                            vec2 mp2 = module_world_pos_ring(station, adj, station->modules[mi2].slot);
                            float dd = v2_dist_sq(positions[i], mp2);
                            if (dd < best_d) { best_d = dd; target = mp2; }
                        }
                    }
                }

                /* Check if any fragment is smelting near this furnace */
                bool has_smelting = false;
                for (int ai = 0; ai < MAX_ASTEROIDS; ai++) {
                    const asteroid_t *fa = &g.world.asteroids[ai];
                    if (!fa->active || fa->smelt_progress < 0.05f) continue;
                    if (v2_dist_sq(fa->pos, positions[i]) < 300.0f * 300.0f) {
                        has_smelting = true; break;
                    }
                }

                if (has_smelting) {
                    /* RED LASER between furnace and target — zappy flicker */
                    float flicker = 0.7f + 0.3f * sinf(g.world.time * 47.0f);
                    float zap1 = sinf(g.world.time * 31.0f) * 0.5f + 0.5f;
                    float zap2 = sinf(g.world.time * 53.0f) * 0.5f + 0.5f;
                    vec2 bdir = v2_sub(target, positions[i]);
                    float blen = sqrtf(v2_len_sq(bdir));
                    if (blen > 1.0f) {
                        vec2 nd = v2_scale(bdir, 1.0f / blen);
                        vec2 perp = v2(-nd.y, nd.x);
                        vec2 mid = v2_scale(v2_add(positions[i], target), 0.5f);
                        vec2 j1 = v2_add(mid, v2_scale(perp, 5.0f * zap1));
                        vec2 j2 = v2_add(mid, v2_scale(perp, -5.0f * zap2));
                        /* Main red beam */
                        draw_segment(positions[i], target, 1.0f, 0.2f, 0.1f, 0.8f * flicker);
                        /* Jittering side beams */
                        draw_segment(positions[i], j1, 1.0f, 0.35f, 0.1f, 0.5f * flicker);
                        draw_segment(j1, target, 1.0f, 0.35f, 0.1f, 0.5f * flicker);
                        draw_segment(positions[i], j2, 1.0f, 0.15f, 0.05f, 0.4f * flicker);
                        draw_segment(j2, target, 1.0f, 0.15f, 0.05f, 0.4f * flicker);
                        /* Hot white core */
                        draw_segment(positions[i], target, 1.0f, 0.9f, 0.7f, 0.25f * flicker);
                    }
                    /* Glow at both ends */
                    draw_circle_filled(positions[i], 36.0f, 10, 1.0f, 0.3f, 0.1f, 0.3f * flicker);
                    draw_circle_filled(target, 28.0f, 8, 1.0f, 0.2f, 0.05f, 0.2f * flicker);
                } else {
                    /* Idle: faint connection line to target */
                    draw_segment(positions[i], target, fr, fg, fb, pulse * 0.15f);
                }
            }

            /* Fabricator: beam to nearest supplier when input buffer has material */
            if (!m->scaffold && (m->type == MODULE_FRAME_PRESS ||
                                  m->type == MODULE_LASER_FAB ||
                                  m->type == MODULE_TRACTOR_FAB)) {
                float fr, fg, fb;
                module_color(m->type, &fr, &fg, &fb);
                bool producing = station->module_input[mod_idx[i]] > 0.1f;

                /* Find nearest module that could supply this fab (furnace or storage) */
                vec2 supplier = positions[i];
                {
                    float best_d = 1e18f;
                    for (int mi2 = 0; mi2 < station->module_count; mi2++) {
                        if (mi2 == mod_idx[i]) continue;
                        if (station->modules[mi2].scaffold) continue;
                        module_type_t st = station->modules[mi2].type;
                        /* Suppliers: furnaces, silos, or other producers */
                        bool is_supplier = (st == MODULE_FURNACE || st == MODULE_FURNACE_CU ||
                                           st == MODULE_FURNACE_CR || st == MODULE_ORE_SILO ||
                                           st == MODULE_CARGO_BAY);
                        if (!is_supplier) continue;
                        vec2 sp = module_world_pos_ring(station, station->modules[mi2].ring,
                                                       station->modules[mi2].slot);
                        float dd = v2_dist_sq(positions[i], sp);
                        if (dd < best_d) { best_d = dd; supplier = sp; }
                    }
                }

                if (producing) {
                    /* Active: colored beam from supplier to fab */
                    float flicker = 0.7f + 0.3f * sinf(g.world.time * 37.0f + (float)m->slot * 2.0f);
                    float zap = sinf(g.world.time * 29.0f) * 0.5f + 0.5f;
                    vec2 bdir = v2_sub(supplier, positions[i]);
                    float blen = sqrtf(v2_len_sq(bdir));
                    if (blen > 1.0f) {
                        vec2 nd = v2_scale(bdir, 1.0f / blen);
                        vec2 perp = v2(-nd.y, nd.x);
                        vec2 mid = v2_scale(v2_add(positions[i], supplier), 0.5f);
                        vec2 j1 = v2_add(mid, v2_scale(perp, 4.0f * zap));
                        /* Main colored beam */
                        draw_segment(positions[i], supplier, fr, fg, fb, 0.6f * flicker);
                        /* Jitter beam */
                        draw_segment(positions[i], j1, fr * 0.7f, fg * 0.7f, fb * 0.7f, 0.35f * flicker);
                        draw_segment(j1, supplier, fr * 0.7f, fg * 0.7f, fb * 0.7f, 0.35f * flicker);
                        /* White core */
                        draw_segment(positions[i], supplier, 1.0f, 0.95f, 0.9f, 0.15f * flicker);
                    }
                    /* Glow at fab */
                    draw_circle_filled(positions[i], 30.0f, 8, fr * 0.8f, fg * 0.8f, fb * 0.8f, 0.2f * flicker);
                } else {
                    /* Idle: faint connection line */
                    float pulse = 0.3f + 0.15f * sinf(g.world.time * 2.0f + (float)m->slot);
                    draw_segment(positions[i], supplier, fr, fg, fb, pulse * 0.1f);
                }
            }

            /* Dock berth indicator: show assigned berth when docking,
             * or all unoccupied berths dimly when in range */
            if (m->type == MODULE_DOCK && is_nearby && !m->scaffold) {
                vec2 outward = v2_sub(positions[i], station->pos);
                float od = sqrtf(v2_len_sq(outward));
                if (od > 0.001f) outward = v2_scale(outward, 1.0f / od);
                vec2 tang = v2(-outward.y, outward.x);
                int dock_slots = STATION_RING_SLOTS[ring];
                float next_ang = module_angle_ring(station, ring, (m->slot + 1) % dock_slots);
                float dock_ang = module_angle_ring(station, ring, m->slot);
                float ang_to_next = wrap_angle(next_ang - dock_ang);
                float gap_dir = (ang_to_next > 0.0f) ? 1.0f : -1.0f;
                vec2 berths[3];
                berths[0] = v2_add(positions[i], v2_scale(outward, 55.0f));
                berths[1] = v2_add(positions[i], v2_scale(outward, -55.0f));
                berths[2] = v2_add(positions[i], v2_scale(tang, gap_dir * 55.0f));
                (void)dock_slots;

                /* Which dock module is this? Compute berth index offset. */
                int dock_idx = 0;
                for (int di = 0; di < station->module_count; di++) {
                    if (station->modules[di].type != MODULE_DOCK) continue;
                    if (di == mod_idx[i]) break;
                    dock_idx++;
                }
                int berth_base = dock_idx * 3;  /* BERTHS_PER_DOCK = 3 */

                int station_idx = (int)(station - g.world.stations);
                bool approaching = LOCAL_PLAYER.docking_approach &&
                    LOCAL_PLAYER.nearby_station == station_idx;
                float dp = 0.5f + 0.4f * sinf(g.world.time * 4.0f);

                for (int b = 0; b < 3; b++) {
                    int global_berth = berth_base + b;
                    bool is_assigned = approaching &&
                        LOCAL_PLAYER.dock_berth == global_berth;

                    /* When approaching: only show the assigned berth */
                    /* When just nearby: show all dimly */
                    float alpha;
                    float cr, cg, cb;
                    if (approaching) {
                        if (!is_assigned) continue;  /* hide non-assigned */
                        cr = 0.2f; cg = 1.0f; cb = 0.6f;
                        alpha = dp;
                    } else {
                        cr = 0.15f; cg = 0.5f; cb = 0.4f;
                        alpha = 0.15f;
                    }

                    vec2 bdir = (b < 2) ? outward : tang;
                    vec2 bperp = (b < 2) ? tang : outward;
                    float bw = 14.0f, bh = 8.0f;
                    vec2 c0 = v2_add(berths[b], v2_add(v2_scale(bdir, -bh), v2_scale(bperp, -bw)));
                    vec2 c1 = v2_add(berths[b], v2_add(v2_scale(bdir,  bh), v2_scale(bperp, -bw)));
                    vec2 c2 = v2_add(berths[b], v2_add(v2_scale(bdir,  bh), v2_scale(bperp,  bw)));
                    vec2 c3 = v2_add(berths[b], v2_add(v2_scale(bdir, -bh), v2_scale(bperp,  bw)));
                    sgl_c4f(cr, cg, cb, alpha);
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
}

void draw_ship_tractor_field(void) {
    float tr = ship_tractor_range(&LOCAL_PLAYER.ship);

    if (LOCAL_PLAYER.ship.tractor_active) {
        /* Pulse ring: expands from ship to tractor range over ~0.3s */
        float hold_time = g.world.time - g.input.tractor_press_time;
        float expand = clampf(hold_time / 0.3f, 0.0f, 1.0f);
        float radius = 20.0f + (tr - 20.0f) * expand;
        float alpha = (0.6f - 0.25f * expand) * (expand < 1.0f ? 1.0f : 0.5f);
        draw_circle_outline(LOCAL_PLAYER.ship.pos, radius, 40, PAL_F_SIGNAL_MINT, alpha);
    } else if (LOCAL_PLAYER.ship.towed_count > 0) {
        /* LEASHED: draw beam lines to fragments, color by stretch */
        float slack = tr * 0.5f;
        float band = tr - slack;
        for (int t = 0; t < LOCAL_PLAYER.ship.towed_count; t++) {
            int idx = LOCAL_PLAYER.ship.towed_fragments[t];
            if (idx < 0 || idx >= MAX_ASTEROIDS || !g.world.asteroids[idx].active) continue;
            vec2 fpos = g.world.asteroids[idx].pos;
            float dist = sqrtf(v2_dist_sq(LOCAL_PLAYER.ship.pos, fpos));
            float stretch = clampf((dist - slack) / band, 0.0f, 1.0f);
            /* Green when slack, amber when taut, red near snap */
            float beam_r = stretch;
            float beam_g = 1.0f - stretch * 0.6f;
            float beam_b = 0.3f * (1.0f - stretch);
            float beam_a = 0.15f + 0.45f * stretch;
            draw_segment(LOCAL_PLAYER.ship.pos, fpos, beam_r, beam_g, beam_b, beam_a);
        }
    }
}

void draw_ship(void) {
    /* While the death cinematic is rolling, the player ship is hidden —
     * we draw the wreckage at the death position via draw_death_wreckage. */
    if (g.death_cinematic.active) return;
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

/* Death wreckage — drawn at the cinematic position when the player has
 * died. Charred core hull + 8 shards drifting outward, plus an ember
 * burst right after the impact. */
void draw_death_wreckage(void) {
    if (!g.death_cinematic.active) return;
    float age = g.death_cinematic.age;
    vec2 wp = g.death_cinematic.pos;

    /* --- Initial explosion flare (first ~0.4s) --- */
    if (age < 0.4f) {
        float flare = 1.0f - (age / 0.4f);
        float r1 = 50.0f + (1.0f - flare) * 60.0f;
        draw_circle_outline(wp, r1, 24, 1.0f, 0.55f, 0.20f, flare * 0.85f);
        draw_circle_outline(wp, r1 * 0.6f, 18, 1.0f, 0.85f, 0.40f, flare * 0.60f);
        draw_circle_outline(wp, r1 * 0.3f, 14, 1.0f, 0.95f, 0.70f, flare * 0.40f);
    }

    /* --- Charred hull core: a few jagged triangles, scorched colors --- */
    sgl_push_matrix();
    sgl_translate(wp.x, wp.y, 0.0f);
    sgl_rotate(g.death_cinematic.angle, 0.0f, 0.0f, 1.0f);

    /* Scorched body — broken outline */
    sgl_c4f(0.32f, 0.10f, 0.06f, 0.95f);
    sgl_begin_triangles();
    sgl_v2f(8.0f, 0.0f);
    sgl_v2f(-10.0f, 9.0f);
    sgl_v2f(-12.0f, -3.0f);
    sgl_end();
    sgl_c4f(0.20f, 0.06f, 0.04f, 0.95f);
    sgl_begin_triangles();
    sgl_v2f(6.0f, -2.0f);
    sgl_v2f(-12.0f, -3.0f);
    sgl_v2f(-9.0f, -10.0f);
    sgl_end();
    /* Cracked outline */
    sgl_c4f(0.55f, 0.18f, 0.10f, 0.85f);
    sgl_begin_lines();
    sgl_v2f(8.0f, 0.0f); sgl_v2f(-10.0f, 9.0f);
    sgl_v2f(-10.0f, 9.0f); sgl_v2f(-12.0f, -3.0f);
    sgl_v2f(-12.0f, -3.0f); sgl_v2f(-9.0f, -10.0f);
    sgl_v2f(-9.0f, -10.0f); sgl_v2f(8.0f, 0.0f);
    /* Fissures */
    sgl_v2f(-2.0f, 4.0f); sgl_v2f(2.0f, -4.0f);
    sgl_v2f(0.0f, 0.0f); sgl_v2f(-7.0f, -2.0f);
    sgl_end();
    sgl_pop_matrix();

    /* --- Shards drifting outward --- */
    for (int i = 0; i < 8; i++) {
        float *f = g.death_cinematic.fragments[i];
        float fx = wp.x + f[0];
        float fy = wp.y + f[1];
        sgl_push_matrix();
        sgl_translate(fx, fy, 0.0f);
        sgl_rotate(f[4], 0.0f, 0.0f, 1.0f);
        /* Shard color tints darker over time */
        float fade = expf(-age * 0.25f);
        sgl_c4f(0.45f * fade, 0.18f * fade, 0.10f * fade, 0.85f);
        sgl_begin_triangles();
        float sz = 4.5f + (float)(i % 3) * 0.8f;
        sgl_v2f(sz, 0.0f);
        sgl_v2f(-sz * 0.6f, sz * 0.7f);
        sgl_v2f(-sz * 0.6f, -sz * 0.7f);
        sgl_end();
        /* Faint trailing line */
        sgl_c4f(0.30f * fade, 0.10f * fade, 0.05f * fade, 0.45f);
        sgl_begin_lines();
        sgl_v2f(-sz * 0.6f, 0.0f);
        sgl_v2f(-sz * 2.0f, 0.0f);
        sgl_end();
        sgl_pop_matrix();
    }

    /* --- Smoldering embers near the wreckage --- */
    for (int i = 0; i < 5; i++) {
        float t = g.world.time;
        float seed = (float)i * 1.7f;
        float ang = t * 0.6f + seed;
        float r = 12.0f + 4.0f * sinf(t * 1.3f + seed);
        float ex = wp.x + cosf(ang) * r;
        float ey = wp.y + sinf(ang) * r - age * 6.0f; /* embers drift up */
        float a = 0.4f + 0.3f * sinf(t * 5.0f + seed * 3.0f);
        if (a < 0.0f) a = 0.0f;
        sgl_begin_triangles();
        sgl_c4f(1.0f, 0.55f, 0.15f, a);
        sgl_v2f(ex - 1.0f, ey - 1.0f);
        sgl_v2f(ex + 1.0f, ey - 1.0f);
        sgl_v2f(ex,        ey + 2.0f);
        sgl_end();
    }
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
        /* NPC tow tether */
        const npc_ship_t *tnpc = &g.world.npc_ships[i];
        if (tnpc->towed_fragment >= 0 && tnpc->towed_fragment < MAX_ASTEROIDS) {
            const asteroid_t *ta = &g.world.asteroids[tnpc->towed_fragment];
            if (ta->active) {
                float tp = 0.4f + 0.15f * sinf(g.world.time * 3.0f + (float)i * 1.5f);
                draw_segment(tnpc->pos, ta->pos, 0.7f, 0.5f, 0.2f, tp);
            }
        }
    }
}

/* Draw furnace tractor beams: orange tendrils to nearby S-tier fragments.
 * Fragments being smelted glow brighter with sparks. */
void draw_hopper_tractors(void) {
    float pull_range = 300.0f;
    float pull_sq = pull_range * pull_range;
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (st->scaffold) continue;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].scaffold) continue;
            module_type_t mt = st->modules[m].type;
            if (mt != MODULE_FURNACE && mt != MODULE_FURNACE_CU && mt != MODULE_FURNACE_CR
                && mt != MODULE_ORE_SILO) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[m].ring, st->modules[m].slot);
            if (!on_screen(mp.x, mp.y, pull_range + 50.0f)) continue;

            float fr, fg, fb;
            module_color(mt, &fr, &fg, &fb);

            /* Draw orange tractor tendrils to all S-tier fragments in range */
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                const asteroid_t *a = &g.world.asteroids[i];
                if (!a->active || a->tier != ASTEROID_TIER_S) continue;
                float d_sq = v2_dist_sq(a->pos, mp);
                if (d_sq > pull_sq) continue;
                float d = sqrtf(d_sq);
                float t = 1.0f - d / pull_range;
                float pulse = 0.5f + 0.3f * sinf(g.world.time * 6.0f + (float)i * 1.7f);

                /* Zappy tractor tendril — jittery, electrical feel */
                float brightness = (a->smelt_progress > 0.01f) ? (0.6f + a->smelt_progress * 0.4f) : 0.3f;
                float zap = sinf(g.world.time * 37.0f + (float)i * 5.3f);
                float jitter = 4.0f * zap;
                vec2 mid = v2_scale(v2_add(mp, a->pos), 0.5f);
                vec2 perp = v2(-((a->pos.y - mp.y)), (a->pos.x - mp.x));
                float plen = sqrtf(v2_len_sq(perp));
                if (plen > 0.1f) perp = v2_scale(perp, jitter / plen);
                vec2 mid_jitter = v2_add(mid, perp);
                /* Two-segment zap line through jittered midpoint */
                draw_segment(mp, mid_jitter, fr, fg, fb, t * pulse * brightness);
                draw_segment(mid_jitter, a->pos, fr, fg, fb, t * pulse * brightness);
                /* Hot core line — straighter, brighter */
                draw_segment(mp, a->pos, 1.0f, 0.85f, 0.4f, t * pulse * brightness * 0.3f);

                /* Sparks on smelting fragments — more intense */
                if (a->smelt_progress > 0.1f) {
                    float spark1 = sinf(g.world.time * 31.0f + (float)i * 3.1f);
                    float spark2 = sinf(g.world.time * 43.0f + (float)i * 7.3f);
                    float spark3 = sinf(g.world.time * 19.0f + (float)i * 2.7f);
                    float sr = a->radius * 1.2f;
                    float sp = a->smelt_progress;
                    if (spark1 > 0.0f) {
                        vec2 s1 = v2_add(a->pos, v2(sr * sinf(g.world.time * 11.0f), sr * cosf(g.world.time * 13.0f)));
                        draw_segment(a->pos, s1, 1.0f, 0.9f, 0.3f, spark1 * sp * 0.7f);
                    }
                    if (spark2 > 0.0f) {
                        vec2 s2 = v2_add(a->pos, v2(-sr * cosf(g.world.time * 9.0f), sr * sinf(g.world.time * 7.0f)));
                        draw_segment(a->pos, s2, 1.0f, 0.7f, 0.15f, spark2 * sp * 0.5f);
                    }
                    if (spark3 > 0.0f) {
                        vec2 s3 = v2_add(a->pos, v2(sr * cosf(g.world.time * 17.0f), -sr * sinf(g.world.time * 23.0f)));
                        draw_segment(a->pos, s3, 0.9f, 0.5f, 0.1f, spark3 * sp * 0.4f);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Spark burst — short, jittery streaks at a contact point.            */
/* Used by the mining laser impact and ship collisions. The "seed"     */
/* parameter de-correlates per-call patterns so two simultaneous       */
/* bursts (e.g. beam + crash) don't pulse in lockstep.                 */
/* ------------------------------------------------------------------ */

static float hash11(float x) {
    /* Cheap deterministic noise in [-1, 1]: take the fractional part of
     * a chaotic sine, remap [0,1) -> [-1, 1). */
    float s = sinf(x * 127.1f + 311.7f) * 43758.5453f;
    float f = s - floorf(s);          /* [0, 1) */
    return f * 2.0f - 1.0f;           /* [-1, 1) */
}

void draw_spark_burst(vec2 pos, float intensity, bool red, float seed) {
    if (intensity <= 0.01f) return;
    /* Intensity > 1 grows the burst (used for damaging-velocity hits). */
    if (intensity > 2.5f) intensity = 2.5f;
    float scale = intensity > 1.0f ? intensity : 1.0f;
    float t = g.world.time;
    float bucket = floorf(t * 32.0f) + seed * 71.3f;

    /* Hot core cross — 3 very short rays */
    float core_r = red ? 1.0f : 1.0f;
    float core_g = red ? 0.42f : 0.95f;
    float core_b = red ? 0.18f : 0.78f;
    for (int k = 0; k < 3; k++) {
        float ang = hash11(bucket + (float)k * 3.7f) * PI_F;
        float len = (1.5f + 1.5f * fabsf(hash11(bucket * 1.3f + (float)k * 5.1f))) * scale;
        vec2 tip = v2_add(pos, v2(cosf(ang) * len, sinf(ang) * len));
        draw_segment(pos, tip, core_r, core_g, core_b, 0.85f * fminf(intensity, 1.0f));
    }

    /* Main spark plume — 6 streaks (8 when overdriven), tight radius */
    int streaks = (intensity > 1.0f) ? 8 : 6;
    for (int k = 0; k < streaks; k++) {
        float kseed = bucket + (float)k * 2.71f + seed;
        float gate = hash11(kseed * 0.91f);
        if (gate < 0.0f) continue; /* drop ~50% of streaks each frame */
        float ang = hash11(kseed) * PI_F;
        float len = (2.0f + 4.0f * fabsf(hash11(kseed * 1.7f))) * scale;
        vec2 tip = v2_add(pos, v2(cosf(ang) * len, sinf(ang) * len));
        float r = red ? 1.0f : 1.0f;
        float g = red ? (0.45f + 0.2f * fabsf(hash11(kseed * 0.5f))) : 0.85f;
        float b = red ? 0.15f : (0.25f + 0.3f * fabsf(hash11(kseed * 0.7f)));
        float a = (0.55f + 0.35f * gate) * fminf(intensity, 1.0f);
        draw_segment(pos, tip, r, g, b, a);
    }
}

void draw_beam(void) {
    if (!LOCAL_PLAYER.beam_active) {
        return;
    }

    if (LOCAL_PLAYER.scan_active) {
        /* Scan beam: cyan/blue — information, not damage */
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.30f, 0.70f, 1.0f, 0.90f);
        draw_segment(LOCAL_PLAYER.beam_start, LOCAL_PLAYER.beam_end, 0.15f, 0.50f, 0.90f, 0.35f);
    } else if (LOCAL_PLAYER.beam_hit && LOCAL_PLAYER.beam_ineffective) {
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

    /* Impact sparks at the beam contact point. */
    if (LOCAL_PLAYER.beam_hit) {
        bool is_station = LOCAL_PLAYER.scan_active &&
            LOCAL_PLAYER.scan_target_type == 1;
        bool is_asteroid = !LOCAL_PLAYER.scan_active;
        if (is_asteroid) {
            draw_spark_burst(LOCAL_PLAYER.beam_end,
                             LOCAL_PLAYER.beam_ineffective ? 0.7f : 1.0f,
                             LOCAL_PLAYER.beam_ineffective,
                             3.14f);
        } else if (is_station) {
            /* Lasering a station/module — hot orange metal sparks. */
            draw_spark_burst(LOCAL_PLAYER.beam_end, 0.9f, true, 9.7f);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Collision sparks — emit a burst at any point where the local ship  */
/* hull is currently in contact with an asteroid or station body.     */
/* Pure visual; the server is authoritative for damage.               */
/* ------------------------------------------------------------------ */

void draw_collision_sparks(void) {
    if (LOCAL_PLAYER.docked) return;
    vec2 sp = LOCAL_PLAYER.ship.pos;
    vec2 sv = LOCAL_PLAYER.ship.vel;
    float ship_r = ship_hull_def(&LOCAL_PLAYER.ship)->ship_radius;
    /* Only spark on actual hull contact (no slack). */
    const float pad = 0.0f;

    /* Pick the single deepest asteroid contact this frame and the deepest
     * station contact — avoids the screen-filling cluster effect when
     * threading through a tight rock field. */
    int best_a = -1;
    float best_a_overlap = 0.0f;
    for (int i = 0; i < MAX_ASTEROIDS; i++) {
        const asteroid_t *a = &g.world.asteroids[i];
        if (!a->active) continue;
        float reach = ship_r + a->radius + pad;
        vec2 d = v2_sub(a->pos, sp);
        float d_sq = v2_len_sq(d);
        if (d_sq >= reach * reach) continue;
        float overlap = reach - sqrtf(d_sq);
        if (overlap > best_a_overlap) { best_a_overlap = overlap; best_a = i; }
    }
    if (best_a >= 0) {
        const asteroid_t *a = &g.world.asteroids[best_a];
        vec2 d = v2_sub(a->pos, sp);
        float dist = sqrtf(v2_len_sq(d));
        if (dist > 0.01f) {
            vec2 normal = v2_scale(d, 1.0f / dist);
            vec2 rel = v2_sub(sv, a->vel);
            float closing = v2_dot(rel, normal);
            /* SHIP_COLLISION_DAMAGE_THRESHOLD = 115. Below = scrape, above
             * = damaging hit, where the burst grows past 1.0 intensity. */
            float intensity;
            if (closing < 115.0f)
                intensity = 0.30f + fmaxf(0.0f, closing) * (0.7f / 115.0f);
            else
                intensity = 1.0f + fminf((closing - 115.0f) / 100.0f, 1.5f);
            vec2 contact = v2_add(sp, v2_scale(normal, ship_r - 1.0f));
            draw_spark_burst(contact, intensity, false, (float)best_a * 0.37f);
        }
    }

    int best_s = -1;
    float best_s_overlap = 0.0f;
    for (int i = 0; i < MAX_STATIONS; i++) {
        const station_t *st = &g.world.stations[i];
        if (!station_exists(st)) continue;
        if (st->planned) continue;
        float reach = ship_r + st->radius + pad;
        vec2 d = v2_sub(st->pos, sp);
        float d_sq = v2_len_sq(d);
        if (d_sq >= reach * reach) continue;
        float overlap = reach - sqrtf(d_sq);
        if (overlap > best_s_overlap) { best_s_overlap = overlap; best_s = i; }
    }
    if (best_s >= 0) {
        const station_t *st = &g.world.stations[best_s];
        vec2 d = v2_sub(st->pos, sp);
        float dist = sqrtf(v2_len_sq(d));
        if (dist > 0.01f) {
            vec2 normal = v2_scale(d, 1.0f / dist);
            float closing = v2_dot(sv, normal);
            float intensity;
            if (closing < 115.0f)
                intensity = 0.40f + fmaxf(0.0f, closing) * (0.6f / 115.0f);
            else
                intensity = 1.0f + fminf((closing - 115.0f) / 100.0f, 1.5f);
            vec2 contact = v2_add(sp, v2_scale(normal, ship_r - 1.0f));
            draw_spark_burst(contact, intensity, true, (float)best_s * 1.13f + 17.0f);
        }
    }
}

/* Draw autopilot path preview: dotted line from ship through next waypoints.
 * Only draws one screen-width worth (~1200u) so it doesn't clutter. */
void draw_autopilot_path(void) {
    if (!LOCAL_PLAYER.autopilot_mode) return;

    /* In MP mode, the server syncs its actual A* path waypoints via
     * PLAYER_SHIP message. g.autopilot_path is already populated by
     * apply_remote_player_ship in net_sync.c. No client computation. */

    if (g.autopilot_path_count == 0) return;
    vec2 prev = LOCAL_PLAYER.ship.pos;
    float total_drawn = 0.0f;
    const float MAX_DRAW_DIST = 1200.0f;
    const float DASH_LEN = 20.0f;
    const float GAP_LEN = 15.0f;
    for (int i = g.autopilot_path_current; i < g.autopilot_path_count; i++) {
        vec2 wp = g.autopilot_path[i];
        vec2 delta = v2_sub(wp, prev);
        float seg_len = v2_len(delta);
        if (seg_len < 1.0f) { prev = wp; continue; }
        float remaining = MAX_DRAW_DIST - total_drawn;
        if (remaining <= 0.0f) break;
        if (seg_len > remaining) seg_len = remaining;
        vec2 dir = v2_scale(delta, 1.0f / v2_len(delta));
        /* Draw dashed line along this segment */
        float t = 0.0f;
        float pulse = 0.35f + 0.15f * sinf(g.world.time * 2.0f);
        sgl_begin_lines();
        sgl_c4f(0.3f, 0.85f, 1.0f, pulse);
        while (t < seg_len) {
            float dash_end = t + DASH_LEN;
            if (dash_end > seg_len) dash_end = seg_len;
            vec2 a = v2_add(prev, v2_scale(dir, t));
            vec2 b = v2_add(prev, v2_scale(dir, dash_end));
            sgl_v2f(a.x, a.y);
            sgl_v2f(b.x, b.y);
            t = dash_end + GAP_LEN;
        }
        sgl_end();
        /* Small dot at waypoint */
        draw_circle_filled(wp, 3.0f, 6, 0.3f, 0.85f, 1.0f, pulse * 1.2f);
        total_drawn += seg_len;
        prev = wp;
    }
}

/* Draw tractor tether lines from ship to towed fragments */
void draw_towed_tethers(void) {
    if (LOCAL_PLAYER.ship.towed_count == 0) return;
    for (int t = 0; t < LOCAL_PLAYER.ship.towed_count; t++) {
        int idx = LOCAL_PLAYER.ship.towed_fragments[t];
        if (idx < 0 || idx >= MAX_ASTEROIDS) continue;
        const asteroid_t *a = &g.world.asteroids[idx];
        if (!a->active) continue;
        /* Faint teal tether line */
        float pulse = 0.4f + 0.15f * sinf(g.world.time * 3.0f + (float)t * 1.5f);
        draw_segment(LOCAL_PLAYER.ship.pos, a->pos, 0.25f, 0.85f, 0.80f, pulse);
    }
}

/* --- Compass ring: navigation pips around the player ship --- */
void draw_compass_ring(void) {
    if (LOCAL_PLAYER.docked) return;
    vec2 ship = LOCAL_PLAYER.ship.pos;
    float ring_r = 120.0f;
    float pip_size = 8.0f;

    /* Faint ring outline */
    draw_circle_outline(ship, ring_r, 32, 0.25f, 0.27f, 0.30f, 0.07f);

    /* Local callsign rendered with sdtx — see draw_callsigns() pass below. */

    /* Helper: draw a chevron pip at position on the ring */
    #define COMPASS_PIP(target, pr, pg, pb) do { \
        vec2 _to = v2_sub(target, ship); \
        float _dsq = v2_len_sq(_to); \
        if (_dsq > 2500.0f) { \
            float _a = atan2f(_to.y, _to.x); \
            float _px = ship.x + cosf(_a) * ring_r; \
            float _py = ship.y + sinf(_a) * ring_r; \
            float _ca = cosf(_a), _sa = sinf(_a); \
            float _pulse = 0.6f + 0.3f * sinf(g.world.time * 3.0f); \
            sgl_begin_lines(); sgl_c4f(pr, pg, pb, _pulse); \
            sgl_v2f(_px+(-_ca*pip_size-_sa*pip_size*0.6f), _py+(-_sa*pip_size+_ca*pip_size*0.6f)); sgl_v2f(_px, _py); \
            sgl_v2f(_px, _py); sgl_v2f(_px+(-_ca*pip_size+_sa*pip_size*0.6f), _py+(-_sa*pip_size-_ca*pip_size*0.6f)); \
            sgl_end(); \
        } \
    } while(0)

    /* Nearest station pip (green) */
    {
        const station_t *nav = navigation_station_ptr();
        if (nav) COMPASS_PIP(nav->pos, 0.34f, 0.96f, 0.76f);
    }

    /* Nav pip (yellow, blueprint placement) */
    if (g.nav_pip_active && g.nav_pip_is_blueprint)
        COMPASS_PIP(g.nav_pip_pos, 1.0f, 0.87f, 0.20f);

    /* Nearest minable asteroid pip (red) — filtered by mining level
     * so the pip matches what the autopilot would target. */
    {
        float best_d = 1e18f;
        vec2 best_pos = ship;
        bool found = false;
        /* Inline max_mineable_tier: level 0→M, 1→L, 2→XL */
        asteroid_tier_t max_tier = (LOCAL_PLAYER.ship.mining_level >= 2) ? ASTEROID_TIER_XL
                                 : (LOCAL_PLAYER.ship.mining_level >= 1) ? ASTEROID_TIER_L
                                 : ASTEROID_TIER_M;
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            const asteroid_t *a = &g.world.asteroids[i];
            if (!a->active) continue;
            if (a->tier == ASTEROID_TIER_S) continue;
            if ((int)a->tier < (int)max_tier) continue; /* too tough */
            float d = v2_dist_sq(a->pos, ship);
            if (d < best_d) { best_d = d; best_pos = a->pos; found = true; }
        }
        if (found) COMPASS_PIP(best_pos, 0.9f, 0.25f, 0.2f);
    }

    /* Tracked contract pip (yellow) */
    if (g.tracked_contract >= 0 && g.tracked_contract < MAX_CONTRACTS) {
        contract_t *ct = &g.world.contracts[g.tracked_contract];
        if (ct->active) {
            vec2 target = (ct->action == CONTRACT_TRACTOR)
                ? g.world.stations[ct->station_index].pos : ct->target_pos;
            COMPASS_PIP(target, 1.0f, 0.87f, 0.20f);
        }
    }

    /* Nearest 3 remote players (colored pips) */
    if (g.multiplayer_enabled) {
        const NetPlayerState *rp = net_get_interpolated_players();
        int nearest[3] = {-1, -1, -1};
        float nearest_d[3] = {1e18f, 1e18f, 1e18f};
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (!rp[i].active || i == (int)net_local_id()) continue;
            if (rp[i].callsign[0] == '\0') continue;
            float d = v2_dist_sq(v2(rp[i].x, rp[i].y), ship);
            for (int s = 0; s < 3; s++) {
                if (d < nearest_d[s]) {
                    for (int j = 2; j > s; j--) { nearest[j] = nearest[j-1]; nearest_d[j] = nearest_d[j-1]; }
                    nearest[s] = i; nearest_d[s] = d; break;
                }
            }
        }
        static const float pcols[][3] = {
            {1.0f, 0.45f, 0.25f}, {0.25f, 1.0f, 0.55f}, {0.55f, 0.35f, 1.0f},
            {1.0f, 0.85f, 0.15f}, {0.15f, 0.85f, 1.0f}, {1.0f, 0.35f, 0.75f},
        };
        for (int s = 0; s < 3; s++) {
            int pi = nearest[s];
            if (pi < 0) continue;
            int ci = pi % 6;
            COMPASS_PIP(v2(rp[pi].x, rp[pi].y), pcols[ci][0], pcols[ci][1], pcols[ci][2]);
        }
    }

    #undef COMPASS_PIP
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

        /* Callsign label above ship */
        /* Callsign rendered with sdtx (real font) — see callsign pass below. */

        /* Mining or scan beam — server-authoritative endpoints. */
        if (mining) {
            bool scanning = (players[i].flags & 8) != 0;
            vec2 muzzle  = v2(players[i].beam_start_x, players[i].beam_start_y);
            vec2 beam_end = v2(players[i].beam_end_x, players[i].beam_end_y);
            if (scanning) {
                draw_segment(muzzle, beam_end, 0.30f, 0.70f, 1.0f, 0.6f);
            } else {
                draw_segment(muzzle, beam_end, cr, cg, cb, 0.6f);
            }
        }

        /* Tractor field circle + towed tethers */
        bool tractor_on = (players[i].flags & 16) != 0;
        if (tractor_on && players[i].towed_count > 0) {
            vec2 pos = v2(players[i].x, players[i].y);
            /* Compute tractor range from level (mirrors ship_tractor_range) */
            float base_range = 150.0f; /* default hull tractor_range */
            float tr = base_range + (float)players[i].tractor_level * SHIP_TRACTOR_UPGRADE_STEP;
            float pulse = 0.28f + (sinf(g.world.time * 7.0f + (float)i * 2.0f) * 0.08f);
            draw_circle_outline(pos, tr, 40, cr * 0.4f, cg * 0.8f, cb * 0.9f, pulse);

            /* Tether lines to towed fragments */
            for (int t = 0; t < players[i].towed_count && t < 10; t++) {
                uint8_t fi = players[i].towed_fragments[t];
                if (fi >= MAX_ASTEROIDS || fi == 0xFF) continue;
                const asteroid_t *a = &g.world.asteroids[fi];
                if (!a->active) continue;
                float tp = 0.4f + 0.15f * sinf(g.world.time * 3.0f + (float)t * 1.5f);
                draw_segment(pos, a->pos, cr * 0.4f, cg * 0.8f, cb * 0.7f, tp);
            }
        }
    }
}

/* ================================================================== */
/* Callsigns — readable sdtx labels above all visible ships           */
/* ================================================================== */

void draw_callsigns(void) {
    /* Set sdtx canvas to match the sgl world projection exactly.
     * sgl_ortho uses [cam_left..cam_right, cam_top..cam_bottom].
     * sdtx_canvas(w, h) maps [0..w, 0..h] with 8px character cells.
     * By using the world view dimensions as canvas size and offsetting
     * the origin to cam_left/cam_top, sdtx_pos takes world coordinates
     * directly — no manual mapping needed. */
    float view_w = cam_right() - cam_left();
    float view_h = cam_bottom() - cam_top();
    const float cell = 8.0f;
    sdtx_canvas(view_w, view_h);
    sdtx_origin(cam_left() / cell, cam_top() / cell);

    /* Remote player callsigns */
    if (g.multiplayer_enabled) {
        const NetPlayerState *players = net_get_interpolated_players();
        int local_id = (int)net_local_id();
        for (int i = 0; i < NET_MAX_PLAYERS; i++) {
            if (!players[i].active) continue;
            if (i == local_id) continue;
            if (players[i].callsign[0] == '\0') continue;
            if (!on_screen(players[i].x, players[i].y, 60.0f)) continue;
            sdtx_color3b(PAL_WORLD_STATION_CYAN);
            int len = (int)strlen(players[i].callsign);
            /* Position in world coords — canvas is set to world view */
            sdtx_pos((players[i].x - len * cell * 0.5f) / cell,
                     (players[i].y - 36.0f) / cell);
            sdtx_puts(players[i].callsign);
        }
    }
}

void draw_npc_chatter(void) {
    /* Same world-space canvas as draw_callsigns */
    float view_w = cam_right() - cam_left();
    float view_h = cam_bottom() - cam_top();
    const float cell = 8.0f;
    sdtx_canvas(view_w, view_h);
    sdtx_origin(cam_left() / cell, cam_top() / cell);

    for (int i = 0; i < MAX_NPC_SHIPS; i++) {
        const npc_ship_t *npc = &g.world.npc_ships[i];
        if (!npc->active) continue;
        if (npc->role == NPC_ROLE_TOW) continue; /* tow drones: silent */
        if (!on_screen(npc->pos.x, npc->pos.y, 50.0f)) continue;
        float dx = npc->pos.x - LOCAL_PLAYER.ship.pos.x;
        float dy = npc->pos.y - LOCAL_PLAYER.ship.pos.y;
        if (dx * dx + dy * dy > 500.0f * 500.0f) continue; /* too far */

        /* Rotate line every 8 seconds, offset by NPC index */
        const char *line;
        if (npc->role == NPC_ROLE_MINER) {
            int idx = (i + (int)(g.world.time / 8.0f)) % NPC_CHATTER_MINER_COUNT;
            line = NPC_CHATTER_MINER[idx];
        } else {
            int idx = (i + (int)(g.world.time / 8.0f)) % NPC_CHATTER_HAULER_COUNT;
            line = NPC_CHATTER_HAULER[idx];
        }

        int len = (int)strlen(line);
        sdtx_color3b(PAL_RADIO_GREEN); /* faded radio green */
        sdtx_pos((npc->pos.x - len * cell * 0.5f) / cell,
                 (npc->pos.y + 24.0f) / cell);
        sdtx_puts(line);
    }
}

/* ================================================================== */
/* Scaffold world objects                                             */
/* ================================================================== */

void draw_scaffolds(void) {
    for (int i = 0; i < MAX_SCAFFOLDS; i++) {
        const scaffold_t *sc = &g.world.scaffolds[i];
        if (!sc->active) continue;
        if (!on_screen(sc->pos.x, sc->pos.y, sc->radius + 20.0f)) continue;

        float amb_r = 1.0f, amb_g = 0.85f, amb_b = 0.47f; /* construction amber */
        float pulse = 0.5f + 0.2f * sinf(g.world.time * 2.5f + sc->age * 3.0f);

        /* Module-type tint blended with amber */
        float mr, mg, mb;
        module_color_fn(sc->module_type, &mr, &mg, &mb);
        amb_r = lerpf(amb_r, mr, 0.3f);
        amb_g = lerpf(amb_g, mg, 0.3f);
        amb_b = lerpf(amb_b, mb, 0.3f);

        /* Nascent build progress (0..1) — drives visual fill */
        float build_frac = 0.0f;
        if (sc->state == SCAFFOLD_NASCENT) {
            float total = module_build_cost_lookup(sc->module_type);
            if (total > 0.0f) build_frac = sc->build_amount / total;
            if (build_frac > 1.0f) build_frac = 1.0f;
        }

        sgl_push_matrix();
        sgl_translate(sc->pos.x, sc->pos.y, 0.0f);
        sgl_rotate(sc->rotation, 0.0f, 0.0f, 1.0f);

        float r = sc->radius;
        /* Nascent scaffolds grow visually as build progress advances */
        if (sc->state == SCAFFOLD_NASCENT) {
            r = sc->radius * (0.4f + 0.6f * build_frac);
        }

        /* Wireframe octagon */
        sgl_begin_lines();
        int segs = 8;
        float alpha = (sc->state == SCAFFOLD_SNAPPING) ? pulse + 0.3f : pulse;
        sgl_c4f(amb_r, amb_g, amb_b, alpha);
        for (int s = 0; s < segs; s++) {
            float a0 = TWO_PI_F * (float)s / (float)segs;
            float a1 = TWO_PI_F * (float)(s + 1) / (float)segs;
            sgl_v2f(cosf(a0) * r, sinf(a0) * r);
            sgl_v2f(cosf(a1) * r, sinf(a1) * r);
        }
        /* Internal cross-brace — scaffolding structure */
        float inner = r * 0.6f;
        sgl_c4f(amb_r * 0.7f, amb_g * 0.7f, amb_b * 0.7f, alpha * 0.6f);
        sgl_v2f(-inner, -inner); sgl_v2f(inner, inner);
        sgl_v2f(-inner, inner); sgl_v2f(inner, -inner);
        sgl_v2f(-inner, 0); sgl_v2f(inner, 0);
        sgl_v2f(0, -inner); sgl_v2f(0, inner);
        sgl_end();

        /* Module type indicator: small filled circle at center */
        sgl_begin_triangles();
        sgl_c4f(mr * 0.8f, mg * 0.8f, mb * 0.8f, pulse * 0.5f);
        int csegs = 8;
        float cr2 = 6.0f;
        for (int s = 0; s < csegs; s++) {
            float a0 = TWO_PI_F * (float)s / (float)csegs;
            float a1 = TWO_PI_F * (float)(s + 1) / (float)csegs;
            sgl_v2f(0, 0);
            sgl_v2f(cosf(a0) * cr2, sinf(a0) * cr2);
            sgl_v2f(cosf(a1) * cr2, sinf(a1) * cr2);
        }
        sgl_end();

        sgl_pop_matrix();

        /* SNAPPING state: draw tendrils from station to scaffold */
        if (sc->state == SCAFFOLD_SNAPPING && sc->placed_station >= 0) {
            const station_t *st = &g.world.stations[sc->placed_station];
            vec2 target = module_world_pos_ring(st, sc->placed_ring, sc->placed_slot);
            float t_pulse = 0.4f + 0.3f * sinf(g.world.time * 4.0f);

            /* Main tendril: station slot → scaffold */
            draw_segment(target, sc->pos, amb_r * 0.6f, amb_g * 0.6f, amb_b * 0.4f, t_pulse);

            /* Secondary tendrils from station center */
            draw_segment(st->pos, sc->pos, amb_r * 0.3f, amb_g * 0.3f, amb_b * 0.2f, t_pulse * 0.4f);

            /* Target slot indicator: pulsing ring at the destination */
            draw_circle_outline(target, sc->radius + 4.0f, 12,
                amb_r * 0.5f, amb_g * 0.5f, amb_b * 0.3f, t_pulse * 0.6f);
        }
    }
}

void draw_scaffold_tether(void) {
    /* Tether line from player ship to towed scaffold */
    int idx = LOCAL_PLAYER.ship.towed_scaffold;
    if (idx < 0 || idx >= MAX_SCAFFOLDS) return;
    const scaffold_t *sc = &g.world.scaffolds[idx];
    if (!sc->active) return;

    float pulse = 0.5f + 0.2f * sinf(g.world.time * 3.0f);
    draw_segment(LOCAL_PLAYER.ship.pos, sc->pos, 0.5f, 0.85f, 0.75f, pulse);
}

/* Draw beams from producer modules to active shipyard intakes.
 * Shipyards with a pending order get a pulsing line to the nearest
 * same-ring producer of the required commodity. */
static module_type_t producer_for_commodity_client(commodity_t c) {
    switch (c) {
        case COMMODITY_FRAME:         return MODULE_FRAME_PRESS;
        case COMMODITY_FERRITE_INGOT: return MODULE_FURNACE;
        case COMMODITY_CUPRITE_INGOT: return MODULE_FURNACE_CU;
        case COMMODITY_CRYSTAL_INGOT: return MODULE_FURNACE_CR;
        default:                      return MODULE_COUNT;
    }
}

/* Compute max unlocked ring on a station. */
static int station_unlocked_rings_client(const station_t *st) {
    int counts[STATION_NUM_RINGS + 1] = {0};
    for (int m = 0; m < st->module_count; m++) {
        int r = st->modules[m].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS) counts[r]++;
    }
    for (int p = 0; p < st->placement_plan_count; p++) {
        int r = st->placement_plans[p].ring;
        if (r >= 1 && r <= STATION_NUM_RINGS) counts[r]++;
    }
    int unlocked = 1;
    if (counts[1] >= 2) unlocked = 2;
    if (counts[2] >= 4) unlocked = 3;
    return unlocked;
}

/* Draw planned stations (server-side ghost outposts) as wireframe rings.
 * Visible to all players. Materialized when a scaffold is towed near. */
static void draw_planned_stations(void) {
    for (int s = 3; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!st->planned) continue;
        vec2 c = st->pos;
        float pulse = 0.4f + 0.3f * sinf(g.world.time * 2.5f);
        int max_ring = station_unlocked_rings_client(st);

        /* Wireframe rings — dashed cyan, only unlocked */
        for (int r = 1; r <= max_ring; r++) {
            float radius = STATION_RING_RADIUS[r];
            int dashes = 32;
            sgl_begin_lines();
            sgl_c4f(0.4f, 0.85f, 1.0f, pulse * 0.6f);
            for (int i = 0; i < dashes; i += 2) {
                float a0 = TWO_PI_F * (float)i / (float)dashes;
                float a1 = TWO_PI_F * (float)(i + 1) / (float)dashes;
                sgl_v2f(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius);
                sgl_v2f(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius);
            }
            sgl_end();
        }
        /* Dashed dock-radius perimeter */
        {
            int dashes = 48;
            sgl_begin_lines();
            sgl_c4f(0.4f, 1.0f, 1.0f, pulse * 0.4f);
            float radius = OUTPOST_DOCK_RADIUS;
            for (int i = 0; i < dashes; i += 2) {
                float a0 = TWO_PI_F * (float)i / (float)dashes;
                float a1 = TWO_PI_F * (float)(i + 1) / (float)dashes;
                sgl_v2f(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius);
                sgl_v2f(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius);
            }
            sgl_end();
        }
        /* Center marker */
        draw_circle_outline(c, 6.0f, 12, 0.4f, 1.0f, 1.0f, pulse);

        /* Planned slot ghosts (already drawn by draw_placement_plans below) */
    }
}

/* Draw existing placement plans as faint colored ghosts at their slots. */
static void draw_placement_plans(void) {
    for (int s = 3; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st) || st->scaffold) continue;
        if (st->placement_plan_count == 0) continue;
        for (int p = 0; p < st->placement_plan_count; p++) {
            int ring = st->placement_plans[p].ring;
            int slot = st->placement_plans[p].slot;
            module_type_t type = st->placement_plans[p].type;
            vec2 pos = module_world_pos_ring(st, ring, slot);
            float mr, mg, mb;
            module_color_fn(type, &mr, &mg, &mb);
            float pulse = 0.25f + 0.15f * sinf(g.world.time * 1.5f + (float)p * 0.7f);
            /* Faint dashed outline + filled core in module color */
            draw_circle_outline(pos, 22.0f, 16, mr, mg, mb, pulse);
            draw_circle_filled(pos, 4.0f, 8, mr, mg, mb, pulse * 1.5f);
        }
    }
}

void draw_placement_reticle(void) {
    /* Always draw planned stations (server-side ghosts) */
    draw_planned_stations();
    /* Always draw existing plans on stations (active or planned) */
    draw_placement_plans();

    /* Ghost preview: draw wireframe rings around the player's ship. */
    if (g.plan_mode_active && g.plan_target_station == -1) {
        vec2 c = LOCAL_PLAYER.ship.pos;
        float pulse = 0.4f + 0.3f * sinf(g.world.time * 2.5f);
        /* Ring 1 wireframe (dashed cyan, same style as planned stations) */
        float radius = STATION_RING_RADIUS[1];
        int dashes = 32;
        sgl_begin_lines();
        sgl_c4f(0.4f, 0.85f, 1.0f, pulse * 0.6f);
        for (int i = 0; i < dashes; i += 2) {
            float a0 = TWO_PI_F * (float)i / (float)dashes;
            float a1 = TWO_PI_F * (float)(i + 1) / (float)dashes;
            sgl_v2f(c.x + cosf(a0) * radius, c.y + sinf(a0) * radius);
            sgl_v2f(c.x + cosf(a1) * radius, c.y + sinf(a1) * radius);
        }
        sgl_end();
        /* Center marker */
        draw_circle_outline(c, 6.0f, 12, 0.4f, 1.0f, 1.0f, pulse);
        /* Slot dots around ring 1 — all slots shown as small circles */
        int slots_n = STATION_RING_SLOTS[1];
        for (int slot = 0; slot < slots_n; slot++) {
            float angle = TWO_PI_F * (float)slot / (float)slots_n;
            vec2 sp = v2_add(c, v2(cosf(angle) * radius, sinf(angle) * radius));
            bool active = (slot == g.placement_target_slot && g.placement_target_ring == 1);
            if (active) {
                float mr, mg, mb;
                module_color_fn((module_type_t)g.plan_type, &mr, &mg, &mb);
                float ap = 0.5f + 0.4f * sinf(g.world.time * 5.0f);
                draw_circle_outline(sp, 32.0f, 24, mr, mg, mb, ap);
                draw_circle_outline(sp, 26.0f, 24, mr, mg, mb, ap * 0.7f);
                draw_circle_filled(sp, 6.0f, 8, mr, mg, mb, ap);
            } else {
                /* Green dot = empty slot available */
                draw_circle_filled(sp, 4.0f, 8, 0.3f, 0.9f, 0.5f, pulse * 0.7f);
            }
        }
    }

    /* Plan mode on real station: draw the cycling-type ghost at the
     * current target slot. */
    if (g.plan_mode_active && g.placement_target_station >= 0) {
        const station_t *st = &g.world.stations[g.placement_target_station];
        if (station_exists(st)) {
            vec2 target = module_world_pos_ring(st, g.placement_target_ring, g.placement_target_slot);
            float mr, mg, mb;
            module_color_fn((module_type_t)g.plan_type, &mr, &mg, &mb);
            float pulse = 0.5f + 0.4f * sinf(g.world.time * 5.0f);
            /* Bright module-tinted ring */
            draw_circle_outline(target, 32.0f, 24, mr, mg, mb, pulse);
            draw_circle_outline(target, 26.0f, 24, mr, mg, mb, pulse * 0.7f);
            draw_circle_filled(target, 6.0f, 8, mr, mg, mb, pulse);
            /* Crosshair tick marks */
            sgl_begin_lines();
            sgl_c4f(mr, mg, mb, pulse);
            float tick = 10.0f;
            sgl_v2f(target.x - 40.0f, target.y); sgl_v2f(target.x - 40.0f + tick, target.y);
            sgl_v2f(target.x + 40.0f, target.y); sgl_v2f(target.x + 40.0f - tick, target.y);
            sgl_v2f(target.x, target.y - 40.0f); sgl_v2f(target.x, target.y - 40.0f + tick);
            sgl_v2f(target.x, target.y + 40.0f); sgl_v2f(target.x, target.y + 40.0f - tick);
            sgl_end();
            /* Tether line from ship to target */
            draw_segment(LOCAL_PLAYER.ship.pos, target, mr, mg, mb, pulse * 0.5f);
        }
    }

    /* Outpost lock effect: expanding ring flash at lock position. */
    if (g.outpost_lock_timer > 0.0f) {
        float t = 1.0f - (g.outpost_lock_timer / 1.5f); /* 0→1 over lifetime */
        float expand_r = STATION_RING_RADIUS[1] * (0.8f + 0.5f * t);
        float alpha = (1.0f - t) * 1.2f;
        if (alpha > 1.0f) alpha = 1.0f;
        draw_circle_outline(g.outpost_lock_pos, expand_r, 48, 0.4f, 1.0f, 0.8f, alpha);
        draw_circle_outline(g.outpost_lock_pos, expand_r * 0.6f, 32, 0.6f, 1.0f, 1.0f, alpha * 0.6f);
    }

    if (!g.placement_reticle_active) return;

    vec2 target;
    bool slot_mode = (g.placement_target_station >= 0);
    bool valid = true;

    if (slot_mode) {
        int s = g.placement_target_station;
        if (s < 0 || s >= MAX_STATIONS) return;
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) return;
        target = module_world_pos_ring(st, g.placement_target_ring, g.placement_target_slot);
    } else {
        /* Found-new-outpost preview: show reticle at the scaffold's position.
         * Color it red if signal is too weak / placement is invalid. */
        int idx = LOCAL_PLAYER.ship.towed_scaffold;
        if (idx < 0 || idx >= MAX_SCAFFOLDS) return;
        const scaffold_t *sc = &g.world.scaffolds[idx];
        if (!sc->active) return;
        target = sc->pos;
        /* Validity: signal range present and no station too close */
        valid = signal_strength_at(&g.world, target) > 0.0f;
        if (valid) {
            for (int s = 0; s < MAX_STATIONS; s++) {
                const station_t *st = &g.world.stations[s];
                if (!station_exists(st)) continue;
                if (v2_dist_sq(st->pos, target) < OUTPOST_MIN_DISTANCE * OUTPOST_MIN_DISTANCE) {
                    valid = false; break;
                }
            }
        }
    }

    float pulse = 0.5f + 0.4f * sinf(g.world.time * 5.0f);
    float r = valid ? 0.4f : 1.0f;
    float g0 = valid ? 1.0f : 0.3f;
    float b = valid ? 1.0f : 0.3f;

    if (slot_mode) {
        /* Slot reticle: small precise crosshair */
        draw_circle_outline(target, 30.0f, 24, r, g0, b, pulse);
        draw_circle_outline(target, 24.0f, 24, r, g0, b, pulse * 0.6f);
        sgl_begin_lines();
        sgl_c4f(r, g0, b, pulse);
        float tick = 8.0f;
        sgl_v2f(target.x - 36.0f, target.y); sgl_v2f(target.x - 36.0f + tick, target.y);
        sgl_v2f(target.x + 36.0f, target.y); sgl_v2f(target.x + 36.0f - tick, target.y);
        sgl_v2f(target.x, target.y - 36.0f); sgl_v2f(target.x, target.y - 36.0f + tick);
        sgl_v2f(target.x, target.y + 36.0f); sgl_v2f(target.x, target.y + 36.0f - tick);
        sgl_end();
        draw_segment(LOCAL_PLAYER.ship.pos, target, r, g0, b, pulse * 0.5f);
    } else {
        /* Outpost-founding reticle: larger, dashed circle showing the
         * approximate dock radius of the outpost-to-be. */
        draw_circle_outline(target, OUTPOST_DOCK_RADIUS, 32, r, g0, b, pulse * 0.7f);
        draw_circle_outline(target, OUTPOST_RADIUS, 18, r, g0, b, pulse);
        /* Compass tick marks */
        sgl_begin_lines();
        sgl_c4f(r, g0, b, pulse);
        for (int i = 0; i < 4; i++) {
            float a = (float)i * (TWO_PI_F / 4.0f);
            float r1 = OUTPOST_DOCK_RADIUS - 6.0f;
            float r2 = OUTPOST_DOCK_RADIUS + 6.0f;
            sgl_v2f(target.x + cosf(a) * r1, target.y + sinf(a) * r1);
            sgl_v2f(target.x + cosf(a) * r2, target.y + sinf(a) * r2);
        }
        sgl_end();
    }
}

void draw_shipyard_intake_beams(void) {
    /* Find each nascent scaffold and draw beams from contributing modules
     * (producer modules of the required commodity, plus the shipyard itself)
     * converging on the scaffold at the station center. */
    for (int si = 0; si < MAX_SCAFFOLDS; si++) {
        const scaffold_t *sc = &g.world.scaffolds[si];
        if (!sc->active || sc->state != SCAFFOLD_NASCENT) continue;
        int s = sc->built_at_station;
        if (s < 0 || s >= MAX_STATIONS) continue;
        const station_t *st = &g.world.stations[s];
        if (!station_exists(st)) continue;

        commodity_t mat = module_build_material_lookup(sc->module_type);
        module_type_t prod_type = producer_for_commodity_client(mat);

        vec2 target = sc->pos;
        float t = g.world.time * 4.0f;

        /* Beam from each contributing module */
        for (int i = 0; i < st->module_count; i++) {
            if (st->modules[i].scaffold) continue;
            bool is_yard = (st->modules[i].type == MODULE_SHIPYARD);
            bool is_prod = (st->modules[i].type == prod_type);
            if (!is_yard && !is_prod) continue;

            vec2 mod_pos = module_world_pos_ring(st, st->modules[i].ring, st->modules[i].slot);
            float pulse = 0.4f + 0.3f * sinf(t + (float)i * 0.7f);
            if (is_yard) pulse *= 0.7f; /* shipyard line is steadier */

            /* Different color for shipyard vs producer */
            float r = is_yard ? 0.5f : 1.0f;
            float gc = is_yard ? 0.75f : 0.85f;
            float b = is_yard ? 1.0f : 0.47f;

            draw_segment(mod_pos, target, r, gc, b, pulse);

            /* Flow dots along the beam */
            int dots = 4;
            for (int d = 0; d < dots; d++) {
                float frac = fmodf((t * 0.18f) + (float)d / (float)dots, 1.0f);
                vec2 p = v2_add(mod_pos, v2_scale(v2_sub(target, mod_pos), frac));
                draw_circle_filled(p, 2.5f, 6, r, gc, b, pulse + 0.15f);
            }
        }
    }
}
