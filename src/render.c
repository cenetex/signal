#include <math.h>
#include "render.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"

void draw_circle_filled(vec2 center, float radius, int segments, float r, float g0, float b, float a) {
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

void draw_circle_outline(vec2 center, float radius, int segments, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_line_strip();
    for (int i = 0; i <= segments; i++) {
        float angle = ((float)i / (float)segments) * TWO_PI_F;
        sgl_v2f(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
    }
    sgl_end();
}

void draw_rect_centered(vec2 center, float half_w, float half_h, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_quads();
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y + half_h);
    sgl_end();
}

void draw_rect_outline(vec2 center, float half_w, float half_h, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_line_strip();
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_end();
}

void draw_segment(vec2 start, vec2 end, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_begin_lines();
    sgl_v2f(start.x, start.y);
    sgl_v2f(end.x, end.y);
    sgl_end();
}

void commodity_material_tint(commodity_t commodity, float* mr, float* mg, float* mb) {
    switch (commodity) {
        case COMMODITY_FERRITE_ORE: *mr = 0.55f; *mg = 0.25f; *mb = 0.18f; break;
        case COMMODITY_CUPRITE_ORE: *mr = 0.22f; *mg = 0.30f; *mb = 0.50f; break;
        case COMMODITY_CRYSTAL_ORE: *mr = 0.25f; *mg = 0.48f; *mb = 0.30f; break;
        default: *mr = 0.30f; *mg = 0.31f; *mb = 0.34f; break;
    }
}

void asteroid_body_color(asteroid_tier_t tier, commodity_t commodity, float hp_ratio, float* r, float* g0, float* b) {
    float base_r = 0.30f, base_g = 0.31f, base_b = 0.34f;
    switch (tier) {
        case ASTEROID_TIER_XL: base_r = 0.29f; base_g = 0.31f; base_b = 0.42f; break;
        case ASTEROID_TIER_L: base_r = 0.31f; base_g = 0.33f; base_b = 0.38f; break;
        case ASTEROID_TIER_M: base_r = 0.26f; base_g = 0.36f; base_b = 0.42f; break;
        case ASTEROID_TIER_S: base_r = 0.28f; base_g = 0.44f; base_b = 0.36f; break;
        default: break;
    }
    float mat_r, mat_g, mat_b;
    commodity_material_tint(commodity, &mat_r, &mat_g, &mat_b);
    base_r = lerpf(base_r, mat_r, 0.5f);
    base_g = lerpf(base_g, mat_g, 0.5f);
    base_b = lerpf(base_b, mat_b, 0.5f);
    *r = lerpf(base_r * 0.72f, base_r * 1.16f, hp_ratio);
    *g0 = lerpf(base_g * 0.72f, base_g * 1.16f, hp_ratio);
    *b = lerpf(base_b * 0.72f, base_b * 1.16f, hp_ratio);
}
