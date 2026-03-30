#ifndef RENDER_H
#define RENDER_H

#include "types.h"

void draw_circle_filled(vec2 center, float radius, int segments, float r, float g0, float b, float a);
void draw_circle_outline(vec2 center, float radius, int segments, float r, float g0, float b, float a);
void draw_rect_centered(vec2 center, float half_w, float half_h, float r, float g0, float b, float a);
void draw_rect_outline(vec2 center, float half_w, float half_h, float r, float g0, float b, float a);
void draw_segment(vec2 start, vec2 end, float r, float g0, float b, float a);

void commodity_material_tint(commodity_t commodity, float* mr, float* mg, float* mb);
void asteroid_body_color(asteroid_tier_t tier, commodity_t commodity, float hp_ratio, float* r, float* g0, float* b);

#endif
