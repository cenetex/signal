/*
 * fixpoint.h — Deterministic transcendental math for cross-platform sim.
 *
 * sinf/cosf/atan2f/sqrtf produce different last-bit results across
 * platforms (macOS ARM vs Linux x86 vs WASM). This module provides
 * deterministic replacements using a fixed-point sin LUT internally.
 *
 * The sim continues to use float — only the transcendental calls change.
 * Add/sub/mul/div on float are deterministic under strict IEEE mode
 * (-ffp-contract=off -fno-fast-math).
 */
#ifndef FIXPOINT_H
#define FIXPOINT_H

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

float fixp_sqrtf(float x);
float fixp_sinf(float x);
float fixp_cosf(float x);
float fixp_atan2f(float y, float x);
float fixp_expf(float x);
float fixp_powf(float base, float exp);
float fixp_tanf(float x);
float fixp_asinf(float x);

#ifdef __cplusplus
}
#endif

#endif /* FIXPOINT_H */

