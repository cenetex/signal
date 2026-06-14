/*
 * ui_clarity.h -- Presentation grammar for uncertain/fading knowledge.
 *
 * This is deliberately UI-only: callers feed it confidence/salience/hops
 * from whatever evidence they already have, and it returns colors + short
 * labels that make certainty visible without printing raw scores.
 */
#ifndef SIGNAL_UI_CLARITY_H
#define SIGNAL_UI_CLARITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

typedef struct {
    float clarity;       /* 0..1 */
    uint8_t fg[3];
    uint8_t dim[3];
    char meter[8];
    const char *word;
} ui_clarity_t;

static inline float ui_clarity_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint8_t ui_clarity_mix_u8(uint8_t a, uint8_t b, float t) {
    t = ui_clarity_clampf(t, 0.0f, 1.0f);
    return (uint8_t)lrintf((float)a + ((float)b - (float)a) * t);
}

static inline void ui_clarity_mix_rgb(const uint8_t hi[3],
                                      const uint8_t lo[3],
                                      float clarity,
                                      uint8_t out[3]) {
    float t = 1.0f - ui_clarity_clampf(clarity, 0.0f, 1.0f);
    out[0] = ui_clarity_mix_u8(hi[0], lo[0], t);
    out[1] = ui_clarity_mix_u8(hi[1], lo[1], t);
    out[2] = ui_clarity_mix_u8(hi[2], lo[2], t);
}

static inline ui_clarity_t ui_clarity_from_evidence(uint8_t confidence,
                                                    uint8_t salience,
                                                    uint8_t hops,
                                                    const uint8_t hi[3],
                                                    const uint8_t lo[3]) {
    float c = (float)confidence / 255.0f;
    float s = (float)salience / 255.0f;
    float hop_penalty = 1.0f / (1.0f + 0.28f * (float)hops);
    float clarity = sqrtf(ui_clarity_clampf(c * s, 0.0f, 1.0f)) * hop_penalty;
    clarity = ui_clarity_clampf(clarity, 0.12f, 1.0f);

    ui_clarity_t out = {0};
    out.clarity = clarity;
    ui_clarity_mix_rgb(hi, lo, clarity, out.fg);
    ui_clarity_mix_rgb(lo, hi, clarity * 0.35f, out.dim);

    if (clarity >= 0.78f) {
        snprintf(out.meter, sizeof(out.meter), "||||");
        out.word = "clear";
    } else if (clarity >= 0.55f) {
        snprintf(out.meter, sizeof(out.meter), "|||?");
        out.word = "heard";
    } else if (clarity >= 0.34f) {
        snprintf(out.meter, sizeof(out.meter), "||??");
        out.word = "faint";
    } else {
        snprintf(out.meter, sizeof(out.meter), "|???");
        out.word = "rumor";
    }
    return out;
}

static inline bool ui_clarity_should_mask_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static inline void ui_clarity_degrade_text(const char *src,
                                           float clarity,
                                           uint32_t seed,
                                           char *out,
                                           size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (!src) return;
    if (clarity >= 0.82f) {
        snprintf(out, cap, "%s", src);
        return;
    }

    int gap = clarity >= 0.62f ? 9 : (clarity >= 0.42f ? 5 : 3);
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        uint32_t h = seed ^ (uint32_t)(i * 1103515245u + 12345u);
        bool mask = ui_clarity_should_mask_char(c) &&
                    ((int)(h % (uint32_t)gap) == 0);
        out[i] = mask ? '?' : c;
    }
    out[n] = '\0';
}

#endif
