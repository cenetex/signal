/* cargo_pod_geometry.h — one-size hex carrier mass, hardpoints, and hull. */
#ifndef CARGO_POD_GEOMETRY_H
#define CARGO_POD_GEOMETRY_H

#include <math.h>

enum { CARGO_POD_HARDPOINT_COUNT = 6 };

#define CARGO_POD_SHELL_RADIUS_SCALE 0.90f
#define CARGO_POD_PAYLOAD_UNIT_MASS  0.25f

/* A connected tow beam acts as an alignment joint: the selected complete
 * edge faces its source and the pod carries no angular velocity while held. */
static inline void cargo_pod_align_tow_hardpoint(cargo_pod_t *pod,
                                                 int hardpoint,
                                                 vec2 source) {
    if (!pod) return;
    int aligned_hardpoint = cell_orientation_normalize(hardpoint);
    vec2 to_source = v2_sub(source, pod->pos);
    if (v2_len_sq(to_source) > 0.0001f) {
        pod->rotation = wrap_angle(
            fixp_atan2f(to_source.y, to_source.x) -
            (float)aligned_hardpoint * 1.0471975511965976f);
    }
    pod->spin = 0.0f;
}

static inline float cargo_pod_shell_mass(const cargo_pod_t *pod) {
    if (!pod) return 0.0f;
    /* A deployed carrier is one complete six-strut hex rim.  The folded
     * source frame changes provenance, not physical rim mass. */
    return (float)CELL_HEX_STRUT_COST;
}

static inline float cargo_pod_payload_mass(const cargo_pod_t *pod) {
    return pod ? (float)pod->quantity * CARGO_POD_PAYLOAD_UNIT_MASS : 0.0f;
}

static inline float cargo_pod_total_mass(const cargo_pod_t *pod) {
    return cargo_pod_shell_mass(pod) + cargo_pod_payload_mass(pod);
}

static inline float cargo_pod_inverse_mass(const cargo_pod_t *pod) {
    float mass = cargo_pod_total_mass(pod);
    return mass > 0.0f ? 1.0f / mass : 0.0f;
}

static inline float cargo_pod_inverse_inertia(const cargo_pod_t *pod) {
    if (!pod) return 0.0f;
    float radius = pod->radius * CARGO_POD_SHELL_RADIUS_SCALE;
    float inertia = (5.0f / 12.0f) * cargo_pod_total_mass(pod) *
                    radius * radius;
    return inertia > 0.0001f ? 1.0f / inertia : 0.0f;
}

static inline const char *cargo_pod_hardpoint_name(int hardpoint) {
    static const char *names[CARGO_POD_HARDPOINT_COUNT] = {
        "east", "south-east", "south-west",
        "west", "north-west", "north-east",
    };
    int index = cell_orientation_normalize(hardpoint);
    return names[index];
}

/* Hardpoints sit at complete-edge midpoints, not the center of mass. */
static inline vec2 cargo_pod_hardpoint_offset(const cargo_pod_t *pod,
                                              int hardpoint) {
    if (!pod) return v2(0.0f, 0.0f);
    float angle = pod->rotation +
                  (float)cell_orientation_normalize(hardpoint) *
                  1.0471975511965976f;
    float apothem = pod->radius * CARGO_POD_SHELL_RADIUS_SCALE *
                    0.8660254037844386f;
    return v2_scale(v2_from_angle(angle), apothem);
}

static inline vec2 cargo_pod_hardpoint_world(const cargo_pod_t *pod,
                                             int hardpoint) {
    return pod ? v2_add(pod->pos,
                        cargo_pod_hardpoint_offset(pod, hardpoint))
               : v2(0.0f, 0.0f);
}

static inline int cargo_pod_select_hardpoint(const cargo_pod_t *pod,
                                             vec2 source) {
    if (!pod) return 0;
    int best = 0;
    float best_distance = INFINITY;
    for (int i = 0; i < CARGO_POD_HARDPOINT_COUNT; i++) {
        float distance = v2_dist_sq(cargo_pod_hardpoint_world(pod, i), source);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

static inline int cargo_pod_tow_hardpoint(const cargo_pod_t *pod) {
    return pod && pod->tow_hardpoint_tag >= 1 &&
           pod->tow_hardpoint_tag <= CARGO_POD_HARDPOINT_COUNT
        ? (int)pod->tow_hardpoint_tag - 1 : -1;
}

static inline void cargo_pod_set_tow_hardpoint(cargo_pod_t *pod,
                                                int hardpoint) {
    if (!pod) return;
    pod->tow_hardpoint_tag = (uint8_t)(
        cell_orientation_normalize(hardpoint) + 1);
}

/* Convex point test used by deterministic polygon collision checks. */
static inline bool cargo_pod_contains_point(const cargo_pod_t *pod,
                                            vec2 point) {
    if (!pod) return false;
    vec2 local = v2_sub(point, pod->pos);
    vec2 basis = v2_from_angle(-pod->rotation);
    float c = basis.x, s = basis.y;
    vec2 p = v2(local.x * c - local.y * s,
                local.x * s + local.y * c);
    float radius = pod->radius * CARGO_POD_SHELL_RADIUS_SCALE;
    /* Pointy-top regular hex: |y| <= R and |x| + sqrt(3)|y| <= sqrt(3)R. */
    float ax = fabsf(p.x), ay = fabsf(p.y);
    return ax <= 0.8660254037844386f * radius + 0.0001f &&
           ay <= radius + 0.0001f &&
           ax + 1.7320508075688772f * ay <=
               1.7320508075688772f * radius + 0.0001f;
}

/* Convex support distance along a world-space direction.  Circle broad-phase
 * still uses pod->radius; narrow-phase resolves against this rotated hex. */
static inline float cargo_pod_support_radius(const cargo_pod_t *pod,
                                             vec2 direction) {
    if (!pod) return 0.0f;
    float length = v2_len(direction);
    if (length <= 0.0001f)
        return pod->radius * CARGO_POD_SHELL_RADIUS_SCALE;
    vec2 n = v2_scale(direction, 1.0f / length);
    float radius = pod->radius * CARGO_POD_SHELL_RADIUS_SCALE;
    float support = 0.0f;
    for (int i = 0; i < 6; i++) {
        float angle = pod->rotation - 1.5707963267948966f +
                      (float)i * 1.0471975511965976f;
        float projection = radius * v2_dot(v2_from_angle(angle), n);
        if (projection > support) support = projection;
    }
    return support;
}

#endif /* CARGO_POD_GEOMETRY_H */
