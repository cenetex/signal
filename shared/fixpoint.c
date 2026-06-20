/*
 * fixpoint.c — Deterministic transcendental math implementations.
 *
 * Uses q32.32 fixed-point internally (64-bit int with 32 frac bits)
 * and converts float ↔ fixp at the boundary.
 */
#include "fixpoint.h"
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>

typedef int64_t fixp_t;

#define FIXP_ONE       ((fixp_t)1 << 32)
#define FIXP_HALF      (FIXP_ONE >> 1)
#define FIXP_PI        ((fixp_t)0x3243F6A88LL)
#define FIXP_TWO_PI    ((fixp_t)0x6487ED511LL)
#define FIXP_HALF_PI   ((fixp_t)0x1921FB544LL)
#define FIXP_INV_TWO_PI ((fixp_t)0x28BE60DCLL)
#define FIXP_PI_OVER_4 ((fixp_t)0x00000000c90fdaa2LL)
#define FIXP_LN2       ((fixp_t)0x00000000b17217f8LL)
#define FIXP_SQRT2     ((fixp_t)0x000000016a09e668LL)
#define FIXP_INV_SQRT2 ((fixp_t)0x00000000b504f334LL)

static fixp_t fixp_from_float(float x) {
    if (!isfinite(x)) return 0;
    double scaled = (double)x * (double)FIXP_ONE;
    if (scaled > (double)INT64_MAX) return INT64_MAX;
    if (scaled < (double)INT64_MIN) return INT64_MIN;
    return (fixp_t)scaled;
}

static float fixp_to_float(fixp_t x) {
    return (float)((double)x / (double)FIXP_ONE);
}

#if defined(_MSC_VER) && !defined(__clang__)
static fixp_t fixp_from_wide_double(long double x) {
    if (!isfinite((double)x)) return 0;
    if (x > (long double)INT64_MAX) return INT64_MAX;
    if (x < (long double)INT64_MIN) return INT64_MIN;
    return (fixp_t)x;
}

/* MSVC's C frontend does not expose __int128. Native Windows release builds
 * use this clamped fallback; GCC/Clang/WASM take the exact integer path below. */
static fixp_t fixp_mul(fixp_t a, fixp_t b) {
    return fixp_from_wide_double(((long double)a * (long double)b) / (long double)FIXP_ONE);
}

static fixp_t fixp_div(fixp_t a, fixp_t b) {
    if (b == 0) return (a >= 0) ? INT64_MAX : INT64_MIN;
    return fixp_from_wide_double(((long double)a * (long double)FIXP_ONE) / (long double)b);
}
#else
static fixp_t fixp_mul(fixp_t a, fixp_t b) {
    return (fixp_t)(((__int128)a * (__int128)b) / (__int128)FIXP_ONE);
}

static fixp_t fixp_div(fixp_t a, fixp_t b) {
    if (b == 0) return (a >= 0) ? INT64_MAX : INT64_MIN;
    return (fixp_t)(((__int128)a * (__int128)FIXP_ONE) / (__int128)b);
}
#endif

/* ---- sin LUT ---- */

#define SIN_LUT_SIZE 1024

static const fixp_t sin_lut[SIN_LUT_SIZE] = {
#include "fixpoint_sin_table.inc"
};

static fixp_t fixp_sin(fixp_t x) {
    while (x >= FIXP_TWO_PI) x -= FIXP_TWO_PI;
    while (x < 0)           x += FIXP_TWO_PI;
    uint64_t scaled = (uint64_t)fixp_mul(x, (fixp_t)(SIN_LUT_SIZE * FIXP_INV_TWO_PI));
    uint32_t idx = (uint32_t)((scaled >> 32) % SIN_LUT_SIZE);
    fixp_t frac = (fixp_t)(scaled & (FIXP_ONE - 1));
    fixp_t a = sin_lut[idx];
    fixp_t b = sin_lut[(idx + 1) % SIN_LUT_SIZE];
    return a + fixp_mul(b - a, frac);
}

static fixp_t fixp_cos(fixp_t x) {
    return fixp_sin(x + FIXP_HALF_PI);
}

/* ---- sqrt: Newton's method ---- */

