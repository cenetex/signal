#include <math.h>
#include <stdbool.h>
#include "render.h"
#include "palette.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"

/* Batched line drawing — call between begin/end_line_batch */
static bool _line_batch_active = false;
static float g_render_saturation = 1.0f;
static render_saturation_sample_fn g_render_saturation_sample = 0;
static void *g_render_saturation_user = 0;

static float render_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void render_desaturate(float saturation, float *r, float *g0, float *b) {
    saturation = render_clampf(saturation, 0.0f, 1.0f);
    float gray = *r * 0.2126f + *g0 * 0.7152f + *b * 0.0722f;
    *r = gray + (*r - gray) * saturation;
    *g0 = gray + (*g0 - gray) * saturation;
    *b = gray + (*b - gray) * saturation;
}

static float render_saturation_at(vec2 pos) {
    float saturation = g_render_saturation;
    if (g_render_saturation_sample) {
        float sampled = g_render_saturation_sample(pos, g_render_saturation_user);
        if (isfinite(sampled) && sampled > saturation) saturation = sampled;
    }
    return render_clampf(saturation, 0.0f, 1.0f);
}

void render_set_saturation(float saturation) {
    g_render_saturation = render_clampf(saturation, 0.0f, 1.0f);
}

void render_set_saturation_sampler(render_saturation_sample_fn fn, void *user) {
    g_render_saturation_sample = fn;
    g_render_saturation_user = user;
}

void render_color4f(float r, float g0, float b, float a) {
    render_desaturate(g_render_saturation, &r, &g0, &b);
    sgl_c4f(r, g0, b, a);
}

void render_color4f_at(vec2 pos, float r, float g0, float b, float a) {
    render_desaturate(render_saturation_at(pos), &r, &g0, &b);
    sgl_c4f(r, g0, b, a);
}

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
    vec2 mid = v2_scale(v2_add(start, end), 0.5f);
    render_color4f_at(mid, r, g0, b, a);
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
    render_color4f_at(center, r, g0, b, a);
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
    render_color4f_at(center, r, g0, b, a);
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
    render_color4f_at(center, r, g0, b, a);
    sgl_begin_quads();
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y + half_h);
    sgl_end();
}

void draw_rect_outline(vec2 center, float half_w, float half_h, float r, float g0, float b, float a) {
    render_color4f_at(center, r, g0, b, a);
    sgl_begin_line_strip();
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y - half_h);
    sgl_v2f(center.x + half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y + half_h);
    sgl_v2f(center.x - half_w, center.y - half_h);
    sgl_end();
}

void draw_segment(vec2 start, vec2 end, float r, float g0, float b, float a) {
    vec2 mid = v2_scale(v2_add(start, end), 0.5f);
    render_color4f_at(mid, r, g0, b, a);
    sgl_begin_lines();
    sgl_v2f(start.x, start.y);
    sgl_v2f(end.x, end.y);
    sgl_end();
}

void draw_texture_rect(uint32_t view_id, uint32_t sampler_id,
                       float x0, float y0, float x1, float y1,
                       float r, float g0, float b, float a) {
    if (view_id == 0 || sampler_id == 0) return;
    render_desaturate(g_render_saturation, &r, &g0, &b);
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
        case COMMODITY_FERRITE_ORE: PAL_UNPACK3(PAL_COMMODITY_FERRITE_ORE, *mr, *mg, *mb); break;
        case COMMODITY_CUPRITE_ORE: PAL_UNPACK3(PAL_COMMODITY_CUPRITE_ORE, *mr, *mg, *mb); break;
        case COMMODITY_CRYSTAL_ORE: PAL_UNPACK3(PAL_COMMODITY_CRYSTAL_ORE, *mr, *mg, *mb); break;
        default: *mr = 0.24f; *mg = 0.25f; *mb = 0.27f; break;
    }
}

void asteroid_body_color(asteroid_tier_t tier, commodity_t commodity, float hp_ratio, float* r, float* g0, float* b) {
    float base_r = 0.22f, base_g = 0.23f, base_b = 0.25f;
    switch (tier) {
        case ASTEROID_TIER_XXL: base_r = 0.17f; base_g = 0.18f; base_b = 0.22f; break;
        case ASTEROID_TIER_XL:  base_r = 0.19f; base_g = 0.20f; base_b = 0.23f; break;
        case ASTEROID_TIER_L:   base_r = 0.21f; base_g = 0.22f; base_b = 0.24f; break;
        case ASTEROID_TIER_M:   base_r = 0.20f; base_g = 0.23f; base_b = 0.24f; break;
        case ASTEROID_TIER_S:   base_r = 0.21f; base_g = 0.25f; base_b = 0.24f; break;
        default: break;
    }
    float mat_r, mat_g, mat_b;
    commodity_material_tint(commodity, &mat_r, &mat_g, &mat_b);
    base_r = lerpf(base_r, mat_r, 0.30f);
    base_g = lerpf(base_g, mat_g, 0.30f);
    base_b = lerpf(base_b, mat_b, 0.30f);
    *r = lerpf(base_r * 0.70f, base_r * 1.08f, hp_ratio);
    *g0 = lerpf(base_g * 0.70f, base_g * 1.08f, hp_ratio);
    *b = lerpf(base_b * 0.70f, base_b * 1.08f, hp_ratio);
}
