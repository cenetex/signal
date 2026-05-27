#ifndef CAMERA_MODEL_H
#define CAMERA_MODEL_H

static inline float camera_model_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float camera_narrow_focus(float width, float height) {
    if (width <= 0.0f || height <= 0.0f) return 0.0f;
    float aspect = width / height;
    float t = camera_model_clampf((0.82f - aspect) / (0.82f - 0.58f),
                                  0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float camera_deadzone_x_scale(float narrow_focus) {
    float t = camera_model_clampf(narrow_focus, 0.0f, 1.0f);
    return 0.45f + (0.23f - 0.45f) * t;
}

static inline float camera_deadzone_y_scale(float narrow_focus) {
    float t = camera_model_clampf(narrow_focus, 0.0f, 1.0f);
    return 0.40f + (0.22f - 0.40f) * t;
}

static inline float camera_narrow_center_strength(float narrow_focus) {
    return 0.72f * camera_model_clampf(narrow_focus, 0.0f, 1.0f);
}

#endif /* CAMERA_MODEL_H */