static fixp_t fixp_sqrt_fixp(fixp_t x) {
    if (x <= 0) return 0;
    fixp_t guess;
    if (x >= FIXP_ONE) {
        uint64_t xi = (uint64_t)(x >> 32);
        uint64_t r = 0;
        uint64_t bit = (uint64_t)1 << ((63 - __builtin_clzll(xi)) & ~(uint64_t)1);
        while (bit) {
            uint64_t t = r + bit;
            r >>= 1;
            if (xi >= t) { xi -= t; r += bit; }
            bit >>= 2;
        }
        guess = (fixp_t)r << 32;
    } else {
        guess = x;
    }
    for (int i = 0; i < 8; i++) {
        if (guess == 0) break;
        fixp_t quot = fixp_div(x, guess);
        fixp_t next = (guess + quot) >> 1;
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

/* ---- atan: minimax polynomial with range reduction ---- */

#define ATAN_C0 ((fixp_t)0x00000000fff737daLL)
#define ATAN_C1 ((fixp_t)0xffffffffab717df2LL)
#define ATAN_C2 ((fixp_t)0x000000002e1db877LL)
#define ATAN_C3 ((fixp_t)0xffffffffea34b946LL)
#define ATAN_C4 ((fixp_t)0x00000000055572f9LL)

/* atan(z) for z >= 0 */
static fixp_t fixp_atan_fixp(fixp_t z) {
    /* Minimax polynomial accurate to ~1e-5 rad on [0, 0.5].
     * For z > 0.5: atan(z) = pi/4 + atan((z-1)/(1+z)) */
    fixp_t base = 0;
    bool reduced = false;
    if (z > FIXP_HALF) {
        fixp_t t = fixp_div(z - FIXP_ONE, z + FIXP_ONE);
        z = (t < 0) ? -t : t;
        base = FIXP_PI_OVER_4;
        reduced = true;
    }
    fixp_t z2 = fixp_mul(z, z);
    fixp_t poly = ATAN_C4;
    poly = fixp_mul(poly, z2);
    poly = poly + ATAN_C3;
    poly = fixp_mul(poly, z2);
    poly = poly + ATAN_C2;
    poly = fixp_mul(poly, z2);
    poly = poly + ATAN_C1;
    poly = fixp_mul(poly, z2);
    poly = poly + ATAN_C0;
    fixp_t result = base + fixp_mul(z, poly);
    if (reduced) {
        result = FIXP_HALF_PI - result;
    }
    return result;
}

static fixp_t fixp_atan2_fixp(fixp_t y, fixp_t x) {
    if (x == 0 && y == 0) return 0;
    fixp_t ay = (y < 0) ? -y : y;
    fixp_t ax = (x < 0) ? -x : x;
    bool swap = (ay > ax);
    fixp_t z = swap ? fixp_div(ax, ay) : fixp_div(ay, ax);
    fixp_t atanz = fixp_atan_fixp(z);
    fixp_t result = swap ? (FIXP_HALF_PI - atanz) : atanz;
    if (x < 0) result = FIXP_PI - result;
    if (y < 0) result = -result;
    return result;
}

/* ---- exp/log: range-reduced Taylor series ---- */

static fixp_t fixp_exp_fixp(fixp_t x) {
    fixp_t scaled = fixp_div(x, FIXP_LN2);
    int64_t n;
    if (scaled >= 0) {
        n = (scaled + FIXP_HALF) >> 32;
    } else {
        n = -(((-scaled) + FIXP_HALF) >> 32);
    }

    fixp_t r = x - (fixp_t)n * FIXP_LN2; /* |r| <= ln(2)/2 */
    fixp_t term = FIXP_ONE;
    fixp_t sum = FIXP_ONE;
    for (int i = 1; i <= 12; i++) {
        term = fixp_mul(term, r);
        term /= i;
        sum += term;
    }

    if (n >= 0) {
        if (n >= 31) return INT64_MAX;
        if (sum > (INT64_MAX >> n)) return INT64_MAX;
        return sum << n;
    }
    int64_t shift = -n;
    if (shift >= 63) return 0;
    return sum >> shift;
}

static fixp_t fixp_ln_fixp(fixp_t x) {
    if (x <= 0) return INT64_MIN;

    int k = 0;
    while (x >= FIXP_SQRT2) {
        x >>= 1;
        k++;
    }
    while (x < FIXP_INV_SQRT2) {
        x <<= 1;
        k--;
    }

    fixp_t z = fixp_div(x - FIXP_ONE, x + FIXP_ONE);
    fixp_t z2 = fixp_mul(z, z);
    fixp_t zpow = z;
    fixp_t sum = z;
    for (int denom = 3; denom <= 23; denom += 2) {
        zpow = fixp_mul(zpow, z2);
        sum += zpow / denom;
    }

    return (sum + sum) + (fixp_t)k * FIXP_LN2;
}

/* ---- Public API ---- */

float fixp_sqrtf(float x) {
    if (!isfinite(x) || x < 0.0f) return 0.0f;
    return fixp_to_float(fixp_sqrt_fixp(fixp_from_float(x)));
}

float fixp_sinf(float x) {
    if (!isfinite(x)) return 0.0f;
    return fixp_to_float(fixp_sin(fixp_from_float(x)));
}

float fixp_cosf(float x) {
    if (!isfinite(x)) return 1.0f;
    return fixp_to_float(fixp_cos(fixp_from_float(x)));
}

float fixp_atan2f(float y, float x) {
    if (!isfinite(y) || !isfinite(x)) return 0.0f;
    return fixp_to_float(fixp_atan2_fixp(fixp_from_float(y), fixp_from_float(x)));
}

float fixp_expf(float x) {
    if (!isfinite(x)) return 0.0f;
    if (x > 20.0f) x = 20.0f;
    if (x < -20.0f) return 0.0f;
    return fixp_to_float(fixp_exp_fixp(fixp_from_float(x)));
}

float fixp_powf(float base, float exp) {
    if (!isfinite(base) || !isfinite(exp) || base <= 0.0f) return 0.0f;
    fixp_t ln_base = fixp_ln_fixp(fixp_from_float(base));
    fixp_t exponent = fixp_mul(fixp_from_float(exp), ln_base);
    return fixp_to_float(fixp_exp_fixp(exponent));
}

float fixp_tanf(float x) {
    if (!isfinite(x)) return 0.0f;
    fixp_t fx = fixp_from_float(x);
    fixp_t s = fixp_sin(fx);
    fixp_t c = fixp_cos(fx);
    if (c == 0) return (s >= 0) ? 1e10f : -1e10f;
    return fixp_to_float(fixp_div(s, c));
}

float fixp_asinf(float x) {
    if (!isfinite(x)) return 0.0f;
    if (x < -1.0f) x = -1.0f;
    if (x > 1.0f) x = 1.0f;
    fixp_t fx = fixp_from_float(x);
    fixp_t x2 = fixp_mul(fx, fx);
    fixp_t x3 = fixp_mul(x2, fx);
    fixp_t x5 = fixp_mul(x3, x2);
    return fixp_to_float(fx + fixp_div(x3, (fixp_t)6 << 32)
                            + fixp_div(fixp_mul((fixp_t)3 << 32, x5), (fixp_t)40 << 32));
}
