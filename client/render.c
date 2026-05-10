#include <math.h>
#include <stdbool.h>
#include "render.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"

/* Batched line drawing — call between begin/end_line_batch */
static bool _line_batch_active = false;

void render_set_screen_space(float screen_w, float screen_h) {
    sgl_defaults();
    sgl_matrix_mode_projection();
    sgl_load_identity();
    sgl_ortho(0.0f, screen_w, screen_h, 0.0f, -1.0f, 1.0f);
    sgl_matrix_mode_modelview();
    sgl_load_identity();
}

void begin_line_batch(void) {
    sgl_begin_lines();
    _line_batch_active = true;
}

void end_line_batch(void) {
    sgl_end();
    _line_batch_active = false;
}

void draw_segment_batched(vec2 start, vec2 end, float r, float g0, float b, float a) {
    sgl_c4f(r, g0, b, a);
    sgl_v2f(start.x, start.y);
    sgl_v2f(end.x, end.y);
}

/* Precomputed sin/cos for common circle segment counts.
 * 64 covers all LOD tiers including medium-distance asteroids. */
#define SINCOS_TABLE_MAX 64
static float sincos_table_sin[SINCOS_TABLE_MAX + 1][SINCOS_TABLE_MAX + 1];
static float sincos_table_cos[SINCOS_TABLE_MAX + 1][SINCOS_TABLE_MAX + 1];
static bool sincos_table_valid[SINCOS_TABLE_MAX + 1];

static void ensure_sincos_table(int segments) {
    if (segments > SINCOS_TABLE_MAX || sincos_table_valid[segments]) return;
    sincos_table_valid[segments] = true;
    float step = TWO_PI_F / (float)segments;
    for (int i = 0; i <= segments; i++) {
        float angle = (float)i * step;
        sincos_table_sin[segments][i] = sinf(angle);
        sincos_table_cos[segments][i] = cosf(angle);
    }
}

void draw_circle_filled(vec2 center, float radius, int segments, float r, float g0, float b, float a) {
    if (segments < 3) segments = 3;
    if (segments > 256) segments = 256;
    sgl_c4f(r, g0, b, a);
    sgl_begin_triangles();
    if (segments <= SINCOS_TABLE_MAX) {
        ensure_sincos_table(segments);
        float prev_cx = center.x + sincos_table_cos[segments][0] * radius;
        float prev_cy = center.y + sincos_table_sin[segments][0] * radius;
        for (int i = 1; i <= segments; i++) {
            float cx = center.x + sincos_table_cos[segments][i] * radius;
            float cy = center.y + sincos_table_sin[segments][i] * radius;
            sgl_v2f(center.x, center.y);
            sgl_v2f(prev_cx, prev_cy);
            sgl_v2f(cx, cy);
            prev_cx = cx;
            prev_cy = cy;
        }
    } else {
        float step = TWO_PI_F / (float)segments;
        float prev_cx = center.x + radius;
        float prev_cy = center.y;
        for (int i = 1; i <= segments; i++) {
            float angle = (float)i * step;
            float cx = center.x + cosf(angle) * radius;
            float cy = center.y + sinf(angle) * radius;
            sgl_v2f(center.x, center.y);
            sgl_v2f(prev_cx, prev_cy);
            sgl_v2f(cx, cy);
            prev_cx = cx;
            prev_cy = cy;
        }
    }
    sgl_end();
}

void draw_circle_outline(vec2 center, float radius, int segments, float r, float g0, float b, float a) {
    if (segments < 3) segments = 3;
    if (segments > 256) segments = 256;
    sgl_c4f(r, g0, b, a);
    sgl_begin_line_strip();
    if (segments <= SINCOS_TABLE_MAX) {
        ensure_sincos_table(segments);
        for (int i = 0; i <= segments; i++) {
            sgl_v2f(center.x + sincos_table_cos[segments][i] * radius, center.y + sincos_table_sin[segments][i] * radius);
        }
    } else {
        float step = TWO_PI_F / (float)segments;
        for (int i = 0; i <= segments; i++) {
            float angle = (float)i * step;
            sgl_v2f(center.x + cosf(angle) * radius, center.y + sinf(angle) * radius);
        }
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

void draw_texture_rect(uint32_t view_id, uint32_t sampler_id,
                       float x0, float y0, float x1, float y1,
                       float r, float g0, float b, float a) {
    if (view_id == 0 || sampler_id == 0) return;
    sgl_enable_texture();
    sgl_texture((sg_view){ view_id }, (sg_sampler){ sampler_id });
    sgl_begin_quads();
    sgl_c4f(r, g0, b, a);
    sgl_v2f_t2f(x0, y0, 0.0f, 0.0f);
    sgl_v2f_t2f(x1, y0, 1.0f, 0.0f);
    sgl_v2f_t2f(x1, y1, 1.0f, 1.0f);
    sgl_v2f_t2f(x0, y1, 0.0f, 1.0f);
    sgl_end();
    sgl_disable_texture();
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
        case ASTEROID_TIER_XXL: base_r = 0.25f; base_g = 0.28f; base_b = 0.45f; break;
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
