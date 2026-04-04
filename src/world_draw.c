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
    case MODULE_SHIPYARD:        *r=0.80f; *g=0.65f; *b=0.20f; return;
    default:                     *r=0.30f; *g=0.35f; *b=0.40f; return;
    }
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

/* Per-type shape internals (drawn in local rotated space, 64x64 bounding) */
static void draw_module_shape(module_type_t type, float mr, float mg, float mb, float alpha) {
    switch (type) {
    case MODULE_DOCK: {
        /* Open berth clamp -- U-shaped bracket with docking arms */
        /* Back plate */
        sgl_c4f(mr*0.25f, mg*0.25f, mb*0.25f, alpha);
        fill_quad(-24, 16, 24, 16, 24, 24, -24, 24);
        /* Left arm */
        sgl_c4f(mr*0.4f, mg*0.4f, mb*0.4f, alpha);
        fill_quad(-24, -20, -18, -20, -18, 24, -24, 24);
        /* Right arm */
        fill_quad(18, -20, 24, -20, 24, 24, 18, 24);
        /* Clamp tips (inward-facing) */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        fill_quad(-18, -20, -10, -20, -10, -14, -18, -14);
        fill_quad(10, -20, 18, -20, 18, -14, 10, -14);
        /* Inner docking guide lights */
        fill_circle_local(-14, 0, 3, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha*0.7f);
        fill_circle_local( 14, 0, 3, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha*0.7f);
        /* Accent edges */
        sgl_c4f(mr, mg, mb, alpha*0.8f);
        sgl_begin_lines();
        sgl_v2f(-24, 24); sgl_v2f(-24, -20);
        sgl_v2f(-24, -20); sgl_v2f(-10, -20);
        sgl_v2f(10, -20); sgl_v2f(24, -20);
        sgl_v2f(24, -20); sgl_v2f(24, 24);
        sgl_v2f(-24, 24); sgl_v2f(24, 24);
        sgl_end();
        break;
    }
    case MODULE_ORE_BUYER: {
        /* Wide funnel/hopper -- open top tapering to narrow chute */
        /* Funnel walls (left trapezoid) */
        sgl_c4f(mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        sgl_begin_triangles();
        sgl_v2f(-28, -22); sgl_v2f(-10, 6); sgl_v2f(-10, -22);
        sgl_v2f(28, -22); sgl_v2f(10, -22); sgl_v2f(10, 6);
        sgl_end();
        /* Funnel interior (dark void) */
        sgl_c4f(0.02f, 0.02f, 0.03f, alpha);
        fill_quad(-10, -22, 10, -22, 10, 6, -10, 6);
        /* Chute body */
        sgl_c4f(mr*0.4f, mg*0.4f, mb*0.4f, alpha);
        fill_quad(-10, 6, 10, 6, 8, 24, -8, 24);
        /* Hopper rim */
        sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha);
        fill_quad(-28, -24, 28, -24, 28, -20, -28, -20);
        /* Left/right funnel lips */
        fill_quad(-28, -24, -24, -24, -10, 6, -10, 2);
        fill_quad(24, -24, 28, -24, 10, 2, 10, 6);
        /* Accent lines */
        sgl_c4f(mr*0.9f, mg*0.9f, mb*0.9f, alpha);
        sgl_begin_lines();
        sgl_v2f(-28, -24); sgl_v2f(-10, 6);
        sgl_v2f(28, -24); sgl_v2f(10, 6);
        sgl_v2f(-10, 6); sgl_v2f(-8, 24);
        sgl_v2f(10, 6); sgl_v2f(8, 24);
        sgl_end();
        break;
    }
    case MODULE_FURNACE: case MODULE_FURNACE_CU: case MODULE_FURNACE_CR: {
        /* Heavy industrial box with chimney/vent and inner glow */
        /* Main body -- thick rectangle */
        sgl_c4f(mr*0.25f, mg*0.25f, mb*0.25f, alpha);
        fill_quad(-24, -18, 24, -18, 24, 24, -24, 24);
        /* Chimney / exhaust stack */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-8, -28, 8, -28, 8, -18, -8, -18);
        /* Chimney cap */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_quad(-10, -28, 10, -28, 10, -26, -10, -26);
        /* Inner glow (color varies per furnace type via mr/mg/mb) */
        fill_circle_local(0, 4, 14, 10, mr*0.5f, mg*0.15f, mb*0.08f, alpha*0.45f);
        fill_circle_local(0, 4, 8, 8, mr*0.85f, mg*0.4f, mb*0.15f, alpha*0.7f);
        fill_circle_local(0, 4, 3, 6, mr*1.0f, mg*0.7f, mb*0.3f, alpha*0.9f);
        /* Grate bars across firebox */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        sgl_begin_lines();
        sgl_v2f(-16, 14); sgl_v2f(16, 14);
        sgl_v2f(-16, 18); sgl_v2f(16, 18);
        sgl_end();
        /* Heavy outline */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        sgl_begin_lines();
        sgl_v2f(-24, -18); sgl_v2f(24, -18); sgl_v2f(24, -18); sgl_v2f(24, 24);
        sgl_v2f(24, 24); sgl_v2f(-24, 24); sgl_v2f(-24, 24); sgl_v2f(-24, -18);
        sgl_v2f(-8, -18); sgl_v2f(-8, -28);
        sgl_v2f(8, -18); sgl_v2f(8, -28);
        sgl_end();
        break;
    }
    case MODULE_FRAME_PRESS: {
        /* Wide heavy rectangle with stamping ram and press marks */
        /* Press bed (bottom plate) */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-30, 4, 30, 4, 30, 22, -30, 22);
        /* Press head (top plate) */
        sgl_c4f(mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        fill_quad(-30, -22, 30, -22, 30, -4, -30, -4);
        /* Ram/piston columns */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_quad(-28, -22, -22, -22, -22, 22, -28, 22);
        fill_quad(22, -22, 28, -22, 28, 22, 22, 22);
        /* Stamp crosshair on bed */
        sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha);
        sgl_begin_lines();
        sgl_v2f(-14, 0); sgl_v2f(14, 0);
        sgl_v2f(0, -4); sgl_v2f(0, 4);
        /* Press marks / scoring */
        sgl_v2f(-10, -2); sgl_v2f(-10, 2);
        sgl_v2f(10, -2); sgl_v2f(10, 2);
        sgl_end();
        /* Outline */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        sgl_begin_lines();
        sgl_v2f(-30, -22); sgl_v2f(30, -22); sgl_v2f(30, -22); sgl_v2f(30, 22);
        sgl_v2f(30, 22); sgl_v2f(-30, 22); sgl_v2f(-30, 22); sgl_v2f(-30, -22);
        sgl_end();
        break;
    }
    case MODULE_LASER_FAB: {
        /* Octagon housing with precision crosshairs and lens */
        float s = 24.0f;
        float c = s * 0.414f; /* tan(PI/8) -- octagon corner cut */
        /* Octagon body */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        sgl_begin_triangles();
        /* Fan-fill octagon from center */
        float ox[8] = { -c, c, s, s, c, -c, -s, -s };
        float oy[8] = { -s, -s, -c, c, s, s, c, -c };
        for (int i = 0; i < 8; i++) {
            int j = (i + 1) % 8;
            sgl_v2f(0, 0); sgl_v2f(ox[i], oy[i]); sgl_v2f(ox[j], oy[j]);
        }
        sgl_end();
        /* Lens center */
        fill_circle_local(0, 0, 6, 8, mr*0.6f, mg*0.6f, mb*0.6f, alpha*0.7f);
        /* Precision crosshairs */
        sgl_c4f(mr*0.9f, mg*0.9f, mb*0.9f, alpha);
        sgl_begin_lines();
        sgl_v2f(-20, 0); sgl_v2f(-8, 0);
        sgl_v2f(8, 0); sgl_v2f(20, 0);
        sgl_v2f(0, -20); sgl_v2f(0, -8);
        sgl_v2f(0, 8); sgl_v2f(0, 20);
        sgl_end();
        /* Octagon outline */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        for (int i = 0; i < 8; i++) {
            int j = (i + 1) % 8;
            sgl_v2f(ox[i], oy[i]); sgl_v2f(ox[j], oy[j]);
        }
        sgl_end();
        break;
    }
    case MODULE_TRACTOR_FAB: {
        /* Circle with concentric field emitter rings */
        /* Outer housing disc */
        fill_circle_local(0, 0, 24, 20, mr*0.25f, mg*0.25f, mb*0.25f, alpha);
        /* Middle emitter ring */
        fill_circle_local(0, 0, 16, 16, mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        /* Inner core */
        fill_circle_local(0, 0, 8, 10, mr*0.55f, mg*0.55f, mb*0.55f, alpha*0.8f);
        /* Center dot */
        fill_circle_local(0, 0, 3, 6, mr*0.9f, mg*0.9f, mb*0.9f, alpha);
        /* Ring outlines (field emitter bands) */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha*0.8f);
        for (int ring = 0; ring < 3; ring++) {
            float rad = 8.0f + (float)ring * 8.0f;
            int segs = 16 + ring * 4;
            sgl_begin_lines();
            for (int i = 0; i < segs; i++) {
                float a0 = TWO_PI_F * (float)i / (float)segs;
                float a1 = TWO_PI_F * (float)(i+1) / (float)segs;
                sgl_v2f(cosf(a0)*rad, sinf(a0)*rad);
                sgl_v2f(cosf(a1)*rad, sinf(a1)*rad);
            }
            sgl_end();
        }
        /* Field direction spokes */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha*0.5f);
        sgl_begin_lines();
        for (int i = 0; i < 4; i++) {
            float a = TWO_PI_F * (float)i / 4.0f + PI_F * 0.25f;
            sgl_v2f(cosf(a)*10, sinf(a)*10);
            sgl_v2f(cosf(a)*22, sinf(a)*22);
        }
        sgl_end();
        break;
    }
    case MODULE_SIGNAL_RELAY: {
        /* Tall antenna mast */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-3, 28, 3, 28, 3, -20, -3, -20);
        /* Cross-arms */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_quad(-16, 6, 16, 6, 16, 2, -16, 2);
        fill_quad(-12, -8, 12, -8, 12, -12, -12, -12);
        /* Tip beacon */
        fill_circle_local(0, -22, 5, 8, mr*1.0f, mg*1.0f, mb*1.0f, alpha * 0.9f);
        /* Radiating signal arcs */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha * 0.5f);
        sgl_begin_lines();
        for (int r = 1; r <= 2; r++) {
            float rad = 14.0f + (float)r * 10.0f;
            for (int i = 0; i < 6; i++) {
                float a0 = -PI_F * 0.4f + (float)i * PI_F * 0.8f / 6.0f;
                float a1 = -PI_F * 0.4f + (float)(i+1) * PI_F * 0.8f / 6.0f;
                sgl_v2f(cosf(a0)*rad, sinf(a0)*rad - 16);
                sgl_v2f(cosf(a1)*rad, sinf(a1)*rad - 16);
            }
        }
        sgl_end();
        break;
    }
    case MODULE_REPAIR_BAY: {
        /* Open bay with refined wrench cross */
        /* Base plate */
        sgl_c4f(mr*0.28f, mg*0.28f, mb*0.28f, alpha);
        fill_quad(-24, -24, 24, -24, 24, 24, -24, 24);
        /* Wrench shaft (diagonal) */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        fill_quad(-3, -3, 3, -3, 16, 10, 10, 16);
        fill_quad(-16, -10, -10, -16, 3, 3, -3, 3);
        /* Wrench head (open jaw at top-right) */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        fill_quad(10, 16, 16, 10, 22, 16, 16, 22);
        fill_quad(16, 10, 22, 10, 22, 16, 16, 16);
        /* Wrench ring (bottom-left) */
        fill_circle_local(-13, -13, 8, 10, mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        for (int i = 0; i < 12; i++) {
            float a0 = TWO_PI_F * (float)i / 12.0f;
            float a1 = TWO_PI_F * (float)(i+1) / 12.0f;
            sgl_v2f(-13+cosf(a0)*8, -13+sinf(a0)*8);
            sgl_v2f(-13+cosf(a1)*8, -13+sinf(a1)*8);
        }
        sgl_end();
        /* Corner bolts */
        fill_circle_local(-20, -20, 2, 4, mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_circle_local( 20, -20, 2, 4, mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_circle_local(-20,  20, 2, 4, mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_circle_local( 20,  20, 2, 4, mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        /* Outline */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        sgl_begin_lines();
        sgl_v2f(-24,-24); sgl_v2f(24,-24); sgl_v2f(24,-24); sgl_v2f(24,24);
        sgl_v2f(24,24); sgl_v2f(-24,24); sgl_v2f(-24,24); sgl_v2f(-24,-24);
        sgl_end();
        break;
    }
    case MODULE_CONTRACT_BOARD: {
        /* Rectangular bulletin board with horizontal text lines */
        /* Board backing */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-20, -24, 20, -24, 20, 24, -20, 24);
        /* Board face (lighter) */
        sgl_c4f(mr*0.45f, mg*0.45f, mb*0.45f, alpha);
        fill_quad(-16, -20, 16, -20, 16, 20, -16, 20);
        /* Text lines */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha*0.8f);
        sgl_begin_lines();
        sgl_v2f(-12, -14); sgl_v2f(12, -14);
        sgl_v2f(-12, -8); sgl_v2f(10, -8);
        sgl_v2f(-12, -2); sgl_v2f(12, -2);
        sgl_v2f(-12, 4); sgl_v2f(8, 4);
        sgl_v2f(-12, 10); sgl_v2f(12, 10);
        sgl_v2f(-12, 16); sgl_v2f(6, 16);
        sgl_end();
        /* Pin / tack at top */
        fill_circle_local(0, -22, 3, 6, mr*0.9f, mg*0.4f, mb*0.3f, alpha);
        /* Frame outline */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        sgl_begin_lines();
        sgl_v2f(-20,-24); sgl_v2f(20,-24); sgl_v2f(20,-24); sgl_v2f(20,24);
        sgl_v2f(20,24); sgl_v2f(-20,24); sgl_v2f(-20,24); sgl_v2f(-20,-24);
        sgl_end();
        break;
    }
    case MODULE_ORE_SILO: {
        /* Tall cylindrical tank with dome top and rivet bands */
        /* Main cylinder body */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-16, -16, 16, -16, 16, 26, -16, 26);
        /* Dome top (triangular approximation) */
        sgl_c4f(mr*0.4f, mg*0.4f, mb*0.4f, alpha);
        sgl_begin_triangles();
        sgl_v2f(-16, -16); sgl_v2f(16, -16); sgl_v2f(0, -28);
        sgl_end();
        /* Base flange */
        sgl_c4f(mr*0.45f, mg*0.45f, mb*0.45f, alpha);
        fill_quad(-20, 22, 20, 22, 20, 26, -20, 26);
        /* Rivet / band lines */
        sgl_c4f(mr*0.55f, mg*0.55f, mb*0.55f, alpha*0.7f);
        sgl_begin_lines();
        sgl_v2f(-16, -6); sgl_v2f(16, -6);
        sgl_v2f(-16, 4); sgl_v2f(16, 4);
        sgl_v2f(-16, 14); sgl_v2f(16, 14);
        sgl_end();
        /* Fill level indicator */
        sgl_c4f(mr*0.7f, mg*0.5f, mb*0.2f, alpha*0.4f);
        fill_quad(-14, 4, 14, 4, 14, 22, -14, 22);
        /* Outline */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        sgl_begin_lines();
        sgl_v2f(-16, -16); sgl_v2f(-16, 26);
        sgl_v2f(16, -16); sgl_v2f(16, 26);
        sgl_v2f(-16, 26); sgl_v2f(16, 26);
        sgl_v2f(-16, -16); sgl_v2f(0, -28);
        sgl_v2f(16, -16); sgl_v2f(0, -28);
        sgl_end();
        break;
    }
    case MODULE_BLUEPRINT_DESK: {
        /* Drafting table with T-square and paper */
        /* Table surface */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-24, -16, 24, -16, 22, 20, -22, 20);
        /* Table legs (trapezoidal profile) */
        sgl_c4f(mr*0.25f, mg*0.25f, mb*0.25f, alpha);
        fill_quad(-22, 20, -18, 20, -16, 26, -24, 26);
        fill_quad(18, 20, 22, 20, 24, 26, 16, 26);
        /* Paper sheet */
        sgl_c4f(mr*0.55f, mg*0.55f, mb*0.55f, alpha*0.7f);
        fill_quad(-14, -12, 14, -12, 14, 14, -14, 14);
        /* T-square (horizontal bar + vertical stem) */
        sgl_c4f(mr*0.8f, mg*0.8f, mb*0.8f, alpha);
        sgl_begin_lines();
        /* Horizontal bar across top of paper */
        sgl_v2f(-18, -6); sgl_v2f(18, -6);
        /* Vertical stem down */
        sgl_v2f(0, -6); sgl_v2f(0, 14);
        sgl_end();
        /* Draft lines on paper */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha*0.4f);
        sgl_begin_lines();
        sgl_v2f(-10, 0); sgl_v2f(10, 0);
        sgl_v2f(-10, 6); sgl_v2f(8, 6);
        sgl_end();
        /* Table edge */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        sgl_begin_lines();
        sgl_v2f(-24, -16); sgl_v2f(24, -16);
        sgl_v2f(-24, -16); sgl_v2f(-22, 20);
        sgl_v2f(24, -16); sgl_v2f(22, 20);
        sgl_end();
        break;
    }
    case MODULE_SHIPYARD: {
        /* Large scaffold frame with crane arm */
        /* Main scaffold frame (open) */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        /* Left pillar */
        fill_quad(-30, -22, -24, -22, -24, 22, -30, 22);
        /* Right pillar */
        fill_quad(24, -22, 30, -22, 30, 22, 24, 22);
        /* Top beam */
        fill_quad(-30, -22, 30, -22, 30, -16, -30, -16);
        /* Bottom beam */
        fill_quad(-30, 16, 30, 16, 30, 22, -30, 22);
        /* Crane arm (extends from top-right, angled) */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        fill_quad(10, -22, 16, -22, 24, -30, 18, -30);
        /* Crane hook (dangling line + hook) */
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        sgl_begin_lines();
        sgl_v2f(21, -30); sgl_v2f(14, -16);
        sgl_v2f(14, -16); sgl_v2f(14, -6);
        /* Hook */
        sgl_v2f(14, -6); sgl_v2f(10, -2);
        sgl_v2f(10, -2); sgl_v2f(14, 0);
        sgl_end();
        /* Cross-bracing inside frame */
        sgl_c4f(mr*0.4f, mg*0.4f, mb*0.4f, alpha*0.5f);
        sgl_begin_lines();
        sgl_v2f(-24, -16); sgl_v2f(24, 16);
        sgl_v2f(24, -16); sgl_v2f(-24, 16);
        sgl_end();
        /* Scaffold outline */
        sgl_c4f(mr*0.6f, mg*0.6f, mb*0.6f, alpha);
        sgl_begin_lines();
        sgl_v2f(-30, -22); sgl_v2f(30, -22); sgl_v2f(30, -22); sgl_v2f(30, 22);
        sgl_v2f(30, 22); sgl_v2f(-30, 22); sgl_v2f(-30, 22); sgl_v2f(-30, -22);
        sgl_end();
        break;
    }
    case MODULE_INGOT_SELLER: {
        /* Crate/pallet with stacked ingot bars */
        /* Pallet base */
        sgl_c4f(mr*0.25f, mg*0.25f, mb*0.25f, alpha);
        fill_quad(-24, 16, 24, 16, 24, 24, -24, 24);
        /* Pallet slats */
        sgl_c4f(mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        fill_quad(-24, 18, 24, 18, 24, 20, -24, 20);
        /* Crate body */
        sgl_c4f(mr*0.3f, mg*0.3f, mb*0.3f, alpha);
        fill_quad(-20, -12, 20, -12, 20, 16, -20, 16);
        /* Stacked ingot bars (3 rows) */
        sgl_c4f(mr*0.65f, mg*0.65f, mb*0.65f, alpha);
        fill_quad(-16, 4, -2, 4, -2, 12, -16, 12);
        fill_quad(2, 4, 16, 4, 16, 12, 2, 12);
        sgl_c4f(mr*0.55f, mg*0.55f, mb*0.55f, alpha);
        fill_quad(-12, -4, 4, -4, 4, 2, -12, 2);
        fill_quad(6, -4, 16, -4, 16, 2, 6, 2);
        sgl_c4f(mr*0.7f, mg*0.7f, mb*0.7f, alpha);
        fill_quad(-8, -12, 8, -12, 8, -6, -8, -6);
        /* Crate outline */
        sgl_c4f(mr*0.5f, mg*0.5f, mb*0.5f, alpha);
        sgl_begin_lines();
        sgl_v2f(-20,-12); sgl_v2f(20,-12); sgl_v2f(20,-12); sgl_v2f(20,16);
        sgl_v2f(20,16); sgl_v2f(-20,16); sgl_v2f(-20,16); sgl_v2f(-20,-12);
        sgl_end();
        break;
    }
    case MODULE_RING: {
        /* Simple ring segment arc */
        sgl_c4f(mr*0.4f, mg*0.4f, mb*0.4f, alpha);
        sgl_begin_lines();
        for (int i = 0; i < 16; i++) {
            float a0 = TWO_PI_F * (float)i / 16.0f;
            float a1 = TWO_PI_F * (float)(i+1) / 16.0f;
            sgl_v2f(cosf(a0)*22, sinf(a0)*22);
            sgl_v2f(cosf(a1)*22, sinf(a1)*22);
        }
        sgl_end();
        break;
    }
    default: {
        /* Generic chamfered square fallback */
        float ch = 6.0f; /* chamfer size */
        sgl_c4f(mr*0.35f, mg*0.35f, mb*0.35f, alpha);
        /* Fill as center rect + corner triangles for chamfer */
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
    float alpha = scaffold ? 0.25f : 0.92f;
    (void)station_center;

    sgl_push_matrix();
    sgl_translate(pos.x, pos.y, 0.0f);
    sgl_rotate(angle, 0.0f, 0.0f, 1.0f);

    draw_module_shape(type, mr, mg, mb, alpha);

    if (scaffold && progress > 0.01f) {
        float bar_w = 48.0f * progress;
        sgl_c4f(0.3f, 1.0f, 0.6f, 0.7f);
        fill_quad(-24, 30, -24+bar_w, 30, -24+bar_w, 34, -24, 34);
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

/* Solid corridor tube between adjacent modules on the same ring. */
/* Draw a curved corridor that arcs along the ring radius between two module positions. */
#define CORRIDOR_ARC_SEGMENTS 8

static void draw_corridor_arc(vec2 center, float ring_radius, float angle_a, float angle_b,
                               float cr, float cg, float cb, float alpha) {
    float hw = 6.0f; /* corridor half-width */
    float r_inner = ring_radius - hw;
    float r_outer = ring_radius + hw;

    /* Tessellate the arc */
    float da = angle_b - angle_a;
    /* Normalize to shortest arc */
    while (da > PI_F) da -= TWO_PI_F;
    while (da < -PI_F) da += TWO_PI_F;

    /* Solid fill — triangle strip as quads */
    sgl_c4f(cr * 0.2f, cg * 0.2f, cb * 0.2f, alpha * 0.8f);
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

    /* Edge lines (inner and outer arcs) */
    sgl_c4f(cr * 0.4f, cg * 0.4f, cb * 0.4f, alpha * 0.6f);
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

        /* Curved corridors between adjacent occupied slots.
         * Inner ring (ring 1): skip the wrap-around to leave a gap. */
        float ring_r = STATION_RING_RADIUS[ring];
        for (int i = 0; i + 1 < mod_count; i++) {
            if (slot_ids[i + 1] - slot_ids[i] == 1) {
                float ang_a = module_angle_ring(station, ring, slot_ids[i]);
                float ang_b = module_angle_ring(station, ring, slot_ids[i + 1]);
                draw_corridor_arc(station->pos, ring_r, ang_a, ang_b,
                    role_r, role_g, role_b, base_alpha * 0.7f);
            }
        }
        /* Wrap: last→first if ring is full, but skip on ring 1 (leave a gap) */
        if (mod_count == slots && ring > 1) {
            float ang_a = module_angle_ring(station, ring, slot_ids[mod_count - 1]);
            float ang_b = module_angle_ring(station, ring, slot_ids[0]);
            draw_corridor_arc(station->pos, ring_r, ang_a, ang_b,
                role_r, role_g, role_b, base_alpha * 0.7f);
        }

        /* Modules + dock indicators */
        for (int i = 0; i < mod_count; i++) {
            const station_module_t *m = &station->modules[mod_idx[i]];
            float angle = module_angle_ring(station, ring, m->slot);
            draw_module_at(positions[i], angle, m->type, m->scaffold, m->build_progress, station->pos);

            /* Dock berth indicators: end + left side + right side */
            if (m->type == MODULE_DOCK && is_nearby && !m->scaffold) {
                float dp = 0.5f + 0.4f * sinf(g.world.time * 4.0f);
                vec2 outward = v2_sub(positions[i], station->pos);
                float od = sqrtf(v2_len_sq(outward));
                if (od > 0.001f) outward = v2_scale(outward, 1.0f / od);
                vec2 tang = v2(-outward.y, outward.x);
                /* End berth (tangentially past dock, capping ring), inner, outer */
                int dock_slots = STATION_RING_SLOTS[ring];
                float slot_arc = TWO_PI_F / (float)dock_slots;
                float end_ang = module_angle_ring(station, ring, m->slot) + slot_arc * 0.45f;
                vec2 end_pos = v2_add(station->pos,
                    v2(cosf(end_ang) * STATION_RING_RADIUS[ring],
                       sinf(end_ang) * STATION_RING_RADIUS[ring]));
                vec2 berths[3];
                berths[0] = end_pos;
                berths[1] = v2_add(positions[i], v2_scale(outward, -28.0f));
                berths[2] = v2_add(positions[i], v2_scale(outward, 28.0f));
                for (int b = 0; b < 3; b++) {
                    vec2 bdir = outward;
                    vec2 bperp = tang;
                    float bw = 14.0f, bh = 8.0f;
                    vec2 c0 = v2_add(berths[b], v2_add(v2_scale(bdir, -bh), v2_scale(bperp, -bw)));
                    vec2 c1 = v2_add(berths[b], v2_add(v2_scale(bdir,  bh), v2_scale(bperp, -bw)));
                    vec2 c2 = v2_add(berths[b], v2_add(v2_scale(bdir,  bh), v2_scale(bperp,  bw)));
                    vec2 c3 = v2_add(berths[b], v2_add(v2_scale(bdir, -bh), v2_scale(bperp,  bw)));
                    sgl_c4f(0.2f, 1.0f, 0.6f, dp * (b == 0 ? 1.0f : 0.6f));
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

/* Draw faint tractor lines from ore buyer modules to nearby fragments */
void draw_hopper_tractors(void) {
    for (int s = 0; s < MAX_STATIONS; s++) {
        const station_t *st = &g.world.stations[s];
        if (st->scaffold) continue;
        for (int m = 0; m < st->module_count; m++) {
            if (st->modules[m].type != MODULE_ORE_BUYER || st->modules[m].scaffold) continue;
            vec2 mp = module_world_pos_ring(st, st->modules[m].ring, st->modules[m].slot);
            if (!on_screen(mp.x, mp.y, 100.0f)) continue;
            for (int i = 0; i < MAX_ASTEROIDS; i++) {
                const asteroid_t *a = &g.world.asteroids[i];
                if (!asteroid_is_collectible(a)) continue;
                float d_sq = v2_dist_sq(a->pos, mp);
                if (d_sq > 80.0f * 80.0f) continue;
                float d = sqrtf(d_sq);
                float alpha = 0.15f + 0.2f * (1.0f - d / 80.0f);
                draw_segment(mp, a->pos, 0.25f, 0.75f, 0.90f, alpha);
            }
        }
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

        /* Mining or scan beam */
        if (mining) {
            bool scanning = (players[i].flags & 8) != 0;
            vec2 pos = v2(players[i].x, players[i].y);
            vec2 forward = v2_from_angle(players[i].angle);
            vec2 muzzle = v2_add(pos, v2_scale(forward, 24.0f));
            vec2 beam_end = v2_add(muzzle, v2_scale(forward, MINING_RANGE));
            if (scanning) {
                draw_segment(muzzle, beam_end, 0.30f, 0.70f, 1.0f, 0.6f);
            } else {
                draw_segment(muzzle, beam_end, cr, cg, cb, 0.6f);
            }
        }
    }
}
