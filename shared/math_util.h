#ifndef MATH_UTIL_H
#define MATH_UTIL_H

#include <math.h>
#include <stdint.h>

static const float PI_F = 3.14159265359f;
static const float TWO_PI_F = 6.28318530718f;
static const float FLOAT_EPSILON = 0.01f;

typedef struct {
    float x;
    float y;
} vec2;

static inline float clampf(float value, float min_value, float max_value) {
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static inline vec2 v2(float x, float y) {
    vec2 result = { x, y };
    return result;
}

static inline vec2 v2_add(vec2 a, vec2 b) {
    return v2(a.x + b.x, a.y + b.y);
}

static inline vec2 v2_sub(vec2 a, vec2 b) {
    return v2(a.x - b.x, a.y - b.y);
}

static inline vec2 v2_scale(vec2 value, float scale) {
    return v2(value.x * scale, value.y * scale);
}

static inline vec2 v2_perp(vec2 value) {
    return v2(-value.y, value.x);
}

static inline float v2_dist_sq(vec2 a, vec2 b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return (dx * dx) + (dy * dy);
}

static inline float v2_dot(vec2 a, vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

static inline float v2_cross(vec2 a, vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

static inline float v2_len_sq(vec2 value) {
    return v2_dot(value, value);
}

static inline float v2_len(vec2 value) {
    return sqrtf(v2_len_sq(value));
}

static inline vec2 v2_norm(vec2 value) {
    float len = v2_len(value);
    if (len > 0.00001f) {
        return v2_scale(value, 1.0f / len);
    }
    return v2(1.0f, 0.0f);
}

static inline vec2 v2_from_angle(float angle) {
    return v2(cosf(angle), sinf(angle));
}

static inline float wrap_angle(float angle) {
    while (angle > PI_F) angle -= TWO_PI_F;
    while (angle < -PI_F) angle += TWO_PI_F;
    return angle;
}

static inline float lerp_angle(float a, float b, float t) {
    float diff = wrap_angle(b - a);
    return wrap_angle(a + diff * t);
}

/* Closest point on line segment AB to point P. */
static inline vec2 v2_closest_on_segment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = v2_sub(b, a);
    float ab_sq = v2_len_sq(ab);
    if (ab_sq < 0.0001f) return a;
    float t = v2_dot(v2_sub(p, a), ab) / ab_sq;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return v2_add(a, v2_scale(ab, t));
}

#endif
